/*
 * test_wifidiag.cpp — wifi_diag.cpp: the disconnect-reason record behind `WIFI LOST` / `wifi why`.
 *
 * What is pinned here, and why each matters on the phone:
 *   - The core predicates, copied from arduino-esp32 1.0.6 WiFiGeneric.cpp:392-410. They are how
 *     an OLD health.log is read (wifi=5 can only have been reason 200 or 204) and how the storm
 *     count `cr=` is computed; a wrong entry would mislead exactly the reading this exists for.
 *   - The IDF 3.3 reason and esp_err numbers (esp_wifi_types.h, esp_wifi.h, esp_err.h).
 *   - The ring: in-order, one slot of margin, an overrun reported as lost, never a torn copy
 *     reported as good.
 *   - The link bookkeeping: a drop carries the channel/RSSI of the link it ENDED, and only that.
 *   - The scan-start runs and the HEALTH field's exact text.
 */
#include "../WiPhone/wifi_diag.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

static void ok(bool cond, const char* what) {
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  }
}

static void testCorePredicates() {
  printf("\n\033[1mThe 1.0.6 core: status set, and whether it calls begin() again\033[0m\n");
  // WiFiGeneric.cpp:392-403
  ok(wifiCoreStatusAfter(201) == 1, "NO_AP_FOUND -> WL_NO_SSID_AVAIL (1)");
  ok(wifiCoreStatusAfter(202) == 4, "AUTH_FAIL -> WL_CONNECT_FAILED (4)");
  ok(wifiCoreStatusAfter(203) == 4, "ASSOC_FAIL -> WL_CONNECT_FAILED (4)");
  ok(wifiCoreStatusAfter(200) == 5, "BEACON_TIMEOUT -> WL_CONNECTION_LOST (5)");
  ok(wifiCoreStatusAfter(204) == 5, "HANDSHAKE_TIMEOUT -> WL_CONNECTION_LOST (5)");
  ok(wifiCoreStatusAfter(2) == -1, "AUTH_EXPIRE leaves the status alone");
  ok(wifiCoreStatusAfter(8) == 6, "ASSOC_LEAVE (our own disconnect) -> WL_DISCONNECTED (6)");
  ok(wifiCoreStatusAfter(205) == 6, "CONNECTION_FAIL -> WL_DISCONNECTED (6)");
  ok(wifiCoreStatusAfter(206) == 6, "AP_TSF_RESET -> WL_DISCONNECTED (6)");
  ok(wifiCoreStatusAfter(15) == 6, "4WAY_HANDSHAKE_TIMEOUT -> WL_DISCONNECTED (6)");
  // The inverse the HEALTH reader needs: exactly which reasons produce wifi=5.
  int fives = 0;
  for (unsigned r = 0; r < 256; r++) {
    if (wifiCoreStatusAfter(r) == 5) {
      fives++;
      ok(r == 200 || r == 204, "only 200/204 read as wifi=5");
    }
  }
  ok(fives == 2, "exactly two reasons read as wifi=5");
  // WiFiGeneric.cpp:404-410
  ok(wifiCoreRejoinsAfter(2), "AUTH_EXPIRE: core rejoins");
  ok(wifiCoreRejoinsAfter(200), "BEACON_TIMEOUT: core rejoins");
  ok(wifiCoreRejoinsAfter(201), "NO_AP_FOUND: core rejoins (the capless storm)");
  ok(!wifiCoreRejoinsAfter(202), "AUTH_FAIL: core does NOT rejoin");
  ok(wifiCoreRejoinsAfter(203), "ASSOC_FAIL: core rejoins");
  ok(wifiCoreRejoinsAfter(204), "HANDSHAKE_TIMEOUT: core rejoins");
  ok(wifiCoreRejoinsAfter(205), "CONNECTION_FAIL: core rejoins");
  ok(wifiCoreRejoinsAfter(206), "AP_TSF_RESET: core rejoins");
  ok(!wifiCoreRejoinsAfter(8), "ASSOC_LEAVE (a plain WiFi.disconnect()) quiets it");
  ok(!wifiCoreRejoinsAfter(1) && !wifiCoreRejoinsAfter(3) && !wifiCoreRejoinsAfter(15),
     "deauth-style reasons below 200 are left to the firmware's own retry");
}

static void testNames() {
  printf("\n\033[1mIDF 3.3 reason and error names\033[0m\n");
  ok(!strcmp(wifiReasonName(200), "BEACON_TIMEOUT"), "200");
  ok(!strcmp(wifiReasonName(201), "NO_AP_FOUND"), "201");
  ok(!strcmp(wifiReasonName(204), "HANDSHAKE_TIMEOUT"), "204");
  ok(!strcmp(wifiReasonName(8), "ASSOC_LEAVE"), "8");
  ok(!strcmp(wifiReasonName(15), "4WAY_HANDSHAKE_TIMEOUT"), "15");
  ok(!strcmp(wifiReasonName(24), "CIPHER_SUITE_REJECTED"), "24");
  ok(!strcmp(wifiReasonName(12), "?"), "12 is unassigned in IDF 3.3");
  ok(!strcmp(wifiReasonName(0), "?") && !strcmp(wifiReasonName(207), "?"), "out of range");
  ok(!strcmp(wifiErrName(0x3006), "ESP_ERR_WIFI_STATE"), "0x3006 = ESP_ERR_WIFI_BASE + 6");
  ok(!strcmp(wifiErrName(0x3002), "ESP_ERR_WIFI_NOT_STARTED"), "0x3002");
  ok(!strcmp(wifiErrName(0x107), "ESP_ERR_TIMEOUT"), "0x107");
  ok(!strcmp(wifiErrName(0), "ESP_OK"), "0");
  ok(!strcmp(wifiErrName(0x3010), "?"), "0x3010 is unassigned");
}

static void testBuckets() {
  printf("\n\033[1mThe per-reason table: every IDF 3.3 reason gets its own row\033[0m\n");
  bool used[WIFI_DIAG_BUCKETS] = {false};
  const unsigned reasons[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20,
                              21, 22, 23, 24, 200, 201, 202, 203, 204, 205, 206};
  bool distinct = true, roundTrip = true, inRange = true;
  for (unsigned r : reasons) {
    const int b = wifiReasonBucket(r);
    if (b <= 0 || b >= WIFI_DIAG_BUCKETS) {
      inRange = false;
      continue;
    }
    if (used[b]) {
      distinct = false;
    }
    used[b] = true;
    if (wifiBucketReason(b) != r) {
      roundTrip = false;
    }
  }
  ok(inRange, "every real reason lands in 1..31");
  ok(distinct, "no two reasons share a row");
  ok(roundTrip, "bucket -> reason is the inverse");
  ok(wifiReasonBucket(0) == 0 && wifiReasonBucket(99) == 0 && wifiReasonBucket(250) == 0,
     "anything else is row 0 (other)");
}

static const uint8_t AP_A[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const uint8_t AP_B[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
static const uint8_t ZERO[6] = {0, 0, 0, 0, 0, 0};

static void testTheNightOf2026_09_24() {
  printf("\n\033[1mThe 2026-09-24 shape: joined, beacon timeout, a NO_AP_FOUND storm, a bounce, rejoined\033[0m\n");
  static WifiDiagLog L;
  L.reset();
  L.onRadio(1000, true);
  L.onConnect(3000, AP_A, 6);
  L.noteRssi(-63, 360000);
  L.noteRssi(0, 362000);                // not associated: ignored, the -63 stands
  L.onDisconnect(363500, 200, AP_A);    // the drop that ENDS the link
  for (int i = 0; i < 5; i++) {
    L.onDisconnect(366000 + 2500u * i, 201, ZERO);
  }
  L.onRadio(400000, false);             // wifi bounce
  L.onRadio(400300, true);
  L.onScanDone(401000, 0, 7);
  L.onConnect(404000, AP_B, 11);

  ok(L.head() == 12, "12 events written");
  ok(L.disconnects() == 6, "6 disconnects");
  ok(L.coreRejoins() == 6, "all 6 were answered by the core's own begin()");
  ok(L.lastReason() == 201, "last reason 201");
  ok(L.count(wifiReasonBucket(200)) == 1 && L.count(wifiReasonBucket(201)) == 5, "per-reason counts");
  ok(L.connects() == 2 && L.scansDone() == 1 && L.scansFailed() == 0, "joins and scan events");

  WifiDiagEvent e;
  ok(L.read(2, &e) && e.kind == WDE_DISC && e.code == 200, "event 2 is the beacon timeout");
  ok(!memcmp(e.bssid, AP_A, 6) && e.chan == 6, "...on AP_A, channel 6 (from its CONNECTED)");
  ok(e.rssi == -63 && e.val == 3500, "...last RSSI -63, sampled 3.5 s before the drop");
  ok(L.read(3, &e) && e.kind == WDE_DISC && e.code == 201, "event 3 is a NO_AP_FOUND");
  ok(e.chan == 0 && e.rssi == 0 && e.val == -1, "...which ended no link: no channel, no RSSI");
  ok(L.read(11, &e) && e.kind == WDE_CONN && e.chan == 11 && !memcmp(e.bssid, AP_B, 6),
     "the rejoin: AP_B on channel 11");
  ok(L.linked() && L.linkChan() == 11, "linked again");
  ok(!L.read(12, &e), "an event not yet written reads as absent");
}

static void testRssiBelongsToTheLink() {
  printf("\n\033[1mA drop only borrows an RSSI sampled on the link it ended\033[0m\n");
  static WifiDiagLog L;
  L.reset();
  L.onConnect(1000, AP_A, 1);
  L.noteRssi(-50, 5000);
  L.onDisconnect(6000, 8, AP_A);
  L.onConnect(9000, AP_B, 6);             // no sample on this link yet
  L.onDisconnect(9500, 200, AP_B);
  WifiDiagEvent e;
  ok(L.read(1, &e) && e.rssi == -50 && e.chan == 1, "first link: its own sample and channel");
  ok(L.read(3, &e) && e.rssi == 0 && e.val == -1 && e.chan == 6,
     "second link: the OLD link's -50 is not reported as this one's");
  L.onDisconnect(9600, 201, AP_B);        // already unlinked
  ok(L.read(4, &e) && e.chan == 0, "a disconnect with no link up carries no channel");
  L.onConnect(10000, AP_A, 3);
  L.noteRssi(-70, 10500);
  L.onRadio(11000, false);                // radio off while associated
  ok(!L.linked(), "a stopped radio has no link");
}

static void testRing() {
  printf("\n\033[1mThe ring: 15 readable, overrun reported, never torn\033[0m\n");
  static WifiDiagLog L;
  L.reset();
  for (uint32_t i = 0; i < 40; i++) {
    L.onScanDone(1000 + i, 0, i);
  }
  WifiDiagEvent e;
  int good = 0;
  bool inOrder = true;
  for (uint32_t s = 0; s < 40; s++) {
    if (L.read(s, &e)) {
      good++;
      if ((uint32_t)e.val != s) {
        inOrder = false;
      }
    }
  }
  ok(good == (int)WifiDiagLog::RING - 1, "exactly RING-1 of the newest are readable (one slot of margin)");
  ok(inOrder, "each readable slot holds the event its sequence names");
  ok(!L.read(40 - WifiDiagLog::RING, &e), "the one about to be overwritten is refused");
  ok(L.read(39, &e) && e.val == 39, "the newest reads");
  ok(!L.read(40, &e), "the future does not");
  ok(L.scansDone() == 40, "the counters are not limited by the ring");
  for (int i = 0; i < 3; i++) {
    L.onScanDone(0, 1, 1234);
  }
  ok(L.read(L.head() - 1, &e) && e.code == 1 && e.val == 1234, "status/aps of a failed scan kept");
  ok(L.scansFailed() == 3, "failed/aborted scans counted apart");
}

static void testScanStarts() {
  printf("\n\033[1mScan starts: refusals grouped into runs; the HEALTH field\033[0m\n");
  WifiScanStartStats s;
  s.note(1000, 0);
  for (uint32_t t = 2000; t < 2100; t++) {
    s.note(t, 0x3006);                    // one 5 s pending round retries every pass
  }
  s.note(40000, 0x3006);                  // the next round, 38 s later: a new run
  s.note(40001, 0x3006);
  s.note(41000, 0);                       // a start that worked ends the run
  s.note(41001, 0x3002);
  ok(s.starts == 105, "attempts");
  ok(s.refused == 103, "refused attempts");
  ok(s.refusedRuns == 3, "three runs of refusals");
  ok(s.refusedState == 102, "of which 'still connecting'");
  ok(s.lastErr == 0x3002 && s.lastRefusedMs == 41001, "the last refusal and when");
  s.noteTimeout(50000);
  ok(s.timedOut == 1, "a blocking scan that never finished is its own count");

  static WifiDiagLog L;
  L.reset();
  L.onDisconnect(1, 200, AP_A);
  L.onDisconnect(2, 201, ZERO);
  char buf[96];
  wifiDiagHealthField(buf, sizeof(buf), L, s);
  ok(!strcmp(buf, " wdis=2/201 cr=2 ssf=3/3002"), buf);
  WifiScanStartStats none;
  static WifiDiagLog empty;
  empty.reset();
  wifiDiagHealthField(buf, sizeof(buf), empty, none);
  ok(!strcmp(buf, " wdis=0/0 cr=0 ssf=0/0"), buf);
  char tiny[8];
  const int n = wifiDiagHealthField(tiny, sizeof(tiny), L, s);
  ok(n > (int)sizeof(tiny) && strlen(tiny) == sizeof(tiny) - 1, "truncates safely into a short buffer");
}

static void testBssid() {
  printf("\n\033[1mBSSID text\033[0m\n");
  char b[18];
  wifiFormatBssid(AP_B, b);
  ok(!strcmp(b, "aa:bb:cc:dd:ee:ff"), b);
  wifiFormatBssid(ZERO, b);
  ok(!strcmp(b, "-"), "all-zero (a failed join names no AP) prints '-'");
}

/* The card gate (review F3): at most one LOST/JOIN pair a minute on /health.log, the rest
 * counted and reported, never a pair split, never a storm's end left unsaid. */
static void testCardGate() {
  printf("\n\033[1mThe card gate: one LOST/JOIN pair a minute, the rest counted\033[0m\n");
  {
    WifiCardGate g;
    ok(g.join(10000), "the boot's first JOIN goes to the card");
    ok(g.lost(30000), "the first LOST ever goes, 20 s after that JOIN (LOSTs gate on LOSTs)");
    ok(g.join(35000), "...and its JOIN: a pair is never split");
    ok(!g.lost(50000), "a LOST 20 s after the last card LOST is held");
    ok(!g.join(55000), "...and its JOIN, 20 s after the last card line");
    ok(g.heldLost == 1 && g.heldJoin == 1 && g.heldSinceMs == 50000, "both counted, from 50 s");
    ok(!g.summaryDue(94999), "no summary before a quiet minute since the last card line (35 s)");
    ok(g.summaryDue(95000), "...a summary at 95 s: the card must hear how the storm ended");
    uint32_t l = 0, j = 0, since = 0;
    ok(g.takeHeld(95000, &l, &j, &since) && l == 1 && j == 1 && since == 50000,
       "takeHeld gives 1 LOST, 1 JOIN since 50 s");
    ok(!g.summaryDue(200000) && !g.takeHeld(200000, &l, &j, &since),
       "and resets them: nothing held, nothing due");
  }
  {
    /* A steady flap: drop at :00, rejoin at :05, every 10 s, for an hour. Before the gate that
     * was 720 card lines; now the LOST+JOIN pairs are one a minute and the count line at most
     * one a minute on top. */
    WifiCardGate g;
    uint32_t card = 0, serialOnly = 0;
    for (uint32_t t = 0; t < 3600000u; t += 10000u) {
      uint32_t l, j, since;
      if (g.lost(t)) {
        card += g.takeHeld(t, &l, &j, &since) ? 2 : 1;
      } else {
        serialOnly++;
      }
      if (g.join(t + 5000)) {
        card += g.takeHeld(t + 5000, &l, &j, &since) ? 2 : 1;
      } else {
        serialOnly++;
      }
      if (g.summaryDue(t + 9999)) {
        g.takeHeld(t + 9999, &l, &j, &since);
        card++;
      }
    }
    char m[96];
    snprintf(m, sizeof(m), "an hour of 10 s flapping: %u card lines (was 720), %u serial-only",
             (unsigned)card, (unsigned)serialOnly);
    printf("  %s\n", m);
    /* 60 LOSTs (one a minute) + their 60 JOINs + 59 count lines (the first minute has nothing
     * held yet); the other 600 lines are serial-only, each counted exactly once. */
    ok(card == 179 && serialOnly == 600, m);
  }
  {
    WifiCardGate g;
    ok(g.lost(1000), "a LOST in the first minute of the boot (millis small) is not mistaken for 'recent'");
    ok(!g.lost(2000) && g.join(3000) == false, "a second drop inside the minute is held, and so is its JOIN");
    ok(g.lost(61000), "exactly a minute after the last card LOST, the next goes");
    ok(g.join(61500), "...with its JOIN");
    WifiCardGate w;
    w.lost(0xFFFFF000u);
    ok(!w.lost(0x00001000u), "millis() wrap: 8 s later is still inside the minute (unsigned difference)");
    ok(w.lost(0x0000F000u), "...and 64 s later is not");
  }
  {
    /* TWO CLOCKS (the misc repair round): diagTick drains events stamped by the WiFi event task
     * but asks summaryDue() with the loop pass's `now`, read BEFORE the drain. One slow pass
     * drains a LOST (card), its JOIN (card) and a second LOST (held), all stamped after `now`. */
    WifiCardGate g;
    ok(g.lost(1000) && g.join(1500) && !g.lost(1600), "LOST + JOIN to the card, the next LOST held");
    ok(!g.summaryDue(1200), "summaryDue at the pass's `now` (1.2 s, BEFORE those stamps): not due - "
       "a card line 300 ms in the future is age 0, not ~49 days");
    ok(!g.summaryDue(61499) && g.summaryDue(61500), "...due a quiet minute after the JOIN, as ever");
    uint32_t l = 0, j = 0, since = 0;
    ok(g.takeHeld(1200, &l, &j, &since) && l == 1 && j == 0 && since == 1600,
       "a takeHeld at the earlier `now` still hands over the count");
    ok(g.lastMs == 1500, "...and does not move the last card line BACK to 1.2 s");
    ok(!g.lost(2000) && !g.summaryDue(61499) && g.summaryDue(61500),
       "the next held LOST's summary still waits a minute from the JOIN at 1.5 s");
    ok(wifiCardAgeMs(1000, 1000) == 0 && wifiCardAgeMs(1000, 61000) == 0 &&
       wifiCardAgeMs(1000, 61001) == 0xFFFFFFFFu - 60000u,
       "wifiCardAgeMs: a stamp up to 60 s ahead is age 0; further ahead is the old wrap (a stamp "
       "~49.7 days old)");
    ok(wifiCardAgeMs(70000, 10000) == 60000 && wifiCardAgeMs(0x1000u, 0xFFFFF000u) == 0x2000u,
       "...and a past stamp is its plain age, across the millis() wrap");
  }
}

int main() {
  testCardGate();
  testCorePredicates();
  testNames();
  testBuckets();
  testTheNightOf2026_09_24();
  testRssiBelongsToTheLink();
  testRing();
  testScanStarts();
  testBssid();
  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
