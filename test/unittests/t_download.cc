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

namespace {
// Stands in for a saturated proxy: it answers, so it is plainly reachable,
// but the body is cut short.  That is a throughput symptom, reported as
// kFailProxyShortTransfer, not evidence of a dead peer.
HTTPResponse TruncatingProxyHandler(const HTTPRequest & /* req */, void *data) {
  int *hits = static_cast<int *>(data);
  if (hits)
    (*hits)++;
  HTTPResponse response;
  response.raw = true;
  response.body = "HTTP/1.1 200 OK\r\nContent-Length: 65536\r\n\r\nshort";
  return response;
}

HTTPResponse CountingProxyHandler(const HTTPRequest & /* req */, void *data) {
  int *hits = static_cast<int *>(data);
  if (hits)
    (*hits)++;
  HTTPResponse response;
  response.code = 200;
  response.body = "served by the fallback proxy";
  return response;
}

struct FixedBodyState {
  FixedBodyState() : hits(0) { }
  int hits;
  std::string body;
};

// A proxy that serves one fixed body for whatever it is asked.
HTTPResponse FixedBodyHandler(const HTTPRequest &, void *data) {
  FixedBodyState *st = static_cast<FixedBodyState *>(data);
  st->hits++;
  HTTPResponse response;
  response.code = 200;
  response.body = st->body;
  return response;
}
}  // namespace


TEST_F(T_Download, EscalatedProxyGroupClassification) {
  vector<vector<DownloadManager::ProxyInfo> > chain;
  unsigned current_group = 0;
  unsigned fallback_group = 0;

  // "proxy;DIRECT": group 0 is local, group 1 is the DIRECT tier.
  download_mgr.SetProxyChain("http://127.0.0.1:3128;DIRECT", "",
                             DownloadManager::kSetProxyBoth);
  download_mgr.GetProxyInfo(&chain, &current_group, &fallback_group);
  ASSERT_EQ(2U, chain.size());
  EXPECT_FALSE(download_mgr.IsEscalatedProxyGroup(0));
  EXPECT_TRUE(download_mgr.IsEscalatedProxyGroup(1));   // DIRECT tier
  EXPECT_FALSE(download_mgr.IsEscalatedProxyGroup(99));  // out of range

  // A proxy plus a fallback: group 0 is local, group 1 is off-site.
  download_mgr.SetProxyChain("http://127.0.0.1:3128", "http://127.0.0.2:3128",
                             DownloadManager::kSetProxyBoth);
  download_mgr.GetProxyInfo(&chain, &current_group, &fallback_group);
  ASSERT_EQ(2U, chain.size());
  EXPECT_FALSE(download_mgr.IsEscalatedProxyGroup(0));
  EXPECT_TRUE(download_mgr.IsEscalatedProxyGroup(1));   // fallback proxy

  // Two local groups and no DIRECT: moving between them is plain failover.
  download_mgr.SetProxyChain("http://127.0.0.1:3128;http://127.0.0.3:3128", "",
                             DownloadManager::kSetProxyBoth);
  EXPECT_FALSE(download_mgr.IsEscalatedProxyGroup(0));
  EXPECT_FALSE(download_mgr.IsEscalatedProxyGroup(1));
}


TEST_F(T_Download, SlowProxyDoesNotEscalateToDirect) {
  // The regression for the leak measured under load: a proxy that is merely
  // saturated must not promote the DIRECT tier, because that puts unproxied
  // traffic on the wire and then lets host failover roam the server list.
  string src_path = GetSmallFile();
  MockFileServer origin(8110, sandbox_path_);
  int proxy_hits = 0;
  MockHTTPServer slow_proxy(8111);
  ASSERT_TRUE(slow_proxy.SetResponseCallback(TruncatingProxyHandler,
                                             &proxy_hits));
  ASSERT_TRUE(slow_proxy.Start());

  download_mgr.SetProxyChain("http://127.0.0.1:8111;DIRECT", "",
                             DownloadManager::kSetProxyBoth);
  download_mgr.SetRetryParameters(2, 0, 0);

  const string url = "http://127.0.0.1:8110/" + GetFileName(src_path);
  cvmfs::MemSink sink;
  JobInfo info(&url, false /* compressed */, false /* probe hosts */, NULL,
               &sink);
  download_mgr.Fetch(&info);

  EXPECT_NE(kFailOk, info.error_code());
  EXPECT_GT(proxy_hits, 0) << "the proxy was never even tried";
  // The decisive assertion: the origin was never contacted directly.
  EXPECT_EQ(0, origin.num_processed_requests());
  EXPECT_NE("DIRECT", info.proxy());
  slow_proxy.Stop();
}


TEST_F(T_Download, SlowProxyDoesNotEscalateToFallback) {
  // Same rule for the off-site fallback proxies: saturation at the local
  // proxy is not a reason to ship the request to another site.
  string src_path = GetSmallFile();
  MockFileServer origin(8112, sandbox_path_);
  int slow_hits = 0;
  int fallback_hits = 0;
  MockHTTPServer slow_proxy(8113);
  ASSERT_TRUE(slow_proxy.SetResponseCallback(TruncatingProxyHandler,
                                             &slow_hits));
  ASSERT_TRUE(slow_proxy.Start());
  MockHTTPServer fallback_proxy(8114);
  ASSERT_TRUE(fallback_proxy.SetResponseCallback(CountingProxyHandler,
                                                 &fallback_hits));
  ASSERT_TRUE(fallback_proxy.Start());

  download_mgr.SetProxyChain("http://127.0.0.1:8113",
                             "http://127.0.0.1:8114",
                             DownloadManager::kSetProxyBoth);
  download_mgr.SetRetryParameters(2, 0, 0);

  const string url = "http://127.0.0.1:8112/" + GetFileName(src_path);
  cvmfs::MemSink sink;
  JobInfo info(&url, false, false, NULL, &sink);
  download_mgr.Fetch(&info);

  EXPECT_NE(kFailOk, info.error_code());
  EXPECT_GT(slow_hits, 0);
  EXPECT_EQ(0, fallback_hits) << "a saturated local proxy escalated off-site";
  EXPECT_EQ(0, origin.num_processed_requests());
  slow_proxy.Stop();
  fallback_proxy.Stop();
}


TEST_F(T_Download, MixedGroupStaysWhenAnyProxyAlive) {
  // A load-balance group with one dead proxy and one saturated one is a
  // group that still delivers data: it must not be left for DIRECT, and that
  // must not depend on which of the two happened to be burned last.  Proxy
  // selection is random, so run it several times.
  string src_path = GetSmallFile();
  MockFileServer origin(8110, sandbox_path_);
  int proxy_hits = 0;
  MockHTTPServer slow_proxy(8127);
  ASSERT_TRUE(slow_proxy.SetResponseCallback(TruncatingProxyHandler,
                                             &proxy_hits));
  ASSERT_TRUE(slow_proxy.Start());

  const string url = "http://127.0.0.1:8110/" + GetFileName(src_path);
  for (unsigned round = 0; round < 6; ++round) {
    download_mgr.SetProxyChain(
        "http://127.0.0.1:8097|http://127.0.0.1:8127;DIRECT", "",
        DownloadManager::kSetProxyBoth);
    download_mgr.SetRetryParameters(1, 0, 0);
    download_mgr.SetTimeout(2, 2);
    cvmfs::MemSink sink;
    JobInfo info(&url, false, false, NULL, &sink);
    download_mgr.Fetch(&info);
    EXPECT_NE(kFailOk, info.error_code()) << "round " << round;
    EXPECT_NE("DIRECT", info.proxy()) << "round " << round;
  }
  EXPECT_GT(proxy_hits, 0);
  EXPECT_EQ(0, origin.num_processed_requests())
      << "the origin was contacted directly although a proxy was alive";
  slow_proxy.Stop();
}


TEST_F(T_Download, RefusedProxyEscalatesToDirect) {
  // A refused connect means nothing is listening: a stopped squid.  (An
  // overloaded one leaves the SYN in its backlog or drops it, and times out
  // instead.)  That is exactly what the DIRECT tier is for.  Nothing listens
  // on 8097, so the kernel refuses at once; after the cheap retries the
  // request must go DIRECT rather than being pinned to the dead proxy.
  string src_path = GetSmallFile();
  MockFileServer file_server(8098, sandbox_path_);

  download_mgr.SetProxyChain("http://127.0.0.1:8097;DIRECT", "",
                             DownloadManager::kSetProxyBoth);
  download_mgr.SetRetryParameters(1, 0, 0);
  download_mgr.SetTimeout(2, 2);

  string src_url = "http://127.0.0.1:8098/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&src_url, false, false, NULL, &memsink);
  download_mgr.Fetch(&info);

  EXPECT_EQ(kFailOk, info.error_code());
  EXPECT_EQ("DIRECT", info.proxy());
  EXPECT_EQ(1, file_server.num_processed_requests());
}


TEST_F(T_Download, RefusedProxyReachesFallbackProxy) {
  // The same for CVMFS_FALLBACK_PROXY: a dead local proxy must hand over to
  // the off-site fallback, not fail the request while a healthy fallback is
  // configured.
  FixedBodyState st;
  st.body = "served by the fallback proxy";
  MockHTTPServer fallback(8125);
  ASSERT_TRUE(fallback.SetResponseCallback(FixedBodyHandler, &st));
  ASSERT_TRUE(fallback.Start());

  download_mgr.SetProxyChain("http://127.0.0.1:8097", "http://127.0.0.1:8125",
                             DownloadManager::kSetProxyBoth);
  download_mgr.SetRetryParameters(1, 0, 0);
  download_mgr.SetTimeout(2, 2);

  const string url = "http://127.0.0.1:8126/object";
  cvmfs::MemSink sink;
  JobInfo info(&url, false, false, NULL, &sink);
  download_mgr.Fetch(&info);

  EXPECT_EQ(kFailOk, info.error_code());
  EXPECT_EQ("http://127.0.0.1:8125", info.proxy());
  EXPECT_EQ(1, st.hits);
  EXPECT_EQ(st.body, string(reinterpret_cast<const char *>(sink.data()),
                            sink.pos()));
  fallback.Stop();
}


TEST_F(T_Download, UnresponsiveProxyEscalatesToDirect) {
  // A proxy that never answers the connect is genuinely unreachable, which is
  // what the DIRECT tier exists for.  192.0.2.0/24 is TEST-NET-1 from RFC
  // 5737: it is not routed, so the connect runs out of time rather than being
  // refused, and CURLINFO_CONNECT_TIME_T stays zero.
  string src_path = GetSmallFile();
  MockFileServer file_server(8096, sandbox_path_);

  download_mgr.SetProxyChain("http://192.0.2.1:3128;DIRECT", "",
                             DownloadManager::kSetProxyBoth);
  download_mgr.SetRetryParameters(1, 0, 0);
  download_mgr.SetTimeout(2, 2);

  string src_url = "http://127.0.0.1:8096/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&src_url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);

  EXPECT_EQ(kFailOk, info.error_code());
  EXPECT_EQ("DIRECT", info.proxy());
  EXPECT_EQ(1, file_server.num_processed_requests());
}


TEST_F(T_Download, UnresolvableProxyEscalatesToDirect) {
  // A proxy whose name does not resolve is pointed at a sentinel address the
  // kernel refuses at once.  That refusal must read as "the proxy is gone",
  // not as "a proxy is listening", or the DIRECT tier is never reached.
  string src_path = GetSmallFile();
  MockFileServer file_server(8098, sandbox_path_);

  download_mgr.SetProxyChain("http://proxy.does-not-exist.invalid:3128;DIRECT",
                             "", DownloadManager::kSetProxyBoth);
  download_mgr.SetRetryParameters(1, 0, 0);
  download_mgr.SetTimeout(2, 2);

  string src_url = "http://127.0.0.1:8098/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&src_url, false, false, NULL, &memsink);
  download_mgr.Fetch(&info);

  EXPECT_EQ(kFailOk, info.error_code());
  EXPECT_EQ("DIRECT", info.proxy());
  EXPECT_EQ(1, file_server.num_processed_requests());
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
