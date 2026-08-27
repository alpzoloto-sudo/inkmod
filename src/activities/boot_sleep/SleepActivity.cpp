#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <PNGdec.h>
#include <Epub/converters/PngToFramebufferConverter.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>

#include "../home/RecentBookProgress.h"
#include "../reader/BookStatsView.h"
#include "../reader/EpubReaderActivity.h"
#include "../reader/ReadingStatsUtils.h"
#include "../reader/TxtReaderActivity.h"
#include "../reader/XtcReaderActivity.h"
#include "AppVersion.h"
#include "InkMODSettings.h"
#include "InkMODState.h"
#include "RecentBooksStore.h"
#include "SleepCoverAssets.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "fontIds.h"
#include "images/InkMODLogo120.h"
#include "images/InkMODLogo240.h"
#include "images/MoonIcon.h"

namespace {

void drawInkMODBitmap(const GfxRenderer& renderer, const uint8_t* bitmap, const int width, const int height,
                      const int x, const int y) {
  const int bytesPerRow = (width + 7) / 8;
  for (int row = 0; row < height; ++row) {
    const uint8_t* srcRow = bitmap + row * bytesPerRow;
    int runStart = -1;
    for (int col = 0; col <= width; ++col) {
      const bool black =
          col < width && !((srcRow[col / 8] >> (7 - (col % 8))) & 1);
      if (black && runStart < 0) {
        runStart = col;
      } else if (!black && runStart >= 0) {
        renderer.fillRect(x + runStart, y + row, col - runStart, 1, true);
        runStart = -1;
      }
    }
  }
}


constexpr bool TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH = true;
constexpr int sleepBuildInfoSideMargin = 20;

void hideOverlayBatteryStrip(const GfxRenderer& renderer) {
  if (!SETTINGS.statusBarBattery) {
    return;
  }

  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  const int textY = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - 4;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == InkMODSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  // Reserve the full left-side status indicator lane used by bookmark + battery.
  // This keeps chapter/progress text readable while removing the battery glance target.
  static constexpr int bookmarkReserveWidth = 13;  // bookmark width + gap from BaseTheme::drawStatusBar()
  static constexpr int batteryPercentSpacing = 4;  // matches BaseTheme::batteryPercentSpacing
  const int clearWidth =
      bookmarkReserveWidth + metrics.batteryWidth +
      (showBatteryPercentage ? batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, "100%") : 0);
  const int clearHeight = std::max(renderer.getTextHeight(SMALL_FONT_ID), metrics.batteryHeight + 6);

  renderer.fillRect(metrics.statusBarHorizontalMargin + orientedMarginLeft + 1, textY, clearWidth, clearHeight, false);
}

// Context passed through PNGdec's decode() user-pointer to the per-scanline draw callback.
struct PngOverlayCtx {
  const GfxRenderer* renderer;
  int screenW;
  int screenH;
  int srcWidth;
  int dstWidth;
  int dstX;
  int dstY;
  float yScale;
  int lastDstY;
  // Color-key transparency (tRNS chunk) for TRUECOLOR and GRAYSCALE images.
  // Initialized lazily on the first draw callback because tRNS is processed during decode(),
  // not during open() — so hasAlpha()/getTransparentColor() are only valid once decode() starts.
  // -2 = not yet read; -1 = no color key; >=0 = 0x00RRGGBB (TRUECOLOR) or low-byte gray.
  int32_t transparentColor;
  PNG* pngObj;  // for lazy-init of transparentColor on first callback
};

// PNGdec file I/O callbacks — mirror the pattern in PngToFramebufferConverter.cpp.
void* pngSleepOpen(const char* filename, int32_t* size) {
  FsFile* f = new FsFile();
  if (!Storage.openFileForRead("SLP", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}
void pngSleepClose(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}
int32_t pngSleepRead(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  return f ? f->read(pBuf, len) : 0;
}
int32_t pngSleepSeek(PNGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// Per-scanline draw callback for PNG overlay compositing.
// Transparent pixels (alpha < 128) are skipped so the reader page shows through.
// Opaque pixels are drawn in their grayscale brightness (dark → black, light → white).
int pngOverlayDraw(PNGDRAW* pDraw) {
  PngOverlayCtx* ctx = reinterpret_cast<PngOverlayCtx*>(pDraw->pUser);

  // Lazy-init: tRNS chunk is processed during decode() before any IDAT data, so by the time
  // the first draw callback fires, hasAlpha() / getTransparentColor() are already valid.
  if (ctx->transparentColor == -2) {
    const int pt = pDraw->iPixelType;
    ctx->transparentColor = (pDraw->iHasAlpha && (pt == PNG_PIXEL_TRUECOLOR || pt == PNG_PIXEL_GRAYSCALE))
                                ? static_cast<int32_t>(ctx->pngObj->getTransparentColor())
                                : -1;
  }

  const int destY = ctx->dstY + (int)(pDraw->y * ctx->yScale);
  if (destY == ctx->lastDstY) return 1;  // skip duplicate rows from Y scaling
  ctx->lastDstY = destY;
  if (destY < 0 || destY >= ctx->screenH) return 1;

  const int srcWidth = ctx->srcWidth;
  const int dstWidth = ctx->dstWidth;
  const uint8_t* pixels = pDraw->pPixels;
  const int pixelType = pDraw->iPixelType;
  const int hasAlpha = pDraw->iHasAlpha;

  // Find the visible drawing span on this scanline. The exterior near-white
  // background stays transparent, but ALL pixels between the first and last
  // real drawing pixel belong to the overlay. This is important for line-art:
  // white skin/clothes/highlights inside the outline must be painted white so
  // reader glyphs cannot show through them.
  auto sourceGray = [&](int x) -> uint8_t {
    switch (pixelType) {
      case PNG_PIXEL_TRUECOLOR_ALPHA: {
        const uint8_t* p = &pixels[x * 4];
        return static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      case PNG_PIXEL_GRAY_ALPHA:
        return pixels[x * 2];
      case PNG_PIXEL_TRUECOLOR: {
        const uint8_t* p = &pixels[x * 3];
        return static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      case PNG_PIXEL_GRAYSCALE:
        return pixels[x];
      case PNG_PIXEL_INDEXED: {
        if (!pDraw->pPalette) return 0;
        const uint8_t* p = &pDraw->pPalette[pixels[x] * 3];
        return static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      default:
        return pixels[x];
    }
  };

  auto sourceAlpha = [&](int x) -> uint8_t {
    switch (pixelType) {
      case PNG_PIXEL_TRUECOLOR_ALPHA:
        return pixels[x * 4 + 3];
      case PNG_PIXEL_GRAY_ALPHA:
        return pixels[x * 2 + 1];
      case PNG_PIXEL_INDEXED:
        if (hasAlpha && pDraw->pPalette) return pDraw->pPalette[768 + pixels[x]];
        return 255;
      case PNG_PIXEL_TRUECOLOR: {
        if (ctx->transparentColor < 0) return 255;
        const uint8_t* p = &pixels[x * 3];
        return (p[0] == static_cast<uint8_t>((ctx->transparentColor >> 16) & 0xFF) &&
                p[1] == static_cast<uint8_t>((ctx->transparentColor >> 8) & 0xFF) &&
                p[2] == static_cast<uint8_t>(ctx->transparentColor & 0xFF))
                   ? 0
                   : 255;
      }
      case PNG_PIXEL_GRAYSCALE:
        if (ctx->transparentColor >= 0 &&
            pixels[x] == static_cast<uint8_t>(ctx->transparentColor & 0xFF)) {
          return 0;
        }
        return 255;
      default:
        return 255;
    }
  };

  constexpr uint8_t kBackgroundWhite = 245;
  int firstInk = -1;
  int lastInk = -1;
  for (int x = 0; x < srcWidth; ++x) {
    if (sourceAlpha(x) >= 128 && sourceGray(x) < kBackgroundWhite) {
      if (firstInk < 0) firstInk = x;
      lastInk = x;
    }
  }

  // A completely white/transparent row is exterior background: leave the
  // saved reader page untouched.
  if (firstInk < 0) return 1;

  int srcX = 0;
  int error = 0;
  for (int dstX = 0; dstX < dstWidth; ++dstX) {
    const int outX = ctx->dstX + dstX;
    if (outX >= 0 && outX < ctx->screenW && srcX >= firstInk && srcX <= lastInk) {
      const uint8_t alpha = sourceAlpha(srcX);
      if (alpha >= 128) {
        const uint8_t gray = sourceGray(srcX);

        // The entire silhouette span is opaque. White pixels explicitly clear
        // old reader text; gray pixels use a stable 2x2 dither; dark pixels
        // remain black.
        bool black = false;
        if (gray < 64) {
          black = true;
        } else if (gray < 160) {
          black = ((outX + destY) & 1) == 0;
        } else if (gray < kBackgroundWhite) {
          black = ((outX & 1) == 0) && ((destY & 1) == 0);
        }
        ctx->renderer->drawPixel(outX, destY, black);
      }
    }

    error += srcWidth;
    while (error >= dstWidth) {
      error -= dstWidth;
      ++srcX;
    }
  }
  return 1;
}

std::string filenameFromPath(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  return lastSlash == std::string::npos ? path : path.substr(lastSlash + 1);
}

std::string recentTitleForPath(const std::string& path) {
  const auto& books = RECENT_BOOKS.getBooks();
  const auto book = std::find_if(books.begin(), books.end(), [&path](const RecentBook& candidate) {
    return candidate.path == path && !candidate.title.empty();
  });
  return book == books.end() ? std::string{} : book->title;
}

RecentBook recentBookForPath(const std::string& path) {
  const auto& books = RECENT_BOOKS.getBooks();
  const auto book =
      std::find_if(books.begin(), books.end(), [&path](const RecentBook& candidate) { return candidate.path == path; });
  if (book != books.end()) {
    return *book;
  }

  RecentBook loadedBook = RECENT_BOOKS.getDataFromBook(path);
  if (loadedBook.title.empty()) {
    loadedBook.title = filenameFromPath(path);
  }
  return loadedBook;
}

std::string epubCachePathFor(const std::string& path) { return Epub::cachePathForFilePath(path, "/.inkmod"); }

BookReadingStats loadBookStatsForPath(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path)) {
    return BookReadingStats{};
  }
  return BookReadingStats::load(epubCachePathFor(path));
}

enum class OverlayDrawResult : uint8_t { NotFound, Drawn, Failed };

enum class SleepImageMode : uint8_t { Custom, Overlay };

struct SleepImageSelection {
  std::string path;
  bool isPng = false;
};

bool isBmpSleepImagePath(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

bool isPngSleepImagePath(const std::string& path) { return FsHelpers::hasPngExtension(path); }

bool tryOpenSleepDirectory(FsFile& dir, std::string& sleepDir, const std::string& candidate) {
  if (candidate.empty()) {
    return false;
  }

  dir = Storage.open(candidate.c_str());
  if (dir && dir.isDirectory()) {
    sleepDir = candidate;
    return true;
  }

  if (dir) {
    dir.close();
  }
  return false;
}

bool openPreferredSleepDirectory(FsFile& dir, std::string& sleepDir) {
  sleepDir.clear();

  if (tryOpenSleepDirectory(dir, sleepDir, APP_STATE.preferredSleepFolderPath)) {
    return true;
  }

  if (!APP_STATE.preferredSleepFolderPath.empty()) {
    LOG_INF("SLP", "Preferred sleep folder missing, falling back: %s", APP_STATE.preferredSleepFolderPath.c_str());
  }

  if (tryOpenSleepDirectory(dir, sleepDir, "/.sleep")) {
    return true;
  }

  return tryOpenSleepDirectory(dir, sleepDir, "/sleep");
}

bool selectPinnedSleepImage(SleepImageMode mode, const std::string& favorite, SleepImageSelection& selection) {
  (void)mode;
  if (favorite.empty()) {
    return false;
  }

  if (!Storage.exists(favorite.c_str())) {
    LOG_INF("SLP", "Pinned sleep image missing, falling back: %s", favorite.c_str());
    return false;
  }

  if (isBmpSleepImagePath(favorite)) {
    selection.path = favorite;
    selection.isPng = false;
    return true;
  }

  if (isPngSleepImagePath(favorite)) {
    selection.path = favorite;
    selection.isPng = true;
    return true;
  }

  LOG_ERR("SLP", "Pinned sleep image has unsupported extension: %s", favorite.c_str());
  return false;
}

bool selectRandomSleepImage(SleepImageMode mode, SleepImageSelection& selection) {
  (void)mode;
  FsFile dir;
  std::string sleepDir;
  if (!openPreferredSleepDirectory(dir, sleepDir)) {
    return false;
  }

  const bool allowPng = true;
  std::vector<std::string> files;
  files.reserve(16);
  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    file.getName(name, sizeof(name));
    std::string filename(name);
    if (filename.empty() || filename[0] == '.') {
      file.close();
      continue;
    }

    const bool isBmp = FsHelpers::hasBmpExtension(filename);
    const bool isPng = allowPng && FsHelpers::hasPngExtension(filename);
    if (!isBmp && !isPng) {
      file.close();
      continue;
    }

    if (isBmp) {
      Bitmap bitmap(file);
      const BmpReaderError parseResult = bitmap.parseHeaders();
      if (parseResult != BmpReaderError::Ok) {
        LOG_ERR("SLP", "Skipping invalid BMP sleep image %s/%s: %s", sleepDir.c_str(), filename.c_str(),
                Bitmap::errorToString(parseResult));
        file.close();
        continue;
      }
    }

    files.emplace_back(std::move(filename));
    file.close();
  }
  dir.close();

  if (files.empty()) {
    return false;
  }

  const uint16_t fileCount = static_cast<uint16_t>(std::min(files.size(), static_cast<size_t>(UINT16_MAX)));
  const uint8_t window =
      static_cast<uint8_t>(std::min(static_cast<size_t>(APP_STATE.recentSleepFill), files.size() - 1));
  auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
  for (uint8_t attempt = 0; attempt < 20 && APP_STATE.isRecentSleep(randomFileIndex, window); attempt++) {
    randomFileIndex = static_cast<uint16_t>(random(fileCount));
  }

  APP_STATE.pushRecentSleep(randomFileIndex);
  APP_STATE.saveToFile();
  selection.path = sleepDir + "/" + files[randomFileIndex];
  selection.isPng = FsHelpers::hasPngExtension(selection.path);
  return true;
}

}  // namespace

uint8_t SleepActivity::effectiveSleepScreenMode() const {
  if (!fromTimeout) return SETTINGS.sleepScreen;

  switch (SETTINGS.quickResumeSleepScreen) {
    case InkMODSettings::QUICK_RESUME_AFTER_TIMEOUT:
      return InkMODSettings::SLEEP_SCREEN_MODE::QUICK_RESUME;
    case InkMODSettings::TIMEOUT_SLEEP_OVERLAY:
      return InkMODSettings::SLEEP_SCREEN_MODE::OVERLAY;
    case InkMODSettings::TIMEOUT_SLEEP_CUSTOM:
      return InkMODSettings::SLEEP_SCREEN_MODE::CUSTOM;
    default:
      return SETTINGS.sleepScreen;
  }
}

const std::string& SleepActivity::effectivePinnedSleepImagePath() const {
  if (fromTimeout &&
      (SETTINGS.quickResumeSleepScreen == InkMODSettings::TIMEOUT_SLEEP_OVERLAY ||
       SETTINGS.quickResumeSleepScreen == InkMODSettings::TIMEOUT_SLEEP_CUSTOM) &&
      !APP_STATE.timeoutSleepImagePath.empty()) {
    return APP_STATE.timeoutSleepImagePath;
  }
  return APP_STATE.favoriteSleepImagePath;
}

void SleepActivity::onEnter() {
  Activity::onEnter();

  const uint8_t sleepMode = effectiveSleepScreenMode();

  if (sleepMode == InkMODSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    return renderLastScreenSleepScreen();
  }

  overlayBackgroundBufferStored =
      sleepMode == InkMODSettings::SLEEP_SCREEN_MODE::OVERLAY && renderer.storeBwBuffer();

  // Show the popup in the reader's orientation when sleep starts from an open book.
  // Reset to portrait afterwards so the sleep screen renderer keeps its existing layout.
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (sleepMode) {
    case (InkMODSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (InkMODSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (InkMODSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (InkMODSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      return renderCoverSleepScreen();
    case (InkMODSettings::SLEEP_SCREEN_MODE::OVERLAY):
      return renderOverlaySleepScreen();
    case (InkMODSettings::SLEEP_SCREEN_MODE::READING_STATS_SLEEP):
      return renderReadingStatsSleepScreen();
    case (InkMODSettings::SLEEP_SCREEN_MODE::MINIMAL_SLEEP):
      return renderMinimalSleepScreen();
    case (InkMODSettings::SLEEP_SCREEN_MODE::MINIMAL_STATS_SLEEP):
      return renderMinimalStatsSleepScreen();
    case (InkMODSettings::SLEEP_SCREEN_MODE::DASHBOARD_SLEEP):
      return renderDashboardSleepScreen();
    case (InkMODSettings::SLEEP_SCREEN_MODE::CALENDAR_SLEEP):
    case (InkMODSettings::SLEEP_SCREEN_MODE::CALENDAR_SLEEP_INVERTED):
      return renderCalendarSleepScreen();
    case (InkMODSettings::SLEEP_SCREEN_MODE::CALENDAR_SLEEP_LANDSCAPE):
    case (InkMODSettings::SLEEP_SCREEN_MODE::CALENDAR_SLEEP_LANDSCAPE_INVERTED):
      return renderCalendarSleepScreenLandscape();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  SleepImageSelection selection;
  if (selectPinnedSleepImage(SleepImageMode::Custom, effectivePinnedSleepImagePath(), selection) ||
      selectRandomSleepImage(SleepImageMode::Custom, selection)) {
    if (selection.isPng) {
      ImageDimensions dims;
      if (PngToFramebufferConverter::getDimensionsStatic(selection.path, dims)) {
        const int pageWidth = renderer.getScreenWidth();
        const int pageHeight = renderer.getScreenHeight();
        float scale = 1.0f;
        if (dims.width > pageWidth || dims.height > pageHeight) {
          scale = std::min(static_cast<float>(pageWidth) / static_cast<float>(dims.width),
                           static_cast<float>(pageHeight) / static_cast<float>(dims.height));
        }
        const int drawWidth = std::max(1, static_cast<int>(dims.width * scale));
        const int drawHeight = std::max(1, static_cast<int>(dims.height * scale));
        RenderConfig config;
        config.x = (pageWidth - drawWidth) / 2;
        config.y = (pageHeight - drawHeight) / 2;
        config.maxWidth = drawWidth;
        config.maxHeight = drawHeight;
        config.useGrayscale = true;
        config.useDithering = true;
        config.performanceMode = false;
        config.useExactDimensions = true;
        renderer.clearScreen();
        PngToFramebufferConverter converter;
        if (converter.decodeToFramebuffer(selection.path, renderer, config)) {
          renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
          return;
        }
      }
      LOG_ERR("SLP", "Failed to render custom sleep PNG: %s", selection.path.c_str());
    } else {
      FsFile file;
      if (Storage.openFileForRead("SLP", selection.path, file)) {
        LOG_INF("SLP", "Loading custom sleep image: %s", selection.path.c_str());
        delay(100);
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          return;
        }
        LOG_ERR("SLP", "Failed to parse custom sleep BMP: %s", selection.path.c_str());
      } else {
        LOG_ERR("SLP", "Failed to open custom sleep image: %s", selection.path.c_str());
      }
    }
  }

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  FsFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  drawInkMODBitmap(renderer, InkMODLogo240, INKMODLOGO240_WIDTH, INKMODLOGO240_HEIGHT,
                   (pageWidth - INKMODLOGO240_WIDTH) / 2,
                   (pageHeight - INKMODLOGO240_HEIGHT) / 2);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  const bool lightSleepScreen = SETTINGS.sleepScreen == InkMODSettings::SLEEP_SCREEN_MODE::LIGHT;
  if (!lightSleepScreen) {
    renderer.invertScreen();
  }

#ifdef INKMOD_SHOW_SLEEP_BUILD_INFO
  const std::string buildInfo = std::string(INKMOD_BUILD_ENV) + " " + INKMOD_VERSION;
  const std::string visibleBuildInfo =
      renderer.truncatedText(SMALL_FONT_ID, buildInfo.c_str(), pageWidth - sleepBuildInfoSideMargin * 2);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 118, visibleBuildInfo.c_str(), lightSleepScreen);
#endif

  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool blackBackground) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == InkMODSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == InkMODSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  // Fit mode normally leaves white letterbox margins.  On black-bezel
  // readers a black fill makes a narrow cover look like a physical inset
  // instead of a small white card floating inside the device.
  renderer.clearScreen(blackBackground ? 0x00 : 0xFF);

  // Bitmap rendering intentionally leaves white pixels untouched.  Give the
  // cover its own white canvas first, otherwise the black letterbox shows
  // through the cover's white background.  The sleep-cover black mode never
  // crops, but deriving the rectangle from the same fit calculation keeps it
  // correct if the renderer later receives a pre-cropped cover.
  const float croppedWidth = (1.0f - cropX) * static_cast<float>(bitmap.getWidth());
  const float croppedHeight = (1.0f - cropY) * static_cast<float>(bitmap.getHeight());
  const float scale = std::min(1.0f, std::min(static_cast<float>(pageWidth) / croppedWidth,
                                              static_cast<float>(pageHeight) / croppedHeight));
  const int coverWidth = static_cast<int>(std::ceil(croppedWidth * scale));
  const int coverHeight = static_cast<int>(std::ceil(croppedHeight * scale));
  const int coverLeft = std::max(0, x);
  const int coverTop = std::max(0, y);
  const int coverRight = std::min(pageWidth, x + coverWidth);
  const int coverBottom = std::min(pageHeight, y + coverHeight);
  const auto paintCoverCanvas = [&]() {
    if (blackBackground && coverRight > coverLeft && coverBottom > coverTop) {
      renderer.fillRect(coverLeft, coverTop, coverRight - coverLeft, coverBottom - coverTop, false);
    }
  };
  paintCoverCanvas();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == InkMODSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (!blackBackground &&
      SETTINGS.sleepScreenCoverFilter == InkMODSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    paintCoverCanvas();
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    paintCoverCanvas();
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (InkMODSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (path.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  const bool cropped = SETTINGS.sleepScreenCoverMode == InkMODSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  const bool blackBackground = SETTINGS.sleepScreenCoverMode == InkMODSettings::SLEEP_SCREEN_COVER_MODE::BLACK_BACKGROUND;
  std::string coverBmpPath = SleepCoverAssets::cachedCoverPathFor(path, cropped);
  if (coverBmpPath.empty() && SleepCoverAssets::prepareFullCoverForPath(path, cropped)) {
    coverBmpPath = SleepCoverAssets::cachedCoverPathFor(path, cropped);
  }
  if (coverBmpPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap, blackBackground);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderReadingStatsSleepScreen() const {
  BookReadingStats bookStats;
  std::string bookTitle = tr(STR_READING_STATS);
  float progressPercent = -1.0f;

  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (!path.empty()) {
    const std::string recentTitle = recentTitleForPath(path);
    bookTitle = recentTitle.empty() ? filenameFromPath(path) : recentTitle;

    bookStats = loadBookStatsForPath(path);
    progressPercent = RecentBookProgress::loadPercent(recentBookForPath(path));
  }

  if (SETTINGS.clockDisabled || !halClock.isAvailable()) {
    const GlobalReadingStats deviceStats = GlobalReadingStats::load();
    const bool hasSyncedStats = GlobalReadingStats::hasSyncedStats();
    const GlobalReadingStats allDevicesStats =
        hasSyncedStats ? GlobalReadingStats::loadAggregated(deviceStats) : GlobalReadingStats{};
    renderNoRtcCombinedStatsPage(renderer, nullptr, bookTitle, bookStats, progressPercent, false, 0, deviceStats,
                                 hasSyncedStats ? &allDevicesStats : nullptr, false);
  } else {
    renderPerBookStatsPage(renderer, nullptr, bookTitle, bookStats, progressPercent, false, 0, false, false, false);
  }
  renderer.invertScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderMinimalSleepScreen() const {
  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (path.empty()) {
    return renderDefaultSleepScreen();
  }

  RecentBook book = recentBookForPath(path);
  book.coverBmpPath = SleepCoverAssets::cachedMinimalCoverPathFor(path);

  const BookReadingStats bookStats = loadBookStatsForPath(path);
  const float progressPercent = RecentBookProgress::loadPercent(book);
  MinimalTheme theme;
  theme.drawSleepScreen(renderer, book, &bookStats, progressPercent);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderMinimalStatsSleepScreen() const {
  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (path.empty()) {
    return renderDefaultSleepScreen();
  }

  RecentBook book = recentBookForPath(path);
  book.coverBmpPath = SleepCoverAssets::cachedMinimalCoverPathFor(path);

  const BookReadingStats bookStats = loadBookStatsForPath(path);
  const GlobalReadingStats globalStats = GlobalReadingStats::load();
  const float progressPercent = RecentBookProgress::loadPercent(book);
  MinimalTheme theme;
  theme.drawStatsSleepScreen(renderer, book, &bookStats, &globalStats, progressPercent);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderDashboardSleepScreen() const {
  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (path.empty()) {
    return renderDefaultSleepScreen();
  }

  // Unlike the Minimal sleep screens, don't overwrite coverBmpPath with
  // SleepCoverAssets::cachedMinimalCoverPathFor() - that helper only checks for a
  // pre-generated Minimal-sized thumbnail (see shouldPrepareMinimalCover()), which
  // never gets generated for Dashboard mode, so it always returns "" here and the
  // cover would silently fail to load. DashboardTheme generates its own
  // adaptively-sized thumbnail on demand from the book's real coverBmpPath/path.
  RecentBook book = recentBookForPath(path);

  const BookReadingStats bookStats = loadBookStatsForPath(path);
  const GlobalReadingStats globalStats = GlobalReadingStats::load();
  const float progressPercent = RecentBookProgress::loadPercent(book);
  DashboardTheme theme;
  theme.drawDashboardSleepScreen(renderer, book, &bookStats, &globalStats, progressPercent);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

// Explicit switches instead of StrId arithmetic: gen_i18n.py does not guarantee
// STR_CAL_MONTH_1..12 (or STR_CAL_DOW_1..7) end up as consecutive enum values just
// because they're adjacent in the yaml source, so "StrId::STR_CAL_MONTH_1 + offset"
// can silently land on a completely unrelated string (confirmed on-device: month 5
// resolved to STR_SUNLIGHT_FADING_FIX instead of STR_CAL_MONTH_5).
static const char* calendarMonthName(const uint8_t month) {
  switch (month) {
    case 1:
      return tr(STR_CAL_MONTH_1);
    case 2:
      return tr(STR_CAL_MONTH_2);
    case 3:
      return tr(STR_CAL_MONTH_3);
    case 4:
      return tr(STR_CAL_MONTH_4);
    case 5:
      return tr(STR_CAL_MONTH_5);
    case 6:
      return tr(STR_CAL_MONTH_6);
    case 7:
      return tr(STR_CAL_MONTH_7);
    case 8:
      return tr(STR_CAL_MONTH_8);
    case 9:
      return tr(STR_CAL_MONTH_9);
    case 10:
      return tr(STR_CAL_MONTH_10);
    case 11:
      return tr(STR_CAL_MONTH_11);
    case 12:
      return tr(STR_CAL_MONTH_12);
    default:
      return "?";
  }
}

static const char* calendarDayOfWeekAbbrev(const int mondayFirstIndex) {
  switch (mondayFirstIndex) {
    case 0:
      return tr(STR_CAL_DOW_1);
    case 1:
      return tr(STR_CAL_DOW_2);
    case 2:
      return tr(STR_CAL_DOW_3);
    case 3:
      return tr(STR_CAL_DOW_4);
    case 4:
      return tr(STR_CAL_DOW_5);
    case 5:
      return tr(STR_CAL_DOW_6);
    case 6:
      return tr(STR_CAL_DOW_7);
    default:
      return "?";
  }
}

// Simple segment-style digit renderer for the calendar's month-number header. There's
// no font in this build tall enough to visually match the two-line month-name+year
// block on the left (max is UI_12), so this draws proper big digits geometrically
// instead of trying to scale an existing font (which isn't supported - see the
// drawImage() note above about why scaling raw bitmaps silently corrupts them).
//      _a_
//     f   b
//      _g_
//     e   c
//      _d_
namespace {
constexpr uint8_t kSegmentsByDigit[10] = {
    0b0111111,  // 0: a b c d e f
    0b0000110,  // 1: b c
    0b1011011,  // 2: a b d e g
    0b1001111,  // 3: a b c d g
    0b1100110,  // 4: b c f g
    0b1101101,  // 5: a c d f g
    0b1111101,  // 6: a c d e f g
    0b0000111,  // 7: a b c
    0b1111111,  // 8: all
    0b1101111,  // 9: a b c d f g
};

void drawBigDigit(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                  const unsigned digit) {
  if (digit > 9) {
    return;
  }
  const uint8_t segs = kSegmentsByDigit[digit];
  const int thickness = std::max(3, h / 9);
  const int halfH = h / 2;
  if (segs & 0x01) renderer.fillRoundedRect(x, y, w, thickness, thickness / 2, Color::Black);                // a
  if (segs & 0x02)
    renderer.fillRoundedRect(x + w - thickness, y, thickness, halfH, thickness / 2, Color::Black);  // b
  if (segs & 0x04)
    renderer.fillRoundedRect(x + w - thickness, y + halfH, thickness, h - halfH, thickness / 2, Color::Black);  // c
  if (segs & 0x08)
    renderer.fillRoundedRect(x, y + h - thickness, w, thickness, thickness / 2, Color::Black);  // d
  if (segs & 0x10)
    renderer.fillRoundedRect(x, y + halfH, thickness, h - halfH, thickness / 2, Color::Black);  // e
  if (segs & 0x20) renderer.fillRoundedRect(x, y, thickness, halfH, thickness / 2, Color::Black);  // f
  if (segs & 0x40)
    renderer.fillRoundedRect(x, y + halfH - thickness / 2, w, thickness, thickness / 2, Color::Black);  // g
}

// GfxRenderer::drawImage() rotates where an image ends up on screen for the
// current orientation, but not the bitmap's own pixels (its own source has
// a "TODO: Rotate bits" marking exactly this gap) - fine for every other
// screen that draws InkMODLogo120, since they all use Portrait, but the
// landscape calendar below is the first to draw it under
// LandscapeClockwise, where the pixels come out wrong. A manual 180-degree
// pre-rotation of the bitmap (matching how LandscapeClockwise's own
// coordinate math is described relative to the panel's native frame) did
// not fix it in testing, and guessing further at some other fixed-angle
// transform risks the same outcome. fillRect(), unlike drawImage(), is
// already proven correct in this orientation - it's what draws the
// today/weekday highlight boxes elsewhere on this same screen, and those
// land correctly - so this draws the logo as a series of small filled
// rectangles (one per horizontal run of "on" pixels in a row) through
// that same call instead of as a single blitted image, guaranteeing
// consistent rotation handling instead of another blind guess at it.

}  // namespace

void SleepActivity::renderCalendarSleepScreen() const {
  if (SETTINGS.clockDisabled) {
    // User turned the clock off after previously picking Calendar - it's no
    // longer offered in the picker, but fall back gracefully for anyone who
    // still has it stored from before.
    return renderDefaultSleepScreen();
  }

  ReadingStatsDateTime now;
  if (!getCurrentLocalReadingStatsDateTime(now)) {
    // No synced clock yet (e.g. X4 before its first NTP sync) - a calendar with the
    // wrong month/day would be worse than no calendar, so fall back.
    return renderDefaultSleepScreen();
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  constexpr int kMargin = 24;

  renderer.clearScreen();

  const ReadingStatsDate firstOfMonth{now.date.year, now.date.month, 1};
  const uint8_t firstDow = readingStatsDayOfWeekIndex(firstOfMonth);  // Monday = 0
  const uint8_t totalDays = daysInMonth(now.date.year, now.date.month);
  const uint8_t todayDow = readingStatsDayOfWeekIndex(now.date);
  const int gridRows = (firstDow + totalDays + 6) / 7;

  constexpr int kRowHeight = 62;
  constexpr int kHighlightSize = 42;
  const int colW = (pageWidth - kMargin * 2) / 7;

  // Month name and year share the biggest UI font (UI_12) and both render bold,
  // so the left header block reads at the same visual weight as the big-digit
  // month number on the right. monthNumH below is derived from headerH, so
  // growing this block also grows the digits in lockstep - keeping the two
  // sides symmetric instead of the digits dwarfing a small light-weight year line.
  const int monthLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int yearLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int dowLineH = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int kHeaderLineGap = 6;
  const int headerH = monthLineH + kHeaderLineGap + yearLineH;
  const int dowGap = 34;
  const int dowRowH = dowLineH + 20;
  const int gridH = gridRows * kRowHeight;
  const int blockH = headerH + dowGap + dowRowH + gridH;

  // Reserve room for the logo below the grid, then vertically center the whole
  // header+weekday+grid block in whatever's left above it - the block's own height
  // varies with how many week-rows the month needs (4-6), so this keeps it looking
  // centered every month instead of using one fixed top margin.
  constexpr int kLogoAreaH = 250;
  const int available = pageHeight - kLogoAreaH;
  const int monthTop = std::max(kMargin, (available - blockH) / 2);

  // Month name + year, top-left.
  const char* monthName = calendarMonthName(now.date.month);
  renderer.drawText(UI_12_FONT_ID, kMargin, monthTop, monthName, true, EpdFontFamily::BOLD);
  char yearBuf[8];
  snprintf(yearBuf, sizeof(yearBuf), "%u", static_cast<unsigned>(now.date.year));
  renderer.drawText(UI_12_FONT_ID, kMargin, monthTop + monthLineH + kHeaderLineGap, yearBuf, true, EpdFontFamily::BOLD);

  // Month number, top-right (e.g. "07" for July) - sized to match the combined
  // height of the month-name + year stack on the left, using drawBigDigit() since
  // no font in this build is tall enough on its own.
  const int monthNumH = headerH;
  const int digitW = static_cast<int>(monthNumH * 0.56f);
  constexpr int kDigitGap = 6;
  const unsigned monthTens = static_cast<unsigned>(now.date.month) / 10;
  const unsigned monthOnes = static_cast<unsigned>(now.date.month) % 10;
  const int monthNumX = pageWidth - kMargin - digitW * 2 - kDigitGap;
  drawBigDigit(renderer, monthNumX, monthTop, digitW, monthNumH, monthTens);
  drawBigDigit(renderer, monthNumX + digitW + kDigitGap, monthTop, digitW, monthNumH, monthOnes);

  // Weekday header row (Monday first, matching readingStatsDayOfWeekIndex's convention).
  // Today's column gets the same solid highlight treatment as today's day in the grid.
  int y = monthTop + headerH + dowGap;
  for (int col = 0; col < 7; col++) {
    const char* dow = calendarDayOfWeekAbbrev(col);
    const int w = renderer.getTextWidth(UI_10_FONT_ID, dow, EpdFontFamily::BOLD);
    const int cx = kMargin + colW * col + colW / 2;
    if (col == todayDow) {
      const int boxW = w + 16;
      const int boxH = dowLineH + 8;
      renderer.fillRoundedRect(cx - boxW / 2, y - 4, boxW, boxH, 6, Color::Black);
      renderer.drawText(UI_10_FONT_ID, cx - w / 2, y, dow, false, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(UI_10_FONT_ID, cx - w / 2, y, dow, true, EpdFontFamily::BOLD);
    }
  }
  y += dowRowH;

  // Day grid.
  const int numLineH = renderer.getLineHeight(UI_10_FONT_ID);

  int dayNum = 1;
  for (int row = 0; dayNum <= totalDays; row++) {
    const int rowY = y + row * kRowHeight;
    for (int col = 0; col < 7; col++) {
      if (row == 0 && col < firstDow) {
        continue;
      }
      if (dayNum > totalDays) {
        break;
      }

      char numBuf[4];
      snprintf(numBuf, sizeof(numBuf), "%u", static_cast<unsigned>(dayNum));
      const int cx = kMargin + colW * col + colW / 2;
      const bool isToday = (dayNum == now.date.day);

      if (isToday) {
        renderer.fillRoundedRect(cx - kHighlightSize / 2, rowY - 8, kHighlightSize, kHighlightSize, 8, Color::Black);
        const int w = renderer.getTextWidth(UI_10_FONT_ID, numBuf, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, cx - w / 2, rowY - 8 + (kHighlightSize - numLineH) / 2, numBuf, false,
                          EpdFontFamily::BOLD);
      } else {
        const int w = renderer.getTextWidth(UI_10_FONT_ID, numBuf, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, cx - w / 2, rowY, numBuf, true, EpdFontFamily::BOLD);
      }
      dayNum++;
    }
  }

  // Small inkMOD wordmark at the bottom, matching the default sleep screen's branding.
  // drawImage() doesn't scale - it reads InkMODLogo120's raw bytes assuming the width/
  // height passed in match its actual 120x120 layout, so it has to be drawn at native
  // size (like renderDefaultSleepScreen does) or the row stride comes out wrong and
  // the image comes out corrupted instead of shrunk.
  const int logoY = pageHeight - INKMODLOGO240_HEIGHT - 18;
  drawInkMODBitmap(renderer, InkMODLogo240, INKMODLOGO240_WIDTH, INKMODLOGO240_HEIGHT,
                   (pageWidth - INKMODLOGO240_WIDTH) / 2, logoY);

  if (SETTINGS.sleepScreen == InkMODSettings::SLEEP_SCREEN_MODE::CALENDAR_SLEEP_INVERTED) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

// Landscape sibling of renderCalendarSleepScreen() above, for standing the
// device on its side like a small desk calendar. Same data/helpers, laid
// out side-by-side (month info on the left, day grid on the right) instead
// of stacked top-to-bottom - stacking the portrait blocks as-is wouldn't
// fit the wide-short 800x480 shape landscape orientation gives this
// screen (see GfxRenderer::Orientation - LandscapeCounterClockwise is the
// panel's native rotated direction, matching "stand it up sideways").
void SleepActivity::renderCalendarSleepScreenLandscape() const {
  if (SETTINGS.clockDisabled) {
    return renderDefaultSleepScreen();
  }

  ReadingStatsDateTime now;
  if (!getCurrentLocalReadingStatsDateTime(now)) {
    return renderDefaultSleepScreen();
  }

  renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  constexpr int kMargin = 24;

  renderer.clearScreen();

  const ReadingStatsDate firstOfMonth{now.date.year, now.date.month, 1};
  const uint8_t firstDow = readingStatsDayOfWeekIndex(firstOfMonth);  // Monday = 0
  const uint8_t totalDays = daysInMonth(now.date.year, now.date.month);
  const uint8_t todayDow = readingStatsDayOfWeekIndex(now.date);
  const int gridRows = (firstDow + totalDays + 6) / 7;

  // Left sidebar: month name, year, big-digit month number, small wordmark
  // at the very bottom - everything the portrait header+logo blocks show,
  // just stacked in a narrow column instead of spread across the full
  // width. Sized to the logo's own width (120px) rather than a wider,
  // independently-chosen column - the month/year text and big digits sit
  // within that same 120px, so the whole column reads as one aligned
  // block instead of text spilling wider than the logo below it.
  constexpr int kSidebarW = 120;
  constexpr int kSidebarGap = 32;
  const int gridX = kMargin + kSidebarW + kSidebarGap;
  const int gridAreaW = pageWidth - gridX - kMargin;
  const int colW = gridAreaW / 7;

  const int monthLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int yearLineH = renderer.getLineHeight(UI_12_FONT_ID);
  constexpr int kHeaderLineGap = 6;
  constexpr int kMonthNumH = 90;
  constexpr int kLogoSize = 120;
  const int wordmarkLineH = renderer.getLineHeight(UI_10_FONT_ID);

  // Weekday header row + day grid sizing, computed here (ahead of actually
  // drawing either side) so the sidebar and the grid can share a single
  // top position below instead of each being centered on its own -
  // centering them independently kept their vertical *centers* aligned,
  // but since the two blocks aren't the same height, that still left
  // their top edges - "Август" vs. the "ПН" weekday row - starting from
  // different heights and reading as unbalanced. A printed calendar's
  // month header and day grid start from the same line; matching that
  // directly is simpler than trying to fix it through independent
  // centering.
  const int dowLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int dowRowH = dowLineH + 20;
  constexpr int dowGap = 20;
  // A shorter row height than the portrait screen's fixed 62px - this
  // layout has less vertical room to spend (480px total vs. 800px) and,
  // unlike the portrait grid, still has to share that height with nothing
  // above it but a top margin, so every pixel of row height directly
  // competes with fitting a 6-row month at all.
  constexpr int kRowHeight = 54;
  constexpr int kHighlightSize = 38;

  // Shared top: a small fixed margin below the top edge, like a real desk
  // calendar's month header sits right near the top of the card rather
  // than being mathematically centered in whatever space is available -
  // centering it (this used to compute a position based on the taller of
  // the two blocks) ended up reading as "randomly" positioned depending
  // on how much shorter the content was than the available height,
  // instead of the simple, predictable "starts near the top" a printed
  // calendar actually looks like.
  constexpr int kTopPad = 20;
  const int sharedTop = kMargin + kTopPad;

  int sidebarY = sharedTop;
  // The weekday row, rather than its invisible container, is the visual top
  // of the calendar. Align it with the month heading in the left column.
  const int gridTop = sharedTop - dowGap;

  renderer.drawText(UI_12_FONT_ID, kMargin, sidebarY, calendarMonthName(now.date.month), true, EpdFontFamily::BOLD);
  sidebarY += monthLineH + kHeaderLineGap;
  char yearBuf[8];
  snprintf(yearBuf, sizeof(yearBuf), "%u", static_cast<unsigned>(now.date.year));
  renderer.drawText(UI_12_FONT_ID, kMargin, sidebarY, yearBuf, true, EpdFontFamily::BOLD);
  sidebarY += yearLineH + 20;

  // Big-digit month number, sized to the sidebar's own width rather than
  // matching the header block's height (there's no equally-wide "other
  // side" to match here, unlike the portrait layout's left/right split).
  // Noticeably smaller than the portrait screen's big digits (150px) -
  // at that size here it visually overpowered the month/year text above
  // it and the logo below, instead of reading as one balanced sidebar.
  const int digitW = static_cast<int>(kMonthNumH * 0.56f);
  constexpr int kDigitGap = 8;
  const unsigned monthTens = static_cast<unsigned>(now.date.month) / 10;
  const unsigned monthOnes = static_cast<unsigned>(now.date.month) % 10;
  drawBigDigit(renderer, kMargin, sidebarY, digitW, kMonthNumH, monthTens);
  drawBigDigit(renderer, kMargin + digitW + kDigitGap, sidebarY, digitW, kMonthNumH, monthOnes);
  sidebarY += kMonthNumH + 16;

  // Full logo + wordmark, same as the portrait sleep screens use - unlike
  // the earlier version of this screen, there's room for it here once the
  // big digits above are trimmed down a little: drawImage() can't scale
  // InkMODLogo120 (it assumes the width/height passed in match the image's
  // actual native 120x120 layout, so shrinking it would corrupt it rather
  // than resize it), so it's drawn at that native size, not scaled to fit.
  // drawRotationSafeLogo(), not a plain drawImage() call, specifically
  // because this screen runs under LandscapeClockwise - see that
  // function's own comment for why.
  const int landscapeLogoX = kMargin + (kSidebarW - kLogoSize) / 2;
  drawInkMODBitmap(renderer, InkMODLogo120, INKMODLOGO120_WIDTH, INKMODLOGO120_HEIGHT,
                   landscapeLogoX, sidebarY);
  sidebarY += kLogoSize + 4;

  // Weekday header row + day grid, on the right, starting from the same
  // sharedTop the sidebar used above.
  int y = gridTop + dowGap;
  for (int col = 0; col < 7; col++) {
    const char* dow = calendarDayOfWeekAbbrev(col);
    const int w = renderer.getTextWidth(UI_10_FONT_ID, dow, EpdFontFamily::BOLD);
    const int cx = gridX + colW * col + colW / 2;
    if (col == todayDow) {
      const int boxW = w + 16;
      const int boxH = dowLineH + 8;
      renderer.fillRoundedRect(cx - boxW / 2, y - 4, boxW, boxH, 6, Color::Black);
      renderer.drawText(UI_10_FONT_ID, cx - w / 2, y, dow, false, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(UI_10_FONT_ID, cx - w / 2, y, dow, true, EpdFontFamily::BOLD);
    }
  }
  y += dowRowH;

  const int numLineH = renderer.getLineHeight(UI_10_FONT_ID);
  int dayNum = 1;
  for (int row = 0; dayNum <= totalDays; row++) {
    const int rowY = y + row * kRowHeight;
    for (int col = 0; col < 7; col++) {
      if (row == 0 && col < firstDow) {
        continue;
      }
      if (dayNum > totalDays) {
        break;
      }

      char numBuf[4];
      snprintf(numBuf, sizeof(numBuf), "%u", static_cast<unsigned>(dayNum));
      const int cx = gridX + colW * col + colW / 2;
      const bool isToday = (dayNum == now.date.day);

      if (isToday) {
        renderer.fillRoundedRect(cx - kHighlightSize / 2, rowY - 6, kHighlightSize, kHighlightSize, 8, Color::Black);
        const int w = renderer.getTextWidth(UI_10_FONT_ID, numBuf, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, cx - w / 2, rowY - 6 + (kHighlightSize - numLineH) / 2, numBuf, false,
                          EpdFontFamily::BOLD);
      } else {
        const int w = renderer.getTextWidth(UI_10_FONT_ID, numBuf, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, cx - w / 2, rowY, numBuf, true, EpdFontFamily::BOLD);
      }
      dayNum++;
    }
  }

  if (SETTINGS.sleepScreen == InkMODSettings::SLEEP_SCREEN_MODE::CALENDAR_SLEEP_LANDSCAPE_INVERTED) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
}

void SleepActivity::renderLastScreenSleepScreen() const {
  // Quick Resume intentionally leaves the page/wallpaper untouched. The old
  // moon bitmap had an opaque rectangle around it on grayscale backgrounds.
  // No marker is preferable to damaging the user's sleep image.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderOverlaySleepScreen() const {
  // Overlay pictures always use portrait orientation regardless of the reader's orientation preference.
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Portrait);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const bool shouldUseReaderPageBackground = canSnapshotOverlayBackground;
  const std::string path = shouldUseReaderPageBackground
                               ? (currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath)
                               : std::string{};

  auto renderSavedReaderPage = [&]() -> bool {
    if (path.empty()) {
      return false;
    }

    if (FsHelpers::checkFileExtension(path, ".xtc") || FsHelpers::checkFileExtension(path, ".xtch")) {
      return XtcReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    if (FsHelpers::checkFileExtension(path, ".txt")) {
      return TxtReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    if (FsHelpers::checkFileExtension(path, ".epub")) {
      return EpubReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    return false;
  };
  const bool backgroundSupportsGrayscale =
      FsHelpers::checkFileExtension(path, ".txt") || FsHelpers::checkFileExtension(path, ".epub");
  bool backgroundWasRebuilt = false;
  bool backgroundAvailable = false;

  // Step 1: Restore the screen that was visible before the sleep popup. When
  // that snapshot is unavailable in the reader, rebuild from the saved position.
  if (overlayBackgroundBufferStored) {
    renderer.restoreBwBuffer();
    backgroundAvailable = true;
  } else if (shouldUseReaderPageBackground && !path.empty()) {
    backgroundWasRebuilt = renderSavedReaderPage();
    backgroundAvailable = backgroundWasRebuilt;

    if (!backgroundWasRebuilt) {
      LOG_DBG("SLP", "Page re-render failed, using white background");
      renderer.clearScreen();
    }
  } else {
    LOG_DBG("SLP", "No current screen snapshot available for overlay sleep screen");
    renderer.clearScreen();
  }

  // Remove the live battery strip from the preserved/reconstructed reader page so the
  // overlay sleep screen still shows chapter/progress details without the battery glance target.
  if (shouldUseReaderPageBackground && backgroundAvailable) {
    hideOverlayBatteryStrip(renderer);
  }

  // Step 2: Load the overlay image using the same selection logic as renderCustomSleepScreen.
  // BMP: white pixels are skipped (transparent via drawBitmap), black pixels composited on top.
  // PNG: pixels with alpha < 128 are skipped; opaque pixels are drawn with their grayscale value.
  auto tryDrawOverlay = [&](const std::string& filename) -> OverlayDrawResult {
    FsFile file;
    if (!Storage.openFileForRead("SLP", filename, file)) {
      if (Storage.exists(filename.c_str())) {
        LOG_ERR("SLP", "BMP overlay exists but could not be opened: %s", filename.c_str());
        return OverlayDrawResult::Failed;
      }
      LOG_DBG("SLP", "BMP overlay not found: %s", filename.c_str());
      return OverlayDrawResult::NotFound;
    }
    Bitmap bitmap(file, true);
    const BmpReaderError parseResult = bitmap.parseHeaders();
    if (parseResult != BmpReaderError::Ok) {
      LOG_ERR("SLP", "BMP overlay header parse failed for %s: %s", filename.c_str(),
              Bitmap::errorToString(parseResult));
      file.close();
      return OverlayDrawResult::Failed;
    }

    int x, y;
    float cropX = 0, cropY = 0;
    if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
      float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
      if (ratio > screenRatio) {
        x = 0;
        y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      } else {
        x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
        y = 0;
      }
    } else {
      x = (pageWidth - bitmap.getWidth()) / 2;
      y = (pageHeight - bitmap.getHeight()) / 2;
    }

    // The generic drawBitmap() intentionally skips white pixels in BW mode.
    // That is useful for icons, but wrong for a sleep overlay: white skin,
    // clothes and highlights would become transparent and reader text would
    // show through. Composite scanline-by-scanline instead.
    LOG_INF("SLP", "Drawing BMP overlay with opaque silhouette: %s", filename.c_str());

    const int srcW = bitmap.getWidth();
    const int srcH = bitmap.getHeight();
    const float scale = std::min(1.0f, std::min(static_cast<float>(pageWidth) / srcW,
                                                static_cast<float>(pageHeight) / srcH));
    const int dstW = std::max(1, static_cast<int>(std::round(srcW * scale)));
    const int dstH = std::max(1, static_cast<int>(std::round(srcH * scale)));
    const int dstX0 = (pageWidth - dstW) / 2;
    const int dstY0 = (pageHeight - dstH) / 2;

    const int packedRowBytes = (srcW + 3) / 4;
    uint8_t* packed = new (std::nothrow) uint8_t[packedRowBytes];
    uint8_t* rawRow = new (std::nothrow) uint8_t[bitmap.getRowBytes()];
    if (!packed || !rawRow) {
      delete[] packed;
      delete[] rawRow;
      file.close();
      LOG_ERR("SLP", "Not enough heap for BMP overlay row buffers");
      return OverlayDrawResult::Failed;
    }

    if (bitmap.rewindToData() != BmpReaderError::Ok) {
      delete[] packed;
      delete[] rawRow;
      file.close();
      LOG_ERR("SLP", "Failed to rewind BMP overlay");
      return OverlayDrawResult::Failed;
    }

    int lastDstY = -1;
    for (int fileRow = 0; fileRow < srcH; ++fileRow) {
      if (bitmap.readNextRow(packed, rawRow) != BmpReaderError::Ok) {
        delete[] packed;
        delete[] rawRow;
        file.close();
        LOG_ERR("SLP", "Failed to read BMP overlay row %d", fileRow);
        return OverlayDrawResult::Failed;
      }

      const int logicalY = bitmap.isTopDown() ? fileRow : (srcH - 1 - fileRow);
      const int dstY = dstY0 + static_cast<int>(std::floor(logicalY * scale));
      if (dstY == lastDstY || dstY < 0 || dstY >= pageHeight) continue;
      lastDstY = dstY;

      auto valueAt = [&](const int sx) -> uint8_t {
        return static_cast<uint8_t>((packed[sx / 4] >> (6 - ((sx * 2) % 8))) & 0x3);
      };

      int firstInk = -1;
      int lastInk = -1;
      for (int sx = 0; sx < srcW; ++sx) {
        if (valueAt(sx) < 3) {
          if (firstInk < 0) firstInk = sx;
          lastInk = sx;
        }
      }
      if (firstInk < 0) continue;

      for (int dx = 0; dx < dstW; ++dx) {
        const int sx = std::min(srcW - 1, static_cast<int>(dx / scale));
        if (sx < firstInk || sx > lastInk) continue;

        const int outX = dstX0 + dx;
        if (outX < 0 || outX >= pageWidth) continue;

        const uint8_t v = valueAt(sx);
        bool black = false;
        if (v == 0) {
          black = true;
        } else if (v == 1) {
          black = ((outX + dstY) & 1) == 0;
        } else if (v == 2) {
          black = ((outX & 1) == 0) && ((dstY & 1) == 0);
        }
        // v==3 is deliberately drawn white INSIDE the silhouette span.
        renderer.drawPixel(outX, dstY, black);
      }
    }

    delete[] packed;
    delete[] rawRow;
    file.close();
    return OverlayDrawResult::Drawn;
  };

  auto tryDrawPngOverlay = [&](const std::string& filename) -> OverlayDrawResult {
    if (!Storage.exists(filename.c_str())) {
      LOG_DBG("SLP", "PNG overlay not found: %s", filename.c_str());
      return OverlayDrawResult::NotFound;
    }

    constexpr size_t MIN_FREE_HEAP = 60 * 1024;  // PNG decoder ~42 KB + overhead
    if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
      LOG_ERR("SLP", "Not enough heap for PNG overlay decoder: %u free, need %u for %s", ESP.getFreeHeap(),
              static_cast<unsigned>(MIN_FREE_HEAP), filename.c_str());
      return OverlayDrawResult::Failed;
    }
    PNG* png = new (std::nothrow) PNG();
    if (!png) {
      LOG_ERR("SLP", "Failed to allocate PNG overlay decoder for %s", filename.c_str());
      return OverlayDrawResult::Failed;
    }

    int rc = png->open(filename.c_str(), pngSleepOpen, pngSleepClose, pngSleepRead, pngSleepSeek, pngOverlayDraw);
    if (rc != PNG_SUCCESS) {
      delete png;
      LOG_ERR("SLP", "PNG overlay open failed for %s: %d", filename.c_str(), rc);
      return OverlayDrawResult::Failed;
    }

    const int srcW = png->getWidth(), srcH = png->getHeight();
    float yScale = 1.0f;
    int dstW = srcW, dstH = srcH;
    if (srcW > pageWidth || srcH > pageHeight) {
      const float scaleX = (float)pageWidth / srcW, scaleY = (float)pageHeight / srcH;
      const float scale = (scaleX < scaleY) ? scaleX : scaleY;
      dstW = (int)(srcW * scale);
      dstH = (int)(srcH * scale);
      yScale = (float)dstH / srcH;
    }

    PngOverlayCtx ctx;
    ctx.renderer = &renderer;
    ctx.screenW = pageWidth;
    ctx.screenH = pageHeight;
    ctx.srcWidth = srcW;
    ctx.dstWidth = dstW;
    ctx.dstX = (pageWidth - dstW) / 2;
    ctx.dstY = (pageHeight - dstH) / 2;
    ctx.yScale = yScale;
    ctx.lastDstY = -1;
    ctx.transparentColor = -2;  // will be resolved on first draw callback (after tRNS is parsed)
    ctx.pngObj = png;

    LOG_INF("SLP", "Drawing PNG overlay: %s", filename.c_str());
    rc = png->decode(&ctx, 0);
    png->close();
    delete png;
    if (rc != PNG_SUCCESS) {
      LOG_ERR("SLP", "PNG overlay decode failed for %s: %d", filename.c_str(), rc);
      return OverlayDrawResult::Failed;
    }
    return OverlayDrawResult::Drawn;
  };

  bool overlayDrawn = false;
  bool overlayCandidateFailed = false;
  SleepImageSelection selection;
  auto trySelectedOverlay = [&](const SleepImageSelection& image) {
    LOG_INF("SLP", "Selected overlay image: %s", image.path.c_str());
    const OverlayDrawResult result = image.isPng ? tryDrawPngOverlay(image.path) : tryDrawOverlay(image.path);
    overlayDrawn = result == OverlayDrawResult::Drawn;
    overlayCandidateFailed = overlayCandidateFailed || result == OverlayDrawResult::Failed;
  };

  if (selectPinnedSleepImage(SleepImageMode::Overlay, effectivePinnedSleepImagePath(), selection)) {
    trySelectedOverlay(selection);
  }
  if (!overlayDrawn && selectRandomSleepImage(SleepImageMode::Overlay, selection)) {
    trySelectedOverlay(selection);
  }

  if (!overlayDrawn) {
    const OverlayDrawResult result = tryDrawOverlay("/sleep.bmp");
    overlayDrawn = result == OverlayDrawResult::Drawn;
    overlayCandidateFailed = overlayCandidateFailed || result == OverlayDrawResult::Failed;
  }
  if (!overlayDrawn) {
    const OverlayDrawResult result = tryDrawPngOverlay("/sleep.png");
    overlayDrawn = result == OverlayDrawResult::Drawn;
    overlayCandidateFailed = overlayCandidateFailed || result == OverlayDrawResult::Failed;
  }

  if (!overlayDrawn) {
    if (overlayCandidateFailed) {
      LOG_ERR("SLP", "Overlay image was found but could not be drawn; falling back to default sleep screen");
      renderer.setOrientation(savedOrientation);
      return renderDefaultSleepScreen();
    }
    if (!backgroundAvailable) {
      LOG_DBG("SLP", "No overlay image or current screen snapshot available, falling back to default sleep screen");
      renderer.setOrientation(savedOrientation);
      return renderDefaultSleepScreen();
    }
    LOG_DBG("SLP", "No overlay image found, displaying background without overlay");
  }

  renderer.setOrientation(savedOrientation);
  // The grayscale re-render has no mask for the overlay image. If an overlay was
  // drawn, keep the composited BW frame intact instead of painting page glyphs
  // over the sleep image.
  const bool shouldRunGrayscalePass = shouldUseReaderPageBackground && backgroundSupportsGrayscale && !overlayDrawn &&
                                      (backgroundWasRebuilt || (overlayBackgroundBufferStored && !path.empty()));
  // The overlay compositor now paints white pixels inside the image silhouette
  // explicitly, so a forced full-screen waveform is no longer necessary.
  // Keep sleep entry quiet: use the normal HALF_REFRESH for the composited frame.
  // This removes the visible full-flash "disco" while preserving the opaque
  // character mask and the saved reader page outside it.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH,
                         !shouldRunGrayscalePass && TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);

  if (!shouldRunGrayscalePass) {
    return;
  }

  if (!renderer.storeBwBuffer()) {
    LOG_ERR("SLP", "Overlay: failed to store BW buffer for grayscale pass");
    return;
  }

  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!renderSavedReaderPage()) {
    LOG_ERR("SLP", "Overlay: failed to rebuild page for grayscale LSB pass");
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    return;
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!renderSavedReaderPage()) {
    LOG_ERR("SLP", "Overlay: failed to rebuild page for grayscale MSB pass");
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    return;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
}
