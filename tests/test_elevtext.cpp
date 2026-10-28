/*
 * test_elevtext.cpp - elev_text.cpp on the host: every wording the map's altitude readouts use
 * (docs/maps.md, "Altitude"), in metric and US, and the rows MEASURED against the phone's own
 * font. The strip draws them in AKROBAT_BOLD_16 at vpW - 8 = 232 px; the glyph table here is
 * the firmware's own (WiPhone/src/assets/fonts.h, Akrobat_Bold16), read the way
 * SmoothFont::loadFont() reads it and measured the way SmoothFont::textWidth() measures - so
 * "fits" here is "fits" on the glass, not a character count.
 */

#include "../WiPhone/elev_text.h"
#include "../WiPhone/elev_tiles.h"
#include "../WiPhone/units.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define PROGMEM
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../WiPhone/src/assets/fonts.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

static bool is(const char* got, const char* want) {
  if (strcmp(got, want) != 0) {
    printf("    got \"%s\", want \"%s\"\n", got, want);
    return false;
  }
  return true;
}

// ── the phone's font, as SmoothFont reads it (TFT_eSPI.cpp: loadFont(const unsigned char*)) ──

struct Font7 {
  int      n;
  uint16_t uni[256];
  uint8_t  w[256], adv[256];
  int8_t   dx[256];
  int      space;
  bool     ok;
};

static uint32_t runDecode(const unsigned char** p) {       // display::runDecodeNumber
  uint32_t v = 0;
  unsigned char more = 0;
  do {
    v <<= 7;
    v |= (**p) & 0x7F;
    more = (**p) & 0x80;
    (*p)++;
  } while (more);
  return v;
}

static void fontLoad(Font7* f, const unsigned char* data) {
  memset(f, 0, sizeof(*f));
  if (data[0] != '7' || data[1] != 'S' || data[2] != 'F') {
    return;
  }
  const unsigned char* p = data + 3;
  f->n = (int)runDecode(&p);
  runDecode(&p);                                           // point size
  const int ascent = (int)runDecode(&p);
  const int descent = (int)runDecode(&p);
  if (f->n <= 0 || f->n > 256) {
    return;
  }
  for (int g = 0; g < f->n; g++) {
    f->uni[g] = (uint16_t)(p[0] | (p[1] << 8));            // (uint16_t)*((uint32_t*)p), little-endian
    p += 4;
    runDecode(&p);                                         // height
    f->w[g] = (uint8_t)runDecode(&p);
    f->adv[g] = (uint8_t)runDecode(&p);
    runDecode(&p);                                         // dY
    f->dx[g] = (int8_t)runDecode(&p);
  }
  f->space = (ascent + descent) * 2 / 7;                   // loadFont's "guess at space width"
  f->ok = true;
}

static bool fontGlyph(const Font7* f, uint16_t u, int* g) {
  for (int i = 0; i < f->n; i++) {
    if (f->uni[i] == u) {
      *g = i;
      return true;
    }
  }
  return false;
}

// SmoothFont::textWidth, for the ASCII these rows are made of.
static int textWidth(void* ctx, const char* s) {
  const Font7* f = (const Font7*)ctx;
  int w = 0;
  for (const char* p = s; *p; p++) {
    const uint16_t u = (unsigned char)*p;
    if (u == 0x20) {
      w += f->space;
      continue;
    }
    int g = 0;
    if (fontGlyph(f, u, &g)) {
      if (w == 0 && f->dx[g] < 0) {
        w -= f->dx[g];
      }
      w += p[1] ? f->adv[g] : (f->dx[g] + f->w[g]);
    } else {
      w += f->space + 1;
    }
  }
  return w;
}

static bool allGlyphs(const Font7* f, const char* s) {
  for (const char* p = s; *p; p++) {
    int g = 0;
    if (*p != ' ' && !fontGlyph(f, (unsigned char)*p, &g)) {
      printf("    no glyph for '%c' in \"%s\"\n", *p, s);
      return false;
    }
  }
  return true;
}

static Font7 g_font;
static const int STRIP_W = 240 - 8;        // vpW - 8, the width drawBottomStrip ellipsizes to

static bool fits(const char* s) {
  const int w = textWidth(&g_font, s);
  if (w > STRIP_W) {
    printf("    \"%s\" is %d px, the strip has %d\n", s, w, STRIP_W);
    return false;
  }
  return true;
}

// ── helpers ─────────────────────────────────────────────────────────────────────────────

static char g_row[128];

static ElevTextIn point(int rc, double m, int z, int units) {
  ElevTextIn in;
  memset(&in, 0, sizeof(in));
  in.rc = rc;
  in.m = m;
  in.z = z;
  in.units = units;
  in.refZ = ELEV_Z;
  return in;
}

static const char* row(const ElevTextIn& in) {
  elevTextRow(g_row, sizeof(g_row), &in);
  return g_row;
}

static const char* rowVs(double here, double ref, int units, const char* name = NULL,
                         int z = ELEV_Z, int refZ = ELEV_Z) {
  ElevTextIn in = point(1, here, z, units);
  in.haveRef = true;
  in.refM = ref;
  in.refZ = refZ;
  in.refName = name;
  elevTextRow(g_row, sizeof(g_row), &in);
  return g_row;
}

static const char* ruler(double d, const char* c, bool climb, double up, int units,
                         bool coarse = false) {
  elevRulerText(g_row, sizeof(g_row), d, c, climb, up, units, coarse);
  return g_row;
}

int main() {
  printf("test_elevtext\n");

  fontLoad(&g_font, Akrobat_Bold16);
  CHECK(g_font.ok && g_font.n > 90, "the firmware's Akrobat_Bold16 glyph table loads");
  {
    /* Sanity for the measurer: a string's width is the sum SmoothFont computes, so two rows the
     * map has drawn for months must come out as they do on the glass - both fit, and the widest
     * help row (measured 203 px when it was written, app_maps.cpp) is 203 here. */
    CHECK(textWidth(&g_font, "grey square  no tile at any level") == 203,
          "the measurer agrees with the 203 px app_maps.cpp measured for a help row");
  }

  // ── the point alone ───────────────────────────────────────────────────────────────────
  CHECK(is(row(point(1, 412.3, ELEV_Z, UNITS_METRIC)), "Elev 412m"), "metric, nothing to compare with");
  CHECK(is(row(point(1, 412.3, ELEV_Z, UNITS_US)), "Elev 1,353ft"), "US: 412.3 m is 1,353 ft, comma");
  CHECK(is(row(point(1, 4392.0, ELEV_Z, UNITS_METRIC)), "Elev 4392m"), "metric has no thousands comma");
  CHECK(is(row(point(1, 412.3, ELEV_Z_COARSE, UNITS_METRIC)), "Elev 412m (coarse)"),
        "the z10 layer says (coarse)");
  CHECK(is(row(point(1, -27.6, ELEV_Z, UNITS_METRIC)), "Elev -28m"), "below sea level keeps its sign");
  CHECK(is(row(point(1, 0.3, ELEV_Z, UNITS_METRIC)), "Elev 0m"), "sea level is 0m, never -0");
  CHECK(is(row(point(0, 0, -1, UNITS_METRIC)), "Elev: no tile here"), "no tile");
  CHECK(is(row(point(-1, 0, -1, UNITS_US)), "Elev: no data here"), "a tile, but no data (either units)");
  CHECK(is(row(point(ELEV_TXT_IOERR, 0, -1, UNITS_METRIC)), "Elev: card read error"),
        "the card could not be read: said, not passed off as a hole");
  CHECK(is(row(point(ELEV_TXT_PENDING, 0, -1, UNITS_METRIC)), "Elev ..."), "not sampled for this view yet");
  {
    ElevTextIn in = point(1, 412.3, ELEV_Z, UNITS_METRIC);
    in.label = "Ground";
    CHECK(is(row(in), "Ground 412m"), "the pin screen's label");
    in.rc = 0;
    CHECK(is(row(in), "Ground: no tile here"), "...and its no-tile wording");
  }
  {
    /* A comparison on a point that has no height is no comparison: the row is the point's. */
    ElevTextIn in = point(0, 0, -1, UNITS_METRIC);
    in.haveRef = true;
    in.refM = 100;
    in.refName = "Camp";
    CHECK(is(row(in), "Elev: no tile here"), "no tile here, whatever 'you' is");
  }

  // ── against you ───────────────────────────────────────────────────────────────────────
  CHECK(is(rowVs(412.3, 377.1, UNITS_METRIC), "Elev 412m, 35m above you"), "metric, above you");
  CHECK(is(rowVs(377.1, 412.3, UNITS_METRIC), "Elev 377m, 35m below you"), "metric, below you");
  CHECK(is(rowVs(412.1, 17.9, UNITS_US), "Elev 1,352ft, 1,293ft above you"),
        "US: both numbers in feet, commas on both");
  CHECK(is(rowVs(412.0, 291.9, UNITS_US), "Elev 1,352ft, 394ft above you"), "the spec's US example");
  CHECK(is(rowVs(412.0, 412.0, UNITS_METRIC), "Elev 412m, level with you"), "the same ground: level");
  CHECK(is(rowVs(412.0, 411.01, UNITS_METRIC), "Elev 412m, level with you"), "0.99 m apart: level");
  CHECK(is(rowVs(412.0, 413.0, UNITS_METRIC), "Elev 412m, 1m below you"),
        "exactly 1 m apart is a difference, and never '0m below'");
  CHECK(is(rowVs(412.0, 411.0, UNITS_US), "Elev 1,352ft, 3ft above you"),
        "1 m in feet is 3ft: the level band never prints '0ft above'");
  CHECK(is(rowVs(412.0, 377.0, UNITS_METRIC, "Camp"), "Elev 412m, 35m above Camp"),
        "no position of your own: the reference, by name");
  CHECK(is(rowVs(412.0, 412.2, UNITS_METRIC, "Camp"), "Elev 412m, level with Camp"), "level with Camp");
  CHECK(is(rowVs(412.0, 377.0, UNITS_METRIC, ""), "Elev 412m, 35m above you"), "an empty name is you");
  CHECK(is(rowVs(412.0, 377.0, UNITS_METRIC, NULL, ELEV_Z_COARSE), "Elev 412m, 35m above you (coarse)"),
        "a coarse crosshair height: (coarse)");
  CHECK(is(rowVs(412.0, 377.0, UNITS_METRIC, NULL, ELEV_Z, ELEV_Z_COARSE),
           "Elev 412m, 35m above you (coarse)"),
        "a coarse 'you' height makes the difference coarse too: (coarse)");

  // ── the ruler ─────────────────────────────────────────────────────────────────────────
  CHECK(is(ruler(1200, "SE", true, 85.2, UNITS_METRIC), "Ruler: 1.2km SE, climb +85m"), "ruler, climbing");
  CHECK(is(ruler(1200, "SE", true, -40.4, UNITS_METRIC), "Ruler: 1.2km SE, climb -40m"), "ruler, descending");
  CHECK(is(ruler(1200, "SE", true, 0.3, UNITS_METRIC), "Ruler: 1.2km SE, climb 0m"), "ruler, flat: 0m, no sign");
  CHECK(is(ruler(1200, "SE", false, 0, UNITS_METRIC), "Ruler: 1.2km SE of the anchor"),
        "no heights: the ruler's old words, exactly");
  CHECK(is(ruler(1609.344 * 2.3, "NW", true, 120.0, UNITS_US), "Ruler: 2.3mi NW, climb +394ft"), "ruler in US");
  CHECK(is(ruler(250, "N", false, 0, UNITS_US), "Ruler: 820ft N of the anchor"), "ruler in US, no heights");
  // Review 2026-09-27: a climb with an end from the ~100 m z10 layer looked as exact as a z13 one.
  CHECK(is(ruler(1200, "SE", true, 85.2, UNITS_METRIC, true), "Ruler: 1.2km SE, climb* +85m"),
        "ruler: a coarse end marks the climb with the map's star");
  CHECK(is(ruler(1609.344 * 2.3, "NW", true, -120.0, UNITS_US, true), "Ruler: 2.3mi NW, climb* -394ft"),
        "ruler: coarse in US too");
  CHECK(is(ruler(1200, "SE", false, 0, UNITS_METRIC, true), "Ruler: 1.2km SE of the anchor"),
        "ruler: no climb, nothing to mark (coarse ignored)");

  // ── every glyph is in the font ────────────────────────────────────────────────────────
  {
    const char* samples[] = {
      "Elev 1,352ft, 394ft above you (coarse)", "Elev: no tile here", "Elev: no data here",
      "Elev: card read error", "Elev ...", "Elev -28m, level with you", "Ruler: 1.2km SE, climb -40m",
      "Ground 412m", "Ruler: 1.2km SE, climb* +85m",
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
      ok = allGlyphs(&g_font, samples[i]) && ok;
    }
    CHECK(ok, "every character these rows use has a glyph in Akrobat_Bold16 (no hollow boxes)");
  }

  // ── the widths, on the glass's own metrics ────────────────────────────────────────────
  {
    /* " (coarse)" is ~57 px of the strip's 232: the FULL wording overflows on ordinary ground
     * ("Elev 1800m, 1300m above you (coarse)" is 242 px). Measured, that is why the fitted row
     * falls back to the map's own star for coarse data - checked here, not assumed. */
    const int wFull = textWidth(&g_font, rowVs(1800.0, 500.0, UNITS_METRIC, NULL, ELEV_Z_COARSE));
    CHECK(wFull > STRIP_W, "the full coarse wording really does overflow the strip on ordinary ground");
  }
  {
    /* The widest rows real ground can make, FITTED as the strip fits them. Mount Rainier is
     * 4,392 m / 14,411 ft - the highest point anywhere near where these phones go - and a
     * difference cannot be bigger than the two heights; the Dead Sea's -430 m is the lowest
     * land. Every one fits 232 px WITHOUT the phone's ellipsis. */
    struct { double here, ref; int units, z, refZ; const char* want; } W[] = {
      { 4392.0, 0.0,    UNITS_US,     ELEV_Z_COARSE, ELEV_Z, "Elev* 14,409ft, 14,409ft above you" },
      { 4392.0, 4.0,    UNITS_METRIC, ELEV_Z_COARSE, ELEV_Z, "Elev* 4392m, 4388m above you" },
      { 0.0,    4392.0, UNITS_US,     ELEV_Z_COARSE, ELEV_Z, "Elev 0ft, 14,409ft below you (coarse)" },
      { -430.0, 4392.0, UNITS_US,     ELEV_Z, ELEV_Z_COARSE, "Elev* -1,411ft, 15,820ft below you" },
      { 4392.0, 4392.4, UNITS_US,     ELEV_Z_COARSE, ELEV_Z, "Elev 14,409ft, level with you (coarse)" },
      { 412.0,  291.9,  UNITS_US,     ELEV_Z, ELEV_Z,        "Elev 1,352ft, 394ft above you" },
      { 1800.0, 500.0,  UNITS_METRIC, ELEV_Z, ELEV_Z,        "Elev 1800m, 1300m above you" },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(W) / sizeof(W[0]); i++) {
      ElevTextIn in = point(1, W[i].here, W[i].z, W[i].units);
      in.haveRef = true;
      in.refM = W[i].ref;
      in.refZ = W[i].refZ;
      elevTextRow(g_row, sizeof(g_row), &in, textWidth, &g_font, STRIP_W);
      ok = is(g_row, W[i].want) && ok;
      ok = fits(g_row) && ok;
    }
    CHECK(ok, "fitted: the widest real rows fit 232 px - a coarse one as Elev*, a fine one unchanged");
    ok = true;
    ok = fits(row(point(ELEV_TXT_IOERR, 0, -1, UNITS_METRIC))) && ok;
    ok = fits(row(point(1, 4392.0, ELEV_Z_COARSE, UNITS_US))) && ok;              // "Elev 14,409ft (coarse)"
    ok = fits(ruler(1609.344 * 999, "NW", true, -4392.0, UNITS_US)) && ok;        // "999mi NW, climb -14,409ft"
    ok = fits(ruler(9999999.0, "NW", true, -4392.0, UNITS_METRIC)) && ok;
    ok = fits(ruler(1609.344 * 999, "NW", true, -4392.0, UNITS_US, true)) && ok;  // "climb* -14,409ft"
    ok = fits(ruler(9999999.0, "NW", true, -4392.0, UNITS_METRIC, true)) && ok;
    CHECK(ok, "the rows with no comparison, and the ruler at its widest (coarse too), fit in their full wording");
  }
  {
    /* A reference name is somebody else's text, up to 23 characters (resolveReference's buffer
     * in drawBottomStrip). Too wide even with the star, it is the NAME that gives - cut on a
     * character, with ".." - and the numbers stay. */
    ElevTextIn in = point(1, 4392.0, ELEV_Z_COARSE, UNITS_US);
    in.haveRef = true;
    in.refM = 0.0;
    in.refName = "Hunting Camp North Fork";                // 23 characters
    elevTextRow(g_row, sizeof(g_row), &in, textWidth, &g_font, STRIP_W);
    const size_t n = strlen(g_row);
    CHECK(textWidth(&g_font, g_row) <= STRIP_W, "a long reference name is shortened until the row fits");
    CHECK(strstr(g_row, "Elev* 14,409ft, 14,409ft above Hunt") == g_row && n > 2 &&
          strcmp(g_row + n - 2, "..") == 0,
          "...keeping the star, the numbers, the start of the name and '..'");
    printf("    shortened: \"%s\" (%d px)\n", g_row, textWidth(&g_font, g_row));

    in.refName = "Camp";
    elevTextRow(g_row, sizeof(g_row), &in, textWidth, &g_font, STRIP_W);
    CHECK(is(g_row, "Elev* 14,409ft, 14,409ft above Camp"), "a short name is left whole once the star fits it");

    /* Review 2026-09-27: "you" was also a POOR fix (under 4 satellites / HDOP 10 - measured 20 km
     * off) or an OLD one, and the row still said "above you". The map names them (elevYouUsable):
     * "poor fix" / "old fix" - whole, never cut, at the widest real heights in both units. */
    {
      const char* who[2] = { "poor fix", "old fix" };
      const double hs[4][2] = { { 4392.0, 0.0 }, { 0.0, 4392.0 }, { -430.0, 4392.0 }, { 4392.0, 4.0 } };
      bool whole = true;
      for (int wi = 0; wi < 2; wi++) {
        for (int hi = 0; hi < 4; hi++) {
          for (int un = 0; un < 2; un++) {
            ElevTextIn q = point(1, hs[hi][0], ELEV_Z_COARSE, un ? UNITS_US : UNITS_METRIC);
            q.haveRef = true;
            q.refM = hs[hi][1];
            q.refZ = ELEV_Z;
            q.refName = who[wi];
            elevTextRow(g_row, sizeof(g_row), &q, textWidth, &g_font, STRIP_W);
            const size_t L = strlen(g_row), N = strlen(who[wi]);
            if (textWidth(&g_font, g_row) > STRIP_W || L < N || strcmp(g_row + L - N, who[wi]) != 0) {
              printf("    \"%s\" (%d px)\n", g_row, textWidth(&g_font, g_row));
              whole = false;
            }
          }
        }
      }
      CHECK(whole, "'above poor fix' / 'below old fix': the name whole, the row inside the strip, "
                   "at the widest real heights (metric and US)");
    }
    in.z = ELEV_Z;
    elevTextRow(g_row, sizeof(g_row), &in, textWidth, &g_font, STRIP_W);
    CHECK(is(g_row, "Elev 14,409ft, 14,409ft above Camp"), "...and a fine height that fits is the plain wording");
    in.z = ELEV_Z_COARSE;

    in.refName = "Hunting Camp North Fork";
    elevTextRow(g_row, sizeof(g_row), &in, textWidth, &g_font, 20);
    CHECK(is(g_row, "Elev* 14,409ft, 14,409ft above Hunting Camp North Fork"),
          "nothing fits even at one letter: the starred row with the whole name, for the phone's ellipsis");

    in.refName = "Caf\xc3\xa9 du Lac Sup\xc3\xa9rieur Nord";   // UTF-8 inside the name
    bool clean = true;
    for (int w = 150; w <= 260; w++) {
      elevTextRow(g_row, sizeof(g_row), &in, textWidth, &g_font, w);
      for (const char* p = strstr(g_row, "above "); p && *p; p++) {
        if (((unsigned char)p[0] & 0xC0) == 0xC0 && ((unsigned char)p[1] & 0xC0) != 0x80) {
          clean = false;                                   // a lead byte cut from its continuation
        }
      }
    }
    CHECK(clean, "a shortened name is never cut inside a UTF-8 character (every width 150..260)");

    ElevTextIn you = point(1, 4392.0, ELEV_Z_COARSE, UNITS_US);
    you.haveRef = true;
    you.refM = 0.0;
    elevTextRow(g_row, sizeof(g_row), &you, textWidth, &g_font, 20);
    CHECK(is(g_row, "Elev* 14,409ft, 14,409ft above you"),
          "'you' is never shortened: no name, nothing to cut past the star");
  }

  // ── the console's words for one sample ───────────────────────────────────────────────
  {
    char b[48];
    elevSampleText(b, sizeof(b), 1, 412.34, ELEV_Z);
    CHECK(is(b, "412.3 m (z13)"), "console: a height, to the decimetre, with its layer");
    elevSampleText(b, sizeof(b), 1, -27.66, ELEV_Z_COARSE);
    CHECK(is(b, "-27.7 m (z10)"), "console: below sea level, from the coarse layer");
    elevSampleText(b, sizeof(b), 0, 0, -1);
    CHECK(is(b, "no tile"), "console: no tile");
    elevSampleText(b, sizeof(b), -1, 0, -1);
    CHECK(is(b, "no data (a hole in the tile)"), "console: no data");
    elevSampleText(b, sizeof(b), ELEV_TXT_IOERR, 0, -1);
    CHECK(is(b, "card read error"), "console: the card");
    elevSampleText(b, sizeof(b), ELEV_TXT_PENDING, 0, -1);
    CHECK(is(b, "not sampled yet"), "console: not yet");
  }

  // ── NULL and tiny buffers ─────────────────────────────────────────────────────────────
  CHECK(elevTextRow(NULL, 10, NULL) == 0, "NULL out is refused");
  {
    char tiny[6];
    ElevTextIn in = point(1, 412.3, ELEV_Z, UNITS_METRIC);
    const int n = elevTextRow(tiny, sizeof(tiny), &in);
    CHECK(n == 5 && is(tiny, "Elev "), "a short buffer is cut like snprintf, and the length says so");
    char z[1] = { 'x' };
    CHECK(elevTextRow(z, 0, &in) == 0 && z[0] == 'x', "cap 0 writes nothing");
    CHECK(elevTextRow(g_row, sizeof(g_row), NULL) == 0 && g_row[0] == '\0', "NULL input is an empty row");
    CHECK(elevRulerText(NULL, 10, 1, "N", false, 0, 0, false) == 0, "the ruler refuses NULL out");
  }

  if (failures) {
    printf("test_elevtext: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("test_elevtext: %d passed, 0 failed\n", checks);
  return 0;
}
