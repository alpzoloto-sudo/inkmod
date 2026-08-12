# inkMOD v1.1.3

This release focuses on stability and display behavior while preserving the current EPUB/FB2 memory budget and all existing interface themes/settings.

### Main fixes

- FB2/FB2.ZIP cached books can recover the prepared source needed to rebuild a chapter after changing the reader font or layout, without clearing reading progress, bookmarks, statistics, or unrelated chapter caches.
- X4 Sunlight Fading Fix now performs a real SSD1677 analog/clock power-down after fast refreshes when enabled. It does not add a second white/black redraw, and the X3 display path is unaffected.
- Removed the false controls-submenu validation error for the six available front-button actions.
- Reading-statistics time-of-day and weekday summaries use exact text durations rather than relative bars.

### Stability policy

The release keeps the conservative production display waveforms. Experimental faster UI refresh paths that caused persistent ghosting are not included.
