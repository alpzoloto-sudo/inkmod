#include "DictionaryStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Dictionary {
namespace {

constexpr char INDEX_FILE_NAME[] = "dictionary.idx";
constexpr char DATA_FILE_NAME[] = "dictionary.dat";
constexpr uint16_t FORMAT_VERSION_V1 = 1;
constexpr uint16_t FORMAT_VERSION_V2 = 2;
constexpr uint16_t INDEX_RECORD_SIZE_V1 = 12;
constexpr uint16_t INDEX_RECORD_SIZE_V2 = 16;
constexpr uint32_t INDEX_HEADER_SIZE = 16;
constexpr uint32_t DATA_HEADER_SIZE = 12;
constexpr uint8_t MAX_HASH_CANDIDATES = 8;
constexpr uint32_t FNV_OFFSET_BASIS = 2166136261UL;
constexpr uint32_t FNV_PRIME = 16777619UL;

struct IndexRecord {
  uint32_t hash = 0;
  uint32_t secondaryHash = 0;
  uint32_t dataOffset = 0;
  uint32_t dataLength = 0;
};

bool readExact(FsFile& file, void* out, const size_t size) {
  return size == 0 || file.read(out, size) == static_cast<int>(size);
}

bool readU16Le(FsFile& file, uint16_t& out) {
  uint8_t bytes[2];
  if (!readExact(file, bytes, sizeof(bytes))) return false;
  out = static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
  return true;
}

bool readU32Le(FsFile& file, uint32_t& out) {
  uint8_t bytes[4];
  if (!readExact(file, bytes, sizeof(bytes))) return false;
  out = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
  return true;
}

bool validateIndexHeader(FsFile& file, uint32_t& entryCount, uint16_t& version, uint16_t& recordSize) {
  char magic[4];
  version = 0;
  recordSize = 0;
  uint32_t flags = 0;
  if (!readExact(file, magic, sizeof(magic)) || !readU16Le(file, version) || !readU16Le(file, recordSize) ||
      !readU32Le(file, entryCount) || !readU32Le(file, flags)) {
    return false;
  }
  (void)flags;
  const bool supported = (version == FORMAT_VERSION_V1 && recordSize == INDEX_RECORD_SIZE_V1) ||
                         (version == FORMAT_VERSION_V2 && recordSize == INDEX_RECORD_SIZE_V2);
  if (memcmp(magic, "IMDX", sizeof(magic)) != 0 || !supported) {
    return false;
  }
  const uint64_t required = static_cast<uint64_t>(INDEX_HEADER_SIZE) +
                            static_cast<uint64_t>(entryCount) * static_cast<uint64_t>(recordSize);
  return required <= file.fileSize64();
}

bool validateDataHeader(FsFile& file, uint32_t& entryCount, uint16_t& version) {
  char magic[4];
  version = 0;
  uint16_t reserved = 0;
  if (!readExact(file, magic, sizeof(magic)) || !readU16Le(file, version) || !readU16Le(file, reserved) ||
      !readU32Le(file, entryCount)) {
    return false;
  }
  return memcmp(magic, "IMDD", sizeof(magic)) == 0 &&
         (version == FORMAT_VERSION_V1 || version == FORMAT_VERSION_V2) && reserved == 0 &&
         file.fileSize64() >= DATA_HEADER_SIZE;
}

bool readIndexRecord(FsFile& file, const uint32_t index, const uint16_t recordSize, IndexRecord& record) {
  const uint64_t offset = static_cast<uint64_t>(INDEX_HEADER_SIZE) +
                          static_cast<uint64_t>(index) * static_cast<uint64_t>(recordSize);
  if (offset > SIZE_MAX || !file.seek(static_cast<size_t>(offset))) return false;
  record.secondaryHash = 0;
  if (!readU32Le(file, record.hash)) return false;
  if (recordSize == INDEX_RECORD_SIZE_V2 && !readU32Le(file, record.secondaryHash)) return false;
  return readU32Le(file, record.dataOffset) && readU32Le(file, record.dataLength);
}

uint32_t hashNormalized(const char* normalized) {
  uint32_t hash = FNV_OFFSET_BASIS;
  for (const auto* p = reinterpret_cast<const uint8_t*>(normalized); *p; ++p) {
    hash ^= *p;
    hash *= FNV_PRIME;
  }
  return hash;
}

uint32_t secondaryHashNormalized(const char* normalized) {
  uint32_t hash = 5381UL;
  for (const auto* p = reinterpret_cast<const uint8_t*>(normalized); *p; ++p) {
    hash = (hash * 33UL) ^ *p;
  }
  return hash;
}

bool decodeUtf8(const uint8_t*& cursor, uint32_t& cp) {
  const uint8_t first = *cursor++;
  if (first < 0x80) {
    cp = first;
    return true;
  }
  uint8_t continuationCount = 0;
  if ((first & 0xE0) == 0xC0) {
    cp = first & 0x1F;
    continuationCount = 1;
  } else if ((first & 0xF0) == 0xE0) {
    cp = first & 0x0F;
    continuationCount = 2;
  } else if ((first & 0xF8) == 0xF0) {
    cp = first & 0x07;
    continuationCount = 3;
  } else {
    cp = 0xFFFD;
    return false;
  }
  for (uint8_t i = 0; i < continuationCount; ++i) {
    const uint8_t next = *cursor;
    if (next == 0 || (next & 0xC0) != 0x80) {
      cp = 0xFFFD;
      return false;
    }
    ++cursor;
    cp = (cp << 6) | (next & 0x3F);
  }
  return true;
}

uint32_t foldCodepoint(uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
  if ((cp >= 0x00C0 && cp <= 0x00D6) || (cp >= 0x00D8 && cp <= 0x00DE)) return cp + 0x20;
  if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;
  switch (cp) {
    case 0x0401:
      return 0x0451;  // Ё
    case 0x0404:
      return 0x0454;  // Є
    case 0x0406:
      return 0x0456;  // І
    case 0x0407:
      return 0x0457;  // Ї
    case 0x0490:
      return 0x0491;  // Ґ
    default:
      return cp;
  }
}

bool isLookupCodepoint(const uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 0x00C0 && cp <= 0x02AF) ||
         (cp >= 0x0400 && cp <= 0x052F);
}

bool isWhitespace(const uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0x00A0 || cp == 0x202F;
}

size_t appendUtf8(const uint32_t cp, char* output, const size_t outputSize, const size_t offset) {
  uint8_t encoded[4];
  size_t count = 0;
  if (cp <= 0x7F) {
    encoded[count++] = static_cast<uint8_t>(cp);
  } else if (cp <= 0x7FF) {
    encoded[count++] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    encoded[count++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    encoded[count++] = static_cast<uint8_t>(0xE0 | (cp >> 12));
    encoded[count++] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    encoded[count++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  } else {
    encoded[count++] = static_cast<uint8_t>(0xF0 | (cp >> 18));
    encoded[count++] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
    encoded[count++] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    encoded[count++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  }
  if (offset + count >= outputSize) return offset;
  memcpy(output + offset, encoded, count);
  return offset + count;
}

}  // namespace

const CatalogEntry* Store::entry(const uint8_t index) const { return index < count_ ? &entries_[index] : nullptr; }

size_t Store::normalizeWord(const char* input, char* output, const size_t outputSize) {
  if (!output || outputSize == 0) return 0;
  output[0] = '\0';
  if (!input) return 0;

  const auto* cursor = reinterpret_cast<const uint8_t*>(input);
  size_t length = 0;
  size_t lastLookupEnd = 0;
  bool started = false;
  bool pendingSpace = false;

  while (*cursor) {
    uint32_t cp = 0;
    decodeUtf8(cursor, cp);
    cp = foldCodepoint(cp);

    if (isWhitespace(cp)) {
      pendingSpace = started;
      continue;
    }

    const bool isLookup = isLookupCodepoint(cp);
    const bool isJoiner = started && (cp == '-' || cp == '\'' || cp == 0x2019);
    if (!isLookup && !isJoiner) continue;

    if (pendingSpace && isLookup) {
      if (length + 1 >= outputSize) break;
      output[length++] = ' ';
      pendingSpace = false;
    }

    const size_t nextLength = appendUtf8(cp, output, outputSize, length);
    if (nextLength == length) break;
    length = nextLength;
    started = true;
    if (isLookup) lastLookupEnd = length;
  }

  length = lastLookupEnd;
  output[length] = '\0';
  return length;
}

bool Store::scan() {
  count_ = 0;
  const char* roots[] = {ROOT_PATH, LEGACY_ROOT_PATH};
  char folderName[MAX_DICTIONARY_NAME_BYTES];
  for (const char* rootPath : roots) {
    FsFile root = Storage.open(rootPath);
    if (!root || !root.isDirectory()) {
      root.close();
      LOG_DBG("DICT", "Dictionary root not found: %s", rootPath);
      continue;
    }

    while (count_ < MAX_DICTIONARIES) {
      FsFile item = root.openNextFile();
      if (!item) break;
      if (!item.isDirectory()) {
        item.close();
        continue;
      }
      item.getName(folderName, sizeof(folderName));
      item.close();
      if (folderName[0] == '\0' || folderName[0] == '.' || folderName[0] == '_') continue;

      bool duplicate = false;
      for (uint8_t i = 0; i < count_; ++i) {
        if (strcmp(entries_[i].name, folderName) == 0) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) continue;

      CatalogEntry candidate;
      snprintf(candidate.name, sizeof(candidate.name), "%s", folderName);
      snprintf(candidate.path, sizeof(candidate.path), "%s/%s", rootPath, folderName);

      char indexPath[MAX_DICTIONARY_PATH_BYTES + 32];
      char dataPath[MAX_DICTIONARY_PATH_BYTES + 32];
      snprintf(indexPath, sizeof(indexPath), "%s/%s", candidate.path, INDEX_FILE_NAME);
      snprintf(dataPath, sizeof(dataPath), "%s/%s", candidate.path, DATA_FILE_NAME);
      if (!Storage.exists(indexPath) || !Storage.exists(dataPath)) continue;

      FsFile indexFile;
      if (!Storage.openFileForRead("DICT", indexPath, indexFile)) continue;
      uint16_t indexVersion = 0;
      uint16_t indexRecordSize = 0;
      const bool valid = validateIndexHeader(indexFile, candidate.entryCount, indexVersion, indexRecordSize);
      indexFile.close();
      if (!valid || candidate.entryCount == 0) {
        LOG_ERR("DICT", "Ignoring invalid prepared dictionary: %s", candidate.path);
        continue;
      }

      FsFile dataFile;
      if (!Storage.openFileForRead("DICT", dataPath, dataFile)) continue;
      uint32_t dataEntryCount = 0;
      uint16_t dataVersion = 0;
      const bool dataValid = validateDataHeader(dataFile, dataEntryCount, dataVersion);
      dataFile.close();
      if (!dataValid || dataVersion != indexVersion || dataEntryCount != candidate.entryCount) {
        LOG_ERR("DICT", "Ignoring dictionary with mismatched data: %s", candidate.path);
        continue;
      }
      entries_[count_++] = candidate;
    }
    root.close();
    if (count_ >= MAX_DICTIONARIES) break;
  }
  LOG_INF("DICT", "Prepared dictionaries found: %u", count_);
  return count_ > 0;
}

bool Store::lookup(const uint8_t dictionaryIndex, const char* word, char* matchedWord, const size_t matchedWordSize,
                   char* article, const size_t articleSize) const {
  if (matchedWord && matchedWordSize > 0) matchedWord[0] = '\0';
  if (article && articleSize > 0) article[0] = '\0';
  if (dictionaryIndex >= count_ || !word || !matchedWord || matchedWordSize < 2 || !article || articleSize < 2) {
    return false;
  }

  char normalized[128];
  if (normalizeWord(word, normalized, sizeof(normalized)) == 0) return false;
  const uint32_t wantedHash = hashNormalized(normalized);
  const uint32_t wantedSecondaryHash = secondaryHashNormalized(normalized);

  char indexPath[MAX_DICTIONARY_PATH_BYTES + 32];
  snprintf(indexPath, sizeof(indexPath), "%s/%s", entries_[dictionaryIndex].path, INDEX_FILE_NAME);
  FsFile indexFile;
  if (!Storage.openFileForRead("DICT", indexPath, indexFile)) return false;

  uint32_t entryCount = 0;
  uint16_t indexVersion = 0;
  uint16_t indexRecordSize = 0;
  if (!validateIndexHeader(indexFile, entryCount, indexVersion, indexRecordSize)) {
    indexFile.close();
    LOG_ERR("DICT", "Invalid dictionary index: %s", indexPath);
    return false;
  }

  uint32_t low = 0;
  uint32_t high = entryCount;
  IndexRecord record;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    if (!readIndexRecord(indexFile, mid, indexRecordSize, record)) {
      indexFile.close();
      return false;
    }
    if (record.hash < wantedHash) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }

  IndexRecord candidates[MAX_HASH_CANDIDATES] = {};
  uint8_t candidateCount = 0;
  if (indexVersion == FORMAT_VERSION_V2) {
    for (uint32_t i = low; i < entryCount; ++i) {
      if (!readIndexRecord(indexFile, i, indexRecordSize, record) || record.hash != wantedHash) break;
      if (record.secondaryHash == wantedSecondaryHash) {
        candidates[candidateCount++] = record;
        break;
      }
    }
  } else {
    for (uint32_t i = low; i < entryCount && candidateCount < MAX_HASH_CANDIDATES; ++i) {
      if (!readIndexRecord(indexFile, i, indexRecordSize, record)) break;
      if (record.hash != wantedHash) break;
      candidates[candidateCount++] = record;
    }
  }
  indexFile.close();
  if (candidateCount == 0) return false;

  char dataPath[MAX_DICTIONARY_PATH_BYTES + 32];
  snprintf(dataPath, sizeof(dataPath), "%s/%s", entries_[dictionaryIndex].path, DATA_FILE_NAME);
  FsFile dataFile;
  if (!Storage.openFileForRead("DICT", dataPath, dataFile)) return false;

  uint32_t dataEntryCount = 0;
  uint16_t dataVersion = 0;
  if (!validateDataHeader(dataFile, dataEntryCount, dataVersion) || dataVersion != indexVersion ||
      dataEntryCount != entryCount) {
    dataFile.close();
    LOG_ERR("DICT", "Dictionary data/index mismatch: %s", entries_[dictionaryIndex].path);
    return false;
  }

  char candidateWord[96];
  char candidateNormalized[128];
  for (uint8_t i = 0; i < candidateCount; ++i) {
    const auto& candidate = candidates[i];
    const uint64_t entryEnd = static_cast<uint64_t>(candidate.dataOffset) + candidate.dataLength;
    if (candidate.dataOffset < DATA_HEADER_SIZE || entryEnd > dataFile.fileSize64() || candidate.dataLength < 6 ||
        !dataFile.seek(candidate.dataOffset)) {
      continue;
    }

    uint16_t headwordLength = 0;
    uint32_t articleLength = 0;
    if (!readU16Le(dataFile, headwordLength) || !readU32Le(dataFile, articleLength) ||
        static_cast<uint64_t>(6) + headwordLength + articleLength > candidate.dataLength ||
        headwordLength == 0 || headwordLength >= sizeof(candidateWord)) {
      continue;
    }
    if (!readExact(dataFile, candidateWord, headwordLength)) continue;
    candidateWord[headwordLength] = '\0';
    if (indexVersion == FORMAT_VERSION_V1) {
      normalizeWord(candidateWord, candidateNormalized, sizeof(candidateNormalized));
      if (strcmp(candidateNormalized, normalized) != 0) continue;
    }

    snprintf(matchedWord, matchedWordSize, "%s", candidateWord);
    const size_t bytesToRead = std::min<size_t>(articleLength, articleSize - 1);
    if (!readExact(dataFile, article, bytesToRead)) {
      dataFile.close();
      article[0] = '\0';
      return false;
    }
    article[bytesToRead] = '\0';
    dataFile.close();
    return bytesToRead > 0;
  }

  dataFile.close();
  return false;
}

}  // namespace Dictionary
