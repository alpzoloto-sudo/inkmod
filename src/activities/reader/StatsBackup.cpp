#include "StatsBackup.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GlobalReadingStats.h"
#include "ReadingStatsUtils.h"

namespace {
constexpr char LOG_TAG[] = "SBACK";
constexpr char INKMOD_DIR[] = "/.inkmod";
constexpr char GLOBAL_STATS_PATH[] = "/.inkmod/global_stats.bin";
constexpr char BACKUP_DIR[] = "/.inkmod-stats-backup";
constexpr int DEFAULT_BACKUP_KEEP_COUNT = 3;
constexpr uint8_t BUNDLE_VERSION = 1;
constexpr uint8_t BUNDLE_MAGIC[] = {'I', 'M', 'S', 'T', 'A', 'T', '2', 0};
constexpr size_t IO_BUFFER_SIZE = 512;
constexpr uint8_t MAX_SCAN_DEPTH = 4;
constexpr size_t MAX_RELATIVE_PATH = 220;

struct BackupName {
  char value[64] = {};
};

bool isStatsBackupFileName(const char* name) {
  if (!name || strncmp(name, "stats_", 6) != 0) return false;
  const size_t len = strlen(name);
  return len > 10 && strcmp(name + len - 4, ".bin") == 0;
}

bool copyString(const char* src, char* dst, const size_t dstLen) {
  if (!dst || dstLen == 0) return false;
  const int written = snprintf(dst, dstLen, "%s", src ? src : "");
  return written > 0 && static_cast<size_t>(written) < dstLen;
}

bool buildDatedBackupName(const ReadingStatsDateTime& dt, const bool manual, char* out, const size_t outLen) {
  int written = 0;
  if (manual) {
    written = snprintf(out, outLen, "stats_%04u-%02u-%02u_%02u%02u.bin", static_cast<unsigned>(dt.date.year),
                       static_cast<unsigned>(dt.date.month), static_cast<unsigned>(dt.date.day),
                       static_cast<unsigned>(dt.hour), static_cast<unsigned>(dt.minute));
  } else {
    written = snprintf(out, outLen, "stats_%04u-%02u-%02u.bin", static_cast<unsigned>(dt.date.year),
                       static_cast<unsigned>(dt.date.month), static_cast<unsigned>(dt.date.day));
  }
  return written > 0 && static_cast<size_t>(written) < outLen;
}

bool parseIncrementingIndex(const char* name, uint32_t& outIndex) {
  constexpr char prefix[] = "stats_backup_";
  constexpr size_t prefixLen = sizeof(prefix) - 1;
  if (!name || strncmp(name, prefix, prefixLen) != 0) return false;
  const char* digits = name + prefixLen;
  const char* suffix = strstr(digits, ".bin");
  if (!suffix || suffix == digits || suffix[4] != '\0') return false;

  uint32_t value = 0;
  for (const char* p = digits; p < suffix; ++p) {
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    value = value * 10u + static_cast<uint32_t>(*p - '0');
  }
  if (value == 0) return false;
  outIndex = value;
  return true;
}

bool nextIncrementingBackupName(char* out, const size_t outLen) {
  FsFile dir = Storage.open(BACKUP_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    const int written = snprintf(out, outLen, "stats_backup_%03u.bin", 1u);
    return written > 0 && static_cast<size_t>(written) < outLen;
  }

  char name[128];
  uint32_t maxIndex = 0;
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (isDirectory || nameLen == 0) continue;

    uint32_t index = 0;
    if (parseIncrementingIndex(name, index) && index > maxIndex) maxIndex = index;
  }
  dir.close();

  const int written = snprintf(out, outLen, "stats_backup_%03u.bin", static_cast<unsigned>(maxIndex + 1));
  return written > 0 && static_cast<size_t>(written) < outLen;
}

bool chooseBackupName(const bool manual, char* out, const size_t outLen) {
  ReadingStatsDateTime now;
  if (getCurrentLocalReadingStatsDateTime(now)) return buildDatedBackupName(now, manual, out, outLen);
  return nextIncrementingBackupName(out, outLen);
}

bool writeExact(FsFile& file, const void* data, size_t size) {
  const uint8_t* src = static_cast<const uint8_t*>(data);
  size_t done = 0;
  while (done < size) {
    const size_t n = file.write(src + done, size - done);
    if (n == 0) return false;
    done += n;
  }
  return true;
}

bool readExact(FsFile& file, void* data, size_t size) {
  uint8_t* dst = static_cast<uint8_t*>(data);
  size_t done = 0;
  while (done < size) {
    const int n = file.read(dst + done, size - done);
    if (n <= 0) return false;
    done += static_cast<size_t>(n);
  }
  return true;
}

bool writeU16(FsFile& file, const uint16_t value) {
  const uint8_t data[2] = {static_cast<uint8_t>(value & 0xffu), static_cast<uint8_t>((value >> 8) & 0xffu)};
  return writeExact(file, data, sizeof(data));
}

bool writeU32(FsFile& file, const uint32_t value) {
  const uint8_t data[4] = {static_cast<uint8_t>(value & 0xffu), static_cast<uint8_t>((value >> 8) & 0xffu),
                           static_cast<uint8_t>((value >> 16) & 0xffu), static_cast<uint8_t>((value >> 24) & 0xffu)};
  return writeExact(file, data, sizeof(data));
}

bool readU16(FsFile& file, uint16_t& value) {
  uint8_t data[2];
  if (!readExact(file, data, sizeof(data))) return false;
  value = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  return true;
}

bool readU32(FsFile& file, uint32_t& value) {
  uint8_t data[4];
  if (!readExact(file, data, sizeof(data))) return false;
  value = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
          (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return true;
}

bool isUserReadingStateFile(const char* name) {
  return name && (strcmp(name, "stats.bin") == 0 || strcmp(name, "progress.bin") == 0);
}

bool appendFileEntry(FsFile& bundle, const char* absolutePath, const char* relativePath, uint32_t& entryCount) {
  if (!absolutePath || !relativePath) return false;
  const size_t pathLen = strlen(relativePath);
  if (pathLen == 0 || pathLen > MAX_RELATIVE_PATH || pathLen > UINT16_MAX) return false;

  FsFile src;
  if (!Storage.openFileForRead(LOG_TAG, absolutePath, src)) return false;
  const uint64_t size64 = src.fileSize64();
  if (size64 > UINT32_MAX) {
    src.close();
    return false;
  }
  const uint32_t fileSize = static_cast<uint32_t>(size64);

  if (!writeU16(bundle, static_cast<uint16_t>(pathLen)) || !writeU32(bundle, fileSize) ||
      !writeExact(bundle, relativePath, pathLen)) {
    src.close();
    return false;
  }

  uint8_t buffer[IO_BUFFER_SIZE];
  uint32_t remaining = fileSize;
  while (remaining > 0) {
    const size_t want = std::min<size_t>(sizeof(buffer), remaining);
    const int got = src.read(buffer, want);
    if (got <= 0 || !writeExact(bundle, buffer, static_cast<size_t>(got))) {
      src.close();
      return false;
    }
    remaining -= static_cast<uint32_t>(got);
  }
  src.close();
  ++entryCount;
  return true;
}

bool scanAndAppendState(FsFile& bundle, const std::string& dirPath, const std::string& relativeDir, const uint8_t depth,
                        uint32_t& entryCount) {
  if (depth > MAX_SCAN_DEPTH) return true;
  FsFile dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return true;
  }

  char name[128];
  bool ok = true;
  for (FsFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    const size_t nameLen = entry.getName(name, sizeof(name));
    entry.close();
    if (nameLen == 0 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

    const std::string abs = dirPath + "/" + name;
    const std::string rel = relativeDir.empty() ? std::string(name) : relativeDir + "/" + name;
    if (isDirectory) {
      if (!scanAndAppendState(bundle, abs, rel, static_cast<uint8_t>(depth + 1), entryCount)) {
        ok = false;
        break;
      }
      continue;
    }

    if (isUserReadingStateFile(name)) {
      if (!appendFileEntry(bundle, abs.c_str(), rel.c_str(), entryCount)) {
        LOG_ERR(LOG_TAG, "Failed to add reading-state file: %s", abs.c_str());
        ok = false;
        break;
      }
    }
  }
  dir.close();
  return ok;
}

bool writeReadingStateBundle(const char* path) {
  const std::string tmpPath = std::string(path) + ".tmp";
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());

  FsFile bundle;
  if (!Storage.openFileForWrite(LOG_TAG, tmpPath.c_str(), bundle)) return false;

  uint32_t entryCount = 0;
  bool ok = writeExact(bundle, BUNDLE_MAGIC, sizeof(BUNDLE_MAGIC)) && writeExact(bundle, &BUNDLE_VERSION, 1);
  if (ok && Storage.exists(GLOBAL_STATS_PATH)) {
    ok = appendFileEntry(bundle, GLOBAL_STATS_PATH, "global_stats.bin", entryCount);
  }
  if (ok) ok = scanAndAppendState(bundle, INKMOD_DIR, "", 0, entryCount);
  if (ok) ok = writeU16(bundle, 0);  // end marker

  if (ok) {
    bundle.flush();
    ok = bundle.sync();
  }
  const bool closed = bundle.close();
  ok = ok && closed && entryCount > 0;

  if (!ok) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (Storage.exists(path) && !Storage.remove(path)) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), path)) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  LOG_DBG(LOG_TAG, "Wrote full reading-state backup with %u file(s): %s", static_cast<unsigned>(entryCount), path);
  return true;
}

bool isSupportedStatsFile(const char* path, size_t* outSize = nullptr) {
  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  const size_t fileSize = file.fileSize();
  uint8_t version = 0;
  const int n = file.read(&version, 1);
  file.close();
  if (n != 1) return false;
  const bool supported = (version == 1 && fileSize == 13) || (version == 2 && fileSize == 17) ||
                         (version == GlobalReadingStats::CURRENT_FILE_VERSION &&
                          fileSize == GlobalReadingStats::CURRENT_FILE_SIZE);
  if (supported && outSize) *outSize = fileSize;
  return supported;
}

bool copyFileExact(const char* srcPath, const char* dstPath, const size_t expectedSize) {
  FsFile src;
  if (!Storage.openFileForRead(LOG_TAG, srcPath, src)) return false;
  FsFile dst;
  if (!Storage.openFileForWrite(LOG_TAG, dstPath, dst)) {
    src.close();
    return false;
  }
  uint8_t buffer[IO_BUFFER_SIZE];
  size_t total = 0;
  bool ok = true;
  while (total < expectedSize) {
    const size_t want = std::min(sizeof(buffer), expectedSize - total);
    const int got = src.read(buffer, want);
    if (got <= 0 || !writeExact(dst, buffer, static_cast<size_t>(got))) {
      ok = false;
      break;
    }
    total += static_cast<size_t>(got);
  }
  if (ok) {
    dst.flush();
    ok = dst.sync();
  }
  const bool dstClosed = dst.close();
  src.close();
  ok = ok && dstClosed && total == expectedSize;
  if (!ok) Storage.remove(dstPath);
  return ok;
}

bool isBundleBackup(const char* path) {
  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  uint8_t magic[sizeof(BUNDLE_MAGIC)] = {};
  uint8_t version = 0;
  const bool ok = readExact(file, magic, sizeof(magic)) && readExact(file, &version, 1) &&
                  memcmp(magic, BUNDLE_MAGIC, sizeof(BUNDLE_MAGIC)) == 0 && version == BUNDLE_VERSION;
  file.close();
  return ok;
}

bool safeRelativePath(const std::string& rel) {
  if (rel.empty() || rel.size() > MAX_RELATIVE_PATH || rel[0] == '/' || rel.find('\\') != std::string::npos ||
      rel.find("..") != std::string::npos) return false;
  return true;
}

bool ensureParentDirectory(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return true;
  return Storage.mkdir(path.substr(0, slash).c_str(), true) || Storage.exists(path.substr(0, slash).c_str());
}

bool restoreOneEntry(FsFile& bundle, const std::string& relativePath, const uint32_t fileSize) {
  if (!safeRelativePath(relativePath)) return false;
  const std::string target = std::string(INKMOD_DIR) + "/" + relativePath;
  const std::string tmp = target + ".restore.tmp";
  const std::string bak = target + ".restore.bak";
  if (!ensureParentDirectory(target)) return false;
  if (Storage.exists(tmp.c_str())) Storage.remove(tmp.c_str());

  FsFile out;
  if (!Storage.openFileForWrite(LOG_TAG, tmp.c_str(), out)) return false;
  uint8_t buffer[IO_BUFFER_SIZE];
  uint32_t remaining = fileSize;
  bool ok = true;
  while (remaining > 0) {
    const size_t want = std::min<size_t>(sizeof(buffer), remaining);
    if (!readExact(bundle, buffer, want) || !writeExact(out, buffer, want)) {
      ok = false;
      break;
    }
    remaining -= static_cast<uint32_t>(want);
  }
  if (ok) {
    out.flush();
    ok = out.sync();
  }
  const bool closed = out.close();
  ok = ok && closed && remaining == 0;
  if (!ok) {
    Storage.remove(tmp.c_str());
    return false;
  }

  if (Storage.exists(bak.c_str())) Storage.remove(bak.c_str());
  const bool hadTarget = Storage.exists(target.c_str());
  if (hadTarget && !Storage.rename(target.c_str(), bak.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (!Storage.rename(tmp.c_str(), target.c_str())) {
    if (hadTarget && Storage.exists(bak.c_str())) Storage.rename(bak.c_str(), target.c_str());
    Storage.remove(tmp.c_str());
    return false;
  }
  if (Storage.exists(bak.c_str())) Storage.remove(bak.c_str());
  return true;
}

bool restoreBundle(const char* sourcePath) {
  FsFile bundle;
  if (!Storage.openFileForRead(LOG_TAG, sourcePath, bundle)) return false;
  uint8_t magic[sizeof(BUNDLE_MAGIC)] = {};
  uint8_t version = 0;
  if (!readExact(bundle, magic, sizeof(magic)) || !readExact(bundle, &version, 1) ||
      memcmp(magic, BUNDLE_MAGIC, sizeof(BUNDLE_MAGIC)) != 0 || version != BUNDLE_VERSION) {
    bundle.close();
    return false;
  }

  uint32_t restoredCount = 0;
  while (true) {
    uint16_t pathLen = 0;
    if (!readU16(bundle, pathLen)) {
      bundle.close();
      return false;
    }
    if (pathLen == 0) break;
    if (pathLen > MAX_RELATIVE_PATH) {
      bundle.close();
      return false;
    }
    uint32_t fileSize = 0;
    if (!readU32(bundle, fileSize)) {
      bundle.close();
      return false;
    }
    char pathBuffer[MAX_RELATIVE_PATH + 1] = {};
    if (!readExact(bundle, pathBuffer, pathLen)) {
      bundle.close();
      return false;
    }
    pathBuffer[pathLen] = '\0';
    if (!restoreOneEntry(bundle, pathBuffer, fileSize)) {
      LOG_ERR(LOG_TAG, "Failed restoring reading-state entry: %s", pathBuffer);
      bundle.close();
      return false;
    }
    ++restoredCount;
  }
  bundle.close();

  if (restoredCount == 0 || !isSupportedStatsFile(GLOBAL_STATS_PATH)) {
    LOG_ERR(LOG_TAG, "Reading-state bundle restored without a valid global stats file");
    return false;
  }
  LOG_DBG(LOG_TAG, "Restored full reading-state backup: %u file(s)", static_cast<unsigned>(restoredCount));
  return true;
}

bool restoreLegacyGlobalBackup(const char* sourcePath) {
  size_t sourceSize = 0;
  if (!isSupportedStatsFile(sourcePath, &sourceSize)) return false;
  constexpr char tmpPath[] = "/.inkmod/global_stats.restore.tmp";
  constexpr char bakPath[] = "/.inkmod/global_stats.bin.bak";
  if (Storage.exists(tmpPath) && !Storage.remove(tmpPath)) return false;
  if (!copyFileExact(sourcePath, tmpPath, sourceSize) || !isSupportedStatsFile(tmpPath)) {
    Storage.remove(tmpPath);
    return false;
  }
  if (Storage.exists(bakPath) && !Storage.remove(bakPath)) {
    Storage.remove(tmpPath);
    return false;
  }
  if (Storage.exists(GLOBAL_STATS_PATH) && !Storage.rename(GLOBAL_STATS_PATH, bakPath)) {
    Storage.remove(tmpPath);
    return false;
  }
  if (!Storage.rename(tmpPath, GLOBAL_STATS_PATH)) {
    if (Storage.exists(bakPath) && !Storage.exists(GLOBAL_STATS_PATH)) Storage.rename(bakPath, GLOBAL_STATS_PATH);
    Storage.remove(tmpPath);
    return false;
  }
  if (!isSupportedStatsFile(GLOBAL_STATS_PATH)) {
    Storage.remove(GLOBAL_STATS_PATH);
    if (Storage.exists(bakPath)) Storage.rename(bakPath, GLOBAL_STATS_PATH);
    return false;
  }
  return true;
}
}  // namespace

bool backupGlobalStats(const bool manual, char* outFileName, const size_t outFileNameLen) {
  if (!Storage.ensureDirectoryExists(BACKUP_DIR)) return false;
  char fileName[64];
  if (!chooseBackupName(manual, fileName, sizeof(fileName))) return false;
  char backupPath[160];
  const int n = snprintf(backupPath, sizeof(backupPath), "%s/%s", BACKUP_DIR, fileName);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(backupPath)) return false;
  if (!writeReadingStateBundle(backupPath)) return false;
  pruneBackups(DEFAULT_BACKUP_KEEP_COUNT);
  if (outFileName && outFileNameLen > 0) copyString(fileName, outFileName, outFileNameLen);
  return true;
}

int pruneBackups(const int keep) {
  if (keep < 0) return 0;
  FsFile dir = Storage.open(BACKUP_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  char name[128];
  std::vector<BackupName> names;
  names.reserve(16);
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (isDirectory || nameLen == 0 || !isStatsBackupFileName(name)) continue;
    BackupName backupName;
    if (copyString(name, backupName.value, sizeof(backupName.value))) names.push_back(backupName);
  }
  dir.close();
  if (static_cast<int>(names.size()) <= keep) return 0;
  std::sort(names.begin(), names.end(), [](const BackupName& a, const BackupName& b) { return strcmp(a.value, b.value) < 0; });
  int removed = 0;
  const int toRemove = static_cast<int>(names.size()) - keep;
  for (int i = 0; i < toRemove; ++i) {
    char path[160];
    const int n = snprintf(path, sizeof(path), "%s/%s", BACKUP_DIR, names[static_cast<size_t>(i)].value);
    if (n > 0 && static_cast<size_t>(n) < sizeof(path) && Storage.remove(path)) ++removed;
  }
  return removed;
}

std::vector<std::string> listGlobalStatsBackups() {
  std::vector<std::string> result;
  FsFile dir = Storage.open(BACKUP_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return result;
  }
  char name[128];
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (!isDirectory && nameLen > 0 && isStatsBackupFileName(name)) result.emplace_back(name);
  }
  dir.close();
  std::sort(result.begin(), result.end(), [](const std::string& a, const std::string& b) { return a > b; });
  return result;
}

bool restoreGlobalStatsFromBackup(const char* fileName) {
  if (!fileName || !isStatsBackupFileName(fileName) || strchr(fileName, '/') || strchr(fileName, '\\')) return false;
  char sourcePath[160];
  const int n = snprintf(sourcePath, sizeof(sourcePath), "%s/%s", BACKUP_DIR, fileName);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(sourcePath)) return false;

  // New backups contain global + per-book stats + progress. Older backups are
  // still accepted and restore only the global stats they originally stored.
  if (isBundleBackup(sourcePath)) return restoreBundle(sourcePath);
  return restoreLegacyGlobalBackup(sourcePath);
}


bool deleteGlobalStatsBackup(const char* fileName) {
  if (!isStatsBackupFileName(fileName) || strchr(fileName, '/') != nullptr || strchr(fileName, '\\') != nullptr) {
    LOG_ERR(LOG_TAG, "Rejected invalid backup name for delete");
    return false;
  }
  const std::string path = std::string(BACKUP_DIR) + "/" + fileName;
  if (!Storage.exists(path.c_str())) return false;
  const bool removed = Storage.remove(path.c_str());
  if (removed) LOG_DBG(LOG_TAG, "Deleted stats backup: %s", fileName);
  return removed;
}
