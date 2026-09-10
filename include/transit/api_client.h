#pragma once

// Transit-Elnk-Firmware — Transit API v4 client.
//
// Endpoints/params/defaults match docs/API_CONTRACT.md exactly. Base host:
// https://external.transitapp.com. Auth: header "apiKey: <key>".
//
// HTTP I/O is behind HttpTransport so this header (and TransitApiClient's
// request-building logic) compiles and is unit-testable under [env:native]
// without FreeInk/Arduino/WiFi. The real [env:xteink_x4] build supplies a
// transport backed by the Arduino core's WiFiClientSecure/HTTPClient (see
// docs/API_CONTRACT.md's HTTPS note in the plan: external.transitapp.com is
// a standard TLS 1.2 REST API, no need for FreeInk's SecureNet/wolfSSL).
//
// Frozen contract for the parallel work units: do not change existing method
// signatures. Adding a method/param is fine; note it in your PR description.

#include <string>
#include <utility>
#include <vector>

#include "transit/models.h"

namespace transit {

struct HttpResponse {
  int statusCode = 0;
  std::string body;
  bool transportOk = false;  // false = connection/DNS/TLS failure, no statusCode
};

// Injected HTTP transport. One GET call per endpoint; the Transit API v4
// surface used here is entirely query-param GET requests.
class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual HttpResponse get(const std::string& url,
                            const std::vector<std::pair<std::string, std::string>>& headers) = 0;
};

// Shared optional query params across nearby_routes/stop_departures
// (docs/API_CONTRACT.md). -1 / empty = "not set, use server default".
struct DepartureQueryParams {
  int maxNumDepartures = 3;      // 1-10, per merged itinerary
  bool shouldUpdateRealtime = true;
  bool mergePlatformStops = false;
  int64_t timeEpoch = 0;         // 0 = now
  bool includeStopsAndShapes = false;
  bool stopDetailed = false;
  std::string locale;
};

struct NearbyRoutesParams : DepartureQueryParams {
  int maxDistanceMeters = 150;  // max 1500
};

struct StopDeparturesParams : DepartureQueryParams {
  bool removeCancelled = false;
  bool excludeTerminalArrivals = false;
};

struct NearbyStopsParams {
  int maxDistanceMeters = 150;  // max 1500
  std::string stopFilter = "Routable";  // Routable/EntrancesAndStopsOutsideStations/Entrances/Any
  std::string pickupDropoffFilter;      // PickupAllowedOnly/DropoffAllowedOnly/Everything
  bool stopDetailed = false;
  bool includeBetaFeeds = false;
  std::string locale;
};

struct SearchStopsParams {
  int maxNumResults = 10;  // 1-50
  int maxDistanceMeters = -1;  // -1 = unset, no distance filtering
  std::string pickupDropoffFilter;
  bool mergeSimilarStops = false;
  std::string locale;
};

class TransitApiClient {
 public:
  TransitApiClient(HttpTransport& transport, std::string apiKey);

  // Updates the key used by subsequent calls. Needed because main.cpp
  // constructs TransitApiClient before the first-run setup flow may have
  // collected a fresh key (setup_flow.h's runFirstTimeSetup) — call this
  // once the user has entered/confirmed one, then validate/search with the
  // same client instance instead of constructing a second one.
  void setApiKey(std::string apiKey);

  // GET /v4/public/nearby_routes
  bool nearbyRoutes(double lat, double lon, const NearbyRoutesParams& params,
                     NearbyRoutesResponse& out);

  // GET /v4/public/stop_departures. Up to 100 stop IDs per call.
  bool stopDepartures(const std::vector<std::string>& globalStopIds,
                      const StopDeparturesParams& params, StopDeparturesResponse& out);

  // GET /v4/public/nearby_stops — used during first-run setup's stop picker.
  bool nearbyStops(double lat, double lon, const NearbyStopsParams& params,
                    NearbyStopsResponse& out);

  // GET /v4/public/search_stops — used during first-run setup's stop search.
  bool searchStops(double lat, double lon, const std::string& query,
                    const SearchStopsParams& params, SearchStopsResponse& out);

 private:
  HttpTransport& transport_;
  std::string apiKey_;
};

}  // namespace transit
