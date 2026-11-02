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

#ifndef _WIFI_NETWORKS_
#define _WIFI_NETWORKS_

#include <WiFi.h>
#include "WiFiUdp.h"
#include "Storage.h"
#include "config.h"
#include "lwip/netdb.h"
#include "src/ping/ping.h"
#include <ESPmDNS.h>
#include "wifi_policy.h"   // WIFI_AR_HOLD_*, wifiRetryMayBegin: the pure half of the rules below

#ifdef WIPHONE_PRODUCTION
#define WIFI_DEBUG(fmt, ...)
#else
#define WIFI_DEBUG(fmt, ...)      DEBUG("[wifi] " fmt, ##__VA_ARGS__)
#endif // WIPHONE_PRODUCTION

//IP address to send UDP data to: either use the ip address of the server or a network broadcast address


/* The tree's ONE WiFi.begin(ssid, pwd). False = refused (a Game Boy game or a live hotspot owns the
 * radio - see wifiStationBlockedBy()); nothing was started. */
extern bool connectToWiFi(const char* ssid, const char* pwd);

/* ── MAY THE STATION COME BACK? (0.9.79, WiPhone/wifi_policy.h) ──────────────────────────
 * 🛑 EVERY SITE THAT TURNED THE RADIO OFF, OR LENT IT TO A HOTSPOT, GIVES IT BACK THROUGH
 * wifiRestoreStation() — never `WiFi.mode(WIFI_STA)`, `WiFi.begin()`, `WiFi.reconnect()` or a bare
 * esp_wifi_start() of its own (tests/check_wifi_restore.py fails the suite on those outside
 * Networks.cpp). Each site used to ask its own subset of the owner's state, and between them
 * they turned WiFi back on behind "off" after every Game Boy game on a phone with no saved
 * network. The decision is the pure wifiRestoreDecision(); this gathers its inputs, does the
 * radio work and says so at log_e: `WIFI restore (<who>): <JOIN|UP_IDLE|OFF|LEAVE> [...]`.
 * Returns false only when WiFi.mode() failed (the Settings toggle's "say what is true" cue).
 * LOOP TASK. */
bool        wifiRestoreStation(const char* who);
/* May the station run at all right now: the owner's two switches, no game, no live hotspot. The
 * loop's join retry asks this. */
bool        wifiStationWanted();
/* Who holds the radio away from the station, or NULL: "a Game Boy game is running" or "a hotspot
 * is live (uploader or sync window)". connectToWiFi() and the serial bench commands refuse on it. */
const char* wifiStationBlockedBy();

/* millis() of the last connectToWiFi() from ANY path — the periodic retry, the
 * auto-switcher, or a manual join in the networks app — and of the transfer server's own
 * WiFi.begin() when it hands the radio back (noteWifiJoinStarted). The reconnect backoff's
 * radio quiesce consults this so it never disconnects an association it did not start, and the
 * loop's join retry so it never re-begins over one (wifiRetryMayBegin, review R2). ⚠ Read it with
 * wifi_policy.h's wifiJoinAgeMs/wifiJoinYoungAt against the loop's `now`: a join stamped later
 * in the same pass is AHEAD of it, and a plain `now - stamp` calls that ~49 days old. */
uint32_t lastWifiConnectAttemptMs();
void     noteWifiJoinStarted();       // a join begun outside connectToWiFi() (a bare WiFi.begin())

/* ── THE CORE'S AUTO-RECONNECT, HELD OFF FOR A SCOPE (0.9.79 review R1; wifi_policy.h) ──────
 * 🛑 Never WiFi.setAutoReconnect() anywhere else. Hold BEFORE the line that takes the station away
 * (a hotspot's WiFi.mode(WIFI_AP), a game's disconnect(true)), release BEFORE the station is given
 * back (wifiRestoreStation()). `who` is one WIFI_AR_HOLD_* bit; holding twice or releasing what
 * was never held is harmless. The flag the core sees is "armed AND nobody holds it", whatever
 * order the scopes end in. LOOP TASK. */
void     wifiAutoReconnectHold(uint8_t who);
void     wifiAutoReconnectRelease(uint8_t who);
/* millis() of the last moment the station was known UP (its GOT_IP, or the drop that ended a
 * connected spell); 0 = never. A blip's own reconnect is the core's WiFi.begin(), which no
 * join path stamps — this is how a caller tells "just dropped, rejoining" from "out of range". */
uint32_t lastWifiLinkUpMs();
extern IPAddress resolveDomain(const char* hostName);

/* WiFi.scanNetworks(async) with the esp_err_t it throws away (see Networks.cpp): -2 from the core
 * means "refused" OR (blocking only) "started, no SCAN_DONE in 10 s", and *whyOut says which —
 * ESP_ERR_WIFI_STATE (0x3006) = the station is mid-connect; ESP_ERR_TIMEOUT (0x107) = the
 * blocking wait ran out. Ask wifiScanMemoryOk() first, exactly as for scanNetworks(). LOOP TASK. */
int16_t wifiScanStart(bool async, int32_t* whyOut = nullptr);
/* The loop's RSSI sample (WiPhone.ino, every WIFI_CHECK_PERIOD_MS), so a drop can say what the
 * link looked like just before it. 0 (not associated) is ignored. */
void    wifiDiagNoteRssi(int rssi, uint32_t ms);
/* " wdis=<disconnects>/<last reason> cr=<core rejoins> ssf=<refused-start runs>/<last err hex>"
 * for the HEALTH line. */
int     wifiDiagHealth(char* out, size_t cap);

// Class to save/load WiFi networks data from Flash
class Networks {
public:
  Networks(void);
  ~Networks(void);

  void init();
  void getMac(uint8_t* mac);

  // Interfaces
  bool loadNetworkSettings(const char* ssid);
  void loadPreferred();

  inline bool isConnected(void) {
    return connected;
  }
  inline bool isConnectionEvent(void) {
    return connectionEvent;
  }
  inline bool doReconnect(void) {
    return reconnect;
  }
  inline void setConnected(bool conn) {
    connected = conn;
  }
  inline void setConnected(bool conn, bool event) {
    connected = conn;
    connectionEvent = event;
  }

  // WiFi network staff
  bool connectToPreferred(void);
  void bounceRadio(void);        // radio off/on + state truth + prompt rescan — see .cpp
  void resumeReconnect(void);    // "manage WiFi again": clears user-disabled, arms rejoin
  bool hasPreferredSsid(void);
  bool connectTo(const char* ssid);
  void disconnect(void);
  void disable(void);
  bool scan(void);

  // WiFi auto-switch: periodically scan (async) for the strongest SAVED network
  // and hop to it. Runs from the main loop via autoSwitchTick(); a scan fires
  // immediately when the screen wakes with no connection. User-toggleable
  // (Settings -> WiFi auto-switch), persisted in networks.ini.
  bool autoSwitchEnabled(void);
  void setAutoSwitch(bool enabled);
  void autoSwitchTick(bool screenOn);
  bool scanBusy(void) {          // a scan is in flight or starting: hold off reconnect
    return _scanning || _scanPending;   // (connectToWiFi hard-cycles WiFi, killing scans)
  }
  /* Wi-Fi location (geoloc_net.cpp, 0.9.82) asks for ONE scan: the next autoSwitchTick starts
   * it through every rule the switcher's own scans obey - the heap gate (wifiScanMemoryOk: each
   * scan's allocation is the one that has rebooted phone 1), the 10 s join hold-off, and the
   * call / game / softAP skips (the tick is not run then) - and waits while a handshake is on the
   * HTTPS worker (geolocScanMayStart). Only while CONNECTED; it never starts the disconnected
   * branch (WiFi.disconnect(), the deaf-radio bounce), and it is a scan and nothing else - never
   * evaluated for a hop, never the INI reload - unless the switcher's own cadence was due anyway
   * (geoloc.h, geoSwitcherMode; autoSwitchTick's geoOnly). Spent when the round starts; geoloc
   * spaces its asks GEO_SCAN_REQ_GAP_MS apart. */
  void requestGeoScan(void) { _geoScanReq = true; }
  bool geoScanRequested(void) const { return _geoScanReq; }

  /* Why the station dropped (2026-09-25): diagTick() drains the event task's record into
   * `WIFI LOST` / `WIFI disc` / `WIFI JOIN` lines (LOST and JOIN also to health.log) — call it
   * from the loop every pass; diagPrint() is the serial `wifi why` report. Both LOOP TASK. */
  void diagTick(uint32_t now);
  void diagPrint(void (*out)(const char*));

  /* Has this phone been off WiFi for long enough that it is plainly not a brief blip?
   *
   * 🔑 {user} ASKED FOR THIS BY NUMBER (2026-09-01): "if it doesn't see a Wi-Fi signal in 10
   * minutes, [make] the retry interval a lot less [often]". He had just driven 50 minutes home
   * with no WiFi, and that stretch measured ~106 mV/h against ~60 mV/h associated.
   *
   * ⚠ THE SCANS WERE NEVER THE EXPENSIVE PART — a scan is a few hundred ms. The cost is the
   * JOIN RETRY: WIFI_RETRY_PERIOD_MS is 20 s and eases only to 180 s, and every failed attempt
   * leaves the radio associating for up to 30 s before the quiesce in loop() disconnects it.
   * Out of range that is roughly 20 attempts an hour x 30 s ≈ an 8 % duty cycle at full radio
   * power, on top of the scans. Both cadences read this one predicate so they cannot disagree
   * — the file already learned that lesson once (see scheduleScanRetry).
   *
   * Both escape hatches survive: connecting clears the spell, and a screen wake still forces an
   * immediate scan and an immediate retry, so picking the phone up is prompt however long the
   * spell has run. */
  /* The user's own WiFi switch: true = they turned the radio off to save power.
   *
   * 🔑 EVERY re-enable path must consult this. The 2026-09-01 audit found three that did not
   * — the WiFi list screen's 5 s rescan, the uploader's softAP fallback, and the Game Boy's
   * exit — so "WiFi off" quietly stopped being true partway through a trip. */
  bool     radioOff() const {
    return _radioOff;
  }
  void     setRadioOff(bool off);      // switches, persists, and applies it
  bool     loadRadioOff();             // read the stored switch at boot (does NOT apply it)

  /* Is a join attempt worth the radio time, given what the last scan actually saw?
   *
   * 🔑 THE POINT: a scan is ~350 ms. A failed join holds the radio associating for up to 30 s
   * before the quiesce in loop() disconnects it. Out of range that is the single largest
   * remaining cost, so do not spend it on air we have just been told is empty.
   *
   * 🛑 IT FAILS OPEN, EVERYWHERE, ON PURPOSE. No scan evidence, stale evidence, or not yet in
   * a long dry spell all return true — this may only ever skip a join it has POSITIVE recent
   * evidence is pointless. A phone that will not rejoin is a far worse failure than a phone
   * that wastes some milliamps, and hidden SSIDs never appear in a scan at all.
   *
   * ⚠ AND IT HAS A SAFETY VALVE: every JOIN_BLIND_EVERY skips it lets one attempt through
   * regardless. So a hidden network, or a deaf radio whose scans lie, costs a slower rejoin
   * and never a permanent one. NOT const — the valve counts. */
  bool     worthAttemptingJoin();
  uint32_t joinsTried() const {
    return _joinsTried;
  }
  uint32_t joinsSkipped() const {
    return _joinsSkipped;
  }

  bool     inLongDrySpell() const {
    return _drySpellStartMs != 0 &&
           (uint32_t)(millis() - _drySpellStartMs) >= WIFI_DRY_SPELL_LONG_MS;
  }

  // Properties
  const char* ssid() {
    return wifiSsidDyn;
  }
  const char* pass() {
    return wifiPassDyn;
  }
  const char* prefSsid() {
    return prefSsidDyn;
  }

  bool userDisabled() {
    return _userDisabled;
  }
  /* The OWNER allows the station: neither the global switch nor the network's own Disconnect
   * says off. A NEW predicate (0.9.79) rather than an edit of either one, because each of those
   * has readers that mean exactly it — and because "radioOff implies userDisabled" is not a
   * rule anyone enforces: loadPreferred() rewrites _userDisabled from the per-network INI flag
   * (Edit network > Save does that with WiFi off), and connectTo() clears it while the switch
   * stays off. Every "may I bring WiFi back" reader asks BOTH, through this. */
  bool stationAllowed() const {
    return !_radioOff && !_userDisabled;
  }

  static constexpr const char* filename = "/networks.ini";
  bool mdnsOk;                // MDNS service is operational

protected:
  void autoSwitchEvaluate(int n);   // pick the strongest saved network from a scan

  char* prefSsidDyn;
  char* wifiSsidDyn;    // current (or last) WiFi network
  char* wifiPassDyn;

  // Auto-switch state
  int8_t   _autoSwitch = -1;        // -1 = not loaded from ini yet
  bool     _scanning = false;       // an async scan is in flight
  bool     _scanPending = false;    // scan requested; keep retrying start briefly
  uint32_t _msScanPendingSince = 0;
  bool     _scanPreMarked = false;  // this pending round's heapEvent("scan-pre") is taken
  bool     _geoScanReq = false;     // Wi-Fi location asked for a scan (requestGeoScan)
  bool     _scanFull = true;        // the scan in flight may be evaluated for a hop (GEO_SW_FULL)
  bool     _scanKeepCadence = false;  // ...a Wi-Fi location round under FULL: _msLastScan untouched
  bool     _prevScreenOn = true;    // for the wake-up edge
  uint32_t _msLastScan = 0;
  uint32_t currentDiscPeriod() const;               // 2 min, easing to 5 — see the .cpp
  void     scheduleScanRetry(uint32_t now, bool connected);
  /* "We were not looking, so there is nothing to have found." Clears the dry-spell clock, the
   * deaf-radio accounting and the air evidence gathered before the radio went down, and makes
   * a scan due immediately. Called from the radio-ON paths only — read the note on the
   * definition for the 22-hour case that made it necessary. */
  void     forgetDrySpell(void);
  /* Consecutive scans run while disconnected. Used to stretch the scan interval when
   * there is clearly nothing in range — see autoSwitchTick(). Reset on any connect. */
  /* The one global WiFi switch, persisted. Distinct from _userDisabled on purpose:
   * _userDisabled is also raised by a per-NETWORK `disabled` flag in loadPreferred(), so it
   * cannot answer "did the user switch the radio off". This one can, which is what lets the
   * menu label, the boot path and the three re-enable paths all agree. */
  bool     _radioOff = false;

  /* Scan evidence for worthAttemptingJoin(). _scanDoneMs is the last COMPLETED scan of any
   * result; _savedSeenMs the last one that actually saw a saved network. Both are needed:
   * "no evidence" and "evidence that there is nothing" must not be confused, because the
   * first has to fail OPEN. */
  uint32_t _scanDoneMs = 0;
  uint32_t _savedSeenMs = 0;
  /* Did the MOST RECENT completed scan see a saved network? A boolean about the latest
   * evidence, which is the question worthAttemptingJoin() actually wants answered. Measured
   * 2026-09-02 on a 50-minute commute: comparing _savedSeenMs against a 20-minute window
   * instead meant a network seen just before setting off counted as "recent" for 20 minutes
   * after it was gone, so the gate engaged 25 minutes into the drive rather than 5 — half
   * the trip still burning blind joins. Scans run every 5 min in a dry spell, so the lookback
   * was buying nothing and costing twenty minutes. */
  bool     _savedSeenLastScan = false;
  uint8_t  _joinsSkippedRun = 0;       // consecutive skips, for the safety valve
  uint32_t _joinsTried = 0;            // instruments, surfaced in the health line
  uint32_t _joinsSkipped = 0;

  uint32_t _discScans = 0;
  /* When the current disconnected spell began, or 0 while connected. Drives the LONG-SPELL
   * easing — see inLongDrySpell(). Time, not a scan count, because the ask was in minutes and
   * a count only maps to minutes through whatever cadence happens to be in force.
   *
   * 🛑 IT MEASURES HOW LONG WE HAVE BEEN *LOOKING* WITHOUT SUCCESS — not how long since we
   * last had a network, which is a different question with a different answer whenever the
   * radio is off. Time with the radio switched off is not looking and must not accrue here;
   * forgetDrySpell() clears it on every radio-ON path for exactly that reason (2026-09-07). */
  uint32_t _drySpellStartMs = 0;
  /* Consecutive scans that COMPLETED with zero results while disconnected. Distinct from
   * _discScans (which counts rounds for the backoff): this one is the deaf-radio detector.
   * MEASURED 2026-08-27 (phone 1, work desk): after hours of disconnected retry churn the
   * driver enters a state where every scan completes EMPTY — async and blocking, 80 and
   * 240 MHz, quiesced or not — while the twin phone hears the same AP at -50 dBm, and the
   * ONLY cure is a radio off/on (which is why "open the WiFi screen and rescan" always
   * fixed it: NetworksApp's constructor bounces the radio by accident). At 2 the
   * auto-switcher bounces the radio itself before scanning — see autoSwitchTick().
   * MECHANISM (2026-08-27, the booksync wedge): the "deaf" radio is usually the radio held
   * perpetually MID-CONNECT — esp_wifi documents that scans run while the station is
   * connecting are ineffective, and the framework's event-task auto-reconnect retries
   * capless after every reason>=200, so once a drop starts churning, every scan completes
   * 0 or -2 until something cycles the radio. Failed completions (-2) therefore count as
   * dry evidence too, not just empty ones. */
  uint32_t _dryScans = 0;
  /* True once this dry spell has had its radio bounce. While false and _dryScans >= 1 the
   * disconnected scan interval drops to AUTO_SCAN_RETRY_MS so the deaf state is confirmed
   * and cured in ~90 s instead of 10-15 min (2026-08-27: the wedge outlived the eased
   * cadence all afternoon and the user's reboot always beat the bounce to it). After the
   * bounce the eased cadence returns: if one bounce did not cure it, the phone is genuinely
   * out of range and fast scanning would just drain the battery. Cleared whenever a scan
   * hears anything or a connection is made. */
  bool _dryBounced = false;

  bool _userDisabled;
  bool reconnect;             // should it try to reconnect when disconnected? TODO: save this in configs somehow
  bool connected;
  bool connectionEvent;       // connection/disconnection event processed

  MDNSResponder mdnsResponder;

  CriticalFile ini;
};

extern Networks wifiState;

#endif // _WIFI_NETWORKS_
