/**
 * This file is part of the CernVM File System.
 */

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <vector>

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
#include "util/smalloc.h"
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


TEST_F(T_Download, ProxyDemoteDirect) {
  bool has_direct = true;
  EXPECT_EQ("", download_mgr.DemoteDirect("", &has_direct));
  EXPECT_FALSE(has_direct);

  // DIRECT on its own stays a tier.
  EXPECT_EQ("DIRECT", download_mgr.DemoteDirect("DIRECT", &has_direct));
  EXPECT_TRUE(has_direct);
  EXPECT_EQ("DIRECT", download_mgr.DemoteDirect("DIRECT|DIRECT", &has_direct));
  EXPECT_TRUE(has_direct);

  // Sharing a load-balance group with a real proxy is not allowed: DIRECT
  // would take a share of the traffic while the proxy is healthy.
  EXPECT_EQ("A", download_mgr.DemoteDirect("A|DIRECT", &has_direct));
  EXPECT_FALSE(has_direct);
  EXPECT_EQ("A|B", download_mgr.DemoteDirect("A|DIRECT|B", &has_direct));
  EXPECT_FALSE(has_direct);

  // A group of its own is kept, and stays in the position it was written in.
  EXPECT_EQ("A;DIRECT", download_mgr.DemoteDirect("A;DIRECT", &has_direct));
  EXPECT_TRUE(has_direct);
  EXPECT_EQ("A;DIRECT;B", download_mgr.DemoteDirect("A;DIRECT;B",
                                                    &has_direct));
  EXPECT_TRUE(has_direct);
  EXPECT_EQ("A;DIRECT", download_mgr.DemoteDirect("A|DIRECT;DIRECT",
                                                  &has_direct));
  EXPECT_TRUE(has_direct);

  // Chains without DIRECT are passed through untouched.
  EXPECT_EQ("A|B;C", download_mgr.DemoteDirect("A|B;C", &has_direct));
  EXPECT_FALSE(has_direct);
}


TEST_F(T_Download, ProxyDirectKeptAsLastResortTier) {
  vector<vector<DownloadManager::ProxyInfo> > chain;
  unsigned current_group = 42;
  unsigned fallback_group = 42;

  // "proxy;DIRECT" must keep DIRECT as a separate, later tier.
  download_mgr.SetProxyChain("http://127.0.0.1:3128;DIRECT", "",
                             DownloadManager::kSetProxyBoth);
  download_mgr.GetProxyInfo(&chain, &current_group, &fallback_group);
  ASSERT_EQ(2U, chain.size());
  ASSERT_EQ(1U, chain[0].size());
  EXPECT_EQ("http://127.0.0.1:3128", chain[0][0].url);
  ASSERT_EQ(1U, chain[1].size());
  EXPECT_EQ("DIRECT", chain[1][0].url);
  // The proxy is where we start, and direct connections are permitted only
  // because the administrator asked for them.
  EXPECT_EQ(0U, current_group);
  EXPECT_FALSE(download_mgr.opt_proxy_mandatory_);

  // Configured fallback proxies must no longer discard the DIRECT tier.
  download_mgr.SetProxyChain("http://127.0.0.1:3128;DIRECT",
                             "http://127.0.0.2:3128",
                             DownloadManager::kSetProxyBoth);
  download_mgr.GetProxyInfo(&chain, &current_group, &fallback_group);
  ASSERT_EQ(3U, chain.size());
  EXPECT_EQ("http://127.0.0.1:3128", chain[0][0].url);
  EXPECT_EQ("DIRECT", chain[1][0].url);
  EXPECT_EQ("http://127.0.0.2:3128", chain[2][0].url);
  EXPECT_EQ(2U, fallback_group);
}


TEST_F(T_Download, ProxyDirectNotPeerOfLiveProxy) {
  vector<vector<DownloadManager::ProxyInfo> > chain;
  unsigned current_group = 42;
  unsigned fallback_group = 42;

  // "proxy|DIRECT" would otherwise load-balance unproxied traffic against a
  // healthy proxy.
  download_mgr.SetProxyChain("http://127.0.0.1:3128|DIRECT", "",
                             DownloadManager::kSetProxyBoth);
  download_mgr.GetProxyInfo(&chain, &current_group, &fallback_group);
  ASSERT_EQ(1U, chain.size());
  ASSERT_EQ(1U, chain[0].size());
  EXPECT_EQ("http://127.0.0.1:3128", chain[0][0].url);
  EXPECT_TRUE(download_mgr.opt_proxy_mandatory_);
}


TEST_F(T_Download, ProxyDirectOnlyPreserved) {
  vector<vector<DownloadManager::ProxyInfo> > chain;
  unsigned current_group = 42;
  unsigned fallback_group = 42;

  // Nothing but DIRECT is a deliberate choice: leave unproxied setups alone.
  download_mgr.SetProxyChain("DIRECT", "", DownloadManager::kSetProxyBoth);
  download_mgr.GetProxyInfo(&chain, &current_group, &fallback_group);
  ASSERT_EQ(1U, chain.size());
  ASSERT_EQ(1U, chain[0].size());
  EXPECT_EQ("DIRECT", chain[0][0].url);
  EXPECT_FALSE(download_mgr.opt_proxy_mandatory_);
}


TEST_F(T_Download, ProxyHealthyProxyIsUsedNotDirect) {
  // With "proxy;DIRECT" and a healthy proxy, every request must go through
  // the proxy; the DIRECT tier must stay untouched.
  string src_path = GetSmallFile();
  MockFileServer file_server(8094, sandbox_path_);
  MockProxyServer proxy_server(8095);
  download_mgr.SetProxyChain("http://127.0.0.1:8095;DIRECT", "",
                             DownloadManager::kSetProxyBoth);

  string src_url = "http://127.0.0.1:8094/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&src_url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);

  EXPECT_EQ(kFailOk, info.error_code());
  EXPECT_EQ(1, proxy_server.num_processed_requests());
  EXPECT_EQ("http://127.0.0.1:8095", info.proxy());
}


TEST_F(T_Download, ProxyDirectUsedOnlyAfterProxyFails) {
  // Same chain, but the proxy is down: the DIRECT tier must rescue the
  // request rather than leaving the client stuck.
  string src_path = GetSmallFile();
  MockFileServer file_server(8096, sandbox_path_);

  // Nothing listens on 8097.
  download_mgr.SetProxyChain("http://127.0.0.1:8097;DIRECT", "",
                             DownloadManager::kSetProxyBoth);

  string src_url = "http://127.0.0.1:8096/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&src_url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);

  EXPECT_EQ(kFailOk, info.error_code());
  EXPECT_EQ("DIRECT", info.proxy());
  EXPECT_EQ(1, file_server.num_processed_requests());
}


TEST_F(T_Download, ProxyWithoutDirectNeverLeaks) {
  // No DIRECT anywhere in the chain: a dead proxy must fail the request, not
  // silently bypass the proxy.
  string src_path = GetSmallFile();
  MockFileServer file_server(8098, sandbox_path_);

  // Nothing listens on 8099 and no DIRECT tier is offered.
  download_mgr.SetProxyChain("http://127.0.0.1:8099", "",
                             DownloadManager::kSetProxyBoth);

  string src_url = "http://127.0.0.1:8098/" + GetFileName(src_path);
  cvmfs::MemSink memsink;
  JobInfo info(&src_url, false /* compressed */, false /* probe hosts */, NULL,
               &memsink);
  download_mgr.Fetch(&info);

  EXPECT_NE(kFailOk, info.error_code());
  EXPECT_EQ(0, file_server.num_processed_requests());

  // Positive control: the origin was reachable all along.
  download_mgr.SetProxyChain("DIRECT", "", DownloadManager::kSetProxyBoth);
  memsink.Reset();
  JobInfo info_direct(&src_url, false /* compressed */,
                      false /* probe hosts */, NULL, &memsink);
  download_mgr.Fetch(&info_direct);
  EXPECT_EQ(kFailOk, info_direct.error_code());
  EXPECT_EQ(1, file_server.num_processed_requests());
}


namespace {
// Replies with a caller-chosen protocol token so the client's status-line
// parser can be exercised for versions other than HTTP/1.x.
std::string g_status_line_protocol = "HTTP/1.1";
int g_status_line_code = 200;

HTTPResponse StatusLineHandler(const HTTPRequest & /* req */, void *data) {
  int *hits = static_cast<int *>(data);
  if (hits)
    (*hits)++;
  HTTPResponse response;
  response.protocol = g_status_line_protocol;
  response.code = g_status_line_code;
  response.reason = (g_status_line_code == 200) ? "OK" : "Not Found";
  response.body = "payload";
  return response;
}
}  // namespace


TEST_F(T_Download, HttpStatusLineAnyVersion) {
  // An HTTP/2 status line carries a bare "2" as its version token, so the
  // status code does not sit at the fixed offset an "HTTP/1." reply puts it
  // at.  Missing it left http_code at -1 and silently skipped the error
  // handling, so a 404 looked like a successful transfer.
  int hits = 0;
  MockHTTPServer server(8102);
  ASSERT_TRUE(server.SetResponseCallback(StatusLineHandler, &hits));
  ASSERT_TRUE(server.Start());

  download_mgr.SetProxyChain("DIRECT", "", DownloadManager::kSetProxyBoth);
  const string url = "http://127.0.0.1:8102/any";

  // Baseline: HTTP/1.1 is parsed, as it always was.
  g_status_line_protocol = "HTTP/1.1";
  g_status_line_code = 404;
  {
    cvmfs::MemSink sink;
    JobInfo info(&url, false, false, NULL, &sink);
    download_mgr.Fetch(&info);
    EXPECT_EQ(404, info.http_code());
    EXPECT_NE(kFailOk, info.error_code());
  }

  // The regression: the same code announced over HTTP/2 must be seen too.
  g_status_line_protocol = "HTTP/2";
  g_status_line_code = 404;
  {
    cvmfs::MemSink sink;
    JobInfo info(&url, false, false, NULL, &sink);
    download_mgr.Fetch(&info);
    EXPECT_EQ(404, info.http_code());
    EXPECT_NE(kFailOk, info.error_code());
  }

  // And a successful HTTP/2 reply still has to be recognised as 2xx.
  g_status_line_protocol = "HTTP/2";
  g_status_line_code = 200;
  {
    cvmfs::MemSink sink;
    JobInfo info(&url, false, false, NULL, &sink);
    download_mgr.Fetch(&info);
    EXPECT_EQ(200, info.http_code());
    EXPECT_EQ(kFailOk, info.error_code());
  }
  server.Stop();
}


TEST_F(T_Download, CurlHandlePoolRespectsMaximum) {
  // Every idle handle can hold a keep-alive connection open, so the pool must
  // not grow past the configured maximum.
  const uint32_t max_handles = download_mgr.pool_max_handles_;
  std::vector<CURL *> handles;
  for (unsigned i = 0; i < max_handles + 4; ++i)
    handles.push_back(download_mgr.AcquireCurlHandle());
  for (unsigned i = 0; i < handles.size(); ++i)
    download_mgr.ReleaseCurlHandle(handles[i], true /* allow_reuse */);

  EXPECT_LE(download_mgr.pool_handles_idle_->size(),
            static_cast<size_t>(max_handles));
  EXPECT_TRUE(download_mgr.pool_handles_inuse_->empty());
}


TEST_F(T_Download, WatchFdsShrinkBackToFloor) {
  // After a burst of parallel transfers the poll array has to come back down.
  // The shrink guard used to test watch_fds_inuse_ against watch_fds_max_,
  // which is the wrong variable: the array only shrank while it was still
  // heavily used, so it stayed at its peak size once the burst was over.
  download_mgr.watch_fds_ = static_cast<struct pollfd *>(
      smalloc(2 * sizeof(struct pollfd)));
  download_mgr.watch_fds_size_ = 2;
  download_mgr.watch_fds_inuse_ = 2;  // the two control pipes
  download_mgr.watch_fds_[0].fd = -1;
  download_mgr.watch_fds_[1].fd = -2;

  const unsigned n = download_mgr.watch_fds_max_ * 2 + 1;
  for (unsigned i = 0; i < n; ++i) {
    DownloadManager::CallbackCurlSocket(NULL, static_cast<curl_socket_t>(100 + i),
                                        CURL_POLL_IN, &download_mgr, NULL);
  }
  EXPECT_GT(download_mgr.watch_fds_size_, download_mgr.watch_fds_max_);

  for (unsigned i = 0; i < n; ++i) {
    DownloadManager::CallbackCurlSocket(NULL, static_cast<curl_socket_t>(100 + i),
                                        CURL_POLL_REMOVE, &download_mgr, NULL);
  }
  EXPECT_EQ(2U, download_mgr.watch_fds_inuse_);
  EXPECT_LE(download_mgr.watch_fds_size_, download_mgr.watch_fds_max_);

  free(download_mgr.watch_fds_);
  download_mgr.watch_fds_ = NULL;
  download_mgr.watch_fds_size_ = 0;
  download_mgr.watch_fds_inuse_ = 0;
}


TEST_F(T_Download, TcpKeepaliveOnByDefault) {
  // Keep-alive probes are what keep an idle flow on the books of a conntrack
  // or DPI middlebox.  Without them the middlebox forgets the flow, the peer's
  // FIN/RST stops matching an established connection and gets filtered by a
  // RELATED,ESTABLISHED-only firewall, and the client is left holding a dead
  // socket that costs it a whole request timeout to discover.
  EXPECT_GT(download_mgr.opt_tcp_keepalive_, 0U);

  // An explicit setting still wins, including switching the probes off.
  download_mgr.SetTcpKeepalive(90);
  EXPECT_EQ(90U, download_mgr.opt_tcp_keepalive_);
  download_mgr.SetTcpKeepalive(0);
  EXPECT_EQ(0U, download_mgr.opt_tcp_keepalive_);

  // A clone inherits the setting rather than silently reverting to the default.
  download_mgr.SetTcpKeepalive(45);
  perf::Statistics stats;
  DownloadManager *clone = download_mgr.Clone(
      perf::StatisticsTemplate("clone", &stats), "clone");
  EXPECT_EQ(45U, clone->opt_tcp_keepalive_);
  delete clone;
}


TEST_F(T_Download, SocketTeardownIsBounded) {
  // Keep-alive only governs an idle connection.  Once anything is in flight --
  // a request, or the FIN that closes the connection -- the retransmit timer
  // takes over, bounded by net.ipv4.tcp_retries2, which defaults to about a
  // quarter of an hour.  A firewall that swallows the peer's ACK or RST
  // therefore leaves the socket retransmitting for minutes.  Every socket
  // libcurl opens must carry an explicit bound instead.
  CURL *handle = download_mgr.AcquireCurlHandle();
  ASSERT_TRUE(handle != NULL);

  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);

  // Defaults first: the kernel leaves TCP_USER_TIMEOUT unset (0 == use
  // tcp_retries2) and keeps the system-wide probe count.
  unsigned before_timeout = 1;
  socklen_t len = sizeof(before_timeout);
  ASSERT_EQ(0, getsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &before_timeout,
                          &len));
  EXPECT_EQ(0U, before_timeout);

  // The callback libcurl invokes for every connection socket.
  ASSERT_EQ(CURL_SOCKOPT_OK,
            download::CallbackCurlSockoptForTest(NULL, fd, CURLSOCKTYPE_IPCXN));

  unsigned user_timeout = 0;
  len = sizeof(user_timeout);
  ASSERT_EQ(0,
            getsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout, &len));
  EXPECT_GT(user_timeout, 0U);
  // Well under the ~15 minutes tcp_retries2 would otherwise allow.
  EXPECT_LE(user_timeout, 120000U);

  int keepcnt = 0;
  len = sizeof(keepcnt);
  ASSERT_EQ(0, getsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, &len));
  EXPECT_GT(keepcnt, 0);
  EXPECT_LE(keepcnt, 5);

  // Sockets that are not connection sockets must be left alone.
  const int fd2 = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd2, 0);
  ASSERT_EQ(CURL_SOCKOPT_OK, download::CallbackCurlSockoptForTest(
                                 NULL, fd2, CURLSOCKTYPE_ACCEPT));
  unsigned untouched = 1;
  len = sizeof(untouched);
  ASSERT_EQ(0,
            getsockopt(fd2, IPPROTO_TCP, TCP_USER_TIMEOUT, &untouched, &len));
  EXPECT_EQ(0U, untouched);

  close(fd);
  close(fd2);
  download_mgr.ReleaseCurlHandle(handle, false /* allow_reuse */);
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
      sscanf(i->second.c_str(), "bytes=%lu", &range_from);  // NOLINT
      st->range_first_byte = range_from;
    }
  }

  HTTPResponse response;
  if (st->requests <= st->cut_responses) {
    // Announce what is left but deliver only part of it, then the mock server
    // closes the connection: a classic short transfer.  Each cut reply still
    // carries enough bytes to count as progress.
    const size_t remaining = st->content.length() - range_from;
    const size_t chunk = remaining / 2;
    response.raw = true;
    response.body = string(has_range ? "HTTP/1.1 206 Partial Content"
                                     : "HTTP/1.1 200 OK")
                    + "\r\nContent-Length: " + StringifyUint(remaining)
                    + "\r\n\r\n" + st->content.substr(range_from, chunk);
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
  if (info.error_code() == kFailOk)
    EXPECT_EQ(st.content, written);
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


TEST_F(T_Download, ParallelFetchAndProxyMandatorySurviveClone) {
  // Settings the branch added must reach a cloned manager; the external
  // download manager is created as a clone, so a setting that does not
  // propagate silently applies to catalogs but not to external data.
  download_mgr.SetTcpKeepalive(45);
  download_mgr.SetParallelFetch(4);
  download_mgr.SetProxyChain("http://127.0.0.1:3128", "",
                             DownloadManager::kSetProxyBoth);
  EXPECT_TRUE(download_mgr.opt_proxy_mandatory_);

  perf::Statistics stats;
  DownloadManager *clone = download_mgr.Clone(
      perf::StatisticsTemplate("clone", &stats), "clone");
  EXPECT_EQ(45U, clone->opt_tcp_keepalive_);
  EXPECT_EQ(4U, clone->opt_parallel_fetch_);
  EXPECT_TRUE(clone->opt_proxy_mandatory_);

  // And a DIRECT-only chain must clone as "direct is allowed".
  download_mgr.SetProxyChain("DIRECT", "", DownloadManager::kSetProxyBoth);
  DownloadManager *clone2 = download_mgr.Clone(
      perf::StatisticsTemplate("clone2", &stats), "clone2");
  EXPECT_FALSE(clone2->opt_proxy_mandatory_);
  delete clone;
  delete clone2;
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
  const int fd_read = open(dest_path.c_str(), O_RDONLY);
  ASSERT_GE(fd_read, 0);
  string written;
  EXPECT_TRUE(SafeReadToString(fd_read, &written));
  close(fd_read);
  EXPECT_EQ(st.content, written);
  unlink(dest_path.c_str());
  server.Stop();
}


TEST_F(T_Download, NoProxyChainIsNotSelectable) {
  // The selection helpers must cope with a manager that has no usable proxy
  // rather than dereferencing an empty map or an empty group.
  download_mgr.SetProxyChain("", "", DownloadManager::kSetProxyBoth);
  EXPECT_TRUE(download_mgr.ChooseProxyUnlocked(NULL) == NULL);
  EXPECT_FALSE(download_mgr.opt_proxy_mandatory_);
  // Rebalancing an empty chain must be a no-op, not a crash.
  download_mgr.RebalanceProxies();
  download_mgr.SwitchProxyGroup();
  EXPECT_TRUE(download_mgr.ChooseProxyUnlocked(NULL) == NULL);

  // And once a chain is installed again, selection works.
  download_mgr.SetProxyChain("http://127.0.0.1:3128", "",
                             DownloadManager::kSetProxyBoth);
  EXPECT_TRUE(download_mgr.ChooseProxyUnlocked(NULL) != NULL);
}


namespace {
// Stands in for a saturated proxy: it answers, so it is plainly reachable,
// but the body is cut short.  That is a throughput symptom, reported as
// kFailProxyShortTransfer, not evidence of a dead peer.  Fewer than
// kMinResumeProgress bytes are delivered so no attempt counts as progress.
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
}  // namespace


TEST_F(T_Download, EscalatedProxyGroupClassification) {
  vector<vector<DownloadManager::ProxyInfo> > chain;
  unsigned current_group = 0;
  unsigned fallback_group = 0;

  // "proxy;DIRECT" plus a fallback: group 0 is local, group 1 is the DIRECT
  // tier, group 2 is off-site.  Only the first is not an escalation.
  download_mgr.SetProxyChain("http://127.0.0.1:3128;DIRECT",
                             "http://127.0.0.2:3128",
                             DownloadManager::kSetProxyBoth);
  download_mgr.GetProxyInfo(&chain, &current_group, &fallback_group);
  ASSERT_EQ(3U, chain.size());
  EXPECT_FALSE(download_mgr.IsEscalatedProxyGroup(0));
  EXPECT_TRUE(download_mgr.IsEscalatedProxyGroup(1));   // DIRECT tier
  EXPECT_TRUE(download_mgr.IsEscalatedProxyGroup(2));   // fallback proxy
  EXPECT_FALSE(download_mgr.IsEscalatedProxyGroup(99));  // out of range

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
