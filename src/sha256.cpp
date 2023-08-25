#include "sha256.h"

#include <kj/array.h>
#include <kj/common.h>
#include <kj/debug.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace hash {

namespace {

static constexpr uint32_t K[] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
  0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static constexpr uint32_t rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

static constexpr uint32_t choose(uint32_t e, uint32_t f, uint32_t g) {
   return (e & f) ^ (~e & g);
 }

static constexpr uint32_t majority(uint32_t a, uint32_t b, uint32_t c) {
   return (a & (b | c)) | (b & c);
 }
  
static constexpr uint32_t sig0(uint32_t x) {
  return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

static constexpr uint32_t sig1(uint32_t x) {
  return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

}

void Sha256x::update(kj::ArrayPtr<const uint8_t> data) {
  for (size_t ii = 0 ; ii < data.size() ; ++ii) {
    m_data[m_blocklen++] = data[ii];
    if (m_blocklen == 64) {
      transform();
      m_bitlen += 512;
      m_blocklen = 0;
    }
  }
}

void Sha256x::transform() {
  kj::FixedArray<uint32_t, 64> W;
  
  for (size_t ii = 0, jj = 0; ii < 16; ii++, jj += 4) {
    W[ii] =
      (m_data[jj + 0] << 24) |
      (m_data[jj + 1] << 16) |
      (m_data[jj + 2] <<  8) |
      (m_data[jj + 3]);
  }

  for (size_t ii = 16 ; ii < 64; ii++) {
    W[ii] =
      sig1(W[ii -  2]) + W[ii -  7] +
      sig0(W[ii - 15]) + W[ii - 16];
  }

  auto A = m_state[0];
  auto B = m_state[1];
  auto C = m_state[2];
  auto D = m_state[3];
  auto E = m_state[4];
  auto F = m_state[5];
  auto G = m_state[6];
  auto H = m_state[7];
  
  for (size_t ii = 0; ii < 64; ii++) {
    uint32_t maj = majority(A, B, C);

    uint32_t S0 = rotr(A, 2) ^ rotr(A, 13) ^ rotr(A, 22);
    uint32_t S1 = rotr(E, 6) ^ rotr(E, 11) ^ rotr(E, 25);

    uint32_t ch = choose(E, F, G);
 

    uint32_t sum = W[ii] + K[ii] + H + ch + S1;

    H = G;
    G = F;
    F = E;
    E = D + sum;
    D = C;
    C = B;
    B = A;
    A = S0 + maj + sum;
  }

  m_state[0] += A;
  m_state[1] += B;
  m_state[2] += C;
  m_state[3] += D;
  m_state[4] += E;
  m_state[5] += F;
  m_state[6] += G;
  m_state[7] += H;
}

  void Sha256x::pad() {

    auto ii = m_blocklen;
    auto end = m_blocklen < 56 ? 56 : 64;

    // add 1 bit
    m_data[ii++] = 0x80;

    // pad with zeroes
    while (ii < end) {
      m_data[ii++] = 0x00;
    }

    if (m_blocklen >= 56) {
      transform();
      memset(m_data.begin(), 0, 56);
    }

    m_bitlen += m_blocklen * 8;

    m_data[63] = m_bitlen;
    m_data[62] = m_bitlen >> 8;
    m_data[61] = m_bitlen >> 16;
    m_data[60] = m_bitlen >> 24;
    m_data[59] = m_bitlen >> 32;
    m_data[58] = m_bitlen >> 40;
    m_data[57] = m_bitlen >> 48;
    m_data[56] = m_bitlen >> 56;

    transform();
}

kj::Array<uint8_t> Sha256x::digest() {
  auto data = kj::heapArray<uint8_t>(32);
  pad();
  revert(data.begin());
  return data;
}

void Sha256x::revert(uint8_t * hash) {
  // SHA uses big endian byte ordering
  // Revert all bytes
  for (uint8_t i = 0 ; i < 4 ; i++) {
    for(uint8_t j = 0 ; j < 8 ; j++) {
      hash[i + (j * 4)] = (m_state[j] >> (24 - i * 8)) & 0x000000ff;
    }
  }
}

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

  EVP_MD_CTX* ctx_;
};

Sha256Impl::Sha256Impl()
  : ctx_{EVP_MD_CTX_new()} {
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
