#pragma once

// Transit-Elnk-Firmware — preset "Home"/"Work" trip planning across a fixed,
// user-configured chain of legs (e.g. "31 -> 32 -> 97"), computed from data
// the existing stopDepartures() endpoint already returns.
//
// Deliberately NOT built on Transit API v4's /v4/public/plan endpoint: its
// exact schema and tier/rate-limit implications are undocumented in this
// project's docs/API_CONTRACT.md, so building against it would be guessing.
// Instead the user pre-configures each preset's fixed route/stop chain
// (they already know it from experience -- that's exactly what "31 -> 32 ->
// 97" is), and this module answers "given that chain and the departures
// already fetched for its stops, when do I need to leave, and when do I get
// off and transfer" -- a much smaller, verifiable problem than general
// origin-to-destination trip planning.
//
// Pure and host-testable, mirroring ui_logic.h's contract: takes already-
// fetched transit::Route data plus nowEpoch, no NVS/network/display
// dependency. See docs/TRIP_PLANNER.md for the known limitations this
// module's approximations carry (arrival-time estimation, direction
// disambiguation, rtTripId reliability).
//
// Frozen contract once this ships: do not change existing struct fields or
// function signatures without updating every caller (config_store.h,
// setup_flow.cpp, main.cpp). Adding a field/function is fine.

#include <cstdint>
#include <string>
#include <vector>

#include "transit/models.h"

namespace transit {

// One leg of a preset's fixed route chain, as configured by the user via
// the settings portal (see setup_flow.cpp / config_store.h's
// presetLegs()/setPresetLegs()).
struct TripLegConfig {
  std::string routeId;      // globalRouteId
  std::string boardStopId;  // globalStopId to board this leg's route at
  std::string alightStopId; // globalStopId to get off / transfer at
  // -1 = unset (any direction considered, picking whichever gives the
  // soonest departure); 0/1 = a direction confirmed live at setup time via
  // the settings portal's /legdirections endpoint, to disambiguate which
  // of a route's directionId's at boardStopId actually continues toward
  // alightStopId (stop_departures alone can't tell you that -- see
  // docs/TRIP_PLANNER.md).
  int directionId = -1;
};

// A preset's full configuration, assembled from ConfigStore before calling
// planPresetTrip() (see config_store.h's PresetId/presetLegs() etc.).
struct PresetConfig {
  std::string presetName;         // "Home" / "Work" -- display only
  std::vector<TripLegConfig> legs;  // ordered, capped at 3 by the portal
  int walkToFirstStopMin = 0;     // subtracted from leg 0's departure for leaveByEpoch
  int transferBufferMin = 3;      // minimum minutes between a leg's estimated
                                    // alight time and the next leg's departure
};

// One leg of a *computed* plan -- a TripLegConfig resolved against actual
// upcoming departure times.
struct PlannedLeg {
  std::string routeId;
  std::string routeShortName;  // looked up from the matching Route, for display
  std::string boardStopId;
  std::string alightStopId;
  int64_t boardEpoch = 0;
  // Approximated: the same route's next scheduled departure/passage at
  // alightStopId at-or-after boardEpoch, since stop_departures reports a
  // departure/passage time, not a distinct arrival time. See
  // docs/TRIP_PLANNER.md's "Known limitations".
  int64_t alightEpoch = 0;
};

struct PresetTripPlan {
  std::string presetName;
  bool found = false;
  // legs[0].boardEpoch - walkToFirstStopMin*60. Only meaningful when
  // !legs.empty().
  int64_t leaveByEpoch = 0;
  std::vector<PlannedLeg> legs;  // legs successfully matched, in order --
                                  // may be a non-empty prefix even when
                                  // found is false (a later leg failed).
  std::string fallbackMessage;   // set when found is false, e.g.
                                  // "no upcoming trip found" or
                                  // "transfer to 32 not found"
};

// routes: the FULL stop_departures response across ALL queried stop_ids
// (the main board's stop plus every preset leg's boardStopId/alightStopId),
// UNFILTERED by ui_logic's hidden-route/route-order/departure-window
// display settings -- those are main-board display preferences and must
// not silently break a preset the user explicitly configured.
//
// preset.legs.empty() returns {found=false, fallbackMessage="Not
// configured"} immediately.
PresetTripPlan planPresetTrip(const std::vector<Route>& routes, const PresetConfig& preset,
                              int64_t nowEpoch);

}  // namespace transit
