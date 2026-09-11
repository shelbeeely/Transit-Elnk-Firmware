// Host-side tests for time_keeper.h's approximate-clock math.
//
// Only the pure half is exercised here: loadApproxClock()/saveApproxClock()
// touch RTC memory and are excluded from [env:native] by the same
// FREEINK_HOST_NATIVE guard power_scheduler.cpp uses. What matters most is
// that the error bound accumulates across offline wakes and that the module
// stops vouching for the clock once that bound gets wide enough to make a
// minutes-until-departure countdown misleading.

#include <unity.h>

#include "transit/time_keeper.h"

using transit::ApproxClockState;
using transit::approximateClockErrorSeconds;
using transit::approximateClockUsable;
using transit::estimateNowEpoch;
using transit::kMaxUsableErrorSec;
using transit::nextClockState;

namespace {

constexpr int64_t kNow = 1'700'000'000;

ApproxClockState syncedState(int64_t epochAtSleep, int plannedSleepMin) {
  ApproxClockState state;
  state.valid = true;
  state.epochAtSleep = epochAtSleep;
  state.plannedSleepMin = plannedSleepMin;
  state.wakesSinceSync = 0;
  state.secondsSinceSync = 0;
  return state;
}

void test_invalid_state_yields_no_clock() {
  ApproxClockState state;  // valid = false
  TEST_ASSERT_EQUAL_INT64(0, estimateNowEpoch(state, 5000));
  TEST_ASSERT_FALSE(approximateClockUsable(state, 5000));
  TEST_ASSERT_EQUAL_INT64(0, approximateClockErrorSeconds(state, 5000));
}

void test_estimate_adds_planned_sleep_and_uptime() {
  const ApproxClockState state = syncedState(kNow, 60);
  // 60 minutes slept, then 12 seconds awake before this call.
  TEST_ASSERT_EQUAL_INT64(kNow + 60 * 60 + 12, estimateNowEpoch(state, 12'400));
}

void test_state_persisted_right_after_a_sync_has_zero_error() {
  const ApproxClockState state = syncedState(kNow, 0);
  TEST_ASSERT_EQUAL_INT64(0, approximateClockErrorSeconds(state, 0));
  TEST_ASSERT_TRUE(approximateClockUsable(state, 0));
}

void test_error_budget_is_one_percent_of_slept_time() {
  // One 60-minute sleep since the last sync: 3600s * 1% = 36s of budget.
  const ApproxClockState state = syncedState(kNow, 60);
  TEST_ASSERT_EQUAL_INT64(36, approximateClockErrorSeconds(state, 0));
  TEST_ASSERT_TRUE(approximateClockUsable(state, 0));
}

void test_error_accumulates_across_consecutive_offline_wakes() {
  // Start synced, then go offline for three hourly wakes in a row. Each
  // one's sleep has to be added to the running total -- an error bound that
  // reset every wake would let the clock drift indefinitely while still
  // claiming to be accurate.
  ApproxClockState state = syncedState(kNow, 60);
  int64_t epoch = kNow;
  for (int wake = 0; wake < 3; ++wake) {
    epoch = estimateNowEpoch(state, 0);
    state = nextClockState(state, epoch, 60, /*fromRealSync=*/false);
  }

  TEST_ASSERT_EQUAL_INT32(3, state.wakesSinceSync);
  // Three completed hours of sleep are behind us, and the state is armed
  // for a fourth: 4 * 3600 * 1% = 144s.
  TEST_ASSERT_EQUAL_INT64(144, approximateClockErrorSeconds(state, 0));
  TEST_ASSERT_TRUE(approximateClockUsable(state, 0));
}

void test_clock_stops_being_usable_once_the_bound_is_too_wide() {
  ApproxClockState state = syncedState(kNow, 60);
  int wakes = 0;
  while (approximateClockUsable(state, 0) && wakes < 500) {
    state = nextClockState(state, estimateNowEpoch(state, 0), 60, /*fromRealSync=*/false);
    ++wakes;
  }

  TEST_ASSERT_LESS_THAN_INT(500, wakes);  // it must actually give up
  TEST_ASSERT_FALSE(approximateClockUsable(state, 0));
  TEST_ASSERT_GREATER_THAN_INT64(kMaxUsableErrorSec, approximateClockErrorSeconds(state, 0));
  // At 1% of an hour per wake (36s), 15 minutes of budget is ~25 wakes --
  // roughly a day offline at the default hourly refresh. Pinned loosely so
  // the test documents the order of magnitude without breaking on a
  // one-wake change to the accounting.
  TEST_ASSERT_GREATER_THAN_INT(20, wakes);
  TEST_ASSERT_LESS_THAN_INT(32, wakes);
}

void test_a_real_sync_resets_the_accumulated_error() {
  ApproxClockState state = syncedState(kNow, 60);
  for (int wake = 0; wake < 10; ++wake) {
    state = nextClockState(state, estimateNowEpoch(state, 0), 60, /*fromRealSync=*/false);
  }
  TEST_ASSERT_GREATER_THAN_INT64(0, approximateClockErrorSeconds(state, 0));

  // Back in range: SNTP answers, and whatever drift had built up is gone.
  const ApproxClockState resynced = nextClockState(state, kNow + 99'999, 60, /*fromRealSync=*/true);
  TEST_ASSERT_EQUAL_INT32(0, resynced.wakesSinceSync);
  TEST_ASSERT_EQUAL_INT64(0, resynced.secondsSinceSync);
  TEST_ASSERT_EQUAL_INT64(kNow + 99'999, resynced.epochAtSleep);
}

void test_no_clock_this_wake_persists_nothing() {
  // Offline, and nothing carried over either -- persisting nowEpoch = 0
  // would hand the next wake a confident "1970", which is worse than
  // admitting the clock is unknown.
  const ApproxClockState state = nextClockState(ApproxClockState{}, 0, 60, /*fromRealSync=*/false);
  TEST_ASSERT_FALSE(state.valid);
  TEST_ASSERT_EQUAL_INT64(0, estimateNowEpoch(state, 0));
}

void test_uptime_counts_toward_the_next_state_but_not_the_error_bound() {
  // Awake time is measured off the main crystal, not the sleep timer, so it
  // advances the clock without widening the error bound.
  const ApproxClockState state = syncedState(kNow, 30);
  const int64_t errorBefore = approximateClockErrorSeconds(state, 0);
  TEST_ASSERT_EQUAL_INT64(errorBefore, approximateClockErrorSeconds(state, 45'000));

  // The caller is responsible for carrying the epoch forward to the moment
  // of sleep entry -- that's what closes the awake-time gap. Here that's
  // 45 seconds of uptime, which lands in epochAtSleep exactly...
  const ApproxClockState next = nextClockState(state, estimateNowEpoch(state, 45'000), 30, false);
  TEST_ASSERT_EQUAL_INT64(kNow + 30 * 60 + 45, next.epochAtSleep);
  // ...and only the 30 minutes actually slept are charged the sleep
  // timer's error rate.
  TEST_ASSERT_EQUAL_INT64(30 * 60, next.secondsSinceSync);
}

// The awake-time gap, driven end to end: establish the time, spend a while
// awake, sleep, wake, repeat. The estimate must not lose the awake seconds
// on each pass -- dropping them would accumulate a drift the error bound
// never accounts for, which is precisely the failure a bound is supposed
// to rule out.
void test_awake_time_does_not_leak_away_across_repeated_wakes() {
  constexpr uint32_t kAwakeMs = 20'000;  // 20 s of fetch + render per wake
  constexpr int kSleepMin = 30;
  constexpr int kWakes = 12;

  ApproxClockState state = nextClockState(ApproxClockState{}, kNow, kSleepMin, /*fromRealSync=*/true);
  int64_t trueEpoch = kNow;

  for (int wake = 0; wake < kWakes; ++wake) {
    // Real elapsed time: the sleep, then this wake's awake period.
    trueEpoch += kSleepMin * 60 + kAwakeMs / 1000;
    const int64_t estimated = estimateNowEpoch(state, kAwakeMs);
    state = nextClockState(state, estimated, kSleepMin, /*fromRealSync=*/false);
  }

  // With a perfect sleep timer the estimate should track real time exactly;
  // the point of the test is that it doesn't silently fall behind by
  // 20 s per wake (240 s over this run).
  TEST_ASSERT_EQUAL_INT64(trueEpoch, state.epochAtSleep);
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_invalid_state_yields_no_clock);
  RUN_TEST(test_estimate_adds_planned_sleep_and_uptime);
  RUN_TEST(test_state_persisted_right_after_a_sync_has_zero_error);
  RUN_TEST(test_error_budget_is_one_percent_of_slept_time);
  RUN_TEST(test_error_accumulates_across_consecutive_offline_wakes);
  RUN_TEST(test_clock_stops_being_usable_once_the_bound_is_too_wide);
  RUN_TEST(test_a_real_sync_resets_the_accumulated_error);
  RUN_TEST(test_no_clock_this_wake_persists_nothing);
  RUN_TEST(test_uptime_counts_toward_the_next_state_but_not_the_error_bound);
  RUN_TEST(test_awake_time_does_not_leak_away_across_repeated_wakes);
  return UNITY_END();
}
