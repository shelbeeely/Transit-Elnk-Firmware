// Host-side tests for trip_planner.h's planPresetTrip() — constructs
// transit::Route/MergedItinerary/ScheduleItem fixtures directly (no JSON
// needed), one Route entry per (route, stop) pair matching how
// stop_departures() responses are shaped when queried across multiple
// stop_ids (see docs/TRIP_PLANNER.md).

#include <unity.h>

#include "transit/trip_planner.h"

using transit::MergedItinerary;
using transit::PresetConfig;
using transit::PresetTripPlan;
using transit::Route;
using transit::ScheduleItem;
using transit::TripLegConfig;

namespace {

constexpr int64_t kNow = 1'700'000'000;

ScheduleItem makeItem(int64_t departureEpoch, bool cancelled = false,
                       const std::string& rtTripId = "") {
  ScheduleItem item;
  item.departureTimeEpoch = departureEpoch;
  item.scheduledDepartureTimeEpoch = departureEpoch;
  item.isCancelled = cancelled;
  item.rtTripId = rtTripId;
  return item;
}

// One Route entry for routeId's departures at stopId, single direction.
Route makeRouteAtStop(const std::string& routeId, const std::string& stopId,
                       const std::string& shortName, int directionId,
                       std::vector<ScheduleItem> items) {
  Route route;
  route.globalRouteId = routeId;
  route.globalStopId = stopId;
  route.routeShortName = shortName;

  MergedItinerary mi;
  mi.directionId = directionId;
  mi.scheduleItems = std::move(items);
  route.mergedItineraries.push_back(std::move(mi));
  return route;
}

TripLegConfig makeLeg(const std::string& routeId, const std::string& boardStopId,
                       const std::string& alightStopId, int directionId = -1) {
  TripLegConfig leg;
  leg.routeId = routeId;
  leg.boardStopId = boardStopId;
  leg.alightStopId = alightStopId;
  leg.directionId = directionId;
  return leg;
}

}  // namespace

void test_empty_legs_not_configured() {
  PresetConfig preset;
  preset.presetName = "Home";

  auto plan = transit::planPresetTrip({}, preset, kNow);

  TEST_ASSERT_FALSE(plan.found);
  TEST_ASSERT_EQUAL_STRING("Not configured", plan.fallbackMessage.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, plan.legs.size());
}

void test_happy_path_two_legs() {
  std::vector<Route> routes = {
      makeRouteAtStop("31", "A", "31", 0, {makeItem(kNow + 300)}),
      makeRouteAtStop("31", "B", "31", 0, {makeItem(kNow + 600)}),
      makeRouteAtStop("32", "B", "32", 0, {makeItem(kNow + 900)}),
      makeRouteAtStop("32", "C", "32", 0, {makeItem(kNow + 1200)}),
  };

  PresetConfig preset;
  preset.presetName = "Home";
  preset.walkToFirstStopMin = 2;
  preset.transferBufferMin = 3;
  preset.legs = {makeLeg("31", "A", "B", 0), makeLeg("32", "B", "C", 0)};

  auto plan = transit::planPresetTrip(routes, preset, kNow);

  TEST_ASSERT_TRUE(plan.found);
  TEST_ASSERT_EQUAL_UINT32(2, plan.legs.size());
  TEST_ASSERT_EQUAL_INT64(kNow + 300, plan.legs[0].boardEpoch);
  TEST_ASSERT_EQUAL_INT64(kNow + 600, plan.legs[0].alightEpoch);
  TEST_ASSERT_EQUAL_INT64(kNow + 900, plan.legs[1].boardEpoch);
  TEST_ASSERT_EQUAL_INT64(kNow + 1200, plan.legs[1].alightEpoch);
  TEST_ASSERT_EQUAL_INT64(kNow + 300 - 2 * 60, plan.leaveByEpoch);
}

void test_happy_path_three_legs() {
  std::vector<Route> routes = {
      makeRouteAtStop("31", "A", "31", 0, {makeItem(kNow + 300)}),
      makeRouteAtStop("31", "B", "31", 0, {makeItem(kNow + 600)}),
      makeRouteAtStop("32", "B", "32", 0, {makeItem(kNow + 900)}),
      makeRouteAtStop("32", "C", "32", 0, {makeItem(kNow + 1200)}),
      makeRouteAtStop("97", "C", "97", 0, {makeItem(kNow + 1500)}),
      makeRouteAtStop("97", "D", "97", 0, {makeItem(kNow + 1800)}),
  };

  PresetConfig preset;
  preset.presetName = "Work";
  preset.transferBufferMin = 3;
  preset.legs = {makeLeg("31", "A", "B", 0), makeLeg("32", "B", "C", 0),
                 makeLeg("97", "C", "D", 0)};

  auto plan = transit::planPresetTrip(routes, preset, kNow);

  TEST_ASSERT_TRUE(plan.found);
  TEST_ASSERT_EQUAL_UINT32(3, plan.legs.size());
  TEST_ASSERT_EQUAL_INT64(kNow + 1500, plan.legs[2].boardEpoch);
  TEST_ASSERT_EQUAL_INT64(kNow + 1800, plan.legs[2].alightEpoch);
}

void test_first_leg_not_found_fallback() {
  std::vector<Route> routes = {
      // Route 31 at stop A has no departures at all.
      makeRouteAtStop("32", "B", "32", 0, {makeItem(kNow + 900)}),
  };

  PresetConfig preset;
  preset.presetName = "Home";
  preset.legs = {makeLeg("31", "A", "B", 0), makeLeg("32", "B", "C", 0)};

  auto plan = transit::planPresetTrip(routes, preset, kNow);

  TEST_ASSERT_FALSE(plan.found);
  TEST_ASSERT_EQUAL_STRING("No upcoming trip found", plan.fallbackMessage.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, plan.legs.size());
}

void test_transfer_infeasible_within_buffer_fallback() {
  std::vector<Route> routes = {
      makeRouteAtStop("31", "A", "31", 0, {makeItem(kNow + 300)}),
      makeRouteAtStop("31", "B", "31", 0, {makeItem(kNow + 600)}),
      // Buffer is 3min (180s): the next leg's departure must be >= 600+180=780.
      // This departure at +700 is too soon to make the transfer.
      makeRouteAtStop("32", "B", "32", 0, {makeItem(kNow + 700)}),
  };

  PresetConfig preset;
  preset.presetName = "Home";
  preset.transferBufferMin = 3;
  preset.legs = {makeLeg("31", "A", "B", 0), makeLeg("32", "B", "C", 0)};

  auto plan = transit::planPresetTrip(routes, preset, kNow);

  TEST_ASSERT_FALSE(plan.found);
  TEST_ASSERT_EQUAL_STRING("Transfer to 32 not found", plan.fallbackMessage.c_str());
  // The first leg was matched even though the plan overall failed.
  TEST_ASSERT_EQUAL_UINT32(1, plan.legs.size());
  TEST_ASSERT_EQUAL_INT64(kNow + 300, plan.legs[0].boardEpoch);
}

void test_direction_filtering() {
  Route route;
  route.globalRouteId = "31";
  route.globalStopId = "A";
  route.routeShortName = "31";

  MergedItinerary wrongDirection;
  wrongDirection.directionId = 1;
  wrongDirection.scheduleItems = {makeItem(kNow + 50)};  // earlier, but wrong direction

  MergedItinerary rightDirection;
  rightDirection.directionId = 0;
  rightDirection.scheduleItems = {makeItem(kNow + 150)};

  route.mergedItineraries = {wrongDirection, rightDirection};

  std::vector<Route> routes = {route, makeRouteAtStop("31", "B", "31", 0, {makeItem(kNow + 400)})};

  PresetConfig preset;
  preset.presetName = "Home";
  preset.legs = {makeLeg("31", "A", "B", /*directionId=*/0)};

  auto plan = transit::planPresetTrip(routes, preset, kNow);

  TEST_ASSERT_TRUE(plan.found);
  TEST_ASSERT_EQUAL_UINT32(1, plan.legs.size());
  TEST_ASSERT_EQUAL_INT64(kNow + 150, plan.legs[0].boardEpoch);
}

void test_cancelled_item_is_skipped() {
  std::vector<Route> routes = {
      makeRouteAtStop("31", "A", "31", 0,
                       {makeItem(kNow + 100, /*cancelled=*/true), makeItem(kNow + 400)}),
      makeRouteAtStop("31", "B", "31", 0, {makeItem(kNow + 700)}),
  };

  PresetConfig preset;
  preset.presetName = "Home";
  preset.legs = {makeLeg("31", "A", "B", 0)};

  auto plan = transit::planPresetTrip(routes, preset, kNow);

  TEST_ASSERT_TRUE(plan.found);
  TEST_ASSERT_EQUAL_INT64(kNow + 400, plan.legs[0].boardEpoch);
}

void test_rttripid_preferred_over_nearest_time() {
  std::vector<Route> routes = {
      makeRouteAtStop("31", "A", "31", 0, {makeItem(kNow + 300, false, "tripX")}),
      // Two candidate passages at the alight stop: the nearest-time one has
      // a different rtTripId, but the one matching the boarding item's
      // rtTripId is preferred even though it's later.
      makeRouteAtStop("31", "B", "31", 0,
                       {makeItem(kNow + 400, false, "tripY"), makeItem(kNow + 500, false, "tripX")}),
  };

  PresetConfig preset;
  preset.presetName = "Home";
  preset.legs = {makeLeg("31", "A", "B", 0)};

  auto plan = transit::planPresetTrip(routes, preset, kNow);

  TEST_ASSERT_TRUE(plan.found);
  TEST_ASSERT_EQUAL_INT64(kNow + 500, plan.legs[0].alightEpoch);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_legs_not_configured);
  RUN_TEST(test_happy_path_two_legs);
  RUN_TEST(test_happy_path_three_legs);
  RUN_TEST(test_first_leg_not_found_fallback);
  RUN_TEST(test_transfer_infeasible_within_buffer_fallback);
  RUN_TEST(test_direction_filtering);
  RUN_TEST(test_cancelled_item_is_skipped);
  RUN_TEST(test_rttripid_preferred_over_nearest_time);
  return UNITY_END();
}
