/*
 * wiki_lookup.cpp - see wiki_lookup.h. Pure C: no Arduino, no IDF, no allocation, no recursion -
 * every buffer is the caller's (PSRAM on the phone) and the JSON is read by json_read's cursor, so
 * it is as safe on the HTTPS worker's 8 KB stack as in the host suite.
 */
#include "wiki_lookup.h"
#include "json_read.h"
#include "gemini.h"        // GEM_NET_* (one vocabulary for the worker), the fold, GEM_SRC_PREFIX

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// ── the keywords ─────────────────────────────────────────────────────────────────────────

/* The owner's list (2026-10-11), compared with apostrophes taken out. Question words, the verbs
 * around them, and the "is it out yet" words that every question about a release carries and no
 * article title does. "whats" / "hows" are the spellings typed WITHOUT an apostrophe; "what's",
 * "who's", "it's" reach the list as their stems (kwContraction, round 3) - so no "whos"-style
 * form is added here: the contraction rule covers them. */
static const char* const WIKI_STOP[] = {
  "a", "an", "the", "is", "are", "was", "were", "be", "been", "being", "has", "have", "had", "do",
  "does", "did", "will", "would", "can", "could", "should", "shall", "may", "might", "must", "of",
  "in", "on", "at", "to", "for", "from", "by", "with", "about", "into", "over", "under", "and",
  "or", "but", "if", "then", "than", "so", "as", "it", "its", "this", "that", "these", "those",
  "there", "here", "what", "which", "who", "whom", "whose", "when", "where", "why", "how", "new",
  "latest", "come", "came", "coming", "out", "yet", "still", "ever", "any", "some", "me", "my",
  "i", "you", "your", "we", "our", "they", "their", "he", "she", "him", "her", "tell", "know",
  "please", "whats", "hows",
};

static bool kwStop(const char* w) {
  char bare[WIKI_WORD_MAX + 1];
  size_t n = 0;
  for (const char* p = w; *p && n + 1 < sizeof(bare); p++) {
    if (*p != '\'') bare[n++] = *p;
  }
  bare[n] = '\0';
  for (size_t i = 0; i < sizeof(WIKI_STOP) / sizeof(WIKI_STOP[0]); i++) {
    if (!strcmp(bare, WIKI_STOP[i])) {
      return true;
    }
  }
  return false;
}

static bool kwHas(const char* list, const char* w) {
  const size_t wl = strlen(w);
  for (const char* p = list; *p;) {
    const char* e = strchr(p, ' ');
    const size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n == wl && !memcmp(p, w, wl)) {
      return true;
    }
    p += n;
    if (*p == ' ') p++;
  }
  return false;
}

/* `w` onto the space-separated `list` (n words in it) - unless it is there already, the list
 * holds `limit`, or it would not fit. */
static void kwAdd(char* list, size_t cap, int* n, int limit, const char* w) {
  if (*n >= limit || kwHas(list, w)) {
    return;
  }
  const size_t at = strlen(list);
  const size_t wl = strlen(w);
  if (at + (at ? 1 : 0) + wl + 1 > cap) {
    return;
  }
  if (at) list[at] = ' ';
  memcpy(list + at + (at ? 1 : 0), w, wl + 1);
  (*n)++;
}

/* THE SHARED FOLD (2026-10-11 - COVEY and the X4 port the same table): a text read one ASCII
 * character at a time through gemini.cpp's table (geminiFoldNext: "Navier–Stokes" reads
 * "Navier-Stokes", "Gauß" "Gauss", "Łódź" "Lodz", "e=mc²" "e=mc^2"), lowercased.
 * What the table drops ("" - a zero-width space, an emoji, a combining accent) is not there at
 * all; every other character that is not [a-z0-9'] its readers make a SEPARATOR, never delete:
 * "navier stokes" and "1959 2015", where deleting would join them ("navierstokes"). Before this
 * the phone's keywords read raw bytes (a UTF-8 byte split a word: "gau", "d"), and COVEY deleted
 * what NFKD could not fold. */
struct KwFold {
  const char* s;        // the text, past what has been folded
  const char* rep;      // the rest of the current character's ASCII ("" = fold the next)
  char        one[2];   // rep's home for an ASCII character - so a KwFold is never copied
};

static void kfInit(KwFold* f, const char* s) {
  f->s = s ? s : "";
  f->rep = "";
}

/* The next folded character, lowercased; '\0' at the end (and every call after it). */
static char kfNext(KwFold* f) {
  while (!*f->rep) {
    f->rep = geminiFoldNext(&f->s, f->one);
    if (!f->rep) {
      f->rep = "";
      return '\0';
    }
  }
  return (char)tolower((unsigned char)*f->rep++);
}

static bool kwWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '\'';
}

/* w (n bytes, lowercased, no apostrophe at either end) without ONE contraction ending: 's 'm 're
 * 'll 've 'd (round 3, the shared rule). "what's" -> "what" (a stopword), "baldur's" -> "baldur"
 * (kept: Wikipedia's "Baldur's Gate 3" holds the word "baldur"). Only the last one, and only with
 * something before it: the word never starts with an apostrophe here. "don't" keeps its "'t" (not
 * in the list; "dont" is no stopword). Returns the new length. */
static size_t kwContraction(const char* w, size_t n) {
  static const char* const ENDS[] = { "'s", "'m", "'d", "'re", "'ll", "'ve" };
  for (size_t i = 0; i < sizeof(ENDS) / sizeof(ENDS[0]); i++) {
    const size_t k = strlen(ENDS[i]);
    if (n > k && !memcmp(w + n - k, ENDS[i], k)) {
      return n - k;
    }
  }
  return n;
}

#define KW_RUN_MAX  (WIKI_WORD_MAX + 3)          // + the longest ending ("'ll"): a stem can still fit

/* The next keyword of the folded text (the cursor moves past it), THE SHARED ORDER: a run of
 * [a-z0-9'], its end apostrophes off, then a contraction ending off (kwContraction), THEN at most
 * WIKI_WORD_MAX, THEN not a stopword. False at the end. */
static bool kwNext(KwFold* f, char* w) {
  char run[KW_RUN_MAX + 1];
  char c = kfNext(f);
  for (;;) {
    while (c && !kwWordChar(c)) c = kfNext(f);   // separators: anything else the fold gave
    if (!c) {
      return false;
    }
    /* The run, as it streams: an apostrophe before the first letter never stored, the ones after
     * it held back until a letter or digit follows (so the trailing ones never are), and a run
     * whose stored part passes KW_RUN_MAX only read to its end - its stem is over WIKI_WORD_MAX
     * whatever ending comes off. */
    size_t n = 0, held = 0;
    bool over = false;
    for (; kwWordChar(c); c = kfNext(f)) {
      if (c == '\'') {
        if (n) held++;
        continue;
      }
      if (over || n + held + 1 > KW_RUN_MAX) {
        over = true;
        continue;
      }
      while (held) {
        run[n++] = '\'';
        held--;
      }
      run[n++] = c;
    }
    if (over || n == 0) {
      continue;                                  // too long, or only apostrophes
    }
    n = kwContraction(run, n);                   // BEFORE the length and stopword checks
    if (n > WIKI_WORD_MAX) {
      continue;                                  // no search term
    }
    memcpy(w, run, n);
    w[n] = '\0';
    if (kwStop(w)) {
      continue;
    }
    return true;
  }
}

/* The next word of a space-separated keyword list at *c into w (WIKI_WORD_MAX + 1): false at its end. */
static bool kwListNext(const char** c, char* w) {
  const char* s = *c;
  if (!*s) {
    return false;
  }
  const char* e = strchr(s, ' ');
  size_t l = e ? (size_t)(e - s) : strlen(s);
  if (l > WIKI_WORD_MAX) {
    l = WIKI_WORD_MAX;                             // never: every keyword is at most that
  }
  memcpy(w, s, l);
  w[l] = '\0';
  s = e ? e + 1 : s + strlen(s);
  *c = s;
  return true;
}

/* A text's OWN keywords, space-separated into `list` (WIKI_KW_CAP): returns how many. */
static int kwOwn(const char* text, char* list, size_t cap) {
  list[0] = '\0';
  int n = 0;
  char w[WIKI_WORD_MAX + 1];
  KwFold f;
  kfInit(&f, text);
  while (n < WIKI_KW_MAX && kwNext(&f, w)) {
    kwAdd(list, cap, &n, WIKI_KW_MAX, w);
  }
  return n;
}

int wikiKeywords(const char* question, const char* const* earlier, int nEarlier, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  char cur[WIKI_KW_CAP];
  const int nCur = kwOwn(question, cur, sizeof(cur));
  char w[WIKI_WORD_MAX + 1];
  int n = 0;
  if (nCur < 2 && earlier) {
    /* A follow-up ("and when?", "how long is it"): the topic's newest question that NAMED something
     * - 2 or more keywords of its own - says what it is about (round 3: not just the previous one,
     * which may itself be a follow-up that only borrowed). Its keywords first - as many as leave
     * room for this question's own. */
    char prev[WIKI_KW_CAP];
    for (int i = nEarlier - 1; i >= 0; i--) {
      if (kwOwn(earlier[i], prev, sizeof(prev)) < 2) {
        continue;
      }
      for (const char* c = prev; n < WIKI_KW_MAX - nCur && kwListNext(&c, w);) {
        if (!kwHas(cur, w)) {
          kwAdd(out, cap, &n, WIKI_KW_MAX - nCur, w);
        }
      }
      break;
    }
  }
  for (const char* c = cur; kwListNext(&c, w);) {
    kwAdd(out, cap, &n, WIKI_KW_MAX, w);
  }
  return n;
}

// ── the URL ──────────────────────────────────────────────────────────────────────────────

/* The same length rule as gemini.cpp's writer: bytes go in while they fit, the count goes on. */
struct WikiW {
  char*  out;
  size_t cap;
  size_t n;
};

static void wwPut(WikiW* w, const char* s, size_t len) {
  if (w->out && w->n + len < w->cap) {
    memcpy(w->out + w->n, s, len);
  }
  w->n += len;
}

static void wwStr(WikiW* w, const char* s) {
  wwPut(w, s, strlen(s));
}

static size_t wwEnd(WikiW* w) {
  if (w->out && w->cap) {
    w->out[w->n < w->cap ? w->n : 0] = '\0';
  }
  return w->n;
}

size_t wikiUrl(const char* keywords, char* out, size_t cap) {
  WikiW w = { out, cap, 0 };
  if (!keywords || !keywords[0]) {
    if (out && cap) out[0] = '\0';
    return 0;
  }
  wwStr(&w, "https://" WIKI_HOST "/w/api.php?action=query&format=json&formatversion=2&redirects=1"
            "&generator=search&gsrsearch=");
  for (const unsigned char* p = (const unsigned char*)keywords; *p; p++) {
    if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_' || *p == '~') {
      const char c = (char)*p;
      wwPut(&w, &c, 1);
    } else {
      char e[4];
      snprintf(e, sizeof(e), "%%%02X", (unsigned)*p);   // a space is %20, an apostrophe %27
      wwStr(&w, e);
    }
  }
  char tail[96];
  snprintf(tail, sizeof(tail), "&gsrlimit=%d&prop=extracts%%7Cdescription&exintro=1&explaintext=1"
           "&exsentences=6", WIKI_PAGES);
  wwStr(&w, tail);
  return wwEnd(&w);
}

// ── the response ─────────────────────────────────────────────────────────────────────────

/* The length of s[0, o) without a last character that was cut short (its lead byte and the
 * continuation bytes after it); a whole one stays. */
static size_t utf8Whole(const char* s, size_t o) {
  size_t i = o;
  while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
  if (i > 0 && (unsigned char)s[i - 1] >= 0xC0) {
    const unsigned char lead = (unsigned char)s[i - 1];
    const size_t need = lead >= 0xF0 ? 4 : (lead >= 0xE0 ? 3 : 2);
    if (o - (i - 1) < need) {
      return i - 1;
    }
  }
  return o;
}

/* A string read into a buffer it did not fit: json_read copies raw UTF-8 a byte at a time, so the
 * cut can land inside a character (an escaped one never is cut). Back to a whole one. */
static void utf8Trim(char* s) {
  s[utf8Whole(s, strlen(s))] = '\0';
}

/* Nothing but whitespace and control characters: what collapse() makes "" (so a kept page's title
 * and extract are never empty once collapsed, and wikiBackground's pages are always the first ones). */
static bool wikiBlank(const char* s) {
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    if (*p > 0x20 && *p != 0x7F) {
      return false;
    }
  }
  return true;
}

/* One page into `pg`: 1 = a page to keep, 0 = not (skipped), -1 = not JSON. */
static int parsePage(Js* j, WikiPage* pg) {
  memset(pg, 0, sizeof(*pg));
  pg->index = 0x7FFFFFFF;                          // no rank: after every ranked page
  if (!jsOpen(j, '{')) {
    return jsSkip(j) ? 0 : -1;
  }
  bool gone = false;
  char key[24];
  int r;
  while ((r = jsObjNext(j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "title")) {
      if (!jsStrInto(j, pg->title, sizeof(pg->title))) return -1;
    } else if (!strcmp(key, "description")) {
      if (!jsStrInto(j, pg->desc, sizeof(pg->desc))) return -1;
    } else if (!strcmp(key, "extract")) {
      if (!jsStrInto(j, pg->extract, sizeof(pg->extract))) return -1;   // cut whole characters only
    } else if (!strcmp(key, "index")) {
      double v = 0;
      if (jsNumber(j, &v) == 1 && v >= 0 && v < 1e6) {
        pg->index = (int)v;
      }
    } else if (!strcmp(key, "missing") || !strcmp(key, "invalid")) {
      gone = true;
      if (!jsSkip(j)) return -1;
    } else if (!jsSkip(j)) {
      return -1;
    }
  }
  if (r < 0) {
    return -1;
  }
  utf8Trim(pg->title);
  utf8Trim(pg->desc);
  utf8Trim(pg->extract);
  return (!gone && !wikiBlank(pg->title) && !wikiBlank(pg->extract)) ? 1 : 0;
}

/* The page just read (page[WIKI_PAGES]) into its place by rank; the worst falls off the end. */
static void keepPage(WikiResult* out) {
  const WikiPage* pg = &out->page[WIKI_PAGES];
  int pos = 0;
  while (pos < out->n && out->page[pos].index <= pg->index) pos++;
  const int last = out->n < WIKI_PAGES ? out->n : WIKI_PAGES - 1;
  if (pos > last) {
    return;                                        // ranked below the WIKI_PAGES already kept
  }
  memmove(&out->page[pos + 1], &out->page[pos], sizeof(WikiPage) * (size_t)(last - pos));
  memcpy(&out->page[pos], pg, sizeof(WikiPage));
  if (out->n < WIKI_PAGES) out->n++;
}

static int parsePages(Js* j, WikiResult* out) {
  char key[24];
  if (jsOpen(j, '[')) {                            // formatversion=2: an array
    int r;
    while ((r = jsArrNext(j)) == 1) {
      const int k = parsePage(j, &out->page[WIKI_PAGES]);
      if (k < 0) return -1;
      if (k) keepPage(out);
    }
    return r < 0 ? -1 : 0;
  }
  if (jsOpen(j, '{')) {                            // formatversion=1: keyed by page id
    int r;
    while ((r = jsObjNext(j, key, sizeof(key))) == 1) {
      const int k = parsePage(j, &out->page[WIKI_PAGES]);
      if (k < 0) return -1;
      if (k) keepPage(out);
    }
    return r < 0 ? -1 : 0;
  }
  return jsSkip(j) ? 0 : -1;
}

static int parseQuery(Js* j, WikiResult* out) {
  if (!jsOpen(j, '{')) {
    return jsSkip(j) ? 0 : -1;
  }
  char key[24];
  int r;
  while ((r = jsObjNext(j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "pages")) {
      if (parsePages(j, out) < 0) return -1;
    } else if (!jsSkip(j)) {                       // "redirects", "searchinfo"...
      return -1;
    }
  }
  return r < 0 ? -1 : 0;
}

int wikiParse(const char* body, size_t len, WikiResult* out) {
  memset(out, 0, sizeof(*out));
  if (!body) {
    return WIKI_BAD;
  }
  Js j = { body, body + len };
  if (!jsOpen(&j, '{')) {
    return WIKI_BAD;
  }
  bool err = false;
  char key[24];
  int r;
  while ((r = jsObjNext(&j, key, sizeof(key))) == 1) {
    if (!strcmp(key, "query")) {
      if (parseQuery(&j, out) < 0) {
        out->n = 0;
        return WIKI_BAD;
      }
    } else if (!strcmp(key, "error")) {
      err = true;
      if (!jsSkip(&j)) {
        out->n = 0;
        return WIKI_BAD;
      }
    } else if (!jsSkip(&j)) {
      out->n = 0;
      return WIKI_BAD;
    }
  }
  if (r < 0) {
    out->n = 0;                                    // cut short: nothing from it is used
    return WIKI_BAD;
  }
  if (err) {
    out->n = 0;
    return WIKI_ERROR;
  }
  return out->n > 0 ? WIKI_OK : WIKI_NONE;
}

// ── relevance ────────────────────────────────────────────────────────────────────────────

/* `s` as the match reads it, into out (cap > 2): " " + THE SHARED FOLD the keywords read through
 * (KwFold: "Pokémon" -> "pokemon", "Øresund" -> "oresund", "Straße" -> "strasse"), every run of
 * what is not [a-z0-9] ONE space (an apostrophe too, as before), + " " - so " raider " is a
 * whole-word test: "tomb raider: legacy" -> " tomb raider legacy ", "Navier–Stokes equations" ->
 * " navier stokes equations ". The fold at most doubles a byte (" deg" for the 2 of "°"): a title
 * fits 2 * WIKI_TITLE_MAX + 4, and the cap is never passed. */
static void relNorm(const char* s, char* out, size_t cap) {
  KwFold f;
  kfInit(&f, s);
  size_t o = 0;
  out[o++] = ' ';
  for (char c; o + 2 < cap && (c = kfNext(&f)) != '\0';) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[o++] = c;
    } else if (out[o - 1] != ' ') {
      out[o++] = ' ';
    }
  }
  if (out[o - 1] != ' ') {
    out[o++] = ' ';
  }
  out[o] = '\0';
}

int wikiRelevant(WikiResult* r, const char* keywords) {
  if (!r) {
    return 0;
  }
  if (r->n < 0 || r->n > WIKI_PAGES) {
    r->n = 0;
  }
  char title[2 * WIKI_TITLE_MAX + 4], desc[2 * WIKI_DESC_MAX + 4];
  char w[WIKI_WORD_MAX + 1], k[WIKI_WORD_MAX + 4];
  int kept = 0;
  for (int i = 0; i < r->n; i++) {
    relNorm(r->page[i].title, title, sizeof(title));
    relNorm(r->page[i].desc, desc, sizeof(desc));
    bool about = false;
    for (const char* c = keywords ? keywords : ""; !about && kwListNext(&c, w);) {
      relNorm(w, k, sizeof(k));                   // "o'neill" -> " o neill ": the words, in order
      about = strcmp(k, " ") != 0 && (strstr(title, k) || strstr(desc, k));
    }
    if (about) {
      if (kept != i) {
        memcpy(&r->page[kept], &r->page[i], sizeof(WikiPage));
      }
      kept++;
    }
  }
  r->n = kept;
  return kept;
}

// ── the background ───────────────────────────────────────────────────────────────────────

/* `in` with every run of whitespace and control characters as one space, both ends trimmed, into
 * out (cap > 0), never cutting inside a UTF-8 character. Returns the length; *more when some of it
 * did not fit. */
static size_t collapse(const char* in, char* out, size_t cap, bool* more) {
  size_t o = 0;
  bool space = false;
  *more = false;
  for (const unsigned char* p = (const unsigned char*)in; *p; p++) {
    if (*p <= 0x20 || *p == 0x7F) {
      space = o > 0;
      continue;
    }
    if (space) {
      if (o + 1 >= cap) {
        *more = true;
        break;
      }
      out[o++] = ' ';
      space = false;
    }
    if (o + 1 >= cap) {
      *more = true;
      break;
    }
    out[o++] = (char)*p;
  }
  if (*more) {
    o = utf8Whole(out, o);                         // the copy may have stopped inside a character
  }
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = '\0';
  return o;
}

/* Where to cut `s` (length n, `more` = it went on past n) to at most `want` bytes: the whole when
 * it is that short, else after the last sentence end (". " "! " "? " or the end) in the first
 * `want`, if there is one past a third of it; 0 = none (cut at a word instead). */
static size_t sentenceEnd(const char* s, size_t n, bool more, size_t want) {
  if (!more && n <= want) {
    return n;
  }
  for (size_t i = (want < n ? want : n); i > want / 3; i--) {
    const char c = s[i - 1];
    if ((c == '.' || c == '!' || c == '?') && (i == n ? !more : s[i] == ' ')) {
      return i;
    }
  }
  return 0;
}

size_t wikiBackground(const WikiResult* r, size_t maxChars, char* out, size_t cap, int* used) {
  if (used) *used = 0;
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  const size_t lim = maxChars < cap - 1 ? maxChars : cap - 1;
  const size_t head = sizeof(WIKI_BG_HEAD) - 1;
  if (!r || r->n <= 0 || head >= lim) {
    return 0;
  }
  memcpy(out, WIKI_BG_HEAD, head);
  size_t o = head;
  int k = 0;
  for (int i = 0; i < r->n && i < WIKI_PAGES; i++) {
    const WikiPage* pg = &r->page[i];
    char title[WIKI_TITLE_MAX], desc[WIKI_DESC_MAX];
    bool more;
    collapse(pg->title, title, sizeof(title), &more);
    collapse(pg->desc, desc, sizeof(desc), &more);
    if (!title[0]) {
      break;
    }
    char prefix[WIKI_TITLE_MAX + WIKI_DESC_MAX + 16];
    snprintf(prefix, sizeof(prefix), "%s- %s%s%s%s: ", k ? "\n" : "", title, desc[0] ? " (" : "",
             desc, desc[0] ? ")" : "");
    const size_t pl = strlen(prefix);
    if (o + pl + WIKI_EXTRACT_MIN > lim) {
      break;                                       // too little room for this page: none after it either
    }
    size_t want = lim - o - pl;
    if (want > WIKI_EXTRACT_CUT) want = WIKI_EXTRACT_CUT;
    /* The extract, collapsed, straight into place - up to `want` bytes and one more (to see whether
     * it goes on) - then cut back to a sentence end, or a word. */
    char* x = out + o + pl;
    const size_t room = lim - o - pl;              // >= want: what the copy may use, NUL aside
    size_t xn = collapse(pg->extract, x, (want + 2 < room + 1 ? want + 2 : room + 1), &more);
    if (xn > want) {
      more = true;
    }
    if (xn == 0) {
      break;                                       // never (wikiParse keeps no blank extract); and
    }                                              // *used must stay "the FIRST *used pages"
    size_t cut = sentenceEnd(x, xn, more, want);
    if (cut == 0) {
      /* No sentence ends in reach: at the last space that leaves room for "...", else at a
       * character boundary. */
      size_t c = want >= 3 ? want - 3 : 0;
      if (c > xn) c = xn;
      size_t s = c;
      while (s > 0 && x[s] != ' ') s--;
      if (s > want / 3) {
        c = s;
      } else {
        while (c > 0 && ((unsigned char)x[c] & 0xC0) == 0x80) c--;
      }
      while (c > 0 && (x[c - 1] == ' ' || x[c - 1] == ',' || x[c - 1] == ';')) c--;
      memcpy(x + c, "...", 3);
      cut = c + 3;
    }
    memcpy(out + o, prefix, pl);                   // the prefix last: the copy above wrote after it
    o += pl + cut;
    out[o] = '\0';
    k++;
  }
  if (k == 0) {
    out[0] = '\0';
    return 0;
  }
  while (o > 0 && out[o - 1] == '\n') o--;         // never: a page always ends in its extract
  out[o] = '\0';
  if (used) *used = k;
  return o;
}

// ── the sources line ─────────────────────────────────────────────────────────────────────

size_t wikiSources(const WikiResult* r, int used, char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  const size_t lim = WIKI_SRC_MAX < cap - 1 ? WIKI_SRC_MAX : cap - 1;
  const size_t pre = sizeof(GEM_SRC_PREFIX) - 1;
  if (!r || used <= 0 || pre + 4 > lim) {
    return 0;
  }
  memcpy(out, GEM_SRC_PREFIX, pre);
  size_t o = pre;
  int named = 0;
  for (int i = 0; i < used && i < r->n && i < WIKI_PAGES; i++) {
    char t[WIKI_TITLE_MAX];
    bool cut = false;
    geminiCleanText(r->page[i].title, t, sizeof(t), &cut);   // the screen's ASCII: "Pokemon"
    for (char* p = t; *p; p++) {
      if (*p == '\n' || *p == ';') *p = ' ';      // one line; "; " is the separator
    }
    size_t tl = strlen(t);
    if (tl == 0) {
      continue;
    }
    const size_t sep = named ? 2 : 0;
    if (o + sep + tl > lim) {
      if (o + sep + 4 > lim) {
        break;                                     // not even "x...": left out
      }
      tl = lim - o - sep - 3;
      while (tl > 0 && t[tl - 1] == ' ') tl--;
      if (tl == 0) {
        break;
      }
      memcpy(t + tl, "...", 4);
      tl += 3;
    }
    if (sep) {
      memcpy(out + o, "; ", 2);
      o += 2;
    }
    memcpy(out + o, t, tl);
    o += tl;
    named++;
  }
  if (!named) {
    out[0] = '\0';
    return 0;
  }
  out[o] = '\0';
  return o;
}

// ── what goes with the question ──────────────────────────────────────────────────────────

int wikiCompose(WikiResult* r, const char* keywords, size_t maxChars, char* bg, size_t bgCap, char* src,
                size_t srcCap, int* why) {
  if (bg && bgCap) bg[0] = '\0';
  if (src && srcCap) src[0] = '\0';
  int dummy;
  if (!why) why = &dummy;
  *why = WIKI_OFF_TOPIC;
  if (!r || !bg || !src || wikiRelevant(r, keywords) == 0) {
    return 0;                                      // nothing about the keywords: nothing goes
  }
  int used = 0;
  if (wikiBackground(r, maxChars, bg, bgCap, &used) == 0 || used <= 0) {
    bg[0] = '\0';
    *why = WIKI_OK;                                // "no page fit"
    return 0;
  }
  if (wikiSources(r, used, src, srcCap) == 0) {
    /* Every title folds to nothing (non-ASCII): the answer could not say where its background came
     * from - so none goes (review 2026-10-11). */
    bg[0] = '\0';
    *why = WIKI_NO_TITLE;
    return 0;
  }
  *why = WIKI_OK;
  return used;
}

// ── the log line ─────────────────────────────────────────────────────────────────────────

const char* wikiSkipWhy(int httpCode, int net, int parsed, char* buf, size_t cap) {
  if (httpCode > 0 && httpCode != 200) {
    snprintf(buf, cap, "HTTP %d%s", httpCode, httpCode == 429 ? " (too many requests)" : "");
    return buf;
  }
  const char* s;
  if (httpCode <= 0) {
    switch (net) {
    case GEM_NET_DNS:       s = "no DNS"; break;
    case GEM_NET_CONNECT:   s = "could not connect"; break;
    case GEM_NET_TLS:       s = "the certificate did not verify"; break;
    case GEM_NET_TLS_OTHER: s = "TLS failed"; break;
    case GEM_NET_NOMEM:     s = "low on memory"; break;
    case GEM_NET_TIMEOUT:   s = "timed out"; break;
    case GEM_NET_LOST:      s = "the connection dropped"; break;
    case GEM_NET_TOO_BIG:   s = "the answer was over 24 KB"; break;
    case GEM_NET_BAD_BODY:  s = "a malformed body"; break;
    default:                s = "no network"; break;
    }
  } else {
    switch (parsed) {
    case WIKI_OK:    s = "no page fit"; break;   // a page that would not fit the background at all
    case WIKI_NONE:  s = "nothing found"; break;
    case WIKI_ERROR: s = "Wikipedia answered an error"; break;
    case WIKI_OFF_TOPIC: s = "no page was about the keywords"; break;
    case WIKI_NO_TITLE:  s = "no page title it can show"; break;
    default:         s = "not JSON it reads"; break;
    }
  }
  snprintf(buf, cap, "%s", s);
  return buf;
}
