#pragma once

// Transit-Elnk-Firmware — fetches STA's live GTFS-RT feed and adapts it into
// transit::Route entries ready to merge with the Transit API's own routes.
//
// Reuses the same HttpTransport interface (and WifiHttpTransport instance)
// api_client.h/icon_cache.h already use — STA's feed is a plain unauthenticated
// HTTPS GET with one header (see kStaFeedUrl/kStaApiKey in sta_client.cpp),
// no different in shape from a Transit API call. Unlike those two, though,
// the response here is a ~190KB protobuf blob covering STA's entire network
// (GTFS-RT has no server-side per-stop filtering), not a small per-request
// JSON payload — fetchDepartures() guards the attempt with a free-heap check
// (kMinFreeHeapBytes below) and skips the fetch rather than risking an
// allocation failure if there isn't comfortably enough room. See
// sta_feed_parser.h's file comment for why this buffers the full response
// rather than streaming it incrementally.
//
// Hardware-dependent (ESP.getFreeHeap()) — only buildable under
// [env:xteink_x4], not [env:native]; sta_feed_parser.h/sta_models.h (the
// actual parsing/adapting logic this calls) are hardware-independent and
// tested there instead.

#include <string>
#include <vector>

#include "transit/api_client.h"
#include "transit/models.h"

namespace transit {
namespace sta {

class StaClient {
 public:
  explicit StaClient(HttpTransport& transport);

  // stopCode is ConfigStore::staStopCode() — the numeric code printed on
  // the physical STA stop sign. Resolves it via sta_models.h's
  // parseStaStopCode() (sta_stop_table.h), fetches and parses the live
  // feed, and returns Route entries ready to concatenate with the Transit
  // API's own (see sta_models.h::staDeparturesToRoutes() for the shape/
  // labeling) — the resolved stop's display name is used internally to
  // populate those Routes' stop name, not returned separately, since
  // nothing else in this firmware currently has a use for it on its own
  // (render_engine.cpp never draws a per-departure-row stop name, only
  // BoardStatus's single Transit-stop header field — see
  // renderDepartureBoard()).
  //
  // Returns an empty vector when: stopCode is empty or unresolvable, the
  // HTTP fetch fails, the feed doesn't parse, or there isn't enough free
  // heap to safely attempt it — always treated as "STA had nothing to
  // report this cycle" rather than surfaced as an error; the board still
  // has Transit API departures either way.
  std::vector<Route> fetchDepartures(const std::string& stopCode);

 private:
  HttpTransport& transport_;
};

}  // namespace sta
}  // namespace transit
