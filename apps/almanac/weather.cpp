/*
 * weather.cpp - see weather.h. Pure C++: no Arduino, no IDF, no allocation, no recursion (the
 * JSON is json_read.h's cursor), so it runs on the shared HTTPS worker's 8 KB stack as it does in
 * the host suite. Every buffer is the caller's (PSRAM on the phone).
 */
#include "weather.h"
#include "json_read.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool wxHave(float v) {
  return v == v;                                 // false for NaN only
}

static const float WX_NAN = NAN;
static const double WX_PI = 3.14159265358979323846;

// ── small helpers ─────────────────────────────────────────────────────────────────────────

/* The whole string or "": a cut URL or number is a different one that looks right. */
static size_t wholeOrEmpty(char* out, size_t cap, int len) {
  if (len < 0) {
    if (out && cap) out[0] = '\0';
    return 0;
  }
  if (out && cap && (size_t)len >= cap) {
    out[0] = '\0';
  }
  return (size_t)len;
}

/* Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil). */
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

/* Into `out` (cap), printable ASCII only: a control character is a space, a UTF-8 sequence one
 * '?' (the Akrobat faces stop at '~'; NWS sent no non-ASCII in 415 alerts, but a cache file is
 * text with lines, so nothing may break a line). */
static void asciiFold(char* s) {
  char* w = s;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    const unsigned char c = *p;
    if (c >= 0x20 && c < 0x7F) {
      *w++ = (char)c;
    } else if (c < 0x20 || c == 0x7F) {
      *w++ = ' ';
    } else if (c >= 0xC0) {
      *w++ = '?';                                // a lead byte; its continuation bytes are dropped
    }
  }
  *w = '\0';
}

/* asciiFold, then the spaces at either end trimmed: a cache line's text is read back from its
 * first non-space to its end (rest()), so "  " or " x" would not load as it was saved. Every
 * text the cache stores comes through here - the alerts' and the place's name (review
 * 2026-10-03: a mesh waypoint named "Caf\xC3\xA9" saved a file the phone then set aside as bad). */
static void textFold(char* s) {
  asciiFold(s);
  char* b = s;
  while (*b == ' ') b++;
  size_t n = strlen(b);
  while (n > 0 && b[n - 1] == ' ') n--;
  memmove(s, b, n);
  s[n] = '\0';
}

void wxFmtCoord(double v, int decimals, char* out, size_t cap) {
  if (!out || !cap) {
    return;
  }
  char buf[32];
  if (!(v == v) || v > 1e6 || v < -1e6) {
    snprintf(buf, sizeof(buf), "0");
  } else {
    snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    if (strchr(buf, '.')) {
      size_t n = strlen(buf);
      while (n > 0 && buf[n - 1] == '0') buf[--n] = '\0';
      if (n > 0 && buf[n - 1] == '.') buf[--n] = '\0';
    }
    if (!strcmp(buf, "-0")) {
      strcpy(buf, "0");
    }
  }
  wholeOrEmpty(out, cap, snprintf(out, cap, "%s", buf));
}

int32_t wxE2(double deg) {
  return (int32_t)lround(deg * 100.0);
}

// ── the queries ──────────────────────────────────────────────────────────────────────────

static const char OM_PATH[] =
    "https://" WX_OM_HOST "/v1/forecast?latitude=%s&longitude=%s"
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,pressure_msl,wind_speed_10m,"
    "wind_direction_10m,wind_gusts_10m,weather_code"
    "&hourly=temperature_2m,precipitation_probability,weather_code,wind_speed_10m&forecast_hours=48"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
    "precipitation_sum,wind_speed_10m_max,wind_gusts_10m_max,wind_direction_10m_dominant"
    "&forecast_days=7&timeformat=unixtime&timezone=auto&wind_speed_unit=ms";

size_t wxOpenMeteoUrl(double lat, double lon, bool haveElev, double elevM, char* out, size_t cap) {
  char la[24], lo[24];
  wxFmtCoord(lat, 2, la, sizeof(la));
  wxFmtCoord(lon, 2, lo, sizeof(lo));
  char tail[32] = "";
  if (haveElev && elevM == elevM && elevM > -1000.0 && elevM < 10000.0) {
    snprintf(tail, sizeof(tail), "&elevation=%ld", (long)(lround(elevM / 10.0) * 10));
  }
  char probe[1];
  const int need = snprintf(probe, 0, OM_PATH, la, lo) + (int)strlen(tail);
  if (!out || !cap || (size_t)need >= cap) {
    if (out && cap) out[0] = '\0';
    return (size_t)need;
  }
  snprintf(out, cap, OM_PATH, la, lo);
  strcat(out, tail);
  return (size_t)need;
}

size_t wxNwsUrl(double lat, double lon, char* out, size_t cap) {
  char la[24], lo[24];
  wxFmtCoord(lat, 4, la, sizeof(la));
  wxFmtCoord(lon, 4, lo, sizeof(lo));
  return wholeOrEmpty(out, cap, snprintf(out, cap, "https://" WX_NWS_HOST
                                         "/alerts/active?point=%s,%s&status=actual", la, lo));
}

// ── weather codes ────────────────────────────────────────────────────────────────────────

struct WxCodeName {
  int         code;
  const char* text;
};
/* The short WMO table, shared word for word with COVEY (docs: Open-Meteo's weather_code). */
static const WxCodeName WX_CODES[] = {
  { 0, "Clear" },          { 1, "Mostly clear" },   { 2, "Partly cloudy" },  { 3, "Overcast" },
  { 45, "Fog" },           { 48, "Rime fog" },
  { 51, "Lt drizzle" },    { 53, "Drizzle" },       { 55, "Hvy drizzle" },
  { 56, "Lt frz drizzle" }, { 57, "Frz drizzle" },
  { 61, "Lt rain" },       { 63, "Rain" },          { 65, "Heavy rain" },
  { 66, "Lt frz rain" },   { 67, "Frz rain" },
  { 71, "Lt snow" },       { 73, "Snow" },          { 75, "Heavy snow" },    { 77, "Snow grains" },
  { 80, "Lt showers" },    { 81, "Showers" },       { 82, "Hvy showers" },
  { 85, "Snow shwrs" },    { 86, "Hvy snow shwrs" },
  { 95, "T-storm" },       { 96, "T-storm+hail" },  { 97, "Hvy t-storm" },   { 99, "T-storm+hvy hail" },
};

const char* wxCodeText(int code, char* buf, size_t cap) {
  for (size_t i = 0; i < sizeof(WX_CODES) / sizeof(WX_CODES[0]); i++) {
    if (WX_CODES[i].code == code) {
      return WX_CODES[i].text;
    }
  }
  if (buf && cap) {
    if (code < 0) {
      snprintf(buf, cap, "-");
    } else {
      snprintf(buf, cap, "Code %d", code);
    }
    return buf;
  }
  return "?";
}

bool wxCodeIsSnow(int code) {
  return (code >= 71 && code <= 77) || code == 85 || code == 86;
}

// ── Open-Meteo ────────────────────────────────────────────────────────────────────────────

static float numOr(Js* j, bool* bad) {
  double v = 0;
  const int r = jsNumber(j, &v);
  if (r < 0) {
    *bad = true;
    return WX_NAN;
  }
  return r == 1 ? (float)v : WX_NAN;
}

enum { H_TIME = 0, H_TEMP, H_POP, H_CODE, H_WIND, H_N };
enum { D_TIME = 0, D_CODE, D_MAX, D_MIN, D_POP, D_SUM, D_WIND, D_GUST, D_DIR, D_N };

static int toCode(double v) {
  return (v == v && v >= 0 && v < 1000) ? (int)lround(v) : -1;
}

static bool parseCurrent(Js* j, WxNow* now) {
  if (!jsOpen(j, '{')) return false;
  char key[40];
  bool bad = false;
  for (;;) {
    const int r = jsObjNext(j, key, sizeof(key));
    if (r < 0) return false;
    if (r == 0) break;
    if (!strcmp(key, "temperature_2m")) now->tempC = numOr(j, &bad);
    else if (!strcmp(key, "apparent_temperature")) now->feelsC = numOr(j, &bad);
    else if (!strcmp(key, "relative_humidity_2m")) now->rhPct = numOr(j, &bad);
    else if (!strcmp(key, "pressure_msl")) now->pressHpa = numOr(j, &bad);
    else if (!strcmp(key, "wind_speed_10m")) now->windMs = numOr(j, &bad);
    else if (!strcmp(key, "wind_direction_10m")) now->windDirDeg = numOr(j, &bad);
    else if (!strcmp(key, "wind_gusts_10m")) now->gustMs = numOr(j, &bad);
    else if (!strcmp(key, "weather_code")) {
      const float c = numOr(j, &bad);
      now->code = toCode(c);
    } else if (!jsSkip(j)) return false;     // time, interval and anything new
    if (bad) return false;
  }
  return true;
}

/* One value of an hourly / daily column, by index, straight into the forecast (no scratch: the
 * worker's stack is 8 KB and internal RAM is what this phone runs out of). `v` is NaN for null. */
static void putHour(WxForecast* out, int col, int i, double v) {
  WxHour& h = out->hour[i];
  const float f = v == v ? (float)v : WX_NAN;
  switch (col) {
  case H_TIME: h.t = (v == v && v > 0 && v < 9e15) ? (int64_t)llround(v) : 0; break;
  case H_TEMP: h.tempC = f; break;
  case H_POP:  h.popPct = f; break;
  case H_CODE: h.code = toCode(v); break;
  default:     h.windMs = f; break;
  }
}

static void putDay(WxForecast* out, int col, int i, double v) {
  WxDay& d = out->day[i];
  const float f = v == v ? (float)v : WX_NAN;
  switch (col) {
  case D_TIME: d.t = (v == v && v > 0 && v < 9e15) ? (int64_t)llround(v) : 0; break;
  case D_CODE: d.code = toCode(v); break;
  case D_MAX:  d.maxC = f; break;
  case D_MIN:  d.minC = f; break;
  case D_POP:  d.popPct = f; break;
  case D_SUM:  d.precipMm = f; break;
  case D_WIND: d.windMs = f; break;
  case D_GUST: d.gustMs = f; break;
  default:     d.dirDeg = f; break;
  }
}

static const char* const H_COLS[H_N] = { "time", "temperature_2m", "precipitation_probability",
                                         "weather_code", "wind_speed_10m" };
static const char* const D_COLS[D_N] = { "time", "weather_code", "temperature_2m_max",
                                         "temperature_2m_min", "precipitation_probability_max",
                                         "precipitation_sum", "wind_speed_10m_max",
                                         "wind_gusts_10m_max", "wind_direction_10m_dominant" };

/* hourly / daily: parallel arrays, in any order, matched by index (null in any of them is NaN /
 * no code). Returns the length of the "time" array (entries past `max` are read and dropped), -1
 * on a bad body. */
static int parseColumns(Js* j, WxForecast* out, bool daily) {
  const char* const* cols = daily ? D_COLS : H_COLS;
  const int nCols = daily ? D_N : H_N;
  const int max = daily ? WX_DAYS : WX_HOURS;
  if (!jsOpen(j, '{')) return -1;
  char key[48];
  int nTime = -1;
  for (;;) {
    const int r = jsObjNext(j, key, sizeof(key));
    if (r < 0) return -1;
    if (r == 0) break;
    int col = -1;
    for (int c = 0; c < nCols; c++) {
      if (!strcmp(key, cols[c])) {
        col = c;
        break;
      }
    }
    if (col < 0) {
      if (!jsSkip(j)) return -1;
      continue;
    }
    if (!jsOpen(j, '[')) return -1;
    int n = 0;
    for (;;) {
      const int k = jsArrNext(j);
      if (k < 0) return -1;
      if (k == 0) break;
      double x = 0;
      const int got = jsNumber(j, &x);
      if (got < 0) return -1;
      if (n < max) {
        if (daily) putDay(out, col, n, got == 1 ? x : NAN);
        else putHour(out, col, n, got == 1 ? x : NAN);
      }
      n++;
    }
    if (col == 0) nTime = n;
  }
  return nTime;
}

/* Drop the entries with no time (a null in the time column), keeping the order. */
static void compact(WxForecast* out, int nH, int nD) {
  int k = 0;
  for (int i = 0; i < nH && i < WX_HOURS; i++) {
    if (out->hour[i].t > 0) out->hour[k++] = out->hour[i];
  }
  out->nHours = k;
  k = 0;
  for (int i = 0; i < nD && i < WX_DAYS; i++) {
    if (out->day[i].t > 0) out->day[k++] = out->day[i];
  }
  out->nDays = k;
}

int wxParseOpenMeteo(const char* body, size_t len, WxForecast* out, char* reason, size_t rcap) {
  if (reason && rcap) reason[0] = '\0';
  memset(out, 0, sizeof(*out));
  out->now.tempC = out->now.feelsC = out->now.rhPct = out->now.pressHpa = WX_NAN;
  out->now.windMs = out->now.windDirDeg = out->now.gustMs = WX_NAN;
  out->now.code = -1;
  if (!body) return WX_OM_BAD;
  Js j = { body, body + len };
  if (!jsOpen(&j, '{')) return WX_OM_BAD;        // a captive portal's HTML page lands here
  char key[48];
  bool error = false, sawHourly = false, sawDaily = false;
  int nH = 0, nD = 0;
  for (int i = 0; i < WX_HOURS; i++) {
    WxHour& h = out->hour[i];
    h.tempC = h.popPct = h.windMs = WX_NAN;
    h.code = -1;
  }
  for (int i = 0; i < WX_DAYS; i++) {
    WxDay& d = out->day[i];
    d.maxC = d.minC = d.popPct = d.precipMm = d.windMs = d.gustMs = d.dirDeg = WX_NAN;
    d.code = -1;
  }
  for (;;) {
    const int r = jsObjNext(&j, key, sizeof(key));
    if (r < 0) return WX_OM_BAD;                 // cut short, or not JSON
    if (r == 0) break;
    if (!strcmp(key, "error")) {
      error = jsTrue(&j);
    } else if (!strcmp(key, "reason")) {
      char tmp[WX_REASON_MAX];
      tmp[0] = '\0';
      if (!jsStrInto(&j, tmp, sizeof(tmp))) return WX_OM_BAD;
      asciiFold(tmp);
      if (reason && rcap) snprintf(reason, rcap, "%s", tmp);
    } else if (!strcmp(key, "utc_offset_seconds")) {
      double v = 0;
      const int k = jsNumber(&j, &v);
      if (k < 0) return WX_OM_BAD;
      if (k == 1 && v > -86400 && v < 86400) {
        out->haveOffset = true;
        out->utcOffsetS = (int32_t)lround(v);
      }
    } else if (!strcmp(key, "current")) {
      if (!parseCurrent(&j, &out->now)) return WX_OM_BAD;
      out->haveNow = true;
    } else if (!strcmp(key, "hourly")) {
      nH = parseColumns(&j, out, false);
      if (nH < 0) return WX_OM_BAD;
      sawHourly = true;
    } else if (!strcmp(key, "daily")) {
      nD = parseColumns(&j, out, true);
      if (nD < 0) return WX_OM_BAD;
      sawDaily = true;
    } else if (!jsSkip(&j)) {
      return WX_OM_BAD;
    }
  }
  if (error) {
    return WX_OM_ERROR;
  }
  if (!sawHourly && !sawDaily) {
    return WX_OM_BAD;                            // JSON, but not a forecast
  }
  compact(out, nH, nD);
  if (out->nHours == 0 && out->nDays == 0) {
    return WX_OM_BAD;
  }
  return WX_OM_OK;
}

// ── NWS alerts ────────────────────────────────────────────────────────────────────────────

void wxCountInit(WxEventCounter* c) {
  memset(c, 0, sizeof(*c));
}

void wxCountFeed(WxEventCounter* c, const char* p, size_t n) {
  static const char PAT[] = "\"event\"";
  for (size_t i = 0; i < n; i++) {
    const char ch = p[i];
    if (c->afterKey) {
      if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
        continue;
      }
      c->afterKey = false;
      if (ch == ':') {
        c->count++;
        continue;
      }
    }
    if (ch == PAT[c->matched]) {
      if (++c->matched == 7) {
        c->afterKey = true;
        c->matched = 0;
      }
    } else {
      c->matched = (ch == '"') ? 1 : 0;          // the pattern's only restart point is a '"'
    }
  }
}

static int dig(const char* s, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return -1;
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

bool wxParseIso(const char* s, int64_t* utc) {
  if (!s || strlen(s) < 20) return false;
  const int y = dig(s, 4), mo = dig(s + 5, 2), d = dig(s + 8, 2);
  const int hh = dig(s + 11, 2), mi = dig(s + 14, 2), ss = dig(s + 17, 2);
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || hh < 0 || hh > 23 || mi < 0 || mi > 59 ||
      ss < 0 || ss > 60 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') ||
      s[13] != ':' || s[16] != ':') {
    return false;
  }
  const char* p = s + 19;
  if (*p == '.') {
    p++;
    while (*p >= '0' && *p <= '9') p++;
  }
  int off = 0;
  if (*p == 'Z' && p[1] == '\0') {
    off = 0;
  } else if ((*p == '+' || *p == '-') && strlen(p) == 6 && p[3] == ':') {
    const int oh = dig(p + 1, 2), om = dig(p + 4, 2);
    if (oh < 0 || oh > 18 || om < 0 || om > 59) return false;
    off = (oh * 3600 + om * 60) * (*p == '-' ? -1 : 1);
  } else {
    return false;                                // no offset: a local time is never guessed
  }
  *utc = daysFromCivil(y, mo, d) * 86400 + hh * 3600 + mi * 60 + ss - off;
  return true;
}

bool wxParseHttpDate(const char* s, int64_t* utc) {
  static const char* const MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  // "Sat, 03 Oct 2026 22:40:22 GMT"
  if (!s || strlen(s) < 29 || s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' ||
      s[16] != ' ' || s[19] != ':' || s[22] != ':' || strncmp(s + 25, " GMT", 4) != 0) {
    return false;
  }
  const int d = dig(s + 5, 2), y = dig(s + 12, 4), hh = dig(s + 17, 2), mi = dig(s + 20, 2),
            ss = dig(s + 23, 2);
  int mo = -1;
  for (int i = 0; i < 12; i++) {
    if (!strncmp(s + 8, MON[i], 3)) mo = i + 1;
  }
  if (mo < 0 || d < 1 || d > 31 || y < 1970 || hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 ||
      ss > 60) {
    return false;
  }
  *utc = daysFromCivil(y, mo, d) * 86400 + hh * 3600 + mi * 60 + ss;
  return true;
}

static int sevOf(const char* s) {
  if (!strcmp(s, "Extreme")) return WX_SEV_EXTREME;
  if (!strcmp(s, "Severe")) return WX_SEV_SEVERE;
  if (!strcmp(s, "Moderate")) return WX_SEV_MODERATE;
  if (!strcmp(s, "Minor")) return WX_SEV_MINOR;
  return WX_SEV_UNKNOWN;
}

static int urgOf(const char* s) {
  if (!strcmp(s, "Immediate")) return WX_URG_IMMEDIATE;
  if (!strcmp(s, "Expected")) return WX_URG_EXPECTED;
  if (!strcmp(s, "Future")) return WX_URG_FUTURE;
  if (!strcmp(s, "Past")) return WX_URG_PAST;
  return WX_URG_UNKNOWN;
}

static int msgOf(const char* s) {
  if (!strcmp(s, "Alert")) return WX_MSG_ALERT;
  if (!strcmp(s, "Update")) return WX_MSG_UPDATE;
  if (!strcmp(s, "Cancel")) return WX_MSG_CANCEL;
  return WX_MSG_OTHER;
}

static int64_t alertEnd(const WxAlert* a) {
  return a->ends ? a->ends : a->expires;
}

/* a before b in the list: more severe, then more urgent, then ending sooner. */
static bool alertBefore(const WxAlert* a, const WxAlert* b) {
  if (a->severity != b->severity) return a->severity > b->severity;
  if (a->urgency != b->urgency) return a->urgency > b->urgency;
  const int64_t ea = alertEnd(a), eb = alertEnd(b);
  if (ea != eb) return ea && (!eb || ea < eb);
  return false;
}

/* One feature's "properties". false on a body cut short or not JSON. *keep = it is shown
 * (status Actual or absent, not Past, not a cancellation). */
static bool parseProps(Js* j, WxAlert* a, bool* keep) {
  memset(a, 0, sizeof(*a));
  *keep = true;
  if (!jsOpen(j, '{')) return false;
  char key[24], val[40];
  for (;;) {
    const int r = jsObjNext(j, key, sizeof(key));
    if (r < 0) return false;
    if (r == 0) break;
    if (!strcmp(key, "event")) {
      if (!jsStrInto(j, a->event, sizeof(a->event))) return false;
      textFold(a->event);
    } else if (!strcmp(key, "headline")) {
      if (!jsStrInto(j, a->headline, sizeof(a->headline))) return false;   // null: stays ""
      textFold(a->headline);
    } else if (!strcmp(key, "severity") || !strcmp(key, "urgency") || !strcmp(key, "messageType") ||
               !strcmp(key, "status") || !strcmp(key, "onset") || !strcmp(key, "ends") ||
               !strcmp(key, "expires")) {
      val[0] = '\0';
      if (!jsStrInto(j, val, sizeof(val))) return false;
      if (!strcmp(key, "severity")) a->severity = sevOf(val);
      else if (!strcmp(key, "urgency")) a->urgency = urgOf(val);
      else if (!strcmp(key, "messageType")) a->msgType = msgOf(val);
      else if (!strcmp(key, "status")) {
        if (val[0] && strcmp(val, "Actual") != 0) *keep = false;     // "Test", "Exercise"...
      } else {
        int64_t t = 0;
        if (val[0] && wxParseIso(val, &t)) {
          if (!strcmp(key, "onset")) a->onset = t;
          else if (!strcmp(key, "ends")) a->ends = t;
          else a->expires = t;
        }
      }
    } else if (!jsSkip(j)) {
      return false;
    }
  }
  if (!a->event[0]) {
    snprintf(a->event, sizeof(a->event), "Alert");
  }
  if (a->urgency == WX_URG_PAST || a->msgType == WX_MSG_CANCEL) {
    *keep = false;
  }
  return true;
}

/* Insert into the sorted top-WX_ALERTS list. */
static void keepAlert(WxAlerts* out, const WxAlert* a) {
  int at = out->n;
  while (at > 0 && alertBefore(a, &out->a[at - 1])) at--;
  if (at >= WX_ALERTS) return;
  const int last = out->n < WX_ALERTS ? out->n : WX_ALERTS - 1;
  for (int i = last; i > at; i--) out->a[i] = out->a[i - 1];
  out->a[at] = *a;
  if (out->n < WX_ALERTS) out->n++;
}

/* The features, as many whole ones as the body holds. Returns false when the body is not the
 * collection (or is cut short) - `*features` and the list hold what came before the cut. */
static bool parseCollection(Js* j, WxAlerts* out, int* features, bool* sawFeatures) {
  *features = 0;
  *sawFeatures = false;
  if (!jsOpen(j, '{')) return false;
  char key[24];
  for (;;) {
    int r = jsObjNext(j, key, sizeof(key));
    if (r < 0) return false;
    if (r == 0) return true;
    if (!strcmp(key, "updated")) {
      char v[40] = "";
      if (!jsStrInto(j, v, sizeof(v))) return false;
      int64_t t = 0;
      if (wxParseIso(v, &t)) out->updated = t;
    } else if (!strcmp(key, "features")) {
      if (!jsOpen(j, '[')) return false;
      *sawFeatures = true;
      for (;;) {
        r = jsArrNext(j);
        if (r < 0) return false;
        if (r == 0) break;
        if (!jsOpen(j, '{')) return false;
        char fk[24];
        WxAlert a;
        bool keep = false, haveProps = false;
        for (;;) {
          const int q = jsObjNext(j, fk, sizeof(fk));
          if (q < 0) return false;
          if (q == 0) break;
          if (!strcmp(fk, "properties")) {
            if (!parseProps(j, &a, &keep)) return false;
            haveProps = true;
          } else if (!jsSkip(j)) {
            return false;
          }
        }
        (*features)++;
        if (haveProps && keep) {
          out->kept++;
          keepAlert(out, &a);
        }
      }
    } else if (!jsSkip(j)) {
      return false;
    }
  }
}

int wxParseNwsAlerts(int httpCode, const char* body, size_t len, bool overflow, int counted,
                     WxAlerts* out) {
  memset(out, 0, sizeof(*out));
  out->state = WX_AL_FAILED;
  if (!body) {
    return out->state;
  }
  Js j = { body, body + len };
  if (httpCode == 400) {
    /* {"title":"Invalid Parameter","status":400,"detail":"Parameter \"point\" is invalid: out of
     * bounds",...}. Only THAT is "outside the US"; any other 400 is a fault, retried later. */
    if (!jsOpen(&j, '{')) return out->state;
    char key[16], detail[160];
    for (;;) {
      const int r = jsObjNext(&j, key, sizeof(key));
      if (r <= 0) break;
      if (!strcmp(key, "detail")) {
        detail[0] = '\0';
        if (!jsStrInto(&j, detail, sizeof(detail))) break;
        if (strstr(detail, "out of bounds")) {
          out->state = WX_AL_OUTSIDE;
        }
        break;
      }
      if (!jsSkip(&j)) break;
    }
    return out->state;
  }
  if (httpCode != 200) {
    return out->state;
  }
  int features = 0;
  bool sawFeatures = false;
  const bool whole = parseCollection(&j, out, &features, &sawFeatures);
  if (overflow) {
    /* The body outgrew the buffer: alerts EXIST (an empty answer is ~230 B). The count is the
     * streaming counter's, which saw all of it; the list is whatever whole features fit. */
    out->total = counted > features ? counted : features;
    if (out->total < 1) out->total = 1;
    out->state = WX_AL_TOO_MANY;
    return out->state;
  }
  if (!whole || !sawFeatures) {
    out->n = 0;
    out->state = WX_AL_FAILED;                   // an HTML 200, a cut body: not an answer
    return out->state;
  }
  out->total = features;
  out->state = out->n > 0 ? WX_AL_OK : WX_AL_NONE;
  return out->state;
}

// ── the cache file ────────────────────────────────────────────────────────────────────────

void wxDataClear(WxData* d) {
  memset(d, 0, sizeof(*d));
  d->elevM = WX_NO_ELEV;
}

struct WxW {
  char*  out;
  size_t cap;
  size_t n;
};

static void wPut(WxW* w, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void wPut(WxW* w, const char* fmt, ...) {
  char line[200];
  va_list ap;
  va_start(ap, fmt);
  int k = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (k < 0) return;
  if ((size_t)k >= sizeof(line)) k = (int)sizeof(line) - 1;
  if (w->out && w->n + (size_t)k < w->cap) {
    memcpy(w->out + w->n, line, (size_t)k);
  }
  w->n += (size_t)k;
}

/* A float as the file writes it: "%.6g", or "-" for NaN. */
static const char* fnum(float v, char* buf, size_t cap) {
  if (!(v == v)) {
    snprintf(buf, cap, "-");
  } else {
    snprintf(buf, cap, "%.6g", (double)v);
  }
  return buf;
}

#define WX_FILE_HEAD "wiphone-wx 1\n"

size_t wxCacheSave(const WxData* d, char* out, size_t cap) {
  WxW w = { out, cap, 0 };
  char a[16], b[16], c[16], e[16], f[16], g[16], h[16], k[16];
  wPut(&w, "%s", WX_FILE_HEAD);
  wPut(&w, "point %ld %ld %d %s\n", (long)d->latE2, (long)d->lonE2, d->placeKind,
       d->placeName[0] ? d->placeName : "-");
  wPut(&w, "forecast %d %lld %d %ld %s\n", d->have ? 1 : 0, (long long)d->fetchedUtc,
       d->trusted ? 1 : 0, (long)d->elevM, d->fc.haveOffset ? "" : "-");
  if (d->fc.haveOffset) {
    // the offset rides on its own line (the "-" above says "none")
    wPut(&w, "offset %ld\n", (long)d->fc.utcOffsetS);
  }
  wPut(&w, "hosts %d %d\n", d->omLast, d->nwsLast);
  if (d->fc.haveNow) {
    const WxNow& n = d->fc.now;
    wPut(&w, "now %s %s %s %s %s %s %s %d\n", fnum(n.tempC, a, 16), fnum(n.feelsC, b, 16),
         fnum(n.rhPct, c, 16), fnum(n.pressHpa, e, 16), fnum(n.windMs, f, 16),
         fnum(n.windDirDeg, g, 16), fnum(n.gustMs, h, 16), n.code);
  }
  for (int i = 0; i < d->fc.nHours && i < WX_HOURS; i++) {
    const WxHour& x = d->fc.hour[i];
    wPut(&w, "hour %lld %s %s %d %s\n", (long long)x.t, fnum(x.tempC, a, 16), fnum(x.popPct, b, 16),
         x.code, fnum(x.windMs, c, 16));
  }
  for (int i = 0; i < d->fc.nDays && i < WX_DAYS; i++) {
    const WxDay& x = d->fc.day[i];
    wPut(&w, "day %lld %d %s %s %s %s %s %s %s\n", (long long)x.t, x.code, fnum(x.maxC, a, 16),
         fnum(x.minC, b, 16), fnum(x.popPct, c, 16), fnum(x.precipMm, e, 16), fnum(x.windMs, f, 16),
         fnum(x.gustMs, g, 16), fnum(x.dirDeg, k, 16));
  }
  wPut(&w, "alerts %d %d %d %lld %lld %d", d->al.state, d->al.kept, d->al.total, (long long)d->al.updated,
       (long long)d->alertsUtc, d->alertsTrusted ? 1 : 0);
  if (d->alPointKnown) {
    wPut(&w, " %ld %ld", (long)d->alLatE2, (long)d->alLonE2);   // the alerts' own point
  }
  wPut(&w, "\n");
  for (int i = 0; i < d->al.n && i < WX_ALERTS; i++) {
    const WxAlert& x = d->al.a[i];
    wPut(&w, "alert %d %d %d %lld %lld %lld %s\n", x.severity, x.urgency, x.msgType,
         (long long)x.onset, (long long)x.ends, (long long)x.expires, x.event[0] ? x.event : "Alert");
    if (x.headline[0]) {
      wPut(&w, "headline %s\n", x.headline);
    }
  }
  wPut(&w, "end\n");
  if (w.out && w.cap) {
    if (w.n < w.cap) w.out[w.n] = '\0';
    else w.out[0] = '\0';
  }
  return w.n;
}

// A cursor over one line's space-separated tokens.
struct WxTok {
  const char* p;
  const char* e;
};

static bool tok(WxTok* t, char* buf, size_t cap) {
  while (t->p < t->e && *t->p == ' ') t->p++;
  if (t->p >= t->e) return false;
  size_t n = 0;
  while (t->p < t->e && *t->p != ' ') {
    if (n + 1 >= cap) return false;
    buf[n++] = *t->p++;
  }
  buf[n] = '\0';
  return n > 0;
}

static bool tokI64(WxTok* t, int64_t* v) {
  char b[24];
  if (!tok(t, b, sizeof(b))) return false;
  char* end = NULL;
  const long long x = strtoll(b, &end, 10);
  if (!end || *end) return false;
  *v = x;
  return true;
}

static bool tokInt(WxTok* t, int* v, int lo, int hi) {
  int64_t x = 0;
  if (!tokI64(t, &x) || x < lo || x > hi) return false;
  *v = (int)x;
  return true;
}

static bool tokF(WxTok* t, float* v) {
  char b[24];
  if (!tok(t, b, sizeof(b))) return false;
  if (!strcmp(b, "-")) {
    *v = WX_NAN;
    return true;
  }
  char* end = NULL;
  const double x = strtod(b, &end);
  if (!end || *end || !(x == x) || x > 1e9 || x < -1e9) return false;
  *v = (float)x;
  return true;
}

/* The rest of the line after one space: a name or an event (printable ASCII). */
static bool rest(WxTok* t, char* buf, size_t cap) {
  while (t->p < t->e && *t->p == ' ') t->p++;
  const size_t n = (size_t)(t->e - t->p);
  if (n == 0 || n >= cap) return false;
  memcpy(buf, t->p, n);
  buf[n] = '\0';
  for (size_t i = 0; i < n; i++) {
    if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] >= 0x7F) return false;
  }
  return true;
}

static bool loadInner(const char* text, size_t len, WxData* d) {
  const size_t hl = strlen(WX_FILE_HEAD);
  if (!text || len < hl || memcmp(text, WX_FILE_HEAD, hl) != 0) return false;
  const char* p = text + hl;
  const char* end = text + len;
  bool sawPoint = false, sawForecast = false, sawHosts = false, sawAlerts = false, sawEnd = false;
  bool wantOffset = false, sawAlPoint = false;
  int alertsWant = -1;
  while (p < end) {
    const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
    if (!nl) return false;                       // a last line with no newline: cut short
    WxTok t = { p, nl };
    if (nl > p && nl[-1] == '\r') t.e = nl - 1;
    p = nl + 1;
    if (sawEnd) {
      if (t.e > t.p) return false;               // nothing after "end"
      continue;
    }
    char w[16];
    if (!tok(&t, w, sizeof(w))) return false;
    if (!strcmp(w, "end")) {
      sawEnd = true;
      continue;
    }
    if (!strcmp(w, "point")) {
      int64_t la = 0, lo = 0;
      if (!tokI64(&t, &la) || !tokI64(&t, &lo) || la < -9000 || la > 9000 || lo < -18000 || lo > 18000 ||
          !tokInt(&t, &d->placeKind, 0, 99) || !rest(&t, d->placeName, sizeof(d->placeName))) {
        return false;
      }
      if (!strcmp(d->placeName, "-")) d->placeName[0] = '\0';
      d->latE2 = (int32_t)la;
      d->lonE2 = (int32_t)lo;
      sawPoint = true;
    } else if (!strcmp(w, "forecast")) {
      int have = 0, tr = 0;
      int64_t el = 0;
      if (!tokInt(&t, &have, 0, 1) || !tokI64(&t, &d->fetchedUtc) || !tokInt(&t, &tr, 0, 1) ||
          !tokI64(&t, &el) || el < WX_NO_ELEV || el > 10000) {
        return false;
      }
      char x[4];
      wantOffset = !tok(&t, x, sizeof(x));       // "-" = no offset line follows
      if (!wantOffset && strcmp(x, "-") != 0) return false;
      d->have = have != 0;
      d->trusted = tr != 0;
      d->elevM = (int32_t)el;
      sawForecast = true;
    } else if (!strcmp(w, "offset")) {
      int64_t o = 0;
      if (!wantOffset || !tokI64(&t, &o) || o <= -86400 || o >= 86400) return false;
      d->fc.haveOffset = true;
      d->fc.utcOffsetS = (int32_t)o;
      wantOffset = false;
    } else if (!strcmp(w, "hosts")) {
      if (!tokInt(&t, &d->omLast, 0, 2) || !tokInt(&t, &d->nwsLast, 0, 2)) return false;
      sawHosts = true;
    } else if (!strcmp(w, "now")) {
      WxNow& n = d->fc.now;
      if (!tokF(&t, &n.tempC) || !tokF(&t, &n.feelsC) || !tokF(&t, &n.rhPct) ||
          !tokF(&t, &n.pressHpa) || !tokF(&t, &n.windMs) || !tokF(&t, &n.windDirDeg) ||
          !tokF(&t, &n.gustMs) || !tokInt(&t, &n.code, -1, 999)) {
        return false;
      }
      d->fc.haveNow = true;
    } else if (!strcmp(w, "hour")) {
      if (d->fc.nHours >= WX_HOURS) return false;
      WxHour& h = d->fc.hour[d->fc.nHours];
      if (!tokI64(&t, &h.t) || h.t <= 0 || !tokF(&t, &h.tempC) || !tokF(&t, &h.popPct) ||
          !tokInt(&t, &h.code, -1, 999) || !tokF(&t, &h.windMs)) {
        return false;
      }
      d->fc.nHours++;
    } else if (!strcmp(w, "day")) {
      if (d->fc.nDays >= WX_DAYS) return false;
      WxDay& x = d->fc.day[d->fc.nDays];
      if (!tokI64(&t, &x.t) || x.t <= 0 || !tokInt(&t, &x.code, -1, 999) || !tokF(&t, &x.maxC) ||
          !tokF(&t, &x.minC) || !tokF(&t, &x.popPct) || !tokF(&t, &x.precipMm) ||
          !tokF(&t, &x.windMs) || !tokF(&t, &x.gustMs) || !tokF(&t, &x.dirDeg)) {
        return false;
      }
      d->fc.nDays++;
    } else if (!strcmp(w, "alerts")) {
      int tr = 0;
      if (!tokInt(&t, &d->al.state, 0, WX_AL_FAILED) || !tokInt(&t, &d->al.kept, 0, 1000000) ||
          !tokInt(&t, &d->al.total, 0, 1000000) ||
          !tokI64(&t, &d->al.updated) || !tokI64(&t, &d->alertsUtc) || !tokInt(&t, &tr, 0, 1)) {
        return false;
      }
      d->alertsTrusted = tr != 0;
      /* The alerts' own point (0.9.81 final): absent in a file from before it - the forecast's
       * point then, below. One without the other is a bad line. */
      while (t.p < t.e && *t.p == ' ') t.p++;
      if (t.p < t.e) {
        int64_t la = 0, lo = 0;
        if (!tokI64(&t, &la) || !tokI64(&t, &lo) || la < -9000 || la > 9000 || lo < -18000 || lo > 18000) {
          return false;
        }
        d->alLatE2 = (int32_t)la;
        d->alLonE2 = (int32_t)lo;
        sawAlPoint = true;
      }
      sawAlerts = true;
      alertsWant = 0;
    } else if (!strcmp(w, "alert")) {
      if (!sawAlerts || d->al.n >= WX_ALERTS) return false;
      WxAlert& x = d->al.a[d->al.n];
      memset(&x, 0, sizeof(x));
      if (!tokInt(&t, &x.severity, 0, WX_SEV_EXTREME) || !tokInt(&t, &x.urgency, 0, WX_URG_IMMEDIATE) ||
          !tokInt(&t, &x.msgType, 0, WX_MSG_OTHER) || !tokI64(&t, &x.onset) || !tokI64(&t, &x.ends) ||
          !tokI64(&t, &x.expires) || !rest(&t, x.event, sizeof(x.event))) {
        return false;
      }
      d->al.n++;
      alertsWant = 1;
    } else if (!strcmp(w, "headline")) {
      if (alertsWant != 1) return false;         // a headline belongs to the alert just read
      if (!rest(&t, d->al.a[d->al.n - 1].headline, sizeof(d->al.a[0].headline))) return false;
      alertsWant = 0;
    } else {
      return false;                              // a line this firmware does not know
    }
  }
  if (!sawEnd || !sawPoint || !sawForecast || !sawHosts || !sawAlerts || wantOffset) {
    return false;
  }
  if ((d->al.state == WX_AL_OK) != (d->al.n > 0) && d->al.state != WX_AL_TOO_MANY) {
    return false;                                // a list that contradicts its own state
  }
  if (sawAlPoint) {
    d->alPointKnown = true;
  } else {
    // A file from before the alerts' own point: they were the forecast's - when it had one.
    d->alLatE2 = d->latE2;
    d->alLonE2 = d->lonE2;
    d->alPointKnown = d->have;
  }
  return true;
}

bool wxCacheLoad(const char* text, size_t len, WxData* out) {
  wxDataClear(out);
  if (!loadInner(text, len, out)) {
    wxDataClear(out);
    return false;
  }
  return true;
}

// ── folding a fetch in ────────────────────────────────────────────────────────────────────

double wxDistanceKm(double lat1, double lon1, double lat2, double lon2) {
  const double k = 111.195;                      // km per degree on the mean sphere
  const double dLat = (lat2 - lat1) * k;
  double dLon = lon2 - lon1;
  if (dLon > 180) dLon -= 360;
  if (dLon < -180) dLon += 360;
  dLon *= k * cos((lat1 + lat2) * 0.5 * WX_PI / 180.0);
  return sqrt(dLat * dLat + dLon * dLon);
}

bool wxFold(WxData* d, const WxFetch* f) {
  int64_t stamp = 0;
  bool trusted = false;
  if (f->askedUtc > 0) {
    stamp = f->askedUtc;
    trusted = true;
  } else if (f->dateUtc > 0) {
    stamp = f->dateUtc;
  } else if (f->nwsTried && f->al.updated > 0) {
    stamp = f->al.updated;
  }
  bool changed = false;
  /* Measured from the ALERTS' point: the forecast's may be another place's (a failed forecast).
   * Held alerts of no known point (a file from before it, with no forecast) count as moved. */
  const bool moved = d->al.state != WX_AL_UNKNOWN &&
                     (!d->alPointKnown || wxDistanceKm(d->alLatE2 / 100.0, d->alLonE2 / 100.0,
                                                       f->latE2 / 100.0, f->lonE2 / 100.0) > WX_MOVED_KM);
  d->omLast = f->omParsed ? WX_HOST_OK : WX_HOST_FAILED;
  if (f->omParsed) {
    d->have = true;
    d->latE2 = f->latE2;
    d->lonE2 = f->lonE2;
    d->placeKind = f->placeKind;
    snprintf(d->placeName, sizeof(d->placeName), "%s", f->placeName);
    textFold(d->placeName);                      // a waypoint's name is anyone's UTF-8
    if (!strcmp(d->placeName, "-")) d->placeName[0] = '\0';   // "-" is the file's "none"
    d->elevM = f->elevM;
    d->fetchedUtc = stamp;
    d->trusted = trusted;
    d->fc = f->fc;
    changed = true;
  }
  if (f->nwsTried) {
    if (f->al.state != WX_AL_FAILED && f->al.state != WX_AL_UNKNOWN) {
      d->al = f->al;
      d->alertsUtc = stamp;
      d->alertsTrusted = trusted;
      d->alLatE2 = f->latE2;                     // THIS place's alerts, whatever the forecast did
      d->alLonE2 = f->lonE2;
      d->alPointKnown = true;
      d->nwsLast = WX_HOST_OK;
    } else {
      d->nwsLast = WX_HOST_FAILED;
      if (moved && f->omParsed) {
        /* The forecast is for a new place and its alerts did not answer: the old ones were for
         * somewhere over WX_MOVED_KM away - not this place's, so not kept. */
        memset(&d->al, 0, sizeof(d->al));
        d->alertsUtc = 0;
        d->alertsTrusted = false;
        d->alLatE2 = 0;
        d->alLonE2 = 0;
        d->alPointKnown = false;
      }
    }
    changed = true;
  }
  return changed;
}

bool wxAlertsApart(const WxData* d) {
  if (d->al.state == WX_AL_UNKNOWN || !d->alPointKnown) {
    return false;                                // no alerts answer held, or from an older file
  }
  if (!d->have) {
    return true;
  }
  return wxDistanceKm(d->latE2 / 100.0, d->lonE2 / 100.0, d->alLatE2 / 100.0, d->alLonE2 / 100.0) >
         WX_MOVED_KM;
}

// ── when to fetch ─────────────────────────────────────────────────────────────────────────

int wxFetchDue(const WxDueIn* in) {
  if (in->hotspot) return WX_DUE_HOTSPOT;
  if (!in->wifiUp) return WX_DUE_NO_WIFI;
  if (!in->placeOk) return WX_DUE_NO_PLACE;
  if (in->busy) return WX_DUE_BUSY;
  if (in->explicitAsk) return WX_DUE_YES;
  if (in->attempted && in->lastFailed && in->sinceAttemptMs < WX_RETRY_MS) return WX_DUE_RETRY_WAIT;
  if (in->haveCache && in->cacheAgeS >= 0 && in->cacheAgeS < WX_STALE_S && in->movedKm >= 0 &&
      in->movedKm <= WX_MOVED_KM) {
    return WX_DUE_FRESH;
  }
  if (in->musicPlaying) return WX_DUE_MUSIC;
  return WX_DUE_YES;
}

const char* wxDueWhy(int due) {
  switch (due) {
  case WX_DUE_YES:        return "";
  case WX_DUE_NO_WIFI:    return "No WiFi - join a network first";
  case WX_DUE_HOTSPOT:    return "The phone's own hotspot is on - no internet there";
  case WX_DUE_NO_PLACE:   return "No place known yet - see Position & GPS";
  case WX_DUE_BUSY:       return "Already fetching the weather";
  case WX_DUE_FRESH:      return "The forecast is under an hour old";
  case WX_DUE_RETRY_WAIT: return "The last try failed - trying again in a few minutes";
  case WX_DUE_MUSIC:      return "Music is playing - Refresh pauses it and fetches";
  default:                return "Not now";
  }
}
