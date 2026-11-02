/* menu_marquee.h — the arithmetic behind the scrolling selected row in every menu list,
 * kept in a header of its own so the HOST SUITE can test the code the firmware executes.
 *
 * WHAT IT IS: a row title (or subtitle) wider than the space it is given used to end in ".."
 * for ever — a book called "Ghosts_of_Timkovichi.epub", a node's "1.2km NE of camp, 4 min
 * ago", a message preview. Now the SELECTED row scrolls: hold the start, advance one glyph at
 * a time, hold the end, jump back, repeat. Unselected rows keep the "..". The drawing lives in
 * GUI.cpp (drawRowText / MenuWidget::redraw / MenuWidget::marqueeTick); this file holds the
 * two things that are pure enough to prove on a Mac: the phase clock and the glyph stepping.
 *
 * WHY WHOLE GLYPHS AND NOT PIXELS: the vendored TFT_eSPI has no clip rectangle. drawString on
 * a string wider than the space left to the screen edge does not overflow — it SHIFTS THE
 * START LEFT (`if (poX+cwidth > width()) poX = width() - cwidth`, TFT_eSPI.cpp ~4460), so a
 * pixel-scrolled string would lose glyphs at BOTH ends. Stepping by glyph and drawing only
 * what fits needs no clipping, no extra sprite and no allocation: the row is repainted through
 * the ordinary REDRAW_SCREEN path exactly as a key press would repaint it.
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header. Keep it that way; that constraint
 * is what makes it testable at all.
 */
#ifndef MENU_MARQUEE_H
#define MENU_MARQUEE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* The rhythm. 200 ms a glyph is 5 Hz — each step is a full sprite push (~30 ms of SPI at any
 * CPU clock), so the rate is what bounds the cost; see the bench numbers in the handoff. */
#define MARQUEE_HOLD_START_MS  800u
#define MARQUEE_STEP_MS        200u
#define MARQUEE_HOLD_END_MS    1000u

enum {
  MARQUEE_HOLD_START = 0,   /* offset 0, the row looks exactly like its unselected self */
  MARQUEE_SCROLL     = 1,   /* one glyph per MARQUEE_STEP_MS */
  MARQUEE_HOLD_END   = 2    /* the whole tail is on screen; let it be read */
};

typedef struct {
  uint16_t offset;     /* glyphs scrolled off the left edge */
  uint8_t  phase;      /* MARQUEE_* above */
  uint32_t phaseMs;    /* millis() when the current phase began */
} MarqueeClock;

static inline void marqueeReset(MarqueeClock* m, uint32_t now) {
  m->offset = 0;
  m->phase = MARQUEE_HOLD_START;
  m->phaseMs = now;
}

/* Advance the clock. `moreToCome` is what the LAST DRAW reported: at the current offset, some
 * text on the row has not yet shown its end. Returns true when the picture changed and the
 * row must be repainted; false while holding. Unsigned subtraction so millis() wrapping is a
 * non-event. */
static inline bool marqueeAdvance(MarqueeClock* m, uint32_t now, bool moreToCome) {
  const uint32_t dt = now - m->phaseMs;
  switch (m->phase) {
  case MARQUEE_HOLD_START:
    if (dt < MARQUEE_HOLD_START_MS) {
      return false;
    }
    m->phaseMs = now;
    if (!moreToCome) {            /* cannot happen for a row that overflowed; be safe anyway */
      m->phase = MARQUEE_HOLD_END;
      return false;
    }
    m->phase = MARQUEE_SCROLL;
    m->offset++;
    return true;
  case MARQUEE_SCROLL:
    if (dt < MARQUEE_STEP_MS) {
      return false;
    }
    m->phaseMs = now;
    if (moreToCome) {
      m->offset++;
      return true;
    }
    m->phase = MARQUEE_HOLD_END;
    return false;
  default:                        /* MARQUEE_HOLD_END */
    if (dt < MARQUEE_HOLD_END_MS) {
      return false;
    }
    marqueeReset(m, now);
    return true;
  }
}

/* Byte offset of the n-th glyph of a UTF-8 string, clamped to its end. Continuation bytes
 * (10xxxxxx) never start a glyph — a cut inside a sequence draws a garbage box, and book
 * titles and track names really do carry accents. */
static inline size_t marqueeGlyphOffset(const char* s, uint16_t n) {
  size_t b = 0;
  while (s[b] && n) {
    b++;
    while (((uint8_t)s[b] & 0xC0) == 0x80) {
      b++;
    }
    n--;
  }
  return b;
}

/* Pixel width of a NUL-terminated string in the row's font; `ctx` is whatever the caller
 * needs to measure with (the LCD, on the phone). */
typedef int (*MarqueeMeasure)(void* ctx, const char* s);

/* Where the visible part of `s` STARTS when `offset` glyphs have scrolled away, as a byte
 * offset to draw from. *atEnd says whether everything from there fits in maxW.
 *
 * The one rule that is not obvious: a text never scrolls PAST the point where its remainder
 * fits. A row can carry two texts of different lengths (a node's name and its "where/when"
 * line; a sender and a message preview) and one marquee offset drives both, so the shorter
 * one parks at its own end and waits there while the longer one finishes — instead of
 * emptying out. The park point is found by bisection over glyph offsets, because the tail's
 * width only shrinks as the offset grows: ~6 measurements instead of one per glyph. */
static inline size_t marqueeStart(const char* s, uint16_t offset, int maxW,
                                  MarqueeMeasure measure, void* ctx, bool* atEnd) {
  size_t b = marqueeGlyphOffset(s, offset);
  if (measure(ctx, s + b) > maxW) {
    *atEnd = false;
    return b;
  }
  *atEnd = true;
  /* The tail fits. Smallest k in [0, offset] whose tail fits; k = offset is known to. */
  uint16_t lo = 0, hi = offset;
  while (lo < hi) {
    const uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
    if (measure(ctx, s + marqueeGlyphOffset(s, mid)) > maxW) {
      lo = (uint16_t)(mid + 1);
    } else {
      hi = mid;
    }
  }
  return marqueeGlyphOffset(s, lo);
}

#endif /* MENU_MARQUEE_H */
