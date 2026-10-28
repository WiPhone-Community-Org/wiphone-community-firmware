/*
 * test_almanac_lines.cpp - WiPhone/almanac_lines.cpp on the host: every wording the Almanac's
 * screens use (and the serial `almanac` prints), the rounding rules that make legal light a
 * legal question, the countdown's cases, the US daylight-saving reminder, the order rows come in
 * - and EVERY display row measured against the phone's own font, the way SmoothFont measures it
 * (AKROBAT_BOLD_20, the Almanac's menu face, drawn from x = 8 across 240 px: 232 px).
 *
 * The astronomy itself is proven against PyEphem by test_almanac.cpp; this proves what the
 * screen does with it. The width sweep runs six places (47 N, 71 N, the equator, 34 S, 78 N and
 * a +5:30 zone) over two years of days, both legal rules and both unit settings.
 */

#include "../WiPhone/almanac_lines.h"
#include "../WiPhone/astro.h"
#include "../WiPhone/elev_tiles.h"
#include "../WiPhone/menu_wrap.h"
#include "../WiPhone/units.h"
#include "../WiPhone/tz_grid.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

#define CHECKQ(cond, ...) do { \
    checks++; \
    if (!(cond)) { printf("  FAIL "); printf(__VA_ARGS__); printf(" (line %d)\n", __LINE__); failures++; } \
  } while (0)

static bool is(const char* got, const char* want) {
  if (strcmp(got, want) != 0) {
    printf("    got \"%s\", want \"%s\"\n", got, want);
    return false;
  }
  return true;
}

// ── the phone's font, as SmoothFont reads and measures it (see test_elevtext.cpp) ────────────

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
  if (data[0] != '7' || data[1] != 'S' || data[2] != 'F') {
    return;
  }
  const unsigned char* p = data + 3;
  f->n = (int)runDecode(&p);
  runDecode(&p);
  const int ascent = (int)runDecode(&p);
  const int descent = (int)runDecode(&p);
  if (f->n <= 0 || f->n > 256) {
    return;
  }
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
    if (f->uni[i] == u) {
      *g = i;
      return true;
    }
  }
  return false;
}

static int textWidth(const Font7* f, const char* s) {
  int w = 0;
  for (const char* p = s; *p; p++) {
    const uint16_t u = (unsigned char)*p;
    if (u == 0x20) {
      w += f->space;
      continue;
    }
    int g = 0;
    if (fontGlyph(f, u, &g)) {
      if (w == 0 && f->dx[g] < 0) {
        w -= f->dx[g];
      }
      w += p[1] ? f->adv[g] : (f->dx[g] + f->w[g]);
    } else {
      w += f->space + 1;
    }
  }
  return w;
}

static Font7 g_font;
static const int ROW_W = 240 - 8;        // MenuWidget draws a row from leftOffset 8: maxW 232

// ── collecting a screen ────────────────────────────────────────────────────────────────────

struct Rows {
  int  n;
  int  kind[64];
  char text[64][100];
};

static void collect(void* ctx, int kind, const char* text) {
  Rows* r = (Rows*)ctx;
  if (r->n < 64) {
    r->kind[r->n] = kind;
    snprintf(r->text[r->n], sizeof(r->text[0]), "%s", text);
    r->n++;
  }
}

static int find(const Rows& r, const char* prefix) {
  for (int i = 0; i < r.n; i++) {
    if (strncmp(r.text[i], prefix, strlen(prefix)) == 0) {
      return i;
    }
  }
  return -1;
}

static void dump(const char* title, const Rows& r) {
  printf("    -- %s --\n", title);
  for (int i = 0; i < r.n; i++) {
    printf("    %3d  %s%s\n", r.kind[i], r.text[i], r.kind[i] == ALM_ROW_WRAP ? "   [wrap]" : "");
  }
}

/* A wrapped sentence as the app breaks it (app_almanac.cpp: wrapNote at ALM_WRAP_W = 220 px of
 * the same face, at most 6 rows): every piece fits, and together they are the whole sentence. */
static const int WRAP_W = 220;
struct WrapAcc { int rows; bool fit; char joined[160]; };
static size_t wrapFit(const char* s, void* ctx) {
  size_t n = strlen(s), best = 0;
  char buf[160];
  for (size_t k = 1; k <= n && k < sizeof(buf); k++) {
    memcpy(buf, s, k);
    buf[k] = '\0';
    if (textWidth(&g_font, buf) <= WRAP_W) best = k;
  }
  return best;
}
static void wrapRow(const char* s, size_t len, void* ctx) {
  WrapAcc* a = (WrapAcc*)ctx;
  char buf[160];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, s, len);
  buf[len] = '\0';
  if (textWidth(&g_font, buf) > WRAP_W) a->fit = false;
  if (a->rows) strncat(a->joined, " ", sizeof(a->joined) - strlen(a->joined) - 1);
  strncat(a->joined, buf, sizeof(a->joined) - strlen(a->joined) - 1);
  a->rows++;
}
static bool wrapsWhole(const char* text, int* rowsOut = NULL) {
  WrapAcc a;
  memset(&a, 0, sizeof(a));
  a.fit = true;
  wrapNote(text, wrapFit, wrapRow, &a, 6);
  // wrapNote drops the spaces it breaks at; a word cut mid-way (no space) joins with one - compare
  // with every space removed.
  char x[160], y[160];
  size_t nx = 0, ny = 0;
  for (const char* p = text; *p && nx < sizeof(x) - 1; p++) if (*p != ' ') x[nx++] = *p;
  for (const char* p = a.joined; *p && ny < sizeof(y) - 1; p++) if (*p != ' ') y[ny++] = *p;
  x[nx] = y[ny] = '\0';
  if (rowsOut) *rowsOut = a.rows;
  return a.fit && a.rows <= 6 && strcmp(x, y) == 0;
}

// Every row a screen shows as ONE row must fit; a wrapped sentence must wrap whole into rows that
// fit; every character must have a glyph.
static int g_widest = 0;
static char g_widestText[100];
static bool fitsAll(const Rows& r, const char* where) {
  bool ok = true;
  for (int i = 0; i < r.n; i++) {
    for (const char* p = r.text[i]; *p; p++) {
      int g = 0;
      if (*p != ' ' && !fontGlyph(&g_font, (unsigned char)*p, &g)) {
        printf("    no glyph for '%c' in \"%s\" (%s)\n", *p, r.text[i], where);
        ok = false;
      }
    }
    if (!r.text[i][0]) {
      printf("    an empty row (%s)\n", where);
      ok = false;
    }
    if (r.kind[i] == ALM_ROW_WRAP || r.kind[i] == ALM_ROW_WARN || r.kind[i] == ALM_ROW_DANGER) {
      if (!wrapsWhole(r.text[i])) {
        printf("    \"%s\" does not wrap whole into 6 rows of %d px (%s)\n", r.text[i], WRAP_W, where);
        ok = false;
      }
      continue;
    }
    const int w = textWidth(&g_font, r.text[i]);
    if (w > g_widest) {
      g_widest = w;
      snprintf(g_widestText, sizeof(g_widestText), "%s", r.text[i]);
    }
    if (w > ROW_W) {
      printf("    \"%s\" is %d px, a row has %d (%s)\n", r.text[i], w, ROW_W, where);
      ok = false;
    }
  }
  return ok;
}

// A context over one day; the AlmDay lives with the caller.
static AlmCtx ctxFor(AlmDay* day, int64_t now, int tzS, int off, double lat, double lon,
                     int rule, int units, int kind = ALM_PLACE_GPS, const char* name = "GPS") {
  AlmCtx c;
  memset(&c, 0, sizeof(c));
  c.clockKnown = true;
  c.now = now;
  c.tzS = tzS;
  c.clockSrc = "ntp";
  c.dayOffset = off;
  c.placeKind = kind;
  c.placeName = name;
  c.lat = lat;
  c.lon = lon;
  c.rule = rule;
  c.units = units;
  c.usDst = true;                  // the phone's default
  if (day) {
    // The day in ITS offset (almDayTzS: today's, or the other side of a US clock change's).
    almDayCompute(day, almDayT0(&c), almDayTzS(&c), lat, lon);
    almDayEnsure(day, ALM_NEED_PREV | ALM_NEED_NEXT | ALM_NEED_PHASES, almPhaseFrom(&c));
  }
  c.day = day;
  return c;
}

/* Every field of two AlmDays (field by field: memcmp would compare the padding). */
static bool sameDay(const AlmDay& a, const AlmDay& b) {
  bool ok = a.valid == b.valid && a.have == b.have && a.t0 == b.t0 && a.tzS == b.tzS &&
            a.lat == b.lat && a.lon == b.lon &&
            !memcmp(&a.sun, &b.sun, sizeof(a.sun)) && !memcmp(&a.moon, &b.moon, sizeof(a.moon)) &&
            a.sol.nMajor == b.sol.nMajor && a.sol.nMinor == b.sol.nMinor &&
            a.sol.rating == b.sol.rating && a.havePrev == b.havePrev && a.haveNext == b.haveNext &&
            a.havePhases == b.havePhases;
  for (int k = 0; ok && k < 2; k++) {
    ok = (k >= a.sol.nMajor || a.sol.majorMid[k] == b.sol.majorMid[k]) &&
         (k >= a.sol.nMinor || a.sol.minorMid[k] == b.sol.minorMid[k]);
  }
  if (ok && a.havePrev) ok = !memcmp(&a.prev, &b.prev, sizeof(a.prev));
  if (ok && a.haveNext) ok = !memcmp(&a.next, &b.next, sizeof(a.next));
  if (ok && a.havePhases) ok = a.phaseFrom == b.phaseFrom && !memcmp(a.phase, b.phase, sizeof(a.phase));
  return ok;
}

// more(): stop after a random number of units (a slice cut anywhere), deterministic by seed.
struct Cutter { unsigned seed; int left; int asked; };
static bool cutMore(void* ctx) {
  Cutter* k = (Cutter*)ctx;
  k->asked++;
  if (k->left-- > 0) return true;
  k->seed = k->seed * 1103515245u + 12345u;
  k->left = (int)((k->seed >> 16) % 50);
  return false;
}
static bool alwaysMore(void* ctx) {
  if (ctx) ((Cutter*)ctx)->asked++;
  return true;
}

static int64_t unixOf(int y, int m, int d, int hh, int mm, int ss, int tzS) {
  return astroDaysFromCivil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss - tzS;
}

/* A context whose clock follows a time zone with rules (0.9.82, Settings > Time zone), the day
 * computed in ITS offset by that rule - as app_almanac.cpp's almCtxZone fills it. */
static AlmCtx zonedFor(AlmDay* day, int64_t now, int tzS, int off, double lat, double lon,
                       int stdS, int tzRule, const char* name) {
  AlmCtx c = ctxFor(NULL, now, tzS, off, lat, lon, ASTRO_LEGAL_30MIN, UNITS_METRIC);
  c.tzZoned = true;
  c.tzStdS = stdS;
  c.tzRule = tzRule;
  c.tzName = name;
  if (day) {
    almDayCompute(day, almDayT0(&c), almDayTzS(&c), lat, lon);
    almDayEnsure(day, ALM_NEED_PREV | ALM_NEED_NEXT | ALM_NEED_PHASES, almPhaseFrom(&c));
  }
  c.day = day;
  return c;
}

int main() {
  printf("test_almanac_lines\n");

  fontLoad(&g_font, Akrobat_Bold20);
  CHECK(g_font.ok && g_font.n > 90, "the firmware's Akrobat_Bold20 glyph table loads");
  {
    int g = 0;
    CHECK(!fontGlyph(&g_font, 0xB0, &g), "Akrobat_Bold20 has NO degree glyph: no row may use one");
  }

  const int PDT = -7 * 3600;
  char s[64];

  // ── the formatters ─────────────────────────────────────────────────────────────────────
  {
    // 2026-09-27 06:33:20 PDT
    const int64_t t = unixOf(2026, 9, 27, 6, 33, 20, PDT);
    almFmtClock(t, PDT, ALM_ROUND_NEAREST, s, sizeof(s));
    CHECK(is(s, "06:33"), "clock: 06:33:20 to the nearest minute is 06:33");
    almFmtClock(t, PDT, ALM_ROUND_UP, s, sizeof(s));
    CHECK(is(s, "06:34"), "clock: first legal light rounds UP (06:33:20 -> 06:34)");
    almFmtClock(t, PDT, ALM_ROUND_DOWN, s, sizeof(s));
    CHECK(is(s, "06:33"), "clock: last legal light rounds DOWN");
    almFmtClock(t - 20, PDT, ALM_ROUND_UP, s, sizeof(s));
    CHECK(is(s, "06:33"), "clock: a whole minute rounds up to itself");
    almFmtClock(unixOf(2026, 9, 27, 6, 33, 30, PDT), PDT, ALM_ROUND_NEAREST, s, sizeof(s));
    CHECK(is(s, "06:34"), "clock: half a minute rounds up to the nearest");
    almFmtClock(unixOf(2026, 9, 27, 23, 59, 45, PDT), PDT, ALM_ROUND_NEAREST, s, sizeof(s));
    CHECK(is(s, "00:00"), "clock: 23:59:45 wraps to 00:00");
    almFmtClock(0, PDT, ALM_ROUND_NEAREST, s, sizeof(s));
    CHECK(is(s, "--:--"), "clock: no event is --:--");
    almFmtClock(unixOf(2026, 9, 27, 19, 5, 0, 19800), 19800, ALM_ROUND_NEAREST, s, sizeof(s));
    CHECK(is(s, "19:05"), "clock: a +5:30 zone");
    almFmtClock(t, PDT, ALM_ROUND_NEAREST, s, 5);
    CHECK(is(s, ""), "clock: a buffer too small writes \"\", never \"06:3\"");

    almFmtSpan(3 * 3600 + 12 * 60 + 59, ALM_ROUND_DOWN, s, sizeof(s));
    CHECK(is(s, "3h 12m"), "span: 3h 12m 59s left rounds DOWN to 3h 12m");
    almFmtSpan(5 * 3600 + 1 * 60 + 1, ALM_ROUND_UP, s, sizeof(s));
    CHECK(is(s, "5h 02m"), "span: 5h 01m 01s to go rounds UP to 5h 02m");
    almFmtSpan(42 * 60, ALM_ROUND_NEAREST, s, sizeof(s));
    CHECK(is(s, "42m"), "span: under an hour has no hours");
    almFmtSpan(11 * 3600 + 55 * 60 + 29, ALM_ROUND_NEAREST, s, sizeof(s));
    CHECK(is(s, "11h 55m"), "span: day length to the nearest minute");
    almFmtSpan(-5, ALM_ROUND_NEAREST, s, sizeof(s));
    CHECK(is(s, "0m"), "span: negative reads as 0");

    almFmtDelta(171, s, sizeof(s));
    CHECK(is(s, "+2m 51s"), "delta: +2m 51s");
    almFmtDelta(-120, s, sizeof(s));
    CHECK(is(s, "-2m"), "delta: a whole minute is -2m");
    almFmtDelta(-185, s, sizeof(s));
    CHECK(is(s, "-3m 05s"), "delta: seconds zero-padded");
    almFmtDelta(51, s, sizeof(s));
    CHECK(is(s, "+51s"), "delta: under a minute");
    almFmtDelta(0, s, sizeof(s));
    CHECK(is(s, "same"), "delta: zero");

    almFmtDate(unixOf(2026, 9, 27, 12, 0, 0, PDT), PDT, s, sizeof(s));
    CHECK(is(s, "Sun Sep 27"), "date: Sun Sep 27");
    almFmtDate(unixOf(2026, 9, 27, 23, 30, 0, PDT), PDT, s, sizeof(s));
    CHECK(is(s, "Sun Sep 27"), "date: 23:30 PDT is still the 27th (06:30 UTC the 28th)");

    almFmtUtcOffset(PDT, s, sizeof(s));
    CHECK(is(s, "UTC-7"), "offset: UTC-7");
    almFmtUtcOffset(19800, s, sizeof(s));
    CHECK(is(s, "UTC+5:30"), "offset: UTC+5:30");
    almFmtUtcOffset(0, s, sizeof(s));
    CHECK(is(s, "UTC"), "offset: UTC");
    almFmtUtcOffset(-(9 * 3600 + 30 * 60), s, sizeof(s));
    CHECK(is(s, "UTC-9:30"), "offset: UTC-9:30");

    CHECK(is(almCompass16(0), "N") && is(almCompass16(359), "N") && is(almCompass16(11.2), "N"),
          "compass: north either side of 0");
    CHECK(is(almCompass16(11.25), "NNE") && is(almCompass16(150), "SSE") &&
          is(almCompass16(212), "SSW") && is(almCompass16(290), "WNW"),
          "compass: 16 points (150 SSE, 212 SSW, 290 WNW)");
    CHECK(is(almCompass16(-90), "W") && is(almCompass16(720 + 45), "NE"), "compass: wraps");

    // Local midnight, both sides of the date line and of UTC midnight.
    const int64_t now = unixOf(2026, 9, 27, 23, 30, 0, PDT);
    CHECK(almMidnight(now, PDT, 0) == unixOf(2026, 9, 27, 0, 0, 0, PDT),
          "midnight: 23:30 PDT belongs to its own local day");
    CHECK(almMidnight(now, PDT, 1) == unixOf(2026, 9, 28, 0, 0, 0, PDT), "midnight: +1 day");
    CHECK(almMidnight(now, PDT, -365) == unixOf(2025, 9, 27, 0, 0, 0, PDT), "midnight: -365 days");
    CHECK(almMidnight(unixOf(2026, 9, 27, 0, 30, 0, 36000), 36000, 0) ==
          unixOf(2026, 9, 27, 0, 0, 0, 36000), "midnight: a +10 h zone just after midnight");

    int y, m, d;
    CHECK(almDateOf(unixOf(2026, 9, 27, 0, 0, 0, 0), 0, &y, &m, &d, NULL, NULL) &&
          y == 2026 && m == 9 && d == 27, "date range: 2026 is in it");
    CHECK(!almDateOf(unixOf(2100, 1, 1, 0, 0, 0, 0), 0, &y, &m, &d, NULL, NULL),
          "date range: 2100 is refused (the old sun_times.cpp range, kept explicitly)");
    CHECK(!almDateOf(unixOf(1969, 12, 31, 12, 0, 0, 0), 0, &y, &m, &d, NULL, NULL),
          "date range: 1969 is refused");
    CHECK(almPlaceOk(90.0, 180.0) && almPlaceOk(-90.0, -180.0) && almPlaceOk(47.5, -121.8),
          "place: the globe, poles and date line included");
    CHECK(!almPlaceOk(95.0, 0) && !almPlaceOk(0, 180.5) && !almPlaceOk(NAN, 0) &&
          !almPlaceOk(0, INFINITY), "place: off the globe, NaN and inf refused");
  }

  // ── the countdown ────────────────────────────────────────────────────────────────────────
  {
    const int64_t first = unixOf(2026, 9, 27, 6, 33, 20, PDT);
    const int64_t last = unixOf(2026, 9, 27, 19, 28, 40, PDT);
    const int64_t next = unixOf(2026, 9, 28, 6, 34, 10, PDT);
    almCountdown(first - (5 * 3600 + 1 * 60 + 1), first, last, next, false, PDT, s, sizeof(s));
    CHECK(is(s, "First light in 5h 02m"), "countdown: before first light (rounded up)");
    almCountdown(last - (3 * 3600 + 12 * 60 + 59), first, last, next, false, PDT, s, sizeof(s));
    CHECK(is(s, "LEGAL LIGHT: 3h 12m left"), "countdown: legal light, time left rounded down");
    almCountdown(last - 30, first, last, next, false, PDT, s, sizeof(s));
    CHECK(is(s, "LEGAL LIGHT: under 1m left"), "countdown: the last minute says so");
    almCountdown(first, first, last, next, false, PDT, s, sizeof(s));
    CHECK(strncmp(s, "LEGAL LIGHT:", 12) == 0, "countdown: legal AT the first second");
    almCountdown(last, first, last, next, false, PDT, s, sizeof(s));
    CHECK(is(s, "Dark - first light 06:35"), "countdown: dark AT the last second; tomorrow's up");
    almCountdown(last + 3600, first, last, 0, false, PDT, s, sizeof(s));
    CHECK(is(s, "Dark - no light tomorrow"), "countdown: no first light tomorrow");
    almCountdown(first, 0, 0, 0, true, PDT, s, sizeof(s));
    CHECK(is(s, "LEGAL LIGHT all day"), "countdown: midnight sun");
    almCountdown(first, 0, 0, 0, false, PDT, s, sizeof(s));
    CHECK(is(s, "No legal light today"), "countdown: polar night");
    // The evening's end BEFORE the morning's start in one local day (a zone far from its meridian).
    almCountdown(first - 3600, last - 86400 + 7200, first, next, false, PDT, s, sizeof(s));
    CHECK(strncmp(s, "First light in", 14) == 0 || strncmp(s, "LEGAL", 5) == 0,
          "countdown: a reversed pair still answers");
    almCountdown(unixOf(2026, 9, 27, 1, 0, 0, PDT), unixOf(2026, 9, 27, 23, 0, 0, PDT),
                 unixOf(2026, 9, 27, 2, 0, 0, PDT), 0, false, PDT, s, sizeof(s));
    CHECK(is(s, "LEGAL LIGHT: 1h 00m left"), "countdown: reversed pair, inside the early window");
    almCountdown(unixOf(2026, 9, 27, 12, 0, 0, PDT), unixOf(2026, 9, 27, 23, 0, 0, PDT),
                 unixOf(2026, 9, 27, 2, 0, 0, PDT), 0, false, PDT, s, sizeof(s));
    CHECK(is(s, "First light in 11h 00m"), "countdown: reversed pair, between the windows");
    almCountdown(unixOf(2026, 9, 27, 23, 30, 0, PDT), unixOf(2026, 9, 27, 23, 0, 0, PDT),
                 unixOf(2026, 9, 27, 2, 0, 0, PDT), 0, false, PDT, s, sizeof(s));
    CHECK(is(s, "LEGAL LIGHT (no end today)"), "countdown: reversed pair, the late window");
  }

  // ── the countdown also heads the Meshtastic Sun screen (AKROBAT_EXTRABOLD_22, 232 px) ─────
  {
    static Font7 xb;
    fontLoad(&xb, Akrobat_ExtraBold22);
    const int64_t t0 = unixOf(2026, 9, 27, 0, 0, 0, PDT);
    const int64_t first = t0 + 60, last = t0 + 86400 - 60;
    const int64_t nows[4] = { t0, t0 + 120, last - 30, last };
    const int64_t fl[5][2] = { { first, last }, { 0, 0 }, { first, 0 }, { 0, last },
                               { last, first } };
    bool ok = xb.ok;
    int widest = 0;
    for (int a = 0; a < 5; a++) {
      for (int n = 0; n < 4; n++) {
        for (int nf = 0; nf < 2; nf++) {
          almCountdown(nows[n], fl[a][0], fl[a][1], nf ? t0 + 86400 + 60 : 0, n & 1, PDT, s,
                       sizeof(s));
          const int w = textWidth(&xb, s);
          widest = w > widest ? w : widest;
          if (!s[0] || w > ROW_W) {
            printf("    \"%s\" is %d px of ExtraBold22\n", s, w);
            ok = false;
          }
        }
      }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "countdown: every variant fits the Meshtastic Sun row (widest %d px)",
             widest);
    CHECK(ok, msg);
  }

  // ── the US daylight-saving reminder: NO arithmetic on an offset it cannot interpret ─────
  {
    /* Review 2026-09-27: on Nov 1 a phone already set to -8 was told "set Time offset to -9"
     * (the offset alone cannot say Pacific-done from Alaska-not-yet), Hawaii's -10 was told to
     * change in March and Arizona's -7 both times. Now the reminder never names an offset, and
     * Settings "US daylight saving: no (HI, AZ)" turns it off. 2026: Sun Nov 1; 2027: Sun Mar 14. */
    const int PST = -8 * 3600;
    CHECK(almDstReminder(unixOf(2026, 10, 15, 12, 0, 0, PDT), PDT, true, s, sizeof(s)) &&
          is(s, "US clocks go back 1 h Sun Nov 1 - set Time offset then"),
          "DST: 17 days before Nov 1 at -7: go back, set it then (no offset named)");
    CHECK(almDstReminder(unixOf(2026, 10, 11, 0, 0, 0, PDT), PDT, true, s, sizeof(s)),
          "DST: exactly 21 days before is inside");
    CHECK(!almDstReminder(unixOf(2026, 10, 10, 23, 59, 0, PDT), PDT, true, s, sizeof(s)) && !s[0],
          "DST: 22 days before is not");
    CHECK(almDstReminder(unixOf(2026, 10, 31, 23, 0, 0, PDT), PDT, true, s, sizeof(s)) &&
          is(s, "US clocks go back 1 h Sun Nov 1 - set Time offset then"), "DST: the evening before");
    CHECK(almDstReminder(unixOf(2026, 11, 1, 9, 0, 0, PST), PST, true, s, sizeof(s)) &&
          is(s, "US clocks went back 1 h at 2 AM today - check Time offset"),
          "DST: on the day at -8 (ALREADY changed): check - never 'set -9'");
    CHECK(almDstReminder(unixOf(2026, 11, 1, 9, 0, 0, PDT), PDT, true, s, sizeof(s)) &&
          is(s, "US clocks went back 1 h at 2 AM today - check Time offset"),
          "DST: on the day at -7 (not changed yet): the same words - both read true");
    CHECK(almDstReminder(unixOf(2026, 11, 1, 1, 30, 0, PDT), PDT, true, s, sizeof(s)) &&
          is(s, "US clocks go back 1 h at 2 AM today - set Time offset then"),
          "DST: on the day before 2 AM: not yet");
    CHECK(!almDstReminder(unixOf(2026, 11, 2, 8, 0, 0, PST), PST, true, s, sizeof(s)), "DST: after it, no");
    CHECK(!almDstReminder(unixOf(2026, 10, 15, 12, 0, 0, PDT), PDT, false, s, sizeof(s)) && !s[0] &&
          !almDstReminder(unixOf(2026, 11, 1, 9, 0, 0, PDT), PDT, false, s, sizeof(s)),
          "DST: 'US daylight saving: no' (Arizona's -7): no reminder, before or on the day");
    CHECK(!almDstReminder(unixOf(2026, 10, 15, 12, 0, 0, -10 * 3600), -10 * 3600, true, s, sizeof(s)),
          "DST: -10 before November is no daylight offset (Hawaii): none");
    CHECK(!almDstReminder(unixOf(2026, 10, 15, 12, 0, 0, 3600), 3600, true, s, sizeof(s)),
          "DST: a European offset: no reminder");
    CHECK(!almDstReminder(unixOf(2026, 10, 15, 12, 0, 0, -12600), -12600, true, s, sizeof(s)),
          "DST: a part-hour offset: no reminder");
    CHECK(almDstReminder(unixOf(2027, 3, 1, 12, 0, 0, PST), PST, true, s, sizeof(s)) &&
          is(s, "US clocks go forward 1 h Sun Mar 14 - set Time offset then"), "DST: March, forward");
    CHECK(almDstReminder(unixOf(2027, 3, 14, 10, 0, 0, PDT), PDT, true, s, sizeof(s)) &&
          is(s, "US clocks went forward 1 h at 2 AM today - check Time offset"),
          "DST: March 14 at -7 (already changed): check - never 'set -6'");
    CHECK(!almDstReminder(unixOf(2027, 3, 1, 12, 0, 0, -4 * 3600), -4 * 3600, true, s, sizeof(s)),
          "DST: -4 before March is no US standard offset (Atlantic)");
    CHECK(almDstReminder(unixOf(2027, 3, 1, 12, 0, 0, -10 * 3600), -10 * 3600, true, s, sizeof(s)) &&
          !almDstReminder(unixOf(2027, 3, 1, 12, 0, 0, -10 * 3600), -10 * 3600, false, s, sizeof(s)),
          "DST: -10 before March could be the Aleutians - reminded; Hawaii says 'no' in Settings");
    CHECK(!almDstReminder(unixOf(2026, 10, 15, 12, 0, 0, PDT), PDT, true, s, 20) && !s[0],
          "DST: a short buffer writes \"\"");
    // On the DATE screen, and the setting that turns it off.
    const int64_t oct15 = unixOf(2026, 10, 15, 12, 0, 0, PDT);
    AlmCtx c = ctxFor(NULL, oct15, PDT, 0, 47.4957, -121.7868, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    Rows r;
    memset(&r, 0, sizeof(r));
    almLinesDate(&c, collect, &r);
    const int dr = find(r, "US clocks go back");
    CHECK(dr > 0 && r.kind[dr] == ALM_ROW_WRAP, "DST: DATE carries it (a wrapped row)");
    c.usDst = false;
    memset(&r, 0, sizeof(r));
    almLinesDate(&c, collect, &r);
    CHECK(find(r, "US clocks") < 0, "DST: ...and not with the setting off");
  }

  // ── a day stepped across a US clock change is given in THAT day's offset (review B2) ─────
  {
    const int PST = -8 * 3600;
    const double LAT = 47.4957, LON = -121.7868;
    const int64_t oct27 = unixOf(2026, 10, 27, 12, 0, 0, PDT);
    int64_t ch = 0;
    CHECK(almDayTz(oct27, PDT, 11, true, &ch) == PST && ch == astroDaysFromCivil(2026, 11, 1),
          "shift: Oct 27 (-7) -> Nov 7 is in -8, across Sun Nov 1");
    CHECK(almDayTz(oct27, PDT, 5, true, NULL) == PST, "shift: the change day itself is on the new side");
    CHECK(almDayTz(oct27, PDT, 4, true, &ch) == PDT && ch == 0, "shift: Oct 31 is not across");
    CHECK(almDayTz(oct27, PDT, 0, true, NULL) == PDT, "shift: today, never");
    CHECK(almDayTz(oct27, PDT, 150, true, NULL) == PDT, "shift: both changes crossed (late March): none");
    CHECK(almDayTz(oct27, PDT, 11, false, NULL) == PDT, "shift: 'US daylight saving: no' - never");
    const int64_t nov7 = unixOf(2026, 11, 7, 12, 0, 0, PST);
    CHECK(almDayTz(nov7, PST, -11, true, NULL) == PDT, "shift: back across Nov 1: -7 again");
    CHECK(almDayTz(unixOf(2026, 11, 1, 12, 0, 0, PST), PST, 6, true, NULL) == PST &&
          almDayTz(unixOf(2026, 11, 1, 12, 0, 0, PST), PST, -2, true, NULL) == PDT,
          "shift: FROM the change day the phone is taken as changed (-8): ahead same, behind -7");
    CHECK(almDayTz(unixOf(2027, 3, 1, 12, 0, 0, PST), PST, 20, true, NULL) == PDT,
          "shift: forward across March: +1 h");
    CHECK(almDayTz(oct27, -10 * 3600, 11, true, NULL) == -10 * 3600, "shift: -10 in daylight time is Hawaii: none");
    CHECK(almDayTz(oct27, 3600, 11, true, NULL) == 3600 && almDayTz(oct27, 19800, 11, true, NULL) == 19800,
          "shift: not a US zone (Europe, +5:30)");
    CHECK(almDayTz(unixOf(2027, 1, 10, 12, 0, 0, -4 * 3600), -4 * 3600, 90, true, NULL) == -4 * 3600,
          "shift: -4 in January is no changing US zone's standard time");

    CHECK(almDayTzNote(oct27, PDT, 11, true, s, sizeof(s)) &&
          is(s, "Times in UTC-8 (after the Nov 1 change)"), "shift note: after the Nov 1 change");
    CHECK(almDayTzNote(nov7, PST, -11, true, s, sizeof(s)) &&
          is(s, "Times in UTC-7 (before the Nov 1 change)"), "shift note: before it, stepping back");
    CHECK(almDayTzNote(unixOf(2027, 3, 1, 12, 0, 0, PST), PST, 20, true, s, sizeof(s)) &&
          is(s, "Times in UTC-7 (after the Mar 14 change)"), "shift note: March");
    CHECK(!almDayTzNote(oct27, PDT, 4, true, s, sizeof(s)) && !s[0] &&
          !almDayTzNote(oct27, PDT, 11, false, s, sizeof(s)), "shift note: none on the same side / setting off");
    CHECK(!almDayTzNote(oct27, PDT, 11, true, s, 20) && !s[0], "shift note: a short buffer writes \"\"");

    /* THE reviewer's case, on the screens: Oct 27 (UTC-7) stepped to Nov 7 used to read "Legal
     * 07:31-18:10" - the clock on Nov 7 reads 17:10 then. Now the day is computed and shown in
     * -8, exactly as on Nov 7 itself. */
    static AlmDay dd, ref;
    AlmCtx c = ctxFor(&dd, oct27, PDT, 11, LAT, LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    AlmCtx cr = ctxFor(&ref, unixOf(2026, 11, 7, 9, 0, 0, PST), PST, 0, LAT, LON, ASTRO_LEGAL_30MIN,
                       UNITS_METRIC);
    CHECK(dd.t0 == ref.t0 && dd.tzS == PST && !memcmp(&dd.sun, &ref.sun, sizeof(dd.sun)),
          "shift: Nov 7 from Oct 27 IS Nov 7's own table (t0 = its local midnight in -8)");
    Rows r, rr;
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    memset(&rr, 0, sizeof(rr));
    almLinesToday(&cr, collect, &rr);
    dump("TODAY Nov 7 from Oct 27", r);
    CHECK(is(r.text[0], "Sat Nov 7 (+11 days)") && is(r.text[1], "Times in UTC-8 (after the Nov 1 change)") &&
          r.kind[1] == ALM_ROW_WRAP, "shift: TODAY says it under the date (wrapped: 297 px whole)");
    const int li = find(r, "Legal "), lr = find(rr, "Legal ");
    CHECK(li > 0 && lr > 0 && is(r.text[li], rr.text[lr]) && is(r.text[li], "Legal 06:31-17:10 (30 min)"),
          "shift: 'Legal 06:31-17:10' - what Nov 7's own screen says (was 07:31-18:10)");
    CHECK(is(r.text[find(r, "Sun ")], rr.text[find(rr, "Sun ")]), "shift: the sun row too");
    CHECK(fitsAll(r, "TODAY across the change"), "shift: TODAY still fits");
    memset(&r, 0, sizeof(r));
    almLinesSun(&c, collect, &r);
    memset(&rr, 0, sizeof(rr));
    almLinesSun(&cr, collect, &rr);
    CHECK(is(r.text[1], "Times in UTC-8 (after the Nov 1 change)") &&
          is(r.text[find(r, "Last legal light")], rr.text[find(rr, "Last legal light")]),
          "shift: SUN under the place, last legal light as on the day");
    CHECK(fitsAll(r, "SUN across the change"), "shift: SUN fits");
    memset(&r, 0, sizeof(r));
    almLinesMoon(&c, collect, &r);
    CHECK(strncmp(r.text[1], "Times in UTC-8", 14) == 0 && fitsAll(r, "MOON across"), "shift: MOON under the place");
    memset(&r, 0, sizeof(r));
    almLinesSolunar(&c, collect, &r);
    CHECK(strncmp(r.text[1], "Times in UTC-8", 14) == 0 && fitsAll(r, "SOLUNAR across"),
          "shift: SOLUNAR under the place");
    // Backwards across the same change, and the setting off.
    c = ctxFor(&dd, nov7, PST, -11, LAT, LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    CHECK(is(r.text[1], "Times in UTC-7 (before the Nov 1 change)") && dd.tzS == PDT,
          "shift: Nov 7 back to Oct 27 is in -7");
    AlmCtx co = ctxFor(NULL, oct27, PDT, 11, LAT, LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    co.usDst = false;
    almDayCompute(&dd, almDayT0(&co), almDayTzS(&co), LAT, LON);
    co.day = &dd;
    memset(&r, 0, sizeof(r));
    almLinesToday(&co, collect, &r);
    CHECK(find(r, "Times in") < 0 && is(r.text[find(r, "Legal ")], "Legal 07:31-18:10 (30 min)"),
          "shift: setting off - today's offset, as before (Hawaii/Arizona do not change)");
    c = ctxFor(&dd, oct27, PDT, 3, LAT, LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    CHECK(find(r, "Times in") < 0, "shift: a day on the same side says nothing");
  }

  // ── ONE countdown: almCountdownDay (the Meshtastic Sun screen, serial `sun`) = TODAY's row ──
  {
    /* Review 2026-09-27: the countdown's inputs were worked out twice - almanacLegalToday for the
     * Meshtastic screen and `sun`, almLinesToday for the Almanac - and only this half was tested.
     * Both now call almCountdownDay; this proves the TODAY row IS that function over a whole day
     * (every 20 minutes, both rules), at 47 N and in a polar summer and winter. */
    struct P { double lat, lon; int tzS; int y, m, d; };
    const P ps[3] = { { 47.4957, -121.7868, PDT, 2026, 9, 27 },
                      { 78.2232, 15.6267, 7200, 2026, 6, 21 },
                      { 78.2232, 15.6267, 3600, 2026, 12, 21 } };
    static AlmDay dd;
    bool same = true;
    int n = 0;
    for (int pi = 0; pi < 3; pi++) {
      for (int rule = 0; rule < 2; rule++) {
        for (int k = 0; k < 72; k++) {
          const int64_t now = unixOf(ps[pi].y, ps[pi].m, ps[pi].d, 0, 0, 0, ps[pi].tzS) + k * 1200 + 7;
          AlmCtx c = ctxFor(&dd, now, ps[pi].tzS, 0, ps[pi].lat, ps[pi].lon, rule, UNITS_METRIC);
          Rows r;
          memset(&r, 0, sizeof(r));
          almLinesToday(&c, collect, &r);
          // What almanacLegalToday does: today's sun, tomorrow's only when needed.
          const int64_t t0 = almMidnight(now, ps[pi].tzS, 0);
          AstroSunDay today, tomorrow;
          astroSunDay(t0, ps[pi].lat, ps[pi].lon, &today);
          int64_t f = 0, l = 0;
          astroLegalLight(&today, rule, &f, &l);
          const bool need = almCountdownNeedsNext(now, f, l);
          if (need) astroSunDay(t0 + 86400, ps[pi].lat, ps[pi].lon, &tomorrow);
          char cd[48];
          almCountdownDay(now, ps[pi].tzS, rule, t0, ps[pi].lat, ps[pi].lon, &today,
                          need ? &tomorrow : NULL, cd, sizeof(cd));
          n++;
          if (!cd[0] || strcmp(cd, r.text[1]) != 0) {
            printf("    %d/%d/%d k=%d rule=%d: countdown \"%s\" vs TODAY \"%s\"\n", ps[pi].y, ps[pi].m,
                   ps[pi].d, k, rule, cd, r.text[1]);
            same = false;
          }
        }
      }
    }
    char msg[120];
    snprintf(msg, sizeof(msg), "one countdown: almCountdownDay == TODAY's row at %d instants (47 N, 78 N summer and winter)", n);
    CHECK(same, msg);
    CHECK(!almCountdownNeedsNext(100, 0, 0), "needs next: not on a day with no bound (polar)");
    CHECK(almCountdownNeedsNext(100, 50, 90) && !almCountdownNeedsNext(80, 50, 90) &&
          almCountdownNeedsNext(100, 50, 0), "needs next: once today's light has ended, or it has no end");
  }

  // ── the screens, at North Bend WA on 2026-09-27 ────────────────────────────────────────────
  static AlmDay day;
  const double NB_LAT = 47.4957, NB_LON = -121.7868;
  {
    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    AlmCtx c = ctxFor(&day, now, PDT, 0, NB_LAT, NB_LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    Rows r;
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    dump("TODAY", r);
    CHECK(r.n >= 10 && is(r.text[0], "Sun Sep 27"), "TODAY: the date first");
    CHECK(strncmp(r.text[1], "LEGAL LIGHT: ", 13) == 0, "TODAY: the countdown second, at 14:05");
    int li = find(r, "Legal ");
    int64_t first = 0, last = 0;
    astroLegalLight(&day.sun, ASTRO_LEGAL_30MIN, &first, &last);
    char a[8], b[8], want[48];
    almFmtClock(day.sun.rise - 1800, PDT, ALM_ROUND_UP, a, sizeof(a));
    almFmtClock(day.sun.set + 1800, PDT, ALM_ROUND_DOWN, b, sizeof(b));
    snprintf(want, sizeof(want), "Legal %s-%s (30 min)", a, b);
    CHECK(li > 0 && is(r.text[li], want), "TODAY: Legal = sunrise - 30 min (up) .. sunset + 30 min (down)");
    CHECK(find(r, "Sun 07:") > 0, "TODAY: the sun row");
    CHECK(find(r, "Moon: ") > 0 && find(r, "Solunar: ") > 0, "TODAY: moon and solunar rows");
    CHECK(find(r, "At GPS") > 0, "TODAY: the place");
    int e = find(r, "Sun...");
    CHECK(e > 0 && r.kind[e] == ALM_ENTRY_SUN && r.kind[r.n - 1] == ALM_ENTRY_SETTINGS &&
          r.n - e == 7, "TODAY: the seven entries last, with their keys");
    const int we = find(r, "Weather...");
    CHECK(we == e + 3 && r.kind[we] == ALM_ENTRY_WEATHER,
          "TODAY: \"Weather...\" (0.9.81) after Solunar..., key ALM_ENTRY_WEATHER");
    CHECK(find(r, "Weather: ") < 0 && find(r, "Alert: ") < 0,
          "TODAY: no weather view (AlmCtx.wx NULL) - no weather rows (test_weather has them)");
    CHECK(fitsAll(r, "TODAY"), "TODAY: every row fits 232 px");

    // The countdown's value against the tables.
    const int64_t leftS = last - now;
    almFmtSpan(leftS, ALM_ROUND_DOWN, a, sizeof(a));
    snprintf(want, sizeof(want), "LEGAL LIGHT: %s left", a);
    CHECK(is(r.text[1], want), "TODAY: the countdown is last legal light - now, rounded down");

    memset(&r, 0, sizeof(r));
    almLinesSun(&c, collect, &r);
    dump("SUN", r);
    CHECK(is(r.text[0], "At GPS"), "SUN: the place first");
    CHECK(strncmp(r.text[1], "First legal light ", 18) == 0 && strncmp(r.text[2], "Sunrise ", 8) == 0 &&
          strncmp(r.text[3], "Solar noon ", 11) == 0 && strncmp(r.text[4], "Sunset ", 7) == 0 &&
          strncmp(r.text[5], "Last legal light ", 17) == 0, "SUN: legal, rise, noon, set, legal");
    CHECK(find(r, "Civil dawn ") > 0 && find(r, "Civil dusk ") > 0, "SUN: civil twilight under the 30 min rule");
    int dl = find(r, "Day length ");
    CHECK(dl > 0 && strstr(r.text[dl + 1], "vs the day before") != NULL,
          "SUN: day length and the change from the day before");
    CHECK(r.text[dl + 1][0] == '-', "SUN: late September days are getting SHORTER at 47 N");
    CHECK(find(r, "Sun now: alt ") > 0, "SUN: where the sun is now (today)");
    CHECK(fitsAll(r, "SUN"), "SUN: every row fits");

    AlmCtx cc = c;
    cc.rule = ASTRO_LEGAL_CIVIL;
    memset(&r, 0, sizeof(r));
    almLinesSun(&cc, collect, &r);
    CHECK(find(r, "Civil dawn") < 0, "SUN: no separate civil rows under the civil rule");
    almFmtClock(day.sun.dawn, PDT, ALM_ROUND_UP, a, sizeof(a));
    snprintf(want, sizeof(want), "First legal light %s", a);
    CHECK(is(r.text[1], want), "SUN: civil rule: first legal light = civil dawn (rounded up)");

    memset(&r, 0, sizeof(r));
    almLinesMoon(&c, collect, &r);
    dump("MOON", r);
    {
      // the events that happen are in time order
      int64_t prev = 0;
      bool ordered = true;
      const char* names[4] = { "Moonrise ", "Overhead ", "Moonset ", "Underfoot " };
      const int64_t ats[4] = { day.moon.rise, day.moon.transit, day.moon.set, day.moon.under };
      for (int i = 1; i < r.n; i++) {
        for (int k = 0; k < 4; k++) {
          if (strncmp(r.text[i], names[k], strlen(names[k])) == 0) {
            if (ats[k] < prev) ordered = false;
            prev = ats[k];
          }
        }
      }
      CHECK(ordered, "MOON: rise/overhead/set/underfoot in time order");
    }
    CHECK(find(r, "Illuminated ") > 0 && find(r, "Age ") > 0, "MOON: illuminated and age");
    int nq = 0;
    int64_t prevQ = 0;
    bool qOrdered = true;
    const char* qn[4] = { "New moon ", "First qtr ", "Full moon ", "Last qtr " };
    for (int i = 0; i < r.n; i++) {
      for (int k = 0; k < 4; k++) {
        if (strncmp(r.text[i], qn[k], strlen(qn[k])) == 0) {
          nq++;
          if (day.phase[k] < prevQ) qOrdered = false;
          prevQ = day.phase[k];
        }
      }
    }
    CHECK(nq == 4 && qOrdered, "MOON: the next four quarters, in time order");
    CHECK(find(r, "Moon now: ") > 0, "MOON: where the moon is now (today)");
    CHECK(fitsAll(r, "MOON"), "MOON: every row fits");

    memset(&r, 0, sizeof(r));
    almLinesSolunar(&c, collect, &r);
    dump("SOLUNAR", r);
    CHECK(strncmp(r.text[1], "Rating: ", 8) == 0, "SOLUNAR: the rating");
    CHECK(is(r.text[r.n - 1], "Folk tables (J. A. Knight)"), "SOLUNAR: labelled as folk tables");
    CHECK(fitsAll(r, "SOLUNAR"), "SOLUNAR: every row fits");
    // Put "now" inside the first major and see it marked, once.
    if (day.sol.nMajor > 0) {
      AlmCtx cn = c;
      cn.now = day.sol.majorMid[0];
      memset(&r, 0, sizeof(r));
      almLinesSolunar(&cn, collect, &r);
      int marked = 0;
      for (int i = 0; i < r.n; i++) {
        const size_t L = strlen(r.text[i]);
        if (L > 4 && strcmp(r.text[i] + L - 4, " now") == 0) marked++;
      }
      CHECK(marked == 1 && find(r, "Major ") >= 0 &&
            strcmp(r.text[find(r, "Major ")] + strlen(r.text[find(r, "Major ")]) - 4, " now") == 0,
            "SOLUNAR: the period we are in is marked 'now', once");
      memset(&r, 0, sizeof(r));
      almLinesToday(&cn, collect, &r);
      CHECK(find(r, "Now: major until ") > 0, "TODAY: the current period");
    }

    memset(&r, 0, sizeof(r));
    almLinesDate(&c, collect, &r);
    dump("DATE", r);
    CHECK(is(r.text[0], "Sunday, September 27"), "DATE: the long date");
    CHECK(is(r.text[1], "2026, day 270 of 365, week 39"), "DATE: day 270 of 365, week 39");
    CHECK(is(r.text[2], "Time 14:05 UTC-7 (ntp)"), "DATE: local time, offset, source");
    CHECK(find(r, "Next: ") < 0, "DATE: no season row until the seasons are computed");
    int64_t seasons[4];
    for (int k = 0; k < 4; k++) seasons[k] = astroNextSeason(now, k);
    c.seasons = seasons;
    memset(&r, 0, sizeof(r));
    almLinesDate(&c, collect, &r);
    int ns = find(r, "Next: ");
    CHECK(ns > 0 && is(r.text[ns], "Next: December solstice") &&
          is(r.text[ns + 1], "Mon Dec 21, in 85 days"), "DATE: next season Mon Dec 21, in 85 days");
    CHECK(fitsAll(r, "DATE"), "DATE: every row fits");
    c.seasons = NULL;

    // A mesh clock is said on the screens that act on the time.
    AlmCtx cm = c;
    cm.clockMesh = true;
    cm.clockSrc = "mesh";
    memset(&r, 0, sizeof(r));
    almLinesToday(&cm, collect, &r);
    CHECK(find(r, "Mesh time: check it!") == 2, "TODAY: a mesh clock is flagged under the countdown");
    memset(&r, 0, sizeof(r));
    almLinesDate(&cm, collect, &r);
    CHECK(find(r, "Mesh time: check it!") > 0 && is(r.text[2], "Time 14:05 UTC-7 (mesh)"),
          "DATE: the mesh clock named and flagged");

    memset(&r, 0, sizeof(r));
    almLinesSettings(&c, collect, &r);
    dump("SETTINGS", r);
    CHECK(r.kind[0] == ALM_SET_LEGAL && is(r.text[0], "Legal light: 30 min rule"), "SETTINGS: legal rule row");
    CHECK(find(r, "Units: metric") > 0 && r.kind[find(r, "Units: metric")] == ALM_SET_UNITS,
          "SETTINGS: units row");
    AlmCtx cs = c;
    cs.rule = ASTRO_LEGAL_CIVIL;
    cs.units = UNITS_US;
    memset(&r, 0, sizeof(r));
    almLinesSettings(&cs, collect, &r);
    CHECK(is(r.text[0], "Legal light: civil twilight") && find(r, "Units: US (ft, mi, mph, F)") > 0 &&
          is(r.text[find(r, "Units: ")], unitsSettingRow(UNITS_US)),
          "SETTINGS: the other values (the units row is units.cpp's, the Maps menu's words)");
    CHECK(fitsAll(r, "SETTINGS"), "SETTINGS: every row fits");
  }

  // ── another day: no countdown, the tag, first period ───────────────────────────────────────
  {
    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    Rows r;
    AlmCtx c = ctxFor(&day, now, PDT, 1, NB_LAT, NB_LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    CHECK(is(r.text[0], "Mon Sep 28 (tomorrow)"), "TODAY +1: (tomorrow)");
    CHECK(strncmp(r.text[1], "Legal ", 6) == 0, "TODAY +1: no countdown on another day");
    CHECK(find(r, "First: ") > 0, "TODAY +1: the day's first period");
    c = ctxFor(&day, now, PDT, -3, NB_LAT, NB_LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    CHECK(is(r.text[0], "Thu Sep 24 (-3 days)"), "TODAY -3: (-3 days)");
    memset(&r, 0, sizeof(r));
    almLinesMoon(&c, collect, &r);
    CHECK(find(r, "Moon now") < 0 && strstr(r.text[find(r, "Illuminated ")], "at noon"),
          "MOON -3: no 'now', the phase at noon");
    CHECK(c.day->phaseFrom == almMidnight(now, PDT, -3), "MOON -3: quarters counted from that day");
  }

  // ── no place, no clock ─────────────────────────────────────────────────────────────────────
  {
    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    Rows r;
    AlmCtx c = ctxFor(NULL, now, PDT, 0, 0, 0, ASTRO_LEGAL_30MIN, UNITS_METRIC, ALM_PLACE_NONE, NULL);
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    dump("TODAY, no place", r);
    CHECK(is(r.text[0], "Sun Sep 27") && find(r, "No place known yet") > 0 &&
          find(r, "Moon: ") > 0 && find(r, "Legal") < 0, "TODAY no place: date, moon, guidance");
    CHECK(fitsAll(r, "TODAY no place"), "TODAY no place: fits");
    memset(&r, 0, sizeof(r));
    almLinesSun(&c, collect, &r);
    CHECK(is(r.text[0], "No place known yet") && r.kind[1] == ALM_ROW_WRAP, "SUN no place: guidance");
    c.clockKnown = false;
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    CHECK(is(r.text[0], "Clock not set yet") && is(r.text[1], "(NTP on WiFi, GPS, or mesh)") &&
          r.kind[2] == ALM_ENTRY_SUN, "TODAY no clock: says so, entries still there");
    memset(&r, 0, sizeof(r));
    almLinesDate(&c, collect, &r);
    CHECK(is(r.text[0], "Clock not set yet"), "DATE no clock: says so");
  }

  // ── POSITION ───────────────────────────────────────────────────────────────────────────────
  {
    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    AlmCtx c = ctxFor(NULL, now, PDT, 0, NB_LAT, NB_LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    AlmPos p;
    memset(&p, 0, sizeof(p));
    p.gpsOn = true;
    p.gpsHaveFix = true;
    p.gpsAgeMs = 3000;
    p.altAgeMs = 3000;
    p.motionAgeMs = 3000;
    p.sats = 8;
    p.hdopX10 = 12;
    p.altM = 425;
    p.speedKnX100 = 173;           // 1.73 kn = 3.2 km/h
    p.courseX10 = 2125;
    p.elevState = ALM_ELEV_OK;
    p.elevM = 412.4;
    p.elevZ = ELEV_Z;
    Rows r;
    memset(&r, 0, sizeof(r));
    almLinesPosition(&c, &p, collect, &r);
    dump("POSITION", r);
    CHECK(is(r.text[0], "Source: GPS 3s"), "POSITION: source and age");
    CHECK(is(r.text[1], "47.49570, -121.78680"), "POSITION: lat/lon %.5f");
    CHECK(is(r.text[2], "47 29.742N 121 47.208W"), "POSITION: degrees + decimal minutes");
    CHECK(strncmp(r.text[3], "UTM 10T ", 8) == 0 && strncmp(r.text[4], "MGRS 10T ", 9) == 0,
          "POSITION: UTM and MGRS (zone 10T)");
    CHECK(find(r, "Ground 412m") > 0, "POSITION: ground height, metric");
    CHECK(find(r, "GPS alt 425m") > 0, "POSITION: GPS altitude");
    CHECK(find(r, "Speed 3.2km/h, 213 SSW") > 0, "POSITION: speed and course (212.5 -> 213 SSW)");
    CHECK(find(r, "Sats 8, HDOP 1.2") > 0, "POSITION: sats/HDOP");
    int dc = find(r, "Declination ");
    CHECK(dc > 0 && strstr(r.text[dc], " E") != NULL && strncmp(r.text[dc + 1], "true = magnetic + ", 18) == 0,
          "POSITION: east declination at North Bend, and how to use it");
    {
      double dv = 0;
      sscanf(r.text[dc], "Declination %lf", &dv);
      CHECK(dv > 14.0 && dv < 16.5, "POSITION: ~15 deg E at North Bend in 2026 (WMM2025)");
    }
    CHECK(is(r.text[dc + 2], "WMM2025, valid to 2030"), "POSITION: the model and its end");
    CHECK(fitsAll(r, "POSITION"), "POSITION: every row fits");
    /* Review 2026-10-10: the serial console at a MEASURED place says ~1 km and nothing finer
     * (console captures land in notes in a public repo); the screen keeps every digit. */
    {
      AlmCtx cc = c;
      cc.lat = 40.0012345;
      cc.lon = -100.0098765;
      cc.coordsCoarse = true;
      Rows rc;
      memset(&rc, 0, sizeof(rc));
      almLinesPosition(&cc, &p, collect, &rc);
      CHECK(is(rc.text[0], "Source: GPS 3s") && is(rc.text[1], "~40.00, -100.01 (rounded here)") &&
            is(rc.text[2], "UTM/MGRS: on the screen only"), "POSITION console: 2 decimals at a measured place");
      bool fine = false;
      for (int k = 0; k < rc.n; k++) {
        fine = fine || strstr(rc.text[k], "UTM 1") || strstr(rc.text[k], "MGRS 1") ||
               strstr(rc.text[k], "40.001") || strstr(rc.text[k], "40 0");
      }
      CHECK(!fine, "POSITION console: no UTM, MGRS, degrees-minutes or 3rd decimal anywhere");
    }

    c.units = UNITS_US;
    p.elevZ = ELEV_Z_COARSE;
    p.speedKnX100 = 20;            // under 1 km/h
    memset(&r, 0, sizeof(r));
    almLinesPosition(&c, &p, collect, &r);
    CHECK(find(r, "Ground 1,353ft coarse") > 0, "POSITION US: feet, coarse layer said");
    CHECK(find(r, "GPS alt 1,394ft") > 0, "POSITION US: GPS altitude in feet");
    CHECK(find(r, "Stationary") > 0, "POSITION: under 1 km/h is Stationary");
    p.gpsAgeMs = 30000;
    p.motionAgeMs = 30000;
    memset(&r, 0, sizeof(r));
    almLinesPosition(&c, &p, collect, &r);
    CHECK(find(r, "Stationary") < 0 && find(r, "Speed") < 0, "POSITION: no motion from a 30 s old fix");
    /* Review 2026-09-27: GGAs arriving while RMCs are lost refresh the FIX, not the motion. */
    p.gpsAgeMs = 1000;
    p.motionAgeMs = 30000;
    p.speedKnX100 = 173;
    memset(&r, 0, sizeof(r));
    almLinesPosition(&c, &p, collect, &r);
    CHECK(find(r, "Stationary") < 0 && find(r, "Speed") < 0 && find(r, "GPS alt") > 0,
          "POSITION: a fresh fix (a GGA) with a 30 s old RMC shows no speed - its own age decides");
    p.motionAgeMs = 1000;
    p.altAgeMs = 130000;
    memset(&r, 0, sizeof(r));
    almLinesPosition(&c, &p, collect, &r);
    CHECK(find(r, "Speed ") > 0 && find(r, "GPS alt") < 0,
          "POSITION: ...and a fresh RMC with a 130 s old GGA shows no altitude");
    p.altAgeMs = 3000;
    p.speedKnX100 = 20;

    c.placeKind = ALM_PLACE_WAYPOINT;
    c.placeName = "Camp";
    p.elevState = ALM_ELEV_NOLAYER;
    p.gpsOn = false;
    memset(&r, 0, sizeof(r));
    almLinesPosition(&c, &p, collect, &r);
    CHECK(is(r.text[0], "Source: waypoint Camp") && find(r, "Ground: no /maps/elev") > 0 &&
          find(r, "GPS receiver off") > 0, "POSITION: waypoint, no elevation layer, GPS off");
    c.placeKind = ALM_PLACE_LAST_GPS;
    p.gpsOn = true;
    p.gpsAgeMs = 12 * 60000;
    p.altAgeMs = 12 * 60000;
    p.motionAgeMs = 12 * 60000;
    p.elevState = ALM_ELEV_PENDING;
    memset(&r, 0, sizeof(r));
    almLinesPosition(&c, &p, collect, &r);
    CHECK(is(r.text[0], "Source: last GPS 12m") && find(r, "Ground: reading...") > 0 &&
          find(r, "GPS alt") < 0, "POSITION: a stale GPS, the ground not read yet, no stale altitude");
    c.now = unixOf(2031, 1, 1, 0, 0, 0, 0);
    memset(&r, 0, sizeof(r));
    almLinesPosition(&c, &p, collect, &r);
    CHECK(find(r, "Declination: model expired") > 0 && find(r, "WMM2025 covers 2025-2030") > 0,
          "POSITION: after 2030 the model has expired - no number");
    CHECK(fitsAll(r, "POSITION expired"), "POSITION expired: fits");
    c.placeKind = ALM_PLACE_PIN;
    c.lat = 85.5;
    c.now = now;
    memset(&r, 0, sizeof(r));
    almLinesPosition(&c, &p, collect, &r);
    CHECK(is(r.text[0], "Source: pin") && find(r, "UTM/MGRS: none (polar)") > 0, "POSITION: polar: no UTM");
  }

  // ── the phase cache: kept while no quarter passes, renewed when one does ───────────────────
  {
    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    AlmCtx c = ctxFor(&day, now, PDT, 0, NB_LAT, NB_LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    CHECK(almDayEnsure(&day, ALM_NEED_PHASES, now + 60) == 0, "phases: a minute later, nothing recomputed");
    int64_t earliest = day.phase[0];
    for (int q = 1; q < 4; q++) if (day.phase[q] < earliest) earliest = day.phase[q];
    CHECK(almDayEnsure(&day, ALM_NEED_PHASES, earliest) == ALM_NEED_PHASES,
          "phases: at the earliest quarter, recomputed");
    bool after = true;
    for (int q = 0; q < 4; q++) if (day.phase[q] <= earliest) after = false;
    CHECK(after, "phases: every quarter now strictly after it");
    CHECK(almDayEnsure(&day, ALM_NEED_PREV, 0) == 0, "prev: already there, nothing computed");
    (void)c;
  }

  // ── A1: the day in slices is the day in one call, bit for bit ──────────────────────────────
  {
    /* Phone 2, 2026-09-27: opening the Almanac spent 148 ms in one loop pass on the core day
     * (a 334 ms LOOP STALL with the rest). The app now runs the SAME arithmetic a slice per
     * timer tick (almDayWorkRun). Proven here: cut at random, and cut after every unit, the
     * finished AlmDay equals almDayCompute + almDayEnsure field for field, for six places over
     * two years, all three screens' wants. */
    struct P { double lat, lon; int tzS; };
    const P ps[6] = { { 47.4957, -121.7868, PDT }, { 71.2906, -156.7887, -8 * 3600 },
                      { -0.1807, -78.4678, -5 * 3600 }, { -33.8688, 151.2093, 10 * 3600 },
                      { 78.2232, 15.6267, 3600 }, { 22.5726, 88.3639, 19800 } };
    static AlmDay whole, sliced;
    static AlmDayWork w;
    bool same = true;
    int days = 0;
    long calls = 0;
    for (int pi = 0; pi < 6; pi++) {
      for (int k = 0; k < 24; k++) {
        const int64_t now = unixOf(2026, 1, 5, 9, 17, 0, ps[pi].tzS) + (int64_t)k * 31 * 86400;
        AlmCtx c = ctxFor(NULL, now, ps[pi].tzS, (k % 5) - 2, ps[pi].lat, ps[pi].lon,
                          ASTRO_LEGAL_30MIN, UNITS_METRIC);
        const int64_t t0 = almDayT0(&c);
        const int tz = almDayTzS(&c);
        almDayCompute(&whole, t0, tz, ps[pi].lat, ps[pi].lon);
        almDayEnsure(&whole, ALM_NEED_PREV | ALM_NEED_NEXT | ALM_NEED_PHASES, almPhaseFrom(&c));
        almDayWorkBegin(&sliced, &w, t0, tz, ps[pi].lat, ps[pi].lon, false);
        AlmWant want;
        memset(&want, 0, sizeof(want));
        want.parts = ALM_PART_CORE | ALM_NEED_PREV | ALM_NEED_NEXT | ALM_NEED_PHASES;
        want.phaseFrom = almPhaseFrom(&c);
        want.moonFirst = (k & 1) != 0;
        Cutter cut = { (unsigned)(pi * 1000 + k), 0, 0 };
        int guard = 0;
        while (almDayWorkLeft(&sliced, &w, NULL, &want) && guard++ < 100000) {
          if (k % 3 == 0) {
            cut.left = 0;                         // every unit its own slice
          }
          almDayWorkRun(&sliced, &w, NULL, &want, cutMore, &cut);
          calls++;
        }
        days++;
        if (!sameDay(whole, sliced)) {
          printf("    %d/%d: sliced != whole\n", pi, k);
          same = false;
        }
      }
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "slices: %d days cut into %ld slices = the one-call tables, field for field", days, calls);
    CHECK(same, msg);

    // A search opens a slice and ends it: never after other work (a slice + ~20 ms).
    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    AlmCtx c = ctxFor(NULL, now, PDT, 0, NB_LAT, NB_LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    static AlmMoonAge age;
    memset(&age, 0, sizeof(age));
    almDayWorkBegin(&sliced, &w, almDayT0(&c), PDT, NB_LAT, NB_LON, false);
    AlmWant want;
    c.day = &sliced;
    almScreenWant(ALM_SCREEN_MOON, &c, &want);
    CHECK(want.parts == (ALM_PART_CORE | ALM_NEED_PHASES | ALM_PART_AGE) && want.moonFirst,
          "wants: MOON = the core, the quarters, the age; its moon first");
    int n = 0, first = 0;
    int got[16];
    while (almDayWorkLeft(&sliced, &w, &age, &want) && n < 16) {
      got[n++] = almDayWorkRun(&sliced, &w, &age, &want, alwaysMore, NULL);
    }
    first = got[0];
    CHECK(n == 7 && first == (ALM_PART_MOON | ALM_PART_SUN | ALM_PART_SOL) && got[1] == 0 &&
          got[2] == ALM_PART_AGE && got[3] == 0 && got[6] == ALM_NEED_PHASES,
          "slices: with time to spare, the core in one, then each of the 2 + 4 searches alone");
    CHECK(sliced.valid && sliced.havePhases && almMoonAgeCovers(&age, now), "slices: all there after");
    almScreenWant(ALM_SCREEN_TODAY, &c, &want);
    CHECK(want.parts == ALM_PART_CORE, "wants: TODAY at 14:05 = the core (no tomorrow before dark)");
    AlmCtx cd = c;
    cd.now = unixOf(2026, 9, 27, 21, 5, 0, PDT);
    almScreenWant(ALM_SCREEN_TODAY, &cd, &want);
    CHECK(want.parts == (ALM_PART_CORE | ALM_NEED_NEXT), "wants: TODAY after dark adds tomorrow's sun");
    almScreenWant(ALM_SCREEN_SUN, &c, &want);
    CHECK(want.parts == (ALM_PART_CORE | ALM_NEED_PREV) && !want.moonFirst, "wants: SUN adds the day before");
    almScreenWant(ALM_SCREEN_POSITION, &c, &want);
    CHECK(want.parts == 0, "wants: POSITION computes no day");
    // A place moved ~100 m: the old tables stay on the screen until the new ones land.
    almDayWorkBegin(&sliced, &w, almDayT0(&c), PDT, NB_LAT + 0.001, NB_LON, true);
    CHECK(sliced.valid && sliced.have == ALM_PART_CORE && sliced.lat == NB_LAT + 0.001,
          "place moved: kept on the screen while it is recomputed");
    almScreenWant(ALM_SCREEN_TODAY, &c, &want);
    CHECK(almDayWorkLeft(&sliced, &w, &age, &want) == ALM_PART_CORE, "place moved: ...and the core is owed again");
    almDayWorkBegin(&sliced, &w, almDayT0(&c) + 86400, PDT, NB_LAT, NB_LON, false);
    CHECK(!sliced.valid && sliced.have == 0 && !sliced.havePhases, "another day: emptied");
  }

  // ── the moon's age without a search a minute ─────────────────────────────────────────────
  {
    /* astroMoonPhaseAt's search for the last new moon was 740 of its 956 libm calls (~20 of
     * ~25 ms on the phone) - on every TODAY and MOON build, the minute tick's included. The
     * new moons either side of one instant cover the whole lunation between them. */
    static AlmMoonAge age;
    memset(&age, 0, sizeof(age));
    const int64_t start = unixOf(2026, 9, 1, 0, 0, 0, 0);
    bool ok = true;
    int searches = 0, checked = 0;
    for (int64_t t = start; t < start + 90 * 86400; t += 6 * 3600 + 17) {
      while (!almMoonAgeCovers(&age, t) && searches < 1000) {
        almMoonAgeWork(&age, t);                  // one search a call
        searches++;
      }
      const int64_t want = astroPrevMoonPhase(t, 0);
      AstroMoonPhase a, b;
      astroMoonPhaseAt(t, &a);
      astroMoonPhaseWith(t, age.prevNew, &b);
      checked++;
      if (age.prevNew != want || a.ageDays != b.ageDays || a.illum != b.illum || a.phase != b.phase ||
          a.elong != b.elong || a.sep != b.sep) {
        printf("    t=%lld: cached %lld, search %lld\n", (long long)t, (long long)age.prevNew, (long long)want);
        ok = false;
      }
    }
    // Right at the edges: the second before and at the next new moon.
    const int64_t nn = age.nextNew;
    CHECK(almMoonAgeCovers(&age, nn - 1) && !almMoonAgeCovers(&age, nn), "age: covers up to the next new moon, not past it");
    while (!almMoonAgeCovers(&age, nn) && searches < 1000) {
      almMoonAgeWork(&age, nn);
      searches++;
    }
    CHECK(age.prevNew == nn && age.prevNew == astroPrevMoonPhase(nn, 0), "age: at the new moon, it is the previous one");
    char msg[128];
    snprintf(msg, sizeof(msg), "age: %d instants over 3 lunations = astroMoonPhaseAt exactly, %d searches in all", checked, searches);
    CHECK(ok && searches <= 10, msg);
    AlmMoonAge none;
    memset(&none, 0, sizeof(none));
    CHECK(!almMoonAgeCovers(&none, start) && !almMoonAgeCovers(NULL, start), "age: nothing known covers nothing");
  }

  // ── the screens while the slices run: what is there, and "Computing..." ──────────────────
  {
    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    static AlmDay part;
    static AlmDayWork w;
    AlmCtx c = ctxFor(NULL, now, PDT, 0, NB_LAT, NB_LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    c.noSearch = true;
    almDayWorkBegin(&part, &w, almDayT0(&c), PDT, NB_LAT, NB_LON, false);
    c.day = &part;
    Rows r;
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    dump("TODAY, nothing computed yet", r);
    CHECK(is(r.text[0], "Sun Sep 27") && is(r.text[1], "Computing...") && find(r, "Legal") < 0 &&
          find(r, "Moon: ") > 0 && find(r, "At GPS") > 0 && r.kind[r.n - 1] == ALM_ENTRY_SETTINGS,
          "partial: TODAY with nothing yet: date, Computing..., the moon, the place, the entries");
    int said = 0;
    for (int i = 0; i < r.n; i++) said += !strcmp(r.text[i], "Computing...");
    CHECK(said == 1, "partial: ONE Computing... row");
    AlmWant want;
    almScreenWant(ALM_SCREEN_TODAY, &c, &want);
    almDayWorkRun(&part, &w, NULL, &want, NULL, NULL);    // a slice with no limit...
    CHECK(part.valid, "partial: ...one unlimited slice finishes the core");
    part.have = ALM_PART_SUN;                              // pretend only the sun has landed
    part.valid = false;
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    dump("TODAY, the sun only", r);
    CHECK(strncmp(r.text[1], "LEGAL LIGHT", 11) == 0 && find(r, "Legal 0") > 0 && find(r, "Sun 07:") > 0 &&
          find(r, "Computing...") > find(r, "Sun 07:") && find(r, "Solunar: ") < 0,
          "partial: the sun's rows as soon as it lands; Computing... where the solunar goes");
    memset(&r, 0, sizeof(r));
    almLinesSun(&c, collect, &r);
    CHECK(find(r, "Sunrise ") > 0 && find(r, "Day before: computing...") > 0, "partial: SUN, the day before still owed");
    memset(&r, 0, sizeof(r));
    almLinesMoon(&c, collect, &r);
    CHECK(is(r.text[1], "Computing...") && r.n == 2, "partial: MOON waits for its moon");
    memset(&r, 0, sizeof(r));
    almLinesSolunar(&c, collect, &r);
    CHECK(is(r.text[1], "Computing...") && r.n == 2, "partial: SOLUNAR waits for the core");
    part.have = ALM_PART_CORE;
    part.valid = true;
    memset(&r, 0, sizeof(r));
    almLinesMoon(&c, collect, &r);
    dump("MOON, no age and no quarters yet (noSearch)", r);
    CHECK(find(r, "Age: computing...") > 0 && find(r, "Quarters: computing...") > 0 && find(r, "Age ") < 0,
          "partial: MOON never searches from the app - the age and the quarters say so");
    static AlmMoonAge age;
    memset(&age, 0, sizeof(age));
    while (!almMoonAgeWork(&age, now)) {}
    c.moonAge = &age;
    memset(&r, 0, sizeof(r));
    almLinesMoon(&c, collect, &r);
    AlmCtx cs = ctxFor(&part, now, PDT, 0, NB_LAT, NB_LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    Rows rs;
    memset(&rs, 0, sizeof(rs));
    almLinesMoon(&cs, collect, &rs);
    CHECK(find(r, "Age ") > 0 && is(r.text[find(r, "Age ")], rs.text[find(rs, "Age ")]),
          "partial: with the cached new moons, the age the search gives (serial `almanac` = screen)");
    c.moonAge = NULL;
    c.day = NULL;
    memset(&r, 0, sizeof(r));
    almLinesSun(&c, collect, &r);
    CHECK(is(r.text[0], "Computing...") && r.n == 1, "partial: SUN with no table at all");
    CHECK(fitsAll(r, "partial"), "partial: every row fits");
  }

  // ── A2: which place, when there is no GPS fix and no pin ───────────────────────────────────
  {
    AlmPlaceIn in;
    AlmPlace pl;
    memset(&in, 0, sizeof(in));
    in.gpsFix = true;
    in.gpsLatI = 474957000;
    in.gpsLonI = -1217868000;
    in.gpsAgeMs = (2 * 3600 + 5 * 60 + 12) * 1000u;
    in.gpsSats = 8;
    in.gpsHdopX10 = 12;
    in.viewOk = true;
    in.viewLatI = 474000000;
    in.viewLonI = -1218000000;
    in.refOk = true;
    in.refKind = ALM_PLACE_WAYPOINT;
    in.refLatI = 474500000;
    in.refLonI = -1217500000;
    in.refName = "Camp";
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_WAYPOINT && pl.latI == 474500000 && is(pl.name, "Camp"),
          "place: resolveReference first (a chosen waypoint)");
    in.refOk = false;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_LAST_GPS && pl.latI == 474957000 && pl.ageMs == in.gpsAgeMs &&
          is(pl.name, "last GPS"), "place: else the last GPS fix of this boot, however old");
    /* Review 2026-09-27: the fallback took ANY fix - a 3-satellite one measured 20 km off read
     * "At last GPS 0s ago". The bar resolveReference() and the map's "me" use (meshPosFixUsable). */
    in.gpsAgeMs = 0;
    in.gpsSats = 3;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_MAP_VIEW, "place: a fresh fix on 3 satellites is no place - the map's view");
    in.gpsSats = 8;
    in.gpsHdopX10 = 101;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_MAP_VIEW, "place: ...nor one over HDOP 10");
    in.gpsSats = 4;
    in.gpsHdopX10 = 100;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_LAST_GPS, "place: 4 satellites at HDOP 10.0 is on the bar: taken");
    in.gpsSats = -1;
    in.gpsHdopX10 = -1;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_LAST_GPS,
          "place: a receiver that never said (no GGA) is not refused for its silence");
    in.gpsSats = 2;
    in.viewOk = false;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_NONE, "place: a poor fix and nothing else: no place (the guidance rows)");
    in.viewOk = true;
    in.gpsSats = 8;
    in.gpsHdopX10 = 12;
    in.gpsFix = false;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_MAP_VIEW && pl.latI == 474000000 && is(pl.name, "map view"),
          "place: else the map's saved view (phone 2's case: no fix since the reboot, no pin)");
    in.viewLatI = in.viewLonI = 0;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_NONE, "place: the map's 0,0 ('nothing to show') is no place");
    in.viewLatI = 950000000;
    in.viewLonI = 10;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_NONE, "place: off the globe is no place");
    in.viewOk = false;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_NONE, "place: nothing at all: none (the guidance rows)");

    /* 0.9.82: the hotspot phone's GPS (self_pos.h). Dummy coordinates (40.0, -100.0). A live
     * phone fix ages from its OWN fix, not the receiver's; the step-2 fallback can be the
     * phone's and says so; the poor-fix bar is the same for it. */
    memset(&in, 0, sizeof(in));
    in.refOk = true;
    in.refKind = ALM_PLACE_PHONE;
    in.refLatI = 400000000;
    in.refLonI = -1000000000;
    in.refName = "phone GPS";
    in.refAgeMs = 3000;
    in.gpsAgeMs = 999000;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_PHONE && pl.ageMs == 3000 && is(pl.name, "phone GPS"),
          "place: a live phone fix, aged from the phone's own fix");
    in.refKind = ALM_PLACE_LAST_PHONE;
    in.refAgeMs = 600000;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_LAST_PHONE && pl.ageMs == 600000, "place: explicit Here, the phone's stale fix");
    in.refOk = false;
    in.phoneFix = true;
    in.phoneLatI = 400000000;
    in.phoneLonI = -1000000000;
    in.phoneAgeMs = 120000;
    in.phoneSats = 9;
    in.phoneHdopX10 = 9;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_LAST_PHONE && is(pl.name, "last phone GPS") && pl.ageMs == 120000,
          "place: else the newest fix of this boot - the phone's, named so");
    in.phoneSats = 3;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_NONE, "place: ...and a 3-satellite phone fix is no place either");

    /* Review 2026-10-10: step 2 used to OVERWRITE the GPS fields with a newer phone fix before
     * judging it. Indoors on the hotspot: the GPS fixed well 20 minutes ago, the phone sends
     * fresh 3-satellite fixes - the usable GPS fix must win, aged by ITS OWN age. */
    in.gpsFix = true;
    in.gpsLatI = 400100000;
    in.gpsLonI = -1000100000;
    in.gpsAgeMs = 20 * 60000;
    in.gpsSats = 8;
    in.gpsHdopX10 = 11;
    in.phoneAgeMs = 2000;
    in.phoneSats = 3;
    in.viewOk = true;
    in.viewLatI = 410000000;
    in.viewLonI = -1010000000;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_LAST_GPS && pl.latI == 400100000 && pl.ageMs == 20 * 60000u &&
          is(pl.name, "last GPS"), "place: a fresh POOR phone fix does not hide an older usable GPS fix");
    in.phoneSats = 9;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_LAST_PHONE && pl.latI == 400000000 && pl.ageMs == 2000,
          "place: both usable - the newer (the phone's), its own age");
    in.phoneAgeMs = 30 * 60000;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_LAST_GPS && pl.ageMs == 20 * 60000u, "place: both usable - the newer (the GPS's)");
    in.phoneAgeMs = in.gpsAgeMs;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_LAST_GPS, "place: a tie keeps the GPS (it outranks the phone)");
    in.gpsSats = 3;
    in.phoneSats = 2;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_MAP_VIEW, "place: both poor - the map's view");
    /* Explicit Here resolved to the GPS's stale fix while the phone has a NEWER (poor) one: the
     * row says the GPS fix's own age, not the phone's ("At last GPS 2s ago" was the bug). */
    in.refOk = true;
    in.refKind = ALM_PLACE_LAST_GPS;
    in.refLatI = 400100000;
    in.refLonI = -1000100000;
    in.refName = "last GPS";
    in.phoneAgeMs = 2000;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_LAST_GPS && pl.ageMs == 20 * 60000u,
          "place: a LAST_GPS reference is aged by the GPS fix, not by a newer phone fix");

    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    static AlmDay dd;
    AlmCtx c = ctxFor(&dd, now, PDT, 0, NB_LAT, NB_LON, ASTRO_LEGAL_30MIN, UNITS_METRIC,
                      ALM_PLACE_LAST_GPS, "last GPS");
    c.placeAgeMs = (2 * 3600 + 5 * 60 + 12) * 1000u;
    Rows r;
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    CHECK(find(r, "At last GPS 2h 5m ago") > 0 && fitsAll(r, "TODAY last GPS"), "place: TODAY says 'At last GPS 2h 5m ago'");
    memset(&r, 0, sizeof(r));
    almLinesSun(&c, collect, &r);
    CHECK(is(r.text[0], "At last GPS 2h 5m ago"), "place: ...and so does SUN");
    c.placeAgeMs = 47u * 3600u * 1000u + 59u * 60000u;
    memset(&r, 0, sizeof(r));
    almLinesSun(&c, collect, &r);
    CHECK(is(r.text[0], "At last GPS 47h 59m ago") && fitsAll(r, "SUN old GPS"), "place: the widest age fits");
    c.placeKind = ALM_PLACE_MAP_VIEW;
    c.placeName = "map view";
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    CHECK(find(r, "At map view") > 0 && find(r, "No place known") < 0, "place: TODAY 'At map view'");
    AlmPos p;
    memset(&p, 0, sizeof(p));
    p.elevState = ALM_ELEV_PENDING;
    memset(&r, 0, sizeof(r));
    almLinesPosition(&c, &p, collect, &r);
    CHECK(is(r.text[0], "Source: map view") && fitsAll(r, "POSITION map view"),
          "place: POSITION names the map view");
  }

  // ── SETTINGS: US daylight saving ───────────────────────────────────────────────────────────
  {
    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    AlmCtx c = ctxFor(NULL, now, PDT, 0, NB_LAT, NB_LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    Rows r;
    memset(&r, 0, sizeof(r));
    almLinesSettings(&c, collect, &r);
    int k = find(r, "US daylight saving: yes");
    CHECK(k > 0 && r.kind[k] == ALM_SET_DST, "settings: 'US daylight saving: yes' (key ALM_SET_DST)");
    CHECK(fitsAll(r, "SETTINGS dst yes"), "settings: fits");
    c.usDst = false;
    memset(&r, 0, sizeof(r));
    almLinesSettings(&c, collect, &r);
    k = find(r, "US daylight saving: no (HI, AZ)");
    CHECK(k > 0 && r.kind[k] == ALM_SET_DST && fitsAll(r, "SETTINGS dst no"), "settings: 'no (HI, AZ)' fits");
    int keys[8], nk = 0;
    bool unique = true;
    for (int i = 0; i < r.n; i++) {
      if (r.kind[i] > ALM_ROW_WRAP) {
        for (int j = 0; j < nk; j++) unique = unique && keys[j] != r.kind[i];
        keys[nk++] = r.kind[i];
      }
    }
    CHECK(unique && nk == 3, "settings: three settings, keys unique");
  }

  // ── the change day BEFORE 2 AM, and the day screens on it (review 2026-09-27, finding 1) ───
  {
    /* almDstReminder counts 00:00-02:00 of a change day as "not changed yet" (set it then) and
     * almDayTz counted the whole day as changed: on Nov 1 at 01:00 (-7, correct) Nov 2 read "Legal
     * 07:23-18:18" - the clock will read 06:23-17:18 - with no note; Oct 31 was put in -6; and on
     * Mar 14 2027 at 01:00 (-8) Mar 15's first legal light read an hour EARLY. One rule now. */
    const int PST = -8 * 3600;
    const double LAT = 47.4957, LON = -121.7868;
    const int64_t nov1 = unixOf(2026, 11, 1, 1, 0, 0, PDT);
    int64_t ch = 0;
    CHECK(almDayTz(nov1, PDT, 1, true, &ch) == PST && ch == astroDaysFromCivil(2026, 11, 1),
          "change day 01:00 at -7 (not changed yet): Nov 2 is in -8, across TODAY's change");
    CHECK(almDayTz(nov1, PDT, -1, true, &ch) == PDT && ch == 0,
          "change day 01:00: Oct 31 is on today's side, -7 (was -6)");
    CHECK(almDayTzNote(nov1, PDT, 1, true, s, sizeof(s)) &&
          is(s, "Times in UTC-8 (after the Nov 1 change)"), "change day 01:00: ...and Nov 2 says so");
    CHECK(!almDayTzNote(nov1, PDT, -1, true, s, sizeof(s)) && !s[0], "change day 01:00: Oct 31 says nothing");
    const int64_t mar14 = unixOf(2027, 3, 14, 1, 0, 0, PST);
    CHECK(almDayTz(mar14, PST, 1, true, &ch) == PDT && ch == astroDaysFromCivil(2027, 3, 14) &&
          almDayTz(mar14, PST, -1, true, NULL) == PST,
          "spring change day 01:00 at -8: Mar 15 in -7 (first light was an hour EARLY), Mar 13 in -8");
    static AlmDay dd, ref;
    AlmCtx c = ctxFor(&dd, nov1, PDT, 1, LAT, LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    AlmCtx cr = ctxFor(&ref, unixOf(2026, 11, 2, 9, 0, 0, PST), PST, 0, LAT, LON, ASTRO_LEGAL_30MIN,
                       UNITS_METRIC);
    Rows r, rr;
    memset(&r, 0, sizeof(r));
    almLinesToday(&c, collect, &r);
    memset(&rr, 0, sizeof(rr));
    almLinesToday(&cr, collect, &rr);
    dump("TODAY Nov 2 from Nov 1 01:00 (-7)", r);
    const int li = find(r, "Legal "), lr = find(rr, "Legal ");
    CHECK(dd.t0 == ref.t0 && li > 0 && lr > 0 && is(r.text[li], rr.text[lr]) &&
          is(r.text[li], "Legal 06:23-17:18 (30 min)"),
          "change day 01:00: Nov 2 reads 'Legal 06:23-17:18' - Nov 2's own screen (was 07:23-18:18)");
    CHECK(is(r.text[1], "Times in UTC-8 (after the Nov 1 change)"), "change day 01:00: under the date");
    const int dn = find(r, "US clocks go back 1 h at 2 AM today");
    CHECK(dn > 1 && dn < li && r.kind[dn] == ALM_ROW_WRAP,
          "change day: TODAY carries the day's reminder, wrapped, above the legal-light row");
    CHECK(fitsAll(r, "TODAY on the change day"), "change day: TODAY fits");

    /* One rule, every 10 minutes of both 2026-27 change days, from both offsets each can have:
     * the reminder's "go" (not yet) is exactly when the day AHEAD is shifted, "went" exactly when
     * the day BEHIND is; and every day screen carries the reminder's own sentence. */
    struct CD { int y, m, d, tz; };
    const CD cds[4] = { { 2026, 11, 1, PDT }, { 2026, 11, 1, PST }, { 2027, 3, 14, PST },
                        { 2027, 3, 14, PDT } };
    bool agree = true, carried = true;
    int n = 0;
    for (int i = 0; i < 4; i++) {
      for (int k = 0; k < 144; k++) {
        const int64_t now = unixOf(cds[i].y, cds[i].m, cds[i].d, 0, 0, 0, cds[i].tz) + k * 600 + 7;
        char rem[96], note[96];
        const bool got = almDstReminder(now, cds[i].tz, true, rem, sizeof(rem));
        const bool notYet = got && strstr(rem, " go ") != NULL;
        const bool ahead = almDayTz(now, cds[i].tz, 1, true, NULL) != cds[i].tz;
        const bool behind = almDayTz(now, cds[i].tz, -1, true, NULL) != cds[i].tz;
        n++;
        if (!got || ahead != notYet || behind == notYet) {
          printf("    %d-%d-%d tz=%d k=%d: \"%s\" ahead=%d behind=%d\n", cds[i].y, cds[i].m, cds[i].d,
                 cds[i].tz, k, rem, ahead, behind);
          agree = false;
        }
        if (!almDstTodayNote(now, cds[i].tz, true, note, sizeof(note)) || strcmp(note, rem) != 0) {
          carried = false;
        }
        if ((k % 24) == 0) {
          static AlmDay d2;
          for (int off = -1; off <= 1; off++) {
            AlmCtx cc = ctxFor(&d2, now, cds[i].tz, off, LAT, LON, ASTRO_LEGAL_30MIN, UNITS_US);
            Rows q;
            memset(&q, 0, sizeof(q));
            almLinesSun(&cc, collect, &q);
            carried = carried && find(q, rem) > 0 && q.kind[find(q, rem)] == ALM_ROW_WRAP &&
                      fitsAll(q, "SUN change day");
            memset(&q, 0, sizeof(q));
            almLinesMoon(&cc, collect, &q);
            carried = carried && find(q, rem) > 0;
            memset(&q, 0, sizeof(q));
            almLinesSolunar(&cc, collect, &q);
            carried = carried && find(q, rem) > 0;
            memset(&q, 0, sizeof(q));
            almLinesToday(&cc, collect, &q);
            carried = carried && find(q, rem) > 0 && fitsAll(q, "TODAY change day");
          }
        }
      }
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "change days: the reminder's 'go'/'went' and the stepped day's offset "
             "agree at all %d instants (2 AM rule, both offsets, both changes)", n);
    CHECK(agree, msg);
    CHECK(carried, "change days: TODAY, SUN, MOON and SOLUNAR carry the reminder's own sentence (wrapped)");
    CHECK(!almDstTodayNote(unixOf(2026, 10, 31, 12, 0, 0, PDT), PDT, true, s, sizeof(s)) && !s[0] &&
          !almDstTodayNote(unixOf(2026, 11, 2, 12, 0, 0, PST), PST, true, s, sizeof(s)) &&
          !almDstTodayNote(unixOf(2026, 11, 1, 12, 0, 0, PST), PST, false, s, sizeof(s)) &&
          !almDstTodayNote(unixOf(2026, 11, 1, 12, 0, 0, 3600), 3600, true, s, sizeof(s)),
          "change day note: not the day before or after, not with the setting off, not in Europe");
    AlmCtx co = ctxFor(&dd, unixOf(2026, 10, 31, 12, 0, 0, PDT), PDT, 0, LAT, LON,
                       ASTRO_LEGAL_30MIN, UNITS_METRIC);
    memset(&r, 0, sizeof(r));
    almLinesToday(&co, collect, &r);
    CHECK(find(r, "US clocks") < 0, "change day note: TODAY the day before carries none (DATE has it)");
  }

  // ── MOON's quarters and DATE's season in the offset of THEIR date (finding 7) ──────────────
  {
    const int PST = -8 * 3600;
    const double LAT = 47.4957, LON = -121.7868;
    const int64_t oct25 = unixOf(2026, 10, 25, 12, 0, 0, PDT);
    CHECK(almTzAt(oct25, PDT, true, unixOf(2026, 11, 1, 1, 59, 59, PDT)) == PDT &&
          almTzAt(oct25, PDT, true, unixOf(2026, 11, 1, 2, 0, 0, PDT)) == PST,
          "instant: back an hour at 02:00 PDT on Nov 1, to the second");
    CHECK(almTzAt(oct25, PDT, true, unixOf(2027, 3, 14, 1, 59, 59, PST)) == PST &&
          almTzAt(oct25, PDT, true, unixOf(2027, 3, 14, 2, 0, 0, PST)) == PDT,
          "instant: forward at 02:00 PST on Mar 14 2027, to the second");
    CHECK(almTzAt(oct25, PDT, true, oct25 + 86400) == PDT && almTzAt(oct25, PDT, true, oct25) == PDT,
          "instant: the same side: today's offset");
    const int64_t nov20 = unixOf(2026, 11, 20, 12, 0, 0, PST);
    CHECK(almTzAt(nov20, PST, true, oct25) == PDT, "instant: from after the change, an instant before it is -7");
    CHECK(almTzAt(oct25, PDT, false, nov20) == PDT && almTzAt(oct25, -10 * 3600, true, nov20) == -10 * 3600 &&
          almTzAt(oct25, 3600, true, nov20) == 3600 && almTzAt(oct25, 19800, true, nov20) == 19800,
          "instant: setting off, Hawaii, Europe, +5:30 - today's offset");
    CHECK(almTzAt(unixOf(2026, 11, 1, 1, 0, 0, PDT), PDT, true, unixOf(2026, 11, 1, 3, 0, 0, PST)) == PST &&
          almTzAt(unixOf(2026, 11, 1, 1, 0, 0, PDT), PDT, true, unixOf(2026, 11, 1, 1, 30, 0, PDT)) == PDT,
          "instant: from 01:00 on the change day - before the change -7, after it -8");
    // The reviewer's case, on the screen: Oct 25 at -7 listed "New moon Mon Nov 9 00:02".
    static AlmDay dd;
    AlmCtx c = ctxFor(&dd, oct25, PDT, 0, LAT, LON, ASTRO_LEGAL_30MIN, UNITS_METRIC);
    Rows r;
    memset(&r, 0, sizeof(r));
    almLinesMoon(&c, collect, &r);
    dump("MOON Oct 25 2026 (-7)", r);
    const int nm = find(r, "New moon ");
    CHECK(nm > 0 && is(r.text[nm], "New moon Sun Nov 8 23:02"),
          "MOON: the new moon past Nov 1 is on the clock it will have - Sun Nov 8 23:02, not Mon Nov 9 00:02");
    bool each = true;
    const char* qn[4] = { "New moon ", "First qtr ", "Full moon ", "Last qtr " };
    for (int q = 0; q < 4; q++) {
      char want[48], dt[24], hm[8];
      const int tz = almTzAt(oct25, PDT, true, dd.phase[q]);
      almFmtDate(dd.phase[q], tz, dt, sizeof(dt));
      almFmtClock(dd.phase[q], tz, ALM_ROUND_NEAREST, hm, sizeof(hm));
      snprintf(want, sizeof(want), "%s%s %s", qn[q], dt, hm);
      const int at = find(r, qn[q]);
      each = each && at > 0 && is(r.text[at], want) &&
             (dd.phase[q] < unixOf(2026, 11, 1, 2, 0, 0, PDT) ? tz == PDT : tz == PST);
    }
    CHECK(each, "MOON: each quarter in its own date's offset (-7 before Nov 1 02:00, -8 after)");
    // DATE: the next season (the December solstice) on the clock of its date: -8.
    int64_t seasons[4];
    for (int k = 0; k < 4; k++) seasons[k] = astroNextSeason(oct25, k);
    c.seasons = seasons;
    c.day = NULL;
    memset(&r, 0, sizeof(r));
    almLinesDate(&c, collect, &r);
    char dt[24], want[48];
    almFmtDate(seasons[3], PST, dt, sizeof(dt));
    const int64_t days = (seasons[3] + PST) / 86400 - (oct25 + PDT) / 86400;
    snprintf(want, sizeof(want), "%s, in %d days", dt, (int)days);
    const int ns = find(r, "Next: ");
    CHECK(ns > 0 && is(r.text[ns], "Next: December solstice") && is(r.text[ns + 1], want),
          "DATE: the December solstice seen from October is dated on the -8 clock");
    CHECK(fitsAll(r, "DATE Oct 25"), "DATE Oct 25: fits");
  }

  // ── the same place: a tolerance around the keyed place, not a grid (findings 2, 6) ─────────
  {
    CHECK(almSamePlace(47.4957, -121.7868, 47.4957, -121.7868), "same place: itself");
    CHECK(almSamePlace(47.4957, -121.7868, 47.4957 + 0.00099, -121.7868 - 0.00099),
          "same place: within 0.001 deg on both axes");
    CHECK(!almSamePlace(47.4957, -121.7868, 47.4957 + 0.0011, -121.7868) &&
          !almSamePlace(47.4957, -121.7868, 47.4957, -121.7868 + 0.0011), "same place: not past it");
    CHECK(!almSamePlace(NAN, 0, NAN, 0) && !almSamePlace(0, 0, 0, NAN), "same place: NaN is nobody's place");
    /* A still GPS within noise of 47.4995 - the old key rounded it to 0.001 deg and flipped
     * between 47.499 and 47.500 on the noise; keyed where it was first seen, it never re-keys. */
    const double k0 = 47.49950001, l0 = -121.78650001;
    bool kept = true, flipped = false;
    int lastE3 = (int)floor(k0 * 1000 + 0.5);
    for (int i = 0; i < 1000; i++) {
      const double jit = ((i * 7919) % 41 - 20) * 1e-6;          // +-20e-6 deg: ~2 m of noise
      const double la = 47.4995 + jit, lo = -121.7865 - jit;
      kept = kept && almSamePlace(k0, l0, la, lo);
      const int e3 = (int)floor(la * 1000 + 0.5);
      flipped = flipped || e3 != lastE3;
      lastE3 = e3;
    }
    CHECK(kept && flipped, "same place: 2 m of GPS noise across a 0.001-degree line - the old grid "
                           "key flipped, the tolerance never re-keys");
  }

  // ── a rebuilt list keeps its scroll (finding 9) ───────────────────────────────────────────
  {
    CHECK(almMenuTop(12, 5, 12, 20, 11) == 5, "scroll: same rows - the list stays where it was");
    CHECK(almMenuTop(12, 5, 13, 21, 11) == 6, "scroll: a row appeared above - the selection keeps its place on the glass");
    CHECK(almMenuTop(12, 5, 11, 19, 11) == 4, "scroll: a row went above - the same");
    CHECK(almMenuTop(12, 9, 12, 15, 11) == 4, "scroll: held to the last screenful");
    CHECK(almMenuTop(0, 0, 15, 20, 11) == 9, "scroll: held to the rows, and the selection (15) stays on the glass (9..19)");
    CHECK(almMenuTop(3, 0, 3, 8, 11) == 0 && almMenuTop(5, 2, 5, 0, 11) == 0,
          "scroll: a list that fits (or is empty) starts at the top");
    CHECK(almMenuTop(2, 5, 2, 20, 11) == 2, "scroll: a nonsense old view still shows the selection");
  }

  // ── the header titles (finding 10) ──────────────────────────────────────────────────────────
  {
    static Font7 f18;
    fontLoad(&f18, Akrobat_Bold18);
    /* ALM_TITLE_MAX_W is what GUI.cpp's header leaves with WiFi, one kind of unread message and
     * the widest clock: 240 - 8 - 6 - (3 + 25 battery + 3) - (17 + 6 WiFi) - (19 + 3 message)
     * - (3 + clock). The clock is measured here, not assumed. */
    int clockW = 0;
    for (int h = 0; h < 24; h++) {
      for (int m = 0; m < 60; m++) {
        char hm[8];
        snprintf(hm, sizeof(hm), "%02d:%02d", h, m);
        const int w = textWidth(&f18, hm);
        clockW = w > clockW ? w : clockW;
      }
    }
    const int room = 240 - 8 - 6 - (3 + 25 + 3) - (17 + 6) - (19 + 3) - (3 + clockW);
    char msg[160];
    snprintf(msg, sizeof(msg), "titles: the budget (%d px) is inside the header's room with WiFi, a message "
             "icon and the widest clock (%d px; clock %d px)", ALM_TITLE_MAX_W, room, clockW);
    CHECK(f18.ok && ALM_TITLE_MAX_W <= room, msg);
    const int64_t now = unixOf(2026, 9, 27, 12, 0, 0, PDT);           // a Sunday
    char t[32], u[32];
    almTitle(ALM_SCREEN_TODAY, true, now, PDT, 0, t, sizeof(t));
    almTitle(ALM_SCREEN_SUN, true, now, PDT, 0, u, sizeof(u));
    CHECK(is(t, "Sep 27 (Sun)") && is(u, "Sun: Sep 27"),
          "titles: on a Sunday TODAY is 'Sep 27 (Sun)' and SUN 'Sun: Sep 27' - not one colon apart");
    almTitle(ALM_SCREEN_SOLUNAR, true, now, PDT, 3, t, sizeof(t));
    CHECK(is(t, "Solunar: Sep 30"), "titles: SOLUNAR has its colon like SUN and MOON");
    almTitle(ALM_SCREEN_MOON, true, now, PDT, -1, t, sizeof(t));
    CHECK(is(t, "Moon: Sep 26"), "titles: MOON, the day stepped");
    almTitle(ALM_SCREEN_TODAY, true, now, PDT, 3, t, sizeof(t));
    CHECK(is(t, "Sep 30 (Wed)"), "titles: TODAY stepped");
    almTitle(ALM_SCREEN_SUN, false, now, PDT, 0, t, sizeof(t));
    CHECK(is(t, "Almanac"), "titles: no clock - 'Almanac'");
    almTitle(ALM_SCREEN_POSITION, true, now, PDT, 0, t, sizeof(t));
    almTitle(ALM_SCREEN_DATE, true, now, PDT, 0, u, sizeof(u));
    CHECK(is(t, "Position & GPS") && is(u, "Date & seasons"), "titles: POSITION, DATE");
    almTitle(ALM_SCREEN_WEATHER, true, now, PDT, 0, t, sizeof(t));
    CHECK(is(t, "Weather"), "titles: WEATHER (0.9.81)");
    almTitle(ALM_SCREEN_TODAY, true, now, PDT, 0, t, 8);
    CHECK(!t[0], "titles: a short buffer writes \"\"");
    int widest = 0;
    char wide[32] = "";
    bool fit = true;
    for (int day = -1; day < 366 + 7; day++) {
      for (int sc = ALM_SCREEN_TODAY; sc < ALM_SCREEN_COUNT; sc++) {
        almTitle(sc, true, unixOf(2026, 1, 1, 12, 0, 0, 0), 0, day, t, sizeof(t));
        const int w = textWidth(&f18, t);
        if (w > widest) {
          widest = w;
          snprintf(wide, sizeof(wide), "%s", t);
        }
        if (!t[0] || w > ALM_TITLE_MAX_W) {
          printf("    title \"%s\" is %d px\n", t, w);
          fit = false;
        }
      }
    }
    snprintf(msg, sizeof(msg), "titles: every screen's title on every date of a year fits %d px of Bold 18 "
             "(widest %d: \"%s\")", ALM_TITLE_MAX_W, widest, wide);
    CHECK(fit, msg);
  }

  // ── a waypoint's name wraps; every row measured (finding 8) ──────────────────────────────────
  {
    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    static AlmDay dd;
    const char* names[3] = { "Hunting Camp North Fork", "WWWWWWWWWWWWWWWWWWWWWWW", "Camp" };
    bool ok = true;
    for (int i = 0; i < 3; i++) {
      AlmCtx c = ctxFor(&dd, now, PDT, 0, NB_LAT, NB_LON, ASTRO_LEGAL_30MIN, UNITS_US,
                        ALM_PLACE_WAYPOINT, names[i]);
      char at[48], src[64];
      snprintf(at, sizeof(at), "At %s", names[i]);
      snprintf(src, sizeof(src), "Source: waypoint %s", names[i]);
      Rows r;
      memset(&r, 0, sizeof(r));
      almLinesToday(&c, collect, &r);
      int k = find(r, at);
      ok = ok && k > 0 && r.kind[k] == ALM_ROW_WRAP && fitsAll(r, names[i]);
      memset(&r, 0, sizeof(r));
      almLinesSun(&c, collect, &r);
      ok = ok && is(r.text[0], at) && r.kind[0] == ALM_ROW_WRAP && fitsAll(r, names[i]);
      AlmPos p;
      memset(&p, 0, sizeof(p));
      p.elevState = ALM_ELEV_PENDING;
      memset(&r, 0, sizeof(r));
      almLinesPosition(&c, &p, collect, &r);
      ok = ok && is(r.text[0], src) && r.kind[0] == ALM_ROW_WRAP && fitsAll(r, names[i]);
    }
    int rows = 0;
    CHECK(ok && wrapsWhole("Source: waypoint WWWWWWWWWWWWWWWWWWWWWWW", &rows) && rows <= 3,
          "waypoint names: 'At <name>' and 'Source: waypoint <name>' are wrapped sentences, whole, "
          "23 characters at their widest - no row is cut to '..'");
  }

  // ── 0.9.82: the phone places' rows fit, at their widest ages ──────────────────────────────
  {
    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    static AlmDay dd;
    const int kinds[2] = { ALM_PLACE_PHONE, ALM_PLACE_LAST_PHONE };
    const uint32_t ages[3] = { 59000u, (47u * 3600u + 59u * 60u) * 1000u, 9u * 86400u * 1000u };
    bool ok = true, sawAt = true;
    for (int ki = 0; ki < 2; ki++) {
      for (int ai = 0; ai < 3; ai++) {
        AlmCtx c = ctxFor(&dd, now, PDT, 0, 40.0, -100.0, ASTRO_LEGAL_30MIN, UNITS_US, kinds[ki],
                          kinds[ki] == ALM_PLACE_PHONE ? "phone GPS" : "last phone GPS");
        c.placeAgeMs = ages[ai];
        AlmPos p;
        memset(&p, 0, sizeof(p));
        p.elevState = ALM_ELEV_PENDING;
        Rows r;
        memset(&r, 0, sizeof(r)); almLinesToday(&c, collect, &r); ok &= fitsAll(r, "phone place");
        sawAt = sawAt && find(r, kinds[ki] == ALM_PLACE_PHONE ? "At phone GPS" : "At last phone") >= 0;
        memset(&r, 0, sizeof(r)); almLinesSun(&c, collect, &r); ok &= fitsAll(r, "phone place");
        memset(&r, 0, sizeof(r)); almLinesPosition(&c, &p, collect, &r); ok &= fitsAll(r, "phone place");
        ok = ok && !strncmp(r.text[0], kinds[ki] == ALM_PLACE_PHONE ? "Source: phone GPS " : "Source: last phone ", 17);
      }
    }
    CHECK(ok && sawAt, "phone places: 'At phone GPS' / 'At last phone <age>' and 'Source: ...' rows "
                       "fit 232 px at every age");
  }

  // ── 0.9.82: a Wi-Fi location place (geoloc.h) - named with its size, at its widest ─────────
  {
    const int64_t now = unixOf(2026, 9, 27, 14, 5, 0, PDT);
    static AlmDay dd;
    const char* names[2] = { "Wi-Fi +/-50 m", "Wi-Fi +/-100 m" };
    bool ok = true, sawAt = true, sawSrc = true;
    for (int ni = 0; ni < 2; ni++) {
      AlmCtx c = ctxFor(&dd, now, PDT, 0, 40.0, -100.0, ASTRO_LEGAL_30MIN, UNITS_US, ALM_PLACE_NET, names[ni]);
      c.placeAgeMs = 11u * 60000u;
      AlmPos p;
      memset(&p, 0, sizeof(p));
      p.elevState = ALM_ELEV_PENDING;
      Rows r;
      char want[40];
      memset(&r, 0, sizeof(r)); almLinesToday(&c, collect, &r); ok &= fitsAll(r, "Wi-Fi place");
      snprintf(want, sizeof(want), "At %s", names[ni]);
      sawAt = sawAt && find(r, want) >= 0;
      memset(&r, 0, sizeof(r)); almLinesSun(&c, collect, &r); ok &= fitsAll(r, "Wi-Fi place");
      memset(&r, 0, sizeof(r)); almLinesPosition(&c, &p, collect, &r); ok &= fitsAll(r, "Wi-Fi place");
      snprintf(want, sizeof(want), "Source: %s", names[ni]);
      sawSrc = sawSrc && is(r.text[0], want);
    }
    CHECK(ok && sawAt && sawSrc, "Wi-Fi place: 'At Wi-Fi +/-100 m' and 'Source: Wi-Fi +/-100 m' fit 232 px");
    AlmPlaceIn in;
    memset(&in, 0, sizeof(in));
    in.refOk = true;
    in.refKind = ALM_PLACE_NET;
    in.refLatI = 400000000;
    in.refLonI = -1000000000;
    in.refName = "Wi-Fi +/-60 m";
    in.refAgeMs = 90000;
    in.gpsAgeMs = 999000;
    AlmPlace pl;
    almPickPlace(&in, &pl);
    CHECK(pl.kind == ALM_PLACE_NET && pl.ageMs == 90000 && is(pl.name, "Wi-Fi +/-60 m"),
          "place: a live Wi-Fi fix, aged from its own last confirming scan, named with its size");
  }

  // ── the sweep: every row of every screen fits, everywhere, all year ────────────────────────
  {
    struct Place { const char* name; double lat, lon; int tzS; };
    const Place places[6] = {
      { "north-bend", 47.4957, -121.7868, -7 * 3600 },
      { "utqiagvik", 71.2906, -156.7887, -8 * 3600 },
      { "quito", -0.1807, -78.4678, -5 * 3600 },
      { "sydney", -33.8688, 151.2093, 10 * 3600 },
      { "longyearbyen", 78.2232, 15.6267, 1 * 3600 },
      { "kolkata", 22.5726, 88.3639, 19800 },
    };
    bool ok = true;
    int screens = 0;
    AlmPos p;
    memset(&p, 0, sizeof(p));
    p.gpsOn = true;
    p.gpsHaveFix = true;
    p.gpsAgeMs = 59000;
    p.altAgeMs = 59000;
    p.motionAgeMs = 5000;          // the speed row is measured too (54 kn)
    p.sats = 12;
    p.hdopX10 = 99;
    p.altM = 4392;
    p.speedKnX100 = 5400;
    p.courseX10 = 3590;
    p.elevState = ALM_ELEV_OK;
    p.elevM = 4392;
    p.elevZ = ELEV_Z_COARSE;
    for (int pi = 0; pi < 6; pi++) {
      for (int k = 0; k < 82; k++) {
        const int64_t base = unixOf(2026, 1, 3, 0, 0, 0, places[pi].tzS) + (int64_t)k * 9 * 86400;
        const int64_t now = base + (k % 4) * 6 * 3600 + 1234;     // 00:20, 06:20, 12:20, 18:20
        for (int rule = 0; rule < 2; rule++) {
          const int units = (k + rule) % 2;
          const int off = (k % 3 == 0) ? 0 : ((k % 3 == 1) ? 1 : -365);
          AlmCtx c = ctxFor(&day, now, places[pi].tzS, off, places[pi].lat, places[pi].lon,
                            rule, units, ALM_PLACE_WAYPOINT, "Camp Muir");
          int64_t seasons[4];
          for (int q = 0; q < 4; q++) seasons[q] = astroNextSeason(now, q);
          c.seasons = seasons;
          char where[64];
          snprintf(where, sizeof(where), "%s k=%d rule=%d off=%d", places[pi].name, k, rule, off);
          Rows r;
          memset(&r, 0, sizeof(r)); almLinesToday(&c, collect, &r); ok &= fitsAll(r, where);
          memset(&r, 0, sizeof(r)); almLinesSun(&c, collect, &r); ok &= fitsAll(r, where);
          memset(&r, 0, sizeof(r)); almLinesMoon(&c, collect, &r); ok &= fitsAll(r, where);
          memset(&r, 0, sizeof(r)); almLinesSolunar(&c, collect, &r); ok &= fitsAll(r, where);
          memset(&r, 0, sizeof(r)); almLinesPosition(&c, &p, collect, &r); ok &= fitsAll(r, where);
          memset(&r, 0, sizeof(r)); almLinesDate(&c, collect, &r); ok &= fitsAll(r, where);
          screens += 6;
        }
      }
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "sweep: %d screens (6 places x 82 days x 2 rules) - every row fits, "
             "every glyph exists (widest %d px: \"%s\")", screens, g_widest, g_widestText);
    CHECK(ok, msg);
  }

  // ── a ZONED clock (0.9.82): the zone's rule decides, and the phone changes by itself ──────
  {
    /* Settings > Time zone = Eastern (named, or Auto at Harrisburg PA, a state capital). The phone
     * changes its own offset at the zone's DST changes (tz_service.cpp), so the reminders must not
     * say "set Time offset", and a day across a change is in that day's offset BY THE RULE. */
    const int EDT = -4 * 3600, EST = -5 * 3600;
    const double LAT = 40.2640, LON = -76.8840;
    char zs[128];                    // the app's buffers are 96 (dstTodayRow, DATE)
    const int64_t oct15 = unixOf(2026, 10, 15, 12, 0, 0, EDT);
    AlmCtx c = zonedFor(NULL, oct15, EDT, 0, LAT, LON, EST, TZ_RULE_USCAN, "Eastern");
    CHECK(almZoneDstReminder(&c, zs, sizeof(zs)) &&
          is(zs, "Clocks go back 1 h Sun Nov 1 - the phone changes by itself"),
          "zoned: 17 days before Nov 1: the change is automatic, nothing to set");
    Rows r;
    memset(&r, 0, sizeof(r));
    almLinesDate(&c, collect, &r);
    const int dr = find(r, "Clocks go back 1 h Sun Nov 1");
    CHECK(dr > 0 && r.kind[dr] == ALM_ROW_WRAP && find(r, "US clocks") < 0 && fitsAll(r, "DATE zoned"),
          "zoned: DATE carries it (wrapped), never 'set Time offset'");
    c.usDst = false;
    CHECK(almZoneDstReminder(&c, zs, sizeof(zs)), "zoned: the zone decides, not the typed-offset switch");
    c.usDst = true;
    c.now = unixOf(2026, 10, 10, 23, 59, 0, EDT);
    CHECK(!almZoneDstReminder(&c, zs, sizeof(zs)) && !s[0], "zoned: 22 days before: none");
    c.now = unixOf(2026, 11, 1, 1, 30, 0, EDT);       // 05:30 UTC, before the change
    CHECK(almZoneDstTodayNote(&c, zs, sizeof(zs)) &&
          is(zs, "Clocks go back 1 h at 2 AM today - the phone changes by itself"),
          "zoned: on the day, before 2 AM EDT: it will");
    c.now = unixOf(2026, 11, 1, 1, 30, 0, EST);       // 06:30 UTC: the SECOND 01:30, after it
    c.tzS = EST;
    CHECK(almZoneDstTodayNote(&c, zs, sizeof(zs)) &&
          is(zs, "Clocks went back 1 h at 2 AM today - the phone changed by itself"),
          "zoned: the repeated hour, after the change: it did (the clock is EST)");
    c.dayOffset = 1;
    CHECK(almDayTzS(&c) == EST && !almZoneDayTzNote(&c, zs, sizeof(zs)) &&
          almDayTz(c.now, EST, 1, true, NULL) == -6 * 3600,
          "zoned: from that hour, tomorrow is EST too - no note (the bare-offset rule said -6 here)");
    c.dayOffset = 0;
    c.now = unixOf(2026, 11, 2, 9, 0, 0, EST);
    CHECK(!almZoneDstReminder(&c, zs, sizeof(zs)) && !almZoneDstTodayNote(&c, zs, sizeof(zs)),
          "zoned: the day after: nothing");
    c.now = unixOf(2027, 3, 1, 12, 0, 0, EST);
    CHECK(almZoneDstReminder(&c, zs, sizeof(zs)) &&
          is(zs, "Clocks go forward 1 h Sun Mar 14 - the phone changes by itself"), "zoned: March, forward");
    c.now = unixOf(2027, 3, 14, 10, 0, 0, EDT);
    c.tzS = EDT;
    CHECK(almZoneDstTodayNote(&c, zs, sizeof(zs)) &&
          is(zs, "Clocks went forward 1 h at 2 AM today - the phone changed by itself"),
          "zoned: March 14 after the change");
    // A zone with no DST (Arizona, named or Auto at Phoenix): no reminder, no shift, ever.
    AlmCtx az = zonedFor(NULL, oct15, -7 * 3600, 11, 33.4480, -112.0740, -7 * 3600, TZ_RULE_NONE, "Arizona");
    CHECK(!almZoneDstReminder(&az, zs, sizeof(zs)) && almDayTzS(&az) == -7 * 3600 &&
          !almZoneDayTzNote(&az, zs, sizeof(zs)), "zoned Arizona: no reminder, no shift");
    // Newfoundland's -3:30: the bare-offset rule refuses part-hour offsets; the zone's rule does not.
    const int NDT = -150 * 60, NST = -210 * 60;
    AlmCtx nf = zonedFor(NULL, unixOf(2026, 10, 27, 12, 0, 0, NDT), NDT, 11, 47.5615, -52.7126, NST,
                         TZ_RULE_USCAN, "Newfoundland");
    CHECK(almDayTzS(&nf) == NST && almZoneDayTzNote(&nf, zs, sizeof(zs)) &&
          is(zs, "Times in UTC-3:30 (after the Nov 1 change)"),
          "zoned Newfoundland: Nov 7 from Oct 27 is in -3:30 and says so");
    nf.dayOffset = 0;
    nf.now = unixOf(2026, 10, 20, 12, 0, 0, NDT);
    CHECK(almZoneDstReminder(&nf, zs, sizeof(zs)) && is(zs, "Clocks go back 1 h Sun Nov 1 - the phone changes by itself"),
          "zoned Newfoundland: reminded (by itself)");

    // The day shift, by the rule, on the screens - and it equals the day's own table.
    static AlmDay dz, refz;
    const int64_t oct27 = unixOf(2026, 10, 27, 12, 0, 0, EDT);
    AlmCtx cz = zonedFor(&dz, oct27, EDT, 11, LAT, LON, EST, TZ_RULE_USCAN, "Eastern");
    AlmCtx crz = zonedFor(&refz, unixOf(2026, 11, 7, 9, 0, 0, EST), EST, 0, LAT, LON, EST, TZ_RULE_USCAN,
                          "Eastern");
    (void)crz;
    CHECK(almDayTzS(&cz) == EST && dz.t0 == refz.t0 && !memcmp(&dz.sun, &refz.sun, sizeof(dz.sun)),
          "zoned shift: Nov 7 from Oct 27 IS Nov 7's own table");
    memset(&r, 0, sizeof(r));
    almLinesToday(&cz, collect, &r);
    CHECK(is(r.text[1], "Times in UTC-5 (after the Nov 1 change)") && fitsAll(r, "TODAY zoned across"),
          "zoned shift: TODAY says it under the date, and fits");
    AlmCtx back = zonedFor(NULL, unixOf(2026, 11, 7, 12, 0, 0, EST), EST, -11, LAT, LON, EST, TZ_RULE_USCAN,
                           "Eastern");
    CHECK(almDayTzS(&back) == EDT && almZoneDayTzNote(&back, zs, sizeof(zs)) &&
          is(zs, "Times in UTC-4 (before the Nov 1 change)"), "zoned shift: stepping back across it");
    CHECK(almCtxTzAt(&cz, unixOf(2026, 11, 9, 0, 2, 0, EST)) == EST &&
          almCtxTzAt(&cz, unixOf(2026, 10, 31, 23, 0, 0, EDT)) == EDT &&
          almCtxTzAt(&cz, unixOf(2026, 11, 1, 5, 59, 59, 0)) == EDT &&
          almCtxTzAt(&cz, unixOf(2026, 11, 1, 6, 0, 0, 0)) == EST,
          "zoned: the offset THEN (MOON's quarters, DATE's season, the weather) changes at 06:00:00 UTC");
    // Before a zone's rule took over (BC/Alberta 2026): the bare-offset rule, as before.
    AlmCtx van = zonedFor(NULL, oct15, -7 * 3600, 0, LAT, LON, -7 * 3600, TZ_RULE_NONE, "Mountain, no DST");
    van.tzSince = 1772964000LL;
    CHECK(almCtxTzAt(&van, unixOf(2026, 1, 15, 12, 0, 0, 0)) == -8 * 3600 &&
          almCtxTzAt(&van, unixOf(2026, 6, 15, 12, 0, 0, 0)) == -7 * 3600,
          "zoned with a since: an instant before it is the US rule on the offset (PST then)");

    // Settings: the row says DST is the zone's, and still fits.
    memset(&r, 0, sizeof(r));
    almLinesSettings(&c, collect, &r);
    const int k = find(r, "Daylight saving: automatic");
    CHECK(k > 0 && r.kind[k] == ALM_SET_DST && strstr(r.text[k + 1], "Eastern") &&
          strstr(r.text[k + 1], "reminder is on") && fitsAll(r, "SETTINGS zoned"),
          "zoned Settings: 'Daylight saving: automatic', named, OK still the typed-offset switch");
    memset(&r, 0, sizeof(r));
    almLinesSettings(&az, collect, &r);
    CHECK(find(r, "Daylight saving: none here") > 0 && fitsAll(r, "SETTINGS zoned no DST"),
          "zoned Settings, Arizona: 'Daylight saving: none here'");

    // Every screen, every day around both changes, fits.
    bool ok = true;
    static AlmDay dw;
    for (int dd = -3; dd <= 25; dd++) {
      const int64_t now = unixOf(2026, 10, 29, 1, 30, 0, EDT) + (int64_t)dd * 6 * 3600;
      const int tzNow = now < unixOf(2026, 11, 1, 6, 0, 0, 0) ? EDT : EST;
      for (int off = -2; off <= 9; off += 3) {
        AlmCtx w = zonedFor(&dw, now, tzNow, off, LAT, LON, EST, TZ_RULE_USCAN, "Eastern");
        int64_t seasons[4];
        for (int q = 0; q < 4; q++) seasons[q] = astroNextSeason(now, q);
        w.seasons = seasons;
        Rows rw;
        memset(&rw, 0, sizeof(rw)); almLinesToday(&w, collect, &rw); ok &= fitsAll(rw, "zoned TODAY");
        memset(&rw, 0, sizeof(rw)); almLinesSun(&w, collect, &rw); ok &= fitsAll(rw, "zoned SUN");
        memset(&rw, 0, sizeof(rw)); almLinesMoon(&w, collect, &rw); ok &= fitsAll(rw, "zoned MOON");
        memset(&rw, 0, sizeof(rw)); almLinesDate(&w, collect, &rw); ok &= fitsAll(rw, "zoned DATE");
        for (int i = 0; i < rw.n; i++) if (strstr(rw.text[i], "Time offset")) ok = false;
      }
    }
    CHECK(ok, "zoned: every screen around the change fits, and none says 'Time offset'");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
