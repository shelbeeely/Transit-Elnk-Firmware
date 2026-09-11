#pragma once

// Transit-Elnk-Firmware — persisted configuration.
//
// Keys/types/defaults match docs/CONFIG_AND_STATE.md's "Firmware ConfigStore
// (NVS) — proposed key mapping" table (see that doc's note below the table
// for the handful of on-disk key names abbreviated to fit NVS's 15-character
// key-name limit). Backed by NVS via Arduino
// Preferences on [env:xteink_x4] (see src/transit/config_store.cpp, unit 3);
// injectable via ConfigBackend so ConfigStore itself is unit-testable under
// [env:native] against an in-memory backend.
//
// Frozen contract for the parallel work units: do not remove/rename existing
// keys or accessors. Adding a key is fine; note it in your PR description.

#include <cstdint>
#include <string>
#include <vector>

namespace transit {

// Minimal typed key/value backend. NVS-backed implementation lives in
// src/transit/config_store.cpp; tests use an in-memory std::map backend.
class ConfigBackend {
 public:
  virtual ~ConfigBackend() = default;
  virtual std::string getString(const char* key, const std::string& defaultValue) = 0;
  virtual void setString(const char* key, const std::string& value) = 0;
  virtual int32_t getInt(const char* key, int32_t defaultValue) = 0;
  virtual void setInt(const char* key, int32_t value) = 0;
  virtual bool getBool(const char* key, bool defaultValue) = 0;
  virtual void setBool(const char* key, bool value) = 0;
  // Whether the given key has ever been explicitly set. Used to distinguish
  // "unconfigured" from "set to the empty/zero value".
  virtual bool isSet(const char* key) = 0;
};

// NVS-backed ConfigBackend (Arduino Preferences), for [env:xteink_x4]
// (implementation in src/transit/config_store_nvs.cpp, excluded from
// [env:native]'s build — Preferences.h isn't available there). Tests under
// [env:native] use their own in-memory ConfigBackend instead.
class NvsConfigBackend : public ConfigBackend {
 public:
  std::string getString(const char* key, const std::string& defaultValue) override;
  void setString(const char* key, const std::string& value) override;
  int32_t getInt(const char* key, int32_t defaultValue) override;
  void setInt(const char* key, int32_t value) override;
  bool getBool(const char* key, bool defaultValue) override;
  void setBool(const char* key, bool value) override;
  bool isSet(const char* key) override;
};

class ConfigStore {
 public:
  explicit ConfigStore(ConfigBackend& backend);

  // True once wifi_ssid, api_key, and stop_id have all been set at least
  // once — gates whether main.cpp runs the first-run setup flow.
  bool isProvisioned();

  std::string wifiSsid();
  void setWifiSsid(const std::string& ssid);
  std::string wifiPassword();
  void setWifiPassword(const std::string& password);

  // Never compiled into tracked source (see root CLAUDE.md) — entered via
  // on-device setup and stored here only.
  std::string apiKey();
  void setApiKey(const std::string& apiKey);

  // Preferred: a fixed global_stop_id picked once via nearby_stops/
  // search_stops during setup (docs/CONFIG_AND_STATE.md), used with
  // stop_departures thereafter instead of a live radius search per poll.
  std::string stopId();
  void setStopId(const std::string& stopId);

  // Minutes between polls while awake; treated as a floor, default 60
  // (docs/DEPLOYMENT_OPS.md's free-tier rate-limit math).
  int refreshIntervalMin();
  void setRefreshIntervalMin(int minutes);

  // Minutes since midnight, local time. -1 = unset (no sleep-window
  // stretching; refresh_interval_min applies 24/7).
  int sleepWindowStartMin();
  void setSleepWindowStartMin(int minutesSinceMidnight);
  int sleepWindowEndMin();
  void setSleepWindowEndMin(int minutesSinceMidnight);

  int departureWindowMin();
  void setDepartureWindowMin(int minutes);

  int maxDeparturesPerDirection();
  void setMaxDeparturesPerDirection(int count);

  bool sortByTime();
  void setSortByTime(bool enabled);

  // -1 = off (normal per-route toggling).
  int staticDirection();
  void setStaticDirection(int directionIndex);

  std::vector<std::string> hiddenRoutes();
  void setHiddenRoutes(const std::vector<std::string>& globalRouteIds);

  std::vector<std::string> routeOrder();
  void setRouteOrder(const std::vector<std::string>& globalRouteIds);

  // "HH:mm" or "hh:mm A".
  std::string timeFormat();
  void setTimeFormat(const std::string& format);

  std::string locale();
  void setLocale(const std::string& locale);

  // Display orientation: false = landscape (the X4 panel's native
  // orientation, default), true = portrait. See render_engine.h/main.cpp
  // for how this selects freeink::ui::Orientation. Changeable after initial
  // setup via SetupFlow::runSettingsPortal(), not just first-run setup.
  bool displayPortrait();
  void setDisplayPortrait(bool portrait);

 private:
  ConfigBackend& backend_;
};

}  // namespace transit
