#!/usr/bin/env python3
from pathlib import Path

if "__file__" in globals():
    ROOT = Path(__file__).resolve().parents[1]
else:
    # PlatformIO executes pre-scripts through SCons where __file__ is not
    # guaranteed to exist. Its working directory is the project root.
    ROOT = Path.cwd().resolve()

FB2_CPP = ROOT / "lib/Fb2/Fb2.cpp"
PARSER_CPP = ROOT / "lib/Fb2/native/Fb2Parser.cpp"
PARSER_H = ROOT / "lib/Fb2/native/Fb2Parser.h"
EPUB_PARSER_CPP = ROOT / "lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp"


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    # Check the old target first. Some replacements intentionally collapse a
    # larger block to a line that already occurs inside that old block, so
    # testing `new in text` first would incorrectly skip the replacement.
    count = text.count(old)
    if count == 1:
        path.write_text(text.replace(old, new, 1), encoding="utf-8")
        return
    if count == 0 and new in text:
        return
    raise SystemExit(f"{path}: expected exactly one patch target, found {count}")


# v16 forces old FB2 packages/layout pages to rebuild. v15 put the cover first
# but still allowed the first body text to use leftover space under the cover.
replace_once(
    FB2_CPP,
    "constexpr uint8_t PACKAGE_VERSION = 14;  // 24 KiB virtual text chunks; invalidates older package indexes",
    "constexpr uint8_t PACKAGE_VERSION = 16;  // dedicated first cover page; invalidates older package indexes",
)

# The cover is metadata, not a body illustration. It must not consume the
# image ordinal used to split illustration-heavy body sections.
replace_once(
    PARSER_CPP,
    '''    // The cover is emitted through the same image-range filter as body\n    // illustrations. When the first section already contains images, include\n    // the cover in that section's ordinal count so virtual image slices keep\n    // exactly the same boundaries and no body illustration is skipped.\n    if (!out.metadata.coverBinaryId.empty() && !out.sections.empty() &&\n        out.sections.front().imageRefCount > 0 && out.sections.front().imageRefCount < UINT16_MAX) {\n        ++out.sections.front().imageRefCount;\n    }\n    return true;\n''',
    '''    // Coverpage belongs to FB2 metadata, not to the body image stream.\n    // Keep imageRefCount limited to real section illustrations so virtual\n    // image slices are stable regardless of whether a book has a cover.\n    return true;\n''',
)

# Expose a tiny cover-only pass so Fb2::renderChapterOnDemand can write the
# cover before its generated title/annotation, while renderSection remains
# strictly body content and never duplicates the cover.
replace_once(
    PARSER_H,
    '''    // sectionIndex must be an index into the vector scan() filled in.\n    bool renderSection(IByteReader& reader,\n                        const Fb2SectionIndexEntry& section,\n                        Fb2ContentSink& sink,\n                        const reader::ReaderCancellationToken* cancellationToken = nullptr);\n''',
    '''    // Emits the metadata cover only when `section` is the first body\n    // section. This is intentionally separate from renderSection() so callers\n    // can place the cover before generated title/annotation content.\n    bool renderCoverForFirstSection(IByteReader& reader,\n                                    const Fb2SectionIndexEntry& section,\n                                    Fb2ContentSink& sink);\n\n    // sectionIndex must be an index into the vector scan() filled in.\n    // Body content only: coverpage is handled by renderCoverForFirstSection().\n    bool renderSection(IByteReader& reader,\n                        const Fb2SectionIndexEntry& section,\n                        Fb2ContentSink& sink,\n                        const reader::ReaderCancellationToken* cancellationToken = nullptr);\n''',
)

replace_once(
    PARSER_CPP,
    '''bool Fb2Parser::renderSection(IByteReader& reader,\n                               const Fb2SectionIndexEntry& section,\n                               Fb2ContentSink& sink,\n                               const reader::ReaderCancellationToken* cancellationToken) {\n    // Emit the FB2 <coverpage> only before the first body section. The outer\n    // range sinks suppress this event for later virtual slices of that section;\n    // for image-sliced sections scan() counted the cover above so ordinals stay aligned.\n    std::string coverId;\n    if (findCoverForFirstSection(reader, section.innerStartOffset, coverId)) {\n        sink.onImage(coverId);\n    }\n\n    Fb2XmlReader xml(reader, 4096);\n''',
    '''bool Fb2Parser::renderCoverForFirstSection(IByteReader& reader,\n                                                  const Fb2SectionIndexEntry& section,\n                                                  Fb2ContentSink& sink) {\n    std::string coverId;\n    if (!findCoverForFirstSection(reader, section.innerStartOffset, coverId)) return false;\n    sink.onImage(coverId);\n    return true;\n}\n\nbool Fb2Parser::renderSection(IByteReader& reader,\n                               const Fb2SectionIndexEntry& section,\n                               Fb2ContentSink& sink,\n                               const reader::ReaderCancellationToken* cancellationToken) {\n    Fb2XmlReader xml(reader, 4096);\n''',
)

# ChapterHtmlSlimParser already owns the only safe primitive for finalizing a
# partially-filled page. Add a private inkMOD marker understood by generated
# FB2 XHTML so the cover can end page 1 without fake blank paragraphs or a
# viewport-size guess. Normal EPUB content is unchanged.
parser_text = EPUB_PARSER_CPP.read_text(encoding="utf-8")
if "inkmod-fb2-cover-page-break" not in parser_text:
    replace_once(
        EPUB_PARSER_CPP,
        '''  auto centeredBlockStyle = BlockStyle();\n''',
        '''  // Synthetic FB2 packages use this private marker immediately after\n  // their metadata cover. Finalize the cover page and start a clean page for\n  // title/annotation/body text. It deliberately bypasses CSS so publisher\n  // styles cannot accidentally disable the boundary.\n  if (attributeContainsToken(classAttr.c_str(), "inkmod-fb2-cover-page-break")) {\n    if (self->partWordBufferIndex > 0 && self->currentTextBlock) {\n      self->flushPartWordBuffer();\n    }\n    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {\n      self->makePages();\n    }\n    self->currentTextBlock.reset();\n    if (self->currentPage && !self->currentPage->elements.empty()) {\n      self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex, self->xpathListItemIndex);\n      self->completedPageCount++;\n    }\n    if (!self->startNewPage("FB2 cover page break")) {\n      return;\n    }\n    self->skipCurrentElement();\n    return;\n  }\n\n  auto centeredBlockStyle = BlockStyle();\n''',
    )

# Reorder generated chapter XHTML to match EPUB semantics:
#   cover -> forced page boundary -> chapter heading/annotation -> section body.
# The cover goes straight to StreamSink, outside both virtual range filters,
# so it cannot shift body illustration ordinals or text-slice accounting.
#
# PlatformIO runs this pre-script once per environment. CI builds developer and
# release sequentially in the same checkout, so this compound transformation
# must be guarded as a unit: after developer has applied it, release must leave
# the already-patched source untouched. Use the generated marker itself rather
# than the exact C++ call syntax: the call lives inside an if-expression after
# the first application, so a semicolon-based marker is not stable.
fb2_text = FB2_CPP.read_text(encoding="utf-8")
cover_block_marker = "inkmod-fb2-cover-page-break"
if cover_block_marker not in fb2_text:
    old_block = '''  if (!title.empty()) {\n    const int heading = std::min(std::max(static_cast<int>(level) + 1, 1), 6);\n    writeHeadingTag(out, heading, false);\n    writeXmlEscaped(out, title);\n    writeHeadingTag(out, heading, true);\n  }\n\n  // An FB2 annotation belongs to <description>, not to a body section.  It\n'''
    new_block = '''  Fb2SectionIndexEntry section;  // only innerStartOffset is read by renderSection()\n  section.innerStartOffset = innerStartOffset;\n  section.level = level;\n  Fb2Parser parser;\n  StreamSink sink(out, packageCachePath + IMAGES_INDEX_FILE, buildLinkResolver(packageCachePath));\n\n  // FB2 stores coverpage in <description>, outside the body. Emit it as the\n  // very first visual content, matching normal EPUB opening behaviour. If a\n  // real cover was emitted, the private marker below gives it a dedicated\n  // first page before generated title/annotation/body content.\n  if (chapterIndex == 0 && parser.renderCoverForFirstSection(reader, section, sink)) {\n    writeBytes(out, "<div class=\\\"inkmod-fb2-cover-page-break\\\"></div>");\n  }\n\n  if (!title.empty()) {\n    const int heading = std::min(std::max(static_cast<int>(level) + 1, 1), 6);\n    writeHeadingTag(out, heading, false);\n    writeXmlEscaped(out, title);\n    writeHeadingTag(out, heading, true);\n  }\n\n  // An FB2 annotation belongs to <description>, not to a body section.  It\n'''
    replace_once(FB2_CPP, old_block, new_block)

    replace_once(
        FB2_CPP,
        '''  Fb2SectionIndexEntry section;  // only innerStartOffset is read by renderSection()\n  section.innerStartOffset = innerStartOffset;\n  section.level = level;\n  Fb2Parser parser;\n  StreamSink sink(out, packageCachePath + IMAGES_INDEX_FILE, buildLinkResolver(packageCachePath));\n  RangeFilterSink imageRangeSink(sink, imageRangeStart, imageRangeEnd);\n''',
        '''  RangeFilterSink imageRangeSink(sink, imageRangeStart, imageRangeEnd);\n''',
    )

print("FB2 cover-order/page-break patch applied")
