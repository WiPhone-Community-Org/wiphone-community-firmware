/*
 * geoloc_net.cpp - see geoloc_net.h. The decisions are geoloc.cpp's and self_pos.cpp's (host-tested
 * by tests/test_geoloc.cpp and tests/test_selfpos.cpp); this file is the NVS setting, the scan copy,
 * the job on the shared worker, the fold on the loop and the bench. tests/check_geoloc_review.py
 * pins what it may not do (log a MAC or a coordinate, leave the IP fallback on, skip the roots).
 */
#include "geoloc_net.h"
#include "geoloc.h"
#include "self_pos.h"
#include "meshtastic_service.h"   // the Wi-Fi slot (netSlot), the beacon's state, placesNews
#include "https_worker.h"         // the shared worker, netHttps, the heap bar
#include "gemini.h"               // GEM_NET_*: the worker's one vocabulary for a failed request
#include "ai_net.h"               // aiRequestActive: a look queued behind a question
#include "weather_net.h"          // wxRequestActive: ...or behind a weather fetch
#include "tile_fetch.h"           // tlsInstallPsramHook, tileFetchActive, tileFetchCallRecent, filesJobActive
#include "app_gbc_xfer.h"         // xferOn / xferWindowUp
#include "kosync_sync.h"          // kosyncWindowActive
#include "Networks.h"             // wifiState (radioOff, requestGeoScan), lastWifiLinkUpMs
#include "phone_pos.h"            // phonePosGatewayServedNmea: a phone's hotspot is left out
#include "clock.h"                // ntpClock: the trusted stamp of a scan (asked, never set)
#include "music_player.h"         // an automatic look waits for the music; `netloc now` pauses it

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <string.h>

extern volatile bool gGbcActive;          // WiPhone.ino: the emulator owns the card and turns WiFi off

#define GEO_CONNECT_MS     10000          // TCP + the handshake, per half (the weather's cap)
#define GEO_TIMEOUT_MS     20000          // the response headers (beaconDB answers in well under a second)
#define GEO_BODY_IDLE_MS   15000u
#define GEO_TICK_MS         1000u         // the decisions, at most this often
/* A look waits this long after a GOT_IP: on a phone's hotspot the app answers the NMEA probe within
 * a few seconds (phone_pos.cpp), and only then is the hotspot's own BSSID known to travel with the
 * phone (phonePosGatewayServedNmea) and left out of the request. */
#define GEO_JOIN_SETTLE_MS 15000u

// ── module state ─────────────────────────────────────────────────────────────────────────

/* One half's request, for `netloc` (never its body: that is a list of MACs). */
struct GeoHalfRes {
  int      code, net, tlsErr;
  uint32_t bytes, dnsMs, hsMs, ms;
  bool     asked;
};
/* Everything this module keeps, in PSRAM (internal RAM is what this phone runs out of - the
 * weather's WxMem), allocated the first time the setting is not off and kept. ~3.9 KB. */
struct GeoMem {
  GeoSched   sched;
  GeoScan    scan;                        // the latest snapshot (MACs: RAM only, never logged)
  GeoSplit   split;                       // ...its split, computed with it
  GeoSplit   job;                         // the split the look in flight asked about (the fold judges it)
  char       bodyA[GEO_REQUEST_CAP];      // the worker's request bodies, built on the loop
  char       bodyB[GEO_REQUEST_CAP];
  GeoHalf    ha, hb;                      // the worker's answers, folded on the loop
  GeoHalfRes ra, rb;                      // ...and how each request went (`netloc`)
  char       skipped[48];                 // a gate closed on the worker: why a half was not asked
  uint32_t   jobMs, stackFloor, largestFloor, minEver;
};

static GeoMem*  s_m = NULL;
/* Read on the WORKER too (geoJob's gate before each half): switched off is checked there. */
static volatile int s_setting = GEO_SET_OFF;
/* "map + mesh" chosen on My node, not yet committed (geolocSetSetting, geolocScreenLeft): it
 * reads as "map only" for everything that could send or that "map + mesh" alone would want. */
static bool     s_meshPending = false;
static volatile bool s_busy = false;      // queued or running on the worker
static volatile bool s_done = false;      // finished, not folded yet
/* Set when Wi-Fi location is switched OFF with a look queued or out: the worker skips every half
 * not yet sent, the request in flight honours it (q.cancel), and the fold keeps nothing. */
static volatile bool s_cancel = false;
static bool     s_haveJob = false;        // s_m->ra/rb describe a finished look (`netloc`)
static bool     s_jobAsked = false;       // the look in flight was `netloc now`
static bool     s_musicPaused = false;    // ...and paused the music
static uint32_t s_jobStartMs = 0;
static uint32_t s_viewedMs = 0;           // Maps or the Almanac last on a lit screen (0 = never)
static uint32_t s_tickMs = 0;
static int      s_lastDue = GEO_DUE_OFF;
static uint32_t s_scans = 0, s_looks = 0, s_located = 0, s_moved = 0, s_scanAsks = 0;

// The setting as the air sees it: a pending "map + mesh" is still "map only" (s_meshPending).
static int effSetting() {
  return (s_setting == GEO_SET_MESH && s_meshPending) ? GEO_SET_MAP : (int)s_setting;
}

static bool memOk() {
  if (!s_m) {
    s_m = (GeoMem*)heap_caps_calloc(1, sizeof(GeoMem), MALLOC_CAP_SPIRAM);
    if (s_m) {
      geoSchedInit(&s_m->sched);
    }
  }
  return s_m != NULL;
}

// ── the setting ──────────────────────────────────────────────────────────────────────────

void geolocSetup() {
  Preferences p;
  p.begin("wpmesh", true);
  s_setting = geoSettingSane(p.getUChar("netloc", GEO_SET_OFF));   // absent = OFF: privacy first
  p.end();
  s_meshPending = false;                   // NVS holds only a committed choice (geolocScreenLeft)
  meshService.setNetLocSetting(s_setting);
  if (s_setting != GEO_SET_OFF) {
    memOk();
    log_e("WI-FI LOCATION: %s (sends nearby Wi-Fi IDs to beaconDB when a position is wanted)",
          geoSettingName(s_setting));
  }
}

int geolocSetting() {
  return s_setting;
}

static void persistSetting(int v) {
  Preferences p;
  p.begin("wpmesh", false);
  p.putUChar("netloc", (uint8_t)v);
  p.end();
}

void geolocSetSetting(int v) {
  v = geoSettingSane(v);
  if (v == GEO_SET_MESH && !(s_setting == GEO_SET_MESH && !s_meshPending)) {
    /* Chosen on My node: the row says "map + mesh" now, but nothing goes on the air until the
     * screen is left (geolocScreenLeft) - the next tap of the same row, on its way to "off",
     * must not be preceded by a beacon. NVS and the slot stay "map only" until then. */
    s_setting = GEO_SET_MESH;
    s_meshPending = true;
    persistSetting(GEO_SET_MAP);
    meshService.setNetLocSetting(GEO_SET_MAP);
    memOk();
    log_e("WI-FI LOCATION map + mesh chosen - starts when the My node screen is left");
    return;
  }
  s_meshPending = false;
  s_setting = v;
  persistSetting(v);
  meshService.setNetLocSetting(v);         // OFF clears the fix (the slot's own rule)
  if (v == GEO_SET_OFF) {
    /* A look queued behind an AI question can wait a minute: the owner's "no" reaches it before
     * its next half (geoJob's gate), mid-request (q.cancel), and at the fold (nothing kept). */
    if (geoRequestActive()) {
      s_cancel = true;
    }
    if (s_m) {
      /* Off means off: the access points last heard are not kept either, and the next "on"
       * starts its schedule over - EXCEPT the hour's cap and the 10-minute gap, so switching it
       * off and on again never buys beaconDB more lookups than the rules allow. */
      memset(&s_m->scan, 0, sizeof(s_m->scan));
      memset(&s_m->split, 0, sizeof(s_m->split));
      const GeoSched keep = s_m->sched;
      geoSchedInit(&s_m->sched);
      memcpy(s_m->sched.attemptMs, keep.attemptMs, sizeof(keep.attemptMs));   // ...but not the cap
      s_m->sched.lastLookMs = keep.lastLookMs;                                // ...nor the gap
    }
  } else {
    memOk();
  }
  log_e("WI-FI LOCATION %s", geoSettingName(v));
}

void geolocScreenLeft() {
  if (!s_meshPending || s_setting != GEO_SET_MESH) {
    return;
  }
  s_meshPending = false;
  persistSetting(GEO_SET_MESH);
  meshService.setNetLocSetting(GEO_SET_MESH);   // the slot may now send, under its own rules
  log_e("WI-FI LOCATION map + mesh");
}

bool geolocScanMayStart() {
  return !netRequestActive();
}

// ── the scan copy ────────────────────────────────────────────────────────────────────────

/* The access point this phone is JOINED to is left out, with its whole device, when it is a
 * phone's hotspot (its gateway served NMEA on this association - sticky, phone_pos.h) or its
 * BSSID is locally administered: it travels with whoever carries it (geoJoinedLeftOut). Read from
 * the driver, no String. Returns `buf` (filled) or NULL. Asked when a scan is copied AND again
 * when a look starts: the NMEA may have arrived in between. */
static const uint8_t* joinedLeftOut(uint8_t buf[6]) {
  wifi_ap_record_t info;
  if (WiFi.status() == WL_CONNECTED && esp_wifi_sta_get_ap_info(&info) == ESP_OK &&
      geoJoinedLeftOut(info.bssid, phonePosGatewayServedNmea())) {
    memcpy(buf, info.bssid, 6);
    return buf;
  }
  return NULL;
}

void geolocOnScan(int n) {
  if (s_setting == GEO_SET_OFF || n <= 0 || !memOk()) {
    return;                                // off: not one MAC is copied anywhere
  }
  const uint32_t now = millis();
  GeoScan* sc = &s_m->scan;
  geoScanBegin(sc, now, ntpClock.getTrustedUtcTime(), lastWifiLinkUpMs());
  for (int i = 0; i < n; i++) {
    const wifi_ap_record_t* r = (const wifi_ap_record_t*)WiFi.getScanInfoByIndex(i);
    if (!r) {
      continue;
    }
    geoScanAdd(sc, r->bssid, r->rssi, geoSsidFlags(r->ssid, sizeof(r->ssid)));
  }
  uint8_t joined[6];
  geoSplitScan(sc, joinedLeftOut(joined), &s_m->split);
  s_scans++;
  const int r = geoOnScan(&s_m->sched, sc, s_m->split.devices, meshService.netSlot());
  if (r == GEO_SCAN_MOVED) {
    s_moved++;
    meshService.notePlacesChanged();       // an open map takes the ring off now
    log_e("WI-FI LOCATION: the access points behind the fix are no longer heard - fix dropped");
  } else if (r == GEO_SCAN_BACK) {
    meshService.notePlacesChanged();
    log_e("WI-FI LOCATION: a scan still hears the access points behind the fix - shown again");
  }
}

// ── the gates (the weather's, weather_net.cpp's wxGateWhy) ──────────────────────────────

static bool wifiUp() {
  return !wifiState.radioOff() && WiFi.status() == WL_CONNECTED;
}

/* NULL = clear. Cheap flag reads: asked on the loop before a look and on the WORKER before each
 * half (a look queued behind an AI question can wait a minute - long enough for a call to ring
 * in or a download to start). */
static const char* geoGateWhy() {
  if (wifiState.radioOff()) {
    return "WiFi is off";
  }
  if (WiFi.status() != WL_CONNECTED) {
    return "not on WiFi";
  }
  if ((WiFi.getMode() & WIFI_MODE_AP) != 0) {
    return "this phone's own hotspot is up";
  }
  if (tileFetchActive()) {
    return "a map download is running";
  }
  if (xferOn()) {
    return "the WiFi uploader is on";
  }
  if (kosyncWindowActive() || xferWindowUp()) {
    return "a book sync window is open";
  }
  if (tileFetchCallRecent()) {
    return "a call is on, or ended under a minute ago";
  }
  if (gGbcActive) {
    return "a game is running";
  }
  if (filesJobActive()) {
    return "a Files folder job is running";
  }
  return NULL;
}

// ── the job (WORKER) ─────────────────────────────────────────────────────────────────────

static void post(const char* body, GeoHalf* h, GeoHalfRes* res) {
  NetHttpReq q;
  memset(&q, 0, sizeof(q));
  q.method = "POST";
  q.host = GEO_HOST;
  q.url = GEO_URL;
  q.rootsPem = GEO_ROOTS_PEM;              // ISRG X1 + X2 + Root YE (geoloc.h, isrg_roots.h)
  q.userAgent = GEO_USER_AGENT;            // generic: nothing that tells one WiPhone from another
  q.hdrName[0] = "Content-Type";           // actix-web answers 400 without it (review item 6)
  q.hdrValue[0] = GEO_CONTENT_TYPE;
  q.body = (const uint8_t*)body;           // ⚠ never logged: it is a list of MACs
  q.bodyLen = strlen(body);
  q.connectMs = GEO_CONNECT_MS;
  q.timeoutMs = GEO_TIMEOUT_MS;
  q.bodyIdleMs = GEO_BODY_IDLE_MS;
  q.connectFirst = true;                   // the handshake timed alone (`netloc` prints it)
  q.heapCheck = true;                      // the bar again before EACH handshake
  q.cancel = &s_cancel;
  q.sink = netWorkerSink();
  q.cap = GEO_RESPONSE_CAP;
  NetHttpRes r;
  netHttps(&q, &r);
  res->asked = true;
  res->code = r.code;
  res->net = r.net;
  res->tlsErr = r.tlsErr;
  res->bytes = (uint32_t)r.got;
  res->dnsMs = r.dnsMs;
  res->hsMs = r.hsMs;
  res->ms = r.ms;
  if (r.largestAfterSend && r.largestAfterSend < s_m->largestFloor) {
    s_m->largestFloor = r.largestAfterSend;
  }
  /* What beaconDB saw: an HTTP answer (whatever it said), a request that never got one (DNS,
   * connect, TLS, a timeout - counted as an attempt: it may have reached them), or a request
   * that never left for want of RAM (not counted). A body past the cap is not parsed. */
  int reply;
  if (r.code > 0) {
    reply = GEO_R_ANSWERED;
  } else if (r.net == GEM_NET_NOMEM) {
    reply = GEO_R_NO_MEMORY;
  } else {
    reply = GEO_R_NO_ANSWER;
  }
  geoHalfFrom(h, reply, r.code, netWorkerSink(), r.overflow ? 0 : r.got);
}

/* The worker's gate before each half: the owner's "off" first (a look can wait a minute behind an
 * AI question, and consent withdrawn meanwhile is withdrawn), then the weather's gates. */
static const char* jobGateWhy() {
  if (s_setting == GEO_SET_OFF || s_cancel) {
    return "Wi-Fi location switched off";
  }
  return geoGateWhy();
}

static void geoJob() {
  GeoMem* m = s_m;
  const uint32_t t0 = millis();
  netHeapNow(NULL, &m->largestFloor, NULL);
  m->skipped[0] = '\0';
  memset(&m->ha, 0, sizeof(m->ha));        // GEO_R_NOT_ASKED until asked
  memset(&m->hb, 0, sizeof(m->hb));
  memset(&m->ra, 0, sizeof(m->ra));
  memset(&m->rb, 0, sizeof(m->rb));
  // Half A - unless switched off, or a gate closed while the look waited behind the AI or the weather.
  const char* gate = jobGateWhy();
  if (gate) {
    strlcpy(m->skipped, gate, sizeof(m->skipped));
  } else {
    post(m->bodyA, &m->ha, &m->ra);
  }
  // Half B only when A is a tight 200 (otherwise A's failure is the verdict) - and the gates again.
  if (!m->skipped[0] && geoSecondHalfNeeded(&m->ha)) {
    gate = jobGateWhy();
    if (gate) {
      strlcpy(m->skipped, gate, sizeof(m->skipped));
    } else {
      post(m->bodyB, &m->hb, &m->rb);
    }
  }
  m->jobMs = millis() - t0;
  m->stackFloor = netWorkerStackFloor();
  netHeapNow(NULL, NULL, &m->minEver);
  s_done = true;                           // before s_busy drops: geoRequestActive never blinks
  s_busy = false;
}

bool geoRequestActive() {
  return s_busy || s_done;
}

// ── the loop side ────────────────────────────────────────────────────────────────────────

/* "no internet (DNS)" - a failed request in words (weather_net.cpp's netWords, short form). */
static const char* netWord(int net) {
  switch (net) {
  case GEM_NET_OK:        return "ok";
  case GEM_NET_DNS:       return "dns";
  case GEM_NET_CONNECT:   return "connect";
  case GEM_NET_TLS:       return "tls-verify";
  case GEM_NET_TLS_OTHER: return "tls";
  case GEM_NET_NOMEM:     return "nomem";
  case GEM_NET_TIMEOUT:   return "timeout";
  case GEM_NET_LOST:      return "lost";
  case GEM_NET_TOO_BIG:   return "toobig";
  case GEM_NET_BAD_BODY:  return "badbody";
  default:                return "?";
  }
}

// "http 200", "dns", "not asked": how one half's request went, in a word.
static void halfWord(const GeoHalfRes* r, char* out, size_t cap) {
  if (!r->asked) {
    strlcpy(out, "not asked", cap);
  } else if (r->code > 0) {
    snprintf(out, cap, "http %d", r->code);
  } else {
    strlcpy(out, netWord(r->net), cap);
  }
}

/* The one line a look leaves in the log: the verdict, the accuracy, how each half went, the time.
 * ⚠ Never a coordinate or a MAC. */
static void logLook(int v, const GeoFix* fx, bool dry) {
  if (dry) {
    log_e("WI-FI LOCATION: too few access points (%u of %u devices) - nothing sent anywhere",
          (unsigned)s_m->split.devices, (unsigned)GEO_MIN_DEVICES);
    return;
  }
  char a[24], b[24];
  halfWord(&s_m->ra, a, sizeof(a));
  halfWord(&s_m->rb, b, sizeof(b));
  if (v == GEO_V_LOCATED) {
    log_e("WI-FI LOCATION: located +/-%u m (halves %u m apart; A %s, B %s; %u.%u s)%s",
          (unsigned)fx->accM, (unsigned)fx->apartM, a, b, (unsigned)(s_m->jobMs / 1000),
          (unsigned)((s_m->jobMs % 1000) / 100),
          s_setting == GEO_SET_MAP ? " - map only" : "");
  } else {
    log_e("WI-FI LOCATION: %s (A %s, B %s; %u.%u s)%s%s", geoVerdictName(v), a, b,
          (unsigned)(s_m->jobMs / 1000), (unsigned)((s_m->jobMs % 1000) / 100),
          s_m->skipped[0] ? " - not asked: " : "", s_m->skipped);
  }
}

static void fold() {
  const uint32_t now = millis();
  if (s_cancel || s_setting == GEO_SET_OFF) {
    /* Switched off while it was out: nothing judged, nothing kept - no fix, and not one of the
     * look's access points back into the yardstick (off means off). A half that left the phone
     * still counts against the hour's cap and the gap (geoFoldCancelled). */
    geoFoldCancelled(&s_m->sched, &s_m->ha, &s_m->hb, now);
    char a[24], b[24];
    halfWord(&s_m->ra, a, sizeof(a));
    halfWord(&s_m->rb, b, sizeof(b));
    log_e("WI-FI LOCATION: look abandoned - switched off (A %s, B %s); nothing kept", a, b);
  } else {
    /* The two things two agreeing halves cannot see (geoloc.h, GeoFoldGuard): this boot's newest
     * GPS/phone fix (a real look is never held to a bench one), and a scan that landed while the
     * look was out. */
    GeoAnchor anchor;
    geoAnchorFrom(meshService.getSelfSlots(), now, false, &anchor);
    GeoFoldGuard guard;
    guard.anchor = &anchor;
    guard.latest = s_m->scan.ms ? &s_m->scan : NULL;
    GeoFix fx;
    const int v = geoFold(&s_m->sched, &s_m->job, &s_m->ha, &s_m->hb, now, false,
                          meshService.netSlot(), &fx, &guard);
    s_looks++;
    if (v == GEO_V_LOCATED) {
      s_located++;
      meshService.notePlacesChanged();     // the map's "me +/-N m Wi-Fi" and every distance
    }
    logLook(v, &fx, false);
  }
  s_haveJob = true;
  /* The question is answered: its MACs are not kept a moment longer than the fold needed them. */
  memset(&s_m->job, 0, sizeof(s_m->job));
  memset(s_m->bodyA, 0, sizeof(s_m->bodyA));
  memset(s_m->bodyB, 0, sizeof(s_m->bodyB));
  s_cancel = false;
  s_done = false;
}

static void dryLook(uint32_t now) {
  /* Too few devices to ask about: nothing is sent, the verdict is "too few", and it counts as a
   * look for the gap (geoloc.h). The split is this snapshot's, so a later scan that hears enough
   * is a reason to look again. */
  geoFold(&s_m->sched, &s_m->split, NULL, NULL, now, false, meshService.netSlot(), NULL);
  s_looks++;
  logLook(GEO_V_TOO_FEW, NULL, true);
}

/* A look that cannot start for want of RAM: folded as a NO_MEMORY half (not an attempt - nothing
 * left the phone; retried after the gap, geoloc.h's lastTransient). */
static void noMemoryLook(uint32_t now, const char* why) {
  GeoHalf a;
  memset(&a, 0, sizeof(a));
  a.reply = GEO_R_NO_MEMORY;
  geoFold(&s_m->sched, &s_m->split, &a, NULL, now, false, meshService.netSlot(), NULL);
  s_looks++;
  log_e("WI-FI LOCATION: not asked - %s", why);
}

static void startLook(uint32_t now, bool asked) {
  /* The joined access point asked about AGAIN (review, 2026-10-10): the hotspot's phone app may
   * have answered since the scan was copied, and its BSSID must not go out - nor into the
   * yardstick a failed look leaves behind (where a device that travels with the phone would
   * always count as "still heard"). The snapshot is of this association (geoLookDue's
   * freshness), so the split is simply taken over; first, so every way out below uses it. */
  uint8_t joined[6];
  geoSplitScan(&s_m->scan, joinedLeftOut(joined), &s_m->split);
  if (!s_m->split.built) {
    dryLook(now);                          // the hotspot was one of the four: too few now
    return;
  }
  /* The heap bar, unless an AI question or a weather fetch holds the worker (its handshake is
   * dipping the heap right now): the worker checks it again before each half anyway. */
  if (!aiRequestActive() && !wxRequestActive()) {
    uint32_t fr = 0, lg = 0;
    if (!netHeapOkForHandshake(&fr, &lg)) {
      char why[64];
      snprintf(why, sizeof(why), "phone low on memory (%u KB block, %u KB free)",
               (unsigned)(lg / 1024), (unsigned)(fr / 1024));
      noMemoryLook(now, why);
      return;
    }
  }
  char why[64];
  if (!netWorkerReady(why, sizeof(why))) {
    noMemoryLook(now, why);
    return;
  }
  /* The question, self-contained: the split itself (not the snapshot a newer scan overwrites while
   * half A is out), both bodies built now on the loop. */
  s_m->job = s_m->split;
  if (geoBuildHalf(&s_m->job, GEO_HALF_A, s_m->bodyA, sizeof(s_m->bodyA)) == 0 ||
      geoBuildHalf(&s_m->job, GEO_HALF_B, s_m->bodyB, sizeof(s_m->bodyB)) == 0) {
    dryLook(now);                          // unreachable for a built split (test_geoloc's sizes)
    return;
  }
  /* `netloc now` pauses the music, as a weather Refresh does (a handshake beside a decoding track
   * failed on phone 1). Paused, not stopped. An automatic look never starts beside it
   * (geoLookDue's GEO_DUE_MUSIC). */
  s_musicPaused = asked && musicPlayerIsPlaying();
  if (s_musicPaused) {
    musicPlayerPause();
  }
  tlsInstallPsramHook();                   // BEFORE the first WiFiClientSecure (tile_fetch.h)
  s_jobAsked = asked;
  s_jobStartMs = now;
  s_cancel = false;
  s_done = false;
  s_busy = true;
  netWorkerSubmit(NET_JOB_GEO, geoJob);
}

/* The live answer of a GPS or phone slot on the screens' bar: a measurement in hand, so no look. */
static bool measuredLive(uint32_t now) {
  const SelfSlot* s = meshService.getSelfSlots();
  return selfSlotState(&s[SELF_GPS - 1], SELF_GPS, now, SELF_PICK_INJECTED) == SELF_ST_LIVE ||
         selfSlotState(&s[SELF_PHONE - 1], SELF_PHONE, now, SELF_PICK_INJECTED) == SELF_ST_LIVE;
}

static bool beaconArmed() {
  return meshService.getPosInterval() != 0 && meshService.getPosChannelName()[0] != '\0';
}

void geolocLoop(bool viewing) {
  const uint32_t now = millis();
  if (viewing) {
    s_viewedMs = now | 1u;
  }
  if (s_done && s_m) {
    fold();                                // every pass: a flag test when idle
  }
  if ((uint32_t)(now - s_tickMs) < GEO_TICK_MS) {
    return;
  }
  s_tickMs = now;
  if (s_setting == GEO_SET_OFF || !memOk()) {
    s_lastDue = GEO_DUE_OFF;
    return;
  }
  SelfSlot* net = meshService.netSlot();
  /* Expired is GONE, not merely old (geoExpire: the slot's age wraps with millis()), and a fix
   * whose WiFi link dropped or changed is HELD off every screen until a scan judges it
   * (geoOnLink - without it a phone walked out of its home WiFi with auto-switch off, which then
   * never scans, kept its home fix on the map for up to 12 minutes). */
  if (geoExpire(&s_m->sched, net, now)) {
    meshService.notePlacesChanged();
    log_e("WI-FI LOCATION: no scan has confirmed the fix for 12 min - gone");
  }
  const uint32_t assoc = lastWifiLinkUpMs();
  if (geoOnLink(&s_m->sched, net, assoc)) {
    meshService.notePlacesChanged();
    log_e("WI-FI LOCATION: the WiFi link changed - the fix is off the map until a scan judges it");
  }
  const bool up = wifiUp() && (WiFi.getMode() & WIFI_MODE_AP) == 0;
  const int set = effSetting();            // a pending "map + mesh" wants and sends nothing more
  const bool armed = beaconArmed() && set == GEO_SET_MESH;

  GeoWhenIn in;
  memset(&in, 0, sizeof(in));
  in.setting = (uint8_t)set;
  in.busy = geoRequestActive();
  in.wifiUp = up;
  in.measuredLive = measuredLive(now);
  in.viewedMs = s_viewedMs;
  in.beaconArmed = beaconArmed();
  in.netOk = geoGateWhy() == NULL;
  in.musicPlaying = musicPlayerIsPlaying();
  in.assoc = assoc;
  in.netLive = selfSlotState(net, SELF_NET, now, SELF_PICK_INJECTED) == SELF_ST_LIVE;
  in.scan = s_m->scan.ms ? &s_m->scan : NULL;
  in.scanDevices = s_m->split.devices;
  const int due = geoLookDue(&s_m->sched, &in, now);
  s_lastDue = due;

  /* A scan, not a lookup, is what keeps a fix that is still true (self_pos.h):
   *  - the look itself needs a snapshot of this association under 2 minutes old;
   *  - geoRefreshWanted: a held fix's judge; a live fix on screen re-checked every 2 minutes (a
   *    fix carried off on a hotspot that stays connected must not ride along for the switcher's
   *    10); the switcher's own scan late while a beacon still wants the fix;
   *  - a beacon slot is OWED and the Wi-Fi fix is the one it would send, but it is still settling
   *    (no second agreeing scan 60 s after the first) or unconfirmed (no scan in the last 60 s):
   *    a scan finished now would make it sendable (selfNetScanUseful) - and only when no GPS or
   *    phone fix would go instead.
   * Never while a handshake is on the worker (an AI question, a weather fetch, a look): the
   * scan's allocations land 1-4 s after it starts, and inside a handshake's internal-heap dip
   * that is the 2026-08-28 reboot. Opening the Almanac starts its weather fetch; the scan waits
   * for it (Networks.cpp holds an ask made just before, too - geolocScanMayStart). */
  bool wantScan = due == GEO_DUE_SCAN || geoRefreshWanted(&s_m->sched, &in, net, now);
  if (!in.busy && up && armed && meshService.posBeaconDue()) {
    const int st = selfSlotState(net, SELF_NET, now, SELF_PICK_TX);
    SelfFix tx;
    if ((st == SELF_ST_SETTLING || st == SELF_ST_UNCONFIRMED) && selfNetScanUseful(net, now) &&
        meshService.getSelfFix(&tx, true) == SELF_NONE) {
      wantScan = true;
    }
  }
  if (wantScan && up && !netRequestActive() && geoScanRequestOk(&s_m->sched, now)) {
    wifiState.requestGeoScan();
    geoScanRequested(&s_m->sched, now);
    s_scanAsks++;
  }

  if (due == GEO_DUE_DRY) {
    dryLook(now);
  } else if (due == GEO_DUE_YES) {
    /* Two holds, each decided again on the next tick: no handshake beside a scan in flight (its
     * allocation is the one the heap bar cannot see coming), and none in the first seconds of an
     * association (a hotspot's phone app has not answered yet - its BSSID would go out). */
    if (wifiState.scanBusy()) {
      s_lastDue = GEO_DUE_NET_BUSY;
    } else if ((uint32_t)(now - assoc) < GEO_JOIN_SETTLE_MS) {
      s_lastDue = GEO_DUE_TOO_SOON;
    } else {
      startLook(now, geoAsked(&s_m->sched, now));
    }
  }
}

// ── the screen and the bench ─────────────────────────────────────────────────────────────

size_t geolocRowText(char* out, size_t cap) {
  if (s_meshPending && s_setting == GEO_SET_MESH) {
    if (!out || cap == 0) {
      return 0;
    }
    strlcpy(out, GEO_ROW_MESH_PENDING, cap);   // test_geoloc measures it against the row
    return strlen(out);
  }
  SelfFix fx;
  const bool have = meshService.getSourceFix(SELF_NET, &fx);
  return geoRowText(out, cap, s_setting, s_m ? &s_m->sched : NULL, have ? &fx : NULL,
                    s_setting == GEO_SET_OFF ? GEO_DUE_OFF : s_lastDue);
}

void geolocAskNow() {
  if (memOk()) {
    geoAsk(&s_m->sched, millis());
    s_tickMs = 0;                          // decide on the next pass, not a second from now
  }
}

/* One half from the console: "{...}" (an HTTP 200 body), "404" (a status, no body) or "-" (not
 * asked). *end = just past it. */
static bool parseHalf(const char* p, const char** end, GeoHalf* h) {
  while (*p == ' ') {
    p++;
  }
  memset(h, 0, sizeof(*h));
  if (*p == '-' && (p[1] == ' ' || p[1] == '\0')) {
    *end = p + 1;
    return true;                           // GEO_R_NOT_ASKED
  }
  if (*p >= '1' && *p <= '9') {
    int code = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 4) {
      code = code * 10 + (*p++ - '0');
      digits++;
    }
    if (digits != 3 || (*p != ' ' && *p != '\0')) {
      return false;
    }
    geoHalfFrom(h, GEO_R_ANSWERED, code, "", 0);
    *end = p;
    return true;
  }
  if (*p != '{') {
    return false;
  }
  /* The object's end by brace depth, strings skipped (a '}' inside one is not the end). */
  const char* s = p;
  int depth = 0;
  bool str = false;
  for (; *p; p++) {
    if (str) {
      if (*p == '\\' && p[1]) {
        p++;
      } else if (*p == '"') {
        str = false;
      }
      continue;
    }
    if (*p == '"') {
      str = true;
    } else if (*p == '{') {
      depth++;
    } else if (*p == '}' && --depth == 0) {
      p++;
      break;
    }
  }
  if (depth != 0) {
    return false;
  }
  geoHalfFrom(h, GEO_R_ANSWERED, 200, s, (size_t)(p - s));
  *end = p;
  return true;
}

bool geolocBenchAnswer(const char* args, int* verdict, char* why, size_t whyCap) {
  if (why && whyCap) {
    why[0] = '\0';
  }
  if (s_setting == GEO_SET_OFF) {
    strlcpy(why, "Wi-Fi location is off (`netloc map` first)", whyCap);
    return false;
  }
  if (!memOk()) {
    strlcpy(why, "no PSRAM for Wi-Fi location", whyCap);
    return false;
  }
  if (geoRequestActive()) {
    strlcpy(why, "a real look is out - try again in a few seconds", whyCap);
    return false;
  }
  GeoHalf a, b;
  const char* p = args ? args : "";
  if (!parseHalf(p, &p, &a) || !parseHalf(p, &p, &b)) {
    strlcpy(why, "usage - netloc answer <A> <B>: each {...} (HTTP 200), a 3-digit status, or -",
            whyCap);
    return false;
  }
  while (*p == ' ') {
    p++;
  }
  if (*p) {
    strlcpy(why, "text after the second half", whyCap);
    return false;
  }
  const uint32_t now = millis();
  /* The latest snapshot's split when it has enough devices (the fix then stands on them, as a
   * real one would); else a stand-in (geoBenchFold). Stamped now either way. */
  const GeoSplit* real = (s_m->scan.ms && s_m->split.built) ? &s_m->split : NULL;
  /* Held to the jump backstop like a real look, bench fixes included: `pos inject gps <far>` then
   * `netloc answer ...` exercises it on the phone. */
  GeoAnchor anchor;
  geoAnchorFrom(meshService.getSelfSlots(), now, true, &anchor);
  GeoFix fx;
  const int v = geoBenchFold(&s_m->sched, real, &a, &b, now, ntpClock.getTrustedUtcTime(),
                             lastWifiLinkUpMs(), meshService.netSlot(), &fx, &anchor);
  if (v == GEO_V_LOCATED) {
    meshService.notePlacesChanged();
  }
  if (verdict) {
    *verdict = v;
  }
  log_e("WI-FI LOCATION: BENCH answer folded - %s%s (shown, never sent by the schedule)",
        geoVerdictName(v), real ? "" : ", on a stand-in set");
  return true;
}

static const char* setWord(int s) {
  return geoSettingName(s);
}

void geolocReport(void (*emit)(const char* line)) {
  char l[192];
  const uint32_t now = millis();
  snprintf(l, sizeof(l), "netloc: Wi-Fi location %s%s", setWord(s_setting),
           s_setting == GEO_SET_OFF ? " (default; nothing is scanned for it or sent)"
           : s_meshPending ? " - CHOSEN, not sent until the My node screen is left" : "");
  emit(l);
  if (!s_m) {
    emit("netloc: nothing since boot");
    return;
  }
  const GeoSched* sc = &s_m->sched;
  if (s_m->scan.ms) {
    snprintf(l, sizeof(l),
             "netloc: last scan %lus ago: %u heard, %u kept, %u usable at -85 dBm or stronger "
             "(%u devices: %s)%s",
             (unsigned long)((uint32_t)(now - s_m->scan.ms) / 1000u), (unsigned)s_m->scan.seen,
             (unsigned)s_m->scan.n, (unsigned)s_m->split.usable, (unsigned)s_m->split.devices,
             s_m->split.built ? "enough to ask" : "too few",
             s_m->scan.assoc == lastWifiLinkUpMs() ? "" : " - an earlier association");
  } else {
    strlcpy(l, "netloc: no scan copied yet (the next WiFi scan, or `netloc now`)", sizeof(l));
  }
  emit(l);
  if (sc->lastLookMs) {
    char acc[40] = "";
    if (sc->lastVerdict == GEO_V_LOCATED || sc->lastVerdict == GEO_V_VAGUE) {
      snprintf(acc, sizeof(acc), ", +/-%u m", (unsigned)sc->lastAccM);
    }
    if (sc->lastApartM) {
      snprintf(acc + strlen(acc), sizeof(acc) - strlen(acc), ", halves %lu m apart",
               (unsigned long)sc->lastApartM);
    }
    snprintf(l, sizeof(l), "netloc: last look %lus ago: %s%s (%u devices); %d attempt(s) in the last hour of %d",
             (unsigned long)((uint32_t)(now - sc->lastLookMs) / 1000u), geoVerdictName(sc->lastVerdict),
             acc, (unsigned)sc->lastDevices, geoAttemptsLastHour(sc, now), GEO_ATTEMPTS_PER_HOUR);
  } else if (sc->lastVerdict != GEO_V_NONE) {
    snprintf(l, sizeof(l), "netloc: no real look yet; the bench's last: %s", geoVerdictName(sc->lastVerdict));
  } else {
    strlcpy(l, "netloc: no look yet", sizeof(l));
  }
  emit(l);
  SelfFix fx;
  if (meshService.getSourceFix(SELF_NET, &fx)) {
    SelfFix tx;
    const bool txOk = selfSource(meshService.getSelfSlots(), SELF_NET, now, SELF_PICK_TX, &tx);
    snprintf(l, sizeof(l), "netloc: fix +/-%u m, %s, last confirming scan %lus ago%s; to send: %s",
             (unsigned)fx.accM, selfStateName(fx.state), (unsigned long)(fx.ageMs / 1000u),
             fx.injected ? " [BENCH]" : "", txOk ? selfStateName(tx.state) : "-");
  } else if (sc->held) {
    snprintf(l, sizeof(l), "netloc: fix HELD off the map since the WiFi link changed (+/-%u m); "
             "the next scan brings it back or drops it", (unsigned)sc->heldFix.accM);
  } else {
    strlcpy(l, "netloc: no fix in hand", sizeof(l));
  }
  emit(l);
  char row[72];
  geolocRowText(row, sizeof(row));
  snprintf(l, sizeof(l), "netloc: now: %s; the screen says \"%s\"", geoDueName(s_lastDue), row);
  emit(l);
  if (geoRequestActive()) {
    const int run = netWorkerRunning();
    snprintf(l, sizeof(l), "netloc: LOOKING (%s, %lus since asked)%s",
             s_done ? "done, folding next pass" : (run == NET_JOB_GEO ? "running" : "queued"),
             (unsigned long)((uint32_t)(now - s_jobStartMs) / 1000u),
             s_musicPaused ? " - music paused" : "");
    emit(l);
  } else if (s_haveJob) {
    for (int h = 0; h < 2; h++) {
      const GeoHalfRes* r = h ? &s_m->rb : &s_m->ra;
      if (!r->asked) {
        snprintf(l, sizeof(l), "netloc:   half %c: not asked", h ? 'B' : 'A');
      } else {
        snprintf(l, sizeof(l), "netloc:   half %c: HTTP %d, %s, %lu B, dns %lu ms, connect+handshake %lu ms, all %lu ms%s",
                 h ? 'B' : 'A', r->code, netWord(r->net), (unsigned long)r->bytes,
                 (unsigned long)r->dnsMs, (unsigned long)r->hsMs, (unsigned long)r->ms,
                 r->tlsErr ? " (tls error)" : "");
      }
      emit(l);
      if (r->asked && r->tlsErr) {
        snprintf(l, sizeof(l), "netloc:     tls error -0x%04X", (unsigned)(-r->tlsErr));
        emit(l);
      }
    }
    snprintf(l, sizeof(l), "netloc:   the look took %lu ms (%s)%s%s; worker stack floor %lu of %u, internal largest floor %lu",
             (unsigned long)s_m->jobMs, s_jobAsked ? "netloc now" : "automatic",
             s_m->skipped[0] ? "; not asked: " : "", s_m->skipped, (unsigned long)s_m->stackFloor,
             (unsigned)NET_STACK_BYTES, (unsigned long)s_m->largestFloor);
    emit(l);
  }
  snprintf(l, sizeof(l), "netloc: since boot: %lu scans copied, %lu scans asked, %lu looks, %lu located, %lu dropped as moved",
           (unsigned long)s_scans, (unsigned long)s_scanAsks, (unsigned long)s_looks,
           (unsigned long)s_located, (unsigned long)s_moved);
  emit(l);
}
