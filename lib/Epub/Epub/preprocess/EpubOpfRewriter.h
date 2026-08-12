#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// Targeted text-level rewriting of an EPUB content.opf: replaces the
// <item>/<itemref> pair for one oversized spine file with one pair per
// split fragment, leaving everything else in the OPF - metadata, guide,
// every other manifest/spine entry - byte-for-byte as the original author
// wrote it. Deliberately not a full parse-modify-reserialize: OPF files in
// the wild vary a lot in formatting and this only ever touches the one
// <item> and one <itemref> it's told to.
namespace EpubOpfRewriter {

// `opfContent` is the original content.opf, read whole (these are small -
// even a complex book's is rarely more than a few KB). `originalHref` is
// the manifest href of the oversized spine file (e.g. "book0.html").
// `originalItemId` is filled in with that item's id="" attribute, needed
// by the caller to redirect toc.ncx entries that reference it.
// `fragmentHrefs` are the split fragment filenames, in document order,
// living alongside the original (same directory). `mediaType` is reused
// from the original item's own media-type attribute.
//
// Returns the rewritten OPF content, or an empty string if `originalHref`
// wasn't found in the manifest/spine (caller should treat that as "don't
// preprocess this book" rather than write a broken OPF).
std::string rewriteForSplitItem(const std::string& opfContent, const std::string& originalHref,
                                const std::vector<std::string>& fragmentHrefs, std::string* originalItemIdOut);

}  // namespace EpubOpfRewriter

// Redirects every toc.ncx <content src="originalHref"/> or
// <content src="originalHref#anchor"/> reference to whichever split
// fragment now holds that anchor (via anchorFragment, built by
// EpubChapterSplitter::splitToFragments()) - or fragment 0 if the anchor
// isn't in the map (a link straight to the file with no anchor, or one
// pointing at an id the scan didn't see for some reason; landing on the
// first fragment beats leaving a link to a file that no longer exists).
// Every other <content src="..."/> in the file is left untouched.
namespace EpubNcxRewriter {
std::string redirectReferences(const std::string& ncxContent, const std::string& originalHref,
                               const std::vector<std::string>& fragmentHrefs,
                               const std::unordered_map<std::string, int>& anchorFragment);
}  // namespace EpubNcxRewriter
