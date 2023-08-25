#pragma once

// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "s3.capnp.h"

#include <kj/compat/http.h>
#include <kj/map.h>

namespace aws {

  struct HttpContext {

    HttpContext(
      kj::Timer& timer,
      kj::Network& network,
      kj::Maybe<kj::Network&> tlsNetwork,
      kj::HttpHeaderTable::Builder&);

    kj::Timer& timer_;
    kj::HttpHeaderId accept;
    kj::HttpHeaderId amzSdkInvocationId;
    kj::HttpHeaderId amzSdkRequest;
    kj::HttpHeaderId auth;
    kj::HttpHeaderId xAmzContentSha256;
    kj::HttpHeaderId xAmzDate;

    kj::HttpHeaderTable& table_;
    kj::Own<kj::HttpClient> client_;

    kj::StringTree canonicalHeaders(kj::HttpHeaders& headers);
    kj::StringTree canonicalRequest(kj::HttpMethod method, kj::StringPtr url, kj::StringTree headers, kj::StringPtr contentHash);

    kj::String hashRequest(kj::HttpMethod method, kj::StringPtr url, kj::HttpHeaders& headers, kj::StringPtr contentHash);

    kj::String signingString(kj::Date date, kj::StringPtr scope, kj::StringPtr hash); 

    kj::String signRequest(kj::StringPtr secretKey, kj::Date date, kj::StringPtr scope, kj::StringPtr requestHash);

    struct Request {

      Request(HttpContext&, kj::HttpMethod method, kj::StringPtr region, kj::StringPtr service, kj::StringPtr contentHash);
    private:
      
      HttpContext& ctx_;
      kj::HttpMethod method_;
      kj::StringPtr region_;
      kj::StringPtr service_;
      kj::Date date_;
      kj::String dateStr_;
      kj::String scope_;
      kj::String id_;
      kj::HttpHeaders headers_;
    };
  };

  kj::Own<kj::HttpService> newAwsService(
      const kj::Clock&,
      kj::Own<kj::HttpClient>,
      kj::HttpHeaderTable::Builder&,
      Credentials::Provider::Client,
      kj::StringPtr service, kj::StringPtr region
  );
}

