/*
 * self_pos.cpp — see self_pos.h. Proven by tests/test_selfpos.cpp on the host.
 */

#include "self_pos.h"
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- slots

void selfSlotSet(SelfSlot* s, int32_t latI, int32_t lonI, int sats, int hdopX10,
                 uint32_t nowMs, bool injected) {
  if (!s) {
    return;
  }
  s->latI = latI;
  s->lonI = lonI;
  s->sats = sats < 0 ? -1 : sats;
  s->hdopX10 = hdopX10 < 0 ? -1 : hdopX10;
  s->fixMs = nowMs ? nowMs : 1;             // millis() == 0 must not read as "never"
  s->injected = injected;
  s->accM = 0;                              // the NET fields belong to selfNetSet's fixes only
  s->firstMs = 0;
  s->scanUtc = 0;
}

void selfSlotClear(SelfSlot* s) {
  if (!s) {
    return;
  }
  s->latI = s->lonI = 0;
  s->fixMs = 0;
  s->sats = s->hdopX10 = -1;
  s->injected = false;
  s->accM = 0;
  s->firstMs = 0;
  s->scanUtc = 0;
}

// ---------------------------------------------------------------- the Wi-Fi slot

void selfNetSet(SelfSlot* s, int32_t latI, int32_t lonI, uint16_t accM, uint32_t scanMs,
                uint32_t scanUtc, bool injected) {
  if (!s || !s->enabled) {
    return;                                 // switched off while the lookup was out: not kept
  }
  selfSlotSet(s, latI, lonI, -1, -1, scanMs, injected);   // no satellites: a network guess
  s->accM = accM;
  s->firstMs = s->fixMs;                    // the scan it was computed from (0 read as 1)
  s->scanUtc = scanUtc;
}

bool selfNetConfirm(SelfSlot* s, uint32_t scanMs, uint32_t scanUtc) {
  if (!s || s->fixMs == 0) {
    return false;
  }
  const uint32_t ms = scanMs ? scanMs : 1;
  /* Signed: right across the millis() wrap. A scan from before the fix's own (an old snapshot
   * folded late) says nothing new; the same scan twice changes nothing. */
  if ((int32_t)(ms - s->fixMs) <= 0) {
    return false;
  }
  s->fixMs = ms;
  s->scanUtc = scanUtc;
  return true;
}

bool selfNetWantsRefresh(const SelfSlot* s, uint32_t nowMs) {
  if (!s || !s->enabled || s->fixMs == 0) {
    return false;
  }
  const uint32_t age = (uint32_t)(nowMs - s->fixMs);
  return age >= SELF_NET_REFRESH_AT_MS && age < SELF_NET_SHOW_MS;   // expired is gone: no rescue
}

bool selfNetScanUseful(const SelfSlot* s, uint32_t nowMs) {
  if (!s || s->fixMs == 0) {
    return false;
  }
  if ((uint32_t)(nowMs - s->firstMs) < SELF_NET_SETTLE_MS) {
    return false;                           // a scan now could not be the second, 60 s on
  }
  const bool settled = (uint32_t)(s->fixMs - s->firstMs) >= SELF_NET_SETTLE_MS;
  const bool recent = (uint32_t)(nowMs - s->fixMs) <= SELF_NET_CONFIRM_MS;
  return !(settled && recent);              // already sendable: a scan adds nothing
}

static bool netAccOk(uint16_t accM) {
  return accM >= 1 && accM <= SELF_NET_MAX_ACC_M;
}

/* The Wi-Fi slot's own states (self_pos.h, "THE WI-FI SLOT"): its clock is the scans, its
 * quality is the accuracy two agreeing lookups gave, and sending has three more bars. */
static int netSlotState(const SelfSlot* s, uint32_t nowMs, unsigned flags) {
  const bool tx = (flags & SELF_PICK_TX) != 0;
  if (tx && s->noTx) {
    return SELF_ST_MAP_ONLY;                // "map only" is a promise about the air, whatever its age
  }
  if ((uint32_t)(nowMs - s->fixMs) >= SELF_NET_SHOW_MS) {
    return SELF_ST_OLD;
  }
  if (!netAccOk(s->accM)) {
    return SELF_ST_POOR;
  }
  if (tx) {
    if ((uint32_t)(s->fixMs - s->firstMs) < SELF_NET_SETTLE_MS) {
      return SELF_ST_SETTLING;              // one scan, or two too close together: maybe moving
    }
    if ((uint32_t)(nowMs - s->fixMs) > SELF_NET_CONFIRM_MS) {
      return SELF_ST_UNCONFIRMED;           // still on the map, too old to tell anybody else
    }
  }
  return SELF_ST_LIVE;
}

int selfSlotState(const SelfSlot* s, int src, uint32_t nowMs, unsigned flags) {
  if (!s || (src != SELF_GPS && src != SELF_PHONE && src != SELF_NET)) {
    return SELF_ST_RESERVED;
  }
  if (!s->enabled) {
    return SELF_ST_OFF;
  }
  if (s->fixMs == 0) {
    return SELF_ST_NEVER;
  }
  if (s->injected && !(flags & SELF_PICK_INJECTED)) {
    return SELF_ST_INJECTED;
  }
  if (src == SELF_NET) {
    return netSlotState(s, nowMs, flags);
  }
  const uint32_t bar = (flags & SELF_PICK_TX) ? SELF_TX_FRESH_MS : SELF_SHOW_FRESH_MS;
  if ((uint32_t)(nowMs - s->fixMs) >= bar) {
    return SELF_ST_OLD;
  }
  /* The same bar for both measured sources (mesh_pos.h): unknown is not failure. For the
   * PHONE slot the gate only writes a fix with no sats AND no HDOP after an RMC of the same
   * second vouched for it — enough to SHOW it, not to send it (UNRATED, self_pos.h). */
  if (!meshPosFixUsable(s->sats, s->hdopX10)) {
    return SELF_ST_POOR;
  }
  if ((flags & SELF_PICK_TX) && src == SELF_PHONE && !s->injected && s->sats < 0 &&
      s->hdopX10 < 0) {
    return SELF_ST_UNRATED;
  }
  return SELF_ST_LIVE;
}

static void fillFix(SelfFix* out, const SelfSlot* s, int src, int state, uint32_t nowMs) {
  out->src = src;
  out->state = state;
  out->latI = s->latI;
  out->lonI = s->lonI;
  out->ageMs = (uint32_t)(nowMs - s->fixMs);
  out->sats = s->sats;
  out->hdopX10 = s->hdopX10;
  out->injected = s->injected;
  /* A bench fix is not a measurement, so it does not say it is one (self_pos.h, SelfFix). */
  out->locSrc = s->injected ? MESH_LOC_MANUAL : selfSrcLocSource(src);
  /* ...but a bench Wi-Fi fix stays a CELL: "coarse" is about how the position was made, and a
   * typed or canned Wi-Fi answer is no sharper than a real one. */
  out->precBits = (s->injected && src != SELF_NET) ? 0 : selfSrcPrecBits(src);
  out->accM = (src == SELF_NET) ? s->accM : 0;
  out->scanUtc = (src == SELF_NET) ? s->scanUtc : 0;
}

static void noFix(SelfFix* out) {
  if (out) {
    memset(out, 0, sizeof(*out));
    out->src = SELF_NONE;
    out->sats = out->hdopX10 = -1;
  }
}

int selfPick(const SelfSlot slots[SELF_SRC_COUNT], uint32_t nowMs, unsigned flags, SelfFix* out) {
  noFix(out);
  if (!slots) {
    return SELF_NONE;
  }
  for (int src = SELF_GPS; src <= SELF_NET; src++) {     // rank order IS src order
    const SelfSlot* s = &slots[src - 1];
    const int st = selfSlotState(s, src, nowMs, flags);
    if (st == SELF_ST_LIVE) {
      if (out) {
        fillFix(out, s, src, st, nowMs);
      }
      return src;
    }
  }
  return SELF_NONE;
}

int selfNewest(const SelfSlot slots[SELF_SRC_COUNT], uint32_t nowMs, unsigned flags, SelfFix* out) {
  noFix(out);
  if (!slots) {
    return SELF_NONE;
  }
  int best = SELF_NONE;
  uint32_t bestAge = 0;
  for (int src = SELF_GPS; src <= SELF_NET; src++) {
    const SelfSlot* s = &slots[src - 1];
    const int st = selfSlotState(s, src, nowMs, flags);
    if (st == SELF_ST_RESERVED || st == SELF_ST_OFF || st == SELF_ST_NEVER ||
        st == SELF_ST_INJECTED) {
      continue;
    }
    /* An expired Wi-Fi fix is GONE (self_pos.h): there is no "last Wi-Fi" to draw grey. Asked
     * on the show bar, so a sending question cannot resurrect one either. */
    if (src == SELF_NET && selfSlotState(s, src, nowMs, flags & SELF_PICK_INJECTED) == SELF_ST_OLD) {
      continue;
    }
    const uint32_t age = (uint32_t)(nowMs - s->fixMs);
    if (best == SELF_NONE || age < bestAge) {            // a tie keeps the better rank
      best = src;
      bestAge = age;
      if (out) {
        fillFix(out, s, src, st, nowMs);
      }
    }
  }
  return best;
}

bool selfSource(const SelfSlot slots[SELF_SRC_COUNT], int src, uint32_t nowMs, unsigned flags,
                SelfFix* out) {
  noFix(out);
  if (!slots || (src != SELF_GPS && src != SELF_PHONE && src != SELF_NET)) {
    return false;
  }
  const SelfSlot* s = &slots[src - 1];
  const int st = selfSlotState(s, src, nowMs, flags);
  if (st == SELF_ST_OFF || st == SELF_ST_NEVER || st == SELF_ST_RESERVED) {
    return false;
  }
  if (out) {
    fillFix(out, s, src, st, nowMs);
  }
  return true;
}

bool selfAnyEnabled(const SelfSlot slots[SELF_SRC_COUNT]) {
  if (!slots) {
    return false;
  }
  return slots[SELF_GPS - 1].enabled || slots[SELF_PHONE - 1].enabled ||
         slots[SELF_NET - 1].enabled;
}

bool selfAnyEnabledForTx(const SelfSlot slots[SELF_SRC_COUNT]) {
  if (!slots) {
    return false;
  }
  const SelfSlot* net = &slots[SELF_NET - 1];
  return slots[SELF_GPS - 1].enabled || slots[SELF_PHONE - 1].enabled ||
         (net->enabled && !net->noTx);
}

// ---------------------------------------------------------------- labels

const char* selfSrcName(int src) {
  switch (src) {
    case SELF_GPS:   return "GPS";
    case SELF_PHONE: return "phone";
    case SELF_NET:   return "Wi-Fi";
    default:         return "none";
  }
}

const char* selfSrcLastName(int src) {
  switch (src) {
    case SELF_GPS:   return "last GPS";
    case SELF_PHONE: return "last phone";
    case SELF_NET:   return "last Wi-Fi";
    default:         return "none";
  }
}

const char* selfMapLabel(int src) {
  switch (src) {
    case SELF_PHONE: return "me (phone)";
    case SELF_NET:   return "me (Wi-Fi)";
    default:         return "me";
  }
}

const char* selfBeaconTag(int src) {
  switch (src) {
    case SELF_GPS:   return "gps beacon";
    case SELF_PHONE: return "phone beacon";
    case SELF_NET:   return "wifi-loc beacon";
    default:         return "beacon";
  }
}

const char* selfStateName(int state) {
  switch (state) {
    case SELF_ST_LIVE:     return "live";
    case SELF_ST_RESERVED: return "not a source";
    case SELF_ST_OFF:      return "off";
    case SELF_ST_NEVER:    return "no fix yet";
    case SELF_ST_INJECTED: return "bench fix, not sent";
    case SELF_ST_OLD:      return "too old";
    case SELF_ST_POOR:     return "poor fix";
    case SELF_ST_UNRATED:  return "no sats/HDOP given, not sent";
    case SELF_ST_MAP_ONLY: return "map only, not sent";
    case SELF_ST_SETTLING: return "waiting for a second scan";
    case SELF_ST_UNCONFIRMED: return "needs a fresh scan to send";
    default:               return "?";
  }
}

uint8_t selfSrcLocSource(int src) {
  /* A claim of a MEASUREMENT — and stock receivers take the packet's time on the strength of
   * it (mesh_pos.h). Only the two GNSS sources ever make it. */
  switch (src) {
    case SELF_GPS:   return MESH_LOC_INTERNAL;
    case SELF_PHONE: return MESH_LOC_EXTERNAL;
    default:         return MESH_LOC_UNSET;
  }
}

uint8_t selfSrcPrecBits(int src) {
  if (src == SELF_NET) {
    return SELF_NET_PREC_BITS;
  }
  return (src == SELF_GPS || src == SELF_PHONE) ? MESH_POS_PREC_FULL : 0;
}

static size_t cutLen(int n, size_t cap, char* out) {
  if (n < 0) {
    out[0] = 0;
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t selfFixLabel(char* out, size_t cap, const SelfFix* fx) {
  if (!out || cap == 0) {
    return 0;
  }
  const int src = fx ? fx->src : SELF_NONE;
  /* "+/-", not the plus-minus sign: the phone's fonts (Akrobat, src/assets/fonts.h) carry
   * printable ASCII only, and a missing glyph draws as a hollow box. */
  if (src == SELF_NET) {
    return cutLen(snprintf(out, cap, "me +/-%u m Wi-Fi", (unsigned)fx->accM), cap, out);
  }
  return cutLen(snprintf(out, cap, "%s", selfMapLabel(src)), cap, out);
}

size_t selfFixName(char* out, size_t cap, const SelfFix* fx) {
  if (!out || cap == 0) {
    return 0;
  }
  const int src = fx ? fx->src : SELF_NONE;
  if (src == SELF_NET) {
    return cutLen(snprintf(out, cap, "Wi-Fi +/-%u m", (unsigned)fx->accM), cap, out);
  }
  return cutLen(snprintf(out, cap, "%s", selfSrcName(src)), cap, out);
}

uint32_t selfFixSendTime(const SelfFix* fx, uint32_t trustedUtcNow) {
  if (!fx) {
    return 0;
  }
  return fx->src == SELF_NET ? fx->scanUtc : trustedUtcNow;
}

// ---------------------------------------------------------------- sending a Wi-Fi fix

void selfNetTxInit(SelfNetTxMem* m) {
  if (m) {
    memset(m, 0, sizeof(*m));
  }
}

void selfNetTxNotePrecise(SelfNetTxMem* m, int32_t latI, int32_t lonI, uint32_t nowMs) {
  if (!m) {
    return;
  }
  m->preciseOk = true;
  m->preciseMs = nowMs;
  m->preciseLatI = latI;
  m->preciseLonI = lonI;
}

void selfNetTxNoteSent(SelfNetTxMem* m, const SelfFix* fx, int32_t cellLatI, int32_t cellLonI) {
  if (!m || !fx || fx->src != SELF_NET) {
    return;
  }
  m->cellOk = true;
  m->cellLatI = cellLatI;
  m->cellLonI = cellLonI;
  m->srcLatI = fx->latI;
  m->srcLonI = fx->lonI;
  m->srcAccM = fx->accM;
}

bool selfNetPreciseWins(const SelfNetTxMem* m, const SelfFix* fx, uint32_t nowMs) {
  if (!m || !fx || fx->src != SELF_NET || !m->preciseOk) {
    return false;
  }
  if ((uint32_t)(nowMs - m->preciseMs) >= SELF_NET_PRECISE_WINS_MS) {
    return false;
  }
  const double reach = (double)fx->accM + SELF_NET_PRECISE_SLACK_M;
  return meshPosDistanceM(m->preciseLatI, m->preciseLonI, fx->latI, fx->lonI) <= reach;
}

void selfNetTxCell(const SelfNetTxMem* m, const SelfFix* fx, int32_t* latI, int32_t* lonI) {
  if (!fx || !latI || !lonI) {
    return;
  }
  if (m && m->cellOk) {
    const double reach = (double)(fx->accM > m->srcAccM ? fx->accM : m->srcAccM);
    if (meshPosDistanceM(m->srcLatI, m->srcLonI, fx->latI, fx->lonI) <= reach) {
      *latI = m->cellLatI;                  // the same place, give or take its own error
      *lonI = m->cellLonI;
      return;
    }
  }
  *latI = meshPosTruncate(fx->latI, SELF_NET_PREC_BITS);
  *lonI = meshPosTruncate(fx->lonI, SELF_NET_PREC_BITS);
}

// ---------------------------------------------------------------- the woods plate's GPS (UART)

void selfGpsInit(SelfGpsRx* rx) {
  if (!rx) {
    return;
  }
  memset(rx, 0, sizeof(*rx));
  rx->sats = rx->hdopX10 = -1;
  rx->rmcHms = rx->ggaHms = -1;
  rx->fixSats = rx->fixHdopX10 = -1;
  rx->altM = -10000;
  rx->speedKnX100 = rx->courseX10 = -1;
}

/* The gga* fields' "present but unreadable" (-2) is no number either: for a rating, -1. */
static int ggaWord(int32_t v) {
  return v >= 0 ? (int)v : -1;
}

/* Is THIS sentence a measured fix (self_pos.h)? Each test reads the sentence's own fields only —
 * never the reader's sticky latI/sats/valid as they stood before it. The same-second veto is the
 * caller's. */
static bool rmcSaysFix(const NmeaFix& f) {
  /* `valid` after an RMC is THAT RMC's: status A with coordinates that parsed (nmea.cpp), and
   * then latI/lonI are its own. rmcStatus is checked as well, so the rule reads whole here. */
  return f.rmcStatus == 'A' && f.valid && selfRmcModeOk(f.rmcMode);
}

static bool ggaSaysFix(const NmeaFix& f) {
  return f.ggaQuality >= 1 && f.ggaQuality <= 5 && f.ggaHasPos;   // 6 DR, 7 manual, 8 sim: not measured
}

static bool sameSecond(int32_t a, int32_t b) {
  return a >= 0 && b >= 0 && a == b;
}

unsigned selfGpsApply(SelfGpsRx* rx, SelfSlot* slot, const NmeaFix& f, uint32_t nowMs) {
  if (!rx || (f.lastKind != NMEA_LAST_RMC && f.lastKind != NMEA_LAST_GGA)) {
    return 0;
  }
  const uint32_t stampMs = nowMs ? nowMs : 1;            // millis() == 0 must not read as "never"
  const bool rmc = f.lastKind == NMEA_LAST_RMC;
  bool stamp = false;
  int sats = -1, hdop = -1;                              // the stamp's rating
  int32_t latI = 0, lonI = 0;

  if (rmc) {
    const bool fix = rmcSaysFix(f);
    /* Vetoed by a GGA of the same second that came FIRST and said no fix (a GGA-first receiver;
     * the M100's GGA for this second has not arrived yet, so it never meets this). */
    stamp = fix && !(sameSecond(rx->ggaHms, f.rmcHms) && !rx->ggaFix);
    rx->rmcHms = f.rmcHms;
    rx->rmcFix = fix;
    if (stamp) {
      latI = f.latI;
      lonI = f.lonI;
      /* No sats/HDOP in an RMC: the last GGA's own words, while they are this epoch's or the
       * one before (self_pos.h - a no-fix GGA's "00" included, which is the point). */
      if (rx->ggaMs != 0 && (uint32_t)(nowMs - rx->ggaMs) <= SELF_GPS_GGA_PAIR_MS) {
        sats = rx->sats;
        hdop = rx->hdopX10;
      }
      /* ...but a borrowed rating may only make the fix look WORSE (review 2026-10-11). Where it
       * passes - "not said" included: an empty GGA, or none within the window - while the last
       * word on record FAILS, that failing word stands: the last GGA's own (its "00" from before
       * a cold start's lost GGAs), else the fix this one replaces (3 satellites, then GGAs lost
       * or a GGA with empty fields). Only a GGA that says better lifts it. -1 above let a 20 km
       * fix go LIVE - and out on the beacon - after 3 s of lost GGAs. */
      if (meshPosFixUsable(sats, hdop)) {
        if (rx->ggaMs != 0 && !meshPosFixUsable(rx->sats, rx->hdopX10)) {
          sats = rx->sats;
          hdop = rx->hdopX10;
        } else if (rx->fixMs != 0 && !meshPosFixUsable(rx->fixSats, rx->fixHdopX10)) {
          sats = rx->fixSats;
          hdop = rx->fixHdopX10;
        }
      }
    }
  } else {
    const bool fix = ggaSaysFix(f);
    /* The receiver's word on its sky is this GGA's, whatever its quality: what `gps` shows and
     * what the next RMC is rated by. */
    rx->sats = ggaWord(f.ggaSats);
    rx->hdopX10 = ggaWord(f.ggaHdopX10);
    rx->ggaMs = stampMs;
    const bool garbled = f.ggaSats == -2 || f.ggaHdopX10 == -2;
    stamp = fix && !garbled && !(sameSecond(rx->rmcHms, f.ggaHms) && !rx->rmcFix);
    rx->ggaHms = f.ggaHms;
    rx->ggaFix = fix;
    if (stamp) {
      latI = f.ggaLatI;
      lonI = f.ggaLonI;
      sats = rx->sats;
      hdop = rx->hdopX10;
    } else if (rx->fixMs != 0 && !meshPosFixUsable(rx->sats, rx->hdopX10)) {
      /* No fix in this sentence, and its OWN numbers say the sky is poor (the M100's "00",
       * 99.99): the last fix reads POOR from now on — as the plate always has — at its own
       * time. Only a failing rating is copied, so nothing here can make a fix look better. */
      rx->fixSats = rx->sats;
      rx->fixHdopX10 = rx->hdopX10;
      if (slot && !slot->injected && slot->fixMs != 0) {
        slot->sats = rx->fixSats;          // a bench fix is the bench's: the next stamp replaces it
        slot->hdopX10 = rx->fixHdopX10;
      }
    }
  }
  if (!stamp) {
    return 0;
  }

  const bool first = rx->fixMs == 0;
  rx->latI = latI;
  rx->lonI = lonI;
  rx->fixMs = stampMs;
  rx->fixSats = sats;
  rx->fixHdopX10 = hdop;
  /* Each from ITS OWN sentence, with that sentence's stamp (review 2026-09-27): the motion only
   * from a stamping RMC (-1 where it had none), the altitude only from a stamping GGA. */
  if (rmc) {
    rx->speedKnX100 = f.speedKnX100;
    rx->courseX10 = f.courseX10;
    rx->motionMs = stampMs;
  } else {
    rx->altM = f.ggaAltM;
    rx->altMs = stampMs;
  }
  if (slot) {
    selfSlotSet(slot, latI, lonI, sats, hdop, stampMs, false);   // replaces a bench fix too
  }
  return SELF_GPS_STAMPED | (first ? SELF_GPS_FIRST : 0);
}

// ---------------------------------------------------------------- the stream's small rules

int selfPhonePorts(uint16_t custom, uint16_t out[SELF_PHONE_PORTS_MAX]) {
  int n = 0;
  out[n++] = SELF_PHONE_PORT_NMEA;
  out[n++] = SELF_PHONE_PORT_GPS2IP;
  if (custom != 0 && custom != SELF_PHONE_PORT_NMEA && custom != SELF_PHONE_PORT_GPS2IP) {
    out[n++] = custom;
  }
  return n;
}

int32_t selfHmsToSod(int32_t hms) {
  if (hms < 0) {
    return -1;
  }
  const int32_t hh = hms / 10000, mm = (hms / 100) % 100, ss = hms % 100;
  if (hh > 23 || mm > 59 || ss > 60) {           // 60: a leap second is a real second
    return -1;
  }
  return hh * 3600 + mm * 60 + ss;
}

int32_t selfSodDiff(int32_t a, int32_t b) {
  int32_t d = (a - b) % 86400;
  if (d < 0) {
    d += 86400;
  }
  return d > 43200 ? 86400 - d : d;
}

bool selfPhoneSettled(uint32_t connMs, uint32_t nowMs) {
  return (uint32_t)(nowMs - connMs) >= SELF_PHONE_SETTLE_MS;
}

bool selfPhoneTimeOk(int32_t hms, int32_t utcSod) {
  if (utcSod < 0) {
    return true;                                  // no trusted clock to judge by
  }
  const int32_t sod = selfHmsToSod(hms);
  if (sod < 0) {
    return false;                                 // cannot be shown to be current
  }
  return selfSodDiff(sod, utcSod) <= SELF_PHONE_TIME_TOL_S;
}

bool selfRmcModeOk(char mode) {
  switch (mode) {
    case 0:                                       // pre-2.3: no mode field at all
    case 'A': case 'D': case 'F': case 'P': case 'R':
      return true;
    default:                                      // E N M S, and anything we do not know
      return false;
  }
}

const char* selfPhoneVerdictName(int v) {
  switch (v) {
    case SELF_PV_NONE:         return "none";
    case SELF_PV_FIX:          return "fix";
    case SELF_PV_POOR:         return "poor";
    case SELF_PV_NOFIX:        return "no fix";
    case SELF_PV_NOT_MEASURED: return "not measured";
    case SELF_PV_GARBLED:      return "garbled";
    case SELF_PV_WAIT:         return "unconfirmed";
    case SELF_PV_STALE:        return "stale";
    case SELF_PV_CHECKING:     return "checking";
    case SELF_PV_JUMPED:       return "jumped";
    default:                   return "?";
  }
}

bool selfPhoneAgrees(int32_t prevLatI, int32_t prevLonI, uint32_t prevMs,
                     int32_t latI, int32_t lonI, uint32_t nowMs) {
  const uint32_t dt = (uint32_t)(nowMs - prevMs);
  if (dt > SELF_PHONE_AGREE_GAP_MS) {
    return false;                                 // too long ago to vouch for anything
  }
  const double lim = SELF_PHONE_AGREE_SLACK_M + SELF_PHONE_MAX_SPEED_MPS * (dt / 1000.0);
  return meshPosDistanceM(prevLatI, prevLonI, latI, lonI) <= lim;
}

bool selfPhoneKeep(SelfPhoneStream* s, const SelfPhoneOut* out, uint32_t nowMs) {
  if (!s || !out) {
    return false;
  }
  const bool hadCand = s->candOk;
  const bool agrees = hadCand && selfPhoneAgrees(s->candLatI, s->candLonI, s->candMs,
                                                 out->latI, out->lonI, nowMs);
  s->candOk = true;                               // the yardstick for the next, kept or not
  s->candLatI = out->latI;
  s->candLonI = out->lonI;
  s->candMs = nowMs;
  if (agrees) {
    return true;
  }
  if (hadCand) {
    s->nDisagree++;
  }
  /* Only when the fix in hand IS the last judgement: a later GGA of the same chunk that wrote
   * nothing (NOFIX, WAIT...) already said what the stream is doing now. */
  if (s->lastVerdict == SELF_PV_FIX || s->lastVerdict == SELF_PV_POOR) {
    s->lastVerdict = hadCand ? SELF_PV_JUMPED : SELF_PV_CHECKING;
  }
  return false;
}

bool selfNmeaStreamFeed(NmeaReader* r, char c) {
  if (!r) {
    return false;
  }
  if (c == '$') {
    const bool done = r->feed('\n');              // finish the sentence in hand, if any
    r->feed('$');                                 // a '$' never completes one
    return done;
  }
  return r->feed(c);
}

// ---------------------------------------------------------------- the stream

static void gateReset(SelfPhoneGate* g) {
  g->seenRmc = g->seenGga = 0;                    // the reader's counts restart at 0 with it
  g->rmcHms = -1;
  g->rmcOk = false;
  g->pend = false;
  g->pendHms = -1;
  g->pendLatI = g->pendLonI = 0;
  g->pendMs = 0;
}

void selfPhoneInit(SelfPhoneStream* s) {
  if (!s) {
    return;
  }
  s->reader.reset();
  gateReset(&s->gate);
  s->connMs = s->lastSentenceMs = 0;
  s->rmcSeen = s->ggaSeen = 0;
  s->lastVerdict = SELF_PV_NONE;
  s->lastSats = s->lastHdopX10 = -1;
  s->candOk = false;
  s->candLatI = s->candLonI = 0;
  s->candMs = 0;
  s->sentBase = s->badBase = 0;
  s->nFix = s->nPoor = s->nNoFix = s->nNotMeasured = s->nGarbled = s->nStale = 0;
  s->nDisagree = 0;
}

void selfPhoneBegin(SelfPhoneStream* s, uint32_t nowMs) {
  if (!s) {
    return;
  }
  /* A new connection starts clean: the last connection's half line, its RMC of "this second"
   * and any GGA waiting for one belong to a stream that no longer exists. The boot-long
   * counters keep what the old reader counted. */
  s->sentBase += s->reader.sentences();
  s->badBase += s->reader.badChecksum();
  s->reader.reset();
  gateReset(&s->gate);
  s->connMs = nowMs;
  s->lastSentenceMs = nowMs;
  s->rmcSeen = s->ggaSeen = 0;
  s->lastVerdict = SELF_PV_NONE;
  s->lastSats = s->lastHdopX10 = -1;
  s->candOk = false;                              // a new connection's first fix waits for a second
  s->port = 0;
  s->placeholderRun = 0;
  s->placeholder = false;
}

void selfPhoneSetPort(SelfPhoneStream* s, uint16_t port) {
  if (s) {
    s->port = port;
  }
}

uint32_t selfPhoneSentences(const SelfPhoneStream* s) {
  return s ? s->sentBase + s->reader.sentences() : 0;
}

uint32_t selfPhoneBadChecksums(const SelfPhoneStream* s) {
  return s ? s->badBase + s->reader.badChecksum() : 0;
}

bool selfPhoneSilent(const SelfPhoneStream* s, uint32_t nowMs) {
  return s && (uint32_t)(nowMs - s->lastSentenceMs) >= SELF_PHONE_SILENT_MS;
}

static void countVerdict(SelfPhoneStream* s, int v) {
  switch (v) {
    case SELF_PV_FIX:          s->nFix++; break;
    case SELF_PV_POOR:         s->nPoor++; break;
    case SELF_PV_NOFIX:        s->nNoFix++; break;
    case SELF_PV_NOT_MEASURED: s->nNotMeasured++; break;
    case SELF_PV_GARBLED:      s->nGarbled++; break;
    case SELF_PV_STALE:        s->nStale++; break;
    default: break;
  }
}

static void wrote(SelfPhoneStream* s, SelfPhoneOut* out, bool* any, int v,
                  int32_t latI, int32_t lonI, int sats, int hdopX10) {
  // GPS 2 IP's stand-in pair is not a rating (self_pos.h): read it as "not said".
  if (sats == SELF_PHONE_PLACEHOLDER_SATS && hdopX10 == SELF_PHONE_PLACEHOLDER_HDOPX10) {
    if (s->placeholderRun < 255) {
      s->placeholderRun++;
    }
    if (s->port == SELF_PHONE_PORT_GPS2IP || s->placeholderRun >= SELF_PHONE_PLACEHOLDER_RUN) {
      s->placeholder = true;
    }
  } else {
    s->placeholderRun = 0;
    s->placeholder = false;
  }
  if (s->placeholder) {
    sats = hdopX10 = -1;
  }
  out->verdict = v;
  out->latI = latI;
  out->lonI = lonI;
  out->sats = sats < 0 ? -1 : sats;
  out->hdopX10 = hdopX10 < 0 ? -1 : hdopX10;
  s->lastSats = out->sats;
  s->lastHdopX10 = out->hdopX10;
  *any = true;
}

/* An RMC: never a position here (decision: position comes from a GGA only), just this second's
 * word on whether the receiver has a measured fix — the confirmation a GGA without sats/HDOP
 * needs, and a veto on one of the same second. */
static void onRmc(SelfPhoneStream* s, const NmeaFix& f, bool settled, uint32_t nowMs,
                  int32_t utcSod, SelfPhoneOut* out, bool* any) {
  SelfPhoneGate* g = &s->gate;
  s->rmcSeen++;
  if (!settled) {
    s->nStale++;
    return;
  }
  const bool ok = f.rmcStatus == 'A' && selfRmcModeOk(f.rmcMode);
  if (ok && !selfPhoneTimeOk(f.rmcHms, utcSod)) {
    s->nStale++;                                  // a stale 'A' must not confirm anything
    return;
  }
  g->rmcHms = f.rmcHms;
  g->rmcOk = ok;
  if (g->pend && f.rmcHms >= 0 && f.rmcHms == g->pendHms) {
    g->pend = false;
    if ((uint32_t)(nowMs - g->pendMs) > SELF_PHONE_PEND_MS) {
      return;                                     // too late to vouch for it
    }
    const int v = ok ? SELF_PV_FIX : SELF_PV_NOFIX;
    s->lastVerdict = v;
    countVerdict(s, v);
    if (ok) {
      wrote(s, out, any, v, g->pendLatI, g->pendLonI, -1, -1);
    }
  }
}

static void onGga(SelfPhoneStream* s, const NmeaFix& f, bool settled, uint32_t nowMs,
                  int32_t utcSod, SelfPhoneOut* out, bool* any) {
  SelfPhoneGate* g = &s->gate;
  s->ggaSeen++;
  g->pend = false;                                // a newer GGA supersedes one still waiting
  int v;
  const bool sameSec = g->rmcHms >= 0 && f.ggaHms >= 0 && g->rmcHms == f.ggaHms;
  if (!settled) {
    v = SELF_PV_STALE;
  } else if (f.ggaQuality <= 0 || !f.ggaHasPos) {
    v = SELF_PV_NOFIX;                            // quality 0, empty, or no coordinates
  } else if (f.ggaQuality >= 6) {
    v = SELF_PV_NOT_MEASURED;                     // dead reckoning, manual, simulator
  } else if (!selfPhoneTimeOk(f.ggaHms, utcSod)) {
    v = SELF_PV_STALE;
  } else if (f.ggaSats == -2 || f.ggaHdopX10 == -2) {
    v = SELF_PV_GARBLED;
  } else if (sameSec && !g->rmcOk) {
    v = SELF_PV_NOFIX;                            // its own second's RMC said V / E N M S
  } else if (f.ggaSats < 0 && f.ggaHdopX10 < 0) {
    if (sameSec) {
      v = SELF_PV_FIX;                            // vouched for by an 'A' RMC of the same second
    } else {
      v = SELF_PV_WAIT;
      if (f.ggaHms >= 0) {                        // an RMC that follows may still vouch for it
        g->pend = true;
        g->pendHms = f.ggaHms;
        g->pendLatI = f.ggaLatI;
        g->pendLonI = f.ggaLonI;
        g->pendMs = nowMs;
      }
    }
  } else if (!meshPosFixUsable(f.ggaSats, f.ggaHdopX10)) {
    v = SELF_PV_POOR;
  } else {
    v = SELF_PV_FIX;
  }
  s->lastVerdict = v;
  countVerdict(s, v);
  if (v == SELF_PV_FIX || v == SELF_PV_POOR) {
    wrote(s, out, any, v, f.ggaLatI, f.ggaLonI, f.ggaSats, f.ggaHdopX10);
  }
}

bool selfPhoneFeed(SelfPhoneStream* s, const uint8_t* buf, size_t n, uint32_t nowMs,
                   int32_t utcSod, SelfPhoneOut* out) {
  if (!s || !out || (!buf && n)) {
    return false;
  }
  bool any = false;
  const uint32_t sent0 = s->reader.sentences();
  for (size_t i = 0; i < n; i++) {
    if (!selfNmeaStreamFeed(&s->reader, (char)buf[i])) {
      continue;
    }
    const NmeaFix& f = s->reader.fix();
    const bool settled = selfPhoneSettled(s->connMs, nowMs);
    /* Exactly one count moves per completed sentence; which one says what just arrived. */
    if (f.rmcCount != s->gate.seenRmc) {
      s->gate.seenRmc = f.rmcCount;
      onRmc(s, f, settled, nowMs, utcSod, out, &any);
    } else if (f.ggaCount != s->gate.seenGga) {
      s->gate.seenGga = f.ggaCount;
      onGga(s, f, settled, nowMs, utcSod, out, &any);
    }
  }
  if (s->reader.sentences() != sent0) {
    s->lastSentenceMs = nowMs;
  }
  return any;
}

// ---------------------------------------------------------------- finding the app

uint32_t selfProbeDelayMs(int fails, int deadRounds) {
  static const uint32_t LADDER[5] = { 2000u, 5000u, 10000u, 30000u, 60000u };
  if (deadRounds >= SELF_PHONE_DEAD_ROUNDS) {
    return SELF_PHONE_DEAD_WAIT_MS;
  }
  if (fails < 1) {
    return 0;
  }
  return LADDER[(fails > 5 ? 5 : fails) - 1];
}

static uint8_t inc8(uint8_t v) {
  return v < 255 ? (uint8_t)(v + 1) : v;
}

static void arm(SelfProbe* p, uint32_t nowMs, uint32_t delayMs) {
  if (delayMs == 0) {
    p->wait = false;
  } else {
    p->wait = true;
    p->nextMs = nowMs + delayMs;
  }
}

void selfProbeStart(SelfProbe* p, uint32_t assoc) {
  if (!p) {
    return;
  }
  memset(p, 0, sizeof(*p));
  p->assoc = assoc;
}

void selfProbeSetHotspot(SelfProbe* p, bool hotspot) {
  if (p) {
    p->hotspot = hotspot;
  }
}

// The ladder, with a phone hotspot's dead rounds asked again sooner (SelfProbe.hotspot).
static uint32_t probeDelay(const SelfProbe* p) {
  const uint32_t d = selfProbeDelayMs(p->fails, p->deadRounds);
  return (p->hotspot && d > SELF_PHONE_HOTSPOT_WAIT_MS) ? SELF_PHONE_HOTSPOT_WAIT_MS : d;
}

bool selfProbeDue(const SelfProbe* p, uint32_t nowMs) {
  // Signed difference: right across the millis() wrap, for any wait under 24 days.
  return p && (!p->wait || (int32_t)(nowMs - p->nextMs) >= 0);
}

uint16_t selfProbePort(const SelfProbe* p, uint16_t custom) {
  if (!p) {
    return 0;
  }
  if (p->port) {
    return p->port;
  }
  uint16_t ports[SELF_PHONE_PORTS_MAX];
  const int n = selfPhonePorts(custom, ports);
  return ports[p->idx < n ? p->idx : 0];          // a custom port removed mid-round: start over
}

void selfProbeFailed(SelfProbe* p, uint16_t custom, uint32_t nowMs) {
  if (!p) {
    return;
  }
  if (p->port) {
    p->fails = inc8(p->fails);
    p->portFails = inc8(p->portFails);
    if (p->portFails >= SELF_PHONE_CACHE_FAILS) {
      p->port = 0;                                // the app may have changed: search again
      p->idx = 0;
      p->portFails = 0;
    }
    arm(p, nowMs, probeDelay(p));
    return;
  }
  uint16_t ports[SELF_PHONE_PORTS_MAX];
  const int n = selfPhonePorts(custom, ports);
  p->idx = inc8(p->idx);
  if (p->idx < n) {
    p->wait = false;                              // the next port of the round, now
    return;
  }
  p->idx = 0;                                     // the whole round failed
  p->fails = inc8(p->fails);
  p->deadRounds = inc8(p->deadRounds);
  arm(p, nowMs, probeDelay(p));
}

void selfProbeGood(SelfProbe* p, uint16_t port) {
  if (!p || port == 0) {
    return;
  }
  p->port = port;
  p->idx = 0;
  p->fails = 0;
  p->portFails = 0;
  p->deadRounds = 0;
  p->wait = false;
}

void selfProbeDropped(SelfProbe* p, uint32_t nowMs) {
  if (!p) {
    return;
  }
  p->fails = inc8(p->fails);
  arm(p, nowMs, probeDelay(p));
}

bool selfProbeNoApp(const SelfProbe* p) {
  return p && p->port == 0 && p->deadRounds > 0;
}

bool selfPhoneMayRun(bool autoOn, bool staConnected, bool ownAp, bool radioOff) {
  return autoOn && staConnected && !ownAp && !radioOff;
}

/* Up to `maxDigits` decimal digits at *p, value <= maxVal; advances *p. */
static bool takeNum(const char** p, int maxDigits, uint32_t maxVal, uint32_t* v) {
  uint32_t x = 0;
  int d = 0;
  while (**p >= '0' && **p <= '9') {
    if (++d > maxDigits) {
      return false;
    }
    x = x * 10 + (uint32_t)(**p - '0');
    (*p)++;
  }
  if (d == 0 || x > maxVal) {
    return false;
  }
  *v = x;
  return true;
}

bool selfPhoneParseAt(const char* s, uint8_t ip[4], uint16_t* port) {
  if (!s || !ip || !port) {
    return false;
  }
  const char* p = s;
  uint8_t b[4];
  for (int i = 0; i < 4; i++) {
    uint32_t v;
    if (!takeNum(&p, 3, 255, &v)) {
      return false;
    }
    b[i] = (uint8_t)v;
    if (i < 3) {
      if (*p != '.') {
        return false;
      }
      p++;
    }
  }
  uint32_t pt = 0;
  if (*p == ':') {
    p++;
    if (!takeNum(&p, 5, 65535, &pt) || pt == 0) {
      return false;
    }
  }
  if (*p != 0) {
    return false;
  }
  memcpy(ip, b, 4);
  *port = (uint16_t)pt;
  return true;
}

// ---------------------------------------------------------------- the screen's words

size_t selfPhoneStatusText(char* out, size_t cap, int link, const SelfPhoneStream* s,
                           uint32_t nowMs) {
  (void)nowMs;
  if (!out || cap == 0) {
    return 0;
  }
  const char* t = "?";
  const char* suffix = NULL;              // set: "<t>, N sats<suffix>" when the count is known
  int sats = -1;
  switch (link) {
    case SELF_LINK_OFF:     t = "off"; break;
    case SELF_LINK_NO_WIFI: t = "not on WiFi"; break;
    case SELF_LINK_LOOKING: t = "looking for a location app"; break;
    case SELF_LINK_NO_APP:  t = "no app found on this network"; break;
    case SELF_LINK_STREAM:
      if (!s) {
        t = "connected";
        break;
      }
      sats = s->lastSats;
      switch (s->lastVerdict) {
        /* "this network", not "on hotspot": any network's gateway is asked (phone_pos.h, the
         * trust model), and a home router or a boat's NMEA gateway is not a hotspot. */
        case SELF_PV_FIX:
          if (s->lastSats < 0 && s->lastHdopX10 < 0) {
            t = s->placeholder ? "iPhone app gives no accuracy: shown, not sent"
                               : "fix, accuracy unknown: not sent";      // UNRATED (self_pos.h)
          } else {
            t = "this network: fix";
            suffix = "";
          }
          break;
        case SELF_PV_POOR:         t = "weak fix"; suffix = ": not sent"; break;
        case SELF_PV_CHECKING:     t = "fix found, checking it"; break;
        case SELF_PV_JUMPED:       t = "fixes jump about: not used"; break;
        case SELF_PV_NOFIX:        t = "no fix (indoors?)"; break;
        case SELF_PV_NOT_MEASURED: t = "estimate only, not a GPS fix"; break;
        case SELF_PV_GARBLED:      t = "unreadable data from the app"; break;
        case SELF_PV_WAIT:         t = "fix not confirmed (no RMC)"; break;
        case SELF_PV_STALE:        t = "old data dropped, waiting"; break;
        default:
          /* An app that sends only GENERATED RMC (gpsdRelay with relaying unticked) never sends
           * the GGA the gate needs: say so, rather than "waiting" for ever. */
          t = (s->rmcSeen >= 3 && s->ggaSeen == 0) ? "the app sends no GGA"
                                                   : "connected, waiting for data";
          break;
      }
      break;
    default: break;
  }
  int n;
  if (suffix && sats >= 0) {
    n = snprintf(out, cap, "%s, %d sats%s", t, sats, suffix);
  } else {
    n = snprintf(out, cap, "%s%s", t, suffix ? suffix : "");
  }
  if (n < 0) {
    out[0] = 0;
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}
