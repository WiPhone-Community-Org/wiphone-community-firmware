/*
 * astro.h - the Almanac's arithmetic: where the sun and the moon are, when they rise, set and
 * cross the meridian in a local day, the moon's phase and the next quarters, the seasons, legal
 * light, the solunar tables, and the calendar helpers the Almanac screen needs.
 *
 * The algorithms are written down in docs/almanac.md and COVEY's covey_ui/almanac.py implements
 * the SAME ones; both are proven against tests/vectors_almanac.{h,json} (PyEphem) by
 * tests/test_almanac.cpp. A wrong moonrise looks exactly as plausible as a right one - change
 * nothing here without running that test.
 *
 * Pure: no Arduino/ESP-IDF headers, no heap, no static mutable state, small stacks (largest frame
 * ~500 B - astroSunDay/astroMoonDay's AstroDayJob - deepest chain ~1 KB on the ESP32 toolchain).
 * Everything is double - software floating point on the ESP32 - so the costs are counted in libm
 * calls (counted on the host, 47.5 N, 2026-09-27): astroSunDay ~1,850 and astroMoonDay ~3,650
 * (the theory at 9 nodes a day, 289 cheap 5-minute samples, bisections), a phase search ~650-830
 * (~8 longitude-only lunar series), astroMoonPhaseAt ~960 (astroMoonPhaseWith ~215), the
 * sun's position ~27, the moon's ~215. On the phone at 160 MHz one libm call with the arithmetic
 * around it is ~26.5 us (phone 2 measured the core day, 5,602 calls, at 148 ms).
 *
 * Conventions: an instant is UNIX seconds (int64). Angles in degrees; latitude north and
 * longitude east positive; azimuth from true north, clockwise, 0..360. A "day" is the LOCAL day
 * [t0, t0 + 86400) with t0 = local midnight as UNIX seconds; every event is the FIRST of its
 * kind in that window, 0 = none (the moon skips a rise about once a month; at 71 N the sun skips
 * for weeks). An event's instant is the first whole second at or past the crossing.
 */
#ifndef ASTRO_H
#define ASTRO_H

#include <stdint.h>

struct AstroSunDay  { int64_t dawn, rise, noon, set, dusk; };      // UNIX s; 0 = none in [t0, t0+86400)
struct AstroMoonDay { int64_t rise, set, transit, under; };        // transit = overhead, under = underfoot

struct AstroMoonPhase {
  double elong;     // lon_m - lon_s, 0..360 (geocentric apparent ecliptic longitudes)
  double sep;       // signed angular separation, deg, + = waxing (east of the sun)
  double illum;     // illuminated fraction, 0..1
  int    phase;     // 0..7: New, Waxing crescent, First quarter, Waxing gibbous, Full,
                    //       Waning gibbous, Last quarter, Waning crescent
  double ageDays;   // days since the previous new moon
};

struct AstroSolunar {
  int64_t majorMid[2], minorMid[2];   // majors +-60 min, minors +-30 min around the mid; time order
  int nMajor, nMinor;
  int rating;                         // 0..4: Poor, Fair, Good, Very good, Excellent
};

// Legal-light rules (astroLegalLight).
enum { ASTRO_LEGAL_30MIN = 0, ASTRO_LEGAL_CIVIL = 1 };

double  astroJd(int64_t unixSec);

// Positions. alt has NO refraction (the event thresholds carry the standard 34').
void    astroSunPos(int64_t t, double lat, double lon, double* altDeg, double* azDeg);
void    astroMoonPos(int64_t t, double lat, double lon, double* altDeg, double* azDeg);  // topocentric
double  astroSunSemidiameter(int64_t t);                              // deg (~0.27)
double  astroMoonSemidiameter(int64_t t, double lat, double lon);     // topocentric, deg (~0.25-0.28)
double  astroMoonDistanceKm(int64_t t);                               // geocentric, km (~356500-406700)
// The moon's geocentric ecliptic place of date (Meeus ch. 47): GEOMETRIC longitude (no nutation),
// latitude, deg, distance, km. Any pointer may be null.
void    astroMoonEcliptic(int64_t t, double* lonDeg, double* latDeg, double* distKm);
// TT - UT in seconds that the theories use (a constant; see astro.cpp).
double  astroDeltaT();

// Events in the local day [t0, t0 + 86400).
//   sun: dawn/dusk = centre 6 deg down (civil); rise/set = upper limb on the 34' horizon;
//        noon = hour angle through 0.
//   moon: rise/set = topocentric upper limb on the 34' horizon; transit/under = topocentric hour
//        angle through 0 / through 180.
// Each is astroDayJob run to its end in one call (below): ~1,850 / ~3,650 libm calls, 49 / 97 ms
// on the phone at 160 MHz (the core day measured 148 ms on phone 2, 2026-09-27).
void    astroSunDay(int64_t t0, double lat, double lon, AstroSunDay* out);
void    astroMoonDay(int64_t t0, double lat, double lon, AstroMoonDay* out);

/* ── The same day, in slices ─────────────────────────────────────────────────────────────────
 * The phone's loop may not stop for 100 ms (a pass over 250 ms is a LOOP STALL, and the Almanac's
 * budget is ~60 ms a pass), so its app runs astroSunDay / astroMoonDay as a JOB, a piece per
 * timer tick. The pieces ("units") are the ones the one-call functions are made of, in the same
 * order: the theory at one of the nine 3-hour nodes (sun ~20 libm calls, moon ~200), then the
 * first sample at t0 - 1, then one 5-minute sample each with the bisection it opens (~6, a
 * bracket ~60). astroSunDay/astroMoonDay ARE this job run whole, so the result is bit-identical
 * however it is cut (tests/test_almanac.cpp cuts it at every unit and at random). Plain data, no
 * pointers: it may be copied or live in PSRAM. Its fields are astro.cpp's working state - read
 * the result through astroDayJobSun / astroDayJobMoon only. */
enum { ASTRO_BODY_SUN = 0, ASTRO_BODY_MOON = 1 };
#define ASTRO_DAY_NODES 9
struct AstroBodyDay {           // one body over one local day (the scan's interpolation table)
  int64_t t0;
  double  xyz[ASTRO_DAY_NODES][3];   // geocentric equatorial at t0 + k*3h (moon: Earth radii; sun: AU)
  double  lst0;                 // local apparent sidereal time at t0, deg
  double  sinLat, cosLat;
  double  obsXY, obsZ;          // the observer's geocentric vector
  bool    isMoon;
};
struct AstroDayJob {
  AstroBodyDay b;
  double  lat, lon, eqeq;
  int     body;                 // ASTRO_BODY_*
  int     stage;                // nodes, the first sample, the scan, done (astro.cpp)
  int     node;                 // nodes evaluated so far
  int64_t scanA, scanB;         // the bracket the next sample closes
  double  prev[4];              // the last sample (limb, civil, hour angle, hour angle - 180)
  int64_t ev[5];                // sun: dawn rise noon set dusk; moon: rise set transit under
};
/* Asked before every unit after the first of a call: false = stop here, resume on the next
 * call. NULL = never stop. */
typedef bool (*AstroMoreFn)(void* ctx);
void    astroDayJobStart(AstroDayJob* j, int body, int64_t t0, double lat, double lon);
/* Run units until the day is done or more() says stop; at least one unit per call (a call
 * always makes progress). True = done (and every later call returns true at once). */
bool    astroDayJobRun(AstroDayJob* j, AstroMoreFn more, void* ctx);
bool    astroDayJobDone(const AstroDayJob* j);
void    astroDayJobSun(const AstroDayJob* j, AstroSunDay* out);     // body SUN, once done
void    astroDayJobMoon(const AstroDayJob* j, AstroMoonDay* out);   // body MOON, once done

// Moon phase (geocentric).
void    astroMoonPhaseAt(int64_t t, AstroMoonPhase* out);
/* astroMoonPhaseAt without its search: `prevNew` is astroPrevMoonPhase(t, 0), already known
 * (0 = unknown: ageDays is 0). Identical to astroMoonPhaseAt given that value - which is
 * astroMoonPhaseWith(t, astroPrevMoonPhase(t, 0), out). The search is ~740 of its ~960 libm calls
 * (20 of 25 ms on the phone): the Almanac keeps the new moons either side of now instead
 * (almanac_lines.h, AlmMoonAge), and TODAY, which shows no age, passes 0. */
void    astroMoonPhaseWith(int64_t t, int64_t prevNew, AstroMoonPhase* out);
const char* astroPhaseName(int phase);                   // "?" outside 0..7
int64_t astroNextMoonPhase(int64_t from, int quarter);   // 0 new, 1 first quarter, 2 full, 3 last quarter; first strictly after `from`
int64_t astroPrevMoonPhase(int64_t from, int quarter);   // last at or before `from`
int64_t astroNextSeason(int64_t from, int which);        // 0 March equinox, 1 June solstice, 2 September equinox, 3 December solstice
                                                         // (the sun's apparent longitude through 90*which); first strictly after `from`

// First and last legal light. rule 0 = sunrise-30 min / sunset+30 min (default), 1 = civil
// twilight; 0 when the underlying event is 0.
void    astroLegalLight(const AstroSunDay* s, int rule, int64_t* first, int64_t* last);

// John Alden Knight's solunar periods for the day that astroSunDay/astroMoonDay computed from t0:
// majors around the moon's transit and underfoot, minors around its rise and set. rating from the
// phase at t0+12h (3 within 12 deg of new or full, 2 within 36, 1 within 60, else 0) plus 1 if
// any period's span contains sunrise or sunset; max 4.
void    astroSolunar(int64_t t0, const AstroSunDay* sun, const AstroMoonDay* moon, AstroSolunar* out);
const char* astroSolunarRatingName(int rating);          // "Poor".."Excellent", "?" outside 0..4

// Calendar. localUnix = UNIX seconds + the UTC offset (any sign; floor division, so it works
// before 1970 too). Proleptic Gregorian.
void    astroCivil(int64_t localUnix, int* y, int* m, int* d, int* wday /*0=Sunday*/, int* yday /*1..366*/);
int     astroIsoWeek(int y, int m, int d);                // ISO 8601 week number 1..53 (may belong to y-1 or y+1)
void    astroUsDst(int year, int* marchDay /*2nd Sunday of March*/, int* novDay /*1st Sunday of November*/);

// Day number of a civil date (days since 1970-01-01; negative before). The inverse of astroCivil's
// date part; handy for t0 = astroDaysFromCivil(y, m, d) * 86400 - utcOffsetSeconds.
int64_t astroDaysFromCivil(int y, int m, int d);

#endif // ASTRO_H
