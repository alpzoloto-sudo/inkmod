// Fb2Parser.h
//
// High-level native FB2 reader for CrossPoint. Two-phase design, matching
// how the firmware already treats EPUB (metadata+TOC cached once to
// book.bin, section content decoded lazily and cached to sections/*.bin):
//
//   1. scan(reader, result)
//        One forward streaming pass over the whole file. Builds:
//          - Fb2Metadata          (title/author/lang/cover/annotation)
//          - flat section index   (byte offsets, titles, nesting)  -> "spine"
//          - binary index         (byte offsets of each base64 payload)
//        Peak RAM: a handful of small structs + whatever title/annotation
//        text is (a few KB at most). The file is never loaded whole.
//
//   2. renderSection(reader, sections[i], sink)
//        Seeks directly to a section's stored offset (no re-scan) and
//        streams just that section's content into an Fb2ContentSink, which
//        is where the existing page-layout code plugs in.
//
//   3. decodeBinary(reader, binaries[i], outFn)
//        Seeks to a <binary>'s payload and streams decoded bytes to a
//        callback (e.g. straight into the JPEG/PNG decoder that already
//        exists for EPUB cover/inline images) — never materializes the full
//        image in RAM as base64 text.
//
// This header has zero framework dependency; see FsFileReader.h for the
// ~15-line adapter around fs::File used on-device.

#pragma once
#include <functional>
#include "IByteReader.h"
#include "Fb2Types.h"

namespace reader {
class ReaderCancellationToken;
}

class Fb2Parser {
public:
    // Returns false only on a hard read error / clearly-not-FB2 input.
    // Missing individual metadata fields are not an error - FB2 files are
    // frequently missing <document-info>, <lang>, a cover, etc.
    bool scan(IByteReader& reader, Fb2ScanResult& outResult,
              size_t xmlBufferSize = 8192);

    // sectionIndex must be an index into the vector scan() filled in.
    bool renderSection(IByteReader& reader,
                        const Fb2SectionIndexEntry& section,
                        Fb2ContentSink& sink,
                        const reader::ReaderCancellationToken* cancellationToken = nullptr);

    using BinaryOutputFn = std::function<void(const uint8_t* data, size_t len)>;
    bool decodeBinary(IByteReader& reader,
                       const Fb2BinaryIndexEntry& binary,
                       const BinaryOutputFn& out,
                       const reader::ReaderCancellationToken* cancellationToken = nullptr);
};
