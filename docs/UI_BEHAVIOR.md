# UI_BEHAVIOR.md — refresh, filtering, sorting, and badge rules

Behavior only — extracted from the running code of both repos
(`public/js/widget/script.js` + `*.dot` templates for the widget;
`nearby.controller.js`, `nearby.service.js`, `screenConfig.js`,
`routeItem.directive.js`/`.html` for Transit-TV, corroborated by
`FAQ.md`). This is the spec the firmware's refresh scheduler and render
rules are built against — no data-shape details here, see `DATA_MODEL.md`
for those.

## Refresh cadence

| | Transit-NearbyWebWidget | Transit-TV |
|---|---|---|
| Data re-fetch | Every **30s**, via a `setTimeout` chain re-armed at the top of `loadNearby()` (`context.refresh`) | Every **20s**, unconditionally, via `$interval` in `nearby.controller.js` — no backoff, no visibility check |
| Countdown re-render | Every **3s**, client-side only (`updateTime()`, no network call) — recomputes "X minutes" labels from already-fetched `departure_time` values | None — Transit-TV re-renders only on the 20s poll; per-minute countdown freshness comes entirely from the next full poll |
| Extra triggers | Hash change (user navigates/searches) re-fetches immediately, replacing the pending timeout | `$rootScope` events `locationChanged` / `nearbyChanged` (fired when settings are saved) trigger an immediate re-fetch outside the interval |

**Firmware implication:** neither existing behavior maps directly onto a
battery-powered device — both assume constant power and a screen that
repaints for free. The plan's answer (translate 20–30s polling into a
user-configurable multi-minute interval, decoupled from any local
countdown) is the right one; there is no "faithful port" of a 20-second
poll loop onto a 2-week-battery e-ink device, full stop.

## Departure window (cutoff for "upcoming")

| | Cutoff | Where enforced |
|---|---|---|
| Widget | **90 minutes** | Client-side in `updateTime()`: `if (time > now && min <= 90)`; anything outside the window has its DOM node removed (`.remove()`) on every 3s tick |
| Transit-TV | **130 minutes** | Client-side in `nearby.service.js`'s `shouldShowDeparture(departure)`: `diff > 0 && diff <= 130 * 60000` |

Both are pure client-side filters — v3's `nearby_routes` (and v4's,
per `API_CONTRACT.md`) doesn't take a "window" parameter; it returns
`max_num_departures` per direction regardless of how far out they are, and
each app trims the list itself. The firmware should do the same: fetch,
then apply a configured cutoff (recommend defaulting near the plan's
"~90–130 min" range, exposed as a setting) rather than trusting the API's
`max_num_departures` alone to bound "upcoming."

## Departures shown per direction

- **Widget**: top **3** per direction, enforced in the doT template
  (`routes_tmpl.dot`: `{{? i < 3}}` inside the `schedule_items` loop) — the
  API itself is not asked to limit this (no `max_num_departures` param sent
  in v3; the template just stops rendering after 3).
- **Transit-TV**: FAQ.md states **up to 4** per direction. The template
  (`routeItem.html`) renders one `ng-repeat` bound to the filtered/live
  items, followed by **3 hardcoded blank placeholder slots** — that's a
  fixed 4-slot layout grid, not proof the API is asked for exactly 4; the
  server passthrough (`routes.controller.js`) forwards only `lat`, `lon`,
  `max_distance` to the v3 API, so the effective count is whatever v3's
  default is, trimmed to 4 renderable slots by the CSS grid.

**Firmware implication:** v4 makes this configurable and honest —
`max_num_departures` (1–10) is a first-class query parameter on both
`nearby_routes` and `stop_departures` (see `API_CONTRACT.md`). Request
exactly what the panel layout needs instead of over-fetching and trimming
client-side.

## Sorting

- **Widget** — two independent, opt-in behaviors, both off unless the
  corresponding URL-hash flag is set (see `CONFIG_AND_STATE.md`):
  - `staticDirection=<index>` pins every route to a specific itinerary/direction
    index (`route.current_itinerary_index = staticDirection`) instead of
    defaulting to index 0 — useful for a kiosk that should only ever show
    one direction (e.g. "outbound only").
  - `sortByTime=1` re-sorts the **routes array itself** by each route's
    current itinerary's earliest still-upcoming `departure_time` — routes
    with no upcoming departure in the current itinerary sort as if tied
    (comparator returns `0`), so their position is otherwise API order.
  - Neither flag is on by default; unconfigured, routes render in whatever
    order the API returned.
- **Transit-TV** — routes are sorted by a **user-defined manual order**
  (`ScreenConfig.routeOrder`, an array of `global_route_id`), maintained via
  drag-and-drop in the on-screen config UI (`onChangeOrder()`). Routes not
  yet in `routeOrder` sort after those that are, in their original relative
  order; there is no time-based auto-sort in Transit-TV at all.

**Firmware implication:** both are worth keeping as independent, exclusive
config options — "sort by soonest departure" (widget's `sortByTime`) and
"user-defined per-route order" (TV's drag-and-drop) serve different use
cases (a shared kiosk vs. a personal board), and neither app tries to
combine them.

## Route hide/reorder (Transit-TV only)

- `ScreenConfig.hiddenRoutes`: array of `global_route_id` a user chose to
  hide via the on-screen settings UI (`hide-route` button per row). A
  route is filtered from view (`isShown()`) if its id is in this list, or if
  `Nearby.hasShownDeparture(route)` is false (i.e., **every** direction's
  departures are all outside the 130-minute window) — a route with nothing
  upcoming disappears from the board entirely, it does not render an empty
  card.
- Re-ordering is drag/drop (jQuery UI `jqyoui`); the resulting order is
  persisted the same way as hiding (cookie, see `CONFIG_AND_STATE.md`).
- The widget has no equivalent — every route the API returns is always
  shown, in whatever order sorting rules (above) produce.

## De-duplication

- **Transit-TV only**: `nearby.service.js`'s `filterRoutes()` drops any
  route whose `global_route_id` has already been seen in the same response
  (keeping the first occurrence, logging the dropped duplicate to
  console). The widget performs no such de-dup. Worth porting — a v4
  `nearby_routes` response can plausibly repeat a `global_route_id` across
  overlapping feed boundaries.

## Badges

Identical rules in both repos, driven by fields unchanged in v4 (see
`DATA_MODEL.md`):

- **Real-time**: `schedule_item.is_real_time === true` → small icon/badge
  next to the time (`<i class="realtime">` / `<i ng-show="item.is_real_time">`).
  No text label in either app, icon-only.
- **Last departure of the day**: `schedule_item.is_last === true` → the
  trailing unit label switches from `"min"`/`"minutes"` to `"last"`
  (`small.last` class). Applies per-departure, not per-route.
- **Cancelled** (`is_cancelled`, new in v4): neither existing app has a rule
  for this because it didn't exist in v3. Per `DATA_MODEL.md`'s
  recommendation, treat it as a filter (drop the item, or request
  `remove_cancelled=true` server-side on `stop_departures`), not a badge.

## Autocomplete (Transit-NearbyWebWidget only; Transit-TV has no search UI)

- Debounced **300ms** after the last keystroke (`setTimeout(..., 300)` in
  `setupAutocomplete()`), only fires if the search field is non-empty.
  Cancelled/superseded by any subsequent keystroke (`clearTimeout(delayer)`).
- Up/Down arrow keys move a `#selected` marker through the suggestion list
  without triggering a new request; Enter clicks the currently-selected
  suggestion; Escape closes the dropdown.
  Results are sorted client-side by `probability` descending
  (v4: `match_strength`, per `API_CONTRACT.md`) before rendering — the API's
  own ordering is not trusted as-is.
- Not relevant to the firmware's runtime UI (no free-text keyboard input on
  a 7-button e-ink device), but directly relevant to the **on-device setup
  flow** (`nearby_stops`/`search_stops` picker mentioned in the plan) —
  the 300ms-debounce-plus-client-resort pattern is reasonable to keep for a
  button-driven incremental search, if the setup UI ever supports typing a
  stop name via an on-screen keyboard.

## Kiosk-style display params (widget only, see `CONFIG_AND_STATE.md`)

`autoScroll=<seconds>` and `autoCarousel=<seconds>` both animate the widget
without user input (auto-scroll the route list; auto-advance the visible
direction per route on a timer) — purely presentational, not applicable to
a static e-ink panel that redraws on its own schedule rather than
animating continuously.
