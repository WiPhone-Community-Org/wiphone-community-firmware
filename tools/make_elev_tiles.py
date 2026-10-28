#!/usr/bin/env python3
"""make_elev_tiles.py - elevation tiles for the maps' altitude readout (WiPhone + COVEY).

Builds `<out>/elev/<z>/<x>/<y>.elv`: 256 x 256 LITTLE-ENDIAN int16 metres, row-major, 131072
bytes - the format WiPhone/elev_tiles.h reads (docs/almanac.md, "Elevation"). The source is the
AWS Open Data Terrain Tiles ("terrarium" PNG, https://registry.opendata.aws/terrain-tiles/:
metres = R*256 + G + B/256 - 32768; USGS 3DEP/NED in the US, SRTM, GMTED, ETOPO elsewhere -
attribution in README.md). Checked 2026-09-27 against USGS's own 3DEP point service at four
points near North Bend: within ~1 m on flat ground, 14 m on a slope (one 13 m pixel).

Which tiles: every z13 tile under an area the tile master holds at zoom 11 or finer (a z >= 13
map tile contributes its z13 ancestor, a z11/z12 tile its z13 descendants), plus a COARSE z10
layer (~100 m pixels) under everything down to z8, so a zoomed-out view still reads an
altitude. The device samples z13 first and falls back to z10. Numbers on 2026-09-27 for
~/tiles-master (otm + usgs-img + usgs-topo): 1,812 z13 tiles (238 MB) + the z10 layer.

    python3 tools/make_elev_tiles.py plan                      # counts only
    python3 tools/make_elev_tiles.py fetch                     # download PNGs to the cache
    python3 tools/make_elev_tiles.py build                     # PNG -> .elv under --out
    python3 tools/make_elev_tiles.py all                       # the three in order
    python3 tools/make_elev_tiles.py all --bbox 47.3,-122.0,47.6,-121.5   # an ad-hoc area

Then: phones  - python3 tools/wiphone_send.py --app maps --tree ~/elev-master/elev
      COVEY   - rsync -rt --rsync-path='sudo rsync' ~/elev-master/elev/ covey:/root/covey-elev/
      or both on card day: tools/cardday.sh <phone> push / covey push copy it (step 4)

`elev` is a RESERVED folder name under /maps: firmware from 0.9.80 never lists it as a map
area. Push it only to a phone that runs such firmware.
"""
import argparse
import concurrent.futures as cf
import math
import os
import struct
import sys
import time
import urllib.request

HOME = os.path.expanduser("~")
URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
FINE_Z, FINE_FROM = 13, 11          # z13 under every area held at z >= 11
COARSE_Z, COARSE_FROM = 10, 8       # z10 under every area held at z >= 8
SOURCES = ("otm", "usgs-img", "usgs-topo")
TILE_BYTES = 256 * 256 * 2
NODATA = -32768


def walk_master(master):
    """{(z, x, y)} of every map tile in the master, any source."""
    out = set()
    for src in SOURCES:
        root = os.path.join(master, src)
        if not os.path.isdir(root):
            continue
        for z in os.listdir(root):
            if not z.isdigit():
                continue
            for x in os.listdir(os.path.join(root, z)):
                if not x.isdigit():
                    continue
                for f in os.listdir(os.path.join(root, z, x)):
                    y = f.split(".")[0]
                    if y.isdigit():
                        out.add((int(z), int(x), int(y)))
    return out


def cover(tiles, target_z, from_z):
    """z=target_z tiles covering every tile at zoom >= from_z."""
    out = set()
    for z, x, y in tiles:
        if z < from_z:
            continue
        if z >= target_z:
            d = z - target_z
            out.add((target_z, x >> d, y >> d))
        else:
            k = 1 << (target_z - z)
            for a in range(x * k, (x + 1) * k):
                for b in range(y * k, (y + 1) * k):
                    out.add((target_z, a, b))
    return out


def bbox_tiles(bbox, z):
    s, w, n, e = bbox
    def xy(lat, lon):
        m = 2 ** z
        x = int((lon + 180.0) / 360.0 * m)
        la = math.radians(max(-85.0511, min(85.0511, lat)))
        y = int((1 - math.log(math.tan(la) + 1 / math.cos(la)) / math.pi) / 2 * m)
        return x, y
    x0, y0 = xy(n, w)
    x1, y1 = xy(s, e)
    return {(z, x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)}


def plan(a):
    if a.bbox:
        b = [float(v) for v in a.bbox.split(",")]
        fine, coarse = bbox_tiles(b, FINE_Z), bbox_tiles(b, COARSE_Z)
    else:
        tiles = walk_master(a.master)
        if not tiles:
            sys.exit("no map tiles under %s" % a.master)
        fine, coarse = cover(tiles, FINE_Z, FINE_FROM), cover(tiles, COARSE_Z, COARSE_FROM)
    return sorted(coarse) + sorted(fine)


def cache_path(a, t):
    z, x, y = t
    return os.path.join(a.cache, str(z), str(x), "%d.png" % y)


def out_path(a, t):
    z, x, y = t
    return os.path.join(a.out, "elev", str(z), str(x), "%d.elv" % y)


def fetch_one(a, t):
    p = cache_path(a, t)
    if os.path.exists(p) and os.path.getsize(p) > 100:
        return "have"
    os.makedirs(os.path.dirname(p), exist_ok=True)
    z, x, y = t
    url = URL.format(z=z, x=x, y=y)
    for attempt in range(5):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "wiphone-elev/1"})
            with urllib.request.urlopen(req, timeout=60) as r:
                body = r.read()
            if not body.startswith(b"\x89PNG"):
                raise ValueError("not a PNG (%d bytes)" % len(body))
            tmp = p + ".tmp"
            with open(tmp, "wb") as f:
                f.write(body)
            os.replace(tmp, p)
            return "got"
        except Exception as e:                       # noqa: BLE001 - retried, then reported
            last = e
            time.sleep(1.5 * (attempt + 1))
    return "FAIL %s: %s" % (url, last)


def fetch(a, todo):
    t0 = time.time()
    got = have = 0
    fails = []
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for i, r in enumerate(ex.map(lambda t: fetch_one(a, t), todo), 1):
            if r == "got":
                got += 1
            elif r == "have":
                have += 1
            else:
                fails.append(r)
            if i % 100 == 0 or i == len(todo):
                print("  fetch %d/%d  got=%d have=%d fail=%d  %.0fs" % (
                    i, len(todo), got, have, len(fails), time.time() - t0), flush=True)
    for f in fails[:20]:
        print("  " + f)
    return not fails


def terrarium_to_elv(png_path):
    """The PNG's pixels as the .elv bytes. Same rounding as elevFromTerrarium() on the phone:
    round-half-away-from-zero of R*256 + G + B/256 - 32768, clamped, -32768 reserved."""
    from PIL import Image
    import numpy as np
    im = Image.open(png_path)
    if im.size != (256, 256):
        raise ValueError("%s is %r, not 256x256" % (png_path, im.size))
    rgb = np.asarray(im.convert("RGB")).astype(np.float64)
    m = rgb[..., 0] * 256.0 + rgb[..., 1] + rgb[..., 2] / 256.0 - 32768.0
    m = np.where(m >= 0, np.floor(m + 0.5), -np.floor(-m + 0.5))
    m = np.clip(m, -32767, 32767).astype("<i2")
    return m.tobytes()


def build(a, todo):
    made = 0
    for t in todo:
        src, dst = cache_path(a, t), out_path(a, t)
        if not os.path.exists(src):
            print("  missing PNG for %r - run fetch" % (t,))
            return False
        if os.path.exists(dst) and os.path.getsize(dst) == TILE_BYTES and \
                os.path.getmtime(dst) >= os.path.getmtime(src):
            continue
        data = terrarium_to_elv(src)
        assert len(data) == TILE_BYTES
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        tmp = dst + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, dst)
        made += 1
    print("  build: %d new/updated .elv, %d total in the plan" % (made, len(todo)))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=("plan", "fetch", "build", "all"))
    ap.add_argument("--master", default=os.path.join(HOME, "tiles-master"))
    ap.add_argument("--cache", default=os.path.join(HOME, "elev-cache", "terrarium"))
    ap.add_argument("--out", default=os.path.join(HOME, "elev-master"))
    ap.add_argument("--bbox", help="south,west,north,east instead of the tile master")
    ap.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args()

    todo = plan(a)
    nf = sum(1 for t in todo if t[0] == FINE_Z)
    nc = len(todo) - nf
    print("plan: %d z%d + %d z%d tiles = %d (%.0f MB as .elv)" % (
        nf, FINE_Z, nc, COARSE_Z, len(todo), len(todo) * TILE_BYTES / 1e6))
    if a.cmd == "plan":
        return 0
    if a.cmd in ("fetch", "all") and not fetch(a, todo):
        return 1
    if a.cmd in ("build", "all") and not build(a, todo):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
