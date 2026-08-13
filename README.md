# inkMOD

A community mod of the inkMOD e-reader firmware featuring Russian and Ukrainian interface localization, expanded format support, and comprehensive UI and reliability enhancements.

## Features

- **Localization**: Full Russian and Ukrainian interface support.
- **Formats**: Added support for `.fb2` and `.fb2.zip` natively alongside the base firmware formats.
- **Themes**: Multiple home-screen themes (**Lyra**, **Lyra Carousel**, **RoundedRaff**, **Minimal**, **Dashboard**), optimized for accuracy and performance.
- **Fonts**: Interface text-size toggle (*Large* vs. *Default*) alongside the reader's native font settings.
- **Converters & Web Portal**:
  - Built-in web-based FB2 → EPUB converter.
  - `.ttf` / `.otf` → `.cpfont` font conversion with automatic upload to the device.
  - Web upload supporting directory trees, converting files on the fly.
- **Clock & Time Sync**:
  - **X4 (no hardware RTC)**: Time syncs via Wi-Fi on connection and during background boot. Falls back to the last known successfully synced time when offline to prevent clock loss.
  - **X3 (hardware RTC)**: Native hardware support.
  - Option to toggle the clock off entirely in Settings (note: this disables the Calendar sleep screen).
- **Sleep Screen**: Added "Calendar" wallpaper with automated Wi-Fi time synchronization, including an inverted dark mode variant.
- **Reading Stats**: Detailed per-weekday reading statistics mirroring X3 firmware features.
- **OTA Updates**: Firmware update checks point directly to this repository's GitHub Releases, built and published automatically via GitHub Actions on tagged releases.

---

## Changelog

### v1.1.3 — 2026-08-10

#### Highlights
- Stabilized cached FB2/FB2.ZIP chapter rebuilding after font and layout changes without wiping reading progress or book caches.
- Fixed X4 Sunlight Fading Fix so the SSD1677 panel power rails are actually shut down after fast refreshes when the option is enabled, without an extra screen flash.
- Preserved the conservative X3/X4 display waveforms after experimental faster UI refreshes proved prone to ghosting.
- Fixed the false controls-submenu count error and completed the latest reading-statistics text-summary cleanup.

For the full list of changes, see `CHANGELOG.md`.

---

### v1.1.2 — 2026-08-07

#### Added
- Plain `.zip` archives are now identified dynamically by their contents upon opening, eliminating strict `.epub`, `.fb2`, or `.fb2.zip` extension dependencies.
- Large EPUB, FB2, and image uploads now enable browser-side optimization by default to offload processing from the device.
- Rebranded firmware and web portal as **inkMOD** with updated boot, sleep screen, and web assets.

#### Changed
- **UI & Accessibility**:
  - Enabling *Increase interface text* now enlarges search fields, on-screen keyboards, and help text while maintaining independent book fonts.
  - On-screen keyboard avoids side-button areas and adds dedicated English, Russian, and Ukrainian layouts.
  - Moving the clock display to the bottom no longer leaves empty space reserved at the top.
  - Keyboard key UI tweaks keep alternate sub-symbols clean, and list layouts prevent empty callbacks from forcing double-height rows.
- **EPUB Engine**:
  - Layout now respects semantic container groups (chapter title blocks, figure elements) and honors image `max-width` CSS rules.
  - Standard `figleft` and `figright` classes correctly wrap text around drop capitals and illustrations.
  - Drop-cap graphics align directly with the first line of text without artificial top gaps.
  - Float reservations persist beyond figure captions to prevent line collisions.
  - Preserves per-line inline offsets for poetry and typographic compositions (e.g., *Mouse's Tale*).
  - Included default **Bookerly** and **Roboto** font pack; the size menu now reflects every available family weight/size.
  - Pre-indexing of upcoming chapters is disabled during active reading to conserve memory on image-heavy books.
- **FB2 Engine**:
  - Loading progress bars animate continuously during on-demand chapter map generation.
  - Chapters now split automatically after two inline images to operate safely within the X4 memory budget.

#### Fixed
- **Typography & Display**:
  - Dynamic list row height calculation based on active font prevents label clipping on large text settings.
  - EPUB `div img { max-width: 100% }` renders images at full text width instead of falling back to thumbnail sizing.
  - Corrected edge-case margin reservations in poem offsets that previously squished text into narrow side columns.
  - E-ink status bar alignment fixed when changing clock positions during reading.
  - Search keyboard gutter adjustments protect button margins on X3/X4 hardware; Lyra selected values display visible right padding.
- **Memory & System Stability**:
  - Direct chapter slicing to SD index prevents OOM reboots on large, heavily illustrated FB2 books during import.
  - Dynamic memory management expands available headroom for JPEG image decodes without premature skipping.
  - Prevented stale font cache bleed across consecutive book loads.
- **General Fixes**:
  - *By book* page counting preference is preserved across sleep cycles.
  - Search queries ignore case sensitivity across Cyrillic character sets and auto-initialize in the selected interface language.
  - *Reset Reader Data* clears system caches while preserving network (`wifi.json`) and core (`inkmod-settings.json`) preferences.
  - OTA release builds embed their GitHub release tags to eliminate repetitive update prompts post-install.

#### Removed
- Legacy online font-management view and custom size-range configuration options.

---

### v1.0.3

#### Fixed
- **Orientation & UI**: Resolved control bar UI overlap across app screens (Settings, KOReader sync, OPDS, dialogs, etc.) when rotated to landscape.
- **Time Display**: Fixed string truncation bug (`Current time: 20:4`) caused by UTF-8 buffer bounds.
- **Font Kerning**: Resolved root issue causing specific digit pairs (e.g., `34`, `35`) to overlap or drop rendering by forcing glyph-by-glyph digital rendering across all UI elements.
- **Localization Fonts**: Fixed missing Cyrillic characters in **RoundedRaff** when operating in Large text mode.
- **EPUB Image Processing**: Implemented fallback regex rewriter for non-strict XHTML parsing during image optimization.
- **Stability**: Added global out-of-memory handlers with heap logging and increased image load memory margins to prevent silent device reboots on heavy media books.

---

### v1.0.2

#### Fixed
- Power-button font swap shortcut now correctly rotates through user-installed SD card fonts (`/Settings/Font`) instead of legacy built-in presets.

---

### v1.0.1

#### Added
- Added Dark / Inverted mode variant for the **Calendar** sleep screen.
- Persisted last NTP-synced time on X4 devices to prevent clock resets on deep power cycles.

#### Fixed
- **Lyra Carousel**: Fixed transient missing bitmap cover rendering issues.
- **RoundedRaff**: Added full header clears prior to e-ink partial refreshes to eliminate ghosting/smearing.
- **Localization**: Improved Ukrainian string accuracy and capitalization formatting.
- **CI/CD**: Pointed automatic update mechanisms to repository release channels.

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
---

## Support InkMOD

InkMOD is free and community-driven. If you find it useful and want to support further development and testing, visit the project Telegram channel:

**[@inkmodx4](https://t.me/inkmodx4)**

GitHub's **Sponsor** button for this repository links to the same project channel. Support is entirely optional and does not unlock or restrict any firmware features.
