/*
 * book_layout.h — turning a chapter's text into pages, without knowing what a screen is.
 *
 * The reader draws pages; this decides where they start and end. It is deliberately free of
 * both the LCD and the font: width comes in through a callback, so the whole of the tricky
 * part — wrapping, paragraph breaks, UTF-8, going BACKWARDS — is provable on a Mac. The only
 * thing left in app_books.cpp is drawing the lines it hands back.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * OFFSETS ARE BYTES INTO THE EXTRACTED TEXT
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * Everything here counts bytes, matching epubChapterText()'s return and epubChapterLen().
 * A page start is therefore usable directly as the `offset` half of a (spine, offset)
 * position. ⚠ COVEY counts CHARACTERS, so the two agree exactly only for ASCII; that drift
 * is pre-existing, documented in epub_parse.h, and absorbed by the whole-book fraction —
 * which is why a synced jump is confirmed rather than taken silently.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * WHY THERE IS NO PAGE NUMBER, AND NO PAGINATE-THE-WHOLE-CHAPTER PASS
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * A .txt book is often ONE spine item of half a megabyte. Laying that out up front to number
 * its pages would cost a full pass through every glyph before the first word appears. So
 * pages are cut lazily, forward from wherever you are, and going back a page is answered by
 * bookLayoutPrevPage(): it re-lays a short run of text that ENDS where you are now.
 *
 * ⚠ What that promises is CONTINUITY, not identity with a forward walk from the top of the
 * chapter. Greedy wrapping started at a different offset can stay permanently out of phase,
 * and blank-line handling depends on where a page begins, so a cold page-back may reflow the
 * lines. What it guarantees is the pair a reader can actually perceive: the page it returns
 * ENDS exactly where the current page begins, and it is a full page. Nothing is skipped and
 * nothing is shown twice. Exact back-paging through pages you just read forward comes from
 * the caller's own history of page starts (app_books keeps one) — not from re-deriving them.
 */
#ifndef BOOK_LAYOUT_H
#define BOOK_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#define BOOK_MAX_LINES  40        // hard ceiling on lines per page (240x320 fits ~11)

/* Pixel width of the UTF-8 run [s, s+len). On the phone this wraps SmoothFont::textWidth;
 * in the tests it is a fixed-width stub, which is what makes the breaking rules checkable. */
struct BookMeasure {
  void* ctx;
  int (*width)(void* ctx, const char* s, size_t len);
};

/* A picture, as the layout sees it: an offset in the text and a height in ROWS.
 *
 * Rows rather than pixels on purpose — the page budget is counted in rows, and keeping the
 * unit the same everywhere is what stops an image quietly overflowing the bottom of a page.
 * Working out how many rows a JPEG needs belongs to the caller, which is the only part that
 * knows the font's line height and what the decoder can scale to. */
struct BookImageBox {
  uint32_t off;                   // byte offset it belongs at, from epubChapterTextImages()
  uint16_t rows;                  // text rows it occupies (>= 1)
};

struct BookLine {
  uint32_t off;                   // byte offset into the chapter text
  uint16_t len;                   // bytes to draw (trailing space/newline already trimmed)
  bool     blank;                 // a paragraph gap: draw nothing, still costs a line
  int8_t   image;                 // index into the caller's image list, or -1 for text
  uint16_t rows;                  // rows this line occupies: 1 for text, more for an image
};

struct BookPage {
  uint32_t start;                 // byte offset this page begins at
  uint32_t next;                  // where the NEXT page begins; == textLen at the end
  int      nLines;
  BookLine lines[BOOK_MAX_LINES];
};

/* Cut one page of at most `maxLines` lines, each fitting in `widthPx`, starting at `start`.
 *
 * Rules, in the order they bite:
 *   - '\n' ends a line. A run of blank lines survives as `blank` lines (COVEY's extractor
 *     joins paragraphs with a blank line, and losing it turns a book into a wall of text),
 *     but a page never OPENS with one — leading blanks are skipped so a page turn does not
 *     spend its first rows on white space.
 *   - Otherwise break at the last space that fits.
 *   - A single word wider than the line is broken mid-word, on a UTF-8 boundary. Without
 *     that rule a long URL is an infinite loop, not a cosmetic problem.
 */
void bookLayoutPage(const char* text, size_t textLen, uint32_t start,
                    int widthPx, int maxLines, const BookMeasure* m, BookPage* out);

/* The same, with pictures placed in the flow. `imgs` must be sorted by offset.
 *
 * A picture is emitted when the page reaches its offset and takes `rows` from the budget. One
 * too tall for a whole page still gets a page of its own rather than being skipped — losing a
 * picture silently would be worse than a cropped one. */
void bookLayoutPageImages(const char* text, size_t textLen, uint32_t start,
                          int widthPx, int maxLines, const BookMeasure* m,
                          const BookImageBox* imgs, int nImgs, BookPage* out);

/* The start of a full page that ends exactly at `before`; 0 if `before` is already the top.
 *
 * Walks back a bounded guess, anchors on a paragraph start, lays out forward with the text
 * cut at `before`, and counts back `maxLines` LINES from the end. See the continuity note
 * above for what this does and does not promise. */
uint32_t bookLayoutPrevPage(const char* text, size_t textLen, uint32_t before,
                            int widthPx, int maxLines, const BookMeasure* m);

uint32_t bookLayoutPrevPageImages(const char* text, size_t textLen, uint32_t before,
                                  int widthPx, int maxLines, const BookMeasure* m,
                                  const BookImageBox* imgs, int nImgs);

/* Snap an arbitrary offset (a restored or synced position) back to the start of its word, so
 * resuming never opens mid-word. Never moves more than 64 bytes. */
uint32_t bookLayoutSnap(const char* text, size_t textLen, uint32_t off);

#endif // BOOK_LAYOUT_H
