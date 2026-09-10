# ASSETS_ICONS.md — route icons, color, and the conversion path to e-ink

## Source: `DisplayShortName` (unchanged in v4)

`route.route_display_short_name` (and the tighter-layout
`compact_display_short_name` alongside it) has this shape, per the v4
OpenAPI spec (see `DATA_MODEL.md`):

```json
{
  "elements": [ "stm-metro", "", "stm-metro-2" ],
  "route_name_redundancy": false,
  "boxed_text": ""
}
```

`elements` is always exactly 3 entries:

- `elements[0]` — optional left-side image slug (nullable)
- `elements[1]` — the textual route label (non-nullable, may be empty string)
- `elements[2]` — optional right-side image slug (nullable)

If `route_display_short_name` is entirely absent, both existing repos fall
back to plain `route.short_name` text with no image
(`routes_tmpl.dot`: `{{??}} {{= route.short_name }}`;
`routeItem.directive.js`: same fallback implied by `getImageSize`/
`getImageUrl` returning nothing when the field is missing).

## Image slug → URL, and the three suffix variants

Both repos independently confirm the same base path and slug convention;
the v4 spec's `DisplayShortName` description documents it explicitly and
adds the two variants neither repo currently uses:

```
https://transitapp-data.com/images/svgx/{slug}{suffix}.svg
```

| Suffix | Meaning | Used by |
|---|---|---|
| `-mono` | Non-transparent shapes tinted with `route_text_color` (or forced black) | Transit-NearbyWebWidget (`routes/index.js`'s `/images/:name.svg` route redirects unconditionally to `-mono`) |
| `-color-light` | Shapes colored `#010101` tint to `route_color`, shapes colored `#FEFEFE` tint to `route_text_color` | Transit-TV (`image.controller.js` proxies this variant, then string-replaces `#010101`/`#EFEFEF` placeholders with the requested `primaryColor`/`secondaryColor` query params server-side) |
| `-color-dark` | Same idea as `-color-light`, tuned for dark-mode legibility | **New in v4 spec docs** — neither existing repo requests this variant |

Transit-TV's proxy (`server/api/image/image.controller.js`) is the more
general of the two approaches: it fetches the raw `-mono` or `-color-light`
SVG from `transitapp-data.com`, then does a literal string replace of the
placeholder hex colors (`#010101` → requested `primaryColor`,
`#EFEFEF` → requested `secondaryColor`) before serving it — i.e. the actual
tinting happens client-request-time, server-side, via text substitution on
the SVG source, not a build-time asset. `useBlackText(route)` (true when
`route_text_color === '000000'`) decides whether to pass `000000` or the
route's own hex as `primaryColor`.

## Firmware translation

The X4 panel is 4-level grayscale with no on-device SVG renderer and a
380KB-SRAM budget — SVG fetch-and-recolor-at-request-time (Transit-TV's
approach) is not viable at runtime. The plan's proposed pipeline:

1. **Fetch the `-mono` variant** (matches the widget's approach, not
   Transit-TV's colored variant) — it's already a single-tone shape
   design intended to be tinted by one color, which maps directly onto a
   1-bit-shape + gray-fill model instead of the two-color light/dark
   scheme `-color-light`/`-color-dark` assume.
2. **Convert offline, not on-device**: a build-time or setup-time step
   (not part of the P0–P4 roadmap's runtime code) rasterizes each needed
   `-mono` SVG to a small monochrome bitmap at the panel's icon display
   size (28px or 34px square, matching the widget's own `size` logic in
   `routes_tmpl.dot`: `it.disp.elements[1] ? 28 : 34` — 28px when there's
   also a text label to fit alongside the icon, 34px when the icon is
   alone).
3. **Tint at render time, not conversion time**: keep the bitmap as a plain
   alpha mask (shape vs. transparent) and apply the route's tint as a
   1-of-4-gray-levels fill when compositing onto the framebuffer — this is
   the cheap, correct equivalent of "tint the mono SVG with
   `route_text_color`," done in the `RenderEngine` instead of pre-baked
   per-color bitmap variants (which would multiply storage 4x for no
   benefit, since the color space is only 4 grays).

## Route-color badge fill (background, not the icon)

`route_color` is the route's own background badge color
(`route_style` def in `routes_tmpl.dot`: `background: #{{route_color}};
color: #{{route_text_color}}`; `getCellStyle()` in
`routeItem.directive.js` — identical). This is a full hex RGB, needing
quantization for a 4-gray panel — independent of the icon/text-label layer
above:

1. Convert `route_color` hex to luminance:
   `L = 0.299*R + 0.587*G + 0.114*B` (standard perceptual luma weights).
2. Quantize `L` to the nearest of the 4 supported gray levels: `0`, `85`,
   `170`, `255`.
3. Pick badge text/icon tint the same way both repos already decide
   black-vs-white text: check `route_text_color === '000000'`
   (`darkText` in `routes_tmpl.dot`, `useBlackText()` in
   `routeItem.directive.js`) — but on a 4-gray panel, quantize
   `route_text_color` the same way as `route_color` rather than assuming
   pure black/white, since GTFS feeds occasionally set a text color other
   than `000000`/`ffffff`.

**Collision case flagged in the plan**: multiple routes with visually
distinct hex colors can quantize to the same gray bucket (e.g. a red route
and a green route of similar luminance both become "level 2 gray"). When
that happens for routes shown together on one panel, fall back to
rendering `route_short_name` as a text overlay on the badge (both existing
repos already show `route_short_name`/`short_name` as the fallback when
`route_display_short_name` is absent entirely — reuse the same fallback
path for a color collision, not just a missing-field case).

## `boxed_text`

Rare additional text shown in a colored box after the route name/icon
(v4 spec example: `boxed_text: "Saint-Laurent"` alongside a `"55"` bus
badge). Neither existing repo's reviewed templates render this field —
optional for the firmware to support; skip for P0–P4 and revisit only if a
real feed is seen using it non-trivially.
