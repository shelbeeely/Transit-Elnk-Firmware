# AGENCY_REGISTRY.md — the community agency list

`agencies/registry.json` tracks agencies with (or requested for) the
**optional second data source and offline static timetable** — the
STA-shaped bonus tier described in
[`STA_INTEGRATION.md`](STA_INTEGRATION.md), not the board's primary way of
getting departures.

## What this registry is *not*

**Live departures already work for almost any agency, with zero entries
here.** The board's main data path is the Transit API
(`docs/API_CONTRACT.md`), and Transit already covers most transit agencies
in North America and a good part of the rest of the world. First-run setup
searches for your stop by name/coordinates against that API — if your
agency is in Transit's coverage (most are), you're done, nothing in this
directory is involved at all.

This registry exists for the second, optional layer: a live GTFS-RT feed
read directly (bypassing Transit's own layer, the way `STA_INTEGRATION.md`
had to for Spokane specifically because `spokanetransit.com` sits behind
Cloudflare) plus a full offline static timetable on the SD card that keeps
working with no network at all. That's real, agency-specific integration
work — reverse-engineering a feed, reviewing that agency's terms of use,
generating baked-in tables — not something that turns on by typing a city
name.

## Two tiers of entry

**`status: "requested"`** — the low-friction path. You want your agency's
second-source/offline tier built; you (or anyone) don't have to do the
integration work to say so. Minimum fields: `id`, `name`, `region`,
`status`, and ideally `requested_by` (your GitHub username, so whoever
picks it up has someone to ask "does this agency publish GTFS-RT at all?").
See [Requesting an agency](#requesting-an-agency) below.

**`status: "supported"`** — the real thing, same depth as
`STA_INTEGRATION.md`: the live feed's actual URL and auth (if any), where
the static GTFS data comes from, that agency's own terms of use reviewed
(not assumed), and what stop-identification scheme a rider has to use. The
schema (`agencies/registry.schema.json`) requires `gtfs_rt`,
`gtfs_static_source`, `stop_code_convention`, `docs`, and `maintainer` for
this tier — CI (`.github/workflows/agency-registry.yml`) rejects a
`supported` entry missing any of them.

## Where this stands today

**The firmware doesn't read this registry at runtime yet.** Right now it's
a coordination and tracking mechanism — a structured, reviewable place to
request an agency and to record one that's been worked out — plus the
schema every future entry has to satisfy. The `sta_*` modules
(`STA_INTEGRATION.md`) are still the one implementation, specific to
Spokane. Generalizing them to read an arbitrary registry entry (picking an
agency in the settings portal, downloading a pre-generated data pack for
it instead of a fixed STA-only flash table) is real, separate firmware
work that hasn't happened yet — this registry is what that work will be
driven by, not a promise that it already is.

## Requesting an agency

Open a PR adding an entry to `agencies/registry.json`:

```json
{
  "id": "your-agency-slug",
  "name": "Your Transit Agency",
  "region": "City, State/Province, Country",
  "status": "requested",
  "requested_by": "your-github-username"
}
```

`id` is a short lowercase slug (letters, digits, hyphens) — it'll become a
filename once packs exist, so treat it as an identifier, not a label. CI
validates the JSON against `agencies/registry.schema.json` automatically;
a green check means the entry is well-formed, not that the agency has been
reviewed for feasibility (does it publish GTFS-RT at all? is it behind
something like STA's Cloudflare wall?) — that's a real follow-up
conversation on the PR.

## Contributing full support for an agency

This is the STA-shaped work: read `STA_INTEGRATION.md` start to finish as
the template for both the code and the writeup a `supported` entry needs.
In short, you'd be figuring out:

- Whether the agency publishes GTFS-RT TripUpdates directly, and whether
  it's reachable the ordinary way or needs a workaround like STA's.
  `agencies/registry.schema.json`'s `gtfs_rt.protocol` currently only
  allows `"gtfs_rt_protobuf"` — the one shape the firmware's hand-rolled
  decoder can read today. An agency that only offers something else (a
  OneBusAway-style JSON API and no raw feed, say) isn't supportable without
  new firmware work first; file it `requested` with a note instead.
- Where the static route/stop/trip data comes from (the agency directly,
  or a public mirror like the Mobility Database, as STA needed).
- That agency's actual published terms of use for using their data — read
  and cited, the way `STA_INTEGRATION.md`'s Compliance section is, not
  assumed from a generic API's usual norms.
- What a rider has to type into the settings portal to identify a stop for
  this agency.

Update the entry to `status: "supported"` with the fields above filled in,
and add your own `docs/<AGENCY>_INTEGRATION.md` for CI's `docs` field to
point at. The generalized runtime code that actually *consumes* a
`supported` entry (rather than STA's still-hardcoded path) is tracked
separately — check open issues/discussion before assuming it's ready to
build against.

## Schema

See `agencies/registry.schema.json` for the authoritative, machine-checked
shape (every field's type, which are required per status, and the URL/id
format constraints) — this document explains the *why*, that file is the
source of truth for the *what*.
