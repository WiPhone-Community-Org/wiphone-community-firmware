/* wifi_policy.h — may the WiFi STATION come back right now, and if so how? ONE answer for every
 * site that switched the radio off (or lent it to a hotspot) and now hands it back. Kept in a
 * header of its own so the HOST SUITE tests the decisions the firmware executes
 * (tests/test_wifi_policy.cpp; the cpu_clock_policy.h pattern). Networks.cpp's
 * wifiRestoreStation() gathers the inputs and does the radio work; this file only decides.
 *
 * 🛑 THE BUG CLASS THIS FILE EXISTS FOR: WiFi TURNED BACK ON BEHIND THE OWNER (0.9.79, 2026-09-25).
 * Nothing answered the shared question, so every site that gave the radio back asked its own
 * subset of it:
 *   ~GbcApp (the Game Boy's exit) ......... radioOff() only
 *   the loop's join retry ................. userDisabled() + "hotspot live" - no game, no radioOff
 *   the uploader/window teardown .......... radioOff() + userDisabled(), a separate game branch
 *   Settings > WiFi's rescan / its exit ... radioOff() / userDisabled() - one each
 *   xferStart() ........................... nothing at all (serial `up on` mid-game = a softAP)
 *   the two "WiFi on" controls ............ a bare esp_wifi_start(), which restarts whatever mode
 *                                            the DRIVER last had - AP, after a hotspot session
 *                                            that ended with WiFi off (WiFi.mode(OFF) never calls
 *                                            esp_wifi_set_mode: arduino-esp32 1.0.6
 *                                            WiFiGeneric.cpp:541 `cm && !m -> espWiFiStop()`).
 * The worst of it, measured in the source: a phone with NO saved network, or after Disconnect
 * or Forget, is userDisabled() but NOT radioOff() (Networks::disable() sets one, not the other),
 * so every Game Boy exit ran `WiFi.mode(WIFI_STA); WiFi.reconnect()` on the ERASED config - and
 * nothing ever quieted it, because the loop's retry, its quiesce and the auto-switcher all stand
 * down for a userDisabled phone. The radio stayed up until the next reboot.
 *
 * ⚠ AND "radioOff IMPLIES userDisabled" IS NOT A RULE ANYONE ENFORCES. loadPreferred() rewrites
 * _userDisabled from the per-network INI flag (Edit network > Save does that with WiFi off), so
 * both switches are always asked - never one standing in for the other.
 *
 * The inputs, in the order they win:
 *   gameActive     a Game Boy game owns the cores and the internal RAM it took with WiFi off.
 *                  The station is NEVER brought back under it; the game's own exit asks again.
 *   softApLive     the uploader's or a KOSync window's hotspot is on the air. A station join
 *                  under a live softAP is the chip panic WiPhone.ino's reconnect gate exists to
 *                  prevent, so the radio is left to its owner, whose teardown asks again.
 *   radioOff / userDisabled   the owner said off (the global switch / Disconnect, Forget, no
 *                  saved network). / !reconnect: Networks::disconnect() left the station off and
 *                  the retry disarmed - nothing is waiting for it, so it is not brought up.
 *   staConnected   already associated: leave it alone (WiFi.status(), never the `connected`
 *                  flag, which a radio stopped while associated leaves stale-TRUE).
 *   joinYoung      a join started < 10 s ago is still associating: bring the station up but do
 *                  not re-begin under it (the loop's own young-join rule, Networks.cpp). Only
 *                  while its station RUNS - the gatherer reports false once the station was
 *                  stopped (a hotspot's mode(AP), a game, disable()), since nothing is then in
 *                  flight and waiting for the loop's next retry would only delay the rejoin.
 *   longDrySpell   5+ min with no network: come up idle and let the loop's GATED retry join -
 *                  it arms the 30 s quiesce and consults worthAttemptingJoin(); a restore's own
 *                  begin() did neither, so a window close in the car (with auto=on, every book
 *                  close) started a NO_AP_FOUND churn - the core's capless auto-reconnect - that
 *                  nothing quieted until the next auto-switch round's pre-scan disconnect.
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header. Keep it that way.
 */
#ifndef WIFI_POLICY_H
#define WIFI_POLICY_H

#include <stdint.h>

enum WifiRestore : uint8_t {
  WIFI_RESTORE_JOIN    = 0,   // station up AND rejoin the network the driver remembers
  WIFI_RESTORE_UP_IDLE = 1,   // station up, no join: the loop's gated retry decides
  WIFI_RESTORE_OFF     = 2,   // radio off
  WIFI_RESTORE_LEAVE   = 3,   // touch nothing: someone else owns the radio, or it is already fine
};

struct WifiRestoreIn {
  bool radioOff;       // the global switch (Settings > WiFi: off), persisted
  bool userDisabled;   // Disconnect / Forget / no saved network / the INI's per-network flag
  bool reconnect;      // Networks::doReconnect(): false after disconnect() until something re-arms
  bool gameActive;     // gGbcActive
  bool softApLive;     // xferServing() && xferUsingAP(): the uploader's or a window's hotspot
  bool staConnected;   // WiFi.status() == WL_CONNECTED
  bool joinYoung;      // a join started less than 10 s ago AND its station is still running
  bool longDrySpell;   // Networks::inLongDrySpell()
};

/* May the STATION run at all right now? The loop's join retry asks exactly this (plus its own
 * cadence and scan gates). A NEW predicate, not an edit of radioOff()/userDisabled(): both of
 * those have other readers that mean something narrower (the menu label, the INI flag). */
static inline bool wifiStationWantedFrom(bool radioOff, bool userDisabled, bool gameActive,
                                         bool softApLive) {
  return !radioOff && !userDisabled && !gameActive && !softApLive;
}

#define WIFI_JOIN_YOUNG_MS 10000u   // a join younger than this is still associating/DHCPing
#define WIFI_JOIN_AHEAD_MS 60000u   // a stamp up to this far AHEAD of `now` was made in this pass

/* How old is the join stamped `lastJoinMs` (the attempt stamp), seen from `now`?
 * 🛑 `now` IS OFTEN THE LOOP'S, READ AT THE TOP OF THE PASS (WiPhone.ino loop()), and a join can be
 * stamped LATER IN THE SAME PASS: a sync window closing in kosyncLoop(), a Game Boy game's exit in
 * the key handling, both through wifiRestoreStation()'s JOIN and its noteWifiJoinStarted(), which
 * reads millis() then. That stamp is AHEAD of `now`, and `(uint32_t)(now - lastJoinMs)` reads it
 * as ~49 days old: the loop's retry, its wake branch and its quiesce all took a join begun
 * milliseconds earlier for a stale one - the retry re-began over it (0.9.79 review R2) and the
 * quiesce would have disconnected it. A stamp up to WIFI_JOIN_AHEAD_MS ahead is age 0. The price
 * is a 60 s window every 49.7 days in which an ancient stamp reads fresh (once, then heals). */
static inline uint32_t wifiJoinAgeMs(uint32_t now, uint32_t lastJoinMs) {
  if ((uint32_t)(lastJoinMs - now) <= WIFI_JOIN_AHEAD_MS) {
    return 0;                       // stamped at or after `now` was read: as young as it gets
  }
  return now - lastJoinMs;          // wrap-safe for any age up to ~49 days
}

/* Did a join start less than 10 s ago (`lastJoinMs`: the attempt stamp, 0 = never)? The loop's
 * join retry and its screen-wake retry ask this before they BEGIN one (wifiRetryMayBegin); the
 * restore's joinYoung asks it with the station's state (wifiJoinInFlight). */
static inline bool wifiJoinYoungAt(uint32_t now, uint32_t lastJoinMs) {
  return lastJoinMs != 0 && wifiJoinAgeMs(now, lastJoinMs) < WIFI_JOIN_YOUNG_MS;
}

/* The joinYoung input: is a join still ASSOCIATING? Started less than 10 s ago AND its station
 * still running. A join whose station has since been stopped - a hotspot's mode(AP), a game's
 * disconnect(true), disable() - is not in flight, and calling it young only made the restore come
 * up idle and wait for the loop's next retry (20 s, or 3 min after five failures) where a begin()
 * now is right (0.9.79 review). Wrap-safe. */
static inline bool wifiJoinInFlight(bool staRunning, uint32_t now, uint32_t lastJoinMs) {
  return staRunning && wifiJoinYoungAt(now, lastJoinMs);
}

/* May the loop's join retry (or its screen-wake retry) BEGIN a join now? Not within 10 s of ANY
 * other join - a restore's begin() (a sync window closing, a game ending), a Settings Connect, the
 * auto-switcher's hop. 🛑 0.9.79 review R2: the retry's cadence stamp (msLastWifiRetry) is written
 * only by the retry itself, and it goes stale while a hotspot or a game holds the radio (the retry
 * is gated off then). So the pass that closed a window - JOIN: mode(STA) + begin() - reached the
 * retry with `due` already true and called connectToPreferred(): a SPIFFS INI read (~1.6 s) on
 * the loop, then connectToWiFi()'s disconnect(false) + begin(ssid, pwd), which ABORTED the
 * association the restore had started milliseconds earlier. The wake branch already had this
 * rule; the retry now shares it, and the one spelling of it. `due` stays true, so the retry runs
 * the moment the other join is 10 s old and still has not associated (and arms the quiesce). */
static inline bool wifiRetryMayBegin(uint32_t now, uint32_t lastJoinMs) {
  return !wifiJoinYoungAt(now, lastJoinMs);
}

/* ── THE CORE'S AUTO-RECONNECT: ONE FLAG, ONE OWNER (0.9.79 review R1) ────────────────────────
 * arduino-esp32 1.0.6's event task answers a STA_DISCONNECTED with reason >= 200 (not 202) - an
 * out-of-range NO_AP_FOUND above all - with `WiFi.disconnect(); WiFi.begin();` whenever
 * WiFi.getAutoReconnect() (WiFiGeneric.cpp:404-410). begin() opens with enableSTA(true), which is
 * mode(current | STA) (WiFiSTA.cpp:197-223), so one such event handled just AFTER something took
 * the station away puts it back:
 *   - after a hotspot's WiFi.mode(WIFI_AP) (transportUp: a sync window or the uploader, brought up
 *     exactly while the station hunts): the phone becomes AP+STA, the station hunting beside the
 *     live softAP for the whole window - the AP follows the station's scan channels, and a station
 *     connect under a live softAP is what WiPhone.ino's reconnect gate calls a chip panic;
 *   - after Networks::disable() (WiFi off, Disconnect, Forget): mode(STA) from NULL restarts the
 *     radio behind "WiFi: off";
 *   - after a Game Boy game's disconnect(true): esp_wifi_start() under the emulator.
 * Each site used to save the flag and put its own copy back (the game did; the others did not
 * touch it). Saved copies do not nest when the scopes are not LIFO, and they are not: a game
 * starting under a sync window HOLDS first and then closes the window, so the window's "restore"
 * re-armed the flag in the middle of the game and the game's own restore then left it OFF for
 * the rest of the boot. So the flag is computed, never restored: ARMED (the owner's policy -
 * false after disable(), true again on a deliberate join or resumeReconnect()) AND NOBODY HOLDING
 * it (a bit per scope: the game, a hotspot). Networks.cpp applies it on every change. */
enum : uint8_t {
  WIFI_AR_HOLD_GAME    = 0x01,   // GbcApp::startGame() .. ~GbcApp()
  WIFI_AR_HOLD_HOTSPOT = 0x02,   // transportUp()'s softAP .. transportDown() (app_gbc_xfer.cpp)
};

struct WifiAutoReconnect {
  bool    armed;   // the owner's policy; the core's own default is true
  uint8_t holds;   // WIFI_AR_HOLD_* bits of the scopes live now
};

static inline void wifiArHold(WifiAutoReconnect& a, uint8_t who)    { a.holds = (uint8_t)(a.holds | who); }
static inline void wifiArRelease(WifiAutoReconnect& a, uint8_t who) { a.holds = (uint8_t)(a.holds & ~who); }
static inline void wifiArArm(WifiAutoReconnect& a, bool armed)      { a.armed = armed; }
/* What WiFi.setAutoReconnect() must be given. */
static inline bool wifiArOn(const WifiAutoReconnect& a) {
  return a.armed && a.holds == 0;
}

static inline WifiRestore wifiRestoreDecision(const WifiRestoreIn& s) {
  if (s.gameActive) {
    return WIFI_RESTORE_OFF;        // the game's exit asks again
  }
  if (s.softApLive) {
    return WIFI_RESTORE_LEAVE;      // the hotspot's teardown asks again
  }
  if (s.radioOff || s.userDisabled || !s.reconnect) {
    return WIFI_RESTORE_OFF;        // the owner said off (or nothing is waiting for the station)
  }
  if (s.staConnected) {
    return WIFI_RESTORE_LEAVE;
  }
  if (s.joinYoung || s.longDrySpell) {
    return WIFI_RESTORE_UP_IDLE;
  }
  return WIFI_RESTORE_JOIN;
}

static inline const char* wifiRestoreName(WifiRestore d) {
  switch (d) {
  case WIFI_RESTORE_JOIN:    return "JOIN";
  case WIFI_RESTORE_UP_IDLE: return "UP_IDLE";
  case WIFI_RESTORE_OFF:     return "OFF";
  case WIFI_RESTORE_LEAVE:   return "LEAVE";
  }
  return "?";
}

#endif // WIFI_POLICY_H
