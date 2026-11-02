/*
 * replay_proto.cpp — see replay_proto.h. covey_ui/replay.py is the reference;
 * tests/test_replay.cpp pins this file against vectors generated FROM it.
 *
 * Parity notes, because they are where interop dies:
 * - Numbers are ASCII digits only, both sides. Python's int() would take '+5'
 *   and unicode digits; replay.py._uint() refuses them exactly like parseUint.
 * - The REQUEST splits on any whitespace run (python str.split()); RECORD
 *   lines are single-space-delimited (python split(" ", 3)) — the emitters
 *   never produce doubled spaces and both parsers refuse them.
 * - The channel field escapes exactly '%'->'%25' and ' '->'%20', in that
 *   order on emit ('Howe group' is a real channel name on this mesh).
 */

#include "replay_proto.h"
#include <string.h>
#include <stdio.h>

bool replayIsReplayText(const char* text) {
  return text != NULL && strncmp(text, "RPL", 3) == 0;
}

static bool isWs(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static bool parseUint(const char* s, size_t len, uint32_t* out) {
  if (len == 0 || len > 10) {
    return false;
  }
  uint64_t v = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
    v = v * 10u + (uint64_t)(s[i] - '0');
    if (v > 0xFFFFFFFFull) {
      return false;
    }
  }
  *out = (uint32_t)v;
  return true;
}

bool replayParseRequest(const char* text, uint32_t* t1, uint32_t* t2, int* maxn) {
  if (text == NULL || strncmp(text, "RPL? ", 5) != 0) {
    return false;
  }
  const char* f[3];
  size_t l[3];
  int n = 0;
  const char* p = text + 5;
  while (*p) {
    while (*p && isWs(*p)) {
      p++;
    }
    if (!*p) {
      break;
    }
    if (n >= 3) {
      return false;               // a fourth field is a malformed request, not v2
    }
    f[n] = p;
    while (*p && !isWs(*p)) {
      p++;
    }
    l[n] = (size_t)(p - f[n]);
    n++;
  }
  if (n != 3) {
    return false;
  }
  uint32_t a, b, m;
  if (!parseUint(f[0], l[0], &a) || !parseUint(f[1], l[1], &b) || !parseUint(f[2], l[2], &m)) {
    return false;
  }
  if (b < a || m == 0) {
    return false;                 // inverted window / zero max: refuse, don't guess
  }
  *t1 = a;
  *t2 = b;
  *maxn = (int)m;
  return true;
}

/* '%'->'%25' then ' '->'%20' — same order and same TWO characters as
 * replay.py's _esc_chan; anything more clever would be a second dialect. */
static int escChan(const char* name, char* out, size_t cap) {
  size_t o = 0;
  for (const char* p = name; *p; p++) {
    const char* rep = (*p == '%') ? "%25" : (*p == ' ') ? "%20" : NULL;
    if (rep) {
      if (o + 3 >= cap) {
        return -1;
      }
      memcpy(out + o, rep, 3);
      o += 3;
    } else {
      if (o + 1 >= cap) {
        return -1;
      }
      out[o++] = *p;
    }
  }
  out[o] = '\0';
  return (int)o;
}

int replayFormatRecord(char* out, size_t cap,
                       uint32_t ts, uint32_t sender,
                       const char* chanName, const char* text) {
  char chan[64];
  if (chanName == NULL || text == NULL || escChan(chanName, chan, sizeof(chan)) < 0) {
    return -1;
  }
  int n = snprintf(out, cap, "RPL %lu %08lx %s ",
                   (unsigned long)ts, (unsigned long)sender, chan);
  if (n < 0 || (size_t)n >= cap) {
    return -1;
  }
  /* Text rides verbatim minus newlines (python scrubs them to spaces too —
   * a newline inside a record would masquerade as a second record). */
  size_t o = (size_t)n;
  for (const char* p = text; *p; p++) {
    if (o + 1 >= cap) {
      return -1;
    }
    out[o++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
  }
  out[o] = '\0';
  return (int)o;
}

int replayFormatSummary(char* out, size_t cap, int sent, bool gap) {
  if (sent < 0) {
    return -1;
  }
  int n = snprintf(out, cap, "RPL. %d %d", sent, gap ? 1 : 0);
  return (n < 0 || (size_t)n >= cap) ? -1 : n;
}
