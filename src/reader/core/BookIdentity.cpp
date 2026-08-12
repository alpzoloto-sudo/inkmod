#include "BookIdentity.h"

#include <cstdio>

namespace reader {

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

void hashByte(uint64_t& hash, const uint8_t value) {
  hash ^= value;
  hash *= kFnvPrime;
}

void hashUint64(uint64_t& hash, const uint64_t value) {
  for (uint8_t shift = 0; shift < 64; shift += 8) hashByte(hash, static_cast<uint8_t>(value >> shift));
}

}  // namespace

BookId BookIdentity::make(const StringRef path, const SourceSignature signature) {
  if (!path.data || path.size == 0) return {};

  uint64_t hash = kFnvOffset;
  for (uint32_t index = 0; index < path.size; ++index) hashByte(hash, static_cast<uint8_t>(path.data[index]));
  hashUint64(hash, signature.size);
  hashUint64(hash, signature.modifiedTime);
  return {.value = hash};
}

bool BookIdentity::cacheDirectoryName(const BookId id, char* const out, const size_t outSize) {
  if (!id.valid() || !out || outSize < kCacheDirectoryNameCapacity) return false;
  const int written = std::snprintf(out, outSize, "book_%016llx", static_cast<unsigned long long>(id.value));
  return written == static_cast<int>(kCacheDirectoryNameCapacity - 1);
}

}  // namespace reader
