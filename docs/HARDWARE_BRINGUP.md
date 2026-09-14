# Hardware bringup — getting this firmware onto a real Xteink X4

Everything in this repo so far has been verified two ways: 244 host-side
tests under `[env:native]`, and a real ESP32-C3 cross-compile in CI
(`pio run -e xteink_x4`). Neither of those has ever touched a panel, an SD
card, a battery, or a radio. This document covers the step nobody has taken
yet — putting the firmware on a physical board — and the tooling added to
make that step debuggable rather than blind.

## 0. Before you plug anything in

You need:

- The Xteink X4 (ESP32-C3, SSD1677 800x480 4-gray) and a **data** USB-C cable.
  A charge-only cable is the single most common "the port doesn't show up"
  cause; if `pio device list` shows nothing, try another cable first.
- Python 3.9+ and PlatformIO: `pip install -U platformio`
- The repo, with submodules: `git submodule update --init --recursive`
  (a fresh clone's `freeink-sdk/` is empty, and the build error it causes —
  `EInkDisplay.h: No such file` — points at entirely the wrong thing).
- Optionally, a microSD card with the STA tables on it
  (`python3 tools/gen_sta_tables.py`, then copy `sd_card_data/` onto the
  card). Fully optional — everything SD-backed degrades to the flash-baked
  tables.

You do **not** need an API key yet. The board asks for one on its own, via
the first-run setup portal, and stores it in NVS. Per `CLAUDE.md`, the key
is never compiled into tracked source.

## 1. One command

```sh
tools/bringup.sh
```

That script fetches submodules if needed, builds `[env:xteink_x4_bringup]`,
flashes it, and opens a monitor while teeing the whole session to
`logs/bringup-<timestamp>-<env>.log` (gitignored). Useful flags:

| Flag | Effect |
|---|---|
| `--field` | Build `[env:xteink_x4]` instead — the normal, sleeping firmware |
| `--port /dev/...` | Skip auto-detection |
| `--monitor-only` | Don't flash, just watch |

If the C3 refuses to enter download mode (`Failed to connect to ESP32-C3`),
hold **BOOT**, tap **RESET**, release BOOT, and re-run. Most X4 units don't
need this; USB-serial-JTAG usually resets itself.

## 2. What the bringup build does differently

`[env:xteink_x4_bringup]` is byte-for-byte `[env:xteink_x4]` plus
`-DFREEINK_BRINGUP=1`. Three differences, all in `src/main.cpp`:

1. **It waits up to 8 s for a USB host** before printing, so a monitor you
   started by hand still catches the boot report. The field build must never
   wait for a host that will not arrive — it runs on a battery.
2. **It runs a self-test** before the wake cycle instead of inferring the
   hardware's health from whatever a normal wake happened to do.
3. **It does not deep-sleep.** It holds the USB link open and drops into a
   small serial console.

> Do not leave the bringup build on a battery-powered board. It never
> sleeps, and it will flatten the cell. Reflash with `--field` once the
> hardware is known good.

### The serial console

| Key | Action |
|---|---|
| `r` | Reprint the boot report |
| `t` | Re-run the self-test |
| `w` | Wi-Fi scan, with SSID / RSSI / channel / encryption |
| `c` | Reboot and run a full wake cycle |
| `s` | Enter deep sleep now (i.e. behave like the field build) |
| `h` | Help |

`c` reboots rather than re-entering the cycle in place, deliberately: a
second pass over `setup()` with half-initialized globals would be debugging
a state the field firmware never reaches.

## 3. Reading the output

Both builds print the same two blocks; the bringup build just adds the
self-test between them. All of it is flat ASCII `key: value`, so it diffs
cleanly between two boots and greps usefully (`grep FAIL`, `grep -A20 '\[config\]'`).

### Boot report

Taken once, right after the peripherals come up and before the firmware
starts changing anything:

```
======== Transit-Elnk boot report ========
[build]
  firmware               a2945b9-dirty
  built                  Sep 14 2026 10:14:02
  env                    xteink_x4_bringup
[chip]
  model                  ESP32-C3
  ...
[boot]
  reset reason           power-on
  wake cause             not a sleep wake
  boot count             1
  free heap              243.1 KB (248936 B)
[peripherals]
  display                yes (800x480)
  sd mounted             yes
  sd stop_times table    yes
[config]
  wifi ssid              HomeNetwork
  wifi password          (set, 28 chars, ...aple)
  api key                (set, 24 chars, ...ab12)
  ...
```

Things worth staring at:

- **`reset reason: BROWNOUT`** — the power rail sagged. On a first bringup
  this usually means the panel's full-refresh current draw is more than the
  supply can give. Try a different USB port or a powered hub.
- **`reset reason: PANIC (crash)`** — the monitor's
  `esp32_exception_decoder` filter (configured in `platformio.ini`) will have
  already turned the backtrace addresses into `file:line`. That decoded
  backtrace is the single most useful thing to capture.
- **`boot count`** resetting to 1 repeatedly — the board is resetting, not
  sleeping. It lives in RTC memory alongside the approximate clock, so a
  brownout loop and normal operation are otherwise nearly indistinguishable.
- **`sd routes table: yes` with `sd stop_times table: no`** — the card was
  written by an older `tools/gen_sta_tables.py`. The offline timetable will
  silently never appear. Regenerate the card.
- **Secrets are fingerprinted, never printed.** `formatBootReport()` runs
  every secret through `redactSecret()` itself, so no caller can forget —
  and `test/test_boot_report/` asserts the raw value doesn't survive
  formatting. The character count is included on purpose: "did my key get
  truncated when I pasted it into the portal" is a real bringup question
  that the last four characters alone cannot answer.

### Wake summary

Printed at the end of every cycle, in both builds:

```
-------- wake summary --------
[network]
  wifi                   yes
  ssid                   HomeNetwork
  rssi                   -58 dBm
[clock]
  sntp                   yes
  now                    2026-09-14 10:14:38 (epoch 1789294478)
[data]
  transit fetch          yes
  transit http           200
  stop ids requested     5
  source                 live
  departures drawn       9
[timing]
  wifi                   3.41 s
  fetch                  1.88 s
  render                 2.90 s
  awake total            9.12 s
  next wake              60 min
```

`transit http` is the field that earns its keep. The API client returns a
bare `bool`, which collapses "401, your key is wrong", "429, you're over the
free tier" and "DNS never resolved" into one indistinguishable failure;
`TransitApiClient::lastStatusCode()` exists so the log can tell them apart.
`0` means the request never reached a server at all.

`source` says which of the three fallback tiers actually put rows on the
panel — `live`, `cached`, `scheduled`, or `none` — which is the fastest way
to tell a network problem from a rendering one.

`awake total` is the number that decides battery life. At a 60-minute
refresh interval, ~9 s awake per wake is roughly 0.25% duty cycle.

### Self-test

```
======== self-test ========
  [PASS] display              800x480 framebuffer allocated
  [PASS] panel refresh        2 full refreshes in 3412 ms -- confirm the panel flashed black then white
  [PASS] nvs config           refresh interval reads back as 60 min
  [SKIP] sd card              not mounted (optional; STA falls back to flash tables)
  [PASS] battery              3981 mV, 80%
  [PASS] heap headroom        248936 B free, largest block 110592 B (want >= 92160 B)
  [PASS] wifi scan            11 networks; "HomeNetwork" at -58 dBm
  5 passed, 0 failed, 1 skipped -- ALL OK
===========================
```

Notes on the verdicts:

- **`panel refresh` cannot fail automatically.** Nothing can read the panel
  back, so what it produces is a human verdict ("did the screen flash black
  then white?") plus the refresh timing — which is the number that actually
  moves when a waveform or the SPI wiring is wrong.
- **`wifi scan` passing means the radio works**, not that your network is
  reachable. Those are separate findings, and the detail line says which
  one you got. Conflating them turns "you typed the SSID wrong" into "the
  Wi-Fi is broken".
- **A skip is not a pass.** A run where *everything* skipped reports
  `ATTENTION NEEDED`, because it learned nothing.
- **`battery` bounds are 2500–4500 mV.** Outside that is an ADC or divider
  problem, not a flat cell — a genuinely empty 1S Li-ion still reads well
  above 2.5 V before its protection circuit cuts in.

## 4. First-bringup risks specific to this board

These are the places this codebase makes an assumption that only real
hardware can confirm. Check them in this order; each one is visible in the
output above.

1. **SD and the display share the SPI bus.** `setup()` calls
   `g_staSdStore.begin()` *before* `g_display.begin()` on purpose — see the
   comment there. If the panel comes up garbled only when a card is
   inserted, this ordering is the first suspect.
2. **Deep sleep and `RTC_DATA_ATTR` have never run on silicon.** The
   approximate clock (`time_keeper.h`) and the boot counter both depend on
   RTC fast memory surviving a timer wake. `boot count` incrementing across
   sleeps, and `[clock] carried across sleep: yes`, are the confirmation.
3. **The battery ADC divider is unverified.** `BatteryMonitor` uses
   `BoardConfig::ACTIVE`'s X4 profile. Compare the reported millivolts
   against a meter on the pack once, and if the divider is wrong the
   percentage will be confidently wrong forever.
4. **TLS is unpinned.** `api_client.cpp` uses the Arduino core's
   `WiFiClientSecure`. A `transit http: 0` with Wi-Fi up and a good RSSI is
   most likely a certificate-chain problem.
5. **Heap headroom for the STA feed.** The STA GTFS-RT feed is ~190 KB and
   is parsed in RAM on a chip with ~400 KB total. `sta_client.cpp` already
   refuses to fetch below its own threshold and logs why. Watch
   `min ever free heap` across a few wakes.
6. **The STA timetable card expires.** `[config]`'s feed validity comes from
   `calendar.bin`; the currently generated card is valid through
   **2026-09-19**. Regenerate with `tools/gen_sta_tables.py`.

## 5. Debugging it with Claude, live

Three ways, in descending order of how well they work.

### A. Run Claude Code on the laptop that has the board (recommended)

This is the whole answer, and it needs nothing built. Install the CLI, open
this repo locally, and a local session can run `tools/bringup.sh`,
`pio run -t upload`, `pio device monitor`, edit source, and reflash — a full
edit/flash/observe loop with no bridge in the middle. A cloud session (like
the one this document was written in) has no USB and no serial ports and
never will.

Bring the branch over with:

```sh
git clone <repo> && cd Transit-Elnk-Firmware
git checkout claude/batch-firmware-research-build-daxt56
git submodule update --init --recursive
```

### B. Paste the log

`tools/bringup.sh` already writes `logs/bringup-*.log`. The boot report and
wake summary are designed to be pasteable as-is: flat, bounded, and already
redacted. For a crash, paste the decoded backtrace with the ~40 lines before
it.

### C. Relay the log to a remote session

```sh
# terminal 1
tools/bringup.sh

# terminal 2
tools/log_relay.py logs/bringup-<...>.log --post-url "<webhook url>"
```

`tools/log_relay.py` tails the file `bringup.sh` is already writing (only one
process can own the serial port, and that's the monitor) and POSTs new lines
as JSON. It scrubs secret-shaped tokens before anything leaves the machine —
the firmware's own redaction covers the boot report, but not whatever you
type into the console.

### What isn't set up, and what it would take

- **OTA updates.** `partitions.csv` already has the layout for it — dual
  6.3 MB OTA slots (`ota_0` at `0x10000`, `ota_1` at `0x650000`) plus
  `otadata` — but **no OTA code exists** in this firmware. Adding it would
  remove the cable from the iteration loop after the first flash. It is the
  obvious next thing if you end up reflashing a lot; say the word.
- **Hardware-in-the-loop CI.** A self-hosted GitHub Actions runner with the
  board attached could run `pio run -t upload` and capture serial into the
  job log on every push — which a cloud session *can* read, via the Actions
  API. Heavier to set up than A, but it's the only option that gets real
  hardware into the existing CI gate.
- **JTAG debugging.** The C3's built-in USB-JTAG supports real breakpoints
  via `pio debug`. Nothing in `platformio.ini` configures `debug_tool` yet;
  the serial diagnostics above were the cheaper 90%.
