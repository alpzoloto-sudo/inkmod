#include "DashboardTheme.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

namespace {
constexpr int kCoverCornerRadius = 8;
constexpr int kContentInset = 20;
constexpr int kCoverStatsGap = 15;
constexpr int kStatsRowCount = 6;
constexpr int kStatsValueLabelGap = 1;
// CrossInk's real cover is 296x444 (2:3-ish). Kept as the target aspect ratio; the
// actual pixel size is solved from the row height inkMOD has budgeted (see
// DashboardMetrics::values.homeCoverTileHeight) so it never overflows into the menu
// below, the way a hardcoded 444px-tall cover would on inkMOD's generic home layout.
constexpr float kCoverAspect = 296.0f / 444.0f;  // width / height

// -- Cover geometry & loading (ported from CrossInk's DashboardTheme.cpp) --------

Rect coverRectForRow(const Rect& rect, const int statsW) {
  const int maxCoverW = rect.width - kContentInset * 2 - statsW - kCoverStatsGap;
  int coverH = rect.height;
  int coverW = std::min(maxCoverW, static_cast<int>(coverH * kCoverAspect));
  coverW = std::max(1, coverW);
  coverH = std::min(rect.height, static_cast<int>(coverW / kCoverAspect));
  return Rect{rect.x + kContentInset, rect.y, coverW, coverH};
}

Rect fittedBitmapRect(const Bitmap& bitmap, const Rect& target) {
  if (bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 || target.width <= 0 || target.height <= 0) {
    return target;
  }
  const float widthScale = static_cast<float>(target.width) / static_cast<float>(bitmap.getWidth());
  const float heightScale = static_cast<float>(target.height) / static_cast<float>(bitmap.getHeight());
  const float scale = std::min(1.0f, std::min(widthScale, heightScale));
  const int drawnW = std::min(target.width, std::max(1, static_cast<int>(std::ceil(bitmap.getWidth() * scale))));
  const int drawnH = std::min(target.height, std::max(1, static_cast<int>(std::ceil(bitmap.getHeight() * scale))));
  return Rect{target.x + (target.width - drawnW) / 2, target.y + (target.height - drawnH) / 2, drawnW, drawnH};
}

// getAdaptiveThumbBmpPath() only computes a path - it does not create the file.
// Actually generating the thumbnail is a separate call (generateAdaptiveThumbBmp),
// which upstream CrossInk runs ahead of time during a library-scan pass. inkMOD's
// Dashboard theme is new, so nothing has generated a thumb at this exact size yet -
// do it here, once, the first time this book is shown. After that the file exists
// on the SD card and every later render just loads it straight away. inkMOD's
// generateAdaptiveThumbBmp() also only takes (width, height) - no renderer/font-id
// params like CrossInk's newer version - so we still fall back to our own
// drawMissingBookCover() below if the whole cover is missing.
std::string coverPathForRect(const RecentBook& book, const Rect& imageRect) {
  if (book.coverBmpPath.empty()) {
    return {};
  }
  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.inkmod");
    const std::string adaptivePath = epub.getAdaptiveThumbBmpPath(imageRect.width, imageRect.height);
    if (Storage.exists(adaptivePath.c_str())) {
      return adaptivePath;
    }
    if (epub.load(true, true) && epub.generateAdaptiveThumbBmp(imageRect.width, imageRect.height) &&
        Storage.exists(adaptivePath.c_str())) {
      return adaptivePath;
    }
  }
  return UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.width, imageRect.height);
}

void drawMissingBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                           Color::White);
  renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);

  const int iconSize = std::min(32, std::min(coverRect.width, coverRect.height) - 20);
  if (iconSize > 8) {
    renderer.drawIcon(CoverIcon, coverRect.x + (coverRect.width - iconSize) / 2, coverRect.y + 14, iconSize,
                      iconSize);
  }

  constexpr int textPadding = 12;
  const int textW = std::max(1, coverRect.width - textPadding * 2);
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  auto titleLines = renderer.wrappedText(UI_10_FONT_ID, title, textW, 5, EpdFontFamily::BOLD);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int textTop = coverRect.y + 14 + (iconSize > 8 ? iconSize + 10 : 0);
  const int availableH = std::max(0, coverRect.y + coverRect.height - textTop);
  int textY = textTop + std::max(0, (availableH - static_cast<int>(titleLines.size()) * lineH) / 2);
  for (const auto& line : titleLines) {
    const int lineW = renderer.getTextWidth(UI_10_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, coverRect.x + (coverRect.width - lineW) / 2, textY, line.c_str(), true,
                      EpdFontFamily::BOLD);
    textY += lineH;
  }
}

void drawBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  bool hasCover = false;
  const std::string coverBmpPath = coverPathForRect(book, coverRect);
  if (!coverBmpPath.empty() && Storage.exists(coverBmpPath.c_str())) {
    FsFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const Rect bitmapRect = fittedBitmapRect(bitmap, coverRect);
        renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                                 Color::White);
        renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height);
        renderer.maskRoundedRectOutsideCorners(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height,
                                               kCoverCornerRadius, Color::White);
        renderer.drawRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, 1,
                                 kCoverCornerRadius, true);
        hasCover = true;
      }
      file.close();
    }
  }

  if (!hasCover) {
    drawMissingBookCover(renderer, coverRect, book);
  }
}

// -- Stats column (ported from CrossInk's drawDashboardStats, X4 branch) ---------

void formatCompactDuration(const uint32_t seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t minutes = (seconds + 30u) / 60u;
  if (minutes < 60) {
    snprintf(buf, len, "%lu %s", static_cast<unsigned long>(minutes), tr(STR_UNIT_MIN_SHORT));
    return;
  }
  const uint32_t hours = minutes / 60u;
  const uint32_t remainder = minutes % 60u;
  if (remainder == 0) {
    snprintf(buf, len, "%lu%s", static_cast<unsigned long>(hours), tr(STR_UNIT_HOUR_SHORT));
  } else {
    snprintf(buf, len, "%lu%s %lu %s", static_cast<unsigned long>(hours), tr(STR_UNIT_HOUR_SHORT),
             static_cast<unsigned long>(remainder), tr(STR_UNIT_MIN_SHORT));
  }
}

// inkMOD's BookReadingStats has no estimatedTimeLeftSeconds field (unlike CrossInk's),
// so this only does the fallback estimate: total time so far, scaled by how much of
// the book is left. Same formula CrossInk falls back to when it has no better number.
bool estimatedTimeLeft(const uint32_t totalReadingSeconds, const float progressPercent, uint32_t& seconds) {
  seconds = 0;
  if (progressPercent <= 0.0f || progressPercent >= 100.0f || totalReadingSeconds < 120) {
    return false;
  }
  const float progress = progressPercent / 100.0f;
  const float estimate = (static_cast<float>(totalReadingSeconds) * (1.0f - progress)) / progress;
  if (estimate <= 0.0f) {
    return false;
  }
  seconds = static_cast<uint32_t>(estimate + 0.5f);
  return seconds > 0;
}

float pagesPerMinute(const uint32_t totalPagesTurned, const uint32_t totalReadingSeconds) {
  if (totalReadingSeconds <= 60) {
    return 0.0f;
  }
  return static_cast<float>(totalPagesTurned) * 60.0f / static_cast<float>(totalReadingSeconds);
}

int statsBlockHeight(const GfxRenderer& renderer) {
  return renderer.getLineHeight(UI_12_FONT_ID) + kStatsValueLabelGap + renderer.getLineHeight(SMALL_FONT_ID);
}

// Evenly distributes kStatsRowCount blocks across rect.height, same spacing approach
// CrossInk uses so rows land evenly instead of bunching at the top with a big gap
// at the bottom.
int statsBlockTop(const Rect& rect, const int index, const int blockH, const int rowCount) {
  const int remainingH = std::max(0, rect.height - blockH * rowCount);
  const int gapCount = rowCount - 1;
  const int gap = gapCount > 0 ? remainingH / gapCount : 0;
  const int remainder = gapCount > 0 ? remainingH % gapCount : 0;
  return rect.y + index * (blockH + gap) + std::min(index, remainder);
}

void drawStatsRow(const GfxRenderer& renderer, const int rightX, const int y, const char* value, const char* label) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, rightX - valueW, y, value, true, EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(SMALL_FONT_ID, label);
  renderer.drawText(SMALL_FONT_ID, rightX - labelW, y + valueLineH + kStatsValueLabelGap, label);
}
void drawSelectionBorder(const GfxRenderer& renderer, const Rect& rect) {
  renderer.drawRect(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2);
  renderer.drawRect(rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4);
}
}  // namespace

void DashboardTheme::drawCoverPanel(const GfxRenderer& renderer, Rect rect, const RecentBook* book,
                                    bool hasBook) const {
  if (!hasBook || book == nullptr) {
    renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, kCoverCornerRadius, true);
    const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    const int y = rect.y + (rect.height - lineH - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    const char* line1 = tr(STR_NO_OPEN_BOOK);
    const char* line2 = tr(STR_START_READING);
    const int line1W = renderer.getTextWidth(UI_12_FONT_ID, line1);
    const int line2W = renderer.getTextWidth(UI_10_FONT_ID, line2);
    renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - line1W) / 2, y, line1);
    renderer.drawText(UI_10_FONT_ID, rect.x + (rect.width - line2W) / 2, y + lineH, line2);
    return;
  }
  drawBookCover(renderer, rect, *book);
}

void DashboardTheme::drawStatsColumn(const GfxRenderer& renderer, Rect rect, const BookReadingStats* stats,
                                     const GlobalReadingStats* globalStats, float progressPercent) const {
  (void)globalStats;  // Reserved: the fuller CrossInk footer (streak / reader type) needs
                      // more i18n strings than inkMOD currently has; not ported in this pass.
  const int rightX = rect.x + rect.width;
  const int blockH = statsBlockHeight(renderer);
  const BookReadingStats emptyStats{};
  const BookReadingStats& bookStats = stats != nullptr ? *stats : emptyStats;

  char value[40];
  uint32_t estimatedSeconds = 0;
  const bool hasEstimate = estimatedTimeLeft(bookStats.totalReadingSeconds, progressPercent, estimatedSeconds);

  int rowIndex = 0;
  int rowY = statsBlockTop(rect, rowIndex, blockH, kStatsRowCount);
  BookReadingStats::formatDuration(bookStats.totalReadingSeconds, value, sizeof(value));
  drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_TIME_LBL));

  rowY = statsBlockTop(rect, ++rowIndex, blockH, kStatsRowCount);
  if (hasEstimate && !bookStats.isCompleted) {
    formatCompactDuration(estimatedSeconds, value, sizeof(value));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawStatsRow(renderer, rightX, rowY, value, tr(STR_TIME_LEFT));

  rowY = statsBlockTop(rect, ++rowIndex, blockH, kStatsRowCount);
  if (progressPercent >= 0.0f) {
    snprintf(value, sizeof(value), "%d%%", static_cast<int>(progressPercent + 0.5f));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_PROGRESS_LBL));

  rowY = statsBlockTop(rect, ++rowIndex, blockH, kStatsRowCount);
  snprintf(value, sizeof(value), "%.1f", pagesPerMinute(bookStats.totalPagesTurned, bookStats.totalReadingSeconds));
  drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_PAGES_PER_MIN));

  rowY = statsBlockTop(rect, ++rowIndex, blockH, kStatsRowCount);
  snprintf(value, sizeof(value), "%u", static_cast<unsigned>(bookStats.sessionCount));
  drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_SESSIONS_LBL));

  rowY = statsBlockTop(rect, ++rowIndex, blockH, kStatsRowCount);
  const uint32_t avgSeconds =
      bookStats.sessionCount > 0 ? bookStats.totalReadingSeconds / bookStats.sessionCount : 0;
  BookReadingStats::formatDuration(avgSeconds, value, sizeof(value));
  drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_AVG_SESSION_LBL));
}

void DashboardTheme::drawDashboardRow(const GfxRenderer& renderer, Rect rect, const RecentBook* book, bool hasBook,
                                      const BookReadingStats* stats, const GlobalReadingStats* globalStats,
                                      float progressPercent) const {
  if (!hasBook || book == nullptr) {
    drawCoverPanel(renderer, rect, nullptr, false);
    return;
  }

  constexpr int kStatsW = 130;
  const Rect coverRect = coverRectForRow(rect, kStatsW);
  const int statsX = coverRect.x + coverRect.width + kCoverStatsGap;
  const int statsRight = rect.x + rect.width - kContentInset;
  const int statsW = std::max(1, statsRight - statsX);

  drawCoverPanel(renderer, coverRect, book, true);
  drawStatsColumn(renderer, Rect{statsX, coverRect.y, statsW, coverRect.height}, stats, globalStats,
                  progressPercent);
}

void DashboardTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                         int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                         bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                         const BookReadingStats* stats, float progressPercent) const {
  const bool hasBook = !recentBooks.empty();
  const bool bookSelected = hasBook && selectorIndex == 0;

  if (!hasBook) {
    drawDashboardRow(renderer, rect, nullptr, false, nullptr, nullptr, -1.0f);
    coverRendered = false;
    coverBufferStored = false;
    return;
  }

  if (!coverRendered) {
    drawDashboardRow(renderer, rect, &recentBooks[0], true, stats, nullptr, progressPercent);
    // Store the buffer BEFORE drawing the selection border, so the cached image never
    // has a stale border baked into it - the border is redrawn fresh on top every time
    // (here, and again below whenever the caller restores this buffer), matching
    // BaseTheme::drawRecentBookCover's exact contract for selectorIndex/bufferRestored.
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
    if (bookSelected) {
      drawSelectionBorder(renderer, rect);
    }
    return;
  }

  if (bufferRestored && bookSelected) {
    drawSelectionBorder(renderer, rect);
  }
}

void DashboardTheme::drawDashboardSleepScreen(const GfxRenderer& renderer, const RecentBook& book,
                                              const BookReadingStats* stats, const GlobalReadingStats* globalStats,
                                              float progressPercent) const {
  renderer.clearScreen();
  const Rect rect{0, DashboardMetrics::values.homeTopPadding, renderer.getScreenWidth(),
                  DashboardMetrics::values.homeCoverTileHeight};
  drawDashboardRow(renderer, rect, &book, true, stats, globalStats, progressPercent);
}
