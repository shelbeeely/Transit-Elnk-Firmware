// Host-side tests for work unit 1 (data model & JSON parsing).
//
// Fixtures are minimal-but-representative JSON matching the v4 shapes
// documented in docs/API_CONTRACT.md and docs/DATA_MODEL.md — in particular
// the v4-specific fact that `itineraries[]` and `schedule_items[]` are
// siblings under `merged_itineraries[]`, cross-referenced by
// `internal_itinerary_id`, not nested one inside the other.

#include <unity.h>

#include "transit/models.h"

namespace {

const char* kNearbyRoutesJson = R"JSON(
{
  "nearby_routes": [
    {
      "global_route_id": "1:897",
      "route_short_name": "55",
      "route_long_name": "Downtown Express",
      "route_type": 3,
      "route_color": "ff0000",
      "route_text_color": "ffffff",
      "route_display_short_name": {
        "elements": [null, "55", "right-slug"],
        "route_name_redundancy": true,
        "boxed_text": ""
      },
      "route_network_name": "Metro",
      "route_network_id": "net-1",
      "mode_name": "Bus",
      "mode_key": "bus",
      "merged_itineraries": [
        {
          "direction_id": 0,
          "closest_stop": {
            "global_stop_id": "1:94380",
            "stop_name": "Main St & 1st Ave",
            "stop_lat": 45.5,
            "stop_lon": -73.5,
            "wheelchair_boarding": 1
          },
          "itineraries": [
            {
              "internal_itinerary_id": "3:65535:false",
              "direction_id": 0,
              "headsign": "Downtown",
              "direction_headsign": "Southbound",
              "merged_headsign": "Southbound to Downtown",
              "branch_code": "A",
              "canonical_itinerary": true,
              "is_active": true
            }
          ],
          "schedule_items": [
            {
              "internal_itinerary_id": "3:65535:false",
              "departure_time": 1700000000,
              "scheduled_departure_time": 1699999900,
              "arrival_time": 1700000050,
              "scheduled_arrival_time": 1699999950,
              "is_real_time": true,
              "is_cancelled": false,
              "is_last": false,
              "wheelchair_accessible": 1,
              "rt_trip_id": "trip-123"
            },
            {
              "internal_itinerary_id": "3:65535:false",
              "departure_time": 1700000600,
              "is_real_time": false,
              "is_cancelled": true,
              "is_last": true
            }
          ]
        }
      ]
    }
  ]
}
)JSON";

const char* kStopDeparturesJson = R"JSON(
{
  "route_departures": [
    {
      "global_route_id": "1:897",
      "route_short_name": "55",
      "global_stop_id": "1:94380",
      "merged_itineraries": [
        {
          "direction_id": 1,
          "closest_stop": {
            "global_stop_id": "1:94380",
            "stop_name": "Main St & 1st Ave"
          },
          "itineraries": [
            {
              "internal_itinerary_id": "3:1:true",
              "merged_headsign": "Northbound to Airport"
            }
          ],
          "schedule_items": [
            {
              "internal_itinerary_id": "3:1:true",
              "departure_time": 1700001000,
              "is_real_time": true
            }
          ]
        }
      ]
    }
  ]
}
)JSON";

const char* kNearbyStopsJson = R"JSON(
{
  "stops": [
    {
      "global_stop_id": "1:94380",
      "stop_name": "Main St & 1st Ave",
      "stop_lat": 45.5017,
      "stop_lon": -73.5673,
      "distance": 123.4,
      "wheelchair_boarding": 2
    },
    {
      "global_stop_id": "1:94381",
      "stop_name": "Main St & 2nd Ave",
      "stop_lat": 45.503,
      "stop_lon": -73.568
    }
  ]
}
)JSON";

const char* kSearchStopsJson = R"JSON(
{
  "results": [
    {
      "distance": 50.5,
      "global_stop_id": "1:94380",
      "location_type": 0,
      "match_strength": 0.95,
      "route_type": 3,
      "stop_lat": 45.5017,
      "stop_lon": -73.5673,
      "stop_name": "Main St & 1st Ave"
    },
    {
      "distance": 200.0,
      "global_stop_id": "1:94382",
      "location_type": 2,
      "match_strength": 0.4,
      "route_type": 1,
      "stop_lat": 45.51,
      "stop_lon": -73.57,
      "stop_name": "Main St Station Entrance"
    }
  ]
}
)JSON";

}  // namespace

void test_parse_nearby_routes_stub_returns_false_on_empty_json() {
  transit::NearbyRoutesResponse out;
  TEST_ASSERT_FALSE(transit::parseNearbyRoutes("", out));
}

void test_parse_nearby_routes_parses_key_fields() {
  transit::NearbyRoutesResponse out;
  TEST_ASSERT_TRUE(transit::parseNearbyRoutes(kNearbyRoutesJson, out));
  TEST_ASSERT_EQUAL(1, out.routes.size());

  const transit::Route& route = out.routes[0];
  TEST_ASSERT_EQUAL_STRING("1:897", route.globalRouteId.c_str());
  TEST_ASSERT_EQUAL_STRING("55", route.routeShortName.c_str());
  TEST_ASSERT_EQUAL_STRING("Downtown Express", route.routeLongName.c_str());
  TEST_ASSERT_EQUAL(3, route.routeType);
  TEST_ASSERT_EQUAL_STRING("ff0000", route.routeColor.c_str());
  // Not present on nearby_routes[] — must stay at its zero-init default.
  TEST_ASSERT_EQUAL_STRING("", route.globalStopId.c_str());

  // DisplayShortName: null element -> empty string, always exactly 3 slots.
  TEST_ASSERT_EQUAL_STRING("", route.routeDisplayShortName.elements[0].c_str());
  TEST_ASSERT_EQUAL_STRING("55", route.routeDisplayShortName.elements[1].c_str());
  TEST_ASSERT_EQUAL_STRING("right-slug", route.routeDisplayShortName.elements[2].c_str());
  TEST_ASSERT_TRUE(route.routeDisplayShortName.routeNameRedundancy);

  TEST_ASSERT_EQUAL(1, route.mergedItineraries.size());
  const transit::MergedItinerary& mi = route.mergedItineraries[0];
  TEST_ASSERT_EQUAL(0, mi.directionId);
  TEST_ASSERT_EQUAL_STRING("1:94380", mi.closestStop.globalStopId.c_str());
  TEST_ASSERT_EQUAL_STRING("Main St & 1st Ave", mi.closestStop.stopName.c_str());
}

void test_parse_nearby_routes_merged_itineraries_are_sibling_arrays() {
  transit::NearbyRoutesResponse out;
  TEST_ASSERT_TRUE(transit::parseNearbyRoutes(kNearbyRoutesJson, out));
  const transit::MergedItinerary& mi = out.routes[0].mergedItineraries[0];

  // itineraries[] and schedule_items[] are flat siblings, not nested.
  TEST_ASSERT_EQUAL(1, mi.itineraries.size());
  TEST_ASSERT_EQUAL(2, mi.scheduleItems.size());

  const transit::Itinerary& it = mi.itineraries[0];
  TEST_ASSERT_EQUAL_STRING("3:65535:false", it.internalItineraryId.c_str());
  TEST_ASSERT_EQUAL_STRING("Southbound to Downtown", it.mergedHeadsign.c_str());
  TEST_ASSERT_EQUAL_STRING("A", it.branchCode.c_str());
  TEST_ASSERT_TRUE(it.canonicalItinerary);
  TEST_ASSERT_TRUE(it.isActive);

  // Both schedule items cross-reference the one itinerary by
  // internal_itinerary_id, response-scoped only.
  const transit::ScheduleItem& si0 = mi.scheduleItems[0];
  TEST_ASSERT_EQUAL_STRING("3:65535:false", si0.internalItineraryId.c_str());
  TEST_ASSERT_EQUAL_STRING(it.internalItineraryId.c_str(), si0.internalItineraryId.c_str());
  TEST_ASSERT_EQUAL_INT64(1700000000, si0.departureTimeEpoch);
  TEST_ASSERT_EQUAL_INT64(1699999900, si0.scheduledDepartureTimeEpoch);
  TEST_ASSERT_TRUE(si0.isRealTime);
  TEST_ASSERT_FALSE(si0.isCancelled);
  TEST_ASSERT_EQUAL_STRING("trip-123", si0.rtTripId.c_str());

  const transit::ScheduleItem& si1 = mi.scheduleItems[1];
  TEST_ASSERT_EQUAL_STRING("3:65535:false", si1.internalItineraryId.c_str());
  TEST_ASSERT_TRUE(si1.isCancelled);
  TEST_ASSERT_TRUE(si1.isLast);
  // Optional fields absent from this second fixture item keep their
  // zero-initialized defaults rather than erroring.
  TEST_ASSERT_EQUAL_INT64(0, si1.scheduledDepartureTimeEpoch);
  TEST_ASSERT_EQUAL_STRING("", si1.rtTripId.c_str());
}

void test_parse_stop_departures_parses_global_stop_id_and_departures() {
  transit::StopDeparturesResponse out;
  TEST_ASSERT_TRUE(transit::parseStopDepartures(kStopDeparturesJson, out));
  TEST_ASSERT_EQUAL(1, out.routeDepartures.size());

  const transit::Route& route = out.routeDepartures[0];
  TEST_ASSERT_EQUAL_STRING("1:897", route.globalRouteId.c_str());
  // Only present on stop_departures' route_departures[].
  TEST_ASSERT_EQUAL_STRING("1:94380", route.globalStopId.c_str());

  TEST_ASSERT_EQUAL(1, route.mergedItineraries.size());
  const transit::MergedItinerary& mi = route.mergedItineraries[0];
  TEST_ASSERT_EQUAL(1, mi.directionId);
  TEST_ASSERT_EQUAL(1, mi.itineraries.size());
  TEST_ASSERT_EQUAL(1, mi.scheduleItems.size());
  TEST_ASSERT_EQUAL_STRING("Northbound to Airport", mi.itineraries[0].mergedHeadsign.c_str());
  TEST_ASSERT_EQUAL_INT64(1700001000, mi.scheduleItems[0].departureTimeEpoch);
}

void test_parse_nearby_stops_parses_stops() {
  transit::NearbyStopsResponse out;
  TEST_ASSERT_TRUE(transit::parseNearbyStops(kNearbyStopsJson, out));
  TEST_ASSERT_EQUAL(2, out.stops.size());

  const transit::Stop& s0 = out.stops[0];
  TEST_ASSERT_EQUAL_STRING("1:94380", s0.globalStopId.c_str());
  TEST_ASSERT_EQUAL_STRING("Main St & 1st Ave", s0.stopName.c_str());
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 45.5017f, s0.stopLat);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 123.4f, s0.distanceMeters);
  TEST_ASSERT_EQUAL(2, s0.wheelchairBoarding);

  // distance is absent in the second fixture entry — must fall back to the
  // struct's documented "-1 = absent" default, not 0.
  const transit::Stop& s1 = out.stops[1];
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, -1.0f, s1.distanceMeters);
  TEST_ASSERT_EQUAL(0, s1.wheelchairBoarding);
}

void test_parse_search_stops_parses_results() {
  transit::SearchStopsResponse out;
  TEST_ASSERT_TRUE(transit::parseSearchStops(kSearchStopsJson, out));
  TEST_ASSERT_EQUAL(2, out.results.size());

  const transit::SearchStopResult& r0 = out.results[0];
  TEST_ASSERT_EQUAL_STRING("1:94380", r0.globalStopId.c_str());
  TEST_ASSERT_EQUAL_STRING("Main St & 1st Ave", r0.stopName.c_str());
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 50.5f, r0.distanceMeters);
  TEST_ASSERT_EQUAL(0, r0.locationType);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.95f, r0.matchStrength);
  TEST_ASSERT_EQUAL(3, r0.routeType);

  const transit::SearchStopResult& r1 = out.results[1];
  TEST_ASSERT_EQUAL(2, r1.locationType);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.4f, r1.matchStrength);
}

void test_parse_nearby_routes_rejects_malformed_json() {
  transit::NearbyRoutesResponse out;
  TEST_ASSERT_FALSE(transit::parseNearbyRoutes("{ not valid json", out));
}

void test_parse_stop_departures_rejects_missing_top_level_key() {
  transit::StopDeparturesResponse out;
  // Valid JSON, but missing the required "route_departures" key.
  TEST_ASSERT_FALSE(transit::parseStopDepartures(R"({"unexpected": []})", out));
}

void test_parse_search_stops_rejects_wrong_shaped_json() {
  transit::SearchStopsResponse out;
  // "results" present but not an array — malformed relative to the contract.
  TEST_ASSERT_FALSE(transit::parseSearchStops(R"({"results": "oops"})", out));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_nearby_routes_stub_returns_false_on_empty_json);
  RUN_TEST(test_parse_nearby_routes_parses_key_fields);
  RUN_TEST(test_parse_nearby_routes_merged_itineraries_are_sibling_arrays);
  RUN_TEST(test_parse_stop_departures_parses_global_stop_id_and_departures);
  RUN_TEST(test_parse_nearby_stops_parses_stops);
  RUN_TEST(test_parse_search_stops_parses_results);
  RUN_TEST(test_parse_nearby_routes_rejects_malformed_json);
  RUN_TEST(test_parse_stop_departures_rejects_missing_top_level_key);
  RUN_TEST(test_parse_search_stops_rejects_wrong_shaped_json);
  return UNITY_END();
}
