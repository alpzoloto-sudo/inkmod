# FB2 support

The file browser accepts `.fb2` and `.fb2.zip` files.

## Architecture: lazy, one rendering engine

`Fb2::load()` does **not** convert the whole book up front. It runs one bounded
scan pass over the FB2, writes the small `container.xml`/`content.opf`/
`toc.ncx`/`style.css` files an EPUB package needs, and persists compact section
and image indexes in its cache dir (`/.inkmod/fb2_<hash>/`). Chapter XHTML is
not generated until the chapter is actually requested.

Chapter text is rendered on first use by `Fb2::renderChapterOnDemand()`.
`Epub::readItemContentsToStream()` calls it whenever a spine item belongs to a
package carrying the `.fb2_source` marker. The generated XHTML then goes through
the same `ChapterHtmlSlimParser` → `ParsedText` → `Page` pipeline and the same
`sections/N.bin` cache as a normal EPUB. FB2 therefore has a native source
adapter, not a second page-layout engine.

Very large FB2 sections are exposed as bounded virtual spine items. Text-heavy
sections are split using approximate decoded-text bytes, while illustrated
sections are split by image references. This bounds first-use layout/image work
without loading the complete book or chapter into RAM.

The scan persists `approxTextBytes` for every virtual chapter. `BookMetadataCache`
loads those estimates in one sequential pass, so the whole-book progress
estimate is useful even before future FB2 chapters have been rendered.

## Images

Images are indexed during first open but **decoded lazily**. The persisted image
index stores the binary id and source byte range. When an image is first needed,
`decodeImageOnDemand()` seeks directly to that `<binary>`, streams its base64
payload through a bounded decoder, and writes only that image. Epub's normal
screen-sized pixel cache serves later renders, while a small LRU limits how many
large raw decoded source images remain on SD.

Image-index lookup uses fixed scratch buffers for skipped records, so reaching a
late `image_N` does not allocate id/name strings for every earlier image.

## FB2.ZIP

For an `fb2.zip` archive, the embedded `.fb2` is staged once in the package cache
and kept there because lazy chapter/image rendering needs a seekable source on
future reads. Small archives may fuse extraction and XML scanning; larger
archives use the lower-memory sequential path.

Release builds bypass the ZIP I/O profiler entirely; developer builds keep its
read/write timing counters. Sequential staging scales between 32, 16 and 8 KiB
according to free and contiguous heap so it cannot crowd out DEFLATE's mandatory
32 KiB history window. The window itself uses a checked `nothrow` allocation, so
low-memory extraction follows the normal failure path rather than causing an
allocation abort on the no-exceptions ESP32-C3 build.

## Persisted indexes and hot paths

Section ids/titles live in one bounded string pool during scanning instead of
hundreds of per-section string allocations. The on-SD section index stores only
the data needed later: source offset, level, id/title, approximate text size and
virtual image/text ranges.

Opening a late chapter scans skipped index records without materializing their
id/title strings. Logical virtual-chapter bounds are found in one sequential
pass. Package OPF/NCX generation writes chapter numbers, hrefs and anchors
directly to the output stream instead of constructing thousands of temporary
`std::string` concatenations on very large books.

XML scan-only entity decoding uses fixed local buffers, and section ids are
copied directly from tokenizer attributes into the shared pool. Normal release
FB2 scans also bypass the developer-only per-read I/O profiler.

The generated package is a cache. It can be safely removed through the book
actions menu or by deleting its corresponding `/.inkmod/fb2_*` folder.

## Links and formatting

Same-document links whose target is a known FB2 section id are resolved against
the persisted section index and emitted as XHTML links. The lookup intentionally
scans the compact SD index only when a link is encountered instead of keeping a
large id-to-chapter string map in RAM.

FB2 paragraphs, headings, emphasis/bold/underline/strike, superscript/subscript,
small-caps style hints, poems, cites/epigraphs, text authors, tables and images
are translated into the common XHTML/layout pipeline. Unsupported or malformed
constructs degrade to readable text rather than requiring a separate FB2
renderer.

## Parser and encodings

FB2/XML parsing and `.fb2.zip` extraction are handled by the native,
dependency-free module in `lib/Fb2/native/`.

The tokenizer expects UTF-8. If an FB2 declares a supported legacy single-byte
encoding such as `windows-1251` or `koi8-r`, `Fb2::prepareSource()` transcodes it
to a UTF-8 cache copy before indexing. A body sample first detects files whose
XML declaration incorrectly claims a legacy encoding even though the content is
already UTF-8, avoiding accidental double conversion.

Large books use bounded scanner buffers, deque-backed section/binary entries and
a hard-limited section string pool. Ordinary body text and base64 payloads are
counted without materializing strings during the metadata/index scan. Cancellation
checks and bounded read-ahead keep long parsing/image work interruptible while
avoiding tiny SD-card reads.
