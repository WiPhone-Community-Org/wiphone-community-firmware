/*
 * test_layout.cpp — paging a chapter into screens.
 *
 * There is no wire format to match here, so these are behaviour tests. They target the ways
 * a reader loses your place without saying anything: a word that vanishes at a page break, a
 * word shown twice, a page that opens on a blank row, a multi-byte character sliced in half,
 * and going BACK a page landing somewhere other than where you came from.
 *
 * The width stub is deliberately crude — every ASCII byte is 6px wide, every continuation
 * byte 0 — so an expected line break can be worked out by hand and a failure means the RULE
 * is wrong rather than the font.
 */
#include "../WiPhone/book_layout.h"

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
  if (strcmp(got, want) == 0) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s\n    want=[%s]\n     got=[%s]\n", g_group, what, want, got);
}

// ---------------------------------------------------------------- the width stub
#define CHARW 6

static int stubWidth(void* ctx, const char* s, size_t len) {
  (void)ctx;
  int w = 0;
  for (size_t i = 0; i < len; i++) {
    if (((unsigned char)s[i] & 0xC0) != 0x80) {      // one width per CHARACTER, not per byte
      w += CHARW;
    }
  }
  return w;
}

static BookMeasure MEASURE = { NULL, stubWidth };

// Copy line `i` of a laid-out page into a C string.
static void lineText(const char* text, const BookPage* pg, int i, char* out, size_t cap) {
  size_t n = pg->lines[i].len;
  if (n > cap - 1) {
    n = cap - 1;
  }
  memcpy(out, text + pg->lines[i].off, n);
  out[n] = '\0';
}

// Every drawn line of every page, joined by single spaces: what the reader actually sees.
static void readAll(const char* text, int widthPx, int maxLines, char* out, size_t cap) {
  out[0] = '\0';
  size_t len = strlen(text);
  uint32_t at = 0;
  BookPage pg;
  for (int guard = 0; guard < 4096; guard++) {
    bookLayoutPage(text, len, at, widthPx, maxLines, &MEASURE, &pg);
    for (int i = 0; i < pg.nLines; i++) {
      if (pg.lines[i].blank) {
        continue;
      }
      char buf[512];
      lineText(text, &pg, i, buf, sizeof(buf));
      if (out[0]) {
        strncat(out, " ", cap - strlen(out) - 1);
      }
      strncat(out, buf, cap - strlen(out) - 1);
    }
    if (pg.next >= len || pg.next <= at) {
      break;
    }
    at = pg.next;
  }
}

// The same text with every run of white space collapsed to one space: the words, in order.
static void words(const char* text, char* out, size_t cap) {
  size_t o = 0;
  bool sp = true;
  for (size_t i = 0; text[i] && o + 1 < cap; i++) {
    char c = text[i];
    bool isSp = (c == ' ' || c == '\n' || c == '\t' || c == '\r');
    if (isSp) {
      sp = true;
      continue;
    }
    if (sp && o > 0) {
      out[o++] = ' ';
    }
    sp = false;
    out[o++] = c;
  }
  out[o] = '\0';
}

// ---------------------------------------------------------------- tests
static void testWrapping() {
  group("wrapping breaks at the last space that fits");

  // 10 characters fit in 60px.
  const char* t = "the quick brown fox jumps";
  BookPage pg;
  bookLayoutPage(t, strlen(t), 0, 10 * CHARW, 8, &MEASURE, &pg);

  char l0[64], l1[64], l2[64];
  ok(pg.nLines == 3, "three lines");
  if (pg.nLines == 3) {
    lineText(t, &pg, 0, l0, sizeof(l0));
    lineText(t, &pg, 1, l1, sizeof(l1));
    lineText(t, &pg, 2, l2, sizeof(l2));
    eqStr(l0, "the quick", "line 0 stops before 'brown'");
    eqStr(l1, "brown fox", "line 1");
    eqStr(l2, "jumps", "line 2");
  }
  eqU32(pg.next, strlen(t), "the page consumed the text");
  eqU32(pg.start, 0, "starts at 0");

  // A line that exactly fills the width keeps its last word.
  const char* e = "abcde fghij k";
  bookLayoutPage(e, strlen(e), 0, 11 * CHARW, 8, &MEASURE, &pg);
  lineText(e, &pg, 0, l0, sizeof(l0));
  eqStr(l0, "abcde fghij", "an exactly-filling line is not broken early");
}

static void testParagraphGaps() {
  group("paragraph gaps survive, runs of them do not");

  const char* t = "one\n\ntwo\n\n\n\nthree";
  BookPage pg;
  bookLayoutPage(t, strlen(t), 0, 20 * CHARW, 12, &MEASURE, &pg);

  eqU32((unsigned long)pg.nLines, 5, "text, gap, text, gap, text");
  ok(!pg.lines[0].blank && pg.lines[1].blank && !pg.lines[2].blank &&
     pg.lines[3].blank && !pg.lines[4].blank, "gaps sit between the paragraphs");

  char l[32];
  lineText(t, &pg, 4, l, sizeof(l));
  eqStr(l, "three", "a four-newline run collapsed to ONE gap");

  // A hard newline inside a paragraph still ends the line.
  const char* h = "ab\ncd";
  bookLayoutPage(h, strlen(h), 0, 40 * CHARW, 8, &MEASURE, &pg);
  eqU32((unsigned long)pg.nLines, 2, "a newline ends a line even when there is room");
}

static void testPageNeverOpensBlank() {
  group("a page never opens on white space");

  // Two lines per page, so page 2 begins right after a paragraph gap.
  const char* t = "aaa bbb\n\nccc ddd";
  BookPage p1, p2;
  bookLayoutPage(t, strlen(t), 0, 7 * CHARW, 2, &MEASURE, &p1);
  bookLayoutPage(t, strlen(t), p1.next, 7 * CHARW, 2, &MEASURE, &p2);

  ok(p2.nLines > 0 && !p2.lines[0].blank, "page 2's first row is text");
  char l[32];
  lineText(t, &p2, 0, l, sizeof(l));
  eqStr(l, "ccc ddd", "and it is the right text");

  // The same holds when a page start is handed in mid-whitespace.
  BookPage p3;
  bookLayoutPage(t, strlen(t), 7, 7 * CHARW, 2, &MEASURE, &p3);      // offset 7 == the '\n'
  eqU32(p3.start, 9, "start skipped forward past the blank line");
  ok(p3.nLines > 0 && !p3.lines[0].blank, "and still opens on text");

  // A trailing gap is dropped rather than eating the last row.
  const char* g = "aaa\n\nbbb";
  BookPage p4;
  bookLayoutPage(g, strlen(g), 0, 7 * CHARW, 2, &MEASURE, &p4);
  eqU32((unsigned long)p4.nLines, 1, "the gap did not take the second row");
  eqU32(p4.next, 5, "and it was consumed, not left for the next page");
}

static void testNothingLostOrDuplicated() {
  group("paging through a chapter shows every word exactly once");

  const char* t =
    "Call me Ishmael. Some years ago, never mind how long precisely, having little or no\n"
    "money in my purse, and nothing particular to interest me on shore, I thought I would\n"
    "sail about a little and see the watery part of the world.\n"
    "\n"
    "It is a way I have of driving off the spleen and regulating the circulation.\n"
    "\n"
    "Whenever I find myself growing grim about the mouth; whenever it is a damp, drizzly\n"
    "November in my soul; then, I account it high time to get to sea as soon as I can.";

  char seen[4096], want[4096];
  words(t, want, sizeof(want));

  // Several geometries, because an off-by-one at a break usually only shows at one width.
  const int widths[] = { 12, 20, 33, 40 };
  const int lines[]  = { 2, 5, 11 };
  for (int wi = 0; wi < 4; wi++) {
    for (int li = 0; li < 3; li++) {
      readAll(t, widths[wi] * CHARW, lines[li], seen, sizeof(seen));
      char what[80];
      snprintf(what, sizeof(what), "%d chars x %d lines reads back word for word",
               widths[wi], lines[li]);
      eqStr(seen, want, what);
    }
  }
}

static void testLongWord() {
  group("a word wider than the line is broken, not looped on");

  const char* t = "go https://example.com/a/very/long/path/that/never/ends/at/all done";
  char seen[512], want[512];
  words(t, want, sizeof(want));

  // Broken mid-word, the pieces still concatenate — but with a space between them, so
  // compare against the source with the break points removed instead.
  BookPage pg;
  size_t len = strlen(t);
  uint32_t at = 0;
  size_t o = 0;
  seen[0] = '\0';
  for (int guard = 0; guard < 200; guard++) {
    bookLayoutPage(t, len, at, 10 * CHARW, 3, &MEASURE, &pg);
    for (int i = 0; i < pg.nLines; i++) {
      if (pg.lines[i].blank) {
        continue;
      }
      // A hard break leaves no space behind it; a space break consumed one.
      if (o > 0 && t[pg.lines[i].off - 1] == ' ') {
        seen[o++] = ' ';
      }
      memcpy(seen + o, t + pg.lines[i].off, pg.lines[i].len);
      o += pg.lines[i].len;
      seen[o] = '\0';
    }
    if (pg.next >= len || pg.next <= at) {
      break;
    }
    at = pg.next;
  }
  eqStr(seen, t, "the long URL comes back whole across its breaks");

  // A single glyph wider than the whole line still advances.
  BookPage one;
  bookLayoutPage("abc", 3, 0, 1, 4, &MEASURE, &one);
  ok(one.next > one.start, "a 1px-wide line still makes progress");
}

static void testUtf8() {
  group("multi-byte characters are never sliced");

  // Curly quotes and an em dash: three bytes each, and everywhere in a real EPUB.
  const char* t = "\xe2\x80\x9cWait\xe2\x80\x9d \xe2\x80\x94 she said \xe2\x80\x94 quietly now";
  size_t len = strlen(t);

  BookPage pg;
  uint32_t at = 0;
  for (int guard = 0; guard < 100; guard++) {
    bookLayoutPage(t, len, at, 7 * CHARW, 2, &MEASURE, &pg);
    for (int i = 0; i < pg.nLines; i++) {
      uint32_t s = pg.lines[i].off, e = s + pg.lines[i].len;
      ok(((unsigned char)t[s] & 0xC0) != 0x80, "a line starts on a character boundary");
      ok(e >= len || ((unsigned char)t[e] & 0xC0) != 0x80, "and ends on one");
    }
    ok(pg.next >= len || ((unsigned char)t[pg.next] & 0xC0) != 0x80,
       "a page boundary is a character boundary");
    if (pg.next >= len || pg.next <= at) {
      break;
    }
    at = pg.next;
  }
}

/* The contract of a page-back, checked directly: it starts earlier, it ENDS exactly where we
 * came from, and it is a full page. Note what is deliberately NOT asserted — that the offset
 * equals what a forward walk from the top produced. Greedy wrapping restarted at a different
 * offset can stay out of phase, so a cold page-back may reflow; continuity is what a reader
 * can actually perceive, and exactness for pages already read comes from the app's history
 * stack. Asserting identity here would be asserting a promise the algorithm cannot keep. */
static void checkBack(const char* t, size_t len, uint32_t before, int w, int ml,
                      const char* label) {
  char what[96];
  uint32_t back = bookLayoutPrevPage(t, len, before, w, ml, &MEASURE);
  snprintf(what, sizeof(what), "%s: starts earlier", label);
  ok(back < before, what);

  /* It has to END where we came from — but "where" is only meaningful up to white space: a
   * page that stops on the newline before a paragraph gap and one that stops after it show
   * the reader the same thing, because the next page skips leading white space either way.
   * What must never happen is a WORD falling into the gap. */
  BookPage pg;
  bookLayoutPage(t, before, back, w, ml, &MEASURE, &pg);
  bool clean = pg.next <= before;
  for (uint32_t i = pg.next; i < before; i++) {
    if (t[i] != ' ' && t[i] != '\n' && t[i] != '\t' && t[i] != '\r') {
      clean = false;
    }
  }
  snprintf(what, sizeof(what), "%s: nothing but white space left between the pages", label);
  ok(clean, what);

  snprintf(what, sizeof(what), "%s: is a full page", label);
  ok(back == 0 || pg.nLines >= ml - 1, what);
}

static void testPrevPage() {
  group("going back is continuous and lands a full page");

  const char* t =
    "Whenever it is a damp, drizzly November in my soul; whenever I find myself involuntarily "
    "pausing before coffin warehouses, and bringing up the rear of every funeral I meet; and "
    "especially whenever my hypos get such an upper hand of me, that it requires a strong "
    "moral principle to prevent me from deliberately stepping into the street, and methodically "
    "knocking people's hats off - then, I account it high time to get to sea as soon as I can.";
  size_t len = strlen(t);
  const int w = 33 * CHARW, ml = 4;

  // Walk forward, remembering every page start, then walk back and compare.
  uint32_t starts[64];
  int n = 0;
  uint32_t at = 0;
  BookPage pg;
  while (n < 64) {
    starts[n++] = at;
    bookLayoutPage(t, len, at, w, ml, &MEASURE, &pg);
    if (pg.next >= len || pg.next <= at) {
      break;
    }
    at = pg.next;
  }
  ok(n >= 3, "the sample is more than two pages long");

  for (int i = n - 1; i > 0; i--) {
    char label[32];
    snprintf(label, sizeof(label), "one paragraph, page %d", i);
    checkBack(t, len, starts[i], w, ml, label);
  }

  eqU32(bookLayoutPrevPage(t, len, 0, w, ml, &MEASURE), 0, "back from the top stays at the top");

  // An arbitrary offset — a restored or synced position, not a page boundary — is the case
  // that has no history stack to fall back on, so it must hold the contract on its own.
  checkBack(t, len, 300, w, ml, "mid-word offset");

  // The shape a real book actually has: short paragraphs separated by blank lines. Every
  // page start on the way down has to come back exactly on the way up.
  const char* para =
    "The Ghost Dogs made planetfall at dusk.\n\n"
    "Nothing moved in the treeline for a long while, and then everything did at once, which "
    "is how ambushes always look from inside one.\n\n"
    "Sergeant Vidal counted three heat blooms, then four, then stopped counting and started "
    "shooting instead.\n\n"
    "Afterwards nobody could agree on how long it had taken.\n";
  size_t plen = strlen(para);
  uint32_t pstarts[64];
  int pn = 0;
  at = 0;
  while (pn < 64) {
    pstarts[pn++] = at;
    bookLayoutPage(para, plen, at, w, 3, &MEASURE, &pg);
    if (pg.next >= plen || pg.next <= at) {
      break;
    }
    at = pg.next;
  }
  ok(pn >= 4, "the paragraph sample is several pages long");
  for (int i = pn - 1; i > 0; i--) {
    char label[32];
    snprintf(label, sizeof(label), "paragraphs, page %d", i);
    checkBack(para, plen, pstarts[i], w, 3, label);
  }

  // One line per page is the pathological geometry; it must still round-trip.
  uint32_t a1 = 0;
  bookLayoutPage(t, len, a1, w, 1, &MEASURE, &pg);
  uint32_t a2 = pg.next;
  bookLayoutPage(t, len, a2, w, 1, &MEASURE, &pg);
  uint32_t a3 = pg.next;
  eqU32(bookLayoutPrevPage(t, len, a3, w, 1, &MEASURE), a2, "single-line pages step back one line");
}

/* Page a whole chapter backwards, every boundary, on text shaped like a real book: many
 * paragraphs of varying length separated by blank lines.
 *
 * This is here because the synthetic cases above all passed while a REAL chapter dropped a
 * word at 3 of its 72 boundaries. The cause was a blank line landing at the end of a page:
 * it is dropped from that page's rows, so counting back a fixed number of lines came up one
 * row short and stranded a line between the two pages. Short samples never produced the
 * unlucky alignment. */
static void testEveryBoundaryOfAChapter() {
  group("paging a whole chapter backwards strands nothing");

  // Deterministic pseudo-book: paragraph lengths cycle so blank lines land at every possible
  // offset within a page.
  static char chapter[24000];
  size_t o = 0;
  int word = 0;
  for (int para = 0; para < 90 && o < sizeof(chapter) - 400; para++) {
    int words = 5 + (para * 7) % 61;
    for (int i = 0; i < words && o < sizeof(chapter) - 40; i++) {
      int len = 2 + (word * 13 + para * 5) % 11;
      if (i) {
        chapter[o++] = ' ';
      }
      for (int c = 0; c < len; c++) {
        chapter[o++] = (char)('a' + (word + c) % 26);
      }
      word++;
    }
    chapter[o++] = '\n';
    chapter[o++] = '\n';
  }
  chapter[o] = '\0';
  size_t len = o;

  // Two geometries, because the failure was an alignment between blanks and the page height.
  const int geo[][2] = { { 34 * CHARW, 11 }, { 22 * CHARW, 7 } };
  for (int g = 0; g < 2; g++) {
    const int w = geo[g][0], ml = geo[g][1];

    uint32_t starts[2048];
    int n = 0;
    uint32_t at = 0;
    BookPage pg;
    while (n < 2048) {
      starts[n++] = at;
      bookLayoutPage(chapter, len, at, w, ml, &MEASURE, &pg);
      if (pg.next >= len || pg.next <= at) {
        break;
      }
      at = pg.next;
    }

    int stranded = 0;
    for (int i = n - 1; i > 0; i--) {
      uint32_t back = bookLayoutPrevPage(chapter, len, starts[i], w, ml, &MEASURE);
      bookLayoutPage(chapter, starts[i], back, w, ml, &MEASURE, &pg);
      if (pg.next > starts[i]) {
        stranded++;
        continue;
      }
      for (uint32_t k = pg.next; k < starts[i]; k++) {
        if (chapter[k] != ' ' && chapter[k] != '\n') {
          stranded++;
          break;
        }
      }
    }
    char what[88];
    snprintf(what, sizeof(what), "%d chars x %d lines: %d boundaries, none stranded",
             geo[g][0] / CHARW, ml, n - 1);
    ok(n > 20 && stranded == 0, what);
  }
}

/* Pictures in the flow. The failure that matters here is not a misplaced picture — it is a
 * page that cannot advance, because the caller reads next <= start as "end of chapter" and
 * the reader then sits on one page for ever. */
static void testImages() {
  group("pictures take rows out of the page");

  const char* t =
    "The Ghost Dogs made planetfall at dusk.\n\n"
    "Nothing moved in the treeline for a long while, and then everything did at once.\n\n"
    "Sergeant Vidal counted three heat blooms, then four, then stopped counting.\n\n"
    "Afterwards nobody could agree on how long it had taken at all.\n";
  size_t len = strlen(t);
  const int w = 20 * CHARW, ml = 6;

  // One picture at the top, one in the middle.
  BookImageBox imgs[2] = { { 0, 3 }, { 130, 2 } };
  BookPage pg;
  bookLayoutPageImages(t, len, 0, w, ml, &MEASURE, imgs, 2, &pg);

  ok(pg.nLines > 0 && pg.lines[0].image == 0, "the picture at offset 0 opens the page");
  eqU32(pg.lines[0].rows, 3, "and takes its three rows");
  int rows = 0;
  for (int i = 0; i < pg.nLines; i++) {
    rows += pg.lines[i].rows;
  }
  ok(rows <= ml, "the page never exceeds its row budget");
  ok(pg.next > pg.start, "and it still advances");

  // Every picture appears exactly once across the whole chapter, and none is lost.
  int seen[2] = { 0, 0 };
  uint32_t at = 0;
  for (int guard = 0; guard < 200; guard++) {
    bookLayoutPageImages(t, len, at, w, ml, &MEASURE, imgs, 2, &pg);
    int r = 0;
    for (int i = 0; i < pg.nLines; i++) {
      r += pg.lines[i].rows;
      if (pg.lines[i].image >= 0) {
        seen[pg.lines[i].image]++;
      }
    }
    ok(r <= ml, "every page stays inside its budget");
    if (pg.next >= len || pg.next <= at) {
      break;
    }
    at = pg.next;
  }
  eqU32(seen[0], 1, "picture 0 shown exactly once");
  eqU32(seen[1], 1, "picture 1 shown exactly once");

  /* A picture taller than the page. It must still be shown, and the page MUST advance —
   * capping it at maxLines-1 rows is what guarantees room for a line of text. */
  BookImageBox huge[1] = { { 0, 99 } };
  bookLayoutPageImages(t, len, 0, w, ml, &MEASURE, huge, 1, &pg);
  ok(pg.nLines >= 2 && pg.lines[0].image == 0, "an oversized picture is still shown");
  eqU32(pg.lines[0].rows, (unsigned long)(ml - 1), "clamped to leave one row");
  ok(pg.next > pg.start, "and the page can still be turned");

  // Same, with one line per page: the degenerate geometry must not deadlock either.
  bookLayoutPageImages(t, len, 0, w, 1, &MEASURE, huge, 1, &pg);
  ok(pg.next > pg.start, "one-line pages advance even with a picture");

  // Page-back over a chapter with pictures strands nothing.
  uint32_t starts[64];
  int n = 0;
  at = 0;
  while (n < 64) {
    starts[n++] = at;
    bookLayoutPageImages(t, len, at, w, ml, &MEASURE, imgs, 2, &pg);
    if (pg.next >= len || pg.next <= at) {
      break;
    }
    at = pg.next;
  }
  ok(n >= 3, "the sample runs to several pages");
  int stranded = 0;
  for (int i = n - 1; i > 0; i--) {
    uint32_t back = bookLayoutPrevPageImages(t, len, starts[i], w, ml, &MEASURE, imgs, 2);
    bookLayoutPageImages(t, starts[i], back, w, ml, &MEASURE, imgs, 2, &pg);
    for (uint32_t k = pg.next; k < starts[i]; k++) {
      if (t[k] != ' ' && t[k] != '\n') {
        stranded++;
        break;
      }
    }
  }
  ok(stranded == 0, "paging back past pictures strands no text");

  /* A picture too tall for the space LEFT on its page. It moves to the next one — and the
   * page it moved off must end at or before its offset, or it is lost for good: the next
   * page skips anything behind its own start, and nothing is logged.
   *
   * This is the shape that a real chapter hit and five synthetic cases missed: a heading, a
   * gap, another heading, then a 264x264 illustration nine rows tall. */
  {
    const char* ch = "PROLOGUE\n\nGHOSTS WHO SELL MEMORIES\n\nTHE RUINS OF GREYWALK\n\n"
                     "Old Tom Frost sat atop a hunk of ferrocrete in a ruined street.\n";
    size_t clen = strlen(ch);
    BookImageBox big[1] = { { 34, 9 } };      // right after the second heading
    int found = 0, dup = 0;
    uint32_t a = 0;
    uint32_t prevNext = 0;
    for (int guard = 0; guard < 100; guard++) {
      BookPage p2;
      bookLayoutPageImages(ch, clen, a, 22 * CHARW, 11, &MEASURE, big, 1, &p2);
      int r = 0;
      for (int i = 0; i < p2.nLines; i++) {
        r += p2.lines[i].rows;
        if (p2.lines[i].image == 0) {
          found++;
          if (found > 1) {
            dup++;
          }
        }
      }
      ok(r <= 11, "budget respected on every page");
      prevNext = p2.next;
      if (p2.next >= clen || p2.next <= a) {
        break;
      }
      a = p2.next;
    }
    ok(found == 1, "a picture that does not fit moves to the next page instead of vanishing");
    ok(dup == 0, "and is not shown twice");
    ok(prevNext >= clen, "the chapter still reaches its end");
  }

  // Passing no pictures must be identical to the plain call.
  BookPage a, b;
  bookLayoutPage(t, len, 0, w, ml, &MEASURE, &a);
  bookLayoutPageImages(t, len, 0, w, ml, &MEASURE, NULL, 0, &b);
  ok(a.start == b.start && a.next == b.next && a.nLines == b.nLines,
     "no pictures means exactly the old behaviour");
}

static void testSnap() {
  group("snap moves back to the start of a word");

  const char* t = "alpha beta gamma";
  eqU32(bookLayoutSnap(t, strlen(t), 8), 6, "mid-word snaps to the word start");
  eqU32(bookLayoutSnap(t, strlen(t), 6), 6, "a word start does not move");
  eqU32(bookLayoutSnap(t, strlen(t), 0), 0, "the top does not move");
  eqU32(bookLayoutSnap(t, strlen(t), 5), 5, "a space does not move");

  // Never slice a character: offset 2 is inside the em dash.
  const char* u = "a\xe2\x80\x94""b";
  uint32_t s = bookLayoutSnap(u, strlen(u), 2);
  ok(((unsigned char)u[s] & 0xC0) != 0x80, "snapped onto a character boundary");

  // A pathological run with no spaces is bounded, not walked to the top of the chapter.
  char big[512];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  eqU32(bookLayoutSnap(big, strlen(big), 400), 400 - 64, "the search back is capped at 64 bytes");
}

static void testDegenerate() {
  group("empty and out-of-range input does not misbehave");

  BookPage pg;
  bookLayoutPage("", 0, 0, 100, 8, &MEASURE, &pg);
  eqU32((unsigned long)pg.nLines, 0, "no lines in an empty chapter");
  eqU32(pg.next, 0, "and nowhere to go");

  const char* t = "hello world";
  bookLayoutPage(t, strlen(t), 999, 100, 8, &MEASURE, &pg);
  eqU32(pg.start, strlen(t), "a start past the end clamps");
  eqU32((unsigned long)pg.nLines, 0, "with nothing to draw");

  bookLayoutPage("   \n\n  \n", 8, 0, 100, 8, &MEASURE, &pg);
  eqU32((unsigned long)pg.nLines, 0, "all-whitespace text yields no rows");

  bookLayoutPage(t, strlen(t), 0, 100, 0, &MEASURE, &pg);
  ok(pg.nLines == 1, "maxLines below 1 is treated as 1");

  eqU32(bookLayoutPrevPage(t, strlen(t), 999, 100, 8, &MEASURE), 0,
        "prev from past the end clamps to the only page");
}

int main() {
  testWrapping();
  testParagraphGaps();
  testPageNeverOpensBlank();
  testNothingLostOrDuplicated();
  testLongWord();
  testUtf8();
  testPrevPage();
  testEveryBoundaryOfAChapter();
  testImages();
  testSnap();
  testDegenerate();

  printf("\n%s%d passed, %d failed\033[0m\n", g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
