/*
 * astro.cpp - the Almanac's sun, moon, phase, season, solunar and calendar arithmetic.
 * See astro.h for the API and docs/almanac.md for the algorithms (COVEY's almanac.py ports THIS
 * file). Proven against PyEphem by tests/test_almanac.cpp (tests/vectors_almanac.h).
 *
 * Pure C++11: <math.h>/<stdint.h>/<string.h> only, no heap, no static mutable state, every
 * function's locals well under 512 bytes.
 */

#include "astro.h"
#include <math.h>
#include <string.h>

namespace {

const double K_PI = 3.14159265358979323846;
const double D2R = K_PI / 180.0;
const double R2D = 180.0 / K_PI;

const int64_t UNIX_J2000 = 946728000;       // 2000-01-01 12:00 UT = JD 2451545.0

// TT - UT, seconds. The sun and moon theories run on TT; sidereal time on UT. Measured 69.1-69.3 s
// through 2020-2026; a few seconds off moves the moon ~1" and the sun 0.1" - nothing shown.
const double DELTA_T = 69.0;

const int64_t DAY_SCAN_STEP = 300;          // the day scan: 5-minute samples, 289 of them

// Upper bounds on how fast the searched angles move (deg/day), for the safe jumps in angleSearch.
// The moon's elongation moves 10.7..14.4 deg/day (measured 2026-2036); the sun's longitude
// 0.953..1.019 deg/day.
const double MOON_ELONG_MAX_RATE = 16.0;
const double SUN_LON_MAX_RATE    = 1.1;

const double EARTH_RADIUS_KM = 6378.14;
const double AU_KM = 149597870.7;

double rev(double x) {                       // [0, 360)
  double r = fmod(x, 360.0);
  if (r < 0) r += 360.0;
  if (r >= 360.0) r -= 360.0;                // -1e-17 + 360 rounds to 360
  return r;
}
double wrap180(double x) {                   // [-180, 180)
  double r = rev(x);
  return r >= 180.0 ? r - 360.0 : r;
}
double sind(double x) { return sin(x * D2R); }
double cosd(double x) { return cos(x * D2R); }
double clamp1(double x) { return x > 1.0 ? 1.0 : (x < -1.0 ? -1.0 : x); }

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}
int floorMod7(int64_t a) { return (int)(a - floorDiv(a, 7) * 7); }

// Julian centuries of TT since J2000.0.
double centuriesTT(int64_t t) {
  return ((double)(t - UNIX_J2000) + DELTA_T) / (86400.0 * 36525.0);
}

// The frame of date both bodies share: nutation in longitude (the sun section's -0.00478 sin Om)
// and the true obliquity (eps0 + 0.00256 cos Om), deg.
struct Frame { double dpsi, eps; };

void frameAt(double T, Frame* f) {
  double Om = 125.04 - 1934.136 * T;
  double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
  f->dpsi = -0.00478 * sind(Om);
  f->eps = eps0 + 0.00256 * cosd(Om);
}

// Local APPARENT sidereal time, deg: GMST (UT, Meeus 12.4) + the equation of the equinoxes
// eqeq = dpsi cos eps (<= 0.0044 deg) + east longitude. RA of both bodies is of the true equinox.
double lstDeg(int64_t t, double lon, double eqeq) {
  double dd = (double)(t - UNIX_J2000) / 86400.0;
  double T = dd / 36525.0;
  double gmst = 280.46061837 + 360.98564736629 * dd + T * T * (0.000387933 - T / 38710000.0);
  return rev(gmst + eqeq + lon);
}

// Equatorial (hour angle, declination) to horizontal. No refraction. Azimuth from north,
// clockwise: the spec's atan2(sin H, cos H sin lat - tan dec cos lat) + 180 with both arguments
// multiplied by cos dec (> 0), which keeps tan() out of it.
void horizontal(double ha, double dec, double lat, double* alt, double* az) {
  double sh = sind(ha), ch = cosd(ha), sd = sind(dec), cd = cosd(dec);
  double sl = sind(lat), cl = cosd(lat);
  if (alt) *alt = asin(clamp1(sl * sd + cl * cd * ch)) * R2D;
  if (az) *az = rev(atan2(sh * cd, ch * sl * cd - sd * cl) * R2D + 180.0);
}

// ---- the sun: NOAA / Meeus ch. 25 low accuracy ------------------------------------------------

struct SunEq {
  double lambda;   // apparent ecliptic longitude, deg [0, 360)
  double ra, dec;  // apparent, deg
  double R;        // distance, AU
  Frame fr;
};

void sunEq(int64_t t, SunEq* s) {
  double T = centuriesTT(t);
  frameAt(T, &s->fr);
  double L0 = rev(280.46646 + T * (36000.76983 + 0.0003032 * T));
  double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
  double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
  double C = sind(M) * (1.914602 - T * (0.004817 + 0.000014 * T))
           + sind(2.0 * M) * (0.019993 - 0.000101 * T)
           + sind(3.0 * M) * 0.000289;
  double nu = M + C;
  s->R = 1.000001018 * (1.0 - e * e) / (1.0 + e * cosd(nu));
  double lam = L0 + C - 0.00569 + s->fr.dpsi;       // aberration + nutation
  double eps = s->fr.eps;
  s->lambda = rev(lam);
  s->ra = rev(atan2(cosd(eps) * sind(lam), cosd(lam)) * R2D);
  s->dec = asin(clamp1(sind(eps) * sind(lam))) * R2D;
}

// ---- the moon: Meeus ch. 47 (ELP-2000/82 truncated: the 60 + 60 terms of tables 47.A/B) ------
//
// Meeus quotes ~10" in longitude, ~4" in latitude; against PyEphem's geocentric moon (4000
// instants, 2022-2030) the worst was 11" / 10" / 11 km, no row of either table off by > 0.5".
// Schlyter's elements (the first version of this file) were 0.08 deg out in elongation: 581 s on
// a quarter, 0.31 deg of azimuth under a high moon, 3.7 min on a slow Arctic moonset.

struct LunarLR { int8_t d, m, mp, f; int32_t l, r; };   // sum_l in 1e-6 deg, sum_r in 1e-3 km
struct LunarB  { int8_t d, m, mp, f; int32_t b; };      // sum_b in 1e-6 deg

const LunarLR MOON_LR[60] = {
  { 0,  0,  1,  0, 6288774, -20905355 }, { 2,  0, -1,  0, 1274027, -3699111 },
  { 2,  0,  0,  0,  658314,  -2955968 }, { 0,  0,  2,  0,  213618,  -569925 },
  { 0,  1,  0,  0, -185116,     48888 }, { 0,  0,  0,  2, -114332,    -3149 },
  { 2,  0, -2,  0,   58793,    246158 }, { 2, -1, -1,  0,   57066,  -152138 },
  { 2,  0,  1,  0,   53322,   -170733 }, { 2, -1,  0,  0,   45758,  -204586 },
  { 0,  1, -1,  0,  -40923,   -129620 }, { 1,  0,  0,  0,  -34720,   108743 },
  { 0,  1,  1,  0,  -30383,    104755 }, { 2,  0,  0, -2,   15327,    10321 },
  { 0,  0,  1,  2,  -12528,         0 }, { 0,  0,  1, -2,   10980,    79661 },
  { 4,  0, -1,  0,   10675,    -34782 }, { 0,  0,  3,  0,   10034,   -23210 },
  { 4,  0, -2,  0,    8548,    -21636 }, { 2,  1, -1,  0,   -7888,    24208 },
  { 2,  1,  0,  0,   -6766,     30824 }, { 1,  0, -1,  0,   -5163,    -8379 },
  { 1,  1,  0,  0,    4987,    -16675 }, { 2, -1,  1,  0,    4036,   -12831 },
  { 2,  0,  2,  0,    3994,    -10445 }, { 4,  0,  0,  0,    3861,   -11650 },
  { 2,  0, -3,  0,    3665,     14403 }, { 0,  1, -2,  0,   -2689,    -7003 },
  { 2,  0, -1,  2,   -2602,         0 }, { 2, -1, -2,  0,    2390,    10056 },
  { 1,  0,  1,  0,   -2348,      6322 }, { 2, -2,  0,  0,    2236,    -9884 },
  { 0,  1,  2,  0,   -2120,      5751 }, { 0,  2,  0,  0,   -2069,        0 },
  { 2, -2, -1,  0,    2048,     -4950 }, { 2,  0,  1, -2,   -1773,     4130 },
  { 2,  0,  0,  2,   -1595,         0 }, { 4, -1, -1,  0,    1215,    -3958 },
  { 0,  0,  2,  2,   -1110,         0 }, { 3,  0, -1,  0,    -892,     3258 },
  { 2,  1,  1,  0,    -810,      2616 }, { 4, -1, -2,  0,     759,    -1897 },
  { 0,  2, -1,  0,    -713,     -2117 }, { 2,  2, -1,  0,    -700,     2354 },
  { 2,  1, -2,  0,     691,         0 }, { 2, -1,  0, -2,     596,        0 },
  { 4,  0,  1,  0,     549,     -1423 }, { 0,  0,  4,  0,     537,    -1117 },
  { 4, -1,  0,  0,     520,     -1571 }, { 1,  0, -2,  0,    -487,    -1739 },
  { 2,  1,  0, -2,    -399,         0 }, { 0,  0,  2, -2,    -381,    -4421 },
  { 1,  1,  1,  0,     351,         0 }, { 3,  0, -2,  0,    -340,        0 },
  { 4,  0, -3,  0,     330,         0 }, { 2, -1,  2,  0,     327,        0 },
  { 0,  2,  1,  0,    -323,      1165 }, { 1,  1, -1,  0,     299,        0 },
  { 2,  0,  3,  0,     294,         0 }, { 2,  0, -1, -2,       0,     8752 },
};

const LunarB MOON_B[60] = {
  { 0,  0,  0,  1, 5128122 }, { 0,  0,  1,  1,  280602 }, { 0,  0,  1, -1,  277693 },
  { 2,  0,  0, -1,  173237 }, { 2,  0, -1,  1,   55413 }, { 2,  0, -1, -1,   46271 },
  { 2,  0,  0,  1,   32573 }, { 0,  0,  2,  1,   17198 }, { 2,  0,  1, -1,    9266 },
  { 0,  0,  2, -1,    8822 }, { 2, -1,  0, -1,    8216 }, { 2,  0, -2, -1,    4324 },
  { 2,  0,  1,  1,    4200 }, { 2,  1,  0, -1,   -3359 }, { 2, -1, -1,  1,    2463 },
  { 2, -1,  0,  1,    2211 }, { 2, -1, -1, -1,    2065 }, { 0,  1, -1, -1,   -1870 },
  { 4,  0, -1, -1,    1828 }, { 0,  1,  0,  1,   -1794 }, { 0,  0,  0,  3,   -1749 },
  { 0,  1, -1,  1,   -1565 }, { 1,  0,  0,  1,   -1491 }, { 0,  1,  1,  1,   -1475 },
  { 0,  1,  1, -1,   -1410 }, { 0,  1,  0, -1,   -1344 }, { 1,  0,  0, -1,   -1335 },
  { 0,  0,  3,  1,    1107 }, { 4,  0,  0, -1,    1021 }, { 4,  0, -1,  1,     833 },
  { 0,  0,  1, -3,     777 }, { 4,  0, -2,  1,     671 }, { 2,  0,  0, -3,     607 },
  { 2,  0,  2, -1,     596 }, { 2, -1,  1, -1,     491 }, { 2,  0, -2,  1,    -451 },
  { 0,  0,  3, -1,     439 }, { 2,  0,  2,  1,     422 }, { 2,  0, -3, -1,     421 },
  { 2,  1, -1,  1,    -366 }, { 2,  1,  0,  1,    -351 }, { 4,  0,  0,  1,     331 },
  { 2, -1,  1,  1,     315 }, { 2, -2,  0, -1,     302 }, { 0,  0,  1,  3,    -283 },
  { 2,  1,  1, -1,    -229 }, { 1,  1,  0, -1,     223 }, { 1,  1,  0,  1,     223 },
  { 0,  1, -2, -1,    -220 }, { 2,  1, -1, -1,    -220 }, { 1,  0,  1,  1,    -185 },
  { 2, -1, -2, -1,     181 }, { 0,  1,  2,  1,    -177 }, { 4,  0, -2, -1,     176 },
  { 4, -1, -1, -1,     166 }, { 1,  0,  1, -1,    -164 }, { 4,  0,  1, -1,     132 },
  { 1,  0, -1, -1,    -119 }, { 4, -1,  0, -1,     115 }, { 2, -2,  0,  1,     107 },
};

struct MoonGeo {
  double lonGeom;      // geometric ecliptic longitude of date, deg [0, 360) (Meeus 47's lambda)
  double lon, lat;     // apparent (lon + nutation) longitude, latitude, deg
  double distKm;       // geocentric distance, km
  double x, y, z;      // geocentric equatorial (true equator and equinox of date), Earth radii
  Frame fr;
};

// full = false computes only lonGeom/lon (the longitude series: a third of the work) - all the
// elongation searches need.
void moonGeo(int64_t t, MoonGeo* g, bool full) {
  double T = centuriesTT(t);
  frameAt(T, &g->fr);
  double T2 = T * T, T3 = T2 * T, T4 = T3 * T;
  double Lp = rev(218.3164477 + 481267.88123421 * T - 0.0015786 * T2 + T3 / 538841.0 - T4 / 65194000.0);
  double D  = rev(297.8501921 + 445267.1114034 * T - 0.0018819 * T2 + T3 / 545868.0 - T4 / 113065000.0);
  double M  = rev(357.5291092 + 35999.0502909 * T - 0.0001536 * T2 + T3 / 24490000.0);
  double Mp = rev(134.9633964 + 477198.8675055 * T + 0.0087414 * T2 + T3 / 69699.0 - T4 / 14712000.0);
  double F  = rev(93.2720950 + 483202.0175233 * T - 0.0036539 * T2 - T3 / 3526000.0 + T4 / 863310000.0);
  double A1 = rev(119.75 + 131.849 * T);
  double A2 = rev(53.09 + 479264.290 * T);
  double A3 = rev(313.45 + 481266.484 * T);
  double E = 1.0 - 0.002516 * T - 0.0000074 * T2;
  double E2 = E * E;

  double sl = 0.0, sr = 0.0, sb = 0.0;
  for (int i = 0; i < 60; i++) {
    const LunarLR& k = MOON_LR[i];
    double arg = (k.d * D + k.m * M + k.mp * Mp + k.f * F) * D2R;
    double ef = k.m == 0 ? 1.0 : (k.m == 1 || k.m == -1) ? E : E2;
    if (k.l) sl += ef * k.l * sin(arg);
    if (full && k.r) sr += ef * k.r * cos(arg);
  }
  sl += 3958.0 * sind(A1) + 1962.0 * sind(Lp - F) + 318.0 * sind(A2);
  g->lonGeom = rev(Lp + sl / 1e6);
  g->lon = rev(g->lonGeom + g->fr.dpsi);
  if (!full) {
    g->lat = g->distKm = g->x = g->y = g->z = 0.0;
    return;
  }
  for (int i = 0; i < 60; i++) {
    const LunarB& k = MOON_B[i];
    double arg = (k.d * D + k.m * M + k.mp * Mp + k.f * F) * D2R;
    double ef = k.m == 0 ? 1.0 : (k.m == 1 || k.m == -1) ? E : E2;
    sb += ef * k.b * sin(arg);
  }
  sb += -2235.0 * sind(Lp) + 382.0 * sind(A3) + 175.0 * sind(A1 - F) + 175.0 * sind(A1 + F)
        + 127.0 * sind(Lp - Mp) - 115.0 * sind(Lp + Mp);

  g->lat = sb / 1e6;
  g->distKm = 385000.56 + sr / 1000.0;

  double r = g->distKm / EARTH_RADIUS_KM;
  double cb = cosd(g->lat);
  double xg = r * cb * cosd(g->lon);
  double yg = r * cb * sind(g->lon);
  double zg = r * sind(g->lat);
  double se = sind(g->fr.eps), ce = cosd(g->fr.eps);
  g->x = xg;
  g->y = yg * ce - zg * se;
  g->z = yg * se + zg * ce;
}

struct MoonTopo {
  double alt, az;   // topocentric, no refraction, deg
  double ha;        // topocentric hour angle, deg (unwrapped)
  double sd;        // topocentric semidiameter, deg
};

// Topocentric by vectors: the geocentric equatorial vector minus the observer's - no division by
// sin g, so the equator is fine.
void moonTopo(int64_t t, double lat, double lon, MoonTopo* o) {
  MoonGeo g;
  moonGeo(t, &g, true);
  double lst = lstDeg(t, lon, g.fr.dpsi * cosd(g.fr.eps));
  double gclat = lat - 0.1924 * sind(2.0 * lat);
  double rho = 0.99833 + 0.00167 * cosd(2.0 * lat);
  double x = g.x - rho * cosd(gclat) * cosd(lst);
  double y = g.y - rho * cosd(gclat) * sind(lst);
  double z = g.z - rho * sind(gclat);
  double rt = sqrt(x * x + y * y + z * z);
  double ra = atan2(y, x) * R2D;
  double dec = asin(clamp1(z / rt)) * R2D;
  o->ha = lst - ra;
  horizontal(o->ha, dec, lat, &o->alt, &o->az);
  o->sd = asin(clamp1(0.272481 / rt)) * R2D;
}

// ---- events in a local day ---------------------------------------------------------------------
//
// One body over one local day, for the 5-minute scan and its bisections. The theory (the lunar
// series, the solar formulas) is evaluated every 3 h - 9 times a day instead of ~320 - and the
// geocentric equatorial vector at any instant is the quadratic through the three nearest of
// those nodes. Everything that changes fast is exact at every sample: sidereal time, the
// observer's place (the moon's parallax) and the rotation to altitude and hour angle. The
// interpolation is < 0.5" off direct evaluation for the moon and ~1e-5" for the sun; over 12,524
// moon events (2026-2028, the six vector places) it moved none by more than 1 s and changed no
// presence. A port may evaluate the theory directly at every sample - same events to the second.
//
// Both semidiameters come from the interpolated vector's length at each sample (the sun's vector
// is in AU). Review 2026-09-27: the sun's used to be held at its noon value, but it changes up
// to 7.7e-5 deg in a day (not < 1e-6): near the poles, where the sun's altitude crawls, that
// moved sunrise/sunset up to 10 s off direct evaluation (89.9 N; 3 s at 78 S) - 8 of 57,126
// events over 12 places x 2 years. With the per-sample value: none off by more than 1 s.

const int DAY_NODES = ASTRO_DAY_NODES;
const int64_t DAY_NODE_STEP = 10800;
const double SIDEREAL_DEG_PER_S = 360.98564736629 / 86400.0;
static_assert(ASTRO_DAY_NODES == 9, "nine 3-hour nodes span t0 .. t0 + 24 h (docs/almanac.md)");

/* The scan's table is astro.h's AstroBodyDay (public so that AstroDayJob, which carries it, can
 * live in the caller's memory between slices). */
typedef AstroBodyDay BodyDay;

void bodyDayCommon(int64_t t0, double lat, double lon, double eqeq, BodyDay* b) {
  b->t0 = t0;
  b->lst0 = lstDeg(t0, lon, eqeq);
  b->sinLat = sind(lat);
  b->cosLat = cosd(lat);
  double gclat = lat - 0.1924 * sind(2.0 * lat);
  double rho = 0.99833 + 0.00167 * cosd(2.0 * lat);
  b->obsXY = rho * cosd(gclat);
  b->obsZ = rho * sind(gclat);
}

// One node of the theory, k = 0..8: the body's geocentric equatorial vector at t0 + k*3h (and the
// equation of the equinoxes at the middle one, noon). noinline: keeps SunEq/MoonGeo out of the
// callers' frames, which already hold the ~400-byte AstroDayJob (the ESP32 loop task has 8 KB of
// stack; -fstack-usage with the ESP32 toolchain put astroSunDay at 400 B before the job existed).
__attribute__((noinline)) void dayNode(AstroDayJob* j, int k) {
  const int64_t t = j->b.t0 + k * DAY_NODE_STEP;
  if (j->body == ASTRO_BODY_MOON) {
    MoonGeo g;
    moonGeo(t, &g, true);
    j->b.xyz[k][0] = g.x;
    j->b.xyz[k][1] = g.y;
    j->b.xyz[k][2] = g.z;
    if (k == DAY_NODES / 2) j->eqeq = g.fr.dpsi * cosd(g.fr.eps);
  } else {
    SunEq s;
    sunEq(t, &s);
    double cd = s.R * cosd(s.dec);
    j->b.xyz[k][0] = cd * cosd(s.ra);
    j->b.xyz[k][1] = cd * sind(s.ra);
    j->b.xyz[k][2] = s.R * sind(s.dec);
    if (k == DAY_NODES / 2) j->eqeq = s.fr.dpsi * cosd(s.fr.eps);
  }
}

// The functions whose zero crossings are the day's events, from one sample of one body.
struct DayF {
  double limb;    // centre alt + 0.5667 + semidiameter: rise/set (upper limb, 34' refraction)
  double civil;   // centre alt + 6: civil dawn/dusk (the sun's)
  double ha;      // hour angle, (-180, 180]: - to + = transit / noon
  double ha180;   // hour angle - 180, same range: - to + = underfoot
};
enum { C_LIMB, C_CIVIL, C_HA, C_HA180 };

void bodySample(const BodyDay* b, int64_t t, DayF* f) {
  // the quadratic through the nearest node j (clamped to 1..7) and its neighbours
  double sn = (double)(t - b->t0) / (double)DAY_NODE_STEP;
  int j = (int)floor(sn + 0.5);
  if (j < 1) j = 1;
  if (j > DAY_NODES - 2) j = DAY_NODES - 2;
  double u = sn - j;
  double p[3];
  for (int i = 0; i < 3; i++) {
    double fm = b->xyz[j - 1][i], f0 = b->xyz[j][i], fp = b->xyz[j + 1][i];
    p[i] = f0 + u * (fp - fm) * 0.5 + u * u * (fp - 2.0 * f0 + fm) * 0.5;
  }
  // LST moves at the sidereal rate; the GMST formula's T^2 term changes < 1e-8 deg in a day
  double lst = b->lst0 + SIDEREAL_DEG_PER_S * (double)(t - b->t0);
  double sL = sind(lst), cL = cosd(lst);
  double x = p[0], y = p[1], z = p[2];
  if (b->isMoon) {                  // topocentric by vectors
    x -= b->obsXY * cL;
    y -= b->obsXY * sL;
    z -= b->obsZ;
  }
  double r = sqrt(x * x + y * y + z * z);
  double a = x * cL + y * sL;       // r cos(dec) cos(H)
  double h = x * sL - y * cL;       // r cos(dec) sin(H), H = LST - RA
  double alt = asin(clamp1((b->sinLat * z + b->cosLat * a) / r)) * R2D;
  double ha = atan2(h, a) * R2D;
  double sd = b->isMoon ? asin(clamp1(0.272481 / r)) * R2D : 0.26656 / r;
  f->limb = alt + 0.5667 + sd;
  f->civil = alt + 6.0;
  f->ha = ha;
  f->ha180 = ha > 0.0 ? ha - 180.0 : ha + 180.0;
}

double pick(const DayF* f, int c) {
  switch (c) {
    case C_LIMB:  return f->limb;
    case C_CIVIL: return f->civil;
    case C_HA:    return f->ha;
    default:      return f->ha180;
  }
}

// Did component c cross zero between two samples, upward (dir > 0) or downward? Hour angles
// wrap at +-180 (a descending jump): the < 90 guard keeps a wrap from ever counting.
bool crossed(double fa, double fb, int dir, int c) {
  if (c == C_HA || c == C_HA180) {
    if (fabs(fb - fa) >= 90.0) return false;
  }
  return dir > 0 ? (fa < 0.0 && fb >= 0.0) : (fa >= 0.0 && fb < 0.0);
}

// Bisect a bracketed crossing to 1 s. Returns the first whole second at or past the crossing:
// the smallest t in (a, b] where the function is on the far side (>= 0 rising, < 0 setting).
int64_t refine(const BodyDay* body, int c, int dir, int64_t a, int64_t b) {
  while (b - a > 1) {
    int64_t m = a + (b - a) / 2;
    DayF f;
    bodySample(body, m, &f);
    double v = pick(&f, c);
    bool past = dir > 0 ? (v >= 0.0) : (v < 0.0);
    if (past) b = m; else a = m;
  }
  return b;
}

// One event being looked for in the day scan.
struct EventSpec { int c; int dir; int64_t* out; };

// The events a job looks for, in the order the one-call functions always looked: sun dawn, rise,
// noon, set, dusk; moon rise, set, transit, underfoot. `out` points into the job.
int jobEvents(AstroDayJob* j, EventSpec* ev) {
  if (j->body == ASTRO_BODY_MOON) {
    ev[0].c = C_LIMB;  ev[0].dir = +1; ev[0].out = &j->ev[0];   // rise
    ev[1].c = C_LIMB;  ev[1].dir = -1; ev[1].out = &j->ev[1];   // set
    ev[2].c = C_HA;    ev[2].dir = +1; ev[2].out = &j->ev[2];   // transit
    ev[3].c = C_HA180; ev[3].dir = +1; ev[3].out = &j->ev[3];   // underfoot
    return 4;
  }
  ev[0].c = C_CIVIL; ev[0].dir = +1; ev[0].out = &j->ev[0];     // dawn
  ev[1].c = C_LIMB;  ev[1].dir = +1; ev[1].out = &j->ev[1];     // rise
  ev[2].c = C_HA;    ev[2].dir = +1; ev[2].out = &j->ev[2];     // noon
  ev[3].c = C_LIMB;  ev[3].dir = -1; ev[3].out = &j->ev[3];     // set
  ev[4].c = C_CIVIL; ev[4].dir = -1; ev[4].out = &j->ev[4];     // dusk
  return 5;
}

void dayFStore(const DayF* f, double* p) { p[0] = f->limb; p[1] = f->civil; p[2] = f->ha; p[3] = f->ha180; }
void dayFLoad(const double* p, DayF* f) { f->limb = p[0]; f->civil = p[1]; f->ha = p[2]; f->ha180 = p[3]; }

enum { JOB_NODES = 0, JOB_FIRST, JOB_SCAN, JOB_DONE };

/* ONE unit of the day job. The day scan: [t0, t0 + 86400] in 5-minute samples; the first bracket
 * of each event refined by bisection to 1 s; only events strictly before t0 + 86400 kept.
 *
 * The first bracket opens one second early, (t0 - 1, t0 + 300]: an event whose instant is t0
 * itself (its crossing in (t0 - 1, t0]) belongs to this day, and the day before stops short of it
 * (t < end). Review 2026-09-27: with the first sample at t0 such an event was on the far side
 * already and lost from BOTH days - 530 of 532 events asked for from their own instant.
 *
 * The units are the loop bodies of the one-pass version this replaced (the nodes, then the
 * first sample, then one sample per 5-minute step), each carrying its state in the job: the
 * same operations on the same doubles in the same order, however the calls are cut. */
void jobUnit(AstroDayJob* j) {
  const int64_t t0 = j->b.t0, end = t0 + 86400;
  switch (j->stage) {
  case JOB_NODES:
    dayNode(j, j->node);
    j->node++;
    if (j->node >= DAY_NODES) {
      j->b.isMoon = (j->body == ASTRO_BODY_MOON);
      bodyDayCommon(t0, j->lat, j->lon, j->eqeq, &j->b);
      j->stage = JOB_FIRST;
    }
    break;
  case JOB_FIRST: {
    DayF f;
    bodySample(&j->b, t0 - 1, &f);
    dayFStore(&f, j->prev);
    j->scanA = t0 - 1;
    j->scanB = t0 + DAY_SCAN_STEP;
    j->stage = JOB_SCAN;
    break;
  }
  case JOB_SCAN: {
    DayF prev, cur;
    dayFLoad(j->prev, &prev);
    bodySample(&j->b, j->scanB, &cur);
    EventSpec ev[5];
    const int nev = jobEvents(j, ev);
    for (int i = 0; i < nev; i++) {
      if (*ev[i].out) continue;
      if (crossed(pick(&prev, ev[i].c), pick(&cur, ev[i].c), ev[i].dir, ev[i].c)) {
        int64_t t = refine(&j->b, ev[i].c, ev[i].dir, j->scanA, j->scanB);
        if (t < end) *ev[i].out = t;
      }
    }
    dayFStore(&cur, j->prev);
    j->scanA = j->scanB;
    j->scanB += DAY_SCAN_STEP;
    if (j->scanB > end) j->stage = JOB_DONE;
    break;
  }
  default:
    break;
  }
}

// ---- angle searches (moon phases, seasons) -----------------------------------------------------

typedef double (*AngleFn)(int64_t t);   // an angle that increases with time, deg

// Elongation lon_m - lon_s, [0, 360): geocentric apparent longitudes (the nutation cancels).
double moonElong(int64_t t) {
  MoonGeo g;
  moonGeo(t, &g, false);
  SunEq s;
  sunEq(t, &s);
  return rev(g.lon - s.lambda);
}

double sunLambda(int64_t t) {
  SunEq s;
  sunEq(t, &s);
  return s.lambda;
}

// The instant the angle passes `target`: g(t) = wrap180(fn(t) - target) goes from < 0 to >= 0.
// dir > 0: the first such instant strictly after `from`; dir < 0: the last at or before `from`.
// Returned as the first whole second at or past the crossing.
//
// Far from the target it JUMPS by (distance left) / maxRate - which cannot pass the target,
// since the angle moves slower than maxRate - and within one `step` it steps by `step` and looks
// for the sign change, then narrows the bracket to 1 s. Same answer as stepping `step` at a time
// from `from` and bisecting, ~10 positions instead of up to ~135.
int64_t angleSearch(AngleFn fn, double target, double maxRate, int64_t step, int64_t from, int dir) {
  int64_t t = from;
  double g = wrap180(fn(t) - target);
  for (int it = 0; it < 400; it++) {
    double dist = dir > 0 ? rev(-g) : rev(g);   // degrees still to go / gone since the crossing
    double jump = dist / maxRate * 86400.0;
    int64_t dt = jump > (double)step ? (int64_t)jump : step;
    int64_t tn = dir > 0 ? t + dt : t - dt;
    double gn = wrap180(fn(tn) - target);
    // (lo, hi) = the pair in time order
    int64_t lo = dir > 0 ? t : tn, hi = dir > 0 ? tn : t;
    double glo = dir > 0 ? g : gn, ghi = dir > 0 ? gn : g;
    if (glo < 0.0 && ghi >= 0.0 && ghi - glo < 90.0) {
      // Down to 1 s by the Illinois method (regula falsi that halves a stale end) on whole
      // seconds, bisection after 8 tries: the answer is the one second where g turns >= 0 - the
      // same second plain bisection finds (g is monotonic over one step) - in ~5 evaluations
      // instead of ~15.
      double flo = glo, fhi = ghi;
      int side = 0, tries = 0;
      while (hi - lo > 1) {
        int64_t m;
        if (tries++ < 8) {
          m = lo + (int64_t)ceil((double)(hi - lo) * (-flo) / (fhi - flo));
          if (m <= lo) m = lo + 1;
          if (m >= hi) m = hi - 1;
        } else {
          m = lo + (hi - lo) / 2;
        }
        double gm = wrap180(fn(m) - target);
        if (gm >= 0.0) {
          hi = m;
          fhi = gm;
          if (side > 0) flo *= 0.5;
          side = 1;
        } else {
          lo = m;
          flo = gm;
          if (side < 0) fhi *= 0.5;
          side = -1;
        }
      }
      return hi;
    }
    t = tn;
    g = gn;
  }
  return 0;   // not reached for a real angle; a NaN input lands here
}

int phaseIndex(double E) {
  if (E < 12.0 || E >= 348.0) return 0;
  if (E < 78.0) return 1;
  if (E < 102.0) return 2;
  if (E < 168.0) return 3;
  if (E < 192.0) return 4;
  if (E < 258.0) return 5;
  if (E < 282.0) return 6;
  return 7;
}

}  // namespace

// ---- public API ----------------------------------------------------------------------------------

double astroJd(int64_t unixSec) {
  return (double)unixSec / 86400.0 + 2440587.5;
}

void astroSunPos(int64_t t, double lat, double lon, double* altDeg, double* azDeg) {
  SunEq s;
  sunEq(t, &s);
  horizontal(lstDeg(t, lon, s.fr.dpsi * cosd(s.fr.eps)) - s.ra, s.dec, lat, altDeg, azDeg);
}

void astroMoonPos(int64_t t, double lat, double lon, double* altDeg, double* azDeg) {
  MoonTopo m;
  moonTopo(t, lat, lon, &m);
  if (altDeg) *altDeg = m.alt;
  if (azDeg) *azDeg = m.az;
}

double astroSunSemidiameter(int64_t t) {
  SunEq s;
  sunEq(t, &s);
  return 0.26656 / s.R;
}

double astroMoonSemidiameter(int64_t t, double lat, double lon) {
  MoonTopo m;
  moonTopo(t, lat, lon, &m);
  return m.sd;
}

double astroMoonDistanceKm(int64_t t) {
  MoonGeo g;
  moonGeo(t, &g, true);
  return g.distKm;
}

void astroMoonEcliptic(int64_t t, double* lonDeg, double* latDeg, double* distKm) {
  MoonGeo g;
  moonGeo(t, &g, true);
  if (lonDeg) *lonDeg = g.lonGeom;
  if (latDeg) *latDeg = g.lat;
  if (distKm) *distKm = g.distKm;
}

double astroDeltaT() {
  return DELTA_T;
}

void astroDayJobStart(AstroDayJob* j, int body, int64_t t0, double lat, double lon) {
  memset(j, 0, sizeof(*j));
  j->body = (body == ASTRO_BODY_MOON) ? ASTRO_BODY_MOON : ASTRO_BODY_SUN;
  j->b.t0 = t0;
  j->lat = lat;
  j->lon = lon;
  j->stage = JOB_NODES;
}

bool astroDayJobRun(AstroDayJob* j, AstroMoreFn more, void* ctx) {
  bool first = true;
  while (j->stage != JOB_DONE) {
    if (!first && more && !more(ctx)) {
      return false;
    }
    first = false;
    jobUnit(j);
  }
  return true;
}

bool astroDayJobDone(const AstroDayJob* j) {
  return j->stage == JOB_DONE;
}

void astroDayJobSun(const AstroDayJob* j, AstroSunDay* out) {
  out->dawn = j->ev[0];
  out->rise = j->ev[1];
  out->noon = j->ev[2];
  out->set = j->ev[3];
  out->dusk = j->ev[4];
}

void astroDayJobMoon(const AstroDayJob* j, AstroMoonDay* out) {
  out->rise = j->ev[0];
  out->set = j->ev[1];
  out->transit = j->ev[2];
  out->under = j->ev[3];
}

void astroSunDay(int64_t t0, double lat, double lon, AstroSunDay* out) {
  AstroDayJob j;
  astroDayJobStart(&j, ASTRO_BODY_SUN, t0, lat, lon);
  astroDayJobRun(&j, NULL, NULL);
  astroDayJobSun(&j, out);
}

void astroMoonDay(int64_t t0, double lat, double lon, AstroMoonDay* out) {
  AstroDayJob j;
  astroDayJobStart(&j, ASTRO_BODY_MOON, t0, lat, lon);
  astroDayJobRun(&j, NULL, NULL);
  astroDayJobMoon(&j, out);
}

void astroMoonPhaseWith(int64_t t, int64_t prevNew, AstroMoonPhase* out) {
  MoonGeo g;
  moonGeo(t, &g, true);
  SunEq s;
  sunEq(t, &s);
  double E = rev(g.lon - s.lambda);
  double psi = acos(clamp1(cosd(g.lat) * cosd(g.lon - s.lambda))) * R2D;   // Meeus 48.2
  double Rs = s.R * AU_KM;
  double i = atan2(Rs * sind(psi), g.distKm - Rs * cosd(psi));              // Meeus 48.3
  out->elong = E;
  out->sep = (E > 0.0 && E < 180.0) ? psi : -psi;
  out->illum = (1.0 + cos(i)) / 2.0;
  out->phase = phaseIndex(E);
  out->ageDays = prevNew ? (double)(t - prevNew) / 86400.0 : 0.0;
}

void astroMoonPhaseAt(int64_t t, AstroMoonPhase* out) {
  astroMoonPhaseWith(t, astroPrevMoonPhase(t, 0), out);
}

const char* astroPhaseName(int phase) {
  static const char* const NAMES[8] = {
    "New", "Waxing crescent", "First quarter", "Waxing gibbous",
    "Full", "Waning gibbous", "Last quarter", "Waning crescent",
  };
  return (phase >= 0 && phase < 8) ? NAMES[phase] : "?";
}

int64_t astroNextMoonPhase(int64_t from, int quarter) {
  return angleSearch(moonElong, 90.0 * (quarter & 3), MOON_ELONG_MAX_RATE, 6 * 3600, from, +1);
}

int64_t astroPrevMoonPhase(int64_t from, int quarter) {
  return angleSearch(moonElong, 90.0 * (quarter & 3), MOON_ELONG_MAX_RATE, 6 * 3600, from, -1);
}

int64_t astroNextSeason(int64_t from, int which) {
  return angleSearch(sunLambda, 90.0 * (which & 3), SUN_LON_MAX_RATE, 86400, from, +1);
}

void astroLegalLight(const AstroSunDay* s, int rule, int64_t* first, int64_t* last) {
  int64_t f = 0, l = 0;
  if (rule == ASTRO_LEGAL_CIVIL) {
    f = s->dawn;
    l = s->dusk;
  } else {
    f = s->rise ? s->rise - 1800 : 0;
    l = s->set ? s->set + 1800 : 0;
  }
  if (first) *first = f;
  if (last) *last = l;
}

void astroSolunar(int64_t t0, const AstroSunDay* sun, const AstroMoonDay* moon, AstroSolunar* out) {
  memset(out, 0, sizeof(*out));
  // majors: overhead and underfoot; minors: rise and set - each pair in time order
  int64_t maj[2] = { moon->transit, moon->under };
  int64_t mnr[2] = { moon->rise, moon->set };
  for (int k = 0; k < 2; k++) {
    if (maj[k]) out->majorMid[out->nMajor++] = maj[k];
    if (mnr[k]) out->minorMid[out->nMinor++] = mnr[k];
  }
  if (out->nMajor == 2 && out->majorMid[1] < out->majorMid[0]) {
    int64_t x = out->majorMid[0]; out->majorMid[0] = out->majorMid[1]; out->majorMid[1] = x;
  }
  if (out->nMinor == 2 && out->minorMid[1] < out->minorMid[0]) {
    int64_t x = out->minorMid[0]; out->minorMid[0] = out->minorMid[1]; out->minorMid[1] = x;
  }

  double E = moonElong(t0 + 43200);
  double dNew = E < 180.0 ? E : 360.0 - E;
  double dFull = fabs(E - 180.0);
  double dist = dNew < dFull ? dNew : dFull;
  int rating = dist <= 12.0 ? 3 : dist <= 36.0 ? 2 : dist <= 60.0 ? 1 : 0;

  bool overlap = false;
  int64_t sunEv[2] = { sun->rise, sun->set };
  for (int s = 0; s < 2; s++) {
    if (!sunEv[s]) continue;
    for (int k = 0; k < out->nMajor; k++) {
      if (sunEv[s] >= out->majorMid[k] - 3600 && sunEv[s] <= out->majorMid[k] + 3600) overlap = true;
    }
    for (int k = 0; k < out->nMinor; k++) {
      if (sunEv[s] >= out->minorMid[k] - 1800 && sunEv[s] <= out->minorMid[k] + 1800) overlap = true;
    }
  }
  if (overlap) rating++;
  out->rating = rating > 4 ? 4 : rating;
}

const char* astroSolunarRatingName(int rating) {
  static const char* const NAMES[5] = { "Poor", "Fair", "Good", "Very good", "Excellent" };
  return (rating >= 0 && rating < 5) ? NAMES[rating] : "?";
}

int64_t astroDaysFromCivil(int y, int m, int d) {
  // Howard Hinnant's days_from_civil (proleptic Gregorian)
  int64_t yy = (int64_t)y - (m <= 2 ? 1 : 0);
  int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  int64_t yoe = yy - era * 400;
  int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void astroCivil(int64_t localUnix, int* y, int* m, int* d, int* wday, int* yday) {
  int64_t days = floorDiv(localUnix, 86400);
  // Howard Hinnant's civil_from_days
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yr = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int dd = (int)(doy - (153 * mp + 2) / 5 + 1);
  int mm = (int)(mp < 10 ? mp + 3 : mp - 9);
  if (mm <= 2) yr++;
  if (y) *y = (int)yr;
  if (m) *m = mm;
  if (d) *d = dd;
  if (wday) *wday = floorMod7(days + 4);                       // 1970-01-01 was a Thursday
  if (yday) *yday = (int)(days - astroDaysFromCivil((int)yr, 1, 1) + 1);
}

int astroIsoWeek(int y, int m, int d) {
  int64_t days = astroDaysFromCivil(y, m, d);
  int wd = floorMod7(days + 3) + 1;                              // Monday = 1 .. Sunday = 7
  int yday = (int)(days - astroDaysFromCivil(y, 1, 1) + 1);
  int w = (yday - wd + 10) / 7;
  if (w < 1) {
    // the last week of the previous year: the week holding its December 28
    int64_t dec28 = astroDaysFromCivil(y - 1, 12, 28);
    int wd28 = floorMod7(dec28 + 3) + 1;
    int yd28 = (int)(dec28 - astroDaysFromCivil(y - 1, 1, 1) + 1);
    return (yd28 - wd28 + 10) / 7;
  }
  int64_t dec28 = astroDaysFromCivil(y, 12, 28);
  int wd28 = floorMod7(dec28 + 3) + 1;
  int yd28 = (int)(dec28 - astroDaysFromCivil(y, 1, 1) + 1);
  if (w > (yd28 - wd28 + 10) / 7) return 1;
  return w;
}

void astroUsDst(int year, int* marchDay, int* novDay) {
  int wMar1 = floorMod7(astroDaysFromCivil(year, 3, 1) + 4);    // 0 = Sunday
  int wNov1 = floorMod7(astroDaysFromCivil(year, 11, 1) + 4);
  if (marchDay) *marchDay = 1 + (7 - wMar1) % 7 + 7;
  if (novDay) *novDay = 1 + (7 - wNov1) % 7;
}
