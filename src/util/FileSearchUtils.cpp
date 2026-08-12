#include "FileSearchUtils.h"

#include <FsHelpers.h>
#include <HalStorage.h>

#include <algorithm>
#include <cctype>

#include "InkMODSettings.h"

namespace {

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.length() != b.length()) return false;
  for (size_t i = 0; i < a.length(); ++i) {
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Search terms and filenames arrive as UTF-8. std::tolower() handles only
// ASCII in the firmware locale, so fold the Cyrillic uppercase codepoints we
// support in the on-device keyboard too. This mutates one already-owned cold
// path string; it adds no per-codepoint heap allocation during the SD walk.
void lowercaseForSearch(std::string& value) {
  for (size_t i = 0; i < value.size(); ++i) {
    const uint8_t first = static_cast<uint8_t>(value[i]);
    if (first < 0x80) {
      value[i] = static_cast<char>(std::tolower(first));
      continue;
    }
    if (i + 1 >= value.size()) continue;

    uint8_t second = static_cast<uint8_t>(value[i + 1]);
    if (first == 0xD0 && second >= 0x90 && second <= 0xAF) {  // А..Я
      value[i + 1] = static_cast<char>(second + 0x20);
    } else if (first == 0xD0 && second == 0x81) {  // Ё
      value[i] = static_cast<char>(0xD1);
      value[i + 1] = static_cast<char>(0x91);
    } else if (first == 0xD0 && second == 0x84) {  // Є
      value[i] = static_cast<char>(0xD1);
      value[i + 1] = static_cast<char>(0x94);
    } else if (first == 0xD0 && second == 0x86) {  // І
      value[i] = static_cast<char>(0xD1);
      value[i + 1] = static_cast<char>(0x96);
    } else if (first == 0xD0 && second == 0x87) {  // Ї
      value[i] = static_cast<char>(0xD1);
      value[i + 1] = static_cast<char>(0x97);
    } else if (first == 0xD2 && second == 0x90) {  // Ґ
      value[i + 1] = static_cast<char>(0x91);
    }
    ++i;
  }
}

bool isMacOSMetadataEntry(std::string_view filename) {
  return filename.rfind("._", 0) == 0 || filename == ".DS_Store" || filename == ".Spotlight-V100" ||
         filename == ".Trashes" || filename == ".fseventsd";
}

bool isWindowsMetadataEntry(std::string_view filename) {
  return equalsIgnoreCase(filename, "System Volume Information") || equalsIgnoreCase(filename, "$RECYCLE.BIN") ||
         equalsIgnoreCase(filename, "desktop.ini") || equalsIgnoreCase(filename, "Thumbs.db") ||
         equalsIgnoreCase(filename, "IndexerVolumeGuid") || equalsIgnoreCase(filename, "WPSettings.dat");
}

std::string buildFullPath(std::string basepath, const std::string& entry) {
  if (basepath.back() != '/') basepath += "/";
  return basepath + entry;
}

// Same "is this a book we'd show" rules FileBrowserActivity's own
// loadFiles() applies while listing one directory.
bool isSearchableBookFile(std::string_view filename) {
  bool isFb2 = FsHelpers::checkFileExtension(filename, ".fb2");
  bool isZip = FsHelpers::checkFileExtension(filename, ".zip");
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
         FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
         FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename) || isFb2 || isZip;
}

// Books = an opened file (a leaf result); Folder = a directory whose name
// itself matched, meant to be navigated into rather than opened directly.
// (SearchResultKind/SearchResultEntry themselves live in the header - this
// file just uses them.)

// Depth- and count-limited on purpose - an SD card with a very large or
// deeply-nested file tree could otherwise search for a very long time or
// grow the results vector unreasonably large on a device with ~275KB of
// heap. Returning "the first N matches" once either limit is hit is far
// better than hanging or exhausting memory.
constexpr int SEARCH_MAX_DEPTH = 6;
constexpr size_t SEARCH_MAX_RESULTS = 200;

void searchFilesRecursive(const std::string& dirPath, const std::string& lowerQuery,
                          std::vector<SearchResultEntry>& results, const int depth) {
  if (depth > SEARCH_MAX_DEPTH || results.size() >= SEARCH_MAX_RESULTS) return;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  dir.rewindDirectory();

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (results.size() >= SEARCH_MAX_RESULTS) {
      file.close();
      break;
    }
    file.getName(name, sizeof(name));
    if (isMacOSMetadataEntry(name) || isWindowsMetadataEntry(name) || (!SETTINGS.showHiddenFiles && name[0] == '.')) {
      file.close();
      continue;
    }

    const bool isDir = file.isDirectory();
    const std::string childPath = buildFullPath(dirPath, name);
    file.close();

    std::string lowerName(name);
    lowercaseForSearch(lowerName);
    const bool nameMatches = lowerName.find(lowerQuery) != std::string::npos;

    if (isDir) {
      // A matching folder is itself a result (to jump straight to), same
      // as a matching file - but still gets walked into either way, since
      // a book inside a non-matching folder is just as findable as one
      // inside a matching one.
      if (nameMatches) {
        results.push_back({childPath, SearchResultKind::Folder});
      }
      searchFilesRecursive(childPath, lowerQuery, results, depth + 1);
    } else if (isSearchableBookFile(name)) {
      if (nameMatches) {
        results.push_back({childPath, SearchResultKind::Book});
      }
    }
  }
  dir.close();
}

}  // namespace

std::vector<SearchResultEntry> searchBookFiles(const std::string& rootPath, const std::string& query) {
  std::string lowerQuery(query);
  lowercaseForSearch(lowerQuery);
  std::vector<SearchResultEntry> results;
  results.reserve(32);
  searchFilesRecursive(rootPath, lowerQuery, results, 0);
  return results;
}
