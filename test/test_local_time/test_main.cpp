// Host-side tests for local_time.h.
//
// These run against the host C library's real POSIX TZ implementation,
// which is the same interface newlib gives the ESP32 -- so the DST cases
// below are genuinely exercised rather than mocked. The dates are chosen
// deliberately: they are the two days a year hand-rolled DST arithmetic
// gets wrong, plus the after-midnight window where GTFS service days stop
// matching calendar days.

#include <unity.h>

#include <ctime>
#include <string>

#include "transit/local_time.h"

using transit::ServiceDay;
using transit::applyTimezone;
using transit::formatYyyymmdd;
using transit::kDefaultPosixTz;
using transit::serviceDayCandidates;
using transit::serviceDayFor;
using transit::serviceDaySecondsToEpoch;

namespace {

// Builds an epoch from a local Pacific wall-clock time, so the tests read
// as "at 7:30am local, ..." rather than as opaque epoch constants.
int64_t pacific(int year, int month, int day, int hour, int minute) {
  std::tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_isdst = -1;
  return static_cast<int64_t>(mktime(&t));
}

void setUpTz() { applyTimezone(kDefaultPosixTz); }

// True local midnight for a YYYYMMDD date, independent of the module under
// test -- used to show the DST days really are 23/25 hours long, which is
// the fact the GTFS reference has to survive.
int64_t localMidnight(int32_t date) {
  std::tm t{};
  t.tm_year = date / 10000 - 1900;
  t.tm_mon = (date / 100) % 100 - 1;
  t.tm_mday = date % 100;
  t.tm_isdst = -1;
  return static_cast<int64_t>(mktime(&t));
}

void test_service_day_uses_local_date_not_utc() {
  setUpTz();
  // 7:30pm Pacific on Sep 11 is already Sep 12 in UTC. Before this module
  // existed the firmware would have called this Sep 12 and looked up the
  // wrong day's service.
  const ServiceDay day = serviceDayFor(pacific(2026, 9, 11, 19, 30));
  TEST_ASSERT_EQUAL_INT32(20260911, day.date);
  TEST_ASSERT_EQUAL_INT32(19 * 3600 + 30 * 60, day.secondsOfDay);
  TEST_ASSERT_EQUAL_INT(5, day.weekday);  // Friday
}

void test_no_clock_yields_an_empty_service_day() {
  setUpTz();
  const ServiceDay day = serviceDayFor(0);
  TEST_ASSERT_EQUAL_INT32(0, day.date);
  TEST_ASSERT_EQUAL_size_t(0, serviceDayCandidates(0).size());
}

void test_daytime_has_exactly_one_candidate_service_day() {
  setUpTz();
  const auto days = serviceDayCandidates(pacific(2026, 9, 11, 14, 0));
  TEST_ASSERT_EQUAL_size_t(1, days.size());
  TEST_ASSERT_EQUAL_INT32(20260911, days[0].date);
}

void test_after_midnight_also_considers_the_previous_service_day() {
  setUpTz();
  // 00:45 on Saturday Sep 12. A bus STA schedules as "24:45:00 Friday"
  // departs right now, and a naive same-calendar-day lookup would miss it
  // entirely -- the last bus of the night is exactly the one you most need
  // the board to tell you about.
  const auto days = serviceDayCandidates(pacific(2026, 9, 12, 0, 45));
  TEST_ASSERT_EQUAL_size_t(2, days.size());

  TEST_ASSERT_EQUAL_INT32(20260912, days[0].date);
  TEST_ASSERT_EQUAL_INT32(45 * 60, days[0].secondsOfDay);

  TEST_ASSERT_EQUAL_INT32(20260911, days[1].date);
  TEST_ASSERT_EQUAL_INT32(24 * 3600 + 45 * 60, days[1].secondsOfDay);
  TEST_ASSERT_EQUAL_INT(5, days[1].weekday);  // still Friday's service
}

void test_after_midnight_window_closes_at_the_cutoff() {
  setUpTz();
  // 3:30am is past kAfterMidnightCutoffSec: yesterday's trips are long
  // done, and consulting its service day would only add work.
  const auto days = serviceDayCandidates(pacific(2026, 9, 12, 3, 30));
  TEST_ASSERT_EQUAL_size_t(1, days.size());
}

void test_round_trip_through_epoch_and_back() {
  setUpTz();
  const ServiceDay day = serviceDayFor(pacific(2026, 9, 11, 12, 0));
  // 17:42 expressed as seconds of day comes back as the same instant.
  const int32_t secs = 17 * 3600 + 42 * 60;
  const int64_t epoch = serviceDaySecondsToEpoch(day, secs);
  const ServiceDay back = serviceDayFor(epoch);
  TEST_ASSERT_EQUAL_INT32(day.date, back.date);
  TEST_ASSERT_EQUAL_INT32(secs, back.secondsOfDay);
}

void test_after_midnight_seconds_resolve_to_the_right_instant() {
  setUpTz();
  // "24:45:00 on Sep 11" must land on 00:45 Sep 12, not on some 25th hour
  // of Sep 11.
  ServiceDay friday;
  friday.date = 20260911;
  const int64_t epoch = serviceDaySecondsToEpoch(friday, 24 * 3600 + 45 * 60);
  TEST_ASSERT_EQUAL_INT64(pacific(2026, 9, 12, 0, 45), epoch);
}

// --- The two days a year this is easy to get wrong -------------------------

// The two days a year the GTFS "noon minus 12 hours" reference matters.
// A rider reads 8:00pm off a printed timetable and expects a bus at 8:00pm
// local, on the transition day as much as any other. Anchoring on midnight
// instead would put it an hour late in March and an hour early in
// November -- which is the bug these two tests originally caught.

void test_spring_forward_keeps_wall_clock_time() {
  setUpTz();
  // March 8 2026 is the second Sunday in March: 02:00 PST jumps to 03:00
  // PDT, leaving the calendar day 23 hours long.
  ServiceDay springForward;
  springForward.date = 20260308;

  // A trip GTFS schedules as 20:00:00 departs at 20:00 local.
  const int64_t evening = serviceDaySecondsToEpoch(springForward, 20 * 3600);
  std::tm local{};
  const std::time_t t = static_cast<std::time_t>(evening);
  localtime_r(&t, &local);
  TEST_ASSERT_EQUAL_INT(20, local.tm_hour);
  TEST_ASSERT_EQUAL_INT(0, local.tm_min);
  TEST_ASSERT_EQUAL_INT(8, local.tm_mday);

  // Round-tripping through serviceDayFor() agrees, so a live "now" is
  // measured on the same scale a scheduled departure is.
  const ServiceDay back = serviceDayFor(evening);
  TEST_ASSERT_EQUAL_INT32(20260308, back.date);
  TEST_ASSERT_EQUAL_INT32(20 * 3600, back.secondsOfDay);

  // The calendar day really is short -- midnight to midnight is 23 hours...
  TEST_ASSERT_EQUAL_INT64(23 * 3600, localMidnight(20260309) - localMidnight(20260308));
  // ...and yet consecutive GTFS references are exactly 24 hours apart.
  // That invariant is the entire point of the noon-minus-12h anchor: it is
  // what lets a flat seconds-of-day offset mean the same wall-clock time
  // on every day of the year.
  ServiceDay next;
  next.date = 20260309;
  TEST_ASSERT_EQUAL_INT64(
      86400, serviceDaySecondsToEpoch(next, 0) - serviceDaySecondsToEpoch(springForward, 0));
}

void test_fall_back_keeps_wall_clock_time() {
  setUpTz();
  // November 1 2026 is the first Sunday in November: 02:00 PDT falls back
  // to 01:00 PST, so the day runs 25 hours and 01:00-02:00 happens twice.
  ServiceDay fallBack;
  fallBack.date = 20261101;

  const int64_t evening = serviceDaySecondsToEpoch(fallBack, 20 * 3600);
  std::tm local{};
  const std::time_t t = static_cast<std::time_t>(evening);
  localtime_r(&t, &local);
  TEST_ASSERT_EQUAL_INT(20, local.tm_hour);
  TEST_ASSERT_EQUAL_INT(0, local.tm_min);
  TEST_ASSERT_EQUAL_INT(1, local.tm_mday);

  const ServiceDay back = serviceDayFor(evening);
  TEST_ASSERT_EQUAL_INT32(20261101, back.date);
  TEST_ASSERT_EQUAL_INT32(20 * 3600, back.secondsOfDay);

  // 25 hours of wall clock, still exactly 86400 between GTFS references --
  // the mirror image of the spring-forward case above.
  TEST_ASSERT_EQUAL_INT64(25 * 3600, localMidnight(20261102) - localMidnight(20261101));
  ServiceDay next;
  next.date = 20261102;
  TEST_ASSERT_EQUAL_INT64(
      86400, serviceDaySecondsToEpoch(next, 0) - serviceDaySecondsToEpoch(fallBack, 0));
}

// On any ordinary day the GTFS reference and local midnight coincide --
// worth pinning so the noon-anchored implementation can't drift into being
// subtly wrong on the 363 days nobody thinks about.
void test_reference_equals_local_midnight_on_an_ordinary_day() {
  setUpTz();
  ServiceDay day;
  day.date = 20260911;
  const int64_t reference = serviceDaySecondsToEpoch(day, 0);
  std::tm local{};
  const std::time_t t = static_cast<std::time_t>(reference);
  localtime_r(&t, &local);
  TEST_ASSERT_EQUAL_INT(0, local.tm_hour);
  TEST_ASSERT_EQUAL_INT(0, local.tm_min);
  TEST_ASSERT_EQUAL_INT(11, local.tm_mday);
}

void test_after_midnight_across_a_dst_transition_steps_one_calendar_day() {
  setUpTz();
  // 00:30 on the spring-forward morning. The previous service day must
  // come back as Mar 7, not as whatever subtracting a flat 86400 seconds
  // would produce.
  const auto days = serviceDayCandidates(pacific(2026, 3, 8, 0, 30));
  TEST_ASSERT_EQUAL_size_t(2, days.size());
  TEST_ASSERT_EQUAL_INT32(20260308, days[0].date);
  TEST_ASSERT_EQUAL_INT32(20260307, days[1].date);
  TEST_ASSERT_EQUAL_INT32(24 * 3600 + 30 * 60, days[1].secondsOfDay);
}

void test_empty_timezone_falls_back_to_the_default_not_utc() {
  applyTimezone("");
  // Same assertion as the very first test: an unset timezone must not
  // silently leave the board eight hours off.
  const ServiceDay day = serviceDayFor(pacific(2026, 9, 11, 19, 30));
  TEST_ASSERT_EQUAL_INT32(20260911, day.date);
}

void test_a_different_timezone_is_honored() {
  applyTimezone("EST5EDT,M3.2.0,M11.1.0");
  std::tm t{};
  t.tm_year = 126;
  t.tm_mon = 8;
  t.tm_mday = 11;
  t.tm_hour = 23;
  t.tm_min = 30;
  t.tm_isdst = -1;
  const int64_t epoch = static_cast<int64_t>(mktime(&t));
  const ServiceDay day = serviceDayFor(epoch);
  TEST_ASSERT_EQUAL_INT32(20260911, day.date);
  TEST_ASSERT_EQUAL_INT32(23 * 3600 + 30 * 60, day.secondsOfDay);
  setUpTz();
}

void test_format_yyyymmdd() {
  TEST_ASSERT_EQUAL_STRING("Sep 19, 2026", formatYyyymmdd(20260919).c_str());
  TEST_ASSERT_EQUAL_STRING("Jan 1, 2027", formatYyyymmdd(20270101).c_str());
  TEST_ASSERT_EQUAL_STRING("", formatYyyymmdd(0).c_str());
  TEST_ASSERT_EQUAL_STRING("", formatYyyymmdd(20261399).c_str());  // month 13
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_service_day_uses_local_date_not_utc);
  RUN_TEST(test_no_clock_yields_an_empty_service_day);
  RUN_TEST(test_daytime_has_exactly_one_candidate_service_day);
  RUN_TEST(test_after_midnight_also_considers_the_previous_service_day);
  RUN_TEST(test_after_midnight_window_closes_at_the_cutoff);
  RUN_TEST(test_round_trip_through_epoch_and_back);
  RUN_TEST(test_after_midnight_seconds_resolve_to_the_right_instant);
  RUN_TEST(test_spring_forward_keeps_wall_clock_time);
  RUN_TEST(test_fall_back_keeps_wall_clock_time);
  RUN_TEST(test_reference_equals_local_midnight_on_an_ordinary_day);
  RUN_TEST(test_after_midnight_across_a_dst_transition_steps_one_calendar_day);
  RUN_TEST(test_empty_timezone_falls_back_to_the_default_not_utc);
  RUN_TEST(test_a_different_timezone_is_honored);
  RUN_TEST(test_format_yyyymmdd);
  return UNITY_END();
}
