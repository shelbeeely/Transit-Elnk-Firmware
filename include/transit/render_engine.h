#pragma once

// Transit-Elnk-Firmware — departure-board layout and drawing.
//
// Draws transit::DirectionBoard rows (already filtered/sorted/badged by
// ui_logic) through FreeInkUI's abstract freeink::ui::DrawTarget -- text via
// DrawTarget::text()/measureText() (FreeInkUI's bundled Noto Sans bitmap
// font, proven on this exact board by Free-Ink/inkdeck's FreeInkUIDisplayTarget
// usage -- no custom font pipeline); route icons via DrawTarget::bitmap()
// with tinted route-color compositing per docs/ASSETS_ICONS.md, using
// bitmaps supplied by icon_cache (unit 6).
//
// Hardware-independent: RenderEngine only ever touches the DrawTarget and
// FramePresenter references it's given, never EInkDisplay directly, so it
// builds and is fully unit-testable under [env:native] as well as
// [env:xteink_x4]. The [env:xteink_x4] path (src/main.cpp) constructs a real
// freeink::ui::DisplayTarget bound to EInkDisplay::getFrameBuffer() plus a
// FramePresenter that pushes via EInkDisplay::displayBuffer(); host-side
// tests (test/test_render_snapshot) pass a recording DrawTarget that
// rasterizes into an in-memory grayscale buffer and a no-op FramePresenter --
// see test/test_render_snapshot/host_render_target.h.
//
// Frozen contract for the parallel work units: do not change
// renderDepartureBoard's signature. The constructor was widened (host-side
// PNG-snapshot testing unit) to take a DrawTarget&/FramePresenter& pair
// instead of an EInkDisplay&; main.cpp's single construction call site was
// updated to match -- see that PR for details. Adding a method is fine; note
// it in your PR description.

#include <cstdint>
#include <string>
#include <vector>

#include <FreeInkUICore.h>

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

// Pushes a fully-drawn frame to the physical panel. RenderEngine only draws
// into the DrawTarget it's given; presenting the result to hardware is a
// separate concern kept behind this tiny interface so RenderEngine itself
// never depends on EInkDisplay (see the file comment above). The real
// implementation (main.cpp) wraps EInkDisplay::displayBuffer(FULL_REFRESH);
// host-side tests pass a no-op/recording stub.
class FramePresenter {
 public:
  virtual ~FramePresenter() = default;
  virtual void present() = 0;
};

class RenderEngine {
 public:
  // screenWidth/screenHeight are target's LOGICAL drawing-surface dimensions
  // (i.e. what a full-screen target.fill(Rect{0, 0, screenWidth, screenHeight}, ...)
  // covers). DrawTarget itself exposes no width/height accessor -- only the
  // concrete freeink::ui::DisplayTarget does (logicalWidth()/logicalHeight())
  // -- so the caller supplies them explicitly. Defaults are the X4 panel's
  // native landscape 800x480; main.cpp's real hardware path now passes
  // displayTarget.logicalWidth()/logicalHeight() explicitly instead of
  // relying on these defaults, since ConfigStore::displayPortrait() can swap
  // them to 480x800 at runtime -- see setScreenSize() below for how a
  // runtime orientation change is applied to an already-constructed engine.
  explicit RenderEngine(freeink::ui::DrawTarget& target, FramePresenter& presenter, IconCache& iconCache,
                        int16_t screenWidth = 800, int16_t screenHeight = 480);

  // Runtime orientation change (docs/CONFIG_AND_STATE.md's display_portrait
  // setting, changed via SetupFlow::runSettingsPortal()): call this with the
  // DrawTarget's new logicalWidth()/logicalHeight() after the caller has
  // already called target.setOrientation() on the same concrete
  // freeink::ui::DisplayTarget passed to the constructor above -- this just
  // updates the layout math's notion of screen size, it doesn't touch the
  // target itself.
  void setScreenSize(int16_t screenWidth, int16_t screenHeight);

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
  freeink::ui::DrawTarget& target_;
  FramePresenter& presenter_;
  IconCache& iconCache_;
  int16_t screenWidth_;
  int16_t screenHeight_;
};

}  // namespace transit
