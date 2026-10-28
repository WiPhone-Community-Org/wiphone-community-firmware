/*
 * wifi_diag.cpp — see wifi_diag.h. Pure: the host suite compiles this file as it ships.
 */
#include "wifi_diag.h"

#include <atomic>
#include <stdio.h>
#include <string.h>

/* IDF 3.3 esp_wifi_types.h wifi_err_reason_t, verbatim numbers. 12 is unassigned there. */
const char* wifiReasonName(unsigned reason) {
  switch (reason) {
  case 1:   return "UNSPECIFIED";
  case 2:   return "AUTH_EXPIRE";
  case 3:   return "AUTH_LEAVE";
  case 4:   return "ASSOC_EXPIRE";
  case 5:   return "ASSOC_TOOMANY";
  case 6:   return "NOT_AUTHED";
  case 7:   return "NOT_ASSOCED";
  case 8:   return "ASSOC_LEAVE";
  case 9:   return "ASSOC_NOT_AUTHED";
  case 10:  return "DISASSOC_PWRCAP_BAD";
  case 11:  return "DISASSOC_SUPCHAN_BAD";
  case 13:  return "IE_INVALID";
  case 14:  return "MIC_FAILURE";
  case 15:  return "4WAY_HANDSHAKE_TIMEOUT";
  case 16:  return "GROUP_KEY_UPDATE_TIMEOUT";
  case 17:  return "IE_IN_4WAY_DIFFERS";
  case 18:  return "GROUP_CIPHER_INVALID";
  case 19:  return "PAIRWISE_CIPHER_INVALID";
  case 20:  return "AKMP_INVALID";
  case 21:  return "UNSUPP_RSN_IE_VERSION";
  case 22:  return "INVALID_RSN_IE_CAP";
  case 23:  return "802_1X_AUTH_FAILED";
  case 24:  return "CIPHER_SUITE_REJECTED";
  case 200: return "BEACON_TIMEOUT";
  case 201: return "NO_AP_FOUND";
  case 202: return "AUTH_FAIL";
  case 203: return "ASSOC_FAIL";
  case 204: return "HANDSHAKE_TIMEOUT";
  case 205: return "CONNECTION_FAIL";
  case 206: return "AP_TSF_RESET";
  default:  return "?";
  }
}

/* arduino-esp32 1.0.6 WiFiGeneric.cpp:392-403. wl_status_t numbers: 1 NO_SSID_AVAIL,
 * 4 CONNECT_FAILED, 5 CONNECTION_LOST, 6 DISCONNECTED. */
int wifiCoreStatusAfter(unsigned reason) {
  if (reason == 201) {
    return 1;
  }
  if (reason == 202 || reason == 203) {
    return 4;
  }
  if (reason == 200 || reason == 204) {
    return 5;
  }
  if (reason == 2) {
    return -1;                          // AUTH_EXPIRE: the core leaves the status as it was
  }
  return 6;
}

/* arduino-esp32 1.0.6 WiFiGeneric.cpp:404-410 (with getAutoReconnect() true). */
bool wifiCoreRejoinsAfter(unsigned reason) {
  return reason == 2 || (reason >= 200 && reason != 202);
}

/* esp_err.h + esp_wifi.h (IDF 3.3). ESP_ERR_TIMEOUT is used by wifiScanStart() for a blocking
 * scan that started and never delivered SCAN_DONE — the core returns -2 for that too. */
const char* wifiErrName(int32_t err) {
  switch (err) {
  case 0:      return "ESP_OK";
  case -1:     return "ESP_FAIL";
  case 0x101:  return "ESP_ERR_NO_MEM";
  case 0x102:  return "ESP_ERR_INVALID_ARG";
  case 0x103:  return "ESP_ERR_INVALID_STATE";
  case 0x107:  return "ESP_ERR_TIMEOUT";
  case 0x3001: return "ESP_ERR_WIFI_NOT_INIT";
  case 0x3002: return "ESP_ERR_WIFI_NOT_STARTED";
  case 0x3003: return "ESP_ERR_WIFI_NOT_STOPPED";
  case 0x3004: return "ESP_ERR_WIFI_IF";
  case 0x3005: return "ESP_ERR_WIFI_MODE";
  case 0x3006: return "ESP_ERR_WIFI_STATE";          // "wifi still connecting" for a scan start
  case 0x3007: return "ESP_ERR_WIFI_CONN";
  case 0x3008: return "ESP_ERR_WIFI_NVS";
  case 0x3009: return "ESP_ERR_WIFI_MAC";
  case 0x300A: return "ESP_ERR_WIFI_SSID";
  case 0x300B: return "ESP_ERR_WIFI_PASSWORD";
  case 0x300C: return "ESP_ERR_WIFI_TIMEOUT";
  case 0x300D: return "ESP_ERR_WIFI_WAKE_FAIL";
  case 0x300E: return "ESP_ERR_WIFI_WOULD_BLOCK";
  case 0x300F: return "ESP_ERR_WIFI_NOT_CONNECT";
  case 0x3012: return "ESP_ERR_WIFI_POST";
  case 0x3013: return "ESP_ERR_WIFI_INIT_STATE";
  case 0x3014: return "ESP_ERR_WIFI_STOP_STATE";
  default:     return "?";
  }
}

int wifiReasonBucket(unsigned reason) {
  if (reason >= 1 && reason <= 24) {
    return (int)reason;
  }
  if (reason >= 200 && reason <= 206) {
    return 25 + (int)(reason - 200);
  }
  return 0;
}

unsigned wifiBucketReason(int bucket) {
  if (bucket >= 1 && bucket <= 24) {
    return (unsigned)bucket;
  }
  if (bucket >= 25 && bucket <= 31) {
    return 200u + (unsigned)(bucket - 25);
  }
  return 0;
}

void wifiFormatBssid(const uint8_t b[6], char out[18]) {
  if (!b || !(b[0] | b[1] | b[2] | b[3] | b[4] | b[5])) {
    out[0] = '-';
    out[1] = '\0';
    return;
  }
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
}

// ───────────────────────────────────────────────────────────────────── the event ring

void WifiDiagLog::reset() {
  memset(_ring, 0, sizeof(_ring));
  _head = 0;
  memset(_count, 0, sizeof(_count));
  _disconnects = 0;
  _coreRejoins = 0;
  _connects = 0;
  _scansDone = 0;
  _scansFailed = 0;
  _lastReason = 0;
  _linked = false;
  memset(_connBssid, 0, sizeof(_connBssid));
  _connChan = 0;
  _connMs = 0;
  _rssi = 0;
  _rssiMs = 0;
}

/* Slot first, THEN the head. The fence stops the compiler sinking the slot stores below the head
 * store; on the ESP32 the volatile head store also gets a memw (GCC's -mserialize-volatile, on by
 * default for Xtensa), so a reader on either core that sees the new head sees the whole slot. */
void WifiDiagLog::push(const WifiDiagEvent& e) {
  const uint32_t h = _head;
  _ring[h % RING] = e;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  _head = h + 1;
}

bool WifiDiagLog::read(uint32_t seq, WifiDiagEvent* out) const {
  const uint32_t before = _head;
  if ((uint32_t)(before - seq) == 0 || (uint32_t)(before - seq) >= RING) {
    return false;                       // not written yet, or (about to be) overwritten
  }
  std::atomic_signal_fence(std::memory_order_seq_cst);
  *out = _ring[seq % RING];
  std::atomic_signal_fence(std::memory_order_seq_cst);
  /* Event seq+RING overwrites this slot, and is written while head == seq+RING. So the copy is
   * only known-good if head is still below that AFTER the copy. Conservative by one slot. */
  return (uint32_t)(_head - seq) < RING;
}

void WifiDiagLog::onDisconnect(uint32_t ms, uint8_t reason, const uint8_t* bssid) {
  WifiDiagEvent e;
  memset(&e, 0, sizeof(e));
  e.ms = ms;
  e.kind = WDE_DISC;
  e.code = reason;
  e.val = -1;
  if (bssid) {
    memcpy(e.bssid, bssid, 6);
  }
  if (_linked) {
    /* The drop that ENDS a link: say which link. The channel is the one CONNECTED reported for
     * this BSSID; the RSSI is the loop's last sample, and only if it was taken on THIS link. */
    if (bssid && memcmp(bssid, _connBssid, 6) == 0) {
      e.chan = _connChan;
    }
    const uint32_t rMs = _rssiMs;
    const int8_t   r = _rssi;
    if (rMs != 0 && r != 0 && (int32_t)(rMs - _connMs) >= 0) {
      e.rssi = r;
      e.val = (int32_t)(ms - rMs);
    }
  }
  _linked = false;
  const int b = wifiReasonBucket(reason);
  if (_count[b] < 0xFFFF) {
    _count[b]++;
  }
  _disconnects = _disconnects + 1;
  if (wifiCoreRejoinsAfter(reason)) {
    _coreRejoins = _coreRejoins + 1;
  }
  _lastReason = reason;
  push(e);
}

void WifiDiagLog::onConnect(uint32_t ms, const uint8_t* bssid, uint8_t chan) {
  WifiDiagEvent e;
  memset(&e, 0, sizeof(e));
  e.ms = ms;
  e.kind = WDE_CONN;
  e.chan = chan;
  if (bssid) {
    memcpy(e.bssid, bssid, 6);
    memcpy(_connBssid, bssid, 6);
  } else {
    memset(_connBssid, 0, sizeof(_connBssid));
  }
  _connChan = chan;
  _connMs = ms;
  _linked = true;
  _connects = _connects + 1;
  push(e);
}

void WifiDiagLog::onScanDone(uint32_t ms, uint8_t status, uint32_t aps) {
  WifiDiagEvent e;
  memset(&e, 0, sizeof(e));
  e.ms = ms;
  e.kind = WDE_SCANDONE;
  e.code = status;
  e.val = (int32_t)aps;
  _scansDone = _scansDone + 1;
  if (status != 0) {
    _scansFailed = _scansFailed + 1;
  }
  push(e);
}

void WifiDiagLog::onRadio(uint32_t ms, bool started) {
  WifiDiagEvent e;
  memset(&e, 0, sizeof(e));
  e.ms = ms;
  e.kind = started ? WDE_START : WDE_STOP;
  if (!started) {
    _linked = false;                    // a stopped radio has no link, whatever it reported
  }
  push(e);
}

void WifiDiagLog::noteRssi(int8_t rssi, uint32_t ms) {
  if (rssi == 0) {
    return;                             // WiFi.RSSI() answers 0 when not associated
  }
  _rssi = rssi;
  _rssiMs = ms ? ms : 1;
}

void WifiDiagLog::linkBssid(uint8_t out[6]) const {
  memcpy(out, _connBssid, 6);
}

// ───────────────────────────────────────────────────────────────────── scan starts

void WifiScanStartStats::note(uint32_t ms, int32_t err) {
  starts++;
  if (err == 0) {
    inRun = false;
    return;
  }
  refused++;
  if (!inRun || (uint32_t)(ms - lastRefusedMs) > 1000u) {
    refusedRuns++;
  }
  inRun = true;
  if (err == 0x3006) {
    refusedState++;
  }
  lastErr = err;
  lastRefusedMs = ms ? ms : 1;
}

void WifiScanStartStats::noteTimeout(uint32_t ms) {
  (void)ms;
  timedOut++;
}

int wifiDiagHealthField(char* out, size_t cap, const WifiDiagLog& log, const WifiScanStartStats& ss) {
  return snprintf(out, cap, " wdis=%lu/%u cr=%lu ssf=%lu/%lx",
                  (unsigned long)log.disconnects(), (unsigned)log.lastReason(),
                  (unsigned long)log.coreRejoins(), (unsigned long)ss.refusedRuns,
                  (unsigned long)(uint32_t)ss.lastErr);
}

// ---------------------------------------------------------------- the card gate (review F3)

uint32_t wifiCardAgeMs(uint32_t ms, uint32_t stamp) {
  if ((uint32_t)(stamp - ms) <= WIFI_CARD_AHEAD_MS) {
    return 0;                   // stamped at or after `ms` was read (this pass): as young as it gets
  }
  return ms - stamp;
}

bool WifiCardGate::lost(uint32_t ms) {
  if (!anyLost || wifiCardAgeMs(ms, lastLostMs) >= WIFI_CARD_PERIOD_MS) {
    anyLost = any = true;
    lastLostMs = lastMs = ms;
    pairOpen = true;
    return true;
  }
  if (heldLost == 0 && heldJoin == 0) {
    heldSinceMs = ms;
  }
  heldLost++;
  pairOpen = false;           // this spell's JOIN is not the partner of the card's last LOST
  return false;
}

bool WifiCardGate::join(uint32_t ms) {
  if (pairOpen) {
    pairOpen = false;
    lastMs = ms;
    return true;              // never split a pair
  }
  if (!any || wifiCardAgeMs(ms, lastMs) >= WIFI_CARD_PERIOD_MS) {
    any = true;
    lastMs = ms;
    return true;
  }
  if (heldLost == 0 && heldJoin == 0) {
    heldSinceMs = ms;
  }
  heldJoin++;
  return false;
}

bool WifiCardGate::summaryDue(uint32_t ms) const {
  /* `ms` is often the loop pass's `now`, and lastMs an event stamped after it (wifi_diag.h). */
  return (heldLost || heldJoin) && wifiCardAgeMs(ms, lastMs) >= WIFI_CARD_PERIOD_MS;
}

bool WifiCardGate::takeHeld(uint32_t ms, uint32_t* nLost, uint32_t* nJoin, uint32_t* sinceMs) {
  const bool had = heldLost || heldJoin;
  if (nLost) {
    *nLost = heldLost;
  }
  if (nJoin) {
    *nJoin = heldJoin;
  }
  if (sinceMs) {
    *sinceMs = heldSinceMs;
  }
  heldLost = heldJoin = 0;
  if (had) {
    if (!any || wifiCardAgeMs(ms, lastMs) != 0) {
      lastMs = ms;              // never BACK past a line stamped later than `ms` (wifi_diag.h)
    }
    any = true;
  }
  return had;
}
