#include "BaseTheme.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <numeric>
#include <string>

#include "I18n.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "components/UITheme.h"
#include "UiTextSize.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;

}  // namespace

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight,
                                   const bool foregroundBlack) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y, foregroundBlack);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1, foregroundBlack);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2, foregroundBlack);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2, foregroundBlack);
  renderer.drawPixel(x + battWidth - 1, y + 3, foregroundBlack);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4, foregroundBlack);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5, foregroundBlack);
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY,
                                         const bool foregroundBlack) {
  // Draw lightning bolt (white/inverted on black fill for visibility)
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, foregroundBlack);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, foregroundBlack);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, foregroundBlack);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, foregroundBlack);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, foregroundBlack);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, foregroundBlack);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, foregroundBlack);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, foregroundBlack);
}

void BaseTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage,
                                const bool foregroundBlack) const {
  const bool charging = gpio.isUsbConnected();

  const int maxFillWidth = rect.width - 5;
  const int fillHeight = rect.height - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }
  // +1 to round up so we always fill at least one pixel
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }

  // When charging, ensure minimum fill so lightning bolt is fully visible
  constexpr int minFillForBolt = 8;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }

  renderer.fillRect(rect.x + 2, rect.y + 2, filledWidth, fillHeight, foregroundBlack);

  if (charging) {
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2, !foregroundBlack);
  }
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage,
                                const bool foregroundBlack) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + rect.width, rect.y, percentageText.c_str(),
                      foregroundBlack);
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height, foregroundBlack);
  fillBatteryIcon(renderer, iconRect, percentage, foregroundBlack);
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage,
                                 const bool foregroundBlack) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  // rect.x is already positioned for the icon (drawHeader calculated it)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x - textWidth - batteryPercentSpacing, rect.y, percentageText.c_str(),
                      foregroundBlack);
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height, foregroundBlack);
  fillBatteryIcon(renderer, iconRect, percentage, foregroundBlack);
}

int BaseTheme::homeHeaderClockTextYOffset(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int statusBarHeight = std::max(UITheme::getStatusBarHeight(), metrics.statusBarVerticalMargin);
  const int centeredClockY = (statusBarHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
  return homeHeaderTopInset - centeredClockY;
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4, const bool allowInvertedText) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  const bool invertText = allowInvertedText && orig_orientation == GfxRenderer::Orientation::PortraitInverted;
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = BaseMetrics::values.buttonHintsHeight;
  constexpr int buttonY = BaseMetrics::values.buttonHintsHeight;
  constexpr int textYOffset = 7;  // Distance from top of button to text baseline
  // Evenly spaced with equal outer margins on both sides (previous values
  // let neighboring buttons overlap by a pixel and had unequal margins).
  constexpr int x4ButtonPositions[] = {11, 128, 246, 363};
  constexpr int x3ButtonPositions[] = {21, 148, 274, 401};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
      renderer.drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
    }
  }

  renderer.setOrientation(invertText ? GfxRenderer::Orientation::PortraitInverted : GfxRenderer::Orientation::Portrait);
  const int textY = invertText ? textYOffset : pageHeight - buttonY + textYOffset;

  constexpr int buttonHintsTextMargin = 8;  // Keep text clear of the button border on each side
  for (int i = 0; i < 4; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[invertText ? 3 - i : i];
      auto label = renderer.truncatedText(UI_10_FONT_ID, labels[i], buttonWidth - buttonHintsTextMargin * 2);
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label.c_str());
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, textY, label.c_str());
    }
  }

  renderer.setOrientation(orig_orientation);
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 4;

  // Text runs along the button's vertical extent once rotated, so clamp to buttonHeight (minus a small margin)
  // rather than letting a long translated label spill past the button edges.
  constexpr int sideButtonTextMargin = 6;
  constexpr int sideButtonMaxTextLen = buttonHeight - sideButtonTextMargin * 2;

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      const int leftX = buttonMargin;
      renderer.drawRect(leftX, x3ButtonY, buttonWidth, buttonHeight);
      auto label = renderer.truncatedText(SMALL_FONT_ID, topBtn, sideButtonMaxTextLen);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = leftX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, label.c_str());
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonMargin - buttonWidth;
      renderer.drawRect(rightX, x3ButtonY, buttonWidth, buttonHeight);
      auto label = renderer.truncatedText(SMALL_FONT_ID, bottomBtn, sideButtonMaxTextLen);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = rightX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, label.c_str());
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    constexpr int topButtonY = 345;
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonMargin - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);
      renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
    }

    if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
      renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                        topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topButtonY + i * buttonHeight;
        auto label = renderer.truncatedText(SMALL_FONT_ID, labels[i], sideButtonMaxTextLen);
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
        const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
        const int textX = x + (buttonWidth - textHeight) / 2;
        const int textY = y + (buttonHeight + textWidth) / 2;
        renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, label.c_str());
      }
    }
  }
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed,
                         const std::function<bool(int index)>& isHeader) const {
  if (itemCount <= 0) return;

  // OptionSelection always supplies a callback, including for a list whose
  // subtitles are all empty. Treating the callback itself as a subtitle made
  // every simple menu use the tall two-line row.
  const int subtitleProbeIndex = std::clamp(selectedIndex, 0, itemCount - 1);
  const bool hasSubtitle = rowSubtitle != nullptr && !rowSubtitle(subtitleProbeIndex).empty();
  // Row height and the subtitle's vertical offset both used to be fixed
  // constants (BaseMetrics::values.listWithSubtitleRowHeight, and a bare
  // "itemY + 22" below) sized for the normal 10pt UI font. Accessibility's
  // "Increase interface text" setting (see main.cpp's applyUiTextSize())
  // swaps UI_10_FONT_ID for a taller 12pt family without touching either
  // of those constants, so the title text itself grew past where the
  // subtitle started drawing, and two-line rows grew past the row height
  // reserved for them - both showing up as the next row's text overlapping
  // this one. Deriving both from the font's own actual line height instead
  // means this stays correct at whichever size is currently loaded.
  const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int kTitleSubtitleGap = 4;
  constexpr int kSubtitleBottomPadding = 6;
  const int subtitleLineH = hasSubtitle ? renderer.getLineHeight(SMALL_FONT_ID) : 0;
  const int subtitleOffsetY = titleLineH + kTitleSubtitleGap;
  const int dynamicSubtitleRowHeight = subtitleOffsetY + subtitleLineH + kSubtitleBottomPadding;
  int rowHeight = hasSubtitle
                      ? std::max(BaseMetrics::values.listWithSubtitleRowHeight, dynamicSubtitleRowHeight)
                      : BaseMetrics::values.listRowHeight;
  int pageItems = rect.height / rowHeight;
  constexpr int sectionHeaderTopPadding = 15;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    constexpr int indicatorWidth = 20;
    constexpr int arrowSize = 6;
    constexpr int margin = 15;  // Offset from right edge

    const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
    const int indicatorTop = rect.y;  // Offset to avoid overlapping side button hints
    const int indicatorBottom = rect.y + rect.height - arrowSize;

    // Draw up arrow at top (^) - narrow point at top, wide base at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + i * 2;
      const int startX = centerX - i;
      renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
    }

    // Draw down arrow at bottom (v) - wide base at top, narrow point at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
      const int startX = centerX - (arrowSize - 1 - i);
      renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                        indicatorBottom - arrowSize + 1 + i);
    }
  }

  // Draw selection (skip header rows)
  int contentWidth = rect.width - 5;
  if (selectedIndex >= 0) {
    renderer.fillRect(rect.x, rect.y + selectedIndex % pageItems * rowHeight - 2, rect.width, rowHeight);
  }
  constexpr int maxValueWidth = 240;
  constexpr int minValueGap = 10;

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  const int rectBottom = rect.y + rect.height;
  if (selectedIndex >= 0 && !(isHeader && isHeader(selectedIndex))) {
    int selY = rect.y;
    for (int j = pageStartIndex; j < selectedIndex; j++) {
      selY += rowHeight;
      if (isHeader && isHeader(j + 1)) selY += sectionHeaderTopPadding;
    }
    if (selY + rowHeight <= rectBottom) {
      renderer.fillRect(rect.x, selY - 2, rect.width, rowHeight);
    }
  }

  // Draw all visible page items
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;

    int rowTextWidth = contentWidth - BaseMetrics::values.contentSidePadding * 2;
    std::string valueText;
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        int maxValW = std::max(0, rowTextWidth - 40 - minValueGap);
        valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxValW);
        int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + minValueGap;
        rowTextWidth -= valueWidth;
      }
    }

    auto itemName = rowTitle(i);
    auto font = UI_10_FONT_ID;
    auto item = renderer.truncatedText(font, itemName.c_str(), rowTextWidth);
    if (isHeader && isHeader(i)) {
      renderer.drawText(font, rect.x + BaseMetrics::values.contentSidePadding, itemY, item.c_str(), true,
                        EpdFontFamily::BOLD);
      continue;
    }
    const int titleY = hasSubtitle ? itemY : itemY + (rowHeight - titleLineH) / 2;
    renderer.drawText(font, rect.x + BaseMetrics::values.contentSidePadding, titleY, item.c_str(), i != selectedIndex);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      const int titleWidth = renderer.getTextWidth(font, item.c_str());
      const int lineH = renderer.getLineHeight(font);
      const int tx = rect.x + BaseMetrics::values.contentSidePadding;
      for (int py = titleY; py < titleY + lineH; py++)
        for (int px = tx; px < tx + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (hasSubtitle) {
      std::string subtitleText = rowSubtitle(i);
      if (!subtitleText.empty()) {
        auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
        renderer.drawText(SMALL_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, itemY + subtitleOffsetY,
                          subtitle.c_str(), i != selectedIndex);
      }
    }

    if (!valueText.empty()) {
      const auto valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      int valueY = itemY;
      if (hasSubtitle) {
        valueY = itemY + 10;
      } else {
        valueY = itemY + (rowHeight - titleLineH) / 2;
      }
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - BaseMetrics::values.contentSidePadding - valueTextWidth,
                        valueY, valueText.c_str(), i != selectedIndex);
    }
  }
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                           const bool readerContext) const {
  // Hide last battery draw
  constexpr int maxBatteryWidth = 80;
  renderer.fillRect(rect.x + rect.width - maxBatteryWidth, rect.y + homeHeaderTopInset, maxBatteryWidth,
                    BaseMetrics::values.batteryHeight + 10, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != InkMODSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - BaseMetrics::values.batteryWidth;
  const int batteryY = rect.y + homeHeaderTopInset;
  drawBatteryRight(renderer,
                   Rect{batteryX, batteryY, BaseMetrics::values.batteryWidth, BaseMetrics::values.batteryHeight},
                   showBatteryPercentage);

  if (title) {
    int padding = rect.width - batteryX + BaseMetrics::values.batteryWidth;
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title,
                                                 rect.width - padding * 2 - BaseMetrics::values.contentSidePadding * 2,
                                                 EpdFontFamily::BOLD);
    const bool showHeaderClock = halClock.isAvailable() && (readerContext ? SETTINGS.shouldShowClockInReader()
                                                                          : SETTINGS.shouldShowClockOutsideReader());
    if (showHeaderClock) {
      renderer.drawText(UI_12_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, rect.y + 5,
                        truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
    } else {
      renderer.drawCenteredText(UI_12_FONT_ID, rect.y + 5, truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
    }
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(
        SMALL_FONT_ID, subtitle, rect.width - BaseMetrics::values.contentSidePadding * 2, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - BaseMetrics::values.contentSidePadding - truncatedSubtitleWidth, subtitleY,
                      truncatedSubtitle.c_str(), true);
  }

  {
    // Anchor the clock immediately to the left of the battery cluster (icon + percentage,
    // when shown), with a small gap, so the clock always sits in the same corner as the
    // battery instead of drifting to the center of the header.
    int batteryClusterLeftX = batteryX;
    if (showBatteryPercentage) {
      const int maxPercentTextWidth = renderer.getTextWidth(SMALL_FONT_ID, "100%");
      batteryClusterLeftX -= maxPercentTextWidth + batteryPercentSpacing;
    }
    constexpr int clockBatteryGap = 14;
    drawTopStatusBarClock(
        renderer, rect.y, nullptr, readerContext,
        readerContext ? 0 : homeHeaderClockTextYOffset(renderer), false,
        readerContext ? -1 : batteryClusterLeftX - clockBatteryGap);
  }
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline
  constexpr int maxListValueWidth = 200;

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  int rightSpace = BaseMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - BaseMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + 10;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_12_FONT_ID, label, rect.width - BaseMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, currentX, rect.y, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
}

void BaseTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline

  const int lineHeight = renderer.getLineHeight(uiControlFontId());

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  const int availableWidth = rect.width - BaseMetrics::values.contentSidePadding * 2;

  // If the natural widths of all tab labels don't fit, cap each label to a fair share of the
  // available width so long translated strings truncate instead of running off the tab bar.
  const int naturalTotalWidth = std::accumulate(tabs.begin(), tabs.end(), 0, [&](const int total, const TabInfo& tab) {
    return total + renderer.getTextWidth(UI_12_FONT_ID, tab.label,
                                         tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR) +
           BaseMetrics::values.tabSpacing;
  });
  const int perTabMaxWidth = tabs.empty() ? 0 : availableWidth / static_cast<int>(tabs.size()) -
                                                    BaseMetrics::values.tabSpacing;
  const bool needsTruncation = naturalTotalWidth > availableWidth && perTabMaxWidth > 0;

  for (const auto& tab : tabs) {
    const auto style = tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    std::string label =
        needsTruncation ? renderer.truncatedText(UI_12_FONT_ID, tab.label, perTabMaxWidth, style) : tab.label;
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label.c_str(), style);

    // Draw underline for selected tab
    if (tab.selected) {
      if (selected) {
        renderer.fillRect(currentX - 3, rect.y, textWidth + 6, lineHeight + underlineGap);
      } else {
        renderer.fillRect(currentX, rect.y + lineHeight + underlineGap, textWidth, underlineHeight);
      }
    }

    // Draw tab label
    renderer.drawText(UI_12_FONT_ID, currentX, rect.y, label.c_str(), !(tab.selected && selected), style);

    currentX += textWidth + BaseMetrics::values.tabSpacing;
  }
}

// Draw the "Recent Book" cover card on the home screen
// TODO: Refactor method to make it cleaner, split into smaller methods
void BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                    const BookReadingStats* /*stats*/, float /*progressPercent*/) const {
  const bool hasContinueReading = !recentBooks.empty();
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  // When there's no cover image, use fixed size (half screen)
  // When there's cover image, adapt width to image aspect ratio, keep height fixed at 400px
  const int baseHeight = rect.height;  // Fixed height (400px)

  int bookWidth, bookX;
  bool hasCoverImage = false;

  if (hasContinueReading && !recentBooks[0].coverBmpPath.empty()) {
    // Try to get actual image dimensions from BMP header
    const std::string coverBmpPath =
        UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

    FsFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        hasCoverImage = true;
        const int imgWidth = bitmap.getWidth();
        const int imgHeight = bitmap.getHeight();

        // Calculate width based on aspect ratio, maintaining baseHeight
        if (imgWidth > 0 && imgHeight > 0) {
          const float aspectRatio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
          bookWidth = static_cast<int>(baseHeight * aspectRatio);

          // Ensure width doesn't exceed reasonable limits (max 90% of screen width)
          const int maxWidth = static_cast<int>(rect.width * 0.9f);
          if (bookWidth > maxWidth) {
            bookWidth = maxWidth;
          }
        } else {
          bookWidth = rect.width / 2;  // Fallback
        }
      }
    }
  }

  if (!hasCoverImage) {
    // No cover: use half screen size
    bookWidth = rect.width / 2;
  }

  bookX = rect.x + (rect.width - bookWidth) / 2;
  const int bookY = rect.y;
  const int bookHeight = baseHeight;

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    // Draw cover image as background if available (inside the box)
    // Only load from SD on first render, then use stored buffer

    if (hasContinueReading && !recentBooks[0].coverBmpPath.empty() && !coverRendered) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

      // First time: load cover from SD and render
      FsFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          LOG_DBG("THEME", "Rendering bmp");

          // Draw the cover image (bookWidth and bookHeight already match image aspect ratio)
          renderer.drawBitmap(bitmap, bookX, bookY, bookWidth, bookHeight);

          // Draw border around the card
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

          // No bookmark ribbon when cover is shown - it would just cover the art

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer

          // First render: if selected, draw selection indicators now
          if (bookSelected) {
            LOG_DBG("THEME", "Drawing selection");
            renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
            renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
          }
        }
      }
    }

    if (!bufferRestored && !coverRendered) {
      // No cover image: draw border or fill, plus bookmark as visual flair
      if (bookSelected) {
        renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
      } else {
        renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
      }

      // Draw bookmark ribbon when no cover image (visual decoration)
      if (hasContinueReading) {
        const int notchDepth = bookmarkHeight / 3;
        const int centerX = bookmarkX + bookmarkWidth / 2;

        const int xPoints[5] = {
            bookmarkX,                  // top-left
            bookmarkX + bookmarkWidth,  // top-right
            bookmarkX + bookmarkWidth,  // bottom-right
            centerX,                    // center notch point
            bookmarkX                   // bottom-left
        };
        const int yPoints[5] = {
            bookmarkY,                                // top-left
            bookmarkY,                                // top-right
            bookmarkY + bookmarkHeight,               // bottom-right
            bookmarkY + bookmarkHeight - notchDepth,  // center notch point
            bookmarkY + bookmarkHeight                // bottom-left
        };

        // Draw bookmark ribbon (inverted if selected)
        renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
      }
    }

    // If buffer was restored, draw selection indicators if needed
    if (bufferRestored && bookSelected && coverRendered) {
      // Draw selection border (no bookmark inversion needed since cover has no bookmark)
      renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
      renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
    } else if (!coverRendered && !bufferRestored) {
      // Selection border already handled above in the no-cover case
    }
  }

  if (!hasContinueReading) {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_NO_OPEN_BOOK));
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), tr(STR_START_READING));
  }
}

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;
  constexpr int maxVisibleItems = 7;
  const int pageItems = maxVisibleItems;
  const int totalPages = (buttonCount + pageItems - 1) / pageItems;

  const int pageStartIndex = (selectedIndex / pageItems) * pageItems;

  if (totalPages > 1) {
    constexpr int indicatorWidth = 20;
    constexpr int arrowSize = 6;
    constexpr int margin = 15;  // Offset from right edge

    const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
    const int menuHeight = maxVisibleItems * (BaseMetrics::values.menuRowHeight + BaseMetrics::values.menuSpacing) -
                           BaseMetrics::values.menuSpacing;
    const int indicatorTop = rect.y + BaseMetrics::values.verticalSpacing;
    const int indicatorBottom = indicatorTop + menuHeight - arrowSize;

    // Draw up arrow (^) only when there are items above the current page
    if (pageStartIndex > 0) {
      for (int i = 0; i < arrowSize; ++i) {
        const int lineWidth = 1 + i * 2;
        const int startX = centerX - i;
        renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
      }
    }

    // Draw down arrow (v) only when there are items below the current page
    if (pageStartIndex + pageItems < buttonCount) {
      for (int i = 0; i < arrowSize; ++i) {
        const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
        const int startX = centerX - (arrowSize - 1 - i);
        renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                          indicatorBottom - arrowSize + 1 + i);
      }
    }
  }

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const int displayIndex = i - pageStartIndex;
    const int tileY =
        BaseMetrics::values.verticalSpacing + rect.y +
        static_cast<int>(displayIndex) * (BaseMetrics::values.menuRowHeight + BaseMetrics::values.menuSpacing);

    const bool selected = selectedIndex == i;
    int tileWidth = rect.width - BaseMetrics::values.contentSidePadding * 2;
    if (totalPages > 1) {
      tileWidth -= 30;  // some margin for scroll arrows
    }

    if (selected) {
      renderer.fillRect(rect.x + BaseMetrics::values.contentSidePadding, tileY, tileWidth,
                        BaseMetrics::values.menuRowHeight);
    } else {
      renderer.drawRect(rect.x + BaseMetrics::values.contentSidePadding, tileY, tileWidth,
                        BaseMetrics::values.menuRowHeight);
    }

    std::string labelStr = buttonLabel(i);
    constexpr int buttonMenuTextMargin = 12;  // Keep text clear of the tile border on each side
    labelStr = renderer.truncatedText(UI_10_FONT_ID, labelStr.c_str(), tileWidth - buttonMenuTextMargin * 2);
    const char* label = labelStr.c_str();
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = rect.x + BaseMetrics::values.contentSidePadding + (tileWidth - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY =
        tileY + (BaseMetrics::values.menuRowHeight - lineHeight) / 2;  // vertically centered assuming y is top of text
    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, selectedIndex != i);
  }
}

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style popupFontFamily = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  // Scale y position proportionally to screen height
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int maxTextWidth = renderer.getScreenWidth() - marginX * 2 - frameThickness * 2;
  const auto truncatedMessage = renderer.truncatedText(UI_12_FONT_ID, message, maxTextWidth, popupFontFamily);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, truncatedMessage.c_str(), popupFontFamily);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + marginX * 2;
  const int h = textHeight + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  const bool useRoundedPopup = metrics.popupCornerRadius > 0;
  if (useRoundedPopup) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             metrics.popupCornerRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius, Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, true);
    renderer.fillRect(x, y, w, h, false);
  }

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + marginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(UI_12_FONT_ID, textX, textY, truncatedMessage.c_str(), metrics.popupTextInverted, popupFontFamily);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barHeight = metrics.popupProgressBarHeight;
  const int barWidth =
      std::max(0, layout.width - metrics.popupMarginX * 2);  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - metrics.popupMarginY / 2 - barHeight / 2 - 1;
  if (barWidth <= 0 || barHeight <= 0) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int scaledProgress = metrics.popupProgressClampPercent ? std::clamp(progress, 0, 100) : progress;
  const int fillWidth = barWidth * scaledProgress / 100;

  if (metrics.popupProgressDrawOutline) {
    renderer.drawRect(barX, barY, barWidth, barHeight, 1, metrics.popupProgressOutlineInverted);
  }
  if (fillWidth > 0) {
    renderer.fillRect(barX, barY, fillWidth, barHeight, metrics.popupProgressFillInverted);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string title, const int paddingBottom, const int textYOffset,
                              const bool isPageBookmarked, const char* timeLeftLabel, const bool darkMode,
                              const int bookWideCurrentPage, const int bookWideTotalPages) const {
  const bool foregroundBlack = !darkMode;
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  // Draw Progress Text
  const auto screenHeight = renderer.getScreenHeight();
  auto textY = screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom - 4;
  int progressTextWidth = 0;

  if (SETTINGS.statusBarBookProgressPercentage || SETTINGS.statusBarChapterPageCount) {
    // Right aligned text for progress counter
    char progressStr[32];

    // Mode 2 (whole-book estimate) needs the caller to have actually
    // supplied it - EpubReaderActivity does; TXT/XTC/the settings preview
    // screen don't, since a book-wide page count isn't a meaningful
    // concept for a single-file TXT or for XTC's own page model. Falling
    // back to the chapter-relative numbers in that case rather than
    // showing a blank/bogus counter.
    const bool useBookWide =
        SETTINGS.statusBarChapterPageCount == 2 && bookWideCurrentPage >= 0 && bookWideTotalPages > 0;
    const int displayCurrentPage = useBookWide ? bookWideCurrentPage : currentPage;
    const int displayPageCount = useBookWide ? bookWideTotalPages : pageCount;

    if (SETTINGS.statusBarBookProgressPercentage && SETTINGS.statusBarChapterPageCount) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", displayCurrentPage, displayPageCount, bookProgress);
    } else if (SETTINGS.statusBarBookProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", displayCurrentPage, displayPageCount);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(
        SMALL_FONT_ID,
        renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - orientedMarginRight - progressTextWidth, textY,
        progressStr, foregroundBlack);
  }

  // Draw Progress Bar
  if (SETTINGS.statusBarProgressBar != InkMODSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    const int progressBarMaxWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const int progressBarY = renderer.getScreenHeight() - orientedMarginBottom -
                             ((SETTINGS.statusBarProgressBarThickness + 1) * 2) - paddingBottom;
    size_t progress;
    if (SETTINGS.statusBarProgressBar == InkMODSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
      progress = static_cast<size_t>(bookProgress);
    } else {
      // Chapter progress
      progress = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) * 100 : 0;
    }
    const int barWidth = progressBarMaxWidth * progress / 100;
    renderer.fillRect(orientedMarginLeft, progressBarY, barWidth, ((SETTINGS.statusBarProgressBarThickness + 1) * 2),
                      foregroundBlack);
  }

  // Bookmark icon: drawn at the far left of the status bar when the current page is bookmarked.
  // Battery (and future left-side indicators) are offset to the right of it.
  static constexpr int bmIconW = 9;
  static constexpr int bmIconH = 14;
  static constexpr int bmIconGap = 4;
  static constexpr int bmNotchDepth = 5;
  static constexpr int statusItemGap = 8;
  const int leftClusterX = metrics.statusBarHorizontalMargin + orientedMarginLeft + 1;
  const int bmTotalWidth = isPageBookmarked ? (bmIconW + bmIconGap) : 0;

  if (isPageBookmarked) {
    const int bmX = leftClusterX;
    // +5 compensates for the battery nub drawn above the rect origin by drawBatteryLeft,
    // which shifts the battery body's visual center below the mathematical rect center.
    const int bmY = textY + (metrics.batteryHeight - bmIconH) / 2 + 5;
    renderer.fillRect(bmX, bmY, bmIconW, bmIconH, foregroundBlack);
    const int xNotch[3] = {bmX, bmX + bmIconW, bmX + bmIconW / 2};
    const int yNotch[3] = {bmY + bmIconH, bmY + bmIconH, bmY + bmIconH - bmNotchDepth};
    renderer.fillPolygon(xNotch, yNotch, 3, darkMode);
  }

  // Draw Battery
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == InkMODSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;
  int leftClusterWidth = bmTotalWidth;
  if (SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(renderer, Rect{leftClusterX + bmTotalWidth, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage, foregroundBlack);
    int batteryWidth = metrics.batteryWidth;
    if (showBatteryPercentage) {
      char batteryPercent[8];
      snprintf(batteryPercent, sizeof(batteryPercent), "%u%%",
               static_cast<unsigned>(powerManager.getBatteryPercentage()));
      batteryWidth += batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, batteryPercent);
    }
    leftClusterWidth += batteryWidth;
  }

  const bool hasTimeLeftLabel = timeLeftLabel != nullptr && timeLeftLabel[0] != '\0';
  if (hasTimeLeftLabel) {
    const bool hasLeftItem = leftClusterWidth > 0;
    const int timeLeftX = leftClusterX + leftClusterWidth + (hasLeftItem ? statusItemGap : 0);
    renderer.drawText(SMALL_FONT_ID, timeLeftX, textY, timeLeftLabel, foregroundBlack);
    const int timeLeftWidth = renderer.getTextWidth(SMALL_FONT_ID, timeLeftLabel);
    leftClusterWidth += (hasLeftItem ? statusItemGap : 0) + timeLeftWidth;
  }

  // Clock, in the reader specifically, when the user has it set to the
  // bottom - appended to this same left cluster (after battery/time-left)
  // rather than its own row across the top of the screen: a standalone
  // top row was tried first, but that took a whole line away from the
  // book text, which was worse than the thing being fixed. Sharing this
  // row costs nothing extra since leftClusterWidth already keeps the
  // (centered) title clear of whatever's actually in this cluster on a
  // given screen. The top-row placement (SETTINGS.readerClockAtBottom ==
  // 0, the default) is still drawn separately by the reader itself via
  // drawTopStatusBarClock(), same as before this setting existed.
  if (SETTINGS.readerClockAtBottom && SETTINGS.shouldShowClockInReader() && halClock.isAvailable()) {
    char clockBuf[9];
    if (halClock.formatTime(clockBuf, sizeof(clockBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      const bool hasLeftItem = leftClusterWidth > 0;
      const int clockX = leftClusterX + leftClusterWidth + (hasLeftItem ? statusItemGap : 0);
      renderer.drawText(SMALL_FONT_ID, clockX, textY, clockBuf, foregroundBlack);
      const int clockWidth = renderer.getTextWidth(SMALL_FONT_ID, clockBuf);
      leftClusterWidth += (hasLeftItem ? statusItemGap : 0) + clockWidth;
    }
  }

  // Draw Title
  if (!title.empty()) {
    textY -= textYOffset;
    // Centered chapter title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth =
        renderer.getScreenWidth() - (metrics.statusBarHorizontalMargin * 2) - orientedMarginLeft - orientedMarginRight;

    const int titleMarginLeft = leftClusterWidth + 30;
    const int titleMarginRight = progressTextWidth + 30;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;

    int titleWidth;
    titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    if (titleWidth > availableTitleSpace) {
      // Not enough space to center on the screen, center it within the remaining space instead
      availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
      titleMarginLeftAdjusted = titleMarginLeft;
    }
    if (titleWidth > availableTitleSpace) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + metrics.statusBarHorizontalMargin + orientedMarginLeft +
                          (availableTitleSpace - titleWidth) / 2,
                      textY, title.c_str(), foregroundBlack);
  }
}

void BaseTheme::drawTopStatusBarClock(const GfxRenderer& renderer, int topY, const char* previewTime,
                                      const bool readerContext, const int textYOffset, const bool darkMode,
                                      const int anchorRightX) const {
  if (!(readerContext ? SETTINGS.shouldShowClockInReader() : SETTINGS.shouldShowClockOutsideReader())) {
    return;
  }

  char timeBuf[9];
  const char* timeText = previewTime;
  if (timeText == nullptr) {
    if (!halClock.isAvailable()) {
      return;
    }
    if (!halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      return;
    }
    timeText = timeBuf;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int statusBarHeight = std::max(UITheme::getStatusBarHeight(), metrics.statusBarVerticalMargin);
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  (void)orientedMarginRight;
  (void)orientedMarginBottom;
  (void)orientedMarginLeft;

  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, timeText);
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int textX =
      anchorRightX >= 0 ? (anchorRightX - textWidth) : (renderer.getScreenWidth() - textWidth) / 2;
  const int effectiveTextYOffset = textYOffset + (readerContext ? homeHeaderClockTextYOffset(renderer) : 0);
  const int baseTopY = topY >= 0 ? topY : orientedMarginTop + metrics.topPadding;
  const int textY = baseTopY + (statusBarHeight - lineHeight) / 2 + effectiveTextYOffset;
  // Draw glyph-by-glyph rather than as one string. getTextWidth() above sums plain
  // per-glyph advances with no kerning, so drawing the same way keeps the centering
  // math and the actual pixels in agreement - and it sidesteps a handful of bad
  // kerning-table entries for specific digit pairs (observed: "3"+"4" and "3"+"5")
  // that otherwise pull the second glyph far enough left to visually vanish behind
  // the first (kerning only ever applies between two characters in the same
  // drawText() call, so single-character calls can't hit it). A live-updating clock
  // has no real typographic need for kerning anyway.
  int glyphX = textX;
  char glyphBuf[2] = {'\0', '\0'};
  for (const char* p = timeText; *p != '\0'; ++p) {
    glyphBuf[0] = *p;
    renderer.drawText(SMALL_FONT_ID, glyphX, textY, glyphBuf, !darkMode);
    glyphX += renderer.getTextWidth(SMALL_FONT_ID, glyphBuf);
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? metrics.textFieldCursorThickness : metrics.textFieldNormalThickness;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY,
                      rect.x + contentStartX + contentWidth + metrics.textFieldLineEndOffset, lineY, thickness, true);
  } else {
    const int lineW = textWidth + metrics.textFieldHorizontalPadding * 2;
    const int lineStart = rect.x + (rect.width - lineW) / 2;
    renderer.drawLine(lineStart, lineY, lineStart + lineW + metrics.textFieldLineEndOffset, lineY, thickness, true);
  }
}

void BaseTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                                const char* secondaryLabel, const KeyboardKeyType keyType,
                                const bool inactiveSelection) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int cr = metrics.keyboardKeyCornerRadius;
  const bool isSpecialKey = keyType == KeyboardKeyType::Shift || keyType == KeyboardKeyType::Mode ||
                            keyType == KeyboardKeyType::Del || keyType == KeyboardKeyType::Space ||
                            keyType == KeyboardKeyType::Ok || keyType == KeyboardKeyType::Disabled;

  if (isSelected) {
    if (inactiveSelection) {
      if (cr > 0) {
        renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::LightGray);
      } else {
        renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 2, true);
      }
    } else if (keyType == KeyboardKeyType::Disabled) {
      if (cr > 0) {
        renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::LightGray);
      } else {
        renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
      }
    } else {
      if (cr > 0) {
        renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::Black);
      } else {
        renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);
      }
    }
  } else {
    if (metrics.keyboardFillUnselected) {
      if (keyType == KeyboardKeyType::Disabled) {
        if (cr > 0) {
          renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::LightGray);
        } else {
          renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
        }
      } else {
        if (cr > 0) {
          renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::White);
        } else {
          renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
        }
      }
    }

    const bool shouldDrawOutline =
        (metrics.keyboardDrawSpecialOutlineWhenUnselected && isSpecialKey) || metrics.keyboardOutlineAllUnselected;
    if (shouldDrawOutline) {
      if (cr > 0) {
        renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, cr, true);
      } else {
        renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
      }
    }
  }

  const bool invert = isSelected && !inactiveSelection;

  if (keyType == KeyboardKeyType::Space) {
    const int lineHalfWidth = rect.width * 3 / 10;
    const int centerX = rect.x + rect.width / 2;
    const int lineY = rect.y + rect.height / 2 + 3;
    renderer.drawLine(centerX - lineHalfWidth, lineY, centerX + lineHalfWidth, lineY, 3, !invert);
    return;
  }

  if (keyType == KeyboardKeyType::Del) {
    const int centerX = rect.x + rect.width / 2;
    const int centerY = rect.y + rect.height / 2;
    const int arrowLen = rect.width / 4;
    const int arrowHead = std::max(metrics.keyboardMinArrowHeadSize, arrowLen / 2);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX + arrowLen / 2, centerY, 3, !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY - arrowHead, 3,
                      !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY + arrowHead, 3,
                      !invert);
    return;
  }

  if (label == nullptr || label[0] == '\0') {
    return;
  }

  const bool hasSecondary = secondaryLabel != nullptr && secondaryLabel[0] != '\0';
  const int primaryFontId = hasSecondary ? UI_10_FONT_ID : uiControlFontId();
  const int secondaryFontId = uiHintFontId();
  const int itemWidth = renderer.getTextWidth(primaryFontId, label);
  const int textX = rect.x + (rect.width - itemWidth) / 2;
  const int textY = rect.y + (rect.height - renderer.getLineHeight(primaryFontId)) / 2;

  renderer.drawText(primaryFontId, textX, textY, label, !invert);

  if (hasSecondary) {
    const int secWidth = renderer.getTextWidth(secondaryFontId, secondaryLabel);
    renderer.drawText(secondaryFontId, rect.x + rect.width - secWidth - metrics.keyboardSecondaryLabelRightPadding,
                      rect.y + metrics.keyboardSecondaryLabelTopPadding, secondaryLabel, !invert);
  }
}
