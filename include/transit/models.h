#pragma once

// Transit-Elnk-Firmware — canonical v4 data shapes.
//
// Field names/types/meanings match docs/DATA_MODEL.md exactly. Kept
// framework-agnostic (std:: only, no Arduino/FreeInk types) so it compiles
// identically under [env:xteink_x4] and [env:native] for host-side testing.
//
// Frozen contract for the parallel work units: do not change field names or
// remove fields. Adding a field is fine; note it in your PR description.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace transit {

// route.route_display_short_name / compact_display_short_name.
// elements is always exactly 3: [leftImageSlug|empty, text, rightImageSlug|empty].
// See docs/ASSETS_ICONS.md for the image-slug -> URL convention.
struct DisplayShortName {
  std::string elements[3];
  bool routeNameRedundancy = false;
  std::string boxedText;
};

// Stop / StopDetailed (docs/DATA_MODEL.md "Stop").
struct Stop {
  std::string globalStopId;
  std::string stopName;
  double stopLat = 0.0;
  double stopLon = 0.0;
  // Meters from the query point. Only present on nearby_stops/search_stops
  // results, not on merged_itineraries[].closest_stop. -1 = absent.
  double distanceMeters = -1.0;
  int wheelchairBoarding = 0;  // 0/1/2
};

// merged_itineraries[].itineraries[] (docs/DATA_MODEL.md "Itinerary").
struct Itinerary {
  std::string internalItineraryId;  // response-scoped only, do not persist
  int directionId = 0;
  std::string headsign;
  std::string directionHeadsign;
  // Prefer this field for the on-panel direction label (see DATA_MODEL.md).
  std::string mergedHeadsign;
  std::string branchCode;
  bool canonicalItinerary = false;
  bool isActive = false;
};

// merged_itineraries[].schedule_items[] (docs/DATA_MODEL.md "ScheduleItem").
struct ScheduleItem {
  std::string internalItineraryId;  // -> Itinerary::internalItineraryId
  int64_t departureTimeEpoch = 0;
  int64_t scheduledDepartureTimeEpoch = 0;
  int64_t arrivalTimeEpoch = 0;
  int64_t scheduledArrivalTimeEpoch = 0;
  bool isRealTime = false;
  bool isCancelled = false;
  bool isLast = false;
  int wheelchairAccessible = 0;  // 0/1/2
  std::string rtTripId;
};

// route.merged_itineraries[] (docs/DATA_MODEL.md "MergedItinerary").
struct MergedItinerary {
  int directionId = 0;
  Stop closestStop;
  std::vector<Itinerary> itineraries;
  std::vector<ScheduleItem> scheduleItems;
};

// nearby_routes[] / route_departures[] (docs/DATA_MODEL.md "Route").
struct Route {
  std::string globalRouteId;
  std::string routeShortName;
  std::string routeLongName;
  int routeType = 3;  // GTFS route type, default 3 = bus
  std::string routeColor;      // hex, no '#'
  std::string routeTextColor;  // hex, no '#'
  DisplayShortName routeDisplayShortName;
  std::string routeNetworkName;
  std::string routeNetworkId;
  std::string modeName;
  std::string modeKey;
  std::vector<MergedItinerary> mergedItineraries;
  // Only present on stop_departures' route_departures[], identifies which
  // queried stop this entry's departures came from.
  std::string globalStopId;
};

// GET /v4/public/nearby_routes response.
struct NearbyRoutesResponse {
  std::vector<Route> routes;
};

// GET /v4/public/stop_departures response.
struct StopDeparturesResponse {
  std::vector<Route> routeDepartures;
};

// GET /v4/public/nearby_stops response.
struct NearbyStopsResponse {
  std::vector<Stop> stops;
};

// One entry of GET /v4/public/search_stops's "results" array
// (docs/API_CONTRACT.md — a distinct, smaller shape from Stop).
struct SearchStopResult {
  std::string globalStopId;
  std::string stopName;
  double stopLat = 0.0;
  double stopLon = 0.0;
  double distanceMeters = 0.0;
  int locationType = 0;
  double matchStrength = 0.0;  // 0-1, higher is better
  int routeType = 3;
};

struct SearchStopsResponse {
  std::vector<SearchStopResult> results;
};

// --- JSON parsing (implemented in src/transit/models.cpp, unit 1) ---------
//
// Each parses one v4 endpoint's raw JSON response body. Returns false (and
// leaves `out` unspecified) on malformed/unexpected JSON; never throws.

bool parseNearbyRoutes(const std::string& json, NearbyRoutesResponse& out);
bool parseStopDepartures(const std::string& json, StopDeparturesResponse& out);
bool parseNearbyStops(const std::string& json, NearbyStopsResponse& out);
bool parseSearchStops(const std::string& json, SearchStopsResponse& out);

}  // namespace transit
