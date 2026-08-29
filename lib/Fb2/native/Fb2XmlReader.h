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
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "IByteReader.h"

enum class Fb2Token {
    None,
    StartTag,
    EndTag,
    SelfClosing,
    Text,
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

    uint32_t tokenStartOffset() const { return tokenStart_; }
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

    void seekTo(uint32_t pos) {
        in_.seek(pos);
        bufLen_ = bufPos_ = 0;
        pendingSelfCloseName_.clear();
        eof_ = false;
        inCData_ = false;
        cdataPendingBrackets_ = 0;
    }

    Fb2Token next() {
        if (inCData_) return readCDataChunk();
        if (!pendingSelfCloseName_.empty()) {
            name_ = pendingSelfCloseName_;
            pendingSelfCloseName_.clear();
            return Fb2Token::EndTag;
        }

        for (;;) {
            int c = peekByte();
            if (c < 0) return Fb2Token::Eof;
            if (c == '<') return readMarkup();
            return readText();
        }
    }

private:
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
        if (bufLen_ == 0) {
            eof_ = true;
            return false;
        }
        return true;
    }

    uint32_t curAbsPos() const {
        return in_.position() - static_cast<uint32_t>(bufLen_ - bufPos_);
    }

    Fb2Token readMarkup() {
        tokenStart_ = curAbsPos();
        getByte();
        int c = peekByte();
        if (c == '?') { skipUntil("?>"); return next(); }
        if (c == '!') {
            if (matchAhead("![CDATA[")) {
                cdataPendingBrackets_ = 0;
                return readCDataChunk();
            }
            if (matchAhead("!--")) { skipUntil("-->"); return next(); }
            skipUntil(">");
            return next();
        }

        bool closing = false;
        if (c == '/') {
            closing = true;
            getByte();
        }

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
            if (p == '>') {
                getByte();
                break;
            }

            Fb2Attr a;
            readNameInto(a.name);
            if (a.name.empty()) return Fb2Token::Error;
            skipWs();
            if (peekByte() == '=') {
                getByte();
                skipWs();
                int q = getByte();
                std::string val;
                while (true) {
                    int ch = getByte();
                    if (ch < 0 || ch == q) break;
                    appendDecoded(val, ch);
                }
                a.value = std::move(val);
            }
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
            // Scan-only mode is used for almost all body text and for FB2
            // <binary> payloads. Short XML text runs still use the old single
            // pointer pass because two libc calls cost more there. Long runs
            // (especially base64, which normally contains neither '<' nor
            // '&') use memchr so newlib can scan machine words instead of one
            // C++ byte comparison at a time.
            constexpr size_t kMemchrThreshold = 128;
            while (textSize_ < bufCap_) {
                if (bufPos_ >= bufLen_ && !refill()) break;
                const size_t remaining = bufCap_ - textSize_;
                const size_t available = std::min(bufLen_ - bufPos_, remaining);
                const uint8_t* begin = buf_.data() + bufPos_;
                const uint8_t* end = begin + available;
                const uint8_t* special = end;

                if (available >= kMemchrThreshold) {
                    const void* ltHit = std::memchr(begin, '<', available);
                    const void* ampHit = std::memchr(begin, '&', available);
                    if (ltHit) special = static_cast<const uint8_t*>(ltHit);
                    if (ampHit) {
                        const auto* amp = static_cast<const uint8_t*>(ampHit);
                        if (amp < special) special = amp;
                    }
                } else {
                    special = begin;
                    while (special < end && *special != '<' && *special != '&') ++special;
                }

                if (special == end) {
                    bufPos_ += available;
                    textSize_ += available;
                    continue;
                }

                const size_t plainBytes = static_cast<size_t>(special - begin);
                bufPos_ += plainBytes;
                textSize_ += plainBytes;
                if (textSize_ >= bufCap_) break;
                if (*special == '<') break;

                bufPos_++;
                textSize_ += consumeEntityDecodedLength();
            }
            return textSize_ == 0 ? next() : Fb2Token::Text;
        }

        while (textSize_ < bufCap_) {
            int c = peekByte();
            if (c < 0 || c == '<') break;
            getByte();
            appendDecoded(text_, c);
            textSize_ = text_.size();
        }

        // A text token may be chunked at bufCap_, but the boundary must never
        // split a UTF-8 codepoint. FB2 sources commonly use U+00A0 as their
        // ordinary inter-word separator (bytes C2 A0). The old byte-counted
        // chunking could return a token ending in C2 and the next token
        // starting in A0. Downstream FB2 whitespace normalization then saw
        // neither token as NBSP and the renderer visually glued the two words.
        // Complete only an already-started UTF-8 sequence; at most three extra
        // bytes are appended, so the bounded-memory behaviour is unchanged.
        completeTrailingUtf8Sequence();
        textSize_ = text_.size();
        return textSize_ == 0 ? next() : Fb2Token::Text;
    }

    Fb2Token readCDataChunk() {
        tokenStart_ = curAbsPos();
        text_.clear();
        textSize_ = 0;
        for (;;) {
            int c = getByte();
            if (c < 0) {
                inCData_ = false;
                break;
            }
            if (c == ']') {
                cdataPendingBrackets_++;
                if (cdataPendingBrackets_ > 2) {
                    text_.push_back(']');
                    cdataPendingBrackets_ = 2;
                }
            } else if (c == '>' && cdataPendingBrackets_ >= 2) {
                cdataPendingBrackets_ = 0;
                inCData_ = false;
                break;
            } else {
                while (cdataPendingBrackets_ > 0) {
                    text_.push_back(']');
                    cdataPendingBrackets_--;
                }
                text_.push_back(static_cast<char>(c));
            }
            if (text_.size() >= bufCap_) {
                textSize_ = text_.size();
                inCData_ = true;
                return Fb2Token::Text;
            }
        }
        textSize_ = text_.size();
        return text_.empty() ? next() : Fb2Token::Text;
    }

    void completeTrailingUtf8Sequence() {
        if (text_.empty()) return;

        const size_t n = text_.size();
        size_t lead = n - 1;
        // Walk back over continuation bytes already present in this token.
        size_t walked = 0;
        while (lead > 0 && walked < 3) {
            const uint8_t b = static_cast<uint8_t>(text_[lead]);
            if ((b & 0xC0u) != 0x80u) break;
            --lead;
            ++walked;
        }

        const uint8_t first = static_cast<uint8_t>(text_[lead]);
        size_t expected = 1;
        if ((first & 0xE0u) == 0xC0u) expected = 2;
        else if ((first & 0xF0u) == 0xE0u) expected = 3;
        else if ((first & 0xF8u) == 0xF0u) expected = 4;
        else return;  // ASCII, continuation without a lead, or invalid lead.

        const size_t have = n - lead;
        if (have >= expected) return;

        for (size_t i = have; i < expected; ++i) {
            const int next = peekByte();
            if (next < 0) return;
            const uint8_t b = static_cast<uint8_t>(next);
            if ((b & 0xC0u) != 0x80u) return;  // malformed UTF-8: don't eat markup/text.
            getByte();
            text_.push_back(static_cast<char>(b));
        }
    }

    void readNameInto(std::string& dst) {
        for (;;) {
            int c = peekByte();
            if (c < 0) break;
            if (isNameChar(static_cast<unsigned char>(c))) {
                dst.push_back(static_cast<char>(c));
                getByte();
            } else {
                break;
            }
        }
    }

    static bool isNameChar(unsigned char c) {
        return (c == ':' || c == '-' || c == '.' || c == '_' ||
                (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') || c >= 0x80);
    }

    void skipWs() {
        for (;;) {
            int c = peekByte();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') getByte();
            else break;
        }
    }

    bool matchAhead(const char* lit) {
        size_t save = bufPos_;
        for (const char* p = lit; *p; ++p) {
            int c = getByte();
            if (c != *p) {
                bufPos_ = save;
                return false;
            }
        }
        return true;
    }

    void skipUntil(const char* closer) {
        size_t n = std::strlen(closer);
        size_t matched = 0;
        for (;;) {
            int c = getByte();
            if (c < 0) return;
            matched = (static_cast<char>(c) == closer[matched])
                          ? matched + 1
                          : (static_cast<char>(c) == closer[0] ? 1 : 0);
            if (matched == n) return;
        }
    }

    static bool entityEquals(const char* value, size_t len, const char* literal, size_t literalLen) {
        return len == literalLen && std::memcmp(value, literal, literalLen) == 0;
    }

    size_t readEntityName(char (&ent)[13]) {
        size_t len = 0;
        int c;
        while ((c = peekByte()) >= 0 && c != ';' && len < 12) {
            ent[len++] = static_cast<char>(c);
            getByte();
        }
        if (peekByte() == ';') getByte();
        ent[len] = '\0';
        return len;
    }

    void appendDecoded(std::string& dst, int firstByte) {
        if (firstByte != '&') {
            dst.push_back(static_cast<char>(firstByte));
            return;
        }
        char ent[13];
        const size_t len = readEntityName(ent);

        if (entityEquals(ent, len, "lt", 2)) dst.push_back('<');
        else if (entityEquals(ent, len, "gt", 2)) dst.push_back('>');
        else if (entityEquals(ent, len, "amp", 3)) dst.push_back('&');
        else if (entityEquals(ent, len, "quot", 4)) dst.push_back('"');
        else if (entityEquals(ent, len, "apos", 4)) dst.push_back('\'');
        else if (len > 0 && ent[0] == '#') {
            const long cp = (len > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                                ? std::strtol(ent + 2, nullptr, 16)
                                : std::strtol(ent + 1, nullptr, 10);
            appendUtf8(dst, static_cast<uint32_t>(cp));
        } else if (const uint32_t cp = namedEntityCodepoint(ent, len)) {
            appendUtf8(dst, cp);
        }
    }

    size_t consumeEntityDecodedLength() {
        char ent[13];
        const size_t len = readEntityName(ent);

        if (entityEquals(ent, len, "lt", 2) || entityEquals(ent, len, "gt", 2) ||
            entityEquals(ent, len, "amp", 3) || entityEquals(ent, len, "quot", 4) ||
            entityEquals(ent, len, "apos", 4)) return 1;
        uint32_t cp = 0;
        if (len > 0 && ent[0] == '#') {
            cp = static_cast<uint32_t>((len > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                                           ? std::strtol(ent + 2, nullptr, 16)
                                           : std::strtol(ent + 1, nullptr, 10));
        } else {
            cp = namedEntityCodepoint(ent, len);
        }
        if (cp <= 0x7F) return cp == 0 ? 0 : 1;
        if (cp <= 0x7FF) return 2;
        if (cp <= 0xFFFF) return 3;
        return 4;
    }

    static uint32_t namedEntityCodepoint(const char* ent, size_t len) {
        struct Ent { const char* name; uint8_t len; uint32_t cp; };
        static const Ent kTable[] = {
            {"nbsp", 4, 0x00A0},
            {"shy", 3, 0x00AD},
            {"ndash", 5, 0x2013},
            {"mdash", 5, 0x2014},
            {"hellip", 6, 0x2026},
            {"laquo", 5, 0x00AB},
            {"raquo", 5, 0x00BB},
            {"ldquo", 5, 0x201C},
            {"rdquo", 5, 0x201D},
            {"lsquo", 5, 0x2018},
            {"rsquo", 5, 0x2019},
            {"copy", 4, 0x00A9},
            {"deg", 3, 0x00B0},
            {"times", 5, 0x00D7},
            {"plusmn", 6, 0x00B1},
        };
        for (const auto& e : kTable) {
            if (len == e.len && std::memcmp(ent, e.name, len) == 0) return e.cp;
        }
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
