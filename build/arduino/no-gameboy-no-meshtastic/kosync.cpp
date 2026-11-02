/*
 * kosync.cpp — see kosync.h. No Arduino headers: tests/test_kosync.cpp builds this file.
 */
#include "kosync.h"
#include "book_hash.h"
#include "booksync.h"
#include "booksync_inbox.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- small helpers
static char ksLower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool ksEqNoCase(const char* a, size_t alen, const char* b) {
  const size_t blen = strlen(b);
  if (alen != blen) {
    return false;
  }
  for (size_t i = 0; i < alen; i++) {
    if (ksLower(a[i]) != ksLower(b[i])) {
      return false;
    }
  }
  return true;
}

static bool ksIsHex32(const char* v);   // (the config section)

static void ksCopy(char* out, size_t cap, const char* in) {
  if (cap == 0) {
    return;
  }
  size_t n = in ? strlen(in) : 0;
  if (n > cap - 1) {
    n = cap - 1;
  }
  if (n) {
    memcpy(out, in, n);
  }
  out[n] = '\0';
}

// ---------------------------------------------------------------- numbers
size_t kosyncFormatPct(double p, char* out, size_t cap) {
  if (!(p >= 0.0)) {              // negative, and NaN
    p = 0.0;
  }
  if (p > 1.0) {
    p = 1.0;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%.6f", p);
  size_t n = strlen(buf);
  if (strchr(buf, '.')) {
    while (n > 0 && buf[n - 1] == '0') {
      buf[--n] = '\0';
    }
    if (n > 0 && buf[n - 1] == '.') {
      buf[--n] = '\0';
    }
  }
  if (n == 0 || !strcmp(buf, "-0")) {
    strcpy(buf, "0");
  }
  ksCopy(out, cap, buf);
  return strlen(out);
}

// ---------------------------------------------------------------- JSON
static const char* jsWs(const char* p, const char* e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
    p++;
  }
  return p;
}

static int jsHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static size_t jsPutUtf8(uint32_t cp, char* o) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
  }
  o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

static bool jsU4(const char* p, const char* e, uint32_t* v) {
  if (e - p < 4) {
    return false;
  }
  uint32_t x = 0;
  for (int i = 0; i < 4; i++) {
    const int h = jsHex(p[i]);
    if (h < 0) {
      return false;
    }
    x = (x << 4) | (uint32_t)h;
  }
  *v = x;
  return true;
}

/* A string starting at the opening quote. Decoded into `out` (cut at cap, never overrun);
 * returns the character after the closing quote, or NULL when it never closes. */
static const char* jsString(const char* p, const char* e, char* out, size_t cap) {
  size_t n = 0;
  if (p >= e || *p != '"') {
    return NULL;
  }
  p++;
  while (p < e && *p != '"') {
    char enc[4];
    size_t el = 0;
    if (*p == '\\') {
      if (p + 1 >= e) {
        return NULL;
      }
      const char c = p[1];
      p += 2;
      switch (c) {
        case '"': enc[0] = '"'; el = 1; break;
        case '\\': enc[0] = '\\'; el = 1; break;
        case '/': enc[0] = '/'; el = 1; break;
        case 'b': enc[0] = '\b'; el = 1; break;
        case 'f': enc[0] = '\f'; el = 1; break;
        case 'n': enc[0] = '\n'; el = 1; break;
        case 'r': enc[0] = '\r'; el = 1; break;
        case 't': enc[0] = '\t'; el = 1; break;
        case 'u': {
          uint32_t cp = 0;
          if (!jsU4(p, e, &cp)) {
            return NULL;
          }
          p += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF) {              // a surrogate pair, if one follows
            uint32_t lo = 0;
            if (e - p >= 6 && p[0] == '\\' && p[1] == 'u' && jsU4(p + 2, e, &lo) &&
                lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              p += 6;
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;                                   // a lone low surrogate
          }
          el = jsPutUtf8(cp, enc);
          break;
        }
        default:
          return NULL;                                     // not JSON
      }
    } else {
      enc[0] = *p++;
      el = 1;
    }
    if (out && n + el < cap) {
      memcpy(out + n, enc, el);
      n += el;
    }
  }
  if (p >= e) {
    return NULL;
  }
  if (out && cap) {
    out[n] = '\0';
  }
  return p + 1;
}

// A number token: returns the character after it, or NULL if there is none here.
static const char* jsNumber(const char* p, const char* e, double* v) {
  char tok[40];
  size_t n = 0;
  while (p < e && n < sizeof(tok) - 1 &&
         ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) {
    tok[n++] = *p++;
  }
  if (n == 0) {
    return NULL;
  }
  tok[n] = '\0';
  char* end = NULL;
  const double d = strtod(tok, &end);
  if (!end || *end != '\0') {
    return NULL;
  }
  *v = d;
  return p;
}

static const char* jsSkip(const char* p, const char* e, int depth) {
  p = jsWs(p, e);
  if (p >= e || depth > 16) {
    return NULL;
  }
  if (*p == '"') {
    return jsString(p, e, NULL, 0);
  }
  if (*p == '{' || *p == '[') {
    const char close = (*p == '{') ? '}' : ']';
    const bool obj = (*p == '{');
    p = jsWs(p + 1, e);
    if (p < e && *p == close) {
      return p + 1;
    }
    for (;;) {
      if (obj) {
        p = jsString(jsWs(p, e), e, NULL, 0);
        if (!p) return NULL;
        p = jsWs(p, e);
        if (p >= e || *p != ':') return NULL;
        p++;
      }
      p = jsSkip(p, e, depth + 1);
      if (!p) return NULL;
      p = jsWs(p, e);
      if (p >= e) return NULL;
      if (*p == ',') { p++; continue; }
      if (*p == close) return p + 1;
      return NULL;
    }
  }
  if (e - p >= 4 && (!strncmp(p, "true", 4) || !strncmp(p, "null", 4))) {
    return p + 4;
  }
  if (e - p >= 5 && !strncmp(p, "false", 5)) {
    return p + 5;
  }
  double d;
  return jsNumber(p, e, &d);
}

bool kosyncParseProgress(const char* body, size_t len, KosyncProgress* out) {
  memset(out, 0, sizeof(*out));
  if (!body) {
    return false;
  }
  const char* e = body + len;
  const char* p = jsWs(body, e);
  if (p >= e || *p != '{') {
    return false;
  }
  p = jsWs(p + 1, e);
  if (p < e && *p == '}') {
    return true;                        // `{}`: nobody has synced this document
  }
  for (;;) {
    char key[24];
    p = jsString(jsWs(p, e), e, key, sizeof(key));
    if (!p) {
      return false;
    }
    p = jsWs(p, e);
    if (p >= e || *p != ':') {
      return false;
    }
    p = jsWs(p + 1, e);
    if (p >= e) {
      return false;
    }
    const char* next = NULL;
    if (!strcmp(key, "percentage") || !strcmp(key, "timestamp")) {
      double d = 0.0;
      bool have = false;
      if (*p == '"') {                  // a server that quotes its numbers: still a number
        char s[40];
        next = jsString(p, e, s, sizeof(s));
        char* end = NULL;
        d = strtod(s, &end);
        have = next && end && end != s && *end == '\0';
      } else if (*p == 'n') {
        next = jsSkip(p, e, 0);         // null: absent
      } else {
        next = jsNumber(p, e, &d);
        have = next != NULL;
      }
      if (have && !strcmp(key, "percentage") && isfinite(d)) {
        out->hasPct = true;
        out->pct = d;
      } else if (have && isfinite(d)) {
        out->hasTimestamp = true;
        out->timestamp = (int64_t)d;
      }
    } else if (!strcmp(key, "device") || !strcmp(key, "device_id") || !strcmp(key, "document")) {
      char* dst = !strcmp(key, "device") ? out->device
                : !strcmp(key, "device_id") ? out->deviceId : out->document;
      const size_t cap = !strcmp(key, "document") ? sizeof(out->document) : sizeof(out->device);
      if (*p == '"') {
        next = jsString(p, e, dst, cap);
      } else {
        next = jsSkip(p, e, 0);         // a number or null where a name should be: ignore it
      }
    } else {
      next = jsSkip(p, e, 0);           // progress, metadata, anything newer
    }
    if (!next) {
      return false;
    }
    p = jsWs(next, e);
    if (p >= e) {
      return false;
    }
    if (*p == ',') {
      p++;
      continue;
    }
    return *p == '}';
  }
}

size_t kosyncJsonEscape(const char* in, char* out, size_t cap) {
  size_t n = 0;
  if (!cap) {
    return 0;
  }
  for (const unsigned char* p = (const unsigned char*)(in ? in : ""); *p; p++) {
    char tmp[8];
    size_t tl;
    if (*p == '"' || *p == '\\') {
      tmp[0] = '\\'; tmp[1] = (char)*p; tl = 2;
    } else if (*p < 0x20) {
      tl = (size_t)snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)*p);
    } else {
      tmp[0] = (char)*p; tl = 1;
    }
    if (n + tl >= cap) {
      break;
    }
    memcpy(out + n, tmp, tl);
    n += tl;
  }
  out[n] = '\0';
  return n;
}

// ---------------------------------------------------------------- HTTP
bool kosyncHeader(const char* hdrs, const char* name, char* out, size_t cap) {
  if (cap) {
    out[0] = '\0';
  }
  if (!hdrs || !name) {
    return false;
  }
  const char* line = strchr(hdrs, '\n');           // skip the request/status line
  while (line) {
    line++;
    const char* eol = line;
    while (*eol && *eol != '\n') {
      eol++;
    }
    const char* end = eol;
    if (end > line && end[-1] == '\r') {
      end--;
    }
    if (end == line) {
      return false;                                // the blank line: end of headers
    }
    const char* colon = (const char*)memchr(line, ':', (size_t)(end - line));
    if (colon && ksEqNoCase(line, (size_t)(colon - line), name)) {
      const char* v = colon + 1;
      while (v < end && (*v == ' ' || *v == '\t')) {
        v++;
      }
      const char* ve = end;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) {
        ve--;
      }
      size_t n = (size_t)(ve - v);
      if (cap) {
        if (n > cap - 1) {
          n = cap - 1;
        }
        memcpy(out, v, n);
        out[n] = '\0';
      }
      return true;
    }
    line = *eol ? eol : NULL;
  }
  return false;
}

bool kosyncAuthOk(const char* hdrs, const char* user, const char* keyHex) {
  if (!user || !user[0] || !keyHex || !keyHex[0]) {
    return false;                                  // nothing configured authorises nobody
  }
  char u[KOSYNC_USER_MAX + 8], k[KOSYNC_KEY_CHARS + 8];
  if (!kosyncHeader(hdrs, "x-auth-user", u, sizeof(u)) ||
      !kosyncHeader(hdrs, "x-auth-key", k, sizeof(k))) {
    return false;
  }
  const bool userOk = bsConstTimeEqual(u, user);
  const bool keyOk = bsConstTimeEqual(k, keyHex);  // both always evaluated
  return userOk && keyOk;
}

bool kosyncClientTakesPercentage(const char* hdrs) {
  // Presence is the marker (kosync.h): the fork's and the phone's `X-BookSync: 1`, or a Basic header.
  char v[4];
  return kosyncHeader(hdrs, "x-booksync", v, sizeof(v)) ||
         kosyncHeader(hdrs, "authorization", v, sizeof(v));
}

int kosyncStatusCode(const char* line) {
  if (!line || strncmp(line, "HTTP/1.", 7) != 0) {
    return -1;
  }
  const char* sp = strchr(line, ' ');
  if (!sp) {
    return -1;
  }
  const int c = atoi(sp + 1);
  return (c >= 100 && c <= 599) ? c : -1;
}

long kosyncDechunk(char* buf, size_t len) {
  size_t r = 0, w = 0;
  for (;;) {
    size_t size = 0;
    int digits = 0;
    while (r < len) {
      const char c = buf[r];
      int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
            : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (h < 0) {
        break;
      }
      if (size > (len >> 4) + 1) {
        return -1;                     // bigger than anything this buffer could hold
      }
      size = size * 16 + (size_t)h;
      digits++;
      r++;
    }
    if (!digits) {
      return -1;
    }
    while (r < len && buf[r] != '\n') {
      r++;                             // an extension (";name=value") and the CR
    }
    if (r >= len) {
      return -1;
    }
    r++;                               // the LF
    if (size == 0) {
      return (long)w;                  // the last chunk; any trailers are dropped
    }
    if (size > len - r) {
      return -1;                       // a chunk that runs past what arrived
    }
    memmove(buf + w, buf + r, size);   // w <= r always: safe in place
    w += size;
    r += size;
    if (r < len && buf[r] == '\r') {
      r++;
    }
    if (r >= len || buf[r] != '\n') {
      return -1;
    }
    r++;
  }
}

static int ksHost(char* out, size_t cap, const char* host, uint16_t port) {
  return port == 80 ? snprintf(out, cap, "%s", host) : snprintf(out, cap, "%s:%u", host, (unsigned)port);
}

size_t kosyncBuildGet(char* out, size_t cap, const char* host, uint16_t port,
                      const char* user, const char* key, const char* doc) {
  char h[KOSYNC_HOST_MAX + 8];
  ksHost(h, sizeof(h), host, port);
  const int n = snprintf(out, cap,
                         "GET /syncs/progress/%s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Accept: " KOSYNC_ACCEPT "\r\n"
                         "x-auth-user: %s\r\n"
                         "x-auth-key: %s\r\n"
                         KOSYNC_MARKER_LINE                 // else a D-163 home answers `{}`
                         "Connection: close\r\n"
                         "\r\n",
                         doc, h, user, key);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t kosyncBuildPut(char* out, size_t cap, const char* host, uint16_t port,
                      const char* user, const char* key, const char* body) {
  char h[KOSYNC_HOST_MAX + 8];
  ksHost(h, sizeof(h), host, port);
  const int n = snprintf(out, cap,
                         "PUT /syncs/progress HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Accept: " KOSYNC_ACCEPT "\r\n"
                         "Content-Type: application/json\r\n"
                         "x-auth-user: %s\r\n"
                         "x-auth-key: %s\r\n"
                         KOSYNC_MARKER_LINE                 // one client, the same on both verbs
                         "Content-Length: %u\r\n"
                         "Connection: close\r\n"
                         "\r\n"
                         "%s",
                         h, user, key, (unsigned)strlen(body), body);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t kosyncBuildPutBody(char* out, size_t cap, const char* doc, double pct,
                          const char* device, const char* deviceId) {
  char p[16], d[KOSYNC_DEV_MAX * 2], di[KOSYNC_DEV_MAX * 2], dc[KOSYNC_DOC_MAX * 2];
  kosyncFormatPct(pct, p, sizeof(p));
  kosyncJsonEscape(device, d, sizeof(d));
  kosyncJsonEscape(deviceId, di, sizeof(di));
  kosyncJsonEscape(doc, dc, sizeof(dc));
  const int n = snprintf(out, cap,
                         "{\"document\":\"%s\",\"progress\":\"\",\"percentage\":%s,"
                         "\"device\":\"%s\",\"device_id\":\"%s\"}",
                         dc, p, d, di);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// ---------------------------------------------------------------- the server
bool kosyncIsPath(const char* path) {
  return path && (!strncmp(path, "/users/", 7) || !strncmp(path, "/syncs/", 7) ||
                  !strcmp(path, "/healthcheck"));
}

KosyncRoute kosyncRoute(const char* method, const char* path, char* doc, size_t cap) {
  if (cap) {
    doc[0] = '\0';
  }
  if (!method || !path) {
    return KOSYNC_ROUTE_NONE;
  }
  if (!strcmp(path, "/healthcheck") && !strcmp(method, "GET")) {
    return KOSYNC_ROUTE_HEALTH;
  }
  if (!strcmp(path, "/users/auth") && !strcmp(method, "GET")) {
    return KOSYNC_ROUTE_AUTH;
  }
  if (!strcmp(path, "/users/create")) {
    return KOSYNC_ROUTE_CREATE;
  }
  if (!strcmp(path, "/syncs/progress") && !strcmp(method, "PUT")) {
    return KOSYNC_ROUTE_PUT_PROGRESS;
  }
  static const char PFX[] = "/syncs/progress/";
  if (!strncmp(path, PFX, sizeof(PFX) - 1) && !strcmp(method, "GET")) {
    const char* d = path + sizeof(PFX) - 1;
    if (!*d || strchr(d, '/') || strlen(d) >= cap) {
      return KOSYNC_ROUTE_NONE;
    }
    ksCopy(doc, cap, d);
    return KOSYNC_ROUTE_GET_PROGRESS;
  }
  return KOSYNC_ROUTE_NONE;
}

bool kosyncIsOwnRecord(const char* theirDevice, const char* theirDeviceId,
                       const char* myDevice, const char* myDeviceId) {
  const bool theirId = theirDeviceId && theirDeviceId[0];
  if (theirId && myDeviceId && myDeviceId[0]) {
    return !strcmp(theirDeviceId, myDeviceId);   // two ids: they decide, the names do not
  }
  if (theirId) {
    return false;                       // an id that cannot be ours (we have none to match)
  }
  // No id on the record: the name is all there is.
  return theirDevice && theirDevice[0] && myDevice && !strcmp(theirDevice, myDevice);
}

bool kosyncWorthParking(double theirs, const char* theirDevice, const char* theirDeviceId,
                        double mine, const char* myDevice, const char* myDeviceId) {
  if (kosyncIsOwnRecord(theirDevice, theirDeviceId, myDevice, myDeviceId)) {
    return false;
  }
  // The same slack every device uses: CrossPoint calls this "synchronized".
  return fabs(theirs - mine) > KOSYNC_EPSILON + 1e-9;
}

int kosyncOfferVerdict(const KosyncOfferIn* in) {
  if (kosyncIsOwnRecord(in->theirDevice, in->theirDeviceId, in->myDevice, in->myDeviceId)) {
    return KOSYNC_SKIP_OWN;
  }
  if (in->mineOk && fabs(in->theirPct - in->minePct) <= KOSYNC_EPSILON + 1e-9) {
    return KOSYNC_SKIP_IN_STEP;        // CrossPoint's "synchronized"
  }
  const uint32_t sig = kosyncOfferSig(in->theirTs, in->theirPct, in->theirDeviceId, in->theirDevice);
  if (in->pendingSig && in->pendingSig == sig) {
    return KOSYNC_SKIP_WAITING;        // on the card already, and not yet answered (KS-2)
  }
  if (in->offeredSig && in->offeredSig == sig) {
    return KOSYNC_SKIP_OFFERED;        // the person has ANSWERED this very record already
  }
  // PROVABLY older: both clocks known, and theirs strictly before our last move.
  if (in->theirTs > 0 && in->myMovedAt > 0 && in->theirTs < (int64_t)in->myMovedAt) {
    return KOSYNC_SKIP_OLDER;
  }
  return KOSYNC_OFFER;
}

int kosyncPickRecord(const KosyncProgress* got, const bool* have, int n,
                     const char* myDevice, const char* myDeviceId, bool* sawAny) {
  int best = -1;
  if (sawAny) {
    *sawAny = false;
  }
  for (int i = 0; i < n; i++) {
    if (!have[i] || !got[i].hasPct) {
      continue;
    }
    if (sawAny) {
      *sawAny = true;
    }
    if (kosyncIsOwnRecord(got[i].device, got[i].deviceId, myDevice, myDeviceId)) {
      continue;
    }
    if (best < 0 || (got[i].hasTimestamp && got[best].hasTimestamp &&
                     got[i].timestamp > got[best].timestamp)) {
      best = i;
    }
  }
  return best;
}

const char* kosyncOfferVerdictText(int v) {
  switch (v) {
    case KOSYNC_OFFER:        return "offered";
    case KOSYNC_SKIP_OWN:     return "our own record";
    case KOSYNC_SKIP_IN_STEP: return "already in step";
    case KOSYNC_SKIP_OFFERED: return "already offered";
    case KOSYNC_SKIP_OLDER:   return "older than your last page turn";
    case KOSYNC_SKIP_WAITING: return "on the card, waiting for your answer";
  }
  return "?";
}

void kosyncOfferFill(KosyncOfferIn* in, const KosyncProgress* g, bool explicitSync, bool mineOk,
                     double minePct, uint32_t askedMovedAt, const KosyncMemoEntry* me,
                     uint32_t pendingSig, const char* myDevice, const char* myDeviceId) {
  memset(in, 0, sizeof(*in));
  in->theirPct = g->pct;
  in->theirTs = g->hasTimestamp ? g->timestamp : 0;
  in->theirDevice = g->device;
  in->theirDeviceId = g->deviceId;
  in->mineOk = mineOk;
  in->minePct = minePct;
  in->myDevice = myDevice;
  in->myDeviceId = myDeviceId;
  /* 🛑 KS-1 (kosync.h): an automatic ask is judged as of the ask. A turn made while its reply
   * was on the way is newer than the ask, not newer than the X4's place. */
  in->myMovedAt = explicitSync ? (me ? me->movedAt : 0) : askedMovedAt;
  in->offeredSig = me ? me->offeredSig : 0;
  in->pendingSig = pendingSig;
}

uint32_t kosyncOfferSig(int64_t ts, double pct, const char* deviceId, const char* device) {
  char p[16];
  kosyncFormatPct(pct, p, sizeof(p));
  const char* who = (deviceId && deviceId[0]) ? deviceId : (device ? device : "");
  char s[40 + 16 + 2 * KOSYNC_DEV_MAX];
  snprintf(s, sizeof(s), "%lld|%s|%s", (long long)(ts > 0 ? ts : 0), p, who);
  uint32_t h = 2166136261u;                           // FNV-1a
  for (const unsigned char* q = (const unsigned char*)s; *q; q++) {
    h ^= *q;
    h *= 16777619u;
  }
  return h ? h : 1u;                                  // 0 means "nothing offered"
}

bool kosyncAutoPushWanted(bool refOk, double refPct, bool movedSinceRef, bool pctOk, double pct) {
  if (!pctOk) {
    return false;                       // nothing expressible to send
  }
  if (refOk) {
    return fabs(pct - refPct) > KOSYNC_EPSILON + 1e-9;
  }
  return movedSinceRef;
}

bool kosyncClosePushWanted(const KosyncMemoEntry* e, bool pctOk, double pct) {
  if (!pctOk) {
    return false;                       // nothing expressible to send
  }
  if (!e) {
    return true;                        // nothing known: the old rule (push on close)
  }
  if (kosyncAutoPushWanted(e->refOk, e->refPct, e->movedSinceRef, pctOk, pct)) {
    return true;                        // moved this session
  }
  if (e->sentOk) {
    return fabs(pct - e->sentPct) > KOSYNC_EPSILON + 1e-9;   // home has another place of ours
  }
  return e->unsent;                     // never delivered: a move home has not had
}

// ---------------------------------------------------------------- the per-book memo
bool kosyncRekeyBook(KosyncMemo* m, KosyncParkLedger* l, const char* oldBook, const char* newBook) {
  if (!oldBook || !newBook || !oldBook[0] || !newBook[0] || !strcmp(oldBook, newBook)) {
    return false;
  }
  bool moved = false;
  if (m) {
    KosyncMemoEntry* from = NULL;
    for (int i = 0; i < KOSYNC_MEMO_MAX; i++) {
      if (m->e[i].book[0] && !strcmp(m->e[i].book, oldBook)) {
        from = &m->e[i];
      }
    }
    if (from) {
      for (int i = 0; i < KOSYNC_MEMO_MAX; i++) {
        if (&m->e[i] != from && m->e[i].book[0] && !strcmp(m->e[i].book, newBook)) {
          memset(&m->e[i], 0, sizeof(m->e[i]));
        }
      }
      ksCopy(from->book, sizeof(from->book), newBook);
      m->dirty = true;
      moved = true;
    }
  }
  if (l) {
    /* A parked offer follows the book, unless one is parked under the new name already: one
     * offer per book is the ledger's rule, and that one is the newer park. */
    bool newHas = false;
    for (int i = 0; i < KOSYNC_LEDGER_MAX; i++) {
      newHas = newHas || (l->id[i] && !strcmp(l->book[i], newBook));
    }
    for (int i = 0; i < KOSYNC_LEDGER_MAX && !newHas; i++) {
      if (!strcmp(l->book[i], newBook)) {         // a spent slot (id 0): ledgerFind must not
        l->book[i][0] = '\0';                     // find it ahead of the one moving in
        l->sig[i] = 0;
      }
    }
    for (int i = 0; i < KOSYNC_LEDGER_MAX && !newHas; i++) {
      if (l->id[i] && !strcmp(l->book[i], oldBook)) {
        ksCopy(l->book[i], sizeof(l->book[i]), newBook);
        moved = true;
      }
    }
  }
  return moved;
}

void kosyncMemoOpened(KosyncMemoEntry* e, bool pctOk, double pct) {
  if (!e) {
    return;
  }
  e->refOk = pctOk;
  e->refPct = pctOk ? pct : 0.0;
  e->movedSinceRef = false;
}

void kosyncMemoMoved(KosyncMemo* m, KosyncMemoEntry* e, uint32_t nowUtc) {
  if (!m || !e) {
    return;
  }
  e->movedSinceRef = true;
  if (!e->unsent) {
    e->unsent = true;
    m->dirty = true;                    // saved at the close, not on every turn
  }
  if (nowUtc && nowUtc != e->movedAt) {
    e->movedAt = nowUtc;
    m->dirty = true;
  }
}

void kosyncMemoSent(KosyncMemo* m, KosyncMemoEntry* e, double pct) {
  if (!m || !e) {
    return;
  }
  e->refOk = true;
  e->refPct = pct;
  e->movedSinceRef = false;
  /* `unsent` goes even if the reader moved again while this PUT was in flight: with sentOk the
   * rule compares PLACES (sentPct), so that later move still differs and is still pushed. */
  if (!e->sentOk || e->sentPct != pct || e->unsent) {
    e->sentOk = true;
    e->sentPct = pct;
    e->unsent = false;
    m->dirty = true;
  }
}

KosyncMemoEntry* kosyncMemoGet(KosyncMemo* m, const char* book, bool create) {
  if (!m || !book || !book[0]) {
    return NULL;
  }
  int freeSlot = -1, oldest = -1;
  for (int i = 0; i < KOSYNC_MEMO_MAX; i++) {
    KosyncMemoEntry* e = &m->e[i];
    if (!e->book[0]) {
      if (freeSlot < 0) {
        freeSlot = i;
      }
      continue;
    }
    if (!strcmp(e->book, book)) {
      e->used = ++m->tick;
      return e;
    }
    if (oldest < 0 || e->used < m->e[oldest].used) {
      oldest = i;
    }
  }
  if (!create) {
    return NULL;
  }
  const int slot = freeSlot >= 0 ? freeSlot : oldest;
  KosyncMemoEntry* e = &m->e[slot];
  if (e->book[0] && (e->movedAt || e->offeredSig || e->sentOk || e->unsent)) {
    m->dirty = true;                    // an evicted book's stamps must leave the card too
  }
  memset(e, 0, sizeof(*e));
  ksCopy(e->book, sizeof(e->book), book);
  e->used = ++m->tick;
  return e;
}

static void ksPut32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t ksGet32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* An entry on the card: the id (32), movedAt, offeredSig, used, the sent place in millionths
 * (the wire's 6 decimals — the D2 comparison is 0.001 wide), then flags: 1 sentOk, 2 unsent. */
#define KSM_SENT_OK 1u
#define KSM_UNSENT  2u

size_t kosyncMemoPack(const KosyncMemo* m, uint8_t* out, size_t cap) {
  if (!m || !out || cap < 4) {
    return 0;
  }
  memcpy(out, "KSM2", 4);
  size_t n = 4;
  for (int i = 0; i < KOSYNC_MEMO_MAX; i++) {
    const KosyncMemoEntry* e = &m->e[i];
    if (strlen(e->book) != 32 || (!e->movedAt && !e->offeredSig && !e->sentOk && !e->unsent)) {
      continue;                         // a slot with nothing worth keeping across a reboot
    }
    if (n + KOSYNC_MEMO_ENTRY_BYTES > cap) {
      break;
    }
    double sp = e->sentPct < 0.0 ? 0.0 : (e->sentPct > 1.0 ? 1.0 : e->sentPct);
    memcpy(out + n, e->book, 32);
    ksPut32(out + n + 32, e->movedAt);
    ksPut32(out + n + 36, e->offeredSig);
    ksPut32(out + n + 40, e->used);
    ksPut32(out + n + 44, e->sentOk ? (uint32_t)(sp * 1000000.0 + 0.5) : 0u);
    ksPut32(out + n + 48, (e->sentOk ? KSM_SENT_OK : 0u) | (e->unsent ? KSM_UNSENT : 0u));
    n += KOSYNC_MEMO_ENTRY_BYTES;
  }
  return n;
}

bool kosyncMemoUnpack(KosyncMemo* m, const uint8_t* in, size_t len) {
  if (!m) {
    return false;
  }
  memset(m, 0, sizeof(*m));
  const size_t EB = KOSYNC_MEMO_ENTRY_BYTES;
  if (!in || len < 4 || memcmp(in, "KSM2", 4) != 0 || (len - 4) % EB != 0 ||
      (len - 4) / EB > KOSYNC_MEMO_MAX) {
    return false;
  }
  const int n = (int)((len - 4) / EB);
  for (int i = 0; i < n; i++) {
    const uint8_t* r = in + 4 + EB * (size_t)i;
    char id[33];
    memcpy(id, r, 32);
    id[32] = '\0';
    const uint32_t micro = ksGet32(r + 44);
    const uint32_t flags = ksGet32(r + 48);
    if (!ksIsHex32(id) || micro > 1000000u || (flags & ~(KSM_SENT_OK | KSM_UNSENT))) {
      memset(m, 0, sizeof(*m));
      return false;                     // not ours, or damaged: start empty, never half-read
    }
    KosyncMemoEntry* e = &m->e[i];
    memcpy(e->book, id, 33);
    e->movedAt = ksGet32(r + 32);
    e->offeredSig = ksGet32(r + 36);
    e->used = ksGet32(r + 40);
    e->sentOk = (flags & KSM_SENT_OK) != 0;
    e->sentPct = e->sentOk ? micro / 1000000.0 : 0.0;
    e->unsent = (flags & KSM_UNSENT) != 0;
    if (e->used > m->tick) {
      m->tick = e->used;
    }
  }
  return true;
}

// ---------------------------------------------------------------- the client's retries
uint32_t kosyncRetryDelayMs(int failures) {
  static const uint32_t WAIT[KOSYNC_CLIENT_TRIES - 1] = { 1000u, 4000u };
  if (failures < 1 || failures >= KOSYNC_CLIENT_TRIES) {
    return 0;
  }
  return WAIT[failures - 1];
}

// ---------------------------------------------------------------- home= by NAME
bool kosyncParseIp(const char* host, uint32_t* ip) {
  if (!host || !host[0]) {
    return false;
  }
  uint8_t b[4];
  const char* p = host;
  for (int i = 0; i < 4; i++) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    unsigned v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10u + (unsigned)(*p - '0');
      if (++digits > 3 || v > 255u) {
        return false;
      }
      p++;
    }
    b[i] = (uint8_t)v;
    if (i < 3 && *p++ != '.') {
      return false;
    }
  }
  if (*p) {
    return false;
  }
  if (ip) {
    memcpy(ip, b, 4);                         // wire order, as IPAddress holds it
  }
  return true;
}

/* The labels of an mDNS name, into `lab` as (start, length) pairs, with ".local" appended when
 * `host` is one bare label. False for anything that is not one: an IP, an empty label, a label
 * over 63 bytes, a dotted name not ending in .local, or more than 8 labels. */
static int ksMdnsLabels(const char* host, const char* lab[8], uint8_t len[8]) {
  if (!host || !host[0] || kosyncParseIp(host, NULL)) {
    return 0;
  }
  size_t hl = strlen(host);
  if (host[hl - 1] == '.') {
    hl--;                                     // "covey.local." — the FQDN spelling
  }
  int n = 0;
  size_t i = 0;
  while (i < hl) {
    size_t j = i;
    while (j < hl && host[j] != '.') {
      j++;
    }
    if (j == i || j - i > 63 || n == 8) {
      return 0;
    }
    lab[n] = host + i;
    len[n] = (uint8_t)(j - i);
    n++;
    i = j + 1;
    if (j == hl - 1 && host[j] == '.') {
      return 0;                               // "covey..local": an empty label
    }
  }
  if (n == 0 || hl > KOSYNC_HOST_MAX + 8) {
    return 0;                                 // longer than home= can hold
  }
  if (n == 1) {
    lab[1] = "local";
    len[1] = 5;
    return 2;
  }
  return ksEqNoCase(lab[n - 1], len[n - 1], "local") ? n : 0;
}

bool kosyncHostIsMdns(const char* host) {
  const char* lab[8];
  uint8_t len[8];
  return ksMdnsLabels(host, lab, len) > 0;
}

size_t kosyncMdnsQuery(const char* host, uint16_t id, uint8_t* out, size_t cap) {
  const char* lab[8];
  uint8_t len[8];
  const int n = ksMdnsLabels(host, lab, len);
  if (!n || !out) {
    return 0;
  }
  size_t need = 12 + 1 + 4;
  for (int i = 0; i < n; i++) {
    need += 1 + len[i];
  }
  if (need > cap || need - 16 > 255) {
    return 0;
  }
  memset(out, 0, 12);
  out[0] = (uint8_t)(id >> 8);
  out[1] = (uint8_t)id;
  out[5] = 1;                                 // QDCOUNT 1; flags 0: a standard query
  size_t k = 12;
  for (int i = 0; i < n; i++) {
    out[k++] = len[i];
    memcpy(out + k, lab[i], len[i]);
    k += len[i];
  }
  out[k++] = 0;
  out[k++] = 0; out[k++] = 1;                 // QTYPE A
  out[k++] = 0; out[k++] = 1;                 // QCLASS IN, QU bit clear
  return k;
}

/* Read the name at `off` into `out` as lower-case dotted text ("covey.local"), following
 * compression pointers. Returns the offset just past the name AS WRITTEN at `off` (a pointer
 * ends it), or 0 when it runs off the packet or loops. A name longer than `cap` is walked to
 * its end all the same and comes back as "" — it is not ours, and the records after it still
 * count. */
static size_t ksDnsName(const uint8_t* pkt, size_t len, size_t off, char* out, size_t cap) {
  size_t o = 0, end = 0;
  bool fits = true;
  int jumps = 0;
  for (;;) {
    if (off >= len) {
      return 0;
    }
    const uint8_t c = pkt[off];
    if (c == 0) {
      if (!end) {
        end = off + 1;
      }
      break;
    }
    if ((c & 0xC0) == 0xC0) {
      if (off + 1 >= len || ++jumps > 16) {
        return 0;                             // off the end, or a loop
      }
      if (!end) {
        end = off + 2;
      }
      off = ((size_t)(c & 0x3F) << 8) | pkt[off + 1];
      continue;
    }
    if (c & 0xC0) {
      return 0;                               // the reserved label types
    }
    if (off + 1 + c > len) {
      return 0;
    }
    if (o + c + 2 > cap) {
      fits = false;                           // too long to be ours: keep walking, keep nothing
    }
    if (fits) {
      if (o) {
        out[o++] = '.';
      }
      for (size_t i = 0; i < c; i++) {
        out[o++] = ksLower((char)pkt[off + 1 + i]);
      }
    }
    off += 1 + c;
  }
  out[fits ? o : 0] = '\0';
  return end;
}

uint32_t kosyncMdnsAnswer(const uint8_t* pkt, size_t len, const char* host, uint16_t id) {
  const char* lab[8];
  uint8_t ln[8];
  const int n = ksMdnsLabels(host, lab, ln);
  if (!pkt || len < 12 || !n) {
    return 0;
  }
  char want[KOSYNC_HOST_MAX + 16];            // (ksMdnsLabels keeps it to home='s size)
  size_t w = 0;
  for (int i = 0; i < n; i++) {
    if (i) {
      want[w++] = '.';
    }
    for (size_t j = 0; j < ln[i]; j++) {
      want[w++] = ksLower(lab[i][j]);
    }
  }
  want[w] = '\0';
  if ((((uint16_t)pkt[0] << 8) | pkt[1]) != id || !(pkt[2] & 0x80)) {
    return 0;                                 // not our query's answer, or not an answer at all
  }
  const unsigned qd = ((unsigned)pkt[4] << 8) | pkt[5];
  const unsigned rr = (((unsigned)pkt[6] << 8) | pkt[7]) + (((unsigned)pkt[8] << 8) | pkt[9]) +
                      (((unsigned)pkt[10] << 8) | pkt[11]);
  size_t off = 12;
  char name[KOSYNC_HOST_MAX + 32];
  for (unsigned i = 0; i < qd; i++) {
    const size_t e = ksDnsName(pkt, len, off, name, sizeof(name));
    if (!e || e + 4 > len) {
      return 0;
    }
    off = e + 4;
  }
  for (unsigned i = 0; i < rr; i++) {
    const size_t e = ksDnsName(pkt, len, off, name, sizeof(name));
    if (!e || e + 10 > len) {
      return 0;
    }
    const uint8_t* r = pkt + e;
    const unsigned type = ((unsigned)r[0] << 8) | r[1];
    const unsigned cls = (((unsigned)r[2] << 8) | r[3]) & 0x7FFFu;   // the cache-flush bit
    const uint32_t ttl = ((uint32_t)r[4] << 24) | ((uint32_t)r[5] << 16) | ((uint32_t)r[6] << 8) | r[7];
    const unsigned rdlen = ((unsigned)r[8] << 8) | r[9];
    if (e + 10 + rdlen > len) {
      return 0;
    }
    if (type == 1 && cls == 1 && rdlen == 4 && ttl != 0 && !strcmp(name, want)) {
      uint32_t ip;
      memcpy(&ip, r + 10, 4);
      if (ip) {
        return ip;
      }
    }
    off = e + 10 + rdlen;
  }
  return 0;
}

static bool ksSameHome(const KosyncHomeAddr* a, const char* host, const char* ssid) {
  return a->ip && ksEqNoCase(a->host, strlen(a->host), host ? host : "") &&
         !strcmp(a->ssid, ssid ? ssid : "");
}

int kosyncHomePlan(const KosyncHomeAddr* a, const char* host, const char* ssid, uint32_t assoc,
                   uint32_t* ip) {
  uint32_t lit = 0;
  if (kosyncParseIp(host, &lit)) {
    *ip = lit;
    return KOSYNC_HOME_USE;
  }
  if (a && assoc && a->assoc == assoc && !a->stale && ksSameHome(a, host, ssid)) {
    *ip = a->ip;
    return KOSYNC_HOME_USE;
  }
  *ip = 0;
  return KOSYNC_HOME_LOOKUP;
}

uint32_t kosyncHomeLooked(KosyncHomeAddr* a, const char* host, const char* ssid, uint32_t assoc,
                          uint32_t answer, bool* fallback, bool* changed) {
  *fallback = false;
  *changed = false;
  if (answer) {
    *changed = !ksSameHome(a, host, ssid) || a->ip != answer || strcmp(a->host, host) != 0;
    ksCopy(a->host, sizeof(a->host), host);
    ksCopy(a->ssid, sizeof(a->ssid), ssid);
    a->ip = answer;
    a->assoc = assoc;
    a->stale = false;
    return answer;
  }
  if (ksSameHome(a, host, ssid)) {
    *fallback = true;                         // unconfirmed: kosyncHomeReached confirms it
    return a->ip;
  }
  return 0;
}

void kosyncHomeReached(KosyncHomeAddr* a, const char* host, const char* ssid, uint32_t assoc,
                       uint32_t ip) {
  if (kosyncParseIp(host, NULL) || !ip || a->ip != ip || !ksSameHome(a, host, ssid)) {
    return;
  }
  a->assoc = assoc;
  a->stale = false;
}

void kosyncHomeGaveUp(KosyncHomeAddr* a, uint32_t ip) {
  if (ip && a->ip == ip) {
    a->stale = true;
  }
}

size_t kosyncHomePack(const KosyncHomeAddr* a, uint8_t* out, size_t cap) {
  if (!a || !out || cap < KOSYNC_HOME_BLOB_BYTES || !a->ip || !a->host[0]) {
    return 0;
  }
  memset(out, 0, KOSYNC_HOME_BLOB_BYTES);
  memcpy(out, "KSH1", 4);
  ksCopy((char*)out + 4, KOSYNC_HOST_MAX, a->host);
  ksCopy((char*)out + 4 + KOSYNC_HOST_MAX, 33, a->ssid);
  memcpy(out + 4 + KOSYNC_HOST_MAX + 33, &a->ip, 4);
  return KOSYNC_HOME_BLOB_BYTES;
}

bool kosyncHomeUnpack(KosyncHomeAddr* a, const uint8_t* in, size_t len) {
  if (!a) {
    return false;
  }
  memset(a, 0, sizeof(*a));
  if (!in || len != KOSYNC_HOME_BLOB_BYTES || memcmp(in, "KSH1", 4) != 0 ||
      !memchr(in + 4, 0, KOSYNC_HOST_MAX) || !memchr(in + 4 + KOSYNC_HOST_MAX, 0, 33)) {
    return false;
  }
  memcpy(a->host, in + 4, KOSYNC_HOST_MAX);
  memcpy(a->ssid, in + 4 + KOSYNC_HOST_MAX, 33);
  memcpy(&a->ip, in + 4 + KOSYNC_HOST_MAX + 33, 4);
  if (!a->host[0] || !a->ip) {
    memset(a, 0, sizeof(*a));
    return false;
  }
  return true;                                // assoc 0: good for no association until looked up
}

// ---------------------------------------------------------------- the window's transport
int kosyncStationWindowCheck(bool switchedOff, bool connected, uint32_t ipNow, uint32_t ipOpened,
                             uint32_t* lostSinceMs, uint32_t now, uint32_t graceMs) {
  if (switchedOff) {
    return KOSYNC_STA_LOST;                   // on purpose, not a blip: no grace
  }
  if (connected) {
    *lostSinceMs = 0;
    return (ipOpened && ipNow && ipNow != ipOpened) ? KOSYNC_STA_MOVED : KOSYNC_STA_OK;
  }
  if (!*lostSinceMs) {
    *lostSinceMs = now ? now : 0xFFFFFFFFu;   // 0 is "connected": a drop AT 0 counts from -1 ms
  }
  return ((uint32_t)(now - *lostSinceMs) >= graceMs) ? KOSYNC_STA_LOST : KOSYNC_STA_BLIP;
}

bool kosyncWindowWaitsForSta(bool radioOff, bool userDisabled, bool staMode, uint32_t now,
                             uint32_t lastJoinMs, uint32_t lastUpMs) {
  if (radioOff || userDisabled || !staMode) {
    return false;
  }
  const bool joining = lastJoinMs != 0 && (uint32_t)(now - lastJoinMs) < 10000u;
  const bool wasUp = lastUpMs != 0 && (uint32_t)(now - lastUpMs) < 10000u;
  return joining || wasUp;
}

bool kosyncWindowWaitsForHome(bool onWifi, bool homeConfigured, bool homeStarted) {
  // All three, or the window opens now: a home nobody asked is nothing to wait for.
  return onWifi && homeConfigured && homeStarted;
}

size_t kosyncWindowProblems(uint32_t otherBook, const char* docId, uint32_t unauth,
                            uint32_t unmarked, char* out, size_t cap) {
  if (!cap) {
    return 0;
  }
  out[0] = '\0';
  char a[96] = "", b[64] = "", c[96] = "";
  if (otherBook) {
    snprintf(a, sizeof(a), "! A place for a DIFFERENT book arrived%s%s%s - not the same file here?",
             (docId && docId[0]) ? " (id " : "", (docId && docId[0]) ? docId : "",
             (docId && docId[0]) ? "..)" : "");
    if (otherBook > 1) {
      char n[16];
      snprintf(n, sizeof(n), " (x%u)", (unsigned)otherBook);
      strncat(a, n, sizeof(a) - strlen(a) - 1);
    }
  }
  if (unauth) {
    snprintf(b, sizeof(b), "! Wrong user/password from the reader (x%u)", (unsigned)unauth);
  }
  if (unmarked) {
    // The reader shows "No progress found" and does not know why; this line says why.
    snprintf(c, sizeof(c), "! A reader that can't take a percentage asked (x%u) - told "
             "'No progress found' (KOReader?)", (unsigned)unmarked);
  }
  snprintf(out, cap, "%s%s%s%s%s", a, (a[0] && b[0]) ? "  " : "", b,
           ((a[0] || b[0]) && c[0]) ? "  " : "", c);
  return strlen(out);
}

// ---------------------------------------------------------------- who PUT what (KS-3)
uint32_t kosyncPeerKey(const char* device, const char* deviceId) {
  const bool id = deviceId && deviceId[0];
  const char* s = id ? deviceId : device;
  if (!s || !s[0]) {
    return 0;                           // names nobody: never "the same device" as anyone
  }
  uint32_t h = 2166136261u;             // FNV-1a over "i" or "n", then the id or the name
  h = (h ^ (uint8_t)(id ? 'i' : 'n')) * 16777619u;
  for (const unsigned char* q = (const unsigned char*)s; *q; q++) {
    h = (h ^ *q) * 16777619u;
  }
  return h ? h : 1u;
}

static bool ksPutLogHasOwn(const KosyncPutLog* l, uint32_t who) {
  for (int i = 0; who && i < l->nOwn && i < KOSYNC_PUTLOG_MAX; i++) {
    if (l->own[i] == who) {
      return true;
    }
  }
  return false;
}

void kosyncPutLogOwn(KosyncPutLog* l, const char* device, const char* deviceId) {
  const uint32_t who = kosyncPeerKey(device, deviceId);
  if (!l || !who || ksPutLogHasOwn(l, who) || l->nOwn >= KOSYNC_PUTLOG_MAX) {
    return;                             // (full: that device's other PUTs stay "different")
  }
  l->own[l->nOwn++] = who;
}

void kosyncPutLogOther(KosyncPutLog* l, const char* device, const char* deviceId, const char* docId) {
  if (!l) {
    return;
  }
  const uint32_t who = kosyncPeerKey(device, deviceId);
  char d[16];
  ksCopy(d, sizeof(d), docId ? docId : "");
  for (int i = 0; i < l->nOther && i < KOSYNC_PUTLOG_MAX; i++) {
    if (l->by[i] == who && !strcmp(l->doc[i], d)) {
      l->n[i]++;                        // the same device, the same document: one more of it
      return;
    }
  }
  if (l->nOther >= KOSYNC_PUTLOG_MAX) {
    l->unlogged++;                      // no room to tell it apart: it counts as different
    return;
  }
  l->by[l->nOther] = who;
  memcpy(l->doc[l->nOther], d, sizeof(d));
  l->n[l->nOther] = 1;
  l->nOther++;
}

uint32_t kosyncPutLogDifferent(const KosyncPutLog* l, const char** doc, uint32_t* secondIds) {
  uint32_t diff = 0, second = 0;
  const char* last = "";
  if (l) {
    for (int i = 0; i < l->nOther && i < KOSYNC_PUTLOG_MAX; i++) {
      if (ksPutLogHasOwn(l, l->by[i])) {
        second += l->n[i];              // that device ALSO sent this window's book: its other id
      } else {
        diff += l->n[i];
        last = l->doc[i];
      }
    }
    diff += l->unlogged;
  }
  if (doc) {
    *doc = last;
  }
  if (secondIds) {
    *secondIds = second;
  }
  return diff;
}

bool kosyncReloadClears(bool asked, bool readBefore, uint32_t sigBefore, uint32_t sigNow) {
  return asked || (readBefore && sigBefore != sigNow);
}

bool kosyncNotABook(const char* basename) {
  if (!basename) {
    return false;
  }
  return ksEqNoCase(basename, strlen(basename), "kosync.txt") ||
         ksEqNoCase(basename, strlen(basename), "smsmirror.txt") ||
         ksEqNoCase(basename, strlen(basename), "gemini.txt");   // Menu > AI's key: opened as a book it would put the key on screen
}

static void ksReply(char* reply, size_t cap, const char* s) {
  ksCopy(reply, cap, s);
}

void kosyncServe(const char* method, const char* path, const char* hdrs,
                 const char* body, size_t bodyLen,
                 const char* user, const char* key, const KosyncServed* book,
                 uint32_t nowUnix, KosyncServeOut* out, char* reply, size_t replyCap) {
  memset(out, 0, sizeof(*out));
  char doc[KOSYNC_DOC_MAX];
  const KosyncRoute r = kosyncRoute(method, path, doc, sizeof(doc));
  switch (r) {
    case KOSYNC_ROUTE_HEALTH:
      out->code = 200;
      ksReply(reply, replyCap, "{\"state\":\"OK\"}");
      return;
    case KOSYNC_ROUTE_CREATE:
      /* One account, from the card. Registering over the air would mean anyone who joins the
       * open hotspot could make themselves a user. */
      out->code = 402;
      ksReply(reply, replyCap, "{\"message\":\"User registration is disabled.\",\"code\":2005}");
      return;
    case KOSYNC_ROUTE_NONE:
      out->code = 404;
      ksReply(reply, replyCap, "{\"message\":\"Not found\"}");
      return;
    default:
      break;
  }
  if (!kosyncAuthOk(hdrs, user, key)) {
    out->code = 401;
    ksReply(reply, replyCap, "{\"message\":\"Unauthorized\",\"code\":2001}");
    return;
  }
  if (r == KOSYNC_ROUTE_AUTH) {
    out->code = 200;
    ksReply(reply, replyCap, "{\"authorized\":\"OK\"}");
    return;
  }
  const bool haveBook = book && book->partial && book->byName;
  if (r == KOSYNC_ROUTE_GET_PROGRESS) {
    out->code = 200;
    if (!haveBook || (strcmp(doc, book->partial) != 0 && strcmp(doc, book->byName) != 0)) {
      ksReply(reply, replyCap, "{}");   // 🛑 NOT a 404: see kosync.h
      return;
    }
    out->pickedUp = true;               // our book, whatever we can say about it
    if (!kosyncClientTakesPercentage(hdrs)) {
      /* 🛑 A reader that has not said it can take a percentage (no X-BookSync, no Authorization
       * — KOReader) gets `{}`, "No progress found", not the record: our reply's empty `progress`
       * would send it to PAGE 1 (kosyncClientTakesPercentage, kosync.h). Still a pick-up: it
       * may PUT its own place next, and that lands as the card as ever. */
      out->unmarkedGet = true;
      ksReply(reply, replyCap, "{}");
      return;
    }
    if (!book->pctOk) {
      /* The phone is somewhere it cannot express as a KOSync percentage (a chapter CrossPoint
       * does not list). `{}` is the honest answer — "nothing to offer" — where a 0 or the
       * place from before the reader moved would send the peer somewhere wrong. */
      ksReply(reply, replyCap, "{}");
      return;
    }
    char p[16], d[KOSYNC_DEV_MAX * 2], di[KOSYNC_DEV_MAX * 2];
    kosyncFormatPct(book->pct, p, sizeof(p));
    kosyncJsonEscape(book->device, d, sizeof(d));
    kosyncJsonEscape(book->deviceId, di, sizeof(di));
    // `doc` equals one of our two hex ids here, so it needs no escaping.
    snprintf(reply, replyCap,
             "{\"document\":\"%s\",\"percentage\":%s,\"progress\":\"\",\"device\":\"%s\","
             "\"device_id\":\"%s\",\"timestamp\":%lu}",
             doc, p, d, di, (unsigned long)book->turnedAt);
    return;
  }
  // PUT /syncs/progress
  KosyncProgress in;
  if (!kosyncParseProgress(body, bodyLen, &in) || !in.document[0] || !in.hasPct) {
    out->code = 400;
    ksReply(reply, replyCap, "{\"message\":\"Invalid request\",\"code\":2003}");
    return;
  }
  char dc[KOSYNC_DOC_MAX * 2];
  kosyncJsonEscape(in.document, dc, sizeof(dc));
  out->code = 200;
  snprintf(reply, replyCap, "{\"document\":\"%s\",\"timestamp\":%lu}", dc, (unsigned long)nowUnix);
  // Who sent it, for ANY PUT: a PUT for another document is told apart by it too (KS-3).
  ksCopy(out->putDevice, sizeof(out->putDevice), in.device);
  ksCopy(out->putDeviceId, sizeof(out->putDeviceId), in.deviceId);
  if (!haveBook || (strcmp(in.document, book->partial) != 0 && strcmp(in.document, book->byName) != 0)) {
    out->otherDoc = true;               // answered, and dropped: this window is one book
    snprintf(out->otherDocId, sizeof(out->otherDocId), "%.8s", in.document);   // for the screen
    return;
  }
  out->pickedUp = true;
  out->gotPut = true;
  out->putPct = in.pct < 0.0 ? 0.0 : (in.pct > 1.0 ? 1.0 : in.pct);
  // With no place of our own to compare against, any other device's place is worth a card.
  out->park = book->pctOk
              ? kosyncWorthParking(out->putPct, in.device, in.deviceId,
                                   book->pct, book->device, book->deviceId)
              : !kosyncIsOwnRecord(in.device, in.deviceId, book->device, book->deviceId);
}

// ---------------------------------------------------------------- the window's clock
void kosyncClockOpen(KosyncWindowClock* c, uint32_t now, uint32_t durationMs) {
  c->open = true;
  c->openedMs = now;
  c->durationMs = durationMs;
  c->picked = false;
  c->pickedMs = 0;
  c->graceMs = 0;
}

void kosyncClockPicked(KosyncWindowClock* c, uint32_t now, uint32_t graceMs) {
  if (!c->open) {
    return;
  }
  c->picked = true;
  c->pickedMs = now;
  c->graceMs = graceMs;
}

int kosyncClockDue(const KosyncWindowClock* c, uint32_t now) {
  if (!c->open) {
    return KOSYNC_CLOCK_CLOSED;
  }
  // Unsigned differences: correct across the 49-day millis() wrap.
  if ((uint32_t)(now - c->openedMs) >= c->durationMs) {
    return KOSYNC_CLOCK_DEADLINE;
  }
  if (c->picked && (uint32_t)(now - c->pickedMs) >= c->graceMs) {
    return KOSYNC_CLOCK_PICKED;
  }
  return KOSYNC_CLOCK_RUNNING;
}

uint32_t kosyncClockRemainingMs(const KosyncWindowClock* c, uint32_t now) {
  if (kosyncClockDue(c, now) != KOSYNC_CLOCK_RUNNING) {
    return 0;
  }
  uint32_t left = c->durationMs - (uint32_t)(now - c->openedMs);
  if (c->picked) {
    const uint32_t g = c->graceMs - (uint32_t)(now - c->pickedMs);
    if (g < left) {
      left = g;
    }
  }
  return left;
}

// ---------------------------------------------------------------- config
static bool ksBool(const char* v, bool* out) {
  static const char* ON[] = { "on", "yes", "true", "1" };
  static const char* OFF[] = { "off", "no", "false", "0" };
  for (int i = 0; i < 4; i++) {
    if (ksEqNoCase(v, strlen(v), ON[i])) { *out = true; return true; }
    if (ksEqNoCase(v, strlen(v), OFF[i])) { *out = false; return true; }
  }
  return false;
}

static bool ksIsHex32(const char* v) {
  if (strlen(v) != 32) {
    return false;
  }
  for (int i = 0; i < 32; i++) {
    const char c = v[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

bool kosyncHotspotPassValid(const char* v, const char** why) {
  const size_t n = v ? strlen(v) : 0;
  if (n < 8 || n > 63) {
    /* softAP() itself refuses 1-7 (and would then bring NO hotspot up at all), and its
     * config field holds 63; a 64-hex raw PSK is not accepted here either. */
    if (why) *why = "hotspot_pass ignored: 8-63 characters";
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)v[i];
    if (c < 0x20 || c > 0x7E) {
      if (why) *why = "hotspot_pass ignored: plain ASCII only";
      return false;
    }
  }
  if (why) *why = "";
  return true;
}

bool kosyncParseConfig(const char* text, size_t len, KosyncConfig* out) {
  memset(out, 0, sizeof(*out));
  out->homePort = 80;
  bool havePw = false;
  /* Why a password=/key= line was REFUSED. Kept apart from `problem` and put first at the end:
   * written straight into `problem`, a later home= complaint overwrote it, and the screen said
   * only "home= has a bad port" for a phone whose KOSync was OFF for want of a password — fix
   * that, and only then learn about the password. */
  char secretWhy[64] = "";
  const char* p = text;
  const char* e = text ? text + len : text;
  char line[160];
  while (p && p < e) {
    const char* eol = p;
    while (eol < e && *eol != '\n') {
      eol++;
    }
    size_t n = (size_t)(eol - p);
    if (n > sizeof(line) - 1) {
      n = sizeof(line) - 1;           // an absurd line: cut, and it will not parse as a key
    }
    memcpy(line, p, n);
    line[n] = '\0';
    p = eol < e ? eol + 1 : e;
    // Trim both ends (CR included: the file may come from Windows).
    char* s = line;
    while (*s == ' ' || *s == '\t') s++;
    char* t = s + strlen(s);
    while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r')) *--t = '\0';
    if (!*s || *s == '#') {
      continue;
    }
    char* eq = strchr(s, '=');
    if (!eq) {
      continue;
    }
    *eq = '\0';
    char* k = s;
    char* v = eq + 1;
    char* ke = eq;
    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
    for (char* q = k; *q; q++) *q = ksLower(*q);
    /* An inline comment: '#' with whitespace right before it (so "#" INSIDE a value, as in
     * "pass#word", is not one). Found on the RAW value, before its leading blanks go. */
    char* hash = NULL;
    for (char* q = v; *q; q++) {
      if (q > v && *q == '#' && (q[-1] == ' ' || q[-1] == '\t')) {
        hash = q;
        break;
      }
    }
    const bool secret = !strcmp(k, "password") || !strcmp(k, "key") || !strcmp(k, "hotspot_pass");
    if (hash && secret) {
      /* 🛑 REFUSED, NOT GUESSED. "password=hunter2secret  # mine" is either a comment or a
       * password with " # mine" in it, and the one wrong guess is a 401 on every exchange (or
       * a hotspot nobody can join) with nothing anywhere saying why. The line is dropped and
       * the reason is what Sync settings and `kosync` show. */
      if (!strcmp(k, "hotspot_pass")) {
        out->hotspotPass[0] = '\0';
        ksCopy(out->hotspotNote, sizeof(out->hotspotNote), "hotspot_pass ignored: ' #' in it");
      } else {
        snprintf(secretWhy, sizeof(secretWhy), "%s= has ' #' - put comments on their own line", k);
      }
      memset(line, 0, sizeof(line));   // no stray copy of a secret on the stack
      continue;
    }
    if (hash) {
      *hash = '\0';                    // a comment on a plain setting: cut it off
      char* te = hash;
      while (te > v && (te[-1] == ' ' || te[-1] == '\t')) *--te = '\0';
    }
    while (*v == ' ' || *v == '\t') v++;

    if (!strcmp(k, "user")) {
      ksCopy(out->user, sizeof(out->user), v);
    } else if (!strcmp(k, "password")) {
      if (*v) {
        bsMd5Hex(v, strlen(v), out->key);
        out->passwordShort = strlen(v) < 12;
        havePw = true;
      }
      memset(line, 0, sizeof(line));  // the plaintext does not outlive this line
    } else if (!strcmp(k, "key")) {
      if (ksIsHex32(v)) {
        for (int i = 0; i < 32; i++) out->key[i] = ksLower(v[i]);
        out->key[32] = '\0';
        havePw = true;
      } else {
        ksCopy(secretWhy, sizeof(secretWhy), "key= must be 32 hex digits (MD5 of the password)");
      }
    } else if (!strcmp(k, "home")) {
      if (!strncmp(v, "https://", 8)) {
        ksCopy(out->problem, sizeof(out->problem), "home= must be http:// (no TLS here)");
        continue;
      }
      if (!strncmp(v, "http://", 7)) {
        v += 7;
      }
      char h[KOSYNC_HOST_MAX + 8];
      ksCopy(h, sizeof(h), v);
      size_t hl = strlen(h);
      while (hl && h[hl - 1] == '/') h[--hl] = '\0';
      char* colon = strrchr(h, ':');
      if (colon) {
        *colon = '\0';
        const long port = strtol(colon + 1, NULL, 10);
        if (port > 0 && port < 65536) {
          out->homePort = (uint16_t)port;
        } else {
          ksCopy(out->problem, sizeof(out->problem), "home= has a bad port");
        }
      }
      if (strchr(h, '/')) {
        ksCopy(out->problem, sizeof(out->problem), "home= takes a host[:port], not a path");
        continue;
      }
      ksCopy(out->home, sizeof(out->home), h);
    } else if (!strcmp(k, "device")) {
      ksCopy(out->device, sizeof(out->device), v);
    } else if (!strcmp(k, "auto") || !strcmp(k, "open_window")) {
      // A value that is neither on nor off leaves the switch OFF — and now says so.
      if (!ksBool(v, !strcmp(k, "auto") ? &out->autoOnClose : &out->openWindow) &&
          !out->problem[0]) {
        char why[64];
        snprintf(why, sizeof(why), "%s= must be on or off", k);
        ksCopy(out->problem, sizeof(out->problem), why);
      }
    } else if (!strcmp(k, "hotspot_pass")) {
      /* Leading/trailing spaces are trimmed with the line (a Windows CR too); the password
       * is what is between them. Invalid -> OPEN, and hotspotNote says why — a hotspot that
       * is silently open when its owner wrote a password is the one failure worth avoiding. */
      const char* why = "";
      if (kosyncHotspotPassValid(v, &why)) {
        ksCopy(out->hotspotPass, sizeof(out->hotspotPass), v);
        out->hotspotNote[0] = '\0';
      } else {
        out->hotspotPass[0] = '\0';
        ksCopy(out->hotspotNote, sizeof(out->hotspotNote), why);
      }
      memset(line, 0, sizeof(line));  // no stray copy of it on the stack
    }
  }
  if (!out->user[0] || !havePw) {
    /* The reason it is OFF, first: with no usable password, a refused secret line IS the
     * reason, whatever else the file got wrong after it. */
    if (!havePw && secretWhy[0]) {
      ksCopy(out->problem, sizeof(out->problem), secretWhy);
    } else if (!out->problem[0]) {
      ksCopy(out->problem, sizeof(out->problem), !out->user[0] ? "no user= line" : "no password= line");
    }
    out->ok = false;
    return false;
  }
  out->ok = true;
  if (secretWhy[0] && !out->problem[0]) {
    ksCopy(out->problem, sizeof(out->problem), secretWhy);   // on (another line's secret), but said
  }
  if (out->passwordShort && !out->problem[0]) {
    ksCopy(out->problem, sizeof(out->problem), "password is under 12 characters");
  }
  return true;
}

static uint32_t ksFnv(uint32_t h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ b[i]) * 16777619u;
  }
  return h;
}

static uint32_t ksFnvStr(uint32_t h, const char* s) {
  return ksFnv(h, s, strlen(s) + 1);          // the NUL too: "ab","c" is not "a","bc"
}

uint32_t kosyncConfigSig(const KosyncConfig* c) {
  uint32_t h = 2166136261u;
  const uint8_t flags[4] = { (uint8_t)c->ok, (uint8_t)c->passwordShort, (uint8_t)c->autoOnClose,
                             (uint8_t)c->openWindow };
  h = ksFnv(h, flags, sizeof(flags));
  h = ksFnvStr(h, c->user);
  h = ksFnvStr(h, c->key);
  h = ksFnvStr(h, c->home);
  const uint8_t port[2] = { (uint8_t)(c->homePort >> 8), (uint8_t)c->homePort };
  h = ksFnv(h, port, sizeof(port));
  h = ksFnvStr(h, c->device);
  h = ksFnvStr(h, c->problem);
  h = ksFnvStr(h, c->hotspotPass);
  h = ksFnvStr(h, c->hotspotNote);
  return h;
}

void kosyncDeviceId(const uint8_t mac[6], char out[KOSYNC_KEY_CHARS]) {
  char s[24];
  snprintf(s, sizeof(s), "WiPhone-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  bsMd5Hex(s, strlen(s), out);
}

// ---------------------------------------------------------------- into the booksync inbox
double kosyncToReaderFraction(int readIdx, double readWithin, int nRead) {
  const int n = nRead > 0 ? nRead : 1;
  double w = readWithin;
  if (!(w >= 0.0)) {
    w = 0.0;
  }
  if (w > 1.0) {
    w = 1.0;
  }
  double f = ((double)readIdx + w) / (double)n + 0.5 / 65535.0;   // see kosync.h
  if (f < 0.0) {
    f = 0.0;
  }
  if (f > 1.0) {
    f = 1.0;
  }
  return f;
}

bool kosyncParkText(const char* const* ids, int nIds, int readIdx, double readWithin, int nRead,
                    uint32_t turnedAt, const char* device, const uint8_t key[32],
                    char* out, size_t cap) {
  if (!ids || nIds <= 0 || readIdx < 0 || readIdx >= nRead) {
    return false;
  }
  BookSyncRecord r;
  /* offset 0: the peer's byte offset means nothing in OUR extraction, and applyPending() goes
   * by the fraction anyway (epubLocate), exactly as it does for a COVEY packet. */
  bookSyncMakeRecord(&r, ids, nIds, (uint32_t)readIdx, 0,
                     kosyncToReaderFraction(readIdx, readWithin, nRead), turnedAt,
                     (device && device[0]) ? device : "KOSync", NULL);
  return bookSyncPackMesh(&r, key, out, cap);
}

// ---------------------------------------------------------------- the park ledger
static int ledgerFind(const KosyncParkLedger* l, const char* book) {
  for (int i = 0; i < KOSYNC_LEDGER_MAX; i++) {
    if (l->book[i][0] && !strcmp(l->book[i], book)) {
      return i;
    }
  }
  return -1;
}

bool kosyncParkInto(KosyncParkLedger* l, const char* book, const char* text, double peerPct,
                    uint32_t rxUnix, uint32_t sig) {
  if (!l || !book || !book[0] || !text) {
    return false;
  }
  int slot = ledgerFind(l, book);
  if (slot >= 0 && l->id[slot]) {
    bookSyncInboxRemoveId(l->id[slot]);   // the offer it replaces; already-gone is fine
    l->id[slot] = 0;
  }
  if (!bookSyncInboxPush(text, 0, rxUnix)) {
    return false;
  }
  uint32_t id = 0;
  for (int i = 0; i < bookSyncInboxCount(); i++) {
    const BookSyncInboxItem* it = bookSyncInboxGet(i);
    if (it && !strcmp(it->text, text)) {
      id = it->id;
      break;
    }
  }
  if (slot < 0) {
    slot = l->next;
    l->next = (l->next + 1) % KOSYNC_LEDGER_MAX;
  }
  ksCopy(l->book[slot], sizeof(l->book[slot]), book);
  l->id[slot] = id;
  l->peerPct[slot] = peerPct;
  l->sig[slot] = id ? sig : 0;            // pending until the person answers THIS card (KS-2)
  return id != 0;
}

bool kosyncParkedPeerPct(const KosyncParkLedger* l, uint32_t inboxId, double* pct) {
  if (!l || !inboxId) {
    return false;
  }
  for (int i = 0; i < KOSYNC_LEDGER_MAX; i++) {
    if (l->book[i][0] && l->id[i] == inboxId) {
      if (pct) {
        *pct = l->peerPct[i];
      }
      return true;
    }
  }
  return false;
}

bool kosyncParkPending(const KosyncParkLedger* l, const char* book, const uint8_t* key,
                       uint32_t* sig) {
  if (sig) {
    *sig = 0;
  }
  if (!l || !book || !book[0]) {
    return false;
  }
  const int slot = ledgerFind(l, book);
  if (slot < 0 || !l->id[slot]) {
    return false;
  }
  // Still in the inbox = nobody answered it: an answer retires it, a restart or eviction loses it.
  for (int i = 0; i < bookSyncInboxCount(); i++) {
    const BookSyncInboxItem* it = bookSyncInboxGet(i);
    if (it && it->id == l->id[slot]) {
      BookSyncRecord r;
      if (key && !bookSyncUnpackMesh(it->text, key, &r)) {
        return false;                     // it can never be a card: nobody will answer it
      }
      if (sig) {
        *sig = l->sig[slot];
      }
      return true;
    }
  }
  return false;
}

bool kosyncOfferAnswered(KosyncParkLedger* l, KosyncMemo* m, uint32_t inboxId) {
  if (!l || !m || !inboxId) {
    return false;
  }
  for (int i = 0; i < KOSYNC_LEDGER_MAX; i++) {
    if (!l->book[i][0] || l->id[i] != inboxId) {
      continue;
    }
    const uint32_t sig = l->sig[i];
    l->sig[i] = 0;
    if (!sig) {
      return false;                       // a window PUT: every new PUT is offered again anyway
    }
    KosyncMemoEntry* e = kosyncMemoGet(m, l->book[i], true);
    if (!e || e->offeredSig == sig) {
      return false;
    }
    e->offeredSig = sig;                  // declined (or taken): not offered again
    m->dirty = true;
    return true;
  }
  return false;                           // a LoRa card: nothing of KOSync's
}
