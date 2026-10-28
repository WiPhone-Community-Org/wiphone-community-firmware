/*
 * geoloc.cpp — see geoloc.h. Proven by tests/test_geoloc.cpp on the host (the X4 Pro e-reader's
 * vectors for the same protocol, minus anything to do with an internet address, plus this
 * phone's scheduling and freshness rules).
 */

#include "geoloc.h"
#include "json_read.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- the setting

int geoSettingSane(int v) {
  return (v == GEO_SET_MAP || v == GEO_SET_MESH) ? v : GEO_SET_OFF;
}

const char* geoSettingName(int v) {
  switch (geoSettingSane(v)) {
    case GEO_SET_MAP:  return "map only";
    case GEO_SET_MESH: return "map + mesh";
    default:           return "off";
  }
}

int geoSettingParse(const char* s) {
  if (!s) {
    return -1;
  }
  if (!strcmp(s, "off")) return GEO_SET_OFF;
  if (!strcmp(s, "map")) return GEO_SET_MAP;
  if (!strcmp(s, "mesh")) return GEO_SET_MESH;
  return -1;
}

bool geoSettingSerialOk(int v) {
  return v == GEO_SET_OFF || v == GEO_SET_MAP;     // "mesh" is chosen on the screen (geoloc.h)
}

int geoSettingNext(int v) {
  const int c = geoSettingSane(v);
  return c == GEO_SET_MESH ? GEO_SET_OFF : c + 1;
}

// ---------------------------------------------------------------- the scan snapshot

static char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool endsNoCase(const char* s, size_t n, const char* suffix) {
  const size_t m = strlen(suffix);
  if (m > n) {
    return false;
  }
  for (size_t i = 0; i < m; i++) {
    if (lowerAscii(s[n - m + i]) != suffix[i]) {
      return false;
    }
  }
  return true;
}

uint8_t geoSsidFlags(const uint8_t* ssid, size_t cap) {
  if (!ssid || cap == 0) {
    return GEO_AP_HIDDEN;
  }
  const uint8_t* z = (const uint8_t*)memchr(ssid, 0, cap);
  if (!z) {
    return GEO_AP_ODD;                        // 32 bytes + NUL is the most an SSID is
  }
  const size_t n = (size_t)(z - ssid);
  if (n == 0) {
    return GEO_AP_HIDDEN;                     // also a hidden AP's zero-filled SSID
  }
  /* The opt-out conventions (Google's "_nomap", Mozilla's "_optout"): the owner of the network
   * asked not to be put in location databases, and asking one about it would be the same ask. */
  const char* s = (const char*)ssid;
  return (endsNoCase(s, n, "_nomap") || endsNoCase(s, n, "_optout")) ? GEO_AP_NOMAP : 0;
}

void geoScanBegin(GeoScan* s, uint32_t nowMs, uint32_t utc, uint32_t assoc) {
  if (!s) {
    return;
  }
  memset(s, 0, sizeof(*s));
  s->ms = nowMs ? nowMs : 1;                  // 0 means "no snapshot"
  s->utc = utc;
  s->assoc = assoc;
}

void geoScanAdd(GeoScan* s, const uint8_t mac[6], int rssi, uint8_t flags) {
  if (!s || !mac) {
    return;
  }
  if (rssi > 127) rssi = 127;
  if (rssi < -128) rssi = -128;
  if (s->seen < 0xFFFF) {
    s->seen++;
  }
  int at = -1;
  if (s->n < GEO_SCAN_MAX) {
    at = s->n++;
  } else {
    /* Full: a stronger one replaces the weakest held. A crowded scan (a city street can return
     * 60+) keeps what is near, which is all a lookup uses anyway (GEO_MIN_RSSI). */
    int weakest = 0;
    for (int i = 1; i < GEO_SCAN_MAX; i++) {
      if (s->ap[i].rssi < s->ap[weakest].rssi) {
        weakest = i;
      }
    }
    if (rssi <= s->ap[weakest].rssi) {
      return;
    }
    at = weakest;
  }
  memcpy(s->ap[at].mac, mac, 6);
  s->ap[at].rssi = (int8_t)rssi;
  s->ap[at].flags = flags;
}

bool geoScanFresh(const GeoScan* s, uint32_t assoc, uint32_t nowMs) {
  return s && s->ms != 0 && s->assoc == assoc && (uint32_t)(nowMs - s->ms) < GEO_SCAN_FRESH_MS;
}

// ---------------------------------------------------------------- the filters

static bool macUsable(const uint8_t* m) {
  if (m[0] & 0x03) {
    return false;                             // locally administered, or a group address
  }
  static const uint8_t ZERO[6] = { 0, 0, 0, 0, 0, 0 };
  if (!memcmp(m, ZERO, 6)) {
    return false;
  }
  // VRRP's virtual routers (RFC 5798): 00:00:5e:00:01:xx (IPv4), 00:00:5e:00:02:xx (IPv6).
  if (m[0] == 0x00 && m[1] == 0x00 && m[2] == 0x5e && m[3] == 0x00 && (m[4] == 0x01 || m[4] == 0x02)) {
    return false;
  }
  return true;
}

bool geoApUsable(const GeoAp* ap) {
  if (!ap || (ap->flags & (GEO_AP_HIDDEN | GEO_AP_NOMAP | GEO_AP_ODD))) {
    return false;
  }
  return macUsable(ap->mac);
}

bool geoSameDevice(const uint8_t a[6], const uint8_t b[6]) {
  if (!a || !b || a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) {
    return false;
  }
  const uint32_t x = ((uint32_t)a[3] << 16) | ((uint32_t)a[4] << 8) | a[5];
  const uint32_t y = ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5];
  return (x > y ? x - y : y - x) < GEO_SAME_DEVICE_SPAN;
}

bool geoJoinedLeftOut(const uint8_t bssid[6], bool gatewayServedNmea) {
  return bssid && (gatewayServedNmea || (bssid[0] & 0x02));
}

// ---------------------------------------------------------------- the two halves

/* Group n addresses (at mac0, mac0 + stride, ...) into devices: dev[k] = the first (strongest,
 * when they are in rank order) member of k's device, joined through shared neighbours too (a
 * chain of radios each within the span of the next is one router). Returns how many devices. */
static int groupDevices(const uint8_t* mac0, size_t stride, size_t n, uint8_t dev[]) {
  for (size_t k = 0; k < n; k++) {
    dev[k] = (uint8_t)k;
  }
  for (size_t k = 1; k < n; k++) {
    for (size_t j = 0; j < k; j++) {
      if (!geoSameDevice(mac0 + j * stride, mac0 + k * stride)) {
        continue;
      }
      const uint8_t from = dev[k] > dev[j] ? dev[k] : dev[j];
      const uint8_t to = dev[k] > dev[j] ? dev[j] : dev[k];
      for (size_t m = 0; m <= k; m++) {
        if (dev[m] == from) {
          dev[m] = to;
        }
      }
    }
  }
  int devices = 0;
  for (size_t k = 0; k < n; k++) {
    if (dev[k] == k) {
      devices++;
    }
  }
  return devices;
}

void geoSplitAps(const GeoAp* aps, size_t n, const uint8_t* exclude, GeoSplit* out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (!aps) {
    return;
  }
  /* Rank: the strongest eligible access point not already taken, then the next. A BSSID already
   * taken (at an equal or stronger reading) is skipped, and so is every address of the device
   * left out. Ties keep scan order. */
  size_t ranked[GEO_RANKED_MAX];
  size_t picked = 0;
  while (picked < GEO_RANKED_MAX) {
    size_t best = n;
    for (size_t i = 0; i < n; i++) {
      if (!geoApUsable(&aps[i]) || aps[i].rssi < GEO_MIN_RSSI) {
        continue;
      }
      if (exclude && geoSameDevice(aps[i].mac, exclude)) {
        continue;
      }
      bool taken = false;
      for (size_t k = 0; k < picked && !taken; k++) {
        taken = !memcmp(aps[ranked[k]].mac, aps[i].mac, 6);
      }
      if (taken) {
        continue;
      }
      if (best == n || aps[i].rssi > aps[best].rssi) {
        best = i;
      }
    }
    if (best == n) {
      break;
    }
    ranked[picked++] = best;
  }
  out->usable = (uint8_t)picked;
  out->n = (uint8_t)picked;
  for (size_t k = 0; k < picked; k++) {
    memcpy(out->q[k].mac, aps[ranked[k]].mac, 6);
    out->q[k].rssi = aps[ranked[k]].rssi;
  }
  uint8_t dev[GEO_RANKED_MAX];
  out->devices = (uint8_t)groupDevices(out->q[0].mac, sizeof(GeoQAp), picked, dev);

  /* Each device's half: its order among the devices (by strongest member), alternately; every
   * address follows its device. Each body then takes its half's strongest GEO_MAX_REQUEST_APS. */
  bool inB[GEO_RANKED_MAX];
  memset(inB, 0, sizeof(inB));
  int order = 0;
  for (size_t k = 0; k < picked; k++) {
    if (dev[k] != k) {
      continue;                               // not a device's strongest member
    }
    for (size_t m = k; m < picked; m++) {
      if (dev[m] == k) {
        inB[m] = (order % 2) == 1;
      }
    }
    order++;
  }
  uint8_t count[2] = { 0, 0 };
  for (size_t k = 0; k < picked; k++) {
    const int h = inB[k] ? GEO_HALF_B : GEO_HALF_A;
    if (count[h] < GEO_MAX_REQUEST_APS) {
      out->q[k].half = (uint8_t)h;
      count[h]++;
    } else {
      out->q[k].half = GEO_HALF_UNSENT;
    }
  }
  out->halfA = count[GEO_HALF_A];
  out->halfB = count[GEO_HALF_B];
  out->built = out->devices >= GEO_MIN_DEVICES && out->halfA > 0 && out->halfB > 0;
}

void geoSplitScan(const GeoScan* scan, const uint8_t* exclude, GeoSplit* out) {
  if (!out) {
    return;
  }
  if (!scan) {
    memset(out, 0, sizeof(*out));
    return;
  }
  geoSplitAps(scan->ap, scan->n, exclude, out);
  out->scanMs = scan->ms;
  out->scanUtc = scan->utc;
  out->scanAssoc = scan->assoc;
}

/* "considerIp":false AND "fallbacks":{"ipf":false}: beaconDB's IP fallback, off twice over. Either
 * alone turns it off (geolocate.rs); both, so one server-side default changing cannot bring an
 * internet-address answer back. "lacf" (cell) is ignored by beaconDB and harmless. */
static const char ENVELOPE[] =
    "{\"considerIp\":false,\"fallbacks\":{\"ipf\":false,\"lacf\":false},\"wifiAccessPoints\":[";

size_t geoBuildHalf(const GeoSplit* sp, int half, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = 0;
  if (!sp || !sp->built || (half != GEO_HALF_A && half != GEO_HALF_B)) {
    return 0;
  }
  int w = snprintf(out, cap, "%s", ENVELOPE);
  if (w < 0 || (size_t)w >= cap) {
    out[0] = 0;
    return 0;
  }
  size_t n = (size_t)w;
  size_t written = 0;
  for (size_t k = 0; k < sp->n; k++) {
    const GeoQAp* q = &sp->q[k];
    if (q->half != half) {
      continue;
    }
    const int rssi = q->rssi > 0 ? 0 : q->rssi;
    w = snprintf(out + n, cap - n,
                 "%s{\"macAddress\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"signalStrength\":%d}",
                 written ? "," : "", (unsigned)q->mac[0], (unsigned)q->mac[1], (unsigned)q->mac[2],
                 (unsigned)q->mac[3], (unsigned)q->mac[4], (unsigned)q->mac[5], rssi);
    if (w < 0 || n + (size_t)w >= cap) {
      out[0] = 0;
      return 0;
    }
    n += (size_t)w;
    written++;
  }
  w = snprintf(out + n, cap - n, "]}");
  if (w < 0 || n + (size_t)w >= cap || written == 0) {
    out[0] = 0;
    return 0;
  }
  return written;
}

// ---------------------------------------------------------------- the answer

static bool validCoords(double lat, double lon) {
  if (!(lat >= -90.0 && lat <= 90.0) || !(lon >= -180.0 && lon <= 180.0)) {
    return false;                             // NaN fails these too
  }
  return !(lat == 0.0 && lon == 0.0);         // what a lookup that found nothing tends to say
}

/* At the value of "location": {"lat": n, "lng": n} — each once, numbers only, other keys skipped. */
static bool readLocation(Js* j, double* lat, double* lon) {
  if (!jsOpen(j, '{')) {
    return false;
  }
  bool haveLat = false, haveLon = false;
  char key[16];
  for (;;) {
    const int r = jsObjNext(j, key, sizeof(key));
    if (r < 0) {
      return false;
    }
    if (r == 0) {
      break;
    }
    double v = 0;
    if (!strcmp(key, "lat") || !strcmp(key, "lng")) {
      bool* have = (key[1] == 'a') ? &haveLat : &haveLon;
      if (*have || jsNumber(j, &v) != 1) {
        return false;                         // twice, null, a string, garbage
      }
      *have = true;
      *(key[1] == 'a' ? lat : lon) = v;
    } else if (!jsSkip(j)) {
      return false;                           // a key with no value, a body cut short
    }
  }
  return haveLat && haveLon;
}

bool geoParseAnswer(const char* body, size_t len, double* lat, double* lon, uint32_t* accM) {
  if (!body || len == 0) {
    return false;
  }
  Js j = { body, body + len };
  if (!jsOpen(&j, '{')) {
    return false;                             // text before the object, or not an object
  }
  bool haveLoc = false, haveAcc = false;
  double la = 0, lo = 0, acc = 0;
  /* 16 bytes: a key longer than 15 is cut by json_read to 15 characters, which cannot equal any
   * name read here — a long key is never mistaken for a short one. */
  char key[16];
  for (;;) {
    const int r = jsObjNext(&j, key, sizeof(key));
    if (r < 0) {
      return false;
    }
    if (r == 0) {
      break;
    }
    if (!strcmp(key, "fallback") || !strcmp(key, "error")) {
      return false;                           // an IP/cell estimate, or no estimate at all
    }
    if (!strcmp(key, "location")) {
      if (haveLoc || !readLocation(&j, &la, &lo)) {
        return false;
      }
      haveLoc = true;
    } else if (!strcmp(key, "accuracy")) {
      if (haveAcc || jsNumber(&j, &acc) != 1) {
        return false;
      }
      haveAcc = true;
    } else if (!jsSkip(&j)) {
      return false;
    }
  }
  jsWs(&j);
  if (j.p != j.e) {
    return false;                             // a second root, or text after the object
  }
  if (!haveLoc || !haveAcc || !isfinite(la) || !isfinite(lo) || !isfinite(acc)) {
    return false;
  }
  if (!validCoords(la, lo) || !(acc > 0.0 && acc < 20000000.0)) {
    return false;
  }
  if (lat) *lat = la;
  if (lon) *lon = lo;
  if (accM) *accM = (uint32_t)ceil(acc);
  return true;
}

void geoHalfFrom(GeoHalf* h, int reply, int status, const char* body, size_t len) {
  if (!h) {
    return;
  }
  memset(h, 0, sizeof(*h));
  h->reply = (uint8_t)reply;
  h->status = status;
  if (reply == GEO_R_ANSWERED && status == 200) {
    h->ok = geoParseAnswer(body, len, &h->lat, &h->lon, &h->accM);
  }
}

double geoDistanceM(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371008.8;                 // mean Earth radius
  const double RAD = 0.017453292519943295;
  const double p1 = lat1 * RAD, p2 = lat2 * RAD;
  const double s1 = sin((lat2 - lat1) * RAD / 2), s2 = sin((lon2 - lon1) * RAD / 2);
  double h = s1 * s1 + cos(p1) * cos(p2) * s2 * s2;
  if (h > 1.0) {
    h = 1.0;
  }
  return 2.0 * R * asin(sqrt(h));
}

const char* geoVerdictName(int v) {
  switch (v) {
    case GEO_V_NONE:        return "none";
    case GEO_V_LOCATED:     return "located";
    case GEO_V_TOO_FEW:     return "too-few-aps";
    case GEO_V_NO_MEMORY:   return "no-memory";
    case GEO_V_UNREACHABLE: return "unreachable";
    case GEO_V_NOT_FOUND:   return "no-fix";
    case GEO_V_VAGUE:       return "too-vague";
    case GEO_V_DISAGREE:    return "disagree";
    case GEO_V_JUMP:        return "far-from-last-fix";
    case GEO_V_MOVED:       return "moved-during-look";
    default:                return "?";
  }
}

// One half on its own: GEO_V_NONE when it is a usable, tight location (so far, so good).
static int judgeHalf(const GeoHalf* h) {
  if (h && h->reply == GEO_R_NO_MEMORY) {
    return GEO_V_NO_MEMORY;
  }
  if (!h || h->reply != GEO_R_ANSWERED) {
    return GEO_V_UNREACHABLE;                 // never heard from (or never asked: same thing here)
  }
  if (h->status != 200 || !h->ok || !validCoords(h->lat, h->lon) || h->accM < 1) {
    return GEO_V_NOT_FOUND;
  }
  if (h->accM > GEO_MAX_HALF_ACC_M) {
    return GEO_V_VAGUE;
  }
  return GEO_V_NONE;
}

bool geoSecondHalfNeeded(const GeoHalf* first) {
  return judgeHalf(first) == GEO_V_NONE;
}

static int32_t toE7(double deg) {
  return (int32_t)llround(deg * 1e7);
}

int geoJudge(const GeoSplit* sp, const GeoHalf* a, const GeoHalf* b, GeoFix* out) {
  GeoFix fx;
  memset(&fx, 0, sizeof(fx));
  int v;
  if (!sp || !sp->built || sp->devices < GEO_MIN_DEVICES) {
    v = GEO_V_TOO_FEW;
  } else if ((v = judgeHalf(a)) != GEO_V_NONE) {
    // the first half decides alone when it fails (the second is then not asked)
  } else if ((v = judgeHalf(b)) != GEO_V_NONE) {
    // ...and the second when it does
  } else {
    const double apart = geoDistanceM(a->lat, a->lon, b->lat, b->lon);
    const uint32_t reach = a->accM > b->accM ? a->accM : b->accM;
    fx.apartM = apart >= 4.0e9 ? 4000000000u : (uint32_t)ceil(apart);
    if (apart > (double)reach) {
      v = GEO_V_DISAGREE;
    } else {
      /* The midpoint; across the date line too (the longitudes differ by a few hundred metres at
       * most, so the short way round is the right one). */
      double dLon = b->lon - a->lon;
      if (dLon > 180.0) dLon -= 360.0;
      if (dLon < -180.0) dLon += 360.0;
      double lon = a->lon + dLon / 2.0;
      if (lon > 180.0) lon -= 360.0;
      if (lon < -180.0) lon += 360.0;
      uint32_t acc = GEO_MIN_REPORTED_ACC_M;
      if (reach > acc) acc = reach;
      if (fx.apartM > acc) acc = fx.apartM;
      fx.latI = toE7((a->lat + b->lat) / 2.0);
      fx.lonI = toE7(lon);
      fx.accM = (uint16_t)acc;
      v = GEO_V_LOCATED;
    }
  }
  if (out) {
    *out = fx;
  }
  return v;
}

// ---------------------------------------------------------------- the jump backstop

void geoAnchorFrom(const SelfSlot slots[SELF_SRC_COUNT], uint32_t nowMs, bool allowInjected,
                   GeoAnchor* out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (!slots) {
    return;
  }
  uint32_t bestAge = 0;
  for (int src = SELF_GPS; src <= SELF_PHONE; src++) {
    const SelfSlot* s = &slots[src - 1];
    if (s->fixMs == 0 || (s->injected && !allowInjected)) {
      continue;
    }
    /* The source's own quality bar: a 3-satellite fix can be 20 km off (mesh_pos.h), and refusing
     * a true Wi-Fi fix on its word would be its error, not the Wi-Fi's. */
    if (!meshPosFixUsable(s->sats, s->hdopX10)) {
      continue;
    }
    const uint32_t age = (uint32_t)(nowMs - s->fixMs);
    if (age >= GEO_JUMP_WINDOW_MS) {
      continue;
    }
    if (!out->ok || age < bestAge) {
      out->ok = true;
      out->latI = s->latI;
      out->lonI = s->lonI;
      out->ms = s->fixMs;
      bestAge = age;
    }
  }
}

bool geoJumpOk(const GeoAnchor* a, int32_t latI, int32_t lonI, uint16_t accM, uint32_t fixMs) {
  if (!a || !a->ok) {
    return true;
  }
  /* Signed: the anchor may be newer than the scan (a GPS fix that arrived while the look was out),
   * and the stamps may straddle the millis() wrap. */
  const int32_t d = (int32_t)(fixMs - a->ms);
  const double dtS = (d < 0 ? -(double)d : (double)d) / 1000.0;
  const double dist = geoDistanceM(a->latI * 1e-7, a->lonI * 1e-7, latI * 1e-7, lonI * 1e-7);
  return dist <= (double)accM + GEO_JUMP_SLACK_M + GEO_JUMP_MAX_MPS * dtS;
}

// ---------------------------------------------------------------- the set a fix stands on

void geoSetFromSplit(GeoSet* set, const GeoSplit* sp) {
  if (!set) {
    return;
  }
  memset(set, 0, sizeof(*set));
  if (!sp) {
    return;
  }
  set->n = sp->n > GEO_RANKED_MAX ? GEO_RANKED_MAX : sp->n;
  for (int k = 0; k < set->n; k++) {
    memcpy(set->mac[k], sp->q[k].mac, 6);
  }
  uint8_t dev[GEO_RANKED_MAX];
  set->devices = (uint8_t)groupDevices(set->mac[0], 6, set->n, dev);
}

int geoSetHeard(const GeoSet* set, const GeoScan* scan, int* devices) {
  if (devices) {
    *devices = 0;
  }
  if (!set || set->n == 0) {
    return 0;
  }
  uint8_t dev[GEO_RANKED_MAX];
  const int nd = groupDevices(set->mac[0], 6, set->n, dev);
  if (devices) {
    *devices = nd;
  }
  bool heard[GEO_RANKED_MAX];
  memset(heard, 0, sizeof(heard));
  if (scan) {
    for (int i = 0; i < scan->n; i++) {
      if (scan->ap[i].rssi < GEO_PRESENT_RSSI) {
        continue;
      }
      /* Any address of the device counts — its other band or guest network too: the question is
       * whether the ROUTER is still near, not whether one radio of it happened to be heard. */
      for (int k = 0; k < set->n; k++) {
        if (!heard[dev[k]] && geoSameDevice(scan->ap[i].mac, set->mac[k])) {
          heard[dev[k]] = true;
        }
      }
    }
  }
  int nh = 0;
  for (int k = 0; k < set->n; k++) {
    if (dev[k] == k && heard[k]) {
      nh++;
    }
  }
  return nh;
}

bool geoSetStillHere(const GeoSet* set, const GeoScan* scan) {
  int devices = 0;
  const int heard = geoSetHeard(set, scan, &devices);
  return devices == 0 || heard * 3 >= devices * 2;
}

// ---------------------------------------------------------------- the schedule

static uint32_t stamp(uint32_t ms) {
  return ms ? ms : 1;                         // 0 means "never" in every field below
}

void geoSchedInit(GeoSched* s) {
  if (s) {
    memset(s, 0, sizeof(*s));
    s->lastVerdict = GEO_V_NONE;
  }
}

void geoAsk(GeoSched* s, uint32_t nowMs) {
  if (s) {
    s->askMs = stamp(nowMs);
  }
}

bool geoAsked(const GeoSched* s, uint32_t nowMs) {
  return s && s->askMs != 0 && (uint32_t)(nowMs - s->askMs) < GEO_ASK_HOLD_MS;
}

int geoAttemptsLastHour(const GeoSched* s, uint32_t nowMs) {
  if (!s) {
    return 0;
  }
  int n = 0;
  for (int i = 0; i < GEO_ATTEMPTS_PER_HOUR; i++) {
    if (s->attemptMs[i] != 0 && (uint32_t)(nowMs - s->attemptMs[i]) < GEO_HOUR_MS) {
      n++;
    }
  }
  return n;
}

bool geoScanRequestOk(const GeoSched* s, uint32_t nowMs) {
  return s && (s->scanReqMs == 0 || (uint32_t)(nowMs - s->scanReqMs) >= GEO_SCAN_REQ_GAP_MS);
}

void geoScanRequested(GeoSched* s, uint32_t nowMs) {
  if (s) {
    s->scanReqMs = stamp(nowMs);
  }
}

const char* geoDueName(int d) {
  switch (d) {
    case GEO_DUE_YES:      return "yes";
    case GEO_DUE_DRY:      return "dry";
    case GEO_DUE_SCAN:     return "need-scan";
    case GEO_DUE_OFF:      return "off";
    case GEO_DUE_BUSY:     return "busy";
    case GEO_DUE_NO_WIFI:  return "no-wifi";
    case GEO_DUE_MEASURED: return "measured-fix";
    case GEO_DUE_UNWANTED: return "not-wanted";
    case GEO_DUE_NET_BUSY: return "net-busy";
    case GEO_DUE_MUSIC:    return "music";
    case GEO_DUE_SAME:     return "same-place";
    case GEO_DUE_TOO_SOON: return "too-soon";
    case GEO_DUE_HOUR_CAP: return "hour-cap";
    default:               return "?";
  }
}

int geoLookDue(const GeoSched* s, const GeoWhenIn* in, uint32_t nowMs) {
  if (!s || !in || geoSettingSane(in->setting) == GEO_SET_OFF) {
    return GEO_DUE_OFF;
  }
  if (in->busy) {
    return GEO_DUE_BUSY;
  }
  if (!in->wifiUp) {
    return GEO_DUE_NO_WIFI;
  }
  if (in->measuredLive) {
    return GEO_DUE_MEASURED;                  // a measurement outranks this; asking would only cost
  }
  const bool asked = geoAsked(s, nowMs);
  const bool viewed = in->viewedMs != 0 && (uint32_t)(nowMs - in->viewedMs) < GEO_WANT_MS;
  const bool beacon = in->beaconArmed && in->setting == GEO_SET_MESH;
  if (!asked && !viewed && !beacon) {
    return GEO_DUE_UNWANTED;
  }
  if (!in->netOk) {
    return GEO_DUE_NET_BUSY;
  }
  if (in->musicPlaying && !asked) {
    return GEO_DUE_MUSIC;
  }
  /* What could make a new look answer differently from the last one. A new association with a
   * fix in hand adds nothing (its own scans keep confirming it); without one, it is worth a look
   * at the air once — phone 1's WiFi drops and rejoins must not each cost two lookups, which is
   * why the term ends the moment a scan of this association has been judged (geoOnScan). */
  const bool due = asked || s->lastLookMs == 0 || s->lastTransient || s->changed ||
                   (s->fixFromRef && !in->netLive) || (!in->netLive && s->scanAssoc != in->assoc);
  if (!due) {
    return GEO_DUE_SAME;
  }
  const uint32_t gap = asked ? GEO_LOOK_GAP_ASKED_MS : GEO_LOOK_GAP_MS;
  if (s->lastLookMs != 0 && (uint32_t)(nowMs - s->lastLookMs) < gap) {
    return GEO_DUE_TOO_SOON;
  }
  const bool capped = geoAttemptsLastHour(s, nowMs) >= GEO_ATTEMPTS_PER_HOUR;
  if (!geoScanFresh(in->scan, in->assoc, nowMs)) {
    return capped ? GEO_DUE_HOUR_CAP : GEO_DUE_SCAN;    // no scan spent on a look that cannot run
  }
  if (in->scanDevices < GEO_MIN_DEVICES) {
    return GEO_DUE_DRY;                       // costs nobody anything: the cap does not apply
  }
  return capped ? GEO_DUE_HOUR_CAP : GEO_DUE_YES;
}

int geoOnScan(GeoSched* s, const GeoScan* scan, uint8_t scanDevices, SelfSlot* net) {
  if (!s || !scan || scan->ms == 0) {
    return GEO_SCAN_NO_FIX;
  }
  s->scanAssoc = scan->assoc;
  const bool here = geoSetStillHere(&s->ref, scan);
  /* A reason to look again: the last look's devices are no longer around — or that look had too
   * few to ask about, and now there are enough. */
  if (!here || (s->ref.devices < GEO_MIN_DEVICES && scanDevices >= GEO_MIN_DEVICES)) {
    s->changed = true;
  }
  if (!s->fixFromRef) {
    return GEO_SCAN_NO_FIX;
  }
  if (s->held) {
    /* A fix held since the link changed (geoOnLink): this scan is the judge it was waiting for.
     * Still heard, and not expired meanwhile -> back, as of this scan, on this association. */
    s->held = false;
    const bool fresh = (uint32_t)(scan->ms - s->heldFix.fixMs) < SELF_NET_SHOW_MS;
    if (here && fresh && net && net->enabled && net->fixMs == 0) {
      const bool noTx = net->noTx;            // the setting NOW, not when it was held
      *net = s->heldFix;
      net->enabled = true;
      net->noTx = noTx;
      selfNetConfirm(net, scan->ms, scan->utc);
      s->fixAssoc = scan->assoc;
      memset(&s->heldFix, 0, sizeof(s->heldFix));
      return GEO_SCAN_BACK;
    }
    memset(&s->heldFix, 0, sizeof(s->heldFix));
    if (!here) {
      s->fixFromRef = false;
      return GEO_SCAN_MOVED;
    }
    return GEO_SCAN_NO_FIX;                   // expired while held: gone (and a reason to look)
  }
  if (here) {
    if (!net) {
      return GEO_SCAN_CONFIRMED;
    }
    if (net->fixMs == 0) {
      return GEO_SCAN_NO_FIX;                 // expired and cleared, or switched off: stays gone
    }
    /* An EXPIRED fix is gone (self_pos.h) and stays gone: a scan does not bring it back, and it is
     * cleared now (geoExpire's rule). The expiry itself is a reason to look again (geoLookDue:
     * fixFromRef with nothing live), and that look replaces it. */
    if (selfSlotState(net, SELF_NET, scan->ms, SELF_PICK_INJECTED) == SELF_ST_OLD) {
      selfSlotClear(net);
      return GEO_SCAN_NO_FIX;
    }
    if (selfNetConfirm(net, scan->ms, scan->utc)) {
      s->fixAssoc = scan->assoc;
    }
    return GEO_SCAN_CONFIRMED;
  }
  const bool had = !net || net->fixMs != 0;
  if (net) {
    selfSlotClear(net);                       // moved: not one more second on the map
  }
  s->fixFromRef = false;
  return had ? GEO_SCAN_MOVED : GEO_SCAN_NO_FIX;
}

bool geoOnLink(GeoSched* s, SelfSlot* net, uint32_t assoc) {
  if (!s || !net || !s->fixFromRef || s->held || net->fixMs == 0 || s->fixAssoc == assoc) {
    return false;
  }
  s->heldFix = *net;
  s->held = true;
  selfSlotClear(net);                         // off every screen and the air until a scan judges it
  return true;
}

bool geoExpire(GeoSched* s, SelfSlot* net, uint32_t nowMs) {
  if (s && s->held && (uint32_t)(nowMs - s->heldFix.fixMs) >= SELF_NET_SHOW_MS) {
    s->held = false;                          // nothing to bring back any more
    memset(&s->heldFix, 0, sizeof(s->heldFix));
  }
  if (!net || net->fixMs == 0 || (uint32_t)(nowMs - net->fixMs) < SELF_NET_SHOW_MS) {
    return false;
  }
  selfSlotClear(net);
  return true;
}

bool geoRefreshWanted(const GeoSched* s, const GeoWhenIn* in, const SelfSlot* net, uint32_t nowMs) {
  if (!s || !in || geoSettingSane(in->setting) == GEO_SET_OFF || in->busy || !in->wifiUp ||
      in->measuredLive) {
    return false;
  }
  const bool asked = geoAsked(s, nowMs);
  const bool viewed = in->viewedMs != 0 && (uint32_t)(nowMs - in->viewedMs) < GEO_WANT_MS;
  const bool beacon = in->beaconArmed && in->setting == GEO_SET_MESH;
  const bool wanted = asked || viewed || beacon;
  if (s->held && wanted) {
    return true;                              // the scan that brings it back, or drops it
  }
  if (in->netLive && (viewed || asked)) {
    return true;                              // on screen: re-checked every GEO_SCAN_REQ_GAP_MS
  }
  return in->netLive && wanted && selfNetWantsRefresh(net, nowMs);
}

static bool reachedNetwork(const GeoHalf* h) {
  return h && (h->reply == GEO_R_ANSWERED || h->reply == GEO_R_NO_ANSWER);
}

// The look as beaconDB saw it: the gap, and an attempt when a half may have reached it.
static void noteLook(GeoSched* s, const GeoHalf* ha, const GeoHalf* hb, uint32_t nowMs) {
  s->lastLookMs = stamp(nowMs);
  /* An attempt is what beaconDB saw: up to two lookups. One that never left the phone (no RAM
   * for a TLS session, or a dry look) costs the service nothing and is not counted. */
  if (reachedNetwork(ha) || reachedNetwork(hb)) {
    memmove(&s->attemptMs[1], &s->attemptMs[0], sizeof(s->attemptMs) - sizeof(s->attemptMs[0]));
    s->attemptMs[0] = stamp(nowMs);
  }
}

void geoFoldCancelled(GeoSched* s, const GeoHalf* a, const GeoHalf* b, uint32_t nowMs) {
  if (!s) {
    return;
  }
  if (reachedNetwork(a) || reachedNetwork(b)) {
    noteLook(s, a, b, nowMs);                 // the cap and the gap; nothing else is kept
  }
}

int geoFold(GeoSched* s, const GeoSplit* sp, const GeoHalf* a, const GeoHalf* b, uint32_t nowMs,
            bool bench, SelfSlot* net, GeoFix* out, const GeoFoldGuard* guard) {
  GeoHalf none;
  memset(&none, 0, sizeof(none));             // GEO_R_NOT_ASKED
  const GeoHalf* ha = a ? a : &none;
  const GeoHalf* hb = b ? b : &none;
  GeoFix fx;
  int v = geoJudge(sp, ha, hb, &fx);
  /* The two things two agreeing halves cannot see (geoloc.h, GeoFoldGuard). */
  if (v == GEO_V_LOCATED && guard && guard->anchor &&
      !geoJumpOk(guard->anchor, fx.latI, fx.lonI, fx.accM, sp->scanMs)) {
    v = GEO_V_JUMP;
  }
  const GeoScan* later = (guard && guard->latest && guard->latest->ms != 0 && sp &&
                          (int32_t)(guard->latest->ms - sp->scanMs) > 0) ? guard->latest : NULL;
  if (v == GEO_V_LOCATED && later) {
    GeoSet asked;
    geoSetFromSplit(&asked, sp);
    if (!geoSetStillHere(&asked, later)) {
      v = GEO_V_MOVED;                        // contradicted before it was ever shown
    }
  }
  if (out) {
    *out = fx;
  }
  if (!s) {
    return v;
  }
  const bool live = net && selfSlotState(net, SELF_NET, nowMs, SELF_PICK_INJECTED) == SELF_ST_LIVE;
  if (!bench) {
    noteLook(s, ha, hb, nowMs);
    s->askMs = 0;
    s->changed = (v == GEO_V_MOVED);          // the newer scan already says: look again
    if (sp) {
      s->scanAssoc = sp->scanAssoc;
    }
    s->lastTransient = (v == GEO_V_UNREACHABLE || v == GEO_V_NO_MEMORY);
  }
  s->lastVerdict = (uint8_t)v;
  s->lastDevices = sp ? sp->devices : 0;
  s->lastApartM = fx.apartM;
  s->lastAccM = 0;
  if (v == GEO_V_LOCATED) {
    s->lastAccM = fx.accM;
  } else if (v == GEO_V_VAGUE) {
    const uint32_t worst = (ha->ok && ha->accM > (hb->ok ? hb->accM : 0)) ? ha->accM
                                                                        : (hb->ok ? hb->accM : 0);
    s->lastAccM = (uint16_t)(worst > 0xFFFF ? 0xFFFF : worst);
  }
  if (v == GEO_V_LOCATED && net && net->enabled) {
    selfNetSet(net, fx.latI, fx.lonI, fx.accM, sp->scanMs, sp->scanUtc, bench);
    geoSetFromSplit(&s->ref, sp);
    s->fixFromRef = true;
    s->fixAssoc = sp->scanAssoc;
    s->held = false;                          // a new fix replaces one held since a link change
    memset(&s->heldFix, 0, sizeof(s->heldFix));
    if (later) {
      selfNetConfirm(net, later->ms, later->utc);   // the newer scan still hears them: its time
      s->fixAssoc = later->assoc;
    }
  } else if (!(s->fixFromRef && live)) {
    /* No fix in hand to keep: this look's devices are the reference for "has anything changed".
     * (A live fix keeps its own: a failed `netloc now` must not move the yardstick the scans hold
     * that fix against.) A held one is let go with its yardstick. */
    geoSetFromSplit(&s->ref, sp);
    s->fixFromRef = false;
    s->held = false;
    memset(&s->heldFix, 0, sizeof(s->heldFix));
  }
  return v;
}

int geoBenchFold(GeoSched* s, const GeoSplit* real, const GeoHalf* a, const GeoHalf* b,
                 uint32_t nowMs, uint32_t utc, uint32_t assoc, SelfSlot* net, GeoFix* out,
                 const GeoAnchor* anchor) {
  GeoFoldGuard guard;
  guard.anchor = anchor;
  guard.latest = NULL;                        // stamped now: no scan can be newer than it
  GeoSplit sp;
  const bool standIn = !(real && real->built);
  if (!standIn) {
    sp = *real;
  } else {
    /* Four devices that exist nowhere: geoJudge reads only the counts, and the addresses are
     * never put in a request (a bench fold sends nothing) nor kept as the yardstick (below). */
    memset(&sp, 0, sizeof(sp));
    sp.usable = sp.n = sp.devices = GEO_MIN_DEVICES;
    sp.halfA = sp.halfB = GEO_MIN_DEVICES / 2;
    for (int k = 0; k < GEO_MIN_DEVICES; k++) {
      sp.q[k].half = (uint8_t)(k & 1);
    }
    sp.built = true;
  }
  sp.scanMs = stamp(nowMs);
  sp.scanUtc = utc;
  sp.scanAssoc = assoc;
  if (!s) {
    return geoFold(NULL, &sp, a, b, nowMs, true, net, out, &guard);
  }
  const GeoSet keepRef = s->ref;
  const bool keepFrom = s->fixFromRef;
  const bool keepHeld = s->held;
  const SelfSlot keepHeldFix = s->heldFix;
  const int v = geoFold(s, &sp, a, b, nowMs, true, net, out, &guard);
  if (standIn) {
    /* The scans keep being held against the real devices of the last look (a change there is
     * still a reason to look), and a stand-in fix in the slot stands on none of them. A canned
     * failure leaves a fix held since a link change exactly as it was. */
    const bool wrote = v == GEO_V_LOCATED && net && net->enabled;
    s->ref = keepRef;
    s->fixFromRef = wrote ? false : keepFrom;
    if (!wrote) {
      s->held = keepHeld;
      s->heldFix = keepHeldFix;
    }
    s->lastDevices = real ? real->devices : 0;
  }
  return v;
}

// ---------------------------------------------------------------- the auto-switcher's scan

int geoSwitcherMode(bool stationAllowed, bool reconnect, bool autoSwitch, bool geoWantsScan,
                    bool connected, bool phoneStreamLive) {
  if (!stationAllowed || !reconnect) {
    return GEO_SW_NONE;                       // the same gates the reconnect loop honours
  }
  if (autoSwitch) {
    return (connected && phoneStreamLive) ? GEO_SW_SCAN_ONLY : GEO_SW_FULL;
  }
  return (geoWantsScan && connected) ? GEO_SW_SCAN_ONLY : GEO_SW_NONE;
}

// ---------------------------------------------------------------- the screen's words

static size_t finish(char* out, size_t cap, int n) {
  if (n < 0) {
    out[0] = 0;
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t geoRowText(char* out, size_t cap, int setting, const GeoSched* s, const SelfFix* net, int due) {
  if (!out || cap == 0) {
    return 0;
  }
  const int set = geoSettingSane(setting);
  /* Every line fits the row's 232 px of Akrobat Bold 16 (test_geoloc measures them with the
   * firmware's own glyph table): the ellipsis must never eat what the line is for. */
  if (set == GEO_SET_OFF) {
    // The consent: what turning it on does, and to whom - on the row that turns it on.
    return finish(out, cap, snprintf(out, cap, "if on: sends Wi-Fi IDs to beaconDB"));
  }
  if (net && net->src == SELF_NET && net->state == SELF_ST_LIVE) {
    const unsigned min = (unsigned)(net->ageMs / 60000u);
    char age[16];
    if (min == 0) {
      snprintf(age, sizeof(age), "just now");
    } else {
      snprintf(age, sizeof(age), "%u min ago", min);
    }
    return finish(out, cap, snprintf(out, cap, "+/-%u m, %s%s", (unsigned)net->accM, age,
                                     net->injected ? " (bench)"
                                     : set == GEO_SET_MAP ? ", map only" : ""));
  }
  const char* t = NULL;
  switch (due) {
    case GEO_DUE_NO_WIFI:  t = "not on WiFi"; break;
    case GEO_DUE_MEASURED: t = "not needed: GPS/phone fix in hand"; break;
    case GEO_DUE_MUSIC:    t = "waits while music plays"; break;
    default: break;
  }
  if (!t && s && s->held) {
    t = "WiFi changed: waiting for a scan";      // geoOnLink: the fix is off the map until then
  }
  if (t) {
    return finish(out, cap, snprintf(out, cap, "%s", t));
  }
  switch (s ? s->lastVerdict : GEO_V_NONE) {
    case GEO_V_TOO_FEW:
      return finish(out, cap, snprintf(out, cap, "too few Wi-Fi networks (%u of %u)",
                                       (unsigned)s->lastDevices, (unsigned)GEO_MIN_DEVICES));
    case GEO_V_NOT_FOUND:   t = "not enough known Wi-Fi here"; break;
    case GEO_V_VAGUE:
      return finish(out, cap, snprintf(out, cap, "too vague (+/-%u m): not used",
                                       (unsigned)s->lastAccM));
    case GEO_V_DISAGREE:    t = "lookups disagree: not used"; break;
    case GEO_V_JUMP:        t = "far from your last fix: not used"; break;
    case GEO_V_MOVED:       t = "moved during the lookup"; break;
    case GEO_V_UNREACHABLE: t = "beaconDB unreachable: will retry"; break;
    case GEO_V_NO_MEMORY:   t = "low memory: will retry"; break;
    case GEO_V_LOCATED:     t = "no fix now (moved or expired)"; break;
    default:
      t = (due == GEO_DUE_UNWANTED) ? "used while Maps or Almanac is open"
          : (due == GEO_DUE_SCAN)   ? "waiting for a Wi-Fi scan"
                                    : "waiting";
      break;
  }
  return finish(out, cap, snprintf(out, cap, "%s", t));
}
