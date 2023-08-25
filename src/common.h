#pragma once

// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/time.h>
#include <kj/string.h>

namespace aws {

kj::String dateStr(kj::Date date, kj::StringPtr format);
kj::String yyyymmdd(kj::Date date);

}
	       
