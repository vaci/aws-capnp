// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "uuid.h"

#include <uuid/uuid.h>

namespace {

inline auto KJ_STRINGIFY(uuid_t uuid) {
  kj::FixedArray<char, 36> txt;
  uuid_unparse(uuid, txt.begin());
  return kj::mv(txt);
}

}

namespace aws::uuid {

kj::String random() {
  uuid_t uuid;
  uuid_generate_random(uuid);
  return kj::str(uuid);
}

}
