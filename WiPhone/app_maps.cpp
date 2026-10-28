/* app_maps.cpp — see app_maps.h for the design: why the tiles are raw, why the card is read
 * in pieces, and why a pin is not a waypoint until you say so. */

#include "app_maps.h"
#include "meshtastic_service.h"
#include "mesh_pos.h"
#include <SD.h>
#include <Preferences.h>
#include "menu_wrap.h"      // the strip's note, broken into rows that fit
#include <WiFi.h>
#include "tile_fetch.h"
#include "elev_sd.h"        // the altitude layer on the card (0.9.80)
#include "units.h"          // metric or US: the scale bar, every distance, every height
#include "prefs_almanac.h"  // gUnits, unitsSetPref
#include <stdarg.h>
#include <string.h>
#include <strings.h>        // strcasecmp: the reserved folder name, whatever its case
#include <sys/stat.h>

extern bool uiKeyStillHeld(uint32_t mask);   // WiPhone.ino: is this key physically down right now?
extern uint32_t uiKeyUpAgeMs(uint32_t mask); // ...and how long ago its last release was decoded


// ── colours ───────────────────────────────────────────────────────────────────────────────
// Deliberately loud. These sit on top of an aerial or topo tile, which can be any colour at
// all, so every marker is drawn with a black surround and nothing relies on contrast alone.
#define MAP_C_VOID      0x0000      // outside the world (letterbox at low zoom)
#define MAP_C_NOTILE    0x2124      // this tile is not on the card
#define MAP_C_LOADING   0x4A69      // ...it is, and it is on its way
#define MAP_C_PIN       0xFD20      // your pins: orange
#define MAP_C_PIN_SH    0xFFE0      // ...yellow once shared with the mesh
#define MAP_C_WP        0x07E0      // places heard from the mesh: green
#define MAP_C_NODE      0x07FF      // other people: cyan
#define MAP_C_NODE_OLD  0x8410      // ...grey when the fix is old enough to mislead
#define MAP_C_ME        0xFFFF      // this phone
#define MAP_C_STRIP     0x18E3      // the readout band at the bottom
#define MAP_C_CHIP      0x0000

#define MAP_SHAPE_DOT      0
#define MAP_SHAPE_DIAMOND  1

/* A node position older than this is drawn grey and its age is shown. Same 30 minutes the
 * research note used: on foot, half an hour is far enough to be somewhere else entirely, and
 * a map that draws a stale dot in the same colour as a live one is lying by omission. */
#define MAP_STALE_MS   (30u * 60u * 1000u)

/* Hold-to-scroll: a press held this long starts repeating, then a repeat every
 * MAP_PAN_HOLD_STEP_MS (map_tiles.h) or as soon after as the loop gets round to it, moving
 * the distance the time warrants (mapPanHoldMove). 400 ms keeps a deliberate tap a single
 * nudge. */
#define MAP_PAN_HOLD_DELAY_MS  400u
/* 🛑 A RELEASE UNDONE THIS SOON IS NOT A FINGER COMING OFF. The keypad chip emits a release
 * under a held thumb — two heartbeats of silence, a release, the same key pressed again 4 ms
 * later — at the first re-report slot or a few repeats in (the trace is by uiKeyUpAgeMs in
 * WiPhone.ino; {user}, 2026-09-20: "works for one to 3 or 4 jumps before it just stops"). The
 * re-press is killed as bounce on the UI path, correctly, so no keypress ever told the map the
 * finger was still there. So a hold survives a release younger than this: the poll keeps the
 * hold and looks again, and the same arrow arriving as a keypress inside the window (a
 * re-press just past KEY_BOUNCE_MS) is the hold continuing, not a tap. 4 ms observed; no
 * human re-taps a key inside 80 ms (WiPhone.ino, KEY_BOUNCE_MS), so 40 sits between. */
#define MAP_PAN_HOLD_BLIP_MS   40u
/* How often the map looks at a download running under it (jobTilesLanded): a finished tile
 * waits at most this long to replace the stretched parent drawn over its hole. */
#define MAPS_JOB_POLL_MS       10000
/* How long a pin's frame may sit "queued" before the map stops waiting and takes the safe side
 * (mapPinMeshSettle, final). A backstop only: every queued frame ends SENT or FAILED — TxDone,
 * the PHY's airtime timeout, or MESH RADIO LOST — within a full queue's worth of frames
 * (8 x ~3 s at the very worst). A minute means the pipeline itself is stuck: say so. */
#define MAPS_MESH_OP_GIVEUP_MS 60000
/* ⚠ The delay must be LONGER than uiKeyStillHeld()'s 350 ms staleness window: at 300 ms a
 * tap whose release was lost (two keys down, or any release the keypad's stale sweep exists
 * for) still read as "held" at the first repeat and the map took one phantom step — off the
 * pin the tap had just snapped to (review, 2026-09-19). */

static const char MAPS_NVS[] = "maps";

/* ALTITUDE (0.9.80). The crosshair's ground is sampled once the view has been still this long:
 * a tap's nudge settles in one or two ticks, a hold's sweep never does until it stops, so no
 * sample is spent on ground the map is only passing over. "You" is sampled again when you have
 * moved this far (or become somebody else: GPS -> pin -> the reference), and a comparison is
 * still drawn from the last sample while the new one is owed, if it is no further than
 * MAPS_ELEV_YOU_SHOW_M from where you are now - the next tick replaces it. */
#define MAPS_ELEV_SETTLE_MS    150u
#define MAPS_ELEV_YOU_MOVE_M   5.0
#define MAPS_ELEV_YOU_SHOW_M   100.0

/* Row keys. 🛑 NEVER 0 — MenuWidget::addOption() refuses a key of 0 outright and adds no row
 * at all (see the note on addNote() in GUI.h, and tests/check_menu_keys.py, which exists
 * because eight sites got this wrong on the same day). Display-only rows use addNote(). */
enum {
  ROW_FIRST        = 1,        // list rows are index + ROW_FIRST
  ROW_ACT          = 10000,
  ROW_M_PINS       = ROW_ACT + 1,
  ROW_M_PLACES     = ROW_ACT + 2,
  ROW_M_NODES      = ROW_ACT + 3,
  ROW_M_ME         = ROW_ACT + 4,
  ROW_M_AREA       = ROW_ACT + 5,
  ROW_M_RESCAN     = ROW_ACT + 6,
  ROW_M_HELP       = ROW_ACT + 7,
  ROW_M_BACK       = ROW_ACT + 8,
  ROW_M_DOWNLOAD   = ROW_ACT + 9,
  ROW_M_GOTO       = ROW_ACT + 10,
  ROW_M_FOLLOW     = ROW_ACT + 11,
  ROW_M_MEASURE    = ROW_ACT + 12,
  ROW_M_SNAP       = ROW_ACT + 13,
  ROW_M_ALT        = ROW_ACT + 14,     // Altitude: ON/off (0.9.80)
  ROW_M_UNITS      = ROW_ACT + 15,     // Units: metric / US
  ROW_P_RENAME     = ROW_ACT + 20,
  ROW_P_MOVE       = ROW_ACT + 21,
  ROW_P_SHARE      = ROW_ACT + 22,
  ROW_P_UNSHARE    = ROW_ACT + 23,
  ROW_P_CENTRE     = ROW_ACT + 24,
  ROW_P_DELETE     = ROW_ACT + 25,
  ROW_P_CANCEL     = ROW_ACT + 26,
  ROW_P_SHARE_ON   = ROW_ACT + 27,     // pick a different channel first
  ROW_DEL_YES      = ROW_ACT + 30,
  ROW_DEL_NO       = ROW_ACT + 31,
  ROW_D_SOURCE     = ROW_ACT + 40,
  ROW_D_RADIUS     = ROW_ACT + 41,
  ROW_D_DEPTH      = ROW_ACT + 42,
  ROW_D_START      = ROW_ACT + 43,
  ROW_D_STOP       = ROW_ACT + 44,
  ROW_D_BACK       = ROW_ACT + 45,
  ROW_D_RESUME     = ROW_ACT + 46,     // a waiting download: start it now, on this network
  ROW_D_ELEV       = ROW_ACT + 47,     // Elevation too: ON/off (0.9.80)
  ROW_C_CANCEL     = ROW_ACT + 59,
  ROW_C_BASE       = ROW_ACT + 60,     // channel i is ROW_C_BASE + i
};

/* The download form's choices. Radii are COVEY's, 2 to 20 km for every depth: a 20 km z17 run
 * is ~51,000 tiles and ~36 h, and that is what it is for ({user}: "overnight downloads or
 * multi day downloads even pre-trips"). The Detail row runs z13..z17 (MAPS_DL_DEPTH_MIN/MAX,
 * tile_plan.h) but offers only what the SOURCE has — z17 is OpenTopoMap's (and the custom
 * relay's); both USGS services answer 404 there, and USGS Topo's z16 is already its z15
 * drawing scaled up (checked 2026-09-18). Every level quadruples the cost. */
static const int MAPS_DL_RADII[] = { 2, 5, 10, 20 };
#define MAPS_DL_RADII_N    ((int)(sizeof(MAPS_DL_RADII) / sizeof(MAPS_DL_RADII[0])))
/* Seconds per tile, all-in (fetch, decode, the 128 KB write), measured on WiPhone 2 the night
 * this was built: USGS over kept-alive HTTPS 24 s / 21 tiles; OpenTopoMap 74 s / 18 tiles
 * (server-bound, at z<=15). The estimate on the screen is the tile count of each level times
 * this or the level's throttle interval (OTM z17: 2.0 s), whichever is longer — a CEILING,
 * since tiles already on the card are skipped. ⚠ OTM z17 is not measured yet: it renders on
 * demand more often, so re-measure on the first z17 run (`maps dl` prints ms/tile). */
static const float MAPS_DL_SEC_PER_TILE[] = { 1.2f, 1.2f, 4.0f, 1.2f };
/* [3] the custom source, a LAN relay: 1.2 s, measured 2026-10-05 on phone 1 over a whole
 * OpenTopoMap job from a Mac (tools/tile_relay.py) - the fetch is 0.12 s, the rest is the
 * phone's own decode and card write. It was a 0.6 s guess. */
/* An elevation tile (0.9.80): ~120 KB over kept-alive HTTPS from S3, an RGB PNG inflated and
 * converted, a 128 KB write. ⚠ NOT MEASURED ON THE PHONE: a guess from the USGS figure scaled by
 * the bytes; `maps dl` prints the run's seconds - re-measure on the first real run. */
#define MAPS_DL_ELEV_SEC_PER_TILE 2.0
#define MAPS_DL_ELEV_ROW          192     // the form's elevation account (PSRAM, not the loop's stack)
#define MAPS_DL_HOLD_MS    (2u * 60u * 1000u)   // the running form holds the screen this long after a key
#define MAPS_DL_STOP_ARM_MS 3000u              // Stop is a second press inside this

static MapsApp* s_instance = NULL;
volatile bool gMapsActive = false;

bool mapsSaveOpenView() {
  if (!s_instance) {
    return false;
  }
  s_instance->saveView();
  if (s_instance->pinsDirty) {
    s_instance->savePins();
  }
  return true;
}

// ---------------------------------------------------------------- construction

MapsApp::MapsApp(LCD& disp, ControlState& state, HeaderWidget* hdr, FooterWidget* ftr)
  : WindowedApp(disp, state, hdr, ftr) {
  log_d("create MapsApp");

  /* ⚠ SET WHAT THIS SCREEN NEEDS. msAppTimerEventPeriod is device-wide and apps routinely
   * leave their own value in it (the Game Boy leaves 250 ms). The map arms it only while a
   * tile is actually on its way; see the end of redrawScreen(). */
  controlState.msAppTimerEventPeriod = 0;

  appState = MAPS_VIEW;
  listKind = LIST_PINS;
  menu = NULL;
  textArea = NULL;
  headerTitle[0] = '\0';
  note[0] = '\0';
  statusLine[0] = '\0';

  vpX = 0;
  vpY = header->height();
  vpW = lcd.width();
  vpH = (int)lcd.height() - (int)header->height() - (int)footer->height();

  zoom = 15;
  cx = cy = 0;
  areaCount = 0;
  areaSel = -1;
  pinCount = 0;
  pinSel = -1;
  pinsDirty = false;
  memset(&meshOp, 0, sizeof(meshOp));   // no pin frame pending
  slotsAlive = 0;
  useSeq = 0;
  missCount = 0;
  drawSeq = 0;
  stretchAny = false;
  stretchMinL = stretchMaxL = 0;
  statusIsStretch = false;
  readErrorInView = false;
  memset(&jobSt, 0, sizeof(jobSt));
  jobPollMs = 0;
  jobSeen = jobSeenActive = false;
  jobSeenDone = jobSeenZ = jobSeenTotal = jobSeenPass = 0;
  loadActive = false;
  loadSlot = -1;
  loadZ = loadTx = loadTy = -1;
  loadGot = 0;
  loadRetries = 0;
  wantAny = false;
  wantZ = wantTx = wantTy = -1;
  pendingTiles = 0;
  tileMemFail = false;
  cardOk = false;
  pinsTruncated = false;
  panRun = 0;
  panLastMs = 0;
  panHoldMask = 0;
  panHoldSinceMs = 0;
  panHoldBlipMs = 0;
  panCarry = 0;
  panHoldPx = 0;
  panHoldDx = panHoldDy = 0;
  panHoldRan = false;
  snapPins = true;
  labelCount = 0;
  listCount = 0;
  helpTop = 0;
  dlSource = 0;
  dlRadiusIdx = 1;                 // 5 km
  dlDepth = 15;
  dlElev = true;
  dlHeld = false;
  dlLastMs = 0;
  dlKeep = ROW_D_START;
  dlWhy[0] = '\0';
  dlShownRunning = false;
  dlShownJob = false;
  dlKeyMs = 0;
  dlStopArmMs = 0;
  gotoLat[0] = gotoLon[0] = '\0';
  gotoField = 0;
  chanPending = 0;
  chanForPin = -1;
  followMe = false;
  followLastStamp = 0;
  measuring = false;
  measLatI = measLonI = 0;
  altOn = true;
  elevPresent = false;
  memset(&elevHere, 0, sizeof(elevHere));
  memset(&elevYou, 0, sizeof(elevYou));
  memset(&elevMeas, 0, sizeof(elevMeas));
  memset(&elevPin, 0, sizeof(elevPin));
  elevHereZoom = -1;
  elevHereCx = elevHereCy = 0;
  elevSeenZoom = -1;
  elevSeenCx = elevSeenCy = 0;
  elevSeenMs = 0;
  elevYouKind = MAPS_SELF_NONE;
  elevYouName[0] = '\0';
  elevJobSeen = tileFetchElevWritten();   // what a download wrote before this open, scanAreas sees
  {
    /* The last download choices, so a second area is two presses. Same namespace as the view. */
    Preferences p;
    if (p.begin(MAPS_NVS, true)) {
      dlSource = p.getInt("dlsrc", dlSource);
      dlRadiusIdx = p.getInt("dlrad", dlRadiusIdx);
      dlDepth = p.getInt("dldep", dlDepth);
      dlElev = p.getInt("dlelev", 1) != 0;
      snapPins = p.getInt("snap", 1) != 0;
      altOn = p.getInt("alt", 1) != 0;
      p.end();
    }
    if (dlSource < 0 || dlSource >= tileSourceCount()) dlSource = 0;
    if (dlRadiusIdx < 0 || dlRadiusIdx >= MAPS_DL_RADII_N) dlRadiusIdx = 1;
    if (dlDepth < MAPS_DL_DEPTH_MIN || dlDepth > MAPS_DL_DEPTH_MAX) dlDepth = 15;
  }
  s_instance = this;
  gMapsActive = true;

  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    slots[i].px = NULL;
    slots[i].z = -1;
    slots[i].tx = slots[i].ty = 0;
    slots[i].used = 0;
    slots[i].ready = false;
  }

  /* PSRAM for everything that is not a handful of ints. The pins table is 2 KB and the
   * projection scratch is another 512 B; neither belongs on the GUI task's stack, and
   * neither belongs in the ~19 KB internal heap that SIP and WiFi fight over. */
  pins  = (MapPin*)ps_malloc(sizeof(MapPin) * MAPS_MAX_PINS);
  pinVx = (int*)ps_malloc(sizeof(int) * MAPS_MAX_PINS);
  pinVy = (int*)ps_malloc(sizeof(int) * MAPS_MAX_PINS);
  /* 🛑 THE THREE SUCCEED TOGETHER OR NOT AT ALL, and that is not tidiness. Every user of the
   * projection scratch is guarded on `pins` alone — because the three are conceptually one
   * table — so a run where `pins` came back and `pinVx` did not would walk straight into
   * `pinVx[i]` through a null pointer, from the draw path, on every frame. Collapsing the
   * three states into two means the one guard everybody writes is the right guard. */
  if (!pins || !pinVx || !pinVy) {
    log_e("MAPS: no PSRAM for the pin table - pins are off this session");
    free(pins);
    free(pinVx);
    free(pinVy);
    pins = NULL;
    pinVx = pinVy = NULL;
  }

  if (!allocSlots()) {
    tileMemFail = true;
    log_e("MAPS: only %d tile slots - the map will not draw", slotsAlive);
  }

  scanAreas();
  loadPins();
  restoreView();
  /* First run, or a phone whose pins have all been deleted: the one key nothing on the screen
   * hints at is the one this app is for. The coordinates it displaces are on the row below on
   * every subsequent open, and the hint goes away the moment any key is pressed. */
  if (pinCount == 0 && !note[0]) {
    setNote("OK drops a pin here - Menu downloads maps");
  }
  enterState(MAPS_VIEW);
}

MapsApp::~MapsApp() {
  log_d("destroy MapsApp");
  /* Save the view FIRST. Everything after this can fail; where the user was looking is the
   * one piece of state they will notice missing, and it costs one NVS write. */
  saveView();
  /* A pin's frame still pending (review M1): whatever the radio has said by now is applied, and
   * no word takes the safe side — a retraction's id is KEPT (mapPinMeshSettle). Before the save
   * below, which writes what this changed. */
  settleMeshOp(true);
  if (pinsDirty) {
    savePins();
  }
  cancelLoad();
  freeWidgets();
  freeSlots();
  free(pins);
  free(pinVx);
  free(pinVy);
  pins = NULL;
  pinVx = pinVy = NULL;
  if (dlHeld) {
    controlState.holdScreenAwake(false);
    dlHeld = false;
  }
  s_instance = NULL;
  gMapsActive = false;
  controlState.msAppTimerEventPeriod = 0;    // do not leave the CPU waking for a closed app
}

bool MapsApp::allocSlots() {
  slotsAlive = 0;
  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    slots[i].px = (uint16_t*)ps_malloc(MAP_TILE_BYTES);
    if (slots[i].px) {
      slotsAlive++;
    }
  }
  /* Two is the floor, not a nicety: a viewport can straddle 2x2 tiles, and with one slot the
   * cache would evict the tile it is about to need on every single blit — the card would be
   * read four times per repaint forever. Below two, the app says so instead. */
  return slotsAlive >= 2;
}

void MapsApp::freeSlots() {
  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    if (slots[i].px) {
      free(slots[i].px);
      slots[i].px = NULL;
    }
    slots[i].z = -1;
    slots[i].ready = false;
  }
  slotsAlive = 0;
}

// ---------------------------------------------------------------- the card

/* A name that is a plain unsigned number, with an optional extension when `allowExt`:
 * "5249" -> 5249, "11443.565" -> 11443, "x" -> -1, "" -> -1, ".565" -> -1.
 *
 * ⚠ NOT atoi(). atoi("12abc") is 12 and atoi("abc") is 0, so with atoi a stray folder becomes
 * zoom 0 or tile column 0 — a real coordinate — and the map quietly offers a level, or a
 * column, with nothing behind it. Every name that is not exactly a number is refused here,
 * which is also what keeps macOS's "._11443.565" sidecars out (they start with a dot). */
static long numericName(const char* name, bool allowExt) {
  if (!name || !name[0]) {
    return -1;
  }
  long v = 0;
  int digits = 0;
  for (const char* p = name; *p; p++) {
    if (*p == '.' && allowExt && digits) {
      return v;                        // the extension starts here and the stem was a number
    }
    if (*p < '0' || *p > '9') {
      return -1;
    }
    v = v * 10 + (*p - '0');
    if (v > 134217727L) {              // past the deepest zoom's tile count: not one of ours
      return -1;
    }
    digits++;
  }
  return digits ? v : -1;
}

// A zoom folder: a number, and one this firmware can address.
static int zoomFolder(const char* name) {
  const long v = numericName(name, false);
  return (v >= 0 && v <= MAP_ZOOM_MAX) ? (int)v : -1;
}

// On this core File::name() returns the FULL path; every caller has to basename it itself.
static const char* baseName(const char* path) {
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

/* A folder under /maps that is NOT a map area, whatever it holds. `elev` (ELEV_DIR) is the
 * altitude layer: numbered zoom folders of .elv tiles, which scanAreas would otherwise list as a
 * map with nothing drawable in it. Case-blind: FAT is, so "Elev" is the same folder. Every loop
 * that takes the folders of /maps for areas asks this (scanAreas, mapsConsoleStatus). */
static bool mapsReservedName(const char* name) {
  return name && strcasecmp(name, ELEV_DIR) == 0;
}

void MapsApp::scanAreas() {
  areaCount = 0;
  areaSel = -1;
  /* The altitude layer is judged with the areas: the same card, read at the same moments (the
   * app opening, Rescan), and every height cached from the card before is forgotten with it. */
  elevPresent = false;
  elevHere.valid = elevYou.valid = elevMeas.valid = elevPin.valid = false;
  elevHereZoom = -1;
  /* Is there a card at all? "No map tiles on the card" sends somebody to the computer to
   * convert tiles; "No SD card" sends them to find the card. Getting those two the wrong way
   * round costs a trip. */
  {
    File root = SD.open("/");
    cardOk = (bool)root && root.isDirectory();
    if (root) {
      root.close();
    }
  }
  if (!cardOk) {
    return;
  }
  elevPresent = elevCardPresent();
  if (!SD.exists(MAPS_ROOT)) {
    /* Make the folder so the message can name a place that exists. A user told "put tiles in
     * /maps" who then cannot find /maps has been given a puzzle, not an instruction. */
    SD.mkdir(MAPS_ROOT);
    return;
  }
  File dir = SD.open(MAPS_ROOT);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return;
  }
  File f;
  while ((f = dir.openNextFile())) {
    const char* nm = baseName(f.name());
    if (!f.isDirectory() || !mapAreaNameOk(nm) || mapsReservedName(nm)) {
      f.close();
      continue;
    }
    if (areaCount >= MAPS_MAX_AREAS) {
      f.close();
      break;
    }
    MapArea* a = &areas[areaCount];
    strlcpy(a->name, nm, sizeof(a->name));
    a->zMin = MAP_ZOOM_MAX + 1;
    a->zMax = -1;
    // Which zoom folders are really there.
    char zpath[96];
    snprintf(zpath, sizeof(zpath), "%s/%s", MAPS_ROOT, nm);
    File zd = SD.open(zpath);
    if (zd && zd.isDirectory()) {
      File zf;
      while ((zf = zd.openNextFile())) {
        const int z = zf.isDirectory() ? zoomFolder(baseName(zf.name())) : -1;
        if (z >= 0) {
          if (z < a->zMin) {
            a->zMin = z;
          }
          if (z > a->zMax) {
            a->zMax = z;
          }
        }
        zf.close();
      }
    }
    if (zd) {
      zd.close();
    }
    f.close();
    if (a->zMax >= 0) {          // a folder with no zoom levels in it is not a map
      scanAreaExtent(a);
      areaCount++;
    }
  }
  dir.close();
  if (areaCount > 0) {
    areaSel = 0;
  }
}

/* The bounding box of an area's tiles at its coarsest zoom: the x folders' range, and the
 * y files' range in the first and last of them. Three directory listings of a few entries
 * each — at z11 the whole of a 20 km download is two x folders. A box, not a census: an
 * L-shaped area reads as its enclosing rectangle, which is what "where is this map" wants. */
void MapsApp::scanAreaExtent(MapArea* a) {
  a->hasExtent = false;
  a->xMin = a->yMin = 0x7FFFFFFFL;
  a->xMax = a->yMax = -1;
  char path[112];
  snprintf(path, sizeof(path), "%s/%s/%d", MAPS_ROOT, a->name, a->zMin);
  File zd = SD.open(path);
  if (!zd || !zd.isDirectory()) {
    if (zd) {
      zd.close();
    }
    return;
  }
  File xf;
  while ((xf = zd.openNextFile())) {
    const long x = xf.isDirectory() ? numericName(baseName(xf.name()), false) : -1;
    if (x >= 0) {
      if (x < a->xMin) {
        a->xMin = x;
      }
      if (x > a->xMax) {
        a->xMax = x;
      }
    }
    xf.close();
  }
  zd.close();
  if (a->xMax < 0) {
    return;
  }
  const long cols[2] = { a->xMin, a->xMax };
  for (int c = 0; c < (cols[0] == cols[1] ? 1 : 2); c++) {
    snprintf(path, sizeof(path), "%s/%s/%d/%ld", MAPS_ROOT, a->name, a->zMin, cols[c]);
    File xd = SD.open(path);
    if (!xd || !xd.isDirectory()) {
      if (xd) {
        xd.close();
      }
      continue;
    }
    File yf;
    while ((yf = xd.openNextFile())) {
      const long y = yf.isDirectory() ? -1 : numericName(baseName(yf.name()), true);
      if (y >= 0) {
        if (y < a->yMin) {
          a->yMin = y;
        }
        if (y > a->yMax) {
          a->yMax = y;
        }
      }
      yf.close();
    }
    xd.close();
  }
  a->hasExtent = (a->yMax >= 0);
}

bool MapsApp::areaHolds(int idx, double lat, double lon) const {
  if (idx < 0 || idx >= areaCount || !areas[idx].hasExtent) {
    return false;
  }
  const MapArea* a = &areas[idx];
  double wx = 0, wy = 0;
  mapLatLonToWorld(lat, lon, a->zMin, &wx, &wy);
  const long tx = (long)(wx / MAP_TILE_PX), ty = (long)(wy / MAP_TILE_PX);
  return tx >= a->xMin && tx <= a->xMax && ty >= a->yMin && ty <= a->yMax;
}

void MapsApp::areaCentre(int idx, double* lat, double* lon) const {
  const MapArea* a = &areas[idx];
  // The middle of the box, in world pixels at zMin (a tile's far edge is x+1).
  const double wx = (a->xMin + a->xMax + 1) * 0.5 * MAP_TILE_PX;
  const double wy = (a->yMin + a->yMax + 1) * 0.5 * MAP_TILE_PX;
  mapWorldToLatLon(wx, wy, a->zMin, lat, lon);
}

void MapsApp::areaWhere(int idx, double lat, double lon, char* out, size_t n) const {
  if (idx < 0 || idx >= areaCount || !areas[idx].hasExtent) {
    snprintf(out, n, "no tiles found");
    return;
  }
  if (areaHolds(idx, lat, lon)) {
    snprintf(out, n, "here");
    return;
  }
  double cl = 0, co = 0;
  areaCentre(idx, &cl, &co);
  const int32_t a0 = mapDegToI7(lat), o0 = mapDegToI7(lon), a1 = mapDegToI7(cl), o1 = mapDegToI7(co);
  char d[16];
  unitsFmtDist(meshPosDistanceM(a0, o0, a1, o1), gUnits, d, sizeof(d));
  snprintf(out, n, "%s %s", d, meshPosCompass8(meshPosBearingDeg(a0, o0, a1, o1)));
}

// ---------------------------------------------------------------- pins on the card

void MapsApp::loadPins() {
  pinCount = 0;
  pinsTruncated = false;
  if (!pins) {
    return;
  }
  File f = SD.open(MAPS_PINS_FILE, FILE_READ);
  if (!f) {
    return;                      // no pins yet is the ordinary case, not an error
  }
  char line[MAP_PIN_LINE_MAX];
  size_t n = 0;
  int bad = 0;
  /* Read a byte at a time. The file is a few kilobytes at most and this needs no buffer of
   * its own; a line longer than the buffer is consumed to its end rather than being split
   * into two half-lines, one of which would parse as a different pin. */
  bool overlong = false;
  while (f.available()) {
    const int ch = f.read();
    if (ch < 0) {
      break;
    }
    if (ch == '\n' || ch == '\r') {
      if (n || overlong) {
        line[n] = '\0';
        if (pinCount >= MAPS_MAX_PINS) {
          /* 🛑 AND NOW NOTHING MAY SAVE. The file holds more pins than this app can hold, so
           * every later savePins() would write our 64 back over the user's 90 and silently
           * destroy the rest. A card edited on a computer is exactly how that happens. The
           * app stays usable read-only until the file is trimmed. */
          pinsTruncated = true;
        } else if (!overlong) {
          const int r = mapPinParseLine(line, &pins[pinCount]);
          if (r == 1) {
            pinCount++;
          } else if (r < 0) {
            bad++;
          }
        } else if (overlong) {
          bad++;
        }
      }
      n = 0;
      overlong = false;
      continue;
    }
    if (n + 1 < sizeof(line)) {
      line[n++] = (char)ch;
    } else {
      overlong = true;
    }
  }
  if ((n || overlong) && !overlong && pinCount < MAPS_MAX_PINS) {
    line[n] = '\0';               // a last line with no newline is still a line
    if (mapPinParseLine(line, &pins[pinCount]) == 1) {
      pinCount++;
    }
  }
  f.close();
  if (bad) {
    log_e("MAPS: %d unreadable line(s) in %s were skipped", bad, MAPS_PINS_FILE);
  }
}

void MapsApp::savePins() {
  if (!pins) {
    return;
  }
  if (pinsTruncated) {
    log_e("MAPS: %s holds more than %d pins - refusing to save over it",
          MAPS_PINS_FILE, MAPS_MAX_PINS);
    return;
  }
  if (!SD.exists(MAPS_ROOT)) {
    SD.mkdir(MAPS_ROOT);
  }
  /* Write to a temporary file and rename over the real one. A power cut in the middle of a
   * rewrite would otherwise leave a half-file — and this phone's power switch is a hard cut,
   * not a shutdown (README). Losing the last pin is a shrug; losing the list is a day. */
  const char tmp[] = MAPS_PINS_FILE ".new";
  SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) {
    log_e("MAPS: could not write %s", tmp);
    return;
  }
  static const char HDR[] = "# WiPhone map pins v1 - latI,lonI (1e-7 deg), shared waypoint id, name\n";
  size_t expect = strlen(HDR);
  f.print(HDR);
  char line[MAP_PIN_LINE_MAX];
  int written = 0;
  for (int i = 0; i < pinCount; i++) {
    const int len = mapPinFormatLine(&pins[i], line, sizeof(line));
    if (len > 0) {
      f.print(line);
      f.print("\n");
      expect += (size_t)len + 1;
      written++;
    }
  }
  f.flush();
  /* ⚠ THE TEST HAS TO BE ABOUT THE PINS. `f.size() > 0` is satisfied by the header comment
   * alone, so a card that accepted the first print and then failed every subsequent one
   * passed this check and was renamed over the real file — the exact loss the temp-file dance
   * exists to prevent. Compare against what we counted on the way in. */
  const size_t got = (size_t)f.size();
  f.close();
  if (got < expect) {
    log_e("MAPS: %s is %u bytes, expected %u - keeping the old file",
          tmp, (unsigned)got, (unsigned)expect);
    SD.remove(tmp);
    return;
  }
  SD.remove(MAPS_PINS_FILE);
  if (!SD.rename(tmp, MAPS_PINS_FILE)) {
    /* 🛑 THE OLD FILE IS ALREADY GONE AT THIS POINT, so a failed rename is not "no change",
     * it is "the pins are now only in a file called pins.txt.new". Write them again, straight
     * to the real path, rather than leaving a user to find that out in the woods. FAT rename
     * cannot overwrite, which is why the remove has to come first and why this exists. */
    log_e("MAPS: could not rename %s into place - writing directly", tmp);
    File d = SD.open(MAPS_PINS_FILE, FILE_WRITE);
    if (!d) {
      log_e("MAPS: and the direct write failed too - the pins are in %s", tmp);
      return;
    }
    d.print("# WiPhone map pins v1 - latI,lonI (1e-7 deg), shared waypoint id, name\n");
    for (int i = 0; i < pinCount; i++) {
      if (mapPinFormatLine(&pins[i], line, sizeof(line)) > 0) {
        d.print(line);
        d.print("\n");
      }
    }
    d.flush();
    d.close();
    SD.remove(tmp);
  }
  pinsDirty = false;
  log_d("MAPS: saved %d pin(s)", written);
}

// ---------------------------------------------------------------- the view

void MapsApp::saveView() {
  double lat = 0, lon = 0;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
  Preferences p;
  if (!p.begin(MAPS_NVS, false)) {
    return;
  }
  p.putInt("lat", mapDegToI7(lat));
  p.putInt("lon", mapDegToI7(lon));
  p.putInt("z", zoom);
  /* ⚠ memset, NOT just a terminator. putBytes writes all 32 bytes, and everything past the
   * NUL would otherwise be whatever was on this stack frame a moment ago — stack contents
   * committed to flash on every exit from the app, and a different 32 bytes each time, so the
   * NVS page churns for no reason too. */
  char area[32];
  memset(area, 0, sizeof(area));
  if (areaSel >= 0) {
    strlcpy(area, areas[areaSel].name, sizeof(area));
  }
  /* putBytes, not putString: a String here is a heap allocation on the internal heap, on the
   * teardown path, for a 31-byte name. Same reason loadPosSettings() uses getBytes. */
  p.putBytes("area", area, sizeof(area));
  p.putInt("saved", 1);
  p.end();
}

void MapsApp::restoreView() {
  /* WHERE THE MAP OPENS, in order:
   *   1. where it was last closed — that is what was asked for, and it is the only one of
   *      these that survives being somewhere with no signal and no sky;
   *   2. this phone's own GPS fix, if the receiver is on and has one;
   *   3. the mesh's reference place (a chosen waypoint, a fresh fix, or the manual pin) —
   *      resolveReference() already encodes that precedence and it is not re-litigated here;
   *   4. the middle of whatever tiles the card actually holds;
   *   5. 0,0, with the status line saying there is nothing to show.
   * Each step is only taken when the one before it has nothing to offer. */
  double lat = 0, lon = 0;
  bool have = false;
  int z = -1;
  char area[32];
  area[0] = '\0';

  Preferences p;
  if (p.begin(MAPS_NVS, true)) {
    if (p.getInt("saved", 0) == 1) {
      lat = mapI7ToDeg(p.getInt("lat", 0));
      lon = mapI7ToDeg(p.getInt("lon", 0));
      z = p.getInt("z", -1);
      p.getBytes("area", area, sizeof(area));
      area[sizeof(area) - 1] = '\0';
      have = true;
    }
    p.end();
  }

  // The remembered area, if it is still on the card.
  if (area[0] && areaCount > 0) {
    for (int i = 0; i < areaCount; i++) {
      if (!strcmp(areas[i].name, area)) {
        areaSel = i;
        break;
      }
    }
  }

  if (!have) {
    int32_t la = 0, lo = 0;
    SelfFix nw;
    if (meshService.getSelfNewest(&nw) != SELF_NONE) {
      // This phone's newest fix from a source that is on (its own GPS, or the hotspot phone's).
      lat = mapI7ToDeg(nw.latI);
      lon = mapI7ToDeg(nw.lonI);
      have = true;
      setNote(nw.src == SELF_PHONE ? "Opened on the phone's GPS fix"
              : nw.src == SELF_NET ? "Opened on the Wi-Fi location" : "Opened on the GPS fix");
    } else if (meshService.resolveReference(&la, &lo, NULL, 0)) {
      lat = mapI7ToDeg(la);
      lon = mapI7ToDeg(lo);
      have = true;
      setNote("Opened on the reference place");
    }
  }

  if (z < MAP_ZOOM_MIN || z > MAP_ZOOM_MAX) {
    z = -1;
  }
  if (areaSel >= 0) {
    const MapArea* a = &areas[areaSel];
    /* A remembered zoom one level PAST the tiles is kept: the user chose the stretched level
     * and closing the app is not a reason to undo it (map_tiles.h, MAP_OVERZOOM_MAX). */
    if (z < a->zMin || z > a->zMax + MAP_OVERZOOM_MAX) {
      /* No remembered zoom, or one this area does not have: start at the deepest level that
       * is not the very deepest, so there is somewhere to zoom in TO. */
      z = (a->zMax > a->zMin) ? a->zMax - 1 : a->zMax;
    }
  } else if (z < 0) {
    z = 15;
  }
  zoom = z;

  if (!have && areaSel >= 0) {
    /* The middle of the tiles that exist. Scanned at the SHALLOWEST zoom the area has, where
     * there are a handful of folders rather than thousands — the same ground, a hundredth of
     * the directory reads.
     *
     * ⚠ The vertical range comes from the FIRST column only, and both loops stop at 64
     * entries. That is deliberate: this runs once, on a screen the user is waiting for, and
     * it only has to land somewhere on the map — anything more precise would be a directory
     * walk of the whole card to save a press of the d-pad. An area whose columns have very
     * different row ranges opens a little off centre and never wrongly. */
    const MapArea* a = &areas[areaSel];
    char zpath[96];
    snprintf(zpath, sizeof(zpath), "%s/%s/%d", MAPS_ROOT, a->name, a->zMin);
    long xMin = -1, xMax = -1, yMin = -1, yMax = -1;
    File xd = SD.open(zpath);
    if (xd && xd.isDirectory()) {
      int seen = 0;
      File xf;
      while (seen < 64 && (xf = xd.openNextFile())) {
        const long col = xf.isDirectory() ? numericName(baseName(xf.name()), false) : -1;
        if (col < 0) {
          xf.close();
          continue;                    // not a tile column; it does not count against the 64
        }
        if (xMin < 0 || col < xMin) {
          xMin = col;
        }
        if (col > xMax) {
          xMax = col;
        }
        if (yMin < 0) {
          char ypath[160];
          snprintf(ypath, sizeof(ypath), "%s/%ld", zpath, col);
          File yd = SD.open(ypath);
          if (yd && yd.isDirectory()) {
            int ys = 0;
            File yf;
            while (ys < 64 && (yf = yd.openNextFile())) {
              const long row = yf.isDirectory() ? -1 : numericName(baseName(yf.name()), true);
              if (row >= 0) {
                if (yMin < 0 || row < yMin) {
                  yMin = row;
                }
                if (row > yMax) {
                  yMax = row;
                }
                ys++;
              }
              yf.close();
            }
          }
          if (yd) {
            yd.close();
          }
        }
        seen++;
        xf.close();
      }
    }
    if (xd) {
      xd.close();
    }
    if (xMin >= 0 && yMin >= 0) {
      const double wx = ((double)xMin + (double)xMax + 1.0) * 0.5 * MAP_TILE_PX;
      const double wy = ((double)yMin + (double)yMax + 1.0) * 0.5 * MAP_TILE_PX;
      mapWorldToLatLon(wx, wy, a->zMin, &lat, &lon);
      have = true;
      setNote("Opened on the middle of the map");
    }
  }

  double wx = 0, wy = 0;
  mapLatLonToWorld(lat, lon, zoom, &wx, &wy);
  cx = (int32_t)(wx + 0.5);
  cy = (int32_t)(wy + 0.5);
  mapClampView(zoom, vpW, vpH, &cx, &cy);
  (void)have;
}

// ---------------------------------------------------------------- tile cache

int MapsApp::findSlot(int z, int tx, int ty) const {
  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    if (slots[i].px && slots[i].z == z && slots[i].tx == tx && slots[i].ty == ty) {
      return i;
    }
  }
  return -1;
}

int MapsApp::claimSlot() {
  int best = -1;
  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    if (!slots[i].px) {
      continue;
    }
    if (loadActive && i == loadSlot) {
      continue;                  // never evict the tile currently being read into
    }
    if (slots[i].z < 0) {
      return i;                  // an empty slot is always the right victim
    }
    if (best < 0 || (int32_t)(slots[i].used - slots[best].used) < 0) {
      best = i;                  // signed difference: `used` is a counter and counters wrap
    }
  }
  return best;
}

bool MapsApp::isKnownMissing(int z, int tx, int ty) const {
  return mapMissFind(misses, missCount, z, tx, ty) >= 0;
}

void MapsApp::rememberMissing(int z, int tx, int ty, bool bad) {
  if (isKnownMissing(z, tx, ty)) {
    return;
  }
  /* The entry walked past LONGEST ago goes, so never one on the screen now (mapMissSlot). A
   * ring evicting in order of arrival throws out holes still in view once a deep view far from
   * the downloads needs more than it holds, and each is then re-asked, redrawn, re-asked. */
  const int i = mapMissSlot(misses, missCount, MAPS_MISS_CACHE);
  if (i < 0) {
    return;
  }
  if (i >= missCount) {
    missCount = i + 1;
  }
  misses[i].z = (int8_t)z;
  misses[i].tx = tx;
  misses[i].ty = ty;
  misses[i].bad = bad ? 1 : 0;
  misses[i].hit = drawSeq;       // the frame that wanted it is the frame on the screen
}

void MapsApp::forgetMissing() {
  missCount = 0;
}

int MapsApp::forgetMissingLevels(int zLo, int zHi) {
  int n = 0;
  for (int i = 0; i < missCount; i++) {
    if (misses[i].z >= zLo && misses[i].z <= zHi) {
      continue;
    }
    misses[n++] = misses[i];
  }
  const int gone = missCount - n;
  missCount = n;
  return gone;
}

/* A download writing into the area on the screen turns holes into tiles, and a known-missing
 * hole is never asked for again — so without this the map goes on drawing the parent stretched,
 * and saying so, over tiles that are on the card now. Called at most every MAPS_JOB_POLL_MS.
 * Only the levels written since the last look are forgotten (a job goes level by level, from
 * TILE_ZOOM_BASE up): forgetting everything would re-ask the card for every hole of every
 * position every ten seconds, most of them still holes. */
bool MapsApp::jobTilesLanded() {
  if (areaSel < 0) {
    return false;
  }
  tileFetchStatus(&jobSt);
  /* Case-blind, as the card is: a relay told to fill "OTM" writes into the folder "otm". */
  if (!(jobSt.active || jobSt.finished) || strcasecmp(jobSt.source, areas[areaSel].name) != 0) {
    jobSeen = false;             // no job, or it is filling another folder
    return false;
  }
  /* A run this map did not watch from its last look — the first sighting, another job (a new
   * total), a restart (done went back) or one that began since — may have written anything
   * from TILE_ZOOM_BASE up to where it is now. */
  const bool fresh = !jobSeen || jobSt.total != jobSeenTotal || jobSt.done < jobSeenDone ||
                     (jobSt.active && !jobSeenActive);
  const bool wrote = fresh ? (jobSt.done > 0) : (jobSt.done != jobSeenDone);
  int zLo = fresh ? TILE_ZOOM_BASE : jobSeenZ;
  int zHi = jobSt.curZ;
  if (!fresh && jobSt.pass != jobSeenPass) {
    /* The automatic second pass began since the last look: pass 1 finished every level up to
     * the job's depth, and pass 2 started again at TILE_ZOOM_BASE and may already be back at
     * the top — either may have written anything. Keyed on the pass itself, not on the order
     * of the two levels: a pass 2 with few holes climbs back inside one poll. Once a run. */
    zLo = TILE_ZOOM_BASE;
    zHi = MAP_ZOOM_MAX;
  } else if (zLo > zHi) {
    zLo = TILE_ZOOM_BASE;          // went back to the coarse levels: anything up to where it had been
    zHi = jobSeenZ;
  }
  /* A job writing a level DEEPER than this area was scanned to (the first z17 run over a z16
   * area) makes that level exist only now. The walk and the zoom-in ceiling both stop at the
   * scanned zMax, so without this the map would go on stretching z16 over z17 tiles on the
   * card until a rescan. One folder check per poll, only while that is true. */
  bool deeper = false;
  if (jobSt.curZ > areas[areaSel].zMax && jobSt.curZ <= MAP_ZOOM_MAX) {
    char dir[72];
    snprintf(dir, sizeof(dir), "/sd%s/%s/%d", MAPS_ROOT, areas[areaSel].name, jobSt.curZ);
    struct stat sb;              // a POSIX stat on the card's mount: SD.exists() logs every miss
    if (::stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode)) {
      areas[areaSel].zMax = jobSt.curZ;
      deeper = true;
    }
  }
  jobSeen = true;
  jobSeenActive = jobSt.active;
  jobSeenDone = jobSt.done;
  jobSeenZ = jobSt.curZ;
  jobSeenPass = jobSt.pass;
  jobSeenTotal = jobSt.total;
  /* A redraw only when a hole at those levels was forgotten: the job writing somewhere this
   * map has not asked about changes nothing on the screen. */
  return (wrote && forgetMissingLevels(zLo, zHi) > 0) || deeper;
}

bool MapsApp::startLoad(int z, int tx, int ty) {
  /* ⚠ isKnownMissing IS PART OF THE REFUSAL, not just a drawing hint. drawMap never wants a
   * known-missing tile, but a want can go stale between the draw and the tick, and without
   * this a tile that is not on the card would be opened, fail, redraw and be opened again —
   * an SD open, a failure and a log line each time, on a phone in somebody's pocket. */
  if (loadActive || tileMemFail || areaSel < 0 || isKnownMissing(z, tx, ty)) {
    return false;
  }
  /* 🛑 ...AND A TILE ALREADY IN A SLOT IS NOT READ AGAIN. The want is rebuilt only by drawMap,
   * and while the phone is LOCKED the redraw after LOAD_DONE goes to the clock, not here — so
   * the stale want named the tile just finished, and each 25 ms tick read the same 128 KB into
   * another slot, in the pocket, until someone unlocked it (review, 2026-09-23). Refusing here
   * sends the tick down its refused-start path, which forgets the want and stands it down. */
  if (findSlot(z, tx, ty) >= 0) {
    return false;
  }
  const int s = claimSlot();
  if (s < 0) {
    return false;
  }
  /* Claim the slot BEFORE the first byte arrives, marked not-ready. findSlot() then reports
   * the tile as "being fetched", which is what stops the next repaint from starting a second
   * load for the same tile and thrashing the card. */
  slots[s].z = z;
  slots[s].tx = tx;
  slots[s].ty = ty;
  slots[s].ready = false;
  slots[s].used = ++useSeq;
  loadSlot = s;
  loadZ = z;
  loadTx = tx;
  loadTy = ty;
  loadGot = 0;
  loadRetries = 0;
  loadActive = true;
  return true;
}

void MapsApp::cancelLoad() {
  /* The want was computed for the view being abandoned, so it is abandoned with it. Leaving it
   * set means the very next tick restarts the load this call exists to stop — a zoom key would
   * cancel a tile and immediately fetch it again for a zoom nobody is looking at. */
  wantAny = false;
  if (!loadActive) {
    return;
  }
  if (loadSlot >= 0 && loadSlot < MAPS_TILE_SLOTS) {
    slots[loadSlot].z = -1;      // a half-read tile is not a tile
    slots[loadSlot].ready = false;
  }
  loadActive = false;
  loadSlot = -1;
  loadGot = 0;
}

/* A read or seek that failed part-way. ⚠ NOT THE SAME THING AS "not on the card": the file
 * opened and its length was right, so this is the card or the bus having a moment — and the
 * card in this phone is one the README already warns can throw write errors under load. A
 * blacklist is for the session, so a single hiccup would leave a grey square in the middle of
 * the map until the app was closed and reopened. One retry from the top first (LOAD_PIECE);
 * a second failure is real and gets the blacklist as BAD (LOAD_MISS): the position falls back
 * to its parent, and the chip says "card read error" rather than passing it off as stretching. */
int MapsApp::tileReadFailed(const char* what) {
  if (loadRetries < 1) {
    loadRetries++;
    loadGot = 0;                     // start this tile again rather than trusting a torn read
    log_e("MAPS: tile %d/%d/%d %s failed - one more go", loadZ, loadTx, loadTy, what);
    return LOAD_PIECE;               // still loadActive: the next tick picks it up
  }
  log_e("MAPS: tile %d/%d/%d %s failed twice - giving up on it", loadZ, loadTx, loadTy, what);
  rememberMissing(loadZ, loadTx, loadTy, true);
  cancelLoad();
  return LOAD_MISS;
}

/* 🛑 A MISS REDRAWS (the caller returns REDRAW_SCREEN for LOAD_MISS in MAPS_VIEW). drawMap is
 * the only place a want is chosen, and a stretched position finds its ancestor one level per
 * miss. Without the repaint, cancelLoad() has cleared the want and nothing asks again: the
 * screen sits grey saying "4 tiles..." until a key is pressed — at the edge of an area's
 * tiles as much as under a stretch. */
int MapsApp::tileLoadStep() {
  if (!loadActive || loadSlot < 0 || !slots[loadSlot].px) {
    loadActive = false;
    return LOAD_PIECE;
  }
  char path[160];
  if (areaSel < 0 ||
      !mapTilePath(path, sizeof(path), MAPS_ROOT, areas[areaSel].name, loadZ, loadTx, loadTy)) {
    rememberMissing(loadZ, loadTx, loadTy, false);
    cancelLoad();
    return LOAD_MISS;
  }
  File f = SD.open(path, FILE_READ);
  if (!f) {
    rememberMissing(loadZ, loadTx, loadTy, false);
    cancelLoad();
    return LOAD_MISS;
  }
  /* 🛑 THE LENGTH IS THE WHOLE FORMAT CHECK, AND IT IS CHECKED ON EVERY PIECE. A raw tile has
   * no header, no magic and no way to be self-describing: a PNG that was copied without being
   * converted is simply a file of the wrong length, and if it were read anyway the screen
   * would fill with the PNG's compressed bytes as colour. The check is cheap and it is the
   * only thing standing between a mistake at the computer and a screenful of confetti. */
  if ((size_t)f.size() != MAP_TILE_BYTES) {
    log_e("MAPS: %s is %u bytes, expected %u - not a raw tile",
          path, (unsigned)f.size(), (unsigned)MAP_TILE_BYTES);
    f.close();
    rememberMissing(loadZ, loadTx, loadTy, true);   // there, but not a tile: BAD, not absent
    cancelLoad();
    return LOAD_MISS;
  }
  if (loadGot && !f.seek(loadGot)) {
    f.close();
    return tileReadFailed("seek");
  }
  size_t want = MAP_TILE_BYTES - loadGot;
  if (want > MAPS_CHUNK_BYTES) {
    want = MAPS_CHUNK_BYTES;
  }
  const int got = f.read((uint8_t*)slots[loadSlot].px + loadGot, want);
  f.close();
  if (got <= 0) {
    log_e("MAPS: %s read failed at %u", path, (unsigned)loadGot);
    return tileReadFailed("read");
  }
  loadGot += (uint32_t)got;
  if (loadGot >= MAP_TILE_BYTES) {
    slots[loadSlot].ready = true;
    if (wantAny && wantZ == loadZ && wantTx == loadTx && wantTy == loadTy) {
      wantAny = false;               // the want was this tile: it is satisfied (see startLoad)
    }
    loadActive = false;
    loadSlot = -1;
    loadGot = 0;
    return LOAD_DONE;
  }
  return LOAD_PIECE;
}

// ---------------------------------------------------------------- drawing primitives

void MapsApp::fillClipped(int x, int y, int w, int h, uint16_t colour) {
  int x0 = x < vpX ? vpX : x;
  int y0 = y < vpY ? vpY : y;
  int x1 = x + w, y1 = y + h;
  if (x1 > vpX + vpW) {
    x1 = vpX + vpW;
  }
  if (y1 > vpY + vpH) {
    y1 = vpY + vpH;
  }
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  lcd.fillRect(x0, y0, x1 - x0, y1 - y0, colour);
}

void MapsApp::drawBlob(int x, int y, int r, uint16_t colour, bool diamond) {
  if (r < 0) {
    return;
  }
  for (int dy = -r; dy <= r; dy++) {
    const int ady = dy < 0 ? -dy : dy;
    int half = diamond ? (r - ady) : r;
    if (!diamond && r >= 2 && ady == r) {
      half = r - 1;              // clip the corners so a "dot" reads as round, not square
    }
    if (half < 0) {
      continue;
    }
    fillClipped(x - half, y + dy, 2 * half + 1, 1, colour);
  }
}

/* Claim a rectangle so no later label lands on it. Silently ignored once the table is full —
 * drawLabel refuses to place anything at that point anyway. */
void MapsApp::reserveBox(int x, int y, int w, int h) {
  if (labelCount >= (int)(sizeof(labelRects) / sizeof(labelRects[0]))) {
    return;
  }
  labelRects[labelCount].x = (int16_t)x;
  labelRects[labelCount].y = (int16_t)y;
  labelRects[labelCount].w = (int16_t)w;
  labelRects[labelCount].h = (int16_t)h;
  labelCount++;
}

void MapsApp::drawMarker(int vx, int vy, uint16_t colour, int shape) {
  const bool diamond = (shape == MAP_SHAPE_DIAMOND);
  drawBlob(vx, vy, 4, BLACK, diamond);     // the surround: markers sit on unknown colours
  drawBlob(vx, vy, 3, colour, diamond);
  /* ⚠ A LABEL MUST NEVER COVER A MARKER — least of all the one it is naming. The label boxes
   * are opaque black, so without this a name sitting slightly left of its neighbour's dot
   * erases that dot, and the map loses a person while gaining a word. */
  reserveBox(vx - 4, vy - 4, 9, 9);
}

bool MapsApp::drawLabel(int vx, int vy, const char* text, uint16_t colour) {
  if (!text || !text[0]) {
    return false;
  }
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (!fnt) {
    return false;
  }
  lcd.setTextFont(fnt);
  const int tw = lcd.textWidth(text);
  const int th = (int)fnt->height();
  int bx = vx + 7;
  const int by = vy - th / 2;
  if (bx + tw + 4 > vpX + vpW) {
    bx = vx - 7 - (tw + 4);      // no room on the right: put it on the left
  }
  /* ⚠ A THIRD TRY BEFORE GIVING UP. A label wider than about half the screen fits on NEITHER
   * side of a marker near the middle, so the all-or-nothing rule below silently created a
   * dead band through the centre of the map where long names never appeared — and the centre
   * is where the crosshair is, which is where people put the thing they are looking at.
   * Sliding it flush against the edge keeps it readable and still inside the viewport. */
  if (bx < vpX) {
    bx = vpX;
  }
  if (bx + tw + 4 > vpX + vpW) {
    bx = vpX + vpW - (tw + 4);
  }
  /* ⚠ ALL OR NOTHING. drawString cannot be clipped — it would write straight into the header
   * or the footer — so a label that does not fit entirely inside the map is not drawn at all.
   * Half a name is not information anyway. */
  if (bx < vpX || by < vpY || bx + tw + 4 > vpX + vpW || by + th + 2 > vpY + vpH) {
    return false;
  }
  const int w = tw + 4, h = th + 2;
  /* 🛑 ONCE THE TABLE IS FULL, NOTHING MORE IS DRAWN. The old code kept drawing labels it
   * could no longer record, so every one past the fourteenth was placed with no collision
   * test at all — the overprinting this whole mechanism exists to prevent, arriving exactly
   * when the screen is busiest and needs it most. */
  if (labelCount >= (int)(sizeof(labelRects) / sizeof(labelRects[0]))) {
    return false;
  }
  // Would it land on something already there — a label, or a marker? Then it is not placed.
  for (int i = 0; i < labelCount; i++) {
    const LabelRect* r = &labelRects[i];
    if (bx < r->x + r->w && bx + w > r->x && by < r->y + r->h && by + h > r->y) {
      return false;
    }
  }
  reserveBox(bx, by, w, h);
  lcd.fillRect(bx, by, w, h, BLACK);
  lcd.setTextColor(colour, BLACK);
  lcd.drawString(text, bx + 2, by + 1);
  return true;
}

void MapsApp::drawCrosshair() {
  const int mx = vpX + vpW / 2, my = vpY + vpH / 2;
  for (int pass = 0; pass < 2; pass++) {
    const uint16_t c = pass ? MAP_C_ME : BLACK;
    const int t = pass ? 1 : 3;            // a black bed under a white line: legible on anything
    const int ext = pass ? 0 : 1;
    fillClipped(mx - 15 - ext, my - t / 2, 10 + 2 * ext, t, c);
    fillClipped(mx + 6 - ext,  my - t / 2, 10 + 2 * ext, t, c);
    fillClipped(mx - t / 2, my - 15 - ext, t, 10 + 2 * ext, c);
    fillClipped(mx - t / 2, my + 6 - ext,  t, 10 + 2 * ext, c);
  }
}

// ---------------------------------------------------------------- the map itself

void MapsApp::drawMap() {
  /* The byte order of a raw tile is the phone's own: RGB565 little-endian, exactly what
   * color565() produces — which is NOT what a sprite stores (a sprite keeps every pixel
   * byte-swapped, see lcdNativePixels in GUI.h). The first cut of this function set both swap
   * flags FALSE with a comment asserting the opposite, and every tile would have reached the
   * glass with its bytes exchanged. The flag is raised only around the row pushes below. */
  wantAny = false;
  pendingTiles = 0;
  statusLine[0] = '\0';
  statusIsStretch = false;
  stretchAny = false;
  stretchMinL = stretchMaxL = 0;
  readErrorInView = false;
  drawSeq++;

  /* ── THE STRETCH (app_maps.h, map_tiles.h) ────────────────────────────────────────────
   * The blits are worked out at the VIEW zoom, as if every position's own tile were on the
   * card — the same proven function every frame has always used — and each position is then
   * drawn from the deepest level that IS there (mapAncestorSrc). One path for all of it: a
   * real tile is d = 0, one level past the area's deepest tiles is d = 1 from the first level
   * tried, and a hole far from the downloads is whatever d it takes, down to
   * MAP_ANCESTOR_MAX_D. Exact at every d: the centre is never rounded to another level, so the
   * stretched ground sits under the crosshair, the pins and the scale bar to the pixel. */
  const int n = mapViewBlits(zoom, cx, cy, vpW, vpH, viewBlits, MAP_MAX_BLITS);
  /* The void behind the tiles is painted only where a tile cannot be: the blits tile the
   * viewport completely except past the world's north or south edge, and filling 60,000
   * pixels of PSRAM sprite to paint them over again cost 15 ms of every frame (measured
   * 2026-09-20, a seventh of the frame). A missing or unread tile is filled in its own
   * colour below, so nothing is left unpainted. */
  long covered = 0;
  for (int i = 0; i < n; i++) {
    covered += (long)viewBlits[i].w * viewBlits[i].h;
  }
  /* ...and always when the tile loop below will not run at all (no area on the card: the
   * map is drawn blank behind the markers) — the memory-failure branch paints its own. */
  if (covered != (long)vpW * vpH || n <= 0 || areaSel < 0) {
    lcd.fillRect(vpX, vpY, vpW, vpH, MAP_C_VOID);
  }

  int haveTiles = 0;
  if (n > 0 && areaSel >= 0 && !tileMemFail) {
    const MapArea* a = &areas[areaSel];
    /* The levels a position may be drawn from: its own zoom, or the deepest the area has if
     * the view is past it; down to the coarsest the area has, but never more than
     * MAP_ANCESTOR_MAX_D levels up (256 >> d must stay >= 1: map_tiles.h). */
    const int topL = zoom < a->zMax ? zoom : a->zMax;
    const int floorL = (zoom - MAP_ANCESTOR_MAX_D > a->zMin) ? zoom - MAP_ANCESTOR_MAX_D : a->zMin;
    for (int i = 0; i < n; i++) {
      const MapBlit* q = &viewBlits[i];
      /* THE WALK, finest level first. The first READY slot is drawn and ends it. A level known
       * missing is passed (its entry stamped: it is on the screen, so it must not be evicted).
       * The first level that is neither is this position's TARGET: wanted if no slot holds it
       * yet, being read if one does — and a coarser level is never wanted while it is, so an
       * ancestor is read only once every finer level is known missing. The walk goes on past
       * the target only to find something ready to show meanwhile. */
      int drawSi = -1, drawL = -1;
      bool pending = false;
      for (int L = topL; L >= floorL; L--) {
        const int d = zoom - L;
        const int ax = q->tileX >> d, ay = q->tileY >> d;
        const int si = findSlot(L, ax, ay);
        if (si >= 0 && slots[si].ready) {
          drawSi = si;
          drawL = L;
          break;
        }
        const int mi = mapMissFind(misses, missCount, L, ax, ay);
        if (mi >= 0) {
          misses[mi].hit = drawSeq;
          if (misses[mi].bad) {
            readErrorInView = true;
          }
          continue;
        }
        if (!pending) {
          pending = true;
          if (si < 0 && !wantAny) {
            wantAny = true;
            wantZ = L;
            wantTx = ax;
            wantTy = ay;
          }
        }
      }
      MapAncestorBlit ab;
      if (drawSi >= 0 && mapAncestorSrc(q, zoom - drawL, &ab)) {
        /* 🛑 EVERY SLOT DRAWN IS BUMPED, ancestor or not. An unbumped ancestor is the LRU
         * victim of the next load, so two positions showing two ancestors would evict each
         * other's by turns forever: 128 KB of reads per tile, forty times a second. */
        slots[drawSi].used = ++useSeq;
        haveTiles++;             // drawn from ANY level: a stretched map is not "no tiles here"
        if (drawL < zoom) {
          /* Stretched — including while this position's own tile is still on its way: this
           * is what is on the screen, so this is what the chip says. */
          if (!stretchAny || drawL < stretchMinL) {
            stretchMinL = drawL;
          }
          if (!stretchAny || drawL > stretchMaxL) {
            stretchMaxL = drawL;
          }
          stretchAny = true;
        }
        /* 🛑 NON-CONST, ON PURPOSE. TFT_eSprite::pushImage has a `const uint16_t*` overload and
         * it is the PROGMEM one: it swaps the bytes when the flag says NOT to, and clips
         * nothing. A `const` row pointer selects it silently and every tile reaches the glass
         * byte-swapped — a recognisable map in the wrong colours. Rows pushed from here are
         * `uint16_t*` (the slot's own pixels, or wideRow). */
        uint16_t* tile = slots[drawSi].px;
        lcdNativePixels(lcd, true);
        if (ab.scale == 1) {
          /* Row at a time. pushImage's `w` is both the width AND the source stride, so a
           * sub-rectangle of a 256-wide tile cannot be pushed in one call; a row can, and the
           * source pointer is the row's start. Every coordinate here is already inside both the
           * tile and the viewport — mapViewBlits guarantees it, and test_maptiles proves it.
           * (Tried 2026-09-20: swapping once at load and memcpy-ing rows here saved nothing
           * measurable — the ~18 ms these rows cost a frame is PSRAM-to-PSRAM bandwidth, not
           * the swap. Left as it was.) */
          for (int r = 0; r < q->h; r++) {
            uint16_t* row = tile + (size_t)(ab.sy0 + r) * MAP_TILE_PX + ab.sx0;
            lcd.pushImage(vpX + q->dstX, vpY + q->dstY + r, (uint16_t)q->w, 1, row);
          }
        } else {
          /* One ancestor row, widened into `wideRow` (mapAncestorRow, the phase-cut first
           * column included), then pushed once for each screen row it covers. The piece lies
           * inside the viewport at the view zoom already, so nothing here needs clipping and
           * nothing starts at a negative x or y. */
          const int cap = q->w < TFT_WIDTH ? q->w : TFT_WIDTH;
          for (int r = 0; r < ab.sh; r++) {
            int y0 = 0;
            const int rows = mapAncestorRow(&ab, tile, r, wideRow, cap, &y0);
            for (int k = 0; k < rows; k++) {
              lcd.pushImage(vpX + q->dstX, vpY + q->dstY + y0 + k, (uint16_t)cap, 1, wideRow);
            }
          }
        }
        lcdNativePixels(lcd, false);
      } else {
        /* Nothing ready at any level. LOADING while some level is still to be asked for or is
         * on its way; NOTILE once every level down to the floor is known missing. */
        fillClipped(vpX + q->dstX, vpY + q->dstY, q->w, q->h,
                    pending ? MAP_C_LOADING : MAP_C_NOTILE);
        if (pending) {
          pendingTiles++;
        }
      }
    }
  } else if (n > 0 && areaSel >= 0 && tileMemFail) {
    lcd.fillRect(vpX, vpY, vpW, vpH, MAP_C_NOTILE);
  }

  /* One sentence, most important first. A position drawn from an ancestor counts as HAVING a
   * tile and not as pending, so a view drawn entirely from stretched tiles says what it is
   * stretching rather than "no tiles here" or "4 tiles...". A tile the card could not read
   * outranks the stretch label: a failing card must not pass for ordinary stretching. */
  if (tileMemFail) {
    strlcpy(statusLine, "no memory for tiles", sizeof(statusLine));
  } else if (areaSel < 0) {
    strlcpy(statusLine, "no map on the card", sizeof(statusLine));
  } else if (pendingTiles > 0) {
    snprintf(statusLine, sizeof(statusLine), "%d tile%s...", pendingTiles,
             pendingTiles == 1 ? "" : "s");
  } else if (readErrorInView) {
    strlcpy(statusLine, "card read error", sizeof(statusLine));
  } else if (haveTiles == 0) {
    strlcpy(statusLine, "no tiles here", sizeof(statusLine));
  } else if (stretchAny) {
    statusIsStretch = mapStretchLabel(statusLine, sizeof(statusLine), zoom,
                                      stretchMinL, stretchMaxL, 0) > 0;
  }

  drawOverlays();
}

/* Where this phone thinks it is, for the marker and for "centre on me". Returns MAPS_SELF_NONE
 * when there is nothing to draw, MAPS_SELF_GPS for a live measured fix (this phone's GPS or the
 * hotspot phone's: *src says which), MAPS_SELF_PIN for the user's declared pin, then the poor
 * and the old fix. One function so the marker, the label and the key never disagree. */
int MapsApp::selfPosition(int32_t* latI, int32_t* lonI, uint32_t* ageMs, int* src,
                          uint16_t* accM) const {
  if (src) {
    *src = SELF_NONE;
  }
  if (accM) {
    *accM = 0;
  }
  /* ⚠ THE SAME BAR THE DISTANCE LINES USE: meshService.getSelfFix() is self_pos.h's selfPick,
   * which resolveReference() and the beacon ask too. It refuses a fix under four satellites or
   * over HDOP 10 (meshPosFixUsable — the "twenty kilometres wrong" fix), and a "centre on me"
   * that trusted one would draw a confident white ring about YOU on the wrong ridge.
   * 🛑 AND IT JUDGES AGE: a fix from hours ago drawn as a live white ring is the same lie the
   * grey node dots exist to avoid, and worse, because it is a lie about YOU — a person checking
   * they are on the right ridge would be looking at where they were before the canopy closed.
   * MESH_GPS_FRESH_MS is the service's own bar; past it the ring goes grey and wears its age.
   *
   * The order after that is resolveReference()'s, deliberately: a live fix beats everything,
   * then the pin the user declared by hand, then the poor or stale fix. A phone with a pin set
   * this morning and a fix from yesterday should show the pin. */
  SelfFix fx;
  if (meshService.getSelfFix(&fx, false) != SELF_NONE) {
    if (latI) {
      *latI = fx.latI;
    }
    if (lonI) {
      *lonI = fx.lonI;
    }
    if (ageMs) {
      *ageMs = fx.ageMs;
    }
    if (src) {
      *src = fx.src;
    }
    if (accM) {
      *accM = fx.accM;                 // a Wi-Fi fix's size (self_pos.h); 0 for GPS / phone
    }
    return MAPS_SELF_GPS;
  }
  if (meshService.getMyPin(latI, lonI, NULL)) {
    if (ageMs) {
      *ageMs = 0;
    }
    return MAPS_SELF_PIN;
  }
  /* Nothing live and no pin: the NEWEST fix of a source that is on, whatever its age or
   * quality — fresh but poor (grey, saying why) or simply old (grey, wearing its age). */
  if (meshService.getSelfNewest(&fx) == SELF_NONE) {
    return MAPS_SELF_NONE;
  }
  if (fx.src == SELF_NET) {
    /* A Wi-Fi fix is never drawn grey: past its bar it is GONE (self_pos.h - selfNewest already
     * skips an expired one), and one that is not LIVE above is not a fix worth a marker. */
    return MAPS_SELF_NONE;
  }
  if (latI) {
    *latI = fx.latI;
  }
  if (lonI) {
    *lonI = fx.lonI;
  }
  if (src) {
    *src = fx.src;
  }
  if (fx.state == SELF_ST_POOR) {
    if (ageMs) {
      /* For this kind the "age" slot carries the REASON: sats in the low byte, HDOP x10
       * above it. Whichever failed the bar is what the label names. */
      *ageMs = ((uint32_t)(fx.hdopX10 < 0 ? 0 : fx.hdopX10) << 8) | (uint32_t)(fx.sats & 0xFF);
    }
    return MAPS_SELF_POOR_GPS;
  }
  if (ageMs) {
    *ageMs = fx.ageMs;
  }
  return MAPS_SELF_OLD_GPS;
}

/* A dotted ring, clipped to the map like every marker (fillClipped): the midpoint circle, every
 * other step, each dot 2x2 with its colour over a black one offset by a pixel, so it reads on a
 * pale tile and a dark one alike. ~5.7 r dots a ring; the caller keeps r within the screen. */
void MapsApp::drawRing(int x, int y, int r, uint16_t colour) {
  if (r < 2) {
    return;
  }
  int dx = r, dy = 0, err = 1 - r, step = 0;
  while (dx >= dy) {
    if ((step++ & 1) == 0) {
      const int px[8] = { x + dx, x - dx, x + dx, x - dx, x + dy, x - dy, x + dy, x - dy };
      const int py[8] = { y + dy, y + dy, y - dy, y - dy, y + dx, y + dx, y - dx, y - dx };
      for (int k = 0; k < 8; k++) {
        fillClipped(px[k], py[k], 2, 2, BLACK);
        fillClipped(px[k], py[k], 1, 1, colour);
      }
    }
    dy++;
    if (err < 0) {
      err += 2 * dy + 1;
    } else {
      dx--;
      err += 2 * (dy - dx) + 1;
    }
  }
}

/* ── TWO PASSES, AND THE ORDERS ARE DELIBERATELY OPPOSITE ─────────────────────────────────
 * Markers are drawn back to front, so the one you most need to see ends up on top: other
 * people, then the mesh's places, then your own pins, then this phone.
 *
 * Labels are drawn afterwards in the REVERSE order, because the first label placed wins the
 * space (drawLabel drops one that would overlap). With eight places and twenty nodes in view
 * on a 240x250 screen, unmanaged labels overprint into a grey smear and the map stops
 * answering the question it exists to answer. A dropped label still leaves its MARKER, which
 * is the part that says where somebody is, and the name is on the Nodes list regardless.
 *
 * Node order is the service's own display order — starred first, then most recently heard —
 * so the people you marked as mattering get their names placed before strangers do. */
void MapsApp::drawOverlays() {
  resetLabels();
  /* The crosshair is painted last (it must be on top) but claims its space first: it is the
   * one thing on this screen that is always exactly where the user is looking, and a name
   * printed under it is a name obscuring the answer to "what is here". */
  reserveBox(vpX + vpW / 2 - 16, vpY + vpH / 2 - 16, 33, 33);

  for (int i = 0; i < pinCount && pins; i++) {
    mapLatLonToView(zoom, cx, cy, vpW, vpH,
                    mapI7ToDeg(pins[i].latI), mapI7ToDeg(pins[i].lonI),
                    &pinVx[i], &pinVy[i]);
  }

  const uint32_t me = meshService.getMyNodeNum();
  const int nodeN = meshService.getNodeCount();
  const int wpN = meshService.getWaypointCount();
  const uint32_t now = millis();
  int32_t selfLat = 0, selfLon = 0;
  uint32_t selfAge = 0;
  int selfSrc = SELF_NONE;
  uint16_t selfAcc = 0;
  const int selfKind = selfPosition(&selfLat, &selfLon, &selfAge, &selfSrc, &selfAcc);

  if (measuring) {
    /* The anchor and a dashed line to the crosshair. The anchor may be far off-screen —
     * mapLatLonToView still returns its coordinates — so the line runs between the two points
     * as they are and the part inside the viewport is what appears. Dashed by drawing every
     * other 6 px step, counted from the anchor. 🛑 The steps worth drawing are found by
     * clipping the line to the viewport first, and each point is an int64 product
     * (mapDashRange / mapDashAt): in int32, `dx * s` overflows for an anchor 46 km away at
     * z18, and walking all |dx|/6 steps is 413k a frame at 1000 km. */
    int ax = 0, ay = 0;
    mapLatLonToView(zoom, cx, cy, vpW, vpH, mapI7ToDeg(measLatI), mapI7ToDeg(measLonI), &ax, &ay);
    const int x0 = vpX + ax, y0 = vpY + ay, x1 = vpX + vpW / 2, y1 = vpY + vpH / 2;
    int64_t sFirst = 0, sEnd = 0;
    const int64_t steps = mapDashRange(x0, y0, x1, y1, vpX, vpY, vpW, vpH, 6, &sFirst, &sEnd);
    for (int64_t s = sFirst; s < sEnd; s += 2) {
      int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
      mapDashAt(x0, y0, x1, y1, s, steps, &sx0, &sy0);
      mapDashAt(x0, y0, x1, y1, s + 1, steps, &sx1, &sy1);
      /* ⚠ ONLY SEGMENTS WITH BOTH ENDS INSIDE THE VIEWPORT: TFT_eSprite::drawLine clips
       * per pixel on the SPRITE, but the sprite is the whole screen, so a segment over the
       * header or the footer would be drawn on top of them. Clip to the map here. */
      if (sx0 >= vpX && sx0 < vpX + vpW && sy0 >= vpY && sy0 < vpY + vpH &&
          sx1 >= vpX && sx1 < vpX + vpW && sy1 >= vpY && sy1 < vpY + vpH) {
        lcd.drawLine(sx0, sy0, sx1, sy1, BLACK);
        lcd.drawLine(sx0, sy0 - 1, sx1, sy1 - 1, MAP_C_ME);
      }
    }
    if (ax >= 0 && ax < vpW && ay >= 0 && ay < vpH) {
      drawMarker(vpX + ax, vpY + ay, MAP_C_ME, MAP_SHAPE_DIAMOND);
      reserveBox(vpX + ax - 6, vpY + ay - 6, 13, 13);
    }
  }

  // ── pass 1: markers, back to front ───────────────────────────────────────────────────
  for (int i = 0; i < nodeN; i++) {
    const MeshNode* nd = meshService.getNode(i);
    if (!nd || !nd->nodeNum || nd->nodeNum == me || !nd->posHeardMs) {
      continue;                  // our own row holds the manual pin; it is drawn below
    }
    int vx = 0, vy = 0;
    if (!mapLatLonToView(zoom, cx, cy, vpW, vpH,
                         mapI7ToDeg(nd->latI), mapI7ToDeg(nd->lonI), &vx, &vy)) {
      continue;
    }
    const bool stale = (uint32_t)(now - nd->posHeardMs) > MAP_STALE_MS;
    /* AN APPROXIMATE POSITION IS A CIRCLE, NOT A DOT (0.9.82). A node that sent precision_bits
     * (a stock channel's position precision: 13 bits is a ~2.9 km cell) put its point in the
     * middle of that cell; the dot says "here" and the ring says "somewhere in here". Radius
     * per mesh_pos.h (the Android app's own formula). Drawn under the marker; skipped when it
     * would hide inside the marker or be bigger than the screen, where it says nothing. */
    if (meshPosIsApprox(nd->posPrec)) {
      const double mpp = mapMetersPerPixel(mapI7ToDeg(nd->latI), zoom);
      const double rp = mpp > 0 ? meshPosPrecisionRadiusM(nd->posPrec) / mpp : 0;
      if (rp >= 7.0 && rp <= 400.0) {
        drawRing(vpX + vx, vpY + vy, (int)(rp + 0.5), stale ? MAP_C_NODE_OLD : MAP_C_NODE);
      }
    }
    drawMarker(vpX + vx, vpY + vy, stale ? MAP_C_NODE_OLD : MAP_C_NODE, MAP_SHAPE_DOT);
  }
  for (int i = 0; i < wpN; i++) {
    const MeshWaypoint* w = meshService.getWaypoint(i);
    if (!w || !w->id) {
      continue;
    }
    int vx = 0, vy = 0;
    if (mapLatLonToView(zoom, cx, cy, vpW, vpH,
                        mapI7ToDeg(w->latI), mapI7ToDeg(w->lonI), &vx, &vy)) {
      drawMarker(vpX + vx, vpY + vy, MAP_C_WP, MAP_SHAPE_DIAMOND);
    }
  }
  for (int i = 0; i < pinCount && pins; i++) {
    if (pinVx[i] < 0 || pinVx[i] >= vpW || pinVy[i] < 0 || pinVy[i] >= vpH) {
      continue;
    }
    drawMarker(vpX + pinVx[i], vpY + pinVy[i],
               pins[i].sharedId ? MAP_C_PIN_SH : MAP_C_PIN, MAP_SHAPE_DOT);
    if (pins[i].sharedId) {
      /* ⚠ SHAPE, NOT JUST HUE. Orange and yellow differ only in the green channel and this is
       * a small screen read in daylight, often through a polarised lens — "is that one shared
       * or not" is a question whose wrong answer is a location on the air, or a camp nobody
       * else can see. A hollow centre is unmistakable at arm's length and costs one call.
       * (The verifier called the colour pair adequate; it is adequate indoors.) */
      drawBlob(vpX + pinVx[i], vpY + pinVy[i], 1, BLACK, false);
    }
  }
  if (selfKind) {
    int vx = 0, vy = 0;
    if (mapLatLonToView(zoom, cx, cy, vpW, vpH,
                        mapI7ToDeg(selfLat), mapI7ToDeg(selfLon), &vx, &vy)) {
      if (selfKind == MAPS_SELF_PIN) {
        drawMarker(vpX + vx, vpY + vy, MAP_C_ME, MAP_SHAPE_DIAMOND);
      } else {
        /* A WI-FI FIX IS A CIRCLE, NOT A DOT (0.9.82, geoloc.h): "somewhere in here", at the
         * accuracy two agreeing lookups gave (50-100 m). Drawn under the marker like a node's
         * precision ring; skipped where it would hide inside the marker or outgrow the screen.
         * The label below carries the same number, so the size is never only in the picture. */
        if (selfKind == MAPS_SELF_GPS && selfSrc == SELF_NET && selfAcc > 0) {
          const double mpp = mapMetersPerPixel(mapI7ToDeg(selfLat), zoom);
          const double rp = mpp > 0 ? (double)selfAcc / mpp : 0;
          if (rp >= 7.0 && rp <= 400.0) {
            drawRing(vpX + vx, vpY + vy, (int)(rp + 0.5), MAP_C_ME);
          }
        }
        const uint16_t c = (selfKind == MAPS_SELF_GPS) ? MAP_C_ME : MAP_C_NODE_OLD;
        drawBlob(vpX + vx, vpY + vy, 6, BLACK, false);
        drawBlob(vpX + vx, vpY + vy, 5, c, false);
        drawBlob(vpX + vx, vpY + vy, 2, BLACK, false);
        reserveBox(vpX + vx - 6, vpY + vy - 6, 13, 13);
      }
    }
  }

  // ── pass 2: labels, most important first ─────────────────────────────────────────────
  if (selfKind) {
    int vx = 0, vy = 0;
    if (mapLatLonToView(zoom, cx, cy, vpW, vpH,
                        mapI7ToDeg(selfLat), mapI7ToDeg(selfLon), &vx, &vy)) {
      char sl[24];
      uint16_t sc = MAP_C_ME;
      if (selfKind == MAPS_SELF_GPS) {
        /* "me", "me (phone)", or "me +/-60 m Wi-Fi": a Wi-Fi fix never appears without its
         * size (self_pos.h, selfFixLabel). */
        SelfFix lf;
        memset(&lf, 0, sizeof(lf));
        lf.src = selfSrc;
        lf.accM = selfAcc;
        selfFixLabel(sl, sizeof(sl), &lf);
      } else if (selfKind == MAPS_SELF_PIN) {
        strlcpy(sl, "my pin", sizeof(sl));
      } else if (selfKind == MAPS_SELF_POOR_GPS) {
        const unsigned sats = selfAge & 0xFF, hdopX10 = selfAge >> 8;
        if (sats < 4) {
          snprintf(sl, sizeof(sl), "me? %u sats", sats);
        } else {
          snprintf(sl, sizeof(sl), "me? HDOP %u.%u", hdopX10 / 10, hdopX10 % 10);
        }
      } else {
        const unsigned mins = (unsigned)(selfAge / 60000u);
        snprintf(sl, sizeof(sl), "%s, %um ago", selfMapLabel(selfSrc), mins);
        sc = MAP_C_NODE_OLD;
      }
      drawLabel(vpX + vx, vpY + vy, sl, sc);
    }
  }
  for (int i = 0; i < pinCount && pins; i++) {
    if (pinVx[i] < 0 || pinVx[i] >= vpW || pinVy[i] < 0 || pinVy[i] >= vpH) {
      continue;
    }
    drawLabel(vpX + pinVx[i], vpY + pinVy[i], pins[i].name,
              pins[i].sharedId ? MAP_C_PIN_SH : MAP_C_PIN);
  }
  for (int i = 0; i < wpN; i++) {
    const MeshWaypoint* w = meshService.getWaypoint(i);
    if (!w || !w->id) {
      continue;
    }
    int vx = 0, vy = 0;
    if (mapLatLonToView(zoom, cx, cy, vpW, vpH,
                        mapI7ToDeg(w->latI), mapI7ToDeg(w->lonI), &vx, &vy)) {
      drawLabel(vpX + vx, vpY + vy, w->name, MAP_C_WP);
    }
  }
  for (int i = 0; i < nodeN; i++) {
    const MeshNode* nd = meshService.getNode(i);
    if (!nd || !nd->nodeNum || nd->nodeNum == me || !nd->posHeardMs) {
      continue;
    }
    int vx = 0, vy = 0;
    if (!mapLatLonToView(zoom, cx, cy, vpW, vpH,
                         mapI7ToDeg(nd->latI), mapI7ToDeg(nd->lonI), &vx, &vy)) {
      continue;
    }
    const uint32_t ageMs = now - nd->posHeardMs;
    const bool stale = ageMs > MAP_STALE_MS;
    char lbl[MESH_NAME_LEN + 12];
    if (stale) {
      /* ⚠ THE AGE RIDES WITH THE NAME WHEN IT IS OLD. A grey dot says "not fresh"; it does
       * not say whether that is forty minutes or yesterday, and on foot those are completely
       * different pieces of information. */
      const unsigned mins = (unsigned)(ageMs / 60000u);
      if (mins >= 60u) {
        snprintf(lbl, sizeof(lbl), "%s %uh%02um", nd->name[0] ? nd->name : "?",
                 mins / 60u, mins % 60u);
      } else {
        snprintf(lbl, sizeof(lbl), "%s %um", nd->name[0] ? nd->name : "?", mins);
      }
    } else {
      strlcpy(lbl, nd->name[0] ? nd->name : "?", sizeof(lbl));
    }
    drawLabel(vpX + vx, vpY + vy, lbl, stale ? MAP_C_NODE_OLD : MAP_C_NODE);
  }

  drawCrosshair();
  drawChips();
  drawBottomStrip();
}

void MapsApp::drawChips() {
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  const int th = (int)fnt->height();
  const int chipH = th + 4;
  if (chipH > vpH) {
    return;
  }

  /* The star is the one always-visible sign that pixels are borrowed from a shallower level —
   * ANY position in view, not only past the area's deepest tiles (drawMap's stretch summary).
   * Without it a fuzzy screen reads as a bad tile or a dirty lens. 🛑 "z18*" IS THE FLOOR OF
   * THE CORNER CHIP: nothing below trims it, and the status chip leaves room for it — the
   * longest stretch label is exactly when the star matters most. */
  char left[48];
  snprintf(left, sizeof(left), "z%d%s", zoom, stretchAny ? "*" : "");
  const size_t floorN = strlen(left);
  const int floorW = lcd.textWidth(left) + 8;

  /* ⚠ THE STATUS CHIP IS DRAWN FIRST AND TAKES THE ROOM IT NEEDS — all of it but the corner
   * chip's floor. "no map on the card" is the most important sentence this screen can say,
   * and the area name beside it is the least; when they compete for 240 pixels the wrong one
   * used to win, because it was drawn first and the other was simply skipped. A stretch label
   * that will not fit is shortened ("z16-14 x2-8") before anything is dropped. */
  int rightEdge = vpX + vpW;
  if (statusLine[0]) {
    const int avail = vpW - floorW - 4;
    int sw = lcd.textWidth(statusLine);
    if (statusIsStretch && sw + 8 > avail &&
        mapStretchLabel(statusLine, sizeof(statusLine), zoom, stretchMinL, stretchMaxL, 1) > 0) {
      sw = lcd.textWidth(statusLine);
    }
    if (sw + 8 <= avail) {
      const int sx = vpX + vpW - sw - 8;
      fillClipped(sx, vpY, sw + 8, chipH, MAP_C_CHIP);
      lcd.setTextColor(0xFD20, MAP_C_CHIP);
      lcd.drawString(statusLine, sx + 4, vpY + 2);
      rightEdge = sx;
    }
  }

  /* An area name may be 31 characters and this chip is not that wide. Trim it to what is left
   * rather than drawing a grey bar with nothing in it, which is what a bare width test gives
   * you: the background had already been painted before the text was skipped. */
  snprintf(left + floorN, sizeof(left) - floorN, " %s", areaSel >= 0 ? areas[areaSel].name : "-");
  const int room = rightEdge - vpX - 4;
  size_t n = strlen(left);
  int lw = lcd.textWidth(left);
  while (lw + 8 > room && n > floorN) {
    left[--n] = '\0';
    lw = lcd.textWidth(left);
  }
  if (lw + 8 <= room) {
    fillClipped(vpX, vpY, lw + 8, chipH, MAP_C_CHIP);
    lcd.setTextColor(MAP_C_ME, MAP_C_CHIP);
    lcd.drawString(left, vpX + 4, vpY + 2);
  }
}

/* The note, broken into rows that fit `maxW` in the current font — at most MAPS_NOTE_ROWS,
 * the last one ellipsized if even that is not enough. Returns the row count. The breaking is
 * menu_wrap.h's (the same one the menus' notes use); this just measures for it. {user},
 * 2026-09-20: "If I don't have a GPS fix and try to center on myself, the message gets cut
 * off" — "No fix yet, and no pin of your own to fall back on" is 50 characters, and the
 * strip's row holds about 30 of this font. */
struct NoteRowsCtx {
  SmoothFont* fnt;
  uint16_t maxW;
  char (*rows)[72];
  int n;
};
static size_t noteRowsFit(const char* s, void* v) {
  NoteRowsCtx* c = (NoteRowsCtx*)v;
  return (size_t)c->fnt->fitTextLength(s, c->maxW, 1);
}
static void noteRowsTake(const char* s, size_t len, void* v) {
  NoteRowsCtx* c = (NoteRowsCtx*)v;
  if (c->n >= MAPS_NOTE_ROWS) {
    return;
  }
  if (len >= sizeof(c->rows[0])) {
    len = sizeof(c->rows[0]) - 1;
  }
  memcpy(c->rows[c->n], s, len);
  c->rows[c->n][len] = 0;
  c->n++;
}
int MapsApp::noteRows(SmoothFont* fnt, uint16_t maxW, char (*rows)[72]) {
  if (!note[0]) {
    return 0;
  }
  NoteRowsCtx c = { fnt, maxW, rows, 0 };
  wrapNote(note, noteRowsFit, noteRowsTake, &c, MAPS_NOTE_ROWS);
  return c.n;
}

/* ElevTextMeasureFn over a SmoothFont itself (not the LCD's current font): the altitude row is
 * fitted in the font it is drawn in, whoever set the LCD's font last. */
static int elevMeasure(void* ctx, const char* s) {
  return (int)((SmoothFont*)ctx)->textWidth(s);
}

void MapsApp::drawBottomStrip() {
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  const int th = (int)fnt->height();
  /* Row 1 is the scale bar; the note takes one row, or two when it needs them (noteRows).
   * The strip grows upward over the map for the second row, and shrinks back when the note
   * is retired by the next key.
   * ALTITUDE (0.9.80): one more row, under the coordinates (or under a one-row note), while the
   * layer is on the card and the setting is on. A TWO-row note takes the room and the altitude
   * row is dropped for it - the note wins, as it wins over the coordinates. The strip is at
   * most three rows either way, and nothing else on the map depends on its height: the
   * crosshair, the pin pick and the snap are at the middle of the viewport, the chips at its
   * top, and labels are placed without regard to the strip, which paints over them (as it did
   * at three rows for a two-row note). */
  char nrows[MAPS_NOTE_ROWS][72];
  const int noteN = noteRows(fnt, (uint16_t)(vpW - 8), nrows);
  const bool altRow = elevActive() && noteN < 2;
  const int stripH = th * (1 + (noteN > 1 ? noteN : 1) + (altRow ? 1 : 0)) + 6;
  const int sy = vpY + vpH - stripH;
  if (stripH >= vpH) {
    return;
  }
  fillClipped(vpX, sy, vpW, stripH, MAP_C_STRIP);

  // Row 1: the scale bar, and how far the crosshair is from the reference place.
  double lat = 0, lon = 0;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
  /* unitsScaleBar: in metric it IS mapScaleBar(mpp, 84, &px) and the label this drew from its
   * metres ("%dm" under 1000, else "%dkm") - same arguments, same bar, same pixels
   * (tests/test_units.cpp compares the two over a sweep of scales). */
  int barPx = 0;
  char sb[24];
  const int barM = unitsScaleBar(mapMetersPerPixel(lat, zoom), 84, gUnits, &barPx, sb, sizeof(sb));
  const int by = sy + 3;
  int usedLeft = vpX + 4;               // how far along row 1 the scale bar reaches
  if (barM > 0 && barPx > 0) {
    fillClipped(vpX + 4, by + th / 2, barPx, 2, MAP_C_ME);
    fillClipped(vpX + 4, by + th / 2 - 3, 2, 8, MAP_C_ME);
    fillClipped(vpX + 4 + barPx - 2, by + th / 2 - 3, 2, 8, MAP_C_ME);
    lcd.setTextColor(MAP_C_ME, MAP_C_STRIP);
    lcd.drawString(sb, vpX + 8 + barPx, by);
    usedLeft = vpX + 8 + barPx + lcd.textWidth(sb);
  }

  int32_t rLat = 0, rLon = 0;
  char rName[24];
  if (meshService.resolveReference(&rLat, &rLon, rName, sizeof(rName))) {
    const int32_t cLat = mapDegToI7(lat), cLon = mapDegToI7(lon);
    const double m = meshPosDistanceM(rLat, rLon, cLat, cLon);
    char d[16];
    unitsFmtDist(m, gUnits, d, sizeof(d));
    char right[44];
    snprintf(right, sizeof(right), "%s %s %s", d,
             meshPosCompass8(meshPosBearingDeg(rLat, rLon, cLat, cLon)), rName);
    /* ⚠ AGAINST THE SCALE BAR, not against the screen. "1.4km NE Camp" is right-aligned, so a
     * long reference name grows LEFTWARDS — a width test against vpW lets it start at x=0 and
     * print straight over the scale bar, which is the one thing on this row that cannot be
     * read wrong without consequence. Dropped rather than overlapped: the distance is also on
     * the Places screen, and an unreadable scale bar is a map you cannot judge a walk from. */
    const int rw = lcd.textWidth(right);
    if (rw + 8 <= (vpX + vpW - 4) - usedLeft) {
      lcd.setTextColor(MAP_C_WP, MAP_C_STRIP);
      lcd.drawString(right, vpX + vpW - rw - 4, by);
    }
  }

  // Row 2 (and 3): the note if there is one, else the ruler's reading, else the crosshair.
  char line[64];
  if (noteN > 0) {
    lcd.setTextColor(0xFD20, MAP_C_STRIP);
    for (int i = 0; i < noteN; i++) {
      /* Ellipsized (see below); only the LAST row can still be too long, when the note ran
       * past MAPS_NOTE_ROWS rows and wrapNote's tail was cut where it stood. */
      guiDrawEllipsized(lcd, nrows[i], (uint16_t)(vpW - 8), (int16_t)(vpX + 4),
                        (int16_t)(sy + th * (1 + i) + 4));
    }
  } else {
    if (measuring) {
      /* The whole row, not the right end of row 1: "1.2km SE" plus a word does not fit beside
       * a 1 km scale bar, and a measurement that is silently dropped for width is the one
       * failure a ruler must not have. With both ends' ground known the row says the climb
       * ("Ruler: 1.2km SE, climb +85m": the anchor's ground to the crosshair's, both from the
       * tiles), else the old words. */
      const int32_t cLat = mapDegToI7(lat), cLon = mapDegToI7(lon);
      const bool climb = elevActive() && elevHereCurrent() && elevHere.rc == 1 &&
                         elevMeas.valid && elevMeas.latI == measLatI && elevMeas.lonI == measLonI &&
                         elevMeas.rc == 1;
      /* "climb*" when either end came from the coarse z10 layer, as the strip's "Elev*" does
       * (review 2026-09-27: a ~100 m-pixel climb looked as exact as a z13 one). */
      const bool coarse = climb && (elevHere.z == ELEV_Z_COARSE || elevMeas.z == ELEV_Z_COARSE);
      elevRulerText(line, sizeof(line), meshPosDistanceM(measLatI, measLonI, cLat, cLon),
                    meshPosCompass8(meshPosBearingDeg(measLatI, measLonI, cLat, cLon)), climb,
                    climb ? elevHere.m - elevMeas.m : 0.0, gUnits, coarse);
    } else {
      snprintf(line, sizeof(line), "%.5f, %.5f", lat, lon);
    }
    lcd.setTextColor(MAP_C_ME, MAP_C_STRIP);
    /* ⚠ ELLIPSIZED, NOT drawString. This row carries a note this app composed from a pin name,
     * a place name or a node name — all of them other people's text — and plain drawString does
     * not clip: it paints to the screen edge and beyond. guiDrawEllipsized exists for exactly
     * this (GUI.h), cuts on a character boundary and appends ".." so a truncated string looks
     * truncated instead of looking complete. */
    guiDrawEllipsized(lcd, line, (uint16_t)(vpW - 8), (int16_t)(vpX + 4), (int16_t)(sy + th + 4));
  }
  if (altRow) {
    /* The last row: "Elev 412m, 35m above you" - composed from the CACHE (elevRowText), fitted
     * to the row by measuring (a coarse height turns "(coarse)" into "Elev*", a long reference
     * name is cut), and ellipsized as a backstop like every row here. */
    elevRowText(line, sizeof(line), NULL, &elevHere, elevHereCurrent(), vpW - 8);
    lcd.setTextColor(MAP_C_ME, MAP_C_STRIP);
    guiDrawEllipsized(lcd, line, (uint16_t)(vpW - 8), (int16_t)(vpX + 4),
                      (int16_t)(sy + th * (1 + (noteN > 0 ? noteN : 1)) + 4));
  }
}

void MapsApp::drawNoMapPage() {
  SmoothFont* fnt = fonts[AKROBAT_BOLD_18];
  lcd.fillRect(vpX, vpY, vpW, vpH, BLACK);
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  lcd.setTextColor(0xFD20, BLACK);
  lcd.drawString(cardOk ? "No map tiles on the card" : "No SD card", vpX + 8, vpY + 10);
  lcd.setTextColor(WHITE, BLACK);
  lcd.setTextFont(fonts[AKROBAT_BOLD_16]);
  static const char* noCard[] = {
    "The phone cannot see a card at all.",
    "",
    "Switch the phone OFF, seat the card,",
    "and switch it back on - the power",
    "switch is a hard cut, so 'off' is off.",
    "",
    "The card must be FAT32 (see the",
    "README); the phone does not speak",
    "exFAT, which is how cards over 32 GB",
    "come from the factory.",
  };
  const char* lines[] = {
    "Pins and mesh places still work -",
    "the map is just blank behind them.",
    "",
    "Put tiles on the card as",
    "  /maps/<area>/<z>/<x>/<y>.565",
    "made by tools/convert_tiles.py",
    "on the computer (docs/maps.md).",
    "",
    "Menu > Rescan card after copying.",
  };
  const char* const* body = cardOk ? lines : noCard;
  const unsigned bodyN = cardOk ? (unsigned)(sizeof(lines) / sizeof(lines[0]))
                                : (unsigned)(sizeof(noCard) / sizeof(noCard[0]));
  int y = vpY + 40;
  for (unsigned i = 0; i < bodyN; i++) {
    if (y + 16 > vpY + vpH - 20) {
      break;
    }
    lcd.drawString(body[i], vpX + 8, y);
    y += 17;
  }
  /* ⚠ THE NOTE GOES HERE TOO. This page replaces the map entirely, and the note line is the
   * only feedback several keys have: on it, 0 (centre on me), 7/9 (step pins) and the zoom
   * keys all wrote a message that nothing ever drew, so they read as dead keys on the one
   * screen where a new user is most likely to be pressing things to see what happens. */
  if (note[0]) {
    char nrows[MAPS_NOTE_ROWS][72];
    const int n = noteRows(fonts[AKROBAT_BOLD_16], (uint16_t)(vpW - 12), nrows);
    lcd.setTextColor(0xFD20, BLACK);
    for (int i = 0; i < n; i++) {
      guiDrawEllipsized(lcd, nrows[i], (uint16_t)(vpW - 12), (int16_t)(vpX + 6),
                        (int16_t)(vpY + vpH - 18 - 17 * (n - 1 - i)));
    }
  }
}

// ---------------------------------------------------------------- actions

void MapsApp::setNote(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(note, sizeof(note), fmt, ap);
  va_end(ap);
}

void MapsApp::centreOn(double lat, double lon) {
  double wx = 0, wy = 0;
  mapLatLonToWorld(lat, lon, zoom, &wx, &wy);
  cx = (int32_t)(wx + 0.5);
  cy = (int32_t)(wy + 0.5);
  mapClampView(zoom, vpW, vpH, &cx, &cy);
}

void MapsApp::centreOnMe() {
  /* "Me" means this phone: the GPS fix, or the position the user declared by hand. It does
   * NOT quietly mean the reference place — that may be a waypoint somebody else shared, and a
   * key labelled "centre on me" that lands on their camp has told you something false. The
   * reference is still offered as a last resort, and then the screen names it. */
  int32_t la = 0, lo = 0;
  uint32_t age = 0;
  int src = SELF_NONE;
  uint16_t acc = 0;
  const int kind = selfPosition(&la, &lo, &age, &src, &acc);
  if (kind == MAPS_SELF_GPS) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    if (src == SELF_NET) {
      // The age of a Wi-Fi fix is its last confirming scan's (self_pos.h): it is true while heard.
      setNote("Centred on the Wi-Fi location, +/-%u m (beaconDB)", (unsigned)acc);
    } else {
      setNote(src == SELF_PHONE ? "Centred on the phone's GPS (%us old)"
                                : "Centred on the GPS fix (%us old)", (unsigned)(age / 1000u));
    }
    return;
  }
  if (kind == MAPS_SELF_PIN) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    setNote(meshService.anySourceEnabled() ? "No fresh fix - centred on your pin"
                                           : "Location sources off - centred on your pin");
    return;
  }
  if (kind == MAPS_SELF_POOR_GPS) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    const unsigned sats = age & 0xFF, hdopX10 = age >> 8;
    setNote("Poor fix (%u sats, HDOP %u.%u) - could be well off", sats, hdopX10 / 10, hdopX10 % 10);
    return;
  }
  if (kind == MAPS_SELF_OLD_GPS) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    setNote("No fresh fix - this is where you were %um ago",
            (unsigned)(age / 60000u));
    return;
  }
  char nm[24];
  if (meshService.resolveReference(&la, &lo, nm, sizeof(nm))) {
    centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
    setNote("Nothing of your own - centred on %s", nm);
    return;
  }
  if (!meshService.anySourceEnabled()) {
    setNote("GPS, phone and Wi-Fi location are off - Meshtastic > My node");
  } else {
    setNote("No fix yet, and no pin of your own to fall back on");
  }
}

int MapsApp::pinUnderCrosshair() {
  if (!pins || pinCount <= 0) {
    return -1;
  }
  for (int i = 0; i < pinCount; i++) {
    mapLatLonToView(zoom, cx, cy, vpW, vpH,
                    mapI7ToDeg(pins[i].latI), mapI7ToDeg(pins[i].lonI),
                    &pinVx[i], &pinVy[i]);
  }
  return mapPinPickNearest(pinVx, pinVy, pinCount, vpW / 2, vpH / 2, MAPS_PICK_RADIUS);
}

/* ── THE MARKERS THE ARROWS CAN STOP ON ──────────────────────────────────────────────────
 * Three kinds, exactly the set the overlay draws: your pins, places heard from the mesh
 * (waypoints — the green gems) and other nodes with a position (cyan, grey when stale). One
 * enumerator feeds both searches below, so the arrows and the screen can never disagree
 * about what is there. Filters are the overlay's own (drawOverlays): a place needs an id, a
 * node needs a heard position and must not be this phone. {user}, 2026-09-19: "make it snap
 * to places in the mesh and nodes with locations too". */
#define SNAP_PIN    0
#define SNAP_PLACE  1
#define SNAP_NODE   2

int MapsApp::snapCount(int kind) {
  switch (kind) {
  case SNAP_PIN:   return pins ? pinCount : 0;
  case SNAP_PLACE: return meshService.getWaypointCount();
  default:         return meshService.getNodeCount();
  }
}

/* Marker `idx` of `kind` in view coordinates (which may lie off the screen — the corridor
 * search wants the ones just past the edge too). False when the slot is empty or filtered. */
bool MapsApp::snapMarker(int kind, int idx, int* vx, int* vy) {
  int32_t la = 0, lo = 0;
  if (kind == SNAP_PIN) {
    if (!pins || idx < 0 || idx >= pinCount) {
      return false;
    }
    la = pins[idx].latI;
    lo = pins[idx].lonI;
  } else if (kind == SNAP_PLACE) {
    const MeshWaypoint* w = meshService.getWaypoint(idx);
    if (!w || !w->id) {
      return false;
    }
    la = w->latI;
    lo = w->lonI;
  } else {
    const MeshNode* nd = meshService.getNode(idx);
    if (!nd || !nd->nodeNum || nd->nodeNum == meshService.getMyNodeNum() || !nd->posHeardMs) {
      return false;
    }
    la = nd->latI;
    lo = nd->lonI;
  }
  mapLatLonToView(zoom, cx, cy, vpW, vpH, mapI7ToDeg(la), mapI7ToDeg(lo), vx, vy);
  return true;
}

/* The marker nearest the crosshair within `radius` pixels, if any. Pins first at equal
 * distance, since a pin dropped ON a node (the usual "camp" case) is the thing OK would open. */
bool MapsApp::snapNear(int radius, MapsSnapHit* out) {
  bool found = false;
  long bestD2 = 0;
  for (int kind = SNAP_PIN; kind <= SNAP_NODE; kind++) {
    const int n = snapCount(kind);
    for (int i = 0; i < n; i++) {
      int vx = 0, vy = 0;
      if (!snapMarker(kind, i, &vx, &vy)) {
        continue;
      }
      const int ox = vx - vpW / 2, oy = vy - vpH / 2;
      /* 🛑 BY AXIS FIRST, BEFORE ANY SQUARING. A marker across the world has view coordinates
       * in the millions, and `long` is 32 bits on this chip: ox*ox wrapped to whatever, and
       * on phone 2 a tap off Pin 1 "landed" on a node an ocean away (2026-09-20; the same
       * arithmetic in yesterday's snapUnder could read "on a marker" while on nothing).
       * Inside the box the squares are small. */
      if (ox > radius || ox < -radius || oy > radius || oy < -radius) {
        continue;
      }
      const long d2 = (long)ox * ox + (long)oy * oy;
      if (!found || d2 < bestD2) {
        found = true;
        bestD2 = d2;
        out->kind = kind;
        out->idx = i;
      }
    }
  }
  return found;
}

/* Land on a marker: the crosshair exactly on it, the strip saying what it is. A pin becomes
 * the selected pin (7/9 step from it, OK opens it); a place or a node is named — OK there
 * drops a pin, as it does anywhere. */
void MapsApp::snapTo(const MapsSnapHit& h) {
  if (h.kind == SNAP_PIN) {
    pinSel = h.idx;
    centreOn(mapI7ToDeg(pins[h.idx].latI), mapI7ToDeg(pins[h.idx].lonI));
    setNote("%s (%d of %d)", pins[h.idx].name, h.idx + 1, pinCount);
  } else if (h.kind == SNAP_PLACE) {
    const MeshWaypoint* w = meshService.getWaypoint(h.idx);
    if (!w) {
      return;
    }
    centreOn(mapI7ToDeg(w->latI), mapI7ToDeg(w->lonI));
    setNote("%s (place from the mesh)", w->name[0] ? w->name : "unnamed");
  } else {
    const MeshNode* nd = meshService.getNode(h.idx);
    if (!nd) {
      return;
    }
    centreOn(mapI7ToDeg(nd->latI), mapI7ToDeg(nd->lonI));
    const uint32_t ageMs = millis() - nd->posHeardMs;
    if (ageMs > MAP_STALE_MS) {
      const unsigned mins = (unsigned)(ageMs / 60000u);
      if (mins >= 60) {
        setNote("%s (node, %uh%02um ago)", nd->name[0] ? nd->name : "?", mins / 60, mins % 60);
      } else {
        setNote("%s (node, %um ago)", nd->name[0] ? nd->name : "?", mins);
      }
    } else {
      setNote("%s (node)", nd->name[0] ? nd->name : "?");
    }
  }
}

/* One press (or one hold repeat) of an arrow.
 *
 * The snap is a LANDING rule now, and nothing else. {user}, 2026-09-20: "make the snap less
 * aggressive... I still want to be able to scroll around them, but if I just land near one
 * while scrolling (as near as I possibly can) then I want it to snap. It needs to feel
 * intentional." The first cut (2026-09-19) stopped a tap on any marker in its path and, from
 * a marker, jumped to the next one anywhere on screen — which meant a pin could not be
 * scrolled AWAY from by taps at all. So: a tap moves the nudge, a hold sweeps, and the map
 * snaps only when the crosshair LANDS within MAP_SNAP_RADIUS_PX of a marker — after a tap
 * (`discrete`), or after a hold when it ends (the release check in the APP_TIMER branch). A
 * hold's repeats never snap. The nudge is bigger than the radius, so the tap after a snap
 * steps clear; it is small enough that taps on the two axes can put the crosshair within
 * the radius of any marker (map_tiles.h has the arithmetic). Pins can still be walked in
 * order with 7 and 9. */
bool MapsApp::panOnce(int dx, int dy, int step, bool discrete) {
  /* Returns true when the press SNAPPED to a marker; a plain pan or a refused one returns
   * false. The map's edge is reported on the strip. */
  if (!mapPanView(zoom, vpW, vpH, dx * step, dy * step, &cx, &cy)) {
    if (dy) {
      /* Only north/south can refuse: longitude wraps, so a sideways press always moves. */
      setNote("That is as far %s as the map goes", dy < 0 ? "north" : "south");
    }
    return false;                        // nothing moved, so nothing landed
  }
  return discrete && snapLanding();
}

/* The landing: with Snap on, centre on the marker within MAP_SNAP_RADIUS_PX of the
 * crosshair, if there is one. True when it did. */
bool MapsApp::snapLanding() {
  if (!snapPins) {
    return false;
  }
  MapsSnapHit h;
  if (!snapNear(MAP_SNAP_RADIUS_PX, &h)) {
    return false;
  }
  snapTo(h);
  return true;
}

bool MapsApp::dropPin() {
  if (!pins) {
    setNote("No memory for pins");
    return false;
  }
  if (pinCount >= MAPS_MAX_PINS) {
    setNote("Full: %d pins. Delete one first.", MAPS_MAX_PINS);
    return false;
  }
  double lat = 0, lon = 0;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
  MapPin* p = &pins[pinCount];
  memset(p, 0, sizeof(*p));
  p->latI = mapDegToI7(lat);
  p->lonI = mapDegToI7(lon);
  p->sharedId = 0;
  mapPinAutoName(pins, pinCount, p->name, sizeof(p->name));
  pinSel = pinCount;
  pinCount++;
  pinsDirty = true;
  savePins();                    // a pin that vanishes because the battery died is not a pin
  return true;
}

bool MapsApp::sharePin(int idx, char* why, size_t whyCap) {
  /* 🛑 NOT OVER ITS OWN RETRACTION (mapPinRetractionQueued, map_pins.h). The pin keeps its id
   * until "Take it off the mesh" is reported SENT, so the settle below would read the
   * still-queued retraction as "no word", keep the id, and this would queue the place again
   * right behind it — back on every other radio. Asked BEFORE the settle, which is left
   * pending: the retraction's own answer still decides the id. Every re-share comes through
   * here (Update, Move here, Rename, the channel picker), so this is the one gate; Move and
   * Rename also ask first, so they can say what they did instead. */
  if (retractionQueued(idx)) {
    strlcpy(why, "Still being taken off the mesh - try again in a second", whyCap);
    return false;
  }
  settleMeshOp(true);                    // one pin frame at a time: the last one settled first
  if (idx < 0 || idx >= pinCount || !pins) {
    strlcpy(why, "no pin", whyCap);
    return false;
  }
  bool onAir = false;
  const bool wasShared = (pins[idx].sharedId != 0);
  /* On the channel stored with the pin. NULL falls back to announceChannel() inside the
   * service — only reachable for a pin shared before channels were recorded (chan ""). A pin
   * whose recorded channel has LEFT this phone is not sent anywhere: an update on some other
   * channel is a second copy the radios holding the first will never see retracted. */
  const MeshChannel* ch = pinChannel(idx);
  if (pins[idx].chan[0] && !ch) {
    snprintf(why, whyCap, "Channel '%s' is gone from this phone - not sent", pins[idx].chan);
    return false;
  }
  const uint32_t id = meshService.shareWaypoint(pins[idx].sharedId,
                                                pins[idx].latI, pins[idx].lonI,
                                                pins[idx].name, 0, &onAir, ch);
  if (!id) {
    strlcpy(why, "Could not share - the mesh refused it", whyCap);
    return false;
  }
  if (!onAir && !wasShared) {
    /* 🛑 A SHARE THAT NEVER LEFT THE RADIO IS NOT A SHARE, and writing the id down makes the
     * claim permanent: the pin draws yellow, the list says "(shared)", and the options offer
     * "Update it on the mesh" — for a place nobody else has. Worse, a later Delete would then
     * send a retraction for a waypoint that was never announced. Roll the whole thing back so
     * the pin stays plainly local, and say what happened.
     * A RE-share (wasShared) is left alone on purpose: the mesh really does hold the old copy,
     * and forgetting the id would strand it there with no way to retract it. */
    meshService.unshareWaypoint(id, ch);
    snprintf(why, whyCap, "NOT sent - radio not ready. Still just yours.");
    return false;
  }
  pins[idx].sharedId = id;
  pinsDirty = true;
  savePins();
  /* 🔑 SAY WHETHER IT ACTUALLY LEFT THE PHONE. A place that is on your map and nobody else's,
   * shown as shared, is the failure the pin-announce path was built to avoid: it reads as
   * "they know where camp is" when nobody does.
   * 🛑 AND `onAir` MEANS QUEUED (review M1, 2026-09-25): the frame is waiting for the radio, so
   * this says "Sending" and the app timer says "Shared" once the radio reports it left — or
   * rolls a first share back if it never does (settleMeshOp). The id is written down NOW, not
   * then: a frame that does go out must never leave the only retracting id unrecorded. */
  if (onAir) {
    beginMeshOp(wasShared ? MAP_PINOP_RESHARE : MAP_PINOP_SHARE, meshService.lastQueuedPacketId(),
                idx);
    if (wasShared) {
      snprintf(why, whyCap, "Updating it on %s...", ch ? ch->name : "the mesh");
    } else {
      snprintf(why, whyCap, "Sending '%s' on %s...", pins[idx].name, ch ? ch->name : "the mesh");
    }
  } else {
    // Only reachable for a RE-share: the mesh still holds the old copy, so the id is kept.
    snprintf(why, whyCap, "NOT sent - the mesh still has the old one");
  }
  return onAir;
}

bool MapsApp::deletePin(int idx) {
  settleMeshOp(true);                    // one pin frame at a time (may clear this pin's id)
  if (idx < 0 || idx >= pinCount || !pins) {
    return true;
  }
  bool retracted = true;
  if (pins[idx].sharedId) {
    /* It is on other people's maps too, and the confirm screen promised to take it off
     * theirs — a camp that has moved is worse than no camp at all.
     * ⚠ THE RESULT IS RETURNED, NOT DISCARDED. Deleting locally while the retraction failed
     * strands the waypoint on every other radio with nothing left on this phone that knows
     * its id, so the caller has to be able to say so. A pin whose channel has left this
     * phone is not sent anywhere: a retraction on some other channel reaches nobody who
     * holds the waypoint and would count as success. */
    if (pins[idx].chan[0] && !pinChannel(idx)) {
      retracted = false;
    } else {
      retracted = meshService.unshareWaypoint(pins[idx].sharedId, pinChannel(idx));
      if (retracted) {
        /* QUEUED (review M1): the pin is copied into meshOp BEFORE it goes, so a retraction the
         * radio then fails to send puts it back, id and all (MAP_PINACT_RESTORE) — the one
         * handle that can ever take the place off other people's maps. */
        beginMeshOp(MAP_PINOP_DELETE, meshService.lastQueuedPacketId(), idx);
      }
    }
  }
  for (int i = idx; i + 1 < pinCount; i++) {
    pins[i] = pins[i + 1];       // keep the order: the list on screen must not reshuffle
  }
  pinCount--;
  if (pinSel >= pinCount) {
    pinSel = pinCount - 1;
  }
  pinsDirty = true;
  savePins();
  return retracted;
}

/* ── A pin's frame, from queued to what the radio said (review M1) ─────────────────────────── */

int MapsApp::pinByWaypoint(uint32_t wpId) const {
  if (!pins || !wpId) {
    return -1;
  }
  for (int i = 0; i < pinCount; i++) {
    if (pins[i].sharedId == wpId) {
      return i;
    }
  }
  return -1;
}

void MapsApp::beginMeshOp(uint8_t op, uint32_t txId, int idx) {
  memset(&meshOp, 0, sizeof(meshOp));
  if (!txId || idx < 0 || idx >= pinCount || !pins) {
    return;                  // nothing to watch: the pin stays as it is now (no word = safe side)
  }
  meshOp.txId = txId;
  meshOp.wpId = pins[idx].sharedId;
  meshOp.sinceMs = millis();
  meshOp.op = op;
  meshOp.pin = pins[idx];    // a copy: the pin may be deleted (or the list reordered) meanwhile
  armTimer();                // the redraw after this arms it too; a menu state needs it now
}

bool MapsApp::retractionQueued(int idx) const {
  if (!meshOp.txId || !pins || idx < 0 || idx >= pinCount) {
    return false;
  }
  return mapPinRetractionQueued(meshOp.op, meshService.txOutcome(meshOp.txId), meshOp.wpId,
                                pins[idx].sharedId);
}

bool MapsApp::settleMeshOp(bool final) {
  if (!meshOp.txId) {
    return false;
  }
  const uint8_t outcome = meshService.txOutcome(meshOp.txId);
  const bool giveUp = !final && (uint32_t)(millis() - meshOp.sinceMs) >= MAPS_MESH_OP_GIVEUP_MS;
  const uint8_t act = mapPinMeshSettle(meshOp.op, outcome, final || giveUp);
  if (act == MAP_PINACT_WAIT) {
    return false;
  }
  const bool noWord = (outcome != MESH_TXO_SENT && outcome != MESH_TXO_FAILED);
  const uint8_t op = meshOp.op;
  const uint32_t wpId = meshOp.wpId;
  const MapPin was = meshOp.pin;
  memset(&meshOp, 0, sizeof(meshOp));          // settled: nothing below can re-enter it
  const char* chName = was.chan[0] ? was.chan : "the mesh";
  const int i = pinByWaypoint(wpId);
  log_e("MAPS: pin '%s' frame %s -> %s (op %u, act %u)", was.name,
        outcome == MESH_TXO_SENT ? "SENT" : outcome == MESH_TXO_FAILED ? "FAILED" : "no word",
        act == MAP_PINACT_DONE ? "done" : act == MAP_PINACT_FORGET_ID ? "id forgotten"
          : act == MAP_PINACT_ROLL_BACK ? "rolled back" : act == MAP_PINACT_KEEP_ID ? "id kept"
          : act == MAP_PINACT_RESTORE ? "pin restored" : "?", (unsigned)op, (unsigned)act);
  switch (act) {
  case MAP_PINACT_FORGET_ID:
    if (i >= 0) {
      pins[i].sharedId = 0;
      pinsDirty = true;
      savePins();
    }
    // The pin's name NOW: it may have been renamed while its retraction waited (retractionQueued).
    setNote("Taken '%s' off the mesh", i >= 0 ? pins[i].name : was.name);
    break;
  case MAP_PINACT_ROLL_BACK: {
    /* A FIRST share that never left: the pin goes back to plainly local, and the local
     * waypoint shareWaypoint() made goes too. unshareWaypoint() also queues a retraction —
     * the conservative side if the "failed" frame did get out (a TX timeout can be a missed
     * TxDone) — but never on a channel other than the pin's (sharePin's rule). */
    if (i >= 0) {
      pins[i].sharedId = 0;
      pinsDirty = true;
      savePins();
    }
    const MeshChannel* ch = was.chan[0] ? meshService.findChannelByName(was.chan) : NULL;
    if (!was.chan[0] || ch) {
      meshService.unshareWaypoint(wpId, ch);
    }
    setNote("NOT sent - the radio did not send '%s'. Still just yours.", was.name);
    break;
  }
  case MAP_PINACT_KEEP_ID:
    if (noWord) {
      setNote("No word from the radio on '%s' - kept as shared", was.name);
    } else if (op == MAP_PINOP_UNSHARE) {
      setNote("NOT taken off - the radio did not send it. Try again.");
    } else {
      setNote("Update NOT sent - the mesh still has the old '%s'", was.name);
    }
    break;
  case MAP_PINACT_RESTORE:
    /* The retraction never left, and the pin is gone from this phone: put it back, id and
     * channel and all, so "Take it off the mesh" can be tried again. At the end of the list;
     * nothing else moves. */
    if (pins && pinCount < MAPS_MAX_PINS) {
      pins[pinCount++] = was;
      pinsDirty = true;
      savePins();
      setNote("Retraction NOT sent - '%s' is back so you can try again", was.name);
    } else {
      setNote("Retraction NOT sent, and no room to put '%s' back", was.name);
    }
    break;
  case MAP_PINACT_DONE:
  default:
    if (op == MAP_PINOP_DELETE) {
      if (noWord) {
        setNote("Deleted '%s' - no word from the radio on the retraction", was.name);
      } else {
        setNote("Deleted '%s' and took it off the mesh", was.name);
      }
    } else if (op == MAP_PINOP_RESHARE) {
      setNote("Updated '%s' on %s", was.name, chName);
    } else {
      setNote("Shared '%s' on %s", was.name, chName);
    }
    break;
  }
  return true;
}

void MapsApp::stepPin(int delta) {
  if (!pins || pinCount <= 0) {
    setNote("No pins yet - OK drops one here");
    return;
  }
  pinSel += delta;
  if (pinSel < 0) {
    pinSel = pinCount - 1;
  }
  if (pinSel >= pinCount) {
    pinSel = 0;
  }
  centreOn(mapI7ToDeg(pins[pinSel].latI), mapI7ToDeg(pins[pinSel].lonI));
  setNote("%s (%d of %d)", pins[pinSel].name, pinSel + 1, pinCount);
}

void MapsApp::setArea(int idx) {
  if (idx < 0 || idx >= areaCount) {
    return;
  }
  // Where we are looking, in ground terms, before the zoom range changes under us.
  double lat = 0, lon = 0;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
  areaSel = idx;
  cancelLoad();
  forgetMissing();
  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    slots[i].z = -1;             // the cache is full of another map's ground
    slots[i].ready = false;
  }
  const MapArea* a = &areas[areaSel];
  if (zoom < a->zMin) {
    zoom = a->zMin;
  }
  /* The stretched level travels between areas, but only one level past THIS area's tiles:
   * z17 over a z16 area is fine, z17 over a z13 area would be a 16x smear. */
  if (zoom > a->zMax + MAP_OVERZOOM_MAX) {
    zoom = a->zMax + MAP_OVERZOOM_MAX;
  }
  centreOn(lat, lon);
  if (!areaHolds(areaSel, lat, lon)) {
    /* The one thing an area switch must not leave you guessing: a grey screen under a map
     * that has tiles somewhere else. "home" on phone 2 is the hand-converted North Bend
     * tiles, 30 km west of the hunt area, and read as empty ({user}, 2026-09-20). */
    char where[24];
    areaWhere(areaSel, lat, lon, where, sizeof(where));
    setNote("Map: %s (z%d-%d) - its tiles are %s of here", a->name, a->zMin, a->zMax, where);
  } else {
    setNote("Map: %s (z%d-%d)", a->name, a->zMin, a->zMax);
  }
}

// ---------------------------------------------------------------- screens

void MapsApp::freeWidgets() {
  if (menu) {
    delete menu;
    menu = NULL;
  }
  if (textArea) {
    delete textArea;
    textArea = NULL;
  }
}

void MapsApp::freeMenu() {
  if (menu) {
    delete menu;
    menu = NULL;
  }
}

MenuWidget* MapsApp::newMenu(const char* emptyMessage) {
  freeMenu();                    // see the note in app_meshtastic.cpp: rebuilds used to leak
  MenuWidget* m = new MenuWidget(0, header->height(), lcd.width(),
                                 lcd.height() - header->height() - footer->height(),
                                 emptyMessage, fonts[AKROBAT_BOLD_20], 0, 8);
  m->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  return m;
}

void MapsApp::enterState(MapsState_t st) {
  appState = st;
  freeWidgets();
  controlState.setInputState(InputType::Numeric);
  if (dlHeld && st != MAPS_DOWNLOAD) {
    controlState.holdScreenAwake(false);   // the progress screen is the only one that holds it
    dlHeld = false;
  }

  switch (st) {
  case MAPS_VIEW:
    snprintf(headerTitle, sizeof(headerTitle), "Maps");
    header->setTitle(headerTitle);
    footer->setButtons("Menu", "Back");     // the soft key IS the menu; OK is the pin
    break;
  case MAPS_MENU:
    snprintf(headerTitle, sizeof(headerTitle), "Map menu");
    header->setTitle(headerTitle);
    footer->setButtons("Select", "Map");
    buildMenu();
    break;
  case MAPS_LIST:
    snprintf(headerTitle, sizeof(headerTitle), "%s",
             listKind == LIST_PINS ? "Pins" : (listKind == LIST_PLACES ? "Places" : "Nodes"));
    header->setTitle(headerTitle);
    // A pin opens its options; a place or a node only moves the map. Say which.
    footer->setButtons(listKind == LIST_PINS ? "Open" : "Go to", "Back");
    buildList();
    break;
  case MAPS_PIN_OPTS:
    snprintf(headerTitle, sizeof(headerTitle), "%s",
             (pinSel >= 0 && pinSel < pinCount) ? pins[pinSel].name : "Pin");
    header->setTitle(headerTitle);
    footer->setButtons("Select", "Back");
    buildPinOpts();
    break;
  case MAPS_RENAME:
    snprintf(headerTitle, sizeof(headerTitle), "Rename pin");
    header->setTitle(headerTitle);
    footer->setButtons("Save", "Clear");        // Back is backspace here; END cancels
    buildRename();
    break;
  case MAPS_AREAS:
    snprintf(headerTitle, sizeof(headerTitle), "Map area");
    header->setTitle(headerTitle);
    footer->setButtons("Use", "Back");
    buildAreas();
    break;
  case MAPS_CONFIRM_DEL:
    snprintf(headerTitle, sizeof(headerTitle), "Delete pin?");
    header->setTitle(headerTitle);
    footer->setButtons("Select", "Back");
    buildConfirmDelete();
    break;
  case MAPS_HELP:
    snprintf(headerTitle, sizeof(headerTitle), "The buttons");
    header->setTitle(headerTitle);
    footer->setButtons("More", "Back");
    helpTop = -1;                        // -1 = the drawn key; 0.. = the text rows after it
    break;
  case MAPS_DOWNLOAD:
    snprintf(headerTitle, sizeof(headerTitle), "Download maps");
    header->setTitle(headerTitle);
    footer->setButtons("Select", "Back");
    dlLastMs = millis();
    dlKeyMs = dlLastMs;
    dlStopArmMs = 0;
    /* Open on the source of the map being LOOKED AT, when its folder is one (otm, usgs-topo,
     * usgs-img): someone viewing OpenTopoMap who opens Download for its z17 must not meet
     * "USGS Topo" and a Detail row that stops at z16. Otherwise the last choice (NVS). It is
     * saved only by a Start. */
    if (areaSel >= 0 && areaSel < areaCount) {
      for (int i = 0; i < tileSourceCount(); i++) {
        /* The FOLDER each source writes (0.9.82: the custom source's is its chosen area). The
         * fixed sources come first, so a relay into "otm" never displaces OpenTopoMap here. */
        const char* folder = tileSourceFolder(i);
        if (folder && !strcasecmp(folder, areas[areaSel].name)) {
          dlSource = i;
          break;
        }
      }
    }
    buildDownload();
    break;
  case MAPS_GOTO:
    snprintf(headerTitle, sizeof(headerTitle), "Go to coordinates");
    header->setTitle(headerTitle);
    footer->setButtons("Go", "Delete");      // Back is backspace here; END cancels
    gotoLat[0] = gotoLon[0] = '\0';
    gotoField = 0;
    break;
  case MAPS_CHANNEL:
    snprintf(headerTitle, sizeof(headerTitle), "Share on which channel?");
    header->setTitle(headerTitle);
    footer->setButtons("Share", "Back");
    chanPending = 0;
    buildChannels();
    break;
  }
}

void MapsApp::rescanCard() {
  char was[32];
  was[0] = '\0';
  if (areaSel >= 0) {
    strlcpy(was, areas[areaSel].name, sizeof(was));
  }
  cancelLoad();
  forgetMissing();
  for (int i = 0; i < MAPS_TILE_SLOTS; i++) {
    slots[i].z = -1;
    slots[i].ready = false;
  }
  /* ⚠ SAVE BEFORE RE-READING. loadPins() overwrites the in-RAM table from the file, so
   * any edit not yet on the card — every path here writes immediately today, but that
   * is a property of the callers, not of this code — would be silently discarded by a
   * menu row whose name promises only to look at the card again. */
  if (pinsDirty) {
    savePins();
  }
  scanAreas();
  loadPins();
  pinSel = -1;               // the array was just re-read; the old index means nothing
  areaSel = -1;
  for (int i = 0; i < areaCount; i++) {
    if (was[0] && !strcmp(areas[i].name, was)) {
      areaSel = i;
    }
  }
  if (areaSel < 0 && areaCount > 0) {
    areaSel = 0;
  }
  if (areaSel >= 0) {
    /* ⚠ cx/cy ARE WORLD PIXELS AT THE CURRENT ZOOM, so changing `zoom` without
     * rescaling them moves the view to completely different ground — at one level out
     * that is half the world away. setArea() already does this correctly; the rescan
     * path did not, and a card whose new area starts at a different zoom would have
     * dropped the user somewhere in the ocean. Read the ground, change the zoom, put
     * the ground back. */
    double keepLat = 0, keepLon = 0;
    mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &keepLat, &keepLon);
    if (zoom < areas[areaSel].zMin) {
      zoom = areas[areaSel].zMin;
    }
    if (zoom > areas[areaSel].zMax + MAP_OVERZOOM_MAX) {
      zoom = areas[areaSel].zMax + MAP_OVERZOOM_MAX;
    }
    centreOn(keepLat, keepLon);
  }
}

void MapsApp::buildMenu() {
  menu = newMenu("");
  char row[64];
  menu->addOption("What the buttons do...", ROW_M_HELP);
  snprintf(row, sizeof(row), "Pins (%d)...", pinCount);
  menu->addOption(row, ROW_M_PINS);
  snprintf(row, sizeof(row), "Places from the mesh (%d)...", meshService.getWaypointCount());
  menu->addOption(row, ROW_M_PLACES);
  snprintf(row, sizeof(row), "Nodes with a position...");
  menu->addOption(row, ROW_M_NODES);
  menu->addOption("Go to coordinates...", ROW_M_GOTO);
  menu->addOption("Centre on me", ROW_M_ME);
  menu->addOption(followMe ? "Follow me: ON (scroll stops it)" : "Follow me: off", ROW_M_FOLLOW);
  menu->addOption(measuring ? "Stop measuring" : "Measure from here...", ROW_M_MEASURE);
  menu->addOption(snapPins ? "Snap to markers: ON" : "Snap to markers: off", ROW_M_SNAP);
  menu->addOption(altOn ? "Altitude: ON" : "Altitude: off", ROW_M_ALT);
  menu->addOption(unitsSettingRow(gUnits), ROW_M_UNITS);   // the Almanac's row, word for word
  menu->addOption(tileFetchActive() ? "Downloading maps..." : "Download maps...", ROW_M_DOWNLOAD);
  if (areaCount > 1) {
    snprintf(row, sizeof(row), "Map area: %s", areaSel >= 0 ? areas[areaSel].name : "-");
    menu->addOption(row, ROW_M_AREA);
  }
  menu->addOption("Rescan the card", ROW_M_RESCAN);
  menu->addOption("Back to the map", ROW_M_BACK);
  if (areaSel < 0) {
    menu->addNote("No tiles on the card - see docs/maps.md");
  }
}

/* ── THE LISTS ARE NEAREST-FIRST, FROM WHERE YOU ARE ────────────────────────────────────
 * COVEY's GO picker (D-126): every row carries "1.4km NE" and the list is sorted by it, so the
 * question "who is closest" is answered by the first row rather than by reading twenty. The
 * reference is the chain selfPosition() already ranks — a live fix, your declared pin, a
 * stale fix — and, with none of those, the crosshair: indoors the column still says how far
 * from the place you are LOOKING AT, which is what the map is for. The rows carry the
 * IDENTITY of the thing (see listId), so sorting here cannot change what OK opens. */
struct MapsListRow {
  uint32_t id;
  double   dist;         // metres from the reference
  char     text[72];
};

static int listRowCmp(const void* a, const void* b) {
  const double da = ((const MapsListRow*)a)->dist, db = ((const MapsListRow*)b)->dist;
  return da < db ? -1 : (da > db ? 1 : 0);
}

void MapsApp::buildList() {
  const char* empty = (listKind == LIST_PINS)
                      ? "No pins yet - OK on the map drops one"
                      : (listKind == LIST_PLACES ? "No places heard from the mesh yet"
                         : "No node has sent a position yet");
  menu = newMenu(empty);
  listCount = 0;

  // Where distances are measured from, and what to call it.
  int32_t rLat = 0, rLon = 0;
  uint32_t age = 0;
  int selfSrc = SELF_NONE;
  uint16_t selfAcc = 0;
  const int selfKind = selfPosition(&rLat, &rLon, &age, &selfSrc, &selfAcc);
  const char* from = "you";
  char fromNet[24];
  if (selfKind == MAPS_SELF_GPS && selfSrc == SELF_NET) {
    /* "nearest first, from Wi-Fi +/-60 m": every distance below is that circle's, not a point's. */
    snprintf(fromNet, sizeof(fromNet), "Wi-Fi +/-%u m", (unsigned)selfAcc);
    from = fromNet;
  } else if (selfKind == MAPS_SELF_NONE) {
    double lat = 0, lon = 0;
    mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
    rLat = mapDegToI7(lat);
    rLon = mapDegToI7(lon);
    from = "the crosshair";
  } else if (selfKind == MAPS_SELF_PIN) {
    from = "your pin";
  }

  /* PSRAM, not the GUI task's stack: 64 rows of 84 bytes is 5 KB, and the loop task's stack
   * has been measured at a few hundred bytes to spare. */
  MapsListRow* rows = (MapsListRow*)ps_malloc(sizeof(MapsListRow) * MAPS_MAX_LIST);
  if (!rows) {
    menu->addNote("No memory for the list");
    return;
  }
  int n = 0;
  char d[16];
  if (listKind == LIST_PINS) {
    for (int i = 0; i < pinCount && n < MAPS_MAX_LIST; i++) {
      MapsListRow* r = &rows[n++];
      r->id = (uint32_t)i;                     // this app owns the pin array; index is identity
      r->dist = meshPosDistanceM(rLat, rLon, pins[i].latI, pins[i].lonI);
      unitsFmtDist(r->dist, gUnits, d, sizeof(d));
      snprintf(r->text, sizeof(r->text), "%s %s  %s%s", d,
               meshPosCompass8(meshPosBearingDeg(rLat, rLon, pins[i].latI, pins[i].lonI)),
               pins[i].name, pins[i].sharedId ? " (shared)" : "");
    }
  } else if (listKind == LIST_PLACES) {
    const int cnt = meshService.getWaypointCount();
    for (int i = 0; i < cnt && n < MAPS_MAX_LIST; i++) {
      const MeshWaypoint* w = meshService.getWaypoint(i);
      if (!w || !w->id) {
        continue;
      }
      MapsListRow* r = &rows[n++];
      r->id = w->id;                           // the waypoint table compacts; the id does not
      r->dist = meshPosDistanceM(rLat, rLon, w->latI, w->lonI);
      unitsFmtDist(r->dist, gUnits, d, sizeof(d));
      snprintf(r->text, sizeof(r->text), "%s %s  %s", d,
               meshPosCompass8(meshPosBearingDeg(rLat, rLon, w->latI, w->lonI)), w->name);
    }
  } else {
    /* The service re-sorts the node list only when a screen asks it to, and this is such a
     * screen — the same contract the Meshtastic app's Nodes list follows. */
    meshService.refreshNodeOrder();
    const uint32_t me = meshService.getMyNodeNum();
    const uint32_t now = millis();
    const int cnt = meshService.getNodeCount();
    for (int i = 0; i < cnt && n < MAPS_MAX_LIST; i++) {
      const MeshNode* nd = meshService.getNode(i);
      if (!nd || !nd->nodeNum || nd->nodeNum == me || !nd->posHeardMs) {
        continue;
      }
      MapsListRow* r = &rows[n++];
      r->id = nd->nodeNum;
      r->dist = meshPosDistanceM(rLat, rLon, nd->latI, nd->lonI);
      unitsFmtDist(r->dist, gUnits, d, sizeof(d));
      const unsigned mins = (unsigned)((now - nd->posHeardMs) / 60000u);
      // "~": an approximate position (precision_bits), the same mark the Meshtastic lists use.
      snprintf(r->text, sizeof(r->text), "%s%s %s  %s, %um ago",
               meshPosIsApprox(nd->posPrec) ? "~" : "", d,
               meshPosCompass8(meshPosBearingDeg(rLat, rLon, nd->latI, nd->lonI)),
               nd->name[0] ? nd->name : "?", mins);
    }
  }
  qsort(rows, (size_t)n, sizeof(MapsListRow), listRowCmp);
  for (int i = 0; i < n; i++) {
    listId[listCount] = rows[i].id;
    menu->addOption(rows[i].text, ROW_FIRST + listCount);
    listCount++;
  }
  free(rows);
  if (listCount > 0) {
    char note[48];
    snprintf(note, sizeof(note), "nearest first, from %s", from);
    menu->addNoteWrapped(note);
  }
  if (listCount >= MAPS_MAX_LIST) {
    menu->addNote("(more not shown)");
  }
}

void MapsApp::buildPinOpts() {
  menu = newMenu("");
  if (pinSel < 0 || pinSel >= pinCount) {
    menu->addNote("That pin is gone");
    menu->addOption("Back", ROW_P_CANCEL);
    return;
  }
  char row[64];
  menu->addOption("Rename...", ROW_P_RENAME);
  menu->addOption("Move it to the crosshair", ROW_P_MOVE);
  /* The rows say WHERE. A pin that has a channel goes back there in one press; the picker is
   * a separate row so changing channel is a choice and never a surprise. */
  const MeshChannel* ch = pinChannel(pinSel);
  if (pins[pinSel].sharedId && pins[pinSel].chan[0] && !ch) {
    /* Shared on a channel this phone no longer has: nothing can be sent for it. Say so,
     * offer nothing that would pretend otherwise, and keep the id for the day it is back. */
    snprintf(row, sizeof(row), "Channel '%s' is gone - cannot update", pins[pinSel].chan);
    menu->addNoteWrapped(row);
    menu->addNoteWrapped("Add the channel back to send or retract");
  } else if (pins[pinSel].sharedId) {
    snprintf(row, sizeof(row), "Update it on %s", ch ? ch->name : "the mesh");
    menu->addOption(row, ROW_P_SHARE);
    menu->addOption("Take it off the mesh", ROW_P_UNSHARE);
  } else if (ch) {
    snprintf(row, sizeof(row), "Share it on %s", ch->name);
    menu->addOption(row, ROW_P_SHARE);
    menu->addOption("Share on another channel...", ROW_P_SHARE_ON);
  } else {
    menu->addOption("Share it on the mesh...", ROW_P_SHARE);
  }
  menu->addOption("Centre the map on it", ROW_P_CENTRE);
  menu->addOption("Delete", ROW_P_DELETE);
  menu->addOption("Back", ROW_P_CANCEL);
  snprintf(row, sizeof(row), "%.5f, %.5f",
           mapI7ToDeg(pins[pinSel].latI), mapI7ToDeg(pins[pinSel].lonI));
  menu->addNote(row);
  if (elevActive()) {
    /* The pin's ground, from the CACHE: "Ground 412m, 35m above you", or "Ground ..." until
     * the app timer has read it (elevStep rebuilds this screen then, keeping the thumb's row).
     * Nothing here reads the card - this runs from key handlers. Wrapped, not fitted: a menu
     * note has the rows to spare. */
    const bool current = elevPin.valid && elevPin.latI == pins[pinSel].latI &&
                         elevPin.lonI == pins[pinSel].lonI;
    elevRowText(row, sizeof(row), "Ground", &elevPin, current, 0);
    menu->addNoteWrapped(row);
  }
}

/* The elevation layer's account of a run (0.9.80), for the form: its own counts - an elevation
 * failure is never one of the map's - and, when it gave up, why. "" when the job has no layer. */
static void dlElevSummary(char* row, size_t cap, const TileJobStatus& st) {
  row[0] = '\0';
  if (!st.elev) {
    return;
  }
  int w = snprintf(row, cap, "Elevation: %d new, %d already had", st.elevDone, st.elevSkipped);
  if (w > 0 && (size_t)w < cap && st.elevNoTile > 0) {
    w += snprintf(row + w, cap - (size_t)w, ", %d none there", st.elevNoTile);
  }
  if (w > 0 && (size_t)w < cap) {
    w += snprintf(row + w, cap - (size_t)w, ", %d failed", st.elevFailed);
  }
  if (w > 0 && (size_t)w < cap && st.elevGaveUp) {
    w += snprintf(row + w, cap - (size_t)w, " - stopped trying this run");
  }
  if (w > 0 && (size_t)w < cap && st.elevFailed > 0 && st.elevErr[0]) {
    snprintf(row + w, cap - (size_t)w, " (last: %s)", st.elevErr);
  }
}

void MapsApp::buildDownload() {
  /* Keep the thumb where it was across the once-a-second progress rebuilds; a row that no
   * longer exists (Start while running, Stop when done) falls back to the one that replaced it. */
  MenuOption::keyType keep = menu ? menu->currentKey() : dlKeep;
  if (keep == 0 || keep == MENU_ROW_NOTE) {
    keep = dlKeep;
  }
  menu = newMenu("");
  char row[96];   // the RAM line ("Needs 13.6 KB free (has 13.5) and a 9.7 KB block (has 10.2)") is 59
  const TileSource* src = tileSource(dlSource);
  if (!src) {
    dlSource = 0;
    src = tileSource(0);
  }
  /* The run's status and the job's summary are ~600 bytes together: PSRAM, allocated on the
   * first build and kept, NOT the loop task's 8 KB stack — this runs from the GUI's key path,
   * among the deepest the loop has, and its measured floor is a few hundred bytes. */
  static TileJobStatus*    s_dlSt = NULL;
  static TileFetchJobInfo* s_dlJob = NULL;
  static char*             s_dlElevRow = NULL;   // the elevation account, with its last error: ~150 chars
  if (!s_dlSt) s_dlSt = (TileJobStatus*)ps_malloc(sizeof(TileJobStatus));
  if (!s_dlJob) s_dlJob = (TileFetchJobInfo*)ps_malloc(sizeof(TileFetchJobInfo));
  if (!s_dlElevRow) s_dlElevRow = (char*)ps_malloc(MAPS_DL_ELEV_ROW);
  if (!s_dlSt || !s_dlJob || !s_dlElevRow) {
    menu->addNoteWrapped("No memory for the download form");
    menu->addOption("Back", ROW_D_BACK);
    menu->select(ROW_D_BACK);
    return;
  }
  TileJobStatus& st = *s_dlSt;
  TileFetchJobInfo& job = *s_dlJob;
  tileFetchStatus(&st);
  tileFetchJobInfo(&job);
  const bool running = st.active;
  const bool battLow = !controlState.usbConnected && controlState.battVoltage < MAPS_DL_BATT_FLOOR;
  if (dlStopArmMs && (uint32_t)(millis() - dlStopArmMs) >= MAPS_DL_STOP_ARM_MS) {
    dlStopArmMs = 0;                       // the second press did not come: Stop is Stop again
  }
  /* Every note on this screen WRAPS (addNoteWrapped): these are the lines whose length the
   * numbers decide — "12853 tiles, 1.6 GB, up to about 4 h", "Last run: 5517 new, 7332 had, 4
   * failed, 7013 s", an error from the fetcher — and a single row ended in `..` with the part
   * that mattered behind it ({user}, 2026-09-20). */
  MenuOption::keyType fallback;

  if (job.exists) {
    /* ── A JOB IS RUNNING OR WAITING TO RESUME: show THAT job, read-only. The choice rows are
     * for the next Start, and a Start now would replace a multi-day job whose centre was the
     * map centre days ago — so Start is not offered until this one is stopped or done. */
    snprintf(row, sizeof(row), "%s, %d km to z%d%s, around %.2f, %.2f",
             job.label, job.radiusKm, job.zMax, job.elev ? " + elevation" : "", job.lat, job.lon);
    menu->addNoteWrapped(row);
    if (running) {
      if (st.pass == 2) {
        snprintf(row, sizeof(row), "Second pass: %d of %d looked at again%s",
                 st.p2Seen, st.p2Total, st.stopping ? " (stopping)" : "");
      } else {
        const char* what = st.waitingRam ? "Waiting for memory" : "Downloading";
        if (st.paused) {
          what = st.pauseWhy == 2 ? "Paused for the game" : (st.pauseWhy == 3 ? "Paused: no WiFi" : "Paused for a call");
        }
        if (st.elevRunning) {
          /* The elevation layer goes first in every run (tile_fetch.h): its own count, labelled,
           * so "0 of 4567" does not sit there looking stuck while it runs. */
          const int seen = st.elevDone + st.elevSkipped + st.elevNoTile + st.elevFailed;
          if (!st.paused && !st.waitingRam) {
            snprintf(row, sizeof(row), "Downloading elevation %d of %d%s", seen, st.elevTotal,
                     st.stopping ? " (stopping)" : "");
          } else {
            snprintf(row, sizeof(row), "%s (elevation %d of %d)%s", what, seen, st.elevTotal,
                     st.stopping ? " (stopping)" : "");
          }
        } else {
          snprintf(row, sizeof(row), "%s %d of %d%s", what,
                   st.before + st.done + st.skipped + st.noTile + st.failed, st.total,
                   st.stopping ? " (stopping)" : "");
        }
      }
      menu->addNoteWrapped(row);
      if (st.serverWaitSec) {
        snprintf(row, sizeof(row), "Server busy - trying again in %u min", (unsigned)((st.serverWaitSec + 59u) / 60u));
        menu->addNoteWrapped(row);
      }
      if (st.waitingRam) {
        /* The numbers, not the adjective: a TLS handshake needs this much internal RAM to be
         * safe, and this is what the phone has. {user}, 2026-09-21: "waiting for memory is always
         * being displayed" — for two hours, with nothing to say why or by how much. */
        snprintf(row, sizeof(row), "Needs %u.%u KB free (has %u.%u) and a %u.%u KB block (has %u.%u)",
                 (unsigned)(st.ramNeedFree / 1024), (unsigned)(((st.ramNeedFree % 1024) * 10) / 1024),
                 (unsigned)(st.ramFree / 1024), (unsigned)(((st.ramFree % 1024) * 10) / 1024),
                 (unsigned)(st.ramNeedLargest / 1024), (unsigned)(((st.ramNeedLargest % 1024) * 10) / 1024),
                 (unsigned)(st.ramLargest / 1024), (unsigned)(((st.ramLargest % 1024) * 10) / 1024));
        menu->addNoteWrapped(row);
      }
      if (st.elevRunning) {
        snprintf(row, sizeof(row), "Elevation z%d, %u KB, %u s, %d failed",
                 st.elevZ, (unsigned)(st.bytes / 1024), (unsigned)(st.elapsedMs / 1000), st.elevFailed);
      } else {
        snprintf(row, sizeof(row), "z%d, %u KB, %u s, %d failed",
                 st.curZ, (unsigned)(st.bytes / 1024), (unsigned)(st.elapsedMs / 1000), st.failed);
      }
      menu->addNoteWrapped(row);
      if (st.elev && !st.elevRunning) {
        dlElevSummary(s_dlElevRow, MAPS_DL_ELEV_ROW, st);   // the layer is done for this run: what it came to
        menu->addNoteWrapped(s_dlElevRow);
      }
      /* The live time left: what is left of the square times the mean all-in time of the last
       * 200 tiles actually FETCHED (skips take milliseconds and are left out). An upper bound:
       * tiles already on the card ahead of the cursor go by in milliseconds too. */
      if (st.msPerTile) {
        const int left = st.pass == 2 ? st.p2Total - st.p2Seen
                                      : st.total - (st.before + st.done + st.skipped + st.noTile + st.failed);
        if (left > 0) {
          char t[24];
          tilePlanDuration((double)left * st.msPerTile / 1000.0, t, sizeof(t));
          snprintf(row, sizeof(row), "About %s left", t);
          menu->addNoteWrapped(row);
        }
      }
    } else {
      snprintf(row, sizeof(row), "%d of %d done", job.reached, job.total);
      menu->addNoteWrapped(row);
      if (job.why[0]) {
        menu->addNoteWrapped(job.why);
      }
      if (st.lastErr[0]) {
        char why[128];
        snprintf(why, sizeof(why), "Last problem: %s", st.lastErr);
        menu->addNoteWrapped(why);
      }
    }
    if (job.note[0]) {
      menu->addNoteWrapped(job.note);
    }
    if (job.resumable && job.giveUp == TILE_GIVEUP_NONE) {
      menu->addNoteWrapped("Resumes by itself after a WiFi drop or a restart");
    }
    if (dlWhy[0]) {
      menu->addNoteWrapped(dlWhy);
    }
    fallback = ROW_D_STOP;
    if (!running && job.gate != TILE_GATE_GO) {
      /* A waiting job can be started NOW by a person: on another network ("Paused: resumes on
       * Home, where it started" — the hotspot on a trip is someone's data plan, so it never
       * does that by itself), after a give-up, or through a cool-down. Same power rule as Start. */
      if (battLow) {
        snprintf(row, sizeof(row), "Battery %.2f V - plug in USB to download", controlState.battVoltage);
        menu->addNoteWrapped(row);
      } else if (WiFi.status() == WL_CONNECTED) {
        menu->addOption(job.otherNet ? "Resume on this network" : "Resume it now", ROW_D_RESUME);
        fallback = ROW_D_RESUME;
      }
    }
    if (dlStopArmMs) {
      snprintf(row, sizeof(row), "Stopping forgets it: %d of %d done, and it will not resume",
               job.reached, job.total);
      menu->addNoteWrapped(row);
      menu->addOption("Press again to stop", ROW_D_STOP);
    } else {
      menu->addOption("Stop", ROW_D_STOP);
    }
    if (keep != ROW_D_STOP && keep != ROW_D_RESUME && keep != ROW_D_BACK) {
      keep = fallback;
    }
    if (keep == ROW_D_RESUME && fallback != ROW_D_RESUME) {
      keep = ROW_D_STOP;
    }
  } else {
    // ── No job: the choices, the estimate, and what the last run came to. ──
    snprintf(row, sizeof(row), "Source: %s", src->label);
    menu->addOption(row, ROW_D_SOURCE);
    if (gUnits == UNITS_US) {
      /* The radii are COVEY's round kilometres (the plan is in km); US says what that is. */
      char mi[16];
      unitsFmtDist(MAPS_DL_RADII[dlRadiusIdx] * 1000.0, UNITS_US, mi, sizeof(mi));
      snprintf(row, sizeof(row), "Radius: %d km (%s)", MAPS_DL_RADII[dlRadiusIdx], mi);
    } else {
      snprintf(row, sizeof(row), "Radius: %d km", MAPS_DL_RADII[dlRadiusIdx]);
    }
    menu->addOption(row, ROW_D_RADIUS);
    /* The depth SHOWN is the depth USED: the wish clamped to what this source has. */
    const int depth = tilePlanDepthShown(dlDepth, src->zMax);
    /* z15's pixel at this latitude in the Units setting (~3 m, 11 ft at 47 N; review
     * 2026-09-27: it was a metric literal) - tile_plan.cpp, host-tested. */
    {
      double cLat = 0, cLon = 0;
      mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &cLat, &cLon);
      tilePlanDetailText(row, sizeof(row), depth, cLat, gUnits);
    }
    menu->addOption(row, ROW_D_DEPTH);
    /* 0.9.80: the altitude layer over the same square (z13 + z10 from the AWS Terrain Tiles), so an
     * area downloaded on the phone has heights too. ON by default; a few percent of the job. */
    menu->addOption(dlElev ? "Elevation too: ON" : "Elevation too: off", ROW_D_ELEV);

    TileJobSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.source = dlSource;
    mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &spec.lat, &spec.lon);
    spec.radiusKm = MAPS_DL_RADII[dlRadiusIdx];
    spec.zMax = depth;
    spec.elev = dlElev;
    uint64_t card = 0, net = 0;
    int elevTiles = 0;
    const int tiles = tileFetchEstimate(&spec, &card, &net, &elevTiles);
    const double secs = tilePlanSeconds(spec.lat, spec.lon, spec.radiusKm, depth,
                                        MAPS_DL_SEC_PER_TILE[dlSource < 4 ? dlSource : 3], src->key) +
                        (double)elevTiles * MAPS_DL_ELEV_SEC_PER_TILE;
    char size[24], t[24];
    tilePlanBytes(card, size, sizeof(size));
    tilePlanDuration(secs, t, sizeof(t));
    /* "up to": tiles already on the card are skipped in milliseconds, so this is a ceiling. The
     * size and the time include the elevation tiles; the count names them apart. */
    if (elevTiles > 0) {
      snprintf(row, sizeof(row), "%d tiles + %d elevation, %s, up to about %s", tiles, elevTiles, size, t);
    } else {
      snprintf(row, sizeof(row), "%d tiles, %s, up to about %s", tiles, size, t);
    }
    menu->addNoteWrapped(row);
    const bool tooMany = tiles > TILE_JOB_MAX_TILES;
    if (tooMany) {
      snprintf(row, sizeof(row), "Too many tiles for one run (%d max) - a smaller radius or less detail", TILE_JOB_MAX_TILES);
      menu->addNoteWrapped(row);
    } else if (secs > 4.0 * 3600.0) {
      /* A run this long outlasts the battery: off USB it stops under the floor and waits. */
      snprintf(row, sizeof(row), "Keep it on USB: about %s of downloading", t);
      menu->addNoteWrapped(row);
    }

    if (st.finished) {
      snprintf(row, sizeof(row), "Last run: %d new, %d already had, %d failed, %u s",
               st.done, st.skipped, st.failed, (unsigned)(st.elapsedMs / 1000));
      menu->addNoteWrapped(row);
      if (st.elev) {
        dlElevSummary(s_dlElevRow, MAPS_DL_ELEV_ROW, st);   // "Elevation: 38 new, 2 already had, 3 failed"
        menu->addNoteWrapped(s_dlElevRow);
      }
      if (st.p2Tried > 0) {
        snprintf(row, sizeof(row), "Second pass: %d tried again, %d fixed", st.p2Tried, st.p2Fixed);
        menu->addNoteWrapped(row);
      }
      if (st.cardRetries > 0) {
        /* The card's own number: how often a write had to wait for it. A few is normal;
         * one in eight (phone 1, 2026-09-21) is a card worth replacing before the woods. */
        snprintf(row, sizeof(row), "The card was busy %d time%s (each write waited and went through)",
                 st.cardRetries, st.cardRetries == 1 ? "" : "s");
        menu->addNoteWrapped(row);
      }
      if (st.lastErr[0]) {
        /* Named for what it is. "z16 10714/23006: card refused the write" on its own read
         * as a mystery ({user}: "something about card refused... idk what that means"); it is
         * the last tile that failed and why, and Start fetches the missing ones again. */
        char why[128];
        snprintf(why, sizeof(why), "Last problem: %s", st.lastErr);
        menu->addNoteWrapped(why);
        if (st.failed > st.p2Fixed) {
          menu->addNoteWrapped("Start again fetches only the missing tiles");
        }
      }
    }
    if (job.note[0]) {
      menu->addNoteWrapped(job.note);      // a waiting job that was dropped, and why
    }
    if (dlWhy[0]) {
      menu->addNoteWrapped(dlWhy);
    }
    fallback = ROW_D_SOURCE;
    if (WiFi.status() != WL_CONNECTED) {
      menu->addNoteWrapped("Not on WiFi - join a network first");
    } else if (battLow) {
      snprintf(row, sizeof(row), "Battery %.2f V - plug in USB to download", controlState.battVoltage);
      menu->addNoteWrapped(row);
    } else if (!tooMany) {
      menu->addOption("Start download", ROW_D_START);
      fallback = ROW_D_START;
    }
    if (keep == ROW_D_STOP || keep == ROW_D_RESUME || (keep == ROW_D_START && fallback != ROW_D_START)) {
      keep = fallback;
    }
  }
  menu->addOption("Back", ROW_D_BACK);
  if (src->credit[0] && !job.exists) {
    menu->addNoteWrapped(src->credit);
  }
  if (dlElev && !job.exists) {
    menu->addNoteWrapped("Elevation: AWS Terrain Tiles (USGS 3DEP, SRTM, GMTED, ETOPO)");
  }
  dlShownRunning = running;
  dlShownJob = job.exists;
  menu->select(keep);
  /* Hold the screen while the progress is being WATCHED: a 30 s sleep would lock the phone with
   * the cancel key behind the unlock chord. Released the moment another screen is entered —
   * and MAPS_DL_HOLD_MS after the last key, because a form left open on a multi-day job would
   * otherwise keep the backlight on for days. The job carries on either way. */
  const bool hold = running && (uint32_t)(millis() - dlKeyMs) < MAPS_DL_HOLD_MS;
  if (hold && !dlHeld) {
    controlState.holdScreenAwake(true);
    dlHeld = true;
  } else if (!hold && dlHeld) {
    controlState.holdScreenAwake(false);
    dlHeld = false;
  }
}

void MapsApp::drawGoto() {
  const int top = (int)header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  SmoothFont* big = fonts[AKROBAT_BOLD_20];
  SmoothFont* small = fonts[AKROBAT_BOLD_16];
  if (!big || !small) {
    return;
  }
  lcd.setTextDatum(TL_DATUM);
  const int boxH = 30;
  const char* labels[2] = { "Latitude", "Longitude" };
  const char* vals[2] = { gotoLat, gotoLon };
  int y = top + 12;
  for (int i = 0; i < 2; i++) {
    lcd.setTextFont(small);
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString(labels[i], 12, y);
    y += (int)small->height() + 2;
    const bool active = (gotoField == i);
    lcd.fillRect(8, y, lcd.width() - 16, boxH, active ? WHITE : MAP_C_STRIP);
    lcd.setTextFont(big);
    lcd.setTextColor(active ? BLACK : WHITE, active ? WHITE : MAP_C_STRIP);
    lcd.drawString(vals[i][0] ? vals[i] : (i == 0 ? "47.42" : "-121.75"), 14, y + 5);
    if (!vals[i][0]) {
      // the placeholder is an example, drawn dim
      lcd.fillRect(8, y, lcd.width() - 16, boxH, active ? WHITE : MAP_C_STRIP);
      lcd.setTextColor(active ? MAP_C_STRIP : MAP_C_NODE_OLD, active ? WHITE : MAP_C_STRIP);
      lcd.drawString(i == 0 ? "e.g. 47.42" : "e.g. -121.75", 14, y + 5);
    }
    y += boxH + 12;
  }
  lcd.setTextFont(small);
  lcd.setTextColor(MAP_C_NODE_OLD, BLACK);
  lcd.drawString("digits type   * point   # minus", 12, y);
  y += (int)small->height() + 2;
  lcd.drawString("up/down field   Back deletes", 12, y);
  y += (int)small->height() + 2;
  lcd.drawString("OK goes there   End cancels", 12, y);
  y += (int)small->height() + 8;
  double lat = 0, lon = 0;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
  char now[64];
  snprintf(now, sizeof(now), "Crosshair: %.5f, %.5f", lat, lon);
  lcd.setTextColor(WHITE, BLACK);
  lcd.drawString(now, 12, y);
  /* ...and its ground, when the map had read it for this view before Go to was opened (the
   * cache only: this is a draw). Not read here otherwise - the view does not move on this
   * screen, and the map reads it again when you are back. */
  if (elevActive() && elevHereCurrent()) {
    y += (int)small->height() + 2;
    elevRowText(now, sizeof(now), NULL, &elevHere, true, lcd.width() - 24);
    guiDrawEllipsized(lcd, now, (uint16_t)(lcd.width() - 24), 12, (int16_t)y);
  }
}

const MeshChannel* MapsApp::pinChannel(int idx) const {
  if (!pins || idx < 0 || idx >= pinCount || !pins[idx].chan[0]) {
    return NULL;
  }
  return meshService.findChannelByName(pins[idx].chan);
}

void MapsApp::buildChannels() {
  /* Rows with a subtitle need the taller pitch the Meshtastic screens use (five rows per
   * screen); the map's usual single-line menu draws the subtitle over the next row. */
  freeMenu();
  menu = new MenuWidget(0, header->height(), lcd.width(),
                        lcd.height() - header->height() - footer->height(),
                        "No channels on this phone", fonts[AKROBAT_EXTRABOLD_22], 5, 8);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  char row[40];
  const int n = meshService.getChannelCount();
  const char* have = (chanForPin >= 0 && chanForPin < pinCount) ? pins[chanForPin].chan : "";
  const char* beacon = meshService.getPosChannelName();
  MenuOption::keyType pick = 0;
  for (int i = 0; i < n; i++) {
    const MeshChannel* c = meshService.getChannel(i);
    if (!c) {
      continue;
    }
    const MenuOption::keyType key = (MenuOption::keyType)(ROW_C_BASE + i);
    if (meshService.channelIsPublic(c)) {
      snprintf(row, sizeof(row), "PUBLIC %s", c->name);
      menu->addOption(row, chanPending == (int)key ? "PUBLIC - press again to confirm"
                                                   : "PUBLIC - any radio in range", key, 1);
    } else if (have[0] && strcmp(have, c->name) == 0) {
      menu->addOption(c->name, "private - this pin is here now", key, 1);
    } else if (beacon[0] && strcmp(beacon, c->name) == 0) {
      menu->addOption(c->name, "private - your position beacon's channel", key, 1);
    } else {
      menu->addOption(c->name, "private", key, 1);
    }
    /* Highlight, in order: where this pin already is, else the beacon's channel, else the
     * first private channel. Never a public one by default. */
    if (!pick) {
      if (have[0] && strcmp(have, c->name) == 0) {
        pick = key;
      } else if (beacon[0] && strcmp(beacon, c->name) == 0) {
        pick = key;
      }
    }
  }
  if (!pick) {
    for (int i = 0; i < n; i++) {
      const MeshChannel* c = meshService.getChannel(i);
      if (c && !meshService.channelIsPublic(c)) {
        pick = (MenuOption::keyType)(ROW_C_BASE + i);
        break;
      }
    }
  }
  menu->addOption("Back", ROW_C_CANCEL);
  if (pick) {
    menu->select(pick);
  }
}

void MapsApp::buildRename() {
  freeMenu();
  delete textArea;
  const int16_t padding = 4;
  textArea = new MultilineTextWidget(0, header->height(), lcd.width(),
                                     lcd.height() - header->height() - footer->height(),
                                     "Pin name", controlState, MAP_PIN_NAME_LEN - 1,
                                     fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum,
                                     padding, padding);
  textArea->setColors(WP_COLOR_1, WP_COLOR_0);
  if (pinSel >= 0 && pinSel < pinCount) {
    textArea->setText(pins[pinSel].name);
  }
  textArea->cursorToEnd();
  textArea->setFocus(true);
  controlState.setInputState(InputType::AlphaNum);
}

void MapsApp::buildAreas() {
  menu = newMenu("No maps on the card");
  char row[72];
  double lat = 0, lon = 0;
  mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
  for (int i = 0; i < areaCount; i++) {
    char where[24];
    areaWhere(i, lat, lon, where, sizeof(where));
    snprintf(row, sizeof(row), "%s%s  z%d-%d  %s", areas[i].name, i == areaSel ? " *" : "",
             areas[i].zMin, areas[i].zMax, where);
    menu->addOption(row, ROW_FIRST + i);
  }
  menu->addNoteWrapped("Where each map's tiles are, from the crosshair. The bottom side button cycles them.");
}

void MapsApp::buildConfirmDelete() {
  menu = newMenu("");
  /* ⚠ THE OPTIONS COME FIRST, and that is not cosmetic: a MenuWidget opens on row 0, and with
   * the explanatory notes at the top that row was a display-only note. The footer said
   * "Select" and the first press of it did nothing at all — on the one screen in this app
   * whose whole job is to take an answer. Every other screen here puts its notes last. */
  menu->addOption("No, keep it", ROW_DEL_NO);
  menu->addOption("Yes, delete it", ROW_DEL_YES);
  char row[72];
  if (pinSel >= 0 && pinSel < pinCount) {
    snprintf(row, sizeof(row), "Delete '%s'", pins[pinSel].name);
    menu->addNoteWrapped(row);
    if (pins[pinSel].sharedId) {
      menu->addNoteWrapped(pins[pinSel].chan[0] && !pinChannel(pinSel)
                           ? "(its channel is gone: it cannot be retracted)"
                           : "...and take it off the mesh");
    }
  }
}

/* ⚠ THIS LIST DOES NOT FIT AND USED TO BE SILENTLY CUT. Twenty-odd rows at a 17 px pitch is
 * 370 px in a 250 px window, so the loop's bottom guard threw away everything from the colour
 * key down — the half of this screen that explains what the coloured dots on the map MEAN,
 * on the one screen whose entire job is to explain things. It scrolls now, and says so. */
/* ⚠ ROWS MUST FIT 240 PX AT AKROBAT_BOLD_16 — about 33 characters. A longer row is not
 * clipped at the right; the smooth-font renderer shifts it so BOTH ends are cut (measured:
 * "Hold an arrow..." lost its H and its last digit), which reads as a typo, not a cut. */
static const struct { const char* text; uint16_t colour; } MAPS_HELP_ROWS[] = {
  { "Hold an arrow: it speeds up.", WHITE },
  { "Land near a pin, place or node", WHITE },
  { "  and the map snaps onto it", WHITE },
  { "  (Snap, in the menu). 7 9: pins", WHITE },
  { "2 4 6 8 scroll too, for gloves.", WHITE },
  { "5 is OK. 0 is centre on me.", WHITE },
  { "4th side button: next map type", WHITE },
  { "", WHITE },
  { "In the menu:", MAP_C_PIN },
  { "Download maps - the area around", WHITE },
  { "  the crosshair, over WiFi", WHITE },
  { "Go to - a pin, a node, a place,", WHITE },
  { "  or typed coordinates", WHITE },
  { "Follow me - stays centred on you", WHITE },
  { "  until you scroll", WHITE },
  { "Measure - drops an anchor; scroll", WHITE },
  { "  away and the bar reads it", WHITE },
  /* 0.9.80. Measured with SmoothFont::textWidth's rule over the Akrobat_Bold16 table (the one
   * tests/test_elevtext.cpp reads): 173-217 px. No '~': the font has no glyph for it. */
  { "  (and the climb, with Altitude on)", WHITE },
  { "Altitude - the ground's height", WHITE },
  { "  under the crosshair, and how far", WHITE },
  { "  above or below you. Elev* = the", WHITE },
  { "  coarse 100 m layer. Tiles go in", WHITE },
  { "  /maps/elev (docs/maps.md)", WHITE },
  { "Units - metric, or US (ft, mi)", WHITE },
  { "", WHITE },
  { "orange dot   your pin", MAP_C_PIN },
  { "yellow ring  your pin, shared", MAP_C_PIN_SH },
  { "green gem    place from the mesh", MAP_C_WP },
  { "cyan dot     someone else", MAP_C_NODE },
  { "grey dot     ...over 30 min old", MAP_C_NODE_OLD },
  { "white ring   this phone, live GPS", MAP_C_ME },
  { "grey ring    ...a fix over 2 min old", MAP_C_NODE_OLD }, // 36 chars: fits, measured
  { "white gem    ...your declared pin", MAP_C_ME },
  { "", WHITE },
  { "A name is dropped, never squashed,", WHITE },
  { "when it would land on another.", WHITE },
  { "", WHITE },
  { "grey ring    ...or a poor fix", MAP_C_NODE_OLD },
  { "             (<4 sats or HDOP >10)", MAP_C_NODE_OLD },
  { "grey square  no tile at any level", WHITE },
  { "", WHITE },
  /* The star: ANY position drawn from a shallower tile, not only one level past the deepest
   * (drawMap's stretch walk). Measured with SmoothFont::textWidth's own rule over the
   * Akrobat_Bold16 glyph table: 181-190 px, and 203 for the "grey square" row above. */
  { "A star (z18*): part of the view", MAP_C_PIN },
  { "  is a shallower tile stretched;", WHITE },
  { "  the chip says which and how", WHITE },
  { "  much. The scale bar is right.", WHITE },
  { "", WHITE },
  { "See docs/maps.md for the tiles.", WHITE },
  { "", WHITE },
  { "USGS: public domain (National Map)", WHITE },
  { "OpenTopoMap: (c) OSM contributors,", WHITE },
  { "  SRTM | OpenTopoMap, CC-BY-SA", WHITE },
};
static const int MAPS_HELP_N = (int)(sizeof(MAPS_HELP_ROWS) / sizeof(MAPS_HELP_ROWS[0]));

/* One labelled box of the drawn key. */
static void helpBox(LCD& lcd, int x, int y, int w, int h, const char* label, uint16_t fill) {
  lcd.fillRoundRect(x, y, w, h, 4, fill);
  lcd.drawRoundRect(x, y, w, h, 4, WHITE);
  if (label && label[0]) {
    lcd.setTextDatum(MC_DATUM);
    lcd.setTextColor(fill == BLACK ? WHITE : BLACK, fill);
    lcd.drawString(label, x + w / 2, y + h / 2);
    lcd.setTextDatum(TL_DATUM);
  }
}

/* The phone's buttons, drawn where they are on the phone, each saying what it does on the
 * map. {user} asked for a key that makes sense at a glance (2026-09-19); a table of key names
 * asks the reader to know which key is which — this shows them. Side buttons down the left
 * edge as they are on the case, soft keys under the screen, the D-pad in the middle. */
void MapsApp::drawHelpDiagram() {
  const int top = (int)header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  SmoothFont* small = fonts[AKROBAT_BOLD_16];
  if (!small) {
    return;
  }
  lcd.setTextFont(small);
  const int th = (int)small->height();
  const uint16_t KEY = 0x39E7;                // a dark grey key face

  // ── side buttons, top to bottom, at the left edge (as on the case) ──
  const int sx = 4, sw = 26, sh = 20;
  int sy = top + 6;
  const char* sideLabel[4] = { "1", "2", "3", "4" };
  /* ⚠ Row 4 shares its line with the D-pad's "arrows scroll" caption (x from ~111): "next
   * map type" ran under it, seen on phone 1. Eight characters is the room there is. */
  const char* sideDoes[4]  = { "zoom in", "zoom out", "centre on me", "map type" };
  for (int i = 0; i < 4; i++) {
    helpBox(lcd, sx, sy, sw, sh, sideLabel[i], KEY);
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString(sideDoes[i], sx + sw + 5, sy + 2);
    sy += sh + 4;
  }
  lcd.setTextColor(MAP_C_NODE_OLD, BLACK);
  lcd.drawString("side buttons", sx, sy);

  // ── the D-pad on the right: arrows scroll, the centre is the pin ──
  const int a = 20, gap = 2, cxp = 156, cyp = top + 6 + a + gap + a / 2;
  helpBox(lcd, cxp - a / 2, cyp - a / 2 - gap - a, a, a, "^", KEY);
  helpBox(lcd, cxp - a / 2, cyp + a / 2 + gap, a, a, "v", KEY);
  helpBox(lcd, cxp - a / 2 - gap - a, cyp - a / 2, a, a, "<", KEY);
  helpBox(lcd, cxp + a / 2 + gap, cyp - a / 2, a, a, ">", KEY);
  helpBox(lcd, cxp - a / 2, cyp - a / 2, a, a, "OK", MAP_C_PIN);
  lcd.setTextDatum(TC_DATUM);
  lcd.setTextColor(WHITE, BLACK);
  lcd.drawString("arrows scroll", cxp, cyp + a / 2 + gap + a + 4);
  lcd.setTextColor(MAP_C_PIN, BLACK);
  lcd.drawString("OK: drop / open a pin", cxp, cyp + a / 2 + gap + a + 4 + th);
  lcd.setTextDatum(TL_DATUM);

  // ── the keypad shortcuts ──
  int y = sy + th + 8;
  lcd.setTextColor(WHITE, BLACK);
  lcd.drawString("7 / 9    previous / next pin", 6, y);
  y += th + 1;
  lcd.drawString("* / #    zoom, like the side buttons", 6, y);
  y += th + 1;
  lcd.drawString("Back     leave; your view is kept", 6, y);
  y += th + 4;
  lcd.setTextColor(MAP_C_PIN, BLACK);
  lcd.drawString("More: colours and the rest v", 6, y);

  // ── the soft keys, under the screen ──
  const int kw = 92, kh = 20;
  const int ky = bottom - kh - 3;
  lcd.setTextColor(MAP_C_NODE_OLD, BLACK);
  lcd.setTextDatum(TC_DATUM);
  lcd.drawString("soft keys under the screen", lcd.width() / 2, ky - th - 1);
  lcd.setTextDatum(TL_DATUM);
  helpBox(lcd, 6, ky, kw, kh, "Menu", KEY);
  helpBox(lcd, lcd.width() - kw - 6, ky, kw, kh, "Back", KEY);
}

void MapsApp::drawHelp() {
  const int top = (int)header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  if (helpTop < 0) {
    drawHelpDiagram();
    return;
  }
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (!fnt) {
    return;
  }
  lcd.setTextFont(fnt);
  const int pitch = (int)fnt->height() + 1;
  int perScreen = (bottom - top - 4) / pitch;
  if (perScreen < 1) {
    perScreen = 1;
  }
  // Keep the window inside the list however it was scrolled.
  int maxTop = MAPS_HELP_N - perScreen;
  if (maxTop < 0) {
    maxTop = 0;
  }
  if (helpTop > maxTop) {
    helpTop = maxTop;
  }
  if (helpTop < 0) {
    helpTop = 0;
  }
  int y = top + 2;
  for (int i = helpTop; i < MAPS_HELP_N && i < helpTop + perScreen; i++) {
    if (MAPS_HELP_ROWS[i].text[0]) {
      lcd.setTextColor(MAPS_HELP_ROWS[i].colour, BLACK);
      lcd.drawString(MAPS_HELP_ROWS[i].text, 6, y);
    }
    y += pitch;
  }
  // "there is more below" is the only thing that makes the scroll discoverable.
  if (maxTop > 0) {
    char more[32];
    snprintf(more, sizeof(more), "%s%s", "^ ",           // the drawn key is always above row 0
             helpTop < maxTop ? "more below v" : "");
    lcd.setTextColor(MAP_C_PIN, BLACK);
    lcd.drawString(more, 6, bottom - (int)fnt->height() - 1);
  }
}

// ---------------------------------------------------------------- events

/* The keypad mask of a scrolling key, 0 for anything else — what the hold polls for. */
static uint32_t arrowMask(EventType event) {
  switch (event) {
  case WIPHONE_KEY_UP:    return WIPHONE_KEY_MASK_UP;
  case WIPHONE_KEY_DOWN:  return WIPHONE_KEY_MASK_DOWN;
  case WIPHONE_KEY_LEFT:  return WIPHONE_KEY_MASK_LEFT;
  case WIPHONE_KEY_RIGHT: return WIPHONE_KEY_MASK_RIFHT;
  case '2':               return WIPHONE_KEY_MASK_2;
  case '8':               return WIPHONE_KEY_MASK_8;
  case '4':               return WIPHONE_KEY_MASK_4;
  case '6':               return WIPHONE_KEY_MASK_6;
  default:                return 0;
  }
}

appEventResult MapsApp::onMapKey(EventType event) {
  /* 🛑 A BATTERY BLINK IS NOT A KEYPRESS. Every non-keyboard event the phone raises — the
   * battery icon, the WiFi RSSI, the clock, a SIP registration — is delivered to the running
   * app, and this function ran for all of them. So the one line explaining what just happened
   * ("Saved, but NOT sent", "That place is gone") was wiped within a second or two of being
   * written, by something the user did not do. IS_KEYBOARD is GUI.h's own test. */
  if (!IS_KEYBOARD(event)) {
    return DO_NOTHING;
  }

  /* One place clears the note, and it is here: ANY key on the map retires the last one, and
   * a handler that has something new to say says it below. Clearing per-branch is how a
   * message from ten minutes ago ends up sitting under a map nobody is looking at any more —
   * and how one branch gets forgotten. */
  /* The arrow being held, pressed again within a blip of its release: the chip's
   * release-and-re-press under a finger that never moved (MAP_PAN_HOLD_BLIP_MS). The hold
   * goes on exactly as it was — no nudge, no restart of the delay, the run kept. */
  if (panHoldMask && arrowMask(event) == panHoldMask &&
      uiKeyUpAgeMs(panHoldMask) < MAP_PAN_HOLD_BLIP_MS) {
    log_e("MAPS: hold continues past a re-press %lums after its release (%d repeats in)",
          (unsigned long)uiKeyUpAgeMs(panHoldMask), panRun);
    panHoldBlipMs = 0;
    return DO_NOTHING;
  }
  note[0] = '\0';
  panHoldMask = 0;                       // whatever this key is, the previous hold is over
  panHoldBlipMs = 0;

  int dx = 0, dy = 0;
  switch (event) {
  case WIPHONE_KEY_UP:
  case '2':
    dy = -1;
    break;
  case WIPHONE_KEY_DOWN:
  case '8':
    dy = 1;
    break;
  case WIPHONE_KEY_LEFT:
  case '4':
    dx = -1;
    break;
  case WIPHONE_KEY_RIGHT:
  case '6':
    dx = 1;
    break;
  default:
    break;
  }

  if (dx || dy) {
    if (followMe) {
      followMe = false;                  // a scroll is a decision to look elsewhere
      setNote("Stopped following");
    }
    /* A tap is ALWAYS the nudge, however fast the taps come: only a hold climbs the
     * accelerator (below). Taps used to speed up too, which made the size of a tap depend
     * on its timing — the opposite of "as near as I possibly can". */
    const uint32_t now = millis();
    panRun = 0;
    panCarry = 0;
    panHoldPx = 0;
    panLastMs = now;
    panHoldRan = false;
    panOnce(dx, dy, mapPanStep(0), true);
    /* Remember which key this was, so the timer can keep scrolling while it stays down.
     * {user}, 2026-09-19: "holding the scroll button doesn't allow it to keep scrolling and
     * speeding up, it just does a small jump and stops" — the keypad path hides the hold
     * from every app on purpose, so the map polls for it itself (uiKeyStillHeld). */
    panHoldMask = arrowMask(event);
    panHoldSinceMs = now;
    panHoldDx = dx;
    panHoldDy = dy;
    return REDRAW_SCREEN;
  }

  switch (event) {
  case WIPHONE_KEY_F2:              // the second side button: zoom out
  case '*':
  case '1': {
    /* ⚠ FALL BACK TO THE PROJECTION'S LIMITS, NOT TO THE CURRENT ZOOM. With no tiles on the
     * card zMin and zMax were both `zoom`, so both zoom keys were dead and said "z15 is the
     * widest this map has" — about a map that does not exist. The markers are still worth
     * zooming around without a basemap. */
    const int zMin = areaSel >= 0 ? areas[areaSel].zMin : MAP_ZOOM_MIN;
    const int zMax = areaSel >= 0 ? areas[areaSel].zMax : MAP_ZOOM_MAX;
    const int nz = mapZoomView(zMin, zMax, zoom, -1, vpW, vpH, &cx, &cy);
    if (nz == zoom) {
      setNote(areaSel >= 0 ? "z%d is the widest this map has" : "z%d is as far out as it goes",
              zoom);
    } else {
      cancelLoad();              // the tile on its way is for a zoom nobody is looking at
      zoom = nz;
    }
    return REDRAW_SCREEN;
  }
  case WIPHONE_KEY_F1:              // the top side button: zoom in
  case '#':
  case '3': {
    const int zMin = areaSel >= 0 ? areas[areaSel].zMin : MAP_ZOOM_MIN;
    /* One level PAST the deepest tiles is allowed, stretched 2x (map_tiles.h). The tiles
     * stop at zMax; the view does not have to. */
    const int zMax = areaSel >= 0 ? areas[areaSel].zMax + MAP_OVERZOOM_MAX : MAP_ZOOM_MAX;
    const int was = zoom;
    const int nz = mapZoomView(zMin, zMax, zoom, +1, vpW, vpH, &cx, &cy);
    if (nz == zoom) {
      setNote(areaSel >= 0 ? "z%d is as close as this map goes, tiles and all" :
              "z%d is as far in as it goes", zoom);
    } else {
      cancelLoad();
      zoom = nz;
      /* Say it ONCE, on the step that crosses the line — not on every frame. Past here the
       * strip's "z17*" and the status line carry it, which is quieter than a note that
       * cannot be dismissed. */
      if (areaSel >= 0 && was <= areas[areaSel].zMax && nz > areas[areaSel].zMax) {
        setNote("Past the tiles: z%d stretched %dx. Same detail, bigger - and fuzzy.",
                areas[areaSel].zMax, 1 << (nz - areas[areaSel].zMax));
      }
    }
    return REDRAW_SCREEN;
  }
  case '5': {
    const int hit = pinUnderCrosshair();   // the same key as OK, for gloves: open, else drop
    if (hit >= 0) {
      pinSel = hit;
      enterState(MAPS_PIN_OPTS);
      return REDRAW_ALL;
    }
    if (dropPin()) {
      setNote("Dropped '%s' - name it", pins[pinSel].name);
      enterState(MAPS_RENAME);
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;
  }
  case '7':
    stepPin(-1);
    return REDRAW_SCREEN;
  case '9':
    stepPin(1);
    return REDRAW_SCREEN;
  case WIPHONE_KEY_F3:              // the third side button: centre on me
  case '0':
    centreOnMe();
    return REDRAW_SCREEN;
  case WIPHONE_KEY_F4:              // the fourth side button: the next map type
    /* {user}, 2026-09-20: "make the last user button cycle through map types". The same
     * switch as Menu > Map area, one press at a time, round and round; setArea keeps the
     * ground under the crosshair and says which map it is on. */
    if (areaCount <= 0) {
      setNote("No maps on the card - see docs/maps.md");
    } else if (areaCount == 1) {
      setNote("Only one map on the card: %s", areas[0].name);
    } else {
      setArea((areaSel + 1) % areaCount);
    }
    return REDRAW_SCREEN;
  default:
    break;
  }

  /* ⚠ SELECT BEFORE LOGIC_BUTTON_OK, ALWAYS. LOGIC_BUTTON_OK is OK||CALL||SELECT on this
   * keyboard (Hardware.h:212), so a test for the macro first would swallow the soft key —
   * app_music.cpp:405-412 has exactly that dead branch. Here the top-left soft key is the
   * MENU, the D-pad centre is the PIN (options on the one under the crosshair, otherwise drop
   * a new one), and CALL does nothing on a map. */
  if (event == WIPHONE_KEY_SELECT) {
    enterState(MAPS_MENU);
    return REDRAW_ALL;
  }
  if (event == WIPHONE_KEY_OK) {
    const int hit = pinUnderCrosshair();
    if (hit >= 0) {
      pinSel = hit;
      enterState(MAPS_PIN_OPTS);
      return REDRAW_ALL;
    }
    if (dropPin()) {
      setNote("Dropped '%s' - name it", pins[pinSel].name);
      enterState(MAPS_RENAME);
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;
  }
  return DO_NOTHING;
}

appEventResult MapsApp::processEvent(EventType event) {
  /* A position or a waypoint arrived (WiPhone.ino forwards it as this). Redraw the map so a
   * marker that has moved actually moves — but only the map: this must never steal a
   * keypress or rebuild a menu under the user's thumb. */
  if (event == NEW_MESSAGE_EVENT) {
    return (appState == MAPS_VIEW) ? REDRAW_SCREEN : DO_NOTHING;
  }

  if (event == APP_TIMER_EVENT) {
    if (panHoldMask) {
      const bool held = (appState == MAPS_VIEW) && uiKeyStillHeld(panHoldMask);
      if (held && panHoldBlipMs) {
        /* Back down within the blip: the chip's release under a finger that never moved. */
        log_e("MAPS: hold continues past a %lums release blip (%d repeats in)",
              (unsigned long)(millis() - panHoldBlipMs), panRun);
        panHoldBlipMs = 0;
      }
      if (!held && appState == MAPS_VIEW && uiKeyUpAgeMs(panHoldMask) < MAP_PAN_HOLD_BLIP_MS) {
        /* Just released — too soon to say the finger came off (MAP_PAN_HOLD_BLIP_MS). Keep
         * the hold and look again next tick; the timer stays armed while panHoldMask is set.
         *
         * 🛑 AND DO NOTHING ELSE THIS PASS. The blip is judged on drain-time stamps: the
         * chip's re-press comes 4 ms after its release, but it is READ on the next pass, and
         * if this pass went on to read a 32 KB tile piece (25-60 ms) the re-press would
         * arrive 25+ ms late — past the bounce filter, so dispatched as a keypress, and past
         * the blip window, so taken for a fresh tap: nudge, snap, a 400 ms restart. Keeping
         * this pass short keeps the re-press inside KEY_BOUNCE_MS, where the keypad path
         * absorbs it silently (review, 2026-09-20). The tile waits one tick. */
        if (!panHoldBlipMs) {
          panHoldBlipMs = millis();
        }
        return DO_NOTHING;
      } else if (!held) {
        panHoldMask = 0;                 // the finger came off (or a lost release aged out)
        panHoldBlipMs = 0;
        if (panHoldRan) {
          log_e("MAPS: hold ended after %d repeats, %lums, %d px", panRun,
                (unsigned long)(millis() - panHoldSinceMs), panHoldPx);
          panRun = 0;
          panLastMs = 0;
          panHoldRan = false;
          /* Where the sweep STOPPED is a landing too: letting go near a marker snaps onto
           * it, the same rule as a tap. Never mid-sweep — the repeats sail past. */
          if (appState == MAPS_VIEW && snapLanding()) {
            armTimer();
            return REDRAW_SCREEN;
          }
        }
        armTimer();                      // stand the 100 ms tick down unless tiles need it
      } else {
        const uint32_t now = millis();
        const uint32_t held = now - panHoldSinceMs;
        if (held >= MAP_PAN_HOLD_DELAY_MS) {
          /* The repeats began at the end of the delay; the speed is set by how long they
           * have been running, and the distance by how long THIS tick took to come round —
           * a late frame moves further, so the sweep is steady when the frames are not.
           * EVERY tick moves (no "at least 100 ms since the last" gate): a frame costs
           * ~80 ms, and a 100 ms gate on a 50 ms timer let every other tick through — one
           * frame per 180 ms, measured, from a phone that could draw one per 90. */
          const int run = 1 + (int)((held - MAP_PAN_HOLD_DELAY_MS) / MAP_PAN_HOLD_STEP_MS);
          const uint32_t dt = panHoldRan ? (now - panLastMs) : MAP_PAN_HOLD_STEP_MS;
          const int px = mapPanHoldMove(run, dt, &panCarry);
          if (panRun < 1000) {
            panRun++;
          }
          panLastMs = now;
          panHoldRan = true;
          if (px > 0) {
            panHoldPx += px;
            panOnce(panHoldDx, panHoldDy, px, false);
            return REDRAW_SCREEN;
          }
        }
      }
    }
    /* ALTITUDE (0.9.80): one owed height a tick, once the view has settled (elevStep). Only
     * with no hold running - a held arrow's ticks do nothing but the hold, the blip-wait tick
     * above all (it has returned already) - and never ahead of a tile piece (elevStep waits
     * for those). */
    if (!panHoldMask && elevStep()) {
      armTimer();                              // stands the tick down once nothing more is owed
      return (appState == MAPS_VIEW || appState == MAPS_PIN_OPTS) ? REDRAW_SCREEN : DO_NOTHING;
    }
    /* A pin's frame (review M1): the radio's answer, when it comes — "Shared", "Taken off",
     * or the roll-back / restore / kept id when it never left. One ring lookup a tick. */
    if (meshOp.txId) {
      const bool moved = settleMeshOp(false);
      if (moved) {
        armTimer();                            // the 250 ms watch stands down with it
      }
      if (appState == MAPS_VIEW) {
        if (moved) {
          return REDRAW_SCREEN;
        }
      } else if (appState != MAPS_DOWNLOAD) {
        /* A menu or a form: this watch is the ONLY reason it ticks (armTimer), so nothing
         * below — tile loads, the follow — may run under it. The note shows on the map. */
        return DO_NOTHING;
      }
    }
    /* A download filling the area on the screen (jobTilesLanded): tiles it has written since
     * the last look are read now, instead of their parents staying stretched over them. */
    if (appState == MAPS_VIEW && (uint32_t)(millis() - jobPollMs) >= MAPS_JOB_POLL_MS) {
      jobPollMs = millis();
      if (jobTilesLanded()) {
        return REDRAW_SCREEN;
      }
      armTimer();                // a download that has ended stands its slow tick down
    }
    if (followMe && appState == MAPS_VIEW && !loadActive) {
      /* A newer usable fix than the one last centred on: follow it. A stale or poor fix, or
       * none, leaves the view exactly where it is. */
      int32_t la = 0, lo = 0;
      uint32_t age = 0;
      if (selfPosition(&la, &lo, &age) == MAPS_SELF_GPS) {
        /* The moment this fix arrived, from its age: the same fix reads the same stamp on
         * every tick (give or take a tick), a new one reads a later stamp. */
        const uint32_t stamp = millis() - age;
        if (stamp - followLastStamp > 500u) {
          followLastStamp = stamp;
          centreOn(mapI7ToDeg(la), mapI7ToDeg(lo));
          /* ALTITUDE: the new crosshair's ground, and yours when that is owed too, in THIS tick
           * (elevStep(true)) so the one redraw below shows them. At most ONE card read a fix
           * (~5-20 ms, estimated) ahead of the ~77 ms frame, and none while the fix stays within
           * 5 m of the last sample (GPS noise): "you" is the crosshair's own sample here (review
           * 2026-09-27: each fix read the crosshair AND you, up to four tile opens, every second).
           * Both steps run - the first reads nothing when the view has not really moved. The
           * card is reached from this frame at the depth the settle-wait path above reaches it
           * (elevStep from processEvent in both). */
          elevStep(true);
          elevStep(true);
          return REDRAW_SCREEN;
        }
      }
      if (!wantAny) {
        return DO_NOTHING;
      }
    }
    if (appState == MAPS_DOWNLOAD) {
      /* Once a second while there is something live to show: a running job's progress, a
       * waiting one's countdown (it may resume, or be dropped, while the form is open), an
       * armed Stop running out, the screen hold letting go. A form with none of those does
       * not rebuild at all. */
      const bool running = tileFetchActive();
      const bool live = running || dlShownJob || dlStopArmMs;
      if (running != dlShownRunning || (live && (uint32_t)(millis() - dlLastMs) >= 1000u)) {
        dlLastMs = millis();
        buildDownload();
        return REDRAW_SCREEN;
      }
      return DO_NOTHING;
    }
    if (!loadActive && wantAny && !startLoad(wantZ, wantTx, wantTy)) {
      /* ⚠ A REFUSED START MUST NOT BE RETRIED AT 40 Hz. startLoad only refuses for reasons
       * that will still be true in 25 ms — no area, no PSRAM, no free slot — so asking again
       * immediately is a spin that keeps the CPU awake and achieves nothing. Forget the want
       * and stand the fast tick down (armTimer keeps any slow one — follow, a download's
       * poll); the next repaint re-discovers the hole if it still matters. */
      wantAny = false;
      armTimer();
      return DO_NOTHING;
    }
    if (loadActive) {
      /* A finished tile redraws, and so does a MISS: the next level down is only chosen by
       * drawMap, so without the repaint a stretched position would wait for a keypress. */
      if (tileLoadStep() != LOAD_PIECE) {
        /* Stand the fast tick down now: while the phone is locked the redraw below goes to the
         * clock, so drawMap (which re-arms it) does not run, and a 25 ms tick would go on doing
         * nothing until unlock. Unlocked, drawMap re-arms it at once. */
        armTimer();
        return (appState == MAPS_VIEW) ? REDRAW_SCREEN : DO_NOTHING;
      }
    }
    /* Deliberately no redraw while a piece is merely in flight: repainting 240x250 to change
     * nothing is the sort of busy-work that shows up as a warm phone and a flat battery. */
    return DO_NOTHING;
  }

  switch (appState) {

  case MAPS_VIEW:
    if (LOGIC_BUTTON_BACK(event)) {
      return EXIT_APP;           // the destructor saves the view
    }
    return onMapKey(event);

  case MAPS_MENU:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      switch (sel) {
      case ROW_M_PINS:
        listKind = LIST_PINS;
        enterState(MAPS_LIST);
        return REDRAW_ALL;
      case ROW_M_PLACES:
        listKind = LIST_PLACES;
        enterState(MAPS_LIST);
        return REDRAW_ALL;
      case ROW_M_NODES:
        listKind = LIST_NODES;
        enterState(MAPS_LIST);
        return REDRAW_ALL;
      case ROW_M_ME:
        centreOnMe();
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      case ROW_M_AREA:
        enterState(MAPS_AREAS);
        return REDRAW_ALL;
      case ROW_M_FOLLOW:
        followMe = !followMe;
        followLastStamp = 0;
        if (followMe) {
          centreOnMe();                  // one honest centre now; the timer keeps it there
          if (!meshService.anySourceEnabled()) {
            setNote("Location sources off - following waits");
          } else {
            setNote("Following you - any scroll stops it");
          }
        } else {
          setNote("Stopped following");
        }
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      case ROW_M_SNAP: {
        snapPins = !snapPins;
        Preferences p;
        if (p.begin(MAPS_NVS, false)) {
          p.putInt("snap", snapPins ? 1 : 0);
          p.end();
        }
        setNote(snapPins ? "Landing near a marker snaps onto it"
                         : "The arrows never snap");
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      case ROW_M_ALT: {
        altOn = !altOn;
        Preferences p;
        if (p.begin(MAPS_NVS, false)) {
          p.putInt("alt", altOn ? 1 : 0);
          p.end();
        }
        /* The card is NOT looked at here (a key handler): elevPresent is what the last scan
         * found, and Rescan looks again. */
        if (!altOn) {
          setNote("Altitude off - the elevation tiles are not read");
        } else if (!elevPresent) {
          setNote("Altitude on - but there is no /maps/elev on the card (docs/maps.md)");
        } else {
          setNote("Altitude on - the ground under the crosshair, and against you");
        }
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      case ROW_M_UNITS:
        /* One device-wide setting (prefs_almanac.h): the Meshtastic lists, the console and the
         * Almanac follow it too. NVS, not the card. */
        unitsSetPref(gUnits == UNITS_US ? UNITS_METRIC : UNITS_US);
        setNote(gUnits == UNITS_US ? "Units: US - feet and miles, on every screen"
                                   : "Units: metric - metres and km, on every screen");
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      case ROW_M_MEASURE:
        measuring = !measuring;
        if (measuring) {
          double lat = 0, lon = 0;
          mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
          measLatI = mapDegToI7(lat);
          measLonI = mapDegToI7(lon);
          setNote("Scroll away - the bar reads the distance");
        } else {
          setNote("Stopped measuring");
        }
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      case ROW_M_DOWNLOAD:
        enterState(MAPS_DOWNLOAD);
        return REDRAW_ALL;
      case ROW_M_GOTO:
        enterState(MAPS_GOTO);
        return REDRAW_ALL;
      case ROW_M_RESCAN:
        rescanCard();
        setNote("%d map%s, %d pin%s on the card", areaCount, areaCount == 1 ? "" : "s",
                pinCount, pinCount == 1 ? "" : "s");
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      case ROW_M_HELP:
        enterState(MAPS_HELP);
        return REDRAW_ALL;
      case ROW_M_BACK:
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      default:
        break;
      }
    }
    return REDRAW_SCREEN;

  case MAPS_DOWNLOAD:
    if (IS_KEYBOARD(event)) {
      dlKeyMs = millis();                // the screen hold's clock (buildDownload)
    }
    if (LOGIC_BUTTON_BACK(event)) {
      /* Coming back to the map after a download: the tiles it wrote are not in the miss
       * ring's memory, and a new source folder is a new area. Re-read the card, keep the view. */
      rescanCard();
      enterState(MAPS_MENU);
      return REDRAW_ALL;
    }
    if (dlStopArmMs && IS_KEYBOARD(event) && !LOGIC_BUTTON_OK(event)) {
      dlStopArmMs = 0;                   // any other key disarms Stop: it reads "Stop" again
      buildDownload();
    }
    if (menu) {
      menu->processEvent(event);
      /* ⚠ NOTE ROWS ARE STOPS TO MenuWidget — an arrow lands on the estimate line and OK there
       * does nothing. Step over them, in the direction of travel, so the form has four rows
       * to the thumb and not seven. */
      if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN) {
        for (int guard = 0; guard < 8 && menu->currentKey() == MENU_ROW_NOTE; guard++) {
          menu->processEvent(event);
        }
      }
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      dlKeep = sel;
      if (sel != ROW_D_STOP) {
        dlStopArmMs = 0;                 // OK on another row is not the second press
      }
      switch (sel) {
      case ROW_D_SOURCE:
        dlSource = (dlSource + 1) % tileSourceCount();
        buildDownload();
        menu->select(sel);
        return REDRAW_SCREEN;
      case ROW_D_RADIUS:
        dlRadiusIdx = (dlRadiusIdx + 1) % MAPS_DL_RADII_N;
        buildDownload();
        menu->select(sel);
        return REDRAW_SCREEN;
      case ROW_D_ELEV:
        dlElev = !dlElev;                // kept in NVS at Start, with the other choices
        buildDownload();
        menu->select(sel);
        return REDRAW_SCREEN;
      case ROW_D_DEPTH: {
        /* Through the depths THIS source has, and nothing else: raising the form's top to z17
         * without this gave USGS a fifth press that still read "z16" — a key that looks dead.
         * The wish survives a Source change (17 on USGS shows 16; back on OTM it is 17). */
        const TileSource* s = tileSource(dlSource);
        dlDepth = tilePlanDepthNext(dlDepth, s ? s->zMax : MAPS_DL_DEPTH_MIN);
        buildDownload();
        menu->select(sel);
        return REDRAW_SCREEN;
      }
      case ROW_D_START: {
        const TileSource* s = tileSource(dlSource);
        TileJobSpec spec;
        memset(&spec, 0, sizeof(spec));
        spec.source = dlSource;
        mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &spec.lat, &spec.lon);
        spec.radiusKm = MAPS_DL_RADII[dlRadiusIdx];
        spec.zMax = tilePlanDepthShown(dlDepth, s ? s->zMax : MAPS_DL_DEPTH_MIN);   // the depth SHOWN
        spec.elev = dlElev;
        char why[96];
        dlKeep = ROW_D_STOP;             // BEFORE the rebuild: Start is gone from a running form
        dlWhy[0] = '\0';
        if (tileFetchStart(&spec, why, sizeof(why))) {
          Preferences p;
          if (p.begin(MAPS_NVS, false)) {
            p.putInt("dlsrc", dlSource);
            p.putInt("dlrad", dlRadiusIdx);
            p.putInt("dldep", dlDepth);
            p.putInt("dlelev", dlElev ? 1 : 0);
            p.end();
          }
          setNote("Downloading %s", tileSource(dlSource)->label);
        } else {
          /* `note` is drawn on the MAP, not here: a refusal has to live on this form. */
          snprintf(dlWhy, sizeof(dlWhy), "Not started: %s", why);
          setNote("Not started: %s", why);
        }
        buildDownload();
        return REDRAW_SCREEN;
      }
      case ROW_D_STOP:
        /* 🛑 TWO PRESSES. A Stop forgets a job that may be two days into a 20 km run, and it will
         * not resume: the first press only says so. */
        if (!dlStopArmMs || (uint32_t)(millis() - dlStopArmMs) >= MAPS_DL_STOP_ARM_MS) {
          dlStopArmMs = millis() | 1u;
          dlKeep = ROW_D_STOP;
          buildDownload();
          return REDRAW_SCREEN;
        }
        dlStopArmMs = 0;
        tileFetchStop();
        dlWhy[0] = '\0';
        dlKeep = ROW_D_START;
        buildDownload();
        return REDRAW_SCREEN;
      case ROW_D_RESUME: {
        char why[96];
        dlWhy[0] = '\0';
        if (tileFetchResumeNow(why, sizeof(why))) {
          setNote("Resumed the download");
          dlKeep = ROW_D_STOP;
        } else {
          snprintf(dlWhy, sizeof(dlWhy), "Not resumed: %s", why);
        }
        buildDownload();
        return REDRAW_SCREEN;
      }
      case ROW_D_BACK:
        rescanCard();
        enterState(MAPS_MENU);
        return REDRAW_ALL;
      default:
        break;
      }
    }
    return REDRAW_SCREEN;

  case MAPS_GOTO: {
    /* Two fields, typed on the keypad: digits, `*` for the point, `#` for a minus sign.
     * UP/DOWN move between latitude and longitude; Back deletes; END cancels; OK goes. */
    char* f = gotoField == 0 ? gotoLat : gotoLon;
    if (event == WIPHONE_KEY_END) {
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_BACK) {
      const size_t n = strlen(f);
      if (n) {
        f[n - 1] = '\0';
      }
      return REDRAW_SCREEN;
    }
    if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN) {
      gotoField = gotoField ? 0 : 1;
      return REDRAW_SCREEN;
    }
    if (LOGIC_BUTTON_OK(event)) {
      if (!gotoLat[0] || !gotoLon[0]) {
        gotoField = gotoLat[0] ? 1 : 0;
        return REDRAW_SCREEN;              // the empty field is highlighted: type it
      }
      char* end = NULL;
      const double lat = strtod(gotoLat, &end);
      const bool latOk = end && *end == '\0' && lat >= -90.0 && lat <= 90.0;
      const double lon = strtod(gotoLon, &end);
      const bool lonOk = end && *end == '\0' && lon >= -180.0 && lon <= 180.0;
      if (!latOk || !lonOk) {
        gotoField = latOk ? 1 : 0;
        return REDRAW_SCREEN;              // the bad one is highlighted
      }
      centreOn(lat, lon);
      setNote("%.5f, %.5f", lat, lon);
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (IS_KEYBOARD(event)) {
      char c = 0;
      if (event >= '0' && event <= '9') {
        c = (char)event;
      } else if (event == '*') {
        c = '.';
      } else if (event == '#') {
        /* Toggle the sign: a minus is only ever the first character. */
        if (f[0] == '-') {
          memmove(f, f + 1, strlen(f));
        } else if (strlen(f) < 14) {
          memmove(f + 1, f, strlen(f) + 1);
          f[0] = '-';
        }
        return REDRAW_SCREEN;
      }
      if (c) {
        const size_t n = strlen(f);
        if (c == '.' && strchr(f, '.')) {
          return REDRAW_SCREEN;            // one point per number
        }
        if (n < 14) {
          f[n] = c;
          f[n + 1] = '\0';
        }
        return REDRAW_SCREEN;
      }
    }
    return DO_NOTHING;
  }

  case MAPS_CHANNEL:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_PIN_OPTS);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType k = menu->currentKey();
      if (k == ROW_C_CANCEL || chanForPin < 0 || chanForPin >= pinCount) {
        enterState(MAPS_PIN_OPTS);
        return REDRAW_ALL;
      }
      /* Re-resolve by index NOW, never trusting the key frozen when the rows were drawn —
       * applyChannelUrl() can have rewritten the table since (app_meshtastic.cpp does this). */
      const MeshChannel* c = meshService.getChannel((int)k - ROW_C_BASE);
      if (!c) {
        chanPending = 0;
        buildChannels();
        return REDRAW_SCREEN;
      }
      /* The PUBLIC row takes two presses: the same idiom as the beacon's channel picker. A
       * shared pin persists on every radio that hears it, so "any radio in range" is the
       * warning to read twice. */
      if (meshService.channelIsPublic(c) && chanPending != (int)k) {
        chanPending = (int)k;
        buildChannels();
        menu->select(k);
        return REDRAW_SCREEN;
      }
      chanPending = 0;
      strlcpy(pins[chanForPin].chan, c->name, sizeof(pins[chanForPin].chan));
      pinSel = chanForPin;
      char why[72];
      sharePin(pinSel, why, sizeof(why));
      setNote("%s", why);
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;


  case MAPS_LIST:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_MENU);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      /* ⚠ BEFORE the range test. MENU_ROW_NOTE minus ROW_FIRST underflows and casts to a
       * NEGATIVE index, which sails straight through an `idx < count` bounds check — the
       * trap Files, Books and Music each carry a guard for. */
      if (sel == MENU_ROW_NOTE || sel < ROW_FIRST || sel >= ROW_ACT) {
        return REDRAW_SCREEN;
      }
      const int idx = (int)(sel - ROW_FIRST);
      if (idx < 0 || idx >= listCount) {
        return REDRAW_SCREEN;
      }
      if (listKind == LIST_PINS) {
        const int pi = (int)listId[idx];
        if (pi >= 0 && pi < pinCount) {
          pinSel = pi;
          centreOn(mapI7ToDeg(pins[pi].latI), mapI7ToDeg(pins[pi].lonI));
          enterState(MAPS_PIN_OPTS);
          return REDRAW_ALL;
        }
      } else if (listKind == LIST_PLACES) {
        /* Looked up again by id, because the waypoint table compacts under an open screen:
         * the expiry sweep moves the last entry into the hole it just made. */
        const MeshWaypoint* w = meshService.findWaypoint(listId[idx]);
        if (w && w->id) {
          centreOn(mapI7ToDeg(w->latI), mapI7ToDeg(w->lonI));
          setNote("%s", w->name);
          enterState(MAPS_VIEW);
          return REDRAW_ALL;
        }
        setNote("That place is gone - it expired or was deleted");
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      } else {
        const MeshNode* nd = meshService.findNode(listId[idx]);
        if (nd && nd->posHeardMs) {
          centreOn(mapI7ToDeg(nd->latI), mapI7ToDeg(nd->lonI));
          setNote("%s", nd->name[0] ? nd->name : "?");
          enterState(MAPS_VIEW);
          return REDRAW_ALL;
        }
        setNote("That node is no longer in the list");
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      return REDRAW_SCREEN;
    }
    return REDRAW_SCREEN;

  case MAPS_PIN_OPTS:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      if (pinSel < 0 || pinSel >= pinCount) {
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      switch (sel) {
      case ROW_P_RENAME:
        enterState(MAPS_RENAME);
        return REDRAW_ALL;
      case ROW_P_MOVE: {
        double lat = 0, lon = 0;
        mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
        pins[pinSel].latI = mapDegToI7(lat);
        pins[pinSel].lonI = mapDegToI7(lon);
        pinsDirty = true;
        savePins();
        if (retractionQueued(pinSel)) {
          /* 🛑 Its retraction is still in the queue: the person asked for it OFF the mesh, so
           * the move is this phone's alone. A re-share here would go out right behind the
           * retraction and put the place back on every radio (mapPinRetractionQueued). If the
           * retraction then fails, the id stays and the pin stays yellow — "Take it off" again. */
          setNote("Moved '%s' - still taking it off the mesh...", pins[pinSel].name);
        } else if (pins[pinSel].sharedId) {
          /* It was shared from its old spot, so the mesh is holding the old one. Moving it
           * here and not there would leave two truths. */
          char why[72];
          sharePin(pinSel, why, sizeof(why));
          setNote("Moved. %s", why);
        } else {
          setNote("Moved '%s' here", pins[pinSel].name);
        }
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      case ROW_P_SHARE_ON:
        chanForPin = pinSel;
        enterState(MAPS_CHANNEL);
        return REDRAW_ALL;
      case ROW_P_SHARE: {
        /* A pin that has never been shared asks which channel first. A re-share goes back out
         * where it already is — for a pin shared before channels were recorded (chan "") that
         * is the automatic pick it went out on, and sharePin() refuses one whose channel has
         * left the phone. */
        if (!pins[pinSel].sharedId && !pinChannel(pinSel)) {
          chanForPin = pinSel;
          enterState(MAPS_CHANNEL);
          return REDRAW_ALL;
        }
        char why[72];
        sharePin(pinSel, why, sizeof(why));
        setNote("%s", why);
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      case ROW_P_UNSHARE: {
        /* 🛑 KEEP THE ID WHEN THE RETRACTION DID NOT GO OUT. Forgetting it removes the pin
         * from THIS phone's Places and leaves it on everyone else's forever, with nothing on
         * this phone that knows the id any more — the retraction can never be sent again. A
         * stale camp on other people's maps is the exact failure "Take it off the mesh"
         * exists to prevent.
         * 🛑 AND "QUEUED" IS NOT "WENT OUT" (review M1, 2026-09-25). unshareWaypoint() is true
         * once the retraction is in the radio's queue; the id used to be cleared right here, so
         * a frame that then timed out, or was dropped with a dying pack, stranded the place
         * exactly as above. The id is now forgotten only when the radio reports the retraction
         * SENT (settleMeshOp -> MAP_PINACT_FORGET_ID); no word keeps it. */
        if (retractionQueued(pinSel)) {
          /* Pressed again while the first is still queued: that one is left to answer (settling
           * it early would read "no word"), and no second frame is spent on the air. */
          setNote("Still taking '%s' off the mesh...", pins[pinSel].name);
          enterState(MAPS_VIEW);
          return REDRAW_ALL;
        }
        settleMeshOp(true);                  // one pin frame at a time: the last one first
        if (!pins[pinSel].sharedId) {
          enterState(MAPS_VIEW);             // that settle took it off: its note says so
          return REDRAW_ALL;
        }
        if (pins[pinSel].chan[0] && !pinChannel(pinSel)) {
          /* The channel it went out on is no longer on this phone: a retraction sent
           * anywhere else reaches nobody who has it, and would report success. */
          setNote("Channel '%s' is gone from this phone - cannot retract", pins[pinSel].chan);
          enterState(MAPS_VIEW);
          return REDRAW_ALL;
        }
        const bool ok = meshService.unshareWaypoint(pins[pinSel].sharedId, pinChannel(pinSel));
        if (ok) {
          beginMeshOp(MAP_PINOP_UNSHARE, meshService.lastQueuedPacketId(), pinSel);
          setNote("Taking '%s' off the mesh...", pins[pinSel].name);
        } else {
          setNote("Radio not ready - the mesh still has it. Try again.");
        }
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      case ROW_P_CENTRE:
        centreOn(mapI7ToDeg(pins[pinSel].latI), mapI7ToDeg(pins[pinSel].lonI));
        setNote("%s", pins[pinSel].name);
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      case ROW_P_DELETE:
        enterState(MAPS_CONFIRM_DEL);
        return REDRAW_ALL;
      case ROW_P_CANCEL:
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      default:
        break;
      }
    }
    return REDRAW_SCREEN;

  case MAPS_CONFIRM_DEL:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_PIN_OPTS);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      if (sel == ROW_DEL_YES) {
        char gone[MAP_PIN_NAME_LEN];
        gone[0] = '\0';
        if (pinSel >= 0 && pinSel < pinCount) {
          strlcpy(gone, pins[pinSel].name, sizeof(gone));
          if (pins[pinSel].sharedId && pins[pinSel].chan[0] && !pinChannel(pinSel)) {
            /* The confirm promised "...and take it off the mesh", and that cannot be done:
             * the channel it lives on is gone. Deleting locally would throw away the only
             * id that can ever retract it. Keep the pin and say why. */
            setNote("Channel '%s' is gone - cannot retract, kept", pins[pinSel].chan);
            enterState(MAPS_VIEW);
            return REDRAW_ALL;
          }
        }
        const bool retracted = deletePin(pinSel);
        if (retracted && meshOp.txId && meshOp.op == MAP_PINOP_DELETE) {
          setNote("Deleted '%s' - taking it off the mesh...", gone);   // queued, not gone yet
        } else if (retracted) {
          setNote("Deleted '%s'", gone);
        } else {
          setNote("Deleted here - but the radio did not send the retraction");
        }
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
      if (sel == ROW_DEL_NO) {
        enterState(MAPS_PIN_OPTS);
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case MAPS_AREAS:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_MENU);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType sel = menu->readChosen();
      if (sel != MENU_ROW_NOTE && sel >= ROW_FIRST && sel < ROW_ACT) {
        setArea((int)(sel - ROW_FIRST));
        enterState(MAPS_VIEW);
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case MAPS_RENAME:
    /* 🛑 CANCEL IS END, NOT BACK — Back is BACKSPACE in a text field. Testing
     * LOGIC_BUTTON_BACK here would consume the key before the widget saw it, and the field
     * would have no way to delete a character: exactly the bug Photos shipped with
     * (app_photos.cpp), and the convention every other text screen here already follows. */
    if (event == WIPHONE_KEY_END) {
      controlState.setInputState(InputType::Numeric);
      /* The pin keeps the name it was given automatically - cancelling the FIELD is not
       * cancelling the PIN, and a person who pressed 5 meant to mark this spot. Say which
       * name it kept, rather than leaving "name it" sitting under a map nobody is naming. */
      if (pinSel >= 0 && pinSel < pinCount) {
        setNote("Kept the name '%s'", pins[pinSel].name);
      } else {
        note[0] = '\0';
      }
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event)) {
      if (pinSel >= 0 && pinSel < pinCount) {
        /* ⚠ AN EMPTY FIELD KEEPS THE NAME IT HAD. mapPinSanitizeName() turns "" into "Pin"
         * because a nameless marker is unusable — but applied here that would quietly rename
         * "Pin 3" to "Pin", and a second empty save would give you two pins called "Pin".
         * Saving nothing means changing nothing. */
        char typed[MAP_PIN_NAME_LEN];
        mapPinSanitizeName(textArea ? textArea->getText() : NULL, typed, sizeof(typed));
        const char* raw = textArea ? textArea->getText() : NULL;
        bool blank = true;
        for (const char* q = raw; q && *q; q++) {
          if (*q != ' ' && *q != '\t') {
            blank = false;
            break;
          }
        }
        if (!blank) {
          strlcpy(pins[pinSel].name, typed, sizeof(pins[pinSel].name));
        }
        pinsDirty = true;
        savePins();
        if (retractionQueued(pinSel)) {
          // Being taken off the mesh: renamed here only (Move here's rule, mapPinRetractionQueued).
          setNote("Named '%s' - still taking it off the mesh...", pins[pinSel].name);
        } else if (pins[pinSel].sharedId) {
          char why[72];
          sharePin(pinSel, why, sizeof(why));   // the mesh holds the old name otherwise
          setNote("Renamed. %s", why);
        } else {
          setNote("Named '%s'", pins[pinSel].name);
        }
      }
      controlState.setInputState(InputType::Numeric);
      enterState(MAPS_VIEW);
      return REDRAW_ALL;
    }
    if (textArea) {
      textArea->processEvent(event);
    }
    return REDRAW_SCREEN;

  case MAPS_HELP:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MAPS_MENU);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_UP || event == '2') {
      if (helpTop > -1) {
        helpTop--;                       // from row 0, up goes back to the drawn key
      }
      return REDRAW_SCREEN;
    }
    if (event == WIPHONE_KEY_DOWN || event == '8' || LOGIC_BUTTON_OK(event)) {
      /* OK pages down rather than leaving: on a screen that scrolls, the key people press to
       * see more must show more. Back is how you leave, and the footer says so. */
      helpTop++;
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;
  }
  return DO_NOTHING;
}

/* GUI::redrawScreen asks this after every repaint: the map VIEW draws nothing outside
 * vpX/vpY/vpW/vpH — the band between the header and the footer, set in the constructor — so
 * only that band needs to reach the glass. The other screens (menus, help, the dialogs) are
 * widgets drawn the ordinary way and take the ordinary full push. */
bool MapsApp::drewInsideBand() {
  return appState == MAPS_VIEW && vpY > 0;
}

void MapsApp::redrawScreen(bool redrawAll) {
  /* 🛑 THE TEXT DATUM IS SHARED STATE AND IT IS NOT LEFT AT TL_DATUM.
   * `lcd` here is the same TFT_eSPI object the header, the footer and every MenuWidget draw
   * into — GUI makes one page sprite and hands it to everyone — and `textdatum` is a plain
   * member of it, not per-caller. GUI draws the app FIRST and the header/footer AFTER, so on
   * the second and every later repaint this app inherits whatever the footer left behind:
   * MR_DATUM for the right-hand soft key, or ML/CL from the header and the menus. With a
   * MIDDLE datum drawString subtracts half the font height from y, so every string on the map
   * — the chips, the scale bar, the coordinates, the whole help screen — is drawn 8 px HIGHER
   * than it was placed, and the top chip lands in the header band. With a RIGHT datum it also
   * runs backwards from x. One line, set once per repaint, before anything draws. */
  lcd.setTextDatum(TL_DATUM);

  switch (appState) {
  case MAPS_VIEW:
    if (areaSel < 0 && pinCount == 0 && meshService.getWaypointCount() == 0) {
      /* Nothing at all to draw: say what to do about it rather than showing a black
       * rectangle with a crosshair on it. With any pin or place to show, the map is drawn
       * (blank behind the markers) because the markers are the point. */
      drawNoMapPage();
    } else {
      drawMap();
    }
    break;
  case MAPS_HELP:
    drawHelp();
    break;
  case MAPS_GOTO:
    drawGoto();
    break;
  case MAPS_RENAME:
    if (textArea) {
      ((GUIWidget*)textArea)->redraw(lcd);
    }
    break;
  default:
    if (menu) {
      ((GUIWidget*)menu)->redraw(lcd);
    }
    break;
  }

  /* ⚠ THE TIMER IS ARMED HERE, AT THE END OF THE DRAW, because this is the only place that
   * knows whether a tile is still missing — drawMap() is what discovers it. 25 ms while there
   * is reading to do, nothing at all otherwise: a map that is fully painted must not keep the
   * CPU awake, and the phone that runs this is one somebody is carrying all day. */
  armTimer();
}

void MapsApp::armTimer() {
  /* A download that has written elevation tiles since the last look (no card I/O: a counter).
   * Here because every draw and every tick that changes what is owed ends in armTimer. */
  elevJobCheck();
  const bool busy = (appState == MAPS_VIEW) && (loadActive || (wantAny && !tileMemFail));
  /* The download screen ticks at 4 Hz for its progress line only while a job is running:
   * a finished screen must not keep the CPU awake either. */
  /* ...and a slow one on a finished form, so a job that ended while a call had every event
   * (a callApp takes them all) is noticed and the Stop row becomes Start again. */
  const bool progress = (appState == MAPS_DOWNLOAD);
  /* Following polls the fix once a second — a slow tick, and only on the map. */
  const bool following = followMe && (appState == MAPS_VIEW) && meshService.anySourceEnabled();
  /* A held arrow polls the key every 50 ms until the finger comes off — a bounded burst, and
   * the map is being scrolled, so the CPU is awake for it anyway. */
  const bool holding = panHoldMask && (appState == MAPS_VIEW);
  /* A download running under the map: a tick every MAPS_JOB_POLL_MS to see whether it has
   * written into the area on the screen (jobTilesLanded). The download holds the CPU up anyway. */
  const bool jobPoll = (appState == MAPS_VIEW) && tileFetchActive();
  /* A pin's frame waiting for the radio's answer (review M1): 4 Hz for the second or two it
   * takes, then nothing. Any state — the answer can change a pin under a menu too. */
  const bool meshWatch = meshOp.txId != 0;
  /* A height owed to the screen (elevDue): 50 ms ticks until it is read - the first waits for
   * the view to settle (MAPS_ELEV_SETTLE_MS), each takes one sample - then nothing. The settle
   * clock is told about the view every draw, which is where this is called from. */
  elevNoteView();
  const bool elevOwed = elevDue() != ELEV_DUE_NONE;
  controlState.msAppTimerEventPeriod = busy ? 25 : ((holding || elevOwed) ? 50 :
                                       ((progress || meshWatch) ? 250 :
                                       (following ? 1000 : (jobPoll ? MAPS_JOB_POLL_MS : 0))));
}

// ---------------------------------------------------------------- altitude (0.9.80)

/* The layer is read only while the setting is on and the last card scan found /maps/elev
 * (elevDue also leaves the no-map page alone: it has no strip to put a height on). */
bool MapsApp::elevActive() const {
  return altOn && elevPresent && cardOk;
}

bool MapsApp::elevHereCurrent() const {
  return elevHere.valid && elevHereZoom == zoom && elevHereCx == cx && elevHereCy == cy;
}

/* Who "you" is for the comparison: selfPosition()'s answer (a fresh GPS fix, else your pin,
 * else a poor or an old fix - the same ranking as the marker and the `0` key), else the mesh's
 * reference place, named. MAPS_SELF_NONE when there is neither. */
int MapsApp::elevYouNow(int32_t* la, int32_t* lo, char* name, size_t cap) const {
  uint32_t age = 0;
  int src = SELF_NONE;
  uint16_t acc = 0;
  const int kind = selfPosition(la, lo, &age, &src, &acc);
  if (name && cap) {
    name[0] = '\0';
  }
  if (kind == MAPS_SELF_GPS && src == SELF_NET && name && cap) {
    /* A Wi-Fi fix is "you" give or take its size, and says so (self_pos.h: never without it):
     * "35m below Wi-Fi +/-60 m" - 60 m across a 30-degree slope is ~35 m of height. */
    snprintf(name, cap, "Wi-Fi +/-%u m", (unsigned)acc);
  }
  if (kind != MAPS_SELF_NONE) {
    return kind;
  }
  if (meshService.resolveReference(la, lo, name, cap)) {
    return MAPS_ELEV_YOU_REF;
  }
  return MAPS_SELF_NONE;
}

/* The comparison half of a row: the cached "you" if it is a height (rc 1) for who "you" is
 * NOW - the same kind, the same named place - and within MAPS_ELEV_YOU_SHOW_M of where that is
 * (a resample is owed past MAPS_ELEV_YOU_MOVE_M and comes on the next tick). */
bool MapsApp::elevYouUsable(ElevTextIn* in) const {
  if (!elevYou.valid || elevYou.rc != 1) {
    return false;
  }
  int32_t la = 0, lo = 0;
  char nm[24];
  const int kind = elevYouNow(&la, &lo, nm, sizeof(nm));
  if (kind == MAPS_SELF_NONE || kind != elevYouKind ||
      (kind == MAPS_ELEV_YOU_REF && strcmp(nm, elevYouName) != 0) ||
      meshPosDistanceM(elevYou.latI, elevYou.lonI, la, lo) > MAPS_ELEV_YOU_SHOW_M) {
    return false;
  }
  if (in) {
    in->haveRef = true;
    in->refM = elevYou.m;
    in->refZ = elevYou.z;
    /* "you" only for a fresh usable fix or your own pin. A poor or an old fix is named for what it
     * is (review 2026-09-27: "35m above you" from a 3-satellite fix that can be 20 km off, or from
     * where you were an hour ago) - the marker already says "me?" / "me, 12m ago". */
    /* ...and a Wi-Fi fix by its size ("Wi-Fi +/-60 m", elevYouNow): a 100 m circle is not a "you"
     * to state a height difference from as if it were exact. Copied: in->refName outlives this
     * call (one MapsApp at a time, so one buffer). */
    static char s_netYou[24];
    if (kind == MAPS_SELF_GPS && nm[0]) {
      strlcpy(s_netYou, nm, sizeof(s_netYou));
    }
    in->refName = (kind == MAPS_ELEV_YOU_REF) ? elevYouName
                : (kind == MAPS_SELF_POOR_GPS) ? "poor fix"
                : (kind == MAPS_SELF_OLD_GPS) ? "old fix"
                : (kind == MAPS_SELF_GPS && nm[0]) ? s_netYou : NULL;
  }
  return true;
}

/* One row about a cached point (the strip's "Elev ...", the pin screen's "Ground ..."): the
 * point's height, and against "you" when that is known. `current` false = not sampled for what
 * is on the screen yet ("Elev ..."). fitW > 0 measures the row against that many pixels of
 * AKROBAT_BOLD_16 (the strip's vpW - 8) so "(coarse)" and a long name give way before the
 * numbers (elevTextRow). Reads members only - no card I/O, safe from a draw. */
int MapsApp::elevRowText(char* out, size_t cap, const char* label, const ElevPoint* p,
                         bool current, int fitW) {
  ElevTextIn in;
  memset(&in, 0, sizeof(in));
  in.label = label;
  in.units = gUnits;
  in.refZ = -1;
  if (!p || !p->valid || !current) {
    in.rc = ELEV_TXT_PENDING;
  } else {
    in.rc = p->rc;
    in.m = p->m;
    in.z = p->z;
    if (p->rc == 1) {
      elevYouUsable(&in);
    }
  }
  SmoothFont* fnt = fonts[AKROBAT_BOLD_16];
  if (fitW > 0 && fnt) {
    return elevTextRow(out, cap, &in, elevMeasure, fnt, fitW);
  }
  return elevTextRow(out, cap, &in);
}

/* The settle clock. Called after every draw (armTimer) and on every tick: a view that differs
 * from the one last seen restarts it. */
void MapsApp::elevNoteView() {
  if (elevSeenZoom != zoom || elevSeenCx != cx || elevSeenCy != cy) {
    elevSeenZoom = zoom;
    elevSeenCx = cx;
    elevSeenCy = cy;
    elevSeenMs = millis();
  }
}

/* A download writes /maps/elev tiles while the map is open (tile_fetch.h, "Elevation too"): pick
 * them up without a restart or a Rescan. The layer is there once one tile has been renamed into
 * place, so elevPresent turns on without a look at the card; and a cached height a new tile
 * could change - no tile, no data, a card error, or one from the coarse z10 - is forgotten, so
 * elevDue asks for it again on the next tick. A fine z13 height stays: its tile was already
 * there. A counter compare when nothing has landed; safe from the draw path (no I/O, no redraw). */
void MapsApp::elevJobCheck() {
  const uint32_t n = tileFetchElevWritten();
  if (n == elevJobSeen) {
    return;
  }
  elevJobSeen = n;
  if (cardOk) {
    elevPresent = true;
  }
  ElevPoint* const pts[] = { &elevHere, &elevYou, &elevMeas, &elevPin };
  for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
    if (pts[i]->valid && !(pts[i]->rc == 1 && pts[i]->z == ELEV_Z)) {
      pts[i]->valid = false;
    }
  }
  if (!elevHere.valid) {
    elevHereZoom = -1;
  }
}

/* Which sample is owed, most important first: the crosshair's ground (what the row is about),
 * then "you", then the ruler's anchor; on the pin screen, the pin. ELEV_DUE_NONE when every
 * height the screen shows is cached for what it shows - then the timer stands down (armTimer). */
int MapsApp::elevDue() const {
  if (!elevActive()) {
    return ELEV_DUE_NONE;
  }
  if (appState == MAPS_VIEW) {
    if (areaSel < 0 && pinCount == 0 && meshService.getWaypointCount() == 0) {
      return ELEV_DUE_NONE;                // the no-map page: no strip to put a height on
    }
    if (!elevHereCurrent()) {
      return ELEV_DUE_HERE;
    }
    int32_t la = 0, lo = 0;
    char nm[24];
    const int kind = elevYouNow(&la, &lo, nm, sizeof(nm));
    if (kind != MAPS_SELF_NONE &&
        (!elevYou.valid || kind != elevYouKind ||
         (kind == MAPS_ELEV_YOU_REF && strcmp(nm, elevYouName) != 0) ||
         meshPosDistanceM(elevYou.latI, elevYou.lonI, la, lo) > MAPS_ELEV_YOU_MOVE_M)) {
      return ELEV_DUE_YOU;
    }
    if (measuring && (!elevMeas.valid || elevMeas.latI != measLatI || elevMeas.lonI != measLonI)) {
      return ELEV_DUE_MEAS;
    }
    return ELEV_DUE_NONE;
  }
  if (appState == MAPS_PIN_OPTS && pins && pinSel >= 0 && pinSel < pinCount &&
      (!elevPin.valid || elevPin.latI != pins[pinSel].latI || elevPin.lonI != pins[pinSel].lonI)) {
    return ELEV_DUE_PIN;
  }
  return ELEV_DUE_NONE;
}

/* One sample, into a cached point. The ONLY place the map reads the elevation layer. */
void MapsApp::elevSampleInto(ElevPoint* p, double lat, double lon) {
  double m = 0;
  int z = -1;
  bool ioErr = false;
  const uint32_t t0 = micros();
  int rc = elevSampleCard(lat, lon, &m, &z, &ioErr);
  p->costUs = micros() - t0;
  if (rc != 1 && ioErr) {
    /* "Not on the card" and "the card had a moment" are different sentences. Not retried by
     * itself (a pulled card would be re-read 20 times a second): the next move of the view, or
     * Rescan, asks again. */
    rc = ELEV_TXT_IOERR;
    log_e("MAPS: elevation at %.5f,%.5f: the card could not be read", lat, lon);
  }
  p->valid = true;
  p->latI = mapDegToI7(lat);
  p->lonI = mapDegToI7(lon);
  p->rc = rc;
  p->m = (rc == 1) ? m : 0.0;
  p->z = (rc == 1) ? z : -1;
}

/* The app timer's altitude work: at most ONE sample a tick (a tile piece is another tick's
 * work - the map's pixels go first), only while the view has been still MAPS_ELEV_SETTLE_MS,
 * and never under a held arrow (processEvent calls this only with no hold running: the
 * blip-wait tick in particular must do nothing else). True = something changed on the screen.
 * `now`: the view was moved by Follow me for a new fix, not by a thumb - there is nothing to
 * wait out, and the sample is taken in the SAME tick as the re-centre so its one redraw shows it
 * (review 2026-09-27: the settle rule showed "Elev ..." and then paid a second full redraw 150 ms
 * later, for every fix - two redraws a second and a flickering row). */
bool MapsApp::elevStep(bool now) {
  const int due = elevDue();
  if (due == ELEV_DUE_NONE || panHoldMask) {
    return false;
  }
  if (due == ELEV_DUE_PIN) {
    elevSampleInto(&elevPin, mapI7ToDeg(pins[pinSel].latI), mapI7ToDeg(pins[pinSel].lonI));
    elevPin.latI = pins[pinSel].latI;      // the key is the pin's own position, not re-rounded
    elevPin.lonI = pins[pinSel].lonI;
    /* The note is part of the menu: rebuild it where the thumb is. buildPinOpts only reads the
     * cache (it runs from key handlers too). */
    const MenuOption::keyType keep = menu ? menu->currentKey() : 0;
    buildPinOpts();
    if (menu && keep && keep != MENU_ROW_NOTE) {
      menu->select(keep);
    }
    return true;
  }
  if (!now && (loadActive || wantAny)) {
    return false;                          // a tile piece first; this waits a tick or two
  }
  elevNoteView();
  if (due == ELEV_DUE_HERE) {
    if (!now && (uint32_t)(millis() - elevSeenMs) < MAPS_ELEV_SETTLE_MS) {
      return false;                        // still moving (or just stopped): wait for it
    }
    double lat = 0, lon = 0;
    mapViewToLatLon(zoom, cx, cy, vpW, vpH, vpW / 2, vpH / 2, &lat, &lon);
    /* Follow me (`now`): GPS noise moves the crosshair a pixel on most fixes, and each of those
     * was a card read in the frame's own pass, once a second (review 2026-09-27). Within
     * MAPS_ELEV_YOU_MOVE_M of the last sample - the distance "you" is re-read at - its height
     * stands for the new view (the layer's pixels are ~13 m) and the card is not touched. A card
     * error is not held: the next fix asks again, as a moved view always has. */
    if (now && elevHere.valid && elevHere.rc != ELEV_TXT_IOERR &&
        meshPosDistanceM(elevHere.latI, elevHere.lonI, mapDegToI7(lat), mapDegToI7(lon)) <=
            MAPS_ELEV_YOU_MOVE_M) {
      elevHereZoom = zoom;
      elevHereCx = cx;
      elevHereCy = cy;
      return false;                        // nothing read, nothing on the screen changed
    }
    elevSampleInto(&elevHere, lat, lon);
    elevHereZoom = zoom;
    elevHereCx = cx;
    elevHereCy = cy;
    return true;
  }
  if (due == ELEV_DUE_YOU) {
    int32_t la = 0, lo = 0;
    char nm[24];
    const int kind = elevYouNow(&la, &lo, nm, sizeof(nm));
    if (kind == MAPS_SELF_NONE) {
      return false;
    }
    if (now && kind == MAPS_SELF_GPS && elevHereCurrent() &&
        meshPosDistanceM(elevHere.latI, elevHere.lonI, la, lo) <= MAPS_ELEV_YOU_MOVE_M) {
      /* Follow me: the crosshair IS you (the view was just centred on this fix) - its sample is
       * yours, and the same ground is not read twice in the frame's pass (review 2026-09-27). */
      elevYou = elevHere;
    } else {
      elevSampleInto(&elevYou, mapI7ToDeg(la), mapI7ToDeg(lo));
    }
    elevYou.latI = la;                     // the key is the position as given, not re-rounded
    elevYou.lonI = lo;
    elevYouKind = kind;
    strlcpy(elevYouName, nm, sizeof(elevYouName));
    return true;
  }
  if (due == ELEV_DUE_MEAS) {
    if (elevHereCurrent() && elevHere.latI == measLatI && elevHere.lonI == measLonI) {
      elevMeas = elevHere;                 // "Measure from here": the crosshair's own sample
      return true;
    }
    elevSampleInto(&elevMeas, mapI7ToDeg(measLatI), mapI7ToDeg(measLonI));
    elevMeas.latI = measLatI;
    elevMeas.lonI = measLonI;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- the serial console

/* See the note in app_maps.h: a map's failure mode is showing the wrong ground, which looks
 * exactly like showing the right ground, and no screenshot can tell them apart. These two
 * functions are how the whole thing is driven over a cable. */
/* ⚠ `w += snprintf(out + w, cap - w, ...)` IS A BUFFER OVERFLOW WAITING FOR A LONG ENOUGH
 * AREA NAME. snprintf returns what it WOULD have written, so on truncation w passes cap, and
 * `cap - w` is size_t: it underflows to about four billion and the next call writes off the
 * end. This is the standard trap in the standard shape, and the fix is to append through one
 * function that cannot get it wrong. */
static size_t sayInto(char* out, size_t cap, size_t w, const char* fmt, ...) {
  if (!out || w >= cap) {
    return w;
  }
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(out + w, cap - w, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return w;
  }
  const size_t room = cap - w - 1;
  return w + ((size_t)n > room ? room : (size_t)n);
}

bool mapsConsoleStatus(char* out, size_t cap) {
  if (!out || cap == 0) {
    return false;
  }
  out[0] = '\0';
  size_t w = 0;
  int areaN = 0;
  char first[32];
  first[0] = '\0';
  int zMin = -1, zMax = -1;
  bool elevSeen = false;
  if (SD.exists(MAPS_ROOT)) {
    File dir = SD.open(MAPS_ROOT);
    if (dir && dir.isDirectory()) {
      File f;
      while ((f = dir.openNextFile())) {
        const char* nm = baseName(f.name());
        if (f.isDirectory() && mapsReservedName(nm)) {
          elevSeen = true;                 // the altitude layer: reported below, never an area
        } else if (f.isDirectory() && mapAreaNameOk(nm)) {
          if (!first[0]) {
            strlcpy(first, nm, sizeof(first));
            char zp[96];
            snprintf(zp, sizeof(zp), "%s/%s", MAPS_ROOT, nm);
            File zd = SD.open(zp);
            if (zd && zd.isDirectory()) {
              File zf;
              while ((zf = zd.openNextFile())) {
                const int z = zf.isDirectory() ? zoomFolder(baseName(zf.name())) : -1;
                if (z >= 0) {
                  if (zMin < 0 || z < zMin) {
                    zMin = z;
                  }
                  if (z > zMax) {
                    zMax = z;
                  }
                }
                zf.close();
              }
            }
            if (zd) {
              zd.close();
            }
          }
          areaN++;
        }
        f.close();
      }
    }
    if (dir) {
      dir.close();
    }
  }
  w = sayInto(out, cap, w, "maps: %d area(s) under %s", areaN, MAPS_ROOT);
  if (first[0]) {
    w = sayInto(out, cap, w, "; first '%s' z%d-%d", first, zMin, zMax);
  }
  // Second, so a long list of areas below cannot push it out of the buffer.
  w = sayInto(out, cap, w, "\nelev: %s/%s %s (the altitude layer, not an area - `elev`)", MAPS_ROOT,
              ELEV_DIR, elevSeen ? "is on the card" : "is NOT on the card");
  if (s_instance) {
    /* With the app open, its scan knows where each area's tiles ARE (scanAreaExtent). */
    for (int i = 0; i < s_instance->areaCount; i++) {
      const MapsApp::MapArea* a = &s_instance->areas[i];
      if (!a->hasExtent) {
        w = sayInto(out, cap, w, "\n  %s z%d-%d: no tiles found at z%d", a->name, a->zMin, a->zMax, a->zMin);
        continue;
      }
      double la = 0, lo = 0;
      s_instance->areaCentre(i, &la, &lo);
      double n = 0, wl = 0, s2 = 0, e = 0;
      mapWorldToLatLon((double)a->xMin * MAP_TILE_PX, (double)a->yMin * MAP_TILE_PX, a->zMin, &n, &wl);
      mapWorldToLatLon((double)(a->xMax + 1) * MAP_TILE_PX, (double)(a->yMax + 1) * MAP_TILE_PX, a->zMin, &s2, &e);
      w = sayInto(out, cap, w, "\n  %s z%d-%d: lat %.3f..%.3f lon %.3f..%.3f, centre %.4f,%.4f%s",
                  a->name, a->zMin, a->zMax, s2, n, wl, e, la, lo, i == s_instance->areaSel ? " (shown)" : "");
    }
  }

  Preferences p;
  if (p.begin(MAPS_NVS, true)) {
    if (p.getInt("saved", 0) == 1) {
      char area[32];
      area[0] = '\0';
      p.getBytes("area", area, sizeof(area));
      area[sizeof(area) - 1] = '\0';
      /* ~2 decimals and no finer (the almanac console's coordsCoarse rule): the saved view is
       * often centred on a measured or Wi-Fi fix - roughly where the phone sits, often a home -
       * and this line is the kind pasted into bench notes in a public repo. */
      w = sayInto(out, cap, w, "\nlast view: ~%.2f,%.2f z%d area '%s'",
                  mapI7ToDeg(p.getInt("lat", 0)), mapI7ToDeg(p.getInt("lon", 0)),
                  p.getInt("z", 0), area);
    } else {
      w = sayInto(out, cap, w, "\nlast view: none saved yet");
    }
    p.end();
  }
  {
    File f = SD.open(MAPS_PINS_FILE, FILE_READ);
    int pinN = 0, badN = 0;
    if (f) {
      char line[MAP_PIN_LINE_MAX];
      size_t n = 0;
      bool overlong = false;
      MapPin tmp;
      /* ⚠ THE SAME OVER-LONG RULE loadPins USES. Without it this counter silently parses the
       * FIRST MAP_PIN_LINE_MAX bytes of a long line as a pin and calls it good, while the app
       * counts the same line as unreadable — so the console would report more pins than the
       * map draws, which is precisely the question somebody runs `maps` to answer. */
      while (f.available()) {
        const int ch = f.read();
        if (ch < 0) {
          break;
        }
        if (ch == '\n' || ch == '\r') {
          line[n] = '\0';
          if (overlong) {
            badN++;
          } else if (n) {
            const int r = mapPinParseLine(line, &tmp);
            if (r == 1) {
              pinN++;
            } else if (r < 0) {
              badN++;
            }
          }
          n = 0;
          overlong = false;
          continue;
        }
        if (n + 1 < sizeof(line)) {
          line[n++] = (char)ch;
        } else {
          overlong = true;
        }
      }
      if (n && !overlong) {
        line[n] = '\0';              // a last line with no newline is still a line
        const int r = mapPinParseLine(line, &tmp);
        if (r == 1) {
          pinN++;
        } else if (r < 0) {
          badN++;
        }
      } else if (overlong) {
        badN++;
      }
      f.close();
    }
    w = sayInto(out, cap, w, "\npins: %d good, %d unreadable in %s",
                pinN, badN, MAPS_PINS_FILE);
    (void)w;
  }
  return true;
}

/* Serial `elev`: what the map would say about altitude, from its CACHE (no card I/O but the
 * one stat that says whether the layer is there when the app is closed). Lines under 192 B. */
bool mapsConsoleElev(char* out, size_t cap) {
  if (!out || cap == 0) {
    return false;
  }
  out[0] = '\0';
  size_t w = 0;
  MapsApp* a = s_instance;
  bool altOn = true;
  if (a) {
    altOn = a->altOn;
  } else {
    Preferences p;
    if (p.begin(MAPS_NVS, true)) {
      altOn = p.getInt("alt", 1) != 0;
      p.end();
    }
  }
  const bool present = a ? a->elevPresent : elevCardPresent();
  w = sayInto(out, cap, w, "elev: %s/%s %s (%s); Altitude %s; units %s; the map is %s",
              MAPS_ROOT, ELEV_DIR, present ? "is on the card" : "is NOT on the card",
              a ? "as the map's last scan found it" : "looked just now",
              altOn ? "ON" : "off", gUnits == UNITS_US ? "US" : "metric",
              a ? "open" : "closed - `elev <lat> <lon>` samples any point");
  if (!a) {
    return true;
  }
  char t[48];
  const MapsApp::ElevPoint* h = &a->elevHere;
  if (!h->valid) {
    w = sayInto(out, cap, w, "\nelev: crosshair: not sampled yet%s",
                a->elevActive() ? "" : " (nothing is read while the layer is off or absent)");
  } else {
    elevSampleText(t, sizeof(t), h->rc, h->m, h->z);
    w = sayInto(out, cap, w, "\nelev: crosshair %.5f,%.5f -> %s, %lu.%lu ms%s", mapI7ToDeg(h->latI),
                mapI7ToDeg(h->lonI), t, (unsigned long)(h->costUs / 1000u),
                (unsigned long)((h->costUs % 1000u) / 100u),
                a->elevHereCurrent() ? "" : " (the view has moved since: not current)");
  }
  const MapsApp::ElevPoint* y = &a->elevYou;
  if (!y->valid) {
    w = sayInto(out, cap, w, "\nelev: you: not sampled (no GPS fix, no pin, no reference - or not yet)");
  } else {
    const int k = a->elevYouKind;
    char who[40];
    if (k == MAPS_ELEV_YOU_REF) {
      snprintf(who, sizeof(who), "the reference '%s'", a->elevYouName);
    } else {
      strlcpy(who, k == MAPS_SELF_GPS ? "live fix" : k == MAPS_SELF_PIN ? "your pin"
                   : k == MAPS_SELF_POOR_GPS ? "a poor fix" : "an old fix", sizeof(who));
    }
    elevSampleText(t, sizeof(t), y->rc, y->m, y->z);
    /* "You" to 2 decimals (~1 km): it is this phone's own fix (the woods plate's, or the hotspot
     * phone's - on by default since 0.9.82), and console captures get pasted into notes in a
     * PUBLIC repo. The sample's answer is what this line is for; `elev <lat> <lon>` is exact. */
    w = sayInto(out, cap, w, "\nelev: you = %s ~%.2f,%.2f -> %s, %lu.%lu ms%s", who,
                mapI7ToDeg(y->latI), mapI7ToDeg(y->lonI), t, (unsigned long)(y->costUs / 1000u),
                (unsigned long)((y->costUs % 1000u) / 100u),
                (y->rc != 1 || a->elevYouUsable(NULL)) ? ""
                  : " (not usable now: you moved, or 'you' is somebody else now)");
  }
  if (a->measuring && a->elevMeas.valid) {
    elevSampleText(t, sizeof(t), a->elevMeas.rc, a->elevMeas.m, a->elevMeas.z);
    w = sayInto(out, cap, w, "\nelev: ruler anchor -> %s", t);
  }
  if (a->elevActive()) {
    char row[64];
    a->elevRowText(row, sizeof(row), NULL, &a->elevHere, a->elevHereCurrent(), a->vpW - 8);
    w = sayInto(out, cap, w, "\nelev: the strip reads \"%s\"", row);
  }
  (void)w;
  return true;
}

bool mapsConsoleGoto(double lat, double lon, int z, const char* area,
                     char* why, size_t whyCap) {
  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
    snprintf(why, whyCap, "lat must be -90..90 and lon -180..180");
    return false;
  }
  if (z < MAP_ZOOM_MIN || z > MAP_ZOOM_MAX) {
    snprintf(why, whyCap, "zoom must be %d..%d", MAP_ZOOM_MIN, MAP_ZOOM_MAX);
    return false;
  }
  Preferences p;
  if (!p.begin(MAPS_NVS, false)) {
    snprintf(why, whyCap, "could not open the maps settings");
    return false;
  }
  p.putInt("lat", mapDegToI7(mapClampLat(lat)));
  p.putInt("lon", mapDegToI7(mapWrapLon(lon)));
  p.putInt("z", z);
  char buf[32];
  memset(buf, 0, sizeof(buf));
  if (area && area[0]) {
    strlcpy(buf, area, sizeof(buf));
  }
  p.putBytes("area", buf, sizeof(buf));
  p.putInt("saved", 1);
  p.end();
  /* ⚠ SAY THAT AN OPEN APP WILL OVERWRITE THIS. MapsApp::saveView() runs in the destructor,
   * so closing the app writes wherever it was actually looking, over whatever was set here.
   * That precedence is right — the screen is the truth while somebody is using it — but a
   * person setting a coordinate over the cable with the app open would otherwise watch it
   * vanish with no explanation. */
  snprintf(why, whyCap, "next open: %.5f,%.5f z%d%s%s (close Maps first if it is open)",
           lat, lon, z, buf[0] ? " area " : "", buf);
  return true;
}
