// Host-side tests for sta_static_schedule.h — the offline timetable.
//
// Tables are built in memory in exactly the layout tools/gen_sta_tables.py
// writes, so these exercise the real binary search, the real calendar
// logic and the real service-day handling rather than a simplified model
// of them. The cases that matter most are the ones a naive implementation
// gets wrong: a holiday, the small hours, and a card written before the
// timetable existed.

#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "transit/local_time.h"
#include "transit/sta_static_schedule.h"

using transit::ServiceDay;
using transit::applyTimezone;
using transit::kDefaultPosixTz;
using transit::serviceDayCandidates;
using transit::serviceDayFor;
using transit::sta::BinaryTableReader;
using transit::sta::FeedValidity;
using transit::sta::ScheduledDeparture;
using transit::sta::StaticScheduleTables;
using transit::sta::activeServices;
using transit::sta::feedCoversDate;
using transit::sta::nextScheduledDepartures;
using transit::sta::readFeedValidity;

namespace {

class InMemoryReader : public BinaryTableReader {
 public:
  explicit InMemoryReader(std::vector<uint8_t> data) : data_(std::move(data)) {}
  bool readAt(uint32_t offset, uint8_t* out, size_t len) override {
    if (static_cast<uint64_t>(offset) + len > data_.size()) return false;
    std::memcpy(out, data_.data() + offset, len);
    return true;
  }

 private:
  std::vector<uint8_t> data_;
};

void appendU32(std::vector<uint8_t>& buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v));
  buf.push_back(static_cast<uint8_t>(v >> 8));
  buf.push_back(static_cast<uint8_t>(v >> 16));
  buf.push_back(static_cast<uint8_t>(v >> 24));
}

std::vector<uint8_t> buildTable(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& records,
                                const std::vector<uint8_t>& blob, char version = '2') {
  const uint32_t recordSize = 4 + (records.empty() ? 0 : static_cast<uint32_t>(records[0].second.size()));
  std::vector<uint8_t> body;
  for (const auto& r : records) {
    appendU32(body, r.first);
    body.insert(body.end(), r.second.begin(), r.second.end());
  }
  std::vector<uint8_t> file = {'S', 'T', 'A', static_cast<uint8_t>(version)};
  appendU32(file, static_cast<uint32_t>(records.size()));
  appendU32(file, recordSize);
  appendU32(file, 16 + static_cast<uint32_t>(body.size()));
  file.insert(file.end(), body.begin(), body.end());
  file.insert(file.end(), blob.begin(), blob.end());
  return file;
}

// --- the fixture agency ----------------------------------------------------
//
// Two services: 0 runs weekdays, 1 runs Sundays. Two routes at stop 4377.
// Trip ids encode which service they belong to so the assertions read
// clearly: 1xx = weekday, 2xx = Sunday.

constexpr uint32_t kStop = 4377;
constexpr uint8_t kWeekdayService = 0;
constexpr uint8_t kSundayService = 1;

std::vector<uint8_t> buildCalendar(int32_t startDate = 20260101, int32_t endDate = 20261231) {
  auto entry = [&](uint8_t daysMask) {
    std::vector<uint8_t> rest;
    rest.push_back(daysMask);
    rest.push_back(0);
    rest.push_back(0);
    rest.push_back(0);
    appendU32(rest, static_cast<uint32_t>(startDate));
    appendU32(rest, static_cast<uint32_t>(endDate));
    return rest;
  };
  // Bit 0 = Sunday .. bit 6 = Saturday (tm_wday order).
  const uint8_t weekdays = 0b0111110;  // Mon-Fri
  const uint8_t sundays = 0b0000001;
  return buildTable({{kWeekdayService, entry(weekdays)}, {kSundayService, entry(sundays)}}, {});
}

std::vector<uint8_t> buildTrips() {
  std::vector<uint8_t> blob;
  auto internedHeadsign = [&blob](const std::string& s) {
    const uint32_t offset = static_cast<uint32_t>(blob.size());
    blob.insert(blob.end(), s.begin(), s.end());
    blob.push_back(0);
    return offset;
  };
  const uint32_t downtown = internedHeadsign("Downtown");
  const uint32_t shadle = internedHeadsign("Shadle");

  auto trip = [](uint32_t routeId, uint8_t direction, uint8_t service, uint32_t headsign) {
    std::vector<uint8_t> rest;
    appendU32(rest, routeId);
    rest.push_back(direction);
    rest.push_back(service);
    rest.push_back(0);
    rest.push_back(0);
    appendU32(rest, headsign);
    return rest;
  };

  return buildTable(
      {
          {101, trip(671, 0, kWeekdayService, downtown)},
          {102, trip(671, 0, kWeekdayService, downtown)},
          {103, trip(672, 1, kWeekdayService, shadle)},
          {104, trip(671, 0, kWeekdayService, downtown)},  // after midnight
          {201, trip(671, 0, kSundayService, downtown)},
      },
      blob);
}

std::vector<uint8_t> buildRoutes() {
  std::vector<uint8_t> blob;
  auto intern = [&blob](const std::string& s) {
    const uint32_t offset = static_cast<uint32_t>(blob.size());
    blob.insert(blob.end(), s.begin(), s.end());
    blob.push_back(0);
    return offset;
  };
  const uint32_t n31 = intern("31");
  const uint32_t n32 = intern("32");
  auto route = [](uint32_t nameOffset) {
    std::vector<uint8_t> rest;
    appendU32(rest, nameOffset);
    appendU32(rest, 0x1A7F37);
    appendU32(rest, 0xFFFFFF);
    return rest;
  };
  return buildTable({{671, route(n31)}, {672, route(n32)}}, blob);
}

std::vector<uint8_t> buildStopTimes() {
  auto row = [](uint32_t tripId, uint32_t departureSec) {
    std::vector<uint8_t> rest;
    appendU32(rest, tripId);
    appendU32(rest, departureSec);
    return rest;
  };
  // Sorted by (stopCode, departureSec), exactly as the generator writes.
  // Includes a decoy stop on each side so a lookup that ignored the key
  // boundary would visibly overrun.
  return buildTable(
      {
          {4000, row(999, 8 * 3600)},
          {kStop, row(201, 9 * 3600)},                   // Sunday only
          {kStop, row(101, 17 * 3600)},                  // 5:00pm weekday
          {kStop, row(103, 17 * 3600 + 20 * 60)},        // 5:20pm weekday
          {kStop, row(102, 18 * 3600)},                  // 6:00pm weekday
          {kStop, row(104, 24 * 3600 + 45 * 60)},        // 24:45 -> 00:45 next day
          {5000, row(998, 19 * 3600)},
      },
      {});
}

struct Fixture {
  InMemoryReader stopTimes{buildStopTimes()};
  InMemoryReader trips{buildTrips()};
  InMemoryReader routes{buildRoutes()};
  InMemoryReader calendar{buildCalendar()};

  StaticScheduleTables tables() {
    StaticScheduleTables t;
    t.stopTimes = &stopTimes;
    t.trips = &trips;
    t.routes = &routes;
    t.calendar = &calendar;
    return t;
  }
};

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

std::string clockOf(int64_t epoch) {
  std::tm local{};
  const std::time_t t = static_cast<std::time_t>(epoch);
  localtime_r(&t, &local);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
  return std::string(buf);
}

void setUp_() { applyTimezone(kDefaultPosixTz); }

// --- calendar --------------------------------------------------------------

void test_weekday_and_sunday_services_are_selected_by_weekday() {
  setUp_();
  Fixture f;
  // Friday Sep 11 2026.
  const auto weekday = activeServices(f.tables(), serviceDayFor(pacific(2026, 9, 11, 12, 0)));
  TEST_ASSERT_EQUAL_size_t(1, weekday.size());
  TEST_ASSERT_EQUAL_UINT8(kWeekdayService, weekday[0]);

  // Sunday Sep 13 2026.
  const auto sunday = activeServices(f.tables(), serviceDayFor(pacific(2026, 9, 13, 12, 0)));
  TEST_ASSERT_EQUAL_size_t(1, sunday.size());
  TEST_ASSERT_EQUAL_UINT8(kSundayService, sunday[0]);
}

void test_a_date_outside_the_service_range_activates_nothing() {
  setUp_();
  InMemoryReader stopTimes(buildStopTimes());
  InMemoryReader trips(buildTrips());
  InMemoryReader routes(buildRoutes());
  InMemoryReader calendar(buildCalendar(20260101, 20260630));  // ended in June
  StaticScheduleTables tables;
  tables.stopTimes = &stopTimes;
  tables.trips = &trips;
  tables.routes = &routes;
  tables.calendar = &calendar;

  TEST_ASSERT_EQUAL_size_t(0, activeServices(tables, serviceDayFor(pacific(2026, 9, 11, 12, 0))).size());
}

// Holidays are the whole reason calendar_dates exists. Without it the board
// shows a full weekday timetable on Thanksgiving.
void test_calendar_dates_exceptions_override_the_weekly_pattern() {
  setUp_();
  Fixture f;
  auto exception = [](uint8_t service, uint8_t type) {
    std::vector<uint8_t> rest;
    rest.push_back(service);
    rest.push_back(type);
    rest.push_back(0);
    rest.push_back(0);
    return rest;
  };
  // Thursday Nov 26 2026 (Thanksgiving): weekday service removed, Sunday
  // service added in its place.
  InMemoryReader dates(buildTable(
      {{20261126, exception(kWeekdayService, 2)}, {20261126, exception(kSundayService, 1)}}, {}));

  StaticScheduleTables tables = f.tables();
  tables.calendarDates = &dates;

  const auto holiday = activeServices(tables, serviceDayFor(pacific(2026, 11, 26, 12, 0)));
  TEST_ASSERT_EQUAL_size_t(1, holiday.size());
  TEST_ASSERT_EQUAL_UINT8(kSundayService, holiday[0]);

  // The very next Thursday is unaffected -- the exception applies to one
  // date, not to every Thursday.
  const auto normal = activeServices(tables, serviceDayFor(pacific(2026, 12, 3, 12, 0)));
  TEST_ASSERT_EQUAL_size_t(1, normal.size());
  TEST_ASSERT_EQUAL_UINT8(kWeekdayService, normal[0]);
}

// --- departures ------------------------------------------------------------

void test_next_departures_on_a_weekday_afternoon() {
  setUp_();
  Fixture f;
  // 4:30pm Friday. The 5:00, 5:20 and 6:00 trips are still to come -- and
  // so is trip 104, which Friday's service day schedules as "24:45:00" and
  // which therefore departs at 00:45 on Saturday. It belongs to *this*
  // service day, not tomorrow's, so it turns up here rather than needing
  // the after-midnight candidate logic.
  const auto days = serviceDayCandidates(pacific(2026, 9, 11, 16, 30));
  const auto found = nextScheduledDepartures(f.tables(), kStop, days, 5);

  TEST_ASSERT_EQUAL_size_t(4, found.size());
  TEST_ASSERT_EQUAL_STRING("17:00", clockOf(found[0].departureEpoch).c_str());
  TEST_ASSERT_EQUAL_STRING("17:20", clockOf(found[1].departureEpoch).c_str());
  TEST_ASSERT_EQUAL_STRING("18:00", clockOf(found[2].departureEpoch).c_str());
  TEST_ASSERT_EQUAL_STRING("00:45", clockOf(found[3].departureEpoch).c_str());
  TEST_ASSERT_EQUAL_INT64(pacific(2026, 9, 12, 0, 45), found[3].departureEpoch);

  // Route name, headsign and direction are all resolved through trips.bin
  // and routes.bin.
  TEST_ASSERT_EQUAL_STRING("31", found[0].routeShortName.c_str());
  TEST_ASSERT_EQUAL_STRING("Downtown", found[0].headsign.c_str());
  TEST_ASSERT_EQUAL_UINT8(0, found[0].directionId);
  TEST_ASSERT_EQUAL_STRING("32", found[1].routeShortName.c_str());
  TEST_ASSERT_EQUAL_STRING("Shadle", found[1].headsign.c_str());
  TEST_ASSERT_EQUAL_UINT8(1, found[1].directionId);
}

void test_departures_already_past_are_not_returned() {
  setUp_();
  Fixture f;
  // 5:30pm: the 5:00 and 5:20 have gone, leaving the 6:00 and the late
  // 24:45 one.
  const auto found =
      nextScheduledDepartures(f.tables(), kStop, serviceDayCandidates(pacific(2026, 9, 11, 17, 30)), 5);
  TEST_ASSERT_EQUAL_size_t(2, found.size());
  TEST_ASSERT_EQUAL_STRING("18:00", clockOf(found[0].departureEpoch).c_str());
  TEST_ASSERT_EQUAL_STRING("00:45", clockOf(found[1].departureEpoch).c_str());
}

void test_max_count_is_respected() {
  setUp_();
  Fixture f;
  const auto found =
      nextScheduledDepartures(f.tables(), kStop, serviceDayCandidates(pacific(2026, 9, 11, 16, 30)), 2);
  TEST_ASSERT_EQUAL_size_t(2, found.size());
}

void test_sunday_sees_only_sunday_service() {
  setUp_();
  Fixture f;
  // Sunday 8:00am: only trip 201 runs, at 9:00. The weekday evening trips
  // are in the same stop block and must be filtered out by service.
  const auto found =
      nextScheduledDepartures(f.tables(), kStop, serviceDayCandidates(pacific(2026, 9, 13, 8, 0)), 5);
  TEST_ASSERT_EQUAL_size_t(1, found.size());
  TEST_ASSERT_EQUAL_STRING("09:00", clockOf(found[0].departureEpoch).c_str());
}

// The case a same-calendar-day lookup silently gets wrong -- and it happens
// to be the last bus of the night, which is when you most need the board.
void test_after_midnight_finds_yesterdays_late_trip() {
  setUp_();
  Fixture f;
  // 00:15 Saturday Sep 12. Friday's "24:45:00" trip departs in 30 minutes.
  const auto found =
      nextScheduledDepartures(f.tables(), kStop, serviceDayCandidates(pacific(2026, 9, 12, 0, 15)), 5);
  TEST_ASSERT_EQUAL_size_t(1, found.size());
  TEST_ASSERT_EQUAL_UINT32(104, found[0].tripId);
  TEST_ASSERT_EQUAL_INT64(pacific(2026, 9, 12, 0, 45), found[0].departureEpoch);
}

void test_unknown_stop_returns_nothing_without_overrunning_its_neighbours() {
  setUp_();
  Fixture f;
  // Stop 4500 sits between the fixture's 4377 and 5000 blocks. A lookup
  // that didn't respect the key boundary would return stop 5000's trip.
  const auto found =
      nextScheduledDepartures(f.tables(), 4500, serviceDayCandidates(pacific(2026, 9, 11, 8, 0)), 5);
  TEST_ASSERT_EQUAL_size_t(0, found.size());
}

// The row-scan cap is per service day, not shared across them. A busy
// stop whose own block exhausts the cap must not starve the yesterday
// candidate -- that candidate is the only source of after-midnight trips,
// which is to say the last bus of the night.
void test_a_busy_stop_today_does_not_starve_yesterdays_late_trip() {
  setUp_();

  auto row = [](uint32_t tripId, uint32_t departureSec) {
    std::vector<uint8_t> rest;
    appendU32(rest, tripId);
    appendU32(rest, departureSec);
    return rest;
  };

  // One stop with far more than kMaxStopTimeRowsScanned rows, every one of
  // them belonging to the Sunday service so none of them match on a
  // Saturday... except a single late-night trip on the weekday service,
  // which Friday schedules as 24:50 and which therefore departs 00:50
  // Saturday.
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> rows;
  std::vector<uint8_t> tripBlob;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> tripRows;
  auto internHeadsign = [&tripBlob](const std::string& str) {
    const uint32_t offset = static_cast<uint32_t>(tripBlob.size());
    tripBlob.insert(tripBlob.end(), str.begin(), str.end());
    tripBlob.push_back(0);
    return offset;
  };
  const uint32_t headsign = internHeadsign("Downtown");
  auto tripRecord = [](uint32_t routeId, uint8_t service, uint32_t hs) {
    std::vector<uint8_t> rest;
    appendU32(rest, routeId);
    rest.push_back(0);
    rest.push_back(service);
    rest.push_back(0);
    rest.push_back(0);
    appendU32(rest, hs);
    return rest;
  };

  const int kBusyRows = transit::sta::kMaxStopTimeRowsScanned + 50;
  for (int i = 0; i < kBusyRows; ++i) {
    const uint32_t tripId = 5000 + static_cast<uint32_t>(i);
    // Sunday-service trips spread across Saturday evening, so on Saturday
    // they are scanned (they're after "now") but every one is filtered out
    // by service.
    rows.push_back({kStop, row(tripId, static_cast<uint32_t>(18 * 3600 + i))});
    tripRows.push_back({tripId, tripRecord(671, kSundayService, headsign)});
  }
  // Friday's late trip, on the weekday service.
  rows.push_back({kStop, row(104, 24 * 3600 + 50 * 60)});
  tripRows.push_back({104, tripRecord(671, kWeekdayService, headsign)});

  // Sorted by (stopCode, departureSec), the order the generator writes and
  // the order the lower-bound search depends on. Decoded rather than
  // memcmp'd: these are little-endian, so a byte-wise compare would order
  // by least-significant byte and silently produce a table the binary
  // search can't navigate.
  auto departureSecOf = [](const std::vector<uint8_t>& rest) {
    return static_cast<uint32_t>(rest[4]) | (static_cast<uint32_t>(rest[5]) << 8) |
           (static_cast<uint32_t>(rest[6]) << 16) | (static_cast<uint32_t>(rest[7]) << 24);
  };
  std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    return departureSecOf(a.second) < departureSecOf(b.second);
  });
  std::sort(tripRows.begin(), tripRows.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  InMemoryReader stopTimes(buildTable(rows, {}));
  InMemoryReader trips(buildTable(tripRows, tripBlob));
  InMemoryReader routes(buildRoutes());
  InMemoryReader calendar(buildCalendar());
  StaticScheduleTables tables;
  tables.stopTimes = &stopTimes;
  tables.trips = &trips;
  tables.routes = &routes;
  tables.calendar = &calendar;

  // 00:30 Saturday. Saturday's own service day has hundreds of rows to
  // chew through (all Sunday-service, all filtered out); Friday's has the
  // one trip that actually matters.
  const auto found =
      nextScheduledDepartures(tables, kStop, serviceDayCandidates(pacific(2026, 9, 12, 0, 30)), 5);
  TEST_ASSERT_EQUAL_size_t(1, found.size());
  TEST_ASSERT_EQUAL_UINT32(104, found[0].tripId);
  TEST_ASSERT_EQUAL_INT64(pacific(2026, 9, 12, 0, 50), found[0].departureEpoch);
}

// --- degradation -----------------------------------------------------------

void test_a_version_one_card_is_refused_rather_than_misread() {
  setUp_();
  // Same data, written in the old format. Every trip's serviceIndex byte
  // is reserved-and-zero there, so believing it would make every trip look
  // like service 0 -- confidently wrong, which is worse than absent.
  InMemoryReader stopTimes(buildTable(
      {{kStop, [] { std::vector<uint8_t> r; appendU32(r, 101); appendU32(r, 17 * 3600); return r; }()}},
      {}, '1'));
  InMemoryReader trips(buildTrips());
  InMemoryReader routes(buildRoutes());
  InMemoryReader calendar(buildCalendar());
  StaticScheduleTables tables;
  tables.stopTimes = &stopTimes;
  tables.trips = &trips;
  tables.routes = &routes;
  tables.calendar = &calendar;

  TEST_ASSERT_EQUAL_size_t(
      0, nextScheduledDepartures(tables, kStop, serviceDayCandidates(pacific(2026, 9, 11, 16, 0)), 5).size());
}

void test_missing_tables_degrade_to_nothing_rather_than_crashing() {
  setUp_();
  Fixture f;
  const auto days = serviceDayCandidates(pacific(2026, 9, 11, 16, 30));

  StaticScheduleTables noStopTimes = f.tables();
  noStopTimes.stopTimes = nullptr;
  TEST_ASSERT_EQUAL_size_t(0, nextScheduledDepartures(noStopTimes, kStop, days, 5).size());

  StaticScheduleTables noTrips = f.tables();
  noTrips.trips = nullptr;
  TEST_ASSERT_EQUAL_size_t(0, nextScheduledDepartures(noTrips, kStop, days, 5).size());

  StaticScheduleTables noCalendar = f.tables();
  noCalendar.calendar = nullptr;
  TEST_ASSERT_EQUAL_size_t(0, nextScheduledDepartures(noCalendar, kStop, days, 5).size());

  // Routes are cosmetic: a departure with an unresolvable route name is
  // still a real bus and must still be reported.
  StaticScheduleTables noRoutes = f.tables();
  noRoutes.routes = nullptr;
  const auto found = nextScheduledDepartures(noRoutes, kStop, days, 5);
  TEST_ASSERT_EQUAL_size_t(4, found.size());
  TEST_ASSERT_EQUAL_STRING("", found[0].routeShortName.c_str());
  TEST_ASSERT_EQUAL_STRING("Downtown", found[0].headsign.c_str());
}

void test_no_clock_means_no_service_days_and_so_no_departures() {
  setUp_();
  Fixture f;
  TEST_ASSERT_EQUAL_size_t(0, nextScheduledDepartures(f.tables(), kStop, serviceDayCandidates(0), 5).size());
}

// --- feed validity ---------------------------------------------------------

void test_feed_validity_spans_every_service() {
  setUp_();
  InMemoryReader calendar(buildCalendar(20260517, 20260919));
  const FeedValidity validity = readFeedValidity(calendar);
  TEST_ASSERT_TRUE(validity.known);
  TEST_ASSERT_EQUAL_INT32(20260517, validity.startDate);
  TEST_ASSERT_EQUAL_INT32(20260919, validity.endDate);

  TEST_ASSERT_TRUE(feedCoversDate(validity, 20260912));
  TEST_ASSERT_TRUE(feedCoversDate(validity, 20260919));   // inclusive
  TEST_ASSERT_FALSE(feedCoversDate(validity, 20260920));  // one day past
  TEST_ASSERT_FALSE(feedCoversDate(validity, 20260516));
}

void test_an_unknown_validity_does_not_block_the_schedule() {
  setUp_();
  // Refusing to show a schedule because the window couldn't be read would
  // turn a minor problem into a blank board.
  FeedValidity unknown;
  TEST_ASSERT_TRUE(feedCoversDate(unknown, 20260912));
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_weekday_and_sunday_services_are_selected_by_weekday);
  RUN_TEST(test_a_date_outside_the_service_range_activates_nothing);
  RUN_TEST(test_calendar_dates_exceptions_override_the_weekly_pattern);
  RUN_TEST(test_next_departures_on_a_weekday_afternoon);
  RUN_TEST(test_departures_already_past_are_not_returned);
  RUN_TEST(test_max_count_is_respected);
  RUN_TEST(test_sunday_sees_only_sunday_service);
  RUN_TEST(test_after_midnight_finds_yesterdays_late_trip);
  RUN_TEST(test_unknown_stop_returns_nothing_without_overrunning_its_neighbours);
  RUN_TEST(test_a_busy_stop_today_does_not_starve_yesterdays_late_trip);
  RUN_TEST(test_a_version_one_card_is_refused_rather_than_misread);
  RUN_TEST(test_missing_tables_degrade_to_nothing_rather_than_crashing);
  RUN_TEST(test_no_clock_means_no_service_days_and_so_no_departures);
  RUN_TEST(test_feed_validity_spans_every_service);
  RUN_TEST(test_an_unknown_validity_does_not_block_the_schedule);
  return UNITY_END();
}
