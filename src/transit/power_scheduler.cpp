#include "transit/power_scheduler.h"

#ifndef FREEINK_HOST_NATIVE
#include <esp_sleep.h>

#include <BatteryMonitor.h>
#include <PowerManager.h>
#endif

namespace transit {

namespace {
constexpr int kMinutesPerDay = 24 * 60;

// docs/DEPLOYMENT_OPS.md: "stretched further during a configured sleep
// window (e.g. 2-4x, or paused entirely)". 3x sits mid-band as the default
// multiplier; see the window-end clamp below for why a flat multiplier
// alone isn't the whole story.
constexpr int kSleepWindowMultiplier = 3;
}  // namespace

// Real, minimal implementation (small enough not to warrant a stub) —
// docs/DEPLOYMENT_OPS.md's 60-min-default math with sleep-window stretching.
//
// A flat kSleepWindowMultiplier alone has a failure mode: woken soon after
// the window opens (e.g. right at sleep_window_start), a 3x-stretched
// interval can land well *past* sleep_window_end on a long window,
// oversleeping into hours the board is supposed to be refreshing normally
// again and delaying the first post-window refresh by however much it
// overshot. So the stretched interval is clamped to never exceed the
// minutes remaining until the window ends — effectively "stretch by 3x, or
// pause until the window ends, whichever wakes sooner". Near the start of a
// long window this is just the flat 3x; near the end it degrades into the
// doc's "paused entirely" option and wakes right as the window closes.
// Deliberately self-contained (no new ConfigStore field) rather than a
// user-facing multiplier setting — this unit's header contract
// (power_scheduler.h) is the only one it may extend; see the PR description
// for why a configurable multiplier was left for a future unit instead.
int computeNextWakeIntervalMin(int refreshIntervalMin, const SleepWindow& window,
                                int nowMinOfDay) {
  if (refreshIntervalMin < 1) refreshIntervalMin = 60;
  if (!window.enabled) return refreshIntervalMin;

  bool inWindow;
  if (window.startMinOfDay <= window.endMinOfDay) {
    inWindow = nowMinOfDay >= window.startMinOfDay && nowMinOfDay < window.endMinOfDay;
  } else {
    // Window wraps past midnight, e.g. 22:00-06:00.
    inWindow = nowMinOfDay >= window.startMinOfDay || nowMinOfDay < window.endMinOfDay;
  }
  if (!inWindow) return refreshIntervalMin;

  int stretched = refreshIntervalMin * kSleepWindowMultiplier;

  // Minutes from now until the window closes (wraps past midnight the same
  // way the wrap-window inWindow check above does). inWindow == true
  // guarantees nowMinOfDay != window.endMinOfDay, so this is always > 0.
  int minutesUntilEnd = window.endMinOfDay - nowMinOfDay;
  if (minutesUntilEnd <= 0) minutesUntilEnd += kMinutesPerDay;

  return stretched < minutesUntilEnd ? stretched : minutesUntilEnd;
}

#ifndef FREEINK_HOST_NATIVE

// Render->sleep sequence modeled on Free-Ink/inkdeck: push the final frame
// with a full refresh, put the panel controller to sleep, arm the ESP32
// timer wakeup, then power down peripheral rails and enter chip deep sleep.
[[noreturn]] void enterDeepSleep(EInkDisplay& display, int wakeIntervalMin) {
  display.displayBuffer(EInkDisplay::FULL_REFRESH);
  display.deepSleep();

  if (wakeIntervalMin < 1) wakeIntervalMin = 60;
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(wakeIntervalMin) * 60ULL * 1000000ULL);

  freeink::PowerManager::powerDownRailsForSleep();
  freeink::PowerManager::deepSleep();
}

// Reads battery level via freeink::BatteryMonitor (X4: ADC backend, per
// BoardConfig::ACTIVE — see BatteryMonitor.h). Returns 0 rather than a
// stale/optimistic value when the board has no battery telemetry path or
// the read didn't produce a known percentage, so a caller never mistakes
// "unknown" for "full".
int readBatteryPercent() {
  freeink::BatteryMonitor batteryMonitor;
  freeink::BatteryMonitor::Status status = batteryMonitor.readStatus();
  if (!status.supported || !status.percentageKnown) return 0;
  return static_cast<int>(status.percentage);
}

#endif

}  // namespace transit
