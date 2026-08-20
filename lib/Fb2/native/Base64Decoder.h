// Base64Decoder.h
//
// Streaming, bounded-memory base64 decoder. FB2 <binary> elements (covers,
// inline images) can be hundreds of KB of base64 text — on an ESP32-C3 with
// ~380KB of usable RAM we cannot read that into a std::string. This decoder
// consumes input a chunk at a time and emits decoded bytes via a callback,
// carrying only a tiny fixed output batch plus one partial base64 group.

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

class Base64Decoder {
public:
    // Called with bounded runs of decoded bytes. Older code called this once
    // per base64 quartet (normally only 3 output bytes), which turned a
    // 300-KiB image into roughly 100,000 std::function invocations. Batching
    // those bytes locally removes that CPU overhead without buffering the
    // image itself.
    using OutputFn = std::function<void(const uint8_t* data, size_t len)>;

    explicit Base64Decoder(OutputFn out) : out_(std::move(out)) {}

    // Feed a chunk of base64 *text* (whitespace/newlines are skipped, which
    // FB2 producers commonly insert to wrap long binaries). Safe to call with
    // arbitrarily small or large chunks, including single bytes.
    void feed(const char* text, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c == '=') continue;
            const int v = decodeChar(c);
            if (v < 0) continue;

            accumulator_ = (accumulator_ << 6) | static_cast<uint32_t>(v);
            if (++sextets_ == 4) {
                emitByte(static_cast<uint8_t>(accumulator_ >> 16));
                emitByte(static_cast<uint8_t>(accumulator_ >> 8));
                emitByte(static_cast<uint8_t>(accumulator_));
                accumulator_ = 0;
                sextets_ = 0;
            }
        }
    }

    // Flush a final padded/partial quartet, then the bounded output batch.
    void finish() {
        if (sextets_ >= 2) {
            const uint32_t triple = accumulator_ << ((4 - sextets_) * 6);
            emitByte(static_cast<uint8_t>(triple >> 16));
            if (sextets_ >= 3) emitByte(static_cast<uint8_t>(triple >> 8));
        }
        accumulator_ = 0;
        sextets_ = 0;
        flushOutput();
    }

private:
    static constexpr size_t kOutputBatchSize = 384;

    static int decodeChar(unsigned char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }

    void emitByte(uint8_t value) {
        output_[output_len_++] = value;
        if (output_len_ == kOutputBatchSize) flushOutput();
    }

    void flushOutput() {
        if (output_len_ == 0) return;
        out_(output_, output_len_);
        output_len_ = 0;
    }

    OutputFn out_;
    uint32_t accumulator_ = 0;
    uint8_t sextets_ = 0;
    uint8_t output_[kOutputBatchSize] = {};
    size_t output_len_ = 0;
};
