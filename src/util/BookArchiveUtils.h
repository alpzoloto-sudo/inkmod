#pragma once

#include <cstdint>
#include <string>

// A plain .zip needs content-based routing: EPUB is itself a ZIP container,
// while FB2-in-ZIP has to be converted before the reader can open it.
enum class BookArchiveType : uint8_t {
  None,
  Epub,
  Fb2,
};

// Inspects only the ZIP central directory. It does not inflate book content.
BookArchiveType detectBookArchiveType(const std::string& path);
