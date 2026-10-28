/* test_wifi_policy.cpp — may the WiFi station come back, and how (WiPhone/wifi_policy.h).
 *
 * WHY THIS IS WORTH A SUITE: the fault it guards is SILENT and it costs battery, not function.
 * A radio brought back behind the owner's "off" still looks off everywhere a person would look
 * (the menu row reads the switch, not the radio), and a station hunting on an erased config
 * prints nothing at the default log level. The only place every combination of the owner's
 * switches and the radio's temporary owners can be checked is here. The functions under test
 * are the REAL ones Networks.cpp's wifiRestoreStation() and the loop's retry gate call.
 *
 * The sweep covers all 2^8 input states against the rules; the named rows are the situations
 * the 2026-09-25 investigation walked through, one per gap it found.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "../WiPhone/wifi_policy.h"

static int failures = 0;
static int checks = 0;

static void group(const char* name) {
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  } else {
    printf("  ok  %s\n", what);
  }
}

static WifiRestoreIn fromMask(unsigned m) {
  WifiRestoreIn s;
  s.radioOff     = (m & 0x01) != 0;
  s.userDisabled = (m & 0x02) != 0;
  s.reconnect    = (m & 0x04) != 0;
  s.gameActive   = (m & 0x08) != 0;
  s.softApLive   = (m & 0x10) != 0;
  s.staConnected = (m & 0x20) != 0;
  s.joinYoung    = (m & 0x40) != 0;
  s.longDrySpell = (m & 0x80) != 0;
  return s;
}

/* One rule, checked in every one of the 256 states; one printed line per rule, and the mask of
 * any state that breaks it. */
typedef bool (*Rule)(const WifiRestoreIn& s, WifiRestore d);

static void sweep(const char* what, Rule rule) {
  int bad = 0;
  for (unsigned m = 0; m < 256; m++) {
    const WifiRestoreIn s = fromMask(m);
    checks++;
    if (!rule(s, wifiRestoreDecision(s))) {
      failures++;
      if (bad++ < 4) {
        printf("  \033[31mFAIL\033[0m %s  (state 0x%02x -> %s)\n", what, m,
               wifiRestoreName(wifiRestoreDecision(s)));
      }
    }
  }
  if (!bad) {
    printf("  ok  %s  (all 256 states)\n", what);
  }
}

static bool up(WifiRestore d) {
  return d == WIFI_RESTORE_JOIN || d == WIFI_RESTORE_UP_IDLE;
}

static WifiRestoreIn base() {     // WiFi on, a saved network, nobody else holding the radio
  WifiRestoreIn s;
  memset(&s, 0, sizeof(s));
  s.reconnect = true;
  return s;
}

int main() {
  group("the sweep: every state of the owner's switches and the radio's temporary owners");
  sweep("the station never comes up when the owner said off (radioOff)",
        [](const WifiRestoreIn& s, WifiRestore d) { return !(s.radioOff && up(d)); });
  sweep("...nor when the network is disabled (userDisabled: Disconnect, Forget, none saved)",
        [](const WifiRestoreIn& s, WifiRestore d) { return !(s.userDisabled && up(d)); });
  sweep("...and radioOff is asked in its own right: userDisabled clear does not let it up",
        [](const WifiRestoreIn& s, WifiRestore d) {
          return !(s.radioOff && !s.userDisabled && up(d));
        });
  sweep("under a Game Boy game the answer is OFF, whatever else is true",
        [](const WifiRestoreIn& s, WifiRestore d) { return !s.gameActive || d == WIFI_RESTORE_OFF; });
  sweep("a live hotspot (no game) is LEFT to its owner - never OFF, never a station join",
        [](const WifiRestoreIn& s, WifiRestore d) {
          return s.gameActive || !s.softApLive || d == WIFI_RESTORE_LEAVE;
        });
  sweep("JOIN only when every input is clear (reconnect armed, nothing else set)",
        [](const WifiRestoreIn& s, WifiRestore d) {
          const bool clear = !s.radioOff && !s.userDisabled && s.reconnect && !s.gameActive &&
                             !s.softApLive && !s.staConnected && !s.joinYoung && !s.longDrySpell;
          return (d == WIFI_RESTORE_JOIN) == clear;
        });
  sweep("disconnect() disarmed the retry (!reconnect): the station is not brought up",
        [](const WifiRestoreIn& s, WifiRestore d) { return s.reconnect || !up(d); });
  sweep("an association already up is never re-begun (LEAVE, or OFF if the owner said off)",
        [](const WifiRestoreIn& s, WifiRestore d) {
          return !s.staConnected || d == WIFI_RESTORE_LEAVE || d == WIFI_RESTORE_OFF;
        });
  sweep("a join younger than 10 s is never re-begun under itself",
        [](const WifiRestoreIn& s, WifiRestore d) { return !s.joinYoung || d != WIFI_RESTORE_JOIN; });
  sweep("a long dry spell leaves the join to the loop's gated retry (never a restore JOIN)",
        [](const WifiRestoreIn& s, WifiRestore d) { return !s.longDrySpell || d != WIFI_RESTORE_JOIN; });
  sweep("the restore and the loop's retry agree: up => the retry gate's predicate is true",
        [](const WifiRestoreIn& s, WifiRestore d) {
          return !up(d) || wifiStationWantedFrom(s.radioOff, s.userDisabled, s.gameActive,
                                                 s.softApLive);
        });
  sweep("...and not wanted => OFF or LEAVE, nothing that starts the station",
        [](const WifiRestoreIn& s, WifiRestore d) {
          return wifiStationWantedFrom(s.radioOff, s.userDisabled, s.gameActive, s.softApLive) ||
                 d == WIFI_RESTORE_OFF || d == WIFI_RESTORE_LEAVE;
        });
  sweep("every answer has a name for the log line",
        [](const WifiRestoreIn& s, WifiRestore d) { return strcmp(wifiRestoreName(d), "?") != 0; });

  group("wifiStationWantedFrom: the loop's retry gate");
  {
    int bad = 0;
    for (unsigned m = 0; m < 16; m++) {
      const bool ro = (m & 1) != 0, ud = (m & 2) != 0, game = (m & 4) != 0, ap = (m & 8) != 0;
      checks++;
      if (wifiStationWantedFrom(ro, ud, game, ap) != (m == 0)) {
        failures++;
        bad++;
        printf("  \033[31mFAIL\033[0m wanted(ro=%d ud=%d game=%d ap=%d)\n", ro, ud, game, ap);
      }
    }
    if (!bad) {
      printf("  ok  wanted only with all four clear (16 states)\n");
    }
  }
  ok(!wifiStationWantedFrom(false, false, true, false),
     "G2: a game running = no join retry (it used to WiFi.begin() under the emulator)");
  ok(!wifiStationWantedFrom(true, false, false, false),
     "G5: WiFi off with the per-network flag cleared by Edit > Save = no join retry");

  group("the situations the investigation walked through");
  {
    WifiRestoreIn s = base();
    s.userDisabled = true;       // Networks::disable(): no saved network at boot / Disconnect / Forget
    s.reconnect = false;         // ...which runs disconnect() (reconnect=false, config erased)
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF,
       "G1: a Disconnected phone's Game Boy exit -> OFF (was STA + reconnect() on an erased config)");
  }
  {
    WifiRestoreIn s = base();
    s.radioOff = true;
    s.userDisabled = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF, "the woods: WiFi off, a sync window closes -> OFF");
  }
  {
    WifiRestoreIn s = base();
    s.radioOff = true;           // the switch still says off...
    s.userDisabled = false;      // ...but loadPreferred() cleared this from the INI (G5)
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF, "G5: switch off, per-network flag cleared -> OFF");
  }
  {
    WifiRestoreIn s = base();
    s.longDrySpell = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_UP_IDLE,
       "G9: the car, 5+ min with no network, a window closes -> UP_IDLE (the loop's gated retry joins)");
  }
  {
    WifiRestoreIn s = base();
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_JOIN, "home, after a game -> JOIN");
  }
  {
    WifiRestoreIn s = base();
    s.gameActive = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF,
       "a window or uploader closes UNDER a game -> OFF (the game's exit restores)");
    s.softApLive = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF, "...even with a hotspot flag still set");
  }
  {
    WifiRestoreIn s = base();
    s.softApLive = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_LEAVE,
       "Settings: WiFi on while a headless uploader hosts its hotspot -> LEAVE (its stop restores)");
  }
  {
    WifiRestoreIn s = base();
    s.staConnected = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_LEAVE,
       "left Settings > WiFi having joined a network -> LEAVE");
  }
  {
    WifiRestoreIn s = base();
    s.joinYoung = true;
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_UP_IDLE,
       "a second restore within 10 s of the first's begin() -> UP_IDLE, no second begin()");
  }
  {
    WifiRestoreIn s = base();
    s.reconnect = false;         // standalone Edit network > Save: disconnect(), nothing re-arms
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_OFF,
       "after a standalone Edit > Save (retry disarmed) -> OFF, not a join on an erased config");
  }

  group("wifiJoinInFlight: the joinYoung input (review, 2026-09-25)");
  {
    const uint32_t t = 100000u;
    ok(wifiJoinInFlight(true, t, t - 2000u), "a join 2 s old on a running station -> in flight");
    ok(!wifiJoinInFlight(false, t, t - 2000u),
       "the same join after a hotspot's mode(AP) / a game stopped the station -> NOT in flight");
    ok(!wifiJoinInFlight(true, t, t - 10000u), "10 s old -> no longer young");
    ok(!wifiJoinInFlight(true, t, 0), "never joined (stamp 0) -> not in flight");
    ok(wifiJoinInFlight(true, 3000u, 0xFFFFF000u), "young across the millis() wrap");
    WifiRestoreIn s = base();
    s.joinYoung = wifiJoinInFlight(false, t, t - 2000u);
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_JOIN,
       "a hotspot up and down within 10 s of the loop's join -> JOIN at its close, not a 20 s wait");
    s.joinYoung = wifiJoinInFlight(true, t, t - 2000u);
    ok(wifiRestoreDecision(s) == WIFI_RESTORE_UP_IDLE,
       "left Settings > WiFi 2 s after its Connect (station still up) -> UP_IDLE, no second begin()");
    ok(wifiJoinInFlight(true, t, t + 300u),
       "a join stamped AFTER the caller read `now` (same loop pass) -> in flight");
  }

  group("wifiJoinAgeMs / wifiRetryMayBegin: the loop's retry never re-begins over a fresh join (R2)");
  {
    /* The pass: `now` read at the top of loop(); kosyncLoop() closes a window 250 ms later and the
     * restore's JOIN stamps millis() then; the wifi-retry phase runs later in the SAME pass with
     * the stale `now` and a stale msLastWifiRetry (due = true). */
    const uint32_t now = 500000u;
    const uint32_t stamp = now + 250u;
    ok(wifiJoinAgeMs(now, stamp) == 0, "a stamp 250 ms AHEAD of `now` is age 0");
    ok((uint32_t)(now - stamp) >= 10000u,
       "(the old spelling `now - stamp >= 10000` reads that same stamp as ~49 days old)");
    ok(!wifiRetryMayBegin(now, stamp),
       "the window-close pass: the restore's JOIN was stamped after `now` -> the retry HOLDS");
    ok(!wifiRetryMayBegin(now, now), "a join stamped at `now` exactly -> holds");
    ok(!wifiRetryMayBegin(now + 9999u, stamp - 250u), "9.999 s old -> holds");
    ok(wifiRetryMayBegin(now + 10000u, now), "10 s old and still not associated -> the retry runs");
    ok(wifiRetryMayBegin(now, 0), "never joined (stamp 0) -> the retry runs");
    ok(wifiRetryMayBegin(5000u, 0), "never joined, 5 s after boot -> the retry runs");
    ok(wifiJoinAgeMs(now, now - 20000u) == 20000u, "an ordinary 20 s age reads 20 s");
    ok(!wifiRetryMayBegin(3000u, 0xFFFFF000u), "7.1 s old across the millis() wrap -> holds");
    ok(wifiRetryMayBegin(20000u, 0xFFFFF000u), "24.1 s old across the millis() wrap -> runs");
    /* An int32 "negative means ahead" rule would block the retry for 24.8 days after a stamp more
     * than 24.8 days old: a phone associated for a month, then out of range, never retrying. */
    const uint32_t day = 86400000u;
    ok(wifiRetryMayBegin(now + 30u * day, now), "a join 30 days old -> the retry runs");
    ok(wifiRetryMayBegin(now + 40u * day, now), "40 days old (int32 would read it negative) -> runs");
    ok(wifiJoinAgeMs(now + 40u * day, now) == 40u * day, "...and its age reads 40 days");
    ok(wifiJoinAgeMs(now, now + 60000u) == 0 && wifiJoinAgeMs(now, now + 60001u) != 0,
       "'ahead' means at most 60 s ahead (WIFI_JOIN_AHEAD_MS), no more");
    /* The quiesce (WiPhone.ino) pushes its deadline out while the last attempt is < 30 s old. */
    ok(wifiJoinAgeMs(now, stamp) < 30000u,
       "the quiesce sees the same-pass join as young and pushes out, not a disconnect");
  }

  group("the core's auto-reconnect: armed AND nobody holding it (R1)");
  {
    WifiAutoReconnect a = { true, 0 };
    ok(wifiArOn(a), "at boot (the core's default) -> on");
    // A game starting under a sync window: startGame holds, THEN closes the window.
    wifiArHold(a, WIFI_AR_HOLD_HOTSPOT);
    ok(!wifiArOn(a), "a hotspot up (transportUp holds before mode(AP)) -> off");
    wifiArHold(a, WIFI_AR_HOLD_GAME);
    wifiArRelease(a, WIFI_AR_HOLD_HOTSPOT);
    ok(!wifiArOn(a), "the window closed INSIDE the game (non-LIFO) -> still off for the game");
    wifiArRelease(a, WIFI_AR_HOLD_GAME);
    ok(wifiArOn(a), "the game ended -> on again (not the window's cleared copy for ever)");

    /* The same sequence with the saved copies the game used to keep, to show why it is gone. */
    bool core = true, windowSaved, gameSaved;
    windowSaved = core; core = false;          // transportUp
    gameSaved = core;   core = false;          // startGame
    core = windowSaved;                        // the window closes inside the game
    ok(core, "(saved copies: the window's restore RE-ARMED the flag in the middle of the game)");
    core = gameSaved;                          // ~GbcApp
    ok(!core, "(saved copies: ...and the game's restore left it OFF for the rest of the boot)");

    WifiAutoReconnect b = { true, 0 };
    wifiArHold(b, WIFI_AR_HOLD_HOTSPOT);
    wifiArArm(b, false);                       // WiFi switched off while a window is up
    wifiArRelease(b, WIFI_AR_HOLD_HOTSPOT);
    ok(!wifiArOn(b), "disable() during a window: the window's close does not re-arm it");
    wifiArArm(b, true);                        // resumeReconnect / connectToWiFi
    ok(wifiArOn(b), "WiFi on again -> on");

    WifiAutoReconnect c = { false, 0 };        // after disable()
    wifiArHold(c, WIFI_AR_HOLD_GAME);
    wifiArArm(c, true);
    ok(!wifiArOn(c), "re-armed while a game holds it -> still off until the game ends");
    wifiArRelease(c, WIFI_AR_HOLD_GAME);
    ok(wifiArOn(c), "...then on");

    WifiAutoReconnect d = { true, 0 };
    wifiArRelease(d, WIFI_AR_HOLD_HOTSPOT);
    ok(wifiArOn(d) && d.holds == 0, "releasing a hold never taken changes nothing");
    wifiArHold(d, WIFI_AR_HOLD_GAME);
    wifiArHold(d, WIFI_AR_HOLD_GAME);
    wifiArRelease(d, WIFI_AR_HOLD_GAME);
    ok(wifiArOn(d), "holding twice (a second start in one app lifetime) needs one release");
    wifiArHold(d, WIFI_AR_HOLD_GAME);
    wifiArRelease(d, WIFI_AR_HOLD_HOTSPOT);
    ok(!wifiArOn(d), "one scope's release never ends another's hold");

    int bad = 0;
    for (int armed = 0; armed < 2; armed++) {
      for (unsigned h = 0; h < 4; h++) {
        WifiAutoReconnect e = { armed != 0, (uint8_t)h };
        checks++;
        if (wifiArOn(e) != (armed && h == 0)) {
          failures++;
          bad++;
        }
      }
    }
    ok(bad == 0, "on <=> armed and no hold, in all 8 states");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
