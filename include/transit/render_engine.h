#pragma once

// Transit-Elnk-Firmware — departure-board layout and drawing.
//
// Draws transit::DirectionBoard rows (already filtered/sorted/badged by
// ui_logic) onto the Xteink X4 panel. Text via FreeInkUI's stock
// DisplayTarget/ui::TextStyle (proven on this exact board by Free-Ink/
// inkdeck's FreeInkUIDisplayTarget usage — no custom font pipeline); route
// icons via EInkDisplay::drawImage with tinted route-color compositing per
// docs/ASSETS_ICONS.md, using bitmaps supplied by icon_cache (unit 6).
//
// Hardware-dependent (EInkDisplay) — only buildable/testable under
// [env:xteink_x4], not [env:native].
//
// Frozen contract for the parallel work units: do not change the
// RenderEngine constructor or renderDepartureBoard signature. Adding a
// method is fine; note it in your PR description.

#include <cstdint>
#include <string>
#include <vector>

#include <EInkDisplay.h>

#include "transit/icon_cache.h"
#include "transit/ui_logic.h"

namespace transit {

// Top-of-screen status the render engine draws above the departure rows.
struct BoardStatus {
  std::string stopName;
  int batteryPercent = 100;
  bool wifiOk = true;
  bool lastFetchFailed = false;
  int64_t lastUpdatedEpoch = 0;
};

class RenderEngine {
 public:
  explicit RenderEngine(EInkDisplay& display, IconCache& iconCache);

  // Lays out and draws one full departure-board frame (status header + one
  // row per DirectionBoard entry, each row's departures rendered per
  // docs/UI_BEHAVIOR.md's badge rules) and pushes it to the panel with a
  // full refresh. Called once per wake cycle by main.cpp.
  void renderDepartureBoard(const std::vector<DirectionBoard>& board, const BoardStatus& status);

  // First-run setup screens (Wi-Fi/API-key/stop-search prompts and results)
  // drawn by setup_flow (unit 8) via this engine, so all text/layout goes
  // through one rendering path.
  void renderSetupPrompt(const std::string& title, const std::string& body);
  void renderSetupList(const std::string& title, const std::vector<std::string>& items,
                       int selectedIndex);

 private:
  EInkDisplay& display_;
  IconCache& iconCache_;
};

}  // namespace transit
