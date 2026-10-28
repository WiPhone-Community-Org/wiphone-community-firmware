/*
 * map_tiles.cpp — see map_tiles.h. Proven by tests/test_maptiles.cpp on the host.
 */

#include "map_tiles.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Circumference of the WGS84 ellipsoid's equator / 256, i.e. metres per pixel at zoom 0.
#define MAP_M_PER_PX_Z0   156543.03392804097

// ---------------------------------------------------------------- world geometry

static int clampZoom(int z) {
  if (z < MAP_ZOOM_MIN) {
    return MAP_ZOOM_MIN;
  }
  if (z > MAP_ZOOM_MAX) {
    return MAP_ZOOM_MAX;
  }
  return z;
}

int32_t mapWorldPx(int z) {
  return (int32_t)MAP_TILE_PX << clampZoom(z);
}

double mapClampLat(double lat) {
  if (!(lat == lat)) {
    return 0.0;                      // NaN in is 0 out; every caller wants a number
  }
  if (lat >  MAP_LAT_LIMIT) {
    return  MAP_LAT_LIMIT;
  }
  if (lat < -MAP_LAT_LIMIT) {
    return -MAP_LAT_LIMIT;
  }
  return lat;
}

double mapWrapLon(double lon) {
  if (!(lon == lon)) {
    return 0.0;
  }
  /* fmod alone leaves -180 as -180 and +180 as +180; the half-open interval matters because
   * +180 and -180 are the SAME meridian and a pin stored at one must project identically to
   * one stored at the other. */
  double v = fmod(lon + 180.0, 360.0);
  if (v < 0.0) {
    v += 360.0;
  }
  return v - 180.0;
}

double mapI7ToDeg(int32_t i7) {
  return (double)i7 * 1e-7;
}

int32_t mapDegToI7(double deg) {
  /* ⚠ Round, do not truncate. (int32_t)(-1.23456785e0 * 1e7) truncates TOWARDS ZERO, so a
   * southern latitude and a northern one of the same magnitude would round in opposite
   * directions and a pin would move by 1e-7 deg (1.1 cm) each time it was saved and reloaded.
   * Cosmetic in isolation; not cosmetic when the value is also what goes on the air. */
  double v = deg * 1e7;
  if (v >= 0) {
    v += 0.5;
  } else {
    v -= 0.5;
  }
  if (v >  2147483647.0) {
    return  2147483647;
  }
  if (v < -2147483648.0) {
    return -2147483647 - 1;
  }
  return (int32_t)v;
}

void mapLatLonToWorld(double lat, double lon, int z, double* wx, double* wy) {
  const double ws = (double)mapWorldPx(z);
  lat = mapClampLat(lat);
  lon = mapWrapLon(lon);
  if (wx) {
    *wx = (lon + 180.0) / 360.0 * ws;
  }
  if (wy) {
    const double s = sin(lat * M_PI / 180.0);
    /* The standard Mercator y. log((1+s)/(1-s))/2 is atanh(s), which is the same thing as
     * log(tan(lat) + sec(lat)) and better behaved near the limit latitude. */
    *wy = (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * ws;
  }
}

void mapWorldToLatLon(double wx, double wy, int z, double* lat, double* lon) {
  const double ws = (double)mapWorldPx(z);
  if (lon) {
    double x = fmod(wx, ws);
    if (x < 0) {
      x += ws;
    }
    *lon = x / ws * 360.0 - 180.0;
    if (*lon >= 180.0) {
      *lon -= 360.0;
    }
  }
  if (lat) {
    double y = wy;
    if (y < 0) {
      y = 0;
    }
    if (y > ws) {
      y = ws;
    }
    const double n = M_PI - 2.0 * M_PI * y / ws;
    *lat = 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
  }
}

double mapMetersPerPixel(double lat, int z) {
  return MAP_M_PER_PX_Z0 * cos(mapClampLat(lat) * M_PI / 180.0) / (double)(1u << clampZoom(z));
}

// ---------------------------------------------------------------- the view

void mapClampView(int z, int vw, int vh, int32_t* cx, int32_t* cy) {
  const int32_t ws = mapWorldPx(z);
  if (cx) {
    int64_t x = (int64_t)*cx % ws;
    if (x < 0) {
      x += ws;
    }
    *cx = (int32_t)x;
  }
  if (cy) {
    const int half = vh / 2;
    if (ws <= vh) {
      *cy = ws / 2;                 // the world is shorter than the screen: park it centred
    } else {
      int32_t lo = half;
      int32_t hi = ws - vh + half;
      if (*cy < lo) {
        *cy = lo;
      }
      if (*cy > hi) {
        *cy = hi;
      }
    }
  }
}

int mapPanView(int z, int vw, int vh, int dx, int dy, int32_t* cx, int32_t* cy) {
  if (!cx || !cy) {
    return 0;
  }
  const int32_t wasX = *cx, wasY = *cy;
  /* int64 on the way in: cx can be 134 million at z19 and a big pan must not wrap the int32
   * before mapClampView gets the chance to fold it properly. */
  int64_t nx = (int64_t)*cx + dx;
  int64_t ny = (int64_t)*cy + dy;
  const int32_t ws = mapWorldPx(z);
  nx %= ws;
  if (nx < 0) {
    nx += ws;
  }
  if (ny < -ws) {
    ny = -ws;
  }
  if (ny > 2 * (int64_t)ws) {
    ny = 2 * (int64_t)ws;
  }
  *cx = (int32_t)nx;
  *cy = (int32_t)ny;
  mapClampView(z, vw, vh, cx, cy);
  return (*cx != wasX) || (*cy != wasY);
}

int mapZoomView(int zMin, int zMax, int z, int delta, int vw, int vh,
                int32_t* cx, int32_t* cy) {
  if (zMin < MAP_ZOOM_MIN) {
    zMin = MAP_ZOOM_MIN;
  }
  if (zMax > MAP_ZOOM_MAX) {
    zMax = MAP_ZOOM_MAX;
  }
  if (zMax < zMin) {
    zMax = zMin;
  }
  int nz = z + delta;
  if (nz < zMin) {
    nz = zMin;
  }
  if (nz > zMax) {
    nz = zMax;
  }
  if (nz == z || !cx || !cy) {
    return nz;
  }
  if (nz > z) {
    const int d = nz - z;
    *cx = (int32_t)(((int64_t)*cx) << d);
    *cy = (int32_t)(((int64_t)*cy) << d);
  } else {
    const int d = z - nz;
    const int32_t half = (int32_t)1 << (d - 1);
    /* Round rather than truncate so zoom-out then zoom-in lands back where it started
     * instead of creeping north-west one pixel per round trip. */
    *cx = (int32_t)(((int64_t)*cx + half) >> d);
    *cy = (int32_t)(((int64_t)*cy + half) >> d);
  }
  mapClampView(nz, vw, vh, cx, cy);
  return nz;
}

int mapViewBlits(int z, int32_t cx, int32_t cy, int vw, int vh, MapBlit* out, int cap) {
  if (!out || cap <= 0 || vw <= 0 || vh <= 0) {
    return -1;
  }
  const int32_t ws = mapWorldPx(z);
  const int64_t left = (int64_t)cx - vw / 2;
  const int64_t top  = (int64_t)cy - vh / 2;

  /* The vertical span the world actually covers. When the world is shorter than the screen
   * (low zoom) this is narrower than the viewport and the caller letterboxes the rest — the
   * alternative, repeating the world vertically, would draw two north poles. */
  int64_t y0 = top  < 0 ? 0 : top;
  int64_t y1 = (top + vh) > ws ? ws : (top + vh);

  int n = 0;
  for (int64_t wy = y0; wy < y1; ) {
    const int ty = (int)(wy / MAP_TILE_PX);
    const int sy = (int)(wy - (int64_t)ty * MAP_TILE_PX);
    int hh = MAP_TILE_PX - sy;
    if (wy + hh > y1) {
      hh = (int)(y1 - wy);
    }
    for (int64_t wx = left; wx < left + vw; ) {
      int64_t mx = wx % ws;
      if (mx < 0) {
        mx += ws;
      }
      const int tx = (int)(mx / MAP_TILE_PX);
      const int sx = (int)(mx - (int64_t)tx * MAP_TILE_PX);
      int wwid = MAP_TILE_PX - sx;
      if (wx + wwid > left + vw) {
        wwid = (int)(left + vw - wx);
      }
      if (wwid <= 0 || hh <= 0) {
        return -1;                  // cannot happen; a zero-width blit would loop forever
      }
      if (n >= cap) {
        return -1;                  // truncation is never silently acceptable: see the header
      }
      out[n].tileX = tx;
      out[n].tileY = ty;
      out[n].srcX  = sx;
      out[n].srcY  = sy;
      out[n].dstX  = (int)(wx - left);
      out[n].dstY  = (int)(wy - top);
      out[n].w     = wwid;
      out[n].h     = hh;
      n++;
      wx += wwid;
    }
    wy += hh;
  }
  return n;
}

// ---------------------------------------------------------------- the stretch

int mapAncestorSrc(const MapBlit* b, int d, MapAncestorBlit* out) {
  if (!b || !out || d < 0 || d > MAP_ANCESTOR_MAX_D) {
    return 0;
  }
  if (b->tileX < 0 || b->tileY < 0 || b->srcX < 0 || b->srcY < 0 || b->w <= 0 || b->h <= 0 ||
      b->srcX + b->w > MAP_TILE_PX || b->srcY + b->h > MAP_TILE_PX) {
    return 0;
  }
  const int sub = MAP_TILE_PX >> d;      // the position's side in ancestor pixels: >= 1 because d <= 8
  const int mask = (1 << d) - 1;
  out->ax = b->tileX >> d;
  out->ay = b->tileY >> d;
  // Where the position sits inside its ancestor, plus how far into the position the piece starts.
  out->sx0 = (b->tileX & mask) * sub + (b->srcX >> d);
  out->sy0 = (b->tileY & mask) * sub + (b->srcY >> d);
  // From the ancestor pixel holding the first screen pixel to the one holding the last.
  out->sw = ((b->srcX + b->w - 1) >> d) - (b->srcX >> d) + 1;
  out->sh = ((b->srcY + b->h - 1) >> d) - (b->srcY >> d) + 1;
  out->scale = 1 << d;
  out->phx = b->srcX & mask;
  out->phy = b->srcY & mask;
  out->w = b->w;
  out->h = b->h;
  return 1;
}

int mapAncestorRow(const MapAncestorBlit* a, const uint16_t* tile, int r,
                   uint16_t* out, int cap, int* y0) {
  if (y0) {
    *y0 = 0;
  }
  if (!a || !tile || !out || cap <= 0 || r < 0 || r >= a->sh || a->scale < 1) {
    return 0;
  }
  /* The screen rows this ancestor row covers. The first is cut short by the phase: `phy` of
   * its rows lie above the piece and belong to the piece above (or to nothing on screen). */
  const int top = (r == 0) ? 0 : (a->scale - a->phy) + (r - 1) * a->scale;
  int rows = (r == 0) ? (a->scale - a->phy) : a->scale;
  if (top >= a->h) {
    return 0;                          // past the piece: not reachable for r < sh
  }
  if (top + rows > a->h) {
    rows = a->h - top;
  }
  const uint16_t* src = tile + (size_t)(a->sy0 + r) * MAP_TILE_PX + a->sx0;
  const int w = a->w < cap ? a->w : cap;
  int x = 0;
  for (int c = 0; c < a->sw && x < w; c++) {
    int run = (c == 0) ? (a->scale - a->phx) : a->scale;   // the first column: the same cut
    if (x + run > w) {
      run = w - x;
    }
    const uint16_t p = src[c];
    for (int k = 0; k < run; k++) {
      out[x++] = p;
    }
  }
  if (y0) {
    *y0 = top;
  }
  return rows;
}

int mapStretchLabel(char* out, size_t cap, int zoom, int minL, int maxL, int compact) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (minL < 0 || minL > maxL || maxL >= zoom || zoom - minL > MAP_ANCESTOR_MAX_D) {
    return 0;
  }
  const int fDeep = 1 << (zoom - maxL);       // the deepest level is stretched the least
  const int fShallow = 1 << (zoom - minL);
  int n;
  if (minL == maxL) {
    n = compact ? snprintf(out, cap, "z%d x%d", maxL, fDeep)
                : snprintf(out, cap, "z%d tiles stretched %dx", maxL, fDeep);
  } else {
    n = compact ? snprintf(out, cap, "z%d-%d x%d-%d", maxL, minL, fDeep, fShallow)
                : snprintf(out, cap, "z%d-%d stretched %d-%dx", maxL, minL, fDeep, fShallow);
  }
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';                     // a cut label would state the wrong factor: none at all
    return 0;
  }
  return n;
}

int mapMissFind(const MapMiss* m, int n, int z, int tx, int ty) {
  if (!m) {
    return -1;
  }
  for (int i = 0; i < n; i++) {
    if (m[i].z == z && m[i].tx == tx && m[i].ty == ty) {
      return i;
    }
  }
  return -1;
}

int mapMissSlot(const MapMiss* m, int n, int cap) {
  if (!m || cap < 1) {
    return -1;
  }
  if (n < cap) {
    return n < 0 ? 0 : n;
  }
  int best = 0;
  for (int i = 1; i < cap; i++) {
    if ((int32_t)(m[i].hit - m[best].hit) < 0) {
      best = i;                        // signed difference: `hit` is a counter and counters wrap
    }
  }
  return best;
}

// ---------------------------------------------------------------- the ruler's dashes

int64_t mapDashRange(int x0, int y0, int x1, int y1, int bx, int by, int bw, int bh,
                     int dash, int64_t* first, int64_t* end) {
  if (first) {
    *first = 0;
  }
  if (end) {
    *end = 0;
  }
  if (dash < 1 || bw <= 0 || bh <= 0) {
    return 0;
  }
  const int64_t dx = (int64_t)x1 - x0, dy = (int64_t)y1 - y0;
  const int64_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  const int64_t steps = (adx > ady ? adx : ady) / dash;
  if (steps <= 0) {
    return 0;
  }
  /* The crosshair end is the viewport's centre, so the part of the line inside the box is one
   * run of steps ending at `steps`. If it is not inside, nothing is tried: this answers the
   * question the ruler asks and no other. */
  if (x1 < bx || x1 >= bx + bw || y1 < by || y1 >= by + bh) {
    return steps;
  }
  /* Where the line ENTERS the box: Liang-Barsky's lower bound on t. Only the lower bound —
   * the far end is inside. The steps are truncated toward zero (mapDashAt, as the old int code
   * did), which moves a point toward the anchor and so can only enter LATER than this t; two
   * steps early is a margin for the double arithmetic, not for that. */
  double lo = 0.0;
  const double p[4] = { -(double)dx, (double)dx, -(double)dy, (double)dy };
  const double q[4] = { (double)x0 - bx, (double)(bx + bw - 1) - x0,
                        (double)y0 - by, (double)(by + bh - 1) - y0 };
  for (int k = 0; k < 4; k++) {
    if (p[k] < 0.0) {
      const double t = q[k] / p[k];
      if (t > lo) {
        lo = t;
      }
    }
  }
  int64_t f = (int64_t)(lo * (double)steps) - 2;
  if (f < 0) {
    f = 0;
  }
  f &= ~(int64_t)1;                    // even: every other step from the ANCHOR is drawn
  if (first) {
    *first = f;
  }
  if (end) {
    *end = steps;
  }
  return steps;
}

void mapDashAt(int x0, int y0, int x1, int y1, int64_t s, int64_t steps, int* x, int* y) {
  if (steps <= 0) {
    s = 0;
    steps = 1;
  }
  // Between the two ends for 0 <= s <= steps, so it fits back in an int.
  if (x) {
    *x = (int)(x0 + ((int64_t)x1 - x0) * s / steps);
  }
  if (y) {
    *y = (int)(y0 + ((int64_t)y1 - y0) * s / steps);
  }
}

int mapLatLonToView(int z, int32_t cx, int32_t cy, int vw, int vh,
                    double lat, double lon, int* vx, int* vy) {
  double wx = 0, wy = 0;
  mapLatLonToWorld(lat, lon, z, &wx, &wy);
  const int32_t ws = mapWorldPx(z);
  const double left = (double)cx - vw / 2;
  const double top  = (double)cy - vh / 2;

  double dx = wx - left;
  /* Fold into [0, ws) first, then take the copy nearest the viewport. Without the second
   * step a pin 1 km east of you across the antimeridian would draw at the far west edge. */
  dx = fmod(dx, (double)ws);
  if (dx < 0) {
    dx += ws;
  }
  if (dx > ((double)ws + vw) / 2.0) {
    dx -= ws;
  }
  const double dy = wy - top;

  const int ix = (int)floor(dx + 0.5);
  const int iy = (int)floor(dy + 0.5);
  if (vx) {
    *vx = ix;
  }
  if (vy) {
    *vy = iy;
  }
  return (ix >= 0 && ix < vw && iy >= 0 && iy < vh) ? 1 : 0;
}

void mapViewToLatLon(int z, int32_t cx, int32_t cy, int vw, int vh,
                     int px, int py, double* lat, double* lon) {
  const double wx = (double)cx - vw / 2 + px;
  const double wy = (double)cy - vh / 2 + py;
  mapWorldToLatLon(wx, wy, z, lat, lon);
}

int mapPanStep(int run) {
  if (run < 1) {
    return MAP_PAN_NUDGE_PX;      /* a tap */
  }
  if (run < 3) {
    return 24;                    /* a hold: 240 px/s at the 100 ms repeat, then ... */
  }
  if (run < 5) {
    return 48;
  }
  if (run < 7) {
    return 72;
  }
  return 96;                      /* ... 960 px/s, four screens a second */
}

int mapPanHoldMove(int run, uint32_t dtMs, int* carry) {
  if (dtMs > MAP_PAN_HOLD_MAX_DT_MS) {
    dtMs = MAP_PAN_HOLD_MAX_DT_MS;
  }
  int c = carry ? *carry : 0;
  c += mapPanStep(run < 1 ? 1 : run) * (int)dtMs;
  const int px = c / MAP_PAN_HOLD_STEP_MS;
  if (carry) {
    *carry = c - px * MAP_PAN_HOLD_STEP_MS;
  }
  return px;
}

int mapScaleBar(double metersPerPixel, int maxPx, int* px) {
  if (px) {
    *px = 0;
  }
  if (!(metersPerPixel > 0) || maxPx <= 0) {
    return 0;
  }
  static const int mant[3] = { 1, 2, 5 };
  int bestM = 0, bestPx = 0;
  double dec = 1.0;
  for (int e = 0; e < 8; e++) {
    for (int i = 0; i < 3; i++) {
      const double m = mant[i] * dec;
      const int p = (int)(m / metersPerPixel + 0.5);
      if (p >= 1 && p <= maxPx) {
        bestM = (int)m;
        bestPx = p;
      }
    }
    dec *= 10.0;
  }
  if (!bestM) {
    /* Nothing from 1 m to 50 km fits, which means the screen is either absurdly zoomed in or
     * absurdly zoomed out. Show the smallest bar rather than no bar: a scale of 1 m drawn
     * wider than the box is visibly wrong, and no bar at all is invisibly wrong. */
    bestM = 1;
    bestPx = (int)(1.0 / metersPerPixel + 0.5);
    if (bestPx < 1) {
      bestPx = 1;
    }
    if (bestPx > maxPx) {
      bestPx = maxPx;
    }
  }
  if (px) {
    *px = bestPx;
  }
  return bestM;
}

int mapTilePath(char* out, size_t cap, const char* root, const char* area,
                int z, int x, int y) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (!root || !area || x < 0 || y < 0) {
    return 0;
  }
  const int n = snprintf(out, cap, "%s/%s/%d/%d/%d.565", root, area, z, x, y);
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return n;
}

int mapAreaNameOk(const char* name) {
  if (!name || !name[0] || name[0] == '.') {
    return 0;
  }
  size_t n = 0;
  for (const char* p = name; *p; p++, n++) {
    const char c = *p;
    const int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!ok) {
      return 0;
    }
  }
  return n <= 31;
}
