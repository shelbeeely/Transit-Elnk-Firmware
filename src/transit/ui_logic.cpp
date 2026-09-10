// Real implementation of buildDepartureBoard — see include/transit/ui_logic.h
// for the frozen contract and docs/UI_BEHAVIOR.md for the source rules.
//
// Pipeline (per route, in API order):
//   1. drop hidden routes (settings.hiddenRoutes)
//   2/3/4. per surviving direction: drop cancelled items, drop items outside
//      the departure window, sort nearest-first, cap to maxDeparturesPerDirection
//   7. settings.staticDirection, when >= 0, restricts which direction index of
//      each route is even considered (applied inline, before 2/3/4, since it
//      decides which merged_itineraries[] entries turn into boards at all)
// then, across the whole flattened list of per-direction boards:
//   5. settings.routeOrder reorders boards (stable; unlisted routes keep API
//      order, appended after the listed ones)
//   6. settings.sortByTime, when set, re-sorts the boards by each board's
//      earliest surviving departure time — see the note below on interpretation.
//
// Judgment call (see PR description): a direction whose departures are all
// filtered out (cancelled/out-of-window/hidden) contributes no DirectionBoard
// at all, matching both reference apps' behavior of a route/direction with
// nothing upcoming simply not appearing (UI_BEHAVIOR.md's "a route with
// nothing upcoming disappears from the board entirely, it does not render an
// empty card").

#include "transit/ui_logic.h"

#include <algorithm>
#include <unordered_map>

namespace transit {

namespace {

// Finds the Itinerary within `mi` whose internalItineraryId matches `item`'s,
// so a ScheduleItem can be mapped back to its mergedHeadsign per
// docs/DATA_MODEL.md. Returns nullptr if no match (defensive; shouldn't
// happen with well-formed API responses).
const Itinerary* findItinerary(const MergedItinerary& mi, const ScheduleItem& item) {
  for (const auto& itin : mi.itineraries) {
    if (itin.internalItineraryId == item.internalItineraryId) {
      return &itin;
    }
  }
  return nullptr;
}

bool isHiddenRoute(const Route& route, const std::vector<std::string>& hiddenRoutes) {
  return std::find(hiddenRoutes.begin(), hiddenRoutes.end(), route.globalRouteId) !=
         hiddenRoutes.end();
}

// Rules 2/3/4: drop cancelled items, drop items outside the departure window,
// sort nearest-first, cap to maxDeparturesPerDirection.
std::vector<ScheduleItem> filteredSortedCappedItems(const MergedItinerary& mi,
                                                      const UiSettings& settings,
                                                      int64_t nowEpoch) {
  const int64_t windowEnd = nowEpoch + static_cast<int64_t>(settings.departureWindowMin) * 60;

  std::vector<ScheduleItem> items;
  items.reserve(mi.scheduleItems.size());
  for (const auto& item : mi.scheduleItems) {
    if (item.isCancelled) continue;
    if (item.departureTimeEpoch < nowEpoch || item.departureTimeEpoch >= windowEnd) continue;
    items.push_back(item);
  }

  std::stable_sort(items.begin(), items.end(), [](const ScheduleItem& a, const ScheduleItem& b) {
    return a.departureTimeEpoch < b.departureTimeEpoch;
  });

  // maxDeparturesPerDirection <= 0 isn't a meaningful cap (API_CONTRACT.md's
  // valid range is 1-10) — treat it as "no cap" rather than silently
  // dropping every departure, in case an unvalidated/misconfigured value
  // ever reaches here.
  if (settings.maxDeparturesPerDirection > 0) {
    const size_t cap = static_cast<size_t>(settings.maxDeparturesPerDirection);
    if (items.size() > cap) items.resize(cap);
  }

  return items;
}

DirectionBoard buildBoard(const Route& route, const MergedItinerary& mi,
                           std::vector<ScheduleItem> items) {
  DirectionBoard board;
  board.globalRouteId = route.globalRouteId;
  board.routeShortName = route.routeShortName;
  board.routeDisplayShortName = route.routeDisplayShortName;
  board.routeColor = route.routeColor;
  board.routeTextColor = route.routeTextColor;
  board.directionId = mi.directionId;

  board.departures.reserve(items.size());
  for (const auto& item : items) {
    DepartureRow row;
    if (const Itinerary* itin = findItinerary(mi, item)) {
      // Prefer mergedHeadsign (DATA_MODEL.md), but fall back to the plain
      // headsign when it's empty (e.g. direction_headsign unset for this
      // agency) — matches the widget's own `merged_headsign || headsign`.
      row.headsign = !itin->mergedHeadsign.empty() ? itin->mergedHeadsign : itin->headsign;
    }
    row.stopName = mi.closestStop.stopName;
    row.departureTimeEpoch = item.departureTimeEpoch;
    row.isRealTime = item.isRealTime;
    row.isLast = item.isLast;
    board.departures.push_back(std::move(row));
  }
  return board;
}

// Rule 5: routes listed in routeOrder come first, in that order; routes not
// listed keep their original (API) relative order, appended after. Multiple
// boards sharing a globalRouteId (different directions) sort as a group,
// preserving their original relative order within that group.
void applyRouteOrder(std::vector<DirectionBoard>& boards,
                      const std::vector<std::string>& routeOrder) {
  if (routeOrder.empty()) return;

  std::unordered_map<std::string, size_t> orderIndex;
  orderIndex.reserve(routeOrder.size());
  for (size_t i = 0; i < routeOrder.size(); ++i) {
    orderIndex.emplace(routeOrder[i], i);
  }
  const size_t notListed = routeOrder.size();

  std::stable_sort(boards.begin(), boards.end(),
                    [&](const DirectionBoard& a, const DirectionBoard& b) {
                      auto ai = orderIndex.find(a.globalRouteId);
                      auto bi = orderIndex.find(b.globalRouteId);
                      size_t akey = ai != orderIndex.end() ? ai->second : notListed;
                      size_t bkey = bi != orderIndex.end() ? bi->second : notListed;
                      return akey < bkey;
                    });
}

// Rule 6: interleave by time. DirectionBoard groups departures per
// route/direction (a frozen part of the output contract), so "interleave ALL
// directions' departures by time" is implemented here as re-sorting the
// per-direction boards themselves by each board's earliest surviving
// departure (its first entry, since items are already nearest-first) —
// the closest thing to a global by-time order the DirectionBoard shape
// allows, applied on top of (overriding, for ties-breaking purposes it falls
// back to) rule 5's ordering. See PR description for the full rationale.
void applySortByTime(std::vector<DirectionBoard>& boards) {
  std::stable_sort(boards.begin(), boards.end(),
                    [](const DirectionBoard& a, const DirectionBoard& b) {
                      // Both are guaranteed non-empty: boards with no
                      // surviving departures are never emitted.
                      return a.departures.front().departureTimeEpoch <
                             b.departures.front().departureTimeEpoch;
                    });
}

}  // namespace

std::vector<DirectionBoard> buildDepartureBoard(const std::vector<Route>& routes,
                                                 const UiSettings& settings,
                                                 int64_t nowEpoch) {
  std::vector<DirectionBoard> boards;

  for (const auto& route : routes) {
    // Rule 1: drop hidden routes.
    if (isHiddenRoute(route, settings.hiddenRoutes)) continue;

    for (size_t i = 0; i < route.mergedItineraries.size(); ++i) {
      // Rule 7: staticDirection restricts each route to a single itinerary
      // index instead of every direction.
      if (settings.staticDirection >= 0 &&
          i != static_cast<size_t>(settings.staticDirection)) {
        continue;
      }

      const MergedItinerary& mi = route.mergedItineraries[i];
      std::vector<ScheduleItem> items = filteredSortedCappedItems(mi, settings, nowEpoch);
      if (items.empty()) continue;  // nothing upcoming: no board for this direction

      boards.push_back(buildBoard(route, mi, std::move(items)));
    }
  }

  applyRouteOrder(boards, settings.routeOrder);

  if (settings.sortByTime) {
    applySortByTime(boards);
  }

  return boards;
}

}  // namespace transit
