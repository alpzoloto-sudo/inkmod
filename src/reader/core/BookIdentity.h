// Stable, allocation-free book/cache identity. The source signature makes a
// replaced file at the same path receive a different cache namespace.
#pragma once

#include <cstddef>
#include <cstdint>

#include "reader/document/DocumentTypes.h"

namespace reader {

struct SourceSignature {
  uint64_t size = 0;
  uint32_t modifiedTime = 0;
};

struct BookId {
  uint64_t value = 0;
  constexpr bool valid() const { return value != 0; }
};

class BookIdentity {
 public:
  static BookId make(StringRef path, SourceSignature signature);

  // Writes `book_` + sixteen lowercase hexadecimal characters + NUL. Caller
  // needs at least kCacheDirectoryNameCapacity bytes.
  static constexpr size_t kCacheDirectoryNameCapacity = 22;
  static bool cacheDirectoryName(BookId id, char* out, size_t outSize);
};

}  // namespace reader
