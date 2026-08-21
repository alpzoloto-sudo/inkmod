#include <HalGPIO.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>

HalGPIO gpio;

namespace X3GPIO {

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) return false;
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue sanitizeNvsDeviceValue(uint8_t raw, NvsDeviceValue defaultValue) {
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) return defaultValue;
  return static_cast<NvsDeviceValue>(raw);
}

struct DeviceDetectionNvs {
  NvsDeviceValue overrideValue = NvsDeviceValue::Unknown;
  NvsDeviceValue cachedValue = NvsDeviceValue::Unknown;
};

DeviceDetectionNvs readDeviceDetectionNvs() {
  DeviceDetectionNvs values;
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) return values;
  values.overrideValue = sanitizeNvsDeviceValue(
      prefs.getUChar(NVS_KEY_DEV_OVERRIDE, static_cast<uint8_t>(NvsDeviceValue::Unknown)), NvsDeviceValue::Unknown);
  values.cachedValue = sanitizeNvsDeviceValue(
      prefs.getUChar(NVS_KEY_DEV_CACHED, static_cast<uint8_t>(NvsDeviceValue::Unknown)), NvsDeviceValue::Unknown);
  prefs.end();
  return values;
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) return;
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  const DeviceDetectionNvs nvs = readDeviceDetectionNvs();

  if (nvs.overrideValue == NvsDeviceValue::X3 || nvs.overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", nvs.overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(nvs.overrideValue);
  }

  if (nvs.cachedValue == NvsDeviceValue::X3 || nvs.cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", nvs.cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(nvs.cachedValue);
  }

  // CrossPoint 1.5.0 / FreeInk canonical two-pass X3/X4 fingerprint.
  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "Xteink probe scores: pass1=%u pass2=%u verdict=%u", score1, score2,
          static_cast<unsigned>(verdict));

  if (verdict == freeink::XteinkVerdict::X3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }
  if (verdict == freeink::XteinkVerdict::X4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Same conservative fallback as CrossPoint: do not cache an inconclusive probe.
  return HalGPIO::DeviceType::X4;
}
}  // namespace

void HalGPIO::begin() {
#ifdef FORCE_DEVICE_X3
  _deviceType = DeviceType::X3;
  LOG_INF("HW", "Device override active via build flag: X3");
#else
  _deviceType = detectDeviceTypeWithFingerprint();
#endif

  // CrossPoint 1.5.0 startup order is intentional: select the family first,
  // resolve the exact per-batch EPD controller while the display pins are free,
  // then hand the bus to SPI/InputManager.
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);
  freeink::applyXteinkDisplayController();

  if (deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
  }

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }

  inputMgr.begin();
}

void HalGPIO::update() {
  inputMgr.update();
  usbStateChanged = false;
  const unsigned long now = millis();
  constexpr unsigned long X3_USB_POLL_INTERVAL_MS = 1000;
  constexpr unsigned long X4_USB_POLL_INTERVAL_MS = 100;
  const unsigned long pollInterval = deviceIsX3() ? X3_USB_POLL_INTERVAL_MS : X4_USB_POLL_INTERVAL_MS;
  if (lastUsbPollMs != 0 && now - lastUsbPollMs < pollInterval) return;
  lastUsbPollMs = now;
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }
bool HalGPIO::isUsbConnectedCached() const { return lastUsbConnected; }
bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }
bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }
bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }
bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }
bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }
unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }
unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

void HalGPIO::startDeepSleep() {
  while (inputMgr.isPressed(BTN_POWER)) {
    delay(50);
    inputMgr.update();
  }
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

void HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  if (shortPressAllowed) return;
  const uint16_t calibration = millis();
  const uint16_t calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;
  const auto start = millis();
  inputMgr.update();
  while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) {
    delay(10);
    inputMgr.update();
  }
  if (inputMgr.isPressed(BTN_POWER)) {
    do {
      delay(10);
      inputMgr.update();
    } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getPowerButtonHeldTime() < calibratedDuration);
    if (inputMgr.getPowerButtonHeldTime() < calibratedDuration) startDeepSleep();
  } else {
    startDeepSleep();
  }
}

bool HalGPIO::isUsbConnected() const {
  if (deviceIsX3()) {
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      int16_t currentMa = 0;
      if (X3GPIO::readBQ27220CurrentMA(&currentMa)) return currentMa > 0;
      delay(2);
    }
    return false;
  }

  if (BoardConfig::ACTIVE.usbDetect < 0) return false;
  return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();
  const bool usbConnected = isUsbConnected();

  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected)
    return WakeupReason::PowerButton;
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected)
    return WakeupReason::AfterFlash;
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected)
    return WakeupReason::AfterUSBPower;
  return WakeupReason::Other;
}
