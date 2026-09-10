// Placeholder host-side test for work unit 1 (data model & JSON parsing).
// Extend with fixture payloads from docs/API_CONTRACT.md's examples once
// the real parsers land — this just proves the harness/build wiring works.

#include <unity.h>

#include "transit/models.h"

void test_parse_nearby_routes_stub_returns_false_on_empty_json() {
  transit::NearbyRoutesResponse out;
  TEST_ASSERT_FALSE(transit::parseNearbyRoutes("", out));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_nearby_routes_stub_returns_false_on_empty_json);
  return UNITY_END();
}
