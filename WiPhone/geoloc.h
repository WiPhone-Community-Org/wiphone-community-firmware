/*
 * geoloc.h — WI-FI LOCATION (0.9.82): where this phone is from the access points it can hear,
 * asked of beaconDB, and believed only when two independent lookups agree. The pure half: the
 * scan snapshot, the filters, the request bodies, the answer's parser, the two-half agreement,
 * and every rule about WHEN to ask and how long an answer stays true. Host-tested by
 * tests/test_geoloc.cpp; the scan copy (Networks.cpp), the HTTPS job (https_worker.cpp) and the
 * screens are the glue and only ask these functions.
 *
 * 🛑 THE OWNER'S RULE ({user}, 2026-10-10): own GPS > the phone's GPS over its hotspot > a Wi-Fi
 * location ONLY when it is genuinely local and accurate > NOTHING. A wrong location is worse
 * than none. And NEVER an internet-address guess: on a phone hotspot it lands at the carrier's
 * internet exit, 180-200 km away (arXiv 2605.21937), looking exactly as sure as a right one.
 * The request below turns beaconDB's own IP fallback off twice ("considerIp":false and
 * "fallbacks":{"ipf":false}) and the parser refuses any answer that carries a "fallback" key.
 *
 *   POST https://api.beacondb.net/v1/geolocate      (the MLS / Ichnaea geolocate API)
 *   Content-Type: application/json                  (actix-web answers 400 without it)
 *   {"considerIp":false,"fallbacks":{"ipf":false,"lacf":false},
 *    "wifiAccessPoints":[{"macAddress":"aa:bb:..","signalStrength":-60},..]}
 *   -> 200 {"location":{"lat":..,"lng":..},"accuracy":<m>}     404 {"error":{..}} = no estimate
 *
 * Only MAC and signal go out: never an SSID, never anything else about the phone, and nothing is
 * ever submitted back. Always the signal too — a missing one weighs as 0 dBm, the loudest.
 *
 * ── WHY TWO HALVES (the adversarial review, 2026-10-10, item 1) ───────────────────────────────
 * beaconDB's answer is a plain signal-weighted average (10^(rssi/30)) of whichever access points
 * it knows (two are enough), and its accuracy is a weighted average of how far each of them is
 * USUALLY heard — not whether they agree. One access point that has moved (a household that
 * moved, a travel router, someone's phone hotspot with a universal MAC) drags the answer while
 * the accuracy still reads ~50 m: at -60/-70/-80 dBm the weakest carries ~13 % of the weight, so
 * a 50 km move puts the answer ~6.5 km off. The reply does not say how many of ours it matched,
 * so there is nothing in one answer to cross-check. Hence: the usable access points are dealt
 * into two halves by signal rank, each half is looked up on its own, and the answer counts only
 * when BOTH are HTTP 200 with no fallback, each claims 1-100 m, and they lie within the larger of
 * the two of each other. Then the midpoint, at max(50 m, accA, accB, the distance between them).
 * One router broadcasts several BSSIDs (2.4/5 GHz, guest) from neighbouring addresses and
 * beaconDB counts BSSIDs, so the halves are dealt by DEVICE (geoSameDevice): a moved router
 * cannot answer for both halves by itself. The X4 Pro e-reader runs the same protocol
 * (crosspoint-reader GeolocateProtocol, bench-proven); this is its pure logic on json_read.h.
 *
 * ── WHY IT OFTEN SAYS "NOT ENOUGH KNOWN WI-FI" ─────────────────────────────────────────────────
 * beaconDB knows only access points volunteers have walked past with its app. A home, a cabin
 * or a campground has often never been seen, and a hotspot that travels is filtered on purpose.
 * Four separate devices, each half answered, is a high bar by design: it says nothing far more
 * often than it says something wrong.
 *
 * ⚠ Free of every Arduino and ESP-IDF header (the host suite links it). No logging here; the
 * glue's log lines carry verdicts and counts, never a MAC or a coordinate past 2 decimals.
 */
#ifndef GEOLOC_H
#define GEOLOC_H

#include <stddef.h>
#include <stdint.h>
#include "self_pos.h"
#include "isrg_roots.h"

// ---------------------------------------------------------------- the service
#define GEO_URL            "https://api.beacondb.net/v1/geolocate"
#define GEO_HOST           "api.beacondb.net"
/* The pinned roots (isrg_roots.h has the why and the second-sourcing): ISRG Root X1, X2 and the
 * self-signed Root YE, so the chain verifies whether beaconDB sends both cross certificates, only
 * the YE one, or none (review item 6). No override file: new roots are a firmware update. */
#define GEO_ROOTS_PEM      ISRG_ROOT_X1_PEM ISRG_ROOT_X2_PEM ISRG_ROOT_YE_PEM
#define GEO_CONTENT_TYPE   "application/json"
/* Generic on purpose: no version, no contact, nothing that tells one WiPhone from another.
 * beaconDB keeps nginx logs (address + User-Agent) for 28 days; request bodies are not kept. */
#define GEO_USER_AGENT     "WiPhone-NH"
#define GEO_RESPONSE_CAP   4096     // the largest answer read (the shared HTTPS sink's size)
/* Twenty access points at their longest ({"macAddress":"..","signalStrength":-128}: 56 bytes,
 * and 19 commas), the envelope (79), "]}" and the NUL: 1,221 (test_geoloc builds it). */
#define GEO_REQUEST_CAP    1280

// ---------------------------------------------------------------- the setting
/* "Wi-Fi location: off | map only | map + mesh" (NVS, OFF by default: turning it on sends the
 * IDs of nearby access points to a third party). MAP: shown on this phone (the map's ring, the
 * Almanac, distances), never sent. MESH: also beaconed, coarsely, under every rule below.
 * 🛑 THE CABLE MAY CHOOSE ONLY OFF OR MAP (the consent rule `pos chan` already follows): putting
 * a position on other people's maps is chosen on the phone's own screen, by a person - and it
 * takes effect only when that screen is left (geoloc_net.h, geolocScreenLeft): the row's cycle
 * passes through MESH on the way from MAP to OFF. */
enum GeoSetting { GEO_SET_OFF = 0, GEO_SET_MAP = 1, GEO_SET_MESH = 2 };
const char* geoSettingName(int v);        // "off" "map only" "map + mesh"
int  geoSettingParse(const char* s);      // "off" "map" "mesh" -> GEO_SET_*; -1 = not one
bool geoSettingSerialOk(int v);           // off/map: yes; mesh: never
int  geoSettingNext(int v);               // the row's tap: off -> map only -> map + mesh -> off
int  geoSettingSane(int v);               // an NVS value out of range reads as OFF

// ---------------------------------------------------------------- the scan snapshot
/* Copied from the auto-switcher's async scan (Networks.cpp) BEFORE WiFi.scanDelete(), from
 * WiFi.getScanInfoByIndex() (no Strings). The SSID is read once, for the two flags below, and
 * never kept: 8 bytes an access point. The strongest GEO_SCAN_MAX are kept. */
#define GEO_SCAN_MAX       48
#define GEO_AP_HIDDEN      0x01     // an empty (hidden) SSID
#define GEO_AP_NOMAP       0x02     // "_nomap" / "_optout" at the end, any case: the owner said no
#define GEO_AP_ODD         0x04     // an SSID with no NUL inside its buffer: refused, never read past
struct GeoAp {
  uint8_t mac[6];
  int8_t  rssi;                     // dBm
  uint8_t flags;                    // GEO_AP_*
};
struct GeoScan {
  uint32_t ms;                      // millis() the scan finished (0 = no snapshot)
  uint32_t utc;                     // the TRUSTED clock then, unix s (0 = not trusted)
  uint32_t assoc;                   // the association it was taken on (lastWifiLinkUpMs)
  uint16_t seen;                    // how many the scan returned (status only)
  uint8_t  n;
  GeoAp    ap[GEO_SCAN_MAX];
};
// `ssid` is wifi_ap_record_t.ssid (33 bytes). NULL or cap 0 reads as hidden.
uint8_t geoSsidFlags(const uint8_t* ssid, size_t cap);
void    geoScanBegin(GeoScan* s, uint32_t nowMs, uint32_t utc, uint32_t assoc);
// Keeps the strongest GEO_SCAN_MAX: a weaker one is dropped when the snapshot is full.
void    geoScanAdd(GeoScan* s, const uint8_t mac[6], int rssi, uint8_t flags);
/* A snapshot a lookup may use: taken on THIS association, under GEO_SCAN_FRESH_MS ago. */
#define GEO_SCAN_FRESH_MS  120000UL
bool    geoScanFresh(const GeoScan* s, uint32_t assoc, uint32_t nowMs);

// ---------------------------------------------------------------- the filters
/* May this access point be sent? Not hidden, not opted out, a globally administered unicast
 * BSSID (no locally administered bit — randomised and phone-hotspot addresses travel with their
 * owner — no group bit, not all zeros), and not a VRRP virtual router (00:00:5e:00:01:xx IPv4,
 * 00:00:5e:00:02:xx IPv6: the same address on routers everywhere). IANA's documentation range
 * 00:00:5e:00:53:xx stays usable: the tests are written in it. */
bool geoApUsable(const GeoAp* ap);
/* One device's BSSIDs: the same OUI and 24-bit tails less than GEO_SAME_DEVICE_SPAN apart.
 * Multi-BSSID routers number their radios and networks from one base (a block of 16 a radio is
 * common). */
#define GEO_SAME_DEVICE_SPAN  256
bool geoSameDevice(const uint8_t a[6], const uint8_t b[6]);
/* Is the JOINED access point left out of the lookup (passed as `exclude`, with its whole
 * device)? When its gateway served phone NMEA (it is a phone's hotspot, and travels with it),
 * and whenever its BSSID is locally administered (a hotspot's usual shape). NULL: not joined. */
bool geoJoinedLeftOut(const uint8_t bssid[6], bool gatewayServedNmea);

// ---------------------------------------------------------------- the two halves
#define GEO_MAX_REQUEST_APS   20     // per half
#define GEO_RANKED_MAX        (2 * GEO_MAX_REQUEST_APS)
#define GEO_MIN_DEVICES        4     // two per half
/* Weaker access points are not used: they may be far away or barely heard, and they carry almost
 * no weight anyway. -85 dBm is roughly 50-150 m through walls. */
#define GEO_MIN_RSSI         (-85)
#define GEO_HALF_A             0
#define GEO_HALF_B             1
#define GEO_HALF_UNSENT     0xFF     // ranked, but over GEO_MAX_REQUEST_APS in its half

struct GeoQAp {
  uint8_t mac[6];
  int8_t  rssi;
  uint8_t half;                      // GEO_HALF_*
};
/* One lookup's question, self-contained: the ranked access points themselves (not indexes into
 * a snapshot a newer scan may overwrite while half A is out), and the scan they came from. */
struct GeoSplit {
  uint8_t  usable;                   // usable access points at GEO_MIN_RSSI or stronger (<= 40)
  uint8_t  devices;                  // ...and the devices they belong to
  uint8_t  halfA, halfB;             // how many go into each body (<= 20 each)
  bool     built;                    // devices >= GEO_MIN_DEVICES: worth asking
  uint32_t scanMs, scanUtc, scanAssoc;
  uint8_t  n;                        // == usable
  GeoQAp   q[GEO_RANKED_MAX];        // strongest first
};
/* The usable access points at GEO_MIN_RSSI or stronger, a BSSID seen twice once at its stronger
 * reading, every address of the `exclude` device left out (NULL: none), the strongest
 * GEO_RANKED_MAX; grouped into devices (geoSameDevice, through shared neighbours too), the
 * devices ranked by their strongest member and dealt alternately into A (ranks 1, 3, 5 ..) and
 * B (2, 4, 6 ..), every address of a device in its device's half. */
void   geoSplitAps(const GeoAp* aps, size_t n, const uint8_t* exclude, GeoSplit* out);
void   geoSplitScan(const GeoScan* scan, const uint8_t* exclude, GeoSplit* out);   // + the scan's stamps
/* One half's request body, strongest first, up to GEO_MAX_REQUEST_APS. Returns how many access
 * points it holds; 0 (and out = "") when the split is not built, the half is empty or the body
 * does not fit `cap`. A positive signal (a driver quirk) is sent as 0. */
size_t geoBuildHalf(const GeoSplit* sp, int half, char* out, size_t cap);

// ---------------------------------------------------------------- the answer
/* beaconDB's answer: ONE JSON object and nothing around it, with "location":{"lat","lng"} numbers
 * inside +-90/+-180 and not (0,0), and a top-level "accuracy" number above 0 and under 2e7 m
 * (rounded UP: never claim better than the service did). Refused: an "error" or a "fallback" key
 * (an IP or cell estimate, not Wi-Fi), a key twice, a missing or non-number field, a value with
 * no key, a key with no value, text before or after the object, a body cut short. Unknown keys
 * are skipped. ⚠ Like the e-reader's tokenizer, a missing comma between two members is not
 * caught (json_read.h does not check commas). */
bool geoParseAnswer(const char* body, size_t len, double* lat, double* lon, uint32_t* accM);

enum GeoReply {
  GEO_R_NOT_ASKED = 0,               // skipped (the first half already failed, or never started)
  GEO_R_NO_MEMORY,                   // not started: too little internal RAM for a TLS session
  GEO_R_NO_ANSWER,                   // DNS, connect, TLS or a timeout: beaconDB never heard from
  GEO_R_ANSWERED,                    // a complete HTTP response, whatever it said
};
struct GeoHalf {
  uint8_t  reply;                    // GEO_R_*
  int      status;                   // HTTP status
  bool     ok;                       // the body parsed (only an ANSWERED 200 is parsed)
  double   lat, lon;
  uint32_t accM;
};
void geoHalfFrom(GeoHalf* h, int reply, int status, const char* body, size_t len);

double geoDistanceM(double lat1, double lon1, double lat2, double lon2);   // haversine, mean radius

#define GEO_MAX_HALF_ACC_M      100   // each half at least this tight...
#define GEO_MIN_REPORTED_ACC_M   50   // ...and never claimed tighter than this (Android's "precise")

enum GeoVerdict {
  GEO_V_NONE = 0,                    // no look yet
  GEO_V_LOCATED,                     // both halves agree
  GEO_V_TOO_FEW,                     // usable access points from fewer than GEO_MIN_DEVICES devices
  GEO_V_NO_MEMORY,                   // a request could not start for want of RAM
  GEO_V_UNREACHABLE,                 // a half was never heard from
  GEO_V_NOT_FOUND,                   // a half got no location (not 200, a 404, a fallback)
  GEO_V_VAGUE,                       // a half's accuracy is over GEO_MAX_HALF_ACC_M
  GEO_V_DISAGREE,                    // the halves lie further apart than the larger accuracy
  // The fold's own two (geoFoldEx): the halves agreed, and the answer is still refused -
  GEO_V_JUMP,                        // ...too far from this boot's last GPS/phone fix (geoJumpOk)
  GEO_V_MOVED,                       // ...a scan that landed while the look was out no longer hears its devices
};
#define GEO_V_LAST  GEO_V_MOVED
const char* geoVerdictName(int v);   // "located" "too-few-aps" ... (log tokens, no spaces)

struct GeoFix {
  int32_t  latI, lonI;               // 1e-7 deg: the midpoint
  uint16_t accM;                     // max(50, accA, accB, apart): <= 100 by construction
  uint32_t apartM;                   // the two halves' distance (0 when not compared)
};
/* Is the second half worth asking? Only when the first is an answered 200 location within
 * GEO_MAX_HALF_ACC_M: otherwise the verdict is already the first half's failure. */
bool geoSecondHalfNeeded(const GeoHalf* first);
/* The two-half check. The first half's failure decides when it fails (the second need not have
 * been asked). LOCATED: *out = the midpoint (across the date line too). out may be NULL. */
int  geoJudge(const GeoSplit* sp, const GeoHalf* a, const GeoHalf* b, GeoFix* out);

/* ── THE JUMP BACKSTOP (the review's item 1: "keep the 300 m/s jump check as a backstop") ─────
 * Two halves cannot catch access points that MOVED TOGETHER: a household's router, extender and
 * travel router mapped at the old home and carried to a cabin agree with each other perfectly,
 * at ~50 m, 300 km from where the phone is. Nothing in beaconDB's answers can tell; this phone's
 * own measurements can. So a Wi-Fi answer is refused (GEO_V_JUMP) when reaching it from the
 * newest GPS or phone fix of this boot, within GEO_JUMP_WINDOW_MS, would take more than
 * GEO_JUMP_MAX_MPS - beyond the Wi-Fi fix's own accuracy and GEO_JUMP_SLACK_M (the selfPhoneAgrees
 * numbers: 300 m/s is a jet, nothing this phone rides in). Refusing is the safe direction: a
 * wrong anchor costs a Wi-Fi fix, never adds a wrong one. Only fixes that passed their source's
 * own quality gate count (meshPosFixUsable), and a bench fix only for a bench fold. */
#define GEO_JUMP_MAX_MPS      300.0
#define GEO_JUMP_SLACK_M       50.0
#define GEO_JUMP_WINDOW_MS  21600000UL   // 6 h: after that the phone may have gone anywhere
struct GeoAnchor {
  bool     ok;                       // a fix to measure from
  int32_t  latI, lonI;
  uint32_t ms;                       // millis() of that fix
};
/* The newest GPS/PHONE slot with a fix under GEO_JUMP_WINDOW_MS old that passed its own quality
 * gate (sats/HDOP usable). `allowInjected`: bench fixes count too (only for `netloc answer`, so
 * the bench can exercise this with `pos inject`). out->ok false when there is none. */
void geoAnchorFrom(const SelfSlot slots[SELF_SRC_COUNT], uint32_t nowMs, bool allowInjected,
                   GeoAnchor* out);
/* Could the phone have got from the anchor to (latI, lonI) +/- accM by fixMs (the scan the Wi-Fi
 * fix was computed from)? True with no anchor. Either order in time (a GPS fix that arrived while
 * the look was out counts too). */
bool geoJumpOk(const GeoAnchor* a, int32_t latI, int32_t lonI, uint16_t accM, uint32_t fixMs);

// ---------------------------------------------------------------- when to look, and for how long
/* ── THE SET A FIX STANDS ON ────────────────────────────────────────────────────────────────
 * A Wi-Fi fix is true only while this phone still hears the access points it was computed from:
 * it is not a measurement of where the phone IS, but of where those networks are. So the devices
 * a look asked about are kept (RAM only, never logged), and every later scan is held against
 * them: a device counts as still here at GEO_PRESENT_RSSI or stronger — 5 dB below the bar it
 * had to clear to be asked about, so a reading that wobbles across -85 does not throw a fix away
 * — and at least 2/3 of the devices must be (review item 2). Fewer: the phone has moved, and the
 * fix is dropped AT ONCE, before any new lookup. */
#define GEO_PRESENT_RSSI     (-90)
struct GeoSet {
  uint8_t n;                         // addresses
  uint8_t devices;                   // ...grouped by geoSameDevice
  uint8_t mac[GEO_RANKED_MAX][6];
};
void geoSetFromSplit(GeoSet* set, const GeoSplit* sp);
// How many of the set's devices the scan still hears at GEO_PRESENT_RSSI; *devices = the set's.
int  geoSetHeard(const GeoSet* set, const GeoScan* scan, int* devices);
// At least 2/3 of the set's devices still heard (true for an empty set: nothing to lose).
bool geoSetStillHere(const GeoSet* set, const GeoScan* scan);

/* ── WHEN A LOOK RUNS (all must hold; geoLookDue checks them in this order) ─────────────────
 *   the setting is not off; no lookup is out; WiFi is up (someone else's network, not this
 *   phone's own hotspot); no live GPS or phone fix (a measurement outranks this, and asking would
 *   only spend a third party's goodwill); the position is WANTED: Maps or the Almanac on screen in
 *   the last GEO_WANT_MS, a beacon armed with "map + mesh", or `netloc now`; the weather's gates
 *   (no map download, uploader, KOSync window, call, game, Files job); no music playing (review
 *   item 7: a handshake takes ~90 % of core 1 for 4-8 s, which is why an AI question pauses a
 *   track; only `netloc now` may) —
 *   and something has changed: never looked this boot; `netloc now`; the last look never heard
 *   beaconDB; the fix it gave has expired; a scan since then no longer hears 2/3 of the last
 *   look's devices, or (after a look with too few) now hears GEO_MIN_DEVICES; or this is a new
 *   association with no fix in hand and no scan of it yet —
 *   and GEO_LOOK_GAP_MS since the last look (GEO_LOOK_GAP_ASKED_MS for `netloc now`), and fewer
 *   than GEO_ATTEMPTS_PER_HOUR real attempts in the last hour (an attempt is up to two lookups;
 *   one that never reached the network does not count).
 * Then a snapshot of THIS association under GEO_SCAN_FRESH_MS old is needed (GEO_DUE_SCAN: ask
 * the auto-switcher for one, at most once per GEO_SCAN_REQ_GAP_MS — each scan's allocation is
 * the one that has rebooted phone 1 — through its own heap gate, join hold-off and call/game/
 * softAP skips). With fewer than GEO_MIN_DEVICES devices in it, the look is DRY (GEO_DUE_DRY):
 * nothing is sent anywhere, the verdict is "too few", and it is recorded like any look (the gap
 * applies to it, the hour's cap does not). A fix in hand is kept alive by SCANS, never by more
 * lookups: geoOnScan confirms it, and geoRefreshWanted asks for one (below). */
#define GEO_LOOK_GAP_MS        600000UL
#define GEO_LOOK_GAP_ASKED_MS  120000UL
#define GEO_ATTEMPTS_PER_HOUR       3
#define GEO_HOUR_MS           3600000UL
#define GEO_WANT_MS            600000UL
#define GEO_SCAN_REQ_GAP_MS    120000UL
#define GEO_ASK_HOLD_MS        180000UL   // `netloc now` waits this long for its scan/its turn

struct GeoSched {
  uint32_t lastLookMs;               // millis() of the last look, dry or real (0 = never)
  uint32_t attemptMs[GEO_ATTEMPTS_PER_HOUR];   // the last real attempts, newest first (0 = none)
  uint32_t askMs;                    // `netloc now` (0 = not asked)
  uint32_t scanReqMs;                // the last scan this module asked for (0 = never)
  uint32_t scanAssoc;                // the association of the last scan judged (geoOnScan)
  bool     changed;                  // a scan since the last look no longer matches its set
  bool     lastTransient;            // the last REAL look never heard beaconDB: try again
  /* `ref` is the set of the last fix this module wrote: in the NET slot, HELD (below), or expired
   * and cleared (the expiry is then a reason to look again, geoLookDue). */
  bool     fixFromRef;
  uint8_t  lastVerdict;              // GEO_V_* of the last look (bench feeds included)
  uint8_t  lastDevices;              // usable devices then
  uint16_t lastAccM;                 // LOCATED: its accuracy
  uint32_t lastApartM;               // the two halves' distance then
  GeoSet   ref;                      // the devices the next scan is held against
  uint32_t fixAssoc;                 // the association of the scan that last stood behind the fix
  bool     held;                     // the fix is HELD off the screens (geoOnLink) until a scan judges it
  SelfSlot heldFix;                  // ...the slot as it was (RAM only, never logged)
};
void geoSchedInit(GeoSched* s);
void geoAsk(GeoSched* s, uint32_t nowMs);                     // `netloc now`
bool geoAsked(const GeoSched* s, uint32_t nowMs);
int  geoAttemptsLastHour(const GeoSched* s, uint32_t nowMs);
bool geoScanRequestOk(const GeoSched* s, uint32_t nowMs);     // GEO_SCAN_REQ_GAP_MS since the last
void geoScanRequested(GeoSched* s, uint32_t nowMs);

enum GeoDue {
  GEO_DUE_YES = 0,                   // look now: build the halves, start half A
  GEO_DUE_DRY,                       // look now, without the network: too few devices (geoFold)
  GEO_DUE_SCAN,                      // due, but needs a fresh snapshot of this association
  GEO_DUE_OFF,
  GEO_DUE_BUSY,                      // a lookup is out
  GEO_DUE_NO_WIFI,
  GEO_DUE_MEASURED,                  // a live GPS/phone fix: not needed
  GEO_DUE_UNWANTED,                  // nothing on screen wants a position
  GEO_DUE_NET_BUSY,                  // a download, the uploader, a call, a game ... (the weather's gates)
  GEO_DUE_MUSIC,
  GEO_DUE_SAME,                      // nothing has changed since the last look
  GEO_DUE_TOO_SOON,
  GEO_DUE_HOUR_CAP,
};
const char* geoDueName(int d);       // "yes" "dry" "need-scan" ... (log tokens)

struct GeoWhenIn {
  uint8_t        setting;            // GEO_SET_*
  bool           busy;               // a geo job is out on the worker
  bool           wifiUp;             // a station link on someone else's network, radio on
  bool           measuredLive;       // GPS or phone live on the screens' bar (selfPick)
  uint32_t       viewedMs;           // millis() Maps or the Almanac was last on screen (0 = never)
  bool           beaconArmed;        // a position interval is set (counts only with GEO_SET_MESH)
  bool           netOk;              // the weather's gates pass
  bool           musicPlaying;
  uint32_t       assoc;              // this association (lastWifiLinkUpMs)
  bool           netLive;            // the NET slot shows a fix (selfSlotState ... == LIVE)
  const GeoScan* scan;               // the snapshot (NULL: none)
  uint8_t        scanDevices;        // its usable devices (geoSplitScan's, computed with it)
};
int geoLookDue(const GeoSched* s, const GeoWhenIn* in, uint32_t nowMs);

/* ── EVERY COMPLETED SCAN (the glue calls this once per snapshot, after geoSplitScan) ──────
 * Holds the scan against the set of the fix in the NET slot: still 2/3 heard -> the slot is
 * CONFIRMED by this scan (selfNetConfirm: its time and UTC become the fix's); fewer -> the slot is
 * CLEARED at once (review item 2: a fix carried on a walk or a drive is a confident wrong dot).
 * A HELD fix (geoOnLink) is judged the same way: still heard and not expired -> BACK on the
 * screens, confirmed by this scan; not heard -> gone. A fix past SELF_NET_SHOW_MS is cleared
 * (expired is gone, and stays gone: the expiry is a reason to look again, not to revive it).
 * Also remembers whether the scan no longer matches the last look's set (a reason to look
 * again) and which association it was taken on. `net` may be NULL (no slot to keep). */
enum { GEO_SCAN_NO_FIX = 0, GEO_SCAN_CONFIRMED, GEO_SCAN_MOVED, GEO_SCAN_BACK };
int geoOnScan(GeoSched* s, const GeoScan* scan, uint8_t scanDevices, SelfSlot* net);

/* ── THE LINK (the review of 2026-10-10: a fix outliving the network it was heard on) ────────
 * Once a second, the glue tells this the station's association now (lastWifiLinkUpMs: it moves
 * on every GOT_IP AND on the drop that ends a connected spell). A fix whose last confirming scan
 * was taken on another association is HELD: off the map, the Almanac and the air at once, until
 * the next scan judges it (geoOnScan: back, or gone). With auto-switch off nothing scans while
 * the station is down, so without this a phone walked out of its home WiFi kept "me +/-60 m
 * Wi-Fi" at home for up to SELF_NET_SHOW_MS; and a new network says the phone may be elsewhere.
 * A blip costs nothing more than a scan: the held fix comes back on the first one that still
 * hears its devices (geoRefreshWanted asks for it while the position is wanted). True when a
 * fix was held now (the glue redraws). */
bool geoOnLink(GeoSched* s, SelfSlot* net, uint32_t assoc);
/* Once a second: a fix past SELF_NET_SHOW_MS since its last confirming scan is CLEARED, and a
 * held one forgotten - "expired is gone" (self_pos.h) for real, not merely aged: the slot's age is
 * (uint32_t)(now - fixMs), and a stale fix left in place would read as fresh again when millis()
 * wraps (49.7 days on a charger). fixFromRef stays: the expiry is a reason to look (geoLookDue).
 * True when a fix on the screens was cleared now. */
bool geoExpire(GeoSched* s, SelfSlot* net, uint32_t nowMs);
/* ── WHEN A SCAN IS WORTH ASKING FOR (besides GEO_DUE_SCAN and an owed beacon slot) ──────────
 * Asked once a second by the glue, which then needs geoScanRequestOk (one per
 * GEO_SCAN_REQ_GAP_MS) and a quiet network (no handshake running: a scan's allocation lands
 * 1-4 s later on the WiFi event task, and inside a handshake's internal-heap dip it is the
 * 2026-08-28 reboot). Never with the setting off, a look out, WiFi down, or a GPS/phone fix live
 * (the Wi-Fi fix is then not what anything shows):
 *   - a HELD fix while the position is wanted: the scan that brings it back, or drops it;
 *   - a live fix while Maps or the Almanac is on screen, or `netloc now`: every
 *     GEO_SCAN_REQ_GAP_MS - a fix carried off on a hotspot that stays connected is then dropped
 *     within ~2 minutes, not the switcher's 10 (review: "me +/-60 m Wi-Fi" at home for 10 min
 *     while the car was kilometres away);
 *   - a live fix still wanted (a beacon armed) whose last confirming scan is
 *     SELF_NET_REFRESH_AT_MS old (selfNetWantsRefresh: the switcher's own scan is late). */
bool geoRefreshWanted(const GeoSched* s, const GeoWhenIn* in, const SelfSlot* net, uint32_t nowMs);

/* ── THE FOLD (the loop, after the worker's last half; `netloc answer` feeds it canned halves) ─
 * Judges the halves, records the look and, when LOCATED and the slot is switched on, writes the
 * NET slot as of the SCAN the split came from (selfNetSet: firstMs = fixMs = sp->scanMs,
 * scanUtc = sp->scanUtc). A failed look leaves a live fix alone (it passed its own agreement
 * check and the scans still confirm it) and keeps its set as the reference. A split that is not
 * built is a DRY look (too few): a and b may be NULL, nothing counts as an attempt.
 * `bench`: canned halves - the fix is written INJECTED (shown, never sent by the schedule), and
 * the schedule (last look, attempts, `netloc now`, retry) is left exactly as it was.
 * `guard` (NULL: none) holds an agreed answer to two more things the halves cannot see:
 *   - anchor: this boot's newest GPS/phone fix - too far from it is GEO_V_JUMP (geoJumpOk);
 *   - latest: the newest snapshot. A scan can land while the look is out (a look queued behind a
 *     weather fetch waits ~40 s); when it is newer than the look's own and no longer hears 2/3 of
 *     the look's devices, the answer is already contradicted: GEO_V_MOVED, nothing written, and
 *     the look is due again (changed). Newer and still heard: the fix is written AND confirmed
 *     by it (its time, UTC and association become the fix's).
 * Either refusal is recorded like any failed look (not transient: the same air would answer the
 * same). */
struct GeoFoldGuard {
  const GeoAnchor* anchor;
  const GeoScan*   latest;
};
int geoFold(GeoSched* s, const GeoSplit* sp, const GeoHalf* a, const GeoHalf* b, uint32_t nowMs,
            bool bench, SelfSlot* net, GeoFix* out, const GeoFoldGuard* guard = NULL);
/* A look abandoned because Wi-Fi location was switched OFF while it was out (the glue never
 * folds it): nothing is judged or kept - no fix, no yardstick, off means off - but a half that
 * reached the network still counts against the hour's cap and the 10-minute gap, so switching
 * off and on again never buys beaconDB more lookups than the rules allow. */
void geoFoldCancelled(GeoSched* s, const GeoHalf* a, const GeoHalf* b, uint32_t nowMs);
/* `netloc answer <A> <B>` (the bench): canned halves through geoFold(bench = true), stamped NOW
 * (scanMs = nowMs, scanUtc = `utc`) so a canned fix starts fresh whatever the snapshot's age.
 *  - `real` built (this phone's latest scan has GEO_MIN_DEVICES devices): judged on it, and the
 *    fix stands on its devices like a real one - later scans confirm it or drop it;
 *  - otherwise a stand-in split of GEO_MIN_DEVICES devices (nothing is sent anywhere): the
 *    verdict is the halves' alone, the yardstick the scans are held against is left as it was,
 *    and a LOCATED fix is not held against any scan (it simply expires SELF_NET_SHOW_MS on).
 * Either way the fix is INJECTED (shown, never sent by the schedule) and the schedule untouched.
 * `anchor` (NULL: none) is the jump backstop's, bench fixes included (geoAnchorFrom). */
int geoBenchFold(GeoSched* s, const GeoSplit* real, const GeoHalf* a, const GeoHalf* b,
                 uint32_t nowMs, uint32_t utc, uint32_t assoc, SelfSlot* net, GeoFix* out,
                 const GeoAnchor* anchor = NULL);

// ---------------------------------------------------------------- the auto-switcher's scan
/* ── WHAT THE AUTO-SWITCHER'S SCAN MAY DO (review item 8) ──────────────────────────────────
 *   FULL       the switcher as it always was: scan on its cadence, evaluate, maybe hop, and the
 *              disconnected branch (its WiFi.disconnect() and the deaf-radio bounce).
 *   SCAN_ONLY  scan and copy the snapshot, NEVER evaluate a hop, never the disconnected branch:
 *              - auto-switch is OFF but Wi-Fi location wants a scan and the station is CONNECTED
 *                (a geo scan must never start the disconnect-and-retry machinery the owner
 *                switched off); or
 *              - the phone's NMEA stream on this hotspot is live: hopping to a stronger saved
 *                network would throw away the better source for a worse one.
 *   NONE       nothing (the station is not allowed, reconnect is off, or auto-switch is off and
 *              nothing else wants a scan). */
enum { GEO_SW_NONE = 0, GEO_SW_FULL, GEO_SW_SCAN_ONLY };
int geoSwitcherMode(bool stationAllowed, bool reconnect, bool autoSwitch, bool geoWantsScan,
                    bool connected, bool phoneStreamLive);

// ---------------------------------------------------------------- the screen's words
/* The small line under "Wi-Fi location" (Meshtastic > My node). Plain words: never a MAC, an
 * SSID or a coordinate; every line within the row's 232 px of Akrobat Bold 16. `net` = the NET source's own fix (selfSource), or NULL; `due` = the last
 * geoLookDue. Returns the length written (cut to cap - 1). */
size_t geoRowText(char* out, size_t cap, int setting, const GeoSched* s, const SelfFix* net, int due);
/* The row's line while "map + mesh" has been chosen but the My node screen not yet left
 * (geoloc_net.h: the tap that passes through it on the way to "off" must never send). */
#define GEO_ROW_MESH_PENDING  "mesh starts when you leave here"

#endif // GEOLOC_H
