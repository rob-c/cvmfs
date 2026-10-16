/**
 * This file is part of the CernVM File System.
 */

#ifndef CVMFS_CHUNK_READAHEAD_H_
#define CVMFS_CHUNK_READAHEAD_H_

#include <pthread.h>

#include <algorithm>
#include <deque>
#include <set>
#include <vector>

#include "cache.h"
#include "clientctx.h"
#include "interrupt.h"
#include "crypto/hash.h"
#include "fetch.h"
#include "file_chunk.h"
#include "util/mutex.h"
#include "util/single_copy.h"

namespace cvmfs {

/**
 * Best-effort read-ahead of the next chunks of a chunked file.  The read path
 * fetches chunks strictly one at a time; on a high-latency or lossy link a
 * single TCP stream is the throughput bottleneck, so the next chunks are
 * fetched concurrently while the current one is consumed.  The fetcher
 * deduplicates concurrent requests for the same object, so the reader simply
 * waits on (or finds in the cache) whatever was prefetched.  Everything here
 * is best effort: a full queue drops requests, errors are ignored.
 */
class ChunkReadahead : SingleCopy {
 public:
  ChunkReadahead(unsigned depth, unsigned num_threads)
      : depth_(depth), num_threads_(num_threads), terminating_(false),
        num_waiting_(0), stop_cue_(this) {
    int retval = pthread_mutex_init(&lock_, NULL);
    assert(retval == 0);
    retval = pthread_cond_init(&cond_, NULL);
    assert(retval == 0);
  }
  ~ChunkReadahead() {
    Stop();
    pthread_cond_destroy(&cond_);
    pthread_mutex_destroy(&lock_);
  }

  void Spawn() {
    for (unsigned i = 0; i < num_threads_; ++i) {
      pthread_t thread;
      const int retval = pthread_create(&thread, NULL, MainReadahead, this);
      assert(retval == 0);
      threads_.push_back(thread);
    }
  }

  /**
   * Stops the workers.  Ones inside a fetch see the cancellation at their
   * next retry or backoff (see StopCue) rather than being joined through the
   * whole retry budget; an attempt already on the wire still runs to its
   * timeout.
   */
  void Stop() {
    pthread_mutex_lock(&lock_);
    terminating_ = true;
    pthread_cond_broadcast(&cond_);
    pthread_mutex_unlock(&lock_);
    for (unsigned i = 0; i < threads_.size(); ++i)
      pthread_join(threads_[i], NULL);
    threads_.clear();
  }

  /**
   * Workers currently parked waiting for work.  Exposed so a test can make
   * sure every worker is asleep before it schedules a batch; otherwise a
   * worker that has not yet reached the wait picks up an item without ever
   * needing a wake-up, and a broken wake-up goes unnoticed.
   */
  unsigned NumWaiting() {
    const MutexLockGuard m(lock_);
    return num_waiting_;
  }

  /**
   * Schedules chunks [first, first + depth) of a chunked file.  Never blocks.
   */
  void Schedule(const FileChunkReflist &chunks, unsigned first,
                Fetcher *fetcher, bool volatile_flag) {
    const MutexLockGuard m(lock_);
    unsigned num_queued = 0;
    const unsigned last = std::min(first + depth_,
                                   static_cast<unsigned>(chunks.list->size()));
    for (unsigned i = first; i < last; ++i) {
      if (queue_.size() >= kMaxQueue)
        break;
      const FileChunk *chunk = chunks.list->AtPtr(i);
      if (pending_.count(chunk->content_hash()) > 0)
        continue;
      Item item;
      item.fetcher = fetcher;
      item.hash = chunk->content_hash();
      // The worker has no thread-local client context of its own.  Without
      // the reader's identity the fetch carries no authz credential, so on a
      // membership-protected repository the prefetch is refused and, through
      // the fetcher's request dedup, so is the reader waiting on the same
      // chunk.  The fuse request's interrupt cue is deliberately not carried:
      // it dies with the fuse operation.
      item.has_ctx = false;
      item.uid = -1;
      item.gid = -1;
      item.pid = -1;
      ClientCtx *ctx = ClientCtx::GetInstance();
      if (ctx && ctx->IsSet()) {
        InterruptCue *unused = NULL;
        ctx->Get(&item.uid, &item.gid, &item.pid, &unused);
        item.has_ctx = true;
      }
      item.label.path = chunks.path.ToString();
      item.label.size = chunk->size();
      item.label.zip_algorithm = chunks.compression_alg;
      item.label.flags |= CacheManager::kLabelChunked;
      if (volatile_flag || chunks.volatile_data)
        item.label.flags |= CacheManager::kLabelVolatile;
      queue_.push_back(item);
      pending_.insert(item.hash);
      ++num_queued;
    }
    // One signal wakes one worker.  It then drains the whole batch by itself,
    // because on the next pass round its loop the queue is still not empty and
    // it never waits again -- so the chunks are fetched one after another and
    // the extra threads stay asleep, which is precisely the serialisation this
    // class exists to avoid.  Wake everyone and let them take an item each.
    if (num_queued > 0)
      pthread_cond_broadcast(&cond_);
  }

 private:
  static const unsigned kMaxQueue = 64;
  struct Item {
    Fetcher *fetcher;
    shash::Any hash;
    CacheManager::Label label;
    bool has_ctx;
    uid_t uid;
    gid_t gid;
    pid_t pid;
  };

  static void *MainReadahead(void *data) {
    ChunkReadahead *self = static_cast<ChunkReadahead *>(data);
    while (true) {
      pthread_mutex_lock(&self->lock_);
      while (self->queue_.empty() && !self->terminating_) {
        self->num_waiting_++;
        pthread_cond_wait(&self->cond_, &self->lock_);
        self->num_waiting_--;
      }
      if (self->terminating_) {
        pthread_mutex_unlock(&self->lock_);
        break;
      }
      const Item item = self->queue_.front();
      self->queue_.pop_front();
      pthread_mutex_unlock(&self->lock_);

      // Always under a context: it carries the reader's identity when one
      // was captured, and in every case the stop cue, so a worker mid-fetch
      // can be cancelled by Stop().
      int fd;
      {
        ClientCtxGuard guard(item.uid, item.gid, item.pid, &self->stop_cue_);
        fd = item.fetcher->Fetch(
            CacheManager::LabeledObject(item.hash, item.label));
      }
      // Close on the cache manager the descriptor came from rather than on the
      // mount point's global one.  They are the same today only because
      // read-ahead is skipped for external data; reaching for the global would
      // quietly close a descriptor on the wrong manager the moment that
      // changed.
      if (fd >= 0)
        item.fetcher->cache_mgr()->Close(fd);

      pthread_mutex_lock(&self->lock_);
      self->pending_.erase(item.hash);
      pthread_mutex_unlock(&self->lock_);
    }
    return NULL;
  }

  unsigned depth_;
  unsigned num_threads_;
  bool terminating_;
  unsigned num_waiting_;

  /**
   * Handed to every worker fetch as its interrupt cue: cancelled the moment
   * Stop() is called.  Reads terminating_ without the lock; a torn read of a
   * bool is not possible and a late observation only costs one more retry.
   */
  class StopCue : public InterruptCue {
   public:
    explicit StopCue(ChunkReadahead *owner) : owner_(owner) { }
    virtual bool IsCanceled() { return owner_->terminating_; }
   private:
    ChunkReadahead *owner_;
  };
  StopCue stop_cue_;
  pthread_mutex_t lock_;
  pthread_cond_t cond_;
  std::deque<Item> queue_;
  std::set<shash::Any> pending_;
  std::vector<pthread_t> threads_;
};

}  // namespace cvmfs

#endif  // CVMFS_CHUNK_READAHEAD_H_
