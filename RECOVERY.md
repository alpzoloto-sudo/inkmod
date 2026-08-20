# inkMOD recovery guide

This document describes safe recovery methods for Xteink X4/X3 running inkMOD 1.1.5 or newer.

## 1. Normal USB flashing

Use this when the device still enumerates over USB and can be flashed normally.

The application image is written at address `0x10000`.

Example with esptool:

```bash
python -m esptool --chip esp32c3 --port COM3 --baud 921600 write_flash 0x10000 firmware-release-v1.1.5.bin
```

Do **not** run `erase_flash` for an ordinary firmware update. Keeping the existing flash contents preserves the factory calibration/NVS data used to select the correct X3/X4 display revision.

After flashing, reset or power-cycle the reader.

## 2. Emergency recovery from the SD card

inkMOD 1.1.5 adds an emergency firmware path intended for cases where the UI or display is unusable but the bootloader and SD card still work.

1. Download a compatible inkMOD firmware `.bin` file.
2. Rename it exactly to:

   `inkmod-recovery.bin`

3. Copy the file to the **root of the SD card**.
4. Insert the SD card into the reader.
5. Restart or power-cycle the reader.
6. Before the normal UI starts, inkMOD checks the root of the SD card for `inkmod-recovery.bin` and flashes it using the same validated firmware flasher used by normal updates.

On a successful recovery, the file is renamed to:

`inkmod-recovery.applied.bin`

This prevents an endless reflashing loop after reboot.

If the image is rejected or cannot be flashed, inkMOD attempts to rename it to:

`inkmod-recovery.failed.bin`

and continues the normal boot path.

### Important

- Use firmware built for Xteink X3/X4 ESP32-C3 only.
- Keep the battery charged before recovery.
- Do not remove power or the SD card while the recovery image is being written.
- Do not use `erase_flash` unless you explicitly intend to erase calibration/settings. Factory NVS calibration may contain the display revision identifier used by inkMOD 1.1.5.
- If a recovery image remains named `inkmod-recovery.bin`, inspect the SD card before retrying rather than repeatedly power-cycling the device.

## 3. Why factory calibration matters in 1.1.5

Different Xteink production runs use different E-Ink controllers. inkMOD 1.1.5 selects known revisions from the factory `hw_calib/screenType` value without probing the EPD bus before display initialization.

Known safe mappings include:

- original X3: UC8253 profile;
- newer X3 (`screenType` 2 / `0x0C`): full `XteinkX3Uc8279` profile;
- original X4: SSD1677 profile;
- known newer X4 revisions: UC8179 / UC8279 profiles when the corresponding factory value is present.

Unknown or missing values fall back to the original controller for that device family rather than guessing.

## 4. Last-resort rollback

If a test firmware causes a boot or display regression, flash the last known-good release at `0x10000` without erasing the flash, or place that release on the SD card as `inkmod-recovery.bin` if emergency recovery is already present on the installed firmware.
