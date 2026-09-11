// ConfigStore: thin typed accessors over ConfigBackend. This part is pure
// logic (native-testable) and maps 1:1 onto docs/CONFIG_AND_STATE.md's key
// table; the NVS-backed ConfigBackend implementation lives in
// config_store_nvs.cpp.

#include "transit/config_store.h"

namespace transit {

namespace {

// hidden_routes[] / route_order[]: NVS/Preferences has no native array type,
// so these are persisted as a single comma-joined string under one key.
// global_route_id values are opaque IDs of the form "<agency>:<id>" (e.g.
// "1:897" — docs/DATA_MODEL.md, docs/API_CONTRACT.md); none of the documented
// examples contain a comma, so a plain join/split is safe in practice. Empty
// entries (e.g. from a stray leading/trailing/doubled separator) are dropped
// on read so a round-trip never produces spurious blank IDs.
std::string joinCsv(const std::vector<std::string>& values) {
  std::string joined;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) joined += ',';
    joined += values[i];
  }
  return joined;
}

std::vector<std::string> splitCsv(const std::string& joined) {
  std::vector<std::string> values;
  size_t start = 0;
  while (start <= joined.size()) {
    size_t comma = joined.find(',', start);
    size_t end = (comma == std::string::npos) ? joined.size() : comma;
    if (end > start) {
      values.push_back(joined.substr(start, end - start));
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return values;
}

}  // namespace

ConfigStore::ConfigStore(ConfigBackend& backend) : backend_(backend) {}

bool ConfigStore::isProvisioned() {
  return backend_.isSet("wifi_ssid") && backend_.isSet("api_key") && backend_.isSet("stop_id");
}

std::string ConfigStore::wifiSsid() { return backend_.getString("wifi_ssid", ""); }
void ConfigStore::setWifiSsid(const std::string& ssid) { backend_.setString("wifi_ssid", ssid); }

std::string ConfigStore::wifiPassword() { return backend_.getString("wifi_pass", ""); }
void ConfigStore::setWifiPassword(const std::string& password) {
  backend_.setString("wifi_pass", password);
}

std::string ConfigStore::apiKey() { return backend_.getString("api_key", ""); }
void ConfigStore::setApiKey(const std::string& apiKey) { backend_.setString("api_key", apiKey); }

std::string ConfigStore::stopId() { return backend_.getString("stop_id", ""); }
void ConfigStore::setStopId(const std::string& stopId) { backend_.setString("stop_id", stopId); }

// NVS/Preferences key names are capped at 15 characters (NVS_KEY_NAME_MAX_SIZE
// is 16 including the null terminator) — "refresh_interval_min" (20 chars)
// would fail at runtime with ESP_ERR_NVS_KEY_TOO_LONG despite compiling fine,
// so the on-disk key is abbreviated to fit, same as sleep_win_start/end and
// max_dep_per_dir below.
int ConfigStore::refreshIntervalMin() { return backend_.getInt("refresh_int_min", 60); }
void ConfigStore::setRefreshIntervalMin(int minutes) {
  backend_.setInt("refresh_int_min", minutes);
}

int ConfigStore::sleepWindowStartMin() { return backend_.getInt("sleep_win_start", -1); }
void ConfigStore::setSleepWindowStartMin(int minutesSinceMidnight) {
  backend_.setInt("sleep_win_start", minutesSinceMidnight);
}

int ConfigStore::sleepWindowEndMin() { return backend_.getInt("sleep_win_end", -1); }
void ConfigStore::setSleepWindowEndMin(int minutesSinceMidnight) {
  backend_.setInt("sleep_win_end", minutesSinceMidnight);
}

// "departure_win_min" (17 chars) exceeds the 15-char NVS key limit (see note
// on refreshIntervalMin above) — abbreviated further to fit.
int ConfigStore::departureWindowMin() { return backend_.getInt("dep_win_min", 110); }
void ConfigStore::setDepartureWindowMin(int minutes) {
  backend_.setInt("dep_win_min", minutes);
}

int ConfigStore::maxDeparturesPerDirection() { return backend_.getInt("max_dep_per_dir", 3); }
void ConfigStore::setMaxDeparturesPerDirection(int count) {
  backend_.setInt("max_dep_per_dir", count);
}

bool ConfigStore::sortByTime() { return backend_.getBool("sort_by_time", false); }
void ConfigStore::setSortByTime(bool enabled) { backend_.setBool("sort_by_time", enabled); }

// "static_direction" (16 chars) exceeds the 15-char NVS key limit (see note
// on refreshIntervalMin above) — abbreviated to fit.
int ConfigStore::staticDirection() { return backend_.getInt("static_dir", -1); }
void ConfigStore::setStaticDirection(int directionIndex) {
  backend_.setInt("static_dir", directionIndex);
}

std::vector<std::string> ConfigStore::hiddenRoutes() {
  return splitCsv(backend_.getString("hidden_routes", ""));
}
void ConfigStore::setHiddenRoutes(const std::vector<std::string>& globalRouteIds) {
  backend_.setString("hidden_routes", joinCsv(globalRouteIds));
}

std::vector<std::string> ConfigStore::routeOrder() {
  return splitCsv(backend_.getString("route_order", ""));
}
void ConfigStore::setRouteOrder(const std::vector<std::string>& globalRouteIds) {
  backend_.setString("route_order", joinCsv(globalRouteIds));
}

std::string ConfigStore::timeFormat() { return backend_.getString("time_format", "HH:mm"); }
void ConfigStore::setTimeFormat(const std::string& format) {
  backend_.setString("time_format", format);
}

std::string ConfigStore::locale() { return backend_.getString("locale", ""); }
void ConfigStore::setLocale(const std::string& locale) { backend_.setString("locale", locale); }

bool ConfigStore::displayPortrait() { return backend_.getBool("portrait", false); }
void ConfigStore::setDisplayPortrait(bool portrait) { backend_.setBool("portrait", portrait); }

std::string ConfigStore::staStopCode() { return backend_.getString("sta_stop", ""); }
void ConfigStore::setStaStopCode(const std::string& stopCode) { backend_.setString("sta_stop", stopCode); }

}  // namespace transit
