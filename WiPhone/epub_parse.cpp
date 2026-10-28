/*
 * epub_parse.cpp — see epub_parse.h.
 *
 * A port of COVEY's covey_ui/epub.py. Where this looks fussy it is matching that file,
 * because the ids it produces are what decide whether a sync packet is about the same book.
 *
 * EPUB_ENTITY_NOTE: COVEY parses XHTML with Python's HTMLParser(convert_charrefs=True), so
 * this decodes the SAME entity set — html_entities.h is generated from the very table Python
 * uses — plus numeric references, and it collapses the same Unicode whitespace set that
 * Python's str.split() does.
 *
 * That is not perfectionism. A reading position is a CHARACTER OFFSET into the extracted
 * text, so any entity decoded to a different length than COVEY produces shifts every offset
 * after it and keeps growing to the end of the chapter — a drift far too small to trip the
 * receiver's "offset past the end of the chapter" check, and therefore silent. A short
 * hand-picked entity list was the first attempt here and &nbsp; alone was enough to break it.
 * Only the ';'-terminated forms are handled; the legacy no-semicolon ones (&amp vs &amp;) are
 * invalid in XHTML.
 */
#include "epub_parse.h"
#include "book_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO)
  #include <Arduino.h>
  #include "rom/miniz.h"
  static void* ebAlloc(size_t n) {
    void* p = ps_malloc(n);                 // books belong in PSRAM; internal RAM is precious
    return p ? p : malloc(n);
  }
  // KOSync's allocations are all OPTIONAL: failing one leaves the book readable, unsyncable.
  static void* ksAlloc(size_t n) { return ebAlloc(n); }
#else
  #include <zlib.h>
  static void* ebAlloc(size_t n) { return malloc(n); }
  int epubTestFailKosyncAllocs = 0;         // see epub_parse.h: host tests only
  static void* ksAlloc(size_t n) { return epubTestFailKosyncAllocs ? NULL : malloc(n); }
#endif

/* ⏱ The open-path timing below is a FIRMWARE measurement (see BooksApp::openBook): on the
 * host there is no millis() and no log_e, and a test run has no superloop to freeze.
 *
 * ⚠ The host EB_TIMING must still CONSUME its arguments. A `do { } while (0)` that dropped
 * them left every timing local unused, and `tests/run_tests.sh` — clean before — started
 * printing ten -Wunused-variable warnings, which is how a suite stops being read. The sink
 * carries `format(printf)` as well, so the HOST build type-checks these format strings; the
 * firmware build is the one that cannot (log_e's own varargs are unchecked here), and a
 * mismatched %u is exactly the kind of thing that goes unnoticed in a log nobody re-reads. */
#if defined(ARDUINO)
  #define EB_NOW()          millis()
  #define EB_TIMING(...)    log_e(__VA_ARGS__)
#else
  #define EB_NOW()          0u
  static inline void ebTimingSink(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  static inline void ebTimingSink(const char* fmt, ...) { (void)fmt; }
  #define EB_TIMING(...)    ebTimingSink(__VA_ARGS__)
#endif

/* Counters for the open-path report: how many SD transactions, how long, how many bytes.
 * ⚠ Scoped to ONE epubOpen by ebSrcReset() at its top, and read only by its report. srcRead
 * keeps incrementing them for the rest of the reading session (chapter loads, images), so
 * they are meaningful only between that reset and that report — do not read them elsewhere
 * expecting an open's figures. */
static uint32_t ebSrcReads = 0, ebSrcMs = 0;
static uint64_t ebSrcBytes = 0;
static void ebSrcReset(void) { ebSrcReads = 0; ebSrcMs = 0; ebSrcBytes = 0; }

static void ebFree(void* p) { free(p); }

// ---------------------------------------------------------------- source helpers
static size_t srcRead(EpubSource* s, uint64_t off, void* buf, size_t len) {
  if (off >= s->size) {
    return 0;
  }
  if (off + len > s->size) {
    len = (size_t)(s->size - off);
  }
  /* ⏱ Every byte the parser takes off the card passes through here, so this is where the
   * cost of the open path is actually counted — see the EB_TIMING report in epubOpen. */
  ebSrcReads++;
  const uint32_t _t0 = EB_NOW();
  const size_t got = s->read(s->ctx, off, buf, len);
  ebSrcMs += (uint32_t)(EB_NOW() - _t0);
  ebSrcBytes += got;
  return got;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------- inflate
/* `partial` accepts a short result instead of failing. Both backends already produce the
 * bytes; they simply refuse to report them unless the whole stream finished. That is right
 * for a chapter — a half-inflated chapter is corruption — and wrong for peeking at the header
 * of a 1 MB cover image just to learn how big it is. */
static int rawInflateEx(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap,
                        bool partial) {
#if defined(ARDUINO)
  // The ROM build of miniz has no allocator, so tinfl_decompress() is used directly and the
  // decompressor state (~11 KB) is heap-allocated rather than put on a task stack.
  tinfl_decompressor* d = (tinfl_decompressor*)ebAlloc(sizeof(tinfl_decompressor));
  if (!d) {
    return -1;
  }
  tinfl_init(d);
  size_t inLen = srcLen, outLen = dstCap;
  tinfl_status st = tinfl_decompress(d, src, &inLen, dst, dst, &outLen,
                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  ebFree(d);
  if (st == TINFL_STATUS_DONE) {
    return (int)outLen;
  }
  return (partial && outLen > 0) ? (int)outLen : -1;
#else
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {     // negative window = raw deflate, no header
    return -1;
  }
  zs.next_in = (Bytef*)src;
  zs.avail_in = (uInt)srcLen;
  zs.next_out = (Bytef*)dst;
  zs.avail_out = (uInt)dstCap;
  int r = inflate(&zs, Z_FINISH);
  size_t got = zs.total_out;
  inflateEnd(&zs);
  if (r == Z_STREAM_END) {
    return (int)got;
  }
  return (partial && got > 0) ? (int)got : -1;
#endif
}

static int rawInflate(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap) {
  return rawInflateEx(src, srcLen, dst, dstCap, false);
}

// ---------------------------------------------------------------- zip
struct ZipEntry {
  uint64_t localOff;
  uint32_t compSize;
  uint32_t uncompSize;
  uint16_t method;
  char     name[EPUB_NAME_MAX];
};

// Offset of the central directory, or 0 if this is not a zip we can read.
static bool zipFindCentral(EpubSource* s, uint64_t* cdOff, uint32_t* nEntries) {
  // The End Of Central Directory record is last, but a zip comment can follow it, so scan
  // back over the largest comment a zip can carry (64 KB) plus the record itself.
  const size_t MAXSCAN = 66000;
  size_t scan = (s->size < MAXSCAN) ? (size_t)s->size : MAXSCAN;
  if (scan < 22) {
    return false;
  }
  uint8_t* buf = (uint8_t*)ebAlloc(scan);
  if (!buf) {
    return false;
  }
  uint64_t base = s->size - scan;
  size_t got = srcRead(s, base, buf, scan);
  bool found = false;
  if (got >= 22) {
    for (size_t i = got - 22 + 1; i-- > 0; ) {
      if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
        *nEntries = rd16(buf + i + 10);
        *cdOff = rd32(buf + i + 16);
        found = true;
        break;
      }
    }
  }
  ebFree(buf);
  return found;
}

typedef bool (*ZipVisit)(const ZipEntry* e, void* user);   // return false to stop

static bool zipForEach(EpubSource* s, ZipVisit cb, void* user) {
  uint64_t cdOff = 0;
  uint32_t n = 0;
  if (!zipFindCentral(s, &cdOff, &n)) {
    return false;
  }
  uint8_t hdr[46];
  uint64_t at = cdOff;
  for (uint32_t i = 0; i < n; i++) {
    if (srcRead(s, at, hdr, 46) != 46) {
      return false;
    }
    if (rd32(hdr) != 0x02014b50) {
      return false;
    }
    ZipEntry e;
    e.method = rd16(hdr + 10);
    e.compSize = rd32(hdr + 20);
    e.uncompSize = rd32(hdr + 24);
    uint16_t nameLen = rd16(hdr + 28);
    uint16_t extraLen = rd16(hdr + 30);
    uint16_t cmtLen = rd16(hdr + 32);
    e.localOff = rd32(hdr + 42);
    size_t want = nameLen < EPUB_NAME_MAX - 1 ? nameLen : EPUB_NAME_MAX - 1;
    if (srcRead(s, at + 46, e.name, want) != want) {
      return false;
    }
    e.name[want] = '\0';
    at += 46 + nameLen + extraLen + cmtLen;
    if (nameLen < EPUB_NAME_MAX) {          // silently skip absurdly long paths
      if (!cb(&e, user)) {
        return true;
      }
    }
  }
  return true;
}

struct FindCtx { const char* want; ZipEntry* out; bool hit; };

static bool findVisit(const ZipEntry* e, void* user) {
  FindCtx* c = (FindCtx*)user;
  if (strcmp(e->name, c->want) == 0) {
    *c->out = *e;
    c->hit = true;
    return false;
  }
  return true;
}

static bool zipFind(EpubSource* s, const char* name, ZipEntry* out) {
  FindCtx c = { name, out, false };
  zipForEach(s, findVisit, &c);
  return c.hit;
}

/* Decompress an entry into `dst`. Returns the size, or -1.
 *
 * The local header's name and extra lengths are read fresh rather than reused from the
 * central directory: the extra field legitimately differs between the two, and using the
 * central one puts the read a few bytes off the start of the data. */
static int zipRead(EpubSource* s, const ZipEntry* e, uint8_t* dst, size_t dstCap) {
  uint8_t lh[30];
  if (srcRead(s, e->localOff, lh, 30) != 30 || rd32(lh) != 0x04034b50) {
    return -1;
  }
  uint64_t dataOff = e->localOff + 30 + rd16(lh + 26) + rd16(lh + 28);
  if (e->method == 0) {                              // stored
    if (e->uncompSize > dstCap) {
      return -1;
    }
    size_t got = srcRead(s, dataOff, dst, e->uncompSize);
    return got == e->uncompSize ? (int)got : -1;
  }
  if (e->method != 8) {                              // only deflate is used by real EPUBs
    return -1;
  }
  uint8_t* comp = (uint8_t*)ebAlloc(e->compSize ? e->compSize : 1);
  if (!comp) {
    return -1;
  }
  int out = -1;
  if (srcRead(s, dataOff, comp, e->compSize) == e->compSize) {
    out = rawInflate(comp, e->compSize, dst, dstCap);
  }
  ebFree(comp);
  return out;
}

/* The first `dstCap` bytes of an entry, however big the entry is. Only a PREFIX of the
 * compressed data is read, so peeking at a 1 MB cover's header costs a few KB either side
 * rather than two megabytes of buffers. */
static int zipReadPrefix(EpubSource* s, const ZipEntry* e, uint8_t* dst, size_t dstCap) {
  uint8_t lh[30];
  if (srcRead(s, e->localOff, lh, 30) != 30 || rd32(lh) != 0x04034b50) {
    return -1;
  }
  uint64_t dataOff = e->localOff + 30 + rd16(lh + 26) + rd16(lh + 28);
  if (e->method == 0) {
    size_t want = e->uncompSize < dstCap ? e->uncompSize : dstCap;
    size_t got = srcRead(s, dataOff, dst, want);
    return (int)got;
  }
  if (e->method != 8) {
    return -1;
  }
  size_t want = dstCap + 1024;                 // slack: deflate can expand a little
  if (want > e->compSize) {
    want = e->compSize;
  }
  uint8_t* comp = (uint8_t*)ebAlloc(want ? want : 1);
  if (!comp) {
    return -1;
  }
  int out = -1;
  if (srcRead(s, dataOff, comp, want) == want) {
    out = rawInflateEx(comp, want, dst, dstCap, true);
  }
  ebFree(comp);
  return out;
}

// ---------------------------------------------------------------- paths
size_t epubNormPath(const char* base, const char* href, char* out, size_t cap) {
  // Drop a fragment first, exactly as COVEY does.
  char h[EPUB_NAME_MAX];
  size_t hl = 0;
  for (const char* p = href; *p && *p != '#' && hl < sizeof(h) - 1; p++) {
    h[hl++] = *p;
  }
  h[hl] = '\0';

  if (!base || !base[0]) {
    // ⚠ COVEY returns the href UNCHANGED when there is no base — it does not normalise.
    // Matching that matters: a name that came out normalised here would not be found in
    // the zip's namelist, and the spine would silently come out empty.
    size_t n = hl < cap - 1 ? hl : cap - 1;
    memcpy(out, h, n);
    out[n] = '\0';
    return n;
  }

  char joined[EPUB_NAME_MAX * 2];
  snprintf(joined, sizeof(joined), "%s/%s", base, h);
  for (char* p = joined; *p; p++) {
    if (*p == '\\') {
      *p = '/';
    }
  }
  /* normpath: collapse "//", drop ".", resolve "..".
   *
   * 24 segments, not 64, because this frame is the largest in the whole book path and it sits
   * at the bottom of the deepest chain: epubOpen -> navTitles -> here measured 4,472 bytes of
   * an 8 KB task stack. A zip path with 24 directories deep does not exist; two 64-entry
   * arrays did, and cost a kilobyte for nothing. */
  const char* seg[24];
  size_t segLen[24];
  int nSeg = 0;
  char* p = joined;
  while (*p) {
    while (*p == '/') {
      p++;
    }
    if (!*p) {
      break;
    }
    char* s = p;
    while (*p && *p != '/') {
      p++;
    }
    size_t len = (size_t)(p - s);
    if (len == 1 && s[0] == '.') {
      continue;
    }
    if (len == 2 && s[0] == '.' && s[1] == '.') {
      if (nSeg > 0) {
        nSeg--;
      }
      continue;
    }
    if (nSeg < (int)(sizeof(seg) / sizeof(seg[0]))) {
      seg[nSeg] = s;
      segLen[nSeg] = len;
      nSeg++;
    }
  }
  size_t n = 0;
  for (int i = 0; i < nSeg; i++) {
    if (i && n < cap - 1) {
      out[n++] = '/';
    }
    size_t take = segLen[i];
    if (n + take > cap - 1) {
      take = cap - 1 - n;
    }
    memcpy(out + n, seg[i], take);
    n += take;
  }
  out[n] = '\0';
  return n;
}

// ---------------------------------------------------------------- slug + fingerprint
size_t epubSlug(const char* in, char* out, size_t cap) {
  /* re.sub(r"[^a-z0-9]+", "-", s.strip().lower()).strip("-")[:64]
   *
   * Only ASCII is lowercased here. That is not a shortcut: the character class replaces
   * every non-[a-z0-9] character with '-' anyway, so a non-ASCII character becomes '-'
   * whether or not it was lowercased first. (The exceptions are the handful of characters
   * that lowercase INTO ASCII, such as U+212A KELVIN SIGN; those would differ from COVEY.
   * Vanishingly rare in book metadata, and the fp: id still matches for identical files.) */
  size_t n = 0;
  bool pendingDash = false;
  bool any = false;
  for (const char* p = in ? in : ""; *p; p++) {
    unsigned char c = (unsigned char)*p;
    char lower = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    bool keep = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9');
    if (keep) {
      if (pendingDash && any && n < cap - 1 && n < 64) {
        out[n++] = '-';
      }
      pendingDash = false;
      if (n < cap - 1 && n < 64) {
        out[n++] = lower;
        any = true;
      }
    } else {
      pendingDash = true;                      // a RUN of them collapses to one '-'
    }
  }
  out[n] = '\0';
  return n;
}

bool epubFingerprint(EpubSource* src, char out[17]) {
  /* sha1(str(size) + first 64 KB + last 64 KB), first 16 hex characters.
   * COVEY hashes the ends plus the length rather than the whole file: a full hash of a
   * 400 MB book on an SD card is not something to do every time a library screen opens,
   * and books are not adversarial input for this purpose. */
  out[0] = '\0';
  BsSha1 c;
  bsSha1Init(&c);
  char sizeStr[32];
  snprintf(sizeStr, sizeof(sizeStr), "%llu", (unsigned long long)src->size);
  bsSha1Update(&c, sizeStr, strlen(sizeStr));

  uint8_t* buf = (uint8_t*)ebAlloc(65536);
  if (!buf) {
    return false;
  }
  size_t got = srcRead(src, 0, buf, 65536);
  bsSha1Update(&c, buf, got);
  if (src->size > 131072) {
    size_t g2 = srcRead(src, src->size - 65536, buf, 65536);
    bsSha1Update(&c, buf, g2);
  }
  ebFree(buf);

  uint8_t d[20];
  bsSha1Final(&c, d);
  static const char* kHexDigits = "0123456789abcdef";  // not HEX: Arduino's Print.h defines that as 16
  for (int i = 0; i < 8; i++) {
    out[i * 2] = kHexDigits[d[i] >> 4];
    out[i * 2 + 1] = kHexDigits[d[i] & 0xF];
  }
  out[16] = '\0';
  return true;
}

// ---------------------------------------------------------------- entities + XML scan
/* The named-entity table is GENERATED from the same html.entities.html5 that COVEY's
 * HTMLParser decodes with (tools/gen_entities.py). It is not a hand-picked "common" list on
 * purpose: a reading position is a character offset into the extracted text, so an entity
 * this failed to decode would leave the WiPhone's text longer than COVEY's from that point
 * to the end of the chapter — a drift that grows rather than staying local, and that is too
 * small to trip the receiver's "offset past the end" sanity check. ~64 KB of flash, which is
 * cheap against the 4.5 MB free. */
#include "html_entities.h"

/* Byte length of the whitespace character at `p`, or 0.
 *
 * Matches Python's str.isspace(), which is what COVEY collapses on in
 * " ".join(text.split()) — and that set is WIDER than ASCII. U+00A0, the character &nbsp;
 * decodes to, is whitespace to Python and would otherwise survive here as a real character,
 * making the WiPhone's text one character longer at every &nbsp; in the chapter. Since a
 * reading position is a character offset, that is a drift, not a cosmetic difference.
 * (U+200B ZERO WIDTH SPACE is deliberately absent: Python does not call it whitespace.) */
static size_t utf8SpaceLen(const char* p, size_t avail) {
  if (avail == 0) {
    return 0;
  }
  unsigned char c = (unsigned char)p[0];
  if (c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F)) {
    return 1;
  }
  if (c < 0x80) {
    return 0;
  }
  uint32_t cp = 0;
  size_t n = 0;
  if ((c & 0xE0) == 0xC0)      { n = 2; cp = c & 0x1Fu; }
  else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0Fu; }
  else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07u; }
  else                         { return 0; }
  if (avail < n) {
    return 0;
  }
  for (size_t k = 1; k < n; k++) {
    if (((unsigned char)p[k] & 0xC0) != 0x80) {
      return 0;
    }
    cp = (cp << 6) | ((unsigned char)p[k] & 0x3Fu);
  }
  if (cp == 0x85 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
      cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000) {
    return n;
  }
  return 0;
}

static size_t utf8Put(uint32_t cp, char* out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* Decode one entity starting at `p` (which points at '&'). Writes UTF-8 into `out` and
 * returns the number of INPUT bytes consumed, or 0 if this is not an entity — in which case
 * the caller passes the '&' through unchanged, as a browser would. */
static size_t decodeEntity(const char* p, size_t avail, char* out, size_t* outLen) {
  if (avail < 3 || p[0] != '&') {
    return 0;
  }
  size_t end = 1;
  while (end < avail && end <= HTML_ENTITY_MAX_NAME && p[end] != ';') {
    end++;
  }
  if (end >= avail || p[end] != ';') {
    return 0;
  }
  if (p[1] == '#') {
    uint32_t cp = 0;
    size_t i = 2;
    if (i < end && (p[i] == 'x' || p[i] == 'X')) {
      i++;
      if (i >= end) {
        return 0;
      }
      for (; i < end; i++) {
        char ch = p[i];
        int v = (ch >= '0' && ch <= '9') ? ch - '0'
              : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
              : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
        if (v < 0) {
          return 0;
        }
        cp = cp * 16 + (uint32_t)v;
      }
    } else {
      if (i >= end) {
        return 0;
      }
      for (; i < end; i++) {
        if (p[i] < '0' || p[i] > '9') {
          return 0;
        }
        cp = cp * 10 + (uint32_t)(p[i] - '0');
      }
    }
    if (cp == 0 || cp > 0x10FFFF) {
      return 0;
    }
    *outLen = utf8Put(cp, out);
    return end + 1;
  }
  // Binary search the generated table; entity names are case-sensitive.
  char name[HTML_ENTITY_MAX_NAME + 1];
  size_t nameLen = end - 1;
  if (nameLen == 0 || nameLen > HTML_ENTITY_MAX_NAME) {
    return 0;
  }
  memcpy(name, p + 1, nameLen);
  name[nameLen] = '\0';
  int lo = 0, hi = HTML_ENTITY_N - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    int cmp = strcmp(name, HTML_ENTITIES[mid].name);
    if (cmp == 0) {
      *outLen = strlen(HTML_ENTITIES[mid].utf8);
      memcpy(out, HTML_ENTITIES[mid].utf8, *outLen);
      return end + 1;
    }
    if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return 0;
}

// Local tag name (after any namespace prefix), lowercased, into `out`.
static void tagLocalName(const char* p, size_t len, char* out, size_t cap) {
  const char* colon = NULL;
  for (size_t i = 0; i < len; i++) {
    if (p[i] == ':') {
      colon = p + i;
    }
  }
  if (colon) {
    len -= (size_t)(colon + 1 - p);
    p = colon + 1;
  }
  size_t n = 0;
  for (size_t i = 0; i < len && n < cap - 1; i++) {
    char c = p[i];
    out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  out[n] = '\0';
}

/* Value of `name` in a raw attribute run. Namespace prefixes are ignored on the attribute
 * name too, so opf:role and role both match "role". Returns false if absent. */
static bool tagAttr(const char* attrs, size_t len, const char* name, char* out, size_t cap) {
  size_t nameLen = strlen(name);
  size_t i = 0;
  while (i < len) {
    while (i < len && (attrs[i] == ' ' || attrs[i] == '\t' || attrs[i] == '\n' || attrs[i] == '\r')) {
      i++;
    }
    size_t ks = i;
    while (i < len && attrs[i] != '=' && attrs[i] != ' ' && attrs[i] != '\t' &&
           attrs[i] != '\n' && attrs[i] != '\r') {
      i++;
    }
    size_t ke = i;
    while (i < len && (attrs[i] == ' ' || attrs[i] == '\t')) {
      i++;
    }
    if (i >= len || attrs[i] != '=') {
      if (ks == ke) {
        i++;
      }
      continue;
    }
    i++;
    while (i < len && (attrs[i] == ' ' || attrs[i] == '\t')) {
      i++;
    }
    char quote = 0;
    if (i < len && (attrs[i] == '"' || attrs[i] == '\'')) {
      quote = attrs[i++];
    }
    size_t vs = i;
    while (i < len && (quote ? attrs[i] != quote : (attrs[i] != ' ' && attrs[i] != '\t'))) {
      i++;
    }
    size_t ve = i;
    if (quote && i < len) {
      i++;
    }
    // Compare, ignoring any namespace prefix on the attribute name.
    const char* k = attrs + ks;
    size_t kl = ke - ks;
    for (size_t j = 0; j < kl; j++) {
      if (k[j] == ':') {
        k += j + 1;
        kl -= j + 1;
        break;
      }
    }
    if (kl == nameLen && strncmp(k, name, nameLen) == 0) {
      size_t n = 0;
      size_t vi = vs;
      while (vi < ve && n < cap - 1) {
        char ebuf[8];
        size_t elen = 0;
        size_t used = decodeEntity(attrs + vi, ve - vi, ebuf, &elen);
        if (used) {
          for (size_t z = 0; z < elen && n < cap - 1; z++) {
            out[n++] = ebuf[z];
          }
          vi += used;
        } else {
          out[n++] = attrs[vi++];
        }
      }
      out[n] = '\0';
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- text extraction
static const char* BLOCK_TAGS[] = { "p", "div", "br", "li", "tr", "blockquote", "section",
                                    "article", "h1", "h2", "h3", "h4", "h5", "h6",
                                    "figcaption", "td", "pre" };
static const char* DROP_TAGS[] = { "script", "style", "head", "title", "svg", "nav" };

static bool inList(const char* const* list, size_t n, const char* name) {
  for (size_t i = 0; i < n; i++) {
    if (strcmp(list[i], name) == 0) {
      return true;
    }
  }
  return false;
}

struct TextOut {
  char*  buf;
  size_t cap;
  size_t len;        // committed blocks, joined by "\n\n"
  char*  pend;       // current block being accumulated
  size_t pendCap;
  size_t pendLen;
  bool   blocks;     // has at least one committed block
};

static void toPut(TextOut* t, const char* s, size_t n) {
  for (size_t i = 0; i < n && t->pendLen < t->pendCap - 1; i++) {
    t->pend[t->pendLen++] = s[i];
  }
}

static void toFlush(TextOut* t) {
  /* COVEY: " ".join("".join(buf).split()) — collapse every run of whitespace to one space
   * and trim; an empty result appends nothing at all. */
  t->pend[t->pendLen] = '\0';
  char* p = t->pend;
  size_t start = t->len;
  bool wroteAny = false;
  bool needSep = t->blocks;
  size_t i = 0;
  while (i < t->pendLen) {
    size_t sp;
    while (i < t->pendLen && (sp = utf8SpaceLen(p + i, t->pendLen - i)) != 0) {
      i += sp;
    }
    if (i >= t->pendLen) {
      break;
    }
    if (needSep && t->len + 2 < t->cap) {          // blank line between blocks
      t->buf[t->len++] = '\n';
      t->buf[t->len++] = '\n';
      needSep = false;
    } else if (wroteAny && t->len + 1 < t->cap) {
      t->buf[t->len++] = ' ';
    }
    while (i < t->pendLen && utf8SpaceLen(p + i, t->pendLen - i) == 0) {
      if (t->len < t->cap - 1) {
        t->buf[t->len++] = p[i];
      }
      i++;
    }
    wroteAny = true;
  }
  if (wroteAny) {
    t->blocks = true;
  } else {
    t->len = start;
  }
  t->pendLen = 0;
  t->buf[t->len] = '\0';
}

/* The extractor. `baseDir`/`imgs` are optional: when given, every <img src> is recorded with
 * the output length committed at that point. Passing them changes the produced TEXT by
 * nothing at all — `img` is neither a BLOCK_TAG nor a DROP_TAG, so it emits no characters and
 * forces no break, and the capture only reads a counter. That invariant is the whole reason
 * pictures could be added without moving a single reading position. */
static size_t extractTextImpl(const char* html, size_t htmlLen, char* out, size_t cap,
                              const char* baseDir, EpubImage* imgs, int maxImgs, int* nImgs) {
  if (cap < 4) {
    return 0;
  }
  size_t pendCap = cap;
  char* pend = (char*)ebAlloc(pendCap);
  if (!pend) {
    return 0;
  }
  TextOut t = { out, cap, 0, pend, pendCap, 0, false };
  out[0] = '\0';

  int skip = 0;
  size_t i = 0;
  while (i < htmlLen) {
    if (html[i] != '<') {
      if (!skip) {
        char ebuf[8];
        size_t elen = 0;
        size_t used = decodeEntity(html + i, htmlLen - i, ebuf, &elen);
        if (used) {
          toPut(&t, ebuf, elen);
          i += used;
          continue;
        }
        toPut(&t, html + i, 1);
      }
      i++;
      continue;
    }
    // Comments, PIs, doctype and CDATA carry no reading text.
    if (i + 3 < htmlLen && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
      size_t j = i + 4;
      while (j + 2 < htmlLen && !(html[j] == '-' && html[j + 1] == '-' && html[j + 2] == '>')) {
        j++;
      }
      i = (j + 3 < htmlLen) ? j + 3 : htmlLen;
      continue;
    }
    if (i + 1 < htmlLen && (html[i + 1] == '?' || html[i + 1] == '!')) {
      size_t j = i + 1;
      while (j < htmlLen && html[j] != '>') {
        j++;
      }
      i = (j < htmlLen) ? j + 1 : htmlLen;
      continue;
    }
    bool closing = (i + 1 < htmlLen && html[i + 1] == '/');
    size_t ns = i + (closing ? 2 : 1);
    size_t j = ns;
    while (j < htmlLen && html[j] != '>' && html[j] != ' ' && html[j] != '\t' &&
           html[j] != '\n' && html[j] != '\r' && html[j] != '/') {
      j++;
    }
    char name[32];
    tagLocalName(html + ns, j - ns, name, sizeof(name));
    size_t attrStart = j;
    while (j < htmlLen && html[j] != '>') {         // skip attributes, honouring quotes
      if (html[j] == '"' || html[j] == '\'') {
        char q = html[j++];
        while (j < htmlLen && html[j] != q) {
          j++;
        }
      }
      j++;
    }
    size_t next = (j < htmlLen) ? j + 1 : htmlLen;

    if (inList(DROP_TAGS, sizeof(DROP_TAGS) / sizeof(DROP_TAGS[0]), name)) {
      if (closing) {
        skip = skip > 0 ? skip - 1 : 0;
      } else {
        skip++;
      }
      i = next;
      continue;
    }
    if (!skip && inList(BLOCK_TAGS, sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]), name)) {
      toFlush(&t);                                  // flush on BOTH open and close, as COVEY
    }
    /* A picture. Recorded, never rendered into the text — see the note on this function.
     * Deliberately after the DROP check, so an <image> inside a dropped <svg> is ignored for
     * the same reason its text is. `t.len` excludes anything still pending, so an image in
     * the middle of a paragraph anchors to the START of that paragraph; in a real book they
     * sit between paragraphs, where this is exact. */
    if (!skip && !closing && imgs && nImgs && *nImgs < maxImgs && strcmp(name, "img") == 0) {
      char href[EPUB_NAME_MAX];
      if (tagAttr(html + attrStart, j - attrStart, "src", href, sizeof(href)) && href[0]) {
        EpubImage* im = &imgs[*nImgs];
        im->off = (uint32_t)t.len;
        im->w = im->h = 0;
        epubNormPath(baseDir, href, im->name, EPUB_NAME_MAX);
        (*nImgs)++;
      }
    }
    i = next;
  }
  toFlush(&t);
  ebFree(pend);
  return t.len;
}

size_t epubExtractText(const char* html, size_t htmlLen, char* out, size_t cap) {
  return extractTextImpl(html, htmlLen, out, cap, NULL, NULL, 0, NULL);
}

// ---------------------------------------------------------------- OPF
struct OpfItem { char id[64]; char href[EPUB_NAME_MAX]; char media[64]; bool present; };
/* 🛑 PINNED AT 0.9.78's SIZE. 512 of these are the one allocation every EPUB open REQUIRES
 * (~164 KB of contiguous PSRAM, while a 512 KB OPF buffer is also live); a failure is
 * EPUB_ERR_MEMORY and the book does not open. KOSync's per-item data briefly lived in here
 * and took it to ~266 KB — for a feature most phones never switch on. It is now OpfCp below,
 * a separate and OPTIONAL block. Anything added to this struct costs every owner. */
static_assert(sizeof(OpfItem) == 321, "OpfItem is the required allocation - see the note");

/* The KOSync map's per-manifest-item data, parallel to items[] (see EpubKosyncMap in the
 * header): the same href resolved CrossPoint's way (%XX decoded, '.' kept) and the
 * uncompressed size of the zip entry with EXACTLY that name. Filled in the one presence walk,
 * read by nothing on the reading path. OPTIONAL: when it cannot be allocated the book opens
 * exactly as it would have, and is simply not syncable over KOSync. */
struct OpfCp { char path[EPUB_NAME_MAX]; uint32_t bytes; };

/* Mark which manifest hrefs actually exist in the archive, in ONE pass over the central
 * directory.
 *
 * 🛑 MEASURED 2026-08-27, and it was the whole of the book-open freeze: the spine loop used
 * to ask `zipHasName(src, it->href)` per spine item, and zipHasName is a full zipForEach —
 * a 64 KB EOCD re-scan plus two small SD reads for every entry it walks. At 90 spine items
 * that is ninety walks of the same archive: **34,332 SD reads, 7.4 MB, 10.9 of the 11.9
 * seconds** an open took (BOOK OPEN / epubOpen instrumentation, phone 1, Ghosts_of_
 * Timkovichi.epub, 227 manifest items). One walk answers the question for every item at
 * once; the inner strcmp loop is memory-speed and costs nothing by comparison.
 *
 * That freeze is not only a reader annoyance: the superloop is stopped for its whole
 * length, and every piece of the WiFi rescue machinery is polled from that loop — see
 * docs/HANDOFF.md on the booksync wedge. */
struct PresenceCtx { OpfItem* items; int n; OpfCp* cp; };
static bool presenceVisit(const ZipEntry* e, void* user) {
  PresenceCtx* c = (PresenceCtx*)user;
  /* ⚠ NO `break` ON THE FIRST MATCH. Two manifest items may carry the SAME href (malformed,
   * but real), and the per-item zipHasName this replaces answered true for BOTH of them.
   * Stopping at the first would leave the second marked absent and its chapter would vanish
   * from the book with no error anywhere — the silent-failure shape this repo keeps
   * relearning. Marking every match keeps the old semantics exactly. */
  for (int k = 0; k < c->n; k++) {
    if (strcmp(c->items[k].href, e->name) == 0) {
      c->items[k].present = true;
    }
    /* KOSync's sizes, from the same pass: zero extra card reads. A later entry with the same
     * name overwrites an earlier one — the generator's dict does the same, and a zip with two
     * entries of one name is broken anyway. Never read by the reading path. */
    if (c->cp && c->cp[k].path[0] && strcmp(c->cp[k].path, e->name) == 0) {
      c->cp[k].bytes = e->uncompSize;
    }
  }
  return true;                        // every entry: we are answering for all items at once
}

struct OpfParse {
  OpfItem* items;
  int      nItems;
  char   (*spineIds)[64];
  int      nSpine;
};

static void collapseCut(const char* in, size_t inLen, char* out, size_t cap, size_t maxChars) {
  // " ".join(text.split())[:120] — collapse whitespace, then cut by CHARACTER.
  char tmp[EPUB_META_MAX * 2];
  size_t n = 0;
  bool wrote = false;
  size_t i = 0;
  while (i < inLen && n < sizeof(tmp) - 1) {
    size_t sp;
    while (i < inLen && (sp = utf8SpaceLen(in + i, inLen - i)) != 0) {
      i += sp;
    }
    if (i >= inLen) {
      break;
    }
    if (wrote && n < sizeof(tmp) - 1) {
      tmp[n++] = ' ';
    }
    while (i < inLen && utf8SpaceLen(in + i, inLen - i) == 0) {
      char ebuf[8];
      size_t elen = 0;
      size_t used = decodeEntity(in + i, inLen - i, ebuf, &elen);
      if (used) {
        for (size_t z = 0; z < elen && n < sizeof(tmp) - 1; z++) {
          tmp[n++] = ebuf[z];
        }
        i += used;
      } else if (n < sizeof(tmp) - 1) {
        tmp[n++] = in[i++];
      } else {
        i++;
      }
    }
    wrote = true;
  }
  tmp[n] = '\0';
  bsUtf8TruncChars(out, cap, tmp, maxChars);
}

int epubIds(const EpubBook* b, char out[3][EPUB_ID_MAX]) {
  int n = 0;
  char slug[EPUB_ID_MAX];
  if (b->identifier[0]) {
    epubSlug(b->identifier, slug, sizeof(slug));
    snprintf(out[n++], EPUB_ID_MAX, "id:%s", slug);
  }
  if (b->title[0]) {
    char joined[EPUB_META_MAX * 2];
    snprintf(joined, sizeof(joined), "%s|%s", b->title, b->author);
    epubSlug(joined, slug, sizeof(slug));
    snprintf(out[n++], EPUB_ID_MAX, "ta:%s", slug);
  }
  snprintf(out[n++], EPUB_ID_MAX, "fp:%s", b->fingerprint);
  return n;
}

// ---------------------------------------------------------------- open
/* ⚠ `zipHasName(src, name)` used to live here — a full central-directory walk to answer one
 * yes/no. It was called per spine item and cost 10.9 s of a book open; PresenceCtx above
 * answers the same question for every item in one walk. Deleted rather than left lying
 * around, because the next per-item caller would silently reintroduce the freeze. */

// ---------------------------------------------------------------- KOSync map (see the header)
/* An href resolved CrossPoint's way — normalisePath(decodeUriEscapes(opfDir + href)) — into
 * `out` (EPUB_NAME_MAX). `scratch` is a PSRAM buffer of EPUB_NAME_MAX*2 + 8: the joined path
 * can be twice a name before '..' shortens it, and epubOpen's frame is not the place for
 * another 400 bytes of stack (see the 24-segment note in epubNormPath).
 *
 * ⚠ A name that ends up EPUB_NAME_MAX or longer is left EMPTY rather than cut: zipForEach
 * never reports such an entry, so a cut name could only ever match the wrong file. The item
 * then weighs 0 bytes here where CrossPoint would find it — one of the documented ways the
 * two can differ, and vanishingly rare (a 192-byte path inside a book). */
static void kosyncCpPath(const char* base, const char* href, char* out, char* scratch) {
  out[0] = '\0';
  if (!scratch) {
    return;
  }
  const size_t cap = EPUB_NAME_MAX * 2 + 8;
  const int j = (base && base[0]) ? snprintf(scratch, cap, "%s/%s", base, href)
                                  : snprintf(scratch, cap, "%s", href);
  if (j < 0 || (size_t)j >= cap) {
    return;
  }
  epubKosyncDecodePath(scratch, scratch, cap);        // shrinks or keeps: safe in place
  const size_t n = epubKosyncNormPath(scratch, scratch, cap);
  if (n >= EPUB_NAME_MAX) {
    return;
  }
  memcpy(out, scratch, n + 1);
}

static EpubKosyncMap* kosyncMapAlloc(int nRead) {
  EpubKosyncMap* m = (EpubKosyncMap*)ksAlloc(sizeof(EpubKosyncMap));
  if (m) {
    memset(m, 0, sizeof(*m));
    m->nRead = nRead;
    for (int i = 0; i < EPUB_MAX_SPINE; i++) {
      m->cpToRead[i] = -1;
      m->readToCp[i] = -1;
    }
  }
  return m;
}

// A .txt: one item the size of the file, so the percentage is just how far through it.
static void kosyncMapForText(EpubBook* b) {
  EpubKosyncMap* m = kosyncMapAlloc(1);
  if (!m) {
    return;
  }
  const uint64_t sz = b->src ? b->src->size : 0;
  m->nCp = 1;
  m->cum[0] = sz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sz;
  m->sizesKnown = true;
  m->cpToRead[0] = 0;
  m->readToCp[0] = 0;
  /* 🛑 ONLY IF THE READER SHOWS ALL OF IT. epubChapterTextImages reads the first cap-1 bytes
   * of a .txt, so a bigger file is shown CUT: every place on it said 0 % (kosyncOversized),
   * and a peer's 80 % landed 80 % into the cut text — about a fifth of a 2 MB novel. Not
   * syncable, and saying so, beats both. (A reader whose buffer fell back to a smaller one
   * marks the same thing itself: BooksApp::openBook.) */
  if (sz > (uint64_t)(EPUB_MAX_DOC - 1)) {
    m->refused = EPUB_KOSYNC_TEXT_TOO_BIG;
  }
  b->kosync = m;
}

/* CrossPoint's spine, its sizes, and the two directions of the reading<->CrossPoint mapping.
 * Every itemref naming a manifest item is an item (the LAST manifest item with that id, as
 * the generator's dict has it); a reading chapter maps to the FIRST CrossPoint item with the
 * same path, and a CrossPoint item to the FIRST reading chapter with its path. */
static void kosyncMapBuild(EpubBook* b, const OpfItem* items, const OpfCp* cp, int nItems,
                           char (*cpIds)[64], int nCpIds, bool sizesKnown,
                           bool manifestCapped, bool itemrefsCapped) {
  if (!cp || !cpIds) {
    return;                              // an optional block failed: readable, not syncable
  }
  EpubKosyncMap* m = kosyncMapAlloc(b->nSpine);
  int16_t* cpItem = (int16_t*)ksAlloc(sizeof(int16_t) * EPUB_MAX_SPINE);
  if (!m || !cpItem) {
    ebFree(m);
    ebFree(cpItem);
    return;                              // the book reads; it just cannot sync over KOSync
  }
  m->sizesKnown = sizesKnown;
  /* ⚠ PAST THE CAPS, THE X4 COUNTS WHAT WE CANNOT. More itemrefs than EPUB_MAX_SPINE: the
   * items past it are missing from the total. A manifest cut at EPUB_MAX_SPINE: an itemref
   * naming an item past the cut looks exactly like one that names nothing (below), which
   * CrossPoint skips and we would too — wrongly. Either way every percentage would differ
   * from the X4's with no warning (picture books, cookbooks), so the book is refused. */
  if (itemrefsCapped) {
    m->refused = EPUB_KOSYNC_TOO_MANY_ITEMS;
  }
  uint32_t sum = 0;
  for (int c = 0; cpIds && c < nCpIds; c++) {
    int k = -1;
    for (int q = 0; q < nItems; q++) {
      if (strcmp(items[q].id, cpIds[c]) == 0) {
        k = q;                           // keep going: the LAST one with this id wins
      }
    }
    if (k < 0 && manifestCapped) {
      m->refused = EPUB_KOSYNC_TOO_MANY_ITEMS;   // past the cut, or truly nothing: cannot tell
    }
    if (k < 0 || !items[k].href[0]) {
      continue;                          // an idref that names nothing is not an item
    }
    const uint32_t sz = cp[k].bytes;
    sum = (sum > 0xFFFFFFFFu - sz) ? 0xFFFFFFFFu : sum + sz;
    cpItem[m->nCp] = (int16_t)k;
    m->cum[m->nCp] = sum;
    m->nCp++;
  }
  for (int r = 0; r < b->nSpine && r < EPUB_MAX_SPINE; r++) {
    for (int c = 0; c < m->nCp; c++) {
      if (strcmp(cp[cpItem[c]].path, b->spine[r].name) == 0) {
        m->readToCp[r] = (int16_t)c;
        break;
      }
    }
  }
  for (int c = 0; c < m->nCp; c++) {
    for (int r = 0; r < b->nSpine && r < EPUB_MAX_SPINE; r++) {
      if (strcmp(cp[cpItem[c]].path, b->spine[r].name) == 0) {
        m->cpToRead[c] = (int16_t)r;
        break;
      }
    }
  }
  ebFree(cpItem);
  b->kosync = m;
}

struct FallbackCtx { EpubBook* b; };
static bool fallbackVisit(const ZipEntry* e, void* user) {
  /* Some malformed files have a manifest but no usable spine. COVEY falls back to every
   * XHTML document in zip order — wrong in principle, right in practice: it is the
   * difference between a readable book and an error message. */
  FallbackCtx* c = (FallbackCtx*)user;
  size_t l = strlen(e->name);
  bool html = (l > 6 && strcmp(e->name + l - 6, ".xhtml") == 0) ||
              (l > 5 && strcmp(e->name + l - 5, ".html") == 0) ||
              (l > 4 && strcmp(e->name + l - 4, ".htm") == 0);
  if (html && c->b->nSpine < EPUB_MAX_SPINE) {
    snprintf(c->b->spine[c->b->nSpine].name, EPUB_NAME_MAX, "%s", e->name);
    c->b->spine[c->b->nSpine].title[0] = '\0';
    c->b->nSpine++;
  }
  return true;
}

static void stemOf(const char* displayName, char* out, size_t cap) {
  const char* dot = strrchr(displayName, '.');
  size_t n = dot ? (size_t)(dot - displayName) : strlen(displayName);
  if (n > cap - 1) {
    n = cap - 1;
  }
  memcpy(out, displayName, n);
  out[n] = '\0';
}

// Give spine item `href` (resolved against `here`) the title `label`, if it has none yet.
static void navApply(EpubBook* b, const char* here, const char* href, const char* label) {
  if (!href[0] || !label[0]) {
    return;
  }
  char full[EPUB_NAME_MAX];
  epubNormPath(here, href, full, sizeof(full));    // also drops the "#aid_75" fragment
  for (int i = 0; i < b->nSpine; i++) {
    if (!b->spine[i].title[0] && strcmp(b->spine[i].name, full) == 0) {
      snprintf(b->spine[i].title, EPUB_CH_TITLE_MAX, "%s", label);
      return;
    }
  }
}

/* Chapter titles from the EPUB3 nav document or the EPUB2 NCX — COVEY's _nav_titles.
 *
 * Optional, and every failure here is swallowed: a missing or broken table of contents must
 * never stop a book opening. Without it every chapter is "Chapter N", which is usable for a
 * novel and useless for the 90-item book this was written against.
 *
 * ⚠ One deliberate difference from COVEY: for an NCX navPoint it takes the FIRST <text>
 * element (the navLabel) rather than concatenating every descendant's text. They agree on a
 * flat table of contents; on a nested one COVEY's outer entries come out as their own label
 * with all their children's labels run together. Titles never travel over the wire — only the
 * spine index does — so the two devices are free to differ here. */
static void navTitles(EpubBook* b, EpubSource* src, const OpfItem* items, int nItems) {
  uint8_t* doc = NULL;
  for (int k = 0; k < nItems; k++) {
    const char* href = items[k].href;
    const char* slash = strrchr(href, '/');
    const char* base = slash ? slash + 1 : href;
    char low[EPUB_NAME_MAX];
    size_t bl = strlen(base);
    if (bl >= sizeof(low)) {
      continue;
    }
    for (size_t i = 0; i <= bl; i++) {
      char c = base[i];
      low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    bool isNcx = bl > 4 && strcmp(low + bl - 4, ".ncx") == 0;
    if (!isNcx && !strstr(low, "nav")) {
      continue;
    }

    ZipEntry ze;
    if (!zipFind(src, href, &ze)) {
      continue;
    }
    if (!doc) {
      doc = (uint8_t*)ebAlloc(EPUB_MAX_DOC);
      if (!doc) {
        return;
      }
    }
    int n = zipRead(src, &ze, doc, EPUB_MAX_DOC);
    if (n <= 0) {
      continue;
    }

    char here[EPUB_NAME_MAX] = {0};
    if (slash) {
      size_t hl = (size_t)(slash - href);
      memcpy(here, href, hl);
      here[hl] = '\0';
    }

    const char* p = (const char*)doc;
    size_t len = (size_t)n;
    size_t i = 0;
    char pendLabel[EPUB_CH_TITLE_MAX] = {0};      // NCX: navLabel awaiting its <content>
    char aHref[EPUB_NAME_MAX] = {0};              // nav: <a href> awaiting its text
    bool inA = false;
    while (i < len) {
      if (p[i] != '<') {
        i++;
        continue;
      }
      if (i + 3 < len && p[i + 1] == '!' && p[i + 2] == '-' && p[i + 3] == '-') {
        size_t j = i + 4;
        while (j + 2 < len && !(p[j] == '-' && p[j + 1] == '-' && p[j + 2] == '>')) {
          j++;
        }
        i = (j + 3 < len) ? j + 3 : len;
        continue;
      }
      bool closing = (i + 1 < len && p[i + 1] == '/');
      size_t ns = i + (closing ? 2 : 1);
      size_t j = ns;
      while (j < len && p[j] != '>' && p[j] != ' ' && p[j] != '\t' && p[j] != '\n' &&
             p[j] != '\r' && p[j] != '/') {
        j++;
      }
      char name[32];
      tagLocalName(p + ns, j - ns, name, sizeof(name));
      size_t as = j;
      while (j < len && p[j] != '>') {
        if (p[j] == '"' || p[j] == '\'') {
          char q = p[j++];
          while (j < len && p[j] != q) {
            j++;
          }
        }
        j++;
      }
      size_t ae = j;
      size_t textStart = j + 1;
      size_t textEnd = textStart;
      while (textEnd < len && p[textEnd] != '<') {
        textEnd++;
      }

      if (!closing && strcmp(name, "navPoint") == 0) {
        pendLabel[0] = '\0';                       // a new entry: forget the last one
      } else if (!closing && strcmp(name, "text") == 0 && !pendLabel[0]) {
        collapseCut(p + textStart, textEnd - textStart, pendLabel, sizeof(pendLabel), 80);
      } else if (!closing && strcmp(name, "content") == 0) {
        char srcAttr[EPUB_NAME_MAX];
        if (tagAttr(p + as, ae - as, "src", srcAttr, sizeof(srcAttr))) {
          navApply(b, here, srcAttr, pendLabel);
          pendLabel[0] = '\0';
        }
      } else if (!closing && strcmp(name, "a") == 0) {
        // EPUB3 nav. The label is usually the text right here; when it is wrapped in a
        // <span> the run is empty, so hold the href until something before </a> has text.
        if (tagAttr(p + as, ae - as, "href", aHref, sizeof(aHref))) {
          inA = true;
          char label[EPUB_CH_TITLE_MAX];
          collapseCut(p + textStart, textEnd - textStart, label, sizeof(label), 80);
          if (label[0]) {
            navApply(b, here, aHref, label);
            inA = false;
            aHref[0] = '\0';
          }
        }
      } else if (inA && closing && strcmp(name, "a") == 0) {
        inA = false;
        aHref[0] = '\0';
      } else if (inA && !closing && textEnd > textStart) {
        char label[EPUB_CH_TITLE_MAX];
        collapseCut(p + textStart, textEnd - textStart, label, sizeof(label), 80);
        if (label[0]) {
          navApply(b, here, aHref, label);
          inA = false;
          aHref[0] = '\0';
        }
      }
      i = ae < len ? ae + 1 : len;
    }
  }
  ebFree(doc);
}

EpubStatus epubOpen(EpubBook* b, EpubSource* src, const char* displayName, bool isTextFile) {
  memset(b, 0, sizeof(*b));
  b->src = src;
  /* ⏱ See the note on BooksApp::openBook: this whole call runs at GUI depth with the
   * superloop stopped, so its parts are timed and reported rather than guessed at. */
  ebSrcReset();
  const uint32_t _e0 = EB_NOW();
  epubFingerprint(src, b->fingerprint);
  const uint32_t _eFp = EB_NOW();
  const uint32_t _rFp = ebSrcReads;

  if (isTextFile) {
    b->isText = true;
    stemOf(displayName, b->title, sizeof(b->title));
    b->spine = (EpubSpineItem*)ebAlloc(sizeof(EpubSpineItem));
    if (!b->spine) {
      return EPUB_ERR_MEMORY;
    }
    snprintf(b->spine[0].name, EPUB_NAME_MAX, "%s", displayName);
    snprintf(b->spine[0].title, EPUB_CH_TITLE_MAX, "%s", b->title);
    b->nSpine = 1;
    kosyncMapForText(b);       // one "CrossPoint item" the size of the file: pct = within
    return EPUB_OK;
  }

  ZipEntry ce;
  if (!zipFind(src, "META-INF/container.xml", &ce)) {
    return EPUB_ERR_NO_CONTAINER;
  }
  const uint32_t _eFindC = EB_NOW();
  const uint32_t _rFindC = ebSrcReads;
  uint8_t* buf = (uint8_t*)ebAlloc(EPUB_MAX_DOC);
  if (!buf) {
    return EPUB_ERR_MEMORY;
  }
  int n = zipRead(src, &ce, buf, EPUB_MAX_DOC);
  if (n <= 0) {
    ebFree(buf);
    return EPUB_ERR_NO_CONTAINER;
  }
  const uint32_t _eReadC = EB_NOW();

  // container.xml -> the OPF path
  char opfName[EPUB_NAME_MAX] = {0};
  {
    const char* p = (const char*)buf;
    size_t len = (size_t)n;
    size_t i = 0;
    while (i < len && !opfName[0]) {
      if (p[i] != '<') {
        i++;
        continue;
      }
      size_t ns = i + 1;
      size_t j = ns;
      while (j < len && p[j] != '>' && p[j] != ' ' && p[j] != '\t' && p[j] != '\n') {
        j++;
      }
      char name[32];
      tagLocalName(p + ns, j - ns, name, sizeof(name));
      size_t as = j;
      while (j < len && p[j] != '>') {
        j++;
      }
      if (strcmp(name, "rootfile") == 0) {
        tagAttr(p + as, j - as, "full-path", opfName, sizeof(opfName));
      }
      i = j + 1;
    }
  }
  if (!opfName[0]) {
    ebFree(buf);
    return EPUB_ERR_NO_ROOTFILE;
  }

  const uint32_t _ePreFindO = EB_NOW();
  ZipEntry oe;
  if (!zipFind(src, opfName, &oe)) {
    ebFree(buf);
    return EPUB_ERR_NO_OPF;
  }
  const uint32_t _eFindO = EB_NOW();
  const uint32_t _rFindO = ebSrcReads;
  n = zipRead(src, &oe, buf, EPUB_MAX_DOC);
  if (n <= 0) {
    ebFree(buf);
    return EPUB_ERR_NO_OPF;
  }
  const uint32_t _eReadO = EB_NOW();

  // ⚠ THE HREF TRAP: manifest hrefs resolve against the OPF's OWN directory, not the zip
  // root. Getting this wrong yields a book with zero chapters and no error at all.
  char base[EPUB_NAME_MAX] = {0};
  {
    const char* slash = strrchr(opfName, '/');
    if (slash) {
      size_t bl = (size_t)(slash - opfName);
      memcpy(base, opfName, bl);
      base[bl] = '\0';
    }
  }

  OpfItem* items = (OpfItem*)ebAlloc(sizeof(OpfItem) * EPUB_MAX_SPINE);
  char (*spineIds)[64] = (char (*)[64])ebAlloc(64 * EPUB_MAX_SPINE);
  b->spine = (EpubSpineItem*)ebAlloc(sizeof(EpubSpineItem) * EPUB_MAX_SPINE);
  if (!items || !spineIds || !b->spine) {
    ebFree(buf); ebFree(items); ebFree(spineIds); ebFree(b->spine);
    b->spine = NULL;
    return EPUB_ERR_MEMORY;
  }
  int nItems = 0, nSpineIds = 0;
  /* KOSync's spine: EVERY itemref, linear="no" included, in document order — kept apart
   * from spineIds so the reading spine is built from exactly the list it always was. Both
   * allocations are optional: if either fails the book reads normally and simply is not
   * syncable over KOSync. PSRAM, with the rest of the open's scratch. */
  char (*cpIds)[64] = (char (*)[64])ksAlloc(64 * EPUB_MAX_SPINE);
  char* cpScratch = (char*)ksAlloc(EPUB_NAME_MAX * 2 + 8);
  OpfCp* cp = (OpfCp*)ksAlloc(sizeof(OpfCp) * EPUB_MAX_SPINE);   // ~100 KB, optional
  int nCpIds = 0;
  bool cpSizesKnown = false;
  bool cpManifestCapped = false, cpItemrefsCapped = false;   // KOSync only: see kosyncMapBuild

  {
    const char* p = (const char*)buf;
    size_t len = (size_t)n;
    size_t i = 0;
    while (i < len) {
      if (p[i] != '<') {
        i++;
        continue;
      }
      if (i + 3 < len && p[i + 1] == '!' && p[i + 2] == '-' && p[i + 3] == '-') {
        size_t j = i + 4;
        while (j + 2 < len && !(p[j] == '-' && p[j + 1] == '-' && p[j + 2] == '>')) {
          j++;
        }
        i = (j + 3 < len) ? j + 3 : len;
        continue;
      }
      bool closing = (i + 1 < len && p[i + 1] == '/');
      size_t ns = i + (closing ? 2 : 1);
      size_t j = ns;
      while (j < len && p[j] != '>' && p[j] != ' ' && p[j] != '\t' && p[j] != '\n' &&
             p[j] != '\r' && p[j] != '/') {
        j++;
      }
      char name[32];
      tagLocalName(p + ns, j - ns, name, sizeof(name));
      size_t as = j;
      while (j < len && p[j] != '>') {
        if (p[j] == '"' || p[j] == '\'') {
          char q = p[j++];
          while (j < len && p[j] != q) {
            j++;
          }
        }
        j++;
      }
      size_t ae = j;
      size_t textStart = j + 1;
      size_t textEnd = textStart;
      while (textEnd < len && p[textEnd] != '<') {
        textEnd++;
      }

      /* KOSync only, and nothing else reads them: did the manifest or the itemrefs run past
       * the caps below? The branches themselves are exactly what they were. */
      if (!closing && nItems >= EPUB_MAX_SPINE && strcmp(name, "item") == 0) {
        cpManifestCapped = true;
      } else if (!closing && nCpIds >= EPUB_MAX_SPINE && strcmp(name, "itemref") == 0) {
        cpItemrefsCapped = true;
      }
      if (!closing && strcmp(name, "item") == 0 && nItems < EPUB_MAX_SPINE) {
        char id[64], href[EPUB_NAME_MAX], media[64];
        if (tagAttr(p + as, ae - as, "id", id, sizeof(id)) &&
            tagAttr(p + as, ae - as, "href", href, sizeof(href))) {
          snprintf(items[nItems].id, sizeof(items[nItems].id), "%s", id);
          epubNormPath(base, href, items[nItems].href, EPUB_NAME_MAX);
          items[nItems].present = false;      // filled in by the single presence pass below
          if (cp) {
            kosyncCpPath(base, href, cp[nItems].path, cpScratch);
            cp[nItems].bytes = 0;             // ...and so is this, from the same walk
          }
          if (tagAttr(p + as, ae - as, "media-type", media, sizeof(media))) {
            for (char* q = media; *q; q++) {
              if (*q >= 'A' && *q <= 'Z') {
                *q = (char)(*q + 32);
              }
            }
            snprintf(items[nItems].media, sizeof(items[nItems].media), "%s", media);
          } else {
            items[nItems].media[0] = '\0';
          }
          nItems++;
        }
      } else if (!closing && strcmp(name, "itemref") == 0 &&
                 (nSpineIds < EPUB_MAX_SPINE || (cpIds && nCpIds < EPUB_MAX_SPINE))) {
        /* ⚠ The reading half of this is EXACTLY what it was: the same skip, the same cap on
         * the same counter. The condition above widened only so a book whose reading list
         * is full still gets its CrossPoint list; with the reading list full the old code
         * fell through to branches that cannot match "itemref", so nothing else moves. */
        char idref[64], linear[16];
        if (tagAttr(p + as, ae - as, "idref", idref, sizeof(idref))) {
          bool skip = tagAttr(p + as, ae - as, "linear", linear, sizeof(linear)) &&
                      (linear[0] == 'n' || linear[0] == 'N');
          if (!skip && nSpineIds < EPUB_MAX_SPINE) {
            snprintf(spineIds[nSpineIds], 64, "%s", idref);
            nSpineIds++;
          }
          if (cpIds && nCpIds < EPUB_MAX_SPINE) {      // KOSync: EVERY itemref
            snprintf(cpIds[nCpIds], 64, "%s", idref);
            nCpIds++;
          }
        }
      } else if (!closing && strcmp(name, "title") == 0 && !b->title[0]) {
        collapseCut(p + textStart, textEnd - textStart, b->title, sizeof(b->title), 120);
      } else if (!closing && strcmp(name, "creator") == 0 && !b->author[0]) {
        collapseCut(p + textStart, textEnd - textStart, b->author, sizeof(b->author), 120);
      } else if (!closing && strcmp(name, "identifier") == 0 && !b->identifier[0]) {
        collapseCut(p + textStart, textEnd - textStart, b->identifier,
                    sizeof(b->identifier), 120);
      }
      i = ae < len ? ae + 1 : len;
    }
  }
  ebFree(buf);

  /* ONE walk of the central directory answers "does this exist?" for every manifest item —
   * see PresenceCtx above for the 90-walks-per-open freeze this replaces.
   *
   * 🛑 AND IT MUST FAIL OPEN. zipForEach abandons the walk and returns false on a short read
   * (a failed seek or read on the card), and sharing one walk means a glitch part-way would
   * leave EVERY later item present=false — the spine loop cannot tell that from "not in the
   * archive", so the tail of the book would simply vanish, silently. The old per-item probe
   * degraded far more gently: one bad walk lost one chapter, the other 89 were fine.
   * presenceVisit never returns false, so a false return here means exactly one thing — the
   * walk did not finish — and the safe answer is to trust the manifest instead of a
   * half-read directory. A chapter that then turns out to be missing fails visibly at read
   * time; a chapter deleted here fails silently, and silent is the one this repo keeps
   * paying for. Skipped entirely when there is nothing to mark. */
  /* ⚠ `|| nCpIds > 0` is KOSync's: a book whose itemrefs are ALL linear="no" has an empty
   * reading list (and falls back to zip order below, exactly as before) but a non-empty
   * CrossPoint one that needs sizes. `present` is only ever read through spineIds, which is
   * empty in that case, so the extra walk cannot change what is read. */
  if (nItems > 0 && (nSpineIds > 0 || nCpIds > 0)) {
    PresenceCtx pc = { items, nItems, cp };
    if (!zipForEach(src, presenceVisit, &pc)) {
      EB_TIMING("  epubOpen: central directory walk did not finish - trusting the manifest");
      for (int k = 0; k < nItems; k++) {
        items[k].present = true;
      }
      // KOSync does NOT trust the manifest: a half-read directory is unknown sizes, and an
      // unknown total must never be sent as if it were one (cpSizesKnown stays false).
    } else {
      cpSizesKnown = true;
    }
  }

  for (int s = 0; s < nSpineIds && b->nSpine < EPUB_MAX_SPINE; s++) {
    const OpfItem* it = NULL;
    for (int k = 0; k < nItems; k++) {
      if (strcmp(items[k].id, spineIds[s]) == 0) {
        it = &items[k];
        break;
      }
    }
    if (!it || !it->href[0]) {
      continue;
    }
    if (!it->present) {                 // answered by the single presence pass above
      continue;
    }
    if (it->media[0] && !strstr(it->media, "html")) {
      continue;                      // images, css and fonts are not reading order
    }
    snprintf(b->spine[b->nSpine].name, EPUB_NAME_MAX, "%s", it->href);
    b->spine[b->nSpine].title[0] = '\0';
    b->nSpine++;
  }
  const uint32_t _eManifest = EB_NOW();
  navTitles(b, src, items, nItems);      // must run before the manifest is freed
  const uint32_t _eNav = EB_NOW();
  EB_TIMING("  epubOpen %u ms [fp=%u findContainer=%u readContainer=%u parseContainer=%u"
            " findOpf=%u readOpf=%u manifest=%u nav=%u]",
        (unsigned)(_eNav - _e0), (unsigned)(_eFp - _e0),
        (unsigned)(_eFindC - _eFp), (unsigned)(_eReadC - _eFindC),
        (unsigned)(_ePreFindO - _eReadC), (unsigned)(_eFindO - _ePreFindO),
        (unsigned)(_eReadO - _eFindO), (unsigned)(_eManifest - _eReadO),
        (unsigned)(_eNav - _eManifest));
  EB_TIMING("  epubOpen SD: %u reads, %u ms, %u KB [fp=%u findContainer=%u findOpf=%u] items=%d spine=%d",
        (unsigned)ebSrcReads, (unsigned)ebSrcMs, (unsigned)(ebSrcBytes / 1024),
        (unsigned)_rFp, (unsigned)(_rFindC - _rFp), (unsigned)(_rFindO - _rFindC),
        nItems, b->nSpine);

  if (b->nSpine == 0) {
    FallbackCtx fc = { b };
    zipForEach(src, fallbackVisit, &fc);
  }
  /* The KOSync map needs the FINAL reading spine (fallback included) and the manifest, so the
   * manifest is freed after it rather than before the fallback walk — which never read it. */
  if (b->nSpine > 0) {
    kosyncMapBuild(b, items, cpScratch ? cp : NULL, nItems, cpIds, nCpIds, cpSizesKnown,
                   cpManifestCapped, cpItemrefsCapped);
  }
  ebFree(items);
  ebFree(spineIds);
  ebFree(cpIds);
  ebFree(cpScratch);
  ebFree(cp);
  if (b->nSpine == 0) {
    ebFree(b->spine);
    b->spine = NULL;
    return EPUB_ERR_NO_CHAPTERS;
  }
  if (!b->title[0]) {
    stemOf(displayName, b->title, sizeof(b->title));
  }
  return EPUB_OK;
}

void epubClose(EpubBook* b) {
  if (b->spine) {
    ebFree(b->spine);
    b->spine = NULL;
  }
  if (b->kosync) {
    ebFree(b->kosync);
    b->kosync = NULL;
  }
  b->nSpine = 0;
}

const char* epubStatusText(EpubStatus s) {
  switch (s) {
    case EPUB_OK:               return "ok";
    case EPUB_ERR_NOT_ZIP:      return "cannot open: not a zip";
    case EPUB_ERR_NO_CONTAINER: return "not an EPUB (no container.xml)";
    case EPUB_ERR_NO_ROOTFILE:  return "no OPF rootfile";
    case EPUB_ERR_NO_OPF:       return "missing OPF";
    case EPUB_ERR_NO_CHAPTERS:  return "no readable chapters";
    case EPUB_ERR_MEMORY:       return "out of memory";
  }
  return "unknown";
}

// ---------------------------------------------------------------- chapters
size_t epubChapterText(EpubBook* b, int i, char* buf, size_t cap) {
  return epubChapterTextImages(b, i, buf, cap, NULL, 0, NULL);
}

size_t epubChapterTextImages(EpubBook* b, int i, char* buf, size_t cap,
                             EpubImage* imgs, int maxImgs, int* nImgs) {
  if (nImgs) {
    *nImgs = 0;
  }
  if (i < 0 || i >= b->nSpine || cap < 2) {
    return 0;
  }
  if (b->isText) {
    size_t n = srcRead(b->src, 0, buf, cap - 1);
    buf[n] = '\0';
    /* COVEY does NOT hand back the raw bytes here:
     *     "\n\n".join(b.strip() for b in re.split(r"\n\s*\n", text) if b.strip())
     * Blocks separated by a blank line are stripped, empties dropped, and the whole thing
     * rejoined — so a trailing newline disappears and ragged spacing is regularised. That
     * changes the character offsets, which are exactly what a sync position is expressed
     * in, so "close enough" is not close enough.
     *
     * Rewritten in place: the result can only ever be shorter than the input, and each
     * block is copied to a write cursor that never passes the block it is reading. */
    size_t w = 0, i = 0;
    bool wroteBlock = false;
    while (i < n) {
      size_t blockEnd = n, nextStart = n;
      for (size_t k = i; k < n; k++) {
        if (buf[k] != '\n') {
          continue;
        }
        // re.split's separator is \n\s*\n; \s is greedy with backtracking, so the match
        // ends at the LAST newline of the whitespace run that follows.
        size_t j = k + 1, lastNl = 0;
        bool haveNl = false;
        while (j < n && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\n' ||
                         buf[j] == '\r' || buf[j] == '\f' || buf[j] == '\v')) {
          if (buf[j] == '\n') {
            lastNl = j;
            haveNl = true;
          }
          j++;
        }
        if (haveNl) {
          blockEnd = k;
          nextStart = lastNl + 1;
          break;
        }
        k = j - 1;                                  // that run held no newline; skip past it
      }
      size_t s = i, e = blockEnd;
      while (s < e && (buf[s] == ' ' || buf[s] == '\t' || buf[s] == '\n' ||
                       buf[s] == '\r' || buf[s] == '\f' || buf[s] == '\v')) {
        s++;
      }
      while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t' || buf[e - 1] == '\n' ||
                       buf[e - 1] == '\r' || buf[e - 1] == '\f' || buf[e - 1] == '\v')) {
        e--;
      }
      if (e > s) {
        if (wroteBlock) {
          buf[w++] = '\n';
          buf[w++] = '\n';
        }
        memmove(buf + w, buf + s, e - s);
        w += e - s;
        wroteBlock = true;
      }
      i = nextStart;
    }
    buf[w] = '\0';
    return w;
  }
  ZipEntry e;
  if (!zipFind(b->src, b->spine[i].name, &e)) {
    snprintf(buf, cap, "[chapter unreadable]");
    return strlen(buf);
  }
  if (e.uncompSize > EPUB_MAX_DOC) {
    snprintf(buf, cap, "[chapter too large to display]");
    return strlen(buf);
  }
  uint8_t* raw = (uint8_t*)ebAlloc(EPUB_MAX_DOC);
  if (!raw) {
    snprintf(buf, cap, "[chapter unreadable]");
    return strlen(buf);
  }
  int n = zipRead(b->src, &e, raw, EPUB_MAX_DOC);
  size_t out = 0;
  if (n > 0) {
    // An <img src> is relative to the CHAPTER's directory, not the OPF's and not the root.
    char here[EPUB_NAME_MAX] = {0};
    const char* slash = strrchr(b->spine[i].name, '/');
    if (slash) {
      size_t hl = (size_t)(slash - b->spine[i].name);
      memcpy(here, b->spine[i].name, hl);
      here[hl] = '\0';
    }
    out = extractTextImpl((const char*)raw, (size_t)n, buf, cap, here, imgs, maxImgs, nImgs);
  } else {
    snprintf(buf, cap, "[chapter unreadable]");
    out = strlen(buf);
  }
  ebFree(raw);
  return out;
}

size_t epubEntrySize(EpubBook* b, const char* name) {
  ZipEntry e;
  if (!b || !name || !name[0] || !zipFind(b->src, name, &e)) {
    return 0;
  }
  return e.uncompSize;
}

size_t epubReadEntry(EpubBook* b, const char* name, void* buf, size_t cap) {
  ZipEntry e;
  if (!b || !name || !name[0] || !zipFind(b->src, name, &e)) {
    return 0;
  }
  int n = zipRead(b->src, &e, (uint8_t*)buf, cap);
  return n > 0 ? (size_t)n : 0;
}

size_t epubReadEntryPrefix(EpubBook* b, const char* name, void* buf, size_t cap) {
  ZipEntry e;
  if (!b || !name || !name[0] || !zipFind(b->src, name, &e)) {
    return 0;
  }
  int n = zipReadPrefix(b->src, &e, (uint8_t*)buf, cap);
  return n > 0 ? (size_t)n : 0;
}

bool epubImageSize(const void* data, size_t len, uint16_t* w, uint16_t* h) {
  return epubImageInfo(data, len, w, h, NULL);
}

bool epubImageInfo(const void* data, size_t len, uint16_t* w, uint16_t* h, uint8_t* comps) {
  if (comps) {
    *comps = 0;
  }
  const uint8_t* p = (const uint8_t*)data;
  if (!p || len < 24) {
    return false;
  }
  // PNG: an IHDR chunk always comes first, at a fixed offset.
  static const uint8_t PNG_SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
  if (memcmp(p, PNG_SIG, 8) == 0 && memcmp(p + 12, "IHDR", 4) == 0) {
    uint32_t pw = ((uint32_t)p[16] << 24) | ((uint32_t)p[17] << 16) |
                  ((uint32_t)p[18] << 8) | p[19];
    uint32_t ph = ((uint32_t)p[20] << 24) | ((uint32_t)p[21] << 16) |
                  ((uint32_t)p[22] << 8) | p[23];
    if (!pw || !ph || pw > 0xFFFF || ph > 0xFFFF) {
      return false;
    }
    *w = (uint16_t)pw;
    *h = (uint16_t)ph;
    return true;
  }
  // JPEG: walk the marker segments to a start-of-frame, which carries the dimensions.
  if (!(p[0] == 0xFF && p[1] == 0xD8)) {
    return false;
  }
  size_t i = 2;
  while (i + 3 < len) {
    if (p[i] != 0xFF) {
      i++;                                  // fill byte or padding: resync
      continue;
    }
    uint8_t m = p[i + 1];
    if (m == 0xFF) {
      i++;
      continue;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      i += 2;                               // standalone markers carry no length
      continue;
    }
    if (m == 0xDA || m == 0xD9) {
      break;                                // start of scan / end: no frame header found
    }
    if (i + 3 >= len) {
      break;
    }
    size_t seg = ((size_t)p[i + 2] << 8) | p[i + 3];
    // SOF0..SOF15, except the DHT/JPG/DAC markers that share the range.
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (i + 9 >= len) {
        break;
      }
      *h = (uint16_t)(((uint16_t)p[i + 5] << 8) | p[i + 6]);
      *w = (uint16_t)(((uint16_t)p[i + 7] << 8) | p[i + 8]);
      if (comps) {
        *comps = p[i + 9];              // 1 = greyscale (the ROM decoder will refuse it)
      }
      return (*w != 0 && *h != 0);
    }
    if (seg < 2) {
      break;
    }
    i += 2 + seg;
  }
  return false;
}

size_t epubChapterLen(EpubBook* b, int i) {
  char* tmp = (char*)ebAlloc(EPUB_MAX_DOC);
  if (!tmp) {
    return 0;
  }
  size_t n = epubChapterText(b, i, tmp, EPUB_MAX_DOC);
  ebFree(tmp);
  return n;
}

double epubFraction(EpubBook* b, int spine, uint32_t offset) {
  int n = b->nSpine > 0 ? b->nSpine : 1;
  size_t chars = epubChapterLen(b, spine);
  if (chars == 0) {
    chars = 1;
  }
  double within = (double)offset / (double)chars;
  if (within < 0.0) within = 0.0;
  if (within > 1.0) within = 1.0;
  int sp = spine < 0 ? 0 : (spine > n - 1 ? n - 1 : spine);
  double f = ((double)sp + within) / (double)n;
  if (f < 0.0) f = 0.0;
  if (f > 1.0) f = 1.0;
  return f;
}

void epubLocate(EpubBook* b, double fraction, int* spineOut, uint32_t* offsetOut) {
  int n = b->nSpine > 0 ? b->nSpine : 1;
  double f = fraction;
  if (f < 0.0) f = 0.0;
  if (f > 0.999999) f = 0.999999;
  int spine = (int)(f * n);
  if (spine > n - 1) {
    spine = n - 1;
  }
  double within = f * n - spine;
  size_t chars = epubChapterLen(b, spine);
  if (spineOut) {
    *spineOut = spine;
  }
  if (offsetOut) {
    *offsetOut = (uint32_t)(within * (double)chars);
  }
}

const char* epubChapterTitle(const EpubBook* b, int i, char* buf, size_t cap) {
  if (i < 0 || i >= b->nSpine) {
    buf[0] = '\0';
    return buf;
  }
  if (b->spine[i].title[0]) {
    snprintf(buf, cap, "%s", b->spine[i].title);
  } else {
    snprintf(buf, cap, "Chapter %d", i + 1);
  }
  return buf;
}

// ---------------------------------------------------------------- KOSync (see epub_parse.h)
bool epubKosyncPartialMd5(EpubSource* src, char out[EPUB_KOSYNC_ID_CHARS]) {
  /* KOReader's util.partialMD5, which CrossPoint's KOReaderDocumentId copies: a 1 KB sample
   * at 0 and at 1 KB, 4 KB, 16 KB ... 1 GB. It identifies the exact BYTES, so a book re-saved
   * by any tool on any device becomes a different document — which is why the filename id
   * below exists too.
   *
   * ⚠ A SHORT READ THAT IS NOT END-OF-FILE FAILS THE WHOLE ID. srcRead() clips at the file's
   * end (that IS KOReader's rule for a short last chunk), so anything shorter than the clip
   * is the card refusing a read — and hashing what did arrive would produce a confident,
   * WRONG id that matches nothing, silently. No id is the honest answer. */
  out[0] = '\0';
  if (!src || !src->read) {
    return false;
  }
  uint8_t* buf = (uint8_t*)ksAlloc(1024);     // not on the 8 KB loop-task stack
  if (!buf) {
    return false;
  }
  BsMd5 c;
  bsMd5Init(&c);
  bool ok = true;
  for (int i = -1; i <= 10; i++) {
    const uint64_t off = i < 0 ? 0 : ((uint64_t)1024 << (2 * i));
    if (off >= src->size) {
      break;
    }
    const uint64_t left = src->size - off;
    const size_t want = left < 1024 ? (size_t)left : 1024;
    if (srcRead(src, off, buf, want) != want) {
      ok = false;
      break;
    }
    bsMd5Update(&c, buf, want);
  }
  ebFree(buf);
  if (!ok) {
    return false;
  }
  uint8_t d[16];
  bsMd5Final(&c, d);
  bsHex(d, 16, out);
  return true;
}

void epubKosyncFilenameMd5(const char* basename, char out[EPUB_KOSYNC_ID_CHARS]) {
  const char* n = basename ? basename : "";
  bsMd5Hex(n, strlen(n), out);
}

bool epubKosyncIds(EpubSource* src, const char* basename,
                   char partial[EPUB_KOSYNC_ID_CHARS], char byName[EPUB_KOSYNC_ID_CHARS]) {
  epubKosyncFilenameMd5(basename, byName);
  return epubKosyncPartialMd5(src, partial);
}

uint32_t epubKosyncTotal(const EpubKosyncMap* m) {
  if (!m || !m->sizesKnown || m->refused || m->nCp <= 0 || m->nRead <= 0) {
    return 0;
  }
  return m->cum[m->nCp - 1];
}

const char* epubKosyncWhyNot(const EpubKosyncMap* m) {
  if (!m) {
    return "no KOSync data (memory)";
  }
  if (m->refused == EPUB_KOSYNC_TOO_MANY_ITEMS) {
    return "over 512 items - the X4 would count it differently";
  }
  if (m->refused == EPUB_KOSYNC_TEXT_TOO_BIG) {
    return "a .txt too big to show whole here";
  }
  if (!m->sizesKnown) {
    return "its zip could not be read to the end";
  }
  if (m->nCp <= 0 || m->nRead <= 0 || !m->cum[m->nCp - 1]) {
    return "no spine for the X4 to count";
  }
  return "";
}

bool epubKosyncPercent(const EpubKosyncMap* m, int readIdx, double within, double* pct) {
  const uint32_t total = epubKosyncTotal(m);
  if (!total || readIdx < 0 || readIdx >= m->nRead || readIdx >= EPUB_MAX_SPINE) {
    return false;
  }
  const int c = m->readToCp[readIdx];
  if (c < 0) {
    return false;                 // a chapter CrossPoint does not list: refuse, never guess
  }
  if (!(within >= 0.0)) {         // also catches NaN
    within = 0.0;
  }
  if (within > 1.0) {
    within = 1.0;
  }
  const uint32_t prev = c > 0 ? m->cum[c - 1] : 0;
  const uint32_t size = m->cum[c] - prev;
  // Written in the generator's order of operations, so the result is the same double.
  *pct = ((double)prev + within * (double)size) / (double)total;
  return true;
}

bool epubKosyncLocate(const EpubKosyncMap* m, double pct, int* readIdx, double* readWithin,
                      int* cpIdx, double* cpWithin) {
  const uint32_t total = epubKosyncTotal(m);
  if (!total) {
    return false;
  }
  double p = pct;
  if (!(p >= 0.0)) {
    p = 0.0;
  }
  if (p > 1.0) {
    p = 1.0;
  }
  const double target = p * (double)total;
  /* `>=`, so a percentage exactly on a boundary picks the EARLIER item — the end of the
   * chapter it closes, not the start of the next. Every device uses the same rule, which is
   * what makes a boundary round-trip land in the same place everywhere. */
  int c = m->nCp - 1;
  for (int i = 0; i < m->nCp; i++) {
    if ((double)m->cum[i] >= target) {
      c = i;
      break;
    }
  }
  const uint32_t prev = c > 0 ? m->cum[c - 1] : 0;
  const uint32_t size = m->cum[c] - prev;
  double w = size == 0 ? 0.0 : (target - (double)prev) / (double)size;
  if (w < 0.0) {
    w = 0.0;
  }
  if (w > 1.0) {
    w = 1.0;
  }
  if (cpIdx) {
    *cpIdx = c;
  }
  if (cpWithin) {
    *cpWithin = w;
  }
  int r = m->cpToRead[c];
  double rw = w;
  if (r < 0) {
    /* It fell in something we do not read: a linear="no" cover, an image in the spine, a
     * file missing from the zip. The next thing we DO read, from its start; failing that,
     * the end of the book. */
    rw = 0.0;
    for (int j = c + 1; j < m->nCp; j++) {
      if (m->cpToRead[j] >= 0) {
        r = m->cpToRead[j];
        break;
      }
    }
    if (r < 0) {
      r = m->nRead - 1;
      rw = 1.0;
    }
  }
  if (readIdx) {
    *readIdx = r;
  }
  if (readWithin) {
    *readWithin = rw;
  }
  return true;
}

static bool kosyncOversized(const EpubKosyncMap* m, int readIdx) {
  if (!m || readIdx < 0 || readIdx >= m->nRead || readIdx >= EPUB_MAX_SPINE) {
    return false;
  }
  const int c = m->readToCp[readIdx];
  if (c < 0) {
    return false;
  }
  const uint32_t prev = c > 0 ? m->cum[c - 1] : 0;
  return (m->cum[c] - prev) > (uint32_t)EPUB_MAX_DOC;   // shown as a placeholder here
}

bool epubKosyncPercentAt(const EpubBook* b, int spine, uint32_t offset, size_t chapLen,
                         double* pct) {
  const EpubKosyncMap* m = b ? b->kosync : NULL;
  if (!m) {
    return false;
  }
  double within = chapLen ? (double)offset / (double)chapLen : 0.0;
  if (kosyncOversized(m, spine)) {
    within = 0.0;                 // "[chapter too large to display]": the offset means nothing
  }
  return epubKosyncPercent(m, spine, within, pct);
}

bool epubKosyncLocateOffset(EpubBook* b, double pct, int* spine, uint32_t* offset) {
  int r = 0;
  double w = 0.0;
  if (!b || !epubKosyncLocate(b->kosync, pct, &r, &w, NULL, NULL)) {
    return false;
  }
  /* The `>=` rule puts an exact chapter START at the END of the chapter before (within 1) —
   * the same place in the text, but a reader would open on the last page of the wrong
   * chapter. For a POSITION, prefer the start of the next one. (epubKosyncLocate itself keeps
   * the rule exactly: every device must agree on which ITEM a boundary belongs to.) */
  if (w >= 1.0 && r + 1 < b->nSpine) {
    r++;
    w = 0.0;
  }
  const size_t chars = kosyncOversized(b->kosync, r) ? 0 : epubChapterLen(b, r);
  if (spine) {
    *spine = r;
  }
  if (offset) {
    *offset = (uint32_t)(w * (double)chars);
  }
  return true;
}

static int kosyncHexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

size_t epubKosyncDecodePath(const char* in, char* out, size_t cap) {
  /* FsHelpers::decodeUriEscapes: "%XX" (either case) becomes that byte; a '%' without two hex
   * digits after it stays literal. Safe with out == in: output never overtakes input. */
  const size_t len = strlen(in);
  size_t n = 0;
  size_t i = 0;
  while (i < len && n + 1 < cap) {
    if (in[i] == '%' && i + 2 < len && kosyncHexVal(in[i + 1]) >= 0 &&
        kosyncHexVal(in[i + 2]) >= 0) {
      out[n++] = (char)(kosyncHexVal(in[i + 1]) * 16 + kosyncHexVal(in[i + 2]));
      i += 3;
      continue;
    }
    out[n++] = in[i++];
  }
  out[n] = '\0';
  return n;
}

size_t epubKosyncNormPath(const char* in, char* out, size_t cap) {
  /* FsHelpers::normalisePath: split on '/', drop empty components, '..' pops the previous
   * one (and is dropped at the top), and — unlike epubNormPath — '.' is KEPT as a component.
   * Safe with out == in: the write position never passes the read position. */
  const size_t len = strlen(in);
  size_t n = 0;
  size_t i = 0;
  while (i < len) {
    while (i < len && in[i] == '/') {
      i++;
    }
    if (i >= len) {
      break;
    }
    const size_t s = i;
    while (i < len && in[i] != '/') {
      i++;
    }
    size_t cl = i - s;
    if (cl == 2 && in[s] == '.' && in[s + 1] == '.') {
      while (n > 0 && out[n - 1] != '/') {
        n--;
      }
      if (n > 0) {
        n--;                      // and the separator before it
      }
      continue;
    }
    if (n > 0) {
      if (n + 1 >= cap) {
        break;
      }
      out[n++] = '/';
    }
    if (n + cl >= cap) {
      cl = cap - 1 - n;
    }
    memmove(out + n, in + s, cl);
    n += cl;
  }
  out[n] = '\0';
  return n;
}
