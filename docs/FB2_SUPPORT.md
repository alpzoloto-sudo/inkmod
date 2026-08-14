# FB2 support

The file browser accepts `.fb2` and `.fb2.zip` files.

## Architecture: lazy, one rendering engine

`Fb2::load()` does **not** convert the whole book up front. It runs one fast
scan pass over the FB2 (metadata + a flat list of every `<section>`'s byte
offset/level/id/title), writes the small `container.xml`/`content.opf`/
`toc.ncx`/`style.css` files an EPUB package needs, and persists a compact
per-section index in its cache dir (`/.inkmod/fb2_<hash>/`). It does **not**
render any chapter text - a real EPUB opens fast because its chapter XHTML
already exists inside the file, and this mirrors that instead of eagerly
generating hundreds of chapter files no one has asked to read yet.

Chapter text is rendered on first use, one `<section>` at a time, by
`Fb2::renderChapterOnDemand()` - `Epub::readItemContentsToStream()` calls it
whenever it's asked for a spine item inside a package carrying the
`.fb2_source` marker file (see `lib/Fb2/Fb2.h`; that marker is never written
for a real EPUB, so this can't affect normal EPUB reading). From there it's
indistinguishable from a real EPUB chapter: same `Section::createSectionFile()`
caching, same `ChapterHtmlSlimParser` → `ParsedText` → `Page` pagination
pipeline, same `sections/N.bin` cache. There is one rendering engine, not a
second one bolted on for FB2 - `Fb2Parser` only produces the XHTML text that
engine already knows how to consume, on demand instead of ahead of time.

Each `<section>`, at any nesting depth, is its own chapter/spine entry; file
count no longer matters for load speed since nothing is written eagerly.

Images are still extracted eagerly during `load()` (typically cheap, and a
chapter can reference one before it's ever rendered) into
`OEBPS/images/`, with an id→filename index persisted alongside the section
index so a later on-demand chapter render can resolve `<image>` references
without re-scanning the source.

For an `fb2.zip` archive, the embedded `.fb2` is extracted once into the
package's cache dir and kept there (not deleted after the first open) -
`renderChapterOnDemand()` needs to reopen the same source file on every
future chapter render, however much later that happens.

The generated package is a cache. It can be safely removed through the book
actions menu or by deleting its corresponding `/.inkmod/fb2_*` folder.

## Known limitations

- **Per-book "% position" estimate.** `BookMetadataCache`'s spine-size scan
  stats each chapter's *file* to build a cumulative-size estimate for the
  progress bar; since FB2 chapters aren't real files until rendered, this
  currently reads 0 for an FB2 book until sections have actually been
  visited. Doesn't affect correctness, just progress-bar accuracy - a
  follow-up could seed it from `Fb2SectionIndexEntry::approxTextBytes`
  (already computed by `scan()`, just not persisted today).
- In-book hyperlinks and footnote references (`<a href="#id">`) render as
  plain text instead of jumping to the target - the native parser's
  content-sink API doesn't expose arbitrary element ids, only `<section>`
  ids (which still back the table of contents). `text-author`/`date`/`code`
  runs also lose their distinct CSS class and render as plain
  paragraph/inline text.

## Parser

FB2/XML parsing and `.fb2.zip` extraction are done by the native,
dependency-free module in `lib/Fb2/native/` (no expat, no zlib/miniz).

- **Non-UTF-8 source encodings.** The native XML tokenizer assumes UTF-8 and
  otherwise just forwards raw bytes. A lot of real-world Russian FB2s declare
  `windows-1251` (or `koi8-r`) instead. `Fb2::prepareSource()` sniffs the
  `encoding="..."` attribute in the XML prolog and, if it's one of those two,
  transcodes the whole file to a UTF-8 copy (kept in the cache dir, since
  on-demand rendering needs to reopen it later) before parsing - same idea
  as the old expat `unknownEncoding` handler, just done up front instead of
  per-byte during parsing. Anything already UTF-8/ASCII, or a declared
  encoding outside that small table, is left as-is (best-effort passthrough).
- **Memory on very large books.** `Fb2Parser::scan()` returns one
  `Fb2SectionIndexEntry` per `<section>` in a single vector - fine for a
  typical book, but something like *War and Peace* has 600+ sections, and
  letting that vector grow by doubling both wastes space (rounds up to the
  next power of two) and transiently holds the old+new buffers at once
  during every reallocation. `native/Fb2Parser.cpp` does a cheap byte-level
  pre-count of `<section>`/`<binary>` tags before scanning so it can
  `reserve()` the exact size up front. `Fb2.cpp` also doesn't hold that
  vector past `load()`: it spills the (level, offset, id, title) each
  section needs down to a small on-SD file (read back later, one section at
  a time, by `renderChapterOnDemand()`) and releases the big in-RAM vector
  once that's written.
