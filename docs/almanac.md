# The Almanac - sun, moon, solunar, date, position, altitude

One app on each device (WiPhone: **Menu > Almanac**; COVEY: the **Almanac** tile), the same
arithmetic on both, and elevation on both maps. Everything here is **offline**: pure math plus
files on the card. Nothing needs WiFi except fetching elevation tiles.

The phone's C++ (`WiPhone/astro.cpp`, `wmm.cpp`, `geo_grid.cpp`, `elev_tiles.cpp`) and COVEY's
Python (`covey_ui/almanac.py`, `covey_ui/elevation.py`) implement **the algorithms below, the
same way**, and both are tested against **one** file of independent ground truth:
`tests/vectors_almanac.json`, made by `tools/gen_almanac_vectors.py` from PyEphem (sun, moon,
phases, seasons), NOAA's own WMM2025 test values (declination), and the `utm` + `mgrs` packages.
A wrong moonrise looks exactly as plausible as a right one; only the vectors can tell.

## Conventions (both devices)

- An **instant** is UNIX seconds (int64). `JD = unix / 86400 + 2440587.5` (UT).
- **Two time scales.** Sidereal time runs on UT (`JD`). The sun and moon theories run on TT:
  `JDE = JD + DT / 86400` with **`DT = 69 s`** (TT - UT, a constant; measured 69.1-69.3 s through
  2020-2026, and a few seconds off moves the moon ~1" - nothing the screen shows).
- A **day** is the LOCAL day: `[t0, t0 + 86400)` where `t0` is local midnight as UNIX seconds
  under the device's current UTC offset. Each event is the **first of its kind in that window**;
  none = absent (the moon skips a rise about once a month; at 71 N the sun skips for weeks).
  ⚠ That is exact for the PHONE, whose clock is a fixed offset (no automatic DST). On a device
  with a real time zone (COVEY: `America/Los_Angeles`) a DST-change day is 23 or 25 hours, and a
  fixed 86,400 s window put a 23:17 PST moonrise on 1 Nov 2026 in NEITHER day and showed a moonrise
  on 8 Mar 2026 that belongs to the 9th (COVEY review, 2026-09-27). COVEY's screen therefore uses
  midnight-to-next-midnight: events past the next midnight are dropped on a 23-hour day, and on a
  25-hour day any kind still missing is filled from a second scan that ends at the next midnight
  (`covey_ui/almanac_screen.py _local_day`). The functions themselves keep the 86,400 s window.
- An event's instant is **the first whole second at or past the crossing** (the upper end of the
  final 1-second bisection bracket). One definition everywhere, so "previous at or before `t`" and
  "next strictly after `t`" are exact inverses (`prev(next(t)) == next(t)`).
- Angles in degrees at the API; azimuth from true north, clockwise, 0..360.
- **Latitude/longitude** in degrees, east and north positive. On the phone the fixed point is the
  mesh's 1e-7 degree int32 and it converts to double at the API.

## Sun position (NOAA / Meeus ch. 25, "low accuracy", ~0.01 deg)

```
T  = (JDE - 2451545) / 36525                                (TT - see Conventions)
L0 = 280.46646 + T*(36000.76983 + 0.0003032*T)            (mod 360)
M  = 357.52911 + T*(35999.05029 - 0.0001537*T)
e  = 0.016708634 - T*(0.000042037 + 0.0000001267*T)
C  = sin(M)*(1.914602 - T*(0.004817 + 0.000014*T)) + sin(2M)*(0.019993 - 0.000101*T) + sin(3M)*0.000289
true lon = L0 + C;  nu = M + C
R  = 1.000001018*(1 - e^2) / (1 + e*cos(nu))                (AU)
Om = 125.04 - 1934.136*T
lambda = true lon - 0.00569 - 0.00478*sin(Om)               (apparent)
eps0 = 23 + (26 + (21.448 - T*(46.815 + T*(0.00059 - T*0.001813)))/60)/60
eps  = eps0 + 0.00256*cos(Om)
RA  = atan2(cos(eps)*sin(lambda), cos(lambda));  dec = asin(sin(eps)*sin(lambda))
GMST = 280.46061837 + 360.98564736629*(JD - 2451545) + 0.000387933*Tu^2 - Tu^3/38710000
                                                            (JD and Tu = (JD - 2451545)/36525 in UT)
dpsi = -0.00478*sin(Om)                                     (the nutation inside lambda above)
LST = GMST + dpsi*cos(eps) + lon;  H = LST - RA             (APPARENT sidereal time: RA is of the
                                                             true equinox; the term is <= 0.0044 deg)
alt = asin(sin(lat)sin(dec) + cos(lat)cos(dec)cos(H))
az  = atan2(sin(H)cos(dec), cos(H)sin(lat)cos(dec) - sin(dec)cos(lat)) + 180
                                                            (Meeus's form times cos(dec) > 0: no tan)
semidiameter = 0.26656 / R  (deg)        (959.63")
```
No refraction in `alt` (that is what the event thresholds are for). The same `Om`, `dpsi`, `eps` and
LST serve the moon. Measured against PyEphem (706 samples): alt 0.010 deg, az 0.018 deg worst;
the seasons (which ride on `lambda` alone) 596 s worst.

## Moon position (Meeus ch. 47 - ELP-2000/82 truncated, ~10" in longitude), topocentric

**Changed from Schlyter's elements (the first draft of this spec), because they failed the
vectors:** 581 s on a first quarter (tolerance 300), 0.31 deg of azimuth under a high moon at Quito
(tolerance 0.2), a 222 s Arctic moonset (tolerance 120) - Schlyter is ~0.08 deg in elongation.
Meeus ch. 47 agrees with PyEphem's geocentric moon to 11" worst (4000 instants, 2022-2030);
against the vectors it is 81 s on a quarter, 0.009 deg of azimuth, 5 s on a moonrise (worst
cases, see the end).

`T = (JDE - 2451545) / 36525` (TT). Fundamental arguments, degrees (reduce mod 360):
```
L' = 218.3164477 + 481267.88123421 T - 0.0015786 T^2 + T^3/538841 - T^4/65194000
D  = 297.8501921 + 445267.1114034 T  - 0.0018819 T^2 + T^3/545868 - T^4/113065000
M  = 357.5291092 + 35999.0502909 T   - 0.0001536 T^2 + T^3/24490000
M' = 134.9633964 + 477198.8675055 T  + 0.0087414 T^2 + T^3/69699  - T^4/14712000
F  = 93.2720950  + 483202.0175233 T  - 0.0036539 T^2 - T^3/3526000 + T^4/863310000
A1 = 119.75 + 131.849 T     A2 = 53.09 + 479264.290 T     A3 = 313.45 + 481266.484 T
E  = 1 - 0.002516 T - 0.0000074 T^2
```
Series (Meeus tables 47.A and 47.B, 60 rows each, **copied into `astro.cpp` as `MOON_LR` and
`MOON_B` - port those arrays verbatim**): each row is integer multipliers `(d, m, m', f)` of
`(D, M, M', F)` and coefficients; a row's term is multiplied by `E` when `|m| = 1` and by `E^2`
when `|m| = 2`.
```
sum_l = sum( l * sin(arg) ) + 3958 sin A1 + 1962 sin(L' - F) + 318 sin A2           (1e-6 deg)
sum_r = sum( r * cos(arg) )                                                        (1e-3 km)
sum_b = sum( b * sin(arg) ) - 2235 sin L' + 382 sin A3 + 175 sin(A1 - F) + 175 sin(A1 + F)
        + 127 sin(L' - M') - 115 sin(L' + M')                                      (1e-6 deg)
lambda = L' + sum_l / 1e6   (geometric, of date)      beta = sum_b / 1e6
distance = 385000.56 + sum_r / 1000  km
```
Check: Meeus example 47.a (1992-04-12 0h TD) gives 133.162655, -3.229126, 368409.7 km - the
test asserts it. Apparent longitude `lambda + dpsi`; ecliptic to equatorial with the sun section's
true `eps`; LST as in the sun section (apparent sidereal time).

**Topocentric by vectors** (no formula that divides by `sin g`, which is zero on the equator):
the geocentric equatorial vector (Earth radii, `distance / 6378.14`) minus the observer's
`rho*(cos gclat cos LST, cos gclat sin LST, sin gclat)` with `gclat = lat - 0.1924 sin(2 lat)`,
`rho = 0.99833 + 0.00167 cos(2 lat)`. Topocentric distance `r'` (Earth radii) gives the
semidiameter `asin(0.272481 / r')`.

Elongation, illumination (geocentric): `lon_s` and `R` from the **sun section** (apparent; the
nutation cancels in the difference), `psi = acos(cos(beta) cos(lon_m - lon_s))` (Meeus 48.2);
sign = east of the sun (`lon_m - lon_s` in (0, 180)) = waxing. Phase angle
`i = atan2(Rs sin psi, Rm - Rs cos psi)` (Meeus 48.3) with `Rs = R * 149597870.7` and `Rm` the
moon's distance, both km; `illuminated = (1 + cos i) / 2`. The elongation searches below need only
`lambda`: the phone skips the distance and latitude series there (a third of the work).

## Events in a local day

Scan the window in **5-minute** steps (289 samples, `t0 .. t0 + 86400`); refine the first bracket
of each event by bisection to 1 s; keep it only if it is before `t0 + 86400`. The FIRST bracket
opens one second early, `(t0 - 1, t0 + 300]`: an event whose instant is exactly t0 is already past
its threshold at t0, and the previous day drops it (`t < end`), so without that second it belonged
to neither day (530 of 532 such events, astro review 2026-09-27). Hour angles wrap at
+-180 (a descending jump), so a bracket whose two samples differ by 90 deg or more never counts.

**How the phone evaluates it** (a cost choice, not a different answer): each body's theory (the
lunar series; the solar formulas) is evaluated every **3 h** - 9 times, `t0 .. t0 + 24 h` - and the
geocentric equatorial vector at any sample or bisection point is the quadratic through the three
nearest of those nodes (nearest node `j`, clamped to 1..7, `u = (t - t_j) / 3 h`:
`f0 + u (f+ - f-)/2 + u^2 (f+ - 2 f0 + f-)/2`). What changes fast is exact at every point:
LST (`LST(t0) + 360.98564736629 deg/day * (t - t0)`; the formula's `T^2` term moves < 1e-8 deg in a
day), the observer's vector (the moon's parallax), and the rotation to altitude and hour angle
(`a = x cos LST + y sin LST = r cos dec cos H`, `h = x sin LST - y cos LST = r cos dec sin H`,
`alt = asin((sin lat * z + cos lat * a) / r)`, `H = atan2(h, a)`). The equation of the equinoxes is
taken at noon (it changes < 1e-6 deg in a day). The sun's semidiameter is NOT: it changes up to
7.7e-5 deg in a day, which moved events up to 10 s at 89.9 N when held at noon - both
semidiameters come from the interpolated vector's length at each sample (the sun's in AU,
`0.26656 / r`) (astro review 2026-09-27). Interpolation error:
< 0.5" (moon), ~1e-5" (sun). Against direct evaluation at every point over 13,970 sun and 11,176
moon events (2026-2028; the six vector places plus 78 N and 78 S) nothing moved by more than 1 s
and no presence changed. A port may evaluate directly at every point instead - same events to the
second. On the ESP32 this is ~1,500 (sun) and ~3,300 (moon) libm calls a day instead of ~8,900 each.

| Event | Function crossing zero | Direction |
|---|---|---|
| sunrise / sunset | sun centre alt + 0.5667 + sun SD | up / down |
| first / last civil light | sun centre alt + 6 | up / down |
| solar noon | sun hour angle (wrapped to -180..180) | - to + |
| moonrise / moonset | topocentric moon centre alt + 0.5667 + moon SD | up / down |
| moon overhead / underfoot | topocentric hour angle through 0 / through 180 | |

0.5667 deg is the standard 34' refraction; with the upper limb that is the almanac definition
(PyEphem: `pressure = 0, horizon = -0:34`, upper limb). A grazing pass - the altitude touching the
threshold between two samples - can be missed or found by either implementation; the test allows
a mismatch only when the day's extreme altitude is within 0.1 deg of the threshold.

**One vector is wrong, and the test says so.** PyEphem 4.2's `next_rising()` iterates on the hour
angle and, from some starts, lands on a moment the body cannot reach the horizon and raises
`NeverUpError` although the event happens later that day; `gen_almanac_vectors.py`'s `first_in()`
writes that as 0. Every 0 in the day vectors was re-searched with PyEphem from each whole hour of
the day: exactly one row is affected (utqiagvik, `t0 = 1787734800`, moonrise: PyEphem's own answer
from a later start is 1787814266; ours is 1 s later). `tests/test_almanac.cpp` carries it as an
**erratum** with that evidence and checks it at the normal tolerance. COVEY's test needs the same
entry until the generator retries a `CircumpolarError` from later starts inside the window.

## Legal light

Two rules, a setting on each device:

- **30 min** (default): first legal light = sunrise - 30 min, last = sunset + 30 min.
  Washington's big-game rule ("one-half hour before sunrise to one-half hour after sunset").
- **Civil twilight**: sun centre 6 deg below the horizon. Longer than 30 min all year at 47 N:
  about 31 min at the equinoxes, 36 at the winter solstice and 41 at the summer solstice - so
  it starts earlier in the morning and ends later in the evening than the 30-minute rule, which
  is the direction that matters.

The countdown says which rule it used. It is only as good as the clock and the place: the screen
names both (clock source; GPS / pin / waypoint).

## Moon phase

8 names by elongation `E = lon_m - lon_s` in [0, 360): New < 12 or >= 348; Waxing crescent;
First quarter 78..102; Waxing gibbous; Full 168..192; Waning gibbous; Last quarter 258..282;
Waning crescent. (+-12 deg is about +-1 day.) **Next new / first quarter / full / last quarter**:
the crossing is where `g = wrap180(E - target)` goes from `< 0` to `>= 0` (a jump the other way,
or two samples 90 deg or more apart, is the wrap, not a crossing). Step forward 6 h at a time
until `g` changes sign, bisect to 1 s. **Next** is strictly after `from`, **previous** is at or
before it (step backwards). Age = days since the previous new moon.

The phone does not take a 6-h step while the target is far: it **jumps** by
`(degrees left to go) / 16 deg/day` (forward: `(target - E) mod 360`; backward:
`(E - target) mod 360`), which cannot pass the target because the elongation moves at most
14.4 deg/day (measured 2026-2036), and takes 6-h steps only once a jump would be shorter than
6 h. The last bracket is narrowed to 1 s by the **Illinois** method (regula falsi that halves a
stale end, whole seconds, bisection after 8 tries) - the same second bisection finds, since the
elongation is monotonic over 6 h. Checked: 2592 next/previous phase and season searches, all
identical to plain 6-h / 1-day stepping + bisection. ~8 longitude-only evaluations a search
instead of up to ~135.

## Seasons

The sun's apparent longitude (`lambda` above) through 0 / 90 / 180 / 270 (March equinox, June
solstice, September equinox, December solstice), first strictly after `from`: 1-day steps,
bisection, with the same safe jump (`degrees left / 1.1 deg/day`; the sun moves at most
1.019 deg/day) and the same Illinois narrowing. Accuracy ~10 min (596 s worst against PyEphem);
shown as a date.

## Solunar (the folk tables - labelled as such)

John Alden Knight's periods, as fishing/hunting apps compute them:
- **Major**: moon overhead and moon underfoot, each +-60 min.
- **Minor**: moonrise and moonset, each +-30 min.
- **Day rating 0-4**: 3 within 12 deg of new or full (~1 day), 2 within 36, 1 within 60, else 0;
  +1 if any period overlaps sunrise or sunset (within its span); max 4.
  0 Poor, 1 Fair, 2 Good, 3 Very good, 4 Excellent.

Exactly: the periods are the events of `astroMoonDay` for the day (an absent event has no
period); majors and minors are each listed in time order. The phase term uses the elongation `E`
at `t0 + 12 h`, distance `min(E, 360 - E, |E - 180|)`, bands inclusive (`<= 12`, `<= 36`, `<= 60`).
"Overlaps" = a non-zero sunrise or sunset inside `[mid - half, mid + half]`, inclusive.

## Accuracy against the vectors (worst case, `tests/test_almanac.cpp`, 2026-09-27)

| Quantity | Worst | Tolerance |
|---|---|---|
| sun dawn / rise / noon / set / dusk | 15 / 13 / 3 / 17 / 17 s (all at 71 N but noon) | 60 s |
| moonrise / moonset / overhead / underfoot | 5 / 5 / 1 / 1 s | 120 s |
| moon alt / az | 0.0027 / 0.0091 deg | 0.1 / 0.2 deg |
| sun alt / az | 0.0099 / 0.0176 deg | 0.05 deg |
| illuminated fraction | 0.0024 (PyEphem's own phase is an approximation) | 0.005 |
| signed elongation | 0.0090 deg | 0.1 deg |
| next new / quarter / full | 81 s | 300 s |
| next equinox / solstice | 596 s | 1200 s |

No grazing allowance was needed (0 of 4981 compared events); one erratum (above).

## Magnetic declination (WMM2025)

NOAA's World Magnetic Model 2025, degree 12, the 90 coefficient rows embedded (public domain,
`WMM.COF` of 2024-11-13). Standard algorithm: geodetic to geocentric spherical, Schmidt
semi-normalised associated Legendre functions by recursion, secular variation `g(t) = g + t*gdot`
with `t = year - 2025.0` (the decimal year is `Y + (seconds since 1 Jan Y 00:00 UTC) / (seconds in
Y)`; NOAA's own tool uses whole days, a difference under 0.0003 deg), field `X, Y, Z` rotated back to geodetic; `D = atan2(Y, X)`,
`I = atan2(Z, H)`. Valid **2025.0 - 2030.0**; outside it the screen says the model has expired
instead of showing a number. Tested against all 100 NOAA test rows (D, I to 0.01 deg, fields to 1 nT).

## Grid references

UTM (WGS84, Kruger series to n^4, Karney's form), with the Norway (32V) and Svalbard (31X..37X)
zone exceptions, 80 S .. 84 N (outside: none). MGRS from UTM: 100 km column letter from
`(zone - 1) % 3` -> ABCDEFGH / JKLMNPQR / STUVWXYZ, row letter ABCDEFGHJKLMNPQRSTUV offset by 5
for even zones; digits TRUNCATED to the metre (MGRS never rounds). Shown as `10T ET 91134 61042`.
Three decisions both ports make the same way: (1) 1e-6 m is added before truncating, so an easting
of exactly 500000 stays `00000` in its own square (the (78, 15) vector); (2) any lat < 0 takes the
10,000 km false northing (-0.0 is north); (3) degrees + decimal minutes are rounded ONCE as a whole
number of thousandths of a minute, so the carry into the next degree is integer arithmetic, and
the hemisphere letter follows the rounded value (a value that rounds to zero is N / E).
⚠ The `utm` package is itself up to ~0.92 mm off (an n^6 series agrees with ours to 2.4e-7 m), so
the vectors' easting/northing are checked to 2 mm, not 1: the margin is the reference's, not ours.
The `mgrs` package disagrees with a true truncation for points < 0.6 mm below a whole metre; none
of the vector points is one.

## Elevation

Tiles: **`/maps/elev/<z>/<x>/<y>.elv`** on the phone card and **`/root/covey-elev/<z>/<x>/<y>.elv`**
on COVEY (beside its tile tree, never inside it: the tile store's janitor judges every file there): 256 x 256 **little-endian int16 metres**, row-major, 131072 bytes,
the web-mercator z/x/y of the map tiles. Built from the AWS Open Data **Terrain Tiles**
(Mapzen "terrarium" PNG: `m = R*256 + G + B/256 - 32768`; sources USGS 3DEP/NED, SRTM, GMTED,
ETOPO; attribution in the README) by `tools/make_elev_tiles.py` on the Mac, or by the phone's
own downloader ("Elevation too" on the Download form: the job's square at z13 and z10, see
docs/maps.md; the same rounding, so the same bytes - tests/test_tilepng.cpp checks a real tile). z13 is ~13 m per pixel at 47 N; 3DEP is 10 m.
Two layers: **z13** (~13 m) under every area held at zoom 11 or finer, and a coarse **z10**
(~100 m) under everything down to zoom 8. A sample tries z13 first, then z10 - also when the
z13 tile is there but has no data at the point - and says which (`elevSample`, `zUsed`); the
result is ok, or "no data" if either layer had a tile, else "no tile".
Sampling (`elevSampleZ`): the point's tile and fractional pixel `(px, py)` in [0, 256) by the
map's own projection (lon wrapped to [-180, 180), lat clamped to the mercator limit, the south
limit held inside the last row). Bilinear between pixel **centres** (pixel i covers [i, i+1),
centre i + 0.5): `i0 = floor(px - 0.5)`, `fx = px - 0.5 - i0`, pair (i0, i0+1), rows the same.
In the **outer half-pixel on any side** (px < 0.5 or px >= 255.5, same for py) both of the pair
are the edge pixel: a sample never opens the neighbouring tile (at most two 4-byte reads, one
per row). -32768 = no data: such a neighbour is dropped and the others' weights renormalised;
none left, or the rest weighing 0 (the point exactly on a line of void centres), = no data.
Terrarium decode (`elevFromTerrarium`, and `make_elev_tiles.py`): the metres rounded **half away
from zero** (-0.5 -> -1, not Python's banker's 0), clamped to [-32767, 32767] - a real -32768
becomes -32767, so -32768 only ever means no data.
The master is `~/elev-master/elev` on the Mac; `tools/cardday.sh` copies it onto a card (step 4)
and to COVEY - `card_clone.sh --no-tiles` skips it like every numbered folder under /maps.

`elev` is a **reserved name** under `/maps`: the map never lists it as a map area.

**Relative altitude on the map** compares the ground under the crosshair with the ground under
"you" - both from the tiles, never the GPS's own altitude (a phone GPS is +-10-20 m vertically,
and two readings of one surface are consistent where a GPS and a surface are not). "You" is the
map's own `selfPosition()` (fresh GPS, else your pin, else a poor or old fix - the row then says
`above poor fix` / `above old fix`, never `you`), and the reference place when there is none of
those.

## Units

One setting per device, **Metric** or **US** (feet, miles, mph; since 0.9.81 also Fahrenheit,
inHg and inches for the weather). The map's scale bar and distances, the elevation readouts and
the Almanac all follow it. `WiPhone/units.cpp`; exact
conversions 1 ft = 0.3048 m, 1 mi = 1609.344 m, 1 mph = 0.44704 m/s.

- **Distance** (negative = 0): metric is the old `meshPosFmtDist` byte for byte - `%dm` under
  999.5 m, `%.1fkm` under 10 km, else `%.0fkm` (so 9,950-9,999 m still prints `10.0km`). US:
  `%dft` under 999.5 ft, `%.1fmi` under **9.95** mi, else `%.0fmi` (never `1000ft`, never `10.0mi`).
- **Altitude** (a difference with a sign): whole m / ft rounded half away from zero; anything
  that rounds to 0 is a bare `0m`/`0ft`; `+` only when asked and > 0; US has a thousands comma
  (`1,352ft`), metric none (`4392m`); not finite = `--`; held to +-1e12 of the unit.
- **Speed**: one decimal, `3.2km/h` / `2.0mph`; negative = 0; not finite = `--`.
- **The weather's** (0.9.81, fetched in SI, written at draw time): temperature whole degrees half
  away from zero, no degree glyph, never `-0` (`11C` / `52F`); wind whole units (`14 km/h` /
  `9 mph`, the gusts bare: `gusts 30`); pressure `1018 hPa` / `30.06 inHg` (1 inHg = 33.8639 hPa,
  hundredths rounded once in integers); precipitation `2.7 mm` / `0.11 in`, a trace that rounds to
  0 `<0.1 mm` / `<0.01 in`.
- **Scale bar**: the longest candidate whose `(int)(len / mPerPx + 0.5)` is 1..maxPx. Metric =
  the old `mapScaleBar`: 1/2/5 x 10^n m from 1 m to 50,000 km, labels `%dm` under 1000 m else
  `%dkm` (whole km). US: 1 2 5 10 20 50 100 200 500 1000 2000 ft then 0.5 1 2 5 10 20 50 100 200
  500 mi, labels `500ft`, `2000ft` (no comma on the bar), `0.5mi`, `2mi`. None fits: the smallest
  (1 m / 1 ft), its pixels held to 1..maxPx. Returns the length in metres rounded, at least 1.

## On the phone (Menu > Almanac)

`WiPhone/app_almanac.cpp` draws eight menu screens from `WiPhone/almanac_lines.cpp` (WEATHER's
from `WiPhone/weather_lines.cpp`, 0.9.81), which hold every wording; the serial `almanac` prints the same lines, so the bench proof against PyEphem is a
proof of the screen. `tests/test_almanac_lines.cpp` checks the wordings and measures every row
against the phone's own font (Akrobat Bold 20, 232 px) over six places, two years of days, both
rules and both units. The Akrobat faces have **no degree glyph** (nor anything past `~`), so no
row uses one.

| Screen | What it says |
|---|---|
| **TODAY** (first) | the date (`Sun Sep 27`, `(tomorrow)`, `(+3 days)`); **today only** the countdown (`LEGAL LIGHT: 3h 12m left` / `First light in 5h 02m` / `Dark - first light 06:34`); `Legal 06:34-19:28 (30 min)`; `Sun 07:03-18:58 (11h 55m)`; `Moon: Waning gibbous 98%`; `Solunar: Good` and the current or next period (another day: its first); the place (`At GPS` / `At Camp` / `At me (pin)` / `At last GPS 2h 5m ago` / `At map view`, see **Which place**); a day across a US clock change from today (TODAY, SUN, MOON, SOLUNAR, under the date or the place) is computed and shown **in the offset that day will have** and says so: `Times in UTC-8 (after the Nov 1 change)` / `Times in UTC-7 (before the Nov 1 change)` (see **Keys**); ON a change day every day screen also carries DATE's sentence for the day (`US clocks went back 1 h at 2 AM today - check Time offset`, before 2 AM `go back ... set Time offset then`; TODAY above its `Legal` row, the others under the place), because from 2 AM the phone is only TAKEN to have been changed; a waypoint's name (`At Hunting Camp North Fork`, `Source: waypoint ...`) is a wrapped sentence, never a row cut to `..`; a part not worked out yet is ONE `Computing...` row where it goes; with a weather cache (0.9.81) the most severe NWS alert in force near the top, in its colour (`Alert: Flood Warning until 18:00`; `Alert 12 km away: ...` when the alerts' own point, kept apart from the forecast's, is elsewhere), and a summary over the entries (`Weather: 52F, rain 60% this afternoon` within an hour of the fetch, `Forecast: ...` after it, `Forecast 12 km away: ...` from elsewhere); then the entries `Sun...` `Moon...` `Solunar...` `Weather...` `Position & GPS...` `Date & seasons...` `Settings...` |
| **SUN** | the place; first legal light, sunrise, solar noon, sunset, last legal light; civil dawn and dusk (under the 30-min rule); day length and the change from the day before (`-2m 51s vs the day before`); today `Sun now: alt 32, az 150 SSE`; which rule |
| **MOON** | the place; rise / overhead / set / underfoot in time order, then the ones that do not happen (`No moonset today`); phase name, illuminated %, age; the next new moon / first quarter / full moon / last quarter in time order with weekday, date and time - each on the clock of ITS date (`almTzAt`: on Oct 25 at UTC-7 the new moon past Nov 1 is `Sun Nov 8 23:02`, not `Mon Nov 9 00:02`; DATE's next season likewise); today `Moon now: alt 23, az 120 ESE` or `below the horizon` (another day: the phase at local noon) |
| **SOLUNAR** | the place; `Rating: Good (2 of 4)`; majors then minors, each `HH:MM-HH:MM` in time order, the one we are in marked `now`; `Folk tables (J. A. Knight)` |
| **POSITION** | source and age (`Source: GPS 3s`, `last GPS 12m`, `pin`, `waypoint Camp`, `map view`); lat/lon to 5 places; degrees + decimal minutes; UTM; MGRS; the ground's height from `/maps/elev` (`coarse` when from z10) and the GPS altitude; speed and course (`Speed 3.2km/h, 213 SSW`) from an RMC under 10 s old moving over 1 km/h, else `Stationary` (the altitude from a GGA under 120 s old: each by its own sentence's age, not the fix's, which either sentence refreshes); sats/HDOP; declination (`14.8 E`, `true = magnetic + 14.8`, `WMM2025, valid to 2030` - after 2030 `model expired`, no number) |
| **DATE** (always today) | `Sunday, September 27`; `2026, day 270 of 365, week 39`; `Time 14:05 UTC-7 (ntp)` (a mesh clock is flagged); the US daylight-saving reminder, which does **no arithmetic on the offset** (review 2026-09-27: `set Time offset to -9` was told to a phone already moved to -8 - the offset alone cannot say Pacific-done from Alaska-not-yet): in the 21 days up to the change `US clocks go back 1 h Sun Nov 1 - set Time offset then` (`go forward` in March); on the day `US clocks went back 1 h at 2 AM today - check Time offset` (before 2 AM: `go back ... at 2 AM today - set Time offset then`). Only with **US daylight saving: yes** and a whole-hour offset a changing US zone can have (before November a daylight one, -4..-9; before March a standard one, -5..-10; on the day either); the next equinox or solstice and the days to go |
| **WEATHER** (0.9.81) | Open-Meteo's forecast and the NWS's active alerts, kept on the card (`/wx/weather.txt`) for offline: alerts first, coloured (red Severe/Extreme, yellow otherwise; `No alerts at 07:10` / `Alerts: US only (NWS)` / `Alerts not checked (no answer)` / `Alerts: 9+ (too many to list)` - never a bare "No alerts"); `Now: 17C, Mostly clear` within an hour of the fetch, else `Forecast for 14:00: ...`; feels-like and humidity, `Wind from N 8 km/h, gusts 13` (where it blows FROM), pressure; the next day of hours every 3 h (past ones dropped); seven days, each labelled by the date of its MIDPOINT (t + 12 h) on the phone's clock; `No forecast after Fri Oct 9 - refresh on Wi-Fi` past the last; `As of 07:10 (3h 05m ago) for GPS` (an age only on an NTP/GPS clock); `Local time here is UTC-7 - check the clock setting` when the place's offset is not the phone's; the credit `Weather: Open-Meteo.com` `(CC BY 4.0); alerts: NWS`. The left soft key is **Refresh** (it only queues: the fetch starts on the next timer pass). The README's "Weather" has the when and what-is-sent |
| **SETTINGS** | `Legal light: 30 min rule` <-> `Legal light: civil twilight`; `Units: metric` <-> `Units: US (ft, mi, mph, F)` (the Maps menu's row, word for word: `unitsSettingRow`); `US daylight saving: yes` <-> `US daylight saving: no (HI, AZ)` (no = no DST reminder and no clock-change shift: Hawaii and Arizona never change, and an offset cannot tell them from the zones that do). All persist (NVS `wpmesh/legalrule`, `wpmesh/units`, `wpmesh/almdst`, default yes) and the units also drive the map |

**Keys**: Up/Down scroll. **Left/Right** step the shown day (-365..+365) on TODAY, SUN, MOON and
SOLUNAR - the header shows the date. **A day across a US clock change** (review 2026-09-27: on Oct
27 at UTC-7, Nov 7 read `Legal 07:31-18:10` while its clock will read 17:10 at that moment - the
planned end of legal light an hour LATE) is computed and shown in the offset it will have (or
had): today's -1 h across the autumn change, +1 h across the spring one (`almDayTz`), with the
`Times in ...` row. Only with **US daylight saving: yes**, when today's offset is one a changing
US zone has on today's side (daylight -4..-9, standard -5..-10) and the day lies on the other side
of the change between them; both changes crossed is the same side. TODAY's side follows DATE's
reminder, the **2 AM rule** (review 2026-09-27: `almDayTz` counted the whole change day as
changed while the reminder said "set Time offset then" before 2 AM - on Nov 1 at 01:00 at -7,
Nov 2 read `Legal 07:23-18:18` with no note, the clock will read 06:23-17:18, and on Mar 14 2027
at 01:00 at -8 Mar 15's first legal light read an hour EARLY): before 2 AM on a change day the
phone is on the old side and a day past the change is shifted and says so; from 2 AM it is taken
to be on the new side already, and every day screen carries the day's `check Time offset`
sentence. A SHOWN day is on the side of its date (the change day itself: the new one). The
test holds the reminder's `go`/`went` and the stepped day's offset to one rule at every 10
minutes of both change days, from both offsets each can have. The phone's clock is never
touched. **A zoned clock (0.9.82):** with Settings → Time zone on Auto (a zone found from a
measured fix) or a named zone, the PHONE changes its own offset at the zone's DST changes
(`tz_service.cpp`), so the Almanac takes the zone's rule instead of all of the above
(`AlmCtx.tzZoned`, `almCtxTzAt`/`almZone*`): a stepped day's offset is the rule's at that day's
local noon, an instant's offset (MOON's quarters, DATE's season, the weather's fetch) is the
rule's at that instant, Newfoundland's -3:30 is handled, and the sentences say it happens by
itself (`Clocks go back 1 h Sun Nov 1 - the phone changes by itself`; on the day `... at 2 AM
today - the phone changes by itself` / `... changed by itself`). Settings' row reads `Daylight
saving: automatic` (`none here`); OK still toggles the switch a typed offset goes by. A typed
offset (Custom, or Auto before a zone is found) is everything above, unchanged. **OK** opens an
entry or toggles a setting; on SUN, MOON and
SOLUNAR the left soft key (`Today`) returns to today. **Back** goes up to TODAY, then exits.

**Which place** (measured on phone 2, 2026-09-27: no fix since a reboot and no pin, so every
screen said `No place known yet`; COVEY's Almanac falls back). In order: `resolveReference()` (a
fresh GPS fix, a chosen waypoint, the pin, or the explicit GPS reference with a stale fix); the
last GPS fix of this boot however old (`At last GPS 2h 5m ago`); the Maps app's saved view (NVS
`maps/saved`, `lat`, `lon` - what `MapsApp::saveView` writes; read once when the Almanac opens;
`At map view`); only then the guidance rows. The last fix must pass the bar `resolveReference()`
and the map's "me" use (`meshPosFixUsable`: 4+ satellites, HDOP <= 10; review 2026-09-27: a fresh
3-satellite fix, measured elsewhere 20 km off, read `At last GPS 0s ago`) - a poor fix is no
place, and the map's view, or the guidance, comes next. 0,0 (the map's "nothing to show") and a place off the
globe are skipped. A place a few km off is still right to the minute (1 km moves sunrise ~4 s),
and every screen names which place it is.

**Rounding, which is a legal question**: sun and moon times are the nearest minute; legal light
is rounded **inward** (first legal light up, last legal light down) and so is the countdown (`in`
rounds up, `left` rounds down), so the window shown is never wider than the true one. The
Meshtastic "Sun & legal light" screen and the serial `sun` use the same computation and rounding
(`almanacLegalToday`, whose countdown is `almCountdownDay` - the function TODAY calls, host-tested
against the screen; it keeps the last day's sun for the place to within `almSamePlace` - 0.001
deg, ~100 m, sunrise ~0.4 s - and the Almanac's slices hand it TODAY's, so the Meshtastic screen's
Refresh costs nothing until you have moved ~100 m or the date changes. Review 2026-09-27: it was
keyed on exact doubles, which a live GPS fix - it moves 1e-7 deg every second - never matched,
so every open ran a ~49 ms `astroSunDay` in its key handler (~98 ms after dark); the first open
per day and place still does); `sun_times.cpp` is gone.

**Cost** (A1, measured on phone 2 at 160 MHz with WiFi on, 2026-09-27): the core day was 148,485 us
in ONE loop pass - sun 1,843 + moon 3,668 + solunar 91 libm calls, ~26.5 us each with their
arithmetic - and the TODAY build on top of it 26 ms more, all of it `astroMoonPhaseAt` (956 calls,
740 of them the search for the last new moon, for an age TODAY does not show): `almanac cost` said
174 ms, a pass logged a 334 ms LOOP STALL (the minute tick recomputing inside the clock's block),
and the serial `almanac` took 423 ms (tables 325). Now:
- The day's tables are computed **in slices on the app timer** (`almDayWorkRun`): units - a
  3-hour node or a 5-minute sample of a sun or moon day (`AstroDayJob`, <= ~5 ms), the solunar
  table, one quarter's or one new moon's search (~20 ms, only ever the FIRST unit of a slice) -
  until 25 ms has gone, then the loop runs for 15 ms, counted from the slice's END (review
  2026-09-27: WiPhone.ino re-arms the app timer from a pass's START, so a 15 ms period after a
  30 ms slice fired on the very next pass - the gap did not exist; the period is now the pass's
  own work + 15). Worst slice ~30 ms (a search-led one ~45); the core is ~6 slices, on the
  screen ~0.4 s after the key.
- **One thing a pass.** A slice never rebuilds or repaints: a part it lands sets `landed`, and
  the NEXT timer pass rebuilds (~6-12 ms of rows), draws the menu and returns REDRAW_SCREEN - and
  runs no slice. The Almanac pushes only the band between the header and the footer
  (`drewInsideBand`, ~4/5 of the ~60 ms full push, ~48 ms estimated). Before, a landing pass was
  slice + rebuild + full push, ~100-120 ms estimated, 3-5 times a day computed. `almanac cost`
  now times the paint too (the menu drawn into the sprite; the push itself is after the app
  returns). **Bit-identical** to the one-call
  functions however it is cut (`tests/test_almanac.cpp`: the day job cut at every unit and at
  random; `tests/test_almanac_lines.cpp`: whole AlmDays after random cuts). Each part's rows
  appear as it lands; `Computing...` until then.
- Kept per (shown day, the day's offset, the place it was keyed for, to within `almSamePlace`'s
  0.001 deg) in two slots, TODAY's and the stepped day's - a tolerance around the keyed place,
  not a grid (review 2026-09-27: a still GPS within noise of a 0.001-degree line re-keyed the whole
  day, ~148 ms of CPU and a log line, on a minute tick). A place that moved ~100 m keeps its rows
  on the screen while the new ones are worked out.
- A rebuild (the minute tick, POSITION's second, a part landing) keeps the list where it was
  scrolled (`almMenuTop`: the highlighted row stays where it was on the glass) and the highlight
  on the row with the same words, else the same position; before, every rebuild started the list
  at the top.
- **Titles** are `almTitle`'s: TODAY `Sep 27 (Sun)` (was `Sun Sep 27`, one colon from SUN's
  `Sun: Sep 27` on a Sunday), SUN `Sun: Sep 27`, MOON `Moon: Sep 27`, SOLUNAR `Solunar: Sep 27`,
  `Position & GPS`, `Date & seasons`, `Settings`. Every one, on every date, is measured against
  108 px of Bold 18 - the room the header leaves with WiFi, one kind of unread message and the
  widest clock (109); both message kinds, or the mute icon, end it in `..`.
- **Key handlers compute no table and never search**: Left/Right and OK only queue a day and
  build from what is there (onto a day not computed yet: no astronomy at all).
- **The minute tick** rebuilds from the caches: the countdown, `Sun now` (~27 libm calls), `Moon
  now` (~215), the moon's phase at now with no search (`astroMoonPhaseWith`, ~215); MOON's age
  comes from the new moons either side of now (`AlmMoonAge`, two searches a lunation). Estimated
  <= ~12 ms. POSITION refreshes once a second (declination ~180, UTM/MGRS ~60 calls: ~7 ms) and
  reads the elevation card on its timer only, after ~11 m of movement.
- `tests/check_almanac_review.py` holds it: no table, search or scan in a key handler, a build or
  a screen builder; the slices run with the deadline.
Every day whose core lands logs one `ALMANAC: day ... whole in N slices: CPU ... us (sun, moon,
sol), longest slice ... us, ... ms after it was asked` line (log_e). `almanac cost` prints the
slices (the longest since boot and which parts), each part's CPU, and the builds (the last, the
minute tick's, the longest); `almanac bench` times each piece now, in one pass.

**Serial**: `almanac [lat,lon] [+N|-N]` prints every screen's lines for that day (default: the
app's place, today) and then the raw instants as UNIX seconds (`raw sun: dawn=... rise=...`,
`raw moon: ...`, `raw quarters after ...`, `raw seasons after ...`) for the comparison with
PyEphem - in ONE pass (~0.4 s at 160 MHz: a bench command, and it prints its own time); `almanac
cost`; `almanac bench`; `open almanac`.
