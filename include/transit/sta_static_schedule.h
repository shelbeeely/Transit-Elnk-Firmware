#pragma once

// Transit-Elnk-Firmware — the static STA timetable, read off the SD card.
//
// This is the difference between a board that degrades gracefully offline
// and one that is actually useful offline. offline_cache.h keeps the last
// *fetched* departures, which is a fallback measured in hours and empties
// itself as those times pass. This answers "when is the next bus" from
// STA's published schedule, so it works on a cold boot, on a dead network,
// and a week into a trip -- for the whole service day, not just the next
// few minutes.
//
// What it is not: real time. There are no delays, no cancellations, no
// "running 6 minutes late" here. A scheduled departure is what the
// timetable promises, and the board must say so rather than letting it
// pass for live data -- see render_engine.h's DepartureSource.
//
// Requires a version-2 card (sta_gtfs_binary.h's TableHeader::version):
// trips.bin's serviceIndex byte was reserved-and-zero before the timetable
// existed, so a version-1 card would make every trip look like service 0.
// That is a confidently wrong answer, which is worse than no answer, so
// this refuses outright.
//
// Hardware-independent: everything here goes through BinaryTableReader, so
// the whole lookup -- calendar logic, after-midnight service days, the
// binary searches -- is unit-tested under [env:native] against in-memory
// tables. sta_sd_store.h supplies the real SD-backed readers.

#include <cstdint>
#include <string>
#include <vector>

#include "transit/local_time.h"
#include "transit/sta_gtfs_binary.h"

namespace transit {
namespace sta {

// One departure the timetable promises, resolved to everything the board
// needs to draw it.
struct ScheduledDeparture {
  uint32_t tripId = 0;
  uint32_t routeId = 0;
  uint8_t directionId = 0;
  std::string routeShortName;
  std::string headsign;
  // Real epoch, converted from the service day's seconds-of-day, so it is
  // directly comparable against live departures and against "now".
  int64_t departureEpoch = 0;
};

// The window the loaded feed actually covers. Outside it the timetable is
// not merely stale but wrong -- service has changed -- so the board warns
// instead of printing times for trips that no longer run. STA publishes
// roughly three service changes a year.
struct FeedValidity {
  bool known = false;
  int32_t startDate = 0;  // YYYYMMDD, inclusive
  int32_t endDate = 0;    // YYYYMMDD, inclusive
};

// The five tables a schedule lookup needs. Null members mean "not
// available" (no card, missing file, failed mount); every function here
// degrades to an empty result rather than failing loudly, matching
// sta_client.h's established "a failure here is just nothing to report"
// convention. calendarDates may legitimately be null even on a good card
// -- a feed with no holiday exceptions simply has none.
struct StaticScheduleTables {
  BinaryTableReader* stopTimes = nullptr;
  BinaryTableReader* trips = nullptr;
  BinaryTableReader* routes = nullptr;
  BinaryTableReader* calendar = nullptr;
  BinaryTableReader* calendarDates = nullptr;
};

// Earliest start_date and latest end_date across calendar.bin.
FeedValidity readFeedValidity(BinaryTableReader& calendar);

// Whether `date` (YYYYMMDD) falls inside the feed's window. An unknown
// validity returns true: the board should not refuse to show a schedule
// just because it couldn't read the window, only when it positively knows
// the window has passed.
bool feedCoversDate(const FeedValidity& validity, int32_t date);

// Service indexes running on `day`, from calendar.bin's weekday mask and
// date range, with calendar_dates.bin's exceptions applied on top (type 1
// adds a service to that date, type 2 removes it). The exceptions are what
// make holidays come out right -- without them the board shows a full
// weekday timetable on Thanksgiving.
std::vector<uint8_t> activeServices(const StaticScheduleTables& tables, const ServiceDay& day);

// Upper bound on stop_times rows examined for one lookup. The busiest STA
// stop has ~1,025 rows and a normal one ~97, so this is generous; it
// exists so a corrupt or unexpectedly huge table can't turn one wake into
// an unbounded scan on battery power.
constexpr int kMaxStopTimeRowsScanned = 400;

// The next `maxCount` scheduled departures at `stopCode`, at or after each
// candidate service day's own secondsOfDay, merged across days and sorted
// by actual departure time.
//
// `days` comes from local_time.h's serviceDayCandidates(), which is what
// makes the small hours correct: at 00:15 a bus STA schedules as "24:45:00
// yesterday" is still upcoming, and a naive same-calendar-day lookup would
// miss exactly the last bus of the night.
std::vector<ScheduledDeparture> nextScheduledDepartures(const StaticScheduleTables& tables,
                                                        uint32_t stopCode,
                                                        const std::vector<ServiceDay>& days,
                                                        int maxCount);

}  // namespace sta
}  // namespace transit
