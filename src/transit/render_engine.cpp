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
// Row height is a range, not a fixed value: renderDepartureBoard() stretches
// rows to fill the body area between the header and the attribution footer,
// so a stop with few routes doesn't leave a dead gap of blank screen below
// the last row. kMinRowHeight is also what bounds how many rows fit at all
// (more routes than that just don't get drawn, same as before); kMaxRowHeight
// keeps a 1-2-route board from stretching into absurdly tall rows.
constexpr int16_t kMinRowHeight = 74;
constexpr int16_t kMaxRowHeight = 140;
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

// Compact "how old is this" label for cached data (BoardStatus::cachedAgeMin).
// Minutes below an hour, then whole hours, then whole days -- a board that
// has been offline for three days should say so at a glance rather than
// printing "4320m ago". ASCII only, like every other on-device string: the
// bundled Noto Sans subset has no glyph for non-ASCII punctuation and
// renders a tofu box instead.
std::string formatCacheAge(int minutes) {
  // Negative means the caller restored a cache but has no clock to measure
  // its age against (BoardStatus::cachedAgeMin) -- say so rather than
  // rounding an unknown down to "just now".
  if (minutes < 0) return "(age unknown)";
  if (minutes < 1) return "just now";
  if (minutes < 60) return std::to_string(minutes) + "m ago";
  if (minutes < 60 * 24) return std::to_string(minutes / 60) + "h ago";
  return std::to_string(minutes / (60 * 24)) + "d ago";
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

// ADHD-friendly "leave now" urgency cue: departures 5 minutes or closer get
// a bold+outline treatment (see the chip-drawing loop below and
// drawFocusRow()) instead of a new gray fill -- a fill would visually
// collide with the real-time pill's own dithered LightGray background just
// above. nowEpoch<=0 (SNTP never synced this wake) means minutes-until
// can't be computed at all, so nothing is ever flagged urgent in that case
// -- matches formatDepartureChip()'s own fallback to a plain clock time.
bool isLeaveNowUrgent(const DepartureRow& dep, int64_t nowEpoch) {
  if (nowEpoch <= 0) return false;
  const int64_t minutes = (dep.departureTimeEpoch - nowEpoch) / 60;
  return minutes <= 5;
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

  // Wi-Fi / fetch-failure indicator, left of the battery glyph. "Offline"
  // rather than "No Wi-Fi" when there's cached data behind it: the board is
  // still showing real departures in that case, and "No Wi-Fi" next to a
  // full board reads like the board itself is broken.
  if (!status.wifiOk || status.lastFetchFailed) {
    // "Offline" only when the radio genuinely couldn't get on a network.
    // A fetch that failed with Wi-Fi up is a different problem -- a bad
    // key, a quota, a 5xx -- and saying "Offline" for it would throw away
    // the one diagnostic the header can give. The staleness itself is
    // already carried by the "Cached 2h ago" line next to this either way.
    const char* label;
    if (!status.wifiOk) {
      label = status.dataIsCached ? "Offline" : "No Wi-Fi";
    } else {
      label = "Fetch failed";
    }
    fui::TextStyle warn;
    warn.align = fui::TextAlign::Right;
    warn.maxLines = 1;
    warn.bold = true;
    const fui::Size sz = target.measureText(warn.font, label, warn);
    const fui::Rect rect{static_cast<int16_t>(cursorRight - sz.width),
                         static_cast<int16_t>((kHeaderHeight - sz.height) / 2), sz.width, sz.height};
    target.text(rect, label, warn);
    cursorRight = static_cast<int16_t>(rect.x - 10);
  }

  // Freshness, left of that. For a live fetch this is the usual
  // last-updated clock (the fetch/SNTP-sync time, not a live countdown --
  // this device deep-sleeps between wakes, see docs/UI_BEHAVIOR.md's
  // refresh cadence discussion). For restored cache it's how stale the data
  // is instead, which is the question a cached board actually raises -- the
  // clock time it was fetched at means much less than "2h ago" does. A
  // leading "~" marks a clock that came from time_keeper.h's approximate
  // RTC-memory clock rather than a real SNTP sync, so an estimate is never
  // presented as the exact time.
  std::string updated;
  if (status.dataIsCached) {
    updated = std::string("Cached ") + formatCacheAge(status.cachedAgeMin);
  } else {
    char clock[8];
    formatClock(status.lastUpdatedEpoch, clock, sizeof(clock));
    updated = std::string("Updated ") + (status.clockIsApproximate ? "~" : "") + clock;
  }
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
    const bool urgent = isLeaveNowUrgent(dep, nowEpoch);
    fui::TextStyle chipStyle;
    chipStyle.maxLines = 1;
    chipStyle.bold = urgent;
    const fui::Size sz = target.measureText(chipStyle.font, chip.c_str(), chipStyle);
    if (chipX + sz.width > chipRight) break;  // out of room; ui_logic already caps the count
    const fui::Rect chipRect{chipX, line2Rect.y, sz.width, line2Rect.height};
    if (dep.isRealTime) {
      // Dithered pill behind real-time chips -- the "small icon/badge next
      // to the time" UI_BEHAVIOR.md describes, without a bundled bolt asset.
      target.fill(chipRect.inset(fui::Insets{-2, -3, -2, -3}), fui::Paint::solid(fui::Color::LightGray), 3);
    }
    target.text(chipRect, chip.c_str(), chipStyle);
    if (urgent) {
      // Outline stroke, not a fill -- see isLeaveNowUrgent()'s comment on
      // why this can't reuse the RT pill's gray fill.
      target.stroke(chipRect.inset(fui::Insets{-2, -3, -2, -3}), fui::Paint::solid(fui::Color::Black), 1);
    }
    chipX = static_cast<int16_t>(chipX + sz.width + kChipGap);
  }
}

// --- Preset trip summary strip (BoardStatus::presetTrips) ------------------

constexpr int16_t kPresetLineHeight = 30;
constexpr int16_t kPresetNameColumnWidth = 70;

// Total height to reserve above the footer for the preset-trip strip --
// zero when there are no configured presets, so the departure-row layout
// below is pixel-identical to before this feature existed in that case.
int16_t presetStripHeight(const std::vector<BoardStatus::PresetTripSummaryLine>& lines) {
  return static_cast<int16_t>(lines.size() * kPresetLineHeight);
}

void drawPresetTripStrip(fui::DrawTarget& target, int16_t screenW, int16_t stripTop,
                         const std::vector<BoardStatus::PresetTripSummaryLine>& lines) {
  for (size_t i = 0; i < lines.size(); ++i) {
    const fui::Rect lineRect{kMargin, static_cast<int16_t>(stripTop + static_cast<int16_t>(i) * kPresetLineHeight),
                             static_cast<int16_t>(screenW - 2 * kMargin), kPresetLineHeight};

    fui::TextStyle nameStyle;
    nameStyle.bold = true;
    nameStyle.maxLines = 1;
    const fui::Rect nameRect{lineRect.x, lineRect.y, kPresetNameColumnWidth, lineRect.height};
    target.text(nameRect, lines[i].presetName.c_str(), nameStyle);

    const int16_t textX = static_cast<int16_t>(lineRect.x + kPresetNameColumnWidth + 8);
    const int16_t textW = static_cast<int16_t>(lineRect.width - kPresetNameColumnWidth - 8);
    fui::TextStyle textStyle;
    textStyle.maxLines = 1;
    textStyle.bold = lines[i].leaveNow;
    const fui::Rect textRect{textX, lineRect.y, textW, lineRect.height};
    target.text(textRect, lines[i].text.c_str(), textStyle);
    if (lines[i].leaveNow) {
      target.stroke(textRect.inset(fui::Insets{-2, -4, -2, -4}), fui::Paint::solid(fui::Color::Black), 1);
    }
  }
}

// --- Focus mode: 1-2 much larger rows instead of the full board ------------

constexpr int kFocusMaxBoards = 2;
constexpr int16_t kFocusRowGap = 16;

// The boards with the soonest upcoming departure, up to maxCount -- ignores
// the caller's routeOrder/sortByTime ordering entirely (focus mode's point
// is "just the next relevant thing," see render_engine.h's setFocusMode()
// comment). ui_logic::buildDepartureBoard() never emits a DirectionBoard
// with an empty departures list in practice, but this filters them out
// defensively anyway (rather than trusting that invariant) since dir-
// ->departures.front() below would otherwise be undefined behavior.
std::vector<const DirectionBoard*> selectFocusBoards(const std::vector<DirectionBoard>& board,
                                                     int maxCount) {
  std::vector<const DirectionBoard*> sorted;
  sorted.reserve(board.size());
  for (const auto& b : board) {
    if (!b.departures.empty()) sorted.push_back(&b);
  }
  std::stable_sort(sorted.begin(), sorted.end(), [](const DirectionBoard* a, const DirectionBoard* b) {
    return a->departures.front().departureTimeEpoch < b->departures.front().departureTimeEpoch;
  });
  if (static_cast<int>(sorted.size()) > maxCount) sorted.resize(static_cast<size_t>(maxCount));
  return sorted;
}

void drawFocusRow(fui::DrawTarget& target, const fui::Rect& rowRect, const DirectionBoard& dir,
                  int64_t nowEpoch) {
  const DepartureRow& nextDep = dir.departures.front();

  const int16_t labelHeight = static_cast<int16_t>(rowRect.height * 4 / 10);
  fui::TextStyle labelStyle;
  labelStyle.align = fui::TextAlign::Center;
  labelStyle.bold = true;
  labelStyle.maxLines = 2;
  const std::string label = dir.routeShortName + "  " + nextDep.headsign;
  const fui::Rect labelRect{rowRect.x, rowRect.y, rowRect.width, labelHeight};
  target.text(labelRect, label.c_str(), labelStyle);

  const fui::Rect chipRect{rowRect.x, static_cast<int16_t>(rowRect.y + labelHeight), rowRect.width,
                           static_cast<int16_t>(rowRect.height - labelHeight)};
  const std::string chip = formatDepartureChip(nextDep, nowEpoch);
  fui::TextStyle chipStyle;
  chipStyle.align = fui::TextAlign::Center;
  chipStyle.bold = true;
  chipStyle.maxLines = 1;
  target.text(chipRect, chip.c_str(), chipStyle);
  if (isLeaveNowUrgent(nextDep, nowEpoch)) {
    target.stroke(chipRect.inset(fui::Insets{4, 24, 4, 24}), fui::Paint::solid(fui::Color::Black), 2);
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

void RenderEngine::setScreenSize(int16_t screenWidth, int16_t screenHeight) {
  screenWidth_ = screenWidth;
  screenHeight_ = screenHeight;
}

void RenderEngine::setFocusMode(bool enabled) { focusMode_ = enabled; }

void RenderEngine::renderDepartureBoard(const std::vector<DirectionBoard>& board, const BoardStatus& status) {
  target_.fill(fui::Rect{0, 0, screenWidth_, screenHeight_}, fui::Paint::solid(fui::Color::White));

  drawStatusHeader(target_, screenWidth_, status);

  const int16_t footerHeight = attributionFooterHeight();
  const int16_t presetStripH = presetStripHeight(status.presetTrips);
  const int16_t bodyTop = static_cast<int16_t>(kHeaderHeight + 8);
  // Reserve the footer strip (plus its own kMargin gap above it) and, when
  // any presets are configured, the preset-trip strip above that, below the
  // last row so neither is ever crowded or covered. presetStripH is 0 when
  // status.presetTrips is empty, making this identical to the pre-preset-
  // trip layout in that case.
  const int16_t bodyBottom =
      static_cast<int16_t>(screenHeight_ - kMargin - footerHeight - presetStripH);
  const int16_t bodyHeight = static_cast<int16_t>(bodyBottom - bodyTop);
  const int maxRows = std::max(0, (bodyHeight + kRowGap) / (kMinRowHeight + kRowGap));

  if (board.empty()) {
    fui::TextStyle empty;
    empty.align = fui::TextAlign::Center;
    empty.maxLines = 3;
    const fui::Rect emptyRect{kMargin, bodyTop, static_cast<int16_t>(screenWidth_ - 2 * kMargin), bodyHeight};
    // Distinguish "the network is fine, this stop just has nothing coming"
    // from the two offline cases, which need completely different action
    // from the reader: a cached board that has aged out entirely is not the
    // same situation as never having had data to cache.
    const char* message;
    if (status.dataIsCached) {
      // main.cpp sets dataIsCached whenever a cache was restored, even if
      // pruning then emptied it -- which is the only way this case can be
      // reached, and is what makes the distinction below meaningful.
      message = "Every cached departure has already left, and there's no network to refresh.";
    } else if (!status.wifiOk || status.lastFetchFailed) {
      message = "Offline, and nothing cached yet to fall back on.";
    } else {
      message = "No departures to show.";
    }
    target_.text(emptyRect, message, empty);
  } else if (focusMode_) {
    const std::vector<const DirectionBoard*> focusBoards = selectFocusBoards(board, kFocusMaxBoards);
    const int rowsToDraw = static_cast<int>(focusBoards.size());
    const int16_t rowHeight = rowsToDraw > 0
        ? static_cast<int16_t>((bodyHeight - (rowsToDraw - 1) * kFocusRowGap) / rowsToDraw)
        : bodyHeight;
    for (int i = 0; i < rowsToDraw; ++i) {
      const fui::Rect rowRect{kMargin, static_cast<int16_t>(bodyTop + i * (rowHeight + kFocusRowGap)),
                              static_cast<int16_t>(screenWidth_ - 2 * kMargin), rowHeight};
      drawFocusRow(target_, rowRect, *focusBoards[static_cast<size_t>(i)], status.lastUpdatedEpoch);
    }
  } else {
    const int rowsToDraw = std::min<int>(maxRows, static_cast<int>(board.size()));

    // Stretch rows to actually fill the body area (clamped to
    // [kMinRowHeight, kMaxRowHeight]) instead of leaving unused screen space
    // below the last row when there are fewer routes than maxRows fits.
    const int16_t idealRowHeight =
        rowsToDraw > 0 ? static_cast<int16_t>((bodyHeight - (rowsToDraw - 1) * kRowGap) / rowsToDraw)
                       : kMinRowHeight;
    const int16_t rowHeight = std::min(kMaxRowHeight, std::max(kMinRowHeight, idealRowHeight));
    // If the clamp left the row block shorter than the body area (few
    // routes, each already at kMaxRowHeight), center the block rather than
    // pinning it to the top with all the leftover space stranded at the
    // bottom.
    const int16_t blockHeight = static_cast<int16_t>(rowsToDraw * rowHeight + (rowsToDraw - 1) * kRowGap);
    const int16_t blockTop = static_cast<int16_t>(bodyTop + std::max<int16_t>(0, (bodyHeight - blockHeight) / 2));

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
      const fui::Rect rowRect{kMargin, static_cast<int16_t>(blockTop + i * (rowHeight + kRowGap)),
                              static_cast<int16_t>(screenWidth_ - 2 * kMargin), rowHeight};
      drawDirectionRow(target_, iconCache_, rowRect, board[i], status.lastUpdatedEpoch, collision);
    }
  }

  if (!status.presetTrips.empty()) {
    drawPresetTripStrip(target_, screenWidth_, bodyBottom, status.presetTrips);
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
