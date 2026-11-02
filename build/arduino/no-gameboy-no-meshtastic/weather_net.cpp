/*
 * weather_net.cpp - see weather_net.h. The decisions (the URLs, the parsers, the fold, the policy,
 * the cache's form) are weather.cpp's and host-tested; this file is the card, the gates, the job
 * on the shared worker and the bench.
 */
#include "weather_net.h"
#include "weather.h"
#include "config.h"             // FIRMWARE_VERSION, for the User-Agent
#include "https_worker.h"       // the shared worker, netHttps
#include "isrg_roots.h"         // ISRG Root X1 + the self-signed Root YR
#include "gemini.h"             // GEM_NET_*: the worker's one vocabulary for a failed request
#include "ai_net.h"             // aiRequestActive: a weather fetch asked behind a question
#include "tile_fetch.h"         // tlsInstallPsramHook, tileFetchActive, tileFetchCallRecent, filesJobActive
#include "app_gbc_xfer.h"       // xferOn / xferWindowUp
#include "kosync_sync.h"        // kosyncWindowActive
#include "Networks.h"           // wifiState.radioOff()
#include "clock.h"              // ntpClock: the trusted stamp
#include "music_player.h"       // a Refresh pauses the music; an automatic fetch waits for it

#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>

/* NWS asks for a User-Agent naming the app (weather.h, WX_UA_CONTACT): the running version. */
#define WX_USER_AGENT "WiPhone/" FIRMWARE_VERSION " " WX_UA_CONTACT

extern volatile bool gGbcActive;          // WiPhone.ino: the emulator owns the card and turns WiFi off

/* Both hosts chain leaf <- YR1/YR2 <- Root YR (cross-signed by X1): isrg_roots.h has the why of both. */
static const char WX_ROOTS_PEM[] = ISRG_ROOT_X1_PEM ISRG_ROOT_YR_PEM;

#define WX_CONNECT_MS      10000           // TCP + the handshake, per host (the AI's cap)
#define WX_TIMEOUT_MS      20000           // the response headers (both answer in well under a second)
#define WX_BODY_IDLE_MS    15000u
#define WX_READ_MAX        (16u * 1024u)   // the cache file: ~2 KB of hours, ~1 KB of days, the alerts
#define WX_URL_MAX         640

// ── module state ─────────────────────────────────────────────────────────────────────────

/* The fetch in flight: written on the loop by wxRequest BEFORE the job is queued; the worker's
 * until s_done; the loop's again in wxLoopTick. PSRAM. */
struct WxReq {
  char    omUrl[WX_URL_MAX];
  char    nwsUrl[128];
  WxFetch fetch;                          // the place etc. filled on the loop, the answers on the worker
  bool    explicitAsk;
  bool    musicPaused;
};
/* One host's request, for `wx`. */
struct WxHostRes {
  int      code, net, tlsErr;
  uint32_t bytes, dnsMs, hsMs, ms;
  bool     overflow;
};
struct WxRes {
  WxHostRes om, nws;
  int       omParse;                      // WX_OM_* (-1 = not parsed: no 200/400 body)
  char      omReason[WX_REASON_MAX];
  int       nwsCounted;                   // the streaming counter's features
  char      skipped[48];                  // a gate closed on the worker: why the hosts left were not asked
  uint32_t  ms;
  uint32_t  stackFloor, largestFloor, minEver;
};

static WxReq*   s_q = NULL;
static WxRes*   s_r = NULL;
static WxData*  s_data = NULL;            // the cache (PSRAM)
/* The module's small state in PSRAM, not the internal .bss (internal RAM is what this phone runs
 * out of - ai_net.cpp's AiMem). */
struct WxMem {
  char           note[96];                // the last attempt's failure, for the screen
  WxEventCounter counter;                 // the NWS body's features, every byte (the worker's)
};
static WxMem*   s_m = NULL;
static volatile bool s_busy = false;      // queued or running on the worker
static volatile bool s_done = false;      // finished, not folded yet
static volatile bool s_cancel = false;    // nothing sets it today; the request honours it
static bool     s_haveRes = false;        // s_r holds a finished fetch (for `wx`)
static bool     s_tried = false;          // the card has been asked
static bool     s_loaded = false;         // ...and answered (a file, or no file)
static uint32_t s_gen = 1;
static bool     s_attempted = false;      // the policy's memory of this boot
static bool     s_lastFailed = false;
static uint32_t s_attemptMs = 0;          // stamped BEFORE the request
static bool     s_fetchedThisBoot = false;
static uint32_t s_fetchedMs = 0;          // the forecast's fold, on the boot clock (an age with no trusted clock)
static uint32_t s_fetches = 0, s_ok = 0, s_failed = 0;

static bool memOk() {
  if (!s_m) {
    s_m = (WxMem*)heap_caps_calloc(1, sizeof(WxMem), MALLOC_CAP_SPIRAM);
  }
  if (!s_data) {
    s_data = (WxData*)heap_caps_calloc(1, sizeof(WxData), MALLOC_CAP_SPIRAM);
    if (s_data) {
      wxDataClear(s_data);
    }
  }
  return s_data != NULL && s_m != NULL;
}

// ── the card ─────────────────────────────────────────────────────────────────────────────

/* ai_net.cpp's readSmall: retried once after 20 ms (the card shares the SPI bus with the screen,
 * and a failed open must never read as "no file"). -1 = unreadable. */
static int readSmall(const char* path, char* buf, size_t cap) {
  File f = SD.open(path, FILE_READ);
  if (!f) {
    delay(20);
    f = SD.open(path, FILE_READ);
  }
  if (!f) {
    return -1;
  }
  int n = f.read((uint8_t*)buf, cap - 1);
  f.close();
  if (n < 0) {
    return -1;
  }
  buf[n] = '\0';
  return n;
}

/* ai_net.cpp's writeFile: tmp, remove the old, rename (FAT cannot rename over a file). */
static bool writeFile(const char* path, const char* tmp, const char* data, size_t len) {
  if (!SD.exists(WX_DIR)) {
    SD.mkdir(WX_DIR);
  }
  if (SD.exists(tmp)) {
    SD.remove(tmp);                      // exists() first: remove() logs an error line for a missing file
  }
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) {
    return false;
  }
  const size_t w = f.write((const uint8_t*)data, len);
  f.close();
  if (w != len) {
    SD.remove(tmp);
    return false;
  }
  if (SD.exists(path)) {
    SD.remove(path);
  }
  return SD.rename(tmp, path);
}

void wxCacheLoadCard() {
  if (s_loaded || !memOk()) {
    return;
  }
  s_tried = true;
  /* weather.tmp when weather.txt is missing: a power cut between writeFile's remove and rename
   * leaves only the .tmp - whole, and the newest. exists() is an open with no retry: asked twice. */
  const char* path = NULL;
  for (int tries = 0; tries < 2 && !path; tries++) {
    if (tries) {
      delay(20);
    }
    path = SD.exists(WX_FILE) ? WX_FILE : (SD.exists(WX_TMP) ? WX_TMP : NULL);
  }
  if (!path) {
    s_loaded = true;                      // no cache yet
    s_gen++;
    return;
  }
  char* buf = (char*)heap_caps_malloc(WX_READ_MAX, MALLOC_CAP_SPIRAM);
  const int n = buf ? readSmall(path, buf, WX_READ_MAX) : -1;
  if (n < 0) {
    log_e("weather: %s unreadable just now - asked again later", path);
    free(buf);
    return;                               // not "no cache": read again on the next call
  }
  WxData* d = (WxData*)heap_caps_malloc(sizeof(WxData), MALLOC_CAP_SPIRAM);
  if (d && wxCacheLoad(buf, (size_t)n, d)) {
    *s_data = *d;
  } else if (d) {
    log_e("weather: %s is not a cache this firmware reads - set aside as %s", path, WX_BAD);
    if (SD.exists(WX_BAD)) {
      SD.remove(WX_BAD);
    }
    SD.rename(path, WX_BAD);
  }
  free(d);
  free(buf);
  s_loaded = true;
  s_gen++;
}

bool wxCacheTried() {
  return s_tried;
}

bool wxCacheLoaded() {
  return s_loaded;
}

const WxData* wxData() {
  /* Only once the card has ANSWERED: a read that failed is not "no weather yet" (review
   * 2026-10-03) - the screen keeps "Reading the weather..." until a later pass reads it. */
  return (s_loaded && s_data) ? s_data : NULL;
}

uint32_t wxGen() {
  return s_gen;
}

static void cacheSave() {
  if (!s_data) {
    return;
  }
  const size_t n = wxCacheSave(s_data, NULL, 0);
  char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (!buf) {
    log_e("weather: no memory to save the cache");
    return;
  }
  wxCacheSave(s_data, buf, n + 1);
  if (!writeFile(WX_FILE, WX_TMP, buf, n)) {
    log_e("weather: the card refused %s", WX_FILE);
  }
  free(buf);
}

void wxClear() {
  if (memOk()) {
    wxDataClear(s_data);
  }
  if (SD.exists(WX_FILE)) SD.remove(WX_FILE);
  if (SD.exists(WX_TMP))  SD.remove(WX_TMP);
  s_tried = s_loaded = true;              // the user's word: whatever the card held is gone
  if (s_m) s_m->note[0] = '\0';
  s_fetchedThisBoot = false;
  s_gen++;
}

// ── the policy's device facts ─────────────────────────────────────────────────────────────

/* The forecast's age: on a trusted clock from its stamp; else, when it was fetched this boot, on
 * the boot clock; else unknown (-1: an automatic fetch, given WiFi). */
static int64_t cacheAgeS() {
  if (!s_data || !s_data->have) {
    return -1;
  }
  const int64_t now = (int64_t)ntpClock.getTrustedUtcTime();
  if (now > 0 && s_data->fetchedUtc > 0) {
    return now >= s_data->fetchedUtc ? now - s_data->fetchedUtc : 0;
  }
  if (s_fetchedThisBoot) {
    return (int64_t)((uint32_t)(millis() - s_fetchedMs) / 1000u);
  }
  return -1;
}

static bool wifiUp() {
  return !wifiState.radioOff() && WiFi.status() == WL_CONNECTED;
}

static bool ownHotspot() {
  return (WiFi.getMode() & WIFI_MODE_AP) != 0;
}

int wxDecide(double lat, double lon, bool placeOk, bool explicitAsk) {
  WxDueIn in;
  memset(&in, 0, sizeof(in));
  in.explicitAsk = explicitAsk;
  in.wifiUp = wifiUp();
  in.hotspot = ownHotspot();
  in.placeOk = placeOk;
  in.busy = wxRequestActive();
  in.musicPlaying = musicPlayerIsPlaying();
  in.haveCache = s_data && s_data->have;
  in.cacheAgeS = cacheAgeS();
  in.movedKm = in.haveCache ? wxDistanceKm(lat, lon, s_data->latE2 / 100.0, s_data->lonE2 / 100.0) : -1;
  in.attempted = s_attempted;
  in.lastFailed = s_lastFailed;
  in.sinceAttemptMs = (uint32_t)(millis() - s_attemptMs);
  return wxFetchDue(&in);
}

/* aiAsk's gates, and its reasons (ai_net.cpp): a running download keeps a TLS session of its own
 * (two handshakes ~24 KB); the uploader and a sync window run the radio as a server; a call's
 * audio cannot share the handshake's heap dip; a game owns the card and the SPI bus; a Files
 * folder job is the card's too. NULL = clear. Cheap flag reads only: wxRequest asks at submit
 * time, and the WORKER asks again before each host - a fetch queued behind an AI ladder can
 * wait a minute or more, long enough for a call to ring in or a download to start (review
 * 2026-10-03). */
static const char* wxGateWhy() {
  if (wifiState.radioOff()) {
    return "WiFi is off (Settings > WiFi)";
  }
  if (WiFi.status() != WL_CONNECTED) {
    return "No WiFi - join a network first";
  }
  if (tileFetchActive()) {
    return "A map download is running";
  }
  if (xferOn()) {
    return "The WiFi uploader is on - stop it first";
  }
  if (kosyncWindowActive() || xferWindowUp()) {
    return "A book sync window is open";
  }
  if (tileFetchCallRecent()) {
    return "A call is on, or ended under a minute ago";
  }
  if (gGbcActive) {
    return "A game is running";
  }
  if (filesJobActive()) {
    return "A Files folder job is running - let it finish";
  }
  return NULL;
}

// ── the job (WORKER) ─────────────────────────────────────────────────────────────────────

static void tapCount(void* ctx, const char* p, size_t n) {
  wxCountFeed((WxEventCounter*)ctx, p, n);
}

static void hostRes(WxHostRes* h, const NetHttpRes& r) {
  h->code = r.code;
  h->net = r.net;
  h->tlsErr = r.tlsErr;
  h->bytes = (uint32_t)r.got;
  h->dnsMs = r.dnsMs;
  h->hsMs = r.hsMs;
  h->ms = r.ms;
  h->overflow = r.overflow;
}

static void wxGet(const char* host, const char* url, const char* accept, NetTapFn tap, void* tapCtx,
                  NetHttpRes* r) {
  NetHttpReq q;
  memset(&q, 0, sizeof(q));
  q.method = "GET";
  q.host = host;
  q.url = url;                            // ⚠ never logged: the place is in it
  q.rootsPem = WX_ROOTS_PEM;
  q.userAgent = WX_USER_AGENT;          // "WiPhone/<FIRMWARE_VERSION> (+<repo>)"
  q.hdrName[0] = "Accept";
  q.hdrValue[0] = accept;
  q.connectMs = WX_CONNECT_MS;
  q.timeoutMs = WX_TIMEOUT_MS;
  q.bodyIdleMs = WX_BODY_IDLE_MS;
  q.connectFirst = true;                  // the handshake timed alone (`wx` prints it per host)
  q.heapCheck = true;                     // the bar again before EACH handshake
  q.cancel = &s_cancel;
  q.sink = netWorkerSink();
  q.cap = NET_SINK_CAP;
  q.tap = tap;
  q.tapCtx = tapCtx;
  netHttps(&q, r);
}

static void wxJob() {
  WxReq* q = s_q;
  WxRes* r = s_r;
  const uint32_t t0 = millis();
  netHeapNow(NULL, &r->largestFloor, NULL);
  WxFetch& f = q->fetch;
  NetHttpRes h;
  int64_t date = 0;

  // 1. Open-Meteo: the forecast - unless a gate closed while the job waited behind the AI.
  const char* gate = wxGateWhy();
  if (gate) {
    memset(&h, 0, sizeof(h));
    h.net = GEM_NET_CONNECT;              // not asked: the fold keeps the old forecast
    strlcpy(r->skipped, gate, sizeof(r->skipped));
  } else {
    wxGet(WX_OM_HOST, q->omUrl, "application/json", NULL, NULL, &h);
  }
  hostRes(&r->om, h);
  if (h.largestAfterSend && h.largestAfterSend < r->largestFloor) r->largestFloor = h.largestAfterSend;
  if (h.date[0]) wxParseHttpDate(h.date, &date);
  r->omParse = -1;
  if ((h.code == 200 || h.code == 400) && !h.overflow) {
    r->omParse = wxParseOpenMeteo(netWorkerSink(), h.got, &f.fc, r->omReason, sizeof(r->omReason));
  }
  f.omParsed = h.code == 200 && r->omParse == WX_OM_OK;

  // 2. NWS: the alerts - unless there is no internet at all (the forecast's DNS failed), or a gate
  //    closed (again, or during the forecast's GET): not checked, the old alerts kept.
  f.nwsTried = true;
  if (!r->skipped[0] && (gate = wxGateWhy()) != NULL) {
    strlcpy(r->skipped, gate, sizeof(r->skipped));
  }
  if (r->skipped[0]) {
    memset(&f.al, 0, sizeof(f.al));
    f.al.state = WX_AL_FAILED;
    memset(&r->nws, 0, sizeof(r->nws));
    r->nws.net = GEM_NET_CONNECT;
  } else if (r->om.net == GEM_NET_DNS) {
    memset(&f.al, 0, sizeof(f.al));
    f.al.state = WX_AL_FAILED;
    r->nws = r->om;
  } else {
    wxCountInit(&s_m->counter);
    wxGet(WX_NWS_HOST, q->nwsUrl, "application/geo+json", tapCount, &s_m->counter, &h);
    hostRes(&r->nws, h);
    if (h.largestAfterSend && h.largestAfterSend < r->largestFloor) r->largestFloor = h.largestAfterSend;
    if (!date && h.date[0]) wxParseHttpDate(h.date, &date);
    r->nwsCounted = s_m->counter.count;
    wxParseNwsAlerts(h.code, netWorkerSink(), h.got, h.overflow, s_m->counter.count, &f.al);
  }
  f.dateUtc = date;
  r->ms = millis() - t0;
  r->stackFloor = netWorkerStackFloor();
  netHeapNow(NULL, NULL, &r->minEver);
  s_done = true;                          // before s_busy drops: wxRequestActive never blinks
  s_busy = false;
}

// ── the loop side ────────────────────────────────────────────────────────────────────────

bool wxRequestActive() {
  return s_busy || s_done;
}

const char* wxLastNote() {
  return s_m ? s_m->note : "";
}

/* "no internet (DNS)" - what a failed request was, in words (also the log's short word). */
static const char* netWords(int net, bool shortWord) {
  switch (net) {
  case GEM_NET_DNS:       return shortWord ? "dns" : "no internet (DNS)";
  case GEM_NET_CONNECT:   return shortWord ? "connect" : "no connection";
  case GEM_NET_TLS:       return shortWord ? "tls" : "certificate not trusted";
  case GEM_NET_TLS_OTHER: return shortWord ? "tls" : "secure connection failed";
  case GEM_NET_NOMEM:     return shortWord ? "nomem" : "phone low on memory";
  case GEM_NET_TIMEOUT:   return shortWord ? "timeout" : "no answer in time";
  case GEM_NET_LOST:      return shortWord ? "lost" : "connection lost";
  case GEM_NET_TOO_BIG:   return shortWord ? "toobig" : "answer too big";
  case GEM_NET_BAD_BODY:  return shortWord ? "badbody" : "garbled answer";
  default:                return shortWord ? "?" : "failed";
  }
}

void wxLoopTick() {
  /* ⚠ Not while a game runs: the fold writes the card, and the game's blit task owns the SPI bus
   * the card shares with the screen (aiLoopTick's rule). wxRequestActive() stays true meanwhile. */
  if (!s_done || gGbcActive) {
    return;
  }
  wxCacheLoadCard();
  WxFetch& f = s_q->fetch;
  /* A fold never writes over a cache it has not read (review 2026-10-03): when the card would not
   * answer, a NEW forecast still goes in (it is newer than whatever the file holds, and from then
   * on memory is the cache), but a fetch that brought no forecast is not folded at all - folding
   * it into the empty record and saving would put "no forecast" over the card's good one. */
  if (memOk() && (s_loaded || f.omParsed)) {
    if (wxFold(s_data, &f)) {
      cacheSave();
    }
    s_loaded = true;
  }
  if (f.omParsed) {
    s_fetchedThisBoot = true;
    s_fetchedMs = millis();
    if (s_m) s_m->note[0] = '\0';
    s_ok++;
  } else {
    s_failed++;
    if (s_r->skipped[0] && s_r->om.code == 0) {
      snprintf(s_m->note, sizeof(s_m->note), "Weather not updated: %s", s_r->skipped);
    } else if (s_r->om.code == 200) {
      strlcpy(s_m->note, "Weather not updated: the answer was not a forecast (a WiFi login page?)",
              sizeof(s_m->note));
    } else if (s_r->om.code > 0 && s_r->omParse == WX_OM_ERROR && s_r->omReason[0]) {
      snprintf(s_m->note, sizeof(s_m->note), "Weather not updated: Open-Meteo says %.60s", s_r->omReason);
    } else if (s_r->om.code > 0) {
      snprintf(s_m->note, sizeof(s_m->note), "Weather not updated: Open-Meteo answered HTTP %d", s_r->om.code);
    } else {
      snprintf(s_m->note, sizeof(s_m->note), "Weather not updated: %s", netWords(s_r->om.net, false));
    }
  }
  s_lastFailed = !f.omParsed;
  s_haveRes = true;
  /* ONE line a fetch - ⚠ never the URL or a coordinate: the codes, the time, a word. */
  if (f.omParsed) {
    log_e("weather: ok %d/%d %u.%u s", s_r->om.code, s_r->nws.code, (unsigned)(s_r->ms / 1000),
          (unsigned)((s_r->ms % 1000) / 100));
  } else if (s_r->skipped[0] && s_r->om.code == 0) {
    log_e("weather: skipped - %s", s_r->skipped);
  } else if (s_r->om.code > 0) {
    log_e("weather: failed http %d", s_r->om.code);
  } else {
    log_e("weather: failed %s", netWords(s_r->om.net, true));
  }
  s_gen++;
  s_cancel = false;
  s_done = false;
}

bool wxRequest(const WxAsk* a, bool explicitAsk, char* why, size_t whyCap) {
  if (!why || whyCap == 0 || !a) {
    return false;
  }
  why[0] = '\0';
  wxLoopTick();                           // a result still waiting goes into the cache first
  if (wxRequestActive()) {
    strlcpy(why, "Already fetching the weather", whyCap);
    return false;
  }
  if (!memOk()) {
    strlcpy(why, "No memory for the weather", whyCap);
    return false;
  }
  if (!s_q) s_q = (WxReq*)heap_caps_calloc(1, sizeof(WxReq), MALLOC_CAP_SPIRAM);
  if (!s_r) s_r = (WxRes*)heap_caps_calloc(1, sizeof(WxRes), MALLOC_CAP_SPIRAM);
  if (!s_q || !s_r) {
    strlcpy(why, "No memory for the weather", whyCap);
    return false;
  }
  const char* gate = wxGateWhy();          // aiAsk's gates (above)
  if (gate) {
    strlcpy(why, gate, whyCap);
    return false;
  }
  /* The heap bar - unless an AI question holds the worker (its handshake is dipping the heap right
   * now): then the worker checks it before each of the weather's handshakes anyway (heapCheck). */
  if (!aiRequestActive()) {
    uint32_t fr, lg;
    if (!netHeapOkForHandshake(&fr, &lg)) {
      snprintf(why, whyCap, "Phone low on memory (%u KB block, %u KB free) - reboot first",
               (unsigned)(lg / 1024), (unsigned)(fr / 1024));
      return false;
    }
  }
  if (!explicitAsk && musicPlayerIsPlaying()) {
    strlcpy(why, wxDueWhy(WX_DUE_MUSIC), whyCap);
    return false;
  }
  if (!netWorkerReady(why, whyCap)) {
    return false;
  }
  // The request: everything the worker reads, built now.
  memset(&s_q->fetch, 0, sizeof(s_q->fetch));
  WxFetch& f = s_q->fetch;
  f.latE2 = wxE2(a->lat);
  f.lonE2 = wxE2(a->lon);
  f.placeKind = a->placeKind;
  strlcpy(f.placeName, a->placeName ? a->placeName : "", sizeof(f.placeName));
  f.elevM = a->haveElev ? (int32_t)lround(a->elevM / 10.0) * 10 : WX_NO_ELEV;
  f.askedUtc = (int64_t)ntpClock.getTrustedUtcTime();   // 0 = not trusted: the Date header stands in
  wxOpenMeteoUrl(a->lat, a->lon, a->haveElev, a->elevM, s_q->omUrl, sizeof(s_q->omUrl));
  wxNwsUrl(a->lat, a->lon, s_q->nwsUrl, sizeof(s_q->nwsUrl));
  if (!s_q->omUrl[0] || !s_q->nwsUrl[0]) {
    strlcpy(why, "Could not build the request", whyCap);
    return false;
  }
  memset(s_r, 0, sizeof(*s_r));
  s_haveRes = false;
  s_q->explicitAsk = explicitAsk;
  /* A Refresh pauses the music, as an AI question does (aiAsk: the handshake beside a decoding track
   * failed on phone 1). Paused, not stopped: Music's Play carries on. An automatic fetch never
   * does - it waits for a quiet moment instead (above). */
  s_q->musicPaused = explicitAsk && musicPlayerIsPlaying();
  if (s_q->musicPaused) {
    musicPlayerPause();
  }
  tlsInstallPsramHook();                  // BEFORE the first WiFiClientSecure (tile_fetch.h)
  s_attempted = true;
  s_attemptMs = millis();                 // stamped BEFORE the request: a failure waits WX_RETRY_MS
  s_fetches++;
  if (s_m) s_m->note[0] = '\0';
  s_cancel = false;
  s_done = false;
  s_busy = true;
  s_gen++;
  netWorkerSubmit(NET_JOB_WX, wxJob);
  return true;
}

// ── the bench ────────────────────────────────────────────────────────────────────────────

static const char* hostWord(int st) {
  return st == WX_HOST_OK ? "ok" : st == WX_HOST_FAILED ? "not-checked" : "never";
}

static const char* alertWord(int st) {
  switch (st) {
  case WX_AL_OK:       return "ok";
  case WX_AL_NONE:     return "none";
  case WX_AL_OUTSIDE:  return "outside-US";
  case WX_AL_TOO_MANY: return "too-many";
  case WX_AL_FAILED:   return "not-checked";
  default:             return "unknown";
  }
}

static void hostLine(void (*emit)(const char*), const char* name, const WxHostRes& h) {
  char l[192];
  snprintf(l, sizeof(l), "    %s: HTTP %d, %s, %u B%s, dns %u ms, connect+handshake %u ms, all %u ms",
           name, h.code, h.net == GEM_NET_OK ? "net ok" : netWords(h.net, false), (unsigned)h.bytes,
           h.overflow ? " (past the 64 KB buffer)" : "", (unsigned)h.dnsMs, (unsigned)h.hsMs,
           (unsigned)h.ms);
  emit(l);
  if (h.tlsErr) {
    snprintf(l, sizeof(l), "      tls error -0x%04X", (unsigned)(-h.tlsErr));
    emit(l);
  }
}

void wxReport(void (*emit)(const char* line)) {
  char l[192];
  if (!memOk()) {
    emit("wx: no PSRAM for the weather's state");
    return;
  }
  wxCacheLoadCard();
  const WxData* d = s_data;
  if (d->have) {
    const int64_t age = cacheAgeS();
    char la[16], lo[16];
    snprintf(la, sizeof(la), "%.2f", d->latE2 / 100.0);
    snprintf(lo, sizeof(lo), "%.2f", d->lonE2 / 100.0);
    if (age >= 0) {
      snprintf(l, sizeof(l), "wx: forecast %lld min old (%s stamp) for %s, point %s,%s (rounded), elev %s%ld",
               (long long)(age / 60), d->trusted ? "trusted-clock" : "server-clock",
               d->placeName[0] ? d->placeName : "?", la, lo, d->elevM == WX_NO_ELEV ? "-" : "",
               d->elevM == WX_NO_ELEV ? 0L : (long)d->elevM);
    } else {
      snprintf(l, sizeof(l), "wx: forecast of UTC %lld (age unknown: no trusted clock) for %s, point %s,%s",
               (long long)d->fetchedUtc, d->placeName[0] ? d->placeName : "?", la, lo);
    }
    emit(l);
    snprintf(l, sizeof(l), "    %d hours, %d days, now %s, place offset %s%ld s",
             d->fc.nHours, d->fc.nDays, d->fc.haveNow ? "yes" : "no",
             d->fc.haveOffset ? "" : "-", d->fc.haveOffset ? (long)d->fc.utcOffsetS : 0L);
    emit(l);
  } else {
    emit(s_loaded ? "wx: no forecast cached (" WX_FILE ")" : "wx: the cache has not been read yet");
  }
  snprintf(l, sizeof(l), "    hosts: open-meteo %s, nws %s; alerts %s (%d listed, %d in force, %d in the body), as of UTC %lld",
           hostWord(d->omLast), hostWord(d->nwsLast), alertWord(d->al.state), d->al.n, d->al.kept,
           d->al.total, (long long)d->alertsUtc);
  emit(l);
  if (wxAlertsApart(d)) {
    // A forecast that failed after a move: the alerts are this place's, the forecast another's.
    snprintf(l, sizeof(l), "    alerts for point %.2f,%.2f (rounded) - not the forecast's", d->alLatE2 / 100.0,
             d->alLonE2 / 100.0);
    emit(l);
  }
  for (int i = 0; i < d->al.n; i++) {
    snprintf(l, sizeof(l), "      %s (sev %d, urg %d) onset %lld ends %lld expires %lld", d->al.a[i].event,
             d->al.a[i].severity, d->al.a[i].urgency, (long long)d->al.a[i].onset,
             (long long)d->al.a[i].ends, (long long)d->al.a[i].expires);
    emit(l);
    if (d->al.a[i].headline[0]) {
      snprintf(l, sizeof(l), "        %.170s", d->al.a[i].headline);
      emit(l);
    }
  }
  if (wxRequestActive()) {
    const int run = netWorkerRunning();
    snprintf(l, sizeof(l), "    NOW: fetching (%s, %u s since asked)%s",
             s_done ? "done, folding next pass" : (run == NET_JOB_WX ? "running" : "queued behind the AI"),
             (unsigned)((millis() - s_attemptMs) / 1000), s_q && s_q->musicPaused ? " - music paused" : "");
    emit(l);
  } else if (s_haveRes && s_r) {
    snprintf(l, sizeof(l), "    last fetch: %s in %u ms (%s)%s%s", s_q->fetch.omParsed ? "ok" : "FAILED",
             (unsigned)s_r->ms, s_q->explicitAsk ? "Refresh" : "automatic", s_m->note[0] ? " - " : "", s_m->note);
    emit(l);
    hostLine(emit, "open-meteo", s_r->om);
    hostLine(emit, "nws", s_r->nws);
    if (s_r->skipped[0]) {
      snprintf(l, sizeof(l), "    not asked (a gate closed on the worker): %s", s_r->skipped);
      emit(l);
    }
    if (s_r->omReason[0]) {
      snprintf(l, sizeof(l), "    open-meteo reason: %.150s", s_r->omReason);
      emit(l);
    }
    snprintf(l, sizeof(l), "    nws features counted %d | worker stack floor %u of %u | internal largest floor %u, min-ever %u",
             s_r->nwsCounted, (unsigned)s_r->stackFloor, (unsigned)NET_STACK_BYTES,
             (unsigned)s_r->largestFloor, (unsigned)s_r->minEver);
    emit(l);
  } else {
    emit("    nothing fetched since boot. wx fetch (WiFi, a place)");
  }
  if (s_attempted) {
    snprintf(l, sizeof(l), "    policy: last attempt %u s ago (%s); automatic: stale > %d min or moved > %.0f km, %u min after a failure",
             (unsigned)((millis() - s_attemptMs) / 1000), s_lastFailed ? "failed" : "ok",
             WX_STALE_S / 60, WX_MOVED_KM, (unsigned)(WX_RETRY_MS / 60000));
    emit(l);
  }
  uint32_t f, lg, m;
  netHeapNow(&f, &lg, &m);
  snprintf(l, sizeof(l), "    internal now free %u largest %u min-ever %u | fetches %u, ok %u, failed %u",
           (unsigned)f, (unsigned)lg, (unsigned)m, (unsigned)s_fetches, (unsigned)s_ok, (unsigned)s_failed);
  emit(l);
}
