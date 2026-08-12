// On-demand EPUB chapter preparation for the new reader. A chapter is copied
// from ZIP/package to its own SD-cache file, then read through FsByteReader.
// This is intentionally chapter-scoped: no full EPUB XML or DOM enters RAM.
#pragma once

#include <cstddef>
#include <cstdint>

class Epub;

namespace reader {

class EpubChapterSource final {
 public:
  explicit EpubChapterSource(const Epub& epub) : epub_(epub) {}

  // Ensures a complete, atomically-written XHTML source for one spine entry.
  // `outPath` receives the absolute cache path suitable for FsByteReader.
  static constexpr size_t kPathCapacity = 192;
  bool prepare(uint32_t chapter, char* outPath, size_t outPathSize) const;

 private:
  const Epub& epub_;
};

}  // namespace reader
