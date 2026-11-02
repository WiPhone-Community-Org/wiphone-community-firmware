/*
 * wmm.cpp - NOAA's World Magnetic Model 2025, evaluated the standard way (see wmm.h and
 * docs/almanac.md). The steps, in the NOAA WMM Technical Report 2025's order:
 *
 *   1. geodetic (lat, lon, height above the WGS84 ellipsoid) -> geocentric spherical
 *      (r, phi', lambda): a = 6378.137 km, f = 1/298.257223563.
 *   2. Gauss coefficients at the time: g(t) = g + (t - 2025.0) * gdot (same for h).
 *   3. Schmidt semi-normalised associated Legendre functions P(n,m)(sin phi') and
 *      dP/dtheta by recursion, n, m <= 12; reference radius a_ref = 6371.2 km.
 *        X' =  sum (a/r)^(n+2) (g cos m.lam + h sin m.lam) dP(n,m)/dtheta
 *        Y' =  sum (a/r)^(n+2) m (g sin m.lam - h cos m.lam) P(n,m) / cos phi'
 *        Z' = -sum (a/r)^(n+2) (n+1) (g cos m.lam + h sin m.lam) P(n,m)
 *      (theta = the geocentric colatitude, so dP/dtheta = -dP/dphi' and X' matches the
 *      report's -sum ... dP/dphi'.)
 *   4. rotate X', Z' by (phi' - phi) back into the geodetic frame; Y is unchanged.
 *   5. H = hypot(X, Y), F = hypot(H, Z), D = atan2(Y, X), I = atan2(Z, H).
 *
 * The recursion runs COLUMN by column (m outer, n inner) on the Schmidt functions
 * directly, so each step needs only P(n-1,m) and P(n-2,m) and the diagonal P(m,m):
 * scalars, no 13x13 (or 91-entry) arrays on the phone's 8 KB loop stack. It is the same
 * three-term recurrence NOAA's MAG_PcupLow uses, with the Schmidt factors folded in:
 *
 *   P(0,0) = 1;  P(1,1) = sin(theta) P(0,0);
 *   P(m,m) = sqrt((2m-1)/(2m)) sin(theta) P(m-1,m-1)                    (m >= 2)
 *   P(n,m) = ((2n-1) cos(theta) P(n-1,m) - sqrt((n-1)^2 - m^2) P(n-2,m)) / sqrt(n^2 - m^2)
 *   and the same recurrences differentiated in theta for dP.
 *
 * The pole: Y' divides by cos phi', which is 0 at the geographic poles. There only the
 * m = 1 terms survive and P(n,1)/cos phi' is finite; like NOAA's MAG_SummationSpecial,
 * that limit is computed by the m = 1 recurrence started from P(1,1)/sin(theta) = 1.
 */
#include "wmm.h"
#include "wmm2025_cof.h"

#include <math.h>

static const double WMM_PI = 3.14159265358979323846;
static const double WMM_DEG = WMM_PI / 180.0;
static const double WGS84_A = 6378.137;                 // km
static const double WGS84_F = 1.0 / 298.257223563;
static const double WMM_REF_RADIUS = 6371.2;            // km, the model's reference sphere

/* g(t), h(t) of row (n, m): index n*(n+1)/2 - 1 + m in the generated table. */
static inline void coefAt(int n, int m, double dt, double* g, double* h) {
  const WmmCoef& c = WMM_COF[n * (n + 1) / 2 - 1 + m];
  *g = (double)c.g + dt * (double)c.gdot;
  *h = (double)c.h + dt * (double)c.hdot;
}

bool wmmCompute(double latDeg, double lonDeg, double altKm, double decimalYear, WmmField* out) {
  if (!out) return false;
  if (!(decimalYear >= WMM_VALID_FROM && decimalYear <= WMM_VALID_TO)) return false;  // NaN too
  if (!(latDeg >= -90.0 && latDeg <= 90.0)) return false;
  if (!isfinite(lonDeg) || !isfinite(altKm)) return false;

  const double dt = decimalYear - WMM_COF_EPOCH;

  // 1. geodetic -> geocentric spherical.
  const double lat = latDeg * WMM_DEG, lon = lonDeg * WMM_DEG;
  const double sinLat = sin(lat), cosLat = cos(lat);
  const double e2 = WGS84_F * (2.0 - WGS84_F);
  const double rc = WGS84_A / sqrt(1.0 - e2 * sinLat * sinLat);  // prime-vertical radius
  const double xp = (rc + altKm) * cosLat;
  const double zp = (rc * (1.0 - e2) + altKm) * sinLat;
  const double r = sqrt(xp * xp + zp * zp);
  if (!(r > 0.0)) return false;
  const double ct = zp / r;        // cos(theta) = sin(phi'), geocentric
  const double st = xp / r;        // sin(theta) = cos(phi') >= 0

  const double ar = WMM_REF_RADIUS / r;
  const double cosl = cos(lon), sinl = sin(lon);

  // 3. sum the field, column by column.
  double bx = 0.0, by = 0.0, bz = 0.0;
  double pmm = 1.0, dpmm = 0.0;            // P(m,m), dP(m,m)/dtheta; P(0,0) = 1
  double cosm = 1.0, sinm = 0.0;           // cos(m.lam), sin(m.lam)
  double arm = ar * ar;                    // (a/r)^(m+2)
  for (int m = 0; m <= WMM_COF_NMAX; m++) {
    if (m > 0) {
      const double k = (m == 1) ? 1.0 : sqrt((2.0 * m - 1.0) / (2.0 * m));
      const double p = k * st * pmm;
      const double dp = k * (st * dpmm + ct * pmm);
      pmm = p;
      dpmm = dp;
      const double c = cosm * cosl - sinm * sinl;
      sinm = sinm * cosl + cosm * sinl;
      cosm = c;
      arm *= ar;
    }
    double pn = pmm, dpn = dpmm;           // P(n,m), starting at n = m
    double pn1 = 0.0, dpn1 = 0.0;          // P(n-1,m)
    double arn = arm;                      // (a/r)^(n+2)
    for (int n = m; n <= WMM_COF_NMAX; n++) {
      if (n > m) {
        const double knm = sqrt((double)(n * n - m * m));
        const double k1 = sqrt((double)((n - 1) * (n - 1) - m * m));
        const double p = ((2.0 * n - 1.0) * ct * pn - k1 * pn1) / knm;
        const double dp = ((2.0 * n - 1.0) * (ct * dpn - st * pn) - k1 * dpn1) / knm;
        pn1 = pn;
        dpn1 = dpn;
        pn = p;
        dpn = dp;
        arn *= ar;
      }
      if (n == 0) continue;                // no monopole term
      double g, h;
      coefAt(n, m, dt, &g, &h);
      const double gc = g * cosm + h * sinm;
      const double gs = g * sinm - h * cosm;
      bx += arn * gc * dpn;
      by += arn * m * gs * pn;
      bz -= arn * (n + 1) * gc * pn;
    }
  }

  if (st > 1e-10) {
    by /= st;
  } else {
    /* The geographic pole: Y' = sum (a/r)^(n+2) (g(n,1) sin lam - h(n,1) cos lam) Q(n),
     * Q(n) = lim P(n,1)/sin(theta), by the m = 1 recurrence from Q(1) = 1. */
    by = 0.0;
    double q = 1.0, q1 = 0.0;              // Q(n), Q(n-1)
    double arn = ar * ar * ar;             // (a/r)^(n+2) at n = 1
    for (int n = 1; n <= WMM_COF_NMAX; n++) {
      if (n > 1) {
        const double qn = ((2.0 * n - 1.0) * ct * q - sqrt((double)((n - 1) * (n - 1) - 1)) * q1) /
                          sqrt((double)(n * n - 1));
        q1 = q;
        q = qn;
        arn *= ar;
      }
      double g, h;
      coefAt(n, 1, dt, &g, &h);
      by += arn * (g * sinl - h * cosl) * q;
    }
  }

  // 4. geocentric -> geodetic frame: rotate by psi = phi' - phi.
  const double cpsi = st * cosLat + ct * sinLat;     // cos(phi' - phi)
  const double spsi = ct * cosLat - st * sinLat;     // sin(phi' - phi)
  const double x = bx * cpsi - bz * spsi;
  const double z = bx * spsi + bz * cpsi;
  const double y = by;

  // 5. the derived elements.
  const double hh = sqrt(x * x + y * y);
  out->x = x;
  out->y = y;
  out->z = z;
  out->h = hh;
  out->f = sqrt(hh * hh + z * z);
  out->decl = atan2(y, x) / WMM_DEG;
  out->incl = atan2(z, hh) / WMM_DEG;
  return true;
}

/* Days since 1970-01-01 of 1 January of year y (proleptic Gregorian; Howard Hinnant's
 * days_from_civil with m = 1, d = 1). */
static int64_t daysToJan1(int64_t y) {
  y -= 1;                                  // January counts in the previous March-based year
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + 306;  // 1 Jan = day 306 from 1 Mar
  return era * 146097 + doe - 719468;
}

double wmmDecimalYear(int64_t unixSec) {
  int64_t days = unixSec / 86400;
  int64_t sod = unixSec % 86400;           // seconds into the UTC day
  if (sod < 0) { sod += 86400; days--; }   // floor, for instants before 1970
  /* Estimate from the mean Gregorian year (146097 days per 400 years), then settle: the
   * estimate is within a year of the truth for EVERY int64 input, so each loop takes at most
   * a step or two. (A days/365 estimate drifts 1.8e-6 year per day - ~1.9e8 steps at the int64
   * ends, seconds on a Mac and minutes on the phone. days * 400 cannot overflow: |days| <=
   * 1.07e14.) */
  int64_t y = 1970 + days * 400 / 146097;
  while (daysToJan1(y) > days) y--;
  while (daysToJan1(y + 1) <= days) y++;
  /* Count in days, then seconds: days - d0 <= 365, so nothing here overflows (a day count
   * times 86400 does, near the int64 ends). Same bits as (unixSec - Jan 1 in seconds). */
  const int64_t d0 = daysToJan1(y);
  const int64_t into = (days - d0) * 86400 + sod;
  const int64_t len = (daysToJan1(y + 1) - d0) * 86400;
  return (double)y + (double)into / (double)len;
}

const char* wmmModelName() { return "WMM2025"; }
