/*
 * map_pins.cpp — see map_pins.h. Proven by tests/test_maptiles.cpp on the host.
 */

#include "map_pins.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* strtol with the whole answer checked, because the pins file is hand-editable and a partial
 * parse is how "47.3" becomes latitude 47 nanodegrees without anybody noticing. Accepts an
 * optional sign and at least one digit, and the field must end exactly where it is expected. */
static int parseI32(const char* s, const char* end, int32_t* out) {
  if (s >= end) {
    return 0;
  }
  int neg = 0;
  const char* p = s;
  if (*p == '+' || *p == '-') {
    neg = (*p == '-');
    p++;
  }
  if (p >= end) {
    return 0;
  }
  int64_t v = 0;
  for (; p < end; p++) {
    if (*p < '0' || *p > '9') {
      return 0;
    }
    v = v * 10 + (*p - '0');
    if (v > 2147483648LL) {
      return 0;                      // out of int32 range: refuse rather than fold
    }
  }
  if (neg) {
    v = -v;
  }
  if (v > 2147483647LL || v < -2147483648LL) {
    return 0;
  }
  *out = (int32_t)v;
  return 1;
}

static int parseU32(const char* s, const char* end, uint32_t* out) {
  if (s >= end) {
    return 0;
  }
  uint64_t v = 0;
  for (const char* p = s; p < end; p++) {
    if (*p < '0' || *p > '9') {
      return 0;
    }
    v = v * 10 + (uint64_t)(*p - '0');
    if (v > 4294967295ULL) {
      return 0;
    }
  }
  *out = (uint32_t)v;
  return 1;
}

int mapPinParseLine(const char* line, MapPin* out) {
  if (!line || !out) {
    return -1;
  }
  // Skip leading blanks; a blank or '#' line is not an error.
  const char* p = line;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') {
    return 0;
  }

  // Find the end of the line (the file reader may or may not have stripped the newline).
  const char* lineEnd = p;
  while (*lineEnd && *lineEnd != '\n' && *lineEnd != '\r') {
    lineEnd++;
  }

  const char* c1 = (const char*)memchr(p, ',', (size_t)(lineEnd - p));
  if (!c1) {
    return -1;
  }
  const char* c2 = (const char*)memchr(c1 + 1, ',', (size_t)(lineEnd - c1 - 1));
  if (!c2) {
    return -1;
  }
  const char* c3 = (const char*)memchr(c2 + 1, ',', (size_t)(lineEnd - c2 - 1));
  if (!c3) {
    return -1;
  }

  MapPin pin;
  memset(&pin, 0, sizeof(pin));
  if (!parseI32(p, c1, &pin.latI) || !parseI32(c1 + 1, c2, &pin.lonI) ||
      !parseU32(c2 + 1, c3, &pin.sharedId)) {
    return -1;
  }
  /* ⚠ A latitude outside +/-90 or a longitude outside +/-180 is not a pin, it is a corrupt
   * line, and accepting it would put a marker at a place Mercator has to invent. */
  if (pin.latI >  900000000 || pin.latI <  -900000000 ||
      pin.lonI > 1800000000 || pin.lonI < -1800000000) {
    return -1;
  }

  /* Five fields when there is a fourth comma: the channel sits between the id and the name.
   * The name is the rest of the line either way (it may hold spaces, never a comma). */
  const char* nameStart = c3 + 1;
  const char* c4 = (const char*)memchr(c3 + 1, ',', (size_t)(lineEnd - c3 - 1));
  if (c4) {
    size_t chanLen = (size_t)(c4 - (c3 + 1));
    if (chanLen >= sizeof(pin.chan)) {
      chanLen = sizeof(pin.chan) - 1;
    }
    for (size_t i = 0; i < chanLen; i++) {
      const unsigned char ch = (unsigned char)c3[1 + i];
      pin.chan[i] = (ch < 0x20) ? ' ' : (char)ch;    // a control character is not a name
    }
    pin.chan[chanLen] = '\0';
    nameStart = c4 + 1;
  }

  char raw[MAP_PIN_LINE_MAX];
  size_t nameLen = (size_t)(lineEnd - nameStart);
  if (nameLen >= sizeof(raw)) {
    nameLen = sizeof(raw) - 1;
  }
  memcpy(raw, nameStart, nameLen);
  raw[nameLen] = '\0';
  mapPinSanitizeName(raw, pin.name, sizeof(pin.name));

  *out = pin;
  return 1;
}

int mapPinFormatLine(const MapPin* p, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (!p) {
    return 0;
  }
  char safe[MAP_PIN_NAME_LEN];
  mapPinSanitizeName(p->name, safe, sizeof(safe));
  /* The channel field may not carry the separator either; a comma in a channel name (none
   * exist, but a card is hand-editable) becomes a space rather than a fifth field. */
  char chan[MAP_PIN_CHAN_LEN];
  size_t k = 0;
  for (const char* q = p->chan; *q && k + 1 < sizeof(chan); q++) {
    chan[k++] = (*q == ',' || (unsigned char)*q < 0x20) ? ' ' : *q;
  }
  chan[k] = '\0';
  const int n = snprintf(out, cap, "%ld,%ld,%lu,%s,%s",
                         (long)p->latI, (long)p->lonI, (unsigned long)p->sharedId, chan, safe);
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return n;
}

void mapPinSanitizeName(const char* in, char* out, size_t cap) {
  if (!out || cap == 0) {
    return;
  }
  out[0] = '\0';
  if (!in) {
    strncpy(out, "Pin", cap - 1);
    out[cap - 1] = '\0';
    return;
  }
  size_t w = 0;
  const char* p = in;
  for (; *p && w + 1 < cap; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == ',') {
      c = ' ';                        // the separator cannot live inside a field
    }
    if (c < 0x20 || c == 0x7f) {
      c = ' ';                        // newlines, tabs and stray control bytes
    }
    if (c == ' ' && w == 0) {
      continue;                       // no leading spaces
    }
    out[w++] = (char)c;
  }
  const bool truncated = (*p != '\0');   // we stopped because the buffer ran out, not the string
  /* ⚠ IF THE CUT WAS THE BUFFER'S AND NOT THE STRING'S, BACK OFF ANY HALF-FINISHED UTF-8
   * SEQUENCE. These bytes go two places that will not forgive them: the pins file, and
   * (via meshWaypointBuild) a protobuf string field on the air, where a truncated sequence is
   * invalid UTF-8 and a strict reader may reject the whole Waypoint. A name that loses its
   * last accented letter is a name; a name that ends mid-codepoint is a bug on somebody
   * else's map. Only a CONTINUATION byte (10xxxxxx) can be a tail, and a lead byte says how
   * many follow, so this is exact rather than heuristic. */
  if (truncated) {
    while (w > 0 && ((unsigned char)out[w - 1] & 0xC0) == 0x80) {
      w--;                            // walk back over continuation bytes
    }
    if (w > 0) {
      const unsigned char lead = (unsigned char)out[w - 1];
      size_t need = 0;
      if ((lead & 0xE0) == 0xC0) {
        need = 1;
      } else if ((lead & 0xF0) == 0xE0) {
        need = 2;
      } else if ((lead & 0xF8) == 0xF0) {
        need = 3;
      }
      if (need) {
        w--;                          // its continuations were cut off: drop the lead too
      }
    }
  }
  while (w > 0 && out[w - 1] == ' ') {
    w--;                              // no trailing spaces
  }
  out[w] = '\0';
  if (w == 0) {
    strncpy(out, "Pin", cap - 1);
    out[cap - 1] = '\0';
  }
}

int mapPinAutoName(const MapPin* pins, int n, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  for (int want = 1; want <= n + 1; want++) {
    char cand[MAP_PIN_NAME_LEN];
    snprintf(cand, sizeof(cand), "Pin %d", want);
    int taken = 0;
    for (int i = 0; i < n && pins; i++) {
      if (!strcmp(pins[i].name, cand)) {
        taken = 1;
        break;
      }
    }
    if (!taken) {
      strncpy(out, cand, cap - 1);
      out[cap - 1] = '\0';
      return want;
    }
  }
  strncpy(out, "Pin", cap - 1);
  out[cap - 1] = '\0';
  return 0;
}

int mapPinPickNearest(const int* vx, const int* vy, int n, int px, int py, int maxPx) {
  if (!vx || !vy || n <= 0 || maxPx < 0) {
    return -1;
  }
  const long limit = (long)maxPx * (long)maxPx;
  int best = -1;
  long bestD = 0;
  for (int i = 0; i < n; i++) {
    const long dx = (long)vx[i] - px;
    const long dy = (long)vy[i] - py;
    /* 🛑 REJECT ON EACH AXIS BEFORE SQUARING. `long` is 32 bits on this chip, and these are
     * viewport coordinates projected from world pixels: mapLatLonToView writes through for
     * points that are nowhere near the screen, and at z19 the world is 134 million pixels
     * across. 134e6 squared is 1.8e16, which wraps — and a wrapped product can come out
     * NEGATIVE, which beats every real distance, so a pin on the far side of the world would
     * be picked as the one "under the crosshair". The axis test costs two comparisons, is a
     * strict superset of the radius test, and cannot overflow: after it, |dx| <= maxPx. */
    if (dx > maxPx || dx < -maxPx || dy > maxPx || dy < -maxPx) {
      continue;
    }
    const long d = dx * dx + dy * dy;
    if (d > limit) {
      continue;
    }
    if (best < 0 || d < bestD) {      // strictly less: ties keep the lower index
      best = i;
      bestD = d;
    }
  }
  return best;
}

/* See map_pins.h. The outcome numbers are MeshTxOutcome's (mesh_txq.h); spelled as literals
 * here so this file stays free of the mesh headers, and pinned by tests/test_maptiles.cpp. */
uint8_t mapPinMeshSettle(uint8_t op, uint8_t outcome, bool final) {
  const uint8_t QUEUED = 1, SENT = 2, FAILED = 3;
  if (outcome == QUEUED && !final) {
    return MAP_PINACT_WAIT;
  }
  if (outcome == SENT) {
    return op == MAP_PINOP_UNSHARE ? MAP_PINACT_FORGET_ID : MAP_PINACT_DONE;
  }
  if (outcome == FAILED) {
    switch (op) {
      case MAP_PINOP_SHARE:  return MAP_PINACT_ROLL_BACK;
      case MAP_PINOP_DELETE: return MAP_PINACT_RESTORE;
      default:               return MAP_PINACT_KEEP_ID;    // re-share, unshare: the mesh still has it
    }
  }
  /* No word (UNKNOWN, or QUEUED with no more waiting). */
  return op == MAP_PINOP_DELETE ? MAP_PINACT_DONE : MAP_PINACT_KEEP_ID;
}

/* See map_pins.h. QUEUED is spelled as mapPinMeshSettle spells it (pinned by test_maptiles). */
bool mapPinRetractionQueued(uint8_t op, uint8_t outcome, uint32_t opWpId, uint32_t pinWpId) {
  const uint8_t QUEUED = 1;
  return (op == MAP_PINOP_UNSHARE || op == MAP_PINOP_DELETE) && outcome == QUEUED &&
         opWpId != 0 && opWpId == pinWpId;
}
