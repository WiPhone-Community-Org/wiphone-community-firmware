/*
 * test_units.cpp - units.cpp on the host.
 *
 * The metric half is checked against the SHIPPING code it replaces, linked in as the oracle:
 * meshPosFmtDist() (mesh_pos.cpp) byte for byte over a dense sweep, and mapScaleBar()
 * (map_tiles.cpp) plus the label app_maps.cpp's drawBottomStrip draws, over a sweep of scales.
 * Switching the map to units.cpp must not move a single pixel or character for a metric user.
 *
 * The US half is checked on every boundary the formats have, and by properties over sweeps:
 * the printed number read back is within its own rounding of the input, feet never print as
 * 1000ft, miles never as 10.0mi, the thousands commas are where they belong, and the scale
 * bar is the longest candidate that fits.
 */

#include "../WiPhone/units.h"
#include "../WiPhone/map_tiles.h"
#include "../WiPhone/mesh_pos.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

static char g_buf[128];

static const char* dist(double m, int u) { unitsFmtDist(m, u, g_buf, sizeof(g_buf)); return g_buf; }
static const char* alt(double m, int u, bool s) { unitsFmtAlt(m, u, s, g_buf, sizeof(g_buf)); return g_buf; }
static const char* spd(double v, int u) { unitsFmtSpeed(v, u, g_buf, sizeof(g_buf)); return g_buf; }

static bool is(const char* got, const char* want) {
  if (strcmp(got, want) != 0) {
    printf("    got \"%s\", want \"%s\"\n", got, want);
    return false;
  }
  return true;
}

// "1,352ft" / "+394ft" / "-115ft" / "0ft": digits grouped by 3 from the right, commas only there.
static bool usAltWellFormed(const char* s, long long* value) {
  const char* p = s;
  int sign = 1;
  if (*p == '+' || *p == '-') {
    sign = *p == '-' ? -1 : 1;
    p++;
  }
  const char* end = strstr(p, "ft");
  if (!end || strcmp(end, "ft") != 0 || end == p) {
    return false;
  }
  const int len = (int)(end - p);
  // groups: the first 1-3 digits, then ",ddd" repeated
  const int first = len % 4 == 0 ? 0 : len % 4;   // chars before the first comma
  if (first == 0 || first > 3) {
    return false;
  }
  long long v = 0;
  for (int i = 0; i < len; i++) {
    const bool commaPos = i >= first && (i - first) % 4 == 0;
    if (commaPos) {
      if (p[i] != ',') {
        return false;
      }
    } else {
      if (p[i] < '0' || p[i] > '9') {
        return false;
      }
      v = v * 10 + (p[i] - '0');
    }
  }
  if (len > 1 && p[0] == '0') {
    return false;
  }
  *value = sign * v;
  return true;
}

// Read a unitsFmtDist() string back to metres, and the half-width of its rounding.
static bool parseDist(const char* s, double* metres, double* halfStep) {
  char* end = NULL;
  const double v = strtod(s, &end);
  if (end == s) {
    return false;
  }
  const bool dec = strchr(s, '.') != NULL;
  if (strcmp(end, "ft") == 0) {
    *metres = v * UNITS_M_PER_FT;
    *halfStep = 0.5 * UNITS_M_PER_FT;
  } else if (strcmp(end, "mi") == 0) {
    *metres = v * UNITS_M_PER_MI;
    *halfStep = (dec ? 0.05 : 0.5) * UNITS_M_PER_MI;
  } else if (strcmp(end, "m") == 0) {
    *metres = v;
    *halfStep = 0.5;
  } else if (strcmp(end, "km") == 0) {
    *metres = v * 1000.0;
    *halfStep = (dec ? 0.05 : 0.5) * 1000.0;
  } else {
    return false;
  }
  return true;
}

// The label the map has always drawn beside the bar (app_maps.cpp, drawBottomStrip).
static void mapLabel(int barM, char* out, size_t cap) {
  if (barM >= 1000) {
    snprintf(out, cap, "%dkm", barM / 1000);
  } else {
    snprintf(out, cap, "%dm", barM);
  }
}

int main() {
  printf("test_units\n");

  // ── distance, metric: meshPosFmtDist exactly ─────────────────────────────────────────
  CHECK(is(dist(0.0, UNITS_METRIC), "0m"), "0 m");
  CHECK(is(dist(-0.0, UNITS_METRIC), "0m"), "negative zero is 0m");
  CHECK(is(dist(-5.0, UNITS_METRIC), "0m"), "a negative distance reads as 0");
  CHECK(is(dist(850.0, UNITS_METRIC), "850m"), "850m");
  CHECK(is(dist(999.49, UNITS_METRIC), "999m"), "999.49 m is 999m");
  CHECK(is(dist(999.5, UNITS_METRIC), "1.0km"), "999.5 m is 1.0km, never 1000m");
  CHECK(is(dist(1400.0, UNITS_METRIC), "1.4km"), "1.4km");
  CHECK(is(dist(9999.0, UNITS_METRIC), "10.0km"), "9999 m is meshPosFmtDist's 10.0km (kept)");
  CHECK(is(dist(10000.0, UNITS_METRIC), "10km"), "10000 m is 10km");
  CHECK(is(dist(12345.0, UNITS_METRIC), "12km"), "12km");
  CHECK(is(dist(850.0, 7), "850m"), "an unknown units value is metric");
  {
    // Byte for byte against the shipping formatter, in the 16-byte buffer the map uses.
    long bad = 0, n = 0;
    char a[16], b[16];
    for (double m = -3.0; m < 25000.0; m += 0.01) {
      meshPosFmtDist(m, a, sizeof(a));
      unitsFmtDist(m, UNITS_METRIC, b, sizeof(b));
      n++;
      if (strcmp(a, b) != 0) bad++;
    }
    for (double m = 1.0; m < 1e12; m *= 1.0007) {
      meshPosFmtDist(m, a, sizeof(a));
      unitsFmtDist(m, UNITS_METRIC, b, sizeof(b));
      n++;
      if (strcmp(a, b) != 0) bad++;
    }
    // 1e300 is 300 characters: compared whole, in a buffer that holds it.
    const double odd[] = { -0.0, 999.4999999999, 999.5, 9999.4999, 9999.5, 9999.9999, 1e300, -1e300,
                           NAN, INFINITY, -INFINITY };
    static char wa[512], wb[512];
    for (size_t i = 0; i < sizeof(odd) / sizeof(odd[0]); i++) {
      meshPosFmtDist(odd[i], wa, sizeof(wa));
      unitsFmtDist(odd[i], UNITS_METRIC, wb, sizeof(wb));
      n++;
      if (strcmp(wa, wb) != 0 || wb[0] == '\0') {
        printf("    %g: \"%s\" vs \"%s\"\n", odd[i], wa, wb);
        bad++;
      }
    }
    printf("    %ld distances against meshPosFmtDist\n", n);
    CHECK(bad == 0, "metric distance is byte-identical to meshPosFmtDist (incl. NaN, inf, 1e300)");
    /* Where the text does NOT fit, meshPosFmtDist cuts 1e297 km to "100000000000000" - fifteen
     * digits of a different number. unitsFmtDist writes nothing instead. */
    unitsFmtDist(1e300, UNITS_METRIC, b, sizeof(b));
    CHECK(is(b, ""), "a distance that does not fit the map's 16 bytes is \"\", never cut short");
  }

  // ── distance, US ─────────────────────────────────────────────────────────────────────
  const double FT = UNITS_M_PER_FT, MI = UNITS_M_PER_MI;
  CHECK(is(dist(0.0, UNITS_US), "0ft"), "0ft");
  CHECK(is(dist(-0.0, UNITS_US), "0ft"), "negative zero is 0ft");
  CHECK(is(dist(-100.0, UNITS_US), "0ft"), "a negative distance reads as 0ft");
  CHECK(is(dist(850 * FT, UNITS_US), "850ft"), "850ft");
  CHECK(is(dist(999.49 * FT, UNITS_US), "999ft"), "999.49 ft is 999ft");
  CHECK(is(dist(999.5001 * FT, UNITS_US), "0.2mi"), "999.5 ft is 0.2mi");
  CHECK(is(dist(999.96 * FT, UNITS_US), "0.2mi"), "999.96 ft is 0.2mi, never 1000ft");
  CHECK(is(dist(1.4 * MI, UNITS_US), "1.4mi"), "1.4mi");
  CHECK(is(dist(9.94 * MI, UNITS_US), "9.9mi"), "9.94 mi is 9.9mi");
  CHECK(is(dist(9.96 * MI, UNITS_US), "10mi"), "9.96 mi is 10mi, never 10.0mi");
  CHECK(is(dist(10.0 * MI, UNITS_US), "10mi"), "10mi");
  CHECK(is(dist(12.3 * MI, UNITS_US), "12mi"), "12mi");
  CHECK(is(dist(1e6 * MI, UNITS_US), "1000000mi"), "a huge distance is still a number");
  {
    char tiny[6];
    unitsFmtDist(850 * FT, UNITS_US, tiny, 6);
    CHECK(is(tiny, "850ft"), "a buffer that exactly fits (5 chars + NUL) is enough");
    unitsFmtDist(850 * FT, UNITS_US, tiny, 5);
    CHECK(is(tiny, ""), "one byte short: \"\", never \"850f\"");
    unitsFmtDist(12.3 * MI, UNITS_US, tiny, 4);
    CHECK(is(tiny, ""), "\"12mi\" in 4 bytes: \"\", never \"12m\" (a different unit)");
    unitsFmtDist(1.4 * MI, UNITS_METRIC, tiny, 5);
    CHECK(is(tiny, ""), "\"2.3km\" in 5 bytes: \"\", never \"2.3k\"");
    strcpy(tiny, "abc");
    unitsFmtDist(850 * FT, UNITS_US, tiny, 0);
    CHECK(is(tiny, "abc"), "cap 0 writes nothing");
    unitsFmtDist(850 * FT, UNITS_US, NULL, 16);
    CHECK(true, "NULL out is ignored");
  }
  {
    // Every US distance reads back within its own rounding, never 1000ft / 10.0mi, and the
    // printed value never goes down as the distance goes up.
    long bad = 0, n = 0;
    double prev = -1.0;
    for (double m = 0.0; m < 60000.0; m += 0.0731) {
      const char* s = dist(m, UNITS_US);
      double back = 0, half = 0;
      n++;
      if (!parseDist(s, &back, &half) || fabs(back - m) > half + 1e-9 ||
          strcmp(s, "1000ft") == 0 || strcmp(s, "10.0mi") == 0 || back < prev) {
        if (bad < 5) printf("    %.4f m -> \"%s\"\n", m, s);
        bad++;
      }
      prev = back;
    }
    printf("    %ld US distances read back\n", n);
    CHECK(bad == 0, "US distances: within rounding, monotonic, no 1000ft, no 10.0mi");
  }

  // ── altitude ─────────────────────────────────────────────────────────────────────────
  CHECK(is(alt(412.0, UNITS_METRIC, false), "412m"), "412m");
  CHECK(is(alt(412.0, UNITS_METRIC, true), "+412m"), "+412m with a sign");
  CHECK(is(alt(120.0, UNITS_METRIC, true), "+120m"), "+120m");
  CHECK(is(alt(-35.0, UNITS_METRIC, true), "-35m"), "-35m");
  CHECK(is(alt(-35.0, UNITS_METRIC, false), "-35m"), "a negative altitude keeps its '-' without withSign");
  CHECK(is(alt(0.0, UNITS_METRIC, true), "0m"), "0 is 0m, never +0m");
  CHECK(is(alt(-0.0, UNITS_METRIC, true), "0m"), "negative zero is 0m, never -0m");
  CHECK(is(alt(-0.0, UNITS_METRIC, false), "0m"), "negative zero, no sign: 0m");
  CHECK(is(alt(0.4, UNITS_METRIC, true), "0m"), "+0.4 rounds to 0m");
  CHECK(is(alt(-0.4, UNITS_METRIC, true), "0m"), "-0.4 rounds to 0m, never -0m");
  CHECK(is(alt(-0.49999, UNITS_METRIC, false), "0m"), "-0.49999 is 0m");
  CHECK(is(alt(0.5, UNITS_METRIC, true), "+1m"), "+0.5 rounds half away from zero: +1m");
  CHECK(is(alt(-0.5, UNITS_METRIC, true), "-1m"), "-0.5 rounds half away from zero: -1m");
  CHECK(is(alt(4392.0, UNITS_METRIC, false), "4392m"), "metric has no thousands comma (4392m)");
  CHECK(is(alt(-10994.0, UNITS_METRIC, false), "-10994m"), "-10994m");
  CHECK(is(alt(412.1, UNITS_US, false), "1,352ft"), "412.1 m is 1,352ft");
  CHECK(is(alt(120.0, UNITS_US, true), "+394ft"), "+120 m is +394ft");
  CHECK(is(alt(-35.0, UNITS_US, true), "-115ft"), "-35 m is -115ft");
  CHECK(is(alt(1000 * FT, UNITS_US, false), "1,000ft"), "1000 ft gets its comma: 1,000ft");
  CHECK(is(alt(999 * FT, UNITS_US, false), "999ft"), "999ft has none");
  CHECK(is(alt(-1000 * FT, UNITS_US, true), "-1,000ft"), "-1,000ft");
  CHECK(is(alt(0.5 * FT, UNITS_US, true), "+1ft"), "+0.5 ft is +1ft");
  CHECK(is(alt(-0.1, UNITS_US, true), "0ft"), "-0.1 m is 0ft");
  CHECK(is(alt(4392.0, UNITS_US, false), "14,409ft"), "4392 m is 14,409ft");
  CHECK(is(alt(1e6 * FT, UNITS_US, false), "1,000,000ft"), "1,000,000ft");
  CHECK(is(alt(-1234567 * FT, UNITS_US, true), "-1,234,567ft"), "-1,234,567ft");
  CHECK(is(alt(1e300, UNITS_US, true), "+1,000,000,000,000ft"), "a huge value is held at 1e12 ft");
  CHECK(is(alt(-1e300, UNITS_METRIC, false), "-1000000000000m"), "and at -1e12 m");
  CHECK(is(alt(NAN, UNITS_METRIC, true), "--"), "NaN is --");
  CHECK(is(alt(INFINITY, UNITS_US, false), "--"), "inf is --");
  CHECK(is(alt(-INFINITY, UNITS_US, true), "--"), "-inf is --");
  {
    char tiny[8];
    unitsFmtAlt(412.1, UNITS_US, false, tiny, 4);
    CHECK(is(tiny, ""), "\"1,352ft\" in 4 bytes is \"\", never \"1,3\"");
    unitsFmtAlt(412.1, UNITS_US, false, tiny, 8);
    CHECK(is(tiny, "1,352ft"), "exactly fitting (7 + NUL)");
    unitsFmtAlt(NAN, UNITS_US, false, tiny, 2);
    CHECK(is(tiny, ""), "\"--\" in 2 bytes is \"\", never \"-\" (a minus sign)");
    strcpy(tiny, "abc");
    unitsFmtAlt(412.1, UNITS_US, false, tiny, 0);
    CHECK(is(tiny, "abc"), "cap 0 writes nothing");
    unitsFmtAlt(412.1, UNITS_US, false, NULL, 8);
    CHECK(true, "NULL out is ignored");
  }
  {
    // Sweep: read back within half a unit, the sign rules, and (US) the commas.
    long bad = 0, n = 0;
    for (double m = -12000.0; m <= 12000.0; m += 0.173) {
      for (int u = 0; u < 2; u++) {
        for (int sg = 0; sg < 2; sg++) {
          const char* s = alt(m, u, sg != 0);
          const double want = u == UNITS_US ? m / FT : m;
          long long v = 0;
          bool ok;
          if (u == UNITS_US) {
            ok = usAltWellFormed(s, &v);
          } else {
            char* end = NULL;
            v = strtoll(s, &end, 10);
            ok = end && strcmp(end, "m") == 0 && !strchr(s, ',');
          }
          ok = ok && fabs((double)v - want) <= 0.5 + 1e-9;
          ok = ok && (v != 0 || (s[0] == '0'));                     // 0 is bare
          ok = ok && (v <= 0 || (sg ? s[0] == '+' : s[0] != '+'));   // '+' only when asked
          ok = ok && (v >= 0 || s[0] == '-');
          n++;
          if (!ok) {
            if (bad < 5) printf("    %.3f u%d s%d -> \"%s\"\n", m, u, sg, s);
            bad++;
          }
        }
      }
    }
    printf("    %ld altitudes read back\n", n);
    CHECK(bad == 0, "altitudes: within half a unit, bare 0, signs as asked, US commas grouped by 3");
  }

  // ── speed ────────────────────────────────────────────────────────────────────────────
  /* The exact conversions, pinned as numbers: the sweeps above convert with these same macros,
   * so a wrong constant would move the code and its check together. */
  CHECK(UNITS_M_PER_FT == 0.3048 && UNITS_M_PER_MI == 1609.344 && UNITS_MPS_PER_MPH == 0.44704 &&
        UNITS_M_PER_MI == 5280 * UNITS_M_PER_FT, "the conversions are the exact international ones");
  CHECK(is(spd(447.04, UNITS_US), "1000.0mph"), "447.04 m/s is 1000.0mph (0.447 would say 1000.1)");
  CHECK(is(spd(1000.0 / 36.0, UNITS_METRIC), "100.0km/h"), "27.78 m/s is 100.0km/h");
  CHECK(is(spd(0.889, UNITS_METRIC), "3.2km/h"), "0.889 m/s is 3.2km/h");
  CHECK(is(spd(0.89408, UNITS_US), "2.0mph"), "0.89408 m/s is 2.0mph");
  CHECK(is(spd(1.0, UNITS_METRIC), "3.6km/h"), "1 m/s is 3.6km/h");
  CHECK(is(spd(1.0, UNITS_US), "2.2mph"), "1 m/s is 2.2mph");
  CHECK(is(spd(27.7778, UNITS_METRIC), "100.0km/h"), "100.0km/h");
  CHECK(is(spd(0.0, UNITS_METRIC), "0.0km/h"), "0.0km/h");
  CHECK(is(spd(-0.0, UNITS_US), "0.0mph"), "negative zero is 0.0mph, never -0.0");
  CHECK(is(spd(-0.01, UNITS_METRIC), "0.0km/h"), "a tiny negative is 0.0, never -0.0");
  CHECK(is(spd(-3.0, UNITS_US), "0.0mph"), "a negative speed reads as 0");
  CHECK(is(spd(NAN, UNITS_METRIC), "--"), "NaN is --");
  CHECK(is(spd(INFINITY, UNITS_US), "--"), "inf is --");
  CHECK(is(spd(1e300, UNITS_METRIC), "1000000000.0km/h"), "a huge speed is held at 1e9");
  {
    char tiny[8];
    unitsFmtSpeed(0.889, UNITS_METRIC, tiny, 3);
    CHECK(is(tiny, ""), "a short buffer is \"\", never \"3.\"");
    unitsFmtSpeed(0.889, UNITS_METRIC, tiny, 8);
    CHECK(is(tiny, "3.2km/h"), "exactly fitting (7 + NUL)");
    unitsFmtSpeed(NAN, UNITS_METRIC, tiny, 2);
    CHECK(is(tiny, ""), "\"--\" in 2 bytes is \"\"");
  }

  // ── scale bar, metric: mapScaleBar + the map's label, exactly ────────────────────────
  {
    int px = -1;
    char lab[16];
    CHECK(unitsScaleBar(3.220972649878169, 100, UNITS_METRIC, &px, lab, sizeof(lab)) == 200 &&
          px == 62 && is(lab, "200m"), "Seattle z15: 200 m / 62 px, \"200m\"");
    CHECK(unitsScaleBar(mapMetersPerPixel(47.5, 18), 84, UNITS_METRIC, &px, lab, sizeof(lab)) == 20 &&
          px == 50 && is(lab, "20m"), "z18 at 47.5 N: 20 m / 50 px");
    CHECK(unitsScaleBar(40.0, 84, UNITS_METRIC, &px, lab, sizeof(lab)) == 2000 && px == 50 &&
          is(lab, "2km"), "2000 m is labelled 2km");
    CHECK(unitsScaleBar(1e-3, 84, UNITS_METRIC, &px, lab, sizeof(lab)) == 1 && px == 84 && is(lab, "1m"),
          "absurdly zoomed in: 1 m, held to the box (no int overflow where mapScaleBar has one)");
    CHECK(unitsScaleBar(1e-300, 84, UNITS_METRIC, &px, lab, sizeof(lab)) == 1 && px == 84,
          "1e-300 m/px: still 1 m / 84 px");
    CHECK(unitsScaleBar(1e9, 84, UNITS_METRIC, &px, lab, sizeof(lab)) == 1 && px == 1 && is(lab, "1m"),
          "absurdly zoomed out: 1 m / 1 px (mapScaleBar's fallback)");
    strcpy(lab, "junk");
    CHECK(unitsScaleBar(0.0, 84, UNITS_METRIC, &px, lab, sizeof(lab)) == 0 && px == 0 && lab[0] == '\0',
          "0 m/px: no bar, px 0, empty label");
    CHECK(unitsScaleBar(-1.0, 84, UNITS_US, &px, lab, sizeof(lab)) == 0 && px == 0 && lab[0] == '\0',
          "negative m/px: no bar");
    CHECK(unitsScaleBar(NAN, 84, UNITS_METRIC, &px, lab, sizeof(lab)) == 0 && px == 0, "NaN m/px: no bar");
    CHECK(unitsScaleBar(3.0, 0, UNITS_METRIC, &px, lab, sizeof(lab)) == 0 && px == 0, "maxPx 0: no bar");
    CHECK(unitsScaleBar(3.0, 84, UNITS_METRIC, NULL, NULL, 0) == 200, "barPx and label may be NULL");
  }
  {
    /* The sweep. mapScaleBar casts len/mpp to int unguarded, which is undefined past INT_MAX
     * (5e7 m / 0.0233 m/px): the sweep starts above that. The map itself never goes below
     * mapMetersPerPixel(85.05, 19) = 0.0258. */
    const int maxes[] = { 1, 2, 10, 50, 84, 100, 240, 1000 };
    long bad = 0, n = 0;
    for (double mpp = 0.03; mpp < 2e9; mpp *= 1.0013) {
      for (size_t k = 0; k < sizeof(maxes) / sizeof(maxes[0]); k++) {
        int pa = -1, pb = -1;
        char la[24], lb[24];
        const int ma = mapScaleBar(mpp, maxes[k], &pa);
        mapLabel(ma, la, sizeof(la));
        const int mb = unitsScaleBar(mpp, maxes[k], UNITS_METRIC, &pb, lb, sizeof(lb));
        n++;
        if (ma != mb || pa != pb || strcmp(la, lb) != 0) {
          if (bad < 5) printf("    %.6g/%d: %d %d \"%s\" vs %d %d \"%s\"\n", mpp, maxes[k], ma, pa, la, mb, pb, lb);
          bad++;
        }
      }
    }
    for (int z = 0; z <= MAP_ZOOM_MAX; z++) {
      const double lats[] = { 0.0, 30.0, 47.5, 60.0, 71.0, 85.0511287798066 };
      for (size_t i = 0; i < sizeof(lats) / sizeof(lats[0]); i++) {
        int pa = -1, pb = -1;
        char la[24], lb[24];
        const double mpp = mapMetersPerPixel(lats[i], z);
        const int ma = mapScaleBar(mpp, 84, &pa);
        mapLabel(ma, la, sizeof(la));
        const int mb = unitsScaleBar(mpp, 84, UNITS_METRIC, &pb, lb, sizeof(lb));
        n++;
        if (ma != mb || pa != pb || strcmp(la, lb) != 0) bad++;
      }
    }
    printf("    %ld scale bars against mapScaleBar\n", n);
    CHECK(bad == 0, "metric scale bar = mapScaleBar + the map's label, every scale and box, z0-z19");
  }

  // ── scale bar, US ────────────────────────────────────────────────────────────────────
  {
    int px = -1;
    char lab[16];
    CHECK(unitsScaleBar(3.22, 84, UNITS_US, &px, lab, sizeof(lab)) == 152 && px == 47 && is(lab, "500ft"),
          "z15-ish (3.22 m/px): 500ft / 47 px, 152 m");
    CHECK(unitsScaleBar(4.0, 84, UNITS_US, &px, lab, sizeof(lab)) == 305 && px == 76 && is(lab, "1000ft"),
          "4 m/px: 1000ft (no comma on the bar) / 76 px");
    CHECK(unitsScaleBar(2.0, 84, UNITS_US, &px, lab, sizeof(lab)) == 152 && px == 76 && is(lab, "500ft"),
          "2 m/px: 500ft");
    CHECK(unitsScaleBar(7.0, 100, UNITS_US, &px, lab, sizeof(lab)) == 610 && px == 87 && is(lab, "2000ft"),
          "7 m/px in 100: 2000ft / 87 px");
    CHECK(unitsScaleBar(10.0, 84, UNITS_US, &px, lab, sizeof(lab)) == 805 && px == 80 && is(lab, "0.5mi"),
          "10 m/px: 0.5mi / 80 px, 805 m");
    CHECK(unitsScaleBar(20.0, 84, UNITS_US, &px, lab, sizeof(lab)) == 1609 && px == 80 && is(lab, "1mi"),
          "20 m/px: 1mi");
    CHECK(unitsScaleBar(40.0, 84, UNITS_US, &px, lab, sizeof(lab)) == 3219 && px == 80 && is(lab, "2mi"),
          "40 m/px: 2mi / 80 px, 3219 m");
    CHECK(unitsScaleBar(5000.0, 84, UNITS_US, &px, lab, sizeof(lab)) == 321869 && px == 64 && is(lab, "200mi"),
          "5000 m/px: 200mi");
    CHECK(unitsScaleBar(mapMetersPerPixel(47.5, 18), 84, UNITS_US, &px, lab, sizeof(lab)) == 30 && px == 76 &&
          is(lab, "100ft"), "z18 at 47.5 N: 100ft / 76 px");
    CHECK(unitsScaleBar(0.1, 84, UNITS_US, &px, lab, sizeof(lab)) == 6 && px == 61 && is(lab, "20ft"),
          "0.1 m/px: 20ft / 61 px");
    CHECK(unitsScaleBar(0.005, 84, UNITS_US, &px, lab, sizeof(lab)) == 1 && px == 61 && is(lab, "1ft"),
          "0.005 m/px: 1ft / 61 px, returned as 1 m (0.3 m never rounds to 'no bar')");
    CHECK(unitsScaleBar(0.01, 84, UNITS_US, &px, lab, sizeof(lab)) == 1 && px == 61 && is(lab, "2ft"),
          "0.01 m/px: 2ft / 61 px");
    CHECK(unitsScaleBar(1e-3, 84, UNITS_US, &px, lab, sizeof(lab)) == 1 && px == 84 && is(lab, "1ft"),
          "absurdly zoomed in: 1ft held to the box");
    CHECK(unitsScaleBar(1e9, 84, UNITS_US, &px, lab, sizeof(lab)) == 1 && px == 1 && is(lab, "1ft"),
          "absurdly zoomed out: 1ft / 1 px");
    char two[6];
    CHECK(unitsScaleBar(3.22, 84, UNITS_US, &px, two, 3) == 152 && px == 47 && is(two, ""),
          "a short label buffer is \"\", never \"50\" (the bar itself is unchanged)");
    CHECK(unitsScaleBar(3.22, 84, UNITS_US, &px, two, 6) == 152 && is(two, "500ft"), "exactly fitting");
    unitsScaleBar(40.0, 84, UNITS_METRIC, &px, two, 3);
    CHECK(is(two, ""), "metric \"2km\" in 3 bytes is \"\", never \"2k\"");
  }
  {
    // Property: the chosen bar is a candidate, its pixels are honest, and the next one up does not fit.
    const double cand[21] = { 1 * FT, 2 * FT, 5 * FT, 10 * FT, 20 * FT, 50 * FT, 100 * FT, 200 * FT,
                              500 * FT, 1000 * FT, 2000 * FT, 0.5 * MI, 1 * MI, 2 * MI, 5 * MI,
                              10 * MI, 20 * MI, 50 * MI, 100 * MI, 200 * MI, 500 * MI };
    const int maxes[] = { 1, 10, 50, 84, 100, 240 };
    long bad = 0, n = 0;
    for (double mpp = 0.005; mpp < 1e8; mpp *= 1.0021) {
      for (size_t k = 0; k < sizeof(maxes) / sizeof(maxes[0]); k++) {
        const int maxPx = maxes[k];
        int want = -1;
        for (int i = 0; i < 21; i++) {
          const int p = (int)(cand[i] / mpp + 0.5);
          if (p >= 1 && p <= maxPx) want = i;
        }
        int px = -1;
        char lab[16];
        const int m = unitsScaleBar(mpp, maxPx, UNITS_US, &px, lab, sizeof(lab));
        double back = 0, half = 0;
        bool ok = parseDist(lab, &back, &half);
        n++;
        if (want >= 0) {
          ok = ok && fabs(back - cand[want]) < 1e-6 && px == (int)(cand[want] / mpp + 0.5) &&
               m == (int)lround(cand[want] < 1 ? 1 : cand[want]) &&
               (want == 20 || (int)(cand[want + 1] / mpp + 0.5) > maxPx);
        } else {
          ok = ok && strcmp(lab, "1ft") == 0 && m == 1 && px >= 1 && px <= maxPx;
        }
        if (!ok) {
          if (bad < 5) printf("    %.6g/%d -> %d %d \"%s\" (want #%d)\n", mpp, maxPx, m, px, lab, want);
          bad++;
        }
      }
    }
    printf("    %ld US scale bars checked\n", n);
    CHECK(bad == 0, "US scale bar: the longest candidate that fits, honest pixels and label");
  }

  // The setting's own row: ONE wording for the Maps menu and the Almanac's Settings (review
  // 2026-09-27: the map said "US (ft, mi)", the Almanac "US (ft, mi, mph)"). 0.9.81: US is
  // Fahrenheit (and inHg, inches) for the weather too, and the row says so.
  CHECK(is(unitsSettingRow(UNITS_US), "Units: US (ft, mi, mph, F)") &&
        is(unitsSettingRow(UNITS_METRIC), "Units: metric") && is(unitsSettingRow(7), "Units: metric"),
        "the setting's row: US names its units, Fahrenheit included; anything not US is metric");

  // ── the weather's formatters (0.9.81): fetched in SI, written in the setting ────────────────
  {
    char b[32];
    unitsFmtTemp(11.2, UNITS_METRIC, b, sizeof(b));
    CHECK(is(b, "11C"), "temp: 11.2 C -> \"11C\" (no degree glyph)");
    unitsFmtTemp(11.2, UNITS_US, b, sizeof(b));
    CHECK(is(b, "52F"), "temp: 11.2 C -> \"52F\"");
    unitsFmtTemp(-0.4, UNITS_METRIC, b, sizeof(b));
    CHECK(is(b, "0C"), "temp: -0.4 C -> \"0C\", never \"-0C\"");
    unitsFmtTemp(-17.9, UNITS_US, b, sizeof(b));
    CHECK(is(b, "0F"), "temp: -17.9 C = -0.2 F -> \"0F\", never \"-0F\"");
    unitsFmtTemp(-18.1, UNITS_US, b, sizeof(b));
    CHECK(is(b, "-1F"), "temp: -18.1 C -> \"-1F\"");
    unitsFmtTemp(-2.5, UNITS_METRIC, b, sizeof(b));
    CHECK(is(b, "-3C"), "temp: half away from zero (-2.5 -> -3)");
    unitsFmtTemp(NAN, UNITS_METRIC, b, sizeof(b));
    CHECK(is(b, "--"), "temp: NaN -> \"--\"");
    unitsFmtTemp(1e9, UNITS_METRIC, b, sizeof(b));
    CHECK(is(b, "9999C"), "temp: held to 9999");
    unitsFmtTemp(11.2, UNITS_METRIC, b, 3);
    CHECK(is(b, ""), "temp: no room -> \"\", never a cut number");
    unitsFmtWind(3.9, UNITS_METRIC, true, b, sizeof(b));
    CHECK(is(b, "14 km/h"), "wind: 3.9 m/s -> \"14 km/h\" (whole)");
    unitsFmtWind(3.9, UNITS_US, true, b, sizeof(b));
    CHECK(is(b, "9 mph"), "wind: 3.9 m/s -> \"9 mph\"");
    unitsFmtWind(8.3, UNITS_METRIC, false, b, sizeof(b));
    CHECK(is(b, "30"), "wind: bare (the gusts after it): 8.3 m/s -> \"30\"");
    unitsFmtWind(-1.0, UNITS_US, true, b, sizeof(b));
    CHECK(is(b, "0 mph"), "wind: negative reads as 0");
    unitsFmtWind(NAN, UNITS_US, true, b, sizeof(b));
    CHECK(is(b, "--"), "wind: NaN -> \"--\"");
    unitsFmtPressure(1017.9, UNITS_METRIC, b, sizeof(b));
    CHECK(is(b, "1018 hPa"), "pressure: 1017.9 hPa -> \"1018 hPa\"");
    unitsFmtPressure(1017.9, UNITS_US, b, sizeof(b));
    CHECK(is(b, "30.06 inHg"), "pressure: 1017.9 hPa -> \"30.06 inHg\"");
    unitsFmtPressure(1013.25, UNITS_US, b, sizeof(b));
    CHECK(is(b, "29.92 inHg"), "pressure: the standard atmosphere -> \"29.92 inHg\"");
    unitsFmtPressure(0, UNITS_METRIC, b, sizeof(b));
    CHECK(is(b, "--"), "pressure: 0 -> \"--\"");
    unitsFmtPrecip(2.7, UNITS_METRIC, b, sizeof(b));
    CHECK(is(b, "2.7 mm"), "precip: \"2.7 mm\"");
    unitsFmtPrecip(2.7, UNITS_US, b, sizeof(b));
    CHECK(is(b, "0.11 in"), "precip: 2.7 mm -> \"0.11 in\"");
    unitsFmtPrecip(0.0, UNITS_US, b, sizeof(b));
    CHECK(is(b, "0 in"), "precip: none -> \"0 in\"");
    unitsFmtPrecip(0.02, UNITS_METRIC, b, sizeof(b));
    CHECK(is(b, "<0.1 mm"), "precip: a trace that rounds to 0 -> \"<0.1 mm\" (\"0\" would say dry)");
    unitsFmtPrecip(0.1, UNITS_US, b, sizeof(b));
    CHECK(is(b, "<0.01 in"), "precip: 0.1 mm -> \"<0.01 in\"");
    unitsFmtPrecip(0.127, UNITS_US, b, sizeof(b));
    CHECK(is(b, "0.01 in"), "precip: 0.127 mm = 0.005 in rounds once, in integers -> \"0.01 in\"");
    unitsFmtPrecip(25.4, UNITS_US, b, sizeof(b));
    CHECK(is(b, "1.00 in"), "precip: 25.4 mm -> \"1.00 in\"");
    unitsFmtPrecip(-3, UNITS_METRIC, b, sizeof(b));
    CHECK(is(b, "0 mm"), "precip: negative reads as 0");
    // A sweep: Fahrenheit read back is within half a degree of the exact conversion, never "-0".
    int bad = 0;
    for (double c = -60.0; c <= 60.0; c += 0.01) {
      unitsFmtTemp(c, UNITS_US, b, sizeof(b));
      const double f = c * 9.0 / 5.0 + 32.0;
      const long got = strtol(b, NULL, 10);
      if (fabs(got - f) > 0.5 + 1e-9 || !strcmp(b, "-0F") || b[strlen(b) - 1] != 'F') bad++;
      unitsFmtTemp(c, UNITS_METRIC, b, sizeof(b));
      if (fabs(strtol(b, NULL, 10) - c) > 0.5 + 1e-9 || !strcmp(b, "-0C")) bad++;
    }
    CHECK(bad == 0, "temp sweep -60..60 C by 0.01: both units within half a degree, never -0");
  }

  if (failures) {
    printf("test_units: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("test_units: %d passed, 0 failed\n", checks);
  return 0;
}
