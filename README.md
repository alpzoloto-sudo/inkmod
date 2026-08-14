<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/inkmod-logo-dark.png">
    <source media="(prefers-color-scheme: light)" srcset="assets/inkmod-logo-light.png">
    <img alt="inkMOD — Custom Firmware for Xteink X4 / X3"
         src="assets/inkmod-logo-light.png"
         width="280">
  </picture>
</p>

<h1 align="center">inkMOD — Custom Firmware for Xteink X4 / X3</h1>

<p align="center">
  Open-source custom firmware for Xteink X4 and Xteink X3
</p>

**inkMOD** is an open-source custom firmware for **Xteink X4 and Xteink X3** e-readers with native **FB2, FB2.ZIP and EPUB** support, Russian and Ukrainian localization, dictionaries, improved typography, sleep covers, reading statistics, web file management and browser-side book optimization.

The project focuses on extending the capabilities of the Xteink X4/X3 while keeping the firmware fast and stable on memory-constrained hardware.

## Features

### 📚 Book formats

- Native **FB2** and **FB2.ZIP** reading without preliminary EPUB conversion.
- EPUB support with browser-side optimization through **EPUBKIT**.
- TXT, XTC and XTCH support.
- ZIP archives can be identified dynamically by their contents instead of relying only on the filename extension.
- Optimized handling of large books for the limited RAM available on Xteink X4.

### 🔤 Reading & Typography

- Word hyphenation for improved text layout.
- Custom reader fonts.
- Unicode fallback for missing characters when possible.
- Improved handling of large chapters and image-heavy books.
- Text clippings for EPUB and FB2.
- Save and manage selected text excerpts directly on the device.
- Customizable book menu ordering and visibility.

### 📖 Dictionaries

- Dictionary lookup directly while reading.
- Multiple installed dictionaries.
- StarDict dictionary support.
- Long dictionary articles with page navigation.
- Improved lookup of words containing punctuation and line-break hyphenation.
- StarDict synonym table support.
- Browser-side dictionary preparation and upload.

### 🌐 Web Interface

- File and directory management.
- Upload individual files or complete directory trees.
- File rename support.
- **EPUBKIT** browser-side EPUB optimizer for Xteink X3/X4.
- Dictionary preparation and upload.
- Sleep-screen generator.
- PNG transparency support.
- Edge-connected background removal.
- Direct upload and application of generated sleep screens.
- `.ttf` / `.otf` → `.cpfont` font conversion with automatic upload to the device.

### 🎨 Interface & Themes

Multiple home-screen themes are available, including:

- Lyra
- Lyra Carousel
- RoundedRaff
- Minimal
- Dashboard

The interface also includes:

- Full Russian localization.
- Full Ukrainian localization.
- Default and enlarged interface text modes.
- Improved search and on-screen keyboard layouts.
- Dedicated English, Russian and Ukrainian keyboards.

### 🌙 Sleep Screens

- Current book cover.
- Calendar.
- Custom image.
- Custom overlay.
- Separate timeout sleep-screen behavior.
- Quick Resume support.
- Improved FB2 cover generation.
- Improved handling of small cover images to prevent low-quality fullscreen upscaling.

### 📊 Reading Statistics

- Reading-time tracking.
- Session statistics.
- Reading progress.
- Completed-book tracking.
- Book information available directly from the File Browser.

### 🕐 Clock & Time

**Xteink X4**

The X4 has no hardware RTC, so inkMOD synchronizes the clock through Wi-Fi and preserves the last successfully synchronized time for offline operation.

**Xteink X3**

Native hardware RTC support is used.

The clock can also be disabled completely in Settings.

### 🔧 Diagnostics & Reliability

- **System → Device → Diagnostics** screen.
- Free RAM information.
- Maximum free memory block.
- SD card/storage information.
- Human-readable reset reason.
- Crash-report status.
- `/crash_report.txt` generation for guarded restarts and memory-related failures.
- Additional release crash breadcrumbs for troubleshooting.
- Memory-conscious FB2/FB2.ZIP processing designed for ESP32-C3 hardware.

### 🔄 OTA Updates

inkMOD can check this repository directly for new firmware releases.

Production releases are built automatically through GitHub Actions and published to GitHub Releases as:

`firmware-release-vX.Y.Z.bin`

Developer and production builds are kept separate so release firmware remains clean while diagnostic builds remain available for testing.

---

# Changelog

## v1.1.4 — 2026-08-14

### Stability Release

- Improved FB2 and FB2.ZIP cover handling, including proper full-size sleep-screen cover generation instead of using low-resolution thumbnails.
- Improved FB2 cover quality on the home screen and sleep screen.
- Added compact low-memory FB2 indexing for very large books.
- Improved first-open processing of large FB2.ZIP archives.
- Disabled the unstable runtime EPUB pre-splitter; large EPUB files now use the proven direct BMC/ERS reader path.
- Added word hyphenation for improved text layout.
- Added Unicode fallback handling for characters missing from the selected book font.
- Added release crash breadcrumbs and `/crash_report.txt` generation for OOM and guarded restarts.
- Added **System → Device → Diagnostics** with live RAM, reset and storage information.
- Reset reasons are displayed as readable descriptions instead of raw numeric codes.
- SD card usage information is available from Diagnostics.
- Added **Book information** to the File Browser long-press menu.
- Added separate timeout sleep-screen behavior with custom overlay/image and Quick Resume.
- Improved custom inkMOD branding across boot, sleep, calendar and web-interface screens.
- Corrected inkMOD logo positioning and orientation across portrait and landscape screens.
- Improved logo alignment on boot and default sleep screens.
- Improved inkMOD branding in the web interface.
- Improved `.fb2.zip` / ZIP book handling in the web uploader.
- Fixed Lyra Carousel right-edge clipping with seven icons.
- Kept Cover Mode / Cover Filter visible consistently in sleep-screen settings.
- GitHub Releases now publish the actual production `release` firmware variant instead of a diagnostic/tiny build.
- CI validates both developer and release builds.
- Added safeguards against accidentally re-enabling known unstable release paths.

## v1.1.3 — 2026-08-13

### Highlights

- Significantly improved memory behavior on large FB2/FB2.ZIP books.
- Verified opening of a real **~46 MB FB2 book** directly on Xteink without preliminary conversion.
- Reworked FB2 indexing to avoid large contiguous heap allocations on ESP32-C3.
- Large FB2.ZIP archives use a safer sequential extract-and-scan path instead of holding DEFLATE + XML processing in RAM simultaneously.
- Stabilized XTC/XTCH reading with low-memory streaming and reduced aggressive refresh behavior.
- Large EPUB/FB2 chapters remain internally split for RAM safety while being presented to the user as one continuous chapter.
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
- Added optional inkMOD support screen with QR/link to `@inkmodx4`.
- Fixed Lyra Carousel centering with seven icons.
- Improved small-cover sleep-screen handling to avoid ugly upscaling.
- Fixed several quick-action/button edge cases around Dictionary.
- Removed experimental idle light-sleep that could freeze X4 after USB disconnect; safe low-frequency idle mode remains.
- Split Developer and Release builds so production builds stay clean while USB flashing remains available.

## v1.1.2 — 2026-08-07

### Added

- Plain `.zip` archives are identified dynamically by their contents upon opening, eliminating strict `.epub`, `.fb2`, or `.fb2.zip` extension dependencies.
- Large EPUB, FB2 and image uploads enable browser-side optimization by default to offload processing from the device.
- Rebranded firmware and web portal as **inkMOD** with updated boot, sleep-screen and web assets.

### Changed

#### UI & Accessibility

- Enabling **Increase interface text** enlarges search fields, on-screen keyboards and help text while maintaining independent book fonts.
- On-screen keyboard avoids side-button areas and adds dedicated English, Russian and Ukrainian layouts.
- Moving the clock display to the bottom no longer leaves empty space reserved at the top.

#### EPUB Engine

- Layout respects semantic container groups and image `max-width` CSS rules.
- Standard `figleft` and `figright` classes correctly wrap text around illustrations.
- Pre-indexing of upcoming chapters is disabled during active reading to conserve memory on image-heavy books.

#### FB2 Engine

- Loading progress bars animate continuously during on-demand chapter map generation.
- Chapters split automatically when necessary to remain within the X4 memory budget.

### Fixed

- Dynamic list row-height calculation prevents label clipping with large UI text.
- Search queries ignore Cyrillic case and initialize in the selected interface language.
- **Reset Reader Data** preserves Wi-Fi and core system settings.
- OTA release builds embed their GitHub release tags to prevent repetitive update prompts.

For the complete development history and detailed changes, see [`CHANGELOG.md`](CHANGELOG.md).

---

# Building

inkMOD uses **PlatformIO**.

```bash
# Install dependencies into the PlatformIO Python environment
pip install -r requirements.txt

# Developer build
pio run -e developer

# Build and flash Developer firmware through USB
pio run -e developer -t upload

# Production Release build
pio run -e release

# Build and flash Production Release firmware
pio run -e release -t upload
```

---

# Xteink X4 / X3 Support

inkMOD is developed primarily for the **Xteink X4** e-reader while maintaining support for compatible **Xteink X3** hardware.

The firmware is designed around the hardware limitations of these devices, particularly the ESP32-C3 platform and limited available RAM.

Special attention is therefore given to:

- low-memory book processing;
- streaming instead of loading complete books into RAM;
- large FB2 and FB2.ZIP files;
- image-heavy books;
- reliable SD-card operation;
- fast page navigation;
- stable everyday reading.

---

# Support inkMOD

inkMOD is free, open-source and community-driven.

For project news, firmware releases, discussion and support:

**Telegram:** [@inkmodx4](https://t.me/inkmodx4)

The repository's **Sponsor** button points to the same project channel.

Support is completely optional and does not unlock or restrict any firmware functionality.

---

# Keywords

Xteink X4 firmware · Xteink X3 firmware · inkMOD · Xteink custom firmware · Xteink X4 FB2 · Xteink X4 FB2.ZIP · Xteink X4 EPUB · ESP32-C3 e-reader · FB2 e-reader · custom e-reader firmware
