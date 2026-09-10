// Stub implementation — replaced by work unit 1 (data model & JSON parsing).
// Exists so the scaffold links and runs end to end before that unit lands.

#include "transit/models.h"

namespace transit {

bool parseNearbyRoutes(const std::string& /*json*/, NearbyRoutesResponse& out) {
  out = NearbyRoutesResponse{};
  return false;
}

bool parseStopDepartures(const std::string& /*json*/, StopDeparturesResponse& out) {
  out = StopDeparturesResponse{};
  return false;
}

bool parseNearbyStops(const std::string& /*json*/, NearbyStopsResponse& out) {
  out = NearbyStopsResponse{};
  return false;
}

bool parseSearchStops(const std::string& /*json*/, SearchStopsResponse& out) {
  out = SearchStopsResponse{};
  return false;
}

}  // namespace transit
