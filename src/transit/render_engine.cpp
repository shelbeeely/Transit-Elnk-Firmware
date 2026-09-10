// Stub implementation — replaced by work unit 5 (render engine).
// Exists so the scaffold links and runs end to end before that unit lands.
// Real implementation lays out DirectionBoard rows via FreeInkUI's stock
// text rendering + tinted icon_cache bitmaps, per docs/UI_BEHAVIOR.md and
// docs/ASSETS_ICONS.md.

#include "transit/render_engine.h"

namespace transit {

RenderEngine::RenderEngine(EInkDisplay& display, IconCache& iconCache)
    : display_(display), iconCache_(iconCache) {}

void RenderEngine::renderDepartureBoard(const std::vector<DirectionBoard>& /*board*/,
                                        const BoardStatus& /*status*/) {
  display_.clearScreen(0xFF);
  display_.displayBuffer(EInkDisplay::FULL_REFRESH);
}

void RenderEngine::renderSetupPrompt(const std::string& /*title*/, const std::string& /*body*/) {
  display_.clearScreen(0xFF);
  display_.displayBuffer(EInkDisplay::FULL_REFRESH);
}

void RenderEngine::renderSetupList(const std::string& /*title*/,
                                   const std::vector<std::string>& /*items*/,
                                   int /*selectedIndex*/) {
  display_.clearScreen(0xFF);
  display_.displayBuffer(EInkDisplay::FULL_REFRESH);
}

}  // namespace transit
