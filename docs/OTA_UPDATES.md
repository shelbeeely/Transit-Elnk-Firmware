# Over-the-air firmware updates

This firmware can update itself two ways. They exist for different moments,
and neither is the ArduinoOTA shape you'll find in most ESP32 tutorials —
for a reason worth stating up front.

## Why this doesn't look like the usual ESP32 OTA example

Nearly every ArduinoOTA / ElegantOTA walkthrough assumes an always-on device
whose `loop()` calls an OTA handler thousands of times a second, so a laptop
can push an image whenever it likes.

This board is the opposite. `setup()` runs once, does its work in about nine
seconds, and ends in deep sleep. There is no `loop()`. At the default
60-minute refresh interval the device is reachable roughly **0.25% of the
time**, and it doesn't announce when. A push-only design would mean waiting
for a nine-second window once an hour and catching it by hand.

So:

| | **Pull** | **Push** |
|---|---|---|
| For | Deployed boards | A board on your desk |
| Trigger | Once per wake, if a manifest URL is set | You, in the settings portal |
| Needs | Outbound HTTPS only — no inbound reachability, works behind NAT | Physical access to hold the power button |
| Cost when idle | One small HTTP request per wake | Nothing |

## Push: uploading a .bin from the settings portal

The settings portal is the one time this firmware is awake and serving HTTP
for longer than a few seconds, so that's where a browser upload belongs.

1. Hold the **power button** at boot for 3 seconds.
2. Join the board's Wi-Fi access point and open the portal.
3. Step through to **Firmware**. It shows the running version and which OTA
   slot it booted from.
4. Choose `.pio/build/xteink_x4/firmware.bin` and press **Install**.

The image streams straight into the inactive OTA partition as it uploads —
it is over a megabyte and there is nowhere near that much heap, so it is
never buffered. When the upload completes, the board arms the rollback trial
(below) and reboots into the new image.

## Pull: automatic updates from a manifest

Set a **manifest URL** in the portal's Firmware step. Blank — the default —
means the pull path is off entirely and the board never reaches out for
firmware.

Once per wake, after the panel has been redrawn, the board fetches that URL
and decides what to do. Redrawing first is deliberate: an update that
reboots the board mid-cycle must not cost the reader their departure board
for the next hour.

### Manifest schema

```json
{
  "version": "v0.1.0",
  "url": "https://example.com/firmware.bin",
  "sha256": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
  "size": 1442304,
  "notes": "optional, shown in the portal and the serial log"
}
```

Every field except `notes` is required, and a manifest missing any of them
is rejected rather than partially honored. In particular:

- **`sha256` is not optional.** A manifest that forgot its digest must not
  buy itself a weaker install than one that remembered — that would be
  exactly backwards. It must be 64 lowercase hex characters; an uppercase
  digest or a truncated paste is refused at parse time, before a megabyte
  has been downloaded onto a battery-powered board.
- **`url` must be `https://`.** An image fetched over plain HTTP is an image
  any intermediary can replace.
- **`version` is compared verbatim** against the running build's
  `FREEINK_FW_VERSION` (baked in from `git describe` by
  `tools/pio_version.py`). There is no ordering comparison, so a manifest
  can move a fleet backwards as easily as forwards — useful for a recall.

### Publishing one

`.github/workflows/release.yml` does it. Push a tag:

```sh
git tag v0.1.0 && git push origin v0.1.0
```

CI builds `[env:xteink_x4]`, computes the digest and size, generates
`manifest.json`, checks the image still fits the OTA slot, and publishes
both as release assets. Then point boards at:

```
https://github.com/<owner>/<repo>/releases/latest/download/manifest.json
```

which redirects to whichever release is newest.

## The safety gates, in the order things actually go wrong

### 1. Battery

An erase/write cycle that browns out mid-flash is the one failure here that
can leave a board needing a cable to recover. The pull path declines below
**50%** (`kMinBatteryPercentForOta`). Being plugged in or charging overrides
that — a board on USB is not going to lose its supply mid-write.

A board whose profile can't measure its battery at all reports `-1` and is
**not** gated. Reading "unknown" as 0% would disable OTA forever on such a
board, which is a worse failure than the one being guarded against.

### 2. Size

The image must fit an OTA slot: `0x640000` (~6.3 MB), per `partitions.csv`.
Checked against the manifest before a single byte is downloaded, and again
while streaming, because a manifest can lie. The release workflow also
checks it at publish time.

### 3. Integrity

SHA-256 over the received image, compared with the manifest **before**
`Update.end()` switches the boot partition. Ending first and checking after
would leave a failed verification pointing the bootloader at a bad image —
the rollback below would eventually recover it, but only after three failed
boots, for a problem that was detectable immediately.

A truncated download that happens to end on a flash-page boundary is
otherwise a perfectly valid-looking image.

### 4. Rollback

An image can be intact and still not work — a bad config read, a panic in a
new module. The ESP-IDF bootloader's own automatic rollback needs
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, which this build's prebuilt Arduino
framework doesn't set, so the firmware does it in software:

1. A freshly-installed image boots **on trial**.
2. At the very start of each boot — before the SD mount, the panel, the
   radio, before anything that could panic — the trial count is read and
   **immediately incremented in NVS**. This ordering is the whole trick: an
   image that crashes before it can write anything still burns a trial, so a
   boot loop terminates instead of retrying forever.
3. Completing one **full wake cycle** clears the trial. Not "booting" —
   completing the thing the firmware exists to do. Anything earlier would
   pass an image that boots and then fails at its actual job.
4. After `kMaxOtaTrialBoots` (3) failed attempts the board marks the
   previous slot bootable again and restarts into it. Three rather than one
   because a single failure can be environmental — no Wi-Fi at that moment,
   an API outage — and reverting over a bad afternoon would be worse than
   the disease.

If the other slot has never been written (a board that has only ever run its
factory image), the rollback reports that it can't proceed rather than
pointing the bootloader at blank flash. That turns a bad update into a board
you reflash over USB, not a dead one.

### 5. Backoff

Three consecutive download or verification failures stop the pull path from
retrying every wake. The count belongs to a **specific version**: a newly
published build clears it, because refusing to try the build that fixes
whatever caused the failures would be the wrong way round.

## What the serial log tells you

Both are in the wake summary (`docs/HARDWARE_BRINGUP.md`):

```
[ota]
  decision               already current
  available              v0.1.0
```

Every refusal is named — `not configured`, `manifest unavailable`, `already
current`, `image too large`, `battery too low`, `backing off after repeated
failures` — so "my board never updates" is answerable. The block is omitted
entirely when the cycle never got as far as checking, which is a different
thing from checking and declining.

The boot report shows the running partition and, while it applies, the
trial:

```
[ota]
  running from           app1
  pull configured        yes
  ON TRIAL               v0.2.0 (boot 2)
```

"Why did my board revert" is unanswerable unless that line is visible while
it's happening.

## Known limitations

- **TLS is not pinned.** `applyOtaFromUrl()` uses `WiFiClientSecure` with
  `setInsecure()`, matching `http_transport.cpp`'s existing posture rather
  than quietly introducing a second, different one. This is the weakest link
  in the chain: an attacker who can terminate TLS can serve both the
  manifest and a matching image, and the SHA-256 check then verifies only
  that the two agree with each other. **Point `ota_url` at a host you
  control.** Pinning the CA, or signing the manifest with a key baked into
  the image, is the real fix and is not implemented.
- **No signature on the image itself.** Secure Boot and signed app images
  are an ESP-IDF feature this Arduino-framework build does not enable.
- **The pull check costs one HTTP request per wake** when configured. At the
  default 60-minute interval that's ~720 requests a month to whatever hosts
  the manifest. It does not touch the Transit API and so has no effect on
  that free-tier budget (`docs/DEPLOYMENT_OPS.md`).
- **Version comparison is equality, not ordering.** See above.
- **The first flash still needs a cable.** OTA can only replace firmware
  that is already running and already on a network.
