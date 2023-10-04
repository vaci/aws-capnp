#pragma once

// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "s3.capnp.h"

#include <kj/compat/http.h>
#include <kj/map.h>

namespace aws {
  kj::Own<kj::HttpService> newAwsService(
      const kj::Clock&,
      kj::HttpService&,
      kj::HttpHeaderTable::Builder&,
      Credentials::Provider::Client,
      kj::StringPtr service, kj::StringPtr region
  );
}

