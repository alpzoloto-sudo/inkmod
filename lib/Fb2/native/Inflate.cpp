#include "Inflate.h"

#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace {

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

    int getBit() { return static_cast<int>(getBits(1)); }

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

    void byteAlign() { bitBuf_ = 0; bitCount_ = 0; }
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

struct Huff {
    uint16_t counts[16] = {0};
    std::vector<uint16_t> symbols;

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

    int decode(BitReader& br) const {
        if (br.ensureBitsFast(kFastBits)) {
            const uint16_t packed = fast[br.peekBitsFast(kFastBits)];
            if (packed != kFastMiss) {
                const int len = packed >> 12;
                br.dropBitsFast(len);
                return packed & 0x0fff;
            }
        }

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

const uint16_t kLenBase[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint8_t  kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDistBase[30]  = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
const int kCodeLenOrder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

class Window {
public:
    // No zero-fill is needed: DEFLATE can only reference bytes already
    // emitted, and copyMatch() enforces distance <= pos_. Avoiding a 32 KiB
    // memset saves pure memory-bandwidth work on every FB2.ZIP extraction.
    explicit Window(const InflateOutputFn& out) : out_(out), buf_(new (std::nothrow) uint8_t[kWindowSize]) {}
    ~Window() { flushOut(); }

    bool valid() const { return static_cast<bool>(buf_); }

    void putByte(uint8_t b) {
        buf_[pos_++ & kMask] = b;
        outBuf_[outLen_++] = b;
        if (outLen_ == sizeof(outBuf_)) flushOut();
    }

    bool copyMatch(uint32_t distance, uint32_t length) {
        if (distance == 0 || distance > pos_) return false;

        // A distance-1 match is a run. Fill the ring and output in contiguous
        // chunks instead of routing every byte through putByte().
        if (distance == 1) {
            const uint8_t repeated = buf_[(pos_ - 1) & kMask];
            while (length > 0) {
                const uint32_t dst = pos_ & kMask;
                const uint32_t chunk = std::min<uint32_t>(length, kWindowSize - dst);
                std::memset(buf_.get() + dst, repeated, chunk);
                appendOut(buf_.get() + dst, chunk);
                pos_ += chunk;
                length -= chunk;
            }
            return true;
        }

        // LZ77 matches may overlap. Copy at most one distance at a time so
        // each chunk only reads bytes that were already produced; the next
        // chunk can then reuse bytes written by the previous one. Ring-wrap
        // boundaries keep both source and destination spans contiguous.
        while (length > 0) {
            const uint32_t src = (pos_ - distance) & kMask;
            const uint32_t dst = pos_ & kMask;
            uint32_t chunk = std::min<uint32_t>(length, distance);
            chunk = std::min<uint32_t>(chunk, kWindowSize - src);
            chunk = std::min<uint32_t>(chunk, kWindowSize - dst);
            if (chunk == 0) return false;

            if (src != dst) std::memmove(buf_.get() + dst, buf_.get() + src, chunk);
            appendOut(buf_.get() + dst, chunk);
            pos_ += chunk;
            length -= chunk;
        }
        return true;
    }

private:
    void appendOut(const uint8_t* data, size_t len) {
        while (len > 0) {
            const size_t space = sizeof(outBuf_) - outLen_;
            const size_t take = std::min(space, len);
            std::memcpy(outBuf_ + outLen_, data, take);
            outLen_ += take;
            data += take;
            len -= take;
            if (outLen_ == sizeof(outBuf_)) flushOut();
        }
    }

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
        br.byteAlign();
        int lenLo = br.rawByte(), lenHi = br.rawByte();
        int nlenLo = br.rawByte(), nlenHi = br.rawByte();
        if (lenLo < 0 || lenHi < 0 || nlenLo < 0 || nlenHi < 0) return false;
        uint16_t len = static_cast<uint16_t>(lenLo | (lenHi << 8));
        uint16_t nlen = static_cast<uint16_t>(nlenLo | (nlenHi << 8));
        if (static_cast<uint16_t>(~len) != nlen) return false;
        for (uint16_t i = 0; i < len; i++) {
            int b = br.rawByte();
            if (b < 0) return false;
            win.putByte(static_cast<uint8_t>(b));
        }
        return true;
    }

    if (btype == 3) return false;

    Huff litLen, dist;
    if (btype == 1) {
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
            } else {
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
        if (sym == 256) return true;

        sym -= 257;
        if (sym >= 29) return false;
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
    if (!win.valid()) return false;
    bool final = false;
    do {
        if (!inflateBlock(br, win, final)) return false;
    } while (!final);
    return true;
}
