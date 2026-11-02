/* weather_lines.cpp - see weather_lines.h. Every wording of the Almanac's weather lives here. */

#include "weather_lines.h"
#include "units.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

const char* const WDAY3[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char* const MON3[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

#define WX_HOUR_STEP   3        // the hours list: every third hour...
#define WX_HOUR_ROWS   8        // ...for a day
#define WX_POP_SAY     30       // the summary names precipitation from this chance up
#define WX_POP_AHEAD_S (12 * 3600)

void emitf(AlmEmitFn emit, void* ctx, int kind, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
void emitf(AlmEmitFn emit, void* ctx, int kind, const char* fmt, ...) {
  char line[128];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }
  emit(ctx, kind, line);
}

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) {
    q--;
  }
  return q;
}

int64_t localDay(int64_t t, int tzS) {
  return floorDiv(t + tzS, 86400);
}

int localHour(int64_t t, int tzS) {
  const int64_t s = t + tzS - localDay(t, tzS) * 86400;
  return (int)(s / 3600);
}

// "17C, Mostly clear" - the temperature in the Units setting, then the sky.
void tempSky(float tempC, int code, int units, char* out, size_t cap) {
  char t[16], cb[16];
  unitsFmtTemp(tempC, units, t, sizeof(t));
  if (code >= 0) {
    snprintf(out, cap, "%s, %s", t, wxCodeText(code, cb, sizeof(cb)));
  } else {
    snprintf(out, cap, "%s", t);
  }
}

// "Wind from NW 14 km/h, gusts 30" / "Wind calm" / "Wind 14 km/h" (no direction given).
void windText(float ms, float dirDeg, float gustMs, int units, char* out, size_t cap) {
  char w[16], g[16];
  unitsFmtWind(ms, units, true, w, sizeof(w));
  /* Calm when it SHOWS as 0 in the Units setting (review 2026-10-03: 0.2 m/s is "1 km/h" but
   * "0 mph" - "Wind from N 0 mph" read as a reading, not as calm). */
  const bool calm = wxHave(ms) && w[0] == '0' && w[1] == ' ';
  if (!wxHave(ms)) {
    snprintf(out, cap, "Wind not given");
    return;
  }
  char head[48];
  if (calm) {
    snprintf(head, sizeof(head), "Wind calm");
  } else if (wxHave(dirDeg)) {
    snprintf(head, sizeof(head), "Wind from %s %s", almCompass16(dirDeg), w);   // FROM: where it blows from
  } else {
    snprintf(head, sizeof(head), "Wind %s", w);
  }
  if (wxHave(gustMs) && gustMs > ms) {
    unitsFmtWind(gustMs, units, false, g, sizeof(g));
    snprintf(out, cap, "%s, gusts %s", head, g);
  } else {
    snprintf(out, cap, "%s", head);
  }
}

// The as-of stamp: "07:10" today, "Sat 07:10" this week, "Oct 3 07:10" else; "an unknown time".
void whenOrUnknown(int64_t t, int64_t now, int tzS, char* out, size_t cap) {
  if (t <= 0) {
    snprintf(out, cap, "an unknown time");
  } else {
    wxFmtWhen(t, now, tzS, out, cap);
  }
}

/* How far the cache's point is from the Almanac's place, in whole km - or -1 when it is the place
 * (within WX_MOVED_KM) or there is no place to measure from. Review 2026-10-03: fetched at home and
 * read 120 km away offline, the forecast and the home county's alerts showed as if for here. */
int kmFromHere(const AlmCtx* c, int32_t latE2, int32_t lonE2) {
  if (c->placeKind == ALM_PLACE_NONE) {
    return -1;
  }
  const double km = wxDistanceKm(c->lat, c->lon, latE2 / 100.0, lonE2 / 100.0);
  if (!(km > WX_MOVED_KM)) {
    return -1;
  }
  return km > 99999 ? 99999 : (int)lround(km);
}

int movedKm(const AlmCtx* c, const WxData* d) {
  return d->have ? kmFromHere(c, d->latE2, d->lonE2) : -1;
}

/* The same for the ALERTS, from their own point: a fetch whose forecast failed after a move
 * brought this place's alerts beside the old place's forecast (final review 2026-10-03). A file
 * from before the alerts' own point: the forecast's. */
int alertsKm(const AlmCtx* c, const WxData* d) {
  if (d->al.state == WX_AL_UNKNOWN) {
    return -1;
  }
  if (!d->alPointKnown) {
    return movedKm(c, d);
  }
  return kmFromHere(c, d->alLatE2, d->alLonE2);
}

void alertsBlock(const AlmCtx* c, const WxData* d, AlmEmitFn emit, void* ctx) {
  const WxAlerts& al = d->al;
  char when[32], line[128];
  whenOrUnknown(d->alertsUtc, c->now, c->tzS, when, sizeof(when));
  const bool failed = d->nwsLast == WX_HOST_FAILED;
  if (failed) {
    emit(ctx, ALM_ROW_WRAP, "Alerts not checked (no answer)");
  }
  /* Alerts from a point of their own (not the forecast's): their own "km from here" - the row
   * over the screen speaks for the forecast only then. */
  const int km = wxAlertsApart(d) ? alertsKm(c, d) : -1;
  if (km > 0) {
    emitf(emit, ctx, ALM_ROW_WARN, "Alerts for a place %d km from here", km);
  }
  switch (al.state) {
  case WX_AL_OUTSIDE:
    emit(ctx, ALM_ROW_INFO, "Alerts: US only (NWS)");
    return;
  case WX_AL_NONE:
    emitf(emit, ctx, ALM_ROW_WRAP, "No alerts at %s", when);
    return;
  case WX_AL_TOO_MANY:
    emitf(emit, ctx, ALM_ROW_WARN, "Alerts: %d+ (too many to list)", al.total > 0 ? al.total : 1);
    break;
  case WX_AL_OK:
    break;
  default:
    return;                                       // never asked: nothing to say about alerts
  }
  int shown = 0;
  for (int i = 0; i < al.n && i < WX_ALERTS; i++) {
    const WxAlert& a = al.a[i];
    if (!wxAlertActive(&a, c->now)) {
      continue;                                   // offline past its end: hidden
    }
    wxAlertText(&a, c->now, c->tzS, line, sizeof(line));
    emit(ctx, a.severity >= WX_SEV_SEVERE ? ALM_ROW_DANGER : ALM_ROW_WARN, line);
    shown++;
  }
  if (al.state == WX_AL_OK && shown == 0) {
    emitf(emit, ctx, ALM_ROW_WRAP, "The alerts of %s have ended", when);
    return;
  }
  if (al.kept > al.n) {
    emitf(emit, ctx, ALM_ROW_INFO, "+%d more alerts", al.kept - al.n);
  }
  /* Their own as-of whenever the forecast's as-of does not cover them - including no forecast. */
  if (failed || !d->have || d->alertsUtc != d->fetchedUtc) {
    emitf(emit, ctx, ALM_ROW_WRAP, "Alerts as of %s", when);
  }
}

/* "Now" (current, within an hour of the fetch) or "Forecast for 14:00" (the hour covering now).
 * A fetch of unknown time is never "Now": its age cannot be shown to be under the hour. */
void nowBlock(const AlmCtx* c, const WxData* d, AlmEmitFn emit, void* ctx) {
  char a[64], b[64];
  const int64_t age = d->fetchedUtc > 0 ? c->now - d->fetchedUtc : -1;
  if (d->fc.haveNow && d->fetchedUtc > 0 && age >= -600 && age <= WX_NOW_FRESH_S) {
    const WxNow& n = d->fc.now;
    tempSky(n.tempC, n.code, c->units, a, sizeof(a));
    emitf(emit, ctx, ALM_ROW_WRAP, "Now: %s", a);
    if (wxHave(n.feelsC) || wxHave(n.rhPct)) {
      unitsFmtTemp(n.feelsC, c->units, a, sizeof(a));
      if (wxHave(n.feelsC) && wxHave(n.rhPct)) {
        emitf(emit, ctx, ALM_ROW_WRAP, "Feels %s, humidity %d%%", a, (int)lround(n.rhPct));
      } else if (wxHave(n.feelsC)) {
        emitf(emit, ctx, ALM_ROW_WRAP, "Feels %s", a);
      } else {
        emitf(emit, ctx, ALM_ROW_WRAP, "Humidity %d%%", (int)lround(n.rhPct));
      }
    }
    windText(n.windMs, n.windDirDeg, n.gustMs, c->units, b, sizeof(b));
    emit(ctx, ALM_ROW_WRAP, b);
    if (wxHave(n.pressHpa)) {
      unitsFmtPressure(n.pressHpa, c->units, a, sizeof(a));
      emitf(emit, ctx, ALM_ROW_INFO, "Pressure %s", a);
    }
    return;
  }
  const int i = wxHourAt(&d->fc, c->now);
  if (i < 0) {
    return;
  }
  const WxHour& h = d->fc.hour[i];
  char hh[8];
  almFmtClock(h.t, c->tzS, ALM_ROUND_NEAREST, hh, sizeof(hh));
  tempSky(h.tempC, h.code, c->units, a, sizeof(a));
  emitf(emit, ctx, ALM_ROW_WRAP, "Forecast for %s: %s", hh, a);
  windText(h.windMs, NAN, NAN, c->units, b, sizeof(b));
  if (wxHave(h.popPct)) {
    b[0] = (char)tolower((unsigned char)b[0]);   // "wind ..." mid-sentence
    emitf(emit, ctx, ALM_ROW_WRAP, "Precip %d%%, %s", (int)lround(h.popPct), b);
  } else {
    emit(ctx, ALM_ROW_WRAP, b);
  }
}

void hoursBlock(const AlmCtx* c, const WxData* d, AlmEmitFn emit, void* ctx) {
  int first = -1;
  for (int i = 0; i < d->fc.nHours; i++) {
    if (d->fc.hour[i].t > c->now) {               // past hours dropped (the current one is "now")
      first = i;
      break;
    }
  }
  if (first < 0) {
    return;                                       // past the last hour: the days only
  }
  emit(ctx, ALM_ROW_INFO, "Next hours");
  const int64_t today = localDay(c->now, c->tzS);
  int rows = 0;
  for (int i = first; i < d->fc.nHours && rows < WX_HOUR_ROWS; i += WX_HOUR_STEP, rows++) {
    const WxHour& h = d->fc.hour[i];
    char hh[8], when[16], t[16], cb[16], pop[8];
    almFmtClock(h.t, c->tzS, ALM_ROUND_NEAREST, hh, sizeof(hh));
    if (localDay(h.t, c->tzS) == today) {
      snprintf(when, sizeof(when), "%s", hh);
    } else {
      int y, m, dd, wd;
      almDateOf(h.t, c->tzS, &y, &m, &dd, &wd, NULL);
      snprintf(when, sizeof(when), "%s %s", WDAY3[wd % 7], hh);
    }
    unitsFmtTemp(h.tempC, c->units, t, sizeof(t));
    pop[0] = '\0';
    if (wxHave(h.popPct)) {
      snprintf(pop, sizeof(pop), " %d%%", (int)lround(h.popPct));
    }
    emitf(emit, ctx, ALM_ROW_WRAP, "%s %s%s %s", when, t, pop,
          h.code >= 0 ? wxCodeText(h.code, cb, sizeof(cb)) : "");
  }
}

void daysBlock(const AlmCtx* c, const WxData* d, AlmEmitFn emit, void* ctx) {
  int y, m, dd, wd;
  const int64_t today = localDay(c->now, c->tzS);
  bool header = false;
  for (int i = 0; i < d->fc.nDays; i++) {
    const WxDay& x = d->fc.day[i];
    if (localDay(x.t + 43200, c->tzS) < today) {
      continue;                                   // its date (by the midpoint) has passed
    }
    if (!header) {
      emit(ctx, ALM_ROW_INFO, "Next days");
      header = true;
    }
    wxDayDate(x.t, c->tzS, &y, &m, &dd, &wd);
    const char* name = localDay(x.t + 43200, c->tzS) == today ? "Today" : WDAY3[wd % 7];
    char hi[16], lo[16], cb[16], w[64], pr[24], pop[8];
    unitsFmtTemp(x.maxC, c->units, hi, sizeof(hi));
    unitsFmtTemp(x.minC, c->units, lo, sizeof(lo));
    pop[0] = '\0';
    if (wxHave(x.popPct)) {
      snprintf(pop, sizeof(pop), ", %d%%", (int)lround(x.popPct));
    }
    emitf(emit, ctx, ALM_ROW_WRAP, "%s %s/%s, %s%s", name, hi, lo,
          x.code >= 0 ? wxCodeText(x.code, cb, sizeof(cb)) : "-", pop);
    windText(x.windMs, x.dirDeg, x.gustMs, c->units, w, sizeof(w));
    if (wxHave(x.precipMm) && x.precipMm > 0) {
      unitsFmtPrecip(x.precipMm, c->units, pr, sizeof(pr));
      w[0] = (char)tolower((unsigned char)w[0]);
      emitf(emit, ctx, ALM_ROW_WRAP, "%s, %s", pr, w);
    } else {
      emit(ctx, ALM_ROW_WRAP, w);
    }
  }
  if (!header && d->fc.nDays > 0) {
    char date[24];
    const WxDay& last = d->fc.day[d->fc.nDays - 1];
    wxDayDate(last.t, c->tzS, &y, &m, &dd, &wd);
    snprintf(date, sizeof(date), "%s %s %d", WDAY3[wd % 7], MON3[(m - 1 + 12) % 12], dd);
    emitf(emit, ctx, ALM_ROW_WRAP, "No forecast after %s - refresh on Wi-Fi", date);
  }
}

void asOfRows(const AlmCtx* c, const WxView* v, const WxData* d, AlmEmitFn emit, void* ctx) {
  if (!d->have) {
    return;
  }
  const char* place = d->placeName[0] ? d->placeName : "the place";
  char when[32], span[16];
  if (d->fetchedUtc <= 0) {
    emitf(emit, ctx, ALM_ROW_WRAP, "Fetched before the clock was set, for %s", place);
  } else if (v->clockTrusted && c->now >= d->fetchedUtc) {
    wxFmtWhen(d->fetchedUtc, c->now, c->tzS, when, sizeof(when));
    almFmtSpan(c->now - d->fetchedUtc, ALM_ROUND_DOWN, span, sizeof(span));
    emitf(emit, ctx, ALM_ROW_WRAP, "As of %s (%s ago) for %s", when, span, place);
  } else {
    // No trusted clock now: the stored date and time, no age.
    char date[24], hh[8];
    almFmtDate(d->fetchedUtc, c->tzS, date, sizeof(date));
    almFmtClock(d->fetchedUtc, c->tzS, ALM_ROUND_NEAREST, hh, sizeof(hh));
    emitf(emit, ctx, ALM_ROW_WRAP, "As of %s %s for %s", date, hh, place);
  }
  /* The place's offset AT THE FETCH against the phone's at that instant (almTzAt: today's, moved
   * by any US change in between) - review 2026-10-03: a cache fetched on Oct 31 in PDT told a phone
   * correctly set to UTC-8 on Nov 2 to "check the clock setting". The offset named is the place's
   * now, moved by the same change. */
  const int phoneThen = d->fetchedUtc > 0 ? almCtxTzAt(c, d->fetchedUtc) : c->tzS;   // a zone's rule too (0.9.82)
  if (d->fc.haveOffset && d->fc.utcOffsetS != phoneThen) {
    char off[16];
    almFmtUtcOffset(d->fc.utcOffsetS + (c->tzS - phoneThen), off, sizeof(off));
    emitf(emit, ctx, ALM_ROW_WRAP, "Local time here is %s - check the clock setting", off);
  }
}

void credit(AlmEmitFn emit, void* ctx) {
  emit(ctx, ALM_ROW_INFO, "Weather: Open-Meteo.com");
  emit(ctx, ALM_ROW_INFO, "(CC BY 4.0); alerts: NWS");
}

}  // namespace

// ---- the pieces ---------------------------------------------------------------------------------

void wxFmtWhen(int64_t t, int64_t now, int tzS, char* out, size_t cap) {
  if (!out || !cap) {
    return;
  }
  char hh[8], buf[32];
  almFmtClock(t, tzS, ALM_ROUND_NEAREST, hh, sizeof(hh));
  const int64_t dt = localDay(t, tzS), dn = localDay(now, tzS);
  int y, m, d, wd;
  almDateOf(t, tzS, &y, &m, &d, &wd, NULL);
  if (dt == dn) {
    snprintf(buf, sizeof(buf), "%s", hh);
  } else if (dt > dn - 6 && dt < dn + 6) {
    snprintf(buf, sizeof(buf), "%s %s", WDAY3[wd % 7], hh);
  } else {
    snprintf(buf, sizeof(buf), "%s %d %s", MON3[(m - 1 + 12) % 12], d, hh);
  }
  snprintf(out, cap, "%s", strlen(buf) < cap ? buf : "");
}

bool wxAlertActive(const WxAlert* a, int64_t now) {
  const int64_t end = a->ends ? a->ends : a->expires;
  return end == 0 || now < end;
}

void wxAlertText(const WxAlert* a, int64_t now, int tzS, char* out, size_t cap) {
  if (!out || !cap) {
    return;
  }
  const int64_t end = a->ends ? a->ends : a->expires;
  char from[32], to[32];
  if (a->onset > now) {
    wxFmtWhen(a->onset, now, tzS, from, sizeof(from));
    if (end) {
      wxFmtWhen(end, now, tzS, to, sizeof(to));
      snprintf(out, cap, "%s from %s to %s", a->event, from, to);
    } else {
      snprintf(out, cap, "%s from %s", a->event, from);
    }
  } else if (end) {
    wxFmtWhen(end, now, tzS, to, sizeof(to));
    snprintf(out, cap, "%s until %s", a->event, to);
  } else {
    snprintf(out, cap, "%s", a->event);
  }
  /* Past the MESSAGE's expiry but not the event's end (the critic's Q6: an Iowa Flood Watch expired
   * Oct 4 00:00 and ends Oct 6 03:00): NWS has most likely re-issued or cancelled it since. */
  if (a->ends && a->expires && now >= a->expires && now < a->ends) {
    const size_t n = strlen(out);
    snprintf(out + n, cap - n, " (may have changed)");
  }
}

int wxHourAt(const WxForecast* f, int64_t t) {
  for (int i = 0; i < f->nHours; i++) {
    if (t >= f->hour[i].t && t < f->hour[i].t + 3600) {
      return i;
    }
  }
  return -1;
}

void wxDayDate(int64_t t, int tzS, int* y, int* m, int* d, int* wday) {
  almDateOf(t + 43200, tzS, y, m, d, wday, NULL);
}

// ---- the screens --------------------------------------------------------------------------------

void almLinesWeather(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  const WxView* v = c->wx;
  if (!v || !v->d) {
    if (v && v->unreadable) {
      if (v->fetching) {
        emit(ctx, ALM_ROW_INFO, "Fetching the weather...");
      }
      if (v->note && v->note[0]) {
        emit(ctx, ALM_ROW_WRAP, v->note);
      }
      emit(ctx, ALM_ROW_WRAP, "The weather kept on the card could not be read just now - reopen the "
                              "Almanac, or Refresh on WiFi");
      credit(emit, ctx);
      return;
    }
    emit(ctx, ALM_ROW_INFO, "Reading the weather...");
    return;
  }
  const WxData* d = v->d;
  if (v->fetching) {
    emit(ctx, ALM_ROW_INFO, "Fetching the weather...");
  }
  if (v->note && v->note[0]) {
    emit(ctx, ALM_ROW_WRAP, v->note);
  }
  const int km = movedKm(c, d);
  if (km > 0) {
    emitf(emit, ctx, ALM_ROW_WARN, "%s a place %d km from here - refresh on WiFi",
          wxAlertsApart(d) ? "Forecast for" : "For", km);
  }
  if (!d->have && d->al.state == WX_AL_UNKNOWN && d->nwsLast != WX_HOST_FAILED) {
    emit(ctx, ALM_ROW_INFO, "No weather yet");
    emit(ctx, ALM_ROW_WRAP, "Refresh fetches it on WiFi: Open-Meteo's forecast, and NWS alerts "
                            "in the US. It is kept for offline.");
    credit(emit, ctx);
    return;
  }
  if (!c->clockKnown) {
    emit(ctx, ALM_ROW_INFO, "Clock not set yet");
    emit(ctx, ALM_ROW_INFO, "(NTP on WiFi, GPS, or mesh)");
    credit(emit, ctx);
    return;
  }
  alertsBlock(c, d, emit, ctx);
  if (d->have) {
    nowBlock(c, d, emit, ctx);
    hoursBlock(c, d, emit, ctx);
    daysBlock(c, d, emit, ctx);
  } else {
    emit(ctx, ALM_ROW_INFO, "No forecast yet");
  }
  asOfRows(c, v, d, emit, ctx);
  credit(emit, ctx);
}

void wxTodayAlertRow(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  if (!c->wx || !c->wx->d || !c->clockKnown) {
    return;
  }
  const WxAlerts& al = c->wx->d->al;
  if (al.state != WX_AL_OK && al.state != WX_AL_TOO_MANY) {
    return;
  }
  /* Alerts are for the NWS zone of the point THEY were asked for (not always the forecast's): from
   * elsewhere, they say how far. */
  const int km = alertsKm(c, c->wx->d);
  char away[24];
  away[0] = '\0';
  if (km > 0) {
    snprintf(away, sizeof(away), " %d km away", km);
  }
  for (int i = 0; i < al.n && i < WX_ALERTS; i++) {
    const WxAlert& a = al.a[i];                   // most severe first: the first in force
    if (wxAlertActive(&a, c->now)) {
      char line[128];
      wxAlertText(&a, c->now, c->tzS, line, sizeof(line));
      emitf(emit, ctx, a.severity >= WX_SEV_SEVERE ? ALM_ROW_DANGER : ALM_ROW_WARN, "Alert%s: %s", away, line);
      break;
    }
  }
  /* Too many to list: the listed ones are the first served, not the most severe of all - say so
   * on TODAY too, whatever row went above (review 2026-10-03). */
  if (al.state == WX_AL_TOO_MANY) {
    emitf(emit, ctx, ALM_ROW_WARN, "Alerts%s: %d+ - see Weather", away, al.total > 0 ? al.total : 1);
  }
}

void wxTodaySummaryRow(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  if (!c->wx || !c->wx->d || !c->clockKnown || !c->wx->d->have) {
    return;
  }
  const WxData* d = c->wx->d;
  const int64_t age = d->fetchedUtc > 0 ? c->now - d->fetchedUtc : -1;
  float temp = NAN;
  int code = -1;
  /* "Weather:" only for `current` within the hour of the fetch; from the hourly or the daily
   * forecast it is "Forecast:" (the spec's never-as-an-observation rule, as WEATHER's "Forecast
   * for 14:00" - review 2026-10-03). From another place: "... 120 km away:". */
  char label[40];
  const char* kind = "Forecast";
  if (d->fc.haveNow && d->fetchedUtc > 0 && age >= -600 && age <= WX_NOW_FRESH_S) {
    temp = d->fc.now.tempC;
    code = d->fc.now.code;
    kind = "Weather";
  } else {
    const int i = wxHourAt(&d->fc, c->now);
    if (i >= 0) {
      temp = d->fc.hour[i].tempC;
      code = d->fc.hour[i].code;
    }
  }
  const int km = movedKm(c, d);
  if (km > 0) {
    snprintf(label, sizeof(label), "%s %d km away", kind, km);
  } else {
    snprintf(label, sizeof(label), "%s", kind);
  }
  char t[16], cb[16];
  if (!wxHave(temp)) {
    // No hour covers now: today's day, if the forecast still has it.
    const int64_t today = localDay(c->now, c->tzS);
    for (int i = 0; i < d->fc.nDays; i++) {
      const WxDay& x = d->fc.day[i];
      if (localDay(x.t + 43200, c->tzS) == today) {
        char lo[16];
        unitsFmtTemp(x.maxC, c->units, t, sizeof(t));
        unitsFmtTemp(x.minC, c->units, lo, sizeof(lo));
        emitf(emit, ctx, ALM_ROW_WRAP, "%s today: %s/%s, %s", label, t, lo,
              x.code >= 0 ? wxCodeText(x.code, cb, sizeof(cb)) : "-");
        return;
      }
    }
    return;
  }
  unitsFmtTemp(temp, c->units, t, sizeof(t));
  // The likeliest wet hour in the next 12, when it is likely enough to mention.
  int best = -1;
  for (int i = 0; i < d->fc.nHours; i++) {
    const WxHour& h = d->fc.hour[i];
    if (h.t + 3600 <= c->now || h.t > c->now + WX_POP_AHEAD_S || !wxHave(h.popPct)) {
      continue;
    }
    if (h.popPct >= WX_POP_SAY && (best < 0 || h.popPct > d->fc.hour[best].popPct)) {
      best = i;
    }
  }
  if (best < 0) {
    if (code >= 0) {
      emitf(emit, ctx, ALM_ROW_WRAP, "%s: %s, %s", label, t, wxCodeText(code, cb, sizeof(cb)));
    } else {
      emitf(emit, ctx, ALM_ROW_WRAP, "%s: %s", label, t);
    }
    return;
  }
  const WxHour& h = d->fc.hour[best];
  const char* what = wxCodeIsSnow(h.code) ? "snow" : "rain";
  const int64_t today = localDay(c->now, c->tzS), day = localDay(h.t, c->tzS);
  const int hr = localHour(h.t, c->tzS);
  const char* when;
  if (h.t <= c->now) {
    when = "now";
  } else if (day == today) {
    when = hr < 5 ? "tonight" : hr < 12 ? "this morning" : hr < 17 ? "this afternoon"
         : hr < 21 ? "this evening" : "tonight";
  } else {
    when = hr < 5 ? "tonight" : hr < 12 ? "tomorrow morning" : hr < 17 ? "tomorrow afternoon"
         : "tomorrow evening";
  }
  emitf(emit, ctx, ALM_ROW_WRAP, "%s: %s, %s %d%% %s", label, t, what, (int)lround(h.popPct), when);
}
