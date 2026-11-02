/*
 * battery_curve.cpp — see battery_curve.h.
 */

#include "battery_curve.h"

/* (millivolts, percent). Strictly increasing in both columns — test_battery.cpp asserts it,
 * because a single transposed pair here would make the gauge run backwards over one band
 * with nothing on screen to say so. Regenerate from the fixture, never by hand:
 *   tests/fixtures/p1_discharge_2026-09-03.tsv → 10 mV bins of time-linear SOC → 50 mV means. */
static const struct { int mv; int pct; } CURVE[] = {
  {3300,   0}, {3350,   1}, {3400,   2}, {3450,   6}, {3500,  10}, {3550,  15},
  {3600,  19}, {3650,  22}, {3700,  27}, {3750,  33}, {3800,  42}, {3850,  53},
  {3900,  63}, {3950,  70}, {4000,  81}, {4050,  93}, {4100,  98}, {4160, 100},
};
static const int N = (int)(sizeof(CURVE) / sizeof(CURVE[0]));

int batterySocFromVoltage(float volts) {
  const int mv = (int)(volts * 1000.0f + 0.5f);
  if (mv <= CURVE[0].mv) {
    return 0;
  }
  if (mv >= CURVE[N - 1].mv) {
    return 100;
  }
  for (int i = 0; i < N - 1; i++) {
    const int a = CURVE[i].mv, b = CURVE[i + 1].mv;
    if (mv >= a && mv <= b) {
      const int pa = CURVE[i].pct, pb = CURVE[i + 1].pct;
      /* +0.5 for round-to-nearest; the integer maths is deliberate — this runs every 15 s
       * on the battery tick and there is nothing here that needs a float. */
      return pa + ((pb - pa) * (mv - a) * 2 + (b - a)) / (2 * (b - a));
    }
  }
  return 0;   // unreachable: the clamps above cover everything outside the table
}
