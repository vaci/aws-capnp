#pragma once
// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "s3.capnp.h"

#include <kj/compat/http.h>

namespace aws {

aws::S3::Client newS3(
    kj::HttpHeaderTable::Builder&,
    kj::Own<kj::HttpClient> awsService,
    kj::StringPtr region);

}
