// Host-side visual-regression snapshot test for RenderEngine (render_engine.h).
//
// Renders the departure board and both first-run setup screens through
// HostRasterTarget (a recording freeink::ui::DrawTarget, host_render_target.h)
// instead of real hardware, asserts each frame actually painted a
// nontrivial, multi-tone layout (not just "didn't crash"), and dumps each
// frame to a real PNG under .pio/test-output/render_snapshot/ (.pio/ is
// already gitignored, see platformio.ini) so a human -- or an image-diffing
// CI step in a future unit -- can actually look at the layout.
//
// IconCache is exercised for real (not stubbed out): FakeHttpTransport below
// always reports a transport failure, so every icon lookup takes
// IconCache's documented "fetch failed -> null IconBitmap" path and
// RenderEngine's drawRouteBadge() falls back to its text badge -- a
// realistic no-network scenario, and it means this test also covers that
// fallback path (see render_engine.cpp's drawRouteBadge()).

#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

#include "host_render_target.h"
#include "transit/api_client.h"
#include "transit/icon_cache.h"
#include "transit/render_engine.h"
#include "transit/ui_logic.h"

using transit::BoardStatus;
using transit::DepartureRow;
using transit::DirectionBoard;
using transit::DisplayShortName;
using transit::FramePresenter;
using transit::HttpResponse;
using transit::HttpTransport;
using transit::IconCache;
using transit::RenderEngine;

namespace {

constexpr int16_t kScreenWidth = 800;
constexpr int16_t kScreenHeight = 480;
constexpr int64_t kNow = 1'700'000'000;

class FakeHttpTransport : public HttpTransport {
 public:
  HttpResponse get(const std::string& /*url*/,
                   const std::vector<std::pair<std::string, std::string>>& /*headers*/) override {
    ++callCount;
    return HttpResponse{};  // transportOk = false: every icon fetch "fails"
  }
  int callCount = 0;
};

class NoopPresenter : public FramePresenter {
 public:
  void present() override { ++presentCount; }
  int presentCount = 0;
};

DepartureRow makeDeparture(const std::string& headsign, int64_t departureEpoch, bool realTime = false,
                           bool isLast = false) {
  DepartureRow row;
  row.headsign = headsign;
  row.stopName = "Main St & 5th Ave";
  row.departureTimeEpoch = departureEpoch;
  row.isRealTime = realTime;
  row.isLast = isLast;
  return row;
}

DisplayShortName makeDisplayShortName(const std::string& imageSlug, const std::string& labelText) {
  DisplayShortName name;
  name.elements[0] = imageSlug;
  name.elements[1] = labelText;
  return name;
}

// A handful of representative rows: a real-time departure, a scheduled one,
// a "last" departure, and a direction with no upcoming departures at all
// (drawDirectionRow()'s "No upcoming departures" branch) -- enough surface
// to exercise render_engine.cpp's layout without needing the full
// ui_logic::buildDepartureBoard pipeline (this test constructs
// board-ready DirectionBoard rows directly, same as ui_logic's own output).
std::vector<DirectionBoard> makeSampleBoard() {
  std::vector<DirectionBoard> board;

  // Route colors are deliberately spread across three of the panel's four
  // quantized gray buckets (docs/ASSETS_ICONS.md) -- near-black, mid-gray,
  // near-white -- so none of them trip the "quantized colors collide"
  // text-badge fallback (renderDepartureBoard()'s tint-collision check);
  // this test wants the *other* fallback path instead (icon fetch failing,
  // below), to cover both fallbacks across the whole test file without
  // conflating them in one case.
  DirectionBoard route55;
  route55.globalRouteId = "R55";
  route55.routeShortName = "55";
  route55.routeDisplayShortName = makeDisplayShortName("bus-55", "55");
  route55.routeColor = "111111";
  route55.routeTextColor = "FFFFFF";
  route55.directionId = 0;
  route55.departures = {
      makeDeparture("Downtown via Main St", kNow + 3 * 60, /*realTime=*/true),
      makeDeparture("Downtown via Main St", kNow + 18 * 60),
      makeDeparture("Downtown via Main St", kNow + 34 * 60),
  };
  board.push_back(route55);

  DirectionBoard route10;
  route10.globalRouteId = "R10";
  route10.routeShortName = "10";
  route10.routeDisplayShortName = makeDisplayShortName("bus-10", "");
  route10.routeColor = "999999";
  route10.routeTextColor = "FFFFFF";
  route10.directionId = 1;
  route10.departures = {
      makeDeparture("Uptown Express", kNow + 7 * 60),
      makeDeparture("Uptown Express", kNow + 41 * 60, /*realTime=*/false, /*isLast=*/true),
  };
  board.push_back(route10);

  DirectionBoard routeExpress;
  routeExpress.globalRouteId = "R7X";
  routeExpress.routeShortName = "7X";
  routeExpress.routeDisplayShortName = makeDisplayShortName("bus-7x", "7X");
  routeExpress.routeColor = "EEEEEE";
  routeExpress.routeTextColor = "000000";
  routeExpress.directionId = 0;
  // Deliberately empty -- exercises the "No upcoming departures" branch.
  board.push_back(routeExpress);

  return board;
}

BoardStatus makeSampleStatus() {
  BoardStatus status;
  status.stopName = "Main St & 5th Ave";
  status.batteryPercent = 62;
  status.wifiOk = true;
  status.lastFetchFailed = false;
  status.lastUpdatedEpoch = kNow;
  return status;
}

// Ensures the snapshot output directory exists (mkdir -p, one level at a
// time since there's no guarantee any parent already exists) and returns
// the path `name` should be written under.
//
// Defaults to .pio/test-output/render_snapshot/, which is gitignored --
// an ordinary `pio test` run must never dirty the working tree. Set
// SNAPSHOT_OUT_DIR to send them somewhere tracked instead; that's how
// tools/refresh_screenshots.sh regenerates the PNGs committed under
// docs/screenshots/ and referenced from the README.
std::string snapshotPath(const std::string& name) {
  const char* override = ::getenv("SNAPSHOT_OUT_DIR");
  if (override != nullptr && override[0] != '\0') {
    std::string dir = override;
    // mkdir -p over the override path, so a nested directory that doesn't
    // exist yet is created rather than silently swallowing every write.
    for (size_t i = 1; i <= dir.size(); ++i) {
      if (i == dir.size() || dir[i] == '/') ::mkdir(dir.substr(0, i).c_str(), 0755);
    }
    if (!dir.empty() && dir.back() != '/') dir += '/';
    return dir + name;
  }
  ::mkdir(".pio", 0755);
  ::mkdir(".pio/test-output", 0755);
  ::mkdir(".pio/test-output/render_snapshot", 0755);
  return ".pio/test-output/render_snapshot/" + name;
}

}  // namespace

void test_departure_board_snapshot_paints_a_nontrivial_frame() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

  engine.renderDepartureBoard(makeSampleBoard(), makeSampleStatus());

  TEST_ASSERT_EQUAL_INT(1, presenter.presentCount);
  TEST_ASSERT_TRUE_MESSAGE(target.hasVisibleContent(/*minDistinctSamples=*/2),
                           "departure board should paint more than one gray level");
  // Every icon lookup failed (FakeHttpTransport), so drawRouteBadge()'s
  // text-badge fallback ran for all three routes -- confirm IconCache was
  // actually asked, not silently skipped.
  TEST_ASSERT_GREATER_THAN_INT(0, transport.callCount);

  const std::string path = snapshotPath("departure_board.png");
  TEST_ASSERT_TRUE_MESSAGE(target.writePng(path), "failed to write departure_board.png");
}

// ConfigStore::displayPortrait(): main.cpp swaps to portrait by constructing
// the real DisplayTarget with Orientation::Portrait (whose logicalWidth()/
// logicalHeight() come back swapped, 480x800 instead of 800x480) and calling
// RenderEngine::setScreenSize() with those swapped dimensions. This exercises
// the same swap against HostRasterTarget to confirm the layout (which reads
// only screenWidth_/screenHeight_, never a hardcoded 800/480) actually
// reflows into a taller-than-wide frame instead of clipping/overflowing, and
// dumps a real portrait PNG to look at.
void test_departure_board_renders_correctly_in_portrait() {
  constexpr int16_t kPortraitWidth = 480;
  constexpr int16_t kPortraitHeight = 800;
  transit_test::HostRasterTarget target(kPortraitWidth, kPortraitHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kPortraitWidth, kPortraitHeight);

  engine.renderDepartureBoard(makeSampleBoard(), makeSampleStatus());

  TEST_ASSERT_EQUAL_INT(1, presenter.presentCount);
  TEST_ASSERT_TRUE_MESSAGE(target.hasVisibleContent(/*minDistinctSamples=*/2),
                           "portrait departure board should paint more than one gray level");

  const std::vector<uint8_t>& pixels = target.pixels();
  const int16_t w = target.width();
  const int16_t h = target.height();
  TEST_ASSERT_EQUAL_INT(kPortraitWidth, w);
  TEST_ASSERT_EQUAL_INT(kPortraitHeight, h);

  // Nothing should paint past the frame's actual width/height -- a real bug
  // this test would catch is layout math that still assumes screenWidth_ is
  // always >= screenHeight_ (e.g. a badge rect computed from the wrong axis)
  // and ends up writing out of bounds. HostRasterTarget's own buffer is
  // exactly w*h, so an out-of-range plot would already have been caught by a
  // crash/ASan failure before this point; this just double-checks the
  // reported dimensions match what was requested.
  TEST_ASSERT_EQUAL_UINT32(static_cast<size_t>(w) * static_cast<size_t>(h), pixels.size());

  // Same footer-visibility/no-overlap invariant as the landscape test below,
  // re-checked here because the footer's own width (screenWidth_ - 2*kMargin)
  // is much narrower in portrait -- confirms the badge still fits and isn't
  // clipped by the narrower frame.
  auto rowHasInk = [&](int16_t y) {
    for (int16_t x = 0; x < w; ++x) {
      if (pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] != 255) return true;
    }
    return false;
  };
  int16_t footerInkTop = -1;
  for (int16_t y = static_cast<int16_t>(h - 1); y >= 0; --y) {
    if (rowHasInk(y)) {
      footerInkTop = y;
    } else if (footerInkTop >= 0) {
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(footerInkTop >= 0, "expected the footer badge near the bottom edge in portrait too");

  const std::string path = snapshotPath("departure_board_portrait.png");
  TEST_ASSERT_TRUE_MESSAGE(target.writePng(path), "failed to write departure_board_portrait.png");
}

// Transit API ToS compliance (docs/DEPLOYMENT_OPS.md): renderDepartureBoard()
// must always show a "Powered by Transit" attribution, small/unobtrusive but
// genuinely visible -- not overlapping the departure rows above it, whatever
// height those rows actually stretched to for this particular board. Checks
// this directly against the rasterized pixels rather than just eyeballing
// the PNG: actual ink in the footer band, and a blank separating gap
// immediately above it.
void test_departure_board_footer_is_visible_and_does_not_overlap_rows() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

  engine.renderDepartureBoard(makeSampleBoard(), makeSampleStatus());

  const std::vector<uint8_t>& pixels = target.pixels();
  const int16_t w = target.width();
  const int16_t h = target.height();

  auto rowHasInk = [&](int16_t y) {
    for (int16_t x = 0; x < w; ++x) {
      if (pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] != 255) return true;
    }
    return false;
  };

  // Find the footer badge's topmost ink row by scanning up from the bottom
  // edge, rather than assuming a fixed y: renderDepartureBoard() now
  // stretches row height to fill the body area (fewer routes -> taller
  // rows), so the exact y where rows end shifts with the sample board's
  // route count -- a hardcoded scanline would only coincidentally still
  // land in a gap, not actually prove one exists.
  int16_t footerInkTop = -1;
  for (int16_t y = static_cast<int16_t>(h - 1); y >= 0; --y) {
    if (rowHasInk(y)) {
      footerInkTop = y;
    } else if (footerInkTop >= 0) {
      break;  // left the footer's contiguous ink block
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(footerInkTop >= 0, "expected \"Powered by Transit\" footer badge near the bottom edge");

  // Directly above the footer's ink, there must be a real blank separating
  // row before hitting departure-row content -- renderDepartureBoard()
  // always reserves kMargin (16px) between the row body and the footer
  // rect regardless of row count, so this should show up within a modest
  // search window even though the exact row layout is no longer fixed.
  bool gapFound = false;
  for (int16_t y = static_cast<int16_t>(footerInkTop - 1); y >= 0 && y > footerInkTop - 25; --y) {
    if (!rowHasInk(y)) {
      gapFound = true;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(gapFound, "expected a blank gap between the departure rows and the footer");
}

void test_departure_board_empty_shows_placeholder_text() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

  engine.renderDepartureBoard({}, makeSampleStatus());

  // Still non-trivial: status header + "No departures to show." message.
  TEST_ASSERT_TRUE(target.hasVisibleContent(2));
}

// Regression check for BoardStatus::presetTrips' additive layout change
// (render_engine.h): an empty presetTrips (the default, same as
// makeSampleStatus() below) must produce the exact same frame as before
// this field existed. All the pre-existing snapshot tests above already
// call renderDepartureBoard() with the default empty presetTrips and still
// pass unmodified, which is itself that regression check -- this test adds
// an explicit pixel-identical comparison between two such renders (one
// through a BoardStatus built the old way, one through a BoardStatus that
// explicitly clears presetTrips) so the invariant is asserted directly
// rather than only implied by other tests happening to still pass.
void test_empty_preset_trips_matches_baseline_layout() {
  transit_test::HostRasterTarget targetA(kScreenWidth, kScreenHeight);
  NoopPresenter presenterA;
  FakeHttpTransport transportA;
  IconCache iconCacheA(transportA);
  RenderEngine engineA(targetA, presenterA, iconCacheA, kScreenWidth, kScreenHeight);
  engineA.renderDepartureBoard(makeSampleBoard(), makeSampleStatus());

  transit_test::HostRasterTarget targetB(kScreenWidth, kScreenHeight);
  NoopPresenter presenterB;
  FakeHttpTransport transportB;
  IconCache iconCacheB(transportB);
  RenderEngine engineB(targetB, presenterB, iconCacheB, kScreenWidth, kScreenHeight);
  BoardStatus statusB = makeSampleStatus();
  statusB.presetTrips.clear();  // explicit, though already empty by default
  engineB.renderDepartureBoard(makeSampleBoard(), statusB);

  TEST_ASSERT_TRUE_MESSAGE(targetA.pixels() == targetB.pixels(),
                           "an empty presetTrips should never change the rendered frame");
}

// BoardStatus::presetTrips (render_engine.h): the strip should paint visible
// content above the footer without covering it, and a preset configured
// this cycle should add strictly more ink to the frame than the same board
// without any presets configured.
void test_departure_board_with_preset_trips_shows_strip_above_footer() {
  transit_test::HostRasterTarget baseline(kScreenWidth, kScreenHeight);
  {
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(baseline, presenter, iconCache, kScreenWidth, kScreenHeight);
    engine.renderDepartureBoard(makeSampleBoard(), makeSampleStatus());
  }

  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

  // ASCII arrows only -- the bundled Noto Sans subset has no glyph for
  // U+2192 (RIGHTWARDS ARROW) and renders it as a tofu box (confirmed
  // visually against a snapshot PNG while writing this test); real preset-
  // summary formatting (main.cpp, not yet wired) must avoid it too.
  BoardStatus status = makeSampleStatus();
  // One urgent line and one fallback line, so this single frame covers both
  // the leaveNow stroke treatment and the not-found message. The urgency
  // belongs on the line with a real plan behind it: main.cpp's
  // formatPresetSummaryLine() forces leaveNow=false whenever !plan.found,
  // so an outlined "No upcoming trip found" is a combination the firmware
  // never actually produces -- and this frame is committed to
  // docs/screenshots/, so it has to show what the board really does.
  status.presetTrips = {
      {"Home", "leave by 5:42p - 31 -> transfer ~5:58p -> 32", /*leaveNow=*/true},
      {"Work", "No upcoming trip found", /*leaveNow=*/false},
  };
  engine.renderDepartureBoard(makeSampleBoard(), status);

  TEST_ASSERT_EQUAL_INT(1, presenter.presentCount);
  TEST_ASSERT_TRUE(target.hasVisibleContent(2));

  const std::vector<uint8_t>& pixels = target.pixels();
  const std::vector<uint8_t>& basePixels = baseline.pixels();
  size_t inkCount = 0;
  size_t baseInkCount = 0;
  for (uint8_t p : pixels) {
    if (p != 255) ++inkCount;
  }
  for (uint8_t p : basePixels) {
    if (p != 255) ++baseInkCount;
  }
  TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(baseInkCount, inkCount,
                                          "adding preset trip lines should add visible ink to the frame");

  // Footer badge is still present and near the bottom edge, same invariant
  // as test_departure_board_footer_is_visible_and_does_not_overlap_rows.
  const int16_t w = target.width();
  const int16_t h = target.height();
  auto rowHasInk = [&](int16_t y) {
    for (int16_t x = 0; x < w; ++x) {
      if (pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] != 255) return true;
    }
    return false;
  };
  int16_t footerInkTop = -1;
  for (int16_t y = static_cast<int16_t>(h - 1); y >= 0; --y) {
    if (rowHasInk(y)) {
      footerInkTop = y;
    } else if (footerInkTop >= 0) {
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(footerInkTop >= 0, "expected the footer badge near the bottom edge with presets shown too");

  const std::string path = snapshotPath("departure_board_with_presets.png");
  TEST_ASSERT_TRUE_MESSAGE(target.writePng(path), "failed to write departure_board_with_presets.png");
}

// setFocusMode(true) (render_engine.h): still paints a nontrivial frame and
// still presents exactly once, using the sample board's mix (including the
// one direction with no upcoming departures, which selectFocusBoards()
// must skip rather than crash on -- see render_engine.cpp's comment).
// BoardStatus::secondSourceAttributions (render_engine.h): empty (the
// default -- STA's own case, since its terms don't require this) must
// never change the rendered frame, same convention/test shape as
// test_empty_preset_trips_matches_baseline_layout above.
void test_empty_second_source_attributions_matches_baseline_layout() {
  transit_test::HostRasterTarget targetA(kScreenWidth, kScreenHeight);
  NoopPresenter presenterA;
  FakeHttpTransport transportA;
  IconCache iconCacheA(transportA);
  RenderEngine engineA(targetA, presenterA, iconCacheA, kScreenWidth, kScreenHeight);
  engineA.renderDepartureBoard(makeSampleBoard(), makeSampleStatus());

  transit_test::HostRasterTarget targetB(kScreenWidth, kScreenHeight);
  NoopPresenter presenterB;
  FakeHttpTransport transportB;
  IconCache iconCacheB(transportB);
  RenderEngine engineB(targetB, presenterB, iconCacheB, kScreenWidth, kScreenHeight);
  BoardStatus statusB = makeSampleStatus();
  statusB.secondSourceAttributions.clear();  // explicit, though already empty by default
  engineB.renderDepartureBoard(makeSampleBoard(), statusB);

  TEST_ASSERT_TRUE_MESSAGE(targetA.pixels() == targetB.pixels(),
                           "empty secondSourceAttributions should never change the rendered frame");
}

// A non-empty entry should add a visible credit line above the Transit
// badge, and multiple entries should join into one line (more ink than a
// single entry alone) rather than each getting their own line -- see
// joinAttributions()'s comment on why the footer grows by a fixed amount
// regardless of how many agencies are active.
void test_second_source_attribution_adds_visible_line_above_badge() {
  transit_test::HostRasterTarget baseline(kScreenWidth, kScreenHeight);
  {
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(baseline, presenter, iconCache, kScreenWidth, kScreenHeight);
    engine.renderDepartureBoard(makeSampleBoard(), makeSampleStatus());
  }

  transit_test::HostRasterTarget singleTarget(kScreenWidth, kScreenHeight);
  {
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(singleTarget, presenter, iconCache, kScreenWidth, kScreenHeight);
    BoardStatus status = makeSampleStatus();
    status.secondSourceAttributions = {"Data provided by Example Transit"};
    engine.renderDepartureBoard(makeSampleBoard(), status);
  }

  transit_test::HostRasterTarget doubleTarget(kScreenWidth, kScreenHeight);
  NoopPresenter doublePresenter;
  FakeHttpTransport doubleTransport;
  IconCache doubleIconCache(doubleTransport);
  RenderEngine doubleEngine(doubleTarget, doublePresenter, doubleIconCache, kScreenWidth, kScreenHeight);
  BoardStatus doubleStatus = makeSampleStatus();
  doubleStatus.secondSourceAttributions = {"Data provided by Example Transit", "Another Agency"};
  doubleEngine.renderDepartureBoard(makeSampleBoard(), doubleStatus);

  auto inkCount = [](const std::vector<uint8_t>& pixels) {
    size_t count = 0;
    for (uint8_t p : pixels) {
      if (p != 255) ++count;
    }
    return count;
  };

  const size_t baseInk = inkCount(baseline.pixels());
  const size_t singleInk = inkCount(singleTarget.pixels());
  const size_t doubleInk = inkCount(doubleTarget.pixels());

  TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(baseInk, singleInk,
                                          "a second-source attribution should add visible ink");
  TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(singleInk, doubleInk,
                                          "joining a second attribution onto the line should add more ink still");

  // Still exactly one present() and the Transit badge itself is untouched
  // -- this adds a line, it doesn't replace or duplicate the badge.
  TEST_ASSERT_EQUAL_INT(1, doublePresenter.presentCount);

  const std::string path = snapshotPath("departure_board_with_attribution.png");
  TEST_ASSERT_TRUE_MESSAGE(singleTarget.writePng(path),
                           "failed to write departure_board_with_attribution.png");
}

void test_focus_mode_paints_a_nontrivial_frame() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);
  engine.setFocusMode(true);

  engine.renderDepartureBoard(makeSampleBoard(), makeSampleStatus());

  TEST_ASSERT_EQUAL_INT(1, presenter.presentCount);
  TEST_ASSERT_TRUE_MESSAGE(target.hasVisibleContent(/*minDistinctSamples=*/2),
                           "focus mode should still paint a visible frame");

  const std::string path = snapshotPath("departure_board_focus_mode.png");
  TEST_ASSERT_TRUE_MESSAGE(target.writePng(path), "failed to write departure_board_focus_mode.png");
}

// isLeaveNowUrgent()'s bold+stroke treatment (render_engine.cpp): a board
// whose only departure is imminent (<=5min) should render with strictly
// more ink than the same board with that departure far in the future, all
// else equal -- the stroke outline and bold glyphs both add pixels.
void test_leave_now_urgency_adds_visible_emphasis() {
  auto makeSingleRouteBoard = [](int64_t departureEpoch) {
    std::vector<DirectionBoard> board;
    DirectionBoard route;
    route.globalRouteId = "R1";
    route.routeShortName = "1";
    route.routeDisplayShortName = makeDisplayShortName("bus-1", "1");
    route.routeColor = "111111";
    route.routeTextColor = "FFFFFF";
    route.departures = {makeDeparture("Downtown", departureEpoch)};
    board.push_back(route);
    return board;
  };

  transit_test::HostRasterTarget urgentTarget(kScreenWidth, kScreenHeight);
  {
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(urgentTarget, presenter, iconCache, kScreenWidth, kScreenHeight);
    engine.renderDepartureBoard(makeSingleRouteBoard(kNow + 2 * 60), makeSampleStatus());
  }

  transit_test::HostRasterTarget farTarget(kScreenWidth, kScreenHeight);
  {
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(farTarget, presenter, iconCache, kScreenWidth, kScreenHeight);
    engine.renderDepartureBoard(makeSingleRouteBoard(kNow + 45 * 60), makeSampleStatus());
  }

  size_t urgentInk = 0;
  size_t farInk = 0;
  for (uint8_t p : urgentTarget.pixels()) {
    if (p != 255) ++urgentInk;
  }
  for (uint8_t p : farTarget.pixels()) {
    if (p != 255) ++farInk;
  }
  TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(farInk, urgentInk,
                                          "an imminent departure's bold+stroke chip should add visible ink");
}

// --- Offline / cached-data treatment (offline_cache.h, time_keeper.h) -----

// A board restored from the NVS cache must look like a normal board with a
// staleness marker, not like a broken one: same rows, same chips, but the
// header says "Offline" and "Cached 2h ago" instead of "Updated 14:32".
void test_offline_cached_board_shows_a_stale_marker() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

  BoardStatus status = makeSampleStatus();
  status.wifiOk = false;
  status.lastFetchFailed = true;
  status.source = BoardStatus::DepartureSource::kCached;
  status.cachedAgeMin = 137;  // "Cached 2h ago"
  status.clockIsApproximate = true;

  engine.renderDepartureBoard(makeSampleBoard(), status);

  TEST_ASSERT_EQUAL_INT(1, presenter.presentCount);
  TEST_ASSERT_TRUE_MESSAGE(target.hasVisibleContent(2),
                           "a cached board should still paint the full multi-tone layout");

  const std::string path = snapshotPath("departure_board_offline_cached.png");
  TEST_ASSERT_TRUE_MESSAGE(target.writePng(path), "failed to write departure_board_offline_cached.png");
}

// The staleness marker has to actually reach the pixels. Rendering the same
// board twice -- once fresh, once cached -- must produce different frames,
// or the header treatment is only happening in the struct.
void test_cached_header_differs_from_a_live_one() {
  auto render = [](bool cached) {
    transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);
    BoardStatus status = makeSampleStatus();
    if (cached) {
      status.wifiOk = false;
      status.lastFetchFailed = true;
      status.source = BoardStatus::DepartureSource::kCached;
      status.cachedAgeMin = 137;
    }
    engine.renderDepartureBoard(makeSampleBoard(), status);
    return target.pixels();
  };

  TEST_ASSERT_TRUE_MESSAGE(render(false) != render(true),
                           "a cached board must be visually distinguishable from a live one");
}

// An approximate clock (time_keeper.h) is marked with a leading "~" so an
// estimate is never shown as if it were the exact time.
void test_approximate_clock_is_marked_in_the_header() {
  auto render = [](bool approximate) {
    transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);
    BoardStatus status = makeSampleStatus();
    status.clockIsApproximate = approximate;
    engine.renderDepartureBoard(makeSampleBoard(), status);
    return target.pixels();
  };

  TEST_ASSERT_TRUE_MESSAGE(render(false) != render(true),
                           "an approximate clock must be marked distinctly from a synced one");
}

// Three genuinely different situations, three different messages. An empty
// board because the stop is quiet is not the same as an empty board because
// the cache aged out, which is not the same as having never had a cache.
void test_empty_board_messages_distinguish_the_offline_cases() {
  auto render = [](bool wifiOk, bool cached) {
    transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);
    BoardStatus status = makeSampleStatus();
    status.wifiOk = wifiOk;
    status.lastFetchFailed = !wifiOk;
    status.source = cached ? BoardStatus::DepartureSource::kCached
                          : BoardStatus::DepartureSource::kLive;
    engine.renderDepartureBoard({}, status);
    return target.pixels();
  };

  const auto online = render(true, false);
  const auto offlineNoCache = render(false, false);
  const auto offlineExpiredCache = render(false, true);

  TEST_ASSERT_TRUE(online != offlineNoCache);
  TEST_ASSERT_TRUE(offlineNoCache != offlineExpiredCache);
}

// Offline long enough that the approximate clock aged out: the cache is
// still worth drawing, but its age is genuinely unknown and must not be
// rounded down to "just now" (BoardStatus::cachedAgeMin's -1 convention).
void test_cached_board_with_unknown_age_is_marked_unknown_not_fresh() {
  auto render = [](int cachedAgeMin) {
    transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);
    BoardStatus status = makeSampleStatus();
    status.wifiOk = false;
    status.lastFetchFailed = true;
    status.source = BoardStatus::DepartureSource::kCached;
    status.cachedAgeMin = cachedAgeMin;
    engine.renderDepartureBoard(makeSampleBoard(), status);
    return target.pixels();
  };

  TEST_ASSERT_TRUE_MESSAGE(render(-1) != render(0),
                           "an unknown cache age must not render the same as a fresh one");
}

// Wi-Fi is up and only the API call failed (bad key, quota, a 5xx). That's
// a different problem from being offline and must keep saying so, or the
// header throws away the one diagnostic it can give.
void test_fetch_failure_with_wifi_up_is_not_labelled_offline() {
  auto render = [](bool wifiOk) {
    transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);
    BoardStatus status = makeSampleStatus();
    status.wifiOk = wifiOk;
    status.lastFetchFailed = true;
    status.source = BoardStatus::DepartureSource::kCached;
    status.cachedAgeMin = 137;
    engine.renderDepartureBoard(makeSampleBoard(), status);
    return target.pixels();
  };

  TEST_ASSERT_TRUE_MESSAGE(render(true) != render(false),
                           "a fetch failure with Wi-Fi up must read differently from being offline");
}

// --- Scheduled times beside real-time, and the timetable source ---------

// A real-time chip carries its scheduled time next to it, so a late bus
// reads as late. This is the frame committed to docs/screenshots/.
void test_realtime_chips_show_the_scheduled_time_for_comparison() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

  std::vector<DirectionBoard> board;
  DirectionBoard late;
  late.globalRouteId = "1:31";
  late.routeShortName = "31";
  late.routeDisplayShortName = makeDisplayShortName("bus-31", "31");
  late.routeColor = "111111";
  late.routeTextColor = "FFFFFF";
  {
    // Running 4 minutes behind: due in 9, timetabled for 5 minutes ago.
    DepartureRow row = makeDeparture("Downtown", kNow + 9 * 60, /*realTime=*/true);
    row.scheduledDepartureTimeEpoch = kNow + 5 * 60;
    late.departures.push_back(row);
    // On time: the delta is omitted, only the scheduled clock shown.
    DepartureRow onTime = makeDeparture("Downtown", kNow + 26 * 60, /*realTime=*/true);
    onTime.scheduledDepartureTimeEpoch = kNow + 26 * 60;
    late.departures.push_back(onTime);
  }
  board.push_back(late);

  DirectionBoard early;
  early.globalRouteId = "1:32";
  early.routeShortName = "32";
  early.routeDisplayShortName = makeDisplayShortName("bus-32", "32");
  early.routeColor = "888888";
  early.routeTextColor = "000000";
  {
    // Running 2 minutes early -- the case worth flagging hardest, since
    // arriving "on time" means missing it.
    DepartureRow row = makeDeparture("Shadle", kNow + 14 * 60, /*realTime=*/true);
    row.scheduledDepartureTimeEpoch = kNow + 16 * 60;
    early.departures.push_back(row);
    // No real-time data at all: the time shown IS the schedule, so no
    // annotation.
    early.departures.push_back(makeDeparture("Shadle", kNow + 44 * 60));
  }
  board.push_back(early);

  engine.renderDepartureBoard(board, makeSampleStatus());

  TEST_ASSERT_EQUAL_INT(1, presenter.presentCount);
  TEST_ASSERT_TRUE(target.hasVisibleContent(2));

  const std::string path = snapshotPath("departure_board_scheduled_vs_realtime.png");
  TEST_ASSERT_TRUE_MESSAGE(target.writePng(path),
                           "failed to write departure_board_scheduled_vs_realtime.png");
}

// The annotation must actually reach the pixels, and a delay must look
// different from an on-time arrival.
void test_a_delayed_chip_renders_differently_from_an_on_time_one() {
  auto render = [](int64_t scheduledOffsetMin) {
    transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

    std::vector<DirectionBoard> board;
    DirectionBoard route;
    route.globalRouteId = "1:31";
    route.routeShortName = "31";
    route.routeDisplayShortName = makeDisplayShortName("bus-31", "31");
    route.routeColor = "111111";
    route.routeTextColor = "FFFFFF";
    DepartureRow row = makeDeparture("Downtown", kNow + 20 * 60, /*realTime=*/true);
    row.scheduledDepartureTimeEpoch = kNow + scheduledOffsetMin * 60;
    route.departures.push_back(row);
    board.push_back(route);

    engine.renderDepartureBoard(board, makeSampleStatus());
    return target.pixels();
  };

  TEST_ASSERT_TRUE_MESSAGE(render(20) != render(14),
                           "a bus running 6 minutes late must not render identically to an on-time one");
  TEST_ASSERT_TRUE_MESSAGE(render(20) != render(23),
                           "a bus running early must not render identically to an on-time one");
}

// A chip with no scheduled time at all (the source didn't supply one) is
// left exactly as it was before this feature existed.
void test_a_chip_without_a_scheduled_time_is_unannotated() {
  auto render = [](bool withScheduled) {
    transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

    std::vector<DirectionBoard> board;
    DirectionBoard route;
    route.globalRouteId = "1:31";
    route.routeShortName = "31";
    route.routeDisplayShortName = makeDisplayShortName("bus-31", "31");
    route.routeColor = "111111";
    route.routeTextColor = "FFFFFF";
    DepartureRow row = makeDeparture("Downtown", kNow + 20 * 60, /*realTime=*/true);
    if (withScheduled) row.scheduledDepartureTimeEpoch = kNow + 20 * 60;
    route.departures.push_back(row);
    board.push_back(route);

    engine.renderDepartureBoard(board, makeSampleStatus());
    return target.pixels();
  };

  TEST_ASSERT_TRUE_MESSAGE(render(false) != render(true),
                           "supplying a scheduled time should visibly annotate the chip");
}

// The static-timetable source is its own state, distinct from both live
// and cached -- it is never stale the way a cache is, and never
// authoritative the way live data is.
void test_scheduled_source_is_marked_distinctly_from_live_and_cached() {
  auto render = [](BoardStatus::DepartureSource source, bool expired) {
    transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
    NoopPresenter presenter;
    FakeHttpTransport transport;
    IconCache iconCache(transport);
    RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);
    BoardStatus status = makeSampleStatus();
    status.wifiOk = false;
    status.lastFetchFailed = true;
    status.source = source;
    status.cachedAgeMin = 137;
    status.scheduleExpired = expired;
    status.scheduleValidUntil = 20260919;
    engine.renderDepartureBoard(makeSampleBoard(), status);
    return target.pixels();
  };

  const auto live = render(BoardStatus::DepartureSource::kLive, false);
  const auto cached = render(BoardStatus::DepartureSource::kCached, false);
  const auto scheduled = render(BoardStatus::DepartureSource::kScheduled, false);
  const auto expired = render(BoardStatus::DepartureSource::kScheduled, true);

  TEST_ASSERT_TRUE(live != cached);
  TEST_ASSERT_TRUE(cached != scheduled);
  TEST_ASSERT_TRUE(live != scheduled);
  TEST_ASSERT_TRUE_MESSAGE(scheduled != expired,
                           "an expired timetable must be called out, not shown as a normal one");
}

void test_scheduled_board_snapshot() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

  BoardStatus status = makeSampleStatus();
  status.wifiOk = false;
  status.lastFetchFailed = true;
  status.source = BoardStatus::DepartureSource::kScheduled;
  engine.renderDepartureBoard(makeSampleBoard(), status);

  TEST_ASSERT_TRUE(target.hasVisibleContent(2));
  const std::string path = snapshotPath("departure_board_offline_timetable.png");
  TEST_ASSERT_TRUE_MESSAGE(target.writePng(path), "failed to write departure_board_offline_timetable.png");
}

// --- Remaining states, captured for the documentation site --------------
//
// These assert the frame paints and then write a PNG; the behavioural
// assertions for each state live in the tests above. Their value is that
// docs/screenshots/ and the product site show every state the board can
// actually reach, generated from the real renderer rather than mocked up.

void test_snapshot_leave_now_urgency() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

  std::vector<DirectionBoard> board;
  DirectionBoard imminent;
  imminent.globalRouteId = "1:31";
  imminent.routeShortName = "31";
  imminent.routeDisplayShortName = makeDisplayShortName("bus-31", "31");
  imminent.routeColor = "111111";
  imminent.routeTextColor = "FFFFFF";
  imminent.departures = {makeDeparture("Downtown", kNow + 2 * 60, /*realTime=*/true),
                         makeDeparture("Downtown", kNow + 22 * 60)};
  board.push_back(imminent);

  DirectionBoard later;
  later.globalRouteId = "1:32";
  later.routeShortName = "32";
  later.routeDisplayShortName = makeDisplayShortName("bus-32", "32");
  later.routeColor = "888888";
  later.routeTextColor = "000000";
  later.departures = {makeDeparture("Shadle", kNow + 17 * 60), makeDeparture("Shadle", kNow + 47 * 60)};
  board.push_back(later);

  BoardStatus status = makeSampleStatus();
  status.presetTrips = {{"Home", "leave by 5:42p - 31 -> transfer ~5:58p -> 32", /*leaveNow=*/true}};
  engine.renderDepartureBoard(board, status);

  TEST_ASSERT_TRUE(target.hasVisibleContent(2));
  TEST_ASSERT_TRUE(target.writePng(snapshotPath("departure_board_leave_now.png")));
}

void test_snapshot_empty_board_online() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);
  engine.renderDepartureBoard({}, makeSampleStatus());
  TEST_ASSERT_TRUE(target.writePng(snapshotPath("departure_board_empty.png")));
}

void test_snapshot_offline_with_no_cache() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);
  BoardStatus status = makeSampleStatus();
  status.wifiOk = false;
  status.lastFetchFailed = true;
  status.lastUpdatedEpoch = 0;
  engine.renderDepartureBoard({}, status);
  TEST_ASSERT_TRUE(target.writePng(snapshotPath("departure_board_offline_nocache.png")));
}

void test_snapshot_expired_timetable() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);
  BoardStatus status = makeSampleStatus();
  status.wifiOk = false;
  status.lastFetchFailed = true;
  status.source = BoardStatus::DepartureSource::kScheduled;
  status.scheduleExpired = true;
  status.scheduleValidUntil = 20260919;
  engine.renderDepartureBoard({}, status);
  TEST_ASSERT_TRUE(target.writePng(snapshotPath("departure_board_timetable_expired.png")));
}

void test_snapshot_cached_age_unknown() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);
  BoardStatus status = makeSampleStatus();
  status.wifiOk = false;
  status.lastFetchFailed = true;
  status.source = BoardStatus::DepartureSource::kCached;
  status.cachedAgeMin = -1;
  status.lastUpdatedEpoch = 0;
  engine.renderDepartureBoard(makeSampleBoard(), status);
  TEST_ASSERT_TRUE(target.writePng(snapshotPath("departure_board_cached_unknown_age.png")));
}

void test_setup_prompt_snapshot_paints_a_nontrivial_frame() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

  engine.renderSetupPrompt("Connect to Wi-Fi",
                           "Join \"TransitBoard-Setup\" from your phone or laptop, then open "
                           "the page that pops up to finish setup.");

  TEST_ASSERT_EQUAL_INT(1, presenter.presentCount);
  TEST_ASSERT_TRUE(target.hasVisibleContent(2));

  const std::string path = snapshotPath("setup_prompt.png");
  TEST_ASSERT_TRUE_MESSAGE(target.writePng(path), "failed to write setup_prompt.png");
}

void test_setup_list_snapshot_paints_a_nontrivial_frame() {
  transit_test::HostRasterTarget target(kScreenWidth, kScreenHeight);
  NoopPresenter presenter;
  FakeHttpTransport transport;
  IconCache iconCache(transport);
  RenderEngine engine(target, presenter, iconCache, kScreenWidth, kScreenHeight);

  const std::vector<std::string> items = {
      "Main St & 5th Ave (0.1 mi)", "Main St & 6th Ave (0.3 mi)", "Downtown Transit Center (0.6 mi)",
  };
  engine.renderSetupList("Pick a stop", items, /*selectedIndex=*/1);

  TEST_ASSERT_EQUAL_INT(1, presenter.presentCount);
  // The selected row fills black with white text -- guarantees black,
  // white, *and* the unselected rows' plain text all appear together.
  TEST_ASSERT_TRUE(target.hasVisibleContent(2));

  const std::string path = snapshotPath("setup_list.png");
  TEST_ASSERT_TRUE_MESSAGE(target.writePng(path), "failed to write setup_list.png");
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_departure_board_snapshot_paints_a_nontrivial_frame);
  RUN_TEST(test_departure_board_renders_correctly_in_portrait);
  RUN_TEST(test_departure_board_footer_is_visible_and_does_not_overlap_rows);
  RUN_TEST(test_departure_board_empty_shows_placeholder_text);
  RUN_TEST(test_empty_preset_trips_matches_baseline_layout);
  RUN_TEST(test_departure_board_with_preset_trips_shows_strip_above_footer);
  RUN_TEST(test_empty_second_source_attributions_matches_baseline_layout);
  RUN_TEST(test_second_source_attribution_adds_visible_line_above_badge);
  RUN_TEST(test_focus_mode_paints_a_nontrivial_frame);
  RUN_TEST(test_leave_now_urgency_adds_visible_emphasis);
  RUN_TEST(test_offline_cached_board_shows_a_stale_marker);
  RUN_TEST(test_cached_header_differs_from_a_live_one);
  RUN_TEST(test_approximate_clock_is_marked_in_the_header);
  RUN_TEST(test_empty_board_messages_distinguish_the_offline_cases);
  RUN_TEST(test_cached_board_with_unknown_age_is_marked_unknown_not_fresh);
  RUN_TEST(test_fetch_failure_with_wifi_up_is_not_labelled_offline);
  RUN_TEST(test_realtime_chips_show_the_scheduled_time_for_comparison);
  RUN_TEST(test_a_delayed_chip_renders_differently_from_an_on_time_one);
  RUN_TEST(test_a_chip_without_a_scheduled_time_is_unannotated);
  RUN_TEST(test_scheduled_source_is_marked_distinctly_from_live_and_cached);
  RUN_TEST(test_scheduled_board_snapshot);
  RUN_TEST(test_snapshot_leave_now_urgency);
  RUN_TEST(test_snapshot_empty_board_online);
  RUN_TEST(test_snapshot_offline_with_no_cache);
  RUN_TEST(test_snapshot_expired_timetable);
  RUN_TEST(test_snapshot_cached_age_unknown);
  RUN_TEST(test_setup_prompt_snapshot_paints_a_nontrivial_frame);
  RUN_TEST(test_setup_list_snapshot_paints_a_nontrivial_frame);
  return UNITY_END();
}
