# Transit-Elnk-Firmware

ESP32 firmware for the Xteink X4 (800×480, 4-gray e-ink) that shows next
transit departures for a location — the same job as the Transit app's main
screen, and of the Transit-NearbyWebWidget / Transit-TV reference apps,
rendered to e-ink instead of a browser.

Before any implementation work, see `CLAUDE.md` at the repo root for the
Transit API key setup flow.

## Documentation (Phase P0)

These reference docs are the spec the firmware is built against — written
from the running behavior of Transit-NearbyWebWidget and Transit-TV plus
the Transit API v4 OpenAPI spec, not re-derived by reading JS source at
implementation time.

| Doc | Covers |
|---|---|
| [`docs/API_CONTRACT.md`](docs/API_CONTRACT.md) | Annotated v4 schema for `nearby_routes`, `stop_departures`, `nearby_stops`, `search_stops` |
| [`docs/DATA_MODEL.md`](docs/DATA_MODEL.md) | Canonical `merged_itineraries`/`schedule_items` shape, mapped from both repos' partial v3-shaped reads |
| [`docs/UI_BEHAVIOR.md`](docs/UI_BEHAVIOR.md) | Refresh cadence, departure-window cutoffs, sort/filter/badge rules |
| [`docs/CONFIG_AND_STATE.md`](docs/CONFIG_AND_STATE.md) | Every user-configurable value today, and the proposed NVS `ConfigStore` mapping |
| [`docs/ASSETS_ICONS.md`](docs/ASSETS_ICONS.md) | Route icon/color source and the SVG→4-gray-bitmap conversion path |
| [`docs/DEPLOYMENT_OPS.md`](docs/DEPLOYMENT_OPS.md) | API key tier limits and what "continuous" polling actually costs |