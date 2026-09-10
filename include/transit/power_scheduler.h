#pragma once

// Transit-Elnk-Firmware — wake-interval math and deep-sleep entry.
//
// Interval math matches docs/DEPLOYMENT_OPS.md (60-min default, treated as a
// floor) and docs/CONFIG_AND_STATE.md's sleep_window_start/end (stretch or
// pause refresh_interval_min between those local times, since nobody reads
// an e-ink board overnight either). computeNextWakeIntervalMin is a pure
// function, unit-testable under [env:native]; enterDeepSleep is hardware-
// dependent (freeink::PowerManager + EInkDisplay) and only buildable under
// [env:xteink_x4].
//
// No sibling Free-Ink org app wakes on a timer (all are button-wake or
// always-on) — mirror Free-Ink/inkdeck's render->sleep sequence
// (clearScreen/render/displayBuffer(FULL_REFRESH)/display.deepSleep()) for
// the shutdown half, then add the timer-wake half: check PowerManager.h for
// a timer-arm function first; if none exists (none was found as of this
// scaffold), call esp_sleep_enable_timer_wakeup(us) directly (standard
// ESP-IDF API under the Arduino-ESP32 core) before
// freeink::PowerManager::powerDownRailsForSleep() + deepSleep().
//
// Frozen contract for the parallel work units: do not change existing
// function signatures. Adding a function is fine; note it in your PR
// description.

#include <cstdint>

// enterDeepSleep() is hardware-dependent; computeNextWakeIntervalMin() is
// pure and must stay includable/testable under [env:native], which has no
// FreeInk/EInkDisplay available. platformio.ini's [env:native] defines
// FREEINK_HOST_NATIVE=1.
#ifndef FREEINK_HOST_NATIVE
#include <EInkDisplay.h>
#endif

namespace transit {

struct SleepWindow {
  bool enabled = false;
  int startMinOfDay = -1;  // minutes since local midnight
  int endMinOfDay = -1;    // wraps past midnight if endMinOfDay < startMinOfDay
};

// Computes the next wake interval in minutes, given the configured
// refresh_interval_min floor, an optional sleep window, and the current
// local minute-of-day. Inside the sleep window, stretches the interval
// (docs/DEPLOYMENT_OPS.md's "2-4x, or paused entirely" guidance) rather than
// polling at the normal cadence. Pure function — no hardware access.
int computeNextWakeIntervalMin(int refreshIntervalMin, const SleepWindow& window,
                                int nowMinOfDay);

#ifndef FREEINK_HOST_NATIVE
// Draws status onto the panel (already-rendered final frame is assumed to
// be in the display's framebuffer by the time this is called — see
// render_engine.h), pushes it with a full refresh, arms a timer wake for
// wakeIntervalMin minutes from now, and enters deep sleep. Does not return
// — the chip resets on wake.
[[noreturn]] void enterDeepSleep(EInkDisplay& display, int wakeIntervalMin);
#endif

}  // namespace transit
