#include "ImageBlock.h"

#include <Fb2.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <Serialization.h>

#include <algorithm>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    cacheFile.close();
    return false;
  }

  // A FB2 PNG may be pre-cached before CSS/layout starts, at its maximum
  // screen-fitting size. If a structural container later makes the final
  // ImageBlock a little smaller, scale the already-decoded 2-bit cache instead
  // of throwing it away and invoking PNGdec again on a fragmented heap.
  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  const bool scaleCachedImage = widthDiff > 1 || heightDiff > 1;
  if (expectedWidth <= 0 || expectedHeight <= 0 || cachedWidth == 0 || cachedHeight == 0) {
    cacheFile.close();
    return false;
  }

  LOG_DBG("IMG", scaleCachedImage ? "Loading/scaling cache: %s (%dx%d -> %dx%d)"
                                  : "Loading from cache: %s (%dx%d)",
          cachePath.c_str(), cachedWidth, cachedHeight, expectedWidth, expectedHeight);

  if (scaleCachedImage) {
    const int screenWidth = renderer.getScreenWidth();
    const int screenHeight = renderer.getScreenHeight();
    int clipXStart = std::max(0, -x);
    int clipYStart = std::max(0, -y);
    int clipXEnd = std::min(expectedWidth, screenWidth - x);
    int clipYEnd = std::min(expectedHeight, screenHeight - y);
    if (clipXStart >= clipXEnd || clipYStart >= clipYEnd) {
      cacheFile.close();
      return true;
    }

    const int srcBytesPerRow = (cachedWidth + 3) / 4;
    uint8_t* srcRow = static_cast<uint8_t*>(malloc(srcBytesPerRow));
    if (!srcRow) {
      LOG_ERR("IMG", "Failed to allocate scaled-cache row buffer");
      cacheFile.close();
      return false;
    }

    DirectPixelWriter pw;
    pw.init(renderer);
    int loadedSrcY = -1;
    for (int dstY = clipYStart; dstY < clipYEnd; ++dstY) {
      const int srcY = static_cast<int>((static_cast<int64_t>(dstY) * cachedHeight) / expectedHeight);
      if (srcY != loadedSrcY) {
        const uint32_t offset = 4U + static_cast<uint32_t>(srcY) * static_cast<uint32_t>(srcBytesPerRow);
        if (!cacheFile.seek(offset) || cacheFile.read(srcRow, srcBytesPerRow) != srcBytesPerRow) {
          LOG_ERR("IMG", "Scaled cache read error at source row %d", srcY);
          free(srcRow);
          cacheFile.close();
          return false;
        }
        loadedSrcY = srcY;
      }

      pw.beginRow(y + dstY);
      for (int dstX = clipXStart; dstX < clipXEnd; ++dstX) {
        const int srcX = static_cast<int>((static_cast<int64_t>(dstX) * cachedWidth) / expectedWidth);
        const int byteIdx = srcX >> 2;
        const int bitShift = 6 - (srcX & 3) * 2;
        pw.writePixel(x + dstX, (srcRow[byteIdx] >> bitShift) & 0x03);
      }
    }
    free(srcRow);
    cacheFile.close();
    return true;
  }

  // Use cached dimensions for the fast 1:1 rendering path.
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int clipXStart = 0;
  int clipYStart = 0;
  int clipXEnd = cachedWidth;
  int clipYEnd = cachedHeight;
  if (x < 0) clipXStart = -x;
  if (y < 0) clipYStart = -y;
  if (screenWidth - x < clipXEnd) clipXEnd = screenWidth - x;
  if (screenHeight - y < clipYEnd) clipYEnd = screenHeight - y;

  if (clipXStart >= clipXEnd || clipYStart >= clipYEnd) {
    LOG_DBG("IMG", "Cached image is outside screen after clipping");
    cacheFile.close();
    return true;
  }

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    cacheFile.close();
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        cacheFile.close();
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    if (row < clipYStart) continue;
    if (row >= clipYEnd) break;

    const int destY = y + row;
    pw.beginRow(destY);
    // Walk only the on-screen columns: writePixel drops off-band rows but does
    // not clip X, so this range is what keeps a partially off-screen image
    // inside the framebuffer.
    for (int col = clipXStart; col < clipXEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  cacheFile.close();
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;
  if (unavailableThisSection) return;
  if (deferredByLowHeap) {
    const auto heap = MemoryBudget::snapshot();
    if (heap.freeHeap < retryWhenFreeAtLeast || heap.maxAllocHeap < retryWhenMaxAllocAtLeast) {
      return;
    }
    LOG_DBG("IMG", "Retrying deferred image decode after heap recovery: %s (free=%u maxAlloc=%u)",
            imagePath.c_str(), heap.freeHeap, heap.maxAllocHeap);
    deferredByLowHeap = false;
    retryWhenFreeAtLeast = 0;
    retryWhenMaxAllocAtLeast = 0;
  }

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  if (width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Invalid image size: %dx%d", width, height);
    return;
  }

  // Reject only fully off-screen images. Decoders and cache rendering clip
  // partially visible images to the logical screen bounds.
  if (x >= screenWidth || y >= screenHeight || x + width <= 0 || y + height <= 0) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }
  const bool fullyOnScreen = x >= 0 && y >= 0 && x + width <= screenWidth && y + height <= screenHeight;

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }

  // No cache - need to decode the image
  // FB2-origin packages don't decode images at load time either (see
  // Fb2::persistImageIndex()/decodeImageOnDemand()) - the raw file below
  // may not exist yet the first time this image is actually rendered. A
  // no-op for any path outside an FB2-origin package (checked internally
  // via a marker file), so this is safe to call unconditionally.
  if (!Storage.exists(imagePath.c_str())) {
    Fb2::decodeImageOnDemand(imagePath);
  }

  // Check if image file exists
  FsFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    unavailableThisSection = true;
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    unavailableThisSection = true;
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  if (fullyOnScreen) {
    config.cachePath = cachePath;  // Enable caching during decode
  }

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    unavailableThisSection = true;
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    const auto heap = MemoryBudget::snapshot();
    const bool likelyLowHeapPng =
        FsHelpers::hasPngExtension(imagePath) && !Storage.exists(cachePath.c_str()) &&
        (heap.freeHeap < 56U * 1024U || heap.maxAllocHeap < 40U * 1024U);
    if (likelyLowHeapPng) {
      deferredByLowHeap = true;
      retryWhenFreeAtLeast = heap.freeHeap + 8U * 1024U;
      retryWhenMaxAllocAtLeast = heap.maxAllocHeap + 4U * 1024U;
      LOG_ERR("IMG",
              "Deferred PNG decode until heap improves: %s (free=%u maxAlloc=%u retry>=%u/%u)",
              imagePath.c_str(), heap.freeHeap, heap.maxAllocHeap, retryWhenFreeAtLeast,
              retryWhenMaxAllocAtLeast);
      return;
    }

    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    unavailableThisSection = true;
    return;
  }

  deferredByLowHeap = false;
  retryWhenFreeAtLeast = 0;
  retryWhenMaxAllocAtLeast = 0;
  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(FsFile& file) {
  return serialization::tryWriteString(file, imagePath) && serialization::tryWritePod(file, width) &&
         serialization::tryWritePod(file, height);
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile& file) {
  std::string path;
  if (!serialization::tryReadString(file, path)) {
    LOG_ERR("IMG", "Deserialization failed: could not read image path");
    return nullptr;
  }
  int16_t w, h;
  if (!serialization::tryReadPod(file, w) || !serialization::tryReadPod(file, h)) {
    LOG_ERR("IMG", "Deserialization failed: truncated image metadata");
    return nullptr;
  }

  auto* imageBlock = new (std::nothrow) ImageBlock(path, w, h);
  if (!imageBlock) {
    LOG_ERR("IMG", "Deserialization failed: could not allocate ImageBlock");
    return nullptr;
  }
  return std::unique_ptr<ImageBlock>(imageBlock);
}
