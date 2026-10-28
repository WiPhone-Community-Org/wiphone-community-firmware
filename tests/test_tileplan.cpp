/*
 * test_tileplan.cpp — the map download's arithmetic and its resume decision (tile_plan.cpp).
 *
 * The tile counts are the ones the reader measured on 2026-09-23 by compiling the old
 * tile_fetch.cpp tileRange() against map_tiles.cpp: moving the code into tile_plan.cpp must not
 * move them by one tile. The fetch order is checked STRUCTURALLY (every tile of the square
 * exactly once, the centre first, rings never going back inward), because a golden list states
 * none of the properties that matter. The resume decision is walked as a table: one world, one
 * closed gate at a time.
 */
#include "../WiPhone/tile_plan.h"
#include "../WiPhone/map_tiles.h"
#include "../WiPhone/units.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void ok(bool c, const char* what) {
  if (c) {
    g_pass++;
  } else {
    g_fail++;
    printf("  FAIL  %s\n", what);
  }
}
static void group(const char* name) {
  printf("%s\n", name);
}

/* Walk one square through the order and check it. `n` is 2^z for the x wrap. */
static void orderCheck(int x0, int x1, int y0, int y1, int cx, int cy, int n, const char* label) {
  TilePlanOrder o;
  tilePlanOrderInit(&o, x0, x1, y0, y1, cx, cy);
  const int w = x1 - x0 + 1, h = y1 - y0 + 1;
  std::vector<int> seen((size_t)w * (size_t)h, 0);
  bool inside = true, ringsOut = true, colMajor = true, firstCentre = true;
  int count = 0, lastRing = -1, px = 0, py = 0;
  // The centre the order promises to start from: the given one, clamped into the square.
  const int cu = cx - x0 < 0 ? 0 : (cx - x0 > w - 1 ? w - 1 : cx - x0);
  const int cv = cy - y0 < 0 ? 0 : (cy - y0 > h - 1 ? h - 1 : cy - y0);
  const int cbu = cu / TILE_PLAN_BLOCK, cbv = cv / TILE_PLAN_BLOCK;
  int x, y;
  while (tilePlanOrderNext(&o, &x, &y)) {
    const int u = x - x0, v = y - y0;
    if (u < 0 || u >= w || v < 0 || v >= h) {
      inside = false;
      break;
    }
    seen[(size_t)v * w + u]++;
    const int bu = u / TILE_PLAN_BLOCK, bv = v / TILE_PLAN_BLOCK;
    const int ring = abs(bu - cbu) > abs(bv - cbv) ? abs(bu - cbu) : abs(bv - cbv);
    if (count == 0 && ring != 0) firstCentre = false;
    if (ring < lastRing) ringsOut = false;
    lastRing = ring;
    if (count > 0) {
      const int pbu = (px - x0) / TILE_PLAN_BLOCK, pbv = (py - y0) / TILE_PLAN_BLOCK;
      if (pbu == bu && pbv == bv) {
        // Same block: either down the same column, or the top of the next column.
        const bool down = (x == px && y == py + 1);
        const bool nextCol = (x == px + 1 && v == bv * TILE_PLAN_BLOCK);
        if (!down && !nextCol) colMajor = false;
      }
    }
    px = x;
    py = y;
    count++;
    if (count > w * h + 5) break;          // a runaway walk
  }
  bool once = true;
  for (size_t i = 0; i < seen.size(); i++) {
    if (seen[i] != 1) once = false;
  }
  // The wrapped x values the fetcher would use: each (x mod n, y) once too.
  std::vector<int> wrapped((size_t)n * (size_t)h, 0);
  bool wrapOnce = true;
  {
    TilePlanOrder o2;
    tilePlanOrderInit(&o2, x0, x1, y0, y1, cx, cy);
    while (tilePlanOrderNext(&o2, &x, &y)) {
      const int wx = ((x % n) + n) % n;
      if (++wrapped[(size_t)(y - y0) * n + wx] != 1) wrapOnce = false;
    }
  }
  char what[160];
  snprintf(what, sizeof(what), "%s: every tile of the %dx%d square exactly once (%d handed out)", label, w, h, count);
  ok(inside && once && count == w * h, what);
  snprintf(what, sizeof(what), "%s: ord counts what was handed out", label);
  ok(o.ord == (int64_t)w * h, what);
  snprintf(what, sizeof(what), "%s: the first tile is in the centre block", label);
  ok(firstCentre, what);
  snprintf(what, sizeof(what), "%s: rings only ever go outward", label);
  ok(ringsOut, what);
  snprintf(what, sizeof(what), "%s: column by column inside a block", label);
  ok(colMajor, what);
  snprintf(what, sizeof(what), "%s: wrapped x, still each tile once", label);
  ok(wrapOnce, what);
}

static TileWorld allClear(uint32_t now) {
  TileWorld w;
  memset(&w, 0, sizeof(w));
  w.now = now;
  w.wifiUp = true;
  w.wifiUpMs = 120000;
  w.sameNet = true;
  w.usb = true;
  w.volts = 4.1f;
  w.call = false;
  w.sinceCallMs = 0xFFFFFFFFu;
  w.game = false;
  w.filesJob = false;
  w.card = true;
  return w;
}

int main() {
  group("tile counts, as the old tileRange() gave them (47.5, -121.8)");
  {
    ok(tilePlanTiles(47.5, -121.8, 20, 17) == 51084, "20 km to z17 = 51,084 tiles");
    ok(tilePlanTiles(47.5, -121.8, 10, 17) == 12917, "10 km to z17 = 12,917 tiles");
    ok(tilePlanTiles(47.5, -121.8, 5, 17) == 3394, "5 km to z17 = 3,394 tiles");
    ok(tilePlanTiles(47.5, -121.8, 2, 17) == 566, "2 km to z17 = 566 tiles");
    ok(tilePlanLevelTiles(47.5, -121.8, 20, 17) == 38220, "20 km, z17 alone = 38,220 tiles");
    ok(tilePlanTiles(47.5, -121.8, 20, 16) == 12864, "20 km to z16 = 12,864 tiles");
    ok(tilePlanTiles(47.5, -121.8, 20, 10) == 0, "a depth under the base level is no tiles");
    int x0, x1, y0, y1;
    tilePlanRange(0.0, 179.99, 20, 11, &x0, &x1, &y0, &y1);
    ok(x1 > (1 << 11) - 1 && x0 <= (1 << 11) - 1, "a square over the antimeridian runs x past 2^z - 1");
    tilePlanRange(84.9, 10.0, 50, 11, &x0, &x1, &y0, &y1);
    ok(y0 == 0 && y1 >= y0, "a square at the top of the world clamps y at 0");
  }

  group("bytes are 64-bit: 20 km to z17 is past 4 GiB");
  {
    const uint64_t card = tilePlanCardBytes(51084);
    ok(card == 6695682048ull, "51,084 tiles = 6,695,682,048 bytes on the card, not 4 GiB - 1");
    ok(tilePlanNetBytes(150000, 35) == 5376000000ull, "150,000 OTM tiles = 5,376,000,000 network bytes, no wrap");
    ok(tilePlanCardBytes(-1) == 0 && tilePlanNetBytes(10, 0) == 0, "nonsense in, zero out");
    char b[24];
    tilePlanBytes(card, b, sizeof(b));
    ok(!strcmp(b, "6.2 GB"), "6,695,682,048 bytes reads \"6.2 GB\"");
    tilePlanBytes(tilePlanCardBytes(566), b, sizeof(b));
    ok(!strcmp(b, "70 MB"), "566 tiles reads \"70 MB\"");
  }

  /* "Elevation too" (0.9.80): the same square at z13 and z10, 131,072 bytes a tile like a map
   * tile. The 20 km figure is what the form's estimate adds to a 20 km job. */
  group("the elevation layer a job adds: the square at z13 + z10");
  {
    ok(tilePlanElevTiles(47.5, -121.8, 2) == 6 + 2, "2 km: 6 z13 + 2 z10 tiles");
    ok(tilePlanElevTiles(47.5, -121.8, 5) == 16 + 2, "5 km: 16 z13 + 2 z10 tiles");
    ok(tilePlanElevTiles(47.5, -121.8, 20) == 169 + 6, "20 km: 169 z13 + 6 z10 tiles");
    ok(tilePlanElevTiles(47.5, -121.8, 20) == tilePlanLevelTiles(47.5, -121.8, 20, 13) +
                                              tilePlanLevelTiles(47.5, -121.8, 20, 10),
       "...exactly the job's own square at those two levels (tilePlanLevelTiles)");
    ok(tilePlanElevTiles(47.5, -121.8, 0) == 0 && tilePlanElevTiles(47.5, -121.8, -3) == 0, "no radius, no tiles");
    ok(tilePlanCardBytes(175) == 175ull * 131072ull, "175 elevation tiles are 175 x 131,072 bytes on the card");
    ok(tilePlanNetBytes(175, TILE_PLAN_ELEV_KB) == 175ull * 120ull * 1024ull, "...and ~120 KB each off the network");
    // The run's space check (review 2026-09-27): all of it, but not again once a run had it whole.
    ok(tilePlanElevSpaceTiles(47.5, -121.8, 20, true, false, false) == 175, "space: a fresh job asks for the whole layer");
    ok(tilePlanElevSpaceTiles(47.5, -121.8, 20, true, false, true) == 175,
       "space: a fresh START asks for it all even over a whole layer (a new job, a new record)");
    ok(tilePlanElevSpaceTiles(47.5, -121.8, 20, true, true, false) == 175,
       "space: a resume whose layer is not known whole asks for all of it (the honest direction)");
    ok(tilePlanElevSpaceTiles(47.5, -121.8, 20, true, true, true) == 0,
       "space: a resume over a layer a run already had whole asks for none of it");
    ok(tilePlanElevSpaceTiles(47.5, -121.8, 20, false, false, false) == 0, "space: no layer, nothing");
  }

  /* `maps dl <src> <lat> <lon> <km> <zmax> [elev] [1|0]` - review 2026-09-27: the help showed
   * "[elev 1|0]", the parser took only a bare digit, and "... 15 elev 0" fell back to the form's
   * ON with no word said. */
  group("maps dl: the elevation argument");
  {
    ok(tilePlanParseElevArg("") == -1 && tilePlanParseElevArg("   ") == -1 && tilePlanParseElevArg(NULL) == -1,
       "absent: the form's setting");
    ok(tilePlanParseElevArg(" 0") == 0 && tilePlanParseElevArg(" 1") == 1, "a bare 0 / 1 (docs/maps.md's form)");
    ok(tilePlanParseElevArg(" elev 0") == 0 && tilePlanParseElevArg(" elev 1 ") == 1 &&
       tilePlanParseElevArg(" ELEV  0") == 0, "the help's form, 'elev 0' / 'elev 1', any case");
    ok(tilePlanParseElevArg(" elev") == -2 && tilePlanParseElevArg(" elev 2") == -2 &&
       tilePlanParseElevArg(" 01") == -2 && tilePlanParseElevArg(" 0 x") == -2 &&
       tilePlanParseElevArg(" off") == -2 && tilePlanParseElevArg(" elev0") == -2 &&
       tilePlanParseElevArg(" 1.5") == -2, "anything else is refused, never read as absent");
  }

  group("the Detail row: only the depths the source has, and the one shown is the one used");
  {
    int d = 13, seq[6];
    for (int i = 0; i < 5; i++) {
      seq[i] = d;
      d = tilePlanDepthNext(d, 16);
    }
    seq[5] = d;
    ok(seq[0] == 13 && seq[1] == 14 && seq[2] == 15 && seq[3] == 16 && seq[4] == 13,
       "USGS (z16): 13 -> 14 -> 15 -> 16 -> 13, no phantom fifth step");
    ok(tilePlanDepthNext(16, 17) == 17 && tilePlanDepthNext(17, 17) == 13, "OpenTopoMap (z17): 16 -> 17 -> 13");
    ok(tilePlanDepthShown(17, 16) == 16, "a wish of 17 on USGS SHOWS 16");
    ok(tilePlanDepthNext(17, 16) == 13, "...and one press from there is 13, not a dead press");
    ok(tilePlanDepthShown(17, 17) == 17, "the same wish on OpenTopoMap shows 17");
    ok(tilePlanDepthTop(19) == 17 && tilePlanDepthShown(19, 19) == 17, "the custom relay (z19) is offered to z17, no further");
    ok(tilePlanDepthShown(9, 17) == MAPS_DL_DEPTH_MIN, "a wish under the form's floor shows the floor");

    /* The row itself (review 2026-09-27: "z15 (3 m/pixel)" was a literal the US setting never
     * reached): z15's pixel at the area's latitude, through the units formatter. */
    char row[48];
    tilePlanDetailText(row, sizeof(row), 15, 47.5, UNITS_METRIC);
    ok(!strcmp(row, "Detail: z15 (3m/pixel)"), "Detail z15 at 47.5 N, metric: 3m/pixel (3.23 m)");
    tilePlanDetailText(row, sizeof(row), 15, 47.5, UNITS_US);
    ok(!strcmp(row, "Detail: z15 (11ft/pixel)"), "...US: 11ft/pixel (10.6 ft) - the setting reaches it");
    tilePlanDetailText(row, sizeof(row), 15, 0.0, UNITS_METRIC);
    ok(!strcmp(row, "Detail: z15 (5m/pixel)"), "...the equator: 5m/pixel (4.78 m) - the latitude counts");
    tilePlanDetailText(row, sizeof(row), 16, 47.5, UNITS_US);
    ok(!strcmp(row, "Detail: z16 (4x z15's cost)"), "Detail z16: the cost");
    tilePlanDetailText(row, sizeof(row), 17, 47.5, UNITS_METRIC);
    ok(!strcmp(row, "Detail: z17 (4x z16's cost)"), "Detail z17: the cost");
    tilePlanDetailText(row, sizeof(row), 13, 47.5, UNITS_METRIC);
    ok(!strcmp(row, "Detail: z13 (coarser)"), "Detail z13: coarser");
    tilePlanDetailText(row, 12, 15, 47.5, UNITS_US);
    ok(row[0] == '\0', "a short buffer writes \"\", never a cut number");
  }

  group("maps dlurl <template> [area] (0.9.82: a relay into a real map)");
  {
    char t[200], a[32];
    ok(tilePlanParseDlurl("http://192.168.1.17:8765/otm/{z}/{x}/{y}.png otm", t, sizeof(t), a, sizeof(a)) == 1 &&
           !strcmp(t, "http://192.168.1.17:8765/otm/{z}/{x}/{y}.png") && !strcmp(a, "otm"),
       "template + area: the relay fills /maps/otm");
    ok(tilePlanParseDlurl("  http://h/{z}/{x}/{y}.png  ", t, sizeof(t), a, sizeof(a)) == 1 &&
           !strcmp(t, "http://h/{z}/{x}/{y}.png") && !strcmp(a, "custom"),
       "no area: \"custom\", as before 0.9.82 (spaces around ignored)");
    ok(tilePlanParseDlurl("http://h/{z}/{x}/{y}.png\tusgs-topo", t, sizeof(t), a, sizeof(a)) == 1 && !strcmp(a, "usgs-topo"),
       "a tab separates too; usgs-topo is a map folder");
    ok(tilePlanParseDlurl("http://h/{z}/{x}/{y}.png elev", t, sizeof(t), a, sizeof(a)) == 0 &&
           tilePlanParseDlurl("http://h/{z}/{x}/{y}.png ELEV", t, sizeof(t), a, sizeof(a)) == 0 &&
           tilePlanParseDlurl("http://h/{z}/{x}/{y}.png Elev", t, sizeof(t), a, sizeof(a)) == 0,
       "elev in any case is refused: map tiles would land in the altitude layer's folder (FAT is case-blind)");
    ok(tilePlanParseDlurl("http://h/{z}/{x}/{y}.png elevation", t, sizeof(t), a, sizeof(a)) == 1 && !strcmp(a, "elevation"),
       "...but only the reserved name itself");
    ok(tilePlanParseDlurl("http://h/{z}/{x}/{y}.png ../x", t, sizeof(t), a, sizeof(a)) == 0 &&
           tilePlanParseDlurl("http://h/{z}/{x}/{y}.png .hidden", t, sizeof(t), a, sizeof(a)) == 0 &&
           tilePlanParseDlurl("http://h/{z}/{x}/{y}.png a/b", t, sizeof(t), a, sizeof(a)) == 0,
       "no path tricks: '/' and a leading '.' are not area names (mapAreaNameOk)");
    ok(tilePlanParseDlurl("http://h/{z}/{x}/{y}.png otm extra", t, sizeof(t), a, sizeof(a)) == 0,
       "a third word is a mistake to be told about, never ignored");
    ok(tilePlanParseDlurl("", t, sizeof(t), a, sizeof(a)) == 0 && tilePlanParseDlurl("   ", t, sizeof(t), a, sizeof(a)) == 0 &&
           tilePlanParseDlurl(NULL, t, sizeof(t), a, sizeof(a)) == 0,
       "nothing: refused (clear is the console's own word, before this)");
    ok(tilePlanParseDlurl("http://h/{z}/{x}/{y}.png abcdefghijabcdefghijabcdefghij12", t, sizeof(t), a, sizeof(a)) == 0 &&
           a[0] == '\0',
       "a 32-character area does not fit 31 + NUL: refused, nothing half-copied");
    ok(tilePlanParseDlurl("http://h/{z}/{x}/{y}.png abcdefghijabcdefghijabcdefghij1", t, sizeof(t), a, sizeof(a)) == 1,
       "31 characters: the longest area name the map lists");
    char small[10];
    ok(tilePlanParseDlurl("http://h/{z}/{x}/{y}.png otm", small, sizeof(small), a, sizeof(a)) == 0 && small[0] == '\0',
       "a template too long for the buffer: refused, never cut");
    ok(tilePlanParseDlurl("http://h/{z}/{x}/{y}.png elev.", t, sizeof(t), a, sizeof(a)) == 0 &&
           tilePlanParseDlurl("http://h/{z}/{x}/{y}.png ELEV..", t, sizeof(t), a, sizeof(a)) == 0 &&
           tilePlanParseDlurl("http://h/{z}/{x}/{y}.png otm.", t, sizeof(t), a, sizeof(a)) == 0,
       "a trailing '.' is refused: FatFs drops it, so 'elev.' IS /maps/elev and 'otm.' is otm misspelt (review)");
    ok(tilePlanAreaWritable("otm") && tilePlanAreaWritable("usgs-topo") && tilePlanAreaWritable("v1.2") &&
           !tilePlanAreaWritable("Elev") && !tilePlanAreaWritable("x.") && !tilePlanAreaWritable("") &&
           !tilePlanAreaWritable(NULL),
       "tilePlanAreaWritable: a dot inside is fine, at the end it is not; never elev; never empty");
    ok(tilePlanMinIntervalMs("custom", 17) == 0,
       "the relay is source \"custom\": OpenTopoMap's z17 interval is for its servers, not the LAN");
  }

  group("the throttle and the time");
  {
    ok(tilePlanMinIntervalMs("otm", 17) == 2000, "OpenTopoMap z17: 2.0 s between request starts");
    ok(tilePlanMinIntervalMs("otm", 16) == 0, "OpenTopoMap z16: no interval");
    ok(tilePlanMinIntervalMs("usgs-topo", 17) == 0 && tilePlanMinIntervalMs(NULL, 17) == 0, "only OpenTopoMap");
    /* Where the interval applies it IS the pace (phone 2 measured 2,067 ms a z17 tile against
     * the 2,000 ms interval, 2026-09-23): interval + 0.1 s there, the source's own rate elsewhere. */
    const double s = tilePlanSeconds(47.5, -121.8, 20, 17, 4.0f, "otm");
    ok(fabs(s - ((51084 - 38220) * 4.0 + 38220 * 2.1)) < 1e-3, "20 km z17 on OTM: 4.0 s for z11-16, 2.1 s for z17");
    const double s2 = tilePlanSeconds(47.5, -121.8, 20, 17, 1.2f, "otm");
    ok(fabs(s2 - ((51084 - 38220) * (double)1.2f + 38220 * 2.1)) < 1e-3, "a faster rate is unchanged below z17");
    const double s3 = tilePlanSeconds(47.5, -121.8, 20, 16, 1.2f, "usgs-topo");
    ok(fabs(s3 - 12864 * (double)1.2f) < 1e-3, "no interval, no change: USGS to z16");
    char t[24];
    tilePlanDuration(s, t, sizeof(t));
    ok(!strcmp(t, "37 h"), "36.6 h reads \"37 h\"");
    tilePlanDuration(51084 * 4.0, t, sizeof(t));
    ok(!strcmp(t, "2.4 days"), "56.8 h reads \"2.4 days\"");
    tilePlanDuration(12917 * 4.0, t, sizeof(t));
    ok(!strcmp(t, "14 h"), "14.4 h reads \"14 h\"");
    tilePlanDuration(38 * 60.0, t, sizeof(t));
    ok(!strcmp(t, "38 min"), "38 min reads \"38 min\"");
    tilePlanDuration(89 * 60.0, t, sizeof(t));
    ok(!strcmp(t, "89 min"), "89 min is still minutes");
    tilePlanDuration(90 * 60.0, t, sizeof(t));
    ok(!strcmp(t, "2 h"), "90 min is hours");
    tilePlanDuration(0, t, sizeof(t));
    ok(!strcmp(t, "1 min"), "nothing reads \"1 min\", never \"0 min\"");
    tilePlanDuration(49 * 3600.0, t, sizeof(t));
    ok(!strcmp(t, "2.0 days"), "49 h is days");
  }

  group("the live rate: the mean of the last 200 fetched tiles");
  {
    TilePlanRate r;
    tilePlanRateReset(&r);
    ok(tilePlanRateMeanMs(&r) == 0, "nothing measured is 0");
    for (int i = 0; i < 50; i++) tilePlanRateAdd(&r, 10000);   // a slow start...
    for (int i = 0; i < 200; i++) tilePlanRateAdd(&r, 4000);   // ...scrolled out by 200 at 4 s
    ok(tilePlanRateMeanMs(&r) == 4000, "only the last 200 count");
    tilePlanRateAdd(&r, 4200);
    ok(tilePlanRateMeanMs(&r) == 4001, "one more slides the window by one");
  }

  group("the order: centre-out 16x16 blocks, column by column");
  {
    orderCheck(100, 136, 200, 222, 118, 211, 1 << 11, "37x23, centre inside");
    orderCheck(0, 15, 0, 15, 3, 3, 1 << 11, "one block");
    orderCheck(5, 5, 7, 7, 5, 7, 1 << 11, "one tile");
    orderCheck(10, 70, 40, 44, 69, 41, 1 << 11, "a wide strip, centre at the east edge");
    const int n = 1 << 8;
    orderCheck(n - 3, n + 2, 30, 40, n + 1, 35, n, "over the antimeridian: n-3 .. n+2");
    orderCheck(40, 90, 0, 20, 60, 0, 1 << 11, "y clamped at the world's top, centre on row 0");
    orderCheck(0, 49, 0, 49, 200, -5, 1 << 11, "a centre outside the square is clamped into it");
    // The real thing: 20 km to z17, through the level helper.
    TilePlanOrder o;
    tilePlanLevelOrder(&o, 47.5, -121.8, 20, 17);
    int64_t cnt = 0;
    int x, y, fx = -1, fy = -1;
    while (tilePlanOrderNext(&o, &x, &y)) {
      if (cnt == 0) {
        fx = x;
        fy = y;
      }
      cnt++;
    }
    ok(cnt == 38220, "20 km z17: the level order hands out all 38,220 tiles");
    double wx, wy;
    mapLatLonToWorld(47.5, -121.8, 17, &wx, &wy);
    const int ctx = (int)floor(wx / 256), cty = (int)floor(wy / 256);
    ok(abs(fx - ctx) < TILE_PLAN_BLOCK && abs(fy - cty) < TILE_PLAN_BLOCK,
       "...and the first of them is within a block of the centre tile");
    // Seeking by ordinal (what a resume does) lands on the same tile a straight walk does.
    TilePlanOrder a, b;
    tilePlanLevelOrder(&a, 47.5, -121.8, 5, 16);
    tilePlanLevelOrder(&b, 47.5, -121.8, 5, 16);
    int ax = 0, ay = 0, bx = 0, by = 0;
    for (int i = 0; i < 300; i++) tilePlanOrderNext(&a, &ax, &ay);
    for (int i = 0; i < 299; i++) tilePlanOrderNext(&b, &bx, &by);
    tilePlanOrderNext(&b, &bx, &by);
    ok(ax == bx && ay == by && a.ord == 300 && b.ord == 300, "the 300th tile is the 300th tile");
  }

  group("resume: one closed gate at a time");
  {
    TileResume r;
    uint32_t wait = 0;
    const uint32_t T = 1000000;
    tileResumeFresh(&r, T);
    TileWorld w = allClear(T);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_IDLE, "a running or fresh job is not pending");

    tileResumeStopped(&r, TILE_STOP_NOWIFI, T);
    ok(r.pending && r.cools == 0, "a WiFi stop is pending, no cool-down");
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GO, "...and goes once everything is clear");
    w.wifiUp = false;
    w.wifiUpMs = 0;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_WIFI, "no WiFi: wait for it");
    w.wifiUp = true;
    w.wifiUpMs = 30000;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_WIFI && wait == 30000, "WiFi up 30 s: 30 s more");
    w.wifiUpMs = 60000;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GO, "WiFi up 60 s: go");
    w.sameNet = false;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_NET, "another network: wait for the one it started on");
    w = allClear(T);
    w.usb = false;
    w.volts = 3.79f;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_POWER, "off USB at 3.79 V: no");
    w.volts = 3.80f;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GO, "off USB at 3.80 V: yes");
    w.volts = 0.0f;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_POWER, "off USB with the gauge unread: no");
    w = allClear(T);
    w.volts = 0.0f;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GO, "on USB the battery does not matter");
    w = allClear(T);
    w.call = true;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_CALL, "a call: no");
    w.call = false;
    w.sinceCallMs = 30000;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_CALL && wait == 30000, "a call ended 30 s ago: 30 s more");
    w.sinceCallMs = 61000;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GO, "a call ended 61 s ago: go");
    w = allClear(T);
    w.game = true;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GAME, "the Game Boy: no");
    w = allClear(T);
    w.filesJob = true;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_FILES, "a Files folder job: no");
    w = allClear(T);
    w.card = false;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_CARD, "no card: no");

    // Failures cool down 10, 20, 40, then every 60 minutes.
    const uint32_t MIN = 60000;
    tileResumeFresh(&r, T);
    tileResumeStopped(&r, TILE_STOP_NETFAILS, T);
    w = allClear(T + 5 * MIN);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_COOL && wait == 5 * MIN, "network gave up: 10 min cool-down (5 to go)");
    w.wifiUp = false;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_COOL, "the cool-down is what the form names, even with WiFi down");
    w = allClear(T + 10 * MIN);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GO, "...10 min on: go");
    tileResumeStarting(&r, true);
    tileResumeStopped(&r, TILE_STOP_NETFAILS, T + 13 * MIN);
    w = allClear(T + 32 * MIN);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_COOL && wait == MIN, "the second in a row: 20 min");
    tileResumeStopped(&r, TILE_STOP_DECODEFAILS, T + 40 * MIN);
    w = allClear(T + 79 * MIN);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_COOL && wait == MIN, "the third: 40 min");
    tileResumeStopped(&r, TILE_STOP_RAM, T + 80 * MIN);
    w = allClear(T + 139 * MIN);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_COOL && wait == MIN, "the fourth: 60 min");
    tileResumeStopped(&r, TILE_STOP_NETFAILS, T + 140 * MIN);
    w = allClear(T + 199 * MIN);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_COOL && wait == MIN, "the fifth: still 60 min");
    tileResumeProgress(&r, 1, T + 200 * MIN);
    ok(r.cools == 0, "a new tile resets the cool-downs");
    tileResumeStopped(&r, TILE_STOP_NETFAILS, T + 201 * MIN);
    w = allClear(T + 202 * MIN);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_COOL && wait == 9 * MIN, "...so the next failure is 10 min again");

    // A day of RETRYING without one new tile (26 tries: 10+20+40 then 23 x 60 min): give up.
    tileResumeFresh(&r, T);
    for (int i = 0; i < TILE_PLAN_STALL_TRIES - 1; i++) {
      tileResumeStopped(&r, TILE_STOP_NETFAILS, T);
    }
    w = allClear(T + 2 * MIN);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_COOL, "25 tries: still cooling down, still trying");
    tileResumeStopped(&r, TILE_STOP_NETFAILS, T);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GAVE_UP && r.giveUp == TILE_GIVEUP_STALLED, "26 tries without a tile: gave up");
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GAVE_UP, "...and stays given up");
    tileResumeStarting(&r, false);
    ok(r.giveUp == TILE_GIVEUP_NONE && !r.pending, "Resume on the form is a fresh chance");

    // A WiFi wait is not retrying: a job pending for days on another network never gives up.
    tileResumeFresh(&r, T);
    tileResumeStopped(&r, TILE_STOP_NOWIFI, T);
    w = allClear(T + 3 * 24 * 60 * MIN);
    w.sameNet = false;
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_NET, "three days on the wrong network: still waiting, not given up");
    // ...and its FIRST failure after that wait cools down; it does not give up (review, 2026-09-23:
    // the old clock since the last tile counted the wait as retrying).
    tileResumeStopped(&r, TILE_STOP_NETFAILS, T + 3 * 24 * 60 * MIN);
    w = allClear(T + 3 * 24 * 60 * MIN + MIN);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_COOL && wait == 9 * MIN, "one failure after days at a gate: 10 min, not given up");
    // A clean power-off, the real path: tileFetchPowerOff writes reason POWEROFF and strikes 0,
    // the next boot loads that record and resumes it (a boot resume, so it counts a strike) — and
    // the NEXT power-off writes strikes 0 again. Four short power cycles in a row never give up.
    for (int cycle = 0; cycle < 4; cycle++) {
      tileResumeLoaded(&r, 0 /* what tileFetchPowerOff writes */, 0, TILE_STOP_POWEROFF, T);
      w = allClear(T + 2 * MIN);
      ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GO, "a switched-off job waits to resume, no cool-down");
      tileResumeStarting(&r, true);
    }
    ok(r.strikes == 1, "...each boot resume counts one strike, cleared by the next clean power-off");
    // ...whereas three CRASHES (no power-off record: the strikes carry over) do give up.
    uint8_t s = 0;
    for (int crash = 0; crash < 3; crash++) {
      tileResumeLoaded(&r, s, 0, TILE_STOP_NONE, T);
      s = tileResumeStarting(&r, true);
    }
    tileResumeLoaded(&r, s, 0, TILE_STOP_NONE, T);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GAVE_UP && r.giveUp == TILE_GIVEUP_STRIKES,
       "three crashes in a row: stops resuming");
    ok(!strcmp(tileStopReasonName(TILE_STOP_POWEROFF), "power-off"), "the console names it");

    tileResumeStopped(&r, TILE_STOP_CARDFAILS, T);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GAVE_UP && r.giveUp == TILE_GIVEUP_CARD, "the card refused writes: never by itself");
    tileResumeStopped(&r, TILE_STOP_USER, T);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_IDLE, "a user Stop: nothing pending");
    tileResumeFresh(&r, T);
    tileResumeStopped(&r, TILE_STOP_FINISHED, T);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_IDLE, "finished: nothing pending");

    // A refused automatic start cools down like a failure, never a retry every pass.
    tileResumeFresh(&r, T);
    tileResumeStopped(&r, TILE_STOP_NOWIFI, T);
    tileResumeRefused(&r, T + MIN);
    w = allClear(T + 2 * MIN);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_COOL && wait == 9 * MIN, "refused: 10 min before the next try");
    ok(r.refused, "...and the form can say the last try was refused, not that the network gave up");
    tileResumeStopped(&r, TILE_STOP_NETFAILS, T + 20 * MIN);
    ok(!r.refused, "a real stop clears it");

    // millis() wraps under a multi-day job: every comparison is a difference.
    tileResumeFresh(&r, 0xFFFFFF00u);
    tileResumeStopped(&r, TILE_STOP_NETFAILS, 0xFFFFFF00u);
    w = allClear(0x100u);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_COOL && wait == 10 * MIN - 512, "across the millis() wrap: 512 ms of 10 min gone");
  }

  group("resume: crash strikes count BOOT resumes only");
  {
    TileResume r;
    uint32_t wait = 0;
    TileWorld w = allClear(5000);
    tileResumeLoaded(&r, 0, TILE_GIVEUP_NONE, TILE_STOP_NONE, 5000);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GO, "a job running when the phone restarted resumes");
    ok(tileResumeStarting(&r, true) == 1, "...with a strike counted before it runs");
    ok(tileResumeStarting(&r, true) == 1 && !r.boot, "a second resume in the same boot counts nothing");
    tileResumeLoaded(&r, 2, TILE_GIVEUP_NONE, TILE_STOP_NONE, 5000);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GO && tileResumeStarting(&r, true) == 3, "strike 3 still runs");
    tileResumeLoaded(&r, 3, TILE_GIVEUP_NONE, TILE_STOP_NONE, 5000);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GAVE_UP && r.giveUp == TILE_GIVEUP_STRIKES,
       "three restarts in a row: stop resuming, and say why");
    tileResumeLoaded(&r, 2, TILE_GIVEUP_NONE, TILE_STOP_NONE, 5000);
    tileResumeStarting(&r, true);
    tileResumeStopped(&r, TILE_STOP_NOWIFI, 9000);
    ok(r.strikes == 0, "a run that ends with its reason recorded clears the strikes");
    tileResumeLoaded(&r, 2, TILE_GIVEUP_NONE, TILE_STOP_NONE, 5000);
    tileResumeStarting(&r, true);
    ok(!tileResumeProgress(&r, 49, 6000) && r.strikes == 3, "49 new tiles: not yet trusted");
    ok(tileResumeProgress(&r, 50, 7000) && r.strikes == 0, "50 new tiles: the strikes are cleared (persist it)");
    tileResumeLoaded(&r, 2, TILE_GIVEUP_NONE, TILE_STOP_NOWIFI, 5000);
    ok(tileResumeStarting(&r, false) == 2, "Resume pressed by a person is not a strike");
    tileResumeLoaded(&r, 0, TILE_GIVEUP_CARD, TILE_STOP_CARDFAILS, 5000);
    ok(tileResumeCheck(&r, &w, &wait) == TILE_GATE_GAVE_UP, "a give-up survives the restart");
    ok(!strcmp(tileStopReasonName(TILE_STOP_BATTERY), "battery") && !strcmp(tileStopReasonName(99), "?"), "reason names");
  }

  printf("\n%s%d passed, %d failed%s\n", g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail, "\033[0m");
  return g_fail ? 1 : 0;
}
