/*
 * test_sun_astro.cpp - the sun checks that were true of the old sun_times.cpp (tests/test_sun.cpp,
 * retired with it in 0.9.80) and must stay true of what replaced it: WiPhone/astro.cpp, the ONE
 * source of legal light for the Almanac, the Meshtastic Sun screen and the serial `sun`.
 *
 * Implementation-independent facts only: almanac anchors (loose tolerance), the geometry of
 * equinoxes and solstices, longitude shifting time linearly, the ordering dawn < rise < noon <
 * set < dusk, polar night, and the calendar helpers. (Its precision is test_almanac.cpp's, against
 * PyEphem.) The old refusals - a date outside 1970..2099, a place off the globe - live in
 * almanac_lines.cpp now (almDateOf, almPlaceOk) and are tested in test_almanac_lines.cpp.
 *
 * Every event is UNIX seconds in the LOCAL day [t0, t0 + 86400) - no more "UTC minutes that
 * wrap into the adjacent date", which is what the old ladder arithmetic existed to undo.
 */

#include "../WiPhone/astro.h"
#include <cstdio>
#include <cstdlib>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

static int64_t localMidnight(int y, int m, int d, int tzS) {
  return astroDaysFromCivil(y, m, d) * 86400 - tzS;
}
// Seconds after local midnight.
static long at(int64_t ev, int64_t t0) { return (long)(ev - t0); }
static bool near(long got, long want, long tolS) { return labs(got - want) <= tolS; }

int main() {
  printf("test_sun_astro\n");

  const double SEA_LAT = 47.6062, SEA_LON = -122.3321;
  const int PDT = -7 * 3600, PST = -8 * 3600;

  // ---- date helpers (what sunUnixToDate did) -------------------------------------------------
  {
    int y, m, d, wd, yd;
    astroCivil(0, &y, &m, &d, &wd, &yd);
    CHECK(y == 1970 && m == 1 && d == 1 && wd == 4 && yd == 1, "epoch: 1970-01-01, a Thursday");
    astroCivil(1787184000LL, &y, &m, &d, &wd, &yd);        // 2026-08-20 00:00 UTC
    CHECK(y == 2026 && m == 8 && d == 20, "2026-08-20 from unix");
    astroCivil(1709164800LL, &y, &m, &d, &wd, &yd);        // 2024-02-29
    CHECK(y == 2024 && m == 2 && d == 29 && yd == 60, "leap day resolves (day 60)");
    CHECK(astroDaysFromCivil(2026, 8, 20) * 86400 == 1787184000LL, "and back again");
  }

  // ---- almanac anchors (NOAA calculator, +-4 min slack) --------------------------------------
  {
    /* Seattle, 2026-09-15 (the hunt's season): sunrise ~06:49 PDT, sunset ~19:16 PDT. */
    const int64_t t0 = localMidnight(2026, 9, 15, PDT);
    AstroSunDay s;
    astroSunDay(t0, SEA_LAT, SEA_LON, &s);
    CHECK(s.rise && s.set && s.dawn && s.dusk && s.noon, "Seattle Sep 15: every event happens");
    CHECK(near(at(s.rise, t0), 6 * 3600 + 49 * 60, 4 * 60), "Seattle Sep 15 sunrise ~06:49 PDT");
    CHECK(near(at(s.set, t0), 19 * 3600 + 16 * 60, 6 * 60), "Seattle Sep 15 sunset ~19:16 PDT");
    /* Civil twilight at mid-latitudes runs ~28-34 min beyond the sun times. */
    const long dawnLead = (long)(s.rise - s.dawn), duskLag = (long)(s.dusk - s.set);
    CHECK(dawnLead >= 24 * 60 && dawnLead <= 38 * 60, "civil dawn ~30 min before sunrise");
    CHECK(duskLag >= 24 * 60 && duskLag <= 38 * 60, "civil dusk ~30 min after sunset");
    int64_t first = 0, last = 0;
    astroLegalLight(&s, ASTRO_LEGAL_30MIN, &first, &last);
    CHECK(first == s.rise - 1800 && last == s.set + 1800, "30-min legal light = sunrise-30, sunset+30");
  }

  // ---- geometry that must hold -----------------------------------------------------------------
  {
    AstroSunDay t;
    // Equinox: day length ~12h07m (disc + refraction), at the equator too.
    astroSunDay(localMidnight(2026, 3, 20, PDT), SEA_LAT, SEA_LON, &t);
    CHECK(near((long)(t.set - t.rise), 12 * 3600 + 7 * 60, 6 * 60), "equinox day ~12h07 in Seattle");
    astroSunDay(localMidnight(2026, 3, 20, PST), 0.0, SEA_LON, &t);
    CHECK(near((long)(t.set - t.rise), 12 * 3600 + 7 * 60, 4 * 60), "equinox day ~12h07 at the equator");

    // Solstices: June day longest, December shortest.
    AstroSunDay ju, de;
    astroSunDay(localMidnight(2026, 6, 21, PDT), SEA_LAT, SEA_LON, &ju);
    astroSunDay(localMidnight(2026, 12, 21, PST), SEA_LAT, SEA_LON, &de);
    CHECK(ju.set - ju.rise > 15 * 3600, "June day in Seattle ~16h");
    CHECK(de.set - de.rise < 9 * 3600, "December day in Seattle ~8.5h");
    CHECK((ju.set - ju.rise) > (de.set - de.rise) + 6 * 3600, "solstice spread > 6h at 47N");

    // 15 degrees of longitude = one hour of clock, same latitude.
    AstroSunDay a, b;
    const int64_t t0 = localMidnight(2026, 9, 15, PDT);
    astroSunDay(t0, SEA_LAT, SEA_LON, &a);
    astroSunDay(t0, SEA_LAT, SEA_LON + 15.0, &b);
    CHECK(near((long)(a.rise - b.rise), 3600, 120), "15 deg east = 60 min earlier");

    // Polar night: Utqiagvik in late December has no sunrise; civil twilight still does.
    astroSunDay(localMidnight(2026, 12, 21, -9 * 3600), 71.29, -156.79, &t);
    CHECK(t.rise == 0 && t.set == 0, "polar night: no sun events");
    CHECK(t.dawn != 0, "polar night: civil twilight still occurs");
    int64_t first = 1, last = 1;
    astroLegalLight(&t, ASTRO_LEGAL_30MIN, &first, &last);
    CHECK(first == 0 && last == 0, "polar night: no 30-min legal light either");
  }

  // ---- ordering wherever everything exists -------------------------------------------------------
  {
    AstroSunDay t;
    astroSunDay(localMidnight(2026, 10, 10, PDT), SEA_LAT, SEA_LON, &t);
    CHECK(t.dawn && t.dawn < t.rise && t.rise < t.noon && t.noon < t.set && t.set < t.dusk,
          "dawn < rise < noon < set < dusk");
    int64_t f30, l30, fc, lc;
    astroLegalLight(&t, ASTRO_LEGAL_30MIN, &f30, &l30);
    astroLegalLight(&t, ASTRO_LEGAL_CIVIL, &fc, &lc);
    CHECK(fc < f30 && lc > l30, "Oct 10 at 47 N: civil twilight is the LONGER rule (docs/almanac.md)");
  }

  if (failures) {
    printf("test_sun_astro: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("test_sun_astro: %d passed, 0 failed\n", checks);
  return 0;
}
