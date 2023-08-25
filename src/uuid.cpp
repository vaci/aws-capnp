// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "uuid.h"

#include <uuid/uuid.h>

namespace aws {

kj::String uuid() {
  uuid_t uuid;
  uuid_generate_random(uuid);
  auto txt = kj::heapArray<char>(37);
  uuid_unparse(uuid, txt.begin());
  return kj::String{kj::mv(txt)};
}

}
