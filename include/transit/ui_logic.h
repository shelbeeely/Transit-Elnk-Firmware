#pragma once

// Transit-Elnk-Firmware — filter/sort/dedupe/badge rules.
//
// Rules match docs/UI_BEHAVIOR.md exactly (departure-window cutoff,
// cancelled-departure removal, real-time/"last" badges, per-direction
// count cap, sort-by-time / static-direction settings). Pure functions
// operating on transit::models types plus a "now" timestamp passed in —
// no display/network/NVS dependency, fully unit-testable under [env:native].
//
// Frozen contract for the parallel work units: do not change existing
// function signatures. Adding a function is fine; note it in your PR
// description.

#include <cstdint>
#include <string>
#include <vector>

#include "transit/models.h"

namespace transit {

// One row ready for the render engine: everything ui_logic has already
// decided (which departures, in what order, which badges), nothing the
// render engine has to re-derive.
struct DepartureRow {
  std::string headsign;    // itineraries[].merged_headsign, preferred per DATA_MODEL.md
  std::string stopName;    // closest_stop.stop_name
  int64_t departureTimeEpoch = 0;
  bool isRealTime = false;
  bool isLast = false;
};

struct DirectionBoard {
  std::string globalRouteId;
  std::string routeShortName;
  DisplayShortName routeDisplayShortName;
  std::string routeColor;
  std::string routeTextColor;
  int directionId = 0;
  std::vector<DepartureRow> departures;  // already capped/sorted/filtered
};

struct UiSettings {
  int departureWindowMin = 110;         // docs/UI_BEHAVIOR.md: recommend ~90-130 range
  int maxDeparturesPerDirection = 3;
  bool sortByTime = false;
  int staticDirection = -1;             // -1 = off
  std::vector<std::string> hiddenRoutes;   // global_route_id
  std::vector<std::string> routeOrder;     // global_route_id, manual order
};

// Transforms raw API routes into board-ready rows:
//  1. drop routes in settings.hiddenRoutes
//  2. drop ScheduleItem entries with isCancelled = true
//  3. drop departures outside [nowEpoch, nowEpoch + departureWindowMin*60)
//  4. cap each direction to maxDeparturesPerDirection, nearest-first
//  5. apply settings.routeOrder (routes not listed keep API order, appended
//     after the ordered ones)
//  6. if settings.sortByTime, interleave all directions' departures by time
//     instead of grouping by route/direction
//  7. if settings.staticDirection >= 0, only emit that itinerary index per
//     route instead of every direction
std::vector<DirectionBoard> buildDepartureBoard(const std::vector<Route>& routes,
                                                 const UiSettings& settings,
                                                 int64_t nowEpoch);

}  // namespace transit
