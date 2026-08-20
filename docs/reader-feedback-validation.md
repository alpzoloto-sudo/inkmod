# Reader feedback validation matrix

Hardware validation checklist for the optimized Xteink X4 reader branch.

- FB2 -> EPUB web conversion: annotation remains visible and in metadata.
- Prepared FB2 cache: annotation survives the shared conversion/preparation path.
- Paragraph spacing `None` with Embedded styles enabled: normal body paragraphs remain compact while headings retain structural spacing.
- Reopen the same prepared/optimized book and verify section-cache v52 rebuild does not restore stale paragraph spacing.
- Front-button Chapter Skip: from the middle of chapter 8, forward opens the start of chapter 9, then the start of chapter 10; backward opens the start of the previous logical chapter.
- Repeat Chapter Skip across EPUB internal RAM-safe spine fragments; no logical chapter may be skipped.
- After Chapter Skip, the first following short or long page-turn press must be accepted.
- Press next/previous immediately while the destination chapter is still loading after Chapter Skip; the press must be queued and applied after the new section becomes ready rather than being discarded.
- Front and side long-press `10 pages`: verify +10/-10 within a chapter and across chapter/internal-fragment boundaries.
- Long-press Join Network from inside a book: reader position is saved, active reader work is cancelled, queued navigation is cleared, and the network/file-transfer activity becomes responsive without a forced reboot.
- Release the button only after the Join Network / File Transfer / Calibre / Hotspot screen appears; that delayed release must not be interpreted as Back/Confirm by the destination activity.
- Repeat the same transition checks for Calibre Wireless, Create Hotspot, and File Transfer.
- EPUB converted from FB2 and optimized in the web UI: with Embedded styles enabled and Paragraph spacing `None`, title/subtitle/poem structural spacing remains intact while ordinary body paragraphs stay compact.
- Future custom-CSS support must preserve the original EPUB file and apply conversion-time overrides only to the generated/optimized output.
