# Xteink OEM 6.2.4 driver-port baseline

This branch is intentionally isolated from `optimize/core`. It is used to reproduce the hardware behaviour of Xteink OEM firmware V6.2.4 from production binaries, without changing the normal inkMOD branch.

## Reference images

| Device | Locale | SHA-256 | Size |
|---|---|---|---:|
| X4 | EN | `eb345d59068ad9a2a14164134cfc521444f9fd295ca05d8164f78d17a46a44f0` | 5,665,376 |
| X4 | CH | `d8fc910ca75ab10ecd749b9b0dfce99fbabb5e8f9bbd90d1e6629dc0819e6aed` | 5,666,448 |
| X3 | EN | `7ec1b2d1b6e05f78dbfd16a61c620bcbad3d68bc3cff9c3c2280c18ff58ff52a` | 5,740,320 |
| X3 | CH | `561e5341f2af76a2cc5671ed977d3a092e6fe3e820d94e32ac13429d390c540d` | 5,822,384 |

All four are ESP32-C3 application images (magic `0xE9`) built on ESP-IDF 5.5.4 / Arduino core. EN/CH images are both retained because comparing them helps separate UI/resources from hardware code; X3/X4 comparison helps isolate device-specific hardware paths.

## Confirmed OEM X4 display identity

The OEM X4 binary contains these original identifiers in its read-only data:

- `GDEQ0426T82(QY 4.26)`
- `QY_GDEQ0426T82`
- `gray4_426`
- `_powerOff`
- `_updateFull`
- `_updatePart`
- `refreshLocalGC`
- `grey_full_wave`
- `grey_fast_wave`
- `grey_bw_base_fc`
- `grey_bw_base_fc_dual_stream`
- `grey_fast_wave_dual_stream`
- `grey_bw_base_fc_stream`
- `grey_fast_wave_stream`

This is important: the target is not to guess a generic SSD1677/UC81xx implementation. The port should reproduce the OEM GDEQ0426T82/QY power, refresh and grayscale behaviour where it differs from FreeInkDisplay.

## Other OEM hardware paths already located

The 6.2.4 X4 binary contains identifiable paths for:

- display/EPD (`epd_lock`, `display_update`, `Screen Refresh Timeout`, `XT-EPD`)
- SPI ownership/assertion (`SPI_ASSERT_SELFHEAL`, `SPI_ASSERT_NOT_ACTIVE` and detailed SPI state diagnostics)
- microSD mount/read/write and card-present handling
- battery-meter error handling
- GPIO diagnostics
- RTC startup path
- sleep/power-down path

The binary is stripped, so original `.cpp/.h` files cannot be recovered byte-for-byte. The safe approach is behavioural reimplementation: recover pin assignments, SPI settings, controller command streams, waits/timeouts, waveform tables and power/sleep sequencing from the machine code and data, then implement the equivalent behaviour behind the existing inkMOD/FreeInk interfaces.

## Port rules

1. Keep `optimize/core` unchanged.
2. Make all OEM-driver work only on `vendor-drivers-6.2.4` until real-device validation passes.
3. Preserve the existing high-level inkMOD reader/UI interfaces; replace only hardware behaviour.
4. Do not copy opaque OEM binary code into the firmware. Reimplement observed hardware behaviour in auditable C++.
5. Validate X4 and X3 separately. Never assume a value recovered from one model applies to the other.
6. Prefer OEM values over generic controller defaults once they are confirmed by at least two independent references (e.g. code path + table/data, or EN + CH image).
7. Keep a fallback to the current FreeInk driver until the corresponding OEM path has been verified on hardware.

## Work order

1. **EPD/display:** controller init, panel geometry/orientation, full/partial/local-GC, grayscale LUTs, BUSY handling, power-on/off/deep-sleep.
2. **SPI/GPIO:** pin map, CS/DC/RST/BUSY ownership, SPI mode/frequency and locking semantics.
3. **Input:** OEM button/ADC-ladder thresholds and wake behaviour.
4. **Power/sleep:** rail states, GPIO hold/release, wake sources, reboot/power-off transitions.
5. **Battery:** ADC/fuel-gauge scaling, charge/VBUS detection and low-battery thresholds.
6. **microSD:** SPI settings, card detect/power sequencing and retry policy.
7. **RTC/time:** model-specific RTC presence/absence and startup behaviour.

Each stage should build in CI before the next hardware subsystem is changed.