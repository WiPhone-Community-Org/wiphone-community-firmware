/*
 * test_epub.cpp — proves the WiPhone's EPUB parser agrees with COVEY's epub.py.
 *
 * The fixtures in tests/fixtures/ are real EPUB zips built by tools/gen_epub_vectors.py, and
 * every expected value was produced by running COVEY's parser over those same bytes. What
 * matters most here is the IDS: they are what decide whether a sync packet is about the same
 * book, and a mismatch shows up on the devices as "sync just doesn't work".
 */
#include "../WiPhone/epub_parse.h"
#include "../WiPhone/book_hash.h"
#include "vectors_epub.h"

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

static void eqU32(unsigned long got, unsigned long want, const char* what) {
  if (got == want) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s  want=%lu got=%lu\n", g_group, what, want, got);
}

static void eqStr(const char* got, const char* want, const char* what) {
  if (got && want && strcmp(got, want) == 0) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s\n", g_group, what);
  printf("        want: [%s]\n", want ? want : "(null)");
  printf("        got:  [%s]\n", got ? got : "(null)");
}

// ---------------------------------------------------------------- FILE*-backed source
static size_t fileRead(void* ctx, uint64_t off, void* buf, size_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) {
    return 0;
  }
  return fread(buf, 1, len, f);
}

static bool openSource(const char* path, EpubSource* s, FILE** fOut) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  s->ctx = f;
  s->size = (uint64_t)sz;
  s->read = fileRead;
  *fOut = f;
  return true;
}

// ---------------------------------------------------------------- units
static void testSlug() {
  group("_slug vs COVEY");
  for (int i = 0; i < EP_SLUG_VEC_N; i++) {
    char out[128];
    epubSlug(EP_SLUG_VECS[i].in, out, sizeof(out));
    eqStr(out, EP_SLUG_VECS[i].want, EP_SLUG_VECS[i].in[0] ? EP_SLUG_VECS[i].in : "(empty)");
  }
}

static void testNorm() {
  group("_norm (the href trap) vs COVEY");
  for (int i = 0; i < EP_NORM_VEC_N; i++) {
    char out[256];
    epubNormPath(EP_NORM_VECS[i].base, EP_NORM_VECS[i].href, out, sizeof(out));
    char what[300];
    snprintf(what, sizeof(what), "base=[%s] href=[%s]",
             EP_NORM_VECS[i].base, EP_NORM_VECS[i].href);
    eqStr(out, EP_NORM_VECS[i].want, what);
  }
}

static void testHtml() {
  group("XHTML -> text vs COVEY");
  for (int i = 0; i < EP_HTML_VEC_N; i++) {
    char out[4096];
    epubExtractText(EP_HTML_VECS[i].in, strlen(EP_HTML_VECS[i].in), out, sizeof(out));
    eqStr(out, EP_HTML_VECS[i].want, EP_HTML_VECS[i].in);
  }
}

// ---------------------------------------------------------------- whole books
static void testBooks() {
  group("open_book vs COVEY (real EPUB fixtures)");
  for (int i = 0; i < EP_BOOK_VEC_N; i++) {
    const EpBookVec* v = &EP_BOOK_VECS[i];
    char path[512];
    snprintf(path, sizeof(path), "tests/fixtures/%s", v->file);

    EpubSource src;
    FILE* f = NULL;
    if (!openSource(path, &src, &f)) {
      ok(false, v->label);
      continue;
    }
    EpubBook b;
    EpubStatus st = epubOpen(&b, &src, v->displayName, v->isText != 0);
    if (st != EPUB_OK) {
      printf("  \033[31mFAIL\033[0m %s :: %s opened as '%s'\n",
             g_group, v->label, epubStatusText(st));
      g_fail++;
      fclose(f);
      continue;
    }
    g_pass++;

    eqStr(b.title, v->title, v->label);
    eqStr(b.author, v->author, v->label);
    eqStr(b.identifier, v->identifier, v->label);

    // The spine: names must resolve exactly, or chapters silently vanish.
    if (b.nSpine != v->nSpine) {
      printf("  \033[31mFAIL\033[0m %s :: %s spine count want=%d got=%d\n",
             g_group, v->label, v->nSpine, b.nSpine);
      g_fail++;
    } else {
      g_pass++;
      for (int k = 0; k < v->nSpine; k++) {
        eqStr(b.spine[k].name, v->spine[k], v->label);
        /* Chapter titles, from the EPUB3 nav document or the EPUB2 NCX. Cosmetic on the wire
         * — only the spine INDEX travels — but the two devices should still call a chapter
         * the same thing, and without this a 90-chapter book is a list of "Chapter N".
         * The fixtures cover both forms: epub3-nav.epub and epub2-subdir.epub. */
        char t[EPUB_CH_TITLE_MAX];
        epubChapterTitle(&b, k, t, sizeof(t));
        eqStr(t, v->chapTitle[k], v->label);
      }
    }

    /* Pictures. WiPhone-only — COVEY has no screen concept of them — so these are behaviour
     * assertions, not interop vectors. The invariant that IS interop-critical is checked for
     * every fixture right below: asking for the images must not change the text by one byte,
     * because a reading position is a byte offset into it. */
    {
      char plain[8192], withImgs[8192];
      size_t lp = epubChapterText(&b, 0, plain, sizeof(plain));
      EpubImage imgs[EPUB_MAX_IMAGES];
      int nImgs = -1;
      size_t li = epubChapterTextImages(&b, 0, withImgs, sizeof(withImgs),
                                        imgs, EPUB_MAX_IMAGES, &nImgs);
      char what[96];
      snprintf(what, sizeof(what), "%s: capturing images leaves the text byte-identical",
               v->label);
      ok(lp == li && memcmp(plain, withImgs, lp) == 0, what);

      if (strcmp(v->label, "epub2_subdir") == 0) {
        eqU32((unsigned long)nImgs, 1, "epub2_subdir: one picture in chapter 1");
        if (nImgs == 1) {
          // "../img/pic.jpg" from OEBPS/text/ resolves up a level, not to the zip root.
          eqStr(imgs[0].name, "OEBPS/img/pic.jpg", "picture src resolves against the chapter");
          // The <h1> before it has already been committed ("One"), the <p> after it has not.
          eqU32(imgs[0].off, 3, "and it anchors between the heading and the paragraph");

          uint8_t hdr[512];
          size_t got = epubReadEntryPrefix(&b, imgs[0].name, hdr, sizeof(hdr));
          ok(got > 0, "the picture can be read out of the zip");
          uint16_t iw = 0, ih = 0;
          ok(epubImageSize(hdr, got, &iw, &ih), "its header parses");
          eqU32(iw, 40, "picture width");
          eqU32(ih, 30, "picture height");
        }
      } else {
        char none[80];
        snprintf(none, sizeof(none), "%s: no pictures reported", v->label);
        ok(nImgs == 0, none);
      }
    }

    // THE ONE THAT MATTERS: ids decide whether a sync packet is about this book.
    char ids[3][EPUB_ID_MAX];
    int n = epubIds(&b, ids);
    if (n != v->nIds) {
      printf("  \033[31mFAIL\033[0m %s :: %s id count want=%d got=%d\n",
             g_group, v->label, v->nIds, n);
      g_fail++;
    } else {
      g_pass++;
      for (int k = 0; k < n; k++) {
        eqStr(ids[k], v->ids[k], v->label);
      }
    }

    char* text = (char*)malloc(64 * 1024);
    epubChapterText(&b, 0, text, 64 * 1024);
    eqStr(text, v->chapter0, v->label);
    free(text);

    epubClose(&b);
    fclose(f);
  }
}

static void testFractionLocate() {
  group("fraction / locate are inverses");
  // This is what makes a position from a foreign reader land somewhere sane, so it is
  // checked as a round trip rather than against a single expected number.
  for (int i = 0; i < EP_BOOK_VEC_N; i++) {
    const EpBookVec* v = &EP_BOOK_VECS[i];
    char path[512];
    snprintf(path, sizeof(path), "tests/fixtures/%s", v->file);
    EpubSource src;
    FILE* f = NULL;
    if (!openSource(path, &src, &f)) {
      continue;
    }
    EpubBook b;
    if (epubOpen(&b, &src, v->displayName, v->isText != 0) != EPUB_OK) {
      fclose(f);
      continue;
    }
    for (int sp = 0; sp < b.nSpine; sp++) {
      size_t len = epubChapterLen(&b, sp);
      uint32_t off = (uint32_t)(len / 2);
      double fr = epubFraction(&b, sp, off);
      ok(fr >= 0.0 && fr <= 1.0, "fraction in range");
      int gotSp = -1;
      uint32_t gotOff = 0;
      epubLocate(&b, fr, &gotSp, &gotOff);
      char what[200];
      snprintf(what, sizeof(what), "%s spine %d round trips", v->label, sp);
      ok(gotSp == sp, what);
      // Offsets are only approximately recoverable — a quantised fraction cannot carry an
      // exact character index. Landing within a few characters is the contract.
      long delta = (long)gotOff - (long)off;
      if (delta < 0) {
        delta = -delta;
      }
      snprintf(what, sizeof(what), "%s spine %d offset within tolerance (%ld)",
               v->label, sp, delta);
      ok(delta <= 4, what);
    }
    // COVEY's own vector: locate(0.5) must pick the same spine item.
    int mid = -1;
    epubLocate(&b, 0.5, &mid, NULL);
    char what[200];
    snprintf(what, sizeof(what), "%s locate(0.5) spine", v->label);
    ok(mid == v->locateSpine, what);

    epubClose(&b);
    fclose(f);
  }
}

static void testFingerprint() {
  group("_fingerprint vs COVEY");
  // The fp: id in each book vector was computed by COVEY over the same file, so a match
  // here proves the size+ends hashing agrees byte for byte.
  for (int i = 0; i < EP_BOOK_VEC_N; i++) {
    const EpBookVec* v = &EP_BOOK_VECS[i];
    char path[512];
    snprintf(path, sizeof(path), "tests/fixtures/%s", v->file);
    EpubSource src;
    FILE* f = NULL;
    if (!openSource(path, &src, &f)) {
      ok(false, v->label);
      continue;
    }
    char fp[17];
    ok(epubFingerprint(&src, fp), v->label);
    char want[32];
    snprintf(want, sizeof(want), "fp:%s", fp);
    eqStr(want, v->ids[v->nIds - 1], v->label);      // fp: is always the LAST id
    fclose(f);
  }
}

static void testBadInput() {
  group("unreadable files fail politely");
  // A hostile or truncated file must produce an error, never a crash or a hang.
  const char* junk = "tests/fixtures/_junk.bin";
  FILE* w = fopen(junk, "wb");
  for (int i = 0; i < 5000; i++) {
    fputc(i * 7 & 0xFF, w);
  }
  fclose(w);
  EpubSource src;
  FILE* f = NULL;
  ok(openSource(junk, &src, &f), "opened junk");
  EpubBook b;
  EpubStatus st = epubOpen(&b, &src, "_junk.bin", false);
  ok(st != EPUB_OK, "random bytes are not an EPUB");
  ok(strlen(epubStatusText(st)) > 0, "error has a message");
  epubClose(&b);
  fclose(f);
  remove(junk);

  // A zero-length file.
  const char* empty = "tests/fixtures/_empty.bin";
  w = fopen(empty, "wb");
  fclose(w);
  ok(openSource(empty, &src, &f), "opened empty");
  st = epubOpen(&b, &src, "_empty.bin", false);
  ok(st != EPUB_OK, "empty file is not an EPUB");
  epubClose(&b);
  fclose(f);
  remove(empty);
}

/* epubImageSize on its own. It walks JPEG marker segments rather than assuming the frame
 * header comes first, which is the part that is easy to get subtly wrong — and a wrong size
 * means an inline picture reserves the wrong number of rows on the page. */
static void testImageSize() {
  group("picture dimensions from a file header");
  uint16_t w = 0, h = 0;

  // JPEG with an APP0 segment in front of the SOF0, as every real JFIF file has.
  const uint8_t jpg[] = {
    0xFF, 0xD8,
    0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
    0xFF, 0xC0, 0x00, 0x11, 0x08, 0x02, 0x58, 0x01, 0x90, 3, 1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1,
    0xFF, 0xD9
  };
  ok(epubImageSize(jpg, sizeof(jpg), &w, &h), "a JFIF header parses");
  eqU32(w, 400, "jpeg width comes from the SOF, not the APP0");
  eqU32(h, 600, "jpeg height");

  // Progressive JPEGs use SOF2; the dimensions live in the same place.
  uint8_t prog[sizeof(jpg)];
  memcpy(prog, jpg, sizeof(jpg));
  prog[21] = 0xC2;
  w = h = 0;
  ok(epubImageSize(prog, sizeof(prog), &w, &h) && w == 400, "SOF2 (progressive) too");

  // 0xC4 is a Huffman table, NOT a frame — mistaking it gives garbage dimensions.
  uint8_t dht[sizeof(jpg)];
  memcpy(dht, jpg, sizeof(dht));
  dht[21] = 0xC4;
  w = h = 0;
  ok(!epubImageSize(dht, sizeof(dht), &w, &h), "a DHT in the SOF range is not a frame");

  const uint8_t png[] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n',
    0, 0, 0, 13, 'I', 'H', 'D', 'R',
    0, 0, 0x01, 0x2C, 0, 0, 0x00, 0xC8, 8, 6, 0, 0, 0
  };
  w = h = 0;
  ok(epubImageSize(png, sizeof(png), &w, &h), "a PNG header parses");
  eqU32(w, 300, "png width");
  eqU32(h, 200, "png height");

  const uint8_t junk[32] = { 0 };
  ok(!epubImageSize(junk, sizeof(junk), &w, &h), "neither: refused");
  ok(!epubImageSize(jpg, 4, &w, &h), "truncated: refused, not read off the end");
}

int main() {
  printf("\033[1mepub — WiPhone <-> COVEY parser agreement\033[0m\n");
  testSlug();
  testNorm();
  testHtml();
  testBooks();
  testImageSize();
  testFingerprint();
  testFractionLocate();
  testBadInput();
  printf("\n%s%d passed, %d failed\033[0m\n",
         g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
