#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Serialization.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <cstring>

#include "Epub/BookMetadataCache.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char MEDIA_TYPE_IMAGE_PREFIX[] = "image/";
constexpr char itemCacheFile[] = "/.items.bin";
constexpr size_t SERIALIZED_COMPARE_CHUNK = 64;

enum class SerializedStringMatch : uint8_t { Error, No, Yes };

bool startsWithImageMediaType(const char* mediaType) {
  if (!mediaType) return false;
  constexpr size_t prefixLen = sizeof(MEDIA_TYPE_IMAGE_PREFIX) - 1;
  for (size_t i = 0; i < prefixLen; ++i) {
    if (mediaType[i] == '\0') return false;
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(mediaType[i])));
    if (c != MEDIA_TYPE_IMAGE_PREFIX[i]) return false;
  }
  return true;
}

bool isPropertyWhitespace(const char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

bool hasPropertyToken(const char* properties, const char* token) {
  if (!properties || !token || token[0] == '\0') return false;
  const size_t tokenLen = std::strlen(token);
  const char* pos = properties;
  while (*pos != '\0') {
    while (isPropertyWhitespace(*pos)) ++pos;
    const char* end = pos;
    while (*end != '\0' && !isPropertyWhitespace(*end)) ++end;
    if (static_cast<size_t>(end - pos) == tokenLen && std::memcmp(pos, token, tokenLen) == 0) return true;
    pos = end;
  }
  return false;
}

SerializedStringMatch serializedStringEquals(FsFile& file, const char* target, const size_t targetLen) {
  uint32_t storedLen = 0;
  if (!serialization::tryReadPod(file, storedLen)) {
    return SerializedStringMatch::Error;
  }

  if (storedLen != targetLen) {
    return file.seekCur(static_cast<int64_t>(storedLen)) ? SerializedStringMatch::No : SerializedStringMatch::Error;
  }

  uint8_t buffer[SERIALIZED_COMPARE_CHUNK];
  size_t offset = 0;
  bool equal = true;
  while (offset < storedLen) {
    const size_t chunk = std::min<size_t>(sizeof(buffer), storedLen - offset);
    if (file.read(buffer, chunk) != static_cast<int>(chunk)) {
      return SerializedStringMatch::Error;
    }
    if (equal && std::memcmp(buffer, target + offset, chunk) != 0) {
      equal = false;
    }
    offset += chunk;
  }
  return equal ? SerializedStringMatch::Yes : SerializedStringMatch::No;
}

bool skipSerializedString(FsFile& file) {
  uint32_t len = 0;
  return serialization::tryReadPod(file, len) && file.seekCur(static_cast<int64_t>(len));
}
}  // namespace

bool ContentOpfParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("COF", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

ContentOpfParser::~ContentOpfParser() {
  destroyXmlParser(parser);
  if (tempItemStore) {
    tempItemStore.close();
  }
  const auto itemCachePath = cachePath + itemCacheFile;
  if (Storage.exists(itemCachePath.c_str())) {
    Storage.remove(itemCachePath.c_str());
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    if (XML_Parse(parser, reinterpret_cast<const char*>(currentBufferPos), static_cast<int>(toRead),
                  remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("COF", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }

  return size;
}

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)atts;

  if (self->state == START && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:title") == 0) {
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
    }
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:creator") == 0) {
    self->state = IN_BOOK_AUTHOR;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:language") == 0) {
    self->state = IN_BOOK_LANGUAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_MANIFEST;
    if (!Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for writing. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_SPINE;
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }

    if (self->itemIndex.size() >= LARGE_SPINE_THRESHOLD) {
      std::sort(self->itemIndex.begin(), self->itemIndex.end(), [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
        return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
      });
      self->useItemIndex = true;
      LOG_DBG("COF", "Using fast index for %zu manifest items", self->itemIndex.size());
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_GUIDE;
    LOG_DBG("COF", "Entering guide state.");
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "meta") == 0 || strcmp(name, "opf:meta") == 0)) {
    bool isCover = false;
    const char* coverItemId = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0 && strcmp(atts[i + 1], "cover") == 0) {
        isCover = true;
      } else if (strcmp(atts[i], "content") == 0) {
        coverItemId = atts[i + 1];
      }
    }

    if (isCover && coverItemId) {
      self->coverItemId = coverItemId;
    }
    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "item") == 0 || strcmp(name, "opf:item") == 0)) {
    std::string_view itemId;
    std::string href;
    const char* mediaType = "";
    const char* properties = "";

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        itemId = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      } else if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "properties") == 0) {
        properties = atts[i + 1];
      }
    }

    if (self->tempItemStore) {
      ItemIndexEntry entry;
      entry.idHash = fnvHash(itemId);
      entry.idLen = static_cast<uint16_t>(itemId.size());
      entry.fileOffset = static_cast<uint32_t>(self->tempItemStore.position());
      self->itemIndex.push_back(entry);
    }

    serialization::writeString(self->tempItemStore, itemId);
    serialization::writeString(self->tempItemStore, href);

    const bool isCoverItem = itemId.size() == self->coverItemId.size() &&
                             std::memcmp(itemId.data(), self->coverItemId.data(), itemId.size()) == 0;
    if (isCoverItem) {
      if (startsWithImageMediaType(mediaType)) {
        self->coverItemHref = href;
      } else {
        LOG_DBG("COF", "Ignoring meta cover item '%.*s' with non-image media type: %s",
                static_cast<int>(itemId.size()), itemId.data(), mediaType);
      }
    }

    if (strcmp(mediaType, MEDIA_TYPE_NCX) == 0) {
      if (self->tocNcxPath.empty()) {
        self->tocNcxPath = href;
      } else {
        LOG_DBG("COF", "Warning: Multiple NCX files found in manifest. Ignoring duplicate: %s", href.c_str());
      }
    }

    if (strcmp(mediaType, MEDIA_TYPE_CSS) == 0) {
      self->cssFiles.push_back(href);
    }

    if (self->tocNavPath.empty() && hasPropertyToken(properties, "nav")) {
      self->tocNavPath = href;
      LOG_DBG("COF", "Found EPUB 3 nav document: %s", href.c_str());
    }

    if (self->coverItemHref.empty() && hasPropertyToken(properties, "cover-image")) {
      self->coverItemHref = href;
    }
    return;
  }

  if (self->cache) {
    if (self->state == IN_SPINE && (strcmp(name, "itemref") == 0 || strcmp(name, "opf:itemref") == 0)) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "idref") == 0) {
          const char* idref = atts[i + 1];
          const size_t targetLen = std::strlen(idref);
          const uint32_t targetHash = fnvHash(std::string_view{idref, targetLen});
          const uint16_t targetIndexLen = static_cast<uint16_t>(targetLen);

          std::string href;
          bool found = false;

          if (self->useItemIndex) {
            auto it = std::lower_bound(self->itemIndex.begin(), self->itemIndex.end(),
                                       ItemIndexEntry{targetHash, targetIndexLen, 0},
                                       [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
                                         return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
                                       });

            while (it != self->itemIndex.end() && it->idHash == targetHash) {
              self->tempItemStore.seek(it->fileOffset);
              const auto match = serializedStringEquals(self->tempItemStore, idref, targetLen);
              if (match == SerializedStringMatch::Error) {
                break;
              }
              if (match == SerializedStringMatch::Yes) {
                serialization::readString(self->tempItemStore, href);
                found = true;
                break;
              }
              ++it;
            }
          } else {
            self->tempItemStore.seek(0);
            while (self->tempItemStore.available()) {
              const auto match = serializedStringEquals(self->tempItemStore, idref, targetLen);
              if (match == SerializedStringMatch::Error) {
                break;
              }
              if (match == SerializedStringMatch::Yes) {
                serialization::readString(self->tempItemStore, href);
                found = true;
                break;
              }
              if (!skipSerializedString(self->tempItemStore)) {
                break;
              }
            }
          }

          if (found && self->cache) {
            self->cache->createSpineEntry(href);
          }
        }
      }
      return;
    }
  }

  if (self->state == IN_GUIDE && (strcmp(name, "reference") == 0 || strcmp(name, "opf:reference") == 0)) {
    const char* type = "";
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        guideHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      }
    }
    if (!guideHref.empty()) {
      if (strcmp(type, "text") == 0 || (strcmp(type, "start") == 0 && !self->textReferenceHref.empty())) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type, guideHref.c_str());
        self->textReferenceHref = guideHref;
      } else if ((strcmp(type, "cover") == 0 || strcmp(type, "cover-page") == 0) && self->guideCoverPageHref.empty()) {
        LOG_DBG("COF", "Found cover reference in guide: %s", guideHref.c_str());
        self->guideCoverPageHref = guideHref;
      }
    }
    return;
  }
}

void XMLCALL ContentOpfParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);

  if (self->state == IN_BOOK_TITLE) {
    self->title.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    if (!self->author.empty()) {
      self->author.append(", ");
    }
    self->author.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE) {
    self->language.append(s, len);
    return;
  }
}

void XMLCALL ContentOpfParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)name;

  if (self->state == IN_SPINE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_GUIDE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_BOOK_TITLE && strcmp(name, "dc:title") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && strcmp(name, "dc:creator") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && strcmp(name, "dc:language") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = START;
    return;
  }
}
