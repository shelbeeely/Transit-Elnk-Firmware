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

 private:
  std::map<std::string, std::string> strings_;
  std::map<std::string, int32_t> ints_;
  std::map<std::string, bool> bools_;
  std::set<std::string> setKeys_;
};

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
  return UNITY_END();
}
