# API_CONTRACT.md — Transit API v4

Annotated contract for the four v4 endpoints the firmware needs. Source: the
live OpenAPI 3.1 spec embedded in https://api-doc.transitapp.com/v4.html
(`Transit API (Stable)`, version `4.0.0`), fetched and cross-checked field by
field — not reverse-engineered from the v3 calls in Transit-NearbyWebWidget
or Transit-TV. Those two repos call `/v3/public/nearby_routes` and
`/v3/public/search_stops` only; `stop_departures` and `nearby_stops` are new
in v4 and have no v3 equivalent in either repo.

## Base URL, auth, locale

- Base host: `https://external.transitapp.com`
- Auth: header `apiKey: <your key>` (OpenAPI `securitySchemes.apiKey`, type
  `apiKey`, `in: header`). No query-param auth exists.
- Locale: pass `locale=fr,en` (query) or an `Accept-Language` header with the
  same comma-separated-preference syntax. `locale` wins if both are present.
  Neither repo sets either today — all responses come back in the feed's
  default language.

## `GET /v4/public/nearby_routes`

Direct v3 equivalent of what both existing repos call. Returns every route
with service near a point, each carrying its own directions and next
departures — a single request covering an entire area.

**Query parameters**

| Param | Type | Default | Notes |
|---|---|---|---|
| `lat` *(required)* | double | — | |
| `lon` *(required)* | double | — | |
| `max_distance` | int | 150 | **max 1500** — meters. Both repos pass 500 (widget default) or 1000 (TV, hardcoded) explicitly; without it you get a 150m radius, not "no limit". |
| `max_num_departures` | int | 3 | min 1, max 10 — departures returned **per merged itinerary** (i.e. per direction), not per route. |
| `should_update_realtime` | bool | true | Set false to force schedule-only data (fewer round trips through the realtime layer server-side). |
| `merge_platform_stops` | bool | false | true = one merged itinerary per direction even if a station has multiple platform stops. |
| `time` | epoch int | now | Query departures as of a specific time instead of now. |
| `include_stops_and_shapes` | bool | false | true adds full `stops[]` + polyline `shape` to every itinerary — response size increase, unnecessary for a departure board. Leave false. |
| `stop_detailed` | bool | false | Only matters when `include_stops_and_shapes=true`. |
| `locale` | string | feed default | See above. |

**Response** — `{ "nearby_routes": [ Route, ... ] }`. See `DATA_MODEL.md` for
the full `Route` → `merged_itineraries` → `schedule_items` shape; the key
structural fact is that `itineraries` and `schedule_items` are now **siblings**
inside each `merged_itineraries[]` entry, cross-referenced by
`internal_itinerary_id` — not nested one inside the other as in v3.

Errors: `400` (bad/missing lat or lon), `500` (internal error, empty result).

## `GET /v4/public/stop_departures` — new in v4, no v3 equivalent

Departures for one or more **known stops**, addressed by `global_stop_id`.
No radius search — this is the endpoint the plan recommends the firmware
use once a stop has been picked during setup, since it skips the "search a
radius, then filter" round trip entirely.

**Query parameters**

| Param | Type | Default | Notes |
|---|---|---|---|
| `global_stop_ids` *(required)* | comma-separated string | — | Up to 100 stop IDs, e.g. `1:82774,2:54321`. (`global_stop_id`, singular, is documented as an alternative for a single stop but does not appear as its own parameter in the spec — pass a one-element list to `global_stop_ids`.) |
| `time` | epoch number | now | |
| `remove_cancelled` | bool | false | Drop `is_cancelled` items server-side instead of filtering client-side. |
| `exclude_terminal_arrivals` | bool | false | Drops schedule items where the queried stop is the trip's final stop (arrival-only, can't be boarded toward the shown headsign). Relevant for a fixed stop board; irrelevant to `nearby_routes`, which doesn't expose it. |
| `should_update_realtime`, `merge_platform_stops`, `max_num_departures`, `include_stops_and_shapes`, `stop_detailed`, `locale` | — | — | Same meaning as `nearby_routes` above. |

**Response** — `{ "route_departures": [ RouteDeparture, ... ] }`. Each entry is
the same `Route`-shaped object as `nearby_routes`' `nearby_routes[]`, **plus**
a `global_stop_id` field identifying which queried stop this entry's
departures belong to (needed because one call can cover up to 100 stops at
once). `alerts`, `merged_itineraries`, etc. are otherwise identical in shape.

Errors: `400` (invalid stop id, missing required params, or no result).

## `GET /v4/public/nearby_stops` — new in v4

Stops (not routes) near a coordinate — used during first-run setup to let
the user pick the exact stop `stop_departures` will poll afterward.

**Query parameters**

| Param | Type | Default | Notes |
|---|---|---|---|
| `lat`, `lon` *(required)* | double | — | |
| `max_distance` | int | 150 | max 1500 |
| `stop_filter` | enum | `Routable` | `Routable` (location_type 0, has service) / `EntrancesAndStopsOutsideStations` / `Entrances` (location_type 2) / `Any`. Setup flow wants `Routable`. |
| `pickup_dropoff_filter` | enum | — | `PickupAllowedOnly` / `DropoffAllowedOnly` / `Everything`. |
| `stop_detailed` | bool | false | true → `StopDetailed` (adds `stop_timezone`, `network_id`, `network_name`, `tts_stop_name`). |
| `include_beta_feeds` | bool | false | |
| `locale` | string | feed default | |

**Response** — `{ "stops": [ Stop | StopDetailed, ... ] }`.

Errors: `400`, `500`.

## `GET /v4/public/search_stops`

Text search for a stop by name/code near an approximate area — same job as
v3, used by both repos' autocomplete. The response shape changed slightly.

**Query parameters**

| Param | Type | Notes |
|---|---|---|
| `lat`, `lon` | number | Approximate search area center. |
| `query` | string | Matched against `stop_name` and `stop_code`. |
| `pickup_dropoff_filter` | enum | Same enum as `nearby_stops`. |
| `max_num_results` | int | 1–50. |
| `max_distance` | int | Optional hard cutoff in meters; unset = no distance filtering. |
| `merge_similar_stops` | bool | Merge a station's platforms into one result. |
| `locale` | string | |

**Response** — `{ "results": [ { distance, global_stop_id, location_type,
match_strength, route_type, stop_lat, stop_lon, stop_name }, ... ] }`.

v3 → v4 field diff for this endpoint: `probability` (0–1 ranking) is renamed
to `match_strength` (same 0–1 scale, same "higher is better" meaning — both
existing repos sort by this field, `showSuggestions()` in
`public/js/widget/script.js` sorts by `probability` descending). `distance`
(meters from the query point) is new; neither repo currently re-sorts by it,
but the field is available for a firmware picker that wants nearest-first.

Errors: `400` (lat, lon, or query missing).

## Shared component reference

These are referenced by `$ref` across all four endpoints above — full field
lists are in `DATA_MODEL.md`, this is just the index:

| Schema | Used by |
|---|---|
| `GlobalRouteId`, `GlobalStopId` | opaque stable string IDs, e.g. `"1:897"`, `"1:94380"` — stable across GTFS updates on a best-effort basis |
| `DisplayShortName` | `route_display_short_name` / `compact_display_short_name` — see `ASSETS_ICONS.md` |
| `Fare` / `Price` | `route.fares[]` — unused by either existing repo |
| `ServiceAlert` | `route.alerts[]` — new in v4, unused by either existing repo today |
| `Stop` / `StopDetailed` | `merged_itineraries[].closest_stop`, `nearby_stops`/`search_stops` results |
| `ItineraryWithInternalId` | `merged_itineraries[].itineraries[]` |
| `ScheduleItemWithInternalId` | `merged_itineraries[].schedule_items[]` |
| `Vehicle` | `route.vehicle` — `{ name, name_inflection, image }`, unused by either existing repo |

## Endpoints intentionally out of scope

The v4 spec also documents trip planning (`/v4/public/plan`,
`estimate_plan_duration`), network/placemark/vehicle-share discovery, and
detail lookups (`route_details`, `stop_details`, `trip_details`,
`schedule_for_dates`, etc.). None of these are used by
Transit-NearbyWebWidget or Transit-TV, and none are needed to reproduce their
behavior — they're excluded here on purpose, not by oversight.
