/*
 * book_layout.cpp — see book_layout.h.
 *
 * Widths are accumulated per CHARACTER and treated as additive, which is exactly how
 * SmoothFont::textWidth works (a sum of per-glyph gxAdvance). That keeps finding a line
 * break O(characters) instead of O(characters x measure), and it means the host tests can
 * use a fixed-width stub and still exercise the real breaking rules.
 */
#include "book_layout.h"

#include <string.h>

static inline bool blSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r';
}

// Bytes in the UTF-8 sequence starting at `p`, never running past `limit`.
static size_t blCharLen(const char* text, size_t p, size_t limit) {
  unsigned char c = (unsigned char)text[p];
  size_t n = 1;
  if ((c & 0xE0) == 0xC0) {
    n = 2;
  } else if ((c & 0xF0) == 0xE0) {
    n = 3;
  } else if ((c & 0xF8) == 0xF0) {
    n = 4;
  }
  if (p + n > limit) {
    n = 1;                      // truncated or malformed: step one byte, never past the end
  }
  return n;
}

void bookLayoutPage(const char* text, size_t textLen, uint32_t start,
                    int widthPx, int maxLines, const BookMeasure* m, BookPage* out) {
  bookLayoutPageImages(text, textLen, start, widthPx, maxLines, m, NULL, 0, out);
}

void bookLayoutPageImages(const char* text, size_t textLen, uint32_t start,
                          int widthPx, int maxLines, const BookMeasure* m,
                          const BookImageBox* imgs, int nImgs, BookPage* out) {
  if (maxLines > BOOK_MAX_LINES) {
    maxLines = BOOK_MAX_LINES;
  }
  if (maxLines < 1) {
    maxLines = 1;
  }
  if (widthPx < 1) {
    widthPx = 1;
  }

  size_t p = (start > textLen) ? textLen : start;
  // A page never opens on white space: a turn that spends its first rows on a paragraph gap
  // reads as a bug, and the skipped bytes are already behind the reader either way.
  while (p < textLen && (text[p] == '\n' || blSpace(text[p]))) {
    p++;
  }

  out->start = (uint32_t)p;
  out->nLines = 0;

  /* Pictures on this page are those at or after where the page was ASKED to begin — not
   * after out->start, which the white-space skip above may have moved forward. A picture
   * sitting on a paragraph gap would otherwise fall in the crack between the two and never
   * be shown at all. */
  const uint32_t askedFrom = (start > textLen) ? (uint32_t)textLen : start;
  int ii = 0;
  while (imgs && ii < nImgs && imgs[ii].off < askedFrom) {
    ii++;
  }
  /* ⚠ A picture is never allowed the WHOLE page. If it were, a page could hold the picture
   * and no text, `next` would not advance past `start`, and the reader would sit on that page
   * for ever — the caller reads next <= start as "end of chapter". Capping at maxLines-1
   * guarantees room for at least one line of text and kills the whole class of problem. It
   * costs one row of height; the number keys open the picture full-screen anyway. */
  const int imgRowCap = maxLines > 1 ? maxLines - 1 : 1;

  int rowsUsed = 0;
  bool lastWasBlank = false;
  /* The loop runs while there is budget left OR nothing has been consumed yet. That second
   * clause is the anti-deadlock rule: a page MUST advance past its own start, or the caller
   * — which reads next <= start as "end of chapter" — parks the reader on it for ever. It
   * only ever fires in the degenerate case of a picture on a one-row page. */
  while (p < textLen && (rowsUsed < maxLines || p == (size_t)out->start)) {
    // Any picture we have now reached, before the text that follows it.
    bool pageFull = false;
    if (rowsUsed < maxLines) {
      while (imgs && ii < nImgs && imgs[ii].off <= (uint32_t)p && out->nLines < BOOK_MAX_LINES) {
        int rows = imgs[ii].rows < 1 ? 1 : imgs[ii].rows;
        if (rows > imgRowCap) {
          rows = imgRowCap;
        }
        if (rowsUsed + rows > maxLines) {
          pageFull = true;            // it belongs on the next page, which starts here
          break;
        }
        out->lines[out->nLines].off = imgs[ii].off;
        out->lines[out->nLines].len = 0;
        out->lines[out->nLines].blank = false;
        out->lines[out->nLines].image = (int8_t)ii;
        out->lines[out->nLines].rows = (uint16_t)rows;
        out->nLines++;
        rowsUsed += rows;
        lastWasBlank = false;
        ii++;
      }
    }
    if (pageFull) {
      /* The picture does not fit, so it opens the NEXT page — which means this page has to
       * end at or before its offset. Ending past it loses it entirely: the next page would
       * skip anything behind its own start, and the picture would vanish with nothing
       * logged. (A 264x264 illustration disappeared exactly this way, found by laying out a
       * real chapter rather than by any of the synthetic tests.) */
      const uint32_t want = imgs[ii].off;
      while (out->nLines > 0) {
        BookLine* last = &out->lines[out->nLines - 1];
        if (last->image >= 0 || (uint32_t)(last->off + last->len) <= want) {
          break;
        }
        rowsUsed -= last->rows ? last->rows : 1;   // its text straddles the picture: re-lay it
        p = last->off;
        out->nLines--;
      }
      if (p > (size_t)want) {
        p = want;
      }
      break;
    }
    if (out->nLines >= BOOK_MAX_LINES) {
      break;
    }
    if (rowsUsed >= maxLines && p > (size_t)out->start) {
      break;                          // full, and we have advanced: a clean page end
    }

    const size_t lineStart = p;

    size_t nl = lineStart;                    // hard stop at the newline
    while (nl < textLen && text[nl] != '\n') {
      nl++;
    }

    // Take characters while they fit. The first character always goes on, or a glyph wider
    // than the whole line would stall here forever.
    size_t q = lineStart;
    int w = 0;
    size_t lastSpace = 0;
    bool haveSpace = false;
    while (q < nl) {
      size_t clen = blCharLen(text, q, nl);
      int cw = m->width(m->ctx, text + q, clen);
      if (w + cw > widthPx && q > lineStart) {
        /* A space that does not fit is a BREAK, not an overflow: the words before it filled
         * the line exactly, and refusing it would push the last word to the next line for no
         * reason a reader can see. (Caught by the "exactly-filling line" test.) */
        if (blSpace(text[q])) {
          lastSpace = q;
          haveSpace = true;
        }
        break;
      }
      w += cw;
      if (blSpace(text[q])) {
        lastSpace = q;
        haveSpace = true;
      }
      q += clen;
    }

    size_t lineEnd;                           // exclusive end of what gets drawn
    size_t nextStart;
    if (q >= nl) {                            // the rest of the paragraph fit
      lineEnd = nl;
      nextStart = (nl < textLen) ? nl + 1 : nl;         // step over the newline
    } else if (haveSpace && lastSpace > lineStart) {
      lineEnd = lastSpace;                    // break at the last space that fit
      nextStart = lastSpace + 1;
    } else {
      lineEnd = q;                            // one word wider than the line: break inside it
      nextStart = q;                          // (already on a UTF-8 boundary)
    }

    while (lineEnd > lineStart && blSpace(text[lineEnd - 1])) {
      lineEnd--;                              // trailing space would only smear the margin
    }

    if (lineEnd == lineStart) {
      // A paragraph gap. Collapse a run of them into one: some EPUBs stack empty <p>s, and
      // four blank rows in the middle of a 11-row page is a hole, not a paragraph break.
      while (nextStart < textLen && text[nextStart] == '\n') {
        nextStart++;
      }
      if (lastWasBlank) {                     // never two in a row
        p = nextStart;
        continue;
      }
      out->lines[out->nLines].off = (uint32_t)lineStart;
      out->lines[out->nLines].len = 0;
      out->lines[out->nLines].blank = true;
      out->lines[out->nLines].image = -1;
      out->lines[out->nLines].rows = 1;
      out->nLines++;
      rowsUsed++;
      lastWasBlank = true;
      p = nextStart;
      continue;
    }

    out->lines[out->nLines].off = (uint32_t)lineStart;
    out->lines[out->nLines].len = (uint16_t)(lineEnd - lineStart);
    out->lines[out->nLines].blank = false;
    out->lines[out->nLines].image = -1;
    out->lines[out->nLines].rows = 1;
    out->nLines++;
    rowsUsed++;
    lastWasBlank = false;
    p = nextStart;
  }

  // A gap as the last row is wasted height, and the reader has already passed it.
  if (out->nLines > 0 && out->lines[out->nLines - 1].blank) {
    out->nLines--;
  }
  out->next = (uint32_t)p;
}

uint32_t bookLayoutPrevPage(const char* text, size_t textLen, uint32_t before,
                            int widthPx, int maxLines, const BookMeasure* m) {
  return bookLayoutPrevPageImages(text, textLen, before, widthPx, maxLines, m, NULL, 0);
}

uint32_t bookLayoutPrevPageImages(const char* text, size_t textLen, uint32_t before,
                                  int widthPx, int maxLines, const BookMeasure* m,
                                  const BookImageBox* imgs, int nImgs) {
  if (before == 0) {
    return 0;
  }
  if (before > textLen) {
    before = (uint32_t)textLen;
  }
  if (maxLines < 1) {
    maxLines = 1;
  }
  if (widthPx < 1) {
    widthPx = 1;
  }

  // Guess how far back a page reaches, from what the text right here actually measures.
  // Too small only costs a short page; too large only costs a little more laying out.
  size_t sampleLen = before < 64 ? before : 64;
  int sampleW = m->width(m->ctx, text + (before - sampleLen), sampleLen);
  size_t guess;
  if (sampleW > 0) {
    guess = (size_t)((double)widthPx * (double)(maxLines + 4) * (double)sampleLen / (double)sampleW);
  } else {
    guess = (size_t)maxLines * 80;
  }
  if (guess < (size_t)maxLines * 8) {
    guess = (size_t)maxLines * 8;
  }
  if (guess > 16384) {
    guess = 16384;
  }

  /* Lay out forward with the text CUT at `before`, and count back `maxLines` LINES from the
   * end. Counting back whole pages instead was the first attempt and it is wrong: a run of
   * text that is not a whole number of pages long leaves the last page short, so going back
   * landed a few lines above where the current page starts rather than a full page above.
   * Lines are the unit that survives an arbitrary starting point. */
  const int ringSize = BOOK_MAX_LINES + 1;
  uint32_t ring[BOOK_MAX_LINES + 1];
  uint16_t ringRows[BOOK_MAX_LINES + 1];      // a picture is several rows tall, not one
  uint32_t cand = 0;
  int total = 0;

  for (int attempt = 0; attempt < 3; attempt++) {
    cand = (before > guess) ? (uint32_t)(before - guess) : 0;
    cand = bookLayoutSnap(text, textLen, cand);

    /* Anchor on a paragraph start if one is within reach. This is what makes going back
     * EXACT rather than merely continuous: greedy wrapping started at an arbitrary offset
     * can stay permanently out of phase with the real line sequence, but no line ever spans
     * a newline, so a paragraph start is a line start in every possible layout. The top of
     * the text is just as good an anchor. Only a paragraph longer than the window falls back
     * to the word snap, and there the page is still continuous — just possibly reflowed. */
    const uint32_t window = 4096;
    uint32_t floorOff = (cand > window) ? cand - window : 0;
    uint32_t anchor = cand;
    while (anchor > floorOff && text[anchor - 1] != '\n') {
      anchor--;
    }
    if (anchor > floorOff || floorOff == 0) {
      cand = anchor;                // landed on a paragraph start, or walked back to the top
    }
    total = 0;

    uint32_t at = cand;
    BookPage pg;
    for (int guard = 0; guard < 64; guard++) {
      bookLayoutPageImages(text, before, at, widthPx, maxLines, m, imgs, nImgs, &pg);
      for (int i = 0; i < pg.nLines; i++) {
        ring[total % ringSize] = pg.lines[i].off;
        ringRows[total % ringSize] = pg.lines[i].rows ? pg.lines[i].rows : 1;
        total++;
      }
      if (pg.next >= before || pg.next <= at) {
        break;
      }
      at = pg.next;
    }
    if (total > maxLines || cand == 0) {
      break;                        // enough lines in hand, or there is no more text behind us
    }
    guess *= 3;                     // the guess was short: reach further back and re-lay
    if (guess > 65536) {
      break;
    }
  }

  if (total == 0) {
    return cand;
  }

  /* Counting back maxLines lines is the right first guess but not a guarantee: a blank line
   * that fell at the END of a page during the walk was dropped from that page's rows, so
   * re-laying one page from the counted-back offset can come up a row short and leave a line
   * of text stranded between the two pages. Caught by paging a real book backwards — 3 of 72
   * boundaries dropped a word. So verify, and step forward a line at a time until the page
   * actually reaches `before`. Two or three extra layouts, on a key press. */
  // Count back a page's worth of ROWS, not of lines — a picture is several rows tall.
  int first = total;
  int rows = 0;
  while (first > 0 && total - first < ringSize - 1) {
    int r = ringRows[(first - 1) % ringSize];
    if (rows + r > maxLines) {
      break;
    }
    rows += r;
    first--;
  }
  uint32_t best = ring[first % ringSize];
  for (int i = first; i < total; i++) {
    uint32_t start = ring[i % ringSize];
    BookPage check;
    bookLayoutPageImages(text, before, start, widthPx, maxLines, m, imgs, nImgs, &check);
    if (check.next >= before) {
      return start;                 // this page reaches where we came from: nothing stranded
    }
    best = start;
  }
  return best;
}

uint32_t bookLayoutSnap(const char* text, size_t textLen, uint32_t off) {
  if (off == 0) {
    return 0;
  }
  if (off > textLen) {
    off = (uint32_t)textLen;
  }
  // Already on white space? Then we are on a boundary, not inside a word: stay put. (Walking
  // back from a space would jump to the START of the word before it, losing a word.)
  if (off < textLen && (text[off] == '\n' || blSpace(text[off]))) {
    return off;
  }
  // Back onto a UTF-8 boundary first, so the word scan never sees half a character.
  int steps = 0;
  while (off > 0 && ((unsigned char)text[off] & 0xC0) == 0x80 && steps < 4) {
    off--;
    steps++;
  }
  steps = 0;
  while (off > 0 && text[off - 1] != '\n' && !blSpace(text[off - 1]) && steps < 64) {
    off--;
    steps++;
  }
  return off;
}
