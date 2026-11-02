/*
 * mesh_pos.cpp — see mesh_pos.h. Proven by tests/test_pos.cpp on the host.
 */

#include "mesh_pos.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- protobuf

/* ⚠ A VARINT CAN BE TEN BYTES. protobuf encodes a NEGATIVE int32 sign-extended to 64 bits,
 * so `altitude = -50` (a GPS below sea level, or a bad geoid) or `timestamp_millis_adjust`
 * arrives as 0xff... x9 0x01. The shift used to run 0,7,...,63 and `<< 35` on a uint32_t is
 * undefined; on Xtensa the shift count masks to 5 bits, so it silently became `<< 3`. The
 * walk still advanced correctly (it follows the continuation bit, not the shift), and every
 * field this file reads is fixed32, so nothing was misread — but it was UB waiting for a
 * consumer. Bits past 32 are now discarded, which is the correct truncation for a uint32
 * reader. Found by UBSan the first time an upstream-generated vector carried a negative
 * int32 (2026-09-02). */
static uint32_t rdVarint(const uint8_t* d, size_t len, size_t* i) {
  uint32_t v = 0;
  int shift = 0;
  while (*i < len && (d[*i] & 0x80)) {
    if (shift < 32) v |= (uint32_t)(d[*i] & 0x7f) << shift;
    shift += 7;
    (*i)++;
  }
  if (*i < len) {
    if (shift < 32) v |= (uint32_t)(d[*i] & 0x7f) << shift;
    (*i)++;
  }
  return v;
}

static uint32_t rdFixed32(const uint8_t* d, size_t* i) {
  uint32_t v = (uint32_t)d[*i] | ((uint32_t)d[*i + 1] << 8) |
               ((uint32_t)d[*i + 2] << 16) | ((uint32_t)d[*i + 3] << 24);
  *i += 4;
  return v;
}

/* One walker for both messages: calls back per field, skips what it must.
 * Returns false on a malformed buffer (truncated fixed32/length overrun). */
typedef struct {
  // wire 0 fields land in vVarint, wire 5 in vFixed, wire 2 in ptr/len.
  void (*field)(void* ctx, uint32_t fieldNo, uint8_t wire,
                uint32_t vVarint, uint32_t vFixed, const uint8_t* p, uint32_t plen);
  void* ctx;
} PbWalk;

static bool pbWalk(const uint8_t* d, size_t len, const PbWalk* w) {
  size_t i = 0;
  while (i < len) {
    /* ⚠ The tag is a VARINT, not a byte. Field numbers >= 16 need two tag bytes
     * and REAL packets carry them: stock firmware appends precision_bits
     * (field 23; 22 is seq_number) to every channel Position broadcast, and
     * sats_in_view is field 19. A one-byte read misparses everything after such a field —
     * caught by the adversarial review before it met a real packet. */
    uint32_t tag = rdVarint(d, len, &i);
    uint32_t fieldNo = tag >> 3;
    uint8_t wire = (uint8_t)(tag & 7);
    if (wire == 0) {
      uint32_t v = rdVarint(d, len, &i);
      w->field(w->ctx, fieldNo, 0, v, 0, NULL, 0);
    } else if (wire == 5) {
      if (i + 4 > len) return false;
      uint32_t v = rdFixed32(d, &i);
      w->field(w->ctx, fieldNo, 5, 0, v, NULL, 0);
    } else if (wire == 2) {
      uint32_t l = rdVarint(d, len, &i);
      if (l > len - i) return false;
      w->field(w->ctx, fieldNo, 2, 0, 0, d + i, l);
      i += l;
    } else if (wire == 1) {
      if (i + 8 > len) return false;
      i += 8;
    } else {
      return false;                          // wire types 3/4/6/7: not protobuf we know
    }
  }
  return true;
}

// ---------------------------------------------------------------- Position

typedef struct {
  MeshPosMsg* out;
  bool hasLat, hasLon;
} PosCtx;

static void posField(void* vctx, uint32_t f, uint8_t wire,
                     uint32_t vv, uint32_t vf, const uint8_t* p, uint32_t plen) {
  (void)p; (void)plen;
  PosCtx* c = (PosCtx*)vctx;
  if (wire == 5) {
    if (f == 1)      { c->out->latI = (int32_t)vf; c->hasLat = true; }
    else if (f == 2) { c->out->lonI = (int32_t)vf; c->hasLon = true; }
    else if (f == 4) { c->out->time = vf; }
  } else if (wire == 0) {
    /* rdVarint keeps the low 32 bits, so an enum sent negative (sign-extended to ten bytes)
     * reads as a huge value here and is clamped like any other out-of-range one: it is not a
     * MESH_LOC_* and no caller compares it to one. */
    if (f == 5) {
      c->out->locSrc = (uint8_t)(vv > 255u ? 255u : vv);
    } else if (f == 23) {
      c->out->precBits = (uint8_t)(vv > MESH_POS_PREC_FULL ? MESH_POS_PREC_FULL : vv);
    }
  }
}

bool meshPosParseEx(const uint8_t* pl, size_t len, MeshPosMsg* out) {
  if (!out) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  if (!pl || len == 0) {
    return false;
  }
  PosCtx c = { out, false, false };
  PbWalk w = { posField, &c };
  return pbWalk(pl, len, &w) && c.hasLat && c.hasLon;
}

bool meshPosParse(const uint8_t* pl, size_t len,
                  int32_t* latI, int32_t* lonI, uint32_t* timeOut) {
  MeshPosMsg m;
  if (!meshPosParseEx(pl, len, &m)) {
    return false;
  }
  if (latI) *latI = m.latI;
  if (lonI) *lonI = m.lonI;
  if (timeOut) *timeOut = m.time;
  return true;
}

double meshPosPrecisionRadiusM(uint32_t precBits) {
  if (precBits == 0 || precBits >= MESH_POS_PREC_FULL) {
    return 0.0;
  }
  return 23905787.925008 / ldexp(1.0, (int)precBits);
}

bool meshPosIsApprox(uint32_t precBits) {
  return precBits >= 1 && precBits <= MESH_POS_APPROX_MAX_BITS;
}

int32_t meshPosTruncate(int32_t v, uint32_t p) {
  if (p == 0 || p >= MESH_POS_PREC_FULL) {
    return v;
  }
  /* On the unsigned bits, as the firmware does: a negative coordinate's cell is found in its
   * two's-complement form, so the cut moves it toward -infinity before the half-cell is added —
   * the same cell a stock node puts it in, which is the whole point. The add cannot carry out
   * of the kept bits: they end in (32-p) zeros and the half-cell is one bit below them. */
  uint32_t u = (uint32_t)v & (0xFFFFFFFFu << (32 - p));
  u += (uint32_t)1 << (31 - p);
  return (int32_t)u;
}

// ---------------------------------------------------------------- Waypoint

typedef struct {
  MeshWaypointMsg* out;
  bool hasLat, hasLon;
} WpCtx;

static void wpField(void* vctx, uint32_t f, uint8_t wire,
                    uint32_t vv, uint32_t vf, const uint8_t* p, uint32_t plen) {
  WpCtx* c = (WpCtx*)vctx;
  if (wire == 0) {
    if (f == 1)      c->out->id = vv;
    else if (f == 4) c->out->expire = vv;
    else if (f == 5) c->out->lockedTo = vv;
  } else if (wire == 5) {
    if (f == 2)      { c->out->latI = (int32_t)vf; c->hasLat = true; }
    else if (f == 3) { c->out->lonI = (int32_t)vf; c->hasLon = true; }
  } else if (wire == 2 && f == 6 && p) {
    size_t n = plen < MESH_WP_NAME_LEN - 1 ? plen : MESH_WP_NAME_LEN - 1;
    memcpy(c->out->name, p, n);
    c->out->name[n] = '\0';
  }
}

bool meshWaypointParse(const uint8_t* pl, size_t len, MeshWaypointMsg* out) {
  if (!pl || len == 0 || !out) return false;
  memset(out, 0, sizeof(*out));
  WpCtx c = { out, false, false };
  PbWalk w = { wpField, &c };
  if (!pbWalk(pl, len, &w)) return false;
  out->hasPos = c.hasLat && c.hasLon;
  /* A waypoint with an id and NO position is a DELETION — that absence is the
   * Meshtastic convention's marker (COVEY's waypoints.is_delete: position is
   * the discriminator, NOT expire, because expire == 0 legitimately means
   * "never expires"). So id-only payloads are VALID and the caller branches
   * on hasPos. Rejecting them here silently broke delete-on-COVEY ->
   * delete-on-phone until 2026-08-19. */
  return out->id != 0;
}

// ---------------------------------------------------------------- build

static size_t wrFixed32(uint8_t* out, uint8_t tag, uint32_t v) {
  out[0] = tag;
  out[1] = (uint8_t)(v >> 0);
  out[2] = (uint8_t)(v >> 8);
  out[3] = (uint8_t)(v >> 16);
  out[4] = (uint8_t)(v >> 24);
  return 5;
}

size_t meshPosBuild(uint8_t* out, int32_t latI, int32_t lonI, uint32_t time) {
  size_t d = 0;
  d += wrFixed32(out + d, 0x0d, (uint32_t)latI);   // field 1, sfixed32
  d += wrFixed32(out + d, 0x15, (uint32_t)lonI);   // field 2, sfixed32
  if (time) {
    d += wrFixed32(out + d, 0x25, time);           // field 4, fixed32
  }
  return d;
}

/* The varint body alone, at most five bytes for a uint32. */
static size_t wrRawVarint(uint8_t* out, uint32_t v) {
  size_t d = 0;
  while (v >= 0x80) {
    out[d++] = (uint8_t)(v | 0x80);
    v >>= 7;
  }
  out[d++] = (uint8_t)v;
  return d;
}

/* A protobuf varint. ⚠ Never called with a value that needs more than five bytes: every
 * field below is a uint32, and a uint32's varint is at most five bytes. ⚠ The tag is ONE byte
 * here, which only holds field numbers 1..15: field 16 and up goes through wrFieldVarint. */
static size_t wrVarint(uint8_t* out, uint8_t tag, uint32_t v) {
  out[0] = tag;
  return 1 + wrRawVarint(out + 1, v);
}

/* A varint field with its tag written as the varint it is: precision_bits is field 23, tag
 * (23 << 3) | 0 = 184 = 0xB8 0x01. Through the one-byte wrVarint, 0xB8 would go out alone —
 * and 0xB8 is not a whole tag (its top bit says another tag byte follows), so the receiver
 * would read the VALUE byte as the rest of the tag and misparse everything after it. */
static size_t wrFieldVarint(uint8_t* out, uint32_t fieldNo, uint32_t v) {
  size_t d = wrRawVarint(out, fieldNo << 3);     // wire type 0
  return d + wrRawVarint(out + d, v);
}

size_t meshPosBuildEx(uint8_t* out, size_t cap, int32_t latI, int32_t lonI, uint32_t time,
                      uint32_t locSrc, uint32_t precBits) {
  if (!out || locSrc > MESH_LOC_EXTERNAL || precBits > MESH_POS_PREC_FULL) {
    return 0;
  }
  if (precBits > 0 && precBits < MESH_POS_PREC_FULL) {
    latI = meshPosTruncate(latI, precBits);
    lonI = meshPosTruncate(lonI, precBits);
  }
  /* Measure first, write second (meshWaypointBuild's rule): a short buffer gets nothing,
   * never the first half of a position. */
  uint8_t tmp[MESH_POS_BUILD_EX_MAX];
  size_t d = 0;
  d += wrFixed32(tmp + d, 0x0d, (uint32_t)latI);         // field 1, sfixed32
  d += wrFixed32(tmp + d, 0x15, (uint32_t)lonI);         // field 2, sfixed32
  if (time) {
    d += wrFixed32(tmp + d, 0x25, time);                 // field 4, fixed32
  }
  if (locSrc) {
    d += wrFieldVarint(tmp + d, 5, locSrc);              // field 5, enum: 0x28 + one byte
  }
  if (precBits) {
    d += wrFieldVarint(tmp + d, 23, precBits);           // field 23: 0xB8 0x01 + one byte
  }
  if (d > cap) {
    return 0;
  }
  memcpy(out, tmp, d);
  return d;
}

size_t meshWaypointBuild(uint8_t* out, size_t cap, uint32_t id, bool hasPos,
                         int32_t latI, int32_t lonI, uint32_t expire, uint32_t lockedTo,
                         const char* name) {
  if (!out || id == 0) {
    return 0;
  }
  /* Measure first, write second. A builder that writes as it goes and discovers halfway
   * through that it does not fit has already scribbled on the caller's buffer, and the caller
   * has no way to know how much of it is now nonsense. */
  size_t nameLen = 0;
  if (name) {
    while (name[nameLen] && nameLen < MESH_WP_NAME_LEN - 1) {
      nameLen++;
    }
  }
  uint8_t tmp[MESH_WP_BUILD_MAX];
  size_t d = 0;
  d += wrVarint(tmp + d, 0x08, id);                       // field 1, varint
  if (hasPos) {
    d += wrFixed32(tmp + d, 0x15, (uint32_t)latI);        // field 2, sfixed32
    d += wrFixed32(tmp + d, 0x1d, (uint32_t)lonI);        // field 3, sfixed32
  }
  if (expire) {
    d += wrVarint(tmp + d, 0x20, expire);                 // field 4, varint
  }
  if (lockedTo) {
    d += wrVarint(tmp + d, 0x28, lockedTo);               // field 5, varint
  }
  if (nameLen) {
    tmp[d++] = 0x32;                                      // field 6, length-delimited
    tmp[d++] = (uint8_t)nameLen;                          // < 20, so always one length byte
    for (size_t i = 0; i < nameLen; i++) {
      tmp[d++] = (uint8_t)name[i];
    }
  }
  if (d > cap) {
    return 0;
  }
  memcpy(out, tmp, d);
  return d;
}

// ---------------------------------------------------------------- geometry

#define DEG(i)  ((double)(i) * 1e-7)
#define RAD(x)  ((x) * (M_PI / 180.0))

double meshPosDistanceM(int32_t aLatI, int32_t aLonI, int32_t bLatI, int32_t bLonI) {
  double p1 = RAD(DEG(aLatI)), p2 = RAD(DEG(bLatI));
  double dp = p2 - p1;
  double dl = RAD(DEG(bLonI) - DEG(aLonI));
  double s1 = sin(dp / 2), s2 = sin(dl / 2);
  double a = s1 * s1 + cos(p1) * cos(p2) * s2 * s2;
  return 6371000.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

int meshPosBearingDeg(int32_t aLatI, int32_t aLonI, int32_t bLatI, int32_t bLonI) {
  double p1 = RAD(DEG(aLatI)), p2 = RAD(DEG(bLatI));
  double dl = RAD(DEG(bLonI) - DEG(aLonI));
  double y = sin(dl) * cos(p2);
  double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
  double deg = atan2(y, x) * (180.0 / M_PI);
  int b = (int)lround(deg);
  return ((b % 360) + 360) % 360;
}

const char* meshPosCompass8(int deg) {
  static const char* PTS[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  return PTS[((deg + 22) / 45) & 7];
}

void meshPosFmtDist(double m, char* out, size_t cap) {
  if (m < 0) m = 0;
  if (m < 999.5) {                 // 999.6 must become "1.0km", never "1000m"
    snprintf(out, cap, "%dm", (int)lround(m));
  } else if (m < 10000.0) {
    snprintf(out, cap, "%.1fkm", m / 1000.0);
  } else {
    snprintf(out, cap, "%.0fkm", m / 1000.0);
  }
}

// ---------------------------------------------------------------- beacon gate

bool meshPosFixUsable(int sats, int hdopX10) {
  if (sats >= 0 && sats < MESH_POS_MIN_SATS) {
    return false;                 // 2D fix: the error goes sideways. See the note in the header.
  }
  if (hdopX10 >= 0 && hdopX10 > MESH_POS_MAX_HDOP_X10) {
    return false;
  }
  return true;                    // includes "the receiver did not say" — see the header
}

bool meshPosShouldBeacon(bool haveLast, int32_t lastLatI, int32_t lastLonI,
                         int32_t latI, int32_t lonI, int skipRuns) {
  /* Nothing has gone out since boot: transmit. Suppressing the FIRST beacon
   * would leave a node that has never announced its position looking exactly
   * like one that is switched off. */
  if (!haveLast) {
    return true;
  }
  /* The keep-alive floor, checked BEFORE the distance so it can never be
   * out-voted: a phone parked all afternoon still says so every fourth slot. */
  if (skipRuns >= MESH_POS_MAX_SKIPS) {
    return true;
  }
  return meshPosDistanceM(lastLatI, lastLonI, latI, lonI) >= MESH_POS_MIN_MOVE_M;
}
