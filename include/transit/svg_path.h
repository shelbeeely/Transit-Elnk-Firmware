#pragma once

// Transit-Elnk-Firmware — minimal SVG-subset parser for route icons.
//
// New header supporting the icon_cache module (unit 6); not part of any
// other unit's frozen contract.
//
// Parses the small subset of SVG actually used by Transit's `-mono` route
// icon set (see docs/ASSETS_ICONS.md, include/transit/icon_cache.h): a
// single <svg viewBox="..."> containing one or more <path d="...">
// elements (plus, defensively, plain <rect>/<circle> primitives, per
// icon_cache.h's "rect/circle/simple paths" note, though none were observed
// in the real icons sampled during development). Supported path commands
// are M/m L/l H/h V/v C/c S/s A/a Z/z (absolute and relative, with SVG's
// usual implicit-command-repeat and M-then-L rule). Elliptical arcs (A/a)
// turned out to matter in practice — half the icons sampled live from
// https://transitapp-data.com/images/svgx/{slug}-mono.svg during
// development (monorail, bike, bikeshare, scooter) use them for rounded
// corners/caps, so they're flattened via the SVG spec's endpoint-to-center
// arc parameterization (Appendix F.6.5) rather than left unsupported; the
// rest (bus, gondola, funicular, stm-metro) needed only M/L/H/V/C/S/Z. NOT
// supported: quadratic curves (Q/q T/t), transforms, <g> grouping,
// gradients/patterns, rounded-rect corners (rx/ry, if present, are ignored
// — rect corners always come out sharp). Any of those (or a missing
// viewBox, or a path/shape this parser can't make sense of) makes
// parseSvgIcon fail closed (return false) rather than guess —
// icon_cache.cpp treats that the same as a fetch failure: a null-data
// IconBitmap, and render_engine's already-existing text-only fallback.
//
// Pure logic — no network I/O, no bitmap/rasterization code — so it
// compiles and is unit-testable under [env:native] same as models.cpp.

#include <string>
#include <vector>

namespace transit {

struct SvgPoint {
  float x = 0.0f;
  float y = 0.0f;
};

// One subpath's flattened outline — curves already subdivided into line
// segments, in the source SVG's viewBox coordinate space. Implicitly closed
// for filling (per SVG fill semantics) regardless of whether the source
// path had an explicit 'Z'.
using SvgSubpath = std::vector<SvgPoint>;

struct SvgIcon {
  float viewBoxMinX = 0.0f;
  float viewBoxMinY = 0.0f;
  float viewBoxWidth = 0.0f;
  float viewBoxHeight = 0.0f;
  // SVG's fill-rule default is "nonzero"; Transit's icons that set it
  // explicitly all use "evenodd" (see file comment above).
  bool evenOddFill = false;
  std::vector<SvgSubpath> subpaths;
};

// Parses `svgSource` (a complete `<svg>...</svg>` document) into flattened
// polygons ready for scanline rasterization. Returns false (leaving `out`
// unspecified) if the document has no viewBox, contains no path/rect/circle
// shape, or uses geometry outside the supported subset described above.
bool parseSvgIcon(const std::string& svgSource, SvgIcon& out);

}  // namespace transit
