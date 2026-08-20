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
    return;
  }

  // This function is called on nearly every main-loop pass. Most calls ask to
  // keep normal speed while we are already at normal speed; avoid touching the
  // WiFi driver or CPU-frequency API in that overwhelmingly common no-op case.
  const LockMode mode = currentLockMode;
  if (!enabled && !isLowPower) {
    return;
  }
  if (enabled && mode != None && !isLowPower) {
    return;
  }

  if (enabled) {
    const wifi_mode_t wifiMode = WiFi.getMode();
    if (wifiMode != WIFI_MODE_NULL) {
      enabled = false;
    }
  }

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
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  disableWiFiBeforeDeepSleep();

  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

#ifdef ENABLE_SERIAL_LOG
  logSerial.end();
#endif

  constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
  gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SPIWP, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_SPIWP);
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

void HalPowerManager::trackChargingState() const {
  const unsigned long now = millis();
  if (_chargeCheckLastPollMs != 0 && (now - _chargeCheckLastPollMs) < BATTERY_POLL_MS) return;
  _chargeCheckLastPollMs = now;

  if (gpio.isUsbConnectedCached()) {
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

  // X4 has no fuel gauge, so avoid repeating ADC conversions when several UI
  // components ask for the battery during the same render window. The cache
  // cadence mirrors the X3 fuel-gauge path above; USB state is tracked
  // separately and still causes the status UI to repaint immediately.
  const unsigned long now = millis();
  if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
    return static_cast<uint16_t>(_batteryCachedPercent / 10);
  }

  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);
  const uint16_t millivolts = battery.readMillivolts();
  const uint16_t rawPercent = BatteryMonitor::percentageFromMillivolts(millivolts);
  LOG_DBG("PWR", "X4 battery: %umV raw=%u%% cached=%d.%d%% usb=%d", millivolts, rawPercent,
          _batteryCachedPercent / 10, std::abs(_batteryCachedPercent % 10), gpio.isUsbConnectedCached() ? 1 : 0);

  constexpr uint16_t X4_USB_FULL_PERCENT = 95;
  constexpr uint16_t X4_USB_FULL_MV = 4080;
  if (gpio.isUsbConnectedCached() && (rawPercent >= X4_USB_FULL_PERCENT || millivolts >= X4_USB_FULL_MV)) {
    _batteryCachedPercent = 1000;
    _batteryLastPollMs = now;
    return 100;
  }

  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * rawPercent;
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + rawPercent * 10) / 10;
  }
  _batteryLastPollMs = now;
  return static_cast<uint16_t>(_batteryCachedPercent / 10);
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
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