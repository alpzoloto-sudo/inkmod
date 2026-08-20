#include "MappedInputManager.h"

#include <algorithm>
#include <utility>

#include "InkMODSettings.h"
#include "GlobalActions.h"

namespace {
using ButtonIndex = uint8_t;
constexpr ButtonIndex kNoButton = UINT8_MAX;

struct SideLayoutMap {
  ButtonIndex pageBackPrimary;
  ButtonIndex pageBackSecondary;
  ButtonIndex pageForwardPrimary;
  ButtonIndex pageForwardSecondary;
};

// Order matches InkMODSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, kNoButton, HalGPIO::BTN_DOWN, kNoButton},
    {HalGPIO::BTN_DOWN, kNoButton, HalGPIO::BTN_UP, kNoButton},
    {kNoButton, kNoButton, kNoButton, kNoButton},
    {kNoButton, kNoButton, HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
};

bool shouldSwapReaderSideButtons(const bool readerMode) {
  return readerMode && SETTINGS.sideButtonOrientationAware && SETTINGS.orientation != InkMODSettings::PORTRAIT;
}

bool shouldSwapReaderFrontNavButtons(const InkMODSettings::FRONT_BUTTON_ORIENTATION_AWARE orientationMode) {
  if (orientationMode == InkMODSettings::FRONT_ORIENTATION_AWARE_OFF) {
    return false;
  }
  return SETTINGS.orientation == InkMODSettings::LANDSCAPE_CW ||
         SETTINGS.orientation == InkMODSettings::LANDSCAPE_CCW ||
         (orientationMode == InkMODSettings::FRONT_ORIENTATION_AWARE_NAV_BUTTONS &&
          SETTINGS.orientation == InkMODSettings::INVERTED);
}

ButtonIndex invertFrontButtonPosition(const ButtonIndex button) {
  switch (button) {
    case HalGPIO::BTN_BACK:
      return HalGPIO::BTN_RIGHT;
    case HalGPIO::BTN_CONFIRM:
      return HalGPIO::BTN_LEFT;
    case HalGPIO::BTN_LEFT:
      return HalGPIO::BTN_CONFIRM;
    case HalGPIO::BTN_RIGHT:
      return HalGPIO::BTN_BACK;
    default:
      return button;
  }
}

ButtonIndex mapFrontButtonForReaderOrientation(const ButtonIndex button, const ButtonIndex leftButton,
                                               const ButtonIndex rightButton, const bool readerMode) {
  if (!readerMode) {
    return button;
  }

  const auto orientationMode =
      static_cast<InkMODSettings::FRONT_BUTTON_ORIENTATION_AWARE>(SETTINGS.frontButtonOrientationAware);

  if (orientationMode == InkMODSettings::FRONT_ORIENTATION_AWARE_ALL_BUTTONS &&
      SETTINGS.orientation == InkMODSettings::INVERTED) {
    return invertFrontButtonPosition(button);
  }

  if (shouldSwapReaderFrontNavButtons(orientationMode)) {
    if (button == leftButton) {
      return rightButton;
    }
    if (button == rightButton) {
      return leftButton;
    }
  }

  return button;
}

SideLayoutMap mapSideLayoutForReaderOrientation(SideLayoutMap side, const bool readerMode) {
  if (shouldSwapReaderSideButtons(readerMode)) {
    const bool hasPageBack = side.pageBackPrimary != kNoButton || side.pageBackSecondary != kNoButton;
    const bool hasPageForward = side.pageForwardPrimary != kNoButton || side.pageForwardSecondary != kNoButton;
    if (hasPageBack && hasPageForward) {
      std::swap(side.pageBackPrimary, side.pageForwardPrimary);
      std::swap(side.pageBackSecondary, side.pageForwardSecondary);
    }
  }
  return side;
}

ButtonIndex mapSideButtonForReaderOrientation(const ButtonIndex button, const bool readerMode) {
  if (!shouldSwapReaderSideButtons(readerMode)) {
    return button;
  }
  if (button == HalGPIO::BTN_UP) {
    return HalGPIO::BTN_DOWN;
  }
  if (button == HalGPIO::BTN_DOWN) {
    return HalGPIO::BTN_UP;
  }
  return button;
}

bool readMappedSideButtons(const HalGPIO& gpio, bool (HalGPIO::*fn)(uint8_t) const, const ButtonIndex primary,
                           const ButtonIndex secondary) {
  return (primary != kNoButton && (gpio.*fn)(primary)) || (secondary != kNoButton && (gpio.*fn)(secondary));
}

#ifdef SIMULATOR
size_t buttonIndex(MappedInputManager::Button button) { return static_cast<size_t>(button); }
#endif

}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  // Avoid rebuilding both front- and side-button mappings for every query.
  // Activities may ask several button states each main-loop pass, so only
  // compute the mapping group required by the requested logical button.
  switch (button) {
    case Button::Power:
      return (gpio.*fn)(HalGPIO::BTN_POWER);

    case Button::Up:
      return (gpio.*fn)(mapSideButtonForReaderOrientation(HalGPIO::BTN_UP, readerMode));
    case Button::Down:
      return (gpio.*fn)(mapSideButtonForReaderOrientation(HalGPIO::BTN_DOWN, readerMode));

    case Button::PageBack:
    case Button::PageForward: {
      const auto sideLayout = static_cast<InkMODSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
      const auto side = mapSideLayoutForReaderOrientation(kSideLayouts[sideLayout], readerMode);
      if (button == Button::PageBack) {
        return readMappedSideButtons(gpio, fn, side.pageBackPrimary, side.pageBackSecondary);
      }
      return readMappedSideButtons(gpio, fn, side.pageForwardPrimary, side.pageForwardSecondary);
    }

    case Button::Back:
    case Button::Confirm:
    case Button::Left:
    case Button::Right: {
      const bool useReaderMapping = readerMode && SETTINGS.readerFrontButtonsEnabled;
      const ButtonIndex btnBack = useReaderMapping ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack;
      const ButtonIndex btnConfirm = useReaderMapping ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm;
      const ButtonIndex btnLeft = useReaderMapping ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft;
      const ButtonIndex btnRight = useReaderMapping ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight;

      ButtonIndex physical = btnBack;
      switch (button) {
        case Button::Confirm:
          physical = btnConfirm;
          break;
        case Button::Left:
          physical = btnLeft;
          break;
        case Button::Right:
          physical = btnRight;
          break;
        case Button::Back:
        default:
          break;
      }
      return (gpio.*fn)(mapFrontButtonForReaderOrientation(physical, btnLeft, btnRight, readerMode));
    }
  }

  return false;
}

bool MappedInputManager::shouldUsePowerAsConfirmFallback() const { return !readerMode || powerAsConfirmInReaderMode; }

bool MappedInputManager::shouldMirrorPowerAsConfirmHold() const {
  return shouldUsePowerAsConfirmFallback() &&
         !isPowerButtonActionAvailableOutsideReader(static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn));
}

bool MappedInputManager::wasPressed(const Button button) const {
#ifdef SIMULATOR
  if (simulatorPressed[buttonIndex(button)]) {
    return true;
  }
#endif

  if (button == Button::Confirm) {
    if (mapButton(button, &HalGPIO::wasPressed)) {
      return true;
    }

    return shouldUsePowerAsConfirmFallback() &&
           !isPowerButtonActionAvailableOutsideReader(
               static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)) &&
           gpio.wasPressed(HalGPIO::BTN_POWER);
  }

  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
#ifdef SIMULATOR
  if (simulatorReleased[buttonIndex(button)]) {
    return true;
  }
#endif

  if (button == Button::Back) {
    if (!mapButton(button, &HalGPIO::wasReleased)) {
      return false;
    }

    if (suppressBackRelease) {
      suppressBackRelease = false;
      return false;
    }

    return true;
  }

  if (button == Button::Confirm) {
    if (mapButton(button, &HalGPIO::wasReleased)) {
      if (suppressConfirmRelease) {
        suppressConfirmRelease = false;
        return false;
      }
      return true;
    }

    if (!shouldUsePowerAsConfirmFallback() || !gpio.wasReleased(HalGPIO::BTN_POWER)) {
      return false;
    }

    if (suppressConfirmRelease) {
      suppressConfirmRelease = false;
      suppressPowerConfirmRelease = false;
      return false;
    }

    if (suppressPowerConfirmRelease) {
      suppressPowerConfirmRelease = false;
      return false;
    }

    const bool longPress = gpio.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
    const auto action = longPress ? static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn)
                                  : static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn);
    return !isPowerButtonActionAvailableOutsideReader(action);
  }

  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const {
#ifdef SIMULATOR
  if (simulatorHeld[buttonIndex(button)]) {
    return true;
  }
#endif

  if (button == Button::Confirm) {
    if (mapButton(button, &HalGPIO::isPressed)) {
      return true;
    }

    if (!shouldMirrorPowerAsConfirmHold() || !gpio.isPressed(HalGPIO::BTN_POWER)) {
      return false;
    }

    return !isPowerButtonActionAvailableOutsideReader(
               static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)) ||
           gpio.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  }

  return mapButton(button, &HalGPIO::isPressed);
}

bool MappedInputManager::wasAnyPressed() const {
#ifdef SIMULATOR
  if (std::any_of(simulatorPressed.begin(), simulatorPressed.end(), [](bool pressed) { return pressed; })) {
    return true;
  }
#endif
  return gpio.wasAnyPressed();
}

bool MappedInputManager::wasAnyReleased() const {
#ifdef SIMULATOR
  if (std::any_of(simulatorReleased.begin(), simulatorReleased.end(), [](bool released) { return released; })) {
    return true;
  }
#endif
  return gpio.wasAnyReleased();
}

unsigned long MappedInputManager::getHeldTime() const {
  unsigned long heldTime = gpio.getHeldTime();
#ifdef SIMULATOR
  const unsigned long now = millis();
  for (size_t i = 0; i < BUTTON_COUNT; i++) {
    if (simulatorHeld[i] && simulatorPressStart[i] > 0) {
      heldTime = std::max(heldTime, now - simulatorPressStart[i]);
    }
  }
#endif
  return heldTime;
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  const bool useReaderMapping = readerMode && SETTINGS.readerFrontButtonsEnabled;
  const ButtonIndex btnBack = useReaderMapping ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack;
  const ButtonIndex btnConfirm = useReaderMapping ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm;
  const ButtonIndex btnLeft = useReaderMapping ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft;
  const ButtonIndex btnRight = useReaderMapping ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight;
  const ButtonIndex mappedBack = mapFrontButtonForReaderOrientation(btnBack, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedConfirm = mapFrontButtonForReaderOrientation(btnConfirm, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedLeft = mapFrontButtonForReaderOrientation(btnLeft, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedRight = mapFrontButtonForReaderOrientation(btnRight, btnLeft, btnRight, readerMode);

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](ButtonIndex hw) -> const char* {
    if (hw == mappedBack) return back;
    if (hw == mappedConfirm) return confirm;
    if (hw == mappedLeft) return previous;
    if (hw == mappedRight) return next;
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}

int MappedInputManager::getReleasedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping for screens whose labels are fixed to physical slots.
  if (gpio.wasReleased(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasReleased(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasReleased(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasReleased(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}

bool MappedInputManager::isFrontButtonPressed(const uint8_t buttonIndex) const { return gpio.isPressed(buttonIndex); }

#ifdef SIMULATOR
void MappedInputManager::simulatorInjectPress(Button button) {
  const size_t idx = buttonIndex(button);
  simulatorPressed[idx] = true;
  simulatorReleased[idx] = false;
  simulatorHeld[idx] = true;
  simulatorPressStart[idx] = millis();
}

void MappedInputManager::simulatorInjectRelease(Button button) {
  const size_t idx = buttonIndex(button);
  simulatorPressed[idx] = false;
  simulatorReleased[idx] = true;
  simulatorHeld[idx] = false;
}

void MappedInputManager::simulatorClearInputFrame() {
  simulatorPressed.fill(false);
  simulatorReleased.fill(false);
}
#endif
