/*
Copyright © 2019, 2020, 2021, 2022 HackEDA, Inc.
Licensed under the WiPhone Public License v.1.0 (the "License"); you
may not use this file except in compliance with the License. You may
obtain a copy of the License at
https://wiphone.io/WiPhone_Public_License_v1.0.txt.

Unless required by applicable law or agreed to in writing, software,
hardware or documentation distributed under the License is distributed
on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/

#include "Networks.h"
#include <Preferences.h>
#include "helpers.h"
#include "wifi_diag.h"
#include "wifi_policy.h"   // wifiRestoreDecision: may the station come back, and how
#include "app_gbc_xfer.h"  // xferServing()/xferUsingAP(): a live hotspot owns the radio
#include "geoloc.h"        // geoSwitcherMode: what the auto-switcher's scan may do (0.9.82)
#include "geoloc_net.h"    // geolocOnScan: Wi-Fi location's copy of a finished scan
#include "phone_pos.h"     // phonePosStreamLive: no hop away from a live phone stream

extern void heapEvent(const char* what);   // WiPhone.ino - the ratchet instrument
extern void healthLogLine(const char* line);   // WiPhone.ino - the durable log; LOOP TASK ONLY
extern volatile bool gGbcActive;               // WiPhone.ino - a game owns the card and the radio
#include "esp_wifi.h"

/* 🔎 WHY THE STATION DROPPED (2026-09-25). See wifi_diag.h for the night that made it necessary.
 * s_wd is written ONLY on the WiFi event task (processWiFiEvent), s_ss ONLY on the loop
 * (wifiScanStart) — one writer per field, so neither needs a lock. The loop drains s_wd into
 * log lines in Networks::diagTick(). */
static WifiDiagLog        s_wd;
static WifiScanStartStats s_ss;
/* The last wifiRestoreStation(), for `wifi why`. `who` is always a string literal. LOOP TASK. */
static const char*        s_restoreWho = NULL;
static WifiRestore        s_restoreWhat = WIFI_RESTORE_LEAVE;
static uint32_t           s_restoreMs = 0;

void Networks::getMac(uint8_t* mac) {
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  log_d("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ===================================================== EXTERNS =====================================================

MDNSResponder mdnsResponder;

Networks wifiState;

/* millis() of the last moment the station was known to be UP: its GOT_IP, and a DISCONNECTED
 * that ends a connected spell (a blip, a roam, an AP reboot). NOT a failed join's DISCONNECTED
 * (NO_AP_FOUND and the like arrive with `connected` already false), so a long out-of-range
 * spell leaves it old. Written on the WiFi event task; a 32-bit store is atomic here. */
static volatile uint32_t s_msLastLinkUp = 0;

uint32_t lastWifiLinkUpMs() {
  return s_msLastLinkUp;
}

/* WiFi event handler — runs on the core's `network_event` task (4 KB stack, priority above the
 * loop, same core). ⚠ NOTHING HERE MAY BLOCK, ALLOCATE, LOG OR TOUCH THE CARD: the s_wd calls are
 * plain stores into static memory, and the loop turns them into log lines (diagTick).
 *
 * Takes the whole system_event_t (the WiFiEventSysCb overload — a plain function pointer, no
 * std::function) because the REASON is in event_info, and throwing it away is why the
 * 2026-09-24 drops could not explain themselves. The core's own `Reason: %u` line is log_w and
 * compiled out (only log_e is in this build — sdkconfig CONFIG_ARDUHAL_LOG_DEFAULT_LEVEL 1). */
void processWiFiEvent(system_event_t* ev) {
  const uint32_t ms = millis();
  switch(ev->event_id) {
  case SYSTEM_EVENT_STA_GOT_IP:
    s_msLastLinkUp = millis() | 1u;   // 0 = never
    /* 🛑 TWO WRITE-ONLY SOCKETS USED TO BE OPENED HERE, AND THEY COULD KILL THE PHONE.
     *
     * `udp.begin(localUdpPort)` and `udpRtcp.begin(localUdpPort+1)` each cost a
     * `new char[1460]` inside WiFiUDP::begin(), behind the same DEAD null check as
     * parsePacket() and _scanDone() — and this runs on the WIFI EVENT TASK, so unlike
     * udpParsePacketSafe() there is nowhere to put a try/catch. Two 1,460-byte contiguous
     * INTERNAL requests, on the association that follows a scan that has just taken ~3.6 KB.
     *
     * Nothing ever read either socket. Verified 2026-08-29 across the whole tree: no
     * read/parsePacket/write/beginPacket/available/stop on either, ever. RTP audio uses its
     * own `rtp` socket (Audio.cpp:1500) and the only `udpRtcp` mentions are inside a
     * commented-out block. So this was ~2,920 bytes of internal heap held for the life of
     * the boot, plus an uncatchable throw, in exchange for nothing at all.
     *
     * The `delay(100)` went with them: its comment said it existed solely to stop the
     * begin() calls below it from erroring, so it was blocking the shared event task for
     * 100 ms on every GOT_IP for the sake of code that is now gone. */
    wifiState.setConnected(true, true);
    //When connected set
    log_d("connected! IP address: %s", WiFi.localIP().toString().c_str());
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    /* The reason, and the AP it concerns. By the time this runs the core has ALREADY acted on
     * it (WiFiGeneric.cpp:390-410 runs before the callback list): status set from the reason,
     * and for AUTH_EXPIRE or any reason >= 200 but AUTH_FAIL, `WiFi.disconnect(); WiFi.begin();`
     * — the capless auto-reconnect. wifiCoreRejoinsAfter() mirrors that predicate. */
    s_wd.onDisconnect(ms, ev->event_info.disconnected.reason, ev->event_info.disconnected.bssid);
    if (wifiState.isConnected()) {
      s_msLastLinkUp = ms | 1u;         // up until just now: the core's auto-reconnect follows
    }
    wifiState.setConnected(false, true);
    break;
  case SYSTEM_EVENT_STA_CONNECTED:
    s_wd.onConnect(ms, ev->event_info.connected.bssid, ev->event_info.connected.channel);
    break;
  case SYSTEM_EVENT_WIFI_READY:
    /* ⚠ Also SYNTHESISED on the CALLER's task by the core's espWiFiStart() (WiFiGeneric.cpp:165-168,
     * event_info uninitialised) — which is why nothing is recorded for it: s_wd has one writer. */
    break;
  case SYSTEM_EVENT_SCAN_DONE:
    /* status != 0 = the scan FAILED or was aborted — the core ignores it and reports the result
     * as `n = 0`, indistinguishable from "heard nothing". This is the only place it is visible. */
    s_wd.onScanDone(ms, (uint8_t)(ev->event_info.scan_done.status > 255 ? 255 : ev->event_info.scan_done.status),
                    ev->event_info.scan_done.number);
    break;
  case SYSTEM_EVENT_STA_START:
    s_wd.onRadio(ms, true);
    break;
  case SYSTEM_EVENT_STA_STOP:
    s_wd.onRadio(ms, false);
    break;
  case SYSTEM_EVENT_STA_AUTHMODE_CHANGE:
  case SYSTEM_EVENT_STA_LOST_IP:
  case SYSTEM_EVENT_STA_WPS_ER_SUCCESS:
  case SYSTEM_EVENT_STA_WPS_ER_FAILED:
  case SYSTEM_EVENT_STA_WPS_ER_TIMEOUT:
  case SYSTEM_EVENT_STA_WPS_ER_PIN:
  case SYSTEM_EVENT_AP_START:
  case SYSTEM_EVENT_AP_STOP:
  default:
    break;
  }
}

static uint32_t s_msLastConnectAttempt = 0;

uint32_t lastWifiConnectAttemptMs() {
  return s_msLastConnectAttempt;
}

void noteWifiJoinStarted() {
  s_msLastConnectAttempt = millis();
}

/* The core's auto-reconnect: armed by the owner's policy, held off by the scopes that take the
 * station away (wifi_policy.h has the why). {armed = true: the core's own default, holds = 0}.
 * Applied on EVERY change, so the core's flag is never a copy someone forgot to put back. */
static WifiAutoReconnect s_ar = { true, 0 };

static void wifiAutoReconnectApply() {
  WiFi.setAutoReconnect(wifiArOn(s_ar));
}

void wifiAutoReconnectHold(uint8_t who) {
  wifiArHold(s_ar, who);
  wifiAutoReconnectApply();
}

void wifiAutoReconnectRelease(uint8_t who) {
  wifiArRelease(s_ar, who);
  wifiAutoReconnectApply();
}

/* The policy half: false in disable(), true again on a deliberate join (connectToWiFi) or on
 * "manage WiFi again" (resumeReconnect). Networks.cpp only. */
static void wifiAutoReconnectArm(bool armed) {
  wifiArArm(s_ar, armed);
  wifiAutoReconnectApply();
}

/* 14 dBm, on every join this file starts: connectToWiFi()'s and wifiRestoreStation()'s JOIN (the
 * restore's bare begin() skipped it, and the duplicate retry R2 removed was what re-applied it). */
static void wifiCapTxPower() {
  int rv = 0;
  if ((rv = esp_wifi_set_max_tx_power(56)) != ESP_OK) {
    log_e("failed to limit transmit power: %d", rv);
  }
}

// ===================================================== WHY IT DROPPED =====================================================

/* ── THE SCAN START, WITH THE REASON IT WAS REFUSED ─────────────────────────────────────────
 * WiFi.scanNetworks() answers WIFI_SCAN_FAILED (-2) for FOUR different things: esp_wifi_scan_start
 * refused (radio not started, wrong mode, or — the one that matters — ESP_ERR_WIFI_STATE, "still
 * connecting"), and, in blocking mode, a scan that started and never delivered SCAN_DONE in 10 s.
 * On 2026-09-24 a serial `wifi scan` read -2 and there was no way to say which.
 *
 * This is the core's own scanNetworks() (arduino-esp32 1.0.6 WiFiScan.cpp:57-107), line for line,
 * with its defaults (active, 100-300 ms per channel, no hidden SSIDs, all channels), keeping the
 * esp_err_t it discards. It has to live in a class derived from both WiFiGenericClass and
 * WiFiScanClass only because the bookkeeping it must leave behind (_scanStarted, _scanAsync,
 * _scanTimeout, the SCANNING/DONE status bits) is `protected` there — scanComplete(), _scanDone()
 * and scanDelete() then treat the scan exactly as one the core started.
 * ⚠ If the core is ever bumped, re-diff this against its scanNetworks(). */
namespace {
struct ScanStarter : public WiFiGenericClass, public WiFiScanClass {
  static int16_t start(bool async, int32_t* why, bool* attempted) {
    *why = ESP_OK;
    *attempted = false;
    if (WiFiGenericClass::getStatusBits() & WIFI_SCANNING_BIT) {
      return WIFI_SCAN_RUNNING;                     // one already in flight: not an attempt
    }
    WiFiScanClass::_scanTimeout = 300u * 20u;       // max_ms_per_chan * 20, as the core does
    WiFiScanClass::_scanAsync = async;
    WiFi.enableSTA(true);                           // the "on" half of the deaf-radio bounce
    WiFi.scanDelete();
    wifi_scan_config_t config;
    memset(&config, 0, sizeof(config));
    config.ssid = 0;
    config.bssid = 0;
    config.channel = 0;
    config.show_hidden = false;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 100;
    config.scan_time.active.max = 300;
    *attempted = true;
    const esp_err_t e = esp_wifi_scan_start(&config, false);
    if (e != ESP_OK) {
      *why = (int32_t)e;
      return WIFI_SCAN_FAILED;
    }
    WiFiScanClass::_scanStarted = millis();
    if (!WiFiScanClass::_scanStarted) {
      ++WiFiScanClass::_scanStarted;                // 0 means "no scan" to scanComplete()
    }
    WiFiGenericClass::clearStatusBits(WIFI_SCAN_DONE_BIT);
    WiFiGenericClass::setStatusBits(WIFI_SCANNING_BIT);
    if (async) {
      return WIFI_SCAN_RUNNING;
    }
    if (WiFiGenericClass::waitStatusBits(WIFI_SCAN_DONE_BIT, 10000)) {
      return (int16_t)WiFiScanClass::_scanCount;
    }
    *why = (int32_t)ESP_ERR_TIMEOUT;                // started, but no SCAN_DONE in 10 s
    return WIFI_SCAN_FAILED;
  }
};
}  // namespace

int16_t wifiScanStart(bool async, int32_t* whyOut) {
  int32_t why = ESP_OK;
  bool attempted = false;
  const int16_t r = ScanStarter::start(async, &why, &attempted);
  if (attempted) {
    const uint32_t ms = millis();
    if (why == (int32_t)ESP_ERR_TIMEOUT) {
      s_ss.note(ms, ESP_OK);                        // it DID start...
      s_ss.noteTimeout(ms);                         // ...and never finished
    } else {
      s_ss.note(ms, why);
    }
  }
  if (whyOut) {
    *whyOut = why;
  }
  return r;
}

void wifiDiagNoteRssi(int rssi, uint32_t ms) {
  s_wd.noteRssi((int8_t)rssi, ms);
}

int wifiDiagHealth(char* out, size_t cap) {
  return wifiDiagHealthField(out, cap, s_wd, s_ss);
}

/* WHICH of the link lines reach the card: at most one LOST/JOIN pair a minute, the rest
 * serial-only and counted (review F3; the rules and the reason are wifi_diag.h's WifiCardGate,
 * host-tested). Loop task only, like everything that drains s_wd. */
static WifiCardGate s_card;

/* The gate's count of lines kept off the card, as a card line of its own — just before the next
 * LOST/JOIN that goes there, or after a quiet minute (then with the link's state, so the card
 * ends on how the storm ended). Nothing held: nothing written. noinline: its buffer exists only
 * while it runs, not in diagTick's frame. */
static void __attribute__((noinline)) wifiCardHeldLine(uint32_t ms, const char* linkNow) {
  uint32_t nLost = 0, nJoin = 0, since = 0;
  if (!s_card.takeHeld(ms, &nLost, &nJoin, &since)) {
    return;
  }
  char l[160];
  snprintf(l, sizeof(l), "WIFI card: %lu LOST + %lu JOIN line(s) since t=%lu.%lus were serial-only "
           "(one pair a minute here)%s%s", (unsigned long)nLost, (unsigned long)nJoin,
           (unsigned long)(since / 1000), (unsigned long)((since % 1000) / 100),
           linkNow ? " - link now " : "", linkNow ? linkNow : "");
  log_e("%s", l);
  if (!gGbcActive) {
    healthLogLine(l);
  }
}

/* One place that writes a link line to the console and, when the gate let it through, to the
 * durable log (after any held count, so the card reads in order). The card is the loop's (never
 * the event task's) and the Game Boy's while a game runs. */
static void wifiDiagDurable(const char* line, bool card, uint32_t ms) {
  if (card) {
    wifiCardHeldLine(ms, NULL);
  }
  log_e("%s", line);
  if (card && !gGbcActive) {
    healthLogLine(line);
  }
}

/* Drain the event ring into log lines. LOOP TASK, every pass (a compare when nothing happened).
 *
 *   WIFI LOST  the drop that ENDS a link: reason, the AP, its channel, the last RSSI and how
 *              long before the drop it was sampled. Also written to health.log — at most one
 *              LOST/JOIN pair a minute there (s_card above; `WIFI card:` counts the rest).
 *   WIFI disc  every later disconnect of the same spell — a core auto-reconnect storm is a
 *              train of these (NO_AP_FOUND every few seconds), so a run of one reason prints
 *              twice and then is COUNTED, with a summary line at least once a minute.
 *   WIFI JOIN  associated: the AP, channel, RSSI, and for a spell the bill — how long down,
 *              how many disconnects, how many of them the core answered with its own begin(),
 *              and how many scan starts were refused. Also written to health.log (s_card).
 *   WIFI scan  a SCAN_DONE that failed/aborted or found nothing (the core reports both as n=0).
 *   WIFI radio STA started/stopped (a bounce, the Game Boy, WiFi off). */
void Networks::diagTick(uint32_t now) {
  static uint32_t s_next = 0;                 // the next event sequence to print
  static bool     s_linkView = false;         // a CONNECTED drained since the last DISCONNECTED
  static bool     s_spell = false;            // a LOST printed, no JOIN since
  static uint32_t s_spellMs = 0;
  static uint32_t s_spellDisc = 0;
  static uint32_t s_spellCr = 0;
  static uint32_t s_spellSsf0 = 0;
  static uint8_t  s_spellLast = 0;
  static uint8_t  s_lostBssid[6] = {0};
  static uint8_t  s_runReason = 0;            // the reason of the current run of disconnects
  static uint32_t s_runLen = 0;
  static uint32_t s_runHidden = 0;
  static uint32_t s_runSaidMs = 0;

  const uint32_t head = s_wd.head();
  if (head == s_next && !(s_runHidden && (uint32_t)(now - s_runSaidMs) >= 60000u) &&
      !s_card.summaryDue(now)) {
    return;                                   // the common case: nothing new
  }
  char line[232];
  char b[18];
  uint32_t missed = 0;
  if ((uint32_t)(head - s_next) >= WifiDiagLog::RING) {
    missed += (uint32_t)(head - s_next) - (WifiDiagLog::RING - 1);
    s_next = head - (WifiDiagLog::RING - 1);
  }

  auto flushRun = [&]() {
    if (s_runHidden) {
      log_e("WIFI disc reason=%u %s x%lu more (counted, not printed)", (unsigned)s_runReason,
            wifiReasonName(s_runReason), (unsigned long)s_runHidden);
      s_runHidden = 0;
    }
    s_runSaidMs = now;
  };

  for (; s_next != head; s_next++) {
    WifiDiagEvent e;
    if (!s_wd.read(s_next, &e)) {
      missed++;
      continue;
    }
    const unsigned long tS = e.ms / 1000, tD = (e.ms % 1000) / 100;
    switch (e.kind) {
    case WDE_DISC: {
      wifiFormatBssid(e.bssid, b);
      const bool coreRejoins = wifiCoreRejoinsAfter(e.code);
      if (s_linkView) {
        flushRun();
        s_linkView = false;
        s_spell = true;
        s_spellMs = e.ms;
        s_spellDisc = 1;
        s_spellCr = coreRejoins ? 1 : 0;
        s_spellSsf0 = s_ss.refusedRuns;
        s_spellLast = e.code;
        memcpy(s_lostBssid, e.bssid, 6);
        s_runReason = e.code;
        s_runLen = 1;
        if (e.rssi != 0) {
          snprintf(line, sizeof(line),
                   "WIFI LOST reason=%u %s ap=%s ch=%u rssi=%d (sampled %ldms before) t=%lu.%lus%s",
                   (unsigned)e.code, wifiReasonName(e.code), b, (unsigned)e.chan, (int)e.rssi,
                   (long)e.val, tS, tD, coreRejoins ? " - core rejoining" : "");
        } else {
          snprintf(line, sizeof(line), "WIFI LOST reason=%u %s ap=%s ch=%u rssi=? t=%lu.%lus%s",
                   (unsigned)e.code, wifiReasonName(e.code), b, (unsigned)e.chan, tS, tD,
                   coreRejoins ? " - core rejoining" : "");
        }
        wifiDiagDurable(line, s_card.lost(e.ms), e.ms);
        break;
      }
      if (s_spell) {
        s_spellDisc++;
        s_spellCr += coreRejoins ? 1 : 0;
        s_spellLast = e.code;
      }
      if (s_runLen > 0 && e.code == s_runReason) {
        if (++s_runLen > 2) {
          s_runHidden++;
          break;                              // counted; flushed on a change or once a minute
        }
      } else {
        flushRun();
        s_runReason = e.code;
        s_runLen = 1;
      }
      log_e("WIFI disc reason=%u %s ap=%s t=%lu.%lus%s", (unsigned)e.code, wifiReasonName(e.code),
            b, tS, tD, coreRejoins ? " - core rejoining" : "");
      break;
    }
    case WDE_CONN: {
      flushRun();
      s_runLen = 0;
      s_linkView = true;
      wifiFormatBssid(e.bssid, b);
      const int rssiNow = (int)WiFi.RSSI();   // 0 if it has already gone again
      if (s_spell) {
        snprintf(line, sizeof(line),
                 "WIFI JOIN ap=%s ch=%u rssi=%d (%s AP) after %lus down: %lu disconnects "
                 "(last %u %s), %lu core rejoins, %lu refused-scan-start runs%s%s t=%lu.%lus",
                 b, (unsigned)e.chan, rssiNow,
                 memcmp(e.bssid, s_lostBssid, 6) == 0 ? "same" : "OTHER",
                 (unsigned long)((uint32_t)(e.ms - s_spellMs) / 1000), (unsigned long)s_spellDisc,
                 (unsigned)s_spellLast, wifiReasonName(s_spellLast), (unsigned long)s_spellCr,
                 (unsigned long)(s_ss.refusedRuns - s_spellSsf0),
                 (s_ss.refusedRuns != s_spellSsf0) ? " last " : "",
                 (s_ss.refusedRuns != s_spellSsf0) ? wifiErrName(s_ss.lastErr) : "", tS, tD);
        s_spell = false;
      } else {
        snprintf(line, sizeof(line), "WIFI JOIN ap=%s ch=%u rssi=%d t=%lu.%lus", b,
                 (unsigned)e.chan, rssiNow, tS, tD);
      }
      wifiDiagDurable(line, s_card.join(e.ms), e.ms);
      break;
    }
    case WDE_SCANDONE:
      if (e.code != 0 || e.val == 0) {
        log_e("WIFI scan event: %s (status=%u) aps=%ld t=%lu.%lus",
              e.code ? "FAILED/aborted" : "completed, heard NOTHING", (unsigned)e.code,
              (long)e.val, tS, tD);
      }
      break;
    case WDE_START:
      log_e("WIFI radio started t=%lu.%lus", tS, tD);
      break;
    case WDE_STOP:
      if (s_linkView) {
        /* A radio stopped while associated delivers no DISCONNECTED (the bounce-v1 lesson,
         * Networks::bounceRadio) — so this IS the end of that link. */
        s_linkView = false;
        s_spell = true;
        s_spellMs = e.ms;
        s_spellDisc = 0;
        s_spellCr = 0;
        s_spellSsf0 = s_ss.refusedRuns;
        s_spellLast = 0;
        memset(s_lostBssid, 0, sizeof(s_lostBssid));
        snprintf(line, sizeof(line), "WIFI LOST radio stopped while associated t=%lu.%lus", tS, tD);
        wifiDiagDurable(line, s_card.lost(e.ms), e.ms);
      } else {
        log_e("WIFI radio stopped t=%lu.%lus", tS, tD);
      }
      break;
    default:
      break;
    }
  }
  if (missed) {
    log_e("WIFI diag: %lu event(s) overwritten before the loop could print them",
          (unsigned long)missed);
  }
  if (s_runHidden && (uint32_t)(now - s_runSaidMs) >= 60000u) {
    flushRun();                               // a storm still running says so once a minute
  }
  if (s_card.summaryDue(now)) {
    wifiCardHeldLine(now, s_linkView ? "UP" : "down");   // the card hears how the storm ended
  }
}

/* `wifi` / `wifi why` over the cable: everything above in one screen, plus the driver's own view.
 * LOOP TASK (the serial console). Reads s_wd without draining it. */
void Networks::diagPrint(void (*out)(const char*)) {
  char l[200];
  char b[18];
  const uint32_t now = millis();

  /* autoReconnect is the core's flag as it is NOW; (armed, held) is why (R1: held 1 = a game,
   * 2 = a hotspot). mode=3 (AP+STA) under a live sync window is the R1 race having happened. */
  snprintf(l, sizeof(l), "wifi why: status=%d conn=%d mode=%d autoReconnect=%d (armed=%d held=%u) "
           "radioOff=%d ud=%d rec=%d",
           (int)WiFi.status(), (int)connected, (int)WiFi.getMode(), (int)WiFi.getAutoReconnect(),
           (int)s_ar.armed, (unsigned)s_ar.holds, (int)_radioOff, (int)_userDisabled, (int)reconnect);
  out(l);
  {
    const char* by = wifiStationBlockedBy();
    snprintf(l, sizeof(l), "  restore: station wanted=%d (owner allows=%d, held by: %s); last '%s' -> %s %lds ago",
             (int)wifiStationWanted(), (int)stationAllowed(), by ? by : "nobody",
             s_restoreWho ? s_restoreWho : "-", s_restoreWho ? wifiRestoreName(s_restoreWhat) : "-",
             s_restoreWho ? (long)((uint32_t)(now - s_restoreMs) / 1000) : -1L);
    out(l);
  }

  /* The config the CORE's auto-reconnect rejoins with (begin() with no arguments re-uses it).
   * An empty or wrong SSID here makes every core rejoin NO_AP_FOUND for ever. 🛑 The password
   * is never printed — only whether there is one. */
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
    snprintf(l, sizeof(l), "  driver STA config: ssid='%.32s' pw=%s bssid_set=%d channel=%u scan_method=%d sort=%d",
             (const char*)conf.sta.ssid, conf.sta.password[0] ? "set" : "EMPTY",
             (int)conf.sta.bssid_set, (unsigned)conf.sta.channel, (int)conf.sta.scan_method,
             (int)conf.sta.sort_method);
  } else {
    snprintf(l, sizeof(l), "  driver STA config: unreadable (driver not initialised)");
  }
  out(l);

  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    wifiFormatBssid(ap.bssid, b);
    snprintf(l, sizeof(l), "  link: ap=%s ch=%u rssi=%d ssid='%.32s'", b, (unsigned)ap.primary,
             (int)ap.rssi, (const char*)ap.ssid);
  } else {
    uint8_t lb[6];
    s_wd.linkBssid(lb);
    wifiFormatBssid(lb, b);
    snprintf(l, sizeof(l), "  link: NOT associated (last joined ap=%s ch=%u)", b,
             (unsigned)s_wd.linkChan());
  }
  out(l);

  snprintf(l, sizeof(l), "  since boot: %lu disconnects (%lu answered by the core's own begin()), %lu joins; "
           "scan events %lu (%lu failed/aborted)",
           (unsigned long)s_wd.disconnects(), (unsigned long)s_wd.coreRejoins(),
           (unsigned long)s_wd.connects(), (unsigned long)s_wd.scansDone(),
           (unsigned long)s_wd.scansFailed());
  out(l);
  snprintf(l, sizeof(l), "  health.log: last LOST there %lds ago, last line %lds ago; held off it now: "
           "%lu LOST + %lu JOIN (one pair a minute; the rest are serial-only)",
           s_card.anyLost ? (long)((uint32_t)(now - s_card.lastLostMs) / 1000) : -1L,
           s_card.any ? (long)((uint32_t)(now - s_card.lastMs) / 1000) : -1L,
           (unsigned long)s_card.heldLost, (unsigned long)s_card.heldJoin);
  out(l);
  for (int k = 0; k < WIFI_DIAG_BUCKETS; k++) {
    if (!s_wd.count(k)) {
      continue;
    }
    const unsigned r = wifiBucketReason(k);
    snprintf(l, sizeof(l), "    reason %3u %-24s x%u%s", r, k ? wifiReasonName(r) : "(other)",
             (unsigned)s_wd.count(k), wifiCoreRejoinsAfter(r) ? "  (core rejoins after it)" : "");
    out(l);
  }
  snprintf(l, sizeof(l), "  scan starts: %lu, refused %lu in %lu run(s) (%lu 'still connecting'), "
           "timed out %lu; last refusal %s (0x%lx) %lds ago",
           (unsigned long)s_ss.starts, (unsigned long)s_ss.refused, (unsigned long)s_ss.refusedRuns,
           (unsigned long)s_ss.refusedState, (unsigned long)s_ss.timedOut, wifiErrName(s_ss.lastErr),
           (unsigned long)(uint32_t)s_ss.lastErr,
           s_ss.lastRefusedMs ? (long)((uint32_t)(now - s_ss.lastRefusedMs) / 1000) : -1L);
  out(l);
  snprintf(l, sizeof(l), "  autosw: scanning=%d pending=%d dryScans=%u dryBounced=%d discScans=%lu "
           "sinceScan=%lus longDry=%d joins=%lu/%lu lastJoinAttempt=%lus ago",
           (int)_scanning, (int)_scanPending, (unsigned)_dryScans, (int)_dryBounced,
           (unsigned long)_discScans, (unsigned long)((uint32_t)(now - _msLastScan) / 1000),
           (int)inLongDrySpell(), (unsigned long)_joinsTried, (unsigned long)_joinsSkipped,
           (unsigned long)((uint32_t)(now - lastWifiConnectAttemptMs()) / 1000));
  out(l);

  out("  recent events (oldest first):");
  const uint32_t head = s_wd.head();
  const uint32_t first = (head >= WifiDiagLog::RING - 1) ? head - (WifiDiagLog::RING - 1) : 0;
  for (uint32_t s = first; s != head; s++) {
    WifiDiagEvent e;
    if (!s_wd.read(s, &e)) {
      continue;
    }
    wifiFormatBssid(e.bssid, b);
    const long ago = (long)((uint32_t)(now - e.ms) / 1000);
    switch (e.kind) {
    case WDE_DISC:
      snprintf(l, sizeof(l), "    %5lds ago  DISC reason=%u %s ap=%s ch=%u rssi=%d%s", ago,
               (unsigned)e.code, wifiReasonName(e.code), b, (unsigned)e.chan, (int)e.rssi,
               wifiCoreRejoinsAfter(e.code) ? " (core rejoined)" : "");
      break;
    case WDE_CONN:
      snprintf(l, sizeof(l), "    %5lds ago  CONN ap=%s ch=%u", ago, b, (unsigned)e.chan);
      break;
    case WDE_SCANDONE:
      snprintf(l, sizeof(l), "    %5lds ago  SCAN_DONE status=%u aps=%ld", ago, (unsigned)e.code,
               (long)e.val);
      break;
    case WDE_START:
      snprintf(l, sizeof(l), "    %5lds ago  STA_START (radio up)", ago);
      break;
    case WDE_STOP:
      snprintf(l, sizeof(l), "    %5lds ago  STA_STOP (radio down)", ago);
      break;
    default:
      continue;
    }
    out(l);
  }
}

bool connectToWiFi(const char* ssid, const char* pwd) {

  // TODO: do not connect while scanning for networks

  /* 🛑 NOT UNDER A GAME, NOT UNDER A LIVE HOTSPOT (0.9.79). This is the tree's only
   * WiFi.begin(ssid, pwd), and begin() opens with enableSTA(true) — so any join path reaching
   * here starts the radio. Under a Game Boy game that is esp_wifi_start() beside the emulator
   * (the loop's retry did exactly that for a phone that was off WiFi when the game began); under
   * the uploader's or a sync window's softAP it is the station connect WiPhone.ino's reconnect
   * gate calls a chip panic (reachable from Settings > WiFi's Connect with a headless `up on`).
   * The callers' own gates should never let it get here; this is the one place that makes it
   * impossible rather than unlikely. Refused BEFORE the attempt stamp: nothing started. */
  if (const char* by = wifiStationBlockedBy()) {
    log_e("WIFI join REFUSED: %s", by);
    return false;
  }

  s_msLastConnectAttempt = millis();   // every join path stamps this — see Networks.h
  /* A deliberate join re-arms the core's auto-reconnect that disable() disarmed (R1): from here a
   * NO_AP_FOUND is retried by the core as it always was. Held scopes (a game, a hotspot) still win,
   * though the refusal above means none is live here. */
  wifiAutoReconnectArm(true);

  log_d("Connecting to network: %s", ssid);

  /* 🔑 `false`, NOT `true`. The argument is `wifioff`, so WiFi.disconnect(true) calls
   * esp_wifi_stop() and tears the whole radio down just to reassociate — and MEASURED on this
   * phone that costs **5007 ms**, against 30 ms for the WiFi.begin() that follows it:
   *
   *     SLOW WIFI: connectToWiFi [disconnect(true)=5007 begin=30 other=0]
   *
   * Everything here is one task, so those five seconds froze the keypad, the screen and the
   * WiFi stack together. That is the freeze {user} reported while scrolling — it needed the
   * hotspot to blip, which is why it was rare and why it always came with "and WiFi dropped":
   * the drop was not a symptom of the freeze, it was the trigger for it.
   *
   * A plain disassociate is all a reconnect needs; WiFi.begin() sets the new config regardless,
   * so "delete old config" was never doing work that begin() would not redo.
   * ⚠ The FULL cycle still exists deliberately elsewhere (`WiFi.disconnect(true, true)` in the
   * hard reset) — this is only the reconnect path. */
  const uint32_t _wt0 = millis();
  WiFi.disconnect(false);
  const uint32_t _wtDisc = millis();
  wifiState.setConnected(false, false);

  /* ⚠ THE EVENT HANDLER IS REGISTERED ONCE, IN Networks::init() — NOT HERE.
   *
   * This used to call WiFi.onEvent(processWiFiEvent) on every attempt, and
   * WiFiGenericClass::onEvent only ever does cbEventList.push_back() — it does not
   * deduplicate, and removeEvent() is never called anywhere in this firmware. So the
   * list grew by one entry per connect attempt and NEVER shrank except at reboot.
   *
   * That matters because connectToWiFi() is the RETRY: WiPhone.ino:1667 calls it every
   * WIFI_RETRY_PERIOD_MS (20 s) for as long as there is no network. Out of range for the
   * length of a car journey — 143 minutes, measured — that is roughly 430 registrations.
   *
   * Then the moment WiFi comes back, ONE SYSTEM_EVENT_STA_GOT_IP runs processWiFiEvent
   * N times, and this handler is not free: each pass does delay(100) plus udp.begin() and
   * udpRtcp.begin(), and WiFiUDP::begin() calls stop() (delete[] tx_buffer) then
   * `new char[1460]`. At N=430 that is ~43 s of blocking delay and ~1,720 alloc/free
   * cycles of 1,460 INTERNAL bytes, from one reconnect.
   *
   * ⚠ There is no malloc→PSRAM auto-diversion in this build to soften that: the framework
   * sdkconfig.h defines CONFIG_SPIRAM_USE_CAPS_ALLOC and NOT CONFIG_SPIRAM_USE_MALLOC, so
   * plain new/malloc is internal at EVERY size. (docs/MUSIC.md's "threshold is 16 KB" is
   * wrong for this build — there is no threshold, only heap_caps_malloc reaches PSRAM.)
   *
   * This is the best candidate found for the measured signature of the reset_reason=4
   * panics: free heap recovers while the LARGEST FREE BLOCK never does, and it degrades
   * on WiFi events rather than with uptime. The registry growth is monotonic, which is
   * exactly the "never recovers" part. */

  //Initiate connection
  const uint32_t _wtPre = millis();
  WiFi.begin(ssid, pwd);
  const uint32_t _wtBegin = millis();
  if (_wtBegin - _wt0 > 150) {
    log_e("SLOW WIFI: connectToWiFi [disconnect=%u begin=%u other=%u]",
          (unsigned)(_wtDisc - _wt0), (unsigned)(_wtBegin - _wtPre),
          (unsigned)(_wtPre - _wtDisc));
  }

  // Limit transmit power to 14 dBm
  wifiCapTxPower();

  /* ── WiFi MODEM SLEEP ─────────────────────────────────────────────────────────────
   * The radio was running with power save OFF, meaning the receiver stayed powered
   * continuously — tens of milliamps, forever, on a phone that spends most of its life
   * in a pocket doing nothing. MIN_MODEM parks the radio between the access point's DTIM
   * beacons and wakes for each one.
   *
   * ⚠ It still receives everything. Broadcast and buffered traffic is delivered at the
   * DTIM interval (typically 100–300 ms), so an incoming SIP INVITE or a page load is
   * delayed by up to that, not lost. That is the right trade for a phone; MAX_MODEM
   * would save more and can miss beacons, which is not.
   *
   * Set after begin() on purpose: the driver resets the power-save mode when the station
   * starts, so setting it earlier is silently undone. */
  int rv = 0;
  if ((rv = esp_wifi_set_ps(WIFI_PS_MIN_MODEM)) != ESP_OK) {
    log_e("failed to enable wifi modem sleep: %d", rv);
  }

  log_d("Waiting for connection...");
  return true;
}

// Inspired by: https://github.com/nkolban/esp32-snippets/blob/master/cpp_utils/WiFi.cpp
// Alternative way: use dns_gethostbyname. See: https://gist.github.com/MakerAsia/37d2659310484bdbba9d38558e2c3cdb
/* 🛑 THIS BLOCKS THE MAIN LOOP, AND THE UI IS SINGLE-THREADED.
 *
 * Everything the user sees runs on the same task, so every millisecond spent in here is a
 * millisecond the screen is frozen. Reached from SIP connect (tinySIP.cpp), from RTP setup
 * DURING a call (WiPhone.ino) and from NTP (clock.cpp).
 *
 * Reported symptom that led here: "sometimes when clicking menus the phone will freeze for
 * like 5 seconds". Two blocking calls in series were doing it:
 *
 *   1. An mDNS query with a 500 ms timeout, run for EVERY name — including public ones like
 *      `seattle1.voip.ms` and `pool.ntp.org`. mDNS only ever answers for `.local` hosts, so
 *      that half second was guaranteed wasted every single time. The overnight log is full of
 *      `seattle1.voip.ms not found on local network`; that line IS the wasted 500 ms.
 *   2. lwip_gethostbyname(), which retries internally and can block for SEVERAL SECONDS when a
 *      DNS server is slow or unreachable. `errno=210: unable to resolve "pool.ntp.org"` in the
 *      same log is that, and NTP keeps retrying it.
 *
 * So: only ask mDNS about names it could possibly answer for, and cache what DNS returns so a
 * reconnect does not pay for it twice.
 *
 * ⚠ `.local` answers are deliberately NOT cached. mDNS names move with the network — the
 * documented `wiphone.local` staleness trap is exactly that — so they must stay live. Only
 * public DNS answers are cached, and only for TTL_MS. */
IPAddress resolveDomain(const char* hostName) {
  if (hostName == NULL || !hostName[0]) {
    return IPAddress((uint32_t)0);
  }

  /* mDNS can only answer for single-label names and `.local`. Anything else is a public name,
   * and asking mDNS about it is a guaranteed half second of nothing. */
  const char* dot = strchr(hostName, '.');
  const bool couldBeLocal = (dot == NULL) || (strcasecmp(dot, ".local") == 0);

  /* ⚠ FAILURES ARE CACHED TOO, and that half matters more than the successes.
   *
   * Measured on the device 2026-08-17: `pool.ntp.org` failed to resolve SEVEN times in two
   * minutes, because NTP retries on TIME_UPDATE_RETRY_DELAY_MS (500 ms) and every retry is a
   * fresh blocking lwip_gethostbyname(). On a network where that name does not resolve — a
   * work WiFi with restrictive DNS, say — the phone spends whole seconds inside this function,
   * over and over, with the UI frozen because it is all one task.
   *
   * A negative entry makes a failing name cost NOTHING until NEG_TTL_MS has passed, no matter
   * how eagerly the caller retries. That is the general fix: it protects against any caller
   * hammering any name that will not resolve, not just this one. Kept much shorter than the
   * positive TTL so a genuinely transient DNS outage recovers quickly. */
  static const int      RESOLVE_CACHE_N = 6;
  static const uint32_t TTL_MS     = 10u * 60u * 1000u;    // 10 min for a good answer
  static const uint32_t NEG_TTL_MS = 60u * 1000u;          // 1 min for a failure
  struct CacheEntry {
    char     host[64];
    uint32_t addr;        // 0 == negative entry (this name did not resolve)
    uint32_t at;          // 0 == slot never used
  };
  static CacheEntry s_cache[RESOLVE_CACHE_N] = {};
  static uint8_t    s_next = 0;
  const uint32_t    nowMs = millis() | 1u;                 // never 0, so `at` marks "used"

  if (!couldBeLocal) {
    for (int i = 0; i < RESOLVE_CACHE_N; i++) {
      if (!s_cache[i].at || strcmp(s_cache[i].host, hostName)) {
        continue;
      }
      const uint32_t age = (uint32_t)(nowMs - s_cache[i].at);
      if (s_cache[i].addr && age < TTL_MS) {
        return IPAddress(s_cache[i].addr);                 // known good: no blocking call
      }
      if (!s_cache[i].addr && age < NEG_TTL_MS) {
        return IPAddress((uint32_t)0);                     // known bad: fail instantly
      }
      break;                                                // stale — fall through and re-look
    }
  }

  if (wifiState.mdnsOk && couldBeLocal) {
    IPAddress addr = mdnsResponder.queryHost(hostName, 500);
    if (addr) { // where is the class definition of IPAddress? Need to know if this is a valid way to test if addr is set.
      log_i("resolved: %s -> %d.%d.%d.%d", hostName, addr[3], addr[2], addr[1], addr[0]);
      return addr;
    } else {
      log_e("%s not found on local network", hostName);
    }
  }

  unsigned long retAddr;
  struct hostent* he = lwip_gethostbyname(hostName);
  if (he != nullptr) {
    retAddr = *(unsigned long*) (he->h_addr_list[0]);       // take only first address
    log_d("resolved: %s -> %d.%d.%d.%d", hostName, retAddr & 0xFF, (retAddr >> 8) & 0xFF, (retAddr >> 16) & 0xFF, (retAddr >> 24) & 0xFF);
  } else {
    retAddr = 0;
    log_e("errno=%d: unable to resolve \"%s\"", h_errno, hostName);
  }

  /* Record the outcome either way — a failure is exactly what we must remember. */
  if (!couldBeLocal && strlen(hostName) < sizeof(s_cache[0].host)) {
    int slot = -1;
    for (int i = 0; i < RESOLVE_CACHE_N; i++) {       // reuse this name's slot if it has one
      if (s_cache[i].at && !strcmp(s_cache[i].host, hostName)) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      slot = s_next;
      s_next = (s_next + 1) % RESOLVE_CACHE_N;
    }
    strlcpy(s_cache[slot].host, hostName, sizeof(s_cache[0].host));
    s_cache[slot].addr = (uint32_t)retAddr;           // 0 here means "known bad"
    s_cache[slot].at   = nowMs;
  }
  return IPAddress(retAddr);
}

// ===================================================== WIFI STATE =====================================================

Networks::Networks() : ini(filename), _userDisabled(false) {
  prefSsidDyn = NULL;
  wifiSsidDyn = NULL;
  wifiPassDyn = NULL;
  connected = false;
  reconnect = true;
}

Networks::~Networks() {
  freeNull((void **) &prefSsidDyn);
  freeNull((void **) &wifiSsidDyn);
  freeNull((void **) &wifiPassDyn);
}

void Networks::init() {
  /* Register the WiFi event handler EXACTLY ONCE, for the life of the boot. It used to be
   * done inside connectToWiFi(), i.e. on every retry — see the long note there for why that
   * was the most likely cause of the reset_reason=4 panics. Guarded as well as moved, so
   * that a second call to init() cannot quietly reintroduce the bug. */
  static bool s_eventHandlerRegistered = false;
  if (!s_eventHandlerRegistered) {
    s_eventHandlerRegistered = true;
    WiFi.onEvent(processWiFiEvent);
  }

  // Reset WiFi (these are needed for proper scanning!!!)
  WiFi.mode(WIFI_STA);
  log_v("Free memory after wifi mode: %d", ESP.getFreeHeap());
  WiFi.disconnect();
  const char* host = "WiPhone"; // Later append serial number here
  log_v("Free memory after disconnect: %d", ESP.getFreeHeap());
  if (mdnsResponder.begin(host)) {
    mdnsOk = true;
    log_i("MDNS Responder Hostname: %s", host);
  } else {
    mdnsOk = false;
    log_e("MDNS Responder Hostname: %s failed to initialize", host);
  }

  log_v("Free memory after responder begin: %d", ESP.getFreeHeap());
  delay(100);
}

// ===================================================== NETWORK CONNECTIONS =====================================================

void Networks::disconnect() {
  WiFi.disconnect(true, true);  // wifioff = true, eraseap = true (erasing might be needed when using Arduino-ESP32 ver. >= 1.0.3)
  connected = false;
  reconnect = false;
}

/*
 * Disable the radio.
 */

void Networks::disable() {
  /* Every caller of disable() is an explicit user action (the menu WIFI toggle OFF, Remove
   * network, the Disconnect button, the edit screen's WIFI-OFF) — so say so in the LIVE
   * flag, not just the INI. Before 2026-08-27 only loadPreferred() ever set _userDisabled
   * (from the INI, typically at boot), so a Disconnect pressed mid-session left the live
   * flag false — and anything gating on userDisabled() was reading a stale answer. The
   * flag is cleared by the symmetric acts: a deliberate connectTo() or resumeReconnect(). */
  _userDisabled = true;
  /* 🛑 THE CORE'S AUTO-RECONNECT IS DISARMED FIRST (0.9.79 review R1), before disconnect() takes
   * the station away. Left armed, one NO_AP_FOUND still queued from a hunting station - the state
   * a phone is in when its owner switches WiFi off because the network is gone - is answered on
   * the event task by `WiFi.disconnect(); WiFi.begin();`, and begin() on the radio stopped below
   * is mode(STA) from NULL: esp_wifi_start() behind "WiFi: off", for the rest of the boot. The
   * mode(OFF)/esp_wifi_stop() after it only won the interleavings where the handler finished
   * first. Re-armed by the symmetric acts: connectToWiFi() (a deliberate join) and
   * resumeReconnect() (WiFi on). */
  wifiAutoReconnectArm(false);
  disconnect();
  WiFi.mode(WIFI_OFF);
  /* 🛑 NO btStop() HERE, EVER (0.9.75). It was a no-op — the Bluetooth controller is never
   * started in this firmware — but merely REFERENCING btStop() links esp32-hal-bt.c, whose
   * strong btInUse() returns true, and that is the one test initArduino() makes before it
   * would hand the 56 KB Bluetooth DRAM reserve back to the heap at boot. So one dead line
   * "to save power" cost every phone ~40 KB of internal RAM for as long as it has existed;
   * the Game Boy reclaimed it on its first launch, nothing else ever saw it (see the BOOT
   * line in WiPhone.ino setup(), and CHANGELOG 0.9.74/0.9.75). Verified in the ELF: the
   * only reference to btStop was this line, and btInUse() compiled to `movi a2,1; retw`. */
  esp_wifi_stop(); //likely unnecessary
  log_d("WiFi disabled");
}

/* Description:
 *      load password for a network and set the network as current network (SSID/pass get remembered)
 */
bool Networks::loadNetworkSettings(const char* ssid) {
  log_d("loadNetworkSettings: %s", ssid);
  // TODO: unload ini after using if it is too big (or there is no PSRAM)
  // TODO: consider that there might be multiple networks with the same name
  ini.unload();
  if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
    int i = ini.query("s", ssid);
    if (i>=0 && ini[i].hasKey("p")) {   // check correctness        TODO: use mnemonics for these tiny key names
      // One network found
      log_d("found");
      freeNull((void **) &wifiSsidDyn);
      freeNull((void **) &wifiPassDyn);
      wifiSsidDyn = strdup(ssid);
      wifiPassDyn = strdup(ini[i]["p"]);
      return true;
    }
  }
  return false;
}

/* Desctiption:
 *      load name of the preferred network
 */
void Networks::loadPreferred() {
  log_d("loadPreferred");
  freeNull((void **) &prefSsidDyn);
  ini.unload();
  if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
    int i = ini.findKey("m");
    if (i>=0 && ini[i].hasKey("s")) {
      // Preferred network found
      log_d("preferred network = %s", ini[i]["s"]);
      prefSsidDyn = strdup(ini[i]["s"]);
      const char *disabled = ini[i]["disabled"];
      log_d("loadPreferred: 0x%x", disabled);
      if (disabled != NULL) {
        log_d("loadPreferred is: %s", disabled);
        if (strcmp(disabled, "true") == 0) {
          _userDisabled = true;
        } else {
          _userDisabled = false;
        }
      }
    }
  }
}

bool Networks::connectTo(const char* ssid) {
  log_d("connectTo");
  /* Split-timed: loadNetworkSettings() reads networks.ini from SPIFFS, and SPIFFS `open` alone
   * costs ~1.6 s on this part (measured with `bench`), while connectToWiFi() is documented as
   * async. One of those two claims is wrong and this says which. */
  const uint32_t t0 = millis();
  /* Nothing joins an empty SSID (0.9.79 review). Only an INI that already holds a blank network
   * (the edit screen's Save used to write one) could "load" it, and WiFi.begin("") starts the
   * station and then refuses to connect — a radio up for nothing, and on the edit screen with
   * WiFi off, the switch turned ON by a Connect that did nothing. */
  if (ssid == NULL || !ssid[0]) {
    return false;
  }
  // Load password and connect to WiFi network
  const bool loaded = this->loadNetworkSettings(ssid);
  const uint32_t tIni = millis();
  bool r = false;
  if (loaded) {
    /* ── A DELIBERATE JOIN RESTORES THE RECONNECT FLAG (found 2026-08-27) ──────
     * `reconnect` is set true in the constructor and false in disconnect()/disable(),
     * and NOTHING ever set it back — so the WiFi settings screen's save path (which
     * calls disconnect() before rejoining) left every phone that ever had a network
     * EDITED in a state where it would never auto-rejoin again until reboot. That is
     * the real shape of phone 1's months of "keeps losing WiFi": not the radio, not
     * SIP, not hardware — a one-way flag, flipped by the very screen used to fix the
     * WiFi. Every caller of connectTo() (the settings screen, connectToPreferred,
     * the auto-switch hop) is expressing "I want to be on this network", which is
     * exactly what reconnect means. The same argument clears _userDisabled (added
     * 2026-08-27 when disable() started setting it live): a deliberate join IS the user
     * re-enabling WiFi, and the settings screen's join path writes disabled=false to the
     * INI in the same breath — the live flag must agree with it.
     * ⚠ ONLY FOR A JOIN THAT STARTED (0.9.79 review). connectToWiFi() refuses under a game or a
     * live hotspot, and a refused Connect shows the owner nothing happening (the edit screen
     * leaves its button at "Connect" and writes no disabled=false) — so the flags must not say
     * otherwise either: cleared here, they had a Disconnected network rejoin by itself the
     * moment the hotspot came down, with the INI still saying Disconnected. */
    r = connectToWiFi(wifiSsidDyn, wifiPassDyn);    // "async"; false = refused, nothing started
    if (r) {
      reconnect = true;
      _userDisabled = false;
      _joinsTried++;                            // instrument: pairs with _joinsSkipped
    }
  }
  const uint32_t total = millis() - t0;
  if (total > 150) {
    log_e("SLOW WIFI: connectTo %u ms [loadNetworkSettings=%u connectToWiFi=%u]",
          (unsigned)total, (unsigned)(tIni - t0), (unsigned)(millis() - tIni));
  }
  return r;
}

bool Networks::hasPreferredSsid(void) {
  log_d("hasPreferredSsid");
  // Do we have a saved default WiFi network?
  if (prefSsidDyn==NULL) {
    this->loadPreferred();
  }
  if (prefSsidDyn==NULL) {
    log_d("SSID NOT LOADED");
    return false;
  }
  return true;
}

bool Networks::connectToPreferred(void) {
  log_d("connectToPreferred");
  /* MEASURED 2026-08-24: this call blocks the WHOLE superloop for ~5 s when the hotspot
   * blips, which is the freeze {user} feels while scrolling. Split-timed here to say which
   * half — the INI read (SPIFFS, and `open` alone costs 1.6 s on this part) or the join. */
  const uint32_t t0 = millis();
  // Load name of preferred WiFi network
  // TODO: find the exact preferred network, not just find the name and then search by name
  if (prefSsidDyn==NULL) {
    this->loadPreferred();
  }
  const uint32_t tLoad = millis();
  if (prefSsidDyn==NULL) {
    log_d("SSID NOT LOADED");
    return false;
  }
  const bool r = connectTo(prefSsidDyn);
  const uint32_t total = millis() - t0;
  if (total > 150) {
    log_e("SLOW WIFI: connectToPreferred %u ms [loadPreferred=%u connectTo=%u]",
          (unsigned)total, (unsigned)(tLoad - t0), (unsigned)(millis() - tLoad));
  }
  return r;
}

/* Radio OFF and back to STA, with the state layer told the truth FIRST (2026-08-27).
 * The opening plain disconnect() matters: issued while associated it delivers
 * STA_DISCONNECTED, which is what clears `connected`. The first cut of `wifi bounce`
 * went straight to disconnect(true) — radio off swallowed that event, `connected`
 * stayed true, and BOTH rescue loops (retry and auto-switch) sat gated on it: an
 * associated bench phone was wedged half-down until a reflash. Found live, same day
 * the command was written. The explicit `connected = false` after the delay is belt
 * and braces — recovery must not depend on event-delivery timing. Ends by making the
 * next auto-switch scan due immediately, so the cure is followed by the look-around
 * that uses it. Does NOT touch `reconnect`, does NOT eraseap. */
void Networks::bounceRadio(void) {
  WiFi.disconnect();            // event → connected=false while the radio can still say so
  delay(400);
  connected = false;
  WiFi.disconnect(true /*radio OFF*/);
  delay(300);
  WiFi.mode(WIFI_STA);          // back up, idle
  _dryScans = 0;
  _dryBounced = false;          // a deliberate bounce starts the spell accounting over
  _msLastScan = millis() - 600000u;   // next autoSwitchTick: a scan is due NOW
}

/* "We were not looking, so there is nothing to have found."
 *
 * 🛑 A RADIO WE SWITCHED OFF IS NOT A DRY SPELL (2026-09-07, phone 1, from the field log).
 * autoSwitchTick() maintains _drySpellStartMs BEFORE its own gates — deliberately, so the
 * clock stays honest while the auto-switcher is off — but `connected` is also false with the
 * radio powered DOWN, and nothing cleared the stamp on the way back up. A phone whose radio
 * had been off ~22 h therefore came back already 22 hours into a "long dry spell", and all
 * four readers of inLongDrySpell() drew the wrong conclusion at the same moment:
 *
 *   autoSwitchTick():671      the deaf-radio cure was DISABLED (it requires !inLongDrySpell)
 *   worthAttemptingJoin():809 the join gate stopped failing open and consulted scan evidence
 *   currentDiscPeriod():838   the scan cadence started at five minutes instead of two
 *   WiPhone.ino:3207          the join retry started at ten minutes
 *
 * MEASURED that day: both scans after the switch completed n=0 while SIX APs were on the air
 * (the documented mid-connect deaf state — see _dryScans in the header), and because an empty
 * scan stamps _scanDoneMs and clears _savedSeenLastScan, a SENSOR FAILURE was written down as
 * fresh, confident evidence that the air was empty. The phone logged the consequence verbatim:
 * "[wifi] join skipped: last scan saw no saved network (1 in a row, 1 total)". With the cure
 * ten minutes away and joins suppressed, the owner gave up and joined by hand 7 s later.
 *
 * Clearing the stamp here restarts the clock on the next tick, from the moment the radio
 * actually came up. The 2/5/10-minute easing then rebuilds exactly as before — a phone that
 * really is somewhere without WiFi is back on the eased cadence five minutes later, which is
 * the situation the easing was written for. What it no longer does is arrive pre-eased.
 *
 * The stale deaf verdict goes with it: _dryBounced means "this spell's deaf hypothesis has
 * already been tested", and a verdict about a radio that has since been powered down is
 * worthless. Nothing else cleared it across an off/on round trip.
 *
 * ⚠ CALLED FROM THE RADIO-ON PATHS, NOT FROM autoSwitchTick(). The tick is skipped during a
 * call, under the Game Boy and under a softAP transfer (WiPhone.ino:3307), while the retry
 * loop that reads inLongDrySpell() is skipped only by the transfer — so doing this on the
 * transition is what makes it atomic. No reader can catch the stale spell. */
void Networks::forgetDrySpell(void) {
  _drySpellStartMs = 0;               // restarts on the next tick, from NOW
  _dryScans = 0;
  _dryBounced = false;                // a verdict about a radio that no longer exists
  _discScans = 0;                     // the backoff rounds start over with the spell
  _joinsSkippedRun = 0;
  /* No evidence about THIS radio's air yet. worthAttemptingJoin() fail-opens on
   * _scanDoneMs == 0, which is the "try immediately when WiFi is switched on" half. */
  _scanDoneMs = 0;
  _savedSeenLastScan = false;
  _msLastScan = millis() - 600000u;   // ...and look NOW, not one period from now
}

/* "Manage WiFi again": the symmetric partner of disable(), without naming a network.
 * Called by the WIFI-ON toggles (menu and edit screen) and by NetworksApp's destructor
 * for the peek-and-back-out case. Arms the retry loop and the auto-switcher; the actual
 * join is theirs — blocking the UI on a join is what froze the edit screen once. */
void Networks::resumeReconnect(void) {
  _userDisabled = false;
  reconnect = true;
  wifiAutoReconnectArm(true);         // disable() disarmed the core's auto-reconnect (R1)
  forgetDrySpell();                   // the spell starts now, not when the radio went off
}

// ===================================================== GIVING THE RADIO BACK =====================================================
/* See wifi_policy.h for the bug class and the order the inputs win in, and Networks.h for the
 * rule every caller follows. Everything here runs on the LOOP task. */

const char* wifiStationBlockedBy() {
  if (gGbcActive) {
    return "a Game Boy game is running";
  }
  if (xferServing() && xferUsingAP()) {
    return "a hotspot is live (uploader or sync window)";
  }
  return NULL;
}

bool wifiStationWanted() {
  return wifiStationWantedFrom(wifiState.radioOff(), wifiState.userDisabled(), gGbcActive,
                               xferServing() && xferUsingAP());
}

bool wifiRestoreStation(const char* who) {
  const uint32_t now = millis();
  const uint32_t lastJoin = lastWifiConnectAttemptMs();
  WifiRestoreIn in;
  in.radioOff     = wifiState.radioOff();
  in.userDisabled = wifiState.userDisabled();
  in.reconnect    = wifiState.doReconnect();
  in.gameActive   = gGbcActive;
  in.softApLive   = xferServing() && xferUsingAP();
  /* 🛑 WiFi.status(), NOT wifiState.isConnected(). A radio stopped while associated delivers no
   * DISCONNECTED (the bounceRadio note), so after a game `connected` can still read TRUE — and a
   * restore that believed it would LEAVE the phone off its network until the next reboot. The
   * core's STA_STOP handler does set status() to WL_NO_SHIELD, so this one tells the truth. */
  in.staConnected = WiFi.status() == WL_CONNECTED;
  /* ⚠ YOUNG ONLY WHILE ITS STATION RUNS (0.9.79 review; wifiJoinInFlight). The rule is "do not
   * re-begin under an association still in flight", and a join whose station has since been
   * stopped is not in flight: a hotspot's WiFi.mode(WIFI_AP) (transportUp), startGame's
   * disconnect(true) and a disable() all killed it. Counted young anyway, the restore came up
   * idle and the rejoin waited for the loop's next retry — 20 s, or 3 min after five failures,
   * with nothing to hurry it — where the old restoreStation() began at once. getMode() reads the
   * core's own started flag, which every Arduino path to the radio sets (mode(), begin(), scans). */
  in.joinYoung    = wifiJoinInFlight((WiFi.getMode() & WIFI_MODE_STA) != 0, now, lastJoin);
  in.longDrySpell = wifiState.inLongDrySpell();
  WifiRestore d = wifiRestoreDecision(in);
  bool ok = true;
  const char* note = "";
  switch (d) {
  case WIFI_RESTORE_OFF:
    ok = WiFi.mode(WIFI_OFF);
    break;
  case WIFI_RESTORE_UP_IDLE:
    /* ⚠ WiFi.mode(WIFI_STA), never a bare esp_wifi_start(): mode() SETS the mode before it
     * starts the driver, where esp_wifi_start() restarts whatever the driver last had — AP, after
     * a sync window closed with WiFi off (softAPdisconnect -> mode(NULL) never resets the
     * driver's mode), which put an unserved soft-AP back on the air. */
    ok = WiFi.mode(WIFI_STA);
    break;
  case WIFI_RESTORE_JOIN: {
    ok = WiFi.mode(WIFI_STA);
    if (!ok) {
      break;                  // said below; the Settings toggle turns that into "off"
    }
    /* begin() with no arguments rejoins whatever STA config the DRIVER holds. Read AFTER the
     * mode switch (the call can fail with the station down). An erased config — disconnect(),
     * Forget, Settings > WiFi's constructor — has nothing to join: come up idle and let the
     * loop's retry join with the saved credentials instead of hunting an empty SSID. */
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0]) {
      WiFi.begin();
      /* A join in flight: the young-join rules see it - the quiesce, the scans, KS-T1, and (R2)
       * the loop's own join retry, which later in this same pass used to re-begin over it. */
      noteWifiJoinStarted();
      wifiCapTxPower();        // what connectToWiFi() applies; the retry that re-applied it is gone
    } else {
      d = WIFI_RESTORE_UP_IDLE;
      note = " - nothing in the driver's config, the loop's retry joins";
    }
    break;
  }
  case WIFI_RESTORE_LEAVE:
  default:
    break;
  }
  s_restoreWho = who;
  s_restoreWhat = d;
  s_restoreMs = now;
  /* Loud, like the audio watchdog: each line names a site handing the radio back and what it was
   * allowed to do. Console only, not health.log — with auto=on a sync window closes on every
   * book close. */
  log_e("WIFI restore (%s): %s%s [ro=%d ud=%d rec=%d game=%d ap=%d conn=%d young=%d dry=%d]%s",
        who ? who : "?", wifiRestoreName(d), note, (int)in.radioOff, (int)in.userDisabled,
        (int)in.reconnect, (int)in.gameActive, (int)in.softApLive, (int)in.staConnected,
        (int)in.joinYoung, (int)in.longDrySpell, ok ? "" : " - WiFi.mode() FAILED");
  return ok;
}

// ===================================================== WIFI AUTO-SWITCH =====================================================
// Scans in the background (async, keeps the current association) and hops to
// the strongest saved network. Driven by autoSwitchTick() from the main loop.

#define AUTO_SCAN_PERIOD_MS      600000u  // scan every 10 min while connected (battery)
#define AUTO_SCAN_DISC_PERIOD_MS 120000u  // every 2 min while disconnected (nothing to lose)
#define AUTO_SCAN_RETRY_MS       30000u   // re-try soon after a failed/aborted scan
#define AUTO_SWITCH_MARGIN_DB 10        // hop only if this much stronger than current

bool Networks::autoSwitchEnabled(void) {
  if (_autoSwitch < 0) {                // lazy-load once; cached afterwards
    _autoSwitch = 1;                    // default: enabled
    ini.unload();
    if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
      const char* v = ini[0]["autosw"];
      if (v != NULL && !strcmp(v, "0")) {
        _autoSwitch = 0;
      }
    }
  }
  return _autoSwitch == 1;
}

void Networks::setAutoSwitch(bool enabled) {
  _autoSwitch = enabled ? 1 : 0;
  ini.unload();
  if (ini.load() || ini.restore()) {
    ini[0]["autosw"] = enabled ? "1" : "0";
    ini.store();
  }
}

void Networks::autoSwitchTick(bool screenOn) {
  bool wake = screenOn && !_prevScreenOn;
  _prevScreenOn = screenOn;

  // Diagnostic line (log_e = visible over serial @500000) — ON STATE CHANGE, not on a
  // timer. The 15 s heartbeat wrote ~5,700 identical lines a day into the field log and
  // buried the lines that matter; the states it reports change a handful of times a day.
  // The feature has failed silently twice, so the line itself stays — every transition of
  // any gate still prints, which is what the heartbeat was really for.
  {
    static uint32_t s_lastState = 0xFFFFFFFF;
    const uint32_t state = ((uint32_t)_userDisabled)
                         | ((uint32_t)reconnect          << 1)
                         | ((uint32_t)autoSwitchEnabled() << 2)
                         | ((uint32_t)connected          << 3)
                         | ((uint32_t)_scanning          << 4);
    if (state != s_lastState) {
      s_lastState = state;
      log_e("[autosw] ud=%d rec=%d en=%d conn=%d scanning=%d sinceScan=%lus",
            (int)_userDisabled, (int)reconnect, (int)autoSwitchEnabled(),
            (int)connected, (int)_scanning, (unsigned long)((millis() - _msLastScan) / 1000));
    }
  }

  /* Maintain the dry-spell clock BEFORE the gates below, so it is honest even while the
   * auto-switcher is switched off — the retry in loop() reads the same predicate. millis()
   * can legitimately be 0 for one millisecond after boot, so 1 is used as the sentinel-safe
   * stamp; the tick runs at ~1 kHz, so at most one millisecond of spell is lost. */
  if (connected) {
    _drySpellStartMs = 0;
    _joinsSkippedRun = 0;               // back on a network: the valve starts over
  } else if (_drySpellStartMs == 0) {
    _drySpellStartMs = millis() | 1u;
  }

  /* A Wi-Fi location scan is asked for a CONNECTED station only (requestGeoScan): one asked
   * before a drop is not carried into the disconnected machinery or the next association. */
  if (!connected) {
    _geoScanReq = false;
  }

  /* stationAllowed(), not _userDisabled alone (0.9.79): with the switch OFF and the per-network
   * flag cleared by an Edit > Save, this scanned — and wifiScanStart() starts the radio.
   *
   * ── WHAT THIS TICK MAY DO (0.9.82: geoloc.h's geoSwitcherMode, the review's item 8) ─────────
   *   FULL       the switcher exactly as it always was (auto-switch on): scan on its cadence,
   *              evaluate a hop, and the disconnected branch below.
   *   SCAN_ONLY  scan and hand the snapshot to Wi-Fi location (geolocOnScan), NEVER evaluate a
   *              hop and NEVER the disconnected branch (its WiFi.disconnect() and the deaf-radio
   *              bounce): auto-switch OFF with a Wi-Fi location scan asked while CONNECTED - the
   *              owner switched the switcher off, and a location feature must not start its
   *              disconnect-and-retry machinery - or auto-switch ON while the hotspot phone's
   *              NMEA stream is live (a stronger saved network would trade the better position
   *              source for a worse one).
   *   NONE       nothing: the station not allowed, reconnect off, or auto-switch off with no scan
   *              asked - the gates the reconnect loop honours, as before.
   * A Wi-Fi location round already begun (pending or in flight, !_scanFull) still "wants a
   * scan": the ask is spent when the round starts, and without this the very next tick would
   * read NONE with auto-switch off and abandon the round half-started.
   * ⚠ scanBusy() HOLDS THE RECONNECT LOOP OFF, so a round must never be left hanging: when the
   * mode reads NONE, a scan IN FLIGHT is polled to its end (pollOnly - e.g. a Wi-Fi location
   * scan whose link dropped mid-scan) and a round still only PENDING is called off. Either way
   * nothing new starts. (Polling only while the station is allowed and reconnect is on, as
   * before: the radio-on paths finish the rest.) */
  const bool geoRound = (_scanPending || _scanning) && !_scanFull;
  const int mode = geoSwitcherMode(stationAllowed(), reconnect, autoSwitchEnabled(),
                                   _geoScanReq || geoRound, connected, phonePosStreamLive());
  if (mode == GEO_SW_NONE && _scanPending) {
    _scanPending = false;               // never started: nothing to poll, nothing held off
  }
  const bool pollOnly = mode == GEO_SW_NONE && _scanning && stationAllowed() && reconnect;
  if (mode == GEO_SW_NONE && !pollOnly) {
    return;                             // same gates as the reconnect loop honors
  }

  uint32_t now = millis();

  if (_scanning) {                      // poll the async scan
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      return;
    }
    _scanning = false;
    /* May THIS scan move the station? Only when its round began in FULL mode and the tick is
     * still FULL now (a phone stream that came up meanwhile holds the hop too). Otherwise it is
     * a look at the air for Wi-Fi location and nothing else: the switcher's join evidence
     * (_scanDoneMs, _savedSeenLastScan - never evaluated, so never true) and its deaf-radio
     * count are left exactly as they were. */
    const bool full = _scanFull && mode == GEO_SW_FULL;
    if (n > 0) {
      log_e("[autosw] scan done: n=%d%s", n, full ? "" : " (for Wi-Fi location: no hop)");
      wifiScanNoteResult(n);          // size the next scan's heap estimate to THIS air
      heapEvent("scan-post");         // pairs with scan-pre: did this scan cost anything permanent?
      _dryScans = 0;
      _dryBounced = false;
      /* Wi-Fi location's copy (geoloc_net.h): {BSSID, RSSI, flags} read through
       * WiFi.getScanInfoByIndex() - no Strings - BEFORE WiFi.scanDelete() below frees the
       * records. Nothing at all when Wi-Fi location is off. Before the evaluate: a hop changes
       * the association the snapshot is stamped with. */
      geolocOnScan(n);
      if (full) {
        _scanDoneMs = now | 1u;         // evidence: a scan completed; evaluate says what it saw
        /* Cleared BEFORE evaluate and set inside it, so the flag always describes THIS scan.
         * Setting it after would leave the previous scan's answer standing whenever evaluate
         * takes one of its early returns. */
        _savedSeenLastScan = false;
        autoSwitchEvaluate(n);
      }
    } else if (n == 0 && !full) {
      log_e("[autosw] scan done: n=0 (for Wi-Fi location: not counted)");
      if (!_scanKeepCadence) {
        _msLastScan = now;
      }
    } else if (n == 0) {
      /* 🛑 A COMPLETED SCAN THAT FOUND NOTHING IS the out-of-range case — OR THE DEAF
       * RADIO (2026-08-27): after hours of disconnected retry churn the driver can reach a
       * state where scans complete empty forever while the AP is on air at -50 dBm. The two
       * are indistinguishable from one result, so count them: out of range clears on the
       * walk home anyway, and the deaf state gets a radio bounce at _dryScans >= 2 (below).
       * Either way the scan is stamped as ordinary so the 2/5-minute easing does its job —
       * taking the retry branch here used to defeat that backoff; see scheduleScanRetry(). */
      if (!connected && _dryScans < 1000) {
        _dryScans++;
      }
      log_e("[autosw] scan done: n=0 dry=%u", (unsigned)_dryScans);
      _msLastScan = now;
      _scanDoneMs = now | 1u;           // evidence: a scan completed and saw nothing at all
      _savedSeenLastScan = false;       // n==0: definitively nothing on the air
    } else {
      /* n < 0: the scan was ABORTED or the API failed — commonly a reconnect attempt
       * cycled WiFi and killed it, or the radio was mid-connect. Learned nothing about the
       * AIR, so try again soon rather than serving out a full period. But it IS evidence
       * about the MACHINERY (2026-08-27, the booksync wedge): the mid-connect churn that
       * presents as deafness alternates 0 and -2 completions, and counting only the zeros
       * let the wedge outlive the detector for a whole afternoon. Disconnected, a failed
       * completion counts toward the bounce like an empty one. (Not a Wi-Fi location scan's:
       * the bounce is the FULL switcher's business - `full` above.) */
      if (full && !connected && _dryScans < 1000) {
        _dryScans++;
      }
      log_e("[autosw] scan done: n=%d dry=%u", n, (unsigned)_dryScans);
      /* A Wi-Fi location round answered under the FULL switcher (below) leaves the switcher's
       * clock alone: a retry here would be a FULL round 30 s on, with its evaluate. Wi-Fi
       * location asks again GEO_SCAN_REQ_GAP_MS later if it still wants one. */
      if (!_scanKeepCadence) {
        scheduleScanRetry(now, connected);
      }
    }
    WiFi.scanDelete();
    return;
  }

  if (mode == GEO_SW_NONE) {
    return;                             // pollOnly: the scan in flight was the only business
  }

  /* A Wi-Fi location ask under the FULL switcher BEFORE its own cadence makes a round due is
   * answered with a SCAN_ONLY round too (the review, 2026-10-10): a FULL round runs
   * autoSwitchEvaluate(), whose ini.unload()/load() of /networks.ini is 0.5-1.6 s of SPIFFS per
   * open on the loop task plus a String per access point - a LOOP STALL (and music dropouts) for
   * every ask, when all Wi-Fi location needs is the snapshot. It also cannot hop: right after
   * joining a phone's hotspot, before its NMEA has arrived (phonePosStreamLive() still false), a
   * geo-asked FULL round could move the station to a stronger saved network. Such a round leaves
   * _msLastScan alone (_scanKeepCadence): the switcher's own 10-minute evaluate still comes on
   * time, however often Wi-Fi location asks. Connected only, as every geo ask is. */
  const bool geoOnly = mode == GEO_SW_FULL && connected && _geoScanReq &&
                       now - _msLastScan < AUTO_SCAN_PERIOD_MS;
  if (!_scanPending && (mode == GEO_SW_SCAN_ONLY || geoOnly)) {
    /* CONNECTED by construction (geoSwitcherMode, and geoOnly): none of the disconnected
     * machinery below can run from here. Due on the switcher's own cadence while auto-switch is
     * on and a live phone stream holds the HOP (the scans still confirm a Wi-Fi fix), or when
     * Wi-Fi location asked. The start that follows obeys the same heap gate as every other scan.
     * ⚠ A Wi-Fi location ask WAITS while an AI question, a weather fetch or a look is on the
     * HTTPS worker (geolocScanMayStart): this scan's driver allocations and _scanDone's
     * `new wifi_ap_record_t[n]` land 1-4 s after the start, on the WiFi event task where a throw
     * cannot be caught, and inside a handshake's ~12 KB internal-heap dip that is the 2026-08-28
     * reboot. The ask stays pending; the round starts when the worker is idle. */
    const bool cadence = mode == GEO_SW_SCAN_ONLY && autoSwitchEnabled() &&
                         now - _msLastScan >= AUTO_SCAN_PERIOD_MS;
    const bool asked = _geoScanReq && geolocScanMayStart();
    if (!cadence && !asked) {
      return;
    }
    if (!geoOnly) {
      _msLastScan = now;
    }
    _discScans = 0;                     // on a network, as the FULL path's connected round does
    _dryScans = 0;
    _dryBounced = false;
    _geoScanReq = false;                // spent: geoloc asks again GEO_SCAN_REQ_GAP_MS later
    _scanFull = false;
    _scanKeepCadence = geoOnly;
    _scanPending = true;
    _msScanPendingSince = now;
    _scanPreMarked = false;
  } else if (!_scanPending) {
    /* ⚠ Back off when there is plainly nothing to join. Disconnected, this scanned every
     * two minutes forever, and a scan lights up the radio for a few hundred milliseconds
     * — so a phone carried out of range all afternoon paid for thirty scans an hour to
     * learn the same thing thirty times.
     *
     * The first few stay at two minutes, which covers the common case of stepping briefly
     * out of range and wanting a quick rejoin. Only a sustained absence (roughly ten
     * minutes of failures) eases to five, and the counter resets the moment anything
     * connects, so coming home is still prompt. */
    uint32_t discPeriod = currentDiscPeriod();
    /* Deaf-evidence fast path (2026-08-27): with dry evidence on the books and no bounce
     * tried yet this spell, do not serve out the eased 2-5 min period — confirm and cure
     * within ~90 s. Capped to the first bounce (see _dryBounced in the header), so a phone
     * genuinely out of range still gets the battery easing. */
    /* ⚠ NOT during a long dry spell. This fast path exists to confirm-and-cure a DEAF RADIO
     * within ~90 s, and it is right to be aggressive for that — but a phone that has simply
     * been away from any network for ten minutes is not deaf, and letting this branch pull the
     * cadence back to 30 s would undo the easing above entirely. The bounce it leads to has
     * also already had its chance by then (_dryBounced). */
    if (!connected && _dryScans >= 1 && !_dryBounced && !inLongDrySpell() &&
        discPeriod > AUTO_SCAN_RETRY_MS) {
      discPeriod = AUTO_SCAN_RETRY_MS;
    }
    /* (A Wi-Fi location ask is never what makes this round due: connected, it was answered above
     * as a SCAN_ONLY round unless the cadence was due anyway, and disconnected it was dropped.) */
    bool due = (now - _msLastScan >= (connected ? AUTO_SCAN_PERIOD_MS : discPeriod));
    if (wake && !connected) {
      /* Screen woke with no WiFi: scan right away. Backdating instead of a bare due=true
       * keeps the wish STICKY — the young-join guard below may hold the scan for a few
       * seconds (the wake RETRY fires earlier in this same loop pass), and a transient
       * flag would have been lost with it. */
      _msLastScan = now - discPeriod;
      due = true;
    }
    if (!due) {
      return;
    }
    if (!connected &&
        (uint32_t)(now - lastWifiConnectAttemptMs()) < 10000u) {
      /* A join younger than 10 s is still associating/DHCPing — it reads as 'not
       * connected' but is about to not be. The pre-scan disconnect below would abort it:
       * measured 2026-08-27, the screen-wake retry and the wake-forced scan killed each
       * other in the SAME loop pass, and at the 20 s retry cadence the mutual kill is
       * what keeps the radio perpetually mid-connect (= every scan completes empty).
       * `due` stays true, so the scan runs the moment the attempt is stale. */
      return;
    }
    _msLastScan = now;
    if (connected) {
      _discScans = 0;                   // on a network: forget the dry spell
      _dryScans = 0;
      _dryBounced = false;
      _geoScanReq = false;              // this round answers a Wi-Fi location ask too
    } else if (_discScans < 1000) {
      _discScans++;
    }
    _scanFull = true;                   // a FULL round: its result may move the station
    _scanKeepCadence = false;
    _scanPending = true;                // scanBusy() now holds the reconnect loop off
    _msScanPendingSince = now;
    _scanPreMarked = false;             // this round's scan-pre mark is still to be taken
    if (!connected) {
      if (_dryScans >= 2) {
        /* THE DEAF-RADIO RECOVERY (2026-08-27, measured on phone 1 at the work desk):
         * two consecutive scans that completed EMPTY while the twin radio heard the AP at
         * -50 dBm. No amount of scanning recovers from this state — screen wake at 240 MHz
         * read n=0 too — but a radio off/on clears it instantly (a rebooted radio heard 6
         * networks and SIP registered inside a minute). This is exactly what the user's
         * "open the WiFi screen and rescan" ritual did by accident: NetworksApp's ctor is
         * disconnect(true, true). Done here deliberately — WITHOUT eraseap, the driver's
         * remembered AP is not the disease — the next scanNetworks() below re-enables STA,
         * which is the "on" half. Counter resets so a genuinely out-of-range phone bounces
         * at most every second round (~10 min), not every scan. */
        log_e("[autosw] %u consecutive empty scans: restarting the radio before this one",
              (unsigned)_dryScans);
        /* ...and to the card (2026-09-25): the one line that proves the self-cure fired, on a
         * phone that was in a pocket with no cable. Rare by construction (at most one per
         * spell's two dry rounds), so one SD append costs nothing. The console text above is
         * kept as it was: the handoff's grep recipes look for "restarting the radio". */
        if (!gGbcActive) {
          char bl[112];
          snprintf(bl, sizeof(bl), "WIFI BOUNCE (auto): %u dry scan rounds (empty, failed or "
                   "refused) - restarting the radio up=%lus", (unsigned)_dryScans,
                   (unsigned long)(now / 1000));
          healthLogLine(bl);
        }
        _dryScans = 0;
        _dryBounced = true;             // this spell had its bounce: cadence re-eases
        WiFi.disconnect(true /*radio OFF; the scan start turns it back on*/);
      } else {
        // The Arduino WiFi driver auto-reconnects on "AP not found", so with the
        // preferred network absent the radio is perpetually mid-connect and scan
        // starts get rejected — the auto-switcher looked completely dead. A plain
        // disconnect stops that cycle (its disconnect reason is one the driver
        // does NOT auto-reconnect from) and frees the radio to scan.
        WiFi.disconnect();
      }
      return;                           // give the driver a tick to settle; start next pass
    }
  }

  // Start (or keep trying to start) the scan. The disconnect above settles
  // asynchronously, so the first attempts can be rejected — retrying every
  // tick for a few seconds beats losing a 30s-backoff race against the radio
  // (that race is why a 5-minute wait once produced zero completed scans).
  /* 🛑 THE ALLOCATION THAT KILLED PHONE 1 ON 2026-08-28 IS THE ONE THIS SCAN IS ABOUT TO
   * MAKE. _scanDone() will `new wifi_ap_record_t[n]` — ~192 contiguous internal bytes per
   * access point, measured — on the WiFi event task, where nothing can catch the throw.
   * This is the unattended scan: it runs every few minutes on an idle phone in a pocket,
   * so it is the one that turns a fragmented heap into a reboot while nobody is looking.
   * Skipping a round costs nothing — we stay on the AP we already have. See helpers.h. */
  if (!wifiScanMemoryOk("autosw")) {
    _scanPending = false;
    if (!_scanKeepCadence) {            // a Wi-Fi location round: geoloc asks again later
      scheduleScanRetry(now, WiFi.status() == WL_CONNECTED);
    }
    return;
  }
  /* ⚠ ONCE PER ROUND, NOT ONCE PER ATTEMPT (2026-09-25). A refused start is retried on EVERY
   * loop pass for up to 5 s, and heapEvent() is a log_e plus an SD open/append/close — so each
   * round of refusals wrote one "MARK scan-pre" line to health.log per ATTEMPT (as many as the
   * loop could turn in 5 s with an SD write in each pass), every ~30 s, for as long as the
   * station stayed wedged: the instrument flooding the very log meant to explain the drop.
   * (A burst of consecutive scan-pre marks with no scan-post in an OLD log is therefore the
   * fingerprint of refused starts.) The mark brackets the scan's allocation; the heap a few ms
   * into the round is the same heap. */
  if (!_scanPreMarked) {
    _scanPreMarked = true;
    heapEvent("scan-pre");            // the scan is the prime remaining suspect: bracket it
  }
  int32_t startErr = 0;
  int16_t r = wifiScanStart(true /*async*/, &startErr);   // keeps an existing association
  if (r != WIFI_SCAN_FAILED) {
    log_e("[autosw] scan started (wifi status %d)", (int)WiFi.status());
    _scanPending = false;
    _scanning = true;
    return;
  }
  if (now - _msScanPendingSince > 5000) {
    /* 🛑 A ROUND OF REFUSED STARTS IS DRY EVIDENCE TOO (2026-09-25, from phone 2's two drops
     * the night before: wifi=5 then wifi=1, a serial scan of -2, cured only by `wifi bounce`).
     * _dryScans — the counter that triggers the deaf-radio bounce above — was only ever
     * advanced by COMPLETED scans (n == 0 and n < 0, in the _scanning branch). A start that
     * esp_wifi_scan_start REFUSES never gets there: this branch only rescheduled. So a station
     * held mid-connect — every start refused with ESP_ERR_WIFI_STATE while the core's capless
     * auto-reconnect keeps it "connecting" (wifiCoreRejoinsAfter) — could NEVER reach
     * `_dryScans >= 2`, and the self-bounce written for exactly that state could never fire:
     * down until a human bounced the radio or rebooted. Counted here, disconnected only, the
     * same way the n < 0 completions already are: two refused rounds (~35 s apart on the
     * fast-confirm cadence) and the round after restarts the radio before it scans. */
    if (!connected && _dryScans < 1000) {
      _dryScans++;
    }
    log_e("[autosw] scan start kept failing for 5 s (wifi status %d, refused with 0x%lx %s) dry=%u",
          (int)WiFi.status(), (unsigned long)(uint32_t)startErr, wifiErrName(startErr),
          (unsigned)_dryScans);
    _scanPending = false;               // give up; normal backoff retries soon
    if (!_scanKeepCadence) {            // (a Wi-Fi location round: geoloc asks again later)
      scheduleScanRetry(now, WiFi.status() == WL_CONNECTED);
    }
  }
}

/* The disconnected scan period in force right now: two minutes for a brief blip, easing to
 * five once it is clearly not one. One definition, because the due-check and the retry
 * scheduler MUST agree — they did not, and that is what broke the backoff. */
/* The one WiFi switch, persisted across reboots.
 *
 * 🛑 THE ORIGINAL TOGGLE WAS DELIBERATELY NOT PERSISTED, and the reason given was sound:
 * "a radio that stays off across a power cycle is a setting you can forget you set, and the
 * failure mode is a phone that silently never connects again." {user} asked for persistence
 * anyway (2026-09-01) because the measured cost of leaving it on away from a network is real
 * — ~106 mV/h searching against ~60 mV/h associated. **So the objection is answered rather
 * than ignored**: the menu row now says "off" in the large font with "saves power - survives
 * restarts" underneath, and the boot log prints WIFI: radio is OFF at log_e. The state is
 * loud in the two places someone would look.
 *
 * Preferences rather than the configs INI: this is one bool, the INI is a CriticalFile with a
 * restore path, and the mesh settings already established this namespace pattern. */
void Networks::setRadioOff(bool off) {
  _radioOff = off;
  Preferences prefs;
  prefs.begin("wpwifi", false);
  prefs.putBool("radiooff", off);
  prefs.end();
  if (off) {
    disable();
  } else {
    resumeReconnect();
  }
  log_e("WIFI: radio switched %s by the user (persisted)", off ? "OFF" : "ON");
}

/* Read the switch at boot. Split from applying it so setup() can decide the order — the
 * radio must not be started first and stopped a moment later. */
bool Networks::loadRadioOff() {
  Preferences prefs;
  prefs.begin("wpwifi", true);          // read-only
  _radioOff = prefs.getBool("radiooff", false);
  prefs.end();
  return _radioOff;
}

bool Networks::worthAttemptingJoin() {
  /* Every early return here is a FAIL-OPEN. Read the note on the declaration before changing
   * any of them: this may only skip a join it has positive, recent evidence is pointless. */
  if (!inLongDrySpell()) {
    return true;                        // a brief blip: behave exactly as before, no latency
  }
  const uint32_t now = millis();
  if (_scanDoneMs == 0 ||
      (uint32_t)(now - _scanDoneMs) > WIFI_SCAN_EVIDENCE_MS) {
    return true;                        // no evidence, or stale — never block on ignorance
  }
  if (_savedSeenLastScan) {
    return true;                        // the LAST scan saw a saved network: go
  }
  if (++_joinsSkippedRun >= WIFI_JOIN_BLIND_EVERY) {
    _joinsSkippedRun = 0;
    log_e("[wifi] scans see no saved network, but trying blind anyway "
          "(insurance for a hidden SSID or a lying scan)");
    return true;
  }
  _joinsSkipped++;
  log_e("[wifi] join skipped: last scan saw no saved network (%u in a row, %lu total) - "
        "a scan costs ~350ms, a failed join up to 30s of radio",
        (unsigned)_joinsSkippedRun, (unsigned long)_joinsSkipped);
  return false;
}

uint32_t Networks::currentDiscPeriod() const {
  /* Third tier added 2026-09-01 at {user}'s ask. Two minutes covers a brief blip, five covers
   * a spell, and past ten minutes with no association at all the phone is somewhere without
   * WiFi — a car, the woods — where scanning three times as often learns the same thing
   * three times. See Networks::inLongDrySpell(). */
  if (inLongDrySpell()) {
    return WIFI_DRY_SCAN_PERIOD_MS;
  }
  return (_discScans >= 5) ? 300000u : AUTO_SCAN_DISC_PERIOD_MS;
}

/* Put the next scan AUTO_SCAN_RETRY_MS away, under whichever period the due-check will apply.
 *
 * 🛑 THIS EXISTS BECAUSE `_msLastScan = now - AUTO_SCAN_PERIOD_MS + AUTO_SCAN_RETRY_MS` IS
 * ONLY CORRECT WHILE CONNECTED. That expression means "600 s ago, minus 30", and the
 * due-check compares against AUTO_SCAN_PERIOD_MS *only when connected*. Disconnected it
 * compares against 120 s (or 300 s), and a stamp 570 s in the past is already older than
 * either — so the scan was due IMMEDIATELY and the next tick started another one.
 *
 * MEASURED on phone 1, 2026-08-26, with the access point gone: **114 scans in 280 seconds,
 * one every ~2.5 s**, where the design in autoSwitchTick() says one every two minutes. Each
 * scan lights the radio for a few hundred ms, so a phone carried out of range was burning
 * the radio essentially continuously — the exact cost the backoff was written to avoid, in
 * the exact situation it was written for. */
void Networks::scheduleScanRetry(uint32_t now, bool connected) {
  const uint32_t period = connected ? AUTO_SCAN_PERIOD_MS : currentDiscPeriod();
  _msLastScan = (period > AUTO_SCAN_RETRY_MS) ? (now - (period - AUTO_SCAN_RETRY_MS)) : now;
}

void Networks::autoSwitchEvaluate(int n) {
  ini.unload();
  if (!(ini.load() || ini.restore()) || ini.isEmpty()) {
    return;
  }

  int bestRssi = -127;
  String bestSsid;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    if (ssid.length() == 0 || rssi <= bestRssi) {
      continue;
    }
    int idx = ini.query("s", ssid.c_str());
    const char* dis = NULL;
    if (idx >= 0) {
      dis = ini[idx]["disabled"];
    }
    log_e("[autosw] seen '%s' %ddBm saved=%d dis=%s", ssid.c_str(), rssi,
          (int)(idx >= 0 && ini[idx].hasKey("p")), dis ? dis : "-");
    if (idx < 0 || !ini[idx].hasKey("p")) {
      continue;                         // not one of our saved networks
    }
    if (dis != NULL && !strcmp(dis, "true")) {
      continue;                         // user explicitly disconnected this one
    }
    bestRssi = rssi;
    bestSsid = ssid;
  }
  if (bestSsid.length() == 0) {
    log_e("[autosw] no saved network in range");
    return;                             // _savedSeenMs deliberately NOT stamped
  }
  _savedSeenMs = millis() | 1u;         // a saved network really is on the air right now
  _savedSeenLastScan = true;

  if (connected && wifiSsidDyn != NULL && bestSsid.equals(wifiSsidDyn)) {
    return;                             // already on the best network
  }
  if (connected && bestRssi < WiFi.RSSI() + AUTO_SWITCH_MARGIN_DB) {
    return;                             // not enough gain to justify the hop
  }

  log_e("[autosw] switching to '%s' (%d dBm)", bestSsid.c_str(), bestRssi);
  // Make it the preferred network so the existing 20s reconnect loop pulls
  // toward the same place instead of fighting the switch.
  int i = ini.query("s", bestSsid.c_str());
  if (i >= 0 && ini.setUniqueFlag(i, "m")) {
    ini.store();
  }
  freeNull((void **) &prefSsidDyn);
  prefSsidDyn = strdup(bestSsid.c_str());
  connectTo(bestSsid.c_str());
}

bool Networks::scan(void) {
  // used as a reference only
  WiFi.mode(WIFI_STA);
  disconnect();
  delay(100);
  // Uncalled today, but guarded anyway: an unguarded scan site is how the phone reboots,
  // and nothing should be revived into that shape by accident. See helpers.h.
  if (!wifiScanMemoryOk("Networks::scan")) {
    return false;
  }
  int n = WiFi.scanNetworks();
  wifiScanNoteResult(n);
  log_d("scan done");
  log_d("networks: %d", n);
  for (int i=0; i<n; i++) {
    log_d("%d: %s (%d) %s", i, WiFi.SSID(i), WiFi.RSSI(i), (WiFi.encryptionType(i) == WIFI_AUTH_OPEN)? "\t- OPEN":"\t- closed");
    delay(10);
  }
  return n >= 0;
}
