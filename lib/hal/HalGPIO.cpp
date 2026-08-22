#include <BoardConfig.h>
#include <HalGPIO.h>
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
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);
  uint16_t raw = 0;
  const bool ok = readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw);
  Wire.end();
  pinMode(X3_I2C_SDA, INPUT);
  pinMode(X3_I2C_SCL, INPUT);
  if (!ok) return false;
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}
}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3
constexpr char NVS_KEY_EPD_OVERRIDE[] = "epd_ovr";  // 0=auto, 1=uc8253, 2=uc8279
// Renamed from "epd_det" -> "epd_det2": firmware built before the boot-order
// fix below could probe with BoardConfig::ACTIVE still pointing at the X4
// profile and cache a wrong UC8253/UC8279 verdict for X3 units. Bumping the
// key name makes any such stale cache simply miss (readNvsDeviceValue()
// returns Unknown) so the panel is re-probed correctly on the first boot of
// this build, with no UART access or manual NVS erase required.
constexpr char NVS_KEY_EPD_CACHED[] = "epd_det2";   // 0=unknown, 1=uc8253, 2=uc8279

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) return defaultValue;
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) return defaultValue;
  return static_cast<NvsDeviceValue>(raw);
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
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "X3 probe scores: pass1=%u pass2=%u", score1, score2);

  switch (verdict) {
    case freeink::XteinkVerdict::X3Confirmed:
      writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
      return HalGPIO::DeviceType::X3;
    case freeink::XteinkVerdict::X4Confirmed:
      writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
      return HalGPIO::DeviceType::X4;
    case freeink::XteinkVerdict::Inconclusive:
      break;
  }

  // Conservative fallback; do not cache an inconclusive result.
  return HalGPIO::DeviceType::X4;
}

bool detectX3DisplayIsUc8279() {
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_EPD_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue != NvsDeviceValue::Unknown) {
    LOG_INF("HW", "EPD controller override active: %s", overrideValue == NvsDeviceValue::X3 ? "UC8279" : "UC8253");
    return overrideValue == NvsDeviceValue::X3;
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_EPD_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue != NvsDeviceValue::Unknown) {
    LOG_INF("HW", "Using cached EPD controller: %s", cachedValue == NvsDeviceValue::X3 ? "UC8279" : "UC8253");
    return cachedValue == NvsDeviceValue::X3;
  }

  uint8_t ver[5] = {0};
  uint8_t flg = 0;
  const freeink::X3DisplayVerdict verdict = freeink::detectX3DisplayController(ver, &flg);
  LOG_INF("HW", "EPD probe: ver=%02X %02X %02X %02X %02X flg=%02X verdict=%u", ver[0], ver[1], ver[2], ver[3],
          ver[4], flg, static_cast<unsigned>(verdict));

  if (verdict == freeink::X3DisplayVerdict::Uc8279Confirmed) {
    writeNvsDeviceValue(NVS_KEY_EPD_CACHED, NvsDeviceValue::X3);
    return true;
  }
  if (verdict == freeink::X3DisplayVerdict::Uc8253Assumed) {
    writeNvsDeviceValue(NVS_KEY_EPD_CACHED, NvsDeviceValue::X4);
  }
  // Inconclusive: use UC8253 but re-probe next boot.
  return false;
}
}  // namespace

void HalGPIO::begin() {
#ifdef FORCE_DEVICE_X3
  _deviceType = DeviceType::X3;
  LOG_INF("HW", "Device override active via build flag: X3");
#else
  _deviceType = detectDeviceTypeWithFingerprint();
#endif

  // Ported from CrossInk: the EPD controller probes bit-bang the display pins,
  // therefore every display-controller decision must happen BEFORE SPI.begin().
  //
  // IMPORTANT: select the base board profile FIRST, before probing the panel.
  // detectX3DisplayIsUc8279() (via detectXteinkDisplayController()) reads
  // BoardConfig::ACTIVE.displayController to decide whether to escalate the
  // probe's reset pulse to the 50ms vendor-ID timing that a real UC8279d needs
  // to answer. If ACTIVE is still the X4 default (SSD1677) at probe time, that
  // escalation never happens, a genuine UC8279d X3 panel fails the screening
  // pass, and the firmware wrongly falls back to (and caches) UC8253 — which
  // then hangs on the loading screen because the wrong command set is sent to
  // the panel. Selecting the X3 profile first makes ACTIVE.displayController
  // == UC8253 during the probe, enabling the escalation, matching the X4 path
  // below and upstream's ordering.
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);
  const bool x3IsUc8279 = deviceIsX3() && detectX3DisplayIsUc8279();
  if (x3IsUc8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
  }

  // Match CrossInk ordering exactly for X4: select the X4 profile first, let the
  // SDK resolve its controller while the EPD pins are still free, then attach SPI.
  if (deviceIsX4()) {
    freeink::applyXteinkDisplayController();
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
  return digitalRead(UART0_RXD) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();
  const bool usbConnected = isUsbConnected();
  if (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP) return WakeupReason::PowerButton;
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected)
    return WakeupReason::PowerButton;
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected)
    return WakeupReason::AfterFlash;
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected)
    return WakeupReason::AfterUSBPower;
  return WakeupReason::Other;
}
