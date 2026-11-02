/*
 * tile_plan.h — the map download's arithmetic and its decisions, with no Arduino in them.
 *
 * tile_fetch.cpp runs the download on a task of its own and cannot be compiled on the Mac;
 * everything here CAN, so the host suite (tests/test_tileplan.cpp) pins it:
 *   - what a job covers and costs: the tile square per level, the tile count, the bytes (64-bit:
 *     20 km to z17 is 6.7 GB, past what a uint32 holds), and the time;
 *   - which depths the Download form's Detail row may offer for a source;
 *   - the order the tiles are fetched in (centre-out, in 16x16 blocks), so a multi-day run that
 *     is cut short still holds the middle;
 *   - whether a stopped job should start again by itself, and if not, why not.
 *
 * ── WHY THE RESUME DECISION IS ONE PURE FUNCTION ────────────────────────────────────────
 * A download can now take days (20 km to z17 on OpenTopoMap is ~51,000 tiles, ~36 h), and
 * nothing lasting that long survives without a WiFi drop, a call, a game or a restart. So a stopped
 * job starts again by itself — but only under the conditions the form's own Start would accept,
 * on the network it was started on (a hotspot on a trip is someone's phone bill), and never in a
 * loop that grinds the card or reboots the phone. Every one of those gates is a fact the loop can
 * read in microseconds; putting the whole decision here, over plain values, is what lets the host
 * suite walk the table instead of trusting it.
 */
#ifndef TILE_PLAN_H
#define TILE_PLAN_H

#include <stdint.h>
#include <stddef.h>

#define TILE_PLAN_ZOOM_BASE 11            // every area starts here, like COVEY's downloader
/* One job's ceiling. 20 km to z17 at 47.5 N is 51,084 tiles; the card-space check is the real
 * limit, and this only refuses a request that is plainly a mistake (and keeps the settled-tile
 * bitmap, one bit a tile, at 12.5 KB of PSRAM). */
#define TILE_JOB_MAX_TILES  100000

/* The Download form's Detail row: z13 up to z17. z17 is offered only where the source has it
 * (OpenTopoMap, and the serial custom relay): both USGS services answer 404 at z17. */
#define MAPS_DL_DEPTH_MIN   13
#define MAPS_DL_DEPTH_MAX   17
/* A download needs USB power or at least this much battery, to start AND to be resumed; a
 * running one off USB stops (and waits for USB) at this less TILE_PLAN_BATT_HYST, so a cell
 * sitting on the line does not start and stop it every second. */
#define MAPS_DL_BATT_FLOOR  3.8f
#define TILE_PLAN_BATT_HYST 0.05f

/* OpenTopoMap's z17: a MINIMUM INTERVAL between the STARTS of two such requests. The phone's
 * natural pace there is ~4 s a tile, so today this only ever bites after a 429/503 doubles it;
 * the rule is the promise, not the throttle. OTM's about page welcomes use "as long as mass
 * downloads do not overburden the server". */
#define TILE_PLAN_THROTTLE_MS     2000
#define TILE_PLAN_THROTTLE_CAP_MS 10000   // 429/503 double it (and the per-tile pause) up to this
#define TILE_PLAN_BLOCK           16      // tiles along a side of one block of the fetch order

// ── what a job covers ────────────────────────────────────────────────────────────────────
/* The +-radius square around the centre, in tile indices at zoom z. Same rule as COVEY's
 * tiles_for(): degrees of latitude per km, longitude scaled by cos(lat). y clamps to the world;
 * x does NOT wrap here — x1 may be past 2^z - 1 when the square straddles the antimeridian, and
 * the caller wraps each x it visits. */
void    tilePlanRange(double lat, double lon, int radiusKm, int z, int* x0, int* x1, int* y0, int* y1);
int64_t tilePlanLevelTiles(double lat, double lon, int radiusKm, int z);
int64_t tilePlanTiles(double lat, double lon, int radiusKm, int zMax);   // TILE_PLAN_ZOOM_BASE..zMax
uint64_t tilePlanCardBytes(int64_t tiles);                  // 131,072 a tile, raw RGB565
uint64_t tilePlanNetBytes(int64_t tiles, int kbPerTile);    // the source's rough compressed size

/* ── the elevation layer a job also fetches (0.9.80, "Elevation too" on the Download form) ──
 * The job's same +-radius square at ELEV_Z (13) and at ELEV_Z_COARSE (10), from the terrarium
 * source (docs/almanac.md, "Elevation"), written as /maps/elev/<z>/<x>/<y>.elv. One .elv is
 * 131,072 bytes on the card - exactly a map tile, so tilePlanCardBytes counts them as tiles -
 * and ~120 KB off the network (a real z13 tile at North Bend: 128,487 bytes). 20 km at 47.5 N
 * is 169 + 6 = 175 tiles (22 MB); they are never part of a job's map-tile count, cursor or
 * TILE_JOB_MAX_TILES. */
#define TILE_PLAN_ELEV_KB   120
int64_t tilePlanElevTiles(double lat, double lon, int radiusKm);
/* What a run's card-space check counts for the layer: all of it (the honest direction: which
 * tiles are on the card is not known without a stat each, and Start runs on the loop) - EXCEPT on
 * a resume whose job record says an earlier run already walked the whole layer with nothing
 * failed (`layerWhole`): then it is all on the card, and asking 22 MB for it again (20 km) could
 * refuse a resume on a nearly full card that needed no room for it (review 2026-09-27). */
int64_t tilePlanElevSpaceTiles(double lat, double lon, int radiusKm, bool elev, bool resume,
                               bool layerWhole);
/* The serial `maps dl ... <zmax> [elev] [1|0]` tail, after the five numbers: -1 = absent (spaces
 * only: the form's setting), 0 or 1, the word "elev" (any case) optional before it; -2 = anything
 * else - refused with the usage line, never quietly read as "absent" (review 2026-09-27: the help
 * said "[elev 1|0]", the parser took a bare digit, and `... 15 elev 0` downloaded elevation). */
int     tilePlanParseElevArg(const char* rest);
/* A map area a download may WRITE into (0.9.82, the custom source's `[area]`): a name the map
 * lists (mapAreaNameOk), not ending in '.', and not the reserved `elev` in any case. Both extra
 * rules are the card's: FAT is case-blind AND FatFs drops trailing dots from every path part, so
 * "ELEV" and "elev." both open /maps/elev (the altitude layer), and "otm." opens /maps/otm under
 * a spelling the Files app's guard and the map's own refresh would not recognise (review). */
int     tilePlanAreaWritable(const char* name);
/* The serial `maps dlurl <template> [area]` argument (0.9.82): the template (the first word) into
 * `tmpl`, the map area its tiles go into into `area` - "custom" when none is given, as before.
 * The area must pass tilePlanAreaWritable. A third word, a template too long for `tcap` or an area
 * too long for `acap` is refused (0); 1 = both filled. {user}, 2026-10-05: the phones should take new
 * OpenTopoMap tiles "over WiFi from the Mac" - `maps dlurl http://<mac>:8765/otm/{z}/{x}/{y}.png otm`
 * writes the Mac's relay (tools/tile_relay.py) into /maps/otm. */
int     tilePlanParseDlurl(const char* arg, char* tmpl, size_t tcap, char* area, size_t acap);

// ── the Detail row ───────────────────────────────────────────────────────────────────────
/* `wish` is what the user last picked (kept in NVS as it is); what is SHOWN and USED is the
 * wish clamped to what the source has. The press steps through the SHOWN depths only: without
 * that, USGS would get a fifth step that still reads "z16" — a press that looks dead. */
int tilePlanDepthTop(int srcZMax);                  // deepest the form offers for the source
int tilePlanDepthShown(int wish, int srcZMax);
int tilePlanDepthNext(int wish, int srcZMax);       // the wish after one press of Detail
/* The form's row for the depth shown: "Detail: z15 (3m/pixel)" - z15's metres a pixel at the
 * area's latitude in the Units setting (US: "Detail: z15 (11ft/pixel)"; review 2026-09-27: it was a
 * metric literal), "Detail: z16 (4x z15's cost)", "Detail: z17 (4x z16's cost)", "Detail: z14
 * (coarser)". units.h's formatter, so "" rather than a cut number when cap is short. */
void tilePlanDetailText(char* out, size_t cap, int depth, double lat, int units);

// ── the time it takes ────────────────────────────────────────────────────────────────────
/* The throttle for this source and level: TILE_PLAN_THROTTLE_MS for "otm" at z17 and deeper
 * (OTM's z18 is a transparent placeholder and never fetched: its zMax is 17), else 0. */
int    tilePlanMinIntervalMs(const char* srcKey, int z);
/* Seconds for the whole job: every level's tiles times that level's rate, the rate being the
 * measured all-in seconds a tile (the form's table) or the throttle interval, whichever is
 * longer. A CEILING: tiles already on the card are skipped in milliseconds. */
double tilePlanSeconds(double lat, double lon, int radiusKm, int zMax, float secPerTile, const char* srcKey);
/* "38 min" (under 90 min), "14 h" (under 48 h), "2.4 days". Never "0 min". */
void   tilePlanDuration(double secs, char* out, size_t cap);
/* "706 MB" / "6.2 GB" (from 1 GiB). */
void   tilePlanBytes(uint64_t bytes, char* out, size_t cap);

/* The live "about N left": the mean all-in time of the last TILE_PLAN_RATE_N tiles that were
 * FETCHED (skips excluded — they take milliseconds and say nothing about the server). */
#define TILE_PLAN_RATE_N 200
typedef struct {
  uint32_t ms[TILE_PLAN_RATE_N];
  int      n, head;
  uint64_t sum;
} TilePlanRate;
void     tilePlanRateReset(TilePlanRate* r);
void     tilePlanRateAdd(TilePlanRate* r, uint32_t ms);
uint32_t tilePlanRateMeanMs(const TilePlanRate* r);   // 0 when nothing is measured yet

// ── the order ────────────────────────────────────────────────────────────────────────────
/* One level, centre-out: the level's square is cut into 16x16 blocks; blocks are visited in
 * rings around the one holding the centre tile (ring 0, then the 8 around it, ...), and inside
 * a block the tiles go COLUMN by column. Why both:
 *   - rings: a multi-day run that is stopped, or that the phone gives up on, still holds the
 *     ground in the middle — the part that was wanted — rather than a stripe down one side;
 *   - columns inside a block: the card keeps a tile at /maps/<src>/<z>/<x>/<y>, so one x is one
 *     folder, and a column-major block touches ~16 folders per 256 tiles. A pure ring would
 *     change folder on every tile along its top and bottom edges.
 * Every tile of the square is handed out exactly once; `ord` counts them, which is what the
 * persisted resume cursor records. x is UNWRAPPED (x0..x0+w-1): the caller wraps it. */
typedef struct {
  int     x0, y0, w, h;       // the square, x unwrapped
  int     bw, bh;             // blocks across and down
  int     cbu, cbv;           // the centre tile's block
  int     maxR;               // the last ring that holds any block
  int     r, dv, du;          // the ring walk: ring r, row dv, column du, relative to the centre block
  int     bu, bv;             // the block being handed out
  int     u, v;               // the next tile in it, square-local
  bool    inBlock, started, done;
  int64_t ord;                // tiles handed out so far
} TilePlanOrder;
void tilePlanOrderInit(TilePlanOrder* o, int x0, int x1, int y0, int y1, int cx, int cy);
bool tilePlanOrderNext(TilePlanOrder* o, int* x, int* y);
/* The order for one level of a job: the square, and the centre tile inside it. */
void tilePlanLevelOrder(TilePlanOrder* o, double lat, double lon, int radiusKm, int z);

// ── why a run ended, and whether to start it again ───────────────────────────────────────
typedef enum {
  TILE_STOP_NONE = 0,     // running — or, read from NVS at boot, it WAS running when the phone restarted
  TILE_STOP_FINISHED,     // the whole square was walked (both passes)
  TILE_STOP_USER,         // Stop on the form or `maps dl stop`: never resumed, the NVS job is gone
  TILE_STOP_NOWIFI,       // WiFi gone 20 s: the phone's WiFi rescue needs the RAM
  TILE_STOP_CALL,         // a call held it paused past ten minutes
  TILE_STOP_NETFAILS,     // 12 tiles in a row failed on the network
  TILE_STOP_DECODEFAILS,  // 12 in a row came back as something that is not a tile (a captive portal)
  TILE_STOP_CARDFAILS,    // 12 in a row the card would not take: never resumed by itself
  TILE_STOP_RAM,          // no internal RAM for a connection, three tiles running
  TILE_STOP_BATTERY,      // off USB and under the floor
  TILE_STOP_GAME,         // the Game Boy held it paused past ten minutes
  TILE_STOP_POWEROFF,     // the phone was switched off (or powered itself off) while it ran:
                          //  a CLEAN stop, recorded so the next boot's resume is not a crash strike
} TileStopReason;
const char* tileStopReasonName(int reason);   // "user", "wifi", ... (the serial console)

typedef enum {
  TILE_GIVEUP_NONE = 0,
  TILE_GIVEUP_STRIKES,    // the phone restarted TILE_PLAN_STRIKES times during boot resumes
  TILE_GIVEUP_STALLED,    // TILE_PLAN_STALL_TRIES retries in a row without one new tile
  TILE_GIVEUP_CARD,       // the card refused writes: a person should look at it first
  TILE_GIVEUP_SPACE,      // no room on the card for the rest: free some, then Resume (the job is KEPT)
} TileGiveUp;

#define TILE_PLAN_STRIKES      3
#define TILE_PLAN_STRIKE_CLEAR 50                          // new tiles after which a boot resume is trusted
#define TILE_PLAN_WIFI_UP_MS   (60u * 1000u)               // WiFi up this long before a resume
#define TILE_PLAN_CALL_QUIET_MS (60u * 1000u)              // ...and no call for this long
/* Give up after this many failure stops / refused starts in a row without one new tile: the
 * cool-downs 10+20+40 then 23 x 60 min are a day of RETRYING. Counted in tries, not by the clock
 * since the last tile: days spent waiting at a gate (another network, off USB) are not retrying,
 * and a clock would have given a job up on its first failure after such a wait (review,
 * 2026-09-23). */
#define TILE_PLAN_STALL_TRIES  26

/* The resume bookkeeping for the one job there can be. The NVS record carries strikes, giveUp
 * and reason across a restart; the millis() stamps are per boot. */
typedef struct {
  bool     pending;       // a job waits to be resumed: persisted, not running, not finished or user-stopped
  bool     boot;          // ...found in NVS at this boot and not started since (its first resume strikes)
  uint8_t  strikes;       // boot resumes that ended neither in a recorded stop nor after 50 new tiles
  uint8_t  giveUp;        // TileGiveUp; set = only a person resumes it
  uint8_t  reason;        // TileStopReason of the last stop
  uint8_t  cools;         // failure stops and refused starts in a row since the last new tile
  bool     refused;       // the last of those was a REFUSED start (the form words it so)
  uint32_t stopMs;        // millis() of that stop / refusal / the boot load
  uint32_t progressMs;    // millis() of the last new tile (or the start, or the boot load)
} TileResume;

/* What the loop sees this pass. */
typedef struct {
  uint32_t now;
  bool     wifiUp;
  uint32_t wifiUpMs;      // how long it has been associated
  bool     sameNet;       // on the SSID the job was started on
  bool     usb;
  float    volts;         // <= 0: not known
  bool     call;          // a call is live or imminent
  uint32_t sinceCallMs;   // since one was last seen; 0xFFFFFFFF = none this boot
  bool     game;          // gGbcActive
  bool     filesJob;      // a Files folder copy/move/delete is running
  bool     card;          // gui.state.cardPresent
} TileWorld;

typedef enum {
  TILE_GATE_GO = 0,       // resume it now
  TILE_GATE_IDLE,         // nothing to resume
  TILE_GATE_GAVE_UP,      // giveUp says why; only "Resume" on the form starts it
  TILE_GATE_COOL,         // cooling down after failures: *waitMs to go
  TILE_GATE_CARD,         // no card
  TILE_GATE_FILES,        // a Files folder job is running
  TILE_GATE_GAME,
  TILE_GATE_CALL,         // a call, or one ended under a minute ago
  TILE_GATE_POWER,        // off USB and under the battery floor
  TILE_GATE_WIFI,         // not on WiFi, or not for a minute yet (*waitMs)
  TILE_GATE_NET,          // on a different network from the one it started on
} TileGate;

/* Cool-down after the n-th failure stop in a row (n >= 1): 10, 20, 40, then every 60 minutes. */
uint32_t tilePlanCoolDownMs(int n);
/* THE decision, one pass. May set r->giveUp (strikes, or a day without progress) and then says
 * TILE_GATE_GAVE_UP: the caller persists that. *waitMs (may be NULL) is how long a timed gate
 * has left. */
TileGate tileResumeCheck(TileResume* r, const TileWorld* w, uint32_t* waitMs);

/* The transitions around it. */
void tileResumeFresh(TileResume* r, uint32_t now);               // a new job starts
void tileResumeLoaded(TileResume* r, uint8_t strikes, uint8_t giveUp, uint8_t reason, uint32_t now);  // found in NVS at boot
/* A (re)start is going ahead: returns the strike count the NVS record must carry BEFORE the
 * run is handed to the worker (one more for a boot resume). */
uint8_t tileResumeStarting(TileResume* r, bool automatic);
void tileResumeStopped(TileResume* r, int reason, uint32_t now); // a run ended
void tileResumeRefused(TileResume* r, uint32_t now);             // an automatic start was refused
/* New tiles were written by the running job. Returns true when the strikes were cleared
 * (TILE_PLAN_STRIKE_CLEAR tiles into this run): the caller persists that. */
bool tileResumeProgress(TileResume* r, int newThisRun, uint32_t now);

#endif // TILE_PLAN_H
