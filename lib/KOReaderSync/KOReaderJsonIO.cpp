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
  store.setServerUrl(doc["serverUrl"] | std::string(""));

  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  store.setMatchMethod(static_cast<DocumentMatchMethod>(method));

  return true;
}

}  // namespace KOReaderJsonIO
