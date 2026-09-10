// Host-side tests for work unit 4 (UI/business logic) — buildDepartureBoard.
// Constructs transit::Route/MergedItinerary/Itinerary/ScheduleItem structs
// directly (no JSON needed) to exercise each rule in docs/UI_BEHAVIOR.md /
// include/transit/ui_logic.h's doc comment independently.

#include <unity.h>

#include "transit/ui_logic.h"

using transit::DirectionBoard;
using transit::Itinerary;
using transit::MergedItinerary;
using transit::Route;
using transit::ScheduleItem;
using transit::UiSettings;

namespace {

constexpr int64_t kNow = 1'700'000'000;

ScheduleItem makeItem(const std::string& internalItineraryId, int64_t departureEpoch,
                       bool cancelled = false, bool realTime = false, bool last = false) {
  ScheduleItem item;
  item.internalItineraryId = internalItineraryId;
  item.departureTimeEpoch = departureEpoch;
  item.scheduledDepartureTimeEpoch = departureEpoch;
  item.isCancelled = cancelled;
  item.isRealTime = realTime;
  item.isLast = last;
  return item;
}

Itinerary makeItinerary(const std::string& internalItineraryId, int directionId,
                         const std::string& mergedHeadsign) {
  Itinerary itin;
  itin.internalItineraryId = internalItineraryId;
  itin.directionId = directionId;
  itin.mergedHeadsign = mergedHeadsign;
  return itin;
}

// One route, one direction, with a single itinerary "it0" and the given
// schedule items (all pointing at "it0").
Route makeSimpleRoute(const std::string& globalRouteId, const std::string& shortName,
                       const std::string& stopName, const std::string& headsign,
                       std::vector<ScheduleItem> items) {
  Route route;
  route.globalRouteId = globalRouteId;
  route.routeShortName = shortName;

  MergedItinerary mi;
  mi.directionId = 0;
  mi.closestStop.stopName = stopName;
  mi.itineraries.push_back(makeItinerary("it0", 0, headsign));
  mi.scheduleItems = std::move(items);

  route.mergedItineraries.push_back(std::move(mi));
  return route;
}

}  // namespace

// --- Rule 1: hiddenRoutes -------------------------------------------------

void test_hidden_route_is_excluded() {
  Route hidden = makeSimpleRoute("R1", "55", "Main St", "Northbound to Downtown",
                                  {makeItem("it0", kNow + 60)});
  Route visible = makeSimpleRoute("R2", "10", "Elm St", "Southbound to Uptown",
                                   {makeItem("it0", kNow + 120)});

  UiSettings settings;
  settings.hiddenRoutes = {"R1"};

  auto board = transit::buildDepartureBoard({hidden, visible}, settings, kNow);

  TEST_ASSERT_EQUAL_UINT32(1, board.size());
  TEST_ASSERT_EQUAL_STRING("R2", board[0].globalRouteId.c_str());
}

// --- Rule 2: cancelled items -----------------------------------------------

void test_cancelled_item_is_excluded() {
  Route route = makeSimpleRoute(
      "R1", "55", "Main St", "Northbound to Downtown",
      {makeItem("it0", kNow + 60, /*cancelled=*/true), makeItem("it0", kNow + 300)});

  UiSettings settings;
  auto board = transit::buildDepartureBoard({route}, settings, kNow);

  TEST_ASSERT_EQUAL_UINT32(1, board.size());
  TEST_ASSERT_EQUAL_UINT32(1, board[0].departures.size());
  TEST_ASSERT_EQUAL_INT64(kNow + 300, board[0].departures[0].departureTimeEpoch);
}

// --- Rule 3: departure window ----------------------------------------------

void test_out_of_window_items_are_excluded() {
  UiSettings settings;
  settings.departureWindowMin = 10;  // window = [now, now+600)

  Route route = makeSimpleRoute("R1", "55", "Main St", "Northbound to Downtown",
                                 {
                                     makeItem("it0", kNow - 60),    // before now: excluded
                                     makeItem("it0", kNow),         // exactly now: included
                                     makeItem("it0", kNow + 300),   // inside window: included
                                     makeItem("it0", kNow + 600),   // exactly at end: excluded
                                     makeItem("it0", kNow + 1200),  // past window: excluded
                                 });

  auto board = transit::buildDepartureBoard({route}, settings, kNow);

  TEST_ASSERT_EQUAL_UINT32(1, board.size());
  TEST_ASSERT_EQUAL_UINT32(2, board[0].departures.size());
  TEST_ASSERT_EQUAL_INT64(kNow, board[0].departures[0].departureTimeEpoch);
  TEST_ASSERT_EQUAL_INT64(kNow + 300, board[0].departures[1].departureTimeEpoch);
}

// --- Rule 4: cap per direction, nearest-first -------------------------------

void test_departures_capped_to_max_nearest_first() {
  UiSettings settings;
  settings.maxDeparturesPerDirection = 3;
  settings.departureWindowMin = 200;  // wide enough to keep all 5

  // Deliberately out of chronological order to also prove the sort.
  Route route = makeSimpleRoute("R1", "55", "Main St", "Northbound to Downtown",
                                 {
                                     makeItem("it0", kNow + 500),
                                     makeItem("it0", kNow + 100),
                                     makeItem("it0", kNow + 300),
                                     makeItem("it0", kNow + 200),
                                     makeItem("it0", kNow + 400),
                                 });

  auto board = transit::buildDepartureBoard({route}, settings, kNow);

  TEST_ASSERT_EQUAL_UINT32(1, board.size());
  TEST_ASSERT_EQUAL_UINT32(3, board[0].departures.size());
  TEST_ASSERT_EQUAL_INT64(kNow + 100, board[0].departures[0].departureTimeEpoch);
  TEST_ASSERT_EQUAL_INT64(kNow + 200, board[0].departures[1].departureTimeEpoch);
  TEST_ASSERT_EQUAL_INT64(kNow + 300, board[0].departures[2].departureTimeEpoch);
}

// --- Rule 5: routeOrder ------------------------------------------------------

void test_route_order_reorders_output_unlisted_routes_appended_after() {
  Route a = makeSimpleRoute("A", "1", "Stop A", "To A", {makeItem("it0", kNow + 60)});
  Route b = makeSimpleRoute("B", "2", "Stop B", "To B", {makeItem("it0", kNow + 60)});
  Route c = makeSimpleRoute("C", "3", "Stop C", "To C", {makeItem("it0", kNow + 60)});

  UiSettings settings;
  settings.routeOrder = {"C", "A"};  // B is unlisted

  auto board = transit::buildDepartureBoard({a, b, c}, settings, kNow);

  TEST_ASSERT_EQUAL_UINT32(3, board.size());
  TEST_ASSERT_EQUAL_STRING("C", board[0].globalRouteId.c_str());
  TEST_ASSERT_EQUAL_STRING("A", board[1].globalRouteId.c_str());
  TEST_ASSERT_EQUAL_STRING("B", board[2].globalRouteId.c_str());
}

// --- Rule 7: staticDirection --------------------------------------------------

void test_static_direction_filters_to_one_direction() {
  Route route;
  route.globalRouteId = "R1";
  route.routeShortName = "55";

  MergedItinerary dir0;
  dir0.directionId = 0;
  dir0.closestStop.stopName = "Outbound Stop";
  dir0.itineraries.push_back(makeItinerary("it0", 0, "Outbound"));
  dir0.scheduleItems = {makeItem("it0", kNow + 60)};

  MergedItinerary dir1;
  dir1.directionId = 1;
  dir1.closestStop.stopName = "Inbound Stop";
  dir1.itineraries.push_back(makeItinerary("it1", 1, "Inbound"));
  dir1.scheduleItems = {makeItem("it1", kNow + 120)};

  route.mergedItineraries = {dir0, dir1};

  UiSettings settings;
  settings.staticDirection = 1;

  auto board = transit::buildDepartureBoard({route}, settings, kNow);

  TEST_ASSERT_EQUAL_UINT32(1, board.size());
  TEST_ASSERT_EQUAL_INT32(1, board[0].directionId);
  TEST_ASSERT_EQUAL_UINT32(1, board[0].departures.size());
  TEST_ASSERT_EQUAL_STRING("Inbound Stop", board[0].departures[0].stopName.c_str());
  TEST_ASSERT_EQUAL_STRING("Inbound", board[0].departures[0].headsign.c_str());
}

// --- Rule 6: sortByTime -------------------------------------------------------

void test_sort_by_time_orders_boards_by_earliest_departure() {
  // Two routes, API order A then B, but B's soonest departure is earlier.
  Route a = makeSimpleRoute("A", "1", "Stop A", "To A", {makeItem("it0", kNow + 600)});
  Route b = makeSimpleRoute("B", "2", "Stop B", "To B", {makeItem("it0", kNow + 60)});

  UiSettings settings;
  settings.sortByTime = true;

  auto board = transit::buildDepartureBoard({a, b}, settings, kNow);

  TEST_ASSERT_EQUAL_UINT32(2, board.size());
  TEST_ASSERT_EQUAL_STRING("B", board[0].globalRouteId.c_str());
  TEST_ASSERT_EQUAL_STRING("A", board[1].globalRouteId.c_str());
}

// --- headsign / stopName / badge field mapping --------------------------------

void test_departure_row_fields_are_mapped_correctly() {
  Route route;
  route.globalRouteId = "R1";
  route.routeShortName = "55";

  MergedItinerary mi;
  mi.directionId = 0;
  mi.closestStop.stopName = "Main St & 5th";
  mi.itineraries.push_back(makeItinerary("branchA", 0, "Northbound to Downtown"));
  mi.itineraries.push_back(makeItinerary("branchB", 0, "Northbound to Uptown"));
  mi.scheduleItems = {
      makeItem("branchB", kNow + 90, /*cancelled=*/false, /*realTime=*/true, /*last=*/true),
  };
  route.mergedItineraries.push_back(mi);

  UiSettings settings;
  auto board = transit::buildDepartureBoard({route}, settings, kNow);

  TEST_ASSERT_EQUAL_UINT32(1, board.size());
  TEST_ASSERT_EQUAL_UINT32(1, board[0].departures.size());
  const auto& row = board[0].departures[0];
  TEST_ASSERT_EQUAL_STRING("Northbound to Uptown", row.headsign.c_str());
  TEST_ASSERT_EQUAL_STRING("Main St & 5th", row.stopName.c_str());
  TEST_ASSERT_TRUE(row.isRealTime);
  TEST_ASSERT_TRUE(row.isLast);
}

// --- headsign falls back to plain headsign when mergedHeadsign is empty -------

void test_headsign_falls_back_when_merged_headsign_empty() {
  Route route;
  route.globalRouteId = "R1";
  route.routeShortName = "55";

  MergedItinerary mi;
  mi.directionId = 0;
  mi.closestStop.stopName = "Main St";
  Itinerary itin;
  itin.internalItineraryId = "it0";
  itin.directionId = 0;
  itin.headsign = "Downtown";
  itin.mergedHeadsign = "";  // unset by this agency
  mi.itineraries.push_back(itin);
  mi.scheduleItems = {makeItem("it0", kNow + 60)};
  route.mergedItineraries.push_back(mi);

  UiSettings settings;
  auto board = transit::buildDepartureBoard({route}, settings, kNow);

  TEST_ASSERT_EQUAL_UINT32(1, board.size());
  TEST_ASSERT_EQUAL_STRING("Downtown", board[0].departures[0].headsign.c_str());
}

// --- maxDeparturesPerDirection <= 0 is treated as "no cap", not "show none" --

void test_non_positive_max_departures_is_treated_as_unbounded() {
  Route route = makeSimpleRoute("R1", "55", "Main St", "Northbound to Downtown",
                                 {makeItem("it0", kNow + 60), makeItem("it0", kNow + 120)});

  UiSettings settings;
  settings.maxDeparturesPerDirection = 0;

  auto board = transit::buildDepartureBoard({route}, settings, kNow);

  TEST_ASSERT_EQUAL_UINT32(1, board.size());
  TEST_ASSERT_EQUAL_UINT32(2, board[0].departures.size());
}

// --- A direction with nothing upcoming produces no board at all --------------

void test_direction_with_no_surviving_departures_is_dropped() {
  Route route = makeSimpleRoute("R1", "55", "Main St", "Northbound to Downtown",
                                 {makeItem("it0", kNow - 60), makeItem("it0", kNow + 60,
                                                                        /*cancelled=*/true)});

  UiSettings settings;
  auto board = transit::buildDepartureBoard({route}, settings, kNow);

  TEST_ASSERT_TRUE(board.empty());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_hidden_route_is_excluded);
  RUN_TEST(test_cancelled_item_is_excluded);
  RUN_TEST(test_out_of_window_items_are_excluded);
  RUN_TEST(test_departures_capped_to_max_nearest_first);
  RUN_TEST(test_route_order_reorders_output_unlisted_routes_appended_after);
  RUN_TEST(test_static_direction_filters_to_one_direction);
  RUN_TEST(test_sort_by_time_orders_boards_by_earliest_departure);
  RUN_TEST(test_departure_row_fields_are_mapped_correctly);
  RUN_TEST(test_headsign_falls_back_when_merged_headsign_empty);
  RUN_TEST(test_non_positive_max_departures_is_treated_as_unbounded);
  RUN_TEST(test_direction_with_no_surviving_departures_is_dropped);
  return UNITY_END();
}
