/*
 * test_maptiles.cpp — map_tiles.cpp and map_pins.cpp on the host.
 *
 * The projection numbers are computed independently (Python, same Web Mercator formulas,
 * double precision) and pasted in; the blit geometry is checked STRUCTURALLY rather than
 * against a golden list, because "did it cover the screen exactly once, and did each piece
 * come from the right place in the right tile" is the property that actually matters and it
 * is one a golden list does not state.
 *
 * Why this suite is worth its weight: a map that is one tile out looks perfectly fine on a
 * 240x320 screen. It just shows you the wrong woods, and there is nothing on the screen that
 * says so. Arithmetic errors here cannot be caught by looking, so they are caught here.
 */

#include "../WiPhone/map_tiles.h"
#include "../WiPhone/map_pins.h"
#include "../WiPhone/mesh_txq.h"      // MeshTxOutcome: the numbers mapPinMeshSettle spells as literals
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

static bool nearly(double a, double b, double eps) {
  return fabs(a - b) <= eps;
}

/* Every viewport pixel the world covers is painted exactly once, by a piece whose source
 * pixel is the right one. This is the whole correctness argument for the renderer. */
static void coverCheck(int z, int32_t cx, int32_t cy, int vw, int vh, const char* label) {
  MapBlit b[MAP_MAX_BLITS];
  const int n = mapViewBlits(z, cx, cy, vw, vh, b, MAP_MAX_BLITS);
  char nm[160];
  snprintf(nm, sizeof(nm), "%s: blits returned (%d)", label, n);
  CHECK(n > 0, nm);
  if (n <= 0) {
    return;
  }

  const int32_t ws = mapWorldPx(z);
  const long long left = (long long)cx - vw / 2;
  const long long top  = (long long)cy - vh / 2;

  int* paint = (int*)malloc(sizeof(int) * (size_t)vw * (size_t)vh);
  for (int i = 0; i < vw * vh; i++) {
    paint[i] = 0;
  }

  bool srcOk = true, mapOk = true, boundsOk = true;
  for (int i = 0; i < n; i++) {
    const MapBlit* q = &b[i];
    if (q->srcX < 0 || q->srcY < 0 || q->w <= 0 || q->h <= 0 ||
        q->srcX + q->w > MAP_TILE_PX || q->srcY + q->h > MAP_TILE_PX) {
      srcOk = false;
    }
    if (q->dstX < 0 || q->dstY < 0 || q->dstX + q->w > vw || q->dstY + q->h > vh) {
      boundsOk = false;
      continue;
    }
    if (q->tileX < 0 || q->tileY < 0 ||
        q->tileX >= ws / MAP_TILE_PX || q->tileY >= ws / MAP_TILE_PX) {
      boundsOk = false;
    }
    for (int y = 0; y < q->h; y++) {
      for (int x = 0; x < q->w; x++) {
        paint[(q->dstY + y) * vw + (q->dstX + x)]++;
        // The world pixel this screen pixel claims to be, two independent ways.
        long long wantX = (left + q->dstX + x) % ws;
        if (wantX < 0) {
          wantX += ws;
        }
        const long long gotX = (long long)q->tileX * MAP_TILE_PX + q->srcX + x;
        const long long wantY = top + q->dstY + y;
        const long long gotY = (long long)q->tileY * MAP_TILE_PX + q->srcY + y;
        if (gotX != wantX || gotY != wantY) {
          mapOk = false;
        }
      }
    }
  }
  snprintf(nm, sizeof(nm), "%s: every piece lies inside its tile", label);
  CHECK(srcOk, nm);
  snprintf(nm, sizeof(nm), "%s: every piece lies inside the viewport and a real tile", label);
  CHECK(boundsOk, nm);
  snprintf(nm, sizeof(nm), "%s: every pixel comes from the right world pixel", label);
  CHECK(mapOk, nm);

  bool exact = true;
  for (int y = 0; y < vh; y++) {
    const long long worldY = top + y;
    const int want = (worldY >= 0 && worldY < ws) ? 1 : 0;   // computed WITHOUT the code under test
    for (int x = 0; x < vw; x++) {
      if (paint[y * vw + x] != want) {
        exact = false;
      }
    }
  }
  snprintf(nm, sizeof(nm), "%s: covers the world exactly once, and nothing else", label);
  CHECK(exact, nm);
  free(paint);
}

/* ── THE STRETCH, DRAWN THE WAY THE PHONE DRAWS IT ──────────────────────────────────────
 * g_anc is an ancestor tile whose every pixel names itself: (y << 8) | x. A piece rendered
 * from it therefore says, pixel by pixel, which ancestor pixel it came from, and pieceMatches
 * compares that with the answer worked out straight from world pixels (the world pixel at the
 * view zoom, shifted down d levels) — no call to the code under test on that side. */
static uint16_t g_anc[MAP_TILE_PX * MAP_TILE_PX];
static uint16_t g_fb[MAP_TILE_PX * MAP_TILE_PX];     // one piece, w x h, stride w
static uint8_t  g_hits[MAP_TILE_PX * MAP_TILE_PX];

static void fillAncestorTile() {
  for (int y = 0; y < MAP_TILE_PX; y++) {
    for (int x = 0; x < MAP_TILE_PX; x++) {
      g_anc[y * MAP_TILE_PX + x] = (uint16_t)((y << 8) | x);
    }
  }
}

/* drawMap's loop, with the LCD replaced by g_fb: mapAncestorSrc, then one mapAncestorRow per
 * ancestor row, pushed once for each screen row it covers. *rowsOk goes false if a row lands
 * outside the piece, the rows are not contiguous from the top, any screen pixel is written
 * other than exactly once, or a widened row leaves a pixel unwritten. */
static bool renderPiece(const MapBlit* b, int d, const uint16_t* tile, MapAncestorBlit* ab, bool* rowsOk) {
  if (!mapAncestorSrc(b, d, ab)) {
    return false;
  }
  memset(g_hits, 0, sizeof(g_hits));
  uint16_t rowA[MAP_TILE_PX], rowB[MAP_TILE_PX];
  int nextTop = 0;
  for (int r = 0; r < ab->sh; r++) {
    int y0 = -1, y0b = -1;
    // Twice, over two different fillings: a pixel the row did not write differs between them.
    for (int x = 0; x < MAP_TILE_PX; x++) {
      rowA[x] = 0x0000;
      rowB[x] = 0xFFFF;
    }
    const int rows = mapAncestorRow(ab, tile, r, rowA, MAP_TILE_PX, &y0);
    const int rowsB = mapAncestorRow(ab, tile, r, rowB, MAP_TILE_PX, &y0b);
    if (rows < 1 || rows != rowsB || y0 != y0b || y0 != nextTop || y0 < 0 || y0 + rows > b->h) {
      *rowsOk = false;
      if (rows < 1) {
        continue;
      }
    }
    for (int x = 0; x < b->w; x++) {
      if (rowA[x] != rowB[x]) {
        *rowsOk = false;
      }
    }
    nextTop = y0 + rows;
    for (int k = 0; k < rows; k++) {
      const int y = y0 + k;
      if (y < 0 || y >= b->h) {
        *rowsOk = false;
        continue;
      }
      for (int x = 0; x < b->w; x++) {
        g_fb[y * b->w + x] = rowA[x];
        g_hits[y * b->w + x]++;
      }
    }
  }
  if (nextTop != b->h) {
    *rowsOk = false;
  }
  for (int i = 0; i < b->w * b->h; i++) {
    if (g_hits[i] != 1) {
      *rowsOk = false;
    }
  }
  return true;
}

// g_fb (rendered from g_anc) against the directly scaled reference, pixel by pixel.
static bool pieceMatches(const MapBlit* b, int d, const MapAncestorBlit* ab) {
  for (int j = 0; j < b->h; j++) {
    for (int i = 0; i < b->w; i++) {
      const long long X = (long long)b->tileX * MAP_TILE_PX + b->srcX + i;
      const long long Y = (long long)b->tileY * MAP_TILE_PX + b->srcY + j;
      const long long XL = X >> d, YL = Y >> d;          // the same ground, d levels up
      if ((XL >> 8) != ab->ax || (YL >> 8) != ab->ay) {
        return false;
      }
      if (g_fb[j * b->w + i] != (uint16_t)(((YL & 255) << 8) | (XL & 255))) {
        return false;
      }
    }
  }
  return true;
}

int main() {
  printf("test_maptiles\n");

  // ---- world geometry -------------------------------------------------------
  CHECK(mapWorldPx(0) == 256 && mapWorldPx(15) == 8388608 && mapWorldPx(19) == 134217728,
        "world size at z0/z15/z19");
  CHECK(mapWorldPx(-3) == 256 && mapWorldPx(99) == mapWorldPx(MAP_ZOOM_MAX),
        "zoom clamped both ways");

  {
    double wx = 0, wy = 0;
    mapLatLonToWorld(47.6062, -122.3321, 15, &wx, &wy);      // Seattle
    CHECK(nearly(wx, 1343759.464676, 1e-3) && nearly(wy, 2929662.722766, 1e-3),
          "Seattle z15 world pixels match the reference");
    CHECK((int)wx / MAP_TILE_PX == 5249 && (int)wy / MAP_TILE_PX == 11443,
          "Seattle z15 lands in tile 5249/11443");

    mapLatLonToWorld(-33.8688, 151.2093, 14, &wx, &wy);      // Sydney: southern + eastern
    CHECK(nearly(wx, 3858868.032853, 1e-3) && nearly(wy, 2516969.330663, 1e-3),
          "Sydney z14 world pixels match the reference");

    mapLatLonToWorld(0, 0, 0, &wx, &wy);
    CHECK(nearly(wx, 128.0, 1e-9) && nearly(wy, 128.0, 1e-9), "null island is the middle of z0");

    double lat = 0, lon = 0;
    mapLatLonToWorld(47.6062, -122.3321, 15, &wx, &wy);
    mapWorldToLatLon(wx, wy, 15, &lat, &lon);
    CHECK(nearly(lat, 47.6062, 1e-9) && nearly(lon, -122.3321, 1e-9),
          "lat/lon -> world -> lat/lon round trips");
  }

  CHECK(nearly(mapWrapLon(180.0), -180.0, 1e-12), "+180 and -180 are the same meridian");
  CHECK(nearly(mapWrapLon(-180.0), -180.0, 1e-12), "-180 stays -180");
  CHECK(nearly(mapWrapLon(190.0), -170.0, 1e-12), "190 wraps to -170, it does not clamp");
  CHECK(nearly(mapWrapLon(-190.0), 170.0, 1e-12), "-190 wraps to 170");
  CHECK(nearly(mapClampLat(90.0), MAP_LAT_LIMIT, 1e-12) &&
        nearly(mapClampLat(-90.0), -MAP_LAT_LIMIT, 1e-12), "poles clamp to the Mercator limit");

  CHECK(mapDegToI7(47.6062) == 476062000 && mapDegToI7(-122.3321) == -1223321000,
        "degrees -> 1e-7 fixed point");
  CHECK(mapDegToI7(1.00000005) == 10000001 && mapDegToI7(-1.00000005) == -10000001,
        "rounding is symmetric about zero, not truncation");
  CHECK(nearly(mapI7ToDeg(476062000), 47.6062, 1e-9), "1e-7 fixed point -> degrees");

  CHECK(nearly(mapMetersPerPixel(47.6062, 15), 3.220972649878169, 1e-9),
        "metres per pixel at Seattle z15");
  CHECK(nearly(mapMetersPerPixel(0, 0), 156543.03392804097, 1e-6),
        "metres per pixel at the equator, z0");

  // ---- the blits ------------------------------------------------------------
  {
    const int VW = 240, VH = 250;
    int32_t cx = 1343759, cy = 2929662;
    coverCheck(15, cx, cy, VW, VH, "Seattle z15");

    // Exactly on a tile corner: the case where an off-by-one hides.
    coverCheck(15, 5249 * 256, 11443 * 256, VW, VH, "tile corner");
    coverCheck(15, 5249 * 256 + 128, 11443 * 256 + 128, VW, VH, "tile centre");

    // Straddling the antimeridian: the right-hand tiles must wrap to column 0.
    const int32_t ws12 = mapWorldPx(12);
    coverCheck(12, ws12 - 4, 200000, VW, VH, "antimeridian seam");
    {
      MapBlit b[MAP_MAX_BLITS];
      const int n = mapViewBlits(12, ws12 - 4, 200000, VW, VH, b, MAP_MAX_BLITS);
      bool sawWrap = false;
      for (int i = 0; i < n; i++) {
        if (b[i].tileX == 0) {
          sawWrap = true;
        }
      }
      CHECK(sawWrap, "the seam really does pull in tile column 0");
    }

    // A world shorter than the screen: letterboxed, never repeated vertically.
    coverCheck(0, 128, 128, VW, 300, "z0 with a 300px viewport (letterbox)");

    // A world NARROWER than the viewport: the world repeats sideways (longitude is a
    // circle) rather than looping forever or overrunning the blit array.
    coverCheck(0, 128, 128, 300, 200, "z0 with a 300px-wide viewport (world repeats)");

    // An odd viewport height: the clamp's vh/2 must not lose a row.
    coverCheck(14, 3858868, 2516969, 240, 251, "odd viewport height");

    // A pan far past the world, at the deepest zoom, must fold rather than overflow int32.
    {
      int32_t px = mapWorldPx(19) - 3, py = 100000;
      mapClampView(19, 240, 250, &px, &py);
      CHECK(mapPanView(19, 240, 250, 2000000000, 0, &px, &py) == 1 &&
            px >= 0 && px < mapWorldPx(19), "a two-billion-pixel pan folds into the world");
      coverCheck(19, px, py, 240, 250, "z19 after the huge pan");
    }

    // A cap that is too small must refuse outright, not half-draw.
    MapBlit one[1];
    CHECK(mapViewBlits(15, cx, cy, VW, VH, one, 1) == -1, "too few blit slots refuses");
    CHECK(mapViewBlits(15, cx, cy, VW, VH, NULL, 8) == -1, "a NULL blit array refuses");
  }

  // ---- panning and zooming --------------------------------------------------
  {
    const int VW = 240, VH = 250;
    int32_t cx = 1343759, cy = 2929662;
    const int32_t x0 = cx, y0 = cy;
    CHECK(mapPanView(15, VW, VH, 96, 0, &cx, &cy) == 1, "panning east moves the view");
    CHECK(mapPanView(15, VW, VH, -96, 0, &cx, &cy) == 1, "panning back west moves it again");
    CHECK(cx == x0 && cy == y0, "east then west lands on the same pixel");

    // Against the northern stop: the clamp holds and says nothing moved.
    cx = 1000;
    cy = 0;
    mapClampView(15, VW, VH, &cx, &cy);
    CHECK(cy == VH / 2, "the view clamps to the top of the world");
    CHECK(mapPanView(15, VW, VH, 0, -40, &cx, &cy) == 0, "panning past the top reports no move");

    // Longitude wraps instead of stopping.
    const int32_t ws = mapWorldPx(15);
    cx = ws - 10;
    cy = 2929662;
    CHECK(mapPanView(15, VW, VH, 40, 0, &cx, &cy) == 1 && cx == 30,
          "panning east off the edge comes back on the west side");

    // Zoom keeps the ground under the crosshair and is reversible.
    cx = 1343759;
    cy = 2929662;
    double lat0 = 0, lon0 = 0;
    mapViewToLatLon(15, cx, cy, VW, VH, VW / 2, VH / 2, &lat0, &lon0);
    int z = mapZoomView(10, 17, 15, +1, VW, VH, &cx, &cy);
    CHECK(z == 16, "zoom in goes to 16");
    double lat1 = 0, lon1 = 0;
    mapViewToLatLon(z, cx, cy, VW, VH, VW / 2, VH / 2, &lat1, &lon1);
    CHECK(nearly(lat0, lat1, 1e-4) && nearly(lon0, lon1, 1e-4),
          "the crosshair is still over the same ground after zooming in");
    z = mapZoomView(10, 17, z, -1, VW, VH, &cx, &cy);
    CHECK(z == 15 && cx == 1343759 && cy == 2929662, "zoom in then out is exactly reversible");
    CHECK(mapZoomView(12, 15, 15, +1, VW, VH, &cx, &cy) == 15,
          "zoom stops at the deepest level the card has");
    CHECK(mapZoomView(12, 15, 12, -1, VW, VH, &cx, &cy) == 12,
          "zoom stops at the shallowest level the card has");
  }

  CHECK(mapPanStep(0) == MAP_PAN_NUDGE_PX && MAP_PAN_NUDGE_PX == 12, "a tap is the 12 px nudge");
  {
    bool grows = true;
    for (int i = 1; i < 30; i++) {
      if (mapPanStep(i) < mapPanStep(i - 1)) {
        grows = false;
      }
    }
    CHECK(grows && mapPanStep(1) == 24 && mapPanStep(100) == 96,
          "a held key starts at 24 px, accelerates, and stops accelerating at 96");
  }
  /* A hold moves by TIME (map_tiles.h, mapPanHoldMove): the same distance for the same
   * milliseconds whether the ticks come every 100 ms or, as measured on the phone, every
   * 125-170. */
  {
    int carry = 0;
    CHECK(mapPanHoldMove(1, 100, &carry) == 24 && carry == 0, "a 100 ms tick at run 1 is the 24 px step");
    carry = 0;
    int a = mapPanHoldMove(1, 165, &carry);
    int b = mapPanHoldMove(1, 165, &carry);
    CHECK(a + b == 79 && carry == 20, "two 165 ms ticks move 3.3 steps, remainder carried (79 px, 20 px.ms over)");
    carry = 0;
    int slow = 0;
    for (int i = 0; i < 4; i++) {
      slow += mapPanHoldMove(9, 125, &carry);       /* 500 ms in five uneven... */
    }
    int fast = 0;
    int carry2 = 0;
    for (int i = 0; i < 5; i++) {
      fast += mapPanHoldMove(9, 100, &carry2);      /* ...or on-time frames */
    }
    CHECK(slow == fast && fast == 480, "500 ms at full speed is 480 px however the frames fall");
    carry = 0;
    CHECK(mapPanHoldMove(9, 1500, &carry) == mapPanHoldMove(9, MAP_PAN_HOLD_MAX_DT_MS, &carry),
          "a stalled tick moves at most MAP_PAN_HOLD_MAX_DT_MS worth: no teleport");
    carry = 0;
    CHECK(mapPanHoldMove(0, 100, &carry) == 24, "run 0 never reaches the hold (it is the tap): treated as run 1");
    CHECK(mapPanHoldMove(3, 50, NULL) == 24, "a null carry is allowed: 50 ms at 48 px/step is 24 px");
  }
  /* The landing snap's two relations (map_tiles.h). Break either and the map misbehaves in
   * a way no test of the snap itself would show: a radius >= the nudge means the tap after a
   * snap lands back inside it and the crosshair is STUCK on the marker; a radius under
   * nudge/2 x sqrt(2) leaves markers that taps on the two axes can never get near enough. */
  CHECK(MAP_SNAP_RADIUS_PX < MAP_PAN_NUDGE_PX, "one tap steps clear of a snapped marker");
  CHECK(2 * MAP_SNAP_RADIUS_PX * MAP_SNAP_RADIUS_PX >= MAP_PAN_NUDGE_PX * MAP_PAN_NUDGE_PX,
        "every marker can be reached to within the snap radius by taps");

  {
    int px = 0;
    CHECK(mapScaleBar(3.220972649878169, 100, &px) == 200 && px == 62,
          "Seattle z15 scale bar is 200 m / 62 px");
    CHECK(mapScaleBar(0.0, 100, &px) == 0 && px == 0, "a nonsense scale asks for no bar");
    const int m = mapScaleBar(156543.0, 100, &px);
    CHECK(m > 0 && px >= 1 && px <= 100, "a z0 scale bar still fits the box");
  }

  // ---- tile paths and area names -------------------------------------------
  {
    char p[128];
    CHECK(mapTilePath(p, sizeof(p), "/maps", "home", 15, 5249, 11443) > 0 &&
          !strcmp(p, "/maps/home/15/5249/11443.565"), "tile path");
    char tiny[8];
    CHECK(mapTilePath(tiny, sizeof(tiny), "/maps", "home", 15, 5249, 11443) == 0 &&
          tiny[0] == '\0', "a path that will not fit is refused, never truncated");
    CHECK(mapTilePath(p, sizeof(p), "/maps", "home", 15, -1, 0) == 0, "a negative tile is refused");
  }
  CHECK(mapAreaNameOk("home") && mapAreaNameOk("unit-3") && mapAreaNameOk("elk_2026"),
        "ordinary area names are accepted");
  CHECK(!mapAreaNameOk(".DS_Store") && !mapAreaNameOk("._home"),
        "macOS sidecar files are not areas");
  CHECK(!mapAreaNameOk("") && !mapAreaNameOk("a/b") && !mapAreaNameOk("a b"),
        "empty, pathy and spacey names are refused");
  CHECK(!mapAreaNameOk("0123456789012345678901234567890123"), "an over-long name is refused");

  // ---- pins -----------------------------------------------------------------
  {
    MapPin p;
    CHECK(mapPinParseLine("476062000,-1223321000,0,Camp", &p) == 1 &&
          p.latI == 476062000 && p.lonI == -1223321000 && p.sharedId == 0 &&
          !strcmp(p.name, "Camp") && p.chan[0] == '\0',
          "the older four-field line parses, with no channel");
    CHECK(mapPinParseLine("476062000,-1223321000,99,hunt-group,Camp", &p) == 1 &&
          p.sharedId == 99 && !strcmp(p.chan, "hunt-group") && !strcmp(p.name, "Camp"),
          "the five-field line carries the channel before the name");
    CHECK(mapPinParseLine("476062000,-1223321000,99,,Camp, the far one", &p) == 1 &&
          p.chan[0] == '\0' && !strcmp(p.name, "Camp  the far one"),
          "an empty channel field is fine, and a comma in the name is still a space");
    CHECK(mapPinParseLine("476062000,-1223321000,3735928559,The gate", &p) == 1 &&
          p.sharedId == 3735928559u && !strcmp(p.name, "The gate"),
          "a shared pin keeps its waypoint id, and a name may hold spaces");
    CHECK(mapPinParseLine("", &p) == 0 && mapPinParseLine("  # a note", &p) == 0,
          "blank lines and comments are skipped, not errors");
    CHECK(mapPinParseLine("476062000,-1223321000,0", &p) == -1, "a short line is malformed");
    CHECK(mapPinParseLine("47.6062,-122.3,0,Camp", &p) == -1,
          "a float latitude is malformed, not silently truncated");
    CHECK(mapPinParseLine("950000000,0,0,North pole", &p) == -1, "an impossible latitude is refused");
    CHECK(mapPinParseLine("0,1900000000,0,East of east", &p) == -1, "an impossible longitude is refused");
    CHECK(mapPinParseLine("476062000,-1223321000,0,", &p) == 1 && !strcmp(p.name, "Pin"),
          "an empty name becomes something you can see");

    char line[MAP_PIN_LINE_MAX];
    MapPin q;
    memset(&q, 0, sizeof(q));
    q.latI = -338688000;
    q.lonI = 1512093000;
    q.sharedId = 7;
    strcpy(q.name, "Truck");
    CHECK(mapPinFormatLine(&q, line, sizeof(line)) > 0 &&
          !strcmp(line, "-338688000,1512093000,7,,Truck"), "a never-shared pin formats with an empty channel");
    strcpy(q.chan, "hunt-group");
    CHECK(mapPinFormatLine(&q, line, sizeof(line)) > 0 &&
          !strcmp(line, "-338688000,1512093000,7,hunt-group,Truck"), "a shared pin formats with its channel");
    MapPin r;
    CHECK(mapPinParseLine(line, &r) == 1 && r.latI == q.latI && r.lonI == q.lonI &&
          r.sharedId == q.sharedId && !strcmp(r.name, q.name) && !strcmp(r.chan, q.chan),
          "format -> parse round trips, channel included");
    strcpy(q.chan, "odd,name");
    CHECK(mapPinFormatLine(&q, line, sizeof(line)) > 0 && mapPinParseLine(line, &r) == 1 &&
          !strcmp(r.chan, "odd name") && !strcmp(r.name, "Truck"),
          "a comma in a channel name cannot become a sixth field");
    char small[10];
    CHECK(mapPinFormatLine(&q, small, sizeof(small)) == 0 && small[0] == '\0',
          "a line that will not fit is refused, never truncated");

    char nm[MAP_PIN_NAME_LEN];
    mapPinSanitizeName("  Big, Rock  ", nm, sizeof(nm));
    CHECK(!strcmp(nm, "Big  Rock"), "commas become spaces and the ends are trimmed");
    mapPinSanitizeName("a\nb\tc", nm, sizeof(nm));
    CHECK(!strcmp(nm, "a b c"), "control characters become spaces");
    mapPinSanitizeName("   ", nm, sizeof(nm));
    CHECK(!strcmp(nm, "Pin"), "a name of nothing becomes 'Pin'");
    mapPinSanitizeName("012345678901234567890123456789", nm, sizeof(nm));
    CHECK(strlen(nm) == MAP_PIN_NAME_LEN - 1, "a long name is cut to the waypoint's length");

    /* A cut that lands mid-codepoint must drop the whole character. These bytes go into a
     * protobuf string field on the air; half a sequence is invalid UTF-8 and a strict reader
     * may reject the entire Waypoint. 19 usable bytes: 18 ASCII then a 2-byte 'e' with an
     * acute, whose second byte does not fit. */
    mapPinSanitizeName("aaaaaaaaaaaaaaaaaa\xc3\xa9zz", nm, sizeof(nm));
    CHECK(strlen(nm) == 18 && nm[17] == 'a',
          "a name cut mid-UTF-8 drops the whole character, not half of it");
    bool tail_ok = true;
    for (size_t i = 0; i < strlen(nm); i++) {
      if (((unsigned char)nm[i] & 0xC0) == 0x80) {
        tail_ok = false;                     // a bare continuation byte survived
      }
    }
    CHECK(tail_ok, "...and no orphan continuation byte is left behind");
    // A sequence that FITS is untouched: 17 ASCII + a 2-byte character = 19 bytes.
    mapPinSanitizeName("aaaaaaaaaaaaaaaaa\xc3\xa9", nm, sizeof(nm));
    CHECK(strlen(nm) == 19, "a multi-byte character that fits is kept whole");
  }

  {
    MapPin pins[3];
    memset(pins, 0, sizeof(pins));
    strcpy(pins[0].name, "Pin 1");
    strcpy(pins[1].name, "Pin 3");
    strcpy(pins[2].name, "Camp");
    char nm[MAP_PIN_NAME_LEN];
    CHECK(mapPinAutoName(pins, 3, nm, sizeof(nm)) == 2 && !strcmp(nm, "Pin 2"),
          "the next pin fills the gap rather than counting up forever");
    CHECK(mapPinAutoName(NULL, 0, nm, sizeof(nm)) == 1 && !strcmp(nm, "Pin 1"),
          "the first pin on an empty map is Pin 1");
  }

  {
    const int vx[4] = { 120, 130,  10, 120 };
    const int vy[4] = { 125, 128, 240, 125 };
    CHECK(mapPinPickNearest(vx, vy, 4, 120, 125, 12) == 0, "the pin under the crosshair wins");
    CHECK(mapPinPickNearest(vx, vy, 4, 131, 129, 12) == 1, "the nearer of two close pins wins");
    CHECK(mapPinPickNearest(vx, vy, 4, 200, 20, 12) == -1, "nothing near the crosshair picks nothing");
    CHECK(mapPinPickNearest(vx, vy, 4, 120, 125, 0) == 0, "a zero radius still matches an exact hit");

    /* 🛑 THE OVERFLOW. mapLatLonToView writes through for points nowhere near the screen, and
     * at z19 the world is 134 million pixels across. Squared in 32-bit that wraps, and a
     * wrapped product can come out NEGATIVE — which beats every real distance, so a pin on
     * the far side of the world would be picked as the one under the crosshair. */
    const int farX[3] = { 120, 46341, 134217000 };
    const int farY[3] = { 999, 46341, 134217000 };
    CHECK(mapPinPickNearest(farX, farY, 3, 120, 125, 14) == -1,
          "pins a world away are not 'under the crosshair', whatever the arithmetic wraps to");
    const int mixX[3] = { 134217000, 121, -134217000 };
    const int mixY[3] = { 134217000, 126, -134217000 };
    CHECK(mapPinPickNearest(mixX, mixY, 3, 120, 125, 14) == 1,
          "...and the one that IS under it still wins, with huge ones on both sides");
  }

  /* ── THE STRETCH: A POSITION DRAWN FROM THE TILE `d` LEVELS UP ─────────────────────────
   * Every piece below is drawn exactly as drawMap draws it (renderPiece: mapAncestorSrc, then
   * mapAncestorRow per ancestor row, each pushed once per screen row it covers) and compared
   * with the answer worked out directly from world pixels. The failure this exists for is the
   * plausible one: a stretched tile showing its NEIGHBOUR's ground, or its own ground a few
   * pixels off — both look like a map. */
  fillAncestorTile();

  // The four-colour parent: each child, grandchild, great-grandchild shows ITS quadrant only.
  {
    static uint16_t quad[MAP_TILE_PX * MAP_TILE_PX];
    for (int y = 0; y < MAP_TILE_PX; y++) {
      for (int x = 0; x < MAP_TILE_PX; x++) {
        quad[y * MAP_TILE_PX + x] = (uint16_t)(1 + (x >= 128 ? 1 : 0) + (y >= 128 ? 2 : 0));
      }
    }
    for (int d = 1; d <= 3; d++) {
      const int kids = 1 << d;
      bool allOwn = true, tileOk = true;
      for (int j = 0; j < kids; j++) {
        for (int i = 0; i < kids; i++) {
          // The parent is tile (37, 90) at its level; child (i, j) of it, a whole tile.
          MapBlit b = { 37 * kids + i, 90 * kids + j, 0, 0, 0, 0, MAP_TILE_PX, MAP_TILE_PX };
          MapAncestorBlit ab;
          bool rowsOk = true;
          if (!renderPiece(&b, d, quad, &ab, &rowsOk) || !rowsOk) {
            allOwn = false;
            continue;
          }
          if (ab.ax != 37 || ab.ay != 90) {
            tileOk = false;
          }
          const uint16_t want = (uint16_t)(1 + ((i * 2) / kids) + ((j * 2) / kids) * 2);
          for (int p = 0; p < MAP_TILE_PX * MAP_TILE_PX; p++) {
            if (g_fb[p] != want) {
              allOwn = false;
            }
          }
        }
      }
      char nm[96];
      snprintf(nm, sizeof(nm), "d=%d: all %d descendants of the parent read the parent tile", d, kids * kids);
      CHECK(tileOk, nm);
      snprintf(nm, sizeof(nm), "d=%d: each one shows its own quadrant, never a neighbour's", d);
      CHECK(allOwn, nm);
    }
  }

  // THE PHASE: a piece starting part-way through an ancestor pixel (srcX 203 at d = 4 is
  // ancestor pixel 12 plus 11/16), against the directly scaled reference, pixel by pixel.
  {
    const MapBlit b = { 16 * 500 + 13, 16 * 700 + 6, 203, 77, 0, 0, MAP_TILE_PX - 203, MAP_TILE_PX - 77 };
    MapAncestorBlit ab;
    bool rowsOk = true;
    const bool ok = renderPiece(&b, 4, g_anc, &ab, &rowsOk);
    CHECK(ok && ab.ax == 500 && ab.ay == 700, "srcX 203 srcY 77 at d=4: the right ancestor tile");
    CHECK(ok && ab.scale == 16 && ab.phx == 11 && ab.phy == 13 && ab.sx0 == 13 * 16 + 12 &&
          ab.sy0 == 6 * 16 + 4, "...the phase is kept, not rounded away (first column 5 px, first row 3 px)");
    CHECK(ok && rowsOk, "...its rows cover the piece exactly once, top to bottom");
    CHECK(ok && pieceMatches(&b, 4, &ab), "...and every pixel is the directly scaled reference's");
  }

  // Every blit of real views, at every d: never a source outside the tile, never a pixel
  // outside the piece, always the right ground.
  {
    struct { int z; double lat, lon; } views[] = {
      { 12, 47.5, -121.8 }, { 15, 47.6062, -122.3321 }, { 17, 47.42, -121.75 },
      { 18, 47.5, -121.8 }, { 19, -33.8688, 151.2093 }, { 18, 47.5, 179.9995 },
    };
    bool srcOk = true, rowsAll = true, pixOk = true, refused = false;
    int pieces = 0;
    for (size_t v = 0; v < sizeof(views) / sizeof(views[0]); v++) {
      double wx = 0, wy = 0;
      mapLatLonToWorld(views[v].lat, views[v].lon, views[v].z, &wx, &wy);
      for (int shift = 0; shift < 3; shift++) {
        int32_t cx = (int32_t)wx + shift * 97, cy = (int32_t)wy + shift * 61;
        mapClampView(views[v].z, 240, 250, &cx, &cy);
        MapBlit bl[MAP_MAX_BLITS];
        const int n = mapViewBlits(views[v].z, cx, cy, 240, 250, bl, MAP_MAX_BLITS);
        for (int i = 0; i < n; i++) {
          for (int d = 0; d <= MAP_ANCESTOR_MAX_D; d++) {
            MapAncestorBlit ab;
            bool rowsOk = true;
            if (!renderPiece(&bl[i], d, g_anc, &ab, &rowsOk)) {
              refused = true;
              continue;
            }
            pieces++;
            if (ab.sx0 < 0 || ab.sy0 < 0 || ab.sx0 + ab.sw > MAP_TILE_PX || ab.sy0 + ab.sh > MAP_TILE_PX ||
                ab.phx < 0 || ab.phy < 0 || ab.phx >= ab.scale || ab.phy >= ab.scale) {
              srcOk = false;
            }
            if (!rowsOk) {
              rowsAll = false;
            }
            if (!pieceMatches(&bl[i], d, &ab)) {
              pixOk = false;
            }
          }
        }
      }
    }
    char nm[96];
    snprintf(nm, sizeof(nm), "%d pieces at d=0..%d: none refused", pieces, MAP_ANCESTOR_MAX_D);
    CHECK(!refused && pieces > 100, nm);
    CHECK(srcOk, "...every source rectangle inside [0,256), every phase inside one ancestor pixel");
    CHECK(rowsAll, "...every row lands inside its piece (dst >= 0), each screen row exactly once");
    CHECK(pixOk, "...and every pixel is the right ground at every d");
  }

  // d = 0 is the tile itself; past MAP_ANCESTOR_MAX_D there is no blit at all.
  {
    const MapBlit b = { 5249, 11443, 40, 16, 0, 0, 200, 240 };
    MapAncestorBlit ab;
    CHECK(mapAncestorSrc(&b, 0, &ab) == 1 && ab.ax == 5249 && ab.ay == 11443 && ab.sx0 == 40 &&
          ab.sy0 == 16 && ab.sw == 200 && ab.sh == 240 && ab.scale == 1 && ab.phx == 0 && ab.phy == 0,
          "d=0 is the identity: the tile's own pixels, unscaled");
    CHECK(mapAncestorSrc(&b, MAP_ANCESTOR_MAX_D, &ab) == 1 && ab.sw == 1 && ab.sh == 1 && ab.scale == 256,
          "d=8: a position is one ancestor pixel, 256x");
    CHECK(mapAncestorSrc(&b, MAP_ANCESTOR_MAX_D + 1, &ab) == 0,
          "d=9: no blit (256 >> 9 is 0 - a divide by zero on the phone)");
    CHECK(mapAncestorSrc(&b, -1, &ab) == 0, "a negative d: no blit");
    const MapBlit bad = { 5249, 11443, 200, 0, 0, 0, 100, 10 };   // runs off the tile's right edge
    CHECK(mapAncestorSrc(&bad, 1, &ab) == 0, "a blit that leaves its tile is refused");
    CHECK(mapAncestorSrc(NULL, 1, &ab) == 0 && mapAncestorSrc(&b, 1, NULL) == 0, "NULLs are refused");
    uint16_t row[8];
    int y0 = -1;
    CHECK(mapAncestorSrc(&b, 2, &ab) == 1 && mapAncestorRow(&ab, g_anc, ab.sh, row, 8, &y0) == 0 && y0 == 0,
          "a row past the piece draws nothing");
  }

  // ── Z18: ONE LEVEL PAST OPENTOPOMAP'S Z17 ────────────────────────────────────────────
  CHECK(mapWorldPx(18) == 67108864, "world size at z18");
  {
    int px = 0;
    CHECK(mapScaleBar(mapMetersPerPixel(47.5, 18), 84, &px) == 20 && px == 50,
          "z18 scale bar at 47.5 N (0.403 m/px) is 20 m / 50 px");
  }
  {
    /* The round trip, and the property that keeps the stretched level honest: z17 -> z18 -> z17
     * lands on the same pixel, and EVERY screen pixel of the z18 view, drawn from the z17 tiles
     * at d = 1, is the z17 pixel under it — the crosshair included, so the readout, the pins and
     * the stretched ground agree exactly. */
    double wx = 0, wy = 0;
    mapLatLonToWorld(47.5, -121.8, 17, &wx, &wy);
    int32_t cx = (int32_t)(wx + 0.5), cy = (int32_t)(wy + 0.5);
    mapClampView(17, 240, 250, &cx, &cy);
    const int32_t cx17 = cx, cy17 = cy;
    const int nz = mapZoomView(11, 17 + MAP_OVERZOOM_MAX, 17, +1, 240, 250, &cx, &cy);
    CHECK(nz == 18 && cx == cx17 * 2 && cy == cy17 * 2, "zoom in from z17 reaches the stretched z18");
    coverCheck(18, cx, cy, 240, 250, "z18 at 47.5,-121.8");
    MapBlit bl[MAP_MAX_BLITS];
    const int n = mapViewBlits(18, cx, cy, 240, 250, bl, MAP_MAX_BLITS);
    static uint16_t view[240 * 250];
    static int32_t viewTx[240 * 250], viewTy[240 * 250];
    bool drawn = n > 0;
    for (int i = 0; i < n; i++) {
      MapAncestorBlit ab;
      bool rowsOk = true;
      if (!renderPiece(&bl[i], 1, g_anc, &ab, &rowsOk) || !rowsOk) {
        drawn = false;
        continue;
      }
      for (int y = 0; y < bl[i].h; y++) {
        for (int x = 0; x < bl[i].w; x++) {
          view[(bl[i].dstY + y) * 240 + bl[i].dstX + x] = g_fb[y * bl[i].w + x];
          viewTx[(bl[i].dstY + y) * 240 + bl[i].dstX + x] = ab.ax;
          viewTy[(bl[i].dstY + y) * 240 + bl[i].dstX + x] = ab.ay;
        }
      }
    }
    bool same = drawn;
    for (int y = 0; y < 250 && same; y++) {
      for (int x = 0; x < 240; x++) {
        const long long w17x = ((long long)cx - 120 + x) >> 1, w17y = ((long long)cy - 125 + y) >> 1;
        if (viewTx[y * 240 + x] != (int32_t)(w17x >> 8) || viewTy[y * 240 + x] != (int32_t)(w17y >> 8) ||
            view[y * 240 + x] != (uint16_t)(((w17y & 255) << 8) | (w17x & 255))) {
          same = false;
          break;
        }
      }
    }
    CHECK(same, "every z18 screen pixel drawn from z17 is the z17 pixel under it");
    CHECK(view[125 * 240 + 120] == (uint16_t)(((cy17 & 255) << 8) | (cx17 & 255)),
          "...the crosshair's included: exactly the z17 crosshair pixel");
    const int back = mapZoomView(11, 18, 18, -1, 240, 250, &cx, &cy);
    CHECK(back == 17 && cx == cx17 && cy == cy17, "...and zooming back out lands on the z17 pixel it left");
  }

  // ── THE LABEL: deepest level first, each end beside its own factor ─────────────────────
  {
    char s[48];
    CHECK(mapStretchLabel(s, sizeof(s), 17, 15, 15, 0) > 0 && !strcmp(s, "z15 tiles stretched 4x"),
          "one level: 'z15 tiles stretched 4x'");
    CHECK(mapStretchLabel(s, sizeof(s), 18, 17, 17, 0) > 0 && !strcmp(s, "z17 tiles stretched 2x"),
          "one level past OTM's z17: 'z17 tiles stretched 2x'");
    CHECK(mapStretchLabel(s, sizeof(s), 17, 14, 16, 0) > 0 && !strcmp(s, "z16-14 stretched 2-8x"),
          "several: 'z16-14 stretched 2-8x' - z16 is the 2x, z14 the 8x");
    CHECK(mapStretchLabel(s, sizeof(s), 18, 11, 17, 0) > 0 && !strcmp(s, "z17-11 stretched 2-128x"),
          "the widest mix at z18: 'z17-11 stretched 2-128x'");
    CHECK(mapStretchLabel(s, sizeof(s), 17, 14, 16, 1) > 0 && !strcmp(s, "z16-14 x2-8"),
          "compact, several: 'z16-14 x2-8'");
    CHECK(mapStretchLabel(s, sizeof(s), 17, 15, 15, 1) > 0 && !strcmp(s, "z15 x4"),
          "compact, one level: 'z15 x4'");
    CHECK(mapStretchLabel(s, sizeof(s), 17, 16, 14, 0) == 0 && s[0] == '\0',
          "levels the wrong way round: no label");
    CHECK(mapStretchLabel(s, sizeof(s), 17, 17, 17, 0) == 0 && s[0] == '\0',
          "a level at the view zoom is not stretched: no label");
    CHECK(mapStretchLabel(s, sizeof(s), 18, 9, 17, 0) == 0, "a factor past 2^MAP_ANCESTOR_MAX_D: no label");
    char tiny[10];
    CHECK(mapStretchLabel(tiny, sizeof(tiny), 17, 14, 16, 0) == 0 && tiny[0] == '\0',
          "a label that will not fit is refused, never cut (a cut one states the wrong factor)");
  }

  /* ── THE MISS CACHE: the least recently walked-past entry goes ─────────────────────────
   * The frame on the screen stamps every hole it walks past with the newest number, so this
   * rule is also "never a hole that is on the screen" while any other entry exists. */
  {
    MapMiss m[4];
    memset(m, 0, sizeof(m));
    int n = 0;
    bool appends = true;
    for (int i = 0; i < 4; i++) {
      const int at = mapMissSlot(m, n, 4);
      if (at != i) {
        appends = false;
        break;
      }
      m[at].z = (int8_t)(14 + i);
      m[at].tx = 1000 + i;
      m[at].ty = 2000 + i;
      n = at + 1;
    }
    CHECK(appends && n == 4, "a cache with room appends");
    CHECK(mapMissFind(m, n, 16, 1002, 2002) == 2 && mapMissFind(m, n, 15, 1002, 2002) == -1,
          "find matches level AND tile");
    m[0].hit = 5; m[1].hit = 3; m[2].hit = 7; m[3].hit = 4;
    CHECK(mapMissSlot(m, 4, 4) == 1, "full: the entry walked past longest ago is replaced");
    m[1].hit = 7;                    // the frame on the screen (7) walks past entry 1 again
    CHECK(mapMissSlot(m, 4, 4) == 3,
          "...and once the screen walks past it again, it is kept and the next oldest goes");
    m[0].hit = 0xFFFFFFFEu; m[1].hit = 2; m[2].hit = 1; m[3].hit = 0xFFFFFFFFu;
    CHECK(mapMissSlot(m, 4, 4) == 0, "...counted with a signed difference, so the frame counter may wrap");
    m[0].hit = m[1].hit = m[2].hit = m[3].hit = 9;
    CHECK(mapMissSlot(m, 4, 4) == 0, "all on the screen (the cache is sized so this cannot happen): still a slot");
    CHECK(mapMissSlot(m, 0, 0) == -1 && mapMissSlot(NULL, 0, 4) == -1, "no room at all: -1");
    CHECK(MAP_ANCESTOR_MAX_D + 1 == 9, "a position walks at most 9 levels (4 positions x 9 = 36 holes on screen)");
  }

  // ── THE RULER'S DASHES ──────────────────────────────────────────────────────────────
  {
    const int BX = 0, BY = 26, BW = 240, BH = 250, X1 = 120, Y1 = 26 + 125;
    int64_t first = -1, end = -1;
    int64_t steps = mapDashRange(100, 60, X1, Y1, BX, BY, BW, BH, 6, &first, &end);
    CHECK(steps == 91 / 6 && first == 0 && end == steps, "an anchor on the screen: every step is tried");

    /* Brute force a line that enters from off-screen: the visible dashes, found by trying every
     * even step, all lie inside the range. */
    steps = mapDashRange(-3000, -2000, X1, Y1, BX, BY, BW, BH, 6, &first, &end);
    int visible = 0, missed = 0;
    for (int64_t s = 0; s < steps; s += 2) {
      int ax = 0, ay = 0, bx = 0, by = 0;
      mapDashAt(-3000, -2000, X1, Y1, s, steps, &ax, &ay);
      mapDashAt(-3000, -2000, X1, Y1, s + 1, steps, &bx, &by);
      const bool in = ax >= BX && ax < BX + BW && ay >= BY && ay < BY + BH &&
                      bx >= BX && bx < BX + BW && by >= BY && by < BY + BH;
      if (in) {
        visible++;
        if (s < first || s >= end) {
          missed++;
        }
      }
    }
    CHECK(visible > 5 && missed == 0 && (first % 2) == 0,
          "a line from off-screen: every visible dash is inside the range");
    CHECK(end - first < 60 && steps > 500, "...and the range is the visible part, not the whole line");

    /* A world away: z19 is 134 million px across. In int32, `dx * s` overflows past 113,512 px
     * (UBSan in this build would report it); the range walks a handful of steps. */
    steps = mapDashRange(-134000000, 90000000, X1, Y1, BX, BY, BW, BH, 6, &first, &end);
    int drawnFar = 0;
    for (int64_t s = first; s < end; s += 2) {
      int ax = 0, ay = 0, bx = 0, by = 0;
      mapDashAt(-134000000, 90000000, X1, Y1, s, steps, &ax, &ay);
      mapDashAt(-134000000, 90000000, X1, Y1, s + 1, steps, &bx, &by);
      if (ax >= BX && ax < BX + BW && ay >= BY && ay < BY + BH &&
          bx >= BX && bx < BX + BW && by >= BY && by < BY + BH) {
        drawnFar++;
      }
    }
    CHECK(steps > 20000000 && end - first < 60 && drawnFar > 5,
          "an anchor 134 million px away: no overflow, a few dozen steps tried, dashes drawn");
    int ex = 0, ey = 0;
    mapDashAt(-134000000, 90000000, X1, Y1, steps, steps, &ex, &ey);
    CHECK(ex == X1 && ey == Y1, "...and the last step is the crosshair");
    CHECK(mapDashRange(-5000, 0, 300, 125, BX, BY, BW, BH, 6, &first, &end) > 0 && first == 0 && end == 0,
          "a crosshair outside the box: nothing is tried");
    CHECK(mapDashRange(118, 150, X1, Y1, BX, BY, BW, BH, 6, &first, &end) == 0 && end == 0,
          "an anchor under the crosshair: no dashes");
  }

  /* Review M1 (2026-09-25): a pin's frame is QUEUED, not on the air. What the map does once the
   * radio answers — and when it gives up waiting — decides whether a place can be stranded on
   * every other radio with nothing left on this phone that could retract it. */
  {
    const uint8_t UNK = MESH_TXO_UNKNOWN, Q = MESH_TXO_QUEUED, S = MESH_TXO_SENT, F = MESH_TXO_FAILED;
    CHECK(UNK == 0 && Q == 1 && S == 2 && F == 3,
          "mapPinMeshSettle's literal outcome numbers are still MeshTxOutcome's");
    const uint8_t ops[4] = { MAP_PINOP_SHARE, MAP_PINOP_RESHARE, MAP_PINOP_UNSHARE, MAP_PINOP_DELETE };
    bool waits = true;
    for (int i = 0; i < 4; i++) {
      waits = waits && mapPinMeshSettle(ops[i], Q, false) == MAP_PINACT_WAIT;
    }
    CHECK(waits, "still queued: every operation waits - nothing is claimed, nothing is undone");
    CHECK(mapPinMeshSettle(MAP_PINOP_UNSHARE, S, false) == MAP_PINACT_FORGET_ID,
          "the retraction LEFT: only now is the waypoint id forgotten");
    CHECK(mapPinMeshSettle(MAP_PINOP_UNSHARE, F, false) == MAP_PINACT_KEEP_ID,
          "the retraction failed: the id stays, so it can be sent again (the M1 stranding)");
    CHECK(mapPinMeshSettle(MAP_PINOP_UNSHARE, Q, true) == MAP_PINACT_KEEP_ID &&
          mapPinMeshSettle(MAP_PINOP_UNSHARE, UNK, false) == MAP_PINACT_KEEP_ID,
          "no word on a retraction (app closed, forgotten): the id stays - the safe side");
    CHECK(mapPinMeshSettle(MAP_PINOP_SHARE, S, false) == MAP_PINACT_DONE &&
          mapPinMeshSettle(MAP_PINOP_RESHARE, S, false) == MAP_PINACT_DONE,
          "a share or an update that left: done");
    CHECK(mapPinMeshSettle(MAP_PINOP_SHARE, F, false) == MAP_PINACT_ROLL_BACK,
          "a FIRST share that never left rolls back: the pin is not drawn as shared");
    CHECK(mapPinMeshSettle(MAP_PINOP_RESHARE, F, false) == MAP_PINACT_KEEP_ID,
          "a failed UPDATE keeps the id: the mesh still holds the old copy");
    CHECK(mapPinMeshSettle(MAP_PINOP_SHARE, Q, true) == MAP_PINACT_KEEP_ID,
          "no word on a first share: keep the id (it may be on the air) rather than lose it");
    CHECK(mapPinMeshSettle(MAP_PINOP_DELETE, F, false) == MAP_PINACT_RESTORE,
          "a delete whose retraction never left puts the pin back, id and all");
    CHECK(mapPinMeshSettle(MAP_PINOP_DELETE, S, false) == MAP_PINACT_DONE &&
          mapPinMeshSettle(MAP_PINOP_DELETE, Q, true) == MAP_PINACT_DONE,
          "a delete that left, or with no word: the pin stays deleted (what deleting always did)");
    bool neverForgets = true;
    for (int i = 0; i < 4; i++) {
      for (uint8_t o = 0; o <= 3; o++) {
        for (int fin = 0; fin < 2; fin++) {
          if (mapPinMeshSettle(ops[i], o, fin != 0) == MAP_PINACT_FORGET_ID &&
              !(ops[i] == MAP_PINOP_UNSHARE && o == S)) {
            neverForgets = false;
          }
        }
      }
    }
    CHECK(neverForgets, "FORGET_ID comes from exactly one answer: a retraction reported SENT");

    /* The integration review's follow-up: Move here / Rename re-share a pin that carries an id,
     * and the id is now kept while its retraction waits — so the question "is a retraction of
     * THIS waypoint still queued?" must be asked before any re-share settles it. */
    const uint32_t WP = 0x5eed1234u, OTHER = 0x0badf00du;
    CHECK(mapPinRetractionQueued(MAP_PINOP_UNSHARE, Q, WP, WP),
          "'Take it off the mesh' still queued, same waypoint: a retraction is waiting");
    CHECK(mapPinRetractionQueued(MAP_PINOP_DELETE, Q, WP, WP),
          "a shared pin's delete (restorable) still queued: a retraction is waiting");
    CHECK(!mapPinRetractionQueued(MAP_PINOP_UNSHARE, S, WP, WP) &&
          !mapPinRetractionQueued(MAP_PINOP_UNSHARE, F, WP, WP) &&
          !mapPinRetractionQueued(MAP_PINOP_UNSHARE, UNK, WP, WP),
          "answered (sent, failed) or forgotten: nothing is waiting any more");
    CHECK(!mapPinRetractionQueued(MAP_PINOP_SHARE, Q, WP, WP) &&
          !mapPinRetractionQueued(MAP_PINOP_RESHARE, Q, WP, WP),
          "a share or an update still queued is not a retraction: an update may follow it");
    CHECK(!mapPinRetractionQueued(MAP_PINOP_UNSHARE, Q, WP, OTHER),
          "another pin's retraction does not hold THIS pin back");
    CHECK(!mapPinRetractionQueued(MAP_PINOP_UNSHARE, Q, 0, 0) &&
          !mapPinRetractionQueued(0, Q, 0, 0),
          "an unshared pin (id 0) and an empty op never match");
    /* The trap itself, as a sequence: Move here on a pin whose retraction is queued. The re-share
     * would settle it with no more waiting — which KEEPS the id, so the pin still reads shared
     * and the update would be queued behind the retraction. The guard must say "waiting" in
     * exactly the state where the settle says "keep". */
    const uint32_t pinIdAfterSettle =
        mapPinMeshSettle(MAP_PINOP_UNSHARE, Q, true) == MAP_PINACT_KEEP_ID ? WP : 0;
    CHECK(pinIdAfterSettle == WP && mapPinRetractionQueued(MAP_PINOP_UNSHARE, Q, WP, pinIdAfterSettle),
          "a still-queued retraction keeps the id to a settle, and the guard refuses the re-share");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  if (failures) {
    printf("test_maptiles: %d FAILURE(S)\n", failures);
  } else {
    printf("test_maptiles: all passed\n");
  }
  return failures ? 1 : 0;
}
