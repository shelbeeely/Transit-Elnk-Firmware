# Transit-Elnk-Firmware

ESP32 firmware for the Xteink X4 (800×480, 4-gray e-ink) that shows next
transit departures for a stop — the same job as the Transit app's main
screen, and of the Transit-NearbyWebWidget / Transit-TV reference apps,
rendered to e-ink instead of a browser. Battery-powered: it wakes on a
timer, fetches departures, draws the board, and deep-sleeps.

Built on the [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk)
(vendored as a git submodule at `freeink-sdk/`) and PlatformIO/Arduino-ESP32.

## Hardware

- **Board**: Xteink X4 — ESP32-C3, SSD1677 e-ink controller, 800×480,
  4-level grayscale.
- **No touch, no built-in keyboard, no GPS, no RTC** — first-run setup and
  time sync are designed around those constraints (see below).

## First-run setup

On first boot (or whenever `wifi_ssid`/`api_key`/`stop_id` aren't all set),
the device hosts its own Wi-Fi access point (`TransitBoard-Setup`) with a
captive-portal web page — connect a phone or laptop to that network, a
setup page should open automatically (or visit `http://192.168.4.1`).
From there: pick/enter your Wi-Fi network, enter and validate a
[Transit API](https://transitapp.com/apis) key, then search for and pick
your stop. Each step is saved as soon as it's confirmed, so an interrupted
setup resumes where it left off rather than starting over.

Before requesting an API key, see root [`CLAUDE.md`](CLAUDE.md) for the
walkthrough (free vs. paid tier, rate limits, where the key is stored —
**never compiled into tracked source**; it's entered on-device and stored
in NVS).

## Building

```sh
git submodule update --init --recursive   # freeink-sdk, needed once per clone
pip install -U platformio

pio run -e xteink_x4      # real firmware build (ESP32-C3)
pio test -e native        # host-side unit tests, no hardware/toolchain needed
```

`pio run -e xteink_x4` downloads an ESP32 toolchain on first run — allow a
few minutes. CI (`.github/workflows/ci.yml`) runs both on every push/PR.

## Project layout

| Path | Contents |
|---|---|
| `src/main.cpp` | Boot → setup-if-unprovisioned → Wi-Fi + fetch → render → deep sleep |
| `src/transit/`, `include/transit/` | Implementation and headers for each module: data model & JSON parsing (`models`), Transit API v4 client (`api_client`), NVS config store (`config_store`), departure filter/sort/badge logic (`ui_logic`), e-ink layout and drawing (`render_engine`), route-icon SVG fetch/rasterize (`icon_cache`, `svg_path`), wake/sleep scheduling (`power_scheduler`), first-run captive-portal setup (`setup_flow`) |
| `test/` | Host-side Unity tests for the hardware-independent modules (`pio test -e native`) |
| `freeink-sdk/` | Vendored SDK submodule — display driver, UI framework, board config, power management, etc. |
| `docs/` | Design spec this firmware is built against (see table below) |

## Documentation

These reference docs are the spec the firmware is built against — written
from the running behavior of Transit-NearbyWebWidget and Transit-TV plus
the Transit API v4 OpenAPI spec, not re-derived by reading JS source at
implementation time.

| Doc | Covers |
|---|---|
| [`docs/API_CONTRACT.md`](docs/API_CONTRACT.md) | Annotated v4 schema for `nearby_routes`, `stop_departures`, `nearby_stops`, `search_stops` |
| [`docs/DATA_MODEL.md`](docs/DATA_MODEL.md) | Canonical `merged_itineraries`/`schedule_items` shape, mapped from both repos' partial v3-shaped reads |
| [`docs/UI_BEHAVIOR.md`](docs/UI_BEHAVIOR.md) | Refresh cadence, departure-window cutoffs, sort/filter/badge rules |
| [`docs/CONFIG_AND_STATE.md`](docs/CONFIG_AND_STATE.md) | Every user-configurable value today, and the NVS `ConfigStore` key mapping |
| [`docs/ASSETS_ICONS.md`](docs/ASSETS_ICONS.md) | Route icon/color source and the SVG→4-gray-bitmap conversion path |
| [`docs/DEPLOYMENT_OPS.md`](docs/DEPLOYMENT_OPS.md) | API key tier limits and what "continuous" polling actually costs |
