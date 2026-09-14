#pragma once

// Transit-Elnk-Firmware — structured serial diagnostics for hardware bringup.
//
// Until this module existed the firmware was very nearly silent on USB: no
// Serial.begin() call anywhere, and exactly one module (sta_client.cpp)
// printing anything at all. Plug the board in, run `pio device monitor`, and
// you would see a handful of STA lines and nothing else -- no confirmation
// that the panel initialized, that the SD card mounted, that Wi-Fi
// associated, that SNTP returned, or why the board decided to sleep for the
// interval it picked. That is a painful way to meet a new board for the
// first time, and it is the gap this module closes.
//
// Three report shapes, all pure and host-tested:
//
//   * BootReport    -- a one-time snapshot taken early in setup(), after the
//                      peripherals have had their begin() calls: silicon,
//                      reset/wake cause, memory, panel, SD, battery, the
//                      persisted config (redacted), and whatever the RTC
//                      -memory clock carried across the sleep.
//   * WakeSummary   -- one block at the end of the cycle saying what this
//                      wake actually did: which network, whether SNTP
//                      answered, what the API returned, which of the three
//                      departure sources won, how long each stage took, and
//                      how long the board is about to sleep.
//   * SelfTestReport-- pass/fail per subsystem, for the bringup build that
//                      exercises the hardware deliberately instead of
//                      inferring its health from a normal wake.
//
// Output is deliberately flat, ASCII, and grep-friendly ("key: value", one
// per line, section headers in brackets) so a captured log can be pasted
// into a chat, diffed between two boots, or scraped by tools/bringup.sh
// without anything having to parse it properly.
//
// Redaction is not optional and not the caller's job to remember:
// setApiKey()-style secrets go through redactSecret() inside the formatter,
// so there is no code path that prints a key or a Wi-Fi password in full
// even if a caller hands one over whole. See CLAUDE.md's guardrails.

#include <cstdint>
#include <string>
#include <vector>

namespace transit {

// Renders a secret as a short, non-reversible fingerprint safe to print,
// log, paste into a chat, or commit in a captured bringup log:
//
//   ""            -> "(unset)"
//   "abc"         -> "(set, 3 chars)"       -- too short to show any of
//   "sk_live_..ab12" -> "(set, 24 chars, ...ab12)"
//
// The length is included because "is the key the right length" is a real
// bringup question (a truncated paste into the setup portal is a common
// failure) that the last four characters alone cannot answer. Four
// characters is the most that is shown, ever, regardless of secret length.
std::string redactSecret(const std::string& secret);

// One-time snapshot of the board, taken once per boot. Every field has a
// benign default so a caller that cannot determine something (no SD card, a
// board profile with no battery telemetry) simply leaves it alone and the
// formatter reports it as unknown rather than as a confident zero.
struct BootReport {
  // --- build ---------------------------------------------------------
  std::string firmwareVersion;  // FREEINK_FW_VERSION, baked in at build time
  std::string buildTimestamp;   // __DATE__ " " __TIME__
  std::string buildEnv;         // "xteink_x4" / "xteink_x4_bringup"

  // --- silicon -------------------------------------------------------
  std::string chipModel;
  int chipRevision = -1;
  int cpuFreqMhz = 0;
  uint32_t flashSizeBytes = 0;
  std::string macAddress;

  // --- this boot -----------------------------------------------------
  std::string resetReason;  // esp_reset_reason(), already stringified
  std::string wakeCause;    // esp_sleep_get_wakeup_cause(), stringified
  uint32_t bootCount = 0;   // RTC-memory counter; 0 = cold boot
  uint32_t freeHeapBytes = 0;
  uint32_t largestFreeBlockBytes = 0;
  uint32_t minEverFreeHeapBytes = 0;

  // --- peripherals ---------------------------------------------------
  bool displayOk = false;
  int displayWidth = 0;
  int displayHeight = 0;
  bool sdMounted = false;
  uint64_t sdTotalBytes = 0;
  // Which of the four SD tables StaSdStore found. A card that mounts but
  // carries no tables looks identical to a missing card from the departure
  // board's point of view, and telling those two apart is most of what
  // bringup needs from the SD path.
  bool sdRoutesTable = false;
  bool sdStopTimesTable = false;
  bool sdCalendarTable = false;
  bool batterySupported = false;
  int batteryPercent = -1;     // -1 = not read
  int batteryMillivolts = -1;  // -1 = not read
  bool batteryCharging = false;

  // --- persisted config (redacted by the formatter) -------------------
  bool provisioned = false;
  std::string wifiSsid;
  std::string wifiPassword;  // never printed; only its fingerprint is
  std::string apiKey;        // never printed; only its fingerprint is
  std::string stopId;
  std::string staStopCode;
  std::string timezone;
  std::string busWifiSsid;
  int refreshIntervalMin = 0;
  int sleepWindowStartMin = -1;
  int sleepWindowEndMin = -1;
  bool cachedBoardPresent = false;
  size_t cachedBoardBytes = 0;

  // --- OTA (ota_update.h) --------------------------------------------
  // Which slot this image is running from, whether a pull URL is
  // configured, and whether this boot is a freshly-installed image still on
  // trial. The last one matters most in a log: "why did my board revert" is
  // unanswerable unless the trial state is visible while it is happening.
  std::string otaPartition;
  bool otaPullConfigured = false;
  std::string otaTrialVersion;  // empty = not on trial
  int otaTrialBoots = 0;
  int otaConsecutiveFailures = 0;

  // --- clock carried across deep sleep (time_keeper.h) ----------------
  bool approxClockValid = false;
  int64_t approxClockEpoch = 0;
  int64_t approxClockErrorSec = 0;
  int32_t wakesSinceSync = 0;
};

// Multi-line, newline-terminated. Safe to print verbatim: every secret in
// the struct passes through redactSecret() here.
std::string formatBootReport(const BootReport& report);

// What one wake cycle actually did. Filled in progressively as the cycle
// runs, so a crash partway through still leaves a partially-populated
// struct whose defaults read as "did not get there" rather than as success.
struct WakeSummary {
  // --- network -------------------------------------------------------
  bool wifiOk = false;
  std::string wifiSsidUsed;
  int wifiRssiDbm = 0;  // 0 = not read
  bool viaBusWifi = false;
  bool viaCaptivePortalLogin = false;

  // --- clock ---------------------------------------------------------
  bool sntpOk = false;
  int64_t nowEpoch = 0;
  bool clockApproximate = false;

  // --- data ----------------------------------------------------------
  bool transitFetchAttempted = false;
  bool transitFetchOk = false;
  int transitHttpStatus = 0;
  int transitStopIdsRequested = 0;
  int transitRouteCount = 0;
  bool staFetchAttempted = false;
  int staRouteCount = 0;

  // "live" / "cached" / "scheduled" / "none" -- which of the three
  // fallback tiers actually put rows on the panel this wake.
  std::string departureSource = "none";
  int cachedAgeMin = -1;
  int boardDirectionCount = 0;
  int boardDepartureCount = 0;
  int presetPlanCount = 0;

  // --- timing / next wake --------------------------------------------
  uint32_t wifiMs = 0;
  uint32_t fetchMs = 0;
  uint32_t renderMs = 0;
  uint32_t totalAwakeMs = 0;
  int nextWakeMin = 0;

  // --- OTA (ota_update.h) --------------------------------------------
  // otaDecision is otaDecisionName()'s string, so every wake says why it
  // did or didn't update rather than silently doing nothing. Empty when the
  // cycle never reached the check (no network).
  std::string otaDecision;
  std::string otaAvailableVersion;
  bool otaApplied = false;
  std::string otaError;
  int batteryPercent = -1;
};

std::string formatWakeSummary(const WakeSummary& summary);

// One subsystem's verdict in the bringup self-test. detail carries whatever
// makes the result actionable -- a measured voltage, a panel size, the
// number of networks a scan found, the reason a mount failed.
struct SelfTestResult {
  std::string name;
  bool passed = false;
  std::string detail;
  // A check that is not applicable to this board (no SD card inserted, no
  // battery telemetry in the profile) is neither a pass nor a failure and
  // must not drag the overall verdict down.
  bool skipped = false;
};

struct SelfTestReport {
  std::vector<SelfTestResult> results;
  // True only when every non-skipped check passed. An empty report is not
  // a pass -- it means the self-test never ran.
  bool allPassed() const;
  int passedCount() const;
  int failedCount() const;
  int skippedCount() const;
};

std::string formatSelfTestReport(const SelfTestReport& report);

}  // namespace transit
