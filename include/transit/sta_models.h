#pragma once

// Transit-Elnk-Firmware — Spokane Transit Authority (STA) data shapes.
//
// STA is a second, optional data source alongside the Transit API
// (docs/CONFIG_AND_STATE.md's sta_stop key) — fetched from STA's own public
// GTFS-RT feed (see sta_feed_parser.h/sta_client.h), not through Transit's
// API. Kept as its own small model rather than shoehorned into
// transit::Route: a GTFS-RT TripUpdate carries far fewer fields than a
// Transit v4 Route (no icon, no schedule/merged-itinerary structure, no
// stable per-direction grouping — see sta_feed_parser.h's note on
// direction_id), so forcing it into Route's shape at the parse layer would
// mean padding out fields that don't apply. staDeparturesToRoutes() below is
// the seam that adapts this into transit::Route once, at the point
// main.cpp actually needs to hand both sources to the same
// ui_logic::buildDepartureBoard() pipeline.
//
// Hardware-independent — builds and is unit-testable under [env:native] as
// well as [env:xteink_x4].

#include <cstdint>
#include <string>
#include <vector>

#include "transit/models.h"
#include "transit/sta_stop_table.h"

namespace transit {
namespace sta {

// One upcoming departure at the configured STA stop, already resolved
// against the baked-in route table (sta_route_table.h) — everything
// staDeparturesToRoutes() needs, nothing it has to re-derive.
struct StaDeparture {
  std::string routeId;        // GTFS-RT TripDescriptor.route_id, e.g. "34"
  std::string routeShortName; // sta_route_table.h lookup, falls back to routeId
  uint32_t routeColor = 0x4E6470;      // 0xRRGGBB; sta_route_table.h, or a
  uint32_t routeTextColor = 0xFFFFFF;  // neutral STA-gray fallback if the
                                       // feed references a route this table
                                       // doesn't know about yet (see that
                                       // header's regeneration note)
  std::string tripId;         // TripDescriptor.trip_id; also this departure's
                               // synthetic Itinerary::internalItineraryId
  std::string destination;    // TripProperties.trip_short_name (a live,
                               // per-trip destination the GTFS-RT feed itself
                               // carries — no static-GTFS lookup needed for
                               // this one field); "Route <routeId>" if absent
  int64_t departureEpoch = 0; // StopTimeUpdate's departure.time, or
                               // arrival.time when departure is absent
                               // (a trip's last stop has no departure)
  // 0 or 1, the standard GTFS direction — NOT from the live feed (see
  // sta_feed_parser.h's note on why its own direction_id isn't used), but
  // from sd_card_data/sta/trips.bin when a StaSdStore lookup for this
  // trip succeeds (sta_client.cpp fills this in as an enrichment pass
  // after parsing, when SD is available). Stays 0 -- the same "one merged
  // direction per route" behavior as before this field existed -- whenever
  // SD isn't available or doesn't have this trip.
  uint8_t directionId = 0;
};

// Adapts already-parsed, already-stop-filtered STA departures into
// transit::Route entries so main.cpp can concatenate them with the Transit
// API's own routes and run both through the existing, unmodified
// ui_logic::buildDepartureBoard() — reusing its departure-window/sort/
// hidden-route/max-per-direction handling instead of duplicating it here.
//
// One Route per distinct routeId, with one MergedItinerary per distinct
// directionId actually seen for that route (so a route with SD-sourced
// direction data splits into two boards, same as a Transit API route with
// two directions would; one still in play for departures whose directionId
// stayed at its 0 default -- no SD trip match, or SD unavailable) and one
// Itinerary+ScheduleItem pair per StaDeparture. routeShortName is prefixed
// "STA " (docs/CONFIG_AND_STATE.md: departures are shown separately from
// Transit's, not deduplicated against it) — drawRouteBadge() in
// render_engine.cpp always falls back to a plain colored text badge for
// these rows (routeDisplayShortName's image slugs are left empty; STA has
// no icon source), so that prefix is what actually distinguishes an STA row
// from a Transit one on the panel, without any render_engine changes.
//
// stopName is applied to every route's MergedItinerary::closestStop —
// callers pass the single configured STA stop's resolved name (sta_stop
// resolves to exactly one stop, unlike Transit's stop_departures which can
// query several).
std::vector<Route> staDeparturesToRoutes(const std::vector<StaDeparture>& departures,
                                          const std::string& stopName);

// Parses and resolves a stop code (what ConfigStore::staStopCode() stores,
// and what a user types during setup) against sta_stop_table.h in one
// place, shared by SetupFlow::handleSetStaStop() (validating a code as it's
// entered) and StaClient::fetchDepartures() (resolving one to actually
// fetch against) — kept in sync by construction rather than by each call
// site re-implementing the same strtoul-and-range-check.
//
// Returns nullptr for an empty string, a non-numeric string, a value that
// can't fit a stop code (sta_stop_table.h's StopInfo::stopCode is 16-bit),
// or a value the table doesn't recognize.
const StopInfo* parseStaStopCode(const std::string& stopCode);

}  // namespace sta
}  // namespace transit
