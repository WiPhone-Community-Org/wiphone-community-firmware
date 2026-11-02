/*
 * tz_grid.cpp - see tz_grid.h. The grid's encoding (tools/gen_tz_grid.py writes it):
 *
 *   TZGRID_RUNS        uint16 runs, row after row, each value << 9 | (length - 1):
 *                      value 0..125 = zone index, 126 = WATER, 127 = BORDER; length 1..512
 *   TZGRID_ROW_START   uint32 index of each row's first run; ROWS + 1 entries, the last is NRUNS
 *   row r, column c    the cell [LAT0 + r*RES, LAT0 + (r+1)*RES) x [LON0 + c*RES, ...), in
 *                      1e-7 degrees, half-open: a point on a cell edge belongs to the cell
 *                      north/east of it, so the cell of a point is pure integer division
 *
 * The data is ODbL 1.0 (THIRD-PARTY-NOTICES.md); this code is not.
 */
#include "tz_grid.h"
#include "tz_grid_data.h"

#include <string.h>

static const int64_t E7_360 = 3600000000LL;    // 360 deg in 1e-7 deg

// Beyond 2^40 s (~34,800 years) the day arithmetic would need care for overflow, and no
// clock that matters is there. Refuse instead.
static const int64_t MAX_ABS_UTC = 1LL << 40;

static const TzGridInfo INFO = {
  TZGRID_RELEASE, TZGRID_TZDATA, TZGRID_SHA256, TZGRID_ATTRIBUTION_TEXT,
  TZGRID_VALID_FROM, TZGRID_VALID_TO,
  TZGRID_LAT0_E7, TZGRID_LON0_E7, TZGRID_RES_E7,
  TZGRID_ROWS, TZGRID_COLS, TZGRID_NZONES, TZGRID_NRUNS,
};

const TzGridInfo* tzGridInfo() { return &INFO; }

int tzGridZoneCount() { return TZGRID_NZONES; }

const TzZone* tzGridZone(int zone) {
  if (zone < 0 || zone >= TZGRID_NZONES) return NULL;
  return &TZGRID_ZONES[zone];
}

int tzGridFindZone(const char* iana) {
  if (!iana) return -1;
  for (int i = 0; i < TZGRID_NZONES; i++)
    if (strcmp(TZGRID_ZONES[i].iana, iana) == 0) return i;
  return -1;
}

const char* tzWhyText(uint8_t why) {
  switch (why) {
    case TZ_WHY_ZONE:         return "time zone found";
    case TZ_WHY_BORDER:       return "near a time zone boundary";
    case TZ_WHY_WATER:        return "at sea";
    case TZ_WHY_OUTSIDE:      return "outside the time zone map";
    case TZ_WHY_UNKNOWN_RULE: return "this zone's rules are not covered";
    case TZ_WHY_BAD_INPUT:    return "no valid position";
  }
  return "?";
}

int tzGridLookup(int32_t latE7, int32_t lonE7, uint8_t* why) {
  uint8_t w;
  int answer = TZ_NOT_COVERED;
  if (latE7 < -900000000 || latE7 > 900000000 || lonE7 < -1800000000 || lonE7 > 1800000000) {
    w = TZ_WHY_BAD_INPUT;
  } else {
    // 172 E..180 E is the grid's westmost 8 degrees (the far Aleutians): read it as lon - 360
    int64_t lon = lonE7;
    if (lon - (int64_t)TZGRID_LON0_E7 >= E7_360) lon -= E7_360;
    const int64_t dy = (int64_t)latE7 - TZGRID_LAT0_E7;
    const int64_t dx = lon - TZGRID_LON0_E7;
    const int64_t r = dy / TZGRID_RES_E7, c = dx / TZGRID_RES_E7;   // both >= 0 when used
    if (dy < 0 || dx < 0 || r >= TZGRID_ROWS || c >= TZGRID_COLS) {
      w = TZ_WHY_OUTSIDE;
    } else {
      int value = -1;
      uint32_t end = TZGRID_ROW_START[r + 1];
      int64_t col = 0;
      for (uint32_t k = TZGRID_ROW_START[r]; k < end; k++) {
        const uint16_t run = TZGRID_RUNS[k];
        col += (run & 511) + 1;
        if (c < col) { value = run >> 9; break; }
      }
      // A row that ends short of column c cannot happen (the test sums every row); if it
      // ever did, "outside" is the answer that changes nothing.
      if (value < 0) {
        w = TZ_WHY_OUTSIDE;
      } else if (value == TZGRID_CODE_BORDER) {
        answer = TZ_BORDER;
        w = TZ_WHY_BORDER;
      } else if (value == TZGRID_CODE_WATER) {
        w = TZ_WHY_WATER;
      } else if (value >= TZGRID_NZONES || (TZGRID_ZONES[value].rule != TZ_RULE_NONE &&
                                               TZGRID_ZONES[value].rule != TZ_RULE_USCAN)) {
        w = TZ_WHY_UNKNOWN_RULE;
      } else {
        answer = value;
        w = TZ_WHY_ZONE;
      }
    }
  }
  if (why) *why = w;
  return answer;
}

bool tzGridSameClock(int a, int b) {
  const TzZone* za = tzGridZone(a);
  const TzZone* zb = tzGridZone(b);
  if (!za || !zb) return false;
  if (za->rule != TZ_RULE_NONE && za->rule != TZ_RULE_USCAN) return false;
  return za->rule == zb->rule && za->stdMinutes == zb->stdMinutes;
}

int tzUtcOffsetMinutes(int zone, int64_t utcSeconds) {
  const TzZone* z = tzGridZone(zone);
  if (!z) return TZ_NO_OFFSET;
  if (z->sinceUtc != 0 && utcSeconds < z->sinceUtc) return TZ_NO_OFFSET;
  return tzRuleOffsetMinutes(z->stdMinutes, z->rule, utcSeconds);
}

int64_t tzNextTransition(int zone, int64_t utcSeconds, int* offsetAfter) {
  const TzZone* z = tzGridZone(zone);
  if (!z || (z->sinceUtc != 0 && utcSeconds < z->sinceUtc)) return 0;
  return tzRuleNextTransition(z->stdMinutes, z->rule, utcSeconds, offsetAfter);
}

// ---- the calendar, from first principles -------------------------------------------------
// Howard Hinnant's days_from_civil / civil_from_days (proleptic Gregorian, day 0 =
// 1970-01-01), which are exact for every year an int64 day count can hold. 1970-01-01 was
// a Thursday.

static int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}

static int64_t daysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = floorDiv(y, 400);
  const int64_t yoe = y - era * 400;                                     // 0..399
  const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;  // 0..365
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // 0..146096
  return era * 146097 + doe - 719468;
}

static int64_t yearFromDays(int64_t z) {
  z += 719468;
  const int64_t era = floorDiv(z, 146097);
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;                                // 0 = March
  return yoe + era * 400 + (mp >= 10 ? 1 : 0);                           // Jan, Feb: next year
}

// The day number of the n-th Sunday of a month (n >= 1)
static int64_t nthSunday(int64_t year, int month, int n) {
  const int64_t first = daysFromCivil(year, month, 1);
  const int64_t weekday = (first + 4) - floorDiv(first + 4, 7) * 7;   // 0 = Sunday
  return first + (7 - weekday) % 7 + 7 * (n - 1);
}

int64_t tzUsCanDstStart(int year, int stdMinutes) {
  return nthSunday(year, 3, 2) * 86400 + 2 * 3600 - (int64_t)stdMinutes * 60;
}

int64_t tzUsCanDstEnd(int year, int stdMinutes) {
  return nthSunday(year, 11, 1) * 86400 + 2 * 3600 - ((int64_t)stdMinutes + 60) * 60;
}

static bool ruleArgsOk(int stdMinutes, int64_t utcSeconds) {
  return stdMinutes >= -1440 && stdMinutes <= 1440 &&
         utcSeconds > -MAX_ABS_UTC && utcSeconds < MAX_ABS_UTC;
}

// The year of the LOCAL STANDARD date at t. Neither transition is near New Year, so the
// local-vs-UTC year question never decides a transition. Standard time keeps it exact anyway.
static int localYear(int stdMinutes, int64_t utcSeconds) {
  return (int)yearFromDays(floorDiv(utcSeconds + (int64_t)stdMinutes * 60, 86400));
}

int tzRuleOffsetMinutes(int stdMinutes, uint8_t rule, int64_t utcSeconds) {
  if (!ruleArgsOk(stdMinutes, utcSeconds)) return TZ_NO_OFFSET;
  if (rule == TZ_RULE_NONE) return stdMinutes;
  if (rule != TZ_RULE_USCAN) return TZ_NO_OFFSET;
  const int y = localYear(stdMinutes, utcSeconds);
  const bool dst = utcSeconds >= tzUsCanDstStart(y, stdMinutes) && utcSeconds < tzUsCanDstEnd(y, stdMinutes);
  return dst ? stdMinutes + 60 : stdMinutes;
}

int64_t tzRuleNextTransition(int stdMinutes, uint8_t rule, int64_t utcSeconds, int* offsetAfter) {
  if (!ruleArgsOk(stdMinutes, utcSeconds) || rule != TZ_RULE_USCAN) return 0;
  const int y = localYear(stdMinutes, utcSeconds);
  int64_t t;
  int after;
  if (utcSeconds < tzUsCanDstStart(y, stdMinutes)) {
    t = tzUsCanDstStart(y, stdMinutes); after = stdMinutes + 60;
  } else if (utcSeconds < tzUsCanDstEnd(y, stdMinutes)) {
    t = tzUsCanDstEnd(y, stdMinutes); after = stdMinutes;
  } else {
    t = tzUsCanDstStart(y + 1, stdMinutes); after = stdMinutes + 60;
  }
  if (offsetAfter) *offsetAfter = after;
  return t;
}
