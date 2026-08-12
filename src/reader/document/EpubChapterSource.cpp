#include "EpubChapterSource.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <string>

namespace reader {

namespace {

constexpr char kCacheDirectory[] = "/reader_v1";

bool copyPath(const std::string& path, char* const out, const size_t outSize) {
  if (!out || outSize == 0) return false;
  const int written = std::snprintf(out, outSize, "%s", path.c_str());
  return written >= 0 && static_cast<size_t>(written) < outSize;
}

}  // namespace

bool EpubChapterSource::prepare(const uint32_t chapter, char* const outPath, const size_t outPathSize) const {
  const int spineCount = epub_.getSpineItemsCount();
  if (spineCount <= 0 || chapter >= static_cast<uint32_t>(spineCount)) return false;

  // These short strings are used once per chapter preparation; a fixed buffer
  // is not viable because legacy cache paths are dynamic. They are released
  // before the cursor/layout phase, so no long-lived chapter text is on heap.
  const std::string cacheDir = epub_.getCachePath() + kCacheDirectory;
  const std::string chapterPath = cacheDir + "/chapter_" + std::to_string(chapter) + ".xhtml";
  const std::string tempPath = chapterPath + ".tmp";
  const std::string backupPath = chapterPath + ".bak";

  if (!Storage.ensureDirectoryExists(cacheDir.c_str())) {
    LOG_ERR("RCORE", "Could not create EPUB reader cache: %s", cacheDir.c_str());
    return false;
  }
  if (Storage.exists(chapterPath.c_str())) return copyPath(chapterPath, outPath, outPathSize);

  if (Storage.exists(tempPath.c_str())) Storage.remove(tempPath.c_str());
  FsFile target;
  if (!Storage.openFileForWrite("RCORE", tempPath, target)) {
    LOG_ERR("RCORE", "Could not open chapter cache temp file");
    return false;
  }

  const auto spine = epub_.getSpineItem(static_cast<int>(chapter));
  const bool copied = epub_.readItemContentsToStream(spine.href, target, 512);
  const bool synced = copied && target.sync();
  const bool closed = target.close();
  if (!synced || !closed) {
    LOG_ERR("RCORE", "Could not prepare EPUB chapter %u", static_cast<unsigned>(chapter));
    Storage.remove(tempPath.c_str());
    return false;
  }

  if (Storage.exists(backupPath.c_str())) Storage.remove(backupPath.c_str());
  if (Storage.exists(chapterPath.c_str()) && !Storage.rename(chapterPath.c_str(), backupPath.c_str())) {
    LOG_ERR("RCORE", "Could not rotate EPUB chapter cache");
    Storage.remove(tempPath.c_str());
    return false;
  }
  if (!Storage.rename(tempPath.c_str(), chapterPath.c_str())) {
    LOG_ERR("RCORE", "Could not publish EPUB chapter cache");
    if (Storage.exists(backupPath.c_str()) && !Storage.exists(chapterPath.c_str())) {
      Storage.rename(backupPath.c_str(), chapterPath.c_str());
    }
    Storage.remove(tempPath.c_str());
    return false;
  }
  if (Storage.exists(backupPath.c_str())) Storage.remove(backupPath.c_str());
  return copyPath(chapterPath, outPath, outPathSize);
}

}  // namespace reader
