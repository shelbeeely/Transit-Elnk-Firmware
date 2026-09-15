# Contributing

This is primarily a personal/hobby project, but the transit-agency list is
deliberately open to community contribution — see
[`docs/AGENCY_REGISTRY.md`](docs/AGENCY_REGISTRY.md) for the full
explanation. Short version:

- **Want departures for your city and it already works?** Nothing to do —
  live departures via the Transit API already cover most agencies, and
  that doesn't involve this repo's `agencies/` directory at all.
- **Want the offline/second-source tier for your agency** (the
  STA-shaped bonus: a live GTFS-RT feed plus a full static
  timetable that works with no network) **but can't build it yourself?**
  Open a PR adding a minimal `status: "requested"` entry to
  `agencies/registry.json` — see
  [Requesting an agency](docs/AGENCY_REGISTRY.md#requesting-an-agency).
- **Want to do the integration work yourself?** See
  [Contributing full support for an agency](docs/AGENCY_REGISTRY.md#contributing-full-support-for-an-agency) —
  `docs/STA_INTEGRATION.md` is the template for both the writeup and the
  depth of review (terms of use included) expected.

Every PR touching `agencies/registry.json` is checked automatically
against `agencies/registry.schema.json`
(`.github/workflows/agency-registry.yml`) — a failing check means the
entry doesn't match the schema, not a verdict on the agency itself.

For anything else — a bug, a firmware change, a doc fix — open an issue or
PR as normal; there's no separate process beyond what CI already checks
(`.github/workflows/ci.yml`'s host tests and firmware build).
