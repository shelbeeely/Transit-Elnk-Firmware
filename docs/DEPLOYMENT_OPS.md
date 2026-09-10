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
sustained continuously. A firmware default of **15–30 minute** polling:

- 15 min → ~2,880 calls/month (over the free cap — needs a paid key for
  strictly continuous 24/7 operation, or deep-sleep hours overnight to stay
  under it)
- 30 min → ~1,440 calls/month (fits comfortably under the free cap)

**Recommendation carried into `CONFIG_AND_STATE.md`'s `refresh_interval_min`
setting**: default to 30 minutes (free-tier-safe out of the box), let a
user with a paid key dial it down. Never default to anything resembling the
web apps' 20–30 **second** cadence — that was only ever viable because
neither existing app is rate-limit-aware or battery-constrained.

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
