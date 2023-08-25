#include "creds.h"

#include <aws/core/auth/AWSCredentialsProviderChain.h>

namespace aws {

namespace {

struct CredentialsProviderServer
  : Credentials::Provider::Server {

  kj::Promise<void> getCredentials(GetCredentialsContext ctx) {
    auto awsCreds = credsProvider_->GetAWSCredentials();
    auto reply = ctx.getResults();
    reply.setAccessKey(awsCreds.GetAWSAccessKeyId());
    reply.setSecretKey(awsCreds.GetAWSSecretKey());
    reply.setSessionToken(awsCreds.GetSessionToken());
    return kj::READY_NOW;
  }

  std::shared_ptr<Aws::Auth::DefaultAWSCredentialsProviderChain> credsProvider_{
     std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>()
  };
  
};

}

Credentials::Provider::Client newCredentialsProvider() {
  return kj::heap<CredentialsProviderServer>();
}

}
