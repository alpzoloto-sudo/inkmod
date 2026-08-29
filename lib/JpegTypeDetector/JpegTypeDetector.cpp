#include "JpegTypeDetector.h"

#include <Logging.h>

namespace {

bool readByte(FsFile& file, uint8_t& value) {
  return file.read(&value, 1) == 1;
}

bool readBe16(FsFile& file, uint16_t& value) {
  uint8_t bytes[2];
  if (file.read(bytes, sizeof(bytes)) != static_cast<int>(sizeof(bytes))) return false;
  value = static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
  return true;
}

bool isStandaloneMarker(uint8_t marker) {
  return marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9);
}

bool isSofMarker(uint8_t marker) {
  switch (marker) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3:
    case 0xC5: case 0xC6: case 0xC7:
    case 0xC9: case 0xCA: case 0xCB:
    case 0xCD: case 0xCE: case 0xCF:
      return true;
    default:
      return false;
  }
}

}  // namespace

JpegHeaderInfo JpegTypeDetector::inspect(FsFile& file) {
  if (!file) return {JpegType::Corrupt, 0, 0};

  const auto originalPos = file.position();
  struct PositionGuard {
    FsFile& f;
    decltype(originalPos) pos;
    ~PositionGuard() { f.seek(pos); }
  } guard{file, originalPos};

  if (!file.seek(0)) return {JpegType::Corrupt, 0, 0};

  uint8_t first = 0, second = 0;
  if (!readByte(file, first) || !readByte(file, second)) return {JpegType::Corrupt, 0, 0};
  if (first != 0xFF || second != 0xD8) return {JpegType::NotJpeg, 0, 0};

  while (file.available() > 0) {
    uint8_t prefix = 0;
    do {
      if (!readByte(file, prefix)) return {JpegType::Corrupt, 0, 0};
    } while (prefix != 0xFF);

    uint8_t marker = 0;
    do {
      if (!readByte(file, marker)) return {JpegType::Corrupt, 0, 0};
    } while (marker == 0xFF);

    if (marker == 0x00) continue;  // stuffed byte (defensive; SOF should precede SOS)
    if (marker == 0xD9 || marker == 0xDA) return {JpegType::Unsupported, 0, 0};
    if (isStandaloneMarker(marker)) continue;

    if (isSofMarker(marker)) {
      uint16_t segmentLength = 0;
      uint8_t precision = 0;
      uint16_t height = 0, width = 0;
      if (!readBe16(file, segmentLength) || segmentLength < 8 || !readByte(file, precision) ||
          !readBe16(file, height) || !readBe16(file, width) || width == 0 || height == 0) {
        return {JpegType::Corrupt, 0, 0};
      }
      (void)precision;
      if (marker == 0xC0) return {JpegType::Baseline, static_cast<int>(width), static_cast<int>(height)};
      if (marker == 0xC2) return {JpegType::Progressive, static_cast<int>(width), static_cast<int>(height)};
      return {JpegType::Unsupported, static_cast<int>(width), static_cast<int>(height)};
    }

    uint16_t segmentLength = 0;
    if (!readBe16(file, segmentLength) || segmentLength < 2) return {JpegType::Corrupt, 0, 0};
    const uint32_t next = static_cast<uint32_t>(file.position()) + segmentLength - 2U;
    if (next > file.size() || !file.seek(next)) return {JpegType::Corrupt, 0, 0};
  }

  return {JpegType::Corrupt, 0, 0};
}

JpegHeaderInfo JpegTypeDetector::inspect(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("JPG", path, file)) return {JpegType::Corrupt, 0, 0};
  return inspect(file);
}

const char* JpegTypeDetector::toString(JpegType type) {
  switch (type) {
    case JpegType::NotJpeg: return "not-jpeg";
    case JpegType::Baseline: return "baseline";
    case JpegType::Progressive: return "progressive";
    case JpegType::Unsupported: return "unsupported";
    case JpegType::Corrupt: return "corrupt";
  }
  return "unknown";
}
