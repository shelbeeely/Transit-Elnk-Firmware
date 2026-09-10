// Transit-Elnk-Firmware — departure-board layout and drawing.
//
// Draws through FreeInkUI's native, dependency-free freeink::ui::DrawTarget
// interface (FreeInkUICore.h) -- the same primitive Free-Ink/inkdeck's
// screens.cpp uses for both text (target.text(rect, str, style)) and icon
// compositing (target.bitmap(rect, ref, mode, tintPaint)). No custom font or
// bitmap pipeline: text uses whatever BitmapFont the concrete DrawTarget was
// set up with (FreeInkUI's bundled Noto Sans on the real hardware path), and
// icon tinting reuses the target's own Paint/Color handling (see the note on
// drawRouteBadge() below for why that's used instead of a raw bitmap blit).
//
// This module only ever talks to the abstract DrawTarget/FramePresenter
// pair it's constructed with (render_engine.h) -- never EInkDisplay -- so it
// has no hardware dependency and builds/tests under [env:native] as well as
// [env:xteink_x4]. See render_engine.h's file comment for who constructs
// which concrete DrawTarget/FramePresenter.
//
// This module is purely a renderer: it never touches input, so it draws
// straight through a DrawTarget without the Frame<N>/InteractionBuffer
// machinery FreeInkUI's interactive components need.

#include "transit/render_engine.h"

#include "transit/powered_by_transit_badge.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <time.h>

namespace transit {

namespace {

namespace fui = freeink::ui;

// --- panel layout constants (800x480, landscape-native X4) -----------------
constexpr int16_t kMargin = 16;
constexpr int16_t kHeaderHeight = 48;
constexpr int16_t kRowHeight = 74;
constexpr int16_t kRowGap = 8;
constexpr int16_t kIconColumnWidth = 44;
constexpr int16_t kLineGap = 4;
constexpr int16_t kChipGap = 10;
// Breathing room above/below the attribution strip's "Powered by Transit"
// badge (see drawAttributionFooter() below), so it isn't flush against the
// screen edge.
constexpr int16_t kFooterPadding = 6;

struct Rgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

// route_color / route_text_color are plain hex, no leading '#' (models.h),
// but tolerate one anyway since it's a cheap check.
bool parseHexColor(const std::string& hexIn, Rgb& out) {
  const std::string s = (!hexIn.empty() && hexIn[0] == '#') ? hexIn.substr(1) : hexIn;
  if (s.size() != 6) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  auto byteAt = [&](size_t i) -> int {
    const int hi = nibble(s[i]);
    const int lo = nibble(s[i + 1]);
    return (hi < 0 || lo < 0) ? -1 : (hi << 4) | lo;
  };
  const int r = byteAt(0);
  const int g = byteAt(2);
  const int b = byteAt(4);
  if (r < 0 || g < 0 || b < 0) return false;
  out = Rgb{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
  return true;
}

// docs/ASSETS_ICONS.md "Route-color badge fill": quantize a hex color to the
// nearest of the panel's 4 supported gray levels (0/85/170/255) via standard
// perceptual luma weights, then map that level to the matching Color. Used
// for both route_color (badge/icon tint) and route_text_color (badge text /
// tint of a colored icon) per the same doc's step 3 -- quantizing
// route_text_color too instead of assuming pure black/white, since a GTFS
// feed can set it to something else.
fui::Color quantizeHexColor(const std::string& hex, fui::Color fallback) {
  Rgb rgb;
  if (!parseHexColor(hex, rgb)) return fallback;
  const double luma = 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
  static constexpr double kLevels[4] = {0.0, 85.0, 170.0, 255.0};
  static constexpr fui::Color kColors[4] = {fui::Color::Black, fui::Color::DarkGray, fui::Color::LightGray,
                                            fui::Color::White};
  int best = 0;
  double bestDist = 1e9;
  for (int i = 0; i < 4; ++i) {
    const double d = luma - kLevels[i] < 0 ? kLevels[i] - luma : luma - kLevels[i];
    if (d < bestDist) {
      bestDist = d;
      best = i;
    }
  }
  return kColors[best];
}

void formatClock(int64_t epochSeconds, char* buf, size_t bufLen) {
  if (epochSeconds <= 0) {
    snprintf(buf, bufLen, "--:--");
    return;
  }
  const time_t t = static_cast<time_t>(epochSeconds);
  struct tm tmVal {};
  localtime_r(&t, &tmVal);
  snprintf(buf, bufLen, "%02d:%02d", tmVal.tm_hour, tmVal.tm_min);
}

// One departure chip. docs/UI_BEHAVIOR.md: real-time gets a badge next to the
// time (icon-only in the reference apps; rendered here as "RT" text plus a
// dithered pill background -- see drawDirectionRow() -- since no bolt icon
// asset is bundled and this module doesn't add a custom graphics pipeline).
// isLast replaces the trailing unit with "last" instead of "m". Falls back
// to a clock time when nowEpoch is unavailable (SNTP never synced this wake).
std::string formatDepartureChip(const DepartureRow& dep, int64_t nowEpoch) {
  std::string text;
  if (nowEpoch <= 0) {
    char clock[8];
    formatClock(dep.departureTimeEpoch, clock, sizeof(clock));
    text = clock;
  } else {
    int64_t minutes = (dep.departureTimeEpoch - nowEpoch) / 60;
    if (minutes < 0) minutes = 0;
    if (minutes <= 0) {
      text = dep.isLast ? "Last" : "Due";
    } else if (dep.isLast) {
      text = std::to_string(minutes) + " last";
    } else {
      text = std::to_string(minutes) + "m";
    }
  }
  if (dep.isRealTime) text += " RT";
  return text;
}

// icon_cache.h: IconBitmap is a row-padded-to-byte 1bpp mask where a set bit
// is the shape (to be tinted) -- the same polarity FreeInkUICore.h documents
// for BitmapFormat::BW1 ("set-bit-is-ink"), as opposed to Mask1 (the
// freeink::Icon asset convention, inverted).
fui::BitmapRef toBitmapRef(const IconBitmap& icon) {
  fui::BitmapRef ref;
  ref.data = icon.data;
  ref.width = icon.widthPx;
  ref.height = icon.heightPx;
  ref.format = fui::BitmapFormat::BW1;
  ref.progmem = false;  // icon_cache-owned buffer, not a flash asset
  return ref;
}

// Which DisplayShortName image slug to fetch (left preferred, then right;
// docs/ASSETS_ICONS.md) and the icon size its own sizing rule calls for: 28px
// when a text label sits alongside the icon, 34px when the icon stands
// alone.
struct RouteBadge {
  std::string slug;
  bool hasLabelText = false;
  uint16_t sizePx = 34;
};

RouteBadge pickBadge(const DisplayShortName& name) {
  RouteBadge badge;
  badge.slug = !name.elements[0].empty() ? name.elements[0] : name.elements[2];
  badge.hasLabelText = !name.elements[1].empty();
  badge.sizePx = badge.hasLabelText ? 28 : 34;
  return badge;
}

void drawStatusHeader(fui::DrawTarget& target, int16_t screenW, const BoardStatus& status) {
  // Battery glyph, right-aligned.
  constexpr int16_t kBatteryW = 26;
  constexpr int16_t kBatteryH = 13;
  constexpr int16_t kBatteryNub = 2;
  const fui::Rect battery{static_cast<int16_t>(screenW - kMargin - kBatteryW - kBatteryNub),
                          static_cast<int16_t>((kHeaderHeight - kBatteryH) / 2), kBatteryW, kBatteryH};
  target.stroke(battery, fui::Paint::solid(fui::Color::Black), 1);
  target.fill(fui::Rect{battery.right(), static_cast<int16_t>(battery.y + kBatteryH / 4), kBatteryNub,
                        static_cast<int16_t>(kBatteryH / 2)},
             fui::Paint::solid(fui::Color::Black));
  const int pct = status.batteryPercent < 0 ? 0 : (status.batteryPercent > 100 ? 100 : status.batteryPercent);
  fui::Rect fillArea = battery.inset(fui::Insets{2, 2, 2, 2});
  fillArea.width = static_cast<int16_t>((fillArea.width * pct) / 100);
  if (fillArea.width > 0) target.fill(fillArea, fui::Paint::solid(fui::Color::Black));

  int16_t cursorRight = static_cast<int16_t>(battery.x - 10);

  // Wi-Fi / fetch-failure indicator, left of the battery glyph.
  if (!status.wifiOk || status.lastFetchFailed) {
    const char* label = !status.wifiOk ? "No Wi-Fi" : "Fetch failed";
    fui::TextStyle warn;
    warn.align = fui::TextAlign::Right;
    warn.maxLines = 1;
    const fui::Size sz = target.measureText(warn.font, label, warn);
    const fui::Rect rect{static_cast<int16_t>(cursorRight - sz.width),
                         static_cast<int16_t>((kHeaderHeight - sz.height) / 2), sz.width, sz.height};
    target.text(rect, label, warn);
    cursorRight = static_cast<int16_t>(rect.x - 10);
  }

  // Last-updated clock (the fetch/SNTP-sync time, not a live countdown --
  // this device deep-sleeps between wakes, see docs/UI_BEHAVIOR.md's refresh
  // cadence discussion), left of that.
  char clock[8];
  formatClock(status.lastUpdatedEpoch, clock, sizeof(clock));
  const std::string updated = std::string("Updated ") + clock;
  fui::TextStyle updatedStyle;
  updatedStyle.align = fui::TextAlign::Right;
  updatedStyle.maxLines = 1;
  const fui::Size updSz = target.measureText(updatedStyle.font, updated.c_str(), updatedStyle);
  const fui::Rect updRect{static_cast<int16_t>(cursorRight - updSz.width),
                          static_cast<int16_t>((kHeaderHeight - updSz.height) / 2), updSz.width, updSz.height};
  target.text(updRect, updated.c_str(), updatedStyle);
  cursorRight = static_cast<int16_t>(updRect.x - 12);

  // Stop name fills the remaining left-hand space.
  int16_t nameWidth = static_cast<int16_t>(cursorRight - kMargin);
  if (nameWidth < 0) nameWidth = 0;
  fui::TextStyle nameStyle;
  nameStyle.maxLines = 1;
  nameStyle.bold = true;
  const fui::Rect nameRect{kMargin, 0, nameWidth, kHeaderHeight};
  const char* stopName = status.stopName.empty() ? "Transit" : status.stopName.c_str();
  target.text(nameRect, stopName, nameStyle);

  target.line(fui::Point{0, kHeaderHeight}, fui::Point{screenW, kHeaderHeight}, 1, fui::Paint::solid(fui::Color::Black));
}

// Route icon compositing. Uses DrawTarget::bitmap() -- the exact primitive
// inkdeck's screens.cpp uses to draw icons (target.bitmap(rect, ref, mode,
// foregroundPaint)) -- rather than a raw 1-bit overwrite/AND blit: that
// would mean hand-rolling docs/ASSETS_ICONS.md's "tint the alpha mask as one
// of the panel's 4 grays" ourselves (and already-dithered gray fill/stroke/
// text draws in this file rely on the same DrawTarget-level color handling).
//
// Returns true when an actual icon glyph was drawn (so the caller knows
// whether line 1 still needs a route-label caption next to it), false when
// the text-badge fallback ran instead (no slug, fetch/rasterize failure, or
// forceLabelFallback -- see the gray-collision handling in
// renderDepartureBoard()) -- the fallback badge already carries the route's
// identifying text, so nothing else should repeat it.
bool drawRouteBadge(fui::DrawTarget& target, IconCache& iconCache, const fui::Rect& slot,
                    const DirectionBoard& dir, const RouteBadge& badge, bool forceLabelFallback) {
  const fui::Color tint = quantizeHexColor(dir.routeColor, fui::Color::Black);

  const IconBitmap icon = (forceLabelFallback || badge.slug.empty()) ? IconBitmap{}
                                                                     : iconCache.getIconBitmap(badge.slug, badge.sizePx);
  if (icon.data != nullptr) {
    target.bitmap(slot, toBitmapRef(icon), fui::BitmapMode::Center, fui::Paint::solid(tint));
    return true;
  }

  // Fallback: no image slug, the fetch/rasterize failed, or this route's
  // quantized badge color collides with another route shown on this same
  // board (docs/ASSETS_ICONS.md: "multiple routes with visually distinct hex
  // colors can quantize to the same gray bucket... fall back to rendering
  // route_short_name as a text overlay on the badge").
  const fui::Color textTint = quantizeHexColor(dir.routeTextColor, fui::Color::White);
  const fui::Rect badgeRect = slot.inset(fui::Insets{4, 4, 4, 4});
  if (badgeRect.empty()) return false;
  target.fill(badgeRect, fui::Paint::solid(tint), 3);
  fui::TextStyle style;
  style.align = fui::TextAlign::Center;
  style.color = textTint;
  style.maxLines = 1;
  const std::string& label = !dir.routeShortName.empty() ? dir.routeShortName : dir.routeDisplayShortName.elements[1];
  target.text(badgeRect, label.c_str(), style);
  return false;
}

void drawDirectionRow(fui::DrawTarget& target, IconCache& iconCache, const fui::Rect& rowRect,
                      const DirectionBoard& dir, int64_t nowEpoch, bool colorCollision) {
  const RouteBadge badge = pickBadge(dir.routeDisplayShortName);
  const fui::Rect iconSlot{rowRect.x, rowRect.y, kIconColumnWidth, rowRect.height};
  const bool iconDrawn = drawRouteBadge(target, iconCache, iconSlot, dir, badge, colorCollision);

  const int16_t textX = static_cast<int16_t>(rowRect.x + kIconColumnWidth + 8);
  const int16_t textW = static_cast<int16_t>(rowRect.width - kIconColumnWidth - 8);
  const int16_t lineH = static_cast<int16_t>((rowRect.height - kLineGap) / 2);

  // Line 1: route label -- only when an actual icon glyph was drawn and
  // needs a caption next to it (badge.hasLabelText); the text-badge fallback
  // already shows the route's identifying text in the icon column, so
  // repeating it here would just print the same route number twice on one
  // row -- plus the headsign of the soonest departure.
  const std::string headsign = dir.departures.empty() ? std::string() : dir.departures.front().headsign;
  std::string line1;
  if (iconDrawn && badge.hasLabelText) {
    line1 = dir.routeDisplayShortName.elements[1] + "  " + headsign;
  } else {
    line1 = headsign;
  }
  if (line1.empty() && iconDrawn) line1 = dir.routeShortName;

  fui::TextStyle line1Style;
  line1Style.maxLines = 1;
  const fui::Rect line1Rect{textX, rowRect.y, textW, lineH};
  target.text(line1Rect, line1.c_str(), line1Style);

  // Line 2: one chip per already-filtered/sorted/capped departure
  // (ui_logic's buildDepartureBoard already applied the departure window,
  // cancellation filter, and per-direction cap).
  const fui::Rect line2Rect{textX, static_cast<int16_t>(rowRect.y + lineH + kLineGap), textW, lineH};
  if (dir.departures.empty()) {
    fui::TextStyle none;
    none.maxLines = 1;
    none.color = fui::Color::DarkGray;
    target.text(line2Rect, "No upcoming departures", none);
    return;
  }

  int16_t chipX = line2Rect.x;
  const int16_t chipRight = line2Rect.right();
  for (const DepartureRow& dep : dir.departures) {
    const std::string chip = formatDepartureChip(dep, nowEpoch);
    fui::TextStyle chipStyle;
    chipStyle.maxLines = 1;
    const fui::Size sz = target.measureText(chipStyle.font, chip.c_str(), chipStyle);
    if (chipX + sz.width > chipRight) break;  // out of room; ui_logic already caps the count
    const fui::Rect chipRect{chipX, line2Rect.y, sz.width, line2Rect.height};
    if (dep.isRealTime) {
      // Dithered pill behind real-time chips -- the "small icon/badge next
      // to the time" UI_BEHAVIOR.md describes, without a bundled bolt asset.
      target.fill(chipRect.inset(fui::Insets{-2, -3, -2, -3}), fui::Paint::solid(fui::Color::LightGray), 3);
    }
    target.text(chipRect, chip.c_str(), chipStyle);
    chipX = static_cast<int16_t>(chipX + sz.width + kChipGap);
  }
}

// Transit API ToS compliance (docs/DEPLOYMENT_OPS.md, "Transit API Terms of
// Service -- compliance requirements"): the departure board -- this device's
// main interface -- must always show a "Powered by Transit" attribution.
// Draws the real logo (include/transit/powered_by_transit_badge.h, Transit's
// official badge kit rasterized to a 1-bit mask -- see that header for the
// exact conversion) via the same DrawTarget::bitmap()+Paint tinting path
// drawRouteBadge() below uses for route icons, rather than a text label.
void drawAttributionFooter(fui::DrawTarget& target, int16_t screenW, int16_t screenH, int16_t footerHeight) {
  const fui::BitmapRef badge{kPoweredByTransitBadgeMask, static_cast<uint16_t>(kPoweredByTransitBadgeWidth),
                             static_cast<uint16_t>(kPoweredByTransitBadgeHeight), fui::BitmapFormat::BW1,
                             /*progmem=*/true};
  const fui::Rect rect{kMargin, static_cast<int16_t>(screenH - footerHeight),
                       static_cast<int16_t>(screenW - 2 * kMargin), footerHeight};
  target.bitmap(rect, badge, fui::BitmapMode::Center, fui::Paint::solid(fui::Color::Black));
}

// Height of the attribution strip -- passed to drawAttributionFooter() and
// reserved from the departure rows' body area: the badge's own native
// height plus a little padding so it isn't flush against the screen edge.
constexpr int16_t attributionFooterHeight() {
  return static_cast<int16_t>(kPoweredByTransitBadgeHeight + kFooterPadding);
}

// Shared by renderSetupPrompt()/renderSetupList(): a bold title plus a
// divider. Returns the y-coordinate the caller's own content should start
// at, below the divider.
int16_t drawScreenHeader(fui::DrawTarget& target, int16_t screenW, const std::string& title) {
  fui::TextStyle titleStyle;
  titleStyle.bold = true;
  titleStyle.maxLines = 1;
  const fui::Rect titleRect{kMargin, kMargin, static_cast<int16_t>(screenW - 2 * kMargin), 40};
  target.text(titleRect, title.c_str(), titleStyle);

  const int16_t dividerY = static_cast<int16_t>(kMargin + 44);
  target.line(fui::Point{kMargin, dividerY}, fui::Point{static_cast<int16_t>(screenW - kMargin), dividerY}, 1,
             fui::Paint::solid(fui::Color::Black));

  return static_cast<int16_t>(kMargin + 56);
}

}  // namespace

RenderEngine::RenderEngine(freeink::ui::DrawTarget& target, FramePresenter& presenter, IconCache& iconCache,
                           int16_t screenWidth, int16_t screenHeight)
    : target_(target),
      presenter_(presenter),
      iconCache_(iconCache),
      screenWidth_(screenWidth),
      screenHeight_(screenHeight) {}

void RenderEngine::renderDepartureBoard(const std::vector<DirectionBoard>& board, const BoardStatus& status) {
  target_.fill(fui::Rect{0, 0, screenWidth_, screenHeight_}, fui::Paint::solid(fui::Color::White));

  drawStatusHeader(target_, screenWidth_, status);

  const int16_t footerHeight = attributionFooterHeight();
  const int16_t bodyTop = static_cast<int16_t>(kHeaderHeight + 8);
  // Reserve the footer strip (plus its own kMargin gap above it) below the
  // last row so the attribution label is never crowded or covered.
  const int16_t bodyBottom = static_cast<int16_t>(screenHeight_ - kMargin - footerHeight);
  const int maxRows = std::max(0, (bodyBottom - bodyTop + kRowGap) / (kRowHeight + kRowGap));

  if (board.empty()) {
    fui::TextStyle empty;
    empty.align = fui::TextAlign::Center;
    empty.maxLines = 2;
    const fui::Rect emptyRect{kMargin, bodyTop, static_cast<int16_t>(screenWidth_ - 2 * kMargin),
                              static_cast<int16_t>(bodyBottom - bodyTop)};
    target_.text(emptyRect, "No departures to show.", empty);
  } else {
    const int rowsToDraw = std::min<int>(maxRows, static_cast<int>(board.size()));

    // docs/ASSETS_ICONS.md: routes with visually distinct hex colors can
    // quantize to the same one of the 4 gray levels. Detect that collision
    // across the rows actually being drawn together, so drawRouteBadge()
    // knows to fall back to a text badge for every route sharing a bucket
    // (an icon tinted the same gray as its neighbor's is not disambiguating,
    // even if the icon artwork itself differs).
    std::vector<fui::Color> tints;
    tints.reserve(static_cast<size_t>(rowsToDraw));
    for (int i = 0; i < rowsToDraw; ++i) {
      tints.push_back(quantizeHexColor(board[i].routeColor, fui::Color::Black));
    }

    for (int i = 0; i < rowsToDraw; ++i) {
      bool collision = false;
      for (int j = 0; j < rowsToDraw; ++j) {
        if (j != i && tints[j] == tints[i]) {
          collision = true;
          break;
        }
      }
      const fui::Rect rowRect{kMargin, static_cast<int16_t>(bodyTop + i * (kRowHeight + kRowGap)),
                              static_cast<int16_t>(screenWidth_ - 2 * kMargin), kRowHeight};
      drawDirectionRow(target_, iconCache_, rowRect, board[i], status.lastUpdatedEpoch, collision);
    }
  }

  drawAttributionFooter(target_, screenWidth_, screenHeight_, footerHeight);

  presenter_.present();
}

void RenderEngine::renderSetupPrompt(const std::string& title, const std::string& body) {
  target_.fill(fui::Rect{0, 0, screenWidth_, screenHeight_}, fui::Paint::solid(fui::Color::White));

  const int16_t bodyTop = drawScreenHeader(target_, screenWidth_, title);

  fui::TextStyle bodyStyle;
  bodyStyle.maxLines = 10;
  const fui::Rect bodyRect{kMargin, bodyTop, static_cast<int16_t>(screenWidth_ - 2 * kMargin),
                           static_cast<int16_t>(screenHeight_ - kMargin - bodyTop)};
  target_.text(bodyRect, body.c_str(), bodyStyle);

  presenter_.present();
}

void RenderEngine::renderSetupList(const std::string& title, const std::vector<std::string>& items,
                                   int selectedIndex) {
  target_.fill(fui::Rect{0, 0, screenWidth_, screenHeight_}, fui::Paint::solid(fui::Color::White));

  const int16_t listTop = drawScreenHeader(target_, screenWidth_, title);

  constexpr int16_t kListRowHeight = 40;
  const int16_t listBottom = static_cast<int16_t>(screenHeight_ - kMargin);
  const fui::Rect listRect{kMargin, listTop, static_cast<int16_t>(screenWidth_ - 2 * kMargin),
                           static_cast<int16_t>(listBottom - listTop)};

  if (items.empty()) {
    presenter_.present();
    return;
  }

  const uint16_t visibleRows = fui::listVisibleRows(listRect, kListRowHeight);
  const int clampedSelected = fui::listClampedIndex(selectedIndex, static_cast<int>(items.size()));
  const uint16_t topIndex = fui::listTopIndexFor(static_cast<int16_t>(clampedSelected), /*topIndex=*/0, visibleRows,
                                                 static_cast<uint16_t>(items.size()));

  for (uint16_t row = 0; row < visibleRows; ++row) {
    const size_t index = static_cast<size_t>(topIndex) + row;
    if (index >= items.size()) break;
    const fui::Rect rowRect{listRect.x, static_cast<int16_t>(listRect.y + row * kListRowHeight), listRect.width,
                            kListRowHeight};
    const bool selected = static_cast<int>(index) == clampedSelected;
    fui::TextStyle style;
    style.maxLines = 1;
    if (selected) {
      target_.fill(rowRect, fui::Paint::solid(fui::Color::Black));
      style.color = fui::Color::White;
    }
    target_.text(rowRect.inset(fui::Insets{4, 4, 4, 8}), items[index].c_str(), style);
  }

  presenter_.present();
}

}  // namespace transit
