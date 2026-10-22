#pragma once

#include <BoardConfig.h>
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
