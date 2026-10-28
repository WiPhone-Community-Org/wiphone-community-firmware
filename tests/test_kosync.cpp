/*
 * test_kosync.cpp — KOSync (KOReader's sync protocol) on the WiPhone, proven on a Mac.
 *
 * Three things are at stake and each has its own section below:
 *
 *   1. AGREEMENT. Every number in tests/vectors_kosync.h (tools/gen_kosync_vectors.py: MD5,
 *      partial MD5, CrossPoint's byte-weighted spine and its inverse, the JSON number format,
 *      the response shapes) is reproduced by the SHIPPING epub_parse.cpp / kosync.cpp. A
 *      mismatch here is a position that lands in the wrong book or chapter on the X4 or COVEY,
 *      with nothing logged anywhere.
 *   2. THE PROTOCOL. Routing, auth, the window's answers, the request bytes the client sends,
 *      the config file, the window clock — the parts of the feature that are not sockets.
 *   3. NOBODY'S PLACE MOVES. {user}: "we can't let it screw up the position people are at in
 *      their own wiphones after they update". tests/golden_positions.h was generated ONCE
 *      from the parser as it stood before KOSync (git 1de15a6); the current parser must give
 *      every fixture the same reading spine, chapter lengths, ids, epubFraction and
 *      epubLocate, and a 0.9.78 positions file must load and re-save byte for byte.
 */
#include "../WiPhone/epub_parse.h"
#include "../WiPhone/book_hash.h"
#include "../WiPhone/bookstore.h"
#include "../WiPhone/booksync.h"
#include "../WiPhone/booksync_inbox.h"
#include "../WiPhone/kosync.h"
#include "vectors_kosync.h"
#include "golden_positions.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
static const char* g_group = "";

static void group(const char* name) {
  g_group = name;
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char* what) {
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
    printf("  \033[31mFAIL\033[0m %s :: %s\n", g_group, what);
  }
}

static void eqInt(long long got, long long want, const char* what) {
  if (got == want) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s  want=%lld got=%lld\n", g_group, what, want, got);
}

static void eqStr(const char* got, const char* want, const char* what) {
  if (got && want && strcmp(got, want) == 0) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s\n        want: [%s]\n        got:  [%s]\n", g_group, what,
         want ? want : "(null)", got ? got : "(null)");
}

// Same double, or within `tol` (0 = bit-exact).
static void eqDbl(double got, double want, double tol, const char* what) {
  if (got == want || fabs(got - want) <= tol) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s  want=%.17g got=%.17g\n", g_group, what, want, got);
}

// ---------------------------------------------------------------- FILE*-backed source
static size_t fileRead(void* ctx, uint64_t off, void* buf, size_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) {
    return 0;
  }
  return fread(buf, 1, len, f);
}

static bool openSource(const char* file, EpubSource* s, FILE** fOut) {
  char path[256];
  snprintf(path, sizeof(path), "tests/fixtures/%s", file);
  FILE* f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  fseek(f, 0, SEEK_END);
  s->ctx = f;
  s->size = (uint64_t)ftell(f);
  s->read = fileRead;
  *fOut = f;
  return true;
}

static bool isTxt(const char* file) {
  const size_t n = strlen(file);
  return n > 4 && !strcmp(file + n - 4, ".txt");
}

/* Is `needle` anywhere in the `n` bytes at `hay`? 🛑 NOT strstr((const char*)&struct, ...):
 * that stops at the first zero byte, and a KosyncConfig starts with `bool ok` — for a config
 * that is OFF the very first byte is 0, so strstr searched an EMPTY string and "no secret is
 * kept" could never fail (a mutant copying the refused password into c.key passed the suite). */
static bool bytesHave(const void* hay, size_t n, const char* needle) {
  const size_t m = strlen(needle);
  const unsigned char* h = (const unsigned char*)hay;
  for (size_t i = 0; m && i + m <= n; i++) {
    if (!memcmp(h + i, needle, m)) {
      return true;
    }
  }
  return false;
}

// ================================================================ 1. AGREEMENT
static void testMd5() {
  group("MD5 vs RFC 1321");
  for (size_t i = 0; i < sizeof(KS_MD5) / sizeof(KS_MD5[0]); i++) {
    char hex[33];
    bsMd5Hex(KS_MD5[i].in, strlen(KS_MD5[i].in), hex);
    eqStr(hex, KS_MD5[i].hex, KS_MD5[i].in[0] ? KS_MD5[i].in : "(empty)");
    // The same bytes fed one at a time: the block boundary and the padding must not care.
    BsMd5 c;
    uint8_t d[16];
    bsMd5Init(&c);
    for (const char* p = KS_MD5[i].in; *p; p++) {
      bsMd5Update(&c, p, 1);
    }
    bsMd5Final(&c, d);
    bsHex(d, 16, hex);
    eqStr(hex, KS_MD5[i].hex, "byte-at-a-time");
  }
  // Lengths either side of the 56-byte padding edge and the 64-byte block.
  static const char* EDGE[] = {
    "0123456789012345678901234567890123456789012345678901234",       // 55
    "01234567890123456789012345678901234567890123456789012345",      // 56
    "0123456789012345678901234567890123456789012345678901234567890123",  // 64
  };
  static const char* EDGE_HEX[] = {
    // hashlib.md5(s).hexdigest()
    "6e7a4fc92eb1c3f6e652425bcc8d44b5", "8af270b2847610e742b0791b53648c09",
    "7f7bfd348709deeaace19e3f535f8c54",
  };
  for (int i = 0; i < 3; i++) {
    char hex[33];
    bsMd5Hex(EDGE[i], strlen(EDGE[i]), hex);
    eqStr(hex, EDGE_HEX[i], "padding edge");
  }
}

static void testDocIds() {
  group("document ids: partial MD5 + md5(filename) vs hashlib");
  for (size_t k = 0; k < sizeof(KS_BOOKS) / sizeof(KS_BOOKS[0]); k++) {
    const KsBook& v = KS_BOOKS[k];
    EpubSource s;
    FILE* f = NULL;
    if (!openSource(v.file, &s, &f)) {
      ok(false, v.file);
      continue;
    }
    eqInt((long long)s.size, v.size, "fixture size");
    char id[33];
    ok(epubKosyncPartialMd5(&s, id), "partial MD5 computed");
    eqStr(id, v.partialMd5, v.file);
    epubKosyncFilenameMd5(v.file, id);
    eqStr(id, v.filenameMd5, "filename id");
    char a[33], bn[33];
    ok(epubKosyncIds(&s, v.file, a, bn), "both ids");
    ok(!strcmp(a, v.partialMd5) && !strcmp(bn, v.filenameMd5), "both ids agree");
    fclose(f);
  }
  for (size_t k = 0; k < sizeof(KS_TXT) / sizeof(KS_TXT[0]); k++) {
    const KsTxt& v = KS_TXT[k];
    EpubSource s;
    FILE* f = NULL;
    if (!openSource(v.file, &s, &f)) {
      ok(false, v.file);
      continue;
    }
    char id[33];
    ok(epubKosyncPartialMd5(&s, id), "txt partial MD5 computed");
    eqStr(id, v.partialMd5, v.file);
    epubKosyncFilenameMd5(v.file, id);
    eqStr(id, v.filenameMd5, "txt filename id");
    fclose(f);
  }
}

/* A card that refuses a read part-way must give NO id, not a confident wrong one. */
struct ShortCtx { FILE* f; uint64_t failAt; };
static size_t shortRead(void* ctx, uint64_t off, void* buf, size_t len) {
  ShortCtx* c = (ShortCtx*)ctx;
  if (off >= c->failAt) {
    return len / 2;
  }
  return fileRead(c->f, off, buf, len);
}

static void testShortRead() {
  group("partial MD5: a failed card read gives no id");
  EpubSource s;
  FILE* f = NULL;
  if (!openSource("kosync-mixed.epub", &s, &f)) {
    ok(false, "fixture");
    return;
  }
  ShortCtx c = { f, 65536 };
  EpubSource bad = { &c, s.size, shortRead };
  char id[33] = "x";
  ok(!epubKosyncPartialMd5(&bad, id), "refused");
  eqStr(id, "", "and left empty");
  fclose(f);
}

static bool openBook(const char* file, EpubBook* b, FILE** f, EpubSource* s) {
  if (!openSource(file, s, f)) {
    return false;
  }
  if (epubOpen(b, s, file, isTxt(file)) != EPUB_OK) {
    fclose(*f);
    return false;
  }
  return true;
}

static void testSpineAndPercent() {
  for (size_t k = 0; k < sizeof(KS_BOOKS) / sizeof(KS_BOOKS[0]); k++) {
    const KsBook& v = KS_BOOKS[k];
    char g[96];
    snprintf(g, sizeof(g), "CrossPoint spine + percentage: %s", v.file);
    group(g);
    EpubSource s;
    FILE* f = NULL;
    EpubBook b;
    if (!openBook(v.file, &b, &f, &s)) {
      ok(false, "opens");
      continue;
    }
    const EpubKosyncMap* m = b.kosync;
    ok(m != NULL, "has a KOSync map");
    if (!m) {
      epubClose(&b);
      fclose(f);
      continue;
    }
    ok(m->sizesKnown, "sizes known");
    eqInt(m->nCp, v.nCp, "CrossPoint item count");
    for (int i = 0; i < v.nCp && i < m->nCp; i++) {
      char w[128];
      snprintf(w, sizeof(w), "cum[%d] (%s, %u B)", i, v.cp[i].path, (unsigned)v.cp[i].size);
      eqInt(m->cum[i], v.cum[i], w);
    }
    eqInt(epubKosyncTotal(m), v.total, "total");
    // The reading spine is the WiPhone's own and must be what the generator modelled.
    eqInt(b.nSpine, v.nReading, "reading spine length");
    for (int r = 0; r < v.nReading && r < b.nSpine; r++) {
      eqStr(b.spine[r].name, v.reading[r], "reading spine entry");
      eqInt(m->readToCp[r], v.readingToCp[r], "reading -> CrossPoint");
    }
    for (int i = 0; i < v.nFwd; i++) {
      double p = -1;
      char w[96];
      snprintf(w, sizeof(w), "forward r=%d within=%g", v.fwd[i].r, v.fwd[i].within);
      ok(epubKosyncPercent(m, v.fwd[i].r, v.fwd[i].within, &p), w);
      eqDbl(p, v.fwd[i].pct, 0.0, w);
    }
    for (int i = 0; i < v.nInv; i++) {
      int r = -9, c = -9;
      double rw = -1, cw = -1;
      char w[96];
      snprintf(w, sizeof(w), "inverse p=%.17g", v.inv[i].p);
      ok(epubKosyncLocate(m, v.inv[i].p, &r, &rw, &c, &cw), w);
      eqInt(c, v.inv[i].cp, w);
      eqDbl(cw, v.inv[i].within, 1e-12, w);
      eqInt(r, v.inv[i].r, w);
      eqDbl(rw, v.inv[i].rwithin, 1e-12, w);
    }
    /* Offsets: a reader position -> percentage -> back to the same place (one byte of slack
     * for the double -> uint32 truncation). This is the round trip a Sync makes. */
    for (int r = 0; r < b.nSpine; r++) {
      if (m->readToCp[r] < 0) {
        continue;
      }
      const size_t len = epubChapterLen(&b, r);
      const uint32_t offs[] = { 0, (uint32_t)(len / 4), (uint32_t)(len / 2), (uint32_t)(len * 9 / 10) };
      for (int j = 0; j < 4; j++) {
        double p = -1;
        ok(epubKosyncPercentAt(&b, r, offs[j], len, &p), "percent at an offset");
        int sp = -1;
        uint32_t off = 0;
        ok(epubKosyncLocateOffset(&b, p, &sp, &off), "locate an offset");
        char w[96];
        snprintf(w, sizeof(w), "round trip r=%d off=%u", r, (unsigned)offs[j]);
        eqInt(sp, r, w);
        ok((off > offs[j] ? off - offs[j] : offs[j] - off) <= 1, w);
        // Independently: the percentage is the vector rule applied to offset / length.
        double direct = -1;
        epubKosyncPercent(m, r, len ? (double)offs[j] / (double)len : 0.0, &direct);
        eqDbl(p, direct, 0.0, "percentAt == percent(offset / length)");
      }
    }
    epubClose(&b);
    fclose(f);
  }
}

static void testTextAndUnsyncable() {
  group("a .txt is one item; a book with no spine is not syncable");
  EpubSource s;
  FILE* f = NULL;
  EpubBook b;
  if (openBook("my-notes.txt", &b, &f, &s)) {
    ok(b.kosync && b.kosync->nCp == 1, "one item");
    eqInt(epubKosyncTotal(b.kosync), 54, "total = the file size");
    double p = -1;
    ok(epubKosyncPercentAt(&b, 0, 13, 52, &p), "percent");
    eqDbl(p, 0.25, 0.0, "pct = offset / extracted length");
    epubClose(&b);
    fclose(f);
  } else {
    ok(false, "my-notes.txt");
  }
  // empty-spine.epub reads (a zip-order fallback chapter) but CrossPoint lists nothing.
  if (openBook("empty-spine.epub", &b, &f, &s)) {
    eqInt(b.nSpine, 1, "still reads");
    eqInt(epubKosyncTotal(b.kosync), 0, "total 0");
    double p = -1;
    ok(!epubKosyncPercentAt(&b, 0, 5, 30, &p), "percent refused");
    int r = 0;
    double w = 0;
    ok(!epubKosyncLocate(b.kosync, 0.5, &r, &w, NULL, NULL), "locate refused");
    epubClose(&b);
    fclose(f);
  } else {
    ok(false, "empty-spine.epub");
  }
  // A map whose zip walk failed: sizes unknown, never a guessed total.
  static EpubKosyncMap m;
  memset(&m, 0, sizeof(m));
  m.nCp = 1; m.nRead = 1; m.cum[0] = 100; m.readToCp[0] = 0; m.cpToRead[0] = 0;
  m.sizesKnown = false;
  double p = -1;
  ok(!epubKosyncPercent(&m, 0, 0.5, &p), "sizes unknown -> refused");
  m.sizesKnown = true;
  ok(epubKosyncPercent(&m, 0, 0.5, &p) && p == 0.5, "sizes known -> 0.5");
  ok(!epubKosyncPercent(&m, 1, 0.5, &p), "a chapter out of range -> refused");
  m.readToCp[0] = -1;
  ok(!epubKosyncPercent(&m, 0, 0.5, &p), "a chapter CrossPoint does not list -> refused");
  // NaN / out of range within and pct are clamped, not propagated.
  m.readToCp[0] = 0;
  ok(epubKosyncPercent(&m, 0, NAN, &p) && p == 0.0, "NaN within -> 0");
  int r = -1;
  double w = -1;
  ok(epubKosyncLocate(&m, 7.0, &r, &w, NULL, NULL) && r == 0 && w == 1.0, "pct > 1 clamps");
  ok(epubKosyncLocate(&m, -3.0, &r, &w, NULL, NULL) && r == 0 && w == 0.0, "pct < 0 clamps");
}

static void testOversized() {
  group("a chapter too big to display sends within 0");
  static EpubBook b;
  static EpubKosyncMap m;
  memset(&b, 0, sizeof(b));
  memset(&m, 0, sizeof(m));
  m.nCp = 2; m.nRead = 2; m.sizesKnown = true;
  m.cum[0] = 1000; m.cum[1] = 1000 + 600 * 1024;
  m.readToCp[0] = 0; m.readToCp[1] = 1; m.cpToRead[0] = 0; m.cpToRead[1] = 1;
  b.kosync = &m;
  b.nSpine = 2;
  double p = -1;
  ok(epubKosyncPercentAt(&b, 1, 20, 30, &p), "percent");
  eqDbl(p, 1000.0 / (1000 + 600 * 1024), 0.0, "the chapter's start, not 2/3 of a placeholder");
  ok(epubKosyncPercentAt(&b, 0, 500, 1000, &p), "a normal chapter");
  eqDbl(p, 500.0 / (1000 + 600 * 1024), 0.0, "normal chapter unaffected");
}

static void testPaths() {
  group("CrossPoint's path rules (its spine only)");
  struct { const char* in; const char* dec; const char* norm; } V[] = {
    { "OEBPS/Text/chapter%202.xhtml", "OEBPS/Text/chapter 2.xhtml", "OEBPS/Text/chapter 2.xhtml" },
    { "a%2Fb%2fc", "a/b/c", "a/b/c" },
    { "bad%zzescape", "bad%zzescape", "bad%zzescape" },
    { "tail%4", "tail%4", "tail%4" },
    { "tail%41", "tailA", "tailA" },
    { "OEBPS/./Text/one.xhtml", "OEBPS/./Text/one.xhtml", "OEBPS/./Text/one.xhtml" },
    { "OEBPS/../Text/one.xhtml", "OEBPS/../Text/one.xhtml", "Text/one.xhtml" },
    { "../../x.xhtml", "../../x.xhtml", "x.xhtml" },
    { "a//b///c/", "a//b///c/", "a/b/c" },
    { "/lead/ing", "/lead/ing", "lead/ing" },
    { "a/b/../../c", "a/b/../../c", "c" },
  };
  for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++) {
    char d[256], n[256];
    epubKosyncDecodePath(V[i].in, d, sizeof(d));
    eqStr(d, V[i].dec, V[i].in);
    epubKosyncNormPath(d, n, sizeof(n));
    eqStr(n, V[i].norm, V[i].in);
    // In place, as epubOpen does it.
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", V[i].in);
    epubKosyncDecodePath(buf, buf, sizeof(buf));
    epubKosyncNormPath(buf, buf, sizeof(buf));
    eqStr(buf, V[i].norm, "in place");
  }
}

static void testFormat() {
  group("percentage as a JSON number (KS_FMT)");
  for (size_t i = 0; i < sizeof(KS_FMT) / sizeof(KS_FMT[0]); i++) {
    char out[32];
    kosyncFormatPct(KS_FMT[i].p, out, sizeof(out));
    eqStr(out, KS_FMT[i].text, KS_FMT[i].text);
  }
  char out[32];
  kosyncFormatPct(-0.2, out, sizeof(out));
  eqStr(out, "0", "negative clamps to 0");
  kosyncFormatPct(1.7, out, sizeof(out));
  eqStr(out, "1", "over 1 clamps to 1");
  kosyncFormatPct(NAN, out, sizeof(out));
  eqStr(out, "0", "NaN -> 0");
  kosyncFormatPct(0.61, out, sizeof(out));
  eqStr(out, "0.61", "0.61");
}

static void testParse() {
  group("reading a progress object (KS_PARSE + the awkward ones)");
  for (size_t i = 0; i < sizeof(KS_PARSE) / sizeof(KS_PARSE[0]); i++) {
    const KsParseVec& v = KS_PARSE[i];
    KosyncProgress p;
    ok(kosyncParseProgress(v.body, strlen(v.body), &p), v.body);
    eqInt(p.hasPct, v.has, "has a percentage");
    if (v.has) {
      eqDbl(p.pct, v.pct, 0.0, "percentage");
      eqStr(p.device, v.device, "device");
      ok(p.hasTimestamp, "has a timestamp");
      eqInt(p.timestamp, v.ts, "timestamp");
    }
  }
  KosyncProgress p;
  const char* nested = "{ \"metadata\": {\"a\":[1,2,{\"b\":\"}\"}]}, \"percentage\" : 1e-1 ,"
                       "\"device\":\"Caf\\u00e9 \\\"X4\\\"\",\"device_id\":\"\\ud83d\\ude00\","
                       "\"document\":\"abc\",\"progress\":null, \"extra\": true }";
  ok(kosyncParseProgress(nested, strlen(nested), &p), "nested values are skipped");
  eqDbl(p.pct, 0.1, 0.0, "1e-1");
  eqStr(p.device, "Caf\xc3\xa9 \"X4\"", "escapes decoded");
  eqStr(p.deviceId, "\xf0\x9f\x98\x80", "a surrogate pair");
  eqStr(p.document, "abc", "document");
  const char* quoted = "{\"percentage\":\"0.25\",\"timestamp\":\"12\"}";
  ok(kosyncParseProgress(quoted, strlen(quoted), &p) && p.hasPct && p.pct == 0.25, "a quoted number");
  eqInt(p.timestamp, 12, "a quoted timestamp");
  const char* nul = "{\"percentage\":null,\"device\":null}";
  ok(kosyncParseProgress(nul, strlen(nul), &p) && !p.hasPct, "null is absent");
  static const char* BAD[] = { "", "[]", "not json", "{\"percentage\":0.5", "{\"a\" 1}",
                               "{\"percentage\":0.5,}", "{\"x\":\"\\q\"}", "{\"x\":\"unterminated}" };
  for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
    ok(!kosyncParseProgress(BAD[i], strlen(BAD[i]), &p), BAD[i][0] ? BAD[i] : "(empty)");
  }
  // Length-bounded: bytes after `len` are not read (a body in a bigger buffer).
  const char* two = "{\"percentage\":0.5}{\"percentage\":0.9}";
  ok(kosyncParseProgress(two, 18, &p) && p.pct == 0.5, "stops at len");
  char esc[64];
  kosyncJsonEscape("a\"b\\c\nd", esc, sizeof(esc));
  eqStr(esc, "a\\\"b\\\\c\\u000ad", "escape");
}

// ================================================================ 2. THE PROTOCOL
static const char* HDRS_OK =
    "PUT /syncs/progress HTTP/1.1\r\nHost: 192.168.4.1\r\nUser-Agent: ESP32HTTPClient\r\n"
    "X-Auth-User:   {user}  \r\nx-auth-key: 5f4dcc3b5aa765d61d8327deb882cf99\r\n"
    "Accept: application/vnd.koreader.v1+json\r\nAuthorization: Basic bmljazpwYXNzd29yZA==\r\n"
    "Content-Length: 12\r\n\r\n";
static const char* USER = "{user}";
static const char* KEY = "5f4dcc3b5aa765d61d8327deb882cf99";   // md5("password") — a test value

static void testHeaders() {
  group("headers and auth");
  char v[64];
  ok(kosyncHeader(HDRS_OK, "x-auth-user", v, sizeof(v)), "found case-insensitively");
  eqStr(v, "{user}", "trimmed");
  ok(kosyncHeader(HDRS_OK, "CONTENT-LENGTH", v, sizeof(v)) && !strcmp(v, "12"), "content-length");
  ok(!kosyncHeader(HDRS_OK, "x-auth", v, sizeof(v)), "a prefix is not a match");
  ok(!kosyncHeader("GET / HTTP/1.1\r\n\r\nx-auth-user: late\r\n", "x-auth-user", v, sizeof(v)),
     "nothing after the blank line");
  ok(kosyncAuthOk(HDRS_OK, USER, KEY), "right user and key");
  ok(!kosyncAuthOk(HDRS_OK, "{user}2", KEY), "wrong user");
  ok(!kosyncAuthOk(HDRS_OK, USER, "5f4dcc3b5aa765d61d8327deb882cf98"), "wrong key");
  ok(!kosyncAuthOk(HDRS_OK, USER, "5F4DCC3B5AA765D61D8327DEB882CF99"), "the key is exact (lower case)");
  ok(!kosyncAuthOk(HDRS_OK, "", ""), "nothing configured authorises nobody");
  ok(!kosyncAuthOk("GET /users/auth HTTP/1.1\r\nx-auth-user: {user}\r\n\r\n", USER, KEY), "no key header");

  group("5.1: which readers may be told a percentage (the marker headers)");
  ok(kosyncClientTakesPercentage(HDRS_OK), "a stock CrossPoint request (x-auth + Authorization: Basic): yes");
  ok(!kosyncClientTakesPercentage("GET / HTTP/1.1\r\nx-auth-user: {user}\r\nx-auth-key: 5f4dcc3b5aa765d6"
                                  "1d8327deb882cf99\r\nAccept: application/vnd.koreader.v1+json\r\n\r\n"),
     "x-auth only (KOReader): no");
  ok(kosyncClientTakesPercentage("GET / HTTP/1.1\r\nx-auth-user: {user}\r\nX-BookSync: 1\r\n\r\n"),
     "X-BookSync: 1 (the X4 fork): yes");
  ok(kosyncClientTakesPercentage("GET / HTTP/1.1\r\nx-booksync: yes\r\n\r\n"),
     "any case, any value");
  ok(kosyncClientTakesPercentage("GET / HTTP/1.1\r\nX-BookSync:\r\n\r\n"), "an empty value still marks");
  ok(kosyncClientTakesPercentage("GET / HTTP/1.1\r\nAuthorization: Basic bmljazpwdw==\r\n\r\n"),
     "Authorization: Basic alone marks (Readest's shape - but it is 401'd before the marker is asked)");
  ok(!kosyncAuthOk("GET / HTTP/1.1\r\nAuthorization: Basic bmljazpwYXNzd29yZA==\r\n\r\n", USER, KEY),
     "...Basic INSTEAD of x-auth-* (Readest) is not authorised on 0.9.79: nobody serves it");
  ok(kosyncClientTakesPercentage("GET / HTTP/1.1\r\nAUTHORIZATION: Bearer x\r\n\r\n"),
     "any Authorization scheme, any case");
  ok(!kosyncClientTakesPercentage("GET / HTTP/1.1\r\nX-BookSync-Extra: 1\r\n\r\n"),
     "a longer name is not the marker");
  ok(!kosyncClientTakesPercentage("GET / HTTP/1.1\r\nX-Book: Sync\r\n\r\n"), "nor a shorter one");
  ok(!kosyncClientTakesPercentage("GET / HTTP/1.1\r\n\r\nX-BookSync: 1\r\n"),
     "a header after the blank line is body, not a marker");
  ok(!kosyncClientTakesPercentage(NULL) && !kosyncClientTakesPercentage(""), "no headers: no");
  eqInt(kosyncStatusCode("HTTP/1.1 200 OK"), 200, "status 200");
  eqInt(kosyncStatusCode("HTTP/1.0 401 Unauthorized"), 401, "status 401");
  eqInt(kosyncStatusCode("garbage"), -1, "not a status line");

  char ch[128];
  strcpy(ch, "7\r\n{\"perce\r\nB;ext=1\r\nntage\":0.5}\r\n0\r\nTrailer: x\r\n\r\n");
  long n = kosyncDechunk(ch, strlen(ch));
  ok(n == 18 && !memcmp(ch, "{\"percentage\":0.5}", 18), "chunked body decoded");
  strcpy(ch, "5\r\nabc");
  ok(kosyncDechunk(ch, strlen(ch)) < 0, "a truncated chunk is refused");
  strcpy(ch, "zz\r\n");
  ok(kosyncDechunk(ch, strlen(ch)) < 0, "no size is refused");
  strcpy(ch, "3\r\nabcX\r\n0\r\n\r\n");
  ok(kosyncDechunk(ch, strlen(ch)) < 0, "a chunk not followed by CRLF is refused");
}

static void testRequests() {
  group("the client's request bytes (contract 7.4)");
  char out[768];
  size_t n = kosyncBuildGet(out, sizeof(out), "192.168.1.20", 8088, USER, KEY,
                            "7db24c08211c49e8e0c2b8522e9efc1b");
  eqStr(out,
        "GET /syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b HTTP/1.1\r\n"
        "Host: 192.168.1.20:8088\r\n"
        "Accept: application/vnd.koreader.v1+json\r\n"
        "x-auth-user: {user}\r\n"
        "x-auth-key: 5f4dcc3b5aa765d61d8327deb882cf99\r\n"
        "X-BookSync: 1\r\n"
        "Connection: close\r\n\r\n", "GET");
  eqInt((long long)n, (long long)strlen(out), "GET length");
  /* 🛑 The phone's own GET carries the marker, judged by the window's own predicate: COVEY
   * D-163 answers `{}` to an unmarked GET of a record with no XPointer - every record a phone
   * wrote - and a phone reading `{}` for its own place PUTs an older one over the newer. */
  ok(kosyncClientTakesPercentage(out), "the phone's own GET carries the marker (a D-163 home tells it its place, not {})");
  ok(kosyncAuthOk(out, USER, KEY), "...beside the x-auth pair, not instead of it");
  char body[256];
  kosyncBuildPutBody(body, sizeof(body), "d1d6d400f5a32bfd6a3534f2a71c0ded", 0.61, "WiPhone-{user}",
                     "0123456789abcdef0123456789abcdef");
  eqStr(body,
        "{\"document\":\"d1d6d400f5a32bfd6a3534f2a71c0ded\",\"progress\":\"\",\"percentage\":0.61,"
        "\"device\":\"WiPhone-{user}\",\"device_id\":\"0123456789abcdef0123456789abcdef\"}", "PUT body");
  n = kosyncBuildPut(out, sizeof(out), "covey.local", 80, USER, KEY, body);
  char want[768];
  snprintf(want, sizeof(want),
           "PUT /syncs/progress HTTP/1.1\r\n"
           "Host: covey.local\r\n"
           "Accept: application/vnd.koreader.v1+json\r\n"
           "Content-Type: application/json\r\n"
           "x-auth-user: {user}\r\n"
           "x-auth-key: 5f4dcc3b5aa765d61d8327deb882cf99\r\n"
           "X-BookSync: 1\r\n"
           "Content-Length: %u\r\n"
           "Connection: close\r\n\r\n%s", (unsigned)strlen(body), body);
  eqStr(out, want, "PUT (port 80 left off Host)");
  ok(kosyncClientTakesPercentage(out), "the PUT carries the marker too: one client, the same on both verbs");
  KosyncProgress p;
  ok(kosyncParseProgress(body, strlen(body), &p) && p.hasPct && p.pct == 0.61, "our body parses back");
  eqStr(p.device, "WiPhone-{user}", "device survives");
  ok(kosyncBuildGet(out, 64, "h", 80, USER, KEY, "doc") == 0, "too small a buffer builds nothing");
}

static const char* PARTIAL = "7db24c08211c49e8e0c2b8522e9efc1b";
static const char* BYNAME = "ad7a1fcd11740eb658e5cd1742a8bfed";

static void serve(const char* method, const char* path, const char* hdrs, const char* body,
                  KosyncServeOut* o, char* reply) {
  static KosyncServed book = { PARTIAL, BYNAME, true, 0.61, "WiPhone-{user}", "wiphone-id", 1790000100u };
  kosyncServe(method, path, hdrs, body, body ? strlen(body) : 0, USER, KEY, &book, 1790000200u,
              o, reply, 512);
}

static void testServe() {
  group("the window's answers");
  KosyncServeOut o;
  char reply[512];
  char hdrs[512];
  // The fork's marker (5.1): these requests may be told the record. The unmarked shapes are below.
  snprintf(hdrs, sizeof(hdrs), "X HTTP/1.1\r\nx-auth-user: %s\r\nx-auth-key: %s\r\nX-BookSync: 1\r\n\r\n",
           USER, KEY);
  const char* noAuth = "X HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n";

  ok(kosyncIsPath("/users/auth") && kosyncIsPath("/syncs/progress") && kosyncIsPath("/healthcheck"),
     "our paths");
  ok(!kosyncIsPath("/") && !kosyncIsPath("/chunk") && !kosyncIsPath("/syncsx"), "not ours");

  serve("GET", "/healthcheck", noAuth, NULL, &o, reply);
  eqInt(o.code, 200, "healthcheck needs no auth");
  eqStr(reply, "{\"state\":\"OK\"}", "healthcheck body");
  serve("GET", "/users/auth", hdrs, NULL, &o, reply);
  eqInt(o.code, 200, "auth ok");
  eqStr(reply, "{\"authorized\":\"OK\"}", "auth body");
  serve("GET", "/users/auth", noAuth, NULL, &o, reply);
  eqInt(o.code, 401, "auth refused");
  eqStr(reply, "{\"message\":\"Unauthorized\",\"code\":2001}", "401 body");
  serve("POST", "/users/create", hdrs, "{\"username\":\"a\",\"password\":\"b\"}", &o, reply);
  eqInt(o.code, 402, "no registration");

  serve("GET", "/syncs/progress/0000000000000000000000000000beef", hdrs, NULL, &o, reply);
  eqInt(o.code, 200, "another document: 200");
  eqStr(reply, "{}", "another document: {} (never a 404)");
  ok(!o.pickedUp, "another document is not a pick-up");

  serve("GET", "/syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b", hdrs, NULL, &o, reply);
  eqInt(o.code, 200, "our book by partial MD5");
  ok(o.pickedUp, "picked up");
  eqStr(reply, "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"percentage\":0.61,\"progress\":\"\","
               "\"device\":\"WiPhone-{user}\",\"device_id\":\"wiphone-id\",\"timestamp\":1790000100}",
        "our place");
  KosyncProgress p;
  ok(kosyncParseProgress(reply, strlen(reply), &p) && p.pct == 0.61, "which parses as 0.61");
  serve("GET", "/syncs/progress/ad7a1fcd11740eb658e5cd1742a8bfed", hdrs, NULL, &o, reply);
  ok(o.code == 200 && o.pickedUp && strstr(reply, "\"document\":\"ad7a1fcd11740eb658e5cd1742a8bfed\""),
     "our book by filename id, echoed");
  serve("GET", "/syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b", noAuth, NULL, &o, reply);
  ok(o.code == 401 && !o.pickedUp, "no auth, no place");

  /* 5.1: the record only to a reader that can take a percentage. The `hdrs` above carry the
   * marker for the rest of this function; here, each shape of request. */
  {
    char ko[512], fork[512], basic[512], wrong[512];
    snprintf(ko, sizeof(ko), "X HTTP/1.1\r\nx-auth-user: %s\r\nx-auth-key: %s\r\n"
             "Accept: application/vnd.koreader.v1+json\r\n\r\n", USER, KEY);
    snprintf(fork, sizeof(fork), "X HTTP/1.1\r\nx-auth-user: %s\r\nx-auth-key: %s\r\n"
             "X-BookSync: 1\r\n\r\n", USER, KEY);
    snprintf(basic, sizeof(basic), "X HTTP/1.1\r\nx-auth-user: %s\r\nx-auth-key: %s\r\n"
             "Authorization: Basic bmljazpwdw==\r\n\r\n", USER, KEY);
    snprintf(wrong, sizeof(wrong), "X HTTP/1.1\r\nx-auth-user: %s\r\nx-auth-key: "
             "5f4dcc3b5aa765d61d8327deb882cf98\r\nX-BookSync: 1\r\n\r\n", USER);
    serve("GET", "/syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b", ko, NULL, &o, reply);
    eqInt(o.code, 200, "x-auth only (KOReader): 200");
    eqStr(reply, "{}", "...and {} for OUR book - 'No progress found', never page 1");
    ok(o.pickedUp && o.unmarkedGet, "...still a pick-up (it may PUT next), and flagged for the log");
    serve("GET", "/syncs/progress/ad7a1fcd11740eb658e5cd1742a8bfed", ko, NULL, &o, reply);
    ok(o.code == 200 && !strcmp(reply, "{}") && o.unmarkedGet, "the same under the file-name id");
    serve("GET", "/syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b", fork, NULL, &o, reply);
    ok(o.code == 200 && o.pickedUp && !o.unmarkedGet, "x-auth + X-BookSync (the fork): the record");
    eqStr(reply, "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"percentage\":0.61,\"progress\":\"\","
                 "\"device\":\"WiPhone-{user}\",\"device_id\":\"wiphone-id\",\"timestamp\":1790000100}",
          "...today's reply, progress \"\" included (the fork falls back to the percentage)");
    serve("GET", "/syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b", basic, NULL, &o, reply);
    ok(o.code == 200 && !o.unmarkedGet && strstr(reply, "\"percentage\":0.61"),
       "x-auth + Authorization: Basic (stock CrossPoint): the record");
    serve("GET", "/syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b", wrong, NULL, &o, reply);
    ok(o.code == 401 && !o.pickedUp && !o.unmarkedGet, "wrong key + X-BookSync: 401 - auth comes first");
    eqStr(reply, "{\"message\":\"Unauthorized\",\"code\":2001}", "...the 401 body");
    serve("GET", "/syncs/progress/0000000000000000000000000000beef", ko, NULL, &o, reply);
    ok(o.code == 200 && !strcmp(reply, "{}") && !o.pickedUp && !o.unmarkedGet,
       "an unknown document stays {} for an unmarked reader (and is not 'no percentage')");
    serve("GET", "/syncs/progress/0000000000000000000000000000beef", fork, NULL, &o, reply);
    ok(o.code == 200 && !strcmp(reply, "{}") && !o.pickedUp, "...and for a marked one");
    serve("GET", "/syncs/progress/0000000000000000000000000000beef", basic, NULL, &o, reply);
    ok(o.code == 200 && !strcmp(reply, "{}") && !o.pickedUp, "...and for a Basic one");
    serve("PUT", "/syncs/progress", ko,
          "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"progress\":\"/body/DocFragment[8]\","
          "\"percentage\":0.7,\"device\":\"KOReader\"}", &o, reply);
    ok(o.code == 200 && o.gotPut && o.park && !o.unmarkedGet,
       "a KOReader PUT is unchanged: parked, a card (the marker is about GETs only)");
    eqStr(reply, "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"timestamp\":1790000200}",
          "...and answered as before");
    serve("GET", "/users/auth", ko, NULL, &o, reply);
    ok(o.code == 200 && !strcmp(reply, "{\"authorized\":\"OK\"}"), "an unmarked reader still logs in");
    KosyncServed lost = { PARTIAL, BYNAME, false, 0.61, "WiPhone-{user}", "wiphone-id", 1790000100u };
    kosyncServe("GET", "/syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b", ko, NULL, 0, USER, KEY,
                &lost, 1790000200u, &o, reply, 512);
    ok(o.code == 200 && !strcmp(reply, "{}") && o.unmarkedGet,
       "no place of ours AND an unmarked reader: {} (and the reader is still named in the log)");
  }

  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"progress\":\"/body/DocFragment[8]\","
        "\"percentage\":0.7,\"device\":\"CrossPoint\",\"device_id\":\"x4\"}", &o, reply);
  eqInt(o.code, 200, "PUT ours: 200");
  eqStr(reply, "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"timestamp\":1790000200}", "PUT reply");
  ok(o.pickedUp && o.gotPut && o.park && !o.otherDoc, "parked");
  eqDbl(o.putPct, 0.7, 0.0, "their percentage");
  eqStr(o.putDevice, "CrossPoint", "their name");

  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"ad7a1fcd11740eb658e5cd1742a8bfed\",\"percentage\":0.6105,\"device\":\"CrossPoint\"}",
        &o, reply);
  ok(o.gotPut && !o.park, "within 0.001 of us: synchronized, no card");
  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"ad7a1fcd11740eb658e5cd1742a8bfed\",\"percentage\":0.2,\"device\":\"WiPhone-{user}\"}",
        &o, reply);
  ok(o.gotPut && !o.park, "our own name coming back: no card");
  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"ad7a1fcd11740eb658e5cd1742a8bfed\",\"percentage\":0.2,\"device\":\"Other\","
        "\"device_id\":\"wiphone-id\"}", &o, reply);
  ok(o.gotPut && !o.park, "our own device id coming back: no card");

  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"someotherbook\",\"percentage\":0.2,\"device\":\"CrossPoint\"}", &o, reply);
  ok(o.code == 200 && o.otherDoc && !o.gotPut && !o.pickedUp, "another book: 200 and discarded");
  eqStr(reply, "{\"document\":\"someotherbook\",\"timestamp\":1790000200}", "still answered properly");
  eqStr(o.otherDocId, "someothe", "...and the start of its id is kept for the screen (#6)");
  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"percentage\":0.7,\"device\":\"CrossPoint\"}",
        &o, reply);
  eqStr(o.otherDocId, "", "our own book leaves no other-book id");

  group("KS-P1 / F7: two phones with ONE name are still two devices");
  // Phone 2 shares phone 1's device= line but has its own MAC-derived id.
  serve("PUT", "/syncs/progress", hdrs,
        "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"percentage\":0.2,\"device\":\"WiPhone-{user}\","
        "\"device_id\":\"phone-two-id\"}", &o, reply);
  ok(o.gotPut && o.park, "same name, different device_id: PARKED (the other phone's place)");
  ok(!kosyncIsOwnRecord("WiPhone-{user}", "phone-two-id", "WiPhone-{user}", "wiphone-id"),
     "both ids present and different: not ours, whatever the names say");
  ok(kosyncIsOwnRecord("Renamed", "wiphone-id", "WiPhone-{user}", "wiphone-id"),
     "our id under another name: ours (the id decides)");
  ok(kosyncIsOwnRecord("WiPhone-{user}", "", "WiPhone-{user}", "wiphone-id"),
     "no id on the record: the name is the fallback");
  ok(!kosyncIsOwnRecord("CrossPoint", "", "WiPhone-{user}", "wiphone-id"), "no id, another name");
  ok(!kosyncIsOwnRecord("WiPhone-{user}", "x4", "WiPhone-{user}", ""),
     "an id we cannot match (we have none) is not ours");
  ok(!kosyncIsOwnRecord("", "", "WiPhone-{user}", "wiphone-id"), "an anonymous record is not ours");

  serve("PUT", "/syncs/progress", hdrs, "{\"percentage\":0.2}", &o, reply);
  eqInt(o.code, 400, "no document: 400");
  serve("PUT", "/syncs/progress", hdrs, "garbage", &o, reply);
  eqInt(o.code, 400, "not JSON: 400");
  serve("PUT", "/syncs/progress", noAuth, "{\"document\":\"x\",\"percentage\":0.2}", &o, reply);
  eqInt(o.code, 401, "PUT without auth: 401");
  serve("GET", "/syncs/progress", hdrs, NULL, &o, reply);
  eqInt(o.code, 404, "GET without a document: 404");
  serve("GET", "/syncs/progress/a/b", hdrs, NULL, &o, reply);
  eqInt(o.code, 404, "a document id with a slash: 404");
  serve("DELETE", "/syncs/progress", hdrs, NULL, &o, reply);
  eqInt(o.code, 404, "unknown method: 404");

  // (6) The phone somewhere it cannot express: `{}` for our book, never 0 or a stale place.
  {
    KosyncServed lost = { PARTIAL, BYNAME, false, 0.61, "WiPhone-{user}", "wiphone-id", 1790000100u };
    kosyncServe("GET", "/syncs/progress/7db24c08211c49e8e0c2b8522e9efc1b", hdrs, NULL, 0, USER, KEY,
                &lost, 1790000200u, &o, reply, 512);
    ok(o.code == 200 && o.pickedUp, "no place of ours: still our book, still 200");
    eqStr(reply, "{}", "no place of ours: {}");
    const char* put = "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"percentage\":0.6105,"
                      "\"device\":\"CrossPoint\"}";
    kosyncServe("PUT", "/syncs/progress", hdrs, put, strlen(put), USER, KEY, &lost, 1790000200u,
                &o, reply, 512);
    ok(o.gotPut && o.park, "no place of ours: another device's place is offered (no epsilon)");
    const char* own = "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"percentage\":0.2,"
                      "\"device\":\"WiPhone-{user}\"}";
    kosyncServe("PUT", "/syncs/progress", hdrs, own, strlen(own), USER, KEY, &lost, 1790000200u,
                &o, reply, 512);
    ok(o.gotPut && !o.park, "no place of ours: our own record is still not offered");
  }

  // The worth-parking rule on its own.
  ok(kosyncWorthParking(0.5, "X4", "", 0.3, "WiPhone-A", "id"), "another device, a real move");
  ok(!kosyncWorthParking(0.5009, "X4", "", 0.5, "WiPhone-A", "id"), "within epsilon");
  ok(kosyncWorthParking(0.4985, "X4", "", 0.5, "WiPhone-A", "id"), "backwards counts too");
  ok(!kosyncWorthParking(0.9, "WiPhone-A", "", 0.1, "WiPhone-A", "id"), "our own name");

  ok(kosyncWorthParking(0.9, "WiPhone-A", "phone-b", 0.1, "WiPhone-A", "id"),
     "our own NAME with another device's id: worth a card");
}

static void testClock() {
  group("the window's clock");
  KosyncWindowClock c;
  memset(&c, 0, sizeof(c));
  eqInt(kosyncClockDue(&c, 5), KOSYNC_CLOCK_CLOSED, "never opened");
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_SYNC_MS);
  eqInt(kosyncClockDue(&c, 1000), KOSYNC_CLOCK_RUNNING, "running");
  eqInt(kosyncClockRemainingMs(&c, 1000), 300000, "300 s left");
  eqInt(kosyncClockDue(&c, 1000 + 299999), KOSYNC_CLOCK_RUNNING, "1 ms before the deadline");
  eqInt(kosyncClockDue(&c, 1000 + 300000), KOSYNC_CLOCK_DEADLINE, "the deadline");

  group("the window's clock: a GET's grace is long, a PUT's short (3)");
  /* A stock CrossPoint: GET at 5 s, then loads and maps the book, then (ASK mode) waits for a
   * human. The window must still be there at 5 s + 119 s for the PUT that follows. */
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_SYNC_MS);
  kosyncClockPicked(&c, 5000, KOSYNC_GRACE_GET_MS);
  eqInt(kosyncClockRemainingMs(&c, 5000), 120000, "a GET arms 120 s");
  eqInt(kosyncClockDue(&c, 5000 + 10000), KOSYNC_CLOCK_RUNNING, "still open 10 s after the GET");
  eqInt(kosyncClockDue(&c, 5000 + 119999), KOSYNC_CLOCK_RUNNING, "still open for the human's PUT");
  kosyncClockPicked(&c, 90000, KOSYNC_GRACE_PUT_MS);       // the PUT, 85 s later
  eqInt(kosyncClockRemainingMs(&c, 90000), 10000, "a PUT arms 10 s");
  eqInt(kosyncClockDue(&c, 99999), KOSYNC_CLOCK_RUNNING, "grace running");
  eqInt(kosyncClockDue(&c, 100000), KOSYNC_CLOCK_PICKED, "closes 10 s after the PUT");
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_SYNC_MS);
  kosyncClockPicked(&c, 5000, KOSYNC_GRACE_GET_MS);
  eqInt(kosyncClockDue(&c, 5000 + 120000), KOSYNC_CLOCK_PICKED, "a GET with no PUT: 120 s, then shut");
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_SYNC_MS);
  kosyncClockPicked(&c, 5000, KOSYNC_GRACE_PUT_MS);
  kosyncClockPicked(&c, 8000, KOSYNC_GRACE_GET_MS);        // a new round starts with a GET
  eqInt(kosyncClockDue(&c, 20000), KOSYNC_CLOCK_RUNNING, "the LAST request decides");
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_OPEN_MS);
  kosyncClockPicked(&c, 30000, KOSYNC_GRACE_GET_MS);
  eqInt(kosyncClockRemainingMs(&c, 30000), 31000, "the deadline caps the long grace");
  eqInt(kosyncClockDue(&c, 61000), KOSYNC_CLOCK_DEADLINE, "60 s window: the deadline wins");
  kosyncClockOpen(&c, 1000, KOSYNC_WINDOW_OPEN_MS);
  kosyncClockPicked(&c, 59000, KOSYNC_GRACE_PUT_MS);
  eqInt(kosyncClockRemainingMs(&c, 59000), 2000, "the deadline caps the grace");
  eqInt(kosyncClockDue(&c, 61000), KOSYNC_CLOCK_DEADLINE, "the deadline wins");
  // millis() wraps every ~49.7 days: a window opened just before must still close on time.
  kosyncClockOpen(&c, 0xFFFFF000u, KOSYNC_WINDOW_OPEN_MS);
  eqInt(kosyncClockDue(&c, 0x00000100u), KOSYNC_CLOCK_RUNNING, "across the wrap: running");
  eqInt(kosyncClockDue(&c, 0xFFFFF000u + 60000u), KOSYNC_CLOCK_DEADLINE, "across the wrap: closes");
}

static void testConfig() {
  group("/books/kosync.txt");
  KosyncConfig c;
  const char* full =
      "# KOSync\r\n"
      "user = {user}\r\n"
      "password=correct horse battery\r\n"
      "home=http://192.168.1.20:8088/\r\n"
      "device=WiPhone-{user}\r\n"
      "auto=on\r\n"
      "open_window = yes\r\n"
      "unknown=whatever\r\n";
  ok(kosyncParseConfig(full, strlen(full), &c), "a full file");
  ok(c.ok, "ok");
  eqStr(c.user, "{user}", "user");
  char want[33];
  bsMd5Hex("correct horse battery", 21, want);
  eqStr(c.key, want, "the key is MD5(password)");
  ok(!c.passwordShort, "21 characters is long enough");
  eqStr(c.home, "192.168.1.20", "home host, scheme and slash stripped");
  eqInt(c.homePort, 8088, "home port");
  eqStr(c.device, "WiPhone-{user}", "device");
  ok(c.autoOnClose && c.openWindow, "switches");
  eqStr(c.problem, "", "no problem");
  ok(!bytesHave(&c, sizeof(c), "correct horse"), "the plaintext is not kept (any byte of the config)");

  const char* minimal = "user=a\npassword=short\n";
  ok(kosyncParseConfig(minimal, strlen(minimal), &c) && c.passwordShort, "short password warns");
  eqStr(c.problem, "password is under 12 characters", "and says so");
  ok(!c.autoOnClose && !c.openWindow && !c.home[0] && c.homePort == 80, "defaults: off, no home");
  const char* keyed = "user=a\nkey=5F4DCC3B5AA765D61D8327DEB882CF99\nhome=covey.local\n";
  ok(kosyncParseConfig(keyed, strlen(keyed), &c), "key= instead of a password");
  eqStr(c.key, KEY, "lower-cased");
  eqStr(c.home, "covey.local", "a hostname");
  const char* noPw = "user=a\n";
  ok(!kosyncParseConfig(noPw, strlen(noPw), &c) && !c.ok, "no password: off");
  eqStr(c.problem, "no password= line", "and says why");
  const char* noUser = "password=abcdefghijklmnop\n";
  ok(!kosyncParseConfig(noUser, strlen(noUser), &c), "no user: off");
  const char* https = "user=a\npassword=abcdefghijklmnop\nhome=https://x.example\n";
  ok(kosyncParseConfig(https, strlen(https), &c) && !c.home[0], "https refused, no client");
  eqStr(c.problem, "home= must be http:// (no TLS here)", "and says so");
  ok(!kosyncParseConfig("", 0, &c), "empty: off");

  group("/books/kosync.txt: hotspot_pass");
  const char* base = "user=a\npassword=abcdefghijklmnop\n";
  char txt[256];
  ok(kosyncParseConfig(base, strlen(base), &c) && !c.hotspotPass[0] && !c.hotspotNote[0],
     "absent: open, and nothing to say");
  snprintf(txt, sizeof(txt), "%shotspot_pass= trail and lead  \r\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c), "valid parses");
  eqStr(c.hotspotPass, "trail and lead", "valid: kept, spaces inside, ends trimmed");
  eqStr(c.hotspotNote, "", "valid: no note");
  eqStr(c.problem, "", "and no effect on the account's own note");
  snprintf(txt, sizeof(txt), "%shotspot_pass=1234567\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && c.ok, "too short: KOSync still on");
  eqStr(c.hotspotPass, "", "too short (7): OPEN");
  eqStr(c.hotspotNote, "hotspot_pass ignored: 8-63 characters", "too short: says why");
  snprintf(txt, sizeof(txt), "%shotspot_pass=12345678\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !strcmp(c.hotspotPass, "12345678"), "8: accepted");
  char p63[64], p64[65];
  memset(p63, 'x', 63); p63[63] = '\0';
  memset(p64, 'x', 64); p64[64] = '\0';
  snprintf(txt, sizeof(txt), "%shotspot_pass=%s\n", base, p63);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !strcmp(c.hotspotPass, p63), "63: accepted whole");
  snprintf(txt, sizeof(txt), "%shotspot_pass=%s\n", base, p64);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !c.hotspotPass[0], "too long (64): OPEN");
  eqStr(c.hotspotNote, "hotspot_pass ignored: 8-63 characters", "too long: says why");
  snprintf(txt, sizeof(txt), "%shotspot_pass=caf\xc3\xa9-pass-word\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !c.hotspotPass[0], "non-ASCII: OPEN");
  eqStr(c.hotspotNote, "hotspot_pass ignored: plain ASCII only", "non-ASCII: says why");
  snprintf(txt, sizeof(txt), "%shotspot_pass=tab\there-12\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !c.hotspotPass[0], "a control character: OPEN");
  snprintf(txt, sizeof(txt), "%shotspot_pass=\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !c.hotspotPass[0] && c.hotspotNote[0],
     "empty: open, and said");
  snprintf(txt, sizeof(txt), "%shotspot_pass=good-password\nhotspot_pass=bad\n", base);
  ok(kosyncParseConfig(txt, strlen(txt), &c) && !c.hotspotPass[0] && c.hotspotNote[0],
     "the last line wins, invalid included");

  group("/books/kosync.txt: inline comments (critic #1)");
  {
    // The CHANGELOG's old template, copied as it stood: a comment on every line.
    const char* tmpl =
        "user={user}          # the one account all your devices share\n"
        "password=abcdefghijklmnop   # kept only as its MD5\n"
        "home=192.168.1.55:8088      # optional\n"
        "device=WiPhone-Sam\t# optional\n"
        "auto=on   # optional: closing a book opens a window\n"
        "open_window=on # optional\n"
        "hotspot_pass=goodpassword   # optional\n";
    ok(!kosyncParseConfig(tmpl, strlen(tmpl), &c) && !c.ok,
       "a password line with a comment: REFUSED, KOSync off (never a guessed MD5)");
    eqStr(c.problem, "password= has ' #' - put comments on their own line", "and it says why");
    eqStr(c.user, "{user}", "user: the comment is cut off");
    eqStr(c.home, "192.168.1.55", "home: the comment is cut off");
    eqInt(c.homePort, 8088, "home port");
    eqStr(c.device, "WiPhone-Sam", "device: a TAB before the '#' counts too");
    ok(c.autoOnClose && c.openWindow, "auto/open_window: the comment no longer turns them off");
    eqStr(c.hotspotPass, "", "hotspot_pass with a comment: refused -> open");
    eqStr(c.hotspotNote, "hotspot_pass ignored: ' #' in it", "...and said");
    ok(!bytesHave(&c, sizeof(c), "goodpassword") && !bytesHave(&c, sizeof(c), "abcdefghijklmnop"),
       "neither refused secret is kept anywhere in the config (all sizeof(c) bytes searched)");
    // ...and the search itself can see a secret there (the check above is not decorative).
    {
      KosyncConfig probe;
      memset(&probe, 0, sizeof(probe));
      memcpy(probe.key, "abcdefghijklmnop", 16);
      ok(bytesHave(&probe, sizeof(probe), "abcdefghijklmnop"), "the byte search finds a secret in an OFF config");
      memset(&probe, 0, sizeof(probe));
      memcpy(probe.hotspotNote + 20, "goodpassword", 12);
      ok(bytesHave(&probe, sizeof(probe), "goodpassword"), "...anywhere, past any zero byte");
    }

    const char* keyed = "user=a\nkey=5f4dcc3b5aa765d61d8327deb882cf99  # from COVEY\n";
    ok(!kosyncParseConfig(keyed, strlen(keyed), &c), "key= with a comment: refused");
    eqStr(c.problem, "key= has ' #' - put comments on their own line", "and says why");

    // The refusal is WHY it is off, and a later home= complaint must not hide it (repair #4).
    const char* thenHttps = "user=a\npassword=abcdefghijklmnop  # mine\nhome=https://x.example\n";
    ok(!kosyncParseConfig(thenHttps, strlen(thenHttps), &c) && !c.ok, "refused password, then a bad home=: off");
    eqStr(c.problem, "password= has ' #' - put comments on their own line",
          "...and the screen names the PASSWORD, not the home= line");
    const char* thenPort = "user=a\nkey=5f4dcc3b5aa765d61d8327deb882cf99 # c\nhome=h:99999\n";
    ok(!kosyncParseConfig(thenPort, strlen(thenPort), &c), "refused key, then a bad port: off");
    eqStr(c.problem, "key= has ' #' - put comments on their own line", "...the key's reason wins");
    const char* thenPath = "user=a\nkey=nothex\nhome=covey/x\n";
    ok(!kosyncParseConfig(thenPath, strlen(thenPath), &c), "a key that is not hex, then a path: off");
    eqStr(c.problem, "key= must be 32 hex digits (MD5 of the password)", "...the key's reason wins");
    const char* noUserEither = "password=abcdefghijklmnop # x\n";
    ok(!kosyncParseConfig(noUserEither, strlen(noUserEither), &c), "no user AND a refused password: off");
    eqStr(c.problem, "password= has ' #' - put comments on their own line", "...the password is named");
    const char* rescued = "user=a\npassword=abcdefghijklmnop # x\nkey=5f4dcc3b5aa765d61d8327deb882cf99\n";
    ok(kosyncParseConfig(rescued, strlen(rescued), &c) && c.ok, "a refused password, but a good key=: on");
    eqStr(c.key, KEY, "...on the key");
    eqStr(c.problem, "password= has ' #' - put comments on their own line", "...and the dropped line is still said");
    ok(!bytesHave(&c, sizeof(c), "abcdefghijklmnop"), "...and not kept");

    // Comments on their own lines (the new template) and '#' INSIDE a value are fine.
    const char* good =
        "# the one account all your devices share\n"
        "user={user}\n"
        "  # kept only as its MD5\n"
        "password=pass#word-long\n"
        "hotspot_pass=hot#spot#pass\n"
        "device=Phone#2\n"
        "auto=on\n";
    ok(kosyncParseConfig(good, strlen(good), &c) && c.ok, "own-line comments: on");
    char want[33];
    bsMd5Hex("pass#word-long", 14, want);
    eqStr(c.key, want, "a '#' with no space before it is part of the password");
    eqStr(c.hotspotPass, "hot#spot#pass", "...and of hotspot_pass");
    eqStr(c.device, "Phone#2", "...and of a name");
    eqStr(c.problem, "", "no problem");

    const char* empty = "user=a\npassword=abcdefghijklmnop\nauto= # nothing\ndevice= # none\n";
    ok(kosyncParseConfig(empty, strlen(empty), &c), "a value that is ONLY a comment");
    eqStr(c.device, "", "...is empty");
    ok(!c.autoOnClose, "auto stays off");
    eqStr(c.problem, "auto= must be on or off", "and an unreadable switch now says so");
    const char* bad = "user=a\npassword=abcdefghijklmnop\nopen_window=maybe\n";
    ok(kosyncParseConfig(bad, strlen(bad), &c) && !c.openWindow, "open_window=maybe: off");
    eqStr(c.problem, "open_window= must be on or off", "...and said");
  }

  uint8_t mac[6] = { 0x24, 0x0a, 0xc4, 0x01, 0x02, 0x03 };
  char id1[33], id2[33];
  kosyncDeviceId(mac, id1);
  kosyncDeviceId(mac, id2);
  eqStr(id1, id2, "device id is stable");
  eqInt((long long)strlen(id1), 32, "32 hex");
}

/* The whole inbound chain: a peer's percentage -> (reading chapter, within) -> the reader's
 * spine-equal fraction -> a CBS1 record signed with the local key -> the inbox -> found for
 * this book -> epubLocate. Every inverse vector must land in the chapter the generator names
 * (or, for "the very end of chapter r", at the start of r+1 — the same place), never in an
 * earlier chapter. */
static void testInboundChain() {
  group("inbound: percentage -> inbox -> the card's fraction -> the right chapter");
  uint8_t key[32];
  bookSyncDeriveKey("test-passcode", key);
  for (size_t k = 0; k < sizeof(KS_BOOKS) / sizeof(KS_BOOKS[0]); k++) {
    const KsBook& v = KS_BOOKS[k];
    EpubSource s;
    FILE* f = NULL;
    EpubBook b;
    if (!openBook(v.file, &b, &f, &s)) {
      ok(false, v.file);
      continue;
    }
    char ids[3][EPUB_ID_MAX];
    const int nIds = epubIds(&b, ids);
    const char* idp[3] = { ids[0], ids[1], ids[2] };
    for (int i = 0; i < v.nInv; i++) {
      int r = -1;
      double w = -1;
      epubKosyncLocate(b.kosync, v.inv[i].p, &r, &w, NULL, NULL);
      char text[BOOKSYNC_MESH_TEXT_MAX];
      ok(kosyncParkText(idp, nIds, r, w, b.nSpine, 0, "CrossPoint", key, text, sizeof(text)),
         "packs");
      bookSyncInboxInit();
      ok(bookSyncInboxPush(text, 0, 0), "the inbox takes it");
      BookSyncRecord rec;
      uint32_t from = 1;
      ok(bookSyncInboxFindFor(key, idp, nIds, &rec, &from) >= 0, "found for this book");
      eqStr(rec.dev, "CrossPoint", "the card will name the peer");
      int sp = -1;
      uint32_t off = 0;
      epubLocate(&b, rec.fraction, &sp, &off);
      const size_t len = epubChapterLen(&b, sp);
      char what[128];
      snprintf(what, sizeof(what), "%s p=%.6f -> r=%d w=%.4f, landed %d at byte %u of %u",
               v.file, v.inv[i].p, r, w, sp, (unsigned)off, (unsigned)len);
      /* In BYTES: epubLocate truncates to a whole byte (as it always has), so on a 7-byte
       * chapter a within of 0.975 is byte 6, not 6.825. Never more than a byte early; never
       * more than 1 % of the chapter (+1 byte) late — the half-quantum nudge. */
      const double want = w * (double)len;
      const bool sameChapter = (sp == r && (double)off + 1.0 >= want &&
                                (double)off <= want + 0.01 * (double)len + 1.0);
      const bool nextStart = (w >= 0.999 && sp == r + 1 && (double)off <= 0.01 * (double)len + 1.0);
      const bool lastEnd = (r == b.nSpine - 1 && w >= 0.999 && sp == r);
      ok(sameChapter || nextStart || lastEnd, what);
    }
    epubClose(&b);
    fclose(f);
  }
  bookSyncInboxInit();
}

/* `mv` (2026-10-09): a renamed book file keeps its KOSync memory and its parked offer. */
static void testRekey() {
  group("a renamed book keeps its KOSync memory (mv)");
  static KosyncMemo m;
  memset(&m, 0, sizeof(m));
  const char* oldId = "0123456789abcdef0123456789abcdef";
  const char* newId = "fedcba9876543210fedcba9876543210";
  const char* other = "11111111111111111111111111111111";
  KosyncMemoEntry* e = kosyncMemoGet(&m, oldId, true);
  e->movedAt = 1790000000u; e->offeredSig = 42; e->unsent = true;
  kosyncMemoGet(&m, other, true)->movedAt = 7;
  m.dirty = false;
  ok(!kosyncRekeyBook(&m, NULL, oldId, oldId), "same name: nothing to do");
  ok(!kosyncRekeyBook(&m, NULL, "", newId) && !kosyncRekeyBook(&m, NULL, oldId, NULL), "no id: nothing");
  ok(!m.dirty, "...and the memo is untouched");
  ok(kosyncRekeyBook(&m, NULL, oldId, newId), "moved");
  ok(m.dirty, "the memo must be saved");
  ok(kosyncMemoGet(&m, oldId, false) == NULL, "nothing left under the old name");
  KosyncMemoEntry* n = kosyncMemoGet(&m, newId, false);
  ok(n && n->movedAt == 1790000000u && n->offeredSig == 42 && n->unsent, "the last move, the answered offer and unsent follow");
  ok(kosyncMemoGet(&m, other, false) && kosyncMemoGet(&m, other, false)->movedAt == 7, "another book is left alone");
  ok(!kosyncRekeyBook(&m, NULL, "22222222222222222222222222222222", newId), "a book with no memory: nothing moves");

  // An entry already under the new name is replaced by the renamed file's.
  memset(&m, 0, sizeof(m));
  kosyncMemoGet(&m, newId, true)->movedAt = 5;
  kosyncMemoGet(&m, oldId, true)->movedAt = 9;
  ok(kosyncRekeyBook(&m, NULL, oldId, newId), "moved over a stale entry");
  int count = 0;
  for (int i = 0; i < KOSYNC_MEMO_MAX; i++) {
    count += !strcmp(m.e[i].book, newId);
  }
  eqInt(count, 1, "one entry for the book, not two");
  ok(kosyncMemoGet(&m, newId, false)->movedAt == 9, "...the renamed file's");
  static uint8_t blob[KOSYNC_MEMO_BLOB_MAX];
  const size_t len = kosyncMemoPack(&m, blob, sizeof(blob));
  static KosyncMemo back;
  ok(kosyncMemoUnpack(&back, blob, len) && kosyncMemoGet(&back, newId, false) &&
     kosyncMemoGet(&back, newId, false)->movedAt == 9 && !kosyncMemoGet(&back, oldId, false),
     "survives a restart under the new name");

  // The park ledger: a parked offer follows the book; a live one under the new name wins.
  static KosyncParkLedger L;
  memset(&L, 0, sizeof(L));
  strcpy(L.book[0], newId); L.id[0] = 0;          // a spent slot under the new name
  strcpy(L.book[1], oldId); L.id[1] = 77; L.sig[1] = 5;
  ok(kosyncRekeyBook(NULL, &L, oldId, newId), "a parked offer moves");
  ok(!strcmp(L.book[1], newId) && L.id[1] == 77 && L.sig[1] == 5, "...with its record and signature");
  ok(!L.book[0][0], "...and the spent slot no longer shadows it");
  memset(&L, 0, sizeof(L));
  strcpy(L.book[0], newId); L.id[0] = 88;
  strcpy(L.book[1], oldId); L.id[1] = 77;
  ok(!kosyncRekeyBook(NULL, &L, oldId, newId), "a live offer under the new name stays the one");
  ok(!strcmp(L.book[0], newId) && L.id[0] == 88 && !strcmp(L.book[1], oldId), "...nothing changes");
}

/* (5) The inbox holds FOUR records for every book and both transports. A peer PUTting
 * over and over, or the same book opened again and again, must replace its own KOSync offer,
 * never push another book's LoRa position out of the far end. */
static void testLedger() {
  group("the park ledger: one KOSync offer per book, other books' positions kept (5)");
  uint8_t key[32];
  bookSyncDeriveKey("test-passcode", key);
  bookSyncInboxInit();
  // Three LoRa positions for three OTHER books, as COVEY would have sent them.
  const char* others[3] = { "id:book-one", "id:book-two", "id:book-three" };
  for (int i = 0; i < 3; i++) {
    BookSyncRecord r;
    const char* ids[1] = { others[i] };
    bookSyncMakeRecord(&r, ids, 1, 2, 100, 0.25 * (i + 1), 1790000000u + i, "COVEY", NULL);
    char text[BOOKSYNC_MESH_TEXT_MAX];
    ok(bookSyncPackMesh(&r, key, text, sizeof(text)), "pack a LoRa record");
    ok(bookSyncInboxPush(text, 0x1234, 0), "park it");
  }
  static KosyncParkLedger L;
  memset(&L, 0, sizeof(L));
  const char* mine[1] = { "id:urn-uuid-kosync-mixed-0001" };
  uint32_t lastId = 0;
  for (int n = 0; n < 10; n++) {
    char text[BOOKSYNC_MESH_TEXT_MAX];
    ok(kosyncParkText(mine, 1, 1, 0.1 * n, 4, 0, "CrossPoint", key, text, sizeof(text)), "pack");
    ok(kosyncParkInto(&L, BYNAME, text, 0.05 * n, 0, 0), "parked");
    eqInt(bookSyncInboxCount(), 4, "three others + ONE for this book, every time");
    lastId = bookSyncInboxIdAt(bookSyncInboxCount() - 1);
  }
  for (int i = 0; i < 3; i++) {
    const char* ids[1] = { others[i] };
    BookSyncRecord r;
    uint32_t from = 0;
    ok(bookSyncInboxFindFor(key, ids, 1, &r, &from) >= 0, "another book's LoRa position survived");
  }
  BookSyncRecord r;
  uint32_t from = 1;
  ok(bookSyncInboxFindFor(key, mine, 1, &r, &from) >= 0, "our book's offer is there");
  eqInt(r.spine, 1, "and it is the newest one");
  double peer = -1;
  ok(kosyncParkedPeerPct(&L, lastId, &peer), "the peer's own percentage is kept");
  eqDbl(peer, 0.45, 1e-12, "for the card");
  ok(!kosyncParkedPeerPct(&L, bookSyncInboxIdAt(0), &peer), "a LoRa record has none");
  // A second book gets its own slot; the first book's offer stays.
  char text[BOOKSYNC_MESH_TEXT_MAX];
  const char* b2[1] = { "ta:plain" };
  ok(kosyncParkText(b2, 1, 0, 0.5, 3, 0, "CrossPoint", key, text, sizeof(text)), "pack book 2");
  ok(kosyncParkInto(&L, "otherbook", text, 0.3, 0, 0), "park book 2");
  ok(bookSyncInboxFindFor(key, mine, 1, &r, &from) >= 0, "book 1's offer still there");
  // The reader took the offer (the card removes it): the next park simply adds.
  bookSyncInboxDropForBook(key, mine, 1);
  ok(kosyncParkText(mine, 1, 2, 0.5, 4, 0, "CrossPoint", key, text, sizeof(text)), "pack");
  ok(kosyncParkInto(&L, BYNAME, text, 0.7, 0, 0), "parks after its predecessor was taken");
  bookSyncInboxInit();
}

/* (7) KOSync's per-book data is OPTIONAL: with every one of its allocations failing, every
 * fixture opens exactly as 0.9.78 opened it, and is simply not syncable. */
static void testAllocFailure() {
  group("KOSync's allocations all fail: every book still opens, identically (7)");
  epubTestFailKosyncAllocs = 1;
  for (size_t k = 0; k < sizeof(GP_BOOKS) / sizeof(GP_BOOKS[0]); k++) {
    const GpBook& g = GP_BOOKS[k];
    EpubSource s;
    FILE* f = NULL;
    if (!openSource(g.file, &s, &f)) {
      ok(false, g.file);
      continue;
    }
    EpubBook b;
    const EpubStatus st = epubOpen(&b, &s, g.file, isTxt(g.file));
    eqInt(st, g.status, g.file);
    eqInt(b.nSpine, g.nSpine, "reading spine length");
    for (int i = 0; i < g.nSpine && i < b.nSpine; i++) {
      eqStr(b.spine[i].name, g.names[i], "reading spine entry");
      eqInt(epubChapterLen(&b, i), g.chapLen[i], "chapter length");
    }
    char ids[3][EPUB_ID_MAX];
    const int nIds = epubIds(&b, ids);
    eqInt(nIds, g.nIds, "ids");
    ok(b.kosync == NULL && epubKosyncTotal(b.kosync) == 0, "not syncable");
    double p = -1;
    ok(!epubKosyncPercentAt(&b, 0, 0, 10, &p), "no percentage is invented");
    char id[33];
    ok(!epubKosyncPartialMd5(&s, id), "no partial id without its buffer");
    if (st == EPUB_OK) {
      epubClose(&b);
    }
    fclose(f);
  }
  epubTestFailKosyncAllocs = 0;
}


// ================================================================ review round 2
/* D1 — the offer rule, shared with the X4 fork and COVEY. */
static void testOfferRule() {
  group("D1: the offer rule (a server record -> the card, or not)");
  KosyncOfferIn in;
  memset(&in, 0, sizeof(in));
  in.theirPct = 0.60; in.theirTs = 2000; in.theirDevice = "CrossPoint"; in.theirDeviceId = "x4";
  in.mineOk = true; in.minePct = 0.40; in.myDevice = "WiPhone-{user}"; in.myDeviceId = "me";
  in.myMovedAt = 1000; in.offeredSig = 0;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "another device, newer than our last move");
  in.theirPct = 0.30;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "...even BEHIND us (the card says backwards)");
  in.theirTs = 999;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_OLDER, "provably older than our last move");
  in.theirTs = 1000;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "the same second is NOT provably older");
  in.theirTs = 999; in.myMovedAt = 0;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "our move time unknown: offer (bias)");
  in.theirTs = 0; in.myMovedAt = 1000;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "their time unknown (COVEY unsynced = 0): offer");
  in.theirTs = -5;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "a negative time is unknown too");
  in.theirTs = 2000; in.theirPct = 0.4009;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_IN_STEP, "within 0.001: synchronized");
  in.theirPct = 0.4011;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "just past 0.001: offered");
  in.mineOk = false; in.theirPct = 0.4;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "we cannot place ourselves: no epsilon test");
  in.mineOk = true; in.theirPct = 0.6;
  in.theirDevice = "WiPhone-{user}"; in.theirDeviceId = "me";
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_OWN, "our own record");
  in.theirDeviceId = "phone-2";
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "our NAME, another phone's id: offered (F7)");

  group("D1: a record offered once is not offered again; a new PUT is");
  in.theirDevice = "CrossPoint"; in.theirDeviceId = "x4"; in.theirTs = 2000; in.theirPct = 0.6;
  in.offeredSig = kosyncOfferSig(2000, 0.6, "x4", "CrossPoint");
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_OFFERED, "the very record already offered");
  in.theirTs = 2001;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "a new PUT (new server time) is offered again");
  in.theirTs = 2000; in.theirPct = 0.61;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "another place is another record");
  // Unknown server time: the place and the device still tell one record from another.
  in.theirTs = 0; in.theirPct = 0.6; in.offeredSig = kosyncOfferSig(0, 0.6, "x4", "CrossPoint");
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_OFFERED, "time unknown: same place, same device");
  in.theirPct = 0.7;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "time unknown: a new place");
  ok(kosyncOfferSig(0, 0.6, "x4", "A") == kosyncOfferSig(0, 0.6, "x4", "B"),
     "the id identifies a record, not the name");
  ok(kosyncOfferSig(0, 0.6, "", "A") != kosyncOfferSig(0, 0.6, "", "B"),
     "no id: the name does");
  ok(kosyncOfferSig(5, 0.6, "x4", "") == kosyncOfferSig(5, 0.6000001, "x4", ""),
     "the percentage as it goes on the wire (6 decimals)");
  ok(kosyncOfferSig(0, 0, "", "") != 0, "never 0 (0 = nothing offered)");
  eqStr(kosyncOfferVerdictText(KOSYNC_SKIP_OLDER), "older than your last page turn", "said plainly");

  group("F4: an open and close with no reading does not hide a newer X4 place");
  /* 20:00 the X4 sends 0.60. 21:00 the phone (last MOVED at 19:00, at 0.40) opens and closes the
   * book with no page turned — which restamps the CBS1 turnedAt to 21:00, and used to hide the
   * X4's place behind it. The move stamp is still 19:00, so the X4's place is offered. */
  memset(&in, 0, sizeof(in));
  in.theirPct = 0.60; in.theirTs = 72000; in.theirDevice = "CrossPoint"; in.theirDeviceId = "x4";
  in.mineOk = true; in.minePct = 0.40; in.myDevice = "WiPhone-{user}"; in.myDeviceId = "me";
  in.myMovedAt = 68400;                              // 19:00, the last real move
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "offered against the last MOVE");
  in.myMovedAt = 75600;                              // what the save/close stamp would say (21:00)
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_OLDER, "(the close stamp would have hidden it)");

  group("the two records a read finds: the newest from another device is judged");
  KosyncProgress g[2];
  bool have[2] = { true, true };
  memset(g, 0, sizeof(g));
  g[0].hasPct = true; g[0].pct = 0.4; g[0].hasTimestamp = true; g[0].timestamp = 3000;
  snprintf(g[0].device, sizeof(g[0].device), "WiPhone-{user}");
  snprintf(g[0].deviceId, sizeof(g[0].deviceId), "me");
  g[1].hasPct = true; g[1].pct = 0.6; g[1].hasTimestamp = true; g[1].timestamp = 2000;
  snprintf(g[1].device, sizeof(g[1].device), "CrossPoint");
  bool saw = false;
  eqInt(kosyncPickRecord(g, have, 2, "WiPhone-{user}", "me", &saw), 1,
        "our own newer push under the partial id is skipped; the X4's (filename id) is judged");
  ok(saw, "records were seen");
  snprintf(g[0].device, sizeof(g[0].device), "KOReader");
  snprintf(g[0].deviceId, sizeof(g[0].deviceId), "ko");
  eqInt(kosyncPickRecord(g, have, 2, "WiPhone-{user}", "me", &saw), 0, "two others: the NEWER one");
  g[1].timestamp = 3000;
  eqInt(kosyncPickRecord(g, have, 2, "WiPhone-{user}", "me", &saw), 0, "a tie keeps the partial id's");
  g[1].hasTimestamp = false; g[1].timestamp = 9000;
  eqInt(kosyncPickRecord(g, have, 2, "WiPhone-{user}", "me", &saw), 0, "no timestamp keeps it too");
  have[0] = false;
  eqInt(kosyncPickRecord(g, have, 2, "WiPhone-{user}", "me", &saw), 1, "only the filename id answered");
  have[1] = false;
  eqInt(kosyncPickRecord(g, have, 2, "WiPhone-{user}", "me", &saw), -1, "nothing answered");
  ok(!saw, "...and nothing was seen");
  have[0] = have[1] = true;
  g[0].hasPct = false;                                // `{}`: nobody has synced this document
  g[1].hasPct = false;
  eqInt(kosyncPickRecord(g, have, 2, "WiPhone-{user}", "me", &saw), -1, "two {}: none");
  ok(!saw, "{} is not a record");
}

/* D2 — the automatic push rule (F3). */
static void testAutoPush() {
  group("D2 / F3: a close pushes only a place that MOVED");
  ok(!kosyncAutoPushWanted(true, 0.40, false, true, 0.40), "opened at 0.40, closed at 0.40: no push");
  ok(!kosyncAutoPushWanted(true, 0.40, true, true, 0.4009), "moved and back within 0.001: no push");
  ok(kosyncAutoPushWanted(true, 0.40, true, true, 0.45), "read on to 0.45: push");
  ok(kosyncAutoPushWanted(true, 0.40, true, true, 0.35), "went back to 0.35: push (a move is a move)");
  ok(!kosyncAutoPushWanted(true, 0.45, false, true, 0.45), "pushed at 0.45 by Sync my place: no second push");
  ok(kosyncAutoPushWanted(false, 0.0, true, true, 0.3), "no reference: a recorded move decides (yes)");
  ok(!kosyncAutoPushWanted(false, 0.0, false, true, 0.3), "no reference, no move: no push");
  ok(!kosyncAutoPushWanted(true, 0.1, true, false, 0.0), "a place that cannot be expressed is never sent");

  /* The whole close rule, driven through the memo the way the phone drives it (open -> moves ->
   * close, a PUT that succeeded, a RESTART as a pack/unpack), so what is kept across a reboot
   * is part of what is proven. */
  group("D2 (repair #1): a move home never had is sent by a later close, even an unmoved one");
  static KosyncMemo m;
  static uint8_t blob[KOSYNC_MEMO_BLOB_MAX];
  const char* B = "0123456789abcdef0123456789abcdef";
  memset(&m, 0, sizeof(m));
  // Home has 0.40 from this phone (an earlier close at home pushed it).
  KosyncMemoEntry* e = kosyncMemoGet(&m, B, true);
  kosyncMemoOpened(e, true, 0.30);
  kosyncMemoMoved(&m, e, 1790000000u);
  ok(kosyncClosePushWanted(e, true, 0.40), "read 0.30 -> 0.40 at home: push");
  kosyncMemoSent(&m, e, 0.40);
  ok(!e->unsent && e->sentOk, "the PUT succeeded: nothing unsent");
  // The woods: 0.40 -> 0.60, closed off WiFi (the close cannot push), then a restart.
  kosyncMemoOpened(e, true, 0.40);
  kosyncMemoMoved(&m, e, 0);                        // no clock out there: the stamp is kept
  ok(e->movedAt == 1790000000u, "a move with no clock keeps the older stamp");
  ok(e->unsent && m.dirty, "the move is marked unsent, and the memo is due a save");
  size_t len = kosyncMemoPack(&m, blob, sizeof(blob));
  ok(kosyncMemoUnpack(&m, blob, len), "restart");
  e = kosyncMemoGet(&m, B, false);
  ok(e && e->sentOk && fabs(e->sentPct - 0.40) < 1e-9 && e->unsent, "what home has, and the unsent move, survive it");
  // Home: opened at 0.60 and closed without a page turned.
  kosyncMemoOpened(e, true, 0.60);
  ok(!kosyncAutoPushWanted(e->refOk, e->refPct, e->movedSinceRef, true, 0.60),
     "the session alone says 'not moved' (the bug: 0.60 was never sent)");
  ok(kosyncClosePushWanted(e, true, 0.60), "...but home has 0.40 from us: PUSH 0.60");
  // The push gives up (3 tries, no answer): nothing recorded as sent. Opened and closed again.
  kosyncMemoOpened(e, true, 0.60);
  ok(kosyncClosePushWanted(e, true, 0.60), "a push that gave up: the next unmoved close tries again");
  kosyncMemoSent(&m, e, 0.60);
  // F3 stays fixed: home has 0.60 from us; the X4 then sends 0.75 (nothing on our side).
  kosyncMemoOpened(e, true, 0.60);
  ok(!kosyncClosePushWanted(e, true, 0.60), "F3: home has our 0.60 - an unmoved close sends nothing");
  len = kosyncMemoPack(&m, blob, sizeof(blob));
  kosyncMemoUnpack(&m, blob, len);
  e = kosyncMemoGet(&m, B, false);
  kosyncMemoOpened(e, true, 0.60);
  ok(!kosyncClosePushWanted(e, true, 0.60), "F3 across a restart too (the sent place is saved)");
  // Away and back to what home has, off WiFi: home already has that place from us.
  kosyncMemoMoved(&m, e, 0);
  kosyncMemoMoved(&m, e, 0);
  kosyncMemoOpened(e, true, 0.60);
  ok(!kosyncClosePushWanted(e, true, 0.6004), "away and back within 0.001 of what home has: nothing");
  ok(kosyncClosePushWanted(e, true, 0.62), "...but 0.62 is news");
  // A PUT of 0.60 still in flight when the book was re-opened and read on to 0.62.
  kosyncMemoOpened(e, true, 0.60);
  kosyncMemoMoved(&m, e, 0);
  kosyncMemoSent(&m, e, 0.60);                      // the older PUT lands after the move
  ok(kosyncClosePushWanted(e, true, 0.62), "a move made while a PUT was in flight is still pushed");

  group("D2 (repair #1): a book never sent from this phone");
  memset(&m, 0, sizeof(m));
  e = kosyncMemoGet(&m, B, true);
  kosyncMemoOpened(e, true, 0.40);
  ok(!kosyncClosePushWanted(e, true, 0.40), "never sent, never moved: nothing (F3: the X4's place stays)");
  kosyncMemoMoved(&m, e, 0);
  len = kosyncMemoPack(&m, blob, sizeof(blob));
  ok(len == 4 + KOSYNC_MEMO_ENTRY_BYTES, "an entry with only an unsent move is still saved");
  kosyncMemoUnpack(&m, blob, len);
  e = kosyncMemoGet(&m, B, false);
  kosyncMemoOpened(e, true, 0.55);
  ok(kosyncClosePushWanted(e, true, 0.55), "read off WiFi, restarted, closed unmoved at home: push");
  ok(!kosyncClosePushWanted(e, false, 0.0), "...never a place that cannot be expressed");
  ok(kosyncClosePushWanted(NULL, true, 0.5) && !kosyncClosePushWanted(NULL, false, 0.5),
     "no memo at all: push whatever can be expressed");
}

/* The per-book memo: last move + last offer, persisted; the push reference, not. */
static void testMemo() {
  group("the per-book memo (F4, D1, D2)");
  static KosyncMemo m;
  memset(&m, 0, sizeof(m));
  char id[KOSYNC_MEMO_MAX + 2][33];
  for (int i = 0; i < KOSYNC_MEMO_MAX + 2; i++) {
    snprintf(id[i], sizeof(id[i]), "%032x", i + 1);
  }
  ok(kosyncMemoGet(&m, id[0], false) == NULL, "absent, not created");
  ok(kosyncMemoGet(&m, "", true) == NULL && kosyncMemoGet(&m, NULL, true) == NULL, "no id, no entry");
  KosyncMemoEntry* e = kosyncMemoGet(&m, id[0], true);
  ok(e && !strcmp(e->book, id[0]) && !e->movedAt && !e->offeredSig, "created empty");
  e->movedAt = 1790000000u; e->offeredSig = 77; e->refOk = true; e->refPct = 0.5; e->movedSinceRef = true;
  ok(kosyncMemoGet(&m, id[0], false) == e, "found again");
  for (int i = 1; i < KOSYNC_MEMO_MAX; i++) {
    KosyncMemoEntry* x = kosyncMemoGet(&m, id[i], true);
    x->movedAt = 1000u + (uint32_t)i;
  }
  kosyncMemoGet(&m, id[0], false);                  // book 0 is now the most recently used
  m.dirty = false;
  KosyncMemoEntry* n = kosyncMemoGet(&m, id[KOSYNC_MEMO_MAX], true);
  ok(n != NULL, "full: a new book still gets a slot");
  ok(kosyncMemoGet(&m, id[1], false) == NULL, "...the least recently used one (book 1) went");
  ok(kosyncMemoGet(&m, id[0], false) != NULL, "...not the one just used");
  ok(m.dirty, "evicting stamps marks the memo for saving");

  static uint8_t blob[KOSYNC_MEMO_BLOB_MAX];
  const size_t len = kosyncMemoPack(&m, blob, sizeof(blob));
  // 16 slots: book 0 (moved+offered), books 2..15 (moved), the new one (nothing to keep).
  eqInt((long long)len, 4 + KOSYNC_MEMO_ENTRY_BYTES * (KOSYNC_MEMO_MAX - 1), "only entries with a stamp are kept");
  ok(len <= KOSYNC_MEMO_BLOB_MAX, "fits the NVS blob");
  static KosyncMemo back;
  ok(kosyncMemoUnpack(&back, blob, len), "reads back");
  KosyncMemoEntry* b0 = kosyncMemoGet(&back, id[0], false);
  ok(b0 && b0->movedAt == 1790000000u && b0->offeredSig == 77, "the stamps survive a restart");
  ok(b0 && !b0->refOk && !b0->movedSinceRef && b0->refPct == 0.0, "the session's reference does not");
  ok(kosyncMemoGet(&back, id[5], false) && kosyncMemoGet(&back, id[5], false)->movedAt == 1005u,
     "another book's stamp too");
  ok(!back.dirty, "a fresh load is clean");
  // The LRU order survives too: after the load, a new book evicts the oldest of the old ones.
  kosyncMemoGet(&back, id[0], false);
  KosyncMemoEntry* fresh = kosyncMemoGet(&back, id[KOSYNC_MEMO_MAX + 1], true);
  ok(fresh && !fresh->movedAt, "a free slot is used first");
  kosyncMemoGet(&back, id[KOSYNC_MEMO_MAX + 1], true)->movedAt = 5;
  KosyncMemoEntry* again = kosyncMemoGet(&back, "ffffffffffffffffffffffffffffffff", true);
  ok(again && kosyncMemoGet(&back, id[2], false) == NULL, "full again: the oldest (book 2) goes");

  group("the memo: anything not written by kosyncMemoPack reads as empty");
  ok(!kosyncMemoUnpack(&back, blob, 3), "too short");
  ok(!kosyncMemoUnpack(&back, blob, len - 1), "ragged");
  static uint8_t bad[KOSYNC_MEMO_BLOB_MAX];
  memcpy(bad, blob, len);
  bad[0] = 'X';
  ok(!kosyncMemoUnpack(&back, bad, len), "wrong magic");
  memcpy(bad, blob, len);
  bad[4] = 'z';                                     // not hex
  ok(!kosyncMemoUnpack(&back, bad, len), "a damaged id");
  ok(kosyncMemoGet(&back, id[0], false) == NULL, "...and nothing half-read is kept");
  ok(kosyncMemoUnpack(&back, blob, 4) && kosyncMemoGet(&back, id[0], false) == NULL,
     "just the magic: a valid, empty memo");
  memcpy(bad, blob, len);
  memcpy(bad, "KSM1", 4);
  ok(!kosyncMemoUnpack(&back, bad, len), "the development build's KSM1: empty, not misread");
  memcpy(bad, blob, len);
  bad[4 + 48] = 0x80;                               // a flag nobody writes
  ok(!kosyncMemoUnpack(&back, bad, len), "unknown flags");
  memcpy(bad, blob, len);
  bad[4 + 44] = 0x41; bad[4 + 45] = 0x42; bad[4 + 46] = 0x0f; bad[4 + 47] = 0x00;   // 1,000,001
  ok(!kosyncMemoUnpack(&back, bad, len), "a sent place past 100 %");

  group("the memo: what home has from us survives a restart (D2, repair #1)");
  memset(&m, 0, sizeof(m));
  KosyncMemoEntry* s1 = kosyncMemoGet(&m, id[0], true);
  kosyncMemoSent(&m, s1, 0.4567894);
  KosyncMemoEntry* s2 = kosyncMemoGet(&m, id[1], true);
  kosyncMemoMoved(&m, s2, 0);
  KosyncMemoEntry* s3 = kosyncMemoGet(&m, id[2], true);
  kosyncMemoSent(&m, s3, 1.0);
  const size_t l2 = kosyncMemoPack(&m, blob, sizeof(blob));
  eqInt((long long)l2, 4 + 3 * KOSYNC_MEMO_ENTRY_BYTES, "sent-only and unsent-only entries are kept");
  ok(kosyncMemoUnpack(&back, blob, l2), "reads back");
  s1 = kosyncMemoGet(&back, id[0], false);
  s2 = kosyncMemoGet(&back, id[1], false);
  s3 = kosyncMemoGet(&back, id[2], false);
  ok(s1 && s1->sentOk && !s1->unsent && fabs(s1->sentPct - 0.456789) < 1e-9, "the sent place, to 6 dp");
  ok(s2 && !s2->sentOk && s2->unsent && !s2->movedAt, "an unsent move with no clock");
  ok(s3 && s3->sentOk && s3->sentPct == 1.0, "the very end");
  ok(s1 && !s1->refOk && !s1->movedSinceRef, "the session's reference is not kept");
}

/* F5 / KS-C1: a push or pull is retried, boundedly; KS-W1 / KS-T1: the window's transport. */
static void testClientAndTransport() {
  group("F5: a job gets KOSYNC_CLIENT_TRIES attempts, 1 s then 4 s apart");
  eqInt(KOSYNC_CLIENT_TRIES, 3, "three tries");
  eqInt(kosyncRetryDelayMs(1), 1000, "after the first failure: 1 s");
  eqInt(kosyncRetryDelayMs(2), 4000, "after the second: 4 s");
  eqInt(kosyncRetryDelayMs(3), 0, "after the third: give up");
  eqInt(kosyncRetryDelayMs(0), 0, "no failure, no wait");
  eqInt(kosyncRetryDelayMs(99), 0, "never unbounded");
  // The whole schedule: at most 3 x 3 s of polled connect + 5 s of waiting, and never a block.
  uint32_t total = 0;
  int tries = 1;
  for (int f = 1; kosyncRetryDelayMs(f); f++) {
    total += kosyncRetryDelayMs(f);
    tries++;
  }
  eqInt(tries, KOSYNC_CLIENT_TRIES, "the schedule ends at the cap");
  eqInt(total, 5000, "5 s of back-off in all");

  group("KS-W1: a window on the station notices the WiFi going");
  const uint32_t IP = 0x2501a8c0u, IP2 = 0x2601a8c0u;
  uint32_t lost = 0;
  eqInt(kosyncStationWindowCheck(false, true, IP, IP, &lost, 1000, KOSYNC_STA_GRACE_MS), KOSYNC_STA_OK, "connected");
  eqInt(kosyncStationWindowCheck(false, false, 0, IP, &lost, 2000, KOSYNC_STA_GRACE_MS), KOSYNC_STA_BLIP, "dropped: a blip first");
  eqInt(lost, 2000, "the drop is timed");
  eqInt(kosyncStationWindowCheck(false, false, 0, IP, &lost, 6999, KOSYNC_STA_GRACE_MS), KOSYNC_STA_BLIP, "4.999 s: still a blip");
  eqInt(kosyncStationWindowCheck(false, false, 0, IP, &lost, 7000, KOSYNC_STA_GRACE_MS), KOSYNC_STA_LOST, "5 s: gone - close it");
  eqInt(kosyncStationWindowCheck(false, true, IP, IP, &lost, 7100, KOSYNC_STA_GRACE_MS), KOSYNC_STA_OK, "back in time: fine");
  eqInt(lost, 0, "...and the clock resets");
  eqInt(kosyncStationWindowCheck(false, false, 0, IP, &lost, 8000, KOSYNC_STA_GRACE_MS), KOSYNC_STA_BLIP, "a second drop starts over");
  eqInt(kosyncStationWindowCheck(false, true, IP2, IP, &lost, 9000, KOSYNC_STA_GRACE_MS), KOSYNC_STA_MOVED,
        "connected at ANOTHER address (auto-switch): the window is stale");
  eqInt(kosyncStationWindowCheck(false, true, IP2, 0, &lost, 9000, KOSYNC_STA_GRACE_MS), KOSYNC_STA_OK,
        "no address recorded: nothing to compare");
  lost = 0;
  eqInt(kosyncStationWindowCheck(false, false, 0, IP, &lost, 0, KOSYNC_STA_GRACE_MS), KOSYNC_STA_BLIP, "a drop at millis() 0");
  ok(lost != 0, "...is still timed (0 means connected)");
  lost = 0xFFFFF000u;
  eqInt(kosyncStationWindowCheck(false, false, 0, IP, &lost, 0x00000388u, KOSYNC_STA_GRACE_MS), KOSYNC_STA_LOST,
        "across the millis() wrap");
  lost = 0;
  eqInt(kosyncStationWindowCheck(true, false, 0, IP, &lost, 1000, KOSYNC_STA_GRACE_MS), KOSYNC_STA_LOST,
        "WiFi switched off / disabled: gone at once (no blip grace)");
  eqInt(kosyncStationWindowCheck(true, true, IP, IP, &lost, 1000, KOSYNC_STA_GRACE_MS), KOSYNC_STA_LOST,
        "...even if the status has not caught up yet");

  group("repair #2: a new ask during a blip keeps the WiFi window (the same rule as the loop)");
  // The ask used to close on ANY status but connected. Through the rule, a sub-second blip is
  // a BLIP: kept, and extended; only after the grace is it LOST (the hotspot's turn).
  lost = 0;
  eqInt(kosyncStationWindowCheck(false, false, 0, IP, &lost, 50000, KOSYNC_STA_GRACE_MS), KOSYNC_STA_BLIP,
        "a book opened 0 ms into a blip: the window stays on WiFi");
  eqInt(kosyncStationWindowCheck(false, true, IP, IP, &lost, 50800, KOSYNC_STA_GRACE_MS), KOSYNC_STA_OK,
        "back 0.8 s later: fine, never closed");
  eqInt(kosyncStationWindowCheck(false, false, 0, IP, &lost, 60000, KOSYNC_STA_GRACE_MS), KOSYNC_STA_BLIP,
        "gone again (the car)...");
  eqInt(kosyncStationWindowCheck(false, false, 0, IP, &lost, 65000, KOSYNC_STA_GRACE_MS), KOSYNC_STA_LOST,
        "...5 s on: LOST, and the next ask brings up the hotspot (KS-W1 stays fixed)");

  group("KS-T1: a window waits for the station only when a join is under way");
  ok(kosyncWindowWaitsForSta(false, false, true, 20000, 15000, 0), "a join 5 s ago: wait for it");
  ok(!kosyncWindowWaitsForSta(false, false, true, 30000, 15000, 0), "the last join 15 s ago: no wait");
  ok(!kosyncWindowWaitsForSta(false, false, true, 30000, 0, 0), "never joined: no wait");
  ok(!kosyncWindowWaitsForSta(true, false, true, 20000, 15000, 19000), "WiFi switched off: no wait");
  ok(!kosyncWindowWaitsForSta(false, true, true, 20000, 15000, 19000), "WiFi disabled (no saved network): no wait");
  ok(!kosyncWindowWaitsForSta(false, false, false, 20000, 15000, 19000), "station not running: no wait");
  ok(kosyncWindowWaitsForSta(false, false, true, 5, 0xFFFFFF00u, 0), "a join just before the wrap");

  group("repair #2: a blip or a roam at home is a join under way too");
  // Joined at boot (an hour ago), up ever since, dropped 300 ms ago: the core's own rejoin is
  // WiFi.begin(), which stamps no join — the link-up time is what says "rejoining".
  ok(kosyncWindowWaitsForSta(false, false, true, 3600300u, 60000u, 3600000u),
     "up until 0.3 s ago (a blip): wait the 2 s for it");
  ok(kosyncWindowWaitsForSta(false, false, true, 3609000u, 60000u, 3600000u), "...9 s on: still");
  ok(!kosyncWindowWaitsForSta(false, false, true, 3611000u, 60000u, 3600000u),
     "11 s on (out of range, the car): no wait - KS-T1 stays fixed");
  ok(!kosyncWindowWaitsForSta(false, false, true, 7200000u, 60000u, 3600000u), "an hour out of range: no wait");
  ok(kosyncWindowWaitsForSta(false, false, true, 5, 0, 0xFFFFFF00u), "up just before the wrap");
  ok(!kosyncWindowWaitsForSta(true, false, true, 3600300u, 60000u, 3600000u),
     "switched off a moment after being up: no wait (nothing will associate)");

  group("5.2: Sync my place asks home first - the window waits only when home was really asked");
  ok(kosyncWindowWaitsForHome(true, true, true), "on WiFi, a home, the job queued: the window waits");
  ok(!kosyncWindowWaitsForHome(false, true, true), "off WiFi: the window opens now (the hotspot)");
  ok(!kosyncWindowWaitsForHome(true, false, true), "no home= in kosync.txt: nothing to wait for");
  ok(!kosyncWindowWaitsForHome(true, true, false), "the home job refused to start: open now, keep its note");
  ok(!kosyncWindowWaitsForHome(false, false, false), "nothing: open now");
  ok(!kosyncWindowWaitsForHome(false, true, false) && !kosyncWindowWaitsForHome(false, false, true) &&
     !kosyncWindowWaitsForHome(true, false, false), "any one thing missing: open now (all eight cases)");
}

/* #6: what went wrong in a window, on the SCREEN; #7: settings files are not books. */
static void testProblemsAndLibrary() {
  group("#6: a window's silent failures, said on the screen");
  char l[200];
  eqInt((long long)kosyncWindowProblems(0, "", 0, 0, l, sizeof(l)), 0, "nothing wrong: nothing said");
  eqStr(l, "", "empty");
  kosyncWindowProblems(1, "0c9a1f7e", 0, 0, l, sizeof(l));
  eqStr(l, "! A place for a DIFFERENT book arrived (id 0c9a1f7e..) - not the same file here?",
        "a PUT for another document");
  kosyncWindowProblems(3, "", 0, 0, l, sizeof(l));
  eqStr(l, "! A place for a DIFFERENT book arrived - not the same file here? (x3)", "three, no id");
  kosyncWindowProblems(0, NULL, 2, 0, l, sizeof(l));
  eqStr(l, "! Wrong user/password from the reader (x2)", "wrong password");
  kosyncWindowProblems(1, "abc", 1, 0, l, sizeof(l));
  ok(strstr(l, "DIFFERENT book") && strstr(l, "Wrong user/password"), "both at once");
  char tiny[10];
  kosyncWindowProblems(1, "abc", 1, 0, tiny, sizeof(tiny));
  eqInt((long long)strlen(tiny), 9, "cut to the buffer, never past it");
  kosyncWindowProblems(0, "", 0, 1, l, sizeof(l));
  eqStr(l, "! A reader that can't take a percentage asked (x1) - told 'No progress found' (KOReader?)",
        "5.1: a reader told {} instead of the record - the reader itself only says 'No progress found'");
  kosyncWindowProblems(1, "abc", 0, 2, l, sizeof(l));
  ok(strstr(l, "DIFFERENT book") && strstr(l, "(x2) - told"), "with another warning, after it");
  kosyncWindowProblems(1, "abc", 1, 1, l, sizeof(l));
  ok(strstr(l, "DIFFERENT book") && strstr(l, "Wrong user/password") && strstr(l, "percentage"),
     "all three at once");

  group("#7: settings files beside the books are not books");
  ok(kosyncNotABook("kosync.txt"), "kosync.txt");
  ok(kosyncNotABook("KOSync.TXT"), "any case (FAT)");
  ok(kosyncNotABook("smsmirror.txt"), "smsmirror.txt");
  ok(kosyncNotABook("gemini.txt") && kosyncNotABook("Gemini.txt"),
     "gemini.txt, any case (the AI's key - the T-Deck keeps it at the card's root)");
  ok(!kosyncNotABook("my-notes.txt"), "a real .txt book");
  ok(!kosyncNotABook("kosync.txt.epub") && !kosyncNotABook("my kosync.txt"), "only the exact names");
  ok(!kosyncNotABook(NULL) && !kosyncNotABook(""), "nothing");
}

// ---------------------------------------------------------------- an EPUB built in memory
/* A stored (uncompressed) zip written here, so a book with 520 manifest items or itemrefs can
 * be opened without a 520-entry fixture on disk. The parser reads the central directory and
 * the local headers and never checks a CRC, so none is written. */
struct MemBook {
  unsigned char* buf;
  size_t len, cap;
};

static void mbPut(MemBook* m, const void* p, size_t n) {
  if (m->len + n > m->cap) {
    m->cap = (m->len + n) * 2 + 4096;
    m->buf = (unsigned char*)realloc(m->buf, m->cap);
  }
  memcpy(m->buf + m->len, p, n);
  m->len += n;
}

static void mbLe(MemBook* m, uint32_t v, int bytes) {
  unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                         (unsigned char)(v >> 24) };
  mbPut(m, b, (size_t)bytes);
}

static size_t memRead(void* ctx, uint64_t off, void* buf, size_t len) {
  const MemBook* m = (const MemBook*)ctx;
  if (off >= m->len) {
    return 0;
  }
  const size_t n = (m->len - off < len) ? (size_t)(m->len - off) : len;
  memcpy(buf, m->buf + off, n);
  return n;
}

// names[i]/datas[i] -> a zip in `z` (the caller frees z->buf).
static void buildZip(MemBook* z, int n, const char* const* names, const char* const* datas) {
  memset(z, 0, sizeof(*z));
  uint32_t* offs = (uint32_t*)calloc((size_t)n, sizeof(uint32_t));
  for (int i = 0; i < n; i++) {
    offs[i] = (uint32_t)z->len;
    const size_t nl = strlen(names[i]), dl = strlen(datas[i]);
    mbLe(z, 0x04034b50u, 4); mbLe(z, 20, 2); mbLe(z, 0, 2); mbLe(z, 0, 2);   // sig, ver, flags, stored
    mbLe(z, 0, 2); mbLe(z, 0, 2); mbLe(z, 0, 4);                              // time, date, crc
    mbLe(z, (uint32_t)dl, 4); mbLe(z, (uint32_t)dl, 4);
    mbLe(z, (uint32_t)nl, 2); mbLe(z, 0, 2);
    mbPut(z, names[i], nl);
    mbPut(z, datas[i], dl);
  }
  const uint32_t cd = (uint32_t)z->len;
  for (int i = 0; i < n; i++) {
    const size_t nl = strlen(names[i]), dl = strlen(datas[i]);
    mbLe(z, 0x02014b50u, 4); mbLe(z, 20, 2); mbLe(z, 20, 2); mbLe(z, 0, 2); mbLe(z, 0, 2);
    mbLe(z, 0, 2); mbLe(z, 0, 2); mbLe(z, 0, 4);
    mbLe(z, (uint32_t)dl, 4); mbLe(z, (uint32_t)dl, 4);
    mbLe(z, (uint32_t)nl, 2); mbLe(z, 0, 2); mbLe(z, 0, 2); mbLe(z, 0, 2); mbLe(z, 0, 2);
    mbLe(z, 0, 4); mbLe(z, offs[i], 4);
    mbPut(z, names[i], nl);
  }
  const uint32_t cdLen = (uint32_t)z->len - cd;
  mbLe(z, 0x06054b50u, 4); mbLe(z, 0, 2); mbLe(z, 0, 2);
  mbLe(z, (uint32_t)n, 2); mbLe(z, (uint32_t)n, 2); mbLe(z, cdLen, 4); mbLe(z, cd, 4); mbLe(z, 0, 2);
  free(offs);
}

/* An OPF with `nItems` manifest items (the two chapters at `chapAt` and `chapAt + 1`, images
 * elsewhere) and `nRefs` itemrefs alternating over the two chapters. */
static void buildCapBook(MemBook* z, int nItems, int chapAt, int nRefs) {
  static char opf[160 * 1024];
  size_t o = 0;
  o += (size_t)snprintf(opf + o, sizeof(opf) - o,
                        "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\">"
                        "<metadata><dc:title>Caps</dc:title></metadata><manifest>");
  for (int i = 0; i < nItems; i++) {
    if (i == chapAt || i == chapAt + 1) {
      o += (size_t)snprintf(opf + o, sizeof(opf) - o,
                            "<item id=\"c%d\" href=\"c%d.xhtml\" media-type=\"application/xhtml+xml\"/>",
                            i - chapAt, i - chapAt);
    } else {
      o += (size_t)snprintf(opf + o, sizeof(opf) - o,
                            "<item id=\"i%d\" href=\"i%d.png\" media-type=\"image/png\"/>", i, i);
    }
  }
  o += (size_t)snprintf(opf + o, sizeof(opf) - o, "</manifest><spine>");
  for (int r = 0; r < nRefs; r++) {
    o += (size_t)snprintf(opf + o, sizeof(opf) - o, "<itemref idref=\"c%d\"/>", r % 2);
  }
  snprintf(opf + o, sizeof(opf) - o, "</spine></package>");
  const char* names[] = { "mimetype", "META-INF/container.xml", "OEBPS/content.opf",
                          "OEBPS/c0.xhtml", "OEBPS/c1.xhtml" };
  const char* datas[] = {
      "application/epub+zip",
      "<?xml version=\"1.0\"?><container version=\"1.0\" "
      "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
      "full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
      "</rootfiles></container>",
      opf,
      "<html><body><p>Chapter one has some words in it.</p></body></html>",
      "<html><body><p>Chapter two has a few more words in it than one.</p></body></html>" };
  buildZip(z, 5, names, datas);
}

static bool openMem(MemBook* z, EpubBook* b, EpubSource* s, const char* name, bool txt) {
  s->ctx = z;
  s->size = z->len;
  s->read = memRead;
  return epubOpen(b, s, name, txt) == EPUB_OK;
}

static void testCapsAndBigText() {
  group("#12: past 512 items or itemrefs the book says it cannot sync");
  MemBook z;
  EpubBook b;
  EpubSource s;
  buildCapBook(&z, 20, 0, 2);
  ok(openMem(&z, &b, &s, "small.epub", false), "a small built book opens");
  eqInt(b.nSpine, 2, "two chapters");
  ok(epubKosyncTotal(b.kosync) > 0, "syncable");
  eqStr(epubKosyncWhyNot(b.kosync), "", "nothing to say");
  epubClose(&b);
  free(z.buf);

  buildCapBook(&z, 520, 0, 2);
  ok(openMem(&z, &b, &s, "manifest-big.epub", false), "520 manifest items, the chapters FIRST");
  eqInt(b.nSpine, 2, "reads as before");
  ok(epubKosyncTotal(b.kosync) > 0,
     "every itemref found its item before the cut: still syncable (not over-refused)");
  epubClose(&b);
  free(z.buf);

  buildCapBook(&z, 520, 518, 2);
  ok(openMem(&z, &b, &s, "manifest-cut.epub", false), "520 items, the chapters PAST the cut");
  eqInt(epubKosyncTotal(b.kosync), 0, "an itemref naming an item past the cut: NOT syncable");
  eqInt(b.kosync ? b.kosync->refused : -1, EPUB_KOSYNC_TOO_MANY_ITEMS, "...and why");
  eqStr(epubKosyncWhyNot(b.kosync), "over 512 items - the X4 would count it differently", "said");
  double p = -1;
  ok(!epubKosyncPercentAt(&b, 0, 0, 10, &p), "no percentage is sent");
  int r = -1;
  double w = -1;
  ok(!epubKosyncLocate(b.kosync, 0.5, &r, &w, NULL, NULL), "none is taken either");
  epubClose(&b);
  free(z.buf);

  buildCapBook(&z, 4, 0, 520);
  ok(openMem(&z, &b, &s, "spine-big.epub", false), "520 itemrefs");
  eqInt(b.nSpine, EPUB_MAX_SPINE, "reads what it always read (the reading cap)");
  eqInt(epubKosyncTotal(b.kosync), 0, "the X4 counts 520: NOT syncable");
  eqInt(b.kosync ? b.kosync->refused : -1, EPUB_KOSYNC_TOO_MANY_ITEMS, "...and why");
  epubClose(&b);
  free(z.buf);

  buildCapBook(&z, 4, 0, EPUB_MAX_SPINE);
  ok(openMem(&z, &b, &s, "spine-512.epub", false), "exactly 512 itemrefs");
  ok(epubKosyncTotal(b.kosync) > 0, "at the cap, not past it: syncable");
  epubClose(&b);
  free(z.buf);

  group("F6: a .txt the reader shows CUT is not syncable");
  MemBook t;
  memset(&t, 0, sizeof(t));
  t.cap = t.len = EPUB_MAX_DOC - 1;
  t.buf = (unsigned char*)malloc(t.cap);
  memset(t.buf, 'a', t.len);
  ok(openMem(&t, &b, &s, "whole.txt", true), "a .txt of EPUB_MAX_DOC-1 bytes");
  ok(epubKosyncTotal(b.kosync) == EPUB_MAX_DOC - 1, "shown whole: syncable");
  ok(epubKosyncPercentAt(&b, 0, 1000, 2000, &p) && p == 0.5, "and its percentage is real");
  epubClose(&b);
  t.len = EPUB_MAX_DOC;                              // one byte more than the reader shows
  t.buf = (unsigned char*)realloc(t.buf, t.len);
  t.buf[t.len - 1] = 'a';
  ok(openMem(&t, &b, &s, "big.txt", true), "a .txt of EPUB_MAX_DOC bytes still OPENS");
  eqInt(epubKosyncTotal(b.kosync), 0, "...but is not syncable (no more 0 % at every page)");
  eqInt(b.kosync ? b.kosync->refused : -1, EPUB_KOSYNC_TEXT_TOO_BIG, "...and why");
  eqStr(epubKosyncWhyNot(b.kosync), "a .txt too big to show whole here", "said");
  ok(!epubKosyncPercentAt(&b, 0, 1000, 2000, &p), "nothing is sent");
  ok(!epubKosyncLocate(b.kosync, 0.8, &r, &w, NULL, NULL), "and 80 % is not applied to cut text");
  epubClose(&b);
  free(t.buf);
  ok(!strcmp(epubKosyncWhyNot(NULL), "no KOSync data (memory)"), "no map at all is said too");
}

// ================================================================ 3. NOBODY'S PLACE MOVES
static void testGolden() {
  group("before/after: every fixture reads and locates exactly as 0.9.78 did");
  for (size_t k = 0; k < sizeof(GP_BOOKS) / sizeof(GP_BOOKS[0]); k++) {
    const GpBook& g = GP_BOOKS[k];
    EpubSource s;
    FILE* f = NULL;
    if (!openSource(g.file, &s, &f)) {
      ok(false, g.file);
      continue;
    }
    EpubBook b;
    const EpubStatus st = epubOpen(&b, &s, g.file, isTxt(g.file));
    eqInt(st, g.status, g.file);
    eqInt(b.nSpine, g.nSpine, "reading spine length");
    for (int i = 0; i < g.nSpine && i < b.nSpine; i++) {
      eqStr(b.spine[i].name, g.names[i], "reading spine entry");
      eqInt(epubChapterLen(&b, i), g.chapLen[i], "chapter length");
    }
    char ids[3][EPUB_ID_MAX];
    const int nIds = epubIds(&b, ids);
    eqInt(nIds, g.nIds, "id count");
    for (int i = 0; i < nIds && i < g.nIds; i++) {
      eqStr(ids[i], g.ids[i], "id");
    }
    for (int i = 0; i < g.nFr; i++) {
      char w[80];
      snprintf(w, sizeof(w), "epubFraction(%d, %u)", g.fr[i].spine, (unsigned)g.fr[i].offset);
      eqDbl(epubFraction(&b, g.fr[i].spine, g.fr[i].offset), g.fr[i].fraction, 0.0, w);
    }
    for (int i = 0; i < g.nLoc; i++) {
      int sp = -1;
      uint32_t off = 0;
      epubLocate(&b, g.loc[i].fraction, &sp, &off);
      char w[80];
      snprintf(w, sizeof(w), "epubLocate(%.6f)", g.loc[i].fraction);
      eqInt(sp, g.loc[i].spine, w);
      eqInt(off, g.loc[i].offset, w);
    }
    if (st == EPUB_OK) {
      epubClose(&b);
    }
    fclose(f);
  }

  group("before/after: a 0.9.78 positions file loads, lands and re-saves unchanged");
  static BookStore store;
  store.init();
  ok(store.parse(GP_STORE_BLOB, strlen(GP_STORE_BLOB)), "parses");
  for (size_t k = 0; k < sizeof(GP_BOOKS) / sizeof(GP_BOOKS[0]); k++) {
    const GpBook& g = GP_BOOKS[k];
    if (g.nIds <= 0) {
      continue;
    }
    // Look it up the way openBook() does: by the ids the CURRENT parser derives.
    EpubSource s;
    FILE* f = NULL;
    EpubBook b;
    if (!openBook(g.file, &b, &f, &s)) {
      ok(false, g.file);
      continue;
    }
    char ids[3][EPUB_ID_MAX];
    const int nIds = epubIds(&b, ids);
    const char* idp[3] = { ids[0], ids[1], ids[2] };
    BookPos pos;
    char w[96];
    snprintf(w, sizeof(w), "%s: its saved place is found", g.file);
    ok(store.get(idp, nIds, &pos), w);
    eqInt(pos.spine, g.savedSpine, "saved spine");
    eqInt(pos.offset, g.savedOffset, "saved offset");
    char six[32];
    snprintf(six, sizeof(six), "%.6f", g.savedFraction);     // the store keeps 6 decimals
    eqDbl(pos.fraction, strtod(six, NULL), 0.0, "saved fraction");
    // openBook() keeps the place only if the spine is in range: it must still be.
    ok((int)pos.spine < b.nSpine, "the saved chapter still exists");
    // ...and loadChapter() + gotoOffset() land on the same text: same chapter length.
    eqInt(epubChapterLen(&b, (int)pos.spine), g.chapLen[pos.spine], "the same chapter text");
    epubClose(&b);
    fclose(f);
  }
  static char again[BOOKSTORE_BLOB_MAX];
  const size_t n = store.serialise(again, sizeof(again));
  ok(n == strlen(GP_STORE_BLOB) && !memcmp(again, GP_STORE_BLOB, n), "re-saved byte for byte");
}

// ---------------------------------------------------------------- home= by name
static uint32_t ip4(int a, int b, int c, int d) {
  const uint8_t x[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
  uint32_t v;
  memcpy(&v, x, 4);
  return v;
}

// An avahi-style legacy unicast answer: our id, QR|AA, the question echoed, then one A record
// whose name is a pointer back at the question.
static const uint8_t AVAHI_ANSWER[] = {
  0x12, 0x34, 0x84, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x05, 'c', 'o', 'v', 'e', 'y', 0x05, 'l', 'o', 'c', 'a', 'l', 0x00, 0x00, 0x01, 0x00, 0x01,
  0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x04, 192, 168, 1, 55,
};

static void testHomeByName() {
  group("home= by name: which names are asked, and how");
  uint32_t ip = 0;
  ok(kosyncParseIp("192.168.1.55", &ip) && ip == ip4(192, 168, 1, 55), "a dotted quad, wire order");
  ok(!kosyncParseIp("192.168.1", NULL) && !kosyncParseIp("192.168.1.256", NULL) &&
     !kosyncParseIp("1.2.3.4.5", NULL) && !kosyncParseIp("1.2.3.4 ", NULL) &&
     !kosyncParseIp("covey.local", NULL) && !kosyncParseIp("", NULL) && !kosyncParseIp(NULL, NULL) &&
     !kosyncParseIp("0001.2.3.4", NULL), "not an address");
  ok(kosyncHostIsMdns("covey") && kosyncHostIsMdns("covey.local") && kosyncHostIsMdns("COVEY.LOCAL") &&
     kosyncHostIsMdns("covey.local.") && kosyncHostIsMdns("my-pi.lab.local"), "mDNS names");
  ok(!kosyncHostIsMdns("kosync.example.com") && !kosyncHostIsMdns("192.168.1.55") &&
     !kosyncHostIsMdns("") && !kosyncHostIsMdns(NULL) && !kosyncHostIsMdns(".local") &&
     !kosyncHostIsMdns("covey..local") && !kosyncHostIsMdns("covey.local..") &&
     !kosyncHostIsMdns("covey.localx"), "not mDNS names (DNS, an IP, broken)");

  uint8_t q[300];
  static const uint8_t WANT_Q[] = {
    0x12, 0x34, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, 'c', 'o', 'v', 'e', 'y', 0x05, 'l', 'o', 'c', 'a', 'l', 0x00, 0x00, 0x01, 0x00, 0x01,
  };
  size_t n = kosyncMdnsQuery("covey", 0x1234, q, sizeof(q));
  ok(n == sizeof(WANT_Q) && !memcmp(q, WANT_Q, n), "'covey' asks for covey.local, A, IN, no QU bit");
  n = kosyncMdnsQuery("covey.local", 0x1234, q, sizeof(q));
  ok(n == sizeof(WANT_Q) && !memcmp(q, WANT_Q, n),
     "'covey.local' is the SAME query — two labels, never the one label 'covey.local'");
  ok(kosyncMdnsQuery("covey.local", 1, q, sizeof(WANT_Q) - 1) == 0, "no room: nothing");
  ok(kosyncMdnsQuery("kosync.example.com", 1, q, sizeof(q)) == 0, "not an mDNS name: nothing");
  char big[80];
  memset(big, 'a', 64);
  big[64] = '\0';
  ok(kosyncMdnsQuery(big, 1, q, sizeof(q)) == 0, "a 64-byte label: nothing");

  group("home= by name: reading the answer");
  eqInt(kosyncMdnsAnswer(AVAHI_ANSWER, sizeof(AVAHI_ANSWER), "covey.local", 0x1234),
        ip4(192, 168, 1, 55), "avahi's legacy unicast answer (compressed name)");
  eqInt(kosyncMdnsAnswer(AVAHI_ANSWER, sizeof(AVAHI_ANSWER), "Covey", 0x1234), ip4(192, 168, 1, 55),
        "asked as the bare label, in another case");
  eqInt(kosyncMdnsAnswer(AVAHI_ANSWER, sizeof(AVAHI_ANSWER), "covey.local", 0x1235), 0,
        "another query's id: not ours");
  eqInt(kosyncMdnsAnswer(AVAHI_ANSWER, sizeof(AVAHI_ANSWER), "other.local", 0x1234), 0,
        "another host's address: not ours");
  uint8_t pkt[sizeof(AVAHI_ANSWER) + 64];
  memcpy(pkt, AVAHI_ANSWER, sizeof(AVAHI_ANSWER));
  pkt[2] = 0x00;
  eqInt(kosyncMdnsAnswer(pkt, sizeof(AVAHI_ANSWER), "covey.local", 0x1234), 0,
        "our own query looped back (QR clear) is not an answer");
  memcpy(pkt, AVAHI_ANSWER, sizeof(AVAHI_ANSWER));
  pkt[33] = 0x80;                             // class 0x8001: the cache-flush bit
  eqInt(kosyncMdnsAnswer(pkt, sizeof(AVAHI_ANSWER), "covey.local", 0x1234), ip4(192, 168, 1, 55),
        "the cache-flush bit is ignored");
  memcpy(pkt, AVAHI_ANSWER, sizeof(AVAHI_ANSWER));
  pkt[35] = pkt[36] = pkt[37] = pkt[38] = 0;  // TTL 0: a goodbye
  eqInt(kosyncMdnsAnswer(pkt, sizeof(AVAHI_ANSWER), "covey.local", 0x1234), 0,
        "a goodbye (TTL 0) withdraws the address, it does not give one");
  memcpy(pkt, AVAHI_ANSWER, sizeof(AVAHI_ANSWER));
  pkt[32] = 0x1C;                             // type 28: AAAA
  eqInt(kosyncMdnsAnswer(pkt, sizeof(AVAHI_ANSWER), "covey.local", 0x1234), 0, "an AAAA only: none");
  // In the ADDITIONAL section (AN 0, AR 1), the name written out in full, upper case.
  static const uint8_t ADDITIONAL[] = {
    0x00, 0x07, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x05, 'C', 'O', 'V', 'E', 'Y', 0x05, 'L', 'O', 'C', 'A', 'L', 0x00,
    0x00, 0x01, 0x80, 0x01, 0x00, 0x00, 0x00, 0x78, 0x00, 0x04, 10, 0, 0, 7,
  };
  eqInt(kosyncMdnsAnswer(ADDITIONAL, sizeof(ADDITIONAL), "covey.local", 7), ip4(10, 0, 0, 7),
        "an A record in the additional section, no question, full upper-case name");
  // Two records: another host's A first, then ours.
  static const uint8_t TWO[] = {
    0x00, 0x09, 0x84, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x03, 'n', 'a', 's', 0x05, 'l', 'o', 'c', 'a', 'l', 0x00,
    0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x04, 192, 168, 1, 9,
    0x05, 'c', 'o', 'v', 'e', 'y', 0xC0, 0x10,  // "covey" + a pointer to "local"
    0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x04, 192, 168, 1, 55,
  };
  eqInt(kosyncMdnsAnswer(TWO, sizeof(TWO), "covey", 9), ip4(192, 168, 1, 55),
        "ours after another's, with a mid-name pointer");
  eqInt(kosyncMdnsAnswer(TWO, sizeof(TWO), "nas.local", 9), ip4(192, 168, 1, 9), "and the other");
  // A record whose name is far too long to be ours comes first: it is skipped, not the end.
  {
    uint8_t lp[400];
    size_t k = 0;
    const uint8_t hdr[12] = { 0x00, 0x05, 0x84, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00 };
    memcpy(lp, hdr, 12);
    k = 12;
    for (int i = 0; i < 3; i++) {             // 3 labels of 60: a 180-character name
      lp[k++] = 60;
      memset(lp + k, 'x', 60);
      k += 60;
    }
    lp[k++] = 0;
    const uint8_t rr1[14] = { 0x00, 0x01, 0x00, 0x01, 0, 0, 0, 10, 0x00, 0x04, 9, 9, 9, 9 };
    memcpy(lp + k, rr1, 14);
    k += 14;
    const uint8_t rr2[] = { 0x05, 'c', 'o', 'v', 'e', 'y', 0x05, 'l', 'o', 'c', 'a', 'l', 0x00,
                            0x00, 0x01, 0x00, 0x01, 0, 0, 0, 10, 0x00, 0x04, 192, 168, 1, 55 };
    memcpy(lp + k, rr2, sizeof(rr2));
    k += sizeof(rr2);
    eqInt(kosyncMdnsAnswer(lp, k, "covey.local", 5), ip4(192, 168, 1, 55),
          "a 180-character name first: skipped, and ours after it still found");
  }
  // A pointer that points at itself must end, not spin.
  static const uint8_t LOOP[] = {
    0x00, 0x01, 0x84, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x04, 1, 2, 3, 4,
  };
  eqInt(kosyncMdnsAnswer(LOOP, sizeof(LOOP), "covey.local", 1), 0, "a pointer loop: none, and it ends");
  // Every truncation of a good answer: never an address from half a record, never a read past
  // the end (ASan watches this: each copy is a heap block of exactly that length).
  bool allClean = true;
  for (size_t cut = 0; cut < sizeof(AVAHI_ANSWER); cut++) {
    uint8_t* h = (uint8_t*)malloc(cut ? cut : 1);
    memcpy(h, AVAHI_ANSWER, cut);
    allClean = allClean && kosyncMdnsAnswer(h, cut, "covey.local", 0x1234) == 0;
    free(h);
  }
  ok(allClean, "every truncated answer: none (and no over-read)");
  memcpy(pkt, AVAHI_ANSWER, sizeof(AVAHI_ANSWER));
  pkt[39] = 0;
  pkt[40] = 60;                               // rdlength 60: past the end
  eqInt(kosyncMdnsAnswer(pkt, sizeof(AVAHI_ANSWER), "covey.local", 0x1234), 0, "rdlength past the end");

  group("home= by name: once per WiFi join, and what it falls back to");
  KosyncHomeAddr a;
  memset(&a, 0, sizeof(a));
  const uint32_t C55 = ip4(192, 168, 1, 55), C77 = ip4(192, 168, 1, 77), HOT = ip4(172, 20, 10, 3);
  bool fb = false, ch = false;
  eqInt(kosyncHomePlan(&a, "192.168.1.55", "SmithWifi", 1, &ip), KOSYNC_HOME_USE, "an IP: used as it is");
  eqInt(ip, C55, "...that address");
  eqInt(kosyncHomePlan(&a, "covey.local", "SmithWifi", 1, &ip), KOSYNC_HOME_LOOKUP,
        "a name, nothing known: look it up");
  eqInt(kosyncHomeLooked(&a, "covey.local", "SmithWifi", 1, C55, &fb, &ch), C55, "answered: used");
  ok(!fb && ch, "not a fallback; the saved copy must be written");
  eqInt(kosyncHomePlan(&a, "covey.local", "SmithWifi", 1, &ip), KOSYNC_HOME_USE,
        "the next job on the SAME join: no lookup");
  eqInt(ip, C55, "...the answer");
  eqInt(kosyncHomePlan(&a, "covey.local", "SmithWifi", 2, &ip), KOSYNC_HOME_LOOKUP,
        "a NEW join (a blip, a reboot of the router): look again");
  eqInt(kosyncHomeLooked(&a, "covey.local", "SmithWifi", 2, C55, &fb, &ch), C55, "same answer");
  ok(!ch, "same answer: nothing to write");
  eqInt(kosyncHomePlan(&a, "COVEY.local", "SmithWifi", 2, &ip), KOSYNC_HOME_USE,
        "home= in another case is the same name");
  eqInt(kosyncHomePlan(&a, "covey.local", "{user}-wifi", 2, &ip), KOSYNC_HOME_LOOKUP,
        "another SSID: look it up there");
  eqInt(kosyncHomeLooked(&a, "covey.local", "{user}-wifi", 2, 0, &fb, &ch), 0,
        "...no answer there: NO fallback to the other network's address");
  ok(!fb, "not a fallback");
  eqInt(kosyncHomePlan(&a, "nas.local", "SmithWifi", 2, &ip), KOSYNC_HOME_LOOKUP,
        "home= changed: look it up");
  eqInt(kosyncHomeLooked(&a, "nas.local", "SmithWifi", 2, 0, &fb, &ch), 0,
        "...and a new name never falls back to the old name's address");

  group("home= by name: the fallback, and a job that gives up");
  eqInt(kosyncHomePlan(&a, "covey.local", "SmithWifi", 3, &ip), KOSYNC_HOME_LOOKUP, "join 3: look");
  eqInt(kosyncHomeLooked(&a, "covey.local", "SmithWifi", 3, 0, &fb, &ch), C55,
        "nobody answered (a lost multicast, avahi renamed to covey-2): the LAST address");
  ok(fb && !ch, "said to be a fallback; nothing to write");
  eqInt(kosyncHomePlan(&a, "covey.local", "SmithWifi", 3, &ip), KOSYNC_HOME_LOOKUP,
        "an unconfirmed fallback is not trusted for the next job");
  kosyncHomeReached(&a, "covey.local", "SmithWifi", 3, C55);
  eqInt(kosyncHomePlan(&a, "covey.local", "SmithWifi", 3, &ip), KOSYNC_HOME_USE,
        "...until it gets an HTTP answer: then no lookup for the rest of the join");
  kosyncHomeGaveUp(&a, C77);
  eqInt(kosyncHomePlan(&a, "covey.local", "SmithWifi", 3, &ip), KOSYNC_HOME_USE,
        "giving up on ANOTHER address changes nothing");
  kosyncHomeGaveUp(&a, C55);
  eqInt(kosyncHomePlan(&a, "covey.local", "SmithWifi", 3, &ip), KOSYNC_HOME_LOOKUP,
        "a job gave up on it: the next job looks again, same join or not");
  eqInt(kosyncHomeLooked(&a, "covey.local", "SmithWifi", 3, C77, &fb, &ch), C77,
        "COVEY moved (a lease ran out): the new address");
  ok(!fb && ch, "and it is saved");
  eqInt(kosyncHomePlan(&a, "covey.local", "SmithWifi", 3, &ip), KOSYNC_HOME_USE, "trusted again");
  eqInt(ip, C77, "...the new one");
  eqInt(kosyncHomeLooked(&a, "covey.local", "{user}-wifi", 4, HOT, &fb, &ch), HOT,
        "found on the hotspot: that network's address replaces it");
  eqInt(kosyncHomePlan(&a, "covey.local", "SmithWifi", 5, &ip), KOSYNC_HOME_LOOKUP, "home again: look");
  eqInt(kosyncHomeLooked(&a, "covey.local", "SmithWifi", 5, 0, &fb, &ch), 0,
        "...and one network's address is never tried on another");
  kosyncHomeReached(&a, "covey.local", "SmithWifi", 5, HOT);
  eqInt(kosyncHomePlan(&a, "covey.local", "SmithWifi", 5, &ip), KOSYNC_HOME_LOOKUP,
        "an answer from an address for another network confirms nothing");
  eqInt(kosyncHomePlan(&a, "covey.local", "{user}-wifi", 0, &ip), KOSYNC_HOME_LOOKUP,
        "association 0 (none counted yet) never matches");

  group("home= by name: kept across a restart");
  uint8_t blob[KOSYNC_HOME_BLOB_BYTES + 8];
  KosyncHomeAddr b;
  memset(&a, 0, sizeof(a));
  eqInt((long long)kosyncHomePack(&a, blob, sizeof(blob)), 0, "nothing known: nothing written");
  kosyncHomeLooked(&a, "covey.local", "SmithWifi", 9, C55, &fb, &ch);
  eqInt((long long)kosyncHomePack(&a, blob, sizeof(blob)), KOSYNC_HOME_BLOB_BYTES, "packed");
  ok(kosyncHomeUnpack(&b, blob, KOSYNC_HOME_BLOB_BYTES), "read back");
  ok(!strcmp(b.host, "covey.local") && !strcmp(b.ssid, "SmithWifi") && b.ip == C55, "the same");
  eqInt(kosyncHomePlan(&b, "covey.local", "SmithWifi", 1, &ip), KOSYNC_HOME_LOOKUP,
        "after a restart it is only a FALLBACK: the first job looks the name up");
  eqInt(kosyncHomeLooked(&b, "covey.local", "SmithWifi", 1, 0, &fb, &ch), C55,
        "...and, unanswered, falls back to it");
  ok(!kosyncHomeUnpack(&b, blob, KOSYNC_HOME_BLOB_BYTES - 1) && !b.ip, "a short blob: empty");
  blob[0] = 'X';
  ok(!kosyncHomeUnpack(&b, blob, KOSYNC_HOME_BLOB_BYTES) && !b.host[0], "another magic: empty");
  blob[0] = 'K';
  memset(blob + 4, 'z', KOSYNC_HOST_MAX);     // no NUL in the host field
  ok(!kosyncHomeUnpack(&b, blob, KOSYNC_HOME_BLOB_BYTES), "an unterminated field: empty");
}

static void testProblemsKept() {
  group("the window's warnings: a re-read clears them only when asked for or the file changed");
  KosyncConfig c1, c2;
  const char* f1 = "user={user}\npassword=correct horse battery\nhome=covey.local:8088\nauto=on\n";
  const char* f1b = "# edited, same meaning\r\n\r\nuser = {user}\r\npassword=correct horse battery\r\n"
                    "home=http://covey.local:8088/\r\nauto=yes\r\n";
  const char* f2 = "user={user}\npassword=correct horse staple\nhome=covey.local:8088\nauto=on\n";
  const char* f3 = "user={user}\npassword=correct horse battery\nhome=192.168.1.55:8088\nauto=on\n";
  kosyncParseConfig(f1, strlen(f1), &c1);
  kosyncParseConfig(f1b, strlen(f1b), &c2);
  eqInt(kosyncConfigSig(&c1), kosyncConfigSig(&c2), "comments, spacing and spelling: the same config");
  kosyncParseConfig(f2, strlen(f2), &c2);
  ok(kosyncConfigSig(&c1) != kosyncConfigSig(&c2), "a new password: another config");
  kosyncParseConfig(f3, strlen(f3), &c2);
  ok(kosyncConfigSig(&c1) != kosyncConfigSig(&c2), "a new home=: another config");
  const char* f4 = "user={user}\npassword=correct horse battery\nhome=covey.local:8088\nauto=off\n";
  KosyncConfig c4;
  kosyncParseConfig(f4, strlen(f4), &c4);
  ok(kosyncConfigSig(&c1) != kosyncConfigSig(&c4), "a switch turned off: another config");
  const KosyncConfig none = KosyncConfig();
  ok(kosyncConfigSig(&c1) != kosyncConfigSig(&none), "the file taken away (nothing read): another config");
  const uint32_t s1 = kosyncConfigSig(&c1), s2 = kosyncConfigSig(&c2);
  ok(!kosyncReloadClears(false, true, s1, s1),
     "the re-read on every Books entry / Sync settings visit, file unchanged: KEPT");
  ok(kosyncReloadClears(false, true, s1, s2), "the file changed since the last read: cleared");
  ok(kosyncReloadClears(true, true, s1, s1), "serial `kosync reload`: cleared");
  ok(!kosyncReloadClears(false, false, 0, s1), "the first read of the boot: nothing to clear");
}

// ================================================================ integration review (KS-1..3)
/* KS-1: an AUTOMATIC ask is judged against the last move AS OF THE ASK. {user}'s Sunday flow:
 * read on the X4, then open the book on the phone — and a page turned in the seconds the pull
 * on open takes must not hide the X4's newer place. */
static void testOfferRace() {
  group("KS-1: a page turn while the pull on open is on its way does not hide the X4's place");
  static KosyncMemo m;
  memset(&m, 0, sizeof(m));
  const uint32_t H13 = 1790046800u, H14 = H13 + 3600u, H15 = H13 + 7200u;
  KosyncMemoEntry* e = kosyncMemoGet(&m, BYNAME, true);
  kosyncMemoMoved(&m, e, H13);                     // the phone last read this book at 13:00
  KosyncProgress g;                                // 14:00: the X4 sends 70 % to COVEY
  memset(&g, 0, sizeof(g));
  g.hasPct = true; g.pct = 0.70; g.hasTimestamp = true; g.timestamp = H14;
  snprintf(g.device, sizeof(g.device), "CrossPoint");
  snprintf(g.deviceId, sizeof(g.deviceId), "x4");
  // 15:00: the book opens at 40 %; the pull is queued with the snapshot (kosyncMovedAt, now).
  const uint32_t asked = e->movedAt;
  kosyncMemoMoved(&m, e, H15 + 1);                 // ...and a page is turned before it answers
  KosyncOfferIn in;
  kosyncOfferFill(&in, &g, false, true, 0.401, asked, e, 0, "WiPhone-{user}", "me");
  eqInt(in.myMovedAt, H13, "the automatic ask is judged by the stamp as of the ask");
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "the X4's 70 % is OFFERED despite the turn");
  in.myMovedAt = e->movedAt;                       // what the glue used to read
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_OLDER,
        "(the freshest stamp would have hidden it: 'older than your last page turn')");
  kosyncOfferFill(&in, &g, true, true, 0.401, asked, e, 0, "WiPhone-{user}", "me");
  eqInt(in.myMovedAt, H15 + 1, "the explicit Sync my place is judged by the freshest stamp");
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_OLDER, "...the person just said 'this is where I am'");

  group("KS-1: the rule itself still holds for an automatic ask");
  memset(&m, 0, sizeof(m));
  e = kosyncMemoGet(&m, BYNAME, true);
  kosyncMemoMoved(&m, e, H15);                     // the phone really did read after the X4
  kosyncOfferFill(&in, &g, false, true, 0.45, e->movedAt, e, 0, "WiPhone-{user}", "me");
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_OLDER, "a move made BEFORE the ask still wins");
  kosyncOfferFill(&in, &g, false, true, 0.45, 0, e, 0, "WiPhone-{user}", "me");
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "no stamp at the ask (no clock then): offered");

  group("KS-1: the book opened before the WiFi came up");
  memset(&m, 0, sizeof(m));
  e = kosyncMemoGet(&m, BYNAME, true);
  kosyncMemoMoved(&m, e, H13);
  const uint32_t atOpen = e->movedAt;              // s_waitBook's snapshot, never refreshed now
  for (int t = 1; t <= 20; t++) {
    kosyncMemoMoved(&m, e, H15 + 30u * (uint32_t)t);   // a GPS-set clock stamps every page
  }
  kosyncOfferFill(&in, &g, false, true, 0.43, atOpen, e, 0, "WiPhone-{user}", "me");
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "the pull once WiFi is up still offers the X4's place");

  group("KS-1: kosyncOfferFill carries everything else as it was");
  static KosyncMemo m2;
  memset(&m2, 0, sizeof(m2));
  KosyncMemoEntry* e2 = kosyncMemoGet(&m2, BYNAME, true);
  e2->offeredSig = 1234; e2->movedAt = 99;
  g.hasTimestamp = false; g.timestamp = 777;
  kosyncOfferFill(&in, &g, false, false, 0.2, 55, e2, 4321, "dev", "id");
  ok(in.theirTs == 0 && in.theirPct == 0.70 && !strcmp(in.theirDevice, "CrossPoint") &&
     !strcmp(in.theirDeviceId, "x4"), "the record (no timestamp = 0)");
  ok(!in.mineOk && in.minePct == 0.2 && !strcmp(in.myDevice, "dev") && !strcmp(in.myDeviceId, "id"),
     "our side");
  ok(in.offeredSig == 1234 && in.pendingSig == 4321 && in.myMovedAt == 55, "the memo's answer, the pending park");
  kosyncOfferFill(&in, &g, true, true, 0.2, 55, NULL, 0, "dev", "id");
  ok(in.myMovedAt == 0 && in.offeredSig == 0, "no memo entry: nothing known");
}

/* KS-2: "declined" means the person ANSWERED the card — never that it was parked. */
static void testPendingOffer() {
  group("KS-2: a parked home record is PENDING until the card is answered");
  uint8_t key[32];
  bookSyncDeriveKey("test-passcode", key);
  bookSyncInboxInit();
  static KosyncParkLedger L;
  static KosyncMemo m;
  static uint8_t blob[KOSYNC_MEMO_BLOB_MAX];
  memset(&L, 0, sizeof(L));
  memset(&m, 0, sizeof(m));
  const char* mine[1] = { "id:urn-uuid-kosync-mixed-0001" };
  const uint32_t S = kosyncOfferSig(1790050400, 0.70, "x4", "CrossPoint");
  KosyncOfferIn in;
  memset(&in, 0, sizeof(in));
  in.theirPct = 0.70; in.theirTs = 1790050400; in.theirDevice = "CrossPoint"; in.theirDeviceId = "x4";
  in.mineOk = true; in.minePct = 0.40; in.myDevice = "WiPhone-{user}"; in.myDeviceId = "me";
  char text[BOOKSYNC_MESH_TEXT_MAX];
  ok(kosyncParkText(mine, 1, 2, 0.5, 4, 0, "CrossPoint", key, text, sizeof(text)), "pack");
  ok(kosyncParkInto(&L, BYNAME, text, 0.70, 0, S), "the pull parks the X4's place");
  uint32_t pend = 0;
  ok(kosyncParkPending(&L, BYNAME, NULL, &pend) && pend == S, "pending, with its record");
  ok(kosyncParkPending(&L, BYNAME, key, &pend) && pend == S, "...and it verifies: a card can show it");
  uint8_t otherKey[32];
  bookSyncDeriveKey("a-new-passcode", otherKey);
  ok(!kosyncParkPending(&L, BYNAME, otherKey, &pend) && pend == 0,
     "the passcode changed since: it can never be a card, so it holds nothing");
  kosyncParkPending(&L, BYNAME, key, &pend);
  ok(kosyncMemoGet(&m, BYNAME, false) == NULL, "...and NOTHING is written to the memo (it was, at park)");
  in.pendingSig = pend;
  in.offeredSig = 0;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_WAITING, "a second read of it: on the card, not re-parked");
  eqStr(kosyncOfferVerdictText(KOSYNC_SKIP_WAITING), "on the card, waiting for your answer", "said so");
  in.theirTs = 1790050500;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "a NEWER record replaces the pending one");
  in.theirTs = 1790050400;
  ok(!kosyncParkPending(&L, "ffffffffffffffffffffffffffffffff", key, &pend) && pend == 0,
     "another book: nothing pending");

  group("KS-2: a park lost unanswered is simply offered again");
  bookSyncInboxInit();                             // a restart (the inbox and the ledger are RAM)
  memset(&L, 0, sizeof(L));
  size_t len = kosyncMemoPack(&m, blob, sizeof(blob));
  ok(kosyncMemoUnpack(&m, blob, len), "the memo, as NVS kept it");
  KosyncMemoEntry* e = kosyncMemoGet(&m, BYNAME, true);
  ok(!kosyncParkPending(&L, BYNAME, key, &pend), "nothing parked after the restart");
  in.pendingSig = pend;
  in.offeredSig = e->offeredSig;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "the next pull OFFERS it (it said 'already offered')");
  // Evicted from the four-slot inbox by LoRa traffic for other books, before anyone saw it.
  ok(kosyncParkInto(&L, BYNAME, text, 0.70, 0, S), "parked again");
  const char* others[4] = { "id:o1", "id:o2", "id:o3", "id:o4" };
  for (int i = 0; i < 4; i++) {
    BookSyncRecord r;
    const char* ids[1] = { others[i] };
    bookSyncMakeRecord(&r, ids, 1, 1, 10, 0.3, 1790000000u + i, "COVEY", NULL);
    char t2[BOOKSYNC_MESH_TEXT_MAX];
    bookSyncPackMesh(&r, key, t2, sizeof(t2));
    bookSyncInboxPush(t2, 0x1234, 0);
  }
  ok(!kosyncParkPending(&L, BYNAME, key, &pend) && pend == 0, "evicted: no longer pending");
  in.pendingSig = pend;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_OFFER, "...and offered again by the next pull");

  group("KS-2: answering THAT card is what declines it");
  bookSyncInboxInit();
  memset(&L, 0, sizeof(L));
  ok(kosyncParkInto(&L, BYNAME, text, 0.70, 0, S), "parked");
  const uint32_t id = bookSyncInboxIdAt(0);
  ok(!kosyncOfferAnswered(&L, &m, id + 1000), "a LoRa card (not in the ledger): nothing");
  ok(!kosyncOfferAnswered(&L, &m, 0), "no card: nothing");
  m.dirty = false;
  ok(kosyncOfferAnswered(&L, &m, id), "Stay (or Go there) on the card");
  e = kosyncMemoGet(&m, BYNAME, false);
  ok(e && e->offeredSig == S && m.dirty, "the record is now the book's answered one, due a save");
  ok(!kosyncOfferAnswered(&L, &m, id), "answered once: a second call changes nothing");
  ok(kosyncParkPending(&L, BYNAME, key, &pend) && pend == 0,
     "until the reader retires it, the park carries no unanswered record");
  bookSyncInboxDropForBook(key, mine, 1);          // the reader retires the book's parks
  ok(!kosyncParkPending(&L, BYNAME, key, &pend), "nothing pending any more");
  in.pendingSig = pend;
  in.offeredSig = e->offeredSig;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_OFFERED, "the next pull: already answered, not offered");
  len = kosyncMemoPack(&m, blob, sizeof(blob));
  kosyncMemoUnpack(&m, blob, len);
  e = kosyncMemoGet(&m, BYNAME, false);
  in.offeredSig = e ? e->offeredSig : 0;
  eqInt(kosyncOfferVerdict(&in), KOSYNC_SKIP_OFFERED, "...and across a restart: a decline stays one");

  group("KS-2: only the card for that record answers it");
  memset(&m, 0, sizeof(m));
  bookSyncInboxInit();
  memset(&L, 0, sizeof(L));
  ok(kosyncParkInto(&L, BYNAME, text, 0.70, 0, S), "record 1 parked");
  const uint32_t id1 = bookSyncInboxIdAt(0);
  char text2[BOOKSYNC_MESH_TEXT_MAX];
  ok(kosyncParkText(mine, 1, 3, 0.1, 4, 0, "CrossPoint", key, text2, sizeof(text2)), "pack");
  const uint32_t S2 = kosyncOfferSig(1790050500, 0.80, "x4", "CrossPoint");
  ok(kosyncParkInto(&L, BYNAME, text2, 0.80, 0, S2), "record 2 replaces it");
  ok(kosyncParkPending(&L, BYNAME, key, &pend) && pend == S2, "record 2 pending");
  ok(!kosyncOfferAnswered(&L, &m, id1), "a card still showing record 1 does not decline record 2");
  ok(kosyncMemoGet(&m, BYNAME, false) == NULL, "...nothing remembered");
  ok(kosyncParkText(mine, 1, 1, 0.2, 4, 0, "CrossPoint", key, text2, sizeof(text2)), "pack");
  ok(kosyncParkInto(&L, BYNAME, text2, 0.9, 0, 0), "a window PUT replaces it (sig 0)");
  ok(kosyncParkPending(&L, BYNAME, key, &pend) && pend == 0, "pending (it blocks Sync), with no home record");
  ok(!kosyncOfferAnswered(&L, &m, bookSyncInboxIdAt(bookSyncInboxCount() - 1)),
     "a window PUT's card: nothing to remember (every new PUT is offered again)");
  bookSyncInboxInit();
}

/* KS-3: the X4 fork PUTs under BOTH ids; the second is not "a different book". */
static void testSecondId() {
  group("KS-3: the X4 fork's second PUT (its file-name id) is not a DIFFERENT book");
  KosyncServeOut o;
  char reply[512], hdrs[512], l[200];
  snprintf(hdrs, sizeof(hdrs), "X HTTP/1.1\r\nx-auth-user: %s\r\nx-auth-key: %s\r\n\r\n", USER, KEY);
  // The same bytes (partial MD5 matches), another file name on the X4 (its name id does not).
  const char* put1 = "{\"document\":\"7db24c08211c49e8e0c2b8522e9efc1b\",\"percentage\":0.7,"
                     "\"device\":\"CrossPoint\",\"device_id\":\"x4-id\"}";
  const char* put2 = "{\"document\":\"0c9a1f7e00000000000000000000beef\",\"percentage\":0.7,"
                     "\"device\":\"CrossPoint\",\"device_id\":\"x4-id\"}";
  static KosyncPutLog w;
  // As kosyncWindowServe feeds the log from each answer.
  const char* order[2][2] = { { put1, put2 }, { put2, put1 } };
  for (int k = 0; k < 2; k++) {
    memset(&w, 0, sizeof(w));
    uint32_t oldCount = 0;
    for (int i = 0; i < 2; i++) {
      serve("PUT", "/syncs/progress", hdrs, order[k][i], &o, reply);
      eqInt(o.code, 200, "answered 200 either way");
      if (o.gotPut) {
        kosyncPutLogOwn(&w, o.putDevice, o.putDeviceId);
      }
      if (o.otherDoc) {
        oldCount++;
        eqStr(o.putDevice, "CrossPoint", "who sent the other-id PUT is known");
        eqStr(o.putDeviceId, "x4-id", "...by its device_id");
        kosyncPutLogOther(&w, o.putDevice, o.putDeviceId, o.otherDocId);
      }
    }
    const char* doc = NULL;
    uint32_t second = 9;
    eqInt(kosyncPutLogDifferent(&w, &doc, &second), 0, k ? "other id FIRST: still not a different book"
                                                           : "the partial PUT, then the name PUT: not a different book");
    eqInt(second, 1, "...it is counted as the reader's second id");
    kosyncWindowProblems(kosyncPutLogDifferent(&w, &doc, NULL), doc, 0, 0, l, sizeof(l));
    eqStr(l, "", "and the screen says nothing is wrong");
    kosyncWindowProblems(oldCount, "0c9a1f7e", 0, 0, l, sizeof(l));
    ok(strstr(l, "DIFFERENT book") != NULL, "(the old count said '! A place for a DIFFERENT book')");
  }

  group("KS-3: a PUT for another document from anyone else is still a DIFFERENT book");
  memset(&w, 0, sizeof(w));
  kosyncPutLogOwn(&w, "CrossPoint", "x4-id");
  kosyncPutLogOther(&w, "KOReader", "ko-id", "abcdef01");
  const char* doc = NULL;
  uint32_t second = 0;
  eqInt(kosyncPutLogDifferent(&w, &doc, &second), 1, "another device's PUT for another book");
  eqStr(doc, "abcdef01", "...with its id for the screen");
  eqInt(second, 0, "no second id");
  kosyncWindowProblems(kosyncPutLogDifferent(&w, &doc, NULL), doc, 0, 0, l, sizeof(l));
  eqStr(l, "! A place for a DIFFERENT book arrived (id abcdef01..) - not the same file here?", "said");
  memset(&w, 0, sizeof(w));
  kosyncPutLogOther(&w, "CrossPoint", "x4-id", "0c9a1f7e");
  eqInt(kosyncPutLogDifferent(&w, &doc, NULL), 1, "a device that never PUT this book: different");
  kosyncPutLogOther(&w, "CrossPoint", "x4-id", "0c9a1f7e");
  eqInt(kosyncPutLogDifferent(&w, &doc, NULL), 2, "twice: (x2)");
  kosyncPutLogOwn(&w, "CrossPoint", "x4-id");
  eqInt(kosyncPutLogDifferent(&w, &doc, &second), 0, "...until it PUTs this book too (either order)");
  eqInt(second, 2, "both were its second id");

  group("KS-3: who is 'the same device'");
  ok(kosyncPeerKey("CrossPoint", "x4-id") == kosyncPeerKey("Renamed", "x4-id"), "the id decides");
  ok(kosyncPeerKey("A", "") != kosyncPeerKey("B", ""), "no id: the name does");
  ok(kosyncPeerKey("x4-id", "") != kosyncPeerKey("", "x4-id"), "a name never passes for an id");
  eqInt(kosyncPeerKey("", ""), 0, "neither: nobody");
  eqInt(kosyncPeerKey(NULL, NULL), 0, "neither (NULL): nobody");
  memset(&w, 0, sizeof(w));
  kosyncPutLogOwn(&w, "", "");
  kosyncPutLogOther(&w, "", "", "0c9a1f7e");
  eqInt(kosyncPutLogDifferent(&w, &doc, NULL), 1, "a PUT naming no device is always counted");
  memset(&w, 0, sizeof(w));
  kosyncPutLogOwn(&w, "KOReader", "");
  kosyncPutLogOther(&w, "KOReader", "", "0c9a1f7e");
  eqInt(kosyncPutLogDifferent(&w, &doc, NULL), 0, "a device with no id, by its name");
  memset(&w, 0, sizeof(w));
  kosyncPutLogOwn(&w, "CrossPoint", "x4-id");
  kosyncPutLogOther(&w, "CrossPoint", "", "0c9a1f7e");
  eqInt(kosyncPutLogDifferent(&w, &doc, NULL), 1, "one PUT with an id, one without: counted (not sure)");

  group("KS-3: a full log never hides a warning");
  memset(&w, 0, sizeof(w));
  for (int i = 0; i < KOSYNC_PUTLOG_MAX + 3; i++) {
    char d[16];
    snprintf(d, sizeof(d), "%08x", 0xa0000000u + (unsigned)i);
    kosyncPutLogOther(&w, "Stranger", "s-id", d);
  }
  eqInt(kosyncPutLogDifferent(&w, &doc, NULL), KOSYNC_PUTLOG_MAX + 3, "every one counted");
  kosyncPutLogOwn(&w, "Stranger", "s-id");
  eqInt(kosyncPutLogDifferent(&w, &doc, &second), 3, "the ones it had no room to tell apart stay counted");
  eqInt(second, KOSYNC_PUTLOG_MAX, "...the logged ones are its second ids");
  memset(&w, 0, sizeof(w));
  for (int i = 0; i < KOSYNC_PUTLOG_MAX; i++) {
    char dev[16];
    snprintf(dev, sizeof(dev), "dev-%d", i);
    kosyncPutLogOwn(&w, "R", dev);
  }
  kosyncPutLogOwn(&w, "Late", "late-id");            // no room left for its own PUT
  kosyncPutLogOther(&w, "Late", "late-id", "0c9a1f7e");
  eqInt(kosyncPutLogDifferent(&w, &doc, NULL), 1, "a device the own-log had no room for: counted");
  eqInt(kosyncPutLogDifferent(NULL, &doc, &second), 0, "no log: nothing");
  ok(doc && !doc[0] && second == 0, "...and nothing said");
}

int main() {
  testMd5();
  testDocIds();
  testShortRead();
  testSpineAndPercent();
  testTextAndUnsyncable();
  testOversized();
  testPaths();
  testFormat();
  testParse();
  testHeaders();
  testRequests();
  testServe();
  testClock();
  testConfig();
  testInboundChain();
  testLedger();
  testAllocFailure();
  testOfferRule();
  testAutoPush();
  testMemo();
  testRekey();
  testClientAndTransport();
  testProblemsAndLibrary();
  testHomeByName();
  testProblemsKept();
  testOfferRace();
  testPendingOffer();
  testSecondId();
  testCapsAndBigText();
  testGolden();
  printf("\n%s%d passed, %d failed\033[0m\n", g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
