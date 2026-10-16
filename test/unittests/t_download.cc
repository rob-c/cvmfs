/**
 * This file is part of the CernVM File System.
 */

#include <unistd.h>

#include <cassert>
#include <cstdio>

#include "c_file_sandbox.h"
#include "c_http_server.h"
#include "compression/compression.h"
#include "crypto/hash.h"
#include "gtest/gtest.h"
#include "interrupt.h"
#include "network/download.h"
#include "network/sink.h"
#include "network/sink_mem.h"
#include "network/sink_file.h"
#include "statistics.h"
#include "util/file_guard.h"
#include "util/platform.h"
#include "util/posix.h"
#include "util/prng.h"

using namespace std;  // NOLINT

namespace {

class TestInterruptCue : public InterruptCue {
 public:
  virtual bool IsCanceled() { return true; }
};

}  // anonymous namespace

namespace download {

class T_Download : public FileSandbox {
 public:
  T_Download()
      : FileSandbox(string(tmp_path) + "/server_dir")
      , download_mgr(8, perf::StatisticsTemplate("test", &statistics)) { }

 protected:
  virtual void SetUp() { CreateSandbox(); }

  virtual void TearDown() { RemoveSandbox(); }

  FILE *CreateTemporaryFile(std::string *path) const {
    return CreateTempFile(GetCurrentWorkingDirectory() + "/cvmfs_ut_download",
                          0600, "w+", path);
  }

  static const char tmp_path[];

  perf::Statistics statistics;
  DownloadManager download_mgr;
};

const char T_Download::tmp_path[] = "./cvmfs_ut_download";

class TestSink : public cvmfs::Sink {
 public:
  TestSink() : Sink(true) {
    FILE *f = CreateTempFile("./cvmfs_ut_download", 0600, "w+", &path);
    assert(f);
    fd = dup(fileno(f));
    assert(fd >= 0);
    fclose(f);
  }

  virtual int64_t Write(const void *buf, uint64_t size) {
    return write(fd, buf, size);
  }

  virtual int Reset() {
    int retval = ftruncate(fd, 0);
    assert(retval == 0);
    return 0;
  }

  virtual int Purge() { return Reset(); }

  virtual bool IsValid() { return fd >= 0; }

  int Flush() { return 0; }

  bool Reserve(size_t /*size*/) { return true; }

  bool RequiresReserve() { return false; }

  std::string Describe() {
    std::string result = "Test Sink for path " + path;
    result += " and " + StringifyInt(fd);
    return result;
  }

  ~TestSink() {
    close(fd);
    unlink(path.c_str());
  }

  int fd;
  string path;
};


//------------------------------------------------------------------------------


TEST_F(T_Download, LocalFile) {
  string dest_path;
  FILE *fdest = CreateTemporaryFile(&dest_path);
  ASSERT_TRUE(fdest != NULL);
  UnlinkGuard unlink_guard(dest_path);

  string src_path = GetAbsolutePath(GetSmallFile());
  string src_url = "file://" + src_path;

  cvmfs::FileSink filesink(fdest);
  JobInfo info(&src_url, false /* compressed */, false /* probe hosts */, NULL,
               &filesink);
  download_mgr.Fetch(&info);
  EXPECT_EQ(info.error_code(), kFailOk);
  fclose(fdest);
}

TEST_F(T_Download, RemoteFile) {
  string dest_path;
  FILE *fdest = CreateTemporaryFile(&dest_path);
  ASSERT_TRUE(fdest != NULL);
  UnlinkGuard unlink_guard(dest_path);

  MockFileServer file_server(8082, sandbox_path_);

  string src_path = GetSmallFile();
  string src_url = "http://127.0.0.1:8082/" + GetFileName(src_path);

  cvmfs::FileSink filesink(fdest);
  JobInfo info(&src_url, false /* compressed */, false /* probe hosts */, NULL,
               &filesink);
  download_mgr.Fetch(&info);
  EXPECT_EQ(file_server.num_processed_requests(), 1);
  EXPECT_EQ(info.error_code(), kFailOk);
  fclose(fdest);
}

TEST_F(T_Download, Clone) {
  DownloadManager *download_mgr_cloned = download_mgr.Clone(
      perf::StatisticsTemplate("x", &statistics), "cloned");

  string dest_path;
  FILE *fdest = CreateTemporaryFile(&dest_path);
  ASSERT_TRUE(fdest != NULL);
  UnlinkGuard unlink_guard(dest_path);
  char buf = '1';
  fwrite(&buf, 1, 1, fdest);
  fclose(fdest);

  string url = "file://" + dest_path;
  cvmfs::MemSink memsink;
  JobInfo info(&url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr_cloned->Fetch(&info);
  ASSERT_EQ(info.error_code(), kFailOk);
  ASSERT_EQ(memsink.pos(), 1U);
  EXPECT_EQ(memsink.data()[0], '1');
  delete download_mgr_cloned;

  // Don't crash
  DownloadManager *dm = new DownloadManager(
      1, perf::StatisticsTemplate("h", &statistics));
  download_mgr_cloned = dm->Clone(perf::StatisticsTemplate("y", &statistics),
                                  "cloned");
  delete dm;
  delete download_mgr_cloned;
}


namespace {
// A server that cuts the first response short, so the client has to resume.
// The second and later requests are served in full, honouring Range.
struct ResumeState {
  ResumeState() : requests(0), cut_responses(1), honour_range(true),
                  saw_range(false), range_first_byte(0) { }
  int requests;
  int cut_responses;  ///< how many replies are truncated before a full one
  bool honour_range;
  bool saw_range;
  uint64_t range_first_byte;
  std::string content;
};

HTTPResponse ResumeHandler(const HTTPRequest &req, void *data) {
  ResumeState *st = static_cast<ResumeState *>(data);
  st->requests++;

  uint64_t range_from = 0;
  bool has_range = false;
  for (HTTPHeaderList::const_iterator i = req.headers.begin();
       i != req.headers.end(); ++i) {
    if (i->first == "Range") {
      has_range = true;
      st->saw_range = true;
      unsigned long long f = 0;  // NOLINT(runtime/int)
      sscanf(i->second.c_str(), "bytes=%llu", &f);
      range_from = f;
      st->range_first_byte = range_from;
    }
  }

  HTTPResponse response;
  if (st->requests <= st->cut_responses) {
    // Announce what is left but deliver only part of it, then the mock server
    // closes the connection: a classic short transfer.  Each cut reply still
    // carries enough bytes to count as progress.
    // A server that does not honour Range answers it with a 200 and the
    // whole object, cut short like everything else it sends.
    if (!st->honour_range) {
      range_from = 0;
    }
    const size_t total_left = st->content.length() - range_from;
    const size_t cut = total_left / 2;
    response.raw = true;
    response.body = string((has_range && st->honour_range)
                               ? "HTTP/1.1 206 Partial Content"
                               : "HTTP/1.1 200 OK")
                    + "\r\nContent-Length: " + StringifyUint(total_left)
                    + "\r\n\r\n" + st->content.substr(range_from, cut);
    return response;
  }

  if (has_range && st->honour_range) {
    response.raw = true;
    response.body = "HTTP/1.1 206 Partial Content\r\nContent-Length: "
                    + StringifyUint(st->content.length() - range_from)
                    + "\r\n\r\n" + st->content.substr(range_from);
    return response;
  }

  // Either no Range was asked for, or this server refuses to honour it.
  response.code = 200;
  response.body = st->content;
  return response;
}
}  // namespace


TEST_F(T_Download, ResumeShortTransferWithRange) {
  // The branch resumes a transfer that was cut after delivering data, instead
  // of starting it again from byte zero.  Nothing covered this.
  ResumeState st;
  st.content = std::string(64 * 1024, 'q');
  shash::Any content_hash(shash::kSha1);
  shash::HashString(st.content, &content_hash);

  MockHTTPServer server(8104);
  ASSERT_TRUE(server.SetResponseCallback(ResumeHandler, &st));
  ASSERT_TRUE(server.Start());
  download_mgr.SetProxyChain("DIRECT", "", DownloadManager::kSetProxyBoth);
  // Retries are off by default in a bare manager, and a resume is a retry.
  download_mgr.SetRetryParameters(3, 0, 0);

  // Resuming is only offered to sinks that do not pre-reserve their space, so
  // a file sink rather than a memory one -- see Sink::RequiresReserve().
  string dest_path;
  FILE *fdest = CreateTemporaryFile(&dest_path);
  ASSERT_TRUE(fdest != NULL);
  cvmfs::FileSink filesink(fdest);

  const string url = "http://127.0.0.1:8104/object";
  JobInfo info(&url, false /* compressed */, false /* probe hosts */,
               &content_hash, &filesink);
  download_mgr.Fetch(&info);

  EXPECT_EQ(kFailOk, info.error_code());
  EXPECT_TRUE(st.saw_range) << "the retry did not ask for a byte range";
  // The resume must start exactly where the first attempt stopped.
  EXPECT_EQ(st.content.length() / 2, st.range_first_byte);
  fclose(fdest);

  const int fd_read = open(dest_path.c_str(), O_RDONLY);
  ASSERT_GE(fd_read, 0);
  string written;
  EXPECT_TRUE(SafeReadToString(fd_read, &written));
  close(fd_read);
  EXPECT_EQ(st.content, written);
  unlink(dest_path.c_str());
  server.Stop();
}


TEST_F(T_Download, ResumeRestartsWhenServerIgnoresRange) {
  // A server that answers a Range request with a plain 200 would otherwise
  // have its full body appended behind the bytes already kept, silently
  // corrupting the object.  The client must notice and start over.
  ResumeState st;
  st.content = std::string(64 * 1024, 'z');
  st.honour_range = false;
  shash::Any content_hash(shash::kSha1);
  shash::HashString(st.content, &content_hash);

  MockHTTPServer server(8105);
  ASSERT_TRUE(server.SetResponseCallback(ResumeHandler, &st));
  ASSERT_TRUE(server.Start());
  download_mgr.SetProxyChain("DIRECT", "", DownloadManager::kSetProxyBoth);
  download_mgr.SetRetryParameters(3, 0, 0);

  string dest_path;
  FILE *fdest = CreateTemporaryFile(&dest_path);
  ASSERT_TRUE(fdest != NULL);
  cvmfs::FileSink filesink(fdest);

  const string url = "http://127.0.0.1:8105/object";
  JobInfo info(&url, false /* compressed */, false /* probe hosts */,
               &content_hash, &filesink);
  download_mgr.Fetch(&info);
  fclose(fdest);

  // Whatever the outcome, what lands on disk must never be the full body
  // appended behind the half already kept.
  const int fd_read = open(dest_path.c_str(), O_RDONLY);
  ASSERT_GE(fd_read, 0);
  string written;
  EXPECT_TRUE(SafeReadToString(fd_read, &written));
  close(fd_read);
  EXPECT_LE(written.length(), st.content.length());
  if (info.error_code() == kFailOk) {
    EXPECT_EQ(st.content, written);
  }
  unlink(dest_path.c_str());
  server.Stop();
}


TEST_F(T_Download, ContentLengthIsRecorded) {
  ResumeState st;
  st.content = std::string(4096, 'c');
  MockHTTPServer server(8106);
  ASSERT_TRUE(server.SetResponseCallback(ResumeHandler, &st));
  ASSERT_TRUE(server.Start());
  download_mgr.SetProxyChain("DIRECT", "", DownloadManager::kSetProxyBoth);

  // Skip the deliberately-short first reply.
  st.requests = 1;
  const string url = "http://127.0.0.1:8106/object";
  cvmfs::MemSink sink;
  JobInfo info(&url, false, false, NULL, &sink);
  download_mgr.Fetch(&info);

  EXPECT_EQ(kFailOk, info.error_code());
  EXPECT_EQ(static_cast<int64_t>(st.content.length()), info.content_length());
  server.Stop();
}


TEST_F(T_Download, IgnoredRangeEndsTheFreeRetries) {
  // A peer that delivers a few KiB, cuts the stream, and answers every Range
  // with a plain 200 used to be retried for ever: each cut attempt made
  // "progress", the budget was refreshed, and the restart began again.  Once
  // the Range is ignored, resuming is switched off for the object and the
  // ordinary, finite budget applies.
  ResumeState st;
  st.content = std::string(96 * 1024, 'x');
  st.cut_responses = 1000;   // cut every reply
  st.honour_range = false;
  shash::Any content_hash(shash::kSha1);
  shash::HashString(st.content, &content_hash);

  MockHTTPServer server(8129);
  ASSERT_TRUE(server.SetResponseCallback(ResumeHandler, &st));
  ASSERT_TRUE(server.Start());
  download_mgr.SetProxyChain("DIRECT", "", DownloadManager::kSetProxyBoth);
  download_mgr.SetRetryParameters(2, 0, 0);
  download_mgr.SetTimeout(0, 0);

  string dest_path;
  FILE *fdest = CreateTemporaryFile(&dest_path);
  ASSERT_TRUE(fdest != NULL);
  cvmfs::FileSink filesink(fdest);

  const string url = "http://127.0.0.1:8129/object";
  JobInfo info(&url, false, false, &content_hash, &filesink);
  download_mgr.Fetch(&info);
  fclose(fdest);
  unlink(dest_path.c_str());
  server.Stop();

  EXPECT_NE(kFailOk, info.error_code());
  EXPECT_TRUE(st.saw_range) << "a resume was never attempted";
  EXPECT_TRUE(info.resume_disabled());
  // Attempt, resume (ignored), then at most the configured retries.
  EXPECT_LE(st.requests, 8) << "unbounded retry storm";
}


TEST_F(T_Download, CutStreamsWithProgressAreStillBounded) {
  // The other half: a peer that honours Range but cuts every reply after
  // delivering something.  Each attempt makes progress, so none of them
  // consumes the retry budget.  Progress accumulates, so this converges on
  // its own; the test is a sanity check on the request count, not the
  // discriminator for the livelock (that is IgnoredRangeEndsTheFreeRetries).
  // kMaxFreeRetries is defence in depth for a peer that lies about ranges.
  ResumeState st;
  st.content = std::string(96 * 1024, 'y');
  st.cut_responses = 1000;
  shash::Any content_hash(shash::kSha1);
  shash::HashString(st.content, &content_hash);

  MockHTTPServer server(8131);
  ASSERT_TRUE(server.SetResponseCallback(ResumeHandler, &st));
  ASSERT_TRUE(server.Start());
  download_mgr.SetProxyChain("DIRECT", "", DownloadManager::kSetProxyBoth);
  download_mgr.SetRetryParameters(2, 0, 0);
  download_mgr.SetTimeout(0, 0);

  string dest_path;
  FILE *fdest = CreateTemporaryFile(&dest_path);
  ASSERT_TRUE(fdest != NULL);
  cvmfs::FileSink filesink(fdest);

  const string url = "http://127.0.0.1:8131/object";
  JobInfo info(&url, false, false, &content_hash, &filesink);
  download_mgr.Fetch(&info);
  fclose(fdest);
  unlink(dest_path.c_str());
  server.Stop();

  // Halving what is left on every attempt, 96 KiB is finished within the
  // free-retry allowance -- but a peer that always cuts is the point here,
  // so what matters is that the count of requests is bounded either way.
  EXPECT_TRUE(st.saw_range);
  EXPECT_LE(st.requests, 32 + 2 + 2) << "free retries are not bounded";
}


TEST_F(T_Download, ProgressRefreshesTheRetryBudget) {
  // The branch lets an attempt that actually delivered bytes off the retry
  // budget (kMinResumeProgress), so a large object can still finish on a link
  // that keeps cutting long transfers.  With a budget of one retry and two
  // cut responses, the object only completes if that rule holds.
  ResumeState st;
  st.content = std::string(96 * 1024, 'p');
  st.cut_responses = 2;
  shash::Any content_hash(shash::kSha1);
  shash::HashString(st.content, &content_hash);

  MockHTTPServer server(8107);
  ASSERT_TRUE(server.SetResponseCallback(ResumeHandler, &st));
  ASSERT_TRUE(server.Start());
  download_mgr.SetProxyChain("DIRECT", "", DownloadManager::kSetProxyBoth);
  download_mgr.SetRetryParameters(1, 0, 0);  // a single retry on its own
  // CanRetry() also grants cheap retries while the attempts without progress
  // have not cost one timeout period.  A zero timeout empties that budget, so
  // the third request below can only come from the progress rule.
  download_mgr.SetTimeout(0, 0);

  string dest_path;
  FILE *fdest = CreateTemporaryFile(&dest_path);
  ASSERT_TRUE(fdest != NULL);
  cvmfs::FileSink filesink(fdest);

  const string url = "http://127.0.0.1:8107/object";
  JobInfo info(&url, false, false, &content_hash, &filesink);
  download_mgr.Fetch(&info);
  fclose(fdest);

  EXPECT_EQ(kFailOk, info.error_code());
  EXPECT_GE(st.requests, 3) << "budget was not refreshed by progress";
  EXPECT_TRUE(st.saw_range) << "the retries did not resume";
  // Progress does not consume the budget: nothing was counted as a retry.
  EXPECT_EQ(0U, info.num_retries());
  const int fd_read = open(dest_path.c_str(), O_RDONLY);
  ASSERT_GE(fd_read, 0);
  string written;
  EXPECT_TRUE(SafeReadToString(fd_read, &written));
  close(fd_read);
  EXPECT_EQ(st.content, written);
  unlink(dest_path.c_str());
  server.Stop();
}


TEST_F(T_Download, Multiple) {
  string dest_path;
  FILE *fdest = CreateTemporaryFile(&dest_path);
  ASSERT_TRUE(fdest != NULL);
  UnlinkGuard unlink_guard(dest_path);

  string src_path = GetAbsolutePath(GetSmallFile());
  string src_url = "file://" + src_path;

  DownloadManager second_mgr(8,
                             perf::StatisticsTemplate("second", &statistics));

  cvmfs::FileSink filesink(fdest);
  JobInfo info(&src_url, false /* compressed */, false /* probe hosts */, NULL,
               &filesink);
  JobInfo info2(&src_url, false /* compressed */, false /* probe hosts */, NULL,
                &filesink);
  download_mgr.Fetch(&info);
  second_mgr.Fetch(&info2);
  EXPECT_EQ(info.error_code(), kFailOk);
  EXPECT_EQ(info2.error_code(), kFailOk);
  fclose(fdest);
}


TEST_F(T_Download, RemoteFile2Mem) {
  string src_path = GetSmallFile();
  string src_content = GetFileContents(src_path);

  MockFileServer file_server(8082, sandbox_path_);

  string url = "http://127.0.0.1:8082/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);
  ASSERT_EQ(file_server.num_processed_requests(), 1);
  ASSERT_EQ(info.error_code(), kFailOk);
  ASSERT_EQ(memsink.pos(), src_content.length());
  EXPECT_STREQ(reinterpret_cast<char *>(memsink.data()), src_content.c_str());
}


TEST_F(T_Download, BackoffDefersInsteadOfSleepingWhenThreaded) {
  // With one I/O thread serving every transfer of a manager, sleeping out a
  // retry there stalls all of them behind the slowest one.  In multi-threaded
  // mode Backoff() therefore records when the job may run again and returns at
  // once, leaving MainDownload to re-queue it; single-threaded, where the
  // sleep only costs the caller its own transfer, it still sleeps.
  //
  // This is asserted on the mechanism rather than on wall-clock behaviour: the
  // first backoff is prng_.Next(init + 1), so its length is not predictable
  // enough to time reliably.
  const string url = "http://127.0.0.1:8122/object";
  cvmfs::MemSink sink;
  download_mgr.SetRetryParameters(2, 1000, 1000);
  // Prng::Next() returns [0, boundary), so the first draw of
  // prng_.Next(backoff_init_ms + 1) can legitimately be 0 and the
  // single-threaded call would then not sleep at all.  Seed it so the draw is
  // fixed (seed 1 yields 423 ms) and the assertions below are deterministic.
  download_mgr.prng_.InitSeed(1);

  JobInfo single(&url, false, false, NULL, &sink);
  single.SetErrorCode(kFailHostTooSlow);
  const uint64_t t0 = platform_monotonic_time_ns() / 1000000;
  download_mgr.Backoff(&single);
  const uint64_t slept_ms = (platform_monotonic_time_ns() / 1000000) - t0;
  EXPECT_EQ(0U, single.retry_not_before_ms())
      << "a single-threaded backoff should not defer";
  EXPECT_EQ(423U, single.backoff_ms());
  // It really did wait, give or take the clock granularity.
  EXPECT_GE(slept_ms + 20, static_cast<uint64_t>(single.backoff_ms()));

  download_mgr.Spawn();   // multi-threaded from here

  JobInfo threaded(&url, false, false, NULL, &sink);
  threaded.SetErrorCode(kFailHostTooSlow);
  const uint64_t t1 = platform_monotonic_time_ns() / 1000000;
  download_mgr.Backoff(&threaded);
  const uint64_t elapsed_ms = (platform_monotonic_time_ns() / 1000000) - t1;

  EXPECT_LT(elapsed_ms, 100U)
      << "Backoff() slept on the I/O thread for " << elapsed_ms << " ms";
  EXPECT_GE(threaded.retry_not_before_ms(), t1)
      << "the job was not deferred to a later time";
  EXPECT_LE(threaded.retry_not_before_ms(),
            t1 + threaded.backoff_ms() + 50);
}


TEST_F(T_Download, RemoteFileRedirect) {
  string src_path = GetSmallFile();
  string src_content = GetFileContents(src_path);

  MockFileServer file_server(8082, sandbox_path_);
  MockRedirectServer redirect_server(8083, "http://127.0.0.1:8082");

  string url = "http://127.0.0.1:8083/" + GetFileName(src_path);

  download_mgr.EnableRedirects();
  cvmfs::MemSink memsink;
  JobInfo info(&url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);
  ASSERT_EQ(file_server.num_processed_requests(), 1);
  ASSERT_EQ(redirect_server.num_processed_requests(), 1);
  ASSERT_EQ(info.error_code(), kFailOk);
  ASSERT_EQ(memsink.pos(), src_content.length());
  EXPECT_STREQ(reinterpret_cast<char *>(memsink.data()), src_content.c_str());
}

TEST_F(T_Download, RemoteFileSimpleProxy) {
  string src_path = GetSmallFile();
  string src_content = GetFileContents(src_path);

  MockProxyServer proxy_server(8083);
  MockFileServer file_server(8082, sandbox_path_);

  download_mgr.SetProxyChain("http://127.0.0.1:8083", "",
                             DownloadManager::kSetProxyRegular);
  string url = "http://127.0.0.1:8082/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);
  ASSERT_EQ(proxy_server.num_processed_requests(), 1);
  ASSERT_EQ(file_server.num_processed_requests(), 1);
  ASSERT_EQ(info.error_code(), kFailOk);
  ASSERT_EQ(memsink.pos(), src_content.length());
  EXPECT_STREQ(reinterpret_cast<char *>(memsink.data()), src_content.c_str());
}

TEST_F(T_Download, RefusedConnectGetsExtraCheapRetries) {
  // A refused connection returns at once, so CanRetry()'s extended budget --
  // "keep retrying while the attempts without progress have not yet cost one
  // timeout period" -- should grant several attempts beyond max_retries before
  // the proxy is written off.  A dead peer that times out on each attempt
  // consumes that budget immediately instead.
  //
  // Nothing listens on 8115, and the kernel refuses instantly.  Measured with
  // these parameters: 3 attempts spread over ~7.7s.  With backoff disabled the
  // same budget allows ~163 attempts inside one 3s period, which is what makes
  // the rule "cheap failures are worth repeating" rather than a fixed count.
  download_mgr.SetProxyChain("http://127.0.0.1:8115", "",
                             DownloadManager::kSetProxyBoth);
  download_mgr.SetRetryParameters(1, 2000, 10000);  // stock-like backoff
  download_mgr.SetTimeout(5, 5);

  const string url = "http://127.0.0.1:8116/object";
  cvmfs::MemSink sink;
  JobInfo info(&url, false, false, NULL, &sink);
  download_mgr.Fetch(&info);

  EXPECT_EQ(kFailProxyConnection, info.error_code());
  // Strictly more attempts than the nominal single retry: that extra is the
  // cheap-failure budget in CanRetry(), not the max_retries allowance.
  EXPECT_GT(static_cast<int>(info.num_retries()), 1)
      << "a refused connect was written off after the nominal retry budget";
}


TEST_F(T_Download, RemoteFileProxyRedirect) {
  string src_path = GetSmallFile();
  string src_content = GetFileContents(src_path);

  MockProxyServer proxy_server(8084);
  MockRedirectServer redirect_server(8083, "http://127.0.0.1:8082");
  MockFileServer file_server(8082, sandbox_path_);

  download_mgr.SetProxyChain("http://127.0.0.1:8084", "",
                             DownloadManager::kSetProxyRegular);
  download_mgr.EnableRedirects();
  string url = "http://127.0.0.1:8083/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);
  ASSERT_EQ(proxy_server.num_processed_requests(), 2);
  ASSERT_EQ(redirect_server.num_processed_requests(), 1);
  ASSERT_EQ(file_server.num_processed_requests(), 1);
  ASSERT_EQ(info.num_used_hosts(), 1);
  ASSERT_EQ(info.error_code(), kFailOk);
  ASSERT_EQ(memsink.pos(), src_content.length());
  EXPECT_STREQ(reinterpret_cast<char *>(memsink.data()), src_content.c_str());
}

TEST_F(T_Download, LocalFile2Mem) {
  string src_path = GetSmallFile();
  string src_content = GetFileContents(src_path);

  string url = "file://" + GetAbsolutePath(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);
  ASSERT_EQ(info.error_code(), kFailOk);
  ASSERT_EQ(memsink.pos(), src_content.length());
  EXPECT_STREQ(reinterpret_cast<char *>(memsink.data()), src_content.c_str());
}

TEST_F(T_Download, RemoteFileSwitchHosts) {
  string src_path = GetSmallFile();
  string src_content = GetFileContents(src_path);

  MockFileServer file_server(8082, sandbox_path_);
  download_mgr.SetHostChain("http://127.0.0.1:8083;http://127.0.0.1:8082");
  string url = "/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&url, false /* compressed */, true /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);
  ASSERT_EQ(file_server.num_processed_requests(), 1);
  ASSERT_EQ(info.num_used_hosts(), 2);
  ASSERT_EQ(info.error_code(), kFailOk);
  ASSERT_EQ(memsink.pos(), src_content.length());
  EXPECT_STREQ(reinterpret_cast<char *>(memsink.data()), src_content.c_str());
}

TEST_F(T_Download, CancelRequest) {
  string src_path = GetSmallFile();
  string src_content = GetFileContents(src_path);

  MockFileServer file_server(8082, sandbox_path_);
  download_mgr.SetHostChain("http://127.0.0.1:8083;http://127.0.0.1:8082");
  string url = "/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&url, false /* compressed */, true /* probe hosts */, NULL,
               &memsink);
  TestInterruptCue tci;
  info.SetInterruptCue(&tci);
  download_mgr.Fetch(&info);
  ASSERT_EQ(info.num_used_hosts(), 1);
  ASSERT_EQ(info.error_code(), kFailCanceled);
  EXPECT_EQ(NULL, memsink.data());
}

TEST_F(T_Download, RemoteFileSwitchHostsAfterRedirect) {
  string src_path = GetSmallFile();
  string src_content = GetFileContents(src_path);

  MockRedirectServer redirect_server(8083, "http://127.0.0.1:8084");
  MockFileServer file_server(8082, sandbox_path_);

  download_mgr.EnableRedirects();
  download_mgr.SetHostChain("http://127.0.0.1:8083;http://127.0.0.1:8082");
  string url = "/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&url, false /* compressed */, true /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);
  ASSERT_EQ(info.num_used_hosts(), 2);
  ASSERT_EQ(info.error_code(), kFailOk);
  ASSERT_EQ(memsink.pos(), src_content.length());
  EXPECT_STREQ(reinterpret_cast<char *>(memsink.data()), src_content.c_str());
}

TEST_F(T_Download, RemoteFileSwitchProxies) {
  string src_path = GetSmallFile();
  int src_fd = open(src_path.c_str(), O_RDONLY);
  string src_content;
  SafeReadToString(src_fd, &src_content);
  close(src_fd);

  MockFileServer file_server(8082, sandbox_path_);
  MockProxyServer proxy_server(8083);
  download_mgr.SetProxyChain("http://127.0.0.1:8084;http://127.0.0.1:8083", "",
                             DownloadManager::kSetProxyRegular);

  string src_url = "http://127.0.0.1:8082/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&src_url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);
  ASSERT_EQ(file_server.num_processed_requests(), 1);
  ASSERT_EQ(proxy_server.num_processed_requests(), 1);
  ASSERT_EQ(info.num_used_proxies(), 2);
  ASSERT_EQ(info.error_code(), kFailOk);
  ASSERT_EQ(memsink.pos(), src_content.length());
  EXPECT_STREQ(reinterpret_cast<char *>(memsink.data()), src_content.c_str());
}

TEST_F(T_Download, RemoteFileEmpty) {
  string src_path = GetEmptyFile();

  MockFileServer file_server(8082, sandbox_path_);

  string src_url = "http://127.0.0.1:8082/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&src_url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);
  ASSERT_EQ(file_server.num_processed_requests(), 1);
  ASSERT_EQ(info.error_code(), kFailOk);
  ASSERT_EQ(memsink.pos(), 0U);
}

TEST_F(T_Download, LocalFile2Sink) {
  string dest_path;
  FILE *fdest = CreateTemporaryFile(&dest_path);
  ASSERT_TRUE(fdest != NULL);
  UnlinkGuard unlink_guard(dest_path);
  char buf = '1';
  fwrite(&buf, 1, 1, fdest);
  fflush(fdest);

  TestSink test_sink;
  string url = "file://" + dest_path;
  JobInfo info(&url, false /* compressed */, false /* probe hosts */,
               NULL /* expected hash */, &test_sink);
  download_mgr.Fetch(&info);
  EXPECT_EQ(info.error_code(), kFailOk);
  EXPECT_EQ(1, pread(test_sink.fd, &buf, 1, 0));
  EXPECT_EQ('1', buf);

  rewind(fdest);
  Prng prng;
  prng.InitLocaltime();
  unsigned N = 16 * 1024;
  unsigned size = N * sizeof(uint32_t);
  uint32_t rnd_buf[N];  // 64kB
  for (unsigned i = 0; i < N; ++i)
    rnd_buf[i] = prng.Next(2147483647);
  shash::Any checksum(shash::kMd5);
  EXPECT_TRUE(
      zlib::CompressMem2File(reinterpret_cast<const unsigned char *>(rnd_buf),
                             size, fdest, &checksum));
  fclose(fdest);

  TestSink test_sink2;
  JobInfo info2(&url, true /* compressed */, false /* probe hosts */,
                &checksum /* expected hash */, &test_sink2);
  download_mgr.Fetch(&info2);
  EXPECT_EQ(info2.error_code(), kFailOk);
  EXPECT_EQ(size, GetFileSize(test_sink2.path));

  uint32_t validation[N];
  EXPECT_EQ(static_cast<int>(size), pread(test_sink2.fd, &validation, size, 0));
  EXPECT_EQ(0, memcmp(validation, rnd_buf, size));
}


TEST_F(T_Download, StripDirect) {
  string cleaned = "FALSE";
  EXPECT_FALSE(download_mgr.StripDirect("", &cleaned));
  EXPECT_EQ("", cleaned);
  EXPECT_TRUE(download_mgr.StripDirect("DIRECT", &cleaned));
  EXPECT_EQ("", cleaned);
  EXPECT_TRUE(download_mgr.StripDirect("DIRECT;DIRECT", &cleaned));
  EXPECT_EQ("", cleaned);
  EXPECT_TRUE(download_mgr.StripDirect("DIRECT;DIRECT|DIRECT", &cleaned));
  EXPECT_EQ("", cleaned);
  EXPECT_TRUE(download_mgr.StripDirect("DIRECT;DIRECT|", &cleaned));
  EXPECT_EQ("", cleaned);
  EXPECT_TRUE(download_mgr.StripDirect(";", &cleaned));
  EXPECT_EQ("", cleaned);
  EXPECT_TRUE(download_mgr.StripDirect(";||;;;|||", &cleaned));
  EXPECT_EQ("", cleaned);
  EXPECT_FALSE(download_mgr.StripDirect("A|B", &cleaned));
  EXPECT_EQ("A|B", cleaned);
  EXPECT_FALSE(download_mgr.StripDirect("A|B;C|D;E|F|G", &cleaned));
  EXPECT_EQ("A|B;C|D;E|F|G", cleaned);
  EXPECT_TRUE(download_mgr.StripDirect("A|DIRECT;C|D;E|F;DIRECT", &cleaned));
  EXPECT_EQ("A;C|D;E|F", cleaned);
}


TEST_F(T_Download, ValidateGeoReply) {
  vector<uint64_t> geo_order;
  EXPECT_FALSE(download_mgr.ValidateGeoReply("", geo_order.size(), &geo_order));

  geo_order.push_back(0);
  EXPECT_FALSE(
      download_mgr.ValidateGeoReply("a", geo_order.size(), &geo_order));
  EXPECT_FALSE(
      download_mgr.ValidateGeoReply("1,1", geo_order.size(), &geo_order));
  EXPECT_FALSE(
      download_mgr.ValidateGeoReply("1,3", geo_order.size(), &geo_order));
  EXPECT_FALSE(
      download_mgr.ValidateGeoReply("2,3", geo_order.size(), &geo_order));
  EXPECT_FALSE(
      download_mgr.ValidateGeoReply("2", geo_order.size(), &geo_order));
  EXPECT_TRUE(download_mgr.ValidateGeoReply("1", geo_order.size(), &geo_order));
  EXPECT_EQ(geo_order.size(), 1U);
  EXPECT_EQ(geo_order[0], 0U);

  geo_order.push_back(0);
  EXPECT_FALSE(
      download_mgr.ValidateGeoReply(",", geo_order.size(), &geo_order));
  EXPECT_FALSE(
      download_mgr.ValidateGeoReply("2,", geo_order.size(), &geo_order));
  EXPECT_FALSE(
      download_mgr.ValidateGeoReply("1", geo_order.size(), &geo_order));
  EXPECT_FALSE(
      download_mgr.ValidateGeoReply("3,2,1", geo_order.size(), &geo_order));
  EXPECT_TRUE(
      download_mgr.ValidateGeoReply("2,1", geo_order.size(), &geo_order));
  EXPECT_EQ(geo_order.size(), 2U);
  EXPECT_EQ(geo_order[0], 1U);
  EXPECT_EQ(geo_order[1], 0U);

  EXPECT_TRUE(
      download_mgr.ValidateGeoReply("2,1\n", geo_order.size(), &geo_order));
  EXPECT_EQ(geo_order.size(), 2U);
  EXPECT_EQ(geo_order[0], 1U);
  EXPECT_EQ(geo_order[1], 0U);

  geo_order.push_back(0);
  geo_order.push_back(0);
  EXPECT_TRUE(
      download_mgr.ValidateGeoReply("4,3,1,2\n", geo_order.size(), &geo_order));
  EXPECT_EQ(geo_order.size(), 4U);
  EXPECT_EQ(geo_order[0], 3U);
  EXPECT_EQ(geo_order[1], 2U);
  EXPECT_EQ(geo_order[2], 0U);
  EXPECT_EQ(geo_order[3], 1U);
}


TEST_F(T_Download, ParseHttpCode) {
  char digits[3];
  digits[0] = '0';
  digits[1] = '0';
  digits[2] = 'a';
  EXPECT_EQ(-1, DownloadManager::ParseHttpCode(digits));
  digits[0] = '0';
  digits[1] = '0';
  digits[2] = '0';
  EXPECT_EQ(0, DownloadManager::ParseHttpCode(digits));
  digits[0] = '0';
  digits[1] = '0';
  digits[2] = '1';
  EXPECT_EQ(1, DownloadManager::ParseHttpCode(digits));
  digits[0] = '1';
  digits[1] = '0';
  digits[2] = '1';
  EXPECT_EQ(101, DownloadManager::ParseHttpCode(digits));
  digits[0] = '9';
  digits[1] = '9';
  digits[2] = '9';
  EXPECT_EQ(999, DownloadManager::ParseHttpCode(digits));
}

TEST_F(T_Download, EscapeUrl) {
  const std::string url = "http://ab0341.¡ÿϦ랝";  // c2a1 c3bf cfa6 eb9e9d
  const std::string correct = "http://ab0341.%C2%A1%C3%BF%CF%A6%EB%9E%9D";
  const std::string res = download_mgr.EscapeUrl(0, url);

  EXPECT_TRUE(res == correct);
}

}  // namespace download
