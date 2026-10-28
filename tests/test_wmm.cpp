/*
 * test_wmm.cpp - wmm.cpp against NOAA's OWN published WMM2025 test values (all 100 rows of
 * WMM2025_TestValues.txt, carried in tests/vectors_almanac.h as ALM_WMM - never recomputed
 * by anything of ours): D and I within 0.01 deg (NOAA prints them to 2 decimals, so a right
 * answer is <= 0.005 off), X, Y, Z, H, F within 1 nT. Plus the embedded table against
 * tools/data/WMM2025.COF, the validity window, the decimal-year clock (to the int64 ends), the
 * geographic pole (where Y' has a 0/0 that must come out finite and continuous) and a local
 * sanity value.
 */

#include "../WiPhone/wmm.h"
#include "../WiPhone/wmm2025_cof.h"
#include "vectors_almanac.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

static double angDiff(double a, double b) {        // degrees, wrapped into -180..180
  double d = fmod(a - b, 360.0);
  if (d > 180.0) d -= 360.0;
  if (d < -180.0) d += 360.0;
  return fabs(d);
}

int main() {
  printf("test_wmm\n");

  // ---- the generated table is the shape wmm.cpp indexes -----------------------
  {
    bool shape = (WMM_COF_ROWS == 90) && (WMM_COF_NMAX == 12);
    for (int n = 1; n <= 12; n++)
      for (int m = 0; m <= n; m++) {
        const WmmCoef& c = WMM_COF[n * (n + 1) / 2 - 1 + m];
        if (c.n != n || c.m != m) shape = false;
      }
    CHECK(shape, "wmm2025_cof.h: 90 rows, row (n,m) at n(n+1)/2-1+m");
    CHECK(WMM_COF_EPOCH == 2025.0 && WMM_COF[0].g == -29351.8f && WMM_COF[0].gdot == 12.0f,
          "wmm2025_cof.h: epoch 2025.0, g(1,0) = -29351.8, gdot 12.0");
    CHECK(strcmp(wmmModelName(), "WMM2025") == 0, "model name WMM2025");
  }

  // ---- every embedded coefficient is NOAA's WMM.COF, as float --------------------------
  /* The NOAA rows below are checked to 1 nT, and a degree-12 coefficient 0.1 nT off moves the
   * field ~0.6 nT: a hand-edited or merge-mangled wmm2025_cof.h can pass them (review
   * 2026-09-27: g(12,12) -0.7 -> -0.8 did). So the table is held to the source file itself. */
  {
    FILE* f = fopen("tools/data/WMM2025.COF", "r");   // the runner works from the repo root
    int rows = 0, bad = 0;
    bool header = false, terminated = false;
    if (f) {
      char line[160];
      if (fgets(line, sizeof line, f)) {
        double epoch = 0.0;
        char model[32] = "";
        header = sscanf(line, "%lf %31s", &epoch, model) == 2 && epoch == WMM_COF_EPOCH &&
                 strcmp(model, WMM_COF_MODEL) == 0;
      }
      while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "9999", 4) == 0) { terminated = true; break; }
        int n, m;
        double g, h, gd, hd;
        if (sscanf(line, "%d %d %lf %lf %lf %lf", &n, &m, &g, &h, &gd, &hd) != 6) { bad++; continue; }
        if (rows >= WMM_COF_ROWS) { bad++; rows++; continue; }
        const WmmCoef& c = WMM_COF[rows++];
        if (c.n != n || c.m != m || c.g != (float)g || c.h != (float)h || c.gdot != (float)gd ||
            c.hdot != (float)hd) {
          bad++;
          printf("  row %d (%d,%d): file %g %g %g %g, table %g %g %g %g\n", rows - 1, n, m, g, h,
                 gd, hd, c.g, c.h, c.gdot, c.hdot);
        }
      }
      fclose(f);
    }
    CHECK(f != NULL, "tools/data/WMM2025.COF opens (run from the repo root)");
    CHECK(header, "WMM2025.COF header: epoch 2025.0, WMM-2025");
    CHECK(rows == WMM_COF_ROWS && bad == 0 && terminated,
          "wmm2025_cof.h = WMM2025.COF, all 90 rows (n, m, g, h, gdot, hdot as float)");
  }

  // ---- NOAA's 100 published test values ----------------------------------------
  {
    const int rows = (int)(sizeof(ALM_WMM) / sizeof(ALM_WMM[0]));
    double worst[7] = {0, 0, 0, 0, 0, 0, 0};       // decl incl h x y z f
    int worstRow[7] = {0, 0, 0, 0, 0, 0, 0};
    int bad = 0, refused = 0;
    for (int i = 0; i < rows; i++) {
      const AlmWmmVec& v = ALM_WMM[i];
      WmmField w;
      if (!wmmCompute(v.lat, v.lon, v.altKm, v.year, &w)) { refused++; continue; }
      const double err[7] = {
        angDiff(w.decl, v.decl), fabs(w.incl - v.incl), fabs(w.h - v.h), fabs(w.x - v.x),
        fabs(w.y - v.y), fabs(w.z - v.z), fabs(w.f - v.f) };
      const double tol[7] = { 0.01, 0.01, 1.0, 1.0, 1.0, 1.0, 1.0 };   // docs/almanac.md
      bool rowBad = false;
      for (int k = 0; k < 7; k++) {
        if (err[k] > worst[k]) { worst[k] = err[k]; worstRow[k] = i; }
        if (!(err[k] <= tol[k])) rowBad = true;
      }
      if (rowBad) {
        bad++;
        printf("  row %d (%.1f, %.0f km, %.0f, %.0f): D %.4f/%.2f I %.4f/%.2f X %.3f/%.3f "
               "Y %.3f/%.3f Z %.3f/%.3f\n", i, v.year, v.altKm, v.lat, v.lon, w.decl, v.decl,
               w.incl, v.incl, w.x, v.x, w.y, v.y, w.z, v.z);
      }
    }
    printf("  %d NOAA rows; worst |err|: D %.5f deg (row %d), I %.5f deg (row %d), "
           "H %.4f nT (row %d), X %.4f (row %d), Y %.4f (row %d), Z %.4f (row %d), F %.4f (row %d)\n",
           rows, worst[0], worstRow[0], worst[1], worstRow[1], worst[2], worstRow[2],
           worst[3], worstRow[3], worst[4], worstRow[4], worst[5], worstRow[5], worst[6],
           worstRow[6]);
    CHECK(rows == 100, "all 100 NOAA WMM2025 test rows present");
    CHECK(refused == 0, "no NOAA row refused (all inside 2025.0-2030.0)");
    CHECK(bad == 0, "every NOAA row: D, I within 0.01 deg; X Y Z H F within 1 nT");
    CHECK(worst[0] <= 0.01 && worst[1] <= 0.01, "worst D and I within 0.01 deg");
    CHECK(worst[2] <= 1.0 && worst[3] <= 1.0 && worst[4] <= 1.0 && worst[5] <= 1.0 &&
          worst[6] <= 1.0, "worst X Y Z H F within 1 nT");
  }

  // ---- the validity window: refuse, and leave *out alone --------------------------
  {
    WmmField w;
    memset(&w, 0x5A, sizeof w);
    WmmField before = w;
    CHECK(!wmmCompute(47.5, -121.8, 0.0, 2024.99, &w), "2024.99 refused (before the model)");
    CHECK(!wmmCompute(47.5, -121.8, 0.0, 2030.01, &w), "2030.01 refused (model expired)");
    CHECK(!wmmCompute(47.5, -121.8, 0.0, NAN, &w), "NaN year refused");
    CHECK(!wmmCompute(90.5, -121.8, 0.0, 2026.0, &w), "latitude 90.5 refused");
    CHECK(!wmmCompute(NAN, -121.8, 0.0, 2026.0, &w), "NaN latitude refused");
    CHECK(!wmmCompute(47.5, INFINITY, 0.0, 2026.0, &w), "infinite longitude refused");
    CHECK(!wmmCompute(47.5, -121.8, NAN, 2026.0, &w), "NaN altitude refused");
    CHECK(memcmp(&w, &before, sizeof w) == 0, "a refusal leaves *out untouched");
    CHECK(!wmmCompute(47.5, -121.8, 0.0, 2026.0, NULL), "NULL out refused");
    CHECK(wmmCompute(47.5, -121.8, 0.0, 2025.0, &w), "2025.0 accepted (first instant)");
    CHECK(wmmCompute(47.5, -121.8, 0.0, 2030.0, &w), "2030.0 accepted (last instant)");
    CHECK(!wmmCompute(47.5, -121.8, 0.0, wmmDecimalYear(1735689599), &w),
          "2024-12-31 23:59:59 UTC refused");
    CHECK(wmmCompute(47.5, -121.8, 0.0, wmmDecimalYear(1893456000), &w),
          "2030-01-01 00:00:00 UTC accepted");
    CHECK(!wmmCompute(47.5, -121.8, 0.0, wmmDecimalYear(1893456001), &w),
          "2030-01-01 00:00:01 UTC refused");
  }

  // ---- decimal years ----------------------------------------------------------------
  {
    CHECK(wmmDecimalYear(1735689600) == 2025.0, "2025-01-01 00:00 UTC = 2025.0 exactly");
    CHECK(wmmDecimalYear(1767225600) == 2026.0, "2026-01-01 00:00 UTC = 2026.0 exactly");
    CHECK(fabs(wmmDecimalYear(1751457600) - 2025.5) < 1e-12, "2025-07-02 12:00 UTC = 2025.5");
    CHECK(fabs(wmmDecimalYear(1846108800) - 2028.5) < 1e-12,
          "2028-07-02 00:00 UTC = 2028.5 (leap year: 183 of 366 days)");
    CHECK(wmmDecimalYear(1830297600) == 2028.0, "2028-01-01 = 2028.0");
    double d = wmmDecimalYear(1767225599);
    CHECK(d < 2026.0 && d > 2025.99999, "one second before 2026 is 2025.99999...");
    CHECK(wmmDecimalYear(946684800) == 2000.0, "2000-01-01 = 2000.0 (the 400-year leap)");
    CHECK(fabs(wmmDecimalYear(4133937600LL) - (2100.0 + 364.5 / 365.0)) < 1e-12,
          "2100-12-31 12:00 = 2100 + 364.5/365 (2100 is not a leap year)");
    CHECK(fabs(wmmDecimalYear(-15768000) - 1969.5) < 1e-12, "1969-07-02 12:00 = 1969.5 (before 1970)");
    CHECK(wmmDecimalYear(0) == 1970.0, "the epoch = 1970.0");
    /* The floor for instants before 1970, where it shows: the last second of a LEAP year
     * (1968) must be counted in 1968's 366 days, not as -1 s of 1969's 365. */
    CHECK(fabs(wmmDecimalYear(-31536001) - (1969.0 - 1.0 / 31622400.0)) < 1e-12,
          "1968-12-31 23:59:59 = 1969 - 1/(366 days) (floor, not truncate, before 1970)");
    /* The int64 ends: no overflow (UBSan), and the year settles in a step or two - a days/365
     * estimate took ~1.9e8 steps here (1.4 s on the Mac, minutes on the phone). */
    const int64_t ends[4] = { INT64_MAX, INT64_MIN, INT64_MAX - 86399, INT64_MIN + 86401 };
    bool plausible = true;
    const clock_t c0 = clock();
    for (int i = 0; i < 4; i++) {
      const double y = wmmDecimalYear(ends[i]);
      if (!(fabs(y - (1970.0 + (double)ends[i] / 31556952.0)) < 2.0)) plausible = false;
    }
    const double secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
    CHECK(plausible, "int64 ends: the year is within 2 of 1970 + t / (mean Gregorian year)");
    CHECK(secs < 0.25, "int64 ends: four decimal years in well under a second (bounded settle)");
  }

  // ---- North Bend WA, the hunt's home, 2026-09-27 ---------------------------------------
  {
    WmmField w;
    memset(&w, 0, sizeof w);                     // printed below even when refused
    bool ok = wmmCompute(47.4957, -121.7868, 0.15, wmmDecimalYear(1790467200), &w);
    printf("  North Bend 2026-09-27: D %+.3f I %.3f H %.1f Z %.1f F %.1f nT\n",
           w.decl, w.incl, w.h, w.z, w.f);
    CHECK(ok, "North Bend 2026-09-27 computes");
    CHECK(ok && w.decl > 14.0 && w.decl < 16.0, "North Bend declination is EAST, +14..+16 deg");
    CHECK(ok && w.incl > 60.0 && w.incl < 75.0, "North Bend dip is steeply down (60..75 deg)");
  }

  // ---- the geographic poles: the 0/0 in Y' comes out finite and continuous --------------
  {
    const double lats[2] = { 90.0, -90.0 };
    bool cont = true, inv = true, finite = true;
    for (int s = 0; s < 2; s++) {
      const double lat = lats[s];
      const double nearLat = lat > 0 ? lat - 1e-6 : lat + 1e-6;   // off the pole branch
      WmmField a, b, c;
      bool ok = wmmCompute(lat, -121.0, 28.0, 2027.3, &a) &&
                wmmCompute(nearLat, -121.0, 28.0, 2027.3, &b) &&
                wmmCompute(lat, 59.0, 28.0, 2027.3, &c);
      if (!ok) { cont = inv = finite = false; continue; }
      if (!(isfinite(a.x) && isfinite(a.y) && isfinite(a.z) && isfinite(a.decl))) finite = false;
      if (fabs(a.x - b.x) > 0.01 || fabs(a.y - b.y) > 0.01 || fabs(a.z - b.z) > 0.01 ||
          angDiff(a.decl, b.decl) > 0.0001) cont = false;
      /* At the pole the horizontal field is ONE vector; the meridian only turns the frame. So
       * H and Z do not depend on longitude, and D turns with it: at the north pole the frame
       * of meridian lon is turned by lon about the axis, so D(lon) + lon is the same for
       * every lon (at the south pole D(lon) - lon is). */
      const double turn = lat > 0 ? (a.decl + -121.0) - (c.decl + 59.0)
                                  : (a.decl - -121.0) - (c.decl - 59.0);
      if (fabs(a.h - c.h) > 0.01 || fabs(a.z - c.z) > 0.01 || angDiff(turn, 0.0) > 0.0001)
        inv = false;
      printf("  pole %+.0f: lon -121 X %.3f Y %.3f Z %.3f D %+.4f | 1e-6 off X %.3f Y %.3f "
             "Z %.3f | lon 59 H %.3f D %+.4f\n", lat, a.x, a.y, a.z, a.decl, b.x, b.y, b.z,
             c.h, c.decl);
    }
    CHECK(finite, "exact poles: finite field (the special-case Y sum)");
    CHECK(cont, "exact poles match 1e-6 deg off the pole (X Y Z within 0.01 nT)");
    CHECK(inv, "exact poles: H and Z independent of longitude, D turns with the meridian");

    /* The pole formula keeps only the m = 1 terms, so it may take over ONLY at the pole (cos
     * phi' <= 1e-10). Along a meridian the field is smooth - over 0.05 deg the midpoint sits
     * within ~0.005 nT of the chord - so a pole branch reaching down to 89.95 (a 1e-3
     * threshold: Y 8.7 nT off there) shows as a kink. */
    bool smooth = true;
    const double lons[2] = { -121.0, 59.0 };
    for (int s = 0; s < 2 && smooth; s++) {
      for (int k = 0; k < 2; k++) {
        const double sg = s == 0 ? 1.0 : -1.0;
        WmmField lo = WmmField(), mid = WmmField(), hi = WmmField();   // printed if refused
        if (!(wmmCompute(sg * 89.9, lons[k], 0.0, 2026.5, &lo) &&
              wmmCompute(sg * 89.95, lons[k], 0.0, 2026.5, &mid) &&
              wmmCompute(sg * 90.0, lons[k], 0.0, 2026.5, &hi)) ||
            fabs(mid.x - (lo.x + hi.x) / 2.0) > 0.05 || fabs(mid.y - (lo.y + hi.y) / 2.0) > 0.05 ||
            fabs(mid.z - (lo.z + hi.z) / 2.0) > 0.05) {
          smooth = false;
          printf("  kink at %+.2f, lon %.0f: Y %.4f vs chord %.4f\n", sg * 89.95, lons[k], mid.y,
                 (lo.y + hi.y) / 2.0);
        }
      }
    }
    CHECK(smooth, "89.9 / 89.95 / 90 (both poles): X Y Z on the chord within 0.05 nT");
  }

  // ---- secular variation: the field moves, and linearly --------------------------------
  {
    WmmField a, b, c;
    const bool ok = wmmCompute(47.4957, -121.7868, 0.15, 2025.0, &a) &&
                    wmmCompute(47.4957, -121.7868, 0.15, 2027.5, &b) &&
                    wmmCompute(47.4957, -121.7868, 0.15, 2030.0, &c);
    CHECK(ok && fabs(a.decl - b.decl) > 0.05, "declination drifts over the model's 5 years");
    CHECK(ok && fabs((a.x + c.x) / 2.0 - b.x) < 1e-6 && fabs((a.z + c.z) / 2.0 - b.z) < 1e-6,
          "X and Z vary linearly in time (g + t*gdot)");
  }

  printf("test_wmm: %d/%d checks passed\n", checks - failures, checks);
  return failures;
}
