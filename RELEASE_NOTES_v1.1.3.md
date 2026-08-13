# inkMOD v1.1.3

This is the refreshed **v1.1.3** release with the post-release stability fixes included.

The main focus is memory stability on Xteink X3/X4, large FB2/FB2.ZIP books, dictionary/clippings usability, XTC stability and the expanded InkMOD web tools.

## Main changes

### FB2 / FB2.ZIP stability

- Reworked large-book indexing to reduce heap pressure on ESP32-C3.
- Avoids large contiguous allocations during FB2 scan.
- Uses a compact pooled index for section IDs/titles instead of hundreds of independent strings.
- Larger FB2.ZIP files use a safer sequential extract-to-SD → scan path.
- Large chapters can still be internally split to protect RAM while remaining one logical chapter to the reader.
- Cached FB2/FB2.ZIP books can rebuild required chapters after font/layout changes without wiping progress, bookmarks or statistics.
- Verified on a real **~46 MB FB2 book** opened directly on Xteink without preliminary conversion.

### XTC / XTCH

- Low-memory streaming path avoids allocating oversized page buffers.
- Reduced aggressive refresh behavior and excessive flashing.
- Improved stability during rapid page turning.

### Dictionary

- Dictionary lookup directly from the reader.
- Multiple installed dictionaries.
- Long article paging.
- Cleaner word selection: punctuation is removed from queries.
- Hyphenated line-break words are reconstructed before lookup.
- StarDict synonym tables are supported.
- Browser-side dictionary preparation/upload added.
- Fixed quick-action input handling so the button used to open Dictionary no longer immediately triggers an extra action.

### Clippings

- Added text clippings for EPUB and FB2.
- Saved highlights can be viewed and managed.
- Selecting the exact same clipping range again can remove it.
- Fixed clipping-related UI labels and UTF-8 handling.

### Web interface

- Added **EPUBKIT** browser-side EPUB optimizer for X3/X4.
- Added dictionary preparation/upload.
- Added sleep-screen generator with:
  - X3/X4 presets
  - Fit / Fill / Stretch
  - grayscale / contrast / brightness
  - edge-connected background removal
  - real PNG alpha transparency
  - direct upload and apply to the device
- Added file rename support.
- Added project-support link to `@inkmodx4`.

### UI / themes

- Book menu entries can be reordered and hidden.
- Long filenames can actually wrap to a second line in File Browser.
- Fixed Lyra Carousel alignment when seven icons are enabled.
- Improved small cover handling on sleep screens so tiny embedded covers are not blindly stretched to full-screen size.
- Added InkMOD-style humorous fallback messages for some non-critical edge cases.

### Power / device stability

- X4 Sunlight Fading Fix performs proper display power-down without an extra redraw.
- Removed experimental `esp_light_sleep_start()` idle path that could freeze X4 after USB disconnect.
- Safe low-frequency idle mode remains enabled.
- Release build keeps production logging disabled while USB flashing remains available.

## Support

InkMOD remains completely free.

If you want to support development and testing, visit:

**Telegram: [@inkmodx4](https://t.me/inkmodx4)**

Support is optional and does not unlock or restrict any firmware functionality.
