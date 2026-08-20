# inkMOD 1.1.5

inkMOD 1.1.5 is a stability and hardware-compatibility release for Xteink X4/X3.

## Highlights

- Added safe display-revision selection using the factory `hw_calib/screenType` calibration value instead of probing the E-Ink bus before display initialization.
- Added full X3 revision routing: original X3 stays on the UC8253 profile, while newer UC8279-based X3 units use the complete `XteinkX3Uc8279` board/display profile.
- Added known X4 UC8179/UC8279 revision selection while retaining SSD1677 as the fail-safe default when calibration is missing or unknown.
- Set X4 EPD SPI to the manufacturer-compatible 10 MHz rate for additional signal margin across display revisions.
- Kept SSD1677-specific booster setup isolated to SSD1677 units only.
- Added emergency SD-card firmware recovery via `inkmod-recovery.bin` before normal UI startup.
- Fixed Lyra Carousel reading-progress percentage clipping at the right edge.
- Preserved the 1.1.4 reader, FB2/FB2.ZIP, EPUB, dictionary, sleep-screen and web-interface improvements.

## Emergency recovery

If the device cannot be operated normally but still boots far enough to access the SD card:

1. Rename a compatible release firmware to `inkmod-recovery.bin`.
2. Copy it to the root of the SD card.
3. Insert the card and restart the reader.
4. inkMOD validates and flashes the image before starting the normal UI.
5. On success the file is renamed to `inkmod-recovery.applied.bin` to avoid a reflash loop.

See [`RECOVERY.md`](RECOVERY.md) for the full procedure and normal USB flashing instructions.

## Flashing

For a normal manual update, flash the release image at `0x10000` and do not erase the whole flash. Preserving NVS keeps factory calibration used for display-revision selection.

## Validation

The 1.1.5 candidate was built through the normal developer/release CI pipeline. The X4 path was hardware-tested after rollback from the unsafe early EPD-bus probe approach; the final selector no longer touches the display bus before `einkDisplay.begin()`.

X3 revision support is designed to fail closed: unknown calibration values stay on the original UC8253 profile rather than guessing a different controller.
