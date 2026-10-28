/* NMEA 0183 reader for the woods backplate's GPS (HGLRC M100 Mini, u-blox M10).
 *
 * Pure C++ on purpose — no Arduino, no floats in the position math — so the host
 * suite (tests/test_nmea.cpp) compiles the exact bytes that ship. The caller owns
 * all timekeeping: feed() reports WHEN something position-relevant completed and
 * the caller stamps it with millis(); nothing in here reads a clock.
 *
 * What it accepts, deliberately: bytes are ignored until a '$' — the woods UART
 * shares the phone with a wake-garbling habit (the first bytes after DFS idle
 * arrive mangled; see tools/panicwatch.py), so junk-tolerance is a requirement,
 * not politeness. A sentence without a checksum, or with a wrong one, is COUNTED
 * and dropped — never parsed "best effort". Only RMC and GGA are read; everything
 * else valid is counted as ignored.
 *
 * Position format: 1e-7 degrees in int32, the same fixed-point the mesh uses
 * (mesh_pos.h), so a fix drops into meshtastic_service without conversion.
 */
#ifndef NMEA_H
#define NMEA_H

#include <stdint.h>

struct NmeaFix {
  bool     valid;        // RMC status 'A' seen more recently than a 'V'
  int32_t  latI, lonI;   // 1e-7 degrees; only meaningful while/after valid
  uint32_t timeHms;      // hhmmss UTC from the last RMC/GGA that carried one (0 = none yet)
  uint32_t dateDmy;      // ddmmyy from the last RMC (0 = none yet)
  int      sats;         // GGA satellites in use (-1 = no GGA yet)
  int      hdopX10;      // GGA HDOP x10 (-1 = unknown)
  int      altM;         // GGA antenna altitude, metres, rounded toward zero (INT32-min sentinel unused; -10000 = unknown)

  /* ── THE LAST RMC'S OWN TIME, READ ONLY FROM THAT ONE SENTENCE (the clock's input) ──────
   * 🛑 timeHms/dateDmy above are NOT a timestamp and must never be paired into one: timeHms
   * is shared with GGA, and dateDmy survives an RMC whose date field was empty. Joined, they
   * can put 00:00:01 on YESTERDAY's date at midnight — a clock a whole day wrong from two
   * individually correct fields. clock_source.cpp reads ONLY these, and each is rewritten by
   * every RMC (absent = -1 / 0), so a field can never be inherited from an older sentence. */
  uint32_t rmcCount;     // RMC sentences parsed; a change means a NEW one completed
  char     rmcStatus;    // 'A' / 'V' as the sentence said it; 0 when empty or anything else
  char     rmcMode;      // NMEA 2.3+ mode indicator (A D E F M N P R S) after the date; 0 = absent
  int32_t  rmcHms;       // hhmmss, -1 when absent or not exactly "hhmmss[.f...]"
  int16_t  rmcMs;        // the time's fraction in ms (0 when none) - 0 at the M100's 1 Hz
  int32_t  rmcDmy;       // ddmmyy, -1 when absent or not exactly six digits

  /* ── MOTION, ALSO READ ONLY FROM THE LAST RMC (0.9.80, the Almanac's Position screen) ────
   * Speed over ground (RMC field 6, knots) and course over ground (field 7, degrees TRUE), in
   * fixed point - no floats here (see the file header). The rmc* rule above applies: EVERY RMC
   * rewrites both, and a field that is empty, absent or unreadable in THAT sentence is -1, so a
   * receiver that stops reporting a course (it does, when you stand still) can never show the
   * last walk's course as if it were now. A GGA never touches them. */
  int32_t  speedKnX100;  // knots x100: "0.36" -> 36, "5" -> 500, "12.345" -> 1234 (truncated); -1 = absent
  int32_t  courseX10;    // degrees true x10, 0..3599: "212.5" -> 2125, "360.0" -> 0; -1 = absent or > 360

  /* ── THE LAST GGA, READ ONLY FROM THAT ONE SENTENCE (the phone-over-WiFi gate, self_pos.h) ──
   * 🛑 sats/hdopX10/latI/lonI above are STICKY: a GGA overwrites each only when its field
   * parses, `valid` changes only on an RMC, and a quality-0 GGA leaves the last coordinates
   * standing. The UART path lives with that (its RMC and GGA come from one receiver, a few ms
   * apart). A stream relayed by an app on another phone does not: a run of GGAs with no RMC
   * (gpsdRelay's TCP clients share ONE queue, so a ghost connection can take every RMC) would
   * re-stamp the last good coordinates, with the last good sats and HDOP, as a fresh fix. So
   * every GGA rewrites ALL of these from itself, present or not — the rmc* rule above. A GGA
   * never touches the rmc* fields, and an RMC never touches these.
   * ⚠ "The UART lives with that" was wrong (review 2026-10-10): the same sticky fields let a
   * run of no-fix GGAs with the RMCs lost re-stamp the plate's last fix as NOW. The UART is
   * judged per sentence too since 0.9.82 (self_pos.h, selfGpsApply), which reads valid/latI/
   * lonI only right after an RMC (then they are that RMC's own) and the gga* fields for a GGA.
   * Nothing in the firmware reads sats/hdopX10/altM/timeHms/dateDmy any more; they stay as the
   * tests pin them. */
  uint32_t ggaCount;     // GGA sentences parsed; a change means a NEW one completed
  int32_t  ggaQuality;   // fix quality digit (0 none, 1 GPS, 2 DGPS, 4/5 RTK, 6 dead reckoning...); -1 absent/garbled
  bool     ggaHasPos;    // THIS sentence carried readable coordinates (whatever its quality said)
  int32_t  ggaLatI, ggaLonI;   // 1e-7 deg; 0 unless ggaHasPos
  int32_t  ggaSats;      // satellites in use; -1 = empty field, -2 = present but not plain digits
  int32_t  ggaHdopX10;   // HDOP x10 by the strict reader ("1.23" -> 12, "12" -> 120);
                         // -1 = empty, -2 = present but not understood ("1.2E-4", "-1", "1.2.3")
  int32_t  ggaHms;       // hhmmss by the strict reader (as rmcHms), -1 = absent/garbled
  /* The woods UART's gate (self_pos.h, selfGpsApply) stamps the altitude with a GGA's own fix,
   * so it takes the altitude from that GGA alone too: altM above survives a GGA whose field was
   * empty. Whole metres toward zero ("-3.2" -> -3), strict ("1.2E3" is no number); -10000 =
   * empty or unreadable. */
  int32_t  ggaAltM;

  /* Which sentence completed LAST: NMEA_LAST_RMC or NMEA_LAST_GGA (NMEA_LAST_NONE before either).
   * What feed() returning true just completed, said outright: the count comparisons above need a
   * remembered count, and that memory goes wrong across reset() (`gps on`, a baud retune), where
   * a GGA with the RMC count back at 0 read as "an RMC". */
  uint8_t  lastKind;
};
#define NMEA_LAST_NONE  0
#define NMEA_LAST_RMC   1
#define NMEA_LAST_GGA   2

class NmeaReader {
public:
  NmeaReader() { reset(); }
  void reset();

  /* Feed one raw UART byte. Returns true when an RMC or GGA sentence just
   * completed WITH a good checksum — including a no-fix RMC (status V), so the
   * caller can tell "GPS alive, sky not seen yet" from "no GPS talking at all".
   * Read the result from fix(). */
  bool feed(char c);

  const NmeaFix& fix() const { return f; }

  // Counters for the serial `gps` status line — what a bench session asks first.
  // bytes rising while sentences stays 0 = wrong baud, THE first bench question.
  uint32_t bytes()       const { return nBytes; }        // every byte fed
  uint32_t sentences()   const { return nSentences; }    // good checksum, any type
  uint32_t badChecksum() const { return nBadChecksum; }
  uint32_t overruns()    const { return nOverrun; }      // line hit the cap; discarded

  /* Parse one complete sentence (between '$' and '*', checksum already verified).
   * Public and static for the host tests; feed() is the production entry. */
  static bool parseSentence(const char* body, NmeaFix* f);

private:
  static const int LINE_CAP = 96;   // NMEA maximum is 82 including framing
  char     line[LINE_CAP];
  int      len;
  bool     inSentence;
  NmeaFix  f;
  uint32_t nBytes, nSentences, nBadChecksum, nOverrun;
};

#endif // NMEA_H
