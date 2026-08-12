// Lazy format-neutral document interface; no renderer or storage dependency.
#pragma once

#include <cstdint>

#include "DocumentTypes.h"

namespace reader {

struct BookMetadata { StringRef title = {}; StringRef author = {}; StringRef language = {}; };
struct ChapterInfo {
  uint32_t id = 0;
  uint64_t approximateTextBytes = 0;
  StringRef title = {};
  // A fixed-size adapter buffer may not fit an unusually long chapter name.
  // UI code can append an ellipsis without guessing whether text was lost.
  bool titleTruncated = false;
};

// The metadata/chapter half of a document. Keeping it separate from
// DocumentModel lets library and progress code adopt the shared view before a
// format has a lazy content cursor. References remain valid until the next
// call on the same catalog, unless an implementation documents a longer life.
class DocumentCatalog {
 public:
  virtual ~DocumentCatalog() = default;
  virtual BookMetadata metadata() const = 0;
  virtual uint32_t chapterCount() const = 0;
  virtual bool chapterInfo(uint32_t chapter, ChapterInfo& out) const = 0;
};

class DocumentCursor {
 public:
  virtual ~DocumentCursor() = default;
  // Returned references expire when this cursor advances to another chunk.
  virtual bool next(DocumentNode& out) = 0;
  // Moves to a parser-defined logical node boundary. The reader only asks for
  // offsets it obtained from this cursor, so an adapter never has to guess a
  // byte position in compressed source data.
  virtual bool seek(uint64_t byteOffset) = 0;
  virtual uint64_t byteOffset() const = 0;
};

class DocumentModel : public DocumentCatalog {
 public:
  virtual ~DocumentModel() = default;
  // Cursor is supplied by caller, avoiding opaque heap-owned iterators.
  virtual bool openChapter(uint32_t chapter, DocumentCursor& cursor) = 0;
};

}  // namespace reader
