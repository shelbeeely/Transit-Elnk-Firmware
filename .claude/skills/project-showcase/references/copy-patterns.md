# Copy patterns — README-speak → product-speak

The goal isn't to make things sound fancier than they are. It's to answer "why should I care" before "how does it work." A few worked examples:

**Example 1**
- Input (README): "Uses a Cloudflare Worker with a D1 database for storage and a KV cache."
- Output (feature copy): "Fast by default — data lives close to the user, and repeat lookups skip the database entirely."
- *The infrastructure detail moves to the tech-stack section; the feature list gets the payoff instead.*

**Example 2**
- Input (README): "Implements a state machine for order processing (draft → submitted → approved)."
- Output: "Nothing falls through the cracks — every order moves through a clear, auditable set of stages from the moment it's created."

**Example 3**
- Input (README): "Written in TypeScript, ~4k LOC, includes unit tests."
- Output: This isn't a feature — it's a craft signal. Pair it with the tech-stack section, not the feature list.

**Example 4 — when there's nothing to reframe**
- Input: "No screenshots yet."
- Output: Don't invent a gallery section to fill space. Leave it out, or flag to the user that a screenshot or short demo GIF would meaningfully strengthen the page.

**Example 5 — features as benefits, not capabilities**
- Input (README): "Supports offline mode via a service worker and IndexedDB."
- Output: "Works with no signal — check in from a basement, a bus, or the middle of nowhere, and it picks up the second you're back online."

## When the user supplies their own copy

If the user hands over already-written feature descriptions, a pitch, or section text, use it directly. Don't rewrite it into "product voice" unprompted — they may be deliberately choosing their own words for a specific project. Only apply these patterns when generating copy from scratch off raw technical material.
