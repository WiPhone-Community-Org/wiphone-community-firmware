/*
 * elev_tiles.cpp - see elev_tiles.h. Proven by tests/test_elev.cpp on the host.
 */

#include "elev_tiles.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// The latitude Web Mercator stops at: atan(sinh(pi)) in degrees (= MAP_LAT_LIMIT).
#define ELEV_LAT_LIMIT 85.0511287798066

static bool isFiniteD(double v) {
  return v >= -DBL_MAX && v <= DBL_MAX;          // false for NaN and both infinities
}

static int clampZoom(int z) {
  if (z < 0) {
    return 0;
  }
  if (z > ELEV_ZOOM_MAX) {
    return ELEV_ZOOM_MAX;
  }
  return z;
}

// mapClampLat(): NaN in is 0 out.
static double clampLat(double lat) {
  if (!(lat == lat)) {
    return 0.0;
  }
  if (lat > ELEV_LAT_LIMIT) {
    return ELEV_LAT_LIMIT;
  }
  if (lat < -ELEV_LAT_LIMIT) {
    return -ELEV_LAT_LIMIT;
  }
  return lat;
}

/* mapWrapLon(): into [-180, 180), NaN in is 0 out - and so is +-inf, where mapWrapLon gives NaN
 * (fmod(inf, 360)). Here that NaN would reach the (int32_t) casts below, which is undefined
 * behaviour: tx = INT_MIN and px = NaN out of elevLocate, against its own contract. */
static double wrapLon(double lon) {
  if (!isFiniteD(lon)) {
    return 0.0;
  }
  double v = fmod(lon + 180.0, 360.0);
  if (v < 0.0) {
    v += 360.0;
  }
  return v - 180.0;
}

/* Two bytes, low first, as a signed 16-bit value. Spelled out rather than cast through an
 * int16_t pointer: the buffer need not be aligned, and the byte order must not depend on the
 * machine (the ESP32 and the Mac are both little-endian today; the file format is what is
 * fixed, not the CPU). */
static int le16(const uint8_t* p) {
  const int v = (int)p[0] | ((int)p[1] << 8);
  return v >= 32768 ? v - 65536 : v;
}

int elevTilePath(char* out, size_t cap, const char* mapsRoot, int z, int x, int y) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (!mapsRoot || z < 0 || z > ELEV_ZOOM_MAX) {
    return 0;
  }
  const int32_t n = (int32_t)1 << z;
  if (x < 0 || y < 0 || x >= n || y >= n) {
    return 0;
  }
  const int len = snprintf(out, cap, "%s/%s/%d/%d/%d.elv", mapsRoot, ELEV_DIR, z, x, y);
  if (len < 0 || (size_t)len >= cap) {
    out[0] = '\0';
    return 0;
  }
  return len;
}

void elevLocate(double lat, double lon, int z, int* tx, int* ty, double* px, double* py) {
  z = clampZoom(z);
  const int32_t n = (int32_t)1 << z;
  const double ws = (double)ELEV_TILE_PX * (double)n;
  lat = clampLat(lat);
  lon = wrapLon(lon);

  // mapLatLonToWorld(), term for term, so the elevation lands on the map's pixel.
  double wx = (lon + 180.0) / 360.0 * ws;
  const double s = sin(lat * M_PI / 180.0);
  double wy = (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * ws;

  /* A longitude a hair under 180 can round to exactly ws: that is the antimeridian, column 0.
   * At the mercator limits wy lands on (or a rounding past) the world's top or bottom edge;
   * the bottom one would be tile row n, which does not exist, so it is held inside the last
   * row. */
  if (wx >= ws) {
    wx -= ws;
  }
  if (wx < 0.0) {
    wx = 0.0;
  }
  if (wy < 0.0) {
    wy = 0.0;
  }

  int32_t cx = (int32_t)floor(wx / ELEV_TILE_PX);
  int32_t cy = (int32_t)floor(wy / ELEV_TILE_PX);
  if (cx > n - 1) {
    cx = n - 1;
  }
  // Both are exact: the tile origin is a multiple of 256 below wx (see test_elev).
  double fx = wx - (double)cx * ELEV_TILE_PX;
  double fy;
  if (cy > n - 1) {
    cy = n - 1;
    fy = ELEV_TILE_PX * (1.0 - 1e-12);
  } else {
    fy = wy - (double)cy * ELEV_TILE_PX;
  }
  if (fx >= ELEV_TILE_PX) {
    fx = ELEV_TILE_PX * (1.0 - 1e-12);
  }
  if (fy >= ELEV_TILE_PX) {
    fy = ELEV_TILE_PX * (1.0 - 1e-12);
  }
  if (tx) {
    *tx = (int)cx;
  }
  if (ty) {
    *ty = (int)cy;
  }
  if (px) {
    *px = fx;
  }
  if (py) {
    *py = fy;
  }
}

/* The two pixels a coordinate falls between, and the weight of the second. `p` is in
 * [0, 256). The centres are at i + 0.5, so the pair is floor(p - 0.5) and the next one; in
 * the outer half-pixel (p < 0.5 or p >= 255.5) both are the edge pixel, and the weight no
 * longer matters because the two values are the same. */
static void elevPair(double p, int* i0, int* i1, double* f) {
  const double u = p - 0.5;
  int a = (int)floor(u);
  *f = u - (double)a;
  int b = a + 1;
  if (a < 0) {
    a = 0;
  }
  if (b < 0) {
    b = 0;
  }
  if (a > ELEV_TILE_PX - 1) {
    a = ELEV_TILE_PX - 1;
  }
  if (b > ELEV_TILE_PX - 1) {
    b = ELEV_TILE_PX - 1;
  }
  *i0 = a;
  *i1 = b;
}

int elevSampleZ(ElevReadFn rd, void* ctx, int z, double lat, double lon, double* metres) {
  if (!rd || z < 0 || z > ELEV_ZOOM_MAX || !isFiniteD(lat) || !isFiniteD(lon)) {
    return 0;
  }
  int tx = 0, ty = 0;
  double px = 0, py = 0;
  elevLocate(lat, lon, z, &tx, &ty, &px, &py);

  int i0, i1, j0, j1;
  double fx, fy;
  elevPair(px, &i0, &i1, &fx);
  elevPair(py, &j0, &j1, &fy);

  /* One 4-byte run per row: columns cs and cs + 1, which hold both i0 and i1 (at the last
   * column both are 255, so the run starts one to the left rather than off the row's end). */
  const int cs = i0 < ELEV_TILE_PX - 1 ? i0 : ELEV_TILE_PX - 2;
  uint8_t r0[4], r1[4];
  const uint32_t off0 = ((uint32_t)j0 * ELEV_TILE_PX + (uint32_t)cs) * 2u;
  if (rd(ctx, z, tx, ty, off0, r0, 4) != 4) {
    return 0;
  }
  if (j1 != j0) {
    const uint32_t off1 = ((uint32_t)j1 * ELEV_TILE_PX + (uint32_t)cs) * 2u;
    if (rd(ctx, z, tx, ty, off1, r1, 4) != 4) {
      return 0;
    }
  } else {
    memcpy(r1, r0, sizeof(r1));
  }

  const int v[4] = {
    le16(r0 + 2 * (i0 - cs)), le16(r0 + 2 * (i1 - cs)),
    le16(r1 + 2 * (i0 - cs)), le16(r1 + 2 * (i1 - cs)),
  };
  const double w[4] = {
    (1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),
    (1.0 - fx) * fy,         fx * fy,
  };
  double acc = 0.0, wsum = 0.0;
  for (int k = 0; k < 4; k++) {
    if (v[k] == ELEV_NODATA) {
      continue;
    }
    acc += w[k] * (double)v[k];
    wsum += w[k];
  }
  /* wsum is 0 with no valid neighbour at all, and also when the valid ones all carry weight 0
   * - the point is exactly on a line of no-data pixel centres, where the bilinear surface is
   * made of no-data alone. Both are "no data here", never a division by zero. */
  if (!(wsum > 0.0)) {
    return -1;
  }
  if (metres) {
    *metres = acc / wsum;
  }
  return 1;
}

int elevSample(ElevReadFn rd, void* ctx, double lat, double lon, double* metres, int* zUsed) {
  static const int kLayers[2] = { ELEV_Z, ELEV_Z_COARSE };
  int result = 0;
  if (zUsed) {
    *zUsed = -1;
  }
  for (int k = 0; k < 2; k++) {
    const int r = elevSampleZ(rd, ctx, kLayers[k], lat, lon, metres);
    if (r == 1) {
      if (zUsed) {
        *zUsed = kLayers[k];
      }
      return 1;
    }
    if (r < 0) {
      result = -1;                 // a tile was there; keep looking, but say so if nothing is
    }
  }
  return result;
}

int16_t elevFromTerrarium(uint8_t r, uint8_t g, uint8_t b) {
  // In 1/256 m, exactly: r*65536 + g*256 + b - 32768*256, within int32 for every pixel.
  const int32_t v = (int32_t)r * 65536 + (int32_t)g * 256 + (int32_t)b - 32768 * 256;
  // Half away from zero: -0.5 m is -1, +0.5 m is +1 (tools/make_elev_tiles.py's np.where).
  int32_t m = v >= 0 ? (v + 128) / 256 : -((-v + 128) / 256);
  if (m > 32767) {
    m = 32767;
  }
  if (m < -32767) {
    m = -32767;                    // -32768 is ELEV_NODATA, never a height
  }
  return (int16_t)m;
}
