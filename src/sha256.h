#pragma once

// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/array.h>
#include <kj/common.h>
#include <kj/string-tree.h>

#include <inttypes.h>

#include <functional>

namespace hash {

static const auto EMPTY_STRING_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"_kj;

struct Sha256 {
  virtual void update(kj::ArrayPtr<const uint8_t>) = 0;
  virtual kj::Array<uint8_t> digest() = 0;

  void update(const char* txt) {
    update(kj::StringPtr{txt}.asBytes());
  }

  void update(kj::StringPtr txt) {
    update(txt.asBytes());
  }
};

kj::Array<uint8_t> sha256(kj::ArrayPtr<const uint8_t>);
kj::Array<uint8_t> sha256(kj::StringTree&);

kj::Own<Sha256> newSha256();
}
