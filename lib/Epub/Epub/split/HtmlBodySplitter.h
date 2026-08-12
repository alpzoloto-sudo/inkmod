#pragma once

#include <string>
#include <vector>

// Splits one XHTML document's <body> content into several chunks at safe,
// top-level boundaries (i.e. never inside a nested tag), each under a
// target byte budget. Used for the rare EPUB whose publisher/converter put
// an entire book (or a whole story collection) into one spine item too
// large for this device to tokenize/paginate in one pass - splitting it
// into several normal-sized ones lets it go through the exact same
// ChapterHtmlSlimParser/Section pipeline every other chapter already does,
// unchanged.
//
// Deliberately NOT a real HTML/XML parser: it only tracks open/close tag
// *depth* relative to <body>, treating "depth==1 tag boundary" as the only
// thing that matters for safety. It doesn't understand what any tag means,
// doesn't validate nesting is well-formed, and doesn't touch anything
// inside a tag it isn't splitting on. That's deliberate - the input is a
// real book's real XHTML, already valid, and the only thing this needs to
// guarantee is that it never cuts a tag in half.
class HtmlBodySplitter {
 public:
  struct Chunk {
    std::string html;                    // this chunk's slice of body content, unwrapped
    std::vector<std::string> idsInChunk;  // every id="..." found in this chunk, in document order
  };

  // targetChunkBytes: soft budget per chunk - a chunk closes at the first
  // top-level boundary at or past this size, so actual chunk sizes vary
  // (a single huge top-level element bigger than the whole budget becomes
  // its own oversized chunk rather than being cut unsafely).
  //
  // bodyContent: everything between (not including) the document's <body ...>
  // and </body> tags.
  //
  // Returns false only if bodyContent is too malformed to find any safe
  // split point at all (e.g. unbalanced tags) - callers should fall back to
  // treating the file as unsplittable in that case, not retry or guess.
  static bool split(const std::string& bodyContent, size_t targetChunkBytes, std::vector<Chunk>& outChunks);
};
