// Real implementation of planPresetTrip — see include/transit/trip_planner.h
// for the frozen contract and docs/TRIP_PLANNER.md for the approximations
// this makes and their known limitations.

#include "transit/trip_planner.h"

#include <algorithm>
#include <limits>

namespace transit {

namespace {

// A single candidate departure found while scanning for the next
// departure of one route at one stop.
struct Candidate {
  bool found = false;
  int64_t departureTimeEpoch = 0;
  std::string routeShortName;
  std::string rtTripId;
};

// Finds routeId's earliest non-cancelled departure at stopId with
// departureTimeEpoch >= afterEpoch. When directionId >= 0, only that
// direction's itineraries are considered; otherwise every direction is,
// picking whichever gives the soonest departure. When preferredRtTripId is
// non-empty, a scheduleItem whose rtTripId matches it is preferred over the
// nearest-time candidate (best-effort same-trip cross-check — see the
// header's note on rtTripId reliability); if none matches, falls back to
// nearest-time as usual.
Candidate findNextDeparture(const std::vector<Route>& routes, const std::string& routeId,
                            const std::string& stopId, int64_t afterEpoch, int directionId,
                            const std::string& preferredRtTripId = "") {
  Candidate nearest;
  Candidate rtTripMatch;

  for (const auto& route : routes) {
    if (route.globalRouteId != routeId || route.globalStopId != stopId) continue;

    for (const auto& mi : route.mergedItineraries) {
      if (directionId >= 0 && mi.directionId != directionId) continue;

      for (const auto& item : mi.scheduleItems) {
        if (item.isCancelled) continue;
        if (item.departureTimeEpoch < afterEpoch) continue;

        if (!nearest.found || item.departureTimeEpoch < nearest.departureTimeEpoch) {
          nearest.found = true;
          nearest.departureTimeEpoch = item.departureTimeEpoch;
          nearest.routeShortName = route.routeShortName;
          nearest.rtTripId = item.rtTripId;
        }

        if (!preferredRtTripId.empty() && !item.rtTripId.empty() &&
            item.rtTripId == preferredRtTripId &&
            (!rtTripMatch.found || item.departureTimeEpoch < rtTripMatch.departureTimeEpoch)) {
          rtTripMatch.found = true;
          rtTripMatch.departureTimeEpoch = item.departureTimeEpoch;
          rtTripMatch.routeShortName = route.routeShortName;
          rtTripMatch.rtTripId = item.rtTripId;
        }
      }
    }
  }

  return rtTripMatch.found ? rtTripMatch : nearest;
}

}  // namespace

PresetTripPlan planPresetTrip(const std::vector<Route>& routes, const PresetConfig& preset,
                              int64_t nowEpoch) {
  PresetTripPlan plan;
  plan.presetName = preset.presetName;

  if (preset.legs.empty()) {
    plan.fallbackMessage = "Not configured";
    return plan;
  }

  int64_t afterEpoch = nowEpoch;

  for (size_t i = 0; i < preset.legs.size(); ++i) {
    const TripLegConfig& legConfig = preset.legs[i];

    // No preferredRtTripId here: a leg's boarding candidate is a different
    // route from the previous leg's alight candidate (that's the whole
    // point of a transfer), so there's no legitimate trip-continuity hint
    // to carry across -- just the soonest departure of this leg's own
    // route after afterEpoch.
    Candidate boardCandidate =
        findNextDeparture(routes, legConfig.routeId, legConfig.boardStopId, afterEpoch, legConfig.directionId);
    if (!boardCandidate.found) {
      plan.fallbackMessage = plan.legs.empty() ? "No upcoming trip found"
                                                : "Transfer to " + legConfig.routeId + " not found";
      plan.found = false;
      return plan;
    }

    // Estimate this leg's alight time: the same route/direction's next
    // scheduled departure/passage at alightStopId at-or-after boarding.
    Candidate alightCandidate =
        findNextDeparture(routes, legConfig.routeId, legConfig.alightStopId,
                          boardCandidate.departureTimeEpoch, legConfig.directionId,
                          boardCandidate.rtTripId);

    PlannedLeg leg;
    leg.routeId = legConfig.routeId;
    leg.routeShortName = boardCandidate.routeShortName;
    leg.boardStopId = legConfig.boardStopId;
    leg.alightStopId = legConfig.alightStopId;
    leg.boardEpoch = boardCandidate.departureTimeEpoch;
    // If no scheduled passage at the alight stop is found (e.g. it's the
    // route's terminus, or table gaps), fall back to the boarding time
    // itself rather than leaving alightEpoch at 0 -- keeps downstream
    // transfer-buffer math sane instead of producing a bogus huge gap.
    leg.alightEpoch =
        alightCandidate.found ? alightCandidate.departureTimeEpoch : boardCandidate.departureTimeEpoch;

    plan.legs.push_back(leg);

    afterEpoch = leg.alightEpoch + static_cast<int64_t>(preset.transferBufferMin) * 60;
  }

  plan.found = true;
  plan.leaveByEpoch = plan.legs.front().boardEpoch - static_cast<int64_t>(preset.walkToFirstStopMin) * 60;
  return plan;
}

}  // namespace transit
