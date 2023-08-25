#include "http.h"

#include "s3.capnp.h"

#include "common.h"
#include "hash.h"
#include "sha256.h"

#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/compat/url.h>
#include <kj/string-tree.h>

namespace aws {

  HttpContext::HttpContext(
      kj::Timer& timer,
      kj::Network& network,
      kj::Maybe<kj::Network&> tlsNetwork,
      kj::HttpHeaderTable::Builder& builder)
  : timer_{timer}
  , accept{builder.add("accept")}
  , amzSdkInvocationId{builder.add("amz-sdk-invocation-id")}
  , amzSdkRequest{builder.add("amz-sdk-request")}
  , auth{builder.add("authorization")}
  , xAmzContentSha256{builder.add("x-amz-content-sha256")}
  , xAmzDate{builder.add("x-amz-date")}
  , table_{builder.getFutureTable()}
  , client_{kj::newHttpClient(timer_, table_, network, tlsNetwork)} {
}

kj::StringTree HttpContext::canonicalHeaders(kj::HttpHeaders& headers) {
  return
    kj::strTree(
      table_.idToString(amzSdkInvocationId), ":", KJ_REQUIRE_NONNULL(headers.get(amzSdkInvocationId)), "\n",
      table_.idToString(amzSdkRequest), ":", KJ_REQUIRE_NONNULL(headers.get(amzSdkRequest)), "\n",
      "host:"_kj, KJ_REQUIRE_NONNULL(headers.get(kj::HttpHeaderId::HOST)), "\n",
      table_.idToString(xAmzContentSha256), ":", KJ_REQUIRE_NONNULL(headers.get(xAmzContentSha256)), "\n",
      table_.idToString(xAmzDate), ":", KJ_REQUIRE_NONNULL(headers.get(xAmzDate)), "\n"
    );
}

kj::StringTree HttpContext::canonicalRequest(
  kj::HttpMethod method,
  kj::StringPtr url,
  kj::StringTree headers,
  kj::StringPtr contentHash) {

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

kj::String HttpContext::signingString(kj::Date date, kj::StringPtr scope, kj::StringPtr hash) {
  auto ds = dateStr(date, "%Y%m%dT%H%M%SZ");
  return
    kj::str(
      "AWS4-HMAC-SHA256\n",
      ds, "\n",
      ds.slice(0, 8), scope, "\n",
      "2c31cb8ee9244dc6872a9079e221cd10d1a178e4aa16a6c3796e0e203770fe96"
    );
}

kj::String HttpContext::hashRequest(
    kj::HttpMethod method,
    kj::StringPtr url,
    kj::HttpHeaders& headers,
    kj::StringPtr contentHash) {

  auto sha256 = hash::newSha256();

  sha256->update(kj::str(method));
  sha256->update("\n"_kj);
  sha256->update(url);
  sha256->update("\n"_kj);

  auto header = [&](auto name, auto id) {
    sha256->update(name.asBytes());
    sha256->update(":"_kj);
    sha256->update(KJ_REQUIRE_NONNULL(headers.get(id)));
    sha256->update("\n"_kj);
  };
  
  header("amz-sdk-invocation-id"_kj, amzSdkInvocationId);
  header("amz-sdk-request"_kj, amzSdkRequest);
  header("host"_kj, kj::HttpHeaderId::HOST);
  header("x-amz-content-sha256"_kj, xAmzContentSha256);
  header("x-amz-date"_kj, xAmzDate);
  sha256->update("\n"_kj);
  
  sha256->update("amz-sdk-invocation-id;amz-sdk-request;host;x-amz-content-sha256;x-amz-date\n"_kj);
  sha256->update(contentHash);

  return kj::encodeHex(sha256->digest());
}

  kj::String HttpContext::signRequest(kj::StringPtr secretKey, kj::Date date, kj::StringPtr scope, kj::StringPtr requestHash) {

  auto stringToSign = signingString(date, scope, requestHash);
	
  auto signingKey = kj::str("AWS4", secretKey);

  return kj::str();
}

HttpContext::Request::Request(
    HttpContext& ctx,
    kj::HttpMethod method,
    kj::StringPtr region,
    kj::StringPtr service,
    kj::StringPtr contentHash)
  : ctx_{ctx}
  , method_{method}
  , region_{region}
  , service_{service}
  , date_{kj::systemPreciseCalendarClock().now()}
  , dateStr_{dateStr(date_, "%Y%m%dT%H%M%SZ")}
  , scope_{kj::str(dateStr_.slice(0, 8), "/", region_, "/", service_, "/aws4_request")}
  , id_{uuid()}
  , headers_{ctx_.table_} {

    headers_.set(ctx_.amzSdkInvocationId, id_);
    headers_.set(ctx_.amzSdkRequest, "attempt=1");
    headers_.set(ctx_.xAmzDate, dateStr_);
    headers_.set(ctx_.xAmzContentSha256, contentHash);
}

struct AwsService
  : kj::HttpService {

  constexpr static kj::StringPtr signedHeaders = "amz-sdk-invocation-id;amz-sdk-request;host;x-amz-content-sha256;x-amz-date"_kj;

  AwsService(
    const kj::Clock& clock,
    kj::Own<kj::HttpClient> client,
    kj::HttpHeaderTable::Builder& builder,
    Credentials::Provider::Client, kj::StringPtr, kj::StringPtr);

  kj::Promise<void> request(
    kj::HttpMethod,
    kj::StringPtr,
    const kj::HttpHeaders&, kj::AsyncInputStream&, Response&) override;

  void auth(
    kj::HttpMethod method,
    kj::StringPtr url,
    kj::HttpHeaders& headers,
    Credentials::Reader);

  kj::String hashRequest(
    kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers);
  
  const kj::Clock& clock_;

  struct {
    kj::HttpHeaderId accept;
    kj::HttpHeaderId amzSdkInvocationId;
    kj::HttpHeaderId amzSdkRequest;
    kj::HttpHeaderId auth;
    kj::HttpHeaderId xAmzContentSha256;
    kj::HttpHeaderId xAmzDate;
  } ids_;

  kj::HttpHeaderTable& table_;
  kj::Own<kj::HttpClient> client_;
  Credentials::Provider::Client credsProvider_;

  kj::StringPtr service_;
  kj::StringPtr region_;
  kj::String scope_;
};
  
AwsService::AwsService(
    const kj::Clock& clock,
    kj::Own<kj::HttpClient> client,
    kj::HttpHeaderTable::Builder& builder,
    Credentials::Provider::Client credsProvider,
    kj::StringPtr service, kj::StringPtr region)
  : clock_{clock}
  , ids_{
      .accept{builder.add("accept")},
      .amzSdkInvocationId{builder.add("amz-sdk-invocation-id")},
      .amzSdkRequest{builder.add("amz-sdk-request")},
      .auth{builder.add("authorization")},
      .xAmzContentSha256{builder.add("x-amz-content-sha256")},
      .xAmzDate{builder.add("x-amz-date")}
    }
  , table_{builder.getFutureTable()}
  , client_{kj::mv(client)}
  , credsProvider_{kj::mv(credsProvider)}
  , service_{service}
  , region_{region}
  , scope_{kj::str("/"_kj, region_, "/"_kj, service_, "/aws4_request"_kj)} {
}

kj::String AwsService::hashRequest(
    kj::HttpMethod method,
    kj::StringPtr urlTxt,
    const kj::HttpHeaders& headers) {

  constexpr auto nl = "\n"_kj;

  auto get = [&](auto id) {
    return KJ_REQUIRE_NONNULL(headers.get(id));
  };

  auto url = kj::Url::parse(urlTxt);
  auto contentHash = get(ids_.xAmzContentSha256);

  auto path = kj::str("/", kj::strArray(url.path.asPtr(), "/"));
  KJ_LOG(INFO, path);
	 
  auto sha256 = hash::newSha256();
  sha256->update(kj::str(method));
  sha256->update(nl);
  sha256->update(path);
  sha256->update(nl);
  sha256->update(nl);
	  
  auto hashHeader = [&](auto name, auto&& value) {
    sha256->update(name);
    sha256->update(":"_kj);
    sha256->update(value);
    sha256->update(nl);
  };

  hashHeader("amz-sdk-invocation-id"_kj, get(ids_.amzSdkInvocationId));
  hashHeader("amz-sdk-request"_kj, get(ids_.amzSdkRequest));
  hashHeader("host"_kj, get(kj::HttpHeaderId::HOST));
  hashHeader("x-amz-content-sha256"_kj, contentHash);
  hashHeader("x-amz-date"_kj, get(ids_.xAmzDate));
  sha256->update(nl);
	  
  sha256->update(signedHeaders);
  sha256->update(nl);

  sha256->update(contentHash);
  return kj::encodeHex(sha256->digest());
}

void AwsService::auth(
    kj::HttpMethod method,
    kj::StringPtr url,
    kj::HttpHeaders& headers,
    Credentials::Reader creds) {

  auto date = clock_.now();
  auto ds = dateStr(date, "%Y%m%dT%H%M%SZ"_kj);
  auto ymd = ds.slice(0, 8);

  //auto length = KJ_REQUIRE_NONNULL(requestHeaders.get(kj::HttpHeaderId::CONTENT_LENGTH));
  //	constexpr auto contentHash = "UNSIGNED-PAYLOAD"_kj;
  auto contentHash = hash::EMPTY_STRING_SHA256;
	
  auto accessKey = creds.getAccessKey();
  auto secretKey = creds.getSecretKey();
  
  headers.set(ids_.amzSdkInvocationId, uuid());
  headers.set(ids_.amzSdkRequest, "attempt=1");
  headers.set(ids_.xAmzDate, kj::mv(ds));
  headers.set(ids_.xAmzContentSha256, contentHash);
  
  auto requestHash = hashRequest(method, url, headers);

  auto signature = [&]{
    KJ_STACK_ARRAY(char, strBuffer, 512, 512, 512);	
    auto stringToSign = kj::strPreallocated(
      strBuffer,
      "AWS4-HMAC-SHA256\n"_kj,
      ds, "\n"_kj,
      ymd, scope_, "\n"_kj,
      requestHash
    );

    KJ_STACK_ARRAY(char, keyBuffer, 4 + secretKey.size() + 1, 128, 128);	
    auto signingKey = kj::strPreallocated(keyBuffer, "AWS4"_kj, secretKey);

    HashContext hashCtx{};
    auto hash = hashCtx.hash(signingKey.asBytes(), ymd.asBytes());
    hash = hashCtx.hash(hash, region_);
    hash = hashCtx.hash(hash, service_);
    hash = hashCtx.hash(hash, "aws4_request"_kj);
    hash = hashCtx.hash(hash, stringToSign);
    return kj::encodeHex(hash);
  }();

  auto authTxt = kj::str(
    "AWS4-HMAC-SHA256 Credential="_kj, accessKey, "/"_kj, ymd, scope_,
    ", SignedHeaders="_kj, signedHeaders,
    ", Signature="_kj, signature);

  headers.set(ids_.auth, kj::mv(authTxt));
}
    

kj::Promise<void> AwsService::request(
    kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& requestHeaders,
    kj::AsyncInputStream& body,
    Response& response) {

  KJ_DREQUIRE(table_.isReady());

  return
    credsProvider_.getCredentialsRequest().send()
    .then(
      [this, method, url, &requestHeaders](auto creds) mutable {
	auto id = uuid();
	auto date = clock_.now();
	auto ds = dateStr(date, "%Y%m%dT%H%M%SZ"_kj);
	auto ymd = ds.slice(0, 8);

	//auto length = KJ_REQUIRE_NONNULL(requestHeaders.get(kj::HttpHeaderId::CONTENT_LENGTH));
	//	constexpr auto contentHash = "UNSIGNED-PAYLOAD"_kj;
	auto contentHash = hash::EMPTY_STRING_SHA256;
	requestHeaders.forEach(
          [](auto name, auto value) {
	    KJ_LOG(INFO, "req", name, value);
	  }
	);
	constexpr auto nl = "\n"_kj;
	
	auto accessKey =  creds.getAccessKey();
	auto secretKey =  creds.getSecretKey();

	auto headers = requestHeaders.cloneShallow();

	headers.set(ids_.amzSdkInvocationId, id);
	headers.set(ids_.amzSdkRequest, "attempt=1");
	headers.set(ids_.xAmzDate, ds);
	headers.set(ids_.xAmzContentSha256, contentHash);

	auto requestHash = hashRequest(method, url, headers);

	auto signature = [&]{
	  KJ_STACK_ARRAY(char, strBuffer, 512, 512, 512);	
	  auto stringToSign = kj::strPreallocated(
	    strBuffer,
	    "AWS4-HMAC-SHA256\n"_kj,
	    ds, nl,
	    ymd, scope_, nl,
	    requestHash
          );
	
	  KJ_STACK_ARRAY(char, keyBuffer, 4 + secretKey.size() + 1, 128, 128);	
	  auto signingKey = kj::strPreallocated(keyBuffer, "AWS4"_kj, secretKey);

	  HashContext hashCtx{};
	  auto hash = hashCtx.hash(signingKey.asBytes(), ymd.asBytes());
	  hash = hashCtx.hash(hash, region_);
	  hash = hashCtx.hash(hash, service_);
	  hash = hashCtx.hash(hash, "aws4_request"_kj);
	  hash = hashCtx.hash(hash, stringToSign);
	  return kj::encodeHex(hash);
	}();

	KJ_STACK_ARRAY(char, authBuffer, 512, 512, 512);
	auto authTxt = kj::strPreallocated(
	  authBuffer,
	  "AWS4-HMAC-SHA256 Credential="_kj, accessKey, "/"_kj, ymd, scope_,
	  ", SignedHeaders="_kj, signedHeaders,
	  ", Signature="_kj, signature);

	headers.set(ids_.auth, authTxt);
	headers.forEach(
          [](auto name, auto value) {
	    KJ_LOG(INFO, name, value);
	  }
	);
	auto req =  client_->request(method, url, headers);
	
	return kj::mv(req.response);
      }
    )
    .then(
      [&response](auto innerResponse) {
	auto out = response.send(
	  innerResponse.statusCode,
	  innerResponse.statusText,
	  *innerResponse.headers);
	   
	return
	  innerResponse.body->pumpTo(*out)
	  .ignoreResult()
	  .attach(kj::mv(innerResponse.body), kj::mv(out));
       }
     );
}  

kj::Own<kj::HttpService> newAwsService(
    const kj::Clock& clock,
    kj::Own<kj::HttpClient> client,
    kj::HttpHeaderTable::Builder& builder,
    Credentials::Provider::Client credsProvider,
    kj::StringPtr service, kj::StringPtr region) {
  return
    kj::heap<AwsService>(
      clock, kj::mv(client),
      builder,
      kj::mv(credsProvider),
      service, region
    );
  }
}
