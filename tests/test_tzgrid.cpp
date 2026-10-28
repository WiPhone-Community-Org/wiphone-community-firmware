/*
 * test_tzgrid.cpp - WiPhone/tz_grid.cpp against tests/vectors_tzgrid.h (tools/gen_tz_grid.py).
 *
 * What is independent of the code under test, and what is not:
 *   * every expected OFFSET and TRANSITION comes from Python's zoneinfo (tzdata), bisected to
 *     the second. None of it comes from a rule model, so the from-first-principles US/Canada
 *     rule is checked against tzdata at every DST change of 2026-2030 (one second before, at,
 *     and after) for every zone in the table;
 *   * every named place's jan/jul offset is zoneinfo's for the POLYGON the point is in (not
 *     the cell's answer). Where the grid answers a zone, its clock must equal the clock that
 *     is really there;
 *   * the rule's dates are also re-derived here for 1900-2200 by a second, deliberately naive
 *     calendar (day counting + Zeller's congruence);
 *   * the lookups (answer + why) come from the generator's own grid. They prove the decoder
 *     and the integer cell arithmetic, and they are the SAME vectors COVEY's
 *     covey_ui/tzgrid.py is tested against, so the two devices agree on every one. The
 *     grid's own correctness (no zone answered where a polygon of another clock lies within
 *     one cell) is the generator's self-check over 400,000 random points.
 *   * the content hash is recomputed here from the arrays the phone compiles. It must equal
 *     the header's and the vectors' hash, which is also the hash in COVEY's tzgrid.bin.
 *
 * Failure lines print coordinates to 2 decimals only (the repo's rule for any position).
 */

#include "../WiPhone/tz_grid.h"
#include "../WiPhone/tz_grid_data.h"   // the same arrays tz_grid.cpp compiles, for the hash
#include "../WiPhone/book_hash.h"
#include "vectors_tzgrid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

/* Quiet per-row checks (the vector tables are long): only failures print. */
#define CHECKQ(cond, fmt, ...) do { \
    checks++; \
    if (!(cond)) { printf("  FAIL " fmt " (line %d)\n", __VA_ARGS__, __LINE__); failures++; } \
  } while (0)

static const char* WHY[] = {"ZONE", "BORDER", "WATER", "OUTSIDE", "UNKNOWN_RULE", "BAD_INPUT"};
static const char* whyName(int w) { return (w >= 0 && w < 6) ? WHY[w] : "?"; }

/* ---- the content hash, exactly as tools/gen_tz_grid.py defines it ------------------------ */
static void putLe(BsSha256* h, uint64_t v, int bytes) {
  uint8_t b[8];
  for (int i = 0; i < bytes; i++) b[i] = (uint8_t)(v >> (8 * i));
  bsSha256Update(h, b, bytes);
}

static void contentHashHex(char out[65]) {
  BsSha256 h;
  bsSha256Init(&h);
  bsSha256Update(&h, "TZGRID1\n", 8);
  putLe(&h, (uint32_t)(int32_t)TZGRID_LAT0_E7, 4);
  putLe(&h, (uint32_t)(int32_t)TZGRID_LON0_E7, 4);
  putLe(&h, (uint32_t)(int32_t)TZGRID_RES_E7, 4);
  putLe(&h, TZGRID_ROWS, 2);
  putLe(&h, TZGRID_COLS, 2);
  for (int i = 0; i < TZGRID_NZONES; i++) {
    const TzZone& z = TZGRID_ZONES[i];
    char line[256];
    int n = snprintf(line, sizeof line, "%s\t%s\t%d\t%d\t%lld\t%s\t%s\n", z.iana, z.display, z.stdMinutes,
                     z.rule, (long long)z.sinceUtc, z.abbrStd, z.abbrDst);
    bsSha256Update(&h, line, (size_t)n);
  }
  bsSha256Update(&h, "\n", 1);
  for (int i = 0; i <= TZGRID_ROWS; i++) putLe(&h, TZGRID_ROW_START[i], 4);
  for (int i = 0; i < TZGRID_NRUNS; i++) putLe(&h, TZGRID_RUNS[i], 2);
  uint8_t d[32];
  bsSha256Final(&h, d);
  for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* ---- a second, naive calendar for the rule's dates --------------------------------------- */
static bool leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static long long naiveDays(long y, int m, int d) {   // days since 1970-01-01, by counting
  static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  long long n = 0;
  if (y >= 1970) for (long k = 1970; k < y; k++) n += leap(k) ? 366 : 365;
  else for (long k = y; k < 1970; k++) n -= leap(k) ? 366 : 365;
  for (int k = 1; k < m; k++) n += mdays[k - 1] + (k == 2 && leap(y) ? 1 : 0);
  return n + d - 1;
}

static int zellerSunday0(long y, int m, int d) {     // 0 = Sunday .. 6 = Saturday
  if (m < 3) { m += 12; y -= 1; }
  long K = y % 100, J = y / 100;
  int h = (int)((d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7);   // 0 = Saturday
  return (h + 6) % 7;
}

static int nthSundayDay(long y, int m, int n) {
  int d = 1;
  while (zellerSunday0(y, m, d) != 0) d++;
  return d + 7 * (n - 1);
}

static bool sameText(const char* a, const char* b) { return strcmp(a ? a : "", b ? b : "") == 0; }

int main() {
  printf("test_tzgrid\n");
  const TzGridInfo* info = tzGridInfo();

  // ---- the data: hash, shape, every row whole ----------------------------------------------
  {
    char hex[65];
    contentHashHex(hex);
    CHECK(strcmp(hex, TZGRID_SHA256) == 0, "content hash recomputed from the compiled arrays == TZGRID_SHA256");
    CHECK(strcmp(hex, TZV_SHA256) == 0, "... == the vectors' hash (= COVEY's tzgrid.bin)");
    CHECK(strcmp(info->sha256, TZGRID_SHA256) == 0, "tzGridInfo() carries the hash");
    CHECK(strstr(info->attribution, "OpenStreetMap contributors") &&
          strstr(info->attribution, "Open Database License (ODbL) 1.0") &&
          strstr(info->attribution, "timezone-boundary-builder"),
          "the ODbL / OpenStreetMap attribution is in the data");
    CHECK(info->validFrom == 2026 && info->validTo == 2035, "rules verified 2026..2035");
    CHECK(info->resE7 == 500000 && info->rows == 1300 && info->cols == 2760, "0.05 deg, 1300 x 2760");
    CHECK(TZGRID_ROW_START[0] == 0 && TZGRID_ROW_START[TZGRID_ROWS] == TZGRID_NRUNS, "row index spans the runs");
    bool whole = true, codes = true, monotone = true;
    uint32_t maxRuns = 0;
    for (int r = 0; r < TZGRID_ROWS; r++) {
      if (TZGRID_ROW_START[r + 1] <= TZGRID_ROW_START[r]) { monotone = false; break; }
      uint32_t n = TZGRID_ROW_START[r + 1] - TZGRID_ROW_START[r];
      if (n > maxRuns) maxRuns = n;
      long cols = 0;
      for (uint32_t k = TZGRID_ROW_START[r]; k < TZGRID_ROW_START[r + 1]; k++) {
        int v = TZGRID_RUNS[k] >> 9;
        if (v >= TZGRID_NZONES && v != TZGRID_CODE_WATER && v != TZGRID_CODE_BORDER) codes = false;
        cols += (TZGRID_RUNS[k] & 511) + 1;
      }
      if (cols != TZGRID_COLS) { whole = false; printf("    row %d sums to %ld\n", r, cols); }
    }
    CHECK(monotone, "every row has at least one run");
    CHECK(whole, "every row's runs add up to exactly TZGRID_COLS");
    CHECK(codes, "every run value is a zone, WATER or BORDER");
    printf("    longest row: %u runs; %d runs, %d zones, %u B runs + %u B index\n", maxRuns, TZGRID_NRUNS,
           TZGRID_NZONES, (unsigned)(2 * TZGRID_NRUNS), (unsigned)(4 * (TZGRID_ROWS + 1)));
    CHECK(maxRuns <= 64, "no row longer than 64 runs (a lookup is a short walk)");
  }

  // ---- the zone table -------------------------------------------------------------------------
  {
    CHECK(tzGridZoneCount() == TZV_NZONES, "zone count matches the vectors");
    bool same = tzGridZoneCount() == TZV_NZONES;
    for (int i = 0; same && i < TZV_NZONES; i++) {
      const TzZone* z = tzGridZone(i);
      const TzvZone& v = TZV_ZONES[i];
      if (!z || !sameText(z->iana, v.iana) || !sameText(z->display, v.display) || z->stdMinutes != v.std ||
          z->rule != v.rule || z->sinceUtc != v.since || !sameText(z->abbrStd, v.abbrStd) ||
          !sameText(z->abbrDst, v.abbrDst)) {
        printf("    zone %d differs: %s\n", i, v.iana);
        same = false;
      }
      if (z && strlen(z->display) > 16) { printf("    display too long: %s\n", z->display); same = false; }
    }
    CHECK(same, "every zone: IANA, display (<= 16 chars), offset, rule, since, abbreviations");
    CHECK(tzGridZone(-1) == NULL && tzGridZone(TZV_NZONES) == NULL, "tzGridZone refuses a bad index");
    CHECK(tzGridFindZone("America/New_York") >= 0 && tzGridFindZone("Europe/London") == -1 &&
          tzGridFindZone(NULL) == -1, "tzGridFindZone");
    const int ny = tzGridFindZone("America/New_York"), to = tzGridFindZone("America/Toronto"),
              ch = tzGridFindZone("America/Chicago"), ph = tzGridFindZone("America/Phoenix"),
              va = tzGridFindZone("America/Vancouver"), ha = tzGridFindZone("America/Havana"),
              re = tzGridFindZone("America/Regina"), ed = tzGridFindZone("America/Edmonton");
    CHECK(tzGridSameClock(ny, to) && !tzGridSameClock(ny, ch), "same clock: New York = Toronto != Chicago");
    CHECK(tzGridSameClock(ph, va) && tzGridSameClock(re, ed), "same clock: Phoenix = Vancouver, Regina = Edmonton (tzdata 2026d)");
    CHECK(!tzGridSameClock(ha, ha) && !tzGridSameClock(ny, -1), "an UNKNOWN zone or a bad index has no clock");
    CHECK(!tzGridSameClock(ph, tzGridFindZone("America/Denver")) && !tzGridSameClock(re, ch),
          "same standard offset, different rule: Phoenix != Denver, Regina != Chicago");
    const TzZone* z = tzGridZone(ny);
    CHECK(z && strcmp(z->display, "Eastern") == 0 && z->stdMinutes == -300 && z->rule == TZ_RULE_USCAN &&
          strcmp(z->abbrStd, "EST") == 0 && strcmp(z->abbrDst, "EDT") == 0 && z->sinceUtc == 0,
          "America/New_York = Eastern, -300, US/Canada rule, EST/EDT");
    z = tzGridZone(tzGridFindZone("America/St_Johns"));
    CHECK(z && z->stdMinutes == -210 && z->rule == TZ_RULE_USCAN, "Newfoundland: -3:30 with the rule");
    z = tzGridZone(va);
    CHECK(z && z->rule == TZ_RULE_NONE && z->stdMinutes == -420 && z->sinceUtc == 1772964000LL,
          "Vancouver: UTC-7 all year since 2026-03-08 10:00 UTC");
  }

  // ---- named places ----------------------------------------------------------------------------
  {
    const int n = (int)(sizeof TZV_NAMED / sizeof TZV_NAMED[0]);
    const int64_t jan27 = 1800014400LL, jul27 = 1815652800LL;   // 2027-01-15 / 2027-07-15 12:00 UTC
    int zones = 0, clocks = 0;
    for (int i = 0; i < n; i++) {
      const TzvLookup& v = TZV_NAMED[i];
      uint8_t why = 99;
      const int a = tzGridLookup(v.lat, v.lon, &why);
      CHECKQ(a == v.answer && why == v.why, "%s (%.2f, %.2f): %d %s, want %d %s", v.name, v.lat / 1e7,
             v.lon / 1e7, a, whyName(why), v.answer, whyName(v.why));
      CHECKQ(tzGridLookup(v.lat, v.lon, NULL) == a, "%s: why=NULL changes the answer", v.name);
      if (a >= 0) {
        zones++;
        CHECKQ(strcmp(tzGridZone(a)->iana, v.iana) == 0, "%s: %s, want %s", v.name, tzGridZone(a)->iana, v.iana);
        if (v.ianaExact) CHECKQ(strcmp(v.iana, v.tzid) == 0, "%s: exact cell names %s, polygon %s", v.name, v.iana, v.tzid);
        if (v.jan != -32768) {
          clocks++;
          CHECKQ(tzUtcOffsetMinutes(a, jan27) == v.jan && tzUtcOffsetMinutes(a, jul27) == v.jul,
                 "%s: clock %d/%d, but %s there is %d/%d", v.name, tzUtcOffsetMinutes(a, jan27),
                 tzUtcOffsetMinutes(a, jul27), v.tzid, v.jan, v.jul);
        }
      }
    }
    printf("    %d named places, %d answered a zone, %d of them checked against zoneinfo's clock there\n", n,
           zones, clocks);
    CHECK(true, "named places (capitals, cities, split states, borders, sea, outside, bad input)");

    // the antimeridian: 180 E and 180 W are one meridian; 172 E is the grid's first column
    bool wrapSame = true;
    for (int32_t lat = 70000000; lat < 720000000; lat += 1234567) {
      uint8_t w1, w2;
      if (tzGridLookup(lat, 1800000000, &w1) != tzGridLookup(lat, -1800000000, &w2) || w1 != w2) wrapSame = false;
    }
    CHECK(wrapSame, "180 E answers exactly what 180 W does, at every latitude");
    uint8_t w;
    CHECK(tzGridLookup(520000000, 1719999999, &w) == TZ_NOT_COVERED && w == TZ_WHY_OUTSIDE,
          "one unit west of 172 E is outside");
  }

  // ---- random and near-boundary points: the decoder against the generator's grid --------------
  {
    const int n = (int)(sizeof TZV_POINTS / sizeof TZV_POINTS[0]);
    int counts[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < n; i++) {
      const TzvPoint& p = TZV_POINTS[i];
      uint8_t why = 99;
      const int a = tzGridLookup(p.lat, p.lon, &why);
      CHECKQ(a == p.answer && why == p.why, "point %d (%.2f, %.2f): %d %s, want %d %s", i, p.lat / 1e7,
             p.lon / 1e7, a, whyName(why), p.answer, whyName(p.why));
      if (why < 6) counts[why]++;
    }
    printf("    %d points: %d zone, %d border, %d water, %d outside, %d unknown rule\n", n, counts[0], counts[1],
           counts[2], counts[3], counts[4]);
    CHECK(counts[0] > 1000 && counts[1] > 1000 && counts[2] > 500, "the points exercise zones, borders and sea");
  }

  // ---- offsets at every DST change 2026-2030, from zoneinfo ------------------------------------
  {
    const int n = (int)(sizeof TZV_OFFSETS / sizeof TZV_OFFSETS[0]);
    int prevZone = -2;
    const char* prevName = "";
    for (int i = 0; i < n; i++) {
      const TzvOffset& v = TZV_OFFSETS[i];
      if (strcmp(v.iana, prevName) != 0) { prevZone = tzGridFindZone(v.iana); prevName = v.iana; }
      const int got = tzUtcOffsetMinutes(prevZone, v.utc);
      CHECKQ(got == v.minutes, "%s at %lld: %d, want %d", v.iana, (long long)v.utc, got, v.minutes);
    }
    printf("    %d offsets\n", n);
    CHECK(true, "every zone's offset one second before, at and after each DST change 2026-2030");
  }

  // ---- the next transition, from zoneinfo ------------------------------------------------------
  {
    const int n = (int)(sizeof TZV_NEXT / sizeof TZV_NEXT[0]);
    for (int i = 0; i < n; i++) {
      const TzvNext& v = TZV_NEXT[i];
      int after = 12345;
      const int64_t t = tzNextTransition(tzGridFindZone(v.iana), v.utc, &after);
      CHECKQ(t == v.next && (v.next == 0 || after == v.after), "%s next after %lld: %lld (%d), want %lld (%d)",
             v.iana, (long long)v.utc, (long long)t, after, (long long)v.next, v.after);
      CHECKQ(tzNextTransition(tzGridFindZone(v.iana), v.utc, NULL) == t, "%s: offsetAfter=NULL", v.iana);
    }
    printf("    %d next-transition rows\n", n);
    CHECK(true, "the next transition (and the offset after it) for every zone");
  }

  // ---- the rule's dates 1900-2200 by a second calendar -----------------------------------------
  {
    bool ok = true;
    const int stds[] = {-600, -540, -480, -420, -360, -300, -240, -210, -180, 0, 330, 720};
    for (long y = 1900; y <= 2200 && ok; y++) {
      const int ms = nthSundayDay(y, 3, 2), ns = nthSundayDay(y, 11, 1);
      if (ms < 8 || ms > 14 || ns < 1 || ns > 7) ok = false;
      for (size_t k = 0; k < sizeof stds / sizeof stds[0]; k++) {
        const int s = stds[k];
        const long long wantStart = naiveDays(y, 3, ms) * 86400LL + 7200 - s * 60LL;
        const long long wantEnd = naiveDays(y, 11, ns) * 86400LL + 7200 - (s + 60) * 60LL;
        if (tzUsCanDstStart((int)y, s) != wantStart || tzUsCanDstEnd((int)y, s) != wantEnd) {
          printf("    %ld std %d: start %lld want %lld, end %lld want %lld\n", y, s, (long long)tzUsCanDstStart((int)y, s),
                 wantStart, (long long)tzUsCanDstEnd((int)y, s), wantEnd);
          ok = false;
          break;
        }
        // and the offset function flips at exactly those seconds
        if (tzRuleOffsetMinutes(s, TZ_RULE_USCAN, wantStart - 1) != s ||
            tzRuleOffsetMinutes(s, TZ_RULE_USCAN, wantStart) != s + 60 ||
            tzRuleOffsetMinutes(s, TZ_RULE_USCAN, wantEnd - 1) != s + 60 ||
            tzRuleOffsetMinutes(s, TZ_RULE_USCAN, wantEnd) != s) {
          printf("    %ld std %d: the offset does not flip at the rule's instants\n", y, s);
          ok = false;
          break;
        }
      }
    }
    CHECK(ok, "US/Canada dates 1900-2200, 12 offsets: = a naive day count + Zeller, flips to the second");
    CHECK(tzUsCanDstStart(2026, -300) == 1772953200LL && tzUsCanDstEnd(2026, -300) == 1793512800LL,
          "Eastern 2026: 2026-03-08 07:00 UTC .. 2026-11-01 06:00 UTC");
    CHECK(tzUsCanDstStart(2027, -210) == 1805002200LL, "Newfoundland 2027: 2027-03-14 05:30 UTC");
  }

  // ---- refusals and edges -----------------------------------------------------------------------
  {
    const int ny = tzGridFindZone("America/New_York");
    const int va = tzGridFindZone("America/Vancouver");
    CHECK(tzUtcOffsetMinutes(-1, 1800000000LL) == TZ_NO_OFFSET &&
          tzUtcOffsetMinutes(tzGridZoneCount(), 1800000000LL) == TZ_NO_OFFSET, "bad zone index: no offset");
    CHECK(tzUtcOffsetMinutes(tzGridFindZone("America/Havana"), 1800000000LL) == TZ_NO_OFFSET &&
          tzNextTransition(tzGridFindZone("America/Havana"), 1800000000LL, NULL) == 0, "UNKNOWN rule: no offset, no transition");
    CHECK(tzUtcOffsetMinutes(va, 1772963999LL) == TZ_NO_OFFSET && tzUtcOffsetMinutes(va, 1772964000LL) == -420 &&
          tzNextTransition(va, 1772963999LL, NULL) == 0, "before a zone's since: refused");
    CHECK(tzUtcOffsetMinutes(ny, (1LL << 40)) == TZ_NO_OFFSET && tzUtcOffsetMinutes(ny, -(1LL << 40)) == TZ_NO_OFFSET &&
          tzUtcOffsetMinutes(ny, (1LL << 40) - 1) != TZ_NO_OFFSET, "|utc| >= 2^40: refused, just inside: answered");
    CHECK(tzRuleOffsetMinutes(-1441, TZ_RULE_NONE, 0) == TZ_NO_OFFSET && tzRuleOffsetMinutes(1440, TZ_RULE_NONE, 0) == 1440 &&
          tzRuleOffsetMinutes(-300, TZ_RULE_UNKNOWN, 0) == TZ_NO_OFFSET && tzRuleOffsetMinutes(-300, 7, 0) == TZ_NO_OFFSET,
          "rule arguments: offsets beyond 24 h and unknown rules refused");
    CHECK(tzRuleNextTransition(-300, TZ_RULE_NONE, 0, NULL) == 0, "no DST: no next transition");
    // a negative instant (1969) goes through the floor division the same way
    CHECK(tzRuleOffsetMinutes(-300, TZ_RULE_USCAN, -1) == -300 &&
          tzRuleOffsetMinutes(-300, TZ_RULE_USCAN, tzUsCanDstStart(1969, -300)) == -240,
          "before 1970: the rule as written (floor division, not truncation)");
    // a whole year walked hour by hour changes offset exactly twice
    int changes = 0, prev = tzUtcOffsetMinutes(ny, 1767225600LL);
    for (int64_t t = 1767225600LL; t < 1798761600LL; t += 3600) {
      const int o = tzUtcOffsetMinutes(ny, t);
      if (o != prev) changes++;
      prev = o;
    }
    CHECK(changes == 2, "Eastern 2026 hour by hour: exactly two changes");
    for (int w = 0; w < 6; w++) CHECKQ(strcmp(tzWhyText((uint8_t)w), "?") != 0, "tzWhyText(%d)", w);
    CHECK(strcmp(tzWhyText(6), "?") == 0, "tzWhyText words for every reason");
  }

  // ---- speed (information only) --------------------------------------------------------------------
  {
    srand(20261010);
    const int N = 1000000;
    volatile long sink = 0;
    clock_t c0 = clock();
    for (int i = 0; i < N; i++) {
      int32_t lat = 70000000 + (int32_t)((rand() % 65000) * 10000);
      int32_t lon = -1700000000 + (int32_t)((rand() % 120000) * 10000);
      sink += tzGridLookup(lat, lon, NULL);
    }
    double us = (double)(clock() - c0) / CLOCKS_PER_SEC * 1e6 / N;
    printf("    lookup: %.3f us on this Mac (ASan build)\n", us);
    (void)sink;
  }

  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
