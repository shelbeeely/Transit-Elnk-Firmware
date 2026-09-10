# CLAUDE.md — Transit API key setup

This project talks to the Transit API (v4, https://external.transitapp.com)
to show departure times. Before any other setup work, walk the user through
getting and configuring their API key as a short interview — don't just
link the request page and move on.

## 1. Check for an existing key
Ask: "Do you already have a Transit API key?"
- Yes → ask them to paste it. Acknowledge receipt without printing it back
  in full (e.g. "Got it, key ending in ...ab12"). Skip to step 3.
- No → continue to step 2.

## 2. Request one
Explain, in your own words:
- Access is requested at transitapp.com/apis — click "Request free access"
  and fill out their form.
- This is manually reviewed, not instant — Transit's own copy says "We'll
  send you an access key," with no published turnaround time. Set that
  expectation plainly; this step won't finish in one session.
- Ask if they'd like a reminder to check back, or if they'll return and
  paste the key in once it arrives (resume at step 3 whenever that is).
- If their use case needs more than the free tier (see 2b), tell them to
  mention it in the form, or follow up directly at partners@transit.app —
  don't let them discover the cap after the fact.

## 2b. Free vs. paid tier
Ask two questions to size the use case:
1. "How many of these displays are you running, and on what refresh interval?"
2. "Is this always-on, or checked occasionally?"

The free tier is 5 calls/min and 1,500 calls/month (transitapp.com/apis).
One device polling every 15-30 min is roughly 720-1,440 calls/month —
comfortably inside the cap. Anything faster than ~1 call per 29 minutes
sustained 24/7, or multiple devices sharing one key, will not fit the free
tier — say so plainly and point them to partners@transit.app rather than
let them find out via a failed request later.

## 3. Store it — never commit it
Where the key goes depends on what's running:
- Node reference apps (Transit-NearbyWebWidget / Transit-TV): a .env file
  in the repo root, API_KEY=<key>. Both already .gitignore .env —
  confirm that's still true before the key goes in, don't assume it.
- ESP32 firmware: entered on-device during first-run setup and stored in
  NVS — never compiled into tracked source. If a key is needed for local
  development before that setup screen exists, put it in an untracked file
  (e.g. secrets.h, added to .gitignore in the same commit that introduces
  it) and read it from there.

Confirm the relevant .gitignore entry exists before the user pastes a key
into any file — add it first if it's missing.

## 4. Validate
Offer a one-off test call so a bad key doesn't surface later as a
confusing bug:

curl -s "https://external.transitapp.com/v4/public/nearby_routes?lat=45.5017&lon=-73.5673&max_distance=500" \
  -H "apiKey: $API_KEY"

A 200 with a nearby_routes array means it's working. A 401/403 usually
means the key is wrong, or freshly issued and not yet propagated — don't
assume it's broken immediately.

## Guardrails
- Never print a full API key in chat output, logs, or commit messages —
  truncate to the last few characters when acknowledging it.
- Never commit a real key to any tracked file, including as a "default"
  or "example" value.
- Don't invent rate-limit numbers you're not sure of — 5/min and
  1,500/month are what's currently published; point the user to
  transitapp.com/apis to double check if it matters.
