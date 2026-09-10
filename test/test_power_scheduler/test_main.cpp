// Host-side test for power_scheduler's pure computeNextWakeIntervalMin
// (enterDeepSleep is hardware-only, not testable here — see
// FREEINK_HOST_NATIVE guards in include/transit/power_scheduler.h).

#include <unity.h>

#include "transit/power_scheduler.h"

void test_no_sleep_window_returns_refresh_interval_unchanged() {
  transit::SleepWindow window;  // disabled
  int result = transit::computeNextWakeIntervalMin(60, window, /*nowMinOfDay=*/120);
  TEST_ASSERT_EQUAL_INT(60, result);
}

void test_inside_sleep_window_stretches_interval() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 22 * 60;  // 22:00
  window.endMinOfDay = 6 * 60;     // 06:00 (wraps past midnight)
  int result = transit::computeNextWakeIntervalMin(60, window, /*nowMinOfDay=*/23 * 60);
  TEST_ASSERT_GREATER_THAN_INT(60, result);
}

void test_outside_sleep_window_is_unchanged() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 22 * 60;
  window.endMinOfDay = 6 * 60;
  int result = transit::computeNextWakeIntervalMin(60, window, /*nowMinOfDay=*/12 * 60);
  TEST_ASSERT_EQUAL_INT(60, result);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_no_sleep_window_returns_refresh_interval_unchanged);
  RUN_TEST(test_inside_sleep_window_stretches_interval);
  RUN_TEST(test_outside_sleep_window_is_unchanged);
  return UNITY_END();
}
