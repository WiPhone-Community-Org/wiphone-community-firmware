/*
 * test_clocksrc.cpp — who may set the phone's clock: GPS time and mesh time (clock_source.h).
 *
 * The GPS half is fed through the SHIPPING NMEA reader (nmea.cpp), sentence by sentence, the
 * way the woods plate's UART feeds it — so a pass covers the parse of the RMC's own time/date
 * fields AND the rules on top. The hardware cannot prove any of this yet: indoors the M100
 * reports "no fix yet (sats in view: 0)", and the rules that matter most (a 'V' sentence's
 * complete-looking time is refused, a pair must agree, a fresh NTP is never overridden) are
 * exactly the ones a lucky afternoon outdoors would never exercise.
 *
 * Every expected epoch below was computed INDEPENDENTLY in Python
 * (datetime(..., tzinfo=timezone.utc).timestamp()), not by the code under test:
 *   2026-01-01 00:00:00  1767225600      2080-01-01 00:00:00  3471292800
 *   2026-08-13 12:35:19  1786624519      2028-02-29 06:00:00  1835416800
 *   2026-08-13 23:59:59  1786665599      2026-08-14 00:00:01  1786665601
 *   2026-08-14 00:00:02  1786665602      2079-12-31 23:59:59  3471292799
 */

#include "../WiPhone/clock_source.h"
#include "../WiPhone/nmea.h"
#include <cstdio>
#include <cstring>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

/* "$<body>*hh\r\n" with the checksum computed here. The checksum is not what this suite tests
 * (test_nmea.cpp pins it against Python); building it keeps the vectors readable as bodies. */
static void sentence(const char* body, char* out, size_t cap) {
  unsigned x = 0;
  for (const char* p = body; *p; p++) {
    x ^= (unsigned char)*p;
  }
  snprintf(out, cap, "$%s*%02X\r\n", body, x);
}

// Feed one sentence body; true when the reader completed an RMC/GGA.
static bool feedBody(NmeaReader& r, const char* body) {
  char s[128];
  sentence(body, s, sizeof(s));
  bool hit = false;
  for (const char* p = s; *p; p++) {
    if (r.feed(*p)) {
      hit = true;
    }
  }
  return hit;
}

// An RMC body with the given time/status/date/mode (Seattle coordinates when status is A).
static void rmcBody(char* out, size_t cap, const char* hms, char status, const char* dmy,
                    const char* mode) {
  if (status == 'A') {
    snprintf(out, cap, "GNRMC,%s,A,4738.96912,N,12220.35817,W,0.36,,%s,,,%s", hms, dmy, mode);
  } else {
    snprintf(out, cap, "GNRMC,%s,%c,,,,,,,%s,,,%s", hms, status, dmy, mode);
  }
}

static int64_t judge(NmeaReader& r, const char* hms, char status, const char* dmy,
                     const char* mode, int* why) {
  char b[100];
  rmcBody(b, sizeof(b), hms, status, dmy, mode);
  feedBody(r, b);
  int64_t ms = -1;
  if (!clockGpsRmcTime(&r.fix(), &ms, why)) {
    return -1;
  }
  return ms;
}

/* One RMC through the whole path, arriving at millis() == rxMs, against `now`. */
static int step(NmeaReader& r, ClockGpsPair* p, const char* hms, char status, const char* dmy,
                uint32_t rxMs, const ClockNow* now, int64_t* setMs, int64_t* diffMs) {
  char b[100];
  rmcBody(b, sizeof(b), hms, status, dmy, "A");
  feedBody(r, b);
  return clockGpsStep(p, &r.fix(), rxMs, now, setMs, diffMs);
}

int main() {
  printf("test_clocksrc\n");

  // ---- sources and trust ------------------------------------------------------------
  {
    CHECK(!strcmp(clockSourceName(CLOCK_SRC_NONE), "none") &&
          !strcmp(clockSourceName(CLOCK_SRC_NTP), "ntp") &&
          !strcmp(clockSourceName(CLOCK_SRC_GPS), "gps") &&
          !strcmp(clockSourceName(CLOCK_SRC_MESH), "mesh"), "source names");
    CHECK(clockSourceTrusted(CLOCK_SRC_NTP) && clockSourceTrusted(CLOCK_SRC_GPS),
          "ntp and gps are trusted");
    CHECK(!clockSourceTrusted(CLOCK_SRC_MESH), "mesh is KNOWN but NOT trusted");
    CHECK(!clockSourceTrusted(CLOCK_SRC_NONE), "an unset clock is not trusted");
    CHECK(clockVerdictSets(CLK_GPS_SET_UNKNOWN) && clockVerdictSets(CLK_GPS_SET_UPGRADE) &&
          clockVerdictSets(CLK_GPS_SET_DRIFT) && clockVerdictSets(CLK_MESH_SET_PRIVATE) &&
          clockVerdictSets(CLK_MESH_SET_AGREED), "the five SET verdicts set");
    CHECK(!clockVerdictSets(CLK_GPS_KEEP_AGREES) && !clockVerdictSets(CLK_GPS_KEEP_NTP_FRESH) &&
          !clockVerdictSets(CLK_MESH_WAIT) && !clockVerdictSets(CLK_GPS_FIRST),
          "keep / wait verdicts do not");
    bool allNamed = true;
    for (int v = CLK_NONE; v <= CLK_MESH_SET_AGREED; v++) {
      if (!strcmp(clockVerdictText(v), "?")) {
        allNamed = false;
      }
    }
    CHECK(allNamed, "every verdict has words for `clock`");
  }

  // ---- the calendar -----------------------------------------------------------------
  {
    int64_t ms = 0;
    CHECK(clockCivilToUnixMs(2026, 1, 1, 0, 0, 0, 0, &ms) && ms == 1767225600000LL,
          "2026-01-01 = CLOCK_UNIX_MIN (python)");
    CHECK(ms / 1000 == CLOCK_UNIX_MIN, "CLOCK_UNIX_MIN constant matches the arithmetic");
    CHECK(clockCivilToUnixMs(2079, 12, 31, 23, 59, 59, 0, &ms) && ms == 3471292799000LL,
          "2079-12-31 23:59:59 accepted (python)");
    CHECK(ms / 1000 + 1 == CLOCK_UNIX_LIMIT, "CLOCK_UNIX_LIMIT is the next second");
    CHECK(clockCivilToUnixMs(2028, 2, 29, 6, 0, 0, 0, &ms) && ms == 1835416800000LL,
          "29 Feb 2028 (leap) accepted (python)");
    CHECK(clockCivilToUnixMs(2026, 8, 13, 12, 35, 19, 250, &ms) && ms == 1786624519250LL,
          "milliseconds carried");
    CHECK(!clockCivilToUnixMs(2027, 2, 29, 0, 0, 0, 0, &ms), "29 Feb 2027 refused");
    CHECK(!clockCivilToUnixMs(2026, 9, 31, 0, 0, 0, 0, &ms), "31 Sep refused");
    CHECK(!clockCivilToUnixMs(2026, 13, 1, 0, 0, 0, 0, &ms), "month 13 refused");
    CHECK(!clockCivilToUnixMs(2026, 1, 0, 0, 0, 0, 0, &ms), "day 0 refused");
    CHECK(!clockCivilToUnixMs(2026, 1, 1, 24, 0, 0, 0, &ms), "24:00 refused");
    CHECK(!clockCivilToUnixMs(2026, 1, 1, 0, 60, 0, 0, &ms), "minute 60 refused");
    CHECK(!clockCivilToUnixMs(2026, 1, 1, 0, 0, 60, 0, &ms), "leap second :60 refused");
    CHECK(!clockCivilToUnixMs(2025, 12, 31, 23, 59, 59, 0, &ms), "2025 refused (floor)");
    CHECK(!clockCivilToUnixMs(2080, 1, 1, 0, 0, 0, 0, &ms), "2080 refused (ceiling)");
    CHECK(!clockCivilToUnixMs(2099, 12, 31, 23, 59, 59, 0, &ms), "2099 refused (an NMEA '99')");
    CHECK(CLOCK_UNIX_LIMIT == 3471292800u, "CLOCK_UNIX_LIMIT = 2080-01-01T00:00:00Z (python)");
    CHECK(clockUnixSane(CLOCK_UNIX_MIN) && !clockUnixSane(CLOCK_UNIX_MIN - 1), "sane floor");
    CHECK(clockUnixSane(CLOCK_UNIX_LIMIT - 1) && !clockUnixSane(CLOCK_UNIX_LIMIT), "sane ceiling");
  }

  // ---- the reader: an RMC's OWN time fields ------------------------------------------
  {
    NmeaReader r;
    CHECK(r.fix().rmcHms == -1 && r.fix().rmcDmy == -1 && r.fix().rmcCount == 0,
          "fresh reader: no RMC time");
    feedBody(r, "GNRMC,123519.00,A,4738.96912,N,12220.35817,W,0.36,,130826,,,A");
    CHECK(r.fix().rmcCount == 1 && r.fix().rmcStatus == 'A' && r.fix().rmcMode == 'A',
          "RMC counted, status and mode read");
    CHECK(r.fix().rmcHms == 123519 && r.fix().rmcMs == 0 && r.fix().rmcDmy == 130826,
          "its own time and date");
    feedBody(r, "GNGGA,123520.00,4738.96912,N,12220.35817,W,1,08,1.2,56.3,M,-17.0,M,,");
    CHECK(r.fix().rmcCount == 1 && r.fix().rmcHms == 123519,
          "a GGA does not touch the RMC's time (timeHms moved: that is the trap)");
    CHECK(r.fix().timeHms == 123520, "(the shared timeHms did move)");
    feedBody(r, "GNRMC,000000.00,A,4738.96912,N,12220.35817,W,0.36,,,,,A");
    CHECK(r.fix().rmcCount == 2 && r.fix().rmcDmy == -1,
          "an RMC with an EMPTY date says so: never inherits the last one");
    CHECK(r.fix().dateDmy == 130826, "(the shared dateDmy kept the old date: that is the trap)");
    feedBody(r, "GNRMC,081836.5,V,,,,,,,130826,,,N");
    CHECK(r.fix().rmcStatus == 'V' && r.fix().rmcMode == 'N' && r.fix().rmcHms == 81836 &&
          r.fix().rmcMs == 500, "a V sentence's time is READ (and then refused by the rules)");
    feedBody(r, "GNRMC,081837.123,V,,,,,,,130826,,");
    CHECK(r.fix().rmcMs == 123 && r.fix().rmcMode == 0, "3-digit fraction; NMEA 2.1 has no mode");
    feedBody(r, "GNRMC,081837x,V,,,,,,,130826,,,N");
    CHECK(r.fix().rmcHms == -1, "a time with junk after it is absent, not 081837");
    feedBody(r, "GNRMC,081837.,V,,,,,,,130826,,,N");
    CHECK(r.fix().rmcHms == -1, "a bare '.' is not a fraction");
    feedBody(r, "GNRMC,0818,V,,,,,,,130826,,,N");
    CHECK(r.fix().rmcHms == -1, "four digits is not a time");
    feedBody(r, "GNRMC,081838.00,V,,,,,,,13082,,,N");
    CHECK(r.fix().rmcDmy == -1, "five-digit date refused");
    feedBody(r, "GNRMC,081838.00,,,,,,,,130826,,,N");
    CHECK(r.fix().rmcStatus == 0, "empty status reads as neither A nor V");
    feedBody(r, "GNRMC,081839.00,A,4738.96912,N,12220.35817,W,0.36,,010127,,,D");
    CHECK(r.fix().rmcDmy == 10127 && r.fix().rmcMode == 'D', "leading-zero day kept as 010127");
  }

  // ---- GPS step 1: one RMC judged alone ----------------------------------------------
  {
    NmeaReader r;
    int why = -1;
    CHECK(judge(r, "123519.00", 'A', "130826", "A", &why) == 1786624519000LL && why == CLK_NONE,
          "A + full date/time -> 2026-08-13 12:35:19 UTC (python)");
    CHECK(judge(r, "123519.25", 'A', "130826", "A", &why) == 1786624519250LL,
          "fraction carried into ms");
    CHECK(judge(r, "081836.00", 'V', "130826", "N", &why) == -1 && why == CLK_GPS_NO_FIX,
          "🛑 V with a complete time+date is REFUSED (u-blox pre-fix RTC/first-satellite time)");
    CHECK(judge(r, "081836.00", 'V', "130826", "A", &why) == -1 && why == CLK_GPS_NO_FIX,
          "V is refused whatever the mode says");
    CHECK(judge(r, "123519.00", 'A', "130826", "E", &why) == -1 && why == CLK_GPS_BAD_MODE,
          "mode E (dead reckoning) refused");
    CHECK(judge(r, "123519.00", 'A', "130826", "M", &why) == -1 && why == CLK_GPS_BAD_MODE,
          "mode M (MANUAL input) refused");
    CHECK(judge(r, "123519.00", 'A', "130826", "S", &why) == -1 && why == CLK_GPS_BAD_MODE,
          "mode S (SIMULATOR) refused");
    CHECK(judge(r, "123519.00", 'A', "130826", "N", &why) == -1 && why == CLK_GPS_BAD_MODE,
          "mode N with status A (contradiction) refused");
    CHECK(judge(r, "123519.00", 'A', "130826", "D", &why) == 1786624519000LL,
          "mode D (differential) accepted");
    CHECK(judge(r, "123519.00", 'A', "130826", "", &why) == 1786624519000LL,
          "no mode field (NMEA 2.1) accepted on the status alone");
    CHECK(judge(r, "", 'A', "130826", "A", &why) == -1 && why == CLK_GPS_NO_TIME,
          "no time -> NO_TIME");
    CHECK(judge(r, "123519.00", 'A', "", "A", &why) == -1 && why == CLK_GPS_NO_DATE,
          "no date -> NO_DATE");
    CHECK(judge(r, "123519.00", 'A', "130806", "A", &why) == -1 && why == CLK_GPS_BAD_YEAR,
          "a week-rollover date (2006) -> BAD_YEAR");
    CHECK(judge(r, "123519.00", 'A', "130800", "A", &why) == -1 && why == CLK_GPS_BAD_YEAR,
          "year 00 -> 2000 -> BAD_YEAR");
    CHECK(judge(r, "123519.00", 'A', "130825", "A", &why) == -1 && why == CLK_GPS_BAD_YEAR,
          "2025 -> BAD_YEAR (older than this firmware)");
    CHECK(judge(r, "235959.00", 'A', "311279", "A", &why) == 3471292799000LL,
          "2079-12-31 23:59:59 accepted (python)");
    /* 🛑 THE 80..99 DEFAULTS. Two-digit years a receiver prints before it knows the time. As
     * 2000 + yy they are well-formed dates decades ahead, and on the GPS path a pair of them
     * would be a TRUSTED clock (clock_source.h, SANE YEARS). */
    CHECK(judge(r, "000015.00", 'A', "060180", "A", &why) == -1 && why == CLK_GPS_BAD_YEAR,
          "🛑 060180 (the 1980 GPS epoch, read as 2080) -> BAD_YEAR");
    CHECK(judge(r, "235959.00", 'A', "311299", "A", &why) == -1 && why == CLK_GPS_BAD_YEAR,
          "a '99' (2099) -> BAD_YEAR");
    CHECK(judge(r, "000000.00", 'A', "010180", "A", &why) == -1 && why == CLK_GPS_BAD_YEAR,
          "2080-01-01 00:00:00, the first refused second -> BAD_YEAR");
    CHECK(judge(r, "060000.00", 'A', "290228", "A", &why) == 1835416800000LL,
          "29 Feb 2028 accepted (python)");
    CHECK(judge(r, "060000.00", 'A', "290227", "A", &why) == -1 && why == CLK_GPS_BAD_DATE,
          "29 Feb 2027 -> BAD_DATE");
    CHECK(judge(r, "240000.00", 'A', "130826", "A", &why) == -1 && why == CLK_GPS_BAD_DATE,
          "24:00:00 -> BAD_DATE");
    CHECK(judge(r, "235960.00", 'A', "130826", "A", &why) == -1 && why == CLK_GPS_BAD_DATE,
          "a leap second's :60 -> BAD_DATE (the next reading is a second away)");
    CHECK(judge(r, "123519.00", 'A', "001326", "A", &why) == -1 && why == CLK_GPS_BAD_DATE,
          "day 00 / month 13 -> BAD_DATE");
    CHECK(!clockGpsRmcTime(NULL, NULL, &why) && why == CLK_GPS_NO_FIX, "NULL fix refused");
    feedBody(r, "GNRMC,123519.00,A,47X8.96912,N,12220.35817,W,0.36,,130826,,,A");
    int64_t ms = 0;
    CHECK(r.fix().rmcStatus == 'A' && !r.fix().valid &&
          !clockGpsRmcTime(&r.fix(), &ms, &why) && why == CLK_GPS_NO_FIX,
          "an 'A' whose coordinates do not parse: not understood, its time goes with it");
    // Each half of the gate on its own: a fix record claiming valid coordinates but status V.
    feedBody(r, "GNRMC,123519.00,A,4738.96912,N,12220.35817,W,0.36,,130826,,,A");
    NmeaFix odd = r.fix();
    odd.rmcStatus = 'V';
    CHECK(odd.valid && !clockGpsRmcTime(&odd, &ms, &why) && why == CLK_GPS_NO_FIX,
          "status V refused by the status itself, not only through `valid`");
  }

  // ---- GPS step 2: two consecutive readings that agree --------------------------------
  {
    const int64_t T = 1786624519000LL;
    ClockGpsPair p;
    clockGpsPairReset(&p);
    int64_t at = 0;
    int why = -1;
    CHECK(!clockGpsPairFeed(&p, T, 10000, &at, &why) && why == CLK_GPS_FIRST,
          "one reading is only an opinion");
    CHECK(clockGpsPairFeed(&p, T + 1000, 11000, &at, &why) && why == CLK_NONE && at == T + 1000,
          "the next, 1 s later by both clocks: agreed, UTC at its arrival");
    CHECK(clockGpsPairFeed(&p, T + 2000, 12030, &at, &why) && at == T + 2030,
          "30 ms of jitter: agreed - and this read was the later one, so the first's estimate wins");
    // the same sentence twice
    CHECK(!clockGpsPairFeed(&p, T + 2000, 12100, &at, &why) && why == CLK_GPS_NOT_LATER,
          "a repeat is not a second opinion");
    CHECK(!clockGpsPairFeed(&p, T + 1000, 13100, &at, &why) && why == CLK_GPS_NOT_LATER,
          "backwards refused");
    // gap
    clockGpsPairReset(&p);
    clockGpsPairFeed(&p, T, 10000, &at, &why);
    CHECK(!clockGpsPairFeed(&p, T + 6000, 16000, &at, &why) && why == CLK_GPS_GAP,
          "6 s between readings: they prove nothing together");
    CHECK(clockGpsPairFeed(&p, T + 7000, 17000, &at, &why), "...and the chain restarts from it");
    // a loop stall
    clockGpsPairReset(&p);
    clockGpsPairFeed(&p, T, 10000, &at, &why);
    CHECK(!clockGpsPairFeed(&p, T + 1000, 12600, &at, &why) && why == CLK_GPS_DISAGREE,
          "second read 1.6 s late (a DB-save stall): disagree");
    CHECK(clockGpsPairFeed(&p, T + 2000, 13600, &at, &why) && at == T + 2000,
          "...and the next second pairs with the late one normally");
    clockGpsPairReset(&p);
    clockGpsPairFeed(&p, T, 10000, &at, &why);
    CHECK(clockGpsPairFeed(&p, T + 1000, 11800, &at, &why) && at == T + 1800,
          "second read 0.8 s late: agreed within tolerance, and the ON-TIME first wins");
    clockGpsPairReset(&p);
    clockGpsPairFeed(&p, T, 10400, &at, &why);            // first read 0.4 s late
    CHECK(clockGpsPairFeed(&p, T + 1000, 11050, &at, &why) && at == T + 1000,
          "first read late: the second (less late) wins");
    // millis() wrap
    clockGpsPairReset(&p);
    clockGpsPairFeed(&p, T, 0xFFFFFE00u, &at, &why);
    CHECK(clockGpsPairFeed(&p, T + 1000, 0x000001E8u, &at, &why) && at == T + 1000,
          "millis() wrapping between the two is fine");
  }

  // ---- GPS step 3: the policy ---------------------------------------------------------
  {
    const int64_t G = 1786624519000LL;
    ClockNow c = { CLOCK_SRC_NONE, 0, CLOCK_NEVER_MS };
    int64_t d = 123;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_SET_UNKNOWN && d == 0, "unknown clock: SET");
    c.source = CLOCK_SRC_MESH;
    c.utcMs = G - 40000;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_SET_UPGRADE && d == 40000,
          "mesh-set clock: GPS replaces it (and says by how much)");
    c.source = CLOCK_SRC_MESH;
    c.utcMs = G;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_SET_UPGRADE,
          "...even when it agrees: the SOURCE is the upgrade");
    c.source = CLOCK_SRC_NTP;
    c.msSinceNtp = 5 * 60 * 1000;
    c.utcMs = G - 500;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_KEEP_NTP_FRESH && d == 500,
          "🛑 fresh NTP, GPS 0.5 s off: NTP kept (never overridden by a small amount)");
    c.utcMs = G - 3600000;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_KEEP_NTP_FRESH && d == 3600000,
          "fresh NTP, GPS an HOUR off: NTP still kept - and the hour is measured for the log");
    c.msSinceNtp = CLOCK_NTP_FRESH_MS - 1;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_KEEP_NTP_FRESH, "fresh up to the last ms");
    c.msSinceNtp = CLOCK_NTP_FRESH_MS;
    c.utcMs = G - 1500;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_KEEP_AGREES, "stale NTP, 1.5 s: kept (agrees)");
    c.utcMs = G - 2000;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_KEEP_AGREES, "exactly 2 s: kept");
    c.utcMs = G - 2001;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_SET_DRIFT && d == 2001,
          "stale NTP, 2.001 s behind: SET");
    c.utcMs = G + 2500;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_SET_DRIFT && d == -2500,
          "stale NTP, 2.5 s ahead: SET");
    c.source = CLOCK_SRC_GPS;
    c.msSinceNtp = CLOCK_NEVER_MS;
    c.utcMs = G - 3000;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_SET_DRIFT, "gps clock drifted 3 s: SET");
    c.utcMs = G - 800;
    CHECK(clockGpsDecide(&c, G, &d) == CLK_GPS_KEEP_AGREES, "gps clock within 2 s: kept");
    CHECK(clockGpsDecide(NULL, G, NULL) == CLK_GPS_SET_UNKNOWN, "NULL state = unknown");
  }

  // ---- the whole path, sentence by sentence -------------------------------------------
  {
    NmeaReader r;
    ClockGpsPair p;
    clockGpsPairReset(&p);
    ClockNow unknown = { CLOCK_SRC_NONE, 0, CLOCK_NEVER_MS };
    int64_t set = 0, d = 0;
    CHECK(step(r, &p, "123517.00", 'V', "130826", 1000, &unknown, &set, &d) == CLK_GPS_NO_FIX,
          "indoors: V");
    CHECK(step(r, &p, "123518.00", 'V', "130826", 2000, &unknown, &set, &d) == CLK_GPS_NO_FIX,
          "indoors: V again - still nothing, however complete it looks");
    CHECK(step(r, &p, "123519.00", 'A', "130826", 3000, &unknown, &set, &d) == CLK_GPS_FIRST,
          "first fix: ONE reading, the clock does not move yet");
    CHECK(step(r, &p, "123520.00", 'A', "130826", 4000, &unknown, &set, &d) ==
          CLK_GPS_SET_UNKNOWN && set == 1786624520000LL,
          "second consecutive reading ~1 s later: SET to 12:35:20 at its arrival");
    // a bad sentence between two good ones
    clockGpsPairReset(&p);
    step(r, &p, "123521.00", 'A', "130826", 5000, &unknown, &set, &d);
    CHECK(step(r, &p, "123522.00", 'V', "130826", 6000, &unknown, &set, &d) == CLK_GPS_NO_FIX,
          "fix lost");
    CHECK(step(r, &p, "123523.00", 'A', "130826", 7000, &unknown, &set, &d) == CLK_GPS_FIRST,
          "good, BAD, good is not a pair: the chain restarted");
    CHECK(step(r, &p, "123524.00", 'A', "130826", 8000, &unknown, &set, &d) == CLK_GPS_SET_UNKNOWN,
          "...and the next good one completes it");
    // the midnight trap, end to end
    clockGpsPairReset(&p);
    step(r, &p, "235959.00", 'A', "130826", 20000, &unknown, &set, &d);
    char b[100];
    snprintf(b, sizeof(b), "GNRMC,000000.00,A,4738.96912,N,12220.35817,W,0.36,,,,,A");
    feedBody(r, b);
    CHECK(clockGpsStep(&p, &r.fix(), 21000, &unknown, &set, &d) == CLK_GPS_NO_DATE,
          "🛑 midnight with the date missing: refused, NOT 00:00:00 on the 13th (a day wrong)");
    CHECK(step(r, &p, "000001.00", 'A', "140826", 22000, &unknown, &set, &d) == CLK_GPS_FIRST,
          "the chain restarts after it");
    CHECK(step(r, &p, "000002.00", 'A', "140826", 23000, &unknown, &set, &d) ==
          CLK_GPS_SET_UNKNOWN && set == 1786665602000LL, "...and sets 2026-08-14 00:00:02 (python)");
    // against a fresh NTP, and then a stale one
    ClockNow ntp = { CLOCK_SRC_NTP, 1786665603000LL + 900, 60 * 1000 };
    CHECK(step(r, &p, "000003.00", 'A', "140826", 24000, &ntp, &set, &d) ==
          CLK_GPS_KEEP_NTP_FRESH && d == -900, "fresh NTP 0.9 s ahead: kept, the difference measured");
    ntp.msSinceNtp = 45 * 60 * 1000;
    ntp.utcMs = 1786665604000LL - 5000;
    set = 0;
    CHECK(step(r, &p, "000004.00", 'A', "140826", 25000, &ntp, &set, &d) ==
          CLK_GPS_SET_DRIFT && set == 1786665604000LL && d == 5000,
          "NTP 45 min stale and 5 s behind: GPS corrects it");
  }

  // ---- mesh time ----------------------------------------------------------------------
  {
    const uint32_t T = 1790355847u;                 // 2026-09-25 17:04:07 UTC
    ClockMeshVote v;
    clockMeshVoteReset(&v);
    uint32_t adopt = 0;
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NTP, 0x1234, true, T, 1000, &adopt) ==
          CLK_MESH_CLOCK_KNOWN, "🛑 never overrides NTP");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_GPS, 0x1234, true, T, 1000, &adopt) ==
          CLK_MESH_CLOCK_KNOWN, "never overrides GPS");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_MESH, 0x1234, true, T, 1000, &adopt) ==
          CLK_MESH_CLOCK_KNOWN, "never overrides an earlier mesh time either");
    CHECK(clockMeshVoteCount(&v, 1000) == 0, "...and a refused offer is not even held");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0x1234, false, 0, 1000, &adopt) == CLK_MESH_NO_TIME,
          "no time field");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0x1234, true, CLOCK_UNIX_MIN - 1, 1000, &adopt) ==
          CLK_MESH_BAD_YEAR, "2025 refused, even privately");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0x1234, true, CLOCK_UNIX_LIMIT, 1000, &adopt) ==
          CLK_MESH_BAD_YEAR, "the ceiling refused");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0x1234, true, 3471292800u, 1000, &adopt) ==
          CLK_MESH_BAD_YEAR, "🛑 2080-01-01 (a stock node's TinyGPS++ '80' default) refused, even privately");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0x1234, true, 3471724800u, 1000, &adopt) ==
          CLK_MESH_BAD_YEAR, "2080-01-06 (the 1980 GPS epoch as 2080, python) refused");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0x1234, false, 4102444799u, 1000, &adopt) ==
          CLK_MESH_BAD_YEAR && clockMeshVoteCount(&v, 1000) == 0,
          "2099 refused on a public channel, and not even held as a candidate");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0, true, T, 1000, &adopt) == CLK_MESH_BAD_NODE &&
          clockMeshOffer(&v, CLOCK_SRC_NONE, 0xFFFFFFFFu, true, T, 1000, &adopt) ==
          CLK_MESH_BAD_NODE, "node 0 / broadcast refused");
    adopt = 0;
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0x1234, true, T, 1000, &adopt) ==
          CLK_MESH_SET_PRIVATE && adopt == T, "private channel: one packet is enough");

    // public: one node is not enough, and cannot vouch for itself
    clockMeshVoteReset(&v);
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0xA, false, T, 1000, &adopt) == CLK_MESH_WAIT &&
          clockMeshVoteCount(&v, 1000) == 1, "LongFast: one stranger's word is HELD, not used");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0xA, false, T + 30, 31000, &adopt) ==
          CLK_MESH_WAIT && clockMeshVoteCount(&v, 31000) == 1,
          "the same node agreeing with itself is still one witness");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0xB, false, T + 60 + 61, 61000, &adopt) ==
          CLK_MESH_WAIT && clockMeshVoteCount(&v, 61000) == 2,
          "a second node 61 s out (after aging): disagree, held");
    adopt = 0;
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0xC, false, T + 90 - 55, 91000, &adopt) ==
          CLK_MESH_SET_AGREED && adopt == T + 35,
          "a third agrees with A within 60 s (aged by millis): SET to the newer packet's time");
    CHECK(clockMeshVoteCount(&v, 91000) == 0, "the vote is cleared by a SET");

    // candidates expire
    clockMeshVoteReset(&v);
    clockMeshOffer(&v, CLOCK_SRC_NONE, 0xA, false, T, 0, &adopt);
    const uint32_t later = CLOCK_MESH_CAND_MAX_AGE_MS + 1000;
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0xB, false, T + later / 1000, later, &adopt) ==
          CLK_MESH_WAIT, "a witness heard over an hour ago no longer counts");
    CHECK(clockMeshVoteCount(&v, later) == 1, "(and is not counted as held)");

    // full table: the oldest is evicted
    clockMeshVoteReset(&v);
    for (uint32_t i = 0; i < CLOCK_MESH_CAND_MAX; i++) {
      // each 10 minutes apart in time but only 1 s apart by millis: all disagree
      clockMeshOffer(&v, CLOCK_SRC_NONE, 0x100 + i, false, T + i * 600, 1000 + i * 1000, &adopt);
    }
    CHECK(clockMeshVoteCount(&v, 5000) == CLOCK_MESH_CAND_MAX, "table full of disagreeing nodes");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0x200, false, T + 9999, 5000, &adopt) ==
          CLK_MESH_WAIT && clockMeshVoteCount(&v, 5000) == CLOCK_MESH_CAND_MAX,
          "a fifth disagreeing node takes the OLDEST slot");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0x300, false, T + 4, 6000, &adopt) == CLK_MESH_WAIT,
          "so the evicted first node's time finds no witness (and 0x300 evicts the next oldest)");
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0x301, false, T + 1200 + 3, 6000, &adopt) ==
          CLK_MESH_SET_AGREED, "while a node still held is a witness");

    // millis() wrap while aging
    clockMeshVoteReset(&v);
    clockMeshOffer(&v, CLOCK_SRC_NONE, 0xA, false, T, 0xFFFFF000u, &adopt);   // 4.1 s pre-wrap
    CHECK(clockMeshOffer(&v, CLOCK_SRC_NONE, 0xB, false, T + 10, 0x00001770u, &adopt) ==
          CLK_MESH_SET_AGREED && adopt == T + 10, "aging across the millis() wrap");
  }

  // ---- an NTP answer the clock may take (clockNtpReplySane) ---------------------------
  {
    /* A real server answer, built as ntpd sends it: LI 0, version 4, mode 4, stratum 2, the
     * transmit time 2026-08-13 12:35:19 UTC = 1786624519 + 2208988800 = 3995613319
     * (0xEE283887 — computed in Python, not here). */
    uint8_t p[48];
    auto answer = [&](uint8_t b0, uint8_t stratum, uint32_t ntpSec) {
      memset(p, 0, sizeof(p));
      p[0] = b0;
      p[1] = stratum;
      p[40] = (uint8_t)(ntpSec >> 24);
      p[41] = (uint8_t)(ntpSec >> 16);
      p[42] = (uint8_t)(ntpSec >> 8);
      p[43] = (uint8_t)ntpSec;
    };
    uint32_t got = 0;
    answer(0x24, 2, 3995613319u);
    CHECK(clockNtpReplySane(p, 48, &got) && got == 1786624519u, "a normal server answer is taken");
    CHECK(!clockNtpReplySane(p, 47, &got), "a short datagram is refused");
    answer(0xE4, 2, 3995613319u);                   // LI 3
    CHECK(!clockNtpReplySane(p, 48, &got), "LI=3 (server unsynchronised) is refused");
    answer(0x23, 2, 3995613319u);                   // mode 3: a client's request echoed back
    CHECK(!clockNtpReplySane(p, 48, &got), "mode other than 4 (server) is refused");
    answer(0x24, 0, 3995613319u);
    CHECK(!clockNtpReplySane(p, 48, &got), "stratum 0 (kiss-o'-death) is refused");
    answer(0x24, 16, 3995613319u);
    CHECK(!clockNtpReplySane(p, 48, &got), "stratum 16 (unsynchronised) is refused");
    answer(0x24, 2, 1000u);                         // 1900 + 1000 s: would wrap to 2036
    CHECK(!clockNtpReplySane(p, 48, &got), "a transmit time before 1970 is refused (no 2036 wrap)");
    answer(0x24, 2, 1767225600u + 2208988800u - 1); // 2025-12-31 23:59:59
    CHECK(!clockNtpReplySane(p, 48, &got), "a time before the 2026 floor is refused");
    answer(0x24, 2, 1767225600u + 2208988800u);     // 2026-01-01 00:00:00
    CHECK(clockNtpReplySane(p, 48, &got) && got == 1767225600u, "the floor itself is taken");
    answer(0x24, 1, 0xFFFFFFFFu);                   // 2036-02-07 06:28:15 UTC (Python): inside 2026..2079
    CHECK(clockNtpReplySane(p, 48, &got) && got == 0xFFFFFFFFu - 2208988800u,
          "the last NTP-era-0 second (2036-02-07) is a sane date and taken");
    CHECK(!clockNtpReplySane(NULL, 48, &got), "no packet, no clock");
  }

  printf("-- the SIP store under a mesh clock (SA-3): the offset, and finalising a stamp\n");
  {
    CHECK(clockMeshCorrS(1786624519000LL, 1786624519000LL) == 0, "no error, no offset");
    CHECK(clockMeshCorrS(1786624519000LL, 1786624519499LL) == 0 &&
          clockMeshCorrS(1786624519000LL, 1786624519500LL) == -1 &&
          clockMeshCorrS(1786624519500LL, 1786624519000LL) == 1,
          "rounded to the nearest second, half away from zero, both signs");
    /* The CTR replay's worst case: a mesh clock at 2079-12-31 23:59:59 replaced by NTP at
     * 2026-08-13 12:35:19 — (1786624519 - 3471292799) s = -1684668280 (Python). */
    CHECK(clockMeshCorrS(1786624519000LL, 3471292799000LL) == -1684668280,
          "a 2079 mesh clock's offset fits and is exact");
    CHECK(clockMeshCorrS(0, 4000000000000LL) == -2147483647 - 1 &&
          clockMeshCorrS(4000000000000LL, 0) == 2147483647, "clamped, never wrapped");

    const uint32_t now = 1786624519u;               // 2026-08-13 12:35:19 UTC, trusted
    ClockMsgStamp st = {now, 0, 0xA5A5A5A5u, -1684668280};
    uint32_t out = 0;
    CHECK(clockMsgFinal(3471292700u, 0xA5A5A5A5u, &st, &out) && out == now - 99,
          "this boot's mesh clock (2079, replayed): a stamp 99 s before NTP lands 99 s before now");
    st.corrS = 3;
    CHECK(clockMsgFinal(now - 600, 0xA5A5A5A5u, &st, &out) && out == now - 597,
          "a mesh clock 3 s behind: its stamps move 3 s");
    CHECK(clockMsgFinal(now - 600, 0x12345678u, &st, &out) && out == now - 600,
          "an EARLIER boot's mesh stamp in the past is kept (no offset is known for it)");
    out = 7;
    CHECK(!clockMsgFinal(now + 1, 0x12345678u, &st, &out) && out == 7,
          "an earlier boot's stamp in the FUTURE is not final: re-stamped like an unknown time");
    CHECK(!clockMsgFinal(3471292700u, 0x12345678u, &st, &out),
          "...which is exactly the 2079 replay whose boot has gone");
    CHECK(clockMsgFinal(now, 0x12345678u, &st, &out) && out == now, "now itself is not the future");
    CHECK(!clockMsgFinal(CLOCK_UNIX_MIN - 1, 0x12345678u, &st, &out),
          "a stamp before 2026 is not final either");
    st.corrS = 10;
    CHECK(!clockMsgFinal(now - 5, 0xA5A5A5A5u, &st, &out),
          "a correction that would put a stamp in the future is refused, not clamped");
    CHECK(clockMsgFinal(now - 600, 0, &st, &out) && out == now - 600,
          "an unreadable id (0) is an earlier boot's: kept, not corrected");
    ClockMsgStamp zero = {now, 0, 0, 10};
    CHECK(clockMsgFinal(now - 600, 0, &zero, &out) && out == now - 600,
          "...even when no mesh clock was replaced (corrId 0 matches nothing)");
    ClockMsgStamp none = {now, 0, 0, 0};
    CHECK(clockMsgFinal(now - 600, 0, &none, &out) && out == now - 600,
          "no correction this boot (NTP came first): past stamps kept");
    ClockMsgStamp mesh = {now, 0xA5A5A5A5u, 0, 0};
    out = 7;
    CHECK(!clockMsgFinal(now - 600, 0xA5A5A5A5u, &mesh, &out) && out == 7,
          "under a MESH clock nothing is final");
    ClockMsgStamp unknown = {0, 0, 0, 0};
    CHECK(!clockMsgFinal(now - 600, 0x12345678u, &unknown, &out) && out == 7,
          "under an unknown clock nothing is final");
    CHECK(!clockMsgFinal(now - 600, 0x12345678u, NULL, &out), "no clock, no answer");
  }

  printf("-- re-stamping what clockMsgFinal refused keeps the order the texts came in\n");
  {
    /* The file order these stamps come in, from the store's own rules (Storage.cpp): a new text
     * is appended, then reorderLast() moves it before the first section messageCompare puts
     * after it — messageCompare(a, b) = strcasecmp(b.t, a.t), so before the first OLDER one,
     * i.e. AFTER its equals. 8-hex-digit stamps compare as the numbers do. */
    struct Txt { uint32_t t; char who; };
    auto arrive = [](Txt* file, uint32_t* n, Txt x) {
      file[(*n)++] = x;
      for (uint32_t j = 0; j + 1 < *n; j++) {
        if (x.t > file[j].t) {                      // messageCompare(file[j], last) > 0
          for (uint32_t k = *n - 1; k > j; k--) {
            file[k] = file[k - 1];
          }
          file[j] = x;
          break;
        }
      }
    };
    const uint32_t now = 1786624519u, bandEnd = now - 2;   // two sentinels took the top
    /* The misc repair round's case: A, B, C arrive under an EARLIER boot's mesh clock ~10 min
     * fast; the phone reboots and NTP finalises them inside those 10 min, all still ahead. */
    Txt file[8];
    uint32_t n = 0;
    arrive(file, &n, {now + 300, 'A'});
    arrive(file, &n, {now + 360, 'B'});
    arrive(file, &n, {now + 420, 'C'});
    CHECK(n == 3 && file[0].who == 'C' && file[1].who == 'B' && file[2].who == 'A',
          "a partition is newest-first: C, B, A");
    uint32_t prov[8], got[8];
    for (uint32_t i = 0; i < n; i++) {
      prov[i] = file[i].t;
    }
    clockMsgRestamp(prov, n, bandEnd, got);
    CHECK(got[0] == bandEnd && got[1] == bandEnd - 1 && got[2] == bandEnd - 2,
          "C (the newest) takes the top of the band, A the bottom - NOT ascending in file order, "
          "which gave C the earliest stamp and reversed the thread for good");
    /* The same, read back the way the thread shows it: oldest first. */
    uint32_t tA = 0, tB = 0, tC = 0;
    for (uint32_t i = 0; i < n; i++) {
      (file[i].who == 'A' ? tA : file[i].who == 'B' ? tB : tC) = got[i];
    }
    CHECK(tA < tB && tB < tC, "reading order after the re-stamp is still A, B, C");

    /* Equal provisional stamps (two texts in one second): the store keeps them in ARRIVAL order,
     * so the later in the file is the newer. */
    n = 0;
    arrive(file, &n, {now + 500, 'D'});
    arrive(file, &n, {now + 500, 'E'});
    arrive(file, &n, {now + 501, 'F'});
    CHECK(file[0].who == 'F' && file[1].who == 'D' && file[2].who == 'E',
          "equals sit in arrival order: F, D, E");
    for (uint32_t i = 0; i < n; i++) {
      prov[i] = file[i].t;
    }
    clockMsgRestamp(prov, n, bandEnd, got);
    CHECK(got[0] == bandEnd && got[2] == bandEnd - 1 && got[1] == bandEnd - 2,
          "D < E < F: the tie goes to the later arrival, and every stamp is distinct");

    /* Ranked, not walked: an unsorted list (a partition someone edited) gets the same answer. */
    const uint32_t un[5] = {now + 30, now + 90, now + 10, now + 90, now + 60};
    clockMsgRestamp(un, 5, bandEnd, got);
    CHECK(got[1] == bandEnd - 1 && got[3] == bandEnd && got[4] == bandEnd - 2 &&
          got[0] == bandEnd - 3 && got[2] == bandEnd - 4,
          "unsorted input: ranked by stamp, equals by position - the band bandEnd-4..bandEnd");
    bool distinct = true;
    for (uint32_t i = 0; i < 5; i++) {
      for (uint32_t j = i + 1; j < 5; j++) {
        distinct = distinct && got[i] != got[j];
      }
    }
    CHECK(distinct, "...and no two share a stamp");
    got[0] = 7;
    clockMsgRestamp(un, 0, bandEnd, got);
    clockMsgRestamp(NULL, 5, bandEnd, got);
    CHECK(got[0] == 7, "nothing to re-stamp, or no list: nothing written");
  }

  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
