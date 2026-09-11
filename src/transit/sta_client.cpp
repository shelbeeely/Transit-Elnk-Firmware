// Real implementation of StaClient::fetchDepartures — see
// include/transit/sta_client.h.

#include "transit/sta_client.h"

#include <Arduino.h>

#include "transit/sta_feed_parser.h"
#include "transit/sta_models.h"

namespace transit {
namespace sta {

namespace {

// STA's live GTFS-RT TripUpdates feed. Reachable unauthenticated with this
// shared key (the same one STA's own OneBusAway config publishes) — see
// sta_client.h's file comment on why this feed, not STA's JSON REST API, is
// what this firmware actually fetches.
constexpr const char* kStaFeedUrl = "https://gtfsbridge.spokanetransit.com/realtime/TripUpdate/TripUpdates.pb";
constexpr const char* kStaApiKeyHeader = "X-API-KEY";
constexpr const char* kStaApiKeyValue = "12345";

// The feed covers STA's entire network (~190KB observed) since GTFS-RT has
// no server-side per-stop filtering — this is buffered into RAM whole (see
// sta_feed_parser.h's file comment), so skip the attempt outright rather
// than risk an allocation failure if free heap is already tight from
// WiFi/TLS/display state. http_transport.cpp reads the response directly
// into one std::string (sized via Content-Length up front when the server
// sends one, avoiding incremental-growth reallocation) rather than via
// HTTPClient::getString(), which would briefly hold a second full copy on
// top of it — so the real peak here is roughly one ~190KB buffer plus
// WiFiClientSecure/TLS session overhead (historically tens of KB) plus the
// small std::vector<StaDeparture> results. This threshold is comfortably
// above that estimate, but empirical/best-effort: it hasn't been profiled
// against real hardware (see docs/STA_INTEGRATION.md's known limitations).
constexpr size_t kMinFreeHeapBytes = 260 * 1024;

}  // namespace

StaClient::StaClient(HttpTransport& transport) : transport_(transport) {}

std::vector<Route> StaClient::fetchDepartures(const std::string& stopCode) {
  const StopInfo* stopInfo = parseStaStopCode(stopCode);
  if (stopInfo == nullptr) return {};

  if (ESP.getFreeHeap() < kMinFreeHeapBytes) return {};

  HttpResponse response = transport_.get(kStaFeedUrl, {{kStaApiKeyHeader, kStaApiKeyValue}});
  if (!response.transportOk || response.statusCode != 200) return {};

  std::vector<StaDeparture> departures;
  const bool ok = parseTripUpdates(reinterpret_cast<const uint8_t*>(response.body.data()),
                                   response.body.size(), stopInfo->stopId, departures);
  if (!ok) return {};

  return staDeparturesToRoutes(departures, stopInfo->stopName);
}

}  // namespace sta
}  // namespace transit
