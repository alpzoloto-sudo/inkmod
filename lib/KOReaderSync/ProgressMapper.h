#pragma once
#include <Epub.h>

#include <memory>
#include <string>

/**
 * InkMOD position representation.
 */
struct InkMODPosition {
  int spineIndex;                  // Current spine item (chapter) index
  int pageNumber;                  // Current page within the spine item
  int totalPages;                  // Total pages in the current spine item
  uint16_t paragraphIndex = 0;     // 1-based synthetic paragraph index from XPath p[N]
  bool hasParagraphIndex = false;  // True when paragraphIndex was resolved from XPath
  uint16_t liIndex = 0;            // Running <li> count at the matched XPath element
  bool hasLiIndex = false;         // True when target element is <li> and liIndex was resolved
  char xpathAnchorId[64] = {};     // First <a id> captured inside the matched XPath element
};

/**
 * KOReader position representation.
 */
struct KOReaderPosition {
  std::string xpath;  // XPath-like progress string
  float percentage;   // Progress percentage (0.0 to 1.0)
};

/**
 * Maps between InkMOD and KOReader position formats.
 *
 * InkMOD tracks position as (spineIndex, pageNumber).
 * KOReader uses XPath-like strings + percentage.
 *
 * Since InkMOD discards HTML structure during parsing, we generate
 * synthetic XPath strings based on spine index, using percentage as the
 * primary sync mechanism.
 */
class ProgressMapper {
 public:
  /**
   * Convert InkMOD position to KOReader format.
   *
   * @param epub The EPUB book
   * @param pos InkMOD position
   * @return KOReader position
   */
  static KOReaderPosition toKOReader(const std::shared_ptr<Epub>& epub, const InkMODPosition& pos);

  /**
   * Convert KOReader position to InkMOD format.
   *
   * Note: The returned pageNumber may be approximate since different
   * rendering settings produce different page counts.
   *
   * @param epub The EPUB book
   * @param koPos KOReader position
   * @param currentSpineIndex Index of the currently open spine item (for density estimation)
   * @param totalPagesInCurrentSpine Total pages in the current spine item (for density estimation)
   * @return InkMOD position
   */
  static InkMODPosition toInkMOD(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                         int currentSpineIndex = -1, int totalPagesInCurrentSpine = 0);

 private:
  /**
   * Generate a fallback XPath by streaming the spine item's XHTML and resolving
   * a paragraph/text position from intra-spine progress.
   * Produces a full ancestry path such as
   * /body/DocFragment[3]/body/p[42]/text().17.
   */
  static std::string generateXPath(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);

 public:
  // FB2 packages are rendered through synthetic XHTML internally, but KOReader
  // opens the original FB2 with crengine. Convert an InkMOD location to a
  // conservative crengine-compatible FB2 XPointer instead of publishing the
  // synthetic XHTML ancestry.
  static std::string generateFb2CompatibleXPath(const InkMODPosition& pos);
};
