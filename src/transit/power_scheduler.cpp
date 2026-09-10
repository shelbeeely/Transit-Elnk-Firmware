#include "transit/power_scheduler.h"

#ifndef FREEINK_HOST_NATIVE
#include <esp_sleep.h>

#include <PowerManager.h>
#endif

namespace transit {

// Real, minimal implementation (small enough not to warrant a stub) —
// docs/DEPLOYMENT_OPS.md's 60-min-default math with sleep-window stretching.
// Unit 7 may extend, e.g. with a "paused entirely" mode.
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
  return inWindow ? refreshIntervalMin * 3 : refreshIntervalMin;
}

#ifndef FREEINK_HOST_NATIVE

// Stub implementation — replaced by work unit 7 (power scheduler).
// Exists so the scaffold links and runs end to end before that unit lands.
[[noreturn]] void enterDeepSleep(EInkDisplay& display, int wakeIntervalMin) {
  display.displayBuffer(EInkDisplay::FULL_REFRESH);
  display.deepSleep();

  if (wakeIntervalMin < 1) wakeIntervalMin = 60;
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(wakeIntervalMin) * 60ULL * 1000000ULL);

  freeink::PowerManager::powerDownRailsForSleep();
  freeink::PowerManager::deepSleep();
}

#endif

}  // namespace transit
