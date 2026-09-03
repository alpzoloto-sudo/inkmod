#include "MinimalTheme.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/UITheme.h"
#include "components/icons/afternoon.h"
#include "components/icons/book24.h"
#include "components/icons/cover.h"
#include "components/icons/evening.h"
#include "components/icons/morning.h"
#include "components/icons/night.h"
#include "components/icons/streak.h"
#include "fontIds.h"

namespace {
// ============================================================================
// КОНСТАНТЫ ДИЗАЙНА
// ============================================================================

// Цветовая схема (E-Ink дружественная)
constexpr Color COLOR_PRIMARY = Color::Black;
constexpr Color COLOR_SECONDARY = Color::DarkGray;
constexpr Color COLOR_BACKGROUND = Color::White;
constexpr Color COLOR_HIGHLIGHT = Color::LightGray;

// Размеры и отступы
constexpr int kCoverCornerRadius = 12;
constexpr int kProgressBarHeight = 8;
constexpr int kButtonCornerRadius = 8;
constexpr int kFileBrowserIconSize = 28;
constexpr int kFileBrowserRowVerticalPadding = 10;
constexpr int kFileBrowserTextGap = 12;
constexpr int kFileBrowserValueMaxWidth = 90;
constexpr int kMenuPanelWidth = 400;
constexpr int kMenuRowHeight = 68;
constexpr int kMenuPanelTop = 200;
constexpr int kMenuPanelRadius = 6;
constexpr int kMenuSelectionTriangleWidth = 16;
constexpr int kMenuSelectionTriangleHeight = 22;
constexpr int kMenuSelectionTriangleInset = 32;
constexpr int kCoverTopOffset = 0;
constexpr int kProgressBlockGap = 12;
constexpr int kProgressBarGap = 6;
constexpr int kProgressLabelGap = 8;
constexpr int kStatsFooterReaderIconSize = 28;
constexpr int kStatsFooterStreakIconSize = 28;
constexpr int kStatsFooterSideInset = 40;
constexpr int kQuotePadding = 24;
constexpr int kHeaderTitleInsetX = 16;
constexpr int kHeaderLineHeight = 4;

// Анимационные константы
constexpr uint32_t kQuoteRotationInterval = 5000;

int homeButtonHintSelection = -1;

// ============================================================================
// СТРУКТУРА ДЛЯ КРАСИВЫХ ЦИТАТ
// ============================================================================

struct MinimalQuote {
  const char* text;
  const char* author;
  const char* source;
};

constexpr MinimalQuote kQuotes[] = {
    {"\"A book is a dream you hold in your hands.\"", "Neil Gaiman", "The Sandman"},
    {"\"I have always imagined that Paradise will be a kind of library.\"", "Jorge Luis Borges", "Poems"},
    {"\"A reader lives a thousand lives before he dies. The man who never reads lives only one.\"",
     "George R.R. Martin", "A Dance with Dragons"},
    {"\"So many books, so little time.\"", "Frank Zappa", "The Real Frank Zappa Book"},
    {"\"If you only read the books that everyone else is reading, you can only think what everyone else is thinking.\"",
     "Haruki Murakami", "Norwegian Wood"},
    {"\"Books are a uniquely portable magic.\"", "Stephen King", "On Writing"},
    {"\"The person who deserves most pity is a lonesome one on a rainy day who doesn't know how to read.\"",
     "Benjamin Franklin", "Autobiography"},
    {"\"Reading is to the mind what exercise is to the body.\"", "Joseph Addison", "The Tatler"},
};

// ============================================================================
// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ДЛЯ СТАТИСТИКИ
// ============================================================================

bool dominantReaderTypeBucket(const GlobalReadingStats& globalStats, ReadingTimeBucket& bucketOut) {
  const auto& values = globalStats.timeOfDaySeconds;
  const uint32_t totalSeconds = std::accumulate(values.begin(), values.end(), 0u);
  if (totalSeconds == 0) {
    return false;
  }

  const size_t dominantIndex =
      static_cast<size_t>(std::distance(values.begin(), std::max_element(values.begin(), values.end())));
  bucketOut = static_cast<ReadingTimeBucket>(dominantIndex);
  return true;
}

const char* readerTypeLabel(const GlobalReadingStats& globalStats) {
  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(globalStats, bucket)) {
    return tr(STR_STATS_NEW_READER);
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return tr(STR_STATS_MORNING_READER);
    case ReadingTimeBucket::Afternoon:
      return tr(STR_STATS_AFTERNOON_READER);
    case ReadingTimeBucket::Evening:
      return tr(STR_STATS_EVENING_READER);
    case ReadingTimeBucket::Night:
    default:
      return tr(STR_STATS_NIGHT_READER);
  }
}

const uint8_t* readerTypeIcon(const GlobalReadingStats& globalStats) {
  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(globalStats, bucket)) {
    return Book24Icon;
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return MorningReaderIcon;
    case ReadingTimeBucket::Afternoon:
      return AfternoonReaderIcon;
    case ReadingTimeBucket::Evening:
      return EveningReaderIcon;
    case ReadingTimeBucket::Night:
    default:
      return NightReaderIcon;
  }
}

void formatStreakStat(const GlobalReadingStats& globalStats, char* buf, const size_t len) {
  if (len == 0) {
    return;
  }

  ReadingStatsDateTime today;
  const uint16_t streak =
      getCurrentLocalReadingStatsDateTime(today) ? globalStats.currentReadingStreak(&today.date) : 0;
  if (streak == 0) {
    snprintf(buf, len, "%s", tr(STR_STATS_NO_STREAK));
    return;
  }

  snprintf(buf, len, "%u %s", static_cast<unsigned>(streak), tr(STR_STATS_DAY_STREAK_FORMAT));
}

// ============================================================================
// ФУНКЦИИ ОТРИСОВКИ СТАТИСТИКИ
// ============================================================================

// Прототип функции, которая определена ниже
Rect coverImageRectForFrame(const Rect& coverRect);

void drawCenteredStatsRow(const GfxRenderer& renderer, const uint8_t* icon, const int iconSize, const char* label,
                          const int regionTop, const int regionBottom, bool inverted = false) {
  const int screenWidth = renderer.getScreenWidth();
  const int regionHeight = regionBottom - regionTop;
  if (regionHeight <= 0 || label == nullptr) {
    return;
  }

  const int labelLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowHeight = std::max(labelLineHeight, iconSize);
  const int topY = regionTop + std::max(0, regionHeight - rowHeight) / 2;
  const int availableWidth = std::max(1, screenWidth - kStatsFooterSideInset * 2);
  const int iconTextGap = 12;
  const std::string text = renderer.truncatedText(UI_10_FONT_ID, label, availableWidth - iconSize - iconTextGap);
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, text.c_str());
  const int blockWidth = iconSize + iconTextGap + textWidth;
  const int iconX = (screenWidth - blockWidth) / 2;
  const int iconY = topY + (rowHeight - iconSize) / 2;
  const int textX = iconX + iconSize + iconTextGap;
  const int textY = topY + (rowHeight - labelLineHeight) / 2;

  if (inverted) {
    renderer.drawIconInverted(icon, iconX, iconY, iconSize, iconSize);
    renderer.drawText(UI_10_FONT_ID, textX, textY, text.c_str(), false);
  } else {
    renderer.drawIcon(icon, iconX, iconY, iconSize, iconSize);
    renderer.drawText(UI_10_FONT_ID, textX, textY, text.c_str(), true);
  }
}

struct ProgressBlockLayout {
  int fontId = UI_10_FONT_ID;
  int lineHeight = 0;
  bool compactHeaderRow = false;
  int compactHeaderY = 0;
  int durationY = 0;
  int barY = 0;
  int labelY = 0;
};

ProgressBlockLayout computeProgressBlockLayout(const GfxRenderer& renderer, const Rect& coverRect,
                                               const char* durationLabel, const char* progressLabel) {
  ProgressBlockLayout layout;
  layout.fontId = UI_10_FONT_ID;
  layout.lineHeight = renderer.getLineHeight(layout.fontId);

  const bool hasDuration = durationLabel != nullptr && durationLabel[0] != '\0';
  const bool hasProgress = progressLabel != nullptr && progressLabel[0] != '\0';
  const int compactGap = gpio.deviceIsX3() ? 8 : 12;
  const int durationWidth = hasDuration ? renderer.getTextWidth(layout.fontId, durationLabel) : 0;
  const int progressWidth = hasProgress ? renderer.getTextWidth(layout.fontId, progressLabel) : 0;
  const bool largeSystemFont = layout.lineHeight >= 18;

  layout.compactHeaderRow = hasDuration && hasProgress &&
                            (largeSystemFont || gpio.deviceIsX3()) &&
                            (durationWidth + compactGap + progressWidth <= coverRect.width);

  if (layout.compactHeaderRow) {
    layout.compactHeaderY = coverRect.y + coverRect.height + kProgressBlockGap;
    layout.durationY = layout.compactHeaderY;
    layout.barY = layout.compactHeaderY + layout.lineHeight + std::max(3, kProgressBarGap - 2);
    layout.labelY = layout.compactHeaderY;
  } else {
    layout.durationY = coverRect.y + coverRect.height + kProgressBlockGap;
    layout.barY = layout.durationY + layout.lineHeight + kProgressBarGap;
    layout.labelY = layout.barY + kProgressBarHeight + kProgressLabelGap;
  }

  return layout;
}


int progressLabelBottomY(const GfxRenderer& renderer, const Rect& coverRect, const float progressPercent) {
  if (progressPercent < 0.0f) {
    return coverRect.y + coverRect.height;
  }

  char progressLabel[12];
  snprintf(progressLabel, sizeof(progressLabel), "%d%%",
           std::clamp(static_cast<int>(progressPercent + 0.5f), 0, 100));
  ProgressBlockLayout layout = computeProgressBlockLayout(renderer, coverRect, nullptr, progressLabel);
  return layout.compactHeaderRow ? (layout.barY + kProgressBarHeight) : (layout.labelY + layout.lineHeight);
}

void drawStatsOverlay(const GfxRenderer& renderer, const GlobalReadingStats& globalStats, const Rect& coverRect,
                      const float progressPercent) {
  if (!gpio.deviceIsX3()) {
    return;
  }

  char streakBuf[64];
  formatStreakStat(globalStats, streakBuf, sizeof(streakBuf));
  const char* readerLabel = readerTypeLabel(globalStats);

  const int readerRegionTop = 0;
  const int readerRegionBottom = coverImageRectForFrame(coverRect).y;
  drawCenteredStatsRow(renderer, readerTypeIcon(globalStats), kStatsFooterReaderIconSize, readerLabel, readerRegionTop,
                       readerRegionBottom, false);

  const int streakRegionTop = progressLabelBottomY(renderer, coverRect, progressPercent);
  const int streakRegionBottom = renderer.getScreenHeight();
  drawCenteredStatsRow(renderer, StreakIcon, kStatsFooterStreakIconSize, streakBuf, streakRegionTop,
                       streakRegionBottom, false);
}

// ============================================================================
// ФУНКЦИИ ДЛЯ РАБОТЫ С ОБЛОЖКАМИ
// ============================================================================

Rect coverRectForScreen(const GfxRenderer& renderer, const Rect& rect) {
  const int coverH = MinimalMetrics::values.homeCoverHeight;
  const int coverW = MinimalMetrics::homeCoverWidth;
  const int coverX = (renderer.getScreenWidth() - coverW) / 2;
  const int coverY = rect.y + kCoverTopOffset;
  return Rect{coverX, coverY, coverW, coverH};
}

Rect coverImageRectForFrame(const Rect& coverRect) {
  const int imageW = std::min(coverRect.width, MinimalMetrics::homeCoverImageWidth);
  const int imageH = std::min(coverRect.height, MinimalMetrics::homeCoverImageHeight);
  return Rect{coverRect.x + (coverRect.width - imageW) / 2, coverRect.y + (coverRect.height - imageH) / 2, imageW,
              imageH};
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

std::string coverPathForImageRect(const RecentBook& book, const Rect& imageRect) {
  if (book.coverBmpPath.empty()) {
    return {};
  }

  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.inkmod");
    const std::string adaptive = epub.getAdaptiveThumbBmpPath(imageRect.width, imageRect.height);
    if (!adaptive.empty() && Storage.exists(adaptive.c_str())) {
      return adaptive;
    }

    const std::string readyThumb = epub.getThumbBmpPath();
    if (!readyThumb.empty() && Storage.exists(readyThumb.c_str())) {
      return readyThumb;
    }

    if (Storage.exists(book.coverBmpPath.c_str())) {
      return book.coverBmpPath;
    }

    std::string themed = UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.width, imageRect.height);
    if (!themed.empty() && Storage.exists(themed.c_str())) {
      return themed;
    }

    themed = UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.height);
    if (!themed.empty() && Storage.exists(themed.c_str())) {
      return themed;
    }

    return {};
  }

  std::string coverBmpPath = UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.width, imageRect.height);
  if (coverBmpPath.empty() || !Storage.exists(coverBmpPath.c_str())) {
    coverBmpPath = UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.height);
  }
  if ((coverBmpPath.empty() || !Storage.exists(coverBmpPath.c_str())) && Storage.exists(book.coverBmpPath.c_str())) {
    coverBmpPath = book.coverBmpPath;
  }
  return coverBmpPath;
}

uint8_t selectedQuoteIndex() {
  static bool initialized = false;
  static uint8_t index = 0;
  static uint32_t lastUpdate = 0;
  
  if (!initialized) {
    index = static_cast<uint8_t>((millis() / 137u) % (sizeof(kQuotes) / sizeof(kQuotes[0])));
    lastUpdate = millis();
    initialized = true;
  }
  
  const uint32_t now = millis();
  if (now - lastUpdate > kQuoteRotationInterval) {
    index = (index + 1) % (sizeof(kQuotes) / sizeof(kQuotes[0]));
    lastUpdate = now;
  }
  
  return index;
}

int centeredRowY(const int rowY, const int rowHeight, const int contentHeight) {
  return rowY + std::max(0, rowHeight - contentHeight) / 2;
}

// ============================================================================
// ОТРИСОВКА ПРОГРЕССА
// ============================================================================

void drawProgressBlock(const GfxRenderer& renderer, const Rect& coverRect, const BookReadingStats* stats,
                       float progressPercent, const bool inverted) {
  if ((stats == nullptr || stats->totalReadingSeconds == 0) && progressPercent < 0.0f) {
    return;
  }

  const int barW = coverRect.width;
  const int barX = coverRect.x;
  const bool textBlack = !inverted;

  char duration[32] = {0};
  if (stats != nullptr && stats->totalReadingSeconds > 0) {
    BookReadingStats::formatDuration(stats->totalReadingSeconds, duration, sizeof(duration));
  }

  char progressLabel[12] = {0};
  if (progressPercent >= 0.0f) {
    const int progress = std::clamp(static_cast<int>(progressPercent + 0.5f), 0, 100);
    snprintf(progressLabel, sizeof(progressLabel), "%d%%", progress);
  }

  ProgressBlockLayout layout = computeProgressBlockLayout(renderer, coverRect,
                                                          duration[0] ? duration : nullptr,
                                                          progressLabel[0] ? progressLabel : nullptr);

  if (duration[0]) {
    if (layout.compactHeaderRow) {
      renderer.drawText(layout.fontId, barX, layout.durationY, duration, textBlack);
    } else {
      const std::string safeDuration = renderer.truncatedText(layout.fontId, duration, barW);
      renderer.drawText(layout.fontId, barX, layout.durationY, safeDuration.c_str(), textBlack);
    }
  }

  if (progressPercent < 0.0f) {
    return;
  }

  const int progress = std::clamp(static_cast<int>(progressPercent + 0.5f), 0, 100);
  const int fillW = (barW * progress) / 100;

  if (inverted) {
    renderer.drawRect(barX, layout.barY, barW, kProgressBarHeight, false);
    if (fillW > 0) {
      renderer.fillRect(barX, layout.barY, fillW, kProgressBarHeight, false);
    }
  } else {
    renderer.fillRectDither(barX, layout.barY, barW, kProgressBarHeight, Color::LightGray);
    if (fillW > 0) {
      renderer.fillRectDither(barX, layout.barY, fillW, kProgressBarHeight, Color::DarkGray);
    }
  }

  const int labelW = renderer.getTextWidth(layout.fontId, progressLabel);
  const int labelX = barX + std::max(0, barW - labelW);
  if (layout.compactHeaderRow) {
    renderer.drawText(layout.fontId, labelX, layout.labelY, progressLabel, textBlack);
  } else {
    renderer.drawText(layout.fontId, labelX, layout.labelY, progressLabel, textBlack);
  }
}

// ============================================================================
// ОТРИСОВКА ОБЛОЖКИ КНИГИ
// ============================================================================

void drawMissingBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  constexpr int commonBookCoverHeightRatio = 3;
  constexpr int commonBookCoverWidthRatio = 2;
  const int placeholderHeight =
      std::min(coverRect.height, (coverRect.width * commonBookCoverHeightRatio) / commonBookCoverWidthRatio);
  const Rect placeholderRect{coverRect.x, coverRect.y + (coverRect.height - placeholderHeight) / 2, coverRect.width,
                             placeholderHeight};

  // Фон плейсхолдера
  renderer.fillRoundedRect(placeholderRect.x, placeholderRect.y, placeholderRect.width, placeholderRect.height,
                           kCoverCornerRadius, Color::White);
  renderer.drawRoundedRect(placeholderRect.x, placeholderRect.y, placeholderRect.width, placeholderRect.height, 1,
                           kCoverCornerRadius, true);

  // Декоративная линия (имитация корешка)
  const int dividerY = placeholderRect.y + placeholderRect.height / 3;
  renderer.drawLine(placeholderRect.x, dividerY, placeholderRect.x + placeholderRect.width - 1, dividerY, true);

  // Иконка книги
  constexpr int iconSize = 40;
  renderer.drawIcon(CoverIcon, placeholderRect.x + (placeholderRect.width - iconSize) / 2,
                    placeholderRect.y + (placeholderRect.height / 3 - iconSize) / 2, iconSize, iconSize);

  // Текст с заголовком и автором
  constexpr int textPadding = 20;
  constexpr int textVerticalPadding = 24;
  constexpr int titleAuthorGap = 30;
  const int textW = placeholderRect.width - textPadding * 2;
  const std::string& titleText = book.title.empty() ? book.path : book.title;
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int authorLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const bool hasAuthor = !book.author.empty();
  auto authorLines =
      hasAuthor ? renderer.wrappedText(UI_10_FONT_ID, book.author.c_str(), textW, 2) : std::vector<std::string>{};
  const int lowerAreaHeight = placeholderRect.y + placeholderRect.height - dividerY;
  const int authorBlockHeight = authorLineHeight * static_cast<int>(authorLines.size());
  const int authorGap = authorLines.empty() ? 0 : titleAuthorGap;
  const int availableTitleHeight = lowerAreaHeight - textVerticalPadding * 2 - authorBlockHeight - authorGap;
  const int maxTitleLines = std::clamp(availableTitleHeight / titleLineHeight, 1, 4);
  auto titleLines = renderer.wrappedText(UI_12_FONT_ID, titleText.c_str(), textW, maxTitleLines);

  const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
  const int totalTextHeight = titleBlockHeight + authorBlockHeight + authorGap;
  int textY = dividerY + std::max(textVerticalPadding, (lowerAreaHeight - totalTextHeight) / 2);

  for (const auto& line : titleLines) {
    const int lineW = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
    renderer.drawText(UI_12_FONT_ID, placeholderRect.x + (placeholderRect.width - lineW) / 2, textY, line.c_str());
    textY += titleLineHeight;
  }

  if (!authorLines.empty()) {
    textY += titleAuthorGap;
    for (const auto& line : authorLines) {
      const int lineW = renderer.getTextWidth(UI_10_FONT_ID, line.c_str());
      renderer.drawText(UI_10_FONT_ID, placeholderRect.x + (placeholderRect.width - lineW) / 2, textY, line.c_str());
      textY += authorLineHeight;
    }
  }
}

void drawBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                   const Color backgroundColor) {
  bool hasCover = false;
  if (!book.coverBmpPath.empty()) {
    const Rect imageRect = coverImageRectForFrame(coverRect);
    const std::string coverBmpPath = coverPathForImageRect(book, imageRect);
    if (!coverBmpPath.empty() && Storage.exists(coverBmpPath.c_str())) {
      FsFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          const Rect bitmapRect = fittedBitmapRect(bitmap, imageRect);
          
          renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                                   backgroundColor);
          renderer.fillRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, kCoverCornerRadius,
                                   Color::White);
          renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height);
          renderer.maskRoundedRectOutsideCorners(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height,
                                                 kCoverCornerRadius, backgroundColor);
          renderer.drawRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, 1,
                                   kCoverCornerRadius, true);
          hasCover = true;
        }
        file.close();
      }
    }
  }

  if (!hasCover) {
    drawMissingBookCover(renderer, coverRect, book);
  }
}
}  // namespace

// ============================================================================
// РЕАЛИЗАЦИЯ МЕТОДОВ MinimalTheme
// ============================================================================

void MinimalTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                              const bool readerContext) const {
  (void)subtitle;

  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != InkMODSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - 12 - MinimalMetrics::values.batteryWidth;
  const int batteryY = rect.y + homeHeaderTopInset;
  drawBatteryRight(renderer,
                   Rect{batteryX, batteryY, MinimalMetrics::values.batteryWidth, MinimalMetrics::values.batteryHeight},
                   showBatteryPercentage);

  if (title) {
    const int maxTitleWidth = batteryX - rect.x - kHeaderTitleInsetX - MinimalMetrics::values.contentSidePadding;
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + kHeaderTitleInsetX, rect.y + MinimalMetrics::values.batteryBarHeight + 3,
                      truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - kHeaderLineHeight, rect.x + rect.width - 1, 
                      rect.y + rect.height - kHeaderLineHeight, kHeaderLineHeight, true);
  }

  {
    int batteryClusterLeftX = batteryX;
    if (showBatteryPercentage) {
      const int maxPercentTextWidth = renderer.getTextWidth(SMALL_FONT_ID, "100%");
      batteryClusterLeftX -= maxPercentTextWidth + batteryPercentSpacing;
    }
    constexpr int clockBatteryGap = 14;
    drawTopStatusBarClock(renderer, rect.y, nullptr, readerContext,
                          readerContext ? 0 : homeHeaderClockTextYOffset(renderer),
                          false, readerContext ? -1 : batteryClusterLeftX - clockBatteryGap);
  }
}

void MinimalTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                              const bool selected) const {
  (void)selected;

  if (tabs.empty()) {
    return;
  }

  const int tabCount = static_cast<int>(tabs.size());
  const int lineY = rect.y + rect.height - 1;
  
  // Рисуем верхнюю и нижнюю линии
  renderer.drawLine(rect.x, rect.y, rect.x + rect.width - 1, rect.y, true);
  renderer.drawLine(rect.x, lineY, rect.x + rect.width - 1, lineY, true);

  for (int i = 0; i < tabCount; i++) {
    const int slotX = rect.x + (i * rect.width) / tabCount;
    const int nextSlotX = rect.x + ((i + 1) * rect.width) / tabCount;
    const int slotWidth = nextSlotX - slotX;
    const auto& tab = tabs[i];
    
    constexpr int tabLabelTextMargin = 6;  // Keep text clear of the slot edges on each side
    const int maxLabelWidth = slotWidth - tabLabelTextMargin * 2;

    // Для выбранной вкладки используем светлый фон
    if (tab.selected) {
      // Рисуем прямоугольник с закруглениями для выбранной вкладки (светло-серый)
      const int paddingX = 8;
      const int paddingY = 4;
      renderer.fillRoundedRect(slotX + paddingX, rect.y + paddingY, 
                               slotWidth - paddingX * 2, rect.height - paddingY * 2,
                               4, Color::LightGray);
      
      // Текст жирным шрифтом
      auto label = renderer.truncatedText(UI_10_FONT_ID, tab.label, maxLabelWidth, EpdFontFamily::BOLD);
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label.c_str(), EpdFontFamily::BOLD);
      const int textX = slotX + (slotWidth - textWidth) / 2;
      const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, textY, label.c_str(), true, EpdFontFamily::BOLD);
    } else {
      // Обычная вкладка - обычный текст
      auto label = renderer.truncatedText(UI_10_FONT_ID, tab.label, maxLabelWidth, EpdFontFamily::REGULAR);
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label.c_str(), EpdFontFamily::REGULAR);
      const int textX = slotX + (slotWidth - textWidth) / 2;
      const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, textY, label.c_str(), true, EpdFontFamily::REGULAR);
    }
  }
}


namespace {

// Smart two-line filename wrapper used by File Browser "2 lines" mode.
//
// GfxRenderer::wrappedText() intentionally refuses to split one very long
// word. That is nice for prose, but file names often contain long tokens,
// underscores, dashes, hashes, etc. In the two-line file-browser mode we
// really want the extra row height to be useful, so this helper can split at
// a UTF-8-safe byte boundary when normal word wrapping cannot.
//
// Priority for line 1:
//   1) normal word wrap;
//   2) nearest separator before the width limit (space/_/-/./()/[]);
//   3) hard UTF-8-safe split at the last glyph that fits.
//
// Line 2 is always truncated with an ellipsis if the complete remainder still
// does not fit.
std::vector<std::string> wrapFileNameTwoLines(const GfxRenderer& renderer, const std::string& title,
                                              const int maxWidth) {
  std::vector<std::string> result;
  if (title.empty() || maxWidth <= 0) return result;

  if (renderer.getTextWidth(UI_10_FONT_ID, title.c_str()) <= maxWidth) {
    result.push_back(title);
    return result;
  }

  // First let the ordinary word wrapper handle human-readable file names.
  const auto normal = renderer.wrappedText(UI_10_FONT_ID, title.c_str(), maxWidth, 2);
  if (normal.size() >= 2) {
    return normal;
  }

  // One over-wide token: find the longest UTF-8-safe prefix that fits.
  size_t bytePos = 0;
  size_t lastFit = 0;
  size_t preferredBreak = std::string::npos;

  while (bytePos < title.size()) {
    const size_t cpStart = bytePos;
    const unsigned char lead = static_cast<unsigned char>(title[bytePos]);

    size_t cpLen = 1;
    if ((lead & 0xE0) == 0xC0) cpLen = 2;
    else if ((lead & 0xF0) == 0xE0) cpLen = 3;
    else if ((lead & 0xF8) == 0xF0) cpLen = 4;
    cpLen = std::min(cpLen, title.size() - bytePos);
    bytePos += cpLen;

    const std::string prefix = title.substr(0, bytePos);
    if (renderer.getTextWidth(UI_10_FONT_ID, prefix.c_str()) > maxWidth) {
      break;
    }

    lastFit = bytePos;

    // Prefer a visually natural filename separator if one exists reasonably
    // close to the edge of line 1.
    if (cpLen == 1) {
      const char ch = title[cpStart];
      if (ch == ' ' || ch == '_' || ch == '-' || ch == '.' ||
          ch == ')' || ch == ']' || ch == '(' || ch == '[') {
        preferredBreak = bytePos;
      }
    }
  }

  if (lastFit == 0) {
    // Extremely narrow column: fall back to the standard safe truncation.
    result.push_back(renderer.truncatedText(UI_10_FONT_ID, title.c_str(), maxWidth));
    return result;
  }

  size_t split = lastFit;
  if (preferredBreak != std::string::npos && preferredBreak >= lastFit / 2) {
    split = preferredBreak;
  }

  std::string first = title.substr(0, split);
  std::string second = title.substr(split);

  auto trimLeft = [](std::string& value) {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '_' || value.front() == '-')) {
      value.erase(value.begin());
    }
  };
  auto trimRight = [](std::string& value) {
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '_' || value.back() == '-')) {
      value.pop_back();
    }
  };

  trimRight(first);
  trimLeft(second);

  if (first.empty()) {
    first = renderer.truncatedText(UI_10_FONT_ID, title.c_str(), maxWidth);
    result.push_back(first);
    return result;
  }

  result.push_back(first);
  if (!second.empty()) {
    result.push_back(renderer.truncatedText(UI_10_FONT_ID, second.c_str(), maxWidth));
  }
  return result;
}

}  // namespace

int MinimalTheme::compactFileBrowserRowHeightFor(const GfxRenderer& renderer) {
  const int textHeight = renderer.getLineHeight(UI_10_FONT_ID) * 2 + kFileBrowserRowVerticalPadding;
  return std::max(kFileBrowserIconSize + kFileBrowserRowVerticalPadding, textHeight);
}

int MinimalTheme::compactFileBrowserRowHeight(const GfxRenderer& renderer) const {
  return compactFileBrowserRowHeightFor(renderer);
}

void MinimalTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                            const std::function<std::string(int index)>& rowTitle,
                            const std::function<std::string(int index)>& rowSubtitle,
                            const std::function<UIIcon(int index)>& rowIcon,
                            const std::function<std::string(int index)>& rowValue, bool highlightValue,
                            const std::function<bool(int index)>& rowDimmed,
                            const std::function<bool(int index)>& isHeader) const {
  const bool compactFileRows = rowSubtitle != nullptr && rowIcon != nullptr && rowValue != nullptr;
  if (!compactFileRows) {
    LyraTheme::drawList(renderer, rect, itemCount, selectedIndex, rowTitle, rowSubtitle, rowIcon, rowValue,
                        highlightValue, rowDimmed, isHeader);
    return;
  }

  drawCompactFileBrowserList(renderer, rect, itemCount, selectedIndex, rowTitle, rowSubtitle, rowIcon, rowValue,
                             rowDimmed);
}

void MinimalTheme::drawCompactFileBrowserList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                              const std::function<std::string(int index)>& rowTitle,
                                              const std::function<std::string(int index)>& rowSubtitle,
                                              const std::function<UIIcon(int index)>& rowIcon,
                                              const std::function<std::string(int index)>& rowValue,
                                              const std::function<bool(int index)>& rowDimmed) {
  if (itemCount <= 0) return;

  const int fileRowHeight = compactFileBrowserRowHeightFor(renderer);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int folderRowHeight = MinimalMetrics::values.listRowHeight;
  const auto isFolderRow = [&](int index) { return rowSubtitle(index) == "folder"; };
  const auto rowHeightFor = [&](int index) { return isFolderRow(index) ? folderRowHeight : fileRowHeight; };
  const auto pageEndFor = [&](int startIndex) {
    int usedHeight = 0;
    int endIndex = startIndex;
    while (endIndex < itemCount) {
      const int nextRowHeight = rowHeightFor(endIndex);
      if (endIndex > startIndex && usedHeight + nextRowHeight > rect.height) break;
      usedHeight += nextRowHeight;
      endIndex++;
    }
    return std::max(startIndex + 1, endIndex);
  };

  int pageStartIndex = 0;
  int pageEndIndex = pageEndFor(pageStartIndex);
  while (selectedIndex >= pageEndIndex && pageEndIndex < itemCount) {
    pageStartIndex = pageEndIndex;
    pageEndIndex = pageEndFor(pageStartIndex);
  }

  int totalPages = 0;
  int currentPage = 0;
  for (int pageStart = 0; pageStart < itemCount;) {
    if (pageStart == pageStartIndex) currentPage = totalPages;
    totalPages++;
    const int nextPageStart = pageEndFor(pageStart);
    if (nextPageStart <= pageStart) break;
    pageStart = nextPageStart;
  }

  const int contentWidth =
      rect.width -
      (totalPages > 1 ? (MinimalMetrics::values.scrollBarWidth + MinimalMetrics::values.scrollBarRightOffset) : 1);

  // Скролл-бар
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;
    const int scrollBarHeight = std::max(MinimalMetrics::values.scrollBarWidth, scrollAreaHeight / totalPages);
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - MinimalMetrics::values.scrollBarRightOffset;
    
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - MinimalMetrics::values.scrollBarWidth, scrollBarY,
                      MinimalMetrics::values.scrollBarWidth, scrollBarHeight, true);
  }

  // Выделение выбранной строки
  if (selectedIndex >= 0) {
    int selectedY = rect.y;
    for (int i = pageStartIndex; i < selectedIndex; i++) {
      selectedY += rowHeightFor(i);
    }
    const int selectedRowHeight = rowHeightFor(selectedIndex);
    renderer.fillRoundedRect(rect.x + MinimalMetrics::values.contentSidePadding, selectedY,
                             contentWidth - MinimalMetrics::values.contentSidePadding * 2, selectedRowHeight, 6,
                             Color::LightGray);
  }

  const int iconX = rect.x + MinimalMetrics::values.contentSidePadding + kFileBrowserTextGap;
  const int textX = iconX + kFileBrowserIconSize + kFileBrowserTextGap;

  int itemY = rect.y;
  for (int i = pageStartIndex; i < itemCount && i < pageEndIndex; i++) {
    const int rowHeight = rowHeightFor(i);
    const bool folderRow = isFolderRow(i);
    const bool selectedRow = i == selectedIndex;

    std::string valueText = rowValue(i);
    if (!valueText.empty()) {
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), kFileBrowserValueMaxWidth);
    }
    const int valueWidth = valueText.empty() ? 0 : renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
    const int valueGap = valueText.empty() ? 0 : kFileBrowserTextGap;
    const int textWidth =
        std::max(1, contentWidth - textX - MinimalMetrics::values.contentSidePadding - valueWidth - valueGap);

    const uint8_t* iconBitmap = iconForName(rowIcon(i), kFileBrowserIconSize);
    if (iconBitmap != nullptr) {
      const int iconY = centeredRowY(itemY, rowHeight, kFileBrowserIconSize);
      renderer.drawIcon(iconBitmap, iconX, iconY, kFileBrowserIconSize, kFileBrowserIconSize);
    }

    const std::string title = rowTitle(i);
    std::vector<std::string> lines;
    if (folderRow) {
      lines.push_back(renderer.truncatedText(UI_10_FONT_ID, title.c_str(), textWidth));
    } else {
      // In File Browser's 2-line mode, actually USE the second line.
      // This also handles long filenames without spaces instead of merely
      // making the row taller and showing the same single-line ellipsis.
      lines = wrapFileNameTwoLines(renderer, title, textWidth);
    }
    const int textBlockHeight = static_cast<int>(lines.size()) * lineHeight;
    int textY = centeredRowY(itemY, rowHeight, textBlockHeight);
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, textX, textY, line.c_str(), true);
      textY += lineHeight;
    }

    if (!valueText.empty()) {
      const int valueY = centeredRowY(itemY, rowHeight, lineHeight);
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - MinimalMetrics::values.contentSidePadding - valueWidth,
                        valueY, valueText.c_str(), true);
    }

    if (rowDimmed && rowDimmed(i) && !selectedRow) {
      const int dimHeight = std::max(lineHeight, textBlockHeight);
      for (int py = itemY; py < itemY + dimHeight; py++) {
        for (int px = textX; px < textX + textWidth; px++) {
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
        }
      }
    }
    itemY += rowHeight;
  }
}

void MinimalTheme::setHomeButtonHintSelection(const int selectedIndex) { homeButtonHintSelection = selectedIndex; }

void MinimalTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4, const bool allowInvertedText) const {
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  const bool invertText = allowInvertedText && origOrientation == GfxRenderer::Orientation::PortraitInverted;
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = 84;
  constexpr int smallButtonHeight = 16;
  constexpr int buttonHeight = MinimalMetrics::values.buttonHintsHeight;
  constexpr int buttonY = MinimalMetrics::values.buttonHintsHeight;
  constexpr int textYOffset = 8;
  constexpr int x4ButtonPositions[] = {54, 142, 254, 342};
  constexpr int x3ButtonPositions[] = {62, 154, 290, 382};
  const int* buttonPositions = screenWidth > 500 ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const int selectedIndex = homeButtonHintSelection;
  homeButtonHintSelection = -1;

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    const bool hasLabel = labels[i] != nullptr && labels[i][0] != '\0';
    if (hasLabel) {
      const Color background = i == selectedIndex ? Color::DarkGray : Color::White;
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, kButtonCornerRadius, background);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, kButtonCornerRadius, true, true,
                               false, false, true);
    } else {
      const int smallButtonY = pageHeight - smallButtonHeight;
      renderer.fillRoundedRect(x, smallButtonY, buttonWidth, smallButtonHeight, kButtonCornerRadius, Color::White);
      renderer.drawRoundedRect(x, smallButtonY, buttonWidth, smallButtonHeight, 1, kButtonCornerRadius, true, true,
                               false, false, true);
    }
  }

  renderer.setOrientation(invertText ? GfxRenderer::Orientation::PortraitInverted : GfxRenderer::Orientation::Portrait);
  const int textY = invertText ? textYOffset : pageHeight - buttonY + textYOffset;

  constexpr int buttonHintsTextMargin = 6;  // Keep text clear of the button border on each side
  for (int i = 0; i < 4; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[invertText ? 3 - i : i];
      auto label = renderer.truncatedText(SMALL_FONT_ID, labels[i], buttonWidth - buttonHintsTextMargin * 2);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, textY, label.c_str());
    }
  }

  renderer.setOrientation(origOrientation);
}

void MinimalTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                       int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                       bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                       const BookReadingStats* stats, float progressPercent) const {
  (void)selectorIndex;
  (void)bufferRestored;

  const Rect coverRect = coverRectForScreen(renderer, rect);
  if (recentBooks.empty()) {
    // Отображение цитат, если нет книг
    renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);

    const MinimalQuote& quote = kQuotes[selectedQuoteIndex()];
    const int textW = coverRect.width - kQuotePadding * 2;
    
    auto lines = renderer.wrappedText(UI_12_FONT_ID, quote.text, textW, 6);
    int lineY = coverRect.y + 80;
    const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    for (const auto& line : lines) {
      renderer.drawText(UI_12_FONT_ID, coverRect.x + kQuotePadding, lineY, line.c_str());
      lineY += lineH;
    }

    const int authorW = renderer.getTextWidth(UI_12_FONT_ID, quote.author, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, coverRect.x + coverRect.width - kQuotePadding - authorW,
                      coverRect.y + coverRect.height - 100, quote.author, true, EpdFontFamily::BOLD);

    if (quote.source != nullptr) {
      const int sourceW = renderer.getTextWidth(UI_10_FONT_ID, quote.source, EpdFontFamily::ITALIC);
      renderer.drawText(UI_10_FONT_ID, coverRect.x + coverRect.width - kQuotePadding - sourceW,
                        coverRect.y + coverRect.height - 78, quote.source, true, EpdFontFamily::ITALIC);
    }

    const int lineX = coverRect.x + kQuotePadding;
    renderer.drawLine(lineX, coverRect.y + coverRect.height - 122, lineX + 60, coverRect.y + coverRect.height - 122, true);

    coverRendered = false;
    coverBufferStored = false;
    return;
  }

  if (!coverRendered) {
    drawBookCover(renderer, coverRect, recentBooks[0], Color::White);
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  drawProgressBlock(renderer, coverRect, stats, progressPercent, false);
}

void MinimalTheme::drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                                   const float progressPercent) const {
  renderer.clearScreen(0x00);

  const Rect contentRect{0, MinimalMetrics::values.homeTopPadding, renderer.getScreenWidth(),
                         MinimalMetrics::values.homeCoverTileHeight};
  const Rect coverRect = coverRectForScreen(renderer, contentRect);
  drawBookCover(renderer, coverRect, book, Color::White);
  drawProgressBlock(renderer, coverRect, stats, progressPercent, true);
}

void MinimalTheme::drawStatsSleepScreen(const GfxRenderer& renderer, const RecentBook& book,
                                        const BookReadingStats* stats, const GlobalReadingStats* globalStats,
                                        const float progressPercent) const {
  drawSleepScreen(renderer, book, stats, progressPercent);
  if (globalStats != nullptr) {
    const Rect contentRect{0, MinimalMetrics::values.homeTopPadding, renderer.getScreenWidth(),
                           MinimalMetrics::values.homeCoverTileHeight};
    const Rect coverRect = coverRectForScreen(renderer, contentRect);
    drawStatsOverlay(renderer, *globalStats, coverRect, progressPercent);
  }
}

void MinimalTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                  const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rect;
  (void)rowIcon;

  if (buttonCount <= 0) {
    return;
  }

  const int panelW = std::min(kMenuPanelWidth, renderer.getScreenWidth() - 80);
  const int panelH = buttonCount * kMenuRowHeight + 4;
  const int panelX = (renderer.getScreenWidth() - panelW) / 2;
  const int panelY = kMenuPanelTop;
  
  // Рисуем панель меню
  renderer.drawRoundedRect(panelX, panelY, panelW, panelH, 1, kMenuPanelRadius, true);

  for (int i = 0; i < buttonCount; ++i) {
    const int rowY = panelY + 2 + i * kMenuRowHeight;
    int availableWidth = panelW - 24;

    // Треугольник для выбранного пункта
    if (i == selectedIndex) {
      const int triangleX = panelX + kMenuSelectionTriangleInset;
      const int triangleCenterY = rowY + kMenuRowHeight / 2;
      const int triangleHalfH = kMenuSelectionTriangleHeight / 2;
      const int triangleXPoints[3] = {triangleX, triangleX, triangleX + kMenuSelectionTriangleWidth};
      const int triangleYPoints[3] = {triangleCenterY - triangleHalfH, triangleCenterY + triangleHalfH,
                                      triangleCenterY};
      renderer.fillPolygon(triangleXPoints, triangleYPoints, 3, true);

      availableWidth -= (kMenuSelectionTriangleInset + kMenuSelectionTriangleWidth + 12) * 2;
    }

    std::string label = buttonLabel(i);
    label = renderer.truncatedText(UI_12_FONT_ID, label.c_str(), availableWidth);

    const int labelW = renderer.getTextWidth(UI_12_FONT_ID, label.c_str());
    const int labelY = rowY + (kMenuRowHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, panelX + (panelW - labelW) / 2, labelY, label.c_str());
    
    // Декоративная линия между пунктами
    if (i < buttonCount - 1) {
      renderer.drawLine(panelX + 20, rowY + kMenuRowHeight - 1, panelX + panelW - 20, rowY + kMenuRowHeight - 1, true);
    }
  }
}
