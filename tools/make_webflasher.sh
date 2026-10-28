#!/usr/bin/env bash
# Stage the current PlatformIO build into webflasher/ and write its manifest.
#
# Run after `pio run`. The four parts and their offsets are the standard ESP32 layout this
# board flashes with (the same offsets `pio run -t upload` uses):
#   0x1000   bootloader (dio/80m — matches board_build.flash_mode/f_flash in platformio.ini)
#   0x8000   partition table
#   0xe000   boot_app0 (OTA data — selects the app slot)
#   0x10000  the application
#
# ⚠ Committing every build's binaries would bloat the repo forever. Commit them when a
# release is CUT, or (better, at go-live) publish binaries as GitHub Release assets and
# point the manifest at those URLs — see docs/webflasher-plan.md.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=.pio/build/wiphone
FRAMEWORK="$HOME/.platformio/packages/framework-arduinoespressif32"
OUT=webflasher

VER=$(sed -n 's/#define FIRMWARE_VERSION "\(.*\)"/\1/p' WiPhone/config.h)
[ -n "$VER" ] || { echo "cannot read FIRMWARE_VERSION"; exit 1; }
[ -f "$BUILD/firmware.bin" ] || { echo "no build - run pio run first"; exit 1; }

# ⚠ ONE MERGED IMAGE, patched by esptool itself — not the four raw parts. This is the
# lesson of the first field test (black screen, boot loop): `pio upload` runs esptool
# with --flash_size detect, which PATCHES THE BOOTLOADER HEADER on the wire to 16MB; the
# raw SDK bootloader says 4MB, and a 4MB-believing bootloader rejects the 16MB partition
# layout and reset-loops. merge_bin applies exactly the same header patching, so the browser
# writes what the cable would have written. (esptool 3.1 rewrites header bytes 2-3 only and
# leaves the bootloader's appended SHA-256 stale - in BOTH paths; the ROM does not check it
# without Secure Boot, and every cable flash and the 0.9.79+ browser installs boot this way.)
ESPTOOL=$(ls -d "$HOME/.platformio/packages/tool-esptoolpy"*/esptool.py | head -1)
MERGED=$(mktemp -t wiphone-merged)
trap 'rm -f "$MERGED"' EXIT
python3 "$ESPTOOL" --chip esp32 merge_bin   --flash_mode dio --flash_freq 80m --flash_size 16MB   -o "$MERGED"   0x1000 "$FRAMEWORK/tools/sdk/bin/bootloader_dio_80m.bin"   0x8000 "$BUILD/partitions.bin"   0xe000 "$FRAMEWORK/tools/partitions/boot_app0.bin"   0x10000 "$BUILD/firmware.bin"
rm -f "$OUT/bootloader.bin" "$OUT/partitions.bin" "$OUT/boot_app0.bin" "$OUT/firmware.bin" \
      "$OUT/wiphone-merged.bin"

# 🛑 NEVER WRITE OVER NVS. merge_bin pads the gap between the partition table and boot_app0
# with 0xFF — and that gap IS the nvs partition (0x9000, 20 KB). Written from offset 0, the
# one merged image ERASED every user's NVS on every web-flasher install through 0.9.78: the
# mesh identity key (other nodes then flag a MISMATCH and DMs to the phone fail until they
# clear nodes — seen on phone 2 after phone 1 was flashed this way, 2026-09-25), the booksync
# passcode and device name, the KOSync last-move memo, the WiFi driver's calibration. So the
# patched image is cut into two parts that step AROUND nvs (read from the partition table
# inside the image, not assumed): the bootloader + table, then boot_app0 + the app. otadata
# (boot_app0) is still written on purpose: it points the bootloader at app0, where the app goes.
# tools/webflasher_split.py does the cut (tests/check_webflasher_split.py pins it) and prints the
# parts' offsets last, as JSON, for the manifest.
PARTS=$(python3 tools/webflasher_split.py "$MERGED" "$OUT" | tee /dev/stderr | tail -1)

python3 - "$OUT/manifest.json" "$VER" "$PARTS" <<'PY'
import json, sys
out, ver, parts = sys.argv[1], sys.argv[2], json.loads(sys.argv[3])
json.dump({"name": "WiPhone", "version": ver, "new_install_prompt_erase": False,
           "builds": [{"chipFamily": "ESP32", "parts": parts}]}, open(out, "w"), indent=2)
open(out, "a").write("\n")
PY

echo "staged $VER:"
ls -la "$OUT"
