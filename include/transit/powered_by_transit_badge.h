#pragma once

// Transit-Elnk-Firmware — embedded "Powered by Transit" logo asset.
//
// Generated from Transit's official EN badge kit (transit-api-badge@3x.png,
// 336x111 source), resized to 96x32 preserving the original ~3:1 aspect
// ratio, then thresholded (luminance < 200, after compositing the source's
// transparent corners onto white) to a 1-bit set-bit-is-ink mask: the
// badge's solid fill becomes ink (the majority of the frame — the rounded
// mark nearly fills the 96x32 canvas); the white icon/wordmark cutouts and
// the thin transparent corner slivers are non-ink, so a dark foreground tint
// reproduces the original badge's look (a solid dark rounded mark with the
// icon/wordmark showing through as the page's own white background) — see
// docs/ASSETS_ICONS.md's same alpha-mask-plus-tint approach used for route
// icons (render_engine.cpp's toBitmapRef()/drawRouteBadge()).
//
// Row-major, MSB-first, each row padded to a whole byte (12 bytes/row for
// width 96) — the same BitmapFormat::BW1 polarity IconBitmap already
// documents.
//
// Regenerating this asset (e.g. a higher-resolution source, the FR variant,
// or a size change): resize to the target dimensions, composite onto white,
// convert to 8-bit grayscale, threshold at roughly the midpoint between the
// badge's fill color's luminance and white, then pack MSB-first per row
// (row byte count = ceil(width / 8)).

#include <cstdint>

namespace transit {

static constexpr int16_t kPoweredByTransitBadgeWidth = 96;
static constexpr int16_t kPoweredByTransitBadgeHeight = 32;
static const uint8_t kPoweredByTransitBadgeMask[384] = {
  3, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 192, 15, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 240, 31, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 248, 63, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 252,
  127, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 127, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 254, 255, 192, 3, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 0, 0, 255, 135, 255, 255, 255, 255, 123, 255, 255,
  254, 0, 0, 127, 179, 255, 255, 255, 255, 123, 255, 255, 254, 0, 0, 127,
  178, 22, 72, 102, 56, 120, 110, 255, 252, 16, 112, 127, 132, 218, 91, 108,
  215, 123, 173, 255, 252, 49, 248, 127, 141, 202, 80, 108, 23, 123, 181, 255,
  252, 49, 220, 127, 188, 217, 147, 237, 247, 123, 177, 255, 252, 1, 140, 127,
  190, 29, 184, 110, 56, 120, 123, 255, 252, 49, 136, 127, 255, 255, 255, 255,
  255, 255, 243, 255, 252, 57, 136, 127, 255, 255, 255, 255, 255, 255, 247, 255,
  252, 49, 156, 127, 255, 255, 255, 255, 255, 255, 255, 255, 252, 49, 140, 127,
  255, 255, 255, 255, 255, 255, 255, 255, 252, 49, 128, 127, 131, 255, 255, 247,
  255, 255, 255, 255, 252, 59, 140, 127, 131, 255, 255, 254, 127, 255, 255, 255,
  252, 31, 12, 127, 204, 97, 14, 100, 63, 255, 255, 255, 252, 14, 8, 127,
  204, 65, 36, 228, 127, 255, 255, 255, 254, 0, 0, 127, 205, 155, 100, 108,
  255, 255, 255, 255, 254, 0, 0, 127, 217, 155, 103, 108, 255, 255, 255, 255,
  255, 0, 0, 255, 217, 194, 108, 76, 127, 255, 255, 255, 255, 192, 3, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 127, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 254, 127, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254,
  63, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 252, 31, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 248, 15, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 240, 3, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 192,
};

}  // namespace transit
