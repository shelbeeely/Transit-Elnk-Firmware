#include "transit/svg_path.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace transit {

namespace {

// Finds `name="value"` (or with single quotes) inside `tag`, requiring the
// attribute name to start at a word boundary (start-of-tag or whitespace)
// so e.g. looking up "d" doesn't match inside "id" or a longer attribute.
bool extractAttr(const std::string& tag, const std::string& name, std::string& value) {
  const std::string key = name + "=";
  size_t searchFrom = 0;
  while (true) {
    const size_t pos = tag.find(key, searchFrom);
    if (pos == std::string::npos) return false;
    const bool boundaryOk = (pos == 0) || std::isspace(static_cast<unsigned char>(tag[pos - 1]));
    if (boundaryOk) {
      const size_t qpos = pos + key.size();
      if (qpos < tag.size() && (tag[qpos] == '"' || tag[qpos] == '\'')) {
        const char quote = tag[qpos];
        const size_t end = tag.find(quote, qpos + 1);
        if (end != std::string::npos) {
          value = tag.substr(qpos + 1, end - qpos - 1);
          return true;
        }
      }
    }
    searchFrom = pos + key.size();
  }
}

// Scans `svgSource` for `<tagName ...>` (self-closing or not) elements and
// returns each one's full opening-tag text (attributes only need to be
// extracted from this, so the tag doesn't need to be well-formed past '>').
std::vector<std::string> findTags(const std::string& svgSource, const std::string& tagName) {
  std::vector<std::string> tags;
  const std::string needle = "<" + tagName;
  size_t searchPos = 0;
  while (true) {
    const size_t tagStart = svgSource.find(needle, searchPos);
    if (tagStart == std::string::npos) break;
    const size_t afterNeedle = tagStart + needle.size();
    // Require a real tag boundary right after the name (whitespace, '>' or
    // '/') so "<path" doesn't match a hypothetical "<pathological".
    if (afterNeedle < svgSource.size()) {
      const char next = svgSource[afterNeedle];
      if (!std::isspace(static_cast<unsigned char>(next)) && next != '>' && next != '/') {
        searchPos = afterNeedle;
        continue;
      }
    }
    const size_t tagEnd = svgSource.find('>', tagStart);
    if (tagEnd == std::string::npos) break;  // malformed tail; stop, don't fail the whole doc
    tags.push_back(svgSource.substr(tagStart, tagEnd - tagStart + 1));
    searchPos = tagEnd + 1;
  }
  return tags;
}

// Parses one float starting at `pos` (skipping leading whitespace/commas),
// advancing `pos` past it. Returns false if no number is there.
bool readNumber(const std::string& s, size_t& pos, float& value) {
  while (pos < s.size() &&
         (std::isspace(static_cast<unsigned char>(s[pos])) || s[pos] == ',')) {
    pos++;
  }
  if (pos >= s.size()) return false;
  char* endPtr = nullptr;
  const char* start = s.c_str() + pos;
  value = std::strtof(start, &endPtr);
  if (endPtr == start) return false;
  pos += static_cast<size_t>(endPtr - start);
  return true;
}

// Reads an arc command's large-arc-flag/sweep-flag: exactly one '0' or '1'
// character. Deliberately not readNumber (which would happily gulp a
// following coordinate too) — minifiers routinely glue a flag directly onto
// the next number with no separator (e.g. "...0 011.047 2.38" is flags 0,1
// then x=1.047, not one number "011.047").
bool readFlag(const std::string& s, size_t& pos, bool& value) {
  while (pos < s.size() &&
         (std::isspace(static_cast<unsigned char>(s[pos])) || s[pos] == ',')) {
    pos++;
  }
  if (pos >= s.size() || (s[pos] != '0' && s[pos] != '1')) return false;
  value = (s[pos] == '1');
  pos++;
  return true;
}

// Cubic Bezier (current point p0, controls c1/c2, endpoint) flattened into
// a fixed number of line segments — plenty smooth at the panel's 28-34px
// icon sizes and cheap enough to not need adaptive subdivision.
constexpr int kCurveSegments = 12;

void appendCubic(SvgSubpath& out, SvgPoint p0, SvgPoint c1, SvgPoint c2, SvgPoint end) {
  for (int i = 1; i <= kCurveSegments; i++) {
    const float t = static_cast<float>(i) / kCurveSegments;
    const float mt = 1.0f - t;
    const float a = mt * mt * mt;
    const float b = 3 * mt * mt * t;
    const float c = 3 * mt * t * t;
    const float d = t * t * t;
    out.push_back({a * p0.x + b * c1.x + c * c2.x + d * end.x,
                   a * p0.y + b * c1.y + c * c2.y + d * end.y});
  }
}

constexpr float kPi = 3.14159265358979323846f;

// Signed angle from vector u to vector v, in (-pi, pi].
float vectorAngle(float ux, float uy, float vx, float vy) {
  const float lenProduct = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
  float cosAngle = lenProduct > 0.0f ? (ux * vx + uy * vy) / lenProduct : 1.0f;
  cosAngle = std::max(-1.0f, std::min(1.0f, cosAngle));
  float angle = std::acos(cosAngle);
  if (ux * vy - uy * vx < 0.0f) angle = -angle;
  return angle;
}

// Elliptical arc (SVG's A/a command) from `start` to `end`, flattened into
// line segments appended to `out`. Implements the endpoint-to-center
// parameterization from the SVG spec (Appendix F.6.5) directly, rather than
// converting to Beziers first — one fewer approximation step.
void appendArc(SvgSubpath& out, SvgPoint start, float rx, float ry, float xAxisRotationDeg,
               bool largeArc, bool sweep, SvgPoint end) {
  if (start.x == end.x && start.y == end.y) return;  // zero-length arc: no-op, per spec
  if (rx == 0.0f || ry == 0.0f) {
    out.push_back(end);  // degenerate radius: spec says treat as a straight line
    return;
  }
  rx = std::fabs(rx);
  ry = std::fabs(ry);
  const float phi = xAxisRotationDeg * kPi / 180.0f;
  const float cosPhi = std::cos(phi), sinPhi = std::sin(phi);

  const float dx2 = (start.x - end.x) / 2.0f;
  const float dy2 = (start.y - end.y) / 2.0f;
  const float x1p = cosPhi * dx2 + sinPhi * dy2;
  const float y1p = -sinPhi * dx2 + cosPhi * dy2;

  float rxSq = rx * rx, rySq = ry * ry;
  const float x1pSq = x1p * x1p, y1pSq = y1p * y1p;
  const float lambda = x1pSq / rxSq + y1pSq / rySq;
  if (lambda > 1.0f) {
    const float s = std::sqrt(lambda);
    rx *= s;
    ry *= s;
    rxSq = rx * rx;
    rySq = ry * ry;
  }

  const float sign = (largeArc != sweep) ? 1.0f : -1.0f;
  float num = rxSq * rySq - rxSq * y1pSq - rySq * x1pSq;
  if (num < 0.0f) num = 0.0f;
  const float den = rxSq * y1pSq + rySq * x1pSq;
  const float co = den > 0.0f ? sign * std::sqrt(num / den) : 0.0f;
  const float cxp = co * (rx * y1p / ry);
  const float cyp = co * -(ry * x1p / rx);

  const float cx = cosPhi * cxp - sinPhi * cyp + (start.x + end.x) / 2.0f;
  const float cy = sinPhi * cxp + cosPhi * cyp + (start.y + end.y) / 2.0f;

  const float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
  const float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;

  const float theta1 = vectorAngle(1.0f, 0.0f, ux, uy);
  float dtheta = vectorAngle(ux, uy, vx, vy);
  if (!sweep && dtheta > 0.0f) dtheta -= 2.0f * kPi;
  if (sweep && dtheta < 0.0f) dtheta += 2.0f * kPi;

  // ~16 segments per full circle — smooth enough for a 28-34px icon.
  const int segments = std::max(2, static_cast<int>(std::ceil(std::fabs(dtheta) / (kPi / 8.0f))));
  for (int i = 1; i <= segments; i++) {
    const float theta = theta1 + dtheta * (static_cast<float>(i) / segments);
    const float ct = std::cos(theta), st = std::sin(theta);
    out.push_back({cx + rx * cosPhi * ct - ry * sinPhi * st,
                   cy + rx * sinPhi * ct + ry * cosPhi * st});
  }
}

// Parses one <path>'s `d` attribute into flattened subpaths, appended to
// `outSubpaths`. Returns false on any command outside M/L/H/V/C/S/Z or on
// malformed numeric data.
bool parsePathData(const std::string& d, std::vector<SvgSubpath>& outSubpaths) {
  const size_t len = d.size();
  size_t pos = 0;

  SvgPoint currentPos{0, 0};
  SvgPoint subpathStart{0, 0};
  SvgSubpath current;
  char cmd = 0;
  bool havePrevCubicCtrl = false;
  SvgPoint prevCubicCtrl{0, 0};

  auto startSubpath = [&](SvgPoint p) {
    if (!current.empty()) outSubpaths.push_back(current);
    current.clear();
    current.push_back(p);
    subpathStart = p;
    currentPos = p;
  };
  auto lineTo = [&](SvgPoint p) {
    current.push_back(p);
    currentPos = p;
  };
  auto cubicTo = [&](SvgPoint c1, SvgPoint c2, SvgPoint end) {
    appendCubic(current, currentPos, c1, c2, end);
    currentPos = end;
    prevCubicCtrl = c2;
    havePrevCubicCtrl = true;
  };

  while (true) {
    while (pos < len &&
           (std::isspace(static_cast<unsigned char>(d[pos])) || d[pos] == ',')) {
      pos++;
    }
    if (pos >= len) break;

    const size_t iterStartPos = pos;
    const char c = d[pos];
    if (std::isalpha(static_cast<unsigned char>(c))) {
      cmd = c;
      pos++;
    } else if (cmd == 0) {
      return false;  // path data must start with a command letter
    }
    // else: implicit repeat of the previous command, `pos` unchanged.

    const bool isRelative = std::islower(static_cast<unsigned char>(cmd)) != 0;
    const char upperCmd = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));

    switch (upperCmd) {
      case 'M': {
        float x, y;
        if (!readNumber(d, pos, x) || !readNumber(d, pos, y)) return false;
        SvgPoint p{x, y};
        if (isRelative) {
          p.x += currentPos.x;
          p.y += currentPos.y;
        }
        startSubpath(p);
        havePrevCubicCtrl = false;
        // Extra coordinate pairs after the first are implicit linetos.
        cmd = isRelative ? 'l' : 'L';
        break;
      }
      case 'L': {
        float x, y;
        if (!readNumber(d, pos, x) || !readNumber(d, pos, y)) return false;
        SvgPoint p{x, y};
        if (isRelative) {
          p.x += currentPos.x;
          p.y += currentPos.y;
        }
        lineTo(p);
        havePrevCubicCtrl = false;
        break;
      }
      case 'H': {
        float x;
        if (!readNumber(d, pos, x)) return false;
        lineTo({isRelative ? currentPos.x + x : x, currentPos.y});
        havePrevCubicCtrl = false;
        break;
      }
      case 'V': {
        float y;
        if (!readNumber(d, pos, y)) return false;
        lineTo({currentPos.x, isRelative ? currentPos.y + y : y});
        havePrevCubicCtrl = false;
        break;
      }
      case 'C': {
        float x1, y1, x2, y2, x, y;
        if (!readNumber(d, pos, x1) || !readNumber(d, pos, y1) || !readNumber(d, pos, x2) ||
            !readNumber(d, pos, y2) || !readNumber(d, pos, x) || !readNumber(d, pos, y)) {
          return false;
        }
        SvgPoint c1{x1, y1}, c2{x2, y2}, end{x, y};
        if (isRelative) {
          c1.x += currentPos.x;
          c1.y += currentPos.y;
          c2.x += currentPos.x;
          c2.y += currentPos.y;
          end.x += currentPos.x;
          end.y += currentPos.y;
        }
        cubicTo(c1, c2, end);
        break;
      }
      case 'S': {
        float x2, y2, x, y;
        if (!readNumber(d, pos, x2) || !readNumber(d, pos, y2) || !readNumber(d, pos, x) ||
            !readNumber(d, pos, y)) {
          return false;
        }
        SvgPoint c2{x2, y2}, end{x, y};
        if (isRelative) {
          c2.x += currentPos.x;
          c2.y += currentPos.y;
          end.x += currentPos.x;
          end.y += currentPos.y;
        }
        const SvgPoint c1 = havePrevCubicCtrl ? SvgPoint{2 * currentPos.x - prevCubicCtrl.x,
                                                          2 * currentPos.y - prevCubicCtrl.y}
                                               : currentPos;
        cubicTo(c1, c2, end);
        break;
      }
      case 'A': {
        float rx, ry, xAxisRotation;
        bool largeArc, sweep;
        float x, y;
        if (!readNumber(d, pos, rx) || !readNumber(d, pos, ry) ||
            !readNumber(d, pos, xAxisRotation) || !readFlag(d, pos, largeArc) ||
            !readFlag(d, pos, sweep) || !readNumber(d, pos, x) || !readNumber(d, pos, y)) {
          return false;
        }
        SvgPoint end{x, y};
        if (isRelative) {
          end.x += currentPos.x;
          end.y += currentPos.y;
        }
        appendArc(current, currentPos, rx, ry, xAxisRotation, largeArc, sweep, end);
        currentPos = end;
        havePrevCubicCtrl = false;
        break;
      }
      case 'Z': {
        if (!current.empty()) current.push_back(subpathStart);
        currentPos = subpathStart;
        havePrevCubicCtrl = false;
        cmd = 0;  // Z takes no args; anything after it must be a new command
        break;
      }
      default:
        return false;  // quadratics (Q/T)/unknown letters: unsupported
    }

    if (pos == iterStartPos) return false;  // safety net against zero-progress loops
  }

  if (!current.empty()) outSubpaths.push_back(current);
  return true;
}

// Appends a closed N-gon approximating a circle of radius `r` centered at
// (cx, cy). 24 sides is smooth enough at the panel's 28-34px icon sizes.
void appendCircle(std::vector<SvgSubpath>& subpaths, float cx, float cy, float r) {
  constexpr int kSides = 24;
  SvgSubpath poly;
  poly.reserve(kSides);
  for (int i = 0; i < kSides; i++) {
    const float theta = static_cast<float>(i) / kSides * 6.283185307f;
    poly.push_back({cx + r * std::cos(theta), cy + r * std::sin(theta)});
  }
  subpaths.push_back(std::move(poly));
}

void appendRect(std::vector<SvgSubpath>& subpaths, float x, float y, float w, float h) {
  if (w <= 0 || h <= 0) return;
  subpaths.push_back({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}});
}

bool attrToFloat(const std::string& tag, const std::string& name, float defaultValue,
                  float& out) {
  std::string raw;
  if (!extractAttr(tag, name, raw)) {
    out = defaultValue;
    return true;
  }
  size_t pos = 0;
  return readNumber(raw, pos, out);
}

}  // namespace

bool parseSvgIcon(const std::string& svgSource, SvgIcon& out) {
  const size_t svgTagEnd = svgSource.find('>');
  if (svgTagEnd == std::string::npos) return false;
  const std::string svgTag = svgSource.substr(0, svgTagEnd + 1);

  std::string viewBoxStr;
  if (!extractAttr(svgTag, "viewBox", viewBoxStr)) return false;
  size_t vbPos = 0;
  float minX, minY, width, height;
  if (!readNumber(viewBoxStr, vbPos, minX) || !readNumber(viewBoxStr, vbPos, minY) ||
      !readNumber(viewBoxStr, vbPos, width) || !readNumber(viewBoxStr, vbPos, height)) {
    return false;
  }
  if (width <= 0.0f || height <= 0.0f) return false;

  SvgIcon result;
  result.viewBoxMinX = minX;
  result.viewBoxMinY = minY;
  result.viewBoxWidth = width;
  result.viewBoxHeight = height;
  result.evenOddFill = false;  // SVG default (nonzero) unless a path overrides it below

  bool foundAnyShape = false;

  for (const std::string& tag : findTags(svgSource, "path")) {
    std::string dValue;
    if (!extractAttr(tag, "d", dValue)) return false;  // a <path> with no d is unsupported input
    std::string fillRuleValue;
    if (extractAttr(tag, "fill-rule", fillRuleValue) && fillRuleValue == "evenodd") {
      result.evenOddFill = true;
    }
    if (!parsePathData(dValue, result.subpaths)) return false;
    foundAnyShape = true;
  }

  for (const std::string& tag : findTags(svgSource, "rect")) {
    float x, y, w, h;
    if (!attrToFloat(tag, "x", 0.0f, x) || !attrToFloat(tag, "y", 0.0f, y) ||
        !attrToFloat(tag, "width", 0.0f, w) || !attrToFloat(tag, "height", 0.0f, h)) {
      return false;
    }
    appendRect(result.subpaths, x, y, w, h);
    foundAnyShape = true;
  }

  for (const std::string& tag : findTags(svgSource, "circle")) {
    float cx, cy, r;
    if (!attrToFloat(tag, "cx", 0.0f, cx) || !attrToFloat(tag, "cy", 0.0f, cy) ||
        !attrToFloat(tag, "r", 0.0f, r)) {
      return false;
    }
    if (r <= 0.0f) return false;
    appendCircle(result.subpaths, cx, cy, r);
    foundAnyShape = true;
  }

  if (!foundAnyShape || result.subpaths.empty()) return false;

  out = std::move(result);
  return true;
}

}  // namespace transit
