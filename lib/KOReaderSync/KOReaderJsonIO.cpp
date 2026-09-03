#include "KOReaderJsonIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include "KOReaderCredentialStore.h"

namespace KOReaderJsonIO {

bool save(const KOReaderCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["username"] = store.getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(store.getPassword());
  doc["serverUrl"] = store.getServerUrl();
  doc["matchMethod"] = static_cast<uint8_t>(store.getMatchMethod());
  doc["sendMetadata"] = store.getSendMetadata();
  doc["syncBehavior"] = static_cast<uint8_t>(store.getSyncBehavior());

  FsFile file;
  if (!Storage.openFileForWrite("KRS", path, file)) {
    return false;
  }

  // Serialize straight into the SD file instead of building a second complete
  // Arduino String first. This reduces peak heap/copying on ESP32-C3 while
  // preserving the exact same JSON representation on disk.
  const size_t expected = measureJson(doc);
  const size_t written = serializeJson(doc, file);
  const bool closed = file.close();
  return written == expected && closed;
}

bool load(KOReaderCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("KRS", "JSON parse error: %s", error.c_str());
    return false;
  }

  std::string user = doc["username"] | std::string("");

  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &status);
  if (status == obfuscation::DecodeStatus::LEGACY && !pass.empty() && needsResave) {
    *needsResave = true;
  }
  if (status == obfuscation::DecodeStatus::INVALID || status == obfuscation::DecodeStatus::EMPTY || pass.empty()) {
    pass = doc["password"] | std::string("");
    if (!pass.empty() && needsResave) *needsResave = true;
  }
  if (status == obfuscation::DecodeStatus::INVALID && pass.empty()) {
    LOG_ERR("KRS", "Ignoring unreadable KOReader password");
  }

  store.setCredentials(user, pass);

  // CrossPoint now uses sync.crosspointreader.com as the default server.
  // Old inkMOD files used an empty URL to mean sync.koreader.rocks, so pin
  // those legacy files explicitly instead of silently moving an existing
  // account to another server.
  const bool hasSyncBehavior = !doc["syncBehavior"].isNull();
  const bool hasSendMetadata = !doc["sendMetadata"].isNull();
  const bool legacyInkmodFile = !hasSyncBehavior && !hasSendMetadata;
  std::string server = doc["serverUrl"] | std::string("");
  if (legacyInkmodFile && server.empty() && !user.empty()) {
    server = "https://sync.koreader.rocks";
    if (needsResave) *needsResave = true;
  }
  store.setServerUrl(server);

  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  store.setMatchMethod(static_cast<DocumentMatchMethod>(method));
  store.setSendMetadata(doc["sendMetadata"] | false);

  if (hasSyncBehavior) {
    const uint8_t behavior = doc["syncBehavior"] | static_cast<uint8_t>(KOReaderSyncBehavior::SMART);
    store.setSyncBehavior(behavior == static_cast<uint8_t>(KOReaderSyncBehavior::ASK_EVERY_TIME)
                              ? KOReaderSyncBehavior::ASK_EVERY_TIME
                              : KOReaderSyncBehavior::SMART);
  } else {
    // Preserve the old interactive flow for migrated users.
    store.setSyncBehavior(KOReaderSyncBehavior::ASK_EVERY_TIME);
    if (needsResave) *needsResave = true;
  }

  return true;
}

}  // namespace KOReaderJsonIO
