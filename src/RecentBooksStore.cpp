#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>
#include <Xtc.h>

#include <algorithm>
#include <iterator>

namespace {
constexpr uint8_t RECENT_BOOKS_FILE_VERSION = 3;
constexpr char RECENT_BOOKS_FILE_BIN[] = "/.inkmod/recent.bin";
constexpr char RECENT_BOOKS_FILE_JSON[] = "/.inkmod/recent.json";
constexpr char RECENT_BOOKS_FILE_BAK[] = "/.inkmod/recent.bin.bak";
constexpr int MAX_RECENT_BOOKS = 18;
constexpr char MY_CLIPPINGS_FILE[] = "My Clippings.txt";

bool isMyClippingsExportPath(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const std::string_view name =
      slash == std::string::npos ? std::string_view(path) : std::string_view(path).substr(slash + 1);

  if (name.size() != sizeof(MY_CLIPPINGS_FILE) - 1) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    unsigned char a = static_cast<unsigned char>(name[i]);
    unsigned char b = static_cast<unsigned char>(MY_CLIPPINGS_FILE[i]);
    if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}
}  // namespace

RecentBooksStore RecentBooksStore::instance;

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  addOrUpdateBook(path, title, author, coverBmpPath);
}

void RecentBooksStore::addOrUpdateBook(const std::string& path, const std::string& title, const std::string& author,
                                       const std::string& coverBmpPath) {
  if (isMyClippingsExportPath(path)) {
    removeByPath(path);
    return;
  }

  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });

  if (it == recentBooks.end() && recentBooks.size() >= MAX_RECENT_BOOKS) {
    pruneMissing();
    it = recentBooks.end();
  }

  if (it != recentBooks.end()) {
    // Re-entering/resuming the already-most-recent book is a very common path.
    // If nothing changed, avoid rewriting the complete recent.json to SD.
    const bool metadataChanged = it->title != title || it->author != author || it->coverBmpPath != coverBmpPath;
    if (it == recentBooks.begin() && !metadataChanged) return;

    it->title = title;
    it->author = author;
    it->coverBmpPath = coverBmpPath;
    if (it != recentBooks.begin()) {
      RecentBook book = std::move(*it);
      recentBooks.erase(it);
      recentBooks.insert(recentBooks.begin(), std::move(book));
    }
  } else {
    recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath});
    if (recentBooks.size() > MAX_RECENT_BOOKS) {
      recentBooks.resize(MAX_RECENT_BOOKS);
    }
  }
  saveToFile();
}

bool RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) return false;
  RecentBook& book = *it;
  if (book.title == title && book.author == author && book.coverBmpPath == coverBmpPath) return true;
  book.title = title;
  book.author = author;
  book.coverBmpPath = coverBmpPath;
  saveToFile();
  return true;
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) return false;
  recentBooks.erase(it);
  if (!saveToFile()) LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  return true;
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return book.path == oldPath; });
  if (it == recentBooks.end()) return;

  std::string nextCoverPath = it->coverBmpPath;
  if (!oldCachePath.empty() && !it->coverBmpPath.empty() && it->coverBmpPath.rfind(oldCachePath, 0) == 0) {
    nextCoverPath = newCachePath + it->coverBmpPath.substr(oldCachePath.size());
  }
  if (it->path == newPath && it->coverBmpPath == nextCoverPath) return;

  it->path = newPath;
  it->coverBmpPath = std::move(nextCoverPath);
  saveToFile();
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::pruneMissing() {
  const size_t before = recentBooks.size();
  recentBooks.erase(
      std::remove_if(recentBooks.begin(), recentBooks.end(),
                     [](const RecentBook& book) { return isMissing(book) || isMyClippingsExportPath(book.path); }),
      recentBooks.end());
  return recentBooks.size() != before;
}

bool RecentBooksStore::saveToFile() const {
  Storage.mkdir("/.inkmod");
  return JsonSettingsIO::saveRecentBooks(*this, RECENT_BOOKS_FILE_JSON);
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) lastBookFileName = path.substr(lastSlash + 1);

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());
  if (isMyClippingsExportPath(path)) return RecentBook{path, "", "", ""};

  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.inkmod");
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    Xtc xtc(path, "/.inkmod");
    if (xtc.load()) return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}

bool RecentBooksStore::loadFromFile() {
  String json = Storage.readFile(RECENT_BOOKS_FILE_JSON);
  if (!json.isEmpty()) {
    if (!JsonSettingsIO::loadRecentBooks(*this, json.c_str())) return false;

    // Keep filtering non-book export artifacts immediately, because those can
    // physically exist and therefore would pass Home's later existence check.
    // This is string-only and performs zero SD lookups. Missing real books are
    // deliberately pruned lazily by the screens / full-list insertion path.
    const size_t before = recentBooks.size();
    recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(),
                                     [](const RecentBook& book) { return isMyClippingsExportPath(book.path); }),
                      recentBooks.end());
    if (recentBooks.size() != before) {
      LOG_DBG("RBS", "Removed non-book export entries from recents");
      saveToFile();
    }
    return true;
  }

  if (loadFromBinaryFile()) {
    saveToFile();
    Storage.rename(RECENT_BOOKS_FILE_BIN, RECENT_BOOKS_FILE_BAK);
    LOG_DBG("RBS", "Migrated recent.bin to recent.json");
    return true;
  }
  return false;
}

bool RecentBooksStore::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("RBS", RECENT_BOOKS_FILE_BIN, inputFile)) return false;

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version == 1 || version == 2) {
    uint8_t count;
    serialization::readPod(inputFile, count);
    const size_t loadCount = std::min<size_t>(count, MAX_RECENT_BOOKS);
    recentBooks.clear();
    recentBooks.reserve(loadCount);
    for (size_t i = 0; i < loadCount; i++) {
      std::string path;
      serialization::readString(inputFile, path);
      RecentBook book = getDataFromBook(path);
      if (book.title.empty() && book.author.empty() && version == 2) {
        std::string title, author;
        serialization::readString(inputFile, title);
        serialization::readString(inputFile, author);
        recentBooks.push_back({path, title, author, ""});
      } else {
        recentBooks.push_back(book);
      }
    }
  } else if (version == 3) {
    uint8_t count;
    serialization::readPod(inputFile, count);
    const size_t loadCount = std::min<size_t>(count, MAX_RECENT_BOOKS);
    recentBooks.clear();
    recentBooks.reserve(loadCount);
    uint8_t omitted = 0;

    for (size_t i = 0; i < loadCount; i++) {
      std::string path, title, author, coverBmpPath;
      serialization::readString(inputFile, path);
      serialization::readString(inputFile, title);
      serialization::readString(inputFile, author);
      serialization::readString(inputFile, coverBmpPath);
      if (title.empty()) {
        omitted++;
        continue;
      }
      recentBooks.push_back({path, title, author, coverBmpPath});
    }

    if (omitted > 0) {
      LOG_DBG("RBS", "Omitted %u recent book(s) with missing title", omitted);
      return true;
    }
  } else {
    LOG_ERR("RBS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  pruneMissing();
  LOG_DBG("RBS", "Recent books loaded from binary file (%d entries)", static_cast<int>(recentBooks.size()));
  return true;
}
