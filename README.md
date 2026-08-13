# inkMOD

A community mod of the inkMOD e-reader firmware featuring Russian and Ukrainian interface localization, expanded format support, and comprehensive UI and reliability enhancements.

## Features

- **Localization:** Full Russian and Ukrainian interface support.
- **Formats:** Native support for `.fb2` and `.fb2.zip` alongside EPUB, TXT, XTC and XTCH.
- **Themes:** Multiple home-screen themes (Lyra, Lyra Carousel, RoundedRaff, Minimal, Dashboard), optimized for accuracy and performance.
- **Fonts:** Interface text-size toggle (Large vs. Default) alongside the reader's native font settings.
- **Dictionary support:** Dictionary lookup directly from a book, multiple dictionaries, long-article paging and browser-side dictionary preparation.
- **Clippings:** Save and manage text excerpts directly from EPUB/FB2 books.
- **Converters & Web Portal:**
  - Built-in EPUBKIT optimizer for X3/X4.
  - Dictionary preparation/upload.
  - Sleep-screen PNG generator with background removal.
  - `.ttf` / `.otf` → `.cpfont` font conversion with automatic upload to the device.
  - Web upload supporting directory trees and file management.
- **Clock & Time Sync:**
  - X4 (no hardware RTC): Time syncs via Wi-Fi on connection and during background boot. Falls back to the last known successfully synced time when offline to prevent clock loss.
  - X3 (hardware RTC): Native hardware support.
  - Option to toggle the clock off entirely in Settings.
- **Sleep Screen:** Book cover, calendar and custom overlay sleep screens.
- **Reading Stats:** Detailed reading statistics and completed-book tracking.
- **OTA Updates:** Firmware update checks point directly to this repository's GitHub Releases, built and published automatically via GitHub Actions on tagged releases.
- **Support:** Optional project support through the Telegram channel `@inkmodx4`.

---

## Changelog

### v1.1.3 — 2026-08-13

#### Highlights

- Significantly improved memory behavior on large FB2/FB2.ZIP books.
- Verified opening of a real **~46 MB FB2 book** directly on Xteink without preliminary conversion.
- Reworked FB2 indexing to avoid large contiguous heap allocations on ESP32-C3.
- Large FB2.ZIP archives now use a safer sequential extract-and-scan path instead of holding DEFLATE + XML processing in RAM at the same time.
- Stabilized XTC/XTCH reading with low-memory streaming and reduced aggressive refresh behavior.
- Large EPUB/FB2 chapters remain internally split for RAM safety but are presented to the user as one continuous chapter.
- Added full dictionary support with selectable words, multiple dictionaries and long-article paging.
- Improved dictionary lookup for punctuation, hyphenated line breaks and StarDict synonym tables.
- Added browser-side dictionary preparation and upload.
- Added EPUBKIT to the web interface for browser-side EPUB optimization targeted at X3/X4.
- Added text clippings for EPUB/FB2 with saved-highlight management.
- Added customizable book menu ordering and visibility.
- Added file rename support in both the device UI and web interface.
- Added real two-line filename wrapping in File Browser.
- Added a browser-based sleep-screen generator with PNG alpha transparency and edge-connected background removal.
- Added direct upload/apply for generated sleep screens.
- Added optional InkMOD support screen with QR/link to `@inkmodx4`.
- Fixed Lyra Carousel centering with seven icons.
- Improved small-cover sleep-screen handling to avoid ugly upscaling.
- Fixed several quick-action/button edge cases around Dictionary.
- Removed experimental idle light-sleep that could freeze X4 after USB disconnect; safe low-frequency idle mode remains.
- Split Developer and Release builds so production builds stay clean while USB flashing remains available.

For the full list of changes, see `CHANGELOG.md`.

---

### v1.1.2 — 2026-08-07

#### Added

- Plain `.zip` archives are identified dynamically by their contents upon opening, eliminating strict `.epub`, `.fb2`, or `.fb2.zip` extension dependencies.
- Large EPUB, FB2, and image uploads enable browser-side optimization by default to offload processing from the device.
- Rebranded firmware and web portal as inkMOD with updated boot, sleep screen, and web assets.

#### Changed

- **UI & Accessibility**
  - Enabling Increase interface text enlarges search fields, on-screen keyboards, and help text while maintaining independent book fonts.
  - On-screen keyboard avoids side-button areas and adds dedicated English, Russian, and Ukrainian layouts.
  - Moving the clock display to the bottom no longer leaves empty space reserved at the top.
- **EPUB Engine**
  - Layout respects semantic container groups and image `max-width` CSS rules.
  - Standard `figleft` and `figright` classes correctly wrap text around illustrations.
  - Pre-indexing of upcoming chapters is disabled during active reading to conserve memory on image-heavy books.
- **FB2 Engine**
  - Loading progress bars animate continuously during on-demand chapter map generation.
  - Chapters split automatically when necessary to remain within the X4 memory budget.

#### Fixed

- Dynamic list row-height calculation prevents label clipping on large UI text.
- Search queries ignore Cyrillic case and initialize in the selected interface language.
- Reset Reader Data preserves Wi-Fi and core system settings.
- OTA release builds embed their GitHub release tags to prevent repetitive update prompts.

---

## Building

This project is built using [PlatformIO](https://platformio.org/).

```bash
# Install dependencies into PlatformIO Python environment
pip install -r requirements.txt

# Build firmware
pio run -e tiny

# Build and flash via USB connection
pio run -e tiny -t upload

# Developer build with diagnostics
pio run -e developer -t upload

# Production release build
pio run -e release -t upload
```

---

## Support InkMOD

InkMOD is free and community-driven.

If you find the project useful and want to support further development, testing and new features, visit the project Telegram channel:

**[@inkmodx4](https://t.me/inkmodx4)**

The repository's **Sponsor** button points to the same project channel. Support is entirely optional and does not unlock or restrict any firmware features.
