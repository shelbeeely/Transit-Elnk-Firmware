#pragma once

// Transit-Elnk-Firmware — first-run on-device provisioning.
//
// Wi-Fi credentials, Transit API key, and stop pick (via nearby_stops /
// search_stops) are collected through a Wi-Fi AP + captive-portal web GUI
// rather than typed on-device: the X4 has no touch or built-in keyboard, and
// (per explicit product direction) this uses the same provisioning pattern
// as common ESP32 libraries like WiFiManager, and as the Free-Ink sibling
// project wakeink's own "self-hosted captive-portal web dashboard" —
// instead of pairing a BLE HID keyboard.
//
// At a glance: WiFi.mode(WIFI_AP_STA) brings up a fixed-SSID AP
// ("TransitBoard-Setup") alongside the STA interface used to actually join
// the user's network; a DNSServer answers every query with the AP's own IP
// (the standard captive-portal trick that makes phones auto-prompt "Sign in
// to network"); a WebServer serves a single inline-HTML page at "/" plus
// redirects for the common captive-portal probe URLs
// (/generate_204, /hotspot-detect.html, /ncsi.txt, ...). The page walks the
// user through picking/typing a Wi-Fi network + password, entering + one-off
// validating a Transit API key (mirroring root CLAUDE.md step 4's curl
// validation), then searching/picking a stop by approximate lat/lon (no GPS
// on this board) — each confirmed value is written to configStore_
// immediately via small JSON endpoints the page polls/posts to, so a setup
// interrupted partway through resumes at the first still-unset step on the
// next boot rather than restarting from scratch. No HTTPS/auth on the
// setup AP: physical proximity to join the freshly created AP is the access
// control, same as WiFiManager-style portals generally rely on.
//
// Hardware-dependent (WiFi AP/STA, DNSServer, WebServer, display) — only
// buildable under [env:xteink_x4], not [env:native].
//
// Frozen contract for the parallel work units: do not change the
// SetupFlow constructor or runFirstTimeSetup signature. Adding a method is
// fine; note it in your PR description.

#include <DNSServer.h>
#include <WebServer.h>

#include <cstdint>
#include <string>
#include <vector>

#include "transit/api_client.h"
#include "transit/config_store.h"
#include "transit/render_engine.h"

namespace transit {

class SetupFlow {
 public:
  SetupFlow(ConfigStore& configStore, TransitApiClient& apiClient, RenderEngine& renderEngine);

  // Runs the full first-run interview over the captive-portal web GUI:
  // brings up the "TransitBoard-Setup" AP + DNS/web servers, prompts for
  // Wi-Fi SSID/password and connects, prompts for the Transit API key and
  // validates it with a one-off nearby_stops call, then lets the user
  // search/pick a stop via searchStops and store its global_stop_id.
  // Writes every value to configStore as it's confirmed, so a setup
  // interrupted partway through (e.g. low battery, or the user closing the
  // portal page) resumes at the first still-unset step on the next boot
  // rather than restarting from scratch. Returns true once
  // configStore.isProvisioned() would return true.
  bool runFirstTimeSetup();

 private:
  // Connection state for the STA interface while the AP portal is up. The
  // AP interface itself is never touched by this — WIFI_AP_STA keeps the
  // portal reachable throughout, connect attempts/failures only affect STA.
  enum class WifiConnectState { kIdle, kConnecting, kConnected, kFailed };

  void startPortal();
  void stopPortal();
  void pollWifiConnectState();
  void touchActivity();

  // WebServer route handlers (see setup_flow.cpp for the served page/JSON
  // shapes). All hang off `this` via lambdas registered in startPortal().
  void handleRoot();
  void handleScan();
  void handleConnect();
  void handleStatus();
  void handleApiKey();
  void handleStopSearch();
  void handleStopSelect();
  void handleCaptiveRedirect();
  void handleNotFound();

  ConfigStore& configStore_;
  TransitApiClient& apiClient_;
  RenderEngine& renderEngine_;

  DNSServer dnsServer_;
  WebServer server_;

  WifiConnectState wifiConnectState_ = WifiConnectState::kIdle;
  uint32_t wifiConnectStartMs_ = 0;
  std::string pendingSsid_;
  std::string pendingPassword_;

  uint32_t lastActivityMs_ = 0;

  // Cached between POST /stopsearch and POST /stopselect (the page refers
  // back to a search result by index rather than resending the full stop).
  std::vector<SearchStopResult> lastStopResults_;
};

}  // namespace transit
