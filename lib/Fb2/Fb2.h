#pragma once

#include <HalStorage.h>
#include <Print.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "native/Fb2Parser.h"
#include "native/Fb2Types.h"
#include "native/IByteReader.h"

namespace reader {
class ReaderCancellationToken;
}

// FB2 -> EPUB-shaped package converter, built for lazy per-chapter rendering
// instead of eagerly writing out every chapter's XHTML up front.
//
// load() only scans the FB2 once (metadata + flat section index), writes the
// small OPF/NCX/container.xml/style.css files, and persists a compact
// section index - it never renders chapter text. A real EPUB opens near-
// instantly because its chapter XHTML already exists inside the file; this
// mirrors that by not doing any chapter work until a chapter is actually
// requested. renderChapterOnDemand() is that hook: Epub::readItemContentsToStream()
// calls it (see lib/Epub/Epub.cpp) whenever it's asked for a spine item
// inside a package that has our marker file, streaming that one section's
// XHTML straight out - through the exact same ChapterHtmlSlimParser/Section
// pagination pipeline a real EPUB's chapters go through, so there is only
// ever one rendering engine, not two.
class Fb2 {
 public:
  struct ImageInfoPublic {
    std::string id;
    std::string filename;
    std::string mediaType;
  };

  using ProgressFn = std::function<void(int percent)>;

  explicit Fb2(std::string path, std::string cacheBasePath);
  ~Fb2() = default;

  Fb2(const Fb2&) = delete;
  Fb2& operator=(const Fb2&) = delete;

  bool load(const ProgressFn& onProgress = nullptr);
  bool clearCache() const;
  void setupCacheDir() const;

  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] const std::string& getPackagePath() const { return packagePath; }
  [[nodiscard]] const std::string& getTitle() const { return title; }
  [[nodiscard]] const std::string& getAuthor() const { return author; }
  [[nodiscard]] const std::string& getLanguage() const { return language; }
  [[nodiscard]] uint64_t getSourceSize() const { return sourceSize; }
  [[nodiscard]] int getChapterCount() const { return chapterCount; }
  [[nodiscard]] bool isLoaded() const { return loaded; }

  // Renders one FB2 <section> (identified by its spine/chapter index, in the
  // same document order scan() produced) straight to `out` as the chapter
  // XHTML Section::createSectionFile() expects - called from
  // Epub::readItemContentsToStream() for packages carrying our marker file.
  // `packageCachePath` is the *.epub package directory's cache path (i.e.
  // Fb2's own cachePath - the two were unified so a converted book doesn't
  // leave a second orphaned cache folder behind).
  static bool renderChapterOnDemand(const std::string& packageCachePath, int chapterIndex, Print& out,
                                    const reader::ReaderCancellationToken* cancellationToken = nullptr);

  // Decodes and writes the single image at `imagePath` (an absolute path
  // like ".../fb2_<hash>/package.epub/OEBPS/images/image_7.png") from its
  // FB2 source on first use - images are never decoded at load() time.
  // Called from ImageBlock::render() right before it would otherwise report
  // the file missing; a no-op (returns false) for any path that isn't
  // inside an FB2-origin package, so it's safe to call unconditionally.
  // Keeps only the last few (see native/Fb2Types.h-adjacent constant in
  // Fb2.cpp) raw decoded images on disk, evicting older ones - once an
  // image has been viewed, Epub's own .pxc pixel-cache (a much smaller,
  // already screen-sized bitmap) is what every later view actually reads,
  // so the raw source rarely needs to stick around.
  static bool decodeImageOnDemand(const std::string& imagePath,
                                  const reader::ReaderCancellationToken* cancellationToken = nullptr);

  // Estimated decoded-text size of chapter `chapterIndex`, in bytes - not a
  // real file size (the chapter isn't a real file until rendered), just
  // scan()'s own approximation, persisted alongside the rest of the section
  // index. BookMetadataCache's spine-size pass calls this as a fallback
  // when a chapter file doesn't exist yet, so the book's cumulative-size
  // (and therefore "% read") estimate isn't always 0 for an unopened FB2
  // chapter. Returns 0 if this isn't an FB2-origin package, the index is
  // missing/corrupt, or chapterIndex is out of range - all of which the
  // caller already treats as "no size available" the same way it does for
  // a real EPUB's own occasional missing-item case.
  static uint32_t getApproxChapterSize(const std::string& packageCachePath, int chapterIndex);

  // Read every persisted approximate chapter size in one sequential pass.
  // BookMetadataCache uses this instead of reopening and rescanning the
  // variable-length section index once for every spine item.
  static bool loadApproxChapterSizes(const std::string& packageCachePath, std::deque<uint32_t>& outSizes);

  // Return the contiguous virtual-spine range that belongs to the same
  // original FB2 <section> as chapterIndex. Large sections are split into
  // several virtual chapters for RAM safety; all slices share innerStartOffset.
  static bool getLogicalChapterBounds(const std::string& packageCachePath, int chapterIndex, int& startIndex,
                                      int& endIndex);

  // Map a virtual chapter back to the ordinal of its original FB2 <section>.
  // Virtual slices share the same innerStartOffset in the persisted index;
  // counting distinct offsets gives a stable source-section ordinal suitable
  // for CREngine/KOReader FB2 XPointer generation. Returns a 1-based ordinal.
  static bool getOriginalSectionOrdinal(const std::string& packageCachePath, int chapterIndex, int& ordinal);

  // Given the path to a converted package (e.g. ".../package.epub"),
  // returns the real original .fb2/.zip location recorded in
  // ORIGINAL_PATH_MARKER_FILE alongside it, or packagePath unchanged if
  // that marker doesn't exist (i.e. this is a real EPUB, not an FB2
  // conversion, or the marker is missing/corrupt). Callers that need the
  // user's actual book file - the file browser's "show this book" action,
  // KOReader document-hash calculation - must resolve through this first:
  // the package path is an internal cache artifact, not something another
  // device or a real folder listing would ever recognize as the book.
  static std::string resolveOriginalPath(const std::string& packagePath);

  // The marker file's name, relative to a package's cache dir. Its presence
  // is what tells Epub::readItemContentsToStream() this package's chapters
  // need to be rendered through renderChapterOnDemand() instead of read as
  // plain files - checked with plain Storage.exists(), no Fb2 instance
  // needed. Real EPUB/unpacked packages never have this file.
  static constexpr char SOURCE_MARKER_FILE[] = "/.fb2_source";
  // Stable pointer to the user's original .fb2/.zip. .fb2_source may point
  // at a temporary extracted/transcoded file inside the cache.
  static constexpr char ORIGINAL_PATH_MARKER_FILE[] = "/.fb2_original";

 private:
  std::string filepath;
  std::string sourcePath;
  std::string temporarySourcePath;
  std::string cacheBaseDir;
  std::string cacheKey;
  std::string cachePath;
  std::string legacyCachePath;
  std::string packagePath;
  std::string title;
  std::string author;
  std::string date;
  std::string language = "und";
  std::string coverImageId;
  uint64_t sourceSize = 0;
  bool loaded = false;

  int chapterCount = 0;
  std::deque<ImageInfoPublic> images;

  // v6: for FB2.ZIP, prepareSource() can parse the decompressed byte stream
  // while the same bytes are being staged to .source.fb2. convertToPackage()
  // consumes this scan result instead of reading the staged file again.
  std::unique_ptr<Fb2ScanResult> preparedZipScan;

  bool convertToPackage(const ProgressFn& onProgress);
  bool prepareSource(const ProgressFn& onProgress);
  static bool ensurePreparedSource(const std::string& packageCachePath, std::string& outSourcePath);
  bool isCompressedFb2() const;
  bool cacheIsCurrent();
  bool loadMetadataCache();
  void saveMetadataCache() const;
  void saveCacheSignature() const;
  void maintainCacheBudget() const;

  bool persistImageIndex(const Fb2ScanResult& scan);
  const ImageInfoPublic* findImage(const std::string& id) const;

  bool writeContainerFile() const;
  bool writeStyleFile() const;
  bool writeOpfFile() const;
  bool writeNcxFile(const Fb2ScanResult& scan) const;
};
