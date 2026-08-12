#include "EpubOpfRewriter.h"

#include <algorithm>
#include <cstring>

namespace EpubOpfRewriter {

namespace {

// Finds attrName="value" anywhere within tagText (order-independent - real
// OPF files don't agree on attribute order) and returns value, or "" if
// not present. Doesn't handle single-quoted attributes (href="" is
// effectively universal in EPUB manifests in practice) - good enough for
// the one thing this reads, not a general HTML/XML attribute parser.
std::string findAttr(const std::string& tagText, const std::string& attrName) {
  const std::string needle = attrName + "=\"";
  const size_t pos = tagText.find(needle);
  if (pos == std::string::npos) return "";
  const size_t valueStart = pos + needle.size();
  const size_t valueEnd = tagText.find('"', valueStart);
  if (valueEnd == std::string::npos) return "";
  return tagText.substr(valueStart, valueEnd - valueStart);
}

// Locates a self-closing-or-not "<tagName ...>" or "<tagName .../>" whose
// attributes (searched via findAttr) contain attrName="attrValue", and
// returns [start, end) covering the whole tag (through the matching '>').
// Returns {npos, npos} if not found. Doesn't handle a tag with '>' inside
// a quoted attribute value (not something any real EPUB manifest does -
// hrefs/ids don't contain '>').
std::pair<size_t, size_t> findTagByAttr(const std::string& xml, const std::string& tagName,
                                        const std::string& attrName, const std::string& attrValue) {
  const std::string openNeedle = "<" + tagName;
  size_t searchFrom = 0;
  for (;;) {
    const size_t tagStart = xml.find(openNeedle, searchFrom);
    if (tagStart == std::string::npos) return {std::string::npos, std::string::npos};
    // Make sure this is a real tag start, not e.g. "<itemrefx" matching "<item".
    const char afterName = tagStart + openNeedle.size() < xml.size() ? xml[tagStart + openNeedle.size()] : '\0';
    if (afterName != ' ' && afterName != '\t' && afterName != '\n' && afterName != '\r' && afterName != '/' &&
        afterName != '>') {
      searchFrom = tagStart + openNeedle.size();
      continue;
    }
    const size_t tagEnd = xml.find('>', tagStart);
    if (tagEnd == std::string::npos) return {std::string::npos, std::string::npos};
    const std::string tagText = xml.substr(tagStart, tagEnd - tagStart + 1);
    if (findAttr(tagText, attrName) == attrValue) {
      return {tagStart, tagEnd + 1};
    }
    searchFrom = tagEnd + 1;
  }
}

std::string xmlEscapeAttr(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

}  // namespace

std::string rewriteForSplitItem(const std::string& opfContent, const std::string& originalHref,
                                const std::vector<std::string>& fragmentHrefs, std::string* originalItemIdOut) {
  if (fragmentHrefs.empty()) return "";

  const auto [itemStart, itemEnd] = findTagByAttr(opfContent, "item", "href", originalHref);
  if (itemStart == std::string::npos) return "";
  const std::string itemTag = opfContent.substr(itemStart, itemEnd - itemStart);
  const std::string itemId = findAttr(itemTag, "id");
  const std::string mediaType = findAttr(itemTag, "media-type");
  if (itemId.empty()) return "";
  if (originalItemIdOut) *originalItemIdOut = itemId;

  const auto [refStart, refEnd] = findTagByAttr(opfContent, "itemref", "idref", itemId);
  if (refStart == std::string::npos) return "";
  const std::string refTag = opfContent.substr(refStart, refEnd - refStart);
  // Anything on the original itemref besides idref (commonly just
  // linear="yes"/"no") gets copied onto every fragment's itemref -
  // duplicating "linear" across fragments is harmless (it's just a
  // per-item flag) and preserves whatever the original spine intended.
  const size_t idrefAttrPos = refTag.find("idref=\"");
  const size_t idrefValueEnd = idrefAttrPos == std::string::npos ? std::string::npos
                                                                  : refTag.find('"', idrefAttrPos + 7);
  std::string refExtraAttrsBefore, refExtraAttrsAfter;
  if (idrefAttrPos != std::string::npos && idrefValueEnd != std::string::npos) {
    refExtraAttrsBefore = refTag.substr(strlen("<itemref"), idrefAttrPos - strlen("<itemref"));
    const size_t afterQuote = idrefValueEnd + 1;
    // Strip the trailing "/>" or ">" from what's left, we re-add our own.
    size_t tailEnd = refTag.size();
    while (tailEnd > afterQuote && (refTag[tailEnd - 1] == '>' || refTag[tailEnd - 1] == '/')) --tailEnd;
    refExtraAttrsAfter = refTag.substr(afterQuote, tailEnd - afterQuote);
  }

  std::string newItems, newRefs;
  for (size_t i = 0; i < fragmentHrefs.size(); ++i) {
    const std::string fragId = itemId + "_split" + std::to_string(i);
    newItems += "<item id=\"" + xmlEscapeAttr(fragId) + "\" href=\"" + xmlEscapeAttr(fragmentHrefs[i]) + "\"";
    if (!mediaType.empty()) newItems += " media-type=\"" + xmlEscapeAttr(mediaType) + "\"";
    newItems += "/>";
    newRefs += "<itemref" + refExtraAttrsBefore + "idref=\"" + xmlEscapeAttr(fragId) + "\"" + refExtraAttrsAfter +
               "/>";
  }

  // Apply the itemref replacement first: it comes later in the file than
  // the manifest item (spine always follows manifest in a valid OPF), so
  // replacing it doesn't shift the earlier item's offsets.
  std::string result = opfContent;
  result.replace(refStart, refEnd - refStart, newRefs);
  result.replace(itemStart, itemEnd - itemStart, newItems);
  return result;
}

}  // namespace EpubOpfRewriter

namespace EpubNcxRewriter {

std::string redirectReferences(const std::string& ncxContent, const std::string& originalHref,
                               const std::vector<std::string>& fragmentHrefs,
                               const std::unordered_map<std::string, int>& anchorFragment) {
  if (fragmentHrefs.empty()) return ncxContent;

  std::string result;
  result.reserve(ncxContent.size());
  const std::string needle = "src=\"" + originalHref;
  size_t pos = 0;
  while (true) {
    const size_t found = ncxContent.find(needle, pos);
    if (found == std::string::npos) {
      result.append(ncxContent, pos, std::string::npos);
      break;
    }
    result.append(ncxContent, pos, found - pos);
    // What follows the original href is either '"' (no anchor) or
    // '#anchor"' - either way, up to the next '"' is ours to replace.
    const size_t valueEnd = ncxContent.find('"', found + 5);
    if (valueEnd == std::string::npos) {
      // Malformed - bail out rather than guess, leaving the rest verbatim.
      result.append(ncxContent, found, std::string::npos);
      break;
    }
    const std::string rest = ncxContent.substr(found + 5 + originalHref.size(), valueEnd - (found + 5 + originalHref.size()));
    int fragmentIndex = 0;
    std::string anchor;
    if (!rest.empty() && rest[0] == '#') {
      anchor = rest.substr(1);
      const auto it = anchorFragment.find(anchor);
      if (it != anchorFragment.end()) fragmentIndex = it->second;
    }
    fragmentIndex = std::min(fragmentIndex, static_cast<int>(fragmentHrefs.size()) - 1);
    result += "src=\"" + fragmentHrefs[static_cast<size_t>(fragmentIndex)];
    if (!anchor.empty()) result += "#" + anchor;
    pos = valueEnd;
  }
  return result;
}

}  // namespace EpubNcxRewriter
