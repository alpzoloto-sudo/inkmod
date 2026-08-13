# inkMOD v1.1.3 — Xteink X4 / X3

This build is a re-upload of **v1.1.3** with post-release stability fixes and the latest InkMOD features.

## Highlights

- **Large FB2 stability:** reworked the FB2 index to reduce permanent heap usage on very large books. Tested with `Колесо Времени.fb2` (~46.2 MB) opening directly on the reader without preliminary conversion.
- **FB2 / FB2.ZIP:** direct native reading remains supported; heavy ZIPs use a safer sequential preparation path when needed.
- **XTC / XTCH:** low-memory streaming, reduced aggressive flashing and improved 2-bit page refresh behavior.
- **Large chapters:** huge EPUB/FB2 chapters can be split internally for RAM safety while remaining one logical chapter for the reader.
- **Dictionary:** in-book word selection, multiple dictionaries, article paging, StarDict synonyms, hyphen reconstruction and punctuation cleanup.
- **Clippings:** create and manage text clippings directly from EPUB/FB2 books.
- **EPUBKIT:** browser-side EPUB optimization for X3/X4.
- **Sleep-screen generator:** browser-side PNG preparation with background removal and direct upload to the reader.
- **Book-menu customization:** hide and reorder reader-menu entries.
- **File manager:** improved long filename display, rename support and additional long-press actions.
- **Power saving:** retained the safe CPU downclock path; experimental idle light-sleep that could freeze X4 after USB disconnect is not used.
- **UI fixes:** Lyra Carousel 7-icon centering, TXT clock duplication, dictionary/clipping overlays, localization and other small fixes.
- **InkMOD personality:** contextual humorous messages and a hidden easter egg.

## Post-release fixes included

- Fixed quick-action dictionary entry so the button used to open the dictionary does not immediately trigger an action inside it.
- Fixed the first Menu press being swallowed after leaving the dictionary.
- Added clipping removal/toggle behavior and corrected clipping-list text rendering.
- Reduced memory usage of the FB2 structural index for very large books.
- Prevented low-resolution embedded covers from being blindly enlarged into heavily pixelated sleep images.
- Fixed centering of seven icons in **Lyra Carousel**.
- Removed unsupported emoji from the e-reader's **Support InkMOD** menu label.
- Added a safe Support InkMOD screen with QR/link to the Telegram project channel.

## Supported devices

- Xteink X4
- Xteink X3

## Install

Download the release firmware `.bin` attached below and flash it using the normal InkMOD/Xteink update method.

For developers using PlatformIO:

```bash
pio run -e developer -t upload
```

Release build:

```bash
pio run -e release -t upload
```

## Support InkMOD

InkMOD remains free. If the project is useful to you, you can support development in the Telegram channel:

**@inkmodx4** — https://t.me/inkmodx4

Thank you to everyone testing books, reporting bugs and helping InkMOD improve.
