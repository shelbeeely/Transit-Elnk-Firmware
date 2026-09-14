// Host-side tests for ota_update.h's manifest parsing, safety gates and
// rollback bookkeeping.
//
// Two properties here are load-bearing rather than cosmetic:
//
//   * A manifest missing or mangling its sha256 must be REJECTED, not
//     accepted with the integrity check quietly skipped. A malformed
//     manifest buying itself a weaker install than a well-formed one is
//     exactly backwards.
//   * The trial-boot counter must terminate. An image that panics before it
//     can write anything still has to burn a trial, or a board that crashes
//     early loops forever on a known-bad image -- which is the failure the
//     whole rollback mechanism exists to prevent.

#include <unity.h>

#include <string>

#include "transit/ota_update.h"

using transit::decideOtaUpdate;
using transit::evaluateOtaTrialBoot;
using transit::kMaxConsecutiveOtaFailures;
using transit::kMaxImageBytes;
using transit::kMaxOtaTrialBoots;
using transit::kMinBatteryPercentForOta;
using transit::OtaDecision;
using transit::OtaGateInputs;
using transit::OtaManifest;
using transit::otaTrialStateAfterBoot;
using transit::otaTrialStateAfterSuccess;
using transit::otaTrialStateForNewImage;
using transit::OtaTrialAction;
using transit::OtaTrialState;
using transit::parseOtaManifest;

namespace {

const char* kDigest = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";

std::string manifestJson(const char* version, const char* url, const char* sha256,
                         long long size) {
  std::string out = "{";
  out += "\"version\":\"" + std::string(version) + "\",";
  out += "\"url\":\"" + std::string(url) + "\",";
  out += "\"sha256\":\"" + std::string(sha256) + "\",";
  out += "\"size\":" + std::to_string(size);
  out += "}";
  return out;
}

OtaManifest goodManifest() {
  OtaManifest m;
  m.version = "v2";
  m.url = "https://example.test/firmware.bin";
  m.sha256 = kDigest;
  m.sizeBytes = 1400000;
  return m;
}

OtaGateInputs readyInputs() {
  OtaGateInputs in;
  in.manifestConfigured = true;
  in.manifestFetched = true;
  in.manifest = goodManifest();
  in.runningVersion = "v1";
  in.batteryPercent = 90;
  return in;
}

void test_parses_a_well_formed_manifest() {
  OtaManifest out;
  TEST_ASSERT_TRUE(parseOtaManifest(
      manifestJson("v1.2.3", "https://example.test/fw.bin", kDigest, 1400000), out));
  TEST_ASSERT_EQUAL_STRING("v1.2.3", out.version.c_str());
  TEST_ASSERT_EQUAL_STRING("https://example.test/fw.bin", out.url.c_str());
  TEST_ASSERT_EQUAL_STRING(kDigest, out.sha256.c_str());
  TEST_ASSERT_EQUAL_INT64(1400000, out.sizeBytes);
}

void test_rejects_a_manifest_with_no_digest() {
  OtaManifest out;
  // The dangerous failure mode: accepting this and skipping verification.
  TEST_ASSERT_FALSE(
      parseOtaManifest(manifestJson("v1", "https://example.test/fw.bin", "", 1400000), out));
}

void test_rejects_a_malformed_digest() {
  OtaManifest out;
  // Right length, wrong alphabet.
  const std::string upper(64, 'A');
  TEST_ASSERT_FALSE(parseOtaManifest(
      manifestJson("v1", "https://example.test/fw.bin", upper.c_str(), 1400000), out));
  // Right alphabet, wrong length -- a truncated paste.
  const std::string shortHex(32, 'a');
  TEST_ASSERT_FALSE(parseOtaManifest(
      manifestJson("v1", "https://example.test/fw.bin", shortHex.c_str(), 1400000), out));
}

void test_rejects_plain_http() {
  OtaManifest out;
  // A digest carried over the same unauthenticated channel as the image it
  // describes is no defence at all.
  TEST_ASSERT_FALSE(
      parseOtaManifest(manifestJson("v1", "http://example.test/fw.bin", kDigest, 140), out));
}

void test_rejects_missing_size_and_malformed_json() {
  OtaManifest out;
  TEST_ASSERT_FALSE(parseOtaManifest(
      std::string("{\"version\":\"v1\",\"url\":\"https://e.test/f.bin\",\"sha256\":\"") + kDigest +
          "\"}",
      out));
  TEST_ASSERT_FALSE(parseOtaManifest("not json at all", out));
  TEST_ASSERT_FALSE(parseOtaManifest("", out));
}

void test_leaves_output_untouched_on_a_bad_manifest() {
  OtaManifest out;
  out.version = "previously parsed";
  TEST_ASSERT_FALSE(parseOtaManifest("{}", out));
  TEST_ASSERT_EQUAL_STRING("previously parsed", out.version.c_str());
}

void test_gate_proceeds_when_everything_is_in_order() {
  TEST_ASSERT_TRUE(decideOtaUpdate(readyInputs()) == OtaDecision::kProceed);
}

void test_gate_names_each_refusal() {
  OtaGateInputs in;
  TEST_ASSERT_TRUE(decideOtaUpdate(in) == OtaDecision::kNotConfigured);

  in.manifestConfigured = true;
  TEST_ASSERT_TRUE(decideOtaUpdate(in) == OtaDecision::kManifestUnavailable);

  in = readyInputs();
  in.runningVersion = in.manifest.version;
  TEST_ASSERT_TRUE(decideOtaUpdate(in) == OtaDecision::kAlreadyCurrent);

  in = readyInputs();
  in.manifest.sizeBytes = kMaxImageBytes + 1;
  TEST_ASSERT_TRUE(decideOtaUpdate(in) == OtaDecision::kImageTooLarge);
}

void test_gate_blocks_on_low_battery_but_not_on_external_power() {
  OtaGateInputs in = readyInputs();
  in.batteryPercent = kMinBatteryPercentForOta - 1;
  TEST_ASSERT_TRUE(decideOtaUpdate(in) == OtaDecision::kBatteryTooLow);

  // A board on USB cannot lose its supply mid-erase, which is the whole
  // reason for the gate.
  in.externalPower = true;
  TEST_ASSERT_TRUE(decideOtaUpdate(in) == OtaDecision::kProceed);
}

void test_gate_does_not_block_a_board_that_cannot_measure_its_battery() {
  OtaGateInputs in = readyInputs();
  in.batteryPercent = -1;  // no telemetry on this board profile
  // Reading "unknown" as 0 % would disable OTA forever on such a board.
  TEST_ASSERT_TRUE(decideOtaUpdate(in) == OtaDecision::kProceed);
}

void test_gate_backs_off_after_repeated_failures() {
  OtaGateInputs in = readyInputs();
  in.consecutiveFailures = kMaxConsecutiveOtaFailures;
  in.failingVersion = in.manifest.version;
  TEST_ASSERT_TRUE(decideOtaUpdate(in) == OtaDecision::kBackingOff);

  in.consecutiveFailures = kMaxConsecutiveOtaFailures - 1;
  TEST_ASSERT_TRUE(decideOtaUpdate(in) == OtaDecision::kProceed);
}

void test_a_newly_published_version_clears_the_backoff() {
  OtaGateInputs in = readyInputs();
  in.consecutiveFailures = kMaxConsecutiveOtaFailures + 5;
  in.failingVersion = "v1-broken-build";
  in.manifest.version = "v2-the-fix";
  // Refusing to try the build that fixes whatever caused the failures would
  // be exactly the wrong way round.
  TEST_ASSERT_TRUE(decideOtaUpdate(in) == OtaDecision::kProceed);
}

void test_no_trial_pending_is_the_steady_state() {
  OtaTrialState state;
  TEST_ASSERT_TRUE(evaluateOtaTrialBoot(state) == OtaTrialAction::kNothingPending);
  // Counting boots when nothing is on trial would eventually trip a
  // rollback on a board that never updated.
  TEST_ASSERT_EQUAL_INT(0, otaTrialStateAfterBoot(state).bootsAttempted);
}

void test_a_new_image_starts_a_trial_at_zero() {
  const OtaTrialState state = otaTrialStateForNewImage("v2");
  TEST_ASSERT_EQUAL_STRING("v2", state.pendingVersion.c_str());
  TEST_ASSERT_EQUAL_INT(0, state.bootsAttempted);
  TEST_ASSERT_TRUE(evaluateOtaTrialBoot(state) == OtaTrialAction::kContinueTrial);
}

void test_a_successful_cycle_ends_the_trial() {
  OtaTrialState state = otaTrialStateForNewImage("v2");
  state = otaTrialStateAfterBoot(state);
  state = otaTrialStateAfterSuccess(state);
  TEST_ASSERT_TRUE(state.pendingVersion.empty());
  TEST_ASSERT_TRUE(evaluateOtaTrialBoot(state) == OtaTrialAction::kNothingPending);
}

void test_a_crashing_image_is_rolled_back_after_a_bounded_number_of_boots() {
  // Models the real sequence: each boot evaluates the *persisted* count,
  // then immediately persists the increment, then crashes before doing
  // anything else. This must terminate.
  OtaTrialState state = otaTrialStateForNewImage("v2-bad");
  int trials = 0;
  for (int i = 0; i < 20; ++i) {
    const OtaTrialAction action = evaluateOtaTrialBoot(state);
    if (action == OtaTrialAction::kRollBack) break;
    TEST_ASSERT_TRUE(action == OtaTrialAction::kContinueTrial);
    state = otaTrialStateAfterBoot(state);  // persisted before the risky work
    ++trials;
  }
  TEST_ASSERT_EQUAL_INT(kMaxOtaTrialBoots, trials);
  TEST_ASSERT_TRUE(evaluateOtaTrialBoot(state) == OtaTrialAction::kRollBack);
}

void test_an_image_that_works_on_its_last_chance_is_not_rolled_back() {
  OtaTrialState state = otaTrialStateForNewImage("v2");
  for (int i = 0; i < kMaxOtaTrialBoots - 1; ++i) {
    state = otaTrialStateAfterBoot(state);
  }
  TEST_ASSERT_TRUE(evaluateOtaTrialBoot(state) == OtaTrialAction::kContinueTrial);
  state = otaTrialStateAfterSuccess(state);
  TEST_ASSERT_TRUE(evaluateOtaTrialBoot(state) == OtaTrialAction::kNothingPending);
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_parses_a_well_formed_manifest);
  RUN_TEST(test_rejects_a_manifest_with_no_digest);
  RUN_TEST(test_rejects_a_malformed_digest);
  RUN_TEST(test_rejects_plain_http);
  RUN_TEST(test_rejects_missing_size_and_malformed_json);
  RUN_TEST(test_leaves_output_untouched_on_a_bad_manifest);
  RUN_TEST(test_gate_proceeds_when_everything_is_in_order);
  RUN_TEST(test_gate_names_each_refusal);
  RUN_TEST(test_gate_blocks_on_low_battery_but_not_on_external_power);
  RUN_TEST(test_gate_does_not_block_a_board_that_cannot_measure_its_battery);
  RUN_TEST(test_gate_backs_off_after_repeated_failures);
  RUN_TEST(test_a_newly_published_version_clears_the_backoff);
  RUN_TEST(test_no_trial_pending_is_the_steady_state);
  RUN_TEST(test_a_new_image_starts_a_trial_at_zero);
  RUN_TEST(test_a_successful_cycle_ends_the_trial);
  RUN_TEST(test_a_crashing_image_is_rolled_back_after_a_bounded_number_of_boots);
  RUN_TEST(test_an_image_that_works_on_its_last_chance_is_not_rolled_back);
  return UNITY_END();
}
