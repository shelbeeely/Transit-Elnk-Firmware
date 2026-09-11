#pragma once

// Transit-Elnk-Firmware — reader for the SD-card-resident STA GTFS tables
// tools/gen_sta_tables.py generates (sd_card_data/sta/{routes,stops,trips}.bin).
//
// File format (one shared shape for all three tables; see that script's
// gen_sd_binaries() for the writer):
//   bytes  0-3:  magic "STA1"
//   bytes  4-7:  uint32 recordCount
//   bytes  8-11: uint32 recordSize (bytes per fixed record, key included)
//   bytes 12-15: uint32 blobOffset (byte offset from file start where the
//                string blob begins)
//   [recordCount * recordSize bytes]: fixed records, sorted ascending by
//                each record's own first 4 bytes (a little-endian uint32
//                key) -- routeId, stopCode, or tripId depending on table.
//   [blob]: NUL-terminated UTF-8 strings; fixed-record fields that need
//                text (shortName, stopName, headsign) store a uint32
//                *Offset into this blob rather than the text inline, so a
//                repeated string (e.g. one headsign shared by many trips)
//                is stored once.
//
// This header is the hardware-independent half: BinaryTableReader abstracts
// "read N bytes at this offset" so the binary-search + record-decoding
// logic here builds and is unit-testable under [env:native] against a
// simple in-memory buffer, exactly like sta_feed_parser.h's protobuf
// decoder is tested against a real captured fixture. sta_sd_store.h (the
// hardware-only half, excluded from native) supplies the real
// implementation wrapping an SdFat FsFile's seek()+read() (SDCardManager),
// and adds the "no card / no file / bad magic" fallback callers actually
// see.
//
// Per-record encodings (all little-endian, no struct padding assumed —
// every field is read at an explicit byte offset, matching
// sta_feed_parser.cpp's own "never trust the compiler's struct layout for
// wire data" convention):
//   routes.bin (16 bytes): routeId:u32, shortNameOffset:u32, color:u32,
//     textColor:u32 (color/textColor are 0xRRGGBB, matching sta_route_table.h)
//   stops.bin  (24 bytes): stopCode:u32, stopId:char[8] (not NUL-padded --
//     always exactly 8 bytes, unlike sta_stop_table.h's char[9] which
//     leaves room for a NUL), nameOffset:u32, lat:f32, lon:f32
//   trips.bin  (16 bytes): tripId:u32, routeId:u32, directionId:u8 (the
//     standard GTFS 0/1 from static trips.txt -- unlike the live GTFS-RT
//     feed's own direction_id, see sta_feed_parser.h, this one is
//     reliable) + 3 reserved/padding bytes, headsignOffset:u32

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace transit {
namespace sta {

// Random-access byte source for one binary table file. The real
// implementation (sta_sd_store.h) wraps SD file I/O; tests use an
// in-memory buffer.
class BinaryTableReader {
 public:
  virtual ~BinaryTableReader() = default;

  // Reads exactly `len` bytes starting at `offset` into `out`. False on any
  // short read or out-of-range offset -- never partially fills `out` and
  // reports success, since a caller can't tell a partial fill from a full
  // one otherwise.
  virtual bool readAt(uint32_t offset, uint8_t* out, size_t len) = 0;
};

// The fixed header's on-disk size (magic + the three TableHeader fields) --
// public so every sequential-access reader (StaSdStore::nearbyStops(),
// which iterates stops.bin's records directly rather than through
// findRecordByKey()) computes the same record offsets this file's own
// binary search does, instead of a second hardcoded copy of this number.
constexpr size_t kTableHeaderSize = 16;

struct TableHeader {
  uint32_t recordCount = 0;
  uint32_t recordSize = 0;
  uint32_t blobOffset = 0;
};

// Reads and validates the 16-byte header (magic "STA1" plus the three
// fields above). False if the magic doesn't match or the reader itself
// fails (no file, no card, I/O error) -- from a caller's perspective
// that's the same "this table just isn't available right now" outcome
// either way.
bool readTableHeader(BinaryTableReader& reader, TableHeader& out);

// Binary-searches for the record whose leading uint32 key equals `key`,
// among a table already validated by readTableHeader(), and reads that
// whole record (key included) into `outRecord` (resized to
// header.recordSize). False if not found, or if a read failed partway
// through the search (treated as "not found" by every caller in this
// codebase, per sta_client.h's established "a failure here is just nothing
// to report" convention).
bool findRecordByKey(BinaryTableReader& reader, const TableHeader& header, uint32_t key,
                     std::vector<uint8_t>& outRecord);

// Reads the NUL-terminated string at blob-relative `offset`. `maxLen`
// (including the terminator) is a defensive bound against a corrupt or
// truncated blob that never hits a NUL -- this reads untrusted removable-
// media data, so an unbounded scan is not an acceptable failure mode here
// any more than it is for sta_feed_parser's network input.
bool readBlobString(BinaryTableReader& reader, const TableHeader& header, uint32_t offset,
                    std::string& out, size_t maxLen = 256);

// Typed views over the three tables tools/gen_sta_tables.py generates --
// each decodes one already-validated table's fixed record (+ a blob string
// read for the text field) into a usable struct. No table-specific I/O of
// their own beyond what readTableHeader/findRecordByKey/readBlobString
// already do.
struct SdRouteInfo {
  uint32_t routeId = 0;
  std::string shortName;
  uint32_t color = 0;
  uint32_t textColor = 0;
};

struct SdStopInfo {
  uint32_t stopCode = 0;
  std::string stopId;  // up to 8 chars, matches sta_feed_parser's stop_id
  std::string stopName;
  float lat = 0.0f;
  float lon = 0.0f;
};

struct SdTripInfo {
  uint32_t tripId = 0;
  uint32_t routeId = 0;
  uint8_t directionId = 0;
  std::string headsign;
};

bool lookupSdRoute(BinaryTableReader& reader, uint32_t routeId, SdRouteInfo& out);
bool lookupSdStop(BinaryTableReader& reader, uint32_t stopCode, SdStopInfo& out);
bool lookupSdTrip(BinaryTableReader& reader, uint32_t tripId, SdTripInfo& out);

// Decodes an already-fetched stops.bin record's fixed (non-string) fields
// -- everything but stopName, which is left empty; the caller reads it
// separately via readBlobString() using the returned blob offset, since
// that call needs the reader/header this function doesn't. `record` must
// be exactly 24 bytes (stops.bin's fixed record size).
//
// Pulled out of lookupSdStop() and shared with StaSdStore::nearbyStops()
// (which reads records sequentially rather than through
// findRecordByKey()'s single-record lookup) specifically so those two
// on-device call sites can't drift out of sync with each other the way a
// second hand-rolled copy of this decode risked.
void decodeStopRecordFixedFields(const uint8_t* record, SdStopInfo& out, uint32_t& outNameOffset);

// Great-circle distance in meters between two lat/lon points (WGS84 mean
// Earth radius). Used by sta_sd_store.h's nearbyStops() to rank
// sd_card_data/sta/stops.bin's ~1,665 entries by distance; pulled out here,
// not sta_sd_store.cpp, purely so it's unit-testable under [env:native]
// like the rest of this file.
double haversineMeters(double lat1, double lon1, double lat2, double lon2);

}  // namespace sta
}  // namespace transit
