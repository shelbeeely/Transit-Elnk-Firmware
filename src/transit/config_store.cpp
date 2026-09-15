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

// Best-effort int parse with no exceptions (this codebase avoids them —
// see joinCsv/splitCsv above for the same "never throw" style). Returns
// defaultValue on anything that isn't a valid (optionally signed) integer.
int parseIntOr(const std::string& text, int defaultValue) {
  if (text.empty()) return defaultValue;
  size_t i = 0;
  bool negative = false;
  if (text[0] == '-' || text[0] == '+') {
    negative = text[0] == '-';
    i = 1;
  }
  if (i >= text.size()) return defaultValue;
  int value = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return defaultValue;
    value = value * 10 + (text[i] - '0');
  }
  return negative ? -value : value;
}

// One preset leg = "routeId,boardStopId,alightStopId,directionId"; legs
// joined with '|'. IDs are opaque "agency:id" strings that never contain
// ',' or '|' (same reasoning as joinCsv's comment above), so this is safe.
std::string encodeLeg(const TripLegConfig& leg) {
  return leg.routeId + "," + leg.boardStopId + "," + leg.alightStopId + "," +
         std::to_string(leg.directionId);
}

TripLegConfig decodeLeg(const std::string& encoded) {
  std::vector<std::string> fields = splitCsv(encoded);
  TripLegConfig leg;
  if (fields.size() > 0) leg.routeId = fields[0];
  if (fields.size() > 1) leg.boardStopId = fields[1];
  if (fields.size() > 2) leg.alightStopId = fields[2];
  if (fields.size() > 3) leg.directionId = parseIntOr(fields[3], -1);
  return leg;
}

std::string encodeLegs(const std::vector<TripLegConfig>& legs) {
  std::string joined;
  for (size_t i = 0; i < legs.size(); ++i) {
    if (i > 0) joined += '|';
    joined += encodeLeg(legs[i]);
  }
  return joined;
}

std::vector<TripLegConfig> decodeLegs(const std::string& joined) {
  std::vector<TripLegConfig> legs;
  size_t start = 0;
  while (start <= joined.size()) {
    size_t bar = joined.find('|', start);
    size_t end = (bar == std::string::npos) ? joined.size() : bar;
    if (end > start) {
      legs.push_back(decodeLeg(joined.substr(start, end - start)));
    }
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  return legs;
}

// One agency = "agencyId,stopCode,enabled(0/1)"; agencies joined with '|' —
// same shape as encodeLeg/encodeLegs above, for the same reason (NVS has no
// array type). agencyId is constrained by agencies/registry.schema.json's
// pattern to lowercase letters/digits/hyphens, so it can never contain ','
// or '|'; stopCode is assumed the same in practice (true of every
// stop_code_convention documented so far) rather than escaped.
//
// stopCode must never be persisted empty: splitCsv (like joinCsv/splitCsv's
// own comment notes) drops empty fields on read, which would shift
// "enabled" into stopCode's position for that entry. This isn't reachable
// through setSecondSourceAgencies() as intended -- "turn an agency off"
// means removing its entry (empty list = none configured, matching the old
// staStopCode()'s empty-means-off behavior) or setting enabled=false with
// its real stopCode kept, never writing an empty stopCode into a kept
// entry.
std::string encodeAgency(const SecondSourceAgencyConfig& agency) {
  return agency.agencyId + "," + agency.stopCode + "," + (agency.enabled ? "1" : "0");
}

SecondSourceAgencyConfig decodeAgency(const std::string& encoded) {
  std::vector<std::string> fields = splitCsv(encoded);
  SecondSourceAgencyConfig agency;
  if (fields.size() > 0) agency.agencyId = fields[0];
  if (fields.size() > 1) agency.stopCode = fields[1];
  agency.enabled = fields.size() > 2 ? fields[2] != "0" : true;
  return agency;
}

std::string encodeAgencies(const std::vector<SecondSourceAgencyConfig>& agencies) {
  std::string joined;
  for (size_t i = 0; i < agencies.size(); ++i) {
    if (i > 0) joined += '|';
    joined += encodeAgency(agencies[i]);
  }
  return joined;
}

std::vector<SecondSourceAgencyConfig> decodeAgencies(const std::string& joined) {
  std::vector<SecondSourceAgencyConfig> agencies;
  size_t start = 0;
  while (start <= joined.size()) {
    size_t bar = joined.find('|', start);
    size_t end = (bar == std::string::npos) ? joined.size() : bar;
    if (end > start) {
      agencies.push_back(decodeAgency(joined.substr(start, end - start)));
    }
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  return agencies;
}

const char* presetLegsKey(ConfigStore::PresetId id) {
  return id == ConfigStore::PresetId::kHome ? "home_legs" : "work_legs";
}

const char* presetWalkKey(ConfigStore::PresetId id) {
  return id == ConfigStore::PresetId::kHome ? "home_walk_min" : "work_walk_min";
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

std::vector<SecondSourceAgencyConfig> ConfigStore::secondSourceAgencies() {
  return decodeAgencies(backend_.getString("agency_list", ""));
}
void ConfigStore::setSecondSourceAgencies(const std::vector<SecondSourceAgencyConfig>& agencies) {
  backend_.setString("agency_list", encodeAgencies(agencies));
}

bool ConfigStore::secondSourceShowAllEnabled() { return backend_.getBool("agency_all", false); }
void ConfigStore::setSecondSourceShowAllEnabled(bool enabled) {
  backend_.setBool("agency_all", enabled);
}

std::string ConfigStore::activeSecondSourceAgencyId() {
  return backend_.getString("agency_active", "");
}
void ConfigStore::setActiveSecondSourceAgencyId(const std::string& agencyId) {
  backend_.setString("agency_active", agencyId);
}

std::string ConfigStore::activeSecondSourceStopCode() {
  std::vector<SecondSourceAgencyConfig> agencies = secondSourceAgencies();
  const std::string activeId = activeSecondSourceAgencyId();

  if (!activeId.empty()) {
    for (const SecondSourceAgencyConfig& agency : agencies) {
      if (agency.agencyId == activeId && agency.enabled) return agency.stopCode;
    }
  }
  for (const SecondSourceAgencyConfig& agency : agencies) {
    if (agency.enabled) return agency.stopCode;
  }
  return "";
}

std::vector<TripLegConfig> ConfigStore::presetLegs(PresetId id) {
  return decodeLegs(backend_.getString(presetLegsKey(id), ""));
}
void ConfigStore::setPresetLegs(PresetId id, const std::vector<TripLegConfig>& legs) {
  backend_.setString(presetLegsKey(id), encodeLegs(legs));
}

int ConfigStore::presetWalkToFirstStopMin(PresetId id) {
  return backend_.getInt(presetWalkKey(id), 0);
}
void ConfigStore::setPresetWalkToFirstStopMin(PresetId id, int minutes) {
  backend_.setInt(presetWalkKey(id), minutes);
}

int ConfigStore::transferBufferMin() { return backend_.getInt("xfer_buf_min", 3); }
void ConfigStore::setTransferBufferMin(int minutes) { backend_.setInt("xfer_buf_min", minutes); }

bool ConfigStore::focusMode() { return backend_.getBool("focus_mode", false); }
void ConfigStore::setFocusMode(bool enabled) { backend_.setBool("focus_mode", enabled); }

std::string ConfigStore::timezone() { return backend_.getString("tz", kDefaultPosixTz); }
void ConfigStore::setTimezone(const std::string& posixTz) { backend_.setString("tz", posixTz); }

// Bus Wi-Fi captive-portal auto-login. All four keys are under the 15-char
// NVS limit as written (docs/CONFIG_AND_STATE.md's note on that cap), so
// unlike refresh_interval_min et al. none of them needed abbreviating.
std::string ConfigStore::busWifiSsid() { return backend_.getString("bus_ssid", ""); }
void ConfigStore::setBusWifiSsid(const std::string& ssid) { backend_.setString("bus_ssid", ssid); }

std::string ConfigStore::busWifiIdentity() { return backend_.getString("bus_ident", ""); }
void ConfigStore::setBusWifiIdentity(const std::string& identity) {
  backend_.setString("bus_ident", identity);
}

std::string ConfigStore::busPortalSubmitUrl() { return backend_.getString("bus_form_url", ""); }
void ConfigStore::setBusPortalSubmitUrl(const std::string& url) {
  backend_.setString("bus_form_url", url);
}

std::string ConfigStore::busPortalFieldName() { return backend_.getString("bus_form_fld", ""); }
void ConfigStore::setBusPortalFieldName(const std::string& fieldName) {
  backend_.setString("bus_form_fld", fieldName);
}

std::string ConfigStore::otaManifestUrl() { return backend_.getString("ota_url", ""); }
void ConfigStore::setOtaManifestUrl(const std::string& url) { backend_.setString("ota_url", url); }

OtaTrialState ConfigStore::otaTrialState() {
  OtaTrialState state;
  state.pendingVersion = backend_.getString("ota_pend_ver", "");
  state.bootsAttempted = backend_.getInt("ota_pend_n", 0);
  return state;
}

void ConfigStore::setOtaTrialState(const OtaTrialState& state) {
  backend_.setString("ota_pend_ver", state.pendingVersion);
  backend_.setInt("ota_pend_n", state.bootsAttempted);
}

int ConfigStore::otaConsecutiveFailures() { return backend_.getInt("ota_fails", 0); }

std::string ConfigStore::otaFailingVersion() { return backend_.getString("ota_fail_ver", ""); }

void ConfigStore::recordOtaFailure(const std::string& version) {
  // The count restarts whenever the version being attempted changes: three
  // failures on yesterday's image say nothing about today's, and letting
  // them carry over would block the very build that fixes them.
  const int previous = version == otaFailingVersion() ? otaConsecutiveFailures() : 0;
  backend_.setString("ota_fail_ver", version);
  backend_.setInt("ota_fails", previous + 1);
}

void ConfigStore::clearOtaFailures() {
  backend_.setInt("ota_fails", 0);
  backend_.setString("ota_fail_ver", "");
}

std::string ConfigStore::cachedBoard() { return backend_.getString("cached_board", ""); }
void ConfigStore::setCachedBoard(const std::string& blob) { backend_.setString("cached_board", blob); }

}  // namespace transit
