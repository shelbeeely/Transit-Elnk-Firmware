#pragma once

// Transit-Elnk-Firmware — route icon fetch/rasterize/cache.
//
// Per docs/ASSETS_ICONS.md: fetch the "-mono" SVG variant from
// https://transitapp-data.com/images/svgx/{slug}-mono.svg, rasterize offline
// (setup-time, not runtime-repeated) to a small monochrome alpha-mask bitmap
// at 28px (icon + text label) or 34px (icon alone), keep it as a plain
// shape-vs-transparent mask, and let render_engine tint it with the route's
// color as a 1-of-4-gray fill at composite time — not baked per-color at
// conversion time (docs/ASSETS_ICONS.md explicitly rules that out: 4x
// storage for no benefit in a 4-gray color space).
//
// A simplified SVG-subset rasterizer (covering whatever primitives the
// Transit icon set actually uses — rect/circle/simple paths) is acceptable
// for the first implementation; document any shape limitations found in the
// icon set in your PR description rather than attempting full SVG fidelity.
//
// Hardware-dependent (network fetch + local storage) — only buildable under
// [env:xteink_x4], not [env:native].
//
// Frozen contract for the parallel work units: do not change the IconCache
// constructor or getIconBitmap signature. Adding a method is fine; note it
// in your PR description.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "transit/api_client.h"

namespace transit {

// A single-tone alpha mask: bit set = shape pixel (to be tinted), bit clear
// = transparent. Sized widthPx x heightPx, row-major, 1 bit per pixel,
// row-padded to a whole byte (matches EInkDisplay::drawImage's expected
// format for a monochrome source).
struct IconBitmap {
  const uint8_t* data = nullptr;  // null = not available (fetch/rasterize failed)
  uint16_t widthPx = 0;
  uint16_t heightPx = 0;
};

class IconCache {
 public:
  explicit IconCache(HttpTransport& transport);

  // Returns the mono alpha-mask bitmap for the given DisplayShortName image
  // slug (elements[0] or elements[2] from docs/DATA_MODEL.md), fetching and
  // rasterizing on first request and serving from cache afterward. sizePx
  // should be 28 (icon + adjacent text label) or 34 (icon alone) per
  // docs/ASSETS_ICONS.md's sizing rule. Returns a null-data IconBitmap if
  // the slug is empty or the fetch/rasterize fails — callers must handle
  // that by falling back to text-only.
  IconBitmap getIconBitmap(const std::string& imageSlug, uint16_t sizePx);

 private:
  // Owns the rasterized bytes an IconBitmap returned above points into, so
  // the pointer stays valid for the cache's (and thus this IconCache
  // instance's) lifetime. In-memory only: one wake-render-sleep cycle per
  // boot doesn't need this to survive deep sleep, so there's no flash/LittleFS
  // persistence here (see icon_cache.cpp's file comment for that tradeoff).
  struct CachedBitmap {
    std::vector<uint8_t> bytes;  // empty = fetch/rasterize failed, cached as such
    uint16_t widthPx = 0;
    uint16_t heightPx = 0;
  };

  HttpTransport& transport_;
  std::map<std::string, CachedBitmap> cache_;  // key: "<imageSlug>|<sizePx>"
};

}  // namespace transit
