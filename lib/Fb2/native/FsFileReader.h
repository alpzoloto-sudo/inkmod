// Compatibility adapter for the legacy/native FB2 parser.
//
// Large scanner reads pass directly to HalFile. Small sequential reads used by
// lazy image decode and section rendering are coalesced by BufferedByteReader
// into bounded 4 KiB storage transactions.
#pragma once

#include "../../../src/reader/core/io/BufferedByteReader.h"
#include "../../../src/reader/core/io/FsByteReader.h"

class FsFileReader final : public IByteReader {
 public:
  explicit FsFileReader(HalFile& file) : source_(file), buffered_(source_) {}

  size_t read(void* dst, size_t len) override { return buffered_.read(dst, len); }
  bool seek(uint64_t pos) override { return buffered_.seek(pos); }
  uint64_t tell() const override { return buffered_.tell(); }
  uint64_t size() const override { return buffered_.size(); }

 private:
  FsByteReader source_;
  BufferedByteReader buffered_;
};
