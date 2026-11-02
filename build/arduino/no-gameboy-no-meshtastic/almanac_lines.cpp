/* almanac_lines.cpp - see almanac_lines.h. Every wording on the Almanac's screens lives here. */

#include "almanac_lines.h"
#include "elev_tiles.h"      // ELEV_Z_COARSE: "coarse" on a z10 height
#include "geo_grid.h"
#include "mesh_pos.h"        // meshPosFixUsable: the fix-quality bar every screen of the phone uses
#include "tz_grid.h"         // a ZONED clock's DST rule (0.9.82: Settings > Time zone)
#include "units.h"
#include "weather_lines.h"   // TODAY's alert and summary rows (0.9.81)
#include "wmm.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

const char* const WDAY3[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char* const WDAY[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                              "Saturday" };
const char* const MON3[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
const char* const MONTH[12] = { "January", "February", "March", "April", "May", "June", "July",
                                "August", "September", "October", "November", "December" };
const char* const SEASON[4] = { "March equinox", "June solstice", "September equinox",
                                "December solstice" };
const char* const QUARTER[4] = { "New moon", "First qtr", "Full moon", "Last qtr" };

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) {
    q--;
  }
  return q;
}

// Whole minutes of a local time or a span, by the rounding rule.
int64_t toMinutes(int64_t secs, int round) {
  if (round == ALM_ROUND_UP) {
    return floorDiv(secs + 59, 60);
  }
  if (round == ALM_ROUND_DOWN) {
    return floorDiv(secs, 60);
  }
  return floorDiv(secs + 30, 60);
}

void emitf(AlmEmitFn emit, void* ctx, int kind, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
void emitf(AlmEmitFn emit, void* ctx, int kind, const char* fmt, ...) {
  char line[72];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }
  emit(ctx, kind, line);
}

// The day's local midnight, and the date it is, from the context (not from the AlmDay: the
// no-place screens have none) - in the offset the shown day's times are given in (almDayTz).
int64_t shownT0(const AlmCtx* c) {
  return almDayT0(c);
}

// The shown day's parts are in the context's AlmDay (a day still being computed is partial).
bool haveParts(const AlmCtx* c, int parts) {
  return c->day && (c->day->have & parts) == parts;
}

const char* const COMPUTING = "Computing...";

// "What to do when there is no place": the guidance, on every screen that needs one.
void noPlaceRows(AlmEmitFn emit, void* ctx) {
  emit(ctx, ALM_ROW_INFO, "No place known yet");
  emit(ctx, ALM_ROW_WRAP, "Get one in Meshtastic: Places > pick a place > Measure from here, "
                          "or My node > GPS receiver on. Or open Maps once: its view is used.");
}

bool isToday(const AlmCtx* c) {
  return c->dayOffset == 0;
}

/* "Clock not set yet" / "Date out of range" / "No place known yet": the rows a screen shows in
 * place of its tables. False = the caller stops (the rows are out). */
bool preamble(const AlmCtx* c, bool needPlace, AlmEmitFn emit, void* ctx) {
  if (!c->clockKnown) {
    emit(ctx, ALM_ROW_INFO, "Clock not set yet");
    emit(ctx, ALM_ROW_INFO, "(NTP on WiFi, GPS, or mesh)");
    return false;
  }
  int y, m, d;
  if (!almDateOf(shownT0(c), almDayTzS(c), &y, &m, &d, NULL, NULL)) {
    emitf(emit, ctx, ALM_ROW_INFO, "Date out of range (%d-%d)", ALM_YEAR_MIN, ALM_YEAR_MAX);
    return false;
  }
  if (needPlace && c->placeKind == ALM_PLACE_NONE) {
    noPlaceRows(emit, ctx);
    return false;
  }
  if (needPlace && !c->day) {
    emit(ctx, ALM_ROW_INFO, COMPUTING);
    return false;
  }
  return true;
}

// "2h 5m ago", "12m ago", "3d ago" - how old a last GPS fix is (the place row).
void fmtAgo(uint32_t ms, char* out, size_t cap) {
  const uint32_t s = ms / 1000u;
  if (s < 60) {
    snprintf(out, cap, "%us ago", (unsigned)s);
  } else if (s < 3600) {
    snprintf(out, cap, "%um ago", (unsigned)(s / 60));
  } else if (s < 48u * 3600u) {
    snprintf(out, cap, "%uh %um ago", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
  } else {
    snprintf(out, cap, "%ud ago", (unsigned)(s / 86400));
  }
}

void placeRow(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  const char* n = c->placeName ? c->placeName : "?";
  char ago[24];
  switch (c->placeKind) {
  case ALM_PLACE_PIN:
    emit(ctx, ALM_ROW_INFO, "At me (pin)");
    break;
  case ALM_PLACE_GIVEN:
    emitf(emit, ctx, ALM_ROW_INFO, "At %.4f, %.4f", c->lat, c->lon);
    break;
  case ALM_PLACE_LAST_GPS:
    fmtAgo(c->placeAgeMs, ago, sizeof(ago));
    emitf(emit, ctx, ALM_ROW_INFO, "At last GPS %s", ago);
    break;
  case ALM_PLACE_PHONE:
    emit(ctx, ALM_ROW_INFO, "At phone GPS");          // the hotspot phone's fix (self_pos.h)
    break;
  case ALM_PLACE_LAST_PHONE:
    fmtAgo(c->placeAgeMs, ago, sizeof(ago));
    emitf(emit, ctx, ALM_ROW_INFO, "At last phone %s", ago);
    break;
  case ALM_PLACE_NET:
    // "At Wi-Fi +/-60 m": the name carries the size (self_pos.h's selfFixName), never cut.
    emitf(emit, ctx, ALM_ROW_INFO, "At %s", n);
    break;
  case ALM_PLACE_MAP_VIEW:
    emit(ctx, ALM_ROW_INFO, "At map view");
    break;
  case ALM_PLACE_WAYPOINT:
    /* A waypoint's name is anybody's (up to 23 characters: "At " + 23 is ~480 px of the row's
     * 232): a sentence the screen wraps, never a row cut to ".." (review 2026-09-27). */
    emitf(emit, ctx, ALM_ROW_WRAP, "At %s", n);
    break;
  default:
    emitf(emit, ctx, ALM_ROW_INFO, "At %s", n);
    break;
  }
}

void meshWarning(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  if (c->clockMesh) {
    emit(ctx, ALM_ROW_INFO, "Mesh time: check it!");
  }
}

/* Is the sun above the threshold that decides a legal bound, all day? Only asked when a bound is
 * missing; one position at the solar noon (or local noon when there is none). */
bool sunAboveThreshold(const AstroSunDay* sun, int64_t t0, double lat, double lon, int rule) {
  double alt = 0, az = 0;
  int64_t t = sun->noon ? sun->noon : t0 + 43200;
  astroSunPos(t, lat, lon, &alt, &az);
  const double threshold = (rule == ASTRO_LEGAL_CIVIL) ? -6.0 : -0.8333;
  return alt > threshold;
}

bool sunAboveAllDay(const AlmCtx* c, const AlmDay* day, int rule) {
  return sunAboveThreshold(&day->sun, day->t0, day->lat, day->lon, rule);
}

/* The whole-hour offsets of the US zones that change their clocks, in each half of the year:
 * daylight EDT -4 .. AKDT -8, HDT -9 (the Aleutians); standard EST -5 .. AKST -9, HST -10 (the
 * Aleutians in winter - and Hawaii, which never changes: Hawaii and Arizona (-7) are what
 * Settings "US daylight saving: no" is for; an offset cannot tell them apart). */
const int US_DAYLIGHT_MIN_H = -9, US_DAYLIGHT_MAX_H = -4;
const int US_STANDARD_MIN_H = -10, US_STANDARD_MAX_H = -5;

bool usRange(int h, int dst) {
  return dst ? (h >= US_DAYLIGHT_MIN_H && h <= US_DAYLIGHT_MAX_H)
             : (h >= US_STANDARD_MIN_H && h <= US_STANDARD_MAX_H);
}

/* 1 = the local day `day` (days since 1970-01-01) is in US daylight time: from the 2nd Sunday of
 * March up to the 1st Sunday of November. *changeDay = 1 when it IS one of those two Sundays. */
int usDstState(int64_t day, int* changeDay) {
  int y, m, d;
  astroCivil(day * 86400, &y, &m, &d, NULL, NULL);
  int marchDay = 0, novDay = 0;
  astroUsDst(y, &marchDay, &novDay);
  const int64_t mar = astroDaysFromCivil(y, 3, marchDay);
  const int64_t nov = astroDaysFromCivil(y, 11, novDay);
  if (changeDay) {
    *changeDay = (day == mar || day == nov) ? 1 : 0;
  }
  return (day >= mar && day < nov) ? 1 : 0;
}

/* Which side of a US change the phone's clock is on NOW, by almDstReminder's 2 AM rule: the local
 * day of (local time - 2 h), so the first two hours of a change day still count as the day
 * before (the old side: "set Time offset then") and from 2 AM the new side ("check Time offset").
 * *effDay = that day number - the day the change search in almDayTz counts from. */
int usSideNow(int64_t now, int tzS, int64_t* effDay) {
  const int64_t d = floorDiv(now + tzS - 2 * 3600, 86400);
  if (effDay) {
    *effDay = d;
  }
  return usDstState(d, NULL);
}

/* On a day screen (TODAY, SUN, MOON, SOLUNAR) shown across a US clock change: its times are in
 * the offset that day will have, and this row says so (almDayTzNote). Review 2026-09-27: Oct 27
 * at UTC-7 stepped to Nov 7 showed "Legal 07:31-18:10" for a day whose clocks will read
 * 06:31-17:10 - a legal-light end an hour late, with nothing on the screen to say so. */
void dstShiftRow(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  char note[64];
  if (c->tzZoned ? almZoneDayTzNote(c, note, sizeof(note))
                 : almDayTzNote(c->now, c->tzS, c->dayOffset, c->usDst, note, sizeof(note))) {
    emit(ctx, ALM_ROW_WRAP, note);              // 297 px of Bold 20: two rows, the words whole
  }
}

/* ON a change day, every day screen says so (almDstTodayNote): from 2 AM the phone is taken to
 * have been changed, and only the user knows whether it was. */
void dstTodayRow(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  char note[96];
  if (c->tzZoned ? almZoneDstTodayNote(c, note, sizeof(note))
                 : almDstTodayNote(c->now, c->tzS, c->usDst, note, sizeof(note))) {
    emit(ctx, ALM_ROW_WRAP, note);
  }
}

/* The local day number (days since 1970-01-01) of each US change in `year`. */
void usChanges(int year, int64_t* mar, int64_t* nov) {
  int marchDay = 0, novDay = 0;
  astroUsDst(year, &marchDay, &novDay);
  *mar = astroDaysFromCivil(year, 3, marchDay);
  *nov = astroDaysFromCivil(year, 11, novDay);
}

bool sunUpAllDay(const AstroSunDay* s, const AlmDay* day) {
  double alt = 0, az = 0;
  int64_t t = s->noon ? s->noon : day->t0 + 43200;
  astroSunPos(t, day->lat, day->lon, &alt, &az);
  return alt > -0.8333;
}

const char* ruleWord(int rule) {
  return rule == ASTRO_LEGAL_CIVIL ? "civil" : "30 min";
}

// "Legal 06:34-19:28 (30 min)": inward rounding (see the header). tz = the day's offset.
void legalRow(const AlmCtx* c, const AlmDay* day, int tz, AlmEmitFn emit, void* ctx) {
  int64_t first = 0, last = 0;
  astroLegalLight(&day->sun, c->rule, &first, &last);
  char a[8], b[8];
  almFmtClock(first, tz, ALM_ROUND_UP, a, sizeof(a));
  almFmtClock(last, tz, ALM_ROUND_DOWN, b, sizeof(b));
  if (first && last) {
    emitf(emit, ctx, ALM_ROW_INFO, "Legal %s-%s (%s)", a, b, ruleWord(c->rule));
  } else if (first) {
    emitf(emit, ctx, ALM_ROW_INFO, "Legal from %s (%s)", a, ruleWord(c->rule));
  } else if (last) {
    emitf(emit, ctx, ALM_ROW_INFO, "Legal until %s (%s)", b, ruleWord(c->rule));
  } else if (sunAboveAllDay(c, day, c->rule)) {
    emitf(emit, ctx, ALM_ROW_INFO, "Legal light all day (%s)", ruleWord(c->rule));
  } else {
    emitf(emit, ctx, ALM_ROW_INFO, "No legal light today (%s)", ruleWord(c->rule));
  }
}

// "Sun 07:03-18:58 (11h 55m)"
void sunSummaryRow(const AlmDay* day, int tz, AlmEmitFn emit, void* ctx) {
  const AstroSunDay& s = day->sun;
  char a[8], b[8], len[16];
  almFmtClock(s.rise, tz, ALM_ROUND_NEAREST, a, sizeof(a));
  almFmtClock(s.set, tz, ALM_ROUND_NEAREST, b, sizeof(b));
  if (s.rise && s.set && s.rise < s.set) {
    almFmtSpan(s.set - s.rise, ALM_ROUND_NEAREST, len, sizeof(len));
    emitf(emit, ctx, ALM_ROW_INFO, "Sun %s-%s (%s)", a, b, len);
  } else if (s.rise || s.set) {
    emitf(emit, ctx, ALM_ROW_INFO, "Sunrise %s, sunset %s", a, b);
  } else {
    emit(ctx, ALM_ROW_INFO, sunUpAllDay(&s, day) ? "Sun up all day" : "Sun down all day");
  }
}

/* The moon's phase at `t`. TODAY shows no age, so it never searches (astroMoonPhaseWith with no
 * new moon: ~215 libm calls). MOON's age: the cached new moons when they cover `t`; else, from
 * the app (noSearch), false - the row says "Computing..." - and from the serial command the
 * search itself (astroMoonPhaseAt, as before). True = ph->ageDays is real. */
bool phaseAt(const AlmCtx* c, int64_t t, bool wantAge, AstroMoonPhase* ph) {
  if (!wantAge) {
    astroMoonPhaseWith(t, 0, ph);
    return false;
  }
  if (c->moonAge && almMoonAgeCovers(c->moonAge, t)) {
    astroMoonPhaseWith(t, c->moonAge->prevNew, ph);
    return true;
  }
  if (c->noSearch) {
    astroMoonPhaseWith(t, 0, ph);
    return false;
  }
  astroMoonPhaseAt(t, ph);
  return true;
}

int pct(double illum) {
  int p = (int)floor(illum * 100.0 + 0.5);
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

struct Period { int64_t a, b; bool major; };

// The day's solunar periods in time order (majors +-60 min, minors +-30 min).
int periodsOf(const AstroSolunar* s, Period* out) {
  int n = 0;
  for (int k = 0; k < s->nMajor && k < 2; k++) {
    out[n].a = s->majorMid[k] - 3600;
    out[n].b = s->majorMid[k] + 3600;
    out[n].major = true;
    n++;
  }
  for (int k = 0; k < s->nMinor && k < 2; k++) {
    out[n].a = s->minorMid[k] - 1800;
    out[n].b = s->minorMid[k] + 1800;
    out[n].major = false;
    n++;
  }
  for (int i = 1; i < n; i++) {                   // insertion sort, n <= 4
    Period p = out[i];
    int j = i - 1;
    while (j >= 0 && out[j].a > p.a) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = p;
  }
  return n;
}

// TODAY's one-line period: the one we are in, else the next; another day: its first.
void periodRow(const AlmCtx* c, const AlmDay* day, int tz, AlmEmitFn emit, void* ctx) {
  Period p[4];
  int n = periodsOf(&day->sol, p);
  char a[8], b[8];
  if (isToday(c)) {
    int cur = -1;
    for (int i = 0; i < n; i++) {
      if (c->now >= p[i].a && c->now < p[i].b && (cur < 0 || (p[i].major && !p[cur].major))) {
        cur = i;
      }
    }
    if (cur >= 0) {
      almFmtClock(p[cur].b, tz, ALM_ROUND_NEAREST, b, sizeof(b));
      emitf(emit, ctx, ALM_ROW_INFO, "Now: %s until %s", p[cur].major ? "major" : "minor", b);
      return;
    }
    for (int i = 0; i < n; i++) {
      if (p[i].a > c->now) {
        almFmtClock(p[i].a, tz, ALM_ROUND_NEAREST, a, sizeof(a));
        almFmtClock(p[i].b, tz, ALM_ROUND_NEAREST, b, sizeof(b));
        emitf(emit, ctx, ALM_ROW_INFO, "Next: %s %s-%s", p[i].major ? "major" : "minor", a, b);
        return;
      }
    }
    emit(ctx, ALM_ROW_INFO, n ? "No more periods today" : "No periods today");
    return;
  }
  if (!n) {
    emit(ctx, ALM_ROW_INFO, "No periods this day");
    return;
  }
  almFmtClock(p[0].a, tz, ALM_ROUND_NEAREST, a, sizeof(a));
  almFmtClock(p[0].b, tz, ALM_ROUND_NEAREST, b, sizeof(b));
  emitf(emit, ctx, ALM_ROW_INFO, "First: %s %s-%s", p[0].major ? "major" : "minor", a, b);
}

// "alt 32, az 150 SSE" - whole degrees, no degree glyph (the Akrobat faces have none).
void altAzRow(const char* who, double alt, double az, AlmEmitFn emit, void* ctx) {
  int a = (int)floor(alt + 0.5);
  int z = (int)floor(az + 0.5) % 360;
  if (z < 0) {
    z += 360;
  }
  emitf(emit, ctx, ALM_ROW_INFO, "%s now: alt %d, az %d %s", who, a, z, almCompass16(az));
}

void entries(AlmEmitFn emit, void* ctx) {
  emit(ctx, ALM_ENTRY_SUN, "Sun...");
  emit(ctx, ALM_ENTRY_MOON, "Moon...");
  emit(ctx, ALM_ENTRY_SOLUNAR, "Solunar...");
  emit(ctx, ALM_ENTRY_WEATHER, "Weather...");         // 0.9.81 (weather_lines.cpp)
  emit(ctx, ALM_ENTRY_POSITION, "Position & GPS...");
  emit(ctx, ALM_ENTRY_DATE, "Date & seasons...");
  emit(ctx, ALM_ENTRY_SETTINGS, "Settings...");
}

// "3s", "12m", "5h", "2d"
void fmtAge(uint32_t ms, char* out, size_t cap) {
  uint32_t s = ms / 1000u;
  if (s < 60) {
    snprintf(out, cap, "%us", (unsigned)s);
  } else if (s < 3600) {
    snprintf(out, cap, "%um", (unsigned)(s / 60));
  } else if (s < 48u * 3600u) {
    snprintf(out, cap, "%uh", (unsigned)(s / 3600));
  } else {
    snprintf(out, cap, "%ud", (unsigned)(s / 86400));
  }
}

}  // namespace

// ---- the day's tables ---------------------------------------------------------------------------

void almDayCompute(AlmDay* d, int64_t t0, int tzS, double lat, double lon) {
  memset(d, 0, sizeof(*d));
  d->t0 = t0;
  d->tzS = tzS;
  d->lat = lat;
  d->lon = lon;
  astroSunDay(t0, lat, lon, &d->sun);
  astroMoonDay(t0, lat, lon, &d->moon);
  astroSolunar(t0, &d->sun, &d->moon, &d->sol);
  d->have = ALM_PART_CORE;
  d->valid = true;
}

int almDayEnsure(AlmDay* d, int needs, int64_t phaseFrom) {
  if (!d || !d->valid) {
    return 0;
  }
  int did = 0;
  if ((needs & ALM_NEED_PREV) && !d->havePrev) {
    astroSunDay(d->t0 - 86400, d->lat, d->lon, &d->prev);
    d->havePrev = true;
    did |= ALM_NEED_PREV;
  }
  if ((needs & ALM_NEED_NEXT) && !d->haveNext) {
    astroSunDay(d->t0 + 86400, d->lat, d->lon, &d->next);
    d->haveNext = true;
    did |= ALM_NEED_NEXT;
  }
  if (needs & ALM_NEED_PHASES) {
    bool keep = d->havePhases && phaseFrom >= d->phaseFrom;
    for (int q = 0; keep && q < 4; q++) {
      if (!d->phase[q] || phaseFrom >= d->phase[q]) {
        keep = false;                             // a quarter passed: the next one is another
      }
    }
    if (!keep) {
      for (int q = 0; q < 4; q++) {
        d->phase[q] = astroNextMoonPhase(phaseFrom, q);
      }
      d->phaseFrom = phaseFrom;
      d->havePhases = true;
      did |= ALM_NEED_PHASES;
    }
  }
  return did;
}

// ---- the moon's age without a search -------------------------------------------------------------

bool almMoonAgeCovers(const AlmMoonAge* a, int64_t t) {
  return a && a->step == 2 && a->prevNew && a->nextNew && t >= a->prevNew && t < a->nextNew;
}

bool almMoonAgeWork(AlmMoonAge* a, int64_t t) {
  if (!a) {
    return false;
  }
  if (almMoonAgeCovers(a, t)) {
    return true;
  }
  if (a->step == 2 || a->step < 0 || a->step > 2) {
    a->step = 0;                                  // a new lunation: start over from `t`
  }
  if (a->step == 0) {
    a->from = t;
    a->prevNew = astroPrevMoonPhase(t, 0);
    a->nextNew = 0;
    a->step = 1;
    return false;
  }
  /* Both searches from the SAME instant, so no new moon lies strictly between them - then the
   * pair covers every t in [prevNew, nextNew), whatever `t` this call was made for. */
  a->nextNew = astroNextMoonPhase(a->from, 0);
  a->step = 2;
  return almMoonAgeCovers(a, t);
}

// ---- the day in slices --------------------------------------------------------------------------

namespace {

bool phasesKept(const AlmDay* d, int64_t phaseFrom) {
  bool keep = d->havePhases && phaseFrom >= d->phaseFrom;
  for (int q = 0; keep && q < 4; q++) {
    if (!d->phase[q] || phaseFrom >= d->phase[q]) {
      keep = false;                               // a quarter passed: the next one is another
    }
  }
  return keep;
}

bool partDone(const AlmDay* d, const AlmDayWork* w, const AlmMoonAge* age, const AlmWant* want,
              int part) {
  switch (part) {
  case ALM_NEED_PHASES: return phasesKept(d, want->phaseFrom);
  case ALM_PART_AGE:    return !age || almMoonAgeCovers(age, want->ageAt);
  default:              return (w->done & part) != 0;
  }
}

int wantClosure(const AlmWant* want, const AlmMoonAge* age) {
  int parts = want->parts;
  if (parts & ALM_PART_SOL) {
    parts |= ALM_PART_SUN | ALM_PART_MOON;        // the solunar rating reads both
  }
  if (!age) {
    parts &= ~ALM_PART_AGE;
  }
  return parts;
}

// The next part to work on: what is under way first, then the screen's order.
int nextPart(const AlmDay* d, const AlmDayWork* w, const AlmMoonAge* age, const AlmWant* want) {
  const int parts = wantClosure(want, age);
  if (w->part && (parts & w->part) && !partDone(d, w, age, want, w->part)) {
    return w->part;
  }
  static const int ORDER[7] = { ALM_PART_SUN, ALM_NEED_NEXT, ALM_NEED_PREV, ALM_PART_MOON,
                                ALM_PART_SOL, ALM_PART_AGE, ALM_NEED_PHASES };
  static const int ORDER_MOON[7] = { ALM_PART_MOON, ALM_PART_SUN, ALM_PART_SOL, ALM_PART_AGE,
                                     ALM_NEED_PHASES, ALM_NEED_NEXT, ALM_NEED_PREV };
  const int* order = want->moonFirst ? ORDER_MOON : ORDER;
  for (int i = 0; i < 7; i++) {
    const int p = order[i];
    if ((parts & p) && !partDone(d, w, age, want, p)) {
      return p;
    }
  }
  return 0;
}

}  // namespace

void almDayWorkBegin(AlmDay* d, AlmDayWork* w, int64_t t0, int tzS, double lat, double lon,
                     bool keep) {
  memset(w, 0, sizeof(*w));
  if (!keep) {
    memset(d, 0, sizeof(*d));
  }
  d->t0 = t0;
  d->tzS = tzS;
  d->lat = lat;
  d->lon = lon;
}

int almDayWorkRun(AlmDay* d, AlmDayWork* w, AlmMoonAge* age, const AlmWant* want,
                  AstroMoreFn more, void* ctx) {
  if (!d || !w || !want) {
    return 0;
  }
  int did = 0;
  bool any = false;                               // a unit has run in this call
  for (;;) {
    const int part = nextPart(d, w, age, want);
    if (!part) {
      break;
    }
    if (any && more && !more(ctx)) {
      break;
    }
    if (part == ALM_NEED_PHASES || part == ALM_PART_AGE) {
      /* A search: ~20 ms on the phone. It OPENS a slice and never stretches one that has
       * already worked (the slice would be its budget plus a search). */
      if (any) {
        break;
      }
      any = true;
      w->lastPart = part;
      if (part == ALM_PART_AGE) {
        w->part = 0;
        if (almMoonAgeWork(age, want->ageAt)) {
          did |= ALM_PART_AGE;
        }
        continue;
      }
      if (w->part != ALM_NEED_PHASES) {
        w->part = ALM_NEED_PHASES;
        w->quarter = 0;
        w->phaseFrom = want->phaseFrom;
        d->havePhases = false;                    // the old ones are about to be wrong
      }
      w->q[w->quarter] = astroNextMoonPhase(w->phaseFrom, w->quarter);
      w->quarter++;
      if (w->quarter >= 4) {
        memcpy(d->phase, w->q, sizeof(d->phase));
        d->phaseFrom = w->phaseFrom;
        d->havePhases = true;
        w->part = 0;
        did |= ALM_NEED_PHASES;
      }
      continue;
    }
    if (part == ALM_PART_SOL) {
      any = true;
      w->lastPart = part;
      astroSolunar(d->t0, &d->sun, &d->moon, &d->sol);
      d->have |= ALM_PART_SOL;
      w->done |= ALM_PART_SOL;
      w->part = 0;
      did |= ALM_PART_SOL;
      continue;
    }
    // A sun or moon day: the day itself, the one before (SUN) or the one after (TODAY).
    if (w->part != part) {
      const int64_t t = part == ALM_NEED_PREV ? d->t0 - 86400
                      : part == ALM_NEED_NEXT ? d->t0 + 86400 : d->t0;
      astroDayJobStart(&w->job, part == ALM_PART_MOON ? ASTRO_BODY_MOON : ASTRO_BODY_SUN, t,
                       d->lat, d->lon);
      w->part = part;
    }
    any = true;
    w->lastPart = part;
    if (!astroDayJobRun(&w->job, more, ctx)) {
      break;                                      // more() said stop inside the day
    }
    switch (part) {
    case ALM_PART_SUN:  astroDayJobSun(&w->job, &d->sun); d->have |= ALM_PART_SUN; break;
    case ALM_PART_MOON: astroDayJobMoon(&w->job, &d->moon); d->have |= ALM_PART_MOON; break;
    case ALM_NEED_PREV: astroDayJobSun(&w->job, &d->prev); d->havePrev = true; break;
    default:            astroDayJobSun(&w->job, &d->next); d->haveNext = true; break;
    }
    w->done |= part;
    w->part = 0;
    did |= part;
  }
  /* The core is whole once all three were computed for THIS key (a place move keeps the old
   * ones on the screen, `have`, until theirs replace them). */
  d->valid = (d->have & ALM_PART_CORE) == ALM_PART_CORE;
  return did;
}

int almDayWorkLeft(const AlmDay* d, const AlmDayWork* w, const AlmMoonAge* age,
                   const AlmWant* want) {
  if (!d || !w || !want) {
    return 0;
  }
  const int parts = wantClosure(want, age);
  int left = 0;
  for (int b = 1; b <= ALM_PART_AGE; b <<= 1) {
    if ((parts & b) && !partDone(d, w, age, want, b)) {
      left |= b;
    }
  }
  return left;
}

int64_t almMidnight(int64_t now, int tzS, int dayOffset) {
  const int64_t localDay = floorDiv(now + tzS, 86400);
  return (localDay + dayOffset) * 86400 - tzS;
}

bool almPlaceOk(double lat, double lon) {
  return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;   // NaN fails every test
}

bool almDateOf(int64_t t, int tzS, int* y, int* m, int* d, int* wday, int* yday) {
  int yy = 0, mm = 0, dd = 0, wd = 0, yd = 0;
  astroCivil(t + tzS, &yy, &mm, &dd, &wd, &yd);
  if (y) *y = yy;
  if (m) *m = mm;
  if (d) *d = dd;
  if (wday) *wday = wd;
  if (yday) *yday = yd;
  return yy >= ALM_YEAR_MIN && yy <= ALM_YEAR_MAX;
}

int64_t almPhaseFrom(const AlmCtx* c) {
  return c->dayOffset == 0 ? c->now : almDayT0(c);
}

int almDayTzS(const AlmCtx* c) {
  if (c->tzZoned) {
    /* The shown day's offset by the zone's rule, at that day's local noon: on a change day that
     * is the new side, as the bare-offset rule has it (a change is at 2 AM). Today is the clock's
     * own offset, whatever the hour. */
    if (c->dayOffset == 0) {
      return c->tzS;
    }
    const int64_t d2 = floorDiv(c->now + c->tzS, 86400) + c->dayOffset;
    return almCtxTzAt(c, d2 * 86400 + 43200 - c->tzStdS);
  }
  return almDayTz(c->now, c->tzS, c->dayOffset, c->usDst, NULL);
}

int almCtxTzAt(const AlmCtx* c, int64_t t) {
  if (c->tzZoned && t >= c->tzSince) {
    const int m = tzRuleOffsetMinutes(c->tzStdS / 60, (uint8_t)c->tzRule, t);
    if (m != TZ_NO_OFFSET) {
      return m * 60;
    }
  }
  return almTzAt(c->now, c->tzS, c->usDst, t);
}

namespace {

/* A zoned clock's two changes in the local year y: the UTC instant, and the local date the clock
 * shows when it happens (2 AM on the OLD side). None for a zone without DST. */
struct ZoneChange {
  int64_t at;
  int64_t day;
  bool    forward;
};

int zoneChanges(const AlmCtx* c, int year, ZoneChange out[2]) {
  if (!c->tzZoned || c->tzRule != TZ_RULE_USCAN) {
    return 0;
  }
  const int stdMin = c->tzStdS / 60;
  out[0].at = tzUsCanDstStart(year, stdMin);
  out[0].day = floorDiv(out[0].at + c->tzStdS, 86400);
  out[0].forward = true;
  out[1].at = tzUsCanDstEnd(year, stdMin);
  out[1].day = floorDiv(out[1].at + c->tzStdS + 3600, 86400);
  out[1].forward = false;
  return 2;
}

bool putIfFits(const char* buf, char* out, size_t cap) {
  if (strlen(buf) >= cap) {
    return false;
  }
  strcpy(out, buf);
  return true;
}

}  // namespace

bool almZoneDstTodayNote(const AlmCtx* c, char* out, size_t cap) {
  if (!out || !cap) {
    return false;
  }
  out[0] = '\0';
  const int64_t today = floorDiv(c->now + c->tzS, 86400);
  int y = 0;
  astroCivil(today * 86400, &y, NULL, NULL, NULL, NULL);
  ZoneChange ch[2];
  const int n = zoneChanges(c, y, ch);
  for (int k = 0; k < n; k++) {
    if (ch[k].day == today) {
      const bool before = c->now < ch[k].at;
      char buf[96];
      snprintf(buf, sizeof(buf), "Clocks %s %s 1 h at 2 AM today - the phone %s by itself",
               before ? "go" : "went", ch[k].forward ? "forward" : "back",
               before ? "changes" : "changed");
      return putIfFits(buf, out, cap);
    }
  }
  return false;
}

bool almZoneDstReminder(const AlmCtx* c, char* out, size_t cap) {
  if (!out || !cap) {
    return false;
  }
  out[0] = '\0';
  if (almZoneDstTodayNote(c, out, cap)) {
    return true;
  }
  const int64_t today = floorDiv(c->now + c->tzS, 86400);
  int y = 0;
  astroCivil(today * 86400, &y, NULL, NULL, NULL, NULL);
  ZoneChange ch[2];
  const int n = zoneChanges(c, y, ch);
  for (int k = 0; k < n; k++) {
    if (ch[k].day - today >= 1 && ch[k].day - today <= 21) {
      int m = 0, d = 0, wd = 0;
      astroCivil(ch[k].day * 86400, NULL, &m, &d, &wd, NULL);
      char buf[96];
      snprintf(buf, sizeof(buf), "Clocks go %s 1 h %s %s %d - the phone changes by itself",
               ch[k].forward ? "forward" : "back", WDAY3[wd % 7], MON3[(m - 1) % 12], d);
      return putIfFits(buf, out, cap);
    }
  }
  return false;
}

bool almZoneDayTzNote(const AlmCtx* c, char* out, size_t cap) {
  if (!out || !cap) {
    return false;
  }
  out[0] = '\0';
  if (!c->tzZoned || c->dayOffset == 0) {
    return false;
  }
  const int tz = almDayTzS(c);
  if (tz == c->tzS) {
    return false;
  }
  /* The change between today and the shown day: the first after now (a day ahead), or the first
   * after the shown day (a day behind). Its date is the clock's just before it. */
  const int64_t d2 = floorDiv(c->now + c->tzS, 86400) + c->dayOffset;
  const int64_t from = c->dayOffset > 0 ? c->now : d2 * 86400 + 43200 - c->tzStdS;
  const int stdMin = c->tzStdS / 60;
  const int64_t at = tzRuleNextTransition(stdMin, (uint8_t)c->tzRule, from, NULL);
  const int before = at ? tzRuleOffsetMinutes(stdMin, (uint8_t)c->tzRule, at - 1) : TZ_NO_OFFSET;
  if (!at || before == TZ_NO_OFFSET) {
    return false;
  }
  int m = 0, d = 0;
  astroCivil(floorDiv(at + (int64_t)before * 60, 86400) * 86400, NULL, &m, &d, NULL, NULL);
  char off[16], buf[64];
  almFmtUtcOffset(tz, off, sizeof(off));
  snprintf(buf, sizeof(buf), "Times in %s (%s the %s %d change)", off,
           c->dayOffset > 0 ? "after" : "before", MON3[(m - 1) % 12], d);
  return putIfFits(buf, out, cap);
}

int64_t almDayT0(const AlmCtx* c) {
  const int64_t localDay = floorDiv(c->now + c->tzS, 86400);   // TODAY's date, by today's offset
  return (localDay + c->dayOffset) * 86400 - almDayTzS(c);
}

void almScreenWant(int screen, const AlmCtx* c, AlmWant* out) {
  memset(out, 0, sizeof(*out));
  out->phaseFrom = almPhaseFrom(c);
  out->ageAt = c->dayOffset == 0 ? c->now : almDayT0(c) + 43200;
  switch (screen) {
  case ALM_SCREEN_TODAY:
  case ALM_SCREEN_SUN:
  case ALM_SCREEN_MOON:
  case ALM_SCREEN_SOLUNAR:
    break;
  default:
    return;                                       // POSITION, DATE, SETTINGS, WEATHER: no day
  }
  if (!c->clockKnown || c->placeKind == ALM_PLACE_NONE) {
    return;
  }
  out->parts = ALM_PART_CORE;
  if (screen == ALM_SCREEN_TODAY && c->dayOffset == 0 && haveParts(c, ALM_PART_SUN)) {
    int64_t first = 0, last = 0;
    astroLegalLight(&c->day->sun, c->rule, &first, &last);
    if (almCountdownNeedsNext(c->now, first, last)) {
      out->parts |= ALM_NEED_NEXT;                // "Dark - first light 06:34" is tomorrow's
    }
  } else if (screen == ALM_SCREEN_SUN) {
    out->parts |= ALM_NEED_PREV;
  } else if (screen == ALM_SCREEN_MOON) {
    out->parts |= ALM_NEED_PHASES | ALM_PART_AGE;
    out->moonFirst = true;
  }
}

// ---- the formatters -----------------------------------------------------------------------------

void almFmtClock(int64_t t, int tzS, int round, char* out, size_t cap) {
  if (!out || !cap) {
    return;
  }
  char buf[8];
  if (t == 0) {
    strcpy(buf, "--:--");
  } else {
    int64_t mins = toMinutes(t + tzS, round);
    int64_t mod = mins - floorDiv(mins, 1440) * 1440;
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)(mod / 60), (int)(mod % 60));
  }
  if (strlen(buf) >= cap) {
    out[0] = '\0';
    return;
  }
  strcpy(out, buf);
}

void almFmtSpan(int64_t secs, int round, char* out, size_t cap) {
  if (!out || !cap) {
    return;
  }
  if (secs < 0) {
    secs = 0;
  }
  int64_t mins = toMinutes(secs, round);
  char buf[24];
  if (mins >= 60) {
    snprintf(buf, sizeof(buf), "%dh %02dm", (int)(mins / 60), (int)(mins % 60));
  } else {
    snprintf(buf, sizeof(buf), "%dm", (int)mins);
  }
  if (strlen(buf) >= cap) {
    out[0] = '\0';
    return;
  }
  strcpy(out, buf);
}

void almFmtDelta(int64_t secs, char* out, size_t cap) {
  if (!out || !cap) {
    return;
  }
  char buf[24];
  if (secs == 0) {
    strcpy(buf, "same");
  } else {
    const char sign = secs > 0 ? '+' : '-';
    int64_t a = secs > 0 ? secs : -secs;
    if (a > 99 * 3600) {
      a = 99 * 3600;
    }
    if (a < 60) {
      snprintf(buf, sizeof(buf), "%c%ds", sign, (int)a);
    } else if (a % 60 == 0) {
      snprintf(buf, sizeof(buf), "%c%dm", sign, (int)(a / 60));
    } else {
      snprintf(buf, sizeof(buf), "%c%dm %02ds", sign, (int)(a / 60), (int)(a % 60));
    }
  }
  if (strlen(buf) >= cap) {
    out[0] = '\0';
    return;
  }
  strcpy(out, buf);
}

void almFmtDate(int64_t t, int tzS, char* out, size_t cap) {
  if (!out || !cap) {
    return;
  }
  int y, m, d, wd;
  almDateOf(t, tzS, &y, &m, &d, &wd, NULL);
  char buf[24];
  snprintf(buf, sizeof(buf), "%s %s %d", WDAY3[wd % 7], MON3[(m - 1) % 12], d);
  if (strlen(buf) >= cap) {
    out[0] = '\0';
    return;
  }
  strcpy(out, buf);
}

void almFmtUtcOffset(int tzS, char* out, size_t cap) {
  if (!out || !cap) {
    return;
  }
  char buf[16];
  if (tzS == 0) {
    strcpy(buf, "UTC");
  } else {
    const char sign = tzS > 0 ? '+' : '-';
    int a = tzS > 0 ? tzS : -tzS;
    int h = a / 3600;
    int mi = (a % 3600) / 60;
    if (mi) {
      snprintf(buf, sizeof(buf), "UTC%c%d:%02d", sign, h, mi);
    } else {
      snprintf(buf, sizeof(buf), "UTC%c%d", sign, h);
    }
  }
  if (strlen(buf) >= cap) {
    out[0] = '\0';
    return;
  }
  strcpy(out, buf);
}

const char* almCompass16(double deg) {
  static const char* const P[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                     "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
  if (!(deg == deg) || deg > 1e9 || deg < -1e9) {
    return "?";
  }
  double r = fmod(deg, 360.0);
  if (r < 0) {
    r += 360.0;
  }
  int i = (int)floor((r + 11.25) / 22.5) % 16;
  return P[i];
}

void almCountdown(int64_t now, int64_t first, int64_t last, int64_t nextFirst, bool allDay,
                  int tzS, char* out, size_t cap) {
  if (!out || !cap) {
    return;
  }
  char span[16], at[8], buf[48];
  /* Where `now` stands against the day's legal window(s). Usually [first, last). Near the
   * polar circles, or in a zone far from its meridian, the evening's end can come BEFORE the
   * morning's start inside one local day ([t0, last) and [first, t0+1d)), and either end can be
   * missing (it happens in the next or the previous day). */
  bool legal = false;
  int64_t end = 0;          // the end of the window we are in; 0 = it runs past midnight
  int64_t start = 0;        // the next window's start today; 0 = none left today
  if (first && last && first < last) {
    legal = now >= first && now < last;
    end = last;
    start = now < first ? first : 0;
  } else if (first && last) {
    if (now < last) {
      legal = true;
      end = last;
    } else if (now >= first) {
      legal = true;
    } else {
      start = first;
    }
  } else if (first) {
    legal = now >= first;
    start = legal ? 0 : first;
  } else if (last) {
    legal = now < last;
    end = last;
  }
  if (!first && !last) {
    snprintf(buf, sizeof(buf), allDay ? "LEGAL LIGHT all day" : "No legal light today");
  } else if (legal && end && end - now < 60) {
    snprintf(buf, sizeof(buf), "LEGAL LIGHT: under 1m left");
  } else if (legal && end) {
    almFmtSpan(end - now, ALM_ROUND_DOWN, span, sizeof(span));
    snprintf(buf, sizeof(buf), "LEGAL LIGHT: %s left", span);
  } else if (legal) {
    snprintf(buf, sizeof(buf), "LEGAL LIGHT (no end today)");
  } else if (start) {
    almFmtSpan(start - now, ALM_ROUND_UP, span, sizeof(span));
    snprintf(buf, sizeof(buf), "First light in %s", span);
  } else if (nextFirst) {
    almFmtClock(nextFirst, tzS, ALM_ROUND_UP, at, sizeof(at));
    snprintf(buf, sizeof(buf), "Dark - first light %s", at);
  } else {
    snprintf(buf, sizeof(buf), "Dark - no light tomorrow");
  }
  if (strlen(buf) >= cap) {
    out[0] = '\0';
    return;
  }
  strcpy(out, buf);
}

bool almDstReminder(int64_t now, int tzS, bool usDst, char* out, size_t cap) {
  if (!out || !cap) {
    return false;
  }
  out[0] = '\0';
  if (!usDst || tzS % 3600 != 0) {
    return false;                                 // no US zone has a part-hour offset
  }
  const int h = tzS / 3600;
  const int64_t local = now + tzS;
  const int64_t today = floorDiv(local, 86400);
  const bool before2am = local - today * 86400 < 2 * 3600;
  int y, m, d;
  almDateOf(now, tzS, &y, &m, &d, NULL, NULL);
  int64_t mar = 0, nov = 0;
  usChanges(y, &mar, &nov);
  int marchDay = 0, novDay = 0;
  astroUsDst(y, &marchDay, &novDay);
  const bool eitherSide = usRange(h, 1) || usRange(h, 0);   // on the day: changed yet or not
  char buf[96];
  if (today == nov && eitherSide) {
    snprintf(buf, sizeof(buf), before2am ? "US clocks go back 1 h at 2 AM today - set Time offset then"
                                         : "US clocks went back 1 h at 2 AM today - check Time offset");
  } else if (today == mar && eitherSide) {
    snprintf(buf, sizeof(buf), before2am ? "US clocks go forward 1 h at 2 AM today - set Time offset then"
                                         : "US clocks went forward 1 h at 2 AM today - check Time offset");
  } else if (nov - today >= 1 && nov - today <= 21 && usRange(h, 1)) {
    snprintf(buf, sizeof(buf), "US clocks go back 1 h Sun Nov %d - set Time offset then", novDay);
  } else if (mar - today >= 1 && mar - today <= 21 && usRange(h, 0)) {
    snprintf(buf, sizeof(buf), "US clocks go forward 1 h Sun Mar %d - set Time offset then", marchDay);
  } else {
    return false;
  }
  if (strlen(buf) >= cap) {
    return false;
  }
  strcpy(out, buf);
  return true;
}

bool almCountdownNeedsNext(int64_t now, int64_t first, int64_t last) {
  return (first || last) && (!last || now >= last);
}

void almCountdownDay(int64_t now, int tzS, int rule, int64_t t0, double lat, double lon,
                     const AstroSunDay* today, const AstroSunDay* tomorrow, char* out, size_t cap) {
  if (!out || !cap) {
    return;
  }
  out[0] = '\0';
  if (!today) {
    return;
  }
  int64_t first = 0, last = 0, nextFirst = 0, nl = 0;
  astroLegalLight(today, rule, &first, &last);
  if (tomorrow && almCountdownNeedsNext(now, first, last)) {
    astroLegalLight(tomorrow, rule, &nextFirst, &nl);
  }
  bool allDay = false;
  if (!first && !last) {
    allDay = sunAboveThreshold(today, t0, lat, lon, rule);
  }
  almCountdown(now, first, last, nextFirst, allDay, tzS, out, cap);
}

int almDayTz(int64_t now, int tzS, int dayOffset, bool usDst, int64_t* changeDay) {
  if (changeDay) {
    *changeDay = 0;
  }
  if (!usDst || dayOffset == 0 || tzS % 3600 != 0) {
    return tzS;
  }
  const int64_t d2 = floorDiv(now + tzS, 86400) + dayOffset;   // the shown day (today's date + N)
  /* Today's SIDE by the 2 AM rule (usSideNow): before 2 AM on a change day the phone has not been
   * changed yet - d1 is then the day before, so the change between today and the shown day is
   * found (and said) like any other. */
  int64_t d1 = 0;
  const int dst1 = usSideNow(now, tzS, &d1);
  const int dst2 = usDstState(d2, NULL);
  if (dst1 == dst2 || !usRange(tzS / 3600, dst1)) {
    return tzS;                 // the same side (or both changes crossed), or no changing US zone
  }
  if (changeDay) {
    /* The one change between them: forward the first in (d1, d2], back the last in (d2, d1]. */
    const int64_t lo = d1 < d2 ? d1 : d2, hi = d1 < d2 ? d2 : d1;
    int ylo, yhi;
    astroCivil(lo * 86400, &ylo, NULL, NULL, NULL, NULL);
    astroCivil(hi * 86400, &yhi, NULL, NULL, NULL, NULL);
    for (int y = ylo; y <= yhi && !*changeDay; y++) {
      int64_t ch[2];
      usChanges(y, &ch[0], &ch[1]);
      for (int k = 0; k < 2; k++) {
        if (ch[k] > lo && ch[k] <= hi) {
          *changeDay = ch[k];
          break;
        }
      }
    }
  }
  return tzS + (dst2 - dst1) * 3600;
}

int almTzAt(int64_t now, int tzS, bool usDst, int64_t t) {
  if (!usDst || tzS % 3600 != 0) {
    return tzS;
  }
  const int dst1 = usSideNow(now, tzS, NULL);
  if (!usRange(tzS / 3600, dst1)) {
    return tzS;
  }
  /* The zone's standard offset, and t on its standard-time clock: daylight time runs from the
   * March change's 02:00 standard to the November change's 02:00 daylight = 01:00 standard. */
  const int stdS = tzS - dst1 * 3600;
  const int64_t ls = t + stdS;
  int y = 0;
  astroCivil(ls, &y, NULL, NULL, NULL, NULL);
  int64_t mar = 0, nov = 0;
  usChanges(y, &mar, &nov);
  const int dst2 = (ls >= mar * 86400 + 2 * 3600 && ls < nov * 86400 + 3600) ? 1 : 0;
  return tzS + (dst2 - dst1) * 3600;
}

bool almDstTodayNote(int64_t now, int tzS, bool usDst, char* out, size_t cap) {
  if (!out || !cap) {
    return false;
  }
  out[0] = '\0';
  int change = 0;
  usDstState(floorDiv(now + tzS, 86400), &change);
  return change && almDstReminder(now, tzS, usDst, out, cap);
}

bool almDayTzNote(int64_t now, int tzS, int dayOffset, bool usDst, char* out, size_t cap) {
  if (!out || !cap) {
    return false;
  }
  out[0] = '\0';
  int64_t change = 0;
  const int tz = almDayTz(now, tzS, dayOffset, usDst, &change);
  if (tz == tzS || !change) {
    return false;
  }
  int y, m, d;
  astroCivil(change * 86400, &y, &m, &d, NULL, NULL);
  char off[16], buf[64];
  almFmtUtcOffset(tz, off, sizeof(off));
  snprintf(buf, sizeof(buf), "Times in %s (%s the %s %d change)", off,
           dayOffset > 0 ? "after" : "before", MON3[(m - 1) % 12], d);
  if (strlen(buf) >= cap) {
    return false;
  }
  strcpy(out, buf);
  return true;
}

void almPickPlace(const AlmPlaceIn* in, AlmPlace* out) {
  memset(out, 0, sizeof(*out));
  if (!in) {
    return;
  }
  // A place the almanac can compute for: on the globe, and not the map's "nothing" 0,0.
  struct Ok {
    static bool at(int32_t la, int32_t lo) {
      return !(la == 0 && lo == 0) && almPlaceOk(la * 1e-7, lo * 1e-7);
    }
  };
  if (in->refOk && Ok::at(in->refLatI, in->refLonI)) {
    out->kind = in->refKind != ALM_PLACE_NONE ? in->refKind : ALM_PLACE_PIN;
    out->latI = in->refLatI;
    out->lonI = in->refLonI;
    const bool own = out->kind == ALM_PLACE_PHONE || out->kind == ALM_PLACE_LAST_PHONE ||
                     out->kind == ALM_PLACE_NET;
    out->ageMs = own ? in->refAgeMs : in->gpsAgeMs;
    snprintf(out->name, sizeof(out->name), "%s", in->refName ? in->refName : "?");
    return;
  }
  /* ⚠ The bar resolveReference() and the map's "me" use (meshPosFixUsable): under 4 satellites
   * or over HDOP 10 a fix was measured 20 km off. Such a fix is no place here either — and it is
   * judged per source BEFORE the newer one is chosen, so a fresh 3-satellite phone fix indoors
   * cannot push a usable 20-minute-old GPS fix aside and leave only the map's view. */
  const bool gpsOk = in->gpsFix && meshPosFixUsable(in->gpsSats, in->gpsHdopX10) &&
                     Ok::at(in->gpsLatI, in->gpsLonI);
  const bool phOk = in->phoneFix && meshPosFixUsable(in->phoneSats, in->phoneHdopX10) &&
                    Ok::at(in->phoneLatI, in->phoneLonI);
  if (phOk && (!gpsOk || in->phoneAgeMs < in->gpsAgeMs)) {
    out->kind = ALM_PLACE_LAST_PHONE;
    out->latI = in->phoneLatI;
    out->lonI = in->phoneLonI;
    out->ageMs = in->phoneAgeMs;
    strcpy(out->name, "last phone GPS");
    return;
  }
  if (gpsOk) {
    out->kind = ALM_PLACE_LAST_GPS;
    out->latI = in->gpsLatI;
    out->lonI = in->gpsLonI;
    out->ageMs = in->gpsAgeMs;
    strcpy(out->name, "last GPS");
    return;
  }
  if (in->viewOk && Ok::at(in->viewLatI, in->viewLonI)) {
    out->kind = ALM_PLACE_MAP_VIEW;
    out->latI = in->viewLatI;
    out->lonI = in->viewLonI;
    strcpy(out->name, "map view");
  }
}

bool almSamePlace(double lat1, double lon1, double lat2, double lon2) {
  // NaN fails every comparison, so it is never the same place as anything.
  return fabs(lat1 - lat2) <= ALM_SAME_PLACE_DEG && fabs(lon1 - lon2) <= ALM_SAME_PLACE_DEG;
}

void almTitle(int screen, bool clockKnown, int64_t now, int tzS, int dayOffset, char* out,
              size_t cap) {
  if (!out || !cap) {
    return;
  }
  char buf[32];
  const bool dayScreen = screen == ALM_SCREEN_TODAY || screen == ALM_SCREEN_SUN ||
                         screen == ALM_SCREEN_MOON || screen == ALM_SCREEN_SOLUNAR;
  if (!dayScreen || !clockKnown) {
    snprintf(buf, sizeof(buf), "%s", screen == ALM_SCREEN_POSITION ? "Position & GPS"
                                   : screen == ALM_SCREEN_DATE ? "Date & seasons"
                                   : screen == ALM_SCREEN_SETTINGS ? "Settings"
                                   : screen == ALM_SCREEN_WEATHER ? "Weather" : "Almanac");
  } else {
    int y, m, d, wd;
    almDateOf(almMidnight(now, tzS, dayOffset), tzS, &y, &m, &d, &wd, NULL);
    const char* md = MON3[(m - 1 + 12) % 12];
    switch (screen) {
    case ALM_SCREEN_SUN:     snprintf(buf, sizeof(buf), "Sun: %s %d", md, d); break;
    case ALM_SCREEN_MOON:    snprintf(buf, sizeof(buf), "Moon: %s %d", md, d); break;
    case ALM_SCREEN_SOLUNAR: snprintf(buf, sizeof(buf), "Solunar: %s %d", md, d); break;
    default:                 snprintf(buf, sizeof(buf), "%s %d (%s)", md, d, WDAY3[wd % 7]); break;
    }
  }
  if (strlen(buf) >= cap) {
    out[0] = '\0';
    return;
  }
  strcpy(out, buf);
}

int almMenuTop(int oldSel, int oldTop, int newSel, int count, int visible) {
  if (count <= 0 || visible <= 0 || count <= visible) {
    return 0;
  }
  int top = newSel - (oldSel - oldTop);          // the selection where it was on the glass
  if (top > count - visible) top = count - visible;
  if (top < 0) top = 0;
  if (newSel >= 0 && newSel < count) {           // ...and never off it
    if (newSel < top) top = newSel;
    if (newSel >= top + visible) top = newSel - visible + 1;
  }
  return top;
}

// ---- the screens --------------------------------------------------------------------------------

void almLinesToday(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  if (!c->clockKnown) {
    preamble(c, false, emit, ctx);
    entries(emit, ctx);
    return;
  }
  const int tz = almDayTzS(c);                    // today's offset, or the day's across a change
  const int64_t t0 = shownT0(c);
  char date[24];
  almFmtDate(t0, tz, date, sizeof(date));
  int y, m, d;
  if (!almDateOf(t0, tz, &y, &m, &d, NULL, NULL)) {
    preamble(c, false, emit, ctx);
    entries(emit, ctx);
    return;
  }
  if (c->dayOffset == 0) {
    emit(ctx, ALM_ROW_INFO, date);
  } else if (c->dayOffset == 1) {
    emitf(emit, ctx, ALM_ROW_INFO, "%s (tomorrow)", date);
  } else if (c->dayOffset == -1) {
    emitf(emit, ctx, ALM_ROW_INFO, "%s (yesterday)", date);
  } else {
    emitf(emit, ctx, ALM_ROW_INFO, "%s (%+d days)", date, c->dayOffset);
  }

  /* A day still being computed (the app's slices) shows what is there and ONE "Computing..."
   * where the first missing part would be; each part's rows appear as it lands. */
  const bool place = c->placeKind != ALM_PLACE_NONE;
  const AlmDay* day = c->day;
  const bool sun = place && haveParts(c, ALM_PART_SUN);
  const bool core = place && haveParts(c, ALM_PART_CORE);
  bool said = false;
  if (place) {
    dstShiftRow(c, emit, ctx);          // no place, no clock times to qualify
  }
  if (place && isToday(c)) {
    int64_t first = 0, last = 0;
    if (sun) {
      astroLegalLight(&day->sun, c->rule, &first, &last);
    }
    if (!sun || (almCountdownNeedsNext(c->now, first, last) && !day->haveNext)) {
      emit(ctx, ALM_ROW_INFO, COMPUTING);         // the countdown (after dark: tomorrow's sun)
      said = true;
    } else {
      char line[48];
      almCountdownDay(c->now, c->tzS, c->rule, day->t0, day->lat, day->lon, &day->sun,
                      day->haveNext ? &day->next : NULL, line, sizeof(line));
      emit(ctx, ALM_ROW_INFO, line);
    }
  }
  meshWarning(c, emit, ctx);
  if (isToday(c)) {
    wxTodayAlertRow(c, emit, ctx);      // an active NWS alert, in its colour, near the top (0.9.81)
  }
  if (place) {
    dstTodayRow(c, emit, ctx);          // a change day: over the clock times it qualifies
  }
  if (sun) {
    legalRow(c, day, tz, emit, ctx);
    sunSummaryRow(day, tz, emit, ctx);
  } else if (place && !said) {
    emit(ctx, ALM_ROW_INFO, COMPUTING);
    said = true;
  }
  /* No age on this screen, so no search: the phase at one instant (~215 libm calls, ~6 ms on the
   * phone). On a stepped day it waits, with the rest, for the day's first slice: Left/Right onto
   * a day not computed yet does no astronomy at all in the key handler (the app's rule). */
  if (isToday(c) || !place || (day && day->have)) {
    AstroMoonPhase ph;
    phaseAt(c, isToday(c) ? c->now : t0 + 43200, false, &ph);
    emitf(emit, ctx, ALM_ROW_INFO, "Moon: %s %d%%", astroPhaseName(ph.phase), pct(ph.illum));
  }
  if (place) {
    if (core) {
      emitf(emit, ctx, ALM_ROW_INFO, "Solunar: %s", astroSolunarRatingName(day->sol.rating));
      periodRow(c, day, tz, emit, ctx);
    } else if (!said) {
      emit(ctx, ALM_ROW_INFO, COMPUTING);
    }
    placeRow(c, emit, ctx);
  } else {
    noPlaceRows(emit, ctx);
  }
  if (isToday(c)) {
    wxTodaySummaryRow(c, emit, ctx);    // "Weather: 52F, rain 60% this afternoon" (0.9.81)
  }
  entries(emit, ctx);
}

void almLinesSun(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  if (!preamble(c, true, emit, ctx)) {
    return;
  }
  const AlmDay* day = c->day;
  const AstroSunDay& s = day->sun;
  const int tz = almDayTzS(c);
  char t[8], span[16], delta[24];
  placeRow(c, emit, ctx);
  dstShiftRow(c, emit, ctx);
  dstTodayRow(c, emit, ctx);
  if (!haveParts(c, ALM_PART_SUN)) {
    emit(ctx, ALM_ROW_INFO, COMPUTING);
    meshWarning(c, emit, ctx);
    return;
  }

  int64_t first = 0, last = 0;
  astroLegalLight(&s, c->rule, &first, &last);
  if (first) {
    almFmtClock(first, tz, ALM_ROUND_UP, t, sizeof(t));
    emitf(emit, ctx, ALM_ROW_INFO, "First legal light %s", t);
  } else {
    emit(ctx, ALM_ROW_INFO, "First legal light: none");
  }
  struct Ev { const char* name; int64_t at; };
  const Ev evs[3] = { { "Sunrise", s.rise }, { "Solar noon", s.noon }, { "Sunset", s.set } };
  for (int i = 0; i < 3; i++) {
    if (evs[i].at) {
      almFmtClock(evs[i].at, tz, ALM_ROUND_NEAREST, t, sizeof(t));
      emitf(emit, ctx, ALM_ROW_INFO, "%s %s", evs[i].name, t);
    } else {
      emitf(emit, ctx, ALM_ROW_INFO, "%s: none today", evs[i].name);
    }
  }
  if (last) {
    almFmtClock(last, tz, ALM_ROUND_DOWN, t, sizeof(t));
    emitf(emit, ctx, ALM_ROW_INFO, "Last legal light %s", t);
  } else {
    emit(ctx, ALM_ROW_INFO, "Last legal light: none");
  }
  if (c->rule != ASTRO_LEGAL_CIVIL) {
    // The civil twilight the other rule would use - near 47 N it is the longer one from October.
    almFmtClock(s.dawn, tz, ALM_ROUND_NEAREST, t, sizeof(t));
    emitf(emit, ctx, ALM_ROW_INFO, s.dawn ? "Civil dawn %s" : "Civil dawn: none", t);
    almFmtClock(s.dusk, tz, ALM_ROUND_NEAREST, t, sizeof(t));
    emitf(emit, ctx, ALM_ROW_INFO, s.dusk ? "Civil dusk %s" : "Civil dusk: none", t);
  }

  if (s.rise && s.set && s.rise < s.set) {
    const int64_t len = s.set - s.rise;
    almFmtSpan(len, ALM_ROUND_NEAREST, span, sizeof(span));
    emitf(emit, ctx, ALM_ROW_INFO, "Day length %s", span);
    const AstroSunDay& p = day->prev;
    if (!day->havePrev) {
      emit(ctx, ALM_ROW_INFO, "Day before: computing...");
    } else if (p.rise && p.set && p.rise < p.set) {
      almFmtDelta(len - (p.set - p.rise), delta, sizeof(delta));
      emitf(emit, ctx, ALM_ROW_INFO, "%s vs the day before", delta);
    }
  } else if (!s.rise && !s.set) {
    emit(ctx, ALM_ROW_INFO, sunUpAllDay(&s, day) ? "Sun up all day" : "Sun down all day");
  }
  if (isToday(c)) {
    double alt = 0, az = 0;
    astroSunPos(c->now, day->lat, day->lon, &alt, &az);
    altAzRow("Sun", alt, az, emit, ctx);
  }
  emit(ctx, ALM_ROW_INFO, c->rule == ASTRO_LEGAL_CIVIL ? "Rule: civil twilight (Settings)"
                                                       : "Rule: 30 min (Settings)");
  meshWarning(c, emit, ctx);
}

void almLinesMoon(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  if (!preamble(c, true, emit, ctx)) {
    return;
  }
  const AlmDay* day = c->day;
  const AstroMoonDay& mo = day->moon;
  const int tz = almDayTzS(c);
  char t[8], date[24];
  placeRow(c, emit, ctx);
  dstShiftRow(c, emit, ctx);
  dstTodayRow(c, emit, ctx);
  if (!haveParts(c, ALM_PART_MOON)) {
    emit(ctx, ALM_ROW_INFO, COMPUTING);
    return;
  }

  struct Ev { const char* name; const char* none; int64_t at; };
  const Ev evs[4] = { { "Moonrise", "No moonrise today", mo.rise },
                      { "Overhead", "Not overhead today", mo.transit },
                      { "Moonset", "No moonset today", mo.set },
                      { "Underfoot", "Not underfoot today", mo.under } };
  int order[4];
  int n = 0;
  for (int i = 0; i < 4; i++) {                   // the ones that happen, in time order...
    if (!evs[i].at) {
      continue;
    }
    int j = n - 1;
    while (j >= 0 && evs[order[j]].at > evs[i].at) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = i;
    n++;
  }
  for (int i = 0; i < n; i++) {
    almFmtClock(evs[order[i]].at, tz, ALM_ROUND_NEAREST, t, sizeof(t));
    emitf(emit, ctx, ALM_ROW_INFO, "%s %s", evs[order[i]].name, t);
  }
  for (int i = 0; i < 4; i++) {                   // ...then, said, the ones that do not
    if (!evs[i].at) {
      emit(ctx, ALM_ROW_INFO, evs[i].none);
    }
  }

  const bool today = isToday(c);
  AstroMoonPhase ph;
  const bool aged = phaseAt(c, today ? c->now : shownT0(c) + 43200, true, &ph);
  emit(ctx, ALM_ROW_INFO, astroPhaseName(ph.phase));
  emitf(emit, ctx, ALM_ROW_INFO, today ? "Illuminated %d%%" : "Illuminated %d%% at noon",
        pct(ph.illum));
  if (aged) {
    emitf(emit, ctx, ALM_ROW_INFO, "Age %.1f days", ph.ageDays);
  } else {
    emit(ctx, ALM_ROW_INFO, "Age: computing...");
  }

  if (!day->havePhases) {
    emit(ctx, ALM_ROW_INFO, "Quarters: computing...");
  } else {
    int order[4] = { 0, 1, 2, 3 };
    for (int i = 1; i < 4; i++) {
      int k = order[i];
      int j = i - 1;
      while (j >= 0 && day->phase[order[j]] > day->phase[k]) {
        order[j + 1] = order[j];
        j--;
      }
      order[j + 1] = k;
    }
    for (int i = 0; i < 4; i++) {
      const int q = order[i];
      if (!day->phase[q]) {
        continue;
      }
      /* Each in the offset the clock will have THEN (almTzAt): a quarter past a US change is in
       * that side's time, date and all (Oct 25 at -7: new moon Sun Nov 8 23:02, not Mon Nov 9). */
      const int qtz = almCtxTzAt(c, day->phase[q]);
      almFmtDate(day->phase[q], qtz, date, sizeof(date));
      almFmtClock(day->phase[q], qtz, ALM_ROUND_NEAREST, t, sizeof(t));
      emitf(emit, ctx, ALM_ROW_INFO, "%s %s %s", QUARTER[q], date, t);
    }
  }
  if (today) {
    double alt = 0, az = 0;
    astroMoonPos(c->now, day->lat, day->lon, &alt, &az);
    // Up = the upper limb above the 34' horizon, the rise/set definition (0.5667 + ~0.26).
    if (alt > -0.83) {
      altAzRow("Moon", alt, az, emit, ctx);
    } else {
      emit(ctx, ALM_ROW_INFO, "Moon now: below the horizon");
    }
  }
}

void almLinesSolunar(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  if (!preamble(c, true, emit, ctx)) {
    return;
  }
  const AlmDay* day = c->day;
  const int tz = almDayTzS(c);
  placeRow(c, emit, ctx);
  dstShiftRow(c, emit, ctx);
  dstTodayRow(c, emit, ctx);
  if (!haveParts(c, ALM_PART_CORE)) {
    emit(ctx, ALM_ROW_INFO, COMPUTING);
    return;
  }
  emitf(emit, ctx, ALM_ROW_INFO, "Rating: %s (%d of 4)", astroSolunarRatingName(day->sol.rating),
        day->sol.rating);
  Period p[4];
  const int n = periodsOf(&day->sol, p);
  char a[8], b[8];
  for (int pass = 0; pass < 2; pass++) {           // majors, then minors, each in time order
    for (int i = 0; i < n; i++) {
      if (p[i].major != (pass == 0)) {
        continue;
      }
      almFmtClock(p[i].a, tz, ALM_ROUND_NEAREST, a, sizeof(a));
      almFmtClock(p[i].b, tz, ALM_ROUND_NEAREST, b, sizeof(b));
      const bool now = isToday(c) && c->now >= p[i].a && c->now < p[i].b;
      emitf(emit, ctx, ALM_ROW_INFO, "%s %s-%s%s", p[i].major ? "Major" : "Minor", a, b,
            now ? " now" : "");
    }
  }
  if (!n) {
    emit(ctx, ALM_ROW_INFO, "No periods (no moon events)");
  }
  emit(ctx, ALM_ROW_INFO, "Folk tables (J. A. Knight)");
}

void almLinesPosition(const AlmCtx* c, const AlmPos* p, AlmEmitFn emit, void* ctx) {
  char a[24], b[24];
  if (c->placeKind == ALM_PLACE_NONE) {
    noPlaceRows(emit, ctx);
  } else {
    switch (c->placeKind) {
    case ALM_PLACE_GPS:
      fmtAge(p->gpsAgeMs, a, sizeof(a));
      emitf(emit, ctx, ALM_ROW_INFO, "Source: GPS %s", a);
      break;
    case ALM_PLACE_LAST_GPS:
      fmtAge(p->gpsAgeMs, a, sizeof(a));
      emitf(emit, ctx, ALM_ROW_INFO, "Source: last GPS %s", a);
      break;
    case ALM_PLACE_PHONE:
      fmtAge(c->placeAgeMs, a, sizeof(a));
      emitf(emit, ctx, ALM_ROW_INFO, "Source: phone GPS %s", a);
      break;
    case ALM_PLACE_LAST_PHONE:
      fmtAge(c->placeAgeMs, a, sizeof(a));
      emitf(emit, ctx, ALM_ROW_INFO, "Source: last phone %s", a);
      break;
    case ALM_PLACE_NET:
      // "Source: Wi-Fi +/-60 m": the age is its last confirming scan's, kept off this row.
      emitf(emit, ctx, ALM_ROW_INFO, "Source: %s", c->placeName ? c->placeName : "Wi-Fi");
      break;
    case ALM_PLACE_MAP_VIEW:
      emit(ctx, ALM_ROW_INFO, "Source: map view");
      break;
    case ALM_PLACE_PIN:
      emit(ctx, ALM_ROW_INFO, "Source: pin");
      break;
    case ALM_PLACE_WAYPOINT:
      // Anybody's name, up to 23 characters: wrapped, never cut (placeRow's note).
      emitf(emit, ctx, ALM_ROW_WRAP, "Source: waypoint %s", c->placeName ? c->placeName : "?");
      break;
    default:
      emit(ctx, ALM_ROW_INFO, "Source: typed in");
      break;
    }
    GeoUtm u;
    if (c->coordsCoarse) {
      // The console at a measured place (AlmCtx.coordsCoarse): ~1 km, and nothing finer.
      emitf(emit, ctx, ALM_ROW_INFO, "~%.2f, %.2f (rounded here)", c->lat, c->lon);
      emit(ctx, ALM_ROW_INFO, "UTM/MGRS: on the screen only");
    } else {
      emitf(emit, ctx, ALM_ROW_INFO, "%.5f, %.5f", c->lat, c->lon);
      geoFmtDdm(c->lat, true, a, sizeof(a));
      geoFmtDdm(c->lon, false, b, sizeof(b));
      emitf(emit, ctx, ALM_ROW_INFO, "%s %s", a, b);
      if (geoToUtm(c->lat, c->lon, &u)) {
        // Truncated to the metre, as the MGRS digits are (a grid reference names the square).
        emitf(emit, ctx, ALM_ROW_INFO, "UTM %d%c %ldE %ldN", u.zone, u.band,
              (long)floor(u.e + 1e-6), (long)floor(u.n + 1e-6));
        char mgrs[24];
        if (geoToMgrs(c->lat, c->lon, mgrs, sizeof(mgrs))) {
          emitf(emit, ctx, ALM_ROW_INFO, "MGRS %s", mgrs);
        }
      } else {
        emit(ctx, ALM_ROW_INFO, "UTM/MGRS: none (polar)");
      }
    }
    switch (p->elevState) {
    case ALM_ELEV_OK:
      unitsFmtAlt(p->elevM, c->units, false, a, sizeof(a));
      emitf(emit, ctx, ALM_ROW_INFO, "Ground %s%s", a, p->elevZ == ELEV_Z_COARSE ? " coarse" : "");
      break;
    case ALM_ELEV_NODATA:  emit(ctx, ALM_ROW_INFO, "Ground: no data here"); break;
    case ALM_ELEV_NOTILE:  emit(ctx, ALM_ROW_INFO, "Ground: no tile here"); break;
    case ALM_ELEV_IOERR:   emit(ctx, ALM_ROW_INFO, "Ground: card read error"); break;
    case ALM_ELEV_NOLAYER: emit(ctx, ALM_ROW_INFO, "Ground: no /maps/elev"); break;
    default:               emit(ctx, ALM_ROW_INFO, "Ground: reading..."); break;
    }
  }

  // The receiver, whatever the place came from.
  if (!p->gpsOn) {
    emit(ctx, ALM_ROW_INFO, "GPS receiver off");
  } else if (!p->gpsHaveFix) {
    if (p->sats >= 0) {
      emitf(emit, ctx, ALM_ROW_INFO, "GPS: no fix yet (%d sats)", p->sats);
    } else {
      emit(ctx, ALM_ROW_INFO, "GPS: no fix yet");
    }
  } else {
    /* Each by the age of the sentence that carried it (review 2026-09-27): the fix's own age is
     * refreshed by an RMC AND a GGA, so RMCs lost to bad checksums while GGAs arrive would keep
     * an old speed looking current (and lost GGAs an old altitude). */
    if (p->altAgeMs < ALM_GPS_ALT_FRESH_MS && p->altM > -10000) {
      unitsFmtAlt((double)p->altM, c->units, false, a, sizeof(a));
      emitf(emit, ctx, ALM_ROW_INFO, "GPS alt %s", a);
    }
    if (p->motionAgeMs < ALM_GPS_MOTION_FRESH_MS && p->speedKnX100 >= 0) {
      if (p->speedKnX100 > ALM_MOVING_KNX100) {
        const double mps = p->speedKnX100 * (1852.0 / 3600.0) / 100.0;
        unitsFmtSpeed(mps, c->units, a, sizeof(a));
        if (p->courseX10 >= 0) {
          const double deg = p->courseX10 / 10.0;
          emitf(emit, ctx, ALM_ROW_INFO, "Speed %s, %d %s", a, ((int)floor(deg + 0.5)) % 360,
                almCompass16(deg));
        } else {
          emitf(emit, ctx, ALM_ROW_INFO, "Speed %s", a);
        }
      } else {
        emit(ctx, ALM_ROW_INFO, "Stationary");
      }
    }
    if (p->sats >= 0 && p->hdopX10 >= 0) {
      emitf(emit, ctx, ALM_ROW_INFO, "Sats %d, HDOP %d.%d", p->sats, p->hdopX10 / 10,
            p->hdopX10 % 10);
    } else if (p->sats >= 0) {
      emitf(emit, ctx, ALM_ROW_INFO, "Sats %d", p->sats);
    }
  }

  if (c->placeKind == ALM_PLACE_NONE) {
    return;
  }
  if (!c->clockKnown) {
    emit(ctx, ALM_ROW_INFO, "Declination: needs the clock");
    return;
  }
  // Magnetic declination at the place, now. Height: the ground's if read, else the GPS's.
  double altKm = 0.0;
  if (p->elevState == ALM_ELEV_OK) {
    altKm = p->elevM / 1000.0;
  } else if (p->gpsHaveFix && p->altAgeMs < ALM_GPS_ALT_FRESH_MS && p->altM > -10000) {
    altKm = p->altM / 1000.0;
  }
  WmmField f;
  if (wmmCompute(c->lat, c->lon, altKm, wmmDecimalYear(c->now), &f)) {
    const double dd = f.decl;
    const double mag = fabs(dd);
    if (mag < 0.05) {
      emit(ctx, ALM_ROW_INFO, "Declination 0.0");
      emit(ctx, ALM_ROW_INFO, "true = magnetic");
    } else {
      emitf(emit, ctx, ALM_ROW_INFO, "Declination %.1f %c", mag, dd > 0 ? 'E' : 'W');
      emitf(emit, ctx, ALM_ROW_INFO, "true = magnetic %c %.1f", dd > 0 ? '+' : '-', mag);
    }
    emitf(emit, ctx, ALM_ROW_INFO, "%s, valid to %d", wmmModelName(), (int)WMM_VALID_TO);
  } else {
    emit(ctx, ALM_ROW_INFO, "Declination: model expired");
    emitf(emit, ctx, ALM_ROW_INFO, "%s covers %d-%d", wmmModelName(), (int)WMM_VALID_FROM,
          (int)WMM_VALID_TO);
  }
}

void almLinesDate(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  if (!c->clockKnown) {
    preamble(c, false, emit, ctx);
    return;
  }
  int y, m, d, wd, yd;
  if (!almDateOf(c->now, c->tzS, &y, &m, &d, &wd, &yd)) {
    emitf(emit, ctx, ALM_ROW_INFO, "Date out of range (%d-%d)", ALM_YEAR_MIN, ALM_YEAR_MAX);
    return;
  }
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  emitf(emit, ctx, ALM_ROW_INFO, "%s, %s %d", WDAY[wd % 7], MONTH[(m - 1) % 12], d);
  emitf(emit, ctx, ALM_ROW_INFO, "%d, day %d of %d, week %d", y, yd, leap ? 366 : 365,
        astroIsoWeek(y, m, d));
  char t[8], off[16];
  almFmtClock(c->now, c->tzS, ALM_ROUND_DOWN, t, sizeof(t));
  almFmtUtcOffset(c->tzS, off, sizeof(off));
  emitf(emit, ctx, ALM_ROW_INFO, "Time %s %s (%s)", t, off, c->clockSrc ? c->clockSrc : "?");
  meshWarning(c, emit, ctx);
  char dst[96];
  if (c->tzZoned ? almZoneDstReminder(c, dst, sizeof(dst))
                 : almDstReminder(c->now, c->tzS, c->usDst, dst, sizeof(dst))) {
    emit(ctx, ALM_ROW_WRAP, dst);
  }
  if (c->seasons) {
    int best = -1;
    for (int k = 0; k < 4; k++) {
      if (c->seasons[k] && (best < 0 || c->seasons[k] < c->seasons[best])) {
        best = k;
      }
    }
    if (best >= 0) {
      const int64_t at = c->seasons[best];
      const int stz = almCtxTzAt(c, at);                        // its date on the clock THEN
      int sy, sm, sd;
      almDateOf(at, stz, &sy, &sm, &sd, NULL, NULL);
      const int64_t days = astroDaysFromCivil(sy, sm, sd) - astroDaysFromCivil(y, m, d);
      char date[24];
      almFmtDate(at, stz, date, sizeof(date));
      emitf(emit, ctx, ALM_ROW_INFO, "Next: %s", SEASON[best]);
      if (days == 0) {
        emitf(emit, ctx, ALM_ROW_INFO, "%s, today", date);
      } else if (days == 1) {
        emitf(emit, ctx, ALM_ROW_INFO, "%s, tomorrow", date);
      } else {
        emitf(emit, ctx, ALM_ROW_INFO, "%s, in %d days", date, (int)days);
      }
    }
  }
}

void almLinesSettings(const AlmCtx* c, AlmEmitFn emit, void* ctx) {
  if (c->rule == ASTRO_LEGAL_CIVIL) {
    emit(ctx, ALM_SET_LEGAL, "Legal light: civil twilight");
    emit(ctx, ALM_ROW_WRAP, "From the sun 6 deg below the horizon in the morning to the same in "
                            "the evening.");
  } else {
    emit(ctx, ALM_SET_LEGAL, "Legal light: 30 min rule");
    emit(ctx, ALM_ROW_WRAP, "From 30 min before sunrise to 30 min after sunset (Washington's "
                            "big-game rule).");
  }
  emit(ctx, ALM_SET_UNITS, unitsSettingRow(c->units));   // the Maps menu's row, word for word
  emit(ctx, ALM_ROW_WRAP, "The map uses these units too.");
  if (c->tzZoned) {
    /* The zone's rule decides (Settings > Time zone): the phone changes its own clock, and a day
     * across a change is in that day's time. OK still flips the switch below, which is what a
     * TYPED offset (Custom, or Auto before a zone is found) goes by - the row says which it is. */
    emit(ctx, ALM_SET_DST, c->tzRule == TZ_RULE_USCAN ? "Daylight saving: automatic"
                                                      : "Daylight saving: none here");
    char why[128];
    snprintf(why, sizeof(why), "Set by the time zone (%s) in Settings. With a typed offset the US "
             "reminder is %s.", c->tzName ? c->tzName : "a named zone", c->usDst ? "on" : "off");
    emit(ctx, ALM_ROW_WRAP, why);
  } else {
    emit(ctx, ALM_SET_DST, c->usDst ? "US daylight saving: yes" : "US daylight saving: no (HI, AZ)");
    emit(ctx, ALM_ROW_WRAP, c->usDst
         ? "Reminds you before US clocks change, and gives a day across a change in that day's "
           "clock time. No for Hawaii and Arizona."
         : "No reminder, and every day in today's offset (Hawaii and Arizona do not change).");
  }
  emit(ctx, ALM_ROW_WRAP, "OK changes a setting; each is saved.");
}
