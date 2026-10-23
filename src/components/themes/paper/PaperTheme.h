#pragma once

#include <BoardConfig.h>
#include <Bitmap.h>
#include <HalStorage.h>
#include <I18n.h>

#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "components/UITheme.h"
#include <GfxRenderer.h>

#include <algorithm>
#include <string>
#include <vector>

#include "components/themes/roundedraff/RoundedRaffTheme.h"
#include "fontIds.h"

// Paper UI: an opt-in, low-overdraw E-Ink skin. The existing Activity,
// input mapping, page rendering, files, dictionaries and storage stay intact.
// Inherit the proven RoundedRaff list geometry so touch hit-testing in Settings
// remains pixel-aligned with the painted rows.
namespace PaperMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics m = RoundedRaffMetrics::values;
  m.contentSidePadding = 16;
  m.homeCoverHeight = 245;
  m.homeCoverTileHeight = 282;
  m.homeTopPadding = 48;
  m.homeMenuTopOffset = 12;
  m.headerHeight = 49;
  m.tabBarHeight = 52;
  m.menuRowHeight = 45;
  m.menuSpacing = 6;
  m.listRowHeight = 56;
  m.listWithSubtitleRowHeight = 69;
  m.popupCornerRadius = 12;
  m.keyboardKeyCornerRadius = 9;
  return m;
}
constexpr ThemeMetrics values = makeValues();
}  // namespace PaperMetrics

class PaperTheme final : public RoundedRaffTheme {
 public:
  void drawRecentBookCover(
      GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
      int selectorIndex, bool& coverRendered, bool& coverBufferStored,
      bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
      const BookReadingStats* stats = nullptr, float progressPercent = -1.0f) const override {
    (void)selectorIndex;
    (void)coverRendered;
    (void)coverBufferStored;
    (void)bufferRestored;
    (void)storeCoverBuffer;

    // Paper has its own smaller cover height. Reusing RoundedRaff's image
    // renderer requested a 260px thumbnail while Home generated a 245px one,
    // leaving a completely empty placeholder on the X4 Pro.
    const int pad = PaperMetrics::values.contentSidePadding;
    const int cardX = rect.x + pad;
    const int cardY = rect.y + 4;
    const int cardW = std::max(1, rect.width - pad * 2);
    const int cardH = std::max(1, rect.height - 8);
    renderer.fillRoundedRect(cardX, cardY, cardW, cardH, 12, Color::White);
    renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 1, 12, true);

    if (recentBooks.empty()) {
      const std::string label = renderer.truncatedText(
          UI_12_FONT_ID, tr(STR_NO_OPEN_BOOK), cardW - 24);
      renderer.drawText(UI_12_FONT_ID, cardX + 12, cardY + cardH / 2,
                        label.c_str(), true, EpdFontFamily::BOLD);
      return;
    }

    const RecentBook& book = recentBooks.front();
    const int imgH = std::max(1, std::min(PaperMetrics::values.homeCoverHeight - 16, cardH - 20));
    const int imgW = std::max(1, std::min(imgH * 3 / 5, cardW * 40 / 100));
    const int imgX = cardX + 12;
    const int imgY = cardY + (cardH - imgH) / 2;
    bool hasImage = false;
    if (!book.coverBmpPath.empty()) {
      // Retry the original BMP when its sized thumbnail has not been generated.
      const std::string thumb = UITheme::getCoverThumbPath(
          book.coverBmpPath, PaperMetrics::values.homeCoverHeight);
      for (int pass = 0; pass < 2 && !hasImage; ++pass) {
        const std::string& candidate = pass == 0 ? thumb : book.coverBmpPath;
        HalFile file;
        if (!Storage.openFileForRead("PAPER", candidate, file)) continue;
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, imgX, imgY, imgW, imgH);
          hasImage = true;
        }
        file.close();
      }
    }
    if (!hasImage) {
      renderer.fillRoundedRect(imgX, imgY, imgW, imgH, 8, Color::LightGray);
      renderer.drawRoundedRect(imgX, imgY, imgW, imgH, 1, 8, true);
      const std::string title = renderer.truncatedText(
          UI_10_FONT_ID, book.title.c_str(), imgW - 12, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, imgX + 6, imgY + imgH / 2,
                        title.c_str(), true, EpdFontFamily::BOLD);
    }
    renderer.drawRoundedRect(imgX, imgY, imgW, imgH, 1, 8, true);

    const int tx = imgX + imgW + 14;
    const int tw = std::max(1, cardX + cardW - 12 - tx);
    const std::string title = renderer.truncatedText(
        UI_12_FONT_ID, book.title.c_str(), tw, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, tx, cardY + 22, title.c_str(),
                      true, EpdFontFamily::BOLD);
    const std::string author = renderer.truncatedText(
        UI_10_FONT_ID, book.author.c_str(), tw);
    renderer.drawText(UI_10_FONT_ID, tx, cardY + 54, author.c_str());

    if (progressPercent >= 0.0f) {
      const int pct = std::clamp(static_cast<int>(progressPercent + 0.5f), 0, 100);
      const std::string progress = std::to_string(pct) + "%";
      renderer.drawText(UI_12_FONT_ID, tx, cardY + 99,
                        progress.c_str(), true, EpdFontFamily::BOLD);
      const int barY = cardY + 134;
      const int barW = std::max(1, tw - 3);
      renderer.drawRoundedRect(tx, barY, barW, 9, 1, 4, true);
      if (pct > 0 && barW > 4) {
        renderer.fillRect(tx + 2, barY + 2, (barW - 4) * pct / 100, 5, true);
      }
    }
    if (stats) {
      char duration[40];
      BookReadingStats::formatDuration(stats->totalReadingSeconds, duration, sizeof(duration));
      const std::string time = renderer.truncatedText(UI_10_FONT_ID, duration, tw);
      renderer.drawText(UI_10_FONT_ID, tx, cardY + 170, time.c_str());
      const std::string sessions = std::to_string(stats->sessionCount) + " " + tr(STR_STATS_SESSIONS_LBL);
      const std::string visible = renderer.truncatedText(UI_10_FONT_ID, sessions.c_str(), tw);
      renderer.drawText(UI_10_FONT_ID, tx, cardY + 197, visible.c_str());
    }
  }

  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2,
                       const char* btn3, const char* btn4,
                       bool allowInvertedText = false,
                       bool forceOnTouch = false) const override {
    if (!BoardConfig::isX4Pro() || !forceOnTouch) {
      RoundedRaffTheme::drawButtonHints(renderer, btn1, btn2, btn3, btn4,
                                        allowInvertedText, forceOnTouch);
      return;
    }
    // BookStatsActivity maps the bottom 64px to four equally sized tap slots.
    // RoundedRaff grouped two labels inside the same pill, so the long Russian
    // "Общая статистика" and "Изменить" labels visually overlapped.
    const GfxRenderer::Orientation original = renderer.getOrientation();
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    const int w = renderer.getScreenWidth();
    const int h = renderer.getScreenHeight();
    const int slotW = w / 4;
    const int top = h - 47;
    const char* labels[4] = {btn1, btn2, btn3, btn4};
    for (int i = 0; i < 4; ++i) {
      if (!labels[i] || !labels[i][0]) continue;
      const int x = i * slotW + 3;
      const int cellW = std::max(1, (i == 3 ? w - i * slotW : slotW) - 6);
      renderer.fillRoundedRect(x, top, cellW, 36, 8, Color::White);
      renderer.drawRoundedRect(x, top, cellW, 36, 1, 8, true);
      const std::string txt = renderer.truncatedText(
          UI_10_FONT_ID, labels[i], std::max(1, cellW - 10));
      const int textX = x + (cellW - renderer.getTextWidth(UI_10_FONT_ID, txt.c_str())) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, top + (36 - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                        txt.c_str(), true);
    }
    renderer.setOrientation(original);
  }

  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                  const char* subtitle = nullptr, bool readerContext = false) const override {
    // Clear the entire header on every render to avoid residual clock text.
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    const int left = rect.x + PaperMetrics::values.contentSidePadding;
    const int right = rect.x + rect.width - PaperMetrics::values.contentSidePadding;
    const int ruleY = rect.y + rect.height - 2;
    renderer.drawLine(left, ruleY, right, ruleY, true);

    if (title == nullptr) {
      drawTopStatusBarClock(renderer, rect.y, nullptr, readerContext,
                            readerContext ? 0 : homeHeaderClockTextYOffset(renderer));
      return;
    }

    constexpr int kFont = UI_12_FONT_ID;
    const bool percentage = SETTINGS.hideBatteryPercentage !=
                            InkMODSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
    const int batteryX = right - PaperMetrics::values.batteryWidth;
    const int batteryY = rect.y + homeHeaderTopInset;
    const int batteryClusterLeft = batteryX - (percentage ? 49 : 0);
    const int titleMaxW = std::max(0, batteryClusterLeft - left - 12);
    const std::string label = renderer.truncatedText(
        kFont, title, titleMaxW, EpdFontFamily::BOLD);
    const int titleY = rect.y + std::max(4, (rect.height - renderer.getLineHeight(kFont)) / 2 - 2);
    renderer.drawText(kFont, left, titleY, label.c_str(), true, EpdFontFamily::BOLD);
    drawBatteryRight(renderer, Rect{batteryX, batteryY,
                     PaperMetrics::values.batteryWidth, PaperMetrics::values.batteryHeight},
                     percentage);
    // The header clock is part of the existing status bar, not a timer.
    drawTopStatusBarClock(renderer, rect.y, nullptr, readerContext,
                          readerContext ? 0 : homeHeaderClockTextYOffset(renderer),
                          false, readerContext ? -1 : batteryClusterLeft - 12);
    (void)subtitle;
  }

  void drawTabBar(const GfxRenderer& renderer, Rect rect,
                  const std::vector<TabInfo>& tabs, bool selected) const override {
    if (tabs.empty()) return;
    const int n = static_cast<int>(tabs.size());
    const int top = rect.y + 6;
    const int height = std::max(24, rect.height - 14);
    for (int i = 0; i < n; ++i) {
      const int slotLeft = rect.x + i * rect.width / n;
      const int slotRight = rect.x + (i + 1) * rect.width / n;
      const int x = slotLeft + 3;
      const int w = std::max(1, slotRight - slotLeft - 6);
      const bool active = tabs[i].selected;
      if (active) {
        renderer.fillRoundedRect(x, top, w, height, 11,
                                 selected ? Color::Black : Color::DarkGray);
      } else {
        renderer.drawRoundedRect(x, top, w, height, 1, 11, true);
      }
      const int font = UI_10_FONT_ID;
      const std::string label = renderer.truncatedText(
          font, tabs[i].label, std::max(0, w - 12), EpdFontFamily::BOLD);
      const int textX = x + (w - renderer.getTextWidth(font, label.c_str(),
                                                       EpdFontFamily::BOLD)) / 2;
      const int textY = top + (height - renderer.getLineHeight(font)) / 2;
      renderer.drawText(font, textX, textY, label.c_str(), !active,
                        EpdFontFamily::BOLD);
    }
  }

  int getMenuRowHeight(const GfxRenderer&) const override {
    return PaperMetrics::values.menuRowHeight;
  }

  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount,
                      int selectedIndex,
                      const std::function<std::string(int)>& buttonLabel,
                      const std::function<UIIcon(int)>& rowIcon) const override {
    // Full-width editorial cards, equal row/hitbox geometry, no animation.
    // Reuse the existing Home action callbacks; visual-only change.
    (void)rowIcon;
    constexpr int gap = 6;
    const int h = PaperMetrics::values.menuRowHeight;
    const int step = h + gap;
    const int count = std::max(1, rect.height / step);
    const int start = (std::max(0, selectedIndex) / count) * count;
    const int x = rect.x + PaperMetrics::values.contentSidePadding;
    const int w = std::max(0, rect.width - 2 * PaperMetrics::values.contentSidePadding);
    const int font = UI_12_FONT_ID;
    for (int i = start; i < buttonCount && i < start + count; ++i) {
      const int y = rect.y + (i - start) * step;
      const bool active = i == selectedIndex;
      renderer.fillRoundedRect(x, y, w, h, 10, active ? Color::Black : Color::White);
      if (!active) renderer.drawRoundedRect(x, y, w, h, 1, 10, true);
      const std::string label = renderer.truncatedText(
          font, buttonLabel(i).c_str(), std::max(0, w - 55), EpdFontFamily::BOLD);
      renderer.drawText(font, x + 16, y + (h - renderer.getLineHeight(font)) / 2,
                        label.c_str(), !active, EpdFontFamily::BOLD);
      // Non-selected chevron supports discoverability without an image asset.
      const int cx = x + w - 20;
      const int cy = y + h / 2;
      renderer.drawLine(cx - 4, cy - 5, cx + 1, cy, !active);
      renderer.drawLine(cx + 1, cy, cx - 4, cy + 5, !active);
    }
    if (buttonCount > count) {
      const int xLine = rect.x + rect.width - 4;
      const int track = std::max(1, rect.height);
      const int marker = std::max(8, track * count / buttonCount);
      const int pos = (track - marker) * start / std::max(1, buttonCount - count);
      renderer.fillRect(xLine, rect.y + pos, 2, marker, true);
    }
  }
};
