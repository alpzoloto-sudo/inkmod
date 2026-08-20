#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"

// inkMOD Classic Book profile
//
// This is deliberately applied after publisher stylesheet + inline style
// resolution.  It therefore behaves like the small set of !important rules
// we want for an e-ink book layout, while leaving font sizes, inline emphasis,
// links, images, lists, tables, RTL direction, superscript/subscript and other
// author semantics intact.
namespace inkmod_book_style {

inline char asciiLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool classHasToken(std::string_view classes, std::string_view token) {
  size_t pos = 0;
  while (pos < classes.size()) {
    while (pos < classes.size() &&
           (classes[pos] == ' ' || classes[pos] == '\t' || classes[pos] == '\r' ||
            classes[pos] == '\n' || classes[pos] == '\f')) {
      ++pos;
    }
    const size_t start = pos;
    while (pos < classes.size() &&
           classes[pos] != ' ' && classes[pos] != '\t' && classes[pos] != '\r' &&
           classes[pos] != '\n' && classes[pos] != '\f') {
      ++pos;
    }
    const size_t length = pos - start;
    if (length != token.size()) continue;
    bool same = true;
    for (size_t i = 0; i < length; ++i) {
      if (asciiLower(classes[start + i]) != asciiLower(token[i])) {
        same = false;
        break;
      }
    }
    if (same) return true;
  }
  return false;
}

inline bool ancestorHasClass(const std::vector<CssAncestorEntry>& ancestors, std::string_view token) {
  for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
    if (classHasToken(it->classAttr, token)) return true;
  }
  return false;
}

inline bool ancestorHasTag(const std::vector<CssAncestorEntry>& ancestors, std::string_view tag) {
  for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
    if (it->tag == tag) return true;
  }
  return false;
}

inline void setLength(CssLength& target, uint32_t& flag, float value) {
  target = CssLength(value, CssUnit::Em);
  flag = 1;
}

inline void setLayout(CssStyle& style, CssTextAlign align, float indent,
                      float top, float bottom, float left, float right) {
  style.textAlign = align;
  style.defined.textAlign = 1;
  setLength(style.textIndent, style.defined.textIndent, indent);
  setLength(style.marginTop, style.defined.marginTop, top);
  setLength(style.marginBottom, style.defined.marginBottom, bottom);
  setLength(style.marginLeft, style.defined.marginLeft, left);
  setLength(style.marginRight, style.defined.marginRight, right);

  // Publisher padding is one of the biggest sources of mysterious blank space
  // on a 480x800 screen. The structural spacing above is the only vertical
  // spacing the classic profile needs.
  setLength(style.paddingTop, style.defined.paddingTop, 0.0f);
  setLength(style.paddingBottom, style.defined.paddingBottom, 0.0f);
  setLength(style.paddingLeft, style.defined.paddingLeft, 0.0f);
  setLength(style.paddingRight, style.defined.paddingRight, 0.0f);
}

inline void setBoldNormal(CssStyle& style) {
  style.fontWeight = CssFontWeight::Bold;
  style.defined.fontWeight = 1;
  style.fontStyle = CssFontStyle::Normal;
  style.defined.fontStyle = 1;
}

inline void setItalic(CssStyle& style) {
  style.fontStyle = CssFontStyle::Italic;
  style.defined.fontStyle = 1;
}

inline void apply(const char* tagName, const std::string& classAttr,
                  const std::vector<CssAncestorEntry>& ancestors, CssStyle& style) {
  const std::string_view tag = tagName ? std::string_view(tagName) : std::string_view{};
  const auto has = [&](std::string_view token) { return classHasToken(classAttr, token); };
  const auto inClass = [&](std::string_view token) { return ancestorHasClass(ancestors, token); };

  const bool heading = tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" ||
                       tag == "h5" || tag == "h6";
  const bool subtitle = has("subtitle") || has("title-line") || has("subheading");
  const bool poemContainer = has("poem") || has("poetry");
  const bool stanzaContainer = has("stanza") || has("stanza-break");
  const bool quoteContainer = tag == "blockquote" || has("cite") || has("quote") || has("epigraph");

  const bool inPoem = poemContainer || stanzaContainer || inClass("poem") || inClass("poetry") ||
                      inClass("stanza") || inClass("stanza-break");
  const bool inQuote = quoteContainer || ancestorHasTag(ancestors, "blockquote") || inClass("cite") ||
                       inClass("quote") || inClass("epigraph");

  // Structural containers first.  Their horizontal inset survives into child
  // blocks through the existing BlockStyle stack.
  if (heading || subtitle) {
    setLayout(style, CssTextAlign::Center, 0.0f, 1.0f, 0.5f, 0.0f, 0.0f);
    setBoldNormal(style);
  } else if (quoteContainer) {
    setLayout(style, CssTextAlign::Justify, 0.0f, 0.6f, 0.6f, 1.2f, 1.2f);
    setItalic(style);
  } else if (poemContainer) {
    setLayout(style, CssTextAlign::Left, 0.0f, 0.6f, 0.6f, 1.2f, 1.2f);
  } else if (stanzaContainer) {
    setLayout(style, CssTextAlign::Left, 0.0f, 0.0f, 0.6f, 0.0f, 0.0f);
  }

  // Normal paragraph rhythm: no extra vertical gap, 1.5em red line and
  // justified text.  Special book structures override this below.
  if (tag != "p") return;

  if (subtitle) {
    return;  // already handled as a centered bold structural line
  }
  if (has("text-author")) {
    setLayout(style, CssTextAlign::Right, 0.0f, 0.3f, 0.0f, 0.0f, 0.0f);
    setItalic(style);
    return;
  }
  if (has("date")) {
    setLayout(style, CssTextAlign::Center, 0.0f, 0.2f, 0.2f, 0.0f, 0.0f);
    setItalic(style);
    return;
  }
  if (has("empty-line")) {
    setLayout(style, CssTextAlign::Left, 0.0f, 0.6f, 0.0f, 0.0f, 0.0f);
    return;
  }

  const bool verseLine = has("v") || has("verse") || has("line");
  if (verseLine || inPoem) {
    setLayout(style, CssTextAlign::Left, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    return;
  }

  if (inQuote) {
    setLayout(style, CssTextAlign::Justify, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    setItalic(style);
    return;
  }

  // Notes are intentionally compact. A note label/number should not look like
  // the first line of a new prose paragraph.
  if (has("note-label")) {
    setLayout(style, CssTextAlign::Left, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    style.fontWeight = CssFontWeight::Bold;
    style.defined.fontWeight = 1;
    return;
  }
  if (inClass("note")) {
    setLayout(style, CssTextAlign::Left, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    return;
  }

  setLayout(style, CssTextAlign::Justify, 1.5f, 0.0f, 0.0f, 0.0f, 0.0f);
}

}  // namespace inkmod_book_style

inline void applyInkmodClassicBookProfile(const char* tagName, const std::string& classAttr,
                                          const std::vector<CssAncestorEntry>& ancestors,
                                          CssStyle& style) {
  inkmod_book_style::apply(tagName, classAttr, ancestors, style);
}
