# CONFIG_AND_STATE.md — every configurable value, today vs. NVS design

Every user-controllable value in both repos, how each persists it today, and
the proposed `ConfigStore` (ESP-IDF NVS) key for the firmware equivalent.

## Transit-NearbyWebWidget — URL hash, no server session

State lives entirely in `window.location.hash`, parsed by `hashChanged()`
in `public/js/widget/script.js`:

```
#<lat>,<lng>|<search text>|<param>=<value>|<param>=<value>...
```

Regex: `/(-?\d+\.\d+),(-?\d+\.\d+)\|?([^\|]*)(\|.+)?/g`

| Value | Source | Default if absent | Notes |
|---|---|---|---|
| `latitude`, `longitude` | hash segments 1–2 | `45.51485, -73.55965` (Montreal) — only used if hash is completely empty **and** no prior `context` values exist | Required pair; no independent default for one without the other |
| search text | hash segment 3 | empty | Cosmetic — repopulates the search input, e.g. after `locateClick()` sets it to "Current location" |
| `distance` | `data-distance` HTML attribute (server-rendered by `views/widget.jade`, not the hash) | **500** (meters) | Set once at page embed time via `<div data-distance="...">`, not user-editable at runtime |
| `filter` | `data-filter` HTML attribute | none | Passed through to `/api/nearby` as `filter` query param — not read by `routes/index.js`'s actual `nearby_routes` call, dead parameter in the current server code |
| `autoScroll` | hash param, `parseInt` | `0` (off) | Seconds per auto-scroll cycle |
| `autoCarousel` | hash param, `parseInt` | `0` (off) | Seconds per auto-advance-direction cycle |
| `staticDirection` | hash param, `parseInt` | `-1` (off — normal per-route toggling) | Pins every route to one itinerary index |
| `sortByTime` | hash param, `parseInt` | `0` / falsy (off) | See `UI_BEHAVIOR.md` |

The hash is reset (`autoScroll`/`autoCarousel`/`staticDirection` back to
their off defaults) on **every** `hashChanged()` call unless the new hash
explicitly sets them — i.e. these are not sticky across navigations, they
must be present in the URL every time to stay active.

**API key**: `process.env.API_KEY`, loaded server-side from a `.env` file
(`dotenv -e .env -o -- nodemon ./bin/www` in `package.json`'s `start`
script). `.env` is `.gitignore`d. Never touches the client.

## Transit-TV — browser cookie, no server session

Everything lives in a single cookie named `config`, JSON-serialized
(`ScreenConfig.save()` in `client/services/screenConfig/screenConfig.js`):

| Field | Default | Notes |
|---|---|---|
| `id` | `''` | Used by `duplicate()` to fork a new screen config from the current one (routes to a new state, empty id) |
| `title` | `''` | Cosmetic label for the screen, no display logic hooks on it in the reviewed code |
| `routeOrder` | `[]` | Array of `global_route_id`, manual drag-drop order — see `UI_BEHAVIOR.md` |
| `hiddenRoutes` | `[]` | Array of `global_route_id` to exclude from display |
| `latLng` | `{ latitude: 40.724029430769484, longitude: -74.00022736495859 }` | **Default is New York**, not Montreal (differs from the widget's Montreal default) — overwritten on first save |
| `timeFormat` | `'HH:mm'` | 24-hour by default; UI offers `'hh:mm A'` (12-hour) as the other option (`ConfigCtrl.timeFormats`) |
| `isEditing` | `true` (in-memory only) | **Deliberately excluded from the saved cookie** (`delete config.isEditing` in `save()`) — always resets to "not editing" on reload once a saved config exists |

Radius is **hardcoded to 1000 meters** in `nearby.controller.js`
(`Nearby.find(ScreenConfig.latLng, 1000)`) — not part of `ScreenConfig`, not
persisted, not user-editable through any UI. `FAQ.md` confirms this
explicitly ("Not through the UI... hardcoded at 1000 meters").

**API key**: `process.env.API_KEY`, read server-side in
`routes.controller.js`'s `nearby` handler. Locally supplied via
`server/config/local.env.js` (gitignored, not committed — the sample lives
at `server/config/local.env.sample.js` and does not itself carry a key
field, since `API_KEY` isn't part of that particular sample); in production
(per `DEPLOYING.md`) it's a platform environment variable (Railway
"Variables" tab). Never touches the client.

## Firmware `ConfigStore` (NVS) — proposed key mapping

| NVS key | Source of truth today | Type | Notes |
|---|---|---|---|
| `wifi_ssid`, `wifi_pass` | n/a (no web equivalent) | string | Entered via on-device setup (P5) |
| `api_key` | `.env` / platform env var in both repos | string | Entered via on-device setup, never compiled into source (see root `CLAUDE.md`) |
| `stop_id` (preferred) or `lat`/`lon` + `radius_m` | widget's hash lat/lng + `data-distance`; TV's `latLng` + hardcoded 1000m | string / double+double+int | Per the plan's v4 design note: prefer a fixed `global_stop_id` (picked once via `nearby_stops`/`search_stops` during setup) over a live radius search on every poll |
| `refresh_interval_min` | widget: fixed 30s; TV: fixed 20s (both hardcoded, not configurable) | int | **New as a user-facing setting** — neither existing app exposes this; default **60** (hourly), treated as a minimum rather than a target — see `DEPLOYMENT_OPS.md` |
| `sleep_window_start` / `sleep_window_end` | n/a (no web equivalent — both existing apps assume mains power and a screen that's always relevant) | time-of-day or absent | **New setting.** When set, `refresh_interval_min` is stretched (e.g. 2–4x, or polling paused entirely) between these times, since nobody's reading an always-on browser tab's overnight equivalent on a battery device either. Leave unset for a 24-hour-relevant display (a lobby, a shared space) — see `DEPLOYMENT_OPS.md` |
| `departure_window_min` | widget: fixed 90; TV: fixed 130 (both hardcoded) | int | Expose as a setting rather than picking one silently |
| `max_departures_per_direction` | widget: fixed 3 (template-enforced); TV: "up to 4" per FAQ (grid-enforced, API count effectively uncontrolled) | int | v4's real `max_num_departures` query param (1–10) makes this an honest, requestable value instead of a client-side trim |
| `sort_by_time` | widget's `sortByTime` hash flag | bool | |
| `static_direction` | widget's `staticDirection` hash flag | int (-1 = off) | |
| `hidden_routes[]` | TV's `hiddenRoutes` cookie array | array of `global_route_id` strings | |
| `route_order[]` | TV's `routeOrder` cookie array | array of `global_route_id` strings | |
| `time_format` | TV's `timeFormat` (`'HH:mm'` / `'hh:mm A'`) | enum | Cosmetic — the firmware shows countdown minutes, not clock time, in most views; keep if a clock-time view is ever added |
| `locale` | neither repo sets `locale`/`Accept-Language` today | string | New capability from v4, optional |
| `display_portrait` | n/a (no web equivalent — both existing apps run in a browser tab, not a fixed physical panel) | bool | **New setting.** `false` (default) = landscape, the X4 panel's native orientation; `true` = portrait (rotated 90°). Not part of the first-run wizard — changed via the settings portal (a long power-button hold at boot on an already-provisioned board — see `SetupFlow::runSettingsPortal()`) |
| `sta_stop` | n/a — a second, optional data source (Spokane Transit Authority), not in either reference app | string | **New setting.** The numeric stop code printed on a physical STA stop sign; empty (default) = STA departures off, only Transit API departures show. Resolved to STA's internal `stop_id`/display name via the baked-in `sta_stop_table.h` (see `tools/gen_sta_tables.py`). Also settings-portal-only, like `display_portrait` — see `docs/STA_INTEGRATION.md` for the data source itself |
| `home_legs`, `work_legs` | n/a — neither reference app has a trip-planning/transfer concept | string | **New setting.** A preset's ordered route-chain legs (e.g. "31 → 32 → 97"), settings-portal-only — see `docs/TRIP_PLANNER.md`. Empty (default) = that preset not configured. Each leg is `routeId,boardStopId,alightStopId,directionId` joined with `,`; legs joined with `\|` |
| `home_walk_min`, `work_walk_min` | n/a | int | **New setting.** Minutes to walk to that preset's first leg's boarding stop; default **0**. Subtracted from the first leg's departure time to compute the preset's "leave by" time |
| `xfer_buf_min` | n/a | int | **New setting.** Shared minimum minutes between an estimated transfer arrival and the next leg's departure, applied to both presets; default **3** |
| `focus_mode` | n/a — neither reference app has an equivalent reduced-clutter mode | bool | **New setting.** `false` (default) = the normal multi-route board; `true` = only the 1-2 soonest departures, drawn larger — an ADHD-friendly reduced-clutter option, settings-portal-only |

NVS/Preferences key names are capped at 15 characters
(`NVS_KEY_NAME_MAX_SIZE` is 16, including the null terminator) — a key
longer than that compiles fine but fails at runtime with
`ESP_ERR_NVS_KEY_TOO_LONG`. Several names above are conceptual/proposed
rather than the literal on-disk key; `src/transit/config_store.cpp` is the
source of truth for actual key strings, and abbreviates where the proposed
name is too long: `refresh_interval_min` → `refresh_int_min`,
`sleep_window_start`/`sleep_window_end` → `sleep_win_start`/`sleep_win_end`,
`departure_window_min` → `dep_win_min`, `max_departures_per_direction` →
`max_dep_per_dir`, `static_direction` → `static_dir`, `display_portrait` →
`portrait`, `sta_stop` is already short enough to use as-is. `hidden_routes[]` /
`route_order[]` are each stored as one comma-joined string under
`hidden_routes` / `route_order` respectively (NVS has no native array
type) — see the accessors' comments in `config_store.cpp`.

Deliberately **not** ported: `data-filter` (dead parameter in the widget's
own server code — passed through but never read by the actual API call),
`id`/`title`/`duplicate()` (Transit-TV's multi-screen-config forking has no
analog on a single physical device), `autoScroll`/`autoCarousel` (kiosk
animation, meaningless on e-ink — see `UI_BEHAVIOR.md`).
