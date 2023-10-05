#pragma once

// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/common.h>
#include <kj/string.h>
#include <kj/string-tree.h>

namespace aws {

  struct HashContext {
    HashContext();
    ~HashContext();

    kj::Array<unsigned char> hash(
        kj::ArrayPtr<const unsigned char> key,
        kj::ArrayPtr<const unsigned char> data);

    kj::Array<unsigned char> hash(
        kj::ArrayPtr<const unsigned char> key,
        kj::StringPtr txt) {
      return hash(key, txt.asBytes());
    }

  private:
    void* mac_;
  };
}
