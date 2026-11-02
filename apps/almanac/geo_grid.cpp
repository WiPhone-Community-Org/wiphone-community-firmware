/*
 * geo_grid.cpp - UTM / MGRS / degrees-and-minutes. See geo_grid.h and
 * docs/almanac.md ("Grid references"). Pure, no heap, no mutable statics.
 */
#include "geo_grid.h"

#include <math.h>
#include <stdio.h>

namespace {

const double DEG = 3.14159265358979323846 / 180.0;

// WGS84 and the UTM constants.
const double WGS84_A = 6378137.0;
const double WGS84_F = 1.0 / 298.257223563;
const double UTM_K0 = 0.9996;
const double UTM_FALSE_E = 500000.0;
const double UTM_FALSE_N_SOUTH = 10000000.0;

/* MGRS truncates to the metre. A value computed a few ulps under a metre
 * line (an ulp at 1e7 m is 1.9e-9 m) would truncate into the PREVIOUS metre,
 * and the two ports' libms need not agree on those ulps. Adding 1e-6 m before
 * flooring is ~500 ulps (it survives any rounding here) yet far finer than the
 * series itself (~2e-7 m off the exact projection), so it only moves a point
 * within a micrometre below a metre line - where which side it is on is below
 * anything the numbers can know. (ON the central meridian eta is exactly 0,
 * so the easting is exactly 500000 with or without it.) test_geogrid pins it
 * with two points ~5e-7 m under a line. */
const double MGRS_EPS_M = 1e-6;

const char BANDS[] = "CDEFGHJKLMNPQRSTUVWX";         // 8 deg each from 80 S; X = 72..84
const char MGRS_ROWS[] = "ABCDEFGHJKLMNPQRSTUV";     // 20 rows of 100 km, cycling
const char* const MGRS_COLS[3] = { "ABCDEFGH", "JKLMNPQR", "STUVWXYZ" };  // by (zone-1)%3

bool isFinite(double x) { return x == x && x - x == 0.0; }  // false for NaN and +-inf

/* The zone for a lon already wrapped into [-180, 180), with the Norway and
 * Svalbard exceptions. */
int utmZone(double lat, double lon) {
  int zone = (int)floor((lon + 180.0) / 6.0) + 1;
  if (zone > 60) zone = 60;   // lon a hair under 180 can round (lon+180)/6 up to 60.0
  if (zone < 1) zone = 1;
  if (lat >= 56.0 && lat < 64.0 && lon >= 3.0 && lon < 12.0) return 32;  // Norway: 32V wide
  if (lat >= 72.0) {                                                    // Svalbard: band X
    if (lon >= 0.0 && lon < 9.0) return 31;
    if (lon >= 9.0 && lon < 21.0) return 33;
    if (lon >= 21.0 && lon < 33.0) return 35;
    if (lon >= 33.0 && lon < 42.0) return 37;
  }
  return zone;
}

}  // namespace

bool geoToUtm(double lat, double lon, GeoUtm* out) {
  if (!out) return false;
  if (!(lat >= -80.0 && lat < 84.0)) return false;   // also refuses NaN and +-inf
  if (!isFinite(lon)) return false;
  if (lon < -180.0 || lon >= 180.0) {                 // wrap only when needed: in-range stays bit-exact
    lon = fmod(lon + 180.0, 360.0);
    if (lon < 0.0) lon += 360.0;
    lon -= 180.0;
    if (lon >= 180.0) lon -= 360.0;
  }

  const int zone = utmZone(lat, lon);
  const double lon0 = zone * 6.0 - 183.0;             // central meridian
  const double phi = lat * DEG;
  const double lam = (lon - lon0) * DEG;

  // Ellipsoid: third flattening n, eccentricity e.
  const double f = WGS84_F;
  const double n = f / (2.0 - f);
  const double e = sqrt(f * (2.0 - f));
  const double n2 = n * n, n3 = n2 * n, n4 = n3 * n;

  // Rectifying radius A and Kruger's alpha_1..4 (Karney 2011, eq. 14 and 35), to n^4.
  const double A = WGS84_A / (1.0 + n) * (1.0 + n2 / 4.0 + n4 / 64.0);
  double alpha[5];
  alpha[0] = 0.0;
  alpha[1] = n / 2.0 - 2.0 / 3.0 * n2 + 5.0 / 16.0 * n3 + 41.0 / 180.0 * n4;
  alpha[2] = 13.0 / 48.0 * n2 - 3.0 / 5.0 * n3 + 557.0 / 1440.0 * n4;
  alpha[3] = 61.0 / 240.0 * n3 - 103.0 / 140.0 * n4;
  alpha[4] = 49561.0 / 161280.0 * n4;

  // Conformal latitude as tau' = tan(chi) (Karney eq. 7-9): exact, no series.
  const double sphi = sin(phi), cphi = cos(phi);
  const double tau = sphi / cphi;                      // |lat| < 84: cphi > 0.1
  const double sigma = sinh(e * atanh(e * sphi));
  const double taup = tau * sqrt(1.0 + sigma * sigma) - sigma * sqrt(1.0 + tau * tau);

  // Gauss-Schreiber (spherical TM on the conformal sphere), then Kruger's series.
  const double clam = cos(lam), slam = sin(lam);
  const double xip = atan2(taup, clam);
  const double etap = asinh(slam / sqrt(taup * taup + clam * clam));
  double xi = xip, eta = etap;
  for (int j = 1; j <= 4; j++) {
    const double a = 2.0 * j * xip, b = 2.0 * j * etap;
    xi += alpha[j] * sin(a) * cosh(b);
    eta += alpha[j] * cos(a) * sinh(b);
  }

  out->zone = zone;
  /* (lat + 80) / 8 can round UP onto a band line from a hair below it
   * (-1e-15 -> 80.0 = 'N' beside the southern false northing; 72 - 1e-14 ->
   * 'X' in zone 32, which does not exist). One step back by the exact line
   * (-80 + 8*bi is exact) makes the band agree with `north` and the zone
   * rules, which compare lat itself. It never rounds DOWN across a line. */
  int bi = (int)floor((lat + 80.0) / 8.0);
  if (bi > 0 && lat < -80.0 + 8.0 * bi) bi--;
  if (bi > 19) bi = 19;                                // X runs 72..84
  if (bi < 0) bi = 0;
  out->band = BANDS[bi];
  out->north = !(lat < 0.0);                           // -0.0 and 0.0 are north
  out->e = UTM_FALSE_E + UTM_K0 * A * eta;
  out->n = UTM_K0 * A * xi + (out->north ? 0.0 : UTM_FALSE_N_SOUTH);
  return true;
}

bool geoToMgrs(double lat, double lon, char* out, size_t cap) {
  if (!out) return false;
  if (cap > 0) out[0] = '\0';
  if (cap < 20) return false;
  GeoUtm u;
  if (!geoToUtm(lat, lon, &u)) return false;

  // Whole metres, truncated (see MGRS_EPS_M). Both fit a 32-bit long (<= 1e7).
  const long em = (long)floor(u.e + MGRS_EPS_M);
  const long nm = (long)floor(u.n + MGRS_EPS_M);
  if (em < 100000 || em >= 900000 || nm < 0) return false;   // cannot happen inside a zone

  const int col = (int)(em / 100000);                  // 1..8
  int row = (int)((nm / 100000) % 20);                 // the 2000 km cycle
  if (u.zone % 2 == 0) row = (row + 5) % 20;           // even zones start at F
  const char colL = MGRS_COLS[(u.zone - 1) % 3][col - 1];
  const char rowL = MGRS_ROWS[row];

  snprintf(out, cap, "%d%c %c%c %05ld %05ld", u.zone, u.band, colL, rowL,
           em % 100000, nm % 100000);
  return true;
}

void geoFmtDdm(double deg, bool isLat, char* out, size_t cap) {
  if (!out || cap == 0) return;
  int len;
  if (!isFinite(deg) || fabs(deg) > 360.0) {
    len = snprintf(out, cap, "--");
  } else {
    // Round once, in thousandths of a minute, so the carry is exact integer math.
    const long total = (long)floor(fabs(deg) * 60000.0 + 0.5);   // <= 21,600,000
    const long d = total / 60000;
    const long mThou = total % 60000;                  // 0..59999
    char hemi;
    if (isLat) hemi = (deg < 0.0 && total != 0) ? 'S' : 'N';
    else hemi = (deg < 0.0 && total != 0) ? 'W' : 'E';
    len = snprintf(out, cap, "%ld %02ld.%03ld%c", d, mThou / 1000, mThou % 1000, hemi);
  }
  if (len < 0 || (size_t)len >= cap) out[0] = '\0';   // never a cut-off number ("121 4")
}
