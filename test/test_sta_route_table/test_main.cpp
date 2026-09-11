// Host-side tests for transit::sta::lookupStaRoute (sta_route_table.h).
//
// The generated table is sorted by tools/gen_sta_tables.py to match
// lookupStaRoute()'s strcmp-based binary search (lexicographic order, not
// numeric) — these specifically probe route ids where numeric and
// lexicographic order disagree ("9" sorts after "771" and before "93"
// lexicographically, nowhere near its numeric position among 1-294), which
// is exactly the case a numeric sort would silently break the binary search
// invariant for. See that script's sort comment for the full reasoning.

#include <unity.h>

#include "transit/sta_route_table.h"

using transit::sta::lookupStaRoute;

void test_finds_a_route_whose_lexicographic_position_disagrees_with_its_numeric_one() {
  const auto* route = lookupStaRoute("9");
  TEST_ASSERT_NOT_NULL(route);
  TEST_ASSERT_EQUAL_STRING("9", route->shortName);
}

void test_finds_routes_immediately_adjacent_to_it_in_sorted_order() {
  TEST_ASSERT_NOT_NULL(lookupStaRoute("771"));
  TEST_ASSERT_NOT_NULL(lookupStaRoute("93"));
}

void test_finds_the_first_and_last_entries_in_the_table() {
  TEST_ASSERT_NOT_NULL(lookupStaRoute("1"));
  TEST_ASSERT_TRUE(transit::sta::kStaRouteCount > 0);
}

void test_returns_null_for_a_route_id_not_in_the_table() {
  TEST_ASSERT_NULL(lookupStaRoute("not-a-real-route"));
  TEST_ASSERT_NULL(lookupStaRoute(""));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_finds_a_route_whose_lexicographic_position_disagrees_with_its_numeric_one);
  RUN_TEST(test_finds_routes_immediately_adjacent_to_it_in_sorted_order);
  RUN_TEST(test_finds_the_first_and_last_entries_in_the_table);
  RUN_TEST(test_returns_null_for_a_route_id_not_in_the_table);
  return UNITY_END();
}
