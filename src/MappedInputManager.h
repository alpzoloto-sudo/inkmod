#pragma once

#include <HalGPIO.h>

#include <array>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
  static constexpr size_t BUTTON_COUNT = static_cast<size_t>(Button::PageForward) + 1;

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

  // Enable/disable reader-specific front button mapping.
  // Call with true in reader activity onEnter(), false in onExit().
  //
  // Reader quick actions can replace the reader while the triggering physical
  // button is still held (for example long-press Join Network). Without
  // suppressing that outstanding release, the newly-entered activity may
  // consume it as its own Back/Confirm event and immediately cancel, wedge a
  // modal flow, or leave the screen looking unresponsive. Capture only buttons
  // that are actually down at the reader -> non-reader transition so ordinary
  // navigation remains unaffected.
  void setReaderMode(bool enabled) {
    if (readerMode && !enabled) {
      if (mapButton(Button::Back, &HalGPIO::isPressed)) {
        suppressBackRelease = true;
      }
      if (mapButton(Button::Confirm, &HalGPIO::isPressed)) {
        suppressConfirmRelease = true;
      }
      if (gpio.isPressed(HalGPIO::BTN_POWER)) {
        suppressPowerConfirmRelease = true;
      }
    }
    readerMode = enabled;
  }
  void setPowerAsConfirmInReaderMode(bool enabled) { powerAsConfirmInReaderMode = enabled; }

  void update() const { gpio.update(); }
  void suppressNextBackRelease() { suppressBackRelease = true; }
  void suppressNextConfirmRelease() { suppressConfirmRelease = true; }
  void suppressNextPowerConfirmRelease() { suppressPowerConfirmRelease = true; }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;
  // Returns the raw front button index that was released this frame (or -1 if none).
  int getReleasedFrontButton() const;
  bool isFrontButtonPressed(uint8_t buttonIndex) const;

#ifdef SIMULATOR
  void simulatorInjectPress(Button button);
  void simulatorInjectRelease(Button button);
  void simulatorClearInputFrame();
#endif

 private:
  HalGPIO& gpio;
  bool readerMode = false;
  bool powerAsConfirmInReaderMode = false;
  mutable bool suppressBackRelease = false;
  mutable bool suppressConfirmRelease = false;
  mutable bool suppressPowerConfirmRelease = false;
#ifdef SIMULATOR
  std::array<bool, BUTTON_COUNT> simulatorPressed{};
  std::array<bool, BUTTON_COUNT> simulatorReleased{};
  std::array<bool, BUTTON_COUNT> simulatorHeld{};
  std::array<unsigned long, BUTTON_COUNT> simulatorPressStart{};
#endif

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  bool shouldUsePowerAsConfirmFallback() const;
  bool shouldMirrorPowerAsConfirmHold() const;
};
