#include "ZipReader.h"
#include "Inflate.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <new>

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kCdSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;
constexpr size_t kEocdSize = 22;
constexpr size_t kCentralHeaderSize = 46;
constexpr size_t kLocalHeaderSize = 30;
constexpr size_t kSearchChunkSize = 4096;
constexpr size_t kZipReadAheadSize = 8192;
constexpr size_t kZipOutputBatchSize = 4096;

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool readExact(IByteReader& r, void* dst, size_t len) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < len) {
        const size_t got = r.read(out + done, len - done);
        if (got == 0) return false;
        done += got;
    }
    return true;
}

// Inflate intentionally keeps a small internal input buffer because it also
// lives next to several Huffman tables. On SD, however, repeatedly asking for
// 1 KiB chunks costs much more than copying those bytes from RAM. This bounded
// slice reader coalesces those requests into up-to-4-KiB physical reads while
// exposing exactly the compressed ZIP entry and never reading past it.
//
// The read-ahead buffer is heap-backed so it does not grow the ESP32-C3 loop
// task stack. If the allocation fails, reads transparently fall back to the
// underlying source without changing decompression correctness.
class BufferedSliceReader final : public IByteReader {
public:
    BufferedSliceReader(IByteReader& source, uint64_t base, uint64_t length)
        : source_(source), base_(base), length_(length),
          buffer_(new (std::nothrow) uint8_t[kZipReadAheadSize]) {}

    size_t read(void* dst, size_t len) override {
        if (cursor_ >= length_ || len == 0) return 0;
        const uint64_t left = length_ - cursor_;
        if (len > left) len = static_cast<size_t>(left);

        if (!buffer_) {
            if (!source_.seek(base_ + cursor_)) return 0;
            const size_t got = source_.read(dst, len);
            cursor_ += got;
            return got;
        }

        auto* out = static_cast<uint8_t*>(dst);
        size_t done = 0;
        while (done < len) {
            if (cursor_ < bufferStart_ || cursor_ >= bufferStart_ + bufferLen_) {
                if (!refill()) break;
            }

            const size_t offset = static_cast<size_t>(cursor_ - bufferStart_);
            const size_t available = bufferLen_ - offset;
            const size_t take = std::min(available, len - done);
            std::memcpy(out + done, buffer_.get() + offset, take);
            done += take;
            cursor_ += take;
        }
        return done;
    }

    bool seek(uint64_t pos) override {
        if (pos > length_) return false;
        cursor_ = pos;
        return true;
    }

    uint64_t tell() const override { return cursor_; }
    uint64_t size() const override { return length_; }

private:
    bool refill() {
        if (cursor_ >= length_) return false;
        const uint64_t absolute = base_ + cursor_;
        // Sequential DEFLATE input already leaves the underlying file at the
        // next compressed byte. Avoid an SD seek before every refill; only
        // reposition after a real logical jump (central-directory skip, etc.).
        if (source_.tell() != absolute && !source_.seek(absolute)) return false;

        const uint64_t left = length_ - cursor_;
        const size_t want = static_cast<size_t>(std::min<uint64_t>(left, kZipReadAheadSize));
        const size_t got = source_.read(buffer_.get(), want);
        if (got == 0) return false;

        bufferStart_ = cursor_;
        bufferLen_ = got;
        return true;
    }

    IByteReader& source_;
    uint64_t base_ = 0;
    uint64_t length_ = 0;
    uint64_t cursor_ = 0;
    std::unique_ptr<uint8_t[]> buffer_;
    uint64_t bufferStart_ = 0;
    size_t bufferLen_ = 0;
};

// Inflate currently emits roughly 1 KiB at a time. The normal FB2.ZIP caller
// writes that stream into the extracted SD cache, so coalescing four of those
// chunks reduces write transactions and std::function dispatches without
// changing the inflater itself. As with read-ahead, allocation failure simply
// falls back to direct streaming.
class BufferedZipOutput {
public:
    explicit BufferedZipOutput(const ZipOutputFn& out)
        : out_(out), buffer_(new (std::nothrow) uint8_t[kZipOutputBatchSize]) {}

    void append(const uint8_t* data, size_t len) {
        if (!buffer_) {
            out_(data, len);
            return;
        }

        while (len > 0) {
            const size_t freeBytes = kZipOutputBatchSize - buffered_;
            const size_t take = std::min(freeBytes, len);
            std::memcpy(buffer_.get() + buffered_, data, take);
            buffered_ += take;
            data += take;
            len -= take;
            if (buffered_ == kZipOutputBatchSize) flush();
        }
    }

    void finish() { flush(); }

private:
    void flush() {
        if (buffered_ == 0) return;
        out_(buffer_.get(), buffered_);
        buffered_ = 0;
    }

    const ZipOutputFn& out_;
    std::unique_ptr<uint8_t[]> buffer_;
    size_t buffered_ = 0;
};

bool hasFb2Suffix(const std::array<uint8_t, 4>& suffix) {
    return suffix[0] == '.' &&
           std::tolower(static_cast<unsigned char>(suffix[1])) == 'f' &&
           std::tolower(static_cast<unsigned char>(suffix[2])) == 'b' &&
           suffix[3] == '2';
}

bool findEocd(IByteReader& zip, uint32_t& cdOffset, uint32_t& cdSize, uint16_t& numEntries) {
    const uint64_t rawSize = zip.size();
    if (rawSize < kEocdSize || rawSize > UINT32_MAX) return false;
    const uint32_t fileSize = static_cast<uint32_t>(rawSize);

    auto tryParseAt = [&](uint32_t pos) -> bool {
        std::array<uint8_t, kEocdSize> header{};
        if (!zip.seek(pos) || !readExact(zip, header.data(), header.size())) return false;
        if (le32(header.data()) != kEocdSig) return false;
        numEntries = le16(header.data() + 10);
        cdSize = le32(header.data() + 12);
        cdOffset = le32(header.data() + 16);
        return static_cast<uint64_t>(cdOffset) + cdSize <= fileSize;
    };

    if (tryParseAt(fileSize - kEocdSize)) return true;

    const uint32_t searchLen = fileSize < (kEocdSize + 65535u) ? fileSize : (kEocdSize + 65535u);
    const uint32_t searchStart = fileSize - searchLen;
    std::unique_ptr<uint8_t[]> large(new (std::nothrow) uint8_t[kSearchChunkSize]);
    std::array<uint8_t, 512> fallback{};
    uint8_t* buffer = large ? large.get() : fallback.data();
    const uint32_t capacity = static_cast<uint32_t>(large ? kSearchChunkSize : fallback.size());

    uint32_t end = fileSize;
    while (end > searchStart) {
        const uint32_t available = end - searchStart;
        const uint32_t chunkLen = available < capacity ? available : capacity;
        const uint32_t chunkStart = end - chunkLen;
        if (!zip.seek(chunkStart) || !readExact(zip, buffer, chunkLen)) return false;

        if (chunkLen >= 4) {
            for (int32_t i = static_cast<int32_t>(chunkLen) - 4; i >= 0; --i) {
                const auto offset = static_cast<size_t>(i);
                if (buffer[offset] == 0x50 && buffer[offset + 1] == 0x4b &&
                    buffer[offset + 2] == 0x05 && buffer[offset + 3] == 0x06) {
                    const uint32_t absolute = chunkStart + static_cast<uint32_t>(i);
                    if (absolute + kEocdSize <= fileSize && tryParseAt(absolute)) return true;
                }
            }
        }

        if (chunkStart == searchStart) break;
        end = chunkStart + 3;
    }
    return false;
}

} // namespace

bool findFb2EntryInZip(IByteReader& zip, ZipEntryInfo& outEntry) {
    uint32_t cdOffset = 0, cdSize = 0;
    uint16_t numEntries = 0;
    if (!findEocd(zip, cdOffset, cdSize, numEntries)) return false;

    // Central-directory records are made from tiny fixed headers followed by
    // short variable-length fields. Reading them directly from SD causes many
    // 46-byte/name reads and seeks. Reuse the same bounded 4-KiB read-ahead
    // layer used by DEFLATE input so sequential metadata parsing normally costs
    // one physical read per block while preserving all existing ZIP semantics.
    BufferedSliceReader central(zip, cdOffset, cdSize);
    std::array<uint8_t, kCentralHeaderSize> header{};
    for (uint16_t i = 0; i < numEntries; ++i) {
        if (!readExact(central, header.data(), header.size()) || le32(header.data()) != kCdSig) return false;

        const uint16_t method = le16(header.data() + 10);
        const uint32_t compSize = le32(header.data() + 20);
        const uint32_t uncompSize = le32(header.data() + 24);
        const uint16_t fnameLen = le16(header.data() + 28);
        const uint16_t extraLen = le16(header.data() + 30);
        const uint16_t commentLen = le16(header.data() + 32);
        const uint32_t localOffset = le32(header.data() + 42);

        const uint64_t filenameStart = central.tell();
        const uint64_t filenameEnd = filenameStart + fnameLen;
        const uint64_t nextEntry = filenameEnd + static_cast<uint32_t>(extraLen) + commentLen;
        if (filenameEnd > central.size() || nextEntry > central.size()) return false;

        bool isFb2 = false;
        if (fnameLen >= 4) {
            std::array<uint8_t, 4> suffix{};
            if (!central.seek(filenameEnd - suffix.size()) ||
                !readExact(central, suffix.data(), suffix.size())) {
                return false;
            }
            isFb2 = hasFb2Suffix(suffix);
        }

        if (isFb2) {
            // Only the matching entry needs its complete path. Avoid allocating
            // and populating a temporary std::string for every unrelated file
            // in archives that contain covers, metadata or other attachments.
            std::string filename(fnameLen, '\0');
            if (!central.seek(filenameStart) ||
                (fnameLen > 0 && !readExact(central, filename.data(), fnameLen))) {
                return false;
            }
            outEntry.filename = std::move(filename);
            outEntry.method = method;
            outEntry.compressedSize = compSize;
            outEntry.uncompressedSize = uncompSize;
            outEntry.localHeaderOffset = localOffset;
            return true;
        }

        if (!central.seek(nextEntry)) return false;
    }
    return false;
}

bool extractZipEntry(IByteReader& zip, const ZipEntryInfo& entry, const ZipOutputFn& out) {
    std::array<uint8_t, kLocalHeaderSize> header{};
    if (!zip.seek(entry.localHeaderOffset) || !readExact(zip, header.data(), header.size())) return false;
    if (le32(header.data()) != kLocalSig) return false;

    const uint16_t fnameLen = le16(header.data() + 26);
    const uint16_t extraLen = le16(header.data() + 28);
    const uint64_t dataOffset = zip.position() + static_cast<uint32_t>(fnameLen) + extraLen;
    if (dataOffset > zip.size() || !zip.seek(dataOffset)) return false;

    if (entry.method == 0) {
        uint32_t remaining = entry.compressedSize;
        std::unique_ptr<uint8_t[]> large(new (std::nothrow) uint8_t[4096]);
        std::array<uint8_t, 512> fallback{};
        uint8_t* buffer = large ? large.get() : fallback.data();
        const size_t capacity = large ? 4096 : fallback.size();
        while (remaining > 0) {
            const size_t want = std::min<size_t>(remaining, capacity);
            const size_t got = zip.read(buffer, want);
            if (got == 0) return false;
            out(buffer, got);
            remaining -= static_cast<uint32_t>(got);
        }
        return true;
    }
    if (entry.method == 8) {
        BufferedSliceReader compressed(zip, dataOffset, entry.compressedSize);
        BufferedZipOutput batched(out);
        const bool ok = inflateRaw(compressed, entry.compressedSize,
                                   [&](const uint8_t* data, size_t len) { batched.append(data, len); });
        batched.finish();
        return ok;
    }
    return false;
}
