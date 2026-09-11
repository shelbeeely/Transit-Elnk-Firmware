// Host-side tests for offline_cache.h: the serialize/parse round trip, the
// NVS size cap, and the ageing rules that keep a restored board honest.

#include <unity.h>

#include <string>

#include "transit/offline_cache.h"

using transit::CachedBoard;
using transit::DepartureRow;
using transit::DirectionBoard;
using transit::PlannedLeg;
using transit::PresetTripPlan;
using transit::cachedAgeMinutes;
using transit::deserializeCachedBoard;
using transit::pruneExpiredDepartures;
using transit::serializeCachedBoard;

namespace {

constexpr int64_t kNow = 1'700'000'000;

DepartureRow makeDeparture(const std::string& headsign, int64_t epoch, bool realTime = false,
                           bool isLast = false) {
  DepartureRow row;
  row.headsign = headsign;
  row.stopName = "Main St & 5th Ave";
  row.departureTimeEpoch = epoch;
  row.isRealTime = realTime;
  row.isLast = isLast;
  return row;
}

DirectionBoard makeRoute(const std::string& routeId, const std::string& shortName) {
  DirectionBoard dir;
  dir.globalRouteId = routeId;
  dir.routeShortName = shortName;
  dir.routeDisplayShortName.elements[0] = "bus-" + shortName;
  dir.routeDisplayShortName.elements[1] = shortName;
  dir.routeDisplayShortName.elements[2] = "";
  dir.routeColor = "1A7F37";
  dir.routeTextColor = "FFFFFF";
  dir.directionId = 1;
  return dir;
}

CachedBoard makeSampleCache() {
  CachedBoard cache;
  cache.fetchedAtEpoch = kNow;

  DirectionBoard r1 = makeRoute("1:31", "31");
  r1.departures.push_back(makeDeparture("Downtown", kNow + 5 * 60, /*realTime=*/true));
  r1.departures.push_back(makeDeparture("Downtown", kNow + 25 * 60));
  cache.board.push_back(r1);

  DirectionBoard r2 = makeRoute("1:32", "32");
  r2.departures.push_back(makeDeparture("Shadle", kNow + 12 * 60, false, /*isLast=*/true));
  cache.board.push_back(r2);

  PresetTripPlan home;
  home.presetName = "Home";
  home.found = true;
  home.leaveByEpoch = kNow + 3 * 60;
  PlannedLeg leg;
  leg.routeId = "1:31";
  leg.routeShortName = "31";
  leg.boardStopId = "1:A";
  leg.alightStopId = "1:B";
  leg.boardEpoch = kNow + 5 * 60;
  leg.alightEpoch = kNow + 20 * 60;
  home.legs.push_back(leg);
  cache.presetPlans.push_back(home);

  return cache;
}

void test_round_trip_preserves_every_field() {
  const CachedBoard original = makeSampleCache();
  const std::string blob = serializeCachedBoard(original);
  TEST_ASSERT_FALSE(blob.empty());

  CachedBoard restored;
  TEST_ASSERT_TRUE(deserializeCachedBoard(blob, restored));

  TEST_ASSERT_EQUAL_INT64(original.fetchedAtEpoch, restored.fetchedAtEpoch);
  TEST_ASSERT_EQUAL_size_t(original.board.size(), restored.board.size());

  const DirectionBoard& a = original.board[0];
  const DirectionBoard& b = restored.board[0];
  TEST_ASSERT_EQUAL_STRING(a.globalRouteId.c_str(), b.globalRouteId.c_str());
  TEST_ASSERT_EQUAL_STRING(a.routeShortName.c_str(), b.routeShortName.c_str());
  TEST_ASSERT_EQUAL_STRING(a.routeDisplayShortName.elements[0].c_str(),
                           b.routeDisplayShortName.elements[0].c_str());
  TEST_ASSERT_EQUAL_STRING(a.routeDisplayShortName.elements[1].c_str(),
                           b.routeDisplayShortName.elements[1].c_str());
  TEST_ASSERT_EQUAL_STRING(a.routeColor.c_str(), b.routeColor.c_str());
  TEST_ASSERT_EQUAL_STRING(a.routeTextColor.c_str(), b.routeTextColor.c_str());
  TEST_ASSERT_EQUAL_INT(a.directionId, b.directionId);

  TEST_ASSERT_EQUAL_size_t(2, b.departures.size());
  TEST_ASSERT_EQUAL_STRING("Downtown", b.departures[0].headsign.c_str());
  TEST_ASSERT_EQUAL_INT64(kNow + 5 * 60, b.departures[0].departureTimeEpoch);
  TEST_ASSERT_TRUE(b.departures[0].isRealTime);
  TEST_ASSERT_FALSE(b.departures[0].isLast);
  TEST_ASSERT_TRUE(restored.board[1].departures[0].isLast);

  TEST_ASSERT_EQUAL_size_t(1, restored.presetPlans.size());
  const PresetTripPlan& plan = restored.presetPlans[0];
  TEST_ASSERT_EQUAL_STRING("Home", plan.presetName.c_str());
  TEST_ASSERT_TRUE(plan.found);
  TEST_ASSERT_EQUAL_INT64(kNow + 3 * 60, plan.leaveByEpoch);
  TEST_ASSERT_EQUAL_size_t(1, plan.legs.size());
  TEST_ASSERT_EQUAL_STRING("31", plan.legs[0].routeShortName.c_str());
  TEST_ASSERT_EQUAL_INT64(kNow + 20 * 60, plan.legs[0].alightEpoch);
}

void test_empty_and_timestampless_caches_serialize_to_nothing() {
  TEST_ASSERT_TRUE(serializeCachedBoard(CachedBoard{}).empty());

  CachedBoard noTimestamp = makeSampleCache();
  noTimestamp.fetchedAtEpoch = 0;
  TEST_ASSERT_TRUE(serializeCachedBoard(noTimestamp).empty());
}

void test_garbage_and_wrong_version_are_rejected() {
  CachedBoard out;
  TEST_ASSERT_FALSE(deserializeCachedBoard("", out));
  TEST_ASSERT_FALSE(deserializeCachedBoard("not a cache at all", out));
  TEST_ASSERT_FALSE(deserializeCachedBoard("TCB9\t1700000000\n", out));
  // Right tag, but no usable fetch timestamp.
  TEST_ASSERT_FALSE(deserializeCachedBoard("TCB1\t0\n", out));
}

void test_tabs_and_newlines_in_a_headsign_do_not_corrupt_the_blob() {
  // Headsigns come straight out of a GTFS feed. A stray delimiter in one
  // must not shift every following field by one.
  CachedBoard cache;
  cache.fetchedAtEpoch = kNow;
  DirectionBoard dir = makeRoute("1:99", "99");
  dir.departures.push_back(makeDeparture("Airport\tvia\nTerminal", kNow + 60));
  cache.board.push_back(dir);

  CachedBoard restored;
  TEST_ASSERT_TRUE(deserializeCachedBoard(serializeCachedBoard(cache), restored));
  TEST_ASSERT_EQUAL_size_t(1, restored.board.size());
  TEST_ASSERT_EQUAL_size_t(1, restored.board[0].departures.size());
  TEST_ASSERT_EQUAL_STRING("Airport via Terminal", restored.board[0].departures[0].headsign.c_str());
  TEST_ASSERT_EQUAL_INT64(kNow + 60, restored.board[0].departures[0].departureTimeEpoch);
}

void test_size_cap_is_respected_and_presets_survive_truncation() {
  // Far more board than fits: the cap must hold, and the preset plans --
  // written first precisely so they're the last thing dropped -- must come
  // back intact.
  CachedBoard cache = makeSampleCache();
  for (int i = 0; i < 200; ++i) {
    DirectionBoard dir = makeRoute("1:" + std::to_string(i), std::to_string(i));
    for (int d = 0; d < 5; ++d) {
      dir.departures.push_back(makeDeparture("A long headsign for route " + std::to_string(i),
                                             kNow + (d + 1) * 60));
    }
    cache.board.push_back(dir);
  }

  const std::string blob = serializeCachedBoard(cache);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(transit::kMaxCachedBoardBytes, blob.size());

  CachedBoard restored;
  TEST_ASSERT_TRUE(deserializeCachedBoard(blob, restored));
  TEST_ASSERT_EQUAL_size_t(1, restored.presetPlans.size());
  TEST_ASSERT_EQUAL_STRING("Home", restored.presetPlans[0].presetName.c_str());
  TEST_ASSERT_GREATER_THAN_size_t(0, restored.board.size());
  TEST_ASSERT_LESS_THAN_size_t(cache.board.size(), restored.board.size());
}

void test_prune_drops_departures_that_have_already_gone() {
  CachedBoard cache = makeSampleCache();
  // 15 minutes later: route 31's 5-minute departure is gone, its 25-minute
  // one isn't, and route 32's 12-minute one has left.
  pruneExpiredDepartures(cache, kNow + 15 * 60);

  TEST_ASSERT_EQUAL_size_t(1, cache.board.size());
  TEST_ASSERT_EQUAL_STRING("1:31", cache.board[0].globalRouteId.c_str());
  TEST_ASSERT_EQUAL_size_t(1, cache.board[0].departures.size());
  TEST_ASSERT_EQUAL_INT64(kNow + 25 * 60, cache.board[0].departures[0].departureTimeEpoch);
  // The Home plan's leave-by time (now + 3 min) is also in the past.
  TEST_ASSERT_EQUAL_size_t(0, cache.presetPlans.size());
}

void test_prune_empties_a_cache_that_has_aged_out_completely() {
  CachedBoard cache = makeSampleCache();
  pruneExpiredDepartures(cache, kNow + 24 * 60 * 60);
  TEST_ASSERT_EQUAL_size_t(0, cache.board.size());
  TEST_ASSERT_EQUAL_size_t(0, cache.presetPlans.size());
}

void test_prune_is_a_no_op_without_a_clock() {
  // nowEpoch <= 0 means the board has no idea what time it is, which is no
  // basis for declaring anything expired.
  CachedBoard cache = makeSampleCache();
  pruneExpiredDepartures(cache, 0);
  TEST_ASSERT_EQUAL_size_t(2, cache.board.size());
  TEST_ASSERT_EQUAL_size_t(1, cache.presetPlans.size());
}

void test_prune_keeps_a_not_found_plan_so_its_message_still_shows() {
  CachedBoard cache;
  cache.fetchedAtEpoch = kNow;
  PresetTripPlan plan;
  plan.presetName = "Work";
  plan.found = false;
  plan.fallbackMessage = "No upcoming trip found";
  cache.presetPlans.push_back(plan);

  pruneExpiredDepartures(cache, kNow + 10 * 60 * 60);
  TEST_ASSERT_EQUAL_size_t(1, cache.presetPlans.size());
  TEST_ASSERT_EQUAL_STRING("No upcoming trip found", cache.presetPlans[0].fallbackMessage.c_str());
}

void test_cached_age_minutes() {
  const CachedBoard cache = makeSampleCache();
  TEST_ASSERT_EQUAL_INT(0, cachedAgeMinutes(cache, kNow));
  TEST_ASSERT_EQUAL_INT(90, cachedAgeMinutes(cache, kNow + 90 * 60));
  // No clock, or a clock correction that moved time backwards.
  TEST_ASSERT_EQUAL_INT(0, cachedAgeMinutes(cache, 0));
  TEST_ASSERT_EQUAL_INT(0, cachedAgeMinutes(cache, kNow - 600));
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_preserves_every_field);
  RUN_TEST(test_empty_and_timestampless_caches_serialize_to_nothing);
  RUN_TEST(test_garbage_and_wrong_version_are_rejected);
  RUN_TEST(test_tabs_and_newlines_in_a_headsign_do_not_corrupt_the_blob);
  RUN_TEST(test_size_cap_is_respected_and_presets_survive_truncation);
  RUN_TEST(test_prune_drops_departures_that_have_already_gone);
  RUN_TEST(test_prune_empties_a_cache_that_has_aged_out_completely);
  RUN_TEST(test_prune_is_a_no_op_without_a_clock);
  RUN_TEST(test_prune_keeps_a_not_found_plan_so_its_message_still_shows);
  RUN_TEST(test_cached_age_minutes);
  return UNITY_END();
}
