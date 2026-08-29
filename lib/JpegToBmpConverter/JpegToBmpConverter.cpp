#include "JpegToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>
#include <JpegTypeDetector.h>
#include <ProgressiveJpegDecoder.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "BitmapHelpers.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;  // true: 8-bit grayscale (no quantization), false: 2-bit (4 levels)
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
static void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
static void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

namespace {

// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
constexpr size_t JPEG_DECODER_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP = JPEG_DECODER_SIZE + 32 * 1024;

// Static file pointer for JPEGDEC open callback.
// Safe in single-threaded embedded context; never accessed concurrently.
static FsFile* s_jpegFile = nullptr;

void* bmpJpegOpen(const char* /*filename*/, int32_t* size) {
  if (!s_jpegFile || !*s_jpegFile) return nullptr;
  s_jpegFile->seek(0);
  *size = static_cast<int32_t>(s_jpegFile->size());
  return s_jpegFile;
}

void bmpJpegClose(void* /*handle*/) {
  // Caller owns the file — do not close it here
}

int32_t bmpJpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t n = f->read(pBuf, len);
  if (n < 0) n = 0;
  pFile->iPos += n;
  return n;
}

int32_t bmpJpegSeek(JPEGFILE* pFile, int32_t pos) {
  auto* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f || !f->seek(pos)) return -1;
  pFile->iPos = pos;
  return pos;
}

// Context passed to the JPEGDEC draw callback via setUserPointer()
struct BmpConvertCtx {
  Print* bmpOut;
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  bool oneBit;
  int bytesPerRow;
  bool needsScaling;
  uint32_t scaleX_fp;  // source pixels per output pixel, 16.16 fixed-point
  uint32_t scaleY_fp;
  uint32_t srcXOffset_fp;
  uint32_t srcYOffset_fp;

  // Accumulates one MCU row (up to MAX_MCU_HEIGHT source rows × srcWidth pixels)
  // Filled column-by-column as JPEGDEC callbacks arrive for the same MCU row
  std::unique_ptr<uint8_t[]> mcuBuf;

  // Y-axis area averaging accumulators (needsScaling only)
  int currentOutY;
  uint32_t nextOutY_srcStart;  // 16.16 fixed-point boundary for the next output row
  std::unique_ptr<uint32_t[]> rowAccum;
  std::unique_ptr<uint32_t[]> rowCount;

  std::unique_ptr<uint8_t[]> bmpRow;

  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<FloydSteinbergDitherer> fsDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

  bool error;
};

struct OutputGeometry {
  int outWidth;
  int outHeight;
  uint32_t scaleX_fp;
  uint32_t scaleY_fp;
  uint32_t srcXOffset_fp;
  uint32_t srcYOffset_fp;
  bool needsScaling;
};

static uint32_t fpPerOutputPixel(const uint64_t srcSpan_fp, const int outPixels) {
  if (outPixels <= 0) return 65536;
  const uint64_t value = srcSpan_fp / static_cast<uint64_t>(outPixels);
  if (value == 0) return 1;
  if (value > UINT32_MAX) return UINT32_MAX;
  return static_cast<uint32_t>(value);
}

static OutputGeometry calculateOutputGeometry(const int srcWidth, const int srcHeight, const int targetWidth,
                                              const int targetHeight, const bool crop, const bool allowUpscale = true) {
  OutputGeometry geometry{srcWidth, srcHeight, 65536, 65536, 0, 0, false};
  if (targetWidth <= 0 || targetHeight <= 0 || srcWidth <= 0 || srcHeight <= 0) {
    return geometry;
  }

  if (crop) {
    geometry.outWidth = targetWidth;
    geometry.outHeight = targetHeight;

    const uint64_t srcWidth_fp = static_cast<uint64_t>(srcWidth) << 16;
    const uint64_t srcHeight_fp = static_cast<uint64_t>(srcHeight) << 16;
    uint64_t cropWidth_fp = srcWidth_fp;
    uint64_t cropHeight_fp = srcHeight_fp;
    const int64_t sourceVsTarget =
        static_cast<int64_t>(srcWidth) * targetHeight - static_cast<int64_t>(targetWidth) * srcHeight;

    if (sourceVsTarget > 0) {
      cropWidth_fp = (static_cast<uint64_t>(targetWidth) * static_cast<uint64_t>(srcHeight) << 16) / targetHeight;
      if (cropWidth_fp > srcWidth_fp) cropWidth_fp = srcWidth_fp;
      geometry.srcXOffset_fp = static_cast<uint32_t>((srcWidth_fp - cropWidth_fp) / 2);
    } else if (sourceVsTarget < 0) {
      cropHeight_fp = (static_cast<uint64_t>(targetHeight) * static_cast<uint64_t>(srcWidth) << 16) / targetWidth;
      if (cropHeight_fp > srcHeight_fp) cropHeight_fp = srcHeight_fp;
      geometry.srcYOffset_fp = static_cast<uint32_t>((srcHeight_fp - cropHeight_fp) / 2);
    }

    geometry.scaleX_fp = fpPerOutputPixel(cropWidth_fp, targetWidth);
    geometry.scaleY_fp = fpPerOutputPixel(cropHeight_fp, targetHeight);
    geometry.needsScaling = srcWidth != targetWidth || srcHeight != targetHeight || geometry.srcXOffset_fp != 0 ||
                            geometry.srcYOffset_fp != 0;
    return geometry;
  }

  if (srcWidth != targetWidth || srcHeight != targetHeight) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / srcWidth;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / srcHeight;
    float scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    if (!allowUpscale) scale = std::min(1.0f, scale);

    geometry.outWidth = static_cast<int>(srcWidth * scale);
    geometry.outHeight = static_cast<int>(srcHeight * scale);
    if (geometry.outWidth < 1) geometry.outWidth = 1;
    if (geometry.outHeight < 1) geometry.outHeight = 1;

    geometry.scaleX_fp = fpPerOutputPixel(static_cast<uint64_t>(srcWidth) << 16, geometry.outWidth);
    geometry.scaleY_fp = fpPerOutputPixel(static_cast<uint64_t>(srcHeight) << 16, geometry.outHeight);
    geometry.needsScaling = true;
  }

  return geometry;
}

static bool shouldContainAdaptive(const int srcWidth, const int srcHeight, const int targetWidth,
                                  const int targetHeight) {
  if (srcWidth <= 0 || srcHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) {
    return false;
  }

  constexpr int64_t kAspectTolerancePercent = 18;
  const int64_t sourceScaledToTargetHeight = static_cast<int64_t>(srcWidth) * targetHeight;
  const int64_t targetScaledToSourceHeight = static_cast<int64_t>(targetWidth) * srcHeight;
  const int64_t diff = sourceScaledToTargetHeight > targetScaledToSourceHeight
                           ? sourceScaledToTargetHeight - targetScaledToSourceHeight
                           : targetScaledToSourceHeight - sourceScaledToTargetHeight;
  return diff * 100 > targetScaledToSourceHeight * kAspectTolerancePercent;
}

// Write a fully-assembled output row (grayscale bytes, length outWidth) to BMP
static void writeOutputRow(BmpConvertCtx* ctx, const uint8_t* srcRow, int outY) {
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      ctx->bmpRow[x] = adjustPixel(srcRow[x]);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(srcRow[x], x)
                                                    : quantize1bit(srcRow[x], x, outY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel(srcRow[x]);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, outY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow);
}

// Flush one scaled output row from Y-axis accumulators and advance currentOutY
static void flushScaledRow(BmpConvertCtx* ctx) {
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      ctx->bmpRow[x] = adjustPixel(gray);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, x)
                                                    : quantize1bit(gray, x, ctx->currentOutY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel((ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, ctx->currentOutY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow);
  ctx->currentOutY++;
}

// JPEGDEC draw callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG).
// Accumulates columns into mcuBuf; once the last column arrives (completing the MCU
// row), applies scaling + dithering and writes packed BMP rows to bmpOut.
int bmpDrawCallback(JPEGDRAW* pDraw) {
  auto* ctx = reinterpret_cast<BmpConvertCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf.get() + r * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process each complete source row in this MCU row.
  // Clamp to MAX_MCU_HEIGHT so srcRow never indexes past the populated mcuBuf rows.
  const int safeEndRow = blockY + std::min(blockH, MAX_MCU_HEIGHT);

  for (int y = blockY; y < safeEndRow && y < ctx->srcHeight; y++) {
    const uint8_t* srcRow = ctx->mcuBuf.get() + (y - blockY) * ctx->srcWidth;

    if (!ctx->needsScaling) {
      // 1:1 — outWidth == srcWidth, write directly
      writeOutputRow(ctx, srcRow, y);
    } else {
      const uint64_t srcY_fp = static_cast<uint64_t>(y + 1) << 16;
      if (srcY_fp <= ctx->srcYOffset_fp) continue;

      // Fixed-point area averaging on X axis
      for (int outX = 0; outX < ctx->outWidth; outX++) {
        const uint64_t srcXStart_fp =
            static_cast<uint64_t>(ctx->srcXOffset_fp) + static_cast<uint64_t>(outX) * ctx->scaleX_fp;
        const uint64_t srcXEnd_fp =
            static_cast<uint64_t>(ctx->srcXOffset_fp) + static_cast<uint64_t>(outX + 1) * ctx->scaleX_fp;
        const int srcXStart = std::min(ctx->srcWidth - 1, static_cast<int>(srcXStart_fp >> 16));
        const int srcXEnd = std::min(ctx->srcWidth, static_cast<int>(srcXEnd_fp >> 16));
        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
          sum += srcRow[srcX];
          count++;
        }
        if (count == 0 && srcXStart < ctx->srcWidth) {
          sum = srcRow[srcXStart];
          count = 1;
        }
        ctx->rowAccum[outX] += sum;
        ctx->rowCount[outX] += count;
      }

      // Flush output row(s) whose Y boundary we've crossed
      while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
        flushScaledRow(ctx);
        ctx->nextOutY_srcStart = static_cast<uint32_t>(static_cast<uint64_t>(ctx->srcYOffset_fp) +
                                                       static_cast<uint64_t>(ctx->currentOutY + 1) * ctx->scaleY_fp);
        if (srcY_fp >= ctx->nextOutY_srcStart) continue;
        memset(ctx->rowAccum.get(), 0, ctx->outWidth * sizeof(uint32_t));
        memset(ctx->rowCount.get(), 0, ctx->outWidth * sizeof(uint32_t));
      }
    }
  }

  return ctx->error ? 0 : 1;
}



static bool convertProgressiveJpegToBmp(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                        bool oneBit, bool crop, bool adaptiveContain, const JpegHeaderInfo& header) {
  constexpr int MAX_IMAGE_WIDTH = 4096;
  constexpr int MAX_IMAGE_HEIGHT = 4096;
  constexpr int64_t MAX_IMAGE_PIXELS = 16LL * 1024LL * 1024LL;
  const int srcWidth = header.width;
  const int srcHeight = header.height;
  const int64_t srcPixels = static_cast<int64_t>(srcWidth) * srcHeight;
  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_IMAGE_WIDTH || srcHeight > MAX_IMAGE_HEIGHT ||
      srcPixels > MAX_IMAGE_PIXELS) {
    LOG_ERR("PJPG", "Progressive JPEG dimensions rejected: %dx%d", srcWidth, srcHeight);
    return false;
  }

  const int progressiveScaleDenom =
      ProgressiveJpegDecoder::chooseScaleDenom(srcWidth, srcHeight, targetWidth, targetHeight);
  const int effectiveSrcW = (srcWidth + progressiveScaleDenom - 1) / progressiveScaleDenom;
  const int effectiveSrcH = (srcHeight + progressiveScaleDenom - 1) / progressiveScaleDenom;

  const bool containInsteadOfCrop =
      crop && adaptiveContain && shouldContainAdaptive(effectiveSrcW, effectiveSrcH, targetWidth, targetHeight);
  const float fitUpscale = std::min(static_cast<float>(targetWidth) / std::max(1, srcWidth),
                                    static_cast<float>(targetHeight) / std::max(1, srcHeight));
  const bool sourceHasUsefulDetail =
      fitUpscale <= 2.50f || srcWidth >= 240 || srcHeight >= 360 || srcPixels >= 90000;
  const bool allowUpscale = fitUpscale <= 1.0f || sourceHasUsefulDetail;
  const bool cropOutput = crop && !containInsteadOfCrop && allowUpscale;
  const OutputGeometry geometry =
      calculateOutputGeometry(effectiveSrcW, effectiveSrcH, targetWidth, targetHeight, cropOutput, allowUpscale);
  const int outWidth = geometry.outWidth;
  const int outHeight = geometry.outHeight;
  if (outWidth <= 0 || outHeight <= 0) return false;

  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  BmpConvertCtx ctx = {};
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = effectiveSrcW;
  ctx.srcHeight = effectiveSrcH;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.oneBit = oneBit;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = geometry.needsScaling;
  ctx.scaleX_fp = geometry.scaleX_fp;
  ctx.scaleY_fp = geometry.scaleY_fp;
  ctx.srcXOffset_fp = geometry.srcXOffset_fp;
  ctx.srcYOffset_fp = geometry.srcYOffset_fp;

  ctx.mcuBuf = makeUniqueNoThrow<uint8_t[]>(MAX_MCU_HEIGHT * effectiveSrcW);
  ctx.bmpRow = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!ctx.mcuBuf || !ctx.bmpRow) {
    LOG_ERR("PJPG", "OOM allocating progressive BMP stream buffers");
    return false;
  }
  memset(ctx.mcuBuf.get(), 0, MAX_MCU_HEIGHT * effectiveSrcW);

  if (ctx.needsScaling) {
    ctx.rowAccum = makeUniqueNoThrow<uint32_t[]>(outWidth);
    ctx.rowCount = makeUniqueNoThrow<uint32_t[]>(outWidth);
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("PJPG", "OOM allocating progressive scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = geometry.srcYOffset_fp + geometry.scaleY_fp;
  }

  if (oneBit) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(outWidth);
    if (!ctx.atkinson1BitDitherer) return false;
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(outWidth);
      if (!ctx.atkinsonDitherer) return false;
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(outWidth);
      if (!ctx.fsDitherer) return false;
    }
  }

  ProgressiveJpegInfo info;
  if (!ProgressiveJpegDecoder::decode(jpegFile, bmpDrawCallback, &ctx, info, targetWidth, targetHeight) || ctx.error) {
    LOG_ERR("PJPG", "Progressive JPEG cover decode failed safely");
    return false;
  }
  if (ctx.needsScaling && ctx.currentOutY < ctx.outHeight) {
    LOG_ERR("PJPG", "Progressive JPEG output incomplete: %d/%d rows", ctx.currentOutY, ctx.outHeight);
    return false;
  }
  LOG_DBG("PJPG", "Progressive JPEG converted to BMP: %dx%d -> %dx%d", srcWidth, srcHeight, outWidth, outHeight);
  return true;
}

}  // namespace

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                     bool oneBit, bool crop, bool adaptiveContain) {
  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);

  const JpegHeaderInfo jpegHeader = JpegTypeDetector::inspect(jpegFile);

  // Full progressive reconstruction is valuable for reader illustrations, but
  // it is wasteful for tiny derived assets such as the 136x226 home thumbnail.
  // Those assets used to trigger the same 10 progressive scans again after the
  // reader had already decoded/cached the image, adding tens of seconds for no
  // visible benefit at thumbnail resolution. Keep the high-quality path for
  // normal/full-size BMP output, but use inkMOD's legacy JPEGDEC 1/8 path when
  // the requested output is small. Baseline JPEG routing is untouched.
  constexpr int64_t FULL_PROGRESSIVE_MIN_OUTPUT_PIXELS = 160000LL;
  const int64_t requestedOutputPixels =
      static_cast<int64_t>(std::max(1, targetWidth)) * static_cast<int64_t>(std::max(1, targetHeight));

  // The legacy progressive path only has a 1/8-scale DC reconstruction. It is
  // perfectly adequate for a genuinely tiny derived asset when that 1/8 image
  // is still at least as large as the requested thumbnail. But when the 1/8
  // image is smaller than the target, the scaler has to magnify already coarse
  // 8x8 information and the cover becomes visibly blocky in Classic/Lyra.
  // In that case pay the one-time full progressive decode and cache the sharp
  // thumbnail instead. Large covers (for example 1524x2339 -> ~191x293 at 1/8)
  // can still use the fast route for a 136x226 thumbnail.
  const int legacyWidth = (jpegHeader.width + 7) / 8;
  const int legacyHeight = (jpegHeader.height + 7) / 8;
  const float legacyFitScale = std::min(
      static_cast<float>(std::max(1, targetWidth)) / std::max(1, legacyWidth),
      static_cast<float>(std::max(1, targetHeight)) / std::max(1, legacyHeight));
  const bool legacyWouldUpscale = legacyFitScale > 1.0f;

  const bool smallDerivedProgressive =
      JpegTypeDetector::shouldUseFullProgressive(jpegHeader) &&
      requestedOutputPixels < FULL_PROGRESSIVE_MIN_OUTPUT_PIXELS &&
      !legacyWouldUpscale;

  if (JpegTypeDetector::shouldUseFullProgressive(jpegHeader) && !smallDerivedProgressive) {
    const int64_t pixels = static_cast<int64_t>(jpegHeader.width) * jpegHeader.height;
    LOG_INF("PJPG", "SOF2 full-quality BMP route: %dx%d (%lld px <= %lld, out=%dx%d)", jpegHeader.width,
            jpegHeader.height, static_cast<long long>(pixels),
            static_cast<long long>(JpegTypeDetector::FULL_PROGRESSIVE_MAX_PIXELS), targetWidth, targetHeight);
    return convertProgressiveJpegToBmp(jpegFile, bmpOut, targetWidth, targetHeight, oneBit, crop, adaptiveContain,
                                       jpegHeader);
  }
  if (smallDerivedProgressive) {
    LOG_INF("PJPG",
            "SOF2 small-output BMP route: %dx%d -> %dx%d (%lld output px, legacy=%dx%d) -> legacy JPEGDEC 1/8",
            jpegHeader.width, jpegHeader.height, targetWidth, targetHeight,
            static_cast<long long>(requestedOutputPixels), legacyWidth, legacyHeight);
  } else if (JpegTypeDetector::shouldUseFullProgressive(jpegHeader) &&
             requestedOutputPixels < FULL_PROGRESSIVE_MIN_OUTPUT_PIXELS && legacyWouldUpscale) {
    LOG_INF("PJPG",
            "SOF2 cover-quality route: legacy 1/8 %dx%d would upscale to %dx%d -> full progressive",
            legacyWidth, legacyHeight, targetWidth, targetHeight);
  }
  const bool legacyProgressive = jpegHeader.type == JpegType::Progressive;
  if (jpegHeader.type != JpegType::Baseline && !legacyProgressive) {
    LOG_ERR("JPG", "JPEG header rejected: %s", JpegTypeDetector::toString(jpegHeader.type));
    return false;
  }

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", ESP.getFreeHeap(), MIN_FREE_HEAP);
    return false;
  }

  s_jpegFile = &jpegFile;

  const auto jpeg = makeUniqueNoThrow<JPEGDEC>();
  if (!jpeg) {
    LOG_ERR("JPG", "OOM: JPEG decoder");
    return false;
  }

  int rc = jpeg->open("", bmpJpegOpen, bmpJpegClose, bmpJpegRead, bmpJpegSeek, bmpDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "JPEG open failed (err=%d)", jpeg->getLastError());
    return false;
  }

  const ScopedCleanup cleanup{[&jpeg]() { jpeg->close(); }};

  const int srcWidth = jpeg->getWidth();
  const int srcHeight = jpeg->getHeight();

  LOG_DBG("JPG", "JPEG dimensions: %dx%d", srcWidth, srcHeight);

  // JPEGDEC works MCU-by-MCU and the cover scaler streams output, so the
  // source image does not need to fit in RAM.  Keep a generous but finite
  // guard for malformed/absurd JPEGs.  Large portrait covers such as
  // 2400x3200 are common in FB2 and are safely downscaled to the X4 target.
  constexpr int MAX_IMAGE_WIDTH = 4096;
  constexpr int MAX_IMAGE_HEIGHT = 4096;
  constexpr int64_t MAX_IMAGE_PIXELS = 16LL * 1024LL * 1024LL;
  const int64_t srcPixels = static_cast<int64_t>(srcWidth) * static_cast<int64_t>(srcHeight);

  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_IMAGE_WIDTH || srcHeight > MAX_IMAGE_HEIGHT ||
      srcPixels > MAX_IMAGE_PIXELS) {
    LOG_DBG("JPG", "Image too large or invalid (%dx%d, pixels=%lld), max supported: %dx%d / %lld pixels",
            srcWidth, srcHeight, static_cast<long long>(srcPixels), MAX_IMAGE_WIDTH, MAX_IMAGE_HEIGHT,
            static_cast<long long>(MAX_IMAGE_PIXELS));
    return false;
  }

  // Use JPEGDEC's built-in coarse downscaling for large baseline covers too.
  // This keeps the MCU row buffer small and avoids decoding millions of source
  // pixels only to throw most of them away for the 800x480 panel/thumbnail.
  int decodeDenom = 1;
  int decodeFlags = 0;
  if (legacyProgressive) {
    // Exact legacy behavior from inkMOD before the full progressive decoder:
    // patched JPEGDEC decodes the first/DC scan at 1/8 very quickly.
    decodeDenom = 8;
    decodeFlags = JPEG_SCALE_EIGHTH;
    const int64_t pixels = static_cast<int64_t>(srcWidth) * srcHeight;
    LOG_INF("PJPG", "SOF2 large-image BMP route: %dx%d (%lld px > %lld) -> legacy JPEGDEC 1/8",
            srcWidth, srcHeight, static_cast<long long>(pixels),
            static_cast<long long>(JpegTypeDetector::FULL_PROGRESSIVE_MAX_PIXELS));
  } else {
    const float scaleX = static_cast<float>(targetWidth) / std::max(1, srcWidth);
    const float scaleY = static_cast<float>(targetHeight) / std::max(1, srcHeight);
    float requestedScale = crop ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
    if (requestedScale > 1.0f) requestedScale = 1.0f;
    if (requestedScale <= 0.125f) {
      decodeDenom = 8;
      decodeFlags = JPEG_SCALE_EIGHTH;
    } else if (requestedScale <= 0.25f) {
      decodeDenom = 4;
      decodeFlags = JPEG_SCALE_QUARTER;
    } else if (requestedScale <= 0.5f) {
      decodeDenom = 2;
      decodeFlags = JPEG_SCALE_HALF;
    }
  }

  const int effectiveSrcW = (srcWidth + decodeDenom - 1) / decodeDenom;
  const int effectiveSrcH = (srcHeight + decodeDenom - 1) / decodeDenom;
  LOG_DBG("JPG", "JPEG coarse decode 1/%d: %dx%d -> %dx%d", decodeDenom, srcWidth, srcHeight, effectiveSrcW,
          effectiveSrcH);

  // Calculate output dimensions. Crop mode behaves like CSS object-fit: cover:
  // scale to fill the requested box, then sample a centered source crop before dithering.
  const bool containInsteadOfCrop =
      crop && adaptiveContain && shouldContainAdaptive(effectiveSrcW, effectiveSrcH, targetWidth, targetHeight);
  // Adaptive cover upscaling.  The previous guard disabled ALL upscaling when
  // either source dimension was even one pixel below the panel size.  That
  // turned perfectly usable 470x720 covers into a small centred card on X4.
  //
  // Allow a modest/full-screen upscale when the source is already reasonably
  // detailed, but keep genuinely tiny thumbnails at native size so a 100x150
  // embedded image is not magnified into huge blocks.
  const float fitUpscale = std::min(static_cast<float>(targetWidth) / std::max(1, srcWidth),
                                    static_cast<float>(targetHeight) / std::max(1, srcHeight));
  const int64_t sourcePixels = static_cast<int64_t>(srcWidth) * srcHeight;
  const bool sourceHasUsefulDetail =
      fitUpscale <= 2.50f || srcWidth >= 240 || srcHeight >= 360 || sourcePixels >= 90000;

  const bool allowUpscale = fitUpscale <= 1.0f || sourceHasUsefulDetail;
  const bool cropOutput = crop && !containInsteadOfCrop && allowUpscale;
  const OutputGeometry geometry =
      calculateOutputGeometry(effectiveSrcW, effectiveSrcH, targetWidth, targetHeight, cropOutput, allowUpscale);
  const int outWidth = geometry.outWidth;
  const int outHeight = geometry.outHeight;
  const bool needsScaling = geometry.needsScaling;
  LOG_DBG("JPG", "Scaling %dx%d -> %dx%d (target %dx%d, mode=%s, offset %u,%u)", effectiveSrcW, effectiveSrcH, outWidth,
          outHeight, targetWidth, targetHeight, cropOutput ? "cover" : "contain", geometry.srcXOffset_fp >> 16,
          geometry.srcYOffset_fp >> 16);

  // Write BMP header with output dimensions
  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  BmpConvertCtx ctx = {};
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = effectiveSrcW;
  ctx.srcHeight = effectiveSrcH;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.oneBit = oneBit;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = needsScaling;
  ctx.scaleX_fp = geometry.scaleX_fp;
  ctx.scaleY_fp = geometry.scaleY_fp;
  ctx.srcXOffset_fp = geometry.srcXOffset_fp;
  ctx.srcYOffset_fp = geometry.srcYOffset_fp;
  ctx.error = false;

  // MCU row buffer: MAX_MCU_HEIGHT rows × ctx.srcWidth columns of grayscale.
  ctx.mcuBuf = makeUniqueNoThrow<uint8_t[]>(MAX_MCU_HEIGHT * effectiveSrcW);
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "OOM: MCU buffer (%d bytes)", MAX_MCU_HEIGHT * effectiveSrcW);
    return false;
  }
  memset(ctx.mcuBuf.get(), 0, MAX_MCU_HEIGHT * effectiveSrcW);

  ctx.bmpRow = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!ctx.bmpRow) {
    LOG_ERR("JPG", "OOM: BMP row buffer");
    return false;
  }

  if (needsScaling) {
    ctx.rowAccum = makeUniqueNoThrow<uint32_t[]>(outWidth);
    ctx.rowCount = makeUniqueNoThrow<uint32_t[]>(outWidth);
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("JPG", "OOM: scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = geometry.srcYOffset_fp + geometry.scaleY_fp;
  }

  if (oneBit) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(outWidth);
    if (!ctx.atkinson1BitDitherer) {
      LOG_ERR("JPG", "OOM: Atkinson1BitDitherer");
      return false;
    }
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(outWidth);
      if (!ctx.atkinsonDitherer) {
        LOG_ERR("JPG", "OOM: AtkinsonDitherer");
        return false;
      }
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(outWidth);
      if (!ctx.fsDitherer) {
        LOG_ERR("JPG", "OOM: FloydSteinbergDitherer");
        return false;
      }
    }
  }

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  rc = jpeg->decode(0, 0, decodeFlags);

  if (rc != 1 || ctx.error) {
    LOG_ERR("JPG", "JPEG decode failed (rc=%d, err=%d)", rc, jpeg->getLastError());
    return false;
  }

  if (ctx.needsScaling && ctx.currentOutY < ctx.outHeight) {
    LOG_ERR("JPG", "JPEG decode incomplete: %d/%d output rows written", ctx.currentOutY, ctx.outHeight);
    return false;
  }

  LOG_DBG("JPG", "Successfully converted JPEG to BMP");
  return true;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetWidth, targetHeight, false, crop);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight, bool adaptiveContain) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false, true, adaptiveContain);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight, bool adaptiveContain) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true, adaptiveContain);
}
