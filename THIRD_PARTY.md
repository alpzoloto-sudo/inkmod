# Third-party and provenance register

This register is a migration guard, not legal advice. A source file must not
be copied into the new reader core until its origin and licence are confirmed
from the source distribution included in the build.

| Component | Current use | Provenance / licence status | Rule for new core |
| --- | --- | --- | --- |
| FreeInk SDK (`freeink-sdk/`) | X4 display, board, input, SD, battery, UI support | SDK README states MIT and describes a re-architecture derived from earlier display work; verify each copied source header before redistribution | Keep behind `src/hal`; prefer adapting public API over copying implementation |
| Arduino-ESP32 / ESP-IDF | Platform and Arduino runtime | Platform dependency; licence governed by the selected framework distribution | Use public APIs only |
| SdFat / SDCardManager | SD storage backend | Transitive/platform dependency | Access only via existing `HalStorage` or reader HAL adapter |
| ArduinoJson | settings/network JSON | Declared PlatformIO dependency | Keep out of reader core unless a reader cache format explicitly needs it |
| PNGdec / JPEGDEC | image conversion | Declared PlatformIO dependencies | Do not copy decoder code; invoke through a renderer/image adapter |
| expat, uzlib, InflateReader, ZipFile | XML/ZIP infrastructure | Mixed local/vendor history; per-file provenance review required | Reuse through narrow stream interfaces; record licence before any relocation |
| DejaVu Sans | Built-in 12 pt reader font (regular, bold, italic, bold-italic), compiled into generated font headers under `lib/EpdFont/builtinFonts/` | DejaVu changes are public domain; underlying Bitstream Vera glyphs retain the Bitstream Vera font licence, with any Arev-derived glyphs retaining Tavmjong Bah's notice. Full bundled text: `freeink-sdk/libs/book/FreeInkBook/test/fixtures/fonts/DEJAVU-LICENSE` | Keep the licence text in the source distribution and preserve required copyright/trademark notices when redistributing the embedded/generated font data |
| Fonts and generated font headers | UI and book glyphs | Assets may have their own licences | Keep source/font licence next to a manifest; do not assume MIT from firmware licence |

## Review checklist for every migration PR

1. Identify the source file's original repository and licence notice.
2. Preserve required copyright and licence text.
3. Record a link/commit or a local distribution version in this table.
4. If origin is uncertain, reimplement the small algorithm from behaviour and
   tests instead of copying code.
5. Do not import code from KOReader, CoolReader, MuPDF or CrossPoint merely as
   a reference implementation without a separate compatibility review.

