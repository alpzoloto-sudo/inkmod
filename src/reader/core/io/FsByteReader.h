// SD adapter with no ownership and no allocation. The caller closes HalFile.
#pragma once

#include <HalStorage.h>

#include "IByteReader.h"

class FsByteReader final : public IByteReader {
 public:
  explicit FsByteReader(HalFile& file) : file_(file) {}

  size_t read(void* dst, size_t len) override {
    const int got = file_.read(dst, len);
    return got > 0 ? static_cast<size_t>(got) : 0;
  }
  bool seek(uint64_t pos) override { return file_.seek64(pos); }
  uint64_t tell() const override { return file_.position(); }
  uint64_t size() const override { return file_.fileSize64(); }

 private:
  HalFile& file_;
};

