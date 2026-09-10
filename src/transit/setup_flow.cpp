// First-run on-device provisioning (see include/transit/setup_flow.h).
//
// Text entry has no touch/keyboard on the X4, so every prompt is answered
// through a paired BLE HID keyboard (FreeInk's BleKeyboardHost): printable
// keys build up a line buffer, Backspace edits it, Enter confirms, Escape
// backs out of the current step. Wi-Fi network / stop-search results are
// picked from a list with Up/Down + Enter. All screens go through
// RenderEngine::renderSetupPrompt/renderSetupList (unit 5) so this file
// never touches EInkDisplay directly.
//
// Each step writes to configStore_ the moment it's confirmed, and every
// step first checks whether configStore_ already has a value (i.e. this is
// a resume after an interrupted setup) before prompting again — see the
// header comment on runFirstTimeSetup for why.

#include "transit/setup_flow.h"

#include <Arduino.h>
#include <BleKeyboardHost.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace transit {

namespace {

// --- Small parsing helper (no exceptions: strtod, not std::stod) -----------

bool parseDouble(const std::string& text, double& out) {
  if (text.empty()) return false;
  const char* start = text.c_str();
  char* end = nullptr;
  double value = strtod(start, &end);
  if (end == start || *end != '\0') return false;
  out = value;
  return true;
}

// "lat,lon" (optional surrounding whitespace on either half).
bool parseLatLon(const std::string& text, double& lat, double& lon) {
  size_t comma = text.find(',');
  if (comma == std::string::npos) return false;
  std::string latPart = text.substr(0, comma);
  std::string lonPart = text.substr(comma + 1);
  auto trim = [](std::string& s) {
    size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) {
      s.clear();
      return;
    }
    size_t last = s.find_last_not_of(" \t");
    s = s.substr(begin, last - begin + 1);
  };
  trim(latPart);
  trim(lonPart);
  double parsedLat = 0.0;
  double parsedLon = 0.0;
  if (!parseDouble(latPart, parsedLat) || !parseDouble(lonPart, parsedLon)) return false;
  if (parsedLat < -90.0 || parsedLat > 90.0 || parsedLon < -180.0 || parsedLon > 180.0) {
    return false;
  }
  lat = parsedLat;
  lon = parsedLon;
  return true;
}

// --- BLE keyboard pairing ---------------------------------------------------

// Generous but bounded: a device left mid-setup with no keyboard in range
// must not spin forever on battery. main.cpp deep-sleeps 1 minute and
// retries setup on the next boot when runFirstTimeSetup() returns with the
// device still unprovisioned, so giving up here just means "try again soon"
// rather than "stuck".
constexpr uint32_t kPairingTimeoutMs = 5UL * 60UL * 1000UL;
// Bonded keyboards auto-reconnect via BleKeyboardHost::poll() on its own
// backoff; give that a moment before we additionally kick off a scan.
constexpr uint32_t kAutoReconnectGraceMs = 4000;

bool pairBleKeyboard(RenderEngine& renderEngine) {
  if (!BleHid.isRunning()) {
    renderEngine.renderSetupPrompt("Bluetooth", "Starting Bluetooth...");
    if (!BleHid.begin("Transit Board")) {
      renderEngine.renderSetupPrompt("Setup paused",
                                      "Bluetooth failed to start.\nWill retry on next wake.");
      return false;
    }
  }
  if (BleHid.isConnected()) return true;

  bool havePairedDevices = BleHid.pairedCount() > 0;
  renderEngine.renderSetupPrompt("Pair a Bluetooth keyboard",
                                  havePairedDevices
                                      ? "Reconnecting to your keyboard..."
                                      : "Put your keyboard in pairing\nmode. Scanning...");

  uint32_t startMs = millis();
  bool scanRequested = false;
  while (!BleHid.isConnected()) {
    BleHid.poll();

    uint32_t elapsedMs = millis() - startMs;
    if (!scanRequested && !BleHid.isScanning() && !BleHid.isConnecting() &&
        (!havePairedDevices || elapsedMs > kAutoReconnectGraceMs)) {
      BleHid.startScan(6000);
      scanRequested = true;
    } else if (scanRequested && !BleHid.isScanning() && !BleHid.isConnecting()) {
      int foundIdx = -1;
      for (uint8_t i = 0; i < BleHid.deviceCount(); ++i) {
        if (BleHid.device(i).hid) {
          foundIdx = static_cast<int>(i);
          break;
        }
      }
      if (foundIdx >= 0) {
        std::string addr(BleHid.device(static_cast<uint8_t>(foundIdx)).addr);
        BleHid.releaseScanResults();
        BleHid.connect(addr.c_str());
      }
      // No HID device found this pass (or connect() above is now pending) —
      // either way, fall through and let the timeout/loop decide whether to
      // scan again.
      scanRequested = false;
    }

    if (millis() - startMs > kPairingTimeoutMs) {
      renderEngine.renderSetupPrompt("Setup paused", "No keyboard found.\nWill retry on next wake.");
      return false;
    }
    delay(50);
  }

  renderEngine.renderSetupPrompt("Keyboard connected", BleHid.connectedName());
  delay(1000);
  return true;
}

// --- Line editing via the paired keyboard -----------------------------------

constexpr size_t kMaxLineLen = 128;
// Guards against a setup screen left unattended forever (e.g. the user
// walked away mid-entry) draining the battery instead of resuming later.
constexpr uint32_t kLineIdleTimeoutMs = 10UL * 60UL * 1000UL;

// Reads one line of text via the paired BLE keyboard, redrawing `title` +
// `promptBody` (plus the in-progress text) after every keystroke. `mask`
// draws asterisks instead of the actual characters for sensitive fields
// (Wi-Fi password, API key) — never the literal text — matching root
// CLAUDE.md's "never print a full API key" guardrail on-device too.
// Sets `cancelled` (Escape, or the idle timeout) and returns whatever was
// typed so far; callers treat a cancelled read as "abort this step".
std::string readLine(RenderEngine& renderEngine, const std::string& title,
                      const std::string& promptBody, bool mask, bool& cancelled) {
  cancelled = false;
  std::string text;

  auto render = [&]() {
    std::string shown = mask ? std::string(text.size(), '*') : text;
    renderEngine.renderSetupPrompt(title, promptBody + "\n\n> " + shown + "_");
  };
  render();

  uint32_t lastActivityMs = millis();
  while (true) {
    BleHid.poll();
    freeink::KeyEvent ev;
    if (!BleHid.popKey(ev)) {
      if (millis() - lastActivityMs > kLineIdleTimeoutMs) {
        cancelled = true;
        return text;
      }
      delay(20);
      continue;
    }
    lastActivityMs = millis();

    if (ev.special == freeink::SpecialKey::Enter) {
      break;
    } else if (ev.special == freeink::SpecialKey::Escape) {
      cancelled = true;
      break;
    } else if (ev.special == freeink::SpecialKey::Backspace) {
      if (!text.empty()) {
        text.pop_back();
        render();
      }
    } else if (ev.ch != 0 && text.size() < kMaxLineLen) {
      text.push_back(ev.ch);
      render();
    }
  }
  return text;
}

// --- List picking via the paired keyboard -----------------------------------

constexpr uint32_t kListIdleTimeoutMs = 10UL * 60UL * 1000UL;

// Returns the chosen index, or -1 on Escape/idle-timeout/empty list.
int selectFromList(RenderEngine& renderEngine, const std::string& title,
                    const std::vector<std::string>& items) {
  if (items.empty()) return -1;

  int selected = 0;
  renderEngine.renderSetupList(title, items, selected);

  uint32_t lastActivityMs = millis();
  while (true) {
    BleHid.poll();
    freeink::KeyEvent ev;
    if (!BleHid.popKey(ev)) {
      if (millis() - lastActivityMs > kListIdleTimeoutMs) return -1;
      delay(20);
      continue;
    }
    lastActivityMs = millis();

    if (ev.special == freeink::SpecialKey::Down) {
      selected = (selected + 1) % static_cast<int>(items.size());
      renderEngine.renderSetupList(title, items, selected);
    } else if (ev.special == freeink::SpecialKey::Up) {
      selected = (selected - 1 + static_cast<int>(items.size())) % static_cast<int>(items.size());
      renderEngine.renderSetupList(title, items, selected);
    } else if (ev.special == freeink::SpecialKey::Enter) {
      return selected;
    } else if (ev.special == freeink::SpecialKey::Escape) {
      return -1;
    }
  }
}

// --- Wi-Fi ------------------------------------------------------------------

bool tryConnectWifi(const std::string& ssid, const std::string& password) {
  if (ssid.empty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.empty() ? nullptr : password.c_str());
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 20000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Scans for nearby networks and lets the user pick one (or type an SSID by
// hand, for hidden networks / if the scan comes back empty).
bool scanAndPickSsid(RenderEngine& renderEngine, std::string& ssidOut) {
  renderEngine.renderSetupPrompt("Wi-Fi", "Scanning for networks...");
  WiFi.mode(WIFI_STA);
  int found = WiFi.scanNetworks();

  std::vector<std::string> ssids;
  std::vector<std::string> items;
  if (found > 0) {
    for (int i = 0; i < found; ++i) {
      std::string ssid(WiFi.SSID(i).c_str());
      if (ssid.empty()) continue;
      if (std::find(ssids.begin(), ssids.end(), ssid) != ssids.end()) continue;  // dedupe APs
      ssids.push_back(ssid);
      items.push_back(ssid);
    }
  }
  WiFi.scanDelete();
  items.push_back("[Enter network name manually]");

  int idx = selectFromList(renderEngine, "Select your Wi-Fi network", items);
  if (idx < 0) return false;  // cancelled

  if (idx == static_cast<int>(items.size()) - 1) {
    bool cancelled = false;
    std::string manual =
        readLine(renderEngine, "Wi-Fi network name", "Type the network name (SSID):", false, cancelled);
    if (cancelled || manual.empty()) return false;
    ssidOut = manual;
  } else {
    ssidOut = ssids[static_cast<size_t>(idx)];
  }
  return true;
}

// Interactive pick-network -> enter-password -> connect -> save loop. Only
// entered when there's no usable saved Wi-Fi (see ensureWifiConnected).
bool connectAndSaveWifi(RenderEngine& renderEngine, ConfigStore& configStore) {
  while (true) {
    std::string ssid;
    if (!scanAndPickSsid(renderEngine, ssid)) return false;

    bool cancelled = false;
    std::string password = readLine(renderEngine, "Wi-Fi password",
                                     "Enter the password for \"" + ssid +
                                         "\",\nthen press Enter (leave blank\nif this network is open).",
                                     /*mask=*/true, cancelled);
    if (cancelled) return false;

    renderEngine.renderSetupPrompt("Wi-Fi", "Connecting to \"" + ssid + "\"...");
    if (tryConnectWifi(ssid, password)) {
      configStore.setWifiSsid(ssid);
      configStore.setWifiPassword(password);
      renderEngine.renderSetupPrompt("Wi-Fi connected", "Connected to \"" + ssid + "\".");
      delay(1000);
      return true;
    }

    renderEngine.renderSetupPrompt("Wi-Fi",
                                    "Couldn't connect to \"" + ssid + "\".\nCheck the password and try again.");
    delay(2000);
  }
}

// Wi-Fi credentials may already be stored (a resume after an interrupted
// setup, or after a later re-run — this device has no "settings" UI beyond
// first-run setup). Try them silently first so a returning user isn't
// re-interviewed for a network that still works; fall back to the
// interactive picker only if that fails or nothing was stored yet.
bool ensureWifiConnected(RenderEngine& renderEngine, ConfigStore& configStore) {
  std::string savedSsid = configStore.wifiSsid();
  if (!savedSsid.empty()) {
    if (tryConnectWifi(savedSsid, configStore.wifiPassword())) return true;
    renderEngine.renderSetupPrompt("Wi-Fi", "Couldn't reconnect to the saved\nnetwork. Let's pick again.");
    delay(1500);
  }
  return connectAndSaveWifi(renderEngine, configStore);
}

// --- Transit API key ---------------------------------------------------------

// Fixed placeholder coordinate purely to exercise the API for key
// validation — the device has no GPS. Same point root CLAUDE.md's own curl
// validation example uses (step 4), not a real location.
constexpr double kKeyCheckLat = 45.5017;
constexpr double kKeyCheckLon = -73.5673;

bool promptAndValidateApiKey(RenderEngine& renderEngine, TransitApiClient& apiClient,
                              ConfigStore& configStore) {
  while (true) {
    bool cancelled = false;
    std::string key = readLine(renderEngine, "Transit API key",
                                "Paste your Transit API key,\nthen press Enter.", /*mask=*/true, cancelled);
    if (cancelled) return false;
    if (key.empty()) {
      renderEngine.renderSetupPrompt("Transit API key", "The key can't be empty. Try again.");
      delay(1500);
      continue;
    }

    renderEngine.renderSetupPrompt("Transit API key", "Checking key...");
    apiClient.setApiKey(key);

    NearbyStopsParams params;
    params.maxDistanceMeters = 500;
    NearbyStopsResponse response;
    bool ok = apiClient.nearbyStops(kKeyCheckLat, kKeyCheckLon, params, response);
    if (ok) {
      configStore.setApiKey(key);
      // Never show the full key on-device either (root CLAUDE.md's "truncate
      // to the last few characters" guardrail) — a short key just shows as
      // fully masked rather than falling back to printing it whole.
      std::string suffix = key.size() > 4 ? ("..." + key.substr(key.size() - 4)) : "(hidden)";
      renderEngine.renderSetupPrompt("Transit API key", "Key accepted, ending in " + suffix + ".");
      delay(1200);
      return true;
    }

    renderEngine.renderSetupPrompt(
        "Transit API key",
        "Key rejected, or offline.\nA freshly issued key can take a\nmoment to propagate. Try again.");
    delay(2000);
  }
}

// --- Stop pick ---------------------------------------------------------------

// No GPS on the X4 (docs/CONFIG_AND_STATE.md), so the user supplies an
// approximate search-area center by hand rather than relying on a live
// radius search every poll. Coordinates are looked up once off-device (any
// maps app) and typed in — impractical to replace with a typed
// address/city without pulling in a geocoder, so this is left to
// search_stops' own text-query matching within that area (max_distance is
// left unset — see SearchStopsParams' default — so the query text and the
// picker do the narrowing, not a client-side radius guess).
bool pickStop(RenderEngine& renderEngine, TransitApiClient& apiClient, ConfigStore& configStore) {
  while (true) {
    bool cancelled = false;
    std::string coordText =
        readLine(renderEngine, "Stop search area",
                 "Enter approximate coordinates as\nlat,lon (e.g. 45.50,-73.57)\nfrom a maps app on your phone.",
                 /*mask=*/false, cancelled);
    if (cancelled) return false;

    double lat = 0.0;
    double lon = 0.0;
    if (!parseLatLon(coordText, lat, lon)) {
      renderEngine.renderSetupPrompt("Stop search area",
                                      "Couldn't read that. Format:\nlat,lon (e.g. 45.50,-73.57)");
      delay(1800);
      continue;
    }

    std::string query = readLine(renderEngine, "Stop name",
                                  "Type part of the stop name,\nthen press Enter (e.g. \"Main St\").",
                                  /*mask=*/false, cancelled);
    if (cancelled) return false;

    renderEngine.renderSetupPrompt("Searching", "Looking up stops...");
    SearchStopsParams params;
    params.maxNumResults = 20;
    SearchStopsResponse response;
    bool ok = apiClient.searchStops(lat, lon, query, params, response);
    if (!ok || response.results.empty()) {
      renderEngine.renderSetupPrompt(
          "No stops found", "No matching stops near that area.\nCheck the coordinates/spelling\nand try again.");
      delay(2000);
      continue;
    }

    std::vector<std::string> items;
    items.reserve(response.results.size());
    for (const auto& result : response.results) {
      std::string label = result.stopName;
      if (result.distanceMeters > 0) {
        label += " (" + std::to_string(static_cast<int>(result.distanceMeters)) + "m)";
      }
      items.push_back(label);
    }

    int idx = selectFromList(renderEngine, "Select your stop", items);
    if (idx < 0) continue;  // back to search rather than aborting setup

    const SearchStopResult& chosen = response.results[static_cast<size_t>(idx)];
    configStore.setStopId(chosen.globalStopId);
    renderEngine.renderSetupPrompt("Stop saved", chosen.stopName);
    delay(1200);
    return true;
  }
}

}  // namespace

SetupFlow::SetupFlow(ConfigStore& configStore, TransitApiClient& apiClient, RenderEngine& renderEngine)
    : configStore_(configStore), apiClient_(apiClient), renderEngine_(renderEngine) {}

bool SetupFlow::runFirstTimeSetup() {
  if (configStore_.isProvisioned()) return true;

  renderEngine_.renderSetupPrompt("Setup", "Let's get your board set up.");
  delay(1000);

  if (!pairBleKeyboard(renderEngine_)) return configStore_.isProvisioned();

  if (!ensureWifiConnected(renderEngine_, configStore_)) return configStore_.isProvisioned();

  if (configStore_.apiKey().empty()) {
    if (!promptAndValidateApiKey(renderEngine_, apiClient_, configStore_)) {
      return configStore_.isProvisioned();
    }
  } else {
    // Resuming with a key already saved from a prior attempt — reuse it
    // rather than asking again; main.cpp constructed apiClient_ with
    // whatever configStore_.apiKey() held at boot, which is this value.
    apiClient_.setApiKey(configStore_.apiKey());
  }

  if (configStore_.stopId().empty()) {
    if (!pickStop(renderEngine_, apiClient_, configStore_)) return configStore_.isProvisioned();
  }

  renderEngine_.renderSetupPrompt("Setup complete", "Your board is ready.");
  delay(1200);
  return configStore_.isProvisioned();
}

}  // namespace transit
