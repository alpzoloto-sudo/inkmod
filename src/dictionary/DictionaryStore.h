#pragma once

#include <cstddef>
#include <cstdint>

namespace Dictionary {

constexpr char ROOT_PATH[] = "/Dictionaries";
constexpr uint8_t MAX_DICTIONARIES = 8;
constexpr size_t MAX_DICTIONARY_NAME_BYTES = 64;
constexpr size_t MAX_DICTIONARY_PATH_BYTES = 192;

struct CatalogEntry {
  char name[MAX_DICTIONARY_NAME_BYTES] = {};
  char path[MAX_DICTIONARY_PATH_BYTES] = {};
  uint32_t entryCount = 0;
};

// Reads browser-prepared InkMOD dictionaries. The on-device side never
// decompresses or builds an index: a lookup is a binary search over fixed
// fixed-size records followed by one bounded article read from the data file.
// Format v2 uses a second hash so browser-prepared StarDict synonyms can point
// directly to the canonical article without duplicating its text on the SD card.
class Store {
 public:
  bool scan();
  uint8_t count() const { return count_; }
  const CatalogEntry* entry(uint8_t index) const;

  bool lookup(uint8_t dictionaryIndex, const char* word, char* matchedWord, size_t matchedWordSize, char* article,
              size_t articleSize) const;

  static size_t normalizeWord(const char* input, char* output, size_t outputSize);

 private:
  CatalogEntry entries_[MAX_DICTIONARIES] = {};
  uint8_t count_ = 0;
};

}  // namespace Dictionary
