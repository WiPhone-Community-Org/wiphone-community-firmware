#!/usr/bin/env python3
"""check_covey_pull.py — tools/covey_pull.py against a fake COVEY built on this Mac.

`cardday.sh covey pull` moves tiles into the one tree every phone card is rebuilt from, and
the ways it can go wrong are all quiet ones: a master tile replaced by a different one, a tile
cut off mid-stream pooled as if whole (bsdtar pads it with zeros to its full size, so it is
exactly COVEY's size), a streamed tile pooled as if chosen, a zero-byte dud copied to every
phone, an empty master file standing where COVEY has a good tile. None of them shows until
someone looks at the wrong ground in the woods. So each one is built here and watched for.

The "remote" is a temporary directory laid out like COVEY's /root (covey-tiles next to
covey-tiles-streamed), and the pull runs its real tar and rsync commands against it through
sh instead of ssh (covey_pull.LocalRemote). The cut stream is a real one: the tar stream piped
through `head -c N`, N landing halfway through one tile's data. Nothing here touches the
network or a device.

    python3 tests/check_covey_pull.py            # VERBOSE=1 also prints the pull's own log
"""
import contextlib
import fcntl
import importlib.util
import io
import math
import os
import pathlib
import random
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("cp", ROOT / "tools" / "covey_pull.py")
cp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cp)

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


PNG_HEAD = b"\x89PNG\r\n\x1a\n"
# The IEND chunk, built from the PNG spec rather than copied from the tool: length 0, the type,
# and the CRC-32 of the type.
PNG_TAIL = struct.pack(">I", 0) + b"IEND" + struct.pack(">I", zlib.crc32(b"IEND"))
JPEG_HEAD, JPEG_TAIL = b"\xff\xd8\xff\xe0", b"\xff\xd9"


def body(seed, n=300, jpeg=False):
    """A tile-shaped body of exactly n bytes: a PNG's magic and IEND chunk (a JPEG's SOI and EOI
    when jpeg, as COVEY keeps usgs-img), with bytes that differ per seed between."""
    head, tail = (JPEG_HEAD, JPEG_TAIL) if jpeg else (PNG_HEAD, PNG_TAIL)
    mid = bytearray()
    while len(mid) < n - len(head) - len(tail):
        mid += ("%s;" % seed).encode()
    return head + bytes(mid[:n - len(head) - len(tail)]) + tail


def real_png(w=4, h=4):
    """A real PNG, chunk by chunk, CRCs and all."""
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    raw = b"".join(b"\x00" + bytes([(x * 40 + y * 7) % 256 for x in range(w * 3)]) for y in range(h))
    return (PNG_HEAD + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def padded(data):
    """What bsdtar leaves when the stream stops halfway through a tile: its full size, the
    first half real, zeros after."""
    return data[:len(data) // 2] + bytes(len(data) - len(data) // 2)


def put(top, rel, data):
    p = os.path.join(top, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)


def read(top, rel):
    try:
        with open(os.path.join(top, rel), "rb") as f:
            return f.read()
    except OSError:
        return None


def pull(remote, master, **kw):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = cp.pull(remote, master, **kw)
    return rc, buf.getvalue()


def tree(top):
    out = {}
    for d, _, files in os.walk(top):
        for f in files:
            p = os.path.join(d, f)
            out[os.path.relpath(p, top)] = read(top, os.path.relpath(p, top))
    return out


def x2lon(x, z):
    return x / float(1 << z) * 360.0 - 180.0


def y2lat(y, z):
    return math.degrees(math.atan(math.sinh(math.pi * (1 - 2.0 * y / (1 << z)))))


def covey_place(src, z, x0, y0, w, h, name, tiles=None, zmin=None, last="2026-09-21"):
    """A PLACE line as COVEY's tilestore prints it for streamed tiles filling columns x0..x0+w-1
    and rows y0..y0+h-1 at z: tilestore._place_record's arithmetic (the bbox of the tile EDGES,
    its middle in degrees, 111 km a degree) and its printing (%.5f, %.2f), written out here
    independently of covey_pull."""
    north, south = y2lat(y0, z), y2lat(y0 + h, z)
    west, east = x2lon(x0, z), x2lon(x0 + w, z)
    lat, lon = (north + south) / 2.0, (west + east) / 2.0
    hy = (north - south) / 2.0 * 111.0
    hx = (east - west) / 2.0 * 111.0 * max(0.2, math.cos(math.radians(lat)))
    return ("PLACE src=%s tiles=%d bytes=%d zmin=%d zmax=%d lat=%.5f lon=%.5f radius_km=2 last=%s "
            "half_x_km=%.2f half_y_km=%.2f name=%s"
            % (src, tiles or w * h, 20000 * (tiles or w * h), zmin or z, z, lat, lon, last,
               hx, hy, name))


# Synthetic ground (NOT {user}'s: see DESIGN §9 on the privacy of the real list). A 3 x 2 block
# of z17 tiles the master will hold in full, and a 3 x 2 block it will hold only in part.
COVERED = ("otm", 17, 27300, 46900, 3, 2)
PARTLY = ("otm", 17, 27400, 46950, 3, 2)
PLACES = """ANCHORS visits=12 waypoints=3 downloaded_cells=40 position=yes visits_path=/root/.covey/visits.json waypoints_path=/root/.covey/waypoints.json
PLACE src=otm tiles=513 bytes=9437184 zmin=15 zmax=17 lat=47.5000 lon=-121.8000 radius_km=2 last=2026-09-20 name=near Elk Camp
PLACE src=otm tiles=66 bytes=700000 zmin=17 zmax=17 lat=47.4000 lon=-121.6000 radius_km=2 last=2026-09-19 name=47.4000, -121.6000
PLACE src=usgs-topo tiles=1204 bytes=1 zmin=16 zmax=16 lat=47.3 lon=-121.5 radius_km=20 part=2/3 last=2026-09-18 name=near Ridge
%s
%s
OVERVIEW src=otm tiles=121 bytes=2000000
SOMETHING-NEW src=otm whatever=1
DISK used_pct=16 free_bytes=95100000000 expire_at_pct=90 expire_to_pct=85 keeping=yes
""" % (covey_place(*COVERED, name="near Covered Knob", tiles=40, zmin=15),
       covey_place(*PARTLY, name="near Partly Pass"))


def rect_tiles(src, z, x0, y0, w, h):
    return ["%s/%d/%d/%d.png" % (src, z, x, y) for x in range(x0, x0 + w) for y in range(y0, y0 + h)]

print("check_covey_pull")

# ── which folders are sources ────────────────────────────────────────────────────────────
TAKE = ["otm", "usgs-topo", "usgs-img", "home", "camp_2"]
LEAVE = ["8", "12", "", ".git", "._otm", "-streamed", "otm-streamed", "lost+found", "my area",
         "café", "x" * 32]
check(all(cp.source_ok(n) for n in TAKE), "the three sources and any phone-nameable folder are taken")
check(not any(cp.source_ok(n) for n in LEAVE),
      "digit folders (the legacy tree), dot folders, *-streamed and names the phone refuses are not")

# ── the commands sent to COVEY name the downloaded tree and nothing else ─────────────────
r = cp.Remote("covey")
cmds = [r.list_cmd(), r.manifest_cmd("otm"), r.tar_cmd("otm"), r.default_places_cmd(),
        " ".join(r.rsync_argv("otm", "/tmp/list", "/tmp/dest"))]
check(not any("streamed" in c for c in cmds), "no command sent to COVEY names the streamed tree")
check(r.manifest_cmd("otm") == "sudo find /root/covey-tiles/otm -type f -name '*.png' -printf '%P\\t%s\\n'",
      "the manifest is one sudo find -printf of <path>\\t<size> under /root/covey-tiles/<src>")
check(r.tar_cmd("otm") == "sudo tar cf - -C /root/covey-tiles/otm -T -",
      "the tar stream reads its names from stdin, relative to the source folder")
rs = r.rsync_argv("otm", "/tmp/list", "/tmp/dest")
check("--ignore-existing" in rs and "--min-size=1" in rs and "--rsync-path=sudo rsync" in rs
      and rs[-2] == "covey:/root/covey-tiles/otm/",
      "rsync never replaces, never takes an empty file, and reads as root")
pc = r.default_places_cmd()
check(all(s in pc for s in ("COVEY_TILES=/root/covey-tiles ", "COVEY_PREFS=/root/.covey/prefs.json",
                            "COVEY_WAYPOINTS=/root/.covey/waypoints.json",
                            "python3 -m covey_ui.tilestore --places")),
      "the places report is asked for with every path given, not left to HOME under sudo")
check(cp.Remote("covey", "/root/covey-tiles/").root == "/root/covey-tiles",
      "a trailing slash on the root is dropped (root + '-streamed' must never land inside it)")
try:
    cp.Remote("covey", "/root/covey-tiles-streamed")
    check(False, "the streamed tree is refused as a root")
except SystemExit:
    check(True, "the streamed tree is refused as a root")

# ── the manifest reader ──────────────────────────────────────────────────────────────────
sizes, ignored = cp.parse_manifest("15/1/2.png\t300\n16/1/._2.png\t4096\n16/1/3.png\t0\n"
                                   "junk\n16/x/3.png\t9\n17/1/1.png\tabc\n")
check(sizes == {"15/1/2.png": 300, "16/1/3.png": 0} and ignored == 4,
      "the manifest keeps z/x/y.png only; an AppleDouble ._ file and garbage are counted, not taken")

# ── COVEY's report, as a person reads it ─────────────────────────────────────────────────
def rows_match(rows, want, name):
    check(rows == want, name)
    if rows != want:
        for a, b in zip(rows + [""] * 12, want + [""] * 12):
            if a != b:
                print("    got  %r\n    want %r" % (a, b))


rep0 = cp.parse_report(PLACES)
check(rep0["places"][3].get("half_x_km") and rep0["places"][3]["name"] == "near Covered Knob"
      and "half_x_km" not in rep0["places"][0] and rep0["places"][0]["name"] == "near Elk Camp",
      "PLACE lines parse with half_x_km/half_y_km before name=, and without them (an older COVEY)")
WH = " → on COVEY: Map › DL › Streamed-only"
rows_match(cp.report_rows(rep0), [
    "Will NOT reach the phones unless downloaded on COVEY first:",
    "  otm: near Elk Camp: 513 tiles z15-17, streamed 2026-09-20" + WH,
    "  otm: 47.4000, -121.6000: 66 tiles z17, streamed 2026-09-19" + WH,
    "  usgs-topo: near Ridge (part 2 of 3): 1,204 tiles z16, streamed 2026-09-18" + WH,
    "  otm: near Covered Knob: 40 tiles z15-17, streamed 2026-09-21" + WH,
    "  otm: near Partly Pass: 6 tiles z17, streamed 2026-09-21" + WH,
    "  otm: also 121 overview tiles from zoomed-out panning (not a place)",
    "COVEY's card: 16 % used, 95.1 GB free; streamed tiles expire past 90 %",
], "the report's PLACE/OVERVIEW/DISK lines become the rows {user} reads (no master: every place listed)")
check(cp.streamed_counts(rep0) == {"otm": 746, "usgs-topo": 1204},
      "the streamed-only totals per source add places and overview")

# The master already holding a place's ground moves it out of 'Will NOT reach the phones'.
cover_td = tempfile.mkdtemp(prefix="check_covey_pull_cover.")
try:
    for rel in rect_tiles(*COVERED):
        put(cover_td, rel, body(rel))
    part = rect_tiles(*PARTLY)
    for rel in part[:4]:
        put(cover_td, rel, body(rel))
    put(cover_td, part[4], b"")                   # an empty file is not a tile
    # The tiles just outside each rectangle, which a rounding error would pull into it.
    for src, z, x0, y0, w, h in (COVERED, PARTLY):
        for x, y in ((x0 - 1, y0), (x0 + w, y0), (x0, y0 - 1), (x0, y0 + h)):
            put(cover_td, "%s/%d/%d/%d.png" % (src, z, x, y), body("edge"))
    rows_match(cp.report_rows(rep0, cover_td), [
        "Will NOT reach the phones unless downloaded on COVEY first:",
        "  otm: near Elk Camp: 513 tiles z15-17, streamed 2026-09-20" + WH,
        "  otm: 47.4000, -121.6000: 66 tiles z17, streamed 2026-09-19" + WH,
        "  usgs-topo: near Ridge (part 2 of 3): 1,204 tiles z16, streamed 2026-09-18" + WH,
        "  otm: near Partly Pass: 6 tiles z17, streamed 2026-09-21 (master has 4 of 6 z17 tiles there)" + WH,
        "  otm: also 121 overview tiles from zoomed-out panning (not a place)",
        "Streamed on COVEY, already in the master (the phones get it):",
        "  otm: near Covered Knob: 40 tiles z15-17, streamed 2026-09-21 (master has all 6 z17 tiles there)",
        "COVEY's card: 16 % used, 95.1 GB free; streamed tiles expire past 90 %",
    ], "a place whose z17 ground is all in the master moves under its own heading; one it holds in "
       "part stays, saying how much (an empty file does not count; older lines are left as they were)")
    with open(os.path.join(cover_td, "places.txt"), "w") as f:
        f.write(PLACES)
    rep_cli = subprocess.run([sys.executable, str(ROOT / "tools" / "covey_pull.py"), "report", "--local",
                              "--remote-root", cover_td, "--master", cover_td,
                              "--places-cmd", "cat %s" % os.path.join(cover_td, "places.txt")],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out_cli = rep_cli.stdout.decode("utf-8")
    check(rep_cli.returncode == 0 and "Streamed on COVEY, already in the master" in out_cli
          and "(master has 4 of 6 z17 tiles there)" in out_cli,
          "`report --master` (cardday.sh status) checks the master the same way")
    only_cov = cp.parse_report(covey_place(*COVERED, name="near Covered Knob") + "\nDISK used_pct=1 "
                               "free_bytes=1 expire_at_pct=90 keeping=yes\n")
    r1 = cp.report_rows(only_cov, cover_td)
    check(r1[0] == cp.HEAD_HAVE and cp.HEAD not in r1,
          "when every place is in the master, nothing is listed as 'Will NOT reach the phones'")
finally:
    shutil.rmtree(cover_td, ignore_errors=True)

# Rounding: COVEY prints lat/lon to 1e-5 degree and the half-extents to 10 m. Every edge must
# still come back as the right tile line, from the equator to 70 degrees, z12 to z19.
rng = random.Random(20260923)
bad = []
for _ in range(3000):
    z = rng.randint(12, 19)
    n = 1 << z
    w, h = rng.randint(1, 60), rng.randint(1, 60)
    x0 = rng.randint(0, n - w - 1)
    lo, hi = int(n * 0.23), int(n * 0.77)          # about 70 N to 70 S (Mercator rows)
    y0 = rng.randint(lo, hi - h)
    got = cp.place_rect(cp.parse_report(covey_place("otm", z, x0, y0, w, h, "x"))["places"][0])
    if got != (z, x0, x0 + w, y0, y0 + h):
        bad.append(((z, x0, y0, w, h), got))
check(not bad, "3,000 random places come back as exactly their own tile rectangle (%d did not%s)"
      % (len(bad), ": e.g. %r" % (bad[0],) if bad else ""))
check(cp.place_rect({"src": "otm", "zmax": "17", "lat": "47.5", "lon": "-121.8"}) is None
      and cp.place_cover({"src": "../etc", "zmax": "17", "lat": "1", "lon": "1", "half_x_km": "1",
                          "half_y_km": "1"}, "/") is None,
      "no extent (an older COVEY) or a source name that is not a plain folder: no master check")
full = cp.report_rows(cp.parse_report(PLACES.replace("keeping=yes", "keeping=no")))
check(full[-1].startswith("⚠ COVEY's card is past 90 % full"), "a card over the line says so")
check(cp.report_rows(cp.parse_report("DISK used_pct=3 free_bytes=100 expire_at_pct=90 keeping=yes\n"))[0]
      .startswith("Nothing on COVEY is streamed-only"), "an empty report says there is nothing to do")


# ── a whole tile, by its end (a cut one is COVEY's size, with zeros for a tail) ─────────────
check(cp.PNG_END == PNG_TAIL, "the tool's IEND chunk is the PNG spec's (length 0, 'IEND', its CRC-32)")
unit_td = tempfile.mkdtemp(prefix="check_covey_pull_unit.")
try:
    png, jpg = real_png(), body("jpeg", 900, jpeg=True)
    cases = {"15/1/1.png": png, "15/1/2.png": jpg, "15/1/3.png": padded(png),
             "15/1/4.png": padded(jpg), "15/1/5.png": b"<html>not a tile</html>" + bytes(80),
             "15/1/6.png": b"\x89PNG\r\n"}
    for rel, data in cases.items():
        put(unit_td, rel, data)
    sz = {rel: len(d) for rel, d in cases.items()}
    good = {rel: cp.staged_good(unit_td, rel, sz) for rel in cases}
    check(good["15/1/1.png"] and good["15/1/2.png"], "a real PNG and a JPEG (usgs-img) are whole tiles")
    check(good["15/1/3.png"] is False and good["15/1/4.png"] is False,
          "a PNG or JPEG cut halfway and zero-padded to COVEY's EXACT size is not")
    check(good["15/1/5.png"] is False and good["15/1/6.png"] is False,
          "nor is a file that is neither, or one shorter than an IEND chunk")
    check(cp.staged_good(unit_td, "15/1/9.png", sz) is None, "a tile that is not there is 'not there'")

    # `tar xv`'s own words, as bsdtar printed them for a stream cut in 15/1/2.png (2026-09-23).
    log = ("x 15/1/0.png\nx 15/1/1.png\nx 15/1/2.pngtar: Write error\n"
           ": Truncated tar archive: Unknown error: -1\ntar: Error exit delayed from previous errors.\n")
    check(cp.tar_x_suspects(log, clean=False)[0] == ["15/1/2.png"],
          "a stopped stream: the entry bsdtar was writing is suspect")
    check(cp.tar_x_suspects("x 15/1/0.png\nx 15/1/1.png\n", clean=True)[0] == []
          and cp.tar_x_suspects("x 15/1/0.png\nx 15/1/1.png\n", clean=False)[0] == ["15/1/1.png"],
          "a clean pass names no suspect; the same pass not ending cleanly names its last entry")
    check(cp.tar_x_suspects("x ./15/1/0.png: Write failed\nx 15/1/1.png\n", clean=True)[0] == ["15/1/0.png"],
          "an entry bsdtar reported an error on is suspect even when the pass ends cleanly")
finally:
    shutil.rmtree(unit_td, ignore_errors=True)


# ── a fake COVEY and a master ────────────────────────────────────────────────────────────
class Faulty(cp.LocalRemote):
    """The real local transfers, plus the faults a failing link and a busy Mac produce."""

    def __init__(self, root, places_cmd, cut=None, race=None, never=None):
        cp.LocalRemote.__init__(self, root, places_cmd=places_cmd)
        self.cut = cut                # (src, rel): the tar stream stops halfway through rel, once
        self.race = race              # (src, rel, master_path): a master copy appears first
        self.never = never            # (src, rel): never delivered
        self.calls = []
        self.cut_at = None
        self.cut_left = None          # what bsdtar left of the cut tile, before the tool looked

    def tar_cmd(self, src):
        cmd = cp.LocalRemote.tar_cmd(self, src)
        return cmd + " | head -c %d" % self.cut_at if self.cut_at else cmd

    def _fault_before(self, src, rels):
        if self.race and self.race[0] == src and self.race[1] in rels:
            put(self.race[2], src + "/" + self.race[1], b"RACE")
            self.race = None
        if self.never and self.never[0] == src:
            rels = [r for r in rels if r != self.never[1]]
        return rels

    def fetch_tar(self, src, rels, dest, nbytes):
        self.calls.append(("tar", src, len(rels)))
        rels = self._fault_before(src, rels)
        if not rels:
            return True, "", []
        if not (self.cut and self.cut[0] == src and self.cut[1] in rels):
            return cp.LocalRemote.fetch_tar(self, src, rels, dest, nbytes)
        rel, self.cut = self.cut[1], None
        # Where rel's data sits in exactly this stream: make it once whole and look.
        whole = subprocess.run(["sh", "-c", cp.LocalRemote.tar_cmd(self, src)],
                               input="".join(r + "\n" for r in rels).encode(),
                               stdout=subprocess.PIPE, check=True).stdout
        with tarfile.open(fileobj=io.BytesIO(whole)) as t:
            m = t.getmember(rel)
        self.cut_at = m.offset_data + m.size // 2
        try:
            r = cp.LocalRemote.fetch_tar(self, src, rels, dest, nbytes)
        finally:
            self.cut_at = None
        self.cut_left = read(dest, rel)
        return r

    def fetch_rsync(self, src, rels, dest, nbytes):
        self.calls.append(("rsync", src, len(rels)))
        rels = self._fault_before(src, rels)
        return cp.LocalRemote.fetch_rsync(self, src, rels, dest, nbytes) if rels else (True, "", [])


# ── the cut stream on its own: fetch_tar names the tile it stopped in ──────────────────────
cut_td = tempfile.mkdtemp(prefix="check_covey_pull_cut.")
try:
    cut_src = {"15/1/%d.png" % i: body("cut%d" % i, n) for i, n in enumerate((3000, 4000, 200000))}
    for rel, data in cut_src.items():
        put(os.path.join(cut_td, "remote", "otm"), rel, data)
    os.makedirs(os.path.join(cut_td, "stage"))
    fc = Faulty(os.path.join(cut_td, "remote"), "true", cut=("otm", "15/1/2.png"))
    ok, why, suspects = fc.fetch_tar("otm", sorted(cut_src), os.path.join(cut_td, "stage"), 1)
    left = fc.cut_left or b""
    check(len(left) == 200000 and left != cut_src["15/1/2.png"] and left[-1000:] == bytes(1000),
          "THE HAZARD IS REAL: bsdtar leaves a tile cut mid-stream at its full size, zeros for a tail")
    check(not ok and suspects == ["15/1/2.png"],
          "...and fetch_tar names it as the one the stream stopped in (%r, %r)" % (why, suspects))
    check(read(os.path.join(cut_td, "stage"), "15/1/1.png") == cut_src["15/1/1.png"],
          "...while the tiles before it are whole")
finally:
    shutil.rmtree(cut_td, ignore_errors=True)


def build(td):
    covey = os.path.join(td, "root", "covey-tiles")
    streamed = os.path.join(td, "root", "covey-tiles-streamed")
    master = os.path.join(td, "master")
    remote = {}
    for rel, n in (("otm/14/2620/5720.png", 300), ("otm/14/2620/5721.png", 310),
                   ("otm/14/2621/5720.png", 320), ("otm/14/2621/5721.png", 20000),
                   ("otm/15/5241/11440.png", 340), ("otm/15/5241/11442.png", 350),
                   ("otm/15/5241/11441.png", 360),
                   ("usgs-topo/15/5241/11440.png", 370), ("usgs-topo/15/5241/11441.png", 380),
                   ("usgs-img/16/10483/22882.png", 390)):
        remote[rel] = body(rel, n, jpeg=rel.startswith("usgs-img"))
        put(covey, rel, remote[rel])
    put(covey, "otm/16/1/1.png", b"")                          # COVEY's zero-byte dud
    put(covey, "otm/16/1/._2.png", b"\x00\x05\x16\x07AppleDouble")
    put(covey, "otm/16/1/3.png.part", b"half a write")
    put(covey, "15/1/1.png", body("legacy"))                     # root-level 'local' tree
    put(covey, "-streamed/otm/15/9/9.png", body("hazard"))      # a trailing-slash streamed root
    put(covey, "otm-streamed/15/9/9.png", body("hazard2"))
    put(covey, ".hidden/15/9/9.png", body("hidden"))
    put(streamed, "otm/17/41940/91500.png", body("streamed"))  # never read, never pooled
    put(streamed, "otm/15/5241/11443.png", body("streamed2"))
    # The master: a phone-only tile COVEY lacks, and a tile COVEY has with DIFFERENT bytes.
    put(master, "otm/13/1310/2860.png", b"phone-only")
    put(master, "otm/15/5241/11441.png", b"the master's own copy")
    # An earlier pull stopped mid-run: a staged copy of COVEY's EXACT size that starts and ends
    # like a tile but is not COVEY's tile (nothing a check can see proves a leftover whole), and
    # an rsync temp file.
    good = remote["otm/14/2620/5721.png"]
    put(master, ".incoming/otm/14/2620/5721.png", good[:40] + b"X" * (len(good) - 52) + good[-12:])
    put(master, ".incoming/otm/14/2620/.5722.png.AbCdEf", b"rsync temp")
    # The master already holds the ground of one of COVEY's streamed-only places.
    for rel in rect_tiles(*COVERED):
        put(master, rel, body(rel))
    with open(os.path.join(td, "places.txt"), "w") as f:
        f.write(PLACES)
    return covey, streamed, master, remote


td = tempfile.mkdtemp(prefix="check_covey_pull.")
try:
    covey, streamed, master, remote = build(td)
    places = "cat %s" % os.path.join(td, "places.txt")

    # ── dry run first: counts, and nothing moves ────────────────────────────────────────
    before = tree(master)
    rc, out = pull(cp.LocalRemote(covey, places_cmd=places), master, dry_run=True)
    after = tree(master)
    for k in [k for k in after if k.startswith(".covey_pull")]:
        del after[k]
    check(rc == 0 and after == before, "a dry run moves nothing, staging included")
    check("would bring 9 tiles into the master" in out,
          "...and says how many would come (a leftover staged copy counts as still to fetch)")

    # ── the real pull, with the tar stream cut mid-tile and a master copy racing in ──────
    streamed_before = tree(streamed)
    os.chmod(streamed, 0)          # a tar, rsync or find that reaches into it fails
    fake = Faulty(covey, places, cut=("otm", "14/2621/5721.png"),
                  race=("usgs-topo", "15/5241/11441.png", master))
    rc, out = pull(fake, master, bulk_min=3)
    os.chmod(streamed, 0o755)
    if os.environ.get("VERBOSE"):
        print("    " + out.replace("\n", "\n    ").rstrip())

    check(rc == 0, "the pull passes (exit 0)")
    new = [k for k in remote if k not in ("otm/15/5241/11441.png", "usgs-topo/15/5241/11441.png")]
    check(all(read(master, k) == remote[k] for k in new),
          "every new tile lands in the master byte for byte (%d of them)" % len(new))
    check(read(master, "otm/15/5241/11441.png") == b"the master's own copy",
          "a master tile that differs from COVEY's is NEVER overwritten")
    check("1 differ in size from COVEY's copy; the master's copies are kept" in out,
          "...and the log says so")
    check(read(master, "usgs-topo/15/5241/11441.png") == b"RACE",
          "a master copy that appears while the pull runs wins (os.link, EEXIST)")
    check("1 were already in the master by the time they went in" in out, "...and the log says so")
    check(read(master, "otm/16/1/1.png") is None, "a zero-byte tile on COVEY is not pulled")
    zb = read(master, "covey_zero_bytes.txt")
    check(zb == b"otm/16/1/1.png\n", "...and is listed in covey_zero_bytes.txt")
    check("(1 zero-byte, not pulled)" in out, "...and in the log")
    check(read(master, "otm/14/2620/5721.png") == remote["otm/14/2620/5721.png"],
          "a leftover staged copy of COVEY's size, looking whole, is dropped unread and fetched again")
    check("2 staged copies left by an earlier, interrupted pull were dropped unread" in out,
          "...the rsync temp file with it, and the log says so")
    cut_rel = "otm/14/2621/5721.png"
    left = fake.cut_left or b""
    check(len(left) == len(remote[cut_rel]) and left != remote[cut_rel] and left[-100:] == bytes(100),
          "the tar stream really was cut inside a tile, and bsdtar left it at full size, zero-padded")
    check(read(master, cut_rel) == remote[cut_rel],
          "the cut tile is NOT pooled: it is fetched again and the master's copy is COVEY's, byte for byte")
    check("otm z14: 4 asked by tar, 3 landed whole, 1 arrived short or cut off (asked again)" in out
          and "otm z14: 4 asked by tar, 4 landed whole" not in out,
          "...and the log never counts it as landed whole")
    check(("tar", "otm", 4) in fake.calls and ("rsync", "otm", 1) in fake.calls,
          "a zoom with enough tiles goes by tar, the cut tile and a few by rsync")
    check(read(master, "otm/13/1310/2860.png") == b"phone-only",
          "a phone-only master tile is untouched, and the pass test is a set difference, not a count")
    check(not any(os.path.exists(os.path.join(master, d)) for d in
                  ("15", "-streamed", "otm-streamed", ".hidden")),
          "the legacy digit tree, *-streamed and dot folders on COVEY are never pulled")
    check(read(master, "otm/17/41940/91500.png") is None and read(master, "otm/15/5241/11443.png") is None,
          "no STREAMED tile reaches the master")
    check("Permission denied" not in out and tree(streamed) == streamed_before,
          "the streamed tree (unreadable while the pull ran) was neither read nor changed")
    check(not os.path.exists(os.path.join(master, ".incoming")), "staging ends empty and is removed")
    pulled = read(master, "covey_pulled_%s.txt" % time.strftime("%Y-%m-%d")) or b""
    check(sorted(pulled.decode().split()) == sorted(new),
          "covey_pulled_<date>.txt lists exactly the tiles this pull added")
    head = out.find(cp.HEAD)
    check(0 <= head < out.find("sources in") < out.find("otm: COVEY has"),
          "the streamed-only places are printed BEFORE anything is transferred")
    check("  otm: near Elk Camp: 513 tiles z15-17, streamed 2026-09-20 → on COVEY: Map › DL › Streamed-only"
          in out, "...as the rows {user} reads")
    have_at, knob_at = out.find(cp.HEAD_HAVE), out.find("near Covered Knob")
    check(head < have_at < knob_at < out.find("sources in") and "(master has all 6 z17 tiles there)" in out,
          "...with the place the master already covers under its own heading, not 'Will NOT reach'")
    check("streamed-only on COVEY, not pulled: otm 746 / usgs-topo 1,204" in out,
          "the done line counts what stays behind, per source")
    check("1 other file(s), not z/x/y.png, left alone" in out, "an AppleDouble file on COVEY is left alone")

    # ── again: nothing to do, and still a pass ──────────────────────────────────────────
    rc2, out2 = pull(cp.LocalRemote(covey, places_cmd=places), master)
    check(rc2 == 0 and "done: +0 tiles" in out2, "a second pull adds nothing and passes")
    check(read(master, "usgs-topo/15/5241/11441.png") == b"RACE"
          and read(master, "otm/15/5241/11441.png") == b"the master's own copy",
          "...and still leaves the master's own copies alone")

    # ── the pass test catches a tile that never arrives ─────────────────────────────────
    put(covey, "otm/16/2/2.png", body("lost", 500))
    put(covey, "otm/16/2/3.png", body("found", 500))
    lossy = Faulty(covey, places, never=("otm", "16/2/2.png"))
    rc3, out3 = pull(lossy, master)
    check(rc3 == 1, "a tile COVEY has and the master still lacks fails the pull (exit 1)")
    check("1 on COVEY missing from the master" in out3 and "16/2/2.png" in out3
          and "NOT COMPLETE" in out3, "...naming it")
    check(read(master, "otm/16/2/3.png") == body("found", 500), "...while the rest still lands")
    check(("tar", "otm", 1) in lossy.calls,
          "an rsync pass that brings nothing is followed by a tar pass (the other transport)")

    # ── no report from COVEY: REFUSED, unless told the cut-over is done ─────────────────
    old_ui = "echo 'No module named covey_ui.tilestore' >&2; exit 1"
    before4 = tree(master)
    rc4, out4 = pull(cp.LocalRemote(covey, places_cmd=old_ui), master)
    check(rc4 == 2 and "COVEY gave no streamed-only report: No module named covey_ui.tilestore" in out4
          and "REFUSED" in out4 and "sources in" not in out4,
          "an old covey-ui without the report (the pre-cut-over COVEY): the pull REFUSES (exit 2)")
    check(tree(master) == before4 and read(master, "otm/16/2/2.png") is None,
          "...and nothing moves (the tile the last pull missed is still not taken)")
    check(cp.WITHOUT in out4, "...and it says how to go on once the cut-over is known to be done")
    rc4d, out4d = pull(cp.LocalRemote(covey, places_cmd=old_ui), master, dry_run=True)
    check(rc4d == 0 and "A real pull REFUSES here" in out4d and tree(master) == before4,
          "a dry run still counts, and says a real pull would refuse")
    rc4b, out4b = pull(cp.LocalRemote(covey, places_cmd=old_ui), master, without_report=True)
    check(rc4b == 0 and read(master, "otm/16/2/2.png") == body("lost", 500),
          "--without-report: the pull carries on (the lost tile lands)")
    check("streamed-only on COVEY: unknown, no report" in out4b, "...and the done line admits it")
    refuse = ("echo 'ANCHORS visits=0 waypoints=0'; echo 'REFUSE reason=roots-overlap "
              "tile_root=/root/covey-tiles streamed_root=/root/covey-tiles'; exit 2")
    rc4r, out4r = pull(cp.LocalRemote(covey, places_cmd=refuse), master)
    check(rc4r == 2 and "no streamed-only report: REFUSE reason=roots-overlap tile_root=/root/covey-tiles"
          in out4r, "COVEY's own REFUSE line (printed on stdout) is what the pull shows")
    crash = ("echo 'ANCHORS visits=1'; echo 'ERROR Traceback (most recent call last): | "
             "File x | KeyError: 7'; echo 'a warning on stderr' >&2; exit 1")
    st, why = cp.fetch_report(cp.LocalRemote(covey, places_cmd=crash))
    check(st == "failed" and why.startswith("ERROR Traceback") and why.endswith("KeyError: 7"),
          "...and so is its ERROR line, ahead of whatever stderr said")
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        cp.report(cp.LocalRemote(covey, places_cmd=old_ui))
    check("REFUSES until it does" in buf.getvalue(),
          "`cardday.sh status` warns, the evening before, that the pull will refuse")

    # ── an EMPTY master file where COVEY has a good tile is a missing tile ──────────────
    put(covey, "otm/16/3/3.png", body("good", 600))
    put(master, "otm/16/3/3.png", b"")
    rc8, out8 = pull(cp.LocalRemote(covey, places_cmd=places), master)
    check(rc8 == 1 and "EMPTY file in the master" in out8
          and os.path.join(master, "otm", "16/3/3.png") in out8 and "NOT COMPLETE" in out8,
          "a 0-byte master file shadowing COVEY's good tile fails the pull (exit 1), naming the file")
    check(read(master, "otm/16/3/3.png") == b"", "...and the pull still never overwrites it")
    os.unlink(os.path.join(master, "otm", "16", "3", "3.png"))
    rc8b, out8b = pull(cp.LocalRemote(covey, places_cmd=places), master)
    check(rc8b == 0 and read(master, "otm/16/3/3.png") == body("good", 600),
          "once it is deleted, the next pull brings COVEY's tile and passes")

    # ── COVEY not reachable: said, nothing moves ────────────────────────────────────────
    class Gone(cp.LocalRemote):
        def unreachable(self, rc):
            return rc == 255
    gone = Gone(covey, places_cmd="echo 'ssh: Could not resolve hostname covey' >&2; exit 255")
    rc5, out5 = pull(gone, master)
    check(rc5 == 2 and "COVEY is not reachable" in out5 and "sources in" not in out5,
          "an unreachable COVEY stops the pull before it starts (exit 2)")
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc6 = cp.report(gone)
    check(rc6 == 0 and "not reachable - ssh: Could not resolve hostname covey" in buf.getvalue(),
          "the status block says 'not reachable' and does not fail")

    # ── a tile DAMAGED ON COVEY (its size, no end) is never pooled, and is named ─────────
    put(covey, "otm/16/4/4.png", padded(body("damaged", 800)))
    rc9, out9 = pull(cp.LocalRemote(covey, places_cmd=places), master)
    check(rc9 == 1 and read(master, "otm/16/4/4.png") is None
          and "do not end like a PNG or JPEG, and no stopped stream explains it" in out9
          and "otm/16/4/4.png" in out9,
          "a tile whose COVEY copy has no end is never pooled: the pull fails naming it, pointing at COVEY")
    os.unlink(os.path.join(covey, "otm", "16", "4", "4.png"))

    # ── one pull at a time ──────────────────────────────────────────────────────────────
    with open(os.path.join(master, ".covey_pull", "lock"), "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX | fcntl.LOCK_NB)
        rc7, out7 = pull(cp.LocalRemote(covey, places_cmd=places), master)
    check(rc7 == 2 and "already running" in out7, "a second pull at the same time refuses")

    # ── the command line, as cardday.sh calls it ────────────────────────────────────────
    put(covey, "usgs-img/16/10483/22883.png", body("cli", 420))
    cli = subprocess.run([sys.executable, str(ROOT / "tools" / "covey_pull.py"), "pull", "--local",
                          "--remote-root", covey, "--master", master, "--places-cmd", places],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    check(cli.returncode == 0 and read(master, "usgs-img/16/10483/22883.png") == body("cli", 420),
          "the command line pulls too")
    rep = subprocess.run([sys.executable, str(ROOT / "tools" / "covey_pull.py"), "report", "--local",
                          "--remote-root", covey, "--places-cmd", places],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    check(rep.returncode == 0 and "Map › DL › Streamed-only" in rep.stdout.decode("utf-8"),
          "`report` prints the same rows for cardday.sh status")
finally:
    try:
        os.chmod(os.path.join(td, "root", "covey-tiles-streamed"), 0o755)
    except OSError:
        pass
    shutil.rmtree(td, ignore_errors=True)

print("\n%d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
