#pragma once

#include <HalStorage.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CssStyle.h"

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *   - Two-part descendant: ancestor subject (e.g. "div p", "section.chapter p")
 *
 * Not supported (silently ignored):
 *   - Three-or-more-part descendant selectors
 *   - Child/sibling combinators (>, +, ~)
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */

struct CssAncestorEntry {
  int depth = 0;
  std::string tag;
  std::string classAttr;
};

class CssParser {
 public:
  // v15: inkMOD Classic Book fixes FB2 front-matter and poem/stanza geometry.
  // Bumping the CSS cache also invalidates existing section/page caches through
  // the normal CSS-rebuild path so stale pre-fix pages are not reused.
  static constexpr uint32_t CSS_CACHE_MAGIC = 0x435843FF;
  static constexpr uint8_t CSS_CACHE_VERSION = 15;

  static constexpr size_t MAX_DESCENDANT_RULES = 100;

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  bool loadFromStream(HalFile& source);

  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr,
                                      const std::vector<CssAncestorEntry>& ancestors = {}) const;

  // ChapterHtmlSlimParser naturally calls resolveStyle with a C-string tag and
  // std::string class. This overload marks only real <p> elements so the
  // reader's Paragraph spacing setting replaces publisher vertical spacing
  // while all horizontal layout, text-indent, alignment and non-paragraph
  // block geometry remain untouched. Inline style="margin..." is also blocked
  // later by CssStyle::applyOver because the runtime lock travels with the
  // resolved style.
  [[nodiscard]] CssStyle resolveStyle(const char* tagName, const std::string& classAttr,
                                      const std::vector<CssAncestorEntry>& ancestors = {}) const {
    const std::string_view tag = tagName ? std::string_view(tagName) : std::string_view{};
    CssStyle style = resolveStyle(tag, std::string_view(classAttr), ancestors);
    if (tag == "p") {
      style.lockVerticalSpacingToZero();
    }
    return style;
  }

  [[nodiscard]] static CssStyle parseInlineStyle(std::string_view styleValue);

  [[nodiscard]] bool empty() const { return rulesBySelector_.empty() && descendantRules_.empty(); }
  [[nodiscard]] size_t ruleCount() const { return rulesBySelector_.size(); }

  void clear() {
    decltype(rulesBySelector_){}.swap(rulesBySelector_);
    decltype(descendantRules_){}.swap(descendantRules_);
    cachePartial_ = false;
  }

  bool hasCache() const;
  void deleteCache() const;
  bool saveToCache(bool complete = true) const;
  bool loadFromCache();
  [[nodiscard]] bool isCachePartial() const { return cachePartial_; }

 private:
  static constexpr uint8_t CSS_CACHE_FLAG_PARTIAL = 1 << 0;

  struct DescendantRule {
    std::string ancestorSelector;
    std::string subjectSelector;
    CssStyle style;
  };

  struct CompositeKey {
    std::initializer_list<std::string_view> pieces;
    CompositeKey(std::initializer_list<std::string_view> p) noexcept : pieces(p) {}
  };

  struct SvHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept;
    size_t operator()(const std::string& s) const noexcept;
    size_t operator()(CompositeKey k) const noexcept;
  };
  struct SvEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept;
    bool operator()(const std::string& a, std::string_view b) const noexcept;
    bool operator()(std::string_view a, const std::string& b) const noexcept;
    bool operator()(const std::string& a, const std::string& b) const noexcept;
    bool operator()(CompositeKey a, std::string_view b) const noexcept;
    bool operator()(std::string_view a, CompositeKey b) const noexcept;
  };

  std::unordered_map<std::string, CssStyle, SvHash, SvEqual> rulesBySelector_;
  std::vector<DescendantRule> descendantRules_;

  std::string cachePath;
  bool cachePartial_ = false;

  [[nodiscard]] bool processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  static bool selectorMatchesElement(std::string_view selector, std::string_view tag, std::string_view classAttr);
  static CssStyle parseDeclarations(std::string_view declBlock);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style);

  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  static bool tryInterpretLength(std::string_view val, CssLength& out);
};
