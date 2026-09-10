#include "host_render_target.h"

#include <algorithm>
#include <cstdlib>
#include <set>

#include <FreeInkUIFont.h>

#include "png_writer.h"

namespace transit_test {

namespace {

namespace fui = freeink::ui;

uint8_t sampleFor(fui::Color color) {
  switch (color) {
    case fui::Color::Black:
      return 0;
    case fui::Color::DarkGray:
      return 85;
    case fui::Color::LightGray:
      return 170;
    case fui::Color::White:
    case fui::Color::Transparent:
    default:
      return 255;
  }
}

// Mirrors FreeInkUIDisplayTarget.h's DisplayTarget::insideRounded() exactly
// (same rounded-rect corner membership math), so this target agrees with
// the real hardware target on geometry.
bool insideRounded(const fui::Rect rect, const uint8_t radius, const uint8_t corners, const int16_t x,
                   const int16_t y) {
  if (x < rect.x || y < rect.y || x >= rect.right() || y >= rect.bottom()) return false;
  int16_t r = radius;
  const int16_t halfW = static_cast<int16_t>(rect.width / 2);
  const int16_t halfH = static_cast<int16_t>(rect.height / 2);
  if (r > halfW) r = halfW;
  if (r > halfH) r = halfH;
  if (r <= 0) return true;
  const bool left = x < rect.x + r;
  const bool right = x >= rect.right() - r;
  const bool top = y < rect.y + r;
  const bool bottom = y >= rect.bottom() - r;
  int16_t cx = 0, cy = 0;
  bool inCorner = false;
  if (left && top && (corners & fui::CornerTopLeft)) {
    cx = static_cast<int16_t>(rect.x + r);
    cy = static_cast<int16_t>(rect.y + r);
    inCorner = true;
  } else if (right && top && (corners & fui::CornerTopRight)) {
    cx = static_cast<int16_t>(rect.right() - 1 - r);
    cy = static_cast<int16_t>(rect.y + r);
    inCorner = true;
  } else if (left && bottom && (corners & fui::CornerBottomLeft)) {
    cx = static_cast<int16_t>(rect.x + r);
    cy = static_cast<int16_t>(rect.bottom() - 1 - r);
    inCorner = true;
  } else if (right && bottom && (corners & fui::CornerBottomRight)) {
    cx = static_cast<int16_t>(rect.right() - 1 - r);
    cy = static_cast<int16_t>(rect.bottom() - 1 - r);
    inCorner = true;
  }
  if (!inCorner) return true;
  const int dxp = x - cx, dyp = y - cy;
  return dxp * dxp + dyp * dyp <= r * r;
}

int16_t min3(int16_t a, int16_t b, int16_t c) {
  const int16_t m = a < b ? a : b;
  return m < c ? m : c;
}
int16_t max3(int16_t a, int16_t b, int16_t c) {
  const int16_t m = a > b ? a : b;
  return m > c ? m : c;
}
int32_t edge(int16_t px, int16_t py, fui::Point a, fui::Point b) {
  return static_cast<int32_t>(px - b.x) * (a.y - b.y) - static_cast<int32_t>(a.x - b.x) * (py - b.y);
}
bool inTriangle(int16_t px, int16_t py, fui::Point a, fui::Point b, fui::Point c) {
  const int32_t d1 = edge(px, py, a, b);
  const int32_t d2 = edge(px, py, b, c);
  const int32_t d3 = edge(px, py, c, a);
  const bool hasNeg = d1 < 0 || d2 < 0 || d3 < 0;
  const bool hasPos = d1 > 0 || d2 > 0 || d3 > 0;
  return !(hasNeg && hasPos);
}

// UTF-8 decode identical to DisplayTarget::decodeUtf8 -- kNotoSansFont only
// covers U+0020..U+007E, so anything outside that (the same as real
// hardware) falls through to a missing-glyph box.
int decodeUtf8(const char* s, uint32_t& cp) {
  const uint8_t c0 = static_cast<uint8_t>(s[0]);
  if (c0 < 0x80) {
    cp = c0;
    return 1;
  }
  if ((c0 & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
    cp = ((c0 & 0x1Fu) << 6) | (static_cast<uint8_t>(s[1]) & 0x3Fu);
    return 2;
  }
  if ((c0 & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    cp = ((c0 & 0x0Fu) << 12) | ((static_cast<uint8_t>(s[1]) & 0x3Fu) << 6) | (static_cast<uint8_t>(s[2]) & 0x3Fu);
    return 3;
  }
  if ((c0 & 0xF8) == 0xF0) {
    cp = '?';
    return 4;
  }
  cp = c0;
  return 1;
}

const fui::BitmapFont& font() { return fui::kNotoSansFont; }

const fui::FontGlyph* glyphFor(uint32_t cp) {
  const fui::BitmapFont& f = font();
  if (cp < f.first || cp > f.last) return nullptr;
  return &f.glyphs[cp - f.first];
}

int16_t missingGlyphAdvance() {
  const int16_t adv = static_cast<int16_t>(font().yAdvance / 2);
  return adv > 6 ? adv : 6;
}

// Pen advance for one codepoint -- mirrors DisplayTarget::runAdvance (minus
// the RuntimeGlyphSource fallback, which render_engine never installs).
int16_t runAdvance(uint32_t cp) {
  if (cp == 0x2026) {  // U+2026 HORIZONTAL ELLIPSIS -> "..."
    const fui::FontGlyph* dot = glyphFor('.');
    return static_cast<int16_t>(dot ? dot->xAdvance * 3 : 0);
  }
  const fui::FontGlyph* g = glyphFor(cp);
  return g ? g->xAdvance : missingGlyphAdvance();
}

}  // namespace

HostRasterTarget::HostRasterTarget(int16_t width, int16_t height)
    : width_(width), height_(height), pixels_(static_cast<size_t>(width) * static_cast<size_t>(height), 255) {}

void HostRasterTarget::plot(int16_t x, int16_t y, fui::Color color) {
  if (x < 0 || y < 0 || x >= width_ || y >= height_ || color == fui::Color::Transparent) return;
  pixels_[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x)] = sampleFor(color);
}

fui::Size HostRasterTarget::measureText(fui::FontId /*font*/, const char* text, fui::TextStyle /*style*/) const {
  if (!text) return fui::Size{0, static_cast<int16_t>(font().yAdvance)};
  int16_t width = 0;
  const char* p = text;
  while (*p) {
    uint32_t cp;
    p += decodeUtf8(p, cp);
    width = static_cast<int16_t>(width + runAdvance(cp));
  }
  return fui::Size{width, static_cast<int16_t>(font().yAdvance)};
}

int16_t HostRasterTarget::lineHeight(fui::FontId /*font*/) const { return static_cast<int16_t>(font().yAdvance); }

void HostRasterTarget::fill(fui::Rect rect, fui::Paint paint, uint8_t radius, uint8_t corners) {
  if (rect.empty() || paint.kind == fui::PaintKind::None) return;
  if (paint.kind == fui::PaintKind::Bitmap) {
    bitmap(rect, paint.bitmap.bitmap, paint.bitmap.mode, fui::Paint::solid(fui::Color::Black));
    return;
  }
  if (paint.color == fui::Color::Transparent) return;
  for (int16_t y = rect.y; y < rect.bottom(); ++y) {
    for (int16_t x = rect.x; x < rect.right(); ++x) {
      if (radius > 0 && !insideRounded(rect, radius, corners, x, y)) continue;
      plot(x, y, paint.color);
    }
  }
}

void HostRasterTarget::stroke(fui::Rect rect, fui::Paint paint, uint8_t width, uint8_t radius, uint8_t corners) {
  if (rect.empty() || width == 0 || paint.kind == fui::PaintKind::None || paint.color == fui::Color::Transparent)
    return;
  const fui::Insets shrink{static_cast<int16_t>(width), static_cast<int16_t>(width), static_cast<int16_t>(width),
                           static_cast<int16_t>(width)};
  const fui::Rect inner = rect.inset(shrink);
  for (int16_t y = rect.y; y < rect.bottom(); ++y) {
    for (int16_t x = rect.x; x < rect.right(); ++x) {
      const bool onOuter = radius == 0 || insideRounded(rect, radius, corners, x, y);
      if (!onOuter) continue;
      const bool inHole =
          !inner.empty() && (radius == 0 ? inner.contains(x, y) : insideRounded(inner, radius, corners, x, y));
      if (!inHole) plot(x, y, paint.color);
    }
  }
}

void HostRasterTarget::line(fui::Point from, fui::Point to, uint8_t width, fui::Paint paint) {
  if (paint.kind == fui::PaintKind::None || paint.color == fui::Color::Transparent) return;
  int x0 = from.x, y0 = from.y;
  const int x1 = to.x, y1 = to.y;
  const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  const int half = width / 2;
  while (true) {
    for (int by = -half; by <= half; ++by) {
      for (int bx = -half; bx <= half; ++bx) {
        plot(static_cast<int16_t>(x0 + bx), static_cast<int16_t>(y0 + by), paint.color);
      }
    }
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void HostRasterTarget::triangle(fui::Point a, fui::Point b, fui::Point c, fui::Paint paint) {
  if (paint.kind == fui::PaintKind::None || paint.color == fui::Color::Transparent) return;
  const int16_t minX = min3(a.x, b.x, c.x), maxX = max3(a.x, b.x, c.x);
  const int16_t minY = min3(a.y, b.y, c.y), maxY = max3(a.y, b.y, c.y);
  for (int16_t y = minY; y <= maxY; ++y) {
    for (int16_t x = minX; x <= maxX; ++x) {
      if (inTriangle(x, y, a, b, c)) plot(x, y, paint.color);
    }
  }
}

void HostRasterTarget::drawRun(const char* s, int16_t penX, int16_t baseline, fui::Color color) {
  while (*s) {
    uint32_t cp;
    s += decodeUtf8(s, cp);
    if (cp == 0x2026) {
      const fui::FontGlyph* dot = glyphFor('.');
      if (dot) {
        for (int i = 0; i < 3; ++i) {
          for (int gy = 0; gy < dot->height; ++gy) {
            for (int gx = 0; gx < dot->width; ++gx) {
              const int bit = gy * dot->width + gx;
              const uint8_t byte = font().bitmap[dot->bitmapOffset + static_cast<size_t>(bit / 8)];
              if ((byte >> (7 - (bit & 7))) & 0x01) {
                plot(static_cast<int16_t>(penX + dot->xOffset + gx), static_cast<int16_t>(baseline + dot->yOffset + gy),
                     color);
              }
            }
          }
          penX = static_cast<int16_t>(penX + dot->xAdvance);
        }
      }
      continue;
    }
    const fui::FontGlyph* g = glyphFor(cp);
    if (g) {
      for (int gy = 0; gy < g->height; ++gy) {
        for (int gx = 0; gx < g->width; ++gx) {
          const int bit = gy * g->width + gx;
          const uint8_t byte = font().bitmap[g->bitmapOffset + static_cast<size_t>(bit / 8)];
          if ((byte >> (7 - (bit & 7))) & 0x01) {
            plot(static_cast<int16_t>(penX + g->xOffset + gx), static_cast<int16_t>(baseline + g->yOffset + gy),
                 color);
          }
        }
      }
      penX = static_cast<int16_t>(penX + g->xAdvance);
    } else {
      // Missing-glyph box, same shape DisplayTarget draws for a codepoint
      // outside the bundled font's coverage.
      const int16_t w = missingGlyphAdvance();
      int16_t h = static_cast<int16_t>((font().yAdvance * 2) / 3);
      if (h < 8) h = 8;
      const int16_t bx = penX;
      const int16_t by = static_cast<int16_t>(baseline - h + 2);
      for (int16_t dx = 0; dx < w; ++dx) {
        plot(static_cast<int16_t>(bx + dx), by, color);
        plot(static_cast<int16_t>(bx + dx), static_cast<int16_t>(by + h - 1), color);
      }
      for (int16_t dy = 0; dy < h; ++dy) {
        plot(bx, static_cast<int16_t>(by + dy), color);
        plot(static_cast<int16_t>(bx + w - 1), static_cast<int16_t>(by + dy), color);
      }
      penX = static_cast<int16_t>(penX + runAdvance(cp));
    }
  }
}

void HostRasterTarget::text(fui::Rect rect, const char* text, fui::TextStyle style) {
  if (!text || rect.empty()) return;
  const fui::Color inkColor =
      style.inverted && style.color != fui::Color::Transparent ? fui::invertedColor(style.color) : style.color;
  fui::layoutText(*this, rect, text, style, [&](const char* line, const fui::Rect lineRect) {
    drawRun(line, lineRect.x, static_cast<int16_t>(lineRect.y + font().ascent), inkColor);
  });
}

void HostRasterTarget::bitmap(fui::Rect rect, fui::BitmapRef bitmap, fui::BitmapMode mode, fui::Paint foreground,
                              fui::Rotation rotation) {
  if (!bitmap || rect.empty()) return;
  if (bitmap.format != fui::BitmapFormat::BW1 && bitmap.format != fui::BitmapFormat::Mask1) return;
  const fui::Color color = foreground.kind == fui::PaintKind::None ? fui::Color::Black : foreground.color;
  if (color == fui::Color::Transparent) return;
  fui::forEachBitmapPixel(
      rect, bitmap, mode, [&](const int16_t px, const int16_t py) { plot(px, py, color); }, rotation);
}

bool HostRasterTarget::hasVisibleContent(size_t minDistinctSamples) const {
  std::set<uint8_t> distinct(pixels_.begin(), pixels_.end());
  return distinct.size() >= minDistinctSamples;
}

bool HostRasterTarget::writePng(const std::string& path) const { return writeGrayscalePng(path, width_, height_, pixels_); }

}  // namespace transit_test
