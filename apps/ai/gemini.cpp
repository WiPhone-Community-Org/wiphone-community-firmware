/*
 * gemini.cpp - see gemini.h. Pure C: no Arduino, no IDF, no allocation. Every buffer is the
 * caller's (PSRAM on the phone), and nothing here recurses - the JSON reader (json_read.cpp) skips
 * nested values by counting brackets - so it is as safe on the AI worker's 8 KB stack as in the host suite.
 */
#include "gemini.h"
#include "json_read.h"     // the reader (Js, jsObjNext...): shared with weather.cpp since 0.9.81

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ── small helpers ─────────────────────────────────────────────────────────────────────────

static void gCopy(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) {
    return;
  }
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) {
    n = cap - 1;
  }
  if (n) {
    memcpy(dst, src, n);
  }
  dst[n] = '\0';
}

static void gCopyN(char* dst, size_t cap, const char* src, size_t n) {
  if (!dst || cap == 0) {
    return;
  }
  if (n >= cap) {
    n = cap - 1;
  }
  if (n) {
    memcpy(dst, src, n);
  }
  dst[n] = '\0';
}

static bool gEqNoCase(const char* a, size_t alen, const char* b) {
  const size_t bl = strlen(b);
  return alen == bl && strncasecmp(a, b, bl) == 0;
}

static bool gHasNoCase(const char* hay, const char* needle) {
  const size_t nl = strlen(needle);
  for (const char* p = hay; p && *p; p++) {
    if (strncasecmp(p, needle, nl) == 0) {
      return true;
    }
  }
  return false;
}

/* The length-returning writer every builder here uses: bytes go in while they fit, the count
 * goes on regardless, and the caller compares. A body that did not fit is left EMPTY rather
 * than cut: half a JSON body sent to Google comes back as a 400 that reads like a bad key. */
struct GemW {
  char*  out;
  size_t cap;
  size_t n;
};

static void wPut(GemW* w, const char* s, size_t len) {
  if (w->out && w->n + len < w->cap) {
    memcpy(w->out + w->n, s, len);
  }
  w->n += len;
}

static void wStr(GemW* w, const char* s) {
  wPut(w, s, strlen(s));
}

static void wChar(GemW* w, char c) {
  wPut(w, &c, 1);
}

static size_t wEnd(GemW* w) {
  if (w->out && w->cap) {
    if (w->n < w->cap) {
      w->out[w->n] = '\0';
    } else {
      w->out[0] = '\0';
    }
  }
  return w->n;
}

/* JSON-escaped, the first `n` bytes of `s` at most (it stops at a NUL too). */
static void wEscN(GemW* w, const char* s, size_t n) {
  if (!s) {
    return;
  }
  const unsigned char* end = (const unsigned char*)s + n;
  for (const unsigned char* p = (const unsigned char*)s; p < end && *p; p++) {
    const unsigned char c = *p;
    if (c == '"') {
      wStr(w, "\\\"");
    } else if (c == '\\') {
      wStr(w, "\\\\");
    } else if (c == '\n') {
      wStr(w, "\\n");
    } else if (c == '\t') {
      wStr(w, "\\t");
    } else if (c == '\r') {
      // dropped
    } else if (c < 0x20 || c == 0x7F) {
      char u[8];
      snprintf(u, sizeof(u), "\\u%04x", (unsigned)c);
      wStr(w, u);
    } else {
      wChar(w, (char)c);
    }
  }
}

static void wEsc(GemW* w, const char* s) {
  wEscN(w, s, s ? strlen(s) : 0);
}

// ── the key file ─────────────────────────────────────────────────────────────────────────

bool geminiModelNameOk(const char* m) {
  if (!m || !m[0]) {
    return false;
  }
  /* Google's model names are lowercase letters, digits, '.' and '-', starting with a letter or
   * a digit. A KEY is mixed case (and may hold '_'): one pasted on the model= line fails here,
   * so it can never reach the URL, the screen or the serial port as a "model". */
  size_t n = 0;
  for (const char* p = m; *p; p++, n++) {
    const char c = *p;
    const bool lowerOrDigit = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!(lowerOrDigit || (n > 0 && (c == '.' || c == '-')))) {
      return false;
    }
  }
  return n < GEM_MODEL_MAX;
}

static bool gSpace(char c) {
  return c == ' ' || c == '\t';
}

/* [s, e) with spaces off both ends and one pair of matching quotes off the outside. */
static void gTrimValue(const char** s, const char** e) {
  while (*s < *e && gSpace(**s)) (*s)++;
  while (*e > *s && gSpace((*e)[-1])) (*e)--;
  if (*e - *s >= 2 && ((**s == '"' && (*e)[-1] == '"') || (**s == '\'' && (*e)[-1] == '\''))) {
    (*s)++;
    (*e)--;
    while (*s < *e && gSpace(**s)) (*s)++;
    while (*e > *s && gSpace((*e)[-1])) (*e)--;
  }
}

/* A model value from the file: "models/" off the front (Google's own spelling of a name). */
static bool gModelValue(const char* s, const char* e, char* out, size_t cap) {
  if (e - s >= 7 && strncasecmp(s, "models/", 7) == 0) {
    s += 7;
  }
  char tmp[GEM_MODEL_MAX + 1];
  if ((size_t)(e - s) >= sizeof(tmp) - 1) {
    return false;
  }
  gCopyN(tmp, sizeof(tmp), s, (size_t)(e - s));
  if (!geminiModelNameOk(tmp)) {
    return false;
  }
  gCopy(out, cap, tmp);
  return true;
}

/* `name` is exactly the text [s, e) (never true for an empty or missing range). */
static bool gSameText(const char* name, const char* s, const char* e) {
  return s && e > s && strlen(name) == (size_t)(e - s) && !memcmp(name, s, (size_t)(e - s));
}

int geminiParseConfig(const char* text, size_t len, GeminiConfig* out) {
  memset(out, 0, sizeof(*out));
  gCopy(out->model, sizeof(out->model), GEM_DEFAULT_MODEL);
  gCopy(out->fallback, sizeof(out->fallback), GEM_LITE_MODEL);
  out->thinking = GEM_THINKING;
  out->lookup = true;
  if (!text) {
    len = 0;
  }
  const char* p = text;
  const char* end = text + len;
  if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
      (unsigned char)p[2] == 0xBF) {
    p += 3;                                      // a UTF-8 BOM (Notepad's habit)
  }
  const char* keyS = NULL;
  const char* keyE = NULL;
  bool keyLine = false;
  const char* bareS = NULL;
  const char* bareE = NULL;
  while (p < end) {
    const char* ls = p;
    const char* le = p;
    while (le < end && *le != '\n' && *le != '\r') le++;
    p = le;
    while (p < end && (*p == '\n' || *p == '\r')) p++;
    while (ls < le && gSpace(*ls)) ls++;
    while (le > ls && gSpace(le[-1])) le--;
    if (ls == le || *ls == '#') {
      continue;
    }
    const char* eq = (const char*)memchr(ls, '=', (size_t)(le - ls));
    if (!eq) {
      const char* s = ls;
      const char* e = le;
      gTrimValue(&s, &e);
      bool oneToken = s < e;
      for (const char* q = s; q < e; q++) {
        if (gSpace(*q)) {
          oneToken = false;
          break;
        }
      }
      if (oneToken) {
        bareS = s;                               // a lone key on its own line (last one wins)
        bareE = e;
      }
      continue;
    }
    const char* ns = ls;
    const char* ne = eq;
    while (ne > ns && gSpace(ne[-1])) ne--;
    const char* vs = eq + 1;
    const char* ve = le;
    gTrimValue(&vs, &ve);
    const size_t nl = (size_t)(ne - ns);
    if (gEqNoCase(ns, nl, "key")) {
      keyS = vs;
      keyE = ve;
      keyLine = true;
    } else if (gEqNoCase(ns, nl, "model")) {
      if (vs == ve) {
        gCopy(out->model, sizeof(out->model), GEM_DEFAULT_MODEL);
        out->modelSet = false;
        out->warn &= ~GEM_WARN_MODEL;
      } else if (gModelValue(vs, ve, out->model, sizeof(out->model))) {
        out->modelSet = true;
        out->warn &= ~GEM_WARN_MODEL;
      } else {
        gCopy(out->model, sizeof(out->model), GEM_DEFAULT_MODEL);
        out->modelSet = false;
        out->warn |= GEM_WARN_MODEL;
      }
    } else if (gEqNoCase(ns, nl, "thinking")) {
      long v = -1;
      if (vs < ve && (size_t)(ve - vs) <= 5) {
        v = 0;
        for (const char* q = vs; q < ve; q++) {
          if (!isdigit((unsigned char)*q)) {
            v = -1;
            break;
          }
          v = v * 10 + (*q - '0');
        }
      }
      if (v >= 0 && v <= GEM_THINKING_MAX) {
        out->thinking = (int)v;
        out->warn &= ~GEM_WARN_THINKING;
      } else {
        out->thinking = GEM_THINKING;
        out->warn |= GEM_WARN_THINKING;
      }
    } else if (gEqNoCase(ns, nl, "fallback")) {
      if (gEqNoCase(vs, (size_t)(ve - vs), "none") || gEqNoCase(vs, (size_t)(ve - vs), "off")) {
        out->fallback[0] = '\0';
        out->warn &= ~GEM_WARN_FALLBACK;
      } else if (vs == ve) {
        gCopy(out->fallback, sizeof(out->fallback), GEM_LITE_MODEL);
        out->warn &= ~GEM_WARN_FALLBACK;
      } else if (gModelValue(vs, ve, out->fallback, sizeof(out->fallback))) {
        out->warn &= ~GEM_WARN_FALLBACK;
      } else {
        gCopy(out->fallback, sizeof(out->fallback), GEM_LITE_MODEL);
        out->warn |= GEM_WARN_FALLBACK;
      }
    } else if (gEqNoCase(ns, nl, "lookup")) {
      /* The Wikipedia lookup before each question (wiki_lookup.h): `off` stops it, `on` (and an
       * empty value) is the default; anything else is a typo - the default, and Key info says so. */
      if (gEqNoCase(vs, (size_t)(ve - vs), "off")) {
        out->lookup = false;
        out->warn &= ~GEM_WARN_LOOKUP;
      } else if (vs == ve || gEqNoCase(vs, (size_t)(ve - vs), "on")) {
        out->lookup = true;
        out->warn &= ~GEM_WARN_LOOKUP;
      } else {
        out->lookup = true;
        out->warn |= GEM_WARN_LOOKUP;
      }
    }
    // Anything else: a line for another device, or a typo - ignored, like the T-Deck does.
  }
  if (!keyLine) {
    keyS = bareS;
    keyE = bareE;
  }
  /* A model name that IS the key (a key that happens to be all lowercase, pasted on both lines):
   * dropped like any other value that is not a model name. */
  const size_t kl = keyS ? (size_t)(keyE - keyS) : 0;
  // ...and one that is the key line's first word (a key refused for a trailing comment).
  const char* kw = keyS;
  while (kw && kw < keyE && !gSpace(*kw)) kw++;
  if (gSameText(out->model, keyS, keyE) || gSameText(out->model, keyS, kw)) {
    gCopy(out->model, sizeof(out->model), GEM_DEFAULT_MODEL);
    out->modelSet = false;
    out->warn |= GEM_WARN_MODEL;
  }
  if (gSameText(out->fallback, keyS, keyE) || gSameText(out->fallback, keyS, kw)) {
    gCopy(out->fallback, sizeof(out->fallback), GEM_LITE_MODEL);
    out->warn |= GEM_WARN_FALLBACK;
  }
  if (!keyS || keyS == keyE) {
    return GEM_CFG_NO_KEY;
  }
  for (const char* q = keyS; q < keyE; q++) {
    const unsigned char c = (unsigned char)*q;
    if (c <= 0x20 || c >= 0x7F) {
      out->problem = gSpace((char)c)
        ? "the key line has a space in it - put a comment on a line of its own"
        : "the key has an odd character in it";
      return GEM_CFG_BAD_KEY;
    }
  }
  if (kl >= GEM_KEY_MAX) {
    out->problem = "the key is too long";
    return GEM_CFG_BAD_KEY;
  }
  memcpy(out->key, keyS, kl);
  out->key[kl] = '\0';
  out->keyLen = (int)kl;
  bool shapeOk = kl >= 20 && kl <= 200;
  for (size_t i = 0; i < kl && shapeOk; i++) {
    const char c = out->key[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
      shapeOk = false;
    }
  }
  if (!shapeOk) {
    out->warn |= GEM_WARN_SHAPE;
  }
  if (kl >= 80) {
    out->warn |= GEM_WARN_TDECK;
  }
  return GEM_CFG_OK;
}

/* geminiConfigLine's writer. Unlike GemW - a request body that does not fit is left EMPTY - a
 * status line that does not fit is CUT, "..." at its end (review 2026-10-11: two long model names
 * and ", Wikipedia lookup off" passed Key info's 200 bytes, and the whole line came out EMPTY).
 * Pieces go in while there is room; *n counts them all. */
static void lineAdd(char* out, size_t cap, size_t* n, const char* s) {
  const size_t l = strlen(s);
  if (out && cap && *n + 1 < cap) {
    const size_t room = cap - 1 - *n;
    memcpy(out + *n, s, l < room ? l : room);
  }
  *n += l;
}

size_t geminiConfigLine(const GeminiConfig* c, int parsed, char* out, size_t cap) {
  size_t n = 0;
  char num[48];
  if (parsed == GEM_CFG_OK) {
    snprintf(num, sizeof(num), "key: set (%d chars)", c->keyLen);
    lineAdd(out, cap, &n, num);
  } else if (parsed == GEM_CFG_BAD_KEY) {
    lineAdd(out, cap, &n, "key: refused - ");
    lineAdd(out, cap, &n, c->problem ? c->problem : "unreadable");
  } else {
    lineAdd(out, cap, &n, "key: none");
  }
  lineAdd(out, cap, &n, ", model ");
  lineAdd(out, cap, &n, c->model);
  if (c->thinking) {
    snprintf(num, sizeof(num), ", thinking %d", c->thinking);
    lineAdd(out, cap, &n, num);
  } else {
    lineAdd(out, cap, &n, ", thinking off");
  }
  lineAdd(out, cap, &n, ", fallback ");
  lineAdd(out, cap, &n, c->fallback[0] ? c->fallback : "none");
  if (!c->lookup)                  lineAdd(out, cap, &n, ", Wikipedia lookup off");
  if (c->warn & GEM_WARN_SHAPE)    lineAdd(out, cap, &n, "; the key's shape is unusual (sent anyway)");
  if (c->warn & GEM_WARN_TDECK)    lineAdd(out, cap, &n, "; 80+ chars (a T-Deck keeps 79)");
  if (c->warn & GEM_WARN_MODEL)    lineAdd(out, cap, &n, "; model= line ignored (not a model name)");
  if (c->warn & GEM_WARN_THINKING) lineAdd(out, cap, &n, "; thinking= must be 0-8192 (default used)");
  if (c->warn & GEM_WARN_FALLBACK) lineAdd(out, cap, &n, "; fallback= line ignored (not a model name)");
  if (c->warn & GEM_WARN_LOOKUP)   lineAdd(out, cap, &n, "; lookup= must be on or off (on used)");
  if (out && cap) {
    if (n < cap) {
      out[n] = '\0';
    } else {
      out[cap - 1] = '\0';
      if (cap >= 4) {
        memcpy(out + cap - 4, "...", 3);         // cut, and says so: never an empty line
      }
    }
  }
  return n;
}

void geminiModelLabel(const char* model, char* out, size_t cap) {
  if (!out || cap == 0) {
    return;
  }
  const char* s = model ? model : "";
  if (!strncasecmp(s, "models/", 7)) s += 7;
  if (!strncasecmp(s, "gemini-", 7)) s += 7;
  size_t n = strlen(s);
  if (n > 7 && !strcasecmp(s + n - 7, "-latest")) n -= 7;
  if (n == 0) {
    gCopy(out, cap, model);
    return;
  }
  gCopyN(out, cap, s, n);
  bool start = true;
  for (char* p = out; *p; p++) {
    if (start && *p >= 'a' && *p <= 'z') {
      *p = (char)(*p - 'a' + 'A');
    }
    start = (*p == '-');
  }
}

// ── the chat ─────────────────────────────────────────────────────────────────────────────

void geminiChatClear(GeminiChat* c) {
  c->n = 0;
  c->breakPending = false;
}

bool geminiChatTopicDue(const GeminiChat* c, int64_t now) {
  if (c->n == 0 || c->breakPending) {
    return true;
  }
  const int64_t last = c->ex[c->n - 1].t;
  return last > 0 && now > 0 && now - last > GEM_TOPIC_GAP_S;
}

static void chatDropOldest(GeminiChat* c) {
  if (c->n <= 0) {
    return;
  }
  memmove(&c->ex[0], &c->ex[1], sizeof(GeminiExchange) * (size_t)(c->n - 1));
  c->n--;
}

void geminiChatAdd(GeminiChat* c, int64_t t, bool topic, bool failed, const char* label,
                   const char* q, const char* a) {
  if (c->n >= GEM_CHAT_MAX) {
    chatDropOldest(c);
  }
  GeminiExchange* e = &c->ex[c->n];
  e->t = t;
  e->flags = (uint8_t)((failed ? GEM_EX_FAILED : 0) | (topic ? GEM_EX_TOPIC : 0));
  gCopy(e->label, sizeof(e->label), failed ? "" : label);
  gCopy(e->q, sizeof(e->q), q);
  gCopy(e->a, sizeof(e->a), a);
  c->n++;
  c->breakPending = false;
  while (c->n > 1 && geminiChatSave(c, NULL, 0) > GEM_CHAT_FILE_MAX) {
    chatDropOldest(c);
  }
}

void geminiChatNewTopic(GeminiChat* c) {
  if (c->n > 0) {
    c->breakPending = true;
  }
}

void geminiChatFold(GeminiChat* c, int64_t t, bool topic, bool failed, const char* label,
                    const char* q, const char* a, bool breakAfter) {
  geminiChatAdd(c, t, topic, failed, label, q, a);
  if (breakAfter) {
    geminiChatNewTopic(c);
  }
}

int geminiChatContext(const GeminiChat* c, int idx[GEM_CONTEXT]) {
  int tmp[GEM_CONTEXT];
  int k = 0;
  for (int i = c->n - 1; i >= 0 && k < GEM_CONTEXT; i--) {
    const GeminiExchange* e = &c->ex[i];
    if (!(e->flags & GEM_EX_FAILED)) {
      tmp[k++] = i;
    }
    if (e->flags & GEM_EX_TOPIC) {
      break;                                     // the topic began here: nothing older is sent
    }
  }
  for (int j = 0; j < k; j++) {
    idx[j] = tmp[k - 1 - j];
  }
  return k;
}

/* One field per line: '\' and newlines escaped, CR and other control characters dropped. */
static void wFieldEsc(GemW* w, const char* s) {
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    if (*p == '\\') {
      wStr(w, "\\\\");
    } else if (*p == '\n') {
      wStr(w, "\\n");
    } else if (*p >= 0x20 && *p != 0x7F) {
      wChar(w, (char)*p);
    }
  }
}

#define GEM_CHAT_MAGIC  "wiphone-ai-chat 1"
#define GEM_QUOTA_MAGIC "wiphone-ai-quota 1"

size_t geminiChatSave(const GeminiChat* c, char* out, size_t cap) {
  GemW w = { out, cap, 0 };
  wStr(&w, GEM_CHAT_MAGIC "\n");
  wStr(&w, c->breakPending ? "pending 1\n" : "pending 0\n");
  char num[48];
  for (int i = 0; i < c->n; i++) {
    const GeminiExchange* e = &c->ex[i];
    snprintf(num, sizeof(num), "x %lld %u ", (long long)e->t, (unsigned)e->flags);
    wStr(&w, num);
    if (e->label[0]) {
      wFieldEsc(&w, e->label);
    } else {
      wChar(&w, '-');
    }
    wStr(&w, "\nq ");
    wFieldEsc(&w, e->q);
    wStr(&w, "\na ");
    wFieldEsc(&w, e->a);
    wChar(&w, '\n');
  }
  wStr(&w, "end\n");
  return wEnd(&w);
}

/* Next line of [*p, end) without its LF (and a CR before it). False at the end. */
static bool gLine(const char** p, const char* end, const char** ls, const char** le) {
  if (*p >= end) {
    return false;
  }
  *ls = *p;
  const char* q = *p;
  while (q < end && *q != '\n') q++;
  *le = q;
  *p = q < end ? q + 1 : q;
  if (*le > *ls && (*le)[-1] == '\r') (*le)--;
  return true;
}

static bool gUnescape(const char* s, const char* e, char* out, size_t cap) {
  size_t o = 0;
  for (const char* p = s; p < e; p++) {
    char c = *p;
    if (c == '\\') {
      if (p + 1 >= e) {
        return false;
      }
      p++;
      c = (*p == 'n') ? '\n' : *p;
    }
    if (o + 1 < cap) {
      out[o++] = c;
    }
  }
  out[o] = '\0';
  return true;
}

static bool gParseI64(const char* s, const char* e, int64_t* v) {
  if (s >= e) {
    return false;
  }
  bool neg = false;
  if (*s == '-') {
    neg = true;
    s++;
  }
  if (s >= e || e - s > 18) {
    return false;
  }
  int64_t n = 0;
  for (const char* p = s; p < e; p++) {
    if (!isdigit((unsigned char)*p)) {
      return false;
    }
    n = n * 10 + (*p - '0');
  }
  *v = neg ? -n : n;
  return true;
}

static bool chatLoadInner(const char* text, size_t len, GeminiChat* out) {
  const char* p = text;
  const char* end = text + len;
  const char* ls;
  const char* le;
  if (!gLine(&p, end, &ls, &le) || !gEqNoCase(ls, (size_t)(le - ls), GEM_CHAT_MAGIC)) {
    return false;
  }
  if (!gLine(&p, end, &ls, &le) || le - ls != 9 || strncmp(ls, "pending ", 8) ||
      (ls[8] != '0' && ls[8] != '1')) {
    return false;
  }
  out->breakPending = ls[8] == '1';
  for (;;) {
    if (!gLine(&p, end, &ls, &le)) {
      return false;                              // no "end": a write cut short
    }
    if (le - ls == 3 && !strncmp(ls, "end", 3)) {
      return true;
    }
    if (le - ls < 2 || strncmp(ls, "x ", 2) || out->n >= GEM_CHAT_MAX) {
      return false;
    }
    GeminiExchange* e = &out->ex[out->n];
    const char* a = ls + 2;
    const char* b = (const char*)memchr(a, ' ', (size_t)(le - a));
    if (!b || !gParseI64(a, b, &e->t)) {
      return false;
    }
    a = b + 1;
    b = a < le ? (const char*)memchr(a, ' ', (size_t)(le - a)) : NULL;
    int64_t fl = 0;
    if (!b || !gParseI64(a, b, &fl) || fl < 0 || fl > 3) {
      return false;
    }
    e->flags = (uint8_t)fl;
    a = b + 1;
    if (le - a == 1 && *a == '-') {
      e->label[0] = '\0';
    } else if (!gUnescape(a, le, e->label, sizeof(e->label))) {
      return false;
    }
    if (!gLine(&p, end, &ls, &le) || le - ls < 2 || strncmp(ls, "q ", 2) ||
        !gUnescape(ls + 2, le, e->q, sizeof(e->q))) {
      return false;
    }
    if (!gLine(&p, end, &ls, &le) || le - ls < 2 || strncmp(ls, "a ", 2) ||
        !gUnescape(ls + 2, le, e->a, sizeof(e->a))) {
      return false;
    }
    out->n++;
  }
}

bool geminiChatLoad(const char* text, size_t len, GeminiChat* out) {
  geminiChatClear(out);
  if (!text || !chatLoadInner(text, len, out)) {
    geminiChatClear(out);
    return false;
  }
  return true;
}

// ── the request ──────────────────────────────────────────────────────────────────────────

const char GEM_SYSTEM_INSTRUCTION[] =
  "You are an assistant on a small handheld phone with a 240x320 screen and a 12-key keypad. "
  "Reply in plain text only, using plain ASCII characters: no markdown, no asterisks, no bullet "
  "characters, no emoji. Be accurate. Answer fully but concisely: a few short paragraphs at "
  "most unless the user asks for more.";

/* After the date, before the device text, every request (2026-10-11): the model's own knowledge
 * stops at its training cut-off and it does not know that - it called a game due in 2027
 * "rumoured, may come out in 2025" in late 2026. The background it is told about is the phone's
 * Wikipedia lookup, sent in the question's own turn (geminiBuildRequest).
 * ⚠ NEVER a tool, a URL or a "function" in here: an instruction that named a "URL tool" the
 * request did not declare made 3.8 Flash answer MALFORMED_FUNCTION_CALL, no text, after 84 s.
 * Short: these are input tokens on every request of a free allowance. */
static const char GEM_SYS_FRESH[] =
  "Your own knowledge ends earlier than today, so things may have changed since. When background "
  "from Wikipedia comes with a question, prefer it for anything recent and ignore it if it is "
  "about something else. If you cannot confirm something recent, say so plainly; never present a "
  "guess as a fact. ";

#define GEM_SANE_UTC 1577836800LL                // 2020-01-01: a "trusted" clock before this is not

/* Days since 1970-01-01 to a civil date (Howard Hinnant's civil_from_days): exact for
 * every date this phone will see, no tables, no time.h (whose time_t is 32-bit on the phone). */
static void gCivil(int64_t days, int* y, int* m, int* d) {
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;                                   // [0, 146096]
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);            // [0, 365]
  const int64_t mp = (5 * doy + 2) / 153;                                 // [0, 11], March first
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*m <= 2 ? 1 : 0));
}

/* "Today is Sunday 11 October 2026, about 2:05 PM Pacific time (UTC-7). " */
static void wDateSentence(GemW* w, int64_t nowUtc, int tzOffsetS, const char* zone) {
  static const char* const DAYS[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                       "Friday", "Saturday" };
  static const char* const MONTHS[12] = { "January", "February", "March", "April", "May", "June",
                                          "July", "August", "September", "October", "November",
                                          "December" };
  const int64_t local = nowUtc + tzOffsetS;
  const int64_t day = (local >= 0 ? local : local - 86399) / 86400;
  const int sod = (int)(local - day * 86400);
  int y, m, d;
  gCivil(day, &y, &m, &d);
  const int wd = (int)(((day % 7) + 7 + 4) % 7);         // 1970-01-01 was a Thursday
  const int h = sod / 3600;
  char t[96];
  snprintf(t, sizeof(t), "Today is %s %d %s %d, about %d:%02d %s ", DAYS[wd], d, MONTHS[m - 1], y,
           (h % 12) ? h % 12 : 12, (sod % 3600) / 60, h < 12 ? "AM" : "PM");
  wStr(w, t);
  /* The zone's name, up to a comma ("Mountain, no DST" -> "Mountain"), letters and spaces only -
   * the plain-ASCII rule, and nothing from a setting can become an instruction. A name that IS an
   * offset - the zone table's "UTC+12" and "UTC-11", or anything with a digit in it - would read
   * "UTC time (UTC+12)" once its digits went: "local time" instead (review 2026-10-11). */
  bool offsetName = zone && !strncasecmp(zone, "UTC", 3);
  for (const char* p = zone; p && *p && !offsetName; p++) {
    offsetName = isdigit((unsigned char)*p) != 0;
  }
  char name[24];
  size_t n = 0;
  for (const char* p = zone; p && !offsetName && *p && *p != ',' && *p != '(' && n + 1 < sizeof(name);
       p++) {
    if (isalpha((unsigned char)*p) || *p == ' ') {
      name[n++] = *p;
    }
  }
  while (n > 0 && name[n - 1] == ' ') n--;
  name[n] = '\0';
  wStr(w, name[0] ? name : "local");
  const int off = tzOffsetS >= 0 ? (tzOffsetS + 30) / 60 : -((-tzOffsetS + 30) / 60);  // minutes
  const int a = off < 0 ? -off : off;
  if (off == 0) {
    snprintf(t, sizeof(t), " time (UTC). ");
  } else if (a % 60) {
    snprintf(t, sizeof(t), " time (UTC%c%d:%02d). ", off < 0 ? '-' : '+', a / 60, a % 60);
  } else {
    snprintf(t, sizeof(t), " time (UTC%c%d). ", off < 0 ? '-' : '+', a / 60);
  }
  wStr(w, t);
}

size_t geminiSystemText(int64_t nowUtc, int tzOffsetS, const char* zone, char* out, size_t cap) {
  GemW w = { out, cap, 0 };
  if (nowUtc >= GEM_SANE_UTC) {
    wDateSentence(&w, nowUtc, tzOffsetS, zone);
  }
  wStr(&w, GEM_SYS_FRESH);
  wStr(&w, GEM_SYSTEM_INSTRUCTION);
  return wEnd(&w);
}

size_t geminiJsonEscape(const char* in, char* out, size_t cap) {
  GemW w = { out, cap, 0 };
  wEsc(&w, in ? in : "");
  return wEnd(&w);
}

static void wTurnN(GemW* w, const char* role, const char* text, size_t n) {
  wStr(w, "{\"role\":\"");
  wStr(w, role);
  wStr(w, "\",\"parts\":[{\"text\":\"");
  wEscN(w, text ? text : "", n);
  wStr(w, "\"}]}");
}

static void wTurn(GemW* w, const char* role, const char* text) {
  wTurnN(w, role, text, text ? strlen(text) : 0);
}

/* A stored answer as it goes back as context: without its own "Wikipedia: ..." line
 * (geminiAnswerText's last line, GEM_SRC_SEP). Sent back, it would teach the model to end answers
 * with one - naming pages it was never given. Only the LAST line, and only after a blank one: a
 * "Wikipedia:" anywhere else is the answer's own words. */
static size_t gContextLen(const char* a) {
  if (!a) {
    return 0;
  }
  const char* last = NULL;
  for (const char* p = strstr(a, GEM_SRC_SEP); p; p = strstr(p + 1, GEM_SRC_SEP)) {
    last = p;
  }
  if (last && !strchr(last + sizeof(GEM_SRC_SEP) - 1, '\n')) {
    return (size_t)(last - a);
  }
  return strlen(a);
}

size_t geminiBuildRequest(const GeminiTurnPair* ctx, int nCtx, const char* question,
                          const char* background, const char* sysInstr, int thinkingBudget,
                          char* out, size_t cap) {
  const bool bg = background && background[0];
  // The newest exchanges that fit the cap with the question (and its background); the oldest go first.
  size_t total = (question ? strlen(question) : 0) +
                 (bg ? strlen(background) + sizeof(GEM_BG_QUESTION) - 1 : 0);
  int first = nCtx;
  for (int i = nCtx - 1; i >= 0; i--) {
    const size_t add = (ctx[i].q ? strlen(ctx[i].q) : 0) + gContextLen(ctx[i].a);
    if (total + add > GEM_SEND_TEXT_CAP) {
      break;
    }
    total += add;
    first = i;
  }
  GemW w = { out, cap, 0 };
  wStr(&w, "{\"system_instruction\":{\"parts\":[{\"text\":\"");
  wEsc(&w, sysInstr ? sysInstr : "");
  wStr(&w, "\"}]},\"contents\":[");
  for (int i = first; i < nCtx; i++) {
    wTurn(&w, "user", ctx[i].q);
    wChar(&w, ',');
    wTurnN(&w, "model", ctx[i].a, gContextLen(ctx[i].a));
    wChar(&w, ',');
  }
  if (bg) {
    /* The question's own turn - and only it - carries the lookup's background, labelled as such
     * (wikiBackground's first line) and the question after it: Wikipedia's text is data, never
     * the system instruction. The context turns above are the chat's: plain questions. */
    wStr(&w, "{\"role\":\"user\",\"parts\":[{\"text\":\"");
    wEsc(&w, background);
    wEsc(&w, GEM_BG_QUESTION);
    wEsc(&w, question ? question : "");
    wStr(&w, "\"}]}");
  } else {
    wTurn(&w, "user", question);
  }
  wChar(&w, ']');                                  // and never "tools" (gemini.h, CURRENT FACTS)
  char num[64];
  snprintf(num, sizeof(num), ",\"generationConfig\":{\"maxOutputTokens\":%d,\"temperature\":%s",
           GEM_MAX_TOKENS, GEM_TEMPERATURE);
  wStr(&w, num);
  if (thinkingBudget >= 0) {
    snprintf(num, sizeof(num), ",\"thinkingConfig\":{\"thinkingBudget\":%d}", thinkingBudget);
    wStr(&w, num);
  }
  wStr(&w, "}}");
  return wEnd(&w);
}

// ── a JSON reader: json_read.h (moved there for weather.cpp, 0.9.81; the same code) ─────────

/* "22517s", "1.5s" -> whole seconds, rounded up; -1 when it is not that shape. */
static int32_t gParseDelay(const char* s) {
  long whole = 0;
  bool frac = false, any = false;
  const char* p = s;
  while (isdigit((unsigned char)*p)) {
    if (whole < 100000000) whole = whole * 10 + (*p - '0');
    p++;
    any = true;
  }
  if (*p == '.') {
    p++;
    while (isdigit((unsigned char)*p)) {
      if (*p != '0') frac = true;
      p++;
      any = true;
    }
  }
  if (!any || *p != 's' || p[1]) {
    return -1;
  }
  return (int32_t)(whole + (frac ? 1 : 0));
}

// ── the answer ───────────────────────────────────────────────────────────────────────────

static int parsePart(Js* j, char* text, size_t textCap, size_t* tlen, GeminiReply* out) {
  if (!jsOpen(j, '{')) return jsSkip(j) ? 0 : -1;
  const size_t before = *tlen;
  const bool truncBefore = out->textTruncated;
  bool thought = false;
  char key[24];
  int r;
  while ((r = jsObjNext(j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "text")) {
      jsWs(j);
      if (j->p < j->e && *j->p == '"') {
        if (!jsString(j, text, textCap, tlen, &out->textTruncated)) return -1;
      } else if (!jsSkip(j)) {
        return -1;
      }
    } else if (!strcmp(key, "thought")) {
      thought = jsTrue(j);
    } else if (!jsSkip(j)) {
      return -1;
    }
  }
  if (r < 0) return -1;
  if (thought) {
    *tlen = before;                              // a thought summary, never the answer
    if (text && textCap) text[before] = '\0';
    out->textTruncated = truncBefore;            // ...and its length says nothing about the answer
  }
  return 0;
}

static int parseCandidate(Js* j, char* text, size_t textCap, size_t* tlen, GeminiReply* out) {
  if (!jsOpen(j, '{')) return jsSkip(j) ? 0 : -1;
  char key[24];
  int r;
  while ((r = jsObjNext(j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "content")) {
      if (!jsOpen(j, '{')) {
        if (!jsSkip(j)) return -1;
        continue;
      }
      char k2[16];
      int r2;
      while ((r2 = jsObjNext(j, k2, sizeof(k2))) == 1) {
        if (!strcmp(k2, "parts") && jsOpen(j, '[')) {
          int r3;
          while ((r3 = jsArrNext(j)) == 1) {
            if (parsePart(j, text, textCap, tlen, out) < 0) return -1;
          }
          if (r3 < 0) return -1;
        } else if (!jsSkip(j)) {
          return -1;
        }
      }
      if (r2 < 0) return -1;
    } else if (!strcmp(key, "finishReason")) {
      if (!jsStrInto(j, out->finish, sizeof(out->finish))) return -1;
    } else if (!jsSkip(j)) {
      return -1;
    }
  }
  return r < 0 ? -1 : 0;
}

static int parseViolation(Js* j, GeminiReply* out) {
  if (!jsOpen(j, '{')) return jsSkip(j) ? 0 : -1;
  char key[16];
  int r;
  while ((r = jsObjNext(j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "quotaId")) {
      char id[96];
      id[0] = '\0';
      if (!jsStrInto(j, id, sizeof(id))) return -1;
      if (strstr(id, "PerDay")) {
        out->quotaPerDay = true;
      }
    } else if (!jsSkip(j)) {
      return -1;
    }
  }
  return r < 0 ? -1 : 0;
}

static int parseDetail(Js* j, GeminiReply* out) {
  if (!jsOpen(j, '{')) return jsSkip(j) ? 0 : -1;
  char key[16];
  int r;
  while ((r = jsObjNext(j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "reason") && !out->errReason[0]) {
      if (!jsStrInto(j, out->errReason, sizeof(out->errReason))) return -1;
    } else if (!strcmp(key, "retryDelay")) {
      char d[24];
      d[0] = '\0';
      if (!jsStrInto(j, d, sizeof(d))) return -1;
      const int32_t s = gParseDelay(d);
      if (s >= 0) out->retryDelayS = s;
    } else if (!strcmp(key, "violations") && jsOpen(j, '[')) {
      int r2;
      while ((r2 = jsArrNext(j)) == 1) {
        if (parseViolation(j, out) < 0) return -1;
      }
      if (r2 < 0) return -1;
    } else if (!jsSkip(j)) {
      return -1;
    }
  }
  return r < 0 ? -1 : 0;
}

static int parseError(Js* j, GeminiReply* out) {
  if (!jsOpen(j, '{')) return jsSkip(j) ? 0 : -1;
  char key[16];
  int r;
  while ((r = jsObjNext(j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "code")) {
      jsWs(j);
      int v = 0;
      for (const char* q = j->p; q < j->e && *q >= '0' && *q <= '9' && v < 100000; q++) {
        v = v * 10 + (*q - '0');
      }
      out->errCode = v;
      if (!jsSkip(j)) return -1;
    } else if (!strcmp(key, "message")) {
      size_t o = 0;
      bool t = false;
      jsWs(j);
      if (j->p < j->e && *j->p == '"') {
        if (!jsString(j, out->errMessage, sizeof(out->errMessage), &o, &t)) return -1;
      } else if (!jsSkip(j)) {
        return -1;
      }
    } else if (!strcmp(key, "status")) {
      if (!jsStrInto(j, out->errStatus, sizeof(out->errStatus))) return -1;
    } else if (!strcmp(key, "details") && jsOpen(j, '[')) {
      int r2;
      while ((r2 = jsArrNext(j)) == 1) {
        if (parseDetail(j, out) < 0) return -1;
      }
      if (r2 < 0) return -1;
    } else if (!jsSkip(j)) {
      return -1;
    }
  }
  return r < 0 ? -1 : 0;
}

int geminiParseReply(const char* body, size_t len, char* text, size_t textCap, GeminiReply* out) {
  memset(out, 0, sizeof(*out));
  out->retryDelayS = -1;
  out->kind = GEM_REPLY_BAD;
  if (text && textCap) text[0] = '\0';
  if (!body) {
    return out->kind;
  }
  Js j = { body, body + len };
  size_t tlen = 0;
  bool cand = false, err = false;
  if (!jsOpen(&j, '{')) {
    return out->kind;
  }
  char key[24];
  int r;
  while ((r = jsObjNext(&j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "candidates") && jsOpen(&j, '[')) {
      int r2;
      bool firstDone = false;
      while ((r2 = jsArrNext(&j)) == 1) {
        if (!firstDone) {
          if (parseCandidate(&j, text, textCap, &tlen, out) < 0) return out->kind;
          firstDone = true;
          cand = true;
        } else if (!jsSkip(&j)) {
          return out->kind;
        }
      }
      if (r2 < 0) return out->kind;
    } else if (!strcmp(key, "promptFeedback") && jsOpen(&j, '{')) {
      char k2[16];
      int r2;
      while ((r2 = jsObjNext(&j, k2, sizeof(k2))) == 1) {
        if (!strcmp(k2, "blockReason")) {
          if (!jsStrInto(&j, out->block, sizeof(out->block))) return out->kind;
        } else if (!jsSkip(&j)) {
          return out->kind;
        }
      }
      if (r2 < 0) return out->kind;
    } else if (!strcmp(key, "error")) {
      if (parseError(&j, out) < 0) return out->kind;
      err = true;
    } else if (!jsSkip(&j)) {
      return out->kind;
    }
  }
  if (r < 0) {
    return out->kind;                            // cut short or not JSON: BAD, whatever was read
  }
  if (err) {
    out->kind = GEM_REPLY_ERROR;
  } else if (cand && tlen > 0) {
    out->kind = GEM_REPLY_TEXT;
  } else if (out->block[0]) {
    out->kind = GEM_REPLY_BLOCKED;
  } else if (cand) {
    out->kind = GEM_REPLY_EMPTY;
  }
  return out->kind;
}

// ── cleaning for the screen ───────────────────────────────────────────────────────────────

struct GemSub {
  uint16_t    cp;
  const char* ascii;
};
/* The phone's fonts are ASCII. These are the characters a model's prose actually uses; anything
 * else non-ASCII is dropped (an emoji would only ever draw as a .notdef bar). The e-reader's
 * bookRenderRun table covers the same punctuation (app_books.cpp); this one also folds accents
 * and symbols, because what is cleaned here is what is STORED, sent back as context and saved. */
static const GemSub GEM_SUBS[] = {
  { 0x2018, "'" },  { 0x2019, "'" },  { 0x201A, "," },  { 0x201B, "'" },  { 0x2032, "'" },
  { 0x201C, "\"" }, { 0x201D, "\"" }, { 0x201E, "\"" }, { 0x2033, "\"" },
  { 0x00AB, "\"" }, { 0x00BB, "\"" }, { 0x2039, "<" },  { 0x203A, ">" },
  { 0x2010, "-" },  { 0x2011, "-" },  { 0x2012, "-" },  { 0x2013, "-" },  { 0x2014, "-" },
  { 0x2015, "-" },  { 0x2212, "-" },  { 0x2026, "..." },
  { 0x00A0, " " },  { 0x1680, " " },  { 0x2000, " " },  { 0x2001, " " },  { 0x2002, " " },
  { 0x2003, " " },  { 0x2004, " " },  { 0x2005, " " },  { 0x2006, " " },  { 0x2007, " " },
  { 0x2008, " " },  { 0x2009, " " },  { 0x200A, " " },  { 0x202F, " " },  { 0x205F, " " },
  { 0x3000, " " },
  { 0x00AD, "" },   { 0x200B, "" },   { 0x200C, "" },   { 0x200D, "" },   { 0xFEFF, "" },
  { 0x2022, "-" },  { 0x2023, "-" },  { 0x2043, "-" },  { 0x25CF, "-" },  { 0x25AA, "-" },
  { 0x25E6, "-" },  { 0x00B7, "." },
  { 0x2122, "(TM)" }, { 0x00A9, "(c)" }, { 0x00AE, "(R)" },
  { 0x00B0, " deg" }, { 0x00B1, "+/-" }, { 0x2248, "~" }, { 0x2260, "!=" }, { 0x2264, "<=" },
  { 0x2265, ">=" },  { 0x2192, "->" }, { 0x2190, "<-" }, { 0x2194, "<->" }, { 0x21D2, "=>" },
  { 0x00BC, "1/4" }, { 0x00BD, "1/2" }, { 0x00BE, "3/4" }, { 0x00B9, "^1" }, { 0x00B2, "^2" },
  { 0x00B3, "^3" },  { 0x20AC, "EUR" }, { 0x00A3, "GBP" }, { 0x00A5, "JPY" }, { 0x00A2, "c" },
  { 0x00B5, "u" },   { 0x03BC, "u" },   { 0x00A7, "S" },
  { 0x0131, "i" },   { 0x0141, "L" },   { 0x0142, "l" },   { 0x0152, "OE" }, { 0x0153, "oe" },
  { 0x0160, "S" },   { 0x0161, "s" },   { 0x017D, "Z" },   { 0x017E, "z" },  { 0x0178, "Y" },
  { 0x010C, "C" },   { 0x010D, "c" },   { 0x0158, "R" },   { 0x0159, "r" },  { 0x011B, "e" },
  { 0x0107, "c" },   { 0x0119, "e" },   { 0x0105, "a" },   { 0x0144, "n" },  { 0x015B, "s" },
  { 0x017A, "z" },   { 0x017C, "z" },
};
/* U+00C0..U+00FF, one entry each (x and / for the two signs in the middle of the block). */
static const char* const GEM_LATIN1[64] = {
  "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
  "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "TH", "ss",
  "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
  "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y",
};

static const char* gFold(uint32_t cp) {
  if (cp >= 0xC0 && cp <= 0xFF) {
    return GEM_LATIN1[cp - 0xC0];
  }
  for (size_t k = 0; k < sizeof(GEM_SUBS) / sizeof(GEM_SUBS[0]); k++) {
    if (GEM_SUBS[k].cp == cp) {
      return GEM_SUBS[k].ascii;
    }
  }
  return "";                                     // anything else: dropped
}

const char* geminiFoldNext(const char** sp, char one[2]) {
  const unsigned char* s = (const unsigned char*)*sp;
  const unsigned char c = *s;
  if (!c) {
    return NULL;
  }
  uint32_t cp;
  int n;
  if (c < 0x80) { cp = c; n = 1; }
  else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; n = 2; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; n = 3; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; n = 4; }
  else {                                         // a stray continuation byte
    *sp = (const char*)(s + 1);
    return "";
  }
  int k = 1;
  for (; k < n; k++) {                           // never past the NUL: it is no continuation byte
    if ((s[k] & 0xC0) != 0x80) break;
    cp = (cp << 6) | (s[k] & 0x3Fu);
  }
  if (k < n) {                                   // a cut sequence: skip its lead byte
    *sp = (const char*)(s + 1);
    return "";
  }
  *sp = (const char*)(s + n);
  if (cp < 0x80) {
    one[0] = (char)cp;
    one[1] = '\0';
    return one;
  }
  return gFold(cp);
}

size_t geminiCleanText(const char* in, char* out, size_t cap, bool* truncated) {
  bool trunc = false;
  if (!out || cap == 0) {
    if (truncated) *truncated = true;
    return 0;
  }
  // Pass 1: to ASCII (geminiFoldNext), control characters out (only \n survives; \t is a space).
  size_t o = 0;
  const char* s = in ? in : "";
  char one[2];
  const char* rep;
  while ((rep = geminiFoldNext(&s, one)) != NULL) {
    if (rep == one) {
      if (one[0] == '\t') one[0] = ' ';
      const unsigned char cp = (unsigned char)one[0];
      if ((cp < 0x20 && cp != '\n') || cp == 0x7F) continue;
    }
    const size_t rl = strlen(rep);
    if (o + rl >= cap) {
      trunc = true;
      break;
    }
    memcpy(out + o, rep, rl);
    o += rl;
  }
  /* Pass 2, in place (it only ever shrinks, so the write never passes the read): markdown off
   * line by line, blank-line runs to one, trailing spaces and both ends trimmed. */
  size_t r = 0, w = 0;
  int blank = 0;
  bool any = false, fence = false;
  while (r < o) {
    const size_t ls = r;
    size_t le = ls;
    while (le < o && out[le] != '\n') le++;
    r = le < o ? le + 1 : le;
    size_t e = le;
    while (e > ls && out[e - 1] == ' ') e--;
    size_t b = ls;
    while (b < e && out[b] == ' ') b++;
    const size_t lead = b - ls;
    if (e - b >= 3 && !strncmp(out + b, "```", 3)) {
      fence = !fence;                            // a code fence line: dropped, its code kept
      continue;
    }
    bool bullet = false;
    if (!fence && b < e) {
      if (out[b] == '#') {
        size_t h = b;
        while (h < e && out[h] == '#') h++;
        if (h == e || out[h] == ' ') {
          b = h;
          while (b < e && out[b] == ' ') b++;
        }
      }
      if (b < e && out[b] == '>') {
        b++;
        while (b < e && out[b] == ' ') b++;
      }
      if (e - b >= 2 && (out[b] == '*' || out[b] == '-' || out[b] == '+') && out[b + 1] == ' ') {
        bullet = true;
        b += 2;
        while (b < e && out[b] == ' ') b++;
      }
    }
    if (b == e && !bullet) {
      if (any) blank++;
      continue;
    }
    if (any) {
      out[w++] = '\n';
      if (blank) out[w++] = '\n';
    }
    blank = 0;
    any = true;
    for (size_t k = 0; k < lead && k < 6; k++) {
      out[w++] = ' ';
    }
    if (bullet) {
      out[w++] = '-';
      out[w++] = ' ';
    }
    for (size_t k = b; k < e;) {
      if (!fence && k + 1 < e && ((out[k] == '*' && out[k + 1] == '*') ||
                                  (out[k] == '_' && out[k + 1] == '_'))) {
        k += 2;
        continue;
      }
      if (!fence && out[k] == '`') {
        k++;
        continue;
      }
      out[w++] = out[k++];
    }
  }
  while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\n')) w--;
  out[w] = '\0';
  if (truncated) *truncated = trunc;
  return w;
}

#define GEM_CUT_NOTE " (cut short)"

size_t geminiAnswerText(const GeminiReply* r, const char* text, const char* sources, char* out,
                        size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  const size_t note = sizeof(GEM_CUT_NOTE);      // with its NUL: room always kept for it
  if (cap <= note + 1) {
    return 0;
  }
  /* The sources line ("Wikipedia: A; B", wikiSources - already capped and ASCII) after a blank
   * line: its room is kept first, so the cap cuts the answer, never the line. */
  size_t slen = (sources && sources[0]) ? strlen(sources) + 2 : 0;   // + "\n\n"
  if (cap <= note + slen + 1) {
    slen = 0;                                    // no room for both: the answer, never the line
  }
  bool trunc = false;
  size_t n = geminiCleanText(text, out, cap - note - slen + 1, &trunc);
  if (n == 0) {
    out[0] = '\0';
    return 0;
  }
  if (trunc || (r && (r->textTruncated || !strcmp(r->finish, "MAX_TOKENS")))) {
    memcpy(out + n, GEM_CUT_NOTE, sizeof(GEM_CUT_NOTE));
    n += sizeof(GEM_CUT_NOTE) - 1;
  }
  if (slen) {
    memcpy(out + n, "\n\n", 2);
    memcpy(out + n + 2, sources, slen - 2 + 1);
    n += slen;
  }
  return n;
}

size_t geminiNoTextMessage(const GeminiReply* r, char* out, size_t cap) {
  GemW w = { out, cap, 0 };
  if (!r || r->kind == GEM_REPLY_BAD) {
    wStr(&w, "Could not read Gemini's answer");
  } else if (r->kind == GEM_REPLY_BLOCKED) {
    wStr(&w, "Gemini declined to answer (");
    wStr(&w, r->block);
    wChar(&w, ')');
  } else if (!strcmp(r->finish, "MAX_TOKENS")) {
    wStr(&w, "Gemini ran out of room before answering (MAX_TOKENS) - ask for less, or set "
             "thinking= lower in gemini.txt");
  } else if (!strcmp(r->finish, "MALFORMED_FUNCTION_CALL")) {
    /* Not a refusal: no tool is ever sent, the model stumbled (twice - the ladder's rule 0). */
    wStr(&w, "Gemini sent an empty answer - ask again");
  } else if (r->finish[0] && strcmp(r->finish, "STOP")) {
    wStr(&w, "Gemini declined to answer (");
    wStr(&w, r->finish);
    wChar(&w, ')');
  } else {
    wStr(&w, "Gemini sent an empty answer");
  }
  return wEnd(&w);
}

// ── sessions and the day's quotas ─────────────────────────────────────────────────────────

void geminiSessionInit(GeminiSession* s) {
  memset(s, 0, sizeof(*s));
  gCopy(s->noThink[0], sizeof(s->noThink[0]), GEM_LITE_MODEL);
  s->n = 1;
}

bool geminiSessionNoThink(const GeminiSession* s, const char* model) {
  for (int i = 0; i < s->n; i++) {
    if (!strcmp(s->noThink[i], model)) {
      return true;
    }
  }
  return false;
}

void geminiSessionAddNoThink(GeminiSession* s, const char* model) {
  if (geminiSessionNoThink(s, model)) {
    return;
  }
  const int cap = (int)(sizeof(s->noThink) / sizeof(s->noThink[0]));
  if (s->n >= cap) {
    memmove(s->noThink[1], s->noThink[2], sizeof(s->noThink[0]) * (size_t)(cap - 2));
    s->n = cap - 1;                              // the seeded lite model stays at [0]
  }
  gCopy(s->noThink[s->n], sizeof(s->noThink[0]), model);
  s->n++;
}

void geminiQuotaClear(GeminiQuota* q) {
  memset(q, 0, sizeof(*q));
}

static int quotaFind(const GeminiQuota* q, const char* model) {
  for (int i = 0; i < q->n; i++) {
    if (!strcmp(q->e[i].model, model)) {
      return i;
    }
  }
  return -1;
}

static bool quotaEntryOut(const GeminiQuotaEntry* e, int64_t nowUtc, uint32_t nowMs) {
  if (e->untilUtc && nowUtc) {
    return nowUtc < e->untilUtc;
  }
  if (e->durMs) {
    return (uint32_t)(nowMs - e->setMs) < e->durMs;
  }
  return false;
}

bool geminiQuotaWhen(const GeminiQuota* q, const char* model, int64_t nowUtc, uint32_t nowMs,
                     int64_t* untilUtc, uint32_t* remainS) {
  *untilUtc = 0;
  *remainS = 0;
  const int i = model ? quotaFind(q, model) : -1;
  if (i < 0 || !quotaEntryOut(&q->e[i], nowUtc, nowMs)) {
    return false;
  }
  const GeminiQuotaEntry* e = &q->e[i];
  if (e->guess) {
    return true;                                 // Google gave no time: none is invented
  }
  if (e->untilUtc && nowUtc) {
    *untilUtc = e->untilUtc;
    *remainS = (uint32_t)(e->untilUtc - nowUtc);
  } else if (e->durMs) {
    *remainS = (e->durMs - (uint32_t)(nowMs - e->setMs) + 999u) / 1000u;
  }
  return true;
}

bool geminiQuotaOut(const GeminiQuota* q, const char* model, int64_t nowUtc, uint32_t nowMs) {
  const int i = model ? quotaFind(q, model) : -1;
  return i >= 0 && quotaEntryOut(&q->e[i], nowUtc, nowMs);
}

int64_t geminiQuotaUntil(const GeminiQuota* q, const char* model, int64_t nowUtc, uint32_t nowMs) {
  const int i = model ? quotaFind(q, model) : -1;
  if (i < 0 || !quotaEntryOut(&q->e[i], nowUtc, nowMs)) {
    return 0;
  }
  return q->e[i].untilUtc;
}

static int quotaSlot(GeminiQuota* q, const char* model) {
  int i = quotaFind(q, model);
  if (i >= 0) {
    return i;
  }
  if (q->n >= GEM_QUOTA_SLOTS) {
    memmove(&q->e[0], &q->e[1], sizeof(q->e[0]) * (GEM_QUOTA_SLOTS - 1));
    q->n = GEM_QUOTA_SLOTS - 1;
  }
  i = q->n++;
  memset(&q->e[i], 0, sizeof(q->e[i]));
  gCopy(q->e[i].model, sizeof(q->e[i].model), model);
  return i;
}

void geminiQuotaMark(GeminiQuota* q, const char* model, uint32_t delayS, bool guess,
                     int64_t nowUtc, uint32_t nowMs) {
  if (!model || !model[0]) {
    return;
  }
  if (delayS > 4000000u) {
    delayS = 4000000u;                           // ~46 days: under the boot clock's wrap
  }
  GeminiQuotaEntry* e = &q->e[quotaSlot(q, model)];
  e->untilUtc = nowUtc ? nowUtc + (int64_t)delayS : 0;
  e->setMs = nowMs;
  e->durMs = delayS * 1000u;
  if (e->durMs == 0) e->durMs = 1;
  e->guess = guess;
}

void geminiQuotaMerge(GeminiQuota* into, const GeminiQuota* from) {
  for (int i = 0; i < from->n; i++) {
    GeminiQuotaEntry* e = &into->e[quotaSlot(into, from->e[i].model)];
    if (from->e[i].untilUtc >= e->untilUtc || from->e[i].durMs) {
      *e = from->e[i];
    }
  }
}

size_t geminiQuotaSave(const GeminiQuota* q, int64_t nowUtc, char* out, size_t cap) {
  GemW w = { out, cap, 0 };
  wStr(&w, GEM_QUOTA_MAGIC "\n");
  char line[GEM_MODEL_MAX + 40];
  for (int i = 0; i < q->n; i++) {
    const GeminiQuotaEntry* e = &q->e[i];
    if (!e->untilUtc || (nowUtc && e->untilUtc <= nowUtc)) {
      continue;
    }
    snprintf(line, sizeof(line), "%s %lld %s\n", e->guess ? "est" : "out", (long long)e->untilUtc,
             e->model);
    wStr(&w, line);
  }
  wStr(&w, "end\n");
  return wEnd(&w);
}

bool geminiQuotaLoad(const char* text, size_t len, GeminiQuota* out) {
  geminiQuotaClear(out);
  if (!text) {
    return false;
  }
  const char* p = text;
  const char* end = text + len;
  const char* ls;
  const char* le;
  if (!gLine(&p, end, &ls, &le) || !gEqNoCase(ls, (size_t)(le - ls), GEM_QUOTA_MAGIC)) {
    return false;
  }
  while (gLine(&p, end, &ls, &le)) {
    if (le - ls == 3 && !strncmp(ls, "end", 3)) {
      return true;
    }
    const bool guess = le - ls > 4 && !strncmp(ls, "est ", 4);   // remembered WITHOUT Google's time
    if (le - ls <= 4 || (!guess && strncmp(ls, "out ", 4))) {
      break;
    }
    const char* a = ls + 4;
    const char* b = (const char*)memchr(a, ' ', (size_t)(le - a));
    int64_t until = 0;
    char model[GEM_MODEL_MAX + 1];
    if (!b || !gParseI64(a, b, &until) || until <= 0 ||
        (size_t)(le - b - 1) >= GEM_MODEL_MAX) {
      break;
    }
    gCopyN(model, sizeof(model), b + 1, (size_t)(le - b - 1));
    if (!geminiModelNameOk(model) || out->n >= GEM_QUOTA_SLOTS) {
      break;
    }
    GeminiQuotaEntry* e = &out->e[out->n++];
    gCopy(e->model, sizeof(e->model), model);
    e->untilUtc = until;
    e->guess = guess;
  }
  geminiQuotaClear(out);
  return false;
}

/* When a limit lifts, in the words that follow "until" / "after" (a deadline on a trusted
 * clock: "about 17:00", "about 00:00 tomorrow") or "for" / "in" (only a duration: "about 4 h").
 * False when neither is known: the caller says "for now" / "later". */
static bool quotaWhenText(GemW* w, const char* atWord, const char* forWord, int64_t untilUtc,
                          int64_t nowUtc, int tzOffsetS, uint32_t remainS) {
  char t[40];
  const int64_t local = untilUtc + tzOffsetS + 30;           // to the nearest minute
  const int64_t day = (local >= 0 ? local : local - 86399) / 86400;
  const int64_t nowLocal = nowUtc + tzOffsetS;
  const int64_t today = (nowLocal >= 0 ? nowLocal : nowLocal - 86399) / 86400;
  // A deadline past tomorrow (Google's daily ones never are) is given as a duration instead.
  if (untilUtc > 0 && nowUtc > 0 && day - today <= 1) {
    const int sod = (int)(local - day * 86400);
    snprintf(t, sizeof(t), "%s about %02d:%02d%s", atWord, sod / 3600, (sod % 3600) / 60,
             day != today ? " tomorrow" : "");
    wStr(w, t);
    return true;
  }
  if (remainS == 0) {
    return false;
  }
  if (remainS < 90u * 60u) {
    const unsigned m = (unsigned)((remainS + 299u) / 300u) * 5u;   // up to the next 5 minutes
    snprintf(t, sizeof(t), "%s about %u min", forWord, m);
  } else if (remainS < 48u * 3600u) {
    snprintf(t, sizeof(t), "%s about %u h", forWord, (unsigned)((remainS + 1800u) / 3600u));
  } else {
    snprintf(t, sizeof(t), "%s about %u days", forWord, (unsigned)((remainS + 43200u) / 86400u));
  }
  wStr(w, t);
  return true;
}

size_t geminiQuotaNotice(const char* modelLabel, const char* fallbackLabel, int64_t untilUtc,
                         int64_t nowUtc, int tzOffsetS, uint32_t remainS, char* out, size_t cap) {
  GemW w = { out, cap, 0 };
  wStr(&w, "Daily free limit for ");
  wStr(&w, modelLabel ? modelLabel : "Gemini");
  wStr(&w, " reached - ");
  if (fallbackLabel && fallbackLabel[0]) {
    wStr(&w, "using ");
    wStr(&w, fallbackLabel);
    wChar(&w, ' ');
    if (!quotaWhenText(&w, "until", "for", untilUtc, nowUtc, tzOffsetS, remainS)) {
      wStr(&w, "for now");
    }
  } else {
    wStr(&w, "try again ");
    if (!quotaWhenText(&w, "after", "in", untilUtc, nowUtc, tzOffsetS, remainS)) {
      wStr(&w, "later");
    }
  }
  return wEnd(&w);
}

size_t geminiQuotaAllOut(int64_t untilUtc, int64_t nowUtc, int tzOffsetS, uint32_t remainS,
                         char* out, size_t cap) {
  GemW w = { out, cap, 0 };
  wStr(&w, "Today's free Gemini limit is used up - try again ");
  if (!quotaWhenText(&w, "after", "in", untilUtc, nowUtc, tzOffsetS, remainS)) {
    wStr(&w, "later");
  }
  return wEnd(&w);
}

void geminiQuotaBack(const GeminiQuota* q, const char* primary, const char* fallback,
                     int64_t nowUtc, uint32_t nowMs, int64_t* untilUtc, uint32_t* remainS) {
  int64_t u1, u2 = 0;
  uint32_t r1, r2 = 0;
  const bool o1 = geminiQuotaWhen(q, primary, nowUtc, nowMs, &u1, &r1);
  const bool o2 = fallback && fallback[0] && geminiQuotaWhen(q, fallback, nowUtc, nowMs, &u2, &r2);
  *untilUtc = 0;
  *remainS = 0;
  if ((o1 && r1 == 0) || (o2 && r2 == 0)) {
    return;                                      // one of them has no time from Google: "later"
  }
  const bool second = o2 && (!o1 || r2 < r1);
  if (o1 || o2) {
    *untilUtc = second ? u2 : u1;
    *remainS = second ? r2 : r1;
  }
}

// ── the ladder ───────────────────────────────────────────────────────────────────────────

static void ladderUse(GeminiLadder* l, const GeminiSession* s, const char* model, bool fallback) {
  gCopy(l->model, sizeof(l->model), model);
  l->onFallback = fallback;
  l->busyTries = 0;
  l->minuteRetried = false;
  l->budget = (fallback || geminiSessionNoThink(s, model)) ? -1 : l->thinking;
}

int geminiLadderStart(GeminiLadder* l, const GeminiConfig* cfg, const GeminiSession* s,
                      const GeminiQuota* q, int64_t nowUtc, uint32_t nowMs) {
  memset(l, 0, sizeof(*l));
  gCopy(l->primary, sizeof(l->primary), cfg->model[0] ? cfg->model : GEM_DEFAULT_MODEL);
  gCopy(l->fallback, sizeof(l->fallback), cfg->fallback);
  if (!strcmp(l->fallback, l->primary)) {
    l->fallback[0] = '\0';                       // falling back to itself is no fallback
  }
  l->thinking = cfg->thinking;
  l->nowUtc = nowUtc;
  l->retryTimeouts = true;                       // the setting's default: the caller sets it after
  if (!geminiQuotaOut(q, l->primary, nowUtc, nowMs)) {
    ladderUse(l, s, l->primary, false);
    return GEM_STEP_RETRY;
  }
  l->quotaSwitch = true;
  geminiQuotaWhen(q, l->primary, nowUtc, nowMs, &l->primaryUntil, &l->primaryRemainS);
  if (l->fallback[0] && !geminiQuotaOut(q, l->fallback, nowUtc, nowMs)) {
    ladderUse(l, s, l->fallback, true);
    return GEM_STEP_RETRY;
  }
  l->allOut = true;
  geminiQuotaBack(q, l->primary, l->fallback, nowUtc, nowMs, &l->backUntil, &l->backRemainS);
  return GEM_STEP_FAIL;
}

/* To the fallback if there is one that is not used up; else FAIL (allOut when every model is). */
static int ladderFallback(GeminiLadder* l, GeminiSession* s, GeminiQuota* q, int64_t nowUtc,
                          uint32_t nowMs, uint32_t delay, const char* stg, uint32_t* delayMs,
                          const char** stage) {
  if (!l->onFallback && l->fallback[0] && !geminiQuotaOut(q, l->fallback, nowUtc, nowMs)) {
    ladderUse(l, s, l->fallback, true);
    *delayMs = delay;
    *stage = stg;
    return GEM_STEP_RETRY;
  }
  l->allOut = geminiQuotaOut(q, l->primary, nowUtc, nowMs) &&
              (!l->fallback[0] || geminiQuotaOut(q, l->fallback, nowUtc, nowMs));
  if (l->allOut) {
    geminiQuotaBack(q, l->primary, l->fallback, nowUtc, nowMs, &l->backUntil, &l->backRemainS);
  }
  return GEM_STEP_FAIL;
}

/* Rule 6 (round 3, "Retry if it times out"): an attempt that got NO answer because nothing came in
 * time or the connection went. The model was not refused - it was slow (free-tier 3.8 Flash took
 * 30-90 s, past 150 s now and then): once more on it, then once on the fallback. Never a daily
 * limit (nothing is marked), and never a loop: two flags a question and the attempt cap. */
static int ladderNoAnswer(GeminiLadder* l, GeminiSession* s, GeminiQuota* q, int net, int64_t nowUtc,
                          uint32_t nowMs, uint32_t* delayMs, const char** stage) {
  if (!l->retryTimeouts || (net != GEM_NET_TIMEOUT && net != GEM_NET_LOST) || l->attempts >= GEM_MAX_ATTEMPTS) {
    return GEM_STEP_FAIL;                        // DNS, a refused connect, TLS, memory...: as before
  }
  if (!l->timeoutRetried) {
    l->timeoutRetried = true;
    *stage = GEM_TIMEOUT_AGAIN;                  // the same model, its thinking kept, at once
    return GEM_STEP_RETRY;
  }
  if (l->onFallback || l->timeoutSwitch) {
    return GEM_STEP_FAIL;                        // already the fallback: "did not answer in time"
  }
  char lab[GEM_LABEL_MAX];
  geminiModelLabel(l->fallback, lab, sizeof(lab));
  snprintf(l->note, sizeof(l->note), "Asking %s instead...", lab);
  /* At once (no GEM_LITE_DELAY_MS): after a delay aiRun says "Asking again...", and this line is
   * what should stay up through the fallback's request. */
  const int step = ladderFallback(l, s, q, nowUtc, nowMs, 0, l->note, delayMs, stage);
  if (step == GEM_STEP_RETRY) {
    l->timeoutSwitch = true;
  } else {
    l->allOut = false;                           // the PRIMARY is not used up: it timed out
  }
  return step;
}

int geminiLadderNext(GeminiLadder* l, GeminiSession* s, GeminiQuota* q, int httpCode, int net,
                     const GeminiReply* r, int64_t nowUtc, uint32_t nowMs,
                     uint32_t* delayMs, const char** stage) {
  *delayMs = 0;
  *stage = NULL;
  l->attempts++;
  if (httpCode == 200) {
    /* 0. MALFORMED_FUNCTION_CALL and no text (2026-10-11: 3.8 Flash, 84 s, a system instruction
     * that named a tool it was not given - none is named now): the model stumbled, it did not
     * refuse. Once more, the same model, at once; a second time it is an empty answer. */
    if (r && r->kind == GEM_REPLY_EMPTY && !strcmp(r->finish, "MALFORMED_FUNCTION_CALL") &&
        !l->askedAgainEmpty && l->attempts < GEM_MAX_ATTEMPTS) {
      l->askedAgainEmpty = true;
      *stage = "Empty answer - asking again...";
      return GEM_STEP_RETRY;
    }
    return GEM_STEP_DONE;
  }
  if (httpCode <= 0) {
    return ladderNoAnswer(l, s, q, net, nowUtc, nowMs, delayMs, stage);   // 6.
  }
  if (l->attempts >= GEM_MAX_ATTEMPTS) {
    return GEM_STEP_FAIL;
  }
  // 1. A model that will not take thinkingConfig: once without it, and remembered.
  if (httpCode == 400 && l->budget >= 0 && !l->droppedThinking && r &&
      !strcmp(r->errStatus, "INVALID_ARGUMENT") && !geminiLooksLikeKeyError(r)) {
    l->droppedThinking = true;
    geminiSessionAddNoThink(s, l->model);
    l->budget = -1;
    *stage = "Retrying without thinking...";
    return GEM_STEP_RETRY;
  }
  // 2. A retired or misspelt model: once with the default.
  if (httpCode == 404 && !l->onFallback && !l->tried404 && strcmp(l->model, GEM_DEFAULT_MODEL)) {
    l->tried404 = true;
    ladderUse(l, s, GEM_DEFAULT_MODEL, false);
    *stage = "Model not found - trying gemini-flash-latest...";
    return GEM_STEP_RETRY;
  }
  if (httpCode == 429 && r) {
    // 3. A DAILY limit: retrying cannot help. Remember it, and the fallback now.
    if (r->quotaPerDay || r->retryDelayS > GEM_MINUTE_MAX_S) {
      /* No RetryInfo: still remembered (GEM_DAY_GUESS_S), but as a GUESS - the screen says "for
       * now" / "later", never a time Google did not give. */
      const bool guess = r->retryDelayS <= 0;
      const uint32_t d = guess ? (uint32_t)GEM_DAY_GUESS_S : (uint32_t)r->retryDelayS;
      geminiQuotaMark(q, l->model, d, guess, nowUtc, nowMs);
      if (!l->onFallback) {
        l->quotaSwitch = true;
        l->primaryUntil = (nowUtc && !guess) ? nowUtc + (int64_t)d : 0;
        l->primaryRemainS = guess ? 0 : d;
      }
      return ladderFallback(l, s, q, nowUtc, nowMs, 0, "Daily free limit reached - trying the "
                            "faster model...", delayMs, stage);
    }
    // 4. A per-MINUTE limit: wait what Google says, once; then the fallback.
    if (r->retryDelayS >= 0) {
      if (!l->minuteRetried) {
        l->minuteRetried = true;
        *delayMs = (uint32_t)(r->retryDelayS > 0 ? r->retryDelayS : 1) * 1000u;
        *stage = "Per-minute limit - waiting...";
        return GEM_STEP_RETRY;
      }
      return ladderFallback(l, s, q, nowUtc, nowMs, GEM_LITE_DELAY_MS,
                            "Still limited - trying the faster model...", delayMs, stage);
    }
  }
  /* 5. Busy: two retries 2.5 s apart, then the fallback with its own two. EVERY 5xx is busy
   * (2026-10-11, shared with COVEY and the X4): 503 UNAVAILABLE is the one measured, but 500
   * INTERNAL, a proxy's 502 and 504 DEADLINE_EXCEEDED are Google's side too - a retry is what helps,
   * never "Gemini error 500" on the first one. */
  if ((httpCode >= 500 && httpCode <= 599) || httpCode == 429) {
    if (l->busyTries < 2) {
      l->busyTries++;
      *delayMs = GEM_BUSY_RETRY_MS;
      *stage = "Google is busy - trying again...";
      return GEM_STEP_RETRY;
    }
    return ladderFallback(l, s, q, nowUtc, nowMs, GEM_LITE_DELAY_MS,
                          "Busy - trying the faster model...", delayMs, stage);
  }
  return GEM_STEP_FAIL;
}

// ── failures ─────────────────────────────────────────────────────────────────────────────

void geminiScrub(char* s, size_t cap, const char* key) {
  (void)cap;                                     // "[key]" is shorter than any key it replaces
  if (!s || !key) {
    return;
  }
  const size_t kl = strlen(key);
  if (kl < 8) {
    return;
  }
  char* p;
  while ((p = strstr(s, key)) != NULL) {
    memcpy(p, "[key]", 5);
    memmove(p + 5, p + kl, strlen(p + kl) + 1);
  }
}

bool geminiLooksLikeKeyError(const GeminiReply* r) {
  if (!r || r->kind != GEM_REPLY_ERROR) {
    return false;
  }
  return !strcmp(r->errStatus, "UNAUTHENTICATED") || !strcmp(r->errStatus, "PERMISSION_DENIED") ||
         strstr(r->errReason, "API_KEY") != NULL || gHasNoCase(r->errMessage, "API key");
}

/* A message cut at errMessage's 200 bytes can end in the FIRST part of the key: a tail of 8+
 * characters that starts the key goes too. */
static void scrubKeyTail(char* s, const char* key) {
  if (!key) {
    return;
  }
  const size_t sl = strlen(s), kl = strlen(key);
  for (size_t n = (sl < kl ? sl : kl); n >= 8; n--) {
    if (!memcmp(s + sl - n, key, n)) {
      memcpy(s + sl - n, "[key]", 6);
      return;
    }
  }
}

/* Google's message, made fit for the screen: the key scrubbed from the RAW text first (the
 * cleanup drops "__" and backticks, so a key holding them would no longer match after it),
 * then ASCII, cut at a word near 120. */
static void shortMessage(const GeminiReply* r, const char* key, char* out, size_t cap) {
  out[0] = '\0';
  if (!r || !r->errMessage[0]) {
    return;
  }
  char raw[sizeof(r->errMessage)];
  gCopy(raw, sizeof(raw), r->errMessage);
  geminiScrub(raw, sizeof(raw), key);
  scrubKeyTail(raw, key);
  bool t = false;
  geminiCleanText(raw, out, cap, &t);
  memset(raw, 0, sizeof(raw));
  for (char* p = out; *p; p++) {
    if (*p == '\n') *p = ' ';
  }
  geminiScrub(out, cap, key);
  const size_t lim = 120;
  if (strlen(out) > lim) {
    size_t cut = lim;
    while (cut > 60 && out[cut] != ' ') cut--;
    while (cut > 0 && (out[cut - 1] == ' ' || out[cut - 1] == '.' || out[cut - 1] == ',')) cut--;
    memcpy(out + cut, "...", 4);
  }
}

size_t geminiFailText(int httpCode, int net, const GeminiReply* r, const GeminiLadder* l,
                      const char* key, int tzOffsetS, char* out, size_t cap) {
  GemW w = { out, cap, 0 };
  if (l && l->allOut) {
    return geminiQuotaAllOut(l->backUntil, l->nowUtc, tzOffsetS, l->backRemainS, out, cap);
  }
  if (httpCode <= 0) {
    switch (net) {
    case GEM_NET_DNS:       wStr(&w, "No internet - Google's name did not resolve"); break;
    case GEM_NET_CONNECT:   wStr(&w, "Could not connect to Google - no internet?"); break;
    case GEM_NET_TLS:       wStr(&w, "Secure connection failed - Google's certificate did not verify"); break;
    case GEM_NET_TLS_OTHER: wStr(&w, "Secure connection failed"); break;
    case GEM_NET_NOMEM:     wStr(&w, "The phone is low on memory - try again, or reboot"); break;
    case GEM_NET_TIMEOUT:   wStr(&w, "Gemini did not answer in time"); break;
    case GEM_NET_LOST:      wStr(&w, "The connection dropped before the answer came"); break;
    case GEM_NET_TOO_BIG:   wStr(&w, "The answer was too big to read"); break;
    case GEM_NET_BAD_BODY:  wStr(&w, "Could not read Gemini's answer"); break;
    default:                wStr(&w, "No network"); break;
    }
    return wEnd(&w);
  }
  char msg[200];
  shortMessage(r, key, msg, sizeof(msg));
  char head[40];
  if (httpCode == 401 || httpCode == 403 || (httpCode == 400 && geminiLooksLikeKeyError(r))) {
    wStr(&w, "Key rejected - check API Keys/gemini.txt");
    if (msg[0]) {
      wStr(&w, " (");
      wStr(&w, msg);
      wChar(&w, ')');
    }
  } else if (httpCode == 429) {
    wStr(&w, "Gemini is busy or the free quota is used up - try again later");
  } else if (httpCode == 503) {
    wStr(&w, "Google is busy right now - try again in a minute");
  } else {
    snprintf(head, sizeof(head), "Gemini error %d", httpCode);
    wStr(&w, head);
    if (msg[0]) {
      wStr(&w, ": ");
      wStr(&w, msg);
    } else if (r && r->errStatus[0]) {
      wStr(&w, ": ");
      wStr(&w, r->errStatus);
    }
  }
  const size_t n = wEnd(&w);
  if (out && n < cap) {
    geminiScrub(out, cap, key);                  // belt and braces: nothing above can hold it
    return strlen(out);
  }
  return n;
}

// ── the chunked body ─────────────────────────────────────────────────────────────────────

enum {
  DC_SIZE = 0, DC_EXT, DC_SIZE_LF, DC_DATA, DC_DATA_CR, DC_DATA_LF,
  DC_TR_START, DC_TR_LINE, DC_TR_LF,
};

void geminiDechunkInit(GeminiDechunk* d) {
  memset(d, 0, sizeof(*d));
  d->state = DC_SIZE;
}

static bool dcSizeDone(GeminiDechunk* d) {
  if (d->digits == 0) {
    d->error = true;
    return false;
  }
  d->digits = 0;
  d->state = d->remain ? DC_DATA : DC_TR_START;
  return true;
}

bool geminiDechunkFeed(GeminiDechunk* d, const uint8_t* in, size_t n, char* out, size_t cap,
                       size_t* outLen) {
  size_t i = 0;
  while (i < n && !d->done && !d->error) {
    const uint8_t c = in[i];
    switch (d->state) {
    case DC_SIZE: {
      int v = -1;
      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
      if (v >= 0) {
        if (d->digits >= 7) {                    // over 256 MB: not an answer
          d->error = true;
          break;
        }
        d->remain = (d->remain << 4) | (uint32_t)v;
        d->digits++;
      } else if (c == ';' || c == ' ' || c == '\t') {
        d->state = DC_EXT;
      } else if (c == '\r') {
        d->state = DC_SIZE_LF;
      } else if (c == '\n') {
        dcSizeDone(d);
      } else {
        d->error = true;
      }
      i++;
      break;
    }
    case DC_EXT:
      if (c == '\n') dcSizeDone(d);
      else if (c == '\r') d->state = DC_SIZE_LF;
      i++;
      break;
    case DC_SIZE_LF:
      if (c == '\n') dcSizeDone(d);
      else d->error = true;
      i++;
      break;
    case DC_DATA: {
      size_t take = n - i;
      if (take > d->remain) take = d->remain;
      size_t room = (out && *outLen < cap) ? cap - *outLen : 0;
      size_t put = take < room ? take : room;
      if (put) {
        memcpy(out + *outLen, in + i, put);
        *outLen += put;
      }
      if (put < take) d->overflow = true;
      d->remain -= (uint32_t)take;
      i += take;
      if (d->remain == 0) d->state = DC_DATA_CR;
      break;
    }
    case DC_DATA_CR:
      if (c == '\r') d->state = DC_DATA_LF;
      else if (c == '\n') d->state = DC_SIZE;
      else d->error = true;
      i++;
      break;
    case DC_DATA_LF:
      if (c == '\n') d->state = DC_SIZE;
      else d->error = true;
      i++;
      break;
    case DC_TR_START:
      if (c == '\r') d->state = DC_TR_LF;
      else if (c == '\n') d->done = true;
      else d->state = DC_TR_LINE;
      i++;
      break;
    case DC_TR_LINE:
      if (c == '\n') d->state = DC_TR_START;
      i++;
      break;
    case DC_TR_LF:
      if (c == '\n') d->done = true;
      else d->error = true;
      i++;
      break;
    default:
      d->error = true;
      break;
    }
  }
  return !d->error;
}
