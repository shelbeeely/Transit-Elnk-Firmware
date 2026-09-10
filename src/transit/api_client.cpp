// Stub implementation — replaced by work unit 2 (Transit API client).
// Exists so the scaffold links and runs end to end before that unit lands.

#include "transit/api_client.h"

namespace transit {

TransitApiClient::TransitApiClient(HttpTransport& transport, std::string apiKey)
    : transport_(transport), apiKey_(std::move(apiKey)) {}

void TransitApiClient::setApiKey(std::string apiKey) { apiKey_ = std::move(apiKey); }

bool TransitApiClient::nearbyRoutes(double /*lat*/, double /*lon*/,
                                     const NearbyRoutesParams& /*params*/,
                                     NearbyRoutesResponse& out) {
  out = NearbyRoutesResponse{};
  return false;
}

bool TransitApiClient::stopDepartures(const std::vector<std::string>& /*globalStopIds*/,
                                       const StopDeparturesParams& /*params*/,
                                       StopDeparturesResponse& out) {
  out = StopDeparturesResponse{};
  return false;
}

bool TransitApiClient::nearbyStops(double /*lat*/, double /*lon*/,
                                    const NearbyStopsParams& /*params*/,
                                    NearbyStopsResponse& out) {
  out = NearbyStopsResponse{};
  return false;
}

bool TransitApiClient::searchStops(double /*lat*/, double /*lon*/, const std::string& /*query*/,
                                    const SearchStopsParams& /*params*/,
                                    SearchStopsResponse& out) {
  out = SearchStopsResponse{};
  return false;
}

}  // namespace transit
