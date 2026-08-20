#include "OpdsServerStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <cstring>

#include "InkMODSettings.h"

OpdsServerStore OpdsServerStore::instance;

namespace {
constexpr char OPDS_FILE_JSON[] = "/.inkmod/opds.json";
constexpr char FILENAME_FORMAT_AUTHOR_TITLE[] = "author_title";
constexpr char FILENAME_FORMAT_TITLE_AUTHOR[] = "title_author";

bool sameServer(const OpdsServer& a, const OpdsServer& b) {
  return a.name == b.name && a.url == b.url && a.username == b.username && a.password == b.password &&
         a.filenameFormat == b.filenameFormat;
}
}  // namespace

const char* opdsFilenameFormatToJson(const OpdsFilenameFormat format) {
  switch (format) {
    case OpdsFilenameFormat::TITLE_AUTHOR:
      return FILENAME_FORMAT_TITLE_AUTHOR;
    case OpdsFilenameFormat::AUTHOR_TITLE:
    default:
      return FILENAME_FORMAT_AUTHOR_TITLE;
  }
}

OpdsFilenameFormat opdsFilenameFormatFromJson(const char* value) {
  if (value && strcmp(value, FILENAME_FORMAT_TITLE_AUTHOR) == 0) {
    return OpdsFilenameFormat::TITLE_AUTHOR;
  }
  return OpdsFilenameFormat::AUTHOR_TITLE;
}

bool OpdsServerStore::saveToFile() const {
  Storage.mkdir("/.inkmod");
  return JsonSettingsIO::saveOpds(*this, OPDS_FILE_JSON);
}

bool OpdsServerStore::loadFromFile() {
  // readFile() already performs a single open and returns an empty String when
  // the file is absent. Avoid a separate exists() directory lookup on every boot.
  String json = Storage.readFile(OPDS_FILE_JSON);
  if (!json.isEmpty()) {
    // resave flag is set when passwords were stored in plaintext and need re-obfuscation
    bool resave = false;
    bool result = JsonSettingsIO::loadOpds(*this, json.c_str(), &resave);
    if (result && resave) {
      LOG_DBG("OPS", "Resaving JSON with obfuscated passwords");
      saveToFile();
    }
    return result;
  }

  // No opds.json found — attempt one-time migration from the legacy single-server
  // fields in InkMODSettings (opdsServerUrl/opdsUsername/opdsPassword).
  if (migrateFromSettings()) {
    LOG_DBG("OPS", "Migrated legacy OPDS settings");
    return true;
  }

  return false;
}

bool OpdsServerStore::migrateFromSettings() {
  if (strlen(SETTINGS.opdsServerUrl) == 0) {
    return false;
  }

  OpdsServer server;
  server.name = "OPDS Server";
  server.url = SETTINGS.opdsServerUrl;
  server.username = SETTINGS.opdsUsername;
  server.password = SETTINGS.opdsPassword;
  servers.push_back(std::move(server));

  if (saveToFile()) {
    // Clear legacy fields so migration won't run again on next boot
    SETTINGS.opdsServerUrl[0] = '\0';
    SETTINGS.opdsUsername[0] = '\0';
    SETTINGS.opdsPassword[0] = '\0';
    SETTINGS.saveToFile();
    LOG_DBG("OPS", "Migrated single-server OPDS config to opds.json");
    return true;
  }

  // Save failed — roll back in-memory state so we don't have a partial migration
  servers.clear();
  return false;
}

bool OpdsServerStore::addServer(const OpdsServer& server) {
  if (servers.size() >= MAX_SERVERS) {
    LOG_DBG("OPS", "Cannot add more servers, limit of %zu reached", MAX_SERVERS);
    return false;
  }

  servers.push_back(server);
  LOG_DBG("OPS", "Added server: %s", server.name.c_str());
  return saveToFile();
}

bool OpdsServerStore::updateServer(size_t index, const OpdsServer& server) {
  if (index >= servers.size()) {
    return false;
  }

  // Saving an unchanged settings form should not rewrite the whole JSON file.
  if (sameServer(servers[index], server)) return true;

  servers[index] = server;
  LOG_DBG("OPS", "Updated server: %s", server.name.c_str());
  return saveToFile();
}

bool OpdsServerStore::removeServer(size_t index) {
  if (index >= servers.size()) {
    return false;
  }

  LOG_DBG("OPS", "Removed server: %s", servers[index].name.c_str());
  servers.erase(servers.begin() + static_cast<ptrdiff_t>(index));
  return saveToFile();
}

const OpdsServer* OpdsServerStore::getServer(size_t index) const {
  if (index >= servers.size()) {
    return nullptr;
  }
  return &servers[index];
}
