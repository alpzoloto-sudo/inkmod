// Versioned, fixed-size reader state. This is deliberately independent of
// storage so an activity can debounce writes without keeping a file open.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "reader/layout/PageAnchor.h"

namespace reader {

enum class PageCounterMode : uint8_t { ByChapter = 0, ByBook = 1 };

struct ReaderState {
  PageAnchor anchor = {};
  uint32_t pageNumber = 0;
  uint32_t layoutRevision = 0;
  PageCounterMode counterMode = PageCounterMode::ByChapter;
  uint8_t orientation = 0;
  uint8_t fontScale = 100;
  uint32_t flags = 0;
};

class ReaderStateCodec final {
 public:
  // 36 bytes of fields plus CRC32. Bounded bytes are used instead of JSON to
  // avoid a parser/allocation during wake and to make interrupted writes detectable.
  static constexpr size_t kSerializedSize = 40;
  static constexpr uint8_t kVersion = 1;

  static bool encode(const ReaderState& state, std::array<uint8_t, kSerializedSize>& out);
  static bool decode(const uint8_t* data, size_t size, ReaderState& out);
};

}  // namespace reader
