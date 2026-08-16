#include "EpubReaderActivity.h"

#include <Arduino.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <HalSystem.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>

#include "../settings/KOReaderSettingsActivity.h"
#include "BookStatsActivity.h"
#include "ClippingListActivity.h"
#include "ClippingSelectionActivity.h"
#include "ClippingUtils.h"
#include "DictionaryActivity.h"
#include "LegacyRenderDiagnostics.h"
#include "activities/util/LegacyRenderPromptActivity.h"
#include "activities/util/LegacyGridDiagnosticsActivity.h"
#include "InkMODSettings.h"
#include "InkMODState.h"
#include "EpubReaderBookmarkListActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "GlobalActions.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/boot_sleep/SleepCoverAssets.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookMoveUtils.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long longPressMenuMs = 600;
constexpr uint16_t DEFAULT_AUTO_PAGE_TURN_INTERVAL_S = 30;
constexpr uint16_t MIN_AUTO_PAGE_TURN_INTERVAL_S = 5;
constexpr uint16_t MAX_AUTO_PAGE_TURN_INTERVAL_S = 120;
constexpr int MAX_PAGE_LOAD_RETRIES = 3;
constexpr uint32_t NAVIGATION_COALESCE_MS = 180;
constexpr uint8_t READER_SETTINGS_FILE_VERSION = 1;
constexpr char READER_SETTINGS_FILE_NAME[] = "/reader_settings.bin";
constexpr char FALLBACK_FONT_SECTION_CACHE_SUFFIX[] = "_fallback_font";
constexpr char SAFE_SECTION_CACHE_SUFFIX[] = "_safe";
constexpr char SURVIVAL_SECTION_CACHE_SUFFIX[] = "_survival";
constexpr unsigned long MIN_READING_STATS_PAGE_MS = 2000UL;
constexpr uint32_t MIN_READING_PACE_SAMPLE_SECONDS = 2;
constexpr uint16_t MIN_STORED_TIME_LEFT_PACE_SAMPLE_COUNT = 3;
constexpr uint16_t MIN_SESSION_TIME_LEFT_PACE_SAMPLE_COUNT = 10;
constexpr uint16_t MIN_STORED_PACE_SLOWER_RECOVERY_SESSION_SAMPLES = 10;
constexpr uint8_t STORED_PACE_SLOWER_RECOVERY_PERCENT = 110;
constexpr uint16_t MIN_STORED_PACE_FASTER_RECOVERY_SESSION_SAMPLES = 15;
constexpr uint8_t STORED_PACE_FASTER_RECOVERY_PERCENT = 90;
constexpr uint8_t BOOK_PROGRESS_ESTIMATE_FLOOR_PERCENT = 90;
constexpr uint8_t PUBLISHER_PAGE_NUMBER_LEFT_MARGIN_MIN = 15;
constexpr int PUBLISHER_PAGE_NUMBER_X = 5;

struct SectionMemoryConfig {
  reader::ReaderMemoryMode mode;
  const char* suffix;
  int fontId;
  bool hyphenationEnabled;
  bool embeddedStyle;
  uint8_t imageRendering;
  bool bionicReadingEnabled;
  bool guideReadingEnabled;
};

const char* readerMemoryModeName(const reader::ReaderMemoryMode mode) {
  switch (mode) {
    case reader::ReaderMemoryMode::Normal:
      return "normal";
    case reader::ReaderMemoryMode::Safe:
      return "safe";
    case reader::ReaderMemoryMode::Survival:
      return "survival";
    case reader::ReaderMemoryMode::Unavailable:
      return "unavailable";
  }
  return "unknown";
}

SectionMemoryConfig sectionMemoryConfig(const reader::ReaderMemoryMode mode, const int readerFontId,
                                        const int fallbackFontId) {
  if (mode == reader::ReaderMemoryMode::Normal) {
    return {mode,
            "",
            readerFontId,
            SETTINGS.hyphenationEnabled != 0,
            SETTINGS.embeddedStyle != 0,
            SETTINGS.imageRendering,
            SETTINGS.bionicReadingEnabled != 0,
            SETTINGS.guideReadingEnabled != 0};
  }
  if (mode == reader::ReaderMemoryMode::Safe) {
    // Safe mode may disable optional reading effects, but it must preserve the
    // publisher's layout and illustrations. A low-memory retry must never turn
    // a correctly formatted EPUB into a plain left-aligned text edition.
    return {mode,
            SAFE_SECTION_CACHE_SUFFIX,
            readerFontId,
            SETTINGS.hyphenationEnabled != 0,
            SETTINGS.embeddedStyle != 0,
            SETTINGS.imageRendering,
            false,
            false};
  }
  // Survival mode switches to the cheaper built-in font and drops optional
  // reading effects/hyphenation, while still preserving book CSS and images.
  return {reader::ReaderMemoryMode::Survival,
          SURVIVAL_SECTION_CACHE_SUFFIX,
          fallbackFontId,
          false,
          SETTINGS.embeddedStyle != 0,
          SETTINGS.imageRendering,
          false,
          false};
}

uint32_t pagesCentipages(const float pages) {
  if (pages <= 0.0f) {
    return 0;
  }
  if (pages >= static_cast<float>(UINT32_MAX) / 100.0f) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(pages * 100.0f + 0.5f);
}

bool hasEnoughPaceSamplesForTimeLeft(const BookReadingStats& stats) {
  return stats.avgSecondsPerForwardPage > 0 && stats.paceSampleCount >= MIN_STORED_TIME_LEFT_PACE_SAMPLE_COUNT;
}

std::string confirmationHeading(const StrId actionLabelId) {
  return std::string(tr(STR_CONFIRM)) + ": " + std::string(I18N.get(actionLabelId));
}

uint8_t largestBlockPercent(const MemoryBudget::HeapSnapshot& heap) {
  if (heap.freeHeap == 0) {
    return 0;
  }
  return static_cast<uint8_t>(std::min<uint32_t>(100, (heap.maxAllocHeap * 100U) / heap.freeHeap));
}

struct TiledGrayscaleTimings {
  unsigned long grayLsb = 0;
  unsigned long grayMsb = 0;
  unsigned long grayDisplay = 0;
  unsigned long cleanup = 0;
};

bool runTiledGrayscalePass(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                           const int marginTop, const bool foregroundBlack, const bool needsTextGrayscale,
                           const bool needsImageGrayscale, TiledGrayscaleTimings& timings) {
  if ((!needsTextGrayscale && !needsImageGrayscale) || !renderer.supportsStripGrayscale()) {
    return false;
  }

  constexpr int STRIP_ROWS = 80;
  const int displayHeight = renderer.getDisplayHeight();
  const int displayWidthBytes = renderer.getDisplayWidthBytes();
  auto scratch =
      std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[static_cast<size_t>(displayWidthBytes) * STRIP_ROWS]);
  if (!scratch) {
    LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); falling back to BW snapshot",
            displayWidthBytes * STRIP_ROWS);
    return false;
  }

  // Keep the live BW framebuffer intact, stream grayscale planes by row-band,
  // then re-sync the controller BW state from the framebuffer.
  const auto renderPlane = [&](const GfxRenderer::RenderMode mode, const bool lsbPlane) {
    renderer.setRenderMode(mode);
    for (int y = 0; y < displayHeight; y += STRIP_ROWS) {
      const int rows = std::min(STRIP_ROWS, displayHeight - y);
      renderer.beginStripTarget(scratch.get(), y, rows);
      renderer.clearScreen(0x00);
      if (needsTextGrayscale) {
        page.render(renderer, fontId, marginLeft, marginTop, foregroundBlack);
      } else {
        page.renderImages(renderer, fontId, marginLeft, marginTop);
      }
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(lsbPlane, scratch.get(), y, rows);
    }
  };

  renderPlane(GfxRenderer::GRAYSCALE_LSB, true);
  timings.grayLsb = millis();

  renderPlane(GfxRenderer::GRAYSCALE_MSB, false);
  timings.grayMsb = millis();

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  timings.grayDisplay = millis();
  renderer.cleanupGrayscaleWithFrameBuffer();
  timings.cleanup = millis();
  return true;
}

void drawToastBuffer(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const bool toastBackgroundBlack = ReaderUtils::readerForegroundBlack();
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, toastBackgroundBlack);
  renderer.drawRect(toastX, toastY, toastW, toastH, !toastBackgroundBlack);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, !toastBackgroundBlack);
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  drawToastBuffer(renderer, msg);
  renderer.displayBuffer();
}

void drawPublisherPageMarkers(const GfxRenderer& renderer, const Page& page, const int contentTop,
                              const int contentBottom, const bool foregroundBlack = true) {
  if (!SETTINGS.publisherPageNumbers || page.publisherPageMarkers.empty()) {
    return;
  }

  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int lineStep = std::max(1, lineHeight - 2);
  const int availableHeight = contentBottom - contentTop;
  if (availableHeight <= lineHeight) {
    return;
  }

  for (const auto& marker : page.publisherPageMarkers) {
    const char* label = marker.label;
    if (!label || label[0] == '\0') {
      continue;
    }

    bool hasNonAscii = false;
    int labelLen = 0;
    int maxCharWidth = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(label); *p != '\0'; p++) {
      if (*p >= 0x80) {
        hasNonAscii = true;
        break;
      }
      if (*p <= ' ') {
        continue;
      }
      const char ch[2] = {static_cast<char>(*p), '\0'};
      maxCharWidth = std::max(maxCharWidth, renderer.getTextWidth(SMALL_FONT_ID, ch));
      labelLen++;
    }

    if (labelLen == 0) {
      continue;
    }

    const int x = PUBLISHER_PAGE_NUMBER_X;
    if (hasNonAscii) {
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
      const int maxY = contentBottom - lineHeight;
      const int y = maxY < contentTop ? contentTop : std::min(std::max(contentTop + marker.yPos, contentTop), maxY);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + textWidth, label, foregroundBlack);
      continue;
    }

    const int markerHeight = lineHeight + (labelLen - 1) * lineStep;
    int y = contentTop + marker.yPos - lineHeight / 2;
    const int maxY = contentBottom - markerHeight;
    y = maxY < contentTop ? contentTop : std::min(std::max(y, contentTop), maxY);

    int row = 0;
    for (const char* p = label; *p != '\0'; p++) {
      if (static_cast<unsigned char>(*p) <= ' ') {
        continue;
      }
      const char ch[2] = {*p, '\0'};
      const int charWidth = renderer.getTextWidth(SMALL_FONT_ID, ch);
      renderer.drawText(SMALL_FONT_ID, x + (maxCharWidth - charWidth) / 2, y + row * lineStep, ch, foregroundBlack);
      row++;
    }
  }
}

uint8_t effectiveReaderLeftMargin() {
  return SETTINGS.publisherPageNumbers ? std::max<uint8_t>(SETTINGS.screenMargin, PUBLISHER_PAGE_NUMBER_LEFT_MARGIN_MIN)
                                       : SETTINGS.screenMargin;
}

struct ReaderViewportLayout {
  int marginTop;
  int marginRight;
  int marginBottom;
  int marginLeft;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
};

ReaderViewportLayout computeReaderViewportLayout(GfxRenderer& renderer, const bool automaticPageTurnActive) {
  ReaderViewportLayout layout{};
  renderer.getOrientedViewableTRBL(&layout.marginTop, &layout.marginRight, &layout.marginBottom, &layout.marginLeft);
  layout.marginLeft += effectiveReaderLeftMargin();
  layout.marginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const int topStatusBarReservedHeight = ReaderUtils::getTopClockStatusBarReservedHeight();
  if (topStatusBarReservedHeight > 0) {
    layout.marginTop += std::max(static_cast<int>(SETTINGS.screenMargin),
                                 topStatusBarReservedHeight + ReaderUtils::STATUS_BAR_TEXT_PADDING);
  } else {
    layout.marginTop += SETTINGS.screenMargin;
  }

  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    layout.marginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin +
                                      ReaderUtils::STATUS_BAR_TEXT_PADDING));
  } else {
    layout.marginBottom +=
        std::max(SETTINGS.screenMargin, static_cast<uint8_t>(statusBarHeight + ReaderUtils::STATUS_BAR_TEXT_PADDING));
  }

  layout.viewportWidth = renderer.getScreenWidth() - layout.marginLeft - layout.marginRight;
  layout.viewportHeight = renderer.getScreenHeight() - layout.marginTop - layout.marginBottom;
  return layout;
}

bool releaseReaderSdFontCachesForLowMemory(const GfxRenderer& renderer, const char* tag, const char* reason) {
  const int fontId = SETTINGS.getReaderFontId();
  if (!renderer.isSdCardFont(fontId)) {
    return false;
  }

#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const auto before = MemoryBudget::snapshot();
#endif
  // Large chapters used to hard-release all persistent SD-font caches here,
  // which looked like the firmware "unloaded" the selected font and made a
  // Safe retry switch to the built-in fallback. First trim only page-local
  // glyph data; keep metrics/kern/ligature caches resident.
  if (!renderer.trimSdCardFontForLowMemory(fontId)) {
    return false;
  }
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const auto after = MemoryBudget::snapshot();
  LOG_DBG(tag, "Released SD font caches after %s: free=%u->%u maxAlloc=%u->%u", reason, before.freeHeap, after.freeHeap,
          before.maxAllocHeap, after.maxAllocHeap);
#endif
  return true;
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

bool isSnippetWhitespace(const std::string& word) {
  if (word.empty()) return true;
  return std::all_of(word.begin(), word.end(),
                     [](const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; });
}

void buildBookmarkSnippet(const Page& page, char* out, const size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  size_t len = 0;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    if (!line.getBlock()) continue;
    const auto& words = line.getBlock()->getWords();
    for (const auto& word : words) {
      if (isSnippetWhitespace(word)) continue;
      const size_t separatorLen = len > 0 ? 1 : 0;
      const size_t wordLen = word.size();
      if (len + separatorLen + wordLen >= outSize) return;
      if (separatorLen > 0) out[len++] = ' ';
      memcpy(out + len, word.c_str(), wordLen);
      len += wordLen;
      out[len] = '\0';
    }
  }
}

uint16_t clampAutoPageTurnIntervalSeconds(const uint16_t seconds) {
  return std::clamp(seconds, MIN_AUTO_PAGE_TURN_INTERVAL_S, MAX_AUTO_PAGE_TURN_INTERVAL_S);
}

uint16_t loadAutoPageTurnIntervalSeconds(const std::string& cachePath) {
  FsFile f;
  if (!Storage.openFileForRead("ERS", cachePath + READER_SETTINGS_FILE_NAME, f)) {
    return DEFAULT_AUTO_PAGE_TURN_INTERVAL_S;
  }

  uint8_t data[3] = {};
  const int n = f.read(data, sizeof(data));
  f.close();

  if (n != static_cast<int>(sizeof(data)) || data[0] != READER_SETTINGS_FILE_VERSION) {
    LOG_DBG("ERS", "Reader settings missing or version mismatch, using defaults");
    return DEFAULT_AUTO_PAGE_TURN_INTERVAL_S;
  }

  const uint16_t seconds = static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[2]) << 8);
  if (seconds == 0) {
    return DEFAULT_AUTO_PAGE_TURN_INTERVAL_S;
  }
  return clampAutoPageTurnIntervalSeconds(seconds);
}

bool saveAutoPageTurnIntervalSeconds(const std::string& cachePath, const uint16_t seconds) {
  FsFile f;
  if (!Storage.openFileForWrite("ERS", cachePath + READER_SETTINGS_FILE_NAME, f)) {
    LOG_ERR("ERS", "Could not open reader settings file for write");
    return false;
  }

  const uint16_t clampedSeconds = clampAutoPageTurnIntervalSeconds(seconds);
  uint8_t data[3];
  data[0] = READER_SETTINGS_FILE_VERSION;
  data[1] = clampedSeconds & 0xFF;
  data[2] = (clampedSeconds >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  f.close();
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving reader settings: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    return false;
  }
  return true;
}

void formatCompactTimeLeft(const uint32_t seconds, char* out, const size_t outSize) {
  if (!out || outSize == 0) return;
  if (seconds < 60) {
    snprintf(out, outSize, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }

  const uint32_t minutes = (seconds + 30U) / 60U;
  if (minutes < 60) {
    snprintf(out, outSize, "%lu %s", static_cast<unsigned long>(minutes), tr(STR_UNIT_MIN_SHORT));
    return;
  }

  const uint32_t hours = minutes / 60U;
  const uint32_t remainingMinutes = minutes % 60U;
  if (remainingMinutes == 0) {
    snprintf(out, outSize, "%lu%s", static_cast<unsigned long>(hours), tr(STR_UNIT_HOUR_SHORT));
  } else {
    snprintf(out, outSize, "%lu%s %lu %s", static_cast<unsigned long>(hours), tr(STR_UNIT_HOUR_SHORT),
             static_cast<unsigned long>(remainingMinutes), tr(STR_UNIT_MIN_SHORT));
  }
}

// SD card folder finished books are moved into. Single source of truth for the path.
constexpr char READ_FOLDER[] = "/Read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // excludes NUL
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

// Relocate a finished book into /Read/, then migrate path-keyed state such as
// cache files, bookmarks, recents, and resume path.
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath, const std::string& title,
                                  const std::string& author) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_MOVE_TO_READ_FAILED_TITLE));
    snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_MOVE_TO_READ_FAILED_BODY),
             title.c_str());
    APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
    APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
    return;
  }

  BookMoveUtils::migrateMovedEpubState(srcPath, dstPath, oldCachePath, title, author,
                                       !SETTINGS.removeReadBooksFromRecents);
}

}  // namespace

float EpubReaderActivity::getCurrentBookProgressPercent() const {
  if (!epub || !section || section->pageCount == 0 || epub->getBookSize() == 0) {
    return 0.0f;
  }

  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
  return epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
}

void EpubReaderActivity::pauseReadingPaceTimer(const char* reason) {
  recordCurrentPageReadingTime(reason);
  pageShownAtMs = 0UL;
  paceSampleWarmupPending = true;
}

void EpubReaderActivity::resumeReadingPaceTimer(const char*) {
  if (section && section->pageCount > 0 && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    pageShownAtMs = millis();
  } else {
    pageShownAtMs = 0UL;
  }
}

void EpubReaderActivity::armReadingPaceWarmup(const char*) { paceSampleWarmupPending = true; }

bool EpubReaderActivity::forwardPageReadElapsed(uint32_t& seconds, const char*) const {
  seconds = 0;
  if (!SETTINGS.shouldTrackReadingStats() || pageShownAtMs == 0UL) {
    return false;
  }

  const unsigned long elapsedMs = millis() - pageShownAtMs;
  if (elapsedMs < MIN_READING_STATS_PAGE_MS) {
    return false;
  }

  seconds = static_cast<uint32_t>(elapsedMs / 1000UL);
  return true;
}

bool EpubReaderActivity::currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const {
  seconds = 0;
  if (!SETTINGS.shouldTrackReadingStats() || pageShownAtMs == 0UL) {
    return false;
  }

  const unsigned long elapsedMs = millis() - pageShownAtMs;
  const uint32_t elapsedSeconds = static_cast<uint32_t>(elapsedMs / 1000UL);
  if (elapsedSeconds == 0) {
    return false;
  }

  const uint32_t thresholdSeconds = SETTINGS.getReadingIdleTimeThresholdSeconds();
  if (elapsedSeconds > thresholdSeconds) {
    LOG_DBG("ERS", "Reading time interval rejected as idle: source=%s seconds=%lu threshold=%lu",
            source ? source : "unknown", static_cast<unsigned long>(elapsedSeconds),
            static_cast<unsigned long>(thresholdSeconds));
    return false;
  }

  seconds = elapsedSeconds;
  return true;
}

void EpubReaderActivity::recordCurrentPageReadingTime(const char* source) {
  uint32_t seconds = 0;
  if (currentPageReadingSecondsForStats(seconds, source)) {
    sessionReadingSeconds = sessionReadingSeconds > UINT32_MAX - seconds ? UINT32_MAX : sessionReadingSeconds + seconds;
  }
  pageShownAtMs = 0UL;
}

void EpubReaderActivity::recordForwardPagePaceSample(uint32_t seconds, const char* source) {
  if (paceSampleWarmupPending) {
    paceSampleWarmupPending = false;
    return;
  }

  if (seconds < MIN_READING_PACE_SAMPLE_SECONDS) {
    LOG_DBG("ERS", "Time-left pace sample rejected: source=%s seconds=%lu minSeconds=%lu", source ? source : "unknown",
            static_cast<unsigned long>(seconds), static_cast<unsigned long>(MIN_READING_PACE_SAMPLE_SECONDS));
    return;
  }

  const uint32_t maxReadingPaceSampleSeconds = SETTINGS.getReadingIdleTimeThresholdSeconds();
  if (seconds > maxReadingPaceSampleSeconds) {
    LOG_DBG("ERS", "Time-left pace sample rejected: source=%s seconds=%lu maxSeconds=%lu", source ? source : "unknown",
            static_cast<unsigned long>(seconds), static_cast<unsigned long>(maxReadingPaceSampleSeconds));
    return;
  }

  if (sessionPaceSampleCount < UINT16_MAX && sessionPaceSampleSeconds <= UINT32_MAX - static_cast<uint32_t>(seconds)) {
    sessionPaceSampleSeconds += seconds;
    sessionPaceSampleCount++;
  }

  stats.recordForwardPageRead(seconds);
  recoverStoredPaceFromSession("pace_sample");
}

bool EpubReaderActivity::getSessionAveragePaceSeconds(uint16_t& avgSeconds) const {
  avgSeconds = 0;
  if (sessionPaceSampleCount < MIN_SESSION_TIME_LEFT_PACE_SAMPLE_COUNT || sessionPaceSampleSeconds == 0) {
    return false;
  }
  const uint32_t roundedAvg =
      (sessionPaceSampleSeconds + static_cast<uint32_t>(sessionPaceSampleCount / 2)) / sessionPaceSampleCount;
  avgSeconds = static_cast<uint16_t>(std::min<uint32_t>(roundedAvg, UINT16_MAX));
  return avgSeconds > 0;
}

void EpubReaderActivity::recoverStoredPaceFromSession(const char* reason) {
  if (stats.avgSecondsPerForwardPage == 0) {
    return;
  }

  uint16_t sessionAvg = 0;
  if (!getSessionAveragePaceSeconds(sessionAvg)) {
    return;
  }

  const uint32_t slowerRecoveryThreshold =
      (static_cast<uint32_t>(stats.avgSecondsPerForwardPage) * STORED_PACE_SLOWER_RECOVERY_PERCENT + 99U) / 100U;
  if (sessionPaceSampleCount >= MIN_STORED_PACE_SLOWER_RECOVERY_SESSION_SAMPLES &&
      sessionAvg >= slowerRecoveryThreshold) {
    LOG_DBG("ERS",
            "Time-left stored pace recovered: reason=%s direction=slower avg=%u->%u samples=%u sessionSamples=%u "
            "threshold=%lu",
            reason ? reason : "unknown", stats.avgSecondsPerForwardPage, sessionAvg, stats.paceSampleCount,
            sessionPaceSampleCount, static_cast<unsigned long>(slowerRecoveryThreshold));
    stats.avgSecondsPerForwardPage = sessionAvg;
    return;
  }

  const uint32_t fasterRecoveryThreshold =
      (static_cast<uint32_t>(stats.avgSecondsPerForwardPage) * STORED_PACE_FASTER_RECOVERY_PERCENT) / 100U;
  if (sessionPaceSampleCount >= MIN_STORED_PACE_FASTER_RECOVERY_SESSION_SAMPLES &&
      sessionAvg <= fasterRecoveryThreshold) {
    LOG_DBG("ERS",
            "Time-left stored pace recovered: reason=%s direction=faster avg=%u->%u samples=%u sessionSamples=%u "
            "threshold=%lu",
            reason ? reason : "unknown", stats.avgSecondsPerForwardPage, sessionAvg, stats.paceSampleCount,
            sessionPaceSampleCount, static_cast<unsigned long>(fasterRecoveryThreshold));
    stats.avgSecondsPerForwardPage = sessionAvg;
  }
}

bool EpubReaderActivity::getTimeLeftPaceSeconds(uint16_t& avgSeconds, const char*& source,
                                                uint16_t& sampleCount) const {
  if (getSessionAveragePaceSeconds(avgSeconds)) {
    source = "session_pace";
    sampleCount = sessionPaceSampleCount;
    return true;
  }
  if (hasEnoughPaceSamplesForTimeLeft(stats)) {
    avgSeconds = stats.avgSecondsPerForwardPage;
    source = "stored_pace";
    sampleCount = stats.paceSampleCount;
    return true;
  }
  avgSeconds = 0;
  source = "none";
  sampleCount = 0;
  return false;
}

bool EpubReaderActivity::estimateRemainingTimeLeftPages(const bool bookEstimate, float& remainingPages) const {
  remainingPages = 0.0f;
  if (!epub || !section || section->pageCount == 0) {
    return false;
  }

  if (!bookEstimate) {
    const int remainingChapterPages = static_cast<int>(section->pageCount) - section->currentPage - 1;
    if (remainingChapterPages <= 0) {
      return false;
    }
    remainingPages = static_cast<float>(remainingChapterPages);
  } else {
    const size_t bookSize = epub->getBookSize();
    if (bookSize == 0) {
      return false;
    }

    const size_t prevChapterSize = currentSpineIndex >= 1 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
    const size_t cumulativeSize = epub->getCumulativeSpineItemSize(currentSpineIndex);
    if (cumulativeSize <= prevChapterSize) {
      return false;
    }

    const float chapterSize = static_cast<float>(cumulativeSize - prevChapterSize);
    const float completedCurrentChapter =
        (static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount)) * chapterSize;
    const float completedBookSize = static_cast<float>(prevChapterSize) + completedCurrentChapter;
    if (completedBookSize >= static_cast<float>(bookSize)) {
      return false;
    }

    const float bytesPerPage = chapterSize / static_cast<float>(section->pageCount);
    if (bytesPerPage <= 0.0f) {
      return false;
    }
    remainingPages = (static_cast<float>(bookSize) - completedBookSize) / bytesPerPage;
  }

  return remainingPages > 0.0f;
}

bool EpubReaderActivity::estimateProgressTimeLeftSeconds(uint32_t& seconds) const {
  seconds = 0;
  const float progressPercent = getCurrentBookProgressPercent();
  uint32_t currentPageSeconds = 0;
  uint32_t sessionSeconds = sessionReadingSeconds;
  if (SETTINGS.shouldTrackReadingStats() &&
      currentPageReadingSecondsForStats(currentPageSeconds, "time_left_preview")) {
    sessionSeconds =
        sessionSeconds > UINT32_MAX - currentPageSeconds ? UINT32_MAX : sessionSeconds + currentPageSeconds;
  }
  const uint32_t elapsedReadingSeconds =
      stats.totalReadingSeconds > UINT32_MAX - sessionSeconds ? UINT32_MAX : stats.totalReadingSeconds + sessionSeconds;

  if (progressPercent <= 0.0f || progressPercent >= 100.0f || elapsedReadingSeconds < 120) {
    return false;
  }

  const float progress = progressPercent / 100.0f;
  const float estimate = (static_cast<float>(elapsedReadingSeconds) * (1.0f - progress)) / progress;
  if (estimate <= 0.0f) {
    return false;
  }

  seconds = static_cast<uint32_t>(std::min(estimate + 0.5f, static_cast<float>(UINT32_MAX)));
  return seconds > 0;
}

bool EpubReaderActivity::estimateTimeLeftSeconds(const bool bookEstimate, uint32_t& seconds) const {
  seconds = 0;
  uint16_t paceSeconds = 0;
  const char* paceSource = "none";
  uint16_t paceSampleCount = 0;
  const bool hasPace = getTimeLeftPaceSeconds(paceSeconds, paceSource, paceSampleCount);

  uint32_t paceEstimateSeconds = 0;
  bool hasPaceEstimate = false;
  float remainingPages = 0.0f;
  if (hasPace && estimateRemainingTimeLeftPages(bookEstimate, remainingPages)) {
    const float estimatedSeconds = remainingPages * static_cast<float>(paceSeconds);
    if (estimatedSeconds > 0.0f) {
      paceEstimateSeconds = static_cast<uint32_t>(std::min(estimatedSeconds + 0.5f, static_cast<float>(UINT32_MAX)));
      hasPaceEstimate = paceEstimateSeconds > 0;
    }
  }

  uint32_t progressEstimateSeconds = 0;
  bool hasProgressEstimate = false;
  if (bookEstimate && hasPace) {
    hasProgressEstimate = estimateProgressTimeLeftSeconds(progressEstimateSeconds);
  }
  if (!hasPaceEstimate && !hasProgressEstimate) {
    return false;
  }

  if (!hasPaceEstimate) {
    seconds = progressEstimateSeconds;
  } else if (hasProgressEstimate) {
    const uint32_t progressFloorSeconds = static_cast<uint32_t>(std::min<uint64_t>(
        (static_cast<uint64_t>(progressEstimateSeconds) * BOOK_PROGRESS_ESTIMATE_FLOOR_PERCENT + 99ULL) / 100ULL,
        UINT32_MAX));
    if (paceEstimateSeconds < progressFloorSeconds) {
      seconds = progressFloorSeconds;
    } else {
      seconds = paceEstimateSeconds;
    }
  } else {
    seconds = paceEstimateSeconds;
  }
  return seconds > 0;
}

bool EpubReaderActivity::formatTimeLeftLabel(char* buf, const size_t len) const {
  if (!buf || len == 0 || SETTINGS.statusBarTimeLeft == InkMODSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_HIDE) {
    return false;
  }

  const bool bookEstimate = SETTINGS.statusBarTimeLeft == InkMODSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_BOOK;
  uint32_t seconds = 0;
  if (estimateTimeLeftSeconds(bookEstimate, seconds)) {
    formatCompactTimeLeft(seconds, buf, len);
    return true;
  }

  uint16_t paceSeconds = 0;
  const char* paceSource = "none";
  uint16_t paceSampleCount = 0;
  if (!getTimeLeftPaceSeconds(paceSeconds, paceSource, paceSampleCount)) {
    float remainingPages = 0.0f;
    if (!estimateRemainingTimeLeftPages(bookEstimate, remainingPages)) {
      return false;
    }
    snprintf(buf, len, "%s", tr(STR_TIME_LEFT_CALCULATING));
    return true;
  }

  return false;
}

void EpubReaderActivity::initializeCompletionPromptTrigger() {
  completionTriggerSpineIndex = -1;
  completionTriggerSpineProgress = 1.0f;
  completionPromptQueued = false;
  completionPromptShown = stats.isCompleted;
  completionTriggerSeenBelow = false;
  completionTriggerCrossed = false;
  lastAtOrPastCompletionTrigger = false;

  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  const int spineCount = epub->getSpineItemsCount();
  if (bookSize == 0 || spineCount <= 0) {
    return;
  }

  size_t targetSize = (bookSize / 100) * 99 + (bookSize % 100) * 99 / 100;
  if (targetSize >= bookSize) {
    targetSize = bookSize - 1;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;

  completionTriggerSpineIndex = targetSpineIndex;
  completionTriggerSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);

  if (completionTriggerSpineProgress < 0.0f) {
    completionTriggerSpineProgress = 0.0f;
  } else if (completionTriggerSpineProgress > 1.0f) {
    completionTriggerSpineProgress = 1.0f;
  }
}

bool EpubReaderActivity::isAtOrPastCompletionTrigger() const {
  if (!epub || !section || section->pageCount == 0 || completionTriggerSpineIndex < 0) {
    return false;
  }

  if (currentSpineIndex > completionTriggerSpineIndex) {
    return true;
  }
  if (currentSpineIndex < completionTriggerSpineIndex) {
    return false;
  }

  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
  return chapterProgress >= completionTriggerSpineProgress;
}

bool EpubReaderActivity::shouldQueueCompletionPromptOnChapterExit() const {
  if (completionPromptShown || completionPromptQueued || stats.isCompleted || footnoteDepth > 0 ||
      !completionTriggerCrossed || !epub || !section || section->pageCount == 0 || completionTriggerSpineIndex < 0) {
    return false;
  }

  if (currentSpineIndex != completionTriggerSpineIndex) {
    return false;
  }

  return section->currentPage >= section->pageCount - 1;
}

void EpubReaderActivity::queueCompletionPromptIfNeeded() {
  if (completionPromptShown || completionPromptQueued || stats.isCompleted || footnoteDepth > 0) {
    return;
  }

  const bool atOrPastTrigger = isAtOrPastCompletionTrigger();

  if (!atOrPastTrigger) {
    completionTriggerSeenBelow = true;
  }

  if (completionTriggerSeenBelow && !lastAtOrPastCompletionTrigger && atOrPastTrigger) {
    completionTriggerCrossed = true;
  }

  lastAtOrPastCompletionTrigger = atOrPastTrigger;
}

void EpubReaderActivity::onEnter() {
  Activity::onEnter();
  pageLoadRetryCount = 0;
  coalescedPageDelta.store(0, std::memory_order_relaxed);
  coalescedSpineDelta.store(0, std::memory_order_relaxed);
  navigationSettleUntilMs.store(0, std::memory_order_relaxed);

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Activate reader-specific front button mapping (if configured).
  mappedInput.setReaderMode(true);

  epub->setupCacheDir();
  lastAutoPageTurnIntervalSeconds = loadAutoPageTurnIntervalSeconds(epub->getCachePath());
  BOOKMARKS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub");
  const bool fb2BackedBook = epub->getPath().find("/fb2_") != std::string::npos ||
                             epub->getPath().find("\\fb2_") != std::string::npos;
  clippings.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), fb2BackedBook ? "fb2" : "epub");

  if (APP_STATE.pendingBookmarkSpine != UINT16_MAX && APP_STATE.pendingBookmarkProgress >= 0.0f) {
    // Resume from a bookmark selected on the Home screen
    currentSpineIndex = APP_STATE.pendingBookmarkSpine;
    pendingSpineProgress = APP_STATE.pendingBookmarkProgress;
    pendingBookmarkParagraphIndex = APP_STATE.pendingBookmarkParagraphIndex;
    pendingPercentJump = true;
    cachedSpineIndex = currentSpineIndex;

    // Clear the pending jump
    APP_STATE.pendingBookmarkSpine = UINT16_MAX;
    APP_STATE.pendingBookmarkProgress = -1.0f;
    APP_STATE.pendingBookmarkParagraphIndex = UINT16_MAX;
    APP_STATE.saveToFile();
  } else {
    EpubReaderUtils::Progress progress;
    if (EpubReaderUtils::loadProgress(*epub, progress)) {
      currentSpineIndex = progress.spineIndex;
      nextPageNumber = progress.pageNumber;
      cachedSpineIndex = currentSpineIndex;
      if (progress.hasPageCount) {
        cachedChapterTotalPageCount = progress.pageCount;
      }
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0 && !pendingPercentJump) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Load reading stats and record session start time.
  // Session count and reading time are committed on exit once thresholds are met.
  stats = BookReadingStats::load(epub->getCachePath());
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const uint32_t cumulativeAvgSeconds =
      stats.totalPagesTurned > 0 ? stats.totalReadingSeconds / stats.totalPagesTurned : 0;
  LOG_DBG("ERS",
          "Reading stats loaded: totalReadingSeconds=%lu totalPagesTurned=%lu avg=%u samples=%u cumulativeAvg=%lu",
          static_cast<unsigned long>(stats.totalReadingSeconds), static_cast<unsigned long>(stats.totalPagesTurned),
          stats.avgSecondsPerForwardPage, stats.paceSampleCount, static_cast<unsigned long>(cumulativeAvgSeconds));
#endif
  armReadingPaceWarmup("reader_open");
  sessionReadingSeconds = 0;
  hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);

  globalStats = GlobalReadingStats::load();

  initializeCompletionPromptTrigger();

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addOrUpdateBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
  SleepCoverAssets::prepareEpub(*epub);

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  readerWork.cancel();
  coalescedPageDelta.store(0, std::memory_order_relaxed);
  coalescedSpineDelta.store(0, std::memory_order_relaxed);
  navigationSettleUntilMs.store(0, std::memory_order_relaxed);

  // Deactivate reader-specific front button mapping.
  mappedInput.setReaderMode(false);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  if (SETTINGS.shouldTrackReadingStats()) {
    recordCurrentPageReadingTime("reader_exit");

    // Commit session stats based on active reading time. Page intervals longer
    // than the idle threshold are rejected before they reach sessionReadingSeconds.
    // Sessions under 1 minute don't count toward session count or reading time.
    // Sessions under 10 seconds don't add to reading time.
    const uint32_t elapsedSecs = sessionReadingSeconds;
    if (elapsedSecs >= 60) {
      stats.sessionCount++;
      globalStats.totalSessions++;
    }
    if (elapsedSecs >= 10) {
      stats.totalReadingSeconds += elapsedSecs;
      globalStats.totalReadingSeconds += elapsedSecs;
      if (hasSessionStartLocalDateTime) {
        stats.recordReadingSpan(sessionStartLocalDateTime, elapsedSecs);
        globalStats.recordReadingSpan(sessionStartLocalDateTime, elapsedSecs);
      }
      if (elapsedSecs >= 120 && !stats.startDateManual && !stats.startDate.isValid() && hasSessionStartLocalDateTime) {
        stats.startDate = sessionStartLocalDateTime.date;
      }
    }
    if (epub) {
      recoverStoredPaceFromSession("reader_exit");
      stats.save(epub->getCachePath());
    }
    globalStats.save();
  }

  BOOKMARKS.unload();
  section.reset();

  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string title = epub->getTitle();
    const std::string author = epub->getAuthor();
    const std::string dstPath = BookMoveUtils::buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath, title, author);
  } else {
    epub.reset();
  }

  // Image-heavy FB2/EPUB parsing can leave the heap fragmented even after the
  // section is gone.  The SD font is restored by ReaderActivity before the
  // next book opens; releasing it here restores one large contiguous block
  // for the next decoder instead of carrying a stale glyph cache between books.
  sdFontSystem.releaseLoadedFont(renderer);
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  if (coalesceNavigationWhileLoading()) {
    return;
  }
  applyCoalescedNavigationIfReady();

  if (completionPromptQueued) {
    completionPromptQueued = false;
    completionPromptShown = true;
    pauseReadingPaceTimer("completion_prompt");
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_MARK_FINISHED_PROMPT_TITLE),
                                               tr(STR_MARK_FINISHED_PROMPT_BODY)),
        [this](const ActivityResult& result) {
          resumeReadingPaceTimer("completion_prompt_return");
          if (!result.isCancelled) {
            setBookCompleted(true);
            showCompletedFeedback(true);
          }
          requestUpdate();
        });
    return;
  }

  if (pendingBookmarkFeedback) {
    const bool timedOut = (millis() - bookmarkFeedbackShowTime) >= 1000UL;
    const bool navPressed = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (timedOut || navPressed) {
      pendingBookmarkFeedback = false;
      requestUpdate();
      return;
    }
  }

  if (pendingCompletedFeedback) {
    const bool timedOut = (millis() - completedFeedbackShowTime) >= 1000UL;
    const bool navPressed = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (timedOut || navPressed) {
      pendingCompletedFeedback = false;
      requestUpdate();
      return;
    }
  }
  if (pendingTiltPageTurnFeedback) {
    const bool timedOut = (millis() - tiltPageTurnFeedbackShowTime) >= 1000UL;
    const bool navPressed = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (timedOut || navPressed) {
      pendingTiltPageTurnFeedback = false;
      requestUpdate();
      return;
    }
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Reaching the unambiguous End-of-Book screen must count even when the
  // optional 99% confirmation prompt was not crossed (for example after
  // opening near the end or in a book with trailing split spine chunks).
  // setBookCompleted() is idempotent, persists the per-book marker and bumps
  // the global counter exactly once.
  if (atEndOfBook && !stats.isCompleted) {
    setBookCompleted(true);
  }

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addOrUpdateBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so any exit path relocates the book into /Read/.
  // setBookCompleted() also arms this when the user marks a book finished before
  // the End-of-Book screen.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else if (!stats.isCompleted) {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true, "auto");
      return;
    }
  }

  // Long-press Confirm: execute the configured reader action without opening the menu
  if (readerMenuRequested || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    readerMenuRequested = false;
    if (longPressMenuHandled) {
      longPressMenuHandled = false;
      return;
    }
    if (SETTINGS.longPressMenuAction != InkMODSettings::LONG_MENU_OFF &&
        mappedInput.getHeldTime() >= longPressMenuMs) {
      executeLongPressMenuAction();
      return;
    }
  }
  if (SETTINGS.longPressMenuAction != InkMODSettings::LONG_MENU_OFF && !longPressMenuHandled &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= longPressMenuMs) {
    longPressMenuHandled = true;
    executeLongPressMenuAction();
    return;
  }

  // Hidden reader-only sequence: Menu -> Back -> Menu -> Back -> Menu.
  // The first two Menu presses still open the normal reader menu; Back closes it.
  // On the fifth key, replace the menu with the Easter-egg prompt.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
      LegacyRenderDiagnostics::feed(LegacyRenderDiagnostics::Key::Menu)) {
    pauseReadingPaceTimer("legacy_render_prompt");
    startActivityForResult(std::make_unique<LegacyRenderPromptActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             resumeReadingPaceTimer("legacy_render_return");
                             if (!result.isCancelled) {
                               startActivityForResult(std::make_unique<LegacyGridDiagnosticsActivity>(renderer, mappedInput),
                                                      [this](const ActivityResult&) {
                                                        static constexpr const char* exitPhrases[] = {
                                                            "Возвращаемся к литературе.",
                                                            "Отдохнул? Теперь обратно к книге.",
                                                            "Перерыв окончен. Буквы ждут."};
                                                        GUI.drawPopup(renderer, exitPhrases[millis() % 3]);
                                                        renderer.displayBuffer();
                                                        delay(800);
                                                        requestUpdate();
                                                      });
                             } else {
                               static constexpr const char* noPhrases[] = {
                                   "Ну тогда читай дальше, спутник.",
                                   "Раз не устал — возвращайся к буквам.",
                                   "Ладно. Мир подождёт, книга — нет."};
                               GUI.drawPopup(renderer, noPhrases[millis() % 3]);
                               renderer.displayBuffer();
                               delay(900);
                               requestUpdate();
                             }
                           });
    return;
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    int currentPage = 0;
    int totalPages = 0;
    float bookProgress = 0.0f;
    uint16_t bmSpine = static_cast<uint16_t>(currentSpineIndex);
    float bmProgress = 0.0f;
    int bookmarkPageCount = 1;
    bool isBookCompleted = stats.isCompleted;
    {
      // Serialize EPUB metadata/file access with the render task.
      RenderLock lock(*this);
      currentPage = section ? section->currentPage + 1 : 0;
      totalPages = section ? section->pageCount : 0;
      bmSpine = static_cast<uint16_t>(currentSpineIndex);
      bmProgress =
          (section && section->pageCount > 0) ? static_cast<float>(section->currentPage) / section->pageCount : 0.0f;
      bookmarkPageCount = (section && section->pageCount > 0) ? section->pageCount : 1;
      isBookCompleted = stats.isCompleted;
      bookProgress = getCurrentBookProgressPercent();
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));

    pauseReadingPaceTimer("reader_menu");
    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                               SETTINGS.orientation, !currentPageFootnotes.empty(), !BOOKMARKS.getBookmarks().empty(),
                               !clippings.empty(),
                               BOOKMARKS.hasBookmarkForPage(bmSpine, bmProgress, bookmarkPageCount), isBookCompleted,
                               automaticPageTurnActive, getAutoPageTurnIntervalSeconds(),
                               SETTINGS.statusBarTimeLeft != InkMODSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_HIDE),
                           [this](const ActivityResult& result) {
                             // Always apply orientation change even if the menu was cancelled
                             const auto& menu = std::get<MenuResult>(result.data);
                             applyOrientation(menu.orientation);
                             if (menu.settingsChanged) {
                               sdFontSystem.ensureLoaded(renderer);
                               RenderLock lock(*this);
                               if (section) {
                                 cachedSpineIndex = currentSpineIndex;
                                 cachedChapterTotalPageCount = section->pageCount;
                                 nextPageNumber = section->currentPage;
                               }
                               section.reset();  // Force re-layout with changed reader settings
                             }
                             resumeReadingPaceTimer("reader_menu_return");
                             if (!result.isCancelled) {
                               returnToReaderMenuOnBack = true;
                               onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                             }
                           });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && longPressBackHandled) {
    longPressBackHandled = false;
    return;
  }

  if (!longPressBackHandled && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    longPressBackHandled = true;
    executeReaderQuickAction(static_cast<InkMODSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressBackAction));
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  // Side button long-press actions use raw Up/Down so the direction stays
  // physical regardless of the Prev/Next side layout setting.
  const bool sideLongPressChangesFont =
      SETTINGS.sideButtonLongPress == InkMODSettings::SIDE_LONG_PRESS::SIDE_LONG_FONT_SIZE;
  const bool sideLongPressChangesOrientation =
      SETTINGS.sideButtonLongPress == InkMODSettings::SIDE_LONG_PRESS::SIDE_LONG_ORIENTATION_CHANGE;
  if (sideLongPressChangesFont || sideLongPressChangesOrientation) {
    const bool topReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool bottomReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (sideButtonLongPressHandled && (topReleased || bottomReleased)) {
      sideButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool topLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Up) || topReleased);
    const bool bottomLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Down) || bottomReleased);

    if (!sideButtonLongPressHandled && topLongPressed) {
      sideButtonLongPressHandled = !topReleased;
      if (sideLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/true)) {
          reindexCurrentSection();
        }
      } else {
        applyOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/false));
        requestUpdate();
      }
      return;
    }
    if (!sideButtonLongPressHandled && bottomLongPressed) {
      sideButtonLongPressHandled = !bottomReleased;
      if (sideLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/false)) {
          reindexCurrentSection();
        }
      } else {
        applyOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/true));
        requestUpdate();
      }
      return;
    }
  }

  if (consumeLongPowerButtonRelease()) {
    return;
  }
  if (executeShortPowerButtonAction()) {
    return;
  }
  if (executeLongPowerButtonAction()) {
    return;
  }

  const bool frontLongPressChangesFont = SETTINGS.longPressButtonBehavior == InkMODSettings::FONT_SIZE_CHANGE;
  const bool frontLongPressAction = SETTINGS.longPressButtonBehavior == InkMODSettings::CHAPTER_SKIP ||
                                    SETTINGS.longPressButtonBehavior == InkMODSettings::ORIENTATION_CHANGE ||
                                    frontLongPressChangesFont;
  if (frontLongPressAction) {
    const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (frontButtonLongPressHandled && (leftReleased || rightReleased)) {
      frontButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool prevLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool nextLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!frontButtonLongPressHandled && (prevLongPressed || nextLongPressed)) {
      frontButtonLongPressHandled = true;
      if (SETTINGS.longPressButtonBehavior == InkMODSettings::CHAPTER_SKIP) {
        if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
          if (nextLongPressed) {
            onGoHome();
          } else {
            currentSpineIndex = epub->getSpineItemsCount() - 1;
            nextPageNumber = 0;
            pendingPageJump = std::numeric_limits<uint16_t>::max();
            requestUpdate();
          }
          return;
        }

        {
          RenderLock lock(*this);
          nextPageNumber = 0;
          currentSpineIndex = nextLongPressed ? currentSpineIndex + 1 : currentSpineIndex - 1;
          section.reset();
        }
        requestUpdate();
        return;
      }

      if (frontLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/nextLongPressed)) {
          reindexCurrentSection();
        }
        return;
      }

      const uint8_t newOrientation = nextLongPressed
                                         ? ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/false)
                                         : ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/true);
      applyOrientation(newOrientation);
      requestUpdate();
      return;
    }
  }

  auto [prevTriggered, nextTriggered, fromSideBtn, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  const bool powerReleased = mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool shortPowerTurn = SETTINGS.shortPwrBtn == InkMODSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                              mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration();
  const bool releasedLongPowerTurn = SETTINGS.longPwrBtn == InkMODSettings::SHORT_PWRBTN::PAGE_TURN &&
                                     powerReleased &&
                                     mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  bool heldLongPowerTurn = false;
  if (SETTINGS.longPwrBtn == InkMODSettings::SHORT_PWRBTN::PAGE_TURN && consumeLongPowerButtonHold()) {
    nextTriggered = true;
    fromSideBtn = false;
    fromTilt = false;
    heldLongPowerTurn = true;
  }
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
  const bool skipChapter =
      longPress &&
      (fromSideBtn ? SETTINGS.sideButtonLongPress == InkMODSettings::SIDE_LONG_PRESS::SIDE_LONG_CHAPTER_SKIP
                   : SETTINGS.longPressButtonBehavior == InkMODSettings::CHAPTER_SKIP);

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (skipChapter) {
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && !fromSideBtn && SETTINGS.longPressButtonBehavior == InkMODSettings::ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  const char* pageTurnSource = fromTilt ? "tilt" : (fromSideBtn ? "side" : "front");
  if (shortPowerTurn || releasedLongPowerTurn || heldLongPowerTurn) {
    pageTurnSource = "power";
  }
  if (prevTriggered) {
    pageTurn(false, pageTurnSource);
  } else {
    pageTurn(true, pageTurnSource);
  }
}

bool EpubReaderActivity::coalesceNavigationWhileLoading() {
  const bool cancellableWorkActive = readerWork.active();
  if (!cancellableWorkActive && !RenderLock::peek()) return false;

  // Menu/back actions need the main loop to continue. Invalidating the token
  // makes the render task release RenderLock at its next 1 KiB parser/ZIP
  // boundary, after which the existing action proceeds normally.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (cancellableWorkActive) readerWork.cancel();
    return false;
  }

  auto [prevTriggered, nextTriggered, fromSideBtn, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) return false;

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
  const bool skipChapter =
      longPress &&
      (fromSideBtn ? SETTINGS.sideButtonLongPress == InkMODSettings::SIDE_LONG_PRESS::SIDE_LONG_CHAPTER_SKIP
                   : SETTINGS.longPressButtonBehavior == InkMODSettings::CHAPTER_SKIP);
  const int32_t delta = nextTriggered ? 1 : -1;
  if (skipChapter) {
    coalescedSpineDelta.fetch_add(delta, std::memory_order_relaxed);
    coalescedPageDelta.store(0, std::memory_order_relaxed);
    readerWork.cancel();
  } else {
    coalescedPageDelta.fetch_add(delta, std::memory_order_relaxed);
  }
  navigationSettleUntilMs.store(millis() + NAVIGATION_COALESCE_MS, std::memory_order_relaxed);
  return true;
}

void EpubReaderActivity::applyCoalescedNavigationIfReady() {
  if (readerWork.active() || RenderLock::peek()) return;
  const uint32_t settleUntil = navigationSettleUntilMs.load(std::memory_order_relaxed);
  if (settleUntil == 0 || static_cast<int32_t>(millis() - settleUntil) < 0) return;

  int32_t spineDelta = 0;
  int32_t pageDelta = 0;
  {
    RenderLock lock(*this);
    // The render task consumes the same queue before it displays a freshly
    // built section. Taking the queue only after the lock prevents a race
    // that could otherwise flash an intermediate page for one refresh.
    spineDelta = coalescedSpineDelta.exchange(0, std::memory_order_relaxed);
    pageDelta = coalescedPageDelta.exchange(0, std::memory_order_relaxed);
    navigationSettleUntilMs.store(0, std::memory_order_relaxed);
    if (spineDelta == 0 && pageDelta == 0) return;
    if (spineDelta != 0) {
      const int spineCount = epub ? epub->getSpineItemsCount() : 0;
      const int targetSpine = std::max(0, std::min(spineCount, currentSpineIndex + static_cast<int>(spineDelta)));
      if (targetSpine != currentSpineIndex) {
        clearLogicalPageCarry();
        currentSpineIndex = targetSpine;
        nextPageNumber = spineDelta < 0 ? std::numeric_limits<uint16_t>::max() : 0;
        section.reset();
      }
    } else {
      if (section && section->pageCount > 0) {
        const int64_t targetPage = static_cast<int64_t>(section->currentPage) + pageDelta;
        if (targetPage >= section->pageCount && currentSpineIndex + 1 < epub->getSpineItemsCount()) {
          coalescedPageDelta.store(
              static_cast<int32_t>(std::min<int64_t>(targetPage - section->pageCount, INT32_MAX)),
              std::memory_order_relaxed);
          rememberLogicalForwardCarry();
          currentSpineIndex++;
          nextPageNumber = 0;
          section.reset();
        } else if (targetPage >= section->pageCount && currentSpineIndex + 1 == epub->getSpineItemsCount()) {
          clearLogicalPageCarry();
          currentSpineIndex = epub->getSpineItemsCount();
          nextPageNumber = 0;
          section.reset();
        } else if (targetPage < 0 && currentSpineIndex > 0) {
          clearLogicalPageCarry();
          coalescedPageDelta.store(static_cast<int32_t>(std::max<int64_t>(targetPage + 1, INT32_MIN)),
                                   std::memory_order_relaxed);
          currentSpineIndex--;
          nextPageNumber = 0;
          pendingPageJump = std::numeric_limits<uint16_t>::max();
          section.reset();
        } else {
          section->currentPage = static_cast<int>(std::max<int64_t>(
              0, std::min<int64_t>(targetPage, static_cast<int64_t>(section->pageCount) - 1)));
        }
      } else {
        // The target section's page count is unknown. Preserve the relative
        // delta until render() has loaded/built that section; clamping an
        // absolute number here would lose the remainder at chapter borders.
        coalescedPageDelta.store(pageDelta, std::memory_order_relaxed);
      }
    }
  }
  logicalStatusCacheSpineIndex = -1;
  logicalStatusCacheLocalPageCount = -1;
  requestUpdate();
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  pageLoadRetryCount = 0;
  if (!epub) {
    return;
  }

  // BookMetadataCache uses a shared seek-based FsFile for spine metadata lookups.
  // Hold the render/file mutex for the full jump calculation so menu-driven jumps
  // cannot race render/status-bar reads of the same cache file.
  RenderLock lock(*this);

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  currentSpineIndex = targetSpineIndex;
  clearLogicalPageCarry();
  nextPageNumber = 0;
  pendingPercentJump = true;
  section.reset();
  armReadingPaceWarmup("percent_jump");
}

void EpubReaderActivity::returnToReaderMenu() {
  if (!returnToReaderMenuOnBack) return;
  returnToReaderMenuOnBack = false;
  readerMenuRequested = true;
  requestUpdate();
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      pauseReadingPaceTimer("chapter_selection");
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              returnToReaderMenuOnBack = false;
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              currentSpineIndex = chapterResult.spineIndex;
              clearLogicalPageCarry();

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
              armReadingPaceWarmup("chapter_jump");
              pauseReadingPaceTimer("chapter_jump");
            } else {
              resumeReadingPaceTimer("chapter_selection_cancel");
              returnToReaderMenu();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      pauseReadingPaceTimer("footnotes");
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 returnToReaderMenuOnBack = false;
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               } else {
                                 resumeReadingPaceTimer("footnotes_cancel");
                                 returnToReaderMenu();
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      if (!section || section->currentPage < 0 || section->currentPage >= section->pageCount) {
        requestUpdate();
        break;
      }

      std::unique_ptr<Page> page;
      {
        RenderLock lock(*this);
        page = section->loadPageFromSectionFile();
      }
      if (!page) {
        LOG_ERR("ERS", "Failed to load current page for dictionary lookup");
        requestUpdate();
        break;
      }

      const ReaderViewportLayout layout = computeReaderViewportLayout(renderer, automaticPageTurnActive);
      auto dictionaryActivity = makeUniqueNoThrow<DictionaryActivity>(
          renderer, mappedInput, std::move(page), activeSectionFontId, layout.marginTop, layout.marginRight,
          layout.marginBottom, layout.marginLeft);
      if (!dictionaryActivity) {
        LOG_ERR("ERS", "Failed to allocate dictionary activity");
        requestUpdate();
        break;
      }

      pauseReadingPaceTimer("dictionary");
      startActivityForResult(std::move(dictionaryActivity), [this](const ActivityResult&) {
        // A long-menu/back/power quick action is handled by the parent reader,
        // but its RELEASE happens while Dictionary owns input. Reset all
        // one-shot/long-press guards here so the first genuine Menu press after
        // returning is not swallowed as the release of the old quick action.
        longPressMenuHandled = false;
        longPressBackHandled = false;
        frontButtonLongPressHandled = false;
        sideButtonLongPressHandled = false;
        longPowerButtonHandled = false;

        resumeReadingPaceTimer("dictionary_return");
        returnToReaderMenu();
        requestUpdate();
      });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      {
        // Serialize EPUB metadata/file access with the render task.
        RenderLock lock(*this);
        bookProgress = getCurrentBookProgressPercent();
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      pauseReadingPaceTimer("percent_selection");
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              returnToReaderMenuOnBack = false;
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            } else {
              resumeReadingPaceTimer("percent_selection_cancel");
              returnToReaderMenu();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
                }
              }
            }
          }
          if (!fullText.empty()) {
            pauseReadingPaceTimer("qr_display");
            startActivityForResult(
                std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                [this](const ActivityResult&) {
                  resumeReadingPaceTimer("qr_display_return");
                  returnToReaderMenu();
                });
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_STATS: {
      pauseReadingPaceTimer("delete_stats_confirm");
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                                    confirmationHeading(StrId::STR_DELETE_BOOK_STATS),
                                                                    epub ? epub->getTitle() : std::string{}),
                             [this](const ActivityResult& result) {
                               bool statsDeleted = false;
                               if (!result.isCancelled) {
                                 {
                                   RenderLock lock(*this);
                                   if (epub) {
                                     statsDeleted = BookReadingStats::remove(epub->getCachePath());
                                     if (statsDeleted) {
                                       resetCurrentBookStatsAfterDelete();
                                     }
                                   }
                                 }
                                 if (statsDeleted) {
                                   drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
                                   delay(1000);
                                 } else {
                                   LOG_ERR("ERS", "Failed to delete book stats");
                                 }
                               }
                               resumeReadingPaceTimer("delete_stats_return");
                               if (result.isCancelled) returnToReaderMenu();
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      pauseReadingPaceTimer("delete_cache_confirm");
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, confirmationHeading(StrId::STR_DELETE_CACHE),
                                                 epub ? epub->getTitle() : std::string{}),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              resumeReadingPaceTimer("delete_cache_cancel");
              returnToReaderMenu();
              requestUpdate();
              return;
            }

            bool cacheDeleted = false;
            {
              RenderLock lock(*this);
              if (epub && section) {
                uint16_t backupSpine = currentSpineIndex;
                uint16_t backupPage = section->currentPage;
                uint16_t backupPageCount = section->pageCount;
                if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
                  LOG_ERR("ERS", "Failed to save progress before cache clear");
                }
                stats.save(epub->getCachePath());
                section.reset();
                cacheDeleted = clearBookCachePreservingUserState(epub->getPath());
                epub->setupCacheDir();
                if (cacheDeleted) {
                  drawToast(renderer, tr(STR_BOOK_CACHE_DELETED));
                }
              }
            }
            if (cacheDeleted) {
              delay(1000);
            }
            onGoHome();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::RESET_READING_PACE: {
      returnToReaderMenuOnBack = false;
      resetReadingPaceData();
      drawToast(renderer, tr(STR_READING_PACE_RESET));
      delay(1000);
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      returnToReaderMenuOnBack = false;
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      // Include elapsed time from the current session in the display stats.
      BookReadingStats displayStats = stats;
      if (SETTINGS.shouldTrackReadingStats()) {
        uint32_t currentPageSeconds = 0;
        displayStats.totalReadingSeconds = displayStats.totalReadingSeconds > UINT32_MAX - sessionReadingSeconds
                                               ? UINT32_MAX
                                               : displayStats.totalReadingSeconds + sessionReadingSeconds;
        if (currentPageReadingSecondsForStats(currentPageSeconds, "book_stats_preview")) {
          displayStats.totalReadingSeconds = displayStats.totalReadingSeconds > UINT32_MAX - currentPageSeconds
                                                 ? UINT32_MAX
                                                 : displayStats.totalReadingSeconds + currentPageSeconds;
        }
      }
      uint32_t estimatedTimeLeftSeconds = 0;
      const bool hasEstimatedTimeLeft = estimateTimeLeftSeconds(true, estimatedTimeLeftSeconds);
      const bool hasSyncedStats = GlobalReadingStats::hasSyncedStats();
      const GlobalReadingStats displayAllDevicesStats =
          hasSyncedStats ? GlobalReadingStats::loadAggregated(globalStats) : GlobalReadingStats{};
      pauseReadingPaceTimer("book_stats");
      if (hasSyncedStats) {
        startActivityForResult(
            std::make_unique<BookStatsActivity>(renderer, mappedInput, epub->getTitle(), epub->getCachePath(),
                                                displayStats, getCurrentBookProgressPercent(), hasEstimatedTimeLeft,
                                                estimatedTimeLeftSeconds, globalStats, displayAllDevicesStats),
            [this](const ActivityResult&) {
              if (epub) {
                stats = BookReadingStats::load(epub->getCachePath());
              }
              globalStats = GlobalReadingStats::load();
              completionPromptShown = stats.isCompleted;
              if (stats.isCompleted && SETTINGS.moveFinishedToReadFolder && epub && !isInReadFolder(epub->getPath())) {
                pendingReadFolderMove = true;
              } else if (!stats.isCompleted) {
                pendingReadFolderMove = false;
              }
              resumeReadingPaceTimer("book_stats_return");
              returnToReaderMenu();
              requestUpdate();
            });
      } else {
        startActivityForResult(
            std::make_unique<BookStatsActivity>(renderer, mappedInput, epub->getTitle(), epub->getCachePath(),
                                                displayStats, getCurrentBookProgressPercent(), hasEstimatedTimeLeft,
                                                estimatedTimeLeftSeconds, globalStats),
            [this](const ActivityResult&) {
              if (epub) {
                stats = BookReadingStats::load(epub->getCachePath());
              }
              globalStats = GlobalReadingStats::load();
              completionPromptShown = stats.isCompleted;
              if (stats.isCompleted && SETTINGS.moveFinishedToReadFolder && epub && !isInReadFolder(epub->getPath())) {
                pendingReadFolderMove = true;
              } else if (!stats.isCompleted) {
                pendingReadFolderMove = false;
              }
              resumeReadingPaceTimer("book_stats_return");
              returnToReaderMenu();
              requestUpdate();
            });
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_COMPLETED: {
      returnToReaderMenuOnBack = false;
      const bool markCompleted = !stats.isCompleted;
      setBookCompleted(markCompleted);
      showCompletedFeedback(markCompleted);
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : nextPageNumber;
        const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
        std::optional<uint16_t> paragraphIndex;
        if (section && currentPage >= 0 && currentPage < section->pageCount) {
          const uint16_t paragraphPage =
              currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
          if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
            paragraphIndex = *pIdx;
          }
        }

        // Pre-compute local KO position and chapter name while Epub is still in RAM.
        InkMODPosition localPos = {currentSpineIndex, currentPage, totalPages};
        if (paragraphIndex.has_value()) {
          localPos.paragraphIndex = *paragraphIndex;
          localPos.hasParagraphIndex = true;
        }
        KOReaderPosition localKoPos = ProgressMapper::toKOReader(epub, localPos);
        const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
        std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
        const std::string savedEpubPath = epub->getPath();

        // Persist current position so the reader resumes at the right page on return.
        // goToReader() depends on this file, so abort the sync if the write fails.
        if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
          LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
          pendingSyncSaveError = true;
          requestUpdate();
          return;
        }

        // Release the heavy Section now. Keep Epub alive until onExit(), which still
        // needs it for stats/cache cleanup before the sync activity starts.
        LOG_DBG("KOSync", "Releasing section for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
        {
          RenderLock lock(*this);
          if (section) {
            nextPageNumber = section->currentPage;
          }
          section.reset();
        }
        LOG_DBG("KOSync", "Section released for sync (heap after: %u)", (unsigned)ESP.getFreeHeap());

        pauseReadingPaceTimer("sync_progress");
        activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
            renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
            std::move(localChapterName), paragraphIndex));
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE: {
      returnToReaderMenuOnBack = false;
      if (!section || section->pageCount == 0) break;
      const uint16_t spine = static_cast<uint16_t>(currentSpineIndex);
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);

      if (BOOKMARKS.hasBookmarkForPage(spine, progress, section->pageCount)) {
        BOOKMARKS.removeBookmarkForPage(spine, progress, section->pageCount);
        bookmarkFeedbackType = BookmarkFeedbackType::Removed;
      } else {
        const char* chapterTitle = nullptr;
        std::string titleStr;
        const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
        if (tocIndex != -1) {
          titleStr = epub->getTocItem(tocIndex).title;
          chapterTitle = titleStr.c_str();
        }
        uint16_t paragraphIndex = UINT16_MAX;
        if (const auto pIdx = section->getParagraphIndexForPage(static_cast<uint16_t>(section->currentPage))) {
          paragraphIndex = *pIdx;
        }
        char snippet[BOOKMARK_SNIPPET_MAX] = {};
        if (auto page = section->loadPageFromSectionFile()) {
          buildBookmarkSnippet(*page, snippet, sizeof(snippet));
        }
        const auto addResult =
            BOOKMARKS.addBookmark(spine, progress, section->pageCount, chapterTitle, paragraphIndex, snippet);
        bookmarkFeedbackType = (addResult == BookmarkStore::AddResult::Added) ? BookmarkFeedbackType::Added
                                                                              : BookmarkFeedbackType::LimitReached;
      }
      pendingBookmarkFeedback = true;
      bookmarkFeedbackShowTime = millis();
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_BOOKMARKS: {
      pauseReadingPaceTimer("bookmark_list");
      startActivityForResult(
          std::make_unique<EpubReaderBookmarkListActivity>(renderer, mappedInput, BOOKMARKS.getBookmarks()),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              returnToReaderMenuOnBack = false;
              const auto& bm = std::get<BookmarkResult>(result.data);
              RenderLock lock(*this);
              currentSpineIndex = bm.spineIndex;
              pendingSpineProgress = bm.progress;
              pendingBookmarkParagraphIndex = bm.paragraphIndex;
              pendingPercentJump = true;
              section.reset();
              armReadingPaceWarmup("bookmark_jump");
              pauseReadingPaceTimer("bookmark_jump");
            } else {
              resumeReadingPaceTimer("bookmark_list_cancel");
              returnToReaderMenu();
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_BOOKMARKS: {
      pauseReadingPaceTimer("delete_bookmarks_confirm");
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                 confirmationHeading(StrId::STR_DELETE_BOOKMARKS),
                                                 epub ? epub->getTitle() : std::string{}),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              BOOKMARKS.clearAll();
            }
            resumeReadingPaceTimer(result.isCancelled ? "delete_bookmarks_cancel" : "delete_bookmarks_return");
            if (result.isCancelled) returnToReaderMenu();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::CREATE_CLIPPING: {
      if (!section || section->pageCount <= 0) break;
      const int renderFontId = activeSectionFontId != 0 ? activeSectionFontId : SETTINGS.getReaderFontId();
      const int selectedSpine = currentSpineIndex;
      const int selectedPage = section->currentPage;
      const int selectedPageCount = section->pageCount;
      const auto layout = computeReaderViewportLayout(renderer, automaticPageTurnActive);
      std::string chapterTitle;
      const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
      if (tocIndex >= 0) chapterTitle = epub->getTocItem(tocIndex).title;

      pauseReadingPaceTimer("clipping_select");
      startActivityForResult(
          std::make_unique<ClippingSelectionActivity>(renderer, mappedInput, *section, renderFontId,
                                                      layout.marginLeft, layout.marginTop),
          [this, selectedSpine, selectedPage, selectedPageCount, chapterTitle](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto* selection = std::get_if<ClippingSelectionResult>(&result.data);
              if (selection) {
                Clipping clip;
                clip.spineIndex = static_cast<uint16_t>(selectedSpine);
                clip.pageNumber = selection->startPageNumber;
                clip.endPageNumber = selection->endPageNumber;
                clip.pageCount = static_cast<uint16_t>(std::max(1, selectedPageCount));
                clip.startWordIndex = selection->startWordIndex;
                clip.endWordIndex = selection->endWordIndex;
                clip.timestamp = millis() / 1000;
                std::snprintf(clip.chapterTitle, sizeof(clip.chapterTitle), "%s", chapterTitle.c_str());
                std::snprintf(clip.text, sizeof(clip.text), "%s", selection->text.c_str());
                const auto addResult = clippings.add(clip);
                const char* clippingMessage = tr(STR_CLIPPING_LIMIT);
                if (addResult == ClippingStore::AddResult::Added) {
                  clippingMessage = tr(STR_CLIPPING_SAVED);
                } else if (addResult == ClippingStore::AddResult::RemovedExisting) {
                  clippingMessage = tr(STR_CLIPPING_REMOVED);
                } else if (addResult == ClippingStore::AddResult::SaveFailed) {
                  clippingMessage = tr(STR_SAVE_FAILED);
                }
                GUI.drawPopup(renderer, clippingMessage);
                renderer.displayBuffer();
                delay(650);
              }
            }
            resumeReadingPaceTimer("clipping_select_return");
            if (result.isCancelled) returnToReaderMenu();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_CLIPPINGS: {
      pauseReadingPaceTimer("clipping_list");
      startActivityForResult(
          std::make_unique<ClippingListActivity>(renderer, mappedInput, clippings),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              returnToReaderMenuOnBack = false;
              if (const auto* jump = std::get_if<ClippingJumpResult>(&result.data)) {
                RenderLock lock(*this);
                currentSpineIndex = jump->spineIndex;
                nextPageNumber = jump->pageNumber;
                pendingPageJump = jump->pageNumber;
                section.reset();
                armReadingPaceWarmup("clipping_jump");
                pauseReadingPaceTimer("clipping_jump");
              }
            } else {
              resumeReadingPaceTimer("clipping_list_cancel");
              returnToReaderMenu();
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
      openAutoPageTurnIntervalPicker();
      break;
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
    case EpubReaderMenuActivity::MenuAction::READER_OPTIONS:
    case EpubReaderMenuActivity::MenuAction::CONTROLS_OPTIONS:
      break;
  }
}

void EpubReaderActivity::reindexCurrentSection() {
  SETTINGS.saveToFile();
  sdFontSystem.ensureLoaded(renderer);
  {
    RenderLock lock(*this);
    showChapterLoadingPopup();
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::showChapterLoadingPopup() {
  // Keep the reader's last visible page behind the notification. Image
  // precaching later reuses the framebuffer as a conversion surface, so this
  // helper must only be called before a section build starts; parser callbacks
  // deliberately use a no-op instead of redisplaying that working buffer.
  GUI.drawPopup(renderer, tr(STR_LOADING_CHAPTER));
}

void EpubReaderActivity::openFileTransfer() {
  pauseReadingPaceTimer("file_transfer");
  if (epub && section) {
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  }

  activityManager.goToFileTransfer(epub ? epub->getPath() : std::string{});
}

void EpubReaderActivity::openAutoPageTurnIntervalPicker(const bool ignoreInitialConfirmRelease) {
  pauseReadingPaceTimer("auto_turn_interval");
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "EpubReaderAutoPageTurnInterval", StrId::STR_AUTO_TURN_INTERVAL_SECONDS,
          StrId::STR_AUTO_TURN_STEP_HINT, getAutoPageTurnIntervalSeconds(), MIN_AUTO_PAGE_TURN_INTERVAL_S,
          MAX_AUTO_PAGE_TURN_INTERVAL_S, 1, 5, StrId::STR_NONE_OPT, /*readerActivity=*/true,
          /*allowPowerAsConfirm=*/true, ignoreInitialConfirmRelease),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          returnToReaderMenuOnBack = false;
          setAutoPageTurnIntervalSeconds(static_cast<uint16_t>(std::get<IntervalResult>(result.data).value));
        } else {
          resumeReadingPaceTimer("auto_turn_interval_cancel");
          returnToReaderMenu();
        }
        requestUpdate();
      });
}

void EpubReaderActivity::resetReadingPaceData() {
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const uint16_t oldAvg = stats.avgSecondsPerForwardPage;
  const uint16_t oldCount = stats.paceSampleCount;
#endif
  stats.avgSecondsPerForwardPage = 0;
  stats.paceSampleCount = 0;
  sessionPaceSampleSeconds = 0;
  sessionPaceSampleCount = 0;
  armReadingPaceWarmup("reading_pace_reset");
  if (epub) {
    epub->setupCacheDir();
    stats.save(epub->getCachePath());
  }
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  LOG_DBG("ERS", "Reading pace reset: avg=%u->%u samples=%u->%u totalReadingSeconds=%lu totalPagesTurned=%lu", oldAvg,
          stats.avgSecondsPerForwardPage, oldCount, stats.paceSampleCount,
          static_cast<unsigned long>(stats.totalReadingSeconds), static_cast<unsigned long>(stats.totalPagesTurned));
#endif
}

void EpubReaderActivity::resetCurrentBookStatsAfterDelete() {
  stats = BookReadingStats{};
  sessionReadingSeconds = 0;
  sessionPaceSampleSeconds = 0;
  sessionPaceSampleCount = 0;
  pendingReadFolderMove = false;
  hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);
  armReadingPaceWarmup("book_stats_delete");
  initializeCompletionPromptTrigger();
}

void EpubReaderActivity::executeReaderQuickAction(InkMODSettings::LONG_PRESS_MENU_ACTION action) {
  switch (action) {
    case InkMODSettings::LONG_MENU_SLEEP:
      enterDeepSleep();
      break;
    case InkMODSettings::LONG_MENU_CHANGE_FONT: {
      // This firmware has no built-in reader fonts - they're SD-card only (see the
      // FontSelectionActivity.cpp comment). SETTINGS.fontFamily is a legacy field kept
      // only so old settings files still parse; cycling it has no visible effect. Cycle
      // through the actually-installed SD font families instead, matching what
      // FontSelectionActivity offers.
      const auto& families = sdFontSystem.registry().getFamilies();
      if (!families.empty()) {
        const int currentIdx = SETTINGS.sdFontFamilyName[0] != '\0'
                                    ? sdFontSystem.registry().getFamilyIndex(SETTINGS.sdFontFamilyName)
                                    : -1;
        const int nextIdx = (currentIdx + 1 >= static_cast<int>(families.size())) ? 0 : currentIdx + 1;
        strncpy(SETTINGS.sdFontFamilyName, families[nextIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
        sdFontSystem.ensureLoaded(renderer);
        SETTINGS.saveToFile();
      } else {
        // No SD fonts installed at all - nothing real to cycle to.
        SETTINGS.fontFamily = (SETTINGS.fontFamily + 1) % InkMODSettings::FONT_FAMILY_COUNT;
      }
      reindexCurrentSection();
      break;
    }
    case InkMODSettings::LONG_MENU_TOGGLE_GUIDE_DOTS:
      SETTINGS.guideReadingEnabled = !SETTINGS.guideReadingEnabled;
      reindexCurrentSection();
      break;
    case InkMODSettings::LONG_MENU_TOGGLE_BIONIC:
      SETTINGS.bionicReadingEnabled = !SETTINGS.bionicReadingEnabled;
      reindexCurrentSection();
      break;
    case InkMODSettings::LONG_MENU_TOGGLE_BOOKMARK:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE);
      break;
    case InkMODSettings::LONG_MENU_REFRESH_SCREEN:
      // Clean ghosting immediately, then repaint through the normal reader path
      // so grayscale/anti-aliasing is applied again after the forced refresh.
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
      requestUpdate();
      break;
    case InkMODSettings::LONG_MENU_SYNC_PROGRESS:
      if (KOREADER_STORE.hasCredentials()) {
        onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::SYNC);
      } else {
        pauseReadingPaceTimer("koreader_settings");
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 resumeReadingPaceTimer("koreader_settings_return");
                                 SETTINGS.saveToFile();
                               });
      }
      break;
    case InkMODSettings::LONG_MENU_MARK_FINISHED: {
      const bool newCompleted = !stats.isCompleted;
      setBookCompleted(newCompleted);
      showCompletedFeedback(newCompleted);
    }
      requestUpdate();
      break;
    case InkMODSettings::LONG_MENU_READING_STATS:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::READING_STATS);
      break;
    case InkMODSettings::LONG_MENU_SCREENSHOT:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::SCREENSHOT);
      break;
    case InkMODSettings::LONG_MENU_CYCLE_PAGE_TURN:
      openAutoPageTurnIntervalPicker(/*ignoreInitialConfirmRelease=*/true);
      break;
    case InkMODSettings::LONG_MENU_FILE_TRANSFER:
      openFileTransfer();
      break;
    case InkMODSettings::LONG_MENU_CALIBRE_WIRELESS:
      activityManager.goToCalibreWireless(epub ? epub->getPath() : "");
      break;
    case InkMODSettings::LONG_MENU_JOIN_NETWORK:
      activityManager.goToJoinNetworkFileTransfer(epub ? epub->getPath() : "");
      break;
    case InkMODSettings::LONG_MENU_CREATE_HOTSPOT:
      activityManager.goToHotspotFileTransfer(epub ? epub->getPath() : "");
      break;
    case InkMODSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN:
      if (halTiltSensor.isAvailable()) {
        SETTINGS.tiltPageTurn = SETTINGS.tiltPageTurn == InkMODSettings::TILT_OFF ? InkMODSettings::TILT_ON
                                                                                      : InkMODSettings::TILT_OFF;
        SETTINGS.saveToFile();
        halTiltSensor.clearPendingEvents();
        showTiltPageTurnFeedback(SETTINGS.tiltPageTurn != InkMODSettings::TILT_OFF);
        requestUpdate();
      }
      break;
    case InkMODSettings::LONG_MENU_TOGGLE_DARK_MODE:
      SETTINGS.readerDarkMode = !SETTINGS.readerDarkMode;
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case InkMODSettings::LONG_MENU_FOOTNOTES:
      executeFootnoteQuickAction();
      break;
    case InkMODSettings::LONG_MENU_FILE_BROWSER:
      activityManager.goToFileBrowser(epub ? epub->getPath() : "");
      break;
    case InkMODSettings::LONG_MENU_DICTIONARY_LOOKUP:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::DICTIONARY);
      break;
    case InkMODSettings::LONG_MENU_OFF:
    default:
      break;
  }
}

void EpubReaderActivity::executeFootnoteQuickAction() {
  if (footnoteDepth > 0 && SETTINGS.pwrBtnFootnoteBack) {
    restoreSavedPosition();
    return;
  }

  if (currentPageFootnotes.size() == 1) {
    navigateToHref(currentPageFootnotes[0].href, true);
    return;
  }

  if (currentPageFootnotes.size() > 1) {
    pauseReadingPaceTimer("footnotes");
    startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                               navigateToHref(footnoteResult.href, true);
                             } else {
                               resumeReadingPaceTimer("footnotes_cancel");
                             }
                             requestUpdate();
                           });
  }
}

bool EpubReaderActivity::executeShortPowerButtonAction() {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }

  switch (SETTINGS.shortPwrBtn) {
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_FONT:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_CHANGE_FONT);
      return true;
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_GUIDE_DOTS:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_TOGGLE_GUIDE_DOTS);
      return true;
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_BIONIC_READING:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_TOGGLE_BIONIC);
      return true;
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_BOOKMARK:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_TOGGLE_BOOKMARK);
      return true;
    case InkMODSettings::SHORT_PWRBTN::SYNC_PROGRESS:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_SYNC_PROGRESS);
      return true;
    case InkMODSettings::SHORT_PWRBTN::MARK_FINISHED:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_MARK_FINISHED);
      return true;
    case InkMODSettings::SHORT_PWRBTN::READING_STATS:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_READING_STATS);
      return true;
    case InkMODSettings::SHORT_PWRBTN::SCREENSHOT:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_SCREENSHOT);
      return true;
    case InkMODSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_CYCLE_PAGE_TURN);
      return true;
    case InkMODSettings::SHORT_PWRBTN::FILE_TRANSFER:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_FILE_TRANSFER);
      return true;
    case InkMODSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_CALIBRE_WIRELESS);
      return true;
    case InkMODSettings::SHORT_PWRBTN::JOIN_NETWORK:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_JOIN_NETWORK);
      return true;
    case InkMODSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_CREATE_HOTSPOT);
      return true;
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN);
      return true;
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_TOGGLE_DARK_MODE);
      return true;
    case InkMODSettings::SHORT_PWRBTN::FOOTNOTES:
      executeFootnoteQuickAction();
      return true;
    case InkMODSettings::SHORT_PWRBTN::FILE_BROWSER:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_FILE_BROWSER);
      return true;
    case InkMODSettings::SHORT_PWRBTN::DICTIONARY_LOOKUP:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_DICTIONARY_LOOKUP);
      return true;
    default:
      return false;
  }
}

bool EpubReaderActivity::consumeLongPowerButtonRelease() {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Power) || !longPowerButtonHandled) {
    return false;
  }

  longPowerButtonHandled = false;
  return true;
}

bool EpubReaderActivity::consumeLongPowerButtonHold() {
  if (longPowerButtonHandled || !mappedInput.isPressed(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }

  longPowerButtonHandled = true;
  return true;
}

bool EpubReaderActivity::executeLongPowerButtonAction() {
  if (SETTINGS.longPwrBtn == InkMODSettings::SHORT_PWRBTN::PAGE_TURN || !consumeLongPowerButtonHold()) {
    return false;
  }

  switch (SETTINGS.longPwrBtn) {
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_FONT:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_CHANGE_FONT);
      return true;
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_GUIDE_DOTS:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_TOGGLE_GUIDE_DOTS);
      return true;
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_BIONIC_READING:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_TOGGLE_BIONIC);
      return true;
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_BOOKMARK:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_TOGGLE_BOOKMARK);
      return true;
    case InkMODSettings::SHORT_PWRBTN::SYNC_PROGRESS:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_SYNC_PROGRESS);
      return true;
    case InkMODSettings::SHORT_PWRBTN::MARK_FINISHED:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_MARK_FINISHED);
      return true;
    case InkMODSettings::SHORT_PWRBTN::READING_STATS:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_READING_STATS);
      return true;
    case InkMODSettings::SHORT_PWRBTN::SCREENSHOT:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_SCREENSHOT);
      return true;
    case InkMODSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_CYCLE_PAGE_TURN);
      return true;
    case InkMODSettings::SHORT_PWRBTN::FILE_TRANSFER:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_FILE_TRANSFER);
      return true;
    case InkMODSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_CALIBRE_WIRELESS);
      return true;
    case InkMODSettings::SHORT_PWRBTN::JOIN_NETWORK:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_JOIN_NETWORK);
      return true;
    case InkMODSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_CREATE_HOTSPOT);
      return true;
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN);
      return true;
    case InkMODSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_TOGGLE_DARK_MODE);
      return true;
    case InkMODSettings::SHORT_PWRBTN::FOOTNOTES:
      executeFootnoteQuickAction();
      return true;
    case InkMODSettings::SHORT_PWRBTN::FILE_BROWSER:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_FILE_BROWSER);
      return true;
    case InkMODSettings::SHORT_PWRBTN::DICTIONARY_LOOKUP:
      executeReaderQuickAction(InkMODSettings::LONG_MENU_DICTIONARY_LOOKUP);
      return true;
    default:
      return false;
  }
}

void EpubReaderActivity::executeLongPressMenuAction() {
  executeReaderQuickAction(static_cast<InkMODSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressMenuAction));
}

void EpubReaderActivity::setBookCompleted(bool isCompleted) {
  if (stats.isCompleted == isCompleted) {
    return;
  }

  stats.isCompleted = isCompleted;
  if (isCompleted && !stats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      stats.finishedDate = now.date;
    }
  }
  if (isCompleted) {
    completionPromptShown = true;
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.removeByPath(epub->getPath());
    }
    if (SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath())) {
      pendingReadFolderMove = true;
    }
  } else {
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.addOrUpdateBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
    }
    recentsEntryRemoved = false;
    pendingReadFolderMove = false;
  }
  if (isCompleted) {
    globalStats.completedBooks++;
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }

  stats.save(epub->getCachePath());
  globalStats.save();
}

void EpubReaderActivity::showCompletedFeedback(bool isCompleted) {
  completedFeedbackIsFinished = isCompleted;
  pendingCompletedFeedback = true;
  completedFeedbackShowTime = millis();
}

void EpubReaderActivity::showTiltPageTurnFeedback(bool enabled) {
  tiltPageTurnFeedbackEnabled = enabled;
  pendingTiltPageTurnFeedback = true;
  tiltPageTurnFeedbackShowTime = millis();
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  const auto targetOrientation = ReaderUtils::toRendererOrientation(orientation);
  const bool settingsChanged = SETTINGS.orientation != orientation;
  const bool rendererChanged = renderer.getOrientation() != targetOrientation;

  // No-op only when both the persisted setting and the live renderer already match.
  if (!settingsChanged && !rendererChanged) {
    return;
  }

  {
    RenderLock lock(*this);

    // Preserve current reading position only when we need a live re-layout.
    if (rendererChanged && section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    if (settingsChanged) {
      // Persist the selection so the reader keeps the new orientation on next launch.
      SETTINGS.orientation = orientation;
      SETTINGS.saveToFile();
    }

    if (rendererChanged) {
      // Update renderer orientation to match the new logical coordinate system.
      renderer.setOrientation(targetOrientation);

      // Reset section to force re-layout in the new orientation.
      section.reset();
    }
  }
}

uint16_t EpubReaderActivity::getAutoPageTurnIntervalSeconds() const {
  if (lastAutoPageTurnIntervalSeconds == 0) {
    return DEFAULT_AUTO_PAGE_TURN_INTERVAL_S;
  }
  return clampAutoPageTurnIntervalSeconds(lastAutoPageTurnIntervalSeconds);
}

void EpubReaderActivity::setAutoPageTurnIntervalSeconds(uint16_t seconds) {
  if (seconds == 0) {
    automaticPageTurnActive = false;
    return;
  }

  seconds = clampAutoPageTurnIntervalSeconds(seconds);
  lastAutoPageTurnIntervalSeconds = seconds;
  if (epub) {
    saveAutoPageTurnIntervalSeconds(epub->getCachePath(), seconds);
  }
  lastPageTurnTime = millis();
  pageTurnDuration = static_cast<unsigned long>(seconds) * 1000UL;
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn, const char* source) {
  pageLoadRetryCount = 0;
  if (isForwardTurn) {
    uint32_t forwardReadSeconds = 0;
    const bool shouldRecordForwardRead = forwardPageReadElapsed(forwardReadSeconds, source);
    recordCurrentPageReadingTime(source);
    const bool exitingChapter = section && section->pageCount > 0 && section->currentPage >= section->pageCount - 1;
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      if (shouldQueueCompletionPromptOnChapterExit()) {
        completionPromptQueued = true;
        requestUpdate();
        return;
      }

      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);

        // Preserve the exact offset when the next spine item is merely an
        // internal RAM-safe fragment of this same logical chapter. Reading
        // cache headers alone is not sufficient: adjacent fragments can have
        // been built using different adaptive-memory cache suffixes.
        rememberLogicalForwardCarry();

        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();

        // Force status aggregation to recompute for the new internal chunk.
        logicalStatusCacheSpineIndex = -1;
        logicalStatusCacheLocalPageCount = -1;
      }
    }
    if (shouldRecordForwardRead) {
      if (!exitingChapter) {
        recordForwardPagePaceSample(forwardReadSeconds, source);
      }
      stats.totalPagesTurned++;
      globalStats.totalPagesTurned++;
    }
  } else {
    recordCurrentPageReadingTime(source);
    armReadingPaceWarmup("back_page");
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
        clearLogicalPageCarry();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

void EpubReaderActivity::rememberLogicalForwardCarry() {
  if (!epub || !section || section->pageCount <= 0) {
    clearLogicalPageCarry();
    return;
  }

  int logicalStart = currentSpineIndex;
  int logicalEnd = currentSpineIndex;
  const bool continuesLogicalChapter =
      epub->getLogicalChapterBounds(currentSpineIndex, logicalStart, logicalEnd) && currentSpineIndex < logicalEnd;
  if (!continuesLogicalChapter) {
    clearLogicalPageCarry();
    return;
  }

  const int carriedBefore = logicalPageCarryNextSpine == currentSpineIndex
                                ? logicalPageCarryPagesBefore
                                : logicalStatusPagesBefore;
  logicalPageCarryNextSpine = currentSpineIndex + 1;
  logicalPageCarryPagesBefore = std::max(0, carriedBefore) + section->pageCount;
}

void EpubReaderActivity::clearLogicalPageCarry() {
  logicalPageCarryNextSpine = -1;
  logicalPageCarryPagesBefore = 0;
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // A chapter-skip burst may arrive in the tiny interval after RenderLock is
  // taken but before cancellable XML work begins. Do not build the now-stale
  // chapter; the main loop applies the coalesced target after the lock drops.
  if (coalescedSpineDelta.load(std::memory_order_relaxed) != 0) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  const auto showLowMemoryLayoutError = [this]() {
    snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_EPUB_LAYOUT_MEMORY_TITLE));
    snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", tr(STR_EPUB_LAYOUT_MEMORY_BODY));
    APP_STATE.pendingAlertGoHomeOnBack.store(true, std::memory_order_relaxed);
    APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
    GUI.drawPopup(renderer, tr(STR_EPUB_LAYOUT_MEMORY_TITLE));
  };

  const auto queueFallbackFontAlert = []() {
    snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_EPUB_FALLBACK_FONT_TITLE));
    snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", tr(STR_EPUB_FALLBACK_FONT_BODY));
    APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
    APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  const ReaderViewportLayout layout = computeReaderViewportLayout(renderer, automaticPageTurnActive);
  const uint16_t viewportWidth = layout.viewportWidth;
  const uint16_t viewportHeight = layout.viewportHeight;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d (free=%u, maxAlloc=%u)", filepath.c_str(), currentSpineIndex,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    const int readerFontId = SETTINGS.getReaderFontId();
    const int fallbackFontId = SETTINGS.getBuiltInReaderFontId();
    const bool canUseFallbackFont = renderer.isSdCardFont(readerFontId) && fallbackFontId != readerFontId;

    // A new chapter no longer needs the decompressed glyph slots accumulated
    // while drawing the previous one. Reclaim only that rebuildable page-local
    // data before loading/layout; keep the selected SD font's metrics, kerning
    // and ligature tables resident so normal page turns stay fast.
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCache();
    }

    auto heap = MemoryBudget::snapshot();
    auto memoryPolicy = reader::selectReaderMemoryPolicy(heap.freeHeap, heap.maxAllocHeap);
    if (memoryPolicy.mode == reader::ReaderMemoryMode::Unavailable && canUseFallbackFont) {
      releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "reader memory-mode selection");
      heap = MemoryBudget::snapshot();
      memoryPolicy = reader::selectReaderMemoryPolicy(heap.freeHeap, heap.maxAllocHeap);
    }
    if (memoryPolicy.mode == reader::ReaderMemoryMode::Unavailable) {
      // A release build can reach this point with a temporarily fragmented
      // heap even though the same chapter fits after a clean boot. Reclaim all
      // rebuildable glyph data once before showing a blocking memory error.
      if (auto* fcm = renderer.getFontCacheManager()) {
        fcm->clearCache();
      }
      if (renderer.isSdCardFont(readerFontId)) {
        renderer.releaseSdCardFontForLowMemory(readerFontId);
      }
      delay(1);
      yield();
      heap = MemoryBudget::snapshot();
      memoryPolicy = reader::selectReaderMemoryPolicy(heap.freeHeap, heap.maxAllocHeap);
    }

    bool loadedSection = false;
    bool usedFallbackFont = false;
    reader::ReaderMemoryMode activeMemoryMode = memoryPolicy.mode;
    auto loadSectionWithConfig = [&](const SectionMemoryConfig& config) {
      section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer, config.suffix);
      if (!section) {
        LOG_ERR("ERS", "Failed to allocate section for spine %d (font=%d, free=%u, maxAlloc=%u)", currentSpineIndex,
                config.fontId, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return false;
      }
      if (!section->loadSectionFile(config.fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                    SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth,
                                    viewportHeight, config.hyphenationEnabled, config.embeddedStyle,
                                    config.imageRendering, config.bionicReadingEnabled,
                                    config.guideReadingEnabled)) {
        section.reset();
        return false;
      }
      activeSectionFontId = config.fontId;
      activeSectionUsesFallbackFont = config.fontId != readerFontId;
      activeMemoryMode = config.mode;
      return true;
    };

    // Cached pages are cheap to open. Prefer the full-quality cache regardless
    // of current heap; memory mode only controls a missing-cache rebuild.
    loadedSection = loadSectionWithConfig(
        sectionMemoryConfig(reader::ReaderMemoryMode::Normal, readerFontId, fallbackFontId));
    // A fallback cache is an emergency artifact, not the preferred chapter.
    // Reuse it only when current memory cannot safely attempt a full-quality
    // rebuild. Otherwise the selected SD font must get another chance first.
    if (!loadedSection && canUseFallbackFont && memoryPolicy.mode == reader::ReaderMemoryMode::Unavailable) {
      SectionMemoryConfig fallbackConfig =
          sectionMemoryConfig(reader::ReaderMemoryMode::Normal, readerFontId, fallbackFontId);
      fallbackConfig.suffix = FALLBACK_FONT_SECTION_CACHE_SUFFIX;
      fallbackConfig.fontId = fallbackFontId;
      loadedSection = loadSectionWithConfig(fallbackConfig);
      if (loadedSection) {
        usedFallbackFont = true;
      }
    }
    // Never reuse degraded Safe/Survival caches as the preferred representation.
    // A well-formed EPUB must always get a chance to rebuild at full quality.
    // Old low-memory caches may have been created with illustrations omitted.

    if (!loadedSection) {
      if (memoryPolicy.mode == reader::ReaderMemoryMode::Unavailable) {
        LOG_ERR("ERS", "Reader has no safe memory mode (free=%u maxAlloc=%u)", heap.freeHeap, heap.maxAllocHeap);
        showLowMemoryLayoutError();
        return;
      }

      showChapterLoadingPopup();
      const auto popupFn = []() {};
      reader::ScopedReaderWork work(readerWork);
      if (coalescedSpineDelta.load(std::memory_order_relaxed) != 0) {
        return;
      }
      bool imagesWereSuppressed = false;
      bool layoutAbortedForLowMemory = false;
      bool cancelled = false;
      bool buildSucceeded = false;
      // Always attempt the publisher-quality layout first. Memory policy may
      // choose how we recover after a real allocation/layout failure, but it
      // must never pre-emptively downgrade a healthy EPUB.
      reader::ReaderMemoryMode buildMode = reader::ReaderMemoryMode::Normal;
      bool normalFontRecoveryAttempted = false;
      bool normalFallbackAttempted = false;

      while (buildMode != reader::ReaderMemoryMode::Unavailable) {
        if (work.token().isCancellationRequested()) {
          cancelled = true;
          break;
        }

        SectionMemoryConfig config = sectionMemoryConfig(buildMode, readerFontId, fallbackFontId);
        if (buildMode == reader::ReaderMemoryMode::Normal && normalFallbackAttempted) {
          config.suffix = FALLBACK_FONT_SECTION_CACHE_SUFFIX;
          config.fontId = fallbackFontId;
        }
        if (buildMode != reader::ReaderMemoryMode::Normal && canUseFallbackFont) {
          releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "adaptive section build");
        }
        LOG_INF("ERS", "Building spine %d in %s mode (free=%u maxAlloc=%u)", currentSpineIndex,
                readerMemoryModeName(buildMode), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        {
          char breadcrumb[48];
          snprintf(breadcrumb, sizeof(breadcrumb), "ERS build s=%d mode=%s", currentSpineIndex,
                   readerMemoryModeName(buildMode));
          HalSystem::recordBreadcrumb(breadcrumb);
        }

        section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer, config.suffix);
        if (!section) {
          layoutAbortedForLowMemory = true;
        } else {
          bool attemptImagesSuppressed = false;
          bool attemptLowMemory = false;
          bool attemptCancelled = false;
          buildSucceeded = section->createSectionFile(
              config.fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
              SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
              config.hyphenationEnabled, config.embeddedStyle, config.imageRendering, config.bionicReadingEnabled,
              config.guideReadingEnabled, popupFn, &attemptImagesSuppressed, &attemptLowMemory, &work.token(),
              &attemptCancelled);
          imagesWereSuppressed = imagesWereSuppressed || attemptImagesSuppressed;
          layoutAbortedForLowMemory = attemptLowMemory;
          cancelled = attemptCancelled;
        }

        if (buildSucceeded) {
          activeSectionFontId = config.fontId;
          activeSectionUsesFallbackFont = config.fontId != readerFontId;
          activeMemoryMode = config.mode;
          usedFallbackFont = buildMode == reader::ReaderMemoryMode::Normal && config.fontId != readerFontId;
          break;
        }
        section.reset();
        if (cancelled || !layoutAbortedForLowMemory) break;

        if (buildMode == reader::ReaderMemoryMode::Normal && canUseFallbackFont &&
            !normalFontRecoveryAttempted) {
          // The first allocation failure may be fragmentation left by the
          // previous chapter. Drop rebuildable glyph data and retry the exact
          // same publisher-quality layout with the user's selected font before
          // considering the built-in fallback font.
          normalFontRecoveryAttempted = true;
          if (auto* fcm = renderer.getFontCacheManager()) {
            fcm->clearCache();
          }
          releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "same-font chapter retry");
          delay(1);
          yield();
          continue;
        }

        if (buildMode == reader::ReaderMemoryMode::Normal && canUseFallbackFont && !normalFallbackAttempted) {
          normalFallbackAttempted = true;
          releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "full-quality fallback-font retry");
          continue;
        }
        buildMode = buildMode == reader::ReaderMemoryMode::Normal
                        ? reader::ReaderMemoryMode::Safe
                        : (buildMode == reader::ReaderMemoryMode::Safe ? reader::ReaderMemoryMode::Survival
                                                                       : reader::ReaderMemoryMode::Unavailable);
      }

      if (cancelled) {
        LOG_INF("ERS", "Discarded obsolete section request for spine %d", currentSpineIndex);
        section.reset();
        return;
      }
      if (!buildSucceeded && layoutAbortedForLowMemory) {
        // A first-pass low-memory failure is often transient: the parser/font
        // caches used by the failed attempt are released only after its objects
        // unwind. Reclaim every rebuildable font cache here and retry the same
        // chapter once, transparently, instead of throwing the reader back out
        // and making the user open the book a second time.
        section.reset();
        if (auto* fcm = renderer.getFontCacheManager()) {
          fcm->clearCache();
        }
        if (renderer.isSdCardFont(readerFontId)) {
          renderer.releaseSdCardFontForLowMemory(readerFontId);
        }
        delay(1);
        yield();

        const auto recovered = MemoryBudget::snapshot();
        LOG_INF("ERS", "Retrying spine %d after emergency cache reclaim (free=%u maxAlloc=%u)",
                currentSpineIndex, recovered.freeHeap, recovered.maxAllocHeap);

        // Use the built-in font for the emergency retry because it requires no
        // SD-font caches, but keep publisher CSS and all images. This is a
        // quality-preserving memory fallback, not a stripped-down book mode.
        SectionMemoryConfig retryConfig =
            sectionMemoryConfig(reader::ReaderMemoryMode::Survival, readerFontId, fallbackFontId);
        section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer, retryConfig.suffix);
        if (section) {
          bool retryImagesSuppressed = false;
          bool retryLowMemory = false;
          bool retryCancelled = false;
          buildSucceeded = section->createSectionFile(
              retryConfig.fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
              SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
              retryConfig.hyphenationEnabled, retryConfig.embeddedStyle, retryConfig.imageRendering,
              retryConfig.bionicReadingEnabled, retryConfig.guideReadingEnabled, popupFn,
              &retryImagesSuppressed, &retryLowMemory, &work.token(), &retryCancelled);
          if (buildSucceeded) {
            activeSectionFontId = retryConfig.fontId;
            activeSectionUsesFallbackFont = retryConfig.fontId != readerFontId;
            activeMemoryMode = retryConfig.mode;
            usedFallbackFont = activeSectionUsesFallbackFont;
            imagesWereSuppressed = imagesWereSuppressed || retryImagesSuppressed;
            layoutAbortedForLowMemory = false;
            cancelled = retryCancelled;
          } else {
            layoutAbortedForLowMemory = retryLowMemory;
            section.reset();
          }
        }
      }

      if (!buildSucceeded) {
        section.reset();
        if (layoutAbortedForLowMemory) {
          showLowMemoryLayoutError();
        } else {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          showPendingSyncSaveError();
        }
        return;
      }

      releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "section cache build");
      LOG_INF("ERS", "Cache build complete: pages=%u mode=%s font=%d free=%u maxAlloc=%u", section->pageCount,
              readerMemoryModeName(activeMemoryMode), activeSectionFontId, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      {
        char breadcrumb[48];
        snprintf(breadcrumb, sizeof(breadcrumb), "ERS done s=%d pages=%u", currentSpineIndex, section->pageCount);
        HalSystem::recordBreadcrumb(breadcrumb);
      }

      if (usedFallbackFont) {
        queueFallbackFontAlert();
      } else if (imagesWereSuppressed) {
        // Image fallback is recoverable: the section cache already contains
        // all text. Do not replace the newly opened book with a blocking
        // alert screen; leave the reader on its first text page and record
        // the omitted images in the serial log instead.
        LOG_INF("ERS", "Opened chapter %d without some images (low-memory fallback)", currentSpineIndex);
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build... (pages=%u, font=%d fallback=%u free=%u, maxAlloc=%u)",
              section->pageCount, activeSectionFontId, activeSectionUsesFallbackFont ? 1U : 0U, ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
      if (usedFallbackFont) {
        queueFallbackFontAlert();
      }
    }

    if (!section) {
      LOG_ERR("ERS", "Section load/build did not produce a section");
      showPendingSyncSaveError();
      return;
    }

    // FB2 virtual chunks are intentionally pre-paginated so the user sees one
    // stable logical chapter page count immediately. Do NOT do this for the
    // oversized real-EPUB splitter: its whole purpose is to make a giant XHTML
    // chapter open quickly by laying out only the current ~200 KiB fragment.
    // Pre-paginating every *_splitN sibling here made the splitter pointless and
    // caused books with one 1+ MiB XHTML file to sit on "Loading" until most of
    // the book had already been laid out. For split EPUBs the status bar already
    // knows how to accumulate page counts from caches that actually exist, and
    // unopened fragments are prepared lazily as the reader reaches them.
    // FB2 fragments must be paginated as one logical chapter before its first
    // page is shown. Otherwise the denominator contains only fragments whose
    // caches happen to exist and grows while reading (1/3 -> 4/6 -> ...).
    // Image dimensions are now read from lightweight PNG/JPEG headers and
    // image decoding is streamed/deferred, so preparing sibling text layouts
    // no longer requires keeping a full image decoder alive for every chunk.
    // Keep this FB2-only: pre-paginating oversized split EPUB XHTML would undo
    // the EPUB lazy-opening design.
    if (epub->isFb2Package()) {
      int logicalStart = currentSpineIndex;
      int logicalEnd = currentSpineIndex;
      if (epub->getLogicalChapterBounds(currentSpineIndex, logicalStart, logicalEnd) && logicalStart != logicalEnd) {
        LOG_INF("ERS", "Preparing exact logical chapter page map: %d..%d", logicalStart, logicalEnd);

        auto loadSiblingCache = [&](const int siblingSpine, const SectionMemoryConfig& config) -> bool {
          Section cached(epub, siblingSpine, renderer, config.suffix);
          return cached.loadSectionFile(
              config.fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
              SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
              config.hyphenationEnabled, config.embeddedStyle, config.imageRendering, config.bionicReadingEnabled,
              config.guideReadingEnabled);
        };

        bool logicalChapterReady = true;
        bool preparationPopupShown = false;
        for (int siblingSpine = logicalStart; siblingSpine <= logicalEnd; ++siblingSpine) {
          if (siblingSpine == currentSpineIndex) continue;

          auto normal = sectionMemoryConfig(reader::ReaderMemoryMode::Normal, readerFontId, fallbackFontId);
          bool siblingReady = loadSiblingCache(siblingSpine, normal);
          if (!siblingReady && canUseFallbackFont) {
            normal.fontId = fallbackFontId;
            normal.suffix = FALLBACK_FONT_SECTION_CACHE_SUFFIX;
            siblingReady = loadSiblingCache(siblingSpine, normal);
          }
          if (!siblingReady) {
            siblingReady = loadSiblingCache(
                siblingSpine, sectionMemoryConfig(reader::ReaderMemoryMode::Safe, readerFontId, fallbackFontId));
          }
          if (!siblingReady) {
            siblingReady = loadSiblingCache(
                siblingSpine, sectionMemoryConfig(reader::ReaderMemoryMode::Survival, readerFontId, fallbackFontId));
          }
          if (siblingReady) continue;

          if (!preparationPopupShown) {
            // If the active section came from cache, the framebuffer still
            // contains a clean visible page and it is safe to draw the loading
            // notification now. If it was just built, its image pre-cache pass
            // has already reused the framebuffer as scratch; the notification
            // was displayed before that build and must not be displayed again
            // from the dirty working buffer.
            if (loadedSection) {
              showChapterLoadingPopup();
            }
            preparationPopupShown = true;
          }

          auto siblingHeap = MemoryBudget::snapshot();
          auto siblingPolicy = reader::selectReaderMemoryPolicy(siblingHeap.freeHeap, siblingHeap.maxAllocHeap);
          if (siblingPolicy.mode == reader::ReaderMemoryMode::Unavailable && canUseFallbackFont) {
            releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "logical chapter exact pagination");
            siblingHeap = MemoryBudget::snapshot();
            siblingPolicy = reader::selectReaderMemoryPolicy(siblingHeap.freeHeap, siblingHeap.maxAllocHeap);
          }

          reader::ReaderMemoryMode siblingMode = siblingPolicy.mode;
          bool normalFallbackAttempted = false;
          while (!siblingReady && siblingMode != reader::ReaderMemoryMode::Unavailable) {
            SectionMemoryConfig config = sectionMemoryConfig(siblingMode, readerFontId, fallbackFontId);
            if (siblingMode == reader::ReaderMemoryMode::Normal && normalFallbackAttempted) {
              config.fontId = fallbackFontId;
              config.suffix = FALLBACK_FONT_SECTION_CACHE_SUFFIX;
            }
            if (siblingMode != reader::ReaderMemoryMode::Normal && canUseFallbackFont) {
              releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "logical chapter fragment pagination");
            }

            bool imagesSuppressed = false;
            bool lowMemory = false;
            bool siblingCancelled = false;
            {
              reader::ScopedReaderWork siblingWork(readerWork);
              Section sibling(epub, siblingSpine, renderer, config.suffix);
              siblingReady = sibling.createSectionFile(
                  config.fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                  SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                  config.hyphenationEnabled, config.embeddedStyle, config.imageRendering, config.bionicReadingEnabled,
                  config.guideReadingEnabled, []() {}, &imagesSuppressed, &lowMemory, &siblingWork.token(),
                  &siblingCancelled);
            }

            if (siblingCancelled) return;
            if (siblingReady) break;
            if (!lowMemory) break;
            if (siblingMode == reader::ReaderMemoryMode::Normal && canUseFallbackFont &&
                !normalFallbackAttempted) {
              normalFallbackAttempted = true;
              continue;
            }
            siblingMode = siblingMode == reader::ReaderMemoryMode::Normal
                              ? reader::ReaderMemoryMode::Safe
                              : (siblingMode == reader::ReaderMemoryMode::Safe
                                     ? reader::ReaderMemoryMode::Survival
                                     : reader::ReaderMemoryMode::Unavailable);
          }

          if (!siblingReady) {
            LOG_ERR("ERS", "Could not build exact page map for logical fragment %d", siblingSpine);
            logicalChapterReady = false;
            break;
          }
          LOG_INF("ERS", "Prepared logical fragment %d/%d", siblingSpine - logicalStart + 1,
                  logicalEnd - logicalStart + 1);
        }

        if (!logicalChapterReady) {
          LOG_ERR("ERS", "Logical chapter page total remains incomplete");
        }
      }
    }

    logicalStatusCacheSpineIndex = -1;
    logicalStatusCacheLocalPageCount = -1;

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      if (pendingBookmarkParagraphIndex != UINT16_MAX) {
        if (const auto paragraphPage = section->getPageForParagraphIndex(pendingBookmarkParagraphIndex)) {
          section->currentPage = *paragraphPage;
          LOG_DBG("ERS", "Resolved bookmark paragraph %u to page %u", pendingBookmarkParagraphIndex, *paragraphPage);
        } else {
          LOG_DBG("ERS", "Bookmark paragraph %u not found; using saved chapter progress",
                  pendingBookmarkParagraphIndex);
        }
      }
      pendingBookmarkParagraphIndex = UINT16_MAX;
      pendingPercentJump = false;
    }

    // Apply rapid page presses only after the page count is known. If the
    // requested position crosses a virtual-section boundary, carry the
    // remainder into the adjacent section and render only the final target.
    // This preserves normal one-page navigation semantics without displaying
    // every intermediate page or allocating work for several chapters at once.
    const uint32_t settleUntil = navigationSettleUntilMs.load(std::memory_order_relaxed);
    if (coalescedPageDelta.load(std::memory_order_relaxed) != 0 && settleUntil != 0 &&
        static_cast<int32_t>(millis() - settleUntil) < 0) {
      // Keep the loading overlay/previous page visible until the short burst
      // of input stops. The main loop applies the complete queue after the
      // deadline and requests exactly one final display update.
      return;
    }
    const int32_t fastPageDelta = coalescedPageDelta.exchange(0, std::memory_order_relaxed);
    if (fastPageDelta != 0 && section->pageCount > 0) {
      const int64_t targetPage = static_cast<int64_t>(section->currentPage) + fastPageDelta;
      if (targetPage >= section->pageCount && currentSpineIndex + 1 < epub->getSpineItemsCount()) {
        const int64_t remainder = targetPage - section->pageCount;
        coalescedPageDelta.store(static_cast<int32_t>(std::min<int64_t>(remainder, INT32_MAX)),
                                 std::memory_order_relaxed);
        rememberLogicalForwardCarry();
        currentSpineIndex++;
        nextPageNumber = 0;
        pendingPageJump.reset();
        section.reset();
        logicalStatusCacheSpineIndex = -1;
        logicalStatusCacheLocalPageCount = -1;
        navigationSettleUntilMs.store(0, std::memory_order_relaxed);
        requestUpdate();
        return;
      }
      if (targetPage >= section->pageCount && currentSpineIndex + 1 == epub->getSpineItemsCount()) {
        clearLogicalPageCarry();
        currentSpineIndex = epub->getSpineItemsCount();
        nextPageNumber = 0;
        section.reset();
        logicalStatusCacheSpineIndex = -1;
        logicalStatusCacheLocalPageCount = -1;
        navigationSettleUntilMs.store(0, std::memory_order_relaxed);
        requestUpdate();
        return;
      }
      if (targetPage < 0 && currentSpineIndex > 0) {
        clearLogicalPageCarry();
        const int64_t remainder = targetPage + 1;
        coalescedPageDelta.store(static_cast<int32_t>(std::max<int64_t>(remainder, INT32_MIN)),
                                 std::memory_order_relaxed);
        currentSpineIndex--;
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        section.reset();
        logicalStatusCacheSpineIndex = -1;
        logicalStatusCacheLocalPageCount = -1;
        navigationSettleUntilMs.store(0, std::memory_order_relaxed);
        requestUpdate();
        return;
      }
      section->currentPage = static_cast<int>(std::max<int64_t>(
          0, std::min<int64_t>(targetPage, static_cast<int64_t>(section->pageCount) - 1)));
      navigationSettleUntilMs.store(0, std::memory_order_relaxed);
    }

    // Clamp the current page to ensure we stay within bounds if reader settings have
    // changed since the page number (e.g., via a bookmark) was saved.
    if (section->pageCount > 0) {
      if (section->currentPage >= section->pageCount) {
        section->currentPage = section->pageCount - 1;
      } else if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }
  }

  if (coalescedSpineDelta.load(std::memory_order_relaxed) != 0) {
    return;
  }
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), ReaderUtils::readerForegroundBlack(),
                              EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), ReaderUtils::readerForegroundBlack(),
                              EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      pageLoadRetryCount++;
      if (pageLoadRetryCount < MAX_PAGE_LOAD_RETRIES) {
        LOG_ERR("ERS", "Failed to load page from SD (retry %d) - clearing section cache", pageLoadRetryCount);
        section->clearCache();
        section.reset();
        requestUpdate();
        automaticPageTurnActive = false;
        showPendingSyncSaveError();
        return;
      }

      LOG_ERR("ERS", "Failed to load page from SD after %d retries", pageLoadRetryCount);
      renderer.clearScreen(ReaderUtils::readerBackgroundColor());
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), ReaderUtils::readerForegroundBlack(),
                                EpdFontFamily::BOLD);
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    pageLoadRetryCount = 0;

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    const int renderFontId = activeSectionFontId != 0 ? activeSectionFontId : SETTINGS.getReaderFontId();
    renderContents(std::move(p), renderFontId, layout.marginTop, layout.marginRight, layout.marginBottom,
                   layout.marginLeft);
    pageShownAtMs = millis();
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  if (!saveProgress(currentSpineIndex, section->currentPage, section->pageCount)) {
    pendingSyncSaveError = true;
  }
  queueCompletionPromptIfNeeded();

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int fontId, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();
  const auto heapBefore = MemoryBudget::snapshot();
  auto scope = fcm->createPrewarmScope();
  page->renderText(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const auto heapAfter = MemoryBudget::snapshot();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG(
      "ERS", "Heap prewarm: free=%u->%u delta=%ld maxAlloc=%u->%u delta=%ld largestPct=%u->%u", heapBefore.freeHeap,
      heapAfter.freeHeap,
      static_cast<long>(static_cast<int32_t>(heapAfter.freeHeap) - static_cast<int32_t>(heapBefore.freeHeap)),
      heapBefore.maxAllocHeap, heapAfter.maxAllocHeap,
      static_cast<long>(static_cast<int32_t>(heapAfter.maxAllocHeap) - static_cast<int32_t>(heapBefore.maxAllocHeap)),
      largestBlockPercent(heapBefore), largestBlockPercent(heapAfter));

  const bool pageHasImages = page->hasImages();
  const bool foregroundBlack = ReaderUtils::readerForegroundBlack();
  const bool needsImageGrayscale = pageHasImages;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing && foregroundBlack;
  const bool needsAnyGrayscale = needsTextGrayscale || needsImageGrayscale;
  const int contentBottom = renderer.getScreenHeight() - orientedMarginBottom;

  const auto finalizeBufferComposition = [&]() {
    drawPublisherPageMarkers(renderer, *page, orientedMarginTop, contentBottom, foregroundBlack);
    if (section && section->currentPage >= 0) {
      ClippingUtils::drawSavedHighlights(renderer, *page, clippings.getClippings(),
                                         static_cast<uint16_t>(currentSpineIndex),
                                         static_cast<uint16_t>(section->currentPage), fontId,
                                         orientedMarginLeft, orientedMarginTop, foregroundBlack);
    }
  };

  const auto composePageBuffer = [&]() {
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, foregroundBlack);
    finalizeBufferComposition();
  };

  const auto composeGrayscaleBuffer = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, foregroundBlack);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    finalizeBufferComposition();
  };

  composePageBuffer();
  renderStatusBar();
  if (pendingBookmarkFeedback) {
    const char* msg = tr(STR_BOOKMARK_ADDED);
    switch (bookmarkFeedbackType) {
      case BookmarkFeedbackType::Added:
        msg = tr(STR_BOOKMARK_ADDED);
        break;
      case BookmarkFeedbackType::Removed:
        msg = tr(STR_BOOKMARK_REMOVED);
        break;
      case BookmarkFeedbackType::LimitReached:
        msg = tr(STR_BOOKMARK_LIMIT_REACHED);
        break;
    }
    drawToastBuffer(renderer, msg);
  }
  if (pendingCompletedFeedback) {
    const char* msg = completedFeedbackIsFinished ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED);
    drawToastBuffer(renderer, msg);
  }
  if (pendingTiltPageTurnFeedback) {
    const char* msg = tiltPageTurnFeedbackEnabled ? tr(STR_TILT_TO_TURN_ON) : tr(STR_TILT_TO_TURN_OFF);
    drawToastBuffer(renderer, msg);
  }
  fcm->logStats("bw_render");
  const auto tBwRender = millis();
  const auto logImagePageProfile = [](const uint32_t imageBlankDisplayMs, const uint32_t imageRestoreRenderMs,
                                      const uint32_t imageFinalDisplayMs) {
    LOG_DBG("ERS", "Image page profile: blank_display=%lums restore_render=%lums final_display=%lums",
            imageBlankDisplayMs, imageRestoreRenderMs, imageFinalDisplayMs);
  };

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      const auto tImageBlankDisplay = millis();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      const uint32_t imageBlankDisplayMs = millis() - tImageBlankDisplay;

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      const auto tImageRestoreRender = millis();
      composePageBuffer();
      const uint32_t imageRestoreRenderMs = millis() - tImageRestoreRender;
      const auto tImageFinalDisplay = millis();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      const uint32_t imageFinalDisplayMs = millis() - tImageFinalDisplay;
      logImagePageProfile(imageBlankDisplayMs, imageRestoreRenderMs, imageFinalDisplayMs);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  TiledGrayscaleTimings tiledTimings;
  if (runTiledGrayscalePass(renderer, *page, fontId, orientedMarginLeft, orientedMarginTop, foregroundBlack,
                            needsTextGrayscale, needsImageGrayscale, tiledTimings)) {
    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tiledTimings.grayLsb - tDisplay,
            tiledTimings.grayMsb - tiledTimings.grayLsb, tiledTimings.grayDisplay - tiledTimings.grayMsb,
            tiledTimings.cleanup - tiledTimings.grayDisplay, tEnd - t0);
    return;
  }

  // Save bw buffer to reset buffer state after grayscale data sync
  const auto bwStoreHeapBefore = MemoryBudget::snapshot();
  const bool storedBwBuffer = needsAnyGrayscale && renderer.storeBwBuffer();
  const auto bwStoreHeapAfter = MemoryBudget::snapshot();
  const auto tBwStore = millis();
  const bool canApplyGrayscale = needsAnyGrayscale && storedBwBuffer;
  if (needsAnyGrayscale && !storedBwBuffer) {
    LOG_ERR("ERS", "Skipping grayscale enhancement: failed to store BW backup");
  }

  // grayscale rendering
  if (canApplyGrayscale) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    composeGrayscaleBuffer();
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    composeGrayscaleBuffer();
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();

    // display grayscale part
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_store_ok=%d "
            "bw_store_free=%u->%u delta=%ld bw_store_maxAlloc=%u->%u delta=%ld bw_store_largestPct=%u->%u "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, storedBwBuffer,
            bwStoreHeapBefore.freeHeap, bwStoreHeapAfter.freeHeap,
            static_cast<long>(static_cast<int32_t>(bwStoreHeapAfter.freeHeap) -
                              static_cast<int32_t>(bwStoreHeapBefore.freeHeap)),
            bwStoreHeapBefore.maxAllocHeap, bwStoreHeapAfter.maxAllocHeap,
            static_cast<long>(static_cast<int32_t>(bwStoreHeapAfter.maxAllocHeap) -
                              static_cast<int32_t>(bwStoreHeapBefore.maxAllocHeap)),
            largestBlockPercent(bwStoreHeapBefore), largestBlockPercent(bwStoreHeapAfter), tGrayLsb - tBwStore,
            tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    if (storedBwBuffer) {
      // Restore the BW data when we skipped grayscale entirely.
      renderer.restoreBwBuffer();
    }
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_store_ok=%d "
            "bw_store_free=%u->%u delta=%ld bw_store_maxAlloc=%u->%u delta=%ld bw_store_largestPct=%u->%u "
            "bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, storedBwBuffer,
            bwStoreHeapBefore.freeHeap, bwStoreHeapAfter.freeHeap,
            static_cast<long>(static_cast<int32_t>(bwStoreHeapAfter.freeHeap) -
                              static_cast<int32_t>(bwStoreHeapBefore.freeHeap)),
            bwStoreHeapBefore.maxAllocHeap, bwStoreHeapAfter.maxAllocHeap,
            static_cast<long>(static_cast<int32_t>(bwStoreHeapAfter.maxAllocHeap) -
                              static_cast<int32_t>(bwStoreHeapBefore.maxAllocHeap)),
            largestBlockPercent(bwStoreHeapBefore), largestBlockPercent(bwStoreHeapAfter), tBwRestore - tBwStore,
            tEnd - t0);
  }
}

void EpubReaderActivity::renderStatusBar() const {
  int currentPage = section->currentPage + 1;
  int pageCount = section->pageCount;
  const float bookProgress = getCurrentBookProgressPercent();

  // One logical TOC chapter may be split into several spine items:
  // - FB2 deliberately creates virtual chunks to stay inside the ESP32-C3 RAM budget;
  // - oversized EPUB spine items can be split for the same reason.
  //
  // Keep the split internally, but make the page counter monotonic across
  // chunks. The just-finished chunk is carried in RAM; older chunks are
  // recovered only from tiny existing section-cache headers when needed.
  // No unopened chunk is paginated ahead of time.
  if (epub && section && pageCount > 0) {
    if (logicalStatusCacheSpineIndex != currentSpineIndex ||
        logicalStatusCacheLocalPageCount != section->pageCount) {
      logicalStatusCacheSpineIndex = currentSpineIndex;
      logicalStatusCacheLocalPageCount = section->pageCount;
      logicalStatusPagesBefore = 0;
      logicalStatusKnownPageCount = section->pageCount;

      int logicalStart = currentSpineIndex;
      int logicalEnd = currentSpineIndex;
      const bool splitLogicalChapter =
          epub->getLogicalChapterBounds(currentSpineIndex, logicalStart, logicalEnd) && logicalStart != logicalEnd;

      if (splitLogicalChapter) {
        // Exact/recovery path: read only tiny headers for sibling caches that
        // already exist. Unopened fragments are deliberately not paginated.
        const auto layout = computeReaderViewportLayout(renderer, automaticPageTurnActive);
        const int readerFontId = SETTINGS.getReaderFontId();
        const int fallbackFontId = SETTINGS.getBuiltInReaderFontId();
        const bool canUseFallbackFont = renderer.isSdCardFont(readerFontId) && fallbackFontId != readerFontId;

        auto cachedPageCount = [&](const int spineIndex) -> int {
          auto tryCache = [&](const SectionMemoryConfig& config) -> int {
            Section cached(epub, spineIndex, renderer, config.suffix);
            if (!cached.loadSectionFile(config.fontId, SETTINGS.getReaderLineCompression(),
                                        SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                        SETTINGS.paragraphAlignment, layout.viewportWidth, layout.viewportHeight,
                                        config.hyphenationEnabled, config.embeddedStyle, config.imageRendering,
                                        config.bionicReadingEnabled, config.guideReadingEnabled)) {
              return 0;
            }
            return cached.pageCount;
          };

          auto normal = sectionMemoryConfig(reader::ReaderMemoryMode::Normal, readerFontId, fallbackFontId);
          int pages = tryCache(normal);
          if (pages == 0 && canUseFallbackFont) {
            normal.fontId = fallbackFontId;
            normal.suffix = FALLBACK_FONT_SECTION_CACHE_SUFFIX;
            pages = tryCache(normal);
          }
          if (pages == 0) {
            const auto safe = sectionMemoryConfig(reader::ReaderMemoryMode::Safe, readerFontId, fallbackFontId);
            pages = tryCache(safe);
          }
          if (pages == 0) {
            const auto survival =
                sectionMemoryConfig(reader::ReaderMemoryMode::Survival, readerFontId, fallbackFontId);
            pages = tryCache(survival);
          }
          return pages;
        };

        logicalStatusKnownPageCount = 0;
        for (int spine = logicalStart; spine <= logicalEnd; ++spine) {
          const int pages = spine == currentSpineIndex ? section->pageCount : cachedPageCount(spine);
          if (pages <= 0) {
            // If a sibling has not been opened yet, report only the known part
            // rather than inventing an estimated total.
            continue;
          }
          if (spine < currentSpineIndex) {
            logicalStatusPagesBefore += pages;
          }
          logicalStatusKnownPageCount += pages;
        }

        if (logicalStatusKnownPageCount < section->pageCount) {
          logicalStatusKnownPageCount = section->pageCount;
        }

        // Sequential navigation knows the exact number of pages already
        // crossed even when an older sibling cache cannot be reopened. Use
        // the larger offset so cache recovery and the runtime carry never
        // double-count each other.
        if (logicalPageCarryNextSpine == currentSpineIndex) {
          logicalStatusPagesBefore =
              std::max(logicalStatusPagesBefore, logicalPageCarryPagesBefore);
          logicalStatusKnownPageCount =
              std::max(logicalStatusKnownPageCount, logicalStatusPagesBefore + section->pageCount);
        }
      }

    }

    if (logicalStatusKnownPageCount > section->pageCount || logicalStatusPagesBefore > 0) {
      currentPage = logicalStatusPagesBefore + section->currentPage + 1;
      pageCount = logicalStatusKnownPageCount;
    }
  }

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(pageTurnDuration / 1000);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == InkMODSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == InkMODSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  const float rawProgress =
      (section->pageCount > 0) ? (static_cast<float>(section->currentPage) / section->pageCount) : 0.0f;
  const bool bookmarked = BOOKMARKS.hasBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), rawProgress,
                                                       section->pageCount > 0 ? section->pageCount : 1);
  char timeLeftLabel[24] = {};
  const char* timeLeft = formatTimeLeftLabel(timeLeftLabel, sizeof(timeLeftLabel)) ? timeLeftLabel : nullptr;

  // A whole-book page count isn't something this reader actually has -
  // computing one for real would mean paginating every chapter up front,
  // which is exactly what the lazy per-chapter loading (FB2 and otherwise)
  // is built to avoid. This is an estimate instead: how many pages-per-byte
  // the *current* chapter works out to, scaled up to the book's total byte
  // size. It's only as good as that one chapter is representative of the
  // whole book (a title page or a chapter with unusually dense/sparse
  // formatting will skew it), but it's cheap - reusing byte totals the
  // reader already tracks for the % progress bar - and self-corrects as
  // the reader moves through chapters with different densities.
  int bookWideCurrentPage = -1;
  int bookWideTotalPages = -1;
  if (SETTINGS.statusBarChapterPageCount == 2 && section->pageCount > 0) {
    const size_t cumulativeThroughHere = epub->getCumulativeSpineItemSize(currentSpineIndex);
    const size_t cumulativeThroughPrevious =
        currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
    const size_t currentChapterBytes =
        cumulativeThroughHere > cumulativeThroughPrevious ? cumulativeThroughHere - cumulativeThroughPrevious : 0;
    const size_t totalBookBytes = epub->getBookSize();
    if (currentChapterBytes > 0 && totalBookBytes > 0) {
      const float avgPagesPerByte = section->pageCount / static_cast<float>(currentChapterBytes);
      const float estimatedTotalPages = avgPagesPerByte * static_cast<float>(totalBookBytes);
      bookWideTotalPages = std::max(1, static_cast<int>(std::lround(estimatedTotalPages)));
      bookWideCurrentPage =
          std::max(1, static_cast<int>(std::lround(bookProgress / 100.0f * estimatedTotalPages)));
      bookWideCurrentPage = std::min(bookWideCurrentPage, bookWideTotalPages);
    }
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, bookmarked, timeLeft,
                    ReaderUtils::readerDarkModeEnabled(), bookWideCurrentPage, bookWideTotalPages);
  // Bottom placement (the other half of this toggle) is drawn as part of
  // drawStatusBar() above instead, folded into its left cluster alongside
  // battery/time-left - see that function's own comment for why. Top is
  // still this separate call, same as before this setting existed.
  if (!SETTINGS.readerClockAtBottom) {
    GUI.drawTopStatusBarClock(renderer, UITheme::getInstance().getMetrics().topPadding, nullptr, true, 0,
                              ReaderUtils::readerDarkModeEnabled());
  }
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  pageLoadRetryCount = 0;
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    clearLogicalPageCarry();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  armReadingPaceWarmup(savePosition ? "href_navigation" : "href_restore");
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  pageLoadRetryCount = 0;
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  armReadingPaceWarmup("saved_position_restore");
  requestUpdate();
}
bool EpubReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  auto epub = std::make_shared<Epub>(filePath, "/.inkmod");
  // Load CSS when embeddedStyle is enabled, as createSectionFile may need it to rebuild the cache.
  if (!epub->load(true, SETTINGS.embeddedStyle == 0)) {
    LOG_DBG("SLP", "EPUB: failed to load %s", filePath.c_str());
    return false;
  }

  epub->setupCacheDir();

  // Load saved spine index and page number
  int spineIndex = 0, pageNumber = 0;
  EpubReaderUtils::Progress progress;
  if (EpubReaderUtils::loadProgress(*epub, progress, "SLP")) {
    spineIndex = progress.spineIndex;
    pageNumber = progress.pageNumber;
  }
  if (spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) spineIndex = 0;

  // Apply the reader orientation so margins match what the reader would produce
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  const ReaderViewportLayout layout = computeReaderViewportLayout(renderer, /*automaticPageTurnActive=*/false);
  const uint16_t viewportWidth = layout.viewportWidth;
  const uint16_t viewportHeight = layout.viewportHeight;

  // Sleep preparation is intentionally cache-only. Rebuilding a missing cache
  // here used to run a complete invisible chapter layout while the device was
  // trying to sleep, wasting battery and making large books look hung.
  const int readerFontId = SETTINGS.getReaderFontId();
  const int fallbackFontId = SETTINGS.getBuiltInReaderFontId();
  const bool canUseFallbackFont = renderer.isSdCardFont(readerFontId) && fallbackFontId != readerFontId;
  int renderFontId = readerFontId;
  auto section = makeUniqueNoThrow<Section>(epub, spineIndex, renderer);
  if (!section) {
    LOG_ERR("SLP", "EPUB: failed to allocate section for spine %d", spineIndex);
    return false;
  }
  bool loadedSection = section->loadSectionFile(
      readerFontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
      SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
      SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled);
  if (!loadedSection && canUseFallbackFont) {
    section = makeUniqueNoThrow<Section>(epub, spineIndex, renderer, FALLBACK_FONT_SECTION_CACHE_SUFFIX);
    if (!section) {
      LOG_ERR("SLP", "EPUB: failed to allocate fallback section for spine %d", spineIndex);
      return false;
    }
    loadedSection =
        section->loadSectionFile(fallbackFontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                 SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth,
                                 viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                 SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled);
    if (loadedSection) {
      renderFontId = fallbackFontId;
      LOG_DBG("SLP", "EPUB: using fallback built-in font cache for spine %d", spineIndex);
    }
  }
  if (!loadedSection) {
    const auto safe = sectionMemoryConfig(reader::ReaderMemoryMode::Safe, readerFontId, fallbackFontId);
    section = makeUniqueNoThrow<Section>(epub, spineIndex, renderer, safe.suffix);
    loadedSection = section && section->loadSectionFile(
                                  safe.fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                  SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, safe.hyphenationEnabled, safe.embeddedStyle, safe.imageRendering,
                                  safe.bionicReadingEnabled, safe.guideReadingEnabled);
    if (loadedSection) renderFontId = safe.fontId;
  }
  if (!loadedSection) {
    const auto survival = sectionMemoryConfig(reader::ReaderMemoryMode::Survival, readerFontId, fallbackFontId);
    section = makeUniqueNoThrow<Section>(epub, spineIndex, renderer, survival.suffix);
    loadedSection = section && section->loadSectionFile(
                                  survival.fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                  SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, survival.hyphenationEnabled, survival.embeddedStyle,
                                  survival.imageRendering, survival.bionicReadingEnabled,
                                  survival.guideReadingEnabled);
    if (loadedSection) renderFontId = survival.fontId;
  }
  if (!loadedSection) {
    LOG_DBG("SLP", "EPUB: no ready section cache for spine %d; skipping invisible rebuild", spineIndex);
    return false;
  }

  if (pageNumber < 0 || pageNumber >= section->pageCount) pageNumber = 0;
  section->currentPage = pageNumber;

  auto page = section->loadPageFromSectionFile();
  if (!page) {
    LOG_DBG("SLP", "EPUB: failed to load page %d", pageNumber);
    return false;
  }

  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  page->render(renderer, renderFontId, layout.marginLeft, layout.marginTop, ReaderUtils::readerForegroundBlack());
  drawPublisherPageMarkers(renderer, *page, layout.marginTop, renderer.getScreenHeight() - layout.marginBottom,
                           ReaderUtils::readerForegroundBlack());
  // No displayBuffer call; caller (SleepActivity) handles that after compositing the overlay.
  return true;
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}
