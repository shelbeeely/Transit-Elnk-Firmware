#pragma once

// Transit-Elnk-Firmware — first-run on-device provisioning.
//
// Wi-Fi credentials, Transit API key, and stop pick (via nearby_stops /
// search_stops) entered on-device and stored to NVS via ConfigStore —
// never compiled into tracked source (root CLAUDE.md). The X4 has no touch
// or built-in keyboard, so text entry pairs a BLE HID keyboard via FreeInk's
// BleKeyboardHost (-DFREEINK_CAP_BLE_HID_HOST=1, already enabled in
// platformio.ini's [base]). Prompts/results are drawn via render_engine's
// renderSetupPrompt/renderSetupList so all on-device text goes through one
// rendering path.
//
// Hardware-dependent (BLE, WiFi, display, network) — only buildable under
// [env:xteink_x4], not [env:native].
//
// Frozen contract for the parallel work units: do not change the
// SetupFlow constructor or runFirstTimeSetup signature. Adding a method is
// fine; note it in your PR description.

#include "transit/api_client.h"
#include "transit/config_store.h"
#include "transit/render_engine.h"

namespace transit {

class SetupFlow {
 public:
  SetupFlow(ConfigStore& configStore, TransitApiClient& apiClient, RenderEngine& renderEngine);

  // Runs the full first-run interview: pair a BLE keyboard, prompt for
  // Wi-Fi SSID/password and connect, prompt for the Transit API key and
  // validate it with a one-off nearby_stops call (mirroring root CLAUDE.md
  // step 4's curl validation), then let the user search/pick a stop via
  // searchStops/nearbyStops and store its global_stop_id. Writes every
  // value to configStore as it's confirmed, so a setup interrupted partway
  // through (e.g. low battery) resumes at the first still-unset step on the
  // next boot rather than restarting from scratch. Returns true once
  // configStore.isProvisioned() would return true.
  bool runFirstTimeSetup();

 private:
  ConfigStore& configStore_;
  TransitApiClient& apiClient_;
  RenderEngine& renderEngine_;
};

}  // namespace transit
