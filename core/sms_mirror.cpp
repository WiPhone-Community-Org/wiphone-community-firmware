/*
 * sms_mirror.cpp — the CSM1 record, packed and parsed.
 *
 * Deliberately free of Arduino headers so tests/test_sms_mirror.cpp can compile it with
 * the Mac's compiler. That constraint is what makes this testable at all; a link error
 * here means something crept in that should not have.
 */
#include "sms_mirror.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bumped whenever a message did not fit its buffer. Exposed rather than logged from here:
 * this file is host-testable and has no logger, and the ingest side is where a count belongs
 * anyway — one line saying "3 messages arrived truncated" beats three identical warnings. */
unsigned smsMirrorTruncations = 0;

// Bounded strlen. The records come off a radio and out of a socket; nothing here may
// assume a NUL is where it should be.
static size_t smLen(const char* s, size_t cap) {
  size_t n = 0;
  while (n < cap && s[n]) {
    n++;
  }
  return n;
}

size_t smsMirrorDigits(const char* s, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (!s) {
    return 0;
  }

  char buf[SMS_MIRROR_PEER_MAX];
  size_t n = 0;
  for (const char* p = s; *p && *p != '@'; p++) {   // a SIP URI's host is not identity
    if (*p < '0' || *p > '9') {
      continue;
    }
    if (n >= sizeof(buf) - 1) {
      /* More digits than any phone number has. Truncating here would INVENT an identity
       * that could collide with a real correspondent, so refuse the whole thing. */
      return 0;
    }
    buf[n++] = *p;
  }
  buf[n] = '\0';

  const char* d = buf;
  if (n == 11 && buf[0] == '1') {       // 1-425-555-0142 and 425-555-0142 are one person
    d++;
    n--;
  }

  if (n >= cap) {
    n = cap - 1;
  }
  memcpy(out, d, n);
  out[n] = '\0';
  return n;
}

bool smsMirrorIsMirrorText(const char* text) {
  if (!text) {
    return false;
  }
  // Prefix AND the separating space, so a channel message that merely starts "CSM1..."
  // is not swallowed.
  static const char* PFX = SMS_MIRROR_PREFIX " ";
  return strncmp(text, PFX, strlen(PFX)) == 0;
}

// ---------------------------------------------------------------- escaping
/* Returns the number of characters written, or 0 if it would not fit. See the header for
 * why this exists: one raw newline in a last-position field destroys two records. */
static size_t smEscape(const char* in, size_t inLen, char* out, size_t cap) {
  size_t w = 0;
  for (size_t i = 0; i < inLen; i++) {
    char c = in[i];
    const char* esc = NULL;
    if (c == '\\') {
      esc = "\\\\";
    } else if (c == '\n') {
      esc = "\\n";
    } else if (c == '\r') {
      esc = "\\r";
    }
    if (esc) {
      if (w + 2 >= cap) {
        return 0;
      }
      out[w++] = esc[0];
      out[w++] = esc[1];
    } else {
      if (w + 1 >= cap) {
        return 0;
      }
      out[w++] = c;
    }
  }
  if (w >= cap) {
    return 0;
  }
  out[w] = '\0';
  return w;
}

/* Lenient by design: an unknown escape keeps BOTH characters rather than dropping them.
 * A sender we do not understand should cost us one odd-looking message, not a silently
 * mangled one. */
// Returns false if the output did not fit — the caller must not let that pass unremarked.
static bool smUnescape(const char* in, char* out, size_t cap) {
  size_t w = 0;
  const char* p = in;
  for (; *p && w + 1 < cap; p++) {
    if (*p != '\\') {
      out[w++] = *p;
      continue;
    }
    char nxt = p[1];
    if (nxt == 'n') {
      out[w++] = '\n';
      p++;
    } else if (nxt == 'r') {
      out[w++] = '\r';
      p++;
    } else if (nxt == '\\') {
      out[w++] = '\\';
      p++;
    } else {
      out[w++] = '\\';                  // unknown escape: keep the backslash verbatim
    }
  }
  const bool fit = (*p == '\0');     // stopped at the end of the input, not at the buffer
  out[w] = '\0';
  return fit;
}

// ---------------------------------------------------------------- pack
bool smsMirrorPack(const SmsMirrorRecord* r, char* out, size_t cap) {
  if (!r || !out || cap == 0) {
    return false;
  }
  out[0] = '\0';

  char peer[SMS_MIRROR_PEER_MAX];
  if (!smsMirrorDigits(r->peer, peer, sizeof(peer))) {
    return false;                       // a record with no correspondent is not storable
  }

  char esc[SMS_MIRROR_TEXT_MAX * 2 + 2];
  size_t tlen = smLen(r->text, SMS_MIRROR_TEXT_MAX);
  if (tlen && !smEscape(r->text, tlen, esc, sizeof(esc))) {
    return false;
  }
  if (!tlen) {
    esc[0] = '\0';
  }

  int w = snprintf(out, cap, "%s %lld %s %c %08lx %s",
                   SMS_MIRROR_PREFIX, (long long)r->id, peer,
                   r->out ? 'o' : 'i', (unsigned long)r->ts, esc);
  if (w <= 0 || (size_t)w >= cap) {
    out[0] = '\0';                      // never hand back a truncated record
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- unpack
// One whitespace-delimited token. Advances *pp past it. NULL when there is nothing left.
static const char* smToken(const char** pp, size_t* lenOut) {
  const char* p = *pp;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (!*p) {
    *pp = p;
    return NULL;
  }
  const char* s = p;
  while (*p && *p != ' ' && *p != '\t') {
    p++;
  }
  *lenOut = (size_t)(p - s);
  *pp = p;
  return s;
}

static bool smAllDigits(const char* s, size_t n) {
  if (!n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return true;
}

bool smsMirrorUnpack(const char* line, SmsMirrorRecord* out) {
  if (!line || !out || !smsMirrorIsMirrorText(line)) {
    return false;
  }
  memset(out, 0, sizeof(*out));

  const char* p = line + strlen(SMS_MIRROR_PREFIX);

  // --- id: decimal, optionally negative (COVEY uses negative ids for un-acked sends)
  size_t n = 0;
  const char* tok = smToken(&p, &n);
  if (!tok) {
    return false;
  }
  {
    bool neg = (n && tok[0] == '-');
    if (!smAllDigits(tok + (neg ? 1 : 0), n - (neg ? 1 : 0))) {
      return false;
    }
    char idbuf[24];
    if (n >= sizeof(idbuf)) {
      return false;
    }
    memcpy(idbuf, tok, n);
    idbuf[n] = '\0';
    out->id = (int64_t)strtoll(idbuf, NULL, 10);
  }

  // --- peer
  tok = smToken(&p, &n);
  if (!tok || n >= SMS_MIRROR_PEER_MAX) {
    return false;
  }
  {
    char raw[SMS_MIRROR_PEER_MAX];
    memcpy(raw, tok, n);
    raw[n] = '\0';
    if (!smsMirrorDigits(raw, out->peer, sizeof(out->peer))) {
      return false;
    }
  }

  // --- direction
  tok = smToken(&p, &n);
  if (!tok || n != 1 || (tok[0] != 'o' && tok[0] != 'i')) {
    return false;
  }
  out->out = (tok[0] == 'o');

  // --- timestamp, 8 hex digits
  tok = smToken(&p, &n);
  if (!tok || n == 0 || n > 8) {
    return false;
  }
  {
    char tsbuf[9];
    for (size_t i = 0; i < n; i++) {
      char c = tok[i];
      bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if (!hex) {
        return false;
      }
      tsbuf[i] = c;
    }
    tsbuf[n] = '\0';
    out->ts = (uint32_t)strtoul(tsbuf, NULL, 16);
  }

  /* --- text: exactly one separator, then the rest of the line verbatim.
   *
   * Only ONE space is consumed on purpose. Skipping a whitespace run here would eat a
   * leading space that is genuinely part of the message. An absent text is allowed —
   * an empty SMS is not worth failing a whole sync over. */
  if (*p == ' ') {
    p++;
  } else if (*p) {
    return false;                       // something other than a separator: malformed
  }

  /* Strip transport artifacts. Safe unconditionally BECAUSE of the escaping: a real
   * newline in the message is now "\n", two characters, so any raw CR/LF reaching here
   * came from the wire and not from the sender. */
  char rawText[SMS_MIRROR_TEXT_MAX * 2 + 2];
  size_t rl = smLen(p, sizeof(rawText) - 1);
  while (rl && (p[rl - 1] == '\n' || p[rl - 1] == '\r')) {
    rl--;
  }
  memcpy(rawText, p, rl);
  rawText[rl] = '\0';

  /* ⚠ TRUNCATION MUST NOT BE SILENT. Unescaping into a fixed buffer cuts, and a cut message
   * parses perfectly — the record looks fine and simply loses its tail. That is exactly the
   * kind of quiet wrong this feature keeps having to defend against. The record is still
   * returned: a message missing its end beats no message at all. */
  if (!smUnescape(rawText, out->text, sizeof(out->text))) {
    smsMirrorTruncations++;
  }
  return true;
}

// ---------------------------------------------------------------- matching
bool smsMirrorSameMessage(const SmsMirrorRecord* a, const SmsMirrorRecord* b) {
  if (!a || !b || a->out != b->out) {
    return false;
  }
  char pa[SMS_MIRROR_PEER_MAX];
  char pb[SMS_MIRROR_PEER_MAX];
  if (!smsMirrorDigits(a->peer, pa, sizeof(pa)) || !smsMirrorDigits(b->peer, pb, sizeof(pb))) {
    return false;
  }
  if (strcmp(pa, pb) != 0) {
    return false;
  }
  return strncmp(a->text, b->text, SMS_MIRROR_TEXT_MAX) == 0;
}
