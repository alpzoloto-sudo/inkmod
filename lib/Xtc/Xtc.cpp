/**
 * Xtc.cpp
 *
 * Main XTC ebook class implementation
 * XTC ebook support for InkMOD Reader
 */

#include "Xtc.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {
bool thumbnailHasDimensions(const std::string& path, const uint16_t width, const uint16_t height) {
  FsFile file;
  if (!Storage.openFileForRead("XTC", path, file)) {
    return false;
  }

  Bitmap bitmap(file);
  const bool matches =
      bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() == width && bitmap.getHeight() == height;
  file.close();
  return matches;
}

// Convert the first XTC/XTCH page directly into a 1-bit BMP without ever
// allocating the complete 48/96-KB source bitmap. For XTCH, plane 1 is staged
// to a temporary SD file while plane 2 is streamed; matching 2-KB chunks are
// then combined into the small destination bitmap.
bool generatePageBmpStreaming(xtc::XtcParser& parser, const xtc::PageInfo& pageInfo, const uint8_t bitDepth,
                              const std::string& outputPath, const uint16_t outputWidth,
                              const uint16_t outputHeight, const bool ditherGrayscale) {
  if (pageInfo.width == 0 || pageInfo.height == 0 || outputWidth == 0 || outputHeight == 0) return false;

  const size_t outputRowSize = ((static_cast<size_t>(outputWidth) + 31) / 32) * 4;
  const size_t outputBytes = outputRowSize * outputHeight;
  uint8_t* output = static_cast<uint8_t*>(malloc(outputBytes));
  int16_t* sourceToOutputX = static_cast<int16_t*>(malloc(static_cast<size_t>(pageInfo.width) * sizeof(int16_t)));
  int16_t* sourceToOutputY = static_cast<int16_t*>(malloc(static_cast<size_t>(pageInfo.height) * sizeof(int16_t)));
  if (!output || !sourceToOutputX || !sourceToOutputY) {
    free(output);
    free(sourceToOutputX);
    free(sourceToOutputY);
    LOG_ERR("XTC", "Failed to allocate streaming BMP buffers (%lu bytes)",
            static_cast<unsigned long>(outputBytes +
                                       (static_cast<size_t>(pageInfo.width) + pageInfo.height) * sizeof(int16_t)));
    return false;
  }

  memset(output, 0xFF, outputBytes);
  std::fill_n(sourceToOutputX, pageInfo.width, static_cast<int16_t>(-1));
  std::fill_n(sourceToOutputY, pageInfo.height, static_cast<int16_t>(-1));

  const float scaleX = static_cast<float>(outputWidth) / pageInfo.width;
  const float scaleY = static_cast<float>(outputHeight) / pageInfo.height;
  const float scale = std::max(scaleX, scaleY);
  const uint32_t scaleInvFp = static_cast<uint32_t>(65536.0f / scale);
  const uint64_t sourceWidthFp = static_cast<uint64_t>(pageInfo.width) << 16;
  const uint64_t sourceHeightFp = static_cast<uint64_t>(pageInfo.height) << 16;
  const uint64_t visibleWidthFp = static_cast<uint64_t>(outputWidth) * scaleInvFp;
  const uint64_t visibleHeightFp = static_cast<uint64_t>(outputHeight) * scaleInvFp;
  const uint32_t cropXFp =
      static_cast<uint32_t>(sourceWidthFp > visibleWidthFp ? (sourceWidthFp - visibleWidthFp) / 2 : 0);
  const uint32_t cropYFp =
      static_cast<uint32_t>(sourceHeightFp > visibleHeightFp ? (sourceHeightFp - visibleHeightFp) / 2 : 0);

  // Destination pixels sample the centre of their source footprint. UI cover
  // slots never upscale an XTC page, so each selected source coordinate maps
  // to at most one destination coordinate.
  for (uint16_t x = 0; x < outputWidth; ++x) {
    uint32_t sourceX =
        (cropXFp + static_cast<uint32_t>(x) * scaleInvFp + scaleInvFp / 2) >> 16;
    if (sourceX >= pageInfo.width) sourceX = pageInfo.width - 1;
    sourceToOutputX[sourceX] = static_cast<int16_t>(x);
  }
  for (uint16_t y = 0; y < outputHeight; ++y) {
    uint32_t sourceY =
        (cropYFp + static_cast<uint32_t>(y) * scaleInvFp + scaleInvFp / 2) >> 16;
    if (sourceY >= pageInfo.height) sourceY = pageInfo.height - 1;
    sourceToOutputY[sourceY] = static_cast<int16_t>(y);
  }

  auto writePixel = [&](const uint16_t sourceX, const uint16_t sourceY, const uint8_t gray) {
    if (sourceX >= pageInfo.width || sourceY >= pageInfo.height) return;
    const int16_t outputX = sourceToOutputX[sourceX];
    const int16_t outputY = sourceToOutputY[sourceY];
    if (outputX < 0 || outputY < 0) return;

    // Full-size sleep cover keeps the reader's historical threshold: only
    // native white stays white, every visible gray/black pixel becomes black.
    // Small UI thumbnails use deterministic dithering to preserve gray detail.
    bool white = ditherGrayscale ? gray >= 128 : gray == 255;
    if (ditherGrayscale && gray != 0 && gray != 255) {
      uint32_t hash = static_cast<uint32_t>(outputX) * 374761393u +
                      static_cast<uint32_t>(outputY) * 668265263u;
      hash = (hash ^ (hash >> 13)) * 1274126177u;
      const int threshold = static_cast<int>(hash >> 24);
      const int adjustedThreshold = 128 + ((threshold - 128) / 2);
      white = gray >= adjustedThreshold;
    }
    if (!white) {
      output[static_cast<size_t>(outputY) * outputRowSize + static_cast<size_t>(outputX) / 8] &=
          static_cast<uint8_t>(~(1u << (7 - (outputX % 8))));
    }
  };

  bool streamOk = true;
  const size_t sourceRowBytes = (pageInfo.width + 7) / 8;
  const size_t planeSize = (static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8;
  const size_t columnBytes = (pageInfo.height + 7) / 8;
  constexpr size_t STREAM_CHUNK_SIZE = 2048;
  const std::string planePath = outputPath + ".plane.tmp";
  FsFile planeFile;
  uint8_t* planeChunk = nullptr;
  bool readingPlane2 = false;

  if (bitDepth == 2) {
    Storage.remove(planePath.c_str());
    planeChunk = static_cast<uint8_t*>(malloc(STREAM_CHUNK_SIZE));
    streamOk = planeChunk && Storage.openFileForWrite("XTC", planePath, planeFile);
  }

  const auto error = streamOk
                         ? parser.loadPageStreaming(
                               0,
                               [&](const uint8_t* data, const size_t size, const size_t offset) {
                                 if (!streamOk) return;
                                 size_t position = 0;
                                 while (position < size) {
                                   const size_t absolute = offset + position;
                                   if (bitDepth == 1) {
                                     const size_t byteOffset = absolute;
                                     const uint16_t sourceY = static_cast<uint16_t>(byteOffset / sourceRowBytes);
                                     const uint16_t byteInRow = static_cast<uint16_t>(byteOffset % sourceRowBytes);
                                     const uint8_t value = data[position++];
                                     for (uint8_t bit = 0; bit < 8; ++bit) {
                                       const uint16_t sourceX = static_cast<uint16_t>(byteInRow * 8 + bit);
                                       if (sourceX >= pageInfo.width || sourceY >= pageInfo.height) break;
                                       writePixel(sourceX, sourceY, (value & (1u << (7 - bit))) ? 255 : 0);
                                     }
                                     continue;
                                   }

                                   if (absolute < planeSize) {
                                     const size_t count = std::min(size - position, planeSize - absolute);
                                     if (planeFile.write(data + position, count) != count) {
                                       streamOk = false;
                                       return;
                                     }
                                     position += count;
                                     continue;
                                   }
                                   if (absolute >= planeSize * 2) break;

                                   if (!readingPlane2) {
                                     planeFile.flush();
                                     planeFile.close();
                                     readingPlane2 = Storage.openFileForRead("XTC", planePath, planeFile);
                                     if (!readingPlane2) {
                                       streamOk = false;
                                       return;
                                     }
                                   }

                                   const size_t plane2Offset = absolute - planeSize;
                                   const size_t count = std::min({size - position, planeSize - plane2Offset,
                                                                  STREAM_CHUNK_SIZE});
                                   if (!planeFile.seek(plane2Offset) || planeFile.read(planeChunk, count) != count) {
                                     streamOk = false;
                                     return;
                                   }

                                   for (size_t i = 0; i < count; ++i) {
                                     const size_t byteOffset = plane2Offset + i;
                                     const size_t columnIndex = byteOffset / columnBytes;
                                     const size_t byteInColumn = byteOffset % columnBytes;
                                     if (columnIndex >= pageInfo.width) continue;
                                     const uint16_t sourceX =
                                         static_cast<uint16_t>(pageInfo.width - 1 - columnIndex);
                                     const uint8_t first = planeChunk[i];
                                     const uint8_t second = data[position + i];
                                     for (uint8_t bit = 0; bit < 8; ++bit) {
                                       const uint16_t sourceY = static_cast<uint16_t>(byteInColumn * 8 + bit);
                                       if (sourceY >= pageInfo.height) break;
                                       const uint8_t shift = static_cast<uint8_t>(7 - bit);
                                       const uint8_t pixel = static_cast<uint8_t>(
                                           ((((first >> shift) & 1u) << 1) | ((second >> shift) & 1u)));
                                       writePixel(sourceX, sourceY, static_cast<uint8_t>((3 - pixel) * 85));
                                     }
                                   }
                                   position += count;
                                 }
                               },
                               STREAM_CHUNK_SIZE)
                         : xtc::XtcError::MEMORY_ERROR;

  if (planeFile) planeFile.close();
  if (bitDepth == 2) Storage.remove(planePath.c_str());
  free(planeChunk);
  free(sourceToOutputX);
  free(sourceToOutputY);

  if (error != xtc::XtcError::OK || !streamOk) {
    free(output);
    Storage.remove(outputPath.c_str());
    LOG_ERR("XTC", "Streaming BMP source read failed: %s", xtc::errorToString(error));
    return false;
  }

  FsFile bmp;
  if (!Storage.openFileForWrite("XTC", outputPath, bmp)) {
    free(output);
    return false;
  }
  BmpHeader header;
  createBmpHeader(&header, outputWidth, outputHeight, BmpRowOrder::TopDown);
  const bool written = bmp.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
                       bmp.write(output, outputBytes) == outputBytes;
  bmp.close();
  free(output);
  if (!written) Storage.remove(outputPath.c_str());
  return written;
}
}  // namespace

bool Xtc::load() {
  LOG_DBG("XTC", "Loading XTC: %s", filepath.c_str());

  // Initialize parser
  parser.reset(new xtc::XtcParser());

  // Open XTC file
  xtc::XtcError err = parser->open(filepath.c_str());
  if (err != xtc::XtcError::OK) {
    LOG_ERR("XTC", "Failed to load: %s", xtc::errorToString(err));
    parser.reset();
    return false;
  }

  loaded = true;
  LOG_DBG("XTC", "Loaded XTC: %s (%lu pages)", filepath.c_str(), parser->getPageCount());
  return true;
}

bool Xtc::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("XTC", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("XTC", "Failed to clear cache");
    return false;
  }

  LOG_DBG("XTC", "Cache cleared successfully");
  return true;
}

void Xtc::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  // Create directories recursively
  for (size_t i = 1; i < cachePath.length(); i++) {
    if (cachePath[i] == '/') {
      Storage.mkdir(cachePath.substr(0, i).c_str());
    }
  }
  Storage.mkdir(cachePath.c_str());
}

std::string Xtc::getTitle() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get title from XTC metadata first
  std::string title = parser->getTitle();
  if (!title.empty()) {
    return title;
  }

  // Fallback: extract filename from path as title
  size_t lastSlash = filepath.find_last_of('/');
  size_t lastDot = filepath.find_last_of('.');

  if (lastSlash == std::string::npos) {
    lastSlash = 0;
  } else {
    lastSlash++;
  }

  if (lastDot == std::string::npos || lastDot <= lastSlash) {
    return filepath.substr(lastSlash);
  }

  return filepath.substr(lastSlash, lastDot - lastSlash);
}

std::string Xtc::getAuthor() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get author from XTC metadata
  return parser->getAuthor();
}

bool Xtc::hasChapters() const {
  if (!loaded || !parser) {
    return false;
  }
  return parser->hasChapters();
}

xtc::ChapterListView Xtc::getChapters() {
  if (!loaded || !parser) {
    return {};
  }
  return parser->getChapters();
}

std::string Xtc::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Xtc::generateCoverBmp() const {
  // Already generated
  if (Storage.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate cover BMP, file not loaded");
    return false;
  }

  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Get first page info for cover
  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }

  const bool generated = generatePageBmpStreaming(*const_cast<xtc::XtcParser*>(parser.get()), pageInfo,
                                                  parser->getBitDepth(), getCoverBmpPath(), pageInfo.width,
                                                  pageInfo.height, false);
  if (generated) {
    LOG_DBG("XTC", "Generated streaming cover BMP: %s", getCoverBmpPath().c_str());
  }
  return generated;
}

std::string Xtc::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Xtc::getThumbBmpPath(uint16_t height) const {
  const uint16_t width = static_cast<uint16_t>(height * 0.6);
  const std::string newPath = getThumbBmpPath(width, height);
  if (Storage.exists(newPath.c_str())) {
    return newPath;
  }

  const std::string legacyPath = cachePath + "/thumb_" + std::to_string(height) + ".bmp";
  if (Storage.exists(legacyPath.c_str())) {
    return legacyPath;
  }

  return newPath;
}
std::string Xtc::getThumbBmpPath(uint16_t width, uint16_t height) const {
  return cachePath + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
}

bool Xtc::generateThumbBmp() const {
  const uint16_t height = getPageHeight();
  return height > 0 && generateThumbBmp(height);
}

bool Xtc::generateThumbBmp(uint16_t height) const {
  return generateThumbBmp(static_cast<uint16_t>(height * 0.6), height);
}

bool Xtc::generateThumbBmp(uint16_t width, uint16_t height) const {
  if (width == 0 || height == 0) {
    LOG_ERR("XTC", "Cannot generate thumb BMP with invalid dimensions: %ux%u", width, height);
    return false;
  }
  const std::string thumbPath = getThumbBmpPath(width, height);
  const bool thumbExists = Storage.exists(thumbPath.c_str());
  if (thumbExists) {
    if (thumbnailHasDimensions(thumbPath, width, height)) {
      return true;
    }
  }

  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate thumb BMP, file not loaded");
    return false;
  }
  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  setupCacheDir();

  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }
  if (pageInfo.width == 0 || pageInfo.height == 0) {
    LOG_ERR("XTC", "Cannot generate thumb BMP with invalid page dimensions: %ux%u", pageInfo.width, pageInfo.height);
    return false;
  }
  if (thumbExists) {
    Storage.remove(thumbPath.c_str());
  }

  const bool generated = generatePageBmpStreaming(*const_cast<xtc::XtcParser*>(parser.get()), pageInfo,
                                                  parser->getBitDepth(), thumbPath, width, height, true);
  if (generated) {
    LOG_DBG("XTC", "Generated streaming thumb BMP (%dx%d): %s", width, height, thumbPath.c_str());
  }
  return generated;
}

uint32_t Xtc::getPageCount() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getPageCount();
}

uint16_t Xtc::getPageWidth() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getWidth();
}

uint16_t Xtc::getPageHeight() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getHeight();
}

uint8_t Xtc::getBitDepth() const {
  if (!loaded || !parser) {
    return 1;  // Default to 1-bit
  }
  return parser->getBitDepth();
}

size_t Xtc::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) const {
  if (!loaded || !parser) {
    return 0;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPage(pageIndex, buffer, bufferSize);
}

xtc::XtcError Xtc::loadPageStreaming(uint32_t pageIndex,
                                     std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                     size_t chunkSize) const {
  if (!loaded || !parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPageStreaming(pageIndex, callback, chunkSize);
}

uint8_t Xtc::calculateProgress(uint32_t currentPage) const {
  if (!loaded || !parser || parser->getPageCount() == 0) {
    return 0;
  }
  return static_cast<uint8_t>((currentPage + 1) * 100 / parser->getPageCount());
}

xtc::XtcError Xtc::getLastError() const {
  if (!parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return parser->getLastError();
}
