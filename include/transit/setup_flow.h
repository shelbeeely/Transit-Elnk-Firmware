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

  // Same AP + captive-portal machinery as runFirstTimeSetup(), but serves a
  // settings-only page (display orientation, docs/CONFIG_AND_STATE.md's
  // display_portrait; the optional STA stop code, sta_stop; and optional
  // "Home"/"Work" preset trip chains, trip_planner.h/config_store.h's
  // presetLegs() et al.) instead of the Wi-Fi/API key/stop wizard — so an
  // already-provisioned board can have a setting changed without redoing
  // first-run setup from scratch. main.cpp is responsible for deciding when
  // to call this (a deliberate long-hold of the power button at boot, not a
  // normal wake) and for re-orienting its DrawTarget/RenderEngine
  // afterward if this returns true (see RenderEngine::setScreenSize()).
  // Returns true if a setting was actually changed and saved, false if the
  // portal timed out/was left untouched.
  bool runSettingsPortal();

 private:
  // Connection state for the STA interface while the AP portal is up. The
  // AP interface itself is never touched by this — WIFI_AP_STA keeps the
  // portal reachable throughout, connect attempts/failures only affect STA.
  enum class WifiConnectState { kIdle, kConnecting, kConnected, kFailed };

  // Which page startPortal()'s shared AP/DNS/web-server machinery serves at
  // "/" — set by runFirstTimeSetup()/runSettingsPortal() before startPortal(),
  // read by handleRoot().
  enum class PortalMode { kFirstRun, kSettings };

  void startPortal();
  void stopPortal();
  void pollWifiConnectState();
  void touchActivity();

  // Guard for the first-run wizard's handlers (handleScan/handleConnect/
  // handleApiKey/handleStopSearch/handleStopSelect): startPortal() keeps all
  // routes registered regardless of portalMode_, so each of those must
  // refuse to act while runSettingsPortal() is showing the settings-only
  // page — otherwise a client still holding the first-run page (or one that
  // just guesses the endpoints) could overwrite Wi-Fi credentials, the API
  // key, or the stop pick during what's presented as an orientation-only
  // change. Sends a 403 JSON response and returns false when not in
  // kFirstRun; callers return immediately in that case.
  bool requireFirstRunMode();

  // Mirror image of requireFirstRunMode(), for the settings-only handlers
  // (handleGetOrientation/handleSetOrientation/handleGetStaStop/
  // handleSetStaStop): refuses to act unless portalMode_ is kSettings, so
  // they aren't reachable from the open first-run AP before the board is
  // even provisioned, even though startPortal() keeps their routes
  // registered regardless of mode.
  bool requireSettingsMode();

  // WebServer route handlers (see setup_flow.cpp for the served page/JSON
  // shapes). All hang off `this` via lambdas registered in startPortal().
  void handleRoot();
  void handleScan();
  void handleConnect();
  void handleStatus();
  void handleApiKey();
  void handleStopSearch();
  void handleStopSelect();
  void handleGetOrientation();
  void handleSetOrientation();
  void handleGetStaStop();
  void handleSetStaStop();

  // Preset "Home"/"Work" trip-planning settings (trip_planner.h,
  // ConfigStore::presetLegs() et al.) -- settings-only, requireSettingsMode()
  // -guarded. handleLegDirections() resolves a route short name typed at a
  // given boarding stop into a real globalRouteId plus that route's
  // available directions/headsigns (stop_departures alone can't tell a
  // caller which direction continues toward a given transfer stop -- see
  // trip_planner.h's TripLegConfig::directionId comment), so the settings
  // page can offer a live direction picker rather than guessing.
  // handleGetPresets()/handleSetPresets() read/write the full leg
  // configuration for both presets in one JSON request each (POST body read
  // via server_.arg("plain") -- see setup_flow.cpp for why a JSON body
  // instead of form fields, unlike this file's other handlers).
  void handleLegDirections();
  void handleGetPresets();
  void handleSetPresets();

  // Bus Wi-Fi captive-portal auto-login (captive_portal.h, ConfigStore's
  // busWifiSsid()/busWifiIdentity()/busPortalSubmitUrl()/
  // busPortalFieldName()) -- settings-only, requireSettingsMode()-guarded
  // like the handlers above. Four plain strings with no live validation
  // behind them: unlike handleSetStaStop()'s stop-code lookup or
  // handleApiKey()'s one-off API call, there is nothing to validate an
  // SSID or a portal field name against without being on that network, so
  // these save what they're given. A wrong value surfaces as the board
  // simply not getting online on that network, which is the same outcome
  // as not configuring it at all.
  void handleGetBusWifi();
  void handleSetBusWifi();

  // Explicit "the user reached the end of the settings flow" signal (POST
  // /settingsdone, called by the settings page's JS once it shows
  // step-done) -- see settingsFinished_'s comment on why this exists
  // instead of runSettingsPortal() closing the portal right after the
  // first individual setting is saved.
  void handleSettingsDone();

  void handleCaptiveRedirect();
  void handleNotFound();

  ConfigStore& configStore_;
  TransitApiClient& apiClient_;
  RenderEngine& renderEngine_;

  DNSServer dnsServer_;
  WebServer server_;

  PortalMode portalMode_ = PortalMode::kFirstRun;

  WifiConnectState wifiConnectState_ = WifiConnectState::kIdle;
  uint32_t wifiConnectStartMs_ = 0;
  std::string pendingSsid_;
  std::string pendingPassword_;

  uint32_t lastActivityMs_ = 0;

  // Set by every individual settings handler (orientation/STA/presets) the
  // moment it saves something -- read only for runSettingsPortal()'s return
  // value ("was anything actually changed this session").
  bool settingsSaved_ = false;

  // Set only by handleSettingsDone(), once the settings page's JS reaches
  // its final step-done section -- this, not settingsSaved_, is what
  // actually tells runSettingsPortal()'s loop to show a confirmation and
  // exit. Kept distinct from settingsSaved_ because the settings page is
  // now a multi-step flow (orientation -> STA -> presets -> done): closing
  // the portal 5 seconds after the *first* setting was saved (the original,
  // single-setting-era behavior) would tear down the AP/web server out from
  // under a user who's still filling in a later step. Each step still
  // writes to configStore_ immediately on its own save, same resume-safe
  // philosophy as the rest of this file -- this flag only affects when the
  // portal itself shuts down, never data safety.
  bool settingsFinished_ = false;

  // Cached between POST /stopsearch and POST /stopselect (the page refers
  // back to a search result by index rather than resending the full stop).
  std::vector<SearchStopResult> lastStopResults_;
};

}  // namespace transit
