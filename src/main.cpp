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
#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <FreeInkUIDisplayTarget.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "transit/api_client.h"
#include "transit/boot_report.h"
#include "transit/build_info.h"
#include "transit/captive_portal.h"
#include "transit/config_store.h"
#include "transit/http_transport.h"
#include "transit/icon_cache.h"
#include "transit/local_time.h"
#include "transit/offline_cache.h"
#include "transit/ota_update.h"
#include "transit/power_scheduler.h"
#include "transit/render_engine.h"
#include "transit/setup_flow.h"
#include "transit/sta_client.h"
#include "transit/sta_models.h"
#include "transit/sta_static_schedule.h"
#include "transit/sta_stop_table.h"
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
//
// configTzTime(), not configTime(): the Arduino-ESP32 core implements
// configTime(0, 0, ...) as setenv("TZ", "UTC0") + tzset(), which would
// silently undo applyTimezone() on every single Wi-Fi wake and leave the
// clock, the sleep window and the timetable lookup all running on UTC --
// precisely the bug local_time.h exists to fix. configTzTime() installs
// the POSIX rule and starts SNTP in one call, so the two can't disagree.
int64_t syncTimeAndGetEpoch(const std::string& posixTz) {
  const std::string tz = posixTz.empty() ? std::string(kDefaultPosixTz) : posixTz;
  configTzTime(tz.c_str(), "pool.ntp.org", "time.nist.gov");
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

// --- Static timetable -> board rows (sta_static_schedule.h) ----------------

// Groups scheduled departures into the same DirectionBoard shape
// ui_logic::buildDepartureBoard() produces for live data, so the render
// engine draws them through exactly one path.
//
// Deliberately NOT run through buildDepartureBoard() itself: that filters
// against the user's display settings (departure window, hidden routes,
// per-direction caps) using transit::Route data this doesn't have, and
// re-deriving a Route from a scheduled departure just to throw most of it
// away would be more code and more ways to be wrong. The timetable path is
// a fallback of last resort; showing what the schedule says, grouped by
// route and direction, is the whole job.
//
// isRealTime stays false throughout, which is the truthful answer: these
// are timetable times, and the board marks the whole frame as such via
// BoardStatus::kScheduled.
std::vector<DirectionBoard> buildScheduledBoard(
    const std::vector<sta::ScheduledDeparture>& scheduled) {
  std::vector<DirectionBoard> board;
  for (const sta::ScheduledDeparture& departure : scheduled) {
    // Same (route, direction) grouping sta_models.h's staDeparturesToRoutes()
    // uses, so a route running both ways doesn't collapse into one row.
    const std::string routeId = "sta:" + std::to_string(departure.routeId);
    DirectionBoard* row = nullptr;
    for (DirectionBoard& candidate : board) {
      if (candidate.globalRouteId == routeId &&
          candidate.directionId == static_cast<int>(departure.directionId)) {
        row = &candidate;
        break;
      }
    }
    if (row == nullptr) {
      DirectionBoard fresh;
      fresh.globalRouteId = routeId;
      fresh.routeShortName = departure.routeShortName;
      fresh.routeDisplayShortName.elements[1] = departure.routeShortName;
      fresh.directionId = static_cast<int>(departure.directionId);
      board.push_back(fresh);
      row = &board.back();
    }

    DepartureRow item;
    item.headsign = departure.headsign;
    item.departureTimeEpoch = departure.departureEpoch;
    // The timetable time IS the scheduled time, so both fields agree --
    // which is exactly why formatDepartureChip() adds no comparison
    // annotation here (it only annotates real-time chips).
    item.scheduledDepartureTimeEpoch = departure.departureEpoch;
    item.isRealTime = false;
    row->departures.push_back(item);
  }
  return board;
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

// ---------------------------------------------------------------------------
// Serial diagnostics (boot_report.h)
//
// Everything below is what makes a board debuggable over USB. It runs on
// every build, not just the bringup one, because none of it happens at all
// unless a USB host is actually attached: `if (Serial)` is false on a
// battery-powered board in the field, and every call site below is behind
// that check rather than behind printIfSerial()'s -- an argument like
// formatBootReport(...) is evaluated before the callee can decide not to
// print it, and gathering the report itself costs an SD status read and a
// battery ADC conversion.
// ---------------------------------------------------------------------------

// Survives deep sleep, reinitialized by a power-on reset -- the same RTC
// fast-memory mechanism time_keeper.cpp uses for the approximate clock. A
// boot count that keeps resetting to 1 is the signature of a board that is
// browning out rather than sleeping, which is otherwise very hard to tell
// apart from normal operation.
RTC_DATA_ATTR uint32_t g_bootCount = 0;

// Kept at file scope so the bringup REPL can reprint it without re-reading
// every peripheral (and, more importantly, without a second SD mount).
BootReport g_bootReport;

const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "PANIC (crash)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
  }
}

const char* wakeCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_GPIO: return "gpio";
    case ESP_SLEEP_WAKEUP_EXT0: return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1: return "ext1";
    case ESP_SLEEP_WAKEUP_ULP: return "ulp";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "not a sleep wake";
    default: return "other";
  }
}

std::string efuseMacString() {
  const uint64_t mac = ESP.getEfuseMac();
  char buf[32];
  // getEfuseMac() returns the 48-bit address byte-reversed relative to how
  // a MAC is conventionally written, which is why this indexes downward --
  // printing it the other way produces a plausible-looking address that
  // matches nothing on the network.
  std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                static_cast<unsigned>((mac >> 0) & 0xFF), static_cast<unsigned>((mac >> 8) & 0xFF),
                static_cast<unsigned>((mac >> 16) & 0xFF),
                static_cast<unsigned>((mac >> 24) & 0xFF),
                static_cast<unsigned>((mac >> 32) & 0xFF),
                static_cast<unsigned>((mac >> 40) & 0xFF));
  return buf;
}

// Snapshot of everything worth knowing at boot. Call after the peripherals
// have had their begin() calls (the SD and panel fields are meaningless
// before that) and before the setup flow, so an unprovisioned board still
// reports itself.
BootReport gatherBootReport() {
  BootReport report;

  report.firmwareVersion = FREEINK_FW_VERSION;
  report.buildTimestamp = __DATE__ " " __TIME__;
  report.buildEnv = FREEINK_BUILD_ENV;

  report.chipModel = ESP.getChipModel();
  report.chipRevision = ESP.getChipRevision();
  report.cpuFreqMhz = static_cast<int>(ESP.getCpuFreqMHz());
  report.flashSizeBytes = ESP.getFlashChipSize();
  report.macAddress = efuseMacString();

  report.resetReason = resetReasonName(esp_reset_reason());
  report.wakeCause = wakeCauseName(esp_sleep_get_wakeup_cause());
  report.bootCount = g_bootCount;
  report.freeHeapBytes = ESP.getFreeHeap();
  report.largestFreeBlockBytes = ESP.getMaxAllocHeap();
  report.minEverFreeHeapBytes = ESP.getMinFreeHeap();

  // begin() returns void, so "did the panel come up" has to be inferred
  // from it having produced a framebuffer of a plausible size.
  report.displayWidth = g_display.getDisplayWidth();
  report.displayHeight = g_display.getDisplayHeight();
  report.displayOk =
      g_display.getFrameBuffer() != nullptr && report.displayWidth > 0 && report.displayHeight > 0;

  const sta::StaSdStore::SdStatus sd = g_staSdStore.status();
  report.sdMounted = sd.mounted;
  report.sdTotalBytes = sd.totalBytes;
  report.sdRoutesTable = sd.routesTable;
  report.sdStopTimesTable = sd.stopTimesTable;
  report.sdCalendarTable = sd.calendarTable;

  BatteryMonitor batteryMonitor;
  const BatteryMonitor::Status battery = batteryMonitor.readStatus();
  report.batterySupported = battery.supported;
  if (battery.percentageKnown) report.batteryPercent = static_cast<int>(battery.percentage);
  if (battery.millivoltsKnown) report.batteryMillivolts = static_cast<int>(battery.millivolts);
  report.batteryCharging = battery.chargingKnown && battery.charging;

  report.provisioned = g_configStore.isProvisioned();
  report.wifiSsid = g_configStore.wifiSsid();
  // Handed over whole on purpose: formatBootReport() fingerprints them, so
  // the redaction lives in one place that no caller can forget. See
  // boot_report.h.
  report.wifiPassword = g_configStore.wifiPassword();
  report.apiKey = g_configStore.apiKey();
  report.stopId = g_configStore.stopId();
  report.staStopCode = g_configStore.activeSecondSourceStopCode();
  report.busWifiSsid = g_configStore.busWifiSsid();
  report.timezone = g_configStore.timezone();
  report.refreshIntervalMin = g_configStore.refreshIntervalMin();
  report.sleepWindowStartMin = g_configStore.sleepWindowStartMin();
  report.sleepWindowEndMin = g_configStore.sleepWindowEndMin();
  const std::string cached = g_configStore.cachedBoard();
  report.cachedBoardPresent = !cached.empty();
  report.cachedBoardBytes = cached.size();

  report.otaPartition = runningPartitionLabel();
  report.otaPullConfigured = !g_configStore.otaManifestUrl().empty();
  const OtaTrialState trial = g_configStore.otaTrialState();
  report.otaTrialVersion = trial.pendingVersion;
  report.otaTrialBoots = trial.bootsAttempted;
  report.otaConsecutiveFailures = g_configStore.otaConsecutiveFailures();

  const ApproxClockState clock = loadApproxClock();
  report.approxClockValid = clock.valid;
  if (clock.valid) {
    report.approxClockEpoch = estimateNowEpoch(clock, millis());
    report.approxClockErrorSec = approximateClockErrorSeconds(clock, millis());
    report.wakesSinceSync = clock.wakesSinceSync;
  }

  return report;
}

// Guarded so a board running on battery with nothing plugged in doesn't
// spend milliseconds formatting a report into a void.
void printIfSerial(const std::string& text) {
  if (!Serial) return;
  Serial.print(text.c_str());
  Serial.flush();
}

// ---------------------------------------------------------------------------
// OTA (ota_update.h)
// ---------------------------------------------------------------------------

// Runs before anything that could panic. See the call site in setup() for
// why the trial counter is written at the front of the boot rather than at
// the end of the cycle.
void handleOtaTrialBoot() {
  const OtaTrialState trial = g_configStore.otaTrialState();
  switch (evaluateOtaTrialBoot(trial)) {
    case OtaTrialAction::kNothingPending:
      return;
    case OtaTrialAction::kContinueTrial:
      g_configStore.setOtaTrialState(otaTrialStateAfterBoot(trial));
      return;
    case OtaTrialAction::kRollBack: {
      std::string error;
      const bool rolledBack = rollBackToPreviousSlot(error);
      // Cleared either way. If the rollback worked, the trial is over; if it
      // didn't -- a board whose other slot has never been written -- leaving
      // the state set would re-attempt an impossible rollback on every
      // single boot forever, and the image, bad as it is, is the only one
      // there is. Say so loudly instead.
      g_configStore.setOtaTrialState(OtaTrialState{});
      if (Serial) {
        Serial.printf("[ota] %s failed %d trial boots; rollback %s%s%s\n",
                      trial.pendingVersion.c_str(), trial.bootsAttempted,
                      rolledBack ? "succeeded, rebooting" : "FAILED: ",
                      rolledBack ? "" : error.c_str(), "");
        Serial.flush();
      }
      if (rolledBack) {
        delay(100);
        ESP.restart();
      }
      return;
    }
  }
}

void reportOtaProgress(size_t written, size_t total) {
  if (!Serial) return;
  Serial.printf("[ota] %u / %u bytes (%u%%)\n", static_cast<unsigned>(written),
                static_cast<unsigned>(total),
                total == 0 ? 0u : static_cast<unsigned>((written * 100) / total));
}

// The pull path: ask the configured manifest whether a newer build exists
// and install it if every gate in decideOtaUpdate() agrees. Returns true
// when an image was installed and the caller should reboot into it rather
// than sleeping.
bool runOtaCheck(WakeSummary& summary) {
  const std::string manifestUrl = g_configStore.otaManifestUrl();

  OtaGateInputs gate;
  gate.manifestConfigured = !manifestUrl.empty();
  gate.runningVersion = FREEINK_FW_VERSION;
  gate.consecutiveFailures = g_configStore.otaConsecutiveFailures();
  gate.failingVersion = g_configStore.otaFailingVersion();

  BatteryMonitor batteryMonitor;
  const BatteryMonitor::Status battery = batteryMonitor.readStatus();
  if (battery.supported && battery.percentageKnown) {
    gate.batteryPercent = static_cast<int>(battery.percentage);
  }
  // Charging and "externally powered" are treated the same here: either way
  // the supply is not about to disappear mid-erase, which is the only thing
  // the battery gate is protecting against.
  gate.externalPower = (battery.externalPowerKnown && battery.externalPower) ||
                       (battery.chargingKnown && battery.charging);

  if (gate.manifestConfigured) {
    // Small JSON, so the buffering HttpTransport is fine here -- unlike the
    // image itself, which applyOtaFromUrl() streams for exactly that reason.
    const HttpResponse response = g_httpTransport.get(manifestUrl, {});
    if (response.transportOk && response.statusCode == 200) {
      gate.manifestFetched = parseOtaManifest(response.body, gate.manifest);
    }
  }

  const OtaDecision decision = decideOtaUpdate(gate);
  summary.otaDecision = otaDecisionName(decision);
  if (gate.manifestFetched) summary.otaAvailableVersion = gate.manifest.version;
  if (decision != OtaDecision::kProceed) return false;

  if (Serial) {
    Serial.printf("[ota] installing %s (%lld bytes) from %s\n", gate.manifest.version.c_str(),
                  static_cast<long long>(gate.manifest.sizeBytes), gate.manifest.url.c_str());
    Serial.flush();
  }

  const OtaApplyResult result = applyOtaFromUrl(gate.manifest, &reportOtaProgress);
  if (!result.ok) {
    summary.otaError = result.error;
    g_configStore.recordOtaFailure(gate.manifest.version);
    return false;
  }

  g_configStore.clearOtaFailures();
  // Written before the reboot, so the next boot knows it is on trial even
  // if it panics immediately.
  g_configStore.setOtaTrialState(otaTrialStateForNewImage(gate.manifest.version));
  summary.otaApplied = true;
  return true;
}

#if FREEINK_BRINGUP
// Deliberately exercises the hardware rather than inferring its health from
// a normal wake: a wake cycle that finds no Wi-Fi tells you nothing about
// whether the radio works, and one that renders a cached board tells you
// nothing about whether the panel's full-refresh waveform is right.
//
// Bringup-only, and not merely because the field build has no way to reach
// it: the panel check deliberately burns two full refreshes (several
// seconds and the cycle's largest single current draw) and the Wi-Fi scan
// another few, which is exactly the wrong trade on a battery.
SelfTestReport runSelfTest(const BootReport& report, const std::string& expectedSsid) {
  SelfTestReport out;
  char detail[192];

  {
    SelfTestResult r;
    r.name = "display";
    r.passed = report.displayOk;
    std::snprintf(detail, sizeof(detail), "%dx%d framebuffer %s", report.displayWidth,
                  report.displayHeight, report.displayOk ? "allocated" : "MISSING");
    r.detail = detail;
    out.results.push_back(r);
  }

  {
    // A visible black/white flash. There is no way to read the panel back,
    // so this cannot fail automatically -- what it produces is a human
    // verdict ("did the screen flash twice?") plus the refresh timing,
    // which is the number that actually moves when a waveform or the SPI
    // wiring is wrong.
    SelfTestResult r;
    r.name = "panel refresh";
    if (!report.displayOk) {
      r.skipped = true;
      r.detail = "no framebuffer";
    } else {
      const uint32_t start = millis();
      g_display.clearScreen(0x00);
      g_display.displayBuffer(EInkDisplay::FULL_REFRESH);
      g_display.clearScreen(0xFF);
      g_display.displayBuffer(EInkDisplay::FULL_REFRESH);
      const uint32_t elapsed = millis() - start;
      r.passed = true;
      std::snprintf(detail, sizeof(detail),
                    "2 full refreshes in %u ms -- confirm the panel flashed black then white",
                    static_cast<unsigned>(elapsed));
      r.detail = detail;
    }
    out.results.push_back(r);
  }

  {
    SelfTestResult r;
    r.name = "nvs config";
    // A refresh interval of 0 means ConfigStore handed back neither a
    // stored value nor its documented default, which only happens when the
    // NVS namespace itself failed to open.
    const int interval = g_configStore.refreshIntervalMin();
    r.passed = interval > 0;
    std::snprintf(detail, sizeof(detail), "refresh interval reads back as %d min", interval);
    r.detail = detail;
    out.results.push_back(r);
  }

  {
    SelfTestResult r;
    r.name = "sd card";
    if (!report.sdMounted) {
      // No card is a supported configuration -- everything SD-backed has a
      // flash-baked fallback -- so this is not a failure, just a fact.
      r.skipped = true;
      r.detail = "not mounted (optional; STA falls back to flash tables)";
    } else {
      r.passed = report.sdRoutesTable;
      std::snprintf(detail, sizeof(detail), "mounted, routes=%s stop_times=%s calendar=%s",
                    report.sdRoutesTable ? "yes" : "NO", report.sdStopTimesTable ? "yes" : "NO",
                    report.sdCalendarTable ? "yes" : "NO");
      r.detail = detail;
    }
    out.results.push_back(r);
  }

  {
    SelfTestResult r;
    r.name = "battery";
    if (!report.batterySupported || report.batteryMillivolts < 0) {
      r.skipped = true;
      r.detail = "no telemetry on this board profile";
    } else {
      // A 1S Li-ion pack outside this range is a divider/ADC problem, not a
      // flat battery -- a genuinely empty cell still reads well above 2.5 V
      // before the protection circuit cuts it off.
      r.passed = report.batteryMillivolts > 2500 && report.batteryMillivolts < 4500;
      std::snprintf(detail, sizeof(detail), "%d mV, %d%%%s", report.batteryMillivolts,
                    report.batteryPercent, report.batteryCharging ? ", charging" : "");
      r.detail = detail;
    }
    out.results.push_back(r);
  }

  {
    SelfTestResult r;
    r.name = "heap headroom";
    // The STA feed is ~190 KB and is parsed in RAM; sta_client.h refuses to
    // fetch below its own threshold. Anything under this leaves no room for
    // it even before TLS buffers.
    constexpr uint32_t kMinFreeHeap = 90u * 1024u;
    r.passed = report.freeHeapBytes >= kMinFreeHeap;
    std::snprintf(detail, sizeof(detail), "%u B free, largest block %u B (want >= %u B)",
                  static_cast<unsigned>(report.freeHeapBytes),
                  static_cast<unsigned>(report.largestFreeBlockBytes),
                  static_cast<unsigned>(kMinFreeHeap));
    r.detail = detail;
    out.results.push_back(r);
  }

  {
    SelfTestResult r;
    r.name = "wifi scan";
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(/*wifioff=*/false);
    const int found = WiFi.scanNetworks();
    bool sawConfigured = false;
    int configuredRssi = 0;
    for (int i = 0; i < found; ++i) {
      if (!expectedSsid.empty() && WiFi.SSID(i) == expectedSsid.c_str()) {
        sawConfigured = true;
        configuredRssi = WiFi.RSSI(i);
      }
    }
    // The radio working and the configured network being reachable are two
    // different findings; conflating them turns "you typed the SSID wrong"
    // into "the Wi-Fi is broken".
    r.passed = found > 0;
    if (expectedSsid.empty()) {
      std::snprintf(detail, sizeof(detail), "%d networks (no SSID configured yet)", found);
    } else if (sawConfigured) {
      std::snprintf(detail, sizeof(detail), "%d networks; \"%s\" at %d dBm", found,
                    expectedSsid.c_str(), configuredRssi);
    } else {
      std::snprintf(detail, sizeof(detail), "%d networks; configured SSID \"%s\" NOT in range",
                    found, expectedSsid.c_str());
    }
    r.detail = detail;
    WiFi.scanDelete();
    out.results.push_back(r);
  }

  return out;
}

// Instead of deep-sleeping at the end of the cycle, stay awake so the USB
// CDC link survives and the board can be poked at. Never returns: 'c'
// reboots (which re-runs the whole cycle from a known state, rather than
// re-entering it halfway with stale globals) and 's' sleeps for real.
[[noreturn]] void bringupHold(const std::string& expectedSsid, int wakeIntervalMin) {
  const char* kHelp =
      "\n-- bringup console --\n"
      "  r  reprint the boot report\n"
      "  t  run the self-test again\n"
      "  w  wifi scan\n"
      "  c  reboot and run a full wake cycle\n"
      "  s  enter deep sleep now (normal firmware behaviour)\n"
      "  h  this help\n"
      "The board is being held awake; it will NOT sleep on its own.\n";
  printIfSerial(kHelp);

  while (true) {
    if (Serial && Serial.available() > 0) {
      const int c = Serial.read();
      switch (c) {
        case 'r': printIfSerial(formatBootReport(g_bootReport)); break;
        case 't': printIfSerial(formatSelfTestReport(runSelfTest(g_bootReport, expectedSsid))); break;
        case 'w': {
          WiFi.mode(WIFI_STA);
          const int found = WiFi.scanNetworks();
          if (Serial) {
            Serial.printf("\n%d networks:\n", found);
            for (int i = 0; i < found; ++i) {
              Serial.printf("  %-32s %4d dBm  ch%2d  %s\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                            WiFi.channel(i),
                            WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
            }
          }
          WiFi.scanDelete();
          break;
        }
        case 'c':
          printIfSerial("rebooting...\n");
          delay(100);
          ESP.restart();
          break;
        case 's':
          printIfSerial("sleeping...\n");
          delay(100);
          enterDeepSleep(g_display, wakeIntervalMin);
          break;
        case 'h': printIfSerial(kHelp); break;
        default: break;
      }
    }
    delay(20);
  }
}
#endif  // FREEINK_BRINGUP

}  // namespace

void setup() {
  // First thing, before any peripheral can hang: a board that dies in
  // BoardConfig or the SD mount should still have said hello. With
  // ARDUINO_USB_CDC_ON_BOOT the core has already brought Serial up, but
  // calling begin() explicitly costs nothing and keeps this correct if that
  // flag ever changes.
  Serial.begin(115200);
#if FREEINK_BRINGUP
  // USB CDC enumeration takes about a second, and a monitor started by
  // hand takes longer still. The normal firmware must never wait for a host
  // that will never arrive (it runs on battery), but the bringup build
  // exists precisely to be watched, so it waits -- briefly, and no longer
  // once the host shows up.
  {
    const uint32_t waitStart = millis();
    while (!Serial && millis() - waitStart < 8000) delay(50);
    delay(200);  // let the host's terminal attach before the first line
  }
#endif
  ++g_bootCount;

  BoardConfig::holdPowerRails();
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);

  // Before the SD mount, the panel, the radio -- before anything that could
  // panic. A freshly-installed image is "on trial" until it completes one
  // whole wake cycle; the count is written here, at the front, precisely so
  // that an image which crashes before it can write anything still burns a
  // trial. Counting at the end instead would let a boot-looping image
  // retry forever, which is the failure this exists to prevent.
  // See ota_update.h.
  handleOtaTrialBoot();

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

  // Before anything formats a time or computes a service day. Until this
  // call the process is on UTC (configTime(0, 0, ...) below), which is
  // merely a few hours of skew for the sleep window but is fatal to the
  // static timetable -- see local_time.h.
  applyTimezone(g_configStore.timezone());

  g_display.begin();

  // Everything the report reads is up by now (panel, SD, NVS, battery), and
  // nothing below has modified any of it yet -- so this is the board as it
  // arrived at this boot, before the firmware starts changing it.
  if (Serial) {
    g_bootReport = gatherBootReport();
    printIfSerial(formatBootReport(g_bootReport));
#if FREEINK_BRINGUP
    printIfSerial(formatSelfTestReport(runSelfTest(g_bootReport, g_configStore.wifiSsid())));
#endif
  }

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

  // Filled in as the cycle runs and printed once at the end (boot_report.h).
  // Its defaults all read as "never got there", so a cycle that dies partway
  // through still leaves an honest record of how far it got.
  WakeSummary summary;
  summary.batteryPercent = status.batteryPercent;

  // Whatever the previous wake left in RTC memory (time_keeper.h). Read
  // before anything else touches the clock, since a successful SNTP sync
  // below overwrites the system time this is the only alternative to.
  const ApproxClockState priorClock = loadApproxClock();

  const uint32_t wifiStartMs = millis();
  bool wifiOk = connectWifi(g_configStore.wifiSsid(), g_configStore.wifiPassword());
  if (wifiOk) summary.wifiSsidUsed = g_configStore.wifiSsid();

  // Home network isn't in range -- try the configured open network (onboard
  // transit Wi-Fi) and, if something is intercepting it, hand the portal
  // the saved email/phone. Only attempted when the user has actually
  // configured an SSID: the board never joins an open network on its own.
  if (!wifiOk) {
    const std::string busSsid = g_configStore.busWifiSsid();
    if (!busSsid.empty() && connectOpenWifi(busSsid)) {
      summary.viaBusWifi = true;
      summary.wifiSsidUsed = busSsid;
      CaptivePortalConfig portalConfig;
      portalConfig.identity = g_configStore.busWifiIdentity();
      portalConfig.overrideSubmitUrl = g_configStore.busPortalSubmitUrl();
      portalConfig.overrideFieldName = g_configStore.busPortalFieldName();

      CaptivePortalClient portalClient(g_httpTransport);
      const CaptivePortalResult portalResult = portalClient.connect(portalConfig);
      wifiOk = portalResult.online;
      summary.viaCaptivePortalLogin = portalResult.online;
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
  summary.wifiOk = wifiOk;
  summary.wifiMs = millis() - wifiStartMs;
  if (wifiOk) summary.wifiRssiDbm = WiFi.RSSI();

  int64_t nowEpoch = 0;
  bool clockFromSntp = false;
  std::vector<Route> routes;
  std::vector<PresetTripPlan> presetPlans;
  bool anyFetchOk = false;
  if (wifiOk) {
    nowEpoch = syncTimeAndGetEpoch(g_configStore.timezone());
    clockFromSntp = nowEpoch > 0;
    summary.sntpOk = clockFromSntp;

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
    const uint32_t fetchStartMs = millis();
    bool fetchOk = apiClient.stopDepartures(stopIds, params, response);
    status.lastFetchFailed = !fetchOk;
    anyFetchOk = anyFetchOk || fetchOk;
    summary.fetchMs = millis() - fetchStartMs;
    summary.transitFetchAttempted = true;
    summary.transitFetchOk = fetchOk;
    // 0 here means the request never reached a server at all (DNS, TLS,
    // no route) -- a distinction a bare "fetch failed" cannot make, and the
    // first thing worth knowing when a board that worked yesterday stops.
    summary.transitHttpStatus = apiClient.lastStatusCode();
    summary.transitStopIdsRequested = static_cast<int>(stopIds.size());
    summary.transitRouteCount = static_cast<int>(response.routeDepartures.size());
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
    // agency_list) shown alongside Transit's own departures, not merged into
    // them -- see sta_models.h's staDeparturesToRoutes(). Independent of
    // the Transit fetch above: attempted whenever Wi-Fi is up regardless of
    // whether that fetch succeeded, and a failure here (network, a stop
    // code sta_stop_table.h doesn't recognize, or too little free heap for
    // the ~190KB feed -- see sta_client.h) never affects status/routes
    // above, since the board still has Transit's departures either way.
    std::string staStopCode = g_configStore.activeSecondSourceStopCode();
    if (!staStopCode.empty()) {
      sta::StaClient staClient(g_httpTransport, &g_staSdStore);
      std::vector<Route> staRoutes = staClient.fetchDepartures(staStopCode);
      summary.staFetchAttempted = true;
      summary.staRouteCount = static_cast<int>(staRoutes.size());
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
      summary.clockApproximate = true;
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
    summary.departureSource = "live";

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
      status.source = BoardStatus::DepartureSource::kCached;
      summary.departureSource = "cached";
      // -1, not 0, when there's no clock: without one the age is genuinely
      // unknown, and reporting 0 would label a board of unknown vintage
      // "Cached just now". See BoardStatus::cachedAgeMin.
      status.cachedAgeMin = nowEpoch > 0 ? cachedAgeMinutes(restored, nowEpoch) : -1;
      summary.cachedAgeMin = status.cachedAgeMin;

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

    // Last resort, and the only one that still works a week into a trip:
    // STA's published timetable, read straight off the SD card
    // (sta_static_schedule.h). Preferred over nothing, but NOT over a
    // cache that still has entries -- cached rows carry real-time
    // predictions the timetable can't know about, so a recent cache is
    // strictly better information while it lasts.
    if (board.empty()) {
      const std::string staStopCode = g_configStore.activeSecondSourceStopCode();
      const sta::StopInfo* staStop =
          staStopCode.empty() ? nullptr : sta::parseStaStopCode(staStopCode);
      if (staStop != nullptr && nowEpoch > 0) {
        sta::StaticScheduleTables tables = g_staSdStore.scheduleTables();

        // Read validity BEFORE looking for departures. An expired feed
        // activates no services at all, so the lookup comes back empty --
        // and reporting that as "nothing scheduled" would hide the one
        // thing the reader can actually act on, which is that the card
        // needs regenerating. The expiry has to be able to speak for
        // itself even with an empty board behind it.
        sta::FeedValidity validity;
        if (tables.calendar != nullptr) validity = sta::readFeedValidity(*tables.calendar);
        const bool expired = validity.known && !sta::feedCoversDate(validity, serviceDayFor(nowEpoch).date);

        const std::vector<sta::ScheduledDeparture> scheduled = sta::nextScheduledDepartures(
            tables, static_cast<uint32_t>(staStop->stopCode), serviceDayCandidates(nowEpoch),
            std::max(g_configStore.maxDeparturesPerDirection(), 4) * 2);

        if (!scheduled.empty() || expired) {
          board = buildScheduledBoard(scheduled);
          status.source = BoardStatus::DepartureSource::kScheduled;
          summary.departureSource = "scheduled";
          status.scheduleValidUntil = validity.endDate;
          status.scheduleExpired = expired;
        }
      }
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
  const uint32_t renderStartMs = millis();
  renderEngine.renderDepartureBoard(board, status);
  summary.renderMs = millis() - renderStartMs;

  summary.nowEpoch = nowEpoch;
  summary.presetPlanCount = static_cast<int>(presetPlans.size());
  summary.boardDirectionCount = static_cast<int>(board.size());
  for (const DirectionBoard& direction : board) {
    summary.boardDepartureCount += static_cast<int>(direction.departures.size());
  }

  // Firmware update check, deliberately AFTER the panel has been redrawn:
  // an update that reboots the board mid-cycle must not cost the reader
  // their departure board for the next hour. Wi-Fi is still up here.
  if (wifiOk && runOtaCheck(summary)) {
    summary.totalAwakeMs = millis();
    if (Serial) printIfSerial(formatWakeSummary(summary));
    delay(200);
    // Not deep sleep: the whole point is to start running the image that
    // was just installed. It comes up on trial (ota_update.h) and has to
    // complete a cycle of its own before it is kept.
    ESP.restart();
  }

  // The cycle completed, which is the only evidence this firmware has that a
  // freshly-installed image actually works. Anything earlier would pass an
  // image that boots and then fails at the thing it exists to do.
  const OtaTrialState trialAtEnd = g_configStore.otaTrialState();
  if (!trialAtEnd.pendingVersion.empty()) {
    g_configStore.setOtaTrialState(otaTrialStateAfterSuccess(trialAtEnd));
    if (Serial) {
      Serial.printf("[ota] %s completed a full cycle on boot %d; keeping it\n",
                    trialAtEnd.pendingVersion.c_str(), trialAtEnd.bootsAttempted);
    }
  }

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

  summary.nextWakeMin = wakeIntervalMin;
  summary.totalAwakeMs = millis();
  if (Serial) printIfSerial(formatWakeSummary(summary));

#if FREEINK_BRINGUP
  // Never returns. Deliberately after the full cycle above, so the console
  // is reached with a real board on the panel and a real summary printed --
  // not on a board that was diverted before it did any work.
  bringupHold(g_configStore.wifiSsid(), wakeIntervalMin);
#endif

  enterDeepSleep(g_display, wakeIntervalMin);  // noreturn — chip resets on wake
}

void loop() {
  // Unreachable: setup() always ends in enterDeepSleep().
}
