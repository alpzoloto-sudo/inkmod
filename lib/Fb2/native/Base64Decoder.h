// Base64Decoder.h
//
// Streaming, allocation-free base64 decoder. FB2 <binary> elements (covers,
// inline images) can be hundreds of KB of base64 text — on an ESP32-C3 with
// ~380KB of usable RAM we cannot read that into a std::string. This decoder
// consumes input a chunk at a time and emits decoded bytes via a callback,
// carrying at most 3 pending bytes of state between calls.

#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

class Base64Decoder {
public:
    // Called with each run of decoded bytes as they become available.
    using OutputFn = std::function<void(const uint8_t* data, size_t len)>;

    explicit Base64Decoder(OutputFn out) : out_(std::move(out)) {}

    // Feed a chunk of base64 *text* (whitespace/newlines are skipped, which
    // FB2 producers commonly insert to wrap long binaries). Safe to call with
    // arbitrarily small or large chunks, including single bytes.
    void feed(const char* text, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c == '=' ) { pad_seen_++; continue; }
            const int v = decodeChar(c);
            if (v < 0) continue; // whitespace / non-alphabet char, skip

            group_[group_len_++] = static_cast<uint8_t>(v);
            if (group_len_ == 4) {
                flushGroup(4);
                group_len_ = 0;
            }
        }
    }

    // Call once after the last feed() to flush a trailing partial group
    // (base64 length not a multiple of 4 because of '=' padding).
    void finish() {
        if (group_len_ > 1) {
            flushGroup(group_len_);
        }
        group_len_ = 0;
        pad_seen_ = 0;
    }

private:
    static int decodeChar(unsigned char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }

    // n is 2..4 valid sextets currently in group_.
    void flushGroup(int n) {
        uint8_t out[3];
        const uint32_t triple = (static_cast<uint32_t>(group_[0]) << 18) |
                                 (static_cast<uint32_t>(n > 1 ? group_[1] : 0) << 12) |
                                 (static_cast<uint32_t>(n > 2 ? group_[2] : 0) << 6) |
                                 (static_cast<uint32_t>(n > 3 ? group_[3] : 0));
        out[0] = static_cast<uint8_t>((triple >> 16) & 0xFF);
        out[1] = static_cast<uint8_t>((triple >> 8) & 0xFF);
        out[2] = static_cast<uint8_t>(triple & 0xFF);

        // Bytes actually valid depend on how many sextets we had:
        // 2 sextets -> 1 byte, 3 sextets -> 2 bytes, 4 sextets -> 3 bytes.
        const size_t validBytes = (n == 2) ? 1 : (n == 3) ? 2 : 3;
        out_(out, validBytes);
    }

    OutputFn out_;
    uint8_t group_[4] = {0, 0, 0, 0};
    int group_len_ = 0;
    int pad_seen_ = 0;
};
