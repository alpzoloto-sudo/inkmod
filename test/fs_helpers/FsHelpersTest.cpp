#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "FsHelpers/FsHelpers.h"

TEST(FsHelpers, MatchesCommonExtensionsCaseInsensitively) {
  EXPECT_TRUE(FsHelpers::hasEpubExtension("BOOK.EPUB"));
  EXPECT_TRUE(FsHelpers::hasXtcExtension("title.XtCh"));
  EXPECT_TRUE(FsHelpers::hasJpgExtension("cover.JPEG"));
  EXPECT_TRUE(FsHelpers::hasPngExtension("cover.PnG"));
  EXPECT_TRUE(FsHelpers::hasMarkdownExtension("notes.MD"));
  EXPECT_FALSE(FsHelpers::hasEpubExtension("book.epub.bak"));
  EXPECT_FALSE(FsHelpers::hasTxtExtension("txt"));
}

TEST(FsHelpers, NaturalSortKeepsDirectoriesFirstAndOrdersNumbersNumerically) {
  std::vector<std::string> files = {
      "book10.epub", "Zoo/", "book2.epub", "alpha10.txt", "Folder/", "Alpha2.txt"};

  FsHelpers::sortFileList(files);

  const std::vector<std::string> expected = {
      "Folder/", "Zoo/", "Alpha2.txt", "alpha10.txt", "book2.epub", "book10.epub"};
  EXPECT_EQ(files, expected);
}

TEST(FsHelpers, DecodesUriEscapesWithoutTouchingPlainUtf8Bytes) {
  EXPECT_EQ(FsHelpers::decodeUriEscapes("Books%20and%20Notes"), "Books and Notes");
  EXPECT_EQ(FsHelpers::decodeUriEscapes("Книги/Тест.epub"), "Книги/Тест.epub");
  EXPECT_EQ(FsHelpers::decodeUriEscapes("bad%2Xpath"), "bad%2Xpath");
}

TEST(FsHelpers, NormalisePathPreservesLegacySemanticsWithoutComponentAllocations) {
  EXPECT_EQ(FsHelpers::normalisePath("/OEBPS//Text/chapter.xhtml"), "OEBPS/Text/chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("OEBPS/Text/../Images/cover.jpg"), "OEBPS/Images/cover.jpg");
  EXPECT_EQ(FsHelpers::normalisePath("../OEBPS/Text/chapter.xhtml"), "OEBPS/Text/chapter.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("Книги/Раздел/../Глава.xhtml"), "Книги/Глава.xhtml");

  // Historical behavior: only a '..' terminated by '/' is resolved. Keep a
  // trailing '..' literal so optimized path handling cannot silently change
  // compatibility with existing callers/cache keys.
  EXPECT_EQ(FsHelpers::normalisePath("OEBPS/Text/.."), "OEBPS/Text/..");
  EXPECT_EQ(FsHelpers::normalisePath("a/b/../../c"), "c");
}
