// Host-side tests for transit::sta::parseTripUpdates (sta_feed_parser.h).
//
// kFixtureBytes is a real, minimal GTFS-RT FeedMessage: entity 1 is byte-
// for-byte the shape of a trip actually captured live from STA's
// TripUpdates.pb feed (trip 1367412, route 34, stop SCCBAY3, destination
// "South Hill P&R" — cross-checked against STA's own published GTFS static
// data), re-serialized down to just what this test needs plus two more
// entities constructed (via the real `google.transit.gtfs_realtime_pb2`
// Python bindings, not hand-typed bytes) to cover schedule_relationship
// SKIPPED, an arrival-only stop_time_update, a route_id absent from
// sta_route_table.h, and a trip with no trip_properties at all. See
// tools/gen_sta_tables.py's docstring for where the route/stop tables this
// parser cross-references come from.

#include <unity.h>

#include "transit/sta_feed_parser.h"
#include "transit/sta_models.h"

using transit::sta::parseTripUpdates;
using transit::sta::StaDeparture;

namespace {

const uint8_t kFixtureBytes[] = {
  10, 13, 10, 3, 50, 46, 48, 16, 0, 24, 248, 155, 142, 213, 6, 18, 108, 10, 7, 49,
  51, 54, 55, 52, 49, 50, 26, 97, 10, 13, 10, 7, 49, 51, 54, 55, 52, 49, 50, 42,
  2, 51, 52, 18, 25, 18, 6, 16, 149, 162, 142, 213, 6, 26, 6, 16, 149, 162, 142, 213,
  6, 34, 7, 83, 67, 67, 66, 65, 89, 51, 18, 18, 18, 6, 16, 244, 162, 142, 213, 6,
  34, 8, 70, 82, 69, 84, 82, 69, 83, 78, 50, 33, 10, 7, 49, 51, 54, 55, 52, 49,
  50, 50, 22, 10, 20, 10, 14, 83, 111, 117, 116, 104, 32, 72, 105, 108, 108, 32, 80, 38,
  82, 18, 2, 101, 110, 18, 100, 10, 7, 57, 57, 57, 57, 57, 57, 57, 26, 89, 10, 13,
  10, 7, 57, 57, 57, 57, 57, 57, 57, 42, 2, 52, 51, 18, 11, 34, 7, 83, 67, 67,
  66, 65, 89, 51, 40, 1, 18, 17, 18, 6, 16, 232, 166, 142, 213, 6, 34, 7, 83, 67,
  67, 66, 65, 89, 51, 50, 40, 10, 7, 57, 57, 57, 57, 57, 57, 57, 50, 29, 10, 27,
  10, 21, 76, 105, 110, 99, 111, 108, 110, 47, 51, 55, 116, 104, 32, 65, 118, 101, 32, 84,
  101, 115, 116, 18, 2, 101, 110, 18, 44, 10, 6, 52, 50, 52, 50, 52, 50, 26, 34, 10,
  13, 10, 6, 52, 50, 52, 50, 52, 50, 42, 3, 57, 57, 57, 18, 17, 26, 6, 16, 208,
  174, 142, 213, 6, 34, 7, 83, 67, 67, 66, 65, 89, 51,
};
constexpr size_t kFixtureLen = sizeof(kFixtureBytes);

const StaDeparture* findByTripId(const std::vector<StaDeparture>& deps, const std::string& tripId) {
  for (const auto& d : deps) {
    if (d.tripId == tripId) return &d;
  }
  return nullptr;
}

}  // namespace

void test_parses_a_real_captured_trip_at_the_target_stop() {
  std::vector<StaDeparture> out;
  TEST_ASSERT_TRUE(parseTripUpdates(kFixtureBytes, kFixtureLen, "SCCBAY3", out));

  const StaDeparture* dep = findByTripId(out, "1367412");
  TEST_ASSERT_NOT_NULL(dep);
  TEST_ASSERT_EQUAL_STRING("34", dep->routeId.c_str());
  TEST_ASSERT_EQUAL_STRING("34", dep->routeShortName.c_str());  // sta_route_table.h: route 34 is "Freya"'s short name "34"
  TEST_ASSERT_EQUAL_STRING("South Hill P&R", dep->destination.c_str());
  TEST_ASSERT_EQUAL_INT64(1789104405, dep->departureEpoch);
  // routes.txt: route 34's color.
  TEST_ASSERT_EQUAL_UINT32(0x3155A6, dep->routeColor);
}

void test_excludes_a_stop_time_update_at_a_different_stop() {
  std::vector<StaDeparture> out;
  TEST_ASSERT_TRUE(parseTripUpdates(kFixtureBytes, kFixtureLen, "SCCBAY3", out));

  // Trip 1367412's second stop_time_update (FRETRESN) must not leak in as a
  // second entry for the same trip.
  int count = 0;
  for (const auto& d : out) {
    if (d.tripId == "1367412") ++count;
  }
  TEST_ASSERT_EQUAL_INT(1, count);
}

void test_excludes_a_skipped_stop_time_update_even_at_the_target_stop() {
  std::vector<StaDeparture> out;
  TEST_ASSERT_TRUE(parseTripUpdates(kFixtureBytes, kFixtureLen, "SCCBAY3", out));

  // Trip 9999999 has two stop_time_updates at SCCBAY3: one SKIPPED (must be
  // dropped) and one arrival-only (must survive via the arrival fallback
  // below) -- exactly one StaDeparture should come out of it, not two.
  int count = 0;
  for (const auto& d : out) {
    if (d.tripId == "9999999") ++count;
  }
  TEST_ASSERT_EQUAL_INT(1, count);
}

void test_falls_back_to_arrival_time_when_departure_is_absent() {
  std::vector<StaDeparture> out;
  TEST_ASSERT_TRUE(parseTripUpdates(kFixtureBytes, kFixtureLen, "SCCBAY3", out));

  const StaDeparture* dep = findByTripId(out, "9999999");
  TEST_ASSERT_NOT_NULL(dep);
  TEST_ASSERT_EQUAL_INT64(1789105000, dep->departureEpoch);
  TEST_ASSERT_EQUAL_STRING("Lincoln/37th Ave Test", dep->destination.c_str());
}

void test_falls_back_to_raw_route_id_and_route_label_when_table_lookup_misses() {
  std::vector<StaDeparture> out;
  TEST_ASSERT_TRUE(parseTripUpdates(kFixtureBytes, kFixtureLen, "SCCBAY3", out));

  // Trip 424242 references route_id "999", which sta_route_table.h doesn't
  // know about (not one of STA's real routes), and carries no
  // trip_properties at all.
  const StaDeparture* dep = findByTripId(out, "424242");
  TEST_ASSERT_NOT_NULL(dep);
  TEST_ASSERT_EQUAL_STRING("999", dep->routeId.c_str());
  TEST_ASSERT_EQUAL_STRING("999", dep->routeShortName.c_str());
  TEST_ASSERT_EQUAL_STRING("", dep->destination.c_str());
  TEST_ASSERT_EQUAL_INT64(1789106000, dep->departureEpoch);
}

void test_returns_no_departures_for_a_stop_id_not_in_the_feed() {
  std::vector<StaDeparture> out;
  TEST_ASSERT_TRUE(parseTripUpdates(kFixtureBytes, kFixtureLen, "NOWHERE", out));
  TEST_ASSERT_TRUE(out.empty());
}

void test_rejects_truncated_input() {
  std::vector<StaDeparture> out;
  // Cut the buffer off mid-message -- the trailing bytes at the end of a
  // length-delimited field's declared span won't be there, so the decoder
  // must fail rather than silently read garbage/out of bounds.
  TEST_ASSERT_FALSE(parseTripUpdates(kFixtureBytes, kFixtureLen - 5, "SCCBAY3", out));
}

void test_empty_input_parses_to_no_departures() {
  std::vector<StaDeparture> out;
  TEST_ASSERT_TRUE(parseTripUpdates(kFixtureBytes, 0, "SCCBAY3", out));
  TEST_ASSERT_TRUE(out.empty());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_parses_a_real_captured_trip_at_the_target_stop);
  RUN_TEST(test_excludes_a_stop_time_update_at_a_different_stop);
  RUN_TEST(test_excludes_a_skipped_stop_time_update_even_at_the_target_stop);
  RUN_TEST(test_falls_back_to_arrival_time_when_departure_is_absent);
  RUN_TEST(test_falls_back_to_raw_route_id_and_route_label_when_table_lookup_misses);
  RUN_TEST(test_returns_no_departures_for_a_stop_id_not_in_the_feed);
  RUN_TEST(test_rejects_truncated_input);
  RUN_TEST(test_empty_input_parses_to_no_departures);
  return UNITY_END();
}
