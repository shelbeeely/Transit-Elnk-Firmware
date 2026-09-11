// Host-side tests for the sta_gtfs_binary.h reader (the SD-card GTFS table
// format tools/gen_sta_tables.py's gen_sd_binaries() writes -- see that
// header for the exact byte layout).
//
// InMemoryReader stands in for the real SD-backed reader (sta_sd_store.h,
// hardware-only, untestable here): it strictly refuses any read that would
// run past the end of its buffer, the same way a real file read at EOF
// would, so tests exercise the same short-read handling a real card would
// trigger (see the "last string in the blob" test).

#include <unity.h>

#include <cstring>

#include "transit/sta_gtfs_binary.h"

using transit::sta::BinaryTableReader;
using transit::sta::findRecordByKey;
using transit::sta::haversineMeters;
using transit::sta::lookupSdRoute;
using transit::sta::lookupSdStop;
using transit::sta::lookupSdTrip;
using transit::sta::readBlobString;
using transit::sta::readTableHeader;
using transit::sta::SdRouteInfo;
using transit::sta::SdStopInfo;
using transit::sta::SdTripInfo;
using transit::sta::TableHeader;

namespace {

class InMemoryReader : public BinaryTableReader {
 public:
  explicit InMemoryReader(std::vector<uint8_t> data) : data_(std::move(data)) {}

  bool readAt(uint32_t offset, uint8_t* out, size_t len) override {
    if (static_cast<uint64_t>(offset) + len > data_.size()) return false;
    std::memcpy(out, data_.data() + offset, len);
    return true;
  }

 private:
  std::vector<uint8_t> data_;
};

void appendU32(std::vector<uint8_t>& buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v));
  buf.push_back(static_cast<uint8_t>(v >> 8));
  buf.push_back(static_cast<uint8_t>(v >> 16));
  buf.push_back(static_cast<uint8_t>(v >> 24));
}

void appendF32(std::vector<uint8_t>& buf, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  appendU32(buf, bits);
}

void appendString(std::vector<uint8_t>& buf, const std::string& s) {
  buf.insert(buf.end(), s.begin(), s.end());
  buf.push_back(0);
}

// Mirrors tools/gen_sta_tables.py's BlobBuilder: computes each string's
// blob-relative offset from the actual bytes appended so far, rather than
// a test hand-computing (and risking mis-counting) string lengths itself.
class BlobBuilder {
 public:
  uint32_t append(const std::string& s) {
    const uint32_t offset = static_cast<uint32_t>(bytes_.size());
    appendString(bytes_, s);
    return offset;
  }
  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
};

// Builds one sta_gtfs_binary-format file: `records` are already-sorted
// (key, fixedBytesAfterKey) pairs; `blobParts` are the NUL-terminated
// strings those records' *Offset fields point into, in the same order the
// caller computed those offsets against.
std::vector<uint8_t> buildTable(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& records,
                                const std::vector<uint8_t>& blob) {
  const uint32_t recordSize = 4 + (records.empty() ? 0 : static_cast<uint32_t>(records[0].second.size()));
  std::vector<uint8_t> body;
  for (const auto& [key, rest] : records) {
    appendU32(body, key);
    body.insert(body.end(), rest.begin(), rest.end());
  }
  const uint32_t blobOffset = 16 + static_cast<uint32_t>(body.size());

  std::vector<uint8_t> file;
  file.push_back('S');
  file.push_back('T');
  file.push_back('A');
  file.push_back('1');
  appendU32(file, static_cast<uint32_t>(records.size()));
  appendU32(file, recordSize);
  appendU32(file, blobOffset);
  file.insert(file.end(), body.begin(), body.end());
  file.insert(file.end(), blob.begin(), blob.end());
  return file;
}

// A tiny 3-stop table: codes 1007, 2101, 4377, matching real STA data shapes
// (stop_id truncated to 8 bytes, real-looking lat/lon) but hand-built here
// rather than pulled from a fixture file, since the format is simple enough
// to construct directly and this keeps the test self-contained.
std::vector<uint8_t> buildStopsTable() {
  BlobBuilder blob;
  const uint32_t washingtonOffset = blob.append("Washington @ North River Drive");
  const uint32_t sccOffset = blob.append("SCC Transit Center Bay 3");

  auto stopRecord = [](const char* stopId, uint32_t nameOffset, float lat, float lon) {
    std::vector<uint8_t> rest;
    rest.insert(rest.end(), stopId, stopId + 8);
    appendU32(rest, nameOffset);
    appendF32(rest, lat);
    appendF32(rest, lon);
    return rest;
  };

  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> records = {
      {1007, stopRecord("WASNORSF", washingtonOffset, 47.664675f, -117.417746f)},
      // "MIDSTOP\0" is exactly 8 bytes including the string literal's own
      // NUL -- matches the real generator's fixed 8-byte field (a 7-char id
      // is NUL-padded the same way).
      {2101, stopRecord("MIDSTOP", washingtonOffset, 47.6f, -117.4f)},
      {4377, stopRecord("SCCBAY3", sccOffset, 47.673183f, -117.35846f)},
  };

  return buildTable(records, blob.bytes());
}

}  // namespace

void test_reads_a_valid_header() {
  InMemoryReader reader(buildStopsTable());
  TableHeader header;
  TEST_ASSERT_TRUE(readTableHeader(reader, header));
  TEST_ASSERT_EQUAL_UINT32(3, header.recordCount);
  TEST_ASSERT_EQUAL_UINT32(24, header.recordSize);
}

void test_rejects_bad_magic() {
  std::vector<uint8_t> data = buildStopsTable();
  data[0] = 'X';
  InMemoryReader reader(data);
  TableHeader header;
  TEST_ASSERT_FALSE(readTableHeader(reader, header));
}

void test_rejects_a_blob_offset_that_overlaps_the_record_table() {
  std::vector<uint8_t> data = buildStopsTable();
  // Corrupt blobOffset (bytes 12-15) to point inside the record table.
  data[12] = 0;
  data[13] = 0;
  data[14] = 0;
  data[15] = 0;
  InMemoryReader reader(data);
  TableHeader header;
  TEST_ASSERT_FALSE(readTableHeader(reader, header));
}

void test_finds_first_middle_and_last_record() {
  InMemoryReader reader(buildStopsTable());
  TableHeader header;
  TEST_ASSERT_TRUE(readTableHeader(reader, header));

  std::vector<uint8_t> record;
  TEST_ASSERT_TRUE(findRecordByKey(reader, header, 1007, record));
  TEST_ASSERT_TRUE(findRecordByKey(reader, header, 2101, record));
  TEST_ASSERT_TRUE(findRecordByKey(reader, header, 4377, record));
}

void test_does_not_find_an_absent_key() {
  InMemoryReader reader(buildStopsTable());
  TableHeader header;
  TEST_ASSERT_TRUE(readTableHeader(reader, header));

  std::vector<uint8_t> record;
  TEST_ASSERT_FALSE(findRecordByKey(reader, header, 9999, record));
  TEST_ASSERT_FALSE(findRecordByKey(reader, header, 0, record));
}

void test_lookup_sd_stop_decodes_all_fields() {
  InMemoryReader reader(buildStopsTable());
  SdStopInfo info;
  TEST_ASSERT_TRUE(lookupSdStop(reader, 4377, info));
  TEST_ASSERT_EQUAL_UINT32(4377, info.stopCode);
  TEST_ASSERT_EQUAL_STRING("SCCBAY3", info.stopId.c_str());
  TEST_ASSERT_EQUAL_STRING("SCC Transit Center Bay 3", info.stopName.c_str());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 47.673183f, info.lat);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -117.35846f, info.lon);
}

void test_lookup_sd_stop_not_found_returns_false() {
  InMemoryReader reader(buildStopsTable());
  SdStopInfo info;
  TEST_ASSERT_FALSE(lookupSdStop(reader, 1, info));
}

// The blob's very last string ends exactly at EOF -- reading the full
// default maxLen window would run past the buffer, which InMemoryReader
// (like a real file read at EOF) refuses; readBlobString must shrink the
// window and still find it, not just fail outright.
void test_reads_the_last_string_in_the_blob_ending_exactly_at_eof() {
  std::vector<uint8_t> blob;
  appendString(blob, "Short");
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> records = {{1, {}}};
  std::vector<uint8_t> data = buildTable(records, blob);
  InMemoryReader reader(data);

  TableHeader header;
  TEST_ASSERT_TRUE(readTableHeader(reader, header));
  std::string out;
  TEST_ASSERT_TRUE(readBlobString(reader, header, 0, out, /*maxLen=*/256));
  TEST_ASSERT_EQUAL_STRING("Short", out.c_str());
}

void test_lookup_sd_route_and_trip_decode_correctly() {
  BlobBuilder blob;
  const uint32_t shortNameOffset = blob.append("34");
  const uint32_t headsignOffset = blob.append("South Hill P&R");

  std::vector<uint8_t> routeRest;
  appendU32(routeRest, shortNameOffset);
  appendU32(routeRest, 0x3155A6);   // color
  appendU32(routeRest, 0xFFFFFF);   // textColor
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> routeRecords = {{34, routeRest}};
  InMemoryReader routeReader(buildTable(routeRecords, blob.bytes()));

  SdRouteInfo routeInfo;
  TEST_ASSERT_TRUE(lookupSdRoute(routeReader, 34, routeInfo));
  TEST_ASSERT_EQUAL_UINT32(34, routeInfo.routeId);
  TEST_ASSERT_EQUAL_STRING("34", routeInfo.shortName.c_str());
  TEST_ASSERT_EQUAL_UINT32(0x3155A6, routeInfo.color);

  std::vector<uint8_t> tripRest;
  appendU32(tripRest, 34);   // routeId
  tripRest.push_back(1);    // directionId
  tripRest.push_back(0);
  tripRest.push_back(0);
  tripRest.push_back(0);    // 3 reserved bytes
  appendU32(tripRest, headsignOffset);
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> tripRecords = {{1367412, tripRest}};
  InMemoryReader tripReader(buildTable(tripRecords, blob.bytes()));

  SdTripInfo tripInfo;
  TEST_ASSERT_TRUE(lookupSdTrip(tripReader, 1367412, tripInfo));
  TEST_ASSERT_EQUAL_UINT32(1367412, tripInfo.tripId);
  TEST_ASSERT_EQUAL_UINT32(34, tripInfo.routeId);
  TEST_ASSERT_EQUAL_UINT8(1, tripInfo.directionId);
  TEST_ASSERT_EQUAL_STRING("South Hill P&R", tripInfo.headsign.c_str());
}

void test_haversine_distance_between_two_real_sta_stops() {
  // SCC Transit Center Bay 3 -> Washington @ North River Drive, computed
  // independently in Python for this same pair of real STA stop
  // coordinates (both seen in tools/gen_sta_tables.py's own test data).
  const double meters = haversineMeters(47.673183, -117.35846, 47.664675, -117.417746);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 4539.03f, static_cast<float>(meters));
}

void test_haversine_distance_to_the_same_point_is_zero() {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, static_cast<float>(haversineMeters(47.673183, -117.35846, 47.673183, -117.35846)));
}

void test_empty_table_never_finds_anything() {
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> noRecords;
  InMemoryReader reader(buildTable(noRecords, {}));
  TableHeader header;
  TEST_ASSERT_TRUE(readTableHeader(reader, header));
  TEST_ASSERT_EQUAL_UINT32(0, header.recordCount);
  std::vector<uint8_t> record;
  TEST_ASSERT_FALSE(findRecordByKey(reader, header, 1, record));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_reads_a_valid_header);
  RUN_TEST(test_rejects_bad_magic);
  RUN_TEST(test_rejects_a_blob_offset_that_overlaps_the_record_table);
  RUN_TEST(test_finds_first_middle_and_last_record);
  RUN_TEST(test_does_not_find_an_absent_key);
  RUN_TEST(test_lookup_sd_stop_decodes_all_fields);
  RUN_TEST(test_lookup_sd_stop_not_found_returns_false);
  RUN_TEST(test_reads_the_last_string_in_the_blob_ending_exactly_at_eof);
  RUN_TEST(test_lookup_sd_route_and_trip_decode_correctly);
  RUN_TEST(test_haversine_distance_between_two_real_sta_stops);
  RUN_TEST(test_haversine_distance_to_the_same_point_is_zero);
  RUN_TEST(test_empty_table_never_finds_anything);
  return UNITY_END();
}
