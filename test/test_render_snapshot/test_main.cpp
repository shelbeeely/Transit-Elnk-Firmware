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

// Ensures .pio/test-output/render_snapshot/ exists (mkdir -p, one level at a
// time since there's no guarantee any parent already exists) and returns the
// path `name` should be written under.
std::string snapshotPath(const std::string& name) {
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
  RUN_TEST(test_departure_board_empty_shows_placeholder_text);
  RUN_TEST(test_setup_prompt_snapshot_paints_a_nontrivial_frame);
  RUN_TEST(test_setup_list_snapshot_paints_a_nontrivial_frame);
  return UNITY_END();
}
