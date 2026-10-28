// test_music_title.cpp - where the Now playing title breaks into two lines (WiPhone/music_title.h).
//
// The probe loop in MusicApp::drawNowPlaying() ran `k < len`, never measured the whole title, and
// so split EVERY title - its last character alone on line two ("My_Name_I" / "s"; the "e" under
// "Pulse" in {user}'s photo, 2026-09-26). A fake monospace face (8 px a character) makes the cases
// exact.
#include <stdio.h>
#include <string.h>
#include "../WiPhone/music_title.h"

static int passed = 0, failed = 0;
static void check(bool ok, const char* what) {
  if (ok) {
    passed++;
  } else {
    failed++;
    printf("  FAIL: %s\n", what);
  }
}

static uint16_t mono8(const char*, size_t n, void*) { return (uint16_t)(8 * n); }
static uint16_t wide(const char* s, size_t n, void*) {
  uint16_t w = 0;                                   // 'W' is 16 px, everything else 8
  for (size_t i = 0; i < n; i++) w += s[i] == 'W' ? 16 : 8;
  return w;
}

int main() {
  // 228 px = 28 characters of 8 px, the panel's width less the margins.
  const uint16_t W = 228;
  const char* one = "My_Name_Is";                    // 10 chars: fits
  check(musicTitleFit(one, strlen(one), W, mono8, NULL) == strlen(one),
        "a title that fits is ONE line: fit == len (the old loop stopped one short)");
  const char* exact = "abcdefghijklmnopqrstuvwxyz12";   // 28 chars = 224 px: fits exactly
  check(musicTitleFit(exact, 28, W, mono8, NULL) == 28, "a title of exactly the line width fits");
  const char* over = "abcdefghijklmnopqrstuvwxyz123";   // 29 chars = 232 px: one too many
  check(musicTitleFit(over, 29, W, mono8, NULL) == 28, "one character over: the 28 that fit");
  check(musicTitleFit("", 0, W, mono8, NULL) == 0, "an empty title fits nothing");
  check(musicTitleFit("x", 1, 4, mono8, NULL) == 1, "a title too wide for even one glyph still shows one");
  check(musicTitleFit("WWWW", 4, 40, wide, NULL) == 2, "a proportional face: measured, not counted");

  // The break, when the title does not fit: the last space in the second half of the fit.
  const char* cc = "Creedence Clearwater Revival - Have You Ever Seen the Rain";   // 58 chars
  size_t fit = musicTitleFit(cc, strlen(cc), W, mono8, NULL);
  check(fit == 28, "CCR: 28 characters fit ('Creedence Clearwater Revival')");
  check(musicTitleBreak(cc, fit) == 28, "CCR breaks at the space right after 'Revival' (index 28)");
  check(musicTitleBreak("abcdefghijklmnopqrstuvwxyz123456", 28) == 28, "no space: the cut itself");
  check(musicTitleBreak("ab cdefghijklmnopqrstuvwxyz123", 28) == 28,
        "a space in the FIRST half (index 2) is too early to use: the cut itself");
  check(musicTitleBreak("abcdefghijklmn opqrstuvwxyz123", 28) == 28,
        "a space AT the midpoint (index 14 = fit/2) is outside the search (i > fit/2): the cut itself");
  check(musicTitleBreak("abcdefghijklmno pqrstuvwxyz123", 28) == 15, "a space just past the midpoint is used");
  check(musicTitleBreak("abcdefghijklmnopqrstuvwxyz1 3", 28) == 27, "a space right at the cut is used");

  printf("%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
