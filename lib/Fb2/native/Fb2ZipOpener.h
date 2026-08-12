// Fb2ZipOpener.h
//
// Ties ZipReader + Inflate together into the one operation callers actually
// need: "this path is an .fb2.zip, give me the decompressed .fb2 bytes."
//
// Deliberately does NOT try to make Fb2Parser itself zip-aware (i.e. no
// seekable-decompression layer plugged in as an IByteReader). Two reasons:
//   1. Fb2Parser::renderSection()/decodeBinary() need real random seeks
//      (jump straight to a stored byte offset), which a streaming DEFLATE
//      decompressor fundamentally can't do cheaply — you'd have to
//      re-decompress from the start of the entry every time.
//   2. CrossPoint already caches parsed per-book data to SD (book.bin,
///     sections/*.bin — see docs/file-formats.md); decompressing the whole
//      .fb2 ONCE into a plain cache file follows that same pattern and
//      then every later scan()/renderSection()/decodeBinary() call is a
//      normal cheap SD seek, exactly like a non-zipped .fb2.
//
// Usage on-device (mirrors how EPUB's own zip contents presumably already
// get unpacked to the .crosspoint/epub_<hash>/ cache — same idea, applied
// per file-formats.md's existing "cache once, reuse forever" convention):
//
//     File zipFile = SD.open(path, FILE_READ);
//     FsFileReader zipReader(zipFile);
//
//     File cacheOut = SD.open(cachePath, FILE_WRITE);
//     bool ok = extractFb2FromZip(zipReader, [&](const uint8_t* d, size_t n) {
//         cacheOut.write(d, n);
//     });
//     cacheOut.close();
//     zipFile.close();
//     if (!ok) { /* not a zip, or no .fb2 entry inside, or unsupported
//                    compression method */ }
//
//     // From here on, treat cachePath exactly like a plain .fb2:
//     File fb2File = SD.open(cachePath, FILE_READ);
//     FsFileReader reader(fb2File);
//     Fb2Parser parser;
//     Fb2ScanResult scan;
//     parser.scan(reader, scan);

#pragma once
#include "ZipReader.h"

// Extracts the single .fb2 entry from a .fb2.zip, streaming decompressed
// bytes to `out` in bounded chunks (never holds the whole file — compressed
// or decompressed — in RAM; see Inflate.h for the one real exception, a
// transient 32KB back-reference window freed when this call returns).
//
// Returns false if `zipFile` isn't a valid ZIP, contains no *.fb2 entry, or
// uses a compression method other than stored/DEFLATE (both effectively
// universal for real-world .fb2.zip; anything else is not worth a
// dependency to support).
inline bool extractFb2FromZip(IByteReader& zipFile, const ZipOutputFn& out,
                               std::string* outEntryName = nullptr) {
    ZipEntryInfo entry;
    if (!findFb2EntryInZip(zipFile, entry)) return false;
    if (outEntryName) *outEntryName = entry.filename;
    return extractZipEntry(zipFile, entry, out);
}
