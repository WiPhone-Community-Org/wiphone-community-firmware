/*
 * test_tzauto.cpp - WiPhone/tz_auto.cpp: the time zone setting, the Auto decision and the offset
 * the clock gets, on the host.
 *
 * What is independent of the code under test:
 *   * every expected OFFSET and DST change comes from tests/vectors_tzgrid.h, which is Python's
 *     zoneinfo (tzdata 2026d) bisected to the second - not from a rule model;
 *   * every place is a public city (state capitals, the vectors' named places) or a random point
 *     of the vectors; failure lines print coordinates to 2 decimals only;
 *   * the 10 km margin (every phone fix) is checked against a brute-force sampling of
 *     tzGridLookup at a 0.02 deg pitch (finer than the 0.05 deg cells), not against the cell walk
 *     it uses;
 *   * every text Settings shows is measured with the firmware's own Akrobat_Bold16 glyph table.
 * The decision is driven through the real self_pos.cpp slots, so "a bench fix never counts" is
 * the same selfSlotState() the beacon obeys.
 */

#include "../WiPhone/tz_auto.h"
#include "../WiPhone/tz_grid.h"
#include "../WiPhone/self_pos.h"
#include "vectors_tzgrid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGMEM
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../WiPhone/src/assets/fonts.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

#define CHECKQ(cond, fmt, ...) do { \
    checks++; \
    if (!(cond)) { printf("  FAIL " fmt " (line %d)\n", __VA_ARGS__, __LINE__); failures++; } \
  } while (0)

// ── the phone's font, as SmoothFont measures it (the same reader as test_almanac_lines) ────────
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

static bool fontGlyph(const Font7* f, uint16_t u, int* g) {
  for (int i = 0; i < f->n; i++) {
    if (f->uni[i] == u) { *g = i; return true; }
  }
  return false;
}

static int textWidth(const Font7* f, const char* s, bool* missing) {
  int w = 0;
  for (const char* p = s; *p; p++) {
    const uint16_t u = (unsigned char)*p;
    if (u == 0x20) { w += f->space; continue; }
    int g = 0;
    if (fontGlyph(f, u, &g)) {
      if (w == 0 && f->dx[g] < 0) w -= f->dx[g];
      w += p[1] ? f->adv[g] : (f->dx[g] + f->w[g]);
    } else {
      w += f->space + 1;
      if (missing) *missing = true;
    }
  }
  return w;
}

/* Settings' info lines: a LabelWidget 240 px wide with 6 px of padding each side. */
#define LINE_W 228
static Font7 g_f16;
static int g_widest = 0;
static char g_widestText[96];
static bool fits(const char* s, const char* where) {
  bool missing = false;
  const int w = textWidth(&g_f16, s, &missing);
  if (w > g_widest) { g_widest = w; snprintf(g_widestText, sizeof(g_widestText), "%s", s); }
  if (missing || w > LINE_W || !*s) {
    printf("    \"%s\" (%s): %d px%s\n", s, where, w, missing ? ", a glyph missing" : "");
    return false;
  }
  return true;
}

// ── places ─────────────────────────────────────────────────────────────────────────────────
static const TzvLookup* place(const char* prefix) {
  for (size_t i = 0; i < sizeof(TZV_NAMED) / sizeof(TZV_NAMED[0]); i++) {
    if (TZV_NAMED[i].name && !strncmp(TZV_NAMED[i].name, prefix, strlen(prefix))) return &TZV_NAMED[i];
  }
  printf("  (no vector place \"%s\")\n", prefix);
  return NULL;
}

static int zoneOf(const char* iana) { return tzGridFindZone(iana); }

static void setFix(SelfSlot* slots, int src, int32_t lat, int32_t lon, int sats, int hdop,
                   uint32_t now, bool injected = false) {
  SelfSlot* s = &slots[src - 1];
  s->enabled = true;
  selfSlotSet(s, lat, lon, sats, hdop, now, injected);
}

static void noSlots(SelfSlot* slots) {
  memset(slots, 0, sizeof(SelfSlot) * SELF_SRC_COUNT);
  for (int i = 0; i < SELF_SRC_COUNT; i++) selfSlotClear(&slots[i]);
}

/* Run tzaStep every `stepMs` from t0 to t1 with the fix re-stamped at each step (a live 1 Hz
 * stream), returning how many switches happened. */
static int runStream(TzAuto* a, uint8_t mode, SelfSlot* slots, int src, const TzvLookup* p,
                     int sats, int hdop, uint32_t t0, uint32_t t1, uint32_t stepMs,
                     bool injected = false) {
  int n = 0;
  for (uint32_t t = t0; t <= t1; t += stepMs) {
    setFix(slots, src, p->lat, p->lon, sats, hdop, t, injected);
    if (tzaStep(a, mode, slots, t)) n++;
  }
  return n;
}

static double metresBetween(double la1, double lo1, double la2, double lo2) {
  const double R = 6371000.0, k = 3.14159265358979323846 / 180.0;
  const double x = (lo2 - lo1) * k * cos((la1 + la2) * 0.5 * k);
  const double y = (la2 - la1) * k;
  return R * sqrt(x * x + y * y);
}

/* ── the setting ───────────────────────────────────────────────────────────────────────────── */
static void testSetting() {
  printf("the setting\n");
  bool keysOk = true, gridOk = true, findOk = true;
  for (int i = 0; i < TZ_NAMED_COUNT; i++) {
    const TzNamed* z = tzNamed(i);
    for (int j = 0; j < i; j++) {
      if (!strcmp(tzNamed(j)->key, z->key) || !strcmp(tzNamed(j)->display, z->display)) keysOk = false;
    }
    for (const char* p = z->key; *p; p++) if (*p < 'a' || *p > 'z') keysOk = false;
    /* Its standard offset and rule are its IANA zone's (the grid's table, which the generator
     * verified hour by hour against tzdata), with no sinceUtc: valid at every instant. */
    const TzZone* g = tzGridZone(zoneOf(z->iana));
    if (!g || g->stdMinutes != z->stdMinutes || g->rule != z->rule || g->sinceUtc != 0) {
      printf("    %s: not its grid zone's clock\n", z->key);
      gridOk = false;
    }
    char up[32];
    snprintf(up, sizeof(up), "%s", z->display);
    for (char* p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    if (tzNamedFind(z->key) != i || tzNamedFind(up) != i || tzNamedFind(z->iana) != i) findOk = false;
  }
  CHECK(tzNamed(-1) == NULL && tzNamed(TZ_NAMED_COUNT) == NULL && tzNamed(0) != NULL,
        "ten named zones, NULL outside");
  CHECK(keysOk, "keys and display names are unique; keys are plain lower-case words (NVS, serial)");
  CHECK(gridOk, "each named zone's offset and rule = the grid's zone of that IANA name (no sinceUtc)");
  CHECK(findOk, "tzNamedFind: key, display in any case, IANA name");
  CHECK(tzNamedFind("") == -1 && tzNamedFind(NULL) == -1 && tzNamedFind("utc") == -1 &&
        tzNamedFind("pacifi") == -1, "tzNamedFind: nothing else");
  CHECK(!strcmp(tzNamed(0)->key, "pacific") && !strcmp(tzNamed(4)->key, "eastern") &&
        !strcmp(tzNamed(9)->key, "saskatchewan"), "the brief's order: Pacific first, Saskatchewan last");

  CHECK(tzHoursToMinutes(-7.0f) == -420 && tzHoursToMinutes(-3.5f) == -210 &&
        tzHoursToMinutes(5.75f) == 345 && tzHoursToMinutes(0.0f) == 0 &&
        tzHoursToMinutes(-0.0f) == 0 && tzHoursToMinutes(14.0f) == 840,
        "typed hours -> minutes: -7, -3.5 (-3:30), 5.75, 0, -0, 14");
  CHECK(tzHoursToMinutes(-7.0f / 3.0f) == -140, "a float that is not exact rounds to the nearest minute");
  CHECK(tzCustomOk(-720) && tzCustomOk(840) && tzCustomOk(0) && !tzCustomOk(-721) &&
        !tzCustomOk(841) && !tzCustomOk(tzHoursToMinutes(1e9f)),
        "a typed offset is -12:00..+14:00");

  bool rt = true;
  for (int c = 0; c < TZ_CHOICE_COUNT; c++) {
    uint8_t m = 99;
    int n = -9;
    tzChoiceTo(c, &m, &n);
    if (tzChoiceOf(m, n) != c) rt = false;
    if (!strcmp(tzChoiceName(c), "?")) rt = false;
  }
  CHECK(rt, "Settings' choice <-> (mode, named zone) round-trips for all 12");
  CHECK(!strcmp(tzChoiceName(0), "Auto") && !strcmp(tzChoiceName(TZ_CHOICE_CUSTOM), "Custom") &&
        !strcmp(tzChoiceName(5), "Eastern"), "choice names: Auto, ..., Eastern, ..., Custom");
  CHECK(tzChoiceOf(TZ_MODE_NAMED, 99) == TZ_CHOICE_AUTO && tzChoiceOf(7, 0) == TZ_CHOICE_AUTO,
        "a corrupt setting reads as Auto");

  CHECK(tzaZoneFromSaved("America/New_York") == zoneOf("America/New_York") &&
        tzaZoneFromSaved("America/Vancouver") == zoneOf("America/Vancouver"),
        "a saved zone comes back by its IANA name");
  CHECK(tzaZoneFromSaved("America/Havana") == -1 && tzaZoneFromSaved("America/Nuuk") == -1,
        "a saved zone whose rules are not modelled is no zone (the typed offset)");
  CHECK(tzaZoneFromSaved("Europe/Nowhere") == -1 && tzaZoneFromSaved("") == -1 &&
        tzaZoneFromSaved(NULL) == -1, "an unknown or empty name is no zone");
}

/* ── which fix the clock may use ───────────────────────────────────────────────────────────── */
static void testPick() {
  printf("which fix\n");
  const TzvLookup* col = place("Harrisburg PA");
  if (!col) { failures++; return; }
  SelfSlot s[SELF_SRC_COUNT];
  SelfFix fx;
  uint8_t why = 0;
  const uint32_t now = 1000000;

  noSlots(s);
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_NONE && why == TZA_WHY_NO_FIX && fx.src == SELF_NONE,
        "nothing on: no fix");
  s[0].enabled = s[1].enabled = true;
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_NONE && why == TZA_WHY_NO_FIX,
        "both on, never fixed: no fix");

  noSlots(s);
  setFix(s, SELF_GPS, col->lat, col->lon, 9, 12, now - 1000);
  setFix(s, SELF_PHONE, col->lat, col->lon, 8, 7, now - 500);
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_GPS && fx.state == SELF_ST_LIVE,
        "GPS and phone both live: the GPS (rank)");

  setFix(s, SELF_GPS, col->lat, col->lon, 3, 12, now - 1000);
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_PHONE, "GPS poor (3 sats): the phone");
  setFix(s, SELF_GPS, col->lat, col->lon, 9, 12, now - 30000);
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_PHONE, "GPS 30 s old: the phone (the beacon's bar)");
  setFix(s, SELF_GPS, col->lat, col->lon, 9, 12, now - 29999);
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_GPS, "GPS 29.999 s old: still the GPS");

  noSlots(s);
  setFix(s, SELF_GPS, col->lat, col->lon, 9, 12, now - 100, true);     // `pos inject gps`
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_NONE && why == TZA_WHY_BENCH,
        "a bench GPS fix (`pos inject gps`): refused, and said so");
  setFix(s, SELF_PHONE, col->lat, col->lon, 9, 12, now - 100, true);   // `phonepos at`
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_NONE && why == TZA_WHY_BENCH,
        "a bench phone fix (`phonepos at` / `pos inject phone`): refused");
  setFix(s, SELF_PHONE, col->lat, col->lon, 9, 12, now - 100, false);
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_PHONE && !fx.injected,
        "a bench GPS fix does not hide a live phone fix");

  noSlots(s);
  setFix(s, SELF_PHONE, col->lat, col->lon, -1, -1, now - 100);         // GPS 2 IP: unrated
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_PHONE && fx.state == SELF_ST_UNRATED,
        "an iPhone fix with no accuracy (shown, not sent): taken, marked UNRATED");
  setFix(s, SELF_PHONE, col->lat, col->lon, 9, 105, now - 100);
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_NONE && why == TZA_WHY_POOR, "HDOP 10.5: poor, refused");
  setFix(s, SELF_PHONE, col->lat, col->lon, 9, 12, now - 45000);
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_NONE && why == TZA_WHY_OLD, "45 s old: refused as old");

  noSlots(s);
  s[SELF_NET - 1].enabled = true;
  selfSlotSet(&s[SELF_NET - 1], col->lat, col->lon, 12, 5, now - 10, false);
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_NONE, "the Wi-Fi slot (written GNSS-style) is never a source");
  /* 0.9.82's Wi-Fi location: a REAL Wi-Fi fix - two agreeing scans 61 s apart, the last 1 s ago,
   * "map + mesh", a fix the beacon itself would send - still never chooses the zone. */
  noSlots(s);
  s[SELF_NET - 1].enabled = true;
  s[SELF_NET - 1].noTx = false;
  selfNetSet(&s[SELF_NET - 1], col->lat, col->lon, 50, now - 62000, 0, false);
  selfNetConfirm(&s[SELF_NET - 1], now - 1000, 0);
  CHECK(selfSlotState(&s[SELF_NET - 1], SELF_NET, now, SELF_PICK_TX) == SELF_ST_LIVE &&
        selfPick(s, now, SELF_PICK_TX, &fx) == SELF_NET,
        "(a live, sendable Wi-Fi fix: the beacon would pick it)");
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_NONE && fx.src == SELF_NONE,
        "...and the time zone still refuses it: never a Wi-Fi fix");
  TzAuto wa;
  tzaInit(&wa, -1, SELF_NONE);
  bool wsw = false;
  for (uint32_t t = now; t < now + 300000; t += 10000) {
    selfNetConfirm(&s[SELF_NET - 1], t, 0);
    wsw = wsw || tzaStep(&wa, TZ_MODE_AUTO, s, t);
  }
  CHECK(!wsw && wa.zone < 0, "five minutes of a confirmed Wi-Fi fix: no zone, no switch");

  noSlots(s);
  setFix(s, SELF_GPS, col->lat, col->lon, 3, 64, now - 100);
  setFix(s, SELF_PHONE, col->lat, col->lon, 9, 12, now - 100, true);
  CHECK(tzaPickFix(s, now, &fx, &why) == SELF_NONE && why == TZA_WHY_BENCH,
        "poor GPS + bench phone: the bench refusal is the one named");
  CHECK(tzaPickFix(NULL, now, NULL, &why) == SELF_NONE && why == TZA_WHY_NO_FIX, "NULL slots");
}

/* ── the woods plate's GPS, through its UART gate (selfGpsApply, 0.9.82) ───────────────────────
 * Auto takes a live GPS fix. Until the 2026-10-10 review the plate's fix was stamped on an RMC's
 * `valid` with the reader's sticky fields, so with the RMCs lost every no-fix GGA re-stamped the
 * last place as NOW: the GPS outranks the hotspot phone, and the clock stayed in the old zone
 * whatever the phone's real fix said. Driven here through the shipping NMEA reader. */
static void nmeaCoord(char* out, size_t cap, int32_t e7, bool lat) {
  const char hemi = lat ? (e7 < 0 ? 'S' : 'N') : (e7 < 0 ? 'W' : 'E');
  const int64_t a = e7 < 0 ? -(int64_t)e7 : e7;
  const int deg = (int)(a / 10000000);
  const int64_t minE5 = (a % 10000000) * 60 / 100;          // minutes x 1e5
  snprintf(out, cap, lat ? "%02d%02d.%05d,%c" : "%03d%02d.%05d,%c", deg, (int)(minE5 / 100000),
           (int)(minE5 % 100000), hemi);
}

static void uartFeed(NmeaReader* r, SelfGpsRx* rx, SelfSlot* slots, const char* body, uint32_t t) {
  unsigned x = 0;
  for (const char* p = body; *p; p++) x ^= (unsigned char)*p;
  char line[160];
  snprintf(line, sizeof(line), "$%s*%02X\r\n", body, x);
  for (const char* p = line; *p; p++) {
    if (r->feed(*p)) selfGpsApply(rx, &slots[SELF_GPS - 1], r->fix(), t);
  }
}

static void testUartGps() {
  printf("the plate's GPS through its UART gate\n");
  const TzvLookup* chi = place("Chicago IL");
  const TzvLookup* col = place("Harrisburg PA");
  if (!chi || !col) { failures++; return; }
  const int EAST = col->answer, CENT = chi->answer;
  char la[24], lo[24], body[140];
  nmeaCoord(la, sizeof(la), chi->lat, true);
  nmeaCoord(lo, sizeof(lo), chi->lon, false);
  SelfSlot s[SELF_SRC_COUNT];
  noSlots(s);
  s[SELF_GPS - 1].enabled = true;
  static NmeaReader r;
  r.reset();
  SelfGpsRx rx;
  selfGpsInit(&rx);
  TzAuto a;
  tzaInit(&a, EAST, SELF_PHONE);
  uint32_t t = 0;
  for (int sec = 0; sec <= 150; sec++) {            // the M100 at 1 Hz in Chicago, looks every 30 s
    t = 1000 + 1000u * sec;
    snprintf(body, sizeof(body), "GNRMC,%02d%02d%02d.00,A,%s,%s,0.10,,101026,,,A", 12, sec / 60, sec % 60, la, lo);
    uartFeed(&r, &rx, s, body, t);
    snprintf(body, sizeof(body), "GNGGA,%02d%02d%02d.00,%s,%s,1,09,0.9,180.0,M,,M,,", 12, sec / 60, sec % 60, la, lo);
    uartFeed(&r, &rx, s, body, t + 40);
    if (sec % 30 == 0) tzaStep(&a, TZ_MODE_AUTO, s, t + 40);
  }
  CHECK(a.zone == CENT && a.zoneSrc == SELF_GPS, "the M100 at 1 Hz in Chicago, RMC + GGA: Central, from GPS");

  /* Then its RMCs are lost and its GGAs say "no fix" with every field empty, while the hotspot
   * phone has a live fix in Harrisburg. */
  SelfFix fx;
  uint8_t why = 0;
  for (int sec = 151; sec <= 340; sec++) {
    t = 1000 + 1000u * sec;
    snprintf(body, sizeof(body), "GNGGA,%02d%02d%02d.00,,,,,0,,,,M,,M,,", 12, sec / 60, sec % 60);
    uartFeed(&r, &rx, s, body, t);
    setFix(s, SELF_PHONE, col->lat, col->lon, 9, 9, t);
    if (sec % 30 == 0) tzaStep(&a, TZ_MODE_AUTO, s, t);
  }
  CHECK(rx.fixMs == 151040 && tzaPickFix(s, t, &fx, &why) == SELF_PHONE,
        "...RMCs lost, 190 s of no-fix GGAs: the GPS fix is not re-stamped, the phone's live fix is used");
  CHECK(a.zone == EAST && a.zoneSrc == SELF_PHONE,
        "...so the clock follows the phone to Eastern (the old re-stamp kept Chicago 'live')");
}

/* ── the 10 km margin, against brute force ─────────────────────────────────────────────────── */
static int g_nearLat = 0, g_nearLon = 0;     // a point the GPS's plain lookup answers and a phone fix does not
static bool g_haveNear = false;

static void testMargin() {
  printf("the margin for a phone fix\n");
  const int M = TZA_PHONE_MARGIN_M;
  // Plain = margin 0, for every named place.
  bool same = true;
  for (size_t i = 0; i < sizeof(TZV_NAMED) / sizeof(TZV_NAMED[0]); i++) {
    if (!TZV_NAMED[i].name) continue;
    uint8_t w1 = 99, w2 = 98;
    const int a = tzGridLookup(TZV_NAMED[i].lat, TZV_NAMED[i].lon, &w1);
    const int b = tzGridLookupMargin(TZV_NAMED[i].lat, TZV_NAMED[i].lon, 0, &w2);
    if (a != b || w1 != w2) same = false;
  }
  CHECK(same, "margin 0 = the plain lookup, answer and why, at every named place");

  const char* inside[] = { "Harrisburg PA", "Springfield IL", "Sacramento CA", "Denver CO",
                           "Phoenix AZ", "Honolulu HI", "Anchorage AK", "Regina SK", "Halifax NS" };
  bool allIn = true;
  for (size_t i = 0; i < sizeof(inside) / sizeof(inside[0]); i++) {
    const TzvLookup* p = place(inside[i]);
    if (!p) { allIn = false; continue; }
    const int z = tzGridLookupMargin(p->lat, p->lon, M, NULL);
    if (z != p->answer) { printf("    %s: %d, wanted %d\n", inside[i], z, p->answer); allIn = false; }
  }
  CHECK(allIn, "10 km: capitals far from any zone line still answer their zone, the coastal ones too");
  // ...and the places near a real change of clock do not (the header names them).
  const char* nearLine[] = { "Knox IN", "Kykotsmovi AZ", "Victoria BC", "Atikokan ON", "Dawson Creek BC" };
  bool allNear = true;
  for (size_t i = 0; i < sizeof(nearLine) / sizeof(nearLine[0]); i++) {
    const TzvLookup* p = place(nearLine[i]);
    if (!p || p->answer < 0 || tzGridLookupMargin(p->lat, p->lon, M, NULL) != TZ_BORDER) {
      printf("    %s: not refused by the margin\n", nearLine[i]);
      allNear = false;
    }
  }
  CHECK(allNear, "10 km: Knox IN, the Hopi villages, Victoria BC, Atikokan, Dawson Creek are refused");

  const TzvLookup* phenix = place("Phenix City AL");
  const TzvLookup* pierre = place("Pierre SD");
  CHECK(phenix && pierre && tzGridLookupMargin(phenix->lat, phenix->lon, M, NULL) == TZ_BORDER &&
        tzGridLookupMargin(pierre->lat, pierre->lon, M, NULL) == TZ_BORDER,
        "a place the plain lookup already refuses stays refused");
  uint8_t w = 0;
  CHECK(tzGridLookupMargin(50000000, -1000000000, M, &w) == TZ_NOT_COVERED && w == TZ_WHY_OUTSIDE,
        "outside the grid (5 N): its own refusal");
  CHECK(tzGridLookupMargin(400000000, -1400000000, M, &w) == TZ_NOT_COVERED && w == TZ_WHY_WATER,
        "at sea: its own refusal");

  // The brute-force cross-check over the vectors' random points that answer a zone.
  int tried = 0, kept = 0, refused = 0, wrongKept = 0, wrongRefused = 0;
  const double PI = 3.14159265358979323846;
  for (size_t i = 0; i < sizeof(TZV_POINTS) / sizeof(TZV_POINTS[0]) && tried < 400; i += 3) {
    const TzvPoint& p = TZV_POINTS[i];
    if (p.answer < 0) continue;
    tried++;
    const int z = tzGridLookupMargin(p.lat, p.lon, M, NULL);
    const double la = p.lat * 1e-7, lo = p.lon * 1e-7;
    if (z >= 0) {
      kept++;
      // Every sample within 10 km must be a zone keeping the same clock.
      bool ok = z == p.answer;
      for (double dy = -0.1; dy <= 0.1 && ok; dy += 0.02) {
        for (double dx = -0.6; dx <= 0.6 && ok; dx += 0.02) {
          const double sla = la + dy, slo = lo + dx;
          if (metresBetween(la, lo, sla, slo) > M) continue;
          const int s = tzGridLookup((int32_t)lround(sla * 1e7), (int32_t)lround(slo * 1e7), NULL);
          if (s < 0 || (s != z && !tzGridSameClock(s, z))) ok = false;
        }
      }
      if (!ok) {
        wrongKept++;
        printf("    kept at %.2f,%.2f but a different clock lies within the margin\n", la, lo);
      }
    } else {
      refused++;
      // Something other than this clock lies within the box (+ one cell of slack).
      const double dLat = M / 110540.0 + 0.05;
      const double dLon = M / (111320.0 * cos((fabs(la) + dLat) * PI / 180.0)) + 0.05;
      bool found = false;
      for (double dy = -dLat; dy <= dLat && !found; dy += 0.02) {
        for (double dx = -dLon; dx <= dLon && !found; dx += 0.02) {
          const int s = tzGridLookup((int32_t)lround((la + dy) * 1e7), (int32_t)lround((lo + dx) * 1e7), NULL);
          if (s < 0 || (s != p.answer && !tzGridSameClock(s, p.answer))) found = true;
        }
      }
      if (!found) {
        wrongRefused++;
        printf("    refused at %.2f,%.2f with one clock all around\n", la, lo);
      }
      if (!g_haveNear && la > 25 && la < 49 && lo < -67 && lo > -125) {
        g_nearLat = p.lat;
        g_nearLon = p.lon;
        g_haveNear = true;
      }
    }
  }
  char line[160];
  snprintf(line, sizeof(line), "brute force over %d random zone points: %d kept, %d refused; "
           "every kept one has one clock within 10 km", tried, kept, refused);
  CHECK(tried >= 300 && wrongKept == 0, line);
  CHECK(wrongRefused == 0, "every refused one has another clock (or no zone) within the box");
  CHECK(kept > 0 && refused > 0, "the band refuses some points and keeps others");
  CHECK(g_haveNear, "a US point the plain lookup answers and the phone margin refuses exists");

  // Across 180 (the Aleutians): no crash, and both spellings of the same place agree.
  const TzvLookup* attu = place("Attu AK");
  CHECK(attu != NULL, "Attu is in the vectors");
  if (attu) {
    const int a = tzGridLookupMargin(attu->lat, attu->lon, M, NULL);
    CHECK(a == TZ_BORDER || a == attu->answer, "Attu (172 E): the margin answers its zone or refuses");
  }
  const int e = tzGridLookupMargin(520000000, 1799500000, M, NULL);
  const int wv = tzGridLookupMargin(520000000, -1799500000, M, NULL);
  CHECK((e == TZ_BORDER || e == TZ_NOT_COVERED || e >= 0) && (wv == TZ_BORDER || wv == TZ_NOT_COVERED || wv >= 0),
        "a box across 180 deg wraps without leaving the grid's arithmetic");
  CHECK(tzGridLookupMargin(719000000, -1000000000, M, NULL) < 0,
        "a box that crosses 72 N (the grid's edge) is refused");
}

/* ── back to Auto (review, 2026-10-10) ────────────────────────────────────────────────────────
 * The reviewers' case: Auto found Pacific at home; in Ohio the owner picks Eastern by hand; later
 * he picks Auto again, with no fix to be had. Before the fix the clock jumped back to Pacific at
 * once, labelled "From phone GPS". Now it stays on Eastern - set by hand, DST and all - until two
 * looks find another clock. The glue (tz_service.cpp's tzSvcSetMode, held by check_tz_glue.py)
 * calls tzaEnterAuto on every change into Auto. */
static void testBackToAuto(int EAST, int CENT, int PAC, const TzvLookup* chi, const TzvLookup* det,
                           const TzvLookup* sac) {
  SelfSlot s[SELF_SRC_COUNT];
  TzAuto a;
  TzEff e1, e2;
  const int nEast = tzNamedFind("eastern");
  const int ny = zoneOf("America/New_York");

  noSlots(s);
  tzaInit(&a, PAC, SELF_PHONE);                                   // found at home
  tzaEnterAuto(&a, TZ_MODE_NAMED, nEast);                          // Eastern by hand -> Auto
  CHECK(a.zone == ny && a.zoneSrc == TZA_SRC_HAND && a.cand == -1 && a.zone != PAC,
        "Named Eastern -> Auto: Auto starts on Eastern, set by hand - never the Pacific found before");

  // Every named zone, at every zoneinfo instant in the vectors: the Save moves nothing.
  bool same = true;
  int n = 0;
  for (int k = 0; k < TZ_NAMED_COUNT; k++) {
    TzAuto b;
    tzaInit(&b, PAC, SELF_PHONE);
    tzaEnterAuto(&b, TZ_MODE_NAMED, k);
    if (b.zone < 0 || b.zoneSrc != TZA_SRC_HAND) { same = false; continue; }
    for (size_t i = 0; i < sizeof(TZV_OFFSETS) / sizeof(TZV_OFFSETS[0]); i += 7) {
      const int64_t u = TZV_OFFSETS[i].utc;
      tzEffective(TZ_MODE_NAMED, k, -1, -420, true, u, &e1);
      tzEffective(TZ_MODE_AUTO, -1, b.zone, -420, true, u, &e2);
      n++;
      if (!(e1.ok && e2.ok && e1.minutes == e2.minutes && e1.next == e2.next &&
            e1.nextMinutes == e2.nextMinutes && e2.kind == TZK_AUTO)) {
        same = false;
        printf("    %s at %lld: named %d, Auto %d\n", tzNamed(k)->key, (long long)u, e1.minutes, e2.minutes);
      }
    }
  }
  char m[128];
  snprintf(m, sizeof(m), "...for all ten named zones, %d instants: the same offset and the same next DST "
           "change - the clock does not move", n);
  CHECK(same && n > 100, m);

  int sw = 0;
  for (uint32_t k = 0; k <= 120; k++) if (tzaStep(&a, TZ_MODE_AUTO, s, 1000 + k * 30000)) sw++;
  CHECK(sw == 0 && a.zone == ny && a.zoneSrc == TZA_SRC_HAND && a.why == TZA_WHY_NO_FIX,
        "an hour with no fix at all: still Eastern (set by hand), never Pacific");
  {
    char src[64], st[72];
    tzEffective(TZ_MODE_AUTO, -1, a.zone, -420, true, 1791676800LL, &e1);
    tzSourceText(&e1, a.zoneSrc, src, sizeof(src));
    tzStatusText(&e1, a.zoneSrc, st, sizeof(st));
    CHECK(!strcmp(src, "Set by hand, Auto checks it") && !strcmp(st, "Eastern, UTC-4, set by hand"),
          "Settings: \"Set by hand, Auto checks it\" (never \"From phone GPS\"); `tz`: \"Eastern, UTC-4, set by hand\"");
  }
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_PHONE, det, 9, 9, 4000000, 4600000, 30000) == 0 &&
        a.zone == ny && a.zoneSrc == TZA_SRC_HAND && a.why == TZA_WHY_SAME,
        "Detroit (the same clock): confirmed, nothing switched or announced");
  noSlots(s);
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, chi, 9, 9, 5000000, 5000000 + TZA_CONFIRM_MS, 30000) == 1 &&
        a.zone == CENT && a.zoneSrc == SELF_GPS,
        "Chicago on two looks 2 min apart: Central, from GPS - a measurement replaces the hand-set zone");

  // From Custom: no zone, the typed offset Custom was showing; the old Pacific needs two looks.
  noSlots(s);
  tzaInit(&a, PAC, SELF_PHONE);
  tzaEnterAuto(&a, TZ_MODE_CUSTOM, nEast);                         // a stale named index: ignored
  tzEffective(TZ_MODE_CUSTOM, -1, -1, -300, true, 1791676800LL, &e1);
  tzEffective(TZ_MODE_AUTO, -1, a.zone, -300, true, 1791676800LL, &e2);
  CHECK(a.zone == -1 && a.zoneSrc == SELF_NONE && e2.kind == TZK_AUTO_WAIT && e1.ok && e2.ok &&
        e1.minutes == e2.minutes, "Custom -> Auto: no zone, the same typed offset - the clock does not move");
  setFix(s, SELF_PHONE, sac->lat, sac->lon, 9, 9, 1000);
  CHECK(!tzaStep(&a, TZ_MODE_AUTO, s, 1000) && a.zone == -1 && a.cand == PAC,
        "...Sacramento seen once: the Pacific found before does not come back on one look");
  setFix(s, SELF_PHONE, sac->lat, sac->lon, 9, 9, 1000 + TZA_CONFIRM_MS);
  CHECK(tzaStep(&a, TZ_MODE_AUTO, s, 1000 + TZA_CONFIRM_MS) && a.zone == PAC && a.zoneSrc == SELF_PHONE,
        "...and on the second look 2 min later, it does");

  // From Auto (not a change) or a bad named index: as `tz forget` - no zone.
  tzaInit(&a, EAST, SELF_GPS);
  tzaEnterAuto(&a, TZ_MODE_NAMED, TZ_NAMED_COUNT);
  CHECK(a.zone == -1 && a.zoneSrc == SELF_NONE, "a named index out of range: no zone (the typed offset)");
  tzaInit(&a, EAST, SELF_GPS);
  a.switches = 3;
  a.checks = 9;
  a.cand = (int16_t)CENT;
  tzaForget(&a);
  CHECK(a.zone == -1 && a.zoneSrc == SELF_NONE && a.cand == -1 && a.switches == 3 && a.checks == 9,
        "tzaForget (`tz forget`): no zone, nothing waiting, the counters kept");
  tzaInit(&a, ny, TZA_SRC_HAND);
  CHECK(a.zone == ny && a.zoneSrc == TZA_SRC_HAND, "a reboot in Auto keeps a hand-set start (NVS azsrc)");
  tzaInit(&a, ny, 77);
  CHECK(a.zone == ny && a.zoneSrc == SELF_NONE, "an unknown saved source reads as none");
}

/* ── the decision ──────────────────────────────────────────────────────────────────────────── */
static void testDecide() {
  printf("the decision\n");
  const TzvLookup* col = place("Harrisburg PA");
  const TzvLookup* chi = place("Chicago IL");
  const TzvLookup* det = place("Detroit MI");
  const TzvLookup* ind = place("Indianapolis IN");
  const TzvLookup* sac = place("Sacramento CA");
  const TzvLookup* pierre = place("Pierre SD");
  if (!col || !chi || !det || !ind || !sac || !pierre) { failures++; return; }
  const int EAST = col->answer, CENT = chi->answer, PAC = sac->answer;
  SelfSlot s[SELF_SRC_COUNT];
  TzAuto a;

  // ---- the first zone, from nothing, needs two looks 2 min apart
  noSlots(s);
  tzaInit(&a, -1, SELF_NONE);
  CHECK(a.zone == -1 && a.cand == -1 && a.why == TZA_WHY_NONE && a.lastLookup == TZA_LOOKUP_NONE,
        "boot with nothing remembered: no zone");
  uint32_t t = 5000;
  setFix(s, SELF_PHONE, col->lat, col->lon, 9, 9, t);
  CHECK(!tzaStep(&a, TZ_MODE_AUTO, s, t) && a.why == TZA_WHY_WAITING && a.cand == EAST && a.zone == -1,
        "Harrisburg seen once: waiting, nothing changed");
  setFix(s, SELF_PHONE, col->lat, col->lon, 9, 9, t + 60000);
  CHECK(!tzaStep(&a, TZ_MODE_AUTO, s, t + 60000) && a.zone == -1, "a minute later: still waiting");
  setFix(s, SELF_PHONE, col->lat, col->lon, 9, 9, t + TZA_CONFIRM_MS - 1);
  CHECK(!tzaStep(&a, TZ_MODE_AUTO, s, t + TZA_CONFIRM_MS - 1) && a.zone == -1,
        "1 ms short of 2 minutes: still waiting");
  setFix(s, SELF_PHONE, col->lat, col->lon, 9, 9, t + TZA_CONFIRM_MS);
  CHECK(tzaStep(&a, TZ_MODE_AUTO, s, t + TZA_CONFIRM_MS) && a.zone == EAST &&
        a.zoneSrc == SELF_PHONE && a.why == TZA_WHY_SWITCHED && a.switches == 1 && a.cand == -1,
        "2 minutes after the first look: Eastern, from the phone, once");
  setFix(s, SELF_PHONE, col->lat, col->lon, 9, 9, t + 150000);
  CHECK(!tzaStep(&a, TZ_MODE_AUTO, s, t + 150000) && a.why == TZA_WHY_SAME, "then: confirmed, no switch");

  // ---- a same-clock neighbour is not a change
  setFix(s, SELF_PHONE, det->lat, det->lon, 9, 9, t + 200000);
  CHECK(!tzaStep(&a, TZ_MODE_AUTO, s, t + 200000) && a.why == TZA_WHY_SAME && a.cand == -1 &&
        a.zone == EAST && a.lastLookup == det->answer,
        "Detroit (the grid names Toronto there): the same clock, nothing switched or waiting");
  const int switchesBefore = (int)a.switches;
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, ind, 9, 9, t + 210000, t + 900000, 30000) == 0 &&
        a.switches == (uint32_t)switchesBefore, "Indianapolis for 11 min: the same clock, never a switch");

  // ---- a real change: Central
  noSlots(s);
  tzaInit(&a, EAST, SELF_PHONE);
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, chi, 10, 8, 1000, 91000, 30000) == 0 && a.zone == EAST &&
        a.cand == CENT, "Chicago for 90 s: still Eastern, Central waiting");
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, chi, 10, 8, 121000, 121000, 30000) == 1 &&
        a.zone == CENT && a.zoneSrc == SELF_GPS, "...at 2 min: Central, from GPS");

  // ---- a refusal between two sightings starts the count again
  noSlots(s);
  tzaInit(&a, EAST, SELF_PHONE);
  runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, chi, 10, 8, 1000, 61000, 30000);
  setFix(s, SELF_GPS, pierre->lat, pierre->lon, 10, 8, 90000);                 // a border cell
  CHECK(!tzaStep(&a, TZ_MODE_AUTO, s, 90000) && a.why == TZA_WHY_BORDER && a.cand == -1,
        "a border between two looks: the waiting zone is dropped");
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, chi, 10, 8, 120000, 120000 + TZA_CONFIRM_MS - 30000, 30000) == 0 &&
        a.zone == EAST, "Central again: counted from the new first look, not switched 90 s in");
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, chi, 10, 8, 120000 + TZA_CONFIRM_MS,
                  120000 + TZA_CONFIRM_MS, 30000) == 1 && a.zone == CENT, "...switched 2 min after it");

  // ---- flapping across a line never switches
  noSlots(s);
  tzaInit(&a, PAC, SELF_GPS);
  int sw = 0;
  for (uint32_t k = 0; k < 120; k++) {
    const TzvLookup* p = (k % 2) ? col : chi;
    setFix(s, SELF_GPS, p->lat, p->lon, 9, 9, k * 30000);
    if (tzaStep(&a, TZ_MODE_AUTO, s, k * 30000)) sw++;
  }
  CHECK(sw == 0 && a.zone == PAC, "Central/Eastern alternating every 30 s for an hour: no switch");

  // ---- a gap of more than 30 min forgets the waiting zone
  noSlots(s);
  tzaInit(&a, EAST, SELF_GPS);
  setFix(s, SELF_GPS, chi->lat, chi->lon, 9, 9, 1000);
  tzaStep(&a, TZ_MODE_AUTO, s, 1000);
  for (uint32_t k = 1; k <= 62; k++) {                                    // 31 min: the fix ages out
    tzaStep(&a, TZ_MODE_AUTO, s, 1000 + k * 30000);
  }
  CHECK(a.why == TZA_WHY_OLD && a.cand == -1, "no fix for 31 min: the waiting zone is forgotten");
  setFix(s, SELF_GPS, chi->lat, chi->lon, 9, 9, 1000 + 63 * 30000);
  CHECK(!tzaStep(&a, TZ_MODE_AUTO, s, 1000 + 63 * 30000) && a.zone == EAST && a.why == TZA_WHY_WAITING,
        "Central again after the gap: waiting again, not switched");
  // ...but a few minutes without a fix between two looks is fine
  noSlots(s);
  tzaInit(&a, EAST, SELF_GPS);
  setFix(s, SELF_GPS, chi->lat, chi->lon, 9, 9, 1000);
  tzaStep(&a, TZ_MODE_AUTO, s, 1000);
  for (uint32_t k = 1; k <= 10; k++) tzaStep(&a, TZ_MODE_AUTO, s, 1000 + k * 30000);
  setFix(s, SELF_GPS, chi->lat, chi->lon, 9, 9, 1000 + 11 * 30000);
  CHECK(tzaStep(&a, TZ_MODE_AUTO, s, 1000 + 11 * 30000) && a.zone == CENT,
        "two looks 5.5 min apart with no fix between: switched");

  // ---- the same fix can never be both looks
  noSlots(s);
  tzaInit(&a, EAST, SELF_GPS);
  setFix(s, SELF_GPS, chi->lat, chi->lon, 9, 9, 1000);            // stamped once, never again
  sw = 0;
  for (uint32_t k = 0; k < 20; k++) if (tzaStep(&a, TZ_MODE_AUTO, s, 1000 + k * 30000)) sw++;
  CHECK(sw == 0 && a.zone == EAST && a.why == TZA_WHY_OLD,
        "one fix, never refreshed: one look, then too old - never a switch");

  // ---- bench and poor fixes never count
  noSlots(s);
  tzaInit(&a, EAST, SELF_GPS);
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, chi, 12, 8, 0, 3600000, 30000, true) == 0 &&
        a.zone == EAST && a.why == TZA_WHY_BENCH && a.cand == -1,
        "an hour of bench GPS fixes at Chicago (`pos inject`): never Central");
  noSlots(s);
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_PHONE, chi, 12, 8, 0, 3600000, 30000, true) == 0 &&
        a.zone == EAST && a.why == TZA_WHY_BENCH, "...nor bench phone fixes (`phonepos at`)");
  noSlots(s);
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, chi, 3, 64, 0, 3600000, 30000) == 0 &&
        a.zone == EAST && a.why == TZA_WHY_POOR, "...nor an hour of poor fixes (sats 3, HDOP 6.4)");
  noSlots(s);
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, pierre, 12, 8, 0, 3600000, 30000) == 0 &&
        a.zone == EAST && a.why == TZA_WHY_BORDER, "...nor an hour on a zone line (Pierre SD)");

  // ---- a phone fix - any app, rated or not - counts, but not near a line (tz_auto.h rule 3)
  noSlots(s);
  tzaInit(&a, PAC, SELF_GPS);
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_PHONE, col, -1, -1, 1000, 121000, 30000) == 1 &&
        a.zone == EAST && a.zoneSrc == SELF_PHONE && a.lastUnrated,
        "an iPhone fix (no accuracy) deep in Pennsylvania: Eastern after 2 min");
  noSlots(s);
  tzaInit(&a, PAC, SELF_GPS);
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_PHONE, col, 12, 8, 1000, 121000, 30000) == 1 &&
        a.zone == EAST && a.zoneSrc == SELF_PHONE && !a.lastUnrated,
        "a rated phone fix deep in Pennsylvania: Eastern after 2 min");
  if (g_haveNear) {
    TzvLookup nearP = *col;
    nearP.lat = g_nearLat;
    nearP.lon = g_nearLon;
    const int nz = tzGridLookup(g_nearLat, g_nearLon, NULL);
    // Start from a zone with another clock, so that a switch to nz would be visible.
    int other = -1;
    for (int z = 0; z < tzGridZoneCount() && other < 0; z++) {
      const TzZone* zz = tzGridZone(z);
      if (zz && (zz->rule == TZ_RULE_NONE || zz->rule == TZ_RULE_USCAN) && zz->sinceUtc == 0 &&
          !tzGridSameClock(z, nz)) other = z;
    }
    noSlots(s);
    tzaInit(&a, other, SELF_GPS);
    CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_PHONE, &nearP, -1, -1, 1000, 3601000, 30000) == 0 &&
          a.zone == other && a.why == TZA_WHY_PHONE_NEAR,
          "an iPhone fix within 10 km of a change of clock: an hour, never a switch");
    /* Review, 2026-10-10: a RATED phone fix can be kilometres off too. gpsdRelay's generated GGA
     * says 12 satellites and HDOP <= 10 and can be 1-9 arc-minutes wrong, the same every second
     * while the phone stands still; GPS 2 IP's stand-in 8 / 0.9 is LIVE and rated off port
     * 11123. Neither may set the clock near a line. */
    noSlots(s);
    tzaInit(&a, other, SELF_GPS);
    CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_PHONE, &nearP, 12, 8, 1000, 3601000, 30000) == 0 &&
          a.zone == other && a.why == TZA_WHY_PHONE_NEAR && !a.lastUnrated && a.lastSrc == SELF_PHONE,
          "...nor a RATED phone fix there (12 sats, HDOP 0.8 - gpsdRelay's generated GGA): never");
    noSlots(s);
    tzaInit(&a, other, SELF_GPS);
    CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_PHONE, &nearP, SELF_PHONE_PLACEHOLDER_SATS,
                    SELF_PHONE_PLACEHOLDER_HDOPX10, 1000, 3601000, 30000) == 0 &&
          a.zone == other && a.why == TZA_WHY_PHONE_NEAR,
          "...nor GPS 2 IP's stand-in 8 sats / HDOP 0.9 taken as rated (off its default port)");
    noSlots(s);
    tzaInit(&a, other, SELF_GPS);
    CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, &nearP, 9, 9, 1000, 121000, 30000) == 1 &&
          a.zone == nz && a.zoneSrc == SELF_GPS,
          "...the woods plate's own GPS at the same place: switched (the grid's one-cell band)");
    SelfFix fx;
    memset(&fx, 0, sizeof(fx));
    fx.latI = g_nearLat;
    fx.lonI = g_nearLon;
    fx.state = SELF_ST_LIVE;
    fx.sats = 12;
    fx.hdopX10 = 8;
    uint8_t lw = 0, gw = 0;
    fx.src = SELF_PHONE;
    const int lp = tzaLookup(&fx, &lw);
    fx.src = SELF_GPS;
    const int lg = tzaLookup(&fx, &gw);
    CHECK(lp == TZ_BORDER && lw == TZA_WHY_PHONE_NEAR && lg == nz && gw == TZA_WHY_NONE,
          "tzaLookup there: a LIVE phone fix is refused, the same GPS fix answers");
  }

  // ---- the setting is not Auto: nothing is looked at
  noSlots(s);
  tzaInit(&a, EAST, SELF_GPS);
  runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, chi, 9, 9, 1000, 61000, 30000);
  CHECK(a.cand == CENT, "(Central waiting)");
  CHECK(runStream(&a, TZ_MODE_NAMED, s, SELF_GPS, chi, 9, 9, 90000, 3600000, 30000) == 0 &&
        a.why == TZA_WHY_MANUAL && a.cand == -1 && a.zone == EAST && a.lastLookup == TZA_LOOKUP_NONE,
        "a named zone chosen: an hour at Chicago changes nothing, the waiting zone is dropped");
  CHECK(runStream(&a, TZ_MODE_CUSTOM, s, SELF_GPS, chi, 9, 9, 3630000, 7200000, 30000) == 0 &&
        a.why == TZA_WHY_MANUAL && a.zone == EAST, "Custom: likewise");
  /* Back to Auto from Custom (the glue's tzaEnterAuto on every change into Auto): the zone found
   * before the override is gone, and Central must still be seen twice afresh. */
  tzaEnterAuto(&a, TZ_MODE_CUSTOM, -1);
  CHECK(a.zone == -1 && a.zoneSrc == SELF_NONE && a.cand == -1,
        "back to Auto from Custom: the zone found before (Eastern) is NOT kept - the typed offset");
  CHECK(!tzaStep(&a, TZ_MODE_AUTO, s, 7200000) && a.why == TZA_WHY_WAITING && a.cand == CENT && a.zone == -1,
        "...and Central must be seen twice afresh");
  CHECK(runStream(&a, TZ_MODE_AUTO, s, SELF_GPS, chi, 9, 9, 7230000, 7200000 + TZA_CONFIRM_MS, 30000) == 1 &&
        a.zone == CENT && a.zoneSrc == SELF_GPS, "...2 min later: Central, from GPS (and its banner)");
  CHECK(!tzaStep(NULL, TZ_MODE_AUTO, s, 1) && !tzaDecide(NULL, EAST, 0, SELF_GPS, 1), "NULL state");
  tzaForget(NULL);
  tzaEnterAuto(NULL, TZ_MODE_NAMED, 0);
  testBackToAuto(EAST, CENT, PAC, chi, det, sac);
  tzaInit(&a, -1, SELF_NONE);
  CHECK(!tzaDecide(&a, zoneOf("America/Havana"), TZA_WHY_NONE, SELF_GPS, 1) &&
        a.why == TZA_WHY_UNKNOWN_RULE && a.cand == -1, "a zone with unmodelled rules is never taken");
  // millis() wrapping between the two looks
  noSlots(s);
  tzaInit(&a, EAST, SELF_GPS);
  const uint32_t w0 = 0xFFFFFFFFu - 60000u;
  setFix(s, SELF_GPS, chi->lat, chi->lon, 9, 9, w0);
  tzaStep(&a, TZ_MODE_AUTO, s, w0);
  setFix(s, SELF_GPS, chi->lat, chi->lon, 9, 9, w0 + 30000u);
  tzaStep(&a, TZ_MODE_AUTO, s, w0 + 30000u);
  const uint32_t w1 = w0 + (uint32_t)TZA_CONFIRM_MS;               // past the wrap
  setFix(s, SELF_GPS, chi->lat, chi->lon, 9, 9, w1);
  CHECK(w1 < w0 && tzaStep(&a, TZ_MODE_AUTO, s, w1) && a.zone == CENT,
        "millis() wrapping between the two looks: still 2 minutes");
}

/* ── the offset ────────────────────────────────────────────────────────────────────────────── */
static void testOffset() {
  printf("the offset\n");
  // Every zoneinfo offset in the vectors, through a NAMED zone and through an Auto zone.
  int namedN = 0, autoN = 0, bad = 0;
  for (size_t i = 0; i < sizeof(TZV_OFFSETS) / sizeof(TZV_OFFSETS[0]); i++) {
    const TzvOffset& v = TZV_OFFSETS[i];
    const int z = tzGridFindZone(v.iana);
    const TzZone* zz = tzGridZone(z);
    if (!zz || (zz->rule != TZ_RULE_NONE && zz->rule != TZ_RULE_USCAN)) continue;
    TzEff e;
    tzEffective(TZ_MODE_AUTO, z, z, -420, true, v.utc, &e);
    if (zz->sinceUtc && v.utc < zz->sinceUtc) {
      CHECKQ(!e.ok, "%s at %lld: before its sinceUtc, not ok", v.iana, (long long)v.utc);
      continue;
    }
    autoN++;
    CHECKQ(e.kind == TZK_AUTO && e.ok && e.minutes == v.minutes, "auto %s at %lld: %d, zoneinfo %d",
           v.iana, (long long)v.utc, e.minutes, v.minutes);
    if (!(e.ok && e.minutes == v.minutes)) bad++;
    const int n = tzNamedFind(v.iana);
    if (n >= 0) {
      namedN++;
      tzEffective(TZ_MODE_NAMED, n, -1, -420, true, v.utc, &e);
      CHECKQ(e.kind == TZK_NAMED && e.ok && e.minutes == v.minutes, "named %s at %lld: %d, zoneinfo %d",
             v.iana, (long long)v.utc, e.minutes, v.minutes);
      if (!(e.ok && e.minutes == v.minutes)) bad++;
    }
  }
  char line[160];
  snprintf(line, sizeof(line), "%d Auto and %d named offsets = zoneinfo's, every DST change "
           "2026-2030 a second before, at and after", autoN, namedN);
  CHECK(bad == 0 && autoN > 1000 && namedN > 50, line);

  // The next change, for the words and the serial `tz`.
  int nextN = 0;
  bad = 0;
  for (size_t i = 0; i < sizeof(TZV_NEXT) / sizeof(TZV_NEXT[0]); i++) {
    const TzvNext& v = TZV_NEXT[i];
    const int n = tzNamedFind(v.iana);
    if (n < 0) continue;
    TzEff e;
    tzEffective(TZ_MODE_NAMED, n, -1, 0, true, v.utc, &e);
    nextN++;
    const bool ok = e.ok && e.next == v.next && (v.next == 0 || e.nextMinutes == v.after);
    if (!ok) bad++;
    CHECKQ(ok, "named %s at %lld: next %lld/%d, zoneinfo %lld/%d", v.iana, (long long)v.utc,
           (long long)e.next, e.nextMinutes, (long long)v.next, v.after);
  }
  snprintf(line, sizeof(line), "%d next changes of the named zones = zoneinfo's", nextN);
  CHECK(bad == 0 && nextN > 30, line);

  // THE 1 Hz GLUE, simulated across the autumn 2026 change and the spring 2027 one: the applied
  // offset follows by itself, changes exactly once, at the second zoneinfo says.
  const int eastern = tzNamedFind("eastern");
  const int64_t fall = 1793512800LL;     // 2026-11-01 06:00 UTC = 02:00 EDT -> 01:00 EST
  const int64_t spring = 1805007600LL;   // 2027-03-14 07:00 UTC = 02:00 EST -> 03:00 EDT
  bool vecHas = false;
  for (size_t i = 0; i < sizeof(TZV_NEXT) / sizeof(TZV_NEXT[0]); i++) {
    if (!strcmp(TZV_NEXT[i].iana, "America/New_York") && (TZV_NEXT[i].next == fall)) vecHas = true;
  }
  CHECK(vecHas, "(the vectors' New York change is 2026-11-01 06:00 UTC)");
  const int64_t starts[2] = { fall, spring };
  const int before[2] = { -240, -300 }, after[2] = { -300, -240 };
  for (int k = 0; k < 2; k++) {
    int applied = -9999, changes = 0;
    bool right = true;
    for (int64_t u = starts[k] - 3 * 3600; u <= starts[k] + 3 * 3600; u++) {
      TzEff e;
      tzEffective(TZ_MODE_NAMED, eastern, -1, -420, true, u, &e);
      if (e.ok && e.minutes != applied) {
        if (applied != -9999) changes++;
        applied = e.minutes;
      }
      if (applied != (u < starts[k] ? before[k] : after[k])) right = false;
    }
    CHECK(right && changes == 1, k == 0 ? "a 1 Hz re-evaluation: EDT -> EST at 2026-11-01 06:00:00 UTC, once"
                                        : "...and EST -> EDT at 2027-03-14 07:00:00 UTC, once");
  }

  // REBOOT MEMORY: the zone saved by name, no fix since boot, and DST still applies.
  {
    TzAuto a;
    tzaInit(&a, tzaZoneFromSaved("America/New_York"), SELF_PHONE);
    SelfSlot s[SELF_SRC_COUNT];
    noSlots(s);
    tzaStep(&a, TZ_MODE_AUTO, s, 5000);
    TzEff e1, e2;
    tzEffective(TZ_MODE_AUTO, -1, a.zone, -420, true, fall - 1, &e1);
    tzEffective(TZ_MODE_AUTO, -1, a.zone, -420, true, fall, &e2);
    CHECK(a.zone >= 0 && a.why == TZA_WHY_NO_FIX && e1.kind == TZK_AUTO && e1.minutes == -240 &&
          e2.minutes == -300 && e1.next == fall && e1.nextMinutes == -300,
          "reboot with no fix: the saved zone (by name) still changes its clock at DST");
  }

  // The clock not known: a DST zone waits; a typed offset and a no-DST zone do not.
  TzEff e;
  tzEffective(TZ_MODE_NAMED, eastern, -1, -420, false, 0, &e);
  CHECK(!e.ok && e.kind == TZK_NAMED && !strcmp(e.name, "Eastern"), "Eastern with no clock: not ok (date unknown)");
  tzEffective(TZ_MODE_NAMED, tzNamedFind("arizona"), -1, -420, false, 0, &e);
  CHECK(e.ok && e.minutes == -420 && !e.next, "Arizona with no clock: UTC-7, no change ever");
  tzEffective(TZ_MODE_AUTO, -1, zoneOf("Pacific/Honolulu"), -420, false, 0, &e);
  CHECK(e.ok && e.minutes == -600, "an Auto no-DST zone with no clock: its offset");
  tzEffective(TZ_MODE_AUTO, -1, zoneOf("America/Vancouver"), -420, false, 0, &e);
  CHECK(!e.ok, "Vancouver (UTC-7 only since 2026-03-08) with no clock: not ok");
  tzEffective(TZ_MODE_CUSTOM, eastern, zoneOf("America/Chicago"), -420, false, 0, &e);
  CHECK(e.ok && e.kind == TZK_CUSTOM && e.minutes == -420 && !e.name,
        "Custom: the typed offset, whatever zone is remembered or named");
  tzEffective(TZ_MODE_AUTO, eastern, -1, -210, true, fall, &e);
  CHECK(e.ok && e.kind == TZK_AUTO_WAIT && e.minutes == -210, "Auto, no zone yet: the typed offset");
  tzEffective(TZ_MODE_CUSTOM, -1, -1, 900, true, fall, &e);
  CHECK(!e.ok, "a typed offset out of range is not applied");
  tzEffective(TZ_MODE_NAMED, 42, -1, -480, true, fall, &e);
  CHECK(e.ok && e.kind == TZK_CUSTOM && e.minutes == -480, "a corrupt named index: the typed offset");
  tzEffective(TZ_MODE_AUTO, -1, zoneOf("America/Nuuk"), -480, true, fall, &e);
  CHECK(e.kind == TZK_AUTO_WAIT && e.minutes == -480, "an unmodelled Auto zone: the typed offset");
}

/* ── the words ─────────────────────────────────────────────────────────────────────────────── */
static void testWords() {
  printf("the words\n");
  fontLoad(&g_f16, Akrobat_Bold16);
  CHECK(g_f16.ok && g_f16.n > 90, "the firmware's Akrobat_Bold16 glyph table loads");
  char b[96];
  tzFmtOffset(-240, b, sizeof(b));
  bool f = !strcmp(b, "UTC-4");
  tzFmtOffset(330, b, sizeof(b));
  f = f && !strcmp(b, "UTC+5:30");
  tzFmtOffset(-210, b, sizeof(b));
  f = f && !strcmp(b, "UTC-3:30");
  tzFmtOffset(0, b, sizeof(b));
  f = f && !strcmp(b, "UTC");
  tzFmtOffset(840, b, sizeof(b));
  f = f && !strcmp(b, "UTC+14");
  CHECK(f, "offsets: UTC-4, UTC+5:30, UTC-3:30, UTC, UTC+14");

  const int64_t oct10 = 1791676800LL;    // 2026-10-11 00:00 UTC (EDT, PDT)
  TzEff e;
  tzEffective(TZ_MODE_AUTO, -1, zoneOf("America/New_York"), -420, true, oct10, &e);
  tzStatusText(&e, SELF_PHONE, b, sizeof(b));
  CHECK(!strcmp(b, "Eastern, UTC-4, from phone GPS"), "\"Eastern, UTC-4, from phone GPS\"");
  tzStatusText(&e, SELF_GPS, b, sizeof(b));
  CHECK(!strcmp(b, "Eastern, UTC-4, from GPS"), "\"Eastern, UTC-4, from GPS\"");
  tzEffective(TZ_MODE_NAMED, tzNamedFind("pacific"), -1, -420, true, oct10, &e);
  tzStatusText(&e, SELF_NONE, b, sizeof(b));
  CHECK(!strcmp(b, "Pacific, UTC-7, set by hand"), "\"Pacific, UTC-7, set by hand\"");
  tzNextText(&e, b, sizeof(b));
  CHECK(!strcmp(b, "DST ends Sun Nov 1 (UTC-8)"), "\"DST ends Sun Nov 1 (UTC-8)\"");
  tzEffective(TZ_MODE_CUSTOM, -1, -1, -420, true, oct10, &e);
  tzStatusText(&e, SELF_NONE, b, sizeof(b));
  CHECK(!strcmp(b, "UTC-7 custom"), "\"UTC-7 custom\"");
  tzEffective(TZ_MODE_AUTO, -1, -1, -420, true, oct10, &e);
  tzStatusText(&e, SELF_NONE, b, sizeof(b));
  CHECK(!strcmp(b, "No zone yet: UTC-7 (typed)"), "\"No zone yet: UTC-7 (typed)\"");
  {
    char z[64], src[64];
    tzEffective(TZ_MODE_AUTO, -1, zoneOf("America/Phoenix"), -420, true, oct10, &e);
    tzZoneText(&e, z, sizeof(z));
    tzSourceText(&e, SELF_PHONE, src, sizeof(src));
    CHECK(!strcmp(z, "Mountain, no DST, UTC-7") && !strcmp(src, "From phone GPS"),
          "Settings' two lines: \"Mountain, no DST, UTC-7\" / \"From phone GPS\"");
    tzEffective(TZ_MODE_NAMED, tzNamedFind("newfoundland"), -1, -420, true, oct10, &e);
    tzZoneText(&e, z, sizeof(z));
    tzSourceText(&e, SELF_NONE, src, sizeof(src));
    CHECK(!strcmp(z, "Newfoundland, UTC-2:30") && !strcmp(src, "Set by hand"),
          "\"Newfoundland, UTC-2:30\" / \"Set by hand\"");
    tzEffective(TZ_MODE_AUTO, -1, -1, -420, true, oct10, &e);
    tzZoneText(&e, z, sizeof(z));
    tzSourceText(&e, SELF_NONE, src, sizeof(src));
    CHECK(!strcmp(z, "No zone yet: UTC-7") && !strcmp(src, "Typed offset until a fix"),
          "\"No zone yet: UTC-7\" / \"Typed offset until a fix\"");
    tzEffective(TZ_MODE_NAMED, tzNamedFind("eastern"), -1, -420, false, 0, &e);
    tzZoneText(&e, z, sizeof(z));
    CHECK(!strcmp(z, "Eastern: clock not set"), "\"Eastern: clock not set\"");
  }
  tzEffective(TZ_MODE_NAMED, tzNamedFind("eastern"), -1, -420, true, 1793512800LL + 86400, &e);
  tzNextText(&e, b, sizeof(b));
  CHECK(!strcmp(b, "DST starts Sun Mar 14 (UTC-4)"), "after November: \"DST starts Sun Mar 14 (UTC-4)\"");
  tzChoiceHint(tzChoiceOf(TZ_MODE_NAMED, tzNamedFind("eastern")), true, oct10, b, sizeof(b));
  CHECK(!strcmp(b, "UTC-4 now, DST by itself"), "hint: \"UTC-4 now, DST by itself\"");
  tzChoiceHint(tzChoiceOf(TZ_MODE_NAMED, tzNamedFind("newfoundland")), false, 0, b, sizeof(b));
  CHECK(!strcmp(b, "UTC-3:30 or -2:30, DST by itself"), "hint, no clock: \"UTC-3:30 or -2:30, DST by itself\"");
  tzChoiceHint(tzChoiceOf(TZ_MODE_NAMED, tzNamedFind("arizona")), true, oct10, b, sizeof(b));
  CHECK(!strcmp(b, "UTC-7 all year, no DST"), "hint: \"UTC-7 all year, no DST\"");

  // Every text Settings can show fits its 228 px line.
  bool all = true;
  int64_t instants[3] = { oct10, 1793512800LL + 86400, 1800000000LL };
  for (int z = 0; z < tzGridZoneCount(); z++) {
    for (int k = 0; k < 3; k++) {
      tzEffective(TZ_MODE_AUTO, -1, z, -420, true, instants[k], &e);
      tzZoneText(&e, b, sizeof(b));
      all = fits(b, "zone") && all;
      const int srcs[] = { SELF_NONE, SELF_GPS, SELF_PHONE, TZA_SRC_HAND };
      for (size_t k2 = 0; k2 < sizeof(srcs) / sizeof(srcs[0]); k2++) {
        tzSourceText(&e, srcs[k2], b, sizeof(b));
        all = fits(b, "source") && all;
      }
      tzNextText(&e, b, sizeof(b));
      if (*b) all = fits(b, "next") && all;
    }
    tzEffective(TZ_MODE_AUTO, -1, z, -420, false, 0, &e);
    tzZoneText(&e, b, sizeof(b));
    all = fits(b, "zone, no clock") && all;
  }
  for (int n = 0; n < TZ_NAMED_COUNT; n++) {
    for (int k = 0; k < 3; k++) {
      tzEffective(TZ_MODE_NAMED, n, -1, 0, true, instants[k], &e);
      tzZoneText(&e, b, sizeof(b));
      all = fits(b, "named") && all;
      tzSourceText(&e, SELF_NONE, b, sizeof(b));
      all = fits(b, "named source") && all;
      tzNextText(&e, b, sizeof(b));
      all = fits(b, "named next") && all;
    }
    tzEffective(TZ_MODE_NAMED, n, -1, 0, false, 0, &e);
    tzZoneText(&e, b, sizeof(b));
    all = fits(b, "named, no clock") && all;
  }
  for (int m = TZ_CUSTOM_MIN_MINUTES; m <= TZ_CUSTOM_MAX_MINUTES + 15; m += 15) {
    for (int mode = TZ_MODE_AUTO; mode <= TZ_MODE_CUSTOM; mode += 2) {
      tzEffective((uint8_t)mode, -1, -1, m, true, oct10, &e);
      tzZoneText(&e, b, sizeof(b));
      all = fits(b, "typed") && all;
      tzSourceText(&e, SELF_NONE, b, sizeof(b));
      all = fits(b, "typed source") && all;
      tzNextText(&e, b, sizeof(b));
      all = fits(b, "typed next") && all;
    }
  }
  TzAuto a;
  tzaInit(&a, -1, SELF_NONE);
  for (int w = TZA_WHY_NONE; w <= TZA_WHY_SWITCHED; w++) {
    for (int z = -1; z < tzGridZoneCount(); z++) {
      a.why = (uint8_t)w;
      a.cand = (int16_t)z;
      a.zone = (int16_t)z;
      a.lastSrc = (uint8_t)(z % 2 ? SELF_GPS : SELF_PHONE);
      tzaWhyText(&a, b, sizeof(b));
      all = fits(b, "auto line") && all;
      if (strcmp(tzaWhyName(w), "?") == 0) all = false;
    }
  }
  for (int c = 0; c < TZ_CHOICE_COUNT; c++) {
    tzChoiceHint(c, true, oct10, b, sizeof(b));
    all = fits(b, "hint") && all;
    tzChoiceHint(c, false, 0, b, sizeof(b));
    all = fits(b, "hint, no clock") && all;
    all = fits(tzChoiceName(c), "choice") && all;
  }
  snprintf(b, sizeof(b), "every Settings line fits 228 px of Akrobat Bold 16 (widest %d px: %s)",
           g_widest, g_widestText);
  CHECK(all, b);
  // The banner: title in Akrobat Bold 18, body in Bold 16, each on 212 px (GUI::showMeshPopup).
  {
    Font7 f18;
    fontLoad(&f18, Akrobat_Bold18);
    bool nOk = f18.ok;
    int wT = 0, wB = 0;
    char title[40], body[48];
    for (int z = 0; z < tzGridZoneCount(); z++) {
      for (int src = SELF_GPS; src <= SELF_PHONE; src++) {
        tzEffective(TZ_MODE_AUTO, -1, z, 0, true, oct10, &e);
        tzNoticeText(z, &e, src, title, sizeof(title), body, sizeof(body));
        bool miss = false;
        const int t1 = textWidth(&f18, title, &miss), b1 = textWidth(&g_f16, body, &miss);
        if (t1 > wT) wT = t1;
        if (b1 > wB) wB = b1;
        if (miss || t1 > 212 || b1 > 212) {
          printf("    banner \"%s\" / \"%s\": %d / %d px\n", title, body, t1, b1);
          nOk = false;
        }
      }
    }
    tzEffective(TZ_MODE_AUTO, -1, zoneOf("America/New_York"), 0, true, oct10, &e);
    tzNoticeText(zoneOf("America/New_York"), &e, SELF_PHONE, title, sizeof(title), body, sizeof(body));
    nOk = nOk && !strcmp(title, "Time zone: Eastern") && !strcmp(body, "Now UTC-4, from phone GPS");
    tzEffective(TZ_MODE_AUTO, -1, zoneOf("America/New_York"), 0, false, 0, &e);
    tzNoticeText(zoneOf("America/New_York"), &e, SELF_PHONE, title, sizeof(title), body, sizeof(body));
    nOk = nOk && !strcmp(body, "Clock not set yet");
    char m[128];
    snprintf(m, sizeof(m), "the zone-change banner: \"Time zone: Eastern\" / \"Now UTC-4, from phone GPS\"; "
             "all fit 212 px (widest %d / %d)", wT, wB);
    CHECK(nOk, m);
  }
  char small[8];
  CHECK(tzStatusText(&e, SELF_NONE, small, sizeof(small)) == 7 && strlen(small) == 7,
        "a short buffer is cut, never overrun");
}

int main() {
  printf("test_tzauto\n");
  testSetting();
  testPick();
  testUartGps();
  testMargin();
  testDecide();
  testOffset();
  testWords();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
