// NvsConfigBackend: Arduino Preferences (NVS) backend. [env:xteink_x4] only
// — excluded from [env:native]'s build (see platformio.ini), since
// Preferences.h isn't available there.
//
// Hardware-only: not exercised by [env:native] tests (see
// test/test_config_store/test_main.cpp, which tests ConfigStore's logic
// against an in-memory ConfigBackend test double instead).

#include "transit/config_store.h"

#include <Preferences.h>

namespace transit {

namespace {
constexpr const char* kNamespace = "transit";
}

std::string NvsConfigBackend::getString(const char* key, const std::string& defaultValue) {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  std::string value = prefs.getString(key, defaultValue.c_str()).c_str();
  prefs.end();
  return value;
}

void NvsConfigBackend::setString(const char* key, const std::string& value) {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.putString(key, value.c_str());
  prefs.end();
}

int32_t NvsConfigBackend::getInt(const char* key, int32_t defaultValue) {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  int32_t value = prefs.getInt(key, defaultValue);
  prefs.end();
  return value;
}

void NvsConfigBackend::setInt(const char* key, int32_t value) {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.putInt(key, value);
  prefs.end();
}

bool NvsConfigBackend::getBool(const char* key, bool defaultValue) {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  bool value = prefs.getBool(key, defaultValue);
  prefs.end();
  return value;
}

void NvsConfigBackend::setBool(const char* key, bool value) {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.putBool(key, value);
  prefs.end();
}

bool NvsConfigBackend::isSet(const char* key) {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  bool set = prefs.isKey(key);
  prefs.end();
  return set;
}

}  // namespace transit
