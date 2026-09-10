# DATA_MODEL.md — canonical v4 data shape for the firmware

One schema, written from the v4 OpenAPI spec, that reconciles what
Transit-NearbyWebWidget and Transit-TV each partially display out of the
older v3 shape. Neither repo's source is authoritative for structure —
both are behavioral references only (which fields they read, how they use
them). Use this doc, not `script.js` or `nearby.service.js`, when defining
the C++ structs in `TransitApiClient`/`DepartureModel`.

## Why v3-shaped reading of the source would mislead you

v3's `GET /v3/public/nearby_routes` (what both repos call today) returns a
top-level `routes` array where each route has `itineraries[]` directly, and
each itinerary has its own `schedule_items[]` nested inside it — a strict
tree. v4 restructures this:

```
v3:  route.itineraries[i].schedule_items[j]              (strict tree)

v4:  route.merged_itineraries[k] = {
       direction_id,
       closest_stop,
       itineraries:     [ Itinerary, ... ],       // metadata per physical itinerary
       schedule_items:  [ ScheduleItem, ... ]      // departures, siblings of itineraries
     }
     schedule_item.internal_itinerary_id  →  itineraries[n].internal_itinerary_id
```

`itineraries[]` and `schedule_items[]` are **siblings** under
`merged_itineraries[]`, not parent/child. A schedule item doesn't belong to
"the itinerary it's nested in" anymore — it points at one via
`internal_itinerary_id`, a string like `"3:65535:false"` that's only stable
within a single response (don't persist it).

In practice, for a departure board that just needs "headsign + next N
times per direction," you can usually ignore the cross-reference: each
`merged_itineraries[]` entry already carries one `direction_id`, one
`closest_stop`, and (if `canonical_itinerary` is what you want) one
representative headsign — `itineraries[0].merged_headsign` covers the
common case where a direction has a single itinerary. Only walk the
`internal_itinerary_id` link if you need to show which physical branch
(`branch_code`) a specific upcoming departure runs.

## Canonical structs

### `Route` (from `nearby_routes[]` or `route_departures[]`)

| Field | Type | Notes |
|---|---|---|
| `global_route_id` | string | Stable ID, e.g. `"1:897"` |
| `route_short_name` | string | e.g. `"55"` — what v3 called `route.short_name` |
| `route_long_name` | string | **New in v4** — no v3/existing-repo equivalent. Optional to display; useful as an accessibility label. |
| `route_type` | int | GTFS route type (0=tram, 1=subway, 2=rail, 3=bus, 4=ferry, …) |
| `route_color` / `route_text_color` | hex string, no `#` | Same as v3 — quantize per `ASSETS_ICONS.md` |
| `route_display_short_name` | `DisplayShortName` | See below and `ASSETS_ICONS.md` |
| `route_network_name` / `route_network_id` | string | Which transit agency/network this route belongs to — useful when a location has overlapping networks |
| `mode_name` / `mode_key` | string | e.g. `"Bus"` / stable key version |
| `vehicle` | `{name, name_inflection, image}` | Unused by both existing repos; optional |
| `alerts[]` | `ServiceAlert[]` | **New in v4** — neither repo surfaces these; see below for whether the firmware should |
| `fares[]` | `Fare[]` | Unused by both existing repos; skip |
| `merged_itineraries[]` | array | The actual departure data — see next section |
| `global_stop_id` | string | **Only present on `stop_departures`' `route_departures[]`**, not on `nearby_routes[]` — identifies which queried stop this entry's departures are from |

### `MergedItinerary` (`route.merged_itineraries[]`)

| Field | Type | Notes |
|---|---|---|
| `direction_id` | int | 0 or 1, from GTFS `direction_id` |
| `closest_stop` | `Stop` | The stop this direction's departures are measured from. Widget/TV display `closest_stop.stop_name` as the secondary line under the headsign. |
| `itineraries[]` | `ItineraryWithInternalId[]` | Metadata per physical branch of this direction — `headsign`, `merged_headsign`, `branch_code`, `internal_itinerary_id` |
| `schedule_items[]` | `ScheduleItemWithInternalId[]` | The actual departures, each referencing one entry in `itineraries[]` |

### `Itinerary` (`merged_itineraries[].itineraries[]`)

| Field | Type | Notes |
|---|---|---|
| `internal_itinerary_id` | string | Cross-reference key, response-scoped only |
| `direction_id` | int | |
| `headsign` | string | From GTFS `trip_headsign`/`stop_headsign` |
| `direction_headsign` | string | Compass-style, e.g. `"Northbound"` |
| `merged_headsign` | string | `headsign` + `direction_headsign` combined for display, e.g. `"Northbound to Downtown"` — **prefer this field for the on-panel direction label**, it's what Transit-TV's `routeItem.html` binds to (`dir.merged_headsign`) |
| `branch_code` | string | e.g. `"A"`/`"B"` for a branching route; empty string if none |
| `canonical_itinerary` | bool | Whether this is the "primary" itinerary for the direction when more than one exists |
| `is_active` | bool | Has active trips in next 24h |

### `ScheduleItem` (`merged_itineraries[].schedule_items[]`)

| Field | Type | Notes |
|---|---|---|
| `internal_itinerary_id` | string | Which `Itinerary` this departure belongs to |
| `departure_time` | epoch number | **What both existing repos render.** Equal to `scheduled_departure_time` when not real-time. |
| `scheduled_departure_time` | epoch number | Static-schedule time, ignoring real-time — **new in v4**, no v3/existing-repo read of this field. Useful if you ever want to show "X min late." |
| `arrival_time` / `scheduled_arrival_time` | epoch number | Not read by either existing repo |
| `is_real_time` | bool | Drives the "RT"/lightning-bolt badge in both repos |
| `is_cancelled` | bool | **New in v4** — no existing-repo handling. The API doc's own guidance: cancelled departures "should either be crossed off or not shown at all." Recommend the firmware filters these out, matching the newer `remove_cancelled=true` query option on `stop_departures`. |
| `is_last` | bool | Drives the "last" badge in both repos (`item.is_last ? 'last' : 'min'`) — unchanged meaning from v3 |
| `wheelchair_accessible` | int (0/1/2) | **New in v4** — unused by either repo, optional to surface |
| `rt_trip_id` | string | Not used by either existing repo for display; can dedupe same-trip duplicates if ever needed |

### `DisplayShortName` (`route.route_display_short_name`)

```
{
  "elements": [ leftImageSlug | null, text, rightImageSlug | null ],  // always exactly 3
  "route_name_redundancy": bool,   // true = hide `text` if showing an image already conveys it
  "boxed_text": string             // extra text in a colored box, rare
}
```

Unchanged from v3 in shape and in the image-slug-to-URL convention — see
`ASSETS_ICONS.md` for the full mono/color-light/color-dark suffix rules.
`compact_display_short_name` is the same shape, intended for tighter layouts
(smaller UI real estate) — worth using over `route_display_short_name` on
an e-ink panel where space is scarce, though neither existing repo
distinguishes the two (both only ever read `route_display_short_name`).

### `Stop`

| Field | Type | Notes |
|---|---|---|
| `global_stop_id` | string | Stable ID — this is the value to store in `ConfigStore` and pass to `stop_departures`' `global_stop_ids` |
| `stop_name` | string | |
| `stop_lat` / `stop_lon` | double | |
| `distance` | number | Meters from the query point — only present on location-based queries (`nearby_stops`, `search_stops`), not on `route_details` |
| `wheelchair_boarding` | int (0/1/2) | |
| `parent_station` | object or absent | Present only if the stop is inside a station |

### `ServiceAlert` — new in v4, not displayed by either existing repo

`{ effect, cause, severity, title, description, created_at, informed_entities[], active_periods[] }`.
`severity` is the field worth surfacing first if the firmware adds alert
support later: `"Severe"` specifically means "service suspended" per the
spec's own description, distinct from GTFS-RT's normal severity meaning.
Not required for P0–P4 of the roadmap; flagged here so it's a deliberate
decision to skip, not an oversight.

## What each existing repo actually reads (for cross-check)

- **Transit-NearbyWebWidget** (`routes_tmpl.dot`): `route.route_display_short_name`,
  `route.short_name`, `route.route_color`, `route.route_text_color`,
  `route.itineraries[]` → `dir.merged_headsign || dir.headsign`,
  `dir.closest_stop.stop_name`, `dir.schedule_items[].departure_time`,
  `.is_real_time`, `.is_last`, `.branch_code` (first schedule item's only).
  This is the v3 shape — map each v3 field one-to-one onto the v4 fields
  documented above (same names, different nesting).
- **Transit-TV** (`routeItem.html`, `nearby.service.js`): identical field set,
  plus `route.global_route_id` (used for de-duping and for hide/reorder
  persistence — see `CONFIG_AND_STATE.md`) and `route_text_color === '000000'`
  as its dark-vs-white-badge-text test.

Both repos' partial v3 reads are consistent with each other and with the v4
schema above once you account for the nesting change — no conflicting
interpretation of any shared field name was found.
