#!/usr/bin/env bash
#
# Assembles the GitHub Pages site into _site/.
#
# The site lives in site/ and the screenshots live in docs/screenshots/ --
# the latter shared with the repository's own markdown docs so there is one
# copy of each image, refreshed by one script (tools/refresh_screenshots.sh).
# This step is what brings them together, so the pages can just reference
# screenshots/ relatively and work identically when opened off disk.
#
# Usage:  tools/build_site.sh [output-dir]      (default: _site)
# Preview: python3 -m http.server -d _site 8000

set -euo pipefail

cd "$(dirname "$0")/.."
OUT="${1:-_site}"

rm -rf "$OUT"
mkdir -p "$OUT"

cp -R site/. "$OUT"/
mkdir -p "$OUT/screenshots"
cp docs/screenshots/*.png "$OUT/screenshots"/

# Tells GitHub Pages to serve the files as-is instead of running them
# through Jekyll, which would otherwise ignore any path starting with an
# underscore and add a build step this site has no use for.
touch "$OUT/.nojekyll"

echo "built $OUT/"
echo "  $(find "$OUT" -name '*.html' | wc -l) pages, $(find "$OUT/screenshots" -name '*.png' | wc -l) screenshots, $(du -sh "$OUT" | cut -f1) total"
