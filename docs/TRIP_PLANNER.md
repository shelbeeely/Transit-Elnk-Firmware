# TRIP_PLANNER.md — preset "Home"/"Work" trip chains

Optional, settings-portal-only (a long power-button hold at boot on an
already-provisioned board — see `SetupFlow::runSettingsPortal()`). Lets a
rider save a fixed route chain to two named destinations — "Home" and
"Work" — so the board can compute *when to leave* and *when to get off and
transfer*, not just show raw departure times at the one configured stop.
Configured via `docs/CONFIG_AND_STATE.md`'s `home_legs`/`work_legs` and
related keys, empty by default (no presets configured, no change in
behavior from before this feature existed).

## Why not Transit API v4's `/v4/public/plan` endpoint

`docs/API_CONTRACT.md` notes that the v4 spec documents a real trip-planning
endpoint (`/v4/public/plan`, `estimate_plan_duration`) but leaves it
out-of-scope/undocumented for this project, since neither reference app
uses it. Investigating it for this feature turned up the same gap: its
exact request/response schema and any tier/rate-limit restrictions
couldn't be confirmed (the API's interactive docs are a JS-rendered page
this environment couldn't render). Rather than guess at an undocumented
endpoint's shape, this feature takes a different, lower-risk approach: the
user already knows their own route chain from experience — that's exactly
what "31 → 32 → 97" is — so they pre-configure each preset's fixed leg
sequence (route + boarding stop + transfer stop, per leg), and the firmware
computes next-departure/transfer-feasibility from `stopDepartures()` data
it's already fetching. If `/v4/public/plan`'s schema is ever confirmed
(the same "live-capture, then document" process `docs/STA_INTEGRATION.md`
used for STA's GTFS-RT quirks), automatic route discovery would be a
natural follow-up — not attempted here.

## How a plan is computed

`include/transit/trip_planner.h`'s `planPresetTrip()` is a pure function
(no NVS/network/display dependency, host-testable under `[env:native]` —
see `test/test_trip_planner`) that takes a preset's configured legs plus
the full `stopDepartures()` response across every stop involved, and
resolves, leg by leg:

1. **Leg 0's boarding time**: that leg's route's next scheduled departure
   at its boarding stop, at or after now (direction-filtered when the leg
   has a confirmed `directionId` — see below).
2. **That leg's alight time**: approximated as the *same route's next
   scheduled departure/passage at the alighting stop*, at or after the
   boarding time — `stopDepartures()` reports a departure/passage time at a
   stop, not a distinct arrival time, so this is the closest available
   proxy for "when do I get off here." Good enough at the minute-level
   granularity a departure board needs; see "Known limitations" below.
3. **Leg N (N>0)'s boarding time**: that leg's route's next departure at
   its boarding stop, at least `xfer_buf_min` minutes (default 3, shared
   across both presets) after the previous leg's alight time.
4. **Leave-by time**: leg 0's boarding time minus that preset's configured
   walk-to-first-stop minutes (`home_walk_min`/`work_walk_min`).

If any leg can't be matched (no upcoming departure, or a transfer isn't
feasible within the window `stopDepartures()` actually returned), the plan
reports a plain fallback message ("No upcoming trip found" / "Transfer to
`<route>` not found") instead of blocking or erroring the board — the same
"nothing to report this cycle" philosophy `sta_client.cpp` already uses for
every STA failure mode.

## Direction disambiguation

A route can run multiple directions through the same stop, and nothing in
`stopDepartures()`'s response says which direction actually continues
toward a given transfer stop. Each configured leg carries an explicit
`directionId` (0/1), resolved **live** at settings-portal setup time: after
picking a route and boarding stop, the portal calls a new endpoint
(`POST /legdirections`) that queries `stopDepartures()` for that stop,
finds the matching route, and returns its available directions/headsigns
for the user to pick from (auto-selected when there's only one). This
avoids guessing — see `setup_flow.cpp`'s `handleLegDirections()`.

## Settings UX

Reuses the existing captive-portal pattern (`setup_flow.cpp`): a "Home"
and "Work" section, each with up to 3 leg rows (route number typed, two
stop pickers reusing the existing `stopsearch` search-and-pick flow,
generalized to resolve a pick entirely client-side rather than needing a
second server round-trip), a walk-time field, and a shared transfer-buffer
field. Editing a **previously-saved** leg chain isn't supported — the page
never rebuilds an editable list of legs from saved `routeId`/`stopId`
strings (that would need a reverse stop-name lookup this page doesn't have)
— only a plain "N leg(s) currently configured" summary is shown, and
changing a chain means re-entering it from scratch. This is a deliberate
simplification at ≤3 legs per preset, not an oversight.

Saving a preset's legs is independent of every other settings field:
the portal only sends a preset's `legs` to the board when the user actually
added/removed a leg that session (see `setup_flow.cpp`'s `legsTouched`) —
otherwise, saving something unrelated (orientation, the walk-time field, a
different preset's legs) leaves a previously-configured chain untouched.

## Call-volume budget

Every leg's boarding/alighting `stop_id` is folded into the **same**
`stopDepartures()` call `main.cpp` already makes for the main board (up to
100 stop IDs per call — comfortably enough for 2 presets × 3 legs × 2 stops
= 13 IDs plus the main stop). Configuring presets costs **zero additional
Transit API calls per wake** — only a larger response body, and a higher
`max_num_departures` request (capped at 8 instead of the display cap) so
trip planning can see far enough into each leg stop's schedule. At the
documented default `refresh_interval_min` (60), that's the same ~720
calls/month as the board already makes without presets configured — well
inside the free tier's 1,500/month, per root `CLAUDE.md`'s rate-limit math.

## Known limitations

- **Alight time is an approximation**, not a true arrival prediction — see
  "How a plan is computed" step 2 above. A route with unusually long dwell
  time at a stop, or a schedule gap right at the alighting stop, could
  understate or overstate the real transfer window by a few minutes.
- **`rtTripId`-based same-trip matching is best-effort, never required.**
  When a boarding departure's `rtTripId` is non-empty, `planPresetTrip()`
  prefers a scheduleItem at the alighting stop with a matching `rtTripId`
  (a stronger signal than nearest-time) before falling back to nearest-time
  regardless. `docs/DATA_MODEL.md` documents `rtTripId` as not guaranteed
  populated on schedule-only (non-realtime) responses or stable across two
  separate `stopDepartures()` queries — this is a genuine limitation of the
  data, not something this module can fix.
- **A leg's boarding search never carries a "preferred trip" hint from the
  previous leg** — deliberately: the previous leg's alight and this leg's
  board are two different routes (that's what makes it a transfer), so
  there's no legitimate trip-continuity signal to carry across. Only the
  *within-one-leg* board→alight `rtTripId` match (same route) is used.
- **Route resolution takes the first matching `Route` entry** at a leg's
  boarding stop whose `routeShortName` matches what was typed. Two
  different agencies serving the same physical stop with the same route
  number is a real but rare GTFS edge case this doesn't disambiguate
  further.
- **Table/schedule staleness** applies the same way it does to the main
  board — a plan is only as fresh as the `stopDepartures()` response it was
  computed from that wake cycle.
