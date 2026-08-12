#include "Fb2.h"

#include <Logging.h>
#include <freertos/task.h>

#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>

#include "Fb2Encoding.h"
#include "native/Fb2ZipOpener.h"
#include "native/FsFileReader.h"

namespace {

// v6 ZIP fused-scan pipe -------------------------------------------------
// ESP32-C3 is single-core, so this does not magically make inflate + XML
// parsing parallel CPU work. The win is that scan() consumes decompressed
// bytes directly from RAM as they are produced instead of reopening and
// rereading the complete staged .source.fb2 from the SD card afterward.
//
// Keep the pipe deliberately small: extraction already owns a 32 KiB DEFLATE
// window. A 4 KiB stream buffer plus an 8 KiB parser task is a much safer
// trade-off than another large book-sized/lookup allocation.
class Fb2PipeReader final : public IByteReader {
 public:
  Fb2PipeReader(StreamBufferHandle_t stream, uint64_t totalBytes, volatile bool* producerDone)
      : stream_(stream), totalBytes_(totalBytes), producerDone_(producerDone) {}

  size_t read(void* buf, size_t len) override {
    auto* dst = static_cast<uint8_t*>(buf);
    size_t total = 0;
    while (total < len) {
      const size_t got = xStreamBufferReceive(stream_, dst + total, len - total, pdMS_TO_TICKS(20));
      total += got;
      pos_ += got;
      if (got == 0 && *producerDone_ && xStreamBufferBytesAvailable(stream_) == 0) break;
      // A short chunk is perfectly valid for Fb2XmlReader; returning here
      // also keeps producer/consumer task switches coarse rather than busy.
      if (total > 0) break;
    }
    return total;
  }

  bool seek(uint64_t) override { return false; }
  uint64_t tell() const override { return pos_; }
  uint64_t size() const override { return totalBytes_; }

 private:
  StreamBufferHandle_t stream_;
  uint64_t totalBytes_;
  volatile bool* producerDone_;
  uint64_t pos_ = 0;
};

struct Fb2ZipScanTaskCtx {
  Fb2PipeReader* reader = nullptr;
  Fb2ScanResult* result = nullptr;
  SemaphoreHandle_t done = nullptr;
  bool ok = false;
};

void fb2ZipScanTask(void* arg) {
  auto* ctx = static_cast<Fb2ZipScanTaskCtx*>(arg);
  Fb2Parser parser;
  // Fused ZIP mode is RAM-sensitive: a 2 KiB tokenizer is enough because
  // the pipe itself already supplies small sequential chunks. Normal plain-FB2
  // scan keeps the default 4 KiB buffer.
  ctx->ok = parser.scan(*ctx->reader, *ctx->result, 2048);
#if defined(ENABLE_SERIAL_LOG)
  LOG_INF("FB2-PROF", "fused task stack free: %u bytes",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
#endif
  xSemaphoreGive(ctx->done);
  vTaskDelete(nullptr);
}


constexpr uint8_t PACKAGE_VERSION = 12;  // incremental text-chunk records added to FB2 section index
// A single FB2 <section> with more inline images than this gets split into
// several virtual chapters while its SD-card index is written, so a chapter
// that's actually opened never needs to extract more than this many images
// at once. Real-world crash trace: 23 images in one un-split section
// reliably tripped the reader's own low-heap image-suppression check
// (MemoryBudget::hasHeapForEpubInlineImage) on every single one of them.
// Keep chapters bounded without splitting too aggressively through nested
// markup.  Images are now converted to their SD pixel cache while parsing, so
// two per virtual chapter no longer requires two live PNG decoders later.
constexpr uint32_t MAX_IMAGES_PER_CHAPTER = 2;

// Large image-free FB2 <section>s are exposed to the common EPUB reader as
// several small virtual spine items. This is deliberately based on decoded
// text bytes rather than "pages": the real page count depends on font,
// margins and viewport and is only known later in ChapterHtmlSlimParser.
// ~20 KiB normally lands in the 5-10 page range on X3/X4, so first-open work
// is bounded without creating hundreds of tiny spine items.
constexpr uint32_t TARGET_TEXT_BYTES_PER_CHAPTER = 20 * 1024;

uint32_t virtualChapterCount(const Fb2SectionIndexEntry& section) {
  if (section.imageRefCount > 0) {
    return std::max<uint32_t>(1, (section.imageRefCount + MAX_IMAGES_PER_CHAPTER - 1) / MAX_IMAGES_PER_CHAPTER);
  }
  return std::max<uint32_t>(1, (section.approxTextBytes + TARGET_TEXT_BYTES_PER_CHAPTER - 1) /
                                   TARGET_TEXT_BYTES_PER_CHAPTER);
}

constexpr char CACHE_MAGIC[] = "FB2IDX";  // 6 bytes, no trailing NUL written
constexpr size_t CACHE_MAGIC_LEN = 6;

void normalizeText(std::string& value) {
  std::string normalized;
  normalized.reserve(value.size());
  bool pendingSpace = false;
  for (const unsigned char c : value) {
    if (std::isspace(c)) {
      pendingSpace = !normalized.empty();
      continue;
    }
    if (pendingSpace) normalized.push_back(' ');
    normalized.push_back(static_cast<char>(c));
    pendingSpace = false;
  }
  value.swap(normalized);
}

uint64_t fnvHash64(const char* data, size_t length) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < length; ++i) {
    hash ^= static_cast<uint8_t>(data[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t hashString(const std::string& value) { return fnvHash64(value.data(), value.size()); }

std::string anchorName(uint64_t hash) {
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
  std::string result = "fb2-";
  result.resize(20);
  for (int i = 0; i < 16; ++i) {
    result[4 + i] = HEX_DIGITS[(hash >> ((15 - i) * 4)) & 0x0f];
  }
  return result;
}

uint64_t automaticAnchor(const char* type, int serial) {
  const std::string value = std::string(type) + ":" + std::to_string(serial);
  return hashString(value);
}

std::string chapterHref(int index) { return "text/chapter_" + std::to_string(index) + ".xhtml"; }

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool isNotesBody(const std::string& name) {
  const std::string normalized = lowercase(name);
  return normalized == "notes" || normalized == "comments" || normalized == "footnotes" ||
         normalized == "endnotes" || normalized == "annotations";
}

std::string normalizeImageMediaType(const std::string& value) {
  const std::string mediaType = lowercase(value);
  if (mediaType == "image/jpeg" || mediaType == "image/jpg" || mediaType == "image/pjpeg") return "image/jpeg";
  if (mediaType == "image/png" || mediaType == "image/x-png") return "image/png";
  return {};
}

void writeBytes(Print& out, const char* data, size_t length) {
  if (length > 0) out.write(reinterpret_cast<const uint8_t*>(data), length);
}

void writeBytes(Print& out, const std::string& value) { writeBytes(out, value.data(), value.size()); }

void writeBytes(Print& out, const char* value) { writeBytes(out, value, strlen(value)); }

void writeXmlEscaped(Print& out, const char* text, size_t length, bool attribute = false) {
  size_t start = 0;
  for (size_t i = 0; i < length; ++i) {
    const char* replacement = nullptr;
    switch (text[i]) {
      case '&':
        replacement = "&amp;";
        break;
      case '<':
        replacement = "&lt;";
        break;
      case '>':
        replacement = "&gt;";
        break;
      case '"':
        if (attribute) replacement = "&quot;";
        break;
      case '\'':
        if (attribute) replacement = "&apos;";
        break;
      default:
        break;
    }
    if (!replacement) continue;
    writeBytes(out, text + start, i - start);
    writeBytes(out, replacement, strlen(replacement));
    start = i + 1;
  }
  writeBytes(out, text + start, length - start);
}

void writeXmlEscaped(Print& out, const std::string& value, bool attribute = false) {
  writeXmlEscaped(out, value.data(), value.size(), attribute);
}

bool writeStaticFile(const std::string& path, const char* contents) {
  HalFile file;
  if (!Storage.openFileForWrite("FB2", path, file)) return false;
  const size_t length = strlen(contents);
  const bool success = file.write(contents, length) == length;
  file.close();
  return success;
}

// Every binary cache artifact (section index, image index, package state)
// starts with the same "FB2IDX" + version header, so a corrupted, truncated,
// or format-mismatched file is caught with one cheap check up front instead
// of misreading whatever bytes happen to follow as if they were valid
// records - which, worst case, could walk off the end of the file or hand
// back garbage a caller trusts.
void writeCacheHeader(HalFile& out) {
  out.write(CACHE_MAGIC, CACHE_MAGIC_LEN);
  const uint8_t version = PACKAGE_VERSION;
  out.write(&version, sizeof(version));
}

bool readAndCheckCacheHeader(HalFile& in) {
  char magic[CACHE_MAGIC_LEN];
  uint8_t version = 0;
  if (in.read(magic, CACHE_MAGIC_LEN) != CACHE_MAGIC_LEN || memcmp(magic, CACHE_MAGIC, CACHE_MAGIC_LEN) != 0) {
    return false;
  }
  return in.read(&version, sizeof(version)) == sizeof(version) && version == PACKAGE_VERSION;
}

// Skip an on-SD variable-length field without allocating a std::string for
// it. The 64-byte scratch buffer stays well below the reader task's stack
// budget and prevents a large chapter title/id from fragmenting the heap.
bool skipCacheBytes(HalFile& in, uint32_t bytes) {
  std::array<uint8_t, 64> scratch = {};
  while (bytes > 0) {
    const size_t chunk = std::min<size_t>(bytes, scratch.size());
    if (in.read(scratch.data(), chunk) != chunk) return false;
    bytes -= static_cast<uint32_t>(chunk);
  }
  return true;
}

// The native parser (native/Fb2XmlReader.h) intentionally never decodes
// bytes - it assumes UTF-8 input and forwards everything else verbatim, by
// design (see its own header comment). Real-world FB2 files frequently
// declare a legacy single-byte Russian encoding instead (windows-1251 is
// extremely common; koi8-r less so), so that assumption doesn't hold as-is.
// This mirrors what the old expat-based converter did via its
// XML_SetUnknownEncodingHandler: read the declared encoding out of the XML
// prolog and, if it's not already UTF-8, transcode the whole file to a UTF-8
// temp copy before handing it to the parser.
std::string extractDeclaredEncoding(const char* prolog, size_t length) {
  const char* needle = "encoding=";
  const char* found = nullptr;
  for (size_t i = 0; i + 9 <= length; ++i) {
    if (strncmp(prolog + i, needle, 9) == 0) {
      found = prolog + i + 9;
      break;
    }
  }
  if (!found) return {};
  const char quote = *found;
  if (quote != '"' && quote != '\'') return {};
  const char* end = static_cast<const char*>(memchr(found + 1, quote, length - (found + 1 - prolog)));
  if (!end) return {};
  return std::string(found + 1, end - (found + 1));
}

// Appends the UTF-8 encoding of a BMP code point (single-byte legacy
// encodings never produce anything outside the BMP) into a fixed buffer.
// Returns the number of bytes written (1-3).
size_t appendUtf8(char* out, int codePoint) {
  const uint32_t cp = static_cast<uint32_t>(codePoint);
  if (cp <= 0x7F) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp <= 0x7FF) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  out[0] = static_cast<char>(0xE0 | (cp >> 12));
  out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[2] = static_cast<char>(0x80 | (cp & 0x3F));
  return 3;
}

// If `path` declares a supported non-UTF-8 encoding, transcodes it to a new
// UTF-8 temp file next to `cacheBaseFile` and rewrites `path` to point at
// it (so the caller can track/clean it up the same way as any other temp
// source). Leaves `path` untouched (and returns true) when the file is
// already UTF-8/ASCII or declares an encoding this module has no table
// for - in the latter case the native parser will pass the original bytes
// through as-is, same as it would for any other unrecognized encoding.
// Checks whether a chunk of `path`'s content is already well-formed UTF-8:
// every byte >= 0x80 must be part of a structurally valid multi-byte
// sequence (right number of 0x80-0xBF continuation bytes, no sequence
// truncated by EOF). Doesn't attempt to detect overlong encodings or
// validate the decoded code points are "sensible" - structural well-
// formedness over several KB is already strong enough evidence, since real
// single-byte-encoded text scatters bytes across the whole 0x80-0xFF range
// fairly uniformly and would only pass this by chance in a vanishingly
// small fraction of cases.
bool bodyLooksLikeUtf8Already(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("FB2", path, file)) return false;
  // Skip roughly past the XML prolog/declaration so the sample is actual
  // book content, not the (pure-ASCII, hence UTF-8-compatible either way)
  // header line.
  file.seek(64);
  uint8_t buf[4096];
  const int got = file.read(buf, sizeof(buf));
  file.close();
  if (got <= 0) return false;

  int multiByteSequences = 0;
  for (int i = 0; i < got;) {
    const uint8_t b = buf[i];
    if (b < 0x80) {
      ++i;
      continue;
    }
    int extra;
    if ((b & 0xE0) == 0xC0) extra = 1;
    else if ((b & 0xF0) == 0xE0) extra = 2;
    else if ((b & 0xF8) == 0xF0) extra = 3;
    else return false;  // 0x80-0xBF or 0xF8-0xFF as a lead byte: not valid UTF-8
    if (i + extra >= got) break;  // sequence runs past the sample; stop, don't guess
    for (int k = 1; k <= extra; ++k) {
      if ((buf[i + k] & 0xC0) != 0x80) return false;
    }
    ++multiByteSequences;
    i += 1 + extra;
  }
  // Require a reasonable amount of evidence, not just "no bytes contradicted
  // it" (a sample with zero high-bit bytes at all would trivially "pass"
  // otherwise, telling us nothing about which encoding is actually in use).
  return multiByteSequences >= 20;
}

bool transcodeToUtf8IfNeeded(std::string& path, const std::string& tempPathBase, const Fb2::ProgressFn& onProgress) {
  char prolog[256];
  const size_t prologLen = Storage.readFileToBuffer(path.c_str(), prolog, sizeof(prolog));
  const std::string declared = extractDeclaredEncoding(prolog, prologLen);
  if (declared.empty()) return true;
  const std::string lower = [&] {
    std::string s = declared;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  }();
  if (lower == "utf-8" || lower == "utf8" || lower == "us-ascii" || lower == "ascii") return true;
  // Probe with a representative high byte to see if this is an encoding we
  // actually have a conversion table for.
  if (Fb2Encoding::decodeByte(declared.c_str(), 0xC0) < 0) return true;

  // Some real-world FB2s declare a legacy encoding but were actually
  // re-saved as UTF-8 at some point without the <?xml?> line being
  // updated to match - the declaration lies. Re-encoding already-correct
  // UTF-8 through a single-byte table produces exactly the kind of
  // well-formed-but-wrong-text "double encoding" garbage that's otherwise
  // very hard to tell apart from a genuine decode bug after the fact, so
  // it's worth checking for directly: sample a chunk of the body and see
  // if it's already well-formed UTF-8. Coincidentally-valid multi-byte
  // sequences over a large enough sample are vanishingly unlikely for
  // real single-byte-encoded text (which uses the whole 0x80-0xFF range
  // fairly uniformly), so this is a reliable signal, not a guess.
  if (bodyLooksLikeUtf8Already(path)) return true;

  HalFile in;
  if (!Storage.openFileForRead("FB2", path, in)) return false;
  const std::string outPath = tempPathBase + ".utf8.fb2";
  HalFile out;
  if (!Storage.openFileForWrite("FB2", outPath, out)) {
    in.close();
    return false;
  }

  const size_t totalSize = in.fileSize();
  size_t processed = 0;
  int chunkCount = 0;
  uint8_t inBuf[1024];
  char outBuf[1024 * 3];
  bool ok = true;
  for (;;) {
    const int got = in.read(inBuf, sizeof(inBuf));
    if (got <= 0) break;
    processed += static_cast<size_t>(got);
    ++chunkCount;
    if (onProgress && totalSize > 0 && chunkCount % 16 == 0) {
      onProgress(30 + static_cast<int>(processed * 10 / totalSize));
    }
    // vTaskDelay only exists here so a very large book can't starve the
    // watchdog - it does NOT need to run anywhere near every chunk, and on
    // this firmware it apparently isn't cheap: something else runs a ~500ms
    // display refresh on its own timer, and yielding at all seems to be
    // enough to let a pending one go ahead before returning control here,
    // so yielding too often turns into a slow drip of ~500ms stalls (this
    // is very likely what regressed the "Девчата" load from ~38s to ~138s
    // between builds - too many yield points, not too few this time).
    // Once every ~1MB is still far more than needed to avoid the watchdog.
    if (chunkCount % 256 == 0) vTaskDelay(1);
    size_t outLen = 0;
    for (int i = 0; i < got; ++i) {
      int cp = Fb2Encoding::decodeByte(declared.c_str(), inBuf[i]);
      if (cp < 0) cp = 0xFFFD;  // undefined byte in this encoding: Unicode replacement char
      outLen += appendUtf8(outBuf + outLen, cp);
      if (outLen > sizeof(outBuf) - 8) {
        if (out.write(outBuf, outLen) != outLen) { ok = false; }
        outLen = 0;
      }
    }
    if (outLen && out.write(outBuf, outLen) != outLen) ok = false;
    if (!ok) break;
  }
  in.close();
  out.close();
  if (!ok) {
    Storage.remove(outPath.c_str());
    return false;
  }
  path = outPath;
  return true;
}

// Bounds how many fully-converted FB2 -> EPUB package caches are kept on disk
// at once. FB2 source files stay untouched in the library; only the unpacked
// package copy is subject to this budget, evicted least-recently-used first.
constexpr int MAX_CACHED_FB2_PACKAGES = 5;
constexpr char LRU_INDEX_FILE[] = "/.fb2_lru_index";
constexpr char METADATA_FILE[] = "/fb2_metadata.txt";
constexpr char ANNOTATION_FILE[] = "/fb2_annotation.txt";
constexpr char PACKAGE_STATE_FILE[] = "/fb2_package.bin";
constexpr char SECTIONS_INDEX_FILE[] = "/.fb2_sections.bin";
constexpr char IMAGES_INDEX_FILE[] = "/.fb2_images.bin";

std::vector<std::string> readLruIndex(const std::string& path) {
  std::vector<std::string> keys;
  char buffer[1024];
  const size_t length = Storage.readFileToBuffer(path.c_str(), buffer, sizeof(buffer) - 1);
  if (length == 0) return keys;
  buffer[length] = '\0';
  size_t start = 0;
  for (size_t i = 0; i <= length; ++i) {
    if (i == length || buffer[i] == '\n') {
      if (i > start) keys.emplace_back(buffer + start, i - start);
      start = i + 1;
    }
  }
  return keys;
}

void writeLruIndex(const std::string& path, const std::vector<std::string>& keys) {
  HalFile file;
  if (!Storage.openFileForWrite("FB2", path, file)) return;
  for (const auto& key : keys) {
    writeBytes(file, key);
    file.write(static_cast<uint8_t>('\n'));
  }
  file.close();
}

// ---------------------------------------------------------------------
// StreamSink: turns Fb2Parser's content events for a single <section> into
// XHTML written straight to a Print& (no intermediate chapter file, no
// chapter-splitting - one call renders exactly one section, the same way a
// real EPUB's single chapter file can be arbitrarily long).
// ---------------------------------------------------------------------
bool findIndexedImageFilename(const std::string& imageIndexPath, const std::string& wantedId, std::string& filename) {
  HalFile imagesIn;
  if (!Storage.openFileForRead("FB2", imageIndexPath, imagesIn) || !readAndCheckCacheHeader(imagesIn)) {
    imagesIn.close();
    return false;
  }

  // The FB2 image index is on SD precisely so large illustrated books don't
  // retain every binary id in RAM. Compare ids in a fixed buffer and allocate
  // a filename only for the one image that is actually emitted on this page.
  std::array<char, 64> scratch{};
  for (;;) {
    uint16_t idLen = 0;
    if (imagesIn.read(&idLen, sizeof(idLen)) != sizeof(idLen)) break;

    bool idMatches = idLen == wantedId.size();
    size_t consumed = 0;
    while (consumed < idLen) {
      const size_t chunk = std::min<size_t>(scratch.size(), idLen - consumed);
      if (imagesIn.read(scratch.data(), chunk) != static_cast<int>(chunk)) {
        imagesIn.close();
        return false;
      }
      if (idMatches && memcmp(scratch.data(), wantedId.data() + consumed, chunk) != 0) idMatches = false;
      consumed += chunk;
    }

    uint16_t nameLen = 0;
    if (imagesIn.read(&nameLen, sizeof(nameLen)) != sizeof(nameLen)) break;
    if (idMatches) {
      // Names are generated as image_<n>.png/jpg. Treat a corrupted cache
      // entry as missing rather than allocating an unbounded string.
      if (nameLen == 0 || nameLen > 96) {
        imagesIn.close();
        return false;
      }
      filename.assign(nameLen, '\0');
      const bool readOk = imagesIn.read(filename.data(), nameLen) == static_cast<int>(nameLen);
      imagesIn.close();
      return readOk;
    }
    if (nameLen && !skipCacheBytes(imagesIn, nameLen)) break;
    uint32_t skipStart = 0;
    uint32_t skipEnd = 0;
    if (imagesIn.read(&skipStart, sizeof(skipStart)) != sizeof(skipStart) ||
        imagesIn.read(&skipEnd, sizeof(skipEnd)) != sizeof(skipEnd)) {
      break;
    }
  }
  imagesIn.close();
  return false;
}

class StreamSink : public Fb2ContentSink {
 public:
  // resolveLink(targetId) -> XHTML href to point at (e.g. "chapter_5.xhtml#fb2-...")
  // for a resolvable target, or an empty string if targetId doesn't match
  // any known section (in which case the link renders as plain text - no
  // point emitting an <a href=""> that goes nowhere).
  using LinkResolver = std::function<std::string(const std::string&)>;

  StreamSink(Print& out, std::string imageIndexPath, LinkResolver resolveLink = nullptr)
      : out_(out), imageIndexPath_(std::move(imageIndexPath)), resolveLink_(std::move(resolveLink)) {}

  void onParagraphBegin() override { writeBytes(out_, "<p>"); }
  void onParagraphEnd() override { writeBytes(out_, "</p>"); }

  void onSubtitle(const std::string& text) override {
    writeBytes(out_, "<h3 class=\"subtitle\">");
    writeXmlEscaped(out_, text);
    writeBytes(out_, "</h3>");
  }
  void onEmptyLine() override { writeBytes(out_, "<p class=\"empty-line\">&#160;</p>"); }
  void onHorizontalRule() override { writeBytes(out_, "<hr/>"); }

  void onPoemBegin() override { writeBytes(out_, "<div class=\"poem\">"); }
  void onPoemEnd() override { writeBytes(out_, "</div>"); }
  void onStanzaBegin() override { writeBytes(out_, "<div class=\"stanza\">"); }
  void onStanzaEnd() override { writeBytes(out_, "</div>"); }
  void onVerseLine(const std::string& text) override {
    writeBytes(out_, "<p class=\"v\">");
    writeXmlEscaped(out_, text);
    writeBytes(out_, "</p>");
  }

  void onCiteBegin() override { writeBytes(out_, "<blockquote class=\"cite\">"); }
  void onCiteEnd() override { writeBytes(out_, "</blockquote>"); }
  void onEpigraphBegin() override { writeBytes(out_, "<blockquote class=\"epigraph\">"); }
  void onEpigraphEnd() override { writeBytes(out_, "</blockquote>"); }
  void onTextAuthor(const std::string& text) override {
    writeBytes(out_, "<p class=\"text-author\">");
    writeXmlEscaped(out_, text);
    writeBytes(out_, "</p>");
  }

  void onText(const std::string& text, Fb2InlineStyle style) override {
    const auto has = [style](Fb2InlineStyle bit) {
      return (static_cast<uint8_t>(style) & static_cast<uint8_t>(bit)) != 0;
    };
    std::string open, close;
    auto wrap = [&](bool cond, const char* openTag, const char* closeTag) {
      if (!cond) return;
      open += openTag;
      close = closeTag + close;
    };
    wrap(has(Fb2InlineStyle::Bold), "<strong>", "</strong>");
    wrap(has(Fb2InlineStyle::Italic), "<em>", "</em>");
    wrap(has(Fb2InlineStyle::Underline), "<span class=\"underline\">", "</span>");
    wrap(has(Fb2InlineStyle::Strikethrough), "<span class=\"strike\">", "</span>");
    wrap(has(Fb2InlineStyle::SmallCaps), "<span class=\"smallcaps\">", "</span>");
    wrap(has(Fb2InlineStyle::Superscript), "<sup>", "</sup>");
    wrap(has(Fb2InlineStyle::Subscript), "<sub>", "</sub>");
    writeBytes(out_, open.c_str());
    writeXmlEscaped(out_, text);
    writeBytes(out_, close.c_str());
  }

  void onImage(const std::string& binaryId) override {
    std::string filename;
    if (!findIndexedImageFilename(imageIndexPath_, binaryId, filename)) return;
    writeBytes(out_, "<img src=\"../images/");
    writeXmlEscaped(out_, filename, true);
    writeBytes(out_, "\" alt=\"\"/>");
  }

  void onLinkBegin(const std::string& targetId) override {
    linkWasEmitted_ = false;
    if (!resolveLink_) return;
    const std::string href = resolveLink_(targetId);
    if (href.empty()) return;  // unresolvable target: render as plain text, no <a> wrapper
    writeBytes(out_, "<a href=\"");
    writeXmlEscaped(out_, href, true);
    writeBytes(out_, "\">");
    linkWasEmitted_ = true;
  }
  void onLinkEnd() override {
    if (linkWasEmitted_) writeBytes(out_, "</a>");
    linkWasEmitted_ = false;
  }

  void onTableBegin() override { writeBytes(out_, "<table>"); }
  void onTableEnd() override { writeBytes(out_, "</table>"); }
  void onTableRowBegin() override { writeBytes(out_, "<tr>"); }
  void onTableRowEnd() override { writeBytes(out_, "</tr>"); }
  void onTableCell(const std::string& text, const Fb2TableCellAttrs& attrs) override {
    const char* tag = attrs.isHeader ? "th" : "td";
    writeBytes(out_, "<");
    writeBytes(out_, tag);
    if (attrs.colspan != 1) writeBytes(out_, " colspan=\"" + std::to_string(attrs.colspan) + "\"");
    if (attrs.rowspan != 1) writeBytes(out_, " rowspan=\"" + std::to_string(attrs.rowspan) + "\"");
    if (!attrs.align.empty()) {
      writeBytes(out_, " style=\"text-align:");
      writeXmlEscaped(out_, attrs.align, true);
      writeBytes(out_, "\"");
    }
    writeBytes(out_, ">");
    writeXmlEscaped(out_, text);
    writeBytes(out_, "</");
    writeBytes(out_, tag);
    writeBytes(out_, ">");
  }

 private:
  Print& out_;
  // Own this path. renderChapterOnDemand() passes a concatenated temporary;
  // retaining a reference to it made image lookups read arbitrary bytes as a
  // path after the constructor returned.
  std::string imageIndexPath_;
  LinkResolver resolveLink_;
  bool linkWasEmitted_ = false;  // whether onLinkBegin actually wrote an <a> for the currently-open link
};

// Splits an illustration-heavy section after the complete block containing
// the last image in a slice.  Both neighbouring slices make the decision at
// the same image ordinal and block boundary, so no prose is lost and a
// virtual chapter never cuts a paragraph in half merely because its image
// quota was reached.
class RangeFilterSink : public Fb2ContentSink {
 public:
  RangeFilterSink(Fb2ContentSink& inner, uint32_t rangeStart, uint32_t rangeEnd)
      : inner_(inner), rangeStart_(rangeStart), rangeEnd_(rangeEnd), emitting_(rangeStart == 0) {}

  void onParagraphBegin() override { beginScope(Scope::Paragraph); }
  void onParagraphEnd() override { endScope(Scope::Paragraph); }
  void onSubtitle(const std::string& text) override {
    if (ensureEmitting()) inner_.onSubtitle(text);
    safeBoundary();
  }
  void onEmptyLine() override {
    if (ensureEmitting()) inner_.onEmptyLine();
    safeBoundary();
  }
  void onHorizontalRule() override {
    if (ensureEmitting()) inner_.onHorizontalRule();
    safeBoundary();
  }
  void onPoemBegin() override { beginScope(Scope::Poem); }
  void onPoemEnd() override { endScope(Scope::Poem); }
  void onStanzaBegin() override { beginScope(Scope::Stanza); }
  void onStanzaEnd() override { endScope(Scope::Stanza); }
  void onVerseLine(const std::string& text) override {
    if (ensureEmitting()) inner_.onVerseLine(text);
    safeBoundary();
  }
  void onCiteBegin() override { beginScope(Scope::Cite); }
  void onCiteEnd() override { endScope(Scope::Cite); }
  void onEpigraphBegin() override { beginScope(Scope::Epigraph); }
  void onEpigraphEnd() override { endScope(Scope::Epigraph); }
  void onTextAuthor(const std::string& text) override {
    if (ensureEmitting()) inner_.onTextAuthor(text);
    safeBoundary();
  }
  void onText(const std::string& text, Fb2InlineStyle style) override {
    if (ensureEmitting()) inner_.onText(text, style);
  }
  void onImage(const std::string& binaryId) override {
    if (ensureEmitting()) inner_.onImage(binaryId);
    ++imageOrdinal_;
    // A top-level image is already an indivisible block. Images inside a
    // paragraph/table wait for that container's end callback instead.
    if (scopeCount_ == 0) safeBoundary();
  }
  void onLinkBegin(const std::string& targetId) override {
    linkEmitted_ = ensureEmitting();
    if (linkEmitted_) inner_.onLinkBegin(targetId);
  }
  void onLinkEnd() override {
    if (linkEmitted_) inner_.onLinkEnd();
    linkEmitted_ = false;
  }
  void onTableBegin() override { beginScope(Scope::Table); }
  void onTableEnd() override { endScope(Scope::Table); }
  void onTableRowBegin() override { beginScope(Scope::TableRow); }
  void onTableRowEnd() override { endScope(Scope::TableRow); }
  void onTableCell(const std::string& text, const Fb2TableCellAttrs& attrs) override {
    if (ensureEmitting()) inner_.onTableCell(text, attrs);
    safeBoundary();
  }

 private:
  enum class Scope : uint8_t { Paragraph, Poem, Stanza, Cite, Epigraph, Table, TableRow };
  static constexpr size_t MAX_SCOPES = 16;

  void beginScope(Scope scope) {
    const bool emitScope = ensureEmitting();
    if (scopeCount_ < MAX_SCOPES) scopes_[scopeCount_++] = scope;
    if (emitScope) openScope(scope);
  }

  void endScope(Scope scope) {
    if (emitting_) closeScope(scope);
    // FB2 input is well-formed, but tolerate an unexpected end tag without
    // underflowing the fixed stack used on this RAM-constrained target.
    if (scopeCount_ > 0) --scopeCount_;
    safeBoundary();
  }

  bool ensureEmitting() {
    if (emitting_) return true;
    if (!startReady_ || finished_) return false;
    for (size_t i = 0; i < scopeCount_; ++i) openScope(scopes_[i]);
    emitting_ = true;
    startReady_ = false;
    return true;
  }

  void safeBoundary() {
    if (emitting_ && rangeEnd_ != UINT32_MAX && imageOrdinal_ >= rangeEnd_) {
      if (linkEmitted_) {
        inner_.onLinkEnd();
        linkEmitted_ = false;
      }
      for (size_t i = scopeCount_; i > 0; --i) closeScope(scopes_[i - 1]);
      emitting_ = false;
      finished_ = true;
      return;
    }
    if (!emitting_ && !finished_ && imageOrdinal_ >= rangeStart_) {
      if (rangeEnd_ != UINT32_MAX && imageOrdinal_ >= rangeEnd_) {
        finished_ = true;
      } else {
        startReady_ = true;
      }
    }
  }

  void openScope(Scope scope) {
    switch (scope) {
      case Scope::Paragraph: inner_.onParagraphBegin(); break;
      case Scope::Poem: inner_.onPoemBegin(); break;
      case Scope::Stanza: inner_.onStanzaBegin(); break;
      case Scope::Cite: inner_.onCiteBegin(); break;
      case Scope::Epigraph: inner_.onEpigraphBegin(); break;
      case Scope::Table: inner_.onTableBegin(); break;
      case Scope::TableRow: inner_.onTableRowBegin(); break;
    }
  }

  void closeScope(Scope scope) {
    switch (scope) {
      case Scope::Paragraph: inner_.onParagraphEnd(); break;
      case Scope::Poem: inner_.onPoemEnd(); break;
      case Scope::Stanza: inner_.onStanzaEnd(); break;
      case Scope::Cite: inner_.onCiteEnd(); break;
      case Scope::Epigraph: inner_.onEpigraphEnd(); break;
      case Scope::Table: inner_.onTableEnd(); break;
      case Scope::TableRow: inner_.onTableRowEnd(); break;
    }
  }

  Fb2ContentSink& inner_;
  uint32_t rangeStart_;
  uint32_t rangeEnd_;
  uint32_t imageOrdinal_ = 0;
  Scope scopes_[MAX_SCOPES]{};
  size_t scopeCount_ = 0;
  bool emitting_ = false;
  bool startReady_ = false;
  bool finished_ = false;
  bool linkEmitted_ = false;
};

// Text-side counterpart to RangeFilterSink. It lets a very large,
// image-free FB2 section become several virtual chapters without changing
// the EPUB/layout engine. Every render still streams the native FB2 section,
// but only ~TARGET_TEXT_BYTES_PER_CHAPTER of text is emitted as XHTML, so
// the expensive ChapterHtmlSlimParser/layout pass only paginates a small
// chunk before the reader can show pages.
//
// Byte thresholds decide only when a cut becomes eligible. The actual cut is
// deferred to the end of the current paragraph (or another atomic text
// block), so neighbouring virtual chapters never force a partial paragraph
// onto an otherwise half-empty page.
class TextRangeFilterSink : public Fb2ContentSink {
 public:
  TextRangeFilterSink(Fb2ContentSink& inner, uint32_t rangeStart, uint32_t rangeEnd)
      : inner_(inner), rangeStart_(rangeStart), rangeEnd_(rangeEnd), emitting_(rangeStart == 0) {}

  void onParagraphBegin() override { beginScope(Scope::Paragraph); }
  void onParagraphEnd() override { endScope(Scope::Paragraph); }
  void onPoemBegin() override { beginScope(Scope::Poem); }
  void onPoemEnd() override { endScope(Scope::Poem); }
  void onStanzaBegin() override { beginScope(Scope::Stanza); }
  void onStanzaEnd() override { endScope(Scope::Stanza); }
  void onCiteBegin() override { beginScope(Scope::Cite); }
  void onCiteEnd() override { endScope(Scope::Cite); }
  void onEpigraphBegin() override { beginScope(Scope::Epigraph); }
  void onEpigraphEnd() override { endScope(Scope::Epigraph); }
  void onTableBegin() override { beginScope(Scope::Table); }
  void onTableEnd() override { endScope(Scope::Table); }
  void onTableRowBegin() override { beginScope(Scope::TableRow); }
  void onTableRowEnd() override { endScope(Scope::TableRow); }

  void onSubtitle(const std::string& text) override {
    emitAtomicText(text, [&](const std::string& part) { inner_.onSubtitle(part); });
  }
  void onVerseLine(const std::string& text) override {
    emitAtomicText(text, [&](const std::string& part) { inner_.onVerseLine(part); });
  }
  void onTextAuthor(const std::string& text) override {
    emitAtomicText(text, [&](const std::string& part) { inner_.onTextAuthor(part); });
  }
  void onText(const std::string& text, Fb2InlineStyle style) override {
    countAndEmitText(text, [&](const std::string& part) { inner_.onText(part, style); });
  }
  void onTableCell(const std::string& text, const Fb2TableCellAttrs& attrs) override {
    emitAtomicText(text, [&](const std::string& part) { inner_.onTableCell(part, attrs); });
  }

  void onEmptyLine() override { emitAtomic([&]() { inner_.onEmptyLine(); }); }
  void onHorizontalRule() override { emitAtomic([&]() { inner_.onHorizontalRule(); }); }

  void onImage(const std::string& binaryId) override {
    if (emitting_) inner_.onImage(binaryId);
  }

  void onLinkBegin(const std::string& targetId) override {
    linkEmitted_ = ensureEmitting();
    if (linkEmitted_) inner_.onLinkBegin(targetId);
  }
  void onLinkEnd() override {
    if (linkEmitted_) inner_.onLinkEnd();
    linkEmitted_ = false;
  }

 private:
  enum class Scope : uint8_t { Paragraph, Poem, Stanza, Cite, Epigraph, Table, TableRow };
  static constexpr size_t MAX_SCOPES = 16;

  template <typename EmitFn>
  void countAndEmitText(const std::string& text, EmitFn&& emit) {
    const uint32_t textLen = static_cast<uint32_t>(std::min<size_t>(text.size(), UINT32_MAX - textOrdinal_));
    textOrdinal_ += textLen;
    if (!text.empty() && ensureEmitting()) emit(text);
  }

  template <typename EmitFn>
  void emitAtomicText(const std::string& text, EmitFn&& emit) {
    countAndEmitText(text, emit);
    safeBoundary();
  }

  template <typename EmitFn>
  void emitAtomic(EmitFn&& emit) {
    if (textOrdinal_ < UINT32_MAX) ++textOrdinal_;
    if (ensureEmitting()) emit();
    safeBoundary();
  }

  void beginScope(Scope scope) {
    const bool emitScope = ensureEmitting();
    if (scopeCount_ < MAX_SCOPES) scopes_[scopeCount_++] = scope;
    if (emitScope) openScope(scope);
  }

  void endScope(Scope scope) {
    if (emitting_) closeScope(scope);
    if (scopeCount_ > 0) --scopeCount_;
    safeBoundary();
  }

  bool ensureEmitting() {
    if (emitting_) return true;
    if (!startReady_ || finished_) return false;
    for (size_t i = 0; i < scopeCount_; ++i) openScope(scopes_[i]);
    emitting_ = true;
    startReady_ = false;
    return true;
  }

  void safeBoundary() {
    if (emitting_ && rangeEnd_ != UINT32_MAX && textOrdinal_ >= rangeEnd_) {
      if (linkEmitted_) {
        inner_.onLinkEnd();
        linkEmitted_ = false;
      }
      for (size_t i = scopeCount_; i > 0; --i) closeScope(scopes_[i - 1]);
      emitting_ = false;
      finished_ = true;
      return;
    }
    if (!emitting_ && !finished_ && textOrdinal_ >= rangeStart_) {
      // An unusually large single paragraph can span more than one nominal
      // 20 KiB slice. In that case the intervening slice is intentionally
      // empty rather than duplicating the paragraph in multiple chapters.
      if (rangeEnd_ != UINT32_MAX && textOrdinal_ >= rangeEnd_) {
        finished_ = true;
      } else {
        startReady_ = true;
      }
    }
  }

  void openScope(Scope scope) {
    switch (scope) {
      case Scope::Paragraph: inner_.onParagraphBegin(); break;
      case Scope::Poem: inner_.onPoemBegin(); break;
      case Scope::Stanza: inner_.onStanzaBegin(); break;
      case Scope::Cite: inner_.onCiteBegin(); break;
      case Scope::Epigraph: inner_.onEpigraphBegin(); break;
      case Scope::Table: inner_.onTableBegin(); break;
      case Scope::TableRow: inner_.onTableRowBegin(); break;
    }
  }

  void closeScope(Scope scope) {
    switch (scope) {
      case Scope::Paragraph: inner_.onParagraphEnd(); break;
      case Scope::Poem: inner_.onPoemEnd(); break;
      case Scope::Stanza: inner_.onStanzaEnd(); break;
      case Scope::Cite: inner_.onCiteEnd(); break;
      case Scope::Epigraph: inner_.onEpigraphEnd(); break;
      case Scope::Table: inner_.onTableEnd(); break;
      case Scope::TableRow: inner_.onTableRowEnd(); break;
    }
  }

  Fb2ContentSink& inner_;
  uint32_t rangeStart_;
  uint32_t rangeEnd_;
  uint32_t textOrdinal_ = 0;
  Scope scopes_[MAX_SCOPES]{};
  size_t scopeCount_ = 0;
  bool emitting_ = false;
  bool startReady_ = false;
  bool finished_ = false;
  bool linkEmitted_ = false;
};

}  // namespace

Fb2::Fb2(std::string path, std::string cacheBasePath) : filepath(std::move(path)) {
  const std::string key = std::to_string(std::hash<std::string>{}(filepath));
  cacheKey = "fb2_" + key;
  cacheBaseDir = cacheBasePath;
  cachePath = cacheBaseDir + "/" + cacheKey;
  // Cache dirs from before the epub_ -> fb2_ rename; still cleaned up here so
  // upgrading firmware doesn't leave orphaned caches behind.
  legacyCachePath = std::move(cacheBasePath) + "/epub_" + key;
  packagePath = cachePath + "/package.epub";
  sourcePath = filepath;

  const size_t slash = filepath.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = filepath.find_last_of('.');
  title = filepath.substr(start, dot == std::string::npos || dot <= start ? std::string::npos : dot - start);
}

bool Fb2::isCompressedFb2() const {
  // Only ".zip" itself matters here, not a specific "*.fb2.zip" naming
  // convention: this class is only ever constructed once something else
  // has already decided the file is an FB2 book (see FileBrowserActivity's
  // own extension checks), so any ".zip" that reaches here needs
  // extracting - requiring an exact double-extension additionally missed
  // real books renamed with e.g. "_fb2.zip" instead of ".fb2.zip", which
  // used to mean prepareSource() skipped extraction entirely and scan()
  // ended up reading raw zip container bytes as if they were the FB2 XML.
  const std::string name = lowercase(filepath);
  return name.size() >= 4 && name.compare(name.size() - 4, 4, ".zip") == 0;
}

bool Fb2::prepareSource(const ProgressFn& onProgress) {
  sourcePath = filepath;
  temporarySourcePath.clear();

  if (isCompressedFb2()) {
    const unsigned long zipStarted = millis();
    HalFile zipFile;
    if (!Storage.openFileForRead("FB2", filepath, zipFile)) return false;
    FsFileReader zipReader(zipFile);

    // Locate the FB2 entry ourselves so its uncompressed size can be exposed
    // to IByteReader::size() in the concurrent parser.
    ZipEntryInfo zipEntry;
    if (!findFb2EntryInZip(zipReader, zipEntry)) {
      zipFile.close();
      LOG_ERR("FB2", "No FB2 file found in archive: %s", filepath.c_str());
      return false;
    }

    setupCacheDir();
    temporarySourcePath = cachePath + "/.source.fb2";
    Storage.remove(temporarySourcePath.c_str());
    HalFile extracted;
    if (!Storage.openFileForWrite("FB2", temporarySourcePath, extracted)) {
      zipFile.close();
      return false;
    }

    // Fused scan is optional and must be failure-safe. If any of these small
    // allocations fail we simply extract exactly as v5 did and convertToPackage
    // performs the normal SD-backed scan afterward.
    // v7 lean: 1 KiB is enough for producer/consumer decoupling on a
    // single-core C3. v6 used 4 KiB but profiling showed no throughput benefit
    // worth the extra RAM.
    constexpr size_t kPipeBytes = 1024;
    StreamBufferHandle_t scanStream = xStreamBufferCreate(kPipeBytes, 1);
    SemaphoreHandle_t scanDone = scanStream ? xSemaphoreCreateBinary() : nullptr;
    volatile bool producerDone = false;
    std::unique_ptr<Fb2ScanResult> candidateScan;
    std::unique_ptr<Fb2PipeReader> pipeReader;
    Fb2ZipScanTaskCtx scanCtx;
    TaskHandle_t scanTaskHandle = nullptr;
    bool fusedScanStarted = false;

    if (scanStream && scanDone && ESP.getFreeHeap() >= 100 * 1024 && ESP.getMaxAllocHeap() >= 64 * 1024) {
      candidateScan.reset(new (std::nothrow) Fb2ScanResult());
      if (candidateScan) {
        pipeReader.reset(new (std::nothrow) Fb2PipeReader(scanStream, zipEntry.uncompressedSize, &producerDone));
      }
      if (pipeReader) {
        scanCtx.reader = pipeReader.get();
        scanCtx.result = candidateScan.get();
        scanCtx.done = scanDone;
        // v8 final polish: real-device high-water in v7 showed ~4.9 KiB
        // unused from a 6 KiB allocation on two different FB2.ZIP books.
        // 4 KiB still leaves a wide safety margin while returning ~2 KiB RAM
        // to the system. Do not reduce further without new real-device traces.
        fusedScanStarted =
            xTaskCreate(fb2ZipScanTask, "fb2ZipScan", 4096, &scanCtx, 1, &scanTaskHandle) == pdPASS;
      }
    }

    // Fused mode already spends ~12 KiB on pipe+task, so keep staging at the
    // proven 8 KiB there. Sequential fallback may use v5's 16 KiB staging.
    size_t flushBufSize = fusedScanStarted ? 8 * 1024 : 16 * 1024;
    auto flushBuf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[flushBufSize]);
    if (!flushBuf && flushBufSize > 8 * 1024) {
      flushBufSize = 8 * 1024;
      flushBuf.reset(new (std::nothrow) uint8_t[flushBufSize]);
    }
    if (!flushBuf) {
      producerDone = true;
      if (fusedScanStarted) xSemaphoreTake(scanDone, portMAX_DELAY);
      if (scanDone) vSemaphoreDelete(scanDone);
      if (scanStream) vStreamBufferDelete(scanStream);
      extracted.close();
      zipFile.close();
      Storage.remove(temporarySourcePath.c_str());
      LOG_ERR("FB2", "Not enough heap for FB2.ZIP extraction staging buffer");
      return false;
    }

    size_t flushUsed = 0;
    int flushCount = 0;
    const uint32_t zipSize = zipReader.size();
    const unsigned long fusedStarted = millis();
    LOG_INF("FB2-PROF", "fused start mem: free=%u max=%u",
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));

    const bool extractedOk = extractZipEntry(
        zipReader, zipEntry,
        [&](const uint8_t* d, size_t n) {
          // Feed the scanner BEFORE staging the same bytes to SD. Backpressure
          // from the 4 KiB pipe bounds memory: producer waits instead of
          // accumulating decompressed book data in RAM.
          if (fusedScanStarted) {
            size_t sent = 0;
            while (sent < n) {
              sent += xStreamBufferSend(scanStream, d + sent, n - sent, portMAX_DELAY);
            }
          }

          size_t offset = 0;
          while (offset < n) {
            const size_t copy = std::min(n - offset, flushBufSize - flushUsed);
            memcpy(flushBuf.get() + flushUsed, d + offset, copy);
            flushUsed += copy;
            offset += copy;
            if (flushUsed < flushBufSize) continue;
            extracted.write(flushBuf.get(), flushUsed);
            flushUsed = 0;
            ++flushCount;
            if (onProgress && zipSize > 0 && flushCount % 8 == 0) {
              onProgress(static_cast<int>(zipReader.position() * 30 / zipSize));
            }
            if (flushCount % 128 == 0) vTaskDelay(1);
          }
        });

    if (flushUsed > 0) extracted.write(flushBuf.get(), flushUsed);
    extracted.close();
    zipFile.close();

    producerDone = true;
    bool fusedScanOk = false;
    if (fusedScanStarted) {
      xSemaphoreTake(scanDone, portMAX_DELAY);
      fusedScanOk = scanCtx.ok;
      LOG_INF("FB2-PROF", "zip fused scan: %lums (%s)", millis() - fusedStarted, fusedScanOk ? "ok" : "fallback");
      LOG_INF("FB2-PROF", "fused end mem: free=%u max=%u",
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    }
    if (scanDone) vSemaphoreDelete(scanDone);
    if (scanStream) vStreamBufferDelete(scanStream);
    pipeReader.reset();

    if (!extractedOk) {
      candidateScan.reset();
      Storage.remove(temporarySourcePath.c_str());
      LOG_ERR("FB2", "FB2.ZIP extraction failed: %s", filepath.c_str());
      return false;
    }

    sourcePath = temporarySourcePath;
    if (fusedScanOk) {
      preparedZipScan = std::move(candidateScan);
    } else {
      preparedZipScan.reset();
    }
    LOG_INF("FB2-PROF", "zip extract+streamscan: %lums (staging=%u fused=%d)",
            millis() - zipStarted, static_cast<unsigned>(flushBufSize), fusedScanOk ? 1 : 0);
  }
  if (onProgress) onProgress(30);

  // Normalize a declared non-UTF-8 encoding (windows-1251 is common for
  // older Russian FB2s) to UTF-8: the native parser assumes UTF-8 input and
  // otherwise just forwards raw bytes, which would corrupt such files.
  setupCacheDir();
  const std::string beforeTranscode = sourcePath;
  const std::string previousTemp = temporarySourcePath;
  const unsigned long transcodeStarted = millis();
  if (!transcodeToUtf8IfNeeded(sourcePath, cachePath + "/.source", onProgress)) return false;
  LOG_INF("FB2-PROF", "encoding check/transcode: %lums", millis() - transcodeStarted);
  if (sourcePath != beforeTranscode) {
    if (!previousTemp.empty()) Storage.remove(previousTemp.c_str());
    temporarySourcePath = sourcePath;
    // The fused ZIP scan observed pre-transcode byte offsets/text encoding.
    // A converted UTF-8 source has different offsets, so force the normal
    // scan against the final prepared file for correctness.
    preparedZipScan.reset();
  }
  return true;
}

void Fb2::setupCacheDir() const { Storage.mkdir(cachePath.c_str(), true); }

bool Fb2::cacheIsCurrent() {
  if (!Storage.exists((packagePath + "/META-INF/container.xml").c_str()) ||
      !Storage.exists((packagePath + "/OEBPS/content.opf").c_str())) {
    return false;
  }

  // Validate the section index's own header too, not just its existence -
  // a truncated/corrupted write (power loss, SD error) would otherwise
  // pass this check and only surface much later, as every single chapter
  // silently failing to render instead of a clean rebuild here.
  {
    HalFile sections;
    if (!Storage.openFileForRead("FB2", cachePath + SECTIONS_INDEX_FILE, sections)) return false;
    const bool sectionsOk = readAndCheckCacheHeader(sections);
    sections.close();
    if (!sectionsOk) return false;
  }

  HalFile state;
  if (!Storage.openFileForRead("FB2", cachePath + PACKAGE_STATE_FILE, state)) return false;

  uint64_t cachedSize = 0;
  uint16_t cachedChapters = 0;
  const bool valid = readAndCheckCacheHeader(state) && state.read(&cachedSize, sizeof(cachedSize)) == sizeof(cachedSize) &&
                     state.read(&cachedChapters, sizeof(cachedChapters)) == sizeof(cachedChapters) &&
                     cachedSize == sourceSize && cachedChapters > 0;
  state.close();
  if (valid) chapterCount = cachedChapters;
  return valid;
}

bool Fb2::loadMetadataCache() {
  char buffer[1536];
  const size_t length = Storage.readFileToBuffer((cachePath + METADATA_FILE).c_str(), buffer, sizeof(buffer));
  if (length == 0) return false;

  const char* first = strchr(buffer, '\n');
  if (!first) return false;
  const char* second = strchr(first + 1, '\n');
  if (!second) return false;
  title.assign(buffer, first - buffer);
  author.assign(first + 1, second - first - 1);
  language.assign(second + 1);
  while (!language.empty() && (language.back() == '\n' || language.back() == '\r')) language.pop_back();
  if (language.empty()) language = "und";
  return !title.empty();
}

void Fb2::saveMetadataCache() const {
  HalFile metadata;
  if (!Storage.openFileForWrite("FB2", cachePath + METADATA_FILE, metadata)) return;
  writeBytes(metadata, title);
  metadata.write(static_cast<uint8_t>('\n'));
  writeBytes(metadata, author);
  metadata.write(static_cast<uint8_t>('\n'));
  writeBytes(metadata, language);
  metadata.write(static_cast<uint8_t>('\n'));
  metadata.close();
}

void Fb2::saveCacheSignature() const {
  HalFile state;
  if (!Storage.openFileForWrite("FB2", cachePath + PACKAGE_STATE_FILE, state)) return;
  const uint16_t chapters = static_cast<uint16_t>(std::min(chapterCount, static_cast<int>(UINT16_MAX)));
  writeCacheHeader(state);
  state.write(&sourceSize, sizeof(sourceSize));
  state.write(&chapters, sizeof(chapters));
  state.close();
}

void Fb2::maintainCacheBudget() const {
  const std::string indexPath = cacheBaseDir + LRU_INDEX_FILE;
  std::vector<std::string> keys = readLruIndex(indexPath);

  keys.erase(std::remove(keys.begin(), keys.end(), cacheKey), keys.end());
  keys.insert(keys.begin(), cacheKey);

  while (static_cast<int>(keys.size()) > MAX_CACHED_FB2_PACKAGES) {
    const std::string evictKey = keys.back();
    keys.pop_back();
    const std::string evictPath = cacheBaseDir + "/" + evictKey;
    if (evictPath != cachePath && Storage.exists(evictPath.c_str())) {
      Storage.removeDir(evictPath.c_str());
      LOG_INF("FB2", "Evicted FB2 package cache: %s", evictKey.c_str());
    }
  }

  writeLruIndex(indexPath, keys);
}

bool Fb2::ensurePreparedSource(const std::string& packageCachePath, std::string& outSourcePath) {
  outSourcePath.clear();

  char sourcePathBuf[600] = {};
  const size_t sourcePathLen =
      Storage.readFileToBuffer((packageCachePath + SOURCE_MARKER_FILE).c_str(), sourcePathBuf, sizeof(sourcePathBuf));
  if (sourcePathLen > 0 && sourcePathLen < sizeof(sourcePathBuf)) {
    outSourcePath.assign(sourcePathBuf, sourcePathLen);
    if (Storage.exists(outSourcePath.c_str())) return true;
    LOG_INF("FB2", "Prepared source disappeared while book was open: %s", outSourcePath.c_str());
  } else {
    LOG_INF("FB2", "Prepared source marker is missing/invalid");
  }

  char originalPathBuf[600] = {};
  const size_t originalPathLen = Storage.readFileToBuffer((packageCachePath + ORIGINAL_PATH_MARKER_FILE).c_str(),
                                                          originalPathBuf, sizeof(originalPathBuf));
  if (originalPathLen == 0 || originalPathLen >= sizeof(originalPathBuf)) {
    LOG_ERR("FB2", "Cannot repair prepared source: original FB2 path marker is missing");
    return false;
  }

  const std::string originalPath(originalPathBuf, originalPathLen);
  if (!Storage.exists(originalPath.c_str())) {
    LOG_ERR("FB2", "Cannot repair prepared source: original book is missing: %s", originalPath.c_str());
    return false;
  }

  const size_t slash = packageCachePath.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    LOG_ERR("FB2", "Cannot repair prepared source: invalid cache path: %s", packageCachePath.c_str());
    return false;
  }
  const std::string cacheBase = packageCachePath.substr(0, slash);

  Fb2 repair(originalPath, cacheBase);
  if (repair.cachePath != packageCachePath) {
    LOG_ERR("FB2", "Cannot repair prepared source: cache key mismatch");
    return false;
  }

  if (!repair.prepareSource(nullptr)) {
    LOG_ERR("FB2", "Failed to reconstruct prepared source from: %s", originalPath.c_str());
    return false;
  }

  outSourcePath = repair.sourcePath;
  if (!Storage.exists(outSourcePath.c_str())) {
    LOG_ERR("FB2", "Prepared source repair returned a missing file: %s", outSourcePath.c_str());
    return false;
  }
  if (!writeStaticFile(packageCachePath + SOURCE_MARKER_FILE, outSourcePath.c_str())) {
    LOG_ERR("FB2", "Failed to rewrite prepared source marker after repair");
    return false;
  }

  LOG_INF("FB2", "Prepared source repaired in-place: %s", outSourcePath.c_str());
  return true;
}

bool Fb2::load(const ProgressFn& onProgress) {
  if (loaded) return true;

  // E-ink progress updates are expensive on X4 (~500 ms refresh + optional
  // sunlight power-down). Conversion/transcode code can report the same
  // integer percentage many times, which used to turn a fast streaming loop
  // into dozens of display stalls. Keep FB2 progress coarse: at most one
  // visible update per 10%% bucket, plus the final 100%%.
  int lastProgressBucket = -1;
  const ProgressFn reportProgress = onProgress ? ProgressFn([&](int percent) {
    percent = std::clamp(percent, 0, 100);
    const int bucket = percent / 10;
    if (percent == 100 || bucket > lastProgressBucket) {
      lastProgressBucket = bucket;
      onProgress(percent);
    }
  }) : ProgressFn{};
  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR("FB2", "File does not exist: %s", filepath.c_str());
    return false;
  }

  HalFile source;
  if (!Storage.openFileForRead("FB2", filepath, source)) return false;
  sourceSize = source.fileSize64();
  source.close();

  if (cacheIsCurrent() && loadMetadataCache()) {
    writeStaticFile(cachePath + ORIGINAL_PATH_MARKER_FILE, filepath.c_str());

    std::string preparedSource;
    if (!ensurePreparedSource(cachePath, preparedSource)) {
      LOG_ERR("FB2", "Cached package cannot restore its prepared source");
      return false;
    }

    loaded = true;
    LOG_INF("FB2", "Loaded cached FB2 package: %d chapters", chapterCount);
    maintainCacheBudget();
    if (reportProgress) reportProgress(100);
    return true;
  }

  LOG_INF("FB2", "Indexing FB2: %llu bytes", static_cast<unsigned long long>(sourceSize));
  // Rebuild the cache directory fresh before prepareSource() writes a
  // zip-extracted/transcoded source copy into it - that copy has to survive
  // this wipe, not get created before it and then deleted a moment later.
  if (Storage.exists(cachePath.c_str())) Storage.removeDir(cachePath.c_str());
  setupCacheDir();
  const unsigned long prepareStarted = millis();
  if (!prepareSource(reportProgress)) return false;
  LOG_INF("FB2-PROF", "prepareSource: %lums", millis() - prepareStarted);
  // temporarySourcePath (that zip-extracted/transcoded copy) is NOT deleted
  // after this: renderChapterOnDemand() reopens whatever `sourcePath`
  // ended up as, on every future chapter render, however much later that
  // is. It lives inside cachePath, so normal cache clearing/eviction
  // cleans it up along with everything else.
  const unsigned long packageStarted = millis();
  const bool converted = convertToPackage(reportProgress);
  LOG_INF("FB2-PROF", "convertToPackage: %lums", millis() - packageStarted);
  if (!converted) return false;
  loaded = true;
  LOG_INF("FB2", "Indexed FB2: %d chapters, %zu images (chapters render on demand)", chapterCount, images.size());
  maintainCacheBudget();
  // The popup remains visible until this function returns. Mark it complete
  // only after metadata writes and cache-budget maintenance are done, rather
  // than leaving a finished FB2 preparation visually stuck at 95%.
  if (reportProgress) reportProgress(100);
  return true;
}

const Fb2::ImageInfoPublic* Fb2::findImage(const std::string& id) const {
  const auto it = std::find_if(images.begin(), images.end(), [&](const ImageInfoPublic& image) { return image.id == id; });
  return it == images.end() ? nullptr : &*it;
}

bool Fb2::convertToPackage(const ProgressFn& onProgress) {
  // Cache directory was already wiped fresh and recreated in load(), before
  // prepareSource() wrote a possibly-transcoded source copy into it.
  setupCacheDir();
  Storage.mkdir((packagePath + "/META-INF").c_str(), true);
  Storage.mkdir((packagePath + "/OEBPS/text").c_str(), true);
  Storage.mkdir((packagePath + "/OEBPS/images").c_str(), true);

  const auto fail = [this]() {
    if (Storage.exists(cachePath.c_str())) Storage.removeDir(cachePath.c_str());
    return false;
  };

  Fb2ScanResult scan;
  if (preparedZipScan) {
    scan = std::move(*preparedZipScan);
    preparedZipScan.reset();
    LOG_INF("FB2-PROF", "scan: 0ms (reused fused ZIP scan)");
  } else {
    HalFile source;
    if (!Storage.openFileForRead("FB2", sourcePath, source)) return fail();
    FsFileReader reader(source);
    Fb2Parser parser;
    const unsigned long scanStarted = millis();
    if (!parser.scan(reader, scan)) {
      source.close();
      LOG_ERR("FB2", "FB2 scan failed (not well-formed?): %s", filepath.c_str());
      return fail();
    }
    source.close();
    LOG_INF("FB2-PROF", "scan: %lums", millis() - scanStarted);
  }
  if (onProgress) onProgress(40);

  title = scan.metadata.title;
  author = scan.metadata.author;
  language = scan.metadata.language;
  normalizeText(title);
  normalizeText(author);
  normalizeText(language);
  if (title.empty()) {
    const size_t slash = filepath.find_last_of('/');
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot = filepath.find_last_of('.');
    title = filepath.substr(start, dot == std::string::npos || dot <= start ? std::string::npos : dot - start);
  }
  if (language.empty()) language = "und";
  coverImageId = scan.metadata.coverBinaryId;

  if (!scan.metadata.annotationText.empty()) {
    HalFile annotation;
    if (!Storage.openFileForWrite("FB2", cachePath + ANNOTATION_FILE, annotation)) return fail();
    const bool annotationOk =
        annotation.write(scan.metadata.annotationText.data(), scan.metadata.annotationText.size()) == scan.metadata.annotationText.size();
    annotation.close();
    if (!annotationOk) return fail();
  }

  {
    const bool imagesOk = persistImageIndex(scan);
    if (!imagesOk) return fail();
  }

  // The scan result already owns every binary id. Persist its offsets first,
  // then transfer those strings into the long-lived image list instead of
  // copying them. On illustration-heavy FB2s, keeping both copies alive is
  // enough to exhaust the C3 heap before the first chapter can be opened.
  images.clear();
  images.reserve(scan.binaries.size());
  for (auto& binary : scan.binaries) {
    const std::string mediaType = normalizeImageMediaType(binary.contentType);
    if (mediaType.empty()) continue;
    ImageInfoPublic image;
    image.id = std::move(binary.id);
    image.mediaType = mediaType;
    image.filename = "image_" + std::to_string(images.size()) + (mediaType == "image/png" ? ".png" : ".jpg");
    images.push_back(std::move(image));
  }
  std::vector<Fb2BinaryIndexEntry>().swap(scan.binaries);
  if (onProgress) onProgress(70);

  // Persist the section index (level, innerStartOffset, id, title, image
  // range) - kept permanently, not deleted after this call, since
  // renderChapterOnDemand() reads it on every later chapter open to find
  // where to seek in the FB2 source. Id/title are needed to build the
  // anchor and heading; approxTextBytes is used by getApproxChapterSize()
  // to seed BookMetadataCache's progress-bar math, since a chapter isn't a
  // real file with a real size until it's actually been rendered once.
  // Offsets, parent/body indices are scan()-only bookkeeping and aren't
  // persisted. One record is written per *virtual* chapter, not per FB2
  // <section> - see splitSectionsForImageLoad().
  chapterCount = 0;
  {
    HalFile sectionsOut;
    if (!Storage.openFileForWrite("FB2", cachePath + SECTIONS_INDEX_FILE, sectionsOut)) return fail();
    writeCacheHeader(sectionsOut);
    for (const auto& section : scan.sections) {
      const uint32_t sliceCount = virtualChapterCount(section);
      const bool imageSliced = section.imageRefCount > 0;
      const uint8_t level = static_cast<uint8_t>(std::min<uint16_t>(section.level, 255));
      const uint32_t innerStartOffset = section.innerStartOffset;
      const uint16_t idLen = static_cast<uint16_t>(std::min(section.id.size(), static_cast<size_t>(4096)));

      for (uint32_t slice = 0; slice < sliceCount; ++slice) {
        const std::string& title = slice == 0 ? section.title : std::string();
        const uint16_t titleLen = static_cast<uint16_t>(std::min(title.size(), static_cast<size_t>(4096)));

        uint32_t imageRangeStart = 0;
        uint32_t imageRangeEnd = UINT32_MAX;
        uint32_t textRangeStart = 0;
        uint32_t textRangeEnd = UINT32_MAX;
        uint32_t approxBytes = section.approxTextBytes;

        if (imageSliced) {
          imageRangeStart = slice * MAX_IMAGES_PER_CHAPTER;
          imageRangeEnd =
              imageRangeStart + MAX_IMAGES_PER_CHAPTER >= section.imageRefCount
                  ? UINT32_MAX
                  : imageRangeStart + MAX_IMAGES_PER_CHAPTER;
          approxBytes = sliceCount > 0 ? section.approxTextBytes / sliceCount : section.approxTextBytes;
        } else if (sliceCount > 1) {
          textRangeStart = slice * TARGET_TEXT_BYTES_PER_CHAPTER;
          textRangeEnd =
              slice + 1 >= sliceCount ? UINT32_MAX : textRangeStart + TARGET_TEXT_BYTES_PER_CHAPTER;
          const uint32_t remaining =
              section.approxTextBytes > textRangeStart ? section.approxTextBytes - textRangeStart : 0;
          approxBytes = std::min<uint32_t>(remaining, TARGET_TEXT_BYTES_PER_CHAPTER);
        }

        sectionsOut.write(&level, sizeof(level));
        sectionsOut.write(&innerStartOffset, sizeof(innerStartOffset));
        sectionsOut.write(&idLen, sizeof(idLen));
        if (idLen) sectionsOut.write(section.id.data(), idLen);
        sectionsOut.write(&titleLen, sizeof(titleLen));
        if (titleLen) sectionsOut.write(title.data(), titleLen);
        sectionsOut.write(&approxBytes, sizeof(approxBytes));
        sectionsOut.write(&imageRangeStart, sizeof(imageRangeStart));
        sectionsOut.write(&imageRangeEnd, sizeof(imageRangeEnd));
        sectionsOut.write(&textRangeStart, sizeof(textRangeStart));
        sectionsOut.write(&textRangeEnd, sizeof(textRangeEnd));
        ++chapterCount;
      }
    }
    sectionsOut.close();
  }

  if (chapterCount <= 0 || chapterCount > UINT16_MAX) {
    LOG_ERR("FB2", "FB2 has no readable chapters: %s", filepath.c_str());
    return fail();
  }

  // The marker file renderChapterOnDemand()/Epub::readItemContentsToStream()
  // key off of: its presence is what says "this package's chapters aren't
  // real files, render them from this FB2 source instead."
  if (!writeStaticFile(cachePath + SOURCE_MARKER_FILE, sourcePath.c_str())) return fail();
  if (!writeStaticFile(cachePath + ORIGINAL_PATH_MARKER_FILE, filepath.c_str())) return fail();

  if (!writeContainerFile() || !writeStyleFile() || !writeOpfFile() || !writeNcxFile(scan))
    return fail();
  if (onProgress) onProgress(95);

  saveMetadataCache();
  saveCacheSignature();
  return true;
}

bool Fb2::persistImageIndex(const Fb2ScanResult& scan) {
  // Images are NOT decoded here - only their (id, filename, byte-offset
  // range) is recorded, so a later decodeImageOnDemand() call can seek
  // straight to the right <binary> and decode just that one image, the
  // first time it's actually about to be rendered.
  HalFile imagesOut;
  if (!Storage.openFileForWrite("FB2", cachePath + IMAGES_INDEX_FILE, imagesOut)) return false;
  writeCacheHeader(imagesOut);
  size_t imageIndex = 0;
  for (const auto& binary : scan.binaries) {
    const std::string mediaType = normalizeImageMediaType(binary.contentType);
    if (mediaType.empty()) continue;
    const std::string filename =
        "image_" + std::to_string(imageIndex++) + (mediaType == "image/png" ? ".png" : ".jpg");
    const uint16_t idLen = static_cast<uint16_t>(std::min(binary.id.size(), static_cast<size_t>(4096)));
    const uint16_t nameLen = static_cast<uint16_t>(std::min(filename.size(), static_cast<size_t>(4096)));
    imagesOut.write(&idLen, sizeof(idLen));
    if (idLen) imagesOut.write(binary.id.data(), idLen);
    imagesOut.write(&nameLen, sizeof(nameLen));
    if (nameLen) imagesOut.write(filename.data(), nameLen);
    imagesOut.write(&binary.payloadStartOffset, sizeof(binary.payloadStartOffset));
    imagesOut.write(&binary.payloadEndOffset, sizeof(binary.payloadEndOffset));
  }
  imagesOut.close();
  return true;
}

namespace {
// Bounds how many raw decoded images (the original, often much larger than
// screen-sized, .png/.jpg straight out of the FB2) stay on disk at once.
// Once a view has happened, Epub's own .pxc pixel-cache - a small,
// already-downsampled-to-screen bitmap - satisfies every later render of
// that same image, so keeping more than a couple of raw sources around
// mostly just wastes SD space on a book with many illustrations.
constexpr int MAX_CACHED_RAW_IMAGES = 3;
constexpr char IMAGE_LRU_FILE[] = "/.fb2_image_lru";
}  // namespace

// static
bool Fb2::decodeImageOnDemand(const std::string& imagePath) {
  // imagePath looks like ".../<cachePrefix>_<hash>/package.epub/OEBPS/images/image_7.png".
  // Recover that package's own cache dir from it rather than needing an
  // Fb2 instance (ImageBlock only has the path baked into its serialized
  // cache entry, from whenever the chapter was first rendered).
  constexpr char kPackageMarker[] = "/package.epub/";
  const size_t markerPos = imagePath.find(kPackageMarker);
  if (markerPos == std::string::npos) return false;
  const std::string packageCachePath = imagePath.substr(0, markerPos);
  const size_t nameStart = imagePath.find_last_of('/');
  if (nameStart == std::string::npos) return false;
  const std::string filename = imagePath.substr(nameStart + 1);

  std::string sourcePath;
  if (!ensurePreparedSource(packageCachePath, sourcePath)) return false;

  HalFile imagesIn;
  if (!Storage.openFileForRead("FB2", packageCachePath + IMAGES_INDEX_FILE, imagesIn)) return false;
  if (!readAndCheckCacheHeader(imagesIn)) {
    imagesIn.close();
    return false;
  }
  Fb2BinaryIndexEntry binary;
  bool found = false;
  for (;;) {
    uint16_t idLen = 0, nameLen = 0;
    if (imagesIn.read(&idLen, sizeof(idLen)) != sizeof(idLen)) break;
    std::string id(idLen, '\0');
    if (idLen && imagesIn.read(id.data(), idLen) != idLen) break;
    if (imagesIn.read(&nameLen, sizeof(nameLen)) != sizeof(nameLen)) break;
    std::string name(nameLen, '\0');
    if (nameLen && imagesIn.read(name.data(), nameLen) != nameLen) break;
    uint32_t startOffset = 0, endOffset = 0;
    if (imagesIn.read(&startOffset, sizeof(startOffset)) != sizeof(startOffset) ||
        imagesIn.read(&endOffset, sizeof(endOffset)) != sizeof(endOffset)) {
      break;
    }
    if (name == filename) {
      binary.id = std::move(id);
      binary.payloadStartOffset = startOffset;
      binary.payloadEndOffset = endOffset;
      found = true;
      break;
    }
  }
  imagesIn.close();
  if (!found) return false;

  HalFile source;
  if (!Storage.openFileForRead("FB2", sourcePath, source)) return false;
  FsFileReader reader(source);

  HalFile out;
  if (!Storage.openFileForWrite("FB2", imagePath, out)) {
    source.close();
    return false;
  }

  // See the equivalent buffering note that used to live on the old eager
  // decodeImages(): Base64Decoder's callback fires 1-3 bytes at a time, so
  // writing straight through would be tens of thousands of individual SD
  // writes for one sizeable illustration.
  constexpr size_t kFlushBufSize = 4096;
  std::vector<uint8_t> flushBuf;
  flushBuf.reserve(kFlushBufSize);
  int flushCount = 0;
  Fb2Parser parser;
  parser.decodeBinary(reader, binary, [&](const uint8_t* d, size_t n) {
    flushBuf.insert(flushBuf.end(), d, d + n);
    if (flushBuf.size() >= kFlushBufSize) {
      out.write(flushBuf.data(), flushBuf.size());
      flushBuf.clear();
      // Rare on purpose - see the comment on the equivalent yield in
      // transcodeToUtf8IfNeeded(). A single image is rarely more than a
      // few hundred KB, so this will often not fire at all, which is fine.
      if (++flushCount % 256 == 0) vTaskDelay(1);
    }
  });
  if (!flushBuf.empty()) out.write(flushBuf.data(), flushBuf.size());
  out.close();
  source.close();

  // LRU bookkeeping: note this filename as most-recently-decoded, and evict
  // the raw file for anything that's fallen out of the last
  // MAX_CACHED_RAW_IMAGES. A plain newline-separated list, same format as
  // the existing package-cache LRU index.
  const std::string lruPath = packageCachePath + IMAGE_LRU_FILE;
  std::vector<std::string> recent = readLruIndex(lruPath);
  recent.erase(std::remove(recent.begin(), recent.end(), filename), recent.end());
  recent.insert(recent.begin(), filename);
  while (static_cast<int>(recent.size()) > MAX_CACHED_RAW_IMAGES) {
    const std::string evictName = recent.back();
    recent.pop_back();
    if (evictName != filename) {
      Storage.remove((packageCachePath + "/package.epub/OEBPS/images/" + evictName).c_str());
    }
  }
  writeLruIndex(lruPath, recent);
  return true;
}

// static
uint32_t Fb2::getApproxChapterSize(const std::string& packageCachePath, int chapterIndex) {
  if (chapterIndex < 0) return 0;

  HalFile sectionsIn;
  if (!Storage.openFileForRead("FB2", packageCachePath + SECTIONS_INDEX_FILE, sectionsIn)) return 0;
  if (!readAndCheckCacheHeader(sectionsIn)) {
    sectionsIn.close();
    return 0;
  }

  uint32_t result = 0;
  for (int i = 0; i <= chapterIndex; ++i) {
    uint8_t level = 0;
    uint32_t innerStartOffset = 0;
    uint16_t idLen = 0, titleLen = 0;
    uint32_t approxTextBytes = 0;
    if (sectionsIn.read(&level, sizeof(level)) != sizeof(level) ||
        sectionsIn.read(&innerStartOffset, sizeof(innerStartOffset)) != sizeof(innerStartOffset) ||
        sectionsIn.read(&idLen, sizeof(idLen)) != sizeof(idLen)) {
      break;
    }
    if (idLen && !skipCacheBytes(sectionsIn, idLen)) break;
    if (sectionsIn.read(&titleLen, sizeof(titleLen)) != sizeof(titleLen)) break;
    if (titleLen && !skipCacheBytes(sectionsIn, titleLen)) break;
    if (sectionsIn.read(&approxTextBytes, sizeof(approxTextBytes)) != sizeof(approxTextBytes)) break;
    uint32_t skipRangeStart = 0, skipRangeEnd = 0, skipTextStart = 0, skipTextEnd = 0;
    if (sectionsIn.read(&skipRangeStart, sizeof(skipRangeStart)) != sizeof(skipRangeStart) ||
        sectionsIn.read(&skipRangeEnd, sizeof(skipRangeEnd)) != sizeof(skipRangeEnd) ||
        sectionsIn.read(&skipTextStart, sizeof(skipTextStart)) != sizeof(skipTextStart) ||
        sectionsIn.read(&skipTextEnd, sizeof(skipTextEnd)) != sizeof(skipTextEnd)) {
      break;
    }
    if (i == chapterIndex) {
      result = approxTextBytes;
      break;
    }
  }
  sectionsIn.close();
  return result;
}


// static
bool Fb2::getLogicalChapterBounds(const std::string& packageCachePath, int chapterIndex, int& startIndex,
                                  int& endIndex) {
  startIndex = chapterIndex;
  endIndex = chapterIndex;
  if (chapterIndex < 0) return false;

  HalFile sectionsIn;
  if (!Storage.openFileForRead("FB2", packageCachePath + SECTIONS_INDEX_FILE, sectionsIn)) return false;
  if (!readAndCheckCacheHeader(sectionsIn)) {
    sectionsIn.close();
    return false;
  }

  auto readRecordKey = [&](uint32_t& innerStartOffset) -> bool {
    uint8_t level = 0;
    uint16_t idLen = 0;
    uint16_t titleLen = 0;
    uint32_t approxTextBytes = 0;
    uint32_t imageRangeStart = 0, imageRangeEnd = 0, textRangeStart = 0, textRangeEnd = 0;

    if (sectionsIn.read(&level, sizeof(level)) != sizeof(level) ||
        sectionsIn.read(&innerStartOffset, sizeof(innerStartOffset)) != sizeof(innerStartOffset) ||
        sectionsIn.read(&idLen, sizeof(idLen)) != sizeof(idLen)) {
      return false;
    }
    if (idLen && !skipCacheBytes(sectionsIn, idLen)) return false;
    if (sectionsIn.read(&titleLen, sizeof(titleLen)) != sizeof(titleLen)) return false;
    if (titleLen && !skipCacheBytes(sectionsIn, titleLen)) return false;
    if (sectionsIn.read(&approxTextBytes, sizeof(approxTextBytes)) != sizeof(approxTextBytes) ||
        sectionsIn.read(&imageRangeStart, sizeof(imageRangeStart)) != sizeof(imageRangeStart) ||
        sectionsIn.read(&imageRangeEnd, sizeof(imageRangeEnd)) != sizeof(imageRangeEnd) ||
        sectionsIn.read(&textRangeStart, sizeof(textRangeStart)) != sizeof(textRangeStart) ||
        sectionsIn.read(&textRangeEnd, sizeof(textRangeEnd)) != sizeof(textRangeEnd)) {
      return false;
    }
    return true;
  };

  uint32_t targetOffset = 0;
  bool targetFound = false;
  int index = 0;
  int firstMatch = -1;
  int lastMatch = -1;

  while (true) {
    uint32_t offset = 0;
    if (!readRecordKey(offset)) break;

    if (index == chapterIndex) {
      targetOffset = offset;
      targetFound = true;
      firstMatch = index;
      lastMatch = index;
    } else if (targetFound) {
      if (offset == targetOffset) {
        lastMatch = index;
      } else {
        break;
      }
    }
    ++index;
  }

  if (!targetFound) {
    sectionsIn.close();
    return false;
  }

  // Rewind once to find earlier contiguous slices sharing the same
  // original section. Re-open and validate instead of depending on the
  // serialized cache-header byte size here.
  sectionsIn.close();
  if (!Storage.openFileForRead("FB2", packageCachePath + SECTIONS_INDEX_FILE, sectionsIn) ||
      !readAndCheckCacheHeader(sectionsIn)) {
    if (sectionsIn) sectionsIn.close();
    return false;
  }

  index = 0;
  while (index < chapterIndex) {
    uint32_t offset = 0;
    if (!readRecordKey(offset)) break;
    if (offset == targetOffset) {
      if (firstMatch == chapterIndex) firstMatch = index;
    } else if (firstMatch != chapterIndex) {
      // Only contiguous equal-offset records belong to this section.
      firstMatch = chapterIndex;
    }
    ++index;
  }

  sectionsIn.close();
  startIndex = firstMatch >= 0 ? firstMatch : chapterIndex;
  endIndex = lastMatch >= 0 ? lastMatch : chapterIndex;
  return true;
}

// Resolves an FB2 section id only when a link is actually emitted.  Keeping a
// complete id-to-chapter vector per rendered chapter used many short-lived
// strings and fragmented the C3 heap in illustrated, multi-section books.
// The section index is already on SD, so a fixed-buffer scan trades a rare
// link lookup for predictable RAM use.
StreamSink::LinkResolver buildLinkResolver(const std::string& packageCachePath) {
  const std::string sectionsPath = packageCachePath + SECTIONS_INDEX_FILE;
  return [sectionsPath](const std::string& targetId) -> std::string {
    if (targetId.empty()) return {};

    HalFile sectionsIn;
    if (!Storage.openFileForRead("FB2", sectionsPath, sectionsIn) || !readAndCheckCacheHeader(sectionsIn)) {
      sectionsIn.close();
      return {};
    }

    std::array<char, 64> scratch{};
    for (int chapterIndex = 0;; ++chapterIndex) {
      uint8_t level = 0;
      uint32_t innerStartOffset = 0;
      uint16_t idLen = 0, titleLen = 0;
      if (sectionsIn.read(&level, sizeof(level)) != sizeof(level) ||
          sectionsIn.read(&innerStartOffset, sizeof(innerStartOffset)) != sizeof(innerStartOffset) ||
          sectionsIn.read(&idLen, sizeof(idLen)) != sizeof(idLen)) {
        break;
      }

      bool isTarget = idLen == targetId.size();
      uint16_t remaining = idLen;
      size_t targetOffset = 0;
      while (remaining > 0) {
        const size_t chunkSize = std::min<size_t>(remaining, scratch.size());
        if (sectionsIn.read(scratch.data(), chunkSize) != chunkSize) {
          remaining = UINT16_MAX;
          break;
        }
        if (isTarget && memcmp(scratch.data(), targetId.data() + targetOffset, chunkSize) != 0) {
          isTarget = false;
        }
        targetOffset += chunkSize;
        remaining -= static_cast<uint16_t>(chunkSize);
      }
      if (remaining != 0) break;

      if (sectionsIn.read(&titleLen, sizeof(titleLen)) != sizeof(titleLen)) break;
      remaining = titleLen;
      while (remaining > 0) {
        const size_t chunkSize = std::min<size_t>(remaining, scratch.size());
        if (sectionsIn.read(scratch.data(), chunkSize) != chunkSize) {
          remaining = UINT16_MAX;
          break;
        }
        remaining -= static_cast<uint16_t>(chunkSize);
      }
      if (remaining != 0) break;

      uint32_t skipApprox = 0, skipRangeStart = 0, skipRangeEnd = 0, skipTextStart = 0, skipTextEnd = 0;
      if (sectionsIn.read(&skipApprox, sizeof(skipApprox)) != sizeof(skipApprox) ||
          sectionsIn.read(&skipRangeStart, sizeof(skipRangeStart)) != sizeof(skipRangeStart) ||
          sectionsIn.read(&skipRangeEnd, sizeof(skipRangeEnd)) != sizeof(skipRangeEnd) ||
          sectionsIn.read(&skipTextStart, sizeof(skipTextStart)) != sizeof(skipTextStart) ||
          sectionsIn.read(&skipTextEnd, sizeof(skipTextEnd)) != sizeof(skipTextEnd)) {
        break;
      }
      if (isTarget) {
        sectionsIn.close();
        const uint64_t anchor = fnvHash64(targetId.data(), targetId.size());
        return chapterHref(chapterIndex) + "#" + anchorName(anchor);
      }
    }
    sectionsIn.close();
    return {};
  };
}

// static
bool Fb2::renderChapterOnDemand(const std::string& packageCachePath, int chapterIndex, Print& out) {
  if (chapterIndex < 0) return false;

  std::string sourcePath;
  if (!ensurePreparedSource(packageCachePath, sourcePath)) {
    LOG_ERR("FB2", "Chapter %d: prepared source is unavailable", chapterIndex);
    return false;
  }

  HalFile sectionsIn;
  if (!Storage.openFileForRead("FB2", packageCachePath + SECTIONS_INDEX_FILE, sectionsIn)) {
    LOG_ERR("FB2", "Chapter %d: sections index is unavailable", chapterIndex);
    return false;
  }
  if (!readAndCheckCacheHeader(sectionsIn)) {
    sectionsIn.close();
    LOG_ERR("FB2", "Chapter %d: sections index header is invalid", chapterIndex);
    return false;
  }

  uint8_t level = 0;
  uint32_t innerStartOffset = 0;
  uint16_t idLen = 0, titleLen = 0;
  uint32_t approxTextBytes = 0;
  uint32_t imageRangeStart = 0, imageRangeEnd = 0;
  uint32_t textRangeStart = 0, textRangeEnd = UINT32_MAX;
  std::string id;
  std::string title;
  bool found = false;
  for (int i = 0; i <= chapterIndex; ++i) {
    if (sectionsIn.read(&level, sizeof(level)) != sizeof(level) ||
        sectionsIn.read(&innerStartOffset, sizeof(innerStartOffset)) != sizeof(innerStartOffset) ||
        sectionsIn.read(&idLen, sizeof(idLen)) != sizeof(idLen)) {
      break;
    }
    id.assign(idLen, '\0');
    if (idLen && sectionsIn.read(id.data(), idLen) != idLen) break;
    if (sectionsIn.read(&titleLen, sizeof(titleLen)) != sizeof(titleLen)) break;
    title.assign(titleLen, '\0');
    if (titleLen && sectionsIn.read(title.data(), titleLen) != titleLen) break;
    // Every record ends with approxTextBytes then the image range (see the
    // write loop building SECTIONS_INDEX_FILE in convertToPackage()) -
    // always consumed, even for a record we're skipping past, so the read
    // position doesn't end up misaligned for the next one.
    if (sectionsIn.read(&approxTextBytes, sizeof(approxTextBytes)) != sizeof(approxTextBytes) ||
        sectionsIn.read(&imageRangeStart, sizeof(imageRangeStart)) != sizeof(imageRangeStart) ||
        sectionsIn.read(&imageRangeEnd, sizeof(imageRangeEnd)) != sizeof(imageRangeEnd) ||
        sectionsIn.read(&textRangeStart, sizeof(textRangeStart)) != sizeof(textRangeStart) ||
        sectionsIn.read(&textRangeEnd, sizeof(textRangeEnd)) != sizeof(textRangeEnd)) {
      break;
    }
    if (i == chapterIndex) {
      found = true;
      break;
    }
  }
  sectionsIn.close();
  if (!found) {
    LOG_ERR("FB2", "Chapter %d: not found in persisted section index", chapterIndex);
    return false;
  }
  normalizeText(title);

  HalFile source;
  if (!Storage.openFileForRead("FB2", sourcePath, source)) {
    LOG_ERR("FB2", "Chapter %d: failed to open prepared source: %s", chapterIndex, sourcePath.c_str());
    return false;
  }
  FsFileReader reader(source);

  writeBytes(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  writeBytes(out, "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><meta charset=\"UTF-8\"/>");
  writeBytes(out, "<link rel=\"stylesheet\" type=\"text/css\" href=\"../style.css\"/><title>");
  writeXmlEscaped(out, title);
  writeBytes(out, "</title></head><body>\n");

  const uint64_t anchor = id.empty() ? automaticAnchor("section", chapterIndex) : fnvHash64(id.data(), id.size());
  writeBytes(out, "<section id=\"");
  writeXmlEscaped(out, anchorName(anchor), true);
  writeBytes(out, "\">");

  if (!title.empty()) {
    const int heading = std::min(std::max(static_cast<int>(level) + 1, 1), 6);
    writeBytes(out, "<h" + std::to_string(heading) + ">");
    writeXmlEscaped(out, title);
    writeBytes(out, "</h" + std::to_string(heading) + ">");
  }

  // An FB2 annotation belongs to <description>, not to a body section.  It
  // is persisted during indexing and streamed only into the first chapter;
  // this keeps the original source closed while pagination is running and
  // bounds temporary storage to a small fixed buffer.
  if (chapterIndex == 0) {
    HalFile annotation;
    if (Storage.openFileForRead("FB2", packageCachePath + ANNOTATION_FILE, annotation)) {
      std::array<char, 128> chunk = {};
      bool emitted = false;
      bool paragraphOpen = false;
      while (true) {
        const int bytesRead = annotation.read(chunk.data(), chunk.size());
        if (bytesRead <= 0) break;
        for (int i = 0; i < bytesRead; ++i) {
          const char c = chunk[static_cast<size_t>(i)];
          if (c == '\r') continue;
          if (c == '\n') {
            if (paragraphOpen) writeBytes(out, "</p>");
            paragraphOpen = false;
            continue;
          }
          if (!paragraphOpen) {
            if (!emitted) writeBytes(out, "<div class=\"annotation\">");
            writeBytes(out, "<p>");
            emitted = true;
            paragraphOpen = true;
          }
          writeXmlEscaped(out, &c, 1);
        }
      }
      if (paragraphOpen) writeBytes(out, "</p>");
      if (emitted) writeBytes(out, "</div>");
      annotation.close();
    }
  }

  Fb2SectionIndexEntry section;  // only innerStartOffset is read by renderSection()
  section.innerStartOffset = innerStartOffset;
  section.level = level;
  Fb2Parser parser;
  StreamSink sink(out, packageCachePath + IMAGES_INDEX_FILE, buildLinkResolver(packageCachePath));
  RangeFilterSink imageRangeSink(sink, imageRangeStart, imageRangeEnd);
  TextRangeFilterSink textRangeSink(imageRangeSink, textRangeStart, textRangeEnd);
  const bool renderOk = parser.renderSection(reader, section, textRangeSink);

  writeBytes(out, "</section>\n</body></html>\n");
  source.close();
  if (!renderOk) {
    LOG_ERR("FB2", "Chapter %d: native parser failed at source offset %u", chapterIndex, innerStartOffset);
  }
  return renderOk;
}

bool Fb2::writeContainerFile() const {
  return writeStaticFile(packagePath + "/META-INF/container.xml",
                         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                         "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                         "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
                         "media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n");
}

bool Fb2::writeStyleFile() const {
  return writeStaticFile(
      packagePath + "/OEBPS/style.css",
      "body { text-align: justify; }\n"
      "h1, h2, h3, h4, h5, h6 { text-align: center; font-weight: bold; margin: 1em 0 0.7em 0; }\n"
      ".subtitle { text-align: center; font-style: italic; }\n"
      "p { margin: 0.25em 0; }\n"
      ".epigraph, .cite { margin: 0.7em 1.5em; font-style: italic; text-indent: 0; }\n"
      ".poem { margin: 0.7em 1em; }\n"
      ".v { text-indent: 0; text-align: left; margin: 0; }\n"
      ".text-author { text-align: right; font-style: italic; text-indent: 0; }\n"
      ".empty-line { margin: 0.6em 0; text-indent: 0; }\n"
      ".annotation { font-style: italic; }\n"
      ".strike { text-decoration: line-through; }\n"
      ".underline { text-decoration: underline; }\n"
      ".smallcaps { font-variant: small-caps; }\n"
      ".code { font-family: monospace; }\n"
      "table { border-collapse: collapse; }\n"
      "td, th { border: 1px solid; padding: 0.2em 0.5em; }\n"
      "img { display: block; margin: 0.5em auto; max-width: 100%; }\n");
}

bool Fb2::writeOpfFile() const {
  HalFile file;
  if (!Storage.openFileForWrite("FB2", packagePath + "/OEBPS/content.opf", file)) return false;
  writeBytes(file, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"bookid\">"
                   "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>");
  writeXmlEscaped(file, title);
  writeBytes(file, "</dc:title><dc:creator>");
  writeXmlEscaped(file, author);
  writeBytes(file, "</dc:creator><dc:language>");
  writeXmlEscaped(file, language);
  writeBytes(file, "</dc:language><dc:identifier id=\"bookid\">fb2-");
  const std::string identifier = anchorName(hashString(filepath));
  writeXmlEscaped(file, identifier);
  writeBytes(file, "</dc:identifier>");

  const ImageInfoPublic* cover = findImage(coverImageId);
  if (cover) {
    writeBytes(file, "<meta name=\"cover\" content=\"cover-image\"/>");
  }
  writeBytes(file, "</metadata><manifest>");
  writeBytes(file, "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>");
  writeBytes(file, "<item id=\"style\" href=\"style.css\" media-type=\"text/css\"/>");
  for (int i = 0; i < chapterCount; ++i) {
    const std::string item = "<item id=\"chapter-" + std::to_string(i) + "\" href=\"" + chapterHref(i) +
                             "\" media-type=\"application/xhtml+xml\"/>";
    writeBytes(file, item);
  }
  for (size_t i = 0; i < images.size(); ++i) {
    const std::string id = cover && images[i].id == cover->id ? "cover-image" : "image-" + std::to_string(i);
    writeBytes(file, "<item id=\"");
    writeXmlEscaped(file, id, true);
    writeBytes(file, "\" href=\"images/");
    writeXmlEscaped(file, images[i].filename, true);
    writeBytes(file, "\" media-type=\"");
    writeXmlEscaped(file, images[i].mediaType, true);
    writeBytes(file, "\"/>");
  }
  writeBytes(file, "</manifest><spine toc=\"ncx\">");
  for (int i = 0; i < chapterCount; ++i) {
    writeBytes(file, "<itemref idref=\"chapter-" + std::to_string(i) + "\"/>");
  }
  writeBytes(file, "</spine><guide><reference type=\"text\" title=\"Start\" href=\"");
  writeBytes(file, chapterHref(0));
  writeBytes(file, "\"/></guide></package>\n");
  file.close();
  return true;
}

bool Fb2::writeNcxFile(const Fb2ScanResult& scan) const {
  HalFile file;
  if (!Storage.openFileForWrite("FB2", packagePath + "/OEBPS/toc.ncx", file)) return false;

  writeBytes(file, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">"
                   "<head><meta name=\"dtb:uid\" content=\"fb2\"/></head><docTitle><text>");
  writeXmlEscaped(file, title);
  writeBytes(file, "</text></docTitle><navMap>\n");

  int openDepth = 0;
  int playOrder = 1;
  int sectionFirstChapterIndex = 0;
  bool any = false;
  for (size_t i = 0; i < scan.sections.size(); ++i) {
    const auto& section = scan.sections[i];
    const int currentFirstChapterIndex = sectionFirstChapterIndex;
    sectionFirstChapterIndex += static_cast<int>(virtualChapterCount(section));
    // Footnotes/comments are only reached via in-text links. A non-empty body
    // name is not itself a footnote marker: multi-book FB2 collections often
    // name every main body after its volume, and those chapters belong in TOC.
    if (section.bodyIndex >= 0 && static_cast<size_t>(section.bodyIndex) < scan.bodies.size() &&
        isNotesBody(scan.bodies[section.bodyIndex].name)) {
      continue;
    }
    std::string sectionTitle = section.title;
    normalizeText(sectionTitle);
    if (sectionTitle.empty()) continue;

    const int targetDepth = std::min(std::max(1, static_cast<int>(section.level) + 1), openDepth + 1);
    while (openDepth >= targetDepth) {
      writeBytes(file, "</navPoint>\n");
      --openDepth;
    }

    const uint64_t anchor =
        section.id.empty() ? automaticAnchor("section", static_cast<int>(i)) : fnvHash64(section.id.data(), section.id.size());
    writeBytes(file, "<navPoint id=\"nav-" + std::to_string(playOrder) + "\" playOrder=\"" +
                         std::to_string(playOrder) + "\"><navLabel><text>");
    writeXmlEscaped(file, sectionTitle);
    writeBytes(file, "</text></navLabel><content src=\"");
    // A split section's TOC entry always points at its first virtual
    // chapter (see splitSectionsForImageLoad()) - i may not equal the
    // chapter index once any earlier section has been split.
    writeBytes(file, chapterHref(currentFirstChapterIndex));
    writeBytes(file, "#" + anchorName(anchor));
    writeBytes(file, "\"/>\n");
    openDepth = targetDepth;
    ++playOrder;
    any = true;
  }

  if (!any) {
    for (int chapterIndex = 0; chapterIndex < chapterCount; ++chapterIndex) {
      writeBytes(file, "<navPoint id=\"nav-" + std::to_string(playOrder) + "\" playOrder=\"" +
                           std::to_string(playOrder) + "\"><navLabel><text>");
      writeXmlEscaped(file, chapterIndex == 0 ? title : "Section " + std::to_string(chapterIndex + 1));
      writeBytes(file, "</text></navLabel><content src=\"" + chapterHref(chapterIndex) + "\"/></navPoint>\n");
      ++playOrder;
    }
  } else {
    while (openDepth-- > 0) writeBytes(file, "</navPoint>\n");
  }
  writeBytes(file, "</navMap></ncx>\n");
  file.close();
  return true;
}

bool Fb2::clearCache() const {
  bool success = true;
  if (Storage.exists(cachePath.c_str())) success = Storage.removeDir(cachePath.c_str()) && success;
  if (Storage.exists(legacyCachePath.c_str())) success = Storage.removeDir(legacyCachePath.c_str()) && success;

  const std::string indexPath = cacheBaseDir + LRU_INDEX_FILE;
  std::vector<std::string> keys = readLruIndex(indexPath);
  keys.erase(std::remove(keys.begin(), keys.end(), cacheKey), keys.end());
  writeLruIndex(indexPath, keys);
  return success;
}
