/*
 * tile_fetch.cpp — see tile_fetch.h.
 */
#include "tile_fetch.h"
#include "tile_decode.h"
#include "tile_plan.h"
#include "map_tiles.h"
#include "elev_tiles.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <sys/stat.h>
#include <string.h>
#include <math.h>

extern volatile bool gGbcActive;          // WiPhone.ino: the emulator owns the screen, the card and the audio

/* The fetch task's stack: internal RAM (IDF 3.3 cannot put a stack in PSRAM), allocated on
 * the first run and kept for the life of the firmware. Measured over four runs (tlstest and
 * three areas, 2026-09-18/19): the deepest path — a TLS handshake, then a tile through decode
 * and the card — left a floor of 4,996-6,264 bytes of 10,240, i.e. ~5.2 KB used. 8 KB keeps
 * a 2.8 KB margin and gives 2 KB of internal heap back to the download itself, which is
 * where the margin is thinner (the handshake's ~12 KB of lwIP buffers). `maps dl` prints
 * the floor after every run: if it ever reads under ~1,500, put this back up. */
#define TF_STACK_BYTES   8192
#define TF_BODY_CAP      (256u * 1024u)    // one tile, with room. PSRAM, so the number is free: USGS aerial tiles run past the old 96 KB ("tile larger than 96 KB" failed the same tiles on every run, 2026-09-21)
#define TF_WRITE_PIECE   (32u * 1024u)
#define TF_MAX_FAILS     12                // consecutive; then the run gives up
#define TF_CONNECT_LARGEST (10u * 1024u)   // internal largest block a TLS handshake may start with (whole KB: it is printed)
#define TF_CONNECT_FREE    (14u * 1024u)   // ...and total internal free. Set when the phones idled at 16-28 KB (phone 1 with Maps open: 13.6, never met it - 0.9.74 gave the phone 40 KB more)
#define TF_CONNECT_WAIT_MS 30000u          // how long to wait for that room before failing the tile
#define TF_MAX_RAM_FAILS   3               // consecutive tiles refused a connection for want of RAM (30 s each); then the run stops
#define TF_PAUSE_MAX_MS  (10u * 60u * 1000u) // a job paused for a CALL this long gives up
#define TF_NOWIFI_MAX_MS (20u * 1000u)     // a job without WiFi this long gives up (and frees its RAM)
#define TF_RECONNECT_AT  3                 // consecutive; then drop and remake the socket
#define TF_DRAIN_CAP     (16u * 1024u)     // how much of an error body to read before giving up on the socket
/* A server that says BUSY (429, 503, and a proxy's 502/504) is asked again for the SAME tile after
 * a wait that starts at a minute and doubles to fifteen, and the per-tile pause (and OTM's z17
 * interval) doubles for the rest of the run, capped at TILE_PLAN_THROTTLE_CAP_MS. Never a failure:
 * twelve busy answers must not stop an overnight run the way twelve dead sockets do. */
#define TF_BUSY_FIRST_MS (60u * 1000u)
#define TF_BUSY_MAX_MS   (15u * 60u * 1000u)
#define TF_BUSY_SHIFT_MAX 4                // 600 ms << 4 = 9.6 s; 2 s << 3 = 16 s, capped at 10
#define TF_LONG_WAIT_MS  (10u * 1000u)     // a wait this long lets go of the socket first
#define TF_FAIL_PACE_MS  500u              // the least pause after a failed request
#define TF_SLICE_MS      200u              // every wait is cut into these, and honours Stop and the pauses
#define TF_LEVELS        (20 - TILE_ZOOM_BASE)   // z11..z19
/* The resume cursor is written to NVS at most every TF_SAVE_TILES tiles or TF_SAVE_MS, and at the
 * end of each level — never once per tile (NVS is flash). */
#define TF_SAVE_TILES    500
#define TF_SAVE_MS       (5u * 60u * 1000u)
#define TF_TICK_MS       1000u             // tileFetchTick does its real work this often
/* Room a job must leave on the card: 256 MB or 2 % of the card, whichever is more. The old 4 MB
 * was fine at 20,000 tiles; at 100,000 a run could fill the card to the last few MB and starve
 * health.log, the mesh DB, the pins and the Game Boy's saves. */
#define TF_SPACE_MARGIN_MIN (256ull << 20)
/* The elevation layer (tile_fetch.h, "THE ELEVATION LAYER"). The AWS Open Data Terrain Tiles:
 * HTTPS only (plain http answers 403), ~120 KB an 8-bit RGB PNG, no politeness rule beyond S3's
 * own - a short pace keeps the loop fed. Four failures in a row leave the rest for the next run. */
#define TF_ELEV_URL      "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
#define TF_ELEV_MAX_FAILS 4
#define TF_REC_ELEV_ON    1              // JobRec.elev: the job fetches the elevation layer
#define TF_REC_ELEV_WHOLE 2              // ...and a run already had all of it on the card
#define TF_ELEV_PACE_MS  50u
#define TF_ELEV_FAIL_PACE_MS 2000u
#define TF_USER_AGENT    "WiPhone-maps/0.1 (wiphone-meshtastic)"
/* One .elv and one map tile are the same 131,072 bytes: the run's pixel buffer holds either, and
 * tileOnCard / writeTile / tilePlanCardBytes serve both. */
static_assert(ELEV_TILE_BYTES == MAP_TILE_BYTES, "an elevation tile must fit the run's pixel buffer exactly");
#define TF_NVS_NS        "maps"            // == MAPS_NVS in app_maps.cpp: the map's one namespace
#define TF_NVS_JOB       "dljob"
#define TF_REC_VER       1

/* ONE WORKER, CREATED ON THE FIRST RUN, NEVER DELETED. Three shapes were tried on hardware
 * (2026-09-19), and this is the one that survived phone 1:
 *   1. A STATIC task per run that deletes itself — races its own TCB: the task clears "busy"
 *      a few instructions before vTaskDelete and the loop task (same core, same priority) can
 *      re-create on the same TCB while the old one is still on a ready list; and
 *      eTaskGetState() on this FreeRTOS (8.2) reads a cleaned-up static task as eReady, so a
 *      liveness check refused every second run.
 *   2. A DYNAMIC task per run (heap stack, self-delete, idle task frees it) — the tidy answer
 *      on paper, and on phone 1 it left the internal heap FRAGMENTED: 26.3 KB largest before
 *      a run, 11.3 KB twenty minutes after, with the free total recovered. Whatever was
 *      allocated during the run (WiFi rejoined mid-run, SIP re-registered) landed inside the
 *      region the stack vacated, and "largest" is the number that predicts this phone's
 *      crashes. The superloop also never blocks for long, so the core-1 idle task that does
 *      the freeing is not guaranteed to run promptly.
 *   3. This: one worker, its 8 KB of internal stack carved once from a steady heap on the
 *      first download and held. The cost is fixed and visible — phone 1 measured five minutes
 *      after a run: free 17.3 KB / largest 12.3 KB, from 25.8 / 20.5 KB before — instead of a
 *      number that depends on what else happened during the run. `maps dl` prints it. */
static StaticTask_t      s_tcb;
static StackType_t*      s_stack = NULL;
static TaskHandle_t      s_task  = NULL;
static SemaphoreHandle_t s_go    = NULL;   // given once per run; the worker takes it
static volatile int      s_kind  = 0;      // 1 = job, 2 = bench
static volatile bool     s_busy  = false;  // a run is queued or in progress
static volatile bool  s_pause = false;     // a call or a game: wait it out (up to TF_PAUSE_MAX_MS)
static volatile bool  s_pauseGame = false; // ...and it is the game (the stop's reason, the form's word)
static volatile bool  s_noWifi = false;    // the network is gone: let go within TF_NOWIFI_MAX_MS
static volatile bool  s_stop  = false;
static bool           s_hookInstalled = false;
static volatile uint32_t s_runsEnded = 0;  // bumped by the worker as each JOB run ends (the tick settles it)
static volatile uint32_t s_elevWritten = 0; // elevation tiles written since boot (tileFetchElevWritten)

// ── sources ─────────────────────────────────────────────────────────────────────────────
static char s_customUrl[200] = "";
/* The map area the custom source's tiles go into (0.9.82): "custom" unless `maps dlurl <template>
 * <area>` names one - e.g. "otm", so new OpenTopoMap tiles relayed from the Mac land in the
 * phone's real OpenTopoMap map. 🔑 Only the FOLDER changes: the source's identity stays "custom"
 * everywhere it means the SERVER - the throttle (tilePlanMinIntervalMs: OpenTopoMap's z17 interval
 * is for OpenTopoMap's servers, never a LAN relay), the estimates, and a custom job is still never
 * persisted. Read through sourceArea(). */
static char s_customArea[32] = "custom";
/* What the form and `maps dl: started ...` call it: "Custom (serial)", or "Custom -> otm" once it
 * fills a named map, so a Start from the keypad says where its tiles will go (review). */
static char s_customLabel[48] = "Custom (serial)";
static void setCustomLabel() {
  if (!strcmp(s_customArea, "custom")) {
    strlcpy(s_customLabel, "Custom (serial)", sizeof(s_customLabel));
  } else {
    snprintf(s_customLabel, sizeof(s_customLabel), "Custom -> %s", s_customArea);
  }
}
static bool customSourceBusy();           // below: is a running job reading s_customUrl / s_customArea?

static const TileSource SOURCES[] = {
  { "usgs-topo", "USGS Topo",
    "https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}",
    50, 24, 16, "USGS National Map (public domain)" },
  { "usgs-img", "USGS Aerial",
    "https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/{z}/{y}/{x}",
    50, 28, 16, "USGS National Map (public domain)" },
  { "otm", "OpenTopoMap",
    "http://a.tile.opentopomap.org/{z}/{x}/{y}.png",
    600, 35, 17, "(c) OpenStreetMap contributors, SRTM | OpenTopoMap (CC-BY-SA)" },
  { "custom", s_customLabel, s_customUrl, 20, 24, 19, "" },
};

int tileSourceCount() {
  return s_customUrl[0] ? 4 : 3;
}

const TileSource* tileSource(int i) {
  if (i < 0 || i >= tileSourceCount()) {
    return NULL;
  }
  return &SOURCES[i];
}

/* The /maps/<folder> a source's tiles go into: its key, or the custom source's chosen area. */
static const char* sourceArea(const TileSource* src) {
  return src == &SOURCES[TILE_SRC_CUSTOM] ? s_customArea : src->key;
}

const char* tileSourceFolder(int i) {
  const TileSource* s = tileSource(i);
  return s ? sourceArea(s) : NULL;
}

bool tileSetCustomUrl(const char* templ, const char* area, char* why, size_t whyCap) {
  if (why && whyCap) why[0] = '\0';
  if (!templ || !templ[0]) {
    if (customSourceBusy()) {
      if (why) strlcpy(why, "a download is using it - stop that first", whyCap);
      return false;
    }
    s_customUrl[0] = '\0';
    strlcpy(s_customArea, "custom", sizeof(s_customArea));
    setCustomLabel();
    return true;
  }
  if (!area || !area[0]) {
    area = "custom";
  }
  /* The same rule tilePlanParseDlurl applies (the console's parser), held here too for any other
   * caller: a name the map lists, no trailing '.', never the altitude layer's folder. */
  if (strlen(area) >= sizeof(s_customArea) || !tilePlanAreaWritable(area)) {
    if (why) strlcpy(why, "area must be a map folder name (letters, digits, - _ .), not elev", whyCap);
    return false;
  }
  if (strlen(templ) >= sizeof(s_customUrl)) {
    if (why) snprintf(why, whyCap, "template longer than %u characters", (unsigned)(sizeof(s_customUrl) - 1));
    return false;
  }
  if (!strstr(templ, "{z}") || !strstr(templ, "{x}") || !strstr(templ, "{y}")) {
    if (why) strlcpy(why, "template needs {z} {x} and {y}", whyCap);
    return false;
  }
  if (strncmp(templ, "http://", 7) != 0 && strncmp(templ, "https://", 8) != 0) {
    if (why) strlcpy(why, "template must start with http:// or https://", whyCap);
    return false;
  }
  if (customSourceBusy()) {
    if (why) strlcpy(why, "a download is using it - stop that first", whyCap);
    return false;
  }
  strlcpy(s_customUrl, templ, sizeof(s_customUrl));
  strlcpy(s_customArea, area, sizeof(s_customArea));
  setCustomLabel();
  return true;
}

/* Expand {z} {x} {y}. Returns false if the template does not fit. */
static bool expandUrl(const char* templ, int z, int x, int y, char* out, size_t cap) {
  size_t w = 0;
  for (const char* p = templ; *p; p++) {
    if (*p == '{' && p[1] && p[2] == '}') {
      int v;
      if (p[1] == 'z') v = z; else if (p[1] == 'x') v = x; else if (p[1] == 'y') v = y; else { return false; }
      const int n = snprintf(out + w, cap - w, "%d", v);
      if (n < 0 || (size_t)n >= cap - w) return false;
      w += (size_t)n;
      p += 2;
    } else {
      if (w + 1 >= cap) return false;
      out[w++] = *p;
    }
  }
  out[w] = '\0';
  return true;
}

/* A fixed source by its KEY — what the persisted job records, never an index: SOURCES may be
 * reordered by a later firmware, and the custom slot is RAM-only (it is never persisted). */
static int sourceByKey(const char* key) {
  for (int i = 0; i < TILE_SRC_CUSTOM; i++) {
    if (!strcmp(SOURCES[i].key, key)) {
      return i;
    }
  }
  return -1;
}

// ── geometry (tile_plan.cpp: the square, the count, the order) ──────────────────────────
int tileFetchEstimate(const TileJobSpec* s, uint64_t* cardBytes, uint64_t* netBytes, int* elevTiles) {
  if (cardBytes) *cardBytes = 0;
  if (netBytes) *netBytes = 0;
  if (elevTiles) *elevTiles = 0;
  const TileSource* src = tileSource(s ? s->source : -1);
  if (!s || !src || s->radiusKm <= 0) {
    return 0;
  }
  int zMax = s->zMax;
  if (zMax > src->zMax) zMax = src->zMax;
  int64_t total = tilePlanTiles(s->lat, s->lon, s->radiusKm, zMax);
  if (total > 1000000) total = 1000000;      // the caller refuses far below this; keep the int an int
  const int64_t elev = s->elev ? tilePlanElevTiles(s->lat, s->lon, s->radiusKm) : 0;
  if (cardBytes) *cardBytes = tilePlanCardBytes(total + elev);
  if (netBytes)  *netBytes  = tilePlanNetBytes(total, src->kbPerTile) + tilePlanNetBytes(elev, TILE_PLAN_ELEV_KB);
  if (elevTiles) *elevTiles = (int)elev;
  return (int)total;
}

// ── the job ─────────────────────────────────────────────────────────────────────────────
struct Job {
  TileJobSpec    spec;
  int            ramFails;       // consecutive "no RAM for a connection" refusals (its own count)
  const TileSource* src;
  int            zMax;
  TileJobStatus  st;
  uint32_t       startMs;
  int            consecutiveFails;
  int            startPass, startZ;   // where this run starts: the resume cursor, or (1, z11)
  int32_t        startOrd;
  int32_t        levelBase[TF_LEVELS];   // the job-wide index of each level's first tile
  uint32_t       lastThrottleMs; // when the last throttled request (OTM z17) started; 0 = none yet
  int            busyShift;      // the server said busy this many times: pace and interval doubled
  uint32_t       busyWaitMs;     // the next busy wait before the same tile again
  TilePlanRate   rate;           // the last 200 fetched tiles' all-in times
  /* The job's OWN copies of the folder and the URL template (0.9.82): the custom source's live
   * buffers may be changed by `maps dlurl` as soon as a run ends, and the report goes on
   * describing the run after that (review: it named a relay and folder the job never used). */
  char           area[32];
  char           url[200];
};
static Job* s_job = NULL;                  // PSRAM

/* THE CURSOR: the next tile a resume would start at (pass, level, ordinal in the level's
 * order), and the tiles this run has written. The worker publishes it after every tile; the
 * loop's tick reads it and decides when it is worth an NVS write. A spinlock rather than a
 * seqlock: four ints, on a core both tasks share. */
struct Cursor {
  int     pass, z;
  int32_t ord;
  int32_t done;
};
static portMUX_TYPE s_curMux = portMUX_INITIALIZER_UNLOCKED;
static Cursor       s_cur;
static void publishCursor(int pass, int z, int32_t ord, int32_t done) {
  portENTER_CRITICAL(&s_curMux);
  s_cur.pass = pass;
  s_cur.z = z;
  s_cur.ord = ord;
  s_cur.done = done;
  portEXIT_CRITICAL(&s_curMux);
}
static Cursor readCursor() {
  Cursor c;
  portENTER_CRITICAL(&s_curMux);
  c = s_cur;
  portEXIT_CRITICAL(&s_curMux);
  return c;
}

/* THE SETTLED BITS: one a tile of the job, set when the tile is settled THIS BOOT — on the card
 * already, written, or nothing there. The one automatic second pass visits the clear bits: the
 * tiles that failed, and (after a restart) the ones before the resume cursor that this boot has
 * not seen, which cost a stat each when they are there. 12.5 KB of PSRAM, allocated with the
 * first job and kept, like the worker. A resume in the same boot keeps the bits it had. */
static uint8_t* s_settled = NULL;
static char     s_settledFor[80] = "";     // the job the bits belong to: "<key> <lat> <lon> <km> <z>"
#define TF_SETTLED_BYTES ((TILE_JOB_MAX_TILES + 7) / 8)
static bool settledGet(int i) {
  return s_settled && i >= 0 && i < TILE_JOB_MAX_TILES && (s_settled[i >> 3] & (1u << (i & 7)));
}
static void settledSet(int i) {
  if (s_settled && i >= 0 && i < TILE_JOB_MAX_TILES) {
    s_settled[i >> 3] |= (uint8_t)(1u << (i & 7));
  }
}
static int unsettledIn(int from, int to) {
  int n = 0;
  for (int i = from; i < to; i++) {
    if (!settledGet(i)) n++;
  }
  return n;
}

static bool customSourceBusy() {
  return s_busy && s_job && s_job->src == &SOURCES[TILE_SRC_CUSTOM];
}

static void heapNow(uint32_t* freeB, uint32_t* largest, uint32_t* minEver) {
  multi_heap_info_t h;
  heap_caps_get_info(&h, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (freeB)   *freeB   = h.total_free_bytes;
  if (largest) *largest = h.largest_free_block;
  if (minEver) *minEver = h.minimum_free_bytes;
}

/* mbedTLS's allocator, pointed at PSRAM. calloc semantics (zeroed). Falls back to the
 * internal heap only if PSRAM itself is exhausted, which on this phone means something else
 * is already badly wrong. */
static void* tfCalloc(size_t n, size_t sz) {
  /* PSRAM or nothing. A fallback to the internal heap would make a PSRAM shortage (3.4 MB
   * free — it has never happened) into an internal-heap raid by TLS, which is the one thing
   * this whole file exists to prevent. A NULL here fails the handshake, which is reported. */
  return heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM);
}
static void tfFree(void* p) {
  free(p);                                  // IDF's free() takes a block from any heap
}
static void installHook() {
  if (!s_hookInstalled) {
    mbedtls_platform_set_calloc_free(tfCalloc, tfFree);
    s_hookInstalled = true;
  }
}

/* The same hook for any other TLS user (tile_fetch.h). Idempotent; never uninstalled. */
void tlsInstallPsramHook() {
  installHook();
}


/* One HTTP GET into `body`. Returns the HTTP code (200 = *got bytes are the tile), 0 for a
 * transport failure. `http`/`client` are the kept-alive pair for the whole run. */
static int fetchOne(HTTPClient& http, WiFiClient& client, const char* url,
                    uint8_t* body, size_t cap, size_t* got, char* err, size_t errCap) {
  *got = 0;
  if (!http.begin(client, url)) {
    strlcpy(err, "bad URL", errCap);
    return 0;
  }
  const int code = http.GET();
  const int len  = http.getSize();       // -1 = no Content-Length
  WiFiClient* s  = http.getStreamPtr();
  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      snprintf(err, errCap, "%s", http.errorToString(code).c_str());
      http.end();
      client.stop();                     // a transport error leaves nothing worth keeping
      return 0;
    }
    snprintf(err, errCap, "HTTP %d", code);
    /* ⚠ THE SOCKET IS KEPT ALIVE, SO THE ERROR BODY MUST BE READ OR THE NEXT TILE STARTS
     * IN THE MIDDLE OF IT. HTTPClient::end() drains only what has ARRIVED; a 404 page whose
     * tail is still in flight would be parsed as the next response and burn two tiles.
     * Read it out (bounded), and if it cannot be read completely, drop the connection. */
    bool clean = false;
    if (len >= 0 && (size_t)len <= TF_DRAIN_CAP) {
      size_t n = 0;
      uint32_t idle = millis();
      while (n < (size_t)len && http.connected() && millis() - idle < 3000) {
        const size_t avail = s->available();
        if (avail) {
          size_t want = avail;
          if (want > cap) want = cap;
          const int r = s->read(body, want);
          if (r > 0) {
            n += (size_t)r;
            idle = millis();
          }
        } else {
          vTaskDelay(1);
        }
      }
      clean = (n >= (size_t)len);
    }
    http.end();
    if (!clean) {
      client.stop();
    }
    return code;
  }
  if (len < 0) {
    /* No Content-Length: a chunked or close-delimited body. HTTPClient's stream pointer gives
     * the raw bytes and no way to know where the tile ends on a kept-alive socket; every
     * such tile would sit out the 8 s stall and fail. Say so, once per tile, and drop the
     * socket so the next GET starts clean. (USGS and OpenTopoMap both send a length.) */
    strlcpy(err, "no Content-Length (chunked?)", errCap);
    http.end();
    client.stop();
    return 0;
  }
  size_t   n    = 0;
  uint32_t idle = millis();
  bool     stalled = false;
  bool     overCap = false;
  while (http.connected() && n < (size_t)len) {
    const size_t avail = s->available();
    if (avail) {
      size_t want = avail;
      if (want > cap - n) want = cap - n;
      const int r = s->read(body + n, want);
      if (r > 0) {
        n += (size_t)r;
        idle = millis();
      }
      if (n >= cap && n < (size_t)len) {   // bigger than any tile
        overCap = true;
        break;
      }
    } else if (millis() - idle > 8000) {
      stalled = true;
      break;
    } else {
      vTaskDelay(1);                     // never spin: the loop task shares this core
    }
  }
  http.end();                            // reuse=true: the socket stays if the server allows
  if (stalled || overCap || n < (size_t)len) {
    /* Whatever is left of the body is still in the socket; a kept-alive next GET would read
     * it as the next response. Drop the connection — the reconnect is the cheaper mistake. */
    client.stop();
    strlcpy(err, stalled ? "body stalled" : (overCap ? "tile larger than 256 KB" : "body cut short"), errCap);
    return 0;
  }
  *got = n;
  return 200;
}

/* Bytes free on the card, or 0xFFFFFFFF if the card cannot say. FatFs trusts FSINFO here
 * (FF_FS_NOFSINFO=0), so this is one read, not a walk. */
static uint64_t cardFreeBytes() {
  const uint64_t total = SD.totalBytes();
  const uint64_t used  = SD.usedBytes();
  if (total == 0) {
    return 0xFFFFFFFFFFFFFFFFull;
  }
  return total > used ? total - used : 0;
}

/* Write the decoded tile as <dir>/<y>.tmp then rename it into place. False = nothing usable
 * was left behind (the .tmp is removed). */
static bool writeTile(const char* path, const char* tmpPath, const uint16_t* px, char* err, size_t errCap) {
  if (SD.exists(tmpPath)) {
    SD.remove(tmpPath);              // exists() first: SD.remove() logs an error line for a missing file
  }
  File f = SD.open(tmpPath, FILE_WRITE);
  if (!f) {
    strlcpy(err, "cannot create the tile file", errCap);
    return false;
  }
  const uint8_t* b = (const uint8_t*)px;
  size_t w = 0;
  while (w < (size_t)MAP_TILE_BYTES) {
    size_t piece = (size_t)MAP_TILE_BYTES - w;
    if (piece > TF_WRITE_PIECE) piece = TF_WRITE_PIECE;
    const size_t r = f.write(b + w, piece);
    if (r != piece) {
      f.close();
      SD.remove(tmpPath);
      strlcpy(err, "card refused the write", errCap);
      return false;
    }
    w += r;
    vTaskDelay(1);                       // let a screen push in between pieces
  }
  f.close();
  File chk = SD.open(tmpPath);
  const size_t size = chk ? (size_t)chk.size() : 0;
  if (chk) chk.close();
  if (size != (size_t)MAP_TILE_BYTES) {
    SD.remove(tmpPath);
    strlcpy(err, "tile file came back short", errCap);
    return false;
  }
  if (SD.exists(path)) {
    SD.remove(path);                     // a wrong-length leftover would make rename fail
  }
  if (!SD.rename(tmpPath, path)) {
    SD.remove(tmpPath);
    strlcpy(err, "rename failed", errCap);
    return false;
  }
  return true;
}

/* Is the tile there at the right size (NOT merely there: a power cut mid-write leaves a short
 * .tmp, never a short .565, but a tile from an old converter bug might be)? A POSIX stat on the
 * card's VFS mount: one path walk and no "[E] ... does not exist" log line, where SD.open() is
 * a stat, an fopen and the File object's internal-heap allocation — and a resumed multi-day run
 * asks this of tens of thousands of tiles. */
static bool tileOnCard(const char* path) {
  char p[128];
  snprintf(p, sizeof(p), "/sd%s", path);
  struct stat sb;
  return ::stat(p, &sb) == 0 && (size_t)sb.st_size == (size_t)MAP_TILE_BYTES;
}

static void mkdirChain(const char* dirPath) {
  /* /maps, /maps/<key>, /maps/<key>/<z>, /maps/<key>/<z>/<x> — mkdir is one level at a time
   * and false for an existing folder, which is the common case. ⚠ NOT cheap on an existing
   * folder: each SD.mkdir opens it (a stat, an opendir, two small internal-heap allocations),
   * five of them a call. So a run calls this only before the FIRST write into a column
   * (makeColumn), never per tile and never for a tile it skips. */
  char tmp[96];
  strlcpy(tmp, dirPath, sizeof(tmp));
  for (char* p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      SD.mkdir(tmp);
      *p = '/';
    }
  }
  SD.mkdir(tmp);
}

/* Wait out a pause (a call, no WiFi) and, when a new connection is about to be made, wait
 * for the internal RAM a TLS handshake needs. Returns false when the tile should be given up
 * on: the run was stopped, the pause outlived TF_PAUSE_MAX_MS (the run stops), or the RAM
 * never came (this tile fails; TF_MAX_RAM_FAILS of those in a row stop the run with the
 * numbers). `needRoom` is whether a handshake is due. */
/* Every internal stop goes through here: the REASON is what the loop's resume rules read
 * (tile_plan.h), and a reason already set is kept — the user's Stop (USER, set by
 * tileFetchStop) must not be overwritten by whatever the worker trips over on its way out. */
static void stopWith(Job* j, int reason, const char* msg) {
  if (j->st.stopReason == TILE_STOP_NONE) {
    j->st.stopReason = reason;
  }
  if (msg) {
    strlcpy(j->st.lastErr, msg, sizeof(j->st.lastErr));
  }
  s_stop = true;
}

static bool waitReady(Job* j, bool needRoom, WiFiClient* client) {
  TileJobStatus* st = &j->st;
  uint32_t pausedMs = 0, noWifiMs = 0, roomMs = 0;
  bool dropped = false;
  for (;;) {
    if (s_stop) {
      return false;
    }
    if (s_pause || s_noWifi) {
      st->paused = true;
      st->pauseWhy = s_noWifi ? 3 : (s_pauseGame ? 2 : 1);
      st->waitingRam = false;
      /* 🛑 LET GO OF THE CONNECTION WHILE PAUSED. Measured on phone 1 (2026-09-19): WiFi
       * dropped during a run's first handshake, the job sat paused holding its TLS session,
       * lwIP buffers and client objects, and the phone's WiFi RESCUE — the scan and rejoin,
       * which need internal heap of their own — had to run against largest = 8,984 bytes.
       * The socket and the TLS state are the part that can be given back at once. */
      if (client && !dropped) {
        client->stop();
        dropped = true;
      }
      if (s_noWifi) {
        /* No network: nothing a wait can fix that the phone's own rescue is not already
         * trying, and the rescue wants the RAM this job holds. Twenty seconds covers a blip;
         * past that the run ends, and a restart skips every tile already written. */
        noWifiMs += 200;
        if (noWifiMs >= TF_NOWIFI_MAX_MS) {
          /* A game turns WiFi off to start (app_gbc.cpp startGame): name the game, so the job
           * waits for it to end rather than only for the network. */
          if (s_pauseGame) {
            stopWith(j, TILE_STOP_GAME, "a game turned WiFi off - stopped");
          } else {
            stopWith(j, TILE_STOP_NOWIFI, "WiFi dropped - stopped");
          }
          st->paused = false;
          return false;
        }
      } else {
        pausedMs += 200;
        if (pausedMs >= TF_PAUSE_MAX_MS) {
          if (s_pauseGame) {
            stopWith(j, TILE_STOP_GAME, "paused too long (a game) - stopped");
          } else {
            stopWith(j, TILE_STOP_CALL, "paused too long (a call) - stopped");
          }
          st->paused = false;
          return false;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    st->paused = false;
    if (!needRoom) {
      st->waitingRam = false;
      return true;
    }
    uint32_t f, l, m;
    heapNow(&f, &l, &m);
    st->ramFree = f;
    st->ramLargest = l;
    st->ramNeedFree = TF_CONNECT_FREE;
    st->ramNeedLargest = TF_CONNECT_LARGEST;
    if (l >= TF_CONNECT_LARGEST && f >= TF_CONNECT_FREE) {
      st->waitingRam = false;
      return true;
    }
    if (roomMs >= TF_CONNECT_WAIT_MS) {
      st->waitingRam = false;
      return false;                       // this tile fails; the caller counts it
    }
    st->waitingRam = true;
    vTaskDelay(pdMS_TO_TICKS(250));
    roomMs += 250;
  }
}

/* "no RAM for a new connection", with the numbers: the two the bar is made of, so the screen
 * and the console say what is short by how much rather than only that something is. Phone 1
 * sat at 13.6 KB free under the 14 KB bar for two hours saying "waiting for memory"
 * (2026-09-21); this is what it should have said. */
/* KB with one decimal: 13,836 -> "13.5". The bar is printed the same way so 14000 reads 13.7,
 * not "13" (integer KB made the screen say "needs 13, has 13.6" and contradict itself). */
static unsigned kbWhole(uint32_t b) {
  return (unsigned)(b / 1024);
}
static unsigned kbTenth(uint32_t b) {
  return (unsigned)(((b % 1024) * 10) / 1024);
}
static void ramShortMsg(char* out, size_t cap, const TileJobStatus* st, const char* prefix) {
  snprintf(out, cap, "%sno RAM to connect: %u.%u/%u.%u KB free, %u.%u/%u.%u KB block",
           prefix, kbWhole(st->ramFree), kbTenth(st->ramFree), kbWhole(st->ramNeedFree), kbTenth(st->ramNeedFree),
           kbWhole(st->ramLargest), kbTenth(st->ramLargest), kbWhole(st->ramNeedLargest), kbTenth(st->ramNeedLargest));
}

/* The pace, doubled once for every time the server has said busy this run, capped. */
static uint32_t paced(const Job* j, uint32_t base) {
  const uint32_t v = base << j->busyShift;
  return v > TILE_PLAN_THROTTLE_CAP_MS ? TILE_PLAN_THROTTLE_CAP_MS : v;
}

/* Wait `ms` in TF_SLICE_MS slices. False = the run was stopped. A pause (a call, a game, no
 * WiFi) met on the way is waited out through waitReady — which also gives the run up when the
 * pause lasts too long — and its time counts toward the wait. `server` shows the seconds left
 * on the form ("Server busy - trying again in N min"). The waits here reach fifteen minutes; a
 * single vTaskDelay that long would leave Stop, the call pause and the WiFi rescue waiting. */
static bool waitSliced(Job* j, uint32_t ms, WiFiClient* client, bool server) {
  TileJobStatus* st = &j->st;
  const uint32_t t0 = millis();
  bool ok = true;
  for (;;) {
    const uint32_t gone = (uint32_t)(millis() - t0);
    if (gone >= ms) {
      break;
    }
    if (s_stop) {
      ok = false;
      break;
    }
    if (s_pause || s_noWifi) {
      if (!waitReady(j, false, client)) {
        ok = false;
        break;
      }
      continue;
    }
    const uint32_t left = ms - gone;
    if (server) {
      st->serverWaitSec = (left + 999u) / 1000u;
    }
    vTaskDelay(pdMS_TO_TICKS(left < TF_SLICE_MS ? left : TF_SLICE_MS));
  }
  if (server) {
    st->serverWaitSec = 0;
  }
  return ok;
}

/* What one run needs through every tile: the kept-alive pair, the PSRAM buffers, and this
 * level's "x folder made" bits. */
struct RunCtx {
  HTTPClient* http;
  WiFiClient* client;
  uint8_t*    body;
  uint16_t*   px;
  uint8_t*    cols;          // one bit a column of the level (PSRAM); NULL = make the folder every write
  int         colX0;         // the level's first column, unwrapped
  uint32_t    floorLargest;
};

/* The x folder, once per column per level: mkdirChain before the FIRST write into it. A skipped
 * tile needs no folder, and a column-major block writes ~16 tiles a column. */
static void makeColumn(RunCtx* c, int xi, const char* dir) {
  const int i = xi - c->colX0;
  if (c->cols && i >= 0 && (c->cols[i >> 3] & (1u << (i & 7)))) {
    return;
  }
  mkdirChain(dir);
  if (c->cols && i >= 0) {
    c->cols[i >> 3] |= (uint8_t)(1u << (i & 7));
  }
}

enum { OUT_DONE = 0, OUT_NOTILE, OUT_FAILED, OUT_STOPPED };

/* writeTile, with the card's busy pauses waited out. `tag` names the layer in the log line.
 * 🛑 A CARD THAT IS BUSY IS NOT A CARD THAT IS BROKEN. "card refused the write" is a short
 * f.write(): the SD layer gives a card 500 ms to come out of busy before a block (sd_diskio.cpp
 * sdWriteBytes) and some cards pause longer than that for their own housekeeping. Phone 2's
 * 12,853-tile run lost 4 tiles to it; phone 1's card does it to ONE WRITE IN EIGHT (2026-09-21: 35
 * rescued on a second try, 7 lost, in the first 311 tiles) — and every tile lost had already
 * crossed the network. So the write is tried again after a pause that grows — 200 ms, then a
 * second, then three — long enough for any card's pause to end; the pixels are still in PSRAM,
 * nothing is fetched twice. Only a card that refuses for five seconds counts the tile as failed.
 * The retries are counted (cardRetries) and shown, because they are the measure of the card. */
static bool writeTileRetrying(Job* j, const char* tag, const char* path, const char* tmpPath,
                              const uint16_t* px, int z, int x, int y, char* err, size_t errCap) {
  static const uint16_t WRITE_WAIT_MS[] = { 200, 1000, 3000 };
  bool wrote = writeTile(path, tmpPath, px, err, errCap);
  for (int t = 0; !wrote && !s_stop && t < 3; t++) {
    vTaskDelay(pdMS_TO_TICKS(WRITE_WAIT_MS[t]));
    wrote = writeTile(path, tmpPath, px, err, errCap);
    if (wrote) {
      j->st.cardRetries++;
      log_e("%s: z%d %d/%d written on try %d", tag, z, x, y, t + 2);
    }
  }
  return wrote;
}

/* A failure is counted on the FIRST pass only: the second pass's "tried again / fixed" say what
 * became of them, and counting a tile twice would make the summary lie. */
static void countFail(Job* j, int pass) {
  if (pass == 1) {
    j->st.failed++;
  }
}

/* One tile that is not on the card: fetch, decode, write. `x` is wrapped, `xi` the unwrapped
 * column (for the folder bits). OUT_STOPPED = the run is stopping and this tile did not finish:
 * the cursor stays on it and a resume tries it again. */
static int processTile(Job* j, RunCtx* c, int pass, int z, int x, int xi, int y,
                       const char* dir, const char* path, const char* tmpPath) {
  TileJobStatus* st = &j->st;
  char url[300], err[64];
  const uint32_t t0 = millis();
  if (!expandUrl(j->url, z, x, y, url, sizeof(url))) {
    strlcpy(st->lastErr, "URL template too long", sizeof(st->lastErr));
    countFail(j, pass);
    return OUT_FAILED;
  }
  const uint32_t interval = (uint32_t)tilePlanMinIntervalMs(j->src->key, z);
  int code = 0;
  size_t got = 0;
  for (;;) {                              // the same tile again, while the server says busy
    /* 🔑 THE THROTTLE IS A MINIMUM INTERVAL BETWEEN REQUEST STARTS, not a pause added to every
     * tile: OpenTopoMap at z17 is never asked more than once per TILE_PLAN_THROTTLE_MS (doubled
     * after a 429/503), and at the phone's natural ~4 s a tile it costs nothing. It is waited
     * out BEFORE the RAM check below, because a pause met inside it can drop the connection,
     * and a reconnect must pass that check. */
    if (interval && j->lastThrottleMs) {
      const uint32_t need = paced(j, interval);
      const uint32_t since = (uint32_t)(millis() - j->lastThrottleMs);
      if (since < need && !waitSliced(j, need - since, c->client, false)) {
        return OUT_STOPPED;
      }
    }
    /* 🛑 A NEW TLS HANDSHAKE COSTS ~12 KB OF INTERNAL RAM FOR A SECOND, AND THE SERVER
     * CLOSES A KEPT-ALIVE CONNECTION EVERY SO MANY REQUESTS. Measured 2026-09-19: a 10 km
     * run with the map open took the internal heap's min-ever to 952 bytes — the
     * reconnect landed on top of the app's own use. So before any GET that will have to
     * reconnect, wait for room (and wait out a call or a WiFi drop, which is the same
     * loop): a handshake that would start below the bar is a reboot with extra steps. */
    const bool reconnecting = !c->client->connected() || s_pause || s_noWifi;
    if (!waitReady(j, reconnecting, c->client)) {
      if (s_stop) {
        return OUT_STOPPED;
      }
      countFail(j, pass);
      ramShortMsg(st->lastErr, sizeof(st->lastErr), st, "");
      /* 🛑 THE RAM IS NOT COMING. A tile that failed for want of a connection is not a
       * bad tile, and the next one meets the same heap: the old rule failed twelve of
       * them thirty seconds apart, six minutes of "waiting for memory" for the same
       * answer, and then stopped without saying what the numbers were. Three in a row
       * is the same fact three times: stop, and say how short by how much. Its own
       * counter — a network failure and a RAM refusal are not the same run of luck. */
      if (++j->ramFails >= TF_MAX_RAM_FAILS) {
        ramShortMsg(st->lastErr, sizeof(st->lastErr), st, "stopped: ");
        stopWith(j, TILE_STOP_RAM, NULL);
        return OUT_STOPPED;
      }
      return OUT_FAILED;
    }
    j->ramFails = 0;                     // the room was there this time
    if (reconnecting && (st->done + st->failed + st->noTile > 0)) {
      st->reconnects++;
    }
    if (interval) {
      j->lastThrottleMs = millis() | 1u; // 0 means "none yet"
    }
    got = 0;
    err[0] = '\0';
    code = fetchOne(*c->http, *c->client, url, c->body, TF_BODY_CAP, &got, err, sizeof(err));
    if (code == 429 || code == 503 || code == 502 || code == 504) {
      /* THE SERVER SAYS BUSY. Not a failure and not a skip: wait, then the SAME tile. The pace
       * (and OTM's z17 interval) stays doubled for the rest of the run. A wait of minutes lets
       * go of the socket first — holding a TLS session through it is RAM the phone wants. */
      st->serverBusy++;
      if (j->busyShift < TF_BUSY_SHIFT_MAX) {
        j->busyShift++;
      }
      const uint32_t wait = j->busyWaitMs;
      j->busyWaitMs = wait * 2 > TF_BUSY_MAX_MS ? TF_BUSY_MAX_MS : wait * 2;
      snprintf(st->lastErr, sizeof(st->lastErr), "z%d %d/%d: server busy (HTTP %d)", z, x, y, code);
      if (wait >= TF_LONG_WAIT_MS) {
        c->client->stop();
      }
      if (!waitSliced(j, wait, c->client, true)) {
        return OUT_STOPPED;
      }
      continue;
    }
    j->busyWaitMs = TF_BUSY_FIRST_MS;    // any other answer ends a run of busy ones
    break;
  }

  int out;
  if (code == 404 || code == 410) {
    st->noTile++;                        // the server has nothing there: not a failure
    j->consecutiveFails = 0;
    out = OUT_NOTILE;
  } else if (code != 200) {
    countFail(j, pass);
    snprintf(st->lastErr, sizeof(st->lastErr), "z%d %d/%d: %s", z, x, y, err);
    if (++j->consecutiveFails >= TF_MAX_FAILS) {
      stopWith(j, TILE_STOP_NETFAILS, "network gave up - stopped");
      return OUT_STOPPED;
    }
    if (j->consecutiveFails % TF_RECONNECT_AT == 0) {
      c->client->stop();                 // remake the socket (and the TLS session) next time
    }
    out = OUT_FAILED;
  } else {
    st->bytes += (uint64_t)got;
    /* ⚠ THE CONSECUTIVE-FAILURE COUNT IS RESET ON A WRITTEN TILE, NOT ON A 200. The
     * first cut reset it here, before decode and write ran, so a card that refused
     * every write (or a server answering 200 with pages) could never reach the
     * give-up threshold — the run fetched the whole area for nothing (review). */
    const TileDecodeResult dr = tileDecode(c->body, got, c->px, err, sizeof(err));
    if (dr == TILE_DECODE_BLANK) {
      /* Every pixel transparent: "no tile here", the same as a 404 (OTM's placeholder for a
       * level it does not have). Nothing is written — a flat grey file would be skipped as
       * "on the card" by every later run and never fetched again. */
      st->noTile++;
      j->consecutiveFails = 0;
      out = OUT_NOTILE;
    } else if (dr != TILE_DECODE_OK) {
      countFail(j, pass);
      snprintf(st->lastErr, sizeof(st->lastErr), "z%d %d/%d: %s", z, x, y, err);
      if (++j->consecutiveFails >= TF_MAX_FAILS) {
        stopWith(j, TILE_STOP_DECODEFAILS, "nothing decodes - stopped");
        return OUT_STOPPED;
      }
      out = OUT_FAILED;
    } else {
      makeColumn(c, xi, dir);
      const bool wrote = writeTileRetrying(j, "TILES", path, tmpPath, c->px, z, x, y, err, sizeof(err));
      if (!wrote) {
        /* A card that refuses twice is not going to accept the next 800 tiles either: a
         * full, absent or pulled card counts the same way the network does, and the run
         * stops rather than downloading the whole area for nothing. */
        countFail(j, pass);
        snprintf(st->lastErr, sizeof(st->lastErr), "z%d %d/%d: %s", z, x, y, err);
        if (++j->consecutiveFails >= TF_MAX_FAILS) {
          stopWith(j, TILE_STOP_CARDFAILS, "the card keeps refusing writes - stopped");
          return OUT_STOPPED;
        }
        out = OUT_FAILED;
      } else {
        st->done++;
        j->consecutiveFails = 0;
        out = OUT_DONE;
      }
    }
  }
  uint32_t f, l, m;
  heapNow(&f, &l, &m);
  if (l < c->floorLargest) c->floorLargest = l;
  st->elapsedMs = millis() - j->startMs;
  /* The politeness pause, after EVERY request — a failure too, and never less than half a
   * second then: an overloaded server answering 502 must not be asked twice a second. A Stop
   * inside it is seen at the top of the next tile; this tile's outcome stands. */
  uint32_t pace = paced(j, (uint32_t)(j->src->delayMs > 0 ? j->src->delayMs : 1));
  if (out == OUT_FAILED && pace < TF_FAIL_PACE_MS) {
    pace = TF_FAIL_PACE_MS;
  }
  waitSliced(j, pace, c->client, false);
  tilePlanRateAdd(&j->rate, (uint32_t)(millis() - t0));
  st->msPerTile = j->rate.n >= 5 ? tilePlanRateMeanMs(&j->rate) : 0;
  return out;
}

/* One level of one pass, in the centre-out block order (tile_plan.h), from ordinal `fromOrd`.
 * Pass 2 visits only the tiles whose settled bit is clear. */
static void walkLevel(Job* j, RunCtx* c, int pass, int z, int32_t fromOrd) {
  TileJobStatus* st = &j->st;
  st->curZ = z;
  TilePlanOrder o;
  tilePlanLevelOrder(&o, j->spec.lat, j->spec.lon, j->spec.radiusKm, z);
  const int n = 1 << z;
  c->colX0 = o.x0;
  c->cols = (uint8_t*)heap_caps_calloc((size_t)(o.w + 7) / 8, 1, MALLOC_CAP_SPIRAM);
  int x, y;
  while (o.ord < fromOrd && tilePlanOrderNext(&o, &x, &y)) {
    // the resume cursor: arithmetic only, the card is not touched
  }
  char dir[96], path[112], tmpPath[112];
  int idle = 0;                           // tiles in a row that needed no fetch
  for (;;) {
    /* 🛑 THE TOP OF EVERY TILE, BEFORE THE CARD IS TOUCHED: a Stop, or a pause for a call or a
     * game. A skip scan is nothing but card reads, and the Game Boy keeps the card and the LCD
     * apart with a lock of its own that this task never takes; the old loop tested the card
     * first and waited later, so a game or a call could meet twenty seconds of it. */
    if (s_stop) {
      break;
    }
    if ((s_pause || s_noWifi) && !waitReady(j, false, c->client)) {
      break;
    }
    const int32_t ord = (int32_t)o.ord;
    if (!tilePlanOrderNext(&o, &x, &y)) {
      break;
    }
    const int bit = j->levelBase[z - TILE_ZOOM_BASE] + ord;
    if (pass == 2) {
      if (settledGet(bit)) {
        publishCursor(pass, z, ord + 1, st->done);
        if (++idle % 256 == 0) {
          vTaskDelay(1);                  // bits only, no card: a longer stride
        }
        continue;
      }
      st->p2Seen++;
    }
    const int wx = ((x % n) + n) % n;
    snprintf(dir, sizeof(dir), "%s/%s/%d/%d", TILE_MAPS_ROOT, j->area, z, wx);
    snprintf(path, sizeof(path), "%s/%d.565", dir, y);
    snprintf(tmpPath, sizeof(tmpPath), "%s/%d.tmp", dir, y);
    if (tileOnCard(path)) {
      if (pass == 1) {
        st->skipped++;
      }
      settledSet(bit);
      publishCursor(pass, z, ord + 1, st->done);
      if (++idle % 16 == 0) {
        vTaskDelay(1);                    // a skip scan never holds the core (or the card's lock) for long
      }
      continue;
    }
    idle = 0;
    const int out = processTile(j, c, pass, z, wx, x, y, dir, path, tmpPath);
    if (out == OUT_STOPPED) {
      break;                              // the cursor stays ON this tile
    }
    if (pass == 2) {
      st->p2Tried++;
      if (out == OUT_DONE) st->p2Fixed++;
    }
    if (out == OUT_DONE || out == OUT_NOTILE) {
      settledSet(bit);
    }
    publishCursor(pass, z, ord + 1, st->done);
  }
  free(c->cols);
  c->cols = NULL;
}

// ── the elevation layer (0.9.80; tile_fetch.h, "THE ELEVATION LAYER") ────────────────────

/* One elevation tile that is not on the card: the terrarium PNG, turned into the .elv bytes in
 * the run's pixel buffer (tileDecodeElev: nothing allocated for the output), written the map
 * tiles' way. The walker counts the outcome in the elev* fields ONLY: nothing here touches
 * `failed`, consecutiveFails or ramFails, so an elevation failure can never stop the map tiles. */
static int elevTile(Job* j, RunCtx* c, int z, int x, int xi, int y,
                    const char* dir, const char* path, const char* tmpPath) {
  TileJobStatus* st = &j->st;
  char url[112], err[64];
  if (!expandUrl(TF_ELEV_URL, z, x, y, url, sizeof(url))) {
    strlcpy(st->elevErr, "URL too long", sizeof(st->elevErr));
    return OUT_FAILED;
  }
  /* The same room a map tile's reconnect waits for (and the same wait for a call or a WiFi drop,
   * which can stop the JOB - that is the job's rule, not this tile's). No room in 30 s is this
   * tile's failure; the map's three-strikes RAM stop is not counted here. */
  const bool reconnecting = !c->client->connected() || s_pause || s_noWifi;
  if (!waitReady(j, reconnecting, c->client)) {
    if (s_stop) {
      return OUT_STOPPED;
    }
    snprintf(st->elevErr, sizeof(st->elevErr), "no RAM to connect (%u.%u KB block)",
             kbWhole(st->ramLargest), kbTenth(st->ramLargest));
    waitSliced(j, TF_ELEV_FAIL_PACE_MS, c->client, false);
    return OUT_FAILED;
  }
  size_t got = 0;
  err[0] = '\0';
  const int code = fetchOne(*c->http, *c->client, url, c->body, TF_BODY_CAP, &got, err, sizeof(err));
  int out;
  if (code == 404 || code == 410) {
    out = OUT_NOTILE;                      // nothing there: not a failure
  } else if (code != 200) {
    /* A busy answer (S3's 503 SlowDown) included: at this layer's size a wait of minutes would
     * cost more than the tile; it is a failure, paced, and the next run asks again. */
    snprintf(st->elevErr, sizeof(st->elevErr), "z%d %d/%d: %s", z, x, y, err);
    out = OUT_FAILED;
  } else {
    st->bytes += (uint64_t)got;
    const TileDecodeResult dr = tileDecodeElev(c->body, got, (uint8_t*)c->px, err, sizeof(err));
    if (dr == TILE_DECODE_BLANK) {
      out = OUT_NOTILE;                    // every pixel transparent: write nothing
    } else if (dr != TILE_DECODE_OK) {
      snprintf(st->elevErr, sizeof(st->elevErr), "z%d %d/%d: %s", z, x, y, err);
      out = OUT_FAILED;
    } else {
      makeColumn(c, xi, dir);
      if (writeTileRetrying(j, "ELEV", path, tmpPath, c->px, z, x, y, err, sizeof(err))) {
        s_elevWritten++;                   // after the rename: the map may read it now
        out = OUT_DONE;
      } else {
        snprintf(st->elevErr, sizeof(st->elevErr), "z%d %d/%d: %s", z, x, y, err);
        out = OUT_FAILED;
      }
    }
  }
  uint32_t f, l, m;
  heapNow(&f, &l, &m);
  if (l < c->floorLargest) c->floorLargest = l;
  st->elapsedMs = millis() - j->startMs;
  waitSliced(j, out == OUT_FAILED ? TF_ELEV_FAIL_PACE_MS : TF_ELEV_PACE_MS, c->client, false);
  return out;
}

/* One elevation level over the job's square, centre-out like the map's (tile_plan.h), skipping
 * what is on the card. `fails` runs across both levels: TF_ELEV_MAX_FAILS in a row give the
 * layer up for this run. */
static bool walkElevLevel(Job* j, RunCtx* c, int z, int* fails) {
  TileJobStatus* st = &j->st;
  st->elevZ = z;
  TilePlanOrder o;
  tilePlanLevelOrder(&o, j->spec.lat, j->spec.lon, j->spec.radiusKm, z);
  const int n = 1 << z;
  c->colX0 = o.x0;
  c->cols = (uint8_t*)heap_caps_calloc((size_t)(o.w + 7) / 8, 1, MALLOC_CAP_SPIRAM);
  char dir[48], path[64], tmpPath[64];
  int idle = 0;
  int x, y;
  bool ended = false;                      // the level's last tile was visited
  for (;;) {
    if (s_stop || st->elevGaveUp) {
      break;
    }
    if ((s_pause || s_noWifi) && !waitReady(j, false, c->client)) {
      break;                               // before the card is touched: walkLevel's rule
    }
    if (!tilePlanOrderNext(&o, &x, &y)) {
      ended = true;
      break;
    }
    const int wx = ((x % n) + n) % n;
    snprintf(dir, sizeof(dir), "%s/%s/%d/%d", TILE_MAPS_ROOT, ELEV_DIR, z, wx);
    snprintf(path, sizeof(path), "%s/%d.elv", dir, y);
    snprintf(tmpPath, sizeof(tmpPath), "%s/%d.tmp", dir, y);
    if (tileOnCard(path)) {                // 131,072 bytes: the same test as a map tile
      st->elevSkipped++;
      if (++idle % 16 == 0) {
        vTaskDelay(1);
      }
      continue;
    }
    idle = 0;
    const int out = elevTile(j, c, z, wx, x, y, dir, path, tmpPath);
    if (out == OUT_STOPPED) {
      break;
    }
    if (out == OUT_FAILED) {
      st->elevFailed++;
      if (++*fails >= TF_ELEV_MAX_FAILS) {
        st->elevGaveUp = true;
        log_e("ELEV: %d failures in a row - the rest of the layer waits for the next run (%s)",
              *fails, st->elevErr);
      }
    } else {
      *fails = 0;
      if (out == OUT_DONE) {
        st->elevDone++;
      } else {
        st->elevNoTile++;
      }
    }
  }
  free(c->cols);
  c->cols = NULL;
  return ended;
}

/* The layer, both levels, on a TLS client of its own that is gone before the map tiles' is made
 * (the kept-alive socket must never carry a request to the other host). A sibling of runJob
 * under jobRun, not inside it: the two HTTPClients and their clients are never on the stack or
 * in the heap together. */
static void runElev(Job* j, uint8_t* body, uint16_t* px, uint32_t* floorLargest) {
  TileJobStatus* st = &j->st;
  st->elevRunning = true;
  WiFiClientSecure* secure = new WiFiClientSecure();
  secure->setInsecure();                   // the map sources' trust model (runJob)
  {
    HTTPClient http;
    http.setReuse(true);
    http.setTimeout(15000);
    http.setUserAgent(TF_USER_AGENT);
    RunCtx c;
    memset(&c, 0, sizeof(c));
    c.http = &http;
    c.client = secure;
    c.body = body;
    c.px = px;
    c.floorLargest = *floorLargest;
    int fails = 0;
    const bool fine = walkElevLevel(j, &c, ELEV_Z, &fails);        // the fine layer first, centre-out
    const bool coarse = walkElevLevel(j, &c, ELEV_Z_COARSE, &fails);
    /* Every tile of both levels visited and none failed: all on the card (or no tile there).
     * recSave() keeps it in the job record, so the next resume asks no card space for it. */
    st->elevWhole = fine && coarse && st->elevFailed == 0 && !st->elevGaveUp;
    http.end();
    *floorLargest = c.floorLargest;
  }                                        // ~HTTPClient stops its client HERE, while it still exists
  secure->stop();
  delete secure;
  st->elevRunning = false;
}

/* The map tiles of the run: both passes on the source's own client. `body`/`px` are jobRun's.
 * The HTTPClient lives in an inner block that closes BEFORE the client is deleted: its destructor
 * calls _client->stop(), and until 0.9.80 that ran on the client already deleted above it (a
 * WiFiClientSecure's stop() then zeroed mbedTLS contexts inside freed memory, at every run's end). */
static void runJob(Job* j, uint8_t* body, uint16_t* px, uint32_t* floorLargest) {
  TileJobStatus* st = &j->st;

  const bool https = !strncmp(j->url, "https", 5);
  WiFiClientSecure* secure = NULL;
  WiFiClient*       plain  = NULL;
  WiFiClient*       client = NULL;
  if (https) {
    secure = new WiFiClientSecure();
    secure->setInsecure();               // the same trust model as the uploader's /fetch
    client = secure;
  } else {
    plain = new WiFiClient();
    client = plain;
  }
  {
    HTTPClient http;
    http.setReuse(true);
    http.setTimeout(15000);
    http.setUserAgent(TF_USER_AGENT);

    RunCtx c;
    memset(&c, 0, sizeof(c));
    c.http = &http;
    c.client = client;
    c.body = body;
    c.px = px;
    c.floorLargest = *floorLargest;

    // Pass 1, from the cursor: every level, centre-out.
    if (j->startPass <= 1) {
      st->pass = 1;
      for (int z = j->startZ; z <= j->zMax && !s_stop; z++) {
        walkLevel(j, &c, 1, z, z == j->startZ ? j->startOrd : 0);
      }
    }
    /* THE ONE AUTOMATIC SECOND PASS. A run that reaches the end without being stopped, with
     * tiles that did not land (server hiccups over two days, a card that was busy five seconds),
     * looks at those again before it calls itself finished — otherwise the holes stay until
     * someone presses Start again, which after a multi-day run nobody does ({user}'s "I leave on
     * the trip with holes"). Once: a tile that fails twice is reported, not chased. */
    if (!s_stop) {
      int fromZ = TILE_ZOOM_BASE;
      int32_t fromOrd = 0;
      if (j->startPass == 2) {
        fromZ = j->startZ;
        fromOrd = j->startOrd;
      }
      st->p2Total = unsettledIn(j->levelBase[fromZ - TILE_ZOOM_BASE] + fromOrd, st->total);
      if (st->p2Total > 0) {
        st->pass = 2;
        for (int z = fromZ; z <= j->zMax && !s_stop; z++) {
          walkLevel(j, &c, 2, z, z == fromZ ? fromOrd : 0);
        }
      }
    }
    if (!s_stop) {
      st->stopReason = TILE_STOP_FINISHED;
    }
    http.end();
    *floorLargest = c.floorLargest;
  }                                        // ~HTTPClient runs here, on a client that still exists
  if (secure) {
    secure->stop();
    delete secure;
  }
  if (plain) {
    plain->stop();
    delete plain;
  }
}

/* The whole run: the elevation layer first when the job asks for it, then the map tiles. The two
 * PSRAM buffers every tile goes through - the body as it arrives, and the decoded tile (a map
 * tile's RGB565 or an elevation tile's int16 metres, 131,072 bytes either way) - are made once a
 * run, here, and shared by both. Everything with a destructor is torn down by the time
 * runElev/runJob return (the worker loops; its locals must not hold internal-heap buffers). */
static void jobRun(Job* j) {
  TileJobStatus* st = &j->st;
  j->startMs = millis();
  uint8_t*  body = (uint8_t*)heap_caps_malloc(TF_BODY_CAP, MALLOC_CAP_SPIRAM);
  uint16_t* px   = (uint16_t*)heap_caps_malloc(MAP_TILE_BYTES, MALLOC_CAP_SPIRAM);
  uint32_t floorLargest = 0xFFFFFFFFu;
  if (!body || !px) {
    stopWith(j, TILE_STOP_RAM, "no PSRAM for the tile buffers");
  } else {
    if (j->spec.elev && !s_stop) {
      runElev(j, body, px, &floorLargest);
    }
    if (!s_stop) {
      runJob(j, body, px, &floorLargest);
    }
  }
  free(body);
  free(px);
  st->heapFloorLargest = (floorLargest == 0xFFFFFFFFu) ? 0 : floorLargest;
  {
    uint32_t f, l, m;
    heapNow(&f, &l, &m);
    st->heapMinEver = m;
  }
  st->stackFloor = (uint32_t)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
  st->elapsedMs = millis() - j->startMs;
  if (st->stopReason == TILE_STOP_NONE) {
    /* Every stop names its reason (stopWith, tileFetchStop). One that did not is not
     * understood, and a job that is not understood is not resumed: treat it as the user's. */
    st->stopReason = TILE_STOP_USER;
  }
  st->stopping = false;
  st->paused = false;
  st->serverWaitSec = 0;
  st->waitingRam = false;
  st->elevRunning = false;
  st->finished = true;
  st->active = false;
}

static void benchRun();                   // below, with the bench

static void workerTask(void*) {
  for (;;) {
    xSemaphoreTake(s_go, portMAX_DELAY);
    if (s_kind == 1) {
      jobRun(s_job);
      s_runsEnded++;                      // the loop's tick settles it once s_busy is down
    } else if (s_kind == 2) {
      benchRun();
    }
    s_kind = 0;
    s_busy = false;                       // back on the semaphore: nothing else runs on this stack
  }
}

/* The worker, its stack and its semaphore, made on first use and kept. */
static bool ensureWorker(char* why, size_t whyCap) {
#if defined(configSUPPORT_STATIC_ALLOCATION) && configSUPPORT_STATIC_ALLOCATION
  if (!s_stack) {
    s_stack = (StackType_t*)heap_caps_malloc(TF_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_stack) {
      strlcpy(why, "no internal RAM for the download task", whyCap);
      return false;
    }
  }
#endif
  if (!s_go) {
    s_go = xSemaphoreCreateBinary();
    if (!s_go) {
      strlcpy(why, "no RAM for the download semaphore", whyCap);
      return false;
    }
  }
  if (!s_task) {
#if defined(configSUPPORT_STATIC_ALLOCATION) && configSUPPORT_STATIC_ALLOCATION
    s_task = xTaskCreateStaticPinnedToCore(workerTask, "tilefetch", TF_STACK_BYTES, NULL,
                                           1, s_stack, &s_tcb, 1);
#else
    BaseType_t created = xTaskCreatePinnedToCore(workerTask, "tilefetch", TF_STACK_BYTES,
                                                 NULL, 1, &s_task, 1);
    if (created != pdPASS) s_task = NULL;
#endif
    if (!s_task) {
      strlcpy(why, "could not create the download task", whyCap);
      return false;
    }
  }
  return true;
}

// ── the loop side: the persisted job and its resume (LOOP TASK ONLY) ───────────────────────
/* The persisted job: ONE NVS blob, so it is written — and would be torn — as a whole. Every
 * read and write of it is on the loop task (the tick, Start, Stop, Resume); the worker never
 * touches NVS, it only publishes the cursor. */
struct JobRec {
  uint8_t  ver;            // TF_REC_VER; anything else is dropped at boot
  uint8_t  pass;           // cursor: 1, or 2 = the second pass
  uint8_t  strikes;        // boot resumes not yet vouched for (tile_plan.h)
  uint8_t  giveUp;         // TileGiveUp
  uint8_t  reason;         // TileStopReason of the last stop; NONE = it was running
  uint8_t  zMax;           // the depth USED (already clamped to the source)
  uint16_t radiusKm;
  char     key[16];        // the source's KEY ("otm"), never its index
  char     ssid[33];       // the network it started on; it resumes only there
  /* 0.9.80: TF_REC_ELEV_ON = the job fetches the elevation layer too; TF_REC_ELEV_WHOLE = ...and
   * a run already had the whole layer on the card (the resume's space check leaves it out). It
   * sits in what was the padding before `lat`, so the record is the SAME SIZE and an older
   * firmware's record still loads (the getBytesLength check): those were written from a zeroed
   * record, so they read 0 - an old job resumes as it was started, without the layer. Anything
   * but ON/WHOLE is read as 0. */
  uint8_t  elev;
  double   lat, lon;
  int32_t  curZ, curOrd;   // cursor: the next tile to visit
  int32_t  written;        // tiles this job has written over all its runs (the resume's space check)
};
/* The 0.9.78 layout, to prove the byte above did not move anything (a size change would make
 * every waiting job "could not be read" after an upgrade, and drop it). */
struct JobRecV1Shape {
  uint8_t  ver, pass, strikes, giveUp, reason, zMax;
  uint16_t radiusKm;
  char     key[16];
  char     ssid[33];
  double   lat, lon;
  int32_t  curZ, curOrd;
  int32_t  written;
};
static_assert(sizeof(JobRec) == sizeof(JobRecV1Shape) && offsetof(JobRec, lat) == offsetof(JobRecV1Shape, lat),
              "JobRec.elev must live in the old padding: the persisted record's size is its version check");

static TileResume s_rs;                    // tile_plan.h's bookkeeping for the one job there can be
static JobRec     s_rec;                   // what NVS holds, when s_persist
static bool       s_persist = false;       // the running or waiting job is in NVS
static bool       s_loaded = false;        // NVS has been read (the first tick after boot)
static uint32_t   s_runsSettled = 0;       // runs whose end the loop has dealt with
static bool       s_ticked = false;
static uint32_t   s_tickMs = 0;
static bool       s_wifiWasUp = false;
static uint32_t   s_wifiUpSinceMs = 0;
static volatile bool s_callNow = false;    // tileFetchPause's call flag, this pass
static bool       s_callSeen = false;      // ...and when one was last seen (the resume's quiet minute)
static uint32_t   s_callLastMs = 0;

bool tileFetchCallRecent() {
  return s_callNow || (s_callSeen && (uint32_t)(millis() - s_callLastMs) < TILE_PLAN_CALL_QUIET_MS);
}
static char       s_ssidNow[33] = "";      // the network the phone is on (the tick copies it)
static TileFetchWorld s_world;             // the last world the tick saw
static int        s_gate = TILE_GATE_IDLE;
static uint32_t   s_gateWaitMs = 0;
static char       s_note[112] = "";        // a refused resume, or a dropped job: the form says it
static uint32_t   s_savedMs = 0;           // the cursor as last written to NVS...
static int        s_savedPass = 0, s_savedZ = 0;
static int32_t    s_savedReach = 0;        // ...as a job-wide tile index
static int32_t    s_writtenBase = 0;       // rec.written when this run started
static int32_t    s_doneSeen = 0;          // the running job's `done` the tick last saw

static void recSave() {
  /* A run that had the elevation layer whole says so in the record (every save goes through
   * here: the tick's cursor saves, a run's end, power-off). Only ever ON -> WHOLE: the layer
   * does not leave the card (the Files app refuses /maps/elev while the job runs or waits).
   * s_job is this record's job - a Start resets it before its first save. */
  if (s_rec.elev == TF_REC_ELEV_ON && s_job && s_job->spec.elev && s_job->st.elevWhole) {
    s_rec.elev = TF_REC_ELEV_WHOLE;
  }
  Preferences p;
  if (p.begin(TF_NVS_NS, false)) {
    p.putBytes(TF_NVS_JOB, &s_rec, sizeof(s_rec));
    p.end();
  }
}

static void recClear() {
  Preferences p;
  if (p.begin(TF_NVS_NS, false)) {
    if (p.isKey(TF_NVS_JOB)) {            // remove() of a missing key logs an error line
      p.remove(TF_NVS_JOB);
    }
    p.end();
  }
}

/* Forget the job for good, and say why on the form (NULL = say nothing: the user's Stop). */
static void dropJob(const char* why) {
  if (s_persist) {
    recClear();
  }
  s_persist = false;
  s_rs.pending = false;
  if (why) {
    strlcpy(s_note, why, sizeof(s_note));
    log_e("TILES: the waiting download was dropped - %s", why);
  }
}

/* The job-wide index of a cursor: the tiles of the levels before it, plus its ordinal. */
static int32_t reachOf(double lat, double lon, int radiusKm, int z, int32_t ord) {
  return (int32_t)tilePlanTiles(lat, lon, radiusKm, z - 1) + ord;
}

/* The first tick after boot: is a job waiting from before the restart? Anything unreadable or
 * out of range is dropped and SAID, never half-trusted: a wrong centre or depth would download
 * the wrong ground for days. */
static void loadPersisted(uint32_t now) {
  s_loaded = true;
  JobRec r;
  memset(&r, 0, sizeof(r));
  bool present = false, ok = false;
  Preferences p;
  if (!p.begin(TF_NVS_NS, true)) {
    return;
  }
  if (p.isKey(TF_NVS_JOB)) {
    present = true;
    ok = p.getBytesLength(TF_NVS_JOB) == sizeof(r) && p.getBytes(TF_NVS_JOB, &r, sizeof(r)) == sizeof(r);
  }
  p.end();
  if (!present) {
    return;
  }
  r.key[sizeof(r.key) - 1] = '\0';
  r.ssid[sizeof(r.ssid) - 1] = '\0';
  ok = ok && r.ver == TF_REC_VER && sourceByKey(r.key) >= 0 &&
       r.lat >= -85.0 && r.lat <= 85.0 && r.lon >= -180.0 && r.lon <= 180.0 &&
       r.radiusKm >= 1 && r.radiusKm <= 50 &&
       r.zMax >= TILE_ZOOM_BASE && r.zMax <= 19 && (r.pass == 1 || r.pass == 2) &&
       r.curZ >= TILE_ZOOM_BASE && r.curZ <= r.zMax && r.curOrd >= 0 && r.written >= 0;
  if (!ok) {
    s_persist = true;                      // so dropJob clears it
    dropJob("the saved download could not be read");
    return;
  }
  r.elev = (r.elev == TF_REC_ELEV_ON || r.elev == TF_REC_ELEV_WHOLE) ? r.elev : 0;
  s_rec = r;
  s_persist = true;
  tileResumeLoaded(&s_rs, r.strikes, r.giveUp, r.reason, now);
  log_e("TILES: a %s download (%d km to z%d, %d written) waits to resume on '%s'%s",
        r.key, (int)r.radiusKm, (int)r.zMax, (int)r.written, r.ssid,
        r.giveUp ? " - it gave up; Resume on the Download form" : "");
}

/* A run that ended: record why, and whether (and how) it waits to resume. */
static void settleEnded(uint32_t now) {
  if (s_runsSettled == s_runsEnded || s_busy) {
    return;                                // nothing new, or the worker is not back on its semaphore yet
  }
  s_runsSettled = s_runsEnded;
  if (!s_job) {
    return;
  }
  const int reason = s_job->st.stopReason;
  if (!s_persist) {
    s_rs.pending = false;                  // a custom-source job, or one the user already stopped
    return;
  }
  const Cursor cur = readCursor();
  if (cur.done > s_doneSeen) {
    s_doneSeen = cur.done;
    tileResumeProgress(&s_rs, cur.done, now);
  }
  tileResumeStopped(&s_rs, reason, now);
  if (!s_rs.pending) {
    dropJob(NULL);                         // finished: nothing left to resume
    return;
  }
  s_rec.pass = (uint8_t)cur.pass;
  s_rec.curZ = cur.z;
  s_rec.curOrd = cur.ord;
  s_rec.written = s_writtenBase + cur.done;
  s_rec.reason = (uint8_t)reason;
  s_rec.strikes = s_rs.strikes;
  s_rec.giveUp = s_rs.giveUp;
  recSave();
  log_e("TILES: the download stopped (%s) and waits to resume at z%d #%d, pass %d",
        tileStopReasonName(reason), cur.z, (int)cur.ord, cur.pass);
}

/* Start a run: a new job (`resume` false), or the persisted one from its cursor. `automatic`:
 * the loop's decision, not a person's press (only that can strike). *spaceRefused says the card
 * cannot hold it — the one refusal a retry cannot fix. */
static bool startJob(const TileJobSpec* s, bool resume, bool automatic, char* why, size_t whyCap,
                     bool* spaceRefused) {
  if (spaceRefused) *spaceRefused = false;
  if (!s_loaded) {
    loadPersisted(millis());       // see tileFetchStop: a new job must not be overwritten by an old one's record
  }
  if (!why || whyCap == 0) {
    return false;
  }
  why[0] = '\0';
  const uint32_t now = millis();
  settleEnded(now);
  if (s_busy) {
    strlcpy(why, "a download is already running", whyCap);
    return false;
  }
  const TileSource* src = tileSource(s ? s->source : -1);
  if (!src) {
    strlcpy(why, "no such source", whyCap);
    return false;
  }
  /* The spec comes from the console as well as the form: check it. A negative radius reads
   * as an antimeridian crossing to tilePlanRange() and becomes the whole world. */
  if (!(s->lat >= -85.0 && s->lat <= 85.0) || !(s->lon >= -180.0 && s->lon <= 180.0)) {
    strlcpy(why, "centre out of range", whyCap);
    return false;
  }
  if (s->radiusKm < 1 || s->radiusKm > 50) {
    strlcpy(why, "radius must be 1-50 km", whyCap);
    return false;
  }
  if (s->zMax < TILE_ZOOM_BASE || s->zMax > 19) {
    snprintf(why, whyCap, "detail must be z%d-z19", TILE_ZOOM_BASE);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    strlcpy(why, "not on WiFi", whyCap);
    return false;
  }
  /* The gates a resume waits for (tile_plan.h), asked of a Start too: a game owns the card and
   * the SPI bus; a Files folder job racing a download over /maps is the race 0.9.72 closed; and
   * a call's TLS-sized heap dip is the reason a download pauses for one at all. */
  if (gGbcActive) {
    strlcpy(why, "a game is running", whyCap);
    return false;
  }
  if (filesJobActive()) {
    strlcpy(why, "a Files folder job is running - let it finish", whyCap);
    return false;
  }
  if (s_callNow || (s_callSeen && (uint32_t)(now - s_callLastMs) < TILE_PLAN_CALL_QUIET_MS)) {
    strlcpy(why, "a call is on, or ended under a minute ago", whyCap);
    return false;
  }
  /* The start gate. Measured 2026-09-18: the first TLS handshake of a run dips the internal
   * heap by ~12 KB on top of the task's stack (10 KB, once), and a run that starts from a
   * heap already fragmented by a long session has reached a min-ever under 3 KB — the zone
   * where SIP cannot re-register and lwIP's listener dies. Refuse to begin rather than find
   * out mid-handshake; the words say what to do. */
  {
    /* The SAME bar the reconnect wait uses (waitReady: largest >= TF_CONNECT_LARGEST, free >=
     * TF_CONNECT_FREE), plus the stack if it has not been allocated yet — the stack is carved
     * out of the largest block and out of the free total, so both bars move up by its size.
     * ⚠ Calibrated on phone 2 first (largest ~26 KB idle) and it refused phone 1 outright: the
     * SIP phone idles at largest ~21.9 KB with a registration up, which is exactly enough. */
    uint32_t f, l, m;
    heapNow(&f, &l, &m);
    const uint32_t needL = TF_CONNECT_LARGEST + (s_stack ? 0u : (uint32_t)TF_STACK_BYTES);
    const uint32_t needF = TF_CONNECT_FREE + (s_stack ? 0u : (uint32_t)TF_STACK_BYTES);
    if (l < needL || f < needF) {
      snprintf(why, whyCap, "phone low on memory (%u KB block, %u KB free) - reboot first",
               (unsigned)(l / 1024), (unsigned)(f / 1024));
      return false;
    }
  }
  if (!tilePlanAreaWritable(sourceArea(src))) {
    strlcpy(why, "source folder name refused", whyCap);
    return false;
  }
  if (!s_job) {
    s_job = (Job*)heap_caps_calloc(1, sizeof(Job), MALLOC_CAP_SPIRAM);
    if (!s_job) {
      strlcpy(why, "no PSRAM for the job", whyCap);
      return false;
    }
  }
  if (!s_settled) {
    s_settled = (uint8_t*)heap_caps_calloc(TF_SETTLED_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (!s_settled) {
      strlcpy(why, "no PSRAM for the job", whyCap);
      return false;
    }
  }
  /* Every refusal below happens BEFORE the job record is touched, so a refused Start leaves
   * the previous run's "Last run: ..." summary on the form instead of wiping it. */
  int zMax = s->zMax > src->zMax ? src->zMax : s->zMax;
  if (zMax < TILE_ZOOM_BASE) zMax = TILE_ZOOM_BASE;
  const int64_t total64 = tilePlanTiles(s->lat, s->lon, s->radiusKm, zMax);
  if (total64 > TILE_JOB_MAX_TILES) {
    snprintf(why, whyCap, "%lld tiles is too many for one run (%d max)", (long long)total64, TILE_JOB_MAX_TILES);
    return false;
  }
  const int total = (int)total64;
  if (SD.totalBytes() == 0) {
    strlcpy(why, "no SD card", whyCap);
    return false;
  }
  {
    /* Room on the card for what is left to write — a fresh job assumes none of it is there (a
     * re-run over a half-done area asks for more than it needs: the honest direction to be
     * wrong in); a resume subtracts what this job has already written, so a 45 %-done area is
     * not refused for want of room it no longer needs — plus a margin the card keeps for
     * everything else on it. */
    const uint64_t cardTotal = SD.totalBytes();
    uint64_t margin = cardTotal / 50;
    if (margin < TF_SPACE_MARGIN_MIN) margin = TF_SPACE_MARGIN_MIN;
    int64_t left = total;
    if (resume) {
      /* The rest is what lies at or past the CURSOR (everything before it is settled — written,
       * already on the card, or no tile), capped by what this job has not written. `written`
       * alone counted the pooled tiles a job SKIPPED as still to come: a 20 km z17 job over a
       * card that already holds z11-16 asked for 1.6 GB it would never write (review,
       * 2026-09-23). */
      int64_t past = (int64_t)total - reachOf(s_rec.lat, s_rec.lon, s_rec.radiusKm, s_rec.curZ, s_rec.curOrd);
      if (past < 0) past = 0;
      left -= s_rec.written;
      if (left > past) left = past;
      if (left < 0) left = 0;
    }
    /* The elevation layer, when the job has it, in full: which of its tiles are on the card is
     * not known without a stat each, and this runs on the loop (the honest direction again) -
     * unless the record says a run already had it whole (tilePlanElevSpaceTiles). */
    left += tilePlanElevSpaceTiles(s->lat, s->lon, s->radiusKm, s->elev, resume,
                                   s_rec.elev == TF_REC_ELEV_WHOLE);
    const uint64_t need = tilePlanCardBytes(left);
    const uint64_t freeB = cardFreeBytes();
    if (freeB < need + margin) {
      snprintf(why, whyCap, "card has %u MB free; %s needs %u MB and %u MB spare",
               (unsigned)(freeB >> 20), resume ? "the rest" : "this area",
               (unsigned)(need >> 20), (unsigned)(margin >> 20));
      if (spaceRefused) *spaceRefused = true;
      return false;
    }
  }
  if (!ensureWorker(why, whyCap)) {
    return false;
  }
  // ── committed: nothing below refuses ──
  memset(s_job, 0, sizeof(Job));
  Job* j = s_job;
  j->spec = *s;
  j->spec.zMax = zMax;
  j->src = src;
  strlcpy(j->area, sourceArea(src), sizeof(j->area));
  strlcpy(j->url, src->url, sizeof(j->url));
  j->zMax = zMax;
  j->st.total = total;
  j->st.elev = s->elev;
  j->st.elevTotal = s->elev ? (int)tilePlanElevTiles(s->lat, s->lon, s->radiusKm) : 0;
  j->busyWaitMs = TF_BUSY_FIRST_MS;
  tilePlanRateReset(&j->rate);
  {
    int32_t base = 0;
    for (int z = TILE_ZOOM_BASE; z < TILE_ZOOM_BASE + TF_LEVELS; z++) {
      j->levelBase[z - TILE_ZOOM_BASE] = base;
      if (z <= zMax) {
        base += (int32_t)tilePlanLevelTiles(s->lat, s->lon, s->radiusKm, z);
      }
    }
  }
  if (resume) {
    j->startPass = s_rec.pass;
    j->startZ = s_rec.curZ;
    j->startOrd = s_rec.curOrd;
  } else {
    j->startPass = 1;
    j->startZ = TILE_ZOOM_BASE;
    j->startOrd = 0;
  }
  j->st.pass = j->startPass;
  j->st.before = j->startPass == 1 ? j->levelBase[j->startZ - TILE_ZOOM_BASE] + j->startOrd : 0;
  j->st.curZ = j->startZ;
  /* st.source is the FOLDER: the map on the screen compares it with its area to see its own
   * holes being filled (MapsApp::jobTilesLanded), so a relay into "otm" must say "otm". */
  strlcpy(j->st.source, j->area, sizeof(j->st.source));
  {
    /* The settled bits carry over only to a resume of the SAME job in the same boot. A custom
     * job's ident says so: its "no tile" (the relay's 404) must never mark a tile settled for a
     * later OpenTopoMap resume over the same square, whose second pass would then skip it. */
    char ident[80];
    snprintf(ident, sizeof(ident), "%s%s %.6f %.6f %d %d", src == &SOURCES[TILE_SRC_CUSTOM] ? "custom:" : "",
             j->area, s->lat, s->lon, s->radiusKm, zMax);
    if (!resume || strcmp(ident, s_settledFor) != 0) {
      memset(s_settled, 0, TF_SETTLED_BYTES);
      strlcpy(s_settledFor, ident, sizeof(s_settledFor));
    }
  }
  /* PERSIST BEFORE THE WORKER HAS IT. A custom-source job is never persisted: its URL lives in
   * RAM only, and after a restart the index would resolve to nothing (or, one day, to
   * something else). It replaces a waiting job all the same. */
  const bool persist = (src != &SOURCES[TILE_SRC_CUSTOM]);
  if (!resume) {
    tileResumeFresh(&s_rs, now);
    if (persist) {
      memset(&s_rec, 0, sizeof(s_rec));
      s_rec.ver = TF_REC_VER;
      s_rec.pass = 1;
      s_rec.zMax = (uint8_t)zMax;
      s_rec.radiusKm = (uint16_t)s->radiusKm;
      strlcpy(s_rec.key, src->key, sizeof(s_rec.key));
      strlcpy(s_rec.ssid, s_ssidNow, sizeof(s_rec.ssid));
      s_rec.lat = s->lat;
      s_rec.lon = s->lon;
      s_rec.curZ = TILE_ZOOM_BASE;
      s_rec.curOrd = 0;
      s_rec.written = 0;
      s_rec.elev = s->elev ? TF_REC_ELEV_ON : 0;
    }
  }
  if (persist) {
    /* 🛑 THE STRIKE IS ON RECORD BEFORE THE RUN EXISTS. A boot resume that takes the phone down
     * must already have been counted, or a crash loop never reaches three. */
    s_rec.strikes = tileResumeStarting(&s_rs, automatic);
    s_rec.giveUp = TILE_GIVEUP_NONE;
    s_rec.reason = TILE_STOP_NONE;
    recSave();
  } else {
    tileResumeStarting(&s_rs, false);
    if (s_persist) {
      recClear();
    }
  }
  s_persist = persist;
  s_writtenBase = resume ? s_rec.written : 0;
  s_doneSeen = 0;
  s_savedMs = now;
  s_savedPass = j->startPass;
  s_savedZ = j->startZ;
  s_savedReach = j->levelBase[j->startZ - TILE_ZOOM_BASE] + j->startOrd;
  s_note[0] = '\0';
  publishCursor(j->startPass, j->startZ, j->startOrd, 0);
  installHook();
  /* ⚠ s_pause and s_noWifi are NOT reset here: the loop owns them (tileFetchPause, every pass).
   * Clearing them let the worker start a handshake inside a call before the loop's next pass
   * said otherwise. */
  s_stop = false;
  s_busy = true;
  j->st.active = true;
  s_kind = 1;
  xSemaphoreGive(s_go);
  return true;
}

bool tileFetchStart(const TileJobSpec* s, char* why, size_t whyCap) {
  return startJob(s, false, false, why, whyCap, NULL);
}

/* The persisted job as a spec, or false (its source is gone: it is dropped). */
static bool recSpec(TileJobSpec* spec) {
  const int idx = sourceByKey(s_rec.key);
  if (idx < 0) {
    dropJob("the source it used is gone");
    return false;
  }
  memset(spec, 0, sizeof(*spec));
  spec->source = idx;
  spec->lat = s_rec.lat;
  spec->lon = s_rec.lon;
  spec->radiusKm = s_rec.radiusKm;
  spec->zMax = s_rec.zMax;
  spec->elev = s_rec.elev != 0;
  return true;
}

/* No room on the card for the rest. Waiting cannot make room, so it does not retry by itself —
 * but the job (its centre, days old, and its cursor) is KEPT: freeing space and pressing Resume
 * carries on where it stopped. Dropping it lost a multi-day job to one full card. */
static void spaceGiveUp(const char* why) {
  s_rs.pending = true;
  s_rs.giveUp = TILE_GIVEUP_SPACE;
  s_rec.giveUp = TILE_GIVEUP_SPACE;
  if (s_persist) {
    recSave();
  }
  snprintf(s_note, sizeof(s_note), "Not resumed: %s", why);
  log_e("TILES: resume refused for space - %s", why);
}

/* The loop's resume. A refusal cools down like a failure (never a retry every second); a card
 * with no room for the rest gives up and says so (spaceGiveUp). */
static void autoResume(uint32_t now) {
  TileJobSpec spec;
  if (!recSpec(&spec)) {
    return;
  }
  char why[96];
  bool space = false;
  if (startJob(&spec, true, true, why, sizeof(why), &space)) {
    log_e("TILES: resumed the %s download at z%d #%d (pass %d, %d written, strike %d)",
          s_rec.key, (int)s_rec.curZ, (int)s_rec.curOrd, (int)s_rec.pass, (int)s_rec.written, (int)s_rec.strikes);
    return;
  }
  if (space) {
    spaceGiveUp(why);
    return;
  }
  tileResumeRefused(&s_rs, now);
  snprintf(s_note, sizeof(s_note), "Not resumed: %s", why);
  log_e("TILES: resume refused - %s", why);
}

bool tileFetchResumeNow(char* why, size_t whyCap) {
  if (!why || whyCap == 0) {
    return false;
  }
  why[0] = '\0';
  if (!s_loaded) {
    loadPersisted(millis());
  }
  settleEnded(millis());
  if (s_busy) {
    strlcpy(why, "a download is already running", whyCap);
    return false;
  }
  if (!s_persist || !s_rs.pending) {
    strlcpy(why, "nothing is waiting to resume", whyCap);
    return false;
  }
  TileJobSpec spec;
  if (!recSpec(&spec)) {
    strlcpy(why, s_note, whyCap);
    return false;
  }
  /* A person pressed it, on this network: the job is tied to this network from now on. */
  if (s_ssidNow[0]) {
    strlcpy(s_rec.ssid, s_ssidNow, sizeof(s_rec.ssid));
  }
  bool space = false;
  if (startJob(&spec, true, false, why, whyCap, &space)) {
    return true;
  }
  if (space) {
    spaceGiveUp(why);
  }
  return false;
}

void tileFetchStop() {
  /* Every entry point sees the persisted job before acting on it: a `maps dl stop` handled on the
   * first loop pass after a boot (the serial port's open resets the phone) ran before the tick
   * had loaded it, "forgot" nothing, and the job resumed a minute later. */
  if (!s_loaded) {
    loadPersisted(millis());
  }
  settleEnded(millis());
  if (s_busy && s_job && s_kind == 1) {
    s_job->st.stopping = true;
    s_job->st.stopReason = TILE_STOP_USER;   // over any reason the worker meets on its way out
  }
  s_stop = true;
  /* 🛑 FORGET IT NOW, NOT WHEN THE WORKER GETS ROUND TO EXITING. That can be 20 s (a 15 s HTTP
   * timeout and the card's write retries); a phone switched off inside that window would find
   * the job in NVS at the next boot and start it again — a Stop that did not stop. */
  dropJob(NULL);
  s_note[0] = '\0';
}

void tileFetchPowerOff() {
  if (!s_loaded) {
    return;                        // nothing was ever read or started this boot: NVS is as it was
  }
  settleEnded(millis());           // a run that ended inside the last tick records its REAL reason
  if (!s_persist || !(s_busy && s_kind == 1 && s_job)) {
    return;                        // nothing running: a waiting job's record is already what it is
  }
  /* Without this a switched-off job looked exactly like a crashed one (reason NONE, a strike
   * pending): three short power cycles stopped auto-resume with "the phone restarted 3 times"
   * (review, 2026-09-23), and the cursor fell back to the last 500-tile save. */
  const Cursor cur = readCursor();
  s_rec.pass = (uint8_t)cur.pass;
  s_rec.curZ = cur.z;
  s_rec.curOrd = cur.ord;
  s_rec.written = s_writtenBase + cur.done;
  s_rec.reason = TILE_STOP_POWEROFF;
  s_rec.strikes = 0;
  recSave();
}

bool tileFetchActive() {
  return s_busy;
}

void tileFetchPause(bool call, bool game, bool noWifi) {
  s_pause = call || game;
  s_pauseGame = game;
  s_noWifi = noWifi;
  s_callNow = call;
  if (call) {
    s_callSeen = true;
    s_callLastMs = millis();
  }
}

/* The world as tile_plan.h's decision wants it. */
static TileWorld worldNow(uint32_t now) {
  TileWorld tw;
  memset(&tw, 0, sizeof(tw));
  tw.now = now;
  tw.wifiUp = s_world.wifiUp;
  tw.wifiUpMs = s_world.wifiUp ? (uint32_t)(now - s_wifiUpSinceMs) : 0;
  /* A job started with no network name known (it cannot say where it belongs) resumes on any. */
  tw.sameNet = !s_rec.ssid[0] || !strcmp(s_ssidNow, s_rec.ssid);
  tw.usb = s_world.usb;
  tw.volts = s_world.volts;
  tw.call = s_callNow;
  tw.sinceCallMs = s_callSeen ? (uint32_t)(now - s_callLastMs) : 0xFFFFFFFFu;
  tw.game = gGbcActive;
  tw.filesJob = s_world.filesJob;
  tw.card = s_world.card;
  return tw;
}

/* A running job, once a second: the battery, the strikes, the cursor. */
static void runningTick(uint32_t now) {
  Job* j = s_job;
  /* Off USB and under the floor (less the hysteresis): stop, and wait for USB. The phone's own
   * power-off at 3.30 V would otherwise end a multi-day job in the night, WiFi on at full speed,
   * with nothing to bring it back. */
  if (!s_world.usb && s_world.volts > 0.5f && s_world.volts < MAPS_DL_BATT_FLOOR - TILE_PLAN_BATT_HYST && !s_stop) {
    char msg[64];
    snprintf(msg, sizeof(msg), "battery %.2f V off USB - stopped", (double)s_world.volts);
    stopWith(j, TILE_STOP_BATTERY, msg);
    log_e("TILES: %s; it resumes on USB", msg);
  }
  if (!s_persist) {
    return;
  }
  /* On a different network from the job's: the WiFi rescue or the auto-switch moved the phone
   * while it ran, inside the 20 s a dropped network is waited out. A hotspot on a trip is
   * someone's data plan: stop, and wait for the job's own network exactly as a resume would. */
  if (s_world.wifiUp && s_ssidNow[0] && s_rec.ssid[0] && strcmp(s_ssidNow, s_rec.ssid) != 0 && !s_stop) {
    stopWith(j, TILE_STOP_NOWIFI, "on another WiFi network - stopped");
    log_e("TILES: the phone moved to '%s'; the download waits for '%s'", s_ssidNow, s_rec.ssid);
  }
  const Cursor cur = readCursor();
  bool strikesCleared = false;            // 50 new tiles into a boot resume: on record at once
  if (cur.done > s_doneSeen) {
    s_doneSeen = cur.done;
    strikesCleared = tileResumeProgress(&s_rs, cur.done, now);
  }
  const int32_t reach = j->levelBase[cur.z - TILE_ZOOM_BASE] + cur.ord;
  const bool due = strikesCleared || cur.pass != s_savedPass || cur.z != s_savedZ ||
                   reach - s_savedReach >= TF_SAVE_TILES ||
                   ((uint32_t)(now - s_savedMs) >= TF_SAVE_MS && reach != s_savedReach);
  if (!due) {
    return;
  }
  s_rec.pass = (uint8_t)cur.pass;
  s_rec.curZ = cur.z;
  s_rec.curOrd = cur.ord;
  s_rec.written = s_writtenBase + cur.done;
  s_rec.reason = TILE_STOP_NONE;
  s_rec.strikes = s_rs.strikes;
  recSave();
  s_savedMs = now;
  s_savedPass = cur.pass;
  s_savedZ = cur.z;
  s_savedReach = reach;
}

void tileFetchTick(const TileFetchWorld* w) {
  const uint32_t now = millis();
  if (w->wifiUp && !s_wifiWasUp) {
    s_wifiUpSinceMs = now;
  }
  s_wifiWasUp = w->wifiUp;
  if (s_ticked && (uint32_t)(now - s_tickMs) < TF_TICK_MS) {
    return;
  }
  s_ticked = true;
  s_tickMs = now;
  s_world = *w;
  if (w->wifiUp && w->ssid) {
    strlcpy(s_ssidNow, w->ssid, sizeof(s_ssidNow));
  } else {
    s_ssidNow[0] = '\0';
  }
  s_world.ssid = s_ssidNow;               // never keep the caller's pointer
  if (!s_loaded) {
    loadPersisted(now);
  }
  settleEnded(now);
  if (s_busy) {
    if (s_kind == 1 && s_job) {
      runningTick(now);
    }
    return;
  }
  if (!s_persist) {
    s_gate = TILE_GATE_IDLE;
    return;
  }
  TileWorld tw = worldNow(now);
  const uint8_t gaveUp = s_rs.giveUp;
  s_gate = tileResumeCheck(&s_rs, &tw, &s_gateWaitMs);
  if (s_rs.giveUp != gaveUp) {
    s_rec.giveUp = s_rs.giveUp;
    recSave();
    log_e("TILES: stopped resuming the %s download (%s)", s_rec.key,
          s_rs.giveUp == TILE_GIVEUP_STRIKES ? "the phone restarted 3 times" : "a day without a new tile");
  }
  if (s_gate == TILE_GATE_GO) {
    autoResume(now);
  }
}

/* The waiting job's one sentence for the form. */
static void gateWords(char* out, size_t cap) {
  const unsigned mins = (unsigned)((s_gateWaitMs + 59999u) / 60000u);
  switch (s_gate) {
  case TILE_GATE_GAVE_UP:
    if (s_rs.giveUp == TILE_GIVEUP_STRIKES) {
      strlcpy(out, "Stopped resuming: the phone restarted 3 times during this download", cap);
    } else if (s_rs.giveUp == TILE_GIVEUP_STALLED) {
      strlcpy(out, "Stopped resuming: a day of retries without one new tile", cap);
    } else if (s_rs.giveUp == TILE_GIVEUP_SPACE) {
      strlcpy(out, "Stopped: no room on the card for the rest - free some, then Resume", cap);
    } else {
      strlcpy(out, "Stopped: the card keeps refusing writes - it will not resume by itself", cap);
    }
    break;
  case TILE_GATE_COOL:
    snprintf(out, cap, "Retrying in %u min (%s)", mins,
             s_rs.refused ? "the last try was refused - see below" :
             (s_rs.reason == TILE_STOP_RAM ? "no memory to connect" :
              (s_rs.reason == TILE_STOP_DECODEFAILS ? "nothing decoded" : "the network gave up")));
    break;
  case TILE_GATE_CARD:  strlcpy(out, "Waiting for the SD card", cap); break;
  case TILE_GATE_FILES: strlcpy(out, "Waiting for the Files app's folder job to finish", cap); break;
  case TILE_GATE_GAME:  strlcpy(out, "Waiting for the game to end", cap); break;
  case TILE_GATE_CALL:  strlcpy(out, "Waiting: a call (it resumes a minute after)", cap); break;
  case TILE_GATE_POWER:
    snprintf(out, cap, "Waiting for USB power (battery %.2f V)", (double)s_world.volts);
    break;
  case TILE_GATE_WIFI:
    if (s_world.wifiUp) {
      snprintf(out, cap, "Resuming in %u s", (unsigned)((s_gateWaitMs + 999u) / 1000u));
    } else {
      strlcpy(out, "Waiting for WiFi", cap);
    }
    break;
  case TILE_GATE_NET:
    snprintf(out, cap, "Paused: resumes on %s, where it started", s_rec.ssid);
    break;
  case TILE_GATE_GO:    strlcpy(out, "Resuming...", cap); break;
  default:              out[0] = '\0'; break;
  }
}

void tileFetchJobInfo(TileFetchJobInfo* out) {
  memset(out, 0, sizeof(*out));
  if (!s_loaded) {
    loadPersisted(millis());
  }
  /* 🛑 SETTLE FIRST. A run that just ended is 'pending' again only once settleEnded has run, and
   * the tick does that at most once a second: the form, rebuilding on the running->stopped flip
   * inside that second, drew "no job" with Start under the cursor — one press replaced a
   * waiting multi-day job (review, 2026-09-23). One compare when there is nothing to settle. */
  settleEnded(millis());
  strlcpy(out->note, s_note, sizeof(out->note));
  const bool running = s_busy && s_kind == 1 && s_job;
  if (!running && !(s_persist && s_rs.pending)) {
    return;
  }
  out->exists = true;
  out->running = running;
  out->resumable = s_persist;
  out->gate = s_gate;
  out->giveUp = s_rs.giveUp;
  if (running) {
    const Job* j = s_job;
    strlcpy(out->label, j->src->label, sizeof(out->label));
    out->lat = j->spec.lat;
    out->lon = j->spec.lon;
    out->radiusKm = j->spec.radiusKm;
    out->zMax = j->zMax;
    out->total = j->st.total;
    const Cursor cur = readCursor();
    out->reached = cur.pass == 1 ? j->levelBase[cur.z - TILE_ZOOM_BASE] + cur.ord : j->st.total;
    out->elev = j->spec.elev;
  } else {
    const int idx = sourceByKey(s_rec.key);
    strlcpy(out->label, idx >= 0 ? SOURCES[idx].label : s_rec.key, sizeof(out->label));
    out->lat = s_rec.lat;
    out->lon = s_rec.lon;
    out->radiusKm = s_rec.radiusKm;
    out->zMax = s_rec.zMax;
    out->total = (int)tilePlanTiles(s_rec.lat, s_rec.lon, s_rec.radiusKm, s_rec.zMax);
    out->reached = s_rec.pass == 1 ? reachOf(s_rec.lat, s_rec.lon, s_rec.radiusKm, s_rec.curZ, s_rec.curOrd)
                                   : out->total;
    out->elev = s_rec.elev != 0;
    gateWords(out->why, sizeof(out->why));
  }
  if (s_persist) {
    strlcpy(out->ssid, s_rec.ssid, sizeof(out->ssid));
    out->otherNet = s_world.wifiUp && s_ssidNow[0] && s_rec.ssid[0] && strcmp(s_ssidNow, s_rec.ssid) != 0;
  }
}

const char* tileFetchJobKey(bool* running) {
  if (!s_loaded) {
    loadPersisted(millis());
  }
  settleEnded(millis());         // as tileFetchJobInfo: the Files app's guard must see a waiting job too
  if (s_busy && s_kind == 1 && s_job) {
    if (running) *running = true;
    return s_job->area;                   // the folder it writes: what the Files app guards
  }
  if (s_persist && s_rs.pending) {
    if (running) *running = false;
    return s_rec.key;
  }
  return NULL;
}

bool tileFetchJobElev() {
  if (!s_loaded) {
    loadPersisted(millis());
  }
  settleEnded(millis());
  if (s_busy && s_kind == 1 && s_job) {
    return s_job->spec.elev;
  }
  return s_persist && s_rs.pending && s_rec.elev != 0;
}

uint32_t tileFetchElevWritten() {
  return s_elevWritten;
}

void tileFetchStatus(TileJobStatus* out) {
  if (!out) {
    return;
  }
  if (!s_job) {
    memset(out, 0, sizeof(*out));
    return;
  }
  *out = s_job->st;
  if (out->active) {
    out->elapsedMs = millis() - s_job->startMs;
  }
}

/* The persisted job and where the resume rules stand, one line, in the caller's buffer: the
 * serial console runs on the loop task, whose 8 KB stack is already carrying the report. */
static void reportResume(void (*emit)(const char* line), char* l, size_t cap) {
  if (!s_persist) {
    snprintf(l, cap, "  resume: nothing kept%s%s", s_note[0] ? " | " : "", s_note);
    emit(l);
    return;
  }
  char words[112] = "";
  if (!s_busy) {
    gateWords(words, sizeof(words));
  }
  snprintf(l, cap, "  resume: %s %d km to z%d on '%s', cursor pass %d z%d #%d, %d written, strikes %d, last stop %s%s%s%s%s",
           s_rec.key, (int)s_rec.radiusKm, (int)s_rec.zMax, s_rec.ssid, (int)s_rec.pass, (int)s_rec.curZ,
           (int)s_rec.curOrd, (int)s_rec.written, (int)s_rs.strikes, tileStopReasonName(s_rs.reason),
           words[0] ? " | " : "", words, s_note[0] ? " | " : "", s_note);
  emit(l);
}

void tileFetchReport(void (*emit)(const char* line)) {
  char l[256];
  /* The status copy is ~400 bytes: PSRAM, made once, not the loop task's 8 KB stack (the console
   * runs on it, under the rest of the command's frames). */
  static TileJobStatus* s_rep = NULL;
  if (!s_rep) {
    s_rep = (TileJobStatus*)heap_caps_calloc(1, sizeof(TileJobStatus), MALLOC_CAP_SPIRAM);
    if (!s_rep) {
      emit("maps dl: no PSRAM for the report");
      return;
    }
  }
  TileJobStatus& st = *s_rep;
  tileFetchStatus(&st);
  if (!s_loaded) {
    loadPersisted(millis());
  }
  if (!s_job) {
    emit("maps dl: nothing run yet this boot");
    reportResume(emit, l, sizeof(l));
    return;
  }
  snprintf(l, sizeof(l), "maps dl: %s %s%s%s%s | %d/%d done, %d skipped, %d no-tile, %d failed, %d reconnects, z%d, pass %d%s%s",
           st.source, st.active ? "RUNNING" : (st.finished ? "finished" : "idle"),
           st.paused ? " (paused)" : "", st.waitingRam ? " (waiting for RAM)" : "",
           st.stopping ? " (stopping)" : "",
           st.done, st.total, st.skipped, st.noTile, st.failed, st.reconnects, st.curZ, st.pass,
           st.active ? "" : ", stopped: ", st.active ? "" : tileStopReasonName(st.stopReason));
  emit(l);
  if (st.waitingRam) {
    ramShortMsg(l, sizeof(l), &st, "  ");
    emit(l);
  }
  /* The depth USED, not the one asked for: `maps dl 0 ... 17` on USGS fetches to z16 and says so. */
  snprintf(l, sizeof(l), "  job: source %d, centre %.5f %.5f, %d km, z%d-%d; started past %d; %u ms/tile (last 200); server busy %d time%s%s",
           s_job->spec.source, s_job->spec.lat, s_job->spec.lon, s_job->spec.radiusKm,
           TILE_ZOOM_BASE, s_job->zMax, st.before, (unsigned)st.msPerTile,
           st.serverBusy, st.serverBusy == 1 ? "" : "s", st.serverWaitSec ? " (waiting now)" : "");
  emit(l);
  if (s_job->src == &SOURCES[TILE_SRC_CUSTOM]) {
    /* Which relay, into which map: the template is the owner's own LAN address, typed at this
     * same console - never a public server's key. */
    snprintf(l, sizeof(l), "  custom source %s into /maps/%s", s_job->url, s_job->area);
    emit(l);
  }
  if (st.pass == 2 || st.p2Total) {
    snprintf(l, sizeof(l), "  second pass: %d/%d looked at, %d tried again, %d fixed",
             st.p2Seen, st.p2Total, st.p2Tried, st.p2Fixed);
    emit(l);
  }
  /* The elevation layer: its own counts (never in the map's `failed`), and whether it is being
   * walked now. Seen = the tiles of both levels this run has looked at. */
  if (st.elev) {
    const int seen = st.elevDone + st.elevSkipped + st.elevNoTile + st.elevFailed;
    if (st.elevRunning) {
      snprintf(l, sizeof(l), "  elevation: RUNNING z%d, %d/%d seen - %d new, %d had, %d no-tile, %d failed%s%s",
               st.elevZ, seen, st.elevTotal, st.elevDone, st.elevSkipped, st.elevNoTile, st.elevFailed,
               st.elevErr[0] ? " | last: " : "", st.elevErr);
    } else {
      snprintf(l, sizeof(l), "  elevation: %d/%d seen - %d new, %d had, %d no-tile, %d failed%s%s%s",
               seen, st.elevTotal, st.elevDone, st.elevSkipped, st.elevNoTile, st.elevFailed,
               st.elevGaveUp ? ", gave up for this run" : "",
               st.elevErr[0] ? " | last: " : "", st.elevErr);
    }
    emit(l);
  } else {
    emit("  elevation: not part of this job");
  }
  snprintf(l, sizeof(l), "  %u KB down in %u s; card busy %d time%s; internal largest floor %u, min-ever %u; task stack floor %u of %u%s%s",
           (unsigned)(st.bytes / 1024), (unsigned)(st.elapsedMs / 1000),
           st.cardRetries, st.cardRetries == 1 ? "" : "s",
           (unsigned)st.heapFloorLargest, (unsigned)st.heapMinEver,
           (unsigned)st.stackFloor, (unsigned)TF_STACK_BYTES,
           st.lastErr[0] ? " | last: " : "", st.lastErr);
  emit(l);
  reportResume(emit, l, sizeof(l));
}

// ── the bench ───────────────────────────────────────────────────────────────────────────
struct BenchJob {
  char     url[512];
  int      count;
  int      done, failed, wrongMagic;
  uint32_t bytes;
  uint32_t tFirstMs, tSumMs, tMinMs, tMaxMs;
  uint32_t freeBefore, largestBefore, minEverBefore;
  uint32_t freeAfter, largestAfter, minEverDuring;
  uint32_t psramBefore, psramAfter;
  uint32_t stackFloor;
  int      lastCode;
  char     lastErr[80];
  bool     finished;
};
static BenchJob* s_bench = NULL;

static void runBench(BenchJob* j) {
  heapNow(&j->freeBefore, &j->largestBefore, &j->minEverBefore);
  j->psramBefore = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  j->tMinMs = 0xFFFFFFFFu;
  uint8_t* body = (uint8_t*)heap_caps_malloc(TF_BODY_CAP, MALLOC_CAP_SPIRAM);
  if (!body) {
    strlcpy(j->lastErr, "no PSRAM for the body", sizeof(j->lastErr));
    return;
  }
  const bool https = !strncmp(j->url, "https", 5);
  WiFiClientSecure* secure = NULL;
  WiFiClient*       plain  = NULL;
  WiFiClient*       client = NULL;
  if (https) {
    secure = new WiFiClientSecure();
    secure->setInsecure();
    client = secure;
  } else {
    plain = new WiFiClient();
    client = plain;
  }
  uint32_t minDuring = 0xFFFFFFFFu;
  {
    /* An inner block: ~HTTPClient calls stop() on its client, which must still exist (runJob). */
    HTTPClient http;
    http.setReuse(true);
    http.setTimeout(15000);
    http.setUserAgent(TF_USER_AGENT);
    for (int i = 0; i < j->count; i++) {
      const uint32_t t0 = millis();
      size_t got = 0;
      char err[64];
      const int code = fetchOne(http, *client, j->url, body, TF_BODY_CAP, &got, err, sizeof(err));
      j->lastCode = code;
      if (code != 200) {
        snprintf(j->lastErr, sizeof(j->lastErr), "%s", err);
        j->failed++;
      } else {
        const uint32_t dt = millis() - t0;
        if (i == 0) j->tFirstMs = dt;
        j->tSumMs += dt;
        if (dt < j->tMinMs) j->tMinMs = dt;
        if (dt > j->tMaxMs) j->tMaxMs = dt;
        j->bytes += (uint32_t)got;
        j->done++;
        const TileFormat f = tileSniff(body, got);
        if (f != TILE_FMT_JPEG && f != TILE_FMT_PNG) j->wrongMagic++;
      }
      uint32_t f, l, m;
      heapNow(&f, &l, &m);
      if (l < minDuring) minDuring = l;
      vTaskDelay(1);
    }
    j->minEverDuring = minDuring;
    http.end();
  }
  if (secure) { secure->stop(); delete secure; }
  if (plain)  { plain->stop();  delete plain; }
  free(body);
}

static void benchRun() {
  BenchJob* j = s_bench;
  runBench(j);                            // HTTPClient and its Strings destructed by the return
  j->stackFloor = (uint32_t)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
  heapNow(&j->freeAfter, &j->largestAfter, NULL);
  j->psramAfter = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  j->finished = true;
}

bool tileFetchBenchStart(const char* url, int count) {
  if (s_busy) {
    return false;
  }
  if (!s_bench) {
    s_bench = (BenchJob*)heap_caps_calloc(1, sizeof(BenchJob), MALLOC_CAP_SPIRAM);
    if (!s_bench) {
      return false;
    }
  }
  memset(s_bench, 0, sizeof(BenchJob));
  strlcpy(s_bench->url, url, sizeof(s_bench->url));
  s_bench->count = count < 1 ? 1 : count;
  char why[64];
  if (!ensureWorker(why, sizeof(why))) {
    strlcpy(s_bench->lastErr, why, sizeof(s_bench->lastErr));
    s_bench->finished = true;
    return false;
  }
  installHook();
  s_busy = true;
  s_kind = 2;
  xSemaphoreGive(s_go);
  return true;
}

void tileFetchBenchReport(void (*emit)(const char* line)) {
  char l[192];
  if (!s_bench) {
    emit("tlstest: nothing run yet. tlstest <url> [n]");
    return;
  }
  const BenchJob* j = s_bench;
  if (!j->finished) {
    snprintf(l, sizeof(l), "tlstest: RUNNING %d/%d done, %d failed, %u bytes so far",
             j->done, j->count, j->failed, (unsigned)j->bytes);
    emit(l);
    return;
  }
  const uint32_t avg = j->done ? j->tSumMs / (uint32_t)j->done : 0;
  snprintf(l, sizeof(l), "tlstest: %s", j->url);
  emit(l);
  snprintf(l, sizeof(l), "  %d/%d ok, %d failed, %d wrong-magic, %u bytes; last code %d %s",
           j->done, j->count, j->failed, j->wrongMagic, (unsigned)j->bytes, j->lastCode,
           j->lastErr[0] ? j->lastErr : "");
  emit(l);
  snprintf(l, sizeof(l), "  first GET %u ms (connect+handshake), then avg %u / min %u / max %u ms",
           (unsigned)j->tFirstMs, (unsigned)avg,
           (unsigned)(j->tMinMs == 0xFFFFFFFFu ? 0 : j->tMinMs), (unsigned)j->tMaxMs);
  emit(l);
  snprintf(l, sizeof(l), "  internal before free=%u largest=%u min-ever=%u",
           (unsigned)j->freeBefore, (unsigned)j->largestBefore, (unsigned)j->minEverBefore);
  emit(l);
  snprintf(l, sizeof(l), "  internal floor during: largest=%u | after free=%u largest=%u",
           (unsigned)(j->minEverDuring == 0xFFFFFFFFu ? 0 : j->minEverDuring),
           (unsigned)j->freeAfter, (unsigned)j->largestAfter);
  emit(l);
  snprintf(l, sizeof(l), "  psram before=%u after=%u (delta %d) | task stack floor %u of %u bytes",
           (unsigned)j->psramBefore, (unsigned)j->psramAfter,
           (int)j->psramAfter - (int)j->psramBefore,
           (unsigned)j->stackFloor, (unsigned)TF_STACK_BYTES);
  emit(l);
}
