// Small streaming XHTML cursor for the new EPUB path. It intentionally keeps
// no DOM: output text is held in one reusable bounded buffer until next().
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "DocumentModel.h"
#include "reader/core/io/IByteReader.h"

namespace reader {

class XhtmlTextCursor final : public DocumentCursor {
 public:
  // Activity-lifetime scratch, not task-stack data. Cached EPUB chapters are
  // consumed sequentially from SD, so a 2 KiB input window halves storage
  // transactions versus the previous 1 KiB window while adding only 1 KiB of
  // bounded cursor-lifetime RAM. This stays deliberately below the 4 KiB
  // temporary buffers used by chapter copy / ZIP streaming paths.
  static constexpr size_t kTextCapacity = 384;
  static constexpr size_t kInputCapacity = 2048;
  static constexpr size_t kTagCapacity = 64;

  explicit XhtmlTextCursor(IByteReader& source) : source_(source) {}

  bool next(DocumentNode& out) override;
  bool seek(uint64_t byteOffset) override;
  uint64_t byteOffset() const override { return logicalOffset_; }

 private:
  bool readChar(char& out);
  bool readTag();
  void handleTag();
  void appendText(char value);
  bool emitText(DocumentNode& out) const;
  static bool isSpace(char value);
  static bool tagNameEquals(const char* tag, size_t len, const char* expected);
  static uint32_t imageResourceId(const char* tag, size_t len);

  IByteReader& source_;
  std::array<uint8_t, kInputCapacity> input_ = {};
  std::array<char, kTextCapacity> text_ = {};
  std::array<char, kTagCapacity> tag_ = {};
  size_t inputPos_ = 0;
  size_t inputSize_ = 0;
  size_t textSize_ = 0;
  size_t tagSize_ = 0;
  uint64_t logicalOffset_ = 0;
  NodeType activeType_ = NodeType::Paragraph;
  TextStyle activeStyle_ = {};
  NodeType textType_ = NodeType::Paragraph;
  TextStyle textStyle_ = {};
  ResourceId pendingImage_ = {};
  bool imagePending_ = false;
  bool pageBreakPending_ = false;
  bool blockBoundaryPending_ = false;
  bool lastWasSpace_ = true;
};

}  // namespace reader
