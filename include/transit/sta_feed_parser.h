#pragma once

// Transit-Elnk-Firmware — minimal GTFS-RT TripUpdates.pb decoder for STA's
// live feed (sta_client.h fetches the bytes; this just decodes them).
//
// STA's site (spokanetransit.com, and the OneBusAway JSON layer it also
// runs) sits behind Cloudflare bot protection that blocks a plain HTTP
// client — including this device's firmware — so the OBA REST API that
// would normally make this a simple JSON fetch (like the Transit API
// client) isn't usable here. The one thing that IS reachable unauthenticated
// is STA's raw GTFS-RT protobuf feed
// (https://gtfsbridge.spokanetransit.com/realtime/TripUpdate/TripUpdates.pb,
// header "X-API-KEY: 12345" — the same shared key STA's own OBA config
// uses), confirmed live and decodable against real captured data. Hence a
// hand-rolled decoder instead of a JSON parse: pulling in a full protobuf
// library (nanopb, protobuf-lite) for the handful of fields this board
// actually needs would be a much bigger dependency than writing a small,
// purpose-built wire-format walker for exactly those fields.
//
// Only the fields a departure board needs are decoded (see
// FeedMessage/FeedEntity/TripUpdate/... in the upstream
// google/transit/realtime/gtfs-realtime.proto for the full schema this is a
// subset of) — everything else (vehicle positions, alerts, occupancy,
// shapes, delay/uncertainty) is walked past via skipField() without being
// materialized. Two fields (StopTimeProperties.stop_headsign,
// TripProperties.trip_short_name) are declared as plain `string` in that
// upstream .proto but STA's server actually serializes them as an embedded
// TranslatedString{repeated Translation{text, language}} submessage instead
// (both are wire-type 2 / length-delimited, so this doesn't fail decoding
// either way, it just means treating their raw bytes as `string` gives back
// undecoded submessage bytes rather than readable text) — confirmed against
// a real capture, decodeTranslatedString() below unwraps this correctly.
// direction_id was checked too and dropped: STA's live feed carries values
// outside GTFS's standard 0/1 (5, 6, 7, 8, 9 were all observed on one
// capture), so it isn't a reliable signal for grouping departures by
// direction here — see sta_models.h's staDeparturesToRoutes() for how that's
// handled instead (one merged direction per route).
//
// Non-streaming by design: parseTripUpdates() takes one complete in-memory
// buffer rather than incremental chunks. A true streaming decoder (bounded
// memory regardless of feed size) would be the more defensible design for a
// ~190KB feed on an ESP32-C3, but is real additional complexity to get
// right and verify without physical hardware to test against. sta_client.h
// instead guards the fetch with a free-heap check and skips the STA fetch
// for that wake cycle rather than risking an allocation failure — see its
// file comment.
//
// Hardware-independent — builds and is unit-testable under [env:native] as
// well as [env:xteink_x4].

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "transit/sta_models.h"

namespace transit {
namespace sta {

// Decodes one FeedMessage and appends every StaDeparture found for
// `targetStopId` (a GTFS-RT stop_id — sta_stop_table.h's StopInfo::stopId,
// not the numeric code printed on the sign) to `out`. `out` is only
// appended to, never cleared, so callers can accumulate across multiple
// calls if ever needed.
//
// A StopTimeUpdate is included only when: its stop_id matches
// targetStopId, its schedule_relationship isn't SKIPPED (a real stop this
// trip is bypassing on this run — it carries no usable time), and it has a
// departure time or, failing that, an arrival time (a trip's very last stop
// has no departure, only an arrival).
//
// Returns false on malformed input (a truncated or invalid protobuf byte
// stream) — `out` may still hold whatever was successfully decoded before
// the point of failure, matching this codebase's other parse*() functions'
// "never throws, leaves partial/unspecified state on failure" convention
// (see transit::parseStopDepartures et al. in models.h). Bounds-checks
// every read; never reads past `data + len` regardless of what the bytes
// claim, since this is untrusted network input.
bool parseTripUpdates(const uint8_t* data, size_t len, const std::string& targetStopId,
                      std::vector<StaDeparture>& out);

}  // namespace sta
}  // namespace transit
