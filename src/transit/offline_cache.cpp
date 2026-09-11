// Last-known-good departure board persistence — see
// include/transit/offline_cache.h for the contract and the reasoning behind
// caching the computed board rather than the raw API response.

#include "transit/offline_cache.h"

#include <cstdlib>

namespace transit {

namespace {

// Bumped whenever the line/field layout below changes, so a firmware update
// reads an older blob as "no cache" instead of misparsing it into a board
// full of garbage.
constexpr char kVersionTag[] = "TCB1";

constexpr char kFieldSep = '\t';
constexpr char kLineSep = '\n';

// Field values are free text straight out of a GTFS feed (headsigns, stop
// names), so the two delimiters have to be neutralized on write. Replacing
// rather than escaping keeps the parser trivial; the cost is a tab inside a
// headsign coming back as a space, which nobody will notice.
std::string sanitize(const std::string& value) {
  std::string out = value;
  for (char& c : out) {
    if (c == kFieldSep || c == kLineSep || c == '\r') c = ' ';
  }
  return out;
}

void appendField(std::string& out, const std::string& value) {
  out += kFieldSep;
  out += sanitize(value);
}

void appendField(std::string& out, int64_t value) {
  out += kFieldSep;
  out += std::to_string(value);
}

// Splits one line on kFieldSep. Empty fields are preserved (an absent
// headsign or image slug is meaningful), which is why this isn't a
// skip-empties tokenizer.
std::vector<std::string> splitFields(const std::string& line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    const size_t sep = line.find(kFieldSep, start);
    if (sep == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, sep - start));
    start = sep + 1;
  }
  return fields;
}

int64_t toInt64(const std::string& value) { return std::strtoll(value.c_str(), nullptr, 10); }

bool toBool(const std::string& value) { return value == "1"; }

std::string serializeRouteHeader(const DirectionBoard& dir) {
  std::string line = "R";
  appendField(line, dir.globalRouteId);
  appendField(line, dir.routeShortName);
  appendField(line, dir.routeDisplayShortName.elements[0]);
  appendField(line, dir.routeDisplayShortName.elements[1]);
  appendField(line, dir.routeDisplayShortName.elements[2]);
  appendField(line, dir.routeColor);
  appendField(line, dir.routeTextColor);
  appendField(line, static_cast<int64_t>(dir.directionId));
  line += kLineSep;
  return line;
}

std::string serializeDeparture(const DepartureRow& dep) {
  std::string line = "D";
  appendField(line, dep.headsign);
  appendField(line, dep.stopName);
  appendField(line, dep.departureTimeEpoch);
  appendField(line, static_cast<int64_t>(dep.isRealTime ? 1 : 0));
  appendField(line, static_cast<int64_t>(dep.isLast ? 1 : 0));
  line += kLineSep;
  return line;
}

std::string serializePresetHeader(const PresetTripPlan& plan) {
  std::string line = "P";
  appendField(line, plan.presetName);
  appendField(line, static_cast<int64_t>(plan.found ? 1 : 0));
  appendField(line, plan.leaveByEpoch);
  appendField(line, plan.fallbackMessage);
  line += kLineSep;
  return line;
}

std::string serializePresetLeg(const PlannedLeg& leg) {
  std::string line = "L";
  appendField(line, leg.routeId);
  appendField(line, leg.routeShortName);
  appendField(line, leg.boardStopId);
  appendField(line, leg.alightStopId);
  appendField(line, leg.boardEpoch);
  appendField(line, leg.alightEpoch);
  line += kLineSep;
  return line;
}

}  // namespace

std::string serializeCachedBoard(const CachedBoard& cache, size_t maxBytes) {
  if (cache.fetchedAtEpoch <= 0) return std::string();
  if (cache.board.empty() && cache.presetPlans.empty()) return std::string();

  std::string out = kVersionTag;
  appendField(out, cache.fetchedAtEpoch);
  out += kLineSep;
  if (out.size() > maxBytes) return std::string();

  // Presets first, deliberately: they're a handful of bytes each and they
  // answer "when do I leave," which is the thing worth keeping when the
  // budget gets tight. Board rows below are the part that gets dropped.
  for (const PresetTripPlan& plan : cache.presetPlans) {
    std::string chunk = serializePresetHeader(plan);
    for (const PlannedLeg& leg : plan.legs) {
      chunk += serializePresetLeg(leg);
    }
    // A preset and its legs are written as one unit or not at all — a
    // header whose legs got truncated would parse as a plan with a
    // silently shortened chain, which is worse than omitting it.
    if (out.size() + chunk.size() > maxBytes) break;
    out += chunk;
  }

  for (const DirectionBoard& dir : cache.board) {
    const std::string header = serializeRouteHeader(dir);
    if (out.size() + header.size() > maxBytes) break;
    out += header;
    for (const DepartureRow& dep : dir.departures) {
      const std::string line = serializeDeparture(dep);
      if (out.size() + line.size() > maxBytes) break;
      out += line;
    }
  }

  return out;
}

bool deserializeCachedBoard(const std::string& blob, CachedBoard& out) {
  if (blob.empty()) return false;

  CachedBoard parsed;
  bool sawHeader = false;

  size_t lineStart = 0;
  while (lineStart <= blob.size()) {
    size_t lineEnd = blob.find(kLineSep, lineStart);
    if (lineEnd == std::string::npos) lineEnd = blob.size();
    const std::string line = blob.substr(lineStart, lineEnd - lineStart);
    lineStart = lineEnd + 1;
    if (line.empty()) {
      if (lineEnd >= blob.size()) break;
      continue;
    }

    const std::vector<std::string> f = splitFields(line);
    if (f.empty()) continue;

    if (!sawHeader) {
      // The very first line must be the version tag — anything else means
      // this isn't a blob this build knows how to read.
      if (f[0] != kVersionTag || f.size() < 2) return false;
      parsed.fetchedAtEpoch = toInt64(f[1]);
      if (parsed.fetchedAtEpoch <= 0) return false;
      sawHeader = true;
      continue;
    }

    if (f[0] == "R" && f.size() >= 9) {
      DirectionBoard dir;
      dir.globalRouteId = f[1];
      dir.routeShortName = f[2];
      dir.routeDisplayShortName.elements[0] = f[3];
      dir.routeDisplayShortName.elements[1] = f[4];
      dir.routeDisplayShortName.elements[2] = f[5];
      dir.routeColor = f[6];
      dir.routeTextColor = f[7];
      dir.directionId = static_cast<int>(toInt64(f[8]));
      parsed.board.push_back(dir);
    } else if (f[0] == "D" && f.size() >= 6 && !parsed.board.empty()) {
      DepartureRow dep;
      dep.headsign = f[1];
      dep.stopName = f[2];
      dep.departureTimeEpoch = toInt64(f[3]);
      dep.isRealTime = toBool(f[4]);
      dep.isLast = toBool(f[5]);
      parsed.board.back().departures.push_back(dep);
    } else if (f[0] == "P" && f.size() >= 5) {
      PresetTripPlan plan;
      plan.presetName = f[1];
      plan.found = toBool(f[2]);
      plan.leaveByEpoch = toInt64(f[3]);
      plan.fallbackMessage = f[4];
      parsed.presetPlans.push_back(plan);
    } else if (f[0] == "L" && f.size() >= 7 && !parsed.presetPlans.empty()) {
      PlannedLeg leg;
      leg.routeId = f[1];
      leg.routeShortName = f[2];
      leg.boardStopId = f[3];
      leg.alightStopId = f[4];
      leg.boardEpoch = toInt64(f[5]);
      leg.alightEpoch = toInt64(f[6]);
      parsed.presetPlans.back().legs.push_back(leg);
    }
    // Anything else is a line from a layout this build doesn't know — skip
    // it rather than failing, so a partially-recognized blob still yields
    // whatever it does understand.

    if (lineEnd >= blob.size()) break;
  }

  if (!sawHeader) return false;
  out = parsed;
  return true;
}

void pruneExpiredDepartures(CachedBoard& cache, int64_t nowEpoch) {
  if (nowEpoch <= 0) return;

  std::vector<DirectionBoard> kept;
  kept.reserve(cache.board.size());
  for (DirectionBoard dir : cache.board) {
    std::vector<DepartureRow> future;
    future.reserve(dir.departures.size());
    for (const DepartureRow& dep : dir.departures) {
      if (dep.departureTimeEpoch > nowEpoch) future.push_back(dep);
    }
    if (future.empty()) continue;
    dir.departures = future;
    kept.push_back(dir);
  }
  cache.board = kept;

  std::vector<PresetTripPlan> keptPlans;
  keptPlans.reserve(cache.presetPlans.size());
  for (const PresetTripPlan& plan : cache.presetPlans) {
    // A plan that was never found has no leave-by time to expire — keep it
    // so its fallback message ("Not configured", "No upcoming trip found")
    // still explains why that preset's line is blank.
    if (plan.found && plan.leaveByEpoch <= nowEpoch) continue;
    keptPlans.push_back(plan);
  }
  cache.presetPlans = keptPlans;
}

int cachedAgeMinutes(const CachedBoard& cache, int64_t nowEpoch) {
  if (cache.fetchedAtEpoch <= 0 || nowEpoch <= 0) return 0;
  if (nowEpoch <= cache.fetchedAtEpoch) return 0;
  return static_cast<int>((nowEpoch - cache.fetchedAtEpoch) / 60);
}

}  // namespace transit
