// Copyright (c) 2023 Vaci Koblizek.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "common.h"

#include <kj/debug.h>

#include <chrono>

namespace aws {
  
kj::String dateStr(kj::Date date, kj::StringPtr format) {
  std::tm tm{
    .tm_sec =  static_cast<int>((date - kj::UNIX_EPOCH)/kj::SECONDS),
    .tm_mday = 1,
    .tm_year = (1970 - 1900)
  };
  std::mktime(&tm);

  kj::FixedArray<char, 256> txt;
  auto c = ::strftime(txt.begin(), txt.size(), format.begin(), &tm);
  KJ_DREQUIRE(c != 0);
  return kj::heapString(txt.begin(), c);
}

kj::String yyyymmdd(kj::Date date) {
  std::tm tm{
    .tm_sec =  static_cast<int>((date - kj::UNIX_EPOCH)/kj::SECONDS),
    .tm_mday = 1,
    .tm_year = (1970 - 1900)
  };
  std::mktime(&tm);

  kj::FixedArray<char, 9> txt;
  auto c = ::strftime(txt.begin(), txt.size(), "%Y%m%d", &tm);
  KJ_DREQUIRE(c != 0);
  return kj::heapString(txt.begin(), c);
}

}
