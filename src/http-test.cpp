// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "http.h"

#include "hash.h"
#include "sha256.h"

#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/compat/tls.h>
#include <kj/compat/url.h>
#include <kj/debug.h>
#include <kj/main.h>
#include <kj/string-tree.h>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>

#include <gtest/gtest.h>

using namespace aws;

static int EKAM_TEST_DISABLE_INTERCEPTOR = 1;

struct HttpTest
  : testing::Test {

  HttpTest() {
    Aws::InitAPI(awsOptions_);
    
  }

  ~HttpTest() noexcept {
    //Aws::ShutdownAPI(awsOptions_);
  }

  kj::TlsContext tlsCtx_{};
  kj::AsyncIoContext ioCtx_{kj::setupAsyncIo()};
  kj::Timer& timer_{ioCtx_.provider->getTimer()};
  kj::WaitScope& waitScope_{ioCtx_.waitScope};
  kj::Network& network_{ioCtx_.provider->getNetwork()};
  kj::Own<kj::Network> tlsNetwork_{tlsCtx_.wrapNetwork(network_)};
  Aws::SDKOptions awsOptions_;
};


TEST_F(HttpTest, Basic2) {
}

int main(int argc, char* argv[]) {
  kj::TopLevelProcessContext processCtx{argv[0]};
  processCtx.increaseLoggingVerbosity();

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
