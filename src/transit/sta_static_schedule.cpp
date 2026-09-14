// Static STA timetable lookup — see include/transit/sta_static_schedule.h
// for the contract and for why a version-1 card is refused outright.

#include "transit/sta_static_schedule.h"

#include <algorithm>

namespace transit {
namespace sta {

namespace {

// Reads a table's header and confirms it's the shape the caller expects
// (right record size, and new enough to carry a serviceIndex). A table
// that fails any of these is treated as absent, not as an error: the
// caller has nothing better to do about it either way.
bool openTable(BinaryTableReader* reader, uint32_t expectedRecordSize, TableHeader& out) {
  if (reader == nullptr) return false;
  if (!readTableHeader(*reader, out)) return false;
  if (out.version < 2) return false;
  if (expectedRecordSize != 0 && out.recordSize != expectedRecordSize) return false;
  return true;
}

// Index of the first record past every record keyed `key` -- i.e. the
// lower bound of key+1. Paired with findFirstRecordAtLeast(key) this
// brackets one stop's whole block of departures, which is what lets the
// time search below be a binary search rather than a linear scan of up to
// a thousand rows.
bool findBlockEnd(BinaryTableReader& reader, const TableHeader& header, uint32_t key,
                  uint32_t& outIndex) {
  if (key == UINT32_MAX) {
    outIndex = header.recordCount;
    return true;
  }
  return findFirstRecordAtLeast(reader, header, key + 1, outIndex);
}

// Within [lo, hi) -- all records for one stop, already sorted ascending by
// departure time -- the first whose departure is at or after
// `afterSeconds`. Returns hi when every departure in the block has passed.
bool findFirstDepartureAtOrAfter(BinaryTableReader& reader, const TableHeader& header, uint32_t lo,
                                 uint32_t hi, int64_t afterSeconds, uint32_t& outIndex) {
  std::vector<uint8_t> record;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (!readRecordAt(reader, header, mid, record)) return false;
    SdStopTime stopTime;
    if (!decodeStopTime(record.data(), stopTime)) return false;
    if (static_cast<int64_t>(stopTime.departureSeconds) < afterSeconds) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  outIndex = lo;
  return true;
}

}  // namespace

FeedValidity readFeedValidity(BinaryTableReader& calendar) {
  FeedValidity validity;
  TableHeader header;
  if (!openTable(&calendar, kCalendarRecordSize, header)) return validity;

  std::vector<uint8_t> record;
  for (uint32_t i = 0; i < header.recordCount; ++i) {
    if (!readRecordAt(calendar, header, i, record)) break;
    SdCalendarEntry entry;
    if (!decodeCalendarEntry(record.data(), entry)) break;
    if (entry.startDate <= 0 || entry.endDate <= 0) continue;
    if (!validity.known) {
      validity.known = true;
      validity.startDate = entry.startDate;
      validity.endDate = entry.endDate;
    } else {
      validity.startDate = std::min(validity.startDate, entry.startDate);
      validity.endDate = std::max(validity.endDate, entry.endDate);
    }
  }
  return validity;
}

bool feedCoversDate(const FeedValidity& validity, int32_t date) {
  // An unknown window is not evidence of expiry. Refusing to show a
  // schedule because the validity couldn't be read would turn a minor
  // problem into a blank board.
  if (!validity.known) return true;
  if (date <= 0) return true;
  return date >= validity.startDate && date <= validity.endDate;
}

std::vector<uint8_t> activeServices(const StaticScheduleTables& tables, const ServiceDay& day) {
  std::vector<uint8_t> active;
  if (day.date <= 0) return active;

  TableHeader calendarHeader;
  if (!openTable(tables.calendar, kCalendarRecordSize, calendarHeader)) return active;

  // calendar.bin is tiny (24 records for STA, one disk sector), so a full
  // scan costs less than any index would.
  std::vector<uint8_t> record;
  const uint8_t weekdayBit = static_cast<uint8_t>(1u << day.weekday);
  for (uint32_t i = 0; i < calendarHeader.recordCount; ++i) {
    if (!readRecordAt(*tables.calendar, calendarHeader, i, record)) break;
    SdCalendarEntry entry;
    if (!decodeCalendarEntry(record.data(), entry)) break;
    if ((entry.daysMask & weekdayBit) == 0) continue;
    if (day.date < entry.startDate || day.date > entry.endDate) continue;
    if (entry.serviceIndex <= 0xFF) active.push_back(static_cast<uint8_t>(entry.serviceIndex));
  }

  // Exceptions override the weekly pattern, which is how holidays work:
  // Thanksgiving removes the weekday service and adds a Sunday-style one.
  TableHeader exceptionHeader;
  if (openTable(tables.calendarDates, kCalendarDateRecordSize, exceptionHeader)) {
    uint32_t index = 0;
    if (findFirstRecordAtLeast(*tables.calendarDates, exceptionHeader,
                               static_cast<uint32_t>(day.date), index)) {
      for (; index < exceptionHeader.recordCount; ++index) {
        if (!readRecordAt(*tables.calendarDates, exceptionHeader, index, record)) break;
        SdCalendarException exception;
        if (!decodeCalendarException(record.data(), exception)) break;
        if (exception.date != day.date) break;  // past this date's block

        auto it = std::find(active.begin(), active.end(), exception.serviceIndex);
        if (exception.exceptionType == 1) {
          if (it == active.end()) active.push_back(exception.serviceIndex);
        } else if (exception.exceptionType == 2) {
          if (it != active.end()) active.erase(it);
        }
      }
    }
  }

  return active;
}

std::vector<ScheduledDeparture> nextScheduledDepartures(const StaticScheduleTables& tables,
                                                        uint32_t stopCode,
                                                        const std::vector<ServiceDay>& days,
                                                        int maxCount) {
  std::vector<ScheduledDeparture> found;
  if (maxCount <= 0 || days.empty()) return found;

  TableHeader stopTimesHeader;
  if (!openTable(tables.stopTimes, kStopTimeRecordSize, stopTimesHeader)) return found;
  TableHeader tripsHeader;
  if (!openTable(tables.trips, 16, tripsHeader)) return found;

  for (const ServiceDay& day : days) {
    // Per service day, not shared across them: a single counter would let
    // today's block exhaust the cap and leave the yesterday candidate --
    // the only source of after-midnight trips, and so of the last bus of
    // the night -- breaking out after one row.
    int rowsScanned = 0;

    const std::vector<uint8_t> active = activeServices(tables, day);
    if (active.empty()) continue;  // nothing runs on this day at all

    // Bracket this stop's block, then binary-search inside it for the
    // first departure still to come.
    uint32_t blockStart = 0;
    uint32_t blockEnd = 0;
    if (!findFirstRecordAtLeast(*tables.stopTimes, stopTimesHeader, stopCode, blockStart)) continue;
    if (!findBlockEnd(*tables.stopTimes, stopTimesHeader, stopCode, blockEnd)) continue;
    if (blockStart >= blockEnd) continue;  // this stop has no departures at all

    uint32_t cursor = 0;
    if (!findFirstDepartureAtOrAfter(*tables.stopTimes, stopTimesHeader, blockStart, blockEnd,
                                     day.secondsOfDay, cursor)) {
      continue;
    }

    int keptForThisDay = 0;
    std::vector<uint8_t> record;
    for (; cursor < blockEnd && keptForThisDay < maxCount; ++cursor) {
      if (++rowsScanned > kMaxStopTimeRowsScanned) break;
      if (!readRecordAt(*tables.stopTimes, stopTimesHeader, cursor, record)) break;
      SdStopTime stopTime;
      if (!decodeStopTime(record.data(), stopTime)) break;
      if (stopTime.stopCode != stopCode) break;  // defensive; blockEnd should have caught it

      // Most rows in a stop's block belong to services that don't run
      // today (weekday trips on a Sunday, say), so the service filter is
      // what does the real work here.
      SdTripInfo trip;
      if (!lookupSdTrip(*tables.trips, stopTime.tripId, trip)) continue;
      if (std::find(active.begin(), active.end(), trip.serviceIndex) == active.end()) continue;

      ScheduledDeparture departure;
      departure.tripId = trip.tripId;
      departure.routeId = trip.routeId;
      departure.directionId = trip.directionId;
      departure.headsign = trip.headsign;
      departure.departureEpoch =
          serviceDaySecondsToEpoch(day, static_cast<int32_t>(stopTime.departureSeconds));

      // Route short name is cosmetic -- a departure with an unresolvable
      // route is still a real bus, so a failed lookup drops the name
      // rather than the departure.
      SdRouteInfo route;
      if (tables.routes != nullptr && lookupSdRoute(*tables.routes, trip.routeId, route)) {
        departure.routeShortName = route.shortName;
      }

      found.push_back(departure);
      ++keptForThisDay;
    }
  }

  // Merged across service days (at 00:15 that's today's early trips plus
  // yesterday's after-midnight ones), so the order has to come from the
  // real epochs rather than from either day's seconds-of-day.
  std::sort(found.begin(), found.end(),
            [](const ScheduledDeparture& a, const ScheduledDeparture& b) {
              return a.departureEpoch < b.departureEpoch;
            });
  if (static_cast<int>(found.size()) > maxCount) found.resize(static_cast<size_t>(maxCount));
  return found;
}

}  // namespace sta
}  // namespace transit
