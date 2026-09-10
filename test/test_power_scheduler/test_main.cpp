// Host-side test for power_scheduler's pure computeNextWakeIntervalMin
// (enterDeepSleep and readBatteryPercent are hardware-only, not testable
// here — see FREEINK_HOST_NATIVE guards in include/transit/power_scheduler.h).

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

// --- refreshIntervalMin clamp -----------------------------------------------

void test_refresh_interval_below_one_falls_back_to_sixty() {
  transit::SleepWindow window;  // disabled, so the fallback is returned as-is
  int result = transit::computeNextWakeIntervalMin(0, window, /*nowMinOfDay=*/120);
  TEST_ASSERT_EQUAL_INT(60, result);

  int negativeResult = transit::computeNextWakeIntervalMin(-5, window, /*nowMinOfDay=*/120);
  TEST_ASSERT_EQUAL_INT(60, negativeResult);
}

void test_refresh_interval_below_one_falls_back_inside_window_too() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 1 * 60;
  window.endMinOfDay = 23 * 60;  // wide non-wrapping window, no clamp interference
  // Falls back to 60, then stretched 3x = 180 (well inside the window).
  int result = transit::computeNextWakeIntervalMin(0, window, /*nowMinOfDay=*/12 * 60);
  TEST_ASSERT_EQUAL_INT(180, result);
}

// --- non-wrapping window -----------------------------------------------------

void test_non_wrapping_window_stretches_when_inside() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 8 * 60;   // 08:00
  window.endMinOfDay = 20 * 60;    // 20:00 — same-day window, no midnight wrap
  int result = transit::computeNextWakeIntervalMin(30, window, /*nowMinOfDay=*/9 * 60);
  // 30 * 3 = 90, minutes until 20:00 from 09:00 is 660 — no clamp needed.
  TEST_ASSERT_EQUAL_INT(90, result);
}

void test_non_wrapping_window_unchanged_when_outside() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 8 * 60;
  window.endMinOfDay = 20 * 60;
  int result = transit::computeNextWakeIntervalMin(30, window, /*nowMinOfDay=*/21 * 60);
  TEST_ASSERT_EQUAL_INT(30, result);
}

// --- boundary minutes ---------------------------------------------------------

void test_window_start_boundary_minute_is_inside_window() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 8 * 60;
  window.endMinOfDay = 10 * 60;
  // Exactly the start minute counts as inside (>=start).
  int result = transit::computeNextWakeIntervalMin(30, window, /*nowMinOfDay=*/8 * 60);
  TEST_ASSERT_GREATER_THAN_INT(30, result);
}

void test_window_end_boundary_minute_is_outside_window() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 8 * 60;
  window.endMinOfDay = 10 * 60;
  // Exactly the end minute counts as outside (<end, not <=end) — the window
  // is a half-open [start, end) interval.
  int result = transit::computeNextWakeIntervalMin(30, window, /*nowMinOfDay=*/10 * 60);
  TEST_ASSERT_EQUAL_INT(30, result);
}

void test_wrapping_window_start_boundary_minute_is_inside_window() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 22 * 60;
  window.endMinOfDay = 6 * 60;
  int result = transit::computeNextWakeIntervalMin(60, window, /*nowMinOfDay=*/22 * 60);
  TEST_ASSERT_GREATER_THAN_INT(60, result);
}

void test_wrapping_window_end_boundary_minute_is_outside_window() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 22 * 60;
  window.endMinOfDay = 6 * 60;
  int result = transit::computeNextWakeIntervalMin(60, window, /*nowMinOfDay=*/6 * 60);
  TEST_ASSERT_EQUAL_INT(60, result);
}

// --- window-end clamp (this unit's judgment call, see PR description) -------
//
// A flat 3x stretch woken late in a long sleep window can land well past
// sleep_window_end, oversleeping into hours the board should be refreshing
// normally again. computeNextWakeIntervalMin instead clamps the stretched
// interval to the minutes remaining until the window closes, so the device
// never oversleeps past the window boundary.

void test_stretch_is_clamped_to_window_end_wrapping() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 22 * 60;  // 22:00
  window.endMinOfDay = 6 * 60;     // 06:00 (wraps)
  // 05:50 — only 10 minutes left in the window. A naive 60*3=180 would sleep
  // until 08:50, nearly 3 hours past the 06:00 window close.
  int result = transit::computeNextWakeIntervalMin(60, window, /*nowMinOfDay=*/5 * 60 + 50);
  TEST_ASSERT_EQUAL_INT(10, result);
}

void test_stretch_is_clamped_to_window_end_non_wrapping() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 8 * 60;   // 08:00
  window.endMinOfDay = 9 * 60;     // 09:00 — a short one-hour window
  // 08:55 — only 5 minutes left. Naive 30*3=90 would badly overshoot.
  int result = transit::computeNextWakeIntervalMin(30, window, /*nowMinOfDay=*/8 * 60 + 55);
  TEST_ASSERT_EQUAL_INT(5, result);
}

void test_stretch_is_not_clamped_early_in_a_long_window() {
  transit::SleepWindow window;
  window.enabled = true;
  window.startMinOfDay = 22 * 60;
  window.endMinOfDay = 6 * 60;
  // Right at window open — the full 3x stretch fits comfortably before 06:00.
  int result = transit::computeNextWakeIntervalMin(60, window, /*nowMinOfDay=*/22 * 60);
  TEST_ASSERT_EQUAL_INT(180, result);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_no_sleep_window_returns_refresh_interval_unchanged);
  RUN_TEST(test_inside_sleep_window_stretches_interval);
  RUN_TEST(test_outside_sleep_window_is_unchanged);
  RUN_TEST(test_refresh_interval_below_one_falls_back_to_sixty);
  RUN_TEST(test_refresh_interval_below_one_falls_back_inside_window_too);
  RUN_TEST(test_non_wrapping_window_stretches_when_inside);
  RUN_TEST(test_non_wrapping_window_unchanged_when_outside);
  RUN_TEST(test_window_start_boundary_minute_is_inside_window);
  RUN_TEST(test_window_end_boundary_minute_is_outside_window);
  RUN_TEST(test_wrapping_window_start_boundary_minute_is_inside_window);
  RUN_TEST(test_wrapping_window_end_boundary_minute_is_outside_window);
  RUN_TEST(test_stretch_is_clamped_to_window_end_wrapping);
  RUN_TEST(test_stretch_is_clamped_to_window_end_non_wrapping);
  RUN_TEST(test_stretch_is_not_clamped_early_in_a_long_window);
  return UNITY_END();
}
