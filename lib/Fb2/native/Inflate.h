// Inflate.h
//
// Native, dependency-free raw DEFLATE (RFC 1951) decompressor. Needed
// because .fb2 files are very commonly distributed as .fb2.zip (a single
// compressed entry), and this module deliberately has zero external
// library dependencies — same rationale as Fb2XmlReader not linking
// libxml2/pugixml.
//
// Design: pull-based, not push/feed-based. inflateRaw() takes an
// IByteReader already positioned at the start of a raw DEFLATE stream and
// pulls compressed bytes from it itself (via a small internal buffer) as
// needed, for up to `compressedLimit` bytes, writing decompressed output to
// `out` in bounded chunks. This avoids the considerable complexity of a
// decompressor that can suspend and resume mid-Huffman-symbol across
// caller-controlled feed() calls — since the input here is a seekable
// on-disk file, pulling on demand is both simpler and just as
// memory-bounded as feeding chunks in would be.
//
// RAM cost: one 32KB sliding window (mandated by the DEFLATE format itself
// — match distances run up to 32768 bytes back, there's no way around
// having *some* buffer that large for a general-purpose inflater) plus a
// small (~1KB) input buffer and a few hundred bytes of Huffman decode
// tables. This is real memory on a 380KB device, which is exactly why the
// intended use (see ZipReader.h) is a one-time extraction of the whole
// .fb2 out to a plain cache file on SD — the window is allocated on the
// stack/heap only for the duration of that one call and freed immediately
// after, never held during normal reading.

#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include "IByteReader.h"

using InflateOutputFn = std::function<void(const uint8_t* data, size_t len)>;

// Returns false on a malformed/truncated/unsupported (e.g. BTYPE==3
// reserved) DEFLATE stream. On success, exactly the decompressed bytes for
// one raw DEFLATE stream (ending at its BFINAL=1 block) have been pushed to
// `out`; `in`'s read position afterward is wherever it happens to land
// (implementation reads in fixed-size chunks, not necessarily exactly
// `compressedLimit` bytes) — callers that need the exact end-of-stream
// position should track it via the ZIP entry's compressedSize instead of
// relying on `in.position()` after this call.
bool inflateRaw(IByteReader& in, uint32_t compressedLimit, const InflateOutputFn& out);
