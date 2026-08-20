#include "FsHelpers.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace FsHelpers {

namespace {
bool isHexDigit(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

inline bool isAsciiDigit(const char c) { return c >= '0' && c <= '9'; }

uint8_t hexValue(const char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
  return static_cast<uint8_t>(10 + (c - 'A'));
}

inline unsigned char asciiLower(const unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

template <size_t N>
bool checkLiteralExtension(const std::string_view fileName, const char (&extension)[N]) {
  static_assert(N > 1, "extension literal must not be empty");
  constexpr size_t extLen = N - 1;
  if (fileName.size() < extLen) return false;

  const size_t offset = fileName.size() - extLen;
  for (size_t i = 0; i < extLen; ++i) {
    if (asciiLower(static_cast<unsigned char>(fileName[offset + i])) !=
        asciiLower(static_cast<unsigned char>(extension[i]))) {
      return false;
    }
  }
  return true;
}

void appendPathComponent(std::string& result, const char* data, const size_t len) {
  if (len == 0) return;
  if (!result.empty()) result.push_back('/');
  result.append(data, len);
}

void popPathComponent(std::string& result) {
  if (result.empty()) return;
  const size_t slash = result.find_last_of('/');
  if (slash == std::string::npos) {
    result.clear();
  } else {
    result.resize(slash);
  }
}
}  // namespace

std::string decodeUriEscapes(const std::string_view path) {
  std::string decoded;
  decoded.reserve(path.size());

  for (size_t i = 0; i < path.size(); i++) {
    if (path[i] == '%' && i + 2 < path.size() && isHexDigit(path[i + 1]) && isHexDigit(path[i + 2])) {
      const uint8_t value = static_cast<uint8_t>((hexValue(path[i + 1]) << 4) | hexValue(path[i + 2]));
      decoded += static_cast<char>(value);
      i += 2;
      continue;
    }

    decoded += path[i];
  }

  return decoded;
}

std::string normalisePath(const std::string_view path) {
  // Build the normalized path directly in one reserved string. The previous
  // implementation allocated a vector plus a separate std::string for every
  // component, which made this very common EPUB helper unnecessarily heap-heavy.
  // Preserve its exact legacy semantics: ".." is resolved only when it is
  // followed by '/', while a trailing ".." remains a literal component.
  std::string result;
  result.reserve(path.size());

  size_t componentStart = 0;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] != '/') continue;

    const size_t len = i - componentStart;
    if (len != 0) {
      if (len == 2 && path[componentStart] == '.' && path[componentStart + 1] == '.') {
        popPathComponent(result);
      } else {
        appendPathComponent(result, path.data() + componentStart, len);
      }
    }
    componentStart = i + 1;
  }

  // Legacy behavior intentionally does not interpret a final ".." segment.
  if (componentStart < path.size()) {
    appendPathComponent(result, path.data() + componentStart, path.size() - componentStart);
  }

  return result;
}

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    const bool isDir1 = str1.back() == '/';
    const bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    while (*s1 && *s2) {
      if (isAsciiDigit(*s1) && isAsciiDigit(*s2)) {
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        int len1 = 0, len2 = 0;
        while (isAsciiDigit(s1[len1])) len1++;
        while (isAsciiDigit(s2[len2])) len2++;

        if (len1 != len2) return len1 < len2;

        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        s1 += len1;
        s2 += len2;
      } else {
        const unsigned char c1 = asciiLower(static_cast<unsigned char>(*s1));
        const unsigned char c2 = asciiLower(static_cast<unsigned char>(*s2));
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    return *s1 == '\0' && *s2 != '\0';
  });
}

bool checkFileExtension(std::string_view fileName, const char* extension) {
  const size_t extLen = strlen(extension);
  if (fileName.length() < extLen) {
    return false;
  }

  const size_t offset = fileName.length() - extLen;
  for (size_t i = 0; i < extLen; i++) {
    if (asciiLower(static_cast<unsigned char>(fileName[offset + i])) !=
        asciiLower(static_cast<unsigned char>(extension[i]))) {
      return false;
    }
  }
  return true;
}

bool hasJpgExtension(std::string_view fileName) {
  return checkLiteralExtension(fileName, ".jpg") || checkLiteralExtension(fileName, ".jpeg");
}

bool hasPngExtension(std::string_view fileName) { return checkLiteralExtension(fileName, ".png"); }

bool hasBmpExtension(std::string_view fileName) { return checkLiteralExtension(fileName, ".bmp"); }

bool hasGifExtension(std::string_view fileName) { return checkLiteralExtension(fileName, ".gif"); }

bool hasEpubExtension(std::string_view fileName) { return checkLiteralExtension(fileName, ".epub"); }

bool hasXtcExtension(std::string_view fileName) {
  return checkLiteralExtension(fileName, ".xtc") || checkLiteralExtension(fileName, ".xtch");
}

bool hasTxtExtension(std::string_view fileName) { return checkLiteralExtension(fileName, ".txt"); }

bool hasMarkdownExtension(std::string_view fileName) { return checkLiteralExtension(fileName, ".md"); }

bool hasCssExtension(std::string_view fileName) { return checkLiteralExtension(fileName, ".css"); }

std::string extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen) {
  if (maxLen == 0) {
    return;
  }

  size_t i = 0;
  for (; i < maxLen - 1 && input[i] != '\0'; i++) {
    const char c = input[i];
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
        c == ' ' || (c > 0x00 && c <= 0x1f)) {
      output[i] = '-';
    } else {
      output[i] = c;
    }
  }
  output[i] = '\0';
}

}  // namespace FsHelpers