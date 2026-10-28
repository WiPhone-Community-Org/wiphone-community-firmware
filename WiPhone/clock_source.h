/*
 * clock_source.h — WHO set the phone's clock, and the rules that let GPS or the mesh set it.
 *
 * {user}, 2026-09-25: *"for wiphones, are we able to also do gps time as long as the gps is
 * enabled?"* Until 0.9.79 the clock had exactly one source — NTP over WiFi — and one flag,
 * set the first time NTP answered. No RTC, no GPS time, nothing from the mesh: a phone that
 * had not seen WiFi since boot did not know what day it was, however long its GPS had had a
 * fix. In the woods that is every phone, all day.
 *
 * Now there are three sources, ranked:
 *
 *   ntp   authoritative whenever it is PRESENT (it set the clock within CLOCK_NTP_FRESH_MS).
 *         Never overridden by anything while fresh.
 *   gps   the woods plate's RMC time. Trusted: a GNSS receiver with a fix knows UTC to well
 *         under a microsecond; everything between it and this clock (the NMEA output delay,
 *         the UART, the loop) costs a few hundred ms, and the clock only keeps seconds.
 *   mesh  a Meshtastic Position packet's `time` field, for phones with no GPS. LOWER TRUST:
 *         nothing on the air is authenticated — a channel key encrypts but does not
 *         authenticate (AES-CTR, no MAC: see the spoofing note at clockMeshOffer) — so it only
 *         ever fills an UNKNOWN clock, and trusted() is false.
 *
 * isTimeKnown() is true for any of them (a clock on the screen, message times, "5 min ago").
 * clockSourceTrusted() is the bar for everything that either LEAVES the phone, DESTROYS or
 * SILENCES something on the strength of the time — the position beacon's time, booksync's
 * turnedAt, the replay ring, waypoint expiry, KOSync's "provably older", the mirrored-text
 * buzz's "is it recent". Those all required a known clock before 0.9.79, which meant NTP; they
 * now require ntp or gps, never mesh, so a mesh clock changes NOTHING any other device sees,
 * deletes nothing and hushes nothing. (clock.h has the list.)
 *
 * Pure C++, no Arduino: tests/test_clocksrc.cpp runs every rule here on a Mac, fed through the
 * shipping NMEA reader (nmea.cpp). clock.cpp is only the glue: it takes the mutex, asks these
 * functions, and applies the answer.
 */
#ifndef CLOCK_SOURCE_H
#define CLOCK_SOURCE_H

#include <stdint.h>
#include "nmea.h"

enum ClockSource {
  CLOCK_SRC_NONE = 0,               // not set: isTimeKnown() false
  CLOCK_SRC_NTP  = 1,
  CLOCK_SRC_GPS  = 2,
  CLOCK_SRC_MESH = 3,
};
const char* clockSourceName(int src);           // "none" "ntp" "gps" "mesh"
bool        clockSourceTrusted(int src);        // ntp or gps

/* ── SANE YEARS ────────────────────────────────────────────────────────────────────────────
 * 2026 <= year < 2080. The floor is this firmware's own year: nothing it will ever see is
 * older, and it is exactly the bar a GPS WEEK-ROLLOVER date fails — a receiver that has lost
 * track of the 1024-week era reports 2006/2007 (19.6 years back), which would otherwise be a
 * perfectly well-formed date.
 *
 * 🛑 THE CEILING IS 2080, NOT 2100, AND THAT IS NOT ABOUT ARITHMETIC. NMEA's year is TWO
 * digits on the wire, and 80..99 is how a receiver with no time yet reads out its DEFAULTS:
 * `060180` is the GPS epoch (1980-01-06), and 99/00 come out of resets. Read as 2000 + yy —
 * which is what clockGpsRmcTime() does, and what TinyGPS++'s year() does inside every stock
 * Meshtastic node, so the SAME default reaches us over the mesh as a Position time in 2080 —
 * those are well-formed dates 54 years in the future. With a 2100 ceiling (0.9.79's first
 * cut) a pair of 'A' RMCs carrying one would have become a TRUSTED clock: stamped on every
 * position beacon, expiring every pin at the next sweep, hiding every KOSync place as
 * "provably older" — and every later GPS pair would AGREE with it, so only NTP could undo it.
 * COVEY's own GPS-time code draws the same line for the same reason (covey_ui/gpstime.py,
 * MAX_YEAR = 2079), so the two devices refuse the same dates. Whoever is still flashing this
 * firmware in 2079 has 53 years' notice. */
#define CLOCK_YEAR_MIN            2026
#define CLOCK_YEAR_LIMIT          2080                 // first year NOT accepted (COVEY: MAX_YEAR 2079)
#define CLOCK_UNIX_MIN            1767225600u          // 2026-01-01T00:00:00Z (test-checked)
#define CLOCK_UNIX_LIMIT          3471292800u          // 2080-01-01T00:00:00Z (test-checked)
bool clockUnixSane(uint32_t unixSec);

/* Is this 48-byte datagram an NTP answer the clock may take? The UNIX seconds of its transmit
 * time in *unixOut when true. Refused: short; LI = 3 (the server says it is unsynchronised);
 * mode other than 4 (server); stratum 0 (a kiss-o'-death) or above 15; a transmit time before
 * 1970, which `ntpTime - SEVENTY_YEARS` would wrap to 2036+; and anything clockUnixSane()
 * refuses. Until 0.9.79 Clock::update() took any 48 bytes on its port with a non-zero,
 * changed time as a TRUSTED clock — the one source the 2080 ceiling above did not cover, and
 * NTP is the source everything else defers to (review, 2026-09-25). Who SENT it (the server
 * asked, port 123) is checked by the caller, which has the socket. */
#define CLOCK_NTP_1900_TO_1970    2208988800u
bool clockNtpReplySane(const uint8_t* pkt, int len, uint32_t* unixOut);

/* A calendar UTC instant -> ms since 1970. False (out untouched) unless it is a real instant
 * in a sane year: month 1-12, the day within THAT month (29 Feb only in a leap year), hh < 24,
 * mm < 60, ss < 60 (a leap second's :60 is refused — the next reading is one second away),
 * ms < 1000. No libc, no time zone: timegm() is not portable and mktime() is local time. */
bool clockCivilToUnixMs(int year, int month, int day, int hour, int minute, int second, int ms,
                        int64_t* outMs);

/* Every outcome the clock can report, one list so `clock` and the log print the same words. */
enum ClockVerdict {
  CLK_NONE = 0,                     // nothing judged yet
  /* One RMC, judged alone. */
  CLK_GPS_NO_FIX,                   // status not 'A' (or 'A' with unreadable coordinates) — see the NOTE ON 'V' below
  CLK_GPS_BAD_MODE,                 // mode indicator says it is not a GNSS solution
  CLK_GPS_NO_TIME,                  // the RMC's own time field empty or garbled
  CLK_GPS_NO_DATE,                  // ...its date field
  CLK_GPS_BAD_DATE,                 // not a calendar instant (31 Sep, 24:00, :60)
  CLK_GPS_BAD_YEAR,                 // outside 2026..2079 (a week-rollover date, an 80..99 default)
  /* Two readings. */
  CLK_GPS_FIRST,                    // one good reading, waiting for the next to agree
  CLK_GPS_NOT_LATER,                // the next did not move forward (repeat or backwards)
  CLK_GPS_GAP,                      // too long between them: they prove nothing together
  CLK_GPS_DISAGREE,                 // their seconds and the millis() between them disagree
  /* The policy, for a pair that agreed. */
  CLK_GPS_SET_UNKNOWN,              // SET: the clock was not set
  CLK_GPS_SET_UPGRADE,              // SET: the clock came from the mesh (lower trust)
  CLK_GPS_SET_DRIFT,                // SET: off by more than CLOCK_GPS_STEP_MS, NTP not fresh
  CLK_GPS_KEEP_AGREES,              // kept: within CLOCK_GPS_STEP_MS
  CLK_GPS_KEEP_NTP_FRESH,           // kept: NTP set it recently and stays authoritative
  /* A mesh Position's time. */
  CLK_MESH_NO_TIME,                 // the packet carried no time (stock nodes strip it)
  CLK_MESH_BAD_YEAR,                // outside 2026..2079
  CLK_MESH_BAD_NODE,                // node 0 / broadcast: not a sender
  CLK_MESH_CLOCK_KNOWN,             // the clock is set: the mesh never overrides it
  CLK_MESH_WAIT,                    // public channel: held until a second node agrees
  CLK_MESH_SET_PRIVATE,             // SET: one packet on a private channel
  CLK_MESH_SET_AGREED,              // SET: two different nodes agreed within 60 s
};
const char* clockVerdictText(int verdict);      // "kept - agrees with the clock", ...
bool        clockVerdictSets(int verdict);      // one of the SET verdicts

/* ── GPS, STEP 1: ONE RMC ON ITS OWN ────────────────────────────────────────────────────────
 * Reads ONLY the rmc* fields of `fx` (see nmea.h for why the others are never paired).
 * True with the sentence's UTC in *utcMs; false with the reason in *why.
 *
 * 🛑 THE NOTE ON 'V': A NO-FIX RMC's TIME IS NOT USED, EVEN WHEN IT LOOKS COMPLETE. u-blox
 * receivers (the M100 Mini is a u-blox M10) print a time and date in RMC before any fix: from
 * the backup RTC if one kept running, else from the FIRST satellite decoded — before the
 * almanac has delivered the GPS-UTC leap-second count, before the week number is resolved,
 * sometimes a firmware-default date outright. The fixture this reader was built on is one:
 * `$GNRMC,081836.00,V,,,,,,,130826,,,N` — a date and a time, from a receiver that could see
 * NO satellites (tests/test_nmea.cpp, NOFIX_RMC). 'V' plus a plausible date is exactly the
 * case that cannot be told apart from 'V' plus a wrong one. Waiting for 'A' costs a minute
 * outdoors and guarantees the time came from a solved position.
 *
 * The mode indicator (NMEA 2.3+), when present, must be a GNSS solution: A autonomous,
 * D differential, F/R RTK, P precise. Refused: N (no fix), E (dead reckoning — the receiver's
 * own estimate), M (MANUAL INPUT) and S (SIMULATOR) — the last two are, literally, a time
 * somebody typed. Absent (an NMEA 2.1 receiver) is accepted: the status carries it. */
bool clockGpsRmcTime(const NmeaFix* fx, int64_t* utcMs, int* why);

/* ── GPS, STEP 2: TWO CONSECUTIVE READINGS THAT AGREE ─────────────────────────────────────
 * One sentence is one opinion. A checksum proves the bytes arrived, not that the receiver was
 * right — and the moment a receiver first reports 'A' is exactly when its time solution is
 * newest. So the clock moves only on a PAIR: the previous good reading and this one, with
 *   - this one strictly LATER in UTC (a repeated sentence is not a second opinion),
 *   - at most CLOCK_GPS_PAIR_MAX_GAP_MS of millis() between them (a fix lost and regained in
 *     between breaks the chain; a bad sentence between them breaks it too — the caller
 *     resets the pair on every refused RMC, so the two are CONSECUTIVE),
 *   - their UTC difference equal to the millis() difference within CLOCK_GPS_PAIR_TOL_MS.
 * The tolerance is not about the GPS: it is the LOOP. Bytes are read in the superloop, and
 * a loop stall (the mesh DB save alone is a measured 1.5 s) delays the read of whichever
 * sentence it lands on. A stall of more than the tolerance fails the pair and the next one,
 * a second later, forms normally.
 *
 * On success, *utcAtRxMs is UTC at millis() == rxMs (this reading's arrival), taken from
 * whichever reading was read SOONER: a reading can only ever be read LATE, never early, so
 * the larger of the two estimates is the closer one. */
#define CLOCK_GPS_PAIR_MAX_GAP_MS  5000u
#define CLOCK_GPS_PAIR_TOL_MS      1000
struct ClockGpsPair {
  bool     have;                    // a previous good reading is held
  int64_t  utcMs;                   // its UTC
  uint32_t rxMs;                    // the millis() it was read at
};
void clockGpsPairReset(ClockGpsPair* p);
bool clockGpsPairFeed(ClockGpsPair* p, int64_t utcMs, uint32_t rxMs, int64_t* utcAtRxMs,
                      int* why);

/* ── GPS, STEP 3: DOES AN AGREEING PAIR MOVE THE CLOCK? ───────────────────────────────────
 * In order:
 *   clock not set                         -> SET   (CLK_GPS_SET_UNKNOWN)
 *   clock set from the mesh               -> SET   (CLK_GPS_SET_UPGRADE: gps outranks mesh)
 *   NTP set it within CLOCK_NTP_FRESH_MS  -> KEEP, WHATEVER THE DIFFERENCE (CLK_GPS_KEEP_NTP_FRESH)
 *   off by more than CLOCK_GPS_STEP_MS    -> SET   (CLK_GPS_SET_DRIFT)
 *   otherwise                             -> KEEP  (CLK_GPS_KEEP_AGREES)
 *
 * Why NTP wins while fresh, even against a GPS that disagrees: NTP is what this clock has
 * always been and what every other device in the house (COVEY, the X4's server) agrees with;
 * a disagreement is far more likely to be ours (a bad sentence that slipped through, a loop
 * stall) than pool.ntp.org's, and a clock that flips between two sources is worse than
 * either. The disagreement is still MEASURED (*diffMs) and logged, so if it is ever NTP's
 * the log shows it. FRESH is three of NTP's own 10-minute resyncs (TIME_UPDATE_DELAY_MS):
 * three missed in a row means WiFi or NTP is gone, and from then on the clock is free-running
 * on the crystal and GPS may correct it.
 *
 * Why 2 s and not 0: NTP here keeps whole seconds (clock.cpp drops the fraction) and a GPS
 * sentence is read a few hundred ms late, so two correct sources routinely differ by up to a
 * second. Stepping the clock over that would move it on every pair for no gain.
 *
 * *diffMs = gps - clock, whenever the clock was set (0 otherwise). */
#define CLOCK_NTP_FRESH_MS         (30u * 60u * 1000u)
#define CLOCK_GPS_STEP_MS          2000
#define CLOCK_NEVER_MS             0xFFFFFFFFu        // msSinceNtp: NTP never set it
struct ClockNow {
  int      source;                  // ClockSource
  int64_t  utcMs;                   // what the clock reads at the moment being judged
  uint32_t msSinceNtp;              // since NTP last set it; CLOCK_NEVER_MS = never
};
int clockGpsDecide(const ClockNow* c, int64_t gpsUtcMs, int64_t* diffMs);

/* ── THE WHOLE GPS PATH FOR ONE NEW RMC (what clock.cpp calls) ────────────────────────────
 * Step 1; a refused sentence RESETS the pair (so a pair is always two CONSECUTIVE good RMCs
 * — "good, bad, good" is not two agreeing readings); step 2; and, for an agreeing pair, step
 * 3 against `now` (the clock as it reads at rxMs). Returns the verdict of the last step that
 * ran. On a SET verdict *setUtcMs is the UTC to give the clock at millis() == rxMs.
 * *diffMs is step 3's (0 when step 3 did not run). */
int clockGpsStep(ClockGpsPair* p, const NmeaFix* fx, uint32_t rxMs, const ClockNow* now,
                 int64_t* setUtcMs, int64_t* diffMs);

/* ── MESH TIME: ONLY INTO AN UNKNOWN CLOCK, AND NEVER ON ONE STRANGER'S WORD ──────────────
 * For phones with no GPS (phone 1). A Meshtastic Position's `time` (field 4) is the sender's
 * clock when it took the position. Adopted ONLY when:
 *   - this clock is NOT SET — the mesh never overrides ntp, gps, or an earlier mesh time;
 *   - the time is in a sane year (2026..2079 — a 2080 is a stock node's GPS default, above);
 *   - and EITHER it arrived on a PRIVATE channel (a key that is not the well-known default and
 *     not empty — MeshtasticService::channelIsPublic() is the test), where one packet is
 *     enough; OR, on a public channel, TWO DIFFERENT nodes agree within CLOCK_MESH_AGREE_S
 *     (each aged by the millis() between their arrivals).
 *
 * WHY A PRIVATE CHANNEL NEEDS ONE PACKET AND LONGFAST NEEDS TWO: the realistic failure is not
 * an attacker, it is one node with a wrong clock. On LongFast that is any stranger's radio in
 * range — a Pi with no RTC, a node somebody set by hand — so one opinion is not enough. On a
 * private channel the senders are your own party's devices: WiPhones, which (after this
 * change) put a time on the air ONLY when it is trusted (ntp or gps — never a mesh-derived
 * time, so a second-hand time cannot come back as an "independent" witness), and stock
 * Meshtastic nodes, whose PositionModule — as I understand upstream; it is not verifiable in
 * this tree — strips `time` unless their own clock came from GPS or NTP. And a woods trip is
 * often exactly two phones: requiring two nodes there would mean phone 1 never gets a time.
 *
 * ⚠ THE SPOOFING TRADE-OFF, stated plainly. Nothing on the mesh is authenticated beyond the
 * channel key. On LongFast anyone can transmit, under ANY node number — "two different nodes"
 * defends against one misconfigured radio, not against somebody who means it, and who can
 * invent two node numbers as easily as one.
 *
 * 🛑 A PRIVATE CHANNEL DOES NOT BOUND THE TIME EITHER — not even to "behind". (0.9.79's first
 * cut said a keyless outsider could only replay a packet, setting the clock to when it was
 * recorded, "behind, never ahead". That was wrong.) Meshtastic channel crypto is AES-CTR with
 * NO MAC (meshCryptCtr, mesh_crypto.cpp): the ciphertext is the plaintext XOR a keystream, so
 * flipping a ciphertext bit flips the same plaintext bit and nothing notices. A Position's
 * plaintext is predictable — meshPosBuild() writes fixed32 lat, lon and time at fixed
 * offsets, and the time is about the moment the packet was heard — so an outsider who
 * recorded ONE timed Position off the channel can XOR any delta into those four bytes, keep
 * the sender and packet id (they ARE the nonce; changing them changes the keystream), wait
 * out the 16-entry packet-id dedup (seenPacketId), and replay it. That puts nearly ANY time
 * on this phone, AHEAD included, with no key. It needs someone in range, recording, who knows
 * the format: a deliberate act, not an accident — but these rules do not stop it.
 *
 * WHAT DOES BOUND THE DAMAGE is where a mesh time is allowed to go, not how it got in: it is
 * never TRUSTED (KOSync ignores it, waypoints never expire on it, the mirrored-text buzz gate
 * treats it as unknown, nothing this phone transmits carries it — so it is never re-radiated),
 * it never replaces a set clock, and NTP or GPS replaces IT the first time either appears.
 * What it can get wrong is what the phone SHOWS — the clock, message times and their order,
 * and the `sun` legal-light times, which say "via mesh" and "Mesh time: check it!" when it
 * is. (Message times are also WRITTEN, to the SIP store; those stamps are provisional and NTP
 * or GPS moves them — see the end of this file.) Any new caller that ACTS on the time must ask isTimeTrusted(), not isTimeKnown(). */
#define CLOCK_MESH_AGREE_S         60
#define CLOCK_MESH_CAND_MAX        4
#define CLOCK_MESH_CAND_MAX_AGE_MS (60u * 60u * 1000u)   // older candidates are forgotten
struct ClockMeshCand {
  uint32_t node;                    // 0 = empty slot
  uint32_t t;                       // its time (unix s)
  uint32_t rxMs;                    // millis() when it arrived
};
struct ClockMeshVote {
  ClockMeshCand c[CLOCK_MESH_CAND_MAX];
};
void clockMeshVoteReset(ClockMeshVote* v);
int  clockMeshVoteCount(const ClockMeshVote* v, uint32_t nowMs);   // live candidates held
/* One Position's time. On a SET verdict *adoptUtc is UTC (s) at millis() == rxMs, and the
 * vote is cleared. A public-channel packet that does not yet agree with anybody is HELD
 * (one slot per node, the newest; the oldest goes when full) and CLK_MESH_WAIT returned. */
int  clockMeshOffer(ClockMeshVote* v, int clockSource, uint32_t node, bool privateChannel,
                    uint32_t t, uint32_t rxMs, uint32_t* adoptUtc);

/* ── THE SIP MESSAGE STORE UNDER A MESH CLOCK (0.9.79 review, SA-3) ──────────────────────────
 * Message times follow any KNOWN clock — the list at the top of this file puts them on the
 * SHOWN side, and a woods trip on a mesh clock must not leave its texts at the unknown-time
 * sentinel, which sorts newest FOREVER ("a new text lands mid-thread", Storage.cpp). But that
 * store is not a display: it WRITES the time to SPIFFS, and until this a text stamped from a
 * mesh clock kept that stamp for good. A wrong mesh time — AHEAD to 2079 by the CTR replay
 * above — then sat those texts on top of their threads long after NTP had put the clock right,
 * and it used up the loop's one forced repair (WiPhone.ino, waitingForClockUpdate) on itself,
 * so the first NTP answer re-stamped nothing. Both broke "NTP or GPS replaces IT".
 *
 * So a stamp written under a mesh clock is PROVISIONAL. The store marks it with the mesh
 * set's id (key "tm", Storage.cpp), and the first load under a TRUSTED clock finalises it:
 *   - written under the mesh clock that NTP or GPS replaced THIS boot: moved by exactly what
 *     that replacement moved the clock (corrS) — right to the second however wrong the mesh
 *     was, and a no-op for the usual right one (phone 2's GPS beacon);
 *   - written under an EARLIER boot's mesh clock (no offset is known any more — there is no
 *     RTC): kept if it is not in the future, because a past stamp cannot pin a text above
 *     later ones; one in the future is re-stamped the way an unknown time is (false below),
 *     by clockMsgRestamp() in the band BELOW this load's unknown-time repairs: those are THIS
 *     boot's texts, and an earlier boot's are older.
 * The id is random per mesh set (clock.cpp), so a replay of the same packet in two boots
 * does not make one boot's texts take the other boot's offset. Everything is UTC seconds. */
struct ClockMsgStamp {
  uint32_t utc;       // now; 0 = the clock is not known (the store's "unknown time")
  uint32_t meshId;    // nonzero: `utc` is a MESH clock's, set `meshId` -> stamps are provisional
  uint32_t corrId;    // nonzero: NTP or GPS replaced mesh set `corrId` this boot...
  int32_t  corrS;     // ...moving the clock by this many seconds (trusted minus mesh)
};
/* (trusted - mesh) in whole seconds, rounded to nearest, clamped to int32. */
int32_t clockMeshCorrS(int64_t trustedMs, int64_t meshMs);
/* Finalise one provisional stamp `t`, written under mesh set `id`, against `st`. Call it ONLY
 * under a trusted clock (st->utc != 0 and st->meshId == 0); anything else returns false with
 * *out untouched. true: *out is the final stamp (corrected when `id` is st->corrId, else `t`).
 * false: it would land before 2026 or after st->utc, so no known offset makes it right — the
 * caller re-stamps it like an unknown time. */
bool clockMsgFinal(uint32_t t, uint32_t id, const ClockMsgStamp* st, uint32_t* out);
/* The new stamps for one partition's texts that clockMsgFinal() refused. `prov[0..n)` are their
 * provisional stamps in FILE order; out[i] (never aliasing prov) gets prov[i]'s replacement.
 * The n stamps are DISTINCT and fill bandEnd-(n-1) .. bandEnd, in the order the texts came:
 * the newest provisional stamp takes bandEnd, and between EQUAL ones the LATER in the file is
 * newer. 🛑 NOT "ascending in file order", which is what the sentinel repair does (Storage.cpp)
 * and is right only there: every sentinel is the same ffffffff, so its file order IS arrival
 * order. A partition is sorted NEWEST-FIRST (messageCompare is strcasecmp(b.t, a.t);
 * reorderLast() puts a new text before the first OLDER one, so after its equals) — ascending
 * stamps in that order handed the newest text the earliest stamp and reversed the thread for
 * good (the misc repair round; the mark goes, so nothing would ever put it back). Ranked, not
 * walked, so the answer does not depend on the file being sorted. O(n^2): n <= a partition. */
void clockMsgRestamp(const uint32_t* prov, uint32_t n, uint32_t bandEnd, uint32_t* out);

#endif // CLOCK_SOURCE_H
