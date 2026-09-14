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
**Compliance** section below before any public deployment.

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
python3 tools/gen_sta_tables.py latest
python3 tools/gen_sta_tables.py <path-or-url-to-a-fresh-sta-gtfs.zip>
```

`latest` resolves the newest object in the Mobility Database mirror by
itself, so nothing has to hardcode a URL that carries a crawl number and
goes stale silently.

## Keeping the timetable from expiring

The static timetable has a hard expiry date: STA publishes a feed covering a
service period of a few months, and past its last day the calendar activates
no services at all. The board handles this correctly — it reports that its
card needs regenerating rather than showing an empty screen — but nobody
should be finding that out from the board on a morning they needed it.

`.github/workflows/sta-timetable.yml` runs weekly and:

1. regenerates from the newest mirrored feed,
2. **builds the firmware before committing anything** — the route/stop
   tables are compiled into the image, and a regenerated table that doesn't
   build, pushed to `main` by a scheduled job nobody is watching, is the one
   way this could do real damage,
3. commits the regenerated flash tables when the feed actually changed (the
   generator is deterministic, so an unchanged feed produces a byte-identical
   tree and no commit),
4. publishes the SD payload as a release asset (`sta-card-<expiry>.zip`),
5. opens **one** issue — updated, never duplicated — when the card has 28
   days or fewer left.

`docs/sta_feed_status.json` is the tracked record of what the current card
covers, so the expiry date is readable without a board:

```json
{ "valid_from": "20260517", "valid_until": "20260919", ... }
```

### What it can't do

- **It can't put files on the card.** That step is yours; the issue says so
  and links the zip.
- **It can't conjure a feed STA hasn't published.** The Mobility Database
  mirror is re-crawled on its own schedule, and STA's own site sits behind
  Cloudflare (the reason this project reads the mirror at all — see above),
  so "no newer feed yet" is a real and expected outcome. The workflow
  reports it as that rather than as a failure, and distinguishes it from
  "a new card is ready". If you find a fresher feed somewhere else, run the
  workflow manually with its URL.
- **Real-time departures are unaffected** by an expired card either way.
  Only the fully-offline timetable tier is.

## SD card: full static GTFS data (optional)

The X4 has a real, working SD slot (SPI-mode, shared with the display's SPI
bus — see `include/transit/sta_sd_store.h`'s file comment on why it must be
mounted before the display's own `begin()`) via `freeink-sdk`'s
`SDCardManager`/SdFat. The same `tools/gen_sta_tables.py` run above also
writes `sd_card_data/sta/{routes,stops,trips}.bin` — copy that `sta/`
directory onto the card's root (paths `/sta/routes.bin`, `/sta/stops.bin`,
`/sta/trips.bin`) to unlock what the flash tables can't fit:

- **`trips.bin`** — all ~8,400 of STA's scheduled trips (trip_id →
  route_id/direction_id/headsign), ~134KB. This is what actually motivates
  the SD path: `direction_id` here is the standard GTFS 0/1 field from
  static `trips.txt`, reliable unlike the live feed's own `direction_id`
  (see above) — `sta_client.cpp` looks up each departure's trip here after
  parsing and, when found, fills in `StaDeparture::directionId` for real
  per-direction grouping (`sta_models.h`'s `staDeparturesToRoutes()`) and a
  fallback headsign for the rare case the live feed's own
  `trip_short_name` was empty.
- **`stops.bin`** — the same ~1,665 stops as the flash table, plus lat/lon
  this time (~61KB). Exposed via `StaSdStore::nearbyStops()` (a linear
  scan + haversine sort — plenty fast at this record count, no spatial
  index needed) for a future nearby-stop search UI matching the Transit
  API setup flow's own lat/lon search; not wired into `setup_flow.cpp`
  yet, STA stop entry is still the direct stop-code field described above.
- **`routes.bin`** — the same route data as the flash table (~1KB),
  present mainly for format symmetry/future use; the flash copy already
  covers what's needed today.

All three are optional and independent of each other and of the card's
presence at all — see `sta_sd_store.h`'s file comment. `sta_gtfs_binary.h`
is the shared, hardware-independent record format/binary-search reader
(tested under `[env:native]` against synthetic fixtures, since real SD
hardware can't be exercised there); `sta_sd_store.h` is the hardware-only
glue that actually opens the files via `SDCardManager`.

## Setup UX

Not part of first-run setup (it's an optional add-on, not something the
board needs to function) — entered/changed via the settings portal
(`SetupFlow::runSettingsPortal()`, reached by a long power-button hold at
boot on an already-provisioned board): a stop-code text field, validated
synchronously against `sta_stop_table.h` (no network call needed) so a typo
surfaces immediately rather than silently producing zero STA departures
later. Leaving it blank (or clearing a previously-set one) turns STA off.

## Static timetable (offline departures)

Alongside the identity tables above, the SD card can carry STA's full
`stop_times` plus its service calendar, which lets the board answer "when
is the next bus" from the published schedule with no network at all. That
is a separate feature with its own document — see
[`OFFLINE_AND_BUS_WIFI.md`](OFFLINE_AND_BUS_WIFI.md) for the file format,
the holiday/expiry handling, and its limitations. It is generated by the
same `tools/gen_sta_tables.py` run and needs a version-2 card.

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
- **Per-direction grouping needs the SD card.** STA's live `direction_id`
  isn't reliable (see above), so without `trips.bin` present every STA
  route still shows as one merged group rather than split by direction the
  way Transit API routes are. Resolved when SD is present (see above) —
  this is the flash-only fallback behavior.
- **Table staleness.** The route/stop/trip tables are a point-in-time
  snapshot, not fetched live — a route STA adds after the tables were last
  generated falls back to its raw numeric id (`sta_feed_parser.h`'s
  table-miss path), and a moved/renamed stop or a new trip needs the
  script re-run (and, for SD, the files recopied onto the card) to pick up.
- **SD mount isn't cached across wake cycles.** `main.cpp`'s wake cycle
  always ends in deep sleep, which resets the MCU — there's no persistent
  "session" for `StaSdStore::begin()`'s own within-a-boot caching
  (`attempted_`) to survive between wakes. A board with no card (or one
  that fails to mount) pays a fresh `SDCardManager::begin()` mount attempt
  every single wake, not just the first. An RTC-memory-backed ("no card
  last time") cache could avoid that repeat cost on cardless boards, at
  the price of not detecting a card inserted after a cached failure until
  a full power cycle — not implemented here; worth revisiting if the
  per-wake mount-probe cost proves significant on real hardware.

## Compliance — STA's Developer Terms of Use, reviewed

`spokanetransit.com`'s Cloudflare protection blocked every earlier attempt
to reach STA's site directly (see above), but the maintainer supplied STA's
actual **Developers Terms of Use** page content directly, so — unlike the
initial version of this doc — the terms below are reviewed, not assumed.
Full text: [spokanetransit.com/developers-terms-of-use](https://www.spokanetransit.com/developers-terms-of-use/).

- **License to Data (§1)**: STA grants "a limited, revocable license to
  use, reproduce, redistribute and display the Data" — this integration's
  use (fetch the live feed, display arrival predictions on the panel) is
  squarely "display the Data" and fits within that grant as written.
- **No trademark/logo use permitted**: "You are not authorized to make any
  use of any proprietary service marks or trademarks of STA, including
  without limitation 'Spokane Transit Authority,' the associated logo, or
  any confusingly similar variant thereof." This integration doesn't use
  STA's logo or wordmark anywhere — departures are labeled with the plain
  text "STA `<route number>`" only, an identifying abbreviation, not a
  reproduction of their mark or logo. Unlike Transit's ToS
  (`docs/DEPLOYMENT_OPS.md`), STA's terms don't require any on-device
  attribution badge at all — the plain-text label is a deliberate choice
  here, not something the terms mandate.
- **"As is," no warranty, no guaranteed availability (§§1-2)**: STA
  disclaims all warranties on the Data and may modify/discontinue the
  service at any time without notice. Informational — matches how
  `sta_client.cpp` already treats every STA failure mode (network, parse,
  heap guard) as "nothing to report this cycle," never a hard error.
- **Termination at STA's discretion (§3)**: STA may terminate access "for
  any reason," without prior notice. Same practical handling as above.
- **Content restrictions (§4)**: schedules/arrival/fare data plus
  trademarks are STA's property; use beyond what §1 licenses
  ("modification, distribution, republication, or performance") needs
  STA's written consent. Displaying live arrivals on a personal device is
  within §1's grant; redistributing the feed itself to third parties
  would not be, and this project doesn't do that.
- **A formal Developer's Resource page exists**, gated behind clicking "I
  Agree to the STA Developers Terms of Use" on that page. This document's
  shared `X-API-KEY: 12345` was found via STA's public OneBusAway config,
  not through that gated flow — it works today, but it's worth the
  maintainer actually clicking through STA's own Developer's Resource page
  before any wider deployment, in case STA's officially sanctioned path
  involves requesting a project-specific key rather than relying on the
  published default indefinitely.
- **Governing law (§5)**: Washington State, Spokane County courts.
  Informational, no firmware action.
