#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# gen_tz_grid.py - part of the WiPhone firmware repository (GPL v3, see LICENSE).
# Copyright (c) 2026 {user} Howe.
#
# THE CODE in this file is GPL v3. THE DATA it writes is not: every output is a
# DERIVATIVE DATABASE of timezone-boundary-builder, which is derived from OpenStreetMap
# and published under the Open Database License (ODbL) 1.0. The outputs are therefore
# under ODbL 1.0 too, and each one carries ATTRIBUTION (below) verbatim.
# THIRD-PARTY-NOTICES.md says what that means for the firmware.
"""Generate the North American time zone grid both devices look their position up in.

WHY. Neither device follows time zones. The WiPhone's clock is a fixed offset typed in
Settings, and COVEY's Linux zone is fixed at America/Los_Angeles. Carried across a zone
line, both keep the old zone's time, and the Almanac's sunrise/sunset/legal-light times
come out hours off. Each
device will set its zone from a MEASURED position (its own GPS, or on the WiPhone the
hotspot phone's GPS), never from an IP guess. The owner's rule is that a wrong answer is worse
than none. So the grid answers "this zone" only where the answer cannot be wrong at the
grid's resolution. Everywhere else it answers BORDER or NOT_COVERED, and the device changes
nothing.

INPUT. timezone-boundary-builder's combined.json: the named IANA zones (tzid) with their
territorial waters, no ocean zones. Download it from
https://github.com/evansiroky/timezone-boundary-builder/releases. The release name cannot
be read from the file, so it is passed with --release and written into every output.

    python3 -m venv ~/tz-data/.venv && ~/tz-data/.venv/bin/pip install shapely numpy
    ~/tz-data/.venv/bin/python tools/gen_tz_grid.py ~/tz-data/combined.json --release 2026d \
        --covey <the covey-ui directory>

needs: shapely >= 2, numpy. Rules come from the system tzdata through zoneinfo (Python
3.9+). Its version (/usr/share/zoneinfo/+VERSION) is recorded in the outputs. A run takes
about a minute. The output is deterministic: the same input + the same tzdata always give
the same bytes (no timestamps; fixed random seed).

OUTPUTS
  WiPhone/tz_grid_data.h        the grid as const arrays (flash) + the zone table; only
                                WiPhone/tz_grid.cpp includes it
  tests/vectors_tzgrid.json     the canonical test vectors (COVEY: tests/fixtures/)
  tests/vectors_tzgrid.h        the same vectors as C arrays for tests/test_tzgrid.cpp
  <covey>/covey_ui/data/tzgrid.bin            with --covey (or --blob PATH): the same grid
  <covey>/tests/fixtures/vectors_tzgrid.json  as a binary file for covey_ui/tzgrid.py

THE GRID. 0.05 deg cells over lat 7..72 N, lon 188..50 W. 188 W is 172 E, so the
Aleutians west of the antimeridian (Attu) are in; a longitude east of 172 E is read as
lon - 360. That gives 1300 rows x 2760 columns. One cell is 5.6 km north-south, and
5.5 km (7 N) to 1.7 km (72 N) east-west.
Why 0.05: the grid only has to say WHICH clock. Where it cannot be sure it refuses, so the
resolution sets how wide the refused band along a boundary is, not how often it is wrong. At
0.05 deg that band is 1-2 cells, 6-11 km. Every state capital and big city in the vectors
that is not on a line itself is outside it. Halving the cell halves the band and about doubles the flash (it scales
with the boundary length in cells). Measured with --sizes (runs + row index; tzdata 2026d):
    0.100 deg   31.7 KB   refused area 469 sq deg   Knox + Jasper IN, Atikokan ON, Victoria BC
                                                    and Bermuda fall into the band
    0.050 deg   70.6 KB   refused area 243 sq deg   (the choice)
    0.025 deg  167.4 KB   refused area 125 sq deg   gains only Phenix City AL among the named towns
Either size is small next to the 6.4 MB app partition. 0.05 is where the towns near the
lines that matter come back, and finer buys little.

WHAT A CELL HOLDS.
  * CLOCK CLASSES: every zone that appears is verified against zoneinfo over 2026-2035
    (VALID_FROM..VALID_TO) as a standard offset plus either NO DST or the US/CANADA rule:
    DST from the 2nd Sunday of March, 02:00 local standard, to the 1st Sunday of November,
    02:00 local daylight. Zones whose offsets agree at every instant of the window share a
    class (America/New_York, Toronto, Detroit, Indiana/* ... = "Eastern"). A zone that
    settled into a model during 2026 is modelled from that instant (`since`, see
    classify_zone: BC, Alberta and Inuvik in tzdata 2026d). A zone that follows neither
    model (Cuba's midnight change, Greenland's EU rule) is UNKNOWN. Its class is its own,
    and the lookup answers NOT_COVERED there.
  * A cell is UNIFORM when no class boundary crosses it or any of its 8 neighbours, and all
    9 have the same class. "Crosses" is decided by rasterising every class boundary
    segment conservatively: a cell a segment might touch is marked. A uniform land cell holds
    the IANA zone at its centre. A uniform water cell (no zone: open sea beyond the
    territorial waters) holds WATER. Every other cell is BORDER.
    So an answered point is at least one whole cell from any place where the CLOCK changes.
    Boundaries between zones that keep the same clock (Detroit/Windsor, the Indiana
    counties that are all Eastern, Oregon's Mountain corner and Idaho) are NOT borders.
    Refusing there would cost the owner an answer and buy nothing. The IANA name in such a
    cell is the one at the cell's centre, so within one cell of that kind of line it can name
    the neighbour. The clock is still identical by construction (vectors mark it with
    iana_exact = false).
  * Outside the grid the answer is NOT_COVERED. A cell on the grid's edge counts the
    outside as sea, so land there is BORDER.
  * Where two classes claim the same cell centre (the disputed US/Canada waters: Machias
    Seal Island, Dixon Entrance, the Beaufort Sea), the cell counts as crossed: BORDER.

ENCODING (identical in the C header and the blob). Each row is run-length encoded as
uint16 runs:
    value << 9 | (length - 1)        value 0..125 = zone index, 126 = WATER, 127 = BORDER
                                     length 1..512 (longer runs are split)
plus a uint32 index of each row's first run (rows + 1 entries). The zone table is sorted
by IANA name.
CONTENT HASH: SHA-256 of
    b"TZGRID1\\n" + <iiiHH lat0_e7 lon0_e7 res_e7 rows cols>
    + one line per zone "iana\\tdisplay\\tstd_minutes\\trule\\tsince\\tabbr_std\\tabbr_dst\\n"
    + b"\\n" + row index as <u32 + runs as <u16.
Both test suites recompute it from what they actually load.

SELF-CHECKS (the run fails if any is violated):
  * every zone's rule verified hour by hour over 2026-2035, and each transition found to
    the second (zoneinfo, bisected) equals the model's;
  * the RLE decodes back to the grid;
  * 400,000 random points: wherever the grid answers a zone, the exact polygon class at the
    point AND at a random point up to one cell away equals the answer's class. Wherever it
    answers WATER, no polygon contains the point;
  * every named vector (state capitals, big cities, the split states, borders, sea,
    outside) answers what is written next to it below.
"""
import argparse
import calendar
import datetime
import hashlib
import json
import math
import os
import struct
import sys
import zoneinfo

try:
    import numpy as np
    import shapely
    from shapely import affinity
    from shapely.geometry import box, shape
except ImportError as e:  # pragma: no cover
    sys.exit("need shapely >= 2 and numpy (see the docstring for a venv): %s" % e)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---- the grid ----------------------------------------------------------------------------
E7 = 10000000
LAT0_E7 = 7 * E7            # 7 N: Panama, Costa Rica, Trinidad, the whole Caribbean basin
LAT1_E7 = 72 * E7           # 72 N: all of mainland Alaska (Utqiagvik 71.3 N) and the Canadian mainland
LON0_E7 = -188 * E7         # 188 W = 172 E: Attu, at the far end of the Aleutians
LON1_E7 = -50 * E7          # 50 W: Newfoundland, Nuuk
RES_E7 = 500000             # 0.05 deg (exact in 1e-7 units, so the cell of a point is integer math)

CODE_WATER = 126
CODE_BORDER = 127
MAX_ZONES = 126             # value field is 7 bits
RUN_MAX = 512               # length field is 9 bits (length - 1)

RULE_NONE, RULE_USCAN, RULE_UNKNOWN = 0, 1, 2
RULE_NAMES = {RULE_NONE: "NONE", RULE_USCAN: "USCAN", RULE_UNKNOWN: "UNKNOWN"}

# The lookup's reason codes (WiPhone/tz_grid.h TzWhy, covey_ui/tzgrid.py WHY_*).
WHY_ZONE, WHY_BORDER, WHY_WATER, WHY_OUTSIDE, WHY_UNKNOWN_RULE, WHY_BAD_INPUT = range(6)
WHY_NAMES = ["ZONE", "BORDER", "WATER", "OUTSIDE", "UNKNOWN_RULE", "BAD_INPUT"]
ANS_BORDER, ANS_NOT_COVERED = -1, -2

VALID_FROM, VALID_TO = 2026, 2035      # the rules are verified for these years (inclusive)
VEC_FROM, VEC_TO = 2026, 2030          # every transition in these years becomes a vector

ATTRIBUTION_FMT = (
    "Time zone grid derived from timezone-boundary-builder {release} "
    "(github.com/evansiroky/timezone-boundary-builder), (c) OpenStreetMap contributors. "
    "Made available under the Open Database License (ODbL) 1.0 "
    "(opendatacommons.org/licenses/odbl/1-0/). This grid is a derivative database under ODbL 1.0; "
    "it is remade by tools/gen_tz_grid.py in the WiPhone firmware repository.")

# Short names for the clock classes, by (standard offset in minutes, rule). Kept to 16
# characters so a later screen row has room. A class not listed gets "UTC-5:30" style.
DISPLAY = {
    (-660, RULE_NONE): "UTC-11",
    (-600, RULE_NONE): "Hawaii",
    (-600, RULE_USCAN): "Aleutian",
    (-540, RULE_USCAN): "Alaska",
    (-480, RULE_USCAN): "Pacific",
    (-420, RULE_USCAN): "Mountain",
    (-420, RULE_NONE): "Mountain, no DST",
    (-360, RULE_USCAN): "Central",
    (-360, RULE_NONE): "Central, no DST",
    (-300, RULE_USCAN): "Eastern",
    (-300, RULE_NONE): "Eastern, no DST",
    (-240, RULE_USCAN): "Atlantic",
    (-240, RULE_NONE): "Atlantic, no DST",
    (-210, RULE_USCAN): "Newfoundland",
    (-180, RULE_USCAN): "St Pierre",
}


def display_name(iana, std, rule):
    if rule == RULE_UNKNOWN:
        return iana.rsplit("/", 1)[-1].replace("_", " ")[:16]
    if (std, rule) in DISPLAY:
        return DISPLAY[(std, rule)]
    h, m = divmod(abs(std), 60)
    s = "UTC%s%d" % ("-" if std < 0 else "+", h) + (":%02d" % m if m else "")
    return s + (" DST" if rule == RULE_USCAN else "")


# ---- the rule model, written independently of the C (datetime, not day arithmetic) -------

def us_can_transitions(year, std_min):
    """(start, end) of DST as UTC seconds: 2nd Sunday of March 02:00 local standard time,
    1st Sunday of November 02:00 local daylight time (= 01:00 standard)."""
    def nth_sunday(month, n):
        d = datetime.date(year, month, 1)
        first = 1 + (6 - d.weekday()) % 7          # weekday(): Monday 0 .. Sunday 6
        return first + 7 * (n - 1)
    s = calendar.timegm((year, 3, nth_sunday(3, 2), 2, 0, 0)) - std_min * 60
    e = calendar.timegm((year, 11, nth_sunday(11, 1), 2, 0, 0)) - (std_min + 60) * 60
    return s, e


def model_offset(std, rule, t):
    if rule == RULE_NONE:
        return std
    y = datetime.datetime.fromtimestamp(t + std * 60, datetime.timezone.utc).year
    s, e = us_can_transitions(y, std)
    return std + 60 if s <= t < e else std


def zi_offset(zi, t):
    return int(datetime.datetime.fromtimestamp(t, zi).utcoffset().total_seconds()) // 60


def zi_transitions(zi, t0, t1):
    """Every instant in (t0, t1] where zoneinfo's offset changes, to the second: sampled
    hourly, then each change bisected (the first second with the new offset)."""
    out = []
    prev_t, prev = t0, zi_offset(zi, t0)
    t = t0
    while t < t1:
        t = min(t + 3600, t1)
        cur = zi_offset(zi, t)
        if cur != prev:
            lo, hi = prev_t, t                 # offset(lo) == prev, offset(hi) == cur
            while hi - lo > 1:
                mid = (lo + hi) // 2
                if zi_offset(zi, mid) == prev:
                    lo = mid
                else:
                    hi = mid
            out.append((hi, cur))
        prev_t, prev = t, cur
    return out


SINCE_CUTOFF = calendar.timegm((VALID_FROM + 1, 1, 1, 0, 0, 0))


def classify_zone(tzid):
    """-> dict(std, rule, since, abbr_std, abbr_dst, why), verified from `since` to the end of
    VALID_TO.

    since = 0: the model holds all through VALID_FROM..VALID_TO.
    since = a UTC instant in VALID_FROM's year: the zone changed rules that year, and the
    model holds from that instant on. tzdata 2026d has three of these. America/Vancouver
    was on the Canada rule until 2026-03-08 and has been UTC-7 since. America/Edmonton and
    America/Inuvik have been UTC-6 since that day. Without `since` they would be UNKNOWN and
    all of BC and Alberta would answer NOT_COVERED. Both devices refuse an offset for an
    instant before `since`: it is outside what was verified.
    A zone that fits neither model from any instant of VALID_FROM's year on is UNKNOWN."""
    unknown = lambda why: dict(std=0, rule=RULE_UNKNOWN, since=0, abbr_std="", abbr_dst="", why=why)
    try:
        zi = zoneinfo.ZoneInfo(tzid)
    except Exception:
        return unknown("not in tzdata")
    t0 = calendar.timegm((VALID_FROM, 1, 1, 0, 0, 0))
    t1 = calendar.timegm((VALID_TO + 1, 1, 1, 0, 0, 0))
    actual = zi_transitions(zi, t0, t1)
    for since in [t0] + [t for (t, _) in actual if t < SINCE_CUTOFF]:
        # the first January after `since` gives the standard offset and the abbreviations
        y = VALID_FROM if since == t0 else \
            datetime.datetime.fromtimestamp(since, datetime.timezone.utc).year + 1
        jan = datetime.datetime.fromtimestamp(calendar.timegm((y, 1, 15, 12, 0, 0)), zi)
        jul = datetime.datetime.fromtimestamp(calendar.timegm((y, 7, 15, 12, 0, 0)), zi)
        if int(jan.utcoffset().total_seconds()) % 60:
            return unknown("offset not whole minutes")
        rest = [(t, o) for (t, o) in actual if t > since]
        if not rest:
            std = zi_offset(zi, since)
            # constant from `since` on; still compared at the US instants, in case a change and
            # its undo fell inside one hour (they never do; this checks "never", not assumes it)
            if any(zi_offset(zi, t) != std
                   for yy in range(VALID_FROM, VALID_TO + 1) for tr in us_can_transitions(yy, std)
                   for t in (tr - 1, tr, tr + 1) if t >= since):
                return unknown("sub-hour change")
            return dict(std=std, rule=RULE_NONE, since=0 if since == t0 else since,
                        abbr_std=jan.tzname(), abbr_dst="", why="")
        std = int(jan.utcoffset().total_seconds()) // 60
        if jan.dst() != datetime.timedelta(0):
            continue
        model = []
        for yy in range(VALID_FROM, VALID_TO + 1):
            s, e = us_can_transitions(yy, std)
            model += [tr for tr in ((s, std + 60), (e, std)) if since < tr[0] <= t1]
        if rest == model and zi_offset(zi, since) == model_offset(std, RULE_USCAN, since):
            return dict(std=std, rule=RULE_USCAN, since=0 if since == t0 else since,
                        abbr_std=jan.tzname(), abbr_dst=jul.tzname(), why="")
    return unknown("%d transitions, not the US/Canada rule" % len(actual))


def tzdata_version():
    for d in zoneinfo.TZPATH:
        p = os.path.join(d, "+VERSION")
        if os.path.isfile(p):
            return open(p).read().strip()
    try:
        import tzdata  # noqa
        return tzdata.IANA_VERSION
    except Exception:
        return "unknown"


# ---- geometry ----------------------------------------------------------------------------

class Grid:
    def __init__(self, res_e7=RES_E7):
        self.res_e7 = res_e7
        self.res = res_e7 / E7
        self.lat0 = LAT0_E7 / E7
        self.lon0 = LON0_E7 / E7
        self.rows = (LAT1_E7 - LAT0_E7) // res_e7
        self.cols = (LON1_E7 - LON0_E7) // res_e7
        assert self.rows * res_e7 == LAT1_E7 - LAT0_E7 and self.cols * res_e7 == LON1_E7 - LON0_E7
        assert self.rows < 65536 and self.cols < 65536


def polygonal(g):
    parts = [p for p in shapely.get_parts(g) if p.geom_type == "Polygon" and not p.is_empty]
    if not parts:
        return None
    return shapely.union_all(parts)


def load_zones(path, grid):
    """{tzid: geometry in grid longitudes (-188..-50)}, clipped to the region + 3 cells. The
    part east of 172 E is shifted by -360, so it meets the rest at -180 and the class union
    closes the antimeridian seam."""
    m = 3 * grid.res
    lat_lo, lat_hi = grid.lat0 - m, grid.lat0 + grid.rows * grid.res + m
    lon_hi = grid.lon0 + grid.cols * grid.res + m
    main_box = box(-180.0, lat_lo, lon_hi, lat_hi)
    wrap_box = box(grid.lon0 + 360.0 - m, lat_lo, 180.0, lat_hi) if grid.lon0 < -180 else None
    with open(path) as f:
        fc = json.load(f)
    zones = {}
    for feat in fc["features"]:
        tzid = feat["properties"]["tzid"]
        g = shape(feat["geometry"])
        pieces = []
        a = polygonal(shapely.intersection(g, main_box))
        if a is not None:
            pieces.append(a)
        if wrap_box is not None:
            b = polygonal(shapely.intersection(g, wrap_box))
            if b is not None:
                pieces.append(affinity.translate(b, xoff=-360.0))
        if pieces:
            u = shapely.union_all(pieces)
            if u.area > 0:
                zones[tzid] = u
    return zones


def boundary_segments(geom):
    parts = shapely.get_parts(shapely.boundary(geom))
    coords, idx = shapely.get_coordinates(parts, return_index=True)
    same = idx[1:] == idx[:-1]
    return (coords[:-1, 0][same], coords[:-1, 1][same], coords[1:, 0][same], coords[1:, 1][same])


def mark_crossed(crossed, grid, segs):
    """Mark every cell a boundary segment might touch. Segments are cut into pieces no longer
    than 1/4 cell on either axis, and the cells under each piece's bounding box, widened by a
    hair, are marked. That is a superset of the cells the line passes through. The error is
    always toward BORDER."""
    x0, y0, x1, y1 = segs
    if len(x0) == 0:
        return
    u0 = (x0 - grid.lon0) / grid.res
    v0 = (y0 - grid.lat0) / grid.res
    du = (x1 - grid.lon0) / grid.res - u0
    dv = (y1 - grid.lat0) / grid.res - v0
    n = np.maximum(1, np.ceil(np.maximum(np.abs(du), np.abs(dv)) / 0.25)).astype(np.int64)
    seg = np.repeat(np.arange(len(n)), n)
    k = np.arange(int(n.sum())) - np.repeat(np.cumsum(n) - n, n)
    nn = n[seg]
    a, b = k / nn, (k + 1) / nn
    pu0, pu1 = u0[seg] + du[seg] * a, u0[seg] + du[seg] * b
    pv0, pv1 = v0[seg] + dv[seg] * a, v0[seg] + dv[seg] * b
    eps = 1e-6
    umin = np.floor(np.minimum(pu0, pu1) - eps).astype(np.int64)
    umax = np.floor(np.maximum(pu0, pu1) + eps).astype(np.int64)
    vmin = np.floor(np.minimum(pv0, pv1) - eps).astype(np.int64)
    vmax = np.floor(np.maximum(pv0, pv1) + eps).astype(np.int64)
    assert int((umax - umin).max()) <= 1 and int((vmax - vmin).max()) <= 1
    for uu in (umin, umax):
        for vv in (vmin, vmax):
            ok = (uu >= 0) & (uu < grid.cols) & (vv >= 0) & (vv < grid.rows)
            crossed[vv[ok], uu[ok]] = True


def centres_in(geom, grid):
    """(rows, cols, mask) of the cell centres inside geom (prepared, vectorised)."""
    minx, miny, maxx, maxy = geom.bounds
    c0 = max(0, int(math.floor((minx - grid.lon0) / grid.res)) - 1)
    c1 = min(grid.cols, int(math.ceil((maxx - grid.lon0) / grid.res)) + 1)
    r0 = max(0, int(math.floor((miny - grid.lat0) / grid.res)) - 1)
    r1 = min(grid.rows, int(math.ceil((maxy - grid.lat0) / grid.res)) + 1)
    if c0 >= c1 or r0 >= r1:
        return None
    xs = grid.lon0 + (np.arange(c0, c1) + 0.5) * grid.res
    ys = grid.lat0 + (np.arange(r0, r1) + 0.5) * grid.res
    X, Y = np.meshgrid(xs, ys)
    shapely.prepare(geom)
    mask = shapely.contains_xy(geom, X, Y)
    return r0, c0, mask


def which_class(points_x, points_y, class_geoms):
    """Exact class index at each point (-1 = in no zone), straight from the polygons."""
    out = np.full(len(points_x), -1, dtype=np.int32)
    for k, g in enumerate(class_geoms):
        shapely.prepare(g)
        hit = shapely.contains_xy(g, points_x, points_y)
        out[hit] = k
    return out


# ---- building the grid -------------------------------------------------------------------

def build(grid, zones, info):
    """-> dict with the value array, the zone table and diagnostics."""
    # clock classes: (std, rule) for verified zones, the zone itself for UNKNOWN ones
    def ckey(tz):
        i = info[tz]
        return ("UNKNOWN", tz) if i["rule"] == RULE_UNKNOWN else (i["std"], i["rule"])
    keys = sorted({ckey(tz) for tz in zones}, key=lambda k: (str(k[0]), str(k[1])))
    kidx = {k: n for n, k in enumerate(keys)}
    zone_class = {tz: kidx[ckey(tz)] for tz in zones}
    class_geoms = [shapely.union_all([zones[tz] for tz in sorted(zones) if zone_class[tz] == n])
                   for n in range(len(keys))]

    R, C = grid.rows, grid.cols
    crossed = np.zeros((R, C), dtype=bool)
    for g in class_geoms:
        mark_crossed(crossed, grid, boundary_segments(g))
    crossed_iana = crossed.copy()
    for tz in sorted(zones):
        mark_crossed(crossed_iana, grid, boundary_segments(zones[tz]))

    cls = np.full((R, C), -1, dtype=np.int32)
    claims = np.zeros((R, C), dtype=np.int32)
    for k, g in enumerate(class_geoms):
        got = centres_in(g, grid)
        if got is None:
            continue
        r0, c0, mask = got
        sub = cls[r0:r0 + mask.shape[0], c0:c0 + mask.shape[1]]
        sub[mask] = k
        claims[r0:r0 + mask.shape[0], c0:c0 + mask.shape[1]] += mask
    overlaps = int((claims > 1).sum())
    crossed |= claims > 1              # two classes claim one centre: never answer there

    names = sorted(zones)
    iana = np.full((R, C), -1, dtype=np.int32)
    for zi, tz in enumerate(names):
        got = centres_in(zones[tz], grid)
        if got is None:
            continue
        r0, c0, mask = got
        sub = iana[r0:r0 + mask.shape[0], c0:c0 + mask.shape[1]]
        sub[mask & (sub < 0)] = zi

    # uniform = this cell and its 8 neighbours unmarked and of one class; outside = water
    P = np.pad(~crossed, 1, constant_values=True)
    K = np.pad(cls, 1, constant_values=-1)
    uniform = np.ones((R, C), dtype=bool)
    for dr in (-1, 0, 1):
        for dc in (-1, 0, 1):
            uniform &= P[1 + dr:1 + dr + R, 1 + dc:1 + dc + C]
            uniform &= K[1 + dr:1 + dr + R, 1 + dc:1 + dc + C] == cls

    land = uniform & (cls >= 0)
    # A uniform land cell's IANA zone is the one at its centre, which must be of the cell's
    # class. A centre exactly on a same-class IANA line is in neither polygon: probe inside.
    zone_of = np.array([zone_class[tz] for tz in names], dtype=np.int32)
    bad = land & ((iana < 0) | (zone_of[np.maximum(iana, 0)] != cls))
    fixups = 0
    for r, c in zip(*np.nonzero(bad)):
        found = -1
        for fu, fv in ((0.3, 0.3), (0.7, 0.3), (0.3, 0.7), (0.7, 0.7), (0.41, 0.59), (0.59, 0.41)):
            x = grid.lon0 + (c + fu) * grid.res
            y = grid.lat0 + (r + fv) * grid.res
            for zi, tz in enumerate(names):
                if zone_class[tz] == cls[r, c] and shapely.contains_xy(zones[tz], x, y):
                    found = zi
                    break
            if found >= 0:
                break
        assert found >= 0, "no zone of the cell's class inside a uniform cell (%d, %d)" % (r, c)
        iana[r, c] = found
        fixups += 1

    used = sorted({names[z] for z in np.unique(iana[land])})
    if len(used) > MAX_ZONES:
        sys.exit("%d zones appear; the 7-bit run value holds %d" % (len(used), MAX_ZONES))
    table_index = {tz: n for n, tz in enumerate(used)}
    remap = np.full(len(names) + 1, CODE_BORDER, dtype=np.int32)
    for zi, tz in enumerate(names):
        if tz in table_index:
            remap[zi] = table_index[tz]
    value = np.full((R, C), CODE_BORDER, dtype=np.int32)
    value[uniform & (cls < 0)] = CODE_WATER
    value[land] = remap[iana[land]]
    assert not (value[land] == CODE_BORDER).any()

    table = []
    for tz in used:
        i = info[tz]
        table.append(dict(iana=tz, display=display_name(tz, i["std"], i["rule"]), std=i["std"],
                          rule=i["rule"], since=i["since"], abbr_std=i["abbr_std"], abbr_dst=i["abbr_dst"]))
    return dict(value=value, table=table, class_geoms=class_geoms, class_keys=keys,
                zone_class=zone_class, crossed=crossed, crossed_iana=crossed_iana,
                uniform=uniform, overlaps=overlaps, fixups=fixups, names=names, cls=cls)


def encode(value):
    rows, runs = [0], []
    for r in range(value.shape[0]):
        row = value[r]
        change = np.nonzero(np.diff(row))[0] + 1
        starts = np.concatenate(([0], change))
        ends = np.concatenate((change, [len(row)]))
        for s, e in zip(starts, ends):
            v, n = int(row[s]), int(e - s)
            while n > 0:
                take = min(n, RUN_MAX)
                runs.append((v << 9) | (take - 1))
                n -= take
        rows.append(len(runs))
    return rows, runs


def decode_row(rows, runs, r, cols):
    out = []
    for w in runs[rows[r]:rows[r + 1]]:
        out += [w >> 9] * ((w & 511) + 1)
    assert len(out) == cols
    return out


def table_lines(table):
    return "".join("%s\t%s\t%d\t%d\t%d\t%s\t%s\n" % (z["iana"], z["display"], z["std"], z["rule"],
                                                    z["since"], z["abbr_std"], z["abbr_dst"]) for z in table)


def content_hash(grid, table, rows, runs):
    h = hashlib.sha256()
    h.update(b"TZGRID1\n")
    h.update(struct.pack("<iiiHH", LAT0_E7, LON0_E7, grid.res_e7, grid.rows, grid.cols))
    h.update(table_lines(table).encode("utf-8"))
    h.update(b"\n")
    h.update(struct.pack("<%dI" % len(rows), *rows))
    h.update(struct.pack("<%dH" % len(runs), *runs))
    return h.hexdigest()


# ---- the lookup, as the devices do it (integer math on 1e-7 degrees) ---------------------

def ref_lookup(value, table, grid, lat_e7, lon_e7):
    """-> (answer, why): answer >= 0 zone index, -1 BORDER, -2 NOT_COVERED."""
    if not (-90 * E7 <= lat_e7 <= 90 * E7) or not (-180 * E7 <= lon_e7 <= 180 * E7):
        return ANS_NOT_COVERED, WHY_BAD_INPUT
    lon = lon_e7
    if lon - LON0_E7 >= 360 * E7:
        lon -= 360 * E7
    dy, dx = lat_e7 - LAT0_E7, lon - LON0_E7
    if dy < 0 or dx < 0:
        return ANS_NOT_COVERED, WHY_OUTSIDE
    r, c = dy // grid.res_e7, dx // grid.res_e7
    if r >= grid.rows or c >= grid.cols:
        return ANS_NOT_COVERED, WHY_OUTSIDE
    v = int(value[r, c])
    if v == CODE_BORDER:
        return ANS_BORDER, WHY_BORDER
    if v == CODE_WATER:
        return ANS_NOT_COVERED, WHY_WATER
    if table[v]["rule"] == RULE_UNKNOWN:
        return ANS_NOT_COVERED, WHY_UNKNOWN_RULE
    return v, WHY_ZONE


def e7(x):
    return int(round(x * E7))


def deg_to_grid_lon(lon):
    return lon - 360.0 if lon - LON0_E7 / E7 >= 360.0 else lon


# ---- named vectors -----------------------------------------------------------------------
# (name, lat, lon, expect), every one checked here before it becomes a vector. expect is:
#   "America/X"   answers a zone of America/X's clock, and the polygon at the point IS America/X
#   "=Eastern"    answers a zone whose display name is "Eastern" (the polygon is recorded)
#   "?zone"       answers some zone (for places whose IANA naming was not certain in advance)
#   "?any"        anything (recorded as found: places that sit 2-3 cells from a line)
#   BORDER, WATER, OUTSIDE, UNKNOWN_RULE   that reason
# Wherever a zone is answered, its clock must also equal the polygon's (checked for all).
# Public coordinates only: capitals and city centres, rounded.
NAMED = [
    # US state capitals
    ("Montgomery AL", 32.3770, -86.3000, "America/Chicago"),
    ("Juneau AK", 58.3020, -134.4200, "America/Juneau"),
    ("Phoenix AZ", 33.4480, -112.0740, "America/Phoenix"),
    ("Little Rock AR", 34.7460, -92.2900, "America/Chicago"),
    ("Sacramento CA", 38.5760, -121.4940, "America/Los_Angeles"),
    ("Denver CO", 39.7390, -104.9850, "America/Denver"),
    ("Hartford CT", 41.7640, -72.6820, "America/New_York"),
    ("Dover DE", 39.1580, -75.5240, "America/New_York"),
    ("Tallahassee FL", 30.4380, -84.2810, "America/New_York"),
    ("Atlanta GA", 33.7490, -84.3880, "America/New_York"),
    ("Honolulu HI", 21.3070, -157.8580, "Pacific/Honolulu"),
    ("Boise ID", 43.6150, -116.2020, "America/Boise"),
    ("Springfield IL", 39.7980, -89.6540, "America/Chicago"),
    ("Indianapolis IN", 39.7680, -86.1580, "America/Indiana/Indianapolis"),
    ("Des Moines IA", 41.5910, -93.6040, "America/Chicago"),
    ("Topeka KS", 39.0480, -95.6780, "America/Chicago"),
    ("Frankfort KY", 38.1870, -84.8750, "America/New_York"),
    ("Baton Rouge LA", 30.4570, -91.1870, "America/Chicago"),
    ("Augusta ME", 44.3070, -69.7820, "America/New_York"),
    ("Annapolis MD", 38.9780, -76.4910, "America/New_York"),
    ("Boston MA", 42.3580, -71.0640, "America/New_York"),
    ("Lansing MI", 42.7330, -84.5550, "America/Detroit"),
    ("Saint Paul MN", 44.9550, -93.1020, "America/Chicago"),
    ("Jackson MS", 32.2990, -90.1850, "America/Chicago"),
    ("Jefferson City MO", 38.5770, -92.1730, "America/Chicago"),
    ("Helena MT", 46.5860, -112.0180, "America/Denver"),
    ("Lincoln NE", 40.8080, -96.7000, "America/Chicago"),
    ("Carson City NV", 39.1640, -119.7660, "America/Los_Angeles"),
    ("Concord NH", 43.2070, -71.5380, "America/New_York"),
    ("Trenton NJ", 40.2200, -74.7700, "America/New_York"),
    ("Santa Fe NM", 35.6820, -105.9400, "America/Denver"),
    ("Albany NY", 42.6530, -73.7570, "America/New_York"),
    ("Raleigh NC", 35.7800, -78.6390, "America/New_York"),
    ("Bismarck ND", 46.8080, -100.7840, "America/Chicago"),
    ("Columbus OH", 39.9620, -83.0010, "America/New_York"),
    ("Oklahoma City OK", 35.4680, -97.5160, "America/Chicago"),
    ("Salem OR", 44.9380, -123.0300, "America/Los_Angeles"),
    ("Harrisburg PA", 40.2640, -76.8840, "America/New_York"),
    ("Providence RI", 41.8240, -71.4130, "America/New_York"),
    ("Columbia SC", 34.0010, -81.0350, "America/New_York"),
    ("Pierre SD (on the Missouri: Fort Pierre across it is Mountain)", 44.3680, -100.3510, "BORDER"),
    ("Nashville TN", 36.1630, -86.7810, "America/Chicago"),
    ("Austin TX", 30.2670, -97.7430, "America/Chicago"),
    ("Salt Lake City UT", 40.7610, -111.8910, "America/Denver"),
    ("Montpelier VT", 44.2600, -72.5760, "America/New_York"),
    ("Richmond VA", 37.5410, -77.4360, "America/New_York"),
    ("Olympia WA", 47.0380, -122.9000, "America/Los_Angeles"),
    ("Charleston WV", 38.3500, -81.6330, "America/New_York"),
    ("Madison WI", 43.0750, -89.3840, "America/Chicago"),
    ("Cheyenne WY", 41.1400, -104.8200, "America/Denver"),
    ("Washington DC", 38.8950, -77.0360, "America/New_York"),
    # big cities, Alaska, Hawaii
    ("New York NY", 40.7128, -74.0060, "America/New_York"),
    ("Chicago IL", 41.8781, -87.6298, "America/Chicago"),
    ("Los Angeles CA", 34.0522, -118.2437, "America/Los_Angeles"),
    ("Seattle WA", 47.6062, -122.3321, "America/Los_Angeles"),
    ("Houston TX", 29.7604, -95.3698, "America/Chicago"),
    ("Miami FL", 25.7617, -80.1918, "America/New_York"),
    ("Detroit MI", 42.3314, -83.0458, "America/Detroit"),
    ("Windsor ON (across the river from Detroit)", 42.3149, -83.0364, "?zone"),
    ("Anchorage AK", 61.2181, -149.9003, "America/Anchorage"),
    ("Fairbanks AK", 64.8378, -147.7164, "America/Anchorage"),
    ("Utqiagvik AK", 71.2906, -156.7886, "America/Anchorage"),
    ("Nome AK", 64.5011, -165.4064, "America/Nome"),
    ("Adak AK", 51.8800, -176.6580, "America/Adak"),
    ("Hilo HI", 19.7070, -155.0850, "Pacific/Honolulu"),
    ("Attu AK (west of 180: read as lon - 360)", 52.8600, 173.1500, "America/Adak"),
    ("Anadyr, Chukotka (UTC+12)", 64.7337, 177.4968, "Asia/Anadyr"),
    # the split states, away from their lines
    ("Evansville IN (Central)", 37.9716, -87.5711, "America/Chicago"),
    ("Gary IN (Central)", 41.5934, -87.3464, "America/Chicago"),
    ("Fort Wayne IN (Eastern)", 41.0793, -85.1394, "America/Indiana/Indianapolis"),
    ("Vincennes IN (on the Wabash: Illinois across it is Central)", 38.6773, -87.5286, "BORDER"),
    ("Washington IN (Eastern, Daviess County)", 38.6592, -87.1728, "=Eastern"),
    ("Jasper IN (Eastern, Dubois County)", 38.3914, -86.9311, "=Eastern"),
    ("Knox IN (Central)", 41.2959, -86.6250, "America/Indiana/Knox"),
    ("Tell City IN (Central)", 37.9514, -86.7678, "?zone"),
    ("Louisville KY (Eastern)", 38.2527, -85.7585, "America/Kentucky/Louisville"),
    ("Lexington KY (Eastern)", 38.0406, -84.5037, "America/New_York"),
    ("Bowling Green KY (Central)", 36.9685, -86.4808, "America/Chicago"),
    ("Paducah KY (Central)", 37.0834, -88.6000, "America/Chicago"),
    ("Fargo ND (Central)", 46.8772, -96.7898, "America/Chicago"),
    ("Dickinson ND (Mountain)", 46.8792, -102.7896, "America/Denver"),
    ("Williston ND (Central)", 48.1470, -103.6180, "?zone"),
    ("Center ND (Central)", 47.1164, -101.2996, "?zone"),
    ("Rapid City SD (Mountain)", 44.0805, -103.2310, "America/Denver"),
    ("Sioux Falls SD (Central)", 43.5446, -96.7311, "America/Chicago"),
    ("Scottsbluff NE (Mountain)", 41.8666, -103.6672, "America/Denver"),
    ("Goodland KS (Mountain)", 39.3508, -101.7102, "America/Denver"),
    ("Wichita KS (Central)", 37.6872, -97.3301, "America/Chicago"),
    ("El Paso TX (Mountain)", 31.7619, -106.4850, "America/Denver"),
    ("Coeur d'Alene ID (Pacific)", 47.6777, -116.7805, "America/Los_Angeles"),
    ("Moscow ID (Pacific)", 46.7324, -117.0002, "America/Los_Angeles"),
    ("Idaho Falls ID (Mountain)", 43.4917, -112.0339, "America/Boise"),
    ("Ontario OR (Mountain)", 44.0266, -116.9629, "America/Boise"),
    ("Bend OR (Pacific)", 44.0582, -121.3153, "America/Los_Angeles"),
    ("Pensacola FL (Central)", 30.4213, -87.2169, "America/Chicago"),
    ("Knoxville TN (Eastern)", 35.9606, -83.9207, "America/New_York"),
    ("Johnson City TN (Eastern)", 36.3134, -82.3535, "America/New_York"),
    ("Memphis TN (Central)", 35.1495, -90.0490, "America/Chicago"),
    ("Iron Mountain MI (Central)", 45.8202, -88.0660, "America/Menominee"),
    ("Marquette MI (Eastern)", 46.5436, -87.3954, "America/Detroit"),
    # Arizona: no DST, except the Navajo Nation (Denver), except the Hopi inside it (Phoenix)
    ("Tucson AZ", 32.2226, -110.9747, "America/Phoenix"),
    ("Flagstaff AZ", 35.1983, -111.6513, "America/Phoenix"),
    ("Tuba City AZ (Navajo; Hopi Moenkopi next door)", 36.1350, -111.2399, "BORDER"),
    ("Kayenta AZ (Navajo Nation)", 36.7278, -110.2546, "America/Denver"),
    ("Chinle AZ (Navajo Nation)", 36.1544, -109.5526, "America/Denver"),
    ("Kykotsmovi AZ (Hopi)", 35.8714, -110.6232, "?zone"),
    # Canada
    ("Ottawa ON", 45.4215, -75.6972, "America/Toronto"),
    ("Toronto ON", 43.6532, -79.3832, "America/Toronto"),
    ("Montreal QC", 45.5017, -73.5673, "America/Toronto"),
    ("Quebec City QC", 46.8139, -71.2080, "America/Toronto"),
    ("Winnipeg MB", 49.8951, -97.1384, "America/Winnipeg"),
    ("Regina SK (no DST)", 50.4452, -104.6189, "America/Regina"),
    ("Saskatoon SK (no DST)", 52.1332, -106.6700, "America/Regina"),
    ("Swift Current SK (no DST)", 50.2881, -107.7939, "America/Swift_Current"),
    ("Edmonton AB (UTC-6 all year since 2026-03-08)", 53.5461, -113.4938, "America/Edmonton"),
    ("Calgary AB", 51.0447, -114.0719, "America/Edmonton"),
    ("Vancouver BC (UTC-7 all year since 2026-03-08)", 49.2827, -123.1207, "America/Vancouver"),
    ("Victoria BC", 48.4284, -123.3656, "America/Vancouver"),
    ("Cranbrook BC (Mountain)", 49.5097, -115.7688, "America/Edmonton"),
    ("Dawson Creek BC (no DST)", 55.7596, -120.2377, "America/Dawson_Creek"),
    ("Fort Nelson BC (no DST)", 58.8050, -122.6972, "America/Fort_Nelson"),
    ("Whitehorse YT (no DST)", 60.7212, -135.0568, "America/Whitehorse"),
    ("Yellowknife NT", 62.4540, -114.3718, "America/Edmonton"),
    ("Inuvik NT", 68.3607, -133.7230, "America/Inuvik"),
    ("Iqaluit NU", 63.7467, -68.5170, "America/Iqaluit"),
    ("Rankin Inlet NU", 62.8090, -92.0853, "America/Rankin_Inlet"),
    ("Cambridge Bay NU", 69.1169, -105.0597, "America/Cambridge_Bay"),
    ("Kenora ON (Central)", 49.7671, -94.4894, "America/Winnipeg"),
    ("Thunder Bay ON (Eastern)", 48.3809, -89.2477, "America/Toronto"),
    ("Atikokan ON (no DST)", 48.7577, -91.6216, "America/Atikokan"),
    ("Halifax NS", 44.6488, -63.5752, "America/Halifax"),
    ("Fredericton NB", 45.9636, -66.6431, "America/Moncton"),
    ("Charlottetown PE", 46.2382, -63.1311, "America/Halifax"),
    ("St. John's NL", 47.5615, -52.7126, "America/St_Johns"),
    ("Gander NL", 48.9569, -54.6089, "America/St_Johns"),
    ("Happy Valley-Goose Bay NL", 53.3017, -60.3261, "America/Goose_Bay"),
    # Mexico: no DST since 2022, except the border cities on the US rule
    ("Mexico City", 19.4326, -99.1332, "America/Mexico_City"),
    ("Guadalajara", 20.6597, -103.3496, "America/Mexico_City"),
    ("Monterrey", 25.6866, -100.3161, "America/Monterrey"),
    ("Tijuana (US rule)", 32.5149, -117.0382, "?zone"),
    ("Mexicali (US rule)", 32.6245, -115.4523, "?zone"),
    ("Ciudad Juarez (US rule)", 31.6904, -106.4245, "?zone"),
    ("Matamoros (US rule)", 25.8690, -97.5027, "?zone"),
    ("Hermosillo (no DST)", 29.0729, -110.9559, "America/Hermosillo"),
    ("Chihuahua (no DST)", 28.6320, -106.0691, "America/Chihuahua"),
    ("Mazatlan (no DST)", 23.2494, -106.4111, "America/Mazatlan"),
    ("Merida (no DST)", 20.9674, -89.5926, "America/Merida"),
    ("Cancun (no DST)", 21.1619, -86.8515, "America/Cancun"),
    # the Caribbean and Central America
    ("San Juan PR", 18.4655, -66.1057, "America/Puerto_Rico"),
    ("Nassau BS", 25.0443, -77.3504, "America/Nassau"),
    ("Kingston JM", 17.9714, -76.7920, "America/Jamaica"),
    ("Port-au-Prince HT", 18.5944, -72.3074, "America/Port-au-Prince"),
    ("Santo Domingo DO", 18.4861, -69.9312, "America/Santo_Domingo"),
    ("Hamilton BM", 32.2949, -64.7814, "?zone"),
    ("Guatemala City", 14.6349, -90.5069, "America/Guatemala"),
    ("Tegucigalpa", 14.0723, -87.1921, "America/Tegucigalpa"),
    ("San Salvador", 13.6929, -89.2182, "America/El_Salvador"),
    ("Managua", 12.1150, -86.2362, "America/Managua"),
    ("San Jose CR", 9.9281, -84.0907, "America/Costa_Rica"),
    ("Panama City", 8.9824, -79.5199, "America/Panama"),
    ("Caracas", 10.4806, -66.9036, "America/Caracas"),
    ("Havana CU (Cuba's own DST rule)", 23.1136, -82.3666, "UNKNOWN_RULE"),
    ("Nuuk GL (EU-style DST)", 64.1814, -51.6941, "?any"),
    # where the clock changes: must be BORDER
    ("Arizona/California line, Colorado River at Blythe", 33.6100, -114.5300, "BORDER"),
    ("Arizona Strip/Utah line near Kanab", 37.0000, -112.5300, "BORDER"),
    ("Chetumal MX / Belize (Hondo River)", 18.4900, -88.3000, "BORDER"),
    ("Saskatchewan/Manitoba line, 52 N", 52.0000, -101.5400, "BORDER"),
    ("Lloydminster AB/SK (one clock both sides since 2026)", 53.2807, -110.0050, "=Central, no DST"),
    ("Alabama/Georgia line, 33.5 N", 33.5000, -85.3060, "BORDER"),
    ("Phenix City AL (keeps Columbus GA's Eastern time; 2-3 cells from the line)", 32.4710, -85.0008, "?any"),
    ("Ohio/Indiana line at Union City (both Eastern: no border)", 40.2000, -84.8100, "?zone"),
    # sea, outside the region, out of range
    ("Pacific, 40 N 140 W", 40.0000, -140.0000, "WATER"),
    ("Atlantic, 30 N 60 W", 30.0000, -60.0000, "WATER"),
    ("Gulf of Mexico, 25 N 90 W", 25.0000, -90.0000, "WATER"),
    ("Bogota (south of 7 N)", 4.7110, -74.0721, "OUTSIDE"),
    ("London", 51.5074, -0.1278, "OUTSIDE"),
    ("Sydney", -33.8688, 151.2093, "OUTSIDE"),
    ("Resolute NU (north of 72 N)", 74.6973, -94.8297, "OUTSIDE"),
    ("Mid-Atlantic, 40 N 45 W (east of 50 W)", 40.0000, -45.0000, "OUTSIDE"),
]


# ---- writers -----------------------------------------------------------------------------

def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_c_header(path, grid, table, rows, runs, sha, release, tzver, attribution):
    out = []
    out.append("/*\n * GENERATED - do not edit. Made by tools/gen_tz_grid.py from timezone-boundary-builder\n"
               " * %s (combined.json) and tzdata %s.\n *\n" % (release, tzver))
    out.append(" * " + attribution.replace(". ", ".\n * ") + "\n *\n")
    out.append(" * Grid %d rows x %d columns of %.2f deg from %.1f N, %.1f E; %d runs. Included ONLY by\n"
               " * WiPhone/tz_grid.cpp (which documents the encoding). The data is ODbL 1.0, not GPL:\n"
               " * see THIRD-PARTY-NOTICES.md.\n */\n" % (grid.rows, grid.cols, grid.res, LAT0_E7 / E7,
                                                         LON0_E7 / E7, len(runs)))
    out.append("#ifndef TZ_GRID_DATA_H\n#define TZ_GRID_DATA_H\n\n#include <stdint.h>\n#include \"tz_grid.h\"\n\n")
    out.append("#define TZGRID_RELEASE %s\n#define TZGRID_TZDATA %s\n#define TZGRID_SHA256 %s\n"
               % (c_str(release), c_str(tzver), c_str(sha)))
    out.append("#define TZGRID_LAT0_E7 %d\n#define TZGRID_LON0_E7 (%d)\n#define TZGRID_RES_E7 %d\n"
               "#define TZGRID_ROWS %d\n#define TZGRID_COLS %d\n#define TZGRID_NRUNS %d\n"
               "#define TZGRID_NZONES %d\n#define TZGRID_CODE_WATER %d\n#define TZGRID_CODE_BORDER %d\n"
               "#define TZGRID_VALID_FROM %d\n#define TZGRID_VALID_TO %d\n\n"
               % (LAT0_E7, LON0_E7, grid.res_e7, grid.rows, grid.cols, len(runs), len(table),
                  CODE_WATER, CODE_BORDER, VALID_FROM, VALID_TO))
    out.append("static const char TZGRID_ATTRIBUTION_TEXT[] = %s;\n\n" % c_str(attribution))
    out.append("static const TzZone TZGRID_ZONES[TZGRID_NZONES] = {\n")
    out.append("  // display, IANA, standard abbr, daylight abbr, standard offset (min), rule, since (UTC s)\n")
    for z in table:
        out.append("  { %s, %s, %s, %s, %d, %s, %dLL },\n" % (
            c_str(z["display"]), c_str(z["iana"]), c_str(z["abbr_std"]), c_str(z["abbr_dst"]),
            z["std"], "TZ_RULE_" + RULE_NAMES[z["rule"]], z["since"]))
    out.append("};\n\n")
    out.append("static const uint32_t TZGRID_ROW_START[TZGRID_ROWS + 1] = {\n")
    for i in range(0, len(rows), 12):
        out.append("  " + ",".join("%d" % x for x in rows[i:i + 12]) + ",\n")
    out.append("};\n\n")
    out.append("static const uint16_t TZGRID_RUNS[TZGRID_NRUNS] = {\n")
    for i in range(0, len(runs), 16):
        out.append("  " + ",".join("0x%04x" % x for x in runs[i:i + 16]) + ",\n")
    out.append("};\n\n#endif // TZ_GRID_DATA_H\n")
    with open(path, "w") as f:
        f.write("".join(out))


def write_blob(path, grid, table, rows, runs, sha, release, tzver, attribution):
    zones = table_lines(table).encode("utf-8")
    attr = attribution.encode("utf-8")
    head = struct.pack("<8sHHiiiHHHHHHHIII16s16s32s",
                       b"TZGRID1\0", 1, 0, LAT0_E7, LON0_E7, grid.res_e7, grid.rows, grid.cols,
                       len(table), CODE_WATER, CODE_BORDER, VALID_FROM, VALID_TO,
                       len(runs), len(zones), len(attr),
                       release.encode()[:16], tzver.encode()[:16], bytes.fromhex(sha))
    body = zones + attr + struct.pack("<%dI" % len(rows), *rows) + struct.pack("<%dH" % len(runs), *runs)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(head + body)
    return len(head) + len(body)


def write_vectors(json_path, h_path, vec):
    with open(json_path, "w") as f:
        json.dump(vec, f, indent=1, sort_keys=True)
        f.write("\n")
    o = []
    o.append("/*\n * GENERATED - do not edit. Made by tools/gen_tz_grid.py; the same data as\n"
             " * tests/vectors_tzgrid.json (COVEY's tests/fixtures/vectors_tzgrid.json).\n"
             " * Expected offsets and transitions come from zoneinfo (tzdata %s), not from the\n"
             " * firmware's rule code. Lookups come from the generator's own grid.\n */\n" % vec["tzdata"])
    o.append("#ifndef VECTORS_TZGRID_H\n#define VECTORS_TZGRID_H\n\n#include <stdint.h>\n\n")
    o.append("#define TZV_SHA256 %s\n#define TZV_NZONES %d\n\n" % (c_str(vec["sha256"]), len(vec["zones"])))
    o.append("struct TzvZone { const char* iana; const char* display; int std; int rule; int64_t since;\n"
             "                 const char* abbrStd; const char* abbrDst; };\n")
    o.append("static const TzvZone TZV_ZONES[] = {\n")
    for z in vec["zones"]:
        o.append("  { %s, %s, %d, %d, %dLL, %s, %s },\n" % (c_str(z["iana"]), c_str(z["display"]), z["std"],
                                                           z["rule"], z["since"], c_str(z["abbr_std"]),
                                                           c_str(z["abbr_dst"])))
    o.append("};\n\n")
    o.append("/* answer: zone index, -1 BORDER, -2 NOT_COVERED; why: TzWhy. iana = the answer's zone\n"
             " * (\"\" if none); tzid = the polygon at the point (\"\" in no zone); ianaExact = the cell\n"
             " * holds no IANA line, so iana == tzid; jan/jul = zoneinfo's offset of tzid on\n"
             " * 2027-01-15 and 2027-07-15 12:00 UTC (-32768 if no tzid or it is unknown). */\n")
    o.append("struct TzvLookup { const char* name; int32_t lat, lon; int answer; int why; const char* iana;\n"
             "                   const char* tzid; int ianaExact; int jan, jul; };\n")
    o.append("static const TzvLookup TZV_NAMED[] = {\n")
    for v in vec["named"]:
        o.append("  { %s, %d, %d, %d, %d, %s, %s, %d, %d, %d },\n" % (
            c_str(v["name"]), v["lat_e7"], v["lon_e7"], v["answer"], v["why"], c_str(v["iana"] or ""),
            c_str(v["tzid"] or ""), 1 if v["iana_exact"] else 0,
            -32768 if v["jan"] is None else v["jan"], -32768 if v["jul"] is None else v["jul"]))
    o.append("};\n\n")
    o.append("struct TzvPoint { int32_t lat, lon; int16_t answer; uint8_t why; };\n")
    o.append("static const TzvPoint TZV_POINTS[] = {\n")
    for p in vec["points"]:
        o.append("  {%d,%d,%d,%d},\n" % tuple(p))
    o.append("};\n\n")
    o.append("/* minutes = zoneinfo's UTC offset of iana at utc (-32768 = UNKNOWN rule: no offset). */\n")
    o.append("struct TzvOffset { const char* iana; int64_t utc; int minutes; };\n")
    o.append("static const TzvOffset TZV_OFFSETS[] = {\n")
    for r in vec["offsets"]:
        o.append("  { %s, %dLL, %d },\n" % (c_str(r[0]), r[1], -32768 if r[2] is None else r[2]))
    o.append("};\n\n")
    o.append("/* next = the first instant > utc where zoneinfo's offset changes (0 = none before 2036),\n"
             " * after = the offset from then. */\n")
    o.append("struct TzvNext { const char* iana; int64_t utc; int64_t next; int after; };\n")
    o.append("static const TzvNext TZV_NEXT[] = {\n")
    for r in vec["next"]:
        o.append("  { %s, %dLL, %dLL, %d },\n" % (c_str(r[0]), r[1], r[2], r[3]))
    o.append("};\n\n#endif // VECTORS_TZGRID_H\n")
    with open(h_path, "w") as f:
        f.write("".join(o))


# ---- main --------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("combined", help="timezone-boundary-builder combined.json (named zones, no oceans)")
    ap.add_argument("--release", required=True, help="the timezone-boundary-builder release, e.g. 2026d")
    ap.add_argument("--header", default=os.path.join(ROOT, "WiPhone", "tz_grid_data.h"))
    ap.add_argument("--vectors", default=os.path.join(ROOT, "tests", "vectors_tzgrid.json"))
    ap.add_argument("--vectors-h", default=os.path.join(ROOT, "tests", "vectors_tzgrid.h"))
    ap.add_argument("--covey", help="covey-ui directory: writes covey_ui/data/tzgrid.bin and "
                                    "tests/fixtures/vectors_tzgrid.json there")
    ap.add_argument("--blob", help="write the binary grid here (default with --covey: its data dir)")
    ap.add_argument("--sizes", action="store_true", help="also report the encoded size at 0.1/0.05/0.025 deg")
    args = ap.parse_args()

    tzver = tzdata_version()
    attribution = ATTRIBUTION_FMT.format(release=args.release)
    grid = Grid()
    print("grid: %d x %d cells of %.3f deg, lat %.1f..%.1f, lon %.1f..%.1f (172 E..50 W)" % (
        grid.rows, grid.cols, grid.res, LAT0_E7 / E7, LAT1_E7 / E7, LON0_E7 / E7, LON1_E7 / E7))
    zones = load_zones(args.combined, grid)
    print("zones touching the region: %d" % len(zones))

    info = {}
    for tz in sorted(zones):
        info[tz] = classify_zone(tz)
    unknown = sorted(tz for tz in zones if info[tz]["rule"] == RULE_UNKNOWN)
    print("rules verified over %d-%d against tzdata %s: %d NONE, %d USCAN, %d UNKNOWN %s" % (
        VALID_FROM, VALID_TO, tzver,
        sum(1 for tz in zones if info[tz]["rule"] == RULE_NONE),
        sum(1 for tz in zones if info[tz]["rule"] == RULE_USCAN), len(unknown),
        ["%s (%s)" % (tz, info[tz]["why"]) for tz in unknown]))

    b = build(grid, zones, info)
    value, table = b["value"], b["table"]
    rows, runs = encode(value)
    for r in range(grid.rows):
        assert decode_row(rows, runs, r, grid.cols) == [int(x) for x in value[r]]
    sha = content_hash(grid, table, rows, runs)

    n_border = int((value == CODE_BORDER).sum())
    n_water = int((value == CODE_WATER).sum())
    n_zone = value.size - n_border - n_water
    print("cells: %d zone, %d border, %d water; class overlaps %d; centre probes %d" % (
        n_zone, n_border, n_water, b["overlaps"], b["fixups"]))
    print("classes: %d (%s)" % (len(b["class_keys"]), ", ".join(
        "UNKNOWN %s" % k[1] if k[0] == "UNKNOWN" else display_name("", k[0], k[1]) for k in b["class_keys"])))
    per_row = [rows[i + 1] - rows[i] for i in range(grid.rows)]
    # once linked on the ESP32: a 32-byte TzZone per zone + its four strings (NUL-terminated)
    table_bytes = sum(32 + len(z["display"]) + len(z["iana"]) + len(z["abbr_std"]) + len(z["abbr_dst"]) + 4
                      for z in table)
    print("table: %d zones (%d NONE, %d USCAN, %d UNKNOWN)" % (
        len(table), sum(1 for z in table if z["rule"] == RULE_NONE),
        sum(1 for z in table if z["rule"] == RULE_USCAN), sum(1 for z in table if z["rule"] == RULE_UNKNOWN)))
    print("runs: %d (max %d in a row, mean %.1f)" % (len(runs), max(per_row), len(runs) / grid.rows))
    print("WiPhone flash: runs %d B + row index %d B + zone table ~%d B + attribution %d B = ~%d B "
          "(+ ~2 KB of code)" % (2 * len(runs), 4 * len(rows), table_bytes, len(attribution) + 1,
                                 2 * len(runs) + 4 * len(rows) + table_bytes + len(attribution) + 1))

    # ---- self-check: random points against the polygons ----------------------------------
    rng = np.random.default_rng(20261010)
    N = 400000
    py = rng.uniform(LAT0_E7 / E7, LAT1_E7 / E7, N)
    px = rng.uniform(LON0_E7 / E7, LON1_E7 / E7, N)
    r_ = ((py - grid.lat0) / grid.res).astype(np.int64)
    c_ = ((px - grid.lon0) / grid.res).astype(np.int64)
    ok = (r_ >= 0) & (r_ < grid.rows) & (c_ >= 0) & (c_ < grid.cols)
    py, px, r_, c_ = py[ok], px[ok], r_[ok], c_[ok]
    v = value[r_, c_]
    exact = which_class(px, py, b["class_geoms"])
    zone_cls = np.array([b["zone_class"][z["iana"]] for z in table] + [-9] * (128 - len(table)))
    is_zone = v < CODE_WATER
    bad_zone = int((is_zone & (exact != zone_cls[np.minimum(v, 127)])).sum())
    bad_water = int(((v == CODE_WATER) & (exact != -1)).sum())
    jx = px + rng.uniform(-0.999, 0.999, len(px)) * grid.res
    jy = py + rng.uniform(-0.999, 0.999, len(py)) * grid.res
    exact_j = which_class(jx, jy, b["class_geoms"])
    bad_margin = int((is_zone & (exact_j != zone_cls[np.minimum(v, 127)])).sum())
    print("self-check, %d random points: %d answered a zone, %d water, %d border; wrong class %d, "
          "water in a zone %d, wrong class one cell away %d" % (
              len(v), int(is_zone.sum()), int((v == CODE_WATER).sum()), int((v == CODE_BORDER).sum()),
              bad_zone, bad_water, bad_margin))
    if bad_zone or bad_water or bad_margin:
        sys.exit("SELF-CHECK FAILED")

    # ---- named vectors ----------------------------------------------------------------------
    names_all = b["names"]
    zone_geoms = [zones[tz] for tz in names_all]
    for g in zone_geoms:
        shapely.prepare(g)

    def tzid_at(lat, lon):
        x, y = deg_to_grid_lon(lon), lat
        for tz, g in zip(names_all, zone_geoms):
            if shapely.contains_xy(g, x, y):
                return tz
        return None

    def jan_jul(tz):
        if tz is None or info.get(tz, {}).get("rule", RULE_UNKNOWN) == RULE_UNKNOWN:
            return None, None
        zi = zoneinfo.ZoneInfo(tz)
        return (zi_offset(zi, calendar.timegm((2027, 1, 15, 12, 0, 0))),
                zi_offset(zi, calendar.timegm((2027, 7, 15, 12, 0, 0))))

    named, problems = [], []
    for name, lat, lon, expect in NAMED:
        la, lo = e7(lat), e7(lon)
        ans, why = ref_lookup(value, table, grid, la, lo)
        tz = tzid_at(lat, lon) if -90 <= lat <= 90 else None
        iana = table[ans]["iana"] if ans >= 0 else None
        if ans == ANS_NOT_COVERED and why == WHY_UNKNOWN_RULE:
            dy, dx = la - LAT0_E7, (lo - 360 * E7 if lo - LON0_E7 >= 360 * E7 else lo) - LON0_E7
            iana = table[int(value[dy // grid.res_e7, dx // grid.res_e7])]["iana"]
        exact_flag = False
        if ans >= 0:
            dy, dx = la - LAT0_E7, (lo - 360 * E7 if lo - LON0_E7 >= 360 * E7 else lo) - LON0_E7
            exact_flag = not bool(b["crossed_iana"][dy // grid.res_e7, dx // grid.res_e7])
        jan, jul = jan_jul(tz)
        tag = "%s (%.2f, %.2f)" % (name, lat, lon)
        if expect in ("BORDER", "WATER", "OUTSIDE", "UNKNOWN_RULE"):
            want_why = WHY_NAMES.index(expect)
            if why != want_why:
                problems.append("%s: %s, want %s" % (tag, WHY_NAMES[why], expect))
        elif expect == "?any":
            pass
        elif expect.startswith("="):
            if ans < 0 or table[ans]["display"] != expect[1:]:
                problems.append("%s: %s, want a zone of the %s clock" % (
                    tag, table[ans]["display"] if ans >= 0 else WHY_NAMES[why], expect[1:]))
        else:
            if ans < 0:
                problems.append("%s: %s, want a zone" % (tag, WHY_NAMES[why]))
            elif expect != "?zone":
                if tz != expect:
                    problems.append("%s: the polygon there is %s, want %s" % (tag, tz, expect))
                if b["zone_class"][iana] != b["zone_class"][expect]:
                    problems.append("%s: answers %s, a different clock from %s" % (tag, iana, expect))
        if ans >= 0 and tz is not None and b["zone_class"][iana] != b["zone_class"][tz]:
            problems.append("%s: answers %s but the polygon is %s (different clock)" % (tag, iana, tz))
        if exact_flag and iana != tz:
            problems.append("%s: IANA-exact cell answers %s, polygon %s" % (tag, iana, tz))
        named.append(dict(name=name, lat_e7=la, lon_e7=lo, answer=ans, why=why, iana=iana, tzid=tz,
                          iana_exact=exact_flag, jan=jan, jul=jul))
    # out-of-range inputs and the grid's own edges
    edge = [("lat above 90 (bad input)", 90 * E7 + 1, 0),
            ("lon below -180 (bad input)", 0, -180 * E7 - 1),
            ("lat = INT32_MIN (bad input)", -2147483648, 0),
            ("first row, first column (172 E, 7 N)", LAT0_E7, LON0_E7 + 360 * E7),
            ("one unit south of the grid", LAT0_E7 - 1, -100 * E7),
            ("one unit west of 172 E", LAT0_E7 + 20 * E7, LON0_E7 + 360 * E7 - 1),
            ("last row (72 N less one unit)", LAT1_E7 - 1, -100 * E7),
            ("72 N exactly (outside)", LAT1_E7, -100 * E7),
            ("50 W exactly (outside)", 40 * E7, LON1_E7),
            ("180 E (= 180 W), Aleutians latitude", 52 * E7, 180 * E7),
            ("180 W, Aleutians latitude", 52 * E7, -180 * E7)]
    for name, la, lo in edge:
        ans, why = ref_lookup(value, table, grid, la, lo)
        named.append(dict(name=name, lat_e7=la, lon_e7=lo, answer=ans, why=why,
                          iana=table[ans]["iana"] if ans >= 0 else None, tzid=None, iana_exact=False,
                          jan=None, jul=None))
    for p in problems:
        print("NAMED VECTOR: " + p)
    if problems:
        sys.exit("%d named vectors are not what they say" % len(problems))
    by_why = {}
    for v_ in named:
        by_why[WHY_NAMES[v_["why"]]] = by_why.get(WHY_NAMES[v_["why"]], 0) + 1
    print("named vectors: %d %s" % (len(named), by_why))

    # ---- random + near-boundary points (decoder agreement) -------------------------------
    pts = []
    for _ in range(3000):
        la = int(rng.integers(LAT0_E7, LAT1_E7))
        lo = int(rng.integers(LON0_E7, LON1_E7))
        if lo < -180 * E7:
            lo += 360 * E7
        ans, why = ref_lookup(value, table, grid, la, lo)
        pts.append([la, lo, ans, why])
    # near class boundaries: a cell next to a BORDER cell, so both answers get exercised
    edge_cells = np.argwhere((value != CODE_BORDER) & np.pad(value == CODE_BORDER, 1)[1:-1, 2:])
    pick = rng.choice(len(edge_cells), size=min(1500, len(edge_cells)), replace=False)
    for k in sorted(pick):
        r, c = edge_cells[k]
        for dc in (0, 1):
            la = LAT0_E7 + int(r) * grid.res_e7 + int(rng.integers(0, grid.res_e7))
            lo = LON0_E7 + (int(c) + dc) * grid.res_e7 + int(rng.integers(0, grid.res_e7))
            if lo < -180 * E7:
                lo += 360 * E7
            ans, why = ref_lookup(value, table, grid, la, lo)
            pts.append([la, lo, ans, why])

    # ---- offsets and transitions (expected values from zoneinfo) ------------------------
    offsets, nexts = [], []
    tv0 = calendar.timegm((VEC_FROM, 1, 1, 0, 0, 0))
    tv1 = calendar.timegm((VALID_TO + 1, 1, 1, 0, 0, 0))
    for z in table:
        tz = z["iana"]
        if z["rule"] == RULE_UNKNOWN:
            for t in (calendar.timegm((2026, 1, 15, 12, 0, 0)), calendar.timegm((2026, 7, 15, 12, 0, 0))):
                offsets.append([tz, t, None])
            nexts.append([tz, calendar.timegm((2026, 1, 1, 0, 0, 0)), 0, 0])
            continue
        zi = zoneinfo.ZoneInfo(tz)
        std = z["std"]
        instants = set()
        for y in range(VEC_FROM, VEC_TO + 1):
            for tr in us_can_transitions(y, std):
                instants.update((tr - 1, tr, tr + 1))
            instants.update((calendar.timegm((y, 1, 1, 0, 0, 0)), calendar.timegm((y, 7, 1, 0, 0, 0))))
        since = z["since"]
        if since:
            instants.update((since - 1, since, since + 1))
        for t in sorted(instants):
            # before `since` the devices refuse: the model was not verified there
            offsets.append([tz, t, None if since and t < since else zi_offset(zi, t)])
        actual = zi_transitions(zi, tv0, tv1)
        probes = set()
        for y in range(VEC_FROM, VEC_TO + 1):
            probes.update((calendar.timegm((y, 1, 1, 0, 0, 0)), calendar.timegm((y, 6, 1, 0, 0, 0))))
        for (t, _) in actual:
            if t < calendar.timegm((VEC_TO + 1, 1, 1, 0, 0, 0)):
                probes.update((t - 1, t, t + 1))
        for p in sorted(probes):
            if since and p < since:
                nexts.append([tz, p, 0, 0])     # refused before `since`
                continue
            nxt = next(((t, o) for (t, o) in actual if t > p), None)
            if nxt is None and z["rule"] == RULE_USCAN:
                continue    # past the verified window: not a vector
            nexts.append([tz, p, nxt[0] if nxt else 0, nxt[1] if nxt else 0])

    vec = dict(
        comment=("Made by tools/gen_tz_grid.py (WiPhone repo). Offsets/transitions: zoneinfo. "
                 "Lookups: the generator's grid. answer >= 0 zone index, -1 BORDER, -2 NOT_COVERED; "
                 "why: " + ", ".join("%d %s" % (i, n) for i, n in enumerate(WHY_NAMES)) + "."),
        release=args.release, tzdata=tzver, sha256=sha, attribution=attribution,
        grid=dict(lat0_e7=LAT0_E7, lon0_e7=LON0_E7, res_e7=grid.res_e7, rows=grid.rows, cols=grid.cols,
                  nruns=len(runs), valid_from=VALID_FROM, valid_to=VALID_TO),
        zones=table, named=named, points=pts, offsets=offsets, next=nexts)

    write_c_header(args.header, grid, table, rows, runs, sha, args.release, tzver, attribution)
    write_vectors(args.vectors, args.vectors_h, vec)
    print("wrote %s (%d B of source)" % (os.path.relpath(args.header, ROOT), os.path.getsize(args.header)))
    print("wrote %s, %s: %d named, %d points, %d offsets, %d next" % (
        os.path.relpath(args.vectors, ROOT), os.path.relpath(args.vectors_h, ROOT),
        len(named), len(pts), len(offsets), len(nexts)))
    blob = args.blob
    if args.covey:
        blob = blob or os.path.join(args.covey, "covey_ui", "data", "tzgrid.bin")
        fx = os.path.join(args.covey, "tests", "fixtures", "vectors_tzgrid.json")
        with open(args.vectors) as src, open(fx, "w") as dst:
            dst.write(src.read())
        print("wrote %s" % fx)
    if blob:
        n = write_blob(blob, grid, table, rows, runs, sha, args.release, tzver, attribution)
        print("wrote %s (%d B)" % (blob, n))
    print("sha256 %s" % sha)

    if args.sizes:
        for res_e7 in (1000000, 500000, 250000):
            g2 = Grid(res_e7)
            z2 = load_zones(args.combined, g2)
            b2 = build(g2, z2, info)
            r2, u2 = encode(b2["value"])
            nb = int((b2["value"] == CODE_BORDER).sum())
            print("  res %.3f deg: %d x %d, %d runs = %d B + index %d B; border cells %d = %.0f sq deg" % (
                res_e7 / E7, g2.rows, g2.cols, len(u2), 2 * len(u2), 4 * len(r2), nb, nb * g2.res ** 2))


if __name__ == "__main__":
    main()
