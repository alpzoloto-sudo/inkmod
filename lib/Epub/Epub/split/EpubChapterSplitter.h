#pragma once

#include <string>

// Detects an EPUB whose publisher/converter put an entire book (or, in the
// one real case this was built for, a "complete collected works") into a
// single spine item too large for this device to tokenize/paginate in one
// pass, and - only for that rare case - rewrites a cached, unpacked copy
// with the oversized file split into several normal-sized ones at safe
// HTML boundaries (see HtmlBodySplitter). The split parts go through the
// exact same ChapterHtmlSlimParser/Section pipeline every other chapter
// already does; nothing downstream needs to know this happened.
//
// For the overwhelming majority of real books (no oversized spine item),
// this only ever does one cheap read of content.opf plus one batched
// zip-central-directory size check, then hands back the original path
// unchanged - no unpacking, no cache directory, no behavior change.
class EpubChapterSplitter {
 public:
  // Returns the path ReaderActivity should actually open: either
  // `originalPath` unchanged, or the package directory of a cached,
  // pre-split unpacked copy. Only ever inspects/acts on `.epub` files that
  // are still a real zip (not already an unpacked directory - an
  // FB2-converted package, for instance, never needs this: its own chapter
  // sizes are already bounded by design). Safe to call on every EPUB open;
  // returns `originalPath` immediately for anything that doesn't need it.
  static std::string resolveReadPath(const std::string& originalPath, const std::string& cacheBaseDir);

#ifdef EPUB_CHAPTER_SPLITTER_EXPOSE_FOR_TESTS
  // Test-only: the actual splitting/rewriting logic, skipping the
  // is-this-already-a-directory check resolveReadPath() does first (real
  // callers always go through resolveReadPath(); this exists so the
  // splitting logic itself can be exercised against a fake ZipFile backed
  // by a plain directory, without needing a real .epub archive on hand).
  static std::string resolveReadPathForZipTestOnly(const std::string& originalPath, const std::string& cacheBaseDir);
#endif
};
