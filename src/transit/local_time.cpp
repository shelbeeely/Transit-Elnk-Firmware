// Local time and GTFS service days — see include/transit/local_time.h for
// why this exists and why it leans on the C library's POSIX TZ support
// instead of hand-rolling DST.

#include "transit/local_time.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace transit {

namespace {

std::tm tmForDateAtHour(int32_t date, int hour) {
  std::tm t{};
  t.tm_year = date / 10000 - 1900;
  t.tm_mon = (date / 100) % 100 - 1;
  t.tm_mday = date % 100;
  t.tm_hour = hour;
  t.tm_min = 0;
  t.tm_sec = 0;
  // tm_isdst = -1 asks mktime() to work out whether DST applies. That is
  // the whole reason this goes through mktime() rather than arithmetic on
  // an epoch: on the two transition days only the C library's own rule
  // evaluation gets the offset right.
  t.tm_isdst = -1;
  return t;
}

int32_t dateFromTm(const std::tm& t) {
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

// The instant GTFS measures a service day's times from: local noon minus
// 12 hours, NOT local midnight.
//
// The distinction only bites twice a year, and it is exactly why the spec
// is worded that way. On an ordinary day the two are identical. On the
// spring-forward day the calendar day is 23 hours long, so midnight plus a
// flat 20 hours lands at 21:00 local -- an hour late -- while noon minus
// 12 hours plus that same flat 20 hours lands at 20:00, which is what the
// timetable printed and what the rider expects. The fall-back day is the
// mirror image. Anchoring on noon (safely clear of both transitions, which
// happen in the small hours) and stepping back 12 real hours is what makes
// a flat seconds-of-day offset preserve wall-clock time year-round.
int64_t serviceDayReference(int32_t date) {
  if (date <= 0) return 0;
  std::tm noonTm = tmForDateAtHour(date, 12);
  const std::time_t noon = mktime(&noonTm);
  if (noon == static_cast<std::time_t>(-1)) return 0;
  return static_cast<int64_t>(noon) - 12 * 3600;
}

}  // namespace

void applyTimezone(const std::string& posixTz) {
  // An empty setting means "never configured," not "run on UTC" -- leaving
  // the board eight hours off is a far worse default than assuming the
  // timezone this firmware was written for.
  const std::string tz = posixTz.empty() ? std::string(kDefaultPosixTz) : posixTz;
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

ServiceDay serviceDayFor(int64_t epoch) {
  ServiceDay day;
  if (epoch <= 0) return day;

  const std::time_t t = static_cast<std::time_t>(epoch);
  std::tm local{};
  localtime_r(&t, &local);

  day.date = dateFromTm(local);
  day.weekday = local.tm_wday;
  // Measured from the GTFS reference, not from wall-clock midnight, so it
  // is directly comparable against a stop_times value without a second
  // correction. Identical to hour*3600+min*60+sec on all but two days a
  // year -- see serviceDayReference().
  const int64_t reference = serviceDayReference(day.date);
  day.secondsOfDay = reference > 0 ? static_cast<int32_t>(epoch - reference) : 0;
  return day;
}

std::vector<ServiceDay> serviceDayCandidates(int64_t epoch) {
  std::vector<ServiceDay> days;
  if (epoch <= 0) return days;

  const ServiceDay today = serviceDayFor(epoch);
  days.push_back(today);

  // Early morning: yesterday's service day may still be running trips it
  // schedules past 24:00. Express the same instant as an offset into that
  // day instead, so a lookup keyed on seconds-of-day can find them.
  if (today.secondsOfDay < kAfterMidnightCutoffSec) {
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm local{};
    localtime_r(&t, &local);

    // Step back a day through mktime() rather than subtracting 86400 from
    // the epoch: on a DST transition the previous local date is not
    // exactly 24 hours earlier, and getting that wrong here would look up
    // the wrong date once or twice a year.
    std::tm yesterdayTm = local;
    yesterdayTm.tm_mday -= 1;
    yesterdayTm.tm_hour = 12;  // midday, safely clear of either transition
    yesterdayTm.tm_min = 0;
    yesterdayTm.tm_sec = 0;
    yesterdayTm.tm_isdst = -1;
    const std::time_t yesterdayNoon = mktime(&yesterdayTm);
    if (yesterdayNoon != static_cast<std::time_t>(-1)) {
      std::tm normalized{};
      localtime_r(&yesterdayNoon, &normalized);

      ServiceDay yesterday;
      yesterday.date = dateFromTm(normalized);
      yesterday.weekday = normalized.tm_wday;

      // How far `epoch` is past yesterday's GTFS reference -- necessarily
      // more than 86400, which is exactly the form GTFS uses for these.
      const int64_t reference = serviceDayReference(yesterday.date);
      if (reference > 0) {
        yesterday.secondsOfDay = static_cast<int32_t>(epoch - reference);
        days.push_back(yesterday);
      }
    }
  }

  return days;
}

int64_t serviceDaySecondsToEpoch(const ServiceDay& day, int32_t secondsOfDay) {
  const int64_t reference = serviceDayReference(day.date);
  if (reference <= 0) return 0;
  return reference + secondsOfDay;
}

std::string formatYyyymmdd(int32_t date) {
  if (date <= 0) return std::string();
  static const char* const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const int year = date / 10000;
  const int month = (date / 100) % 100;
  const int dayOfMonth = date % 100;
  if (month < 1 || month > 12) return std::string();

  char buf[20];
  snprintf(buf, sizeof(buf), "%s %d, %d", kMonths[month - 1], dayOfMonth, year);
  return std::string(buf);
}

}  // namespace transit
