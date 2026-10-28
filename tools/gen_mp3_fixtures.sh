#!/usr/bin/env bash
# Make the MP3 decoder fixture from one of your own tracks.
#
#   tools/gen_mp3_fixtures.sh ~/Downloads/Some\ Song.mp3
#
# Copies the first slice of a real MP3 into tests/fixtures/mp3/. A slice is enough: the
# decoder is exercised frame by frame, and a few hundred KB is a few seconds of audio and
# hundreds of frames — far more than needed to catch a decoder that is subtly wrong.
#
# The fixture is NOT committed. This repo is public and the music is not ours, which is
# the same reason the JPEG fixtures are gitignored. Without it, tests/test_mp3.cpp skips
# itself and says so.
set -euo pipefail

src="${1:-}"
if [ -z "$src" ] || [ ! -f "$src" ]; then
  echo "usage: $0 <file.mp3>" >&2
  exit 1
fi

cd "$(dirname "$0")/.."
out="tests/fixtures/mp3"
mkdir -p "$out"

# 400 KB is ~25 s at 128 kbps and keeps the fixture small enough to decode quickly under
# ASan. Taken from the START of the file so the ID3 tag is included -- skipping that tag
# is one of the things being tested.
dd if="$src" of="$out/track.mp3" bs=1024 count=400 2>/dev/null

echo "wrote $out/track.mp3 ($(wc -c < "$out/track.mp3") bytes) from $(basename "$src")"
echo "run ./tests/run_tests.sh"
