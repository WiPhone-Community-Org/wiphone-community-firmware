/* elev_text.cpp - see elev_text.h. Proven by tests/test_elevtext.cpp. */

#include "elev_text.h"
#include "elev_tiles.h"      // ELEV_Z_COARSE
#include "units.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* One row from its parts. `name` is the reference's name as it should be printed (possibly
 * shortened), NULL for "you". `star`: a coarse height is marked "Elev*" instead of " (coarse)". */
static int compose(char* out, size_t cap, const ElevTextIn* in, const char* name, bool star) {
  const char* label = (in->label && in->label[0]) ? in->label : "Elev";
  int n = 0;
  switch (in->rc) {
  case 1:
    break;
  case ELEV_TXT_PENDING:
    n = snprintf(out, cap, "%s ...", label);
    return n < 0 ? 0 : (int)strlen(out);
  case ELEV_TXT_IOERR:
    n = snprintf(out, cap, "%s: card read error", label);
    return n < 0 ? 0 : (int)strlen(out);
  case -1:
    n = snprintf(out, cap, "%s: no data here", label);
    return n < 0 ? 0 : (int)strlen(out);
  default:
    n = snprintf(out, cap, "%s: no tile here", label);
    return n < 0 ? 0 : (int)strlen(out);
  }
  /* Buffers sized to what they hold, not rounded up: this runs inside the map's draw, on the
   * loop task, whose stack is the phone's scarcest thing. "-14,409ft" is 9 characters (the
   * formatter caps a height at 1e12 of the unit, "1,000,000,000,000ft" = 19), a name 23. */
  char alt[24];
  unitsFmtAlt(in->m, in->units, false, alt, sizeof(alt));
  bool coarse = (in->z == ELEV_Z_COARSE);
  char cmp[56];
  cmp[0] = '\0';
  if (in->haveRef) {
    const char* who = (name && name[0]) ? name : "you";
    const double d = in->m - in->refM;
    if (fabs(d) < 1.0) {
      snprintf(cmp, sizeof(cmp), ", level with %s", who);
    } else {
      char dd[24];
      unitsFmtAlt(fabs(d), in->units, false, dd, sizeof(dd));
      snprintf(cmp, sizeof(cmp), ", %s %s %s", dd, d > 0 ? "above" : "below", who);
    }
    if (in->refZ == ELEV_Z_COARSE) {
      coarse = true;
    }
  }
  if (coarse && star) {
    n = snprintf(out, cap, "%s* %s%s", label, alt, cmp);
  } else {
    n = snprintf(out, cap, "%s %s%s%s", label, alt, cmp, coarse ? " (coarse)" : "");
  }
  return n < 0 ? 0 : (int)strlen(out);
}

int elevTextRow(char* out, size_t cap, const ElevTextIn* in, ElevTextMeasureFn measure,
                void* mctx, int maxW) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (!in) {
    return 0;
  }
  const bool named = in->rc == 1 && in->haveRef && in->refName && in->refName[0];
  const char* whole = named ? in->refName : NULL;
  int len = compose(out, cap, in, whole, false);
  if (!measure || maxW <= 0 || measure(mctx, out) <= maxW) {
    return len;
  }
  // 1. " (coarse)" -> "Elev*" (compose ignores the star when nothing is coarse).
  len = compose(out, cap, in, whole, true);
  if (!named || measure(mctx, out) <= maxW) {
    return len;                            // fits, or nothing else can give: the ellipsis's
  }
  /* 2. The name: "Hunting Camp North" -> "Hunting C.." -> ... -> "H..". Cut on a character
   * boundary (a UTF-8 continuation byte is 10xxxxxx), never inside one. */
  char name[32];
  size_t k = strlen(in->refName);
  while (k > 1) {
    k--;                                   // keep the first k bytes...
    while (k > 0 && ((unsigned char)in->refName[k] & 0xC0) == 0x80) {
      k--;                                 // ...ending on a character boundary
    }
    if (k == 0) {
      break;
    }
    if (k + 3 > sizeof(name)) {
      continue;                            // longer than the buffer: keep cutting
    }
    memcpy(name, in->refName, k);
    name[k] = '.';
    name[k + 1] = '.';
    name[k + 2] = '\0';
    len = compose(out, cap, in, name, true);
    if (measure(mctx, out) <= maxW) {
      return len;
    }
  }
  // 3. Nothing fits: the whole name, for the phone to ellipsize from the end.
  return compose(out, cap, in, whole, true);
}

int elevRulerText(char* out, size_t cap, double distM, const char* compass, bool haveClimb,
                  double climbM, int units, bool coarse) {
  if (!out || cap == 0) {
    return 0;
  }
  char d[16];
  unitsFmtDist(distM, units, d, sizeof(d));
  const char* c = compass ? compass : "";
  int n;
  if (haveClimb) {
    char up[24];
    unitsFmtAlt(climbM, units, true, up, sizeof(up));
    n = snprintf(out, cap, "Ruler: %s %s, climb%s %s", d, c, coarse ? "*" : "", up);
  } else {
    n = snprintf(out, cap, "Ruler: %s %s of the anchor", d, c);
  }
  return n < 0 ? 0 : (int)strlen(out);
}

int elevSampleText(char* out, size_t cap, int rc, double m, int z) {
  if (!out || cap == 0) {
    return 0;
  }
  int n;
  switch (rc) {
  case 1:                n = snprintf(out, cap, "%.1f m (z%d)", m, z); break;
  case -1:               n = snprintf(out, cap, "no data (a hole in the tile)"); break;
  case ELEV_TXT_IOERR:   n = snprintf(out, cap, "card read error"); break;
  case ELEV_TXT_PENDING: n = snprintf(out, cap, "not sampled yet"); break;
  default:               n = snprintf(out, cap, "no tile"); break;
  }
  return n < 0 ? 0 : (int)strlen(out);
}
