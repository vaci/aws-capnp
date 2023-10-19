// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/async-io.h>
#include <kj/compat/tls.h>
#include <kj/debug.h>
#include <kj/main.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#include <gtest/gtest.h>

namespace bp = boost::process;
namespace fs = boost::filesystem;

static int EKAM_TEST_DISABLE_INTERCEPTOR = 1;

struct MinioTest
  : testing::Test {
  
  MinioTest() {}
  ~MinioTest() noexcept {}

  void SetUp() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
    ::mkdtemp(dataPath_.begin());
    ::mkdtemp(configPath_.begin());
#pragma GCC diagnostic pop

    port_ = [&]{
      auto address = network_.parseAddress("0.0.0.0:0").wait(waitScope_);
      auto listener = address->listen();
      return listener->getPort();
    }();

    minio_ = bp::child(
      bp::search_path("minio"),
      "server", "--quiet",
      "--address", kj::str(':', port_).cStr(),
      dataPath_.cStr()
    );

    while (true) {
      timer_.afterDelay(kj::MILLISECONDS*100).wait(waitScope_);
      int err = bp::system(
	bp::search_path("mc"),
	"--config-dir", configPath_.cStr(),
	"alias", "set", "minio", kj::str("http://localhost:", port_).cStr(),
	"minioadmin", "minioadmin");
      if (err == 0) {
	break;
      }
    }
    KJ_REQUIRE(minio_.running());
  }

  void TearDown() {
    minio_.terminate();
    minio_.wait();
    fs::remove_all(configPath_.cStr());
    fs::remove_all(dataPath_.cStr());
  }

  int port_;
  kj::String configPath_{kj::str("/tmp/s3-minio-config.XXXXXX"_kj)};
  kj::String dataPath_{kj::str("/tmp/s3-minio-test.XXXXXX"_kj)};
  bp::child minio_;

  kj::TlsContext tlsCtx_{};
  kj::AsyncIoContext ioCtx_{kj::setupAsyncIo()};
  kj::WaitScope& waitScope_{ioCtx_.waitScope};
  kj::Timer& timer_{ioCtx_.provider->getTimer()};
  kj::Network& network_{ioCtx_.provider->getNetwork()};
};

int main(int argc, char* argv[]) {
  kj::TopLevelProcessContext processCtx{argv[0]};
  processCtx.increaseLoggingVerbosity();

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
