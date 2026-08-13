// Fb2Types.h
//
// FB2 has no "files" the way EPUB has spine hrefs — it's one XML stream with
// nested <section> elements. To slot into CrossPoint's existing
// BookMetadataCache / book.bin shape (see docs/file-formats.md: Metadata,
// SpineEntry, TocEntry) with minimal changes downstream, each <section> is
// treated as one spine entry, addressed by a synthetic href of the form
// "#fb2sec:<index>" instead of a real file path. cumulativeSize is an
// estimate (decoded-text byte count) used the same way EPUB's is: to seed
// progress-bar math before real pagination has happened.

#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

struct Fb2Author {
    std::string firstName;
    std::string middleName;
    std::string lastName;
    std::string nickname;
};

struct Fb2Metadata {
    std::string title;                 // book-title
    std::string author;                // formatted "First Middle Last" of the
                                        // first <author>, joined with "; " if
                                        // there are more — matches EPUB's
                                        // single `author` string field
    std::vector<Fb2Author> authors;     // full structured list, for callers
                                        // that want more than the flattened
                                        // string (e.g. an "About" screen)
    std::string language;              // <lang>
    std::string coverBinaryId;          // id= of the <binary> referenced by
                                        // <coverpage><image l:href="#id"/></coverpage>,
                                        // WITHOUT the leading '#'
    std::string annotationText;        // flattened <annotation> paragraphs,
                                        // plain text, for a library-view blurb
    std::string sequenceName;          // <sequence name="..."> if present
    uint32_t sequenceNumber = 0;

    // Raw CSS text from <description><stylesheet type="text/css">...</stylesheet>,
    // if the producer embedded one (mirrors the `embeddedStyle` cache-busting
    // flag EPUB sections already carry per docs/file-formats.md — same idea:
    // a CSS-aware layout pass can pull line-height/letter-spacing/etc. rules
    // out of this instead of the parser inventing typographic APIs for
    // properties FB2's tag set has no element for). Empty if none present.
    std::string embeddedStylesheetCss;
};

// One entry per <section> found anywhere under any <body>. The scan keeps
// only fields needed to write the on-SD index or render a chapter later.
// Parent/end offsets are deliberately omitted: retaining them for hundreds
// of sections exhausts the ESP32-C3 heap before the index can be persisted.
struct Fb2SectionIndexEntry {
    uint32_t innerStartOffset = 0; // offset right after the opening <section ...> tag
    uint16_t level = 0;            // nesting depth, 0 = direct child of <body>
    int16_t bodyIndex = 0;         // which <body> this section lives under

    // IMPORTANT: do not put std::string here.
    //
    // A 700-section anthology previously kept two std::string objects in
    // EVERY section entry even when most IDs were empty. On ESP32-C3 the
    // object/capacity overhead alone consumed tens of KiB before actual title
    // bytes were counted. Offsets below point into Fb2ScanResult::stringPool,
    // so one bounded contiguous allocation holds all section text.
    uint32_t idPoolOffset = UINT32_MAX;
    uint16_t idLength = 0;
    uint32_t titlePoolOffset = UINT32_MAX;
    uint16_t titleLength = 0;

    uint32_t approxTextBytes = 0; // decoded-text size estimate, for progress math
    uint16_t imageRefCount = 0;   // direct inline images, capped at UINT16_MAX
};

// One entry per top-level <body>. CrossPoint currently only renders the
// main flow; `name` lets a caller skip bodies like name="notes"/"comments"
// (endnotes) when building the primary spine, while still being able to
// look them up later for footnote rendering.
struct Fb2BodyIndexEntry {
    std::string name; // <body name="notes"> -> "notes"; empty for main body
};

struct Fb2BinaryIndexEntry {
    std::string id;             // matches href targets ("#id") elsewhere in the doc
    std::string contentType;    // e.g. "image/jpeg"
    uint32_t payloadStartOffset = 0; // offset of the first base64 byte
    uint32_t payloadEndOffset = 0;   // offset just past the last base64 byte
};

// Result of a full metadata/index scan (Fb2Parser::scan()).
struct Fb2ScanResult {
    Fb2Metadata metadata;
    std::vector<Fb2BodyIndexEntry> bodies;
    std::deque<Fb2SectionIndexEntry> sections;   // flat, in document order
    std::deque<Fb2BinaryIndexEntry> binaries;

    // Shared pool for section IDs/titles. It is deliberately bounded: TOC
    // text is useful, but must never be allowed to consume the reader heap.
    // The parser reserves this once near the start of a seekable scan, so the
    // heap does not get fragmented by hundreds of tiny title allocations.
    std::string stringPool;

    std::string_view sectionId(const Fb2SectionIndexEntry& section) const {
        if (section.idPoolOffset == UINT32_MAX || section.idLength == 0 ||
            section.idPoolOffset + section.idLength > stringPool.size()) {
            return {};
        }
        return std::string_view(stringPool.data() + section.idPoolOffset, section.idLength);
    }

    std::string_view sectionTitle(const Fb2SectionIndexEntry& section) const {
        if (section.titlePoolOffset == UINT32_MAX || section.titleLength == 0 ||
            section.titlePoolOffset + section.titleLength > stringPool.size()) {
            return {};
        }
        return std::string_view(stringPool.data() + section.titlePoolOffset, section.titleLength);
    }
};

// ---------------------------------------------------------------------
// Content sink: the parser calls these while streaming a single section's
// body so the caller's existing text-layout code (whatever currently turns
// EPUB's parsed XHTML into TextBlock/Page structures) can build pages from
// FB2 the same way. This mirrors WordStyle from file-formats.md (BOLD,
// ITALIC, UNDERLINE, STRIKETHROUGH, SUP, SUB) plus the block-level shapes
// FB2 actually has (paragraph, subtitle, poem/stanza/verse, cite, epigraph,
// empty-line, table, image).
// ---------------------------------------------------------------------
enum class Fb2InlineStyle : uint8_t {
    Regular = 0,
    Bold = 1,
    Italic = 2,
    Underline = 4,
    Strikethrough = 8,
    Superscript = 16,
    Subscript = 32,
    // FB2 has no dedicated small-caps element. Producers express it via a
    // named <style name="..."> run (often mirroring a CSS class from an
    // embedded stylesheet, e.g. name="smallcaps"/"small-caps"/"sc"). The
    // parser recognizes the common spellings — see isSmallCapsStyleName()
    // in Fb2Parser.cpp — and folds them into this bit so callers don't have
    // to duplicate that heuristic.
    SmallCaps = 64,
};
inline Fb2InlineStyle operator|(Fb2InlineStyle a, Fb2InlineStyle b) {
    return static_cast<Fb2InlineStyle>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

// Real (not text-flattened) FB2 tables: <table>/<tr>/<td|th> with the
// attributes producers actually use — colspan/rowspan for merged cells,
// align/valign for cell content, and header vs. data cell distinction —
// so a table can be laid out as an actual grid instead of collapsing to
// plain paragraphs.
struct Fb2TableCellAttrs {
    uint16_t colspan = 1;
    uint16_t rowspan = 1;
    std::string align;    // "left" | "center" | "right" | "" (unset)
    std::string valign;   // "top" | "middle" | "bottom" | "" (unset)
    bool isHeader = false; // true for <th>, false for <td>
};

class Fb2ContentSink {
public:
    virtual ~Fb2ContentSink() = default;

    virtual void onParagraphBegin() {}
    virtual void onParagraphEnd() {}
    virtual void onSubtitle(const std::string& /*text*/) {}
    virtual void onEmptyLine() {}
    virtual void onHorizontalRule() {}

    virtual void onPoemBegin() {}
    virtual void onPoemEnd() {}
    virtual void onStanzaBegin() {}
    virtual void onStanzaEnd() {}
    virtual void onVerseLine(const std::string& /*text*/) {}

    virtual void onCiteBegin() {}
    virtual void onCiteEnd() {}
    virtual void onEpigraphBegin() {}
    virtual void onEpigraphEnd() {}
    // Attribution inside <cite> or <epigraph>, distinct from quoted text.
    virtual void onTextAuthor(const std::string& /*text*/) {}

    // Called with successive runs of text inside the current block-level
    // element, tagged with whatever inline styles are currently active.
    // May be called multiple times per paragraph (once per style run).
    virtual void onText(const std::string& /*text*/, Fb2InlineStyle /*style*/) {}

    // href is the binary id (no leading '#') this <image> points at; caller
    // decodes it via Fb2Parser::decodeBinary() using the index from scan().
    virtual void onImage(const std::string& /*binaryId*/) {}

    // <a l:href="#n1">...</a> - typically a footnote reference. targetId has
    // any leading '#' stripped already (FB2 links are always same-document,
    // there's no cross-file href in the format). Content between begin/end
    // still flows through onText() as usual - the caller decides how (or
    // whether) to render this as a clickable link.
    virtual void onLinkBegin(const std::string& /*targetId*/) {}
    virtual void onLinkEnd() {}

    virtual void onTableBegin() {}
    virtual void onTableEnd() {}
    virtual void onTableRowBegin() {}
    virtual void onTableCell(const std::string& /*text*/, const Fb2TableCellAttrs& /*attrs*/) {}
    virtual void onTableRowEnd() {}
};
