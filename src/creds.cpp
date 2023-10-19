#include "creds.h"

#include <kj/common.h>
#include <kj/debug.h>

#include <aws/core/auth/AWSCredentialsProviderChain.h>

#include <stdlib.h>

namespace aws {

namespace {

kj::Maybe<kj::StringPtr> getenv(kj::StringPtr name) {
  auto value = ::getenv(name.cStr());
  if (value) {
    return kj::StringPtr{value};
  }
  else {
    return {};
  }
}

struct CredentialsProviderServer
  : Credentials::Provider::Server {

  kj::Promise<void> getCredentials(GetCredentialsContext ctx) {
    auto reply = ctx.getResults();
    KJ_IF_MAYBE(value, getenv("AWS_ACCESS_KEY_ID"_kj)) {
      reply.setAccessKey(*value);
    }
    KJ_IF_MAYBE(value, getenv("AWS_SECRET_ACCESS_KEY"_kj)) {
      reply.setSecretKey(*value);
    }
    KJ_IF_MAYBE(value, getenv("AWS_SESSION_TOKEN"_kj)) {
      reply.setSessionToken(*value);
    }
    KJ_LOG(INFO, reply);
    return kj::READY_NOW;
  }
};

}

Credentials::Provider::Client newCredentialsProvider() {
  return kj::heap<CredentialsProviderServer>();
}

}
