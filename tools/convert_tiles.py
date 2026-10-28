#!/usr/bin/env python3
"""convert_tiles.py - turn map tiles into the raw RGB565 tiles the WiPhone reads.

    # the usual one: COVEY's tile tree -> the SD card
    python3 tools/convert_tiles.py ~/covey-tiles /Volumes/WIPHONE/maps/home --zoom 12-15

    # from an .mbtiles file instead
    python3 tools/convert_tiles.py camp.mbtiles /Volumes/WIPHONE/maps/camp --zoom 12-15

    # count first, convert nothing
    python3 tools/convert_tiles.py ~/covey-tiles /Volumes/WIPHONE/maps/home --dry-run

The phone decodes NOTHING - see WiPhone/app_maps.h for the whole argument. There is no PNG
decoder in the firmware and the ESP32's ROM JPEG decoder refuses greyscale outright, so a map
made of compressed tiles would work until the day it met a tile of the wrong flavour, in the
woods. A raw tile is 256x256 RGB565 little-endian, 131072 bytes exactly, and the phone's only
check is that length - which is also what catches a PNG that got copied instead of converted.

INPUT may be:
  * a directory of z/x/y.png (or .jpg) - the standard slippy layout COVEY's fetch_tiles.py
    writes, and what every tile downloader on earth produces;
  * an .mbtiles file (SQLite; the phone never sees it - this script unpacks it).

OUTPUT is a directory. Everything under it is <z>/<x>/<y>.565. Point it straight at the card:
  /Volumes/<CARD>/maps/<area>
where <area> is whatever you want the phone to call this map (letters, digits, - and _).

⚠ MACOS WRITES SIDECAR FILES ONTO FAT32 CARDS. Every ._name file that Finder leaves behind is
a file the phone has to skip, and there can be one per tile. This script never writes them,
but copying a converted tree in Finder will. Either convert straight onto the card (what the
examples above do) or use `rsync` / `ditto --norsrc --noextattr --noacl`, and run
`dot_clean -m /Volumes/<CARD>` afterwards.

DEPENDENCIES: none that macOS does not already have. Pillow is used when it is installed
because it is several times faster; otherwise the image is decoded by `sips`, which ships with
macOS, via an uncompressed BMP that this script parses itself. Linux without Pillow will say
so rather than guess.
"""

import argparse
import multiprocessing
import os
import sqlite3
import string
import struct
import subprocess
import sys
import tempfile

TILE = 256
TILE_BYTES = TILE * TILE * 2

try:
    from PIL import Image          # noqa: F401
    HAVE_PIL = True
except Exception:
    HAVE_PIL = False

# numpy is optional too: with it a tile packs in ~1 ms instead of ~150 ms (the pure-Python
# loop below visits 65,536 pixels one at a time). The output is byte-identical either way —
# tests/check_convert_tiles.py holds the two paths to each other.
try:
    import numpy as np             # noqa: F401
    HAVE_NUMPY = True
except Exception:
    HAVE_NUMPY = False


# ---------------------------------------------------------------- RGB565

def rgb565_bytes(rgb, w, h):
    """rgb: a bytes-like of w*h*3 RGB. Returns w*h*2 bytes, little-endian RGB565.

    ⚠ Little-endian, because that is how the value color565() produces sits in the ESP32's
    memory and how TFT_eSprite stores a pixel. Getting this backwards does not fail: it draws
    a recognisable map in wrong colours, which is the kind of bug that survives a demo.
    """
    if w != TILE or h != TILE:
        raise ValueError("tile is %dx%d, must be %dx%d" % (w, h, TILE, TILE))
    if HAVE_NUMPY:
        return rgb565_bytes_numpy(rgb)
    out = bytearray(TILE_BYTES)
    j = 0
    for i in range(0, len(rgb), 3):
        v = ((rgb[i] & 0xF8) << 8) | ((rgb[i + 1] & 0xFC) << 3) | (rgb[i + 2] >> 3)
        out[j] = v & 0xFF
        out[j + 1] = (v >> 8) & 0xFF
        j += 2
    return bytes(out)


def rgb565_bytes_numpy(rgb):
    """The same packing as the loop above, vectorised. '<u2' is the little-endian half."""
    a = np.frombuffer(bytes(rgb), dtype=np.uint8).reshape(-1, 3).astype(np.uint16)
    v = ((a[:, 0] & 0xF8) << 8) | ((a[:, 1] & 0xFC) << 3) | (a[:, 2] >> 3)
    return v.astype("<u2").tobytes()


def fully_transparent(im):
    """True when EVERY pixel is transparent (alpha < 128, a tRNS colour included): a tile
    server's way of saying "no tile here" with a 200. Flattened to RGB it would reach the card
    as a solid square. Half-transparent tiles are kept; this is the same line the phone's own
    downloader draws (all 65,536 pixels, judged on alpha).

    ⚠ OTM's z18 answer is NOT this: it is the words "max zoom layer = 17" in opaque pixels on
    a transparent tile (5,198 opaque pixels of 65,536, measured 2026-09-23). What keeps it off
    the cards is that nothing fetches OTM z18 and cardday.sh never converts z18."""
    if "A" not in im.getbands() and "transparency" not in im.info:
        return False
    return im.convert("RGBA").getchannel("A").getextrema()[1] < 128


def decode_with_pil(data):
    from PIL import Image
    import io
    im = Image.open(io.BytesIO(data))
    if fully_transparent(im):
        raise ValueError("fully transparent - a tile server's 'no tile here' placeholder")
    im = im.convert("RGB")
    if im.size != (TILE, TILE):
        # ⚠ SAY SO. This used to be a silent rescale, which meant the SAME command gave
        # different results on two Macs: the sips path cannot resize, so there it came out as
        # "could not be read" instead. Resizing is the friendlier behaviour of the two - a
        # 512 px tile source is usable - but it must not be invisible.
        print("  note: %dx%d tile rescaled to %dx%d"
              % (im.size[0], im.size[1], TILE, TILE), file=sys.stderr)
        im = im.resize((TILE, TILE), Image.BILINEAR)
    return im.tobytes(), TILE, TILE


def parse_bmp(data):
    """Just enough BMP for what sips writes: uncompressed 24- or 32-bit, either row order."""
    if len(data) < 54 or data[0:2] != b"BM":
        raise ValueError("not a BMP")
    off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h_raw = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    comp = struct.unpack_from("<I", data, 30)[0]
    if comp != 0 or bpp not in (24, 32) or w <= 0 or h_raw == 0:
        raise ValueError("BMP is %d-bit compression %d - not one this script reads" % (bpp, comp))
    top_down = h_raw < 0
    h = -h_raw if top_down else h_raw
    bypp = bpp // 8
    stride = (w * bypp + 3) & ~3
    if off + stride * h > len(data):
        raise ValueError("BMP is truncated")
    rgb = bytearray(w * h * 3)
    for y in range(h):
        src_row = y if top_down else (h - 1 - y)
        p = off + src_row * stride
        q = y * w * 3
        for x in range(w):
            b = data[p + x * bypp]
            g = data[p + x * bypp + 1]
            r = data[p + x * bypp + 2]        # BMP is BGR
            rgb[q + x * 3] = r
            rgb[q + x * 3 + 1] = g
            rgb[q + x * 3 + 2] = b
    return bytes(rgb), w, h


def decode_with_sips(data, suffix):
    """macOS's own decoder. One subprocess per tile, which is why Pillow is preferred."""
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "in" + suffix)
        dst = os.path.join(td, "out.bmp")
        with open(src, "wb") as f:
            f.write(data)
        # --matchTo forces RGB: a greyscale or indexed source would otherwise stay that way,
        # and the BMP reader above wants 24/32-bit colour.
        cmd = ["sips", "-s", "format", "bmp",
               "--matchTo", "/System/Library/ColorSync/Profiles/sRGB Profile.icc",
               src, "--out", dst]
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if r.returncode != 0 or not os.path.exists(dst):
            # Retry without the profile: some systems do not have that exact .icc path.
            cmd = ["sips", "-s", "format", "bmp", src, "--out", dst]
            r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            if r.returncode != 0 or not os.path.exists(dst):
                raise ValueError("sips could not read it: %s"
                                 % r.stderr.decode("utf-8", "replace").strip())
        with open(dst, "rb") as f:
            return parse_bmp(f.read())


def to_565(data, suffix):
    if HAVE_PIL:
        rgb, w, h = decode_with_pil(data)
    elif sys.platform == "darwin":
        # ⚠ No transparency check on this path: the BMP sips writes carries no alpha to judge.
        # Pillow is on the card-day Mac; this fallback is for someone else's.
        rgb, w, h = decode_with_sips(data, suffix)
    else:
        raise SystemExit("This needs Pillow on anything but macOS:\n"
                         "    python3 -m pip install --user Pillow")
    return rgb565_bytes(rgb, w, h)


# ---------------------------------------------------------------- sources

def walk_mbtiles(src, zlo, zhi):
    """Yield (z, x, y, bytes, suffix) from an .mbtiles file.

    ⚠ MBTiles rows are TMS: y counts from the SOUTH. Slippy z/x/y counts from the NORTH, and
    that is what the phone's arithmetic assumes (map_tiles.h). The flip below is the whole
    difference between a correct map and one that is mirrored top to bottom - which, on
    forest, is remarkably hard to notice.
    """
    con = sqlite3.connect("file:%s?mode=ro" % src, uri=True)
    try:
        cur = con.execute(
            "SELECT zoom_level, tile_column, tile_row, tile_data FROM tiles "
            "WHERE zoom_level BETWEEN ? AND ? ORDER BY zoom_level, tile_column, tile_row",
            (zlo, zhi))
        for z, x, tms_y, blob in cur:
            y = (1 << z) - 1 - tms_y
            if not blob:
                # A NULL or empty tile_data is legal in an .mbtiles and used to raise
                # TypeError out of the slice below, killing the run mid-way.
                print("  skipped %d/%d/%d: empty tile_data" % (z, x, y), file=sys.stderr)
                continue
            blob = bytes(blob)
            suffix = ".png" if blob[:8] == b"\x89PNG\r\n\x1a\n" else ".jpg"
            yield z, x, y, blob, suffix
    finally:
        con.close()


def walk_dir_paths(src, zlo, zhi):
    """Yield (z, x, y, path, suffix) WITHOUT reading anything. Counting a tree should not
    read it: walk_dir used to open and discard every PNG just to be counted, so a directory
    conversion read the whole tree twice and --dry-run read it once to print one line."""
    for zname in sorted(os.listdir(src)):
        if not zname.isdigit():
            continue
        z = int(zname)
        if z < zlo or z > zhi:
            continue
        zdir = os.path.join(src, zname)
        if not os.path.isdir(zdir):
            continue
        for xname in sorted(os.listdir(zdir)):
            if not xname.isdigit():
                continue
            xdir = os.path.join(zdir, xname)
            if not os.path.isdir(xdir):
                continue
            for yname in sorted(os.listdir(xdir)):
                stem, ext = os.path.splitext(yname)
                if not stem.isdigit() or ext.lower() not in (".png", ".jpg", ".jpeg"):
                    continue
                path = os.path.join(xdir, yname)
                if not os.path.isfile(path):
                    continue                    # a DIRECTORY named 123.png used to abort the run
                yield z, int(xname), int(stem), path, ext.lower()


def count_source(src, zlo, zhi):
    if os.path.isfile(src):
        con = sqlite3.connect("file:%s?mode=ro" % src, uri=True)
        try:
            n = con.execute("SELECT COUNT(*) FROM tiles WHERE zoom_level BETWEEN ? AND ?",
                            (zlo, zhi)).fetchone()[0]
        finally:
            con.close()
        return n
    n = 0
    for _ in walk_dir_paths(src, zlo, zhi):
        n += 1
    return n


# ---------------------------------------------------------------- one tile

def convert_one(job):
    """(z, x, y, blob, suffix, dst) -> (z, x, y, verdict, detail). Runs in a worker process
    when --jobs is more than one, so it must not touch anything but its own arguments.

    verdict: 'wrote' | 'unreadable' | 'stopped'. 'stopped' is an OSError on the WRITE — a
    full card, a pulled card, a read-only remount — and ends the run; 'unreadable' is this
    one tile and the run goes on."""
    z, x, y, blob, suffix, dst = job
    if blob is None:                     # a directory source: the path came instead
        try:
            with open(suffix, "rb") as f:
                blob = f.read()
        except OSError as e:
            return z, x, y, "unreadable", str(e)
        suffix = os.path.splitext(suffix)[1].lower()
        if not blob:
            return z, x, y, "unreadable", "empty file"
    try:
        raw = to_565(blob, suffix)
    except SystemExit as e:
        return z, x, y, "stopped", str(e)
    except Exception as e:
        return z, x, y, "unreadable", str(e)
    if len(raw) != TILE_BYTES:
        return z, x, y, "unreadable", "produced %d bytes" % len(raw)
    # Write, then rename: a half-written tile is a file of the wrong length, which the
    # phone refuses - but it refuses it every time you pan over it, forever.
    tmp = dst + ".part"
    try:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(tmp, "wb") as f:
            f.write(raw)
        os.replace(tmp, dst)
    except OSError as e:
        try:
            os.remove(tmp)
        except OSError:
            pass
        return z, x, y, "stopped", str(e)
    return z, x, y, "wrote", ""


# ---------------------------------------------------------------- main

def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return "%.1f %s" % (n, unit)
        n /= 1024.0


def main():
    ap = argparse.ArgumentParser(
        description="Convert map tiles to the WiPhone's raw RGB565 format.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("src", help="a z/x/y tile directory, or an .mbtiles file")
    ap.add_argument("out", help="output directory, e.g. /Volumes/WIPHONE/maps/home")
    ap.add_argument("--zoom", default="0-19",
                    help="zoom levels to convert, e.g. 12-15 or 14 (default: all)")
    ap.add_argument("--force", action="store_true",
                    help="rewrite tiles that are already there and the right length")
    ap.add_argument("--dry-run", action="store_true",
                    help="say what would be written and stop")
    ap.add_argument("--jobs", type=int, default=0,
                    help="tiles to convert at once (default: one per CPU core)")
    a = ap.parse_args()
    jobs = a.jobs if a.jobs > 0 else (os.cpu_count() or 1)

    try:
        if "-" in a.zoom:
            zlo, zhi = (int(v) for v in a.zoom.split("-", 1))
        else:
            zlo = zhi = int(a.zoom)
    except ValueError:
        # "12-", "abc" and "12-15-18" all used to print a raw traceback past the polite
        # message that was sitting two lines below them.
        raise SystemExit("--zoom must be 0..19 and low-high, e.g. 12-15 or 14")
    if not (0 <= zlo <= zhi <= 19):
        raise SystemExit("--zoom must be 0..19 and low-high, e.g. 12-15 or 14")

    if not os.path.exists(a.src):
        raise SystemExit("no such source: %s" % a.src)

    name = os.path.basename(os.path.normpath(a.out))
    # ⚠ NOT str.isalnum(): it is Unicode-aware, so "café", "Jagdhütte" and "日本" all pass here
    # and are then INVISIBLE on the phone — mapAreaNameOk() (WiPhone/map_tiles.cpp) accepts
    # ASCII letters, digits, '-', '_' and '.' only, and scanAreas() skips a folder it rejects
    # in silence. The user would have a card full of correct tiles and a Maps app that says
    # there is no map. This is the firmware's rule, transcribed.
    ALLOWED = set(string.ascii_letters + string.digits + "-_.")
    ok = (name and name[0] != "." and all(c in ALLOWED for c in name)
          and len(name.encode("utf-8")) <= 31)
    if not ok:
        raise SystemExit(
            "the output folder's name is the map's name on the phone, and it must be\n"
            "ASCII letters, digits, '-', '_' or '.', at most 31 characters, not starting\n"
            "with '.'  (the phone silently ignores a folder it cannot name)\n"
            "got: %r" % name)
    # The one wrong path that otherwise validates: /maps itself. The tiles land at
    # /maps/<z>/<x>/<y> and the phone finds zero areas, with nothing anywhere saying why.
    if name == "maps":
        raise SystemExit(
            "--out is the AREA folder, not /maps itself.\n"
            "Use .../maps/<area>, e.g. /Volumes/WIPHONE/maps/home")

    total = count_source(a.src, zlo, zhi)
    print("%d tile(s) at z%d-%d -> %s each, %s in total"
          % (total, zlo, zhi, human(TILE_BYTES), human(total * TILE_BYTES)))
    if not HAVE_PIL:
        print("(Pillow is not installed - falling back to sips, which is slower. "
              "`python3 -m pip install --user Pillow` if this drags.)")
    if a.dry_run:
        return

    done = skipped = failed = 0
    stopped = None

    def pending():
        """Every tile that is not already on the card, as a job for convert_one(). A
        directory source hands the worker the PATH (it reads the file itself, so the
        parent never holds a tile it is not writing); an .mbtiles source hands the bytes,
        because SQLite cannot be shared across processes."""
        nonlocal skipped
        if os.path.isfile(a.src):
            source = ((z, x, y, blob, suffix) for z, x, y, blob, suffix
                      in walk_mbtiles(a.src, zlo, zhi))
        else:
            source = ((z, x, y, None, path) for z, x, y, path, ext
                      in walk_dir_paths(a.src, zlo, zhi))
        for z, x, y, blob, suffix in source:
            dst = os.path.join(a.out, str(z), str(x), "%d.565" % y)
            if not a.force and os.path.exists(dst) and os.path.getsize(dst) == TILE_BYTES:
                skipped += 1
                continue
            yield z, x, y, blob, suffix, dst

    # One process is the plain loop; more is a pool. Results come back unordered, which is
    # fine: nothing here depends on the order tiles land in.
    if jobs > 1:
        pool = multiprocessing.Pool(jobs)
        results = pool.imap_unordered(convert_one, pending(), chunksize=4)
    else:
        pool = None
        results = map(convert_one, pending())
    try:
        for z, x, y, verdict, detail in results:
            if verdict == "wrote":
                done += 1
                if done % 50 == 0:
                    print("  %d/%d..." % (done + skipped, total), flush=True)
            elif verdict == "unreadable":
                print("  skipped %d/%d/%d: %s" % (z, x, y, detail), file=sys.stderr)
                failed += 1
            else:
                print("  stopped at %d/%d/%d: %s" % (z, x, y, detail), file=sys.stderr)
                stopped = detail
                break
    finally:
        if pool is not None:
            pool.terminate()
            pool.join()

    print("wrote %d, kept %d already there, %d could not be read" % (done, skipped, failed))
    if stopped:
        print("\nSTOPPED before the end: %s" % stopped)
        print("Nothing written is damaged - re-run the same command once the cause is "
              "fixed and it will carry on from here.")
    if done or skipped:
        print("\nOn the phone: Menu > Tools > Maps. If it was already open, "
              "Menu > Rescan the card.")
    if failed:
        print("\nThe unreadable ones are usually a tile server's 'no tile here' placeholder, "
              "which is safe to ignore.")
    if stopped:
        # Non-zero, so cardday.sh can tell a full or pulled card from a finished copy: it
        # used to exit 0 here, and a card that filled half-way through read as done.
        sys.exit(1)


if __name__ == "__main__":
    main()
