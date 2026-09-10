// Transit-Elnk-Firmware — integration entry point.
//
// Boot -> ConfigStore -> if unprovisioned, run setup_flow -> else Wi-Fi
// connect + SNTP -> api_client fetch -> ui_logic transform -> render_engine
// draw -> power_scheduler sleep.
//
// Work units replace the stub .cpp files under src/transit/ without needing
// to touch this file — that's what keeps their PRs conflict-free with each
// other. If a module's header contract needs a real additive change to wire
// correctly, note it in that unit's PR description rather than editing this
// file silently.

#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <FreeInkUIDisplayTarget.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <time.h>

#include <vector>

#include "transit/api_client.h"
#include "transit/config_store.h"
#include "transit/http_transport.h"
#include "transit/icon_cache.h"
#include "transit/power_scheduler.h"
#include "transit/render_engine.h"
#include "transit/setup_flow.h"
#include "transit/ui_logic.h"

using namespace transit;

namespace {

namespace fui = freeink::ui;

// Real-hardware FramePresenter (render_engine.h): pushes whatever
// RenderEngine just drew into EInkDisplay's framebuffer out to the physical
// panel. The only place in this firmware where RenderEngine's drawing
// touches actual hardware -- see render_engine.h's file comment.
class EInkDisplayPresenter : public FramePresenter {
 public:
  explicit EInkDisplayPresenter(EInkDisplay& display) : display_(display) {}
  void present() override { display_.displayBuffer(EInkDisplay::FULL_REFRESH); }

 private:
  EInkDisplay& display_;
};

// Global objects with trivial constructors only (no NVS/network/display
// hardware touched until begin()/setup(), matching the Free-Ink ecosystem's
// own convention of file-scope subsystem objects wired up inside setup()).
EInkDisplay g_display(-1, -1, -1, -1, -1, -1);
NvsConfigBackend g_configBackend;
ConfigStore g_configStore(g_configBackend);
WifiHttpTransport g_httpTransport;
IconCache g_iconCache(g_httpTransport);

bool connectWifi(const std::string& ssid, const std::string& password) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 20000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// No RTC on the X4 (docs/CONFIG_AND_STATE.md's plan; confirmed via
// BoardConfig — FREEINK_CAP_RTC excludes XteinkX4), so time comes from SNTP
// after Wi-Fi connect on every wake cycle. Returns 0 on sync failure/timeout.
int64_t syncTimeAndGetEpoch() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = 0;
  for (int i = 0; i < 40 && now < 1700000000; ++i) {
    delay(250);
    time(&now);
  }
  return now >= 1700000000 ? static_cast<int64_t>(now) : 0;
}

int minutesSinceLocalMidnight(int64_t nowEpoch) {
  if (nowEpoch <= 0) return 0;
  struct tm timeInfo{};
  time_t nowTimeT = static_cast<time_t>(nowEpoch);
  localtime_r(&nowTimeT, &timeInfo);
  return timeInfo.tm_hour * 60 + timeInfo.tm_min;
}

}  // namespace

void setup() {
  BoardConfig::holdPowerRails();
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);
  freeink::PowerManager::waitForPowerButtonRelease();

  g_display.begin();

  // displayTarget/presenter/renderEngine are local, not global, for the same
  // reason apiClient below is: they read real hardware state
  // (g_display.getFrameBuffer() et al.) at construction, which must happen
  // after g_display.begin() above, not at static-init time. Explicit
  // LandscapeCounterClockwise (native): the X4's panel is landscape-native
  // (800x480), and this is a standing board, not a hand-held reader --
  // DisplayTarget's default orientation heuristic (auto-Portrait for a
  // landscape-native panel, meant for e-readers held tall) would rotate the
  // whole layout 90 degrees.
  fui::DisplayTarget displayTarget(g_display.getFrameBuffer(), g_display.getDisplayWidth(),
                                   g_display.getDisplayHeight(), g_display.getDisplayWidthBytes(),
                                   fui::Orientation::LandscapeCounterClockwise);
  EInkDisplayPresenter presenter(g_display);
  RenderEngine renderEngine(displayTarget, presenter, g_iconCache);

  // TransitApiClient is local, not global: it reads the (possibly still
  // empty, pre-setup) API key at construction, which must happen after
  // board/NVS bring-up above, not at static-init time.
  TransitApiClient apiClient(g_httpTransport, g_configStore.apiKey());

  if (!g_configStore.isProvisioned()) {
    SetupFlow setupFlow(g_configStore, apiClient, renderEngine);
    setupFlow.runFirstTimeSetup();
  }

  if (!g_configStore.isProvisioned()) {
    // Setup was interrupted or skipped — try again next wake rather than
    // spinning forever on battery power.
    enterDeepSleep(g_display, 1);
  }

  BoardStatus status;
  status.stopName = g_configStore.stopId();
  status.batteryPercent = readBatteryPercent();

  bool wifiOk = connectWifi(g_configStore.wifiSsid(), g_configStore.wifiPassword());
  status.wifiOk = wifiOk;

  int64_t nowEpoch = 0;
  std::vector<Route> routes;
  if (wifiOk) {
    nowEpoch = syncTimeAndGetEpoch();

    StopDeparturesParams params;
    params.maxNumDepartures = g_configStore.maxDeparturesPerDirection();
    params.removeCancelled = true;

    StopDeparturesResponse response;
    bool fetchOk =
        apiClient.stopDepartures({g_configStore.stopId()}, params, response);
    status.lastFetchFailed = !fetchOk;
    if (fetchOk) {
      routes = response.routeDepartures;
    }
  } else {
    status.lastFetchFailed = true;
  }
  status.lastUpdatedEpoch = nowEpoch;

  UiSettings uiSettings;
  uiSettings.departureWindowMin = g_configStore.departureWindowMin();
  uiSettings.maxDeparturesPerDirection = g_configStore.maxDeparturesPerDirection();
  uiSettings.sortByTime = g_configStore.sortByTime();
  uiSettings.staticDirection = g_configStore.staticDirection();
  uiSettings.hiddenRoutes = g_configStore.hiddenRoutes();
  uiSettings.routeOrder = g_configStore.routeOrder();

  std::vector<DirectionBoard> board = buildDepartureBoard(routes, uiSettings, nowEpoch);
  renderEngine.renderDepartureBoard(board, status);

  SleepWindow sleepWindow;
  int startMin = g_configStore.sleepWindowStartMin();
  int endMin = g_configStore.sleepWindowEndMin();
  sleepWindow.enabled = startMin >= 0 && endMin >= 0;
  sleepWindow.startMinOfDay = startMin;
  sleepWindow.endMinOfDay = endMin;

  int wakeIntervalMin = computeNextWakeIntervalMin(
      g_configStore.refreshIntervalMin(), sleepWindow, minutesSinceLocalMidnight(nowEpoch));

  enterDeepSleep(g_display, wakeIntervalMin);  // noreturn — chip resets on wake
}

void loop() {
  // Unreachable: setup() always ends in enterDeepSleep().
}
