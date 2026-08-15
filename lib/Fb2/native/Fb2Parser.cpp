#include "Fb2Parser.h"
#include "Fb2XmlReader.h"
#include "Base64Decoder.h"
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <ReaderWork.h>

namespace {

std::string stripHash(const char* v) {
    if (!v) return {};
    return (v[0] == '#') ? std::string(v + 1) : std::string(v);
}

const char* firstOf(const Fb2XmlReader& r, std::initializer_list<const char*> names) {
    for (auto n : names) {
        if (const char* v = r.attr(n)) return v;
    }
    return nullptr;
}

std::string formatAuthorName(const Fb2Author& a) {
    std::string out;
    auto append = [&](const std::string& part) {
        if (part.empty()) return;
        if (!out.empty()) out.push_back(' ');
        out += part;
    };
    append(a.firstName);
    append(a.middleName);
    append(a.lastName);
    if (out.empty()) out = a.nickname; // some FB2s only have a nickname
    return out;
}

// Bounds how much annotation text we'll buffer during scan(); annotations
// are almost always a short blurb, but nothing stops a producer from putting
// a huge one in, so we cap it defensively.
constexpr size_t kMaxAnnotationBytes = 4000;

// Same idea for an embedded <stylesheet>: real per-book CSS is typically a
// few hundred bytes to a couple KB (a handful of font/line-height/small-caps
// rules), so this is generous headroom while still bounding worst case RAM.
constexpr size_t kMaxStylesheetBytes = 8000;

bool isSmallCapsStyleName(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower.find("smallcaps") != std::string::npos ||
           lower.find("small-caps") != std::string::npos ||
           lower.find("small_caps") != std::string::npos ||
           lower == "sc" || lower == "caps";
}

uint16_t parseSpan(const char* v) {
    if (!v) return 1;
    long n = std::strtol(v, nullptr, 10);
    return n > 0 ? static_cast<uint16_t>(n) : 1;
}

// Cheap byte-level pass counting occurrences of up to two literal tag
// openers (e.g. "<section", "<binary") in a single read of the file,
// without any XML tokenizing. Used only to reserve() the scan-result
// vectors up front: a book like "War and Peace" has 600+ <section>
// entries, and letting std::vector grow by doubling would (a) round the
// final allocation up to the next power of two (wasting up to ~50% of that
// buffer) and (b) transiently hold both the old and new backing buffers at
// once during every reallocation - on a ~380KB-RAM device that transient 2x
// spike is what actually exhausts memory, not the final steady-state size.
// A single pre-sized allocation avoids both. Counting both tags together
// (instead of one pre-count pass per tag) halves how many times the whole
// file has to be read before the real scan even starts.
void countTwoTagOpeners(IByteReader& reader, const char* tagA, uint32_t& countA, const char* tagB, uint32_t& countB) {
    const size_t lenA = std::strlen(tagA);
    const size_t lenB = std::strlen(tagB);
    const size_t maxLen = lenA > lenB ? lenA : lenB;
    constexpr size_t CHUNK = 4096;
    uint8_t buf[CHUNK + 16]; // CHUNK of fresh data + room to carry a partial match
    size_t pending = 0;      // bytes already sitting at the front of buf, carried
                              // over from the previous chunk (a possible partial match)
    countA = 0;
    countB = 0;
    for (;;) {
        size_t got = reader.read(buf + pending, CHUNK);
        size_t total = pending + got;
        if (total < maxLen) break;
        // Every position where the longer tag could fit is fully buffered
        // (i in [0, total-maxLen]); a match starting later would need bytes
        // not read yet, so those trailing bytes get carried into the next
        // chunk instead of being searched now.
        const size_t searchEnd = total - maxLen + 1;
        for (size_t i = 0; i < searchEnd; ++i) {
            if (std::memcmp(buf + i, tagA, lenA) == 0) ++countA;
            if (std::memcmp(buf + i, tagB, lenB) == 0) ++countB;
        }
        if (got < CHUNK) break; // that was the last chunk (short read / EOF)
        pending = maxLen - 1;
        std::memmove(buf, buf + total - pending, pending);
    }
}

} // namespace

namespace {
constexpr size_t kMaxFb2SectionIdBytes = 192;
constexpr size_t kMaxFb2SectionTitleBytes = 384;
constexpr size_t kMaxFb2BinaryIdBytes = 192;
constexpr size_t kMaxFb2ContentTypeBytes = 80;
constexpr size_t kMaxFb2BodyNameBytes = 80;

// The shared section-string pool is a hard safety boundary. 48 KiB is enough
// for >700 normal chapter titles while remaining one predictable allocation.
constexpr size_t kSectionStringPoolReserve = 28 * 1024;
constexpr size_t kSectionStringPoolHardLimit = 28 * 1024;

bool storePoolString(Fb2ScanResult& out, const std::string& value,
                     uint32_t& offset, uint16_t& length) {
    offset = UINT32_MAX;
    length = 0;
    if (value.empty() || out.stringPool.size() >= kSectionStringPoolHardLimit) return false;

    const size_t available = kSectionStringPoolHardLimit - out.stringPool.size();
    const size_t copyLen = std::min({value.size(), available, static_cast<size_t>(UINT16_MAX)});
    if (copyLen == 0) return false;

    offset = static_cast<uint32_t>(out.stringPool.size());
    length = static_cast<uint16_t>(copyLen);
    out.stringPool.append(value.data(), copyLen);
    return true;
}

bool storePoolCString(Fb2ScanResult& out, const char* value, const size_t maxLen,
                      uint32_t& offset, uint16_t& length) {
    if (!value) {
        offset = UINT32_MAX;
        length = 0;
        return false;
    }
    const size_t n = strnlen(value, maxLen);
    return storePoolString(out, std::string(value, n), offset, length);
}

bool storePoolTitle(Fb2ScanResult& out, const std::string& value,
                    uint32_t& offset, uint16_t& length) {
    offset = UINT32_MAX;
    length = 0;
    if (value.empty() || out.stringPool.size() >= kSectionStringPoolHardLimit) return false;

    const size_t start = out.stringPool.size();
    bool pendingSpace = false;
    for (const unsigned char c : value) {
        if (out.stringPool.size() >= kSectionStringPoolHardLimit) break;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
            pendingSpace = out.stringPool.size() > start;
            continue;
        }
        if (pendingSpace && out.stringPool.size() < kSectionStringPoolHardLimit) {
            out.stringPool.push_back(' ');
        }
        pendingSpace = false;
        if (out.stringPool.size() < kSectionStringPoolHardLimit) {
            out.stringPool.push_back(static_cast<char>(c));
        }
    }

    const size_t stored = out.stringPool.size() - start;
    if (stored == 0) return false;
    offset = static_cast<uint32_t>(start);
    length = static_cast<uint16_t>(std::min(stored, static_cast<size_t>(UINT16_MAX)));
    return true;
}

void assignBoundedFb2(std::string& dst, const char* src, const size_t limit) {
    if (!src) {
        dst.clear();
        return;
    }
    const size_t len = strnlen(src, limit);
    dst.assign(src, len);
}

void appendBoundedFb2(std::string& dst, const std::string& src, const size_t limit) {
    if (dst.size() >= limit || src.empty()) return;
    const size_t remaining = limit - dst.size();
    dst.append(src.data(), std::min(remaining, src.size()));
}
}  // namespace

bool Fb2Parser::scan(IByteReader& reader, Fb2ScanResult& out, size_t xmlBufferSize) {
    out = Fb2ScanResult{};

    // Fast-I/O v4 SAFE:
    // remove the complete pre-count pass. It read the whole FB2 once just to
    // count <section>/<binary>, then the real XML scan read the whole file
    // again. Keep only small bounded reserves so we don't trade SD time for
    // large contiguous heap allocations on ESP32-C3.
    // sections/binaries use deque: no large contiguous reserve/reallocation.
    out.bodies.reserve(4);

    // One predictable allocation replaces hundreds of per-section title/ID
    // allocations. 28 KiB is intentionally below the normal contiguous block
    // available at scan start on X3/X4 and is enough for hundreds of normal
    // chapter titles. The pool never grows past that limit.
    out.stringPool.reserve(reader.size() >= 8ULL * 1024 * 1024 ? kSectionStringPoolReserve : 8 * 1024);

    // 8 KiB halves SD read calls on large plain FB2 files without returning
    // to v3's unsafe 16 KiB buffer. Fused ZIP explicitly requests 2 KiB.
    const size_t safeXmlBufferSize =
        std::max<size_t>(1024, std::min<size_t>(8192, xmlBufferSize));
    Fb2XmlReader xml(reader, safeXmlBufferSize);

    bool inDescription = false;
    bool inTitleInfo = false;
    bool inAuthorTag = false;
    bool inAnnotation = false;
    bool inCoverpage = false;
    bool inTitle = false;
    bool inStylesheet = false;

    bool inBinaryTag = false;
    Fb2BinaryIndexEntry curBinary;

    Fb2Author curAuthor;
    std::string* activeTarget = nullptr; // where Text tokens currently get appended

    std::string annotationBuf;
    bool annotationNeedsSep = false;

    std::string titleBuf;
    bool titleNeedsSep = false;

    int currentBodyIndex = -1;
    std::vector<int> sectionStack;

    for (;;) {
        // Most of a large FB2 is ordinary paragraph text. During indexing we
        // only count those decoded bytes; materialize strings solely where
        // metadata/TOC/CSS actually consumes them.
        xml.setCaptureText(activeTarget != nullptr || inStylesheet || inAnnotation || inTitle);
        Fb2Token tok = xml.next();
        if (tok == Fb2Token::Eof) break;
        if (tok == Fb2Token::Error) return false;

        out.tokenCount++;
        if (tok == Fb2Token::Text) {
            out.textTokenCount++;
            out.textPayloadBytes += xml.textSize();
            if (inBinaryTag) out.binaryTextBytes += xml.textSize();
        }

        const std::string& name = xml.name();

        if (tok == Fb2Token::StartTag || tok == Fb2Token::SelfClosing) {
            if (name == "description") {
                inDescription = true;
            } else if (name == "title-info" && inDescription) {
                inTitleInfo = true;
            } else if (name == "author" && inTitleInfo) {
                inAuthorTag = true;
                curAuthor = Fb2Author{};
            } else if (inAuthorTag && name == "first-name") {
                activeTarget = &curAuthor.firstName;
            } else if (inAuthorTag && name == "middle-name") {
                activeTarget = &curAuthor.middleName;
            } else if (inAuthorTag && name == "last-name") {
                activeTarget = &curAuthor.lastName;
            } else if (inAuthorTag && name == "nickname") {
                activeTarget = &curAuthor.nickname;
            } else if (name == "book-title" && inTitleInfo && !inAuthorTag) {
                activeTarget = &out.metadata.title;
            } else if (name == "lang" && inTitleInfo && !inAuthorTag) {
                activeTarget = &out.metadata.language;
            } else if (name == "annotation" && inTitleInfo) {
                inAnnotation = true;
                annotationBuf.clear();
                annotationNeedsSep = false;
            } else if (name == "p" && inAnnotation) {
                if (annotationNeedsSep) annotationBuf += "\n\n";
                annotationNeedsSep = true;
            } else if (name == "stylesheet" && inDescription) {
                inStylesheet = true;
                out.metadata.embeddedStylesheetCss.clear();
            } else if (name == "coverpage" && inTitleInfo) {
                inCoverpage = true;
            } else if (name == "image" && inCoverpage) {
                if (const char* href = firstOf(xml, {"l:href", "xlink:href", "href"})) {
                    out.metadata.coverBinaryId = stripHash(href);
                }
            } else if (name == "image" && !inCoverpage && !sectionStack.empty()) {
                if (out.sections[sectionStack.back()].imageRefCount < UINT16_MAX) out.sections[sectionStack.back()].imageRefCount++;
            } else if (name == "sequence" && inTitleInfo && !inAuthorTag &&
                       out.metadata.sequenceName.empty()) {
                if (const char* n = xml.attr("name")) out.metadata.sequenceName = n;
                if (const char* num = xml.attr("number")) out.metadata.sequenceNumber = static_cast<uint32_t>(std::strtoul(num, nullptr, 10));
            } else if (name == "body") {
                Fb2BodyIndexEntry b;
                if (const char* n = xml.attr("name")) assignBoundedFb2(b.name, n, kMaxFb2BodyNameBytes);
                out.bodies.push_back(b);
                currentBodyIndex = static_cast<int>(out.bodies.size()) - 1;
                sectionStack.clear();
            } else if (name == "section" && currentBodyIndex >= 0) {
                Fb2SectionIndexEntry e;
                e.level = static_cast<uint16_t>(sectionStack.size());
                e.bodyIndex = currentBodyIndex;
                if (const char* id = xml.attr("id")) {
                    storePoolCString(out, id, kMaxFb2SectionIdBytes, e.idPoolOffset, e.idLength);
                }
                out.sections.push_back(e);
                sectionStack.push_back(static_cast<int>(out.sections.size()) - 1);
                out.sections.back().innerStartOffset = xml.streamPos();
            } else if (name == "title" && !sectionStack.empty()) {
                inTitle = true;
                titleBuf.clear();
                titleNeedsSep = false;
            } else if (name == "p" && inTitle) {
                if (titleNeedsSep) titleBuf += " ";
                titleNeedsSep = true;
            } else if (name == "binary") {
                inBinaryTag = true;
                curBinary = Fb2BinaryIndexEntry{};
                if (const char* id = xml.attr("id")) assignBoundedFb2(curBinary.id, id, kMaxFb2BinaryIdBytes);
                if (const char* ct = xml.attr("content-type")) assignBoundedFb2(curBinary.contentType, ct, kMaxFb2ContentTypeBytes);
                curBinary.payloadStartOffset = xml.streamPos();
            }
        } else if (tok == Fb2Token::Text) {
            if (activeTarget) {
                *activeTarget += xml.text();
            } else if (inStylesheet) {
                if (out.metadata.embeddedStylesheetCss.size() < kMaxStylesheetBytes)
                    out.metadata.embeddedStylesheetCss += xml.text();
            } else if (inAnnotation) {
                if (annotationBuf.size() < kMaxAnnotationBytes) annotationBuf += xml.text();
            } else if (inTitle) {
                appendBoundedFb2(titleBuf, xml.text(), kMaxFb2SectionTitleBytes);
            } else if (inBinaryTag) {
                // Skip: base64 payload bytes are read directly by
                // decodeBinary() later via byte offsets, never buffered here.
            } else if (currentBodyIndex >= 0 && !sectionStack.empty()) {
                out.sections[sectionStack.back()].approxTextBytes +=
                    static_cast<uint32_t>(xml.textSize());
            }
        }

        if (tok == Fb2Token::EndTag) {
            if (name == "description") {
                inDescription = false;
            } else if (name == "title-info") {
                inTitleInfo = false;
            } else if (name == "author" && inAuthorTag) {
                out.metadata.authors.push_back(curAuthor);
                std::string display = formatAuthorName(curAuthor);
                if (!display.empty()) {
                    if (!out.metadata.author.empty()) out.metadata.author += "; ";
                    out.metadata.author += display;
                }
                inAuthorTag = false;
            } else if (inAuthorTag && (name == "first-name" || name == "middle-name" ||
                                        name == "last-name" || name == "nickname")) {
                activeTarget = nullptr;
            } else if (name == "book-title" || name == "lang") {
                activeTarget = nullptr;
            } else if (name == "annotation") {
                out.metadata.annotationText = annotationBuf;
                inAnnotation = false;
            } else if (name == "stylesheet") {
                inStylesheet = false;
            } else if (name == "coverpage") {
                inCoverpage = false;
            } else if (name == "body") {
                currentBodyIndex = -1;
                sectionStack.clear();
            } else if (name == "section" && !sectionStack.empty()) {
                sectionStack.pop_back();
            } else if (name == "title") {
                if (!sectionStack.empty()) {
                    auto& section = out.sections[sectionStack.back()];
                    storePoolTitle(out, titleBuf, section.titlePoolOffset, section.titleLength);
                }
                inTitle = false;
            } else if (name == "binary") {
                curBinary.payloadEndOffset = xml.tokenStartOffset();
                out.binaries.push_back(curBinary);
                inBinaryTag = false;
            }
        }
    }

    return true;
}

bool Fb2Parser::renderSection(IByteReader& reader,
                               const Fb2SectionIndexEntry& section,
                               Fb2ContentSink& sink,
                               const reader::ReaderCancellationToken* cancellationToken) {
    // See scan() above for why this isn't the default 512.
    Fb2XmlReader xml(reader, 4096);
    xml.seekTo(section.innerStartOffset);

    int boldDepth = 0, italicDepth = 0, underlineDepth = 0, strikeDepth = 0, supDepth = 0, subDepth = 0;
    int smallCapsDepth = 0;
    std::vector<bool> styleTagIsSmallCaps; // one entry per open <style>, for correct un-nesting
    auto currentStyle = [&]() {
        Fb2InlineStyle s = Fb2InlineStyle::Regular;
        if (boldDepth > 0) s = s | Fb2InlineStyle::Bold;
        if (italicDepth > 0) s = s | Fb2InlineStyle::Italic;
        if (underlineDepth > 0) s = s | Fb2InlineStyle::Underline;
        if (strikeDepth > 0) s = s | Fb2InlineStyle::Strikethrough;
        if (supDepth > 0) s = s | Fb2InlineStyle::Superscript;
        if (subDepth > 0) s = s | Fb2InlineStyle::Subscript;
        if (smallCapsDepth > 0) s = s | Fb2InlineStyle::SmallCaps;
        return s;
    };

    bool inSubtitle = false; std::string subtitleBuf;
    bool inVerse = false; std::string verseBuf;
    bool inTextAuthor = false; std::string textAuthorBuf;
    bool inTableCell = false; std::string cellBuf; Fb2TableCellAttrs cellAttrs;
    bool inTitleTag = false; // skip the section's own <title>; already indexed

    bool skipping = false;
    int skipDepth = 0;

    for (;;) {
        if (cancellationToken && cancellationToken->isCancellationRequested()) return false;
        Fb2Token tok = xml.next();
        if (tok == Fb2Token::Eof) return true; // malformed/truncated: best effort
        if (tok == Fb2Token::Error) return false;

        const std::string& name = xml.name();

        if (skipping) {
            if ((tok == Fb2Token::StartTag) && name == "section") skipDepth++;
            else if (tok == Fb2Token::EndTag && name == "section") {
                if (--skipDepth == 0) skipping = false;
            }
            continue;
        }

        if (tok == Fb2Token::StartTag && name == "section") {
            // Nested subsection: it's its own spine/TOC entry and will be
            // rendered independently, so skip its whole subtree here.
            skipping = true;
            skipDepth = 1;
            continue;
        }
        if (tok == Fb2Token::EndTag && name == "section") {
            // Our own closing tag: this section's content is complete.
            return true;
        }

        if (tok == Fb2Token::StartTag || tok == Fb2Token::SelfClosing) {
            if (name == "title") { inTitleTag = true; continue; }
            if (inTitleTag) {
                // The indexed text becomes the synthetic heading, but an
                // ornamental image in the original FB2 title must survive.
                if (name == "image") {
                    if (const char* href = firstOf(xml, {"l:href", "xlink:href", "href"}))
                        sink.onImage(stripHash(href));
                }
                continue;
            }

            if (name == "p") sink.onParagraphBegin();
            else if (name == "empty-line") sink.onEmptyLine();
            else if (name == "poem") sink.onPoemBegin();
            else if (name == "stanza") sink.onStanzaBegin();
            else if (name == "v") { inVerse = true; verseBuf.clear(); }
            else if (name == "cite") sink.onCiteBegin();
            else if (name == "epigraph") sink.onEpigraphBegin();
            else if (name == "text-author") { inTextAuthor = true; textAuthorBuf.clear(); }
            else if (name == "subtitle") { inSubtitle = true; subtitleBuf.clear(); }
            else if (name == "strong" || name == "b") boldDepth++;
            else if (name == "emphasis" || name == "i") italicDepth++;
            else if (name == "underline" || name == "u") underlineDepth++;
            else if (name == "strikethrough") strikeDepth++;
            else if (name == "sup") supDepth++;
            else if (name == "sub") subDepth++;
            else if (name == "style") {
                bool isSC = isSmallCapsStyleName(xml.attr("name") ? xml.attr("name") : "");
                styleTagIsSmallCaps.push_back(isSC);
                if (isSC) smallCapsDepth++;
            }
            else if (name == "image") {
                if (const char* href = firstOf(xml, {"l:href", "xlink:href", "href"}))
                    sink.onImage(stripHash(href));
            }
            else if (name == "a") {
                if (const char* href = firstOf(xml, {"l:href", "xlink:href", "href"}))
                    sink.onLinkBegin(stripHash(href));
                else
                    sink.onLinkBegin(std::string());  // still balanced by the matching onLinkEnd() below
            }
            else if (name == "table") sink.onTableBegin();
            else if (name == "tr") sink.onTableRowBegin();
            else if (name == "td" || name == "th") {
                inTableCell = true;
                cellBuf.clear();
                cellAttrs = Fb2TableCellAttrs{};
                cellAttrs.isHeader = (name == "th");
                cellAttrs.colspan = parseSpan(xml.attr("colspan"));
                cellAttrs.rowspan = parseSpan(xml.attr("rowspan"));
                if (const char* a = xml.attr("align")) cellAttrs.align = a;
                if (const char* v = xml.attr("valign")) cellAttrs.valign = v;
            }
            // "a" (hyperlinks/footnote refs), "code", genre, etc.
            // intentionally fall through: their text still flows through as
            // a normal text run below.
        }

        if (tok == Fb2Token::Text) {
            if (inTitleTag) { /* swallowed */ }
            else if (inSubtitle) subtitleBuf += xml.text();
            else if (inVerse) verseBuf += xml.text();
            else if (inTextAuthor) textAuthorBuf += xml.text();
            else if (inTableCell) cellBuf += xml.text();
            else sink.onText(xml.text(), currentStyle());
        }

        if (tok == Fb2Token::EndTag) {
            if (name == "title") { inTitleTag = false; continue; }
            if (inTitleTag) continue;

            if (name == "p") sink.onParagraphEnd();
            else if (name == "poem") sink.onPoemEnd();
            else if (name == "stanza") sink.onStanzaEnd();
            else if (name == "v") { sink.onVerseLine(verseBuf); inVerse = false; }
            else if (name == "cite") sink.onCiteEnd();
            else if (name == "epigraph") sink.onEpigraphEnd();
            else if (name == "text-author") { sink.onTextAuthor(textAuthorBuf); inTextAuthor = false; }
            else if (name == "subtitle") { sink.onSubtitle(subtitleBuf); inSubtitle = false; }
            else if (name == "strong" || name == "b") boldDepth--;
            else if (name == "emphasis" || name == "i") italicDepth--;
            else if (name == "underline" || name == "u") underlineDepth--;
            else if (name == "strikethrough") strikeDepth--;
            else if (name == "sup") supDepth--;
            else if (name == "sub") subDepth--;
            else if (name == "style") {
                if (!styleTagIsSmallCaps.empty()) {
                    bool was = styleTagIsSmallCaps.back();
                    styleTagIsSmallCaps.pop_back();
                    if (was) smallCapsDepth--;
                }
            }
            else if (name == "table") sink.onTableEnd();
            else if (name == "tr") sink.onTableRowEnd();
            else if (name == "td" || name == "th") { sink.onTableCell(cellBuf, cellAttrs); inTableCell = false; }
            else if (name == "a") sink.onLinkEnd();
        }
    }
}

bool Fb2Parser::decodeBinary(IByteReader& reader,
                              const Fb2BinaryIndexEntry& binary,
                              const BinaryOutputFn& out,
                              const reader::ReaderCancellationToken* cancellationToken) {
    if (binary.payloadEndOffset < binary.payloadStartOffset) return false;
    if (!reader.seek(binary.payloadStartOffset)) return false;

    Base64Decoder decoder(out);
    uint32_t remaining = binary.payloadEndOffset - binary.payloadStartOffset;
    uint8_t chunk[256];
    while (remaining > 0) {
        if (cancellationToken && cancellationToken->isCancellationRequested()) return false;
        size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        size_t got = reader.read(chunk, want);
        if (got == 0) return false; // short read: truncated file, bail gracefully
        decoder.feed(reinterpret_cast<const char*>(chunk), got);
        remaining -= static_cast<uint32_t>(got);
    }
    decoder.finish();
    return remaining == 0;
}
