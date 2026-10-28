/*
 * kosync_sync.cpp — see kosync_sync.h. The protocol is kosync.cpp (host-tested); what is
 * here is the glue that needs the card, the radio and a socket, kept as thin as it can be.
 */
#include "kosync_sync.h"
#include "kosync.h"
#include "booksync.h"
#include "booksync_inbox.h"
#include "app_gbc_xfer.h"
#include "meshtastic_service.h"   // the default device name: the mesh long name
#include "clock.h"                // ntpClock: UTC for timestamps
#include "Networks.h"             // lastWifiLinkUpMs: an answered home= name lasts the join
#include "wifi_policy.h"          // wifiJoinAgeMs: a join in progress holds an ask (review N1)
#include "GUI.h"                  // gui.isAppRunning: no window under Settings > WiFi

extern GUI gui;

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <SD.h>
#include <Preferences.h>
#include <esp_system.h>           // esp_read_mac
#include <lwip/sockets.h>         // the home client's polled connect (see KS_CONNECTING)
#include <lwip/dns.h>             // home= as a DNS name: lwIP's resolver, never waited on
#include <lwip/tcpip.h>           // ...started on the tcpip thread (tcpip_callback_with_block)
#include <esp_wifi.h>             // esp_wifi_sta_get_ap_info: the SSID, without a String
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

/* 🛑 TRUSTED (ntp or gps), NOT merely known — the one clock rule KOSync has (0.9.79).
 * Every time KOSync takes is used to JUDGE: the last-move stamp decides "provably older" (and a
 * "provably older" server record is HIDDEN — no card), the window stamps `timestamp` on what it
 * serves (the X4 judges by it), and a parked offer carries it into the inbox's newest-wins.
 * A mesh-set clock was adopted from unauthenticated packets (clock_source.h): if it ran AHEAD,
 * our last move would look newer than every real server record and each would be hidden — the
 * one failure the offer rule exists to prevent ("a spurious card costs one Back; a hidden
 * position is the bug"). As 0 ("unknown") it costs at most that one Back. A GPS-set clock is
 * UTC from a solved fix and is as good as NTP here. */
static uint32_t nowUtc() {
  return ntpClock.getTrustedUtcTime();
}

static size_t largestInternal() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void setNote(char* note, size_t cap, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void setNote(char* note, size_t cap, const char* fmt, ...) {
  if (!note || !cap) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(note, cap, fmt, ap);
  va_end(ap);
}

static int pctInt(double p) {
  return (int)(p * 100.0 + 0.5);
}

// ================================================================ state
/* ⚠ THE TEXT AND THE CONFIG LIVE IN PSRAM, allocated the first time Books asks. As plain
 * statics they were ~1.2 KB of permanent INTERNAL RAM (measured: the 0.9.79 build's static
 * RAM went 88,828 -> 90,084) for a feature most phones never switch on — on a phone whose
 * internal heap is the thing that runs out. What stays static below is flags, counters and
 * pointers. */
struct KsText {
  KosyncConfig cfg;
  char cfgNote[80];
  char dev[40];
  char devId[KOSYNC_KEY_CHARS];
  char peer[KOSYNC_DEV_MAX];                  // who picked the window up
  char winLast[96];                           // how the last window ended, or why none opened
  char parkDev[KOSYNC_DEV_MAX];               // the PUT waiting to be parked
  char hline[128];                            // the home client's current header line
  char cliLast[96];                           // the last home job's outcome
  KosyncPutLog puts;                          // who PUT what in the window (kosync.h, KS-3)
  uint32_t cfgSig;                            // kosyncConfigSig of the file as last read
  KosyncHomeAddr home;                        // home='s looked-up address (kosync.h)
  uint8_t homeBlob[KOSYNC_HOME_BLOB_BYTES];   // ...its bytes on the way to/from NVS
  char jobSsid[33];                           // the SSID the current home job started on
  char lookName[KOSYNC_HOST_MAX];             // the name being looked up (lwIP reads it too)
  KosyncParkLedger ledger;                    // one KOSync offer per book (kosync.h)
  KosyncMemo memo;                            // last move + last offer per book (kosync.h)
  uint8_t memoBlob[KOSYNC_MEMO_BLOB_MAX];     // its bytes on the way to/from NVS
};
static KsText* T = NULL;

static bool ksAlloc() {
  if (!T) {
    T = (KsText*)ps_malloc(sizeof(KsText));
    if (T) {
      memset(T, 0, sizeof(*T));
      snprintf(T->cfgNote, sizeof(T->cfgNote), "not read yet");
    }
  }
  return T != NULL;
}

// ================================================================ config
static bool         s_cfgTried = false;
static bool         s_cfgRead = false;            // T->cfgSig holds a real read's signature
static void         clearWindowProblems();        // (the window, below)

static void reloadDone(bool asked) {
  /* 🛑 NOT ON EVERY RE-READ: Books re-reads the file on each entry and Sync settings on each
   * visit, and clearing there would wipe the window's warning on the way to the one screen
   * that shows it. Asked for (serial `kosync reload`), or the file now SAYS something else. */
  const uint32_t sig = kosyncConfigSig(&T->cfg);
  if (kosyncReloadClears(asked, s_cfgRead, T->cfgSig, sig)) {
    clearWindowProblems();
  }
  T->cfgSig = sig;
  s_cfgRead = true;
}

bool kosyncReloadConfig(bool asked) {
  s_cfgTried = true;
  if (!ksAlloc()) {
    return false;                                 // no PSRAM: KOSync is simply off
  }
  memset(&T->cfg, 0, sizeof(T->cfg));
  T->dev[0] = '\0';                               // re-derived: the name may have changed
  /* exists() first: in this core SD.open() of a missing file prints "[E] ... does not
   * exist" (vfs_api.cpp), and this runs on every entry to Books for every phone that has
   * never heard of KOSync. exists() asks the same question without the line. */
  File f;
  if (SD.exists(KOSYNC_CONFIG_FILE)) {
    f = SD.open(KOSYNC_CONFIG_FILE, FILE_READ);
  }
  if (!f) {
    snprintf(T->cfgNote, sizeof(T->cfgNote), "off (no %s)", KOSYNC_CONFIG_FILE);
    reloadDone(asked);                            // a file taken away is a change too
    return false;
  }
  // PSRAM, and wiped after: the password is in these bytes until kosyncParseConfig hashes it.
  const size_t cap = 2048;
  char* buf = (char*)ps_malloc(cap);
  if (!buf) {
    f.close();
    snprintf(T->cfgNote, sizeof(T->cfgNote), "off (no memory to read the file)");
    if (asked) {
      clearWindowProblems();                      // (unread: not a signature worth keeping)
    }
    return false;
  }
  int n = f.read((uint8_t*)buf, cap - 1);
  f.close();
  if (n < 0) {
    n = 0;
  }
  buf[n] = '\0';
  kosyncParseConfig(buf, (size_t)n, &T->cfg);
  memset(buf, 0, cap);
  free(buf);
  reloadDone(asked);
  if (!T->cfg.ok) {
    snprintf(T->cfgNote, sizeof(T->cfgNote), "off - %s", T->cfg.problem);
  } else {
    snprintf(T->cfgNote, sizeof(T->cfgNote), "%s", T->cfg.problem);   // a warning, or ""
  }
  /* ⚠ The key is never logged, here or anywhere: it is the password as far as any KOSync
   * server is concerned. */
  log_e("KOSYNC config: %s user=%s home=%s:%u device=%s auto=%d open_window=%d hotspot=%s%s%s%s%s",
        T->cfg.ok ? "ON" : "off", T->cfg.user, T->cfg.home[0] ? T->cfg.home : "(none)",
        (unsigned)T->cfg.homePort, kosyncMyDevice(), (int)T->cfg.autoOnClose,
        (int)T->cfg.openWindow, T->cfg.hotspotPass[0] ? "WPA2" : "open",
        T->cfg.hotspotNote[0] ? " - " : "", T->cfg.hotspotNote,
        T->cfgNote[0] ? " - " : "", T->cfgNote);
  return T->cfg.ok;
}

bool kosyncConfigured() {
  if (!s_cfgTried) {
    kosyncReloadConfig();
  }
  return T && T->cfg.ok;
}

const KosyncConfig* kosyncConfig() {
  static const KosyncConfig kOff = KosyncConfig();   // const: flash, not RAM
  return T ? &T->cfg : &kOff;
}

const char* kosyncConfigNote() {
  return T ? T->cfgNote : "off (no memory)";
}

const char* kosyncMyDevice() {
  if (!T) {
    return "WiPhone";
  }
  if (T->cfg.device[0]) {
    return T->cfg.device;
  }
  if (!T->dev[0]) {
    /* The booksync device name (Sync settings > This device), which itself defaults to the
     * mesh long name — the same name the LoRa card already shows for this phone. */
    char name[24] = {0};
    Preferences p;
    if (p.begin("wpmesh", true)) {
      p.getString("bsdev", name, sizeof(name));
      p.end();
    }
    if (!name[0]) {
      const char* n = meshService.getMyLongName();
      snprintf(name, sizeof(name), "%s", (n && n[0]) ? n : "");
    }
    if (!name[0] || !strncasecmp(name, "WiPhone", 7)) {
      snprintf(T->dev, sizeof(T->dev), "%s", name[0] ? name : "WiPhone");
    } else {
      snprintf(T->dev, sizeof(T->dev), "WiPhone-%s", name);
    }
  }
  return T->dev;
}

const char* kosyncMyDeviceId() {
  if (!T) {
    return "";
  }
  if (!T->devId[0]) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    kosyncDeviceId(mac, T->devId);
  }
  return T->devId;
}

// On a network we joined ourselves — not our own hotspot, which has nothing behind it.
/* Review N1: how long an ask waits for a join that has just begun, and the most it waits in all. */
#define KOSYNC_JOIN_WAIT_MS      30000u
#define KOSYNC_JOIN_WAIT_MAX_MS  60000u
/* When the idle ask started waiting for ONE join; 0 = not waiting. 🛑 Non-zero ONLY while
 * clientStep()'s idle branch is returning "held for a join": every other way out of that branch
 * zeroes it (integration review 3, KOS-N1a/R3-3). It used to survive the Games-app hold, so the
 * join ~GbcApp begins after a game inherited a start from BEFORE the game, was already "60 s
 * old" on its first pass, and dropped the very push N1 was written to keep. */
static uint32_t s_joinWaitSinceMs = 0;

static bool onWifi() {
  return WiFi.status() == WL_CONNECTED && !xferUsingAP();
}

/* Review N1: the WiFi is not up but it is COMING — the owner wants the station (not off in
 * Settings, no game holding the radio: wifiStationWanted), no hotspot of ours is on the radio, and
 * a join was begun < KOSYNC_JOIN_WAIT_MS ago (wifiJoinAgeMs: a stamp made later in this pass, as
 * ~GbcApp's and a window's close make, is age 0, not ~49 days). While this holds, "not on WiFi"
 * is not a reason to give up an ask — in ANY state of the home client (heldOffWifi()). */
static bool joinInProgress(uint32_t now) {
  return T->cfg.ok && T->cfg.home[0] && !onWifi() && !xferUsingAP() && wifiStationWanted() &&
         lastWifiConnectAttemptMs() != 0 &&
         wifiJoinAgeMs(now, lastWifiConnectAttemptMs()) < KOSYNC_JOIN_WAIT_MS;
}

// ================================================================ the per-book memo (kosync.h)
/* The last real MOVE of each book's place, the server record last offered for it, and what
 * home last had from us (or that a move never reached it), kept in NVS so a reboot neither
 * forgets a move, re-offers a declined place, nor drops a place that was never sent. ~840
 * bytes, written at most once per book close (and once per offer, once per successful PUT) —
 * never per page turn. */
static bool s_memoLoaded = false;

static KosyncMemo* memo() {
  if (!T) {
    return NULL;
  }
  if (!s_memoLoaded) {
    s_memoLoaded = true;
    size_t n = 0;
    Preferences p;
    /* Read-WRITE on purpose: a read-only open of a namespace that does not exist yet prints
     * "[E] nvs_open failed: NOT_FOUND" on every boot until the first save. This creates the
     * (empty) namespace once instead. */
    if (p.begin("kosync", false)) {
      if (p.isKey("memo")) {
        n = p.getBytes("memo", T->memoBlob, sizeof(T->memoBlob));
      }
      p.end();
    }
    if (n && !kosyncMemoUnpack(&T->memo, T->memoBlob, n)) {
      log_e("KOSYNC memo: %u stored bytes did not read back - starting empty", (unsigned)n);
    }
  }
  return &T->memo;
}

static KosyncMemoEntry* memoFor(const char* byName, bool create) {
  KosyncMemo* m = memo();
  return m ? kosyncMemoGet(m, byName, create) : NULL;
}

static void memoSaveIfDirty() {
  KosyncMemo* m = memo();
  if (!m || !m->dirty) {
    return;
  }
  m->dirty = false;
  const size_t n = kosyncMemoPack(m, T->memoBlob, sizeof(T->memoBlob));
  Preferences p;
  if (!p.begin("kosync", false)) {
    log_e("KOSYNC memo: NVS would not open - last moves not kept across a restart");
    return;
  }
  if (p.putBytes("memo", T->memoBlob, n) != n) {
    log_e("KOSYNC memo: NVS refused %u bytes", (unsigned)n);
  }
  p.end();
}

void kosyncNoteMoved(const char* byName, uint32_t nowUtc) {
  // The session's flag, `unsent` (home has not had this move) and the stamp; saved at the close.
  kosyncMemoMoved(memo(), memoFor(byName, true), nowUtc);
}

void kosyncSaveState() {
  if (T && s_memoLoaded) {
    memoSaveIfDirty();
  }
}

bool kosyncBookRenamed(const char* oldBasename, const char* newBasename) {
  char from[EPUB_KOSYNC_ID_CHARS], to[EPUB_KOSYNC_ID_CHARS];
  epubKosyncFilenameMd5(oldBasename, from);
  epubKosyncFilenameMd5(newBasename, to);
  if (!ksAlloc()) {
    return false;
  }
  KosyncMemo* m = memo();                       // loads it from NVS first, if not yet this boot
  const bool moved = kosyncRekeyBook(m, &T->ledger, from, to);
  memoSaveIfDirty();
  return moved;
}

uint32_t kosyncMovedAt(const char* byName) {
  const KosyncMemoEntry* e = memoFor(byName, false);
  return e ? e->movedAt : 0;
}

static bool allocBook(KosyncBook** p) {
  if (!*p) {
    *p = (KosyncBook*)ps_malloc(sizeof(KosyncBook));
  }
  return *p != NULL;
}

// ================================================================ parking a peer's place
/* The peer's percentage -> (reading chapter, within) -> a CBS1 record signed with the LOCAL
 * booksync key -> the inbox. The reading screen's one-second tick (or the next open of this
 * book) then raises the ordinary card. Called only from kosyncLoop(), never from inside the
 * raw pump: the packer and its HMAC want ~2 KB of stack, and the pump's own frames are
 * already on the 8 KB loop task when a request is being answered. */
// The LOCAL booksync key: the one the reader verifies a parked record under (Sync settings).
static void localSyncKey(uint8_t key[32]) {
  char pass[24] = {0};
  Preferences p;
  if (p.begin("wpmesh", true)) {
    p.getString("bspw", pass, sizeof(pass));    // the reader verifies under this same key
    p.end();
  }
  bookSyncDeriveKey(pass, key);
  memset(pass, 0, sizeof(pass));
}

static bool kosyncPark(const KosyncBook* b, double pct, const char* device, uint32_t turnedAt,
                       uint32_t sig) {
  int r = 0;
  double w = 0.0;
  if (!T || !b || !epubKosyncLocate(&b->map, pct, &r, &w, NULL, NULL)) {
    log_e("KOSYNC park: '%s' cannot place %.4f (not syncable: %s)", b ? b->title : "?", pct,
          b ? epubKosyncWhyNot(&b->map) : "?");
    return false;
  }
  uint8_t key[32];
  localSyncKey(key);
  const char* idp[BOOKSYNC_MAX_IDS] = { b->ids[0], b->ids[1], b->ids[2] };
  char text[BOOKSYNC_MESH_TEXT_MAX];
  if (!kosyncParkText(idp, b->nIds, r, w, b->nRead, turnedAt, device, key, text, sizeof(text))) {
    log_e("KOSYNC park: could not build the record for '%s'", b->title);
    return false;
  }
  /* Through the ledger: this book's previous KOSync offer is REPLACED, so repeated PUTs (or
   * the same book opened again and again) can never push another book's LoRa position out
   * of the four-slot inbox. `sig` (a home record's; 0 for a window PUT) waits there for the
   * person's answer — it is NOT "offered" until they give one (kosync.h, KS-2). */
  const bool ok = kosyncParkInto(&T->ledger, b->byName, text, pct, nowUtc(), sig);
  log_e("KOSYNC parked %s's place in '%s': %.4f -> chapter %d, %d%% through it (%s)",
        device && device[0] ? device : "a peer", b->title, pct, r + 1, pctInt(w),
        ok ? "the card will offer it" : "inbox REFUSED it");
  return ok;
}

// ================================================================ the window
#define KOSYNC_REPLY_CAP 512
static KosyncBook*        s_win = NULL;       // PSRAM: the ONE book the window serves
static char*              s_reply = NULL;     // PSRAM: the JSON answer
static bool               s_winArmed = false;
static KosyncWindowClock  s_clock;
static uint32_t           s_gets = 0, s_puts = 0, s_parked = 0, s_unauth = 0;
// GETs for the book from a reader that cannot take a percentage (KOReader): told `{}`, §5.1.
static uint32_t           s_unmarked = 0;
static uint32_t           s_healthMs = 0;
// A window on the phone's WiFi address: the address it opened on, and the loss watch (W1).
static uint32_t           s_winIp = 0;        // 0 = on our own hotspot
static uint32_t           s_staLostMs = 0;
static uint32_t           s_staCheckMs = 0;
// A PUT worth a card, parked on the next kosyncLoop() pass (see kosyncPark) — WITH its own
// copy of the book it was for, so a window re-opened for another book in between (a serial
// `kosync open`, a close/open request) can never file this place under that one.
static KosyncBook*        s_parkBook = NULL;  // PSRAM
static bool               s_parkWant = false;
static double             s_parkPct = 0.0;
static uint32_t           s_parkTurned = 0;
// A window asked for by a book close/open, opened from the loop (see kosyncLoop).
static KosyncBook*        s_req = NULL;
static bool               s_reqWant = false;
static uint32_t           s_reqDurMs = 0;
static const char*        s_reqWhy = "";

bool kosyncWindowActive() {
  return s_winArmed;
}

static bool s_askInBlip = false;   // a window was asked for while its WiFi was blipping
static void requestWindow(const KosyncBook* b, uint32_t durMs, const char* why);
static void windowAfterHome(const char* how);   // "Sync my place": the window the home job holds

/* A window on the phone's WiFi address, checked by the ONE rule (kosyncStationWindowCheck) from
 * both the loop's once-a-second look and a new ask (`asking`): closed when the WiFi is LOST
 * (past the grace, or switched off / disabled) or MOVED; a BLIP keeps it. True = it was closed.
 * An ask that a blip kept on the WiFi is not lost if the blip turns out to be the WiFi GOING
 * (walking out of the house): the loop's close asks again, for what was left of it, and that
 * ask brings up the hotspot. Not from an ask's own check: that ask goes on to open a window
 * itself, and a second one queued behind it would replace it. */
static bool staWindowGoneClose(uint32_t now, bool asking) {
  const bool off = wifiState.radioOff() || wifiState.userDisabled();
  const int st = kosyncStationWindowCheck(off, WiFi.status() == WL_CONNECTED,
                                          (uint32_t)WiFi.localIP(), s_winIp, &s_staLostMs, now,
                                          KOSYNC_STA_GRACE_MS);
  if (st == KOSYNC_STA_OK) {
    s_askInBlip = false;                         // the WiFi came back: the ask was served on it
    return false;
  }
  if (st == KOSYNC_STA_BLIP) {
    s_askInBlip = s_askInBlip || asking;
    return false;
  }
  const bool reask = s_askInBlip && !asking && !off && s_win;
  const uint32_t left = kosyncClockRemainingMs(&s_clock, now);
  kosyncWindowClose(st == KOSYNC_STA_MOVED ? "the WiFi address changed"
                    : off ? "the WiFi was switched off" : "the WiFi went away");
  if (reask && left) {
    requestWindow(s_win, left, "asked during a WiFi drop");
  }
  return true;
}

bool kosyncWindowOpen(const KosyncBook* b, uint32_t durationMs, char* note, size_t cap) {
  if (note && cap) {
    note[0] = '\0';
  }
  if (!kosyncConfigured()) {
    setNote(note, cap, "KOSync is %s", kosyncConfigNote());
    return false;
  }
  if (!b || (!b->partial[0] && !b->byName[0])) {
    setNote(note, cap, "No KOSync id for this book");
    return false;
  }
  if (epubKosyncTotal(&b->map) == 0) {
    setNote(note, cap, "This book can't sync over KOSync: %s", epubKosyncWhyNot(&b->map));
    return false;
  }
  if (!allocBook(&s_win) || (!s_reply && !(s_reply = (char*)ps_malloc(KOSYNC_REPLY_CAP)))) {
    setNote(note, cap, "No memory for a sync window");
    return false;
  }
  const uint32_t now = millis();
  /* 🛑 A WINDOW ON A WIFI THAT IS GONE IS NOT A WINDOW. Opened on the phone's WiFi address at
   * home, then asked for again in the car: without this the dead window was simply extended
   * (the transport below is only started when no window is armed), 'WiPhone-Books' never came
   * up, and the note went on saying "on WiFi at 192.168.1.37" to an X4 that found nothing.
   * Close it first so the ask below brings up the hotspot. (kosyncLoop closes one that goes
   * stale while nobody is asking.)
   * ⚠ BY THE LOOP'S RULE, grace included: "not connected THIS instant" is also a blip or a
   * roam at home, and closing on it swapped a working WiFi window for the hotspot. A blip
   * keeps the window (and is extended below); only LOST / switched off / MOVED closes it. */
  if (s_winArmed && !xferUsingAP()) {
    staWindowGoneClose(now, true);
  }
  const bool sameBook = s_winArmed && !strcmp(s_win->byName, b->byName) &&
                        !strcmp(s_win->partial, b->partial);
  /* 🛑 NOT UNDER Settings > WiFi. That screen scans every 5 s and joins networks, and with a
   * hotspot live that is AP+STA channel-hopping at best and a station join under a live
   * softAP at worst — the combination WiPhone.ino's reconnect gate exists to prevent. Both
   * of its screens close any window as they open (GUI.cpp); this refuses a new one while
   * either is up (a close/open request queued a moment before, say). */
  if (gui.isAppRunning(GUI_APP_NETWORKS) || gui.isAppRunning(GUI_APP_EDITWIFI)) {
    setNote(note, cap, "Window not opened: Settings > WiFi is open");
    return false;
  }
  if (!s_winArmed && !xferWindowStart(KOSYNC_AP_NAME, T->cfg.hotspotPass)) {
    const char* why = xferWindowError();
    setNote(note, cap, "Window not opened: %s", why ? why : "no network");
    log_e("KOSYNC window NOT opened: %s", why ? why : "no network");
    return false;
  }
  uint32_t dur = durationMs;
  if (sameBook) {
    // A second ask never SHORTENS a window already open for this book (a 60 s open-window
    // arriving during a 5-minute one, say).
    const uint32_t left = kosyncClockRemainingMs(&s_clock, now);
    if (left > dur) {
      dur = left;
    }
  } else {
    /* A NEW window: the last one's warnings go (they stayed on the screen until now). The
     * open one extended for the same book keeps its own. */
    s_gets = s_puts = s_parked = 0;
    T->peer[0] = '\0';
    clearWindowProblems();
  }
  if (!s_winArmed) {
    s_winIp = xferUsingAP() ? 0 : (uint32_t)WiFi.localIP();
    s_staLostMs = 0;
    s_staCheckMs = now;
    s_askInBlip = false;
  }
  memcpy(s_win, b, sizeof(*b));
  kosyncClockOpen(&s_clock, now, dur);
  s_winArmed = true;
  s_healthMs = now;
  T->winLast[0] = '\0';
  char hs[48];
  kosyncHotspotLine(hs, sizeof(hs));
  log_e("KOSYNC window OPEN %lus for '%s' at %s%s%s (%s)  ids %s / %s  pct=%.4f  largest=%u",
        (unsigned long)(dur / 1000), s_win->title, xferAddr(),
        xferUsingAP() ? " on hotspot " : " (on WiFi)", xferUsingAP() ? xferApName() : "", hs,
        s_win->partial[0] ? s_win->partial : "-", s_win->byName, s_win->pct,
        (unsigned)largestInternal());
  return true;
}

void kosyncWindowClose(const char* why) {
  if (!s_winArmed) {
    return;
  }
  s_winArmed = false;
  s_clock.open = false;
  s_winIp = 0;
  s_askInBlip = false;
  xferWindowStop();
  snprintf(T->winLast, sizeof(T->winLast), "Window closed: %s", why ? why : "");
  uint32_t second = 0;
  const uint32_t diff = kosyncPutLogDifferent(&T->puts, NULL, &second);
  log_e("KOSYNC window CLOSED (%s): gets=%u puts=%u parked=%u other-book=%u second-id=%u "
        "unauthorised=%u no-percentage=%u", why ? why : "", (unsigned)s_gets, (unsigned)s_puts,
        (unsigned)s_parked, (unsigned)diff, (unsigned)second, (unsigned)s_unauth,
        (unsigned)s_unmarked);
}

/* The window's warnings (a PUT for another book, a wrong password): cleared by a NEW window,
 * or by a reload that was asked for or found the file changed (reloadDone). kosync.h. */
static void clearWindowProblems() {
  s_unauth = 0;
  s_unmarked = 0;
  memset(&T->puts, 0, sizeof(T->puts));        // the PUTs for another book (KS-3's log)
}

const char* kosyncWindowServe(const char* method, const char* path, const char* hdrs,
                              const char* body, size_t bodyLen, int* code) {
  if (!s_winArmed || !s_win || !s_reply) {
    *code = 404;
    return "{\"message\":\"Not found\"}";
  }
  KosyncServed sv = { s_win->partial, s_win->byName, s_win->pctOk, s_win->pct, kosyncMyDevice(),
                      kosyncMyDeviceId(), s_win->movedAt };
  KosyncServeOut o;
  kosyncServe(method, path, hdrs, body, bodyLen, T->cfg.user, T->cfg.key, &sv, nowUtc(),
              &o, s_reply, KOSYNC_REPLY_CAP);
  *code = o.code;
  if (o.code == 401) {
    s_unauth++;
  }
  if (o.pickedUp) {
    // A GET: the peer may now wait on a human before it PUTs — the long grace. A PUT: done.
    kosyncClockPicked(&s_clock, millis(), o.gotPut ? KOSYNC_GRACE_PUT_MS : KOSYNC_GRACE_GET_MS);
    if (o.gotPut) {
      s_puts++;
    } else {
      s_gets++;
    }
  }
  if (o.gotPut) {
    snprintf(T->peer, sizeof(T->peer), "%s", o.putDevice[0] ? o.putDevice : "a KOSync reader");
    kosyncPutLogOwn(&T->puts, o.putDevice, o.putDeviceId);   // KS-3: this device syncs THIS book
  }
  if (o.unmarkedGet && !s_unmarked++) {
    /* ONE line a window (the count is cleared with the window's other warnings): the reader
     * was told `{}` — "No progress found" — because our reply's empty `progress` would have
     * sent it to page 1 (kosync.h, kosyncClientTakesPercentage; the plan's §5.1). */
    log_e("KOSYNC window: a reader that cannot take a percentage asked for '%s' - answered {} "
          "(KOReader? see docs/booksync-simple-plan.md 5.1)", s_win->title);
  }
  if (o.park && !allocBook(&s_parkBook)) {
    log_e("KOSYNC: no memory to hold %s's place - not offered", o.putDevice);
  } else if (o.park) {
    memcpy(s_parkBook, s_win, sizeof(*s_parkBook));
    s_parkWant = true;
    s_parkPct = o.putPct;
    snprintf(T->parkDev, sizeof(T->parkDev), "%s", o.putDevice[0] ? o.putDevice : "KOSync");
    s_parkTurned = nowUtc();
  }
  if (o.otherDoc) {
    /* 🛑 NOT "a different book" by itself (KS-3): the CrossPoint fork with booksync PUTs every
     * upload under BOTH its ids, and the file-name one differs whenever the file NAME does.
     * Whether it was that device's second id is decided when the line is shown
     * (kosyncProblemLine), so the order of the two PUTs does not matter. */
    kosyncPutLogOther(&T->puts, o.putDevice, o.putDeviceId, o.otherDocId);
  }
  /* log_e on purpose: only log_e is compiled into this firmware, and a window that "did
   * nothing" must be tellable from one that was never reached. A window sees a handful of
   * requests, so this is a handful of lines. */
  log_e("KOSYNC %s %s -> %d%s%s%s", method, path, o.code,
        o.code == 401 ? " (wrong user/password on the peer?)" : "",
        o.park ? " PARKED" : (o.gotPut ? " (already in step - no card)" : ""),
        o.otherDoc ? " (another book - discarded)" : "");
  return s_reply;
}

// A pull asked for while the phone was not yet on WiFi (see kosyncBookOpened).
static KosyncBook*        s_waitBook = NULL;  // PSRAM
static bool               s_waitWifi = false;
static uint32_t           s_waitCheckMs = 0;

bool kosyncWantsPosition() {
  return s_winArmed || s_waitWifi;
}

static void notePlace(KosyncBook* k, const char* partial, const char* byName, double pct,
                      bool pctOk, uint32_t movedAt) {
  if (!k || strcmp(k->byName, byName) != 0 || strcmp(k->partial, partial ? partial : "") != 0) {
    return;
  }
  /* ⚠ An update that CANNOT be expressed still updates: the reader has left the place the
   * window was holding, and a GET must now answer `{}`, not that old place. */
  k->pctOk = pctOk;
  k->pct = pctOk ? pct : 0.0;       // a GET now answers where the reader actually is
  if (movedAt) {
    k->movedAt = movedAt;
  }
}

void kosyncNotePosition(const char* partial, const char* byName, double pct, bool pctOk,
                        uint32_t movedAt) {
  if (!byName) {
    return;
  }
  if (s_winArmed) {
    notePlace(s_win, partial, byName, pct, pctOk, movedAt);
  }
  if (s_waitWifi) {
    /* The place, yes (the in-step test compares from HERE, later) — 🛑 but NOT the last-move
     * stamp (0 = keep it): the ask was made as the book opened, and it is judged against the
     * stamp as it stood then (kosync.h, KS-1). Pages read before the WiFi came up (a GPS-set
     * clock stamps every one) used to make the X4's newer place "provably older". */
    notePlace(s_waitBook, partial, byName, pct, pctOk, 0);
  }
}

static void requestWindow(const KosyncBook* b, uint32_t durMs, const char* why) {
  if (!allocBook(&s_req)) {
    return;
  }
  memcpy(s_req, b, sizeof(*b));
  s_reqWant = true;
  s_reqDurMs = durMs;
  s_reqWhy = why;
}

/* ── "SYNC MY PLACE" ON WIFI WITH A HOME: THE WINDOW WAITS FOR THE HOME JOB (kosync.h,
 * kosyncWindowWaitsForHome; the plan's §5.2). The ask is kept here — its own PSRAM copy of the
 * book, like every other window ask — and turned into an ordinary deferred request
 * (requestWindow) when the job has its verdict (finishJob: pushed, offered, gave up), when the
 * idle branch drops the ask without a job (off WiFi, no memory), or when
 * KOSYNC_WINDOW_AFTER_HOME_MAX_MS has passed (kosyncLoop), whichever is first. The loop's own
 * rules then apply to the open: not during a call, not under a game, not under Settings > WiFi.
 *
 * THE BOUND, against clientStep()'s own clocks. The verdict that takes longest and matters is
 * "home did not answer": a DNS name's lookup gives up at KS_DNS_GIVEUP_MS (6 s; an mDNS name at
 * KS_MDNS_GIVEUP_MS, 2 s; an IP address looks nothing up), then the job's KOSYNC_CLIENT_TRIES
 * connects each time out at KS_CONNECT_MS (3 s), 1 s and 4 s apart (kosyncRetryDelayMs):
 * 6 + 3x3 + 1 + 4 = 20 s, inside this bound (tests/check_kosync_glue.py reads those constants
 * and holds the bound above their sum). A home that answers takes a few hundred ms a request,
 * four requests (GET both ids, then PUT both). The theoretical worst — every request stalling to
 * KS_TOTAL_MS (12 s) without failing, ~56 s with the lookup — and a join wait (off WiFi only,
 * up to KOSYNC_JOIN_WAIT_MAX_MS) are cut off here: the window then opens under the running job,
 * which is what every press did before this change (a 0.9.79 window on WiFi serves on the
 * station and leaves the push alone; off WiFi the window belongs on the hotspot anyway). */
#define KOSYNC_WINDOW_AFTER_HOME_MAX_MS 25000u
static KosyncBook* s_afterBook = NULL;        // PSRAM: the book the held ask is for
static bool        s_afterWant = false;
static uint32_t    s_afterSinceMs = 0;

static bool rememberWindowAfterHome(const KosyncBook* b, uint32_t now) {
  if (!allocBook(&s_afterBook)) {
    return false;                             // no memory to hold the ask: open it now instead
  }
  memcpy(s_afterBook, b, sizeof(*b));
  s_afterWant = true;
  s_afterSinceMs = now;
  snprintf(T->winLast, sizeof(T->winLast), "Window waits for home - opens after its answer");
  log_e("KOSYNC: the window for '%s' waits for the home job (at most %lu s)", b->title,
        (unsigned long)(KOSYNC_WINDOW_AFTER_HOME_MAX_MS / 1000));
  return true;
}

static void windowAfterHome(const char* how) {
  if (!s_afterWant) {
    return;
  }
  s_afterWant = false;
  log_e("KOSYNC: %s - opening the window for '%s' now", how, s_afterBook->title);
  requestWindow(s_afterBook, KOSYNC_WINDOW_SYNC_MS, "sync my place, after home");
}

// ================================================================ the home client
/* KS_CONNECT opens a socket and starts a NON-BLOCKING connect; KS_CONNECTING polls it (a
 * zero-timeout select, once a pass) until it is writable, refused, or KS_CONNECT_MS has gone
 * by; KS_HEADERS/KS_BODY read the answer; KS_RETRY waits out the back-off after a request
 * that got no answer (kosyncRetryDelayMs). Nothing here waits on the network.
 *
 * 🛑 THE CONNECT USED TO BE WiFiClient::connect(ip, port, 600) — which, in this core, is a
 * non-blocking connect followed by a select() that SLEEPS the loop task for the whole 600 ms:
 * the 608 ms LOOP STALL named 'kosync' on hardware, and on a foreign WiFi where home= never
 * answers, a 600 ms freeze on every book open and close. And one failed attempt (the first
 * contact after a quiet spell, on a station in modem sleep — seen on hardware 2026-09-24)
 * dropped the whole push or pull: the place sent on close never reached home, and the ask on
 * open offered nothing. The same steps are taken here, one per pass, and a job gets
 * KOSYNC_CLIENT_TRIES. */
enum KsCliState { KS_IDLE = 0, KS_RESOLVE, KS_CONNECT, KS_CONNECTING, KS_HEADERS, KS_BODY, KS_RETRY };

#define KS_IO_CAP   1536                      // the request, then the response body
struct KsCliWork {
  KosyncBook     book;                        // the job's OWN copy: a new ask may not mix in
  char           io[KS_IO_CAP];
  KosyncProgress got[2];
  bool           have[2];
  int            sent;
};

static KsCliWork*  s_w = NULL;                // PSRAM
static KsCliState  s_cs = KS_IDLE;
static WiFiClient  s_client;
static int         s_fd = -1;                 // the socket while it connects (not yet s_client's)
static KosyncBook* s_pushBook = NULL;
static KosyncBook* s_pullBook = NULL;
static bool        s_pushWant = false, s_pullWant = false;
static bool        s_pushReadFirst = false;   // "Sync my place": read the server before sending
static uint32_t    s_pushGen = 0, s_pullGen = 0, s_jobGen = 0;
static bool        s_curPush = false;         // this job came from the push slot
static bool        s_jobReadFirst = false;    // ...and reads before it sends
static bool        s_phaseGet = false;        // the request in hand is a GET
static int         s_step = 0;                // 0: the partial MD5 id, 1: the file-name id
static int         s_fails = 0;               // requests of this job that got no answer
static uint32_t    s_retryAtMs = 0;
static size_t      s_hlen = 0;
static size_t      s_bodyGot = 0;
static long        s_clen = -1;
static bool        s_chunked = false;
static int         s_code = -1;
static uint32_t    s_deadlineMs = 0, s_reqStartMs = 0, s_connStartMs = 0;
static IPAddress   s_ip((uint32_t)0);
/* The WiFi join the job started on: lastWifiLinkUpMs(), the GOT_IP stamp. While the station is
 * connected that stamp is THIS association's (a drop rewrites it, and so does the GOT_IP that
 * ends the rejoin), so it tells "the same join" from "a new one" with nothing added to the WiFi
 * event handler. */
static uint32_t    s_jobAssoc = 0;
static bool        s_viaFallback = false;     // s_ip is home='s LAST address: the name did not answer

static const uint32_t KS_CONNECT_MS = 3000;    // polled, never waited on
static const uint32_t KS_STALL_MS   = 5000;    // silence once connected
static const uint32_t KS_TOTAL_MS   = 12000;   // one request, stall or not
static const int      KS_READ_BUDGET = 512;    // bytes per main-loop pass

bool kosyncClientBusy() {
  // Waiting out a back-off is not "mid-transfer": the stretched idle tick is fine for that.
  return s_cs != KS_IDLE && s_cs != KS_RETRY;
}

static bool homeReady(char* note, size_t cap) {
  if (!kosyncConfigured()) {
    setNote(note, cap, "KOSync is %s", kosyncConfigNote());
    return false;
  }
  if (!T->cfg.home[0]) {
    setNote(note, cap, "No home= in %s", KOSYNC_CONFIG_FILE);
    return false;
  }
  if (!onWifi()) {
    setNote(note, cap, "Not on WiFi - nothing sent home");
    return false;
  }
  return true;
}

static bool queuePush(const KosyncBook* b, bool readFirst, char* note, size_t cap) {
  if (!homeReady(note, cap)) {
    return false;
  }
  if (!b || !b->pctOk) {
    setNote(note, cap, "This place can't be sent over KOSync");
    return false;
  }
  if (!allocBook(&s_pushBook)) {
    setNote(note, cap, "No memory to send");
    return false;
  }
  memcpy(s_pushBook, b, sizeof(*b));
  s_pushWant = true;
  s_pushReadFirst = readFirst;
  s_pushGen++;
  snprintf(T->cliLast, sizeof(T->cliLast), readFirst ? "Checking home, then sending %d%%..."
                                                     : "Sending %d%% to home...", pctInt(b->pct));
  return true;
}

bool kosyncPush(const KosyncBook* b, char* note, size_t cap) {
  return queuePush(b, false, note, cap);
}

bool kosyncSyncHome(const KosyncBook* b, char* note, size_t cap) {
  return queuePush(b, true, note, cap);
}

bool kosyncPull(const KosyncBook* b, char* note, size_t cap) {
  if (!homeReady(note, cap)) {
    return false;
  }
  if (!b || epubKosyncTotal(&b->map) == 0) {
    setNote(note, cap, "This book can't sync over KOSync");
    return false;
  }
  if (!allocBook(&s_pullBook)) {
    setNote(note, cap, "No memory to ask");
    return false;
  }
  memcpy(s_pullBook, b, sizeof(*b));
  s_pullWant = true;
  s_pullGen++;
  snprintf(T->cliLast, sizeof(T->cliLast), "Asking home about this book...");
  return true;
}

// ================================================================ home= by NAME (kosync.h)
/* KS_RESOLVE: nothing here waits.
 *   mDNS (covey.local, or the bare covey): a one-shot query from our own UDP socket, sent at 0,
 *     250 and 1000 ms and answered by UNICAST to that socket (RFC 6762 §6.7) — read with a
 *     zero-wait recvfrom once a pass, given up at KS_MDNS_GIVEUP_MS. Three sends, because one
 *     lost multicast (a station in modem sleep, a busy AP) must not cost the job.
 *   DNS (a dotted name that is not .local, or a bare name mDNS did not know — the order
 *     resolveDomain() tried them in): lwIP's own resolver, STARTED ON THE TCPIP THREAD (it is
 *     not thread-safe from here) and answered by a callback into s_dns*; the loop only reads
 *     the flag. Given up at KS_DNS_GIVEUP_MS; a late answer carries a stale tag and is dropped.
 * An ANSWERED name is not looked up again on the same WiFi join (kosyncHomePlan); one nobody
 * answered is asked again by the next job (nothing recorded - kosync.h). The last answer, its
 * SSID and home= are kept in NVS (one record) so an unanswered lookup after a restart still has
 * the last address to fall back to.
 * ⚠ The name looked up is T->lookName, copied from home= as the job starts: a `kosync reload`
 *   mid-lookup cannot change what the query asks, what the answer must match, or what lwIP is
 *   reading on its own thread. */
static const uint32_t KS_MDNS_SEND_AT_MS[3] = { 0, 250, 1000 };
static const uint32_t KS_MDNS_GIVEUP_MS = 2000;
static const uint32_t KS_DNS_GIVEUP_MS  = 6000;
static int               s_udp = -1;
static uint16_t          s_qid = 0;
static int               s_qSent = 0;
static bool              s_lookMdns = false, s_lookTriedDns = false;
static uint32_t          s_lookStartMs = 0, s_lookFirstMs = 0;
static uint32_t          s_lookMaxUs = 0;     // the longest lookup pass this boot (serial `kosync`)
static volatile bool     s_dnsDone = false;
static volatile uint32_t s_dnsIp = 0;
static volatile uint32_t s_dnsTag = 0;
static bool              s_homeLoaded = false;

static KosyncHomeAddr* homeAddr() {
  if (!s_homeLoaded) {
    s_homeLoaded = true;
    size_t n = 0;
    Preferences p;
    if (p.begin("kosync", false)) {           // read-write: see memo()
      if (p.isKey("home")) {
        n = p.getBytes("home", T->homeBlob, sizeof(T->homeBlob));
      }
      p.end();
    }
    if (n && !kosyncHomeUnpack(&T->home, T->homeBlob, n)) {
      log_e("KOSYNC home: %u stored bytes did not read back - no last address", (unsigned)n);
    }
  }
  return &T->home;
}

// Written only when the address, the name or the network CHANGED (kosyncHomeLooked's *changed).
static void homeSave() {
  const size_t n = kosyncHomePack(&T->home, T->homeBlob, sizeof(T->homeBlob));
  Preferences p;
  if (!n || !p.begin("kosync", false)) {
    return;
  }
  if (p.putBytes("home", T->homeBlob, n) != n) {
    log_e("KOSYNC home: NVS refused %u bytes", (unsigned)n);
  }
  p.end();
}

// The joined network's SSID, without a String (esp_wifi_sta_get_ap_info). "" when not joined.
static void staSsid(char out[33]) {
  out[0] = '\0';
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    memcpy(out, ap.ssid, 32);
    out[32] = '\0';
  }
}

static void dnsFound(const char* name, const ip_addr_t* a, void* arg) {     // the tcpip thread
  if ((uint32_t)(uintptr_t)arg != s_dnsTag) {
    return;                                    // a lookup given up on: nobody is waiting
  }
  s_dnsIp = (a && IP_IS_V4(a)) ? ip4_addr_get_u32(ip_2_ip4(a)) : 0;
  s_dnsDone = true;
}

static void dnsStart(void* arg) {                                             // the tcpip thread
  ip_addr_t a;
  const err_t e = dns_gethostbyname(T->lookName, &a, dnsFound, arg);
  if (e == ERR_OK) {
    dnsFound(T->lookName, &a, arg);            // lwIP's own table had it
  } else if (e != ERR_INPROGRESS) {
    dnsFound(T->lookName, NULL, arg);
  }
}

static void lookupEnd() {
  if (s_udp >= 0) {
    close(s_udp);
    s_udp = -1;
  }
  s_dnsTag = s_dnsTag + 1u;                    // an answer still on its way belongs to nobody
}

static bool lookupStart(uint32_t now, bool dns) {
  lookupEnd();
  s_lookStartMs = now;
  s_lookMdns = !dns;
  if (dns) {
    s_dnsDone = false;
    s_dnsIp = 0;
    const uint32_t tag = s_dnsTag;
    return tcpip_callback_with_block(dnsStart, (void*)(uintptr_t)tag, 0) == ERR_OK;
  }
  s_udp = socket(AF_INET, SOCK_DGRAM, 0);
  if (s_udp < 0) {
    return false;
  }
  fcntl(s_udp, F_SETFL, fcntl(s_udp, F_GETFL, 0) | O_NONBLOCK);
  struct in_addr ifa;
  ifa.s_addr = (uint32_t)WiFi.localIP();       // out of the station, whatever else is up
  setsockopt(s_udp, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
  const uint8_t ttl = 255;                     // RFC 6762 §11
  setsockopt(s_udp, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  s_qid = (uint16_t)(esp_random() | 1u);
  s_qSent = 0;
  return true;                                 // the first query goes out on the first poll
}

// 1: answered (*ip). 0: still waiting. -1: nobody answered.
static int lookupPoll(uint32_t now, uint32_t* ip) {
  const uint32_t age = now - s_lookStartMs;
  if (!s_lookMdns) {
    if (s_dnsDone) {
      *ip = s_dnsIp;
      return *ip ? 1 : -1;
    }
    return age >= KS_DNS_GIVEUP_MS ? -1 : 0;
  }
  if (s_qSent < 3 && age >= KS_MDNS_SEND_AT_MS[s_qSent]) {
    const size_t n = kosyncMdnsQuery(T->lookName, s_qid, (uint8_t*)s_w->io, KS_IO_CAP);
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(KOSYNC_MDNS_PORT);
    to.sin_addr.s_addr = htonl(0xE00000FBu);   // 224.0.0.251
    if (!n || sendto(s_udp, s_w->io, n, 0, (struct sockaddr*)&to, sizeof(to)) != (int)n) {
      log_e("KOSYNC home: mDNS query %d for '%s' not sent (errno %d)", s_qSent + 1, T->lookName,
            errno);
    }
    s_qSent++;                                 // a failed send counts: the next one is the retry
  }
  for (int i = 0; i < 4; i++) {                // a few datagrams a pass, never a wait
    const int n = recvfrom(s_udp, s_w->io, KS_IO_CAP, MSG_DONTWAIT, NULL, NULL);
    if (n <= 0) {
      break;
    }
    const uint32_t a = kosyncMdnsAnswer((const uint8_t*)s_w->io, (size_t)n, T->lookName, s_qid);
    if (a) {
      *ip = a;
      return 1;
    }
  }
  return age >= KS_MDNS_GIVEUP_MS ? -1 : 0;
}

static void ipText(uint32_t ip, char out[16]) {
  const uint8_t* b = (const uint8_t*)&ip;
  snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void dropSocket() {
  if (s_fd >= 0) {
    close(s_fd);                               // a connect still in flight: nobody owns it yet
    s_fd = -1;
  }
  s_client.stop();
  lookupEnd();                                 // (and a lookup's socket, if one is open)
}

static void finishJob(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void finishJob(const char* fmt, ...) {
  dropSocket();
  s_cs = KS_IDLE;
  // Only retire the ask this job answered: a newer one made meanwhile still runs.
  if (s_curPush && s_pushGen == s_jobGen) {
    s_pushWant = false;
    windowAfterHome("home answered the push");   // whatever the verdict: the window opens now
  } else if (!s_curPush && s_pullGen == s_jobGen) {
    s_pullWant = false;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(T->cliLast, sizeof(T->cliLast), fmt, ap);
  va_end(ap);
  log_e("KOSYNC home %s: %s", s_curPush ? (s_jobReadFirst ? "sync" : "push") : "pull", T->cliLast);
}

/* A request that got no answer (no connect, a timeout, a close with no status line). Tried
 * again after kosyncRetryDelayMs, from the step it was on; given up after KOSYNC_CLIENT_TRIES.
 * The address is kept for the retries (F5: a retry must not also re-resolve) and dropped only
 * when the job gives up. An HTTP answer (401, 500...) is an answer and is NOT retried. */
static void noAnswer(uint32_t now, const char* what) {
  dropSocket();
  s_fails++;
  const uint32_t wait = kosyncRetryDelayMs(s_fails);
  if (wait) {
    s_cs = KS_RETRY;
    s_retryAtMs = now + wait;
    snprintf(T->cliLast, sizeof(T->cliLast), "Home: %s - trying again", what);
    log_e("KOSYNC home %s: %s - try %d of %d in %lu ms", s_curPush ? "push" : "pull", what,
          s_fails + 1, KOSYNC_CLIENT_TRIES, (unsigned long)wait);
    return;
  }
  /* A NAME's address is looked up again by the next job (it may have moved: a new DHCP lease
   * within this join); an unanswered lookup then still falls back to it — COVEY may simply have
   * been off. The retries above did NOT look again (F5: a retry never re-resolves). */
  char at[36] = "";
  if (!kosyncParseIp(T->lookName, NULL)) {
    kosyncHomeGaveUp(homeAddr(), (uint32_t)s_ip);
    char t[16];
    ipText((uint32_t)s_ip, t);
    snprintf(at, sizeof(at), " (%s%s)", s_viaFallback ? "last known " : "", t);
  }
  finishJob("Home: %s from %s%s:%u (%d tries)", what, T->lookName, at, (unsigned)T->cfg.homePort,
            s_fails);
}

/* The server's side of this book is read (both ids): is there a place from another device
 * worth the card (kosync.h, D1)? Offered -> true, the job is finished. For a plain pull the
 * job is finished either way (true). For "Sync my place" (`thenSend`) nothing to offer ->
 * false: the caller goes on to PUT — 🛑 unless a KOSync place for this book is still on the
 * card, unanswered (`parkLive`): then nothing is sent (KS-2). The person answers it first
 * (the card goes up within a second of a park, and while the book is open it must be answered
 * before anything else), and presses again. */
static bool evaluateOffer(bool thenSend) {
  const KosyncBook* b = &s_w->book;
  /* A park still waiting on the card (kosync.h, KS-2) — checked cheaply first, then (rarely)
   * that it still VERIFIES, so a park the card can never show does not hold Sync my place. */
  uint32_t pendingSig = 0;
  uint8_t key[32];
  const bool maybe = kosyncParkPending(&T->ledger, b->byName, NULL, NULL);
  if (maybe) {
    localSyncKey(key);
  }
  const bool parkLive = maybe && kosyncParkPending(&T->ledger, b->byName, key, &pendingSig);
  /* Both ids live on ONE server, so its timestamps are comparable: the NEWEST record from
   * another device is the one that counts (a stock X4 on "filename" matching and our own push
   * under the partial MD5 can both be there for the same book). A tie or a missing timestamp
   * keeps the partial MD5's. Only that one is judged: an older record under the other id is
   * history, not a second offer. */
  bool sawAny = false;
  const int best = kosyncPickRecord(s_w->got, s_w->have, 2, kosyncMyDevice(), kosyncMyDeviceId(),
                                    &sawAny);
  if (best < 0) {
    if (thenSend && !parkLive) {
      return false;
    }
    finishJob(thenSend ? "Home: answer the sync card first - yours NOT sent"
              : sawAny ? "Home: nothing newer from another device"
                       : "Home: this book is not on the server yet");
    return true;
  }
  const KosyncProgress& g = s_w->got[best];
  KosyncMemoEntry* me = memoFor(b->byName, true);
  /* 🛑 KS-1: `b->movedAt` is the last move AS OF THE ASK (the job's snapshot); only the explicit
   * "Sync my place" (thenSend) is judged against the memo's freshest stamp. A page turned while
   * the pull on open was on its way hid the X4's newer place when this read the freshest. */
  KosyncOfferIn in;
  kosyncOfferFill(&in, &g, thenSend, b->pctOk, b->pct, b->movedAt, me, pendingSig,
                  kosyncMyDevice(), kosyncMyDeviceId());
  const int v = kosyncOfferVerdict(&in);
  const char* dev = g.device[0] ? g.device : "Home";
  if (v != KOSYNC_OFFER) {
    if (thenSend && !parkLive) {
      log_e("KOSYNC home sync: %s at %d%% is not offered (%s) - sending ours", dev,
            pctInt(g.pct), kosyncOfferVerdictText(v));
      return false;
    }
    if (thenSend) {
      finishJob("Home: %s is at %d%% - answer the sync card first; yours NOT sent", dev,
                pctInt(g.pct));
    } else if (v == KOSYNC_SKIP_WAITING) {
      finishJob("Home: %s is at %d%% - %s", dev, pctInt(g.pct), kosyncOfferVerdictText(v));
    } else {
      finishJob("Home: %s is at %d%% - %s, not offered", dev, pctInt(g.pct),
                kosyncOfferVerdictText(v));
    }
    return true;
  }
  const uint32_t ts = (in.theirTs > 0 && in.theirTs < 4000000000LL) ? (uint32_t)in.theirTs : 0;
  /* 🛑 NOT "offered" yet (KS-2): the signature waits in the ledger and reaches the memo only
   * when the person answers THIS card (kosyncCardAnswered). Saved here, a reboot before the
   * book was reopened lost the card and hid the place as "already offered". */
  if (kosyncPark(b, g.pct, dev, ts, kosyncOfferSig(in.theirTs, g.pct, g.deviceId, g.device))) {
    finishJob(thenSend ? "Home: %s is at %d%% - offered; yours NOT sent over it"
                       : "Home: %s is at %d%% - offered", dev, pctInt(g.pct));
  } else {
    // Not offered and not overwritten either: a place the card could not show is not ours to bury.
    finishJob("Home: %s is at %d%% - could not offer it%s", dev, pctInt(g.pct),
              thenSend ? " (yours not sent)" : "");
  }
  return true;
}

/* 🛑 KS-2's rule for EVERY push, not only "Sync my place" (evaluateOffer): a place from another
 * device parked for this book and still unanswered (kosyncParkPending, WITH the local key — a
 * park the card can never show must not hold the pushes) is not ours to send over. Asked before
 * a job's FIRST PUT (clientStep, KS_CONNECT). The case it closes: the pull on open is still on
 * its way when the book is closed (auto=on, the place moved or home never had it); the close's
 * push is queued behind the pull, the pull parks the X4's newer place, and the push used to PUT
 * ours over it on home a moment later — COVEY then held our place while the card, on the next
 * open, offered the X4's. Refused, nothing is recorded as sent (kosyncMemoSent is not reached),
 * so the next close after the card is answered sends it (kosyncClosePushWanted: sentPct differs
 * or `unsent`). The first test is the cheap one; the key is read only when a park exists. */
static bool parkAwaitsAnswer(const char* byName) {
  if (!kosyncParkPending(&T->ledger, byName, NULL, NULL)) {
    return false;
  }
  uint8_t key[32];
  localSyncKey(key);
  const bool live = kosyncParkPending(&T->ledger, byName, key, NULL);
  memset(key, 0, sizeof(key));
  return live;
}

static void stepDone(int code) {
  // code -2: this id is missing on our side (an unreadable partial MD5) — skip the step.
  if (code == 401) {
    finishJob("Home: wrong user or password (401)");
    return;
  }
  if (!s_phaseGet) {
    if (code != -2 && code != 200 && code != 202) {
      finishJob("Home: HTTP %d on PUT", code);
      return;
    }
    if (code != -2) {
      s_w->sent++;
    }
  } else if (code != -2) {
    if (code == 200) {
      s_w->have[s_step] = kosyncParseProgress(s_w->io, strlen(s_w->io), &s_w->got[s_step]);
    } else if (code != 404) {                // 404: an older server's "unknown document"
      finishJob("Home: HTTP %d on GET", code);
      return;
    }
  }
  if (s_step == 0) {
    s_step = 1;
    s_cs = KS_CONNECT;                         // a fresh connection next pass
    return;
  }
  if (s_phaseGet) {
    if (evaluateOffer(s_curPush)) {
      return;                                  // offered (or, for a pull, judged): done
    }
    s_phaseGet = false;                        // "Sync my place": nothing to offer - now send
    s_step = 0;
    s_cs = KS_CONNECT;
    return;
  }
  if (s_w->sent > 0) {
    /* THE PUSH REFERENCE (D2): the place is now what home has from us, so a close that does
     * not move it sends nothing — this session or any later one (it is SAVED now: a restart
     * before the next close must not turn this into "never sent" and push an unmoved place
     * over a newer one). Only for the book this job was about. */
    kosyncMemoSent(memo(), memoFor(s_w->book.byName, true), s_w->book.pct);
    memoSaveIfDirty();
    finishJob("Sent %d%% to home", pctInt(s_w->book.pct));
  } else {
    finishJob("Home: no id to send under");
  }
}

// Start a non-blocking connect to home=. False: no socket, or refused on the spot.
static bool startConnect() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  const uint32_t ip = (uint32_t)s_ip;
  memcpy(&a.sin_addr.s_addr, &ip, 4);          // the byte order WiFiClient::connect uses
  a.sin_port = htons(T->cfg.homePort);
  const int r = lwip_connect_r(fd, (struct sockaddr*)&a, sizeof(a));
  if (r < 0 && errno != EINPROGRESS) {
    close(fd);
    return false;
  }
  s_fd = fd;
  return true;
}

// Connected: hand the socket to s_client and send the request.
static void sendRequest(uint32_t now) {
  fcntl(s_fd, F_SETFL, fcntl(s_fd, F_GETFL, 0) & ~O_NONBLOCK);   // as WiFiClient::connect leaves it
  /* The same adoption WiFiServer::available() makes: from here s_client owns (and closes) the
   * socket. The temporary's destructor only drops its share of it. */
  s_client = WiFiClient(s_fd);
  s_fd = -1;
  const char* doc = s_step == 0 ? s_w->book.partial : s_w->book.byName;
  size_t n;
  if (!s_phaseGet) {
    char body[320];
    kosyncBuildPutBody(body, sizeof(body), doc, s_w->book.pct, kosyncMyDevice(),
                       kosyncMyDeviceId());
    n = kosyncBuildPut(s_w->io, KS_IO_CAP, T->cfg.home, T->cfg.homePort, T->cfg.user,
                       T->cfg.key, body);
  } else {
    n = kosyncBuildGet(s_w->io, KS_IO_CAP, T->cfg.home, T->cfg.homePort, T->cfg.user,
                       T->cfg.key, doc);
  }
  if (!n) {
    finishJob("Home: request too long");
    return;
  }
  s_client.write((const uint8_t*)s_w->io, n);
  s_cs = KS_HEADERS;
  s_hlen = 0;
  s_bodyGot = 0;
  s_clen = -1;
  s_chunked = false;
  s_code = -1;
  s_deadlineMs = now + KS_STALL_MS;
  s_reqStartMs = now;
}

/* A job PAST KS_IDLE found the station down (integration review 3, KOS-N1a). 🛑 N1's hold used to
 * cover only the idle ask, and these states gave up on the spot:
 *   - KS_RETRY: a push whose first connect timed out (KS_CONNECT_MS, the modem-sleeping station's
 *     first contact) waits out its 1 s / 4 s back-off — in the Games picker, if the owner got there
 *     that fast — and the first pass after a game had taken the radio said "gave up";
 *   - KS_CONNECT: held across the whole game (the !mayUseNetwork hold), then connected on a station
 *     not yet rejoined: no route, a try burnt, KS_RETRY, "gave up" well inside the 7-11 s rejoin;
 *   - KS_RESOLVE: the WiFi dropped mid-lookup and the loop is already rejoining (a game cannot
 *     reach it: the first pass in the Games picker already sends a lookup back to KS_IDLE).
 * So off WiFi with the network not ours to use (a game has the radio), or with a join in progress,
 * the job is dropped and the ASK KEPT — as KS_RESOLVE's own hold does — and the idle branch then
 * waits for the join (at most KOSYNC_JOIN_WAIT_MAX_MS) and starts it over from scratch: a fresh
 * snapshot, s_fails 0, and the parked-place check again (sent == 0). A step already sent is sent
 * again — a PUT of the same place, or a GET. True: held (the caller returns); false: the caller
 * gives up honestly — really off WiFi, nothing coming. */
static bool heldOffWifi(bool mayUseNetwork, uint32_t now) {
  if (mayUseNetwork && !joinInProgress(now)) {
    return false;
  }
  log_e("KOSYNC home %s: off WiFi %s - %s; the ask waits and starts over",
        s_curPush ? "push" : "pull", s_cs == KS_RETRY ? "waiting to retry" : s_cs == KS_CONNECT
        ? "before connecting" : s_cs == KS_RESOLVE ? "while looking up home" : "mid-job",
        mayUseNetwork ? "a join is in progress" : "a game has the radio");
  dropSocket();
  s_cs = KS_IDLE;
  return true;
}

static void clientStep(bool mayUseNetwork, uint32_t now) {
  if (s_cs == KS_IDLE) {
    if (!s_pushWant && !s_pullWant) {
      return;
    }
    if (!mayUseNetwork) {
      /* Held (the Game Boy owns the heap), not dropped. 🛑 AND THE JOIN WAIT ENDS HERE (review 3,
       * KOS-N1a/R3-3): its 60 s budget is for ONE join, not for a whole held period. Kept, a wait
       * begun before the game (or before a second trip into Games, back-to-back) made the join
       * ~GbcApp begins look 60 s old on its first pass, and the ask was dropped "not on WiFi any
       * more" - the exact loss N1 fixed. Zeroed, the wait starts again from the quit. */
      s_joinWaitSinceMs = 0;
      return;
    }
    /* 🛑 A JOIN IN PROGRESS IS NOT "OFF WIFI" (0.9.79 review N1). A push queued at a book close
     * is held while the Games app is open (mayUseNetwork above); a game takes the radio away, and
     * the first pass after ~GbcApp found WiFi.status() != CONNECTED because the rejoin had only
     * just begun — so the push was dropped "not on WiFi any more" and COVEY kept the older place
     * until the next close. Same after a hotspot window. So while the owner wants the station
     * and a join started < 30 s ago (joinInProgress), the ask waits; a phone still unjoined 60 s
     * after it started waiting (out of range: the retry keeps re-stamping joins) gives up
     * honestly below. A job past this point that finds the station down comes back here to wait
     * the same way (heldOffWifi). */
    if (joinInProgress(now)) {
      if (!s_joinWaitSinceMs) {
        s_joinWaitSinceMs = now ? now : 1;
        snprintf(T->cliLast, sizeof(T->cliLast), "Home: waiting for WiFi to join");
      }
      if ((uint32_t)(now - s_joinWaitSinceMs) < KOSYNC_JOIN_WAIT_MAX_MS) {
        return;                                // held while the station comes up
      }
    }
    s_joinWaitSinceMs = 0;
    if (!T->cfg.ok || !T->cfg.home[0] || !onWifi()) {
      s_pushWant = s_pullWant = false;
      snprintf(T->cliLast, sizeof(T->cliLast), "Home: not on WiFi any more - nothing sent");
      log_e("KOSYNC home: %s", T->cliLast);
      windowAfterHome("home not asked (off WiFi)");   // a window off WiFi is the hotspot: now
      return;
    }
    if (!s_w) {
      s_w = (KsCliWork*)ps_malloc(sizeof(KsCliWork));
      if (!s_w) {
        s_pushWant = s_pullWant = false;
        snprintf(T->cliLast, sizeof(T->cliLast), "Home: no memory");
        windowAfterHome("home not asked (no memory)");
        return;
      }
    }
    s_curPush = s_pushWant;                    // a push first: it is the fresher news
    s_jobGen = s_curPush ? s_pushGen : s_pullGen;
    s_jobReadFirst = s_curPush && s_pushReadFirst;
    s_phaseGet = !s_curPush || s_jobReadFirst;
    memcpy(&s_w->book, s_curPush ? s_pushBook : s_pullBook, sizeof(KosyncBook));
    memset(s_w->got, 0, sizeof(s_w->got));
    s_w->have[0] = s_w->have[1] = false;
    s_w->sent = 0;
    s_step = 0;
    s_fails = 0;
    s_viaFallback = false;
    snprintf(T->lookName, sizeof(T->lookName), "%s", T->cfg.home);
    uint32_t ip = 0;
    if (kosyncParseIp(T->lookName, &ip)) {
      s_ip = IPAddress(ip);                    // an IP: nothing looked up, nothing remembered
      s_cs = KS_CONNECT;
      return;                                  // connect on the NEXT pass: this one stays cheap
    }
    /* 🛑 NEVER resolveDomain() here (kosync.h): it froze the loop >= 0.5 s on every book open
     * and close, and never found a .local name at all. */
    staSsid(T->jobSsid);
    s_jobAssoc = lastWifiLinkUpMs();
    if (kosyncHomePlan(homeAddr(), T->lookName, T->jobSsid, s_jobAssoc, &ip) == KOSYNC_HOME_USE) {
      s_ip = IPAddress(ip);                    // already found on this WiFi join
      s_cs = KS_CONNECT;
      return;
    }
    s_lookTriedDns = !kosyncHostIsMdns(T->lookName);
    s_lookFirstMs = now;
    if (!lookupStart(now, s_lookTriedDns)) {
      finishJob("Home: could not start looking up '%s'", T->lookName);
      return;
    }
    s_cs = KS_RESOLVE;
    return;
  }

  if (s_cs == KS_RESOLVE) {
    if (!onWifi()) {
      if (!heldOffWifi(mayUseNetwork, now)) {  // a join in progress: the ask waits (KOS-N1a)
        finishJob("Home: not on WiFi any more - gave up");
      }
      return;
    }
    if (!mayUseNetwork) {
      /* HELD, as a job is at its start and between steps (the Game Boy owns the heap) — but a
       * lookup's clock cannot be stopped, so its socket goes now and the job starts over, with
       * a fresh lookup, once the network may be used again. The ask itself is kept. */
      dropSocket();
      s_cs = KS_IDLE;
      return;
    }
    uint32_t got = 0;
    const int r = lookupPoll(now, &got);
    if (r == 0) {
      return;
    }
    if (r < 0 && !s_lookTriedDns && !strchr(T->lookName, '.')) {
      s_lookTriedDns = true;                   // a bare name mDNS did not know: the router may
      if (lookupStart(now, true)) {
        return;
      }
    }
    const bool mdns = s_lookMdns;
    const char* how = mdns ? "mDNS" : kosyncHostIsMdns(T->lookName) ? "mDNS, then DNS" : "DNS";
    lookupEnd();
    bool fb = false, changed = false;
    const uint32_t ip = kosyncHomeLooked(homeAddr(), T->lookName, T->jobSsid, s_jobAssoc,
                                         r > 0 ? got : 0, &fb, &changed);
    if (changed) {
      homeSave();                              // rare: the address (or the network) changed
    }
    char t[16];
    ipText(ip, t);
    if (!ip) {
      finishJob("Home: '%s' did not answer (%s) - is it on this WiFi?", T->lookName, how);
      return;
    }
    s_viaFallback = fb;
    log_e("KOSYNC home: %s -> %s %s after %lu ms", T->lookName, t,
          fb ? "(did NOT answer: the last address it had here)" : mdns ? "by mDNS" : "by DNS",
          (unsigned long)(now - s_lookFirstMs));
    s_ip = IPAddress(ip);
    s_cs = KS_CONNECT;
    return;                                    // connect on the NEXT pass
  }

  if (s_cs == KS_RETRY) {
    if ((s_curPush ? s_pushGen : s_pullGen) != s_jobGen) {
      /* A newer ask of the same kind came in while this one waited: that one is the news.
       * Dropped WITHOUT retiring the ask, so the next pass starts it fresh. */
      dropSocket();
      s_cs = KS_IDLE;
      log_e("KOSYNC home: a newer ask replaces the one waiting to retry");
      return;
    }
    if (!onWifi()) {
      /* 🛑 Held, not given up, while a game has the radio or a join is under way (KOS-N1a): a
       * back-off that began in the Games picker used to end in "gave up" on the first pass after
       * the game took the radio away. */
      if (!heldOffWifi(mayUseNetwork, now)) {
        finishJob("Home: not on WiFi any more - gave up");
      }
      return;
    }
    if (!mayUseNetwork || (int32_t)(now - s_retryAtMs) < 0) {
      return;
    }
    s_cs = KS_CONNECT;
    // fall through: connect on this pass
  }

  if (s_cs == KS_CONNECT) {
    const char* doc = s_step == 0 ? s_w->book.partial : s_w->book.byName;
    if (!doc[0]) {
      stepDone(-2);
      return;
    }
    if (!onWifi()) {
      /* 🛑 BEFORE startConnect() (KOS-N1a): a job held here across a game used to connect on the
       * first pass after it, on a station not yet rejoined — no route, a try burnt, KS_RETRY, and
       * "gave up" well inside the 7-11 s rejoin. Now it waits for the join like an idle ask. */
      if (!heldOffWifi(mayUseNetwork, now)) {
        finishJob("Home: not on WiFi any more - gave up");
      }
      return;
    }
    if (!mayUseNetwork) {
      return;                                  // held between steps, as at the start
    }
    /* Before the job's FIRST PUT (nothing sent yet: a retry of it asks again, the second id of
     * a pair already half-sent does not) — after the hold above, so a held job never reads NVS
     * on every pass. See parkAwaitsAnswer(). */
    if (!s_phaseGet && s_w->sent == 0 && parkAwaitsAnswer(s_w->book.byName)) {
      finishJob("Home: another device's place waits for your answer - yours NOT sent");
      return;
    }
    if (!startConnect()) {
      noAnswer(now, "could not connect");
      return;
    }
    s_connStartMs = now;
    s_cs = KS_CONNECTING;
    return;
  }

  if (s_cs == KS_CONNECTING) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s_fd, &w);
    struct timeval tv = { 0, 0 };              // a look, not a wait
    const int r = select(s_fd + 1, NULL, &w, NULL, &tv);
    if (r < 0) {
      noAnswer(now, "connect failed");
      return;
    }
    if (r == 0) {
      if ((uint32_t)(now - s_connStartMs) >= KS_CONNECT_MS) {
        noAnswer(now, "no answer");
      }
      return;
    }
    int err = 0;
    socklen_t len = (socklen_t)sizeof(err);
    if (getsockopt(s_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
      noAnswer(now, err == ECONNREFUSED ? "connection refused" : "no answer");
      return;
    }
    sendRequest(now);
    return;
  }

  // KS_HEADERS / KS_BODY: at most KS_READ_BUDGET bytes this pass.
  if ((int32_t)(now - s_deadlineMs) >= 0 || (uint32_t)(now - s_reqStartMs) > KS_TOTAL_MS) {
    noAnswer(now, "timed out");
    return;
  }
  int budget = KS_READ_BUDGET;
  int consumed = 0;
  while (budget-- > 0 && s_client.available()) {
    const int c = s_client.read();
    if (c < 0) {
      break;
    }
    consumed++;
    if (s_cs == KS_HEADERS) {
      if (c == '\r') {
        continue;
      }
      if (c == '\n') {
        T->hline[s_hlen] = '\0';
        if (s_hlen == 0) {
          s_cs = KS_BODY;                      // the blank line
        } else if (s_code < 0) {
          s_code = kosyncStatusCode(T->hline);
        } else if (!strncasecmp(T->hline, "content-length:", 15)) {
          s_clen = strtol(T->hline + 15, NULL, 10);
        } else if (!strncasecmp(T->hline, "transfer-encoding:", 18) && strstr(T->hline, "chunked")) {
          s_chunked = true;
        }
        s_hlen = 0;
        continue;
      }
      if (s_hlen < sizeof(T->hline) - 1) {
        T->hline[s_hlen++] = (char)c;
      }
    } else {
      if (s_bodyGot < KS_IO_CAP - 1) {
        s_w->io[s_bodyGot] = (char)c;
      }
      s_bodyGot++;                             // counted even past the cap, for Content-Length
    }
  }
  if (consumed > 0) {
    s_deadlineMs = now + KS_STALL_MS;
  }
  const bool bodyDone = s_cs == KS_BODY && !s_chunked && s_clen >= 0 && s_bodyGot >= (size_t)s_clen;
  const bool closed = !s_client.connected() && !s_client.available();
  if (bodyDone || closed) {
    s_client.stop();
    if (s_code < 0) {
      noAnswer(now, "no HTTP answer");
      return;
    }
    size_t len = s_bodyGot < KS_IO_CAP - 1 ? s_bodyGot : KS_IO_CAP - 1;
    s_w->io[len] = '\0';
    if (s_chunked) {
      const long d = kosyncDechunk(s_w->io, len);
      s_w->io[d > 0 ? (size_t)d : 0] = '\0';
    }
    // An HTTP answer, whatever its status: this address is home= on this WiFi join.
    if (!kosyncParseIp(T->lookName, NULL)) {
      kosyncHomeReached(homeAddr(), T->lookName, T->jobSsid, s_jobAssoc, (uint32_t)s_ip);
    }
    stepDone(s_code);
  }
}

// ================================================================ from the reader
bool kosyncSyncMyPlace(const KosyncBook* b, char* note, size_t cap) {
  if (note && cap) {
    note[0] = '\0';
  }
  if (!kosyncConfigured()) {
    return false;
  }
  if (b && epubKosyncTotal(&b->map) == 0) {
    setNote(note, cap, "This book can't sync over KOSync: %s", epubKosyncWhyNot(&b->map));
    return false;
  }
  if (!b || !b->pctOk) {
    setNote(note, cap, "This place can't be sent over KOSync");
    return false;
  }
  const bool wifi = onWifi();
  const bool home = T->cfg.home[0] != '\0';
  bool pushed = false;
  char pn[80] = "";
  if (wifi && home) {
    /* D2: an explicit sync always SENDS — but it READS home first, and a newer place from
     * another device is offered as the card instead of being overwritten.
     * 🛑 QUEUED BEFORE THE WINDOW (§5.2, kosyncWindowWaitsForHome): the window used to be
     * opened first, and a hotspot window takes the station this job needs from under it. */
    pushed = kosyncSyncHome(b, pn, sizeof(pn));
  }
  bool opened;
  if (kosyncWindowWaitsForHome(wifi, home, pushed) && rememberWindowAfterHome(b, millis())) {
    setNote(note, cap, "Asking home first - the window opens after");
    opened = true;                             // it will: from the loop, on the job's verdict
  } else {
    // Off WiFi, no home, home refused (its note stays), or no memory to hold the ask: now.
    opened = kosyncWindowOpen(b, KOSYNC_WINDOW_SYNC_MS, note, cap);
    if (wifi && home && !pushed && note && !note[0]) {
      setNote(note, cap, "%s", pn);
    }
  }
  return opened || pushed;
}

void kosyncBookOpened(const KosyncBook* b) {
  if (!kosyncConfigured() || !b) {
    return;
  }
  /* The session's push reference (D2): where the place stands as the book opens. What home
   * last had from us is NOT reset here — that is kept (kosyncClosePushWanted). */
  kosyncMemoOpened(memoFor(b->byName, true), b->pctOk, b->pct);
  s_waitWifi = false;                          // an ask for another book is superseded
  if (onWifi()) {
    if (T->cfg.home[0]) {
      kosyncPull(b, NULL, 0);                  // a newer place from another device -> the card
    }
    return;
  }
  /* Not on WiFi YET (a book opened straight after power-on, before the join finished): the
   * same ask is made once it comes up, while this book is still open (kosyncLoop). */
  if (T->cfg.home[0] && epubKosyncTotal(&b->map) > 0 && allocBook(&s_waitBook)) {
    memcpy(s_waitBook, b, sizeof(*b));
    s_waitWifi = true;
    s_waitCheckMs = millis();
  }
  if (T->cfg.openWindow && epubKosyncTotal(&b->map) > 0) {
    requestWindow(b, KOSYNC_WINDOW_OPEN_MS, "book opened");
  }
}

void kosyncBookClosed(const KosyncBook* b) {
  if (!kosyncConfigured() || !b) {
    return;
  }
  if (s_waitWifi && s_waitBook && !strcmp(s_waitBook->byName, b->byName)) {
    s_waitWifi = false;                        // the book it was for is shut
  }
  if (T->cfg.autoOnClose && b->pctOk) {
    if (onWifi() && T->cfg.home[0]) {
      /* 🛑 ONLY IF THE PLACE MOVED (D2). An open and a close with no reading between used to
       * PUT the unmoved place under both ids — and a server keeps the LAST PUT, so a newer
       * place the X4 had sent was simply gone, and COVEY was then offered this stale one as
       * news. "Moved" is this session OR a place home never had from us (read in the woods,
       * closed off WiFi; a push that gave up) — see kosyncClosePushWanted. */
      const KosyncMemoEntry* e = memoFor(b->byName, false);
      if (kosyncClosePushWanted(e, b->pctOk, b->pct)) {
        const bool session = !e || kosyncAutoPushWanted(e->refOk, e->refPct, e->movedSinceRef,
                                                        b->pctOk, b->pct);
        log_e("KOSYNC home: '%s' at %d%% - pushing on close (%s)", b->title, pctInt(b->pct),
              session ? "moved since it was opened"
                      : (e->sentOk ? "home has an older place of ours" : "a move home never had"));
        kosyncPush(b, NULL, 0);
      } else {
        snprintf(T->cliLast, sizeof(T->cliLast), "Not moved - nothing sent home");
        log_e("KOSYNC home: '%s' at %d%% has not moved - no push on close", b->title,
              pctInt(b->pct));
      }
    }
    requestWindow(b, KOSYNC_WINDOW_SYNC_MS, "book closed");
  }
  memoSaveIfDirty();                           // this session's last move, kept across a restart
}

// ================================================================ status
/* Minutes, not m:ss. These lines are notes in a MenuWidget, which cannot change a row's text
 * in place, so a line that changed every second rebuilt the whole menu every second for as
 * long as a window stayed open. Whole minutes change the text ~5 times a window. */
static void minutesLeft(uint32_t ms, char* out, size_t cap) {
  if (ms < 60000u) {
    snprintf(out, cap, "under a minute");
  } else {
    snprintf(out, cap, "%u min", (unsigned)((ms + 59999u) / 60000u));
  }
}

size_t kosyncHotspotLine(char* out, size_t cap) {
  /* 🛑 WHETHER, never WHAT: "hotspot: WPA2" is the most any screen, serial line or log
   * says about hotspot_pass. */
  if (!cap) {
    return 0;
  }
  out[0] = '\0';
  if (!T) {
    return 0;
  }
  if (s_winArmed && xferUsingAP()) {
    // The hotspot actually on the air — which is the uploader's, as it was, if it got there first.
    snprintf(out, cap, "hotspot: %s%s", xferApProtected() ? "WPA2" : "open",
             xferApByUploader() ? " (uploader's)" : "");
  } else if (s_winArmed) {
    snprintf(out, cap, "hotspot: none (on WiFi)");
  } else if (T->cfg.hotspotPass[0]) {
    snprintf(out, cap, "hotspot: WPA2");
  } else if (T->cfg.hotspotNote[0]) {
    snprintf(out, cap, "hotspot: open - %s", T->cfg.hotspotNote);
  } else {
    snprintf(out, cap, "hotspot: open");
  }
  return strlen(out);
}

size_t kosyncProblemLine(char* out, size_t cap) {
  if (!cap) {
    return 0;
  }
  out[0] = '\0';
  if (!T) {
    return 0;
  }
  /* The current window's, or the last one's — kept after it closes, until a new window opens
   * or a reload that was asked for (or found the file changed) clears them. */
  const char* doc = "";
  const uint32_t diff = kosyncPutLogDifferent(&T->puts, &doc, NULL);   // KS-3: not a second id
  return kosyncWindowProblems(diff, doc, s_unauth, s_unmarked, out, cap);
}

bool kosyncPeerPctFor(uint32_t inboxId, double* pct) {
  return T && kosyncParkedPeerPct(&T->ledger, inboxId, pct);
}

void kosyncCardAnswered(uint32_t inboxId) {
  /* The cheap test first: a LoRa card (every card, for a phone without KOSync) is not in the
   * ledger, and must not load the memo from NVS just to find that out. */
  if (!T || !inboxId || !kosyncParkedPeerPct(&T->ledger, inboxId, NULL)) {
    return;
  }
  if (kosyncOfferAnswered(&T->ledger, memo(), inboxId)) {
    memoSaveIfDirty();                         // rare (one per answer), and a decline must stay one
  }
}

size_t kosyncWindowLine(char* out, size_t cap) {
  if (!cap) {
    return 0;
  }
  out[0] = '\0';
  if (!T) {
    return 0;
  }
  if (s_winArmed) {
    char left[24];
    minutesLeft(kosyncClockRemainingMs(&s_clock, millis()), left, sizeof(left));
    if (s_clock.picked) {
      snprintf(out, cap, "Picked up%s%s - open %s more", T->peer[0] ? " by " : "", T->peer, left);
    } else if (xferUsingAP()) {
      // "(password)": whoever joins must type the hotspot_pass — say so where they look.
      snprintf(out, cap, "Window open, %s left - %s%s %s", left, xferApName(),
               xferApProtected() ? " (password)" : "", xferAddr());
    } else {
      snprintf(out, cap, "Window open, %s left - on WiFi at %s", left, xferAddr());
    }
  } else if (s_afterWant) {
    snprintf(out, cap, "Window waits for home - opens after its answer");
  } else {
    snprintf(out, cap, "%s", T->winLast);
  }
  return strlen(out);
}

size_t kosyncClientLine(char* out, size_t cap) {
  if (!cap) {
    return 0;
  }
  if (!T) {
    out[0] = '\0';
    return 0;
  }
  snprintf(out, cap, "%s", T->cliLast);
  return strlen(out);
}

void kosyncDumpStatus(void (*emit)(const char* line)) {
  char l[192];
  kosyncConfigured();
  if (!T) {
    emit("kosync: off - no PSRAM for its state");
    return;
  }
  snprintf(l, sizeof(l), "kosync: config %s%s%s  file %s", T->cfg.ok ? "ON" : "", T->cfg.ok && T->cfgNote[0] ? " - " : "",
           T->cfgNote, KOSYNC_CONFIG_FILE);
  emit(l);
  if (T->cfg.ok) {
    snprintf(l, sizeof(l), "kosync: user=%s home=%s:%u device=%s id=%.8s... auto=%s open_window=%s",
             T->cfg.user, T->cfg.home[0] ? T->cfg.home : "(none)", (unsigned)T->cfg.homePort,
             kosyncMyDevice(), kosyncMyDeviceId(), T->cfg.autoOnClose ? "on" : "off",
             T->cfg.openWindow ? "on" : "off");
    emit(l);
  }
  if (T->cfg.ok) {
    char hs[80];
    kosyncHotspotLine(hs, sizeof(hs));
    snprintf(l, sizeof(l), "kosync: %s", hs);
    emit(l);
  }
  if (s_winArmed && s_win) {
    uint32_t second = 0;
    const uint32_t diff = kosyncPutLogDifferent(&T->puts, NULL, &second);
    snprintf(l, sizeof(l), "kosync: window OPEN %lus left at %s%s%s  gets=%u puts=%u parked=%u "
             "other-book=%u second-id=%u unauthorised=%u no-percentage=%u%s%s",
             (unsigned long)(kosyncClockRemainingMs(&s_clock, millis()) / 1000), xferAddr(),
             xferUsingAP() ? " hotspot " : " (on WiFi)", xferUsingAP() ? xferApName() : "",
             (unsigned)s_gets, (unsigned)s_puts, (unsigned)s_parked, (unsigned)diff,
             (unsigned)second, (unsigned)s_unauth, (unsigned)s_unmarked,
             s_clock.picked ? "  picked up" : "", T->peer[0] ? T->peer : "");
    emit(l);
    snprintf(l, sizeof(l), "kosync: serving '%s' partial=%s name=%s pct=%.6f",
             s_win->title, s_win->partial[0] ? s_win->partial : "-", s_win->byName, s_win->pct);
    emit(l);
  } else if (s_afterWant) {
    snprintf(l, sizeof(l), "kosync: window waits for home (%lu s of at most %lu s) - opens on the "
             "job's verdict, for '%s'", (unsigned long)((millis() - s_afterSinceMs) / 1000),
             (unsigned long)(KOSYNC_WINDOW_AFTER_HOME_MAX_MS / 1000), s_afterBook->title);
    emit(l);
  } else {
    snprintf(l, sizeof(l), "kosync: window closed%s%s", T->winLast[0] ? " - " : "", T->winLast);
    emit(l);
  }
  {
    char pl[160];
    if (kosyncProblemLine(pl, sizeof(pl))) {
      snprintf(l, sizeof(l), "kosync: %s", pl);
      emit(l);
    }
  }
  snprintf(l, sizeof(l), "kosync: home client %s%s%s%s", s_cs == KS_RETRY ? "WAITING TO RETRY"
           : s_cs == KS_RESOLVE ? "LOOKING UP HOME" : s_cs != KS_IDLE ? "BUSY" : "idle",
           s_waitWifi ? " (an ask waits for WiFi)" : "", T->cliLast[0] ? " - " : "", T->cliLast);
  emit(l);
  if (T->cfg.home[0] && !kosyncParseIp(T->cfg.home, NULL)) {
    /* home= by NAME: the address it was last found at, on which network, and whether the next
     * job uses it as it is ("this join") or looks the name up first — by kosyncHomePlan, the
     * rule the job itself follows. The longest lookup pass is the bench's proof it never waits. */
    const KosyncHomeAddr* h = homeAddr();
    char ssid[33];
    staSsid(ssid);
    uint32_t use = 0;
    const bool thisJoin = kosyncHomePlan(h, T->cfg.home, ssid, lastWifiLinkUpMs(), &use) ==
                          KOSYNC_HOME_USE;
    if (!h->ip || strcasecmp(h->host, T->cfg.home) != 0) {
      snprintf(l, sizeof(l), "kosync: home %s -> not found yet (each home job asks until it "
               "answers)  longest lookup pass %lu us", T->cfg.home,
               (unsigned long)s_lookMaxUs);
    } else {
      char t[16];
      ipText(h->ip, t);
      snprintf(l, sizeof(l), "kosync: home %s -> %s on '%s' - %s  longest lookup pass %lu us",
               T->cfg.home, t, h->ssid, thisJoin ? "this join" : h->stale
               ? "gave up on: looked up again first" : "last known: looked up again first",
               (unsigned long)s_lookMaxUs);
    }
    emit(l);
  }
  snprintf(l, sizeof(l), "kosync: heap largest=%u free=%u psram=%u", (unsigned)largestInternal(),
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  emit(l);
}

// ================================================================ the loop
void kosyncLoop(bool mayUseNetwork, bool callActive) {
  // The common case, every pass, for every phone: nothing asked, nothing open.
  if (!s_reqWant && !s_winArmed && !s_parkWant && s_cs == KS_IDLE && !s_pushWant && !s_pullWant &&
      !s_waitWifi && !s_afterWant) {
    return;
  }
  const uint32_t now = millis();

  // "Sync my place" waiting on the home job (windowAfterHome): the bound, if the job has not.
  if (s_afterWant && (uint32_t)(now - s_afterSinceMs) >= KOSYNC_WINDOW_AFTER_HOME_MAX_MS) {
    windowAfterHome("home took too long");
  }

  /* The ask a book open could not make because the WiFi was not up yet: made once it is (a
   * second's look at most; the ask itself is the ordinary non-blocking pull). */
  if (s_waitWifi && (uint32_t)(now - s_waitCheckMs) >= 1000u) {
    s_waitCheckMs = now;
    if (onWifi()) {
      s_waitWifi = false;
      char note[80] = "";
      if (s_waitBook && kosyncPull(s_waitBook, note, sizeof(note))) {
        log_e("KOSYNC: WiFi is up - asking home about '%s' (opened before it was)",
              s_waitBook->title);
      } else if (note[0]) {
        log_e("KOSYNC: WiFi is up, but no ask: %s", note);
      }
    }
  }

  if (s_parkWant) {
    s_parkWant = false;
    if (s_parkBook && kosyncPark(s_parkBook, s_parkPct, T->parkDev, s_parkTurned, 0)) {
      s_parked++;
    }
  }

  if (s_reqWant) {
    s_reqWant = false;
    if (callActive) {
      /* A hotspot would take the phone off the WiFi the call is riding on. An automatic window
       * is not worth that. */
      snprintf(T->winLast, sizeof(T->winLast), "No window (%s during a call)", s_reqWhy);
      log_e("KOSYNC: %s - no window, a call is on", s_reqWhy);
    } else if (s_req) {
      char note[96];
      if (!kosyncWindowOpen(s_req, s_reqDurMs, note, sizeof(note))) {
        snprintf(T->winLast, sizeof(T->winLast), "%s", note);
        log_e("KOSYNC: %s - %s", s_reqWhy, note);
      }
    }
  }

  if (s_winArmed) {
    /* The hotspot can be taken away without the transport being told: "WiFi: off" in
     * Settings stops the radio (Networks::disable) whatever is running on it. A window with
     * no hotspot under it is not open — say so rather than count down to nothing. */
    if (xferWindowUp() && xferUsingAP() && !(WiFi.getMode() & WIFI_MODE_AP)) {
      kosyncWindowClose("the hotspot went off (WiFi switched off?)");
    } else if (xferWindowUp() && !xferUsingAP() && (uint32_t)(now - s_staCheckMs) >= 1000u) {
      /* ...and so can the WiFi under a window on the phone's WiFi address: out of range,
       * switched off, or the auto-switcher moved the phone to another network. A few
       * seconds' grace for a blip (none for a switch-off); after that the window is closed,
       * so the next ask (a book opened in the car, Sync my place) brings up the hotspot
       * instead of extending a window nobody can reach. */
      s_staCheckMs = now;
      staWindowGoneClose(now, false);
    }
  }
  if (s_winArmed) {
    if (!xferWindowUp()) {
      // The transport went from under us (the uploader started on another hotspot name).
      s_winArmed = false;
      s_clock.open = false;
      snprintf(T->winLast, sizeof(T->winLast), "Window closed: the uploader took the radio");
      log_e("KOSYNC window CLOSED: the uploader took the transport");
    } else {
      const int due = kosyncClockDue(&s_clock, now);
      if (due == KOSYNC_CLOCK_DEADLINE) {
        kosyncWindowClose((s_gets || s_puts) ? "time is up" : "nobody came");
      } else if (due == KOSYNC_CLOCK_PICKED) {
        char why[80];
        snprintf(why, sizeof(why), "picked up%s%s", T->peer[0] ? " by " : "", T->peer);
        kosyncWindowClose(why);
      } else if ((uint32_t)(now - s_healthMs) >= 15000u) {
        /* THE WINDOW'S HEALTH LINE — the bench's instrument for "what does a window cost":
         * the heap (a hotspot + a listening socket on a phone whose internal RAM is the
         * scarcest thing it has) and whether anyone is actually reaching it. */
        s_healthMs = now;
        log_e("KOSYNC window %lus left %s%s%s gets=%u puts=%u parked=%u 401=%u heap=%u largest=%u",
              (unsigned long)(kosyncClockRemainingMs(&s_clock, now) / 1000), xferAddr(),
              xferUsingAP() ? " ap=" : "", xferUsingAP() ? xferApName() : "",
              (unsigned)s_gets, (unsigned)s_puts, (unsigned)s_parked, (unsigned)s_unauth,
              (unsigned)ESP.getFreeHeap(), (unsigned)largestInternal());
      }
    }
  }

  /* Timed only on a pass that starts a lookup or is in one: the bench's proof that the lookup
   * never waits (a job's own NVS memo save is not a lookup, and is not counted here). */
  const bool looking = s_cs == KS_RESOLVE;
  const uint32_t t0 = micros();
  clientStep(mayUseNetwork, now);
  if (looking || s_cs == KS_RESOLVE) {
    const uint32_t us = micros() - t0;
    if (us > s_lookMaxUs) {
      s_lookMaxUs = us;
    }
  }
}
