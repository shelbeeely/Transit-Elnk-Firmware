// Host-side tests for transit::sta::staDeparturesToRoutes (sta_models.h) --
// the adapter that lets main.cpp hand STA's departures to the same
// ui_logic::buildDepartureBoard() pipeline the Transit API's routes go
// through.

#include <unity.h>

#include "transit/sta_models.h"
#include "transit/ui_logic.h"

using transit::DirectionBoard;
using transit::Route;
using transit::UiSettings;
using transit::buildDepartureBoard;
using transit::sta::StaDeparture;
using transit::sta::staDeparturesToRoutes;

namespace {

StaDeparture makeDeparture(const std::string& routeId, const std::string& tripId,
                           const std::string& destination, int64_t departureEpoch) {
  StaDeparture dep;
  dep.routeId = routeId;
  dep.routeShortName = routeId;
  dep.routeColor = 0x3155A6;
  dep.routeTextColor = 0xFFFFFF;
  dep.tripId = tripId;
  dep.destination = destination;
  dep.departureEpoch = departureEpoch;
  return dep;
}

}  // namespace

void test_empty_input_yields_no_routes() {
  std::vector<Route> routes = staDeparturesToRoutes({}, "Main St & 5th Ave");
  TEST_ASSERT_TRUE(routes.empty());
}

void test_groups_multiple_departures_on_the_same_route_into_one_route() {
  std::vector<StaDeparture> deps = {
      makeDeparture("34", "trip1", "South Hill P&R", 1000),
      makeDeparture("34", "trip2", "South Hill P&R", 2000),
  };
  std::vector<Route> routes = staDeparturesToRoutes(deps, "SCC Transit Center Bay 3");

  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(routes.size()));
  TEST_ASSERT_EQUAL_STRING("sta:34", routes[0].globalRouteId.c_str());
  TEST_ASSERT_EQUAL_STRING("STA 34", routes[0].routeShortName.c_str());
  TEST_ASSERT_EQUAL_STRING("3155A6", routes[0].routeColor.c_str());
  TEST_ASSERT_EQUAL_STRING("FFFFFF", routes[0].routeTextColor.c_str());
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(routes[0].mergedItineraries.size()));

  const auto& mi = routes[0].mergedItineraries[0];
  TEST_ASSERT_EQUAL_INT(0, mi.directionId);
  TEST_ASSERT_EQUAL_STRING("SCC Transit Center Bay 3", mi.closestStop.stopName.c_str());
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(mi.itineraries.size()));
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(mi.scheduleItems.size()));
}

void test_different_routes_become_separate_routes() {
  std::vector<StaDeparture> deps = {
      makeDeparture("34", "trip1", "South Hill P&R", 1000),
      makeDeparture("43", "trip2", "Lincoln/37th Ave", 2000),
  };
  std::vector<Route> routes = staDeparturesToRoutes(deps, "SCC Transit Center Bay 3");
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(routes.size()));
  TEST_ASSERT_EQUAL_STRING("sta:34", routes[0].globalRouteId.c_str());
  TEST_ASSERT_EQUAL_STRING("sta:43", routes[1].globalRouteId.c_str());
}

void test_itinerary_and_schedule_item_are_linked_and_carry_the_destination() {
  std::vector<StaDeparture> deps = {makeDeparture("34", "trip1", "South Hill P&R", 1000)};
  std::vector<Route> routes = staDeparturesToRoutes(deps, "SCC Transit Center Bay 3");

  const auto& mi = routes[0].mergedItineraries[0];
  TEST_ASSERT_EQUAL_STRING("trip1", mi.itineraries[0].internalItineraryId.c_str());
  TEST_ASSERT_EQUAL_STRING("South Hill P&R", mi.itineraries[0].mergedHeadsign.c_str());
  TEST_ASSERT_EQUAL_STRING("trip1", mi.scheduleItems[0].internalItineraryId.c_str());
  TEST_ASSERT_EQUAL_INT64(1000, mi.scheduleItems[0].departureTimeEpoch);
  TEST_ASSERT_TRUE(mi.scheduleItems[0].isRealTime);
  TEST_ASSERT_FALSE(mi.scheduleItems[0].isCancelled);
}

void test_empty_destination_falls_back_to_route_label() {
  std::vector<StaDeparture> deps = {makeDeparture("999", "trip1", "", 1000)};
  std::vector<Route> routes = staDeparturesToRoutes(deps, "SCC Transit Center Bay 3");
  TEST_ASSERT_EQUAL_STRING("Route 999", routes[0].mergedItineraries[0].itineraries[0].mergedHeadsign.c_str());
}

// Confirms the adapter's output is actually usable by the existing,
// unmodified ui_logic pipeline end to end -- not just structurally similar
// to a Route.
void test_output_flows_through_build_departure_board() {
  std::vector<StaDeparture> deps = {makeDeparture("34", "trip1", "South Hill P&R", 1000)};
  std::vector<Route> routes = staDeparturesToRoutes(deps, "SCC Transit Center Bay 3");

  UiSettings settings;
  settings.departureWindowMin = 60;
  std::vector<DirectionBoard> boards = buildDepartureBoard(routes, settings, /*nowEpoch=*/0);

  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(boards.size()));
  TEST_ASSERT_EQUAL_STRING("STA 34", boards[0].routeShortName.c_str());
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(boards[0].departures.size()));
  TEST_ASSERT_EQUAL_STRING("South Hill P&R", boards[0].departures[0].headsign.c_str());
  TEST_ASSERT_EQUAL_STRING("SCC Transit Center Bay 3", boards[0].departures[0].stopName.c_str());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_input_yields_no_routes);
  RUN_TEST(test_groups_multiple_departures_on_the_same_route_into_one_route);
  RUN_TEST(test_different_routes_become_separate_routes);
  RUN_TEST(test_itinerary_and_schedule_item_are_linked_and_carry_the_destination);
  RUN_TEST(test_empty_destination_falls_back_to_route_label);
  RUN_TEST(test_output_flows_through_build_departure_board);
  return UNITY_END();
}
