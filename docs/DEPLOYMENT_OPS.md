# DEPLOYMENT_OPS.md — API key tier, "continuous" reality check, power budget

Ported from Transit-TV's `FAQ.md` and `DEPLOYING.md` — those two files are
the only place either repo documents operational requirements, and their
guidance transfers to the firmware essentially unchanged in substance, even
though the *mechanism* (Node hosting vs. an on-device NVS-stored key)
differs completely.

## The paid-key requirement, verbatim from what already exists

Transit-TV's `FAQ.md`, "Do I need a paid API key?":

> Yes, for 24/7 operation. The free Transit API tier has rate limits that
> won't sustain continuous polling.

`DEPLOYING.md` lists a paid plan as "recommended for 24/7 use" alongside the
Railway/hosting prerequisites. Neither document states the actual numbers —
those come from `transitapp.com/apis` directly (see root `CLAUDE.md`):
**5 calls/min, 1,500 calls/month** on the free tier.

## Why the web apps' own polling intervals don't fit the free tier

Both existing repos poll far faster than the free tier allows for 24/7
operation:

| App | Interval | Calls/month if run 24/7 |
|---|---|---|
| Transit-NearbyWebWidget | 30s | ~86,400 |
| Transit-TV | 20s | ~129,600 |

Both are roughly **60–90x** over the 1,500/month free-tier cap. This isn't a
firmware-specific constraint to work around — it's the exact reason
Transit-TV's own FAQ tells operators to get a paid key for 24/7 use. The
firmware inherits the same math, just with a battery-powered device instead
of an always-on browser tab making it more visible.

## What actually fits the free tier

1,500 calls/month ÷ 30 days ÷ 24 hours ≈ **1 call every ~29 minutes**
sustained continuously — that's the loosest bound the free tier allows.
The firmware's chosen default sits well inside it rather than riding the
line:

| Cadence | Calls/month (24/7) | Headroom vs. 1,500 cap |
|---|---|---|
| **60 min (chosen default, awake)** | ~720 | ~2.1x |
| 30 min | ~1,440 | ~1.04x — essentially no margin for setup calls, retries, or clock drift |
| 15 min | ~2,880 | Over the cap — needs a paid key |

**Recommendation carried into `CONFIG_AND_STATE.md`'s `refresh_interval_min`
setting**: default to **60 minutes**, treated as the *minimum* interval —
not a target to poll faster than absent a reason to. A user with a paid key
can dial it down; the firmware should not tempt a free-tier user toward
30 minutes just because the math technically allows it, since that leaves
almost no room for the setup flow's own validation call (root `CLAUDE.md`,
step 4) or an occasional retry without tipping over the cap. Never default
to anything resembling the web apps' 20–30 **second** cadence — that was
only ever viable because neither existing app is rate-limit-aware or
battery-constrained.

### Sleeping longer than the base interval

The 60-minute figure is the cadence while the device is in normal
awake/display use. When the device is otherwise asleep — a configured quiet
window (e.g. overnight, when no one is reading the panel) or a
lower-power/away mode the user selects — the firmware should stretch the
interval further rather than keep polling on the same schedule:

- A sleep-window multiplier (e.g. 2–4x the base interval, or a fixed
  "don't poll between HH:MM and HH:MM" range) cuts both API calls and
  Wi-Fi/redraw wake cycles during hours nobody's looking — the free tier
  gets more headroom for exactly the same reason the battery does.
- This is a natural fit for `PowerScheduler`'s fetch → render → deep-sleep →
  wake loop: the *wake* interval itself is what varies (60 min baseline,
  longer inside a sleep window), not just how much work happens per wake.
- Treat "how much longer while sleeping" as its own setting rather than a
  hardcoded multiple — someone running the display in a 24-hour space (a
  lobby, a shared kitchen) has no true "asleep" period at all, and the
  default should not assume one.

## Battery vs. freshness is a real, user-facing tradeoff

The X4's ~2-week battery baseline (see hardware specs in the root plan) is
measured against some assumed duty cycle — every refresh this device
performs costs both a Wi-Fi radio wake and a display redraw (full or
partial, see `PowerManager`/`RenderEngine` in the proposed architecture).
Shortening `refresh_interval_min` trades directly against that baseline.
Unlike the web apps (which run on mains power and never had to make this
tradeoff visible to a user), the firmware should surface it directly —
e.g. showing an estimated battery-life impact next to the refresh-interval
setting during setup, rather than picking a number silently.

## Deployment mechanism difference (context, not firmware-relevant)

`DEPLOYING.md` walks through forking Transit-TV to a Railway/Render/Fly.io/
Heroku Node host, setting `API_KEY` as a platform environment variable, and
exposing a public URL for a browser to load. None of that applies to the
firmware — there is no server to deploy, no public URL, and no browser.
The only piece that carries over is the *upstream* requirement (a valid,
appropriately-tiered Transit API key) — see root `CLAUDE.md` for the
firmware's own key-acquisition and on-device-storage flow.
