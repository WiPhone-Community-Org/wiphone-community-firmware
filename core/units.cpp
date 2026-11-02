/*
 * units.cpp - see units.h. Proven by tests/test_units.cpp on the host, against the
 * shipping meshPosFmtDist() and mapScaleBar() for the metric half.
 */

#include "units.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool isFiniteD(double v) {
  return v >= -DBL_MAX && v <= DBL_MAX;          // false for NaN and both infinities
}

/* The whole string or nothing. A cut-short number is a DIFFERENT number that looks right:
 * "1,352ft" in 4 bytes is "1,3", "500ft" in 3 is "50", "--" in 2 is "-". Empty is honest.
 * `len` is snprintf's return (the length it wanted). */
static void wholeOrEmpty(char* out, size_t cap, int len) {
  if (len < 0 || (size_t)len >= cap) {
    out[0] = '\0';
  }
}

/* A whole number with an optional '+' (only when > 0 and asked for), an optional thousands
 * comma, and a suffix. 0 is always bare "0": the sign belongs to the rounded value, so -0.4 m
 * is "0m", never "-0m". |v| <= 1e12 here, so the digits fit many times over. */
static void fmtWhole(long long v, bool withSign, bool commas, const char* suffix,
                     char* out, size_t cap) {
  char digits[24];
  int nd = 0;
  unsigned long long mag = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
  do {
    digits[nd++] = (char)('0' + (int)(mag % 10u));
    mag /= 10u;
  } while (mag && nd < (int)sizeof(digits));
  char buf[48];
  int k = 0;
  if (v < 0) {
    buf[k++] = '-';
  } else if (v > 0 && withSign) {
    buf[k++] = '+';
  }
  for (int i = nd - 1; i >= 0; i--) {
    buf[k++] = digits[i];
    if (commas && i > 0 && i % 3 == 0) {
      buf[k++] = ',';
    }
  }
  buf[k] = '\0';
  wholeOrEmpty(out, cap, snprintf(out, cap, "%s%s", buf, suffix));
}

void unitsFmtDist(double m, int units, char* out, size_t cap) {
  if (!out || cap == 0) {
    return;
  }
  if (units != UNITS_US) {
    // meshPosFmtDist(), verbatim: test_units compares the two byte for byte.
    // (Byte for byte whenever the text fits `cap`; where it does not, meshPosFmtDist cuts it.)
    int len;
    if (m < 0) m = 0;
    if (m < 999.5) {                 // 999.6 must become "1.0km", never "1000m"
      len = snprintf(out, cap, "%dm", (int)lround(m));
    } else if (m < 10000.0) {
      len = snprintf(out, cap, "%.1fkm", m / 1000.0);
    } else {
      len = snprintf(out, cap, "%.0fkm", m / 1000.0);
    }
    wholeOrEmpty(out, cap, len);
    return;
  }
  if (m < 0) m = 0;
  const double ft = m / UNITS_M_PER_FT;
  if (ft < 999.5) {                  // 999.96 ft must become "0.2mi", never "1000ft"
    wholeOrEmpty(out, cap, snprintf(out, cap, "%dft", (int)lround(ft)));
    return;
  }
  const double mi = m / UNITS_M_PER_MI;
  int len;
  if (mi < 9.95) {                   // 9.97 mi is "10mi", never "10.0mi"
    len = snprintf(out, cap, "%.1fmi", mi);
  } else {
    len = snprintf(out, cap, "%.0fmi", mi);
  }
  wholeOrEmpty(out, cap, len);
}

void unitsFmtAlt(double m, int units, bool withSign, char* out, size_t cap) {
  if (!out || cap == 0) {
    return;
  }
  if (!isFiniteD(m)) {
    wholeOrEmpty(out, cap, snprintf(out, cap, "--"));
    return;
  }
  const bool us = units == UNITS_US;
  double v = us ? m / UNITS_M_PER_FT : m;
  if (v > 1e12) {
    v = 1e12;
  }
  if (v < -1e12) {
    v = -1e12;
  }
  // Half away from zero, like the elevation tiles themselves (elevFromTerrarium).
  fmtWhole(llround(v), withSign, us, us ? "ft" : "m", out, cap);
}

void unitsFmtSpeed(double metresPerSec, int units, char* out, size_t cap) {
  if (!out || cap == 0) {
    return;
  }
  if (!isFiniteD(metresPerSec)) {
    wholeOrEmpty(out, cap, snprintf(out, cap, "--"));
    return;
  }
  const bool us = units == UNITS_US;
  double v = us ? metresPerSec / UNITS_MPS_PER_MPH : metresPerSec * 3.6;
  if (!(v > 0.0)) {
    v = 0.0;                         // negative, and -0.0, would print "-0.0"
  }
  if (v > 1e9) {
    v = 1e9;
  }
  wholeOrEmpty(out, cap, snprintf(out, cap, "%.1f%s", v, us ? "mph" : "km/h"));
}

/* (int)(len / metresPerPx + 0.5) - mapScaleBar's own expression - held under INT_MAX. There
 * the cast of a larger value is undefined behaviour; here it is simply "does not fit". */
static int barPixels(double len, double metresPerPx) {
  const double q = len / metresPerPx + 0.5;
  if (!(q < 2147483647.0)) {
    return 2147483647;
  }
  return (int)q;
}

int unitsScaleBar(double metresPerPx, int maxPx, int units, int* barPx, char* label, size_t cap) {
  if (barPx) {
    *barPx = 0;
  }
  if (label && cap) {
    label[0] = '\0';
  }
  if (!(metresPerPx > 0) || maxPx <= 0) {
    return 0;
  }

  if (units != UNITS_US) {
    // mapScaleBar(), step for step: test_units compares the two over a sweep.
    static const int mant[3] = { 1, 2, 5 };
    int bestM = 0, bestPx = 0;
    double dec = 1.0;
    for (int e = 0; e < 8; e++) {
      for (int i = 0; i < 3; i++) {
        const double m = mant[i] * dec;
        const int p = barPixels(m, metresPerPx);
        if (p >= 1 && p <= maxPx) {
          bestM = (int)m;
          bestPx = p;
        }
      }
      dec *= 10.0;
    }
    if (!bestM) {
      bestM = 1;
      bestPx = barPixels(1.0, metresPerPx);
      if (bestPx < 1) {
        bestPx = 1;
      }
      if (bestPx > maxPx) {
        bestPx = maxPx;
      }
    }
    if (barPx) {
      *barPx = bestPx;
    }
    if (label && cap) {
      // app_maps.cpp drawBottomStrip's label: whole kilometres from 1000 m up.
      const int len = bestM >= 1000 ? snprintf(label, cap, "%dkm", bestM / 1000)
                                    : snprintf(label, cap, "%dm", bestM);
      wholeOrEmpty(label, cap, len);
    }
    return bestM;
  }

  /* US: feet up to 2000 ft, then miles from half a mile. Ascending in length (2000 ft =
   * 609.6 m < 0.5 mi = 804.7 m), so the last candidate that fits is the longest. */
  static const int    kFt[11] = { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000 };
  static const double kMi[10] = { 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500 };
  int best = -1, bestPx = 0;          // 0..10 = kFt[best], 11..20 = kMi[best - 11]
  double bestLen = 0.0;
  for (int i = 0; i < 21; i++) {
    const double len = i < 11 ? kFt[i] * UNITS_M_PER_FT : kMi[i - 11] * UNITS_M_PER_MI;
    const int p = barPixels(len, metresPerPx);
    if (p >= 1 && p <= maxPx) {
      best = i;
      bestPx = p;
      bestLen = len;
    }
  }
  if (best < 0) {
    best = 0;
    bestLen = kFt[0] * UNITS_M_PER_FT;
    bestPx = barPixels(bestLen, metresPerPx);
    if (bestPx < 1) {
      bestPx = 1;
    }
    if (bestPx > maxPx) {
      bestPx = maxPx;
    }
  }
  if (barPx) {
    *barPx = bestPx;
  }
  if (label && cap) {
    int len;
    if (best < 11) {
      len = snprintf(label, cap, "%dft", kFt[best]);
    } else if (kMi[best - 11] < 1.0) {
      len = snprintf(label, cap, "%.1fmi", kMi[best - 11]);
    } else {
      len = snprintf(label, cap, "%dmi", (int)kMi[best - 11]);
    }
    wholeOrEmpty(label, cap, len);
  }
  const long r = lround(bestLen);
  return r < 1 ? 1 : (int)r;
}

void unitsFmtTemp(double celsius, int units, char* out, size_t cap) {
  if (!out || cap == 0) {
    return;
  }
  if (!isFiniteD(celsius)) {
    wholeOrEmpty(out, cap, snprintf(out, cap, "--"));
    return;
  }
  const bool us = units == UNITS_US;
  double v = us ? celsius * 9.0 / 5.0 + 32.0 : celsius;
  if (v > 9999.0) {
    v = 9999.0;
  }
  if (v < -9999.0) {
    v = -9999.0;
  }
  // Half away from zero; a value that rounds to 0 prints bare "0" (fmtWhole's rule: never "-0C").
  fmtWhole(llround(v), false, false, us ? "F" : "C", out, cap);
}

void unitsFmtWind(double metresPerSec, int units, bool withUnit, char* out, size_t cap) {
  if (!out || cap == 0) {
    return;
  }
  if (!isFiniteD(metresPerSec)) {
    wholeOrEmpty(out, cap, snprintf(out, cap, "--"));
    return;
  }
  const bool us = units == UNITS_US;
  double v = us ? metresPerSec / UNITS_MPS_PER_MPH : metresPerSec * 3.6;
  if (!(v > 0.0)) {
    v = 0.0;
  }
  if (v > 99999.0) {
    v = 99999.0;
  }
  fmtWhole(llround(v), false, false, withUnit ? (us ? " mph" : " km/h") : "", out, cap);
}

void unitsFmtPressure(double hPa, int units, char* out, size_t cap) {
  if (!out || cap == 0) {
    return;
  }
  if (!isFiniteD(hPa) || !(hPa > 0.0) || hPa > 1e6) {
    wholeOrEmpty(out, cap, snprintf(out, cap, "--"));
    return;
  }
  if (units == UNITS_US) {
    const long long q = llround(hPa / UNITS_HPA_PER_INHG * 100.0);   // hundredths, rounded once
    wholeOrEmpty(out, cap, snprintf(out, cap, "%lld.%02lld inHg", q / 100, q % 100));
    return;
  }
  fmtWhole(llround(hPa), false, false, " hPa", out, cap);
}

void unitsFmtPrecip(double mm, int units, char* out, size_t cap) {
  if (!out || cap == 0) {
    return;
  }
  if (!isFiniteD(mm)) {
    wholeOrEmpty(out, cap, snprintf(out, cap, "--"));
    return;
  }
  if (!(mm > 0.0)) {
    wholeOrEmpty(out, cap, snprintf(out, cap, units == UNITS_US ? "0 in" : "0 mm"));
    return;
  }
  if (mm > 1e6) {
    mm = 1e6;
  }
  /* Hundredths of an inch / tenths of a mm, rounded once in integers (printf's "%.2f" of a
   * value on a half rounds by its binary neighbour: 0.127 mm could print "0.00 in"). */
  const bool us = units == UNITS_US;
  const long long q = llround(us ? mm / UNITS_MM_PER_IN * 100.0 : mm * 10.0);
  if (q == 0) {
    wholeOrEmpty(out, cap, snprintf(out, cap, us ? "<0.01 in" : "<0.1 mm"));
  } else if (us) {
    wholeOrEmpty(out, cap, snprintf(out, cap, "%lld.%02lld in", q / 100, q % 100));
  } else {
    wholeOrEmpty(out, cap, snprintf(out, cap, "%lld.%lld mm", q / 10, q % 10));
  }
}

const char* unitsSettingRow(int units) {
  return units == UNITS_US ? "Units: US (ft, mi, mph, F)" : "Units: metric";
}
