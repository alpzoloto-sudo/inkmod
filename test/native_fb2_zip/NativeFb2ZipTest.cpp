#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Base64Decoder.h"
#include "Fb2XmlReader.h"
#include "ZipReader.h"

namespace {

class MemoryReader final : public IByteReader {
 public:
  explicit MemoryReader(std::vector<uint8_t> data) : data_(std::move(data)) {}
  explicit MemoryReader(const std::string& data) : data_(data.begin(), data.end()) {}

  size_t read(void* dst, size_t len) override {
    const size_t available = position_ < data_.size() ? data_.size() - position_ : 0;
    const size_t count = std::min(len, available);
    if (count != 0) std::memcpy(dst, data_.data() + position_, count);
    position_ += count;
    return count;
  }

  bool seek(uint64_t pos) override {
    if (pos > data_.size()) return false;
    position_ = static_cast<size_t>(pos);
    return true;
  }

  uint64_t tell() const override { return position_; }
  uint64_t size() const override { return data_.size(); }

 private:
  std::vector<uint8_t> data_;
  size_t position_ = 0;
};

void put16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 24));
}

std::vector<uint8_t> makeZip(const std::string& filename,
                             const std::vector<uint8_t>& compressed,
                             uint32_t uncompressedSize,
                             uint16_t method,
                             size_t commentSize) {
  std::vector<uint8_t> zip;

  const uint32_t localOffset = static_cast<uint32_t>(zip.size());
  put32(zip, 0x04034b50);
  put16(zip, 20);
  put16(zip, 0);
  put16(zip, method);
  put16(zip, 0); put16(zip, 0);
  put32(zip, 0);
  put32(zip, static_cast<uint32_t>(compressed.size()));
  put32(zip, uncompressedSize);
  put16(zip, static_cast<uint16_t>(filename.size()));
  put16(zip, 0);
  zip.insert(zip.end(), filename.begin(), filename.end());
  zip.insert(zip.end(), compressed.begin(), compressed.end());

  const uint32_t centralOffset = static_cast<uint32_t>(zip.size());
  put32(zip, 0x02014b50);
  put16(zip, 20); put16(zip, 20);
  put16(zip, 0);
  put16(zip, method);
  put16(zip, 0); put16(zip, 0);
  put32(zip, 0);
  put32(zip, static_cast<uint32_t>(compressed.size()));
  put32(zip, uncompressedSize);
  put16(zip, static_cast<uint16_t>(filename.size()));
  put16(zip, 0); put16(zip, 0);
  put16(zip, 0); put16(zip, 0);
  put32(zip, 0);
  put32(zip, localOffset);
  zip.insert(zip.end(), filename.begin(), filename.end());
  const uint32_t centralSize = static_cast<uint32_t>(zip.size()) - centralOffset;

  put32(zip, 0x06054b50);
  put16(zip, 0); put16(zip, 0);
  put16(zip, 1); put16(zip, 1);
  put32(zip, centralSize);
  put32(zip, centralOffset);
  put16(zip, static_cast<uint16_t>(commentSize));
  zip.insert(zip.end(), commentSize, static_cast<uint8_t>('x'));
  return zip;
}

std::vector<uint8_t> makeStoredFb2Zip(size_t commentSize) {
  const std::string payload = "<FictionBook><body><section><p>OK</p></section></body></FictionBook>";
  return makeZip("books/Test.FB2",
                 std::vector<uint8_t>(payload.begin(), payload.end()),
                 static_cast<uint32_t>(payload.size()), 0, commentSize);
}

std::vector<uint8_t> makeDeflatedFb2Zip() {
  const std::string payload =
      "<FictionBook><body><section><p>DEFLATE OK</p></section></body></FictionBook>";
  const std::vector<uint8_t> compressed = {
      0xb3, 0x71, 0xcb, 0x4c, 0x2e, 0xc9, 0xcc, 0xcf, 0x73, 0xca, 0xcf, 0xcf,
      0xb6, 0xb3, 0x49, 0xca, 0x4f, 0xa9, 0xb4, 0xb3, 0x29, 0x4e, 0x05, 0x0b,
      0xd9, 0xd9, 0x14, 0xd8, 0xb9, 0xb8, 0xba, 0xf9, 0x38, 0x86, 0xb8, 0x2a,
      0xf8, 0x7b, 0xdb, 0xe8, 0x17, 0xd8, 0xd9, 0xe8, 0xc3, 0xa5, 0xf4, 0x21,
      0x4a, 0xf5, 0x91, 0xb5, 0x03, 0x00};
  return makeZip("book.fb2", compressed, static_cast<uint32_t>(payload.size()), 8, 0);
}

TEST(NativeFb2Zip, FindsAndExtractsStoredEntryWithoutComment) {
  MemoryReader reader(makeStoredFb2Zip(0));
  ZipEntryInfo entry;
  ASSERT_TRUE(findFb2EntryInZip(reader, entry));
  EXPECT_EQ(entry.filename, "books/Test.FB2");
  EXPECT_EQ(entry.method, 0);

  std::string output;
  ASSERT_TRUE(extractZipEntry(reader, entry, [&](const uint8_t* data, size_t len) {
    output.append(reinterpret_cast<const char*>(data), len);
  }));
  EXPECT_NE(output.find("<FictionBook>"), std::string::npos);
  EXPECT_NE(output.find("<p>OK</p>"), std::string::npos);
}

TEST(NativeFb2Zip, FindsAndExtractsDeflatedEntryThroughReadAhead) {
  MemoryReader reader(makeDeflatedFb2Zip());
  ZipEntryInfo entry;
  ASSERT_TRUE(findFb2EntryInZip(reader, entry));
  ASSERT_EQ(entry.method, 8);

  std::string output;
  ASSERT_TRUE(extractZipEntry(reader, entry, [&](const uint8_t* data, size_t len) {
    output.append(reinterpret_cast<const char*>(data), len);
  }));
  EXPECT_EQ(output,
            "<FictionBook><body><section><p>DEFLATE OK</p></section></body></FictionBook>");
}

TEST(NativeFb2Zip, FindsEocdAcrossChunkedCommentSearch) {
  MemoryReader reader(makeStoredFb2Zip(9000));
  ZipEntryInfo entry;
  ASSERT_TRUE(findFb2EntryInZip(reader, entry));
  EXPECT_EQ(entry.filename, "books/Test.FB2");
}

TEST(NativeFb2Base64, PreservesPaddingAndWhitespace) {
  std::string output;
  Base64Decoder decoder([&](const uint8_t* data, size_t len) {
    output.append(reinterpret_cast<const char*>(data), len);
  });
  decoder.feed("TWE=\n", 5);
  decoder.finish();
  EXPECT_EQ(output, "Ma");
}

TEST(NativeFb2Base64, BatchesOutputAcrossTinyInputFeeds) {
  std::string encoded;
  encoded.reserve(800);
  for (int i = 0; i < 200; ++i) encoded += "QUJD";

  std::string output;
  size_t callbackCount = 0;
  Base64Decoder decoder([&](const uint8_t* data, size_t len) {
    ++callbackCount;
    output.append(reinterpret_cast<const char*>(data), len);
  });

  for (size_t pos = 0; pos < encoded.size(); pos += 7) {
    const size_t len = std::min<size_t>(7, encoded.size() - pos);
    decoder.feed(encoded.data() + pos, len);
  }
  decoder.finish();

  ASSERT_EQ(output.size(), 600u);
  for (size_t i = 0; i < output.size(); i += 3) EXPECT_EQ(output.substr(i, 3), "ABC");
  EXPECT_LE(callbackCount, 2u);
}

TEST(NativeFb2Xml, ScanOnlyLongRunKeepsBinaryOffsetsExact) {
  constexpr size_t kPayloadSize = 20000;
  std::string xmlText = "<binary>";
  xmlText.append(kPayloadSize, 'A');
  xmlText += "</binary><p>A&amp;B</p>";

  MemoryReader reader(xmlText);
  Fb2XmlReader xml(reader, 8192);
  xml.setCaptureText(false);

  ASSERT_EQ(xml.next(), Fb2Token::StartTag);
  ASSERT_EQ(xml.name(), "binary");
  EXPECT_EQ(xml.streamPos(), 8u);

  size_t payloadBytes = 0;
  for (;;) {
    const Fb2Token tok = xml.next();
    if (tok == Fb2Token::Text) {
      payloadBytes += xml.textSize();
      continue;
    }
    ASSERT_EQ(tok, Fb2Token::EndTag);
    ASSERT_EQ(xml.name(), "binary");
    EXPECT_EQ(xml.tokenStartOffset(), 8u + kPayloadSize);
    break;
  }
  EXPECT_EQ(payloadBytes, kPayloadSize);
}

TEST(NativeFb2Xml, ScanOnlyLongPathStillCountsEntitiesByDecodedLength) {
  std::string xmlText = "<p>";
  xmlText.append(300, 'x');
  xmlText += "&amp;B</p>";

  MemoryReader reader(xmlText);
  Fb2XmlReader xml(reader, 512);
  xml.setCaptureText(false);

  ASSERT_EQ(xml.next(), Fb2Token::StartTag);
  ASSERT_EQ(xml.name(), "p");

  size_t decodedBytes = 0;
  for (;;) {
    const Fb2Token tok = xml.next();
    if (tok == Fb2Token::Text) {
      decodedBytes += xml.textSize();
      continue;
    }
    ASSERT_EQ(tok, Fb2Token::EndTag);
    break;
  }
  EXPECT_EQ(decodedBytes, 302u);  // 300 x + '&' + 'B'
}

}  // namespace
