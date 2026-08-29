#include "PngToFramebufferConverter.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <PNGdec.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "BitmapHelpers.h"
#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// ===============================
// PNGdec fast path
// ===============================

struct PngContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  float scale{1.f};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};

  PixelCache cache;
  bool caching{false};

  uint8_t* grayLineBuffer{nullptr};

  // Error-diffusion (Atkinson) dithering carries accumulated error across
  // scanlines, giving smooth photographic gradients instead of the coarse,
  // regular crosshatch pattern of ordered (Bayer) dithering. Sized to the
  // final output width and owned for the lifetime of one decode; nullptr
  // falls back to the old ordered dithering (e.g. under memory pressure).
  AtkinsonDitherer* ditherer{nullptr};

  ~PngContext() { delete ditherer; }
};

void* pngOpenWithHandle(const char* filename, int32_t* size) {
  FsFile* f = static_cast<FsFile*>(malloc(sizeof(FsFile)));
  if (!f) return nullptr;
  new (f) FsFile();
  if (!Storage.openFileForRead("PNG", std::string(filename), *f)) {
    f->~FsFile();
    free(f);
    return nullptr;
  }
  *size = f->size();
  return f;
}

void pngCloseWithHandle(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    f->~FsFile();
    free(f);
  }
}

int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  return f->read(pBuf, len);
}

int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

constexpr uint32_t PNG_DECODER_APPROX_SIZE = 44U * 1024U;

bool validatePngDimensionsLocal(int width, int height) {
  constexpr int MAX_SOURCE_WIDTH = 2048;
  constexpr int MAX_SOURCE_HEIGHT = 3072;
  constexpr int64_t MAX_SOURCE_PIXELS = 2048LL * 3072LL;
  if (width <= 0 || height <= 0 || width > MAX_SOURCE_WIDTH || height > MAX_SOURCE_HEIGHT ||
      static_cast<int64_t>(width) * static_cast<int64_t>(height) > MAX_SOURCE_PIXELS) {
    LOG_ERR("PNG", "Invalid PNG dimensions: %dx%d", width, height);
    return false;
  }
  return true;
}

void warnUnsupportedPngFeatureLocal(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("PNG", "Unsupported PNG feature %s in %s; attempting best-effort decode", feature.c_str(), imagePath.c_str());
}

int bytesPerPixelFromType(int pixelType) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return 3;
    case PNG_PIXEL_GRAY_ALPHA:
      return 2;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return 4;
    case PNG_PIXEL_GRAYSCALE:
    case PNG_PIXEL_INDEXED:
    default:
      return 1;
  }
}

int requiredPngInternalBufferBytes(int srcWidth, int pixelType) {
  int pitch = srcWidth * bytesPerPixelFromType(pixelType);
  return ((pitch + 1) * 2) + 32;
}

void convertLineToGray(uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, uint8_t* palette, int hasAlpha) {
  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      memcpy(grayLine, pPixels, width);
      break;

    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        uint8_t* p = &pPixels[x * 3];
        grayLine[x] = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      break;

    case PNG_PIXEL_INDEXED:
      if (palette) {
        if (hasAlpha) {
          for (int x = 0; x < width; x++) {
            uint8_t idx = pPixels[x];
            uint8_t* p = &palette[idx * 3];
            uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            uint8_t alpha = palette[768 + idx];
            grayLine[x] = static_cast<uint8_t>((gray * alpha + 255 * (255 - alpha)) / 255);
          }
        } else {
          for (int x = 0; x < width; x++) {
            uint8_t* p = &palette[pPixels[x] * 3];
            grayLine[x] = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          }
        }
      } else {
        memcpy(grayLine, pPixels, width);
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t gray = pPixels[x * 2];
        uint8_t alpha = pPixels[x * 2 + 1];
        grayLine[x] = static_cast<uint8_t>((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t* p = &pPixels[x * 4];
        uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        uint8_t alpha = p[3];
        grayLine[x] = static_cast<uint8_t>((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    default:
      memset(grayLine, 128, width);
      break;
  }
}

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return 0;

  const int srcY = pDraw->y;
  const int srcWidth = ctx->srcWidth;
  const int dstY = static_cast<int>(srcY * ctx->scale);
  if (dstY == ctx->lastDstY) return 1;
  ctx->lastDstY = dstY;
  if (dstY >= ctx->dstHeight) return 1;

  const int outY = ctx->config->y + dstY;
  if (outY >= ctx->screenHeight) return 1;

  convertLineToGray(pDraw->pPixels, ctx->grayLineBuffer, srcWidth, pDraw->iPixelType, pDraw->pPalette,
                    pDraw->iHasAlpha);

  const int dstWidth = ctx->dstWidth;
  const int outXBase = ctx->config->x;
  const int screenWidth = ctx->screenWidth;
  const bool useDithering = ctx->config->useDithering;
  bool caching = ctx->caching;

  DirectPixelWriter pw;
  pw.init(*ctx->renderer);
  pw.beginRow(outY);

  DirectCacheWriter cw;
  if (caching) {
    if (!ctx->cache.advanceTo(dstY)) {
      caching = false;
      ctx->caching = false;
    } else {
      cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
      cw.beginRow(outY, ctx->config->y + ctx->cache.bandStart);
    }
  }

  int srcX = 0;
  int error = 0;
  for (int dstX = 0; dstX < dstWidth; dstX++) {
    const int outX = outXBase + dstX;
    if (outX >= 0 && outX < screenWidth) {
      const uint8_t gray = ctx->grayLineBuffer[srcX];
      uint8_t ditheredGray;
      if (ctx->ditherer) {
        ditheredGray = ctx->ditherer->processPixel(gray, dstX);
      } else if (useDithering) {
        ditheredGray = applyBayerDither4Level(gray, outX, outY);
      } else {
        ditheredGray = gray / 85;
        if (ditheredGray > 3) ditheredGray = 3;
      }
      pw.writePixel(outX, ditheredGray);
      if (caching) cw.writePixel(outX, ditheredGray);
    }

    error += srcWidth;
    while (error >= dstWidth) {
      error -= dstWidth;
      srcX++;
    }
    if (srcX >= srcWidth) srcX = srcWidth - 1;
  }

  if (ctx->ditherer) ctx->ditherer->nextRow();

  return 1;
}

bool decodeToFramebufferWithPngDec(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) {
  constexpr uint32_t PNG_DECODER_HEADROOM = 16U * 1024U;
  if (!MemoryBudget::hasHeapForImageDecoder("PNG", "PNG", PNG_DECODER_APPROX_SIZE, PNG_DECODER_HEADROOM)) {
    return false;
  }

  void* pngMem = malloc(sizeof(PNG));
  if (!pngMem) {
    LOG_DBG("PNG", "PNGdec allocation skipped, falling back to streaming decoder");
    return false;
  }
  PNG* png = new (pngMem) PNG();

  PngContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  const int rcOpen = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle,
                               pngSeekWithHandle, pngDrawCallback);
  if (rcOpen != PNG_SUCCESS) {
    LOG_ERR("PNG", "Failed to open PNG with PNGdec: %d", rcOpen);
    png->~PNG();
    free(pngMem);
    return false;
  }

  if (!validatePngDimensionsLocal(png->getWidth(), png->getHeight())) {
    png->close();
    png->~PNG();
    free(pngMem);
    return false;
  }

  ctx.srcWidth = png->getWidth();
  ctx.srcHeight = png->getHeight();
  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = static_cast<float>(ctx.dstWidth) / std::max(1, ctx.srcWidth);
  } else {
    float scaleX = static_cast<float>(config.maxWidth) / std::max(1, ctx.srcWidth);
    float scaleY = static_cast<float>(config.maxHeight) / std::max(1, ctx.srcHeight);
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;
    ctx.dstWidth = std::max(1, static_cast<int>(ctx.srcWidth * ctx.scale));
    ctx.dstHeight = std::max(1, static_cast<int>(ctx.srcHeight * ctx.scale));
  }
  ctx.lastDstY = -1;

  // Error-diffusion dithering needs one row of error state per output column.
  // Allocation failure is not fatal: the draw callback falls back to ordered
  // (Bayer) dithering whenever ctx.ditherer is null.
  if (config.useDithering) {
    ctx.ditherer = new (std::nothrow) AtkinsonDitherer(ctx.dstWidth);
  }

  const int pixelType = png->getPixelType();
  const int requiredInternal = requiredPngInternalBufferBytes(ctx.srcWidth, pixelType);
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    LOG_INF("PNG", "PNGdec path would overflow internal row buffer (need %d, have %d); using streaming decoder",
            requiredInternal, PNG_MAX_BUFFERED_PIXELS);
    png->close();
    png->~PNG();
    free(pngMem);
    return false;
  }

  if (png->getBpp() != 8) {
    warnUnsupportedPngFeatureLocal("bit depth (" + std::to_string(png->getBpp()) + "bpp)", imagePath);
  }

  const size_t grayBufSize = std::max<size_t>(static_cast<size_t>(ctx.srcWidth), 256U);
  ctx.grayLineBuffer = static_cast<uint8_t*>(malloc(grayBufSize));
  if (!ctx.grayLineBuffer) {
    LOG_DBG("PNG", "Failed to allocate PNGdec gray row buffer, using streaming decoder");
    png->close();
    png->~PNG();
    free(pngMem);
    return false;
  }

  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      LOG_ERR("PNG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  const unsigned long decodeStart = millis();
  const int rcDecode = png->decode(&ctx, 0);
  const unsigned long decodeTime = millis() - decodeStart;

  free(ctx.grayLineBuffer);
  ctx.grayLineBuffer = nullptr;

  if (rcDecode != PNG_SUCCESS) {
    LOG_ERR("PNG", "PNGdec decode failed: %d", rcDecode);
    if (ctx.caching) ctx.cache.abort();
    png->close();
    png->~PNG();
    free(pngMem);
    return false;
  }

  png->close();
  png->~PNG();
  free(pngMem);

  if (ctx.caching) {
    ctx.cache.finalize();
  }

  LOG_DBG("PNG", "PNGdec path complete - render time: %lu ms", decodeTime);
  return true;
}

// ===============================
// Low-memory streaming PNG fallback
// ===============================

inline uint8_t paethPredictor(uint8_t a, uint8_t b, uint8_t c) {
  int p = static_cast<int>(a) + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

constexpr uint8_t PNG_SIGNATURE[8] = {137, 80, 78, 71, 13, 10, 26, 10};

enum PngColorType : uint8_t {
  PNG_COLOR_GRAYSCALE = 0,
  PNG_COLOR_RGB = 2,
  PNG_COLOR_PALETTE = 3,
  PNG_COLOR_GRAYSCALE_ALPHA = 4,
  PNG_COLOR_RGBA = 6,
};

enum PngFilter : uint8_t {
  PNG_FILTER_NONE = 0,
  PNG_FILTER_SUB = 1,
  PNG_FILTER_UP = 2,
  PNG_FILTER_AVERAGE = 3,
  PNG_FILTER_PAETH = 4,
};

bool readBE32(FsFile& file, uint32_t& value) {
  uint8_t buf[4];
  if (file.read(buf, 4) != 4) return false;
  value = (static_cast<uint32_t>(buf[0]) << 24) | (static_cast<uint32_t>(buf[1]) << 16) |
          (static_cast<uint32_t>(buf[2]) << 8) | buf[3];
  return true;
}

bool readBE16Buf(const uint8_t* data, uint16_t& value) {
  value = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
  return true;
}

struct StreamingGeometry {
  int outWidth{0};
  int outHeight{0};
  uint32_t scaleX_fp{65536};
  uint32_t scaleY_fp{65536};
  bool needsScaling{false};
};

StreamingGeometry calculateGeometry(const uint32_t srcWidth, const uint32_t srcHeight, const RenderConfig& config) {
  StreamingGeometry g;
  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    g.outWidth = std::max(1, config.maxWidth);
    g.outHeight = std::max(1, config.maxHeight);
  } else {
    float scaleX = static_cast<float>(config.maxWidth) / std::max<uint32_t>(1, srcWidth);
    float scaleY = static_cast<float>(config.maxHeight) / std::max<uint32_t>(1, srcHeight);
    float scale = std::min(scaleX, scaleY);
    if (scale > 1.0f) scale = 1.0f;
    g.outWidth = std::max(1, static_cast<int>(srcWidth * scale));
    g.outHeight = std::max(1, static_cast<int>(srcHeight * scale));
  }
  g.needsScaling = (g.outWidth != static_cast<int>(srcWidth) || g.outHeight != static_cast<int>(srcHeight));
  g.scaleX_fp = static_cast<uint32_t>((static_cast<uint64_t>(srcWidth) << 16) / std::max(1, g.outWidth));
  g.scaleY_fp = static_cast<uint32_t>((static_cast<uint64_t>(srcHeight) << 16) / std::max(1, g.outHeight));
  if (g.scaleX_fp == 0) g.scaleX_fp = 1;
  if (g.scaleY_fp == 0) g.scaleY_fp = 1;
  return g;
}

struct StreamingPngContext {
  InflateReader reader;
  FsFile* file{nullptr};

  uint32_t width{0};
  uint32_t height{0};
  uint8_t bitDepth{0};
  uint8_t colorType{0};
  uint8_t bytesPerPixel{0};
  uint32_t rawRowBytes{0};

  uint8_t* currentRow{nullptr};
  uint8_t* previousRow{nullptr};

  uint32_t chunkBytesRemaining{0};
  bool idatFinished{false};
  uint8_t readBuf[2048];

  uint8_t palette[256 * 3];
  uint8_t paletteAlpha[256];
  int paletteSize{0};
  bool hasPaletteAlpha{false};
  bool hasGrayTransparent{false};
  uint16_t transparentGray{0};
  bool hasRgbTransparent{false};
  uint16_t transparentRed{0};
  uint16_t transparentGreen{0};
  uint16_t transparentBlue{0};
};

bool findNextIdatChunk(StreamingPngContext& ctx) {
  while (true) {
    uint32_t chunkLen;
    if (!readBE32(*ctx.file, chunkLen)) return false;

    uint8_t chunkType[4];
    if (ctx.file->read(chunkType, 4) != 4) return false;

    if (memcmp(chunkType, "IDAT", 4) == 0) {
      ctx.chunkBytesRemaining = chunkLen;
      return true;
    }

    if (!ctx.file->seekCur(chunkLen + 4)) return false;
    if (memcmp(chunkType, "IEND", 4) == 0) return false;
  }
}

int pngIdatReadCallback(uzlib_uncomp* uncomp) {
  auto* ctx = reinterpret_cast<StreamingPngContext*>(uncomp);
  if (!ctx || ctx->idatFinished) return -1;

  while (ctx->chunkBytesRemaining == 0) {
    if (!ctx->file->seekCur(4)) {
      ctx->idatFinished = true;
      return -1;
    }
    if (!findNextIdatChunk(*ctx)) {
      ctx->idatFinished = true;
      return -1;
    }
  }

  size_t toRead = sizeof(ctx->readBuf);
  if (toRead > ctx->chunkBytesRemaining) toRead = ctx->chunkBytesRemaining;
  const int bytesRead = ctx->file->read(ctx->readBuf, toRead);
  if (bytesRead <= 0) {
    ctx->idatFinished = true;
    return -1;
  }

  ctx->chunkBytesRemaining -= static_cast<uint32_t>(bytesRead);
  uncomp->source = ctx->readBuf + 1;
  uncomp->source_limit = ctx->readBuf + bytesRead;
  return ctx->readBuf[0];
}

bool decodeScanline(StreamingPngContext& ctx) {
  uint8_t filterType;
  if (!ctx.reader.read(&filterType, 1)) return false;
  if (!ctx.reader.read(ctx.currentRow, ctx.rawRowBytes)) return false;

  const int bpp = ctx.bytesPerPixel;
  switch (filterType) {
    case PNG_FILTER_NONE:
      break;
    case PNG_FILTER_SUB:
      for (uint32_t i = bpp; i < ctx.rawRowBytes; ++i) ctx.currentRow[i] += ctx.currentRow[i - bpp];
      break;
    case PNG_FILTER_UP:
      for (uint32_t i = 0; i < ctx.rawRowBytes; ++i) ctx.currentRow[i] += ctx.previousRow[i];
      break;
    case PNG_FILTER_AVERAGE:
      for (uint32_t i = 0; i < ctx.rawRowBytes; ++i) {
        uint8_t a = (i >= static_cast<uint32_t>(bpp)) ? ctx.currentRow[i - bpp] : 0;
        uint8_t b = ctx.previousRow[i];
        ctx.currentRow[i] += (a + b) / 2;
      }
      break;
    case PNG_FILTER_PAETH:
      for (uint32_t i = 0; i < ctx.rawRowBytes; ++i) {
        uint8_t a = (i >= static_cast<uint32_t>(bpp)) ? ctx.currentRow[i - bpp] : 0;
        uint8_t b = ctx.previousRow[i];
        uint8_t c = (i >= static_cast<uint32_t>(bpp)) ? ctx.previousRow[i - bpp] : 0;
        ctx.currentRow[i] += paethPredictor(a, b, c);
      }
      break;
    default:
      LOG_ERR("PNG", "Unknown PNG filter type: %d", filterType);
      return false;
  }
  return true;
}

inline uint8_t blendOverWhite(const uint8_t gray, const uint8_t alpha) {
  return static_cast<uint8_t>((gray * alpha + 255 * (255 - alpha)) / 255);
}

void convertStreamingScanlineToGray(const StreamingPngContext& ctx, uint8_t* grayRow) {
  const uint8_t* src = ctx.currentRow;
  const uint32_t w = ctx.width;

  switch (ctx.colorType) {
    case PNG_COLOR_GRAYSCALE:
      if (ctx.bitDepth == 8) {
        if (ctx.hasGrayTransparent) {
          const uint8_t transparent = static_cast<uint8_t>(ctx.transparentGray >> 8);
          for (uint32_t x = 0; x < w; ++x) grayRow[x] = (src[x] == transparent) ? 255 : src[x];
        } else {
          memcpy(grayRow, src, w);
        }
      } else if (ctx.bitDepth == 16) {
        const uint8_t transparent = static_cast<uint8_t>(ctx.transparentGray >> 8);
        for (uint32_t x = 0; x < w; ++x) {
          const uint8_t gray = src[x * 2];
          grayRow[x] = (ctx.hasGrayTransparent && gray == transparent) ? 255 : gray;
        }
      } else {
        const int ppb = 8 / ctx.bitDepth;
        const uint8_t mask = (1 << ctx.bitDepth) - 1;
        const uint8_t transparentRaw = static_cast<uint8_t>(ctx.transparentGray & mask);
        for (uint32_t x = 0; x < w; ++x) {
          int shift = (ppb - 1 - (x % ppb)) * ctx.bitDepth;
          const uint8_t raw = (src[x / ppb] >> shift) & mask;
          const uint8_t gray = static_cast<uint8_t>(raw * 255 / mask);
          grayRow[x] = (ctx.hasGrayTransparent && raw == transparentRaw) ? 255 : gray;
        }
      }
      break;

    case PNG_COLOR_RGB:
      if (ctx.bitDepth == 8) {
        const uint8_t tr = static_cast<uint8_t>(ctx.transparentRed >> 8);
        const uint8_t tg = static_cast<uint8_t>(ctx.transparentGreen >> 8);
        const uint8_t tb = static_cast<uint8_t>(ctx.transparentBlue >> 8);
        for (uint32_t x = 0; x < w; ++x) {
          const uint8_t* p = src + x * 3;
          if (ctx.hasRgbTransparent && p[0] == tr && p[1] == tg && p[2] == tb) {
            grayRow[x] = 255;
          } else {
            grayRow[x] = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          }
        }
      } else {
        const uint8_t tr = static_cast<uint8_t>(ctx.transparentRed >> 8);
        const uint8_t tg = static_cast<uint8_t>(ctx.transparentGreen >> 8);
        const uint8_t tb = static_cast<uint8_t>(ctx.transparentBlue >> 8);
        for (uint32_t x = 0; x < w; ++x) {
          const uint8_t* p = src + x * 6;
          if (ctx.hasRgbTransparent && p[0] == tr && p[2] == tg && p[4] == tb) {
            grayRow[x] = 255;
          } else {
            grayRow[x] = static_cast<uint8_t>((p[0] * 77 + p[2] * 150 + p[4] * 29) >> 8);
          }
        }
      }
      break;

    case PNG_COLOR_PALETTE: {
      const int ppb = 8 / ctx.bitDepth;
      const uint8_t mask = (1 << ctx.bitDepth) - 1;
      for (uint32_t x = 0; x < w; ++x) {
        int shift = (ppb - 1 - (x % ppb)) * ctx.bitDepth;
        uint8_t idx = (src[x / ppb] >> shift) & mask;
        if (idx >= ctx.paletteSize) idx = 0;
        const uint8_t* p = &ctx.palette[idx * 3];
        uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        if (ctx.hasPaletteAlpha) gray = blendOverWhite(gray, ctx.paletteAlpha[idx]);
        grayRow[x] = gray;
      }
      break;
    }

    case PNG_COLOR_GRAYSCALE_ALPHA:
      if (ctx.bitDepth == 8) {
        for (uint32_t x = 0; x < w; ++x) {
          grayRow[x] = blendOverWhite(src[x * 2], src[x * 2 + 1]);
        }
      } else {
        for (uint32_t x = 0; x < w; ++x) {
          grayRow[x] = blendOverWhite(src[x * 4], src[x * 4 + 2]);
        }
      }
      break;

    case PNG_COLOR_RGBA:
      if (ctx.bitDepth == 8) {
        for (uint32_t x = 0; x < w; ++x) {
          const uint8_t* p = src + x * 4;
          const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          grayRow[x] = blendOverWhite(gray, p[3]);
        }
      } else {
        for (uint32_t x = 0; x < w; ++x) {
          const uint8_t* p = src + x * 8;
          const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[2] * 150 + p[4] * 29) >> 8);
          grayRow[x] = blendOverWhite(gray, p[6]);
        }
      }
      break;

    default:
      memset(grayRow, 128, w);
      break;
  }
}

bool emitRenderedRow(GfxRenderer& renderer, const RenderConfig& config, const int outWidth, const int outHeight,
                     const uint8_t* grayRow, const int outY, PixelCache* cache, bool* cachingActive,
                     AtkinsonDitherer* ditherer) {
  const int screenWidth = renderer.getScreenWidth();
  if (outY < 0 || outY >= outHeight) return true;

  const int screenY = config.y + outY;
  DirectPixelWriter pw;
  pw.init(renderer);
  pw.beginRow(screenY);

  DirectCacheWriter cw;
  bool caching = cache && cachingActive && *cachingActive;
  if (caching) {
    if (!cache->advanceTo(outY)) {
      *cachingActive = false;
      caching = false;
    } else {
      cw.init(cache->buffer, cache->bytesPerRow, cache->bandRows, cache->originX);
      cw.beginRow(screenY, config.y + cache->bandStart);
    }
  }

  for (int outX = 0; outX < outWidth; ++outX) {
    const int screenX = config.x + outX;
    uint8_t level;
    if (ditherer) {
      level = ditherer->processPixel(grayRow[outX], outX);
    } else if (config.useDithering) {
      level = applyBayerDither4Level(grayRow[outX], screenX, screenY);
    } else {
      level = grayRow[outX] / 85;
      if (level > 3) level = 3;
    }

    if (screenX >= 0 && screenX < screenWidth) {
      pw.writePixel(screenX, level);
    }
    if (caching) {
      cw.writePixel(screenX, level);
    }
  }

  if (ditherer) ditherer->nextRow();

  return true;
}

bool decodeToFramebufferStreaming(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) {
  FsFile pngFile;
  if (!Storage.openFileForRead("PNG", imagePath, pngFile)) {
    LOG_ERR("PNG", "Failed to open PNG for streaming decode: %s", imagePath.c_str());
    return false;
  }

  uint8_t sig[8];
  if (pngFile.read(sig, 8) != 8 || memcmp(sig, PNG_SIGNATURE, 8) != 0) {
    LOG_ERR("PNG", "Invalid PNG signature: %s", imagePath.c_str());
    pngFile.close();
    return false;
  }

  uint32_t ihdrLen;
  if (!readBE32(pngFile, ihdrLen)) {
    pngFile.close();
    return false;
  }
  uint8_t ihdrType[4];
  if (pngFile.read(ihdrType, 4) != 4 || memcmp(ihdrType, "IHDR", 4) != 0) {
    LOG_ERR("PNG", "Missing IHDR chunk: %s", imagePath.c_str());
    pngFile.close();
    return false;
  }

  uint32_t width, height;
  if (!readBE32(pngFile, width) || !readBE32(pngFile, height)) {
    pngFile.close();
    return false;
  }

  uint8_t ihdrRest[5];
  if (pngFile.read(ihdrRest, 5) != 5) {
    pngFile.close();
    return false;
  }

  const uint8_t bitDepth = ihdrRest[0];
  const uint8_t colorType = ihdrRest[1];
  const uint8_t compression = ihdrRest[2];
  const uint8_t filter = ihdrRest[3];
  const uint8_t interlace = ihdrRest[4];
  pngFile.seekCur(4);  // IHDR CRC

  if (!validatePngDimensionsLocal(static_cast<int>(width), static_cast<int>(height))) {
    pngFile.close();
    return false;
  }
  if (compression != 0 || filter != 0) {
    LOG_ERR("PNG", "Unsupported PNG compression/filter method");
    pngFile.close();
    return false;
  }
  if (interlace != 0) {
    LOG_ERR("PNG", "Interlaced PNGs are not supported by the low-memory decoder: %s", imagePath.c_str());
    pngFile.close();
    return false;
  }

  uint8_t bytesPerPixel;
  uint32_t rawRowBytes;
  switch (colorType) {
    case PNG_COLOR_GRAYSCALE:
      if (bitDepth == 16) {
        bytesPerPixel = 2;
        rawRowBytes = width * 2;
      } else if (bitDepth == 8) {
        bytesPerPixel = 1;
        rawRowBytes = width;
      } else {
        bytesPerPixel = 1;
        rawRowBytes = (width * bitDepth + 7) / 8;
      }
      break;
    case PNG_COLOR_RGB:
      bytesPerPixel = (bitDepth == 16) ? 6 : 3;
      rawRowBytes = width * bytesPerPixel;
      break;
    case PNG_COLOR_PALETTE:
      bytesPerPixel = 1;
      rawRowBytes = (width * bitDepth + 7) / 8;
      break;
    case PNG_COLOR_GRAYSCALE_ALPHA:
      bytesPerPixel = (bitDepth == 16) ? 4 : 2;
      rawRowBytes = width * bytesPerPixel;
      break;
    case PNG_COLOR_RGBA:
      bytesPerPixel = (bitDepth == 16) ? 8 : 4;
      rawRowBytes = width * bytesPerPixel;
      break;
    default:
      LOG_ERR("PNG", "Unsupported PNG color type: %u", colorType);
      pngFile.close();
      return false;
  }

  if (rawRowBytes == 0 || rawRowBytes > 16384) {
    LOG_ERR("PNG", "Unsupported PNG row size: %u", rawRowBytes);
    pngFile.close();
    return false;
  }

  StreamingPngContext ctx{};
  ctx.file = &pngFile;
  ctx.width = width;
  ctx.height = height;
  ctx.bitDepth = bitDepth;
  ctx.colorType = colorType;
  ctx.bytesPerPixel = bytesPerPixel;
  ctx.rawRowBytes = rawRowBytes;
  memset(ctx.paletteAlpha, 255, sizeof(ctx.paletteAlpha));

  ctx.currentRow = static_cast<uint8_t*>(malloc(rawRowBytes));
  ctx.previousRow = static_cast<uint8_t*>(calloc(rawRowBytes, 1));
  if (!ctx.currentRow || !ctx.previousRow) {
    LOG_ERR("PNG", "OOM: streaming scanline buffers (%u bytes each)", rawRowBytes);
    free(ctx.currentRow);
    free(ctx.previousRow);
    pngFile.close();
    return false;
  }

  bool foundIdat = false;
  while (!foundIdat) {
    uint32_t chunkLen;
    if (!readBE32(pngFile, chunkLen)) break;

    uint8_t chunkType[4];
    if (pngFile.read(chunkType, 4) != 4) break;

    if (memcmp(chunkType, "PLTE", 4) == 0) {
      const int entries = std::min<int>(256, chunkLen / 3);
      ctx.paletteSize = entries;
      const size_t palBytes = static_cast<size_t>(entries) * 3U;
      if (palBytes && pngFile.read(ctx.palette, palBytes) != static_cast<int>(palBytes)) break;
      if (chunkLen > palBytes) pngFile.seekCur(chunkLen - palBytes);
      pngFile.seekCur(4);
    } else if (memcmp(chunkType, "tRNS", 4) == 0) {
      if (colorType == PNG_COLOR_PALETTE) {
        const int count = std::min<int>(256, chunkLen);
        if (count && pngFile.read(ctx.paletteAlpha, count) != count) break;
        if (chunkLen > static_cast<uint32_t>(count)) pngFile.seekCur(chunkLen - count);
        ctx.hasPaletteAlpha = true;
      } else if (colorType == PNG_COLOR_GRAYSCALE && chunkLen >= 2) {
        uint8_t buf[2];
        if (pngFile.read(buf, 2) != 2) break;
        readBE16Buf(buf, ctx.transparentGray);
        if (chunkLen > 2) pngFile.seekCur(chunkLen - 2);
        ctx.hasGrayTransparent = true;
      } else if (colorType == PNG_COLOR_RGB && chunkLen >= 6) {
        uint8_t buf[6];
        if (pngFile.read(buf, 6) != 6) break;
        readBE16Buf(buf, ctx.transparentRed);
        readBE16Buf(buf + 2, ctx.transparentGreen);
        readBE16Buf(buf + 4, ctx.transparentBlue);
        if (chunkLen > 6) pngFile.seekCur(chunkLen - 6);
        ctx.hasRgbTransparent = true;
      } else {
        pngFile.seekCur(chunkLen);
      }
      pngFile.seekCur(4);
    } else if (memcmp(chunkType, "IDAT", 4) == 0) {
      ctx.chunkBytesRemaining = chunkLen;
      foundIdat = true;
    } else if (memcmp(chunkType, "IEND", 4) == 0) {
      break;
    } else {
      pngFile.seekCur(chunkLen + 4);
    }
  }

  if (!foundIdat) {
    LOG_ERR("PNG", "No IDAT chunk found: %s", imagePath.c_str());
    free(ctx.currentRow);
    free(ctx.previousRow);
    pngFile.close();
    return false;
  }

  if (!ctx.reader.init(true)) {
    LOG_ERR("PNG", "Failed to init streaming inflate reader (needs 32KB DEFLATE history)");
    free(ctx.currentRow);
    free(ctx.previousRow);
    pngFile.close();
    return false;
  }
  ctx.reader.setReadCallback(pngIdatReadCallback);
  ctx.reader.skipZlibHeader();

  const StreamingGeometry geometry = calculateGeometry(width, height, config);
  LOG_INF("PNG", "Streaming PNG decode: %s (%ux%u -> %dx%d, free=%u maxAlloc=%u row=%u)", imagePath.c_str(), width,
          height, geometry.outWidth, geometry.outHeight, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), rawRowBytes);

  PixelCache cache;
  bool cachingActive = !config.cachePath.empty();
  if (cachingActive) {
    if (!cache.begin(config.cachePath, geometry.outWidth, geometry.outHeight, config.x, config.y, 1)) {
      LOG_ERR("PNG", "Failed to start cache stream, continuing without caching");
      cachingActive = false;
    }
  }

  uint8_t* grayRow = static_cast<uint8_t*>(malloc(width));
  uint8_t* scaledRow = static_cast<uint8_t*>(malloc(geometry.outWidth));
  uint32_t* rowAccum = geometry.needsScaling ? static_cast<uint32_t*>(calloc(geometry.outWidth, sizeof(uint32_t))) : nullptr;
  uint16_t* rowCount = geometry.needsScaling ? static_cast<uint16_t*>(calloc(geometry.outWidth, sizeof(uint16_t))) : nullptr;

  if (!grayRow || !scaledRow || (geometry.needsScaling && (!rowAccum || !rowCount))) {
    LOG_ERR("PNG", "OOM: streaming grayscale/scaling buffers");
    if (cachingActive) cache.abort();
    free(grayRow);
    free(scaledRow);
    free(rowAccum);
    free(rowCount);
    free(ctx.currentRow);
    free(ctx.previousRow);
    pngFile.close();
    return false;
  }

  // Error-diffusion dithering needs one row of error state per output column.
  // Allocation failure is not fatal: emitRenderedRow falls back to ordered
  // (Bayer) dithering whenever ditherer is null.
  AtkinsonDitherer* ditherer =
      config.useDithering ? new (std::nothrow) AtkinsonDitherer(geometry.outWidth) : nullptr;

  bool success = true;
  int currentOutY = 0;
  uint32_t nextOutYBoundary_fp = geometry.scaleY_fp;

  for (uint32_t y = 0; y < height; ++y) {
    if (!decodeScanline(ctx)) {
      LOG_ERR("PNG", "Failed to decode PNG scanline %u", y);
      success = false;
      break;
    }

    convertStreamingScanlineToGray(ctx, grayRow);

    if (!geometry.needsScaling) {
      memcpy(scaledRow, grayRow, geometry.outWidth);
      if (!emitRenderedRow(renderer, config, geometry.outWidth, geometry.outHeight, scaledRow, currentOutY, &cache,
                           &cachingActive, ditherer)) {
        success = false;
        break;
      }
      currentOutY++;
    } else {
      for (int outX = 0; outX < geometry.outWidth; ++outX) {
        const uint64_t srcXStart_fp = static_cast<uint64_t>(outX) * geometry.scaleX_fp;
        const uint64_t srcXEnd_fp = static_cast<uint64_t>(outX + 1) * geometry.scaleX_fp;
        const int srcXStart = std::min<int>(static_cast<int>(width) - 1, static_cast<int>(srcXStart_fp >> 16));
        const int srcXEnd = std::min<int>(static_cast<int>(width), static_cast<int>(srcXEnd_fp >> 16));

        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < static_cast<int>(width); ++srcX) {
          sum += grayRow[srcX];
          count++;
        }
        if (count == 0 && srcXStart < static_cast<int>(width)) {
          sum = grayRow[srcXStart];
          count = 1;
        }

        rowAccum[outX] += static_cast<uint32_t>(sum);
        rowCount[outX] += static_cast<uint16_t>(count);
      }

      const uint64_t srcY_fp = static_cast<uint64_t>(y + 1) << 16;
      while (srcY_fp >= nextOutYBoundary_fp && currentOutY < geometry.outHeight) {
        for (int outX = 0; outX < geometry.outWidth; ++outX) {
          scaledRow[outX] = (rowCount[outX] > 0) ? static_cast<uint8_t>(rowAccum[outX] / rowCount[outX]) : 255;
        }

        if (!emitRenderedRow(renderer, config, geometry.outWidth, geometry.outHeight, scaledRow, currentOutY, &cache,
                             &cachingActive, ditherer)) {
          success = false;
          break;
        }

        currentOutY++;
        nextOutYBoundary_fp = static_cast<uint32_t>(static_cast<uint64_t>(currentOutY + 1) * geometry.scaleY_fp);
        if (srcY_fp >= nextOutYBoundary_fp) {
          continue;
        }
        memset(rowAccum, 0, geometry.outWidth * sizeof(uint32_t));
        memset(rowCount, 0, geometry.outWidth * sizeof(uint16_t));
      }
      if (!success) break;
    }

    uint8_t* temp = ctx.previousRow;
    ctx.previousRow = ctx.currentRow;
    ctx.currentRow = temp;
  }

  if (success && geometry.needsScaling && currentOutY < geometry.outHeight) {
    for (; currentOutY < geometry.outHeight; ++currentOutY) {
      for (int outX = 0; outX < geometry.outWidth; ++outX) {
        scaledRow[outX] = (rowCount && rowCount[outX] > 0) ? static_cast<uint8_t>(rowAccum[outX] / rowCount[outX]) : 255;
      }
      if (!emitRenderedRow(renderer, config, geometry.outWidth, geometry.outHeight, scaledRow, currentOutY, &cache,
                           &cachingActive, ditherer)) {
        success = false;
        break;
      }
    }
  }

  if (success && cachingActive) {
    cache.finalize();
  } else if (cachingActive) {
    cache.abort();
  }

  free(grayRow);
  free(scaledRow);
  free(rowAccum);
  free(rowCount);
  delete ditherer;
  free(ctx.currentRow);
  free(ctx.previousRow);
  pngFile.close();

  if (!success) return false;
  LOG_DBG("PNG", "Streaming PNG decode complete");
  return true;
}

}  // namespace

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  FsFile file;
  if (!Storage.openFileForRead("PNG", imagePath, file)) return false;

  uint8_t header[24];
  const int bytesRead = file.read(header, sizeof(header));
  file.close();
  static constexpr uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (bytesRead != static_cast<int>(sizeof(header)) || memcmp(header, signature, sizeof(signature)) != 0 ||
      memcmp(header + 12, "IHDR", 4) != 0) {
    LOG_ERR("PNG", "Invalid or truncated PNG header: %s", imagePath.c_str());
    return false;
  }

  const uint32_t width = (static_cast<uint32_t>(header[16]) << 24) | (static_cast<uint32_t>(header[17]) << 16) |
                         (static_cast<uint32_t>(header[18]) << 8) | header[19];
  const uint32_t height = (static_cast<uint32_t>(header[20]) << 24) | (static_cast<uint32_t>(header[21]) << 16) |
                          (static_cast<uint32_t>(header[22]) << 8) | header[23];
  if (width == 0 || height == 0 || width > 32767 || height > 32767) {
    LOG_ERR("PNG", "Invalid PNG dimensions: %ux%u", width, height);
    return false;
  }
  out.width = static_cast<int>(width);
  out.height = static_cast<int>(height);
  return true;
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("PNG", "Decoding PNG: %s", imagePath.c_str());

  if (decodeToFramebufferWithPngDec(imagePath, renderer, config)) {
    return true;
  }

  LOG_INF("PNG", "Falling back to low-memory streaming PNG decoder: %s", imagePath.c_str());
  return decodeToFramebufferStreaming(imagePath, renderer, config);
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasPngExtension(extension);
}
