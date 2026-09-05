<h1 align="center">⚠️ DISCLAIMER ⚠️</h1>

<p align="center">
<b>By installing inkMOD, you acknowledge that you are doing so entirely at your own risk.</b>
</p>

<p align="center">
The developers and contributors of inkMOD accept <b>no responsibility or liability</b> for any damage, loss of functionality, software corruption, or bricked hardware that may occur to your device during or after the installation process.
</p>

<p align="center">
<i>Proceed with caution and ensure you follow all instructions carefully.</i>
</p>


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
- Streaming, memory-conscious handling of large FB2/FB2.ZIP and EPUB chapters on Xteink X3/X4 without intentionally stripping book formatting or images.

### 🔤 Reading & Typography

- Word hyphenation for improved text layout.
- Custom reader fonts.
- Unicode fallback for missing characters when possible.
- Improved handling of large chapters and image-heavy books.
- Expanded FB2 semantic styling: annotations, epigraphs, headings, subtitles, quotations, poems, stanzas and text authors.
- FB2 `<emphasis>` follows the book markup, including mixed italic/regular text inside styled blocks.
- Wrapped verse lines keep a distinct continuation indent instead of being flattened into ordinary prose.
- Logical FB2 chapters can span multiple internal spine fragments while retaining a stable chapter page total.
- `Paragraph spacing: None` keeps ordinary prose compact while preserving heading and structural spacing when embedded book styles are enabled.
- Long-press page controls can jump by logical chapter boundaries or by 10 pages.
- Text clippings for EPUB and FB2.
- Save and manage selected text excerpts directly on the device.
- Create Clipping can be assigned to configurable physical-button actions / long presses.
- Faster held-button navigation in clipping selection and other supported navigation screens.
- Customizable book menu ordering and visibility.
- General menu/list navigation supports held-button repeat after roughly 0.5 s, while specialized screens keep their own accelerated behavior.

### 📖 Dictionaries

- Dictionary lookup directly while reading.
- Multiple installed dictionaries.
- StarDict dictionary support.
- Long dictionary articles with page navigation.
- Reworked dictionary article pagination: measured test cases dropped from roughly 21–25 seconds to about 2 ms for pagination after font metrics are prepared.
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
- Book/file information available directly from the File Browser.
- PNG and BMP are handled consistently as images, including format reporting and sleep-screen selection.

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

inkMOD is a fork of [Crosspoint](https://github.com/crosspoint-reader/crosspoint-reader).

### 💖 Support & Community

* 📢 **Telegram Channel:** [t.me/inkmodx4](https://t.me/inkmodx4)
* 💸 **Donate via Crypto Bot:** [Send Tip](https://t.me/send?start=IVwqEXbSLhTR)
* [☕ Buy me a Coffee]([https://secure.wayforpay.com/tips/t76dfc3462ce1](https://send.monobank.ua/jar/9p1oM8v2sa]))

Support is completely optional and does not unlock or restrict any firmware functionality.

---

# Keywords

Xteink X4 firmware · Xteink X3 firmware · inkMOD · Xteink custom firmware · Xteink X4 FB2 · Xteink X4 FB2.ZIP · Xteink X4 EPUB · ESP32-C3 e-reader · FB2 e-reader · custom e-reader firmware

