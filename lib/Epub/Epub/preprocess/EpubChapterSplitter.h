#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Splits an oversized XHTML spine item from a real (not FB2-converted) EPUB
// into several smaller, independently-valid XHTML fragment files.
//
// Why this exists: ChapterHtmlSlimParser parses one whole spine item in a
// single pass before any of it can be shown - fine for a normal chapter
// (tens of KB), but some real-world EPUBs (particularly ones exported by
// tools that don't split by chapter) put an entire book, or a whole story
// collection, into ONE spine item several megabytes long. On this
// hardware that's not a crash so much as a multi-minute stall - the parser
// is doing real, unavoidable work (tokenizing, hyphenating, laying out
// hundreds of thousands of words) before it can render page 1.
//
// This runs once, cached, before the reader ever sees the book - see
// EpubPreprocessor, which decides whether a book needs this at all and
// rewrites content.opf/toc.ncx to reference the resulting fragments
// instead of the original oversized file. Everything downstream (Epub,
// BookMetadataCache, Section, ChapterHtmlSlimParser) is completely
// unmodified: it just sees more, smaller spine items than the book
// shipped with.
namespace EpubStreamingChapterSplitter {

// A spine item bigger than this (bytes) is a split candidate. Well below
// where real trouble starts (the book that prompted this was 4.5MB), but
// large enough that ordinary chapters - even unusually long ones - are
// never touched.
constexpr size_t SPLIT_THRESHOLD_BYTES = 300 * 1024;

// Target size for each fragment once a file has been judged split-worthy.
// Not a hard cap - actual fragments end wherever the nearest safe
// boundary (a direct child of <body> closing) falls at or after this many
// bytes, so a book with e.g. one huge <div> per story still splits
// cleanly between stories rather than not at all.
constexpr size_t TARGET_FRAGMENT_BYTES = 200 * 1024;

// Splits `sourcePath` (an XHTML file) into fragments written to
// `outputDir` as "<baseName>_0.xhtml", "<baseName>_1.xhtml", etc.
// `baseName` should be filesystem-safe (no slashes) - callers typically
// derive it from the original file's own name. Returns the fragment
// filenames (just the name, not a full path) in document order, or an
// empty vector on failure (source unreadable, not valid XML, or - very
// rare for real content - no safe split point found at all, meaning the
// whole file is effectively one giant top-level element).
//
// `anchorFragmentOut`, if non-null, is filled with every element id=""
// found anywhere in <body> mapped to the (0-based) index of the fragment
// it ended up in - anchor ids themselves stay untouched by the split
// (each fragment keeps whatever ids its content already had), so a
// toc.ncx/internal link that pointed at "original.html#someid" just needs
// its filename half redirected to whichever fragment this map says.
std::vector<std::string> splitToFragments(const std::string& sourcePath, const std::string& outputDir,
                                          const std::string& baseName,
                                          std::unordered_map<std::string, int>* anchorFragmentOut = nullptr);

}  // namespace EpubStreamingChapterSplitter
