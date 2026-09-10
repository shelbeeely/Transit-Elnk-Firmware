// ConfigStore: thin typed accessors over ConfigBackend. This part is pure
// logic (native-testable); the NVS-backed ConfigBackend implementation is in
// config_store_nvs.cpp (unit 3 replaces both with the real thing per
// docs/CONFIG_AND_STATE.md's key table — this stub uses reasonable defaults
// so the scaffold links and runs end to end before that unit lands).

#include "transit/config_store.h"

namespace transit {

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

int ConfigStore::refreshIntervalMin() { return backend_.getInt("refresh_interval_min", 60); }
void ConfigStore::setRefreshIntervalMin(int minutes) {
  backend_.setInt("refresh_interval_min", minutes);
}

int ConfigStore::sleepWindowStartMin() { return backend_.getInt("sleep_win_start", -1); }
void ConfigStore::setSleepWindowStartMin(int minutesSinceMidnight) {
  backend_.setInt("sleep_win_start", minutesSinceMidnight);
}

int ConfigStore::sleepWindowEndMin() { return backend_.getInt("sleep_win_end", -1); }
void ConfigStore::setSleepWindowEndMin(int minutesSinceMidnight) {
  backend_.setInt("sleep_win_end", minutesSinceMidnight);
}

int ConfigStore::departureWindowMin() { return backend_.getInt("departure_win_min", 110); }
void ConfigStore::setDepartureWindowMin(int minutes) {
  backend_.setInt("departure_win_min", minutes);
}

int ConfigStore::maxDeparturesPerDirection() { return backend_.getInt("max_dep_per_dir", 3); }
void ConfigStore::setMaxDeparturesPerDirection(int count) {
  backend_.setInt("max_dep_per_dir", count);
}

bool ConfigStore::sortByTime() { return backend_.getBool("sort_by_time", false); }
void ConfigStore::setSortByTime(bool enabled) { backend_.setBool("sort_by_time", enabled); }

int ConfigStore::staticDirection() { return backend_.getInt("static_direction", -1); }
void ConfigStore::setStaticDirection(int directionIndex) {
  backend_.setInt("static_direction", directionIndex);
}

// hidden_routes[] / route_order[]: stub stores/returns nothing yet — unit 3
// picks a concrete serialization (e.g. comma-joined into one NVS string key)
// per docs/CONFIG_AND_STATE.md.
std::vector<std::string> ConfigStore::hiddenRoutes() { return {}; }
void ConfigStore::setHiddenRoutes(const std::vector<std::string>& /*globalRouteIds*/) {}

std::vector<std::string> ConfigStore::routeOrder() { return {}; }
void ConfigStore::setRouteOrder(const std::vector<std::string>& /*globalRouteIds*/) {}

std::string ConfigStore::timeFormat() { return backend_.getString("time_format", "HH:mm"); }
void ConfigStore::setTimeFormat(const std::string& format) {
  backend_.setString("time_format", format);
}

std::string ConfigStore::locale() { return backend_.getString("locale", ""); }
void ConfigStore::setLocale(const std::string& locale) { backend_.setString("locale", locale); }

}  // namespace transit
