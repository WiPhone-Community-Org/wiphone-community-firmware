/*
 * elev_text.h - the words the map puts on its altitude readouts (docs/maps.md, "Altitude").
 * Pure: no Arduino, no heap, C++11 - tests/test_elevtext.cpp proves every wording, in metric
 * and US, and measures the rows against the phone's own AKROBAT_BOLD_16 glyph table.
 *
 * The strip's row (label "Elev") and the pin screen's note (label "Ground"):
 *   "Elev 1,352ft, 394ft above you"      a comparison with you (US: thousands comma)
 *   "Elev 412m, 35m below you"
 *   "Elev 412m, level with you"          |difference| under 1 m
 *   "Elev 412m, 35m above Camp"          no position of your own: the reference, named
 *   "Elev 412m"                          nothing to compare with
 *   "Elev 412m (coarse)"                 a height came from the ~100 m z10 layer (either one)
 *   "Elev* 1800m, 1300m above you"       ...the same, when " (coarse)" would not fit the strip:
 *                                        the star is the map's own mark for borrowed, coarser
 *                                        data ("z17*"). Measured, not guessed - see elevTextRow
 *   "Elev: no tile here" / "Elev: no data here" / "Elev: card read error"
 *   "Elev ..."                           not sampled for this view yet (the map is moving)
 * The ruler:
 *   "Ruler: 1.2km SE, climb +85m"        both ends have a height (the anchor's ground to the
 *   "Ruler: 1.2km SE, climb* +85m"       crosshair's; the star: either end came from z10),
 *   "Ruler: 1.2km SE of the anchor"      else the old wording
 */
#ifndef ELEV_TEXT_H
#define ELEV_TEXT_H

#include <stddef.h>

/* elevSample()'s results (1 ok, -1 no data, 0 no tile) plus the two the map adds: */
#define ELEV_TXT_IOERR    (-2)     // the card could not be read (elevSampleCard's ioErr)
#define ELEV_TXT_PENDING  (-3)     // not sampled for this view yet

/* The pixel width of a string in the font the row is drawn in (SmoothFont::textWidth). */
typedef int (*ElevTextMeasureFn)(void* ctx, const char* s);

struct ElevTextIn {
  const char* label;     // "Elev" when NULL; "Ground" on the pin screen
  int         rc;        // the point: 1 / -1 / 0 / ELEV_TXT_IOERR / ELEV_TXT_PENDING
  double      m;         // its height in metres (rc == 1)
  int         z;         // the layer that answered it (ELEV_Z_COARSE = "(coarse)")
  bool        haveRef;   // a height to compare with (ignored unless rc == 1)
  double      refM;
  int         refZ;      // its layer (ELEV_Z_COARSE = "(coarse)" too: a difference is as coarse
                         // as its coarser end)
  const char* refName;   // NULL or "" = "you"; else the reference's name
  int         units;     // UNITS_METRIC / UNITS_US
};

/* The row, NUL-terminated and cut to cap like snprintf. Returns strlen(out).
 * With `measure` and maxW > 0, a row wider than maxW gives way in this order, the numbers last:
 *   1. " (coarse)" becomes the star: "Elev* 1800m, 1300m above you" (" (coarse)" is ~57 px of
 *      the strip's 232, and "Elev 1800m, 1300m above you (coarse)" is 242 - measured);
 *   2. a reference's NAME is cut, on a character, with ".." ("above Hunting C..");
 *   3. nothing fits: the step-1 row with the whole name, for the phone's ellipsis
 *      (guiDrawEllipsized) - it cuts from the end, so the heights survive it.
 * Without `measure` it is always the full wording (the host tests pin every one). */
int elevTextRow(char* out, size_t cap, const ElevTextIn* in,
                ElevTextMeasureFn measure = NULL, void* mctx = NULL, int maxW = 0);

/* The ruler's row: distance and 8-point bearing from the anchor, and with haveClimb the climb
 * from the anchor's ground to the crosshair's, signed ("climb +85m", "climb -40m", "climb 0m").
 * `coarse`: either end's height came from the ~100 m z10 layer - "climb* +85m", the star the
 * strip's row and the map use for coarser data (a difference is as coarse as its coarser end;
 * " (coarse)" would be the first thing the row's ellipsis cut). Returns strlen(out). compass
 * NULL reads as "". */
int elevRulerText(char* out, size_t cap, double distM, const char* compass, bool haveClimb,
                  double climbM, int units, bool coarse);

/* One sample for the serial console: "412.3 m (z13)", "no tile", "no data (a hole in the
 * tile)", "card read error", "not sampled yet" (ELEV_TXT_PENDING). Returns strlen(out). */
int elevSampleText(char* out, size_t cap, int rc, double m, int z);

#endif // ELEV_TEXT_H
