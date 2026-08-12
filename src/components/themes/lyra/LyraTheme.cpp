#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/chart.h"
#include "components/icons/cover.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/search24.h"
#include "components/icons/search32.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int maxListValueWidth = 240;
constexpr uint32_t mainMenuIconSize = 32;
constexpr uint32_t listIconSize = 24;
constexpr int mainMenuColumns = 2;
int coverWidth = 0;

int centeredRowY(const int rowY, const int rowHeight, const int contentHeight) {
  return rowY + std::max(0, rowHeight - contentHeight) / 2;
}

int mainMenuIconYOffset(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Chart:
      return -7;
    case UIIcon::Folder:
      return -4;
    case UIIcon::Recent:
      return -3;
    case UIIcon::Transfer:
      return -2;
    case UIIcon::Settings:
      return -2;
    case UIIcon::Library:
      return -4;
    default:
      return 0;
  }
}

}  // namespace

const uint8_t* LyraTheme::iconForName(UIIcon icon, uint32_t size) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder:
        return Folder24Icon;
      case UIIcon::Text:
        return Text24Icon;
      case UIIcon::Image:
        return Image24Icon;
      case UIIcon::Book:
        return Book24Icon;
      case UIIcon::File:
        return File24Icon;
      case UIIcon::Search:
        return Search24Icon;
      default:
        return nullptr;
    }
  } else if (size == 32) {
    switch (icon) {
      case UIIcon::Folder:
        return FolderIcon;
      case UIIcon::Book:
        return BookIcon;
      case UIIcon::Chart:
        return ChartIcon;
      case UIIcon::Search:
        return SearchIcon;
      case UIIcon::Recent:
        return RecentIcon;
      case UIIcon::Settings:
        return Settings2Icon;
      case UIIcon::Transfer:
        return TransferIcon;
      case UIIcon::Library:
        return LibraryIcon;
      case UIIcon::Wifi:
        return WifiIcon;
      case UIIcon::Hotspot:
        return HotspotIcon;
      default:
        return nullptr;
    }
  }
  return nullptr;
}

void LyraTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage,
                                const bool foregroundBlack) const {
  const bool charging = gpio.isUsbConnected();

  if (charging) {
    // Solid fill when charging so lightning bolt is visible
    renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4, foregroundBlack);
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2, !foregroundBlack);
  } else {
    if (percentage > 10) {
      renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4, foregroundBlack);
    }
    if (percentage > 40) {
      renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4, foregroundBlack);
    }
    if (percentage > 70) {
      renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4, foregroundBlack);
    }
  }
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                           const bool readerContext) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != InkMODSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - LyraMetrics::values.batteryWidth;
  const int batteryY = rect.y + homeHeaderTopInset;
  drawBatteryRight(renderer,
                   Rect{batteryX, batteryY, LyraMetrics::values.batteryWidth, LyraMetrics::values.batteryHeight},
                   showBatteryPercentage);

  // Уменьшаем отступ для заголовка, чтобы больше текста помещалось
  const int titleMaxWidth = rect.width - LyraMetrics::values.contentSidePadding * 2 - LyraMetrics::values.batteryWidth - 20;
  
  int maxTitleWidth = title != nullptr ? renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD) : 0;
  int maxSubtitleWidth =
      subtitle != nullptr ? renderer.getTextWidth(SMALL_FONT_ID, subtitle, EpdFontFamily::REGULAR) : 0;

  // Доступное пространство - ширина экрана минус отступы
  const int availableSpace = rect.width - LyraMetrics::values.contentSidePadding * 3 - LyraMetrics::values.batteryWidth - 20;

  // Если текст не помещается, используем меньший шрифт
  if (maxTitleWidth + maxSubtitleWidth > availableSpace) {
    // Проверяем, поместится ли заголовок с меньшим шрифтом
    int smallerTitleWidth = title != nullptr ? renderer.getTextWidth(UI_10_FONT_ID, title, EpdFontFamily::BOLD) : 0;
    if (smallerTitleWidth + maxSubtitleWidth <= availableSpace) {
      // Используем меньший шрифт для заголовка
      maxTitleWidth = smallerTitleWidth;
      if (title) {
        auto truncatedTitle = renderer.truncatedText(UI_10_FONT_ID, title, titleMaxWidth, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding,
                          rect.y + LyraMetrics::values.batteryBarHeight + 3, truncatedTitle.c_str(), true,
                          EpdFontFamily::BOLD);
      }
      if (subtitle) {
        auto truncatedSubtitle = renderer.truncatedText(SMALL_FONT_ID, subtitle, 
                                                         availableSpace - maxTitleWidth - 10, EpdFontFamily::REGULAR);
        int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
        renderer.drawText(SMALL_FONT_ID,
                          rect.x + rect.width - LyraMetrics::values.contentSidePadding - truncatedSubtitleWidth,
                          rect.y + 50, truncatedSubtitle.c_str(), true);
      }
      renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
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
      return;
    }
    
    // Стандартная обработка с обрезкой
    if ((maxTitleWidth > availableSpace / 2) && (maxSubtitleWidth > availableSpace / 2)) {
      maxTitleWidth = availableSpace / 2;
      maxSubtitleWidth = availableSpace / 2;
    } else {
      if (maxTitleWidth > maxSubtitleWidth) {
        maxTitleWidth = availableSpace - maxSubtitleWidth - 5;
      } else {
        maxSubtitleWidth = availableSpace - maxTitleWidth - 5;
      }
    }
  }

  if (title) {
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding,
                      rect.y + LyraMetrics::values.batteryBarHeight + 3, truncatedTitle.c_str(), true,
                      EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(SMALL_FONT_ID, subtitle, maxSubtitleWidth, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - LyraMetrics::values.contentSidePadding - truncatedSubtitleWidth,
                      rect.y + 50, truncatedSubtitle.c_str(), true);
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

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;
  int rightSpace = LyraMetrics::values.contentSidePadding;
  
  // Проверяем длину правой метки
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - LyraMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + hPaddingInSelection;
  }

  // Доступная ширина для основного текста
  int availableWidth = rect.width - LyraMetrics::values.contentSidePadding - rightSpace - 10;
  
  // Если текст не помещается, используем меньший шрифт
  int fontId = UI_10_FONT_ID;
  int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::REGULAR);
  if (labelWidth > availableWidth) {
    // Пробуем еще меньший шрифт
    fontId = SMALL_FONT_ID;
  }

  auto truncatedLabel = renderer.truncatedText(fontId, label, availableWidth, EpdFontFamily::REGULAR);
  renderer.drawText(fontId, currentX, rect.y + 6, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;

  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  }

  // Рассчитываем общую ширину всех вкладок
  int totalWidth = 0;
  for (const auto& tab : tabs) {
    int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);
    totalWidth += textWidth + 2 * hPaddingInSelection + LyraMetrics::values.tabSpacing;
  }

  const int availableWidth = rect.width - 2 * LyraMetrics::values.contentSidePadding;

  // Если вкладки не помещаются, уменьшаем отступы
  int padding = hPaddingInSelection;
  bool overflows = totalWidth > availableWidth;
  if (overflows) {
    padding = 4; // Минимальный отступ
  }

  // Если даже с минимальным отступом вкладки не помещаются, обрезаем текст каждой вкладки,
  // чтобы длинный перевод не вылезал за пределы полосы вкладок.
  const int minTotalWidth = std::accumulate(tabs.begin(), tabs.end(), 0, [&](const int total, const TabInfo& tab) {
    return total + renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR) + 2 * padding +
           LyraMetrics::values.tabSpacing;
  });
  const int perTabMaxWidth =
      tabs.empty() ? 0 : availableWidth / static_cast<int>(tabs.size()) - 2 * padding - LyraMetrics::values.tabSpacing;
  const bool needsTruncation = minTotalWidth > availableWidth && perTabMaxWidth > 0;

  for (const auto& tab : tabs) {
    std::string label = needsTruncation
                            ? renderer.truncatedText(UI_10_FONT_ID, tab.label, perTabMaxWidth, EpdFontFamily::REGULAR)
                            : tab.label;
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label.c_str(), EpdFontFamily::REGULAR);

    if (tab.selected) {
      if (selected) {
        renderer.fillRoundedRect(currentX, rect.y + 1, textWidth + 2 * padding, rect.height - 4,
                                 cornerRadius, Color::Black);
      } else {
        renderer.fillRectDither(currentX, rect.y, textWidth + 2 * padding, rect.height - 3,
                                Color::LightGray);
        renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + textWidth + 2 * padding,
                          rect.y + rect.height - 3, 2, true);
      }
    }

    renderer.drawText(UI_10_FONT_ID, currentX + padding, rect.y + 6, label.c_str(), !(tab.selected && selected),
                      EpdFontFamily::REGULAR);

    currentX += textWidth + LyraMetrics::values.tabSpacing + 2 * padding;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed,
                         const std::function<bool(int index)>& isHeader) const {
  drawListWithMetrics(renderer, rect, itemCount, selectedIndex, rowTitle, rowSubtitle, rowIcon, rowValue,
                      highlightValue, rowDimmed, isHeader, LyraMetrics::values, false);
}

void LyraTheme::drawListWithMetrics(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                    const std::function<std::string(int index)>& rowTitle,
                                    const std::function<std::string(int index)>& rowSubtitle,
                                    const std::function<UIIcon(int index)>& rowIcon,
                                    const std::function<std::string(int index)>& rowValue, bool highlightValue,
                                    const std::function<bool(int index)>& rowDimmed,
                                    const std::function<bool(int index)>& isHeader, const ThemeMetrics& metrics,
                                    const bool invertSelectedRows) const {
  if (itemCount <= 0) return;

  // See BaseTheme::drawList(): simple option lists provide an empty subtitle
  // callback, which must not turn the entire list into tall two-line rows.
  const int subtitleProbeIndex = std::clamp(selectedIndex, 0, itemCount - 1);
  const bool hasSubtitle = rowSubtitle != nullptr && !rowSubtitle(subtitleProbeIndex).empty();
  // Row height and the subtitle's offset (itemY + 30 below) were both
  // fixed constants sized for the normal 10pt UI font - see BaseTheme.cpp's
  // matching fix for why Accessibility's "Increase interface text" setting
  // (swaps UI_10_FONT_ID for a taller 12pt family) broke that assumption.
  // titleLineHeight is normally computed further down in this function;
  // pulled up here since rowHeight needs it too.
  const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int kSubtitleTopOffset = 7;  // matches titleY's own itemY+7 when a subtitle is present
  constexpr int kTitleSubtitleGap = 4;
  constexpr int kSubtitleBottomPadding = 6;
  const int subtitleOffsetY = kSubtitleTopOffset + titleLineHeight + kTitleSubtitleGap;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  if (hasSubtitle) {
    const int subtitleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    rowHeight = std::max(rowHeight, subtitleOffsetY + subtitleLineHeight + kSubtitleBottomPadding);
  }
  const auto isHeaderRow = [&isHeader](int index) { return isHeader != nullptr && isHeader(index); };
  const int sectionHeaderTopPadding = halTiltSensor.isAvailable() ? 10 : 20;
  constexpr int sectionHeaderFontId = UI_10_FONT_ID;
  constexpr int sectionHeaderUnderlineGap = 4;
  const int sectionHeaderLineHeight = renderer.getLineHeight(sectionHeaderFontId);
  const int sectionHeaderRowHeight = sectionHeaderLineHeight + sectionHeaderUnderlineGap;
  const auto visualRowHeight = [&](int index) { return isHeaderRow(index) ? sectionHeaderRowHeight : rowHeight; };
  int totalContentHeight = 0;
  for (int i = 0; i < itemCount; ++i) {
    if (i > 0 && isHeaderRow(i)) totalContentHeight += sectionHeaderTopPadding;
    totalContentHeight += visualRowHeight(i);
  }
  const bool contentFits = totalContentHeight <= rect.height;
  int pageItems = contentFits ? itemCount : rect.height / rowHeight;
  if (pageItems <= 0) pageItems = 1;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (!contentFits && totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - metrics.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - metrics.scrollBarWidth, scrollBarY, metrics.scrollBarWidth, scrollBarHeight, true);
  }

  // Draw selection (skip header rows)
  int contentWidth = rect.width - (totalPages > 1 ? (metrics.scrollBarWidth + metrics.scrollBarRightOffset) : 1);
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  if (selectedIndex >= 0 && !isHeaderRow(selectedIndex)) {
    int selY = rect.y;
    for (int j = pageStartIndex; j < selectedIndex; j++) {
      selY += visualRowHeight(j);
      if (isHeaderRow(j + 1)) selY += sectionHeaderTopPadding;
    }
    renderer.fillRoundedRect(rect.x + metrics.contentSidePadding, selY, contentWidth - metrics.contentSidePadding * 2,
                             rowHeight, cornerRadius, invertSelectedRows ? Color::Black : Color::LightGray);
  }

  int textX = rect.x + metrics.contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - metrics.contentSidePadding * 2 - hPaddingInSelection * 2;
  uint32_t iconSize;
  if (rowIcon != nullptr) {
    iconSize = hasSubtitle ? mainMenuIconSize : listIconSize;
    textX += iconSize + hPaddingInSelection;
    textWidth -= iconSize + hPaddingInSelection;
  }

  // Draw all items using a running Y to accommodate variable-height section headers.
  int currentY = rect.y;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    if (i > pageStartIndex && isHeaderRow(i)) currentY += sectionHeaderTopPadding;
    const int itemY = currentY;
    const int currentRowHeight = visualRowHeight(i);
    currentY += currentRowHeight;
    const bool selectedRow = i == selectedIndex;
    const bool foreground = !(invertSelectedRows && selectedRow);

    if (isHeaderRow(i)) {
      // Section header: bold uppercase label + divider line below
      std::string label = rowTitle(i);
      std::transform(label.begin(), label.end(), label.begin(),
                     [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      auto truncated = renderer.truncatedText(sectionHeaderFontId, label.c_str(),
                                              contentWidth - metrics.contentSidePadding * 2, EpdFontFamily::BOLD);
      const int headerTextY = itemY;
      renderer.drawText(sectionHeaderFontId, rect.x + metrics.contentSidePadding, headerTextY, truncated.c_str(), true,
                        EpdFontFamily::BOLD);
      renderer.drawLine(rect.x, itemY + currentRowHeight - 1, rect.x + contentWidth, itemY + currentRowHeight - 1,
                        true);
      continue;
    }

    int rowTextWidth = textWidth;

    // Reserve the marker's complete pill before measuring the title. The old
    // code reserved only the letters but painted a full-row black block, which
    // made the current-setting marker look as if it had drifted into the row.
    int valueTextWidth = 0;
    int valuePillWidth = 0;
    std::string valueText = "";
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxListValueWidth);
        valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
        valuePillWidth = valueTextWidth + hPaddingInSelection * 2;
        rowTextWidth = std::max(0, rowTextWidth - valuePillWidth - hPaddingInSelection);
      }
    }

    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    const int titleY = hasSubtitle ? itemY + 7 : centeredRowY(itemY, currentRowHeight, titleLineHeight);
    renderer.drawText(UI_10_FONT_ID, textX, titleY, item.c_str(), foreground);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && !selectedRow) {
      const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, item.c_str());
      for (int py = titleY; py < titleY + titleLineHeight; py++)
        for (int px = textX; px < textX + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, iconSize);
      if (iconBitmap != nullptr) {
        const int iconX = rect.x + metrics.contentSidePadding + hPaddingInSelection;
        const int iconY =
            hasSubtitle ? itemY + 16 : centeredRowY(itemY, currentRowHeight, static_cast<int>(iconSize));
        if (invertSelectedRows && selectedRow) {
          renderer.drawIconInverted(iconBitmap, iconX, iconY, iconSize, iconSize);
        } else {
          renderer.drawIcon(iconBitmap, iconX, iconY, iconSize, iconSize);
        }
      }
    }

    if (hasSubtitle) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      renderer.drawText(SMALL_FONT_ID, textX, itemY + subtitleOffsetY, subtitle.c_str(), foreground);
    }

    // Draw value
    if (!valueText.empty()) {
      // Keep the "Selected" pill inside the selection highlight. Its former
      // right edge coincided with the highlight edge, making it look clipped
      // on real panels even at the normal interface size.
      const int valueX =
          rect.x + contentWidth - metrics.contentSidePadding - hPaddingInSelection - valuePillWidth;
      const int valueY = centeredRowY(itemY, currentRowHeight, titleLineHeight);
      if (selectedRow && highlightValue) {
        const int pillHeight = std::min(currentRowHeight - 4, titleLineHeight + 8);
        const int pillY = itemY + (currentRowHeight - pillHeight) / 2;
        renderer.fillRoundedRect(valueX, pillY, valuePillWidth, pillHeight, cornerRadius, Color::Black);
      }

      const bool valueForeground = invertSelectedRows ? !selectedRow : !(selectedRow && highlightValue);
      renderer.drawText(UI_10_FONT_ID, valueX + hPaddingInSelection, valueY, valueText.c_str(), valueForeground);
    }
  }
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4, const bool allowInvertedText) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  const bool invertText = allowInvertedText && orig_orientation == GfxRenderer::Orientation::PortraitInverted;
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = LyraMetrics::values.buttonHintsHeight;
  constexpr int buttonY = LyraMetrics::values.buttonHintsHeight;
  constexpr int textYOffset = 7;
  
  const char* labels[] = {btn1, btn2, btn3, btn4};

  // Fill the row edge-to-edge (minus a small fixed margin) instead of only
  // growing buttons to fit their text: on a wide screen, capping width at a
  // small fixed max left buttons bunched in the middle with a lot of empty
  // margin on both sides, which doesn't match how a button row is supposed
  // to read - four buttons occupying nearly the full width with a small,
  // even margin and small, even gaps.
  constexpr int outerMargin = 12;
  constexpr int gap = 12;
  const int screenWidth = renderer.getScreenWidth();
  int adaptiveButtonWidth = (screenWidth - 2 * outerMargin - 3 * gap) / 4;
  adaptiveButtonWidth = std::max(adaptiveButtonWidth, 60);

  const int totalButtonsWidth = adaptiveButtonWidth * 4 + 3 * gap;
  const int startX = (screenWidth - totalButtonsWidth) / 2;

  // Calculate positions
  int buttonPos[4];
  for (int i = 0; i < 4; i++) {
    buttonPos[i] = startX + i * (adaptiveButtonWidth + gap);
  }

  for (int i = 0; i < 4; i++) {
    const int x = buttonPos[i];
    const int btnWidth = adaptiveButtonWidth;
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // Draw the filled background and border for a FULL-sized button
      renderer.fillRoundedRect(x, pageHeight - buttonY, btnWidth, buttonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, pageHeight - buttonY, btnWidth, buttonHeight, 1, cornerRadius, true, true, false,
                               false, true);
    } else {
      // Draw the filled background and border for a SMALL-sized button
      const int smallButtonY = pageHeight - smallButtonHeight;
      renderer.fillRoundedRect(x, smallButtonY, btnWidth, smallButtonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, smallButtonY, btnWidth, smallButtonHeight, 1, cornerRadius, true, true, false,
                               false, true);
    }
  }

  renderer.setOrientation(invertText ? GfxRenderer::Orientation::PortraitInverted : GfxRenderer::Orientation::Portrait);
  const int textY = invertText ? textYOffset : pageHeight - buttonY + textYOffset;

  constexpr int buttonHintsTextMargin = 8;  // Keep text clear of the button border on each side
  for (int i = 0; i < 4; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int idx = invertText ? 3 - i : i;
      const int x = buttonPos[idx];
      const int btnWidth = adaptiveButtonWidth;
      auto label = renderer.truncatedText(SMALL_FONT_ID, labels[i], btnWidth - buttonHintsTextMargin * 2);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
      const int textX = x + (btnWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, textY, label.c_str());
    }
  }

  renderer.setOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;
  constexpr int buttonHeight = 78;
  constexpr int buttonMargin = 0;

  // Text runs along the button's vertical extent once rotated, so clamp to buttonHeight (minus a small margin)
  // rather than letting a long translated label spill past the button edges.
  constexpr int sideButtonTextMargin = 6;
  constexpr int sideButtonMaxTextLen = buttonHeight - sideButtonTextMargin * 2;

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(buttonMargin, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, false, true, false,
                               true, true);
      auto label = renderer.truncatedText(SMALL_FONT_ID, topBtn, sideButtonMaxTextLen);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
      renderer.drawTextRotated90CW(SMALL_FONT_ID, buttonMargin, x3ButtonY + (buttonHeight + textWidth) / 2,
                                   label.c_str());
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonWidth;
      renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
      auto label = renderer.truncatedText(SMALL_FONT_ID, bottomBtn, sideButtonMaxTextLen);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
      renderer.drawTextRotated90CW(SMALL_FONT_ID, rightX, x3ButtonY + (buttonHeight + textWidth) / 2, label.c_str());
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                               false, true, false, true);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topHintButtonY + (i * buttonHeight) + 5;
        auto label = renderer.truncatedText(SMALL_FONT_ID, labels[i], sideButtonMaxTextLen);
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
        renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, label.c_str());
      }
    }
  }
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                    const BookReadingStats* stats, float progressPercent) const {
  const int tileWidth = rect.width - 2 * LyraMetrics::values.contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = LyraMetrics::values.homeCoverHeight * 0.6;
  }

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      int tileX = LyraMetrics::values.contentSidePadding;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, LyraMetrics::values.homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                                LyraMetrics::values.homeCoverHeight);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                        LyraMetrics::values.homeCoverHeight, true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + hPaddingInSelection,
                          tileY + hPaddingInSelection + (LyraMetrics::values.homeCoverHeight / 3), coverWidth,
                          2 * LyraMetrics::values.homeCoverHeight / 3, true);
        renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;
    }

    bool bookSelected = (selectorIndex == 0);

    int tileX = LyraMetrics::values.contentSidePadding;
    int textWidth = tileWidth - 2 * hPaddingInSelection - LyraMetrics::values.verticalSpacing - coverWidth;

    if (bookSelected) {
      // Draw selection box
      renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                               Color::LightGray);
      renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                              LyraMetrics::values.homeCoverHeight, Color::LightGray);
      renderer.fillRectDither(tileX + hPaddingInSelection + coverWidth, tileY + hPaddingInSelection,
                              tileWidth - hPaddingInSelection - coverWidth, LyraMetrics::values.homeCoverHeight,
                              Color::LightGray);
      renderer.fillRoundedRect(tileX, tileY + LyraMetrics::values.homeCoverHeight + hPaddingInSelection, tileWidth,
                               hPaddingInSelection, cornerRadius, false, false, true, true, Color::LightGray);
    }

    // Определяем оптимальный шрифт для русского текста
    int titleFontId = UI_12_FONT_ID;
    int titleMaxLines = 3;
    int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, book.title.c_str(), EpdFontFamily::BOLD);
    if (titleWidth > textWidth * 2) {
      titleFontId = UI_10_FONT_ID;
      titleMaxLines = 4;
    }

    auto titleLines = renderer.wrappedText(titleFontId, book.title.c_str(), textWidth, titleMaxLines, EpdFontFamily::BOLD);

    auto author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textWidth);
    const int titleLineHeight = renderer.getLineHeight(titleFontId);
    const int statsLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int progressLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
    const int authorHeight = book.author.empty() ? 0 : (renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2);
    const bool hasStats = (stats != nullptr && stats->sessionCount > 0);
    const bool hasProgress = progressPercent >= 0.0f;
    const int statsBlockHeight = hasStats ? (statsLineHeight * 2 + 6) : 0;
    const int progressBlockHeight = hasProgress ? (progressLineHeight + 12) : 0;
    const int totalBlockHeight = titleBlockHeight + authorHeight + statsBlockHeight + progressBlockHeight;
    int titleY = tileY + tileHeight / 2 - totalBlockHeight / 2;
    const int textX = tileX + hPaddingInSelection + coverWidth + LyraMetrics::values.verticalSpacing;
    
    for (const auto& line : titleLines) {
      renderer.drawText(titleFontId, textX, titleY, line.c_str(), true, EpdFontFamily::BOLD);
      titleY += titleLineHeight;
    }
    if (!book.author.empty()) {
      titleY += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, titleY, author.c_str(), true);
      titleY += renderer.getLineHeight(UI_10_FONT_ID);
    }
    if (hasStats) {
      titleY += 6;
      char buf[48];
      char statLine[64];
      BookReadingStats::formatDuration(stats->totalReadingSeconds, buf, sizeof(buf));
      snprintf(statLine, sizeof(statLine), "%s%s", tr(STR_STATS_TOTAL_TIME), buf);
      renderer.drawText(SMALL_FONT_ID, textX, titleY, statLine, true);
      titleY += statsLineHeight;
      BookReadingStats::formatDuration(stats->totalReadingSeconds / stats->sessionCount, buf, sizeof(buf));
      snprintf(statLine, sizeof(statLine), "%s%s", tr(STR_STATS_AVG_SESSION), buf);
      renderer.drawText(SMALL_FONT_ID, textX, titleY, statLine, true);
      titleY += statsLineHeight;
    }
    if (hasProgress) {
      titleY += 8;
      constexpr int progressBarHeight = 4;
      const int progressBarWidth = textWidth;
      const int progressBarY = titleY + progressLineHeight + 2;
      const int filledWidth =
          std::clamp(static_cast<int>((progressPercent / 100.0f) * progressBarWidth), 0, progressBarWidth);
      char progressLabel[16];
      snprintf(progressLabel, sizeof(progressLabel), "%.0f%%", progressPercent);
      renderer.drawText(UI_10_FONT_ID, textX, titleY, progressLabel, true, EpdFontFamily::BOLD);
      renderer.drawRect(textX, progressBarY, progressBarWidth, progressBarHeight, true);
      if (filledWidth > 0) {
        renderer.fillRect(textX + 1, progressBarY + 1, std::max(0, filledWidth - 2),
                          std::max(0, progressBarHeight - 2));
      }
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

void LyraTheme::drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const {
  constexpr int padding = 48;
  renderer.drawText(UI_12_FONT_ID, rect.x + padding,
                    rect.y + rect.height / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 2, tr(STR_NO_OPEN_BOOK), true,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rect.x + padding, rect.y + rect.height / 2 + 2, tr(STR_START_READING), true);
}

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  const auto& menuMetrics = UITheme::getInstance().getMetrics();

  constexpr int maxVisibleItems = 7;
  const int pageItems = maxVisibleItems;
  const int totalPages = (buttonCount + pageItems - 1) / pageItems;

  if (totalPages > 1) {
    const int scrollAreaHeight =
        maxVisibleItems * (menuMetrics.menuRowHeight + menuMetrics.menuSpacing) - menuMetrics.menuSpacing;
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / buttonCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - LyraMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - LyraMetrics::values.scrollBarWidth, scrollBarY, LyraMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  const int pageStartIndex = (selectedIndex / pageItems) * pageItems;

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const int displayIndex = i - pageStartIndex;
    int tileWidth = rect.width - menuMetrics.contentSidePadding * 2;
    if (totalPages > 1) {
      tileWidth -= (LyraMetrics::values.scrollBarWidth + LyraMetrics::values.scrollBarRightOffset);
    }
    Rect tileRect = Rect{rect.x + menuMetrics.contentSidePadding,
                         rect.y + displayIndex * (menuMetrics.menuRowHeight + menuMetrics.menuSpacing), tileWidth,
                         menuMetrics.menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, Color::LightGray);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (menuMetrics.menuRowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      if (icon == UIIcon::BookmarkIcon) {
        // Draw a small bookmark ribbon icon to match the status bar ribbon.
        const int ribbonWidth = 16;
        const int ribbonHeight = 22;
        const int notchSize = 6;
        // Center the ribbon horizontally within the mainMenuIconSize box
        const int iconX = textX + (mainMenuIconSize - ribbonWidth) / 2;
        const int iconY = textY + 4;
        const int centerX = iconX + ribbonWidth / 2;

        const int polyX[5] = {iconX, iconX + ribbonWidth, iconX + ribbonWidth, centerX, iconX};
        const int polyY[5] = {iconY, iconY, iconY + ribbonHeight, iconY + ribbonHeight - notchSize,
                              iconY + ribbonHeight};
        renderer.fillPolygon(polyX, polyY, 5, true);
        textX += mainMenuIconSize + hPaddingInSelection + 2;
      } else {
        const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
        if (iconBitmap != nullptr) {
          renderer.drawIcon(iconBitmap, textX, textY + 3 + mainMenuIconYOffset(icon), mainMenuIconSize,
                            mainMenuIconSize);
          textX += mainMenuIconSize + hPaddingInSelection + 2;
        }
      }
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
  }
}
