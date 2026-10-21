#pragma once

#include <InkMODSettings.h>
#include <GfxRenderer.h>
#include <FreeInkUICore.h>
#include <HalClock.h>
#include <HalGPIO.h>
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

  // X4 Pro touch controls are copied from CrossPoint 1.6.0: swipe mode turns
  // pages horizontally; tap modes use the outer thirds and leave the center
  // third free for the reader menu.
  bool touchPrev = false;
  bool touchNext = false;
  if (SETTINGS.touchReaderControls != InkMODSettings::TOUCH_READER_OFF && input.hasTouch()) {
    if (SETTINGS.touchReaderControls == InkMODSettings::TOUCH_READER_SWIPE) {
      const auto dir = input.wasSwipe();
      touchNext = dir == MappedInputManager::SwipeDir::Left;
      touchPrev = dir == MappedInputManager::SwipeDir::Right;
    } else {
      int x = 0, y = 0;
      if (input.wasScreenTapped(x, y)) {
        if (SETTINGS.touchReaderControls == InkMODSettings::TOUCH_READER_FULL_TAP) {
          touchNext = true;
        } else {
          const int width = input.getRendererWidth();
          const int zoneWidth = width / 3;
          const bool inverted = SETTINGS.touchReaderControls == InkMODSettings::TOUCH_READER_INVERTED_TAP;
          if (x < zoneWidth) { touchPrev = !inverted; touchNext = inverted; }
          else if (x >= width - zoneWidth) { touchNext = !inverted; touchPrev = inverted; }
        }
      }
    }
  }

  // fromSideBtn is true when only side buttons contributed to this page turn.
  const bool fromSide = (sidePrev || sideNext) && !(frontPrev || frontNext || touchPrev || touchNext);
  return {tiltPrev || sidePrev || frontPrev || touchPrev,
          tiltNext || sideNext || frontNext || touchNext, fromSide, tiltPrev || tiltNext};
}

inline bool isTouchReaderMenuTap(const MappedInputManager& input) {
  if (!input.hasTouch()) return false;

  // The reader menu is opened by a tap in the central zone, never by an
  // upward swipe. An upward swipe must not bypass disabled touch controls.
  // Full-screen tap mode still reserves all taps for page turning; use a
  // configured hardware shortcut to open the menu in that mode.
  if (SETTINGS.touchReaderControls == InkMODSettings::TOUCH_READER_FULL_TAP) return false;

  int x = 0, y = 0;
  if (!input.wasScreenTapped(x, y)) return false;
  const int width = input.getRendererWidth();
  const int height = input.getRendererHeight();

  // In landscape the old center-third box is too easy to hit while reaching
  // for the page edges. Keep the menu gesture deliberately central there:
  // middle 25% of the long axis and middle 50% of the short axis. Portrait
  // keeps the familiar center-third behaviour.
  if (width > height) {
    const int left = (width * 3) / 8;
    const int right = width - left;
    const int top = height / 4;
    const int bottom = height - top;
    return x >= left && x < right && y >= top && y < bottom;
  }

  return x >= width / 3 && x < width - width / 3 && y >= height / 3 && y < height - height / 3;
}

inline bool isRefreshActionDue(const int pagesUntilRefreshAction) {
  return pagesUntilRefreshAction != InkMODSettings::REFRESH_COUNTDOWN_DISABLED && pagesUntilRefreshAction <= 1;
}

inline bool isFullRefreshForced(const int pagesUntilRefreshAction) {
  return pagesUntilRefreshAction == InkMODSettings::REFRESH_COUNTDOWN_FORCE_FULL;
}

inline bool isBwReinforcementForced(const int pagesUntilRefreshAction) {
  return pagesUntilRefreshAction == InkMODSettings::REFRESH_COUNTDOWN_FORCE_BW_REINFORCEMENT;
}

inline void forceFullRefresh(int& pagesUntilRefreshAction) {
  pagesUntilRefreshAction = InkMODSettings::REFRESH_COUNTDOWN_FORCE_FULL;
}

inline void forceBwReinforcement(int& pagesUntilRefreshAction) {
  pagesUntilRefreshAction = InkMODSettings::REFRESH_COUNTDOWN_FORCE_BW_REINFORCEMENT;
}

inline void countOrdinaryRefresh(int& pagesUntilRefreshAction) {
  if (pagesUntilRefreshAction != InkMODSettings::REFRESH_COUNTDOWN_DISABLED) {
    pagesUntilRefreshAction--;
  }
}

inline bool shouldUseNoFlashReinforcement(const int pagesUntilRefreshAction,
                                          const bool pageSupportsNoFlashReinforcement = true) {
  return gpio.deviceIsX3() && pageSupportsNoFlashReinforcement &&
         (isBwReinforcementForced(pagesUntilRefreshAction) ||
          (!isFullRefreshForced(pagesUntilRefreshAction) &&
           SETTINGS.refreshAction == InkMODSettings::REFRESH_ACTION_BW_REINFORCEMENT));
}

inline bool x3DoubleNoFlashEnabled() {
  return gpio.deviceIsX3() && SETTINGS.refreshAction == InkMODSettings::REFRESH_ACTION_BW_REINFORCEMENT;
}

// Two consecutive OEM AA-pre-BW(mid) passes. The first pass intentionally
// keeps panel power on; the second is either also intermediate (when a
// grayscale/AA overlay follows) or final (plain B/W page).
inline void displayX3DoubleNoFlash(const GfxRenderer& renderer, const bool anotherPassFollows = false) {
  renderer.displayGrayscaleBaseIntermediate(HalDisplay::FAST_REFRESH);
  if (anotherPassFollows) {
    renderer.displayGrayscaleBaseIntermediate(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  }
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilRefreshAction,
                                    const bool pageSupportsNoFlashReinforcement = true) {
  if (isRefreshActionDue(pagesUntilRefreshAction)) {
    if (shouldUseNoFlashReinforcement(pagesUntilRefreshAction, pageSupportsNoFlashReinforcement)) {
      // The X3 no-flash cleanup is deliberately stronger than an ordinary
      // page turn: two consecutive OEM AA-pre-BW(mid) reinforcement passes.
      displayX3DoubleNoFlash(renderer);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    pagesUntilRefreshAction = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    countOrdinaryRefresh(pagesUntilRefreshAction);
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
  // The controller's gray planes were just displayed; restore the B/W RAM
  // baseline so the next differential page turn compares against real B/W,
  // not against stale grayscale plane contents.
  renderer.cleanupGrayscaleWithFrameBuffer();
}

}  // namespace ReaderUtils
