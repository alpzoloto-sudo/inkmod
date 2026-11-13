#include "IntervalSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
void formatCompactSeconds(const int seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%d %s", seconds, tr(STR_UNIT_SEC_SHORT));
  } else if (seconds % 60 == 0) {
    snprintf(buf, len, "%d %s", seconds / 60, tr(STR_UNIT_MIN_SHORT));
  } else {
    snprintf(buf, len, "%d %s %d %s", seconds / 60, tr(STR_UNIT_MIN_SHORT), seconds % 60, tr(STR_UNIT_SEC_SHORT));
  }
}
}  // namespace

int IntervalSelectionActivity::clampedValue(const int candidate) const {
  return std::clamp(candidate, minValue, maxValue);
}

void IntervalSelectionActivity::onEnter() {
  Activity::onEnter();
  value = clampedValue(value);
  requestUpdate();
}

void IntervalSelectionActivity::adjustValue(const int delta) {
  value = clampedValue(value + delta);
  requestUpdate();
}

void IntervalSelectionActivity::loop() {
  if (mappedInput.hasTouch()) {
    const int screenWidth = renderer.getScreenWidth();
    const int screenHeight = renderer.getScreenHeight();
    const int barWidth = std::min(360, std::max(0, screenWidth - 40));
    const int barX = std::max(0, (screenWidth - barWidth) / 2);
    constexpr int barY = 140;

    // Drag anywhere around the visible slider. The whole 56px band is active,
    // not just the thin 16px track. Values snap to the configured small step.
    int tx = 0, ty = 0;
    if (mappedInput.isScreenTouchHeld(tx, ty) && ty >= barY - 20 && ty <= barY + 36 && barWidth > 4) {
      const int clampedX = std::clamp(tx, barX + 2, barX + barWidth - 2);
      const int range = std::max(1, maxValue - minValue);
      int candidate = minValue + ((clampedX - (barX + 2)) * range + (barWidth - 4) / 2) / (barWidth - 4);
      const int step = std::max(1, smallStep);
      candidate = minValue + ((candidate - minValue + step / 2) / step) * step;
      candidate = clampedValue(candidate);
      if (candidate != value) {
        value = candidate;
        requestUpdate();
      }
      return;
    }

    // Large touch actions at the bottom: cancel on the left, save on the right.
    if (mappedInput.wasScreenTapped(tx, ty) && ty >= screenHeight - 86) {
      if (tx < screenWidth / 2) {
        ActivityResult result;
        result.isCancelled = true;
        setResult(std::move(result));
      } else {
        setResult(IntervalResult{static_cast<uint32_t>(value)});
      }
      finish();
      return;
    }

    // Touch UX: after changing a value, the edge Back gesture commits it.
    // Physical Back keeps the legacy cancel behaviour below.
    if (mappedInput.wasBackGesture()) {
      setResult(IntervalResult{static_cast<uint32_t>(value)});
      finish();
      return;
    }
  }

  if (ignoreConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(IntervalResult{static_cast<uint32_t>(value)});
    finish();
    return;
  }

  if (mappedInput.hasTouch()) {
    // X4 Pro has two physical side keys. They are exposed both as the raw
    // Up/Down pair and, through the configured side-button layout, as
    // PageBack/PageForward. Handling both interpretations in the same picker
    // made one short press apply a 10% large step and an opposing 1% small
    // step (net 9%). On touch devices the side keys are precision controls:
    // consume only their PageBack/PageForward mapping.
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::PageBack},
                                         [this] { adjustValue(-smallStep); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::PageForward},
                                         [this] { adjustValue(smallStep); });
  } else {
    // On X4 the logical PageBack/PageForward keys can map to the SAME physical
    // side buttons as Up/Down. Including both here applied an extra +/-10 s
    // small step alongside the +/-60 s large step (net 50 s).
    // The front Left/Right keys own fine adjustment; side Up/Down own large.
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                         [this] { adjustValue(-smallStep); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                         [this] { adjustValue(smallStep); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustValue(largeStep); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustValue(-largeStep); });
  }
}

void IntervalSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, screenWidth, metrics.headerHeight}, I18N.get(titleId), nullptr,
                 readerActivity);

  char formattedValue[32];
  if (maxBoundaryLabelId != StrId::STR_NONE_OPT && value == maxValue) {
    snprintf(formattedValue, sizeof(formattedValue), "%s", I18N.get(maxBoundaryLabelId));
  } else if (showPercentValue) {
    snprintf(formattedValue, sizeof(formattedValue), "%d%%", value);
  } else if (valueFormatId == StrId::STR_SECONDS_VALUE_FORMAT) {
    formatCompactSeconds(value, formattedValue, sizeof(formattedValue));
  } else if (valueFormatId != StrId::STR_NONE_OPT) {
    snprintf(formattedValue, sizeof(formattedValue), I18N.get(valueFormatId), static_cast<unsigned int>(value));
  } else {
    snprintf(formattedValue, sizeof(formattedValue), "%d", value);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, 90, formattedValue, true, EpdFontFamily::BOLD);

  const int barWidth = std::min(360, std::max(0, screenWidth - 40));
  constexpr int barHeight = 16;
  const int barX = std::max(0, (screenWidth - barWidth) / 2);
  const int barY = 140;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  const int range = std::max(1, maxValue - minValue);
  const int fillWidth = (barWidth - 4) * (value - minValue) / range;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  const int knobX = std::max(barX + 2, barX + 2 + fillWidth - 2);
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  // On touch-only X4 Pro the old hint mentioned Left/Right/Up/Down buttons
  // that do not exist. The side buttons still provide precise +/- smallStep.
  if (!mappedInput.hasTouch()) {
    renderer.drawCenteredText(SMALL_FONT_ID, barY + 30, I18N.get(stepHintId), true);
  } else if (showPercentValue) {
    renderer.drawCenteredText(SMALL_FONT_ID, barY + 30, I18N.get(StrId::STR_SIDE_BUTTON_PERCENT_HINT), true);
  }

  if (mappedInput.hasTouch()) {
    const int screenHeight = renderer.getScreenHeight();
    const int margin = 14;
    const int gap = 12;
    const int buttonY = screenHeight - 72;
    const int buttonH = 54;
    const int buttonW = (screenWidth - margin * 2 - gap) / 2;
    const int leftX = margin;
    const int rightX = leftX + buttonW + gap;
    renderer.drawRect(leftX, buttonY, buttonW, buttonH);
    renderer.drawRect(rightX, buttonY, buttonW, buttonH);
    const int cancelW = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_CANCEL));
    renderer.drawText(UI_10_FONT_ID, leftX + std::max(0, (buttonW - cancelW) / 2), buttonY + 15, tr(STR_CANCEL));
    const int confirmW = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_CONFIRM));
    renderer.drawText(UI_10_FONT_ID, rightX + std::max(0, (buttonW - confirmW) / 2), buttonY + 15, tr(STR_CONFIRM));
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, readerActivity);
  }

  renderer.displayBuffer();
}
