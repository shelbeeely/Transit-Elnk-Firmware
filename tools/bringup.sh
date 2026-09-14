#!/usr/bin/env bash
#
# One command to go from a fresh clone to a board on your desk printing what
# it is doing. Build, flash, and monitor -- with the whole session captured
# to a log file you can paste somewhere or diff against the last run.
#
#   tools/bringup.sh                 # bringup build, auto-detect the port
#   tools/bringup.sh --field         # the normal firmware (sleeps as usual)
#   tools/bringup.sh --port /dev/x   # pick the port yourself
#   tools/bringup.sh --monitor-only  # don't flash, just watch
#
# The default env is xteink_x4_bringup, which holds the board awake with a
# serial console instead of deep-sleeping -- see docs/HARDWARE_BRINGUP.md.
# Use --field once the hardware is known good; leaving the bringup build on
# a battery-powered board will flatten it, because it never sleeps.

set -euo pipefail

cd "$(dirname "$0")/.."

ENV_NAME="xteink_x4_bringup"
PORT=""
MONITOR_ONLY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --field) ENV_NAME="xteink_x4"; shift ;;
    --bringup) ENV_NAME="xteink_x4_bringup"; shift ;;
    --env) ENV_NAME="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --monitor-only) MONITOR_ONLY=1; shift ;;
    -h|--help) sed -n '3,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if ! command -v pio >/dev/null 2>&1; then
  echo "PlatformIO (pio) not found. Install it with:  pip install -U platformio" >&2
  exit 1
fi

# A fresh clone's submodule directory is empty, and the resulting build
# failure ("EInkDisplay.h: No such file") points at the wrong thing.
if [[ ! -f freeink-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h ]]; then
  echo "==> fetching the freeink-sdk submodule"
  git submodule update --init --recursive
fi

mkdir -p logs
LOG="logs/bringup-$(date +%Y%m%d-%H%M%S)-${ENV_NAME}.log"

PORT_ARGS=()
if [[ -n "$PORT" ]]; then
  PORT_ARGS=(--upload-port "$PORT")
else
  echo "==> serial ports seen right now:"
  pio device list || true
  echo "    (no --port given; PlatformIO will auto-detect. If it picks the"
  echo "     wrong one, re-run with --port /dev/cu.usbmodemXXXX)"
fi

if [[ "$MONITOR_ONLY" -eq 0 ]]; then
  echo "==> building and flashing [$ENV_NAME]"
  # Build and upload as one step so a compile error never leaves you
  # monitoring the *previous* firmware while reading new source.
  pio run -e "$ENV_NAME" -t upload "${PORT_ARGS[@]}"
fi

MONITOR_ARGS=(-e "$ENV_NAME")
if [[ -n "$PORT" ]]; then
  MONITOR_ARGS+=(--port "$PORT")
fi

echo "==> monitoring; logging to $LOG"
echo "    Ctrl-C to stop. In the bringup build, press 'h' for the console help."
echo

# unbuffer/stdbuf keep the log current rather than arriving a block at a
# time, which matters when the interesting line is the last one before a
# crash. Fall back to a plain pipe when neither is installed.
if command -v stdbuf >/dev/null 2>&1; then
  stdbuf -oL -eL pio device monitor "${MONITOR_ARGS[@]}" 2>&1 | tee "$LOG"
else
  pio device monitor "${MONITOR_ARGS[@]}" 2>&1 | tee "$LOG"
fi
