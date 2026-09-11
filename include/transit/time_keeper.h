#pragma once

// Transit-Elnk-Firmware — approximate wall clock that survives deep sleep.
//
// The X4 has no RTC chip (BoardConfig's FREEINK_CAP_RTC excludes it), so
// until now every wake got its time from SNTP and a wake with no network got
// no time at all: BoardStatus::lastUpdatedEpoch stayed 0, every chip fell
// back to a bare clock time, nothing could be flagged "leave now," and
// power_scheduler's sleep-window math saw minute-of-day 0 (midnight) no
// matter what time it actually was. That's the gap this module closes for a
// board carried somewhere with no Wi-Fi.
//
// The trick is ESP32 RTC memory: a small region (RTC *fast* memory on the
// C3 — it has no RTC slow memory) that keeps its contents through deep
// sleep, the same mechanism behind the Arduino-ESP32 deep-sleep example's
// RTC_DATA_ATTR boot counter. Before sleeping, the firmware records the
// best-known epoch and how long the wake timer was armed for; on the next
// wake it adds those together (plus this boot's own uptime) to estimate the
// current time without any network. A successful SNTP sync always wins and
// resets the accumulated error to zero.
//
// Accuracy, stated honestly: deep-sleep timing is driven by the RTC slow
// clock, which on this board is an internal RC oscillator calibrated against
// the main crystal at startup rather than a watch crystal. It is not
// disciplined during the sleep itself and drifts with temperature, so the
// error grows with every offline wake instead of staying put. This module
// budgets kSleepTimerErrorPermille per unit of elapsed sleep as a
// deliberately conservative *bound* (not a measurement) and refuses to
// vouch for the clock once that bound passes kMaxUsableErrorSec — at which
// point callers should treat the time as unknown again rather than render a
// confidently wrong countdown.
//
// The math is pure and unit-tested under [env:native]; only the RTC-memory
// load/save pair is hardware-dependent (guarded by FREEINK_HOST_NATIVE, the
// same split power_scheduler.h uses).

#include <cstdint>

namespace transit {

// What the previous wake left behind in RTC memory. All-defaults (valid =
// false) means nothing carried over — a cold boot, a battery pull, or a
// firmware flash — and the caller has no clock at all beyond SNTP.
struct ApproxClockState {
  bool valid = false;
  // Best-known epoch at the moment the previous wake entered deep sleep.
  int64_t epochAtSleep = 0;
  // Minutes the wake timer was armed for when that sleep began.
  int32_t plannedSleepMin = 0;
  // How many consecutive wakes have gone by without a real SNTP sync. 0
  // means epochAtSleep descends directly from a sync on the previous wake.
  int32_t wakesSinceSync = 0;
  // Total seconds slept since the last real SNTP sync, accumulated across
  // however many offline wakes have happened — this, not wakesSinceSync, is
  // what the error bound is computed from (ten short sleeps and one long
  // one drift very differently).
  int64_t secondsSinceSync = 0;
};

// Conservative error budget for the deep-sleep wake timer, in parts per
// thousand of elapsed sleep time (10 = 1%). See the file comment: this is an
// upper bound chosen to stay honest about an uncalibrated-during-sleep RC
// oscillator, not a figure measured on this hardware.
constexpr int64_t kSleepTimerErrorPermille = 10;

// Once the accumulated error bound exceeds this, approximateClockUsable()
// returns false and callers should fall back to "time unknown". 15 minutes
// is the point where a minutes-until-departure countdown stops being
// actionable — the whole reason the clock exists.
constexpr int64_t kMaxUsableErrorSec = 15 * 60;

// Estimated current epoch: where the clock was when sleep began, plus the
// sleep the timer was armed for, plus however long this boot has been awake.
// Returns 0 when state.valid is false, matching the existing
// "0 = time unknown" convention used everywhere else in this firmware
// (BoardStatus::lastUpdatedEpoch, formatDepartureChip(), ...).
int64_t estimateNowEpoch(const ApproxClockState& state, uint32_t awakeMillis);

// Upper bound, in seconds, on how far estimateNowEpoch() may be off — the
// error budget applied to every second slept since the last real sync, plus
// the sleep this wake is in the middle of accounting for. 0 for an invalid
// state (no claim is being made at all) and 0 immediately after a sync.
int64_t approximateClockErrorSeconds(const ApproxClockState& state, uint32_t awakeMillis);

// Whether the approximate clock is still worth showing. False for an
// invalid state, and false once the error bound passes kMaxUsableErrorSec.
bool approximateClockUsable(const ApproxClockState& state, uint32_t awakeMillis);

// Builds the state to persist before entering deep sleep. previous is what
// this wake started with; fromRealSync says whether the epoch descends
// from an SNTP result this wake; sleepMin is what the wake timer is about
// to be armed for.
//
// epochAtSleepEntry must be the best-known epoch **as of this call**, not
// as of whenever the time was first established this wake. The caller
// establishes it early (right after SNTP) and then spends real seconds
// fetching, rendering and computing a wake interval before getting here.
// Storing the earlier value would drop that gap, and because the gap
// repeats every wake it would accumulate into a drift the error bound
// above never accounts for -- an estimate that is confidently wrong is the
// one outcome this module exists to avoid. Awake time is measured off the
// main crystal, so folding it in costs nothing in accuracy; see main.cpp
// for how it is added.
//
// Pure, so the accumulation rules are testable without an ESP32 --
// saveApproxClock() below is a thin wrapper that writes the result to RTC
// memory.
ApproxClockState nextClockState(const ApproxClockState& previous, int64_t epochAtSleepEntry,
                                int sleepMin, bool fromRealSync);

#ifndef FREEINK_HOST_NATIVE
// Reads the state the previous wake persisted. Returns an invalid (valid =
// false) state unless this boot was a deep-sleep timer wake AND the RTC
// region still carries this module's magic — a power-on reset, a flash, or
// a brownout all correctly read as "no clock carried over" rather than as
// whatever bytes happened to survive.
ApproxClockState loadApproxClock();

// Persists state to RTC memory. Call immediately before enterDeepSleep().
void saveApproxClock(const ApproxClockState& state);
#endif

}  // namespace transit
