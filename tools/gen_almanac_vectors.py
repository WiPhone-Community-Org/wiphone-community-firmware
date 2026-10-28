#!/usr/bin/env python3
"""Generate the Almanac's ground truth: tests/vectors_almanac.json + tests/vectors_almanac.h.

The Almanac (WiPhone/astro.cpp, wmm.cpp, geo_grid.cpp on the phone; covey_ui/almanac.py on
COVEY) computes sun and moon events, moon phases, seasons, magnetic declination and UTM/MGRS
grid references OFFLINE. None of it can be checked by looking at it, and a wrong moonrise or a
wrong declination looks exactly as plausible as a right one. So both implementations are tested
against what independent, established references say for the same inputs:

  * sun + moon: PyEphem (libastro, the XEphem engine), pressure 0 and the standard -0:34
    horizon, upper limb for rise/set - the almanac definition. Civil twilight: the centre at -6.
  * phases and seasons: PyEphem's next_new_moon / next_first_quarter_moon / ... / next_*_equinox.
  * declination: NOAA's OWN published WMM2025 test values (WMM2025_TestValues.txt), copied in
    verbatim - not recomputed by anything of ours.
  * UTM: the `utm` package; MGRS: the `mgrs` package (GeoTrans-derived), 1 m precision.

    pip install --user ephem utm mgrs
    python3 tools/gen_almanac_vectors.py --wmm-tests <WMM2025_TestValues.txt>

The JSON is the canonical copy (COVEY's tests read it: copy it to covey-ui/tests/fixtures/);
the header is the same data as C arrays for tests/test_almanac.cpp.

A day is a LOCAL day: the window [t0, t0 + 86400) where t0 is local midnight as UNIX seconds
under the place's fixed offset. Each event is the FIRST of its kind in that window, 0 = none
(the moon skips a rise about once a month; the sun at 71 N skips everything for weeks).
"""
import argparse
import json
import math
import os
import sys

try:
    import ephem
    import utm
    import mgrs
except ImportError as e:
    sys.exit("need ephem, utm, mgrs: pip install --user ephem utm mgrs (%s)" % e)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EPOCH = ephem.Date("1970/1/1 00:00:00")

# name, lat, lon, fixed UTC offset in hours (standard-ish; the WINDOW is what is tested)
PLACES = [
    ("north-bend", 47.4957, -121.7868, -7),     # the hunt
    ("seattle", 47.6062, -122.3321, -8),
    ("fairbanks", 64.8378, -147.7164, -9),     # the moon goes circumpolar near the standstill
    ("utqiagvik", 71.2906, -156.7886, -9),     # polar night and midnight sun
    ("quito", -0.1807, -78.4678, -5),          # the equator
    ("hobart", -42.8821, 147.3272, 10),        # the south, east of Greenwich
]


def unix(d):
    return int(round((float(d) - float(EPOCH)) * 86400.0))


def edate(u):
    return ephem.Date(EPOCH + u / 86400.0)


def observer(lat, lon, horizon="-0:34"):
    o = ephem.Observer()
    o.lat, o.lon = str(lat), str(lon)
    o.elevation = 0
    o.pressure = 0          # no refraction model: the -0:34 horizon IS the standard refraction
    o.horizon = horizon
    return o


def first_in(fn, t0, obs=None):
    """The first event fn() finds at or after t0, if it is inside the local day; else 0.

    PyEphem raises NeverUpError / AlwaysUpError when the body is circumpolar or never up AT THE
    SEARCH'S START (it judges by the declination there), even if the moon's declination changes
    enough to rise later that day - at Utqiagvik on 2026-08-26 a search from t0 raised while one
    started an hour before the event found it (astro.cpp's builder caught it). So on a raise,
    the search is retried from each later hour of the window; the first hit inside it wins."""
    starts = [t0] + ([t0 + h * 3600 for h in range(1, 24)] if obs is not None else [])
    for s in starts:
        if obs is not None:
            obs.date = edate(s)
        try:
            t = unix(fn())
        except (ephem.NeverUpError, ephem.AlwaysUpError, ephem.CircumpolarError):
            continue
        return t if t0 <= t < t0 + 86400 else 0
    return 0


def sun_day(lat, lon, t0):
    o = observer(lat, lon)
    s = ephem.Sun()
    rise = first_in(lambda: o.next_rising(s), t0, o)
    sset = first_in(lambda: o.next_setting(s), t0, o)
    noon = first_in(lambda: o.next_transit(s), t0, o)
    c = observer(lat, lon, "-6")
    dawn = first_in(lambda: c.next_rising(s, use_center=True), t0, c)
    dusk = first_in(lambda: c.next_setting(s, use_center=True), t0, c)
    return dict(dawn=dawn, rise=rise, noon=noon, set=sset, dusk=dusk)


def moon_day(lat, lon, t0):
    o = observer(lat, lon)
    m = ephem.Moon()
    out = {}
    for k, fn in (("rise", o.next_rising), ("set", o.next_setting),
                  ("transit", o.next_transit), ("under", o.next_antitransit)):
        out[k] = first_in(lambda: fn(m), t0, o)
    return out


def body_now(body, lat, lon, t):
    o = observer(lat, lon)
    o.date = edate(t)
    body.compute(o)
    return math.degrees(body.alt), math.degrees(body.az)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--wmm-tests", required=True, help="NOAA's WMM2025_TestValues.txt")
    ap.add_argument("--json", default=os.path.join(ROOT, "tests", "vectors_almanac.json"))
    ap.add_argument("--header", default=os.path.join(ROOT, "tests", "vectors_almanac.h"))
    a = ap.parse_args()

    start = unix(ephem.Date("2026/1/1"))
    end = unix(ephem.Date("2029/1/1"))

    days = []
    for pi, (name, lat, lon, tz) in enumerate(PLACES):
        # 11-day steps, offset per place so the places do not all sample the same moon phase
        u = start + pi * 86400 * 2
        while u < end:
            t0 = u - tz * 3600          # local midnight of the UTC date u, as UNIX seconds
            d = dict(place=name, lat=lat, lon=lon, t0=t0)
            d["sun"] = sun_day(lat, lon, t0)
            d["moon"] = moon_day(lat, lon, t0)
            days.append(d)
            u += 11 * 86400

    nows = []
    t = start + 1234
    i = 0
    while t < end:
        name, lat, lon, tz = PLACES[i % len(PLACES)]
        m = ephem.Moon()
        malt, maz = body_now(m, lat, lon, t)
        salt, saz = body_now(ephem.Sun(), lat, lon, t)
        g = ephem.Moon(edate(t))           # geocentric, for phase
        nows.append(dict(place=name, lat=lat, lon=lon, t=t, moon_alt=malt, moon_az=maz,
                         sun_alt=salt, sun_az=saz, illum=float(g.moon_phase),
                         elong=math.degrees(float(g.elong))))
        t += int(37.3 * 3600)
        i += 1

    phases = []
    u = start
    while u < end:
        d = edate(u)
        phases.append(dict(frm=u, new=unix(ephem.next_new_moon(d)),
                           first=unix(ephem.next_first_quarter_moon(d)),
                           full=unix(ephem.next_full_moon(d)),
                           last=unix(ephem.next_last_quarter_moon(d))))
        u += 23 * 86400

    seasons = []
    u = start
    while u < end:
        d = edate(u)
        seasons.append(dict(frm=u, march=unix(ephem.next_vernal_equinox(d)),
                            june=unix(ephem.next_summer_solstice(d)),
                            sept=unix(ephem.next_autumnal_equinox(d)),
                            dec=unix(ephem.next_winter_solstice(d))))
        u += 61 * 86400

    wmm = []
    for line in open(a.wmm_tests):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = [float(x) for x in line.split()]
        wmm.append(dict(year=f[0], alt_km=f[1], lat=f[2], lon=f[3], decl=f[4], incl=f[5],
                        h=f[6], x=f[7], y=f[8], z=f[9], f=f[10]))
    if len(wmm) < 10:
        sys.exit("expected NOAA's WMM test rows, got %d" % len(wmm))

    M = mgrs.MGRS()
    grid = []
    pts = [(47.49643, -121.79), (47.6062, -122.3321), (0.0, 0.0), (0.0000001, -0.0000001),
           (-0.0000001, 0.0000001), (-33.8688, 151.2093), (60.0, 5.0), (56.0, 3.0),
           (63.9, 2.99), (72.0, 8.9), (72.0, 9.1), (78.0, 15.0), (78.0, 20.9), (78.0, 21.1),
           (83.99, 40.0), (-79.99, -179.99), (-45.0, -180.0), (45.0, 179.9999), (29.9, -6.0),
           (47.0, -120.0), (47.0, -120.0000001), (64.8378, -147.7164), (-42.8821, 147.3272),
           (19.8968, -155.5828), (35.6762, 139.6503), (51.4779, -0.0015), (51.4779, 0.0015)]
    lat = -79.5
    k = 0
    while lat < 84.0:
        lon = -179.3 + (k * 47.77) % 358.6
        pts.append((round(lat, 5), round(lon, 5)))
        lat += 3.71
        k += 1
    for lat, lon in pts:
        e, n, zone, band = utm.from_latlon(lat, lon)
        grid.append(dict(lat=lat, lon=lon, zone=int(zone), band=band, e=float(e), n=float(n),
                         mgrs=M.toMGRS(lat, lon, MGRSPrecision=5).decode()
                         if isinstance(M.toMGRS(lat, lon, MGRSPrecision=5), bytes)
                         else M.toMGRS(lat, lon, MGRSPrecision=5)))

    doc = dict(generator="tools/gen_almanac_vectors.py", ephem=ephem.__version__,
               note="t0/t/frm are UNIX seconds; 0 = no such event in [t0, t0+86400)",
               days=days, nows=nows, phases=phases, seasons=seasons, wmm=wmm, grid=grid)
    with open(a.json, "w") as f:
        json.dump(doc, f, indent=0, sort_keys=True)
        f.write("\n")

    h = ["/* GENERATED by tools/gen_almanac_vectors.py - do not edit. PyEphem %s, NOAA WMM2025"
         " test values, utm + mgrs. */" % ephem.__version__,
         "#pragma once", "#include <stdint.h>", "",
         "struct AlmDayVec { const char* place; double lat, lon; int64_t t0;"
         " int64_t dawn, rise, noon, set, dusk; int64_t mrise, mset, mtransit, munder; };",
         "struct AlmNowVec { const char* place; double lat, lon; int64_t t;"
         " double moonAlt, moonAz, sunAlt, sunAz, illum, elong; };",
         "struct AlmPhaseVec { int64_t from, next[4]; };      // new, first quarter, full, last quarter",
         "struct AlmSeasonVec { int64_t from, next[4]; };     // March, June, September, December",
         "struct AlmWmmVec { double year, altKm, lat, lon, decl, incl, h, x, y, z, f; };",
         "struct AlmGridVec { double lat, lon; int zone; char band; double e, n; const char* mgrs; };",
         ""]
    h.append("static const AlmDayVec ALM_DAYS[] = {")
    for d in days:
        s, m = d["sun"], d["moon"]
        h.append('  { "%s", %.6f, %.6f, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d },' % (
            d["place"], d["lat"], d["lon"], d["t0"], s["dawn"], s["rise"], s["noon"], s["set"],
            s["dusk"], m["rise"], m["set"], m["transit"], m["under"]))
    h.append("};")
    h.append("static const AlmNowVec ALM_NOWS[] = {")
    for n in nows:
        h.append('  { "%s", %.6f, %.6f, %d, %.5f, %.5f, %.5f, %.5f, %.6f, %.5f },' % (
            n["place"], n["lat"], n["lon"], n["t"], n["moon_alt"], n["moon_az"], n["sun_alt"],
            n["sun_az"], n["illum"], n["elong"]))
    h.append("};")
    h.append("static const AlmPhaseVec ALM_PHASES[] = {")
    for p in phases:
        h.append("  { %d, { %d, %d, %d, %d } }," % (p["frm"], p["new"], p["first"], p["full"],
                                                   p["last"]))
    h.append("};")
    h.append("static const AlmSeasonVec ALM_SEASONS[] = {")
    for p in seasons:
        h.append("  { %d, { %d, %d, %d, %d } }," % (p["frm"], p["march"], p["june"], p["sept"],
                                                   p["dec"]))
    h.append("};")
    h.append("static const AlmWmmVec ALM_WMM[] = {")
    for w in wmm:
        h.append("  { %.6f, %.3f, %.4f, %.4f, %.2f, %.2f, %.6f, %.6f, %.6f, %.6f, %.6f }," % (
            w["year"], w["alt_km"], w["lat"], w["lon"], w["decl"], w["incl"], w["h"], w["x"],
            w["y"], w["z"], w["f"]))
    h.append("};")
    h.append("static const AlmGridVec ALM_GRID[] = {")
    for g in grid:
        h.append('  { %.7f, %.7f, %d, \'%s\', %.4f, %.4f, "%s" },' % (
            g["lat"], g["lon"], g["zone"], g["band"], g["e"], g["n"], g["mgrs"]))
    h.append("};")
    with open(a.header, "w") as f:
        f.write("\n".join(h) + "\n")
    print("days=%d nows=%d phases=%d seasons=%d wmm=%d grid=%d -> %s, %s" % (
        len(days), len(nows), len(phases), len(seasons), len(wmm), len(grid), a.json, a.header))


if __name__ == "__main__":
    main()
