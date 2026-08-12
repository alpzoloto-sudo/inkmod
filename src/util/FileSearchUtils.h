#pragma once

#include <string>
#include <vector>

// Book = an opened file (a leaf result); Folder = a directory whose name
// itself matched, meant to be navigated into (in the file browser) rather
// than opened directly.
enum class SearchResultKind { Book, Folder };

struct SearchResultEntry {
  std::string path;
  SearchResultKind kind;
};

// Recursively searches the SD card (from rootPath down) for book files and
// folders whose name contains query (case-insensitive substring match).
// Returns full paths of matches, applying the same extension rules the
// file browser itself uses to decide what counts as an openable book, and
// the same hidden-file convention (SETTINGS.showHiddenFiles). A folder
// itself matching still gets walked into as usual - a book inside a
// non-matching folder is just as findable as one inside a matching one.
//
// Depth- and count-limited on purpose: an SD card with a very large or
// deeply-nested file tree could otherwise search for a very long time or
// grow the results vector unreasonably large on a device with ~275KB of
// heap. Returning "the first N matches" once either limit is hit is far
// better than hanging or exhausting memory.
std::vector<SearchResultEntry> searchBookFiles(const std::string& rootPath, const std::string& query);
