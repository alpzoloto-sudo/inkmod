#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  // True when a physical DS3231 RTC chip was found (X3). False means we fall back to a
  // software clock backed by the ESP32's internal timekeeping (X4), which is populated by
  // syncFromNTP() and needs a fresh NTP sync after every real power loss.
  bool _useHardwareRtc = false;
  // True when the software clock (X4) is currently running on a value seeded by
  // seedFallbackTime() rather than a real hardware RTC read or an NTP sync from this boot.
  bool _usingFallbackTime = false;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable uint16_t _cachedYear = 2000;
  mutable uint8_t _cachedMonth = 1;
  mutable uint8_t _cachedDay = 1;
  mutable bool _hasCachedTime = false;
  mutable bool _hasCachedDate = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // True if a usable clock source exists on this device: a DS3231 RTC (X3) or the
  // software clock (X4). Does NOT mean the time has been synced yet — formatTime()/
  // formatDate() still return false until a real time value is available.
  bool isAvailable() const { return _available; }

  // True if this device has no battery-backed RTC chip and instead relies on the
  // software clock, which loses its value on every real power loss and needs an
  // NTP sync after each such reset (see syncFromNTP()).
  bool needsPeriodicNTPSync() const { return _available && !_useHardwareRtc; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if no clock source is available, or the software clock hasn't been synced yet.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if no time value is available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Returns the raw clock date/time before any user-configured timezone offset is applied.
  // The clock is synced in UTC, so callers that need wall-clock local time should apply SETTINGS.clockUtcOffsetQ.
  bool getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const {
    return getDate(year, month, day, hour, minute);
  }

  // Format date into a caller-provided buffer as "Mon D, YYYY".
  // utcOffsetQuarterHoursBiased matches formatTime so the date rolls over at local midnight.
  // Returns false if no clock source is available or the date is invalid/unsynced.
  bool formatDate(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48) const;

  // Sync the clock from an NTP server. Requires WiFi to be connected.
  // On X3 this writes the result to the DS3231. On X4 the SNTP sync already sets the
  // ESP32's internal clock directly, so no further hardware write is needed.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the clock was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

  // X4 only: seed the software clock from a UTC epoch persisted from a previous successful
  // syncFromNTP() (the caller owns that persistence - see the layering note above; this HAL
  // stays free of any app-layer settings dependency). Intended as a best-effort fallback so
  // date/time-dependent UI (e.g. the Calendar sleep screen) has *something* to show on boots
  // where no WiFi network is reachable to get a fresh time, rather than showing nothing at
  // all. Has no effect on X3 (real battery-backed RTC) and is a no-op if a real time value
  // (hardware RTC read or this boot's own NTP sync) is already available.
  // Returns true if the fallback value was applied.
  bool seedFallbackTime(time_t epochUtc);

  // True if getTime()/getDate() are currently returning a value applied by seedFallbackTime()
  // rather than one confirmed by a hardware RTC read or this boot's own NTP sync. Always false
  // on X3. Callers that want to flag displayed time/dates as approximate can check this.
  bool isUsingFallbackTime() const { return _usingFallbackTime; }

 private:
  bool getDate(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const;
  bool writeDateTimeToRTC(uint16_t year, uint8_t month, uint8_t day, uint8_t weekday, uint8_t hour, uint8_t minute,
                          uint8_t second);
};
