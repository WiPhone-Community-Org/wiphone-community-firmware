/* music_title.h - where the Now playing title breaks into its two lines. Pure: the width of a
 * prefix comes from a callback, so the host suite runs it (tests/test_music_title.cpp).
 *
 * 🛑 The loop lived in MusicApp::drawNowPlaying() as `for (k = 1; k < len; k++)`: it never
 * measured the WHOLE title, so `cut` could never reach `len`, and every title that fit on one
 * line still split - its last character alone on line two ("My_Name_I" / "s"; the "e" under
 * "Pulse" in {user}'s photo, 2026-09-26). The probe runs to `len` inclusive here. */
#ifndef MUSIC_TITLE_H
#define MUSIC_TITLE_H

#include <stddef.h>
#include <stdint.h>

/* Pixel width of the first `n` characters of `s`. */
typedef uint16_t (*MusicTitleWidth)(const char* s, size_t n, void* ctx);

/* Characters of `s` (length `len`) that fit in `lineW` pixels: `len` when the whole title does,
 * otherwise the longest prefix that fits, never 0 for a non-empty title (one character shows
 * even when it overruns; the ellipsizer marks it). */
static inline size_t musicTitleFit(const char* s, size_t len, uint16_t lineW, MusicTitleWidth widthOf,
                                   void* ctx) {
  if (len == 0) {
    return 0;
  }
  size_t fit = 0;
  for (size_t k = 1; k <= len; k++) {
    if (widthOf(s, k, ctx) > lineW) {
      break;
    }
    fit = k;
  }
  return fit == 0 ? 1 : fit;
}

/* Where line one ends when the title does NOT fit: on the last space in the second half of the
 * fitting prefix, so a title splits between words rather than mid-syllable; the cut itself
 * when there is none. `fit` is musicTitleFit()'s answer and is < len. */
static inline size_t musicTitleBreak(const char* s, size_t fit) {
  for (size_t i = fit; i > fit / 2; i--) {
    if (s[i] == ' ') {
      return i;
    }
  }
  return fit;
}

#endif  // MUSIC_TITLE_H
