/* NMEA 0183 → fixed-point position. See nmea.h for the contract.
 *
 * The only piece with any subtlety is coordinate conversion without floats:
 * NMEA latitude is ddmm.mmmmm (degrees then MINUTES), longitude dddmm.mmmmm.
 * We parse the minutes as an integer scaled to 1e6 ("minE6"), then
 *     degreesE7 = deg * 10,000,000  +  minE6 / 6
 * because minutes/60 in 1e-7-degree units is minE6 * 1e7 / (60 * 1e6) = minE6/6.
 * Worst-case truncation error is 1e-7 deg ≈ 1.1 cm — beneath the GPS's own noise
 * by three orders of magnitude. int64 intermediates; no rounding ambiguity to
 * argue about in a test.
 */
#include "nmea.h"

#include <string.h>

void NmeaReader::reset() {
  len = 0;
  inSentence = false;
  memset(&f, 0, sizeof(f));
  f.sats = -1;
  f.hdopX10 = -1;
  f.altM = -10000;
  f.rmcHms = -1;
  f.rmcDmy = -1;
  f.speedKnX100 = -1;
  f.courseX10 = -1;
  f.ggaQuality = -1;
  f.ggaSats = -1;
  f.ggaHdopX10 = -1;
  f.ggaHms = -1;
  f.ggaAltM = -10000;
  f.lastKind = NMEA_LAST_NONE;
  nBytes = nSentences = nBadChecksum = nOverrun = 0;
}

/* ---- small field helpers ---------------------------------------------------
 * NMEA fields are comma-separated and MAY BE EMPTY (a no-fix RMC is mostly
 * commas). Every helper treats "empty" as "absent" and leaves the output alone,
 * so a V sentence cannot smear zeros over the last good coordinates. */

// Advance to field `idx` (0 = first after the sentence type). NULL if missing.
static const char* fieldAt(const char* body, int idx) {
  const char* p = strchr(body, ',');
  if (!p) {
    return 0;
  }
  p++;                                  // first field
  while (idx-- > 0) {
    p = strchr(p, ',');
    if (!p) {
      return 0;
    }
    p++;
  }
  return p;
}

static int fieldLen(const char* p) {
  int n = 0;
  while (p[n] && p[n] != ',') {
    n++;
  }
  return n;
}

// Plain unsigned digits (no sign, no dot) → -1 if empty/non-digit.
static long parseUInt(const char* p, int n) {
  if (n <= 0) {
    return -1;
  }
  long v = 0;
  for (int i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') {
      return -1;
    }
    v = v * 10 + (p[i] - '0');
  }
  return v;
}

/* "ddmm.mmmmm" (lat, degDigits=2) or "dddmm.mmmmm" (lon, degDigits=3) → 1e-7 deg.
 * Fraction digits beyond 6 are ignored; missing fraction is fine (whole minutes). */
static bool parseCoord(const char* p, int n, int degDigits, char hemi, int32_t* out) {
  if (n < degDigits + 2) {
    return false;
  }
  long deg = parseUInt(p, degDigits);
  long minWhole = parseUInt(p + degDigits, 2);
  if (deg < 0 || minWhole < 0 || minWhole > 59) {
    return false;
  }
  int64_t minE6 = (int64_t)minWhole * 1000000;
  int i = degDigits + 2;
  if (i < n && p[i] == '.') {
    i++;
    int64_t scale = 100000;             // first fraction digit = 1e-1 minute = 1e5 in E6
    while (i < n && scale > 0) {
      if (p[i] < '0' || p[i] > '9') {
        return false;
      }
      minE6 += (p[i] - '0') * scale;
      scale /= 10;
      i++;
    }
    // beyond 6 fraction digits: legal, ignored
    while (i < n && p[i] >= '0' && p[i] <= '9') {
      i++;
    }
    if (i != n) {
      return false;
    }
  } else if (i != n) {
    return false;
  }
  int64_t e7 = (int64_t)deg * 10000000 + minE6 / 6;
  if (hemi == 'S' || hemi == 'W') {
    e7 = -e7;
  } else if (hemi != 'N' && hemi != 'E') {
    return false;
  }
  if (e7 > 1800000000LL || e7 < -1800000000LL) {
    return false;
  }
  *out = (int32_t)e7;
  return true;
}

// hhmmss(.sss) → hhmmss as an integer; -1 if absent/garbled.
static long parseHms(const char* p, int n) {
  int digits = 0;
  while (digits < n && p[digits] >= '0' && p[digits] <= '9') {
    digits++;
  }
  if (digits != 6) {
    return -1;
  }
  return parseUInt(p, 6);
}

/* The STRICT form, for the clock: exactly "hhmmss" or "hhmmss.f..." and nothing else, with
 * the fraction kept (first three digits, as ms). parseHms above is lenient on purpose — it
 * feeds a display — but a clock set from "123519x" would be a clock set from a sentence we
 * did not understand. Range checks (hh < 24 ...) are clock_source.cpp's, with the date's. */
static bool parseHmsStrict(const char* p, int n, int32_t* hms, int16_t* ms) {
  if (n < 6) {
    return false;
  }
  long v = parseUInt(p, 6);
  if (v < 0) {
    return false;
  }
  int frac = 0;
  if (n > 6) {
    if (p[6] != '.' || n == 7) {
      return false;
    }
    int scale = 100;
    for (int i = 7; i < n; i++) {
      if (p[i] < '0' || p[i] > '9') {
        return false;
      }
      frac += (p[i] - '0') * scale;
      scale /= 10;                      // beyond the third digit: legal, ignored
    }
  }
  *hms = (int32_t)v;
  *ms = (int16_t)frac;
  return true;
}

/* An unsigned decimal "ddd" or "ddd.fff" as a fixed-point integer with `decimals` places
 * (decimals 2: "0.36" -> 36, "5" -> 500, "12.345" -> 1234 - digits past `decimals` are
 * truncated, legal and ignored). Empty, a sign, a second dot, a bare "." or any other
 * character is -1: a speed we did not understand is no speed, never a guess. At most 6 whole
 * digits, so the result always fits an int32 (a knot figure that long is not a speed). */
static int32_t parseFixed(const char* p, int n, int decimals) {
  if (n <= 0) {
    return -1;
  }
  int i = 0;
  int32_t whole = 0;
  int wholeDigits = 0;
  while (i < n && p[i] >= '0' && p[i] <= '9') {
    if (++wholeDigits > 6) {
      return -1;
    }
    whole = whole * 10 + (p[i] - '0');
    i++;
  }
  int32_t frac = 0;
  int fracDigits = 0;
  if (i < n && p[i] == '.') {
    i++;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
      if (fracDigits < decimals) {
        frac = frac * 10 + (p[i] - '0');
        fracDigits++;
      }
      i++;
    }
  }
  if (i != n || (wholeDigits == 0 && fracDigits == 0)) {
    return -1;                          // trailing junk, or "." / "" with no digit at all
  }
  while (fracDigits < decimals) {       // "5" -> 500, "5.4" -> 540
    frac *= 10;
    fracDigits++;
  }
  int32_t scale = 1;
  for (int k = 0; k < decimals; k++) {
    scale *= 10;
  }
  return whole * scale + frac;
}

/* One verified sentence body: "GNRMC,123519,A,4807.038,N,...". Talker (GP/GN/GL/
 * GA/GB...) is ignored — the M100 is multi-constellation and answers as GN. */
bool NmeaReader::parseSentence(const char* body, NmeaFix* f) {
  size_t blen = strlen(body);
  if (blen < 5) {
    return false;
  }
  const char* type = body + 2;          // past the 2-char talker

  if (strncmp(type, "RMC", 3) == 0) {
    /* RMC: 0=time 1=status 2=lat 3=N/S 4=lon 5=E/W 6=knots 7=course 8=date */
    const char* st = fieldAt(body, 1);
    if (!st) {
      return false;
    }
    bool valid = (st[0] == 'A');
    const char* lat = fieldAt(body, 2);
    const char* ns  = fieldAt(body, 3);
    const char* lon = fieldAt(body, 4);
    const char* ew  = fieldAt(body, 5);
    if (valid && lat && ns && lon && ew) {
      int32_t la, lo;
      if (parseCoord(lat, fieldLen(lat), 2, ns[0], &la) &&
          parseCoord(lon, fieldLen(lon), 3, ew[0], &lo)) {
        f->latI = la;
        f->lonI = lo;
      } else {
        valid = false;                  // status A with unreadable coords = not a fix
      }
    } else if (valid) {
      valid = false;
    }
    f->valid = valid;
    const char* t = fieldAt(body, 0);
    if (t) {
      long hms = parseHms(t, fieldLen(t));
      if (hms >= 0) {
        f->timeHms = (uint32_t)hms;
      }
    }
    const char* d = fieldAt(body, 8);
    if (d) {
      long dmy = parseUInt(d, fieldLen(d));
      if (dmy > 0) {
        f->dateDmy = (uint32_t)dmy;
      }
    }
    /* This RMC's own time, for the clock (see NmeaFix). Every field is rewritten, present
     * or not: "absent in THIS sentence" must read as absent, never as the last one's. */
    f->rmcCount++;
    f->rmcStatus = (st[0] == 'A' || st[0] == 'V') ? st[0] : 0;
    f->rmcHms = -1;
    f->rmcMs = 0;
    if (t) {
      int32_t hms;
      int16_t ms;
      if (parseHmsStrict(t, fieldLen(t), &hms, &ms)) {
        f->rmcHms = hms;
        f->rmcMs = ms;
      }
    }
    f->rmcDmy = -1;
    if (d && fieldLen(d) == 6) {
      long dmy = parseUInt(d, 6);       // "010127" -> 10127: the leading zero is the day's
      if (dmy >= 0) {
        f->rmcDmy = (int32_t)dmy;
      }
    }
    const char* mode = fieldAt(body, 11);   // 9=magvar 10=E/W 11=mode (NMEA 2.3+)
    f->rmcMode = (mode && fieldLen(mode) == 1) ? mode[0] : 0;
    /* Motion (0.9.80): this sentence's own speed and course, or -1 - never the last RMC's
     * (the rule above: rewritten by every RMC, present or not). */
    const char* sog = fieldAt(body, 6);
    f->speedKnX100 = sog ? parseFixed(sog, fieldLen(sog), 2) : -1;
    const char* cog = fieldAt(body, 7);
    int32_t c = cog ? parseFixed(cog, fieldLen(cog), 1) : -1;
    if (c == 3600) {
      c = 0;                            // "360.0" is north, the same as "0.0"
    } else if (c > 3600) {
      c = -1;                           // not a bearing
    }
    f->courseX10 = c;
    f->lastKind = NMEA_LAST_RMC;
    return true;
  }

  if (strncmp(type, "GGA", 3) == 0) {
    /* GGA: 0=time 1=lat 2=N/S 3=lon 4=E/W 5=quality 6=sats 7=hdop 8=alt 9=M */
    const char* q = fieldAt(body, 5);
    long quality = q ? parseUInt(q, fieldLen(q)) : -1;
    const char* s = fieldAt(body, 6);
    if (s) {
      long sats = parseUInt(s, fieldLen(s));
      if (sats >= 0) {
        f->sats = (int)sats;
      }
    }
    const char* h = fieldAt(body, 7);
    if (h) {
      int n = fieldLen(h);
      // hdop "1.23" → 12 (x10, extra digits truncated); "2" → 20
      const char* dot = (const char*)memchr(h, '.', n);
      long whole = parseUInt(h, dot ? (int)(dot - h) : n);
      long tenth = (dot && dot + 1 < h + n && dot[1] >= '0' && dot[1] <= '9') ? dot[1] - '0' : 0;
      if (whole >= 0) {
        f->hdopX10 = (int)(whole * 10 + tenth);
      }
    }
    const char* a = fieldAt(body, 8);
    if (a) {
      int n = fieldLen(a);
      bool neg = n > 0 && a[0] == '-';
      const char* dot = (const char*)memchr(a, '.', n);
      long whole = parseUInt(a + (neg ? 1 : 0), (dot ? (int)(dot - a) : n) - (neg ? 1 : 0));
      if (whole >= 0) {
        f->altM = (int)(neg ? -whole : whole);
      }
    }
    if (quality > 0) {
      const char* lat = fieldAt(body, 1);
      const char* ns  = fieldAt(body, 2);
      const char* lon = fieldAt(body, 3);
      const char* ew  = fieldAt(body, 4);
      int32_t la, lo;
      if (lat && ns && lon && ew &&
          parseCoord(lat, fieldLen(lat), 2, ns[0], &la) &&
          parseCoord(lon, fieldLen(lon), 3, ew[0], &lo)) {
        f->latI = la;
        f->lonI = lo;
      }
    }
    const char* t = fieldAt(body, 0);
    if (t) {
      long hms = parseHms(t, fieldLen(t));
      if (hms >= 0) {
        f->timeHms = (uint32_t)hms;
      }
    }

    /* THIS GGA's own fields (nmea.h): every one rewritten, present or not, so nothing here is
     * ever inherited from an older sentence. Lengths are capped before parseUInt, which would
     * overflow a long on a long digit run — harmless from the woods UART, not from a socket. */
    f->ggaCount++;
    f->ggaQuality = (q && fieldLen(q) >= 1 && fieldLen(q) <= 2) ? (int32_t)quality : -1;
    f->ggaSats = -1;
    if (s && fieldLen(s) > 0) {
      long v = fieldLen(s) <= 3 ? parseUInt(s, fieldLen(s)) : -1;
      f->ggaSats = v >= 0 ? (int32_t)v : -2;
    }
    f->ggaHdopX10 = -1;
    if (h && fieldLen(h) > 0) {
      int32_t v = parseFixed(h, fieldLen(h), 1);
      f->ggaHdopX10 = v >= 0 ? v : -2;
    }
    f->ggaHasPos = false;
    f->ggaLatI = f->ggaLonI = 0;
    {
      const char* lat = fieldAt(body, 1);
      const char* ns  = fieldAt(body, 2);
      const char* lon = fieldAt(body, 3);
      const char* ew  = fieldAt(body, 4);
      int32_t la, lo;
      if (lat && ns && lon && ew &&
          parseCoord(lat, fieldLen(lat), 2, ns[0], &la) &&
          parseCoord(lon, fieldLen(lon), 3, ew[0], &lo)) {
        f->ggaHasPos = true;
        f->ggaLatI = la;
        f->ggaLonI = lo;
      }
    }
    f->ggaHms = -1;
    if (t) {
      int32_t hms;
      int16_t ms;
      if (parseHmsStrict(t, fieldLen(t), &hms, &ms)) {
        f->ggaHms = hms;
      }
    }
    /* This GGA's own altitude (nmea.h): parseFixed's strict reading of the digits after an
     * optional '-', whole metres toward zero - the same number altM gives a well-formed field. */
    f->ggaAltM = -10000;
    if (a && fieldLen(a) > 0) {
      const int n = fieldLen(a);
      const bool neg = a[0] == '-';
      const int32_t v = parseFixed(a + (neg ? 1 : 0), n - (neg ? 1 : 0), 0);
      if (v >= 0) {
        f->ggaAltM = neg ? -v : v;
      }
    }
    f->lastKind = NMEA_LAST_GGA;
    return true;
  }

  return false;                         // valid sentence, not one we read
}

bool NmeaReader::feed(char c) {
  nBytes++;
  if (c == '$') {
    // A '$' ALWAYS restarts assembly — a truncated sentence must not eat the next one.
    inSentence = true;
    len = 0;
    return false;
  }
  if (!inSentence) {
    return false;                       // junk between sentences (incl. wake garbage)
  }
  if (c == '\r' || c == '\n') {
    inSentence = false;
    if (len < 4) {
      return false;                     // "x*hh" is the shortest conceivable body
    }
    line[len] = 0;
    // Checksum required: "...*hh" where hh = XOR of everything before '*'.
    if (len < 3 || line[len - 3] != '*') {
      nBadChecksum++;
      return false;
    }
    uint8_t want = 0;
    for (int i = 0; i < 2; i++) {
      char h = line[len - 2 + i];
      uint8_t v;
      if (h >= '0' && h <= '9') {
        v = h - '0';
      } else if (h >= 'A' && h <= 'F') {
        v = h - 'A' + 10;
      } else if (h >= 'a' && h <= 'f') {
        v = h - 'a' + 10;
      } else {
        nBadChecksum++;
        return false;
      }
      want = (uint8_t)(want << 4) | v;
    }
    uint8_t got = 0;
    for (int i = 0; i < len - 3; i++) {
      got ^= (uint8_t)line[i];
    }
    if (got != want) {
      nBadChecksum++;
      return false;
    }
    nSentences++;
    line[len - 3] = 0;                  // strip "*hh"; parse the body
    return parseSentence(line, &f);
  }
  if (len >= LINE_CAP - 1) {
    // Overlong: not NMEA. Discard and wait for the next '$'.
    inSentence = false;
    nOverrun++;
    return false;
  }
  line[len++] = c;
  return false;
}
