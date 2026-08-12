#pragma once

#include <Arduino.h>
#include <Logging.h>

#include <cstdint>
#include <cstring>

namespace MemoryBudget {

struct HeapSnapshot {
  uint32_t freeHeap;
  uint32_t maxAllocHeap;
};

struct HeapRequirement {
  uint32_t minFree;
  uint32_t minMaxAlloc;
};

constexpr uint32_t EPUB_INLINE_IMAGE_MIN_FREE = 96U * 1024U;
constexpr uint32_t EPUB_INLINE_IMAGE_MIN_MAX_ALLOC = 56U * 1024U;
// JPEGDEC needs about 20 KB plus its 48 KB safety margin. Requiring the
// PNG-sized 96 KB budget here needlessly suppresses otherwise safe JPEGs.
constexpr uint32_t EPUB_INLINE_JPEG_MIN_FREE = 72U * 1024U;
constexpr uint32_t EPUB_INLINE_JPEG_MIN_MAX_ALLOC = 42U * 1024U;
constexpr uint32_t EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_FREE = 120U * 1024U;
constexpr uint32_t EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_MAX_ALLOC = 80U * 1024U;
constexpr uint32_t OPTIONAL_EPUB_REBUILD_MIN_FREE = 96U * 1024U;
constexpr uint32_t OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC = 48U * 1024U;
// Margin above the decoder's own estimated size that hasHeapForImageDecoder()
// requires before letting PngToFramebufferConverter/JpegToFramebufferConverter
// call new (std::nothrow) PNG()/JPEGDEC() (which then does its own further
// internal allocation for zlib/scanline buffers). This check and that
// allocation aren't atomic - the display refresh runs as its own
// concurrently-scheduled task (see the periodic "Wait complete: refresh"
// activity that continues throughout book loading), so heap state can shift
// between "checked OK" and "actually allocated" if that task's own working
// set lands in between. A real crash was traced to exactly this gap: the
// check passed with free=66600/maxAlloc=59380 against a 16KB headroom
// (which cleared the ~45KB decoder requirement by only ~14KB), and the
// PNG decoder's own subsequent internal allocation still failed. Widened
// well past that observed margin so a modest concurrent allocation in the
// gap can't eat all of it.
constexpr uint32_t IMAGE_DECODER_HEADROOM = 48U * 1024U;

inline HeapSnapshot snapshot() { return {ESP.getFreeHeap(), ESP.getMaxAllocHeap()}; }

inline bool hasHeap(const HeapSnapshot heap, const uint32_t minFree, const uint32_t minMaxAlloc) {
  return heap.freeHeap >= minFree && heap.maxAllocHeap >= minMaxAlloc;
}

inline char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline bool endsWithIgnoreCase(const char* value, const char* suffix) {
  if (!value || !suffix) return false;
  const size_t valueLen = strlen(value);
  const size_t suffixLen = strlen(suffix);
  if (suffixLen > valueLen) return false;

  const char* valueTail = value + valueLen - suffixLen;
  for (size_t i = 0; i < suffixLen; ++i) {
    if (asciiLower(valueTail[i]) != asciiLower(suffix[i])) return false;
  }
  return true;
}

inline bool isJpegSource(const char* source) {
  return endsWithIgnoreCase(source, ".jpg") || endsWithIgnoreCase(source, ".jpeg");
}

inline HeapRequirement epubInlineImageRequirementForSource(const char* source) {
  return isJpegSource(source) ? HeapRequirement{EPUB_INLINE_JPEG_MIN_FREE, EPUB_INLINE_JPEG_MIN_MAX_ALLOC}
                               : HeapRequirement{EPUB_INLINE_IMAGE_MIN_FREE, EPUB_INLINE_IMAGE_MIN_MAX_ALLOC};
}

inline bool shouldReleaseSdFontCachesForEpubInlineImage(const HeapSnapshot heap) {
  return !hasHeap(heap, EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_FREE, EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_MAX_ALLOC);
}

inline bool hasHeapForEpubInlineImage(const char* tag, const char* source) {
  const auto heap = snapshot();
  const auto requirement = epubInlineImageRequirementForSource(source);
  if (hasHeap(heap, requirement.minFree, requirement.minMaxAlloc)) {
    return true;
  }

  LOG_ERR(tag, "Low heap for inline image (%u free, %u max alloc, need %u/%u); suppressing %s", heap.freeHeap,
          heap.maxAllocHeap, requirement.minFree, requirement.minMaxAlloc, source ? source : "");
  return false;
}

inline bool hasHeapForOptionalEpubRebuild(const char* tag, const char* action, const int spineIndex) {
  const auto heap = snapshot();
  if (hasHeap(heap, OPTIONAL_EPUB_REBUILD_MIN_FREE, OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC)) {
    return true;
  }

  LOG_DBG(tag, "Skipping %s for spine %d: low heap (free=%u, maxAlloc=%u, need free>=%u maxAlloc>=%u)", action,
          spineIndex, heap.freeHeap, heap.maxAllocHeap, OPTIONAL_EPUB_REBUILD_MIN_FREE,
          OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC);
  return false;
}

inline bool hasHeapForImageDecoder(const char* tag, const char* decoderName, const uint32_t decoderApproxBytes) {
  const auto heap = snapshot();
  const uint32_t minFree = decoderApproxBytes + IMAGE_DECODER_HEADROOM;
  if (hasHeap(heap, minFree, decoderApproxBytes)) {
    return true;
  }

  LOG_ERR(tag, "Not enough heap for %s decoder (%u free, %u max alloc, need %u/%u)", decoderName, heap.freeHeap,
          heap.maxAllocHeap, minFree, decoderApproxBytes);
  return false;
}

}  // namespace MemoryBudget
