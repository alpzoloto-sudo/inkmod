# InkMOD 1.1.4 repaired source

This package is based on the exact working tree supplied after the regression photos.

## Repaired
- Runtime missing-glyph fallback is INCLUDED.
  A missing Unicode glyph in an SD/.cpfont reading font is rendered with the
  closest built-in Inter family instead of immediately showing a diamond/box.
- Diagnostics storage row is actionable again:
  `Нажмите «Выбрать»` -> `Загрузка...` -> used / total.
- SD total capacity now recalculates from the mounted filesystem if its cached
  capacity is unexpectedly zero.
- Reset reason remains human-readable in Russian.
- Time since last charge remains in Diagnostics.
- Progressive JPEG cover cache bumped to `cover_q2_v4` so stale tiny/blocky
  generated covers are not reused.
- Web FB2 cover optimizer keeps the progressive-JPEG normalization added earlier.

## Important progressive JPEG limitation
The on-device JPEGDEC path cannot reconstruct the full detail of a progressive
JPEG cover; it only has the reduced DC preview. Firmware can either keep that
preview small or enlarge it and make it blocky — neither can recreate missing
pixels. Therefore this recovery build does NOT fake a full-resolution result.

For a progressive cover to become genuinely sharp, upload/optimize the book
once through the InkMOD web interface. The browser re-encodes the cover as a
normal baseline JPEG while it still has the full source image.

## Build
    pio run -e developer -t clean
    pio run -e developer -t upload

Test this developer build before producing release.

## Runtime missing-glyph fallback

Fallback works at GfxRenderer level. Normal characters remain in the selected
reading font; only genuinely missing glyphs use built-in Inter 12/14.

This avoids loading a huge Unicode font into RAM.

Test:
`№ © ® ™ € £ ¥ → ← ± × ÷ ≠ ≤ ≥ Ω α β ё Є Ї Ґ`
