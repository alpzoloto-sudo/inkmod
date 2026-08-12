// Reader Core — allocation-free byte-stream contract.
//
// This is intentionally the same small contract that the legacy FB2 parser
// already used. Moving the canonical definition here lets EPUB, FB2 and cache
// adapters share it later without giving parsers access to HalStorage or UI.
// Implementations are pass-through wrappers around an already-open source;
// they must not allocate or retain a second copy of a book in RAM.

#pragma once

#include <cstddef>
#include <cstdint>

class IByteReader {
 public:
  virtual ~IByteReader() = default;

  // Reads at most len bytes. Returns 0 at EOF or on a recoverable read error.
  // The caller owns buf and may use a small reusable chunk buffer.
  virtual size_t read(void* buf, size_t len) = 0;

  // Absolute seek from the start of the stream. A streaming implementation may
  // return false; callers must then choose an extract-to-cache path instead of
  // buffering the complete resource in RAM.
  virtual bool seek(uint64_t pos) = 0;

  virtual uint64_t tell() const = 0;
  virtual uint64_t size() const = 0;

  // Legacy FB2 code used position(). Keep this spelling during the transition.
  uint64_t position() const { return tell(); }
  bool eof() const { return tell() >= size(); }
};
