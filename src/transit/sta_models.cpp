// Real implementation of staDeparturesToRoutes — see include/transit/sta_models.h.

#include "transit/sta_models.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace transit {
namespace sta {

namespace {

std::string toHex6(uint32_t rgb) {
  char buf[7];
  std::snprintf(buf, sizeof(buf), "%06X", rgb & 0xFFFFFFu);
  return std::string(buf);
}

}  // namespace

std::vector<Route> staDeparturesToRoutes(const std::vector<StaDeparture>& departures,
                                          const std::string& stopName) {
  // Group by routeId, preserving each route's first-seen order (the feed's
  // own entity order) — buildDepartureBoard() only reorders via
  // settings.routeOrder/sortByTime, it doesn't care about input order
  // otherwise, but a stable grouping still makes output deterministic for
  // a given input, which the tests below rely on.
  std::vector<std::string> routeOrder;
  std::unordered_map<std::string, Route> routesByid;

  for (const auto& dep : departures) {
    auto it = routesByid.find(dep.routeId);
    if (it == routesByid.end()) {
      Route route;
      route.globalRouteId = "sta:" + dep.routeId;
      route.routeShortName = "STA " + (!dep.routeShortName.empty() ? dep.routeShortName : dep.routeId);
      route.routeColor = toHex6(dep.routeColor);
      route.routeTextColor = toHex6(dep.routeTextColor);

      MergedItinerary mi;
      mi.directionId = 0;
      mi.closestStop.stopName = stopName;
      route.mergedItineraries.push_back(std::move(mi));

      routeOrder.push_back(dep.routeId);
      it = routesByid.emplace(dep.routeId, std::move(route)).first;
    }

    MergedItinerary& mi = it->second.mergedItineraries[0];

    Itinerary itin;
    itin.internalItineraryId = dep.tripId;
    itin.directionId = 0;
    itin.headsign = !dep.destination.empty() ? dep.destination : ("Route " + dep.routeId);
    itin.mergedHeadsign = itin.headsign;
    itin.isActive = true;
    mi.itineraries.push_back(std::move(itin));

    ScheduleItem item;
    item.internalItineraryId = dep.tripId;
    item.departureTimeEpoch = dep.departureEpoch;
    item.scheduledDepartureTimeEpoch = dep.departureEpoch;
    item.arrivalTimeEpoch = dep.departureEpoch;
    item.isRealTime = true;  // GTFS-RT is inherently a live prediction
    item.isCancelled = false;  // sta_feed_parser.h already drops SKIPPED updates
    item.isLast = false;
    mi.scheduleItems.push_back(std::move(item));
  }

  std::vector<Route> routes;
  routes.reserve(routeOrder.size());
  for (const auto& routeId : routeOrder) {
    routes.push_back(std::move(routesByid.at(routeId)));
  }
  return routes;
}

const StopInfo* parseStaStopCode(const std::string& stopCode) {
  if (stopCode.empty()) return nullptr;

  const char* start = stopCode.c_str();
  char* end = nullptr;
  const unsigned long parsed = strtoul(start, &end, 10);
  if (end == start || *end != '\0' || parsed > 0xFFFFu) return nullptr;

  return lookupStaStop(static_cast<uint16_t>(parsed));
}

}  // namespace sta
}  // namespace transit
