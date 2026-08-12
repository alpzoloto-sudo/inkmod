#pragma once

#include <cstdint>

namespace Fb2Encoding {

// Returns a Unicode code point for a byte in a supported single-byte XML
// encoding, or -1 when either the encoding or byte is undefined.
int decodeByte(const char* encodingName, uint8_t value);

}  // namespace Fb2Encoding
