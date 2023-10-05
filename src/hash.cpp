// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "hash.h"

#include <kj/debug.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace aws {

HashContext::HashContext() {
  mac_ = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
}

HashContext::~HashContext() {
  EVP_MAC_free(reinterpret_cast<EVP_MAC*>(mac_));
}

kj::Array<unsigned char> HashContext::hash(
  kj::ArrayPtr<const unsigned char> key,
  kj::ArrayPtr<const unsigned char> data) {

  KJ_STACK_ARRAY(unsigned char, digest, EVP_MAX_MD_SIZE, 256, 256);
  auto digestSize = digest.size();
  auto* macCtx = EVP_MAC_CTX_new(reinterpret_cast<EVP_MAC*>(mac_));

  OSSL_PARAM params[2];
  params[0] = OSSL_PARAM_construct_utf8_string("digest", (char*)"SHA256", 0);
  params[1] = OSSL_PARAM_construct_end();

  EVP_MAC_init(macCtx, key.begin(), key.size(), params);
  EVP_MAC_update(macCtx, data.begin(), data.size());
  EVP_MAC_final(macCtx, digest.begin(), &digestSize, digest.size());
  EVP_MAC_CTX_free(macCtx);

  return kj::heapArray(digest.begin(), digestSize); 
}

}

/*
  AWS sha256 HMAC

#include <aws/core/utils/crypto/Sha256HMAC.h>
#include <aws/core/utils/Outcome.h>

  auto hmac = Aws::MakeUnique<Aws::Utils::Crypto::Sha256HMAC>("");
  auto hashResult = hmac->Calculate(
    {data.begin(), data.size()},
    {key.begin(), key.size()}
  );

  KJ_REQUIRE(hashResult.IsSuccess());
  auto result = hashResult.GetResult();
  return kj::heapArray(&result[0], result.GetLength());

*/
