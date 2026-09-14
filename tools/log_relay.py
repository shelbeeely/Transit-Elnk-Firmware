#!/usr/bin/env python3
"""Relay a live bringup log to an HTTP endpoint, so someone who is not sitting
at the board can watch it boot.

Designed to compose with tools/bringup.sh rather than compete with it: only
one process can hold a serial port, and that process is `pio device monitor`.
So this tails the log file bringup.sh is already writing and POSTs new lines
as they appear -- run them in two terminals, side by side.

    # terminal 1
    tools/bringup.sh

    # terminal 2
    tools/log_relay.py logs/bringup-20260914-101500-xteink_x4_bringup.log \\
        --post-url "https://..."

Stdlib only (urllib), so there is nothing to install beyond what PlatformIO
already puts on the path.

Safety: lines are scrubbed before they leave the machine. The firmware's own
boot report already redacts the API key and Wi-Fi password (see
include/transit/boot_report.h), but a bringup log also captures whatever you
type into the serial console, and that is not covered by the firmware's
redaction. --no-scrub disables it; think before you use it.
"""

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

# Long unbroken runs of secret-shaped characters. Deliberately crude and
# deliberately eager: a false positive costs a masked line in a debug log,
# a false negative costs a leaked API key.
SECRET_RE = re.compile(r"\b[A-Za-z0-9_\-]{20,}\b")


def scrub(line: str) -> str:
    def mask(match: re.Match) -> str:
        token = match.group(0)
        return f"<redacted:{len(token)} chars, ...{token[-4:]}>"

    return SECRET_RE.sub(mask, line)


def post(url: str, lines: list[str], source: str) -> None:
    body = json.dumps({"source": source, "lines": lines}).encode("utf-8")
    request = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"}, method="POST"
    )
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            response.read()
    except urllib.error.URLError as exc:
        # A relay that dies on a transient network blip takes the debugging
        # session with it. Complain and keep tailing.
        print(f"[log_relay] POST failed: {exc}", file=sys.stderr)


def tail(path: Path, from_start: bool):
    """Yield lines as they are appended, surviving the file not existing yet."""
    while not path.exists():
        time.sleep(0.5)
    with path.open("r", errors="replace") as handle:
        if not from_start:
            handle.seek(0, 2)
        while True:
            line = handle.readline()
            if line:
                yield line.rstrip("\n")
            else:
                time.sleep(0.25)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("logfile", type=Path, help="the log tools/bringup.sh is writing")
    parser.add_argument(
        "--post-url",
        help="POST batched lines here as JSON {source, lines}. Omit to only print.",
    )
    parser.add_argument(
        "--batch-seconds",
        type=float,
        default=5.0,
        help="how long to accumulate lines before sending (default 5)",
    )
    parser.add_argument(
        "--from-start",
        action="store_true",
        help="relay the whole existing file first, not just new lines",
    )
    parser.add_argument(
        "--no-scrub",
        action="store_true",
        help="do not mask secret-shaped tokens before sending",
    )
    args = parser.parse_args()

    source = args.logfile.name
    pending: list[str] = []
    last_send = time.monotonic()

    print(f"[log_relay] tailing {args.logfile}", file=sys.stderr)
    if args.post_url:
        print(f"[log_relay] relaying to {args.post_url}", file=sys.stderr)

    try:
        for line in tail(args.logfile, args.from_start):
            pending.append(line if args.no_scrub else scrub(line))
            now = time.monotonic()
            if pending and now - last_send >= args.batch_seconds:
                if args.post_url:
                    post(args.post_url, pending, source)
                else:
                    for pending_line in pending:
                        print(pending_line)
                pending.clear()
                last_send = now
    except KeyboardInterrupt:
        if pending and args.post_url:
            post(args.post_url, pending, source)
        print("\n[log_relay] stopped", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
