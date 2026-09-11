# OFFLINE_AND_BUS_WIFI.md — what the board does with no network

Two related features, both aimed at the same situation: the board leaves the
Wi-Fi network it was provisioned on. Carried onto a bus, that used to mean a
blank screen and no clock. Now it means a cached board marked as stale, an
approximate clock that keeps counting, and — where the vehicle offers open
Wi-Fi behind a sign-in page — an attempt to get back online without touching
the device.

| Module | Covers |
|---|---|
| `include/transit/time_keeper.h` | Approximate wall clock that survives deep sleep, in RTC memory |
| `include/transit/offline_cache.h` | Last-known-good departure board, persisted in NVS |
| `include/transit/captive_portal.h` | Detecting and signing in to an open network's portal |

## What used to be lost offline

For reference, the behavior before any of this existed — a wake with no
network produced:

| | Before | Now |
|---|---|---|
| Departure rows | Blank "No departures to show." | Last fetched board, pruned of departures that have already left |
| Header clock | `--:--` | `~14:32`, from the RTC-memory clock, or `--:--` once its error bound gets too wide |
| Countdowns (`3m`, `18m`) | Not computable — chips fell back to bare clock times | Computed against the approximate clock |
| "Leave now" urgency | Never triggered (needs a clock) | Triggers normally |
| Preset trip lines | Absent | Restored from cache and re-formatted against the current clock |
| Sleep-window math | Evaluated as if it were midnight (`minutesSinceLocalMidnight(0)` returns 0) | Evaluated against the approximate clock |
| Battery | Correct (local ADC read) | Unchanged |

## 1. The approximate clock (`time_keeper.h`)

The X4 has no RTC chip — `BoardConfig`'s `FREEINK_CAP_RTC` excludes it — so
time has always come from SNTP on each wake. What the board *does* have is
ESP32 RTC memory: a few KB (RTC **fast** memory; the C3 has no RTC slow
memory) that keeps its contents across deep sleep. This is the same
mechanism behind the Arduino-ESP32 deep-sleep example's `RTC_DATA_ATTR` boot
counter.

Before sleeping, the firmware records the best epoch it knows and how long
the wake timer is armed for. On the next wake it adds those together, plus
this boot's own uptime, and has an estimate. A successful SNTP sync always
wins and resets everything.

### Accuracy, stated plainly

Deep-sleep timing runs off the RTC slow clock, which on this board is an
internal RC oscillator calibrated against the main crystal at startup — not
a watch crystal, and not disciplined during the sleep itself. It drifts with
temperature, and the error compounds across every offline wake instead of
staying put.

`kSleepTimerErrorPermille` budgets **1% of elapsed sleep time** as an error
bound. That figure is a deliberately conservative upper bound, **not
something measured on this hardware** — treat it as the module refusing to
overpromise rather than as a spec.

Once the accumulated bound passes `kMaxUsableErrorSec` (15 minutes — the
point where a minutes-until-departure countdown stops being actionable),
`approximateClockUsable()` returns false and the board goes back to showing
no time at all. At the default hourly refresh that happens after roughly a
day offline.

While the clock is an estimate rather than a sync, the header shows a
leading `~` on the time. An estimate is never presented as the real time.

RTC memory does **not** survive a power-on reset, a battery pull, or a
firmware flash. `loadApproxClock()` requires both a timer wake cause and its
own magic value, so all three of those correctly read as "no clock carried
over" rather than as whatever bytes happened to remain.

## 2. The offline board cache (`offline_cache.h`)

After every successful fetch, the **already-computed** board — the output of
`ui_logic::buildDepartureBoard()`, plus each configured preset's
`PresetTripPlan` — is serialized into NVS under `cached_board`. A wake that
fetches nothing restores it.

**Why the computed board rather than the raw API response:** a multi-stop
`stop_departures` response with presets configured runs to tens of
kilobytes, and an ESP-IDF NVS string value tops out just under 4000 bytes.
The drawn board is a few hundred. `kMaxCachedBoardBytes` is 3500, and
preset plans are written first so board rows, not preset lines, are what
gets dropped if the budget runs out.

**The trade this implies:** cached data can't be re-filtered against changed
display settings. A hidden-route or departure-window change made while
offline takes effect at the next successful fetch, not immediately.

### Ageing

Departure times are absolute epochs, so the cache ages correctly on its own.
`pruneExpiredDepartures()` drops anything already gone (otherwise
`formatDepartureChip()` would clamp negative minutes to 0 and show "Due"
forever), then drops rows left with no departures, then drops preset plans
whose leave-by time has passed. A cache left offline long enough empties
itself out rather than lying. A plan that was never found keeps its
fallback message, since that message is still the honest explanation for a
blank preset line.

With no clock at all (`nowEpoch <= 0`), pruning is a no-op — there is no
basis for calling anything expired.

### On screen

![A cached board marked offline](screenshots/departure_board_offline_cached.png)

The header says `Cached 2h ago` in place of the usual `Updated 14:32`, and
the warning reads **Offline** rather than "No Wi-Fi" — the board is showing
real departures, just not fresh ones, and "No Wi-Fi" next to a full board
reads like the board itself is broken.

Two cases that look similar but aren't:

- **Wi-Fi up, fetch failed** (bad key, quota, a 5xx) still says
  **Fetch failed**, not "Offline". The staleness is already carried by the
  `Cached …` line; throwing away the diagnostic on top of it would help
  nobody.
- **Cached, but no clock** — offline long enough that the approximate clock
  aged out, or a cold boot with no network — says `Cached (age unknown)`.
  Reporting `just now` there would label a board of completely unknown
  vintage as fresh, which is the exact failure this whole treatment exists
  to prevent. Note that without a clock the prune above can't run either,
  so those rows really could be from yesterday.

Three distinct empty-board messages, because they call for different
responses from the reader:

| Situation | Message |
|---|---|
| Online, stop is just quiet | "No departures to show." |
| Offline, cache aged out entirely | "Every cached departure has already left, and there's no network to refresh." |
| Offline, nothing ever cached | "Offline, and nothing cached yet to fall back on." |

### Flash wear

One NVS string write per successful wake. At the documented 60-minute
default that's ~8,760 writes/year against flash rated for ~100,000
erase cycles, spread further by NVS's own wear levelling — not a concern at
this cadence. It would become one at a refresh interval of a minute or two,
which the free-tier API budget rules out anyway (see `DEPLOYMENT_OPS.md`).

## 3. Bus Wi-Fi captive-portal sign-in (`captive_portal.h`)

### The problem

`WiFi.begin()` to an open SSID reports `WL_CONNECTED` long before there is
any internet. A captive portal answers every request with its own splash
page until something submits the form on it. To the firmware, an undismissed
portal looks exactly like a working network that returns garbage for every
API call.

### Detection

The same trick every phone OS uses, run in reverse of how `setup_flow.cpp`
uses it: request a URL whose correct answer is known in advance. All three
canaries in `kDefaultProbeUrls` answer an uncaptured request with a bare
HTTP 204 and no body:

- `http://connectivitycheck.gstatic.com/generate_204`
- `http://cp.cloudflare.com/generate_204`
- `http://clients3.google.com/generate_204`

Two operators rather than three Google hostnames, so one provider being
blocked doesn't take out the whole list. Apple's `/hotspot-detect.html` and
Microsoft's `/connecttest.txt` are deliberately **not** used: they answer
200 with specific body text, which would make a portal's own 200 splash page
indistinguishable from success without a per-URL content check.

The probes are plain `http://` on purpose. A portal can't transparently
intercept TLS without a certificate error, so an `https` probe can't tell
"captured" apart from "offline."

### Sign-in

There is a hard limit on what can be built without a live capture of a
specific portal, and the feature is shaped around that rather than guessing
at one vendor's form:

1. **Form discovery (default).** `discoverLoginForm()` reads the splash
   page's own `<form>` — its action, method, and every named input. Hidden
   inputs keep their values (a portal's CSRF/session token lives there), a
   valueless checkbox is sent as `on` (accepting the terms is the point of
   pressing Connect), and the first input whose `type` is `email`/`tel` — or
   whose name contains `email`, `phone`, `user`, and so on — receives the
   saved identity. For an ordinary "type your email and press Connect"
   portal this is enough with no per-network configuration at all, because
   the page says what it wants.
2. **Manual override.** When discovery isn't enough, two settings-portal
   fields (`bus_form_url`, `bus_form_fld`) carry a submit URL and field name
   read off one real capture. These win over anything discovered.

If a form is found but nothing in it reads as an identity field, and no
override is set, the attempt **stops** rather than posting a guess at the
portal.

### Verifying, not assuming

A 200 on the login POST proves nothing — portals return 200 for a rejected
sign-in as readily as an accepted one. `connect()` re-probes afterward and
only reports `online` when a canary comes back clean. If the attempt fails,
`main.cpp` drops the association rather than leaving the radio camped on a
network nothing can be fetched through.

### Capturing a portal's form by hand

Only needed if automatic discovery doesn't work.

1. Join the network from a phone or laptop and let the sign-in page open.
2. View source (on a phone: prefix the URL with `view-source:`, or use
   Share → Desktop browser). For these simple portals the form is usually
   plain HTML, no developer tools needed.
3. Find `<form action="...">` — that's the submit URL. If it's relative
   (`/login`), prepend the page's own scheme and host.
4. Find the input you'd type your email into and read its `name="..."`.
5. Enter those two values in the settings portal's **Advanced** fields.

### Known limitations

- **Forms built by JavaScript are invisible to discovery.** The scanner is a
  small tag reader, not a browser; a page that constructs its form at
  runtime needs the manual override.
- **Only the first `<form>` on the page is considered.** A page whose search
  box comes before its login form would need the override.
- **Portals wanting a real account** (username *and* password, a room
  number, a voucher code) are out of scope. This handles the
  "email-or-phone, submit, you're in" shape only.
- **Per-ride re-authentication** isn't tracked specially — each wake probes
  and, if captured, signs in again. That's the right behavior for a session
  that expires, at the cost of a redundant POST on one that hasn't.
- **No STA-specific knowledge is baked in.** Whether Spokane Transit
  currently offers onboard Wi-Fi on any given route was not confirmed while
  building this; a public search turned up no evidence that it does. The
  feature is generic, works on any open network with a simple portal, and is
  off entirely unless an SSID is configured.

### Security notes

- The board **never** joins an open network on its own. `bus_ssid` empty
  (the default) means the whole path is skipped.
- The identity (`bus_ident`) is stored in NVS alongside the Wi-Fi password
  and API key and, like them, is never compiled into tracked source. Unlike
  those two, the settings page *does* read it back, so the field stays
  editable — it's an email address the user just typed on the board's own
  AP, not a credential.
- Traffic over an open network is unencrypted at the link layer. The Transit
  API and icon CDN calls are HTTPS, so their contents stay protected; the
  canary probes are plain HTTP by design and carry nothing but the request
  itself.
- Certificate verification is still `setInsecure()` for HTTPS
  (`http_transport.cpp`'s existing TODO on pinning the Transit CA). That
  matters more on an untrusted network than on a home one — worth revisiting
  before relying on this heavily.

## Configuration

All of it lives in the settings portal (a long power-button hold at boot on
an already-provisioned board), as the fourth and final step after
orientation, STA, and presets. Nothing here is part of first-run setup.

See `CONFIG_AND_STATE.md` for the NVS keys.

## Call budget

Unchanged on a normal wake. The captive-portal path only runs when the home
network fails, and costs at most: 1-3 canary probes, up to one splash-page
fetch, one form submission, and 1-3 confirming probes. None of those touch
the Transit API, so none count against the 1,500 calls/month free tier
(`DEPLOYMENT_OPS.md`). An offline wake makes *fewer* API calls than a normal
one — zero.
