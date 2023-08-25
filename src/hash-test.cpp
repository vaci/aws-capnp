#include "hash.h"
#include "sha256.h"

#include <aws/core/Aws.h>
#include <aws/core/auth/signer/AWSAuthSignerHelper.h>
#include <aws/core/utils/Array.h>

#include <gtest/gtest.h>

#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/main.h>

#include <openssl/evp.h>

using namespace aws;

static int EKAM_TEST_DISABLE_INTERCEPTOR = 1;

struct HashTest
  : testing::Test {
  
  HashTest() {
  }

  ~HashTest() noexcept {
  }
};

TEST_F(HashTest, EmptyStringHash) {
  auto hash = hash::sha256(kj::str("").asBytes());
  //EXPECT_EQ(hash, ::hash::EMPTY_STRING_SHA256);
}



TEST_F(HashTest, Sha256Impl) {
  auto sha256 = hash::newSha256();
  auto digest = kj::encodeHex(sha256->digest());
  KJ_LOG(INFO, digest);
  EXPECT_EQ(digest, hash::EMPTY_STRING_SHA256);
}

TEST_F(HashTest, Sha256HMAC) {

  auto key = "foobar"_kj.asBytes();
  auto data = "barfoo"_kj.asBytes();
  
  auto evpHmac = [&]{
    auto mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    KJ_DEFER(EVP_MAC_free(mac));

    KJ_STACK_ARRAY(unsigned char, digest, EVP_MAX_MD_SIZE, 256, 256);
    auto digestSize = digest.size();
    auto* macCtx = EVP_MAC_CTX_new(mac);
    KJ_DEFER(EVP_MAC_CTX_free(macCtx));

    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_utf8_string("digest", (char*)"SHA256", 0);
    params[1] = OSSL_PARAM_construct_end();

    EVP_MAC_init(macCtx, key.begin(), key.size(), params);
    EVP_MAC_update(macCtx, data.begin(), data.size());
    EVP_MAC_final(macCtx, digest.begin(), &digestSize, digest.size());

    return kj::heapArray(digest.begin(), digestSize);
  }();

  auto awsHmac = [&]{
    auto hmac = Aws::MakeUnique<Aws::Utils::Crypto::Sha256HMAC>("");
    auto hashResult = hmac->Calculate(
      {data.begin(), data.size()},
      {key.begin(), key.size()}
    );

    auto result = hashResult.GetResult();
    return kj::heapArray(&result[0], result.GetLength());
  }();;

  KJ_LOG(INFO, evpHmac, awsHmac);
  EXPECT_EQ(evpHmac, awsHmac);
}

TEST_F(HashTest, ComputeHash) {
  auto date = "20230728"_kj;
  auto signingKey = "AWS4foorbarEXAMPLE1234"_kj;

  auto region = "us-east-1"_kj;
  auto serviceName = "s3"_kj;

  KJ_LOG(INFO, signingKey, date);
  auto awsHash = [&]{
    auto hmac = Aws::MakeUnique<Aws::Utils::Crypto::Sha256HMAC>("");

    auto hashResult = hmac->Calculate(
      {date.asBytes().begin(), date.asBytes().size()},
      {signingKey.asBytes().begin(), signingKey.asBytes().size()});

    EXPECT_TRUE(hashResult.IsSuccess());
    auto kDate = hashResult.GetResult();
    hashResult = hmac->Calculate(Aws::Utils::ByteBuffer((unsigned char*)region.begin(), region.size()), kDate);
    EXPECT_TRUE(hashResult.IsSuccess());
 
    auto kRegion = hashResult.GetResult();
    hashResult = hmac->Calculate(Aws::Utils::ByteBuffer((unsigned char*)serviceName.begin(), serviceName.size()), kRegion);
    EXPECT_TRUE(hashResult.IsSuccess());
 
    auto kService = hashResult.GetResult();
    hashResult = hmac->Calculate(Aws::Utils::ByteBuffer((unsigned char*)Aws::Auth::AWSAuthHelper::AWS4_REQUEST, strlen(Aws::Auth::AWSAuthHelper::AWS4_REQUEST)), kService);
    EXPECT_TRUE(hashResult.IsSuccess());

    auto& result = hashResult.GetResult();
    return kj::heapArray(&result[0], result.GetLength());
  }();

  
  auto evpHash = [&]{
  
    HashContext hashCtx{};
    auto hash = hashCtx.hash(signingKey.asBytes(), date.asBytes());
    hash = hashCtx.hash(hash, region.asBytes());
    hash = hashCtx.hash(hash, serviceName.asBytes());
    hash = hashCtx.hash(hash, "aws4_request"_kj.asBytes());
    return hash;
  }();

  KJ_LOG(INFO, awsHash, evpHash);
  EXPECT_EQ(awsHash.size(), evpHash.size());
}

int main(int argc, char* argv[]) {
  kj::TopLevelProcessContext processCtx{argv[0]};
  processCtx.increaseLoggingVerbosity();

  Aws::SDKOptions awsOptions;
  //awsOptions.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Trace;

  Aws::InitAPI(awsOptions);
  KJ_DEFER(Aws::ShutdownAPI(awsOptions));

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}


