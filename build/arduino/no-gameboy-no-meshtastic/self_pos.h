/*
 * self_pos.h — WHERE THIS PHONE IS: which source to believe, and the phone-over-WiFi source.
 *
 * Until 0.9.82 "my position" meant one thing, the woods plate's GPS on the UART, and phone 1
 * (no plate) never had one. Now a second source can stand in for it: the GPS of the Android or
 * iPhone whose hotspot this phone has joined, read as NMEA from an app on that phone —
 *
 *   gpsdRelay  (Android, F-Droid)  TCP server, we tell people to use port 10110 (IANA nmea-0183)
 *   Share GPS  (Android, Play)     TCP server, ditto 10110
 *   GPS 2 IP   (iPhone)            socket/server mode, its own default 11123
 *
 * — and, last and coarsest, a Wi-Fi location: beaconDB's answer for the access points this phone
 * hears, believed only when two independent lookups agree (geoloc.h), and only while later scans
 * still hear those access points (the NET rules below).
 * 🛑 NO IP GEOLOCATION, ANYWHERE, EVER: on a phone hotspot it answers with the carrier's
 * internet exit, typically 180-200 km away, with no hint that it is wrong.
 *
 * Ranking, best first: GPS (this phone's own receiver) > PHONE (relayed) > NET (Wi-Fi). A wrong
 * location is worse than none (the owner's rule), so NET has the most rules and the least trust:
 * shown on this phone only while scans confirm it, sent only when the owner chose "map + mesh"
 * on the screen, only as a ~182 m cell, and never past a precise fix.
 * The user's hand-set pin is not a source here: it stays where meshtastic_service/app_maps rank
 * it, above a stale or poor measured fix and below any live one.
 *
 * WHAT IS PURE HERE, AND WHY: every rule that decides whether a position is put on somebody
 * else's map — freshness, quality, the stale-data guards, which port to try — runs on a Mac in
 * tests/test_selfpos.cpp. The socket, the service slots and the screens are the glue
 * (phone_pos.cpp, meshtastic_service.cpp), and they only ask these functions.
 *
 * 🛑 THE PHONE'S NMEA NEVER SETS THE CLOCK (never ntpClock.gpsSentence) and never touches the
 * woods plate's reader (gGpsNmea / gGpsReader). Unauthenticated bytes from a LAN do not get to
 * be a TRUSTED time (clock_source.h); NTP already runs on any network that has a gateway.
 */
#ifndef SELF_POS_H
#define SELF_POS_H

#include <stddef.h>
#include <stdint.h>
#include "nmea.h"
#include "mesh_pos.h"

// ---------------------------------------------------------------- sources and slots
enum SelfSrc {
  SELF_NONE  = 0,
  SELF_GPS   = 1,      // the woods plate's receiver (UART NMEA)
  SELF_PHONE = 2,      // the hotspot phone's GPS, relayed as NMEA over WiFi
  SELF_NET   = 3,      // a Wi-Fi location (geoloc.h): two agreeing beaconDB lookups
};
#define SELF_SRC_COUNT   3          // slots[] is indexed src - 1

/* Freshness — the same two bars the GPS has always had (meshtastic_service.h's MESH_GPS_FRESH_MS
 * and MESH_POS_TX_FRESH_MS must equal these; tests/test_selfpos.cpp reads that header and fails if
 * they drift). SHOW: on this phone's own screens (its owner knows where they are). SEND: worth
 * other people's trust — 30 missed 1 Hz epochs is a guess dressed as a measurement. */
#define SELF_SHOW_FRESH_MS  120000UL
#define SELF_TX_FRESH_MS     30000UL

/* One source's last fix. ~36 B, RAM only: nothing here is ever written to NVS (a position kept
 * across a restart would come back "fresh" with no receiver behind it). */
struct SelfSlot {
  int32_t  latI, lonI;   // 1e-7 deg
  /* millis() of the fix; 0 = never (selfSlotSet turns a 0 stamp into 1). NET: of the LATEST scan
   * that still heard the fix's access points (selfNetConfirm) — the freshness clock. */
  uint32_t fixMs;
  int      sats;         // from the SAME sentence as the position; -1 = it did not say
  int      hdopX10;      // ditto
  bool     enabled;      // the source is switched on (GPS: gpsen; PHONE: "Phone location: auto";
                         // NET: "Wi-Fi location" is not off)
  bool     injected;     // written by `pos inject` (or `netloc answer`): shown, never sent by the schedule
  // ---- NET only (0 / false for the GNSS sources) ----
  bool     noTx;         // "map only": shown on this phone, never sent. A setting: kept by clear.
  uint16_t accM;         // the fix's accuracy, metres (geoloc's max(50, accA, accB, apart))
  uint32_t firstMs;      // millis() of the scan the fix was computed from
  uint32_t scanUtc;      // the trusted UTC of the latest confirming scan (0 = clock not trusted)
};

// Write a whole fix at once — there is no partial update, so nothing can be inherited from an
// older sentence. `enabled` (and `noTx`) are left as they are; the NET fields are zeroed.
void selfSlotSet(SelfSlot* s, int32_t latI, int32_t lonI, int sats, int hdopX10,
                 uint32_t nowMs, bool injected);
void selfSlotClear(SelfSlot* s);                 // forget the fix (`enabled`, `noTx` kept)

/* ── THE WI-FI SLOT (SELF_NET) ──────────────────────────────────────────────────────────────
 * A Wi-Fi fix is not a measurement of where the phone is: it is where some access points are,
 * true only while the phone still hears them. So its clock is the SCANS, not the lookup:
 *   - selfNetSet stamps it with the scan the lookup was computed from (firstMs = fixMs);
 *   - every later scan that still hears 2/3 of those devices CONFIRMS it (selfNetConfirm: fixMs
 *     and scanUtc become that scan's); one that does not CLEARS it at once (geoloc.h, geoOnScan)
 *     — review item 2: carried on a walk or a drive, an old Wi-Fi fix is a confident wrong dot;
 *   - SHOWN while the last confirming scan is under SELF_NET_SHOW_MS old: the auto-switcher's
 *     own 10-minute cadence while connected, plus room for that scan to be late. A scan held off
 *     (the heap gate, a join's hold-off, a call) would expire a fix that is still true and cost
 *     two more lookups, so at SELF_NET_REFRESH_AT_MS with no scan yet the glue asks for one
 *     (selfNetWantsRefresh) — a scan, not a lookup. Expired is GONE: no grey "last Wi-Fi" marker,
 *     unlike the GNSS sources (selfNewest skips it);
 *   - SENT only when (a) the setting is "map + mesh" (noTx clear), (b) TWO agreeing scans at
 *     least SELF_NET_SETTLE_MS apart stand behind it (fixMs - firstMs: a fix taken while moving
 *     never goes out), and (c) the latest confirming scan finished under SELF_NET_CONFIRM_MS
 *     before the send. Field 4 (time) carries THAT scan's UTC, never the send's (selfFixSendTime)
 *     — the position is as of the scan;
 *   - sent as location_source UNSET (a network guess claims no measurement, and stock receivers
 *     then take no time from it) and precision_bits SELF_NET_PREC_BITS, cut to the cell's centre
 *     (receivers draw ~182 m: the WiPhone map and COVEY now draw rings). 17, not 18: at 18 the
 *     ring is 91 m, no bigger than the floored accuracy, and it ignores the snap to the cell
 *     centre (a half-diagonal of ~115 m at 40 deg). */
#define SELF_NET_SHOW_MS        720000UL   // 12 min after the latest confirming scan
#define SELF_NET_REFRESH_AT_MS  630000UL   // the cadence's scan 30 s late: ask for one
#define SELF_NET_CONFIRM_MS      60000UL   // sending: a confirming scan this recent
#define SELF_NET_SETTLE_MS       60000UL   // sending: two agreeing scans at least this far apart
#define SELF_NET_PREC_BITS          17
#define SELF_NET_MAX_ACC_M         100     // geoloc never claims more; anything else is not a fix
/* A fix from the lookup on the scan at `scanMs` (millis; 0 reads as 1), whose trusted UTC was
 * `scanUtc` (0 = not trusted). `accM` outside 1..SELF_NET_MAX_ACC_M is stored and refused as
 * POOR. Nothing is written when the slot is switched off. */
void selfNetSet(SelfSlot* s, int32_t latI, int32_t lonI, uint16_t accM, uint32_t scanMs,
                uint32_t scanUtc, bool injected);
/* A later scan still heard the fix's devices: it is now the fix's clock. False (nothing changed)
 * when there is no fix or the scan is not later than the last one. */
bool selfNetConfirm(SelfSlot* s, uint32_t scanMs, uint32_t scanUtc);
/* Is the fix still showing, but its last confirming scan SELF_NET_REFRESH_AT_MS old (the
 * auto-switcher's 10-minute scan is late)? The glue then asks for a scan (geoScanRequestOk) while
 * the position is wanted, so a fix that is still true does not expire into a new lookup. */
bool selfNetWantsRefresh(const SelfSlot* s, uint32_t nowMs);
/* Would a scan finished NOW make the NET fix sendable (the owed beacon slot asks for one)? True
 * when there is a fix, the first scan is at least SELF_NET_SETTLE_MS old, and the fix is not
 * already confirmed within SELF_NET_CONFIRM_MS by a scan SELF_NET_SETTLE_MS after the first. */
bool selfNetScanUseful(const SelfSlot* s, uint32_t nowMs);

/* Why a slot is or is not usable for a purpose, checked in this order. */
enum SelfState {
  SELF_ST_LIVE = 0,      // fresh enough and good enough
  SELF_ST_RESERVED,      // not a source at all (an src out of range)
  SELF_ST_OFF,           // switched off
  SELF_ST_NEVER,         // on, no fix since boot (or since cleared)
  SELF_ST_INJECTED,      // a bench fix, and this purpose refuses them (the scheduled beacon)
  SELF_ST_OLD,           // older than the purpose allows
  SELF_ST_POOR,          // fresh, but its own sentence said sats < 4 or HDOP > 10 (NET: accM out of range)
  SELF_ST_UNRATED,       // SENDING only: a phone fix whose sentence gave neither sats nor HDOP
  // SENDING only, NET only (the Wi-Fi slot's rules, above):
  SELF_ST_MAP_ONLY,      // "Wi-Fi location: map only"
  SELF_ST_SETTLING,      // no second agreeing scan SELF_NET_SETTLE_MS after the first yet
  SELF_ST_UNCONFIRMED,   // the latest confirming scan is over SELF_NET_CONFIRM_MS old
};

/* Purpose flags. 0 = show it on this phone's screens. */
#define SELF_PICK_TX        0x01    // the 30 s bar: it is going on the air
#define SELF_PICK_INJECTED  0x02    // a `pos inject` fix may be used (screens; `pos now`)

/* ── UNRATED: SHOWN, NEVER SENT ─────────────────────────────────────────────────────────────
 * A PHONE fix with no satellite count AND no HDOP (the gate's RMC-vouched path) says nothing
 * about how good it is. iOS gives apps no raw NMEA: GPS 2 IP builds its sentences from
 * CoreLocation, whose position can be Wi-Fi or cell based (tens of metres to kilometres) or,
 * with Precise Location off for the app, fuzzed by kilometres on purpose — and it would go out
 * as EXTERNAL at full precision, a sharp dot on every receiver. So it is good enough for this
 * phone's own screens (its owner knows where they are) and never for SELF_PICK_TX. A bench fix
 * is exempt: it is sent as MANUAL (selfPick below), which claims no measurement at all. The
 * woods plate's GPS is not touched by this: a receiver that has not said is not a failure. */
int selfSlotState(const SelfSlot* s, int src, uint32_t nowMs, unsigned flags);

struct SelfFix {
  int      src;          // SELF_* (SELF_NONE: nothing)
  int      state;        // SELF_ST_* of that slot for the purpose asked
  int32_t  latI, lonI;
  uint32_t ageMs;        // NET: since the latest confirming scan
  int      sats, hdopX10;
  bool     injected;
  /* The mesh location_source and precision_bits a beacon from this fix carries: the source's
   * own (GPS INTERNAL, PHONE EXTERNAL, 32 bits; NET UNSET, SELF_NET_PREC_BITS) — except a bench
   * fix (`pos inject`, `phonepos at`, `netloc answer`), which is MANUAL with field 23 left out
   * like the hand-set pin (a bench NET fix keeps its 17 bits: coarse stays coarse).
   * INTERNAL/EXTERNAL is a claim of a MEASUREMENT that stock receivers take the packet's time on
   * (mesh_pos.h), and a typed 40.0,-100.0 must not reach COVEY looking like a live phone-GPS fix. */
  uint8_t  locSrc;
  uint8_t  precBits;
  uint16_t accM;         // NET: the fix's accuracy (the map's ring, "Wi-Fi +/-60 m"); 0 otherwise
  uint32_t scanUtc;      // NET: the latest confirming scan's trusted UTC (0 = unknown); 0 otherwise
};

/* THE ONE RULE every caller asks: the best-ranked source whose slot is LIVE for the purpose.
 * Returns its SELF_* (out filled), or SELF_NONE (out->src = SELF_NONE, the rest zero). No
 * hysteresis: a 1 Hz stream either keeps a source alive or lets it age out, and that is the
 * switch. slots[] is indexed src - 1. */
int selfPick(const SelfSlot slots[SELF_SRC_COUNT], uint32_t nowMs, unsigned flags, SelfFix* out);

/* The most recently stamped slot of any enabled source, whatever its age or quality — the grey
 * "last GPS" / "last phone" marker and the `pos` lines. out->state says LIVE/OLD/POOR for the
 * purpose. SELF_NONE when no enabled source has a fix. A NET fix past SELF_NET_SHOW_MS is not
 * a candidate: an expired Wi-Fi fix is gone, not "last Wi-Fi". */
int selfNewest(const SelfSlot slots[SELF_SRC_COUNT], uint32_t nowMs, unsigned flags, SelfFix* out);

/* One source's own fix, whatever its age or quality (`pos`, the Almanac's "last ..." place, the
 * Wi-Fi row): false when that source is not GPS/PHONE/NET, is off, or has no fix; out->state
 * for `flags`.
 * ⚠ tz_auto.cpp's tzaPickFix asks this for GPS and PHONE only, by its loop's bound: a Wi-Fi fix
 * never chooses the clock's zone (a network guess near a zone line is an hour wrong all day). */
bool selfSource(const SelfSlot slots[SELF_SRC_COUNT], int src, uint32_t nowMs, unsigned flags,
                SelfFix* out);

/* Is any source switched on — for SHOWING (the map's "no fresh fix" note, follow-me, the rows)?
 * NET counts when "Wi-Fi location" is not off. */
bool selfAnyEnabled(const SelfSlot slots[SELF_SRC_COUNT]);
/* ...for SENDING (the Position/Send-to block, the beacon's "nothing to send"): GPS, PHONE, and NET
 * only with "map + mesh" (review item 16) — "map only" never makes the beacon think it has a
 * source. */
bool selfAnyEnabledForTx(const SelfSlot slots[SELF_SRC_COUNT]);

// ---------------------------------------------------------------- labels
const char* selfSrcName(int src);        // "GPS" "phone" "Wi-Fi" "none"
const char* selfSrcLastName(int src);    // "last GPS" "last phone" ... (a fix past its bar)
const char* selfMapLabel(int src);       // "me" "me (phone)" "me (Wi-Fi)" (selfFixLabel adds the size)
const char* selfBeaconTag(int src);      // the log's provenance: "gps beacon" "phone beacon"
const char* selfStateName(int state);    // "live" "off" "no fix yet" "too old" "poor fix" ...
uint8_t     selfSrcLocSource(int src);   // GPS INTERNAL, PHONE EXTERNAL, else UNSET
uint8_t     selfSrcPrecBits(int src);    // 32 for a measured source, SELF_NET_PREC_BITS for NET, else 0
/* "me" "me (phone)" "me +/-60 m Wi-Fi" — the map's label for a pick (the accuracy is part of the
 * NET label: a Wi-Fi fix never appears without its size). "+/-" because the phone's fonts are
 * printable ASCII only. Returns the length (cut to cap - 1). */
size_t      selfFixLabel(char* out, size_t cap, const SelfFix* fx);
/* "GPS" "phone" "Wi-Fi +/-60 m" — a pick's name on the distance lines, the Nodes self row, the
 * Status screen's "From:". */
size_t      selfFixName(char* out, size_t cap, const SelfFix* fx);
/* Field 4 of a beacon from this pick: a NET fix is AS OF ITS LATEST CONFIRMING SCAN, so that
 * scan's UTC (0: leave the field out — the clock was not trusted then); a GNSS fix is current,
 * so the send's own trusted clock `trustedUtcNow` (0 = not trusted). */
uint32_t    selfFixSendTime(const SelfFix* fx, uint32_t trustedUtcNow);

// ---------------------------------------------------------------- sending a Wi-Fi fix
/* ── THE BEACON'S MEMORY FOR THE WI-FI SLOT ─────────────────────────────────────────────────
 * Two rules need what was SENT before, which no slot holds:
 *  - PRECISE FIX WINS: no Wi-Fi beacon while a GPS or phone beacon went out in the last
 *    SELF_NET_PRECISE_WINS_MS within the Wi-Fi fix's accuracy + SELF_NET_PRECISE_SLACK_M of it.
 *    A receiver keeps the LAST position it heard: a coarse cell after a precise dot replaces the
 *    better answer with a worse one about the same place.
 *  - KEEP THE CELL (review item 15): a parked Wi-Fi fix near a cell edge would alternate between
 *    cells 182-364 m apart and beat the 100 m movement gate every slot. So while a new fix lies
 *    within the larger accuracy of it and the fix that produced the last cell sent, the SAME cell
 *    is sent (and the movement gate then sees no movement). */
#define SELF_NET_PRECISE_WINS_MS  3600000UL
#define SELF_NET_PRECISE_SLACK_M       50
struct SelfNetTxMem {
  bool     cellOk;                 // a Wi-Fi cell has gone out
  int32_t  cellLatI, cellLonI;     // ...that cell's centre (what was sent)
  int32_t  srcLatI, srcLonI;       // ...the Wi-Fi fix it was cut from
  uint16_t srcAccM;
  bool     preciseOk;              // a GPS/phone beacon (not a bench one) has gone out
  uint32_t preciseMs;
  int32_t  preciseLatI, preciseLonI;
};
void selfNetTxInit(SelfNetTxMem* m);
void selfNetTxNotePrecise(SelfNetTxMem* m, int32_t latI, int32_t lonI, uint32_t nowMs);
// After a Wi-Fi beacon went out: `fx` the NET pick, (cellLatI, cellLonI) what selfNetTxCell gave.
void selfNetTxNoteSent(SelfNetTxMem* m, const SelfFix* fx, int32_t cellLatI, int32_t cellLonI);
bool selfNetPreciseWins(const SelfNetTxMem* m, const SelfFix* fx, uint32_t nowMs);
// The coordinates to send for a NET pick: the kept cell, or the fix cut to SELF_NET_PREC_BITS.
void selfNetTxCell(const SelfNetTxMem* m, const SelfFix* fx, int32_t* latI, int32_t* lonI);

// ---------------------------------------------------------------- the woods plate's GPS (UART)
/* ── WHICH SENTENCE MAY STAMP THE GPS FIX (0.9.82, the phone-location review of 2026-10-10) ──
 * The plate's receiver (phone 2's M100) sends an RMC and then a GGA every second. Until this,
 * the loop handed the service the reader's STICKY fields (nmea.h) on every RMC or GGA, and the
 * fix was stamped NOW whenever `valid` was true — but `valid` changes only on an RMC, a quality-0
 * GGA leaves the last coordinates standing, and sats/HDOP survive a GGA whose fields are empty.
 * So with the RMCs lost (checksum errors on this garbling UART) or switched off, every no-fix GGA
 * re-stamped the LAST position, with the last good sats and HDOP, as a fix taken now: the map's
 * "me", the Almanac, the beacon and the time zone (tz_auto takes a live GPS fix) saw an old
 * position as live for as long as GGAs kept coming. The phone's stream never could (its gate
 * judges one GGA alone, below). The same rule for the UART, one sentence at a time:
 *   - ONLY A SENTENCE THAT ITSELF CARRIES A MEASURED POSITION STAMPS THE FIX:
 *       an RMC with status A, readable coordinates and a measurement mode (selfRmcModeOk), or
 *       a GGA with quality 1-5 and its own readable coordinates, sats/HDOP not garbled
 *       (0 no fix, 6 dead reckoning, 7 manual and 8 simulator are not measurements; 3 PPS is,
 *       as in the phone gate);
 *     and not when the OTHER sentence of the same second, arriving first, said there was no fix
 *     (the phone gate's veto, both ways: some receivers send the GGA first).
 *   - The position is that sentence's own. A GGA's sats and HDOP are its own (-1 = not said).
 *     An RMC carries none, so it takes the LAST GGA's own words when that GGA arrived within
 *     SELF_GPS_GGA_PAIR_MS (the epoch before, at 1 Hz; one lost GGA and some loop jitter), and
 *     -1 after that — never older GOOD numbers.
 *     ⚠ The last GGA's words WHATEVER ITS QUALITY: at a first fix the GGA before the RMC said
 *     "00 satellites", and that keeps the RMC's fix POOR until its own GGA rates it, tens of
 *     ms later. With -1 there it would be LIVE in between, and an owed beacon slot fires on the
 *     first usable fix — the 2026-08-25 first fix at 3 satellites was 20 km off.
 *     ⚠ AND A BORROWED RATING MAY ONLY MAKE IT WORSE (review 2026-10-11): where what the RMC
 *     would take passes meshPosFixUsable — "not said" included (an empty GGA, or none within
 *     the window) — but the last word on record fails (the last GGA's own, else the fix this
 *     one replaces), the failing one stands. A 3-satellite fix followed by 3 s of lost GGAs, or
 *     by one empty no-fix GGA, went LIVE (and out on the beacon) on the next RMC otherwise. Only
 *     a GGA that itself says better lifts a POOR fix; a good rating still lapses to -1.
 *   - The motion is the stamping RMC's own, the altitude the stamping GGA's own (ggaAltM), each
 *     with its own stamp (review 2026-09-27: a GGA re-stamped the last RMC's speed).
 *   - A sentence that stamps nothing leaves the fix's TIME alone: a receiver that loses its fix
 *     goes OLD on the bars (30 s to send, 120 s to show). One thing it may do: a GGA whose OWN
 *     sats/HDOP fail meshPosFixUsable (the M100's no-fix "00" satellites, HDOP 99.99) marks the
 *     last fix POOR at once, as the plate always has. It can only make a fix look worse, and it
 *     never re-scores a bench fix (`pos inject gps`), which the next stamp replaces.
 * TWO PAIRS OF NUMBERS, never to be swapped: the fix's rating (fixSats/fixHdopX10 = the slot's,
 * getGpsFix - what every gate judges the fix by) and the receiver's own word on its sky (sats/
 * hdopX10, getGpsSky - `gps`, the Almanac's satellite rows): the last GGA's own, -1 where that
 * sentence left a field empty, so after a POOR fix one empty no-fix GGA reads -1/-1 = "usable"
 * there (review 2026-10-11: the Almanac judged the fix by it). The clock's use of the RMC is
 * clock_source.h's and does not pass through here. */
#define SELF_GPS_GGA_PAIR_MS  3000UL

struct SelfGpsRx {
  /* The receiver's word on its sky: the LAST GGA's own sats and HDOP (-1 = empty or unreadable
   * in that sentence), whatever its quality, and when it arrived (millis; 0 = none yet). */
  int      sats, hdopX10;
  uint32_t ggaMs;
  /* The same second's veto: the last RMC's and the last GGA's hhmmss (-1 = none / no time) and
   * whether each said it had a fix (RMC: A with a measurement mode; GGA: quality 1-5 with
   * coordinates). */
  int32_t  rmcHms, ggaHms;
  bool     rmcFix, ggaFix;
  /* THE FIX, written only by a sentence that carried one. RAM only, like the slots. */
  int32_t  latI, lonI;           // 1e-7 deg
  uint32_t fixMs;                // millis() of that sentence; 0 = never
  int      fixSats, fixHdopX10;  // its rating as the GPS slot holds it (-1 = not said)
  int      altM;                 // the last stamping GGA's own altitude, m (-10000 = it had none)
  uint32_t altMs;                // ...that GGA's millis(); 0 = none
  int32_t  speedKnX100, courseX10;   // the last stamping RMC's own motion (-1 = it had none)
  uint32_t motionMs;             // ...that RMC's millis(); 0 = none
};
void selfGpsInit(SelfGpsRx* rx);                 // boot: no fix, nothing heard
#define SELF_GPS_STAMPED  0x01    // the sentence carried a measured position: the fix is now
#define SELF_GPS_FIRST    0x02    // ...and it was the first since boot
/* One completed RMC or GGA from the woods UART (NmeaReader::feed() returned true; f.lastKind
 * says which), at millis() `nowMs`. Updates rx and the GPS slot (NULL: rx only): a stamp writes
 * the whole slot (selfSlotSet, replacing a bench fix); a POOR-sky GGA re-scores a measured
 * slot's sats/HDOP; nothing else touches it. Returns SELF_GPS_* flags. */
unsigned selfGpsApply(SelfGpsRx* rx, SelfSlot* slot, const NmeaFix& f, uint32_t nowMs);

// ---------------------------------------------------------------- the phone stream
/* Which ports, in order: 10110, 11123, then the user's own (NVS phport; 0 = none) unless it
 * repeats one. Returns the count (2 or 3). */
#define SELF_PHONE_PORT_NMEA     10110   // IANA nmea-0183: gpsdRelay and Share GPS, as we tell people
#define SELF_PHONE_PORT_GPS2IP   11123   // GPS 2 IP's default (iPhone)
#define SELF_PHONE_PORTS_MAX     3
int selfPhonePorts(uint16_t custom, uint16_t out[SELF_PHONE_PORTS_MAX]);

#define SELF_PHONE_POLL_MS        100UL    // read the socket this often, not every pass
#define SELF_PHONE_CONNECT_MS    3000UL    // a connect not finished by then counts as refused
#define SELF_PHONE_SETTLE_MS     2000UL    // after every connect, sentences are dropped this long
#define SELF_PHONE_SILENT_MS    10000UL    // no checksum-good sentence this long: the link is dead
#define SELF_PHONE_PEND_MS       2000UL    // how long a GGA without sats/HDOP waits for its RMC
#define SELF_PHONE_TIME_TOL_S       5      // a sentence's UTC second vs a trusted clock

/* ── THE STALE-DATA GUARDS ─────────────────────────────────────────────────────────────────
 * gpsdRelay keeps ONE 30-line queue for all its TCP clients, and its `connected` flag never
 * resets: after a reconnect the first thing that arrives is up to 30 lines of the PAST, which
 * a stamp-on-arrival would call a fresh fix. Two guards, both cheap:
 *   1. SETTLE — everything in the first 2 s of a connection is dropped (the backlog is written
 *      the moment the socket is accepted).
 *   2. TIME OF DAY — while the clock is TRUSTED (NTP/GPS, clock_source.h), a sentence that
 *      could write or confirm a fix must carry a UTC time within 5 s of now, compared across
 *      midnight (23:59:58 vs 00:00:01 is 3 s, not 86,397). One with no readable time is
 *      dropped: it cannot be shown to be current. With an untrusted clock only the settle
 *      applies — a mesh-set clock is not good enough to judge someone else's GPS by. */
int32_t selfHmsToSod(int32_t hms);                 // hhmmss -> second of day; -1 absent/out of range
int32_t selfSodDiff(int32_t a, int32_t b);         // across midnight: 0..43200
bool    selfPhoneSettled(uint32_t connMs, uint32_t nowMs);
// utcSod < 0 = the clock is not trusted: true. Otherwise the sentence's own hhmmss must be
// present, in range, and within SELF_PHONE_TIME_TOL_S.
bool    selfPhoneTimeOk(int32_t hms, int32_t utcSod);

/* An RMC's NMEA 2.3 mode: E (estimated/dead reckoning), N (not valid), M (manual input) and
 * S (simulator) are not measurements; absent (0, pre-2.3) and A/D/F/P/R are. */
bool    selfRmcModeOk(char mode);

/* ── THE PHONE'S QUALITY GATE (one GGA, nothing inherited) ─────────────────────────────────
 * Position, fix quality, sats and HDOP are all taken from ONE GGA (nmea.h's gga* fields):
 *   quality 0 / empty / no coordinates   NOFIX         nothing written
 *   quality 6+ (6 dead reckoning,        NOT_MEASURED  nothing written: an estimate, not a fix
 *     7 manual input, 8 simulator)
 *   sats or HDOP present but unreadable  GARBLED       nothing written
 *   the same second's RMC said V, or a   NOFIX         nothing written (only when that RMC came
 *     mode E/N/M/S                                       FIRST; one that follows cannot retract)
 *   sats present and < 4, or HDOP        POOR          written WITH those numbers: the slot is
 *     present and > 10.0                                 then refused for sending, and drawn grey
 *   neither sats nor HDOP present        FIX only when an RMC with status A and a measurement
 *     (GPS 2 IP can send them empty)       mode carries the same hhmmss — before the GGA, or
 *                                          after it within SELF_PHONE_PEND_MS (WAIT meanwhile);
 *                                          such a fix is shown, never sent (SELF_ST_UNRATED)
 *   otherwise (quality 1 GPS, 2 DGPS,    FIX
 *     3 PPS, 4/5 RTK fixed/float)
 * The trusted-clock time check (above) and the settle window come first for anything that
 * could write. HDOP is compared in tenths, truncated — "10.05" passes as 10.0, exactly as the
 * woods plate's own HDOP always has (nmea.cpp).
 * gpsdRelay's GENERATED GGA (mock locations) puts the accuracy in METRES in the HDOP field and
 * always says 12 satellites: that passes when the accuracy is 10 m or better, which is the
 * right answer for a different reason. Its Double.toString can print "5.0E-4", which the
 * strict readers refuse (GARBLED, or a coordinate that does not parse) rather than misread. */
enum SelfPhoneVerdict {
  SELF_PV_NONE = 0,      // nothing judged yet
  SELF_PV_FIX,
  SELF_PV_POOR,
  SELF_PV_NOFIX,
  SELF_PV_NOT_MEASURED,
  SELF_PV_GARBLED,
  SELF_PV_WAIT,
  SELF_PV_STALE,
  /* Set by selfPhoneKeep(), after the gate: the gate let a fix through but it was not kept. */
  SELF_PV_CHECKING,      // a connection's first fix, waiting for the next to agree with it
  SELF_PV_JUMPED,        // it did not agree with the fix before it (self_pos.h, two in a row)
};
const char* selfPhoneVerdictName(int v);

struct SelfPhoneGate {
  uint32_t seenRmc, seenGga;   // the reader's rmcCount/ggaCount already judged
  int32_t  rmcHms;             // the last current RMC's hhmmss (-1 none)
  bool     rmcOk;              // ...it said A with a measurement mode
  bool     pend;               // a GGA without sats/HDOP waiting for its RMC
  int32_t  pendHms, pendLatI, pendLonI;
  uint32_t pendMs;
};

/* A position the gate let through, for the glue to write into the PHONE slot. */
struct SelfPhoneOut {
  int      verdict;            // SELF_PV_FIX or SELF_PV_POOR
  int32_t  latI, lonI;
  int      sats, hdopX10;      // from the SAME sentence; -1 = it did not say
};

/* One connection's reader, gate and counters. ~0.3 KB; the glue keeps one in BSS. The reader
 * is NOT gGpsReader: the woods UART's reader never sees a byte from the network. */
struct SelfPhoneStream {
  NmeaReader    reader;
  SelfPhoneGate gate;
  uint32_t connMs;             // millis() this connection began (the settle window)
  uint32_t lastSentenceMs;     // last checksum-good sentence (or connMs): the silence watchdog
  uint32_t rmcSeen, ggaSeen;   // this connection
  /* The last GGA judged (SELF_PV_*), this connection — and after selfPhoneKeep(), whether that
   * fix was KEPT: the screen's "fix" means a fix is in the PHONE slot, not just that the gate
   * liked a sentence (CHECKING / JUMPED say why one was not). */
  int      lastVerdict;
  int      lastSats, lastHdopX10;   // ...and its numbers, when it wrote a fix (-1 = not said)
  /* The last fix the gate let through on THIS connection, kept or not: the next must agree
   * with it (selfPhoneAgrees). Reset by selfPhoneBegin — a new connection's first fix waits. */
  bool     candOk;
  int32_t  candLatI, candLonI;
  uint32_t candMs;
  // since boot (the `phonepos` line):
  uint32_t sentBase, badBase;  // the reader's counts folded in at each new connection
  uint32_t nFix, nPoor, nNoFix, nNotMeasured, nGarbled, nStale, nDisagree;
  /* GPS 2 IP's stand-in numbers (selfPhoneSetPort, below): the port this connection is on, how
   * many GGAs running carried exactly 8 sats / HDOP 0.9, and whether this connection's numbers
   * are known to be stand-ins (the status line says so). */
  uint16_t port;
  uint8_t  placeholderRun;
  bool     placeholder;
};

void     selfPhoneInit(SelfPhoneStream* s);                  // boot: everything zero
void     selfPhoneBegin(SelfPhoneStream* s, uint32_t nowMs); // a new connection
/* ── GPS 2 IP's STAND-IN NUMBERS ────────────────────────────────────────────────────────────
 * iOS gives apps neither a satellite count nor a DOP, so GPS 2 IP writes every GGA with 8
 * satellites and HDOP 0.9 (its own NMEA docs), whatever the fix is really worth - an indoor
 * Wi-Fi position included. Taken at face value that is an excellent fix and it would go out as
 * a sharp dot. So the pair is read as "not said" (UNRATED: shown on this phone, never sent):
 * at once on GPS 2 IP's default port, and after five GGAs running with exactly that pair on
 * any other port (a real receiver's HDOP moves; one that sits on 8 / 0.9 for five seconds
 * only waits a while to be sent). The glue tells the stream its port after selfPhoneBegin. */
#define SELF_PHONE_PLACEHOLDER_SATS     8
#define SELF_PHONE_PLACEHOLDER_HDOPX10  9
#define SELF_PHONE_PLACEHOLDER_RUN      5
void     selfPhoneSetPort(SelfPhoneStream* s, uint16_t port);
/* Feed one chunk read from the socket. `utcSod` = the trusted UTC second of the day, or -1 when
 * the clock is not trusted. True when a GGA in the chunk wrote a fix (FIX or POOR): *out is the
 * LAST such, which is what the per-sentence loop would have left in the slot. */
bool     selfPhoneFeed(SelfPhoneStream* s, const uint8_t* buf, size_t n, uint32_t nowMs,
                       int32_t utcSod, SelfPhoneOut* out);
uint32_t selfPhoneSentences(const SelfPhoneStream* s);       // checksum-good, since boot
uint32_t selfPhoneBadChecksums(const SelfPhoneStream* s);    // since boot
bool     selfPhoneSilent(const SelfPhoneStream* s, uint32_t nowMs);

/* ── TWO FIXES IN A ROW MUST AGREE (the glue's last check, phone_pos.cpp) ──────────────────
 * One GGA can be well-formed, checksum-good, quality 1 with 12 satellites — and ~11 km wrong.
 * gpsdRelay's GENERATED sentences build each coordinate with ddToDDMM: Kotlin's
 * Double.toString of the minutes, split at the '.', the fraction cut to 6 characters. Minutes
 * under 0.001 print in E notation, so 40 deg 0.00006 min becomes "5.999999998E-5" and then
 * "4005.999999" — 40 deg 6 min, a coordinate the strict reader has no way to refuse (one with
 * fewer digits keeps its 'E' and IS refused). It happens within ~1.85 m of every whole minute
 * of latitude or longitude, and the error is 1 to 9 minutes (1.9-17 km). The receiver's own
 * relayed NMEA does not do this; generation is the default in gpsdRelay's add-server dialog.
 * So a phone fix is kept only when the GGA before it on the same connection was at most
 * SELF_PHONE_AGREE_GAP_MS earlier and within SLACK + MAX_SPEED x dt of it: a glitch costs
 * that fix and the one after it, and the first fix of a connection waits one epoch. Moving
 * through such a strip is caught; standing still inside one, with the same leading digit every
 * second, is not — the setup instructions say to relay the receiver's own NMEA.
 * ⚠ THE GAP IS THE SILENCE WATCHDOG'S (review): at 5 s, an app sending every 5-10 s (gpsdRelay
 * with its generation interval raised, a slower GPS 2 IP setting) stayed connected — a sentence
 * inside every 10 s — and never produced one kept fix, while the screen said "fix". The bound
 * still grows with dt: at a 1 Hz stream it is 350 m, so the km-scale glitch is caught there. */
#define SELF_PHONE_AGREE_GAP_MS     SELF_PHONE_SILENT_MS
#define SELF_PHONE_AGREE_SLACK_M      50.0
#define SELF_PHONE_MAX_SPEED_MPS     300.0
bool selfPhoneAgrees(int32_t prevLatI, int32_t prevLonI, uint32_t prevMs,
                     int32_t latI, int32_t lonI, uint32_t nowMs);
/* The glue's question for each fix selfPhoneFeed() let through: keep it (write the PHONE slot)?
 * True when it agrees with the connection's previous candidate; either way it becomes the next
 * candidate. When it is not kept, lastVerdict says why (CHECKING for a connection's first fix,
 * JUMPED for one that disagreed, counted in nDisagree), so the screen never says "fix" over an
 * empty slot. */
bool selfPhoneKeep(SelfPhoneStream* s, const SelfPhoneOut* out, uint32_t nowMs);

/* ── THE '\n' BEFORE EVERY '$' ──────────────────────────────────────────────────────────────
 * NmeaReader completes a sentence only on CR or LF, and a '$' throws the partial line away
 * (nmea.cpp). Android does not document whether the strings its NMEA listener hands an app end
 * in CRLF, and gpsdRelay relays them as they are — so a stream with no line endings would be
 * discarded whole, sentence by sentence, in silence. A '\n' fed before each '$' completes the
 * sentence in hand; outside a sentence a '\n' is ignored, so a stream that has CRLF loses
 * nothing. True when an RMC or GGA completed (reader->fix() holds it). At most one completes per
 * call: the '\n' can finish one, the '$' only starts the next. A line cut short now counts as
 * a bad checksum instead of vanishing — visible corruption, which is better. ⚠ The reader's
 * bytes() counts the '\n's added here: the glue counts what the socket delivered itself. */
bool selfNmeaStreamFeed(NmeaReader* r, char c);

// ---------------------------------------------------------------- finding the app
/* ── WHICH PORT, AND HOW OFTEN TO ASK ────────────────────────────────────────────────────────
 * Zero configuration: on ANY station network the client asks the DHCP gateway (on a hotspot,
 * the phone itself) on each port in turn. The first port that delivers a checksum-good sentence
 * is remembered for that association (assoc = lastWifiLinkUpMs(), the GOT_IP stamp), so no
 * SSID is stored and nothing is typed. A gateway that answers on no port backs off:
 *   failures in a row (rounds, drops)   1: 2 s  2: 5 s  3: 10 s  4: 30 s  5+: 60 s
 *   3 full rounds with no answer        5 min from then on — a home router is asked 3 times
 *                                       in ~10 s, then once every 5 minutes (a round of 2-3
 *                                       non-blocking connects, each refused in a few ms or
 *                                       given up at SELF_PHONE_CONNECT_MS).
 * A good sentence resets the failures; a new association resets everything. The remembered
 * port failing to connect SELF_PHONE_CACHE_FAILS times in a row is forgotten (the app may have
 * changed, gpsdRelay -> GPS 2 IP), and the rounds start again. */
#define SELF_PHONE_DEAD_ROUNDS      3
#define SELF_PHONE_DEAD_WAIT_MS   300000UL
/* 2026-10-11, the first real test: the WiPhone joined the owner's hotspot before gpsdRelay's
 * server was switched on, three rounds were refused in ~17 s, and the next ask was 5 min away.
 * A home router never grows an app; a phone hotspot does, a minute later. */
#define SELF_PHONE_HOTSPOT_WAIT_MS 30000UL
#define SELF_PHONE_CACHE_FAILS      3

struct SelfProbe {
  uint32_t assoc;              // the association this belongs to (0 = none yet)
  uint16_t port;               // the port that delivered on it; 0 = none found yet
  uint8_t  idx;                // next port in the round (while port == 0)
  uint8_t  fails;              // failures in a row since the last good sentence
  uint8_t  deadRounds;         // rounds on this association with no port answering
  uint8_t  portFails;          // the remembered port's connect failures in a row
  bool     wait;               // nextMs is armed
  uint32_t nextMs;             // no attempt before this millis()
  /* The joined network looks like a phone's hotspot (its BSSID is locally administered, the
   * shape Android and iOS hotspots use): its app may simply not be started YET, so a dead round
   * is asked again every SELF_PHONE_HOTSPOT_WAIT_MS, never SELF_PHONE_DEAD_WAIT_MS. Set by the
   * glue after every selfProbeStart (selfProbeSetHotspot). */
  bool     hotspot;
};
uint32_t selfProbeDelayMs(int fails, int deadRounds);
void     selfProbeStart(SelfProbe* p, uint32_t assoc);       // a new association: ask now
void     selfProbeSetHotspot(SelfProbe* p, bool hotspot);   // see SelfProbe.hotspot
bool     selfProbeDue(const SelfProbe* p, uint32_t nowMs);
uint16_t selfProbePort(const SelfProbe* p, uint16_t custom); // the port to try now
// A connect refused or timed out, or a stream that ended before any good sentence.
void     selfProbeFailed(SelfProbe* p, uint16_t custom, uint32_t nowMs);
// A checksum-good sentence arrived on `port`: remember it, forget the failures.
void     selfProbeGood(SelfProbe* p, uint16_t port);
// A stream that HAD delivered ended (closed, or silent SELF_PHONE_SILENT_MS).
void     selfProbeDropped(SelfProbe* p, uint32_t nowMs);
// "no app found on this network": a whole round failed and nothing has delivered since.
bool     selfProbeNoApp(const SelfProbe* p);

/* Should the client run at all? On ANY station network that is up — never on this phone's own
 * softAP (the uploader / a KOSync window) and never with the radio switched off. */
bool selfPhoneMayRun(bool autoOn, bool staConnected, bool ownAp, bool radioOff);

/* `phonepos at <ip>[:port]` (bench, RAM only): a dotted quad and an optional port 1..65535.
 * ip[] in written order; *port 0 when absent (the usual ports are probed at that address). */
bool selfPhoneParseAt(const char* s, uint8_t ip[4], uint16_t* port);

// ---------------------------------------------------------------- the screen's words
enum SelfPhoneLink {
  SELF_LINK_OFF = 0,           // "Phone location: off"
  SELF_LINK_NO_WIFI,           // not on a station network (or on our own hotspot, radio off)
  SELF_LINK_LOOKING,           // asking the gateway's ports
  SELF_LINK_NO_APP,            // selfProbeNoApp()
  SELF_LINK_STREAM,            // connected; the stream's own state says the rest
};
/* The small line under "Phone location" (Meshtastic > My node). Plain words, no coordinates,
 * no SSID, no address. Returns the length written (truncated to cap - 1). */
size_t selfPhoneStatusText(char* out, size_t cap, int link, const SelfPhoneStream* s,
                           uint32_t nowMs);

#endif // SELF_POS_H
