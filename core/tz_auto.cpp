/*
 * tz_auto.cpp - see tz_auto.h. Proven by tests/test_tzauto.cpp on the host.
 */
#include "tz_auto.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- the setting

/* The ten zones Settings offers by name. Order is Settings' order; NVS keeps the KEY, so the
 * order can change, a key cannot. Each one's offset and rule are its IANA zone's from 2026 on
 * (test_tzauto checks them against the grid's table, which tools/gen_tz_grid.py verified
 * against tzdata hour by hour). Pacific is the US one (Los Angeles): since tzdata 2026d British
 * Columbia keeps UTC-7 all year, which is Arizona's clock. */
static const TzNamed NAMED[TZ_NAMED_COUNT] = {
  { "pacific",      "Pacific",      "America/Los_Angeles", -480, TZ_RULE_USCAN },
  { "mountain",     "Mountain",     "America/Denver",      -420, TZ_RULE_USCAN },
  { "arizona",      "Arizona",      "America/Phoenix",     -420, TZ_RULE_NONE },
  { "central",      "Central",      "America/Chicago",     -360, TZ_RULE_USCAN },
  { "eastern",      "Eastern",      "America/New_York",    -300, TZ_RULE_USCAN },
  { "atlantic",     "Atlantic",     "America/Halifax",     -240, TZ_RULE_USCAN },
  { "newfoundland", "Newfoundland", "America/St_Johns",    -210, TZ_RULE_USCAN },
  { "alaska",       "Alaska",       "America/Anchorage",   -540, TZ_RULE_USCAN },
  { "hawaii",       "Hawaii",       "Pacific/Honolulu",    -600, TZ_RULE_NONE },
  { "saskatchewan", "Saskatchewan", "America/Regina",      -360, TZ_RULE_NONE },
};

const TzNamed* tzNamed(int i) {
  return (i >= 0 && i < TZ_NAMED_COUNT) ? &NAMED[i] : NULL;
}

static bool sameNoCase(const char* a, const char* b) {
  for (;; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
    if (!x) return true;
  }
}

int tzNamedFind(const char* name) {
  if (!name || !*name) {
    return -1;
  }
  for (int i = 0; i < TZ_NAMED_COUNT; i++) {
    if (sameNoCase(name, NAMED[i].key) || sameNoCase(name, NAMED[i].display) ||
        sameNoCase(name, NAMED[i].iana)) {
      return i;
    }
  }
  return -1;
}

bool tzCustomOk(int minutes) {
  return minutes >= TZ_CUSTOM_MIN_MINUTES && minutes <= TZ_CUSTOM_MAX_MINUTES;
}

int tzHoursToMinutes(float hours) {
  const double m = (double)hours * 60.0;
  if (m > 1e6 || m < -1e6 || m != m) {
    return 1000000;                       // out of every range: tzCustomOk refuses it
  }
  return (int)(m >= 0 ? m + 0.5 : m - 0.5);
}

int tzChoiceOf(uint8_t mode, int namedIdx) {
  if (mode == TZ_MODE_NAMED && tzNamed(namedIdx)) {
    return 1 + namedIdx;
  }
  return mode == TZ_MODE_CUSTOM ? TZ_CHOICE_CUSTOM : TZ_CHOICE_AUTO;
}

void tzChoiceTo(int choice, uint8_t* mode, int* namedIdx) {
  uint8_t m = TZ_MODE_AUTO;
  int n = -1;
  if (choice >= 1 && choice <= TZ_NAMED_COUNT) {
    m = TZ_MODE_NAMED;
    n = choice - 1;
  } else if (choice == TZ_CHOICE_CUSTOM) {
    m = TZ_MODE_CUSTOM;
  }
  if (mode) *mode = m;
  if (namedIdx) *namedIdx = n;
}

const char* tzChoiceName(int choice) {
  if (choice == TZ_CHOICE_AUTO) return "Auto";
  if (choice == TZ_CHOICE_CUSTOM) return "Custom";
  const TzNamed* z = tzNamed(choice - 1);
  return z ? z->display : "?";
}

// ---------------------------------------------------------------- the Auto decision

const char* tzaWhyName(int why) {
  switch (why) {
    case TZA_WHY_NONE:         return "not checked yet";
    case TZA_WHY_MANUAL:       return "manual";
    case TZA_WHY_NO_FIX:       return "no fix";
    case TZA_WHY_OLD:          return "fix too old";
    case TZA_WHY_POOR:         return "poor fix";
    case TZA_WHY_BENCH:        return "bench fix";
    case TZA_WHY_BORDER:       return "border";
    case TZA_WHY_WATER:        return "at sea";
    case TZA_WHY_OUTSIDE:      return "outside the grid";
    case TZA_WHY_UNKNOWN_RULE: return "rules not covered";
    case TZA_WHY_BAD_INPUT:    return "bad position";
    case TZA_WHY_PHONE_NEAR:   return "phone fix near a line";
    case TZA_WHY_SAME:         return "confirmed";
    case TZA_WHY_WAITING:      return "waiting";
    case TZA_WHY_SWITCHED:     return "switched";
  }
  return "?";
}

void tzaInit(TzAuto* a, int zone, int zoneSrc) {
  if (!a) {
    return;
  }
  memset(a, 0, sizeof(*a));
  a->zone = (int16_t)(tzGridZone(zone) ? zone : -1);
  a->zoneSrc = (uint8_t)(a->zone >= 0 && (zoneSrc == SELF_GPS || zoneSrc == SELF_PHONE ||
                                          zoneSrc == TZA_SRC_HAND) ? zoneSrc : SELF_NONE);
  a->cand = -1;
  a->why = TZA_WHY_NONE;
  a->lastLookup = TZA_LOOKUP_NONE;
  a->lastSrc = SELF_NONE;
}

void tzaForget(TzAuto* a) {
  if (!a) {
    return;
  }
  a->zone = -1;
  a->zoneSrc = SELF_NONE;
  a->cand = -1;
}

static bool ruleModelled(uint8_t rule) {
  return rule == TZ_RULE_NONE || rule == TZ_RULE_USCAN;
}

int tzaZoneFromSaved(const char* iana) {
  if (!iana || !*iana) {
    return -1;
  }
  const int z = tzGridFindZone(iana);
  const TzZone* zz = tzGridZone(z);
  return (zz && ruleModelled(zz->rule)) ? z : -1;
}

void tzaEnterAuto(TzAuto* a, uint8_t fromMode, int fromNamed) {
  if (!a) {
    return;
  }
  tzaForget(a);                           // never the zone found before the override
  /* The named zone's own grid zone: the same offset and rule at every instant (test_tzauto holds
   * each named zone to its grid zone, sinceUtc 0), so the clock does not move. */
  const TzNamed* nz = (fromMode == TZ_MODE_NAMED) ? tzNamed(fromNamed) : NULL;
  const int z = nz ? tzaZoneFromSaved(nz->iana) : -1;
  if (z >= 0) {
    a->zone = (int16_t)z;
    a->zoneSrc = TZA_SRC_HAND;
  }
}

/* How telling a refusal is, so the line under the setting names the one that matters: a bench
 * fix that was refused says so rather than "no fix"; a poor fix rather than an old one. */
static int refusalRank(uint8_t w) {
  switch (w) {
    case TZA_WHY_BENCH: return 3;
    case TZA_WHY_POOR:  return 2;
    case TZA_WHY_OLD:   return 1;
  }
  return 0;
}

int tzaPickFix(const SelfSlot slots[SELF_SRC_COUNT], uint32_t nowMs, SelfFix* out, uint8_t* why) {
  SelfFix tmp;
  if (!out) {
    out = &tmp;
  }
  memset(out, 0, sizeof(*out));
  out->src = SELF_NONE;
  out->sats = out->hdopX10 = -1;
  uint8_t worst = TZA_WHY_NO_FIX;
  if (slots) {
    /* 🛑 GPS AND PHONE ONLY - THE LOOP'S BOUND IS THE RULE. Since 0.9.82's Wi-Fi location the
     * slots hold a third source (SELF_NET) that selfSource() answers for like the others; a
     * Wi-Fi fix never chooses the clock's zone (a network guess near a zone line is an hour wrong
     * all day, and it is not a measurement - the owner's rule). test_tzauto proves a fully live,
     * sendable NET slot is still refused. */
    for (int src = SELF_GPS; src <= SELF_PHONE; src++) {     // rank order IS src order
      SelfFix fx;
      /* SELF_PICK_TX: the 30 s bar, and no SELF_PICK_INJECTED - a bench fix is SELF_ST_INJECTED.
       * selfSource returns false for a source that is off or has never fixed. */
      if (!selfSource(slots, src, nowMs, SELF_PICK_TX, &fx)) {
        continue;
      }
      uint8_t w = TZA_WHY_NO_FIX;
      if ((fx.state == SELF_ST_LIVE || fx.state == SELF_ST_UNRATED) && !fx.injected) {
        *out = fx;
        if (why) *why = TZA_WHY_NONE;
        return src;
      }
      switch (fx.state) {
        case SELF_ST_INJECTED: w = TZA_WHY_BENCH; break;
        case SELF_ST_POOR:     w = TZA_WHY_POOR; break;
        case SELF_ST_OLD:      w = TZA_WHY_OLD; break;
        default:               w = fx.injected ? TZA_WHY_BENCH : TZA_WHY_NO_FIX; break;
      }
      if (refusalRank(w) > refusalRank(worst)) {
        worst = w;
      }
    }
  }
  if (why) *why = worst;
  return SELF_NONE;
}

static int64_t floorDiv64(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}

int tzGridLookupMargin(int32_t latE7, int32_t lonE7, int32_t marginM, uint8_t* why) {
  uint8_t w = TZ_WHY_BAD_INPUT;
  const int centre = tzGridLookup(latE7, lonE7, &w);
  if (centre < 0 || marginM <= 0) {
    if (why) *why = w;
    return centre;
  }
  /* The box that holds the circle: half-height marginM of latitude (110.54 km a degree, the
   * shortest a degree of latitude gets, so the box is never short), half-width marginM at the
   * box's poleward edge, where a degree of longitude is shortest. Every grid cell the box touches
   * is looked up at its centre: a change of clock anywhere in the box puts a TZ_BORDER cell in
   * it (the grid marks the cells beside every change), so walking the cells is exact at the
   * grid's own resolution - no line can slip between two samples. */
  const double PI = 3.14159265358979323846;
  const double dLat = marginM / 110540.0;
  const double poleward = fabs(latE7 * 1e-7) + dLat;
  if (poleward >= 85.0) {
    if (why) *why = TZ_WHY_BORDER;
    return TZ_BORDER;                     // the grid stops at 72 N: nothing up here is answered
  }
  const double dLon = marginM / (111320.0 * cos(poleward * PI / 180.0));
  const TzGridInfo* g = tzGridInfo();
  const int64_t res = g->resE7;
  const int64_t laLo = (int64_t)latE7 - (int64_t)(dLat * 1e7) - 1;
  const int64_t laHi = (int64_t)latE7 + (int64_t)(dLat * 1e7) + 1;
  const int64_t loLo = (int64_t)lonE7 - (int64_t)(dLon * 1e7) - 1;
  const int64_t loHi = (int64_t)lonE7 + (int64_t)(dLon * 1e7) + 1;
  // Cell indices on an unbounded axis aligned with the grid (360 deg is a whole number of cells).
  const int64_t r0 = floorDiv64(laLo - g->lat0E7, res), r1 = floorDiv64(laHi - g->lat0E7, res);
  const int64_t c0 = floorDiv64(loLo - g->lon0E7, res), c1 = floorDiv64(loHi - g->lon0E7, res);
  const int64_t E7_180 = 1800000000LL, E7_360 = 3600000000LL;
  for (int64_t r = r0; r <= r1; r++) {
    const int64_t la = g->lat0E7 + r * res + res / 2;
    if (la < -900000000LL || la > 900000000LL) {
      if (why) *why = TZ_WHY_BORDER;
      return TZ_BORDER;
    }
    for (int64_t c = c0; c <= c1; c++) {
      int64_t lo = g->lon0E7 + c * res + res / 2;
      while (lo > E7_180) lo -= E7_360;
      while (lo < -E7_180) lo += E7_360;
      const int z = tzGridLookup((int32_t)la, (int32_t)lo, NULL);
      if (z < 0 || (z != centre && !tzGridSameClock(z, centre))) {
        if (why) *why = TZ_WHY_BORDER;
        return TZ_BORDER;
      }
    }
  }
  if (why) *why = TZ_WHY_ZONE;
  return centre;
}

static uint8_t whyFromGrid(uint8_t gw) {
  switch (gw) {
    case TZ_WHY_BORDER:       return TZA_WHY_BORDER;
    case TZ_WHY_WATER:        return TZA_WHY_WATER;
    case TZ_WHY_OUTSIDE:      return TZA_WHY_OUTSIDE;
    case TZ_WHY_UNKNOWN_RULE: return TZA_WHY_UNKNOWN_RULE;
  }
  return TZA_WHY_BAD_INPUT;
}

int tzaLookup(const SelfFix* fx, uint8_t* why) {
  if (!fx || fx->src == SELF_NONE) {
    if (why) *why = TZA_WHY_NO_FIX;
    return TZ_NOT_COVERED;
  }
  uint8_t gw = TZ_WHY_BAD_INPUT;
  const int z = tzGridLookup(fx->latI, fx->lonI, &gw);
  if (z < 0) {
    if (why) *why = whyFromGrid(gw);
    return z;
  }
  /* Every PHONE fix, not only an UNRATED one: a rated phone fix can be kilometres off too (GPS 2
   * IP's stand-in numbers off its default port, gpsdRelay's generation glitch - tz_auto.h rule
   * 3). The woods plate's GPS is the only source the plain one-cell band is sized for. */
  if (fx->src == SELF_PHONE &&
      tzGridLookupMargin(fx->latI, fx->lonI, TZA_PHONE_MARGIN_M, NULL) < 0) {
    if (why) *why = TZA_WHY_PHONE_NEAR;
    return TZ_BORDER;
  }
  if (why) *why = TZA_WHY_NONE;
  return z;
}

static void expireCand(TzAuto* a, uint32_t nowMs) {
  if (a->cand >= 0 && (uint32_t)(nowMs - a->candLastMs) > TZA_CAND_GAP_MS) {
    a->cand = -1;
  }
}

bool tzaDecide(TzAuto* a, int zone, uint8_t why, int src, uint32_t nowMs) {
  if (!a) {
    return false;
  }
  a->whyMs = nowMs ? nowMs : 1;
  expireCand(a, nowMs);
  if (zone < 0 || !tzGridZone(zone) || !ruleModelled(tzGridZone(zone)->rule)) {
    /* A refusal is an answer, and it is not the waiting zone: a border, the sea or the edge of
     * the grid between two sightings starts the count again. */
    a->why = zone < 0 ? why : TZA_WHY_UNKNOWN_RULE;
    a->cand = -1;
    return false;
  }
  if (a->zone >= 0 && (zone == a->zone || tzGridSameClock(zone, a->zone))) {
    a->why = TZA_WHY_SAME;
    a->cand = -1;
    return false;
  }
  if (a->cand >= 0 && (zone == a->cand || tzGridSameClock(zone, a->cand))) {
    a->cand = (int16_t)zone;             // the newest name for the same clock
    a->candSrc = (uint8_t)src;
    a->candLastMs = nowMs;
    if ((uint32_t)(nowMs - a->candFirstMs) >= TZA_CONFIRM_MS) {
      a->zone = (int16_t)zone;
      a->zoneSrc = (uint8_t)src;
      a->cand = -1;
      a->why = TZA_WHY_SWITCHED;
      a->switches++;
      return true;
    }
    a->why = TZA_WHY_WAITING;
    return false;
  }
  a->cand = (int16_t)zone;
  a->candSrc = (uint8_t)src;
  a->candFirstMs = a->candLastMs = nowMs;
  a->why = TZA_WHY_WAITING;
  return false;
}

bool tzaStep(TzAuto* a, uint8_t mode, const SelfSlot slots[SELF_SRC_COUNT], uint32_t nowMs) {
  if (!a) {
    return false;
  }
  a->checks++;
  a->whyMs = nowMs ? nowMs : 1;
  a->lastLookup = TZA_LOOKUP_NONE;
  a->lastSrc = SELF_NONE;
  a->lastUnrated = false;
  if (mode != TZ_MODE_AUTO) {
    a->why = TZA_WHY_MANUAL;
    a->cand = -1;
    return false;
  }
  SelfFix fx;
  uint8_t w = TZA_WHY_NO_FIX;
  const int src = tzaPickFix(slots, nowMs, &fx, &w);
  if (src == SELF_NONE) {
    /* No measurement is not an answer: a waiting zone keeps waiting (TZA_CAND_GAP_MS ends it),
     * and the zone in use stays in use - "change nothing". */
    expireCand(a, nowMs);
    a->why = w;
    return false;
  }
  a->lastSrc = (uint8_t)src;
  a->lastUnrated = fx.state == SELF_ST_UNRATED;
  uint8_t lw = TZA_WHY_NONE;
  const int z = tzaLookup(&fx, &lw);
  a->lastLookup = (int16_t)z;
  return tzaDecide(a, z, lw, src, nowMs);
}

// ---------------------------------------------------------------- the offset

void tzEffective(uint8_t mode, int namedIdx, int autoZone, int customMinutes, bool clockKnown,
                 int64_t utcNow, TzEff* out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  const TzNamed* nz = (mode == TZ_MODE_NAMED) ? tzNamed(namedIdx) : NULL;
  const TzZone* az = (mode == TZ_MODE_AUTO) ? tzGridZone(autoZone) : NULL;
  if (az && !ruleModelled(az->rule)) {
    az = NULL;
  }
  if (nz || az) {
    out->kind = nz ? TZK_NAMED : TZK_AUTO;
    out->stdMinutes = nz ? nz->stdMinutes : az->stdMinutes;
    out->rule = nz ? nz->rule : az->rule;
    out->name = nz ? nz->display : az->display;
    out->iana = nz ? nz->iana : az->iana;
    out->minutes = out->stdMinutes;
    const bool since = az && az->sinceUtc != 0;
    if (!clockKnown && (out->rule != TZ_RULE_NONE || since)) {
      return;                             // the date decides it, and the date is not known
    }
    int m;
    if (nz) {
      m = tzRuleOffsetMinutes(nz->stdMinutes, nz->rule, utcNow);
    } else if (clockKnown) {
      m = tzUtcOffsetMinutes(autoZone, utcNow);
    } else {
      m = az->stdMinutes;                 // no DST, no sinceUtc: the same at any instant
    }
    if (m == TZ_NO_OFFSET) {
      return;
    }
    out->ok = true;
    out->minutes = m;
    out->dst = m != out->stdMinutes;
    if (clockKnown) {
      out->next = nz ? tzRuleNextTransition(nz->stdMinutes, nz->rule, utcNow, &out->nextMinutes)
                     : tzNextTransition(autoZone, utcNow, &out->nextMinutes);
      if (!out->next) {
        out->nextMinutes = 0;
      }
    }
    return;
  }
  out->kind = (mode == TZ_MODE_AUTO) ? TZK_AUTO_WAIT : TZK_CUSTOM;
  out->rule = TZ_RULE_NONE;
  out->minutes = out->stdMinutes = customMinutes;
  out->ok = tzCustomOk(customMinutes);
}

// ---------------------------------------------------------------- the words

size_t tzFmtOffset(int minutes, char* out, size_t cap) {
  if (!out || !cap) {
    return 0;
  }
  int n;
  if (minutes == 0) {
    n = snprintf(out, cap, "UTC");
  } else {
    const int a = minutes < 0 ? -minutes : minutes;
    const char s = minutes < 0 ? '-' : '+';
    n = (a % 60) ? snprintf(out, cap, "UTC%c%d:%02d", s, a / 60, a % 60)
                 : snprintf(out, cap, "UTC%c%d", s, a / 60);
  }
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

const char* tzSrcWords(int selfSrc) {
  switch (selfSrc) {
    case SELF_GPS:   return "from GPS";
    case SELF_PHONE: return "from phone GPS";
    case TZA_SRC_HAND: return "set by hand";
  }
  return "";
}

static size_t finish(int n, char* out, size_t cap) {
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t tzStatusText(const TzEff* e, int zoneSrc, char* out, size_t cap) {
  if (!out || !cap) {
    return 0;
  }
  out[0] = '\0';
  if (!e) {
    return 0;
  }
  char off[16];
  tzFmtOffset(e->minutes, off, sizeof(off));
  switch (e->kind) {
    case TZK_CUSTOM:
      return finish(e->ok ? snprintf(out, cap, "%s custom", off)
                          : snprintf(out, cap, "Custom offset not valid"), out, cap);
    case TZK_AUTO_WAIT:
      return finish(e->ok ? snprintf(out, cap, "No zone yet: %s (typed)", off)
                          : snprintf(out, cap, "No zone yet"), out, cap);
    case TZK_NAMED:
      return finish(e->ok ? snprintf(out, cap, "%s, %s, set by hand", e->name, off)
                          : snprintf(out, cap, "%s, clock not set", e->name), out, cap);
    case TZK_AUTO: {
      const char* w = tzSrcWords(zoneSrc);
      if (!e->ok) {
        return finish(snprintf(out, cap, "%s, clock not set", e->name), out, cap);
      }
      return finish(*w ? snprintf(out, cap, "%s, %s, %s", e->name, off, w)
                       : snprintf(out, cap, "%s, %s", e->name, off), out, cap);
    }
  }
  return 0;
}

size_t tzZoneText(const TzEff* e, char* out, size_t cap) {
  if (!out || !cap) {
    return 0;
  }
  out[0] = '\0';
  if (!e) {
    return 0;
  }
  char off[16];
  tzFmtOffset(e->minutes, off, sizeof(off));
  switch (e->kind) {
    case TZK_CUSTOM:
      return finish(e->ok ? snprintf(out, cap, "%s custom", off)
                          : snprintf(out, cap, "Custom offset not valid"), out, cap);
    case TZK_AUTO_WAIT:
      return finish(e->ok ? snprintf(out, cap, "No zone yet: %s", off)
                          : snprintf(out, cap, "No zone yet"), out, cap);
    case TZK_NAMED:
    case TZK_AUTO:
      return finish(e->ok ? snprintf(out, cap, "%s, %s", e->name, off)
                          : snprintf(out, cap, "%s: clock not set", e->name), out, cap);
  }
  return 0;
}

size_t tzSourceText(const TzEff* e, int zoneSrc, char* out, size_t cap) {
  if (!out || !cap) {
    return 0;
  }
  out[0] = '\0';
  if (!e) {
    return 0;
  }
  switch (e->kind) {
    case TZK_CUSTOM:    return finish(snprintf(out, cap, "Typed offset, no DST"), out, cap);
    case TZK_AUTO_WAIT: return finish(snprintf(out, cap, "Typed offset until a fix"), out, cap);
    case TZK_NAMED:     return finish(snprintf(out, cap, "Set by hand"), out, cap);
    case TZK_AUTO:
      return finish(zoneSrc == SELF_GPS       ? snprintf(out, cap, "From GPS")
                    : zoneSrc == SELF_PHONE   ? snprintf(out, cap, "From phone GPS")
                    : zoneSrc == TZA_SRC_HAND ? snprintf(out, cap, "Set by hand, Auto checks it")
                                              : snprintf(out, cap, "From a measured fix"), out, cap);
  }
  return 0;
}

size_t tzaWhyText(const TzAuto* a, char* out, size_t cap) {
  if (!out || !cap) {
    return 0;
  }
  out[0] = '\0';
  if (!a) {
    return 0;
  }
  const TzZone* cz = tzGridZone(a->cand);
  const TzZone* zz = tzGridZone(a->zone);
  int n;
  switch (a->why) {
    case TZA_WHY_NONE:         n = snprintf(out, cap, "Auto: not checked yet"); break;
    case TZA_WHY_MANUAL:       n = snprintf(out, cap, "Auto is off"); break;
    case TZA_WHY_NO_FIX:       n = snprintf(out, cap, "Auto: no fix from GPS or phone"); break;
    case TZA_WHY_OLD:          n = snprintf(out, cap, "Auto: the last fix is too old"); break;
    case TZA_WHY_POOR:         n = snprintf(out, cap, "Auto: poor fix, not used"); break;
    case TZA_WHY_BENCH:        n = snprintf(out, cap, "Auto: bench fix, not used"); break;
    case TZA_WHY_BORDER:       n = snprintf(out, cap, "Auto: near a time zone line"); break;
    case TZA_WHY_WATER:        n = snprintf(out, cap, "Auto: at sea, no zone"); break;
    case TZA_WHY_OUTSIDE:      n = snprintf(out, cap, "Auto: outside the zone map"); break;
    case TZA_WHY_UNKNOWN_RULE: n = snprintf(out, cap, "Auto: this zone is not covered"); break;
    case TZA_WHY_BAD_INPUT:    n = snprintf(out, cap, "Auto: no valid position"); break;
    case TZA_WHY_PHONE_NEAR:   n = snprintf(out, cap, "Auto: phone fix too near a line"); break;
    case TZA_WHY_SAME:
      n = snprintf(out, cap, "Auto: confirmed by %s", a->lastSrc == SELF_GPS ? "GPS" : "phone GPS");
      break;
    case TZA_WHY_WAITING:
      n = cz ? snprintf(out, cap, "Auto: checking %s", cz->display)
             : snprintf(out, cap, "Auto: checking");
      break;
    case TZA_WHY_SWITCHED:
      n = zz ? snprintf(out, cap, "Auto: changed to %s", zz->display)
             : snprintf(out, cap, "Auto: changed");
      break;
    default:                   n = snprintf(out, cap, "Auto: ?"); break;
  }
  return finish(n, out, cap);
}

static const char* const WDAY3[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char* const MON3[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// Howard Hinnant's civil_from_days (day 0 = 1970-01-01, a Thursday).
static void civil(int64_t days, int* m, int* d, int* wday) {
  int64_t z = days + 719468;
  const int64_t era = floorDiv64(z, 146097);
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *wday = (int)(days + 4 - floorDiv64(days + 4, 7) * 7);
}

size_t tzNextText(const TzEff* e, char* out, size_t cap) {
  if (!out || !cap) {
    return 0;
  }
  out[0] = '\0';
  if (!e) {
    return 0;
  }
  if (e->kind == TZK_CUSTOM) {
    return finish(snprintf(out, cap, "Custom: change it by hand for DST"), out, cap);
  }
  if (e->kind == TZK_AUTO_WAIT) {
    return finish(snprintf(out, cap, "No DST change until a zone is found"), out, cap);
  }
  if (e->rule == TZ_RULE_NONE) {
    return finish(snprintf(out, cap, "No DST: the offset never changes"), out, cap);
  }
  if (!e->ok || !e->next) {
    return 0;
  }
  /* The wall-clock date of the change: the instant in the offset BEFORE it (the clocks read
   * 02:00 on that date when it happens). */
  int m, d, wd;
  civil(floorDiv64(e->next + (int64_t)e->minutes * 60, 86400), &m, &d, &wd);
  char off[16];
  tzFmtOffset(e->nextMinutes, off, sizeof(off));
  return finish(snprintf(out, cap, "DST %s %s %s %d (%s)",
                         e->nextMinutes > e->minutes ? "starts" : "ends",
                         WDAY3[wd % 7], MON3[(m - 1) % 12], d, off), out, cap);
}

size_t tzChoiceHint(int choice, bool clockKnown, int64_t utcNow, char* out, size_t cap) {
  if (!out || !cap) {
    return 0;
  }
  out[0] = '\0';
  if (choice == TZ_CHOICE_AUTO) {
    return finish(snprintf(out, cap, "From GPS or the phone's GPS"), out, cap);
  }
  if (choice == TZ_CHOICE_CUSTOM) {
    return finish(snprintf(out, cap, "The offset below, no DST"), out, cap);
  }
  const TzNamed* z = tzNamed(choice - 1);
  if (!z) {
    return 0;
  }
  char a[16];
  if (z->rule == TZ_RULE_NONE) {
    tzFmtOffset(z->stdMinutes, a, sizeof(a));
    return finish(snprintf(out, cap, "%s all year, no DST", a), out, cap);
  }
  const int now = clockKnown ? tzRuleOffsetMinutes(z->stdMinutes, z->rule, utcNow) : TZ_NO_OFFSET;
  if (now != TZ_NO_OFFSET) {
    tzFmtOffset(now, a, sizeof(a));
    return finish(snprintf(out, cap, "%s now, DST by itself", a), out, cap);
  }
  char b[16];
  tzFmtOffset(z->stdMinutes, a, sizeof(a));
  tzFmtOffset(z->stdMinutes + 60, b, sizeof(b));
  return finish(snprintf(out, cap, "%s or %s, DST by itself", a, b + 3), out, cap);
}

void tzNoticeText(int zone, const TzEff* e, int zoneSrc, char* title, size_t titleCap, char* body,
                  size_t bodyCap) {
  if (title && titleCap) {
    const TzZone* z = tzGridZone(zone);
    finish(snprintf(title, titleCap, "Time zone: %s", z ? z->display : "?"), title, titleCap);
  }
  if (body && bodyCap) {
    if (e && e->ok) {
      char off[16];
      tzFmtOffset(e->minutes, off, sizeof(off));
      const char* w = tzSrcWords(zoneSrc);
      finish(*w ? snprintf(body, bodyCap, "Now %s, %s", off, w)
                : snprintf(body, bodyCap, "Now %s", off), body, bodyCap);
    } else {
      finish(snprintf(body, bodyCap, "Clock not set yet"), body, bodyCap);
    }
  }
}
