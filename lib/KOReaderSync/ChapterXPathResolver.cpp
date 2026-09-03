#include "ChapterXPathResolver.h"

#include <Logging.h>
#include <Print.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
std::string stripPrefix(const XML_Char* name) {
  if (!name) {
    return "";
  }

  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

struct NameCounter {
  std::string name;
  int count;
};

struct ParentState {
  std::vector<NameCounter> children;

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }

    children.push_back({name, 1});
    return 1;
  }
};

struct PathSegment {
  std::string name;
  int index;
};

std::string buildParagraphXPath(const int spineIndex, const std::vector<PathSegment>& path, const int textNodeIndex,
                                const size_t charOffset) {
  std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  for (const auto& segment : path) {
    xpath += "/" + segment.name + "[" + std::to_string(segment.index) + "]";
  }
  if (textNodeIndex > 0 && charOffset > 0) {
    xpath += "/text()[" + std::to_string(textNodeIndex) + "]." + std::to_string(charOffset);
  }
  return xpath;
}

size_t countUtf8Codepoints(const XML_Char* data, const int len) {
  if (!data || len <= 0) {
    return 0;
  }

  size_t count = 0;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }

  return count;
}

class ParagraphTextCounter final : public Print {
 public:
  ParagraphTextCounter() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &ParagraphTextCounter::startElement, &ParagraphTextCounter::endElement);
    XML_SetCharacterDataHandler(parser, &ParagraphTextCounter::characterData);
  }

  ~ParagraphTextCounter() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  size_t totalVisibleChars() const { return visibleChars; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }

    if (name == "p") {
      paragraphDepth++;
    }
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      return;
    }

    if (name == "p" && paragraphDepth > 0) {
      paragraphDepth--;
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || paragraphDepth <= 0 || len <= 0) {
      return;
    }

    visibleChars += countUtf8Codepoints(data, len);
  }

 private:
  XML_Parser parser = nullptr;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphDepth = 0;
  size_t visibleChars = 0;
};

class XPathParagraphResolver final : public Print {
 public:
  explicit XPathParagraphResolver(const int targetParagraph) : targetParagraph(targetParagraph) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathParagraphResolver::startElement, &XPathParagraphResolver::endElement);
  }

  ~XPathParagraphResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onEndElement(name);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();

    if (name == "p") {
      paragraphCount++;
      if (paragraphCount == targetParagraph) {
        xpath = buildParagraphXPath(spineIndex, path, 0, 0);
        stopped = true;
        XML_StopParser(parser, XML_FALSE);
      }
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      return;
    }

    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  XML_Parser parser = nullptr;
  const int targetParagraph;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphCount = 0;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};

class XPathProgressResolver final : public Print {
 public:
  explicit XPathProgressResolver(const size_t targetVisibleChar) : targetVisibleChar(targetVisibleChar) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathProgressResolver::startElement, &XPathProgressResolver::endElement);
    XML_SetCharacterDataHandler(parser, &XPathProgressResolver::characterData);
  }

  ~XPathProgressResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();
    textNodeIndexStack.push_back(0);
    pendingTextNode = true;

    if (name == "p") {
      paragraphDepth++;
    }
    if (name == "li") {
      liDepth++;
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      textNodeIndexStack.clear();
      return;
    }

    if (name == "p" && paragraphDepth > 0) {
      paragraphDepth--;
    }
    if (name == "li" && liDepth > 0) {
      liDepth--;
    }

    if (!textNodeIndexStack.empty()) {
      textNodeIndexStack.pop_back();
    }
    if (paragraphDepth > 0 || liDepth > 0) {
      pendingTextNode = true;
    }
    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || (paragraphDepth <= 0 && liDepth <= 0) || len <= 0 || stopped) {
      return;
    }

    const size_t codepointCount = countUtf8Codepoints(data, len);
    if (codepointCount == 0) {
      return;
    }

    // Start a new text node on first non-empty content after any element boundary.
    // Only counting non-empty nodes matches KOReader's text()[N] indexing behavior,
    // which skips empty text nodes created by bare <a id="anchor"/> anchors.
    if (pendingTextNode) {
      if (!textNodeIndexStack.empty()) {
        textNodeIndexStack.back()++;
      }
      textNodeStartChars = visibleChars;
      pendingTextNode = false;
    }

    const size_t nextVisibleChars = visibleChars + codepointCount;
    if (targetVisibleChar <= nextVisibleChars) {
      const size_t delta = targetVisibleChar - visibleChars;
      const int texNode = textNodeIndexStack.empty() ? 0 : textNodeIndexStack.back();
      const size_t charOff = visibleChars - textNodeStartChars + delta;
      xpath = buildParagraphXPath(spineIndex, path, texNode, charOff);
      stopped = true;
      XML_StopParser(parser, XML_FALSE);
      return;
    }

    visibleChars = nextVisibleChars;
  }

  XML_Parser parser = nullptr;
  const size_t targetVisibleChar;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  bool pendingTextNode = true;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphDepth = 0;
  int liDepth = 0;
  size_t visibleChars = 0;
  size_t textNodeStartChars = 0;
  std::vector<int> textNodeIndexStack;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};

class TargetParagraphCapture final : public Print {
 public:
  explicit TargetParagraphCapture(uint16_t target) : targetParagraph(target) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) return;
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &TargetParagraphCapture::startElement, &TargetParagraphCapture::endElement);
    XML_SetCharacterDataHandler(parser, &TargetParagraphCapture::characterData);
  }

  ~TargetParagraphCapture() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }
  bool found() const { return captured; }
  const std::string& text() const { return paragraphText; }

  bool finish() {
    if (!parser || !parseOk || stopped) return parseOk;
    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      const auto err = XML_GetErrorCode(parser);
      if (err != XML_ERROR_ABORTED) parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) return size;
    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const auto err = XML_GetErrorCode(parser);
      if (err != XML_ERROR_ABORTED) parseOk = false;
    }
    return size;
  }

 private:
  static void XMLCALL startElement(void* ud, const XML_Char* raw, const XML_Char**) {
    static_cast<TargetParagraphCapture*>(ud)->onStart(raw);
  }
  static void XMLCALL endElement(void* ud, const XML_Char* raw) {
    static_cast<TargetParagraphCapture*>(ud)->onEnd(raw);
  }
  static void XMLCALL characterData(void* ud, const XML_Char* data, int len) {
    static_cast<TargetParagraphCapture*>(ud)->onText(data, len);
  }

  void onStart(const XML_Char* raw) {
    const std::string name = stripPrefix(raw);
    if (!insideBody) {
      if (name == "body") insideBody = true;
      return;
    }
    if (name == "p") {
      ++paragraphCounter;
      if (paragraphCounter == targetParagraph) {
        inTarget = true;
        targetDepth = 1;
      } else if (inTarget) {
        ++targetDepth;
      }
    } else if (inTarget) {
      ++targetDepth;
    }
  }

  void onEnd(const XML_Char* raw) {
    if (!insideBody || !inTarget) return;
    const std::string name = stripPrefix(raw);
    (void)name;
    if (targetDepth > 0) --targetDepth;
    if (targetDepth == 0) {
      inTarget = false;
      captured = true;
      stopped = true;
      XML_StopParser(parser, XML_FALSE);
    }
  }

  void onText(const XML_Char* data, int len) {
    if (!inTarget || len <= 0) return;
    // A pathological FB2 paragraph should not be allowed to consume all of the
    // C3 heap just because sync was requested.  24 KiB is far beyond a normal
    // prose paragraph and still enough for accurate top-of-page matching.
    static constexpr size_t MAX_CAPTURE_BYTES = 24 * 1024;
    const size_t room = paragraphText.size() < MAX_CAPTURE_BYTES ? MAX_CAPTURE_BYTES - paragraphText.size() : 0;
    if (room > 0) paragraphText.append(data, std::min<size_t>(room, static_cast<size_t>(len)));
  }

  XML_Parser parser = nullptr;
  uint16_t targetParagraph = 0;
  uint16_t paragraphCounter = 0;
  bool parseOk = true;
  bool insideBody = false;
  bool inTarget = false;
  bool captured = false;
  bool stopped = false;
  int targetDepth = 0;
  std::string paragraphText;
};

struct MatchToken {
  std::string value;
  size_t codepointStart = 0;
};

bool asciiSpace(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

bool asciiTrimPunct(unsigned char c) {
  switch (c) {
    case '.': case ',': case ';': case ':': case '!': case '?': case '"': case '\'':
    case '(': case ')': case '[': case ']': case '{': case '}': case '<': case '>':
      return true;
    default:
      return false;
  }
}

std::string normalizeToken(const std::string& in) {
  size_t a = 0, b = in.size();
  while (a < b && static_cast<unsigned char>(in[a]) < 0x80 && asciiTrimPunct(static_cast<unsigned char>(in[a]))) ++a;
  while (b > a && static_cast<unsigned char>(in[b - 1]) < 0x80 && asciiTrimPunct(static_cast<unsigned char>(in[b - 1]))) --b;
  std::string out = in.substr(a, b - a);
  for (char& c : out) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 'A' && u <= 'Z') c = static_cast<char>(u + ('a' - 'A'));
  }
  return out;
}

std::vector<MatchToken> tokenizeForMatch(const std::string& text) {
  std::vector<MatchToken> out;
  size_t i = 0;
  size_t cp = 0;
  while (i < text.size()) {
    while (i < text.size() && static_cast<unsigned char>(text[i]) < 0x80 && asciiSpace(static_cast<unsigned char>(text[i]))) {
      ++i; ++cp;
    }
    if (i >= text.size()) break;
    const size_t byteStart = i;
    const size_t cpStart = cp;
    while (i < text.size()) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      if (c < 0x80 && asciiSpace(c)) break;
      if ((c & 0xC0) != 0x80) ++cp;
      ++i;
    }
    std::string token = normalizeToken(text.substr(byteStart, i - byteStart));
    if (!token.empty()) out.push_back({std::move(token), cpStart});
  }
  return out;
}

int findSnippetOffsetInParagraph(const std::string& paragraph, const std::string& snippet) {
  const auto hay = tokenizeForMatch(paragraph);
  const auto needle = tokenizeForMatch(snippet);
  if (hay.empty() || needle.empty()) return -1;

  // If the first screen word is a continuation produced by hyphenation, allow
  // dropping one or two initial snippet tokens.  Prefer an exact top token
  // match whenever possible.
  for (size_t skip = 0; skip <= 2 && skip < needle.size(); ++skip) {
    const size_t available = needle.size() - skip;
    const size_t want = std::min<size_t>(8, available);
    if (want < 3) continue;
    for (size_t h = 0; h + want <= hay.size(); ++h) {
      bool same = true;
      for (size_t n = 0; n < want; ++n) {
        if (hay[h + n].value != needle[skip + n].value) { same = false; break; }
      }
      if (same) return static_cast<int>(hay[h].codepointStart);
    }
  }
  return -1;
}

bool captureParagraph(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex,
                      std::string& out) {
  out.clear();
  if (!epub || paragraphIndex == 0 || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) return false;
  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) return false;
  TargetParagraphCapture cap(paragraphIndex);
  if (!cap.ok() || !epub->readItemContentsToStream(href, cap, 1024) || !cap.finish() || !cap.found()) return false;
  out = cap.text();
  return true;
}

}  // namespace

std::string ChapterXPathResolver::findXPathForParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                        const uint16_t paragraphIndex) {
  if (!epub || paragraphIndex == 0 || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  XPathParagraphResolver resolver(paragraphIndex);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved paragraph %u in spine %d -> %s", paragraphIndex, spineIndex, resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Paragraph %u not found in spine %d", paragraphIndex, spineIndex);
  return "";
}

std::string ChapterXPathResolver::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const float intraSpineProgress) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  if (!(intraSpineProgress > 0.0f)) {
    return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  ParagraphTextCounter counter;
  if (!counter.ok() || !epub->readItemContentsToStream(href, counter, 1024) || !counter.finish()) {
    return "";
  }

  const size_t totalVisibleChars = counter.totalVisibleChars();
  if (totalVisibleChars == 0) {
    return "";
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetVisibleChar =
      std::max<size_t>(1, std::min(totalVisibleChars, static_cast<size_t>(std::ceil(clamped * totalVisibleChars))));

  XPathProgressResolver resolver(targetVisibleChar);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved progress %.3f in spine %d -> %s", intraSpineProgress, spineIndex,
            resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Could not resolve progress %.3f in spine %d", intraSpineProgress, spineIndex);
  return "";
}


int ChapterXPathResolver::findCharOffsetForTextSnippet(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                        const uint16_t paragraphIndex,
                                                        const std::string& snippet) {
  if (snippet.empty()) return -1;
  std::string paragraph;
  if (!captureParagraph(epub, spineIndex, paragraphIndex, paragraph)) return -1;
  const int offset = findSnippetOffsetInParagraph(paragraph, snippet);
  if (offset >= 0) {
    LOG_DBG("KOX", "Top-text snippet matched spine=%d p=%u char=%d", spineIndex, paragraphIndex, offset);
  } else {
    LOG_DBG("KOX", "Top-text snippet did not match spine=%d p=%u", spineIndex, paragraphIndex);
  }
  return offset;
}

size_t ChapterXPathResolver::countVisibleCharsInParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                           const uint16_t paragraphIndex) {
  std::string paragraph;
  if (!captureParagraph(epub, spineIndex, paragraphIndex, paragraph)) return 0;
  return countUtf8Codepoints(paragraph.c_str(), static_cast<int>(paragraph.size()));
}
