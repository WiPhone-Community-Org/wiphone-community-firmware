/* test_marquee.cpp — the scrolling selected menu row: its phase clock and its glyph stepping.
 *
 * WHY THIS IS WORTH A SUITE: the two ways this can go wrong are both SILENT on the phone. A
 * clock that never reaches HOLD_END, or never wraps, just looks like a row that stopped; a
 * byte offset that lands inside a UTF-8 sequence draws one garbage box in a title that
 * happens to carry an accent — and every test title anyone types by hand is plain ASCII.
 * marqueeStart's "park at your own end" rule is the third: get it wrong and a row with a
 * short title and a long subtitle scrolls its title clean off the screen while the subtitle
 * is still going.
 *
 * The functions under test are the REAL ones GUI.cpp calls: it includes menu_marquee.h and so
 * does this file. The measurer stands in for the LCD: 10 px per glyph, so widths are exact.
 */
#include <cstdio>
#include <cstring>
#include <cstddef>

#include "../WiPhone/menu_marquee.h"

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

/* A monospace stand-in for SmoothFont::textWidth: 10 px per glyph, continuation bytes free. */
static int glyphs(const char* s) {
  int n = 0;
  for (; *s; s++) {
    if (((unsigned char)*s & 0xC0) != 0x80) {
      n++;
    }
  }
  return n;
}
static int measure10(void* ctx, const char* s) {
  (void)ctx;
  return glyphs(s) * 10;
}

int main() {
  group("marqueeGlyphOffset: n glyphs in, a byte offset out, never inside a sequence");
  ok(marqueeGlyphOffset("abc", 0) == 0, "0 glyphs is offset 0");
  ok(marqueeGlyphOffset("abc", 2) == 2, "ASCII: one byte a glyph");
  ok(marqueeGlyphOffset("abc", 3) == 3, "the whole string is its length");
  ok(marqueeGlyphOffset("abc", 50) == 3, "past the end clamps to the end, no read-over");
  ok(marqueeGlyphOffset("", 5) == 0, "empty string stays at 0");
  /* "é" is C3 A9 (2 bytes), "€" is E2 82 AC (3), "😀" is F0 9F 98 80 (4). */
  ok(marqueeGlyphOffset("\xC3\xA9" "x", 1) == 2, "a 2-byte glyph is stepped as one");
  ok(marqueeGlyphOffset("\xE2\x82\xAC" "x", 1) == 3, "a 3-byte glyph is stepped as one");
  ok(marqueeGlyphOffset("\xF0\x9F\x98\x80" "x", 1) == 4, "a 4-byte glyph is stepped as one");
  ok(marqueeGlyphOffset("a\xC3\xA9" "b\xE2\x82\xAC" "c", 3) == 4, "mixed: a,é,b -> 4 bytes");
  ok(marqueeGlyphOffset("a\xC3\xA9" "b\xE2\x82\xAC" "c", 4) == 7, "mixed: a,é,b,€ -> 7 bytes");

  group("marqueeAdvance: hold the start, step, hold the end, wrap");
  MarqueeClock c;
  marqueeReset(&c, 1000);
  ok(c.offset == 0 && c.phase == MARQUEE_HOLD_START, "reset is offset 0, holding the start");
  ok(!marqueeAdvance(&c, 1000 + MARQUEE_HOLD_START_MS - 1, true), "799 ms in: still holding");
  ok(c.offset == 0, "...and the offset has not moved");
  ok(marqueeAdvance(&c, 1000 + MARQUEE_HOLD_START_MS, true), "800 ms: the first step, repaint");
  ok(c.offset == 1 && c.phase == MARQUEE_SCROLL, "offset 1, scrolling");
  uint32_t t = 1000 + MARQUEE_HOLD_START_MS;
  ok(!marqueeAdvance(&c, t + MARQUEE_STEP_MS - 1, true), "199 ms after a step: nothing");
  ok(marqueeAdvance(&c, t + MARQUEE_STEP_MS, true), "200 ms after a step: the next glyph");
  ok(c.offset == 2, "offset 2");
  t += MARQUEE_STEP_MS;
  /* The draw at offset 2 reports every text has shown its end. */
  ok(!marqueeAdvance(&c, t + MARQUEE_STEP_MS, false), "end reached: no step, no repaint");
  ok(c.phase == MARQUEE_HOLD_END && c.offset == 2, "holding the end at the same offset");
  t += MARQUEE_STEP_MS;
  ok(!marqueeAdvance(&c, t + MARQUEE_HOLD_END_MS - 1, false), "999 ms at the end: still holding");
  ok(marqueeAdvance(&c, t + MARQUEE_HOLD_END_MS, false), "1000 ms: wrap, repaint");
  ok(c.offset == 0 && c.phase == MARQUEE_HOLD_START, "back at the start, holding");
  t += MARQUEE_HOLD_END_MS;
  ok(!marqueeAdvance(&c, t + 100, true), "a fresh start hold: nothing for 800 ms again");

  group("marqueeAdvance: the clock survives millis() wrapping");
  marqueeReset(&c, 0xFFFFFF00u);                      // 256 ms before the wrap
  ok(!marqueeAdvance(&c, 0xFFFFFFF0u, true), "240 ms in (before the wrap): holding");
  ok(marqueeAdvance(&c, 0x00000300u, true), "1024 ms in (after the wrap): stepped");
  ok(c.offset == 1, "offset 1 across the wrap");

  group("marqueeAdvance: a start hold that finds nothing to scroll parks, never spins");
  marqueeReset(&c, 0);
  ok(!marqueeAdvance(&c, MARQUEE_HOLD_START_MS, false), "no more to come at offset 0: no step");
  ok(c.phase == MARQUEE_HOLD_END && c.offset == 0, "it goes to the end hold and wraps from there");

  group("marqueeStart: where the visible text begins, and parking at your own end");
  /* 12 glyphs, 120 px, in a 50 px space (5 glyphs). Fits from glyph 7 onwards. */
  const char* s = "ABCDEFGHIJKL";
  bool atEnd = true;
  ok(marqueeStart(s, 0, 50, measure10, NULL, &atEnd) == 0 && !atEnd, "offset 0: from A, more to come");
  ok(marqueeStart(s, 3, 50, measure10, NULL, &atEnd) == 3 && !atEnd, "offset 3: from D, more to come");
  ok(marqueeStart(s, 6, 50, measure10, NULL, &atEnd) == 6 && !atEnd, "offset 6: G..L is 60 px, still more");
  ok(marqueeStart(s, 7, 50, measure10, NULL, &atEnd) == 7 && atEnd, "offset 7: H..L is 50 px, at the end");
  ok(marqueeStart(s, 9, 50, measure10, NULL, &atEnd) == 7 && atEnd, "offset 9: PARKED at 7, not emptied");
  ok(marqueeStart(s, 12, 50, measure10, NULL, &atEnd) == 7 && atEnd, "offset 12 (whole string): parked at 7");
  ok(marqueeStart(s, 500, 50, measure10, NULL, &atEnd) == 7 && atEnd, "offset 500: parked at 7");
  /* Exactly fits: not a marquee case, but the function must not misbehave. */
  ok(marqueeStart("ABCDE", 0, 50, measure10, NULL, &atEnd) == 0 && atEnd, "a text that fits is at its end at 0");
  ok(marqueeStart("ABCDE", 3, 50, measure10, NULL, &atEnd) == 0 && atEnd, "...and any offset parks it at 0");
  /* One glyph over. */
  ok(marqueeStart("ABCDEF", 0, 50, measure10, NULL, &atEnd) == 0 && !atEnd, "6 in 5: offset 0 has more");
  ok(marqueeStart("ABCDEF", 1, 50, measure10, NULL, &atEnd) == 1 && atEnd, "6 in 5: offset 1 is the end");
  ok(marqueeStart("ABCDEF", 4, 50, measure10, NULL, &atEnd) == 1 && atEnd, "6 in 5: offset 4 parks at 1");

  group("marqueeStart: UTF-8 offsets are byte offsets on glyph boundaries");
  /* é€😀 then ASCII: 3 multi-byte glyphs (9 bytes) + "abcdef" (6) = 9 glyphs, 90 px, in 40 px. */
  const char* u = "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" "abcdef";
  ok(marqueeStart(u, 1, 40, measure10, NULL, &atEnd) == 2 && !atEnd, "past é: byte 2");
  ok(marqueeStart(u, 2, 40, measure10, NULL, &atEnd) == 5 && !atEnd, "past €: byte 5");
  ok(marqueeStart(u, 3, 40, measure10, NULL, &atEnd) == 9 && !atEnd, "past the emoji: byte 9, 'abcdef' 60 px");
  ok(marqueeStart(u, 5, 40, measure10, NULL, &atEnd) == 11 && atEnd, "offset 5: 'cdef' 40 px, the end");
  ok(marqueeStart(u, 8, 40, measure10, NULL, &atEnd) == 11 && atEnd, "offset 8 parks at byte 11");

  group("a whole row's life: short title, long subtitle, one offset");
  /* Title 7 glyphs, subtitle 12, both in 50 px. The title parks at 2; the subtitle ends at 7. */
  const char* title = "ABCDEFG";
  const char* sub = "abcdefghijkl";
  bool anyMore = true;
  int steps = 0;
  marqueeReset(&c, 0);
  uint32_t now = MARQUEE_HOLD_START_MS;   // the first step
  bool titleEnd = false, subEnd = false;
  size_t tb = 0, sb = 0;
  while (steps < 100) {
    if (!marqueeAdvance(&c, now, anyMore)) {
      if (c.phase == MARQUEE_HOLD_END) {
        break;
      }
      now += MARQUEE_STEP_MS;
      continue;
    }
    steps++;
    /* "the draw": both texts consult the same offset. */
    tb = marqueeStart(title, c.offset, 50, measure10, NULL, &titleEnd);
    sb = marqueeStart(sub, c.offset, 50, measure10, NULL, &subEnd);
    anyMore = !titleEnd || !subEnd;
    now += MARQUEE_STEP_MS;
  }
  ok(steps == 7, "seven steps to bring a 12-glyph subtitle's end into 5 glyphs of room");
  ok(c.offset == 7 && c.phase == MARQUEE_HOLD_END, "then the end hold, at offset 7");
  ok(tb == 2 && titleEnd, "the 7-glyph title parked at its own end (2) and stayed there");
  ok(sb == 7 && subEnd, "the subtitle's last five glyphs are on screen");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
