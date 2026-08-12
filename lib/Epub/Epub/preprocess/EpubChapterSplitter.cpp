#include "EpubChapterSplitter.h"

#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace EpubStreamingChapterSplitter {

namespace {

constexpr size_t MIN_FRAGMENT_BYTES = 32 * 1024;
constexpr size_t MAX_TRACKED_ANCHORS = 256;

struct BoundaryTracker {
  size_t contentStart = 0;
  size_t lastChosen = 0;
  size_t nextTarget = 0;
  size_t previousCandidate = 0;
  size_t fallbackCandidate = 0;
  size_t fallbackDistance = std::numeric_limits<size_t>::max();
  std::vector<size_t> chosen;

  void begin(const size_t start) {
    contentStart = start;
    lastChosen = start;
    nextTarget = start + TARGET_FRAGMENT_BYTES;
  }

  void record(const size_t candidate) {
    if (candidate <= contentStart) return;
    const size_t fallbackTarget = contentStart + SPLIT_THRESHOLD_BYTES / 2;
    const size_t distance = candidate > fallbackTarget ? candidate - fallbackTarget : fallbackTarget - candidate;
    if (distance < fallbackDistance) {
      fallbackCandidate = candidate;
      fallbackDistance = distance;
    }
    if (candidate >= nextTarget) {
      size_t split = candidate;
      if (candidate - lastChosen > SPLIT_THRESHOLD_BYTES &&
          previousCandidate >= lastChosen + MIN_FRAGMENT_BYTES) {
        split = previousCandidate;
      }
      chosen.push_back(split);
      lastChosen = split;
      nextTarget = split + TARGET_FRAGMENT_BYTES;
      previousCandidate = candidate > split ? candidate : 0;
    } else {
      previousCandidate = candidate;
    }
  }
};

struct ScanContext {
  XML_Parser parser = nullptr;  // set right after XML_ParserCreate(), needed by the callbacks below
  int depthSinceBody = -1;      // -1 = haven't seen <body> yet; 0 = inside <body>, at its direct children's level
  size_t bodyContentStartOffset = 0;  // byte right after <body ...>'s closing '>'
  size_t bodyContentEndOffset = 0;    // byte at the start of </body>
  bool sawBody = false;
  bool bodyClosed = false;
  BoundaryTracker bodyBoundaries;
  BoundaryTracker wrapperBoundaries;
  size_t directWrapperStartOffset = 0;
  size_t directWrapperOpenEndOffset = 0;
  size_t directWrapperContentEndOffset = 0;
  std::string directWrapperName;
  uint32_t directBodyChildCount = 0;

  // Every element id="" seen anywhere under <body>, with the byte offset
  // of that element's own opening tag - resolved to a fragment index
  // after boundaries are chosen, so a toc.ncx entry pointing at "#someid"
  // can be redirected to the right fragment.
  std::vector<std::pair<std::string, size_t>> anchorOffsets;
  bool anchorLimitReached = false;
};

// Byte offset of the character right after whatever expat just finished
// handling (its current event's start + length) - not exposed directly by
// expat as one call, but composable from the two that are.
size_t currentEventEndOffset(XML_Parser p) {
  const XML_Index start = XML_GetCurrentByteIndex(p);
  if (start < 0) return 0;
  return static_cast<size_t>(start) + static_cast<size_t>(XML_GetCurrentByteCount(p));
}

bool isReopenableBodyWrapper(const std::string& name) {
  return name == "div" || name == "section" || name == "article" || name == "main";
}

void XMLCALL onStart(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* ctx = static_cast<ScanContext*>(userData);
  if (!ctx->sawBody) {
    if (strcmp(name, "body") == 0) {
      ctx->sawBody = true;
      ctx->depthSinceBody = 0;
      ctx->bodyContentStartOffset = currentEventEndOffset(ctx->parser);
      ctx->bodyBoundaries.begin(ctx->bodyContentStartOffset);
    }
    return;
  }
  if (ctx->bodyClosed) return;
  if (ctx->depthSinceBody == 0) {
    ctx->directBodyChildCount++;
    if (ctx->directBodyChildCount == 1) {
      const XML_Index start = XML_GetCurrentByteIndex(ctx->parser);
      ctx->directWrapperStartOffset = start < 0 ? 0 : static_cast<size_t>(start);
      ctx->directWrapperOpenEndOffset = currentEventEndOffset(ctx->parser);
      ctx->directWrapperName = name;
      ctx->wrapperBoundaries.begin(ctx->directWrapperOpenEndOffset);
    }
  }
  ctx->depthSinceBody++;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "id") == 0 && atts[i + 1] && atts[i + 1][0] != '\0') {
      const XML_Index start = XML_GetCurrentByteIndex(ctx->parser);
      if (ctx->anchorOffsets.size() < MAX_TRACKED_ANCHORS) {
        ctx->anchorOffsets.emplace_back(std::string(atts[i + 1]), start < 0 ? 0 : static_cast<size_t>(start));
      } else {
        ctx->anchorLimitReached = true;
      }
      break;
    }
  }
}

void XMLCALL onEnd(void* userData, const XML_Char* name) {
  auto* ctx = static_cast<ScanContext*>(userData);
  if (!ctx->sawBody || ctx->bodyClosed) return;
  if (ctx->depthSinceBody == 0) {
    // This is </body> itself, since depth 0 means "direct child of body"
    // and everything at that depth already decremented back to 0 when
    // its own end tag fired - the only end-tag event left at depth 0 is
    // body's own.
    const XML_Index start = XML_GetCurrentByteIndex(ctx->parser);
    ctx->bodyContentEndOffset = start < 0 ? 0 : static_cast<size_t>(start);
    ctx->bodyClosed = true;
    return;
  }
  if (ctx->depthSinceBody == 1 && ctx->directBodyChildCount == 1 && name == ctx->directWrapperName) {
    const XML_Index start = XML_GetCurrentByteIndex(ctx->parser);
    ctx->directWrapperContentEndOffset = start < 0 ? 0 : static_cast<size_t>(start);
  }
  ctx->depthSinceBody--;
  if (ctx->depthSinceBody == 0) {
    // Just closed a direct child of <body> - a safe place to end a
    // fragment. Record every one of these; the caller decides which
    // subset to actually use as real split points based on accumulated
    // size, so a single scan serves any target fragment size.
    ctx->bodyBoundaries.record(currentEventEndOffset(ctx->parser));
  } else if (ctx->depthSinceBody == 1) {
    // Many EPUB generators wrap the entire chapter in one <div>/<section>.
    // These boundaries are safe once that common wrapper is reopened in each
    // output fragment.
    ctx->wrapperBoundaries.record(currentEventEndOffset(ctx->parser));
  }
}

bool writeFragment(const std::string& sourcePath, const std::string& outPath, const size_t prefixEnd,
                   const size_t wrapperStart, const size_t wrapperEnd, const std::string& closingBytes,
                   const size_t bodyStart, const size_t bodyEnd) {
  HalFile in;
  if (!Storage.openFileForRead("EHS", sourcePath, in)) return false;
  HalFile out;
  if (!Storage.openFileForWrite("EHS", outPath, out)) {
    in.close();
    return false;
  }

  constexpr size_t kBufSize = 4096;
  std::vector<uint8_t> buf(kBufSize);
  const auto copyRange = [&](const size_t start, const size_t end) {
    if (end < start || !in.seek(start)) return false;
    size_t remaining = end - start;
    while (remaining > 0) {
      const size_t want = std::min(remaining, kBufSize);
      const int got = in.read(buf.data(), want);
      if (got <= 0 || out.write(buf.data(), static_cast<size_t>(got)) != static_cast<size_t>(got)) {
        return false;
      }
      remaining -= static_cast<size_t>(got);
    }
    return true;
  };

  bool ok = copyRange(0, prefixEnd);
  if (ok && wrapperEnd > wrapperStart) ok = copyRange(wrapperStart, wrapperEnd);
  if (ok) ok = copyRange(bodyStart, bodyEnd);
  if (ok) ok = out.write(closingBytes.data(), closingBytes.size()) == closingBytes.size();

  in.close();
  out.close();
  if (!ok) Storage.remove(outPath.c_str());
  return ok;
}

}  // namespace

std::vector<std::string> splitToFragments(const std::string& sourcePath, const std::string& outputDir,
                                          const std::string& baseName,
                                          std::unordered_map<std::string, int>* anchorFragmentOut) {
  HalFile source;
  if (!Storage.openFileForRead("EHS", sourcePath, source)) {
    LOG_ERR("EHS", "splitToFragments: can't open %s", sourcePath.c_str());
    return {};
  }
  const size_t fileSize = static_cast<size_t>(source.fileSize64());

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    source.close();
    return {};
  }
  ScanContext ctx;
  ctx.parser = parser;
  XML_SetUserData(parser, &ctx);
  XML_SetElementHandler(parser, onStart, onEnd);

  bool parseOk = true;
  constexpr size_t kChunkSize = 4096;
  std::vector<char> chunk(kChunkSize);
  for (;;) {
    const int got = source.read(chunk.data(), chunk.size());
    if (got < 0) {
      parseOk = false;
      break;
    }
    const bool isFinal = got == 0;
    if (XML_Parse(parser, chunk.data(), got, isFinal) == XML_STATUS_ERROR) {
      LOG_ERR("EHS", "splitToFragments: XML parse error in %s at byte %ld: %s", sourcePath.c_str(),
              static_cast<long>(XML_GetCurrentByteIndex(parser)), XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
      break;
    }
    if (isFinal) break;
  }
  source.close();
  XML_ParserFree(parser);

  if (ctx.anchorLimitReached) {
    LOG_INF("EHS", "Anchor map capped at %u entries while splitting %s",
            static_cast<unsigned>(MAX_TRACKED_ANCHORS), sourcePath.c_str());
  }

  if (!parseOk || !ctx.sawBody) {
    if (!ctx.sawBody) LOG_ERR("EHS", "splitToFragments: no <body> found in %s", sourcePath.c_str());
    return {};
  }

  auto chooseBoundaries = [](const BoundaryTracker& tracker, const size_t contentStart, const size_t contentEnd) {
    std::vector<size_t> chosen = tracker.chosen;
    while (!chosen.empty() &&
           (chosen.back() >= contentEnd || contentEnd - chosen.back() < MIN_FRAGMENT_BYTES)) {
      chosen.pop_back();
    }

    // A two-block 300 KiB chapter commonly has a 150/150 KiB boundary: no
    // candidate reaches the 200 KiB target, but splitting it in half is still
    // substantially safer than passing the full file to pagination.
    if (chosen.empty() && contentEnd > contentStart && contentEnd - contentStart >= SPLIT_THRESHOLD_BYTES) {
      const size_t fallback = tracker.fallbackCandidate;
      if (fallback > contentStart && fallback < contentEnd && fallback - contentStart >= MIN_FRAGMENT_BYTES &&
          contentEnd - fallback >= MIN_FRAGMENT_BYTES) {
        chosen.push_back(fallback);
      }
    }

    if (contentEnd > contentStart) chosen.push_back(contentEnd);
    return chosen;
  };

  size_t fragmentStart = ctx.bodyContentStartOffset;
  if (ctx.bodyContentEndOffset <= fragmentStart) return {};
  const size_t bodyContentEnd = ctx.bodyContentEndOffset;
  std::vector<size_t> boundaries = chooseBoundaries(ctx.bodyBoundaries, fragmentStart, bodyContentEnd);
  bool reopenDirectWrapper = false;
  if (ctx.directBodyChildCount == 1 && isReopenableBodyWrapper(ctx.directWrapperName) &&
      ctx.directWrapperOpenEndOffset > ctx.directWrapperStartOffset &&
      ctx.directWrapperContentEndOffset > ctx.directWrapperOpenEndOffset) {
    auto wrappedBoundaries = chooseBoundaries(ctx.wrapperBoundaries, ctx.directWrapperOpenEndOffset,
                                              ctx.directWrapperContentEndOffset);
    if (wrappedBoundaries.size() >= 2) {
      boundaries = std::move(wrappedBoundaries);
      fragmentStart = ctx.directWrapperOpenEndOffset;
      reopenDirectWrapper = true;
    }
  }
  if (boundaries.size() < 2) {
    LOG_ERR("EHS", "splitToFragments: no repeatable safe boundary in %s", sourcePath.c_str());
    return {};
  }

  std::string closingBytes = "</body></html>";
  size_t wrapperStart = 0;
  size_t wrapperEnd = 0;
  if (reopenDirectWrapper) {
    const size_t wrapperBytes = ctx.directWrapperOpenEndOffset - ctx.directWrapperStartOffset;
    constexpr size_t kMaxWrapperTagBytes = 4096;
    if (wrapperBytes > kMaxWrapperTagBytes) return {};
    wrapperStart = ctx.directWrapperStartOffset;
    wrapperEnd = ctx.directWrapperOpenEndOffset;
    closingBytes = "</" + ctx.directWrapperName + "></body></html>";
  }

  std::vector<std::string> fragmentNames;
  std::vector<std::pair<size_t, size_t>> fragmentRanges;  // parallel to fragmentNames
  for (size_t i = 0; i < boundaries.size(); ++i) {
    const size_t fragmentEnd = boundaries[i];
    if (fragmentEnd <= fragmentStart) continue;  // shouldn't happen, but never emit an empty/inverted fragment
    // Epub::getLogicalChapterBounds recognises this explicit suffix and
    // presents all internal fragments as one visible chapter.
    const std::string fragmentName = baseName + "_split" + std::to_string(fragmentNames.size()) + ".xhtml";
    const std::string outPath = outputDir + "/" + fragmentName;
    if (!writeFragment(sourcePath, outPath, ctx.bodyContentStartOffset, wrapperStart, wrapperEnd, closingBytes,
                       fragmentStart, fragmentEnd)) {
      LOG_ERR("EHS", "splitToFragments: failed writing fragment %s", outPath.c_str());
      return {};
    }
    fragmentNames.push_back(fragmentName);
    fragmentRanges.emplace_back(fragmentStart, fragmentEnd);
    fragmentStart = fragmentEnd;
  }

  if (anchorFragmentOut) {
    for (const auto& [id, offset] : ctx.anchorOffsets) {
      for (size_t i = 0; i < fragmentRanges.size(); ++i) {
        if (offset >= fragmentRanges[i].first && offset < fragmentRanges[i].second) {
          (*anchorFragmentOut)[id] = static_cast<int>(i);
          break;
        }
      }
    }
  }

  LOG_INF("EHS", "Split %s (%zu bytes) into %zu fragments", sourcePath.c_str(), fileSize, fragmentNames.size());
  return fragmentNames;
}

}  // namespace EpubStreamingChapterSplitter
