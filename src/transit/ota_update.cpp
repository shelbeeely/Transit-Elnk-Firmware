// OTA manifest parsing, safety gates and rollback bookkeeping -- the pure,
// host-testable half of ota_update.h. The streaming download and the boot
// -partition switch live in src/transit/ota_applier.cpp, which is excluded
// from [env:native].

#include "transit/ota_update.h"

#include <ArduinoJson.h>

#include <cctype>

namespace transit {

namespace {

std::string getStr(JsonVariantConst v) {
  if (v.isNull()) return std::string();
  const char* s = v.as<const char*>();
  return s ? std::string(s) : std::string();
}

// A SHA-256 digest is exactly 64 lowercase hex characters. Anything else --
// an uppercase digest, a truncated paste, a placeholder like "TBD" -- is
// rejected at parse time rather than failing after a megabyte has already
// been downloaded onto a battery-powered board.
bool isSha256Hex(const std::string& value) {
  if (value.size() != 64) return false;
  for (const char c : value) {
    const bool isDigit = c >= '0' && c <= '9';
    const bool isLowerHex = c >= 'a' && c <= 'f';
    if (!isDigit && !isLowerHex) return false;
  }
  return true;
}

// Only https. A firmware image fetched over plain http is an image any
// intermediary can replace, and the SHA-256 in the manifest is no defence
// when the manifest came down the same unauthenticated channel.
bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

}  // namespace

bool parseOtaManifest(const std::string& json, OtaManifest& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;

  OtaManifest parsed;
  parsed.version = getStr(doc["version"]);
  parsed.url = getStr(doc["url"]);
  parsed.sha256 = getStr(doc["sha256"]);
  parsed.notes = getStr(doc["notes"]);
  parsed.sizeBytes = doc["size"].isNull() ? 0 : doc["size"].as<int64_t>();

  if (parsed.version.empty()) return false;
  if (!isHttpsUrl(parsed.url)) return false;
  // Absent integrity data is rejected rather than downgraded to "no check
  // requested" -- a manifest that forgot its digest must not silently buy
  // itself a weaker install than one that remembered.
  if (!isSha256Hex(parsed.sha256)) return false;
  if (parsed.sizeBytes <= 0) return false;

  out = parsed;
  return true;
}

const char* otaDecisionName(OtaDecision decision) {
  switch (decision) {
    case OtaDecision::kNotConfigured: return "not configured";
    case OtaDecision::kManifestUnavailable: return "manifest unavailable";
    case OtaDecision::kAlreadyCurrent: return "already current";
    case OtaDecision::kImageTooLarge: return "image too large";
    case OtaDecision::kBatteryTooLow: return "battery too low";
    case OtaDecision::kBackingOff: return "backing off after repeated failures";
    case OtaDecision::kProceed: return "proceeding";
  }
  return "unknown";
}

OtaDecision decideOtaUpdate(const OtaGateInputs& inputs) {
  if (!inputs.manifestConfigured) return OtaDecision::kNotConfigured;
  if (!inputs.manifestFetched) return OtaDecision::kManifestUnavailable;

  // Checked before anything else about the image, because "you are already
  // running it" is the overwhelmingly common answer and none of the other
  // gates are worth reporting when it applies.
  if (!inputs.runningVersion.empty() && inputs.manifest.version == inputs.runningVersion) {
    return OtaDecision::kAlreadyCurrent;
  }

  if (inputs.manifest.sizeBytes > kMaxImageBytes) return OtaDecision::kImageTooLarge;

  // The backoff belongs to a specific version. A newly published build gets
  // a clean slate, because three failures downloading yesterday's image say
  // nothing about today's -- and refusing to try the fix for the bug that
  // caused the failures would be the wrong way round.
  const bool failuresApplyToThisVersion =
      inputs.failingVersion.empty() || inputs.failingVersion == inputs.manifest.version;
  if (failuresApplyToThisVersion && inputs.consecutiveFailures >= kMaxConsecutiveOtaFailures) {
    return OtaDecision::kBackingOff;
  }

  // External power makes the battery irrelevant: nothing is going to lose
  // its supply mid-erase. A board with no battery telemetry at all reports
  // -1 and is likewise not gated -- refusing forever on a board that simply
  // cannot measure itself would be a worse failure than the one being
  // guarded against.
  const bool batteryKnown = inputs.batteryPercent >= 0;
  if (!inputs.externalPower && batteryKnown &&
      inputs.batteryPercent < kMinBatteryPercentForOta) {
    return OtaDecision::kBatteryTooLow;
  }

  return OtaDecision::kProceed;
}

OtaTrialAction evaluateOtaTrialBoot(const OtaTrialState& state) {
  if (state.pendingVersion.empty()) return OtaTrialAction::kNothingPending;
  // >= , not >: a state carrying kMaxOtaTrialBoots already-counted boots has
  // used every chance. Using > here would grant one extra trial and, more
  // to the point, would let an image that panics at exactly the wrong moment
  // loop forever a count behind the limit.
  if (state.bootsAttempted >= kMaxOtaTrialBoots) return OtaTrialAction::kRollBack;
  return OtaTrialAction::kContinueTrial;
}

OtaTrialState otaTrialStateAfterBoot(const OtaTrialState& state) {
  OtaTrialState next = state;
  if (next.pendingVersion.empty()) return next;
  next.bootsAttempted = state.bootsAttempted + 1;
  return next;
}

OtaTrialState otaTrialStateAfterSuccess(const OtaTrialState& /*state*/) {
  // Deliberately returns the empty state rather than clearing fields on a
  // copy: "the trial is over" is the whole content of a success, and
  // carrying anything forward from the trial would only be something else
  // to get stale.
  return OtaTrialState{};
}

OtaTrialState otaTrialStateForNewImage(const std::string& version) {
  OtaTrialState next;
  next.pendingVersion = version;
  next.bootsAttempted = 0;
  return next;
}

}  // namespace transit
