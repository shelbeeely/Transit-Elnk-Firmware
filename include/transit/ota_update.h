#pragma once

// Transit-Elnk-Firmware — over-the-air firmware updates.
//
// Why this doesn't look like the usual ESP32 OTA example
// -----------------------------------------------------
// Nearly every ArduinoOTA / ElegantOTA / AsyncElegantOTA walkthrough assumes
// an always-on device whose loop() calls an OTA handler thousands of times a
// second, so a laptop can push an image whenever it likes. This board is the
// opposite: setup() runs once, does its work in about nine seconds, and ends
// in deep sleep. There is no loop(). At the default 60-minute refresh
// interval the device is reachable roughly 0.25% of the time, and the window
// is not announced. A push-only design would mean waiting for a nine-second
// window once an hour and catching it by hand.
//
// So there are two paths, and they exist for different moments:
//
//   * PULL, for deployed boards. Once per wake, if a manifest URL is
//     configured, the firmware asks a URL whether a newer build exists and
//     applies it if so. Costs one HTTP request on wakes where nothing has
//     changed, needs no inbound reachability, and works through NAT.
//   * PUSH, for a board on your desk. The settings portal (setup_flow.h)
//     already stands up a SoftAP and a WebServer and stays awake while it is
//     open -- so that, not the nine-second wake, is where a browser upload
//     belongs. Hold the power button at boot, open the portal, drop a .bin
//     on it.
//
// Safety, in the order the failures actually happen
// ------------------------------------------------
//  1. BATTERY. An erase/write cycle that browns out mid-flash is the one
//     failure here that can leave a board needing a cable to recover.
//     kMinBatteryPercentForOta gates the pull path; charging overrides it,
//     because a board on USB is not going to lose power mid-write.
//  2. SIZE. The image must fit the OTA slot (partitions.csv: 0x640000).
//     Checked against the manifest before a single byte is downloaded, and
//     again while streaming, because a manifest can lie.
//  3. INTEGRITY. SHA-256 over the received image, compared with the
//     manifest, before the boot partition is switched. A truncated download
//     that happens to end on a flash-page boundary is otherwise a perfectly
//     valid-looking image.
//  4. ROLLBACK. An image can be intact and still not work -- a bad config
//     read, a panic in a new module. The ESP-IDF bootloader's own automatic
//     rollback needs CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, which this
//     build's prebuilt Arduino framework does not set, so this module does
//     it in software instead: a freshly-flashed image boots "on trial",
//     each trial boot is counted in NVS *before* the risky work, and an
//     image that fails to complete kMaxValidationBoots cycles gets the
//     previous slot marked bootable again. See evaluateOtaTrialBoot().
//
// Everything above the hardware line is pure and host-tested. The two
// hardware pieces -- streaming a download into the inactive partition, and
// switching boot partitions -- are behind FREEINK_HOST_NATIVE at the bottom
// of this header, the same split power_scheduler.h and time_keeper.h use.

#include <cstdint>
#include <string>

namespace transit {

// What an update manifest says. Served as JSON at the URL in ConfigStore's
// ota_url; see docs/OTA_UPDATES.md for the schema and how CI publishes one.
struct OtaManifest {
  std::string version;   // compared verbatim against the running build
  std::string url;       // absolute https:// URL of the .bin
  std::string sha256;    // lowercase hex, 64 chars; required
  int64_t sizeBytes = 0;  // required, and checked against the slot size
  std::string notes;     // optional, shown in the portal and serial log
};

// Parses a manifest document. Returns false and leaves `out` untouched for
// malformed JSON or any missing required field -- a manifest missing its
// sha256 is rejected rather than treated as "no integrity check requested",
// since the whole point of the field is that its absence must not be a
// silent downgrade in safety.
bool parseOtaManifest(const std::string& json, OtaManifest& out);

// One OTA slot from partitions.csv (app0/app1, 0x640000 each). An image
// larger than this cannot be written no matter what the manifest claims.
constexpr int64_t kMaxImageBytes = 0x640000;

// Below this, the pull path declines to start. A half-written app partition
// is recoverable (the other slot still boots) but a board that browns out
// mid-erase may not be, and an update is never urgent enough to risk that.
// Overridden by the board being on external power.
constexpr int kMinBatteryPercentForOta = 50;

// Consecutive download/verify failures before the pull path stops retrying
// every wake. Reset by any success, and by the manifest advertising a
// different version (a new build is worth a fresh try).
constexpr int kMaxConsecutiveOtaFailures = 3;

// How many boots a freshly-installed image gets to complete one full wake
// cycle before it is rolled back. More than one because a single failure
// can be environmental -- no Wi-Fi at that moment, an API outage -- and
// rolling back over a bad afternoon would be worse than the disease.
constexpr int kMaxOtaTrialBoots = 3;

// Why the pull path did or didn't run this wake. Every refusal is named so
// the wake summary can say which one happened rather than silently doing
// nothing -- "my board never updates" is otherwise unanswerable.
enum class OtaDecision {
  kNotConfigured,       // no manifest URL set
  kManifestUnavailable,  // fetch failed or the document didn't parse
  kAlreadyCurrent,       // manifest version == running version
  kImageTooLarge,        // manifest's size exceeds the OTA slot
  kBatteryTooLow,        // see kMinBatteryPercentForOta
  kBackingOff,           // too many consecutive failures for this version
  kProceed,
};

const char* otaDecisionName(OtaDecision decision);

struct OtaGateInputs {
  bool manifestConfigured = false;
  bool manifestFetched = false;
  OtaManifest manifest;
  std::string runningVersion;
  // -1 when the board has no battery telemetry. Treated as "unknown, do not
  // gate on it" rather than as 0 %, which would block OTA forever on a
  // board profile that simply cannot measure its own battery.
  int batteryPercent = -1;
  bool externalPower = false;
  int consecutiveFailures = 0;
  // The version the failure count belongs to. A newly published build
  // clears the backoff, since the old failures said nothing about it.
  std::string failingVersion;
};

OtaDecision decideOtaUpdate(const OtaGateInputs& inputs);

// --- software rollback bookkeeping ----------------------------------------

// Persisted across boots in NVS (ConfigStore's ota_pend_ver / ota_pend_n).
struct OtaTrialState {
  // Empty when no image is on trial -- the steady state.
  std::string pendingVersion;
  int bootsAttempted = 0;
};

enum class OtaTrialAction {
  kNothingPending,  // steady state; nothing to do
  kContinueTrial,   // count this boot and carry on
  kRollBack,        // the trial image has had its chances
};

// Call at the start of a boot, before anything that could panic. Deciding
// from the count *as persisted* (rather than after incrementing) is what
// makes a boot loop terminate: an image that panics before it can write
// anything still burns a trial, because otaTrialStateAfterBoot() below is
// written immediately after this returns and before the risky work starts.
OtaTrialAction evaluateOtaTrialBoot(const OtaTrialState& state);

// The state to persist immediately after evaluateOtaTrialBoot() returns
// kContinueTrial.
OtaTrialState otaTrialStateAfterBoot(const OtaTrialState& state);

// The state to persist once a wake cycle has actually completed -- i.e. the
// new image works. Clears the trial.
OtaTrialState otaTrialStateAfterSuccess(const OtaTrialState& state);

// The state to persist right after an image has been written and the boot
// partition switched, so the next boot knows it is on trial.
OtaTrialState otaTrialStateForNewImage(const std::string& version);

#ifndef FREEINK_HOST_NATIVE

// Progress callback: (bytesWritten, bytesTotal). Called every few KB so the
// serial log shows an update making progress rather than appearing hung for
// forty seconds.
using OtaProgressFn = void (*)(size_t bytesWritten, size_t bytesTotal);

struct OtaApplyResult {
  bool ok = false;
  std::string error;      // empty on success
  size_t bytesWritten = 0;
};

// Downloads manifest.url straight into the inactive OTA partition,
// verifying size and SHA-256, and switches the boot partition on success.
// Does NOT reboot -- the caller decides when (main.cpp finishes its cycle
// and persists the trial state first).
//
// Streams rather than buffering, deliberately and necessarily: the image is
// well over a megabyte and free heap on a C3 running this firmware is a
// couple of hundred kilobytes, so HttpTransport -- which accumulates a whole
// response body in a std::string -- cannot be used here at any size that
// matters.
OtaApplyResult applyOtaFromUrl(const OtaManifest& manifest, OtaProgressFn progress);

// Marks the *other* OTA slot bootable again, for evaluateOtaTrialBoot()'s
// kRollBack. Fails when there is no other valid slot (a board that has only
// ever run its factory image), which is reported rather than treated as
// success -- silently continuing to boot a known-bad image is the outcome
// this whole mechanism exists to prevent.
bool rollBackToPreviousSlot(std::string& error);

// Human-readable label for the partition this boot is running from
// ("app0"/"ota_0", ...), for the boot report.
std::string runningPartitionLabel();

#endif  // FREEINK_HOST_NATIVE

}  // namespace transit
