#include "LegacyReaderHal.h"

#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>

#include "MappedInputManager.h"

namespace reader::hal {

namespace {

HalDisplay::RefreshMode toLegacyRefreshMode(const RefreshMode mode) {
  return mode == RefreshMode::Full ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH;
}

}  // namespace

void LegacyDisplay::clear() { display.clearScreen(); }

void LegacyDisplay::update(const RefreshMode mode) { display.displayBuffer(toLegacyRefreshMode(mode)); }

void LegacyDisplay::updatePartial(const Rect& dirty, const RefreshMode mode) {
  // Current X4 HAL exposes a safe full-frame buffer flush, not a rectangle API.
  // Keep the dirty rect in the contract for a future backend, but do not fake a
  // second framebuffer or invoke panel internals from reader code.
  (void)dirty;
  update(mode);
}

bool LegacyStorage::exists(const char* path) const { return path && Storage.exists(path); }

bool LegacyStorage::remove(const char* path) { return path && Storage.remove(path); }

bool LegacyInput::poll(KeyEvent& out) {
  input_.update();
  constexpr MappedInputManager::Button kButtons[] = {
      MappedInputManager::Button::Up,      MappedInputManager::Button::Down,
      MappedInputManager::Button::Left,    MappedInputManager::Button::Right,
      MappedInputManager::Button::Confirm, MappedInputManager::Button::Back,
      MappedInputManager::Button::Power,
  };
  constexpr Key kKeys[] = {Key::Up, Key::Down, Key::Left, Key::Right, Key::Confirm, Key::Back, Key::Power};

  for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); ++i) {
    if (input_.wasPressed(kButtons[i])) {
      out = {kKeys[i], KeyAction::Pressed};
      return true;
    }
    if (input_.wasReleased(kButtons[i])) {
      out = {kKeys[i], KeyAction::Released};
      return true;
    }
  }
  out = {};
  return false;
}

void LegacyPower::sleep() { power_.startDeepSleep(gpio_); }

void LegacyPower::shutdown() { power_.startDeepSleep(gpio_); }

}  // namespace reader::hal
