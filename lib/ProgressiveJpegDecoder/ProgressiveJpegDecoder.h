#pragma once

#include <HalStorage.h>
#include <JPEGDEC.h>

#include <string>

struct ProgressiveJpegInfo {
  int width{0};
  int height{0};
  int scaledWidth{0};
  int scaledHeight{0};
  int scaleDenom{1};
  int scans{0};
};

// Isolated SOF2 decoder for ESP32-C3/no-PSRAM targets.
//
// Baseline JPEG never enters this code path. Progressive coefficients are kept
// in a temporary SD-backed file instead of SRAM. Only the luminance component
// is reconstructed because inkMOD ultimately renders to a grayscale e-paper
// framebuffer. This keeps heap usage essentially independent of source image
// dimensions while still processing every progressive Y scan (including AC
// refinement scans), unlike JPEGDEC's DC-only thumbnail mode.
class ProgressiveJpegDecoder {
 public:
  static constexpr uint32_t REQUIRED_HEADROOM = 32U * 1024U;

  // Select the coarsest 1/1, 1/2, 1/4 or 1/8 output that still covers the
  // requested target in both axes. The coefficient stream itself is decoded
  // completely; this only controls the final grayscale output resolution.
  static int chooseScaleDenom(int srcWidth, int srcHeight, int targetWidth, int targetHeight);

  static bool decode(FsFile& file, JPEG_DRAW_CALLBACK* drawCallback, void* user, ProgressiveJpegInfo& info,
                     int targetWidth = 0, int targetHeight = 0);
  static bool decode(const std::string& path, JPEG_DRAW_CALLBACK* drawCallback, void* user, ProgressiveJpegInfo& info,
                     int targetWidth = 0, int targetHeight = 0);
};
