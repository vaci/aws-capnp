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

struct DefaultCredsServer
  : Credentials::Provider::Server {

  kj::Promise<void> getCredentials(GetCredentialsContext ctx) {
    auto reply = ctx.getResults();
    auto awsCreds = credsProvider_->GetAWSCredentials();
    reply.setAccessKey(awsCreds.GetAWSAccessKeyId());
    reply.setSecretKey(awsCreds.GetAWSSecretKey());
    reply.setSessionToken(awsCreds.GetSessionToken());
    return kj::READY_NOW;
  }

  std::shared_ptr<Aws::Auth::DefaultAWSCredentialsProviderChain> credsProvider_{
    std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>()
  };
};

Aws::Http::HeaderValueCollection convert(kj::HttpHeaders headers) {
  Aws::Http::HeaderValueCollection result;
  headers.forEach([&](auto name, auto value) {
    result.insert(std::make_pair(name, value));
  });
  return result;
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
  auto creds = kj::heap<DefaultCredsServer>();
  
  kj::HttpHeaderTable::Builder builder;

  auto client = kj::newHttpClient(timer_, builder.getFutureTable(), network_, *tlsNetwork_);
  auto s3 = newS3(kj::systemPreciseCalendarClock(), timer_, network_, *tlsNetwork_, builder, kj::mv(creds), "eu-west-1"_kj);
  auto headerTable = builder.build();

  auto req = s3.listBucketsRequest();
  auto reply = req.send().wait(waitScope_);
  KJ_LOG(INFO, reply);
}

TEST_F(S3Test, GetObject) {
  auto service = "s3"_kj;
  auto region = "eu-west-1"_kj;
  auto creds = kj::heap<DefaultCredsServer>();
  
  kj::HttpHeaderTable::Builder builder;

  auto s3 = newS3(kj::systemPreciseCalendarClock(), timer_, network_, *tlsNetwork_, builder, kj::mv(creds), region);
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
    KJ_LOG(INFO, txt);
  }
}

TEST_F(S3Test, PutObject) {
  auto service = "s3"_kj;
  auto region = "eu-west-1"_kj;
  auto creds = kj::heap<DefaultCredsServer>();

  kj::HttpHeaderTable::Builder builder;

  auto s3 = newS3(kj::systemPreciseCalendarClock(), timer_, network_, *tlsNetwork_, builder, kj::mv(creds), region);
  auto headerTable = builder.build();

  auto bucket = [&]{
    auto req = s3.getBucketRequest();
    req.setName("vaci-bf"_kj);
    return req.send().getBucket();
  }();

  auto obj = [&]{
    auto req = bucket.getObjectRequest();
    req.setKey(uuid());
    return req.send().getObject();
  }();

  {
    auto req = obj.headRequest();
    auto reply = req.send().wait(waitScope_);
    KJ_LOG(INFO, reply);
  }

  KJ_LOG(INFO, "WRITING...");
  {
    auto content = uuid();
    auto bytes = content.asBytes();
    auto req = obj.writeRequest();
    req.setLength(bytes.size());
    auto reply = req.send().wait(waitScope_);
    KJ_LOG(INFO, reply);

    auto stream = reply.getStream();
    {
      auto req = stream.writeRequest();
      req.setBytes(bytes);
      req.send().wait(waitScope_);
    }
    {
      auto req = stream.endRequest();
      req.send().wait(waitScope_);
    }
  }

  KJ_LOG(INFO, "READING...");
  {
    auto pipe = kj::newOneWayPipe();
    capnp::ByteStreamFactory factory;
    auto out = factory.kjToCapnp(kj::mv(pipe.out));
    auto req = obj.readRequest();
    req.setStream(kj::mv(out));
    auto reply = req.send().wait(waitScope_);
    KJ_LOG(INFO, reply);

    auto txt = pipe.in->readAllText().wait(waitScope_);
    KJ_LOG(INFO, txt);
  }

  {
    auto req = obj.deleteRequest();
    auto reply = req.send().wait(waitScope_);
    KJ_LOG(INFO, reply);
  }
}

TEST_F(S3Test, PutMultipartObject) {
  auto service = "s3"_kj;
  auto region = "eu-west-1"_kj;
  auto creds = kj::heap<DefaultCredsServer>();

  kj::HttpHeaderTable::Builder builder;

  auto s3 = newS3(kj::systemPreciseCalendarClock(), timer_, network_, *tlsNetwork_, builder, kj::mv(creds), region);
  auto headerTable = builder.build();

  auto bucket = [&]{
    auto req = s3.getBucketRequest();
    req.setName("vaci-bf"_kj);
    return req.send().getBucket();
  }();

  auto obj = [&]{
    auto req = bucket.getObjectRequest();
    req.setKey(uuid());
    return req.send().getObject();
  }();

  {
    auto content = uuid();
    auto bytes = content.asBytes();
    auto req = obj.multipartRequest();
    auto reply = req.send().wait(waitScope_);
    KJ_LOG(INFO, reply);

    auto stream = reply.getStream();
    {
      auto req = stream.writeRequest();
      req.setBytes(bytes);
      req.send().wait(waitScope_);
    }
    {
      auto req = stream.endRequest();
      req.send().wait(waitScope_);
    }
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
    KJ_LOG(INFO, txt);
  }

  {
    auto req = obj.deleteRequest();
    auto reply = req.send().wait(waitScope_);
    KJ_LOG(INFO, reply);
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
