#pragma once

#include <InkMODSettings.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace ReaderUtils {

constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
constexpr uint8_t STATUS_BAR_TEXT_PADDING = 3;

inline GfxRenderer::Orientation toRendererOrientation(const uint8_t orientation) {
  switch (orientation) {
    case InkMODSettings::ORIENTATION::PORTRAIT:
      return GfxRenderer::Orientation::Portrait;
    case InkMODSettings::ORIENTATION::LANDSCAPE_CW:
      return GfxRenderer::Orientation::LandscapeClockwise;
    case InkMODSettings::ORIENTATION::INVERTED:
      return GfxRenderer::Orientation::PortraitInverted;
    case InkMODSettings::ORIENTATION::LANDSCAPE_CCW:
      return GfxRenderer::Orientation::LandscapeCounterClockwise;
    default:
      return GfxRenderer::Orientation::Portrait;
  }
}

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  renderer.setOrientation(toRendererOrientation(orientation));
}

inline bool shouldShowTopClockStatusBar() {
  // With the clock folded into the bottom status bar, the page must not
  // reserve an unused header row above its text or illustrations.
  return !SETTINGS.readerClockAtBottom && halClock.isAvailable() && SETTINGS.shouldShowClockInReader();
}

inline bool readerDarkModeEnabled() { return SETTINGS.readerDarkMode != 0; }

inline uint8_t readerBackgroundColor() { return readerDarkModeEnabled() ? 0x00 : 0xFF; }

inline bool readerForegroundBlack() { return !readerDarkModeEnabled(); }

inline int getTopClockStatusBarHeight() {
  if (!shouldShowTopClockStatusBar()) {
    return 0;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  return std::max(UITheme::getStatusBarHeight(), metrics.statusBarVerticalMargin);
}

inline int getTopClockStatusBarReservedHeight() {
  const int statusBarHeight = getTopClockStatusBarHeight();
  if (statusBarHeight <= 0) {
    return 0;
  }

  return UITheme::getInstance().getMetrics().topPadding + statusBarHeight;
}

inline uint8_t rotatedOrientation(const uint8_t orientation, const bool clockwise) {
  return clockwise ? (orientation + 1) % InkMODSettings::ORIENTATION_COUNT
                   : (orientation + InkMODSettings::ORIENTATION_COUNT - 1) % InkMODSettings::ORIENTATION_COUNT;
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromSideBtn;
  bool fromTilt;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  // Side buttons fire on press only when long-press action is OFF (nothing to detect).
  const bool sideUsePress = SETTINGS.sideButtonLongPress == InkMODSettings::SIDE_LONG_PRESS::SIDE_LONG_OFF;

  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool sidePrev = sideUsePress ? input.wasPressed(MappedInputManager::Button::PageBack)
                                     : input.wasReleased(MappedInputManager::Button::PageBack);
  const bool sideNext = sideUsePress ? input.wasPressed(MappedInputManager::Button::PageForward)
                                     : input.wasReleased(MappedInputManager::Button::PageForward);

  const bool frontPrev = input.wasReleased(MappedInputManager::Button::Left);
  const bool powerReleased = input.wasReleased(MappedInputManager::Button::Power);
  const bool shortPowerTurn = SETTINGS.shortPwrBtn == InkMODSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                              input.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration();
  const bool longPowerTurn = SETTINGS.longPwrBtn == InkMODSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                             input.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  const bool powerTurn = shortPowerTurn || longPowerTurn;
  const bool frontNext = input.wasReleased(MappedInputManager::Button::Right) || powerTurn;

  // fromSideBtn is true when only side buttons contributed to this page turn.
  const bool fromSide = (sidePrev || sideNext) && !(frontPrev || frontNext);
  return {tiltPrev || sidePrev || frontPrev, tiltNext || sideNext || frontNext, fromSide, tiltPrev || tiltNext};
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

}  // namespace ReaderUtils
