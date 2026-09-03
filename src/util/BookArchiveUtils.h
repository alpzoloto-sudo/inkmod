#pragma once

#include <cstdint>
#include <string>

// A plain .zip needs content-based routing: EPUB is itself a ZIP container,
// while FB2-in-ZIP has to be converted before the reader can open it.
enum class BookArchiveType : uint8_t {
  None,
  Epub,
  Fb2,
  WrappedEpub,
};

// Inspects only the ZIP central directory. It does not inflate book content.
BookArchiveType detectBookArchiveType(const std::string& path);

// For a plain ZIP containing a single .epub file, extracts that inner EPUB
// to a persistent SD cache and returns its path. The extraction is streamed;
// the whole EPUB is never held in RAM. Returns false for other ZIP shapes.
bool extractWrappedEpub(const std::string& archivePath, std::string& outEpubPath);
