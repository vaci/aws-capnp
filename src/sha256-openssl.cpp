#include "sha256.h"

#include <kj/array.h>
#include <kj/common.h>
#include <kj/debug.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>


namespace hash {

  kj::Array<uint8_t> sha256(kj::ArrayPtr<const unsigned char> bytes) {
  KJ_STACK_ARRAY(unsigned char, digest, EVP_MAX_MD_SIZE, 256, 256);
  unsigned int size = digest.size();
  auto ctx = EVP_MD_CTX_new();
  KJ_DEFER(EVP_MD_CTX_free(ctx));
  KJ_REQUIRE(EVP_DigestInit_ex2(ctx, EVP_sha256(), nullptr) != 0);
  KJ_REQUIRE(EVP_DigestUpdate(ctx, bytes.begin(), bytes.size()) != 0);
  KJ_REQUIRE(EVP_DigestFinal_ex(ctx, digest.begin(), &size) != 0);
  KJ_DREQUIRE(size == 32);
  return kj::heapArray(digest.begin(), size);
}
  
kj::Array<uint8_t> sha256(kj::StringTree& tree) {
  KJ_STACK_ARRAY(unsigned char, digest, EVP_MAX_MD_SIZE, 128, 128);
  unsigned int size = digest.size();
  auto ctx = EVP_MD_CTX_new();
  KJ_DEFER(EVP_MD_CTX_free(ctx));
  KJ_REQUIRE(EVP_DigestInit_ex2(ctx, EVP_sha256(), nullptr) != 0);
  tree.visit(
    [&](auto str) {
      auto bytes = str.asBytes();
      KJ_REQUIRE(EVP_DigestUpdate(ctx, bytes.begin(), bytes.size()) != 0);
    }
  );
  KJ_REQUIRE(EVP_DigestFinal_ex(ctx, digest.begin(), &size) != 0);
  KJ_DREQUIRE(size == 32);
  return kj::heapArray(digest.begin(), size);
}
  
namespace {

struct Sha256Impl
  : Sha256 {
  Sha256Impl();
  ~Sha256Impl();
  
  void update(kj::ArrayPtr<const uint8_t>) override;
  kj::Array<uint8_t> digest() override;

  EVP_MD_CTX* ctx_{EVP_MD_CTX_new()};
};

Sha256Impl::Sha256Impl() {
  EVP_DigestInit_ex2(ctx_, EVP_sha256(), nullptr);
}

Sha256Impl::~Sha256Impl() {
  EVP_MD_CTX_free(ctx_);
}

void Sha256Impl::update(kj::ArrayPtr<const uint8_t> data) {
  KJ_REQUIRE(EVP_DigestUpdate(ctx_, data.begin(), data.size()));
}

kj::Array<uint8_t> Sha256Impl::digest() {
  KJ_STACK_ARRAY(unsigned char, digest, EVP_MAX_MD_SIZE, 128, 128);
  unsigned int size = digest.size();
  KJ_REQUIRE(EVP_DigestFinal_ex(ctx_, digest.begin(), &size) != 0);
  KJ_DREQUIRE(size == 32);
  return kj::heapArray(digest.begin(), size);
}

}

kj::Own<Sha256> newSha256() {
  return kj::heap<Sha256Impl>();
}

}
