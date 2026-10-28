/*
 * tile_plan.cpp — see tile_plan.h.
 */
#include "tile_plan.h"
#include "map_tiles.h"
#include "elev_tiles.h"                   // ELEV_Z, ELEV_Z_COARSE: macros only
#include "units.h"                        // the Detail row in the Units setting

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>                      // strncasecmp

// ── what a job covers ────────────────────────────────────────────────────────────────────

void tilePlanRange(double lat, double lon, int radiusKm, int z, int* x0, int* x1, int* y0, int* y1) {
  const double dlat = radiusKm / 111.0;
  double c = cos(lat * M_PI / 180.0);
  if (c < 0.2) c = 0.2;
  const double dlon = radiusKm / (111.0 * c);
  double wx0, wy0, wx1, wy1;
  mapLatLonToWorld(mapClampLat(lat + dlat), mapWrapLon(lon - dlon), z, &wx0, &wy0);
  mapLatLonToWorld(mapClampLat(lat - dlat), mapWrapLon(lon + dlon), z, &wx1, &wy1);
  const int n = 1 << z;
  *x0 = (int)floor(wx0 / MAP_TILE_PX);
  *x1 = (int)floor(wx1 / MAP_TILE_PX);
  *y0 = (int)floor(wy0 / MAP_TILE_PX);
  *y1 = (int)floor(wy1 / MAP_TILE_PX);
  if (*x1 < *x0) *x1 += n;             // the square straddles the antimeridian
  if (*y0 < 0) *y0 = 0;
  if (*y1 > n - 1) *y1 = n - 1;
  if (*y1 < *y0) *y1 = *y0;
}

int64_t tilePlanLevelTiles(double lat, double lon, int radiusKm, int z) {
  int x0, x1, y0, y1;
  tilePlanRange(lat, lon, radiusKm, z, &x0, &x1, &y0, &y1);
  return (int64_t)(x1 - x0 + 1) * (int64_t)(y1 - y0 + 1);
}

int64_t tilePlanTiles(double lat, double lon, int radiusKm, int zMax) {
  int64_t total = 0;
  for (int z = TILE_PLAN_ZOOM_BASE; z <= zMax; z++) {
    total += tilePlanLevelTiles(lat, lon, radiusKm, z);
  }
  return total;
}

uint64_t tilePlanCardBytes(int64_t tiles) {
  return tiles > 0 ? (uint64_t)tiles * (uint64_t)MAP_TILE_BYTES : 0;
}

uint64_t tilePlanNetBytes(int64_t tiles, int kbPerTile) {
  return (tiles > 0 && kbPerTile > 0) ? (uint64_t)tiles * (uint64_t)kbPerTile * 1024u : 0;
}

int64_t tilePlanElevTiles(double lat, double lon, int radiusKm) {
  if (radiusKm <= 0) {
    return 0;
  }
  return tilePlanLevelTiles(lat, lon, radiusKm, ELEV_Z) + tilePlanLevelTiles(lat, lon, radiusKm, ELEV_Z_COARSE);
}

int64_t tilePlanElevSpaceTiles(double lat, double lon, int radiusKm, bool elev, bool resume,
                               bool layerWhole) {
  if (!elev || (resume && layerWhole)) {
    return 0;
  }
  return tilePlanElevTiles(lat, lon, radiusKm);
}

int tilePlanParseElevArg(const char* rest) {
  if (!rest) {
    return -1;
  }
  while (*rest == ' ' || *rest == '\t') rest++;
  if (!*rest) {
    return -1;
  }
  if (strncasecmp(rest, "elev", 4) == 0 && (rest[4] == ' ' || rest[4] == '\t')) {
    rest += 4;
    while (*rest == ' ' || *rest == '\t') rest++;
  }
  if (*rest != '0' && *rest != '1') {
    return -2;
  }
  const int v = *rest - '0';
  rest++;
  while (*rest == ' ' || *rest == '\t') rest++;
  return *rest ? -2 : v;
}

int tilePlanAreaWritable(const char* name) {
  if (!mapAreaNameOk(name)) {
    return 0;
  }
  const size_t n = strlen(name);
  return name[n - 1] != '.' && strcasecmp(name, ELEV_DIR) != 0;
}

int tilePlanParseDlurl(const char* arg, char* tmpl, size_t tcap, char* area, size_t acap) {
  if (!arg || !tmpl || !area || tcap == 0 || acap == 0) {
    return 0;
  }
  tmpl[0] = '\0';
  area[0] = '\0';
  while (*arg == ' ' || *arg == '\t') arg++;
  const char* t = arg;
  while (*arg && *arg != ' ' && *arg != '\t') arg++;
  const size_t tn = (size_t)(arg - t);
  if (tn == 0 || tn >= tcap) {
    return 0;
  }
  while (*arg == ' ' || *arg == '\t') arg++;
  const char* a = arg;
  while (*arg && *arg != ' ' && *arg != '\t') arg++;
  size_t an = (size_t)(arg - a);
  while (*arg == ' ' || *arg == '\t') arg++;
  if (*arg) {
    return 0;                             // a third word: a mistake to be told about
  }
  const char* name = an ? a : "custom";
  if (!an) an = strlen(name);
  if (an >= acap) {
    return 0;
  }
  memcpy(area, name, an);
  area[an] = '\0';
  if (!tilePlanAreaWritable(area)) {
    area[0] = '\0';
    return 0;
  }
  memcpy(tmpl, t, tn);
  tmpl[tn] = '\0';
  return 1;
}

// ── the Detail row ───────────────────────────────────────────────────────────────────────

int tilePlanDepthTop(int srcZMax) {
  int top = srcZMax < MAPS_DL_DEPTH_MAX ? srcZMax : MAPS_DL_DEPTH_MAX;
  return top < MAPS_DL_DEPTH_MIN ? MAPS_DL_DEPTH_MIN : top;
}

int tilePlanDepthShown(int wish, int srcZMax) {
  const int top = tilePlanDepthTop(srcZMax);
  if (wish > top) return top;
  if (wish < MAPS_DL_DEPTH_MIN) return MAPS_DL_DEPTH_MIN;
  return wish;
}

int tilePlanDepthNext(int wish, int srcZMax) {
  const int shown = tilePlanDepthShown(wish, srcZMax);
  return shown >= tilePlanDepthTop(srcZMax) ? MAPS_DL_DEPTH_MIN : shown + 1;
}

void tilePlanDetailText(char* out, size_t cap, int depth, double lat, int units) {
  if (!out || !cap) {
    return;
  }
  char px[16], buf[48];
  if (depth >= 17) {
    snprintf(buf, sizeof(buf), "Detail: z%d (4x z16's cost)", depth);
  } else if (depth == 16) {
    snprintf(buf, sizeof(buf), "Detail: z16 (4x z15's cost)");
  } else if (depth == 15) {
    unitsFmtDist(mapMetersPerPixel(lat, 15), units, px, sizeof(px));
    snprintf(buf, sizeof(buf), "Detail: z15 (%s/pixel)", px);
  } else {
    snprintf(buf, sizeof(buf), "Detail: z%d (coarser)", depth);
  }
  if (strlen(buf) >= cap) {
    out[0] = '\0';
    return;
  }
  strcpy(out, buf);
}

// ── the time it takes ────────────────────────────────────────────────────────────────────

int tilePlanMinIntervalMs(const char* srcKey, int z) {
  return (srcKey && !strcmp(srcKey, "otm") && z >= 17) ? TILE_PLAN_THROTTLE_MS : 0;
}

double tilePlanSeconds(double lat, double lon, int radiusKm, int zMax, float secPerTile, const char* srcKey) {
  double secs = 0;
  for (int z = TILE_PLAN_ZOOM_BASE; z <= zMax; z++) {
    double rate = secPerTile;
    const double floorRate = tilePlanMinIntervalMs(srcKey, z) / 1000.0;
    /* Where the throttle applies it IS the pace: OpenTopoMap's z17 tiles are small, and phone 2
     * measured 2,067 ms a tile over 400 of them (2026-09-23) against the 2,000 ms interval —
     * not the 4.0 s its z11-16 tiles take. So the interval plus a little, not max(rate, it). */
    if (floorRate > 0) rate = floorRate + 0.1;
    secs += (double)tilePlanLevelTiles(lat, lon, radiusKm, z) * rate;
  }
  return secs;
}

void tilePlanDuration(double secs, char* out, size_t cap) {
  if (!out || !cap) return;
  if (!(secs > 0)) secs = 0;
  const double mins = secs / 60.0;
  if (mins < 90.0) {
    int m = (int)(mins + 0.5);
    snprintf(out, cap, "%d min", m < 1 ? 1 : m);
  } else if (mins < 48.0 * 60.0) {
    snprintf(out, cap, "%d h", (int)(mins / 60.0 + 0.5));
  } else {
    snprintf(out, cap, "%.1f days", mins / (24.0 * 60.0));
  }
}

void tilePlanBytes(uint64_t bytes, char* out, size_t cap) {
  if (!out || !cap) return;
  const uint64_t MiB = 1024ull * 1024ull;
  if (bytes >= 1024ull * MiB) {
    snprintf(out, cap, "%.1f GB", (double)bytes / (double)(1024ull * MiB));
  } else {
    snprintf(out, cap, "%u MB", (unsigned)(bytes / MiB));
  }
}

void tilePlanRateReset(TilePlanRate* r) {
  memset(r, 0, sizeof(*r));
}

void tilePlanRateAdd(TilePlanRate* r, uint32_t ms) {
  if (r->n == TILE_PLAN_RATE_N) {
    r->sum -= r->ms[r->head];
  } else {
    r->n++;
  }
  r->ms[r->head] = ms;
  r->sum += ms;
  r->head = (r->head + 1) % TILE_PLAN_RATE_N;
}

uint32_t tilePlanRateMeanMs(const TilePlanRate* r) {
  return r->n ? (uint32_t)(r->sum / (uint64_t)r->n) : 0;
}

// ── the order ────────────────────────────────────────────────────────────────────────────

void tilePlanOrderInit(TilePlanOrder* o, int x0, int x1, int y0, int y1, int cx, int cy) {
  memset(o, 0, sizeof(*o));
  o->x0 = x0;
  o->y0 = y0;
  o->w = x1 - x0 + 1;
  o->h = y1 - y0 + 1;
  if (o->w < 1 || o->h < 1) {
    o->done = true;
    return;
  }
  o->bw = (o->w + TILE_PLAN_BLOCK - 1) / TILE_PLAN_BLOCK;
  o->bh = (o->h + TILE_PLAN_BLOCK - 1) / TILE_PLAN_BLOCK;
  int u = cx - x0, v = cy - y0;
  if (u < 0) u = 0;
  if (u > o->w - 1) u = o->w - 1;
  if (v < 0) v = 0;
  if (v > o->h - 1) v = o->h - 1;
  o->cbu = u / TILE_PLAN_BLOCK;
  o->cbv = v / TILE_PLAN_BLOCK;
  int m = o->cbu;
  if (o->bw - 1 - o->cbu > m) m = o->bw - 1 - o->cbu;
  if (o->cbv > m) m = o->cbv;
  if (o->bh - 1 - o->cbv > m) m = o->bh - 1 - o->cbv;
  o->maxR = m;
}

/* The next block of the ring walk that lies inside the square. Ring r is the border of the
 * (2r+1)^2 blocks around the centre block, walked row by row: a full row at dv = -r and dv = r,
 * only the two ends (du = -r, du = r) on the rows between. */
static bool nextBlock(TilePlanOrder* o) {
  for (;;) {
    if (!o->started) {
      o->started = true;
      o->r = 0;
      o->dv = 0;
      o->du = 0;
    } else if (o->r == 0) {
      o->r = 1;
      o->dv = -1;
      o->du = -1;
    } else if (o->dv == -o->r || o->dv == o->r) {
      if (++o->du > o->r) {
        o->dv++;
        o->du = -o->r;
      }
    } else if (o->du == -o->r) {
      o->du = o->r;
    } else {
      o->dv++;
      o->du = -o->r;
    }
    if (o->r > 0 && o->dv > o->r) {         // this ring is walked: the next one
      o->r++;
      o->dv = -o->r;
      o->du = -o->r;
    }
    if (o->r > o->maxR) {
      return false;
    }
    const int bu = o->cbu + o->du, bv = o->cbv + o->dv;
    if (bu >= 0 && bu < o->bw && bv >= 0 && bv < o->bh) {
      o->bu = bu;
      o->bv = bv;
      o->u = bu * TILE_PLAN_BLOCK;
      o->v = bv * TILE_PLAN_BLOCK;
      return true;
    }
  }
}

bool tilePlanOrderNext(TilePlanOrder* o, int* x, int* y) {
  if (o->done) {
    return false;
  }
  if (!o->inBlock) {
    if (!nextBlock(o)) {
      o->done = true;
      return false;
    }
    o->inBlock = true;
  }
  *x = o->x0 + o->u;
  *y = o->y0 + o->v;
  o->ord++;
  // Column-major inside the block: down the column, then the next column.
  const int vEnd = (o->bv + 1) * TILE_PLAN_BLOCK < o->h ? (o->bv + 1) * TILE_PLAN_BLOCK : o->h;
  const int uEnd = (o->bu + 1) * TILE_PLAN_BLOCK < o->w ? (o->bu + 1) * TILE_PLAN_BLOCK : o->w;
  if (++o->v >= vEnd) {
    o->v = o->bv * TILE_PLAN_BLOCK;
    if (++o->u >= uEnd) {
      o->inBlock = false;
    }
  }
  return true;
}

void tilePlanLevelOrder(TilePlanOrder* o, double lat, double lon, int radiusKm, int z) {
  int x0, x1, y0, y1;
  tilePlanRange(lat, lon, radiusKm, z, &x0, &x1, &y0, &y1);
  double wx, wy;
  mapLatLonToWorld(mapClampLat(lat), mapWrapLon(lon), z, &wx, &wy);
  int cx = (int)floor(wx / MAP_TILE_PX);
  const int cy = (int)floor(wy / MAP_TILE_PX);
  if (cx < x0) cx += 1 << z;           // the centre is east of the antimeridian, the square starts west of it
  tilePlanOrderInit(o, x0, x1, y0, y1, cx, cy);
}

// ── why a run ended, and whether to start it again ───────────────────────────────────────

const char* tileStopReasonName(int reason) {
  switch (reason) {
  case TILE_STOP_NONE:        return "none";
  case TILE_STOP_FINISHED:    return "finished";
  case TILE_STOP_USER:        return "user";
  case TILE_STOP_NOWIFI:      return "wifi";
  case TILE_STOP_CALL:        return "call";
  case TILE_STOP_NETFAILS:    return "network";
  case TILE_STOP_DECODEFAILS: return "decode";
  case TILE_STOP_CARDFAILS:   return "card";
  case TILE_STOP_RAM:         return "ram";
  case TILE_STOP_BATTERY:     return "battery";
  case TILE_STOP_GAME:        return "game";
  case TILE_STOP_POWEROFF:    return "power-off";
  default:                    return "?";
  }
}

uint32_t tilePlanCoolDownMs(int n) {
  static const uint32_t MIN[] = { 10, 20, 40, 60 };
  if (n < 1) return 0;
  const int i = n > 4 ? 3 : n - 1;
  return MIN[i] * 60u * 1000u;
}

/* The order is deliberate: first what ends the matter (nothing pending, given up), then time
 * (a cool-down says "Retrying in 12 min" whatever else is closed), then the gates a person
 * can open — card, Files, game, call, power, WiFi, network — so the form names the one to
 * act on. Every millis() comparison is a difference: a multi-day job sees millis() wrap. */
TileGate tileResumeCheck(TileResume* r, const TileWorld* w, uint32_t* waitMs) {
  if (waitMs) *waitMs = 0;
  if (!r->pending) {
    return TILE_GATE_IDLE;
  }
  if (r->giveUp != TILE_GIVEUP_NONE) {
    return TILE_GATE_GAVE_UP;
  }
  if (r->boot && r->strikes >= TILE_PLAN_STRIKES) {
    r->giveUp = TILE_GIVEUP_STRIKES;
    return TILE_GATE_GAVE_UP;
  }
  if (r->cools > 0) {
    if (r->cools >= TILE_PLAN_STALL_TRIES) {
      r->giveUp = TILE_GIVEUP_STALLED;
      return TILE_GATE_GAVE_UP;
    }
    const uint32_t cool = tilePlanCoolDownMs(r->cools);
    const uint32_t since = (uint32_t)(w->now - r->stopMs);
    if (since < cool) {
      if (waitMs) *waitMs = cool - since;
      return TILE_GATE_COOL;
    }
  }
  if (!w->card) return TILE_GATE_CARD;
  if (w->filesJob) return TILE_GATE_FILES;
  if (w->game) return TILE_GATE_GAME;
  if (w->call || w->sinceCallMs < TILE_PLAN_CALL_QUIET_MS) {
    if (waitMs && !w->call) *waitMs = TILE_PLAN_CALL_QUIET_MS - w->sinceCallMs;
    return TILE_GATE_CALL;
  }
  if (!w->usb && !(w->volts >= MAPS_DL_BATT_FLOOR)) return TILE_GATE_POWER;
  if (!w->wifiUp) return TILE_GATE_WIFI;
  if (!w->sameNet) return TILE_GATE_NET;
  if (w->wifiUpMs < TILE_PLAN_WIFI_UP_MS) {
    if (waitMs) *waitMs = TILE_PLAN_WIFI_UP_MS - w->wifiUpMs;
    return TILE_GATE_WIFI;
  }
  return TILE_GATE_GO;
}

void tileResumeFresh(TileResume* r, uint32_t now) {
  memset(r, 0, sizeof(*r));
  r->progressMs = now;
  r->stopMs = now;
}

void tileResumeLoaded(TileResume* r, uint8_t strikes, uint8_t giveUp, uint8_t reason, uint32_t now) {
  memset(r, 0, sizeof(*r));
  r->pending = true;
  r->boot = true;
  r->strikes = strikes;
  r->giveUp = giveUp;
  r->reason = reason;
  r->stopMs = now;
  r->progressMs = now;       // millis() is per boot: the day-without-progress clock starts again
}

uint8_t tileResumeStarting(TileResume* r, bool automatic) {
  if (automatic && r->boot && r->strikes < 255) {
    r->strikes++;            // counted BEFORE the run: a crash inside it cannot skip the count
  }
  r->boot = false;
  r->pending = false;
  r->giveUp = TILE_GIVEUP_NONE;   // a start (by a person, or by the rules) is a fresh chance
  r->reason = TILE_STOP_NONE;
  r->refused = false;
  return r->strikes;
}

void tileResumeStopped(TileResume* r, int reason, uint32_t now) {
  r->reason = (uint8_t)reason;
  r->refused = false;
  r->strikes = 0;            // the run ended with its reason recorded: it did not crash the phone
  r->stopMs = now;
  switch (reason) {
  case TILE_STOP_FINISHED:
  case TILE_STOP_USER:
    r->pending = false;
    r->cools = 0;
    return;
  case TILE_STOP_CARDFAILS:
    r->pending = true;
    r->giveUp = TILE_GIVEUP_CARD;
    return;
  case TILE_STOP_NETFAILS:
  case TILE_STOP_DECODEFAILS:
  case TILE_STOP_RAM:
    r->pending = true;
    if (r->cools < 255) r->cools++;
    return;
  default:                   // wifi, call, game, battery: wait for the gate, no cool-down
    r->pending = true;
    return;
  }
}

void tileResumeRefused(TileResume* r, uint32_t now) {
  r->pending = true;
  r->refused = true;
  if (r->cools < 255) r->cools++;
  r->stopMs = now;
}

bool tileResumeProgress(TileResume* r, int newThisRun, uint32_t now) {
  r->progressMs = now;
  r->cools = 0;
  if (newThisRun >= TILE_PLAN_STRIKE_CLEAR && r->strikes) {
    r->strikes = 0;
    return true;
  }
  return false;
}
