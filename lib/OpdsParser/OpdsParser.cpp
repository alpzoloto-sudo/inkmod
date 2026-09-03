#include "OpdsParser.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cstring>
#include <utility>

OpdsParser::OpdsParser(OpdsEntry* entries, const size_t entryCapacity)
    : entries(entries), entryCapacity(entryCapacity) {
  if (!entries || entryCapacity == 0) {
    errorOccured = true;
    errorReason = OpdsParserError::NO_ENTRY_BUFFER;
    LOG_DBG("OPDS", "No entry buffer supplied");
  }

  resetXmlParser();
}


OpdsParser::OpdsParser(EntrySink sink, void* sinkContext)
    : entrySink(sink), sinkContext(sinkContext) {
  if (!entrySink) {
    errorOccured = true;
    errorReason = OpdsParserError::NO_ENTRY_BUFFER;
  }
  resetXmlParser();
}
OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  // Once the fixed entry buffer is full we already have everything that can
  // be displayed. Do not keep driving Expat through the rest of a huge feed:
  // its internal allocations while TLS is alive are enough to panic an X4.
  if (saturated) return length;

  if (errorOccured || !parser) {
    errorOccured = true;
    return length;
  }
  if (!xmlData && length > 0) {
    errorOccured = true;
    errorReason = OpdsParserError::INVALID_INPUT;
    return length;
  }

  // Feed chunks arrive from HttpDownloader already.  Feeding those bytes
  // directly to Expat avoids XML_GetBuffer()/memcpy() allocating a second
  // parser input buffer.  On the ESP32-C3 that extra allocation is enough to
  // fail on TLS-heavy OPDS feeds when the heap is fragmented (Flibusta's
  // "new books" feed is a common example).
  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 512;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    if (XML_Parse(parser, currentPos, static_cast<int>(toRead), XML_FALSE) == XML_STATUS_ERROR) {
      errorOccured = true;
      errorReason = OpdsParserError::XML_PARSE;
      LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      parser = nullptr;
      // Signal the Stream/HTTP layer immediately.  This stops the TLS
      // transfer instead of continuing to download a feed that can no longer
      // be parsed, which is especially important on the C3.
      return 0;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (saturated) {
    // The XML document is intentionally only partially parsed. Destroy Expat
    // without asking it to validate an artificial EOF.
    destroyXmlParser(parser);
    parser = nullptr;
    return;
  }
  if (!parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    errorReason = OpdsParserError::XML_PARSE;
    destroyXmlParser(parser);
    parser = nullptr;
  }
}

bool OpdsParser::parse(const uint8_t* xmlData, const size_t length) {
  clear();
  if (!xmlData && length > 0) {
    errorOccured = true;
    errorReason = OpdsParserError::INVALID_INPUT;
    return false;
  }

  if (length > 0) {
    write(xmlData, length);
  }
  flush();
  return !error();
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entryCount = 0;
  truncated = false;
  searchTemplate.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = inSummary = inContent = false;
  discardCurrentEntry = false;
  saturated = false;
  errorOccured = entrySink ? false : (!entries || entryCapacity == 0);
  errorReason = errorOccured ? OpdsParserError::NO_ENTRY_BUFFER : OpdsParserError::NONE;
  resetXmlParser();
}

bool OpdsParser::resetXmlParser() {
  if (parser) {
    if (XML_ParserReset(parser, nullptr) != XML_TRUE) {
      destroyXmlParser(parser);
    }
  }

  if (!parser) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      errorOccured = true;
      errorReason = OpdsParserError::PARSER_MEMORY;
      LOG_DBG("OPDS", "Couldn't allocate memory for parser");
      return false;
    }
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

namespace {
bool containsIgnoreCase(const char* haystack, const char* needle) {
  if (!haystack || !needle || !*needle) return false;
  for (const char* h = haystack; *h; ++h) {
    const char* a = h;
    const char* b = needle;
    while (*a && *b) {
      char ca = *a;
      char cb = *b;
      if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
      if (ca != cb) break;
      ++a;
      ++b;
    }
    if (!*b) return true;
  }
  return false;
}

bool classifyAcquisitionType(const char* type, OpdsBookFormat& format) {
  if (!type) return false;

  // Be liberal with legacy OPDS catalogs: MIME parameters, case variants and
  // historical spellings are all seen in the wild.
  if (containsIgnoreCase(type, "epub+zip") || containsIgnoreCase(type, "application/epub")) {
    format = OpdsBookFormat::EPUB;
    return true;
  }
  if (containsIgnoreCase(type, "fb2+zip")) {
    format = OpdsBookFormat::FB2_ZIP;
    return true;
  }
  if (containsIgnoreCase(type, "application/fb2") || containsIgnoreCase(type, "text/fb2") ||
      containsIgnoreCase(type, "fictionbook+xml")) {
    format = OpdsBookFormat::FB2;
    return true;
  }
  return false;
}


std::string sanitizeDescriptionText(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() < 1536 ? raw.size() : 1536);
  bool inTag = false;
  bool pendingSpace = false;

  auto appendSpace = [&]() {
    if (!out.empty() && out.back() != ' ') pendingSpace = true;
  };

  for (size_t i = 0; i < raw.size();) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    if (c == '<') {
      inTag = true;
      appendSpace();
      ++i;
      continue;
    }
    if (inTag) {
      if (c == '>') inTag = false;
      ++i;
      continue;
    }

    if (c == '&') {
      const size_t semi = raw.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 10) {
        const std::string entity = raw.substr(i, semi - i + 1);
        char decoded = 0;
        if (entity == "&nbsp;" || entity == "&#160;") decoded = ' ';
        else if (entity == "&amp;") decoded = '&';
        else if (entity == "&lt;") decoded = '<';
        else if (entity == "&gt;") decoded = '>';
        else if (entity == "&quot;") decoded = '"';
        else if (entity == "&apos;") decoded = '\'';
        if (decoded) {
          if (decoded == ' ') appendSpace();
          else {
            if (pendingSpace && !out.empty()) out.push_back(' ');
            pendingSpace = false;
            out.push_back(decoded);
          }
          i = semi + 1;
          continue;
        }
      }
    }

    if (c <= 0x20) {
      appendSpace();
      ++i;
      continue;
    }

    size_t cpLen = 1;
    if ((c & 0xE0) == 0xC0) cpLen = 2;
    else if ((c & 0xF0) == 0xE0) cpLen = 3;
    else if ((c & 0xF8) == 0xF0) cpLen = 4;
    if (i + cpLen > raw.size()) break;

    if (pendingSpace && !out.empty()) out.push_back(' ');
    pendingSpace = false;
    if (out.size() + cpLen > 1536) break;
    out.append(raw, i, cpLen);
    i += cpLen;
  }

  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool sameAcquisition(const OpdsEntry& entry, const OpdsBookFormat format, const char* href) {
  for (uint8_t i = 0; i < entry.acquisitionCount; ++i) {
    if (entry.acquisitions[i].format == format && entry.acquisitions[i].href == href) return true;
  }
  return false;
}
}  // namespace

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        std::string sHref(href);
        if (sHref.find("{searchTerms}") != std::string::npos) {
          self->searchTemplate = std::move(sHref);
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        self->nextPageUrl = href;
      } else if (rel && (strcmp(rel, "previous") == 0 || strcmp(rel, "prev") == 0) && !self->inEntry) {
        self->prevPageUrl = href;
      }

      if (self->inEntry && !self->discardCurrentEntry) {
        OpdsBookFormat format;
        // Some older Russian OPDS catalogs publish valid book MIME types but
        // omit or use a non-standard rel value.  A recognized publication MIME
        // inside an <entry> is enough to treat the link as an acquisition.
        const bool acquisition = classifyAcquisitionType(type, format);
        if (acquisition) {
          if (self->currentEntry.acquisitionCount < MAX_OPDS_ACQUISITIONS &&
              !sameAcquisition(self->currentEntry, format, href)) {
            auto& slot = self->currentEntry.acquisitions[self->currentEntry.acquisitionCount++];
            slot.format = format;
            slot.href = href;
            // Do not log every acquisition link here.  On TLS-heavy feeds the
            // logger itself adds transient allocations while Expat, mbedTLS and
            // the entry strings are all live.  The selected download URL is
            // still logged later by OpdsBookBrowserActivity.
          }
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          // Keep publication-details navigation even when the same entry also
          // contains direct acquisition links.  The UI can still download the
          // supported formats without losing the catalog relationship.
          if (self->currentEntry.navigationHref.empty()) self->currentEntry.navigationHref = href;
        }
      }
    }
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->discardCurrentEntry = !self->entrySink && self->entryCount >= self->entryCapacity;
    if (self->discardCurrentEntry) {
      self->truncated = true;
      self->saturated = true;
      return;
    }
    self->currentEntry = OpdsEntry{};
    self->currentText.clear();
    return;
  }

  if (!self->inEntry || self->discardCurrentEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  } else if (strcmp(name, "summary") == 0 || strstr(name, ":summary") != nullptr) {
    self->inSummary = true;
    self->currentText.clear();
  } else if (strcmp(name, "content") == 0 || strstr(name, ":content") != nullptr) {
    // Prefer summary when both are present. Content is only a fallback.
    if (self->currentEntry.description.empty()) {
      self->inContent = true;
      self->currentText.clear();
    }
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (!self->discardCurrentEntry && !self->currentEntry.title.empty()) {
      if (self->currentEntry.acquisitionCount > 0) {
        self->currentEntry.type = OpdsEntryType::BOOK;
        self->currentEntry.href = self->currentEntry.acquisitions[0].href;
      } else if (!self->currentEntry.navigationHref.empty()) {
        self->currentEntry.type = OpdsEntryType::NAVIGATION;
        self->currentEntry.href = self->currentEntry.navigationHref;
      }

      if (self->currentEntry.href.empty() && !self->currentEntry.description.empty()) {
        self->currentEntry.type = OpdsEntryType::INFO;
      }

      if (!self->currentEntry.href.empty() || self->currentEntry.type == OpdsEntryType::INFO) {
        if (self->entrySink) {
          if (!self->entrySink(self->sinkContext, std::move(self->currentEntry))) {
            self->errorOccured = true;
            self->errorReason = OpdsParserError::SINK_ERROR;
            self->saturated = true;
          } else {
            ++self->entryCount;
          }
        } else if (self->entryCount < self->entryCapacity) {
          self->entries[self->entryCount++] = std::move(self->currentEntry);
          if (self->entryCount >= self->entryCapacity) {
            self->truncated = true;
            self->saturated = true;
          }
        } else {
          self->truncated = true;
          self->saturated = true;
        }
      }
    }
    self->currentEntry = OpdsEntry{};
    self->currentText.clear();
    self->discardCurrentEntry = false;
    self->inEntry = false;
    self->inTitle = self->inAuthor = self->inAuthorName = self->inId = false;
    self->inSummary = self->inContent = false;
  } else if (self->inEntry && !self->discardCurrentEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = std::move(self->currentText);
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      self->currentEntry.author = std::move(self->currentText);
      self->inAuthorName = false;
    } else if (self->inId && (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr)) {
      self->currentEntry.id = std::move(self->currentText);
      self->inId = false;
    } else if (self->inSummary && (strcmp(name, "summary") == 0 || strstr(name, ":summary") != nullptr)) {
      self->currentEntry.description = sanitizeDescriptionText(self->currentText);
      self->inSummary = false;
    } else if (self->inContent && (strcmp(name, "content") == 0 || strstr(name, ":content") != nullptr)) {
      if (self->currentEntry.description.empty()) self->currentEntry.description = sanitizeDescriptionText(self->currentText);
      self->inContent = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->discardCurrentEntry ||
      !(self->inTitle || self->inAuthorName || self->inId || self->inSummary || self->inContent) || len <= 0)
    return;

  // OPDS is UI/navigation data, not book content.  Do not allow a malformed or
  // unusually verbose feed to grow one field without bound.  These limits are
  // deliberately generous for real titles/authors while keeping RAM usage
  // deterministic on devices without PSRAM.
  const size_t limit = self->inTitle ? 256u
                                     : (self->inAuthorName ? 160u
                                                           : (self->inId ? 128u
                                                                         : ((self->inSummary || self->inContent) ? 2048u : 0u)));
  if (limit == 0 || self->currentText.size() >= limit) return;
  const size_t room = limit - self->currentText.size();
  const size_t appendLen = static_cast<size_t>(len) < room ? static_cast<size_t>(len) : room;
  self->currentText.append(s, appendLen);
}
