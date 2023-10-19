#include "s3-server.h"

// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/asio/ip/udp.hpp>

#include <kj/async-io.h>
#include <kj/compat/tls.h>
#include <kj/debug.h>
#include <kj/main.h>

#include <gtest/gtest.h>

using namespace aws;

namespace bp = boost::process;
namespace fs = boost::filesystem;

static int EKAM_TEST_DISABLE_INTERCEPTOR = 1;

namespace {

struct S3ServerTest
  : testing::Test {
  
  S3ServerTest() {}
  ~S3ServerTest() noexcept {}

  kj::TlsContext tlsCtx_{};
  kj::AsyncIoContext ioCtx_{kj::setupAsyncIo()};
  kj::WaitScope& waitScope_{ioCtx_.waitScope};
  kj::Network& network_{ioCtx_.provider->getNetwork()};
  kj::Timer& timer_{ioCtx_.provider->getTimer()};
  kj::Own<kj::Network> tlsNetwork_{tlsCtx_.wrapNetwork(network_)};
};

}

TEST_F(S3ServerTest, ListBuckets) {
  auto dir = kj::newInMemoryDirectory(kj::systemPreciseCalendarClock());
  auto s3 = newS3Server(dir->clone());

  auto req = s3.createBucketRequest();
  req.setName("foo/bar");
  auto bucket = req.send().getBucket();
  
  for (auto&& name: dir->listNames()) {
    KJ_LOG(INFO, name);
  }  

}
TEST_F(S3ServerTest, WriteObject) {
  auto dir = kj::newInMemoryDirectory(kj::systemPreciseCalendarClock());
  auto s3 = newS3Server(dir->clone());
}

int main(int argc, char* argv[]) {
  kj::TopLevelProcessContext processCtx{argv[0]};
  processCtx.increaseLoggingVerbosity();

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
