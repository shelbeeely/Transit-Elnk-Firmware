#pragma once

// Host-side recording freeink::ui::DrawTarget -- test/test_render_snapshot
// only, never compiled into the firmware or any other test binary (this
// file lives under one test's own directory, so PlatformIO only pulls it
// into that test's build; see platformio.ini's [env:native] comment).
//
// Rasterizes every DrawTarget call into an in-memory 8bpp grayscale buffer
// instead of touching real hardware, so RenderEngine::renderDepartureBoard()
// et al. (render_engine.h) can be captured to a real PNG with no device and
// no simulator. Samples map 1:1 to freeink::ui::Color's 4 real gray levels
// (Black=0, DarkGray=85, LightGray=170, White=255 -- see
// freeink-sdk/docs/grayscale-capabilities.md) as flat fills rather than
// reproducing the 1-bit Bayer-dithered halftone the real panel's
// DisplayTarget renders: a flat fill/text reads far more clearly in a
// screenshot meant for a human to eyeball for "does the layout look right"
// than a dithered pattern would, and it faithfully represents what each
// Paint::solid() call actually asked for.
//
// Text rendering mirrors DisplayTarget's own approach one-for-one
// (FreeInkUIDisplayTarget.h's drawRun()/drawGlyph()) against the same
// bundled Noto Sans 1bpp bitmap font (FreeInkUIFont.h) -- FreeInkUI's own
// layoutText() (FreeInkUICore.h) does the line-wrap/truncation/alignment
// work and hands this class one already-positioned line at a time, so this
// class only has to walk each character's fixed glyph bitmap, not implement
// text layout itself.
//
// fill()/stroke()/line()/triangle()/bitmap() mirror DisplayTarget's own
// rasterization math (rounded-rect membership, Bresenham lines, edge-
// function triangles, forEachBitmapPixel() for bitmap blits -- the last is
// a public FreeInkUICore.h helper DisplayTarget itself uses) so the two
// DrawTargets agree pixel-for-pixel on geometry; only the "how is a Color
// turned into an output sample" step differs, per the flat-vs-dithered
// rationale above.

#include <cstdint>
#include <string>
#include <vector>

#include <FreeInkUICore.h>

namespace transit_test {

class HostRasterTarget : public freeink::ui::DrawTarget {
 public:
  HostRasterTarget(int16_t width, int16_t height);

  freeink::ui::Size measureText(freeink::ui::FontId font, const char* text,
                                freeink::ui::TextStyle style) const override;
  int16_t lineHeight(freeink::ui::FontId font) const override;
  void fill(freeink::ui::Rect rect, freeink::ui::Paint paint, uint8_t radius = 0,
           uint8_t corners = freeink::ui::CornersAll) override;
  void stroke(freeink::ui::Rect rect, freeink::ui::Paint paint, uint8_t width, uint8_t radius = 0,
             uint8_t corners = freeink::ui::CornersAll) override;
  void line(freeink::ui::Point from, freeink::ui::Point to, uint8_t width, freeink::ui::Paint paint) override;
  void triangle(freeink::ui::Point a, freeink::ui::Point b, freeink::ui::Point c,
               freeink::ui::Paint paint) override;
  void text(freeink::ui::Rect rect, const char* text, freeink::ui::TextStyle style) override;
  void bitmap(freeink::ui::Rect rect, freeink::ui::BitmapRef bitmap, freeink::ui::BitmapMode mode,
             freeink::ui::Paint foreground = freeink::ui::Paint::solid(freeink::ui::Color::Black),
             freeink::ui::Rotation rotation = freeink::ui::Rotation::None) override;

  int16_t width() const { return width_; }
  int16_t height() const { return height_; }
  // Row-major, one byte per pixel, 0 = black .. 255 = white.
  const std::vector<uint8_t>& pixels() const { return pixels_; }

  // True once at least minDistinctSamples different 0-255 values appear in
  // the buffer -- a real regression signal ("this frame actually painted a
  // nontrivial, multi-tone layout"), not just "the call didn't crash".
  bool hasVisibleContent(size_t minDistinctSamples = 2) const;

  // Writes the buffer as an 8-bit grayscale PNG (png_writer.h). Returns
  // false only on an I/O failure opening/writing `path`.
  bool writePng(const std::string& path) const;

 private:
  int16_t width_;
  int16_t height_;
  std::vector<uint8_t> pixels_;  // width_ * height_, row-major

  void plot(int16_t x, int16_t y, freeink::ui::Color color);
  void drawRun(const char* utf8, int16_t penX, int16_t baseline, freeink::ui::Color color);
};

}  // namespace transit_test
