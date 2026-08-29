#include "ProgressiveJpegDecoder.h"

#include <Arduino.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

namespace {

constexpr uint8_t M_SOI = 0xD8;
constexpr uint8_t M_EOI = 0xD9;
constexpr uint8_t M_SOS = 0xDA;
constexpr uint8_t M_DQT = 0xDB;
constexpr uint8_t M_DHT = 0xC4;
constexpr uint8_t M_DRI = 0xDD;
constexpr uint8_t M_SOF2 = 0xC2;
constexpr uint8_t M_RST0 = 0xD0;
constexpr uint8_t M_RST7 = 0xD7;
constexpr int MAX_COMPONENTS = 3;
constexpr int BLOCK_COEFFS = 64;
constexpr size_t BLOCK_BYTES = BLOCK_COEFFS * sizeof(int16_t);
constexpr int MAX_DIMENSION = 4096;
constexpr int64_t MAX_PIXELS = 16LL * 1024LL * 1024LL;

constexpr uint8_t kZigZag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

// C(u)*cos((2*x+1)u*pi/16), Q14. Used by the separable integer IDCT.
constexpr int16_t kIdctBasis[8][8] = {
    {11585, 16069, 15137, 13623, 11585,  9102,  6270,  3196},
    {11585, 13623,  6270, -3196,-11585,-16069,-15137, -9102},
    {11585,  9102, -6270,-16069,-11585,  3196, 15137, 13623},
    {11585,  3196,-15137, -9102, 11585, 13623, -6270,-16069},
    {11585, -3196,-15137,  9102, 11585,-13623, -6270, 16069},
    {11585, -9102, -6270, 16069,-11585, -3196, 15137,-13623},
    {11585,-13623,  6270,  3196,-11585, 16069,-15137,  9102},
    {11585,-16069, 15137,-13623, 11585, -9102,  6270, -3196},
};

struct HuffTable {
  bool valid{false};
  int32_t minCode[17]{};
  int32_t maxCode[17]{};
  int32_t valPtr[17]{};
  uint8_t values[256]{};
  int valueCount{0};
};

struct Component {
  uint8_t id{0};
  uint8_t h{1};
  uint8_t v{1};
  uint8_t q{0};
  int actualBlockCols{0};
  int actualBlockRows{0};
  int paddedBlockCols{0};
  int paddedBlockRows{0};
};

struct ScanComponent {
  int comp{-1};
  uint8_t dcTable{0};
  uint8_t acTable{0};
};

struct Scan {
  int count{0};
  ScanComponent comps[MAX_COMPONENTS];
  uint8_t ss{0};
  uint8_t se{0};
  uint8_t ah{0};
  uint8_t al{0};
};

class Decoder {
 public:
  Decoder(FsFile& source, JPEG_DRAW_CALLBACK* cb, void* user, ProgressiveJpegInfo& info,
          int targetWidth, int targetHeight)
      : src_(source), cb_(cb), user_(user), info_(info), targetWidth_(targetWidth), targetHeight_(targetHeight) {}

  bool run() {
    const uint32_t originalPos = static_cast<uint32_t>(src_.position());
    struct RestorePosition {
      FsFile& f;
      uint32_t p;
      ~RestorePosition() { f.seek(p); }
    } restore{src_, originalPos};

    if (!src_.seek(0)) return fail("seek source");
    const int a = readRaw();
    const int b = readRaw();
    if (a != 0xFF || b != M_SOI) return fail("missing SOI");

    while (!failed_) {
      int marker = pendingMarker_ >= 0 ? takePendingMarker() : nextMarker();
      if (marker < 0) return fail("unexpected EOF before EOI");
      if (marker == M_EOI) break;
      if (marker >= M_RST0 && marker <= M_RST7) continue;
      if (marker == 0x01) continue;  // TEM, no payload

      uint16_t len = 0;
      if (!readU16(len) || len < 2) return fail("bad marker length");
      size_t payload = len - 2;

      switch (marker) {
        case M_DQT:
          if (!parseDqt(payload)) return false;
          break;
        case M_DHT:
          if (!parseDht(payload)) return false;
          break;
        case M_DRI:
          if (!parseDri(payload)) return false;
          break;
        case M_SOF2:
          if (!parseSof2(payload)) return false;
          break;
        case M_SOS:
          if (!frameReady_) return fail("SOS before SOF2");
          if (!ensureCoeffStore()) return false;
          if (!parseAndDecodeScan(payload)) return false;
          break;
        default:
          if (!skipRaw(payload)) return fail("truncated marker payload");
          break;
      }
    }

    if (!frameReady_ || !sawScan_) return fail("incomplete progressive JPEG");
    if (!renderLuma()) return false;
    info_.width = width_;
    info_.height = height_;
    info_.scaleDenom = scaleDenom_;
    info_.scaledWidth = (width_ + scaleDenom_ - 1) / scaleDenom_;
    info_.scaledHeight = (height_ + scaleDenom_ - 1) / scaleDenom_;
    info_.scans = scanCount_;
    cleanupTemp();
    return true;
  }

  ~Decoder() { cleanupTemp(); }

 private:
  FsFile& src_;
  JPEG_DRAW_CALLBACK* cb_{nullptr};
  void* user_{nullptr};
  ProgressiveJpegInfo& info_;
  int targetWidth_{0};
  int targetHeight_{0};

  int width_{0};
  int height_{0};
  int precision_{0};
  int compCount_{0};
  Component comps_[MAX_COMPONENTS];
  int maxH_{1};
  int maxV_{1};
  int yComp_{0};
  int mcuCols_{0};
  int mcuRows_{0};
  int scaleDenom_{1};

  uint16_t quant_[4][64]{};
  bool quantValid_[4]{};
  HuffTable dc_[4];
  HuffTable ac_[4];
  uint16_t restartInterval_{0};

  FsFile coeff_;
  std::string coeffPath_;
  bool coeffReady_{false};
  bool frameReady_{false};
  bool sawScan_{false};
  bool failed_{false};
  int pendingMarker_{-1};
  int scanCount_{0};

  // DC scans are processed through a small MCU-row cache. Unlike the former
  // all-or-nothing full-image cache, this remains fast even for very large JPEGs:
  // memory usage is O(image width), not O(total block count).
  int16_t* dcRowCache_{nullptr};
  size_t dcRowCacheBlocks_{0};

  // Entropy state. We intentionally keep no read-ahead buffer: when a scan
  // ends the underlying file is exactly at the marker, which makes restart
  // handling and the marker parser deterministic on tiny systems.
  uint8_t entropyByte_{0};
  int entropyBits_{0};
  uint32_t eobRun_{0};
  int dcPred_[MAX_COMPONENTS]{};

  bool fail(const char* what) {
    if (!failed_) LOG_ERR("PJPG", "Progressive decode error: %s (pos=%u)", what, static_cast<unsigned>(src_.position()));
    failed_ = true;
    return false;
  }

  int readRaw() { return src_.read(); }

  bool readU16(uint16_t& v) {
    const int a = readRaw();
    const int b = readRaw();
    if (a < 0 || b < 0) return false;
    v = static_cast<uint16_t>((a << 8) | b);
    return true;
  }

  bool skipRaw(size_t n) {
    const size_t pos = src_.position();
    if (pos + n > src_.size()) return false;
    return src_.seek(pos + n);
  }

  int takePendingMarker() {
    const int m = pendingMarker_;
    pendingMarker_ = -1;
    return m;
  }

  int nextMarker() {
    int c;
    do {
      c = readRaw();
      if (c < 0) return -1;
    } while (c != 0xFF);
    do {
      c = readRaw();
      if (c < 0) return -1;
    } while (c == 0xFF);
    if (c == 0x00) return nextMarker();
    return c;
  }

  bool parseDqt(size_t n) {
    while (n > 0) {
      int pqTq = readRaw();
      if (pqTq < 0 || n < 1) return fail("truncated DQT");
      --n;
      const int precision = pqTq >> 4;
      const int table = pqTq & 0x0F;
      if (table > 3 || precision > 1) return fail("unsupported DQT");
      const size_t bytes = precision ? 128 : 64;
      if (n < bytes) return fail("short DQT");
      for (int k = 0; k < 64; ++k) {
        uint16_t qv;
        if (precision) {
          if (!readU16(qv)) return fail("truncated DQT16");
        } else {
          int v = readRaw();
          if (v < 0) return fail("truncated DQT8");
          qv = static_cast<uint16_t>(v);
        }
        quant_[table][kZigZag[k]] = qv;
      }
      quantValid_[table] = true;
      n -= bytes;
    }
    return true;
  }

  bool buildHuff(HuffTable& h, const uint8_t counts[16], const uint8_t* vals, int count) {
    h = {};
    int code = 0;
    int k = 0;
    for (int len = 1; len <= 16; ++len) {
      const int c = counts[len - 1];
      if (c) {
        h.valPtr[len] = k;
        h.minCode[len] = code;
        code += c - 1;
        h.maxCode[len] = code;
        ++code;
        k += c;
      } else {
        h.minCode[len] = -1;
        h.maxCode[len] = -1;
        h.valPtr[len] = k;
      }
      code <<= 1;
    }
    if (count > 256 || k != count) return false;
    memcpy(h.values, vals, count);
    h.valueCount = count;
    h.valid = true;
    return true;
  }

  bool parseDht(size_t n) {
    while (n > 0) {
      int tcTh = readRaw();
      if (tcTh < 0 || n < 17) return fail("truncated DHT");
      --n;
      const int cls = tcTh >> 4;
      const int table = tcTh & 0x0F;
      if (cls > 1 || table > 3) return fail("unsupported Huffman table");
      uint8_t counts[16];
      int total = 0;
      for (int i = 0; i < 16; ++i) {
        int c = readRaw();
        if (c < 0) return fail("truncated DHT counts");
        counts[i] = static_cast<uint8_t>(c);
        total += c;
      }
      n -= 16;
      if (total < 0 || total > 256 || n < static_cast<size_t>(total)) return fail("invalid DHT size");
      uint8_t vals[256];
      for (int i = 0; i < total; ++i) {
        int v = readRaw();
        if (v < 0) return fail("truncated DHT values");
        vals[i] = static_cast<uint8_t>(v);
      }
      n -= total;
      if (!buildHuff(cls ? ac_[table] : dc_[table], counts, vals, total)) return fail("invalid Huffman codes");
    }
    return true;
  }

  bool parseDri(size_t n) {
    if (n != 2) {
      if (!skipRaw(n)) return fail("bad DRI");
      return true;
    }
    uint16_t ri;
    if (!readU16(ri)) return fail("truncated DRI");
    restartInterval_ = ri;
    return true;
  }

  bool parseSof2(size_t n) {
    if (n < 6) return fail("short SOF2");
    int p = readRaw();
    uint16_t h, w;
    int nc;
    if (p < 0 || !readU16(h) || !readU16(w) || (nc = readRaw()) < 0) return fail("truncated SOF2");
    n -= 6;
    if (p != 8 || nc < 1 || nc > MAX_COMPONENTS) return fail("unsupported progressive frame");
    if (n != static_cast<size_t>(nc * 3)) return fail("bad SOF2 component length");
    if (w == 0 || h == 0 || w > MAX_DIMENSION || h > MAX_DIMENSION ||
        static_cast<int64_t>(w) * h > MAX_PIXELS) return fail("progressive dimensions rejected");

    precision_ = p;
    width_ = w;
    height_ = h;
    compCount_ = nc;
    maxH_ = maxV_ = 1;
    for (int i = 0; i < nc; ++i) {
      int id = readRaw();
      int hv = readRaw();
      int q = readRaw();
      if (id < 0 || hv < 0 || q < 0) return fail("truncated SOF2 components");
      comps_[i].id = static_cast<uint8_t>(id);
      comps_[i].h = static_cast<uint8_t>(hv >> 4);
      comps_[i].v = static_cast<uint8_t>(hv & 15);
      comps_[i].q = static_cast<uint8_t>(q);
      if (comps_[i].h < 1 || comps_[i].h > 2 || comps_[i].v < 1 || comps_[i].v > 2 || q > 3)
        return fail("unsupported sampling factors");
      maxH_ = std::max(maxH_, static_cast<int>(comps_[i].h));
      maxV_ = std::max(maxV_, static_cast<int>(comps_[i].v));
    }

    mcuCols_ = (width_ + maxH_ * 8 - 1) / (maxH_ * 8);
    mcuRows_ = (height_ + maxV_ * 8 - 1) / (maxV_ * 8);
    for (int i = 0; i < nc; ++i) {
      auto& c = comps_[i];
      c.actualBlockCols = (width_ * c.h + maxH_ * 8 - 1) / (maxH_ * 8);
      c.actualBlockRows = (height_ * c.v + maxV_ * 8 - 1) / (maxV_ * 8);
      c.paddedBlockCols = mcuCols_ * c.h;
      c.paddedBlockRows = mcuRows_ * c.v;
    }
    // JPEG convention is Y first; if not, use the component with the largest sampling grid.
    yComp_ = 0;
    for (int i = 1; i < nc; ++i) {
      if (comps_[i].h * comps_[i].v > comps_[yComp_].h * comps_[yComp_].v) yComp_ = i;
    }
    scaleDenom_ = ProgressiveJpegDecoder::chooseScaleDenom(width_, height_, targetWidth_, targetHeight_);
    frameReady_ = true;
    LOG_DBG("PJPG", "SOF2 %dx%d comps=%d sampling=%dx%d output=1/%d", width_, height_, compCount_, maxH_, maxV_,
            scaleDenom_);
    return true;
  }

  int findComponentById(int id) const {
    for (int i = 0; i < compCount_; ++i) if (comps_[i].id == id) return i;
    return -1;
  }

  bool parseAndDecodeScan(size_t n) {
    if (n < 4) return fail("short SOS");
    int ns = readRaw();
    if (ns < 1 || ns > MAX_COMPONENTS) return fail("bad SOS component count");
    --n;
    if (n != static_cast<size_t>(ns * 2 + 3)) return fail("bad SOS length");
    Scan s;
    s.count = ns;
    for (int i = 0; i < ns; ++i) {
      int id = readRaw();
      int tables = readRaw();
      if (id < 0 || tables < 0) return fail("truncated SOS components");
      s.comps[i].comp = findComponentById(id);
      s.comps[i].dcTable = static_cast<uint8_t>(tables >> 4);
      s.comps[i].acTable = static_cast<uint8_t>(tables & 15);
      if (s.comps[i].comp < 0 || s.comps[i].dcTable > 3 || s.comps[i].acTable > 3)
        return fail("invalid SOS selector");
    }
    int ss = readRaw(), se = readRaw(), ahal = readRaw();
    if (ss < 0 || se < 0 || ahal < 0) return fail("truncated SOS spectral fields");
    s.ss = static_cast<uint8_t>(ss);
    s.se = static_cast<uint8_t>(se);
    s.ah = static_cast<uint8_t>(ahal >> 4);
    s.al = static_cast<uint8_t>(ahal & 15);
    if (s.ss > s.se || s.se > 63 || s.ah > 13 || s.al > 13) return fail("invalid progressive scan parameters");
    if (s.ss != 0 && s.count != 1) return fail("interleaved AC progressive scan unsupported by JPEG spec");
    if (s.ss == 0 && s.se != 0) return fail("invalid DC scan band");

    ++scanCount_;
    sawScan_ = true;
    resetEntropy();
    const unsigned long scanStarted = millis();
    LOG_INF("PJPG", "scan %d start: comps=%d Ss=%u Se=%u Ah=%u Al=%u", scanCount_, s.count,
            static_cast<unsigned>(s.ss), static_cast<unsigned>(s.se), static_cast<unsigned>(s.ah),
            static_cast<unsigned>(s.al));

    const bool touchesY = [&]() {
      for (int i = 0; i < s.count; ++i) if (s.comps[i].comp == yComp_) return true;
      return false;
    }();

    // A chroma-only scan cannot affect grayscale output. Skip its entropy bytes
    // safely (respecting FF00 stuffing and restart markers) and resume at the
    // next marker. This is a large speed win on e-paper.
    if (!touchesY) {
      if (!skipEntropyToMarker()) return false;
      LOG_INF("PJPG", "scan %d skipped chroma-only in %lu ms", scanCount_, millis() - scanStarted);
      return true;
    }

    if (s.ss == 0) {
      if (!decodeDcScan(s)) return false;
    } else {
      if (!decodeAcScan(s)) return false;
    }
    if (!finishEntropyScan()) return false;
    LOG_INF("PJPG", "scan %d done in %lu ms", scanCount_, millis() - scanStarted);
    return true;
  }

  bool ensureCoeffStore() {
    if (coeffReady_) return true;
    if (ESP.getFreeHeap() < ProgressiveJpegDecoder::REQUIRED_HEADROOM + 8U * 1024U)
      return fail("not enough heap for progressive decoder");

    Storage.ensureDirectoryExists("/.inkmod");
    coeffPath_ = "/.inkmod/.pjpg_coeff_" + std::to_string(static_cast<unsigned long>(millis())) + ".tmp";
    coeff_ = Storage.open(coeffPath_.c_str(), O_RDWR | O_CREAT | O_TRUNC);
    if (!coeff_) return fail("cannot create coefficient backing file");

    const Component& y = comps_[yComp_];
    const uint64_t blocks = static_cast<uint64_t>(y.paddedBlockCols) * y.paddedBlockRows;
    const uint64_t bytes = blocks * BLOCK_BYTES;
    // SdFat seekSet() as wrapped by HalFile cannot seek beyond EOF on the
    // X4/X3 storage stack, so a sparse seek+write extension is not portable
    // here. Initialise the backing store sequentially, but in large chunks
    // instead of the old 512-byte loop. This keeps the number of SD writes
    // small while preserving the required logical zero state for progressive
    // refinement scans.
    const uint32_t initStarted = millis();
    constexpr size_t INIT_CHUNK = 8192;
    uint8_t* zeros = static_cast<uint8_t*>(malloc(INIT_CHUNK));
    if (!zeros) return fail("OOM allocating coefficient init buffer");
    memset(zeros, 0, INIT_CHUNK);

    uint64_t remaining = bytes;
    uint64_t written = 0;
    while (remaining > 0) {
      const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, INIT_CHUNK));
      if (!writeCoeffExact(zeros, chunk, "cannot initialise coefficient backing file")) {
        free(zeros);
        return false;
      }
      remaining -= chunk;
      written += chunk;

      // Let the scheduler/watchdog breathe during large cover images without
      // flushing every chunk (flush would make SD initialisation much slower).
      if ((written & ((256U * 1024U) - 1U)) == 0) delay(0);
    }
    free(zeros);
    coeff_.flush();

    // Rewind explicitly: the first progressive scan usually starts with the
    // first Y block and subsequent helpers do random seek/read/write.
    if (bytes > 0 && !coeff_.seek64(0)) return fail("cannot rewind coefficient backing file");

    // Allocate enough DC cache for one complete MCU row of the Y component.
    // At the 4096px decoder limit this is only a few KiB, so large images no
    // longer fall back to per-block SD seeks when a full-image DC cache cannot fit.
    const size_t rowBlocks = static_cast<size_t>(y.paddedBlockCols) * static_cast<size_t>(y.v ? y.v : 1);
    const size_t rowBytes = rowBlocks * sizeof(int16_t);
    if (rowBlocks == 0 || rowBytes > 16U * 1024U ||
        ESP.getFreeHeap() <= ProgressiveJpegDecoder::REQUIRED_HEADROOM + rowBytes + 8U * 1024U) {
      return fail("not enough heap for DC row cache");
    }
    dcRowCache_ = static_cast<int16_t*>(malloc(rowBytes));
    if (!dcRowCache_) return fail("OOM allocating DC row cache");
    memset(dcRowCache_, 0, rowBytes);
    dcRowCacheBlocks_ = rowBlocks;

    coeffReady_ = true;
    LOG_INF("PJPG", "SD coefficient store ready: %llu blocks / %llu bytes init=%lu ms dcRowCache=%u",
            static_cast<unsigned long long>(blocks), static_cast<unsigned long long>(bytes), millis() - initStarted,
            static_cast<unsigned>(rowBytes));
    return true;
  }

  void cleanupTemp() {
    if (coeff_) coeff_.close();
    if (!coeffPath_.empty()) {
      Storage.remove(coeffPath_.c_str());
      coeffPath_.clear();
    }
    if (dcRowCache_) {
      free(dcRowCache_);
      dcRowCache_ = nullptr;
      dcRowCacheBlocks_ = 0;
    }
    coeffReady_ = false;
  }

  // Coefficient backing store lives on the SD card. Some cards/controllers can
  // legally return a short transfer even though no hard error is reported.
  // Treating a short read as an unwritten/sparse tail corrupts progressive
  // coefficients (white/black blocks). The file is fully zero-initialised in
  // ensureCoeffStore(), so every subsequent coefficient transfer must be exact.
  bool readCoeffExact(void* dst, size_t bytes, const char* what) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t done = 0;
    uint8_t noProgressRetries = 0;
    while (done < bytes) {
      const size_t want = bytes - done;
      const int got = coeff_.read(p + done, want);
      if (got < 0) return fail(what);
      if (got == 0) {
        if (++noProgressRetries > 2) return fail(what);
        delay(1);
        continue;
      }
      noProgressRetries = 0;
      done += static_cast<size_t>(got);
      if (done < bytes) delay(0);
    }
    return true;
  }

  bool writeCoeffExact(const void* src, size_t bytes, const char* what) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    size_t done = 0;
    uint8_t noProgressRetries = 0;
    while (done < bytes) {
      const size_t want = bytes - done;
      const size_t put = coeff_.write(p + done, want);
      if (put == 0) {
        if (++noProgressRetries > 2) return fail(what);
        delay(1);
        continue;
      }
      if (put > want) return fail(what);
      noProgressRetries = 0;
      done += put;
      if (done < bytes) delay(0);
    }
    return true;
  }

  bool readYBlock(int bx, int by, int16_t out[64]) {
    const Component& y = comps_[yComp_];
    if (bx < 0 || by < 0 || bx >= y.paddedBlockCols || by >= y.paddedBlockRows) return fail("Y block index OOB");
    const uint64_t index = static_cast<uint64_t>(by) * y.paddedBlockCols + bx;
    if (!coeff_.seek64(index * BLOCK_BYTES)) return fail("coefficient seek read");
    if (!readCoeffExact(out, BLOCK_BYTES, "coefficient read")) return false;
    return true;
  }

  bool writeYBlock(int bx, int by, const int16_t in[64]) {
    const Component& y = comps_[yComp_];
    const uint64_t index = static_cast<uint64_t>(by) * y.paddedBlockCols + bx;
    if (!coeff_.seek64(index * BLOCK_BYTES)) return fail("coefficient seek write");
    if (!writeCoeffExact(in, BLOCK_BYTES, "coefficient write")) return false;
    return true;
  }

  bool readYBlocks(int bx, int by, int count, int16_t* out) {
    const Component& y = comps_[yComp_];
    if (bx < 0 || by < 0 || count <= 0 || bx + count > y.paddedBlockCols || by >= y.paddedBlockRows)
      return fail("Y block range OOB");
    const uint64_t index = static_cast<uint64_t>(by) * y.paddedBlockCols + bx;
    const size_t bytes = static_cast<size_t>(count) * BLOCK_BYTES;
    if (!coeff_.seek64(index * BLOCK_BYTES)) return fail("coefficient range seek read");
    if (!readCoeffExact(out, bytes, "coefficient range read")) return false;
    return true;
  }

  bool writeYBlocks(int bx, int by, int count, const int16_t* in) {
    const Component& y = comps_[yComp_];
    if (bx < 0 || by < 0 || count <= 0 || bx + count > y.paddedBlockCols || by >= y.paddedBlockRows)
      return fail("Y block range OOB");
    const uint64_t index = static_cast<uint64_t>(by) * y.paddedBlockCols + bx;
    const size_t bytes = static_cast<size_t>(count) * BLOCK_BYTES;
    if (!coeff_.seek64(index * BLOCK_BYTES)) return fail("coefficient range seek write");
    if (!writeCoeffExact(in, bytes, "coefficient range write")) return false;
    return true;
  }

  bool readYRowSpan(int by, int rowCount, int16_t* out) {
    const Component& y = comps_[yComp_];
    if (by < 0 || rowCount <= 0 || by + rowCount > y.paddedBlockRows) return fail("Y row span OOB");
    const uint64_t index = static_cast<uint64_t>(by) * y.paddedBlockCols;
    const size_t blockCount = static_cast<size_t>(rowCount) * y.paddedBlockCols;
    const size_t bytes = blockCount * BLOCK_BYTES;
    if (!coeff_.seek64(index * BLOCK_BYTES)) return fail("coefficient row-span seek read");
    if (!readCoeffExact(out, bytes, "coefficient row-span read")) return false;
    return true;
  }

  bool writeYRowSpan(int by, int rowCount, const int16_t* in) {
    const Component& y = comps_[yComp_];
    if (by < 0 || rowCount <= 0 || by + rowCount > y.paddedBlockRows) return fail("Y row span OOB");
    const uint64_t index = static_cast<uint64_t>(by) * y.paddedBlockCols;
    const size_t blockCount = static_cast<size_t>(rowCount) * y.paddedBlockCols;
    const size_t bytes = blockCount * BLOCK_BYTES;
    if (!coeff_.seek64(index * BLOCK_BYTES)) return fail("coefficient row-span seek write");
    if (!writeCoeffExact(in, bytes, "coefficient row-span write")) return false;
    return true;
  }

  void resetEntropy() {
    entropyBits_ = 0;
    eobRun_ = 0;
    memset(dcPred_, 0, sizeof(dcPred_));
  }

  int entropyDataByte() {
    int b = readRaw();
    if (b < 0) return -1;
    if (b != 0xFF) return b;
    int c;
    do {
      c = readRaw();
      if (c < 0) return -1;
    } while (c == 0xFF);
    if (c == 0x00) return 0xFF;
    pendingMarker_ = c;
    return -2;
  }

  int getBit() {
    if (entropyBits_ == 0) {
      const int b = entropyDataByte();
      if (b < 0) return -1;
      entropyByte_ = static_cast<uint8_t>(b);
      entropyBits_ = 8;
    }
    const int bit = (entropyByte_ >> 7) & 1;
    entropyByte_ <<= 1;
    --entropyBits_;
    return bit;
  }

  int getBits(int n) {
    int v = 0;
    for (int i = 0; i < n; ++i) {
      const int b = getBit();
      if (b < 0) return -1;
      v = (v << 1) | b;
    }
    return v;
  }

  int huffDecode(const HuffTable& h) {
    if (!h.valid) return -1;
    int code = 0;
    for (int len = 1; len <= 16; ++len) {
      const int b = getBit();
      if (b < 0) return -1;
      code = (code << 1) | b;
      if (h.maxCode[len] >= 0 && code <= h.maxCode[len]) {
        const int idx = h.valPtr[len] + code - h.minCode[len];
        if (idx < 0 || idx >= h.valueCount) return -1;
        return h.values[idx];
      }
    }
    return -1;
  }

  int receiveExtend(int s) {
    if (s == 0) return 0;
    const int v = getBits(s);
    if (v < 0) return INT32_MIN;
    const int vt = 1 << (s - 1);
    return v < vt ? v - ((1 << s) - 1) : v;
  }

  bool consumeRestart(int expected) {
    entropyBits_ = 0;
    int marker = pendingMarker_ >= 0 ? takePendingMarker() : nextMarker();
    if (marker < M_RST0 || marker > M_RST7) return fail("missing restart marker");
    if (((marker - M_RST0) & 7) != (expected & 7)) {
      LOG_DBG("PJPG", "Restart sequence mismatch: got RST%d expected RST%d", marker - M_RST0, expected & 7);
    }
    memset(dcPred_, 0, sizeof(dcPred_));
    eobRun_ = 0;
    return true;
  }

  bool finishEntropyScan() {
    entropyBits_ = 0;
    if (pendingMarker_ >= 0) return true;
    int marker = nextMarker();
    if (marker < 0) return fail("scan missing terminating marker");
    pendingMarker_ = marker;
    return true;
  }

  bool skipEntropyToMarker() {
    entropyBits_ = 0;
    while (true) {
      int b = readRaw();
      if (b < 0) return fail("truncated skipped scan");
      if (b != 0xFF) continue;
      int c;
      do {
        c = readRaw();
        if (c < 0) return fail("truncated skipped scan marker");
      } while (c == 0xFF);
      if (c == 0x00) continue;
      if (c >= M_RST0 && c <= M_RST7) continue;
      pendingMarker_ = c;
      return true;
    }
  }

  bool decodeDcBlock(const ScanComponent& sc, int16_t block[64], bool store, bool refine) {
    const int ci = sc.comp;
    if (refine) {
      const int bit = getBit();
      if (bit < 0) return fail("truncated DC refinement");
      if (store && bit) block[0] = static_cast<int16_t>(block[0] + (1 << currentAl_));
      return true;
    }
    if (!dc_[sc.dcTable].valid) return fail("missing DC Huffman table");
    const int s = huffDecode(dc_[sc.dcTable]);
    if (s < 0 || s > 11) return fail("bad DC Huffman code");
    const int diff = receiveExtend(s);
    if (diff == INT32_MIN) return fail("truncated DC amplitude");
    dcPred_[ci] += diff;
    if (store) block[0] = static_cast<int16_t>(dcPred_[ci] << currentAl_);
    return true;
  }

  uint8_t currentAl_{0};

  size_t yBlockIndex(int bx, int by) const {
    const Component& y = comps_[yComp_];
    return static_cast<size_t>(by) * y.paddedBlockCols + bx;
  }

  bool loadDcRows(int firstBy, int rowCount) {
    const Component& y = comps_[yComp_];
    if (!dcRowCache_ || rowCount <= 0 || firstBy < 0 || firstBy + rowCount > y.paddedBlockRows)
      return fail("DC row cache range OOB");
    const size_t needed = static_cast<size_t>(y.paddedBlockCols) * rowCount;
    if (needed > dcRowCacheBlocks_) return fail("DC row cache too small");

    constexpr int BLOCKS_PER_CHUNK = 32;
    constexpr size_t CHUNK_BYTES = BLOCKS_PER_CHUNK * BLOCK_BYTES;
    int16_t* chunk = static_cast<int16_t*>(malloc(CHUNK_BYTES));
    if (!chunk) return fail("OOM DC row load chunk");
    struct Guard { int16_t* p; ~Guard() { free(p); } } guard{chunk};

    for (int ry = 0; ry < rowCount; ++ry) {
      const int by = firstBy + ry;
      for (int bx = 0; bx < y.paddedBlockCols; bx += BLOCKS_PER_CHUNK) {
        const int count = std::min(BLOCKS_PER_CHUNK, y.paddedBlockCols - bx);
        if (!readYBlocks(bx, by, count, chunk)) return false;
        for (int i = 0; i < count; ++i) {
          dcRowCache_[static_cast<size_t>(ry) * y.paddedBlockCols + bx + i] =
              chunk[i * BLOCK_COEFFS];
        }
      }
    }
    return true;
  }

  bool flushDcRows(int firstBy, int rowCount, bool preserveAc) {
    const Component& y = comps_[yComp_];
    if (!dcRowCache_ || rowCount <= 0 || firstBy < 0 || firstBy + rowCount > y.paddedBlockRows)
      return fail("DC row flush range OOB");
    const size_t needed = static_cast<size_t>(y.paddedBlockCols) * rowCount;
    if (needed > dcRowCacheBlocks_) return fail("DC row flush cache too small");

    constexpr int BLOCKS_PER_CHUNK = 32;
    constexpr size_t CHUNK_BYTES = BLOCKS_PER_CHUNK * BLOCK_BYTES;
    int16_t* chunk = static_cast<int16_t*>(malloc(CHUNK_BYTES));
    if (!chunk) return fail("OOM DC row flush chunk");
    struct Guard { int16_t* p; ~Guard() { free(p); } } guard{chunk};

    for (int ry = 0; ry < rowCount; ++ry) {
      const int by = firstBy + ry;
      for (int bx = 0; bx < y.paddedBlockCols; bx += BLOCKS_PER_CHUNK) {
        const int count = std::min(BLOCKS_PER_CHUNK, y.paddedBlockCols - bx);
        if (preserveAc) {
          if (!readYBlocks(bx, by, count, chunk)) return false;
        } else {
          memset(chunk, 0, static_cast<size_t>(count) * BLOCK_BYTES);
        }
        for (int i = 0; i < count; ++i) {
          chunk[i * BLOCK_COEFFS] =
              dcRowCache_[static_cast<size_t>(ry) * y.paddedBlockCols + bx + i];
        }
        if (!writeYBlocks(bx, by, count, chunk)) return false;
      }
    }
    return true;
  }

  bool decodeDcRefineInterleavedBatched(const Scan& s) {
    const Component& yComp = comps_[yComp_];
    const int rowsPerMcu = std::max<int>(1, yComp.v);
    const size_t blocksPerMcuRow = static_cast<size_t>(yComp.paddedBlockCols) * rowsPerMcu;
    const size_t bytes = blocksPerMcuRow * BLOCK_BYTES;

    // A full MCU-row cache removes the read-modify-write amplification from
    // DC refinement (normally scan 7). We need all coefficients because AC
    // scans have already populated them and must be preserved byte-for-byte.
    // Fall back to the small DC-only cache if contiguous RAM is tight.
    constexpr size_t MAX_DC_REFINE_ROW_BYTES = 48 * 1024;
    if (bytes == 0 || bytes > MAX_DC_REFINE_ROW_BYTES) return false;

    int16_t* row = static_cast<int16_t*>(malloc(bytes));
    if (!row) return false;
    struct RowGuard { int16_t* p; ~RowGuard() { free(p); } } guard{row};

    LOG_INF("PJPG", "DC refinement cache: %u blocks / %u bytes (MCU-row=%d rows)",
            static_cast<unsigned>(blocksPerMcuRow), static_cast<unsigned>(bytes), rowsPerMcu);

    uint32_t mcuCounter = 0;
    int rst = 0;
    for (int my = 0; my < mcuRows_; ++my) {
      const int firstYBy = my * yComp.v;
      const int yRows = std::min<int>(yComp.v, yComp.paddedBlockRows - firstYBy);
      if (yRows <= 0) break;

      // One contiguous SD read per Y block-row (instead of 32-block chunks
      // plus a second read during flush).
      for (int ry = 0; ry < yRows; ++ry) {
        int16_t* dst = row + static_cast<size_t>(ry) * yComp.paddedBlockCols * BLOCK_COEFFS;
        if (!readYBlocks(0, firstYBy + ry, yComp.paddedBlockCols, dst)) return false;
      }

      for (int mx = 0; mx < mcuCols_; ++mx) {
        if (restartInterval_ && mcuCounter && (mcuCounter % restartInterval_) == 0) {
          if (!consumeRestart(rst++)) return false;
        }
        for (int si = 0; si < s.count; ++si) {
          const auto& sc = s.comps[si];
          const Component& c = comps_[sc.comp];
          for (int vy = 0; vy < c.v; ++vy) {
            for (int hx = 0; hx < c.h; ++hx) {
              const int bx = mx * c.h + hx;
              const int by = my * c.v + vy;
              if (sc.comp == yComp_) {
                const int localY = by - firstYBy;
                if (localY < 0 || localY >= yRows || bx < 0 || bx >= yComp.paddedBlockCols)
                  return fail("DC refine MCU-row mapping OOB");
                int16_t* block = row +
                    (static_cast<size_t>(localY) * yComp.paddedBlockCols + bx) * BLOCK_COEFFS;
                if (!decodeDcBlock(sc, block, true, true)) return false;
              } else {
                // Chroma bits still belong to the entropy stream, but the
                // grayscale renderer does not retain chroma coefficients.
                int16_t dummy[64]{};
                if (!decodeDcBlock(sc, dummy, false, true)) return false;
              }
            }
          }
        }
        ++mcuCounter;
      }

      // One contiguous write per row; AC coefficients remain untouched.
      for (int ry = 0; ry < yRows; ++ry) {
        const int16_t* src = row + static_cast<size_t>(ry) * yComp.paddedBlockCols * BLOCK_COEFFS;
        if (!writeYBlocks(0, firstYBy + ry, yComp.paddedBlockCols, src)) return false;
      }
      delay(0);
    }
    return true;
  }

  bool decodeDcScan(const Scan& s) {
    currentAl_ = s.al;
    const bool refine = s.ah != 0;
    uint32_t mcuCounter = 0;
    int rst = 0;
    const Component& yComp = comps_[yComp_];
    if (!dcRowCache_ || dcRowCacheBlocks_ == 0) return fail("DC row cache unavailable");

    if (refine && s.count > 1) {
      // Scan 7 in common progressive JPEGs: try a full MCU-row RMW cache.
      // false here means only "not enough/too much RAM for the fast path";
      // the original small-cache implementation below remains the fallback.
      const size_t refineBytes = static_cast<size_t>(yComp.paddedBlockCols) *
                                 std::max<int>(1, yComp.v) * BLOCK_BYTES;
      if (refineBytes <= 48 * 1024) {
        const size_t beforePos = src_.position();
        if (decodeDcRefineInterleavedBatched(s)) return true;
        // If decoding consumed entropy and then failed, do not attempt to
        // rewind/fallback because the stream state is no longer recoverable.
        if (src_.position() != beforePos || failed_) return false;
        LOG_DBG("PJPG", "DC refinement row cache unavailable; using compact fallback");
      }
    }

    if (s.count > 1) {
      // Interleaved DC scans are MCU ordered. Cache exactly the Y block rows
      // touched by one MCU row, decode all components normally, then commit the
      // Y DC values in large contiguous SD transactions.
      for (int my = 0; my < mcuRows_; ++my) {
        const int firstYBy = my * yComp.v;
        const int yRows = std::min<int>(yComp.v, yComp.paddedBlockRows - firstYBy);
        if (refine) {
          if (!loadDcRows(firstYBy, yRows)) return false;
        } else {
          memset(dcRowCache_, 0, static_cast<size_t>(yComp.paddedBlockCols) * yRows * sizeof(int16_t));
        }

        for (int mx = 0; mx < mcuCols_; ++mx) {
          if (restartInterval_ && mcuCounter && (mcuCounter % restartInterval_) == 0) {
            if (!consumeRestart(rst++)) return false;
          }
          for (int si = 0; si < s.count; ++si) {
            const auto& sc = s.comps[si];
            const Component& c = comps_[sc.comp];
            for (int vy = 0; vy < c.v; ++vy) {
              for (int hx = 0; hx < c.h; ++hx) {
                const int bx = mx * c.h + hx;
                const int by = my * c.v + vy;
                const bool isY = sc.comp == yComp_;
                int16_t block[64]{};
                if (isY) {
                  const int localY = by - firstYBy;
                  if (localY < 0 || localY >= yRows || bx < 0 || bx >= yComp.paddedBlockCols)
                    return fail("DC MCU row mapping OOB");
                  const size_t idx = static_cast<size_t>(localY) * yComp.paddedBlockCols + bx;
                  block[0] = dcRowCache_[idx];
                  if (!decodeDcBlock(sc, block, true, refine)) return false;
                  dcRowCache_[idx] = block[0];
                } else {
                  // Chroma entropy still has to be consumed to keep the bitstream
                  // aligned, but grayscale rendering never stores its coefficients.
                  if (!decodeDcBlock(sc, block, false, refine)) return false;
                }
              }
            }
          }
          ++mcuCounter;
        }
        if (!flushDcRows(firstYBy, yRows, refine)) return false;
        delay(0);
      }
    } else {
      const auto& sc = s.comps[0];
      const Component& c = comps_[sc.comp];
      const bool isYScan = sc.comp == yComp_;

      if (!isYScan) {
        // Should normally be handled by touchesY/skipEntropyToMarker, but keep a
        // safe entropy-consuming path for unusual files.
        int16_t dummy[64]{};
        for (int by = 0; by < c.actualBlockRows; ++by) {
          for (int bx = 0; bx < c.actualBlockCols; ++bx) {
            if (restartInterval_ && mcuCounter && (mcuCounter % restartInterval_) == 0) {
              if (!consumeRestart(rst++)) return false;
            }
            if (!decodeDcBlock(sc, dummy, false, refine)) return false;
            ++mcuCounter;
          }
        }
        return true;
      }

      // Non-interleaved Y DC scans are raster ordered. One block row is enough
      // for the cache and gives perfectly sequential backing-store I/O.
      for (int by = 0; by < c.actualBlockRows; ++by) {
        if (refine) {
          if (!loadDcRows(by, 1)) return false;
        } else {
          memset(dcRowCache_, 0, static_cast<size_t>(yComp.paddedBlockCols) * sizeof(int16_t));
        }
        for (int bx = 0; bx < c.actualBlockCols; ++bx) {
          if (restartInterval_ && mcuCounter && (mcuCounter % restartInterval_) == 0) {
            if (!consumeRestart(rst++)) return false;
          }
          int16_t block[64]{};
          block[0] = dcRowCache_[bx];
          if (!decodeDcBlock(sc, block, true, refine)) return false;
          dcRowCache_[bx] = block[0];
          ++mcuCounter;
        }
        if (!flushDcRows(by, 1, refine)) return false;
        if ((by & 7) == 7) delay(0);
      }
    }
    return true;
  }

  bool refineExisting(int16_t& coef, int p1) {
    const int bit = getBit();
    if (bit < 0) return fail("truncated AC refinement bit");
    if (bit && (coef & p1) == 0) coef = static_cast<int16_t>(coef + (coef > 0 ? p1 : -p1));
    return true;
  }

  bool decodeAcInitial(const Scan& s, int16_t block[64]) {
    if (eobRun_ > 0) {
      --eobRun_;
      return true;
    }
    const auto& sc = s.comps[0];
    if (!ac_[sc.acTable].valid) return fail("missing AC Huffman table");
    int k = s.ss;
    do {
      const int rs = huffDecode(ac_[sc.acTable]);
      if (rs < 0) return fail("bad AC Huffman code");
      int r = rs >> 4;
      const int sz = rs & 15;
      if (sz == 0) {
        if (r < 15) {
          uint32_t run = 1u << r;
          if (r) {
            const int bits = getBits(r);
            if (bits < 0) return fail("truncated EOB run");
            run += static_cast<uint32_t>(bits);
          }
          eobRun_ = run - 1;
          break;
        }
        k += 16;
      } else {
        k += r;
        if (k > s.se) return fail("AC run exceeds band");
        const int v = receiveExtend(sz);
        if (v == INT32_MIN) return fail("truncated AC amplitude");
        block[kZigZag[k++]] = static_cast<int16_t>(v << s.al);
      }
    } while (k <= s.se);
    return true;
  }

  bool decodeAcRefine(const Scan& s, int16_t block[64]) {
    const auto& sc = s.comps[0];
    if (!ac_[sc.acTable].valid) return fail("missing AC refinement Huffman table");
    const int p1 = 1 << s.al;
    int k = s.ss;

    // If an EOB run was started in an earlier block, no Huffman symbol is read
    // for this block; only already-nonzero coefficients receive refinement bits.
    if (eobRun_ > 0) {
      --eobRun_;
      for (; k <= s.se; ++k) {
        int16_t& c = block[kZigZag[k]];
        if (c != 0 && !refineExisting(c, p1)) return false;
      }
      return true;
    }

    do {
      const int rs = huffDecode(ac_[sc.acTable]);
      if (rs < 0) return fail("bad AC refinement Huffman code");
      int r = rs >> 4;
      int newCoef = 0;
      const int sz = rs & 15;

      if (sz == 0) {
        if (r < 15) {
          // The current block is handled below; eobRun_ counts only following
          // blocks, matching Annex G and established progressive decoders.
          eobRun_ = (1u << r) - 1u;
          if (r) {
            const int bits = getBits(r);
            if (bits < 0) return fail("truncated refinement EOB run");
            eobRun_ += static_cast<uint32_t>(bits);
          }
          r = 64;  // consume refinement bits to the end of this block
        }
        // r==15,sz==0 is ZRL: advance across 16 zero coefficients. Using
        // newCoef=0 below deliberately consumes the 16th zero as well.
      } else {
        if (sz != 1) return fail("invalid AC refinement size");
        const int sign = getBit();
        if (sign < 0) return fail("truncated AC refinement sign");
        newCoef = sign ? p1 : -p1;
      }

      while (k <= s.se) {
        int16_t& c = block[kZigZag[k++]];
        if (c != 0) {
          if (!refineExisting(c, p1)) return false;
        } else {
          if (r == 0) {
            c = static_cast<int16_t>(newCoef);
            break;
          }
          --r;
        }
      }
    } while (k <= s.se);
    return true;
  }

  bool decodeAcScan(const Scan& s) {
    if (s.count != 1 || s.comps[0].comp != yComp_) return fail("unexpected non-Y AC scan route");
    const Component& c = comps_[yComp_];
    uint32_t mcuCounter = 0;
    int rst = 0;
    eobRun_ = 0;

    // AC scans are now buffered by 1-3 full DCT rows instead of a single row
    // or tiny 4-KB chunks. On X4/X3 that cuts the number of SD seek/read/write
    // cycles sharply for medium illustrations while still keeping RAM bounded.
    // If the requested multi-row cache does not fit, we automatically fall back
    // to fewer rows and finally to the old 32-block safety path.
    constexpr size_t SAFE_AC_CACHE_BYTES = 24U * 1024U;
    constexpr size_t FAST_AC_CACHE_BYTES = 40U * 1024U;
    constexpr size_t AC_ALLOC_RESERVE = 12U * 1024U;

    // Keep the conservative 24-KB limit by default. When the heap is healthy
    // and, importantly, the largest contiguous allocation is big enough, let
    // medium/large images use up to 40 KB so two DCT rows can be processed per
    // SD transaction. Never consume the last large heap block: keep both the
    // decoder headroom and an extra allocation reserve for page rendering.
    const size_t freeHeap = ESP.getFreeHeap();
    const size_t maxAlloc = ESP.getMaxAllocHeap();
    size_t maxAcCacheBytes = SAFE_AC_CACHE_BYTES;
    if (freeHeap > ProgressiveJpegDecoder::REQUIRED_HEADROOM + FAST_AC_CACHE_BYTES + 8U * 1024U &&
        maxAlloc > FAST_AC_CACHE_BYTES + AC_ALLOC_RESERVE) {
      maxAcCacheBytes = FAST_AC_CACHE_BYTES;
    } else if (maxAlloc > SAFE_AC_CACHE_BYTES + AC_ALLOC_RESERVE) {
      const size_t contiguousBudget = maxAlloc - AC_ALLOC_RESERVE;
      maxAcCacheBytes = std::min(FAST_AC_CACHE_BYTES,
                                 std::max(SAFE_AC_CACHE_BYTES, contiguousBudget));
    }

    int rowsPerChunk = 1;
    const int paddedCols = std::max(1, c.paddedBlockCols);
    const size_t rowBytes = static_cast<size_t>(paddedCols) * BLOCK_BYTES;
    const int maxRowsByRam = static_cast<int>(maxAcCacheBytes / rowBytes);

    // Empirically on the X4/X3 SD path, "as many rows as fit" is not always
    // fastest. Very small rows benefit from 3-row batching, medium rows from
    // 2-row batching, while wide rows are best kept to one row. This keeps
    // each transfer large enough to amortize seek overhead without making the
    // individual SD transactions unnecessarily heavy.
    int preferredRows = 1;
    if (rowBytes <= 8U * 1024U) preferredRows = 3;
    else if (rowBytes <= 18U * 1024U) preferredRows = 2;

    rowsPerChunk = std::min(preferredRows, std::max(1, maxRowsByRam));
    rowsPerChunk = std::min(rowsPerChunk, std::max(1, c.actualBlockRows));
    size_t chunkBlocks = static_cast<size_t>(rowsPerChunk) * paddedCols;
    size_t chunkBytes = chunkBlocks * BLOCK_BYTES;
    int16_t* chunk = static_cast<int16_t*>(malloc(chunkBytes));
    while (!chunk && rowsPerChunk > 1) {
      --rowsPerChunk;
      chunkBlocks = static_cast<size_t>(rowsPerChunk) * paddedCols;
      chunkBytes = chunkBlocks * BLOCK_BYTES;
      chunk = static_cast<int16_t*>(malloc(chunkBytes));
    }
    bool fallbackSmallChunks = false;
    int blocksPerChunk = 0;
    if (!chunk) {
      fallbackSmallChunks = true;
      blocksPerChunk = std::min(32, c.actualBlockCols);
      chunkBytes = static_cast<size_t>(blocksPerChunk) * BLOCK_BYTES;
      chunk = static_cast<int16_t*>(malloc(chunkBytes));
      if (!chunk) return fail("OOM AC coefficient cache");
      LOG_INF("PJPG", "AC cache fallback: %d blocks / %u bytes", blocksPerChunk,
              static_cast<unsigned>(chunkBytes));
    }
    struct ChunkGuard { int16_t* p; ~ChunkGuard() { free(p); } } guard{chunk};

    if (fallbackSmallChunks) {
      LOG_INF("PJPG", "AC scan cache: %d blocks / %u bytes (row=%d blocks)", blocksPerChunk,
              static_cast<unsigned>(chunkBytes), c.actualBlockCols);
      for (int by = 0; by < c.actualBlockRows; ++by) {
        for (int chunkX = 0; chunkX < c.actualBlockCols; chunkX += blocksPerChunk) {
          const int count = std::min(blocksPerChunk, c.actualBlockCols - chunkX);
          if (!readYBlocks(chunkX, by, count, chunk)) return false;
          for (int i = 0; i < count; ++i) {
            if (restartInterval_ && mcuCounter && (mcuCounter % restartInterval_) == 0) {
              if (!consumeRestart(rst++)) return false;
            }
            int16_t* block = chunk + i * BLOCK_COEFFS;
            const bool ok = s.ah ? decodeAcRefine(s, block) : decodeAcInitial(s, block);
            if (!ok) return false;
            ++mcuCounter;
          }
          if (!writeYBlocks(chunkX, by, count, chunk)) return false;
        }
        if ((by & 7) == 7) delay(0);
      }
      return true;
    }

    LOG_INF("PJPG",
            "AC scan cache: %d rows / %u blocks / %u bytes (row=%d blocks rowBytes=%u preferred=%d limit=%u free=%u maxAlloc=%u)",
            rowsPerChunk, static_cast<unsigned>(chunkBlocks), static_cast<unsigned>(chunkBytes),
            c.actualBlockCols, static_cast<unsigned>(rowBytes), preferredRows,
            static_cast<unsigned>(maxAcCacheBytes), static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(maxAlloc));

    for (int by = 0; by < c.actualBlockRows; by += rowsPerChunk) {
      const int rowsThisChunk = std::min(rowsPerChunk, c.actualBlockRows - by);
      if (!readYRowSpan(by, rowsThisChunk, chunk)) return false;
      for (int ry = 0; ry < rowsThisChunk; ++ry) {
        int16_t* rowBase = chunk + static_cast<size_t>(ry) * paddedCols * BLOCK_COEFFS;
        for (int bx = 0; bx < c.actualBlockCols; ++bx) {
          if (restartInterval_ && mcuCounter && (mcuCounter % restartInterval_) == 0) {
            if (!consumeRestart(rst++)) return false;
          }
          int16_t* block = rowBase + static_cast<size_t>(bx) * BLOCK_COEFFS;
          const bool ok = s.ah ? decodeAcRefine(s, block) : decodeAcInitial(s, block);
          if (!ok) return false;
          ++mcuCounter;
        }
      }
      if (!writeYRowSpan(by, rowsThisChunk, chunk)) return false;
      if (((by / std::max(1, rowsPerChunk)) & 3) == 3) delay(0);
    }
    return true;
  }

  void idct8(const int16_t coeff[64], const uint16_t q[64], uint8_t out[64]) {
    int64_t tmp[64];
    for (int v = 0; v < 8; ++v) {
      for (int x = 0; x < 8; ++x) {
        int64_t sum = 0;
        for (int u = 0; u < 8; ++u) {
          const int idx = v * 8 + u;
          sum += static_cast<int64_t>(coeff[idx]) * q[idx] * kIdctBasis[x][u];
        }
        tmp[v * 8 + x] = sum;
      }
    }
    constexpr int64_t denom = 4LL * 16384LL * 16384LL;
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        int64_t sum = 0;
        for (int v = 0; v < 8; ++v) sum += tmp[v * 8 + x] * kIdctBasis[y][v];
        int val = static_cast<int>((sum >= 0 ? sum + denom / 2 : sum - denom / 2) / denom) + 128;
        val = std::max(0, std::min(255, val));
        out[y * 8 + x] = static_cast<uint8_t>(val);
      }
    }
  }

  bool renderLuma() {
    const Component& y = comps_[yComp_];
    if (y.q > 3 || !quantValid_[y.q]) return fail("missing luminance quant table");
    const int outW = (width_ + scaleDenom_ - 1) / scaleDenom_;
    const int outH = (height_ + scaleDenom_ - 1) / scaleDenom_;
    const int stripH = std::max(1, 8 / scaleDenom_);
    const size_t stripBytes = static_cast<size_t>(outW) * stripH;
    uint8_t* strip = static_cast<uint8_t*>(malloc(stripBytes));
    if (!strip) return fail("OOM output strip");
    struct FreeGuard { uint8_t* p; ~FreeGuard() { free(p); } } freeGuard{strip};

    constexpr int RENDER_BLOCKS_PER_CHUNK = 32;
    constexpr size_t RENDER_CHUNK_BYTES = RENDER_BLOCKS_PER_CHUNK * BLOCK_BYTES;
    int16_t* coeffChunk = static_cast<int16_t*>(malloc(RENDER_CHUNK_BYTES));
    if (!coeffChunk) return fail("OOM render coefficient chunk");
    struct CoeffGuard { int16_t* p; ~CoeffGuard() { free(p); } } coeffGuard{coeffChunk};

    const unsigned long started = millis();
    for (int by = 0; by < y.actualBlockRows; ++by) {
      memset(strip, 0xFF, stripBytes);
      for (int chunkX = 0; chunkX < y.actualBlockCols; chunkX += RENDER_BLOCKS_PER_CHUNK) {
        const int blockCount = std::min(RENDER_BLOCKS_PER_CHUNK, y.actualBlockCols - chunkX);
        if (!readYBlocks(chunkX, by, blockCount, coeffChunk)) return false;
        for (int bi = 0; bi < blockCount; ++bi) {
          const int bx = chunkX + bi;
          const int16_t* block = coeffChunk + bi * BLOCK_COEFFS;
          uint8_t pix[64];
          idct8(block, quant_[y.q], pix);

        const int srcX0 = bx * 8;
        const int srcY0 = by * 8;
        for (int oy = 0; oy < stripH; ++oy) {
          const int srcYBase = oy * scaleDenom_;
          if (srcY0 + srcYBase >= height_) continue;
          for (int ox = 0; ox < 8 / scaleDenom_; ++ox) {
            const int srcXBase = ox * scaleDenom_;
            if (srcX0 + srcXBase >= width_) continue;
            int sum = 0;
            int count = 0;
            for (int yy = 0; yy < scaleDenom_; ++yy) {
              for (int xx = 0; xx < scaleDenom_; ++xx) {
                const int sx = srcXBase + xx;
                const int sy = srcYBase + yy;
                if (sx < 8 && sy < 8 && srcX0 + sx < width_ && srcY0 + sy < height_) {
                  sum += pix[sy * 8 + sx];
                  ++count;
                }
              }
            }
            const int dx = (srcX0 / scaleDenom_) + ox;
            if (dx < outW && count) strip[oy * outW + dx] = static_cast<uint8_t>(sum / count);
          }
        }
        }
      }

      const int drawY = (by * 8) / scaleDenom_;
      int validH = std::min(stripH, outH - drawY);
      if (validH <= 0) break;
      JPEGDRAW d{};
      d.x = 0;
      d.y = drawY;
      d.iWidth = outW;
      d.iWidthUsed = outW;
      d.iHeight = validH;
      d.iBpp = 8;
      d.pPixels = reinterpret_cast<uint16_t*>(strip);
      d.pUser = user_;
      if (!cb_(&d)) return fail("draw callback cancelled");
    }
    LOG_DBG("PJPG", "Full progressive grayscale rendered %dx%d -> %dx%d in %lu ms, scans=%d heap=%u", width_, height_,
            outW, outH, millis() - started, scanCount_, ESP.getFreeHeap());
    return true;
  }
};

}  // namespace

int ProgressiveJpegDecoder::chooseScaleDenom(int srcWidth, int srcHeight, int targetWidth, int targetHeight) {
  if (srcWidth <= 0 || srcHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) return 1;
  int denom = 1;
  while (denom < 8) {
    const int next = denom * 2;
    const int w = (srcWidth + next - 1) / next;
    const int h = (srcHeight + next - 1) / next;
    if (w < targetWidth || h < targetHeight) break;
    denom = next;
  }
  return denom;
}

bool ProgressiveJpegDecoder::decode(FsFile& file, JPEG_DRAW_CALLBACK* drawCallback, void* user,
                                    ProgressiveJpegInfo& info, int targetWidth, int targetHeight) {
  info = {};
  if (!file || !drawCallback) return false;
  const uint32_t heapBefore = ESP.getFreeHeap();
  if (heapBefore < REQUIRED_HEADROOM + 8U * 1024U) {
    LOG_ERR("PJPG", "Progressive JPEG skipped: low heap (%u)", heapBefore);
    return false;
  }
  // Decoder contains Huffman tables and parser state (~several KB). Keep it
  // off ActivityManager's stack; the previous stack allocation could trip the
  // ESP32-C3 stack canary on large progressive images.
  Decoder* d = new (std::nothrow) Decoder(file, drawCallback, user, info, targetWidth, targetHeight);
  if (!d) {
    LOG_ERR("PJPG", "Progressive JPEG skipped: OOM allocating decoder state");
    return false;
  }
  const bool ok = d->run();
  delete d;
  LOG_DBG("PJPG", "Progressive path finished: ok=%d heap %u -> %u", ok ? 1 : 0, heapBefore, ESP.getFreeHeap());
  return ok;
}

bool ProgressiveJpegDecoder::decode(const std::string& path, JPEG_DRAW_CALLBACK* drawCallback, void* user,
                                    ProgressiveJpegInfo& info, int targetWidth, int targetHeight) {
  FsFile file;
  if (!Storage.openFileForRead("PJPG", path, file)) {
    LOG_ERR("PJPG", "Failed to open progressive JPEG: %s", path.c_str());
    return false;
  }
  return decode(file, drawCallback, user, info, targetWidth, targetHeight);
}
