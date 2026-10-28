#!/usr/bin/env python3
"""tiles_565_to_png.py - the other direction: a phone's raw tiles back into a z/x/y PNG tree.

    python3 tools/tiles_565_to_png.py /Volumes/WIPHONE/maps/usgs-topo ~/tiles-master/usgs-topo

Reads <src>/<z>/<x>/<y>.565 (256x256 RGB565 little-endian, 131072 bytes - what
convert_tiles.py writes and the phone's own downloader writes) and puts <out>/<z>/<x>/<y>.png
beside whatever is already there. A PNG that already exists is LEFT ALONE - the point of this
script is to fill the gaps in a tree of originals (COVEY's cache, say) with the tiles only a
phone has, and an original from the tile server is always better than a tile that has been
through RGB565: 5-6-5 keeps 16 bits of the 24, and no amount of care gets the other 8 back.
The one exception is a 0-byte PNG: that is a write that failed, not a tile, and it is filled.

The expansion is the usual bit replication (r8 = r5 << 3 | r5 >> 2, and so on), so pure
white comes back as pure white and pure black as pure black, not 248 and 0.

A file that is not exactly 131072 bytes is reported and skipped: that is the phone's own
rule, and it is how a half-written tile or a stray PNG shows up.

--jpeg writes JPEG bytes instead (quality 92) - STILL under the .png name, because that is
how COVEY's cache holds aerial imagery: its downloader saves whatever the server sent under
<y>.png and the map loads by content, and USGS imagery is JPEG. Use it for a photo source
(usgs-img); a photo as a true-colour PNG is three to five times the bytes for nothing.

Needs Pillow (`python3 -m pip install --user Pillow`); numpy makes it a few times faster and
is used when it is there. Byte order and packing are the ones tests/check_convert_tiles.py
pins for convert_tiles.py - this is its inverse, and the test round-trips the two.
"""
import argparse
import multiprocessing
import os
import sys

TILE = 256
TILE_BYTES = TILE * TILE * 2

try:
    import numpy as np
    HAVE_NUMPY = True
except Exception:
    HAVE_NUMPY = False


def rgb_from_565(raw):
    """131072 bytes of little-endian RGB565 -> 196608 bytes of RGB888, bit-replicated."""
    if len(raw) != TILE_BYTES:
        raise ValueError("tile is %d bytes, must be %d" % (len(raw), TILE_BYTES))
    if HAVE_NUMPY:
        v = np.frombuffer(raw, dtype="<u2").astype(np.uint16)
        r = ((v >> 11) & 0x1F).astype(np.uint8)
        g = ((v >> 5) & 0x3F).astype(np.uint8)
        b = (v & 0x1F).astype(np.uint8)
        out = np.empty((TILE * TILE, 3), dtype=np.uint8)
        out[:, 0] = (r << 3) | (r >> 2)
        out[:, 1] = (g << 2) | (g >> 4)
        out[:, 2] = (b << 3) | (b >> 2)
        return out.tobytes()
    out = bytearray(TILE * TILE * 3)
    j = 0
    for i in range(0, TILE_BYTES, 2):
        v = raw[i] | (raw[i + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        out[j] = (r << 3) | (r >> 2)
        out[j + 1] = (g << 2) | (g >> 4)
        out[j + 2] = (b << 3) | (b >> 2)
        j += 3
    return bytes(out)


def png_from_565(raw, jpeg=False):
    from PIL import Image
    im = Image.frombytes("RGB", (TILE, TILE), rgb_from_565(raw))
    import io
    buf = io.BytesIO()
    if jpeg:
        im.save(buf, format="JPEG", quality=92, subsampling=0)
    else:
        im.save(buf, format="PNG", optimize=False)
    return buf.getvalue()


def convert_one(job):
    """(z, x, y, src, dst) -> (z, x, y, verdict, detail); verdict 'wrote' | 'skipped' |
    'stopped' (an OSError on the write ends the run)."""
    z, x, y, src, dst, jpeg = job
    try:
        with open(src, "rb") as f:
            raw = f.read()
        png = png_from_565(raw, jpeg)
    except OSError as e:
        return z, x, y, "skipped", str(e)
    except ValueError as e:
        return z, x, y, "skipped", str(e)
    tmp = dst + ".part"
    try:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(tmp, "wb") as f:
            f.write(png)
        os.replace(tmp, dst)
    except OSError as e:
        try:
            os.remove(tmp)
        except OSError:
            pass
        return z, x, y, "stopped", str(e)
    return z, x, y, "wrote", ""


def walk_565(src, zlo, zhi):
    for zname in sorted(os.listdir(src)):
        if not zname.isdigit() or not (zlo <= int(zname) <= zhi):
            continue
        zdir = os.path.join(src, zname)
        if not os.path.isdir(zdir):
            continue
        for xname in sorted(os.listdir(zdir)):
            xdir = os.path.join(zdir, xname)
            if not xname.isdigit() or not os.path.isdir(xdir):
                continue
            for yname in sorted(os.listdir(xdir)):
                stem, ext = os.path.splitext(yname)
                if not stem.isdigit() or ext != ".565":
                    continue
                yield int(zname), int(xname), int(stem), os.path.join(xdir, yname)


def main():
    ap = argparse.ArgumentParser(description="Raw RGB565 phone tiles -> a z/x/y PNG tree.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("src", help="a phone area folder, e.g. /Volumes/WIPHONE/maps/usgs-topo")
    ap.add_argument("out", help="the PNG tree to fill, e.g. ~/tiles-master/usgs-topo")
    ap.add_argument("--zoom", default="0-19", help="zoom levels, e.g. 12-15 or 14 (default: all)")
    ap.add_argument("--force", action="store_true", help="rewrite PNGs that already exist")
    ap.add_argument("--dry-run", action="store_true", help="count and stop")
    ap.add_argument("--jobs", type=int, default=0, help="tiles at once (default: one per core)")
    ap.add_argument("--jpeg", action="store_true",
                    help="write JPEG bytes (under the .png name, as COVEY's cache does) - for imagery")
    a = ap.parse_args()
    try:
        zlo, zhi = (int(v) for v in a.zoom.split("-", 1)) if "-" in a.zoom else (int(a.zoom),) * 2
    except ValueError:
        raise SystemExit("--zoom must be 0..19 and low-high, e.g. 12-15 or 14")
    if not (0 <= zlo <= zhi <= 19):
        raise SystemExit("--zoom must be 0..19 and low-high, e.g. 12-15 or 14")
    if not os.path.isdir(a.src):
        raise SystemExit("no such folder: %s" % a.src)
    try:
        from PIL import Image  # noqa: F401
    except Exception:
        raise SystemExit("This needs Pillow:\n    python3 -m pip install --user Pillow")

    jobs = a.jobs if a.jobs > 0 else (os.cpu_count() or 1)
    todo, kept, empty = [], 0, 0
    for z, x, y, path in walk_565(a.src, zlo, zhi):
        dst = os.path.join(a.out, str(z), str(x), "%d.png" % y)
        if not a.force and os.path.exists(dst):
            # A 0-byte PNG is a write that failed (COVEY's usgs-img/14/2621/5733 was one), not
            # a tile: kept, it would shadow this phone's good copy on every card from now on.
            if os.path.getsize(dst) > 0:
                kept += 1
                continue
            empty += 1
        todo.append((z, x, y, path, dst, a.jpeg))
    print("%d tile(s) to write as %s%s, %d already there"
          % (len(todo), "JPEG (under .png names)" if a.jpeg else "PNG",
             " (%d over an empty PNG)" % empty if empty else "", kept))
    if a.dry_run or not todo:
        return

    pool = multiprocessing.Pool(jobs) if jobs > 1 else None
    results = pool.imap_unordered(convert_one, todo, chunksize=4) if pool else map(convert_one, todo)
    done = skipped = 0
    stopped = None
    try:
        for z, x, y, verdict, detail in results:
            if verdict == "wrote":
                done += 1
                if done % 500 == 0:
                    print("  %d/%d..." % (done, len(todo)), flush=True)
            elif verdict == "skipped":
                print("  skipped %d/%d/%d: %s" % (z, x, y, detail), file=sys.stderr)
                skipped += 1
            else:
                print("  stopped at %d/%d/%d: %s" % (z, x, y, detail), file=sys.stderr)
                stopped = detail
                break
    finally:
        if pool:
            pool.terminate()
            pool.join()
    print("wrote %d, kept %d already there, %d skipped" % (done, kept, skipped))
    if stopped:
        print("\nSTOPPED before the end: %s" % stopped)
        sys.exit(1)


if __name__ == "__main__":
    main()
