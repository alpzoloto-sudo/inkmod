#include "HeaderDate.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>

#include <algorithm>
#include <cstddef>

#include "InkMODSettings.h"
#include "components/themes/BaseTheme.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kHeaderDateRightInset = 12;
constexpr int kHeaderDateBottomGap = 10;

bool formatHeaderDate(char* buf, const size_t len) {
  if (!gpio.deviceIsX3()) return false;
  if (!SETTINGS.clockDateHasBeenSynced) return false;
  return halClock.formatDate(buf, len, SETTINGS.clockUtcOffsetQ);
}
}  // namespace

int headerDateReservedWidth(const GfxRenderer& renderer) {
  char dateBuf[13];
  if (!formatHeaderDate(dateBuf, sizeof(dateBuf))) return 0;

  return renderer.getTextWidth(UI_10_FONT_ID, dateBuf) + kHeaderDateRightInset;
}

int headerDateLineBottomY(const GfxRenderer&, const ThemeMetrics& metrics, const int headerHeight) {
  const int effectiveHeaderHeight = headerHeight >= 0 ? headerHeight : metrics.headerHeight;
  return metrics.topPadding + effectiveHeaderHeight - kHeaderDateBottomGap;
}

void drawHeaderDate(const GfxRenderer& renderer, const int pageWidth, const ThemeMetrics& metrics,
                    const int headerHeight) {
  drawHeaderDateAtLineBottom(renderer, pageWidth, headerDateLineBottomY(renderer, metrics, headerHeight));
}

void drawHeaderDateAtLineBottom(const GfxRenderer& renderer, const int pageWidth, const int lineBottomY) {
  char dateBuf[13];
  if (!formatHeaderDate(dateBuf, sizeof(dateBuf))) return;

  constexpr int dateFontId = UI_10_FONT_ID;
  const int textWidth = renderer.getTextWidth(dateFontId, dateBuf);
  const int dateX = pageWidth - kHeaderDateRightInset - textWidth;
  const int dateY = lineBottomY - renderer.getLineHeight(dateFontId);
  renderer.drawText(dateFontId, std::max(0, dateX), std::max(0, dateY), dateBuf);
}


bool drawHeaderDateBeforeClock(const GfxRenderer& renderer, const Rect& headerRect, const ThemeMetrics& metrics) {
  char dateBuf[13];
  if (!formatHeaderDate(dateBuf, sizeof(dateBuf))) return false;
  if (!halClock.isAvailable() || !SETTINGS.shouldShowClockOutsideReader()) return false;

  char timeBuf[9];
  if (!halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) return false;

  constexpr int rightInset = 12;
  constexpr int batteryPercentSpacing = 4;
  constexpr int clockBatteryGap = 14;
  constexpr int dateClockGap = 8;
  constexpr int topStatusY = 2;  // Same homeHeaderTopInset used by BaseTheme.
  constexpr int fontId = SMALL_FONT_ID;

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != InkMODSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  int batteryClusterLeftX = headerRect.x + headerRect.width - rightInset - metrics.batteryWidth;
  if (showBatteryPercentage) {
    batteryClusterLeftX -= renderer.getTextWidth(fontId, "100%") + batteryPercentSpacing;
  }

  const int clockRightX = batteryClusterLeftX - clockBatteryGap;
  const int clockWidth = renderer.getTextWidth(fontId, timeBuf);
  const int clockX = clockRightX - clockWidth;
  const int dateWidth = renderer.getTextWidth(fontId, dateBuf);
  const int dateX = std::max(headerRect.x, clockX - dateClockGap - dateWidth);
  const int dateY = headerRect.y + topStatusY;

  renderer.drawText(fontId, dateX, dateY, dateBuf);
  return true;
}
