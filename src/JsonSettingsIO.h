#pragma once

class InkMODSettings;
class InkMODState;
class WifiCredentialStore;
class RecentBooksStore;
class OpdsServerStore;

namespace JsonSettingsIO {

// InkMODSettings
bool saveSettings(const InkMODSettings& s, const char* path);
bool loadSettings(InkMODSettings& s, const char* json, bool* needsResave = nullptr);

// InkMODState
bool saveState(const InkMODState& s, const char* path);
bool loadState(InkMODState& s, const char* json);

// WifiCredentialStore
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave = nullptr);

// RecentBooksStore
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooks(RecentBooksStore& store, const char* json);

// OpdsServerStore
bool saveOpds(const OpdsServerStore& store, const char* path);
bool loadOpds(OpdsServerStore& store, const char* json, bool* needsResave = nullptr);

}  // namespace JsonSettingsIO
