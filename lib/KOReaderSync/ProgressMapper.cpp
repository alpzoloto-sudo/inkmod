#include "ProgressMapper.h"
#include "Fb2.h"

#include <Logging.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <climits>
#include <cstring>
#include <vector>
#include <expat.h>

#include "ChapterXPathResolver.h"
#include "Epub/htmlEntities.h"
#include "Utf8.h"

namespace {
int parseIndex(const std::string& xpath, const char* prefix, bool last = false) {
  const size_t prefixLen = strlen(prefix);
  const size_t pos = last ? xpath.rfind(prefix) : xpath.find(prefix);
  if (pos == std::string::npos) return -1;
  const size_t numStart = pos + prefixLen;
  const size_t numEnd = xpath.find(']', numStart);
  if (numEnd == std::string::npos || numEnd == numStart) return -1;
  int val = 0;
  for (size_t i = numStart; i < numEnd; i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return -1;
    val = val * 10 + (xpath[i] - '0');
  }
  return val;
}

int parseCharOffset(const std::string& xpath) {
  const size_t textPos = xpath.rfind("text()");
  const size_t dotPos = (textPos != std::string::npos) ? xpath.find('.', textPos) : xpath.rfind('.');
  if (dotPos == std::string::npos || dotPos + 1 >= xpath.size()) return 0;
  int val = 0;
  for (size_t i = dotPos + 1; i < xpath.size(); i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return 0;
    val = val * 10 + (xpath[i] - '0');
  }
  return val;
}

// Parse the N from text()[N] in the XPath (1-based; defaults to 1 if absent or 1).
int parseTextNodeIndex(const std::string& xpath) {
  const size_t textPos = xpath.rfind("text()[");
  if (textPos == std::string::npos) return 1;
  const size_t numStart = textPos + 7;  // strlen("text()[")
  const size_t numEnd = xpath.find(']', numStart);
  if (numEnd == std::string::npos || numEnd == numStart) return 1;
  int val = 0;
  for (size_t i = numStart; i < numEnd; i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return 1;
    val = val * 10 + (xpath[i] - '0');
  }
  return val > 0 ? val : 1;
}

bool isChapterStartXPath(const std::string& xpath) {
  if (xpath.find("/p[") != std::string::npos || xpath.find("/li[") != std::string::npos) {
    return false;
  }

  static constexpr char kDocFragment[] = "/body/DocFragment[";
  const size_t docFragPos = xpath.find(kDocFragment);
  if (docFragPos == std::string::npos) {
    return false;
  }
  const size_t docFragEnd = xpath.find(']', docFragPos + strlen(kDocFragment));
  if (docFragEnd == std::string::npos) {
    return false;
  }
  if (docFragEnd + 1 == xpath.size()) {
    return true;
  }
  if (xpath[docFragEnd + 1] == '.') {
    if (docFragEnd + 2 >= xpath.size()) {
      return false;
    }
    for (size_t i = docFragEnd + 2; i < xpath.size(); i++) {
      if (xpath[i] != '0') return false;
    }
    return true;
  }

  static constexpr char kDocBody[] = "]/body";
  const size_t docBodyPos = xpath.find(kDocBody);
  if (docBodyPos == std::string::npos) {
    return false;
  }
  size_t bodyContentStart = docBodyPos + strlen(kDocBody);
  if (bodyContentStart == xpath.size()) {
    return true;
  }
  if (xpath[bodyContentStart] != '/') {
    return false;
  }
  bodyContentStart++;
  if (bodyContentStart == xpath.size()) {
    return true;
  }

  const size_t dotPos = xpath.rfind('.');
  if (dotPos == std::string::npos || dotPos <= bodyContentStart || dotPos + 1 >= xpath.size()) {
    return false;
  }
  size_t terminalEnd = dotPos;
  static constexpr char kTextNode[] = "/text()";
  const size_t textNodePos = xpath.rfind(kTextNode, dotPos);
  if (textNodePos != std::string::npos && textNodePos >= bodyContentStart) {
    terminalEnd = textNodePos;
  }
  if (xpath.find('/', bodyContentStart) < terminalEnd) {
    return false;
  }

  for (size_t i = dotPos + 1; i < xpath.size(); i++) {
    if (xpath[i] != '0') return false;
  }
  return true;
}

// Parsed representation of one step in the XPath ancestry.
struct XPathStep {
  char tag[12];      // element name, null-terminated
  int siblingIndex;  // 1-based sibling index, or 0 if unspecified (treat as 1)
};

static constexpr int MAX_XPATH_DEPTH = 16;

// Parse the XPath segment between /body/DocFragment[N]/body/ and the terminal position
// into an ordered sequence of steps. Returns step count, 0 on failure.
// Example input: "/body/DocFragment[1]/body/div[1]/ul/li[4]/text()[1].51"
// Fills steps with: {div,1}, {ul,1}, {li,4}
int parseXPathSteps(const std::string& xpath, XPathStep steps[MAX_XPATH_DEPTH]) {
  static const char kBodyFrag[] = "/body/DocFragment[";
  const size_t fragPos = xpath.find(kBodyFrag);
  if (fragPos == std::string::npos) return 0;
  const size_t afterBracket = xpath.find(']', fragPos + strlen(kBodyFrag));
  if (afterBracket == std::string::npos) return 0;
  static const char kBody[] = "/body/";
  if (xpath.compare(afterBracket + 1, strlen(kBody), kBody) != 0) return 0;
  size_t pos = afterBracket + 1 + strlen(kBody);

  size_t stepsEnd = xpath.rfind("/text()");
  if (stepsEnd == std::string::npos) {
    stepsEnd = xpath.rfind('.');
    if (stepsEnd == std::string::npos || stepsEnd <= pos || stepsEnd + 1 >= xpath.size()) return 0;
    for (size_t i = stepsEnd + 1; i < xpath.size(); i++) {
      if (xpath[i] < '0' || xpath[i] > '9') return 0;
    }
  }
  if (stepsEnd <= pos) return 0;

  int count = 0;
  while (pos < stepsEnd && count < MAX_XPATH_DEPTH) {
    const size_t slash = xpath.find('/', pos);
    const size_t segEnd = (slash < stepsEnd) ? slash : stepsEnd;

    XPathStep& step = steps[count];
    const size_t bracket = xpath.find('[', pos);
    const size_t nameEnd = (bracket != std::string::npos && bracket < segEnd) ? bracket : segEnd;
    const size_t nameLen = nameEnd - pos;
    if (nameLen == 0 || nameLen >= sizeof(step.tag)) return 0;
    memcpy(step.tag, xpath.c_str() + pos, nameLen);
    step.tag[nameLen] = '\0';

    if (bracket != std::string::npos && bracket < segEnd) {
      const size_t closeBracket = xpath.find(']', bracket + 1);
      if (closeBracket == std::string::npos || closeBracket > segEnd) return 0;
      int idx = 0;
      for (size_t i = bracket + 1; i < closeBracket; i++) {
        if (xpath[i] < '0' || xpath[i] > '9') return 0;
        idx = idx * 10 + (xpath[i] - '0');
      }
      step.siblingIndex = idx;
    } else {
      step.siblingIndex = 1;
    }

    count++;
    pos = (slash < stepsEnd) ? slash + 1 : stepsEnd;
  }
  return count;
}

class ParagraphStreamer final : public Print {
  size_t bytesWritten = 0;
  bool globalInTag = false;
  bool globalInEntity = false;
  static constexpr size_t MAX_ENTITY_SIZE = 16;
  char entityBuffer[MAX_ENTITY_SIZE] = {};
  size_t entityLen = 0;

  // Forward mode: count <p> paragraphs at a byte offset (legacy, used by generateXPath)
  size_t fwdTarget;
  int fwdResult = 0;
  bool fwdCaptured = false;

  // Reverse mode shared state
  int revChar;
  bool revPFound = false;
  bool revDone = false;
  int revVisChars = 0;
  size_t totalVisChars = 0;
  size_t targetVisChars = 0;

  // --- Legacy reverse mode (paragraph index only, no ancestry) ---
  int revParagraph = 0;
  int pCount = 0;
  int paragraphAtMatch = 0;
  int liCount = 0;
  int liCountAtMatch = 0;
  int targetTextNode = 1;
  int currentTextNode = 0;
  int paragraphHtmlDepth = -1;

  // --- Ancestry-aware reverse mode ---
  const XPathStep* steps = nullptr;
  int stepCount = 0;
  int siblingCounters[MAX_XPATH_DEPTH] = {};
  bool insideStep[MAX_XPATH_DEPTH] = {};
  int htmlDepth = 0;
  int stepEnteredAtDepth[MAX_XPATH_DEPTH] = {};

  // Tag name accumulation
  enum TagParseState { TAG_IDLE, TAG_IN_NAME, TAG_ATTRS } tagState = TAG_IDLE;
  bool tagIsClose = false;
  char tagName[12] = {};
  int tagNameLen = 0;

  int matchedDepth = 0;

  // Anchor ID capture
  static constexpr int MAX_ANCHOR_ID = 64;
  char capturedAnchorId[MAX_ANCHOR_ID] = {};
  int capturedAnchorIdLen = 0;
  bool capturingAnchorTag = false;
  enum AnchorAttrState {
    ATTR_FIND_NAME,
    ATTR_READ_NAME,
    ATTR_AFTER_NAME,
    ATTR_BEFORE_VALUE,
    ATTR_CAPTURE_D,
    ATTR_CAPTURE_S
  } attrState = ATTR_FIND_NAME;
  uint8_t attrNameLen = 0;
  bool currentAttrIsId = false;
  bool inAttrQuote =
      false;  // true while inside a quoted attribute value (prevents '/' from being treated as self-close)
  char attrQuoteChar = 0;
  uint8_t nonVisibleDepth = 0;

  bool isNonVisibleTag() const {
    return strcasecmp(tagName, "head") == 0 || strcasecmp(tagName, "style") == 0 ||
           strcasecmp(tagName, "script") == 0 || strcasecmp(tagName, "title") == 0;
  }

  static bool isAttrWhitespace(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

  static bool isAttrNameChar(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == ':' || c == '.';
  }

  void resetAnchorAttrScan() {
    attrState = ATTR_FIND_NAME;
    attrNameLen = 0;
    currentAttrIsId = false;
  }

  void finishCapturedAnchorId() {
    capturedAnchorId[capturedAnchorIdLen] = '\0';
    capturingAnchorTag = false;
    resetAnchorAttrScan();
  }

  void beginAnchorIdScan() {
    capturingAnchorTag = true;
    resetAnchorAttrScan();
  }

  void endAnchorIdScan() {
    if (capturingAnchorTag) {
      capturedAnchorIdLen = 0;
    }
    capturingAnchorTag = false;
    resetAnchorAttrScan();
  }

  void appendCapturedAnchorId(uint8_t c) {
    if (capturedAnchorIdLen + 1 < MAX_ANCHOR_ID) {
      capturedAnchorId[capturedAnchorIdLen++] = c;
    }
  }

  void scanAnchorAttribute(uint8_t c) {
    switch (attrState) {
      case ATTR_FIND_NAME:
        if (isAttrNameChar(c)) {
          attrState = ATTR_READ_NAME;
          attrNameLen = 1;
          currentAttrIsId = c == 'i';
        }
        break;
      case ATTR_READ_NAME:
        if (isAttrNameChar(c)) {
          if (attrNameLen == 1) {
            currentAttrIsId = currentAttrIsId && c == 'd';
          } else {
            currentAttrIsId = false;
          }
          attrNameLen++;
        } else {
          currentAttrIsId = currentAttrIsId && attrNameLen == 2;
          if (isAttrWhitespace(c)) {
            attrState = ATTR_AFTER_NAME;
          } else if (c == '=') {
            attrState = ATTR_BEFORE_VALUE;
          } else {
            resetAnchorAttrScan();
          }
        }
        break;
      case ATTR_AFTER_NAME:
        if (isAttrWhitespace(c)) {
          break;
        }
        if (c == '=') {
          attrState = ATTR_BEFORE_VALUE;
        } else if (isAttrNameChar(c)) {
          attrState = ATTR_READ_NAME;
          attrNameLen = 1;
          currentAttrIsId = c == 'i';
        } else {
          resetAnchorAttrScan();
        }
        break;
      case ATTR_BEFORE_VALUE:
        if (isAttrWhitespace(c)) {
          break;
        }
        if (currentAttrIsId && c == '"') {
          capturedAnchorIdLen = 0;
          attrState = ATTR_CAPTURE_D;
        } else if (currentAttrIsId && c == '\'') {
          capturedAnchorIdLen = 0;
          attrState = ATTR_CAPTURE_S;
        } else if (c == '"') {
          attrState = ATTR_CAPTURE_D;
        } else if (c == '\'') {
          attrState = ATTR_CAPTURE_S;
        } else {
          resetAnchorAttrScan();
        }
        break;
      case ATTR_CAPTURE_D:
        if (c == '"') {
          if (currentAttrIsId) {
            finishCapturedAnchorId();
          } else {
            resetAnchorAttrScan();
          }
        } else if (currentAttrIsId) {
          appendCapturedAnchorId(c);
        }
        break;
      case ATTR_CAPTURE_S:
        if (c == '\'') {
          if (currentAttrIsId) {
            finishCapturedAnchorId();
          } else {
            resetAnchorAttrScan();
          }
        } else if (currentAttrIsId) {
          appendCapturedAnchorId(c);
        }
        break;
    }
  }

  void onVisibleCodepoint() {
    totalVisChars++;
    if (revPFound && !revDone) {
      // Ancestry mode: count only while inside the fully-matched element and in the target text node.
      // Legacy mode: count only while still inside the matched paragraph and in the target text node.
      const bool inTargetNode = (stepCount > 0) ? (matchedDepth == stepCount && currentTextNode == targetTextNode)
                                                : (paragraphHtmlDepth >= 0 && currentTextNode == targetTextNode);
      if (inTargetNode) {
        revVisChars++;
        if (revVisChars >= revChar) {
          targetVisChars = totalVisChars;
          revDone = true;
        }
      }
    }
  }

  void onVisibleText(const char* text) {
    if (!text) return;
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text);
    while (*ptr != 0) {
      utf8NextCodepoint(&ptr);
      onVisibleCodepoint();
    }
  }

  void flushEntityAsLiteral() {
    for (size_t i = 0; i < entityLen; i++) onVisibleCodepoint();
  }

  void finishEntity() {
    entityBuffer[entityLen] = '\0';
    const char* resolved = lookupHtmlEntity(entityBuffer, entityLen);
    if (resolved)
      onVisibleText(resolved);
    else
      flushEntityAsLiteral();
    globalInEntity = false;
    entityLen = 0;
  }

  void onLegacyP() {
    pCount++;
    if (!revPFound && revParagraph > 0 && pCount >= revParagraph) {
      revPFound = true;
      revVisChars = 0;
      paragraphHtmlDepth = htmlDepth;
      currentTextNode = 1;
      if (revChar <= 0 && targetTextNode <= 1) {
        targetVisChars = totalVisChars;
        revDone = true;
      }
    }
  }

  void onOpenTag() {
    htmlDepth++;

    if (nonVisibleDepth > 0 || isNonVisibleTag()) {
      nonVisibleDepth++;
      return;
    }

    if (stepCount == 0) {
      if (strcasecmp(tagName, "p") == 0) onLegacyP();
      return;
    }

    // Capture a child <a id> inside the fully-matched element even after target char is found.
    if (revPFound && matchedDepth == stepCount && capturedAnchorIdLen == 0 && strcasecmp(tagName, "a") == 0) {
      beginAnchorIdScan();
    }

    if (revDone) return;

    if (strcasecmp(tagName, "p") == 0) pCount++;
    if (strcasecmp(tagName, "li") == 0) liCount++;

    if (matchedDepth < stepCount) {
      const XPathStep& target = steps[matchedDepth];
      if (strcasecmp(tagName, target.tag) == 0) {
        // Count only direct children of the previously matched ancestor step.
        // For step 0 any depth is valid; subsequent steps must be exactly one level deeper.
        const bool atCorrectDepth = (matchedDepth == 0) || (htmlDepth == stepEnteredAtDepth[matchedDepth - 1] + 1);
        if (!atCorrectDepth) return;
        siblingCounters[matchedDepth]++;
        if (siblingCounters[matchedDepth] == target.siblingIndex) {
          insideStep[matchedDepth] = true;
          stepEnteredAtDepth[matchedDepth] = htmlDepth;
          matchedDepth++;
          if (matchedDepth == stepCount) {
            beginAnchorIdScan();
            paragraphAtMatch = pCount;
            liCountAtMatch = liCount;
            revPFound = true;
            capturedAnchorIdLen = 0;
            revVisChars = 0;
            currentTextNode = 1;  // Reset text node counter for this element
            if (revChar <= 0 && targetTextNode <= 1) {
              targetVisChars = totalVisChars;
              revDone = true;
            }
          }
        }
      }
    }
  }

  void onCloseTag() {
    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
      if (htmlDepth > 0) htmlDepth--;
      return;
    }

    // Legacy mode: each direct child element closing advances the text node index.
    if (stepCount == 0 && revPFound && !revDone && paragraphHtmlDepth >= 0 && htmlDepth == paragraphHtmlDepth + 1) {
      currentTextNode++;
      if (currentTextNode == targetTextNode && revChar <= 0) {
        targetVisChars = totalVisChars;
        revDone = true;
      }
    }
    // Legacy mode: stop tracking when the matched paragraph itself closes.
    if (stepCount == 0 && revPFound && !revDone && paragraphHtmlDepth >= 0 && htmlDepth == paragraphHtmlDepth) {
      revPFound = false;
      paragraphHtmlDepth = -1;
    }

    // Ancestry mode: advance text node when a direct child of the fully-matched element closes.
    if (stepCount > 0 && matchedDepth == stepCount && revPFound && !revDone) {
      const int elementDepth = stepEnteredAtDepth[stepCount - 1];
      if (htmlDepth == elementDepth + 1) {
        currentTextNode++;
        if (currentTextNode == targetTextNode && revChar <= 0) {
          targetVisChars = totalVisChars;
          revDone = true;
        }
      }
    }

    if (stepCount > 0 && matchedDepth > 0) {
      const int step = matchedDepth - 1;
      if (insideStep[step] && htmlDepth == stepEnteredAtDepth[step]) {
        insideStep[step] = false;
        matchedDepth--;
        // If the fully-matched element just closed without finding the target, abort.
        if (matchedDepth < stepCount && revPFound && !revDone) {
          revPFound = false;
        }
        for (int i = matchedDepth + 1; i < stepCount; i++) {
          siblingCounters[i] = 0;
          insideStep[i] = false;
          stepEnteredAtDepth[i] = -1;
        }
      }
    }
    if (htmlDepth > 0) htmlDepth--;
  }

  void processByteInTag(uint8_t c) {
    switch (tagState) {
      case TAG_IDLE:
        if (c == '/') {
          tagIsClose = true;
          tagState = TAG_IN_NAME;
        } else if (c != '!' && c != '?') {
          tagIsClose = false;
          tagName[0] = static_cast<char>(c);
          tagNameLen = 1;
          tagState = TAG_IN_NAME;
        }
        break;
      case TAG_IN_NAME:
        if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
          tagName[tagNameLen] = '\0';
          if (tagNameLen > 0) {
            if (tagIsClose)
              onCloseTag();
            else
              onOpenTag();
            // Self-closing open tag (<br/>). Don't double-fire for close tags (</br/>).
            if (c == '/' && !tagIsClose) onCloseTag();
          }
          tagNameLen = 0;
          tagState = (c == '>') ? TAG_IDLE : TAG_ATTRS;
        } else if (tagNameLen + 1 < static_cast<int>(sizeof(tagName))) {
          tagName[tagNameLen++] = static_cast<char>(c);
        }
        break;
      case TAG_ATTRS:
        // Track quoted attribute values so '/' inside them is not mistaken for self-closing.
        if (!inAttrQuote) {
          if (c == '"' || c == '\'') {
            inAttrQuote = true;
            attrQuoteChar = c;
          }
        } else if (c == attrQuoteChar) {
          inAttrQuote = false;
          attrQuoteChar = 0;
        }
        if (capturingAnchorTag) {
          scanAnchorAttribute(c);
        }
        // Only treat '/' as self-closing when outside a quoted attribute value.
        if (c == '/' && !inAttrQuote) {
          endAnchorIdScan();
          onCloseTag();
        }
        break;
    }
  }

 public:
  explicit ParagraphStreamer(size_t targetByte) : fwdTarget(targetByte), revChar(0) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  ParagraphStreamer(int paragraph, int charOff, int textNodeIdx = 1)
      : fwdTarget(SIZE_MAX), revChar(charOff), revParagraph(paragraph), targetTextNode(textNodeIdx) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  ParagraphStreamer(const XPathStep* xpathSteps, int xpathStepCount, int charOff, int textNodeIdx = 1)
      : fwdTarget(SIZE_MAX),
        revChar(charOff),
        steps(xpathSteps),
        stepCount(xpathStepCount),
        targetTextNode(textNodeIdx) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  size_t write(uint8_t c) override {
    if (!fwdCaptured && bytesWritten >= fwdTarget) {
      fwdResult = pCount;
      fwdCaptured = true;
    }
    bytesWritten++;

    if (globalInEntity) {
      if (entityLen + 1 < MAX_ENTITY_SIZE) {
        entityBuffer[entityLen++] = static_cast<char>(c);
      } else {
        flushEntityAsLiteral();
        globalInEntity = false;
        entityLen = 0;
      }
      if (globalInEntity) {
        if (c == ';') {
          finishEntity();
        } else if (c == '<' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          flushEntityAsLiteral();
          globalInEntity = false;
          entityLen = 0;
        }
      }
      return 1;
    }

    if (c == '<') {
      globalInTag = true;
      tagState = TAG_IDLE;
      tagNameLen = 0;
      tagIsClose = false;
      capturingAnchorTag = false;
      resetAnchorAttrScan();
      inAttrQuote = false;
      attrQuoteChar = 0;
    } else if (c == '>') {
      if (tagState == TAG_ATTRS) {
        endAnchorIdScan();
      }
      globalInTag = false;
      inAttrQuote = false;
      if (tagState == TAG_IN_NAME && tagNameLen > 0) {
        tagName[tagNameLen] = '\0';
        if (tagIsClose)
          onCloseTag();
        else
          onOpenTag();
        tagNameLen = 0;
      }
      tagState = TAG_IDLE;
    } else if (globalInTag) {
      processByteInTag(c);
    } else if (nonVisibleDepth > 0) {
      // Ignore head/style/script/title text. KOReader XPaths are body-relative, and CSS text
      // should not contribute to intra-spine progress.
    } else {
      if (c == '&') {
        globalInEntity = true;
        entityBuffer[0] = '&';
        entityLen = 1;
      } else {
        const bool startsCodepoint = (c & 0xC0) != 0x80;
        if (startsCodepoint) onVisibleCodepoint();
      }
    }
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) write(buffer[i]);
    return size;
  }

  int paragraphCount() const { return fwdCaptured ? fwdResult : pCount; }
  int getParagraphAtMatch() const { return paragraphAtMatch; }
  int getListItemAtMatch() const { return liCountAtMatch; }
  const char* getCapturedAnchorId() const { return capturedAnchorIdLen > 0 ? capturedAnchorId : nullptr; }
  size_t totalBytes() const { return bytesWritten; }
  bool found() const { return revDone || revPFound; }
  size_t getTotalVisChars() const { return totalVisChars; }
  size_t getTargetVisChars() const { return targetVisChars; }
  float progress() const {
    return totalVisChars > 0 ? static_cast<float>(targetVisChars) / static_cast<float>(totalVisChars) : 0.0f;
  }
};

bool streamSpine(const std::shared_ptr<Epub>& epub, int spineIndex, ParagraphStreamer& s) {
  const auto href = epub->getSpineItem(spineIndex).href;
  return !href.empty() && epub->readItemContentsToStream(href, s, 1024);
}

int countParagraphsInSpine(const std::shared_ptr<Epub>& epub, int spineIndex) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) return 0;
  ParagraphStreamer s(SIZE_MAX);
  if (!streamSpine(epub, spineIndex, s)) return 0;
  return std::max(0, s.paragraphCount());
}


// Canonical FB2 source XPath parser -----------------------------------------
// KOReader's FB2 backend can expose/store source-document positions such as:
//   /FictionBook/body/section[2]/section[7]/p[328]/text().76
// or with explicit [1] indices on FictionBook/body.  This is the coordinate
// we want for cross-device sync: it names the real FB2 XML hierarchy and is
// independent of CREngine DocFragment numbering and pagination.
bool parseCanonicalFb2SourceXPath(const std::string& xpath, uint16_t& bodyIndex,
                                  std::vector<uint16_t>& sectionPath,
                                  int& paragraphIndex, int& charOffset) {
  bodyIndex = 1;
  sectionPath.clear();
  paragraphIndex = 0;
  charOffset = parseCharOffset(xpath);

  const size_t fb = xpath.find("/FictionBook");
  if (fb == std::string::npos) return false;
  size_t body = xpath.find("/body", fb + 1);
  if (body == std::string::npos) return false;
  size_t pos = body + strlen("/body");
  if (pos < xpath.size() && xpath[pos] == '[') {
    const size_t close = xpath.find(']', pos + 1);
    if (close == std::string::npos) return false;
    int idx = 0;
    for (size_t i = pos + 1; i < close; ++i) {
      if (xpath[i] < '0' || xpath[i] > '9') return false;
      idx = idx * 10 + (xpath[i] - '0');
    }
    if (idx <= 0 || idx > 65535) return false;
    bodyIndex = static_cast<uint16_t>(idx);
    pos = close + 1;
  }

  while (true) {
    const size_t sec = xpath.find("/section[", pos);
    const size_t para = xpath.find("/p[", pos);
    if (sec == std::string::npos || (para != std::string::npos && para < sec)) break;
    const size_t numStart = sec + strlen("/section[");
    const size_t close = xpath.find(']', numStart);
    if (close == std::string::npos) return false;
    int idx = 0;
    for (size_t i = numStart; i < close; ++i) {
      if (xpath[i] < '0' || xpath[i] > '9') return false;
      idx = idx * 10 + (xpath[i] - '0');
    }
    if (idx <= 0 || idx > 65535 || sectionPath.size() >= 32) return false;
    sectionPath.push_back(static_cast<uint16_t>(idx));
    pos = close + 1;
  }

  const size_t ppos = xpath.find("/p[", pos);
  if (ppos != std::string::npos) {
    const size_t numStart = ppos + strlen("/p[");
    const size_t close = xpath.find(']', numStart);
    if (close == std::string::npos) return false;
    int idx = 0;
    for (size_t i = numStart; i < close; ++i) {
      if (xpath[i] < '0' || xpath[i] > '9') return false;
      idx = idx * 10 + (xpath[i] - '0');
    }
    paragraphIndex = idx;
  }
  return !sectionPath.empty();
}


// Translate between KOReader's *source XML sibling path* and inkMOD's flattened
// paragraph stream.  These are NOT the same numbering when an FB2 section
// contains <cite><p>...</p></cite>, <empty-line/>, verse <v>, text-author, etc.
// KOReader's /section/.../p[N] indexes siblings in the original XML, while the
// synthetic XHTML renderer turns all of those text blocks into page-LUT <p>s.
struct SourceRelStep {
  std::string tag;
  int index = 1;
};

struct SourceNameCount {
  std::string name;
  int count = 0;
};

struct SourceXmlFrame {
  std::string name;
  int siblingIndex = 1;
  std::vector<SourceNameCount> children;

  int nextChildIndex(const std::string& childName) {
    for (auto& item : children) {
      if (item.name == childName) return ++item.count;
    }
    children.push_back({childName, 1});
    return 1;
  }
};

static std::string sourceLocalName(const XML_Char* raw) {
  if (!raw) return "";
  const char* colon = std::strrchr(raw, ':');
  return colon ? std::string(colon + 1) : std::string(raw);
}

static bool sourcePathEq(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
  return a == b;
}

static bool isRenderedParagraphSourceTag(const std::string& name) {
  return name == "p" || name == "v" || name == "empty-line" || name == "text-author";
}

static bool readPreparedFb2SourcePath(const std::string& packageCachePath, std::string& out) {
  out.clear();
  char pathBuf[600] = {};
  const size_t n = Storage.readFileToBuffer((packageCachePath + Fb2::SOURCE_MARKER_FILE).c_str(),
                                            pathBuf, sizeof(pathBuf));
  if (n == 0 || n >= sizeof(pathBuf)) return false;
  out.assign(pathBuf, n);
  return Storage.exists(out.c_str());
}

// Parse everything after the final /section[...] and before /text().
// Examples:
//   .../section[5]/p[101]/text().12          -> {p[101]}
//   .../section[5]/cite[1]/p[2]/text().12   -> {cite[1],p[2]}
//   .../section[5]/poem[1]/stanza[2]/v[3]...-> {poem[1],stanza[2],v[3]}
static bool parseCanonicalFb2Tail(const std::string& xpath, std::vector<SourceRelStep>& out) {
  out.clear();

  // Most KOReader FB2 positions end in /text().N, but non-text layout nodes
  // (most importantly <empty-line/>) are published as e.g.
  //   /FictionBook/body/section[3]/section[3]/empty-line[15].0
  // Treat both forms as canonical source coordinates.  The old parser required
  // /text(), so every empty-line sync silently fell back to render paragraph 1
  // and therefore jumped to the beginning of the chapter.
  const size_t textPos = xpath.rfind("/text()");
  size_t pathEnd = textPos;
  if (pathEnd == std::string::npos) {
    const size_t dot = xpath.rfind('.');
    if (dot == std::string::npos) return false;
    // Only accept a numeric KOReader offset suffix.
    if (dot + 1 >= xpath.size()) return false;
    for (size_t i = dot + 1; i < xpath.size(); ++i) {
      if (xpath[i] < '0' || xpath[i] > '9') return false;
    }
    pathEnd = dot;
  }

  size_t lastSectionEnd = std::string::npos;
  size_t scan = 0;
  while (true) {
    const size_t sec = xpath.find("/section[", scan);
    if (sec == std::string::npos || sec >= pathEnd) break;
    const size_t close = xpath.find(']', sec + 9);
    if (close == std::string::npos || close >= pathEnd) return false;
    lastSectionEnd = close + 1;
    scan = close + 1;
  }
  if (lastSectionEnd == std::string::npos) return false;

  size_t pos = lastSectionEnd;
  while (pos < pathEnd) {
    if (xpath[pos] != '/') return false;
    ++pos;
    const size_t segEnd = xpath.find('/', pos);
    const size_t end = (segEnd == std::string::npos || segEnd > pathEnd) ? pathEnd : segEnd;
    if (end <= pos) break;
    const size_t bracket = xpath.find('[', pos);
    SourceRelStep step;
    if (bracket != std::string::npos && bracket < end) {
      step.tag = xpath.substr(pos, bracket - pos);
      const size_t close = xpath.find(']', bracket + 1);
      if (close == std::string::npos || close > end) return false;
      int val = 0;
      for (size_t i = bracket + 1; i < close; ++i) {
        if (xpath[i] < '0' || xpath[i] > '9') return false;
        val = val * 10 + (xpath[i] - '0');
      }
      if (val <= 0) return false;
      step.index = val;
    } else {
      step.tag = xpath.substr(pos, end - pos);
      step.index = 1;
    }
    if (step.tag.empty()) return false;
    out.push_back(std::move(step));
    pos = end;
  }
  return !out.empty();
}

class SourceRenderParagraphMapParser {
 public:
  SourceRenderParagraphMapParser(uint16_t targetBody,
                                 std::vector<uint16_t> targetSections,
                                 std::vector<SourceRelStep> targetTail,
                                 int wantedRenderOrdinal)
      : targetBody_(targetBody),
        targetSections_(std::move(targetSections)),
        targetTail_(std::move(targetTail)),
        wantedRenderOrdinal_(wantedRenderOrdinal) {
    parser_ = XML_ParserCreate(nullptr);
    if (!parser_) return;
    XML_SetUserData(parser_, this);
    XML_SetElementHandler(parser_, &SourceRenderParagraphMapParser::startElement,
                          &SourceRenderParagraphMapParser::endElement);
    XML_SetCharacterDataHandler(parser_, &SourceRenderParagraphMapParser::characterData);
  }

  ~SourceRenderParagraphMapParser() {
    if (parser_) XML_ParserFree(parser_);
  }

  bool ok() const { return parser_ != nullptr && parseOk_; }
  bool feed(const uint8_t* data, size_t len, bool finalChunk) {
    if (!parser_ || !parseOk_ || stopped_) return parseOk_;
    if (XML_Parse(parser_, reinterpret_cast<const char*>(data), static_cast<int>(len),
                  finalChunk ? XML_TRUE : XML_FALSE) != XML_STATUS_OK) {
      if (XML_GetErrorCode(parser_) != XML_ERROR_ABORTED) parseOk_ = false;
    }
    return parseOk_;
  }

  int mappedRenderOrdinal() const { return mappedRenderOrdinal_; }
  const std::string& mappedRelativePath() const { return mappedRelativePath_; }

 private:
  static void XMLCALL startElement(void* ud, const XML_Char* raw, const XML_Char**) {
    static_cast<SourceRenderParagraphMapParser*>(ud)->onStart(raw);
  }
  static void XMLCALL endElement(void* ud, const XML_Char* raw) {
    static_cast<SourceRenderParagraphMapParser*>(ud)->onEnd(raw);
  }
  static void XMLCALL characterData(void* ud, const XML_Char* text, int len) {
    static_cast<SourceRenderParagraphMapParser*>(ud)->onText(text, len);
  }

  std::vector<uint16_t> currentSectionPath() const {
    std::vector<uint16_t> out;
    bool bodySeen = false;
    for (const auto& f : frames_) {
      if (!bodySeen) {
        if (f.frame.name == "body" && f.frame.siblingIndex == targetBody_) bodySeen = true;
        continue;
      }
      if (f.frame.name == "section") out.push_back(static_cast<uint16_t>(f.frame.siblingIndex));
    }
    return out;
  }

  bool inTargetBody() const {
    for (const auto& f : frames_) {
      if (f.frame.name == "body") return f.frame.siblingIndex == targetBody_;
    }
    return false;
  }

  bool inTargetSection() const {
    return inTargetBody() && sourcePathEq(currentSectionPath(), targetSections_);
  }

  bool insideTitleBelowTarget() const {
    if (!inTargetSection()) return false;
    bool targetReached = false;
    size_t seenSections = 0;
    for (const auto& f : frames_) {
      if (f.frame.name == "body") continue;
      if (f.frame.name == "section") {
        ++seenSections;
        if (seenSections == targetSections_.size()) targetReached = true;
        continue;
      }
      if (targetReached && f.frame.name == "title") return true;
    }
    return false;
  }

  std::vector<SourceRelStep> currentRelativePath() const {
    std::vector<SourceRelStep> out;
    if (!inTargetSection()) return out;
    bool afterTarget = false;
    size_t sectionDepth = 0;
    for (const auto& f : frames_) {
      if (f.frame.name == "body") continue;
      if (f.frame.name == "section") {
        ++sectionDepth;
        if (sectionDepth == targetSections_.size()) afterTarget = true;
        continue;
      }
      if (afterTarget) out.push_back({f.frame.name, f.frame.siblingIndex});
    }
    return out;
  }

  static bool relEq(const std::vector<SourceRelStep>& a, const std::vector<SourceRelStep>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (a[i].tag != b[i].tag || a[i].index != b[i].index) return false;
    }
    return true;
  }

  // A KOReader point may name an inline descendant of a rendered paragraph,
  // e.g. cite[8]/p[1]/emphasis[1]/text().259. The synthetic XHTML page LUT
  // still counts only the enclosing <p>, so map any descendant path back to
  // that rendered block instead of requiring an exact path match.
  static bool relIsPrefix(const std::vector<SourceRelStep>& prefix,
                          const std::vector<SourceRelStep>& full) {
    if (prefix.size() > full.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
      if (prefix[i].tag != full[i].tag || prefix[i].index != full[i].index) return false;
    }
    return true;
  }

  static std::string relToXPath(const std::vector<SourceRelStep>& rel) {
    std::string out;
    for (const auto& s : rel) {
      out += "/" + s.tag + "[" + std::to_string(s.index) + "]";
    }
    return out;
  }

  void onStart(const XML_Char* raw) {
    const std::string name = sourceLocalName(raw);
    int sibling = 1;
    if (!frames_.empty()) {
      frames_.back().lastChildWasElement = true;
      sibling = frames_.back().frame.nextChildIndex(name);
    }
    Holder h;
    h.frame.name = name;
    h.frame.siblingIndex = sibling;
    frames_.push_back(std::move(h));

    // A nested <section> beneath the target section is rendered as a separate
    // virtual chapter, so currentSectionPath() becomes longer and it is
    // automatically excluded here.
    if (!inTargetSection() || insideTitleBelowTarget()) return;

    const auto rel = currentRelativePath();

    // FB2 <subtitle> is rendered by inkMOD as an <h3>, while the section
    // paragraph LUT counts only XHTML <p> elements. Remember an exact subtitle
    // source point and bind it to the NEXT rendered paragraph. In normal book
    // layout that paragraph starts on the same page as the subtitle.
    if (mappedRenderOrdinal_ <= 0 && !targetTail_.empty() &&
        name == "subtitle" && relEq(rel, targetTail_)) {
      pendingSubtitleTarget_ = true;
      return;
    }

    if (!isRenderedParagraphSourceTag(name)) return;

    ++renderOrdinal_;

    if (pendingSubtitleTarget_ && mappedRenderOrdinal_ <= 0) {
      mappedRenderOrdinal_ = renderOrdinal_;
      pendingSubtitleTarget_ = false;
      if (wantedRenderOrdinal_ <= 0) stop();
      return;
    }

    // Exact block paths and inline descendants (emphasis/strong/etc.) both map
    // to this enclosing rendered paragraph.
    if (mappedRenderOrdinal_ <= 0 && !targetTail_.empty() &&
        relIsPrefix(rel, targetTail_)) {
      mappedRenderOrdinal_ = renderOrdinal_;
      if (wantedRenderOrdinal_ <= 0) stop();
      return;
    }

    if (wantedRenderOrdinal_ > 0 && renderOrdinal_ == wantedRenderOrdinal_) {
      // Do not stop at <p> start. Wait for its first real text node so reverse
      // sync preserves inline containers such as <emphasis>.
      captureReverseTextPath_ = true;
      captureDepth_ = frames_.size();
      fallbackReversePath_ = relToXPath(rel);
    }
  }

  void onText(const XML_Char* text, int len) {
    if (!captureReverseTextPath_ || wantedRenderOrdinal_ <= 0 || len <= 0) return;
    bool visible = false;
    for (int i = 0; i < len; ++i) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      if (c > 0x20 && c != 0xA0) { visible = true; break; }
    }
    if (!visible) return;
    mappedRelativePath_ = relToXPath(currentRelativePath());
    captureReverseTextPath_ = false;
    stop();
  }

  void onEnd(const XML_Char*) {
    // A non-text block such as <empty-line/> has no character callback. Keep
    // the rendered element itself as its reverse source coordinate.
    if (captureReverseTextPath_ && wantedRenderOrdinal_ > 0 &&
        frames_.size() == captureDepth_) {
      mappedRelativePath_ = fallbackReversePath_;
      captureReverseTextPath_ = false;
      stop();
    }
    if (!frames_.empty()) frames_.pop_back();
  }

  void stop() {
    stopped_ = true;
    XML_StopParser(parser_, XML_FALSE);
  }

  struct Holder {
    SourceXmlFrame frame;
    bool lastChildWasElement = false;
  };

  XML_Parser parser_ = nullptr;
  bool parseOk_ = true;
  bool stopped_ = false;
  uint16_t targetBody_ = 1;
  std::vector<uint16_t> targetSections_;
  std::vector<SourceRelStep> targetTail_;
  int wantedRenderOrdinal_ = 0;
  int renderOrdinal_ = 0;
  int mappedRenderOrdinal_ = 0;
  std::string mappedRelativePath_;
  bool pendingSubtitleTarget_ = false;
  bool captureReverseTextPath_ = false;
  size_t captureDepth_ = 0;
  std::string fallbackReversePath_;
  std::vector<Holder> frames_;
};

// Feed the prepared (plain, UTF-8/transcoded) FB2 source through a tiny Expat
// walker. It never loads a chapter or the book into RAM.
static bool runSourceRenderParagraphMapper(const std::string& packageCachePath,
                                           uint16_t bodyIndex,
                                           const std::vector<uint16_t>& sectionPath,
                                           const std::vector<SourceRelStep>& targetTail,
                                           int wantedRenderOrdinal,
                                           int& outRenderOrdinal,
                                           std::string& outRelativePath) {
  outRenderOrdinal = 0;
  outRelativePath.clear();
  std::string sourcePath;
  if (!readPreparedFb2SourcePath(packageCachePath, sourcePath)) return false;

  HalFile f;
  if (!Storage.openFileForRead("PM", sourcePath, f)) return false;
  SourceRenderParagraphMapParser parser(bodyIndex, sectionPath, targetTail, wantedRenderOrdinal);
  if (!parser.ok()) {
    f.close();
    return false;
  }

  uint8_t buf[1024];
  bool ok = true;
  for (;;) {
    const int got = f.read(buf, sizeof(buf));
    if (got < 0) { ok = false; break; }
    if (got == 0) break;
    if (!parser.feed(buf, static_cast<size_t>(got), false)) { ok = false; break; }
    if (parser.mappedRenderOrdinal() > 0 && wantedRenderOrdinal <= 0) break;
    if (!parser.mappedRelativePath().empty() && wantedRenderOrdinal > 0) break;
  }
  if (ok) parser.feed(nullptr, 0, true);
  f.close();

  outRenderOrdinal = parser.mappedRenderOrdinal();
  outRelativePath = parser.mappedRelativePath();
  return ok && ((wantedRenderOrdinal > 0 && !outRelativePath.empty()) ||
                (wantedRenderOrdinal <= 0 && outRenderOrdinal > 0));
}

static bool canonicalSourceToRenderOrdinal(const std::string& packageCachePath,
                                           const std::string& xpath,
                                           uint16_t bodyIndex,
                                           const std::vector<uint16_t>& sectionPath,
                                           int& outRenderOrdinal) {
  std::vector<SourceRelStep> tail;
  if (!parseCanonicalFb2Tail(xpath, tail)) return false;
  std::string unused;
  return runSourceRenderParagraphMapper(packageCachePath, bodyIndex, sectionPath, tail, 0,
                                        outRenderOrdinal, unused);
}

static bool renderOrdinalToCanonicalSourceElement(const std::string& packageCachePath,
                                                   int originalSectionOrdinal,
                                                   int renderOrdinal,
                                                   std::string& outElementXPath) {
  outElementXPath.clear();
  if (renderOrdinal <= 0) return false;
  uint16_t body = 1;
  std::vector<uint16_t> sections;
  if (!Fb2::getSourceSectionPath(packageCachePath, originalSectionOrdinal, body, sections)) return false;
  int unused = 0;
  std::string rel;
  if (!runSourceRenderParagraphMapper(packageCachePath, body, sections, {}, renderOrdinal, unused, rel)) return false;
  const std::string base = Fb2::buildCanonicalSourceSectionXPath(packageCachePath, originalSectionOrdinal);
  if (base.empty()) return false;
  outElementXPath = base + rel;
  return true;
}


static std::string normalizeSyncSourceText(const std::string& in) {
  std::string out;
  out.reserve(std::min<size_t>(in.size(), 192));
  bool pendingSpace = false;
  for (size_t i = 0; i < in.size() && out.size() < 191; ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (c <= 0x20 || c == 0xA0) {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace && !out.empty()) out.push_back(' ');
    pendingSpace = false;
    out.push_back(static_cast<char>(c));
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// Locate a real FB2 <subtitle> by the text visible at the top of the current X4
// page. The page paragraph LUT cannot represent <subtitle> because inkMOD emits
// it as <h3>, not <p>. This tiny streaming walker lets X4 -> KOReader publish
// the native /subtitle[N]/text().0 coordinate instead of a nearby prose <p>.
class SourceSubtitleTopFinder {
 public:
  SourceSubtitleTopFinder(uint16_t targetBody,
                          std::vector<uint16_t> targetSections,
                          std::string topSnippet)
      : targetBody_(targetBody),
        targetSections_(std::move(targetSections)),
        wanted_(normalizeSyncSourceText(topSnippet)) {
    parser_ = XML_ParserCreate(nullptr);
    if (!parser_) return;
    XML_SetUserData(parser_, this);
    XML_SetElementHandler(parser_, &SourceSubtitleTopFinder::startElement,
                          &SourceSubtitleTopFinder::endElement);
    XML_SetCharacterDataHandler(parser_, &SourceSubtitleTopFinder::textData);
  }
  ~SourceSubtitleTopFinder() { if (parser_) XML_ParserFree(parser_); }

  bool ok() const { return parser_ != nullptr && parseOk_; }
  bool feed(const uint8_t* data, size_t len, bool finalChunk) {
    if (!parser_ || !parseOk_ || stopped_) return parseOk_;
    if (XML_Parse(parser_, reinterpret_cast<const char*>(data), static_cast<int>(len),
                  finalChunk ? XML_TRUE : XML_FALSE) != XML_STATUS_OK) {
      if (XML_GetErrorCode(parser_) != XML_ERROR_ABORTED) parseOk_ = false;
    }
    return parseOk_;
  }
  const std::string& relativePath() const { return foundRel_; }

 private:
  struct Frame {
    SourceXmlFrame frame;
  };

  static void XMLCALL startElement(void* ud, const XML_Char* raw, const XML_Char**) {
    static_cast<SourceSubtitleTopFinder*>(ud)->onStart(raw);
  }
  static void XMLCALL endElement(void* ud, const XML_Char* raw) {
    static_cast<SourceSubtitleTopFinder*>(ud)->onEnd(raw);
  }
  static void XMLCALL textData(void* ud, const XML_Char* text, int len) {
    static_cast<SourceSubtitleTopFinder*>(ud)->onText(text, len);
  }

  std::vector<uint16_t> currentSectionPath() const {
    std::vector<uint16_t> out;
    bool bodySeen = false;
    for (const auto& f : frames_) {
      if (!bodySeen) {
        if (f.frame.name == "body" && f.frame.siblingIndex == targetBody_) bodySeen = true;
        continue;
      }
      if (f.frame.name == "section") out.push_back(static_cast<uint16_t>(f.frame.siblingIndex));
    }
    return out;
  }

  bool inTargetSection() const {
    return currentSectionPath() == targetSections_;
  }

  std::string currentRelativePath() const {
    std::string out;
    bool afterTarget = false;
    size_t sectionDepth = 0;
    for (const auto& f : frames_) {
      if (f.frame.name == "body") continue;
      if (f.frame.name == "section") {
        ++sectionDepth;
        if (sectionDepth == targetSections_.size()) afterTarget = true;
        continue;
      }
      if (afterTarget) out += "/" + f.frame.name + "[" + std::to_string(f.frame.siblingIndex) + "]";
    }
    return out;
  }

  void onStart(const XML_Char* raw) {
    const std::string name = sourceLocalName(raw);
    int sibling = 1;
    if (!frames_.empty()) sibling = frames_.back().frame.nextChildIndex(name);
    Frame h;
    h.frame.name = name;
    h.frame.siblingIndex = sibling;
    frames_.push_back(std::move(h));

    if (name == "subtitle" && inTargetSection()) {
      capturing_ = true;
      captureDepth_ = frames_.size();
      captureRel_ = currentRelativePath();
      captureText_.clear();
    }
  }

  void onText(const XML_Char* text, int len) {
    if (!capturing_ || len <= 0 || captureText_.size() >= 256) return;
    const size_t room = 256 - captureText_.size();
    captureText_.append(text, std::min<size_t>(static_cast<size_t>(len), room));
  }

  void onEnd(const XML_Char* raw) {
    const std::string name = sourceLocalName(raw);
    if (capturing_ && name == "subtitle" && frames_.size() == captureDepth_) {
      const std::string candidate = normalizeSyncSourceText(captureText_);
      if (!candidate.empty() && !wanted_.empty() &&
          (wanted_.rfind(candidate, 0) == 0 || candidate.rfind(wanted_, 0) == 0)) {
        foundRel_ = captureRel_;
        stopped_ = true;
        XML_StopParser(parser_, XML_FALSE);
        return;
      }
      capturing_ = false;
      captureText_.clear();
      captureRel_.clear();
    }
    if (!frames_.empty()) frames_.pop_back();
  }

  XML_Parser parser_ = nullptr;
  bool parseOk_ = true;
  bool stopped_ = false;
  uint16_t targetBody_ = 1;
  std::vector<uint16_t> targetSections_;
  std::string wanted_;
  bool capturing_ = false;
  size_t captureDepth_ = 0;
  std::string captureText_;
  std::string captureRel_;
  std::string foundRel_;
  std::vector<Frame> frames_;
};

static bool findCanonicalSubtitleForTopSnippet(const std::string& packageCachePath,
                                                int originalSectionOrdinal,
                                                const std::string& topSnippet,
                                                std::string& outXPath) {
  outXPath.clear();
  if (topSnippet.empty()) return false;
  uint16_t body = 1;
  std::vector<uint16_t> sections;
  if (!Fb2::getSourceSectionPath(packageCachePath, originalSectionOrdinal, body, sections)) return false;

  std::string sourcePath;
  if (!readPreparedFb2SourcePath(packageCachePath, sourcePath)) return false;
  HalFile f;
  if (!Storage.openFileForRead("PM", sourcePath, f)) return false;

  SourceSubtitleTopFinder parser(body, sections, topSnippet);
  if (!parser.ok()) {
    f.close();
    return false;
  }
  uint8_t buf[1024];
  bool ok = true;
  for (;;) {
    const int got = f.read(buf, sizeof(buf));
    if (got < 0) { ok = false; break; }
    if (got == 0) break;
    if (!parser.feed(buf, static_cast<size_t>(got), false)) { ok = false; break; }
    if (!parser.relativePath().empty()) break;
  }
  if (ok && parser.relativePath().empty()) parser.feed(nullptr, 0, true);
  f.close();
  if (!ok || parser.relativePath().empty()) return false;

  const std::string base = Fb2::buildCanonicalSourceSectionXPath(packageCachePath, originalSectionOrdinal);
  if (base.empty()) return false;
  outXPath = base + parser.relativePath() + "/text().0";
  return true;
}

// FB2/CREngine bridge -------------------------------------------------------
//
// DocFragment[N] is a CREngine DOM fragment index. It is *not* a chapter
// number, and its numbering may include front matter / split fragments.  Keep
// a tiny per-book calibration table learned from real KOReader XPointers.  The
// actual section is selected from text-weighted position + structural hints;
// once selected, docFrag<->sourceSection is remembered for future uploads.
struct Fb2FragMapPair {
  int16_t fragment = 0;
  int16_t section = 0;
};

static constexpr const char* FB2_FRAG_MAP_FILE = "/kosync_cre_fragmap.bin";
static constexpr uint32_t FB2_FRAG_MAP_MAGIC = 0x314D464B;  // KFM1

bool loadFragMap(const std::string& cachePath, std::vector<Fb2FragMapPair>& out) {
  out.clear();
  HalFile f;
  if (!Storage.openFileForRead("KOS", cachePath + FB2_FRAG_MAP_FILE, f)) return false;
  uint32_t magic = 0;
  uint16_t count = 0;
  if (f.read(&magic, sizeof(magic)) != sizeof(magic) || magic != FB2_FRAG_MAP_MAGIC ||
      f.read(&count, sizeof(count)) != sizeof(count) || count > 96) {
    f.close();
    return false;
  }
  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    Fb2FragMapPair pair;
    if (f.read(&pair, sizeof(pair)) != sizeof(pair)) break;
    if (pair.fragment > 0 && pair.section > 0) out.push_back(pair);
  }
  f.close();
  return !out.empty();
}

void saveFragMap(const std::string& cachePath, const std::vector<Fb2FragMapPair>& pairs) {
  HalFile f;
  const std::string tmp = cachePath + std::string(FB2_FRAG_MAP_FILE) + ".tmp";
  if (!Storage.openFileForWrite("KOS", tmp, f)) return;
  const uint32_t magic = FB2_FRAG_MAP_MAGIC;
  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(pairs.size(), 96));
  bool ok = f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic)) == sizeof(magic) &&
            f.write(reinterpret_cast<const uint8_t*>(&count), sizeof(count)) == sizeof(count);
  for (uint16_t i = 0; ok && i < count; ++i) {
    ok = f.write(reinterpret_cast<const uint8_t*>(&pairs[i]), sizeof(Fb2FragMapPair)) == sizeof(Fb2FragMapPair);
  }
  f.close();
  if (!ok) {
    Storage.remove(tmp.c_str());
    return;
  }
  const std::string dst = cachePath + FB2_FRAG_MAP_FILE;
  Storage.remove(dst.c_str());
  Storage.rename(tmp.c_str(), dst.c_str());
}

void rememberFragMap(const std::string& cachePath, int fragment, int section) {
  if (fragment <= 0 || section <= 0 || fragment > INT16_MAX || section > INT16_MAX) return;
  std::vector<Fb2FragMapPair> pairs;
  loadFragMap(cachePath, pairs);
  for (auto& pair : pairs) {
    if (pair.fragment == fragment || pair.section == section) {
      pair.fragment = static_cast<int16_t>(fragment);
      pair.section = static_cast<int16_t>(section);
      saveFragMap(cachePath, pairs);
      return;
    }
  }
  if (pairs.size() >= 96) pairs.erase(pairs.begin());
  pairs.push_back({static_cast<int16_t>(fragment), static_cast<int16_t>(section)});
  saveFragMap(cachePath, pairs);
}

int learnedSectionForFragment(const std::string& cachePath, int fragment) {
  if (fragment <= 0) return -1;
  std::vector<Fb2FragMapPair> pairs;
  if (!loadFragMap(cachePath, pairs)) return -1;
  for (const auto& pair : pairs) if (pair.fragment == fragment) return pair.section;
  return -1;
}

int learnedFragmentForSection(const std::string& cachePath, int section) {
  if (section <= 0) return -1;
  std::vector<Fb2FragMapPair> pairs;
  if (!loadFragMap(cachePath, pairs)) return -1;
  for (const auto& pair : pairs) if (pair.section == section) return pair.fragment;

  // No exact sample yet: use the nearest learned sample as a local bias. This
  // is deliberately only an estimate; exact mappings overwrite it as soon as
  // a real KOReader XPointer for that section is seen.
  int bestDist = INT_MAX;
  int best = -1;
  for (const auto& pair : pairs) {
    const int dist = std::abs(static_cast<int>(pair.section) - section);
    if (dist < bestDist) {
      bestDist = dist;
      best = section + (static_cast<int>(pair.fragment) - static_cast<int>(pair.section));
    }
  }
  return best > 0 ? best : -1;
}

struct Fb2LogicalSection {
  int ordinal = 0;
  int startSpine = 0;
  int endSpine = 0;
  size_t beginBytes = 0;
  size_t endBytes = 0;
  int paragraphs = 0;
};

std::vector<Fb2LogicalSection> collectLogicalSections(const std::shared_ptr<Epub>& epub,
                                                       const std::string& cachePath) {
  std::vector<Fb2LogicalSection> out;
  if (!epub) return out;
  const int spineCount = epub->getSpineItemsCount();
  int i = 0;
  while (i < spineCount) {
    int ordinal = 0;
    int start = i, end = i;
    if (!Fb2::getOriginalSectionOrdinal(cachePath, i, ordinal) || ordinal <= 0) ordinal = static_cast<int>(out.size()) + 1;
    if (!Fb2::getLogicalChapterBounds(cachePath, i, start, end)) { start = i; end = i; }
    start = std::max(0, start);
    end = std::min(spineCount - 1, std::max(start, end));
    Fb2LogicalSection sec;
    sec.ordinal = ordinal;
    sec.startSpine = start;
    sec.endSpine = end;
    sec.beginBytes = start > 0 ? epub->getCumulativeSpineItemSize(start - 1) : 0;
    sec.endBytes = epub->getCumulativeSpineItemSize(end);
    for (int sidx = start; sidx <= end; ++sidx) sec.paragraphs += std::max(0, countParagraphsInSpine(epub, sidx));
    sec.paragraphs = std::max(1, sec.paragraphs);
    out.push_back(sec);
    i = end + 1;
  }
  return out;
}

const Fb2LogicalSection* chooseSectionForRemote(const std::vector<Fb2LogicalSection>& sections,
                                                size_t bookSize, float remotePct, int xpathP,
                                                int currentSectionOrdinal, float localPct,
                                                int learnedSection) {
  if (sections.empty() || bookSize == 0) return nullptr;
  if (learnedSection > 0) {
    for (const auto& sec : sections) if (sec.ordinal == learnedSection) return &sec;
  }

  const Fb2LogicalSection* best = nullptr;
  float bestScore = 1e9f;
  for (const auto& sec : sections) {
    const float begin = static_cast<float>(sec.beginBytes) / static_cast<float>(bookSize);
    const float end = static_cast<float>(sec.endBytes) / static_cast<float>(bookSize);
    float intraHint = 0.5f;
    if (xpathP > 0 && sec.paragraphs > 1) {
      intraHint = std::max(0.0f, std::min(1.0f,
          static_cast<float>(xpathP - 1) / static_cast<float>(sec.paragraphs - 1)));
    }
    const float predicted = begin + (end - begin) * intraHint;
    float score = std::fabs(predicted - remotePct);

    // If both readers report reasonably close whole-book percentages, the
    // currently open source section is a strong clue. This fixes the common
    // case where pagination/page-break differences shift KOReader by 3-8% but
    // both devices are in the same chapter.
    if (currentSectionOrdinal > 0 && sec.ordinal == currentSectionOrdinal &&
        std::fabs(localPct - remotePct) <= 0.10f) {
      score *= 0.22f;
    }

    // Prefer sections whose weighted byte range actually brackets the remote
    // percentage, but do not require it: page-based percentages can drift.
    if (remotePct >= begin && remotePct <= end) score *= 0.55f;
    if (score < bestScore) { bestScore = score; best = &sec; }
  }
  return best;
}
}  // namespace


std::string ProgressMapper::generateFb2CompatibleXPath(const InkMODPosition& pos,
                                                         int originalSectionOrdinal) {
  // KOReader/CREngine FB2 XPointer paths include the source <section> node,
  // e.g. /body/DocFragment[12]/body/section/p[23]/text().5.  Omitting
  // /section makes GotoXPointer miss the real FB2 DOM even though the server
  // accepts and stores the progress string.
  //
  // Large FB2 sections may be split into several synthetic inkMOD spines.
  // originalSectionOrdinal collapses those slices back to the source-section
  // ordinal before constructing the CRE-compatible path.
  const int fragmentIndex = std::max(1, originalSectionOrdinal);
  const int paragraphIndex = pos.hasParagraphIndex ? std::max(1, static_cast<int>(pos.paragraphIndex)) : 1;
  char buf[96];
  snprintf(buf, sizeof(buf), "/body/DocFragment[%d]/body/section/p[%d]/text().0", fragmentIndex, paragraphIndex);
  return std::string(buf);
}

std::string ProgressMapper::generateFb2SourceXPath(const std::shared_ptr<Epub>& epub,
                                                        const std::string& packageCachePath,
                                                        const InkMODPosition& pos,
                                                        int originalSectionOrdinal) {
  int rangeStart = -1, rangeEnd = -1;
  if (!Fb2::getChapterRangeForOriginalSectionOrdinal(packageCachePath, originalSectionOrdinal, rangeStart, rangeEnd) ||
      rangeStart < 0 || rangeEnd < rangeStart || pos.spineIndex < rangeStart || pos.spineIndex > rangeEnd) {
    return generateFb2CompatibleXPath(pos, originalSectionOrdinal);
  }

  const std::string sourceSection = Fb2::buildCanonicalSourceSectionXPath(packageCachePath, originalSectionOrdinal);
  if (sourceSection.empty()) {
    LOG_ERR("PM", "FB2 canonical source path missing for sourceSection=%d", originalSectionOrdinal);
    return generateFb2CompatibleXPath(pos, originalSectionOrdinal);
  }

  // <subtitle> is rendered as <h3> and is therefore invisible to the paragraph
  // LUT. If it is actually the first visible text on this X4 page, recover its
  // native FB2 XPath directly from the source so KOReader lands on the same
  // subheading instead of the neighbouring prose paragraph.
  if (!pos.topTextSnippet.empty()) {
    std::string subtitleXPath;
    if (findCanonicalSubtitleForTopSnippet(packageCachePath, originalSectionOrdinal,
                                           pos.topTextSnippet, subtitleXPath)) {
      LOG_INF("PM", "FB2 subtitle top-text upload: %s", subtitleXPath.c_str());
      return subtitleXPath;
    }
  }

  // Resolve the position at the START of the current X4 page against the
  // synthetic XHTML.  Its <p> sequence preserves the source FB2 paragraph
  // sequence, so we can translate the local virtual-slice paragraph back to
  // the original section.  The text offset is retained too, giving KOReader
  // a much tighter landing point than paragraph-only sync.
  const float intra = (pos.totalPages > 1)
      ? std::max(0.0f, std::min(1.0f, static_cast<float>(pos.pageNumber) /
                                         static_cast<float>(pos.totalPages - 1)))
      : 0.0f;
  const std::string localXPath = ChapterXPathResolver::findXPathForProgress(epub, pos.spineIndex, intra);
  int localP = parseIndex(localXPath, "/p[", true);
  int charOffset = parseCharOffset(localXPath);
  if (pos.hasParagraphIndex && pos.paragraphIndex > 0) {
    // The reader derives paragraphIndex from the page LUT at the actual page
    // boundary. Prefer that over a page-count fraction, which is only an
    // approximation when page text density varies.
    localP = pos.paragraphIndex;
  }
  if (localP <= 0) localP = 1;

  // Best path: match the first visible words of the real rendered page back
  // into this synthetic paragraph.  This gives the character position at the
  // top of the X4 page instead of pageNumber/totalPages interpolation.
  if (!pos.topTextSnippet.empty()) {
    const int exact = ChapterXPathResolver::findCharOffsetForTextSnippet(
        epub, pos.spineIndex, static_cast<uint16_t>(localP), pos.topTextSnippet);
    if (exact >= 0) {
      charOffset = exact;
      LOG_INF("PM", "FB2 exact top-text upload: spine=%d p=%d char=%d snippet=%s",
              pos.spineIndex, localP, charOffset, pos.topTextSnippet.c_str());
    }
  }

  int sourceRenderOrdinal = 1;
  for (int spine = rangeStart; spine < pos.spineIndex; ++spine) {
    sourceRenderOrdinal += std::max(0, countParagraphsInSpine(epub, spine));
  }
  sourceRenderOrdinal += localP - 1;
  sourceRenderOrdinal = std::max(1, sourceRenderOrdinal);

  // Reverse the flattened render ordinal back to the exact original FB2 XML
  // element. This is what prevents a cite/poem/empty-line earlier in a large
  // chapter from shifting all later KOReader p[N] positions.
  std::string sourceElement;
  if (!renderOrdinalToCanonicalSourceElement(packageCachePath, originalSectionOrdinal,
                                              sourceRenderOrdinal, sourceElement)) {
    // Safe fallback for simple prose sections.
    sourceElement = sourceSection + "/p[" + std::to_string(sourceRenderOrdinal) + "]";
    LOG_ERR("PM", "FB2 reverse source-block map failed; using flat p[%d]", sourceRenderOrdinal);
  }

  std::string out;
  if (sourceElement.find("/empty-line[") != std::string::npos) {
    out = sourceElement + ".0";
  } else {
    out = sourceElement + "/text()." + std::to_string(std::max(0, charOffset));
  }
  LOG_INF("PM", "FB2 canonical upload: sourceSection=%d element=%s virtual=%d range=%d..%d localP=%d char=%d renderP=%d",
          originalSectionOrdinal, sourceElement.c_str(), pos.spineIndex, rangeStart, rangeEnd,
          localP, charOffset, sourceRenderOrdinal);
  return out;
}

KOReaderPosition ProgressMapper::toKOReader(const std::shared_ptr<Epub>& epub, const InkMODPosition& pos) {
  KOReaderPosition result;
  float intra =
      (pos.totalPages > 1) ? static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages - 1) : 0.0f;
  result.percentage = epub->calculateProgress(pos.spineIndex, intra);
  // Progress-based XPath correctly handles both <p> and <li> positions.
  result.xpath = ChapterXPathResolver::findXPathForProgress(epub, pos.spineIndex, intra);
  // Fall back to paragraph-index lookup when progress-based resolution fails.
  if (result.xpath.empty() && pos.hasParagraphIndex && pos.paragraphIndex > 0) {
    result.xpath = ChapterXPathResolver::findXPathForParagraph(epub, pos.spineIndex, pos.paragraphIndex);
  }
  if (result.xpath.empty()) {
    result.xpath = generateXPath(epub, pos.spineIndex, intra);
  }
  LOG_DBG("PM", "-> KO: spine=%d page=%d/%d %.2f%% %s", pos.spineIndex, pos.pageNumber, pos.totalPages,
          result.percentage * 100, result.xpath.c_str());
  return result;
}

InkMODPosition ProgressMapper::toInkMOD(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                                int currentSpineIndex, int totalPagesInCurrentSpine) {
  InkMODPosition result{};
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return result;

  const int spineCount = epub->getSpineItemsCount();
  const float clampedPercentage = std::max(0.0f, std::min(1.0f, koPos.percentage));
  const size_t targetBytes = static_cast<size_t>(static_cast<float>(bookSize) * clampedPercentage);

  const int docFrag = parseIndex(koPos.xpath, "/body/DocFragment[");
  const int xpathP = parseIndex(koPos.xpath, "/p[", true);
  const int xpathChar = parseCharOffset(koPos.xpath);
  const int xpathTextNode = parseTextNodeIndex(koPos.xpath);
  const int xpathSpine = (docFrag >= 1) ? (docFrag - 1) : -1;

  XPathStep xpathSteps[MAX_XPATH_DEPTH];
  const int xpathStepCount = parseXPathSteps(koPos.xpath, xpathSteps);
  // Use ancestry mode whenever the XPath has a structured path (always more accurate than global counting).
  const bool useAncestry = xpathStepCount > 0;

  if (xpathSpine >= 0 && xpathSpine < spineCount) {
    result.spineIndex = xpathSpine;
  } else {
    for (int i = 0; i < spineCount; i++) {
      if (epub->getCumulativeSpineItemSize(i) >= targetBytes) {
        result.spineIndex = i;
        break;
      }
    }
  }
  if (result.spineIndex >= spineCount) return result;

  const size_t prevCum = (result.spineIndex > 0) ? epub->getCumulativeSpineItemSize(result.spineIndex - 1) : 0;
  const size_t spineSize = epub->getCumulativeSpineItemSize(result.spineIndex) - prevCum;

  if (result.spineIndex == currentSpineIndex && totalPagesInCurrentSpine > 0) {
    result.totalPages = totalPagesInCurrentSpine;
  } else if (currentSpineIndex >= 0 && currentSpineIndex < spineCount && totalPagesInCurrentSpine > 0) {
    const size_t pc = (currentSpineIndex > 0) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
    const size_t cs = epub->getCumulativeSpineItemSize(currentSpineIndex) - pc;
    if (cs > 0)
      result.totalPages = std::max(
          1, static_cast<int>(totalPagesInCurrentSpine * static_cast<float>(spineSize) / static_cast<float>(cs)));
  }
  if (spineSize == 0 || result.totalPages == 0) return result;

  float intra = 0.0f;
  bool resolvedIntra = false;
  if (useAncestry) {
    ParagraphStreamer s(xpathSteps, xpathStepCount, xpathChar, xpathTextNode);
    if (streamSpine(epub, result.spineIndex, s) && s.found()) {
      intra = s.progress();
      resolvedIntra = true;
      const int pAtMatch = s.getParagraphAtMatch();
      if (pAtMatch > 0) {
        result.paragraphIndex = static_cast<uint16_t>(pAtMatch);
        result.hasParagraphIndex = true;
      }
      if (xpathStepCount > 0 && strcasecmp(xpathSteps[xpathStepCount - 1].tag, "li") == 0) {
        const int liAtMatch = s.getListItemAtMatch();
        if (liAtMatch > 0) {
          result.liIndex = static_cast<uint16_t>(liAtMatch);
          result.hasLiIndex = true;
        }
      }
      const char* anchorId = s.getCapturedAnchorId();
      if (anchorId) {
        strncpy(result.xpathAnchorId, anchorId, sizeof(result.xpathAnchorId) - 1);
      }
      LOG_DBG("PM", "XPath ancestry(%s[%d])/text()[%d]+%d -> %.1f%% (target=%zu total=%zu p~%d li~%d anchor=%s)",
              xpathSteps[xpathStepCount - 1].tag, xpathSteps[xpathStepCount - 1].siblingIndex, xpathTextNode, xpathChar,
              intra * 100, s.getTargetVisChars(), s.getTotalVisChars(), pAtMatch,
              result.hasLiIndex ? static_cast<int>(result.liIndex) : 0, anchorId ? anchorId : "none");
    }
  } else if (xpathP > 0) {
    ParagraphStreamer s(xpathP, xpathChar, xpathTextNode);
    if (streamSpine(epub, result.spineIndex, s) && s.found()) {
      intra = s.progress();
      resolvedIntra = true;
      LOG_DBG("PM", "XPath p[%d]/text()[%d]+%d -> %.1f%% (target=%zu total=%zu)", xpathP, xpathTextNode, xpathChar,
              intra * 100, s.getTargetVisChars(), s.getTotalVisChars());
    }
  }
  if (!resolvedIntra && xpathSpine >= 0 && xpathSpine < spineCount && isChapterStartXPath(koPos.xpath)) {
    intra = 0.0f;
    resolvedIntra = true;
    LOG_DBG("PM", "Chapter-start XPath %s -> spine=%d page start", koPos.xpath.c_str(), result.spineIndex);
  }
  if (!resolvedIntra) {
    const size_t bytesIn = (targetBytes > prevCum) ? (targetBytes - prevCum) : 0;
    intra = std::max(0.0f, std::min(1.0f, static_cast<float>(bytesIn) / static_cast<float>(spineSize)));
  }

  result.pageNumber = std::max(
      0, std::min(static_cast<int>(intra * static_cast<float>(result.totalPages - 1) + 0.5f), result.totalPages - 1));
  LOG_DBG("PM", "<- KO: %.2f%% %s -> spine=%d page=%d/%d", koPos.percentage * 100, koPos.xpath.c_str(),
          result.spineIndex, result.pageNumber, result.totalPages);
  return result;
}

InkMODPosition ProgressMapper::toInkMODFb2(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                                   const std::string& packageCachePath, int currentSpineIndex,
                                                   int totalPagesInCurrentSpine) {
  InkMODPosition result{};
  if (!epub) return result;
  const int spineCount = epub->getSpineItemsCount();
  const size_t bookSize = epub->getBookSize();
  if (spineCount <= 0 || bookSize == 0) return result;

  const float pct = std::max(0.0f, std::min(1.0f, koPos.percentage));
  // Preferred path: KOReader gave us the canonical source FB2 XPath.
  // This is exact at the XML level and does not depend on page percentages or
  // CREngine's internal DocFragment numbering.
  uint16_t sourceBody = 1;
  std::vector<uint16_t> sourceSections;
  int sourceParagraphExact = 0;
  int sourceCharExact = 0;
  if (parseCanonicalFb2SourceXPath(koPos.xpath, sourceBody, sourceSections,
                                  sourceParagraphExact, sourceCharExact)) {
    int sourceOrdinal = 0;
    if (Fb2::findOriginalSectionBySourcePath(packageCachePath, sourceBody, sourceSections, sourceOrdinal)) {
      int rangeStart = -1, rangeEnd = -1;
      if (Fb2::getChapterRangeForOriginalSectionOrdinal(packageCachePath, sourceOrdinal, rangeStart, rangeEnd) &&
          rangeStart >= 0 && rangeEnd >= rangeStart) {
        // KOReader's p[N] is a sibling index in the ORIGINAL FB2 XML.
        // inkMOD's page LUT counts flattened rendered <p>s, including nested
        // cite paragraphs, verse lines, <empty-line/> and text-author blocks.
        // Translate the complete canonical XML tail to that flattened ordinal
        // before selecting a virtual spine.
        int sourceRenderOrdinal = 0;
        if (!canonicalSourceToRenderOrdinal(packageCachePath, koPos.xpath, sourceBody,
                                            sourceSections, sourceRenderOrdinal)) {
          // Old/plain books usually have a 1:1 mapping; retain that only as a
          // fallback when the source bridge cannot be built.
          sourceRenderOrdinal = std::max(1, sourceParagraphExact);
          LOG_ERR("PM", "FB2 source-block map failed; falling back p=%d", sourceParagraphExact);
        }

        int remainingP = std::max(1, sourceRenderOrdinal);
        int chosen = rangeStart;
        int localP = 1;
        for (int spine = rangeStart; spine <= rangeEnd; ++spine) {
          const int count = std::max(1, countParagraphsInSpine(epub, spine));
          if (remainingP <= count || spine == rangeEnd) {
            chosen = spine;
            localP = std::max(1, std::min(remainingP, count));
            break;
          }
          remainingP -= count;
        }

        result.spineIndex = chosen;
        const size_t prevCum = chosen > 0 ? epub->getCumulativeSpineItemSize(chosen - 1) : 0;
        const size_t spineSize = epub->getCumulativeSpineItemSize(chosen) - prevCum;
        if (chosen == currentSpineIndex && totalPagesInCurrentSpine > 0) {
          result.totalPages = totalPagesInCurrentSpine;
        } else if (currentSpineIndex >= 0 && currentSpineIndex < spineCount && totalPagesInCurrentSpine > 0) {
          const size_t curPrev = currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
          const size_t curSize = epub->getCumulativeSpineItemSize(currentSpineIndex) - curPrev;
          if (curSize > 0) result.totalPages = std::max(1, static_cast<int>(
              static_cast<float>(totalPagesInCurrentSpine) * static_cast<float>(spineSize) /
              static_cast<float>(curSize)));
        }
        if (result.totalPages <= 0) result.totalPages = 1;

        float intra = 0.0f;
        ParagraphStreamer ps(localP, std::max(0, sourceCharExact), 1);
        if (streamSpine(epub, chosen, ps) && ps.found()) {
          intra = ps.progress();
          result.paragraphIndex = static_cast<uint16_t>(localP);
          result.hasParagraphIndex = true;
          result.paragraphCharOffset = static_cast<uint32_t>(std::max(0, sourceCharExact));
          result.paragraphCharCount = static_cast<uint32_t>(
              ChapterXPathResolver::countVisibleCharsInParagraph(epub, chosen, static_cast<uint16_t>(localP)));
          result.hasParagraphCharOffset = result.paragraphCharCount > 0;
        } else {
          const int count = std::max(1, countParagraphsInSpine(epub, chosen));
          intra = count > 1 ? static_cast<float>(localP - 1) / static_cast<float>(count - 1) : 0.0f;
        }
        result.pageNumber = std::max(0, std::min(
            static_cast<int>(intra * static_cast<float>(result.totalPages - 1) + 0.5f),
            result.totalPages - 1));

        std::string secPath;
        for (const uint16_t idx : sourceSections) secPath += "/section[" + std::to_string(idx) + "]";
        LOG_INF("PM", "<- KO FB2 canonical: body=%u%s p=%d renderP=%d char=%d/%u -> sourceSection=%d range=%d..%d spine=%d localP=%d page=%d/%d (remote %.2f%% ignored)",
                sourceBody, secPath.c_str(), sourceParagraphExact, sourceRenderOrdinal, sourceCharExact,
                static_cast<unsigned>(result.paragraphCharCount), sourceOrdinal,
                rangeStart, rangeEnd, chosen, localP, result.pageNumber, result.totalPages,
                koPos.percentage * 100.0f);
        return result;
      }
    }
    LOG_ERR("PM", "Canonical FB2 XPath could not be mapped: %s", koPos.xpath.c_str());
  }

  const int docFrag = parseIndex(koPos.xpath, "/body/DocFragment[");
  const int xpathP = parseIndex(koPos.xpath, "/p[", true);
  const int xpathChar = parseCharOffset(koPos.xpath);
  const int xpathTextNode = parseTextNodeIndex(koPos.xpath);

  int currentSectionOrdinal = -1;
  if (currentSpineIndex >= 0) Fb2::getOriginalSectionOrdinal(packageCachePath, currentSpineIndex, currentSectionOrdinal);
  const auto sections = collectLogicalSections(epub, packageCachePath);
  float currentApproxPct = -10.0f;
  if (currentSectionOrdinal > 0) {
    for (const auto& sec : sections) {
      if (sec.ordinal == currentSectionOrdinal) {
        currentApproxPct = static_cast<float>(sec.beginBytes + (sec.endBytes - sec.beginBytes) / 2) /
                           static_cast<float>(bookSize);
        break;
      }
    }
  }
  const int learnedSection = learnedSectionForFragment(packageCachePath, docFrag);
  const Fb2LogicalSection* selected = chooseSectionForRemote(
      sections, bookSize, pct, xpathP, currentSectionOrdinal, currentApproxPct, learnedSection);

  if (!selected) {
    // Last-resort compatibility path.
    const size_t targetBytes = static_cast<size_t>(static_cast<double>(bookSize) * pct);
    int chosen = 0;
    for (int i = 0; i < spineCount; ++i) {
      if (epub->getCumulativeSpineItemSize(i) >= targetBytes) { chosen = i; break; }
    }
    result.spineIndex = chosen;
    result.totalPages = (chosen == currentSpineIndex && totalPagesInCurrentSpine > 0) ? totalPagesInCurrentSpine : 1;
    result.pageNumber = 0;
    LOG_DBG("PM", "<- KO FB2 CRE fallback: %.2f%% docFrag=%d -> spine=%d", pct * 100.0f, docFrag, chosen);
    return result;
  }

  if (docFrag > 0 && learnedSection != selected->ordinal) {
    rememberFragMap(packageCachePath, docFrag, selected->ordinal);
    LOG_INF("PM", "FB2 CRE calibration learned: DocFragment[%d] -> sourceSection=%d", docFrag, selected->ordinal);
  }

  // Convert the remote position into a chapter-local coordinate. If the
  // XPointer is a simple direct section/p[N] path, p[N] is a useful structural
  // hint. For nested CREngine paths, p[N] is only a sibling index inside its
  // parent, so use text-weighted chapter progress instead of pretending it is
  // a global paragraph number.
  const float secBeginPct = static_cast<float>(selected->beginBytes) / static_cast<float>(bookSize);
  const float secEndPct = static_cast<float>(selected->endBytes) / static_cast<float>(bookSize);
  float chapterFrac = (secEndPct > secBeginPct)
      ? (pct - secBeginPct) / (secEndPct - secBeginPct) : 0.0f;
  chapterFrac = std::max(0.0f, std::min(1.0f, chapterFrac));

  const std::string directNeedle = "/body/section/p[";
  const size_t bodyPos = koPos.xpath.find("]/body/");
  const bool simpleDirectP = bodyPos != std::string::npos &&
      koPos.xpath.find(directNeedle, bodyPos) != std::string::npos &&
      koPos.xpath.find("/section/", koPos.xpath.find(directNeedle, bodyPos) + directNeedle.size()) == std::string::npos;
  int sourceP = 1;
  if (simpleDirectP && xpathP > 0) {
    sourceP = std::max(1, std::min(xpathP, selected->paragraphs));
    const float pFrac = selected->paragraphs > 1
        ? static_cast<float>(sourceP - 1) / static_cast<float>(selected->paragraphs - 1) : 0.0f;
    // Structural paragraph beats page-derived percentage, but retain a small
    // percentage contribution to tolerate CREngine-generated wrapper nodes.
    chapterFrac = 0.82f * pFrac + 0.18f * chapterFrac;
  } else {
    sourceP = selected->paragraphs > 1
        ? 1 + static_cast<int>(chapterFrac * static_cast<float>(selected->paragraphs - 1) + 0.5f)
        : 1;
  }

  int remainingP = sourceP;
  int chosen = selected->startSpine;
  int localP = 1;
  for (int spine = selected->startSpine; spine <= selected->endSpine; ++spine) {
    const int count = std::max(1, countParagraphsInSpine(epub, spine));
    if (remainingP <= count || spine == selected->endSpine) {
      chosen = spine;
      localP = std::max(1, std::min(remainingP, count));
      break;
    }
    remainingP -= count;
  }

  result.spineIndex = chosen;
  const size_t prevCum = chosen > 0 ? epub->getCumulativeSpineItemSize(chosen - 1) : 0;
  const size_t spineSize = epub->getCumulativeSpineItemSize(chosen) - prevCum;
  if (chosen == currentSpineIndex && totalPagesInCurrentSpine > 0) {
    result.totalPages = totalPagesInCurrentSpine;
  } else if (currentSpineIndex >= 0 && currentSpineIndex < spineCount && totalPagesInCurrentSpine > 0) {
    const size_t curPrev = currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
    const size_t curSize = epub->getCumulativeSpineItemSize(currentSpineIndex) - curPrev;
    if (curSize > 0) result.totalPages = std::max(1, static_cast<int>(
        static_cast<float>(totalPagesInCurrentSpine) * static_cast<float>(spineSize) / static_cast<float>(curSize)));
  }
  if (result.totalPages <= 0) result.totalPages = 1;

  float intra = 0.0f;
  ParagraphStreamer ps(localP, simpleDirectP ? xpathChar : 0, xpathTextNode);
  if (streamSpine(epub, chosen, ps) && ps.found()) {
    intra = ps.progress();
    result.paragraphIndex = static_cast<uint16_t>(localP);
    result.hasParagraphIndex = true;
  } else {
    const int count = std::max(1, countParagraphsInSpine(epub, chosen));
    intra = count > 1 ? static_cast<float>(localP - 1) / static_cast<float>(count - 1) : 0.0f;
  }
  result.pageNumber = std::max(0, std::min(
      static_cast<int>(intra * static_cast<float>(result.totalPages - 1) + 0.5f), result.totalPages - 1));

  LOG_INF("PM", "<- KO FB2 CRE: docFrag=%d%s -> sourceSection=%d range=%d..%d sourceP=%d/%d spine=%d localP=%d page=%d/%d remote=%.2f%% chapter=%.1f%%",
          docFrag, learnedSection > 0 ? " learned" : " calibrated", selected->ordinal,
          selected->startSpine, selected->endSpine, sourceP, selected->paragraphs,
          chosen, localP, result.pageNumber, result.totalPages, pct * 100.0f, chapterFrac * 100.0f);
  return result;
}

std::string ProgressMapper::generateXPath(const std::shared_ptr<Epub>& epub, int spineIndex, float intra) {
  const std::string base = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  if (intra <= 0.0f) return base;

  size_t spineSize = 0;
  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty() || !epub->getItemSize(href, &spineSize) || spineSize == 0) return base;

  ParagraphStreamer s(static_cast<size_t>(spineSize * std::min(intra, 1.0f)));
  if (!streamSpine(epub, spineIndex, s)) return base;

  const int p = s.paragraphCount();
  return (p > 0) ? base + "/p[" + std::to_string(p) + "]" : base;
}
