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
  // Every failure path here returns {} the same way -- to main.cpp, "STA had
  // nothing to report" and "STA is misconfigured" and "the network hiccuped"
  // are all the same non-fatal outcome (see sta_client.h). But that means
  // there's otherwise no way to tell those apart from outside a debugger, so
  // each one logs a one-line reason over Serial -- the same "if (Serial)"
  // guard freeink-sdk's own SecureClient.cpp uses, since a battery device
  // isn't guaranteed to have a monitor attached at the time.
  const StopInfo* stopInfo = parseStaStopCode(stopCode);
  if (stopInfo == nullptr) {
    if (Serial) Serial.printf("[StaClient] stop code \"%s\" not recognized, skipping STA fetch\n",
                              stopCode.c_str());
    return {};
  }

  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < kMinFreeHeapBytes) {
    if (Serial)
      Serial.printf("[StaClient] skipping STA fetch: %u bytes free heap, need >= %u\n",
                    static_cast<unsigned>(freeHeap), static_cast<unsigned>(kMinFreeHeapBytes));
    return {};
  }

  HttpResponse response = transport_.get(kStaFeedUrl, {{kStaApiKeyHeader, kStaApiKeyValue}});
  if (!response.transportOk || response.statusCode != 200) {
    if (Serial)
      Serial.printf("[StaClient] STA feed fetch failed: transportOk=%d statusCode=%d\n",
                    response.transportOk, response.statusCode);
    return {};
  }

  std::vector<StaDeparture> departures;
  const bool ok = parseTripUpdates(reinterpret_cast<const uint8_t*>(response.body.data()),
                                   response.body.size(), stopInfo->stopId, departures);
  if (!ok) {
    if (Serial)
      Serial.printf("[StaClient] STA feed did not parse (%u bytes received)\n",
                    static_cast<unsigned>(response.body.size()));
    return {};
  }

  if (Serial)
    Serial.printf("[StaClient] stop %s (\"%s\"): %u departures\n", stopInfo->stopId,
                  stopInfo->stopName, static_cast<unsigned>(departures.size()));
  return staDeparturesToRoutes(departures, stopInfo->stopName);
}

}  // namespace sta
}  // namespace transit
