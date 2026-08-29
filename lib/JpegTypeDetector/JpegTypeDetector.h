#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>

enum class JpegType : unsigned char {
  NotJpeg = 0,
  Baseline,
  Progressive,
  Unsupported,
  Corrupt,
};

struct JpegHeaderInfo {
  JpegType type{JpegType::Corrupt};
  int width{0};
  int height{0};
};

class JpegTypeDetector {
 public:
  // Lightweight SOF scanner. The original file position is restored before return.
  static JpegHeaderInfo inspect(FsFile& file);
  static JpegHeaderInfo inspect(const std::string& path);
  static JpegType getJpegType(FsFile& file) { return inspect(file).type; }
  static JpegType getJpegType(const std::string& path) { return inspect(path).type; }

  // Full progressive reconstruction is useful for normal FB2/EPUB artwork,
  // but becomes prohibitively slow on ESP32-C3 for multi-megapixel sources.
  // Larger SOF2 images intentionally fall back to inkMOD's original patched
  // JPEGDEC progressive path (DC-only, JPEG_SCALE_EIGHTH).
  static constexpr int64_t FULL_PROGRESSIVE_MAX_PIXELS = 1200000LL;
  static bool shouldUseFullProgressive(const JpegHeaderInfo& info) {
    return info.type == JpegType::Progressive && info.width > 0 && info.height > 0 &&
           static_cast<int64_t>(info.width) * static_cast<int64_t>(info.height) <= FULL_PROGRESSIVE_MAX_PIXELS;
  }

  static const char* toString(JpegType type);
};
