/*
 * booksync.cpp — see booksync.h. A port of COVEY's booksync.py; the Python is the spec.
 *
 * Every place this file looks fussier than it needs to be, it is matching a specific
 * behaviour of the Python that a receiver depends on. Those spots are commented with WHY,
 * because "simplify it" is the natural instinct and each one would break sync silently.
 */
#include "booksync.h"
#include "book_hash.h"
#include <math.h>          // ceil(), for the fraction quantiser
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- key + canonical
void bookSyncDeriveKey(const char* passcode, uint8_t key[32]) {
  const char* pw = passcode ? passcode : "";
  size_t saltLen = strlen(BOOKSYNC_KDF_SALT);
  size_t pwLen = strlen(pw);
  // Hash the concatenation without building it: the salt is a literal and the passcode may
  // be any length, and there is no reason to bound a buffer we do not need.
  BsSha256 c;
  bsSha256Init(&c);
  bsSha256Update(&c, BOOKSYNC_KDF_SALT, saltLen);
  bsSha256Update(&c, pw, pwLen);
  bsSha256Final(&c, key);
}

void bookSyncMakeRecord(BookSyncRecord* r,
                        const char* const* ids, int nIds,
                        uint32_t spine, uint32_t offset, double fraction,
                        uint32_t turnedAt, const char* device, const char* nonce) {
  memset(r, 0, sizeof(*r));
  r->version = BOOKSYNC_VERSION;
  if (nIds > BOOKSYNC_MAX_IDS) {
    nIds = BOOKSYNC_MAX_IDS;
  }
  for (int i = 0; i < nIds; i++) {
    const char* s = ids[i] ? ids[i] : "";
    strncpy(r->ids[i], s, BOOKSYNC_ID_MAX - 1);
    r->ids[i][BOOKSYNC_ID_MAX - 1] = '\0';
  }
  r->nIds = (uint8_t)(nIds < 0 ? 0 : nIds);
  r->spine = spine;
  r->offset = offset;
  /* Python's make_record stores round(float(fraction), 6) — and that rounding is NOT
   * cosmetic, which is the mistake this line originally made.
   *
   * The 6dp value is what reduce_for_mesh later quantises to 1/65535ths, so rounding first
   * or not changes the integer on the wire. int(round(1/3, 6) * 65535) is 21844; without
   * the rounding, 1/3 * 65535 is exactly 21845.0 and you get 21845 — a packet COVEY drops
   * in silence. The test vectors "frac-third" and "frac-tiny" exist to catch exactly this.
   *
   * snprintf + strtod is the faithful way to do it: both it and Python's round() are
   * correctly rounded with ties to even, whereas round(x * 1e6) / 1e6 would round halves
   * away from zero. It also guarantees the stored value agrees with what the canonical
   * string prints, since that uses the same "%.6f".
   *
   * No clamping here: Python does not clamp in make_record either (reduce_for_mesh does),
   * and clamping would change the canonical string for an out-of-range fraction. */
  char frBuf[32];
  snprintf(frBuf, sizeof(frBuf), "%.6f", fraction);
  r->fraction = strtod(frBuf, NULL);
  r->turnedAt = turnedAt;
  bsUtf8TruncChars(r->dev, sizeof(r->dev), device ? device : "", 16);
  if (nonce && strlen(nonce) == BOOKSYNC_NONCE_CHARS) {
    memcpy(r->nonce, nonce, BOOKSYNC_NONCE_CHARS);
    r->nonce[BOOKSYNC_NONCE_CHARS] = '\0';
  } else {
    static const char* kHexDigits = "0123456789abcdef";  // not HEX: Arduino's Print.h defines that as 16
    for (int i = 0; i < BOOKSYNC_NONCE_CHARS; i++) {
      r->nonce[i] = kHexDigits[rand() & 0xF];
    }
    r->nonce[BOOKSYNC_NONCE_CHARS] = '\0';
  }
}

size_t bookSyncCanonical(const BookSyncRecord* r, char* out, size_t cap) {
  /* "v|ids,joined|sp|off|fr(6dp)|t|dev|n"
   *
   * The fraction is printed with exactly 6 decimal places because Python writes
   * "%.6f" % fr. Any other precision changes the signed bytes. */
  char frac[32];
  snprintf(frac, sizeof(frac), "%.6f", r->fraction);

  size_t n = 0;
  int w = snprintf(out, cap, "%u|", (unsigned)r->version);
  if (w < 0 || (size_t)w >= cap) {
    return 0;
  }
  n = (size_t)w;
  for (int i = 0; i < r->nIds; i++) {
    w = snprintf(out + n, cap - n, "%s%s", (i ? "," : ""), r->ids[i]);
    if (w < 0 || (size_t)w >= cap - n) {
      return 0;
    }
    n += (size_t)w;
  }
  w = snprintf(out + n, cap - n, "|%lu|%lu|%s|%lu|%s|%s",
               (unsigned long)r->spine, (unsigned long)r->offset, frac,
               (unsigned long)r->turnedAt, r->dev, r->nonce);
  if (w < 0 || (size_t)w >= cap - n) {
    return 0;
  }
  return n + (size_t)w;
}

bool bookSyncSign(const BookSyncRecord* r, const uint8_t key[32], char* out) {
  char canon[BOOKSYNC_CANON_MAX];
  size_t len = bookSyncCanonical(r, canon, sizeof(canon));
  if (!len) {
    return false;
  }
  uint8_t mac[32];
  bsHmacSha256(key, 32, canon, len, mac);
  return bsBase32Encode(mac, BOOKSYNC_MAC_LEN, out, BOOKSYNC_MAC_CHARS + 1) > 0;
}

// ---------------------------------------------------------------- mesh framing
void bookSyncReduceForMesh(const BookSyncRecord* in, BookSyncRecord* out, uint16_t* frQOut) {
  memset(out, 0, sizeof(*out));
  out->version = BOOKSYNC_VERSION;
  // ONLY the first id travels. It is the best one (see epub ids), and airtime is the budget.
  bsUtf8TruncBytes(out->ids[0], BOOKSYNC_ID_MAX,
                 in->nIds > 0 ? in->ids[0] : "", BOOKSYNC_MESH_ID_BYTES);
  out->nIds = 1;
  out->spine = in->spine > 65535u ? 65535u : in->spine;
  out->offset = in->offset;                       // already u32; Python clamps to the same
  out->turnedAt = in->turnedAt;

  double f = in->fraction;
  if (f < 0.0) f = 0.0;
  if (f > 1.0) f = 1.0;
  /* 🛑 ROUND UP, NEVER DOWN — and this must stay identical to COVEY's `math.ceil` in
   * reduce_for_mesh(). Chapter starts fall at arbitrary real fractions, so truncating put the
   * dequantised value BELOW the boundary essentially every time, and a fraction a hair under
   * a chapter start locates to the END OF THE PREVIOUS CHAPTER — epubLocate is the only thing
   * applyPending() uses. Syncing from the top of a chapter, which is exactly where a contents
   * jump or the last page turn of a chapter leaves you, sent the reader a chapter backwards.
   * Rounding to nearest only makes it a coin toss; rounding UP keeps a chapter start inside
   * its own chapter, at a cost of at most 1/65535 of a book forward — a few characters, in
   * the harmless direction.
   *
   * ⚠ This changes the bytes a SENDER puts on the air but NOT interoperability: a receiver
   * verifies the mac over the reduced record it rebuilds from the packet and never
   * re-quantises anything of its own, so old and new builds talk both ways. What it does
   * change is the differential vectors — tests/vectors_booksync.h is regenerated from
   * COVEY's module, and the two expressions have to move together or the suite says so. */
  double scaled = f * 65535.0;
  uint16_t q = (uint16_t)ceil(scaled);
  out->fraction = (double)q / 65535.0;
  if (frQOut) {
    *frQOut = q;
  }

  bsUtf8TruncBytes(out->dev, BOOKSYNC_DEV_MAX, in->dev, BOOKSYNC_MESH_DEV_BYTES);
  // Python right-justifies a short nonce with '0' to 8 characters.
  size_t nl = strlen(in->nonce);
  if (nl >= BOOKSYNC_NONCE_CHARS) {
    memcpy(out->nonce, in->nonce, BOOKSYNC_NONCE_CHARS);
  } else {
    size_t pad = BOOKSYNC_NONCE_CHARS - nl;
    memset(out->nonce, '0', pad);
    memcpy(out->nonce + pad, in->nonce, nl);
  }
  out->nonce[BOOKSYNC_NONCE_CHARS] = '\0';
}

static int hexVal(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool bookSyncPackMesh(const BookSyncRecord* r, const uint8_t key[32], char* out, size_t cap) {
  BookSyncRecord red;
  uint16_t frQ = 0;
  bookSyncReduceForMesh(r, &red, &frQ);

  // struct.pack(">BHIIH", version, spine, offset, turnedAt, fractionQuantised)
  uint8_t body[BOOKSYNC_MESH_ID_BYTES + BOOKSYNC_MESH_DEV_BYTES + 32];
  size_t b = 0;
  body[b++] = BOOKSYNC_VERSION;
  body[b++] = (uint8_t)(red.spine >> 8);
  body[b++] = (uint8_t)(red.spine);
  body[b++] = (uint8_t)(red.offset >> 24);
  body[b++] = (uint8_t)(red.offset >> 16);
  body[b++] = (uint8_t)(red.offset >> 8);
  body[b++] = (uint8_t)(red.offset);
  body[b++] = (uint8_t)(red.turnedAt >> 24);
  body[b++] = (uint8_t)(red.turnedAt >> 16);
  body[b++] = (uint8_t)(red.turnedAt >> 8);
  body[b++] = (uint8_t)(red.turnedAt);
  body[b++] = (uint8_t)(frQ >> 8);
  body[b++] = (uint8_t)(frQ);

  size_t devLen = strlen(red.dev);
  body[b++] = (uint8_t)devLen;
  memcpy(body + b, red.dev, devLen);
  b += devLen;

  size_t idLen = strlen(red.ids[0]);
  body[b++] = (uint8_t)idLen;
  memcpy(body + b, red.ids[0], idLen);
  b += idLen;

  for (int i = 0; i < 4; i++) {                   // bytes.fromhex(nonce)
    int hi = hexVal(red.nonce[i * 2]);
    int lo = hexVal(red.nonce[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return false;                               // a non-hex nonce cannot be packed
    }
    body[b++] = (uint8_t)((hi << 4) | lo);
  }

  char payload[BOOKSYNC_MESH_TEXT_MAX];
  if (!bsBase32Encode(body, b, payload, sizeof(payload))) {
    return false;
  }
  char mac[BOOKSYNC_MAC_CHARS + 1];
  if (!bookSyncSign(&red, key, mac)) {            // ⚠ signed over the REDUCED record
    return false;
  }
  int w = snprintf(out, cap, "%s %s %s", BOOKSYNC_MESH_PREFIX, payload, mac);
  return w > 0 && (size_t)w < cap;
}

bool bookSyncUnpackMesh(const char* text, const uint8_t key[32], BookSyncRecord* out) {
  if (!bookSyncIsSyncText(text)) {
    return false;
  }
  /* Python does text.split(), which splits on ANY whitespace run and needs >= 3 fields;
   * anything past the third is ignored. Match that, so a packet a Python sender would
   * accept is not rejected here. */
  const char* tok[3] = {NULL, NULL, NULL};
  size_t tokLen[3] = {0, 0, 0};
  int nTok = 0;
  const char* p = text;
  while (*p && nTok < 3) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      p++;
    }
    if (!*p) {
      break;
    }
    const char* s = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
      p++;
    }
    tok[nTok] = s;
    tokLen[nTok] = (size_t)(p - s);
    nTok++;
  }
  if (nTok < 3) {
    return false;
  }
  if (tokLen[1] >= BOOKSYNC_MESH_TEXT_MAX || tokLen[2] != BOOKSYNC_MAC_CHARS) {
    return false;
  }

  uint8_t raw[BOOKSYNC_MESH_TEXT_MAX];
  int rawLen = bsBase32Decode(tok[1], tokLen[1], raw, sizeof(raw));
  if (rawLen < 13) {                              // shorter than the fixed header
    return false;
  }
  size_t at = 0;
  uint8_t ver = raw[at++];
  uint32_t sp = ((uint32_t)raw[at] << 8) | raw[at + 1];             at += 2;
  uint32_t off = ((uint32_t)raw[at] << 24) | ((uint32_t)raw[at + 1] << 16) |
                 ((uint32_t)raw[at + 2] << 8) | raw[at + 3];        at += 4;
  uint32_t t = ((uint32_t)raw[at] << 24) | ((uint32_t)raw[at + 1] << 16) |
               ((uint32_t)raw[at + 2] << 8) | raw[at + 3];          at += 4;
  uint32_t frQ = ((uint32_t)raw[at] << 8) | raw[at + 1];            at += 2;
  if (ver != BOOKSYNC_VERSION) {
    return false;
  }
  if (at >= (size_t)rawLen) {
    return false;
  }
  size_t devLen = raw[at++];
  if (at + devLen > (size_t)rawLen || devLen >= BOOKSYNC_DEV_MAX) {
    return false;
  }
  char dev[BOOKSYNC_DEV_MAX];
  memcpy(dev, raw + at, devLen);
  dev[devLen] = '\0';
  at += devLen;

  if (at >= (size_t)rawLen) {
    return false;
  }
  size_t idLen = raw[at++];
  if (at + idLen > (size_t)rawLen || idLen >= BOOKSYNC_ID_MAX) {
    return false;
  }
  char id[BOOKSYNC_ID_MAX];
  memcpy(id, raw + at, idLen);
  id[idLen] = '\0';
  at += idLen;

  if (at + 4 > (size_t)rawLen) {
    return false;
  }
  char nonce[BOOKSYNC_NONCE_CHARS + 1];
  static const char* kHexDigits = "0123456789abcdef";  // not HEX: Arduino's Print.h defines that as 16
  for (int i = 0; i < 4; i++) {
    nonce[i * 2]     = kHexDigits[(raw[at + i] >> 4) & 0xF];
    nonce[i * 2 + 1] = kHexDigits[raw[at + i] & 0xF];
  }
  nonce[BOOKSYNC_NONCE_CHARS] = '\0';

  BookSyncRecord r;
  memset(&r, 0, sizeof(r));
  r.version = ver;
  strncpy(r.ids[0], id, BOOKSYNC_ID_MAX - 1);
  r.nIds = 1;
  r.spine = sp;
  r.offset = off;
  r.fraction = (double)frQ / 65535.0;
  r.turnedAt = t;
  strncpy(r.dev, dev, BOOKSYNC_DEV_MAX - 1);
  memcpy(r.nonce, nonce, sizeof(nonce));

  /* The rebuilt record IS the reduced record, so signing it directly is correct — that is
   * the whole point of reduce_for_mesh being what gets signed. */
  char expect[BOOKSYNC_MAC_CHARS + 1];
  if (!bookSyncSign(&r, key, expect)) {
    return false;
  }
  char got[BOOKSYNC_MAC_CHARS + 1];
  memcpy(got, tok[2], BOOKSYNC_MAC_CHARS);
  got[BOOKSYNC_MAC_CHARS] = '\0';
  if (!bsConstTimeEqual(expect, got)) {
    return false;                                 // wrong passcode: drop, say nothing
  }
  if (out) {
    *out = r;
  }
  return true;
}

bool bookSyncIsSyncText(const char* text) {
  if (!text) {
    return false;
  }
  // Prefix AND the separating space, matching Python's startswith(MESH_PREFIX + " ").
  static const char* PFX = BOOKSYNC_MESH_PREFIX " ";
  return strncmp(text, PFX, strlen(PFX)) == 0;
}

// ---------------------------------------------------------------- matching
/* 🛑 COMPARE LIKE WITH LIKE. Only the FIRST id travels, and reduce_for_mesh cuts it to
 * BOOKSYNC_MESH_ID_BYTES (40) — but the ids being generated run to 67 bytes ("ta:" or "id:"
 * plus a 64-character slug, EPUB_ID_MAX 72). A plain strcmp of a 40-byte wire id against a
 * 67-byte local one can never be equal, so for any book with a long id sync silently did
 * NOTHING, for ever, on both ends. ⚠ That is the worst possible presentation, because it is
 * indistinguishable from a wrong passcode: no card, no message, nothing to tell you which.
 *
 * So truncate BOTH sides the same way before comparing. An already-cut id truncates to
 * itself, so this costs nothing for short ids and needs no change to the wire — old and new
 * builds interoperate either way.
 *
 * ⚠ It does mean two books whose first 40 bytes agree are treated as one. That collision is
 * already on the wire and cannot be fixed from this end: the sender only ever transmits those
 * 40 bytes. Matching is strictly better than never matching. */
static bool bsIdEquivalent(const char* x, const char* y) {
  if (strcmp(x, y) == 0) {
    return true;
  }
  char cx[BOOKSYNC_ID_MAX], cy[BOOKSYNC_ID_MAX];
  bsUtf8TruncBytes(cx, sizeof(cx), x, BOOKSYNC_MESH_ID_BYTES);
  bsUtf8TruncBytes(cy, sizeof(cy), y, BOOKSYNC_MESH_ID_BYTES);
  return cx[0] && strcmp(cx, cy) == 0;
}

bool bookSyncIdsMatch(const char* const* a, int na, const char* const* b, int nb) {
  for (int i = 0; i < na; i++) {
    if (!a[i] || !a[i][0]) {
      continue;
    }
    for (int j = 0; j < nb; j++) {
      if (b[j] && bsIdEquivalent(a[i], b[j])) {
        return true;
      }
    }
  }
  return false;
}

bool bookSyncRecordMatchesIds(const BookSyncRecord* r, const char* const* ids, int nIds) {
  const char* mine[BOOKSYNC_MAX_IDS];
  for (int i = 0; i < r->nIds; i++) {
    mine[i] = r->ids[i];
  }
  return bookSyncIdsMatch(mine, r->nIds, ids, nIds);
}

bool bookSyncSuspectClock(const BookSyncRecord* r, uint32_t nowUnix) {
  return r->turnedAt > nowUnix + 300u;
}
