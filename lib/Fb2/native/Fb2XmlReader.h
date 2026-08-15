// Fb2XmlReader.h
//
// A small streaming XML tokenizer, written from scratch (no libxml2/tinyxml2/
// pugixml dependency) so the whole FB2 stack stays "native" and has a tiny,
// auditable footprint. It is NOT a general-purpose XML parser: it assumes
// well-formed input (true for the overwhelming majority of real FB2 files)
// and only implements what FB2 needs:
//
//   - elements, attributes, self-closing tags
//   - text content, with the 5 predefined XML entities + numeric refs
//   - '<!--' comments and '<?xml ... ?>' / '<!DOCTYPE ...>' skipping
//   - UTF-8 passthrough (FB2 is UTF-8; we never decode codepoints, just
//     forward bytes, so Cyrillic/etc. text is preserved untouched)
//
// It does NOT build a tree: callers get a pull-style Next() that returns one
// token at a time, and long text runs are delivered as a *sequence* of Text
// tokens (chunked to a fixed buffer) rather than one giant allocation — this
// is what makes it safe to run against multi-hundred-KB <p> runs on 380KB
// of RAM.

#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "IByteReader.h"

enum class Fb2Token {
    None,
    StartTag,     // name_ / attrs_ valid
    EndTag,       // name_ valid
    SelfClosing,  // name_ / attrs_ valid (equivalent to StartTag immediately
                   // followed by EndTag of the same name; reader auto-emits
                   // the matching EndTag on the NEXT Next() call so callers
                   // can treat StartTag/SelfClosing uniformly if they want)
    Text,         // text_ valid (a chunk, not necessarily the whole run)
    Eof,
    Error,
};

struct Fb2Attr {
    std::string name;
    std::string value;
};

class Fb2XmlReader {
public:
    explicit Fb2XmlReader(IByteReader& in, size_t bufCap = 512)
        : in_(in), bufCap_(bufCap) {
        buf_.resize(bufCap_);
    }

    // Absolute byte offset (into the underlying stream) of the start of the
    // token just returned by Next(). Used by Fb2Parser to record section
    // boundaries for the lazy re-read index.
    uint32_t tokenStartOffset() const { return tokenStart_; }

    // Absolute byte offset of the stream position right after the token
    // just returned by Next() (e.g. immediately past a tag's '>'). Combined
    // with tokenStartOffset(), this lets Fb2Parser record exact [start,end)
    // byte ranges for sections/binaries without a second pass.
    uint32_t streamPos() const { return curAbsPos(); }

    const std::string& name() const { return name_; }
    const std::vector<Fb2Attr>& attrs() const { return attrs_; }
    const std::string& text() const { return text_; }
    size_t textSize() const { return textSize_; }

    // Index scans need the actual text only for metadata, annotations,
    // stylesheets and section titles. For ordinary book paragraphs they only
    // need the decoded byte count. Disabling capture avoids hundreds of
    // thousands of push_back() calls while preserving token boundaries and
    // exact XML byte offsets. Rendering keeps the default capture=true.
    void setCaptureText(bool capture) { captureText_ = capture; }

    const char* attr(const char* n) const {
        for (auto& a : attrs_) if (a.name == n) return a.value.c_str();
        return nullptr;
    }

    // Seek the underlying stream and reset all tokenizer state. Used to jump
    // straight to a previously-indexed <section> without re-scanning the file.
    void seekTo(uint32_t pos) {
        in_.seek(pos);
        bufLen_ = bufPos_ = 0;
        pendingSelfCloseName_.clear();
        eof_ = false;
        inCData_ = false;
        cdataPendingBrackets_ = 0;
    }

    Fb2Token next() {
        if (inCData_) {
            return readCDataChunk();
        }
        if (!pendingSelfCloseName_.empty()) {
            name_ = pendingSelfCloseName_;
            pendingSelfCloseName_.clear();
            return Fb2Token::EndTag;
        }

        for (;;) {
            int c = peekByte();
            if (c < 0) return Fb2Token::Eof;

            if (c == '<') {
                return readMarkup();
            }
            return readText();
        }
    }

private:
    // ---- low-level buffered byte access over IByteReader -----------------
    int peekByte() {
        if (bufPos_ >= bufLen_) {
            if (!refill()) return -1;
        }
        return bufPos_ < bufLen_ ? buf_[bufPos_] : -1;
    }
    int getByte() {
        int c = peekByte();
        if (c >= 0) bufPos_++;
        return c;
    }
    bool refill() {
        if (eof_) return false;
        bufLen_ = in_.read(buf_.data(), bufCap_);
        bufPos_ = 0;
        if (bufLen_ == 0) { eof_ = true; return false; }
        return true;
    }
    uint32_t curAbsPos() const {
        // Bytes already consumed from the stream minus what's still buffered.
        return in_.position() - static_cast<uint32_t>(bufLen_ - bufPos_);
    }

    // ---- token readers ------------------------------------------------
    Fb2Token readMarkup() {
        tokenStart_ = curAbsPos();
        getByte(); // consume '<'
        int c = peekByte();
        if (c == '?') { skipUntil("?>"); return next(); }
        if (c == '!') {
            // CDATA (producers commonly wrap embedded <stylesheet> CSS in
            // this so '<','>','&' in the CSS don't need XML-escaping),
            // comment, or doctype.
            if (matchAhead("![CDATA[")) { cdataPendingBrackets_ = 0; return readCDataChunk(); }
            if (matchAhead("!--")) { skipUntil("-->"); return next(); }
            skipUntil(">");
            return next();
        }
        bool closing = false;
        if (c == '/') { closing = true; getByte(); }

        name_.clear();
        readNameInto(name_);

        if (closing) {
            skipUntil(">");
            return Fb2Token::EndTag;
        }

        attrs_.clear();
        bool selfClosing = false;
        for (;;) {
            skipWs();
            int p = peekByte();
            if (p < 0) return Fb2Token::Error;
            if (p == '/') {
                getByte();
                skipWs();
                if (peekByte() == '>') getByte();
                selfClosing = true;
                break;
            }
            if (p == '>') { getByte(); break; }

            Fb2Attr a;
            readNameInto(a.name);
            // readNameInto() consumes nothing for a byte that isn't a
            // valid name-start character - peekByte() would then return
            // that exact same byte again next iteration with nothing
            // having advanced, spinning forever on content that was never
            // going to become a valid attribute (this is what let attrs_
            // grow unbounded below, before the loop itself ever noticed
            // there was no tag closer coming). No progress means this
            // isn't a well-formed tag; give up on it here instead.
            if (a.name.empty()) return Fb2Token::Error;
            skipWs();
            if (peekByte() == '=') {
                getByte();
                skipWs();
                int q = getByte(); // quote char (' or ")
                std::string val;
                while (true) {
                    int ch = getByte();
                    if (ch < 0 || ch == q) break;
                    appendDecoded(val, ch);
                }
                a.value = std::move(val);
            }
            // A real tag has a handful of attributes at most. Content that
            // never finds this tag's closing '>' (malformed input, or a
            // '<' that isn't actually the start of a tag at all) can send
            // this loop reading indefinitely, growing attrs_ without bound
            // until vector growth finally tries to allocate more memory
            // than exists - an out-of-memory abort with no useful error,
            // instead of just treating the tag as unparseable. Bailing out
            // here (dropping this and any further attribute) keeps the
            // eventual token recognizable rather than crashing outright.
            if (attrs_.size() < 256) attrs_.push_back(std::move(a));
        }

        if (selfClosing) {
            pendingSelfCloseName_ = name_;
            return Fb2Token::SelfClosing;
        }
        return Fb2Token::StartTag;
    }

    Fb2Token readText() {
        tokenStart_ = curAbsPos();
        text_.clear();
        textSize_ = 0;

        if (!captureText_) {
            // Fast scan-only path. Most FB2 bytes are ordinary paragraph
            // text, so consume whole spans from the already-filled XML
            // buffer instead of calling peekByte()/getByte()/push_back() for
            // every byte. Stop only at markup ('<'), an entity ('&'), the
            // decoded token-size limit, or EOF.
            while (textSize_ < bufCap_) {
                if (bufPos_ >= bufLen_ && !refill()) break;
                const size_t remaining = bufCap_ - textSize_;
                const size_t available = std::min(bufLen_ - bufPos_, remaining);
                const uint8_t* begin = buf_.data() + bufPos_;
                const uint8_t* special = begin;
                const uint8_t* end = begin + available;
                // A single pointer pass is faster on ESP32-C3 than two
                // memchr() calls, which scan every short text run twice.
                while (special < end && *special != '<' && *special != '&') ++special;

                if (special == end) {
                    bufPos_ += available;
                    textSize_ += available;
                    continue;
                }

                const size_t plainBytes = static_cast<size_t>(special - begin);
                bufPos_ += plainBytes;
                textSize_ += plainBytes;
                if (textSize_ >= bufCap_) break;
                if (*special == '<') break;  // leave markup for readMarkup()

                // Consume '&'; the helper consumes the entity body and ';'.
                bufPos_++;
                textSize_ += consumeEntityDecodedLength();
            }
            return textSize_ == 0 ? next() : Fb2Token::Text;
        }

        // Grab a bounded chunk so a multi-hundred-KB paragraph never becomes
        // one giant std::string; caller re-enters next() for more chunks.
        while (textSize_ < bufCap_) {
            int c = peekByte();
            if (c < 0 || c == '<') break;
            getByte();
            appendDecoded(text_, c);
            textSize_ = text_.size();
        }
        return textSize_ == 0 ? next() : Fb2Token::Text;
    }

    // Reads (a chunk of) raw CDATA content, verbatim / not entity-decoded,
    // up to the "]]>" terminator. Like readText(), this is bounded to
    // bufCap_ per call and resumes via inCData_/cdataPendingBrackets_ state
    // if the section is larger than one chunk (an embedded stylesheet can
    // legitimately be a few KB).
    Fb2Token readCDataChunk() {
        tokenStart_ = curAbsPos();
        text_.clear();
        textSize_ = 0;
        for (;;) {
            int c = getByte();
            if (c < 0) { inCData_ = false; break; } // truncated file: best effort
            if (c == ']') {
                cdataPendingBrackets_++;
                if (cdataPendingBrackets_ > 2) {
                    text_.push_back(']');
                    cdataPendingBrackets_ = 2;
                }
            } else if (c == '>' && cdataPendingBrackets_ >= 2) {
                cdataPendingBrackets_ = 0;
                inCData_ = false;
                break; // "]]>" terminator consumed, CDATA section closed
            } else {
                while (cdataPendingBrackets_ > 0) { text_.push_back(']'); cdataPendingBrackets_--; }
                text_.push_back(static_cast<char>(c));
            }
            if (text_.size() >= bufCap_) {
                textSize_ = text_.size();
                inCData_ = true; // more content beyond this chunk; resume on next next()
                return Fb2Token::Text;
            }
        }
        textSize_ = text_.size();
        return text_.empty() ? next() : Fb2Token::Text;
    }

    void readNameInto(std::string& dst) {
        for (;;) {
            int c = peekByte();
            if (c < 0) break;
            if (isNameChar(static_cast<unsigned char>(c))) { dst.push_back(static_cast<char>(c)); getByte(); }
            else break;
        }
    }
    static bool isNameChar(unsigned char c) {
        return (c == ':' || c == '-' || c == '.' || c == '_' ||
                (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') || c >= 0x80 /* utf-8 continuation */);
    }
    void skipWs() {
        for (;;) {
            int c = peekByte();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') getByte();
            else break;
        }
    }
    bool matchAhead(const char* lit) {
        // Only used right after '<', with a tiny literal ("!--"), so a
        // one-byte-at-a-time compare is fine and keeps buffering simple.
        size_t save = bufPos_;
        for (const char* p = lit; *p; ++p) {
            int c = getByte();
            if (c != *p) { bufPos_ = save; return false; }
        }
        return true;
    }
    void skipUntil(const char* closer) {
        size_t n = std::strlen(closer);
        size_t matched = 0;
        for (;;) {
            int c = getByte();
            if (c < 0) return;
            matched = (static_cast<char>(c) == closer[matched]) ? matched + 1 : (static_cast<char>(c) == closer[0] ? 1 : 0);
            if (matched == n) return;
        }
    }

    // Handles the 5 predefined XML entities + decimal/hex numeric refs.
    // Anything else (raw UTF-8, including multi-byte Cyrillic sequences) is
    // passed through byte-for-byte.
    void appendDecoded(std::string& dst, int firstByte) {
        if (firstByte != '&') { dst.push_back(static_cast<char>(firstByte)); return; }
        std::string ent;
        int c;
        while ((c = peekByte()) >= 0 && c != ';' && ent.size() < 12) { ent.push_back(static_cast<char>(c)); getByte(); }
        if (peekByte() == ';') getByte();

        if (ent == "lt") dst.push_back('<');
        else if (ent == "gt") dst.push_back('>');
        else if (ent == "amp") dst.push_back('&');
        else if (ent == "quot") dst.push_back('"');
        else if (ent == "apos") dst.push_back('\'');
        else if (!ent.empty() && ent[0] == '#') {
            long cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                          ? std::strtol(ent.c_str() + 2, nullptr, 16)
                          : std::strtol(ent.c_str() + 1, nullptr, 10);
            appendUtf8(dst, static_cast<uint32_t>(cp));
        } else if (uint32_t cp = namedEntityCodepoint(ent)) {
            // Strictly speaking FB2/XML only guarantees the 5 predefined
            // entities above; everything else must be a numeric reference.
            // In practice a lot of real-world FB2 (converted from Word/HTML
            // sources) uses common named entities anyway — notably &shy;
            // (soft hyphen, U+00AD), which the hyphenation-aware line-wrap
            // pass downstream needs as a genuine break candidate, and
            // &nbsp;/dash/quote variants that affect line breaking and
            // typography. Recognizing them here means the caller gets a
            // correct Unicode codepoint instead of the entity being
            // silently dropped.
            appendUtf8(dst, cp);
        }
        // any other unknown named entity: drop silently
    }

    // Same entity semantics as appendDecoded(), but returns only the number
    // of resulting UTF-8 bytes. Called by the allocation-free scan path after
    // the leading '&' has already been consumed.
    size_t consumeEntityDecodedLength() {
        std::string ent;
        int c;
        while ((c = peekByte()) >= 0 && c != ';' && ent.size() < 12) {
            ent.push_back(static_cast<char>(c));
            getByte();
        }
        if (peekByte() == ';') getByte();

        if (ent == "lt" || ent == "gt" || ent == "amp" || ent == "quot" || ent == "apos") return 1;
        uint32_t cp = 0;
        if (!ent.empty() && ent[0] == '#') {
            cp = static_cast<uint32_t>((ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                                           ? std::strtol(ent.c_str() + 2, nullptr, 16)
                                           : std::strtol(ent.c_str() + 1, nullptr, 10));
        } else {
            cp = namedEntityCodepoint(ent);
        }
        if (cp <= 0x7F) return cp == 0 ? 0 : 1;
        if (cp <= 0x7FF) return 2;
        if (cp <= 0xFFFF) return 3;
        return 4;
    }
    // Returns the Unicode codepoint for a small set of common named
    // entities beyond the 5 XML-predefined ones, or 0 if unrecognized.
    static uint32_t namedEntityCodepoint(const std::string& ent) {
        struct Ent { const char* name; uint32_t cp; };
        static const Ent kTable[] = {
            {"nbsp", 0x00A0},   // non-breaking space
            {"shy", 0x00AD},    // soft hyphen: valid line-break point, invisible unless used
            {"ndash", 0x2013},
            {"mdash", 0x2014},
            {"hellip", 0x2026},
            {"laquo", 0x00AB},
            {"raquo", 0x00BB},
            {"ldquo", 0x201C},
            {"rdquo", 0x201D},
            {"lsquo", 0x2018},
            {"rsquo", 0x2019},
            {"copy", 0x00A9},
            {"deg", 0x00B0},
            {"times", 0x00D7},
            {"plusmn", 0x00B1},
        };
        for (auto& e : kTable) if (ent == e.name) return e.cp;
        return 0;
    }
    static void appendUtf8(std::string& dst, uint32_t cp) {
        if (cp <= 0x7F) {
            dst.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            dst.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            dst.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            dst.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    IByteReader& in_;
    size_t bufCap_;
    std::vector<uint8_t> buf_;
    size_t bufLen_ = 0;
    size_t bufPos_ = 0;
    bool eof_ = false;
    bool inCData_ = false;
    int cdataPendingBrackets_ = 0;

    uint32_t tokenStart_ = 0;
    std::string name_;
    std::vector<Fb2Attr> attrs_;
    std::string text_;
    std::string pendingSelfCloseName_;
    size_t textSize_ = 0;
    bool captureText_ = true;
};
