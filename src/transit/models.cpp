// JSON parsing for the Transit API v4 endpoints (work unit 1).
//
// Field-name mapping and JSON shapes follow docs/API_CONTRACT.md and
// docs/DATA_MODEL.md exactly. See models.h for the struct contracts this
// fills in. Uses ArduinoJson v7 (JsonDocument, deserializeJson) which is
// available identically under [env:xteink_x4] and [env:native].

#include "transit/models.h"

#include <ArduinoJson.h>

namespace transit {

namespace {

// Pulls a string field, defaulting to "" when absent/null (ArduinoJson's
// JsonVariantConst::as<std::string>() returns "" for a null/missing value,
// which matches the struct's zero-initialized default).
std::string getStr(JsonVariantConst v) {
  if (v.isNull()) return std::string();
  const char* s = v.as<const char*>();
  return s ? std::string(s) : std::string();
}

void parseDisplayShortName(JsonVariantConst v, DisplayShortName& out) {
  out = DisplayShortName{};
  if (v.isNull()) return;
  JsonArrayConst elements = v["elements"].as<JsonArrayConst>();
  if (!elements.isNull()) {
    size_t i = 0;
    for (JsonVariantConst el : elements) {
      if (i >= 3) break;
      out.elements[i] = getStr(el);
      ++i;
    }
  }
  out.routeNameRedundancy = v["route_name_redundancy"] | false;
  out.boxedText = getStr(v["boxed_text"]);
}

void parseStop(JsonVariantConst v, Stop& out) {
  out = Stop{};
  if (v.isNull()) return;
  out.globalStopId = getStr(v["global_stop_id"]);
  out.stopName = getStr(v["stop_name"]);
  out.stopLat = v["stop_lat"] | 0.0;
  out.stopLon = v["stop_lon"] | 0.0;
  out.distanceMeters = v["distance"] | -1.0;
  out.wheelchairBoarding = v["wheelchair_boarding"] | 0;
}

void parseItinerary(JsonVariantConst v, Itinerary& out) {
  out = Itinerary{};
  out.internalItineraryId = getStr(v["internal_itinerary_id"]);
  out.directionId = v["direction_id"] | 0;
  out.headsign = getStr(v["headsign"]);
  out.directionHeadsign = getStr(v["direction_headsign"]);
  out.mergedHeadsign = getStr(v["merged_headsign"]);
  out.branchCode = getStr(v["branch_code"]);
  out.canonicalItinerary = v["canonical_itinerary"] | false;
  out.isActive = v["is_active"] | false;
}

void parseScheduleItem(JsonVariantConst v, ScheduleItem& out) {
  out = ScheduleItem{};
  out.internalItineraryId = getStr(v["internal_itinerary_id"]);
  out.departureTimeEpoch = v["departure_time"] | (int64_t)0;
  out.scheduledDepartureTimeEpoch = v["scheduled_departure_time"] | (int64_t)0;
  out.arrivalTimeEpoch = v["arrival_time"] | (int64_t)0;
  out.scheduledArrivalTimeEpoch = v["scheduled_arrival_time"] | (int64_t)0;
  out.isRealTime = v["is_real_time"] | false;
  out.isCancelled = v["is_cancelled"] | false;
  out.isLast = v["is_last"] | false;
  out.wheelchairAccessible = v["wheelchair_accessible"] | 0;
  out.rtTripId = getStr(v["rt_trip_id"]);
}

void parseMergedItinerary(JsonVariantConst v, MergedItinerary& out) {
  out = MergedItinerary{};
  out.directionId = v["direction_id"] | 0;
  parseStop(v["closest_stop"], out.closestStop);

  JsonArrayConst itineraries = v["itineraries"].as<JsonArrayConst>();
  if (!itineraries.isNull()) {
    out.itineraries.reserve(itineraries.size());
    for (JsonVariantConst it : itineraries) {
      parseItinerary(it, out.itineraries.emplace_back());
    }
  }

  JsonArrayConst scheduleItems = v["schedule_items"].as<JsonArrayConst>();
  if (!scheduleItems.isNull()) {
    out.scheduleItems.reserve(scheduleItems.size());
    for (JsonVariantConst si : scheduleItems) {
      parseScheduleItem(si, out.scheduleItems.emplace_back());
    }
  }
}

void parseRoute(JsonVariantConst v, Route& out) {
  out = Route{};
  out.globalRouteId = getStr(v["global_route_id"]);
  out.routeShortName = getStr(v["route_short_name"]);
  out.routeLongName = getStr(v["route_long_name"]);
  out.routeType = v["route_type"] | 3;
  out.routeColor = getStr(v["route_color"]);
  out.routeTextColor = getStr(v["route_text_color"]);
  parseDisplayShortName(v["route_display_short_name"], out.routeDisplayShortName);
  out.routeNetworkName = getStr(v["route_network_name"]);
  out.routeNetworkId = getStr(v["route_network_id"]);
  out.modeName = getStr(v["mode_name"]);
  out.modeKey = getStr(v["mode_key"]);

  JsonArrayConst mergedItineraries = v["merged_itineraries"].as<JsonArrayConst>();
  if (!mergedItineraries.isNull()) {
    out.mergedItineraries.reserve(mergedItineraries.size());
    for (JsonVariantConst mi : mergedItineraries) {
      parseMergedItinerary(mi, out.mergedItineraries.emplace_back());
    }
  }

  // Only present on stop_departures' route_departures[]; absent on
  // nearby_routes[] leaves this at its default-constructed "".
  out.globalStopId = getStr(v["global_stop_id"]);
}

void parseSearchStopResult(JsonVariantConst v, SearchStopResult& out) {
  out = SearchStopResult{};
  out.globalStopId = getStr(v["global_stop_id"]);
  out.stopName = getStr(v["stop_name"]);
  out.stopLat = v["stop_lat"] | 0.0;
  out.stopLon = v["stop_lon"] | 0.0;
  out.distanceMeters = v["distance"] | 0.0;
  out.locationType = v["location_type"] | 0;
  out.matchStrength = v["match_strength"] | 0.0;
  out.routeType = v["route_type"] | 3;
}

}  // namespace

bool parseNearbyRoutes(const std::string& json, NearbyRoutesResponse& out) {
  out = NearbyRoutesResponse{};
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  if (!doc["nearby_routes"].is<JsonArrayConst>()) return false;

  JsonArrayConst routes = doc["nearby_routes"].as<JsonArrayConst>();
  out.routes.reserve(routes.size());
  for (JsonVariantConst r : routes) {
    parseRoute(r, out.routes.emplace_back());
  }
  return true;
}

bool parseStopDepartures(const std::string& json, StopDeparturesResponse& out) {
  out = StopDeparturesResponse{};
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  if (!doc["route_departures"].is<JsonArrayConst>()) return false;

  JsonArrayConst routeDepartures = doc["route_departures"].as<JsonArrayConst>();
  out.routeDepartures.reserve(routeDepartures.size());
  for (JsonVariantConst r : routeDepartures) {
    parseRoute(r, out.routeDepartures.emplace_back());
  }
  return true;
}

bool parseNearbyStops(const std::string& json, NearbyStopsResponse& out) {
  out = NearbyStopsResponse{};
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  if (!doc["stops"].is<JsonArrayConst>()) return false;

  JsonArrayConst stops = doc["stops"].as<JsonArrayConst>();
  out.stops.reserve(stops.size());
  for (JsonVariantConst s : stops) {
    parseStop(s, out.stops.emplace_back());
  }
  return true;
}

bool parseSearchStops(const std::string& json, SearchStopsResponse& out) {
  out = SearchStopsResponse{};
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  if (!doc["results"].is<JsonArrayConst>()) return false;

  JsonArrayConst results = doc["results"].as<JsonArrayConst>();
  out.results.reserve(results.size());
  for (JsonVariantConst r : results) {
    parseSearchStopResult(r, out.results.emplace_back());
  }
  return true;
}

}  // namespace transit
