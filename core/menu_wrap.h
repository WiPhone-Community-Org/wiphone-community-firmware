/* menu_wrap.h — breaking a display-only menu note into rows that fit, kept in a header of its
 * own so the HOST SUITE can test the code the firmware executes (the menu_marquee.h pattern).
 *
 * WHAT IT IS: a note whose length the numbers decide — "12853 tiles, 1606 MB, about 257 min",
 * "Last run: 5517 new, 7332 already had, 4 failed, 7013 s", an error handed up from the tile
 * fetcher — used to be one row ending in `..`, and the part cut off was the part that mattered
 * ({user}, on the Maps download screen, 2026-09-20). The selected-row marquee does not help: a
 * note is read, not selected. So it wraps: break at the last space that fits, next row, until
 * the text is out. MenuWidget::addNoteWrapped (GUI.cpp) supplies the font measurement and adds
 * the rows; this file holds the breaking, which is the part that can be wrong quietly (a break
 * inside a word, an infinite loop on a word wider than the row, a lost last word).
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header. Keep it that way; that constraint
 * is what makes it testable at all.
 */
#ifndef MENU_WRAP_H
#define MENU_WRAP_H

#include <stddef.h>
#include <string.h>

/* How many bytes of `s` fit on one row (SmoothFont::fitTextLength on the phone). Must be a
 * UTF-8 boundary; 0 when not even one glyph fits. */
typedef size_t (*WrapFitFn)(const char* s, void* ctx);
/* One finished row, `len` bytes of `s`, NOT NUL-terminated. */
typedef void (*WrapRowFn)(const char* s, size_t len, void* ctx);

/* Break `text` into rows. Leading spaces of a row are dropped; a row breaks at the last space
 * within what fits, or mid-word when a single word is wider than the row (never a zero-length
 * row, so the loop always advances). At most `maxRows` rows; returns the number emitted. */
static inline int wrapNote(const char* text, WrapFitFn fit, WrapRowFn row, void* ctx, int maxRows) {
  int rows = 0;
  const char* p = text;
  if (!p) {
    return 0;
  }
  while (*p && rows < maxRows) {
    while (*p == ' ') {
      p++;
    }
    if (!*p) {
      break;
    }
    const size_t len = strlen(p);
    size_t f = fit(p, ctx);
    if (f >= len) {
      row(p, len, ctx);
      rows++;
      break;
    }
    size_t cut = f;
    while (cut > 0 && p[cut] != ' ') {
      cut--;
    }
    if (cut == 0) {
      cut = f ? f : 1;                       /* a word wider than the row: cut it where it is */
    }
    row(p, cut, ctx);
    rows++;
    p += cut;
  }
  return rows;
}

#endif /* MENU_WRAP_H */
