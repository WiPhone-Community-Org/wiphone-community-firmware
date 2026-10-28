/* clock_source.cpp — see clock_source.h for every rule and the reasons for it. This file is
 * only the arithmetic; nothing here reads a clock, takes a lock or logs. */
#include "clock_source.h"

#include <string.h>

const char* clockSourceName(int src) {
  switch (src) {
  case CLOCK_SRC_NTP:
    return "ntp";
  case CLOCK_SRC_GPS:
    return "gps";
  case CLOCK_SRC_MESH:
    return "mesh";
  default:
    return "none";
  }
}

bool clockSourceTrusted(int src) {
  return src == CLOCK_SRC_NTP || src == CLOCK_SRC_GPS;
}

bool clockUnixSane(uint32_t unixSec) {
  return unixSec >= CLOCK_UNIX_MIN && unixSec < CLOCK_UNIX_LIMIT;
}

bool clockNtpReplySane(const uint8_t* pkt, int len, uint32_t* unixOut) {
  if (!pkt || len < 48) {
    return false;
  }
  const int li      = pkt[0] >> 6;
  const int mode    = pkt[0] & 7;
  const int stratum = pkt[1];
  if (li == 3 || mode != 4 || stratum < 1 || stratum > 15) {
    return false;
  }
  const uint32_t ntp = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                       ((uint32_t)pkt[42] << 8) | (uint32_t)pkt[43];
  if (ntp < CLOCK_NTP_1900_TO_1970) {
    return false;                              // before 1970: the subtraction would wrap
  }
  const uint32_t unixSec = ntp - CLOCK_NTP_1900_TO_1970;
  if (!clockUnixSane(unixSec)) {
    return false;
  }
  if (unixOut) {
    *unixOut = unixSec;
  }
  return true;
}

static bool isLeap(int y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int daysInMonth(int y, int m) {
  static const uint8_t DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  return (m == 2 && isLeap(y)) ? 29 : DAYS[m - 1];
}

/* Days since 1970-01-01 of a proleptic-Gregorian date (Howard Hinnant's days_from_civil —
 * exact for every date, no tables, no loops). Only called with a validated date. */
static int64_t daysFromCivil(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;                                   // [0, 399]
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + doe - 719468;
}

bool clockCivilToUnixMs(int year, int month, int day, int hour, int minute, int second, int ms,
                        int64_t* outMs) {
  if (year < CLOCK_YEAR_MIN || year >= CLOCK_YEAR_LIMIT) {
    return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) {
    return false;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 ||
      ms < 0 || ms > 999) {
    return false;
  }
  const int64_t s = daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
  if (outMs) {
    *outMs = s * 1000 + ms;
  }
  return true;
}

const char* clockVerdictText(int v) {
  switch (v) {
  case CLK_NONE:               return "nothing yet";
  case CLK_GPS_NO_FIX:         return "no fix (RMC status V) - pre-fix time is not used";
  /* (also an 'A' whose coordinates would not parse: see clockGpsRmcTime) */
  case CLK_GPS_BAD_MODE:       return "RMC mode is not a GNSS solution (N/E/M/S)";
  case CLK_GPS_NO_TIME:        return "RMC has no usable time";
  case CLK_GPS_NO_DATE:        return "RMC has no usable date";
  case CLK_GPS_BAD_DATE:       return "RMC date/time is not a real instant";
  case CLK_GPS_BAD_YEAR:       return "RMC year outside 2026-2079 (rollover or default?)";
  case CLK_GPS_FIRST:          return "one good reading, waiting for the next to agree";
  case CLK_GPS_NOT_LATER:      return "next reading did not move forward";
  case CLK_GPS_GAP:            return "too long between readings";
  case CLK_GPS_DISAGREE:       return "two readings disagree with millis() (loop stall?)";
  case CLK_GPS_SET_UNKNOWN:    return "SET - the clock was not set";
  case CLK_GPS_SET_UPGRADE:    return "SET - replaced a mesh time";
  case CLK_GPS_SET_DRIFT:      return "SET - clock was off by more than 2 s";
  case CLK_GPS_KEEP_AGREES:    return "kept - agrees within 2 s";
  case CLK_GPS_KEEP_NTP_FRESH: return "kept - NTP set it recently and stays in charge";
  case CLK_MESH_NO_TIME:       return "position carried no time";
  case CLK_MESH_BAD_YEAR:      return "position time outside 2026-2079";
  case CLK_MESH_BAD_NODE:      return "not a real sender";
  case CLK_MESH_CLOCK_KNOWN:   return "clock already set - mesh never overrides";
  case CLK_MESH_WAIT:          return "public channel - held until a second node agrees";
  case CLK_MESH_SET_PRIVATE:   return "SET - one position on a private channel";
  case CLK_MESH_SET_AGREED:    return "SET - two nodes agreed within 60 s";
  default:                     return "?";
  }
}

bool clockVerdictSets(int v) {
  return v == CLK_GPS_SET_UNKNOWN || v == CLK_GPS_SET_UPGRADE || v == CLK_GPS_SET_DRIFT ||
         v == CLK_MESH_SET_PRIVATE || v == CLK_MESH_SET_AGREED;
}

// ---------------------------------------------------------------- GPS, step 1

bool clockGpsRmcTime(const NmeaFix* fx, int64_t* utcMs, int* why) {
  int w = CLK_NONE;
  int64_t ms = 0;
  /* `valid` is this same RMC's 'A' AND readable coordinates (nmea.cpp): an 'A' whose position
   * we could not parse is a sentence we did not understand, and its time goes with it. */
  if (!fx || fx->rmcStatus != 'A' || !fx->valid) {
    w = CLK_GPS_NO_FIX;             // see THE NOTE ON 'V' in the header
  } else if (fx->rmcMode != 0 && fx->rmcMode != 'A' && fx->rmcMode != 'D' &&
             fx->rmcMode != 'F' && fx->rmcMode != 'R' && fx->rmcMode != 'P') {
    w = CLK_GPS_BAD_MODE;
  } else if (fx->rmcHms < 0) {
    w = CLK_GPS_NO_TIME;
  } else if (fx->rmcDmy < 0) {
    w = CLK_GPS_NO_DATE;
  } else {
    const int hh = fx->rmcHms / 10000, mi = (fx->rmcHms / 100) % 100, ss = fx->rmcHms % 100;
    const int dd = fx->rmcDmy / 10000, mo = (fx->rmcDmy / 100) % 100, yy = fx->rmcDmy % 100;
    /* NMEA's year is two digits. 2000 + yy covers everything this firmware can meet; a
     * rollover date (2006/2007) lands below 2026 and is refused as BAD_YEAR, not as a bad
     * date — the distinction is the whole diagnosis. 🛑 So does a receiver DEFAULT: 80..99
     * (`060180` is the 1980 GPS epoch) reads as 2080..2099, above CLOCK_YEAR_LIMIT — see
     * SANE YEARS in the header for why that ceiling is 2080 and not 2100. */
    const int year = 2000 + yy;
    if (year < CLOCK_YEAR_MIN || year >= CLOCK_YEAR_LIMIT) {
      w = CLK_GPS_BAD_YEAR;
    } else if (!clockCivilToUnixMs(year, mo, dd, hh, mi, ss, fx->rmcMs, &ms)) {
      w = CLK_GPS_BAD_DATE;
    }
  }
  if (why) {
    *why = w;
  }
  if (w != CLK_NONE) {
    return false;
  }
  if (utcMs) {
    *utcMs = ms;
  }
  return true;
}

// ---------------------------------------------------------------- GPS, step 2

void clockGpsPairReset(ClockGpsPair* p) {
  if (p) {
    memset(p, 0, sizeof(*p));
  }
}

bool clockGpsPairFeed(ClockGpsPair* p, int64_t utcMs, uint32_t rxMs, int64_t* utcAtRxMs,
                      int* why) {
  int w;
  bool ok = false;
  if (!p->have) {
    w = CLK_GPS_FIRST;
  } else {
    /* millis() wraps every 49.7 days; the unsigned difference does not care. */
    const uint32_t dRx = rxMs - p->rxMs;
    const int64_t dUtc = utcMs - p->utcMs;
    if (dUtc <= 0) {
      w = CLK_GPS_NOT_LATER;
    } else if (dRx > CLOCK_GPS_PAIR_MAX_GAP_MS) {
      w = CLK_GPS_GAP;
    } else {
      const int64_t err = dUtc - (int64_t)dRx;
      if (err > CLOCK_GPS_PAIR_TOL_MS || err < -CLOCK_GPS_PAIR_TOL_MS) {
        w = CLK_GPS_DISAGREE;
      } else {
        /* Both readings, carried to this one's arrival. A read is only ever LATE, and a
         * late read makes its estimate SMALLER, so the larger one is the truer. */
        const int64_t fromPrev = p->utcMs + (int64_t)dRx;
        const int64_t best = fromPrev > utcMs ? fromPrev : utcMs;
        if (utcAtRxMs) {
          *utcAtRxMs = best;
        }
        w = CLK_NONE;
        ok = true;
      }
    }
  }
  // Whatever happened, THIS reading is the one the next pairs with.
  p->have = true;
  p->utcMs = utcMs;
  p->rxMs = rxMs;
  if (why) {
    *why = w;
  }
  return ok;
}

// ---------------------------------------------------------------- GPS, step 3

int clockGpsDecide(const ClockNow* c, int64_t gpsUtcMs, int64_t* diffMs) {
  if (diffMs) {
    *diffMs = 0;
  }
  if (!c || c->source == CLOCK_SRC_NONE) {
    return CLK_GPS_SET_UNKNOWN;
  }
  const int64_t d = gpsUtcMs - c->utcMs;
  if (diffMs) {
    *diffMs = d;
  }
  if (c->source == CLOCK_SRC_MESH) {
    return CLK_GPS_SET_UPGRADE;
  }
  if (c->msSinceNtp < CLOCK_NTP_FRESH_MS) {
    return CLK_GPS_KEEP_NTP_FRESH;
  }
  if (d > CLOCK_GPS_STEP_MS || d < -CLOCK_GPS_STEP_MS) {
    return CLK_GPS_SET_DRIFT;
  }
  return CLK_GPS_KEEP_AGREES;
}

int clockGpsStep(ClockGpsPair* p, const NmeaFix* fx, uint32_t rxMs, const ClockNow* now,
                 int64_t* setUtcMs, int64_t* diffMs) {
  if (diffMs) {
    *diffMs = 0;
  }
  int why = CLK_NONE;
  int64_t utc = 0;
  if (!clockGpsRmcTime(fx, &utc, &why)) {
    clockGpsPairReset(p);           // consecutive means consecutive
    return why;
  }
  int64_t atRx = 0;
  if (!clockGpsPairFeed(p, utc, rxMs, &atRx, &why)) {
    return why;
  }
  const int v = clockGpsDecide(now, atRx, diffMs);
  if (clockVerdictSets(v) && setUtcMs) {
    *setUtcMs = atRx;
  }
  return v;
}

// ---------------------------------------------------------------- mesh

void clockMeshVoteReset(ClockMeshVote* v) {
  if (v) {
    memset(v, 0, sizeof(*v));
  }
}

static bool candLive(const ClockMeshCand* c, uint32_t nowMs) {
  return c->node != 0 && (uint32_t)(nowMs - c->rxMs) <= CLOCK_MESH_CAND_MAX_AGE_MS;
}

int clockMeshVoteCount(const ClockMeshVote* v, uint32_t nowMs) {
  int n = 0;
  for (int i = 0; v && i < CLOCK_MESH_CAND_MAX; i++) {
    if (candLive(&v->c[i], nowMs)) {
      n++;
    }
  }
  return n;
}

int clockMeshOffer(ClockMeshVote* v, int clockSource, uint32_t node, bool privateChannel,
                   uint32_t t, uint32_t rxMs, uint32_t* adoptUtc) {
  if (clockSource != CLOCK_SRC_NONE) {
    return CLK_MESH_CLOCK_KNOWN;    // first: a set clock is not even a question
  }
  if (t == 0) {
    return CLK_MESH_NO_TIME;
  }
  if (!clockUnixSane(t)) {
    return CLK_MESH_BAD_YEAR;
  }
  if (node == 0 || node == 0xFFFFFFFFu) {
    return CLK_MESH_BAD_NODE;
  }
  if (privateChannel) {
    if (adoptUtc) {
      *adoptUtc = t;
    }
    clockMeshVoteReset(v);
    return CLK_MESH_SET_PRIVATE;
  }
  // Public: does a DIFFERENT node, heard within the hour, agree?
  for (int i = 0; i < CLOCK_MESH_CAND_MAX; i++) {
    const ClockMeshCand* c = &v->c[i];
    if (!candLive(c, rxMs) || c->node == node) {
      continue;
    }
    const int64_t predicted = (int64_t)c->t + (int64_t)((uint32_t)(rxMs - c->rxMs) / 1000u);
    const int64_t d = (int64_t)t - predicted;
    if (d <= CLOCK_MESH_AGREE_S && d >= -CLOCK_MESH_AGREE_S) {
      if (adoptUtc) {
        *adoptUtc = t;              // the newer of the two: it has aged least
      }
      clockMeshVoteReset(v);
      return CLK_MESH_SET_AGREED;
    }
  }
  /* Hold it: this node's slot if it has one (its newest word replaces its older one — one
   * node can never be two witnesses), else an empty or expired slot, else the oldest. */
  int slot = -1;
  for (int i = 0; i < CLOCK_MESH_CAND_MAX && slot < 0; i++) {
    if (v->c[i].node == node) {
      slot = i;
    }
  }
  for (int i = 0; i < CLOCK_MESH_CAND_MAX && slot < 0; i++) {
    if (!candLive(&v->c[i], rxMs)) {
      slot = i;
    }
  }
  if (slot < 0) {
    slot = 0;
    for (int i = 1; i < CLOCK_MESH_CAND_MAX; i++) {
      if ((uint32_t)(rxMs - v->c[i].rxMs) > (uint32_t)(rxMs - v->c[slot].rxMs)) {
        slot = i;
      }
    }
  }
  v->c[slot].node = node;
  v->c[slot].t = t;
  v->c[slot].rxMs = rxMs;
  return CLK_MESH_WAIT;
}

// ---------------------------------------------------------------- the SIP message store (SA-3)

int32_t clockMeshCorrS(int64_t trustedMs, int64_t meshMs) {
  const int64_t d = trustedMs - meshMs;
  int64_t s = (d >= 0) ? (d + 500) / 1000 : -((-d + 500) / 1000);   // nearest, half away from 0
  if (s > 2147483647LL) {
    s = 2147483647LL;
  } else if (s < -2147483647LL - 1) {
    s = -2147483647LL - 1;
  }
  return (int32_t)s;
}

bool clockMsgFinal(uint32_t t, uint32_t id, const ClockMsgStamp* st, uint32_t* out) {
  if (!st || !out || st->utc == 0 || st->meshId != 0) {
    return false;               // not a trusted clock: nothing is final yet
  }
  int64_t f = t;
  if (id != 0 && id == st->corrId) {
    f += st->corrS;             // the mesh clock this boot's NTP/GPS replaced: its exact error
  }
  if (f < (int64_t)CLOCK_UNIX_MIN || f > (int64_t)st->utc) {
    return false;               // before this firmware existed, or in the future: re-stamp it
  }
  *out = (uint32_t)f;
  return true;
}

void clockMsgRestamp(const uint32_t* prov, uint32_t n, uint32_t bandEnd, uint32_t* out) {
  if (!prov || !out) {
    return;
  }
  for (uint32_t i = 0; i < n; i++) {
    uint32_t newer = 0;         // how many of the others came after text i
    for (uint32_t j = 0; j < n; j++) {
      if (prov[j] > prov[i] || (prov[j] == prov[i] && j > i)) {
        newer++;
      }
    }
    out[i] = bandEnd - newer;   // the newest (newer == 0) takes the top of the band
  }
}
