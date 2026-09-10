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

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_unprovisioned_until_wifi_key_and_stop_are_set);
  RUN_TEST(test_refresh_interval_defaults_to_60);
  return UNITY_END();
}
