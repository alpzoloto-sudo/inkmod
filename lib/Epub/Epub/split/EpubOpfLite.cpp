#include "EpubOpfLite.h"

#include <string_view>

namespace {

// EpubOpfLite is used only by the optional oversized-chapter pre-splitter.
// Pulling in Expat here costs a surprisingly large transient heap block on
// ESP32-C3, even though content.opf itself is usually only a few KiB.
//
// This tiny scanner intentionally understands only START tags and the few
// attributes the splitter needs. The normal EPUB reader still uses its full
// parser later; this does not replace normal EPUB XML handling.

std::string_view localName(std::string_view name) {
  const size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

bool isNameChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.';
}

std::string getAttr(std::string_view tag, std::string_view wanted) {
  // Skip the element name.
  size_t p = 1;
  while (p < tag.size() && tag[p] != '>' && tag[p] != '/' &&
         tag[p] != ' ' && tag[p] != '\t' && tag[p] != '\r' && tag[p] != '\n') {
    ++p;
  }

  while (p < tag.size()) {
    while (p < tag.size() &&
           (tag[p] == ' ' || tag[p] == '\t' || tag[p] == '\r' || tag[p] == '\n' || tag[p] == '/')) {
      ++p;
    }
    if (p >= tag.size() || tag[p] == '>') break;

    const size_t nameStart = p;
    while (p < tag.size() && isNameChar(tag[p])) ++p;
    if (p == nameStart) {
      ++p;
      continue;
    }

    std::string_view name = localName(tag.substr(nameStart, p - nameStart));

    while (p < tag.size() &&
           (tag[p] == ' ' || tag[p] == '\t' || tag[p] == '\r' || tag[p] == '\n')) {
      ++p;
    }
    if (p >= tag.size() || tag[p] != '=') {
      while (p < tag.size() && tag[p] != '>' && tag[p] != ' ') ++p;
      continue;
    }
    ++p;

    while (p < tag.size() &&
           (tag[p] == ' ' || tag[p] == '\t' || tag[p] == '\r' || tag[p] == '\n')) {
      ++p;
    }
    if (p >= tag.size()) break;

    const char quote = tag[p];
    if (quote != '"' && quote != '\'') {
      while (p < tag.size() && tag[p] != '>' && tag[p] != ' ') ++p;
      continue;
    }
    ++p;
    const size_t valueStart = p;
    const size_t valueEnd = tag.find(quote, p);
    if (valueEnd == std::string_view::npos) break;

    if (name == wanted) {
      return std::string(tag.substr(valueStart, valueEnd - valueStart));
    }
    p = valueEnd + 1;
  }

  return {};
}

}  // namespace

bool EpubOpfLite::parse(const std::string& opfContent, EpubOpfLite& out) {
  out = EpubOpfLite{};

  const std::string_view xml(opfContent);
  size_t p = 0;

  while ((p = xml.find('<', p)) != std::string_view::npos) {
    if (p + 1 >= xml.size()) break;

    // Skip declarations, comments, DOCTYPE and closing tags.
    const char next = xml[p + 1];
    if (next == '?' || next == '!' || next == '/') {
      const size_t end = xml.find('>', p + 1);
      if (end == std::string_view::npos) return false;
      p = end + 1;
      continue;
    }

    size_t nameStart = p + 1;
    size_t nameEnd = nameStart;
    while (nameEnd < xml.size() && isNameChar(xml[nameEnd])) ++nameEnd;
    if (nameEnd == nameStart) {
      ++p;
      continue;
    }

    const size_t tagEnd = xml.find('>', nameEnd);
    if (tagEnd == std::string_view::npos) return false;

    const std::string_view tag = xml.substr(p, tagEnd - p + 1);
    const std::string_view element = localName(xml.substr(nameStart, nameEnd - nameStart));

    if (element == "item") {
      std::string id = getAttr(tag, "id");
      std::string href = getAttr(tag, "href");
      if (!id.empty() && !href.empty()) {
        out.manifest.push_back({std::move(id), std::move(href), getAttr(tag, "media-type")});
      }
    } else if (element == "itemref") {
      std::string idref = getAttr(tag, "idref");
      if (!idref.empty()) out.spineIdrefs.push_back(std::move(idref));
    } else if (element == "spine") {
      out.tocNcxItemId = getAttr(tag, "toc");
    }

    p = tagEnd + 1;
  }

  // Match the old parser contract: syntactically scan-able OPF is a success;
  // caller separately decides whether manifest/spine contain enough data.
  return true;
}
