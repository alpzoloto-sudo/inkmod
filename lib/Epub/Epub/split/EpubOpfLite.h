#pragma once

#include <string>
#include <vector>

// A minimal, read-and-rewrite view of content.opf's <manifest>/<spine> -
// just enough for EpubChapterSplitter to find an oversized spine item and
// splice in replacement entries for its split parts. Not a general OPF
// reader: metadata, guide references, and anything else BookMetadataCache's
// own ContentOpfParser handles for normal reading isn't touched here at
// all - this only ever runs once, ahead of that, to rewrite the manifest/
// spine XML itself before the normal reading path ever sees it.
struct EpubOpfManifestItem {
  std::string id;
  std::string href;
  std::string mediaType;
};

struct EpubOpfLite {
  std::vector<EpubOpfManifestItem> manifest;  // document order
  std::vector<std::string> spineIdrefs;       // document (reading) order
  std::string tocNcxItemId;                   // <spine toc="..."> value, if present

  // Parses opfContent (the whole content.opf file, as read into memory) via
  // expat. Returns false on a genuine XML parse failure; a structurally
  // valid OPF with an empty/unexpected manifest or spine is not itself a
  // failure (the caller checks for what it needs and gives up gracefully
  // if it's missing, same as it would for any other unsupported OPF shape).
  static bool parse(const std::string& opfContent, EpubOpfLite& out);
};
