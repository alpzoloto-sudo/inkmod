#include "ReaderState.h"

namespace reader {

namespace {

constexpr uint32_t kMagic = 0x31535452U;  // "RTS1" in little-endian storage.

void put32(uint8_t* const out, const uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8));
}

void put64(uint8_t* const out, const uint64_t value) {
  for (uint8_t i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8));
}

uint32_t get32(const uint8_t* const in) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(in[i]) << (i * 8);
  return value;
}

uint64_t get64(const uint8_t* const in) {
  uint64_t value = 0;
  for (uint8_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(in[i]) << (i * 8);
  return value;
}

uint32_t crc32(const uint8_t* const data, const size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & -(crc & 1U));
  }
  return ~crc;
}

}  // namespace

bool ReaderStateCodec::encode(const ReaderState& state, std::array<uint8_t, kSerializedSize>& out) {
  out.fill(0);
  put32(out.data(), kMagic);
  out[4] = kVersion;
  out[5] = static_cast<uint8_t>(state.counterMode);
  out[6] = state.orientation;
  out[7] = state.fontScale;
  put32(out.data() + 8, state.anchor.chapter);
  put64(out.data() + 12, state.anchor.byteOffset);
  put32(out.data() + 20, state.anchor.nodeOffset);
  put32(out.data() + 24, state.pageNumber);
  put32(out.data() + 28, state.flags);
  put32(out.data() + 32, state.layoutRevision);
  put32(out.data() + 36, crc32(out.data(), 36));
  return true;
}

bool ReaderStateCodec::decode(const uint8_t* const data, const size_t size, ReaderState& out) {
  if (!data || size != kSerializedSize || get32(data) != kMagic || data[4] != kVersion ||
      get32(data + 36) != crc32(data, 36)) {
    return false;
  }
  if (data[5] > static_cast<uint8_t>(PageCounterMode::ByBook)) return false;

  out.counterMode = static_cast<PageCounterMode>(data[5]);
  out.orientation = data[6];
  out.fontScale = data[7];
  out.anchor.chapter = get32(data + 8);
  out.anchor.byteOffset = get64(data + 12);
  out.anchor.nodeOffset = get32(data + 20);
  out.pageNumber = get32(data + 24);
  out.flags = get32(data + 28);
  out.layoutRevision = get32(data + 32);
  return true;
}

}  // namespace reader
