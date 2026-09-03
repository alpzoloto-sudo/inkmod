#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>

class ChapterXPathResolver {
 public:
  /**
   * Resolve the Nth paragraph in a spine item to its real XHTML ancestry path.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]
   *
   * An empty string means parsing failed or the paragraph index was not found.
   */
  static std::string findXPathForParagraph(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex);

  /**
   * Resolve intra-spine progress to a real XHTML ancestry path plus text offset.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]/text().96
   *
   * An empty string means parsing failed or the location could not be resolved.
   */
  static std::string findXPathForProgress(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);

  /**
   * Find the visible-character offset inside a synthetic paragraph that best
   * matches the first visible words of a rendered X4 page.  This lets FB2
   * sync publish the actual top-of-page text position instead of estimating
   * it from pageNumber/totalPages. Returns -1 when no reliable match exists.
   */
  static int findCharOffsetForTextSnippet(const std::shared_ptr<Epub>& epub, int spineIndex,
                                          uint16_t paragraphIndex, const std::string& snippet);

  /** Count visible Unicode codepoints in one synthetic paragraph. */
  static size_t countVisibleCharsInParagraph(const std::shared_ptr<Epub>& epub, int spineIndex,
                                             uint16_t paragraphIndex);
};
