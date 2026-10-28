/*
 * test_geogrid.cpp - geo_grid.cpp against independent ground truth: every
 * ALM_GRID row of tests/vectors_almanac.h (the `utm` package for zone, band,
 * easting, northing; the `mgrs` package - GeoTrans-derived - for the MGRS
 * string), plus the degrees-and-minutes formatter and the refusals.
 *
 * The MGRS comparison: the vectors are the mgrs package's run-together form
 * ("01CDM4200418039"); ours is spaced and writes the zone without a leading
 * zero ("1C DM 42004 18039"), as docs/almanac.md shows it. The test removes
 * our spaces and the vector's leading zone zero - every letter and digit is
 * still compared exactly.
 */

#include "../WiPhone/geo_grid.h"
#include "vectors_almanac.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

/* Quiet per-row checks (the vector table is long): only failures print. */
#define CHECKQ(cond, fmt, ...) do { \
    checks++; \
    if (!(cond)) { printf("  FAIL " fmt " (line %d)\n", __VA_ARGS__, __LINE__); failures++; } \
  } while (0)

static void stripSpaces(const char* in, char* out, size_t cap) {
  size_t k = 0;
  for (; *in && k + 1 < cap; in++) if (*in != ' ') out[k++] = *in;
  out[k] = '\0';
}

static bool ddmIs(double deg, bool isLat, const char* want) {
  char buf[24];
  geoFmtDdm(deg, isLat, buf, sizeof buf);
  if (strcmp(buf, want) != 0) {
    printf("    geoFmtDdm(%.9f, %s) = \"%s\", want \"%s\"\n", deg, isLat ? "lat" : "lon", buf, want);
    return false;
  }
  return true;
}

int main() {
  printf("test_geogrid\n");

  // ---- every vector row: UTM from `utm`, MGRS from `mgrs` ---------------------
  {
    const int rows = (int)(sizeof ALM_GRID / sizeof ALM_GRID[0]);
    double worstE = 0, worstN = 0;
    int worstERow = -1, worstNRow = -1, bad = 0;
    for (int i = 0; i < rows; i++) {
      const AlmGridVec& v = ALM_GRID[i];
      GeoUtm u;
      const int before = failures;
      CHECKQ(geoToUtm(v.lat, v.lon, &u), "row %d (%.7f, %.7f) refused", i, v.lat, v.lon);
      if (failures != before) { bad++; continue; }
      CHECKQ(u.zone == v.zone, "row %d (%.7f, %.7f) zone %d want %d", i, v.lat, v.lon, u.zone, v.zone);
      CHECKQ(u.band == v.band, "row %d (%.7f, %.7f) band %c want %c", i, v.lat, v.lon, u.band, v.band);
      CHECKQ(u.north == (v.lat >= 0), "row %d (%.7f, %.7f) hemisphere", i, v.lat, v.lon);
      const double de = fabs(u.e - v.e), dn = fabs(u.n - v.n);
      if (de > worstE) { worstE = de; worstERow = i; }
      if (dn > worstN) { worstN = dn; worstNRow = i; }
      /* 2 mm, not 1: the REFERENCE (the `utm` package) is itself up to ~0.92 mm off - an
       * independent n^6 Kruger series agrees with geo_grid.cpp to 2e-7 m (docs/almanac.md). At
       * 1 mm the margin was 39 um and belonged to the reference, not to this code. */
      CHECKQ(de <= 0.002, "row %d (%.7f, %.7f) easting %.4f want %.4f", i, v.lat, v.lon, u.e, v.e);
      CHECKQ(dn <= 0.002, "row %d (%.7f, %.7f) northing %.4f want %.4f", i, v.lat, v.lon, u.n, v.n);

      char mg[32], mine[32];
      CHECKQ(geoToMgrs(v.lat, v.lon, mg, sizeof mg), "row %d (%.7f, %.7f) MGRS refused", i, v.lat, v.lon);
      stripSpaces(mg, mine, sizeof mine);
      const char* want = v.mgrs;
      if (want[0] == '0') want++;                      // "01C..." -> "1C...": our zone has no leading zero
      CHECKQ(strcmp(mine, want) == 0, "row %d (%.7f, %.7f) MGRS \"%s\" want \"%s\"", i, v.lat, v.lon, mg, v.mgrs);
      if (failures != before) bad++;
    }
    printf("  %d grid rows, %d bad; worst |de| = %.6f m (row %d), worst |dn| = %.6f m (row %d)\n",
           rows, bad, worstE, worstERow, worstN, worstNRow);
    CHECK(bad == 0, "every ALM_GRID row: zone, band, e/n within 1 mm, MGRS exact");
  }

  // ---- MGRS format -------------------------------------------------------------
  {
    char mg[32];
    CHECK(geoToMgrs(47.49643, -121.79, mg, sizeof mg) && strcmp(mg, "10T ET 91134 61042") == 0,
          "MGRS spaced form 10T ET 91134 61042");
    CHECK(geoToMgrs(-79.99, -179.99, mg, sizeof mg) && strcmp(mg, "1C DM 42004 18039") == 0,
          "MGRS zone 1 has no leading zero");
    /* On the central meridian the easting is exactly 500000 (eta is exactly
     * 0 there): square W, 00000 - not 99999 of square V. */
    CHECK(geoToMgrs(78.0, 15.0, mg, sizeof mg) && strcmp(mg, "33X WG 00000 58369") == 0,
          "central meridian: 00000 in its own square");
    CHECK(geoToMgrs(47.0, 9.0, mg, sizeof mg) && strncmp(mg + 4, "N", 1) == 0 && strncmp(mg + 7, "00000", 5) == 0,
          "zone 32 CM at 47 N: column N, easting 00000");
    CHECK(geoToMgrs(0.0, 3.0, mg, sizeof mg) && strcmp(mg, "31N EA 00000 00000") == 0,
          "equator on a central meridian: 31N EA 00000 00000");
    CHECK(geoToMgrs(-0.0, 3.0, mg, sizeof mg) && strcmp(mg, "31N EA 00000 00000") == 0,
          "-0.0 latitude is north (band N, northing 0)");
    CHECK(geoToMgrs(47.49643, -121.79, mg, 20), "cap 20 is enough");
    CHECK(strlen(mg) == 18, "MGRS is 18 chars");
    mg[0] = 'x';
    CHECK(!geoToMgrs(47.49643, -121.79, mg, 19) && mg[0] == '\0', "cap 19 refused, out cleared");
    CHECK(!geoToMgrs(47.49643, -121.79, NULL, 32), "NULL out refused");
  }

  // ---- range: 80 S <= lat < 84 N, no UPS -----------------------------------------
  {
    GeoUtm u;
    char mg[32];
    CHECK(!geoToUtm(84.0, 10.0, &u), "UTM refuses 84.0 N");
    CHECK(!geoToMgrs(84.0, 10.0, mg, sizeof mg) && mg[0] == '\0', "MGRS refuses 84.0 N");
    CHECK(!geoToUtm(-80.5, 10.0, &u), "UTM refuses 80.5 S");
    CHECK(!geoToMgrs(-80.5, 10.0, mg, sizeof mg), "MGRS refuses 80.5 S");
    CHECK(!geoToUtm(90.0, 0.0, &u) && !geoToUtm(-90.0, 0.0, &u), "UTM refuses the poles");
    CHECK(geoToUtm(-80.0, 10.0, &u) && u.band == 'C' && !u.north, "80.0 S is band C");
    CHECK(geoToUtm(83.9999, 10.0, &u) && u.band == 'X' && u.zone == 33, "83.9999 N is 33X");
    CHECK(geoToUtm(71.9999, 10.0, &u) && u.band == 'W' && u.zone == 32, "71.9999 N is 32W (no Svalbard rule)");
    CHECK(geoToUtm(64.0, 5.0, &u) && u.zone == 31, "64.0 N at 5 E leaves the Norway rule (31W)");
    CHECK(geoToUtm(55.9999, 5.0, &u) && u.zone == 31, "55.9999 N at 5 E is 31U");
    CHECK(geoToUtm(60.0, 11.9999, &u) && u.zone == 32, "Norway 32V runs to 12 E");
    CHECK(geoToUtm(60.0, 12.0, &u) && u.zone == 33, "12 E at 60 N is 33V");
    CHECK(geoToUtm(78.0, 42.0, &u) && u.zone == 38, "42 E at 78 N is 38X (past Svalbard 37X)");
    CHECK(geoToUtm(78.0, -0.0001, &u) && u.zone == 30, "just west of 0 at 78 N is 30X");
    const double nan = NAN;
    CHECK(!geoToUtm(nan, 0.0, &u) && !geoToUtm(0.0, nan, &u), "NaN refused");
    CHECK(!geoToUtm(0.0, HUGE_VAL, &u), "infinite longitude refused");
    CHECK(!geoToUtm(47.0, -122.0, NULL), "NULL out refused");
    GeoUtm a, b;
    CHECK(geoToUtm(47.0, 180.0, &a) && geoToUtm(47.0, -180.0, &b) && a.zone == 1 &&
          a.e == b.e && a.n == b.n, "180 E wraps to zone 1 = -180");
    CHECK(geoToUtm(47.0, 238.0, &a) && geoToUtm(47.0, -122.0, &b) && a.zone == b.zone &&
          fabs(a.e - b.e) < 1e-6 && fabs(a.n - b.n) < 1e-6, "238 E wraps to -122");
    CHECK(geoToUtm(47.0, -482.0, &a) && a.zone == b.zone && fabs(a.e - b.e) < 1e-6,
          "-482 wraps to -122");
  }

  // ---- the series itself, to a micrometre ----------------------------------------
  /* The vectors hold e/n to 1 mm, which cannot see ANY n^4 term (each is 1e-6..4e-5 m):
   * alpha4 set to 0, or 41/180 n^4 dropped from alpha1, passed every check above. On a
   * central meridian eta is exactly 0, so e = 500000 and n = k0 * (meridian arc) exactly,
   * and the arc is a plain integral: a(1-e^2) / (1 - e^2 sin^2 t)^1.5 from 0 to phi. The
   * values below are that integral by 20-point Gauss-Legendre quadrature (16 and 32
   * panels agree to the last digit, composite Simpson to 2e-9 m) in Python - no series,
   * nothing shared with geo_grid.cpp. The n^4 series is ~2e-7 m off it; 1e-6 m holds
   * alpha1..alpha4 and A to the order the spec asks for. */
  {
    struct Arc { double lat, lon, n; };
    const Arc arcs[] = {
      {  5.0, -123.0,  552664.296877937 }, { 15.0, -123.0, 1658325.993564785 },
      { 25.0, -123.0, 2764947.747478365 }, { 35.0, -123.0, 3873043.064534261 },
      { 47.5, -123.0, 5260729.733077291 }, { 55.0, -123.0, 6094791.420998934 },
      { 65.0, -123.0, 7208454.581661082 }, { 75.0, -123.0, 8323606.812245434 },
      { 83.5, -123.0, 9272275.871019397 },
      { -33.0, 147.0, 6348713.056040317 }, { -79.5, 147.0, 1174220.967042636 },  // south: 1e7 - k0*arc
    };
    double worst = 0;
    int bad = 0;
    for (size_t i = 0; i < sizeof arcs / sizeof arcs[0]; i++) {
      GeoUtm u;
      const bool ok = geoToUtm(arcs[i].lat, arcs[i].lon, &u);
      const double dn = ok ? fabs(u.n - arcs[i].n) : 1e9;
      if (dn > worst) worst = dn;
      CHECKQ(ok && u.e == 500000.0 && dn <= 1e-6, "arc lat %.1f: e %.9f n %.9f want 500000 / %.9f",
             arcs[i].lat, ok ? u.e : 0.0, ok ? u.n : 0.0, arcs[i].n);
      if (!(ok && u.e == 500000.0 && dn <= 1e-6)) bad++;
    }
    printf("  meridian arc: worst |dn| = %.3g m\n", worst);
    CHECK(bad == 0, "central-meridian northing = k0 * quadrature arc within 1e-6 m (11 latitudes)");
  }

  // ---- the MGRS truncation epsilon (docs/almanac.md, decision 1) ----------------
  /* Two 1e-7-grid points whose easting / northing computes ~5e-7 m UNDER a metre line
   * (found by search; an n^6 series puts them there too, 4.8e-7 and 6.5e-7 under): the
   * documented +1e-6 m takes them to the next metre. Without it they read one metre less. */
  {
    GeoUtm u;
    char mg[32];
    CHECK(geoToUtm(47.4812768, -121.8570712, &u) && u.e > 586108.0 - 9e-7 && u.e < 586108.0 - 1e-7,
          "epsilon point E: easting just under 586108");
    CHECK(geoToMgrs(47.4812768, -121.8570712, mg, sizeof mg) && strcmp(mg, "10T ET 86108 59281") == 0,
          "epsilon point E: MGRS easting 86108, not 86107");
    CHECK(geoToUtm(47.4006819, -121.8234543, &u) && u.n > 5250363.0 - 9e-7 && u.n < 5250363.0 - 1e-7,
          "epsilon point N: northing just under 5250363");
    CHECK(geoToMgrs(47.4006819, -121.8234543, mg, sizeof mg) && strcmp(mg, "10T ET 88776 50363") == 0,
          "epsilon point N: MGRS northing 50363, not 50362");
  }

  // ---- lines a hair away: band, hemisphere and zone stay one consistent answer ------
  {
    GeoUtm u;
    char mg[32];
    /* (lat + 80) / 8 rounds UP onto a band line from within ~1.4e-14 below it. The band
     * must follow the same exact comparison as `north` and the zone rules. */
    CHECK(geoToUtm(-1e-300, 3.0, &u) && !u.north && u.band == 'M' && u.n == 10000000.0,
          "lat -1e-300: south, band M (not N beside a 10,000 km northing)");
    CHECK(geoToUtm(-5e-15, 3.0, &u) && !u.north && u.band == 'M', "lat -5e-15: band M");
    CHECK(geoToMgrs(-1e-300, 3.0, mg, sizeof mg) && strcmp(mg, "31M EA 00000 00000") == 0,
          "lat -1e-300 MGRS: 31M EA 00000 00000");
    CHECK(geoToUtm(nextafter(72.0, 0.0), 10.0, &u) && u.zone == 32 && u.band == 'W',
          "a hair under 72 N at 10 E is 32W (never 32X, which does not exist)");
    CHECK(geoToUtm(nextafter(56.0, 0.0), 5.0, &u) && u.zone == 31 && u.band == 'U',
          "a hair under 56 N at 5 E is 31U");
    CHECK(geoToUtm(nextafter(64.0, 0.0), 5.0, &u) && u.zone == 32 && u.band == 'V',
          "a hair under 64 N at 5 E is 32V (Norway)");
    CHECK(geoToUtm(nextafter(-8.0, -90.0), 5.0, &u) && u.band == 'L' &&
          geoToUtm(-8.0, 5.0, &u) && u.band == 'M', "-8 exactly is M, a hair south of it L");
    /* The largest lon under 180: (lon + 180) / 6 rounds to 60.0 - zone 60, not 61. */
    const double lonMax = nextafter(180.0, 0.0);
    CHECK(geoToUtm(47.0, lonMax, &u) && u.zone == 60 && fabs(u.e - 728069.565) < 0.001,
          "lon a hair under 180 is zone 60 (the clamp), easting at 180 E");
    CHECK(geoToMgrs(47.0, lonMax, mg, sizeof mg) && strncmp(mg, "60T Y", 5) == 0,
          "lon a hair under 180: MGRS 60T, zone 60's column set (Y)");
    mg[0] = 'x';
    CHECK(!geoToMgrs(NAN, 0.0, mg, sizeof mg) && mg[0] == '\0', "MGRS refuses NaN, out cleared");
    mg[0] = 'x';
    CHECK(!geoToMgrs(0.0, -HUGE_VAL, mg, sizeof mg) && mg[0] == '\0', "MGRS refuses -inf lon");
    CHECK(geoToUtm(47.0, 1e300, &u) && u.zone >= 1 && u.zone <= 60, "lon 1e300 wraps, no hang");
  }

  // ---- degrees + decimal minutes ----------------------------------------------
  {
    CHECK(ddmIs(47.4964300, true, "47 29.786N"), "47.49643 -> 47 29.786N");
    CHECK(ddmIs(-121.7868, false, "121 47.208W"), "-121.7868 -> 121 47.208W");
    CHECK(ddmIs(-33.8688, true, "33 52.128S"), "southern latitude S");
    CHECK(ddmIs(151.2093, false, "151 12.558E"), "eastern longitude E");
    CHECK(ddmIs(5.05, true, "5 03.000N"), "degrees without a leading zero, minutes padded");
    // The carry: 59.9996' rounds to 60.000' -> the next degree, 00.000.
    CHECK(ddmIs(47.0 + 59.9996 / 60.0, true, "48 00.000N"), "59.9996' carries to the next degree");
    CHECK(ddmIs(-(121.0 + 59.9996 / 60.0), false, "122 00.000W"), "carry west");
    CHECK(ddmIs(47.0 + 59.9994 / 60.0, true, "47 59.999N"), "59.9994' stays 59.999");
    CHECK(ddmIs(47.0 + 29.7855 / 60.0 + 1e-12, true, "47 29.786N"), "half-thousandth rounds up");
    CHECK(ddmIs(179.99999999, false, "180 00.000E"), "carry into 180");
    // Equator / prime meridian: zero, and values that ROUND to zero, are N / E.
    CHECK(ddmIs(0.0, true, "0 00.000N"), "equator is N");
    CHECK(ddmIs(0.0, false, "0 00.000E"), "prime meridian is E");
    CHECK(ddmIs(-0.0, true, "0 00.000N"), "-0.0 latitude is N");
    CHECK(ddmIs(-0.0, false, "0 00.000E"), "-0.0 longitude is E");
    CHECK(ddmIs(-0.000001, true, "0 00.000N"), "a hair south that rounds to zero is N");
    CHECK(ddmIs(-0.000001, false, "0 00.000E"), "a hair west that rounds to zero is E");
    CHECK(ddmIs(-0.0001, true, "0 00.006S"), "0.006' south is S");
    CHECK(ddmIs(-0.0001, false, "0 00.006W"), "0.006' west is W");
    CHECK(ddmIs(0.0001, false, "0 00.006E"), "0.006' east is E");
    CHECK(ddmIs(-180.0, false, "180 00.000W"), "-180 is 180 00.000W");
    CHECK(ddmIs(NAN, true, "--"), "NaN -> --");
    CHECK(ddmIs(400.0, false, "--"), "|deg| > 360 -> --");
    CHECK(ddmIs(360.0, false, "360 00.000E"), "|deg| = 360 is still formatted (the header's limit)");
    CHECK(ddmIs(-359.5, false, "359 30.000W"), "a value between 180 and 360 is formatted, not refused");
    CHECK(ddmIs(360.0000001, false, "--"), "just past 360 -> --");
    CHECK(ddmIs(-HUGE_VAL, true, "--"), "-inf -> --");
    // A cap too small never leaves a cut-off number ("121 4" reads as 121 deg 4').
    char small[12];
    memset(small, 'x', sizeof small);
    geoFmtDdm(-121.7868, false, small, 6);
    CHECK(small[0] == '\0', "cap 6 for an 11-char value writes \"\", not \"121 4\"");
    memset(small, 'x', sizeof small);
    geoFmtDdm(-121.7868, false, small, 11);
    CHECK(small[0] == '\0', "cap 11 (one short of the NUL) writes \"\"");
    geoFmtDdm(-121.7868, false, small, 12);
    CHECK(strcmp(small, "121 47.208W") == 0, "cap 12 fits the longest value");
    char five[5];
    geoFmtDdm(5.05, true, five, sizeof five);
    CHECK(five[0] == '\0', "cap 5 for \"5 03.000N\" writes \"\"");
    char two[2] = { 'x', 'x' };
    geoFmtDdm(NAN, true, two, sizeof two);
    CHECK(two[0] == '\0', "cap 2 for \"--\" writes \"\", not a lone \"-\"");
    char one[1] = { 'x' };
    geoFmtDdm(1.0, true, one, 1);
    CHECK(one[0] == '\0', "cap 1 writes only the NUL");
    geoFmtDdm(1.0, true, NULL, 16);                      // must not crash
    CHECK(true, "NULL out is ignored");
  }

  printf("test_geogrid: %d checks, %d failures\n", checks, failures);
  return failures;
}
