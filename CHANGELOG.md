# Changelog

## [v1.1.7] - 2026-09-03

### Added
- Quick Sync for KOReader progress synchronization.
- OPDS catalog caching and improved book downloads.
- Browser-side cache preparation for EPUB, FB2 and supported ZIP books.

### Changed
- Improved FB2 synchronization accuracy.
- Improved EPUB/FB2 image and cover handling.
- Improved X3/X4 theme compatibility and stability.
- Restored fast page-by-page scrolling in the file browser.

### Fixed
- Fixed FB2 empty lines, subtitles, poetry and citation synchronization.
- Fixed EPUB SVG covers and some blank image pages.
- Fixed EPUB/EPUB.ZIP covers in home themes.
- Fixed X3 cover generation issues in Lyra themes.
- Fixed large interface font layout issues.
- Fixed X3 header date placement across themes.
- Fixed font ZIP uploads containing Windows-style paths.
- Improved file handle cleanup and backup rotation.
## [v1.1.6] - 2026-08-27

### Added
- Expanded FB2 semantic styling for annotations, epigraphs, headings, subtitles, quotations, poems, stanzas and text authors.
- Added faithful `<emphasis>` handling in FB2.
- Added Create Clipping as an assignable button / long-press action.
- Added held-button repeat for ordinary menus and lists.
- Added improved PNG/BMP handling in File Browser and sleep-screen selection.

### Changed
- Reworked large FB2/FB2.ZIP and EPUB processing around low-memory streaming.
- Improved preservation of author formatting during streaming.
- Improved verse wrapping and continuation indentation.
- Improved logical chapter pagination across internally split spine fragments.
- Dictionary article pagination was heavily optimized.
- File extension setting renamed to “Show file extensions” with corrected switch semantics.

### Fixed
- Fixed rare split words such as `п ривет` caused by streaming boundaries.
- Fixed logical chapter counters jumping from values such as `40/40` to `41/45`.
- Fixed the first clipping-selection press sometimes being ignored.
- Fixed PNG/BMP format reporting in File Browser.
- Fixed custom sleep-screen selection changing the Lock Screen mode.
- Removed the need to create a duplicate `/sleep.bmp` when setting a new custom image.

## [v1.1.5] - 2026-08-20

### Added
- Added safe factory-calibration based display revision selection for Xteink X3/X4 without probing the E-Ink bus before display initialization.
- Added full X3 UC8279 revision routing through the dedicated `XteinkX3Uc8279` hardware profile while keeping original X3 units on UC8253.
- Added known X4 UC8179/UC8279 revision selection with fail-safe fallback to SSD1677 when factory calibration is missing or unknown.
- Added emergency SD-card firmware recovery: place a compatible firmware image named `inkmod-recovery.bin` in the root of the SD card and restart the reader. Successful recovery renames it to `inkmod-recovery.applied.bin` to prevent a reflash loop.
- Added a dedicated `RECOVERY.md` guide and versioned 1.1.5 release notes.

### Changed
- X4 EPD SPI now uses the manufacturer-compatible 10 MHz rate for additional signal margin across display revisions.
- SSD1677-specific booster setup is emitted only when the selected X4 controller is actually SSD1677.
- Unknown X3/X4 factory display identifiers now fail closed to the original controller for that device family instead of guessing another panel controller.

### Fixed
- Fixed Lyra Carousel reading-progress percentage clipping at the right edge.
- Removed the unsafe early display-bus auto-probe path that could leave the reader apparently frozen before normal UI/button processing.

### Recovery
- Normal manual flashing remains at application offset `0x10000`.
- Ordinary updates should not use `erase_flash`; preserving factory NVS keeps the display-revision calibration used by 1.1.5.
- See `RECOVERY.md` for USB and SD-card recovery procedures.

## [v1.1.4] - 2026-08-14

### Added
- Added a separate power-button **Quick Resume sleep** action that uses the same preserved-frame wake path as automatic timeout sleep, without changing the existing normal Sleep action.
- Added a built-in **DejaVu Sans** reader family using DejaVu Sans 12 pt (regular, bold, italic and bold-italic) as a starter font while preserving existing SD-card font selections.
- Added an on-device **Diagnostics** screen with firmware variant, device type, free heap, largest allocatable block, SD size, reset reason and crash-report status.
- Added a low-overhead RTC breadcrumb trail for release builds. OOM-triggered restarts now persist the last reader actions into `/crash_report.txt` on the next boot, even when serial logging is compiled out.
- Added **Book information** to the File Browser long-press menu with format, source size, cache state and a lightweight load-profile hint.
- Added separate timeout sleep-screen profiles, including a dedicated custom overlay/image for automatic sleep while preserving Quick Resume behavior.

### Release engineering
- GitHub Releases now build the real `release` PlatformIO environment and publish `firmware-release-v<version>.bin`, matching what release OTA clients search for.
- CI now builds both developer and release environments. Release validation rejects an accidentally re-enabled runtime EPUB pre-splitter and verifies the production artifact naming path.

### Changed
- Runtime EPUB opening now uses the proven direct EPUB → BMC → ERS pipeline. The optional pre-splitter remains in-tree for future work/tests but is no longer called from `ReaderActivity`, eliminating a release-only reboot observed before normal cache creation.
- Large EPUB logical fragments are prepared on demand instead of synchronously paginating all sibling fragments before showing the first page.
- Large FB2 indexing uses a compact pooled section index and safer ZIP extraction thresholds to reduce persistent heap pressure on ESP32-C3.
- Lyra Carousel now keeps equal safe margins at both screen edges with seven icons, including the selection highlight.
- Cover mode/filter stay visible in Sleep Screen settings instead of disappearing when another wallpaper mode is selected.

### Fixed
- Fixed the inverted master clock switch in the web settings page.
- Sleep-screen mode, timeout screen, cover fit and cover filter are now all available in the device settings.
- Reader screen margins now start at 5; the legacy value 1 is no longer offered.
- Reading-statistics cards now use rounded corners, and the no-clock device summary has a concise title.
- Removed the redundant `Back — НЕТ / OK — ДА` text from the easter-egg prompt because the actual button hints are already rendered at the bottom.
- Removed the experimental idle light-sleep path that could freeze X4 after USB disconnect while keeping safe low-frequency idle behavior.


### Added
- Added a single reader-work controller for EPUB, FB2 and FB2.ZIP chapter preparation. Navigation invalidates obsolete work at bounded ZIP/XML/image checkpoints without starting a second parser, SD reader, or background worker.
- Added adaptive `normal`, `safe`, and `survival` layout modes. Under memory pressure the reader progressively releases the SD font and omits optional CSS, images, hyphenation, and reading effects before reporting an out-of-memory error.

### Changed
- Rapid page and chapter input is now coalesced while an uncached section is being prepared. Only the final requested position is rendered, and accumulated page turns carry across internal chapter fragments.
- Large section page indexes are spooled as linked fixed records inside the temporary section cache instead of growing a RAM vector with every generated page or opening another SD file.
- The reader prepares only the requested virtual fragment. It no longer pre-paginates every sibling fragment of the same logical chapter while holding the render lock.
- Sleep-screen preparation is now cache-only. It never starts an invisible chapter rebuild while the device is trying to sleep.
- Lazy FB2 image decoding now uses a fixed 4 KiB write buffer and removes truncated output after cancellation, read failure, or SD write failure.

### Fixed
- Cancelled section builds are written to temporary files and discarded before promotion, so rapid navigation cannot replace a valid cache with a partial one.
- Section cache promotion preserves the previous file as a short-lived backup and restores it after an interrupted replacement.
- Safe and survival layouts use separate cache names and are reused by the reader, logical chapter counter, and sleep-page renderer without overwriting full-quality caches.

## [v1.1.3] - 2026-08-10

### Added
- Added a dedicated browser-side EPUB Kit page with X3/X4 profiles, Quick/Full/Custom presets, metadata editing, the original 20-stage optimization flow, and direct upload of the finished EPUB to a selected device folder.
- EPUB Kit now prepares 4-level Floyd–Steinberg images, baseline JPEGs, Light Novel spreads, XHTML/CSS/metadata cleanup, font removal, generated covers, repaired references, and regenerated NCX tables of contents in the browser.
- EPUB Kit repackages books with lazy ZIP reads and zero-copy reuse of unchanged compressed entries, keeping only the current image or document in working memory for phone-friendly processing.
- Reader dictionaries with word selection, an article popup, multi-page definitions, and switching between installed dictionaries.
- Browser-side dictionary preparation and upload from folders or ZIP archives. StarDict, DSL, TSV, CSV, and TXT sources are converted into a compact on-device binary-search index before transfer.
- StarDict synonym tables (`.syn`) can now be prepared in the browser, allowing inflected word forms to resolve directly to their canonical dictionary articles without on-device morphology processing.
- System settings now include a protected "Boot other firmware slot" action.
  It validates the inactive OTA image before confirmation and then restarts
  into it, making rollback possible without exposing a blind slot toggle.
- Sleep-screen cover mode now includes a black background for letterboxed cover art on dark-bezel readers.
- Internal reader-core milestone: EPUB metadata and chapter information now
  have a format-neutral, fixed-buffer catalog adapter. This does not change the
  current reading screen yet; it prepares the unified EPUB/FB2 reader without
  adding a second full-book memory copy.
- FB2 now has the same internal catalog boundary, reusing its on-SD section
  index instead of scanning or rendering the source again for library data.
- Internal reader-core now includes a format-neutral `ReaderSession` with an
  eight-entry fixed page-anchor history; it is not yet the active reader UI.
- Reader preparation now has a fixed-capacity internal job queue for explicit
  extract, index, pagination, image, and cache-cleanup phases.
- EPUB and FB2 now share an internal byte-based progress estimator that stays
  independent of the selected font and page layout.
- New reader cache namespaces now have a stable internal book identity derived
  from the source path and signature, ready for versioned cache migration.
- Added a format-neutral bounded render-command executor, separating page
  layout from the future e-ink display adapter.
- EPUB chapter preparation for the new reader now streams one spine entry into
  a versioned, atomically-written SD cache source instead of buffering HTML in RAM.
- The internal reader state now has a compact versioned binary record with a
  CRC check. It preserves the selected page-counter mode and logical position
  safely across a future sleep/wake integration.

### Documentation
- Added a coursework-ready architecture summary with module mapping, embedded
  memory constraints, verification procedure, and staged migration limits.

### Changed
- Reading-statistics time-of-day and weekday summaries now show exact text durations instead of relative bars.
- Large StarDict dictionaries are now decompressed, indexed, and sorted in bounded chunks backed by browser storage. ZIP entries are streamed directly as well, keeping preparation usable on phones instead of retaining the complete archive, article data, and synonym index in RAM.

### Fixed
- Removed four embedded NUL bytes from the home-screen carousel cache-key source. The separators and cache-key behaviour are unchanged, but the C++ compiler no longer reports `null character(s) preserved in literal`.
- Idle operation no longer starts a battery ADC conversion on every main-loop pass or polls the X3 fuel gauge over I2C dozens of times per second. Release builds also stop emitting unchanged heap telemetry every ten seconds, while error and out-of-memory diagnostics remain available.
- `Loading chapter` is shown over the last visible reader page again. Parser callbacks no longer redisplay the framebuffer while it is being reused for illustration conversion, preventing both the white-background regression and temporary decoder pixels behind the popup.
- EPUB spine items above the safe pagination size are now split into bounded logical fragments from about 300 KB instead of only after 1 MB. Discovery metadata is released before extraction, ZIP entry names are staged on SD instead of retained as an unbounded RAM list, existing v48 EPUB page caches remain reusable, and generated fragments retain the original `<body>` styling while still appearing as one chapter.
- Large FB2 sections now place virtual text and illustration-slice boundaries between complete text blocks instead of inside a paragraph, preventing torn sentences and artificial blank space at internal chapter joins.
- Dictionary articles now expose dictionary cycling on the visible confirm-button slot; side Up/Down shortcuts remain available, while Left/Right continue paging through the article.
- EPUB/FB2 books now increment `Completed books` when the real End-of-Book screen is reached even if the optional 99% prompt was skipped. XTC/XTCH books now persist the same one-time completion marker and are included in per-book home statistics.
- Dictionary lookup now searches the complete highlighted word before any line-break spelling variant, so `ку-да` and `реше-ний` can no longer resolve to the valid but unrelated short entries `ку` and `реш`.
- Dictionary articles now use the active book font directly and prewarm only the visible article page. Their typeface and size match the reader, while comprehensive SD fonts load Wiktionary IPA characters before drawing instead of showing replacement diamonds.
- Two-bit XTC/XTCH pages now pause briefly after their grayscale LUT pass so rapid page turning cannot start the next BW refresh while the panel is still settling; one-bit XTC timing is unchanged.
- Dictionary selection now excludes surrounding punctuation, treats visible line-break fragments as one word, skips the second fragment during horizontal navigation, and tries both joined and retained-hyphen spellings during lookup.
- Dictionary articles now follow the reader's text-size range, leave the unused confirm-button slot blank, and vertically center the page counter above the button row.
- Dictionary word selection now prewarms the active reader font before redrawing the page, preventing custom SD-card fonts such as Bookerly from showing every book glyph as a replacement diamond.
- Cached FB2/FB2.ZIP books can now rebuild a chapter after a font or layout change even if the prepared source copy was evicted: the cache keeps the original book path and repairs the prepared source in place without clearing progress, bookmarks, statistics, or already-built chapter caches.
- X4 Sunlight Fading Fix now explicitly powers down the SSD1677 analog/clock rails after a fast refresh when enabled, without adding an extra white/black image flash; X3 display drivers are unchanged.
- Controls settings validation now accepts the six front-button actions actually exposed by the UI instead of logging a false submenu-count error.
- Opening consecutive chapters in an illustrated FB2 no longer rebuilds a
  full in-memory cross-reference table for every page, avoiding heap
  fragmentation and restarts on ESP32-C3.
- Reading-statistics buckets with no reading time now display `0 min` instead
  of `< 1 min`; only a real non-zero sub-minute session uses the latter label.
- Opening a cached, illustration-heavy FB2 no longer loads the complete book-wide image index into RAM for every chapter; only the current chapter's images are retained while it is rendered.
- Prevented EPUB opening from restarting the device when an unusually large package manifest is inspected after other books: optional streaming preparation now safely skips that non-essential pass.
- Web uploads now optimise every oversized embedded FB2 illustration, not only the cover image.
- When a chapter must skip images because of low memory, the reader now stays
  on the text page instead of replacing it with a blocking warning screen.
- Backspace in the on-screen keyboard now removes a complete UTF-8 character.
  Russian and Ukrainian letters no longer leave an unknown-symbol marker in
  search fields; cursor-mode left/right movement also stays on character edges.
- Opening an EPUB, TXT, or XTC now handles a reader-object allocation failure
  as a recoverable error instead of relying on throwing `new` on ESP32-C3.
- FB2 preparation now reports 100% only after its final metadata and cache
  maintenance work completes.
- Looking up FB2 chapter sizes no longer allocates temporary strings while
  scanning the on-SD section index, reducing heap fragmentation on large books.
- FB2 virtual chapters split around illustrations now retain balanced
  paragraphs, quotes, poems, and tables. Illustration-heavy books no longer
  stop at a later "mismatched tag" XML error after initially opening normally.
- Glyph drawing now clips at the logical display edge before entering the
  framebuffer path. Malformed or intentionally offset typography can no longer
  flood the serial log with out-of-range pixel writes.
- Valid image-only FB2 sections no longer appear as metadata-cache errors while
  their reading-progress estimate is being assembled.
- Oversized EPUB spine files can now use the existing SD-streaming splitter at
  safe X4 heap levels instead of being rejected before preparation.
- FB2 collections with named main bodies now retain their real book and chapter
  titles in the table of contents instead of falling back to `Section N`.

## [v1.1.2] - 2026-08-07

### Changed
- The accessibility setting **Increase interface text** now also enlarges search fields, the on-screen keyboard, and keyboard help text, while keeping the reader's book font independent.
- EPUB layout now respects semantic book containers such as chapter title groups and figures, and honours image `max-width` rules. This preserves more of publishers' printed-edition composition, including half-page illustrations.
- EPUBs that use standard `figleft` and `figright` image classes now wrap the following text around drop capitals and side illustrations.
- Drop-cap illustrations now align with the first text line instead of retaining a paragraph-sized gap above it.
- Float reservations now continue past figure captions, so following text no longer draws through right-aligned illustrations.
- EPUB poetry and typographic compositions now preserve per-line inline offsets, including the curved layout of "Mouse's Tale".
- The reader now uses the supplied Bookerly and Roboto SD-card font pack. Bookerly is the default, and the size menu shows every size present in the selected family.
- Rebranded the firmware and web portal as inkMOD, with a new boot, sleep-screen, and web logo.
- EPUB and FB2 readers no longer pre-index the next chapter while you are still reading, avoiding unexpected work when leaving image-heavy books.
- Plain `.zip` files are now identified by their contents when opened, so EPUB and FB2 books no longer need `.epub`, `.fb2`, or `.fb2.zip` in their filename.
- Large EPUB, FB2, and image uploads now enable browser-side optimization by default, keeping image preparation off the reader.
- The on-screen keyboard now stays clear of the side buttons and supports English, Russian, and Ukrainian layouts.
- Moving the reader clock to the bottom no longer reserves an empty row at the top of the page.

### Fixed
- List rows now derive their height from the active interface font, preventing large-text labels from touching neighbouring rows. The current-theme marker is a compact reserved pill instead of covering the selected title.
- FB2 loading now keeps its progress bar moving while the on-demand chapter map is built, and no longer probes every not-yet-created virtual chapter file.
- EPUBs that use `div img { max-width:100% }` now display chapter illustrations at full text width instead of their small source thumbnail size.
- Opening several books before an illustration-heavy FB2 no longer carries the previous reader font cache into the next image decode; FB2 chapters now split after two images to keep decoding within the X4 memory budget.
- EPUB line-level poem offsets now reserve a readable line width instead of pushing text into a narrow column at the screen edge.
- The reader's "By book" page counter now remains selected after waking from sleep.
- JPEG illustrations now remain available at safe memory levels where the previous PNG-sized memory reserve unnecessarily skipped them.
- Very large FB2 files now write their chapter slices directly to the SD-card index instead of allocating a second in-memory chapter list during import, preventing X4 restarts while indexing illustration-heavy books.
- Search results no longer incorrectly label the first result as the selected value.
- Changing the reader clock between top and bottom now repaginates the chapter, preventing text from overlapping the status bar.
- File search now ignores Russian and Ukrainian letter case and starts its keyboard in the interface language.
- Reset Reader Data now clears every `.inkmod` entry except `wifi.json` and `inkmod-settings.json`.
- OTA releases now embed their GitHub release tags to prevent repetitive update prompts.
- The search keyboard now reserves an additional gutter around the X3/X4 side-button column and wraps its help text inside that safe area; Lyra's selected-value pill has a visible right inset.
- Large keyboard keys no longer draw their small alternate symbols over the main glyph. Empty subtitle callbacks no longer turn simple system lists into tall two-line rows.

### Removed
- Removed the online font-management screen and downloaded-font size-range setting.
