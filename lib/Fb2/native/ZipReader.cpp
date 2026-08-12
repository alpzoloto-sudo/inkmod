#include "ZipReader.h"
#include "Inflate.h"
#include <vector>
#include <cctype>
#include <cstring>

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kCdSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;

uint16_t readU16(IByteReader& r) {
    uint8_t b[2] = {0, 0};
    r.read(b, 2);
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
}
uint32_t readU32(IByteReader& r) {
    uint8_t b[4] = {0, 0, 0, 0};
    r.read(b, 4);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}
void skip(IByteReader& r, uint32_t n) { r.seek(r.position() + n); }

bool endsWithFb2CaseInsensitive(const std::string& name) {
    if (name.size() < 4) return false;
    std::string tail = name.substr(name.size() - 4);
    for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ".fb2";
}

// Finds the End Of Central Directory record and returns the Central
// Directory's offset/size. Tries the comment-less fast path (EOCD is
// exactly the last 22 bytes) first, since that covers the overwhelming
// majority of real files without needing a large transient buffer; only
// falls back to scanning up to 64KB of trailing bytes (the ZIP spec's
// maximum possible archive comment) for the rarer case of a non-empty
// comment field.
bool findEocd(IByteReader& zip, uint32_t& cdOffset, uint32_t& cdSize, uint16_t& numEntries) {
    uint32_t fileSize = zip.size();
    if (fileSize < 22) return false;

    auto tryParseAt = [&](uint32_t pos) -> bool {
        if (!zip.seek(pos)) return false;
        if (readU32(zip) != kEocdSig) return false;
        skip(zip, 2 + 2 + 2); // disk#, cd-start-disk#, entries-this-disk
        numEntries = readU16(zip);
        cdSize = readU32(zip);
        cdOffset = readU32(zip);
        return true;
    };

    if (tryParseAt(fileSize - 22)) return true;

    uint32_t tailLen = fileSize < (22 + 65535u) ? fileSize : (22 + 65535u);
    uint32_t tailStart = fileSize - tailLen;
    std::vector<uint8_t> tail(tailLen);
    zip.seek(tailStart);
    if (zip.read(tail.data(), tailLen) != tailLen) return false;

    for (int32_t i = static_cast<int32_t>(tailLen) - 22; i >= 0; i--) {
        if (tail[static_cast<size_t>(i)] == 0x50 && tail[static_cast<size_t>(i) + 1] == 0x4b &&
            tail[static_cast<size_t>(i) + 2] == 0x05 && tail[static_cast<size_t>(i) + 3] == 0x06) {
            if (tryParseAt(tailStart + static_cast<uint32_t>(i))) return true;
        }
    }
    return false;
}

} // namespace

bool findFb2EntryInZip(IByteReader& zip, ZipEntryInfo& outEntry) {
    uint32_t cdOffset = 0, cdSize = 0;
    uint16_t numEntries = 0;
    if (!findEocd(zip, cdOffset, cdSize, numEntries)) return false;
    if (!zip.seek(cdOffset)) return false;

    for (uint16_t i = 0; i < numEntries; i++) {
        if (readU32(zip) != kCdSig) return false; // malformed / truncated central directory
        skip(zip, 2 + 2 + 2); // version made by, version needed, flags
        uint16_t method = readU16(zip);
        skip(zip, 2 + 2 + 4); // mod time, mod date, crc32
        uint32_t compSize = readU32(zip);
        uint32_t uncompSize = readU32(zip);
        uint16_t fnameLen = readU16(zip);
        uint16_t extraLen = readU16(zip);
        uint16_t commentLen = readU16(zip);
        skip(zip, 2 + 2 + 4); // disk#, internal attrs, external attrs
        uint32_t localOffset = readU32(zip);

        std::string filename(fnameLen, '\0');
        if (fnameLen > 0) {
            if (zip.read(reinterpret_cast<uint8_t*>(&filename[0]), fnameLen) != fnameLen) return false;
        }
        skip(zip, static_cast<uint32_t>(extraLen) + commentLen);

        if (endsWithFb2CaseInsensitive(filename)) {
            outEntry.filename = filename;
            outEntry.method = method;
            outEntry.compressedSize = compSize;
            outEntry.uncompressedSize = uncompSize;
            outEntry.localHeaderOffset = localOffset;
            return true;
        }
    }
    return false;
}

bool extractZipEntry(IByteReader& zip, const ZipEntryInfo& entry, const ZipOutputFn& out) {
    if (!zip.seek(entry.localHeaderOffset)) return false;
    if (readU32(zip) != kLocalSig) return false;
    skip(zip, 2 + 2 + 2 + 2 + 2 + 4 + 4 + 4); // verNeeded,flags,method,time,date,crc32,compSize,uncompSize
    uint16_t fnameLen = readU16(zip);
    uint16_t extraLen = readU16(zip);
    if (!zip.seek(zip.position() + fnameLen + extraLen)) return false; // now at compressed data

    if (entry.method == 0) {
        uint32_t remaining = entry.compressedSize;
        uint8_t chunk[512];
        while (remaining > 0) {
            size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
            size_t got = zip.read(chunk, want);
            if (got == 0) return false;
            out(chunk, got);
            remaining -= static_cast<uint32_t>(got);
        }
        return true;
    }
    if (entry.method == 8) {
        return inflateRaw(zip, entry.compressedSize, out);
    }
    return false; // unsupported compression method
}
