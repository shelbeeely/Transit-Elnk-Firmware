#pragma once

// Transit-Elnk-Firmware — local time and GTFS service days.
//
// Until this existed the firmware ran entirely on UTC: main.cpp calls
// configTime(0, 0, ...), so localtime_r() returned UTC and
// minutesSinceLocalMidnight() was really minutes-since-UTC-midnight. That
// was survivable while the only consumer was the sleep window (a few hours
// of skew on when the board slows down overnight), but it is fatal to a
// static timetable: GTFS times are local service-day times, so an 8-hour
// offset would select the wrong day's service and compare departures
// against the wrong "now."
//
// The fix is the standard ESP32/newlib one -- a POSIX TZ string plus
// tzset() -- rather than hand-rolled DST arithmetic. "PST8PDT,M3.2.0,M11.1.0"
// encodes the whole rule (base offset, DST offset, and the exact transition
// instants) in a form the C library already implements correctly, including
// the awkward parts: the spring-forward hour that doesn't exist and the
// fall-back hour that happens twice. Hand-rolling that is a classic source
// of once-a-year bugs on exactly the two days it matters most.
//
// GTFS service days, and why they aren't calendar days: stop_times.txt
// expresses a departure as seconds since *noon minus 12 hours* on the day
// the trip started, which lets a trip that leaves at 00:30 belong to the
// previous service day as "24:30:00". STA's own feed goes up to 25:10:00
// (confirmed against the real feed), so a board asking "what's next at
// 00:15 on Saturday" must consider both Saturday's early trips and
// Friday's after-midnight ones. serviceDayCandidates() below exists
// entirely to make that case correct rather than silently missing the last
// bus of the night.
//
// Host-testable: newlib on the ESP32 and glibc on the host implement the
// same POSIX TZ interface, so these functions are exercised for real under
// [env:native] rather than mocked.

#include <cstdint>
#include <string>
#include <vector>

namespace transit {

// Spokane. Pacific time with US DST rules: PST (UTC-8) outside DST, PDT
// (UTC-7) from the second Sunday in March to the first Sunday in November.
// The default rather than a hardcode -- ConfigStore::timezone() can hold
// any POSIX TZ string, so a board somewhere else just needs the setting
// changed, not a rebuild.
constexpr char kDefaultPosixTz[] = "PST8PDT,M3.2.0,M11.1.0";

// Installs `posixTz` as the process timezone (setenv("TZ")+tzset()), so
// every subsequent localtime_r()/mktime() in this firmware resolves local
// time correctly. Call once at boot, before any time is formatted or any
// service day is computed. An empty string installs kDefaultPosixTz rather
// than leaving the process on UTC, since "no timezone configured" should
// not silently mean "eight hours wrong".
void applyTimezone(const std::string& posixTz);

// A GTFS service day: the calendar date a trip is considered to have
// started on, plus how far into that service day a given instant falls.
struct ServiceDay {
  // Local calendar date as the integer YYYYMMDD -- the same shape
  // calendar.txt's start_date/end_date and calendar_dates.txt's date use,
  // so comparisons need no conversion.
  int32_t date = 0;
  // 0 = Sunday .. 6 = Saturday, matching struct tm's tm_wday and the
  // column order of calendar.txt (which lists monday first but is indexed
  // here by weekday number, not column position).
  int weekday = 0;
  // Seconds since this service day's 00:00 local. May exceed 86400 when
  // the instant belongs to the previous service day's after-midnight
  // trips -- see serviceDayCandidates().
  int32_t secondsOfDay = 0;
};

// The service day `epoch` falls in, by local calendar date. Pure
// wall-clock conversion: no after-midnight reasoning, which is
// serviceDayCandidates()'s job.
ServiceDay serviceDayFor(int64_t epoch);

// Every service day that could legitimately contain a departure happening
// at `epoch`, most-likely first.
//
// Ordinarily that's just today. Between local midnight and
// kAfterMidnightCutoffSec, it's today *and* yesterday-with-secondsOfDay-
// past-86400, because a trip STA schedules as "24:45:00 Friday" really
// departs at 00:45 Saturday and would be invisible to a naive
// same-calendar-day lookup. Callers merge the results and sort by time.
std::vector<ServiceDay> serviceDayCandidates(int64_t epoch);

// How far past midnight the previous service day is still worth
// consulting. STA's feed tops out at 25:10:00, so 3 hours covers it with
// room to spare while keeping the common daytime lookup to a single
// service day.
constexpr int32_t kAfterMidnightCutoffSec = 3 * 3600;

// Converts a service day plus a GTFS seconds-of-day (which may exceed
// 86400) back into a real epoch, so a scheduled departure can be compared
// against, and rendered alongside, live times. Handles DST correctly by
// going through mktime() with tm_isdst = -1 ("work it out"), which is what
// makes a 25:10 departure on a spring-forward night land on the right
// instant.
int64_t serviceDaySecondsToEpoch(const ServiceDay& day, int32_t secondsOfDay);

// Formats YYYYMMDD as a date a person reads ("Sep 19, 2026"), for the
// feed-expiry warning. ASCII only, like every on-device string -- the
// bundled Noto Sans subset renders anything else as a tofu box.
std::string formatYyyymmdd(int32_t date);

}  // namespace transit
