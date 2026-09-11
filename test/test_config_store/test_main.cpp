// Host-side test for ConfigStore's pure logic (unit 3's NvsConfigBackend
// itself is hardware-only and not testable here — see
// src/transit/config_store_nvs.cpp). Uses a simple in-memory ConfigBackend.

#include <map>
#include <set>
#include <unity.h>

#include "transit/config_store.h"

namespace {

class InMemoryConfigBackend : public transit::ConfigBackend {
 public:
  std::string getString(const char* key, const std::string& defaultValue) override {
    auto it = strings_.find(key);
    return it != strings_.end() ? it->second : defaultValue;
  }
  void setString(const char* key, const std::string& value) override {
    strings_[key] = value;
    setKeys_.insert(key);
  }
  int32_t getInt(const char* key, int32_t defaultValue) override {
    auto it = ints_.find(key);
    return it != ints_.end() ? it->second : defaultValue;
  }
  void setInt(const char* key, int32_t value) override {
    ints_[key] = value;
    setKeys_.insert(key);
  }
  bool getBool(const char* key, bool defaultValue) override {
    auto it = bools_.find(key);
    return it != bools_.end() ? it->second : defaultValue;
  }
  void setBool(const char* key, bool value) override {
    bools_[key] = value;
    setKeys_.insert(key);
  }
  bool isSet(const char* key) override { return setKeys_.count(key) > 0; }

  // Every key that has actually been written, for the NVS key-length
  // check below.
  const std::set<std::string>& writtenKeys() const { return setKeys_; }

 private:
  std::map<std::string, std::string> strings_;
  std::map<std::string, int32_t> ints_;
  std::map<std::string, bool> bools_;
  std::set<std::string> setKeys_;
};


// --- Bus Wi-Fi captive-portal auto-login + offline cache -------------------

void test_bus_wifi_settings_empty_by_default_and_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);

  // Every one of these empty is what turns the whole feature off -- the
  // board must never join an open network it wasn't told about.
  TEST_ASSERT_EQUAL_STRING("", store.busWifiSsid().c_str());
  TEST_ASSERT_EQUAL_STRING("", store.busWifiIdentity().c_str());
  TEST_ASSERT_EQUAL_STRING("", store.busPortalSubmitUrl().c_str());
  TEST_ASSERT_EQUAL_STRING("", store.busPortalFieldName().c_str());

  store.setBusWifiSsid("STA-WiFi");
  store.setBusWifiIdentity("rider@example.com");
  store.setBusPortalSubmitUrl("http://1.2.3.4:8080/auth");
  store.setBusPortalFieldName("phone");

  TEST_ASSERT_EQUAL_STRING("STA-WiFi", store.busWifiSsid().c_str());
  TEST_ASSERT_EQUAL_STRING("rider@example.com", store.busWifiIdentity().c_str());
  TEST_ASSERT_EQUAL_STRING("http://1.2.3.4:8080/auth", store.busPortalSubmitUrl().c_str());
  TEST_ASSERT_EQUAL_STRING("phone", store.busPortalFieldName().c_str());

  // Clearing the SSID alone turns the feature off without discarding the
  // identity the user typed, so re-enabling it doesn't mean re-typing.
  store.setBusWifiSsid("");
  TEST_ASSERT_EQUAL_STRING("", store.busWifiSsid().c_str());
  TEST_ASSERT_EQUAL_STRING("rider@example.com", store.busWifiIdentity().c_str());
}

void test_cached_board_empty_by_default_and_round_trips_a_large_blob() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);

  TEST_ASSERT_EQUAL_STRING("", store.cachedBoard().c_str());

  // A realistic worst case: right at offline_cache.h's own size cap, with
  // the tab/newline delimiters that format uses.
  std::string blob = "TCB1\t1700000000\n";
  while (blob.size() < 3400) blob += "R\t1:31\t31\tbus-31\t31\t\t1A7F37\tFFFFFF\t1\n";
  store.setCachedBoard(blob);
  TEST_ASSERT_EQUAL_STRING(blob.c_str(), store.cachedBoard().c_str());

  store.setCachedBoard("");
  TEST_ASSERT_EQUAL_STRING("", store.cachedBoard().c_str());
}

// NVS key names are capped at 15 characters (NVS_KEY_NAME_MAX_SIZE is 16
// including the null terminator). A longer name compiles fine and then
// fails at runtime with ESP_ERR_NVS_KEY_TOO_LONG -- invisible to every
// host-side test that doesn't check for it, and invisible to the
// cross-compile too. This drives every accessor once and asserts that
// nothing ConfigStore actually writes could trip that at runtime.
void test_every_nvs_key_fits_the_fifteen_character_limit() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);

  store.setWifiSsid("s");
  store.setWifiPassword("p");
  store.setApiKey("k");
  store.setStopId("1:1");
  store.setRefreshIntervalMin(60);
  store.setSleepWindowStartMin(0);
  store.setSleepWindowEndMin(1);
  store.setDepartureWindowMin(90);
  store.setMaxDeparturesPerDirection(3);
  store.setSortByTime(true);
  store.setStaticDirection(0);
  store.setHiddenRoutes({"1:1"});
  store.setRouteOrder({"1:1"});
  store.setTimeFormat("HH:mm");
  store.setLocale("en");
  store.setDisplayPortrait(true);
  store.setStaStopCode("4377");
  store.setPresetLegs(transit::ConfigStore::PresetId::kHome, {});
  store.setPresetLegs(transit::ConfigStore::PresetId::kWork, {});
  store.setPresetWalkToFirstStopMin(transit::ConfigStore::PresetId::kHome, 5);
  store.setPresetWalkToFirstStopMin(transit::ConfigStore::PresetId::kWork, 5);
  store.setTransferBufferMin(3);
  store.setFocusMode(true);
  store.setBusWifiSsid("s");
  store.setBusWifiIdentity("i");
  store.setBusPortalSubmitUrl("u");
  store.setBusPortalFieldName("f");
  store.setCachedBoard("c");

  for (const std::string& key : backend.writtenKeys()) {
    TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(15, key.size(), key.c_str());
  }
  // Sanity check that the loop above actually saw the keys, rather than
  // passing vacuously against an empty set.
  TEST_ASSERT_GREATER_THAN_size_t(20, backend.writtenKeys().size());
}

}  // namespace

void test_unprovisioned_until_wifi_key_and_stop_are_set() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_FALSE(store.isProvisioned());

  store.setWifiSsid("home-wifi");
  store.setApiKey("test-key");
  store.setStopId("1:82774");
  TEST_ASSERT_TRUE(store.isProvisioned());
}

void test_refresh_interval_defaults_to_60() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_INT(60, store.refreshIntervalMin());
}

void test_refresh_interval_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  store.setRefreshIntervalMin(30);
  TEST_ASSERT_EQUAL_INT(30, store.refreshIntervalMin());
}

void test_departure_window_min_default_and_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_INT(110, store.departureWindowMin());
  store.setDepartureWindowMin(90);
  TEST_ASSERT_EQUAL_INT(90, store.departureWindowMin());
}

void test_max_departures_per_direction_default_and_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_INT(3, store.maxDeparturesPerDirection());
  store.setMaxDeparturesPerDirection(5);
  TEST_ASSERT_EQUAL_INT(5, store.maxDeparturesPerDirection());
}

void test_sort_by_time_default_and_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_FALSE(store.sortByTime());
  store.setSortByTime(true);
  TEST_ASSERT_TRUE(store.sortByTime());
}

void test_static_direction_default_and_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_INT(-1, store.staticDirection());
  store.setStaticDirection(2);
  TEST_ASSERT_EQUAL_INT(2, store.staticDirection());
}

void test_time_format_default_and_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_STRING("HH:mm", store.timeFormat().c_str());
  store.setTimeFormat("hh:mm A");
  TEST_ASSERT_EQUAL_STRING("hh:mm A", store.timeFormat().c_str());
}

void test_locale_default_and_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_STRING("", store.locale().c_str());
  store.setLocale("en-CA");
  TEST_ASSERT_EQUAL_STRING("en-CA", store.locale().c_str());
}

void test_display_portrait_defaults_to_landscape_and_round_trips() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_FALSE(store.displayPortrait());
  store.setDisplayPortrait(true);
  TEST_ASSERT_TRUE(store.displayPortrait());
  store.setDisplayPortrait(false);
  TEST_ASSERT_FALSE(store.displayPortrait());
}

void test_sta_stop_code_empty_by_default_and_round_trips() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_STRING("", store.staStopCode().c_str());
  store.setStaStopCode("4377");
  TEST_ASSERT_EQUAL_STRING("4377", store.staStopCode().c_str());
  store.setStaStopCode("");
  TEST_ASSERT_EQUAL_STRING("", store.staStopCode().c_str());
}

void test_sleep_window_start_and_end_default_and_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_INT(-1, store.sleepWindowStartMin());
  TEST_ASSERT_EQUAL_INT(-1, store.sleepWindowEndMin());
  store.setSleepWindowStartMin(23 * 60);
  store.setSleepWindowEndMin(6 * 60 + 30);
  TEST_ASSERT_EQUAL_INT(23 * 60, store.sleepWindowStartMin());
  TEST_ASSERT_EQUAL_INT(6 * 60 + 30, store.sleepWindowEndMin());
}

void test_wifi_ssid_and_password_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_STRING("", store.wifiSsid().c_str());
  TEST_ASSERT_EQUAL_STRING("", store.wifiPassword().c_str());
  store.setWifiSsid("home-wifi");
  store.setWifiPassword("hunter2");
  TEST_ASSERT_EQUAL_STRING("home-wifi", store.wifiSsid().c_str());
  TEST_ASSERT_EQUAL_STRING("hunter2", store.wifiPassword().c_str());
}

void test_api_key_and_stop_id_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  store.setApiKey("test-key");
  store.setStopId("1:82774");
  TEST_ASSERT_EQUAL_STRING("test-key", store.apiKey().c_str());
  TEST_ASSERT_EQUAL_STRING("1:82774", store.stopId().c_str());
}

void test_hidden_routes_empty_by_default() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_INT(0, store.hiddenRoutes().size());
}

void test_hidden_routes_single_entry_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  store.setHiddenRoutes({"1:897"});
  auto routes = store.hiddenRoutes();
  TEST_ASSERT_EQUAL_INT(1, routes.size());
  TEST_ASSERT_EQUAL_STRING("1:897", routes[0].c_str());
}

void test_hidden_routes_multiple_entries_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  store.setHiddenRoutes({"1:897", "1:94380", "2:15"});
  auto routes = store.hiddenRoutes();
  TEST_ASSERT_EQUAL_INT(3, routes.size());
  TEST_ASSERT_EQUAL_STRING("1:897", routes[0].c_str());
  TEST_ASSERT_EQUAL_STRING("1:94380", routes[1].c_str());
  TEST_ASSERT_EQUAL_STRING("2:15", routes[2].c_str());
}

void test_hidden_routes_set_empty_clears() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  store.setHiddenRoutes({"1:897", "1:94380"});
  TEST_ASSERT_EQUAL_INT(2, store.hiddenRoutes().size());
  store.setHiddenRoutes({});
  TEST_ASSERT_EQUAL_INT(0, store.hiddenRoutes().size());
}

void test_route_order_empty_by_default() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_INT(0, store.routeOrder().size());
}

void test_route_order_single_entry_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  store.setRouteOrder({"1:897"});
  auto order = store.routeOrder();
  TEST_ASSERT_EQUAL_INT(1, order.size());
  TEST_ASSERT_EQUAL_STRING("1:897", order[0].c_str());
}

void test_route_order_multiple_entries_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  store.setRouteOrder({"2:15", "1:897", "1:94380"});
  auto order = store.routeOrder();
  TEST_ASSERT_EQUAL_INT(3, order.size());
  TEST_ASSERT_EQUAL_STRING("2:15", order[0].c_str());
  TEST_ASSERT_EQUAL_STRING("1:897", order[1].c_str());
  TEST_ASSERT_EQUAL_STRING("1:94380", order[2].c_str());
}

void test_hidden_routes_and_route_order_are_independent_keys() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  store.setHiddenRoutes({"1:897"});
  store.setRouteOrder({"2:15", "1:94380"});
  auto hidden = store.hiddenRoutes();
  auto order = store.routeOrder();
  TEST_ASSERT_EQUAL_INT(1, hidden.size());
  TEST_ASSERT_EQUAL_STRING("1:897", hidden[0].c_str());
  TEST_ASSERT_EQUAL_INT(2, order.size());
  TEST_ASSERT_EQUAL_STRING("2:15", order[0].c_str());
  TEST_ASSERT_EQUAL_STRING("1:94380", order[1].c_str());
}

void test_preset_legs_empty_by_default() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_TRUE(store.presetLegs(transit::ConfigStore::PresetId::kHome).empty());
  TEST_ASSERT_TRUE(store.presetLegs(transit::ConfigStore::PresetId::kWork).empty());
}

void test_preset_legs_round_trip_single_leg() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);

  transit::TripLegConfig leg;
  leg.routeId = "1:31";
  leg.boardStopId = "1:100";
  leg.alightStopId = "1:200";
  leg.directionId = 0;
  store.setPresetLegs(transit::ConfigStore::PresetId::kHome, {leg});

  auto legs = store.presetLegs(transit::ConfigStore::PresetId::kHome);
  TEST_ASSERT_EQUAL_INT(1, legs.size());
  TEST_ASSERT_EQUAL_STRING("1:31", legs[0].routeId.c_str());
  TEST_ASSERT_EQUAL_STRING("1:100", legs[0].boardStopId.c_str());
  TEST_ASSERT_EQUAL_STRING("1:200", legs[0].alightStopId.c_str());
  TEST_ASSERT_EQUAL_INT(0, legs[0].directionId);
}

void test_preset_legs_round_trip_multiple_legs_unset_direction() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);

  transit::TripLegConfig leg1;
  leg1.routeId = "1:31";
  leg1.boardStopId = "1:100";
  leg1.alightStopId = "1:200";
  leg1.directionId = 0;

  transit::TripLegConfig leg2;
  leg2.routeId = "1:32";
  leg2.boardStopId = "1:200";
  leg2.alightStopId = "1:300";
  leg2.directionId = -1;  // unset

  store.setPresetLegs(transit::ConfigStore::PresetId::kWork, {leg1, leg2});

  auto legs = store.presetLegs(transit::ConfigStore::PresetId::kWork);
  TEST_ASSERT_EQUAL_INT(2, legs.size());
  TEST_ASSERT_EQUAL_STRING("1:32", legs[1].routeId.c_str());
  TEST_ASSERT_EQUAL_INT(-1, legs[1].directionId);
}

void test_home_and_work_legs_are_independent_keys() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);

  transit::TripLegConfig homeLeg;
  homeLeg.routeId = "1:31";
  homeLeg.boardStopId = "1:100";
  homeLeg.alightStopId = "1:200";
  store.setPresetLegs(transit::ConfigStore::PresetId::kHome, {homeLeg});

  TEST_ASSERT_EQUAL_INT(1, store.presetLegs(transit::ConfigStore::PresetId::kHome).size());
  TEST_ASSERT_TRUE(store.presetLegs(transit::ConfigStore::PresetId::kWork).empty());
}

void test_preset_walk_to_first_stop_min_default_and_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_INT(0, store.presetWalkToFirstStopMin(transit::ConfigStore::PresetId::kHome));
  store.setPresetWalkToFirstStopMin(transit::ConfigStore::PresetId::kHome, 5);
  store.setPresetWalkToFirstStopMin(transit::ConfigStore::PresetId::kWork, 8);
  TEST_ASSERT_EQUAL_INT(5, store.presetWalkToFirstStopMin(transit::ConfigStore::PresetId::kHome));
  TEST_ASSERT_EQUAL_INT(8, store.presetWalkToFirstStopMin(transit::ConfigStore::PresetId::kWork));
}

void test_transfer_buffer_min_default_and_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_EQUAL_INT(3, store.transferBufferMin());
  store.setTransferBufferMin(5);
  TEST_ASSERT_EQUAL_INT(5, store.transferBufferMin());
}

void test_focus_mode_default_and_round_trip() {
  InMemoryConfigBackend backend;
  transit::ConfigStore store(backend);
  TEST_ASSERT_FALSE(store.focusMode());
  store.setFocusMode(true);
  TEST_ASSERT_TRUE(store.focusMode());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_unprovisioned_until_wifi_key_and_stop_are_set);
  RUN_TEST(test_refresh_interval_defaults_to_60);
  RUN_TEST(test_refresh_interval_round_trip);
  RUN_TEST(test_departure_window_min_default_and_round_trip);
  RUN_TEST(test_max_departures_per_direction_default_and_round_trip);
  RUN_TEST(test_sort_by_time_default_and_round_trip);
  RUN_TEST(test_static_direction_default_and_round_trip);
  RUN_TEST(test_time_format_default_and_round_trip);
  RUN_TEST(test_locale_default_and_round_trip);
  RUN_TEST(test_display_portrait_defaults_to_landscape_and_round_trips);
  RUN_TEST(test_sta_stop_code_empty_by_default_and_round_trips);
  RUN_TEST(test_sleep_window_start_and_end_default_and_round_trip);
  RUN_TEST(test_wifi_ssid_and_password_round_trip);
  RUN_TEST(test_api_key_and_stop_id_round_trip);
  RUN_TEST(test_hidden_routes_empty_by_default);
  RUN_TEST(test_hidden_routes_single_entry_round_trip);
  RUN_TEST(test_hidden_routes_multiple_entries_round_trip);
  RUN_TEST(test_hidden_routes_set_empty_clears);
  RUN_TEST(test_route_order_empty_by_default);
  RUN_TEST(test_route_order_single_entry_round_trip);
  RUN_TEST(test_route_order_multiple_entries_round_trip);
  RUN_TEST(test_hidden_routes_and_route_order_are_independent_keys);
  RUN_TEST(test_preset_legs_empty_by_default);
  RUN_TEST(test_preset_legs_round_trip_single_leg);
  RUN_TEST(test_preset_legs_round_trip_multiple_legs_unset_direction);
  RUN_TEST(test_home_and_work_legs_are_independent_keys);
  RUN_TEST(test_preset_walk_to_first_stop_min_default_and_round_trip);
  RUN_TEST(test_transfer_buffer_min_default_and_round_trip);
  RUN_TEST(test_focus_mode_default_and_round_trip);
  RUN_TEST(test_bus_wifi_settings_empty_by_default_and_round_trip);
  RUN_TEST(test_cached_board_empty_by_default_and_round_trips_a_large_blob);
  RUN_TEST(test_every_nvs_key_fits_the_fifteen_character_limit);
  return UNITY_END();
}
