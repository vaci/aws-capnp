#include "http.h"

#include "s3.capnp.h"

#include "common.h"
#include "hash.h"
#include "sha256.h"
#include "uuid.h"

#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/compat/url.h>

namespace aws {

struct AwsService
  : kj::HttpService {

  constexpr static kj::StringPtr signedHeaders =
    "amz-sdk-invocation-id;"
    "amz-sdk-request;host;"
    "x-amz-content-sha256;"
    "x-amz-date"_kj;

  AwsService(
    const kj::Clock& clock,
    kj::HttpService& proxy,
    kj::HttpHeaderTable::Builder& builder,
    Credentials::Provider::Client, kj::StringPtr, kj::StringPtr);

  kj::String hashRequest(
    kj::HttpMethod method,
    kj::StringPtr urlTxt,
    const kj::HttpHeaders& headers);

  kj::Promise<void> request(
    kj::HttpMethod,
    kj::StringPtr,
    const kj::HttpHeaders&, kj::AsyncInputStream&, Response&) override;

  const kj::Clock& clock_;

  struct {
    kj::HttpHeaderId accept;
    kj::HttpHeaderId amzSdkInvocationId;
    kj::HttpHeaderId amzSdkRequest;
    kj::HttpHeaderId auth;
    kj::HttpHeaderId xAmzContentSha256;
    kj::HttpHeaderId xAmzDate;
    kj::HttpHeaderId xAmzSecurityToken;
  } ids_;

  kj::HttpHeaderTable& table_;
  kj::HttpService& proxy_;
  Credentials::Provider::Client credsProvider_;

  kj::StringPtr service_;
  kj::StringPtr region_;
  kj::String scope_;
};
  
AwsService::AwsService(
    const kj::Clock& clock,
    kj::HttpService& proxy,
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
      .xAmzDate{builder.add("x-amz-date")},
      .xAmzSecurityToken{builder.add("X-Amz-Security-Token")}
    }
  , table_{builder.getFutureTable()}
  , proxy_{proxy}
  , credsProvider_{kj::mv(credsProvider)}
  , service_{service}
  , region_{region}
  , scope_{kj::str('/', region_, '/', service_, "/aws4_request"_kj)} {
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
      [this, method, url, &requestHeaders, &body, &response](auto creds) mutable {
	auto id = uuid();
	auto date = clock_.now();
	auto ds = dateStr(date, "%Y%m%dT%H%M%SZ"_kj);
	auto ymd = ds.slice(0, 8);
	auto contentHash = "UNSIGNED-PAYLOAD"_kj;

	KJ_IF_MAYBE(length, body.tryGetLength()) {
	  if (length == 0u) {
	    contentHash = hash::EMPTY_STRING_SHA256;
	  }
	}

	auto headers = requestHeaders.cloneShallow();	
	headers.set(ids_.amzSdkInvocationId, id);
	headers.set(ids_.amzSdkRequest, "attempt=1");
	headers.set(ids_.xAmzDate, ds);
	headers.set(ids_.xAmzContentSha256, contentHash);
	{
	  auto sessionToken = creds.getSessionToken();
	  if (sessionToken.size()) {
	    headers.set(ids_.xAmzSecurityToken, sessionToken);
	  }
	}

	auto signature = [&]{
	  auto requestHash = hashRequest(method, url, headers);

	  KJ_STACK_ARRAY(char, strBuffer, 512, 512, 512);	
	  auto stringToSign = kj::strPreallocated(
	    strBuffer,
	    "AWS4-HMAC-SHA256\n"_kj,
	    ds, '\n',
	    ymd, scope_, '\n',
	    requestHash
	  );

	  auto secretKey =  creds.getSecretKey();
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

	{
	  auto accessKey =  creds.getAccessKey();
	  auto authTxt = kj::str(
	    "AWS4-HMAC-SHA256 Credential="_kj, accessKey, '/', ymd, scope_,
	    ", SignedHeaders="_kj, signedHeaders,
	    ", Signature="_kj, signature);

	  headers.set(ids_.auth, kj::mv(authTxt));
	}
	return proxy_.request(method, url, headers, body, response);
     }
   );
}

kj::String AwsService::hashRequest(
    kj::HttpMethod method,
    kj::StringPtr urlTxt,
    const kj::HttpHeaders& headers) {

  auto get = [&](auto id) {
    return KJ_REQUIRE_NONNULL(headers.get(id));
  };

  auto url = kj::Url::parse(urlTxt);
  auto contentHash = get(ids_.xAmzContentSha256);

  auto path = kj::str("/", kj::strArray(url.path.asPtr(), "/"));
  KJ_LOG(INFO, path);
	 
  auto sha256 = hash::newSha256();
  sha256->update(kj::str(method));
  sha256->update("\n"_kj);
  sha256->update(path);
  sha256->update("\n"_kj);

  {
    auto query = KJ_MAP(param, url.query) {
      return kj::str(param.name, "=", param.value);
    };
    sha256->update(kj::strArray(query, "&"));
    sha256->update("\n"_kj);
  }
	  
  auto hashHeader = [&](auto name, auto&& value) {
    sha256->update(name);
    sha256->update(":"_kj);
    sha256->update(value);
    sha256->update("\n"_kj);
  };

  hashHeader("amz-sdk-invocation-id"_kj, get(ids_.amzSdkInvocationId));
  hashHeader("amz-sdk-request"_kj, get(ids_.amzSdkRequest));
  hashHeader("host"_kj, get(kj::HttpHeaderId::HOST));
  hashHeader("x-amz-content-sha256"_kj, contentHash);
  hashHeader("x-amz-date"_kj, get(ids_.xAmzDate));
  sha256->update("\n"_kj);
	  
  sha256->update(signedHeaders);
  sha256->update("\n"_kj);

  sha256->update(contentHash);
  return kj::encodeHex(sha256->digest());
}
 
kj::Own<kj::HttpService> newAwsService(
    const kj::Clock& clock,
    kj::HttpService& proxy,
    kj::HttpHeaderTable::Builder& builder,
    Credentials::Provider::Client credsProvider,
    kj::StringPtr service, kj::StringPtr region) {
  return
    kj::heap<AwsService>(
      clock, proxy,
      builder,
      kj::mv(credsProvider),
      service, region
    );
}

}
