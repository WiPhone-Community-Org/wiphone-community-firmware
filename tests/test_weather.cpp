/* test_weather.cpp - the pure half of the Almanac's weather (0.9.81): WiPhone/weather.cpp (the
 * URLs, the Open-Meteo and NWS parsers, the streaming alert counter, the times, the cache file, the
 * fold, the fetch policy), WiPhone/weather_lines.cpp (every row the WEATHER screen and TODAY show,
 * measured against the phone's own Akrobat Bold 20), json_read.cpp's numbers and nulls, the
 * ClientHello's curve order (tls_curves.h) and the two pinned roots' fingerprints (isrg_roots.h).
 *
 * THE FIXTURES (tests/fixtures/wx/) were captured 2026-10-03 at PUBLIC points only - Seattle
 * 47.61,-122.33 and offshore 45,-130 - with the echoed point, model elevation and title
 * scrubbed; the alert bodies are SYNTHETIC, built from a real two-alert answer's field shapes with
 * every place, office, zone and polygon replaced (the polygon is a ring around the Seattle point).
 * The 111 event names are NWS's own /alerts/types list (no place in it).
 *
 * NO NETWORK: everything here is pure code on bytes in memory and files under tests/fixtures.
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../WiPhone/weather.h"
#include "../WiPhone/weather_lines.h"
#include "../WiPhone/json_read.h"
#include "../WiPhone/gemini.h"         // the de-chunker the worker feeds bodies through
#include "../WiPhone/units.h"
#include "../WiPhone/isrg_roots.h"
#include "../WiPhone/tls_curves.h"
#include "../WiPhone/book_hash.h"      // SHA-256, for the roots' fingerprints
#include "../WiPhone/menu_wrap.h"

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

static bool is(const char* got, const char* want) {
  if (strcmp(got, want) != 0) {
    printf("    got \"%s\", want \"%s\"\n", got, want);
    return false;
  }
  return true;
}

static bool near(double a, double b, double eps = 1e-4) {
  return fabs(a - b) <= eps;
}

static std::string readFile(const char* path) {
  std::string s;
  FILE* f = fopen(path, "rb");
  if (!f) {
    printf("  \033[31mmissing fixture %s\033[0m\n", path);
    failures++;
    return s;
  }
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static std::string replaceOnce(const std::string& s, const char* from, const char* to) {
  std::string r = s;
  size_t at = r.find(from);
  if (at != std::string::npos) r.replace(at, strlen(from), to);
  return r;
}

// HTTP chunked framing of `body` in pieces of `step` (the shape Open-Meteo sends).
static std::string chunked(const std::string& body, size_t step) {
  std::string out;
  char hdr[32];
  for (size_t i = 0; i < body.size(); i += step) {
    const size_t n = body.size() - i < step ? body.size() - i : step;
    snprintf(hdr, sizeof(hdr), "%zx\r\n", n);
    out += hdr;
    out.append(body, i, n);
    out += "\r\n";
  }
  out += "0\r\n\r\n";
  return out;
}

// ── the phone's font (test_almanac_lines.cpp's copy of SmoothFont's maths) ───────────────────

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
      if (w == 0 && f->dx[g] < 0) w -= f->dx[g];
      w += p[1] ? f->adv[g] : (f->dx[g] + f->w[g]);
    } else {
      w += f->space + 1;
    }
  }
  return w;
}

static Font7 g_font;
static const int ROW_W = 240 - 8;        // MenuWidget draws a row from leftOffset 8: maxW 232
static const int WRAP_W = 220;           // app_almanac.cpp: ALM_WRAP_W

struct WrapAcc { int rows; bool fit; std::string joined; };
static size_t wrapFit(const char* s, void* ctx) {
  size_t n = strlen(s), best = 0;
  std::string b;
  for (size_t k = 1; k <= n; k++) {
    b.assign(s, k);
    if (textWidth(&g_font, b.c_str()) <= WRAP_W) best = k;
  }
  return best;
}
static void wrapRow(const char* s, size_t len, void* ctx) {
  WrapAcc* a = (WrapAcc*)ctx;
  std::string b(s, len);
  if (textWidth(&g_font, b.c_str()) > WRAP_W) a->fit = false;
  a->joined += b;
  a->rows++;
}
static bool wrapsWhole(const char* text, int* rowsOut = NULL) {
  WrapAcc a;
  a.rows = 0;
  a.fit = true;
  wrapNote(text, wrapFit, wrapRow, &a, 6);
  std::string x;
  for (const char* p = text; *p; p++) if (*p != ' ') x += *p;
  std::string y;
  for (char ch : a.joined) if (ch != ' ') y += ch;
  if (rowsOut) *rowsOut = a.rows;
  return a.fit && a.rows <= 6 && x == y;
}

// ── collecting rows ──────────────────────────────────────────────────────────────────────────

struct Rows {
  std::vector<int> kind;
  std::vector<std::string> text;
  int find(const char* prefix) const {
    for (size_t i = 0; i < text.size(); i++) {
      if (text[i].compare(0, strlen(prefix), prefix) == 0) return (int)i;
    }
    return -1;
  }
  bool has(const char* exact) const {
    for (size_t i = 0; i < text.size(); i++) if (text[i] == exact) return true;
    return false;
  }
  void dump(const char* title) const {
    printf("    -- %s --\n", title);
    for (size_t i = 0; i < text.size(); i++) printf("    %3d  %s\n", kind[i], text[i].c_str());
  }
};

static void collect(void* ctx, int kind, const char* text) {
  Rows* r = (Rows*)ctx;
  r->kind.push_back(kind);
  r->text.push_back(text);
}

static int g_rowsMeasured = 0, g_wrapsMeasured = 0, g_widest = 0;
static std::string g_widestText;

/* Every one-line row fits 232 px, every wrapped one (plain or an alert's colour) wraps whole into
 * at most 6 rows of 220 px, every character has a glyph, no row is empty. */
static bool fitsAll(const Rows& r, const char* where) {
  bool good = true;
  for (size_t i = 0; i < r.text.size(); i++) {
    const char* t = r.text[i].c_str();
    for (const char* p = t; *p; p++) {
      int g = 0;
      if (*p != ' ' && !fontGlyph(&g_font, (unsigned char)*p, &g)) {
        printf("    no glyph for '%c' in \"%s\" (%s)\n", *p, t, where);
        good = false;
      }
    }
    if (!t[0]) {
      printf("    an empty row (%s)\n", where);
      good = false;
    }
    const int k = r.kind[i];
    if (k == ALM_ROW_WRAP || k == ALM_ROW_WARN || k == ALM_ROW_DANGER) {
      g_wrapsMeasured++;
      if (!wrapsWhole(t)) {
        printf("    \"%s\" does not wrap whole into 6 rows of %d px (%s)\n", t, WRAP_W, where);
        good = false;
      }
      continue;
    }
    g_rowsMeasured++;
    const int w = textWidth(&g_font, t);
    if (w > g_widest) {
      g_widest = w;
      g_widestText = t;
    }
    if (w > ROW_W) {
      printf("    \"%s\" is %d px, a row has %d (%s)\n", t, w, ROW_W, where);
      good = false;
    }
  }
  return good;
}

// ── shared state ─────────────────────────────────────────────────────────────────────────────

static const int64_t FETCH = 1791067222;          // the fixture's Date: Sat, 03 Oct 2026 22:40:22 GMT
static const int PDT = -7 * 3600;

static AlmCtx ctxAt(int64_t now, int tzS, int units, const WxView* v) {
  AlmCtx c;
  memset(&c, 0, sizeof(c));
  c.clockKnown = true;
  c.now = now;
  c.tzS = tzS;
  c.clockSrc = "ntp";
  c.placeKind = ALM_PLACE_GPS;
  c.placeName = "GPS";
  c.lat = 47.61;
  c.lon = -122.33;
  c.units = units;
  c.usDst = true;
  c.wx = v;
  return c;
}

static Rows weatherRows(const AlmCtx& c) {
  Rows r;
  almLinesWeather(&c, collect, &r);
  return r;
}

static WxData g_seattle;                           // the fixture, folded as the phone would

// ── the groups ───────────────────────────────────────────────────────────────────────────────

static void testJsonNumbers() {
  group("json_read: numbers and null (Open-Meteo's arrays)");
  const char* body = "[17.1, null,-3,0,1e2,-0.5E-1,12.,01,\"x\",true]";
  Js j = { body, body + strlen(body) };
  ok(jsOpen(&j, '['), "an array opens");
  double v = 0;
  int r;
  jsArrNext(&j); r = jsNumber(&j, &v); ok(r == 1 && near(v, 17.1), "17.1");
  jsArrNext(&j); v = 99; r = jsNumber(&j, &v); ok(r == 0 && v == 99, "null: 0, the value untouched");
  jsArrNext(&j); r = jsNumber(&j, &v); ok(r == 1 && v == -3, "-3");
  jsArrNext(&j); r = jsNumber(&j, &v); ok(r == 1 && v == 0, "0");
  jsArrNext(&j); r = jsNumber(&j, &v); ok(r == 1 && v == 100, "1e2");
  jsArrNext(&j); r = jsNumber(&j, &v); ok(r == 1 && near(v, -0.05), "-0.5E-1");
  jsArrNext(&j); r = jsNumber(&j, &v); ok(r == -1, "12. is not a JSON number (skipped)");
  jsArrNext(&j); r = jsNumber(&j, &v); ok(r == -1, "01 is not a JSON number");
  jsArrNext(&j); r = jsNumber(&j, &v); ok(r == -1, "a string is not a number (skipped)");
  jsArrNext(&j); r = jsNumber(&j, &v); ok(r == -1, "true is not a number (skipped)");
  ok(jsArrNext(&j) == 0, "...and the array closes after them");
  const char* cut = "[17.";
  Js k = { cut, cut + strlen(cut) };
  jsOpen(&k, '[');
  jsArrNext(&k);
  ok(jsNumber(&k, &v) == -1, "a body cut mid-number is not a number");
  const char* cut2 = "[17";
  Js k2 = { cut2, cut2 + strlen(cut2) };
  jsOpen(&k2, '[');
  jsArrNext(&k2);
  ok(jsNumber(&k2, &v) == -1, "nor one cut before its delimiter");
}

static void testUrls() {
  group("the queries: rounded points, the elevation, nothing else");
  char u[700];
  wxOpenMeteoUrl(47.6149, -122.3349, false, 0, u, sizeof(u));
  ok(is(u, "https://api.open-meteo.com/v1/forecast?latitude=47.61&longitude=-122.33"
           "&current=temperature_2m,apparent_temperature,relative_humidity_2m,pressure_msl,"
           "wind_speed_10m,wind_direction_10m,wind_gusts_10m,weather_code"
           "&hourly=temperature_2m,precipitation_probability,weather_code,wind_speed_10m"
           "&forecast_hours=48&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "precipitation_probability_max,precipitation_sum,wind_speed_10m_max,wind_gusts_10m_max,"
           "wind_direction_10m_dominant&forecast_days=7&timeformat=unixtime&timezone=auto"
           "&wind_speed_unit=ms"),
     "Open-Meteo: the spec's query exactly, the point at 2 decimals, no sunrise, no gzip");
  ok(!strstr(u, "sunrise") && !strstr(u, "gzip") && !strstr(u, "elevation"), "...and no sunrise / gzip / elevation");
  wxOpenMeteoUrl(47.61, -122.33, true, 147.0, u, sizeof(u));
  ok(strstr(u, "&elevation=150") != NULL && strlen(strstr(u, "&elevation=")) == strlen("&elevation=150"),
     "the ground under the place, rounded to 10 m, last (147 -> 150)");
  wxOpenMeteoUrl(47.61, -122.33, true, 1204.9, u, sizeof(u));
  ok(strstr(u, "&elevation=1200") != NULL, "1204.9 -> 1200");
  wxOpenMeteoUrl(47.61, -122.33, true, NAN, u, sizeof(u));
  ok(!strstr(u, "elevation"), "a NaN ground is never sent");
  char small[40];
  const size_t need = wxOpenMeteoUrl(47.61, -122.33, false, 0, small, sizeof(small));
  ok(small[0] == '\0' && need > 400, "a buffer too small gets \"\" (never a cut URL) and the length needed");
  wxNwsUrl(47.61, -122.33, u, sizeof(u));
  ok(is(u, "https://api.weather.gov/alerts/active?point=47.61,-122.33&status=actual"), "NWS: 4 dp, trailing zeros stripped, status=actual");
  wxNwsUrl(45.0, -130.0, u, sizeof(u));
  ok(is(u, "https://api.weather.gov/alerts/active?point=45,-130&status=actual"), "NWS: \"45,-130\" (the offshore point)");
  wxNwsUrl(47.612468, -122.331279, u, sizeof(u));
  ok(is(u, "https://api.weather.gov/alerts/active?point=47.6125,-122.3313&status=actual"), "NWS: 6 decimals rounded to 4");
  char c[24];
  wxFmtCoord(-0.001, 2, c, sizeof(c));
  ok(is(c, "0"), "never \"-0\"");
  wxFmtCoord(47.6, 2, c, sizeof(c));
  ok(is(c, "47.6"), "47.60 -> 47.6");
  ok(wxE2(47.615) == 4762 || wxE2(47.615) == 4761, "0.01-degree units");
  ok(wxE2(-122.335) == -12234 || wxE2(-122.335) == -12233, "...negative too");
  ok(wxE2(-122.33) == -12233 && wxE2(47.61) == 4761, "the Seattle point: 4761, -12233");
}

static void testOpenMeteo(const std::string& om) {
  group("Open-Meteo: the Seattle fixture (current, 48 hours, 7 days, SI)");
  WxForecast f;
  char reason[WX_REASON_MAX];
  ok(wxParseOpenMeteo(om.data(), om.size(), &f, reason, sizeof(reason)) == WX_OM_OK, "parses");
  ok(f.haveOffset && f.utcOffsetS == -25200, "utc_offset_seconds -25200 (PDT)");
  ok(f.haveNow && near(f.now.tempC, 17.2) && near(f.now.feelsC, 17.6) && near(f.now.rhPct, 85) &&
     near(f.now.pressHpa, 1017.9, 1e-3) && near(f.now.windMs, 2.34) && near(f.now.windDirDeg, 5) &&
     near(f.now.gustMs, 3.6) && f.now.code == 1, "current: 17.2 C, feels 17.6, 85 %, 1017.9 hPa, 2.34 m/s from 5, gusts 3.6, code 1");
  ok(f.nHours == 48 && f.hour[0].t == 1791064800 && f.hour[47].t == 1791064800 + 47 * 3600,
     "48 hourly entries, an hour apart");
  ok(near(f.hour[0].tempC, 17.1) && f.hour[0].code == 2 && near(f.hour[0].popPct, 0) && near(f.hour[0].windMs, 2.85),
     "hour 0: 17.1 C, code 2, 0 %, 2.85 m/s");
  ok(f.nDays == 7 && f.day[0].t == 1791010800 && f.day[0].code == 45 && near(f.day[0].maxC, 18) &&
     near(f.day[0].minC, 12) && near(f.day[0].dirDeg, 23), "day 0: the location's midnight 1791010800, code 45 (fog), 18/12, from 23");
  ok(f.day[6].code == 53 && near(f.day[6].precipMm, 2.7) && near(f.day[6].gustMs, 8.2) && near(f.day[6].popPct, 27),
     "day 6: drizzle, 2.7 mm, gusts 8.2 m/s, 27 %");

  // The same body CHUNKED, fed to the de-chunker one byte at a time, then parsed.
  const std::string wire = chunked(om, 188);
  GeminiDechunk d;
  geminiDechunkInit(&d);
  std::vector<char> out(om.size() + 16);
  size_t got = 0;
  bool good = true;
  for (size_t i = 0; i < wire.size(); i++) {
    good = good && geminiDechunkFeed(&d, (const uint8_t*)&wire[i], 1, out.data(), out.size(), &got);
  }
  ok(good && d.done && got == om.size() && memcmp(out.data(), om.data(), got) == 0,
     "a chunked body fed byte by byte de-chunks to the same bytes");
  WxForecast g;
  ok(wxParseOpenMeteo(out.data(), got, &g, NULL, 0) == WX_OM_OK && g.nHours == 48 && g.nDays == 7 &&
     near(g.now.tempC, 17.2), "...and parses the same");

  group("Open-Meteo: null in any array, errors, a captive portal");
  std::string n = om;
  n = replaceOnce(n, "\"temperature_2m\":[17.1,", "\"temperature_2m\":[null,");
  n = replaceOnce(n, "\"weather_code\":[45,", "\"weather_code\":[null,");
  n = replaceOnce(n, "\"temperature_2m\":17.2,", "\"temperature_2m\":null,");
  n = replaceOnce(n, "\"time\":[1791064800,1791068400,", "\"time\":[1791064800,null,");
  n = replaceOnce(n, "\"precipitation_sum\":[0.00,", "\"precipitation_sum\":[null,");
  ok(wxParseOpenMeteo(n.data(), n.size(), &f, NULL, 0) == WX_OM_OK, "a body with nulls parses");
  ok(!wxHave(f.hour[0].tempC) && f.hour[0].t == 1791064800, "hourly temperature null -> NaN, the hour kept");
  ok(f.nHours == 47 && f.hour[1].t == 1791072000, "an hour whose TIME is null is dropped (47 left, in order)");
  ok(f.day[0].code == -1 && !wxHave(f.day[0].precipMm) && near(f.day[0].maxC, 18), "daily code null -> -1, precip null -> NaN");
  ok(!wxHave(f.now.tempC) && near(f.now.feelsC, 17.6), "current temperature null -> NaN, the rest kept");
  const char* err = "{\"error\":true,\"reason\":\"Latitude must be in range of -90 to 90\\u00b0. Given: 91.0.\"}";
  ok(wxParseOpenMeteo(err, strlen(err), &f, reason, sizeof(reason)) == WX_OM_ERROR &&
     strstr(reason, "Latitude must be in range") != NULL && strchr(reason, '?') != NULL,
     "a 400's {error, reason}: WX_OM_ERROR, the reason kept in ASCII (the degree sign folded)");
  const char* html = "<!DOCTYPE html><html><head><title>Hotel WiFi - log in</title></head><body>Accept</body></html>";
  ok(wxParseOpenMeteo(html, strlen(html), &f, NULL, 0) == WX_OM_BAD, "a captive portal's HTML 200 is not a forecast");
  ok(wxParseOpenMeteo(om.data(), om.size() / 2, &f, NULL, 0) == WX_OM_BAD, "a body cut in half is not a forecast");
  const char* noArr = "{\"latitude\":0,\"current\":{\"temperature_2m\":3}}";
  ok(wxParseOpenMeteo(noArr, strlen(noArr), &f, NULL, 0) == WX_OM_BAD, "JSON with no hourly and no daily is not a forecast");
  ok(wxParseOpenMeteo(NULL, 0, &f, NULL, 0) == WX_OM_BAD, "no body");
}

static void testTimes() {
  group("times: ISO 8601 with the office's offset, the HTTP Date header");
  int64_t t = 0;
  ok(wxParseIso("2026-10-03T20:15:00-04:00", &t) && t == 1791072900, "20:15 -04:00 = 00:15 UTC the next day");
  ok(wxParseIso("2026-10-04T10:00:00-05:00", &t) && t == 1791126000, "-05:00");
  ok(wxParseIso("2026-10-03T22:13:26+00:00", &t) && t == 1791065606, "+00:00 (NWS `updated`)");
  ok(wxParseIso("2026-10-03T22:13:26Z", &t) && t == 1791065606, "Z");
  ok(wxParseIso("2026-10-03T22:13:26.123Z", &t) && t == 1791065606, "fractional seconds");
  ok(wxParseIso("2026-10-04T05:30:00+05:30", &t) && t == 1791072000, "+05:30");
  ok(!wxParseIso("2026-10-03T22:13:26", &t), "no offset: never guessed");
  ok(!wxParseIso("2026-13-03T22:13:26Z", &t) && !wxParseIso("garbage", &t) && !wxParseIso("", &t), "junk refused");
  ok(wxParseHttpDate("Sat, 03 Oct 2026 22:40:22 GMT", &t) && t == FETCH, "the Date header -> the fetch stamp");
  ok(!wxParseHttpDate("Sat, 03 Okt 2026 22:40:22 GMT", &t) && !wxParseHttpDate("1791067222", &t), "junk refused");
}

static int nwsParse(const std::string& b, int code, size_t cap, WxAlerts* out, int* counted) {
  WxEventCounter c;
  wxCountInit(&c);
  for (size_t i = 0; i < b.size(); i++) wxCountFeed(&c, &b[i], 1);   // byte by byte, ALL of it
  if (counted) *counted = c.count;
  const bool overflow = b.size() > cap;
  return wxParseNwsAlerts(code, b.data(), overflow ? cap : b.size(), overflow, c.count, out);
}

static void testNws(const std::string& empty, const std::string& offshore, const std::string& outside,
                    const std::string& two, const std::string& mixed) {
  group("NWS: the four answers - never a bare \"No alerts\"");
  WxAlerts a;
  int counted = 0;
  ok(nwsParse(empty, 200, 65536, &a, &counted) == WX_AL_NONE && a.n == 0 && a.total == 0 && counted == 0,
     "Seattle, an empty 200: NONE (\"No alerts at 15:40\")");
  int64_t up = 0;
  wxParseIso("2026-10-03T22:36:28+00:00", &up);
  ok(a.updated == up, "...its `updated` kept (the last fallback stamp)");
  ok(empty.size() < 300, "an empty answer is ~230 B (the size that makes a big one proof)");
  ok(nwsParse(offshore, 200, 65536, &a, NULL) == WX_AL_NONE, "offshore 45,-130 (a marine zone): an empty 200 too - NONE, not \"outside\"");
  ok(nwsParse(outside, 400, 65536, &a, NULL) == WX_AL_OUTSIDE, "the 400 out of bounds: OUTSIDE (\"Alerts: US only (NWS)\")");
  const std::string other400 = replaceOnce(outside, "out of bounds", "not a valid number");
  ok(nwsParse(other400, 400, 65536, &a, NULL) == WX_AL_FAILED, "any OTHER 400 is a failure, not \"outside the US\"");
  ok(nwsParse(empty, 503, 65536, &a, NULL) == WX_AL_FAILED, "a 503: not checked");
  const char* html = "<html><body><h1>Access Denied</h1></body></html>";
  ok(wxParseNwsAlerts(403, html, strlen(html), false, 0, &a) == WX_AL_FAILED, "Akamai's 403 HTML (no User-Agent): not checked");
  ok(wxParseNwsAlerts(200, html, strlen(html), false, 0, &a) == WX_AL_FAILED, "an HTML 200: not checked");
  ok(nwsParse(two.substr(0, two.size() - 40), 200, 65536, &a, NULL) == WX_AL_FAILED, "a cut 200 that was not an overflow: not checked");

  group("NWS: the synthetic two-alert body (14.7 KB as served)");
  ok(two.size() > 14000 && two.size() < 15500, "the body is ~14.7 KB");
  ok(nwsParse(two, 200, 65536, &a, &counted) == WX_AL_OK && a.n == 2 && a.kept == 2 && a.total == 2,
     "two alerts listed");
  ok(counted == 2, "the streaming counter saw 2 features, byte by byte");
  ok(is(a.a[0].event, "Flash Flood Warning") && a.a[0].severity == WX_SEV_SEVERE &&
     a.a[0].urgency == WX_URG_IMMEDIATE, "Severe first: Flash Flood Warning");
  ok(is(a.a[1].event, "Flood Advisory") && a.a[1].severity == WX_SEV_MINOR, "then Minor: Flood Advisory");
  ok(a.a[0].ends == 1791072900 && a.a[0].expires == 1791072900 && a.a[0].onset == 1791065340,
     "the office's -04:00 times in UTC (ends 00:15Z)");
  ok(strstr(a.a[0].headline, "Flash Flood Warning issued October 3") == a.a[0].headline, "the headline kept");

  group("NWS: Test, Past, Cancel, a null `ends` and headline, sorting");
  ok(nwsParse(mixed, 200, 65536, &a, &counted) == WX_AL_OK, "parses");
  ok(a.total == 7 && counted == 7 && a.kept == 4 && a.n == 4, "7 features: the Test, the Past and the Cancel dropped, 4 kept");
  ok(is(a.a[0].event, "Tornado Warning") && a.a[0].severity == WX_SEV_EXTREME, "Extreme first");
  ok(is(a.a[1].event, "Winter Storm Watch") && a.a[1].severity == WX_SEV_SEVERE, "then Severe");
  ok(is(a.a[2].event, "Special Weather Statement") && a.a[2].ends == 0 && a.a[2].headline[0] == '\0' &&
     a.a[2].expires > 0, "then Moderate: `ends` null -> 0 (expires stands in), headline null -> \"\"");
  ok(is(a.a[3].event, "Flood Advisory"), "then Minor");
  bool noTest = true;
  for (int i = 0; i < a.n; i++) {
    if (strstr(a.a[i].event, "Test") || strstr(a.a[i].event, "Gale")) noTest = false;
  }
  ok(noTest, "\"Required Weekly Test\" (status Test) and the Past \"Gale Watch\" are never listed");

  group("NWS: a body too big to keep still proves alerts exist");
  ok(nwsParse(mixed, 200, 8000, &a, &counted) == WX_AL_TOO_MANY && a.total == 7,
     "34 KB into an 8 KB buffer: TOO_MANY, 7 counted from ALL the bytes");
  ok(nwsParse(two, 200, 600, &a, NULL) == WX_AL_TOO_MANY && a.total == 2 && a.n == 0,
     "two alerts into 600 B: TOO_MANY (2), none whole enough to list");
  ok(wxParseNwsAlerts(200, two.data(), 600, true, 0, &a) == WX_AL_TOO_MANY && a.total >= 1,
     "even with the counter at 0, an overflow is \"1+\", never none");
  // The counter, fed the CHUNKED wire's de-chunked bytes in odd splits.
  const std::string wire = chunked(mixed, 1000);
  GeminiDechunk d;
  geminiDechunkInit(&d);
  WxEventCounter c;
  wxCountInit(&c);
  char dec[64];
  for (size_t i = 0; i < wire.size(); i += 7) {
    size_t dn = 0;
    const size_t n = wire.size() - i < 7 ? wire.size() - i : 7;
    geminiDechunkFeed(&d, (const uint8_t*)&wire[i], n, dec, sizeof(dec), &dn);
    wxCountFeed(&c, dec, dn);
  }
  ok(d.done && c.count == 7, "the counter on a chunked body de-chunked in 7-byte reads: 7");
  WxEventCounter e;
  wxCountInit(&e);
  const char* tricky = "{\"eventCode\":1, \"event\" :\"A\", \"x\":\"\\\"event\\\": no\", \"\"event\": \"B\"}";
  wxCountFeed(&e, tricky, strlen(tricky));
  ok(e.count == 2, "\"eventCode\" and an escaped \\\"event\\\" are not counted; '\"\"event\"' still is");
}

static void testCodes() {
  group("the WMO weather codes: the short table, \"Code N\" otherwise");
  char b[16];
  ok(is(wxCodeText(0, b, sizeof(b)), "Clear") && is(wxCodeText(3, b, sizeof(b)), "Overcast") &&
     is(wxCodeText(45, b, sizeof(b)), "Fog") && is(wxCodeText(99, b, sizeof(b)), "T-storm+hvy hail"),
     "0 Clear, 3 Overcast, 45 Fog, 99 T-storm+hvy hail");
  ok(is(wxCodeText(42, b, sizeof(b)), "Code 42") && is(wxCodeText(-1, b, sizeof(b)), "-"), "unknown: \"Code 42\"; none: \"-\"");
  ok(wxCodeIsSnow(73) && wxCodeIsSnow(86) && !wxCodeIsSnow(63) && !wxCodeIsSnow(95), "snow codes: 71-77, 85-86");
}

static WxFetch fetchOf(const WxForecast* fc, const WxAlerts* al, int64_t asked, int64_t date) {
  WxFetch f;
  memset(&f, 0, sizeof(f));
  f.latE2 = 4761;
  f.lonE2 = -12233;
  f.placeKind = ALM_PLACE_GPS;
  snprintf(f.placeName, sizeof(f.placeName), "GPS");
  f.elevM = 150;
  f.askedUtc = asked;
  f.dateUtc = date;
  if (fc) {
    f.fc = *fc;
    f.omParsed = true;
  }
  f.nwsTried = true;
  if (al) {
    f.al = *al;
  } else {
    f.al.state = WX_AL_FAILED;
  }
  return f;
}

static void testFoldAndCache(const std::string& om, const std::string& empty, const std::string& two) {
  group("the fold: one record, the forecast and the alerts each with their own as-of");
  WxForecast fc;
  wxParseOpenMeteo(om.data(), om.size(), &fc, NULL, 0);
  WxAlerts none, alerts;
  nwsParse(empty, 200, 65536, &none, NULL);
  nwsParse(two, 200, 65536, &alerts, NULL);
  WxData d;
  wxDataClear(&d);
  WxFetch f = fetchOf(&fc, &none, FETCH, 0);
  ok(wxFold(&d, &f) && d.have && d.fetchedUtc == FETCH && d.trusted && d.alertsUtc == FETCH &&
     d.al.state == WX_AL_NONE && d.omLast == WX_HOST_OK && d.nwsLast == WX_HOST_OK,
     "a good fetch: the trusted clock stamps both");
  g_seattle = d;
  WxFetch u = fetchOf(&fc, &none, 0, FETCH + 60);
  WxData d2;
  wxDataClear(&d2);
  wxFold(&d2, &u);
  ok(d2.fetchedUtc == FETCH + 60 && !d2.trusted, "no trusted clock: the HTTP Date header stamps it (not trusted)");
  WxFetch v = fetchOf(&fc, &none, 0, 0);
  wxDataClear(&d2);
  wxFold(&d2, &v);
  ok(d2.fetchedUtc == none.updated && !d2.trusted, "no Date either: NWS `updated`");
  ok(d2.fetchedUtc != 1791066600, "never Open-Meteo's current.time (a 15-minute model slot)");

  // A later fetch: the forecast fails (a captive portal), the alerts answer.
  WxData d3 = d;
  WxFetch p = fetchOf(NULL, &alerts, FETCH + 3600, 0);
  wxFold(&d3, &p);
  ok(d3.have && d3.fetchedUtc == FETCH && d3.fc.nHours == 48 && d3.omLast == WX_HOST_FAILED,
     "a forecast that did not parse (an HTML 200) never replaces the cache");
  ok(d3.al.state == WX_AL_OK && d3.alertsUtc == FETCH + 3600, "...the alerts that answered do, with their own as-of");
  // Then the alerts fail and the forecast answers.
  WxData d4 = d3;
  WxFetch q = fetchOf(&fc, NULL, FETCH + 7200, 0);
  wxFold(&d4, &q);
  ok(d4.fetchedUtc == FETCH + 7200 && d4.al.state == WX_AL_OK && d4.al.n == 2 && d4.alertsUtc == FETCH + 3600 &&
     d4.nwsLast == WX_HOST_FAILED, "an alerts failure KEEPS the previous alerts, marked with THEIR as-of");
  // ...unless the forecast moved over 5 km: those alerts were someone else's.
  WxData d5 = d3;
  WxFetch m = fetchOf(&fc, NULL, FETCH + 7200, 0);
  m.latE2 = 4761 + 10;                            // 0.1 deg north: ~11 km
  wxFold(&d5, &m);
  ok(d5.al.state == WX_AL_UNKNOWN && d5.al.n == 0, "a new place over 5 km away drops the old place's alerts");

  group("the cache file: wiphone-wx 1 ... end, a round trip, anything else empty");
  std::vector<char> buf(wxCacheSave(&d4, NULL, 0) + 1);
  const size_t n = wxCacheSave(&d4, buf.data(), buf.size());
  ok(n == strlen(buf.data()) && strncmp(buf.data(), "wiphone-wx 1\n", 13) == 0 &&
     strcmp(buf.data() + n - 4, "end\n") == 0, "\"wiphone-wx 1\" ... \"end\"");
  ok(strstr(buf.data(), "point 4761 -12233 1 GPS\n") != NULL, "the ROUNDED point, the place source");
  WxData back;
  ok(wxCacheLoad(buf.data(), n, &back), "it loads");
  std::vector<char> again(n + 1);
  wxCacheSave(&back, again.data(), again.size());
  ok(strcmp(buf.data(), again.data()) == 0, "...and saves again to the same bytes");
  ok(back.fc.nHours == 48 && back.fc.nDays == 7 && near(back.fc.now.tempC, 17.2) && back.al.n == 2 &&
     back.alertsUtc == FETCH + 3600 && back.nwsLast == WX_HOST_FAILED && back.fc.utcOffsetS == -25200 &&
     back.elevM == 150 && is(back.al.a[0].event, "Flash Flood Warning") &&
     strstr(back.al.a[0].headline, "Flash Flood Warning issued") != NULL, "every field back");
  // NaN survives.
  WxData nan = d4;
  nan.fc.hour[3].tempC = NAN;
  nan.fc.day[2].code = -1;
  std::vector<char> nb(wxCacheSave(&nan, NULL, 0) + 1);
  wxCacheSave(&nan, nb.data(), nb.size());
  WxData nb2;
  ok(wxCacheLoad(nb.data(), strlen(nb.data()), &nb2) && !wxHave(nb2.fc.hour[3].tempC) && nb2.fc.day[2].code == -1,
     "a missing value round-trips as \"-\"");
  const std::string good(buf.data(), n);
  struct Bad { const char* what; std::string text; };
  std::vector<Bad> bad;
  bad.push_back({ "no \"end\" (a cut-off write)", good.substr(0, good.size() - 4) });
  bad.push_back({ "cut mid-line", good.substr(0, good.size() / 2) });
  bad.push_back({ "another version", replaceOnce(good, "wiphone-wx 1", "wiphone-wx 2") });
  bad.push_back({ "a line it does not know", replaceOnce(good, "hosts ", "junk 1\nhosts ") });
  bad.push_back({ "a bad number", replaceOnce(good, "hour 1791064800 17.1", "hour 1791064800 17.x") });
  bad.push_back({ "something after end", good + "hour 1 2 3 4 5\n" });
  bad.push_back({ "a headline with no alert", replaceOnce(good, "alerts ", "headline stray\nalerts ") });
  bad.push_back({ "half the alerts' point", replaceOnce(good, " 4761 -12233\nalert ", " 4761\nalert ") });
  bad.push_back({ "an empty file", std::string() });
  bad.push_back({ "binary junk", std::string("\x01\x02\x03", 3) });
  for (size_t i = 0; i < bad.size(); i++) {
    WxData x;
    char what[96];
    snprintf(what, sizeof(what), "%s -> EMPTY, never half a forecast", bad[i].what);
    ok(!wxCacheLoad(bad[i].text.data(), bad[i].text.size(), &x) && !x.have && x.fc.nHours == 0 && x.al.n == 0, what);
  }
  std::string tooMany = good;
  for (int i = 0; i < 3; i++) tooMany = replaceOnce(tooMany, "day ", "day 1791010800 0 1 1 1 1 1 1 1\nday ");
  WxData x;
  ok(!wxCacheLoad(tooMany.data(), tooMany.size(), &x), "more days than the record holds -> empty");
}

static void testPolicy() {
  group("when to fetch: one on a stale open with WiFi, and no more");
  WxDueIn in;
  memset(&in, 0, sizeof(in));
  in.wifiUp = true;
  in.placeOk = true;
  in.haveCache = true;
  in.cacheAgeS = 2 * 3600;
  in.movedKm = 0.4;
  ok(wxFetchDue(&in) == WX_DUE_YES, "stale (2 h), WiFi up: fetch");
  WxDueIn t = in;
  t.wifiUp = false;
  ok(wxFetchDue(&t) == WX_DUE_NO_WIFI, "no WiFi: none");
  t = in;
  t.hotspot = true;
  ok(wxFetchDue(&t) == WX_DUE_HOTSPOT, "the phone's own hotspot: none");
  t = in;
  t.cacheAgeS = 1800;
  ok(wxFetchDue(&t) == WX_DUE_FRESH, "fresh (30 min) and near: none");
  t.movedKm = 7.5;
  ok(wxFetchDue(&t) == WX_DUE_YES, "fresh but fetched 7.5 km away: fetch (a tolerance, not a grid)");
  t = in;
  t.cacheAgeS = -1;
  ok(wxFetchDue(&t) == WX_DUE_YES, "an age unknown (no trusted clock, an old boot's cache): fetch");
  t = in;
  t.placeOk = false;
  ok(wxFetchDue(&t) == WX_DUE_NO_PLACE, "no place: none");
  t = in;
  t.musicPlaying = true;
  ok(wxFetchDue(&t) == WX_DUE_MUSIC, "music playing: no AUTOMATIC fetch");
  t.explicitAsk = true;
  ok(wxFetchDue(&t) == WX_DUE_YES, "...but Refresh fetches (and pauses it)");
  t = in;
  t.attempted = true;
  t.lastFailed = true;
  t.sinceAttemptMs = 4 * 60 * 1000;
  ok(wxFetchDue(&t) == WX_DUE_RETRY_WAIT, "a failure 4 min ago: no automatic retry yet");
  t.sinceAttemptMs = 11 * 60 * 1000;
  ok(wxFetchDue(&t) == WX_DUE_YES, "...11 min later: one");
  t.sinceAttemptMs = 60 * 1000;
  t.explicitAsk = true;
  ok(wxFetchDue(&t) == WX_DUE_YES, "Refresh is never held by the retry wait");
  t = in;
  t.explicitAsk = true;
  t.wifiUp = false;
  ok(wxFetchDue(&t) == WX_DUE_NO_WIFI && is(wxDueWhy(WX_DUE_NO_WIFI), "No WiFi - join a network first"),
     "Refresh with no WiFi says why");

  /* Many frames of an open Almanac: the device's loop as weather_net runs it - stamped BEFORE the
   * request, busy while out, the result folded. Count the fetches started. */
  for (int outcome = 0; outcome < 2; outcome++) {
    bool attempted = false, failed = false, busy = false;
    uint32_t attemptMs = 0;
    int64_t age = 2 * 3600;
    int started = 0;
    for (uint32_t ms = 0; ms < 9 * 60 * 1000; ms += 250) {      // nine minutes of 250 ms polls
      WxDueIn f = in;
      f.attempted = attempted;
      f.lastFailed = failed;
      f.sinceAttemptMs = ms - attemptMs;
      f.busy = busy;
      f.cacheAgeS = age;
      if (wxFetchDue(&f) == WX_DUE_YES) {
        started++;
        attempted = true;
        attemptMs = ms;                                         // before the request
        busy = true;
      }
      if (busy && ms - attemptMs >= 6000) {                     // it lands 6 s later
        busy = false;
        failed = outcome == 0;
        if (!failed) age = 0;
      }
      if (!failed && attempted) age = (ms - attemptMs) / 1000;
    }
    ok(started == 1, outcome == 0 ? "a failing fetch across 2,160 frames: ONE attempt (the retry wait)"
                                  : "a good fetch across 2,160 frames: ONE (then fresh)");
  }
  ok(near(wxDistanceKm(47.61, -122.33, 47.61, -122.33), 0) && near(wxDistanceKm(47.61, -122.33, 47.71, -122.33), 11.12, 0.05),
     "the distance: 0.1 deg of latitude = 11.1 km");
}

static void testRows(const std::string& om) {
  group("the WEATHER screen: alerts, now, hours, days, as-of, credit (metric)");
  WxView v;
  memset(&v, 0, sizeof(v));
  v.d = &g_seattle;
  v.clockTrusted = true;
  v.note = "";
  const int64_t now = FETCH + 600;                       // Sat 15:50 PDT
  AlmCtx c = ctxAt(now, PDT, UNITS_METRIC, &v);
  Rows r = weatherRows(c);
  r.dump("Seattle, 10 min after the fetch");
  ok(r.has("No alerts at 15:40"), "\"No alerts at 15:40\" (never a bare \"No alerts\")");
  ok(r.find("No alerts") == 0, "alerts first");
  ok(r.has("Now: 17C, Mostly clear"), "Now: 17C, Mostly clear (no degree glyph)");
  ok(r.has("Feels 18C, humidity 85%"), "Feels 18C, humidity 85%");
  ok(r.has("Wind from N 8 km/h, gusts 13"), "Wind from N 8 km/h, gusts 13 (FROM, whole km/h)");
  ok(r.has("Pressure 1018 hPa"), "Pressure 1018 hPa");
  ok(r.has("16:00 18C 0% Clear") && r.has("19:00 15C 0% Clear") && r.find("15:00") < 0,
     "the hours from the next one (past hours dropped), every 3 h");
  ok(r.has("Sun 01:00 11C 0% Clear"), "an hour on another day carries its weekday");
  ok(r.has("Today 18C/12C, Fog, 1%") && r.has("Wind from NNE 10 km/h, gusts 22"), "today's day: 18C/12C, Fog, 1 %; its wind");
  ok(r.has("Fri 18C/11C, Drizzle, 27%") && r.has("2.7 mm, wind from S 15 km/h, gusts 30"), "Friday: drizzle, 2.7 mm");
  ok(r.has("As of 15:40 (10m ago) for GPS"), "As of 15:40 (10m ago) for GPS");
  ok(r.find("Local time here") < 0, "the place's offset is the phone's: no warning");
  ok(r.text.size() >= 2 && r.text[r.text.size() - 2] == "Weather: Open-Meteo.com" &&
     r.text.back() == "(CC BY 4.0); alerts: NWS", "the credit closes the screen");
  ok(fitsAll(r, "Seattle, metric"), "every row fits");

  group("...in US units (F, mph, inHg, in)");
  c = ctxAt(now, PDT, UNITS_US, &v);
  r = weatherRows(c);
  ok(r.has("Now: 63F, Mostly clear") && r.has("Feels 64F, humidity 85%"), "63F, feels 64F");
  ok(r.has("Wind from N 5 mph, gusts 8"), "Wind from N 5 mph, gusts 8");
  ok(r.has("Pressure 30.06 inHg"), "Pressure 30.06 inHg");
  ok(r.has("Fri 65F/51F, Drizzle, 27%") && r.has("0.11 in, wind from S 9 mph, gusts 18"), "Friday: 65F/51F, 0.11 in");
  ok(fitsAll(r, "Seattle, US"), "every row fits");

  group("after an hour: the hourly forecast, labelled as one");
  c = ctxAt(FETCH + 2 * 3600 + 300, PDT, UNITS_METRIC, &v);   // 17:45 PDT
  r = weatherRows(c);
  ok(r.has("Forecast for 17:00: 18C, Clear") && r.find("Now:") < 0, "\"Forecast for 17:00\" - never \"Now\" from an old fetch");
  ok(r.has("Precip 0%, wind 9 km/h"), "its precipitation chance and wind");
  ok(r.has("As of 15:40 (2h 05m ago) for GPS"), "the age keeps up");
  ok(r.find("16:00") < 0 && r.find("17:00 ") < 0 && r.has("18:00 16C 0% Clear"), "the hours start after now");
  c.wx = &v;
  v.clockTrusted = false;
  r = weatherRows(c);
  ok(r.has("As of Sat Oct 3 15:40 for GPS"), "no trusted clock: the stored date and time, no age");
  v.clockTrusted = true;

  group("past the hours, past the days");
  c = ctxAt(1791234000 + 7200, PDT, UNITS_METRIC, &v);       // two hours after the last hour
  r = weatherRows(c);
  ok(r.find("Next hours") < 0 && r.find("Next days") > 0 && r.find("Forecast for") < 0,
     "past the last hourly entry: the days only");
  c = ctxAt(1791529200 + 2 * 86400, PDT, UNITS_METRIC, &v);  // two days after the last day
  r = weatherRows(c);
  ok(r.has("No forecast after Fri Oct 9 - refresh on Wi-Fi") && r.find("Next days") < 0,
     "past the last day: \"No forecast after Fri Oct 9 - refresh on Wi-Fi\"");
  ok(fitsAll(r, "past the forecast"), "every row fits");

  group("days by their MIDPOINT, in the phone's offset");
  int y, m, d, wd;
  wxDayDate(1791010800, -8 * 3600, &y, &m, &d, &wd);
  ok(y == 2026 && m == 10 && d == 3 && wd == 6, "a phone an hour BEHIND the place (UTC-8 vs PDT): Oct 3 stays Sat Oct 3");
  wxDayDate(1791010800, PDT, &y, &m, &d, &wd);
  ok(m == 10 && d == 3, "the place's own offset: Oct 3");
  wxDayDate(1791010800, 3 * 3600, &y, &m, &d, &wd);
  ok(m == 10 && d == 3, "a phone 10 h ahead: Oct 3");
  c = ctxAt(FETCH + 600, -8 * 3600, UNITS_METRIC, &v);
  r = weatherRows(c);
  ok(r.has("Today 18C/12C, Fog, 1%") && r.find("Fri 18C/12C") < 0, "the screen at UTC-8: today's high is TODAY's (not Friday's)");
  ok(r.has("Local time here is UTC-7 - check the clock setting"), "...and it says the place is on UTC-7");
  ok(fitsAll(r, "UTC-8"), "every row fits");
  /* Across the US change (Nov 1 2026): the location's midnights move from 07:00Z to 08:00Z - or,
   * as Open-Meteo actually sends them (ONE offset for the whole answer), stay at 07:00Z. Either
   * way, at either fixed offset, the labels are four consecutive dates. */
  const int64_t real[4] = { 1793430000, 1793516400, 1793606400, 1793692800 };   // Oct 31, Nov 1 (07Z), Nov 2, 3 (08Z)
  const int64_t fixed[4] = { 1793430000, 1793516400, 1793602800, 1793689200 };  // all 07:00Z
  const int tzs[2] = { -7 * 3600, -8 * 3600 };
  bool consecutive = true;
  for (int k = 0; k < 2; k++) {
    for (int z = 0; z < 2; z++) {
      for (int i = 0; i < 4; i++) {
        wxDayDate(k ? fixed[i] : real[i], tzs[z], &y, &m, &d, &wd);
        const int want = i == 0 ? 31 : i;
        const int wantM = i == 0 ? 10 : 11;
        if (m != wantM || d != want) {
          consecutive = false;
          printf("    day %d (%s) at UTC%+d -> %d-%d\n", i, k ? "fixed" : "real", tzs[z] / 3600, m, d);
        }
      }
    }
  }
  ok(consecutive, "across the Nov 1 change: Oct 31, Nov 1, 2, 3 at UTC-7 and UTC-8, either shape");

  group("the alerts' rows: colour, until/from-to, ended, not checked, US only, too many");
  WxData a = g_seattle;
  const std::string mixed = readFile("tests/fixtures/wx/nws_mixed.json");
  nwsParse(mixed, 200, 65536, &a.al, NULL);
  a.alertsUtc = FETCH;
  v.d = &a;
  c = ctxAt(1791126000 - 3600, -5 * 3600, UNITS_METRIC, &v);   // Sun 04:00 CDT: before the watch's onset
  r = weatherRows(c);
  r.dump("four alerts");
  int tor = r.find("Tornado Warning");
  int wsw = r.find("Winter Storm Watch from ");
  ok(tor >= 0 && r.kind[tor] == ALM_ROW_DANGER, "Extreme: red (ALM_ROW_DANGER)");
  ok(wsw >= 0 && r.kind[wsw] == ALM_ROW_DANGER && r.has("Winter Storm Watch from 10:00 to Tue 03:00"),
     "Severe, not begun: \"from 10:00 to Tue 03:00\", red");
  int sws = r.find("Special Weather Statement");
  ok(sws >= 0 && r.kind[sws] == ALM_ROW_WARN, "Moderate: yellow (ALM_ROW_WARN)");
  ok(tor < wsw && wsw < sws, "most severe first");
  ok(r.find("Flood Advisory") < 0, "the advisory that ENDED (Oct 3 21:00 EDT) is hidden offline");
  ok(r.find("Gale") < 0 && r.find("Weekly Test") < 0, "no Past, no Test");
  ok(fitsAll(r, "four alerts"), "every row fits");
  // Not checked, keeping them.
  a.nwsLast = WX_HOST_FAILED;
  r = weatherRows(c);
  ok(r.find("Alerts not checked (no answer)") == 0 && r.find("Tornado Warning") > 0 &&
     r.find("Alerts as of") > 0, "a failed check: \"Alerts not checked\", the last answer kept, with its own as-of");
  a.nwsLast = WX_HOST_OK;
  a.al.state = WX_AL_OUTSIDE;
  a.al.n = 0;
  r = weatherRows(c);
  ok(r.has("Alerts: US only (NWS)"), "outside the US: \"Alerts: US only (NWS)\"");
  a.al.state = WX_AL_TOO_MANY;
  a.al.total = 12;
  r = weatherRows(c);
  ok(r.has("Alerts: 12+ (too many to list)") && r.kind[r.find("Alerts: 12+")] == ALM_ROW_WARN,
     "too many: \"Alerts: 12+ (too many to list)\" - never none");
  v.d = &g_seattle;

  group("TODAY's two rows: the alert near the top, the summary over the entries");
  WxData t = g_seattle;
  nwsParse(mixed, 200, 65536, &t.al, NULL);
  v.d = &t;
  c = ctxAt(1791126000 - 3600, -5 * 3600, UNITS_METRIC, &v);
  Rows tr;
  wxTodayAlertRow(&c, collect, &tr);
  ok(tr.text.size() == 1 && tr.text[0].find("Alert: Tornado Warning") == 0 && tr.kind[0] == ALM_ROW_DANGER,
     "the most severe alert in force, in red");
  v.d = &g_seattle;
  c = ctxAt(FETCH + 600, PDT, UNITS_METRIC, &v);
  Rows sr;
  wxTodayAlertRow(&c, collect, &sr);
  ok(sr.text.empty(), "no alert: no row");
  wxTodaySummaryRow(&c, collect, &sr);
  ok(sr.text.size() == 1 && is(sr.text[0].c_str(), "Weather: 17C, Mostly clear"), "\"Weather: 17C, Mostly clear\"");
  WxData w = g_seattle;
  w.fc.hour[4].popPct = 60;                                // 19:00 PDT
  w.fc.hour[4].code = 63;
  v.d = &w;
  Rows wr;
  wxTodaySummaryRow(&c, collect, &wr);
  ok(wr.text.size() == 1 && is(wr.text[0].c_str(), "Weather: 17C, rain 60% this evening"), "\"Weather: 17C, rain 60% this evening\"");
  w.fc.hour[4].code = 73;
  w.fc.hour[4].t = 1791097200;                              // Sun 00:00 PDT
  Rows sn;
  wxTodaySummaryRow(&c, collect, &sn);
  ok(sn.text.size() == 1 && is(sn.text[0].c_str(), "Weather: 17C, snow 60% tonight"), "snow tonight");
  c.units = UNITS_US;
  Rows us;
  wxTodaySummaryRow(&c, collect, &us);
  ok(us.text.size() == 1 && is(us.text[0].c_str(), "Weather: 63F, snow 60% tonight"), "...in F");
  ok(fitsAll(tr, "today alert") && fitsAll(wr, "today summary"), "both fit");

  group("the TODAY screen carries them (almLinesToday)");
  v.d = &t;
  c = ctxAt(1791126000 - 3600, -5 * 3600, UNITS_METRIC, &v);
  AlmDay day;
  memset(&day, 0, sizeof(day));
  c.day = NULL;
  Rows today;
  almLinesToday(&c, collect, &today);
  const int al = today.find("Alert: ");
  const int sum = today.find("Forecast: ");             // ~15 h after the fetch: a forecast, not "Weather:"
  const int ent = today.find("Weather...");
  ok(al > 0 && al < 4, "the alert row near the top");
  ok(sum > al && ent > sum && today.kind[ent] == ALM_ENTRY_WEATHER, "the summary over the entries, then \"Weather...\"");
  c.wx = NULL;
  Rows plain;
  almLinesToday(&c, collect, &plain);
  ok(plain.find("Alert: ") < 0 && plain.find("Weather: ") < 0 && plain.find("Forecast: ") < 0 &&
     plain.find("Weather...") > 0, "no weather view (the serial `almanac`): no weather rows, the entry still there");
  v.d = &g_seattle;
  (void)om;

  group("TODAY's summary: \"Weather:\" within the hour, \"Forecast:\" after it, \"N km away\" elsewhere");
  Rows fr;
  c = ctxAt(FETCH + 600, PDT, UNITS_METRIC, &v);
  wxTodaySummaryRow(&c, collect, &fr);
  c = ctxAt(FETCH + 2 * 3600 + 300, PDT, UNITS_METRIC, &v);  // 17:45 PDT: past the hour
  wxTodaySummaryRow(&c, collect, &fr);
  c.lat = 48.75;                                             // ~127 km north of the fetch's point
  wxTodaySummaryRow(&c, collect, &fr);
  c = ctxAt(FETCH + 600, PDT, UNITS_METRIC, &v);
  c.lat = 48.75;
  wxTodaySummaryRow(&c, collect, &fr);
  fr.dump("summary: fresh, old, old + moved, fresh + moved");
  ok(fr.text.size() == 4 && fr.text[0].compare(0, 9, "Weather: ") == 0, "10 min after: \"Weather: ...\"");
  ok(fr.text.size() == 4 && is(fr.text[1].c_str(), "Forecast: 18C, Clear"), "2 h after: \"Forecast: 18C, Clear\" (the 17:00 hour)");
  ok(fr.text.size() == 4 && fr.text[2].compare(0, 9, "Forecast ") == 0 &&
     fr.text[2].find(" km away: ") != std::string::npos, "...from 127 km away: \"Forecast N km away: ...\"");
  ok(fr.text.size() == 4 && fr.text[3].compare(0, 8, "Weather ") == 0 &&
     fr.text[3].find(" km away: ") != std::string::npos, "fresh but moved: \"Weather N km away: ...\"");
  ok(fitsAll(fr, "summary labels"), "every label fits");

  group("a fetch of unknown time is never \"Now\" (no trusted clock, no Date, no `updated`)");
  WxData nt = g_seattle;
  nt.fetchedUtc = 0;
  v.d = &nt;
  c = ctxAt(FETCH + 3 * 86400, PDT, UNITS_METRIC, &v);       // three days on, offline
  r = weatherRows(c);
  ok(r.find("Now:") < 0 && r.find("Feels ") < 0 && r.has("Fetched before the clock was set, for GPS"),
     "WEATHER: no \"Now:\" from `current`, the unknown as-of said");
  Rows ns;
  wxTodaySummaryRow(&c, collect, &ns);
  ok(ns.text.size() == 1 && ns.text[0].compare(0, 9, "Forecast ") == 0, "TODAY: \"Forecast...\", never \"Weather:\"");
  c = ctxAt(FETCH + 600, PDT, UNITS_METRIC, &v);             // even when it happens to be fresh
  r = weatherRows(c);
  ok(r.find("Now:") < 0 && r.find("Forecast for ") >= 0, "even 10 min on: the hourly forecast, labelled as one");
  v.d = &g_seattle;

  group("alerts without a forecast carry their own as-of");
  WxData ao;
  wxDataClear(&ao);
  nwsParse(readFile("tests/fixtures/wx/nws_two_alerts.json"), 200, 65536, &ao.al, NULL);
  ao.alertsUtc = FETCH;
  ao.nwsLast = WX_HOST_OK;
  ao.omLast = WX_HOST_FAILED;
  v.d = &ao;
  c = ctxAt(FETCH + 600, PDT, UNITS_METRIC, &v);
  r = weatherRows(c);
  r.dump("alerts only (Open-Meteo failed)");
  ok(r.find("Flash Flood Warning") >= 0 && r.has("No forecast yet") && r.has("Alerts as of 15:40"),
     "the alerts, \"No forecast yet\", and \"Alerts as of 15:40\"");
  ok(fitsAll(r, "alerts only"), "every row fits");
  v.d = &g_seattle;

  group("Open-Meteo failed, NWS ok, after a 127 km move: the alerts are THERE's, with their own point");
  WxAlerts two;
  nwsParse(readFile("tests/fixtures/wx/nws_two_alerts.json"), 200, 65536, &two, NULL);
  WxData mv = g_seattle;
  WxFetch mf = fetchOf(NULL, &two, FETCH + 300, 0);
  mf.latE2 = 4875;                                           // 48.75 N: ~127 km north of Seattle
  wxFold(&mv, &mf);
  ok(mv.have && mv.latE2 == 4761 && mv.lonE2 == -12233 && mv.fetchedUtc == FETCH && mv.omLast == WX_HOST_FAILED,
     "the forecast stays Seattle's (it did not parse)");
  ok(mv.al.state == WX_AL_OK && mv.al.n == 2 && mv.alPointKnown && mv.alLatE2 == 4875 && mv.alLonE2 == -12233 &&
     wxAlertsApart(&mv), "...the alerts are the new place's, at THEIR point");
  std::vector<char> mb(wxCacheSave(&mv, NULL, 0) + 1);
  const size_t mn = wxCacheSave(&mv, mb.data(), mb.size());
  WxData mback;
  ok(strstr(mb.data(), " 4875 -12233\n") != NULL && wxCacheLoad(mb.data(), mn, &mback) && mback.alPointKnown &&
     mback.alLatE2 == 4875 && mback.latE2 == 4761, "the alerts' point rides in the cache and comes back");
  v.d = &mv;
  c = ctxAt(FETCH + 600, PDT, UNITS_METRIC, &v);
  c.lat = 48.75;                                             // read at the new place
  Rows mt;
  wxTodayAlertRow(&c, collect, &mt);
  r = weatherRows(c);
  r.dump("127 km on: the alerts here, the forecast Seattle's");
  ok(mt.text.size() == 1 && mt.text[0].compare(0, 7, "Alert: ") == 0, "TODAY at the new place: \"Alert: ...\" - no km (they ARE here)");
  ok(r.find("Forecast for a place 127 km from here") >= 0 && r.find("Alerts for a place") < 0 &&
     r.find("For a place") < 0, "WEATHER there: the forecast's 127 km said, the alerts' not");
  ok(fitsAll(r, "127 km, there"), "every row fits");
  c = ctxAt(FETCH + 600, PDT, UNITS_METRIC, &v);             // back in Seattle, offline
  Rows ms;
  wxTodayAlertRow(&c, collect, &ms);
  r = weatherRows(c);
  r.dump("back in Seattle: the alerts are 127 km away");
  ok(ms.text.size() == 1 && ms.text[0].compare(0, 19, "Alert 127 km away: ") == 0,
     "TODAY in Seattle: \"Alert 127 km away: ...\" (the alerts' point, not the forecast's)");
  const int awayRow = r.find("Alerts for a place 127 km from here");
  ok(awayRow >= 0 && r.kind[awayRow] == ALM_ROW_WARN && r.find("For a place") < 0 &&
     r.find("Forecast for a place") < 0, "WEATHER in Seattle: \"Alerts for a place 127 km from here\", the forecast's here");
  ok(fitsAll(r, "127 km, Seattle") && fitsAll(ms, "127 km, TODAY"), "every row fits");
  WxData mk = mv;
  WxFetch mf2 = fetchOf(&g_seattle.fc, NULL, FETCH + 900, 0);
  mf2.latE2 = 4875;
  wxFold(&mk, &mf2);
  ok(mk.latE2 == 4875 && mk.al.state == WX_AL_OK && mk.al.n == 2 && !wxAlertsApart(&mk),
     "...then a forecast there, its alerts failing: the alerts KEPT (they are that place's)");
  // A file from before the alerts' own point: the alerts at the forecast's.
  WxData mo = mv;
  mo.alPointKnown = false;
  std::vector<char> ob(wxCacheSave(&mo, NULL, 0) + 1);
  const size_t on = wxCacheSave(&mo, ob.data(), ob.size());
  WxData oback;
  ok(strstr(ob.data(), " 4875 -12233\n") == NULL && wxCacheLoad(ob.data(), on, &oback) && oback.alPointKnown &&
     oback.alLatE2 == 4761 && oback.alLonE2 == -12233 && !wxAlertsApart(&oback),
     "an older file (no alerts' point) loads: the alerts at the forecast's point");
  wxDataClear(&mo);
  mo.al = two;
  mo.nwsLast = WX_HOST_OK;
  ob.assign(wxCacheSave(&mo, NULL, 0) + 1, 0);
  wxCacheSave(&mo, ob.data(), ob.size());
  ok(wxCacheLoad(ob.data(), strlen(ob.data()), &oback) && !oback.alPointKnown && !wxAlertsApart(&oback),
     "...one with no forecast either: no point claimed for them");
  v.d = &g_seattle;

  group("no cache, no clock, the notes");
  WxData empty;
  wxDataClear(&empty);
  v.d = &empty;
  c = ctxAt(FETCH, PDT, UNITS_METRIC, &v);
  r = weatherRows(c);
  ok(r.find("No weather yet") == 0 && fitsAll(r, "empty"), "\"No weather yet\" and how to get it");
  v.d = NULL;
  r = weatherRows(c);
  ok(r.text.size() == 1 && r.text[0] == "Reading the weather...", "not read from the card yet");
  v.d = &g_seattle;
  v.fetching = true;
  v.note = "Weather not updated: the answer was not a forecast (a WiFi login page?)";
  r = weatherRows(c);
  ok(r.find("Fetching the weather...") == 0 && r.find("Weather not updated") == 1 && fitsAll(r, "notes"),
     "\"Fetching the weather...\" and the last failure, over the rest");
  v.fetching = false;
  v.note = "";
  c.clockKnown = false;
  r = weatherRows(c);
  ok(r.has("Clock not set yet"), "no clock: no hours or days to drop");
}

/* Every row the weather can make, measured: both units, every WMO code and an unknown one, the
 * extremes of every value, every one of NWS's 111 event names as an alert (in force, not begun,
 * no end, far dates), the longest place name, every note and every refusal. */
static void testWidths() {
  group("every row measured against Akrobat Bold 20 (232 px; wraps whole at 220)");
  const std::string types = readFile("tests/fixtures/wx/nws_event_types.txt");
  std::vector<std::string> events;
  size_t at = 0;
  while (at < types.size()) {
    size_t nl = types.find('\n', at);
    if (nl == std::string::npos) nl = types.size();
    if (nl > at) events.push_back(types.substr(at, nl - at));
    at = nl + 1;
  }
  ok(events.size() == 111, "NWS's 111 event names");
  size_t longest = 0;
  for (size_t i = 0; i < events.size(); i++) longest = events[i].size() > longest ? events[i].size() : longest;
  ok(longest == 32 && longest < WX_EVENT_MAX, "the longest is 32 characters (WX_EVENT_MAX 40)");
  static const int CODES[] = { 0, 1, 2, 3, 45, 48, 51, 53, 55, 56, 57, 61, 63, 65, 66, 67, 71, 73, 75, 77,
                               80, 81, 82, 85, 86, 95, 96, 97, 99, 42, -1 };
  const int nCodes = (int)(sizeof(CODES) / sizeof(CODES[0]));
  static const float TEMPS[] = { -45.4f, -9.6f, -0.4f, 0.4f, 9.6f, 37.7f, 49.6f };
  static const float WINDS[] = { 0.0f, 0.1f, 2.34f, 27.8f, 61.3f };
  static const float POPS[] = { 0.0f, 5.0f, 100.0f };
  static const float PRECIP[] = { 0.0f, 0.02f, 2.7f, 254.0f, 999.9f };
  bool all = true;
  int screens = 0;
  for (int units = 0; units < 2; units++) {
    for (int variant = 0; variant < 7; variant++) {
      WxData d;
      wxDataClear(&d);
      d.have = true;
      d.latE2 = 4761;
      d.lonE2 = -12233;
      d.placeKind = ALM_PLACE_WAYPOINT;
      snprintf(d.placeName, sizeof(d.placeName), "%s", variant % 2 ? "Camp below the long ridg" : "map view");
      d.fetchedUtc = FETCH;
      d.trusted = true;
      d.fc.haveOffset = true;
      d.fc.utcOffsetS = variant % 3 ? 5 * 3600 + 1800 : -10 * 3600;
      d.fc.haveNow = true;
      d.fc.now.tempC = TEMPS[variant % 7];
      d.fc.now.feelsC = TEMPS[(variant + 3) % 7];
      d.fc.now.rhPct = POPS[variant % 3];
      d.fc.now.pressHpa = variant % 2 ? 1085.4f : 870.2f;
      d.fc.now.windMs = WINDS[variant % 5];
      d.fc.now.windDirDeg = 22.5f * variant + 11.0f;
      d.fc.now.gustMs = WINDS[(variant + 2) % 5] + 30.0f;
      d.fc.now.code = CODES[(variant * 5) % nCodes];
      d.fc.nHours = WX_HOURS;
      for (int i = 0; i < WX_HOURS; i++) {
        WxHour& h = d.fc.hour[i];
        h.t = 1791064800 + i * 3600;
        h.tempC = TEMPS[(i + variant) % 7];
        h.popPct = POPS[(i + variant) % 3];
        h.windMs = WINDS[(i + variant) % 5];
        h.code = CODES[(i + variant) % nCodes];
      }
      d.fc.nDays = WX_DAYS;
      for (int i = 0; i < WX_DAYS; i++) {
        WxDay& x = d.fc.day[i];
        x.t = 1791010800 + i * 86400;
        x.code = CODES[(i * 3 + variant) % nCodes];
        x.maxC = TEMPS[(i + variant + 1) % 7];
        x.minC = TEMPS[(i + variant) % 7];
        x.popPct = POPS[(i + variant) % 3];
        x.precipMm = PRECIP[(i + variant) % 5];
        x.windMs = WINDS[(i + variant) % 5];
        x.gustMs = WINDS[(i + variant + 1) % 5] + 20.0f;
        x.dirDeg = 360.0f * i / 7;
      }
      d.alertsUtc = FETCH - 6 * 86400;
      d.omLast = WX_HOST_OK;
      d.nwsLast = variant % 4 == 3 ? WX_HOST_FAILED : WX_HOST_OK;
      // The 111 events, eight at a time, in every shape of row.
      for (size_t e0 = 0; e0 < events.size(); e0 += WX_ALERTS) {
        d.al.state = variant == 6 ? WX_AL_TOO_MANY : WX_AL_OK;
        d.al.total = 99999;
        d.al.kept = 99999;
        d.al.n = 0;
        for (size_t e = e0; e < e0 + WX_ALERTS && e < events.size(); e++) {
          WxAlert& a = d.al.a[d.al.n++];
          memset(&a, 0, sizeof(a));
          snprintf(a.event, sizeof(a.event), "%s", events[e].c_str());
          a.severity = (int)(e % 5);
          const int shape = (int)((e + variant) % 4);
          a.onset = shape == 1 ? FETCH + 9 * 86400 : FETCH - 3600;   // 1: not begun, a far date
          a.ends = shape == 2 ? 0 : FETCH + 20 * 86400;               // 2: no end
          a.expires = shape == 3 ? 0 : FETCH + 20 * 86400;
          if (shape == 2) a.expires = 0;
        }
        for (int when = 0; when < 3; when++) {
          const int64_t now = when == 0 ? FETCH + 300 : when == 1 ? FETCH + 5 * 3600 : FETCH + 2 * 86400 + 33000;
          WxView v;
          v.d = &d;
          v.clockTrusted = when != 1;
          v.fetching = when == 2;
          v.note = when == 2 ? "Weather not updated: the answer was not a forecast (a WiFi login page?)" : "";
          for (int tz = 0; tz < 3; tz++) {
            const int tzS = tz == 0 ? -10 * 3600 : tz == 1 ? 13 * 3600 + 2700 : 0;
            AlmCtx c = ctxAt(now, tzS, units, &v);
            Rows r;
            almLinesWeather(&c, collect, &r);
            wxTodayAlertRow(&c, collect, &r);
            wxTodaySummaryRow(&c, collect, &r);
            char where[64];
            snprintf(where, sizeof(where), "units %d variant %d events %zu when %d tz %d", units, variant, e0, when, tz);
            if (!fitsAll(r, where)) all = false;
            screens++;
          }
        }
      }
    }
  }
  // The notes the screen can carry, and every refusal.
  {
    Rows r;
    for (int due = 0; due <= WX_DUE_MUSIC; due++) {
      if (due != WX_DUE_YES) collect(&r, ALM_ROW_WRAP, wxDueWhy(due));
    }
    static const char* const NOTES[] = {
      "Weather not updated: no internet (DNS)", "Weather not updated: certificate not trusted",
      "Weather not updated: Open-Meteo answered HTTP 503",
      "Weather not updated: Open-Meteo says Latitude must be in range of -90 to 90?. Given: 91.0.",
      "Phone low on memory (10 KB block, 13 KB free) - reboot first",
      "A Files folder job is running - let it finish", "The WiFi uploader is on - stop it first",
      "A call is on, or ended under a minute ago",
    };
    for (size_t i = 0; i < sizeof(NOTES) / sizeof(NOTES[0]); i++) collect(&r, ALM_ROW_WRAP, NOTES[i]);
    if (!fitsAll(r, "notes")) all = false;
  }
  char msg[160];
  snprintf(msg, sizeof(msg), "%d screens: %d one-line rows fit 232 px (widest %d px: \"%s\"), %d wrapped sentences whole",
           screens, g_rowsMeasured, g_widest, g_widestText.c_str(), g_wrapsMeasured);
  ok(all, msg);
}

static void testUnitsRow() {
  group("the units row names Fahrenheit (shared word for word with the Maps menu)");
  ok(is(unitsSettingRow(UNITS_US), "Units: US (ft, mi, mph, F)"), "\"Units: US (ft, mi, mph, F)\"");
  const int w = textWidth(&g_font, unitsSettingRow(UNITS_US));
  char m[64];
  snprintf(m, sizeof(m), "...%d px of the row's 232", w);
  ok(w <= ROW_W, m);
}

// ── the roots and the curves ─────────────────────────────────────────────────────────────────

static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

/* The DER of the n-th certificate in a PEM string. */
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
  group("the pinned roots: ISRG Root X1 + the self-signed Root YR (isrg_roots.h)");
  static const char BOTH[] = ISRG_ROOT_X1_PEM ISRG_ROOT_YR_PEM;
  const std::vector<uint8_t> x1 = derOf(BOTH, 0), yr = derOf(BOTH, 1);
  ok(x1.size() > 1000 && yr.size() > 1000 && derOf(BOTH, 2).empty(), "exactly two certificates, concatenated");
  ok(sha256Hex(x1) == "96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6",
     "X1: SHA-256 96:BC:EC...DF:08:C6 (the bytes ota.cpp carried)");
  ok(sha256Hex(yr) == "E5:7B:7E:6F:15:0C:41:91:02:E8:D5:C0:55:72:9F:F9:67:B9:D1:A8:29:BF:00:CE:C8:9C:A6:04:EB:F4:A8:6F",
     "Root YR: SHA-256 E5:7B:7E...A8:6F");
  ok(derOf(ISRG_ROOT_X1_PEM, 0) == x1, "ISRG_ROOT_X1_PEM alone is X1 (what ota.cpp pins)");
}

static void testCurves() {
  group("the ClientHello's curves: P-256 first, exactly the linked table (tls_curves.h)");
  // libmbedtls.a's ecp.o, .rodata.ecp_supported_curves (arduino-esp32 1.0.6, mbedTLS 2.16.7):
  // 521, bp512, 384, bp384, 256, 256k1, bp256, 224, 224k1, 192, 192k1.
  const int table[12] = { 5, 8, 4, 7, 3, 12, 6, 2, 11, 1, 10, 0 };
  int out[13];
  const int n = tlsCurveOrder(table, out, 13);
  const int want[12] = { 3, 4, 5, 8, 7, 12, 6, 2, 11, 1, 10, 0 };
  ok(n == 11 && memcmp(out, want, sizeof(want)) == 0, "secp256r1, secp384r1, secp521r1, then the table's order");
  bool same = true;
  for (int i = 0; i < 11; i++) {
    int hits = 0;
    for (int k = 0; k < 11; k++) hits += out[k] == table[i];
    same = same && hits == 1;
  }
  ok(same, "the same 11 ids, each once: nothing added (never x25519 = 9), nothing dropped");
  const int noP256[4] = { 5, 4, 1, 0 };
  ok(tlsCurveOrder(noP256, out, 13) == 3 && out[0] == 4 && out[1] == 5 && out[2] == 1 && out[3] == 0,
     "a table without P-256: the others in the rule's order, no invented id");
  ok(tlsCurveOrder(table, out, 5) == 4 && out[0] == 5 && out[4] == 0, "a list too long for the room: copied as it is, terminated");
}

int main() {
  printf("test_weather\n");
  fontLoad(&g_font, Akrobat_Bold20);
  ok(g_font.ok, "the firmware's Akrobat_Bold20 loads");
  const std::string om = readFile("tests/fixtures/wx/om_seattle.json");
  const std::string empty = readFile("tests/fixtures/wx/nws_empty_seattle.json");
  const std::string offshore = readFile("tests/fixtures/wx/nws_empty_offshore.json");
  const std::string outside = readFile("tests/fixtures/wx/nws_400_outside.json");
  const std::string two = readFile("tests/fixtures/wx/nws_two_alerts.json");
  const std::string mixed = readFile("tests/fixtures/wx/nws_mixed.json");
  testJsonNumbers();
  testUrls();
  testOpenMeteo(om);
  testTimes();
  testNws(empty, offshore, outside, two, mixed);
  testCodes();
  testFoldAndCache(om, empty, two);
  testPolicy();
  testRows(om);
  testWidths();
  testUnitsRow();
  testRoots();
  testCurves();
  printf("\ntest_weather: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
