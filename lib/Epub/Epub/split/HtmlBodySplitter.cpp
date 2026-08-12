#include "HtmlBodySplitter.h"

#include <cctype>

namespace {

// Returns the tag name starting at `pos` (right after '<' or "</"), and
// advances `pos` past it. Tag names are [a-zA-Z][a-zA-Z0-9:_-]*.
std::string readTagName(const std::string& s, size_t& pos) {
  const size_t start = pos;
  while (pos < s.size() &&
         (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == ':' || s[pos] == '_' || s[pos] == '-')) {
    ++pos;
  }
  return s.substr(start, pos - start);
}

bool isVoidElement(const std::string& name) {
  // HTML5 void elements: never have a closing tag, never affect depth,
  // even if not self-closed with "/>" - be defensive here since this is a
  // depth tracker, not a validating parser, and XHTML always self-closes
  // these anyway (so this mostly guards against slightly-off-spec input).
  static const char* const kVoid[] = {"area", "base",  "br",    "col",   "embed",  "hr",     "img",
                                       "input", "link", "meta",  "param", "source", "track", "wbr"};
  for (const char* v : kVoid) {
    if (name == v) return true;
  }
  return false;
}

}  // namespace

bool HtmlBodySplitter::split(const std::string& bodyContent, size_t targetChunkBytes, std::vector<Chunk>& outChunks) {
  outChunks.clear();
  const size_t n = bodyContent.size();
  size_t i = 0;
  int depth = 0;
  size_t chunkStart = 0;
  Chunk current;

  const auto flushChunk = [&](size_t endPos) {
    current.html = bodyContent.substr(chunkStart, endPos - chunkStart);
    outChunks.push_back(std::move(current));
    current = Chunk{};
    chunkStart = endPos;
  };

  while (i < n) {
    if (bodyContent[i] != '<') {
      ++i;
      continue;
    }

    // Comment: <!-- ... --> - opaque, never affects depth or gets scanned
    // for tags/ids inside it.
    if (bodyContent.compare(i, 4, "<!--") == 0) {
      const size_t end = bodyContent.find("-->", i + 4);
      i = (end == std::string::npos) ? n : end + 3;
      continue;
    }
    // DOCTYPE / processing instruction: <! ... > or <? ... ?> - likewise
    // opaque (shouldn't really appear inside a <body>, but be defensive).
    if (i + 1 < n && (bodyContent[i + 1] == '!' || bodyContent[i + 1] == '?')) {
      const size_t end = bodyContent.find('>', i + 1);
      i = (end == std::string::npos) ? n : end + 1;
      continue;
    }

    const bool isClosing = (i + 1 < n && bodyContent[i + 1] == '/');
    const size_t tagStart = i + (isClosing ? 2 : 1);
    size_t namePos = tagStart;
    const std::string tagName = readTagName(bodyContent, namePos);
    if (tagName.empty()) {
      // Not actually a tag (e.g. a stray '<' in text, already-escaped
      // content aside) - treat as ordinary text and move on one char.
      ++i;
      continue;
    }

    // Find the end of this tag (the unquoted '>'), tracking attribute
    // quoting so a '>' inside an attribute value doesn't end the tag early
    // (e.g. <a title="5 > 3">).
    size_t p = namePos;
    bool selfClosing = false;
    char quote = '\0';
    while (p < n) {
      const char c = bodyContent[p];
      if (quote != '\0') {
        if (c == quote) quote = '\0';
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '>') {
        selfClosing = (p > 0 && bodyContent[p - 1] == '/');
        break;
      }
      ++p;
    }
    if (p >= n) {
      // Unterminated tag - the document ends mid-tag. Stop scanning here;
      // whatever's accumulated so far becomes the last chunk below.
      break;
    }

    // id="..." extraction (opening tags only - a closing tag can't carry
    // a fresh id, and self-closing tags are handled by this same check
    // since isClosing is false for them too).
    if (!isClosing) {
      const size_t idPos = bodyContent.find("id=", namePos);
      if (idPos != std::string::npos && idPos < p) {
        const size_t q = idPos + 3;
        if (q < p && (bodyContent[q] == '"' || bodyContent[q] == '\'')) {
          const char qc = bodyContent[q];
          const size_t vStart = q + 1;
          const size_t vEnd = bodyContent.find(qc, vStart);
          if (vEnd != std::string::npos && vEnd < p) {
            current.idsInChunk.push_back(bodyContent.substr(vStart, vEnd - vStart));
          }
        }
      }
    }

    const size_t tagEnd = p + 1;  // one past the '>'

    // <script>/<style> content is opaque - it can contain '<'/'>' that
    // aren't markup at all (comparison operators, CSS selectors). Skip
    // straight to the real closing tag instead of tokenizing through it.
    if (!isClosing && !selfClosing && (tagName == "script" || tagName == "style")) {
      const std::string closer = "</" + tagName;
      const size_t closePos = bodyContent.find(closer, tagEnd);
      if (closePos == std::string::npos) {
        i = n;
      } else {
        const size_t closeTagEnd = bodyContent.find('>', closePos);
        i = (closeTagEnd == std::string::npos) ? n : closeTagEnd + 1;
      }
      continue;  // never itself changes depth
    }

    if (isClosing) {
      if (depth > 0) --depth;
      if (depth == 0 && tagEnd - chunkStart >= targetChunkBytes) {
        // Just closed a top-level element and we're over budget - this is
        // a safe boundary (we're back at direct-<body>-child level, not
        // inside anything), so end the chunk right here.
        flushChunk(tagEnd);
      }
    } else if (!selfClosing && !isVoidElement(tagName)) {
      ++depth;
    } else if (depth == 0 && tagEnd - chunkStart >= targetChunkBytes) {
      // A self-closing or void element that is itself a direct top-level
      // child (depth was already 0 going in) - equally safe to end on.
      flushChunk(tagEnd);
    }

    i = tagEnd;
  }

  if (chunkStart < n) {
    current.html = bodyContent.substr(chunkStart, n - chunkStart);
    outChunks.push_back(std::move(current));
  }

  return !outChunks.empty();
}
