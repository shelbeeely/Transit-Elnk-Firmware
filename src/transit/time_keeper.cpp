// Approximate wall clock across deep sleep — see include/transit/time_keeper.h
// for the contract and the honest accuracy discussion.

#include "transit/time_keeper.h"

#ifndef FREEINK_HOST_NATIVE
#include <esp_attr.h>
#include <esp_sleep.h>
#endif

namespace transit {

namespace {

// Seconds of sleep this wake is accounting for: what the previous wake armed
// its timer for. Shared by the estimate and the error bound so the two can
// never disagree about how long the board was out.
int64_t plannedSleepSeconds(const ApproxClockState& state) {
  if (state.plannedSleepMin <= 0) return 0;
  return static_cast<int64_t>(state.plannedSleepMin) * 60;
}

}  // namespace

int64_t estimateNowEpoch(const ApproxClockState& state, uint32_t awakeMillis) {
  if (!state.valid || state.epochAtSleep <= 0) return 0;
  return state.epochAtSleep + plannedSleepSeconds(state) + static_cast<int64_t>(awakeMillis / 1000);
}

int64_t approximateClockErrorSeconds(const ApproxClockState& state, uint32_t awakeMillis) {
  if (!state.valid) return 0;
  // Time spent awake is measured by esp_timer off the main crystal, which is
  // orders of magnitude better than the sleep timer and not worth budgeting
  // for; only slept seconds carry error. secondsSinceSync covers every
  // earlier offline wake, plannedSleepSeconds() the one just finished.
  const int64_t sleptSinceSync = state.secondsSinceSync + plannedSleepSeconds(state);
  (void)awakeMillis;
  return (sleptSinceSync * kSleepTimerErrorPermille) / 1000;
}

bool approximateClockUsable(const ApproxClockState& state, uint32_t awakeMillis) {
  if (!state.valid || state.epochAtSleep <= 0) return false;
  return approximateClockErrorSeconds(state, awakeMillis) <= kMaxUsableErrorSec;
}

ApproxClockState nextClockState(const ApproxClockState& previous, int64_t epochAtSleepEntry,
                                int sleepMin, bool fromRealSync) {
  ApproxClockState next;
  if (epochAtSleepEntry <= 0) {
    // No usable time this wake at all (offline, and nothing carried over):
    // persisting a zero epoch would just hand the next wake a confidently
    // wrong "1970" clock, so leave the state invalid instead.
    return next;
  }

  next.valid = true;
  next.epochAtSleep = epochAtSleepEntry;
  next.plannedSleepMin = sleepMin > 0 ? sleepMin : 0;

  if (fromRealSync) {
    // A real sync resets the whole error accumulation — whatever drift had
    // built up is gone, because the epoch is authoritative again.
    next.wakesSinceSync = 0;
    next.secondsSinceSync = 0;
    return next;
  }

  next.wakesSinceSync = previous.valid ? previous.wakesSinceSync + 1 : 1;
  // Only *slept* seconds go into the running total: that's the part the
  // sleep timer measured, and so the only part carrying its error. Awake
  // seconds are measured off the main crystal and are already folded into
  // epochAtSleepEntry exactly by the caller, so charging them the sleep
  // timer's error rate would overstate the bound.
  next.secondsSinceSync = (previous.valid ? previous.secondsSinceSync : 0) + plannedSleepSeconds(previous);
  return next;
}

#ifndef FREEINK_HOST_NATIVE

namespace {

// Bumped if the persisted layout ever changes, so a firmware update that
// reshapes these fields reads as "no clock carried over" instead of
// misinterpreting the previous layout's bytes.
constexpr uint32_t kClockMagic = 0x54494D31;  // "TIM1"

// RTC fast memory (the only RTC memory an ESP32-C3 has). Contents survive
// deep sleep; a power-on reset re-initializes them from the image, which is
// exactly what makes the magic check below a reliable cold-boot detector.
RTC_DATA_ATTR uint32_t g_clockMagic = 0;
RTC_DATA_ATTR int64_t g_epochAtSleep = 0;
RTC_DATA_ATTR int32_t g_plannedSleepMin = 0;
RTC_DATA_ATTR int32_t g_wakesSinceSync = 0;
RTC_DATA_ATTR int64_t g_secondsSinceSync = 0;

}  // namespace

ApproxClockState loadApproxClock() {
  ApproxClockState state;
  // Belt and braces: the magic alone would survive a reset that isn't a
  // sleep wake on some reset causes, and a state carried across anything but
  // a timer wake has an unknown amount of unaccounted-for time in it.
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) return state;
  if (g_clockMagic != kClockMagic) return state;
  if (g_epochAtSleep <= 0) return state;

  state.valid = true;
  state.epochAtSleep = g_epochAtSleep;
  state.plannedSleepMin = g_plannedSleepMin;
  state.wakesSinceSync = g_wakesSinceSync;
  state.secondsSinceSync = g_secondsSinceSync;
  return state;
}

void saveApproxClock(const ApproxClockState& state) {
  if (!state.valid) {
    g_clockMagic = 0;
    return;
  }
  g_clockMagic = kClockMagic;
  g_epochAtSleep = state.epochAtSleep;
  g_plannedSleepMin = state.plannedSleepMin;
  g_wakesSinceSync = state.wakesSinceSync;
  g_secondsSinceSync = state.secondsSinceSync;
}

#endif  // FREEINK_HOST_NATIVE

}  // namespace transit
