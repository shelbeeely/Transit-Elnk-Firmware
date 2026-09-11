// Real implementation of StaSdStore — see include/transit/sta_sd_store.h.

#include "transit/sta_sd_store.h"

#include <SDCardManager.h>

#include <algorithm>

namespace transit {
namespace sta {

bool StaSdStore::FileReader::open(const char* path) {
  return SDCardManager::getInstance().openFileForRead("StaSdStore", path, file_);
}

bool StaSdStore::FileReader::readAt(uint32_t offset, uint8_t* out, size_t len) {
  if (!file_) return false;
  if (!file_.seekSet(offset)) return false;
  const int n = file_.read(out, len);
  return n >= 0 && static_cast<size_t>(n) == len;
}

void StaSdStore::begin() {
  if (attempted_) return;
  attempted_ = true;

  sdMounted_ = SDCardManager::getInstance().begin();
  if (!sdMounted_) return;

  // Each file's absence/corruption is independent -- a card carrying only
  // some of the three tables still serves what it has (see this class's
  // header comment).
  routesReader_.open("/sta/routes.bin");
  stopsReader_.open("/sta/stops.bin");
  tripsReader_.open("/sta/trips.bin");
}

bool StaSdStore::lookupRoute(uint32_t routeId, SdRouteInfo& out) {
  if (!routesReader_.isOpen()) return false;
  return lookupSdRoute(routesReader_, routeId, out);
}

bool StaSdStore::lookupStop(uint32_t stopCode, SdStopInfo& out) {
  if (!stopsReader_.isOpen()) return false;
  return lookupSdStop(stopsReader_, stopCode, out);
}

bool StaSdStore::lookupTrip(uint32_t tripId, SdTripInfo& out) {
  if (!tripsReader_.isOpen()) return false;
  return lookupSdTrip(tripsReader_, tripId, out);
}

std::vector<SdStopInfo> StaSdStore::nearbyStops(double lat, double lon, double radiusMeters,
                                                int maxResults) {
  std::vector<SdStopInfo> results;
  if (!stopsReader_.isOpen() || maxResults <= 0) return results;

  TableHeader header;
  if (!readTableHeader(stopsReader_, header)) return results;
  if (header.recordSize != 24) return results;  // stops.bin's fixed record size

  // Paired with each result so the sort below doesn't recompute
  // haversineMeters() (several trig calls) per comparison -- computed once
  // here instead, where it's already needed for the radius filter anyway.
  std::vector<double> distances;

  for (uint32_t i = 0; i < header.recordCount; ++i) {
    const uint32_t recordOffset = kTableHeaderSize + i * header.recordSize;
    uint8_t buf[24];
    if (!stopsReader_.readAt(recordOffset, buf, sizeof(buf))) continue;

    SdStopInfo info;
    uint32_t nameOffset = 0;
    decodeStopRecordFixedFields(buf, info, nameOffset);

    const double distance = haversineMeters(lat, lon, info.lat, info.lon);
    if (distance > radiusMeters) continue;

    readBlobString(stopsReader_, header, nameOffset, info.stopName);
    results.push_back(std::move(info));
    distances.push_back(distance);
  }

  // Sort both vectors together (by index, since std::sort has no built-in
  // "sort A, permute B the same way") rather than recomputing distance
  // inside the comparator.
  std::vector<size_t> order(results.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
           [&](size_t a, size_t b) { return distances[a] < distances[b]; });

  std::vector<SdStopInfo> sorted;
  sorted.reserve(std::min(order.size(), static_cast<size_t>(maxResults)));
  for (size_t idx : order) {
    if (sorted.size() >= static_cast<size_t>(maxResults)) break;
    sorted.push_back(std::move(results[idx]));
  }
  return sorted;
}

}  // namespace sta
}  // namespace transit
