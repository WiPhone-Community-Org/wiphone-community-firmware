/*
 * wmm.h - the Earth's magnetic field from NOAA's World Magnetic Model 2025, offline.
 *
 * What the Almanac needs it for: DECLINATION - the angle between true north (the map's
 * north) and where a compass needle points. Add it to a magnetic bearing to get the true
 * bearing (east declination positive: at North Bend WA in 2026 it is about +15, so a
 * compass reading of 100 is a true bearing of ~115). It changes with place AND year,
 * which is why a paper map's printed value goes stale and why this is computed.
 *
 * The model is NOAA's degree-12 spherical-harmonic WMM2025 (coefficients embedded from
 * WMM.COF via tools/gen_wmm_cof.py -> wmm2025_cof.h; public domain), evaluated the
 * standard way (docs/almanac.md, "Magnetic declination"). It is valid 2025.0 - 2030.0
 * only; outside that window wmmCompute() refuses and the screen should say the model
 * has expired, not show a number that silently drifts.
 *
 * Accuracy: this code reproduces all 100 of NOAA's published test values to their
 * printed precision (tests/test_wmm.cpp). The MODEL itself is good to about 0.5 deg of
 * declination at mid-latitudes; local rock (and the phone's own speaker magnet) is not in
 * any model.
 *
 * Pure: no Arduino headers, no heap, no static mutable state (reentrant). Stack: a few
 * dozen doubles (the Legendre recursion runs column by column in scalars, no arrays).
 */
#ifndef WMM_H
#define WMM_H

#include <stdint.h>

/* The window the embedded model is valid for (decimal years, inclusive). */
#define WMM_VALID_FROM 2025.0
#define WMM_VALID_TO   2030.0

struct WmmField {
  double decl;   // D, degrees, east positive (true = magnetic + decl), -180..180
  double incl;   // I (dip), degrees, positive = field points down (northern hemisphere)
  double h;      // horizontal intensity, nT
  double x;      // north component (geodetic frame), nT
  double y;      // east component, nT
  double z;      // down component, nT
  double f;      // total intensity, nT
};

/* The field at a place (geodetic WGS84 latitude/longitude in degrees, east and north
 * positive; altKm = height above the WGS84 ELLIPSOID in km - a GPS height; mean-sea-level
 * height differs by the geoid, ~ -20 m around Seattle, which does not matter here) at a
 * decimal year. Returns false and leaves *out untouched when decimalYear is outside
 * [WMM_VALID_FROM, WMM_VALID_TO] (the model has expired), when |lat| > 90, when any input
 * is NaN/inf, or when out is NULL. At the geographic poles D is measured from the
 * meridian of lonDeg. */
bool wmmCompute(double latDeg, double lonDeg, double altKm, double decimalYear, WmmField* out);

/* The decimal year of a UTC instant: year + (seconds since 1 Jan 00:00 UTC) / (seconds in
 * that year), leap-year aware. 2025-01-01 00:00 UTC = 2025.0; 2025-07-02 12:00 UTC = 2025.5. */
double wmmDecimalYear(int64_t unixSec);

/* "WMM2025" - for the screen's "source" line. */
const char* wmmModelName();

#endif  // WMM_H
