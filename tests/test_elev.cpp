/*
 * test_elev.cpp - elev_tiles.cpp on the host.
 *
 * Three kinds of proof:
 *   - WHERE: elevLocate() against the map's own projection (map_tiles.cpp's mapLatLonToWorld,
 *     linked in as the oracle) and against tile/pixel positions computed independently in
 *     Python. An altitude read from the pixel next door is a plausible altitude; only the
 *     arithmetic can say it is the wrong one.
 *   - WHAT: elevSampleZ()/elevSample() over synthetic tiles held in memory and served through
 *     the same ElevReadFn the phone's card reader implements - exact centres, bilinear
 *     midpoints, the edge clamp that never opens the neighbouring tile, no-data renormalised,
 *     the z13 -> z10 fallback, the antimeridian, and the byte order of negative heights.
 *   - DECODE: elevFromTerrarium() on hand-picked ties and limits, exhaustively against the
 *     float formula tools/make_elev_tiles.py uses, and on five real pixels of the terrarium
 *     tile z13/1330/2862 (near North Bend) whose metres were computed in Python from the PNG.
 */

#include "../WiPhone/elev_tiles.h"
#include "../WiPhone/map_tiles.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

static bool near(double a, double b, double tol) { return fabs(a - b) <= tol; }

// ── the in-memory card ──────────────────────────────────────────────────────────────────

struct MemTile {
  int z, x, y;
  bool used;
  int16_t v[ELEV_TILE_PX * ELEV_TILE_PX];
};

#define MAX_TILES 8
static MemTile g_tiles[MAX_TILES];     // 1 MB: global, never on the stack

struct Store {
  int reads;            // calls to the reader
  int failAt;           // 1-based read number that fails (0 = none)
  bool shortRead;       // every read returns n - 1
  bool badRequest;      // a read that was not 4 bytes, odd, or off the tile's end
  int foreign;          // reads of a tile other than `expectX/expectY` (when expectZ >= 0)
  int expectZ, expectX, expectY;
};

static MemTile* findTile(int z, int x, int y) {
  for (int i = 0; i < MAX_TILES; i++) {
    if (g_tiles[i].used && g_tiles[i].z == z && g_tiles[i].x == x && g_tiles[i].y == y) {
      return &g_tiles[i];
    }
  }
  return NULL;
}

static MemTile* addTile(int z, int x, int y) {
  MemTile* t = findTile(z, x, y);
  if (t) {
    return t;
  }
  for (int i = 0; i < MAX_TILES; i++) {
    if (!g_tiles[i].used) {
      g_tiles[i].used = true;
      g_tiles[i].z = z;
      g_tiles[i].x = x;
      g_tiles[i].y = y;
      return &g_tiles[i];
    }
  }
  abort();
}

static void fillConst(MemTile* t, int16_t v) {
  for (int i = 0; i < ELEV_TILE_PX * ELEV_TILE_PX; i++) {
    t->v[i] = v;
  }
}

static void setPx(MemTile* t, int col, int row, int16_t v) { t->v[row * ELEV_TILE_PX + col] = v; }

static int memRead(void* ctx, int z, int x, int y, uint32_t offset, uint8_t* buf, int n) {
  Store* s = (Store*)ctx;
  s->reads++;
  if (n != 4 || (offset & 1u) || offset + (uint32_t)n > ELEV_TILE_BYTES) {
    s->badRequest = true;
    return -1;
  }
  if (s->expectZ >= 0 && (z != s->expectZ || x != s->expectX || y != s->expectY)) {
    s->foreign++;
  }
  const MemTile* t = findTile(z, x, y);
  if (!t) {
    return -1;
  }
  if (s->failAt && s->reads == s->failAt) {
    return -1;
  }
  // The file's bytes: little-endian int16, written byte by byte like the .elv on the card.
  for (int k = 0; k < n / 2; k++) {
    const uint16_t u = (uint16_t)t->v[offset / 2 + (uint32_t)k];
    buf[2 * k] = (uint8_t)(u & 0xFF);
    buf[2 * k + 1] = (uint8_t)(u >> 8);
  }
  return s->shortRead ? n - 1 : n;
}

static void resetStore(Store* s) {
  memset(s, 0, sizeof(*s));
  s->expectZ = -1;
}

// The lat/lon of a fractional pixel inside a tile, through the map's inverse projection.
static void tilePoint(int z, int tx, int ty, double px, double py, double* lat, double* lon) {
  mapWorldToLatLon((double)tx * 256.0 + px, (double)ty * 256.0 + py, z, lat, lon);
}

// Sample tile (z,tx,ty) at pixel (px,py), expecting reads of that tile only.
static int sampleAt(Store* s, int z, int tx, int ty, double px, double py, double* m) {
  double lat, lon;
  tilePoint(z, tx, ty, px, py, &lat, &lon);
  s->reads = 0;
  s->expectZ = z;
  s->expectX = tx;
  s->expectY = ty;
  return elevSampleZ(memRead, s, z, lat, lon, m);
}

// Tile A's field: linear, so bilinear reproduces it exactly: v = 3i + 7j - 1000 at pixel (i, j).
static double linA(double u, double v) { return 3.0 * u + 7.0 * v - 1000.0; }

int main() {
  printf("test_elev\n");

  // ── the path ─────────────────────────────────────────────────────────────────────────
  {
    char p[64];
    CHECK(elevTilePath(p, sizeof(p), "/maps", 13, 1330, 2862) == 27 &&
          strcmp(p, "/maps/elev/13/1330/2862.elv") == 0, "path is <root>/elev/<z>/<x>/<y>.elv");
    CHECK(elevTilePath(p, sizeof(p), "/root/covey", 10, 0, 0) == 27 &&
          strcmp(p, "/root/covey/elev/10/0/0.elv") == 0, "any root, tile 0/0");
    CHECK(elevTilePath(p, 28, "/maps", 13, 1330, 2862) == 27 && strlen(p) == 27,
          "an exactly-fitting buffer is enough (27 chars + NUL)");
    strcpy(p, "junk");
    CHECK(elevTilePath(p, 27, "/maps", 13, 1330, 2862) == 0 && p[0] == '\0',
          "one byte short: refused and emptied, never truncated");
    strcpy(p, "junk");
    CHECK(elevTilePath(p, 0, "/maps", 13, 1330, 2862) == 0 && strcmp(p, "junk") == 0,
          "cap 0 writes nothing");
    CHECK(elevTilePath(NULL, 64, "/maps", 13, 1, 1) == 0, "NULL out");
    strcpy(p, "junk");
    CHECK(elevTilePath(p, sizeof(p), NULL, 13, 1, 1) == 0 && p[0] == '\0', "NULL root refused");
    CHECK(elevTilePath(p, sizeof(p), "/maps", 13, -1, 1) == 0 && p[0] == '\0', "negative x refused");
    CHECK(elevTilePath(p, sizeof(p), "/maps", 13, 1, -1) == 0 && p[0] == '\0', "negative y refused");
    CHECK(elevTilePath(p, sizeof(p), "/maps", 13, 8192, 1) == 0 && p[0] == '\0',
          "x past the world (2^13) refused");
    CHECK(elevTilePath(p, sizeof(p), "/maps", 13, 8191, 8191) > 0, "the last tile at z13 is fine");
    CHECK(elevTilePath(p, sizeof(p), "/maps", 0, 1, 0) == 0, "z0 has one tile");
    CHECK(elevTilePath(p, sizeof(p), "/maps", -1, 0, 0) == 0, "negative z refused");
    CHECK(elevTilePath(p, sizeof(p), "/maps", 20, 0, 0) == 0, "z past ELEV_ZOOM_MAX refused");
    CHECK(strcmp(ELEV_DIR, "elev") == 0 && ELEV_TILE_BYTES == ELEV_TILE_PX * ELEV_TILE_PX * 2,
          "constants agree with the file format");
  }

  // ── where: independent positions (Python, same formulas) ─────────────────────────────
  {
    struct { double lat, lon; int z, tx, ty; double px, py; } pts[] = {
      { 47.4957, -121.7868, 13, 1324, 2864, 172.46890666667605, 185.41718774940819 },  // North Bend
      { 47.4957, -121.7868, 10,  165,  358, 149.5586133333345,   23.177148468676023 },
      { -33.8568, 151.2153, 13, 7536, 4915, 252.96895999996923, 160.4803167085629 },  // Sydney
      { 64.8378, -147.7164, 13,  734, 2140, 161.6008533333661,  152.62323292391375 },  // Fairbanks
      { 0.0, 0.0, 13, 4096, 4096, 0.0, 0.0 },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
      int tx, ty;
      double px, py;
      elevLocate(pts[i].lat, pts[i].lon, pts[i].z, &tx, &ty, &px, &py);
      if (tx != pts[i].tx || ty != pts[i].ty || !near(px, pts[i].px, 1e-6) || !near(py, pts[i].py, 1e-6)) {
        printf("    %g,%g z%d -> %d/%d %.9f %.9f\n", pts[i].lat, pts[i].lon, pts[i].z, tx, ty, px, py);
        ok = false;
      }
    }
    CHECK(ok, "tile and pixel match Python for North Bend, Sydney, Fairbanks, 0/0");
  }

  // ── where: against mapLatLonToWorld over a grid, and the invariants ──────────────────
  {
    const double lats[] = { -89.0, -85.0511287798066, -85.0, -60.0, -33.9, -0.0001, 0.0, 0.0001,
                            12.345678, 47.4957, 60.0, 85.0, 85.0511287798066, 89.0 };
    // The two nextafter()s are the longitudes whose world x rounds to exactly the world width.
    const double lons[] = { -540.0, nextafter(-180.0, -1000.0), -180.0, -179.9999999, -121.7868,
                            -0.0000001, 0.0, 0.5, 45.0, 151.2153, 179.9999999,
                            nextafter(180.0, 0.0), 180.0, 181.0, 540.0 };
    const int zs[] = { 0, 1, 8, 10, 13, 19 };
    int bad = 0, cases = 0;
    for (size_t a = 0; a < sizeof(lats) / sizeof(lats[0]); a++) {
      for (size_t b = 0; b < sizeof(lons) / sizeof(lons[0]); b++) {
        for (size_t c = 0; c < sizeof(zs) / sizeof(zs[0]); c++) {
          const int z = zs[c];
          const int n = 1 << z;
          int tx = -9, ty = -9;
          double px = -9, py = -9;
          elevLocate(lats[a], lons[b], z, &tx, &ty, &px, &py);
          double wx, wy;
          mapLatLonToWorld(lats[a], lons[b], z, &wx, &wy);
          const double ws = 256.0 * n;
          if (wx >= ws) wx -= ws;
          cases++;
          bool ok = tx >= 0 && tx < n && ty >= 0 && ty < n &&
                    px >= 0.0 && px < 256.0 && py >= 0.0 && py < 256.0;
          ok = ok && tx == (int)floor(wx / 256.0) && px == wx - 256.0 * tx;   // bit-exact in x
          if (wy <= 0.0) {
            ok = ok && ty == 0 && py == 0.0;                   // the north limit: row 0, top edge
          } else if (wy >= ws) {
            ok = ok && ty == n - 1 && py > 255.999999;         // the south limit: held in the last row
          } else {
            ok = ok && ty == (int)floor(wy / 256.0) && py == wy - 256.0 * ty;
          }
          if (!ok) {
            bad++;
            printf("    %.10g,%.10g z%d -> %d/%d %.12f %.12f (world %.12f %.12f)\n",
                   lats[a], lons[b], z, tx, ty, px, py, wx, wy);
          }
        }
      }
    }
    printf("    %d positions against mapLatLonToWorld\n", cases);
    CHECK(bad == 0, "elevLocate lands on mapLatLonToWorld's pixel, bit for bit, everywhere");
  }
  {
    int tx, ty;
    double px, py;
    elevLocate(85.0511287798066, -179.296875, 0, &tx, &ty, &px, &py);
    CHECK(tx == 0 && ty == 0 && px == 0.5 && py == 0.0,
          "the north limit at z0 is row 0 (the raw formula gives wy = -2e-13, tile row -1)");
    elevLocate(-90.0, 0.0, 13, &tx, &ty, &px, &py);
    CHECK(ty == 8191 && py < 256.0 && py > 255.99, "the south limit is inside the last row");
    elevLocate(180.0, 10.0, 13, &tx, &ty, &px, &py);   // (lat, lon) = (180, 10)
    CHECK(ty == 0, "a latitude past 90 clamps to the north limit");
    elevLocate(10.0, 180.0, 13, &tx, &ty, &px, &py);
    CHECK(tx == 0 && px == 0.0, "180 E is column 0 (the same meridian as 180 W)");
    elevLocate(10.0, 179.99999999, 13, &tx, &ty, &px, &py);
    CHECK(tx == 8191 && px > 255.9, "just west of the antimeridian is the last column");
    elevLocate(NAN, NAN, 13, &tx, &ty, &px, &py);
    CHECK(tx == 4096 && ty == 4096 && px == 0.0 && py == 0.0, "NaN reads as 0,0 (like the map)");
    /* An infinite longitude: mapWrapLon gives NaN (fmod(inf, 360)), and a NaN reaching the tile
     * cast was UB here - tx INT_MIN, px NaN. It reads as 0 like NaN. */
    tx = ty = -9;
    px = py = -9;
    elevLocate(0.0, INFINITY, 13, &tx, &ty, &px, &py);
    bool infOk = tx == 4096 && ty == 4096 && px == 0.0 && py == 0.0;
    elevLocate(0.0, -INFINITY, 13, &tx, &ty, &px, &py);
    infOk = infOk && tx == 4096 && ty == 4096 && px == 0.0 && py == 0.0;
    CHECK(infOk, "an infinite longitude reads as 0 (a real tile and pixel, never INT_MIN / NaN)");
    elevLocate(INFINITY, 0.0, 13, &tx, &ty, &px, &py);
    infOk = tx == 4096 && ty == 0 && py == 0.0;
    elevLocate(-INFINITY, 0.0, 13, &tx, &ty, &px, &py);
    infOk = infOk && tx == 4096 && ty == 8191 && py > 255.99 && py < 256.0;
    CHECK(infOk, "an infinite latitude is the mercator limit (row 0 / the last row)");
    /* The two longitudes whose world x ROUNDS to exactly the world's width: 180 - 1 ulp
     * (lon + 180 = 360 - half an ulp of 360, rounded to even = 360) and -180 - 1 ulp (wrapped
     * to 360 - 180 = 180). Both are the antimeridian: column 0, px 0 - never column 2^z. */
    elevLocate(10.0, nextafter(180.0, 0.0), 13, &tx, &ty, &px, &py);
    bool amOk = tx == 0 && px == 0.0;
    elevLocate(10.0, nextafter(-180.0, -1000.0), 13, &tx, &ty, &px, &py);
    amOk = amOk && tx == 0 && px == 0.0;
    elevLocate(10.0, nextafter(180.0, 0.0), 0, &tx, &ty, &px, &py);
    amOk = amOk && tx == 0 && px == 0.0;
    CHECK(amOk, "a longitude whose world x rounds to the world's width is column 0, px 0");
    elevLocate(47.4957, -121.7868, 25, &tx, &ty, &px, &py);
    int tx19, ty19;
    elevLocate(47.4957, -121.7868, 19, &tx19, &ty19, NULL, NULL);
    CHECK(tx == tx19 && ty == ty19, "z past 19 clamps like the map's");
  }

  // ── what: synthetic tiles ────────────────────────────────────────────────────────────
  const int Z = ELEV_Z;
  const int AX = 1330, AY = 2862;       // tile A: linear, 3i + 7j - 1000
  const int BX = 1331;                  // tile B: 500, with hand-set blocks
  const int CX = 1332;                  // tile C: scratch, refilled per check
  MemTile* A = addTile(Z, AX, AY);
  for (int j = 0; j < 256; j++) {
    for (int i = 0; i < 256; i++) {
      setPx(A, i, j, (int16_t)(3 * i + 7 * j - 1000));
    }
  }
  MemTile* B = addTile(Z, BX, AY);
  fillConst(B, 500);
  // block 1 at cols 10-11, rows 20-21:  0 100 / 200 1000
  setPx(B, 10, 20, 0);    setPx(B, 11, 20, 100);
  setPx(B, 10, 21, 200);  setPx(B, 11, 21, 1000);
  // block 2 at cols 30-31, rows 40-41:  100 200 / NODATA 400
  setPx(B, 30, 40, 100);  setPx(B, 31, 40, 200);
  setPx(B, 30, 41, ELEV_NODATA);  setPx(B, 31, 41, 400);
  // block 3 at cols 50-51, rows 60-61: all NODATA
  setPx(B, 50, 60, ELEV_NODATA);  setPx(B, 51, 60, ELEV_NODATA);
  setPx(B, 50, 61, ELEV_NODATA);  setPx(B, 51, 61, ELEV_NODATA);
  MemTile* C = addTile(Z, CX, AY);

  Store s;
  resetStore(&s);
  double m = 0;
  const double TOL = 1e-5;             // the lat/lon round trip is < 3e-10 px (measured); <= 800 m/px here

  // pixel centres
  CHECK(sampleAt(&s, Z, AX, AY, 100.5, 50.5, &m) == 1 && near(m, linA(100, 50), TOL),
        "a pixel centre reads that pixel (tile A 100,50 = -350)");
  CHECK(sampleAt(&s, Z, BX, AY, 10.5, 20.5, &m) == 1 && near(m, 0.0, TOL), "B centre (10,20) = 0");
  CHECK(sampleAt(&s, Z, BX, AY, 11.5, 21.5, &m) == 1 && near(m, 1000.0, TOL), "B centre (11,21) = 1000");
  // bilinear
  CHECK(sampleAt(&s, Z, BX, AY, 11.0, 20.5, &m) == 1 && near(m, 50.0, TOL),
        "midway between two columns = their mean (0,100 -> 50)");
  CHECK(sampleAt(&s, Z, BX, AY, 10.5, 21.0, &m) == 1 && near(m, 100.0, TOL),
        "midway between two rows = their mean (0,200 -> 100)");
  CHECK(sampleAt(&s, Z, BX, AY, 11.0, 21.0, &m) == 1 && near(m, 325.0, TOL),
        "the middle of four = their mean (0,100,200,1000 -> 325)");
  CHECK(sampleAt(&s, Z, BX, AY, 10.75, 20.75, &m) == 1 && near(m, 118.75, TOL),
        "a quarter of the way: weights 9/16 3/16 3/16 1/16 -> 118.75");
  CHECK(s.reads == 2, "an interior point is two 4-byte reads");
  CHECK(sampleAt(&s, Z, AX, AY, 123.37, 45.81, &m) == 1 && near(m, linA(122.87, 45.31), TOL),
        "a linear field is reproduced exactly anywhere inside (bilinear)");
  CHECK(sampleAt(&s, Z, AX, AY, 255.2, 100.5, &m) == 1 && near(m, linA(254.7, 100), TOL),
        "between the last two centres (254.5..255.5) is still interpolated");

  // edges: the outer half-pixel is the edge pixel; the neighbour tile is never read
  CHECK(sampleAt(&s, Z, AX, AY, 255.8, 10.5, &m) == 1 && near(m, linA(255, 10), TOL) && s.foreign == 0,
        "px 255.8 is column 255, and tile x+1 is not opened");
  CHECK(sampleAt(&s, Z, AX, AY, 255.9999, 100.25, &m) == 1 && near(m, linA(255, 99.75), TOL) &&
        s.foreign == 0, "px 255.9999 is column 255 (rows still interpolated)");
  CHECK(sampleAt(&s, Z, AX, AY, 100.5, 255.8, &m) == 1 && near(m, linA(100, 255), TOL) &&
        s.reads == 1 && s.foreign == 0, "py 255.8 is row 255: one read, tile y+1 not opened");
  CHECK(sampleAt(&s, Z, AX, AY, 255.9, 255.9, &m) == 1 && near(m, linA(255, 255), TOL) &&
        s.reads == 1 && s.foreign == 0 && !s.badRequest,
        "the last corner: pixel 255,255 = 1550, read at byte 131068 (inside the file)");
  CHECK(sampleAt(&s, Z, AX, AY, 0.2, 100.5, &m) == 1 && near(m, linA(0, 100), TOL) && s.foreign == 0,
        "px 0.2 is column 0, and tile x-1 is not opened");
  CHECK(sampleAt(&s, Z, AX, AY, 100.5, 0.2, &m) == 1 && near(m, linA(100, 0), TOL) &&
        s.reads == 1 && s.foreign == 0, "py 0.2 is row 0: one read, tile y-1 not opened");
  CHECK(sampleAt(&s, Z, AX, AY, 0.1, 0.1, &m) == 1 && near(m, linA(0, 0), TOL), "the first corner");

  // no data
  CHECK(sampleAt(&s, Z, BX, AY, 30.75, 41.0, &m) == 1 && near(m, 180.0, TOL),
        "a no-data neighbour is dropped and the rest renormalised (100 200 / -- 400 -> 180)");
  m = 12345.0;
  CHECK(sampleAt(&s, Z, BX, AY, 51.0, 61.0, &m) == -1 && m == 12345.0,
        "all four neighbours no-data: -1, and *metres untouched");
  CHECK(sampleAt(&s, Z, BX, AY, 50.8, 60.8, &m) == -1, "anywhere between four void centres: -1");
  CHECK(sampleAt(&s, Z, BX, AY, 51.9, 61.4, &m) == 1 && near(m, 500.0, TOL),
        "beside the void, the valid neighbours carry it (500)");
  {
    /* Exactly on a column of void centres, with valid pixels beside it carrying weight 0: the
     * bilinear surface there is made of no-data alone. z0, where lon -179.296875 is px 0.5 to
     * the last bit, and the north limit is py 0 (row 0 used for both rows). */
    MemTile* W = addTile(0, 0, 0);
    fillConst(W, 250);
    setPx(W, 0, 0, ELEV_NODATA);
    Store w;
    resetStore(&w);
    CHECK(elevSampleZ(memRead, &w, 0, 89.0, -179.296875, &m) == -1,
          "a point on a void centre whose valid neighbours weigh 0: -1, not a division by zero");
    CHECK(elevSampleZ(memRead, &w, 0, 89.0, -179.0, &m) == 1 && near(m, 250.0, TOL),
          "a little east of it, the valid neighbour carries the value");
    W->used = false;
  }

  // byte order
  fillConst(C, -1234);
  CHECK(sampleAt(&s, Z, CX, AY, 77.3, 88.9, &m) == 1 && m == -1234.0,
        "a negative height is little-endian two's complement (-1234 = 2E FB)");
  fillConst(C, -1);
  CHECK(sampleAt(&s, Z, CX, AY, 77.3, 88.9, &m) == 1 && m == -1.0, "-1 (FF FF)");
  fillConst(C, 32767);
  CHECK(sampleAt(&s, Z, CX, AY, 77.3, 88.9, &m) == 1 && m == 32767.0, "32767 (FF 7F)");
  fillConst(C, -32767);
  CHECK(sampleAt(&s, Z, CX, AY, 77.3, 88.9, &m) == 1 && m == -32767.0, "-32767 (01 80) is a height");
  fillConst(C, 256);
  CHECK(sampleAt(&s, Z, CX, AY, 77.3, 88.9, &m) == 1 && m == 256.0, "256 (00 01): high byte second");
  CHECK(!s.badRequest, "every read was 4 bytes, even, and inside the file");

  // failures of the reader
  {
    m = 999.0;
    CHECK(sampleAt(&s, Z, 1340, AY, 100.5, 100.5, &m) == 0 && m == 999.0, "no tile: 0, *metres untouched");
    s.failAt = 2;
    CHECK(sampleAt(&s, Z, AX, AY, 100.5, 100.5, &m) == 0, "the second row's read fails: 0");
    s.failAt = 0;
    s.shortRead = true;
    CHECK(sampleAt(&s, Z, AX, AY, 100.5, 100.5, &m) == 0, "a short read: 0");
    s.shortRead = false;
    CHECK(elevSampleZ(NULL, &s, Z, 47.5, -121.5, &m) == 0, "no reader: 0");
    /* The guards must refuse BEFORE the card is touched: without them NaN/inf read as 0,0 (a
     * real tile at 4096/4096) and z 20 / -1 would be clamped by elevLocate and read anyway -
     * a "no tile" answer alone cannot tell the guard from an empty store. */
    s.reads = 0;
    m = 999.0;
    CHECK(elevSampleZ(memRead, &s, Z, NAN, -121.5, &m) == 0, "NaN latitude: 0");
    CHECK(elevSampleZ(memRead, &s, Z, 47.5, NAN, &m) == 0, "NaN longitude: 0");
    CHECK(elevSampleZ(memRead, &s, Z, 47.5, INFINITY, &m) == 0, "infinite longitude: 0");
    CHECK(elevSampleZ(memRead, &s, Z, 47.5, -INFINITY, &m) == 0, "-infinite longitude: 0");
    CHECK(elevSampleZ(memRead, &s, Z, INFINITY, -121.5, &m) == 0, "infinite latitude: 0");
    CHECK(elevSampleZ(memRead, &s, -1, 47.5, -121.5, &m) == 0, "negative z: 0");
    CHECK(elevSampleZ(memRead, &s, 20, 47.5, -121.5, &m) == 0, "z past ELEV_ZOOM_MAX: 0");
    CHECK(s.reads == 0 && m == 999.0, "none of those reached the reader, and *metres is untouched");
    double lat, lon;
    tilePoint(Z, AX, AY, 100.5, 100.5, &lat, &lon);
    CHECK(elevSampleZ(memRead, &s, Z, lat, lon, NULL) == 1, "metres may be NULL");
  }

  // the antimeridian
  {
    double lat, lon;
    tilePoint(Z, 0, AY, 10.0, 128.0, &lat, &lon);       // a latitude inside row AY
    int tx, ty;
    elevLocate(lat, 180.0, Z, &tx, &ty, NULL, NULL);
    MemTile* E = addTile(Z, 0, ty);
    fillConst(E, 111);
    MemTile* Wt = addTile(Z, 8191, ty);
    fillConst(Wt, 222);
    resetStore(&s);
    CHECK(elevSampleZ(memRead, &s, Z, lat, 180.0, &m) == 1 && m == 111.0, "180 E reads column 0's tile");
    CHECK(elevSampleZ(memRead, &s, Z, lat, -180.0, &m) == 1 && m == 111.0, "180 W reads the same tile");
    CHECK(elevSampleZ(memRead, &s, Z, lat, 540.0, &m) == 1 && m == 111.0, "540 wraps to 180");
    CHECK(elevSampleZ(memRead, &s, Z, lat, 179.9999999, &m) == 1 && m == 222.0,
          "a hair west of 180 reads the last column's tile");
    CHECK(elevSampleZ(memRead, &s, Z, lat, -179.9999999, &m) == 1 && m == 111.0,
          "a hair east of 180 W reads the first");
    E->used = false;
    Wt->used = false;
  }

  // ── the two layers ───────────────────────────────────────────────────────────────────
  {
    // z10 parent of the z13 tiles 1328..1335 in row 2862: (166, 357)
    MemTile* P = addTile(ELEV_Z_COARSE, AX >> 3, AY >> 3);
    fillConst(P, 777);
    resetStore(&s);
    double lat, lon;
    int zu = 99;
    tilePoint(Z, AX, AY, 100.5, 50.5, &lat, &lon);
    CHECK(elevSample(memRead, &s, lat, lon, &m, &zu) == 1 && near(m, linA(100, 50), TOL) && zu == 13,
          "z13 first when it has the point");
    tilePoint(Z, 1335, AY, 100.5, 50.5, &lat, &lon);
    CHECK(elevSample(memRead, &s, lat, lon, &m, &zu) == 1 && m == 777.0 && zu == 10,
          "no z13 tile: the z10 layer, and it says so");
    tilePoint(Z, BX, AY, 51.0, 61.0, &lat, &lon);
    CHECK(elevSample(memRead, &s, lat, lon, &m, &zu) == 1 && m == 777.0 && zu == 10,
          "z13 has a void there: the z10 layer");
    CHECK(elevSample(memRead, &s, lat, lon, &m) == 1 && m == 777.0, "zUsed may be left out");
    P->used = false;
    zu = 99;
    m = 5.0;
    CHECK(elevSample(memRead, &s, lat, lon, &m, &zu) == -1 && zu == -1 && m == 5.0,
          "z13 void, no z10: -1 (a tile was there), zUsed -1");
    tilePoint(Z, 1335, AY, 100.5, 50.5, &lat, &lon);
    CHECK(elevSample(memRead, &s, lat, lon, &m, &zu) == 0 && zu == -1, "neither layer: 0");
    CHECK(!s.badRequest, "every read was 4 bytes, even, and inside the file");
  }

  // ── terrarium ────────────────────────────────────────────────────────────────────────
  CHECK(elevFromTerrarium(128, 0, 0) == 0, "(128,0,0) is sea level");
  /* (127,255,128) is exactly -0.5 m. It rounds HALF AWAY FROM ZERO to -1, because that is
   * what tools/make_elev_tiles.py does (np.where(m >= 0, floor(m + .5), -floor(-m + .5))):
   * a tile the phone downloads and decodes must be the same bytes as one built on the Mac.
   * (Python's round() and numpy's would give -0 -> 0: banker's rounding.) */
  CHECK(elevFromTerrarium(127, 255, 128) == -1, "-0.5 m rounds half away from zero: -1");
  CHECK(elevFromTerrarium(128, 0, 128) == 1, "+0.5 m rounds to +1");
  CHECK(elevFromTerrarium(127, 255, 129) == 0, "-0.496 m is 0");
  CHECK(elevFromTerrarium(127, 255, 127) == -1, "-0.504 m is -1");
  CHECK(elevFromTerrarium(128, 0, 127) == 0, "+0.496 m is 0");
  CHECK(elevFromTerrarium(0, 0, 0) == -32767, "(0,0,0) = -32768 m is held off NODATA: -32767");
  CHECK(elevFromTerrarium(0, 0, 1) == -32767, "-32767.996 rounds to -32768 -> -32767");
  CHECK(elevFromTerrarium(0, 0, 200) == -32767, "-32767.22 is -32767");
  CHECK(elevFromTerrarium(255, 255, 255) == 32767, "the top (32767.996) clamps to 32767");
  CHECK(elevFromTerrarium(255, 255, 127) == 32767, "32767.496 is 32767");
  CHECK(elevFromTerrarium(0x84, 0x3A, 0) == 1082, "(132,58,0) = 1082 m");
  {
    // Exhaustively: all 2^24 pixels against the float formula make_elev_tiles.py uses.
    long bad = 0;
    for (int r = 0; r < 256; r++) {
      for (int g = 0; g < 256; g++) {
        for (int b = 0; b < 256; b++) {
          const double mm = r * 256.0 + g + b / 256.0 - 32768.0;
          double want = mm >= 0 ? floor(mm + 0.5) : -floor(-mm + 0.5);
          if (want < -32767) want = -32767;
          if (want > 32767) want = 32767;
          if (elevFromTerrarium((uint8_t)r, (uint8_t)g, (uint8_t)b) != (int)want) {
            bad++;
          }
        }
      }
    }
    CHECK(bad == 0, "all 16,777,216 pixels equal make_elev_tiles.py's float formula");
  }
  {
    /* Real pixels of the AWS terrarium tile z13/1330/2862 (the one saved as terr_https.png
     * on 2026-09-27), decoded in Python from the PNG and matching ~/elev-master's .elv of it.
     * row, col: (r, g, b) -> metres (exact metres before rounding in the comment). */
    struct { int r, g, b, want; } px[] = {
      { 129,  50, 195,  307 },   // row 255 col 62:  306.76171875 (the tile's lowest)
      { 133, 220,  67, 1500 },   // row 188 col 244: 1500.26171875 (the tile's highest)
      { 132, 173, 128, 1198 },   // row 0 col 55:    1197.5 exactly - a real tie, rounded up
      { 131, 244, 127, 1012 },   // row 0 col 23:    1012.49609375
      { 130, 200, 129,  713 },   // row 1 col 135:   712.50390625
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(px) / sizeof(px[0]); i++) {
      const int got = elevFromTerrarium((uint8_t)px[i].r, (uint8_t)px[i].g, (uint8_t)px[i].b);
      if (got != px[i].want) {
        printf("    (%d,%d,%d) -> %d, want %d\n", px[i].r, px[i].g, px[i].b, got, px[i].want);
        ok = false;
      }
    }
    CHECK(ok, "five real pixels of terrarium z13/1330/2862 decode to Python's metres");
  }

  if (failures) {
    printf("test_elev: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("test_elev: %d passed, 0 failed\n", checks);
  return 0;
}
