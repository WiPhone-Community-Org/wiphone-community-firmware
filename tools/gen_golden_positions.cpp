/*
 * gen_golden_positions.cpp — pin what the reader does with every fixture BEFORE a parser change.
 *
 * 🛑 NOT A VECTOR GENERATOR TO RE-RUN AT WILL. Its whole value is that it was run ONCE against
 * the parser as it stood before KOSync touched epub_parse.cpp (git 1de15a6, 0.9.78), and its
 * output — tests/golden_positions.h — is what test_kosync.cpp holds the CURRENT parser to.
 * Regenerating it from a later revision would make the "before" equal the "after" by
 * construction and prove nothing. {user}'s rule for this feature was that nobody's saved place
 * may move when they update; this file is how that is checked rather than promised.
 *
 * What it pins, per fixture: the reading spine (count and every entry name), every chapter's
 * extracted length (which decides the empty-chapter skip at open and every offset's meaning),
 * the ids the store matches on, epubFraction at a spread of offsets, epubLocate at a spread of
 * fractions, and a CBSTORE1 positions file holding one saved place per book.
 *
 * Build it against the OLD sources, e.g.:
 *   mkdir -p /tmp/g && for f in epub_parse.cpp epub_parse.h book_hash.cpp book_hash.h \
 *       bookstore.cpp bookstore.h booksync.cpp booksync.h html_entities.h; do \
 *     git show 1de15a6:WiPhone/$f > /tmp/g/$f; done
 *   c++ -std=c++11 -O1 -I/tmp/g -o /tmp/g/gen tools/gen_golden_positions.cpp \
 *       /tmp/g/epub_parse.cpp /tmp/g/book_hash.cpp /tmp/g/bookstore.cpp /tmp/g/booksync.cpp -lz
 *   /tmp/g/gen tests/fixtures > tests/golden_positions.h
 */
#include "epub_parse.h"
#include "bookstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t fileRead(void* ctx, uint64_t off, void* buf, size_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) {
    return 0;
  }
  return fread(buf, 1, len, f);
}

static void cstr(const char* s) {
  putchar('"');
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    if (*p == '"' || *p == '\\') {
      printf("\\%c", *p);
    } else if (*p == '\n') {
      printf("\\n");
    } else if (*p < 0x20 || *p >= 0x7F) {
      printf("\\x%02x\"\"", *p);
    } else {
      putchar(*p);
    }
  }
  putchar('"');
}

// Every fixture the reader can open, in a fixed order (the store blob below depends on it).
static const char* FIXTURES[] = {
  "empty-spine.epub", "epub2-subdir.epub", "epub3-nav.epub", "kosync-mixed.epub",
  "kosync-plain.epub", "my-notes.txt", "no-metadata.epub",
};
#define NFIX ((int)(sizeof(FIXTURES) / sizeof(FIXTURES[0])))

static const double FRACS[] = { 0.0, 0.1, 0.25, 0.3333, 0.5, 0.6180339887, 0.75, 0.9, 0.999, 1.0 };
#define NFRACS ((int)(sizeof(FRACS) / sizeof(FRACS[0])))

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : "tests/fixtures";
  static BookStore store;
  store.init();

  printf("// GENERATED ONCE by tools/gen_golden_positions.cpp from the PRE-KOSync parser\n");
  printf("// (git 1de15a6, 0.9.78). 🛑 Do not regenerate from a later revision: this is the\n");
  printf("// \"before\" that test_kosync.cpp proves the current parser still matches.\n");
  printf("#pragma once\n#include <stdint.h>\n");
  printf("typedef struct { int spine; uint32_t offset; double fraction; } GpFrac;\n");
  printf("typedef struct { double fraction; int spine; uint32_t offset; } GpLoc;\n");
  printf("typedef struct {\n  const char* file; int status; int nSpine; const char* const* names;\n"
         "  const uint32_t* chapLen; const char* const* ids; int nIds;\n"
         "  const GpFrac* fr; int nFr; const GpLoc* loc; int nLoc;\n"
         "  int savedSpine; uint32_t savedOffset; double savedFraction;\n} GpBook;\n");

  for (int k = 0; k < NFIX; k++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, FIXTURES[k]);
    FILE* f = fopen(path, "rb");
    if (!f) {
      fprintf(stderr, "missing %s\n", path);
      return 1;
    }
    fseek(f, 0, SEEK_END);
    EpubSource src = { f, (uint64_t)ftell(f), fileRead };
    const bool txt = strstr(FIXTURES[k], ".txt") != NULL;
    EpubBook b;
    EpubStatus st = epubOpen(&b, &src, FIXTURES[k], txt);
    char ids[3][EPUB_ID_MAX];
    int nIds = st == EPUB_OK ? epubIds(&b, ids) : 0;

    printf("static const char* const GP_B%d_NAMES[] = { ", k);
    for (int i = 0; i < b.nSpine; i++) {
      cstr(b.spine[i].name);
      printf(", ");
    }
    printf("\"\" };\n");
    printf("static const uint32_t GP_B%d_LEN[] = { ", k);
    uint32_t lens[EPUB_MAX_SPINE];
    for (int i = 0; i < b.nSpine; i++) {
      lens[i] = (uint32_t)epubChapterLen(&b, i);
      printf("%uu, ", (unsigned)lens[i]);
    }
    printf("0u };\n");
    printf("static const char* const GP_B%d_IDS[] = { ", k);
    for (int i = 0; i < nIds; i++) {
      cstr(ids[i]);
      printf(", ");
    }
    printf("\"\" };\n");

    int nFr = 0;
    printf("static const GpFrac GP_B%d_FR[] = {\n", k);
    for (int i = 0; i < b.nSpine; i++) {
      const uint32_t L = lens[i];
      const uint32_t offs[] = { 0, L / 3, L / 2, L ? L - 1 : 0, L, L + 7 };
      for (int j = 0; j < 6; j++) {
        printf("  { %d, %uu, %.17g },\n", i, (unsigned)offs[j], epubFraction(&b, i, offs[j]));
        nFr++;
      }
    }
    printf("  { 0, 0u, 0 }\n};\n");

    int nLoc = 0;
    printf("static const GpLoc GP_B%d_LOC[] = {\n", k);
    for (int j = 0; j < NFRACS; j++) {
      int sp = -1;
      uint32_t off = 0;
      epubLocate(&b, FRACS[j], &sp, &off);
      printf("  { %.17g, %d, %uu },\n", FRACS[j], sp, (unsigned)off);
      nLoc++;
    }
    for (int i = 0; i < b.nSpine; i++) {
      const double fs[] = { (double)i / b.nSpine, (i + 0.5) / b.nSpine };
      for (int j = 0; j < 2; j++) {
        int sp = -1;
        uint32_t off = 0;
        epubLocate(&b, fs[j], &sp, &off);
        printf("  { %.17g, %d, %uu },\n", fs[j], sp, (unsigned)off);
        nLoc++;
      }
    }
    printf("  { 0, 0, 0u }\n};\n");

    // One saved place per book: the middle of the middle chapter, as the reader stores it.
    int sSp = b.nSpine > 0 ? b.nSpine / 2 : 0;
    uint32_t sOff = b.nSpine > 0 ? lens[sSp] / 2 : 0;
    double sFr = b.nSpine > 0 ? epubFraction(&b, sSp, sOff) : 0.0;
    if (nIds > 0) {
      const char* idp[3] = { ids[0], ids[1], ids[2] };
      store.put(idp, nIds, (uint32_t)sSp, sOff, sFr, 1790000000u + (uint32_t)k);
    }
    printf("#define GP_B%d_INIT { \"%s\", %d, %d, GP_B%d_NAMES, GP_B%d_LEN, GP_B%d_IDS, %d, "
           "GP_B%d_FR, %d, GP_B%d_LOC, %d, %d, %uu, %.17g }\n",
           k, FIXTURES[k], (int)st, b.nSpine, k, k, k, nIds, k, nFr, k, nLoc,
           sSp, (unsigned)sOff, sFr);
    if (st == EPUB_OK) {
      epubClose(&b);
    }
    fclose(f);
  }
  printf("static const GpBook GP_BOOKS[] = {\n");
  for (int k = 0; k < NFIX; k++) {
    printf("  GP_B%d_INIT,\n", k);
  }
  printf("};\n");

  static char blob[BOOKSTORE_BLOB_MAX];
  size_t n = store.serialise(blob, sizeof(blob));
  printf("// /books/positions.cbs as the 0.9.78 store wrote it, one saved place per book above.\n");
  printf("static const char GP_STORE_BLOB[] =\n");
  const char* p = blob;
  while (p < blob + n) {
    const char* nl = (const char*)memchr(p, '\n', (size_t)(blob + n - p));
    size_t len = nl ? (size_t)(nl - p) + 1 : (size_t)(blob + n - p);
    char line[1024];
    memcpy(line, p, len);
    line[len] = '\0';
    printf("  ");
    cstr(line);
    printf("\n");
    p += len;
  }
  printf("  ;\n");
  return 0;
}
