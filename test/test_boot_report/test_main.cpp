// Host-side tests for boot_report.h's diagnostic formatting.
//
// The load-bearing test in this file is the redaction one. Everything else
// here is cosmetic -- a misaligned column in a serial log costs nobody
// anything -- but a boot report that prints a Transit API key in full puts
// that key into every captured bringup log, every pasted-into-chat excerpt
// and, sooner or later, a committed file. CLAUDE.md's guardrail is that a
// key is never printed whole, and the only way to keep that true is to put
// the redaction inside the formatter (so no caller can forget it) and then
// assert here that the raw secret does not survive formatting.

#include <unity.h>

#include <string>

#include "transit/boot_report.h"

using transit::BootReport;
using transit::formatBootReport;
using transit::formatSelfTestReport;
using transit::formatWakeSummary;
using transit::redactSecret;
using transit::SelfTestReport;
using transit::SelfTestResult;
using transit::WakeSummary;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

SelfTestResult result(const char* name, bool passed, bool skipped = false) {
  SelfTestResult r;
  r.name = name;
  r.passed = passed;
  r.skipped = skipped;
  return r;
}

void test_redact_unset_secret() { TEST_ASSERT_EQUAL_STRING("(unset)", redactSecret("").c_str()); }

void test_redact_short_secret_shows_no_characters() {
  // Seven characters: showing the last four would be showing most of it.
  const std::string redacted = redactSecret("abc1234");
  TEST_ASSERT_EQUAL_STRING("(set, 7 chars)", redacted.c_str());
  TEST_ASSERT_FALSE(contains(redacted, "1234"));
}

void test_redact_long_secret_shows_only_last_four() {
  const std::string key = "d3f9a1c07b25e8461f0aab12";  // 24 chars
  const std::string redacted = redactSecret(key);
  TEST_ASSERT_EQUAL_STRING("(set, 24 chars, ...ab12)", redacted.c_str());
  TEST_ASSERT_FALSE(contains(redacted, key));
  // The length is part of the fingerprint on purpose: "did my key get
  // truncated when I pasted it into the portal" is a real bringup question
  // that the last four characters alone cannot answer.
  TEST_ASSERT_TRUE(contains(redacted, "24"));
}

void test_boot_report_never_prints_secrets_in_full() {
  BootReport report;
  report.apiKey = "d3f9a1c07b25e8461f0aab12";
  report.wifiPassword = "correct-horse-battery-staple";
  report.wifiSsid = "HomeNetwork";

  const std::string text = formatBootReport(report);

  TEST_ASSERT_FALSE(contains(text, report.apiKey));
  TEST_ASSERT_FALSE(contains(text, report.wifiPassword));
  // The fingerprints are still there -- redaction that erased the field
  // entirely would make the report useless for confirming a key was stored.
  TEST_ASSERT_TRUE(contains(text, "...ab12"));
  TEST_ASSERT_TRUE(contains(text, "...aple"));
  // The SSID is not a secret and is what identifies which network failed.
  TEST_ASSERT_TRUE(contains(text, "HomeNetwork"));
}

void test_boot_report_distinguishes_unread_from_zero() {
  BootReport report;
  report.batterySupported = true;
  report.batteryPercent = -1;
  report.batteryMillivolts = -1;

  const std::string text = formatBootReport(report);
  TEST_ASSERT_TRUE(contains(text, "(not read)"));
  // A 0 % battery and a battery that was never read are very different
  // bringup findings and must not render identically.
  TEST_ASSERT_FALSE(contains(text, "0 %"));
}

void test_boot_report_reports_disabled_sleep_window_as_off() {
  BootReport report;
  report.sleepWindowStartMin = -1;
  report.sleepWindowEndMin = -1;
  TEST_ASSERT_TRUE(contains(formatBootReport(report), "(off)"));

  report.sleepWindowStartMin = 23 * 60 + 30;  // 23:30
  report.sleepWindowEndMin = 6 * 60;          // 06:00
  const std::string text = formatBootReport(report);
  TEST_ASSERT_TRUE(contains(text, "23:30 - 06:00"));
}

void test_boot_report_hides_sd_table_detail_when_no_card_mounted() {
  BootReport report;
  report.sdMounted = false;
  TEST_ASSERT_FALSE(contains(formatBootReport(report), "sd routes table"));

  report.sdMounted = true;
  report.sdRoutesTable = true;
  report.sdStopTimesTable = false;
  const std::string text = formatBootReport(report);
  // routes-present/stop_times-absent is exactly the state a card written by
  // an older generator is in, and it degrades silently on the panel.
  TEST_ASSERT_TRUE(contains(text, "sd routes table"));
  TEST_ASSERT_TRUE(contains(text, "sd stop_times table"));
}

void test_boot_report_shows_the_ota_trial_only_while_it_applies() {
  BootReport report;
  report.otaPartition = "app0";
  TEST_ASSERT_FALSE(contains(formatBootReport(report), "ON TRIAL"));

  report.otaTrialVersion = "v2";
  report.otaTrialBoots = 2;
  const std::string text = formatBootReport(report);
  // "why did my board revert" is unanswerable unless the trial is visible
  // in the log while it is happening.
  TEST_ASSERT_TRUE(contains(text, "ON TRIAL"));
  TEST_ASSERT_TRUE(contains(text, "v2 (boot 2)"));
}

void test_wake_summary_omits_the_ota_block_when_no_check_ran() {
  WakeSummary summary;  // no network, so the check never happened
  TEST_ASSERT_FALSE(contains(formatWakeSummary(summary), "decision"));

  summary.otaDecision = "already current";
  TEST_ASSERT_TRUE(contains(formatWakeSummary(summary), "already current"));
}

void test_wake_summary_reports_which_source_won() {
  WakeSummary summary;
  summary.departureSource = "cached";
  summary.cachedAgeMin = 47;
  const std::string text = formatWakeSummary(summary);
  TEST_ASSERT_TRUE(contains(text, "cached"));
  TEST_ASSERT_TRUE(contains(text, "47 min"));
}

void test_wake_summary_omits_cache_age_when_unknown() {
  WakeSummary summary;
  summary.departureSource = "cached";
  summary.cachedAgeMin = -1;  // restored with no clock to age it against
  TEST_ASSERT_FALSE(contains(formatWakeSummary(summary), "cache age"));
}

void test_wake_summary_marks_a_fetch_that_never_ran() {
  WakeSummary summary;  // wifi down: the fetch was never attempted
  const std::string text = formatWakeSummary(summary);
  TEST_ASSERT_TRUE(contains(text, "(not attempted)"));
  // "transit fetch: no" would read as a failed request, which is a
  // different diagnosis from never having had a network to try it on.
  TEST_ASSERT_FALSE(contains(text, "transit http"));
}

void test_self_test_verdict_ignores_skipped_checks() {
  SelfTestReport report;
  report.results.push_back(result("display", true));
  report.results.push_back(result("sd card", false, /*skipped=*/true));
  TEST_ASSERT_TRUE(report.allPassed());
  TEST_ASSERT_EQUAL_INT(1, report.passedCount());
  TEST_ASSERT_EQUAL_INT(0, report.failedCount());
  TEST_ASSERT_EQUAL_INT(1, report.skippedCount());
  TEST_ASSERT_TRUE(contains(formatSelfTestReport(report), "ALL OK"));
}

void test_self_test_with_no_real_checks_is_not_a_pass() {
  SelfTestReport empty;
  TEST_ASSERT_FALSE(empty.allPassed());

  SelfTestReport allSkipped;
  allSkipped.results.push_back(result("sd card", false, /*skipped=*/true));
  // Every check skipping means the self-test learned nothing, which must
  // not render as a green light.
  TEST_ASSERT_FALSE(allSkipped.allPassed());
  TEST_ASSERT_TRUE(contains(formatSelfTestReport(allSkipped), "ATTENTION NEEDED"));
}

void test_self_test_one_failure_fails_the_run() {
  SelfTestReport report;
  report.results.push_back(result("display", true));
  report.results.push_back(result("battery", false));
  TEST_ASSERT_FALSE(report.allPassed());
  const std::string text = formatSelfTestReport(report);
  TEST_ASSERT_TRUE(contains(text, "[FAIL]"));
  TEST_ASSERT_TRUE(contains(text, "1 passed, 1 failed, 0 skipped"));
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_redact_unset_secret);
  RUN_TEST(test_redact_short_secret_shows_no_characters);
  RUN_TEST(test_redact_long_secret_shows_only_last_four);
  RUN_TEST(test_boot_report_never_prints_secrets_in_full);
  RUN_TEST(test_boot_report_distinguishes_unread_from_zero);
  RUN_TEST(test_boot_report_reports_disabled_sleep_window_as_off);
  RUN_TEST(test_boot_report_hides_sd_table_detail_when_no_card_mounted);
  RUN_TEST(test_boot_report_shows_the_ota_trial_only_while_it_applies);
  RUN_TEST(test_wake_summary_omits_the_ota_block_when_no_check_ran);
  RUN_TEST(test_wake_summary_reports_which_source_won);
  RUN_TEST(test_wake_summary_omits_cache_age_when_unknown);
  RUN_TEST(test_wake_summary_marks_a_fetch_that_never_ran);
  RUN_TEST(test_self_test_verdict_ignores_skipped_checks);
  RUN_TEST(test_self_test_with_no_real_checks_is_not_a_pass);
  RUN_TEST(test_self_test_one_failure_fails_the_run);
  return UNITY_END();
}
