#!/usr/bin/env bash
# Publish the staged webflasher/ to the `gh-pages` branch — the branch GitHub Pages serves.
#
# The branch is RECREATED as a single orphan commit every publish, deliberately: firmware
# binaries live next to the manifest (same-origin, so the browser fetch never meets CORS),
# and recreating the branch means the repo never accumulates a history of 2 MB binaries —
# gh-pages always holds exactly one copy of exactly one version.
#
# Run tools/make_webflasher.sh first (this script refuses a stale or missing stage).
set -euo pipefail
cd "$(dirname "$0")/.."

[ -f webflasher/manifest.json ] || { echo "no stage - run tools/make_webflasher.sh"; exit 1; }
[ webflasher/wiphone-app.bin -nt .pio/build/wiphone/firmware.bin ] || \
  [ ! -f .pio/build/wiphone/firmware.bin ] || {
    echo "stage is OLDER than the build - run tools/make_webflasher.sh"; exit 1; }

VER=$(python3 -c "import json;print(json.load(open('webflasher/manifest.json'))['version'])")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# The two parts make_webflasher.sh cut around nvs (never the old merged image: it erased NVS),
# and the vendored esptool-js the page imports (webflasher/THIRD_PARTY.md).
cp webflasher/index.html webflasher/manifest.json webflasher/wiphone-boot.bin \
   webflasher/wiphone-app.bin webflasher/esptool-js-*.bundle.js webflasher/THIRD_PARTY.md "$TMP/"
touch "$TMP/.nojekyll"

git -C "$TMP" init -q -b gh-pages
git -C "$TMP" add -A
git -C "$TMP" -c user.name="$(git config user.name)" \
              -c user.email="$(git config user.email)" \
              commit -q -m "webflasher $VER"
git -C "$TMP" push -q --force "$(git remote get-url origin)" gh-pages:gh-pages

echo "published $VER to gh-pages"
