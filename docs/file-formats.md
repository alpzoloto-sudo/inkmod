# File Formats

These formats describe the SD-card cache files under `/.inkmod/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8.

## Prepared dictionaries (`/Dictionaries/<name>/`)

The web File Manager converts StarDict, DSL, TSV, CSV, and TXT dictionaries in
the browser. It uploads one `dictionary.idx` and one `dictionary.dat` file per
dictionary. The firmware does not parse, decompress, or index source formats;
it validates these files and performs a bounded binary search for each lookup.

All integers are little-endian and text is UTF-8. Format version 1 uses FNV-1a
32-bit hashes of normalized headwords. Records are sorted by unsigned hash;
the stored headword resolves the rare hash collision.

`dictionary.idx` layout:

- 16-byte header: `"IMDX"`, `version:u16` (1), `recordSize:u16` (12),
  `entryCount:u32`, `flags:u32` (0)
- `entryCount` records: `hash:u32`, `dataOffset:u32`, `dataLength:u32`

`dictionary.dat` layout:

- 12-byte header: `"IMDD"`, `version:u16` (1), `reserved:u16` (0),
  `entryCount:u32`
- variable records referenced by the index: `headwordLength:u16`,
  `articleLength:u32`, then the headword and article bytes

The browser limits a headword to 95 UTF-8 bytes and an article to 8192 bytes.
Firmware opens only one SD file at a time: it collects matching index records,
closes the index, and then opens the data file for the final comparison/read.

## `book.bin`

### Version 7

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 7
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## Reader session state (`reader_state_v1.bin`)

The in-progress format-neutral reader stores only a fixed 40-byte state record.
It contains the logical page anchor, page-counter mode, orientation, font scale,
layout revision and flags. The last four bytes are a CRC32 over the preceding
36 bytes. A bad magic, version or CRC means the record must be ignored and the
reader opens at its normal fallback position. This avoids JSON allocation during
wake and ensures an interrupted SD write cannot silently restore a wrong page.

Byte layout, little-endian: `magic(4)`, `version(1)`, `counterMode(1)`,
`orientation(1)`, `fontScale(1)`, `chapter(4)`, `byteOffset(8)`,
`nodeOffset(4)`, `pageNumber(4)`, `flags(4)`, `layoutRevision(4)`, `crc32(4)`.

## New reader chapter source cache (`reader_v1/`)

The staged EPUB reader copies only the chapter being opened to
`<epub-cache>/reader_v1/chapter_<spine>.xhtml`. It is streamed in 512-byte
chunks from the EPUB/package, synced, closed, then atomically renamed from a
temporary file. A failed preparation removes the temporary file and retains a
previous completed chapter source. The cache never contains a complete book.

## `section.bin`

### Version 40

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 40 includes:

- cache-busting fields for font, line compression, extra paragraph spacing,
  forced paragraph indents, paragraph alignment, viewport size, hyphenation,
  embedded CSS, image rendering mode, Bionic Reading, and Guide Dots
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- optional per-word Bionic Reading split metadata
- optional per-word Guide Dot x-offset metadata
- table fragments
- per-page footnote entries
- per-page publisher page markers

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 40
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageTableFragment = 3,
    TAG_PageHorizontalRule = 4
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
};

struct TextBlock {
    u16 wordCount;
    String words[wordCount];
    s16 wordXPos[wordCount];
    WordStyle wordStyle[wordCount];

    u8 hasFocus;
    if (hasFocus != 0) {
        u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
    }

    u8 hasGuideDots;
    if (hasGuideDots != 0) {
        u16 wordGuideDotXOffset[wordCount] [[comment("Guide dot x offset from word start; 0 means no dot")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct TableFragmentCell {
    bool isHeader;
    u8 lineCount;
    TextBlock lines[lineCount];
};

struct TableFragmentRow {
    u16 height;
    bool headerSeparator;
    u8 cellCount;
    TableFragmentCell cells[cellCount];
};

struct PageTableFragment {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 columnCount;
    u8 cellPadding;
    u16 lineHeight;
    u8 rowCount;
    TableFragmentRow rows[rowCount];
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageTableFragment) {
        PageTableFragment tableFragment [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
};

struct PublisherPageMarker {
    s16 yPos;
    char label[16];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];

    u8 publisherPageMarkerCount;
    PublisherPageMarker publisherPageMarkers[publisherPageMarkerCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    bool forceParagraphIndents;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool bionicReadingEnabled;
    bool guideReadingEnabled;

    u16 pageCount;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```
