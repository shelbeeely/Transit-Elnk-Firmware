#pragma once

// Transit-Elnk-Firmware — SD-card-backed reader for STA's full static GTFS
// data (sd_card_data/sta/{routes,stops,trips}.bin — see
// tools/gen_sta_tables.py and sta_gtfs_binary.h for how those files are
// generated and shaped).
//
// Optional and additive: this supplements the small always-available
// flash-baked tables (sta_route_table.h/sta_stop_table.h), it never
// replaces them. begin() never blocks boot on SD trouble -- no card, a
// card that fails to mount, or a missing/corrupt table file all just leave
// the corresponding lookups unavailable (returning false), the same
// "nothing to report" outcome sta_client.h's own failure paths already
// use. Every caller in this codebase already has a flash-table fallback to
// reach for when a lookup here comes back false — see sta_client.cpp.
//
// Must be initialized (begin() called) before EInkDisplay::begin(): the X4
// shares its SPI bus between the display and the SD card (BoardConfig.h's
// XTEINK_X4.sd has no dedicated SCLK/MOSI of its own), and
// SDCardManager::begin()'s own comment notes it expects to run first so it
// can deselect a not-yet-initialized, never-deselected display controller
// that would otherwise drive the shared MISO line. See main.cpp's setup().
//
// Hardware-dependent (SDCardManager/SdFat) — only buildable under
// [env:xteink_x4], not [env:native]. sta_gtfs_binary.h (the actual
// record-decoding/binary-search logic this calls) is hardware-independent
// and tested there instead.

#include <cstdint>
#include <vector>

#include <SdFat.h>

#include "transit/sta_gtfs_binary.h"
#include "transit/sta_static_schedule.h"

namespace transit {
namespace sta {

class StaSdStore {
 public:
  // Mounts the SD card (via SDCardManager, shared with any other SD user)
  // and opens the three table files if present. attempted_ only guards
  // against redundant work *within* one call to setup() (a second begin()
  // call this wake is a cheap no-op) -- it does NOT persist across wake
  // cycles: main.cpp's setup() always ends in deep sleep, which resets the
  // MCU, so g_staSdStore (and attempted_ with it) is freshly
  // default-constructed on every wake regardless. A board with no card
  // (or one that fails to mount) pays SDCardManager::begin()'s mount
  // attempt once per wake, not once ever -- an RTC-memory-backed
  // "no card last time" cache could avoid that repeat cost, but isn't
  // implemented here (see docs/STA_INTEGRATION.md's known limitations).
  // Each table's availability is tracked independently: e.g. a card with
  // trips.bin but no stops.bin still serves trip lookups while stop
  // lookups fall through to the flash table.
  void begin();

  bool lookupRoute(uint32_t routeId, SdRouteInfo& out);
  bool lookupStop(uint32_t stopCode, SdStopInfo& out);
  bool lookupTrip(uint32_t tripId, SdTripInfo& out);

  // Every SD-resident stop within radiusMeters of (lat, lon), nearest
  // first, capped at maxResults. A linear scan of stops.bin (~1,665
  // records, ~40KB sequential read) -- there's no spatial index, but at
  // this record count a full scan is simpler and fast enough not to need
  // one. Empty if stops.bin isn't available. Exposed for a future nearby-
  // stop search UI (matching the Transit API setup flow's own lat/lon
  // search); not wired into setup_flow.cpp yet -- STA stop entry is still
  // the direct stop-code field, see docs/STA_INTEGRATION.md.
  std::vector<SdStopInfo> nearbyStops(double lat, double lon, double radiusMeters, int maxResults);

  // The tables sta_static_schedule.h needs to answer "when is the next
  // bus" from the timetable alone, with no network. Members are null for
  // whatever isn't available, which that module already treats as "no
  // schedule to offer" rather than an error -- so this is safe to call on
  // a board with no card, an old card, or a partially-copied one.
  //
  // Returned by value each wake rather than cached: setup() always ends in
  // deep sleep, so there is no session for a cache to live in (same
  // reasoning as begin()'s attempted_ flag).
  StaticScheduleTables scheduleTables();

  // What begin() actually found, for the boot report (boot_report.h). A
  // card that mounts but carries no tables is indistinguishable from a
  // missing card everywhere else in this firmware -- every caller already
  // degrades silently to the flash-baked tables -- so bringup has no way
  // to tell "no card" from "wrong card" without this. routesTable /
  // stopsTable / tripsTable are the version-1 set every card has;
  // stopTimesTable / calendarTable are the version-2 timetable additions,
  // and their absence on an otherwise-good card is exactly the symptom of
  // a card written by an older tools/gen_sta_tables.py.
  struct SdStatus {
    bool mounted = false;
    uint64_t totalBytes = 0;
    bool routesTable = false;
    bool stopsTable = false;
    bool tripsTable = false;
    bool stopTimesTable = false;
    bool calendarTable = false;
    bool calendarDatesTable = false;
  };
  SdStatus status() const;

 private:
  // Wraps one open FsFile as a BinaryTableReader (sta_gtfs_binary.h's
  // hardware-independent half only needs seek+read, both real SdFat
  // primitives).
  class FileReader : public BinaryTableReader {
   public:
    bool open(const char* path);
    bool isOpen() const { return static_cast<bool>(file_); }
    bool readAt(uint32_t offset, uint8_t* out, size_t len) override;

   private:
    FsFile file_;
  };

  bool attempted_ = false;
  bool sdMounted_ = false;
  FileReader routesReader_;
  FileReader stopsReader_;
  FileReader tripsReader_;
  // The static timetable (version-2 cards only -- see sta_gtfs_binary.h).
  // Much larger than the three above: stop_times.bin alone is ~2.6MB for
  // STA, which is nothing on an SD card but is the reason this data lives
  // there rather than in flash.
  FileReader stopTimesReader_;
  FileReader calendarReader_;
  FileReader calendarDatesReader_;
};

}  // namespace sta
}  // namespace transit
