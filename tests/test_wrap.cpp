/* test_wrap.cpp — a menu note broken into rows that fit (WiPhone/menu_wrap.h).
 *
 * WHY THIS IS WORTH A SUITE: the three ways this goes wrong are silent on the phone. A break
 * inside a word reads as a typo; a word wider than the row must be cut, not looped on for
 * ever; and the last word of a note is the easiest one to lose. The function under test is
 * the REAL one GUI.cpp's MenuWidget::addNoteWrapped calls. The measurer stands in for the
 * font: a row holds `rowChars` glyphs, so widths are exact.
 */
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>

#include "../WiPhone/menu_wrap.h"

static int failures = 0;
static int checks = 0;

static void group(const char *name) {
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char *what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  } else {
    printf("  ok  %s\n", what);
  }
}

struct Ctx {
  size_t rowChars;                 // glyphs per row
  std::vector<std::string> rows;
};

/* Bytes of s that fit: rowChars glyphs, UTF-8 continuation bytes free (like fitTextLength). */
static size_t fitFn(const char* s, void* v) {
  Ctx* c = (Ctx*)v;
  size_t glyphs = 0, i = 0;
  while (s[i]) {
    if (((unsigned char)s[i] & 0xC0) != 0x80) {   // a glyph starts here
      if (glyphs == c->rowChars) {
        break;
      }
      glyphs++;
    }
    i++;
  }
  return i;
}

static void rowFn(const char* s, size_t len, void* v) {
  ((Ctx*)v)->rows.push_back(std::string(s, len));
}

static std::vector<std::string> wrap(const char* text, size_t rowChars, int maxRows = 8) {
  Ctx c;
  c.rowChars = rowChars;
  int n = wrapNote(text, fitFn, rowFn, &c, maxRows);
  ok(n == (int)c.rows.size(), "row count returned matches rows emitted");
  return c.rows;
}

int main() {
  group("fits: one row, untouched");
  {
    auto r = wrap("Back", 20);
    ok(r.size() == 1 && r[0] == "Back", "a short note is one row");
    r = wrap("exactly twenty chars", 20);
    ok(r.size() == 1 && r[0] == "exactly twenty chars", "a note exactly the row width is one row");
    r = wrap("", 20);
    ok(r.empty(), "an empty note is no rows");
    r = wrap(NULL, 20);
    ok(r.empty(), "a NULL note is no rows");
    r = wrap("   ", 20);
    ok(r.empty(), "spaces only is no rows");
  }

  group("breaks at the last space that fits");
  {
    auto r = wrap("12853 tiles, 1606 MB, about 257 min", 20);
    ok(r.size() == 2, "the estimate line is two rows at 20 glyphs");
    ok(r[0] == "12853 tiles, 1606", "first row ends at a word boundary");
    ok(r[1] == "MB, about 257 min", "second row starts at the next word, no leading space");
    r = wrap("Last run: 5517 new, 7332 already had, 4 failed, 7013 s", 24);
    ok(r.size() == 3, "the last-run line is three rows at 24 glyphs");
    ok(r[0] == "Last run: 5517 new, 7332", "row 1");
    ok(r[1] == "already had, 4 failed,", "row 2");
    ok(r[2] == "7013 s", "row 3 keeps the last word");
    std::string all;
    for (auto& x : r) { all += x; all += ' '; }
    ok(all == "Last run: 5517 new, 7332 already had, 4 failed, 7013 s ", "nothing lost, nothing added");
  }

  group("a word wider than the row is cut, never looped on");
  {
    auto r = wrap("supercalifragilisticexpialidocious", 10);
    ok(r.size() == 4, "34 glyphs at 10 a row is four rows");
    ok(r[0] == "supercalif" && r[3] == "ious", "cut where it is, tail kept");
    r = wrap("a supercalifragilisticexpialidocious b", 10);
    ok(r.size() == 5, "the long word after a short one: 'a' alone, then the pieces");
    ok(r.size() > 0 && r[0] == "a", "the short word before takes its own row (the break is at the space)");
    ok(r.size() == 5 && r[4] == "ious b", "the tail of the word and the short word after share the last row");
  }

  group("UTF-8 is broken only at glyph boundaries");
  {
    auto r = wrap("café crème brûlée forever", 11);   // glyphs: 4+1+5+1+6 = 17 to 'brûlée'
    ok(r.size() == 3, "three rows");
    ok(r.size() == 3 && r[0] == "café crème", "an accented word is whole");
    ok(r.size() == 3 && r[1] == "brûlée", "the next one too");
    ok(r.size() == 3 && r[2] == "forever", "and the tail");
  }

  group("the row cap");
  {
    auto r = wrap("a b c d e f g h i j k l", 1, 3);
    ok(r.size() == 3, "stops at maxRows");
    ok(r.size() == 3 && r[0] == "a" && r[2] == "c", "the first rows are the first words");
  }

  group("real widths: the phone's 232 px row in Akrobat 20 is about 27 glyphs");
  {
    auto r = wrap("Last problem: z16 10714/23006: card refused the write", 27);
    for (size_t i = 0; i < r.size(); i++) {
      printf("      [%zu] '%s'\n", i, r[i].c_str());
    }
    ok(r.size() == 3, "the card-refused line wraps to three rows");
    ok(r.size() == 3 && r[0] == "Last problem: z16", "row 1");
    ok(r.size() == 3 && r[1] == "10714/23006: card refused", "row 2");
    ok(r.size() == 3 && r[2] == "the write", "row 3");
    r = wrap("Start again fetches only the missing tiles", 27);
    ok(r.size() == 2 && r[0] == "Start again fetches only" && r[1] == "the missing tiles", "the hint is two rows");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
