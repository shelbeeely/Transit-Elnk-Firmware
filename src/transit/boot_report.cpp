// Structured serial diagnostics — see include/transit/boot_report.h for the
// contract and the redaction guarantee.

#include "transit/boot_report.h"

#include <cstdio>
#include <ctime>

namespace transit {

namespace {

const char* yesNo(bool value) { return value ? "yes" : "no"; }

// "1.2 MB" / "512 KB" / "0 B". Bringup logs get read by a human comparing
// them against a datasheet, not by a parser, so a rounded human figure beats
// an exact byte count everywhere except free heap (where the exact number is
// the point and is printed as-is).
std::string humanBytes(uint64_t bytes) {
  char buf[48];
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    std::snprintf(buf, sizeof(buf), "%.1f GB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
  } else if (bytes >= 1024ULL * 1024ULL) {
    std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024ULL) {
    std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  }
  return buf;
}

std::string line(const char* key, const std::string& value) {
  // Fixed-width key column: two boot logs from different firmware revisions
  // diff cleanly when the values line up, which is most of what a bringup
  // log is for.
  char buf[256];
  std::snprintf(buf, sizeof(buf), "  %-22s %s\n", key, value.c_str());
  return buf;
}

std::string line(const char* key, const char* value) { return line(key, std::string(value)); }

std::string line(const char* key, bool value) { return line(key, yesNo(value)); }

std::string line(const char* key, long long value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", value);
  return line(key, std::string(buf));
}

// An int field whose sentinel means "never read". Printing -1 as a number
// would read as a measurement; "(not read)" does not.
std::string optionalLine(const char* key, int value, const char* suffix) {
  if (value < 0) return line(key, "(not read)");
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%d%s", value, suffix);
  return line(key, std::string(buf));
}

std::string orPlaceholder(const std::string& value, const char* placeholder) {
  return value.empty() ? std::string(placeholder) : value;
}

// Local time, for a human reading the log next to a wall clock. Falls back
// to the raw epoch rather than an empty string when the value is outside
// what localtime_r can represent.
std::string formatEpochLocal(int64_t epoch) {
  if (epoch <= 0) return "(unknown)";
  const std::time_t t = static_cast<std::time_t>(epoch);
  std::tm tm {};
  if (localtime_r(&t, &tm) == nullptr) {
    char raw[32];
    std::snprintf(raw, sizeof(raw), "%lld", static_cast<long long>(epoch));
    return raw;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d (epoch %lld)", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<long long>(epoch));
  return buf;
}

// "07:30" from a minute-of-day, or "(off)" for the -1 sentinel the sleep
// window uses when it is disabled.
std::string formatMinuteOfDay(int minuteOfDay) {
  if (minuteOfDay < 0) return "(off)";
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", (minuteOfDay / 60) % 24, minuteOfDay % 60);
  return buf;
}

std::string formatMillis(uint32_t ms) {
  char buf[32];
  if (ms >= 1000) {
    std::snprintf(buf, sizeof(buf), "%.2f s", static_cast<double>(ms) / 1000.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%u ms", static_cast<unsigned>(ms));
  }
  return buf;
}

}  // namespace

std::string redactSecret(const std::string& secret) {
  if (secret.empty()) return "(unset)";
  char buf[64];
  // Below this length the "last four characters" would be most of the
  // secret, so the length alone is all that gets reported. Eight is chosen
  // rather than five so that a short-but-real value (a test key, a stop id
  // pasted into the wrong field) still doesn't leak half of itself.
  if (secret.size() < 8) {
    std::snprintf(buf, sizeof(buf), "(set, %u chars)", static_cast<unsigned>(secret.size()));
    return buf;
  }
  std::snprintf(buf, sizeof(buf), "(set, %u chars, ...%s)", static_cast<unsigned>(secret.size()),
                secret.substr(secret.size() - 4).c_str());
  return buf;
}

std::string formatBootReport(const BootReport& report) {
  std::string out;
  out += "\n";
  out += "======== Transit-Elnk boot report ========\n";

  out += "[build]\n";
  out += line("firmware", orPlaceholder(report.firmwareVersion, "(unversioned)"));
  out += line("built", orPlaceholder(report.buildTimestamp, "(unknown)"));
  out += line("env", orPlaceholder(report.buildEnv, "(unknown)"));

  out += "[chip]\n";
  out += line("model", orPlaceholder(report.chipModel, "(unknown)"));
  out += optionalLine("revision", report.chipRevision, "");
  out += line("cpu", std::to_string(report.cpuFreqMhz) + " MHz");
  out += line("flash", humanBytes(report.flashSizeBytes));
  out += line("mac", orPlaceholder(report.macAddress, "(unknown)"));

  out += "[boot]\n";
  out += line("reset reason", orPlaceholder(report.resetReason, "(unknown)"));
  out += line("wake cause", orPlaceholder(report.wakeCause, "(unknown)"));
  out += line("boot count", static_cast<long long>(report.bootCount));
  out += line("free heap", humanBytes(report.freeHeapBytes) + " (" +
                               std::to_string(report.freeHeapBytes) + " B)");
  out += line("largest free block", humanBytes(report.largestFreeBlockBytes));
  out += line("min ever free heap", humanBytes(report.minEverFreeHeapBytes));

  out += "[peripherals]\n";
  {
    std::string panel = yesNo(report.displayOk);
    if (report.displayWidth > 0 && report.displayHeight > 0) {
      panel += " (" + std::to_string(report.displayWidth) + "x" +
               std::to_string(report.displayHeight) + ")";
    }
    out += line("display", panel);
  }
  out += line("sd mounted", report.sdMounted);
  if (report.sdMounted) {
    out += line("sd size", humanBytes(report.sdTotalBytes));
    // Named individually rather than as a single "tables present" flag:
    // routes-without-stop-times is the exact state a card generated by an
    // older tools/gen_sta_tables.py is in, and it degrades silently (the
    // offline timetable just never appears) unless the log says so.
    out += line("sd routes table", report.sdRoutesTable);
    out += line("sd stop_times table", report.sdStopTimesTable);
    out += line("sd calendar table", report.sdCalendarTable);
  }
  out += line("battery telemetry", report.batterySupported);
  if (report.batterySupported) {
    out += optionalLine("battery", report.batteryPercent, " %");
    out += optionalLine("battery voltage", report.batteryMillivolts, " mV");
    out += line("charging", report.batteryCharging);
  }

  out += "[config]\n";
  out += line("provisioned", report.provisioned);
  out += line("wifi ssid", orPlaceholder(report.wifiSsid, "(unset)"));
  out += line("wifi password", redactSecret(report.wifiPassword));
  out += line("api key", redactSecret(report.apiKey));
  out += line("stop id", orPlaceholder(report.stopId, "(unset)"));
  out += line("sta stop code", orPlaceholder(report.staStopCode, "(unset)"));
  out += line("bus wifi ssid", orPlaceholder(report.busWifiSsid, "(unset)"));
  out += line("timezone", orPlaceholder(report.timezone, "(default)"));
  out += line("refresh interval", std::to_string(report.refreshIntervalMin) + " min");
  out += line("sleep window", formatMinuteOfDay(report.sleepWindowStartMin) + " - " +
                                  formatMinuteOfDay(report.sleepWindowEndMin));
  out += line("cached board", report.cachedBoardPresent
                                  ? humanBytes(report.cachedBoardBytes)
                                  : std::string("(none)"));

  out += "[clock]\n";
  out += line("carried across sleep", report.approxClockValid);
  if (report.approxClockValid) {
    out += line("estimated now", formatEpochLocal(report.approxClockEpoch));
    out += line("error bound", std::to_string(report.approxClockErrorSec) + " s");
    out += line("wakes since sync", static_cast<long long>(report.wakesSinceSync));
  }

  out += "==========================================\n";
  return out;
}

std::string formatWakeSummary(const WakeSummary& summary) {
  std::string out;
  out += "-------- wake summary --------\n";

  out += "[network]\n";
  out += line("wifi", summary.wifiOk);
  if (!summary.wifiSsidUsed.empty()) out += line("ssid", summary.wifiSsidUsed);
  if (summary.wifiRssiDbm != 0) out += line("rssi", std::to_string(summary.wifiRssiDbm) + " dBm");
  out += line("via bus wifi", summary.viaBusWifi);
  if (summary.viaBusWifi) out += line("portal login", summary.viaCaptivePortalLogin);

  out += "[clock]\n";
  out += line("sntp", summary.sntpOk);
  out += line("now", formatEpochLocal(summary.nowEpoch));
  out += line("approximate", summary.clockApproximate);

  out += "[data]\n";
  if (summary.transitFetchAttempted) {
    out += line("transit fetch", summary.transitFetchOk);
    out += line("transit http", static_cast<long long>(summary.transitHttpStatus));
    out += line("stop ids requested", static_cast<long long>(summary.transitStopIdsRequested));
    out += line("transit routes", static_cast<long long>(summary.transitRouteCount));
  } else {
    out += line("transit fetch", "(not attempted)");
  }
  if (summary.staFetchAttempted) {
    out += line("sta routes", static_cast<long long>(summary.staRouteCount));
  }
  out += line("source", orPlaceholder(summary.departureSource, "none"));
  if (summary.cachedAgeMin >= 0) {
    out += line("cache age", std::to_string(summary.cachedAgeMin) + " min");
  }
  out += line("directions drawn", static_cast<long long>(summary.boardDirectionCount));
  out += line("departures drawn", static_cast<long long>(summary.boardDepartureCount));
  out += line("preset plans", static_cast<long long>(summary.presetPlanCount));

  out += "[timing]\n";
  out += line("wifi", formatMillis(summary.wifiMs));
  out += line("fetch", formatMillis(summary.fetchMs));
  out += line("render", formatMillis(summary.renderMs));
  out += line("awake total", formatMillis(summary.totalAwakeMs));
  out += optionalLine("battery", summary.batteryPercent, " %");
  out += line("next wake", std::to_string(summary.nextWakeMin) + " min");

  out += "------------------------------\n";
  return out;
}

bool SelfTestReport::allPassed() const {
  if (results.empty()) return false;
  bool sawRealCheck = false;
  for (const SelfTestResult& result : results) {
    if (result.skipped) continue;
    sawRealCheck = true;
    if (!result.passed) return false;
  }
  return sawRealCheck;
}

int SelfTestReport::passedCount() const {
  int count = 0;
  for (const SelfTestResult& result : results) {
    if (!result.skipped && result.passed) ++count;
  }
  return count;
}

int SelfTestReport::failedCount() const {
  int count = 0;
  for (const SelfTestResult& result : results) {
    if (!result.skipped && !result.passed) ++count;
  }
  return count;
}

int SelfTestReport::skippedCount() const {
  int count = 0;
  for (const SelfTestResult& result : results) {
    if (result.skipped) ++count;
  }
  return count;
}

std::string formatSelfTestReport(const SelfTestReport& report) {
  std::string out;
  out += "\n======== self-test ========\n";
  for (const SelfTestResult& result : report.results) {
    // Fixed-width verdict column so a failing line is findable by eye in a
    // scrolling terminal, and by `grep FAIL` in a captured log.
    const char* verdict = result.skipped ? "SKIP" : (result.passed ? "PASS" : "FAIL");
    char buf[256];
    std::snprintf(buf, sizeof(buf), "  [%s] %-20s %s\n", verdict, result.name.c_str(),
                  result.detail.c_str());
    out += buf;
  }
  char tally[128];
  std::snprintf(tally, sizeof(tally), "  %d passed, %d failed, %d skipped -- %s\n",
                report.passedCount(), report.failedCount(), report.skippedCount(),
                report.allPassed() ? "ALL OK" : "ATTENTION NEEDED");
  out += tally;
  out += "===========================\n";
  return out;
}

}  // namespace transit
