# Changelog

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
- OTA releases now embed their GitHub tag version into the firmware, so a manually re-published update is not offered repeatedly after installation.
- The search keyboard now reserves an additional gutter around the X3/X4 side-button column and wraps its help text inside that safe area; Lyra's selected-value pill has a visible right inset.
- Large keyboard keys no longer draw their small alternate symbols over the main glyph. Empty subtitle callbacks no longer turn simple system lists into tall two-line rows.

### Removed
- Removed the online font-management screen and downloaded-font size-range setting.
