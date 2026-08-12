#pragma once

#include <cstdint>
#include <type_traits>

namespace reader {

struct PageAnchor {
  uint32_t chapter = 0;
  uint64_t byteOffset = 0;
  uint32_t nodeOffset = 0;
  uint16_t layoutVersion = 1;
  uint16_t reserved = 0;
};

static_assert(std::is_trivially_copyable_v<PageAnchor>);

}  // namespace reader

