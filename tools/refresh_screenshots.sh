#!/usr/bin/env bash
#
# Regenerates the rendered board screenshots committed under
# docs/screenshots/ and referenced from README.md.
#
# These are real frames, not mockups: test/test_render_snapshot drives the
# actual RenderEngine through a host DrawTarget that rasterizes into a
# grayscale buffer, so what lands here is pixel-for-pixel what the panel
# draws (minus the e-ink's own contrast). That's the point of committing
# them -- a layout change that breaks the docs is visible in the diff.
#
# An ordinary `pio test -e native` run writes these to the gitignored
# .pio/test-output/render_snapshot/ instead, so running the test suite
# never dirties the working tree. Only this script redirects them into the
# tracked directory, via the SNAPSHOT_OUT_DIR the test honors.
#
# Usage:  tools/refresh_screenshots.sh
# Then:   git diff --stat docs/screenshots/    # review before committing

set -euo pipefail

cd "$(dirname "$0")/.."

SNAPSHOT_OUT_DIR="docs/screenshots" pio test -e native -f test_render_snapshot

# The host PNG writer emits uncompressed deflate to avoid a zlib dependency
# (test/test_render_snapshot/png_writer.h), which is ~384 KB a frame. Shrink
# them losslessly before they reach a commit -- skipping this step is how
# you accidentally add 2.6 MB to the repo on every layout tweak.
python3 tools/png_recompress.py docs/screenshots/*.png

echo
echo "Screenshots refreshed in docs/screenshots/:"
ls -1lh docs/screenshots/ | tail -n +2
