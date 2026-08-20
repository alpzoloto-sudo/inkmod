#pragma once

#include <cstddef>

// Minimal host-test stand-in for Arduino String. FsHelpers' production header
// only needs these two accessors for its inline overloads; the tests exercise
// the std::string_view API directly.
class String {
 public:
  const char* c_str() const { return ""; }
  std::size_t length() const { return 0; }
};
