#include <BoardConfig.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>

#include "BootLog.h"

HalGPIO gpio;

namespace X3GPIO {
struct X3ProbeResult {
  bool bq27220 = false;
  bool ds3231 = false;
  bool qmi8658 = false;
  uint8_t score() const { return static_cast<uint8_t>(bq27220) + static_cast<uint8_t>(ds3231) + static_cast<uint8_t>(qmi8658); }
};

bool readI2CReg8(uint8_t addr, uint8_t reg, uint8_t* outValue) {
  Wire.beginTransmission(addr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  *outValue = Wire.read(); return true;
}

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  const uint8_t lo = Wire.read(); const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo; return true;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0; if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) return false;
  *outCurrent = static_cast<int16_t>(raw); return true;
}

bool probeBQ27220Signature() {
  uint16_t soc = 0, voltageMv = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_SOC_REG, &soc) || soc > 100) return false;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_VOLT_REG, &voltageMv)) return false;
  return voltageMv >= 2500 && voltageMv <= 5000;
}

bool probeDS3231Signature() {
  uint8_t sec = 0; if (!readI2CReg8(I2C_ADDR_DS3231, DS3231_SEC_REG, &sec)) return false;
  return ((sec >> 4) & 0x07) <= 5 && (sec & 0x0F) <= 9;
}

bool probeQMI8658Signature() {
  uint8_t whoami = 0;
  if (readI2CReg8(I2C_ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) return true;
  return readI2CReg8(I2C_ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE;
}

X3ProbeResult runX3ProbePass() {
  X3ProbeResult result;
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ); Wire.setTimeOut(6);
  result.bq27220 = probeBQ27220Signature(); result.ds3231 = probeDS3231Signature(); result.qmi8658 = probeQMI8658Signature();
  Wire.end(); pinMode(20, INPUT); pinMode(0, INPUT); return result;
}
}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";
enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue sanitizeNvsDeviceValue(const uint8_t raw, const NvsDeviceValue defaultValue) {
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
  Preferences prefs; if (!prefs.begin(HW_NAMESPACE, false)) return;
  prefs.putUChar(key, static_cast<uint8_t>(value)); prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) { return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4; }

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Read override + cached fingerprint in one NVS session. Preferences::begin()
  // touches NVS metadata, so opening the same namespace twice on every boot is
  // needless work when both keys live together.
  const DeviceDetectionNvs nvs = readDeviceDetectionNvs();
  const NvsDeviceValue overrideValue = nvs.overrideValue;
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }
  const NvsDeviceValue cachedValue = nvs.cachedValue;
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }
  const X3GPIO::X3ProbeResult pass1 = X3GPIO::runX3ProbePass(); delay(2); const X3GPIO::X3ProbeResult pass2 = X3GPIO::runX3ProbePass();
  const uint8_t score1 = pass1.score(), score2 = pass2.score();
  LOG_INF("HW", "X3 probe scores: pass1=%u(bq=%d rtc=%d imu=%d) pass2=%u(bq=%d rtc=%d imu=%d)", score1, pass1.bq27220, pass1.ds3231, pass1.qmi8658, score2, pass2.bq27220, pass2.ds3231, pass2.qmi8658);
  if ((score1 >= 2) && (score2 >= 2)) { writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3); return HalGPIO::DeviceType::X3; }
  if ((score1 == 0) && (score2 == 0)) { writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4); return HalGPIO::DeviceType::X4; }
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
  BootLog::stepf("HW", "device fingerprint: %s", deviceIsX3() ? "X3" : "X4");

  // Match CrossInk exactly: select the base board profile FIRST (so
  // BoardConfig::ACTIVE already reflects X3 or X4 pins/defaults), THEN
  // resolve the physical panel controller via the SDK's single shared probe
  // - freeink::applyXteinkDisplayController() - for BOTH device families.
  // inkMOD previously had a bespoke X3-only wrapper here
  // (detectX3DisplayIsUc8279()/detectX3DisplayController(), with its own
  // epd_ovr/epd_det NVS keys) that never went through this same, better-
  // tested shared path the way X4 always did. The EPD controller probe
  // bit-bangs the display pins, so this whole sequence must finish before
  // SPI.begin() claims them.
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);

  BootLog::step("HW", "calling applyXteinkDisplayController() - EPD bus probe starts here");
  const bool promoted = freeink::applyXteinkDisplayController();
  BootLog::stepf("HW", "applyXteinkDisplayController() returned promoted=%d controller=%d", promoted ? 1 : 0,
                  static_cast<int>(BoardConfig::ACTIVE.displayController));
  if (deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
    BootLog::step("HW", "X3 promoted to XteinkX3Uc8279 profile");
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
  while (inputMgr.isPressed(BTN_POWER)) { delay(50); inputMgr.update(); }
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

void HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  if (shortPressAllowed) return;
  const uint16_t calibration = millis();
  const uint16_t calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;
  const auto start = millis(); inputMgr.update();
  while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) { delay(10); inputMgr.update(); }
  if (inputMgr.isPressed(BTN_POWER)) {
    do { delay(10); inputMgr.update(); } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getPowerButtonHeldTime() < calibratedDuration);
    if (inputMgr.getPowerButtonHeldTime() < calibratedDuration) startDeepSleep();
  } else startDeepSleep();
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
  return digitalRead(UART0_RXD) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();
  const bool usbConnected = isUsbConnected();
  if (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP) return WakeupReason::PowerButton;
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) return WakeupReason::PowerButton;
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) return WakeupReason::AfterFlash;
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) return WakeupReason::AfterUSBPower;
  return WakeupReason::Other;
}