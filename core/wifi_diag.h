/*
 * wifi_diag.h — why the station dropped, what it was on, and why a scan would not start.
 *
 * 🔑 BORN OF 2026-09-24 EVENING (phone 2, twice): `MARK disassoc` with HEALTH wifi=5 then wifi=1,
 * a serial `wifi scan` of -2, and only `wifi bounce` brought it back. Every one of those numbers
 * is a CONSEQUENCE; the cause is the disconnect REASON, and the firmware threw it away
 * (processWiFiEvent logged "lost connection" at log_d, and the core's own `Reason: %u` line is
 * log_w — both compiled out, only log_e is in). So the next drop has to explain itself.
 *
 * PURE: no Arduino, no IDF headers — the host suite (tests/test_wifidiag.cpp) compiles this file.
 * The reason and error numbers are IDF 3.3's (esp_wifi_types.h wifi_err_reason_t, esp_wifi.h
 * ESP_ERR_WIFI_*), written out here so the host can test them; the core predicates are copied
 * from arduino-esp32 1.0.6 WiFiGeneric.cpp:392-410 and cited there.
 *
 * ⚠ ONE WRITER PER FIELD, BY DESIGN. WifiDiagLog's ring and counters are written ONLY by the WiFi
 * event task (Networks.cpp processWiFiEvent) — plain stores, no locks, no allocation, no I/O, so
 * it is safe on that task's 4 KB stack. The loop READS the ring (Networks::diagTick) and writes
 * only the RSSI pair (noteRssi). WifiScanStartStats is written only by the loop. Nothing here
 * ever needs a lock because no field has two writers.
 */
#ifndef WIPHONE_WIFI_DIAG_H
#define WIPHONE_WIFI_DIAG_H

#include <stddef.h>
#include <stdint.h>

/* IDF 3.3 disconnect reason -> its name ("BEACON_TIMEOUT"); "?" for a number IDF 3.3 never sends. */
const char* wifiReasonName(unsigned reason);

/* The wl_status_t (as the number HEALTH `wifi=` prints) the 1.0.6 core sets on a STA_DISCONNECTED
 * with this reason, or -1 where it leaves the status alone (AUTH_EXPIRE). WiFiGeneric.cpp:392-403.
 * 🔑 This is how an OLD health.log is read: wifi=5 (CONNECTION_LOST) can only have been reason 200
 * or 204; wifi=1 (NO_SSID_AVAIL) only 201; wifi=4 only 202/203; wifi=6 is everything else. */
int wifiCoreStatusAfter(unsigned reason);

/* Does the core's event-task auto-reconnect run `WiFi.disconnect(); WiFi.begin();` after this
 * reason? WiFiGeneric.cpp:404-410: AUTH_EXPIRE, or any reason >= 200 except AUTH_FAIL — and only
 * while getAutoReconnect(), which Networks.cpp owns (review R1, wifi_policy.h): off after
 * disable() (WiFi off, Disconnect, Forget) until a deliberate join, and held off for a Game Boy
 * game and a hotspot's lifetime (a disconnect handled then is still counted as a rejoin here).
 * No cap, no delay: a NO_AP_FOUND begets another attempt, which begets another NO_AP_FOUND. */
bool wifiCoreRejoinsAfter(unsigned reason);

/* esp_err_t -> name, for the codes esp_wifi_scan_start/connect can return ("ESP_ERR_WIFI_STATE"). */
const char* wifiErrName(int32_t err);

/* The per-reason count table: 1..24 map to themselves, 200..206 to 25..31, anything else to 0. */
#define WIFI_DIAG_BUCKETS 32
int      wifiReasonBucket(unsigned reason);
unsigned wifiBucketReason(int bucket);       // the inverse; bucket 0 -> 0 ("other")

enum : uint8_t {
  WDE_NONE = 0,
  WDE_DISC,       // STA_DISCONNECTED: code = reason; chan/rssi/val = the link it ended, if known
  WDE_CONN,       // STA_CONNECTED:    chan = channel, bssid = the AP joined
  WDE_SCANDONE,   // SCAN_DONE:        code = status (0 = completed, else failed/aborted), val = APs
  WDE_START,      // STA_START: the radio came up
  WDE_STOP,       // STA_STOP:  the radio went down
};

struct WifiDiagEvent {
  uint32_t ms;          // millis() on the event task
  int32_t  val;         // DISC: ms between the last RSSI sample and the drop (-1 = none); SCANDONE: APs
  uint8_t  kind;        // WDE_*
  uint8_t  code;        // DISC: reason; SCANDONE: status
  uint8_t  chan;        // DISC: the channel of the link it ended (0 = unknown); CONN: channel
  int8_t   rssi;        // DISC: the last RSSI sampled on that link (0 = none)
  uint8_t  bssid[6];    // DISC: the AP it was on / tried; CONN: the AP joined
};

class WifiDiagLog {
public:
  static const uint32_t RING = 16;      // power of two; the loop drains it every pass

  WifiDiagLog() {
    reset();
  }
  void reset();

  // ── WRITER: the WiFi event task, and nothing else ──
  void onDisconnect(uint32_t ms, uint8_t reason, const uint8_t* bssid);
  void onConnect(uint32_t ms, const uint8_t* bssid, uint8_t chan);
  void onScanDone(uint32_t ms, uint8_t status, uint32_t aps);
  void onRadio(uint32_t ms, bool started);

  // ── The loop's RSSI sample (its own two fields; the event task only reads them) ──
  void noteRssi(int8_t rssi, uint32_t ms);

  // ── READER: the loop ──
  uint32_t head() const {
    return _head;                       // events ever written; event `seq` lives in slot seq % RING
  }
  /* Copy event `seq` out. False if it is not written yet, or has been (or may be being)
   * overwritten — checked AFTER the copy, so a torn copy is never reported as good. */
  bool read(uint32_t seq, WifiDiagEvent* out) const;

  uint32_t disconnects() const {
    return _disconnects;
  }
  uint32_t coreRejoins() const {
    return _coreRejoins;
  }
  uint32_t connects() const {
    return _connects;
  }
  uint32_t scansDone() const {
    return _scansDone;
  }
  uint32_t scansFailed() const {
    return _scansFailed;
  }
  uint8_t lastReason() const {
    return _lastReason;
  }
  uint16_t count(int bucket) const {
    return (bucket >= 0 && bucket < WIFI_DIAG_BUCKETS) ? _count[bucket] : 0;
  }
  bool linked() const {
    return _linked;
  }
  uint8_t linkChan() const {
    return _connChan;
  }
  void linkBssid(uint8_t out[6]) const;

private:
  void push(const WifiDiagEvent& e);

  WifiDiagEvent     _ring[RING];
  volatile uint32_t _head;
  uint16_t          _count[WIFI_DIAG_BUCKETS];
  volatile uint32_t _disconnects;
  volatile uint32_t _coreRejoins;
  volatile uint32_t _connects;
  volatile uint32_t _scansDone;
  volatile uint32_t _scansFailed;       // SCAN_DONE with a non-zero status: aborted, not "heard nothing"
  volatile uint8_t  _lastReason;
  // The link the station is on (event task): set by CONNECTED, cleared by DISCONNECTED.
  volatile bool     _linked;
  uint8_t           _connBssid[6];
  volatile uint8_t  _connChan;
  volatile uint32_t _connMs;
  // The loop's last RSSI sample while associated.
  volatile int8_t   _rssi;
  volatile uint32_t _rssiMs;
};

/* The scan START counters — written by the loop only (every esp_wifi_scan_start the firmware
 * issues through wifiScanStart() in Networks.cpp). A START refused is a different fault from a
 * scan that ran and heard nothing: ESP_ERR_WIFI_STATE means the station is mid-connect. */
struct WifiScanStartStats {
  uint32_t starts = 0;        // attempts
  uint32_t refused = 0;       // esp_wifi_scan_start != ESP_OK (every attempt: the auto-switcher
                              // retries EVERY loop pass for 5 s, so this runs to thousands)
  uint32_t refusedRuns = 0;   // ...grouped: a run is refusals with no success and < 1 s apart
  uint32_t refusedState = 0;  // ...attempts refused with ESP_ERR_WIFI_STATE ("still connecting")
  uint32_t timedOut = 0;      // a BLOCKING scan that started and never finished in 10 s
  int32_t  lastErr = 0;       // the last refusal's esp_err_t (0 = never refused)
  uint32_t lastRefusedMs = 0; // millis() of it (0 = never)
  bool     inRun = false;

  void note(uint32_t ms, int32_t err);
  void noteTimeout(uint32_t ms);
};

/* " wdis=<disconnects>/<last reason> cr=<core rejoins> ssf=<refusal RUNS>/<last err hex>" —
 * the compact HEALTH field. Returns what snprintf returned. */
int wifiDiagHealthField(char* out, size_t cap, const WifiDiagLog& log, const WifiScanStartStats& ss);

/* "aa:bb:cc:dd:ee:ff" into out[18]; "-" for an all-zero BSSID (a failed join has none). */
void wifiFormatBssid(const uint8_t bssid[6], char out[18]);

/* ── WHICH LINK LINES REACH THE CARD (review F3, 0.9.79) ───────────────────────────────────────
 * Serial gets EVERY `WIFI LOST` / `WIFI JOIN` (and `MARK assoc/disassoc`); /health.log gets at
 * most ONE LOST/JOIN pair a minute. At the edge of an AP the core's capless auto-reconnect can
 * associate and drop every 10-20 s for as long as the phone sits there, and each line is an SD
 * open/append/close (~150-230 B) — at 6 cycles a minute that is 12 card writes a minute where
 * the HEALTH line was cut to 1 on purpose, ~100 KB an hour into a log that keeps 128 KB across a
 * trim: one flapping hour would push the BOOT reset_reason lines — what the trim exists to keep
 * — off the end. The console's `disc` lines were already counted; these were not.
 *
 *   LOST   to the card if no LOST has gone there in the last minute (so the FIRST drop of any
 *          minute keeps its full line: reason, AP, channel, RSSI).
 *   JOIN   to the card if it closes a pair (its LOST went) — a pair is never split — or, on its
 *          own, if nothing at all has gone there in the last minute.
 *   HELD   everything else is serial-only and COUNTED; the counts go to the card as their own
 *          line ("WIFI card: ...") just before the next LOST/JOIN that does, or after a quiet
 *          minute (summaryDue) so the card still ends on how the storm ended.
 * A steady flap every 10 s now costs three card lines a minute (the count, one LOST, its JOIN)
 * where it cost twelve; tests/test_wifidiag.cpp plays that hour through. Pure; loop task only
 * (Networks::diagTick, and a second instance for the MARK lines in WiPhone.ino).
 *
 * 🛑 TWO CLOCKS MEET HERE. Networks::diagTick feeds lost()/join() the EVENT's millis() (stamped
 * by the WiFi event task) but asks summaryDue() with the loop pass's `now`, read at the TOP of the
 * pass — and an event drained later in that same pass is stamped AFTER it. A plain
 * `(uint32_t)(now - lastMs)` then read a card line made milliseconds ago as ~49 days old, and a
 * held count went to the card at once instead of after a quiet minute (the trap wifi_policy.h's
 * wifiJoinAgeMs records for R2). So every age here goes through one rule: a stamp up to
 * WIFI_CARD_AHEAD_MS AHEAD of the time asked about is age 0, and takeHeld() never moves lastMs
 * back. The price, as there: a 60 s window every 49.7 days in which an ancient stamp reads fresh
 * — one line held that would have gone, never one lost. */
#define WIFI_CARD_PERIOD_MS 60000u
#define WIFI_CARD_AHEAD_MS  60000u
struct WifiCardGate {
  uint32_t lastLostMs = 0;    // millis() of the last LOST that went to the card
  uint32_t lastMs = 0;        // ...of the last line of ANY kind that did
  uint32_t heldSinceMs = 0;   // the first held line of the current batch
  uint32_t heldLost = 0;      // LOST lines kept off the card since the last "WIFI card:" line
  uint32_t heldJoin = 0;      // JOIN lines, ditto
  bool     anyLost = false;
  bool     any = false;
  bool     pairOpen = false;  // the last LOST went to the card and its JOIN has not come yet

  bool lost(uint32_t ms);     // true: this LOST goes to the card
  bool join(uint32_t ms);     // true: this JOIN goes to the card
  /* Lines are held and nothing has gone to the card for a minute: write the counts now. */
  bool summaryDue(uint32_t ms) const;
  /* The held counts and when the batch began; true if there were any. Resets them, and counts
   * as a card line at `ms` (the caller writes it) — unless a line already went there LATER than
   * `ms` (the two clocks above), which then stays the last one. */
  bool takeHeld(uint32_t ms, uint32_t* nLost, uint32_t* nJoin, uint32_t* sinceMs);
};
/* How long before `ms` was `stamp`? 0 for a stamp up to WIFI_CARD_AHEAD_MS after `ms` (made later
 * in the same loop pass); wrap-safe otherwise. The gate's only age rule. */
uint32_t wifiCardAgeMs(uint32_t ms, uint32_t stamp);

#endif  // WIPHONE_WIFI_DIAG_H
