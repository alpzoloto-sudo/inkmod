#include "BookArchiveUtils.h"

#include <ZipFile.h>

#include <cctype>
#include <string_view>

namespace {

bool hasExtensionIgnoreCase(std::string_view path, std::string_view extension) {
  if (path.size() < extension.size()) return false;

  const size_t start = path.size() - extension.size();
  for (size_t i = 0; i < extension.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(path[start + i])) !=
        std::tolower(static_cast<unsigned char>(extension[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

BookArchiveType detectBookArchiveType(const std::string& path) {
  ZipFile zip(path);

  // Every valid EPUB has this required container descriptor. Looking up its
  // entry scans metadata only; no chapter or image bytes are decompressed.
  size_t containerSize = 0;
  if (zip.getInflatedFileSize("META-INF/container.xml", &containerSize) && containerSize > 0) {
    return BookArchiveType::Epub;
  }

  bool containsFb2 = false;
  if (!zip.enumerateFilePaths([&containsFb2](std::string_view entryPath) {
        if (hasExtensionIgnoreCase(entryPath, ".fb2")) containsFb2 = true;
      })) {
    return BookArchiveType::None;
  }

  return containsFb2 ? BookArchiveType::Fb2 : BookArchiveType::None;
}
