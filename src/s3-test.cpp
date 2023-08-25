// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "creds.h"
#include "hash.h"
#include "http.h"
#include "s3.h"
#include "sha256.h"
#include "uuid.h"

#include <capnp/message.h>

#include "capnp/compat/byte-stream.h"

#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/compat/tls.h>
#include <kj/compat/url.h>
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/filesystem.h>
#include <kj/main.h>
#include <kj/string-tree.h>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/s3/S3Client.h>
#include <aws/testing/AwsTestHelpers.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <gtest/gtest.h>

#include <chrono>

using namespace aws;

static int EKAM_TEST_DISABLE_INTERCEPTOR = 1;

static const char ALLOC_TAG[] = "AwsAuthV4SignerTest";
static const char UNSIGNED_PAYLOAD[] = "UNSIGNED-PAYLOAD";
static const char X_AMZ_SIGNATURE[] = "X-Amz-Signature";


kj::String dateStr(kj::Date date, kj::StringPtr format) {
  std::tm tm{
    .tm_sec =  static_cast<int>((date - kj::UNIX_EPOCH)/kj::SECONDS),
    .tm_mday = 1,
    .tm_year = (1970 - 1900)
  };
  std::mktime(&tm);

  std::array<char, 256> txt;
  auto c = strftime(txt.begin(), txt.size(), format.begin(), &tm);
  KJ_DREQUIRE(c != 0);
  return kj::heapString(txt.begin(), c);
}

kj::String yyyymmdd(kj::Date date) {
  std::tm tm{
    .tm_sec =  static_cast<int>((date - kj::UNIX_EPOCH)/kj::SECONDS),
    .tm_mday = 1,
    .tm_year = (1970 - 1900)
  };
  std::mktime(&tm);

  std::array<char, 9> txt;
  auto c = strftime(txt.begin(), txt.size(), "%Y%m%d", &tm);
  KJ_DREQUIRE(c != 0);
  return kj::heapString(txt.begin(), c);
}

kj::Date toDate(std::tm& tm) {
  auto t = std::mktime(&tm);
  return kj::UNIX_EPOCH + (t * kj::SECONDS);
}

auto canonicalRequest(kj::HttpMethod method, kj::StringPtr url, kj::StringTree headers, kj::StringPtr contentHash) {
  return kj::strTree(
    method, "\n",
    url, "\n",
    "\n",
    kj::mv(headers),
    "\n",
    "amz-sdk-invocation-id;amz-sdk-request;host;x-amz-content-sha256;x-amz-date\n",
    contentHash
  );
}

auto credential(kj::Date date, kj::StringPtr region, kj::StringPtr service) {
  return
    kj::strTree(
      yyyymmdd(date), "/", region, "/", service, "/aws4_request"
    );
}

auto authHeader(kj::StringTree creds, kj::StringPtr signature) {
  return
    kj::strTree(
      "AWS4-HMAC-SHA256", " ",
      "Credential=", kj::mv(creds), ", ",
      "SignedHeaders=host;x-amz-content-sha256;x-amz-date, ",
      "Signature=", signature
    );
}

auto signingString(kj::Date date, kj::StringTree creds, kj::StringPtr requestHash) {
  return kj::strTree(
    "AWS4-HMAC-SHA256", "\n",
    dateStr(date, "%Y%m%dT%H%M%SZ"), "\n",
    kj::mv(creds), "\n",
    requestHash
  );
}

Aws::Http::HeaderValueCollection convert(kj::HttpHeaders headers) {
  Aws::Http::HeaderValueCollection result;
  headers.forEach([&](auto name, auto value) {
    result.insert(std::make_pair(name, value));
  });
  return result;
}

auto signRequst(
  kj::Date date,		
  kj::HttpMethod method,
  kj::StringPtr url,
  const kj::HttpHeaders& headers) {
  
}

struct S3Test
  : testing::Test {
  
  S3Test() {
    credsProvider_ = std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>();
    
  }

  ~S3Test() noexcept {
  }

  kj::TlsContext tlsCtx_{};
  kj::AsyncIoContext ioCtx_{kj::setupAsyncIo()};
  kj::WaitScope& waitScope_{ioCtx_.waitScope};
  kj::Network& network_{ioCtx_.provider->getNetwork()};
  kj::Timer& timer_{ioCtx_.provider->getTimer()};
  kj::Own<kj::Network> tlsNetwork_{tlsCtx_.wrapNetwork(network_)};

  Aws::Client::ClientConfiguration awsConfig_{};
  std::shared_ptr<Aws::Auth::DefaultAWSCredentialsProviderChain> credsProvider_;
};

TEST_F(S3Test, Sign) {
  std:tm tm {
    .tm_sec = 30,
    .tm_min = 37,
    .tm_hour = 13,
    .tm_mday = 30,
    .tm_mon = 6,
    .tm_year = (2023-1900)
  };
  auto date = toDate(tm);
  auto awsCreds = credsProvider_->GetAWSCredentials();
  auto accessKey = awsCreds.GetAWSAccessKeyId();
  auto secretKey = kj::str(awsCreds.GetAWSSecretKey());

  auto region = "us-east-1"_kj;
  auto service = "s3"_kj;
  auto host = kj::str(service, ".", region, ".amazonaws.com");

  //auto id = uuid();
  auto id = "CC978435-7447-4D01-A431-649E43C5E75B"_kj;

  kj::HttpHeaderTable::Builder builder;
  HttpContext ctx{timer_, network_, nullptr, builder};
  auto table = builder.build();

  kj::HttpHeaders headers{ctx.table_};
  headers.set(ctx.amzSdkInvocationId, id);
  headers.set(ctx.amzSdkRequest, "attempt=1");
  headers.set(kj::HttpHeaderId::HOST, host);
  headers.set(ctx.xAmzDate, dateStr(date, "%Y%m%dT%H%M%SZ"));
  headers.set(ctx.xAmzContentSha256, hash::EMPTY_STRING_SHA256);

  
  auto stringToSign = [&]{
    auto canonHeaders = ctx.canonicalHeaders(headers);
    auto request = canonicalRequest(kj::HttpMethod::GET, "/", kj::mv(canonHeaders), hash::EMPTY_STRING_SHA256);
    KJ_LOG(INFO, request);
    auto requestHash = hash::sha256(request);
    return signingString(date, credential(date, region, service), kj::encodeHex(requestHash));
  }();

  EXPECT_EQ(
    stringToSign.flatten(),
    kj::str(
      "AWS4-HMAC-SHA256\n",
      "20230730T133730Z\n",
      "20230730/us-east-1/s3/aws4_request\n",
      "2c31cb8ee9244dc6872a9079e221cd10d1a178e4aa16a6c3796e0e203770fe96"
    )
  );

  HashContext hashCtx{};

  auto signingKey = kj::str("AWS4", secretKey);

  auto signature = [&]{
    auto hash = hashCtx.hash(signingKey.asBytes(), yyyymmdd(date).asBytes());
    hash = hashCtx.hash(hash, region.asBytes());
    hash = hashCtx.hash(hash, service.asBytes());
    hash = hashCtx.hash(hash, "aws4_request"_kj.asBytes());
    hash = hashCtx.hash(hash, stringToSign.flatten().asBytes());
    return kj::encodeHex(hash);
  }();
  KJ_LOG(INFO, signature);

  KJ_REQUIRE(signature == "04eb404f4ac8dca2b6ba4cc7fb82f9b4318c5e2b3b2aaa114d9e9f8e69674f3b"_kj);
}

struct ListCallbackServer
  : Callback<capnp::Text>::Server {

  kj::Promise<void> next(NextContext ctx) override {
    auto params = ctx.getParams();
    auto name = params.getValue();
    KJ_LOG(INFO, "bucket", name);
    return kj::READY_NOW;
  }

  kj::Promise<void> end(EndContext ctx) override {
    return kj::READY_NOW;
  }
};

TEST_F(S3Test, ListBuckets) {
  auto service = "s3"_kj;
  auto region = "eu-west-1"_kj;
  auto creds = ::aws::newCredentialsProvider();
  
  kj::HttpHeaderTable::Builder builder;

  auto client = kj::newHttpClient(timer_, builder.getFutureTable(), network_, *tlsNetwork_);
  auto awsService = newAwsService(
    kj::systemPreciseCalendarClock(),
    kj::mv(client),
    builder,
    creds,
    "s3"_kj, "eu-west-1"_kj
  );
  auto awsClient = kj::newHttpClient(*awsService);
  auto s3 = newS3(builder, kj::mv(awsClient), "eu-west-1"_kj);
  auto headerTable = builder.build();

  auto req = s3.listRequest();
  req.setCallback(kj::heap<ListCallbackServer>());
  req.send().wait(waitScope_);
}

TEST_F(S3Test, GetObject) {
  auto service = "s3"_kj;
  auto region = "eu-west-1"_kj;
  auto creds = ::aws::newCredentialsProvider();
  
  kj::HttpHeaderTable::Builder builder;

  auto client = kj::newHttpClient(timer_, builder.getFutureTable(), network_, *tlsNetwork_);
  auto awsService = newAwsService(
    kj::systemPreciseCalendarClock(),
    kj::mv(client),
    builder,
    creds,
    "s3"_kj, "eu-west-1"_kj
  );
  auto awsClient = kj::newHttpClient(*awsService);
  auto s3 = newS3(builder, kj::mv(awsClient), region);
  auto headerTable = builder.build();

  auto bucket = [&]{
    auto req = s3.getBucketRequest();
    req.setName("vaci-bf"_kj);
    return req.send().getBucket();
  }();
  
  auto obj = [&]{
    auto req = bucket.getObjectRequest();
    req.setKey("hello.txt"_kj);
    return req.send().getObject();
  }();

  {
    auto req = obj.headRequest();
    auto reply = req.send().wait(waitScope_);
    KJ_LOG(INFO, reply);
  }

  {
    auto pipe = kj::newOneWayPipe();
    capnp::ByteStreamFactory factory;
    auto out = factory.kjToCapnp(kj::mv(pipe.out));
    auto req = obj.readRequest();
    req.setStream(kj::mv(out));
    auto reply = req.send().wait(waitScope_);
    KJ_LOG(INFO, reply);

    auto txt = pipe.in->readAllText().wait(waitScope_);
    EXPECT_EQ(txt.size(), reply.getLength());
  }
}

TEST_F(S3Test, BasicHttp) {
  
  auto date = kj::systemPreciseCalendarClock().now();
  auto txt = dateStr(date, "%Y%m%dT%H%M%SZ");
  
  auto awsCreds = credsProvider_->GetAWSCredentials();
  auto accessKey = awsCreds.GetAWSAccessKeyId();
  auto secretKey = awsCreds.GetAWSSecretKey();
  auto sessionToken = awsCreds.GetSessionToken();
  
  KJ_LOG(INFO, "creds", accessKey, sessionToken);

  // Build a header table with the headers we are interested in.
  kj::HttpHeaderTable::Builder builder;
  auto accept = builder.add("Accept");
  auto auth = builder.add(Aws::Http::AUTHORIZATION_HEADER);
  auto awsSecurityToken = builder.add(Aws::Http::AWS_SECURITY_TOKEN);
  auto invocationId = builder.add(Aws::Http::SDK_INVOCATION_ID_HEADER);
  auto awsDate = builder.add(Aws::Http::AWS_DATE_HEADER);
  auto host = builder.add(Aws::Http::HOST_HEADER);
  auto contentSha = builder.add("x-amz-content-sha256");

  // AKIAIOSFODNN7EXAMPLE/YYYYMMDD/region/service/aws4_request
  auto creds = kj::str(accessKey, "/", "eu-west-2", "/", "s3", "/", "aws4_request");
  auto algo  = "AWS4-HMAC-SHA256";
  auto signedHeaders = "amz-sdk-invocation-id;amz-sdk-request;host;x-amz-content-sha256;x-amz-date";

  auto table = builder.build();
  auto client = kj::newHttpClient(timer_, *table, network_, *tlsNetwork_);
  auto signature = hash::sha256("foo"_kj.asBytes());

  // Get http:example.com.
  kj::HttpHeaders headers{*table};
  headers.set(accept, "*/*");
  headers.set(auth, kj::str("AWS4-HMAC-SHA256 Credential=", accessKey, "/", yyyymmdd(date), "/", "eu-west-2", "/", "s3", "/aws4_request, SignedHeaders=host;x-amz-date, Signature=", signature));
  headers.set(awsDate, dateStr(date, "%Y%m%dT%H%M%SZ"));
  headers.set(host, "");
  headers.set(invocationId, uuid());
 

  headers.forEach([](auto name, auto value) {
    KJ_LOG(INFO, "request header", name, value);
  });
  
  auto req = client->request(kj::HttpMethod::GET, "https://s3.eu-west-1.amazonaws.com", headers);
  auto reply =  req.response.wait(waitScope_);

  KJ_LOG(INFO, reply.statusCode, reply.statusText);
  reply.headers->forEach([](auto name, auto value) {
    KJ_LOG(INFO, "response", name, value);
  });
}

int main(int argc, char* argv[]) {
  kj::TopLevelProcessContext processCtx{argv[0]};
  processCtx.increaseLoggingVerbosity();

  Aws::SDKOptions awsOptions;
  awsOptions.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Trace;
  Aws::InitAPI(awsOptions);
  KJ_DEFER(Aws::ShutdownAPI(awsOptions));

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}


