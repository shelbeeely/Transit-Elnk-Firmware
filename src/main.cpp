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
#include <WiFi.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "transit/api_client.h"
#include "transit/captive_portal.h"
#include "transit/config_store.h"
#include "transit/http_transport.h"
#include "transit/icon_cache.h"
#include "transit/offline_cache.h"
#include "transit/power_scheduler.h"
#include "transit/render_engine.h"
#include "transit/setup_flow.h"
#include "transit/sta_client.h"
#include "transit/time_keeper.h"
#include "transit/trip_planner.h"
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
sta::StaSdStore g_staSdStore;

bool connectWifi(const std::string& ssid, const std::string& password) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 20000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Associates to an open (no-password) network -- the shape onboard transit
// Wi-Fi takes. Association is NOT the same as having internet here: an
// open network that fronts a captive portal reports WL_CONNECTED long
// before anything can actually be fetched through it, which is exactly why
// captive_portal.h exists and why the caller must probe afterward rather
// than trusting this return value. Shorter timeout than connectWifi()
// above: this is a fallback attempt on battery, after the home network has
// already failed and burned its own 20 seconds.
bool connectOpenWifi(const std::string& ssid) {
  WiFi.disconnect(/*wifioff=*/false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str());
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 12000) {
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

// --- Preset "Home"/"Work" trip planning (trip_planner.h) --------------------

void addStopIdIfAbsent(std::vector<std::string>& ids, const std::string& id) {
  if (id.empty()) return;
  if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
}

PresetConfig loadPresetConfig(ConfigStore& configStore, ConfigStore::PresetId id, const std::string& name) {
  PresetConfig preset;
  preset.presetName = name;
  preset.legs = configStore.presetLegs(id);
  preset.walkToFirstStopMin = configStore.presetWalkToFirstStopMin(id);
  preset.transferBufferMin = configStore.transferBufferMin();
  return preset;
}

// 12-hour clock, no leading zero, lowercase am/pm suffix (e.g. "5:42p") --
// matches formatDepartureChip()'s general style (render_engine.cpp) without
// pulling that file's own formatClock() (24-hour, header-clock-specific) in
// here. ASCII only, deliberately: the bundled Noto Sans subset has no glyph
// for non-ASCII punctuation and silently renders a tofu box for anything it
// doesn't have -- confirmed by hand while building render_engine's preset
// strip (see that file's test for the same note re: arrows below).
void formatClockLabel(int64_t epochSeconds, char* buf, size_t bufLen) {
  if (epochSeconds <= 0) {
    snprintf(buf, bufLen, "--:--");
    return;
  }
  time_t t = static_cast<time_t>(epochSeconds);
  struct tm tmVal{};
  localtime_r(&t, &tmVal);
  int hour12 = tmVal.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  snprintf(buf, bufLen, "%d:%02d%s", hour12, tmVal.tm_min, tmVal.tm_hour < 12 ? "a" : "p");
}

// Turns a computed PresetTripPlan into the one line RenderEngine draws for
// it (render_engine.h's BoardStatus::PresetTripSummaryLine). ASCII "->" for
// leg separators, never a Unicode arrow -- see formatClockLabel()'s comment.
BoardStatus::PresetTripSummaryLine formatPresetSummaryLine(const PresetTripPlan& plan, int64_t nowEpoch) {
  BoardStatus::PresetTripSummaryLine line;
  line.presetName = plan.presetName;

  if (!plan.found) {
    line.text = plan.fallbackMessage;
    line.leaveNow = false;
    return line;
  }

  char leaveByClock[8];
  formatClockLabel(plan.leaveByEpoch, leaveByClock, sizeof(leaveByClock));
  std::string text = std::string("leave by ") + leaveByClock;
  for (size_t i = 0; i < plan.legs.size(); ++i) {
    const PlannedLeg& leg = plan.legs[i];
    const std::string& label = !leg.routeShortName.empty() ? leg.routeShortName : leg.routeId;
    if (i == 0) {
      text += " - " + label;
    } else {
      char transferClock[8];
      formatClockLabel(plan.legs[i - 1].alightEpoch, transferClock, sizeof(transferClock));
      text += std::string(" -> transfer ~") + transferClock + " -> " + label;
    }
  }
  line.text = text;
  line.leaveNow = nowEpoch > 0 && (plan.leaveByEpoch - nowEpoch) / 60 <= 5;
  return line;
}

// How long the power button must be held at boot to request the settings
// portal (SetupFlow::runSettingsPortal()) instead of a normal wake cycle.
// Long enough that the ordinary "press to wake" tap never triggers it.
constexpr uint32_t kSettingsHoldMs = 3000;

// Absorbs the button press that woke the board (the same raw-GPIO-poll
// approach as freeink::PowerManager::waitForPowerButtonRelease(), which this
// replaces) while also classifying whether this was a deliberate long-hold
// requesting the settings portal rather than a normal wake tap. Blocks until
// the button is released either way. Reads BoardConfig::ACTIVE.input.power
// directly rather than pulling in InputManager, which brings up touch-panel
// and ADC-attenuation machinery this boot-time check has no use for.
bool waitForBootButtonAndCheckSettingsHold() {
  const int8_t pin = BoardConfig::ACTIVE.input.power;
  if (pin < 0) return false;
  const bool activeHigh = BoardConfig::ACTIVE.input.powerActiveHigh;

  pinMode(pin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  const int pressedLevel = activeHigh ? HIGH : LOW;

  uint32_t pressStartMs = millis();
  while (digitalRead(pin) == pressedLevel) {
    delay(50);
  }
  return millis() - pressStartMs >= kSettingsHoldMs;
}

}  // namespace

void setup() {
  BoardConfig::holdPowerRails();
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);
  bool enterSettingsRequested = waitForBootButtonAndCheckSettingsHold();

  // Must run before g_display.begin(): the X4 shares its SPI bus between
  // the display and the SD card (see StaSdStore's file comment), and
  // SDCardManager::begin() expects to run first so it can deselect a
  // not-yet-initialized display controller that would otherwise drive the
  // shared MISO line. If there's no card or it fails to mount, this costs
  // one mount attempt (repeated every wake -- see StaSdStore::begin()'s own
  // comment on why that isn't cached across deep-sleep cycles) but never
  // blocks boot; STA still works via the flash-baked route/stop tables
  // regardless.
  g_staSdStore.begin();

  g_display.begin();

  // displayTarget/presenter/renderEngine are local, not global, for the same
  // reason apiClient below is: they read real hardware state
  // (g_display.getFrameBuffer() et al.) at construction, which must happen
  // after g_display.begin() above, not at static-init time. Explicit
  // orientation, not DisplayTarget's default heuristic (auto-Portrait for a
  // landscape-native panel, meant for e-readers held tall) -- this is a
  // standing board, and ConfigStore::displayPortrait() (set via
  // SetupFlow::runSettingsPortal()) decides which way it stands.
  // LandscapeCounterClockwise is the X4 panel's native orientation
  // (800x480); Portrait rotates it 90 degrees (480x800).
  fui::DisplayTarget displayTarget(g_display.getFrameBuffer(), g_display.getDisplayWidth(),
                                   g_display.getDisplayHeight(), g_display.getDisplayWidthBytes(),
                                   g_configStore.displayPortrait() ? fui::Orientation::Portrait
                                                                    : fui::Orientation::LandscapeCounterClockwise);
  EInkDisplayPresenter presenter(g_display);
  RenderEngine renderEngine(displayTarget, presenter, g_iconCache, displayTarget.logicalWidth(),
                            displayTarget.logicalHeight());

  // TransitApiClient is local, not global: it reads the (possibly still
  // empty, pre-setup) API key at construction, which must happen after
  // board/NVS bring-up above, not at static-init time.
  TransitApiClient apiClient(g_httpTransport, g_configStore.apiKey());

  if (!g_configStore.isProvisioned()) {
    SetupFlow setupFlow(g_configStore, apiClient, renderEngine);
    setupFlow.runFirstTimeSetup();
  } else if (enterSettingsRequested) {
    // A deliberate long-hold of the power button at boot on an
    // already-provisioned board, not a normal wake -- offer the settings
    // portal instead of fetching/rendering the departure board this cycle.
    SetupFlow setupFlow(g_configStore, apiClient, renderEngine);
    if (setupFlow.runSettingsPortal()) {
      // Orientation may have changed -- re-orient the same DisplayTarget and
      // tell renderEngine its new logical dimensions before anything below
      // draws with it.
      displayTarget.setOrientation(g_configStore.displayPortrait() ? fui::Orientation::Portrait
                                                                    : fui::Orientation::LandscapeCounterClockwise);
      renderEngine.setScreenSize(displayTarget.logicalWidth(), displayTarget.logicalHeight());
    }
  }

  if (!g_configStore.isProvisioned()) {
    // Setup was interrupted or skipped — try again next wake rather than
    // spinning forever on battery power.
    enterDeepSleep(g_display, 1);
  }

  BoardStatus status;
  status.stopName = g_configStore.stopId();
  status.batteryPercent = readBatteryPercent();

  // Whatever the previous wake left in RTC memory (time_keeper.h). Read
  // before anything else touches the clock, since a successful SNTP sync
  // below overwrites the system time this is the only alternative to.
  const ApproxClockState priorClock = loadApproxClock();

  bool wifiOk = connectWifi(g_configStore.wifiSsid(), g_configStore.wifiPassword());

  // Home network isn't in range -- try the configured open network (onboard
  // transit Wi-Fi) and, if something is intercepting it, hand the portal
  // the saved email/phone. Only attempted when the user has actually
  // configured an SSID: the board never joins an open network on its own.
  if (!wifiOk) {
    const std::string busSsid = g_configStore.busWifiSsid();
    if (!busSsid.empty() && connectOpenWifi(busSsid)) {
      CaptivePortalConfig portalConfig;
      portalConfig.identity = g_configStore.busWifiIdentity();
      portalConfig.overrideSubmitUrl = g_configStore.busPortalSubmitUrl();
      portalConfig.overrideFieldName = g_configStore.busPortalFieldName();

      CaptivePortalClient portalClient(g_httpTransport);
      const CaptivePortalResult portalResult = portalClient.connect(portalConfig);
      wifiOk = portalResult.online;
      if (!wifiOk) {
        // Associated but still walled off. Drop the association rather than
        // leaving the radio camped on a network nothing can be fetched
        // through -- every subsequent request would just burn battery
        // collecting splash pages.
        WiFi.disconnect(/*wifioff=*/false);
      }
    }
  }
  status.wifiOk = wifiOk;

  int64_t nowEpoch = 0;
  bool clockFromSntp = false;
  std::vector<Route> routes;
  std::vector<PresetTripPlan> presetPlans;
  bool anyFetchOk = false;
  if (wifiOk) {
    nowEpoch = syncTimeAndGetEpoch();
    clockFromSntp = nowEpoch > 0;

    // Preset "Home"/"Work" trip planning (trip_planner.h) rides along on
    // this SAME stopDepartures() call rather than issuing its own -- every
    // leg's boarding/alighting stop_id is folded into the one batched
    // request (stopDepartures accepts up to 100), so configuring presets
    // costs zero *additional* Transit API calls per wake, only a bigger
    // response body. See docs/TRIP_PLANNER.md's call-budget note.
    PresetConfig homePreset = loadPresetConfig(g_configStore, ConfigStore::PresetId::kHome, "Home");
    PresetConfig workPreset = loadPresetConfig(g_configStore, ConfigStore::PresetId::kWork, "Work");
    const bool anyPresetConfigured = !homePreset.legs.empty() || !workPreset.legs.empty();

    std::vector<std::string> stopIds;
    addStopIdIfAbsent(stopIds, g_configStore.stopId());
    for (const PresetConfig* preset : {&homePreset, &workPreset}) {
      for (const TripLegConfig& leg : preset->legs) {
        addStopIdIfAbsent(stopIds, leg.boardStopId);
        addStopIdIfAbsent(stopIds, leg.alightStopId);
      }
    }

    StopDeparturesParams params;
    // Trip planning needs to see further into each leg stop's schedule than
    // the main board's own display cap -- this only grows the response
    // size, not the call count, so it's free against the free-tier budget
    // (see docs/TRIP_PLANNER.md).
    params.maxNumDepartures =
        anyPresetConfigured ? std::max(g_configStore.maxDeparturesPerDirection(), 8)
                            : g_configStore.maxDeparturesPerDirection();
    params.removeCancelled = true;

    StopDeparturesResponse response;
    bool fetchOk = apiClient.stopDepartures(stopIds, params, response);
    status.lastFetchFailed = !fetchOk;
    anyFetchOk = anyFetchOk || fetchOk;
    if (fetchOk) {
      // ui_logic::buildDepartureBoard() assumes it's handed exactly one
      // stop's routes -- filter the combined multi-stop response down to
      // the main stop before handing it off, or preset-leg-only stops'
      // routes would leak onto the main departure board.
      for (const Route& route : response.routeDepartures) {
        if (route.globalStopId == g_configStore.stopId()) {
          routes.push_back(route);
        }
      }

      // planPresetTrip() gets the FULL unfiltered response across every
      // queried stop -- main-board display prefs (hiddenRoutes/routeOrder/
      // departureWindowMin) are irrelevant to, and must not silently break,
      // a preset the user explicitly configured.
      if (!homePreset.legs.empty()) {
        presetPlans.push_back(planPresetTrip(response.routeDepartures, homePreset, nowEpoch));
      }
      if (!workPreset.legs.empty()) {
        presetPlans.push_back(planPresetTrip(response.routeDepartures, workPreset, nowEpoch));
      }
    }

    // STA is a second, optional data source (docs/CONFIG_AND_STATE.md's
    // sta_stop) shown alongside Transit's own departures, not merged into
    // them -- see sta_models.h's staDeparturesToRoutes(). Independent of
    // the Transit fetch above: attempted whenever Wi-Fi is up regardless of
    // whether that fetch succeeded, and a failure here (network, a stop
    // code sta_stop_table.h doesn't recognize, or too little free heap for
    // the ~190KB feed -- see sta_client.h) never affects status/routes
    // above, since the board still has Transit's departures either way.
    std::string staStopCode = g_configStore.staStopCode();
    if (!staStopCode.empty()) {
      sta::StaClient staClient(g_httpTransport, &g_staSdStore);
      std::vector<Route> staRoutes = staClient.fetchDepartures(staStopCode);
      if (!staRoutes.empty()) anyFetchOk = true;
      routes.insert(routes.end(), staRoutes.begin(), staRoutes.end());
    }
  } else {
    status.lastFetchFailed = true;
  }

  // No network (or no usable answer from it), but the RTC-memory clock may
  // still know roughly what time it is -- which is the difference between
  // a board that can count down to a cached departure and one that can only
  // print bare clock times. time_keeper.h refuses to vouch for the estimate
  // once its accumulated error bound gets too wide, and that refusal is
  // honored here rather than second-guessed.
  if (nowEpoch <= 0 && approximateClockUsable(priorClock, millis())) {
    nowEpoch = estimateNowEpoch(priorClock, millis());
    if (nowEpoch > 0) {
      status.clockIsApproximate = true;
      // Push it into the system clock too: formatClock()/localtime_r() in
      // render_engine.cpp and minutesSinceLocalMidnight() below both read
      // the C library's notion of time, not this variable, so an estimate
      // that isn't installed here would leave them stuck at the epoch.
      struct timeval tv {};
      tv.tv_sec = static_cast<time_t>(nowEpoch);
      tv.tv_usec = 0;
      settimeofday(&tv, nullptr);
    }
  }

  // Everything from here on takes real time (rendering, an SD mount, a
  // 190KB STA feed parse), so nextClockState() below needs to know how much
  // of it has elapsed -- see its comment on why storing the epoch as of
  // *this* moment would quietly lose that gap on every single wake.
  const uint32_t nowEpochEstablishedMs = millis();

  UiSettings uiSettings;
  uiSettings.departureWindowMin = g_configStore.departureWindowMin();
  uiSettings.maxDeparturesPerDirection = g_configStore.maxDeparturesPerDirection();
  uiSettings.sortByTime = g_configStore.sortByTime();
  uiSettings.staticDirection = g_configStore.staticDirection();
  uiSettings.hiddenRoutes = g_configStore.hiddenRoutes();
  uiSettings.routeOrder = g_configStore.routeOrder();

  std::vector<DirectionBoard> board;
  if (anyFetchOk) {
    board = buildDepartureBoard(routes, uiSettings, nowEpoch);

    // Persist what was just computed, so the next wake has something to
    // draw if it comes up with no network (offline_cache.h). The already-
    // built board is cached rather than the raw API response because the
    // response runs to tens of kilobytes and an NVS string value tops out
    // near 4000 -- see that header for the trade that implies.
    CachedBoard toCache;
    toCache.fetchedAtEpoch = nowEpoch;
    toCache.board = board;
    toCache.presetPlans = presetPlans;
    const std::string blob = serializeCachedBoard(toCache);
    if (!blob.empty()) g_configStore.setCachedBoard(blob);
  } else {
    // Nothing fetched this wake. Fall back to the last board that was, and
    // say so in the header rather than drawing an empty screen -- a bus
    // that left 10 minutes ago is still better information than nothing,
    // as long as the reader can tell it's old.
    CachedBoard restored;
    if (deserializeCachedBoard(g_configStore.cachedBoard(), restored)) {
      // Set before the prune, and set even when the prune empties the
      // cache out: "the cache aged out completely" and "there has never
      // been a cache" call for different things on screen, and the render
      // engine can only tell them apart from this flag.
      status.dataIsCached = true;
      // -1, not 0, when there's no clock: without one the age is genuinely
      // unknown, and reporting 0 would label a board of unknown vintage
      // "Cached just now". See BoardStatus::cachedAgeMin.
      status.cachedAgeMin = nowEpoch > 0 ? cachedAgeMinutes(restored, nowEpoch) : -1;

      // Absolute departure epochs age on their own: anything already gone
      // is dropped here rather than rendered as "Due" forever
      // (formatDepartureChip() clamps negative minutes to 0). A cache left
      // offline long enough empties itself out instead of lying.
      //
      // Without a clock this prune is a no-op (offline_cache.h: no clock
      // means no basis for calling anything expired), which is exactly why
      // the age above has to be reported as unknown -- the rows could be
      // from yesterday and nothing here can tell.
      pruneExpiredDepartures(restored, nowEpoch);
      board = restored.board;
      presetPlans = restored.presetPlans;
    }
  }

  // Formatted last, from whichever source won above, so a restored plan's
  // "leave now" urgency is recomputed against the current clock instead of
  // being frozen at whatever it was when the plan was cached.
  std::vector<BoardStatus::PresetTripSummaryLine> presetSummaryLines;
  for (const PresetTripPlan& plan : presetPlans) {
    presetSummaryLines.push_back(formatPresetSummaryLine(plan, nowEpoch));
  }
  status.lastUpdatedEpoch = nowEpoch;
  status.presetTrips = presetSummaryLines;

  renderEngine.setFocusMode(g_configStore.focusMode());
  renderEngine.renderDepartureBoard(board, status);

  SleepWindow sleepWindow;
  int startMin = g_configStore.sleepWindowStartMin();
  int endMin = g_configStore.sleepWindowEndMin();
  sleepWindow.enabled = startMin >= 0 && endMin >= 0;
  sleepWindow.startMinOfDay = startMin;
  sleepWindow.endMinOfDay = endMin;

  int wakeIntervalMin = computeNextWakeIntervalMin(
      g_configStore.refreshIntervalMin(), sleepWindow, minutesSinceLocalMidnight(nowEpoch));

  // Hand the next wake a clock (time_keeper.h). Must happen after
  // wakeIntervalMin is known -- the estimate on the other side of the sleep
  // is "the time at sleep entry, plus however long the timer was armed
  // for" -- and before enterDeepSleep(), which never returns. nowEpoch is
  // carried forward to *this* instant rather than stored as of when it was
  // established, so the seconds spent fetching and rendering aren't
  // silently dropped once per wake.
  const int64_t epochAtSleepEntry =
      nowEpoch > 0 ? nowEpoch + static_cast<int64_t>((millis() - nowEpochEstablishedMs) / 1000) : 0;
  saveApproxClock(nextClockState(priorClock, epochAtSleepEntry, wakeIntervalMin, clockFromSntp));

  enterDeepSleep(g_display, wakeIntervalMin);  // noreturn — chip resets on wake
}

void loop() {
  // Unreachable: setup() always ends in enterDeepSleep().
}
