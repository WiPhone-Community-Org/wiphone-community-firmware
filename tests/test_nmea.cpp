/*
 * test_nmea.cpp — the woods plate's GPS reader against byte-literal sentences.
 *
 * Every vector's checksum and every expected 1e-7-degree value was computed
 * INDEPENDENTLY in Python (see the generator lines in the commit that added
 * this file), so a pass means the C++ agrees with a second implementation,
 * not with itself. The junk-tolerance cases exist because this UART lives on
 * a phone whose first bytes after DFS idle arrive mangled (the panicwatch
 * lesson of 2026-08-20) — garbage before '$' is the EXPECTED steady state.
 */

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

// Feed a whole string; return how many times feed() reported a completed RMC/GGA.
static int feedAll(NmeaReader& r, const char* s) {
  int hits = 0;
  for (const char* p = s; *p; p++) {
    if (r.feed(*p)) {
      hits++;
    }
  }
  return hits;
}

/* Python-verified vectors (checksums + expectations):
 *   Seattle 47°38.96912'N 122°20.35817'W → latI 476494853, lonI -1223393028
 *   Sydney  33°52.12800'S 151°12.55800'E → latI -338688000, lonI 1512093000 */
static const char* SEA_RMC = "$GNRMC,123519.00,A,4738.96912,N,12220.35817,W,0.36,,130826,,,A*4C\r\n";
static const char* SEA_GGA = "$GNGGA,123520.00,4738.96912,N,12220.35817,W,1,08,1.2,56.3,M,-17.0,M,,*71\r\n";
static const char* SYD_RMC = "$GNRMC,020000.00,A,3352.12800,S,15112.55800,E,0.10,,130826,,,A*4C\r\n";
static const char* NOFIX_RMC = "$GNRMC,081836.00,V,,,,,,,130826,,,N*69\r\n";
static const char* GSV = "$GPGSV,3,1,10,05,60,120,35,07,45,300,40,08,10,050,22,13,30,200,31*70\r\n";
static const char* GGA_HDOP2_ALTNEG = "$GNGGA,123521.00,4738.96912,N,12220.35817,W,1,12,2,-3.2,M,,M,,*4D\r\n";
static const char* MIN60_RMC = "$GNRMC,123519.00,A,4760.00000,N,12220.35817,W,0.36,,130826,,,A*44\r\n";
static const char* GGA_Q0_SATS3 = "$GNGGA,081836.00,,,,,0,03,,,M,,M,,*51\r\n";
static const char* GLL = "$GNGLL,4738.96912,N,12220.35817,W,123519.00,A,A*6E\r\n";
static const char* SEA_RMC_BADCK = "$GNRMC,123519.00,A,4738.96912,N,12220.35817,W,0.36,,130826,,,A*4D\r\n";

/* Motion (0.9.80): RMC field 6 = speed over ground (knots), 7 = course over ground (deg true).
 * Checksums computed in Python (XOR of the body between '$' and '*'); the expected fixed-point
 * values by hand: 5.43 kn -> 543, 212.5 deg -> 2125, "12.345" truncated at 2 places -> 1234,
 * "7.25" at 1 place -> 72, "5" -> 500, "360.0" -> 0 (north). */
static const char* MOVING_RMC = "$GNRMC,123521.00,A,4738.96912,N,12220.35817,W,5.43,212.5,130826,,,A*6A\r\n";
static const char* NOCOURSE_RMC = "$GNRMC,123522.00,A,4738.96912,N,12220.35817,W,0.02,,130826,,,A*43\r\n";
static const char* VSTATUS_RMC = "$GNRMC,123523.00,V,,,,,,,130826,,,N*69\r\n";
static const char* GARBLED_RMC = "$GNRMC,123524.00,A,4738.96912,N,12220.35817,W,5.4x,21a.5,130826,,,A*77\r\n";
static const char* WHOLE_RMC = "$GNRMC,123525.00,A,4738.96912,N,12220.35817,W,5,360.0,130826,,,A*46\r\n";
static const char* LONGFRAC_RMC = "$GNRMC,123526.00,A,4738.96912,N,12220.35817,W,12.345,7.25,130826,,,A*5A\r\n";
static const char* BADCOURSE_RMC = "$GNRMC,123527.00,A,4738.96912,N,12220.35817,W,-1.0,361.0,130826,,,A*72\r\n";
static const char* DOTS_RMC = "$GNRMC,123528.00,A,4738.96912,N,12220.35817,W,.,1.2.3,130826,,,A*4B\r\n";
static const char* SHORT_RMC = "$GNRMC,123529.00,A,4738.96912,N,12220.35817,W*1B\r\n";

/* THIS GGA's own fields (0.9.82, the phone-over-WiFi gate). Dummy coordinates (40.0,-100.0 is
 * "4000.00000,N,10000.00000,W": 400000000 / -1000000000 exactly); checksums by Python. */
static const char* D_GGA      = "$GPGGA,120000.00,4000.00000,N,10000.00000,W,1,09,0.9,100.0,M,,M,,*67\r\n";
static const char* D_GGA_BARE = "$GPGGA,120001.00,4000.00000,N,10000.00000,W,1,,,100.0,M,,M,,*48\r\n";
static const char* D_GGA_GARB = "$GPGGA,120002.00,4000.00000,N,10000.00000,W,1,1x,1.2E-4,100.0,M,,M,,*73\r\n";
static const char* D_GGA_DR   = "$GPGGA,120003.00,4000.00000,N,10000.00000,W,6,05,1.5,100.0,M,,M,,*62\r\n";
static const char* D_GGA_LONGQ = "$GPGGA,120004,4000.00000,N,10000.00000,W,123,05,12.34,100.0,M,,M,,*4D\r\n";
static const char* D_GGA_EMPTY = "$GPGGA,,,,,,0,,,,,,,,*66\r\n";
static const char* D_RMC      = "$GPRMC,120005.00,A,4000.00000,N,10000.00000,W,0.0,,101026,,,A*65\r\n";
static const char* SOUTH_EAST_GGA = "$GNGGA,235959.50,3000.00000,S,05000.00000,E,2,12,1.0,5.0,M,,M,,*77\r\n";
/* This GGA's own altitude (ggaAltM, 0.9.82): "1.2E3" is no number to the strict reader (the
 * lenient altM reads 1 from it), "-0.4" is 0 m toward zero. Checksums by Python. */
static const char* D_GGA_ALT_E = "$GPGGA,120006.00,4000.00000,N,10000.00000,W,1,09,0.9,1.2E3,M,,M,,*15\r\n";
static const char* D_GGA_ALT_NEG0 = "$GPGGA,120007.00,4000.00000,N,10000.00000,W,1,09,0.9,-0.4,M,,M,,*48\r\n";

int main() {
  printf("test_nmea\n");

  // ---- the happy path: a valid RMC then its GGA -----------------------------
  {
    NmeaReader r;
    CHECK(feedAll(r, SEA_RMC) == 1, "RMC completes once");
    CHECK(r.fix().valid, "RMC 'A' = valid");
    CHECK(r.fix().latI == 476494853, "Seattle latitude in 1e-7 deg");
    CHECK(r.fix().lonI == -1223393028, "Seattle longitude negative (W)");
    CHECK(r.fix().timeHms == 123519, "UTC time captured");
    CHECK(r.fix().dateDmy == 130826, "date captured");
    CHECK(r.fix().sats == -1, "no GGA yet: sats unknown");
    CHECK(feedAll(r, SEA_GGA) == 1, "GGA completes once");
    CHECK(r.fix().sats == 8, "GGA sats");
    CHECK(r.fix().hdopX10 == 12, "GGA hdop 1.2 as x10");
    CHECK(r.fix().altM == 56, "GGA altitude, metres");
    CHECK(r.sentences() == 2 && r.badChecksum() == 0, "counters clean");
    CHECK(r.bytes() == strlen(SEA_RMC) + strlen(SEA_GGA), "every byte counted");
  }

  // ---- southern + eastern hemisphere ----------------------------------------
  {
    NmeaReader r;
    feedAll(r, SYD_RMC);
    CHECK(r.fix().latI == -338688000, "Sydney latitude negative (S)");
    CHECK(r.fix().lonI == 1512093000, "Sydney longitude positive (E)");
  }

  // ---- a no-fix RMC must not smear the last good coordinates ----------------
  {
    NmeaReader r;
    feedAll(r, SEA_RMC);
    CHECK(feedAll(r, NOFIX_RMC) == 1, "no-fix RMC still reports (GPS alive)");
    CHECK(!r.fix().valid, "status V = not valid");
    CHECK(r.fix().latI == 476494853, "last good latitude retained");
    CHECK(r.fix().dateDmy == 130826, "date still flows on V sentences");
  }

  // ---- checksum discipline ---------------------------------------------------
  {
    NmeaReader r;
    CHECK(feedAll(r, SEA_RMC_BADCK) == 0, "bad checksum: no report");
    CHECK(r.badChecksum() == 1 && r.sentences() == 0, "bad checksum counted");
    CHECK(!r.fix().valid, "bad checksum: fix untouched");
    CHECK(feedAll(r, SEA_RMC) == 1, "good sentence right after still parses");
  }

  // ---- junk tolerance: wake garbage, mid-sentence '$', overlong lines -------
  {
    NmeaReader r;
    const char junk[] = { (char)0xFF, (char)0xFE, 'g', 'a', 'r', 'b', 0 };
    feedAll(r, junk);
    CHECK(feedAll(r, SEA_RMC) == 1, "garbage before '$' is ignored");

    NmeaReader r2;
    feedAll(r2, "$GNRMC,truncated-nonsense");   // no EOL — then a fresh '$'
    CHECK(feedAll(r2, SYD_RMC) == 1, "'$' mid-sentence restarts assembly");

    NmeaReader r3;
    r3.feed('$');
    for (int i = 0; i < 150; i++) {
      r3.feed('A');                             // way past the 82-char NMEA cap
    }
    CHECK(r3.overruns() == 1, "overlong line counted as overrun");
    CHECK(feedAll(r3, SEA_RMC) == 1, "reader recovers after an overrun");
  }

  // ---- sentences we ignore are counted, not parsed --------------------------
  {
    NmeaReader r;
    CHECK(feedAll(r, GSV) == 0, "GSV: no report");
    CHECK(feedAll(r, GLL) == 0, "GLL: no report");
    CHECK(r.sentences() == 2, "ignored sentences still count (checksum ok)");
    CHECK(!r.fix().valid, "ignored sentences leave the fix alone");
  }

  // ---- GGA details: integer hdop, negative altitude, quality-0 --------------
  {
    NmeaReader r;
    feedAll(r, GGA_HDOP2_ALTNEG);
    CHECK(r.fix().hdopX10 == 20, "hdop '2' = 20 x10");
    CHECK(r.fix().altM == -3, "altitude '-3.2' = -3 m");
    CHECK(r.fix().sats == 12, "sats 12");

    NmeaReader r2;
    feedAll(r2, GGA_Q0_SATS3);
    CHECK(r2.fix().sats == 3, "quality-0 GGA still reports sats seen");
    CHECK(!r2.fix().valid && r2.fix().latI == 0, "quality-0 GGA never writes coords");
  }

  // ---- malformed coordinates: minutes must stay under 60 --------------------
  {
    NmeaReader r;
    feedAll(r, MIN60_RMC);
    CHECK(!r.fix().valid, "60 minutes = unreadable coords = not a fix");
    CHECK(r.fix().latI == 0, "rejected coords never written");
  }

  // ---- bare-\n framing (some modules skip the \r) ---------------------------
  {
    NmeaReader r;
    char buf[160];
    size_t n = strlen(SEA_RMC);
    memcpy(buf, SEA_RMC, n + 1);
    buf[n - 2] = '\n';                          // "...A*4C\n" (drop the \r)
    buf[n - 1] = 0;
    CHECK(feedAll(r, buf) == 1, "bare \\n terminates a sentence too");
  }

  // ---- motion: speed and course over ground, from THIS RMC only (0.9.80) ----
  {
    NmeaReader r;
    CHECK(r.fix().speedKnX100 == -1 && r.fix().courseX10 == -1, "motion unknown before any RMC");
    CHECK(feedAll(r, MOVING_RMC) == 1 && r.fix().valid, "moving RMC parses as a fix");
    CHECK(r.fix().speedKnX100 == 543, "speed 5.43 kn = 543 x100");
    CHECK(r.fix().courseX10 == 2125, "course 212.5 deg = 2125 x10");
    feedAll(r, SEA_GGA);
    CHECK(r.fix().speedKnX100 == 543 && r.fix().courseX10 == 2125, "a GGA leaves the motion alone");

    CHECK(feedAll(r, NOCOURSE_RMC) == 1, "RMC with an empty course parses");
    CHECK(r.fix().speedKnX100 == 2, "speed 0.02 kn = 2");
    CHECK(r.fix().courseX10 == -1, "empty course = -1, NOT the last RMC's 212.5");

    feedAll(r, MOVING_RMC);
    CHECK(feedAll(r, VSTATUS_RMC) == 1 && !r.fix().valid, "a 'V' RMC after a moving one");
    CHECK(r.fix().speedKnX100 == -1 && r.fix().courseX10 == -1,
          "'V' with empty fields: speed and course -1, never inherited");

    feedAll(r, MOVING_RMC);
    CHECK(feedAll(r, GARBLED_RMC) == 1 && r.fix().valid, "garbled motion fields: still a fix");
    CHECK(r.fix().speedKnX100 == -1, "garbled speed '5.4x' = -1");
    CHECK(r.fix().courseX10 == -1, "garbled course '21a.5' = -1");

    feedAll(r, WHOLE_RMC);
    CHECK(r.fix().speedKnX100 == 500, "whole-number speed '5' = 500");
    CHECK(r.fix().courseX10 == 0, "course '360.0' = 0 (north)");

    feedAll(r, LONGFRAC_RMC);
    CHECK(r.fix().speedKnX100 == 1234, "'12.345' kn truncated to 1234");
    CHECK(r.fix().courseX10 == 72, "'7.25' deg truncated to 72");

    feedAll(r, BADCOURSE_RMC);
    CHECK(r.fix().speedKnX100 == -1, "a signed speed '-1.0' is not a speed");
    CHECK(r.fix().courseX10 == -1, "course 361.0 is not a bearing");

    feedAll(r, MOVING_RMC);
    feedAll(r, DOTS_RMC);
    CHECK(r.fix().speedKnX100 == -1 && r.fix().courseX10 == -1, "'.' and '1.2.3' are -1");

    feedAll(r, MOVING_RMC);
    CHECK(feedAll(r, SHORT_RMC) == 1 && r.fix().valid, "an RMC cut after E/W still parses");
    CHECK(r.fix().speedKnX100 == -1 && r.fix().courseX10 == -1, "absent fields 6/7 = -1");

    r.reset();
    CHECK(r.fix().speedKnX100 == -1 && r.fix().courseX10 == -1, "reset() forgets the motion");
  }

  /* ---- THIS GGA's own fields: nothing inherited (0.9.82) -----------------------------
   * The sticky fields above keep the last good sats/HDOP/coordinates across a GGA that lacks
   * them — fine for the UART, wrong for a relayed stream (nmea.h). The gga* fields are each
   * rewritten by every GGA, from that sentence alone. */
  {
    NmeaReader r;
    CHECK(r.fix().ggaCount == 0 && r.fix().ggaQuality == -1 && r.fix().ggaSats == -1 &&
          r.fix().ggaHdopX10 == -1 && r.fix().ggaHms == -1 && !r.fix().ggaHasPos,
          "gga*: unknown before any GGA");
    CHECK(feedAll(r, D_GGA) == 1 && r.fix().ggaCount == 1, "a GGA counts");
    CHECK(r.fix().ggaQuality == 1 && r.fix().ggaHasPos && r.fix().ggaLatI == 400000000 &&
          r.fix().ggaLonI == -1000000000, "gga quality and position from the sentence");
    CHECK(r.fix().ggaSats == 9 && r.fix().ggaHdopX10 == 9 && r.fix().ggaHms == 120000,
          "gga sats 9, HDOP 0.9 -> 9, time 120000");

    CHECK(feedAll(r, D_GGA_BARE) == 1, "a GGA with empty sats and HDOP parses");
    CHECK(r.fix().sats == 9 && r.fix().hdopX10 == 9, "...the sticky sats/HDOP keep the last (as before)");
    CHECK(r.fix().ggaSats == -1 && r.fix().ggaHdopX10 == -1 && r.fix().ggaHasPos &&
          r.fix().ggaHms == 120001, "...the gga* ones say THIS sentence had none");

    feedAll(r, D_GGA_GARB);
    CHECK(r.fix().ggaSats == -2 && r.fix().ggaHdopX10 == -2,
          "'1x' sats and '1.2E-4' HDOP are present-but-unreadable (-2), not a number");

    feedAll(r, D_GGA_DR);
    CHECK(r.fix().ggaQuality == 6 && r.fix().ggaHasPos, "quality 6 (dead reckoning) is reported as 6");

    feedAll(r, D_GGA_LONGQ);
    CHECK(r.fix().ggaQuality == -1 && r.fix().ggaHdopX10 == 123 && r.fix().ggaHms == 120004,
          "a 3-digit quality is garbled (-1); '12.34' -> 123; whole-second time");

    feedAll(r, D_GGA);
    CHECK(feedAll(r, D_GGA_EMPTY) == 1, "an all-empty no-fix GGA parses");
    CHECK(r.fix().ggaQuality == 0 && !r.fix().ggaHasPos && r.fix().ggaLatI == 0 &&
          r.fix().ggaSats == -1 && r.fix().ggaHdopX10 == -1 && r.fix().ggaHms == -1,
          "...and inherits NOTHING from the good GGA before it");
    CHECK(r.fix().latI == 400000000, "...while the sticky latI keeps the last fix (the UART's rule)");

    const uint32_t gc = r.fix().ggaCount;
    feedAll(r, D_RMC);
    CHECK(r.fix().ggaCount == gc && r.fix().ggaQuality == 0 && r.fix().rmcHms == 120005,
          "an RMC leaves every gga* field alone");
    const uint32_t rc = r.fix().rmcCount;
    feedAll(r, D_GGA);
    CHECK(r.fix().rmcCount == rc && r.fix().rmcHms == 120005 && r.fix().rmcStatus == 'A',
          "a GGA leaves every rmc* field alone");

    feedAll(r, SOUTH_EAST_GGA);
    CHECK(r.fix().ggaLatI == -300000000 && r.fix().ggaLonI == 500000000 &&
          r.fix().ggaQuality == 2 && r.fix().ggaSats == 12 && r.fix().ggaHms == 235959,
          "south/east GGA, DGPS, 12 sats, 23:59:59.5 -> 235959");

    r.reset();
    CHECK(r.fix().ggaCount == 0 && r.fix().ggaQuality == -1 && r.fix().ggaSats == -1 &&
          r.fix().ggaHdopX10 == -1 && r.fix().ggaHms == -1 && !r.fix().ggaHasPos &&
          r.fix().ggaAltM == -10000, "reset() forgets the gga* fields");
  }

  /* ---- THIS GGA's own altitude, and which sentence completed last (0.9.82) -------------------
   * The woods UART's gate (self_pos.h) stamps a GGA's fix with that GGA's own altitude: the
   * sticky altM survives an empty field. lastKind says outright what feed() just completed, so
   * the gate needs no remembered count (one that reset() would make lie). */
  {
    NmeaReader r;
    CHECK(r.fix().ggaAltM == -10000 && r.fix().lastKind == NMEA_LAST_NONE, "nothing yet: no altitude, no kind");
    feedAll(r, D_GGA);
    CHECK(r.fix().ggaAltM == 100 && r.fix().lastKind == NMEA_LAST_GGA, "'100.0' -> 100 m; the last was a GGA");
    feedAll(r, GGA_HDOP2_ALTNEG);
    CHECK(r.fix().ggaAltM == -3 && r.fix().altM == -3, "'-3.2' -> -3 m, the same as the sticky altM");
    feedAll(r, D_GGA_EMPTY);
    CHECK(r.fix().ggaAltM == -10000 && r.fix().altM == -3,
          "an empty altitude: -10000 (THIS sentence had none) while the sticky altM keeps -3");
    feedAll(r, D_GGA_ALT_E);
    CHECK(r.fix().ggaAltM == -10000, "'1.2E3' is no number to the strict reader");
    feedAll(r, D_GGA_ALT_NEG0);
    CHECK(r.fix().ggaAltM == 0, "'-0.4' -> 0 m (toward zero)");
    feedAll(r, D_RMC);
    CHECK(r.fix().lastKind == NMEA_LAST_RMC && r.fix().ggaAltM == 0, "an RMC: the last was an RMC; ggaAltM untouched");
    feedAll(r, GSV);
    CHECK(r.fix().lastKind == NMEA_LAST_RMC, "a GSV (not read) leaves lastKind alone");
    feedAll(r, D_GGA);
    CHECK(r.fix().lastKind == NMEA_LAST_GGA, "a GGA again");
    r.reset();
    CHECK(r.fix().lastKind == NMEA_LAST_NONE && r.fix().ggaAltM == -10000, "reset() forgets both");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
