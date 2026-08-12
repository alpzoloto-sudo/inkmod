#include "Inflate.h"
#include <cstring>
#include <array>
#include <memory>
#include <vector>

namespace {

// ---- bounded pull-based bit reader over IByteReader --------------------
class BitReader {
public:
    BitReader(IByteReader& in, uint32_t limit) : in_(in), remaining_(limit) {}

    bool ok() const { return ok_; }

    uint32_t getBits(int n) {
        while (bitCount_ < n) {
            int b = getByte();
            if (b < 0) { ok_ = false; return 0; }
            bitBuf_ |= static_cast<uint32_t>(b) << bitCount_;
            bitCount_ += 8;
        }
        uint32_t v = bitBuf_ & ((1u << n) - 1);
        bitBuf_ >>= n;
        bitCount_ -= n;
        return v;
    }

    // DEFLATE Huffman codes are read one bit at a time, MSB-first *within
    // the canonical decode loop* even though the bitstream itself is
    // packed LSB-first per byte — getBits(1) already handles that framing.
    int getBit() { return static_cast<int>(getBits(1)); }

    // Fast Huffman decoder support. DEFLATE codes are usually short; being
    // able to inspect 8 bits at once avoids calling getBit() 5-9 times for
    // most literal/distance symbols. ensureBitsFast() is deliberately
    // non-fatal: near the exact end of the compressed stream there may be
    // fewer than 8 source bits left while still having enough buffered bits
    // for a valid shorter code, in which case the caller falls back to the
    // canonical bit-by-bit decoder.
    bool ensureBitsFast(int n) {
        while (bitCount_ < n) {
            if (bufPos_ >= bufLen_) {
                if (remaining_ == 0) return false;
                size_t want = remaining_ < sizeof(buf_) ? remaining_ : sizeof(buf_);
                bufLen_ = in_.read(buf_, want);
                bufPos_ = 0;
                if (bufLen_ == 0) return false;
                remaining_ -= static_cast<uint32_t>(bufLen_);
            }
            bitBuf_ |= static_cast<uint32_t>(buf_[bufPos_++]) << bitCount_;
            bitCount_ += 8;
        }
        return true;
    }

    uint32_t peekBitsFast(int n) const { return bitBuf_ & ((1u << n) - 1u); }
    void dropBitsFast(int n) {
        bitBuf_ >>= n;
        bitCount_ -= n;
    }

    // Discard any partial byte in the bit buffer (used before a stored
    // block, which is byte-aligned per RFC 1951 §3.2.4).
    void byteAlign() { bitBuf_ = 0; bitCount_ = 0; }

    // Raw byte read, bypassing the bit buffer entirely (only valid right
    // after byteAlign()).
    int rawByte() { return getByte(); }

private:
    int getByte() {
        if (bufPos_ >= bufLen_) {
            if (remaining_ == 0) return -1;
            size_t want = remaining_ < sizeof(buf_) ? remaining_ : sizeof(buf_);
            bufLen_ = in_.read(buf_, want);
            bufPos_ = 0;
            if (bufLen_ == 0) return -1;
            remaining_ -= static_cast<uint32_t>(bufLen_);
        }
        return buf_[bufPos_++];
    }

    IByteReader& in_;
    uint32_t remaining_;
    uint8_t buf_[1024];
    size_t bufLen_ = 0;
    size_t bufPos_ = 0;
    uint32_t bitBuf_ = 0;
    int bitCount_ = 0;
    bool ok_ = true;
};

// ---- canonical Huffman decode table (RFC 1951 reference-decoder shape) --
struct Huff {
    uint16_t counts[16] = {0};   // counts[len] = number of symbols with that code length
    std::vector<uint16_t> symbols; // symbols ordered by (length, symbol value)

    // A compact first-level decode table. One uint16_t packs:
    //   high 4 bits = code length (1..8)
    //   low 12 bits = symbol
    // 0xffff means "use the canonical slow path".
    //
    // 8 bits is intentional: 256 * 2 = 512 bytes per Huff object, small
    // enough for ESP32-C3, while catching most literal and virtually all
    // common distance codes. A 9/10-bit table would cost considerably more
    // stack for only a modest extra hit rate.
    static constexpr int kFastBits = 8;
    static constexpr uint16_t kFastMiss = 0xffffu;
    std::array<uint16_t, 1u << kFastBits> fast{};

    static uint16_t reverseBits(uint16_t code, int len) {
        uint16_t r = 0;
        for (int i = 0; i < len; ++i) {
            r = static_cast<uint16_t>((r << 1) | (code & 1u));
            code >>= 1;
        }
        return r;
    }

    // code_lengths[i] = bit length of symbol i (0 = symbol unused).
    void build(const uint8_t* code_lengths, int n) {
        fast.fill(kFastMiss);
        for (int i = 0; i < 16; i++) counts[i] = 0;
        for (int i = 0; i < n; i++) counts[code_lengths[i]]++;
        counts[0] = 0;

        uint16_t offsets[16] = {0};
        offsets[1] = 0;
        for (int len = 1; len < 15; len++) offsets[len + 1] = offsets[len] + counts[len];

        symbols.assign(static_cast<size_t>(n), 0);
        for (int i = 0; i < n; i++) {
            uint8_t len = code_lengths[i];
            if (len != 0) symbols[offsets[len]++] = static_cast<uint16_t>(i);
        }

        // Build canonical codes, reverse them for DEFLATE's LSB-first
        // bitstream, then replicate each short code across all table entries
        // that share that prefix.
        uint16_t nextCode[16] = {0};
        uint16_t code = 0;
        for (int bits = 1; bits <= 15; ++bits) {
            code = static_cast<uint16_t>((code + counts[bits - 1]) << 1);
            nextCode[bits] = code;
        }
        for (int sym = 0; sym < n; ++sym) {
            const int len = code_lengths[sym];
            if (len <= 0 || len > kFastBits) continue;
            const uint16_t canonical = nextCode[len]++;
            const uint16_t reversed = reverseBits(canonical, len);
            const uint16_t packed = static_cast<uint16_t>((len << 12) | (sym & 0x0fffu));
            const uint16_t step = static_cast<uint16_t>(1u << len);
            for (uint16_t slot = reversed; slot < fast.size(); slot = static_cast<uint16_t>(slot + step)) {
                fast[slot] = packed;
            }
        }
    }

    // Returns the decoded symbol, or -1 on a malformed code.
    int decode(BitReader& br) const {
        // Fast path: one table lookup + one shift instead of 5-9 calls to
        // getBit() for the overwhelming majority of real DEFLATE symbols.
        if (br.ensureBitsFast(kFastBits)) {
            const uint16_t packed = fast[br.peekBitsFast(kFastBits)];
            if (packed != kFastMiss) {
                const int len = packed >> 12;
                br.dropBitsFast(len);
                return packed & 0x0fff;
            }
        }

        // Canonical reference path for codes longer than 8 bits, malformed
        // inputs, and the final few buffered bits at end-of-stream.
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; len++) {
            code |= br.getBit();
            if (!br.ok()) return -1;
            int count = counts[len];
            if (code - first < count) return symbols[static_cast<size_t>(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

// RFC 1951 §3.2.5 length/distance extra-bit and base-value tables.
const uint16_t kLenBase[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint8_t  kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDistBase[30]  = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
const int kCodeLenOrder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

// 32KB sliding window: DEFLATE back-references can point up to 32768 bytes
// behind the current output position, so a general-purpose decompressor
// needs a buffer at least that large, full stop.
class Window {
public:
    // buf_ used to be a plain uint8_t[32768] member - meaning a whole
    // Window instance (this buffer plus everything else) sat directly on
    // whichever stack constructed it. inflateRaw() below constructs one as
    // a local variable, so every call put 32KB+ on the CALLING task's
    // stack - on this device that's the "loopTask" stack, which is only
    // 8-16KB total, guaranteeing overflow the moment a real deflate stream
    // needed the full window (not always on the very first call - depends
    // on when extraction actually happened to run). Heap-allocating it
    // instead keeps Window itself small enough to live on the stack
    // safely; the 32KB now lives on the heap, which has roughly 275KB
    // available on this device - not remotely the same constraint.
    explicit Window(const InflateOutputFn& out) : out_(out), buf_(new uint8_t[kWindowSize]) {
        std::memset(buf_.get(), 0, kWindowSize);
    }
    // Emits any bytes still sitting in the output buffer. Also runs via the
    // destructor, so every inflateRaw() exit path (success or the several
    // early `return false`s in inflateBlock()) flushes automatically -
    // nothing here otherwise needs to change to stay correct.
    ~Window() { flushOut(); }

    void putByte(uint8_t b) {
        buf_[pos_++ & kMask] = b;
        // `out_` is a std::function - type-erased, so every invocation has
        // real overhead beyond whatever the caller's callback body itself
        // does. Calling it once per decompressed byte (previously: no
        // batching at all here) means a book with a few MB of text turns
        // into a few million calls just to satisfy that overhead, which on
        // a weak MCU is enough by itself to make decompression the
        // dominant cost - independent of how efficiently the caller then
        // writes those bytes onward. Buffering locally and calling `out_`
        // in ~1KB chunks doesn't change what bytes get produced or when a
        // caller can rely on them (still fully sequential, still delivered
        // before inflateRaw() returns) - only how many times the call
        // itself happens.
        outBuf_[outLen_++] = b;
        if (outLen_ == sizeof(outBuf_)) flushOut();
    }
    // Copies `length` bytes from `distance` bytes back in the output
    // stream (distance <= 32768). Must go one byte at a time (not memcpy)
    // because source and destination ranges legitimately overlap when
    // distance < length (that's how DEFLATE encodes run-length repeats).
    bool copyMatch(uint32_t distance, uint32_t length) {
        if (distance == 0 || distance > pos_) return false; // reference before start of stream
        for (uint32_t i = 0; i < length; i++) {
            uint8_t b = buf_[(pos_ - distance) & kMask];
            putByte(b);
        }
        return true;
    }

private:
    void flushOut() {
        if (outLen_ > 0) {
            out_(outBuf_, outLen_);
            outLen_ = 0;
        }
    }

    static constexpr uint32_t kWindowSize = 32768;
    static constexpr uint32_t kMask = kWindowSize - 1;
    const InflateOutputFn& out_;
    std::unique_ptr<uint8_t[]> buf_;
    uint32_t pos_ = 0;
    uint8_t outBuf_[1024];
    size_t outLen_ = 0;
};

bool inflateBlock(BitReader& br, Window& win, bool& outFinal) {
    outFinal = br.getBits(1) != 0;
    uint32_t btype = br.getBits(2);
    if (!br.ok()) return false;

    if (btype == 0) {
        // Stored (uncompressed) block: byte-align, then LEN/NLEN (16-bit
        // LE each, NLEN is one's-complement of LEN), then LEN raw bytes.
        br.byteAlign();
        int lenLo = br.rawByte(), lenHi = br.rawByte();
        int nlenLo = br.rawByte(), nlenHi = br.rawByte();
        if (lenLo < 0 || lenHi < 0 || nlenLo < 0 || nlenHi < 0) return false;
        uint16_t len = static_cast<uint16_t>(lenLo | (lenHi << 8));
        uint16_t nlen = static_cast<uint16_t>(nlenLo | (nlenHi << 8));
        if (static_cast<uint16_t>(~len) != nlen) return false; // corrupt block header
        for (uint16_t i = 0; i < len; i++) {
            int b = br.rawByte();
            if (b < 0) return false;
            win.putByte(static_cast<uint8_t>(b));
        }
        return true;
    }

    if (btype == 3) return false; // reserved value: invalid stream

    Huff litLen, dist;
    if (btype == 1) {
        // Fixed Huffman codes, hardwired lengths per RFC 1951 §3.2.6.
        uint8_t litLenLens[288];
        for (int i = 0; i < 144; i++) litLenLens[i] = 8;
        for (int i = 144; i < 256; i++) litLenLens[i] = 9;
        for (int i = 256; i < 280; i++) litLenLens[i] = 7;
        for (int i = 280; i < 288; i++) litLenLens[i] = 8;
        litLen.build(litLenLens, 288);

        uint8_t distLens[30];
        for (int i = 0; i < 30; i++) distLens[i] = 5;
        dist.build(distLens, 30);
    } else {
        // Dynamic Huffman codes: header describes the two code tables.
        uint32_t hlit = br.getBits(5) + 257;
        uint32_t hdist = br.getBits(5) + 1;
        uint32_t hclen = br.getBits(4) + 4;
        if (!br.ok()) return false;

        uint8_t clcLens[19] = {0};
        for (uint32_t i = 0; i < hclen; i++) {
            clcLens[kCodeLenOrder[i]] = static_cast<uint8_t>(br.getBits(3));
        }
        Huff clc;
        clc.build(clcLens, 19);

        // RFC 1951 bounds: HLIT <= 286 and HDIST <= 32, so 318 bytes is
        // enough for every valid dynamic block. Fixed storage avoids a heap
        // allocation/free for every block and therefore reduces fragmentation
        // during large FB2.ZIP extraction.
        std::array<uint8_t, 318> lens{};
        const uint32_t lensCount = hlit + hdist;
        uint32_t i = 0;
        while (i < lensCount) {
            int sym = clc.decode(br);
            if (sym < 0) return false;
            if (sym < 16) {
                lens[i++] = static_cast<uint8_t>(sym);
            } else if (sym == 16) {
                if (i == 0) return false;
                uint32_t rep = br.getBits(2) + 3;
                uint8_t prev = lens[i - 1];
                while (rep-- > 0 && i < lensCount) lens[i++] = prev;
            } else if (sym == 17) {
                uint32_t rep = br.getBits(3) + 3;
                while (rep-- > 0 && i < lensCount) lens[i++] = 0;
            } else { // 18
                uint32_t rep = br.getBits(7) + 11;
                while (rep-- > 0 && i < lensCount) lens[i++] = 0;
            }
            if (!br.ok()) return false;
        }
        litLen.build(lens.data(), static_cast<int>(hlit));
        dist.build(lens.data() + hlit, static_cast<int>(hdist));
    }

    for (;;) {
        int sym = litLen.decode(br);
        if (sym < 0) return false;
        if (sym < 256) {
            win.putByte(static_cast<uint8_t>(sym));
            continue;
        }
        if (sym == 256) return true; // end of block

        sym -= 257;
        if (sym >= 29) return false; // invalid length code
        uint32_t length = kLenBase[sym] + br.getBits(kLenExtra[sym]);
        if (!br.ok()) return false;

        int dsym = dist.decode(br);
        if (dsym < 0 || dsym >= 30) return false;
        uint32_t distance = kDistBase[dsym] + br.getBits(kDistExtra[dsym]);
        if (!br.ok()) return false;

        if (!win.copyMatch(distance, length)) return false;
    }
}

} // namespace

bool inflateRaw(IByteReader& in, uint32_t compressedLimit, const InflateOutputFn& out) {
    BitReader br(in, compressedLimit);
    Window win(out);
    bool final = false;
    do {
        if (!inflateBlock(br, win, final)) return false;
    } while (!final);
    return true;
}
