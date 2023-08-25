// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sha256.h"
#include "hash.h"
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/main.h>

#include <gtest/gtest.h>

using namespace hash;


TEST(Sha256, EmptyString) {
  auto hash = newSha256();

  auto data = kj::heapArray<uint8_t>(0);
  hash->update(data);
  auto digest = hash->digest();
  auto x = kj::encodeHex(digest);
  KJ_LOG(INFO, x);
  EXPECT_STREQ(x.cStr(), hash::EMPTY_STRING_SHA256.cStr());
}

int main(int argc, char* argv[]) {
  kj::TopLevelProcessContext processCtx{argv[0]};
  processCtx.increaseLoggingVerbosity();

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
