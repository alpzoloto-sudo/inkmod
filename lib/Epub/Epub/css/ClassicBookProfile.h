#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"

// inkMOD Classic Book profile
//
// Applied after publisher stylesheet + inline-style resolution, so these few
// e-ink layout rules behave like !important without throwing away author font
// sizes, inline emphasis, links, images, lists, tables, RTL, super/subscript,
// small-caps or other semantic styling.
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

inline void setLayout(CssStyle& style, CssTextAlign align, float indent,
                      float top, float bottom, float left, float right) {
  style.textAlign = align;
  style.defined.textAlign = 1;
  style.textIndent = CssLength(indent, CssUnit::Em);
  style.defined.textIndent = 1;
  style.marginTop = CssLength(top, CssUnit::Em);
  style.defined.marginTop = 1;
  style.marginBottom = CssLength(bottom, CssUnit::Em);
  style.defined.marginBottom = 1;
  style.marginLeft = CssLength(left, CssUnit::Em);
  style.defined.marginLeft = 1;
  style.marginRight = CssLength(right, CssUnit::Em);
  style.defined.marginRight = 1;

  // Publisher padding is one of the biggest sources of mysterious blank space
  // on a 480x800 screen. Structural spacing above is all this profile needs.
  style.paddingTop = CssLength(0.0f, CssUnit::Em);
  style.defined.paddingTop = 1;
  style.paddingBottom = CssLength(0.0f, CssUnit::Em);
  style.defined.paddingBottom = 1;
  style.paddingLeft = CssLength(0.0f, CssUnit::Em);
  style.defined.paddingLeft = 1;
  style.paddingRight = CssLength(0.0f, CssUnit::Em);
  style.defined.paddingRight = 1;
}

inline void setBold(CssStyle& style) {
  style.fontWeight = CssFontWeight::Bold;
  style.defined.fontWeight = 1;
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

inline void setNormalFontStyle(CssStyle& style) {
  style.fontStyle = CssFontStyle::Normal;
  style.defined.fontStyle = 1;
}

inline void apply(const char* tagName, const std::string& classAttr,
                  const std::vector<CssAncestorEntry>& ancestors, CssStyle& style) {
  const std::string_view tag = tagName ? std::string_view(tagName) : std::string_view{};
  const auto has = [&](std::string_view token) { return classHasToken(classAttr, token); };
  const auto inClass = [&](std::string_view token) { return ancestorHasClass(ancestors, token); };

  const bool topHeading = tag == "h1" || tag == "h2" || tag == "title" || has("title");
  const bool subHeading = tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6" || has("subtitle") ||
                          has("title-line") || has("subheading");
  const bool poemContainer = has("poem") || has("poetry");
  const bool stanzaBreak = has("stanza-break");
  const bool stanzaContainer = has("stanza") || stanzaBreak;
  const bool quoteContainer = tag == "blockquote" || has("cite") || has("quote") || has("epigraph");
  const bool fb2BookAuthor = has("fb2-book-author");
  const bool fb2BookTitle = has("fb2-book-title");
  const bool fb2BookDate = has("fb2-book-date");

  const bool inPoem = poemContainer || stanzaContainer || inClass("poem") || inClass("poetry") ||
                      inClass("stanza") || inClass("stanza-break");
  const bool inQuote = quoteContainer || ancestorHasTag(ancestors, "blockquote") || inClass("cite") ||
                       inClass("quote") || inClass("epigraph");

  // Generated FB2 front matter needs deliberately non-stacking margins:
  // author -> title must have exactly the same 0.6em gap as title -> annotation.
  // Treat these before generic h2 handling because both are emitted as <h2>.
  if (fb2BookAuthor) {
    setLayout(style, CssTextAlign::Center, 0.0f, 0.6f, 0.0f, 0.0f, 0.0f);
    setBold(style);
  } else if (fb2BookTitle) {
    setLayout(style, CssTextAlign::Center, 0.0f, 0.6f, 0.6f, 0.0f, 0.0f);
    setBold(style);
  // Container geometry is inherited by the existing block-style stack.
  } else if (topHeading) {
    // Book/part/chapter-level titles (FB2 <title>, EPUB h1/h2/.title) keep a
    // visible gap around them - unlike subHeading below, these usually are
    // NOT already wrapped in a blank line by the source markup.
    setLayout(style, CssTextAlign::Center, 0.0f, 0.6f, 0.6f, 0.0f, 0.0f);
    setBold(style);
  } else if (subHeading) {
    // No external top/bottom margin: FB2/EPUB source markup usually already
    // wraps these in a blank line, so an added margin here doubles the gap.
    setLayout(style, CssTextAlign::Center, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    setBold(style);
  } else if (quoteContainer) {
    setLayout(style, CssTextAlign::Justify, 0.0f, 0.6f, 0.6f, 1.2f, 1.2f);
    // font-style intentionally left alone here: don't force italic over
    // whatever the source markup/publisher stylesheet already set.
  } else if (poemContainer) {
    // Inside an epigraph the attribution follows the poem.  Do not add a poem
    // bottom margin there: text-author supplies the requested 0.5em gap.
    const float bottom = inClass("epigraph") ? 0.0f : 0.6f;
    setLayout(style, CssTextAlign::Left, 0.0f, 0.6f, bottom, 1.2f, 1.2f);
  } else if (stanzaContainer) {
    // StreamSink marks every stanza after the first with stanza-break.  Putting
    // 0.5em on the *next* stanza gives an exact inter-stanza gap without also
    // stacking that gap in front of a following text-author.
    setLayout(style, CssTextAlign::Left, 0.0f, stanzaBreak ? 0.5f : 0.0f, 0.0f, 0.0f, 0.0f);
  }

  // Ordinary prose: no inter-paragraph holes, 1.2em red line, justified.
  // Structural book paragraphs override this below.
  if (tag != "p") return;

  if (subHeading) return;

  if (fb2BookDate) {
    // Publication year/date below the annotation: right aligned, compact
    // 0.5em separation, no inherited first-line indent.
    setLayout(style, CssTextAlign::Right, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f);
    return;
  }

  if (has("text-author") || has("author") || has("cite-author")) {
    // Attribution alignment is structural; font style is not.  FB2 does not
    // require text-author to be italic, so preserve whatever the source
    // actually requested instead of forcing italics here.
    setLayout(style, CssTextAlign::Right, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f);
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
    // Hanging indent: the line's own start sits at the left edge (textIndent
    // pulls it back by the same amount marginLeft pushed the block in), but
    // if the line is long enough to wrap, the wrapped continuation lands
    // indented under marginLeft instead of collapsing back to the margin.
    setLayout(style, CssTextAlign::Left, -1.5f, 0.0f, 0.0f, 1.5f, 0.0f);
    return;
  }

  if (inQuote) {
    setLayout(style, CssTextAlign::Justify, 1.2f, 0.0f, 0.0f, 0.0f, 0.0f);
    // font-style intentionally left alone here too, same reasoning as the
    // quoteContainer case above.
    return;
  }

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

  setLayout(style, CssTextAlign::Justify, 1.2f, 0.0f, 0.0f, 0.0f, 0.0f);
}

}  // namespace inkmod_book_style

inline void applyInkmodClassicBookProfile(const char* tagName, const std::string& classAttr,
                                          const std::vector<CssAncestorEntry>& ancestors,
                                          CssStyle& style) {
  inkmod_book_style::apply(tagName, classAttr, ancestors, style);
}
