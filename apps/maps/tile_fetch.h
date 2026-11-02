/*
 * tile_fetch.h — pulling map tiles off the network onto the card, from a task of their own.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * WHY A TASK, AND WHY THE ALLOCATOR HOOK
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * Measured 2026-09-18 on WiPhone 2 (0.9.63): an HTTPS pull of one USGS tile through the
 * uploader's /fetch path FAILED in start_ssl_client() with -32512 (SSL - Memory allocation
 * failed), took the internal heap's min-ever from 19,860 to 2,676 bytes, and left the largest
 * block fragmented at 19,160 for the rest of the session. Two causes, both in the precompiled
 * framework and neither fixable from a sketch the ordinary way:
 *
 *   1. CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y — every mbedTLS allocation comes from INTERNAL RAM,
 *      and mbedtls_ssl_setup() wants two 16,717-byte record buffers before the handshake even
 *      starts. 33.4 KB against a phone whose largest free block idles at ~25 KB.
 *   2. The handshake runs on the CALLING task's stack, and the loop task has 8 KB total
 *      (CONFIG_ARDUINO_LOOP_STACK_SIZE, baked into sdkconfig.h) with a measured floor of a few
 *      hundred bytes. TLS from loop() is impossible independent of heap.
 *
 * The escape, MEASURED the same night (`tlstest`, 70 kept-alive HTTPS GETs of a USGS tile,
 * every one a JPEG): esp_config.h defines MBEDTLS_PLATFORM_MEMORY, so mbedtls_calloc is a
 * function POINTER and libmbedtls.a exports mbedtls_platform_set_calloc_free(). Install a
 * PSRAM calloc once and every mbedTLS object — record buffers, contexts, the parsed
 * certificate chain — lands in the 3.4 MB that is actually free; PSRAM came back to the byte
 * afterwards. It is global: nothing else in this firmware allocates through mbedTLS
 * (mesh_crypto and the hashes use stack contexts), hardware AES is CPU-fed (no DMA, so PSRAM
 * operands are fine) and MPI/SHA are software. The work runs on a task with its own INTERNAL
 * stack (IDF 3.3 cannot place task stacks in PSRAM), allocated ONCE and never freed — the Game
 * Boy's rule, app_gbc.cpp:138-160. The handshake used ~4.2 KB of it; 10 KB is the margin.
 * The price: ~10 KB of internal RAM for the life of the firmware from the first download,
 * a handshake of ~1.5 s once per area (keep-alive after that: ~250 ms per tile), and a
 * transient internal dip to ~4 KB during the handshake — which is why a download PAUSES for
 * a call (tileFetchPause) rather than sharing the heap with SIP audio.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * WHAT ONE TILE COSTS, AND WHERE
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * GET (kept-alive) -> body in PSRAM -> sniff -> decode to a PSRAM 256x256 RGB565 -> write
 * /maps/<source>/<z>/<x>/<y>.tmp in four 32 KB pieces -> SD.rename to .565. The rename is
 * what makes a power-off mid-tile harmless: the viewer never sees a wrong-length file, and
 * the next run's skip test (size == 131072, NOT existence) repairs any hole.
 *
 * The task writes the card itself. The SPI bus is serialised at the HAL (TFT_eSPI force-
 * enables SUPPORT_TRANSACTIONS on ESP32; SD wraps every sector in AcquireSPI) and FatFs is
 * built FF_FS_REENTRANT, so a tile write cannot interleave bytes with a screen push — it can
 * only DELAY one. A whole tile is written between two draws rather than a sector at a time
 * for exactly that reason.
 *
 * Internal RAM per tile: the WiFiClient's 1.4 KB RX buffer and HTTPClient's URL strings.
 * Everything else — body, decoded tile, decoder pools — is PSRAM.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * MULTI-DAY JOBS (0.9.78)
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * OpenTopoMap has a real z17, and 20 km of it is ~51,000 tiles: about 2.4 days at the phone's
 * ~4 s a tile. Nothing lasts that long without a WiFi drop, a call, a game or a restart, so:
 *   - a job is PERSISTED in NVS (source key, network name, centre, radius, depth, and a cursor
 *     saved at most every 500 tiles / 5 minutes / level end) and a stopped one is RESUMED by the
 *     loop (tileFetchTick) under the rules of tile_plan.h — the same gates as Start, on the same
 *     network, with cool-downs after failures and a strike count against reboot loops;
 *   - within a level the tiles go centre-out in 16x16 blocks (tile_plan.h), so a run cut short
 *     holds the middle; a resume starts at the cursor instead of re-testing every tile;
 *   - a run that reaches the end with holes makes ONE second pass over what did not land;
 *   - a user Stop is final: it forgets the NVS job at once, not when the worker gets round to
 *     exiting (that can be 20 s: a 15 s HTTP timeout plus the card's write retries).
 * Every NVS read and write happens on the LOOP task (the tick, Start, Stop): the worker only
 * publishes its cursor, under a spinlock, for the tick to save.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * THE ELEVATION LAYER (0.9.80, "Elevation too" on the Download form)
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * A job with TileJobSpec.elev also fetches the altitude layer the map reads (docs/almanac.md,
 * "Elevation"): its own +-radius square at z13 and z10 (tilePlanElevTiles) from the AWS Terrain
 * Tiles, https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png (HTTPS only: plain
 * http is refused there with a 403), decoded by tileDecodeElev into the run's own 128 KB pixel
 * buffer and written as /maps/elev/<z>/<x>/<y>.elv with the map tiles' .tmp -> size check ->
 * rename, skipped when already on the card at 131,072 bytes. Why these rules:
 *   - IT GOES FIRST IN EVERY RUN, before the map tiles: ~20-200 tiles, so the altitude is there
 *     minutes into a job that may run for days. It has no cursor: a resumed run walks it again,
 *     and what is on the card costs one stat a tile. That is also its only retry.
 *   - ITS OWN CONNECTION, torn down before the map's is made. HTTPClient reuses any CONNECTED
 *     socket whatever host the next URL names (HTTPClient::connect), so one kept-alive client
 *     shared between two servers would send the next request to the wrong one.
 *   - A FAILURE THERE NEVER FAILS OR STOPS THE MAP TILES. It is counted in the elev* fields only
 *     (never `failed`, the consecutive-failure stops, or the RAM strikes); four in a row and the
 *     layer is left for the next run (elevGaveUp) and the map tiles start. The job's own rules
 *     still hold throughout: Stop, a call or a game pausing it, a WiFi drop, the battery.
 *   - Its tiles count in the card-space check and the form's estimate (131,072 bytes each, like
 *     a map tile) but never in the job's tile count, cursor, second pass or TILE_JOB_MAX_TILES.
 */
#ifndef TILE_FETCH_H
#define TILE_FETCH_H

#include <stdint.h>
#include <stddef.h>
#include "tile_plan.h"                    // TileStopReason, TILE_JOB_MAX_TILES, the resume rules

// ── sources ─────────────────────────────────────────────────────────────────────────────
typedef struct {
  const char* key;        // the /maps/<key> folder; must pass mapAreaNameOk()
  const char* label;      // what the screen calls it
  const char* url;        // template with {z} {x} {y}
  int         delayMs;    // politeness pause after each tile
  int         kbPerTile;  // rough download size, for the estimate
  int         zMax;       // deepest zoom the server has
  const char* credit;     // the attribution the source asks for
} TileSource;

#define TILE_SRC_CUSTOM 3                 // the serial-settable template (a LAN relay)
int               tileSourceCount();      // the fixed ones, plus custom when it is set
const TileSource* tileSource(int i);      // NULL past the end
/* `maps dlurl <template> [area]` — a plain-HTTP relay (COVEY's cache, a Mac's tile pool) when TLS
 * is not wanted, and the map area its tiles go into: /maps/<area>, "custom" when none is given
 * (NULL or ""). 0.9.82: an area may be a real map's folder ("otm", "usgs-topo") - the relay then
 * fills that map, at LAN speed and with no throttle, since the throttle belongs to OpenTopoMap's
 * servers (measured 2026-10-05 on phone 1: a z17 tile from the Mac 124 ms, from OpenTopoMap
 * 1,220 ms plus the 2.0 s interval). Never `elev`. An empty template clears both. Refused while a
 * job of this source runs. */
bool              tileSetCustomUrl(const char* templ, const char* area, char* why, size_t whyCap);
const char*       tileSourceFolder(int i); // /maps/<folder> source i writes: its key, or the custom area

// ── the job ─────────────────────────────────────────────────────────────────────────────
#define TILE_ZOOM_BASE TILE_PLAN_ZOOM_BASE   // every area starts here, like COVEY's downloader
#define TILE_MAPS_ROOT "/maps"            // == MAPS_ROOT in app_maps.h; the viewer's folder

typedef struct {
  int    source;
  double lat, lon;        // centre
  int    radiusKm;        // the +-radius square around it
  int    zMax;            // TILE_ZOOM_BASE..zMax
  bool   elev;            // also the elevation layer over the same square (see above); every caller memsets the spec
} TileJobSpec;

typedef struct {
  bool     active;        // the task is running (or about to)
  bool     finished;      // a run completed (look at the counters)
  bool     paused;        // held by tileFetchPause: a call, a game, or WiFi is down (pauseWhy)
  uint8_t  pauseWhy;      // 1 = a call, 2 = the Game Boy, 3 = no WiFi
  bool     waitingRam;    // a new connection is due and the internal heap has no room for it yet
  uint32_t ramFree, ramLargest;   // ...and what it has (bytes), for the screen and the console
  uint32_t ramNeedFree, ramNeedLargest;   // ...against what a handshake needs (the bar)
  bool     stopping;      // stop requested, finishing the current tile
  int      total;         // tiles in the job
  int      done;          // written this run
  int      skipped;       // already on the card at the right size
  int      noTile;        // 404/410, or a PNG with every pixel transparent: nothing there (not an error)
  int      failed;        // network, decode or write failures on the FIRST pass (the second's: p2Tried - p2Fixed)
  int      reconnects;    // TLS/TCP connections made after the first (the server closed keep-alive)
  int      cardRetries;   // tile writes the card refused once and took on a later try (its busy pauses)
  int      curZ;
  int      stopReason;    // TileStopReason: why the run ended (NONE while it runs)
  int      before;        // tiles a resumed run starts past (the cursor): visited by earlier runs
  int      pass;          // 1; 2 = the one automatic second pass over the tiles that did not land
  int      p2Total, p2Seen, p2Tried, p2Fixed;   // the second pass: to look at, looked at, fetched, written
  int      serverBusy;    // 429/503/502/504 answers this run (never counted as failures)
  uint32_t serverWaitSec; // > 0: the server said busy; the same tile is tried again in this many seconds
  uint32_t msPerTile;     // mean all-in time of the last 200 FETCHED tiles (0 = too few yet)
  uint64_t bytes;         // downloaded
  uint32_t elapsedMs;
  uint32_t heapFloorLargest;   // the smallest "largest internal block" seen this run
  uint32_t heapMinEver;        // the allocator's own low-water mark when the run ended
  uint32_t stackFloor;         // the task's unused stack at the end of the run (bytes)
  char     lastErr[96];   // "stopped: no RAM to connect: 13.6/14 KB free, 10.5/10 KB block" and its like
  char     source[32];    // the FOLDER the job writes (0.9.82: a custom area is up to 31 characters)
  /* The elevation layer (spec.elev): its own counts, never mixed into the map's above. */
  bool     elev;          // this job fetches it
  bool     elevRunning;   // ...and is doing so now (it goes first in every run)
  int      elevZ;         // the layer being walked: ELEV_Z, then ELEV_Z_COARSE
  int      elevTotal;     // tiles in both layers over the job's square
  int      elevDone;      // written this run
  int      elevSkipped;   // already on the card at the right size
  int      elevNoTile;    // 404/410, or a PNG with every pixel transparent
  int      elevFailed;    // network, decode or write failures (never counted in `failed`)
  bool     elevGaveUp;    // TF_ELEV_MAX_FAILS in a row: the rest is left for the next run
  bool     elevWhole;     // this run walked both levels to their end with nothing failed: the
                          // layer is all on the card (the job record keeps it - a resume's space
                          // check then asks no room for it, tilePlanElevSpaceTiles)
  char     elevErr[64];   // the last elevation problem
} TileJobStatus;

/* Tiles in the job and the bytes it will put on the card / pull from the network. 64-bit: 20 km
 * to z17 is 6.7 GB on the card, and a uint32 saturated at 4095 MB on the form. The return is the
 * MAP tiles; with spec.elev the elevation tiles are in *elevTiles (may be NULL) and in both byte
 * counts. */
int  tileFetchEstimate(const TileJobSpec* s, uint64_t* cardBytes, uint64_t* netBytes, int* elevTiles = NULL);
/* Start a NEW job (it replaces any waiting one). False with `why` filled: a run is already
 * going, no WiFi, a game or a Files folder job, a call under a minute ago, no RAM, no room... */
bool tileFetchStart(const TileJobSpec* s, char* why, size_t whyCap);
/* The user's Stop (form or serial): the run finishes the tile it is on, and the job is
 * FORGOTTEN now — it will not resume. Also forgets a waiting job that is not running. */
void tileFetchStop();
/* The phone is about to power off (the held power button, or the low-battery cut): a RUNNING
 * job's cursor and a CLEAN stop reason go to NVS now, so the next boot resumes exactly where it
 * was and does not count the restart as a crash strike. Loop task only; one NVS write. */
void tileFetchPowerOff();
bool tileFetchActive();                   // the DFS / screen-hold predicates read this
/* The loop sets these every pass. A CALL or a GAME pauses the run (up to ten minutes; the worker
 * does not touch the card while paused); NO WIFI drops the connection at once and ends the run
 * after 20 s — the phone's WiFi rescue needs the RAM. tileFetchStart never touches them: the
 * loop owns them. */
void tileFetchPause(bool call, bool game, bool noWifi);
/* What the loop sees, for tileFetchTick. */
typedef struct {
  bool        wifiUp;     // wifiState.isConnected()
  const char* ssid;       // wifiState.ssid(): the network a job is tied to
  bool        usb;
  float       volts;
  bool        card;       // gui.state.cardPresent
  bool        filesJob;   // filesJobActive(): a Files folder copy/move/delete is running
} TileFetchWorld;
/* The loop, every pass, straight after tileFetchPause. A compare or two per pass; once a second
 * it stops a running job off USB under the battery floor, saves the resume cursor when one is
 * due, settles a run that ended, and resumes a waiting job when tile_plan.h says so. It reads
 * NVS once (the first call after boot) and writes it only when one of those fires. */
void tileFetchTick(const TileFetchWorld* w);
void tileFetchStatus(TileJobStatus* out);

/* The job that is running or waiting to resume, for the Download form. */
typedef struct {
  bool     exists;        // a job is running, or persisted and waiting (or given up)
  bool     running;
  bool     resumable;     // it is in NVS: a WiFi drop or a restart does not lose it
  char     label[48];     // "OpenTopoMap", or "Custom -> <area>" (0.9.82: an area of up to 31 characters)
  char     ssid[33];      // the network it resumes on
  double   lat, lon;
  int      radiusKm, zMax;
  int      total;         // tiles in the job
  int      reached;       // tiles its cursor is past, for "31200 of 51084"
  int      gate;          // TileGate, when not running
  int      giveUp;        // TileGiveUp
  bool     otherNet;      // WiFi is up, on a different network from the job's
  bool     elev;          // it fetches the elevation layer too
  char     why[112];      // one sentence: what it waits for, or why it stopped resuming
  /* Why the last automatic resume was refused — or why a waiting job was DROPPED (the card has
   * no room for the rest, its source is gone, the saved record was unreadable). Set even when
   * `exists` is false: a dropped job must still say so on the form. */
  char     note[112];
} TileFetchJobInfo;
void tileFetchJobInfo(TileFetchJobInfo* out);
/* The form's "Resume": start the waiting job now, on THIS network (it becomes the job's
 * network), whatever a cool-down or a give-up said. The Start gates still apply. */
bool tileFetchResumeNow(char* why, size_t whyCap);
/* The /maps/<key> a running or waiting job writes under, or NULL: the Files app refuses to
 * delete or move a folder at or above it. `running` says which it is. */
const char* tileFetchJobKey(bool* running);
/* That job fetches the elevation layer too, so it writes under /maps/elev as well (every run
 * walks it first, a resume included). False when there is no such job. */
bool tileFetchJobElev();
/* Elevation tiles written by downloads since boot. The map compares it with what it last saw to
 * pick up a layer that a job has just made or filled in, without an app restart or a rescan: a
 * plain counter, bumped by the worker after each rename, read with no lock. */
uint32_t tileFetchElevWritten();
/* One line per call of `emit`, for the serial console. */
void tileFetchReport(void (*emit)(const char* line));

/* mbedTLS's allocator pointed at PSRAM (see "WHY A TASK, AND WHY THE ALLOCATOR HOOK" above): global
 * and idempotent, installed by the first TLS user that needs it - a map job, `tlstest`, or the AI
 * worker (ai_net.cpp), which may well be the first since boot. Call it BEFORE the first
 * WiFiClientSecure, or the two 16.7 KB record buffers land on the internal heap (-32512). */
void tlsInstallPsramHook();
/* A call is up, or ended under a minute ago (TILE_PLAN_CALL_QUIET_MS) - what tileFetchPause has been
 * told by the loop. The AI worker's start gate reads it: a handshake's ~12 KB internal dip is the
 * reason a download waits out a call, and a question waits the same way. */
bool tileFetchCallRecent();

/* app_files.cpp: a Files folder copy/move/delete is running (it and a download must not race
 * over /maps). Declared here because tile_fetch.cpp and the loop both read it. */
bool filesJobActive();

// ── the bench that proved the mechanism (`tlstest <url> [n]`) ────────────────────────────
bool tileFetchBenchStart(const char* url, int count);
void tileFetchBenchReport(void (*emit)(const char* line));

#endif // TILE_FETCH_H
