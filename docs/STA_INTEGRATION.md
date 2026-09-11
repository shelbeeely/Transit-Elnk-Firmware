# STA_INTEGRATION.md — Spokane Transit Authority, a second data source

STA departures are optional and shown alongside Transit API departures, not
merged/deduplicated with them (each STA row is labeled "STA `<route>`" —
see `include/transit/sta_models.h`'s `staDeparturesToRoutes()`). Configured
via `docs/CONFIG_AND_STATE.md`'s `sta_stop` key, empty by default (STA off).

## Data source: GTFS-RT, not the JSON REST API

STA runs the usual two-layer setup — a OneBusAway JSON REST API on top of
raw GTFS-RT protobuf feeds — but `spokanetransit.com` (and by extension its
OBA API, `api.spokanetransit.com`) sits behind Cloudflare bot protection
that returns an interstitial challenge page instead of JSON to a plain HTTP
client, this firmware included. That rules out the JSON layer entirely.

What *is* reachable unauthenticated, confirmed against live traffic:

```
GET https://gtfsbridge.spokanetransit.com/realtime/TripUpdate/TripUpdates.pb
Header: X-API-KEY: 12345
```

`12345` is the same shared key published in STA's own public OBA
configuration, not a private credential issued to this project — see
**Compliance note** below before any public deployment.

This is the entire network's TripUpdates in one response (~190KB observed;
GTFS-RT has no server-side per-stop filtering) — `sta_client.h` fetches and
filters it client-side. `sta_feed_parser.h` decodes it with a small
hand-rolled protobuf reader rather than pulling in a full protobuf library
(nanopb, protobuf-lite): the board only ever needs six fields out of the
whole GTFS-RT schema (route_id, trip_id, trip_short_name, stop_id, arrival/
departure time, schedule_relationship), so a purpose-built decoder is a much
smaller dependency than a general one. See that header's file comment for
two upstream-schema quirks it decodes around (STA serializes
`stop_headsign`/`trip_short_name` as an embedded `TranslatedString`
submessage even though the version of `gtfs-realtime.proto` most tooling
ships declares them as plain `string`; `direction_id` carries STA-internal
values outside GTFS's standard 0/1 and isn't used for anything here).

Vehicle positions and service alerts (the feed's other two endpoints) aren't
fetched — this board only ever needed arrival predictions.

## Baked-in route/stop tables

GTFS-RT carries no naming data at all (no route short name, no color, no
stop names) — that's static-GTFS-only. STA's static feed lives behind the
same Cloudflare protection as everything else on `spokanetransit.com`, so
`tools/gen_sta_tables.py` instead pulls from the
[Mobility Database](https://database.mobilitydata.org)'s public,
unauthenticated mirror of STA's GTFS
(`storage.googleapis.com/mdb-latest/...`) and generates:

- `include/transit/sta_route_table.h` (+ `.cpp`) — `route_id` → rider-facing
  short name + panel color, from `routes.txt`. Only rows whose `route_id` is
  purely numeric are kept (a live capture showed every real TripUpdate uses
  one; `routes.txt` also has a second family of non-numeric ids that never
  appeared on the wire — see the script for detail).
- `include/transit/sta_stop_table.h` (+ `.cpp`) — the numeric stop code
  printed on a physical sign → the feed's internal alphanumeric `stop_id` +
  a display name, from `stops.txt`. All ~1,665 stops, ~62KB combined — small
  next to this board's flash budget.

Per-trip destination text doesn't need either table: `trip_short_name` is
carried live in the GTFS-RT feed itself (see the quirk noted above), so
`sta_feed_parser.h` reads it directly rather than needing a static lookup.

Regenerate periodically (STA doesn't renumber routes or move stops often —
this isn't a per-build step):

```
python3 tools/gen_sta_tables.py <path-or-url-to-a-fresh-sta-gtfs.zip>
```

## Setup UX

Not part of first-run setup (it's an optional add-on, not something the
board needs to function) — entered/changed via the settings portal
(`SetupFlow::runSettingsPortal()`, reached by a long power-button hold at
boot on an already-provisioned board): a stop-code text field, validated
synchronously against `sta_stop_table.h` (no network call needed) so a typo
surfaces immediately rather than silently producing zero STA departures
later. Leaving it blank (or clearing a previously-set one) turns STA off.

## Known limitations

- **Whole-feed fetch, not streaming.** `sta_client.h` buffers the full
  ~190KB response before parsing rather than decoding it incrementally as
  bytes arrive — simpler and much easier to verify correct without physical
  hardware to test against, at the cost of a real (if guarded) memory risk
  on an ESP32-C3. It checks free heap first and skips the STA fetch for
  that wake cycle if there isn't comfortable headroom, rather than risking
  an allocation failure — see `sta_client.cpp`'s `kMinFreeHeapBytes`. A true
  streaming decoder would be the more defensible long-term design if this
  proves insufficient on real hardware.
- **No per-direction grouping.** STA's live `direction_id` isn't reliable
  (see above), so every STA route shows as one merged group rather than
  split by direction the way Transit API routes are.
- **Table staleness.** The route/stop tables are a point-in-time snapshot,
  not fetched live — a route STA adds after the tables were last generated
  falls back to its raw numeric id (`sta_feed_parser.h`'s table-miss path),
  and a moved/renamed stop needs the script re-run to pick up.

## Compliance note — unverified, flag before any public release

Transit's own API terms (`docs/DEPLOYMENT_OPS.md`) are explicit and were
reviewed directly; STA's equivalent terms for its GTFS-RT feed were not
found/reviewed as part of this integration — `spokanetransit.com`'s
Cloudflare protection blocked every attempt to reach its site, including
whatever developer/open-data policy page it may have. Two things worth the
maintainer's attention before this goes in front of anyone but the
maintainer:

- The `X-API-KEY: 12345` value is STA's own published default, not a key
  issued to this project — reasonable for a single personal device, but
  worth confirming with STA directly (their site, once reachable through
  whatever channel, or a direct inquiry) before a wider/public deployment
  rather than assuming it's fine at any scale.
- No STA logo or trademark asset is used anywhere in this integration —
  departures are labeled with the plain text "STA `<route number>`" only,
  deliberately, so nothing here implies STA's endorsement or uses a mark
  that wasn't confirmed available for use (see the project's earlier
  Transit-API compliance pass for the same reasoning applied to Transit's
  actual badge, where the real asset *was* confirmed and used).
