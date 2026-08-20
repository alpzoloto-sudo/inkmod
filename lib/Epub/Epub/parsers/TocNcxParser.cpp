#include "TocNcxParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <XmlParserUtils.h>

#include <string_view>

#include "Epub/BookMetadataCache.h"

bool TocNcxParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("TOC", "Couldn't allocate memory for parser");
    return false;
  }

  currentLabel.reserve(96);
  currentSrc.reserve(128);
  currentTarget.reserve(baseContentPath.size() + 128);
  currentAnchor.reserve(48);

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

TocNcxParser::~TocNcxParser() { destroyXmlParser(parser); }

size_t TocNcxParser::write(const uint8_t data) { return write(&data, 1); }

size_t TocNcxParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    if (XML_Parse(parser, reinterpret_cast<const char*>(currentBufferPos), static_cast<int>(toRead),
                  remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("TOC", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
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

void XMLCALL TocNcxParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<TocNcxParser*>(userData);

  if (self->state == START && strcmp(name, "ncx") == 0) {
    self->state = IN_NCX;
    return;
  }

  if (self->state == IN_NCX && strcmp(name, "navMap") == 0) {
    self->state = IN_NAV_MAP;
    return;
  }

  if ((self->state == IN_NAV_MAP || self->state == IN_NAV_POINT) && strcmp(name, "navPoint") == 0) {
    self->state = IN_NAV_POINT;
    self->currentDepth++;
    self->currentLabel.clear();
    self->currentSrc.clear();
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "navLabel") == 0) {
    self->state = IN_NAV_LABEL;
    return;
  }

  if (self->state == IN_NAV_LABEL && strcmp(name, "text") == 0) {
    self->state = IN_NAV_LABEL_TEXT;
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "content") == 0) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "src") == 0) {
        self->currentSrc = atts[i + 1];
        break;
      }
    }
    return;
  }
}

void XMLCALL TocNcxParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<TocNcxParser*>(userData);
  if (self->state == IN_NAV_LABEL_TEXT) {
    self->currentLabel.append(s, len);
  }
}

void XMLCALL TocNcxParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<TocNcxParser*>(userData);

  if (self->state == IN_NAV_LABEL_TEXT && strcmp(name, "text") == 0) {
    self->state = IN_NAV_LABEL;
    return;
  }

  if (self->state == IN_NAV_LABEL && strcmp(name, "navLabel") == 0) {
    self->state = IN_NAV_POINT;
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "navPoint") == 0) {
    self->currentDepth--;
    if (self->currentDepth == 0) {
      self->state = IN_NAV_MAP;
    }
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "content") == 0) {
    if (!self->currentLabel.empty() && !self->currentSrc.empty()) {
      self->currentTarget.clear();
      self->currentTarget.append(self->baseContentPath);
      self->currentTarget.append(self->currentSrc);

      const size_t pos = self->currentTarget.find('#');
      self->currentAnchor.clear();
      if (pos != std::string::npos) {
        self->currentAnchor = FsHelpers::decodeUriEscapes(std::string_view{self->currentTarget}.substr(pos + 1));
        self->currentTarget.resize(pos);
      }
      std::string href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->currentTarget));

      if (self->cache) {
        self->cache->createTocEntry(self->currentLabel, href, self->currentAnchor, self->currentDepth);
      }

      self->currentLabel.clear();
      self->currentSrc.clear();
    }
  }
}