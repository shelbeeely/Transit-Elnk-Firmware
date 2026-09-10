// Route icon fetch/rasterize/cache — see include/transit/icon_cache.h and
// docs/ASSETS_ICONS.md for the contract this implements.
//
// Pipeline per docs/ASSETS_ICONS.md, adapted to run at request time (the
// header's own contract already documents it this way: "fetching and
// rasterizing on first request and serving from cache afterward") rather
// than the doc's aspirational build-time step, since there's no build-time
// hook wired up for it in this scaffold:
//
//   1. Fetch https://transitapp-data.com/images/svgx/{slug}-mono.svg via the
//      injected HttpTransport.
//   2. Parse it with svg_path.h's small SVG-subset parser (M/L/H/V/C/S/Z
//      paths, plus defensive <rect>/<circle> — see that header's file
//      comment for exactly what's covered and what isn't).
//   3. Rasterize the flattened polygons to a 1bpp alpha mask at sizePx x
//      sizePx: shape pixels set, everything else clear, aspect-fit and
//      centered within the square (the source icons aren't all square —
//      e.g. the sampled "monorail" is 93x144). No color is baked in here;
//      render_engine tints at composite time per ASSETS_ICONS.md.
//   4. Cache the result (success or failure) by "slug|sizePx" so a repeat
//      request for the same icon/size in one boot doesn't re-fetch/re-parse.
//
// Caching is in-memory only (owned by IconCache::cache_, see icon_cache.h)
// and does not survive deep sleep. The device does one wake-render-sleep
// cycle per boot, so that's sufficient per the module's brief; persisting
// rasterized bitmaps to LittleFS across wakes (to skip the network fetch on
// every hourly refresh) would cut boot-to-display latency and radio-on time
// further but is left as a follow-up, not implemented here.
//
// Rasterization uses exact-x/4x-supersampled-y scanline coverage per pixel,
// thresholded at 50% to stay a true 1-bit mask (no gray levels smuggled in
// here — this class's contract is a plain alpha mask) while still
// anti-aliasing away the worst of the jaggies at 28-34px.

#include "transit/icon_cache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "transit/svg_path.h"

namespace transit {

namespace {

constexpr int kSupersampleY = 4;  // vertical sub-scanlines sampled per output row
constexpr const char* kIconBaseUrl = "https://transitapp-data.com/images/svgx/";
constexpr const char* kIconSuffix = "-mono.svg";

struct Crossing {
  float x;
  int winding;  // +1 or -1 by edge direction; used for nonzero fill
};

// Adds fractional horizontal coverage in [x0, x1) (clamped to the bitmap's
// width) to each pixel column's running total for the current sub-scanline.
void addSpanCoverage(std::vector<float>& coverage, float x0, float x1, uint16_t widthPx) {
  x0 = std::max(x0, 0.0f);
  x1 = std::min(x1, static_cast<float>(widthPx));
  if (x1 <= x0) return;
  const int colStart = static_cast<int>(std::floor(x0));
  const int colEnd = static_cast<int>(std::ceil(x1));
  for (int col = std::max(colStart, 0); col < colEnd && col < widthPx; col++) {
    const float left = std::max(x0, static_cast<float>(col));
    const float right = std::min(x1, static_cast<float>(col + 1));
    if (right > left) coverage[static_cast<size_t>(col)] += (right - left);
  }
}

// Transforms `subpaths` (viewBox space) into a `sizePx` x `sizePx` pixel
// space, preserving the source aspect ratio (contain) and centering it —
// Transit's icons aren't all square (e.g. "monorail" is 93x144).
std::vector<SvgSubpath> toPixelSpace(const SvgIcon& icon, uint16_t sizePx) {
  std::vector<SvgSubpath> result;
  if (icon.viewBoxWidth <= 0.0f || icon.viewBoxHeight <= 0.0f) return result;

  const float scale =
      std::min(sizePx / icon.viewBoxWidth, sizePx / icon.viewBoxHeight);
  const float drawnW = icon.viewBoxWidth * scale;
  const float drawnH = icon.viewBoxHeight * scale;
  const float offsetX = (sizePx - drawnW) / 2.0f;
  const float offsetY = (sizePx - drawnH) / 2.0f;

  result.reserve(icon.subpaths.size());
  for (const SvgSubpath& sub : icon.subpaths) {
    SvgSubpath transformed;
    transformed.reserve(sub.size());
    for (const SvgPoint& p : sub) {
      transformed.push_back({offsetX + (p.x - icon.viewBoxMinX) * scale,
                              offsetY + (p.y - icon.viewBoxMinY) * scale});
    }
    result.push_back(std::move(transformed));
  }
  return result;
}

// Scanline-rasterizes `pixelSubpaths` (already in sizePx x sizePx pixel
// space) into a 1bpp row-major mask, rows padded to a whole byte, MSB
// first — the byte *layout* matches EInkDisplay::drawImage's monochrome
// source format, per icon_cache.h's IconBitmap doc comment. The *bit
// value* deliberately does not: bit=1 here means "shape pixel, needs the
// route's tint," per that same doc comment, which is the opposite of
// drawImage/drawImageTransparent's own bit=1-means-white convention (see
// FreeInkDisplay::blitImage). That's intentional, not a mismatch to fix:
// ASSETS_ICONS.md's compositing step is a 1-of-4-gray-levels tint applied
// per pixel, which drawImage/drawImageTransparent (black/white only) can't
// do directly — so render_engine (a separate work unit) reads this mask's
// bits itself when compositing rather than blitting it as-is; it does not
// pass an IconBitmap straight to drawImage/drawImageTransparent.
std::vector<uint8_t> rasterize(const std::vector<SvgSubpath>& pixelSubpaths, bool evenOddFill,
                                uint16_t widthPx, uint16_t heightPx) {
  const uint16_t rowBytes = static_cast<uint16_t>((widthPx + 7) / 8);
  std::vector<uint8_t> bitmap(static_cast<size_t>(rowBytes) * heightPx, 0);
  std::vector<float> coverage(widthPx, 0.0f);
  std::vector<Crossing> crossings;

  for (uint16_t row = 0; row < heightPx; row++) {
    std::fill(coverage.begin(), coverage.end(), 0.0f);

    for (int sub = 0; sub < kSupersampleY; sub++) {
      const float scanY = row + (sub + 0.5f) / kSupersampleY;
      crossings.clear();
      for (const SvgSubpath& poly : pixelSubpaths) {
        const size_t n = poly.size();
        if (n < 2) continue;
        for (size_t i = 0; i < n; i++) {
          const SvgPoint& a = poly[i];
          const SvgPoint& b = poly[(i + 1) % n];
          if (a.y == b.y) continue;  // horizontal edges never cross a scanline
          const bool downward = b.y > a.y;
          const float y0 = downward ? a.y : b.y;
          const float y1 = downward ? b.y : a.y;
          if (scanY < y0 || scanY >= y1) continue;
          const float t = (scanY - a.y) / (b.y - a.y);
          crossings.push_back({a.x + t * (b.x - a.x), downward ? 1 : -1});
        }
      }
      if (crossings.size() < 2) continue;
      std::sort(crossings.begin(), crossings.end(),
                [](const Crossing& l, const Crossing& r) { return l.x < r.x; });

      if (evenOddFill) {
        for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
          addSpanCoverage(coverage, crossings[i].x, crossings[i + 1].x, widthPx);
        }
      } else {
        int winding = 0;
        for (size_t i = 0; i + 1 < crossings.size(); i++) {
          winding += crossings[i].winding;
          if (winding != 0) {
            addSpanCoverage(coverage, crossings[i].x, crossings[i + 1].x, widthPx);
          }
        }
      }
    }

    uint8_t* rowPtr = &bitmap[static_cast<size_t>(row) * rowBytes];
    for (uint16_t col = 0; col < widthPx; col++) {
      if (coverage[col] / kSupersampleY >= 0.5f) {
        rowPtr[col >> 3] |= static_cast<uint8_t>(0x80 >> (col & 7));
      }
    }
  }
  return bitmap;
}

}  // namespace

IconCache::IconCache(HttpTransport& transport) : transport_(transport) {}

IconBitmap IconCache::getIconBitmap(const std::string& imageSlug, uint16_t sizePx) {
  if (imageSlug.empty() || sizePx == 0) return IconBitmap{};

  const std::string cacheKey = imageSlug + "|" + std::to_string(sizePx);
  const auto cached = cache_.find(cacheKey);
  if (cached != cache_.end()) {
    const CachedBitmap& hit = cached->second;
    if (hit.bytes.empty()) return IconBitmap{};  // previously-failed fetch/rasterize, cached
    return IconBitmap{hit.bytes.data(), hit.widthPx, hit.heightPx};
  }

  const std::string url = std::string(kIconBaseUrl) + imageSlug + kIconSuffix;
  const HttpResponse response = transport_.get(url, {});
  if (!response.transportOk) {
    // Connection/DNS/TLS-level failure rather than a real answer from the
    // server — don't cache this one. Wi-Fi can still be stabilizing this
    // early in the boot, and caching a transient miss would keep serving
    // "not available" for the rest of the boot even after connectivity
    // recovers, for every later request of the same icon (e.g. the same
    // route appearing in more than one DirectionBoard row).
    return IconBitmap{};
  }

  // From here on the server gave a real answer (any status code) — cache
  // whatever we conclude below, success or failure, using try_emplace so
  // the not-found `cached` iterator above isn't re-walked to insert.
  CachedBitmap& slot = cache_.try_emplace(cacheKey).first->second;

  if (response.statusCode != 200 || response.body.empty()) return IconBitmap{};

  SvgIcon icon;
  if (!parseSvgIcon(response.body, icon)) return IconBitmap{};

  const std::vector<SvgSubpath> pixelSubpaths = toPixelSpace(icon, sizePx);
  if (pixelSubpaths.empty()) return IconBitmap{};

  slot.bytes = rasterize(pixelSubpaths, icon.evenOddFill, sizePx, sizePx);
  slot.widthPx = sizePx;
  slot.heightPx = sizePx;
  return IconBitmap{slot.bytes.data(), slot.widthPx, slot.heightPx};
}

}  // namespace transit
