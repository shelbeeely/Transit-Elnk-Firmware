// Stub implementation — replaced by work unit 4 (UI/business logic).
// Exists so the scaffold links and runs end to end before that unit lands.
// Real implementation applies docs/UI_BEHAVIOR.md's filter/sort/dedupe/
// window-cutoff/badge rules; this stub returns an empty board.

#include "transit/ui_logic.h"

namespace transit {

std::vector<DirectionBoard> buildDepartureBoard(const std::vector<Route>& /*routes*/,
                                                 const UiSettings& /*settings*/,
                                                 int64_t /*nowEpoch*/) {
  return {};
}

}  // namespace transit
