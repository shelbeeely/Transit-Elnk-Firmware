// Transit API v4 client — builds the GET request (URL + query string +
// apiKey header) for each endpoint per docs/API_CONTRACT.md, dispatches it
// through the injected HttpTransport, and parses the response body via
// models.h's parse* functions (implemented separately in models.cpp).
//
// No Arduino/FreeInk dependencies here — this file compiles under
// [env:native] as well as [env:xteink_x4] (see platformio.ini's
// build_src_filter, which does not exclude this file).

#include "transit/api_client.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace transit {

namespace {

constexpr const char* kBaseHost = "https://external.transitapp.com";

// RFC 3986 percent-encoding for query-string values. Unreserved characters
// (ALPHA / DIGIT / "-" / "." / "_" / "~") pass through untouched, which
// means it's a safe no-op to run on values we already know are plain
// numbers/bools/enum tokens -- so every value below goes through this,
// keeping the endpoint builders simple.
std::string percentEncode(const std::string& value) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0xF]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

std::string formatDouble(double value) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6f", value);
  return std::string(buf);
}

std::string boolToStr(bool value) { return value ? "true" : "false"; }

// Small helper that appends "&key=percent-encoded-value" (or "?..." for the
// first param) to a URL under construction.
class QueryBuilder {
 public:
  explicit QueryBuilder(std::string url) : url_(std::move(url)) {}

  QueryBuilder& add(const std::string& key, const std::string& rawValue) {
    url_.push_back(first_ ? '?' : '&');
    first_ = false;
    url_ += key;
    url_.push_back('=');
    url_ += percentEncode(rawValue);
    return *this;
  }

  QueryBuilder& add(const std::string& key, int value) { return add(key, std::to_string(value)); }
  QueryBuilder& add(const std::string& key, double value) { return add(key, formatDouble(value)); }
  QueryBuilder& add(const std::string& key, bool value) { return add(key, boolToStr(value)); }

  // Appends a value that's already percent-encoded (or otherwise known-safe,
  // e.g. a comma-joined list of already-encoded IDs) without encoding it a
  // second time.
  QueryBuilder& addRaw(const std::string& key, const std::string& preEncodedValue) {
    url_.push_back(first_ ? '?' : '&');
    first_ = false;
    url_ += key;
    url_.push_back('=');
    url_ += preEncodedValue;
    return *this;
  }

  QueryBuilder& addIfNonEmpty(const std::string& key, const std::string& rawValue) {
    if (!rawValue.empty()) add(key, rawValue);
    return *this;
  }

  // DepartureQueryParams::timeEpoch's documented sentinel is exactly 0
  // ("0 = now" -- see api_client.h); this only omits that sentinel value,
  // it does not treat negative timestamps as unset.
  QueryBuilder& addIfNonZero(const std::string& key, int64_t value) {
    if (value != 0) add(key, std::to_string(value));
    return *this;
  }

  const std::string& str() const { return url_; }

 private:
  std::string url_;
  bool first_ = true;
};

// global_stop_ids is a single comma-joined query value (docs/API_CONTRACT.md:
// "1:82774,2:54321"). Each ID is percent-encoded individually (defensive --
// IDs are documented as usually safe, but may contain ':') and joined with a
// literal comma, which is a valid unencoded query character and is what the
// server expects as the list delimiter. Contract caps this at 100 IDs per
// call; extras are dropped rather than sent and rejected server-side.
std::string joinStopIds(const std::vector<std::string>& ids) {
  std::string joined;
  size_t count = std::min<size_t>(ids.size(), 100);
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) joined.push_back(',');
    joined += percentEncode(ids[i]);
  }
  return joined;
}

// Params shared by nearby_routes/stop_departures (DepartureQueryParams).
void addDepartureQueryParams(QueryBuilder& qb, const DepartureQueryParams& params) {
  qb.add("max_num_departures", std::clamp(params.maxNumDepartures, 1, 10));
  qb.add("should_update_realtime", params.shouldUpdateRealtime);
  qb.add("merge_platform_stops", params.mergePlatformStops);
  qb.addIfNonZero("time", params.timeEpoch);
  qb.add("include_stops_and_shapes", params.includeStopsAndShapes);
  qb.add("stop_detailed", params.stopDetailed);
  qb.addIfNonEmpty("locale", params.locale);
}

}  // namespace

TransitApiClient::TransitApiClient(HttpTransport& transport, std::string apiKey)
    : transport_(transport), apiKey_(std::move(apiKey)) {}

void TransitApiClient::setApiKey(std::string apiKey) { apiKey_ = std::move(apiKey); }

bool TransitApiClient::nearbyRoutes(double lat, double lon, const NearbyRoutesParams& params,
                                     NearbyRoutesResponse& out) {
  QueryBuilder qb(std::string(kBaseHost) + "/v4/public/nearby_routes");
  qb.add("lat", lat);
  qb.add("lon", lon);
  qb.add("max_distance", std::clamp(params.maxDistanceMeters, 1, 1500));
  addDepartureQueryParams(qb, params);

  HttpResponse response = transport_.get(qb.str(), {{"apiKey", apiKey_}});
  if (!response.transportOk || response.statusCode != 200) {
    out = NearbyRoutesResponse{};
    return false;
  }
  return parseNearbyRoutes(response.body, out);
}

bool TransitApiClient::stopDepartures(const std::vector<std::string>& globalStopIds,
                                       const StopDeparturesParams& params,
                                       StopDeparturesResponse& out) {
  QueryBuilder qb(std::string(kBaseHost) + "/v4/public/stop_departures");
  qb.addRaw("global_stop_ids", joinStopIds(globalStopIds));
  qb.add("remove_cancelled", params.removeCancelled);
  qb.add("exclude_terminal_arrivals", params.excludeTerminalArrivals);
  addDepartureQueryParams(qb, params);

  HttpResponse response = transport_.get(qb.str(), {{"apiKey", apiKey_}});
  if (!response.transportOk || response.statusCode != 200) {
    out = StopDeparturesResponse{};
    return false;
  }
  return parseStopDepartures(response.body, out);
}

bool TransitApiClient::nearbyStops(double lat, double lon, const NearbyStopsParams& params,
                                    NearbyStopsResponse& out) {
  QueryBuilder qb(std::string(kBaseHost) + "/v4/public/nearby_stops");
  qb.add("lat", lat);
  qb.add("lon", lon);
  qb.add("max_distance", std::clamp(params.maxDistanceMeters, 1, 1500));
  qb.addIfNonEmpty("stop_filter", params.stopFilter);
  qb.addIfNonEmpty("pickup_dropoff_filter", params.pickupDropoffFilter);
  qb.add("stop_detailed", params.stopDetailed);
  qb.add("include_beta_feeds", params.includeBetaFeeds);
  qb.addIfNonEmpty("locale", params.locale);

  HttpResponse response = transport_.get(qb.str(), {{"apiKey", apiKey_}});
  if (!response.transportOk || response.statusCode != 200) {
    out = NearbyStopsResponse{};
    return false;
  }
  return parseNearbyStops(response.body, out);
}

bool TransitApiClient::searchStops(double lat, double lon, const std::string& query,
                                    const SearchStopsParams& params, SearchStopsResponse& out) {
  QueryBuilder qb(std::string(kBaseHost) + "/v4/public/search_stops");
  qb.add("lat", lat);
  qb.add("lon", lon);
  qb.add("query", query);
  qb.addIfNonEmpty("pickup_dropoff_filter", params.pickupDropoffFilter);
  qb.add("max_num_results", std::clamp(params.maxNumResults, 1, 50));
  if (params.maxDistanceMeters >= 0) {
    qb.add("max_distance", params.maxDistanceMeters);
  }
  qb.add("merge_similar_stops", params.mergeSimilarStops);
  qb.addIfNonEmpty("locale", params.locale);

  HttpResponse response = transport_.get(qb.str(), {{"apiKey", apiKey_}});
  if (!response.transportOk || response.statusCode != 200) {
    out = SearchStopsResponse{};
    return false;
  }
  return parseSearchStops(response.body, out);
}

}  // namespace transit
