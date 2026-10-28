/*
 * test_geoloc.cpp — geoloc.cpp: the Wi-Fi location's pure half (0.9.82). The beaconDB request
 * bodies (MAC formatting, the privacy filters, the -85 dBm floor, the halves dealt by device and
 * signal rank), the answer's parser on json_read.cpp (good answers, the real 404, fallbacks,
 * garbage, missing fields, out-of-range values), the two-half agreement — the X4 Pro e-reader's
 * own vectors for the same protocol (crosspoint-reader test/geolocate), ported — and this phone's
 * rules on top: the scan snapshot, when a look runs, how a later scan confirms or drops a fix,
 * the fold into the real self_pos NET slot, the auto-switcher's scan mode and the row's words.
 * There is no internet-address lookup to test: none exists.
 *
 * Fixtures are public landmarks (the Space Needle, Pike Place Market), the date line near
 * Taveuni and the suite's dummy 40.0,-100.0 only: this repo is public. BSSIDs use IANA's own OUI
 * 00:00:5e (RFC 7042), which no vendor's hardware carries: one device per number in the fourth
 * octet (00:00:5e:<n>:53:00, 64 Ki apart), and the documentation block 00:00:5e:00:53:xx for
 * several addresses of one device.
 */

#include "../WiPhone/geoloc.h"
#include "../WiPhone/book_hash.h"      // SHA-256, for the pinned roots' fingerprints
#define PROGMEM
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../WiPhone/src/assets/fonts.h"   // the row's font, to measure every line it shows
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

// ---------------------------------------------------------------- fixtures

// Any address, with the flags its SSID earns (geoSsidFlags, as the scan copy computes them).
static GeoAp at(const uint8_t (&mac)[6], int rssi, const char* ssid = "CoffeeShop") {
  GeoAp a;
  memset(&a, 0, sizeof(a));
  memcpy(a.mac, mac, 6);
  a.rssi = (int8_t)(rssi > 127 ? 127 : rssi);
  uint8_t buf[33];
  memset(buf, 0, sizeof(buf));
  memcpy(buf, ssid, strlen(ssid) < 32 ? strlen(ssid) : 32);
  a.flags = geoSsidFlags(buf, sizeof(buf));
  return a;
}

// Device n: 00:00:5e:<n>:53:00 (first: the first octet, for the locally administered/group bits).
static GeoAp ap(uint8_t n, int rssi, const char* ssid = "CoffeeShop", uint8_t first = 0x00) {
  const uint8_t mac[6] = { first, 0x00, 0x5e, n, 0x53, 0x00 };
  return at(mac, rssi, ssid);
}

// One more address of a single device: 00:00:5e:00:53:<last> (the documentation block).
static GeoAp radio(uint8_t last, int rssi, const char* ssid = "TravelRouter") {
  const uint8_t mac[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, last };
  return at(mac, rssi, ssid);
}

struct Bodies {
  GeoSplit split;
  std::string a, b;
};

static Bodies bodies(const std::vector<GeoAp>& aps, const uint8_t* exclude = NULL) {
  Bodies out;
  geoSplitAps(aps.empty() ? NULL : aps.data(), aps.size(), exclude, &out.split);
  char a[GEO_REQUEST_CAP], b[GEO_REQUEST_CAP];
  geoBuildHalf(&out.split, GEO_HALF_A, a, sizeof(a));
  geoBuildHalf(&out.split, GEO_HALF_B, b, sizeof(b));
  out.a = a;
  out.b = b;
  return out;
}

static std::string entry(const char* mac, int rssi) {
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"macAddress\":\"%s\",\"signalStrength\":%d}", mac, rssi);
  return buf;
}

static const std::string ENVELOPE =
    "{\"considerIp\":false,\"fallbacks\":{\"ipf\":false,\"lacf\":false},\"wifiAccessPoints\":[";

static bool has(const std::string& s, const char* what) {
  return s.find(what) != std::string::npos;
}

// Space Needle.
static const char* NEEDLE_OK = "{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38.2}";

static bool beacon(const char* json, double* lat = NULL, double* lon = NULL, uint32_t* acc = NULL) {
  double la, lo;
  uint32_t ac;
  return geoParseAnswer(json, strlen(json), lat ? lat : &la, lon ? lon : &lo, acc ? acc : &ac);
}

// Four usable access points, strongest first: 01 (-50), 02 (-60), 03 (-70), 04 (-80).
static std::vector<GeoAp> four() {
  std::vector<GeoAp> v;
  v.push_back(ap(3, -70));
  v.push_back(ap(1, -50));
  v.push_back(ap(4, -80));
  v.push_back(ap(2, -60));
  return v;
}

static const double M_PER_DEG_LAT = 111195.0;

static GeoSplit built4() {
  GeoSplit s;
  memset(&s, 0, sizeof(s));
  s.usable = s.devices = 4;
  s.halfA = s.halfB = 2;
  s.built = true;
  return s;
}

static GeoHalf answer(double lat, double lon, uint32_t acc, int status = 200) {
  GeoHalf h;
  memset(&h, 0, sizeof(h));
  h.reply = GEO_R_ANSWERED;
  h.status = status;
  h.ok = true;
  h.lat = lat;
  h.lon = lon;
  h.accM = acc;
  return h;
}

static GeoHalf reply(int r) {
  GeoHalf h;
  memset(&h, 0, sizeof(h));
  h.reply = (uint8_t)r;
  return h;
}

static void scanOf(GeoScan* s, const std::vector<GeoAp>& aps, uint32_t ms, uint32_t utc, uint32_t assoc) {
  geoScanBegin(s, ms, utc, assoc);
  for (size_t i = 0; i < aps.size(); i++) {
    geoScanAdd(s, aps[i].mac, aps[i].rssi, aps[i].flags);
  }
}

static bool printable(const char* s) {
  for (const char* p = s; *p; p++) {
    if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e || *p == '`') {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------- the request

static void testRequest() {
  printf("request bodies\n");
  {
    const Bodies r = bodies(four());
    CHECK(r.split.built && r.split.usable == 4 && r.split.devices == 4 && r.split.halfA == 2 &&
          r.split.halfB == 2, "four devices: built, two in each half");
    CHECK(r.a == ENVELOPE + entry("00:00:5e:01:53:00", -50) + "," + entry("00:00:5e:03:53:00", -70) + "]}",
          "half A: ranks 1 and 3, strongest first, lowercase colon MACs");
    CHECK(r.b == ENVELOPE + entry("00:00:5e:02:53:00", -60) + "," + entry("00:00:5e:04:53:00", -80) + "]}",
          "half B: ranks 2 and 4");
    CHECK(has(r.a, "\"considerIp\":false") && has(r.a, "\"ipf\":false") && has(r.b, "\"considerIp\":false") &&
          has(r.b, "\"ipf\":false"), "beaconDB's own IP fallback is off twice over, in both bodies");
    CHECK(!has(r.a, "true") && !has(r.b, "true"), "nothing in either body is true");
    CHECK(!has(r.a + r.b, "CoffeeShop") && !has(r.a + r.b, "ssid"), "no SSID ever leaves the phone");
    std::vector<GeoAp> five = four();
    five.push_back(ap(5, -85));
    const Bodies o = bodies(five);
    CHECK(o.split.built && o.split.halfA == 3 && o.split.halfB == 2, "an odd count: A gets the extra one");
    CHECK(has(o.a, entry("00:00:5e:05:53:00", -85).c_str()), "-85 dBm itself is in");
  }
  {
    const uint8_t VRRP4[6] = { 0x00, 0x00, 0x5e, 0x00, 0x01, 0x10 };
    const uint8_t VRRP6[6] = { 0x00, 0x00, 0x5e, 0x00, 0x02, 0x11 };
    std::vector<GeoAp> v;
    v.push_back(ap(1, -40, ""));                 // hidden
    v.push_back(ap(2, -41, "Home_nomap"));       // opted out
    v.push_back(ap(3, -42, "Cabin_NoMap"));      // any case
    v.push_back(ap(4, -43, "Office_optout"));    // the other convention
    v.push_back(ap(5, -44, "Phone", 0x02));      // locally administered
    v.push_back(ap(6, -45, "Phone", 0x06));      // locally administered, another prefix
    v.push_back(ap(7, -46, "Group", 0x01));      // group bit
    v.push_back(at(VRRP4, -47, "Vrrp"));         // VRRP IPv4 virtual router
    v.push_back(at(VRRP6, -47, "Vrrp6"));        // VRRP IPv6
    v.push_back(ap(8, -60, "Library"));          // kept
    v.push_back(ap(9, -70, "nomap_Cafe"));       // "nomap" not at the end: kept
    v.push_back(ap(10, -75, "Museum_nomap "));   // not the suffix: kept
    v.push_back(ap(11, -80, "Bakery"));          // kept
    const Bodies r = bodies(v);
    CHECK(r.split.usable == 4 && r.split.built, "hidden, opted out, local, group and VRRP: all dropped");
    const std::string both = r.a + r.b;
    const char* gone[] = { "00:00:5e:01:53:00", "02:00:5e", "06:00:5e", "01:00:5e", "00:00:5e:00:01:10",
                           "00:00:5e:00:02:11" };
    bool none = true;
    for (size_t i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) none = none && !has(both, gone[i]);
    CHECK(none, "none of the refused addresses is in either body");
    const char* kept[] = { "00:00:5e:08:53:00", "00:00:5e:09:53:00", "00:00:5e:0a:53:00", "00:00:5e:0b:53:00" };
    bool all = true;
    for (size_t i = 0; i < 4; i++) all = all && has(both, kept[i]);
    CHECK(all, "the four ordinary ones are");
    const uint8_t VRRP4_ANY[6] = { 0x00, 0x00, 0x5e, 0x00, 0x01, 0x01 };
    const uint8_t VRRP6_ANY[6] = { 0x00, 0x00, 0x5e, 0x00, 0x02, 0x01 };
    const uint8_t NEXT[6] = { 0x00, 0x00, 0x5e, 0x00, 0x03, 0x01 };
    const uint8_t DOC[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x01 };
    GeoAp a1 = at(VRRP4_ANY, -50, "x"), a2 = at(VRRP6_ANY, -50, "x"), a3 = at(NEXT, -50, "x"), a4 = at(DOC, -50, "x");
    CHECK(!geoApUsable(&a1) && !geoApUsable(&a2) && geoApUsable(&a3) && geoApUsable(&a4),
          "VRRP's two ranges only: the address next to them, and the documentation block, are ordinary");
    GeoAp zero = ap(0, -30);
    memset(zero.mac, 0, 6);
    GeoAp one = ap(1, -30);
    CHECK(!geoApUsable(&zero) && geoApUsable(&one) && !geoApUsable(NULL), "an all-zero BSSID is refused");
  }
  {
    std::vector<GeoAp> v;
    v.push_back(ap(1, -50)); v.push_back(ap(2, -60)); v.push_back(ap(3, -70));
    v.push_back(ap(4, -86)); v.push_back(ap(5, -95));
    const Bodies r = bodies(v);
    CHECK(r.split.usable == 3 && !r.split.built && r.a == "" && r.b == "",
          "-86 dBm and weaker do not count: three is too few, both bodies empty");
  }
  {
    // The phone answered on this network: its hotspot's own BSSID travels with it.
    const uint8_t hotspot[6] = { 0x00, 0x00, 0x5e, 0x01, 0x53, 0x00 };
    std::vector<GeoAp> v = four();
    CHECK(has(bodies(v).a + bodies(v).b, "00:00:5e:01:53:00"), "not left out: the joined network is asked about");
    const Bodies left = bodies(v, hotspot);
    CHECK(left.split.usable == 3 && !left.split.built, "left out: three remain, too few");
    v.push_back(ap(6, -82));
    const Bodies five = bodies(v, hotspot);
    CHECK(five.split.built && !has(five.a + five.b, "00:00:5e:01:53:00"), "with one more: built, without it");
    CHECK(five.a.find(entry("00:00:5e:02:53:00", -60)) == ENVELOPE.size(), "the strongest left becomes rank 1");
    const uint8_t la[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    const uint8_t gl[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    CHECK(geoJoinedLeftOut(gl, true) && !geoJoinedLeftOut(gl, false) && geoJoinedLeftOut(la, false) &&
          !geoJoinedLeftOut(NULL, true),
          "the joined BSSID is left out when its gateway served NMEA, or when it is locally administered");
  }
  {
    std::vector<GeoAp> v;
    for (int i = 0; i < 50; i++) v.push_back(ap((uint8_t)i, -84 + (i % 40) / 2 + i / 40));
    const Bodies r = bodies(v);
    CHECK(r.split.built && r.split.usable == GEO_RANKED_MAX && r.split.halfA == GEO_MAX_REQUEST_APS &&
          r.split.halfB == GEO_MAX_REQUEST_APS, "fifty: the strongest forty, twenty a half");
    bool ordered = true;
    int prev = 1;
    for (size_t pos = r.a.find("signalStrength\":"); pos != std::string::npos;
         pos = r.a.find("signalStrength\":", pos + 1)) {
      const int rssi = atoi(r.a.c_str() + pos + 16);
      ordered = ordered && rssi <= prev;
      prev = rssi;
    }
    CHECK(ordered, "strongest first within a half");
  }
  {
    std::vector<GeoAp> v;
    v.push_back(ap(1, -80)); v.push_back(ap(2, -60)); v.push_back(ap(1, -50));
    v.push_back(ap(3, -70)); v.push_back(ap(4, -75));
    const Bodies r = bodies(v);
    CHECK(r.split.built && r.split.usable == 4 &&
          r.a == ENVELOPE + entry("00:00:5e:01:53:00", -50) + "," + entry("00:00:5e:03:53:00", -70) + "]}",
          "a BSSID seen twice counts once, at its stronger reading");
    std::vector<GeoAp> w;
    w.push_back(ap(1, -80)); w.push_back(ap(1, -50)); w.push_back(ap(2, -60)); w.push_back(ap(3, -61));
    CHECK(!bodies(w).split.built, "two readings of one access point are not two access points");
  }
  {
    const uint8_t a[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x10 };
    const uint8_t b[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x13 };  // 2.4/5 GHz, main and guest
    const uint8_t c[6] = { 0x00, 0x00, 0x5e, 0x00, 0x54, 0x0f };  // across the fifth octet, 255 on
    const uint8_t d[6] = { 0x00, 0x00, 0x5e, 0x00, 0x54, 0x10 };  // 256 on: another device
    const uint8_t e[6] = { 0x00, 0x00, 0x5f, 0x00, 0x53, 0x10 };  // another OUI
    const uint8_t f[6] = { 0x00, 0x00, 0x5e, 0x01, 0x53, 0x10 };  // 64 Ki on
    CHECK(geoSameDevice(a, a) && geoSameDevice(a, b) && geoSameDevice(b, a) && geoSameDevice(a, c) &&
          !geoSameDevice(a, d) && !geoSameDevice(a, e) && !geoSameDevice(a, f) && !geoSameDevice(a, NULL),
          "one device: the same OUI, tails under 256 apart");
  }
  {
    // A travel router beaconDB knows where it USED to be: four BSSIDs from one base, the strongest.
    std::vector<GeoAp> v;
    v.push_back(radio(0x10, -40)); v.push_back(radio(0x11, -42)); v.push_back(radio(0x12, -46));
    v.push_back(radio(0x13, -48)); v.push_back(ap(1, -70)); v.push_back(ap(2, -75)); v.push_back(ap(3, -80));
    const Bodies r = bodies(v);
    CHECK(r.split.built && r.split.usable == 7 && r.split.devices == 4, "seven addresses, four devices");
    bool inA = true;
    const char* router[] = { "00:00:5e:00:53:10", "00:00:5e:00:53:11", "00:00:5e:00:53:12", "00:00:5e:00:53:13" };
    for (int i = 0; i < 4; i++) inA = inA && has(r.a, router[i]) && !has(r.b, router[i]);
    CHECK(inA, "every address of the router goes into ONE half (it cannot answer for both)");
    CHECK(r.split.halfA == 5 && r.split.halfB == 2 &&
          r.b == ENVELOPE + entry("00:00:5e:01:53:00", -70) + "," + entry("00:00:5e:03:53:00", -80) + "]}",
          "devices by strongest address: router A, 1 B, 2 A, 3 B");
    v.pop_back();
    const Bodies few = bodies(v);
    CHECK(few.split.usable == 6 && few.split.devices == 3 && !few.split.built && few.a == "",
          "three devices in all: too few for two independent halves");
    std::vector<GeoAp> one;
    for (uint8_t i = 0; i < 10; i++) one.push_back(radio((uint8_t)(0x20 + i), -50 - i));
    CHECK(bodies(one).split.devices == 1 && !bodies(one).split.built, "ten BSSIDs of one device: one device");
  }
  {
    const uint8_t hotspot[6] = { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x10 };
    std::vector<GeoAp> v;
    v.push_back(radio(0x10, -35)); v.push_back(radio(0x14, -38)); v.push_back(ap(1, -60));
    v.push_back(ap(2, -65)); v.push_back(ap(3, -70)); v.push_back(ap(4, -75));
    const Bodies r = bodies(v, hotspot);
    CHECK(r.split.built && r.split.usable == 4 && r.split.devices == 4 && !has(r.a + r.b, "00:00:5e:00:53:1"),
          "a device left out is left out whole (the hotspot's other band too)");
  }
  {
    std::vector<GeoAp> v;
    CHECK(!bodies(v).split.built, "nothing: nothing");
    v.push_back(ap(1, -50)); v.push_back(ap(2, -60)); v.push_back(ap(3, -70)); v.push_back(ap(4, -60, ""));
    CHECK(!bodies(v).split.built, "three usable and one hidden: too few");
    GeoSplit sp;
    memset(&sp, 0x5a, sizeof(sp));
    geoSplitAps(NULL, 3, NULL, &sp);
    char a[8] = "x";
    CHECK(!sp.built && sp.usable == 0 && geoBuildHalf(&sp, GEO_HALF_A, a, sizeof(a)) == 0 && a[0] == 0,
          "no list: an empty split, and its body is \"\"");
    CHECK(geoBuildHalf(NULL, GEO_HALF_A, a, sizeof(a)) == 0 && geoBuildHalf(&sp, 2, a, sizeof(a)) == 0,
          "no split, or no such half: nothing");
  }
  {
    std::vector<GeoAp> v;
    v.push_back(ap(1, 5)); v.push_back(ap(2, -60)); v.push_back(ap(3, -70)); v.push_back(ap(4, -80));
    CHECK(has(bodies(v).a, entry("00:00:5e:01:53:00", 0).c_str()), "a positive signal (a driver quirk) is sent as 0");
  }
  {
    std::vector<GeoAp> v;
    for (int i = 0; i < 45; i++) v.push_back(ap((uint8_t)(0xa0 + i), -85));
    const Bodies r = bodies(v);
    CHECK(r.split.built && r.split.halfA == GEO_MAX_REQUEST_APS && r.a.size() < GEO_REQUEST_CAP &&
          r.a.substr(r.a.size() - 2) == "]}", "twenty entries fit the request buffer");
    // ...even at the longest a signal can print (-128), which the -85 bar never lets through
    GeoSplit sp = r.split;
    for (int k = 0; k < sp.n; k++) sp.q[k].rssi = -128;
    char big[GEO_REQUEST_CAP];
    CHECK(geoBuildHalf(&sp, GEO_HALF_A, big, sizeof(big)) == GEO_MAX_REQUEST_APS && strlen(big) == 1220 &&
          ENVELOPE.size() == 79, "twenty entries at -128 dBm: 1,220 characters (+ NUL), inside GEO_REQUEST_CAP");
  }
  {
    const Bodies full = bodies(four());
    char small[64];
    CHECK(geoBuildHalf(&full.split, GEO_HALF_A, small, sizeof(small)) == 0 && small[0] == 0,
          "a buffer too small gives nothing (never half a body)");
    const size_t longest = full.a.size();
    char a[256];
    CHECK(geoBuildHalf(&full.split, GEO_HALF_A, a, longest) == 0 && a[0] == 0,
          "one byte short (no room for the NUL): nothing");
    CHECK(geoBuildHalf(&full.split, GEO_HALF_A, a, longest + 1) == 2 && full.a == a, "exactly enough: the body");
  }
  {
    CHECK(!strcmp(GEO_URL, "https://api.beacondb.net/v1/geolocate") && !strcmp(GEO_HOST, "api.beacondb.net") &&
          !strcmp(GEO_CONTENT_TYPE, "application/json"), "beaconDB, over HTTPS, as JSON");
    bool digits = false;
    for (const char* p = GEO_USER_AGENT; *p; p++) digits = digits || (*p >= '0' && *p <= '9');
    CHECK(!digits && strlen(GEO_USER_AGENT) > 0, "a generic User-Agent: no version, nothing to tell phones apart");
    CHECK(GEO_MIN_DEVICES == 4 && GEO_MIN_RSSI == -85 && GEO_MAX_HALF_ACC_M == 100 &&
          GEO_MIN_REPORTED_ACC_M == 50 && GEO_ATTEMPTS_PER_HOUR == 3,
          "the bars: 4 devices, -85 dBm, 100 m a half, never under 50 m, 3 attempts an hour");
  }
}

// ---------------------------------------------------------------- the scan snapshot

static void testScan() {
  printf("the scan snapshot\n");
  uint8_t s[33];
  memset(s, 0, sizeof(s));
  CHECK(geoSsidFlags(s, sizeof(s)) == GEO_AP_HIDDEN && geoSsidFlags(NULL, 33) == GEO_AP_HIDDEN &&
        geoSsidFlags(s, 0) == GEO_AP_HIDDEN, "an empty SSID (or none): hidden");
  memcpy(s, "Trailhead_NOMAP", 15);
  CHECK(geoSsidFlags(s, sizeof(s)) == GEO_AP_NOMAP, "_NOMAP, any case: opted out");
  memset(s, 0, sizeof(s));
  memcpy(s, "x_OptOut", 8);
  CHECK(geoSsidFlags(s, sizeof(s)) == GEO_AP_NOMAP, "_optout: opted out");
  memset(s, 0, sizeof(s));
  memcpy(s, "_nomap", 6);
  CHECK(geoSsidFlags(s, sizeof(s)) == GEO_AP_NOMAP, "the bare suffix is the suffix");
  memset(s, 0, sizeof(s));
  memcpy(s, "nomap_Cafe", 10);
  CHECK(geoSsidFlags(s, sizeof(s)) == 0, "'nomap' at the start: an ordinary network");
  memset(s, 'a', 26);
  memcpy(s + 26, "_nomap", 6);
  s[32] = 0;
  CHECK(geoSsidFlags(s, sizeof(s)) == GEO_AP_NOMAP, "a full 32-byte SSID ending _nomap");
  memset(s, 'a', sizeof(s));
  CHECK(geoSsidFlags(s, sizeof(s)) == GEO_AP_ODD, "no NUL in 33 bytes: refused, never read past");
  GeoAp odd = ap(1, -50);
  odd.flags = GEO_AP_ODD;
  CHECK(!geoApUsable(&odd), "...and not usable");

  GeoScan sc;
  geoScanBegin(&sc, 0, 7, 99);
  CHECK(sc.ms == 1 && sc.utc == 7 && sc.assoc == 99 && sc.n == 0, "begin: millis 0 kept as 1 (0 = no snapshot)");
  for (int i = 0; i < 60; i++) {
    const uint8_t mac[6] = { 0x00, 0x00, 0x5e, (uint8_t)i, 0x53, 0x00 };
    geoScanAdd(&sc, mac, -30 - i, 0);
  }
  int weakest = 0;
  for (int i = 0; i < sc.n; i++) weakest = (i == 0 || sc.ap[i].rssi < weakest) ? sc.ap[i].rssi : weakest;
  CHECK(sc.n == GEO_SCAN_MAX && sc.seen == 60 && weakest == -30 - (GEO_SCAN_MAX - 1),
        "sixty heard: the strongest 48 kept, all 60 counted");
  const uint8_t loud[6] = { 0x00, 0x00, 0x5e, 0x77, 0x53, 0x00 };
  geoScanAdd(&sc, loud, 300, 0);
  bool found = false;
  for (int i = 0; i < sc.n; i++) found = found || (!memcmp(sc.ap[i].mac, loud, 6) && sc.ap[i].rssi == 127);
  CHECK(found && sc.n == GEO_SCAN_MAX, "a stronger one replaces the weakest; a silly reading is clamped to int8");
  const uint8_t quiet[6] = { 0x00, 0x00, 0x5e, 0x78, 0x53, 0x00 };
  geoScanAdd(&sc, quiet, -127, 0);
  found = false;
  for (int i = 0; i < sc.n; i++) found = found || !memcmp(sc.ap[i].mac, quiet, 6);
  CHECK(!found, "a weaker one is dropped when full");

  geoScanBegin(&sc, 1000, 0, 5);
  CHECK(geoScanFresh(&sc, 5, 1000 + GEO_SCAN_FRESH_MS - 1) && !geoScanFresh(&sc, 5, 1000 + GEO_SCAN_FRESH_MS) &&
        !geoScanFresh(&sc, 6, 1001) && !geoScanFresh(NULL, 5, 1001),
        "fresh: this association, under 2 minutes (119.999 s yes, 120 s no)");
  GeoSplit sp;
  std::vector<GeoAp> v = four();
  scanOf(&sc, v, 4242, 1760000000u, 77);
  geoSplitScan(&sc, NULL, &sp);
  CHECK(sp.built && sp.scanMs == 4242 && sp.scanUtc == 1760000000u && sp.scanAssoc == 77,
        "the split carries its scan's stamps (a newer scan may replace the snapshot mid-lookup)");
}

// ---------------------------------------------------------------- the answer

static void testAnswer() {
  printf("beaconDB's answer\n");
  double lat = 0, lon = 0;
  uint32_t acc = 0;
  CHECK(beacon(NEEDLE_OK, &lat, &lon, &acc) && lat == 47.6205 && lon == -122.3493 && acc == 39,
        "a good answer; the accuracy rounded UP (never better than the service said)");
  CHECK(beacon("\n{ \"accuracy\" : 1.2e2 ,\r\n \"location\" : { \"lng\" : -122.3422, \"lat\" : 4.76097e1 } }\n",
               &lat, &lon, &acc) && fabs(lat - 47.6097) < 1e-12 && lon == -122.3422 && acc == 120,
        "any order, whitespace, exponents");
  CHECK(!beacon("{\"error\":{\"code\":404,\"errors\":[{\"domain\":\"geolocation\",\"message\":\"No location could be "
                "estimated based on the data provided\",\"reason\":\"notFound\"}],\"message\":\"Not found\"}}"),
        "the real 404 body (notFound): refused");
  CHECK(!beacon("{\"error\":{\"code\":500},\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38}"),
        "an error object beside a location: refused");
  CHECK(!beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493}}") &&
        !beacon("{\"location\":{\"lat\":47.6205},\"accuracy\":30}") &&
        !beacon("{\"location\":{\"lng\":-122.3493},\"accuracy\":30}") &&
        !beacon("{\"lat\":47.6205,\"lng\":-122.3493,\"accuracy\":30}") &&
        !beacon("{\"x\":{\"location\":{\"lat\":47.6205,\"lng\":-122.3493}},\"accuracy\":30}") &&
        !beacon("{\"location\":{\"lat\":\"47.6205\",\"lng\":\"-122.3493\"},\"accuracy\":30}") &&
        !beacon("{\"location\":{\"lat\":null,\"lng\":-122.3493},\"accuracy\":30}") &&
        !beacon("{\"location\":null,\"accuracy\":30}") && !beacon("{}"),
        "missing fields, misplaced fields, strings and null: refused");
  CHECK(!beacon("{\"location\":{\"lat\":90.5,\"lng\":-122.3},\"accuracy\":30}") &&
        !beacon("{\"location\":{\"lat\":-91,\"lng\":-122.3},\"accuracy\":30}") &&
        !beacon("{\"location\":{\"lat\":47.6,\"lng\":180.01},\"accuracy\":30}") &&
        !beacon("{\"location\":{\"lat\":47.6,\"lng\":-181},\"accuracy\":30}") &&
        !beacon("{\"location\":{\"lat\":0,\"lng\":0},\"accuracy\":30}") &&
        !beacon("{\"location\":{\"lat\":47.6,\"lng\":-122.3},\"accuracy\":0}") &&
        !beacon("{\"location\":{\"lat\":47.6,\"lng\":-122.3},\"accuracy\":-5}") &&
        !beacon("{\"location\":{\"lat\":1e999,\"lng\":-122.3},\"accuracy\":30}") &&
        !beacon("{\"location\":{\"lat\":47.6,\"lng\":-122.3},\"accuracy\":1e999}") &&
        !beacon("{\"location\":{\"lat\":47.6,\"lng\":-122.3},\"accuracy\":20000000}"),
        "out of range, (0,0), accuracy 0 / negative / infinite / 2e7: refused");
  CHECK(beacon("{\"location\":{\"lat\":90,\"lng\":-180},\"accuracy\":30}"), "the edges are places");
  CHECK(!beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":25000,\"fallback\":\"ipf\"}") &&
        !beacon("{\"fallback\":\"lacf\",\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":3000}") &&
        !beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":50,\"fallback\":null}"),
        "a fallback estimate (IP or cell, not Wi-Fi), whatever its value: refused");
  CHECK(!beacon("") && !geoParseAnswer(NULL, 10, &lat, &lon, &acc) && !beacon("hello") &&
        !beacon("<html><body>502 Bad Gateway</body></html>") &&
        !beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38") &&
        !beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38,") &&
        !beacon("[{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38}]") &&
        !beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38}{}") &&
        !beacon("{\"location\":{\"lat\":NaN,\"lng\":-122.3493},\"accuracy\":38}") &&
        !beacon("{\"location\":{\"lat\":tru,\"lng\":-122.3493},\"accuracy\":38}") &&
        !beacon("{\"location\":{\"lat\":-,\"lng\":-122.3493},\"accuracy\":38}") &&
        !beacon("{\"location\":{\"lat\":47.,\"lng\":-122.3493},\"accuracy\":38}") &&
        !beacon("{\"location\":{\"lat\":047.6,\"lng\":-122.3493},\"accuracy\":38}"),
        "garbage, cut-off bodies, an array, two roots, non-JSON numbers: refused");
  CHECK(!beacon("{\"location\":{\"latitude_in_degrees\":47.6,\"lng\":-122.3},\"accuracy\":30}") &&
        beacon("{\"location\":{\"latitude_in_degrees\":1,\"lat\":47.6,\"lng\":-122.3},\"accuracy\":30}"),
        "a long key is not mistaken for a short one");
  {
    /* The e-reader's tokenizer DROPPED a 600-byte key and so had to refuse the body; json_read
     * reads it (cut to 15 characters) and skips its value: what matters is the same — the long
     * key's value never becomes "accuracy". */
    const std::string longKey(600, 'k');
    const std::string json = "{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38,\"" + longKey + "\":99999}";
    acc = 0;
    CHECK(geoParseAnswer(json.c_str(), json.size(), &lat, &lon, &acc) && acc == 38,
          "a 600-byte key does not lend its value to the last key");
    const std::string acc2 = "{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy" + longKey + "\":99999,\"accuracy\":38}";
    acc = 0;
    CHECK(geoParseAnswer(acc2.c_str(), acc2.size(), &lat, &lon, &acc) && acc == 38,
          "...nor does one that STARTS with \"accuracy\"");
  }
  CHECK(!beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38,\"extra\":}") &&
        !beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493,\"x\"},\"accuracy\":38}") &&
        !beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38}garbage") &&
        !beacon("garbage{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38}") &&
        !beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38}}"),
        "a key without a value, text around the object: refused");
  CHECK(beacon(" \r\n\t{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38}\n "),
        "whitespace around the object is fine");
  CHECK(!beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"accuracy\":38,\"accuracy\":5}") &&
        !beacon("{\"location\":{\"lat\":47.6205,\"lat\":40.0,\"lng\":-122.3493},\"accuracy\":38}") &&
        !beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493},\"location\":{\"lat\":40,\"lng\":-100},\"accuracy\":38}"),
        "a key twice (a body that says two things is not an answer): refused");
  CHECK(beacon("{\"location\":{\"lat\":47.6205,\"lng\":-122.3493,\"alt\":{\"a\":[1,2]}},\"accuracy\":38,\"x\":[{}]}"),
        "unknown keys, nested or not, are skipped");

  GeoHalf h;
  geoHalfFrom(&h, GEO_R_ANSWERED, 200, NEEDLE_OK, strlen(NEEDLE_OK));
  CHECK(h.reply == GEO_R_ANSWERED && h.status == 200 && h.ok && h.accM == 39, "an answered 200 is parsed");
  geoHalfFrom(&h, GEO_R_ANSWERED, 404, NEEDLE_OK, strlen(NEEDLE_OK));
  CHECK(!h.ok && h.status == 404, "anything but 200 is not even parsed");
  geoHalfFrom(&h, GEO_R_NO_ANSWER, 200, NEEDLE_OK, strlen(NEEDLE_OK));
  CHECK(!h.ok && h.reply == GEO_R_NO_ANSWER, "no answer: nothing parsed");
}

// ---------------------------------------------------------------- the two halves

static void testHalves() {
  printf("the two halves\n");
  GeoFix out;
  const GeoSplit s4 = built4();
  const double north = 47.6205 + 60.0 / M_PER_DEG_LAT;
  GeoHalf a = answer(47.6205, -122.3493, 80), b = answer(north, -122.3493, 70);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_LOCATED && out.latI == (int32_t)llround((47.6205 + north) / 2 * 1e7) &&
        out.lonI == -1223493000 && out.apartM >= 59 && out.apartM <= 61 && out.accM == 80,
        "two halves 60 m apart within 80/70: the midpoint, at the larger accuracy");
  a = answer(47.6205, -122.3493, 10);
  b = answer(47.6205, -122.3493, 12);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_LOCATED && out.apartM == 0 && out.accM == GEO_MIN_REPORTED_ACC_M,
        "never claimed tighter than 50 m");
  const double north95 = 47.6205 + 95.0 / M_PER_DEG_LAT;
  a = answer(47.6205, -122.3493, 30);
  b = answer(north95, -122.3493, 100);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_LOCATED && out.accM == 100, "95 m apart, accuracies 30 and 100: 100");
  a = answer(47.6205, -122.3493, 40);
  b = answer(47.6205 + 45.0 / M_PER_DEG_LAT, -122.3493, 40);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_DISAGREE, "45 m apart, both 40: they disagree");
  a = answer(47.6205, -122.3493, 60);
  b = answer(47.6205 + 55.0 / M_PER_DEG_LAT, -122.3493, 40);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_LOCATED && out.accM == 60, "55 m apart within 60: the larger accuracy");

  // a moved access point
  const double dragged = 47.6205 + 6500.0 / M_PER_DEG_LAT;
  a = answer(47.6205, -122.3493, 50);
  b = answer(dragged, -122.3493, 50);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_DISAGREE && out.latI == 0 && out.apartM >= 6498 && out.apartM <= 6502,
        "one half dragged 6.5 km by a moved access point, each reading 50 m: refused");
  a = answer(47.6205, -122.3493, 80);
  b = answer(47.6205 + 81.0 / M_PER_DEG_LAT, -122.3493, 40);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_DISAGREE, "81 m apart within 80/40: refused");
  b = answer(47.6205 + 79.0 / M_PER_DEG_LAT, -122.3493, 40);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_LOCATED, "79 m: agreed");

  // each half must be tight
  a = answer(47.6205, -122.3493, 101);
  b = answer(47.6205, -122.3493, 40);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_VAGUE, "a half at 101 m: too vague");
  a = answer(47.6205, -122.3493, 40);
  b = answer(47.6205, -122.3493, 5000);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_VAGUE, "the other at 5 km: too vague");
  a = answer(47.6205, -122.3493, 100);
  b = answer(47.6205, -122.3493, 100);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_LOCATED && out.accM == 100, "100 m each: accepted at 100");
  a = answer(47.6205, -122.3493, 0);
  b = answer(47.6205, -122.3493, 40);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_NOT_FOUND, "an accuracy of 0: no fix");

  // both must answer 200 with a location
  const GeoHalf good = answer(47.6205, -122.3493, 40);
  GeoHalf x = answer(47.6205, -122.3493, 40, 404);
  CHECK(geoJudge(&s4, &good, &x, &out) == GEO_V_NOT_FOUND, "a 404 half: no fix");
  x = answer(47.6205, -122.3493, 40, 302);
  CHECK(geoJudge(&s4, &x, &good, &out) == GEO_V_NOT_FOUND, "a 302 half: no fix");
  x = reply(GEO_R_ANSWERED);
  x.status = 200;
  CHECK(geoJudge(&s4, &good, &x, &out) == GEO_V_NOT_FOUND, "a 200 whose body did not parse (a fallback): no fix");
  x = answer(0.0, 0.0, 40);
  CHECK(geoJudge(&s4, &x, &good, &out) == GEO_V_NOT_FOUND && out.latI == 0, "(0,0): no fix");

  // the network, and too few
  GeoHalf na = reply(GEO_R_NO_ANSWER), nm = reply(GEO_R_NO_MEMORY), nq = reply(GEO_R_NOT_ASKED);
  CHECK(geoJudge(&s4, &na, &na, &out) == GEO_V_UNREACHABLE && geoJudge(&s4, &good, &na, &out) == GEO_V_UNREACHABLE &&
        geoJudge(&s4, &good, &nq, &out) == GEO_V_UNREACHABLE && geoJudge(&s4, &good, NULL, &out) == GEO_V_UNREACHABLE,
        "a half never heard from (or never asked): unreachable");
  CHECK(geoJudge(&s4, &nm, &good, &out) == GEO_V_NO_MEMORY && geoJudge(&s4, &good, &nm, &out) == GEO_V_NO_MEMORY,
        "no memory for a TLS session: said so");
  GeoSplit few = s4;
  few.devices = 3;
  few.usable = 3;
  CHECK(geoJudge(&few, &good, &good, &out) == GEO_V_TOO_FEW && geoJudge(NULL, &good, &good, &out) == GEO_V_TOO_FEW,
        "three devices (four addresses), or no split: too few");
  GeoSplit unbuilt = s4;
  unbuilt.built = false;
  CHECK(geoJudge(&unbuilt, &good, &good, &out) == GEO_V_TOO_FEW, "not built: too few");

  // the second half is spared when the first fails
  a = answer(47.6205, -122.3493, 40);
  CHECK(geoSecondHalfNeeded(&a), "a good first half: ask the second");
  a = answer(47.6205, -122.3493, 100);
  CHECK(geoSecondHalfNeeded(&a), "100 m: ask the second");
  a = answer(47.6205, -122.3493, 101);
  b = answer(47.6205, -122.3493, 40, 404);
  CHECK(!geoSecondHalfNeeded(&a) && !geoSecondHalfNeeded(&b) && !geoSecondHalfNeeded(&na) &&
        !geoSecondHalfNeeded(&nm) && !geoSecondHalfNeeded(&nq) && !geoSecondHalfNeeded(NULL),
        "101 m, a 404, no answer, no memory, not asked: the second is not asked");
  CHECK(geoJudge(&s4, &b, &nq, &out) == GEO_V_NOT_FOUND && geoJudge(&s4, &a, &nq, &out) == GEO_V_VAGUE &&
        geoJudge(&s4, &na, &nq, &out) == GEO_V_UNREACHABLE, "...and the verdict is the first half's");

  // the date line
  const double lat = -16.8;
  const double dLon = 30.0 / (M_PER_DEG_LAT * cos(lat * 3.141592653589793 / 180.0));
  a = answer(lat, 180.0 - dLon, 70);
  b = answer(lat, -180.0 + dLon, 70);
  CHECK(geoJudge(&s4, &a, &b, &out) == GEO_V_LOCATED && out.apartM >= 59 && out.apartM <= 61 &&
        (out.lonI >= 1799999000 || out.lonI <= -1799999000), "60 m apart across the date line: the midpoint on it");

  CHECK(fabs(geoDistanceM(47.6205, -122.3493, 47.6097, -122.3422) - 1300.0) < 60.0 &&
        geoDistanceM(47.6205, -122.3493, 47.6205, -122.3493) == 0.0, "Space Needle to Pike Place: ~1.3 km");
  bool names = true;
  for (int v = GEO_V_NONE; v <= GEO_V_LAST; v++) {
    const char* n = geoVerdictName(v);
    names = names && n && *n && !strchr(n, ' ') && strcmp(n, "?");
  }
  CHECK(names, "every verdict has a log token without spaces");
}

// ---------------------------------------------------------------- the set a fix stands on

static GeoSplit splitOf(const std::vector<GeoAp>& v, uint32_t ms = 1000, uint32_t utc = 0, uint32_t assoc = 1) {
  GeoScan sc;
  scanOf(&sc, v, ms, utc, assoc);
  GeoSplit sp;
  geoSplitScan(&sc, NULL, &sp);
  return sp;
}

// Devices 1..4 (one with a second radio on the 5 GHz band) at good signal.
static std::vector<GeoAp> place() {
  std::vector<GeoAp> v;
  v.push_back(ap(1, -50));
  v.push_back(ap(2, -60));
  v.push_back(ap(3, -65));
  v.push_back(ap(4, -70));
  const uint8_t twin[6] = { 0x00, 0x00, 0x5e, 0x01, 0x53, 0x04 };   // device 1's other radio
  v.push_back(at(twin, -55));
  return v;
}

static void testSet() {
  printf("the set a fix stands on\n");
  const GeoSplit sp = splitOf(place());
  GeoSet set;
  geoSetFromSplit(&set, &sp);
  CHECK(set.n == 5 && set.devices == 4 && sp.devices == 4, "five addresses, four devices");
  GeoScan sc;
  int devs = 0;
  scanOf(&sc, place(), 2000, 0, 1);
  CHECK(geoSetHeard(&set, &sc, &devs) == 4 && devs == 4 && geoSetStillHere(&set, &sc), "the same air: all four heard");
  std::vector<GeoAp> v;
  v.push_back(ap(1, -60)); v.push_back(ap(2, -70)); v.push_back(ap(3, -89)); v.push_back(ap(9, -40));
  scanOf(&sc, v, 3000, 0, 1);
  CHECK(geoSetHeard(&set, &sc, &devs) == 3 && geoSetStillHere(&set, &sc),
        "three of four (one at -89, under the -85 bar but over -90): still here (3/4 >= 2/3)");
  v[2] = ap(3, -91);
  scanOf(&sc, v, 3000, 0, 1);
  CHECK(geoSetHeard(&set, &sc, &devs) == 2 && !geoSetStillHere(&set, &sc), "two of four (one at -91): moved");
  v.clear();
  const uint8_t otherBand[6] = { 0x00, 0x00, 0x5e, 0x01, 0x53, 0x08 };   // device 1, a radio not asked about
  v.push_back(at(otherBand, -70)); v.push_back(ap(2, -70)); v.push_back(ap(3, -70));
  scanOf(&sc, v, 3000, 0, 1);
  CHECK(geoSetHeard(&set, &sc, &devs) == 3, "a device's other radio counts: the router is still near");
  GeoSet empty;
  geoSetFromSplit(&empty, NULL);
  CHECK(empty.n == 0 && geoSetStillHere(&empty, &sc) && geoSetHeard(&empty, &sc, &devs) == 0 && devs == 0,
        "an empty set: nothing to lose");
  CHECK(!geoSetStillHere(&set, NULL), "no scan at all: nothing heard");
}

// ---------------------------------------------------------------- the schedule

struct World {
  GeoSched s;
  GeoScan scan;
  GeoSplit split;
  SelfSlot net;
  GeoWhenIn in;
  bool viewing;      // Maps is on screen: viewedMs follows the clock
};

static void worldInit(World* w, uint32_t now) {
  geoSchedInit(&w->s);
  memset(&w->net, 0, sizeof(w->net));
  selfSlotClear(&w->net);
  w->net.enabled = true;
  memset(&w->in, 0, sizeof(w->in));
  w->in.setting = GEO_SET_MAP;
  w->in.wifiUp = true;
  w->in.netOk = true;
  w->in.viewedMs = now;
  w->in.assoc = 1;
  scanOf(&w->scan, place(), now, 1760000000u, 1);
  geoSplitScan(&w->scan, NULL, &w->split);
  w->in.scan = &w->scan;
  w->in.scanDevices = w->split.devices;
  w->viewing = true;
}

static bool netLive(const World* w, uint32_t now) {
  return selfSlotState(&w->net, SELF_NET, now, SELF_PICK_INJECTED) == SELF_ST_LIVE;
}

static int due(World* w, uint32_t now) {
  w->in.netLive = netLive(w, now);
  w->in.scan = &w->scan;                     // (a copied World points at its own snapshot)
  if (w->viewing) {
    w->in.viewedMs = now;
  }
  return geoLookDue(&w->s, &w->in, now);
}

// A completed scan of `v` at `now`: snapshot, split, held against the fix.
static int scanAt(World* w, const std::vector<GeoAp>& v, uint32_t now, uint32_t assoc = 1) {
  scanOf(&w->scan, v, now, 1760000000u + now / 1000, assoc);
  geoSplitScan(&w->scan, NULL, &w->split);
  w->in.scanDevices = w->split.devices;
  return geoOnScan(&w->s, &w->scan, w->split.devices, &w->net);
}

// Needle answers for both halves, 20 m apart.
static int foldNeedle(World* w, uint32_t now, bool bench = false) {
  const GeoHalf a = answer(47.6205, -122.3493, 40);
  const GeoHalf b = answer(47.6205 + 20.0 / M_PER_DEG_LAT, -122.3493, 45);
  return geoFold(&w->s, &w->split, &a, &b, now, bench, &w->net, NULL);
}

// ...the same, through the fold's guard (geoloc.h, GeoFoldGuard).
static int foldNeedleG(World* w, uint32_t now, const GeoFoldGuard* g) {
  const GeoHalf a = answer(47.6205, -122.3493, 40);
  const GeoHalf b = answer(47.6205 + 20.0 / M_PER_DEG_LAT, -122.3493, 45);
  return geoFold(&w->s, &w->split, &a, &b, now, false, &w->net, NULL, g);
}

static void testSchedule() {
  printf("when a look runs\n");
  const uint32_t T = 10000000;
  World w;
  worldInit(&w, T);
  CHECK(due(&w, T) == GEO_DUE_YES, "never looked, wanted, a fresh scan of four devices: look");

  // the gates, in order
  World g = w;
  g.in.setting = GEO_SET_OFF;
  g.in.busy = true;
  CHECK(due(&g, T) == GEO_DUE_OFF, "off beats everything");
  g.in.setting = 7;
  CHECK(due(&g, T) == GEO_DUE_OFF, "a garbled setting reads as off");
  g = w; g.in.busy = true; g.in.wifiUp = false;
  CHECK(due(&g, T) == GEO_DUE_BUSY, "a lookup out: busy");
  g = w; g.in.wifiUp = false; g.in.measuredLive = true;
  CHECK(due(&g, T) == GEO_DUE_NO_WIFI, "no WiFi");
  g = w; g.in.measuredLive = true;
  CHECK(due(&g, T) == GEO_DUE_MEASURED, "a live GPS or phone fix: not needed");
  g = w; g.viewing = false; g.in.viewedMs = T - GEO_WANT_MS;
  CHECK(due(&g, T) == GEO_DUE_UNWANTED, "Maps/Almanac last seen 10:00 ago: not wanted");
  g.in.viewedMs = T - GEO_WANT_MS + 1;
  CHECK(due(&g, T) == GEO_DUE_YES, "...9:59.999 ago: wanted");
  g.in.viewedMs = 0;
  g.in.beaconArmed = true;
  CHECK(due(&g, T) == GEO_DUE_UNWANTED, "a beacon armed with 'map only' does not want a Wi-Fi fix");
  g.in.setting = GEO_SET_MESH;
  CHECK(due(&g, T) == GEO_DUE_YES, "...with 'map + mesh' it does");
  g = w; g.in.netOk = false; g.in.musicPlaying = true;
  CHECK(due(&g, T) == GEO_DUE_NET_BUSY, "a download, a call, a game...: the weather's gates");
  g = w; g.in.musicPlaying = true;
  CHECK(due(&g, T) == GEO_DUE_MUSIC, "music playing: not automatically (review item 7)");
  geoAsk(&g.s, T);
  CHECK(due(&g, T) == GEO_DUE_YES, "`netloc now` may (it pauses the track)");
  g = w; g.viewing = false; g.in.viewedMs = 0; geoAsk(&g.s, T);
  CHECK(due(&g, T) == GEO_DUE_YES && geoAsked(&g.s, T + GEO_ASK_HOLD_MS - 1) &&
        !geoAsked(&g.s, T + GEO_ASK_HOLD_MS), "`netloc now` wants it on its own, for 3 minutes");

  // the snapshot
  g = w;
  CHECK(due(&g, T + GEO_SCAN_FRESH_MS) == GEO_DUE_SCAN, "a 2-minute-old snapshot: ask for a scan first");
  g.in.assoc = 2;
  CHECK(due(&g, T + 1000) == GEO_DUE_SCAN, "a snapshot of another association: ask for a scan first");
  g = w;
  std::vector<GeoAp> three;
  three.push_back(ap(1, -50)); three.push_back(ap(2, -60)); three.push_back(ap(3, -70));
  scanAt(&g, three, T);
  CHECK(due(&g, T) == GEO_DUE_DRY, "three devices: a dry look (nothing sent anywhere)");

  // a look: LOCATED
  printf("the fold\n");
  CHECK(foldNeedle(&w, T + 5000) == GEO_V_LOCATED, "both halves agree: located");
  CHECK(w.net.fixMs == T && w.net.firstMs == T && w.net.scanUtc == w.split.scanUtc && w.net.accM == 50 &&
        w.net.latI == (int32_t)llround((47.6205 + 47.6205 + 20.0 / M_PER_DEG_LAT) / 2 * 1e7) && !w.net.injected,
        "the slot is written as of the SCAN, at max(50, 40, 45, 20) = 50 m");
  CHECK(w.s.lastLookMs == T + 5000 && geoAttemptsLastHour(&w.s, T + 5000) == 1 && w.s.fixFromRef &&
        w.s.ref.devices == 4 && w.s.lastVerdict == GEO_V_LOCATED && w.s.lastAccM == 50 && !w.s.lastTransient,
        "the look is recorded: one attempt, the fix's devices kept");
  CHECK(due(&w, T + 6000) == GEO_DUE_SAME, "a live fix and nothing changed: no look");

  // the scans hold it
  printf("later scans\n");
  CHECK(scanAt(&w, place(), T + 61000) == GEO_SCAN_CONFIRMED && w.net.fixMs == T + 61000 &&
        w.net.firstMs == T && w.net.scanUtc == 1760000000u + (T + 61000) / 1000,
        "a scan 61 s on that still hears them: confirmed (its time is the fix's now)");
  CHECK(selfSlotState(&w.net, SELF_NET, T + 62000, SELF_PICK_TX) == SELF_ST_LIVE,
        "...two agreeing scans 61 s apart: sendable (with 'map + mesh')");
  CHECK(due(&w, T + 62000) == GEO_DUE_SAME && !w.s.changed, "confirmed: still nothing to look up");
  std::vector<GeoAp> moved;
  moved.push_back(ap(1, -80)); moved.push_back(ap(7, -50)); moved.push_back(ap(8, -55));
  moved.push_back(ap(9, -60)); moved.push_back(ap(10, -65));
  CHECK(scanAt(&w, moved, T + 300000) == GEO_SCAN_MOVED && w.net.fixMs == 0 && !w.s.fixFromRef && w.s.changed,
        "a scan that hears one of four: MOVED - the fix is dropped at once (review item 2)");
  CHECK(due(&w, T + 300000) == GEO_DUE_TOO_SOON, "changed, but under 10 minutes since the last look");
  CHECK(due(&w, T + 5000 + GEO_LOOK_GAP_MS - 1) == GEO_DUE_TOO_SOON, "9:59.999: too soon");
  CHECK(due(&w, T + 5000 + GEO_LOOK_GAP_MS) == GEO_DUE_SCAN, "10:00 on, the snapshot is stale: ask for a scan");
  const uint32_t T2 = T + 5000 + GEO_LOOK_GAP_MS;
  scanAt(&w, moved, T2);
  CHECK(due(&w, T2) == GEO_DUE_YES, "a fresh scan of the new place: look");

  // a failed look with no fix in hand: its devices become the reference
  const GeoHalf nf = answer(47.6205, -122.3493, 40, 404);
  CHECK(geoFold(&w.s, &w.split, &nf, NULL, T2, false, &w.net, NULL) == GEO_V_NOT_FOUND && w.net.fixMs == 0 &&
        !w.s.fixFromRef && w.s.ref.devices == 5 && geoAttemptsLastHour(&w.s, T2) == 2 && !w.s.lastTransient,
        "not found: no fix, the new place's devices are the yardstick, the attempt counted");
  scanAt(&w, moved, T2 + GEO_LOOK_GAP_MS + 1);
  CHECK(due(&w, T2 + GEO_LOOK_GAP_MS + 1) == GEO_DUE_SAME,
        "the same air 10 minutes later: beaconDB would say the same - no look");

  // transient failures retry; the hour's cap
  printf("retries and the hour's cap\n");
  World r;
  worldInit(&r, T);
  GeoHalf na = reply(GEO_R_NO_ANSWER);
  CHECK(geoFold(&r.s, &r.split, &na, NULL, T, false, &r.net, NULL) == GEO_V_UNREACHABLE && r.s.lastTransient &&
        geoAttemptsLastHour(&r.s, T) == 1, "never heard beaconDB: counted (it may have), and retried");
  scanAt(&r, place(), T + GEO_LOOK_GAP_MS);
  CHECK(due(&r, T + GEO_LOOK_GAP_MS) == GEO_DUE_YES, "...10 minutes later, though nothing changed");
  geoFold(&r.s, &r.split, &na, NULL, T + GEO_LOOK_GAP_MS, false, &r.net, NULL);
  scanAt(&r, place(), T + 2 * GEO_LOOK_GAP_MS);
  geoFold(&r.s, &r.split, &na, NULL, T + 2 * GEO_LOOK_GAP_MS, false, &r.net, NULL);
  scanAt(&r, place(), T + 3 * GEO_LOOK_GAP_MS);
  CHECK(geoAttemptsLastHour(&r.s, T + 3 * GEO_LOOK_GAP_MS) == 3 && due(&r, T + 3 * GEO_LOOK_GAP_MS) == GEO_DUE_HOUR_CAP,
        "three attempts in the hour: capped (an attempt is up to two lookups)");
  CHECK(due(&r, T + 3 * GEO_LOOK_GAP_MS + GEO_SCAN_FRESH_MS) == GEO_DUE_HOUR_CAP,
        "capped: no scan is spent on a look that cannot run");
  std::vector<GeoAp> three2;
  three2.push_back(ap(1, -50)); three2.push_back(ap(2, -60)); three2.push_back(ap(3, -70));
  World d = r;
  scanAt(&d, three2, T + 3 * GEO_LOOK_GAP_MS);
  CHECK(due(&d, T + 3 * GEO_LOOK_GAP_MS) == GEO_DUE_DRY, "...but a dry look costs nobody anything: allowed");
  scanAt(&r, place(), T + GEO_HOUR_MS);
  CHECK(geoAttemptsLastHour(&r.s, T + GEO_HOUR_MS) == 2 && due(&r, T + GEO_HOUR_MS) == GEO_DUE_YES,
        "the first attempt an hour old: room again");
  GeoHalf nm = reply(GEO_R_NO_MEMORY);
  World m;
  worldInit(&m, T);
  CHECK(geoFold(&m.s, &m.split, &nm, NULL, T, false, &m.net, NULL) == GEO_V_NO_MEMORY && m.s.lastTransient &&
        geoAttemptsLastHour(&m.s, T) == 0, "no RAM for the session: retried, NOT counted (beaconDB never saw it)");

  // dry looks
  World dr;
  worldInit(&dr, T);
  scanAt(&dr, three2, T);
  CHECK(geoFold(&dr.s, &dr.split, NULL, NULL, T, false, &dr.net, NULL) == GEO_V_TOO_FEW &&
        geoAttemptsLastHour(&dr.s, T) == 0 && dr.s.lastLookMs == T && dr.s.lastDevices == 3 && !dr.s.lastTransient,
        "a dry look: too few, recorded, not an attempt");
  scanAt(&dr, three2, T + GEO_LOOK_GAP_MS);
  CHECK(due(&dr, T + GEO_LOOK_GAP_MS) == GEO_DUE_SAME, "the same three, later: nothing to do");
  scanAt(&dr, place(), T + GEO_LOOK_GAP_MS + 1000);
  CHECK(dr.s.changed && due(&dr, T + GEO_LOOK_GAP_MS + 1000) == GEO_DUE_YES,
        "now four devices are heard: worth a real look");

  // asked: the 2-minute gap
  World a;
  worldInit(&a, T);
  foldNeedle(&a, T);
  geoAsk(&a.s, T + 60000);
  CHECK(due(&a, T + 60000) == GEO_DUE_TOO_SOON, "`netloc now` a minute after a look: too soon");
  CHECK(due(&a, T + GEO_LOOK_GAP_ASKED_MS) == GEO_DUE_SCAN, "two minutes after: due (needs a scan)");
  scanAt(&a, place(), T + GEO_LOOK_GAP_ASKED_MS);
  CHECK(due(&a, T + GEO_LOOK_GAP_ASKED_MS) == GEO_DUE_YES, "...and with one: look, a live fix or not");
  const GeoHalf dis1 = answer(47.6205, -122.3493, 40);
  const GeoHalf dis2 = answer(47.6205 + 500.0 / M_PER_DEG_LAT, -122.3493, 40);
  const uint32_t refN = a.s.ref.n;
  CHECK(geoFold(&a.s, &a.split, &dis1, &dis2, T + GEO_LOOK_GAP_ASKED_MS, false, &a.net, NULL) == GEO_V_DISAGREE &&
        a.net.fixMs != 0 && a.s.fixFromRef && a.s.ref.n == refN && a.s.askMs == 0,
        "the asked look disagrees: the live fix and its yardstick stay; the ask is spent");

  // the fix expires: a reason to look
  World e;
  worldInit(&e, T);
  foldNeedle(&e, T);
  CHECK(due(&e, T + SELF_NET_SHOW_MS - 1) == GEO_DUE_SAME, "a fix not yet expired: nothing");
  CHECK(due(&e, T + SELF_NET_SHOW_MS) == GEO_DUE_SCAN, "12 minutes with no confirming scan: gone - look again");
  CHECK(scanAt(&e, place(), T + SELF_NET_SHOW_MS + 1000) == GEO_SCAN_NO_FIX && e.net.fixMs == 0 &&
        selfSlotState(&e.net, SELF_NET, T + SELF_NET_SHOW_MS + 1000, 0) == SELF_ST_NEVER && e.s.fixFromRef,
        "a scan after expiry does not bring the fix back: it CLEARS it (expired is gone)");
  CHECK(due(&e, T + SELF_NET_SHOW_MS + 1000) == GEO_DUE_YES, "...the look replaces it");

  // a new association
  World n;
  worldInit(&n, T);
  foldNeedle(&n, T);
  n.in.assoc = 2;
  CHECK(due(&n, T + GEO_LOOK_GAP_MS) == GEO_DUE_SAME, "a new association with a live fix: nothing (its scans judge it)");
  selfSlotClear(&n.net);
  n.s.fixFromRef = false;
  CHECK(due(&n, T + GEO_LOOK_GAP_MS) == GEO_DUE_SCAN, "a new association, no fix, no scan of it yet: look at the air");
  scanAt(&n, place(), T + GEO_LOOK_GAP_MS, 2);
  CHECK(due(&n, T + GEO_LOOK_GAP_MS) == GEO_DUE_SAME,
        "...the same air: nothing (phone 1's rejoins must not each cost two lookups)");

  // the slot switched off mid-lookup
  World o;
  worldInit(&o, T);
  o.net.enabled = false;
  CHECK(foldNeedle(&o, T) == GEO_V_LOCATED && o.net.fixMs == 0 && !o.s.fixFromRef,
        "located, but Wi-Fi location was turned off meanwhile: nothing kept");

  // VAGUE remembers how vague
  World v;
  worldInit(&v, T);
  const GeoHalf va = answer(47.6205, -122.3493, 40), vb = answer(47.6205, -122.3493, 350);
  CHECK(geoFold(&v.s, &v.split, &va, &vb, T, false, &v.net, NULL) == GEO_V_VAGUE && v.s.lastAccM == 350,
        "too vague: the worse half's accuracy is kept for the row");

  // the bench
  printf("the bench (`netloc answer`)\n");
  World b;
  worldInit(&b, T);
  CHECK(foldNeedle(&b, T + 1000, true) == GEO_V_LOCATED && b.net.injected && b.net.fixMs == T &&
        b.s.lastLookMs == 0 && geoAttemptsLastHour(&b.s, T + 1000) == 0 && b.s.lastVerdict == GEO_V_LOCATED,
        "canned halves: an INJECTED fix (never sent by the schedule); the schedule untouched");
  geoAsk(&b.s, T + 1000);
  const GeoHalf bn = answer(47.6205, -122.3493, 40, 404);
  geoFold(&b.s, &b.split, &bn, NULL, T + 2000, true, &b.net, NULL);
  CHECK(b.s.askMs != 0 && !b.s.lastTransient && b.s.lastVerdict == GEO_V_NOT_FOUND,
        "a canned failure leaves a pending `netloc now` pending");

  // the scan request gap
  GeoSched q;
  geoSchedInit(&q);
  CHECK(geoScanRequestOk(&q, 5) && !geoScanRequestOk(NULL, 5), "never asked: a scan may be asked for");
  geoScanRequested(&q, T);
  CHECK(!geoScanRequestOk(&q, T + GEO_SCAN_REQ_GAP_MS - 1) && geoScanRequestOk(&q, T + GEO_SCAN_REQ_GAP_MS),
        "at most one scan asked for every 2 minutes (each scan's allocation has rebooted phone 1)");
  bool dn = true;
  for (int x = GEO_DUE_YES; x <= GEO_DUE_HOUR_CAP; x++) {
    dn = dn && strcmp(geoDueName(x), "?") && !strchr(geoDueName(x), ' ');
  }
  CHECK(dn, "every due reason has a log token");
}

// ---------------------------------------------------------------- what two halves cannot see

// A GPS or phone slot with a fix (sats/HDOP as given; -1 = not said).
static SelfSlot gnss(double lat, double lon, uint32_t ms, int sats = 9, int hdopX10 = 12, bool inj = false) {
  SelfSlot s;
  memset(&s, 0, sizeof(s));
  selfSlotClear(&s);
  s.enabled = true;
  selfSlotSet(&s, (int32_t)llround(lat * 1e7), (int32_t)llround(lon * 1e7), sats, hdopX10, ms, inj);
  return s;
}

static void testGuards() {
  printf("the jump backstop (review item 1)\n");
  const uint32_t T = 30000000;
  SelfSlot slots[SELF_SRC_COUNT];
  for (int i = 0; i < SELF_SRC_COUNT; i++) { memset(&slots[i], 0, sizeof(slots[i])); selfSlotClear(&slots[i]); }
  GeoAnchor an;
  geoAnchorFrom(slots, T, false, &an);
  CHECK(!an.ok, "no GPS or phone fix this boot: no anchor (the Wi-Fi answer stands on its halves)");
  slots[SELF_GPS - 1] = gnss(40.0, -100.0, T - 300000);
  slots[SELF_PHONE - 1] = gnss(47.6205, -122.3493, T - 60000);
  geoAnchorFrom(slots, T, false, &an);
  CHECK(an.ok && an.ms == T - 60000 && an.latI == (int32_t)llround(47.6205 * 1e7), "the NEWEST of GPS and phone");
  slots[SELF_PHONE - 1] = gnss(47.6205, -122.3493, T - 60000, 3, 64);
  geoAnchorFrom(slots, T, false, &an);
  CHECK(an.ok && an.ms == T - 300000, "a 3-satellite fix is no anchor (it can be 20 km off): the GPS one");
  slots[SELF_PHONE - 1] = gnss(47.6205, -122.3493, T - 60000, 9, 12, true);
  geoAnchorFrom(slots, T, false, &an);
  CHECK(an.ok && an.ms == T - 300000, "a bench fix is no anchor for a real look...");
  geoAnchorFrom(slots, T, true, &an);
  CHECK(an.ok && an.ms == T - 60000, "...but is for a bench fold (`pos inject` + `netloc answer`)");
  geoAnchorFrom(slots, T - 300000 + GEO_JUMP_WINDOW_MS, false, &an);
  CHECK(!an.ok, "6 hours on, the phone may have gone anywhere: no anchor");
  selfSlotClear(&slots[SELF_PHONE - 1]);
  selfSlotClear(&slots[SELF_GPS - 1]);

  GeoAnchor needle;
  needle.ok = true;
  needle.latI = (int32_t)llround(47.6205 * 1e7);
  needle.lonI = (int32_t)llround(-122.3493 * 1e7);
  needle.ms = T;
  const int32_t farLat = (int32_t)llround(40.0 * 1e7), farLon = (int32_t)llround(-100.0 * 1e7);
  const int32_t pikeLat = (int32_t)llround(47.6097 * 1e7), pikeLon = (int32_t)llround(-122.3422 * 1e7);
  CHECK(!geoJumpOk(&needle, farLat, farLon, 50, T + 300000),
        "~2,000 km five minutes after a GPS fix: refused (it would take 6,700 m/s)");
  CHECK(geoJumpOk(&needle, pikeLat, pikeLon, 50, T + 300000), "1.3 km five minutes on: a walk, fine");
  CHECK(!geoJumpOk(&needle, pikeLat, pikeLon, 50, T + 2000) && !geoJumpOk(&needle, pikeLat, pikeLon, 50, T - 2000),
        "1.3 km two seconds from a GPS fix, either side of it: refused");
  CHECK(geoJumpOk(&needle, needle.latI, needle.lonI + 500, 50, T), "the same place at the same moment: fine");
  CHECK(geoJumpOk(NULL, farLat, farLon, 50, T) && geoJumpOk(&an, farLat, farLon, 50, T), "no anchor: nothing to hold it to");
  needle.ms = 0xFFFFF000u;
  CHECK(geoJumpOk(&needle, pikeLat, pikeLon, 50, 300000u - 4096u) &&
        !geoJumpOk(&needle, farLat, farLon, 50, 300000u - 4096u), "across the millis() wrap: five minutes, not 49 days");
  needle.ms = T;

  // Through the fold: routers that moved together agree perfectly, at the old home.
  World w;
  worldInit(&w, T);
  GeoFoldGuard g;
  g.anchor = &needle;
  g.latest = NULL;
  const GeoHalf fa = answer(40.0, -100.0, 40), fb = answer(40.0 + 20.0 / M_PER_DEG_LAT, -100.0, 45);
  GeoFix fx;
  CHECK(geoFold(&w.s, &w.split, &fa, &fb, T + 300000, false, &w.net, &fx, &g) == GEO_V_JUMP &&
        w.net.fixMs == 0 && !w.s.fixFromRef && w.s.ref.devices == 4 && !w.s.lastTransient &&
        geoAttemptsLastHour(&w.s, T + 300000) == 1 && w.s.lastVerdict == GEO_V_JUMP,
        "two agreeing halves far from a GPS fix of 5 min ago: refused, nothing written, recorded");
  scanAt(&w, place(), T + 300000 + GEO_LOOK_GAP_MS);
  CHECK(due(&w, T + 300000 + GEO_LOOK_GAP_MS) == GEO_DUE_SAME,
        "...and the same air later is not asked about again (it would say the same)");
  World w2;
  worldInit(&w2, T);
  needle.ms = T - 600000;
  CHECK(foldNeedleG(&w2, T + 5000, &g) == GEO_V_LOCATED && w2.net.fixMs == T,
        "an answer where the last GPS fix was: located");
  GeoAnchor farA;
  farA.ok = true;
  farA.latI = farLat;
  farA.lonI = farLon;
  farA.ms = T;
  World w3;
  worldInit(&w3, T);
  const GeoHalf na = answer(47.6205, -122.3493, 40), nb = answer(47.6205 + 20.0 / M_PER_DEG_LAT, -122.3493, 45);
  CHECK(geoBenchFold(&w3.s, &w3.split, &na, &nb, T + 1000, 0, 1, &w3.net, NULL, &farA) == GEO_V_JUMP &&
        w3.net.fixMs == 0, "the bench fold is held to it too");
  CHECK(geoBenchFold(&w3.s, &w3.split, &na, &nb, T + 1000, 0, 1, &w3.net, NULL) == GEO_V_LOCATED,
        "...and without an anchor, as before");

  printf("a scan that lands while the look is out\n");
  std::vector<GeoAp> moved;
  moved.push_back(ap(1, -80)); moved.push_back(ap(7, -50)); moved.push_back(ap(8, -55));
  moved.push_back(ap(9, -60)); moved.push_back(ap(10, -65));
  World m;
  worldInit(&m, T);
  const GeoSplit job = m.split;              // the look's question: the scan at T
  scanAt(&m, moved, T + 30000);              // the switcher's scan lands at +30 s, far from it
  g.anchor = NULL;
  g.latest = &m.scan;
  CHECK(geoFold(&m.s, &job, &na, &nb, T + 45000, false, &m.net, NULL, &g) == GEO_V_MOVED && m.net.fixMs == 0 &&
        m.s.changed && !m.s.fixFromRef && m.s.ref.devices == 4,
        "a newer scan no longer hears the look's devices: MOVED, nothing written, due again");
  CHECK(due(&m, T + 45000 + GEO_LOOK_GAP_MS) == GEO_DUE_SCAN, "...a look again after the gap");
  World k;
  worldInit(&k, T);
  const GeoSplit job2 = k.split;
  scanAt(&k, place(), T + 30000, 7);         // the same air, on another association
  g.latest = &k.scan;
  CHECK(geoFold(&k.s, &job2, &na, &nb, T + 45000, false, &k.net, NULL, &g) == GEO_V_LOCATED &&
        k.net.firstMs == T && k.net.fixMs == T + 30000 && k.s.fixAssoc == 7 && !k.s.changed,
        "a newer scan that still hears them: located AND confirmed by it (its time, its association)");
  World same;
  worldInit(&same, T);
  g.latest = &same.scan;                     // the look's own scan: nothing newer
  CHECK(geoFold(&same.s, &same.split, &na, &nb, T + 5000, false, &same.net, NULL, &g) == GEO_V_LOCATED &&
        same.net.fixMs == T && same.s.fixAssoc == 1, "the look's own scan is not 'newer'");

  printf("the link: a fix held when the WiFi drops or changes\n");
  World h;
  worldInit(&h, T);
  h.net.noTx = true;                         // "map only" when the fix is held...
  foldNeedle(&h, T);
  CHECK(!geoOnLink(&h.s, &h.net, 1) && h.net.fixMs == T && h.s.fixAssoc == 1, "the same association: nothing");
  CHECK(geoOnLink(&h.s, &h.net, 2) && h.net.fixMs == 0 && h.s.held && h.s.fixFromRef && !netLive(&h, T + 1000),
        "the link dropped (a new stamp): the fix is HELD - off the map, the Almanac and the air at once");
  CHECK(!geoOnLink(&h.s, &h.net, 3), "held once is held: nothing more to hide");
  GeoWhenIn in = h.in;
  in.netLive = false;
  in.viewedMs = T + 1000;
  CHECK(geoRefreshWanted(&h.s, &in, &h.net, T + 1000), "held, and the map is open: ask for the scan that judges it");
  in.viewedMs = 0;
  CHECK(!geoRefreshWanted(&h.s, &in, &h.net, T + 1000), "held, nothing wants a position: no scan asked");
  h.net.noTx = false;                        // "map + mesh" chosen meanwhile
  CHECK(scanAt(&h, place(), T + 40000, 2) == GEO_SCAN_BACK && h.net.fixMs == T + 40000 && h.net.firstMs == T &&
        h.net.accM == 50 && !h.s.held && h.s.fixAssoc == 2 && !h.net.noTx && netLive(&h, T + 40000),
        "a scan that still hears them: BACK, as of that scan, on the new association, with today's setting");
  CHECK(geoOnLink(&h.s, &h.net, 3) && scanAt(&h, moved, T + 80000, 3) == GEO_SCAN_MOVED && h.net.fixMs == 0 &&
        !h.s.held && !h.s.fixFromRef, "held, then a scan of other air: gone");
  World x;
  worldInit(&x, T);
  foldNeedle(&x, T);
  geoOnLink(&x.s, &x.net, 2);
  CHECK(!geoExpire(&x.s, &x.net, T + SELF_NET_SHOW_MS) && !x.s.held,
        "held past SELF_NET_SHOW_MS: forgotten (nothing was showing, nothing to redraw)");
  CHECK(scanAt(&x, place(), T + SELF_NET_SHOW_MS + 1000, 2) == GEO_SCAN_NO_FIX && x.net.fixMs == 0,
        "...so a scan that hears them does not bring an expired fix back");
  x.in.assoc = 2;
  CHECK(due(&x, T + SELF_NET_SHOW_MS + 1000) == GEO_DUE_YES, "...the expiry is a reason to look");
  World y;
  worldInit(&y, T);
  foldNeedle(&y, T);
  geoOnLink(&y.s, &y.net, 2);
  CHECK(foldNeedle(&y, T + GEO_LOOK_GAP_MS) == GEO_V_LOCATED && !y.s.held && y.net.fixMs == T,
        "a new fix replaces a held one");
  World z;
  worldInit(&z, T);
  CHECK(!geoOnLink(&z.s, &z.net, 9), "no fix: nothing to hold");
  std::vector<GeoAp> two;
  two.push_back(ap(1, -50)); two.push_back(ap(2, -60));
  scanAt(&z, two, T);
  geoBenchFold(&z.s, &z.split, &na, &nb, T, 0, 1, &z.net, NULL);
  CHECK(z.net.fixMs != 0 && !z.s.fixFromRef && !geoOnLink(&z.s, &z.net, 9),
        "a stand-in bench fix stands on no set: not held (it simply expires)");

  printf("expiry clears the slot\n");
  World e;
  worldInit(&e, T);
  foldNeedle(&e, T);
  CHECK(!geoExpire(&e.s, &e.net, T + SELF_NET_SHOW_MS - 1) && e.net.fixMs == T, "11:59.999 after its scan: kept");
  CHECK(geoExpire(&e.s, &e.net, T + SELF_NET_SHOW_MS) && e.net.fixMs == 0 && e.s.fixFromRef,
        "12:00: CLEARED (and still a reason to look)");
  CHECK(selfSlotState(&e.net, SELF_NET, (uint32_t)(T + 1000u), SELF_PICK_INJECTED) == SELF_ST_NEVER,
        "...so 49.7 days on, when millis() wraps, no weeks-old fix reads as fresh again");
  CHECK(!geoExpire(&e.s, &e.net, T + SELF_NET_SHOW_MS + 5) && !geoExpire(NULL, NULL, T), "nothing to clear: false");

  printf("when a scan is asked for\n");
  World r;
  worldInit(&r, T);
  foldNeedle(&r, T);
  GeoWhenIn ri = r.in;
  ri.netLive = true;
  ri.viewedMs = T + 60000;
  CHECK(geoRefreshWanted(&r.s, &ri, &r.net, T + 60000),
        "a live fix while Maps is open: re-checked by a scan (the glue spaces them 2 min apart)");
  ri.measuredLive = true;
  CHECK(!geoRefreshWanted(&r.s, &ri, &r.net, T + 60000), "...not while a GPS/phone fix is what the map shows");
  ri.measuredLive = false;
  ri.busy = true;
  CHECK(!geoRefreshWanted(&r.s, &ri, &r.net, T + 60000), "...not while a look is out");
  ri.busy = false;
  ri.wifiUp = false;
  CHECK(!geoRefreshWanted(&r.s, &ri, &r.net, T + 60000), "...not with WiFi down");
  ri.wifiUp = true;
  ri.setting = GEO_SET_OFF;
  CHECK(!geoRefreshWanted(&r.s, &ri, &r.net, T + 60000), "...never with the setting off");
  ri.setting = GEO_SET_MESH;
  ri.viewedMs = 0;
  ri.beaconArmed = true;
  CHECK(!geoRefreshWanted(&r.s, &ri, &r.net, T + 60000) &&
        geoRefreshWanted(&r.s, &ri, &r.net, T + SELF_NET_REFRESH_AT_MS),
        "a beacon armed, nothing on screen: only when the switcher's own scan is late");
  ri.beaconArmed = false;
  CHECK(!geoRefreshWanted(&r.s, &ri, &r.net, T + SELF_NET_REFRESH_AT_MS), "nothing wants a position: no scan");

  printf("switched off while a look was out\n");
  GeoSched c;
  geoSchedInit(&c);
  const GeoHalf sent = answer(47.6205, -122.3493, 40), skipped = reply(GEO_R_NOT_ASKED);
  geoFoldCancelled(&c, &sent, &skipped, T);
  CHECK(geoAttemptsLastHour(&c, T) == 1 && c.lastLookMs == T && c.ref.n == 0 && !c.fixFromRef &&
        c.lastVerdict == GEO_V_NONE, "half A went out: the cap and the gap count it; nothing else is kept");
  GeoSched c2;
  geoSchedInit(&c2);
  geoFoldCancelled(&c2, &skipped, &skipped, T);
  CHECK(geoAttemptsLastHour(&c2, T) == 0 && c2.lastLookMs == 0, "nothing went out: nothing counted");
  geoFoldCancelled(NULL, &sent, &sent, T);
}

// ---------------------------------------------------------------- end to end

static void testEndToEnd() {
  printf("end to end: scan, halves, fold, confirm, send\n");
  const uint32_t T = 20000000;
  SelfSlot slots[SELF_SRC_COUNT];
  for (int i = 0; i < SELF_SRC_COUNT; i++) { memset(&slots[i], 0, sizeof(slots[i])); selfSlotClear(&slots[i]); }
  SelfSlot* net = &slots[SELF_NET - 1];
  net->enabled = true;
  net->noTx = false;                                          // "map + mesh"
  GeoSched s;
  geoSchedInit(&s);
  GeoScan sc;
  std::vector<GeoAp> v = place();
  v.push_back(ap(5, -75));
  v.push_back(ap(6, -84));
  scanOf(&sc, v, T, 1760000000u, 3);
  GeoSplit sp;
  geoSplitScan(&sc, NULL, &sp);
  geoOnScan(&s, &sc, sp.devices, net);
  char a[GEO_REQUEST_CAP], b[GEO_REQUEST_CAP];
  CHECK(sp.devices == 6 && geoBuildHalf(&sp, GEO_HALF_A, a, sizeof(a)) == 4 &&
        geoBuildHalf(&sp, GEO_HALF_B, b, sizeof(b)) == 3, "six devices: half A four addresses, half B three");
  GeoHalf ha, hb;
  const char* ra = "{\"location\":{\"lat\":47.62051,\"lng\":-122.34932},\"accuracy\":44.0}";
  const char* rb = "{\"location\":{\"lat\":47.62032,\"lng\":-122.34921},\"accuracy\":61.5}";
  geoHalfFrom(&ha, GEO_R_ANSWERED, 200, ra, strlen(ra));
  CHECK(geoSecondHalfNeeded(&ha), "half A good: ask half B");
  geoHalfFrom(&hb, GEO_R_ANSWERED, 200, rb, strlen(rb));
  GeoFix fx;
  CHECK(geoFold(&s, &sp, &ha, &hb, T + 9000, false, net, &fx) == GEO_V_LOCATED && fx.accM == 62 &&
        net->accM == 62 && net->firstMs == T, "both agree (~22 m apart, 44/62 m): located at 62 m, as of the scan");
  SelfFix pick;
  CHECK(selfPick(slots, T + 9000, 0, &pick) == SELF_NET && pick.accM == 62, "the map shows it: me +/-62 m Wi-Fi");
  CHECK(selfPick(slots, T + 9000, SELF_PICK_TX, &pick) == SELF_NONE &&
        selfNewest(slots, T + 9000, SELF_PICK_TX, &pick) == SELF_NET && pick.state == SELF_ST_SETTLING,
        "the beacon waits: one scan is not enough");
  CHECK(selfNetScanUseful(net, T + 61000) && !selfNetScanUseful(net, T + 30000),
        "an owed slot asks for a scan once a second one could settle it");
  scanOf(&sc, v, T + 70000, 1760000070u, 3);
  geoSplitScan(&sc, NULL, &sp);
  CHECK(geoOnScan(&s, &sc, sp.devices, net) == GEO_SCAN_CONFIRMED, "the scan 70 s on confirms it");
  CHECK(selfPick(slots, T + 75000, SELF_PICK_TX, &pick) == SELF_NET && pick.locSrc == MESH_LOC_UNSET &&
        pick.precBits == 17 && selfFixSendTime(&pick, 1760000075u) == 1760000070u,
        "sendable: UNSET, 17 bits, stamped with the confirming scan's UTC");
  SelfNetTxMem mem;
  selfNetTxInit(&mem);
  int32_t cl = 0, co = 0;
  CHECK(!selfNetPreciseWins(&mem, &pick, T + 75000), "no precise beacon of late: the Wi-Fi fix goes");
  selfNetTxCell(&mem, &pick, &cl, &co);
  uint8_t pl[MESH_POS_BUILD_EX_MAX];
  const size_t n = meshPosBuildEx(pl, sizeof(pl), cl, co, selfFixSendTime(&pick, 1760000075u), pick.locSrc, pick.precBits);
  MeshPosMsg msg;
  CHECK(n == 18 && meshPosParseEx(pl, n, &msg) && msg.precBits == 17 && msg.locSrc == 0 &&
        msg.time == 1760000070u && msg.latI == meshPosTruncate(pick.latI, 17),
        "the beacon: 18 bytes, a 17-bit cell, no location_source, the scan's time");
  CHECK(meshPosDistanceM(msg.latI, msg.lonI, pick.latI, pick.lonI) < meshPosPrecisionRadiusM(17) * 1.5,
        "the cell's centre is within its own drawn ring (x1.5 for the corner) of the fix");
  CHECK(meshPosDistanceM(msg.latI, msg.lonI, (int32_t)llround(47.6205 * 1e7), (int32_t)llround(-122.3493 * 1e7)) < 400.0,
        "...and near the landmark the canned halves described");
  // a drive: the next scan hears none of it
  std::vector<GeoAp> road;
  road.push_back(ap(20, -60)); road.push_back(ap(21, -70));
  scanOf(&sc, road, T + 200000, 1760000200u, 3);
  geoSplitScan(&sc, NULL, &sp);
  CHECK(geoOnScan(&s, &sc, sp.devices, net) == GEO_SCAN_MOVED && selfPick(slots, T + 200000, 0, &pick) == SELF_NONE,
        "driving away: the next scan drops it, from the map and the air alike");
}

// ---------------------------------------------------------------- the switcher, the setting, the words

static void testSwitcher() {
  printf("the auto-switcher's scan\n");
  bool neverFullWhenOff = true, neverScanDisconnected = true, gates = true, unchanged = true;
  for (int i = 0; i < 64; i++) {
    const bool sa = i & 1, rc = i & 2, as = i & 4, gw = i & 8, cn = i & 16, ph = i & 32;
    const int m = geoSwitcherMode(sa, rc, as, gw, cn, ph);
    if (!as && m == GEO_SW_FULL) neverFullWhenOff = false;
    if (!cn && m == GEO_SW_SCAN_ONLY) neverScanDisconnected = false;
    if ((!sa || !rc) && m != GEO_SW_NONE) gates = false;
    if (sa && rc && as && !(cn && ph) && m != GEO_SW_FULL) unchanged = false;
  }
  CHECK(neverFullWhenOff, "auto-switch off: never the full switcher (never its disconnect machinery)");
  CHECK(neverScanDisconnected, "never a geo scan while disconnected (review item 8)");
  CHECK(gates, "station not allowed or reconnect off: nothing at all");
  CHECK(unchanged, "auto-switch on, no live phone stream: the switcher exactly as before");
  CHECK(geoSwitcherMode(true, true, true, false, true, true) == GEO_SW_SCAN_ONLY,
        "a live phone stream on this hotspot: scan, but never hop away from it");
  CHECK(geoSwitcherMode(true, true, false, true, true, false) == GEO_SW_SCAN_ONLY &&
        geoSwitcherMode(true, true, false, false, true, false) == GEO_SW_NONE,
        "auto-switch off: a scan only when Wi-Fi location asked for one");
}

// ── the phone's font, as SmoothFont measures it (test_tzauto.cpp's reader) ──────────────────
struct Font7 {
  int      n;
  uint16_t uni[256];
  uint8_t  w[256], adv[256];
  int8_t   dx[256];
  int      space;
  bool     ok;
};

static uint32_t runDecode(const unsigned char** p) {
  uint32_t v = 0;
  unsigned char more = 0;
  do {
    v <<= 7;
    v |= (**p) & 0x7F;
    more = (**p) & 0x80;
    (*p)++;
  } while (more);
  return v;
}

static void fontLoad(Font7* f, const unsigned char* data) {
  memset(f, 0, sizeof(*f));
  if (data[0] != '7' || data[1] != 'S' || data[2] != 'F') return;
  const unsigned char* p = data + 3;
  f->n = (int)runDecode(&p);
  runDecode(&p);
  const int ascent = (int)runDecode(&p);
  const int descent = (int)runDecode(&p);
  if (f->n <= 0 || f->n > 256) return;
  for (int g = 0; g < f->n; g++) {
    f->uni[g] = (uint16_t)(p[0] | (p[1] << 8));
    p += 4;
    runDecode(&p);
    f->w[g] = (uint8_t)runDecode(&p);
    f->adv[g] = (uint8_t)runDecode(&p);
    runDecode(&p);
    f->dx[g] = (int8_t)runDecode(&p);
  }
  f->space = (ascent + descent) * 2 / 7;
  f->ok = true;
}

static int textWidth(const Font7* f, const char* s, bool* missing) {
  int w = 0;
  for (const char* p = s; *p; p++) {
    const uint16_t u = (unsigned char)*p;
    if (u == 0x20) { w += f->space; continue; }
    int g = -1;
    for (int i = 0; i < f->n; i++) {
      if (f->uni[i] == u) { g = i; break; }
    }
    if (g >= 0) {
      if (w == 0 && f->dx[g] < 0) w -= f->dx[g];
      w += p[1] ? f->adv[g] : (f->dx[g] + f->w[g]);
    } else {
      w += f->space + 1;
      if (missing) *missing = true;
    }
  }
  return w;
}

/* A My node subtitle: Akrobat Bold 16 in the menu row's 240 - 8 px (GUI.cpp MenuOption). */
#define ROW_W 232
static Font7 g_f16;
static int g_widest = 0;
static char g_widestText[96];

static bool rowFits(const char* s) {
  bool missing = false;
  const int w = textWidth(&g_f16, s, &missing);
  if (w > g_widest) {
    g_widest = w;
    snprintf(g_widestText, sizeof(g_widestText), "%s", s);
  }
  if (missing || w > ROW_W) {
    printf("    \"%s\": %d px%s\n", s, w, missing ? ", a glyph missing" : "");
    return false;
  }
  return true;
}

static void testSettingAndWords() {
  printf("the setting and the words\n");
  fontLoad(&g_f16, Akrobat_Bold16);
  CHECK(g_f16.ok && g_f16.n > 90, "the firmware's Akrobat_Bold16 glyph table loads");
  CHECK(!strcmp(geoSettingName(GEO_SET_OFF), "off") && !strcmp(geoSettingName(GEO_SET_MAP), "map only") &&
        !strcmp(geoSettingName(GEO_SET_MESH), "map + mesh") && !strcmp(geoSettingName(9), "off"), "names");
  CHECK(geoSettingParse("off") == 0 && geoSettingParse("map") == 1 && geoSettingParse("mesh") == 2 &&
        geoSettingParse("MESH") == -1 && geoSettingParse("") == -1 && geoSettingParse(NULL) == -1, "parse");
  CHECK(geoSettingSerialOk(GEO_SET_OFF) && geoSettingSerialOk(GEO_SET_MAP) && !geoSettingSerialOk(GEO_SET_MESH) &&
        !geoSettingSerialOk(5), "the cable may choose off or map, never mesh (the consent rule)");
  CHECK(geoSettingNext(0) == 1 && geoSettingNext(1) == 2 && geoSettingNext(2) == 0 && geoSettingNext(-3) == 1,
        "the row's tap: off -> map only -> map + mesh -> off");
  CHECK(geoSettingSane(2) == 2 && geoSettingSane(3) == 0 && geoSettingSane(-1) == 0, "NVS garbage reads as off");

  char t[96];
  GeoSched s;
  geoSchedInit(&s);
  geoRowText(t, sizeof(t), GEO_SET_OFF, &s, NULL, GEO_DUE_OFF);
  CHECK(has(t, "beaconDB") && has(t, "Wi-Fi IDs") && printable(t), "off: the row says what turning it on sends, and to whom");
  SelfFix f;
  memset(&f, 0, sizeof(f));
  f.src = SELF_NET;
  f.state = SELF_ST_LIVE;
  f.accM = 60;
  f.ageMs = 30000;
  geoRowText(t, sizeof(t), GEO_SET_MAP, &s, &f, GEO_DUE_SAME);
  CHECK(!strcmp(t, "+/-60 m, just now, map only"), "a live fix: its size and age, 'map only'");
  f.ageMs = 3 * 60000 + 5;
  f.injected = true;
  geoRowText(t, sizeof(t), GEO_SET_MESH, &s, &f, GEO_DUE_SAME);
  CHECK(!strcmp(t, "+/-60 m, 3 min ago (bench)"), "'map + mesh', 3 min, a bench fix says so");
  f.state = SELF_ST_OLD;
  s.lastVerdict = GEO_V_TOO_FEW;
  s.lastDevices = 2;
  geoRowText(t, sizeof(t), GEO_SET_MAP, &s, &f, GEO_DUE_SAME);
  CHECK(!strcmp(t, "too few Wi-Fi networks (2 of 4)"), "too few: how many of the four");
  s.lastVerdict = GEO_V_NOT_FOUND;
  geoRowText(t, sizeof(t), GEO_SET_MAP, &s, NULL, GEO_DUE_SAME);
  CHECK(!strcmp(t, "not enough known Wi-Fi here"), "not found: 'not enough known Wi-Fi'");
  s.lastVerdict = GEO_V_VAGUE;
  s.lastAccM = 350;
  geoRowText(t, sizeof(t), GEO_SET_MAP, &s, NULL, GEO_DUE_SAME);
  CHECK(!strcmp(t, "too vague (+/-350 m): not used"), "too vague: how vague");
  geoRowText(t, sizeof(t), GEO_SET_MAP, &s, NULL, GEO_DUE_MEASURED);
  CHECK(has(t, "not needed"), "a GPS/phone fix in hand: not needed");
  geoRowText(t, sizeof(t), GEO_SET_MAP, &s, NULL, GEO_DUE_NO_WIFI);
  CHECK(!strcmp(t, "not on WiFi"), "no WiFi");
  geoSchedInit(&s);
  geoRowText(t, sizeof(t), GEO_SET_MAP, &s, NULL, GEO_DUE_UNWANTED);
  CHECK(has(t, "Maps") && has(t, "Almanac"), "nothing yet, not wanted: when it is used");
  bool allOk = true;
  for (int vv = GEO_V_NONE; vv <= GEO_V_LAST; vv++) {
    for (int dd = GEO_DUE_YES; dd <= GEO_DUE_HOUR_CAP; dd++) {
      for (int st = 0; st < 3; st++) {
        s.lastVerdict = (uint8_t)vv;
        s.lastDevices = 3;
        s.lastAccM = 100;
        const size_t len = geoRowText(t, sizeof(t), st, &s, NULL, dd);
        allOk = allOk && len == strlen(t) && len > 0 && len <= 52 && printable(t);
        // no MAC (xx:xx) and no coordinate (a decimal point between digits)
        for (const char* p = t; *p && allOk; p++) {
          if (p[0] == '.' && p > t && p[-1] >= '0' && p[-1] <= '9' && p[1] >= '0' && p[1] <= '9') allOk = false;
        }
      }
    }
  }
  CHECK(allOk, "every row: printable ASCII the phone's font has, at most 52 characters, no coordinate");
  // ...and every one of them fits the row, the live fix's at its widest too.
  bool fit = true;
  for (int vv = GEO_V_NONE; vv <= GEO_V_LAST; vv++) {
    for (int dd = GEO_DUE_YES; dd <= GEO_DUE_HOUR_CAP; dd++) {
      for (int st = 0; st < 3; st++) {
        s.lastVerdict = (uint8_t)vv;
        s.lastDevices = 40;
        s.lastAccM = 65535;
        geoRowText(t, sizeof(t), st, &s, NULL, dd);
        fit = rowFits(t) && fit;
      }
    }
  }
  s.held = true;
  geoRowText(t, sizeof(t), GEO_SET_MAP, &s, NULL, GEO_DUE_SAME);
  CHECK(has(t, "WiFi changed") && printable(t), "a fix held since the WiFi changed: says so");
  fit = rowFits(t) && fit;
  s.held = false;
  s.lastVerdict = GEO_V_JUMP;
  geoRowText(t, sizeof(t), GEO_SET_MAP, &s, NULL, GEO_DUE_SAME);
  CHECK(has(t, "far from your last fix"), "too far from the last GPS/phone fix: said in words");
  s.lastVerdict = GEO_V_MOVED;
  geoRowText(t, sizeof(t), GEO_SET_MAP, &s, NULL, GEO_DUE_SAME);
  CHECK(has(t, "moved"), "a scan contradicted the look: said in words");
  CHECK(printable(GEO_ROW_MESH_PENDING) && has(GEO_ROW_MESH_PENDING, "leave"),
        "'map + mesh' chosen, the screen not left yet: says when it starts");
  fit = rowFits(GEO_ROW_MESH_PENDING) && fit;
  SelfFix wide;
  memset(&wide, 0, sizeof(wide));
  wide.src = SELF_NET;
  wide.state = SELF_ST_LIVE;
  wide.accM = 100;
  for (int st = 1; st < 3; st++) {
    for (int inj = 0; inj < 2; inj++) {
      wide.injected = inj != 0;
      wide.ageMs = SELF_NET_SHOW_MS - 1;          // "11 min ago", the oldest a shown fix gets
      geoRowText(t, sizeof(t), st, &s, &wide, GEO_DUE_SAME);
      fit = rowFits(t) && fit;
    }
  }
  char wl[160];
  snprintf(wl, sizeof(wl), "every line fits the row's %d px of Akrobat Bold 16 (widest %d px: %s)",
           ROW_W, g_widest, g_widestText);
  CHECK(fit, wl);
  char small[6];
  CHECK(geoRowText(small, sizeof(small), GEO_SET_OFF, &s, NULL, GEO_DUE_OFF) == 5 && strlen(small) == 5,
        "cut to fit, NUL kept");
  CHECK(geoRowText(NULL, 5, 0, &s, NULL, 0) == 0 && geoRowText(small, 0, 0, &s, NULL, 0) == 0, "no room: nothing");
}

// ---------------------------------------------------------------- the bench's canned answers

static void testBench() {
  printf("`netloc answer`: canned halves through geoBenchFold\n");
  const uint32_t T = 20000000;
  const GeoHalf a = answer(47.6205, -122.3493, 40);
  const GeoHalf b = answer(47.6205 + 20.0 / M_PER_DEG_LAT, -122.3493, 45);
  {
    // A real split (this phone's latest scan, 4 devices): the fix stands on its devices.
    World w;
    worldInit(&w, T);
    const GeoSched before = w.s;
    GeoFix fx;
    const int v = geoBenchFold(&w.s, &w.split, &a, &b, T + 600000, 1760000123u, 1, &w.net, &fx);
    CHECK(v == GEO_V_LOCATED && fx.accM == 50, "a real split: located, +/-50 m");
    CHECK(w.net.injected && w.net.fixMs == T + 600000 && w.net.firstMs == T + 600000 &&
          w.net.scanUtc == 1760000123u, "stamped NOW (not the snapshot's time), INJECTED");
    CHECK(w.s.lastLookMs == before.lastLookMs && w.s.attemptMs[0] == 0 && w.s.askMs == before.askMs,
          "the schedule untouched: no look, no attempt");
    CHECK(w.s.fixFromRef && w.s.ref.devices == 4, "...and the fix stands on the scan's four devices");
    SelfFix tx;
    SelfSlot slots[SELF_SRC_COUNT];
    memset(slots, 0, sizeof(slots));
    slots[SELF_NET - 1] = w.net;
    slots[SELF_NET - 1].noTx = false;
    CHECK(selfSource(slots, SELF_NET, T + 600000, SELF_PICK_TX, &tx) && tx.state == SELF_ST_INJECTED,
          "a bench fix is never sendable by the schedule");
    // The next scan that no longer hears them drops it, like a real fix.
    std::vector<GeoAp> away;
    away.push_back(ap(20, -50)); away.push_back(ap(21, -55)); away.push_back(ap(22, -60));
    CHECK(scanAt(&w, away, T + 660000) == GEO_SCAN_MOVED && w.net.fixMs == 0, "...and a moved scan drops it");
  }
  {
    // Too few devices around: a stand-in split - the verdict is the halves' alone.
    World w;
    worldInit(&w, T);
    std::vector<GeoAp> two;
    two.push_back(ap(1, -50)); two.push_back(ap(2, -60));
    scanAt(&w, two, T);                       // the real snapshot: two devices, not built
    const GeoSet refBefore = w.s.ref;
    GeoFix fx;
    const int v = geoBenchFold(&w.s, &w.split, &a, &b, T + 1000, 0, 1, &w.net, &fx);
    CHECK(!w.split.built && v == GEO_V_LOCATED && w.net.injected && w.net.fixMs == T + 1000,
          "a stand-in split: located from the halves alone, a bench fix");
    CHECK(!w.s.fixFromRef && memcmp(&w.s.ref, &refBefore, sizeof(GeoSet)) == 0 && w.s.lastDevices == 2,
          "...the yardstick left as it was, the fix on no set, the real device count reported");
    CHECK(scanAt(&w, two, T + 61000) == GEO_SCAN_NO_FIX && w.net.fixMs == T + 1000,
          "...so a scan neither confirms nor drops it");
    CHECK(selfSlotState(&w.net, SELF_NET, T + 1000 + SELF_NET_SHOW_MS, SELF_PICK_INJECTED) == SELF_ST_OLD,
          "...it simply expires SELF_NET_SHOW_MS on");
    const GeoHalf vague = answer(47.6205, -122.3493, 140);
    CHECK(geoBenchFold(&w.s, NULL, &vague, &b, T + 2000, 0, 1, &w.net, NULL) == GEO_V_VAGUE,
          "no snapshot at all: still judged (+/-140 m is too vague)");
    const GeoHalf none = reply(GEO_R_NOT_ASKED);
    GeoHalf h404;
    geoHalfFrom(&h404, GEO_R_ANSWERED, 404, "", 0);
    CHECK(geoBenchFold(&w.s, NULL, &h404, &none, T + 3000, 0, 1, &w.net, NULL) == GEO_V_NOT_FOUND,
          "a canned 404: not found");
    CHECK(w.s.lastLookMs == 0 && geoAttemptsLastHour(&w.s, T + 3000) == 0, "bench folds never count as looks");
  }
  {
    // Switched off: a canned fix is not kept.
    World w;
    worldInit(&w, T);
    w.net.enabled = false;
    CHECK(geoBenchFold(&w.s, &w.split, &a, &b, T, 0, 1, &w.net, NULL) == GEO_V_LOCATED && w.net.fixMs == 0,
          "the setting off: the verdict, but no fix written");
  }
}

// ---------------------------------------------------------------- the pinned roots

static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// The DER of the n-th certificate in a PEM string (test_weather.cpp's).
static std::vector<uint8_t> derOf(const char* pem, int nth) {
  std::vector<uint8_t> der;
  const char* p = pem;
  for (int i = 0; i <= nth; i++) {
    p = strstr(p, "-----BEGIN CERTIFICATE-----\n");
    if (!p) return der;
    p += strlen("-----BEGIN CERTIFICATE-----\n");
  }
  const char* end = strstr(p, "-----END CERTIFICATE-----");
  uint32_t acc = 0;
  int bits = 0;
  for (; p < end; p++) {
    const int v = b64v(*p);
    if (v < 0) continue;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      der.push_back((uint8_t)((acc >> bits) & 0xFF));
    }
  }
  return der;
}

static std::string sha256Hex(const std::vector<uint8_t>& d) {
  uint8_t h[32];
  bsSha256(d.data(), d.size(), h);
  std::string s;
  char b[4];
  for (int i = 0; i < 32; i++) {
    snprintf(b, sizeof(b), i ? ":%02X" : "%02X", h[i]);
    s += b;
  }
  return s;
}

static void testRoots() {
  printf("the pinned roots: ISRG Root X1 + X2 + the self-signed Root YE (geoloc.h, isrg_roots.h)\n");
  static const char ROOTS[] = GEO_ROOTS_PEM;
  const std::vector<uint8_t> x1 = derOf(ROOTS, 0), x2 = derOf(ROOTS, 1), ye = derOf(ROOTS, 2);
  CHECK(x1.size() > 1000 && x2.size() > 400 && ye.size() > 400 && derOf(ROOTS, 3).empty(),
        "exactly three certificates, concatenated");
  CHECK(sha256Hex(x1) == "96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6",
        "X1: SHA-256 96:BC:EC...DF:08:C6 (the weather's and the OTA's bytes)");
  CHECK(sha256Hex(x2) == "69:72:9B:8E:15:A8:6E:FC:17:7A:57:AF:B7:17:1D:FC:64:AD:D2:8C:2F:CA:8C:F1:50:7E:34:45:3C:CB:14:70",
        "X2: SHA-256 69:72:9B...CB:14:70 (macOS keychain = certifi = letsencrypt.org)");
  CHECK(sha256Hex(ye) == "E1:4F:FC:AD:5B:00:25:73:10:06:CA:A4:3A:12:1A:22:D8:E9:70:0F:4F:B9:CF:85:2F:02:A7:08:AA:5D:56:66",
        "Root YE: SHA-256 E1:4F:FC...5D:56:66 (second-sourced by its key, isrg_roots.h)");
  CHECK(derOf(ISRG_ROOT_X1_PEM, 0) == x1 && derOf(ISRG_ROOT_X2_PEM, 0) == x2 && derOf(ISRG_ROOT_YE_PEM, 0) == ye,
        "each macro alone is its own certificate");
  CHECK(strncmp(GEO_URL, "https://", 8) == 0 && strstr(GEO_URL, GEO_HOST) != NULL,
        "the lookup is HTTPS, to the pinned host");
}

int main() {
  printf("test_geoloc\n");
  testRequest();
  testScan();
  testAnswer();
  testHalves();
  testSet();
  testSchedule();
  testGuards();
  testEndToEnd();
  testSwitcher();
  testSettingAndWords();
  testBench();
  testRoots();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
