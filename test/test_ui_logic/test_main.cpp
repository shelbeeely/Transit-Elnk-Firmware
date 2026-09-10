// Placeholder host-side test for work unit 4 (UI/business logic).
// Extend with real filter/sort/dedupe/badge cases from docs/UI_BEHAVIOR.md
// once the real implementation lands — this just proves the harness/build
// wiring works.

#include <unity.h>

#include "transit/ui_logic.h"

void test_build_departure_board_stub_returns_empty() {
  std::vector<transit::Route> routes;
  transit::UiSettings settings;
  auto board = transit::buildDepartureBoard(routes, settings, /*nowEpoch=*/0);
  TEST_ASSERT_TRUE(board.empty());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_build_departure_board_stub_returns_empty);
  return UNITY_END();
}
