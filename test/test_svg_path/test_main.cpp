// Host-side tests for the SVG-subset parser (work unit 6, icon_cache).
// Pure logic, no network/bitmap code involved — see include/transit/svg_path.h.

#include <unity.h>

#include <cmath>

#include "transit/svg_path.h"

using transit::parseSvgIcon;
using transit::SvgIcon;

namespace {

// A real "-mono" icon fetched live from
// https://transitapp-data.com/images/svgx/stm-metro-mono.svg during
// development — small enough to embed verbatim as a fixture.
const char* kStmMetroSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" xml:space=\"preserve\" "
    "viewBox=\"0 0 102 102\"><path d=\"M51 0C22.9 0 0 22.9 0 51s22.9 51 51 51 51-22.9 "
    "51-51S79.1 0 51 0m0 88.6c-20.7 0-37.6-16.9-37.6-37.6 0-17.8 12.4-33.4 29.7-36.9v43.3L24.8 "
    "39 14 49.8l37 37 37-37L77.2 39 59 57.4V14.1C76.3 17.7 88.6 33.2 88.6 51c0 20.7-16.9 "
    "37.6-37.6 37.6\"/></svg>";

// A real icon that carries an explicit fill-rule="evenodd", fetched live
// from https://transitapp-data.com/images/svgx/bus-mono.svg.
const char* kBusSvgSnippet =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"115\" height=\"102\" "
    "viewBox=\"0 0 115 102\"><path fill=\"#010101\" fill-rule=\"evenodd\" "
    "d=\"M10 10h20v20h-20z\"/></svg>";

}  // namespace

void test_parses_real_icon_without_explicit_fill_rule() {
  SvgIcon icon;
  TEST_ASSERT_TRUE(parseSvgIcon(kStmMetroSvg, icon));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, icon.viewBoxMinX);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, icon.viewBoxMinY);
  TEST_ASSERT_EQUAL_FLOAT(102.0f, icon.viewBoxWidth);
  TEST_ASSERT_EQUAL_FLOAT(102.0f, icon.viewBoxHeight);
  // No fill-rule attribute in the source -> SVG default, nonzero.
  TEST_ASSERT_FALSE(icon.evenOddFill);
  TEST_ASSERT_FALSE(icon.subpaths.empty());
  // The path has two subpaths (outer ring, inner "M" cutout) each closed by
  // an explicit 'Z' or the implicit-close-for-fill the parser applies.
  TEST_ASSERT_EQUAL(2, icon.subpaths.size());
}

void test_honors_explicit_evenodd_fill_rule() {
  SvgIcon icon;
  TEST_ASSERT_TRUE(parseSvgIcon(kBusSvgSnippet, icon));
  TEST_ASSERT_TRUE(icon.evenOddFill);
}

void test_simple_rect_path_square() {
  const char* svg =
      "<svg viewBox=\"0 0 10 10\"><path d=\"M1 1H9V9H1Z\"/></svg>";
  SvgIcon icon;
  TEST_ASSERT_TRUE(parseSvgIcon(svg, icon));
  TEST_ASSERT_EQUAL(1, icon.subpaths.size());
  const auto& sub = icon.subpaths[0];
  // M, H, V, H, then the implicit-close point appended by Z.
  TEST_ASSERT_EQUAL(5, sub.size());
  TEST_ASSERT_EQUAL_FLOAT(1.0f, sub[0].x);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, sub[0].y);
  TEST_ASSERT_EQUAL_FLOAT(9.0f, sub[1].x);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, sub[1].y);
  TEST_ASSERT_EQUAL_FLOAT(9.0f, sub[2].x);
  TEST_ASSERT_EQUAL_FLOAT(9.0f, sub[2].y);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, sub[3].x);
  TEST_ASSERT_EQUAL_FLOAT(9.0f, sub[3].y);
  // Closes back to the start.
  TEST_ASSERT_EQUAL_FLOAT(sub[0].x, sub[4].x);
  TEST_ASSERT_EQUAL_FLOAT(sub[0].y, sub[4].y);
}

void test_relative_commands_and_implicit_repeat() {
  // Relative moveto, then a bare pair (implicit lineto), then relative
  // lineto, closed — a rotated-square-ish shape.
  const char* svg = "<svg viewBox=\"0 0 20 20\"><path d=\"m5 5 5 0 l0 5 -5 0z\"/></svg>";
  SvgIcon icon;
  TEST_ASSERT_TRUE(parseSvgIcon(svg, icon));
  TEST_ASSERT_EQUAL(1, icon.subpaths.size());
  const auto& sub = icon.subpaths[0];
  TEST_ASSERT_EQUAL_FLOAT(5.0f, sub[0].x);
  TEST_ASSERT_EQUAL_FLOAT(5.0f, sub[0].y);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, sub[1].x);  // implicit lineto after 'm's first pair
  TEST_ASSERT_EQUAL_FLOAT(5.0f, sub[1].y);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, sub[2].x);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, sub[2].y);
  TEST_ASSERT_EQUAL_FLOAT(5.0f, sub[3].x);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, sub[3].y);
}

void test_cubic_curve_endpoint_lands_correctly() {
  const char* svg = "<svg viewBox=\"0 0 20 20\"><path d=\"M0 0C0 10 10 10 10 0Z\"/></svg>";
  SvgIcon icon;
  TEST_ASSERT_TRUE(parseSvgIcon(svg, icon));
  TEST_ASSERT_EQUAL(1, icon.subpaths.size());
  const auto& sub = icon.subpaths[0];
  // Start point, N flattened curve points (last one is the true endpoint
  // (10,0)), then the close-point back to (0,0).
  TEST_ASSERT_TRUE(sub.size() > 3);
  const auto& lastCurvePoint = sub[sub.size() - 2];
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, lastCurvePoint.x);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, lastCurvePoint.y);
}

void test_missing_viewbox_fails() {
  const char* svg = "<svg><path d=\"M0 0L10 10Z\"/></svg>";
  SvgIcon icon;
  TEST_ASSERT_FALSE(parseSvgIcon(svg, icon));
}

void test_no_shapes_fails() {
  const char* svg = "<svg viewBox=\"0 0 10 10\"></svg>";
  SvgIcon icon;
  TEST_ASSERT_FALSE(parseSvgIcon(svg, icon));
}

void test_unsupported_quadratic_command_fails() {
  const char* svg = "<svg viewBox=\"0 0 10 10\"><path d=\"M0 0Q5 5 10 10Z\"/></svg>";
  SvgIcon icon;
  TEST_ASSERT_FALSE(parseSvgIcon(svg, icon));
}

void test_arc_command_semicircle_endpoint_and_midpoint() {
  // A semicircle of radius 5 from (0,5) to (10,5), sweeping through (5,0):
  // large-arc=0, sweep=1 (per the SVG spec's own worked example shape).
  const char* svg = "<svg viewBox=\"0 0 10 10\"><path d=\"M0 5A5 5 0 0 1 10 5Z\"/></svg>";
  SvgIcon icon;
  TEST_ASSERT_TRUE(parseSvgIcon(svg, icon));
  TEST_ASSERT_EQUAL(1, icon.subpaths.size());
  const auto& sub = icon.subpaths[0];
  TEST_ASSERT_TRUE(sub.size() > 4);
  // Endpoint of the arc should land essentially exactly on (10, 5).
  const auto& arcEnd = sub[sub.size() - 2];  // last point before the Z-close
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 10.0f, arcEnd.x);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 5.0f, arcEnd.y);
  // Somewhere along the arc it should pass near the top of the circle (5,0).
  bool sawTop = false;
  for (const auto& p : sub) {
    if (std::fabs(p.x - 5.0f) < 0.3f && std::fabs(p.y - 0.0f) < 0.3f) sawTop = true;
  }
  TEST_ASSERT_TRUE(sawTop);
}

void test_arc_glued_flags_no_separator_parses_correctly() {
  // Minified real-world SVG glues the two single-digit flags directly onto
  // the coordinate that follows (see readFlag's comment in svg_path.cpp).
  // "0 011.047 2.38" must parse as large-arc=0, sweep=1, x=1.047, y=2.38 —
  // not one glued number "011.047".
  const char* svg = "<svg viewBox=\"0 0 10 10\"><path d=\"M0 0a1.85 1.85 0 011.047 2.38\"/></svg>";
  SvgIcon icon;
  TEST_ASSERT_TRUE(parseSvgIcon(svg, icon));
  TEST_ASSERT_EQUAL(1, icon.subpaths.size());
  const auto& sub = icon.subpaths[0];
  const auto& arcEnd = sub[sub.size() - 1];
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.047f, arcEnd.x);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 2.38f, arcEnd.y);
}

void test_malformed_path_data_fails() {
  const char* svg = "<svg viewBox=\"0 0 10 10\"><path d=\"M0 0 L garbage\"/></svg>";
  SvgIcon icon;
  TEST_ASSERT_FALSE(parseSvgIcon(svg, icon));
}

void test_rect_and_circle_primitives_supported() {
  const char* svg =
      "<svg viewBox=\"0 0 20 20\"><rect x=\"1\" y=\"2\" width=\"3\" height=\"4\"/>"
      "<circle cx=\"10\" cy=\"10\" r=\"5\"/></svg>";
  SvgIcon icon;
  TEST_ASSERT_TRUE(parseSvgIcon(svg, icon));
  TEST_ASSERT_EQUAL(2, icon.subpaths.size());
  TEST_ASSERT_EQUAL(4, icon.subpaths[0].size());   // rect: 4 corners
  TEST_ASSERT_EQUAL(24, icon.subpaths[1].size());  // circle: 24-gon approximation
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_parses_real_icon_without_explicit_fill_rule);
  RUN_TEST(test_honors_explicit_evenodd_fill_rule);
  RUN_TEST(test_simple_rect_path_square);
  RUN_TEST(test_relative_commands_and_implicit_repeat);
  RUN_TEST(test_cubic_curve_endpoint_lands_correctly);
  RUN_TEST(test_missing_viewbox_fails);
  RUN_TEST(test_no_shapes_fails);
  RUN_TEST(test_unsupported_quadratic_command_fails);
  RUN_TEST(test_arc_command_semicircle_endpoint_and_midpoint);
  RUN_TEST(test_arc_glued_flags_no_separator_parses_correctly);
  RUN_TEST(test_malformed_path_data_fails);
  RUN_TEST(test_rect_and_circle_primitives_supported);
  return UNITY_END();
}
