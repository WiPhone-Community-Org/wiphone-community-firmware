#!/usr/bin/env python3
"""check_convert_tiles.py — the Mac-side tile converter, against what the phone reads.

tools/convert_tiles.py is the only thing standing between a folder of PNGs and a map, and it
is run on a computer that this test suite will never see. Two of the three things it can get
wrong are silent on the phone:

  * BYTE ORDER. RGB565 the wrong way round still draws a recognisable map, in wrong colours.
  * THE BMP FALLBACK. On a Mac without Pillow the image is decoded by `sips` into an
    uncompressed BMP that this script parses itself — bottom-up rows, BGR, 4-byte stride
    padding, three chances to be subtly off.

So: the packing is checked against the SAME expression the firmware's color565() uses, and
the BMP reader is checked against a BMP built here byte by byte, including the top-down and
32-bit variants. Pillow is not required; when it is installed the two decode paths are also
checked against each other.

Run by tests/run_tests.sh.
"""
import importlib.util
import io
import os
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("ct", ROOT / "tools" / "convert_tiles.py")
ct = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ct)

failures = 0
checks = 0


def check(cond, name):
    global failures, checks
    checks += 1
    if cond:
        print("  ok  %s" % name)
    else:
        print("  FAIL %s" % name)
        failures += 1


# ── the firmware's own packing, written out independently ────────────────────────────────
# TFT_eSPI::color565: ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3), stored as a uint16
# in the ESP32's memory, i.e. LOW BYTE FIRST.
def firmware_565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def make_bmp(w, h, pixels, bpp=24, top_down=False):
    """pixels: list of (r, g, b), row 0 = TOP row. Builds the BMP sips would write."""
    bypp = bpp // 8
    stride = (w * bypp + 3) & ~3
    rows = []
    for y in range(h):
        src = y if top_down else (h - 1 - y)
        row = bytearray()
        for x in range(w):
            r, g, b = pixels[src * w + x]
            row += bytes([b, g, r])           # BMP is BGR
            if bypp == 4:
                row += b"\x00"
        row += b"\x00" * (stride - len(row))
        rows.append(bytes(row))
    data = b"".join(rows)
    off = 54
    hdr = b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off)
    info = struct.pack("<IiiHHIIiiII", 40, w, -h if top_down else h, 1, bpp, 0,
                       len(data), 2835, 2835, 0, 0)
    return hdr + info + data


print("check_convert_tiles")

# ── the packing ──────────────────────────────────────────────────────────────────────────
SAMPLES = [(0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 255, 0), (0, 0, 255),
           (40, 80, 40), (17, 231, 99), (1, 2, 3), (254, 253, 252)]
rgb = b"".join(bytes(s) for s in SAMPLES) + bytes(3 * (256 * 256 - len(SAMPLES)))
out = ct.rgb565_bytes(rgb, 256, 256)
check(len(out) == 256 * 256 * 2, "a tile is exactly 131072 bytes")
ok = True
for i, (r, g, b) in enumerate(SAMPLES):
    want = firmware_565(r, g, b)
    got = out[i * 2] | (out[i * 2 + 1] << 8)      # little-endian, as the firmware reads it
    if got != want:
        ok = False
        print("    %s -> 0x%04x, firmware says 0x%04x" % ((r, g, b), got, want))
check(ok, "every pixel packs exactly as TFT_eSPI::color565 does, low byte first")

# Red must land in the HIGH bits. A byte-swapped tile passes a length check and fails here.
red = ct.rgb565_bytes(bytes([255, 0, 0]) + bytes(3 * (256 * 256 - 1)), 256, 256)
check(red[0] == 0x00 and red[1] == 0xF8, "pure red is 0x00 0xF8 on disk, not 0xF8 0x00")

check(ct.TILE_BYTES == 131072, "TILE_BYTES agrees with the firmware's MAP_TILE_BYTES")

# The numpy path is a fast copy of the loop, not a second opinion: with numpy installed the
# two must agree byte for byte on every value a channel can take.
if ct.HAVE_NUMPY:
    import random
    rnd = random.Random(565)
    noise = bytes(rnd.getrandbits(8) for _ in range(256 * 256 * 3))
    fast = ct.rgb565_bytes_numpy(noise)
    ct.HAVE_NUMPY = False
    slow = ct.rgb565_bytes(noise, 256, 256)
    ct.HAVE_NUMPY = True
    check(fast == slow, "the numpy packing and the loop write identical tiles")
else:
    print("  --  numpy not installed; the fast-path comparison was skipped")

try:
    ct.rgb565_bytes(bytes(3 * 64 * 64), 64, 64)
    check(False, "a wrong-sized buffer is refused")
except ValueError:
    check(True, "a wrong-sized buffer is refused")

# ── the BMP fallback (what sips hands back on a Mac without Pillow) ───────────────────────
PIX = [(x * 7 % 256, y * 5 % 256, (x + y) * 3 % 256) for y in range(9) for x in range(7)]
for bpp in (24, 32):
    for td in (False, True):
        blob = make_bmp(7, 9, PIX, bpp=bpp, top_down=td)
        got, w, h = ct.parse_bmp(blob)
        want = b"".join(bytes(p) for p in PIX)
        check((w, h) == (7, 9) and got == want,
              "BMP %d-bit %s round trips, padding and BGR and all"
              % (bpp, "top-down" if td else "bottom-up"))

for bad, why in ((b"not a bmp at all" + bytes(64), "a file that is not a BMP"),
                 (make_bmp(4, 4, [(0, 0, 0)] * 16)[:40], "a truncated BMP")):
    try:
        ct.parse_bmp(bad)
        check(False, "%s is refused" % why)
    except ValueError:
        check(True, "%s is refused" % why)

# ── both decoders agree, when both exist ─────────────────────────────────────────────────
if ct.HAVE_PIL:
    from PIL import Image
    im = Image.new("RGB", (256, 256))
    p = im.load()
    for y in range(256):
        for x in range(256):
            p[x, y] = (x, y, (x ^ y) & 0xFF)
    png = io.BytesIO()
    im.save(png, format="PNG")
    bmp = io.BytesIO()
    im.save(bmp, format="BMP")
    via_pil, _, _ = ct.decode_with_pil(png.getvalue())
    via_bmp, _, _ = ct.parse_bmp(bmp.getvalue())
    check(via_pil == via_bmp, "the Pillow path and the BMP path decode identically")
    check(ct.rgb565_bytes(via_pil, 256, 256) == ct.rgb565_bytes(via_bmp, 256, 256),
          "...and therefore write identical tiles")
else:
    print("  --  Pillow not installed; the two-path comparison was skipped")

# ── a "no tile here" placeholder is refused; a map with transparency in it is not ─────────
# A server that answers 200 with a fully transparent PNG used to reach the card as a solid
# square (convert("RGB") flattens it). The line is the phone downloader's: EVERY pixel
# transparent, judged on alpha. One opaque pixel is a tile.
import subprocess, tempfile


def png_bytes(im):
    buf = io.BytesIO()
    im.save(buf, format="PNG")
    return buf.getvalue()


def refused(data):
    try:
        ct.decode_with_pil(data)
        return False
    except ValueError as e:
        return "transparent" in str(e)


if ct.HAVE_PIL:
    from PIL import Image
    clear = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    check(refused(png_bytes(clear)), "a fully transparent RGBA tile is refused")
    faint = Image.new("RGBA", (256, 256), (255, 255, 255, 127))
    check(refused(png_bytes(faint)), "...and so is one whose every pixel is under half opaque")
    one = clear.copy()
    one.putpixel((200, 13), (40, 80, 40, 255))
    check(not refused(png_bytes(one)), "one opaque pixel makes it a tile")
    # OTM's z18 answer has this shape: opaque words on a transparent ground. It is NOT refused
    # here, and the check says so, so nobody mistakes this rule for the thing that stops it.
    words = clear.copy()
    for x in range(20, 236):
        for y in range(80, 104):
            words.putpixel((x, y), (0, 0, 0, 255))
    check(not refused(png_bytes(words)),
          "a tile that is only partly transparent is kept (OTM's z18 words are this shape)")
    pal = Image.new("P", (256, 256), 0)
    pal.putpalette([10, 20, 30] * 256)
    buf = io.BytesIO()
    pal.save(buf, format="PNG", transparency=0)
    check(refused(buf.getvalue()), "a palette tile whose one colour is the tRNS colour is refused")
    buf = io.BytesIO()
    pal.save(buf, format="PNG")
    check(not refused(buf.getvalue()), "the same palette tile with no tRNS is a tile")
    check(not refused(png_bytes(Image.new("RGB", (256, 256), (0, 0, 0)))),
          "an opaque black tile is a tile (no alpha: nothing to judge)")
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "1.png")
        with open(p, "wb") as f:
            f.write(png_bytes(clear))
        v = ct.convert_one((18, 1, 1, None, p, os.path.join(td, "out", "1.565")))
        check(v[3] == "unreadable" and "transparent" in v[4]
              and not os.path.exists(os.path.join(td, "out", "1.565")),
              "the converter counts it unreadable and writes nothing")
else:
    print("  --  Pillow not installed; the transparent-tile checks were skipped")

# ── a card that fills STOPS the run, and the exit code says so ───────────────────────────
# cardday.sh push dies on a non-zero exit. convert_tiles used to print "STOPPED before the end"
# and exit 0, so a card that filled half-way read as a finished push.
if ct.HAVE_PIL:
    from PIL import Image
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "src")
        os.makedirs(os.path.join(src, "15", "5"))
        with open(os.path.join(src, "15", "5", "7.png"), "wb") as f:
            f.write(png_bytes(Image.new("RGB", (256, 256), (40, 80, 40))))
        conv = [sys.executable, str(ROOT / "tools" / "convert_tiles.py")]
        good = subprocess.run(conv + [src, os.path.join(td, "ok", "area"), "--jobs", "1"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(good.returncode == 0
              and os.path.getsize(os.path.join(td, "ok", "area", "15", "5", "7.565")) == 131072,
              "a run that writes everything exits 0")
        # A FILE where the zoom folder must go: the write fails the way a full card does, with
        # an OSError on the output side.
        blocked = os.path.join(td, "full", "area")
        os.makedirs(blocked)
        open(os.path.join(blocked, "15"), "w").close()
        bad = subprocess.run(conv + [src, blocked, "--jobs", "1"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(bad.returncode == 1 and b"STOPPED" in bad.stdout,
              "a run that STOPPED on a failed write exits 1 (got %d)" % bad.returncode)
else:
    print("  --  Pillow not installed; the STOPPED exit-code check was skipped")

# ── the way back: tiles_565_to_png.py is convert_tiles.py's inverse ──────────────────────
# A phone-only tile goes 565 -> PNG into the master tree and later PNG -> 565 onto another
# card; if the two scripts disagree on the packing by one bit, every such tile lands with
# the wrong colours on the second phone. So: 565 -> PNG -> 565 must be the identity, and the
# expansion must send pure white to pure white (bit replication, not a shift).
spec2 = importlib.util.spec_from_file_location("rv", ROOT / "tools" / "tiles_565_to_png.py")
rv = importlib.util.module_from_spec(spec2)
spec2.loader.exec_module(rv)
import random as _random
_rnd = _random.Random(1310)
raw = bytes(_rnd.getrandbits(8) for _ in range(256 * 256 * 2))
expanded = rv.rgb_from_565(raw)
check(len(expanded) == 256 * 256 * 3, "a 565 tile expands to 256x256 RGB")
check(ct.rgb565_bytes(expanded, 256, 256) == raw, "565 -> RGB -> 565 is the identity")
check(rv.rgb_from_565(b"\xff\xff" * 65536)[:3] == b"\xff\xff\xff"
      and rv.rgb_from_565(b"\x00\x00" * 65536)[:3] == b"\x00\x00\x00",
      "pure white and pure black come back exactly")
if rv.HAVE_NUMPY:
    rv.HAVE_NUMPY = False
    check(rv.rgb_from_565(raw) == expanded, "the reverse numpy path and its loop agree")
    rv.HAVE_NUMPY = True
if ct.HAVE_PIL:
    png = rv.png_from_565(raw)
    got, _, _ = ct.decode_with_pil(png)
    check(ct.rgb565_bytes(got, 256, 256) == raw, "565 -> PNG file -> 565 is the identity too")
try:
    rv.rgb_from_565(raw[:-2])
    check(False, "a tile of the wrong length is refused on the way back")
except ValueError:
    check(True, "a tile of the wrong length is refused on the way back")

# A 0-byte PNG already in the master is a failed write, not a tile. Kept, it would shadow this
# phone's good copy on every card from then on; a PNG with bytes in it is still left alone.
if ct.HAVE_PIL:
    with tempfile.TemporaryDirectory() as td:
        card = os.path.join(td, "card")
        master = os.path.join(td, "master")
        for y in (1, 2):
            os.makedirs(os.path.join(card, "14", "3"), exist_ok=True)
            with open(os.path.join(card, "14", "3", "%d.565" % y), "wb") as f:
                f.write(raw)
        os.makedirs(os.path.join(master, "14", "3"))
        open(os.path.join(master, "14", "3", "1.png"), "wb").close()          # the dud
        with open(os.path.join(master, "14", "3", "2.png"), "wb") as f:
            f.write(b"an original from the tile server")
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "tiles_565_to_png.py"),
                            card, master, "--jobs", "1"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        with open(os.path.join(master, "14", "3", "1.png"), "rb") as f:
            filled = f.read()
        with open(os.path.join(master, "14", "3", "2.png"), "rb") as f:
            kept = f.read()
        same = bool(filled) and ct.rgb565_bytes(ct.decode_with_pil(filled)[0], 256, 256) == raw
        check(r.returncode == 0 and same, "an empty PNG in the master is filled from the card")
        check(kept == b"an original from the tile server",
              "...and a PNG with something in it is left alone")
        check(b"1 tile(s) to write as PNG (1 over an empty PNG), 1 already there" in r.stdout,
              "...and the count says which was which")
else:
    print("  --  Pillow not installed; the empty-PNG check was skipped")

# ── the area name rule, against the FIRMWARE'S OWN FUNCTION ───────────────────────────────
# Not a grep over the C source: a grep only proves the words are still there. This compiles
# map_tiles.cpp and asks mapAreaNameOk() itself, then asks the script's gate the same
# questions. They must agree on every one — a name the script accepts and the firmware
# rejects is a card full of correct tiles and a Maps app that says there is no map, with
# nothing anywhere explaining it (scanAreas() skips a folder it cannot name, in silence).
NAMES = ["home", "unit-3", "elk_2026", "a.b", "H", "0",
         "café", "Jagdhütte", "日本", "home²", "ｈｏｍｅ",          # Unicode: str.isalnum() says yes
         "", ".hidden", "._home", ".DS_Store", "a b", "a/b", "a\\b", "a:b", "maps",
         "0123456789012345678901234567890123", "0123456789012345678901234567890"]

def script_accepts(name):
    ALLOWED = set(__import__("string").ascii_letters + __import__("string").digits + "-_.")
    return bool(name) and name[0] != "." and all(c in ALLOWED for c in name) \
        and len(name.encode("utf-8")) <= 31

import shutil, subprocess, tempfile
cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++")
if cxx:
    with tempfile.TemporaryDirectory() as td:
        harness = os.path.join(td, "h.cpp")
        with open(harness, "w") as f:
            f.write('#include "%s"\n' % (ROOT / "WiPhone" / "map_tiles.h"))
            f.write('#include <cstdio>\n#include <cstring>\n')
            f.write('int main(int c, char** v){for(int i=1;i<c;i++)'
                    'printf("%d\\n", mapAreaNameOk(v[i]));return 0;}\n')
        exe = os.path.join(td, "h")
        r = subprocess.run([cxx, "-std=c++11", "-O0", "-o", exe, harness,
                            str(ROOT / "WiPhone" / "map_tiles.cpp")],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if r.returncode != 0:
            print("  --  could not build the mapAreaNameOk harness; rule check skipped")
        else:
            out = subprocess.run([exe] + NAMES, stdout=subprocess.PIPE).stdout.decode().split()
            disagree = [n for n, fw in zip(NAMES, out) if (fw == "1") != script_accepts(n)]
            check(not disagree,
                  "the script's area-name gate agrees with mapAreaNameOk() on every name"
                  + ("" if not disagree else " (differs on %r)" % disagree))
            # ...and the rule is actually doing something, in both directions.
            check(out[NAMES.index("home")] == "1" and out[NAMES.index("café")] == "0",
                  "...and the firmware really does take 'home' and refuse 'café'")
else:
    print("  --  no C++ compiler; the area-name rule check was skipped")

print("\n%d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
