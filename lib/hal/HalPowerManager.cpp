#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#include <cassert>
#include <cstdlib>
#include <ctime>

#include "HalGPIO.h"

namespace {
// esp_timer_get_time() value (microseconds since boot) at the last time USB
// charging was observed. RTC_DATA_ATTR keeps this in the small block of RAM
// that survives deep sleep - same reset domain as esp_timer_get_time()
// itself (both keep counting across deep sleep, both reset to 0/default on
// a hard reset), so the two stay consistent with each other without ever
// needing to touch the SD card. Declared here rather than as a class member
// specifically so the RTC_DATA_ATTR placement applies correctly.
RTC_DATA_ATTR uint64_t lastChargeEpochSeconds = 0;
}  // namespace

HalPowerManager powerManager;  // Singleton instance

namespace {
void disableWiFiBeforeDeepSleep() {
  const wifi_mode_t wifiMode = WiFi.getMode();
  if (wifiMode == WIFI_MODE_NULL) {
    return;
  }

  LOG_DBG("PWR", "Disabling WiFi before deep sleep (mode=%d)", static_cast<int>(wifiMode));
  if (wifiMode & WIFI_MODE_AP) {
    WiFi.softAPdisconnect(true);
  }
  if (wifiMode & WIFI_MODE_STA) {
    WiFi.disconnect(true);
  }
  delay(30);
  WiFi.mode(WIFI_OFF);
  delay(30);
}
}  // namespace

void HalPowerManager::begin() {
  if (gpio.deviceIsX3()) {
    // X3 uses an I2C fuel gauge for battery monitoring.
    // I2C init must come AFTER gpio.begin() so early hardware detection/probes are finished.
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    Wire.setTimeOut(4);
    _batteryUseI2C = true;
  } else {
    pinMode(BAT_GPIO0, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  disableWiFiBeforeDeepSleep();

  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

  // Pre-sleep routines from the original firmware
  // GPIO13 is connected to battery latch MOSFET, we need to make sure it's low during sleep
  // Note that this means the MCU will be completely powered off during sleep, including RTC
  constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
  gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SPIWP, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_SPIWP);
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  // Arm the wakeup trigger *after* the button is released
  // Note: this is only useful for waking up on USB power. On battery, the MCU will be completely powered off, so the
  // power button is hard-wired to briefly provide power to the MCU, waking it up regardless of the wakeup source
  // configuration
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

void HalPowerManager::trackChargingState() const {
  const unsigned long now = millis();
  if (_chargeCheckLastPollMs != 0 && (now - _chargeCheckLastPollMs) < BATTERY_POLL_MS) return;
  _chargeCheckLastPollMs = now;

  // HalGPIO owns the hardware polling cadence. Reading its cached state here
  // prevents this background tracker from doing another GPIO/I2C transaction.
  if (gpio.isUsbConnectedCached()) {
    // Wall-clock epoch seconds, not esp_timer_get_time() (monotonic
    // microseconds since boot) - this device fully powers the MCU off on
    // battery-only deep sleep (see startDeepSleep()'s own comment), which
    // resets that boot-relative counter to ~0 on every wake. A value
    // stored from a previous session would then almost always look
    // "later than now", making every stored timestamp appear invalid
    // after the very next sleep cycle - which is exactly what happened
    // before this fix (the setting permanently showed "-"). Wall-clock
    // time doesn't have that problem: the device's clock (HalClock) is
    // specifically designed to keep reading correctly across sleep/reboot
    // (that's what makes the status bar's own time-of-day display correct
    // after waking up), so comparing against it stays meaningful.
    // RTC memory, not SD - cheap enough to update on every poll while
    // charging, no transition-detection/debounce needed to avoid wear.
    lastChargeEpochSeconds = static_cast<uint64_t>(time(nullptr));
  }
}

uint64_t HalPowerManager::getLastChargeEpochSeconds() const { return lastChargeEpochSeconds; }

void HalPowerManager::seedLastChargeEpochSeconds(const uint64_t persistedValue) {
  if (lastChargeEpochSeconds == 0) {
    lastChargeEpochSeconds = persistedValue;
  }
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  trackChargingState();
  if (_batteryUseI2C) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    // Read SOC directly from I2C fuel gauge (16-bit LE register).
    // On I2C error, keep last known value to avoid UI jitter/slowdowns.
    Wire.beginTransmission(I2C_ADDR_BQ27220);
    Wire.write(BQ27220_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    Wire.requestFrom(I2C_ADDR_BQ27220, (uint8_t)2);
    if (Wire.available() < 2) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    const uint16_t soc = (hi << 8) | lo;
    _batteryCachedPercent = soc > 100 ? 100 : soc;
    _batteryLastPollMs = now;
    return _batteryCachedPercent;
  }
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);

  // X4 has no fuel gauge: estimate SOC from the ADC, then smooth display jitter.
  // Read voltage only once so percentage and the full-charge decision use the same sample.
  const uint16_t millivolts = battery.readMillivolts();
  const uint16_t rawPercent = BatteryMonitor::percentageFromMillivolts(millivolts);
  LOG_DBG("PWR", "X4 battery: %umV raw=%u%% cached=%d.%d%% usb=%d", millivolts, rawPercent,
          _batteryCachedPercent / 10, std::abs(_batteryCachedPercent % 10), gpio.isUsbConnectedCached() ? 1 : 0);

  // X4 has no MCU-readable "charge complete" signal: the green charger LED is
  // driven by the charging circuit itself, while firmware only sees USB presence
  // plus the battery ADC. On some X4 units ADC calibration reads a physically-full
  // cell as only ~95% on the generic LiPo curve. Treat the top end as full while
  // USB is present; merely plugging in at a lower charge still does not force 100%.
  //
  // Keep both gates: raw SOC handles unit-to-unit ADC offset, while the voltage
  // threshold catches curves that round slightly below 95%.
  constexpr uint16_t X4_USB_FULL_PERCENT = 95;
  constexpr uint16_t X4_USB_FULL_MV = 4080;
  if (gpio.isUsbConnectedCached() && (rawPercent >= X4_USB_FULL_PERCENT || millivolts >= X4_USB_FULL_MV)) {
    _batteryCachedPercent = 1000;
    _batteryLastPollMs = millis();
    return 100;
  }

  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * rawPercent;
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + rawPercent * 10) / 10;
  }
  return static_cast<uint16_t>(_batteryCachedPercent / 10);
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
