#pragma once

// Transit-Elnk-Firmware — last-known-good departure board, kept across deep
// sleep so an offline wake shows something useful.
//
// Before this existed, a wake with no Wi-Fi (or a failed fetch) drew the
// bare "No departures to show." placeholder: correct, but useless on a bus
// where the board is carried out of range of the network it was provisioned
// on. This module persists the *already-computed* board — the output of
// ui_logic::buildDepartureBoard() plus each configured preset's
// PresetTripPlan — so the next wake can redraw it, clearly marked as cached,
// instead of drawing nothing.
//
// Why cache the computed board rather than the raw stop_departures JSON: a
// multi-stop response with presets configured runs to tens of kilobytes,
// well past what an NVS string value holds (~4000 bytes), whereas the
// board that actually gets drawn is a few hundred. The cost is that cached
// data can't be re-filtered against changed display settings — a setting
// changed while offline takes effect at the next successful fetch. That's a
// deliberate trade, documented in docs/OFFLINE_AND_BUS_WIFI.md.
//
// Departure times are absolute epochs, so a cached board ages correctly on
// its own: pruneExpiredDepartures() drops what has already left, and what
// remains still counts down properly against the approximate clock
// (time_keeper.h). A cache eventually empties itself out rather than
// showing stale times as if they were live.
//
// Pure and host-testable — no NVS/network/display dependency. ConfigStore
// owns the actual persistence (cachedBoard()/setCachedBoard()).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "transit/trip_planner.h"
#include "transit/ui_logic.h"

namespace transit {

// Upper bound on a serialized cache blob. ESP-IDF caps a single NVS string
// value at just under 4000 bytes; this leaves headroom under that rather
// than serializing right up to the edge and failing the write at runtime.
constexpr size_t kMaxCachedBoardBytes = 3500;

struct CachedBoard {
  // When the data was fetched (the wake's own nowEpoch). 0 = no cache.
  int64_t fetchedAtEpoch = 0;
  std::vector<DirectionBoard> board;
  // One entry per configured preset, so main.cpp can re-format each line
  // against the *current* clock on restore — a cached "leave by 5:42p" is
  // still true as an absolute time, but whether it's urgent right now is
  // not something the cache can know.
  std::vector<PresetTripPlan> presetPlans;
};

// Serializes to a compact tab/newline-delimited text blob (NVS has no blob
// type exposed through Arduino Preferences' string API that's any cheaper).
// Never exceeds maxBytes: preset plans are written first because they're
// small and carry the most decision-relevant information, then board rows
// are appended while they fit and dropped once they don't. Returns an empty
// string when there is nothing worth caching.
std::string serializeCachedBoard(const CachedBoard& cache, size_t maxBytes = kMaxCachedBoardBytes);

// Parses a blob produced by serializeCachedBoard(). Returns false (leaving
// out untouched) for an empty string, an unrecognized version tag, or a
// blob carrying no fetch timestamp. Individual malformed lines inside an
// otherwise-valid blob are skipped rather than failing the whole parse —
// a truncated cache should still yield the rows that did survive.
bool deserializeCachedBoard(const std::string& blob, CachedBoard& out);

// Drops departures at or before nowEpoch, then drops any DirectionBoard
// left with no departures at all, so a restored cache never renders a
// departure that has already gone (which would otherwise show as "Due"
// forever — formatDepartureChip() clamps negative minutes to 0). Also drops
// preset plans whose leave-by time has passed. A no-op when nowEpoch <= 0,
// since without a clock there is no basis for calling anything expired.
void pruneExpiredDepartures(CachedBoard& cache, int64_t nowEpoch);

// Whole minutes between the cache's fetch time and nowEpoch, for the
// "cached Xm ago" header treatment (render_engine.h's BoardStatus). Returns
// 0 when either timestamp is missing or the cache somehow post-dates now
// (a clock correction can move time backwards).
int cachedAgeMinutes(const CachedBoard& cache, int64_t nowEpoch);

}  // namespace transit
