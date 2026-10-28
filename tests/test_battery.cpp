/*
 * test_battery.cpp — battery_curve.cpp against phone 1's recorded full-to-empty run.
 *
 * The fixture is the actual health.log of the 2026-09-02 discharge (557 one-a-minute samples:
 * up_min, the CW2015's own soc, cell volts, charger flag). "Truth" is the fraction of runtime
 * left — 100 % at the unplug, 0 % at the last sample — which at a constant load is the state
 * of charge. The table was BUILT from this run, so passing here proves it reproduces the
 * curve; it does not prove the curve holds on another day. It also pins the reason the
 * table exists: the chip's own number, scored the same way, is an order of magnitude worse.
 */
#include "../WiPhone/battery_curve.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

static int failures = 0, checks = 0;
#define CHECK(cond, name) do { checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

struct Row { int up; int chipSoc; float v; int chg; };

static int loadFixture(const char* path, Row* out, int cap) {
  FILE* f = fopen(path, "r");
  if (!f) return -1;
  char line[256]; int n = 0;
  while (fgets(line, sizeof(line), f) && n < cap) {
    if (line[0] == '#') continue;
    Row r;
    if (sscanf(line, "%d %d %f %d", &r.up, &r.chipSoc, &r.v, &r.chg) == 4) out[n++] = r;
  }
  fclose(f);
  return n;
}

int main() {
  printf("test_battery\n");

  // ---- the table itself -----------------------------------------------------------------
  CHECK(batterySocFromVoltage(3.30f) == 0,   "floor: 3.30 V reads 0 %");
  CHECK(batterySocFromVoltage(3.00f) == 0,   "below the floor still reads 0 %");
  CHECK(batterySocFromVoltage(4.16f) == 100, "top: 4.16 V reads 100 %");
  CHECK(batterySocFromVoltage(4.20f) == 100, "on the charger (4.20 V) reads 100 %");
  CHECK(batterySocFromVoltage(4.35f) == 100, "above the top still reads 100 %");
  CHECK(batterySocFromVoltage(3.85f) == 53,  "a breakpoint lands exactly (3.85 V = 53 %)");
  CHECK(batterySocFromVoltage(3.825f) == 48 || batterySocFromVoltage(3.825f) == 47,
        "midway between breakpoints interpolates (3.825 V ≈ 47-48 %)");

  /* Monotonic over the whole plausible range, 1 mV at a time. A transposed pair in the table
   * would make the gauge run BACKWARDS over one band, and nothing on screen would say so. */
  bool mono = true; int prev = -1;
  for (int mv = 2900; mv <= 4500; mv++) {
    int s = batterySocFromVoltage(mv / 1000.0f);
    if (s < prev) { mono = false; break; }
    prev = s;
  }
  CHECK(mono, "monotonic: SOC never falls as voltage rises (1 mV steps, 2.9-4.5 V)");

  // ---- against the recorded run ---------------------------------------------------------
  static Row rows[700];
  const int n = loadFixture("tests/fixtures/p1_discharge_2026-09-03.tsv", rows, 700);
  CHECK(n == 557, "fixture loads all 557 samples");
  if (n != 557) { printf("test_battery: %d passed, %d failed\n", checks - failures, failures); return 1; }

  int tUnplug = -1;
  for (int i = 0; i < n; i++) if (rows[i].chg == 0) { tUnplug = rows[i].up; break; }
  const int tEnd = rows[n - 1].up;
  CHECK(tUnplug == 9,  "fixture: charger came off at up=9min");
  CHECK(tEnd == 556,   "fixture: last sample at up=556min");
  CHECK(fabsf(rows[n - 1].v - 3.30f) < 0.005f, "fixture: died at 3.30 V");
  CHECK(rows[n - 1].chipSoc == 0, "fixture: the chip said 0 % at death");

  double sumOurs = 0, sumChip = 0; int maxOurs = 0, maxChip = 0, cnt = 0;
  int worstUp = -1; float worstV = 0;
  for (int i = 0; i < n; i++) {
    if (rows[i].chg) continue;                       // the 9 pre-unplug minutes are not the run
    const double truth = 100.0 * (tEnd - rows[i].up) / (double)(tEnd - tUnplug);
    const int ours = batterySocFromVoltage(rows[i].v);
    const int eo = (int)fabs(ours - truth), ec = (int)fabs(rows[i].chipSoc - truth);
    sumOurs += fabs(ours - truth); sumChip += fabs(rows[i].chipSoc - truth);
    if (eo > maxOurs) { maxOurs = eo; worstUp = rows[i].up; worstV = rows[i].v; }
    if (ec > maxChip) maxChip = ec;
    cnt++;
  }
  const double meanOurs = sumOurs / cnt, meanChip = sumChip / cnt;
  printf("  [%d on-battery samples: ours mean %.2f max %d | chip mean %.2f max %d | worst up=%d v=%.2f]\n",
         cnt, meanOurs, maxOurs, meanChip, maxChip, worstUp, worstV);
  CHECK(cnt == 548, "548 on-battery samples scored");
  CHECK(maxOurs <= 3,   "worst-case error against the run is <= 3 points");
  CHECK(meanOurs < 1.0, "mean error against the run is under 1 point");
  CHECK(meanChip > 5.0, "...and the chip's own number scores worse than 5 points mean (the reason this exists)");
  CHECK(meanOurs * 5 < meanChip, "ours is at least 5x better than the chip on the run");

  /* THE fault this fixes: at 3.54 V the chip said 0 % and the phone ran another 1.3 hours. */
  CHECK(batterySocFromVoltage(3.54f) >= 12,
        "3.54 V (where the chip reads 0 %) still reports >= 12 % — the 1.3 h is not thrown away");
  int firstChipZero = -1;
  for (int i = 0; i < n; i++) if (!rows[i].chg && rows[i].chipSoc == 0) { firstChipZero = i; break; }
  CHECK(firstChipZero > 0 && (tEnd - rows[firstChipZero].up) >= 70,
        "fixture: the chip hit 0 % with >= 70 min of runtime still to come");

  printf("test_battery: %d passed, %d failed\n", checks - failures, failures);
  return failures ? 1 : 0;
}
