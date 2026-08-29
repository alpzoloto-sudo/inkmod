#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Creates a compact reading-state backup in /.inkmod-stats-backup/ containing:
//   - /.inkmod/global_stats.bin
//   - every per-book stats.bin
//   - every per-book progress.bin
// The historical function name is kept to avoid touching callers.
bool backupGlobalStats(bool manual, char* outFileName = nullptr, size_t outFileNameLen = 0);

int pruneBackups(int keep = 7);
std::vector<std::string> listGlobalStatsBackups();

// Restores a new full reading-state bundle. Legacy backups created by older
// versions are also accepted; those can restore global_stats.bin only because
// they never contained per-book stats/progress.
bool restoreGlobalStatsFromBackup(const char* fileName);

// Deletes exactly one validated backup file from /.inkmod-stats-backup/.
bool deleteGlobalStatsBackup(const char* fileName);
