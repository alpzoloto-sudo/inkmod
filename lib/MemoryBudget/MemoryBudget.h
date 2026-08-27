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

// Keep the pre-extraction gate aligned with the PNG decoder's real budget.
// PNGdec is ~44 KB and the converter already enforces a separate 48 KB
// decoder headroom immediately before allocation.  The old 96/56 KB gate
// rejected valid chapters at ~94 KB free even though the real decoder guard
// would safely allow them, causing the reader to abort and eject the user.
// PNGdec embeds its workspace inside PNG. With our 8192-byte scanline buffer
// sizeof(PNG) is ~51 KiB, so keep a little allocator margin and enough total
// free heap for the gray row/cache writer. This is still low enough for the
// observed FB2 spine-2 state (~80 KiB free / 53 KiB maxAlloc).
constexpr uint32_t EPUB_INLINE_IMAGE_MIN_FREE = 68U * 1024U;
constexpr uint32_t EPUB_INLINE_IMAGE_MIN_MAX_ALLOC = 50U * 1024U;
// JPEGDEC needs about 20 KB plus its dedicated 24 KB safety margin. Requiring the
// PNG-sized 96 KB budget here needlessly suppresses otherwise safe JPEGs.
// Match the decoder's real guard (20 KiB JPEGDEC + 24 KiB headroom).  The
// previous 42 KiB contiguous-allocation gate rejected JPEG precaching even
// though JPEGDEC itself only needs a ~20 KiB contiguous allocation.
constexpr uint32_t EPUB_INLINE_JPEG_MIN_FREE = 44U * 1024U;
constexpr uint32_t EPUB_INLINE_JPEG_MIN_MAX_ALLOC = 20U * 1024U;
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
constexpr uint32_t JPEG_DECODER_HEADROOM = 24U * 1024U;

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

inline bool tailMatchesIgnoreCase(const char* value, const size_t valueLen, const char* suffix,
                                  const size_t suffixLen) {
  if (suffixLen > valueLen) return false;
  const char* valueTail = value + valueLen - suffixLen;
  for (size_t i = 0; i < suffixLen; ++i) {
    if (asciiLower(valueTail[i]) != asciiLower(suffix[i])) return false;
  }
  return true;
}

inline bool isJpegSource(const char* source) {
  if (!source) return false;
  // Inline-image checks are on a hot render path. Compute the source length
  // once and use compile-time literal lengths instead of running strlen() for
  // both .jpg and .jpeg probes.
  const size_t sourceLen = strlen(source);
  return tailMatchesIgnoreCase(source, sourceLen, ".jpg", sizeof(".jpg") - 1) ||
         tailMatchesIgnoreCase(source, sourceLen, ".jpeg", sizeof(".jpeg") - 1);
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

inline bool hasHeapForImageDecoder(const char* tag, const char* decoderName, const uint32_t decoderApproxBytes,
                                   const uint32_t headroom = IMAGE_DECODER_HEADROOM) {
  const auto heap = snapshot();
  const uint32_t minFree = decoderApproxBytes + headroom;
  if (hasHeap(heap, minFree, decoderApproxBytes)) {
    return true;
  }

  LOG_ERR(tag, "Not enough heap for %s decoder (%u free, %u max alloc, need %u/%u)", decoderName, heap.freeHeap,
          heap.maxAllocHeap, minFree, decoderApproxBytes);
  return false;
}

}  // namespace MemoryBudget
