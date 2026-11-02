/*
 * app_maps.h — an offline map. Tiles off the SD card, pins you drop yourself, and everything
 * the mesh already knows about where people are, on one screen.
 *
 * Main menu -> Maps (its own row, with an icon).
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * WHAT IT IS FOR
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * The Meshtastic screens already carry every fact this app draws: camp is a waypoint, the
 * truck is a waypoint, everyone's last position arrives on port 3 every few minutes, and the
 * Nodes list turns them into "3.2km E of camp - 4 min ago". That sentence is correct and it
 * is the wrong shape for the question people actually ask, which is "is he on the other side
 * of the creek or this side". A map answers that and a list cannot.
 *
 * So: no new radio traffic, no new database, no new source of truth. This app is a VIEW of
 * state that already exists, plus one thing of its own — pins, which are your private marks
 * on your own map and are shared with the mesh only when you say so.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * THE TILES: RAW RGB565, AND WHY NOT PNG
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * /maps/<area>/<z>/<x>/<y>.565 — 256x256 pixels, RGB565, little-endian, 131072 bytes exactly.
 * Standard slippy-map z/x/y numbering, so the tiles a computer already has for COVEY's map are
 * the same tiles, converted once by tools/convert_tiles.py. docs/maps.md is the format.
 *
 * 🛑 THE PHONE DECODES NOTHING, AND THAT IS THE DESIGN, NOT A SHORTCUT. There is no PNG
 * decoder in this firmware and adding one costs a vendored library plus contiguous internal
 * RAM on a phone whose crashes are internal-heap fragmentation (app_photos.h has the whole
 * argument). The ESP32's ROM JPEG decoder exists but refuses greyscale outright — jpeg_grey.h
 * exists solely because 33 of 45 pictures in one book were 1-component JPEGs. A map made of
 * compressed tiles would therefore work until the day it met a tile of the wrong flavour, in
 * the woods, with no way to tell what was wrong.
 *
 * Raw costs 8x the card space (~85 MB for a 20x20 km area at z12-z15 — nothing on a 32 GB
 * card) and buys: zero decode time, zero decoder memory, and exactly ONE failure mode, which
 * is "the file is not 131072 bytes long" and is checkable in one comparison.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * WHY THE CARD IS READ IN PIECES
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * ⚠ ONE TILE IS 128 KB AND EVERYTHING IN THIS FIRMWARE SHARES ONE TASK. A 128 KB SD read is
 * 100-250 ms on this hardware, and 250 ms is precisely the threshold the superloop's own
 * stall detector was built to complain about (0.9.58) — the same 1-2 s freezes that drop WiFi
 * and eat keypresses. Four tiles for one screen, read the obvious way, is a second of frozen
 * phone every time you pan.
 *
 * So a tile arrives in MAPS_CHUNK_BYTES pieces, one piece per timer tick, and the map draws
 * the hole in the meantime as a plainly-marked grey square. The screen is honest about what
 * it does not have yet, the keypad never stops answering, and no single pass through the
 * superloop owes more than a fraction of a tile.
 *
 * The file is opened, seeked, read and CLOSED on every piece rather than held open across
 * ticks. That costs a directory lookup per piece and removes the entire class of bug where an
 * app is torn down, or the card is pulled, with a File still live.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * MEMORY
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * MAPS_TILE_SLOTS x 128 KB of PSRAM, allocated ONCE when the app opens and freed when it
 * closes — no churn, because heap churn is what this phone dies of. Internal RAM: a file
 * handle and some ints. The app itself is PSRAM too (WiPhoneApp::operator new).
 *
 * A 240x250 viewport over 256 px tiles touches at most 2x2 = 4 tiles, so 6 slots means
 * panning across a tile boundary does not immediately evict something about to be needed
 * again. Fewer than 2 slots and the app refuses to run and says why.
 */
#ifndef APP_MAPS_H
#define APP_MAPS_H

#include "GUI.h"
#include "map_tiles.h"
#include "map_pins.h"
#include "meshtastic_service.h"   // MeshChannel: a pin remembers the channel it is shared on
#include "tile_fetch.h"           // TileJobStatus: a download filling the area on screen
#include "elev_text.h"            // ElevTextIn: the altitude row's words (0.9.80)

#define MAPS_ROOT          "/maps"
#define MAPS_PINS_FILE     "/maps/pins.txt"

#define MAPS_MAX_PINS      64      // PSRAM, ~2 KB. A day's worth of marks, not a database.
#define MAPS_MAX_AREAS     12
#define MAPS_TILE_SLOTS    6       // x 128 KB PSRAM; see the note above
/* Tiles known not to be on the card, so they are not re-asked every frame. 64 because a
 * position far from the downloads remembers one hole for every level it walks past (up to
 * MAP_ANCESTOR_MAX_D + 1 = 9), and 4 positions x 9 = 36 can be on the screen at once. A cache
 * that has to evict one of THOSE re-asks the card for it at once, redraws, and does it again
 * (mapMissSlot, map_tiles.h). 16 B each, in the PSRAM app object. */
#define MAPS_MISS_CACHE    64
#define MAPS_CHUNK_BYTES   (32u * 1024u)
#define MAPS_PICK_RADIUS   14      // how near the crosshair a pin must be to be "under" it
#define MAPS_NOTE_ROWS     2       // the strip grows to this many rows for a long note
#define MAPS_MAX_LIST      64      // rows in the Pins / Places / Nodes lists

// What selfPosition() found. See the note on it.
#define MAPS_SELF_NONE     0
/* "GPS" in these names is history: since 0.9.82 a fix is this phone's own GPS OR the hotspot
 * phone's (self_pos.h), and selfPosition()'s `src` out-parameter says which. */
#define MAPS_SELF_GPS      1       // a LIVE measured fix (fresher than MESH_GPS_FRESH_MS, good enough)
#define MAPS_SELF_PIN      2       // the position the user declared by hand
#define MAPS_SELF_OLD_GPS  3       // a fix, but an old one: drawn grey, wearing its age
#define MAPS_SELF_POOR_GPS 4       // fresh, but under 4 satellites / HDOP over 10: grey, and says so
#define MAPS_ELEV_YOU_REF  5       // (altitude only) no position of your own: the reference place

/* Set while a MapsApp exists. WiPhone.ino's music transport leaves F1-F3 alone while it is
 * up (the Game Boy's gGbcActive rule): on the map the top buttons are zoom and the third is
 * "centre on me", and a zoom key that pauses the music instead is the silent "dead hardware"
 * symptom the keypad self-test already documents. */
extern volatile bool gMapsActive;
/* The pre-power-off hook, like booksSaveOpenPosition(): both power-off paths pull the latch
 * BEFORE any destructor runs, so the view must be written by hand from WiPhone.ino first.
 * True if a MapsApp was open and its view (and any unsaved pins) went to the card. */
bool mapsSaveOpenView();

/* Everything the serial console needs, so this app can be driven without a thumb.
 *
 * ⚠ THIS IS NOT A CONVENIENCE. "Set as wallpaper" reached a user completely untried because
 * every path into it needed a finger on a key and a serial cable cannot press keys
 * (app_photos.cpp). A map is worse: its failure mode is showing the wrong ground, which looks
 * exactly like showing the right ground. `maps` prints what the card holds and where the view
 * is; `maps goto` moves it. Both drive the same code the keypad does. */
bool mapsConsoleStatus(char* out, size_t cap);
bool mapsConsoleGoto(double lat, double lon, int z, const char* area, char* why, size_t whyCap);
/* Serial `elev` (0.9.80): the altitude layer, the setting, and - with the app open - the last
 * samples of the crosshair's ground and "you", as the strip would word them. Several lines,
 * each under 192 bytes (print with sayLines). Reads the map's CACHE only: no card I/O. */
bool mapsConsoleElev(char* out, size_t cap);

/* One marker the arrows can stop on: which kind (SNAP_PIN / SNAP_PLACE / SNAP_NODE) and its
 * index in that kind's table. Indices are only good for the press they were found in — the
 * mesh tables compact. */
struct MapsSnapHit {
  int kind;
  int idx;
};

class MapsApp : public WindowedApp {
public:
  MapsApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~MapsApp();
  ActionID_t getId() {
    return GUI_APP_MAPS;
  }
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll = false);
  bool drewInsideBand();         // the map view paints only between the header and the footer

protected:
  typedef enum {
    MAPS_VIEW,          // the map itself
    MAPS_MENU,          // the map's own menu (Back from here returns to the map)
    MAPS_LIST,          // pins / places / nodes, whichever listKind says
    MAPS_PIN_OPTS,      // one pin: rename, move, share, delete
    MAPS_RENAME,        // the text field behind "Rename"
    MAPS_AREAS,         // which map folder on the card
    MAPS_CONFIRM_DEL,
    MAPS_HELP,
    MAPS_DOWNLOAD,      // source / radius / detail / start-stop, with the estimate and progress
    MAPS_GOTO,          // type a latitude and a longitude
    MAPS_CHANNEL,       // which channel a pin is shared on
  } MapsState_t;

  typedef enum { LIST_PINS = 0, LIST_PLACES, LIST_NODES } MapsListKind_t;

  // ---- one cached tile ----
  struct TileSlot {
    uint16_t* px;          // MAP_TILE_PX*MAP_TILE_PX pixels, PSRAM. NULL = slot never allocated
    int       z, tx, ty;   // what it holds; z < 0 = empty
    uint32_t  used;        // LRU stamp
    bool      ready;       // fully read; false while a load is in flight
  };

  struct MapArea {
    char name[32];
    int  zMin, zMax;       // the zoom folders that actually exist
    /* Where its tiles ARE, read once at scan time from the folder names at zMin (the
     * coarsest level: a handful of x folders, two of them listed for their y range). An
     * area that is not where you are looking is otherwise indistinguishable from an empty
     * one — {user}, 2026-09-20, on "home": "I don't have any tiles for it". Tile units at
     * zMin; hasExtent false when the folders held nothing numeric. */
    bool hasExtent;
    long xMin, xMax, yMin, yMax;
  };
  bool areaHolds(int idx, double lat, double lon) const;   // is this ground inside its box?
  void areaCentre(int idx, double* lat, double* lon) const;
  // "12km NE" / "here": the area's tiles relative to a point, for the strip and the list.
  void areaWhere(int idx, double lat, double lon, char* out, size_t n) const;

  // ---- state ----
  MapsState_t appState;
  MapsListKind_t listKind;
  /* ⚠ WHAT EACH LIST ROW *IS*, NOT WHERE IT SAT. A row key used to be the index into the
   * service's waypoint or node array — and those arrays move under an open screen: the
   * waypoint expiry sweep compacts with `waypoints[i] = waypoints[--count]`, and getNode()
   * re-sorts whenever the node count changes. So the row said "Camp" and OK centred on the
   * truck. Rows now carry the IDENTITY of the thing (waypoint id, node number, pin index)
   * and the handler looks it up again, which also means a place that expired while the list
   * was open resolves to nothing and says so instead of moving the map somewhere else. */
  uint32_t listId[MAPS_MAX_LIST];
  int      listCount;

  MenuWidget*          menu;
  MultilineTextWidget* textArea;
  char headerTitle[40];     // HeaderWidget stores the POINTER; this must outlive the call
  char note[72];            // one line of result/why nothing happened, shown on the map
  char statusLine[48];      // "loading 2 tiles" / "no tiles here" — the map's own honesty

  // The view. See map_tiles.h for why this is world pixels and not a lat/lon pair.
  int      zoom;
  int32_t  cx, cy;
  int      vpX, vpY, vpW, vpH;

  MapArea  areas[MAPS_MAX_AREAS];
  int      areaCount;
  int      areaSel;         // index into areas[], -1 when the card has no maps at all

  MapPin*  pins;            // PSRAM
  int      pinCount;
  int      pinSel;          // index into pins[] for PIN_OPTS/RENAME/CONFIRM_DEL
  bool     pinsDirty;
  /* ── A PIN'S FRAME ON ITS WAY TO THE RADIO (review M1, 2026-09-25) ────────────────────────
   * 🛑 QUEUED IS NOT ON THE AIR. A share, an update, "Take it off the mesh" and a shared pin's
   * delete each QUEUE one waypoint frame; the map now keeps its packet id here and asks the
   * service (txOutcome) on the app timer until the radio answers, and mapPinMeshSettle()
   * (map_pins.h, host-tested) decides what the answer means. One at a time: a new operation
   * settles the old one first, with no more waiting. ~80 bytes in the app object. */
  struct {
    uint32_t txId;          // the frame's packet id; 0 = nothing pending
    uint32_t wpId;          // the waypoint it concerns (pins are found BY THIS: the list moves)
    uint32_t sinceMs;       // when it was queued: the give-up clock
    uint8_t  op;            // MapPinMeshOp
    MapPin   pin;           // the pin as it was: name/channel for the note, all of it for a restore
  } meshOp;
  int*     pinVx;           // projection scratch, PSRAM: members not stack, this is the GUI task
  int*     pinVy;

  TileSlot slots[MAPS_TILE_SLOTS];
  int      slotsAlive;
  uint32_t useSeq;

  // The load in flight (one at a time, one piece per tick).
  bool     loadActive;
  int      loadSlot, loadZ, loadTx, loadTy;
  uint32_t loadGot;
  int      loadRetries;     // a transient card error gets one more go before a blacklist

  // Wanted but not resident: what the next load should fetch.
  bool     wantAny;
  int      wantZ, wantTx, wantTy;
  int      pendingTiles;      // positions painted LOADING: nothing of theirs to draw yet

  // ════ THE STRETCH (2026-09-23): per-position ancestors, their label, the miss cache ════
  /* Everything drawMap() needs per frame lives HERE, in the PSRAM app object, never on the
   * stack: drawMap runs on the loop task, whose 8 KB stack has a measured floor of a few hundred
   * bytes (tile_fetch.h). */
  MapBlit  viewBlits[MAP_MAX_BLITS];
  uint16_t wideRow[TFT_WIDTH];      // one ancestor row, widened to screen pixels
  MapMiss  misses[MAPS_MISS_CACHE]; // tiles known missing (MAPS_MISS_CACHE, mapMissSlot)
  int      missCount;
  uint32_t drawSeq;                 // frames drawn: the stamp a miss gets when a frame walks past it
  /* What the last frame stretched, for the status chip and the corner star: the shallowest and
   * deepest levels drawn stretched (a level's factor is 2^(zoom - level)). The star shows
   * whenever ANY position is stretched, not only past the area's deepest level. */
  bool     stretchAny;
  int      stretchMinL, stretchMaxL;
  bool     statusIsStretch;         // statusLine holds the stretch label (drawChips may shorten it)
  bool     readErrorInView;         // a position fell back past a tile the card could not read
  /* A download filling the area on screen. Its progress is read every MAPS_JOB_POLL_MS while
   * the map is up, and the misses at the levels it has written since the last look are
   * forgotten, so those tiles are read now instead of staying stretched until a rescan. */
  TileJobStatus jobSt;              // a member, not a local: ~200 B the loop stack cannot spare
  uint32_t jobPollMs;
  bool     jobSeen, jobSeenActive;
  int      jobSeenDone, jobSeenZ, jobSeenTotal, jobSeenPass;

  int      helpTop;         // first help row on screen; the legend does not fit at once
  int      panRun;          // hold repeats so far (the log); 0 = a tap
  int      panCarry;        // the hold's sub-pixel remainder, px x ms (mapPanHoldMove)
  int      panHoldPx;       // pixels the hold has moved so far (the log)
  int      noteRows(SmoothFont* fnt, uint16_t maxW, char (*rows)[72]);   // the note, wrapped
  uint32_t panLastMs;
  /* Hold-to-scroll (2026-09-19). The keypad path suppresses held-key re-reports for every app
   * (that is what killed the menus' auto-repeat), so the map repeats for itself: the arrow's
   * key mask is remembered at the press, the timer polls uiKeyStillHeld() and pans again
   * while the finger stays down, through the same accelerator a run of taps uses. */
  uint32_t panHoldMask;     // the arrow (or digit) physically down, 0 = not holding
  uint32_t panHoldSinceMs;  // when the press landed; the first repeat waits MAP_PAN_HOLD_DELAY_MS
  int      panHoldDx, panHoldDy;
  bool     panHoldRan;      // the hold actually repeated: its end ends the run too
  uint32_t panHoldBlipMs;   // the poll first saw the key up (0 = down); see MAP_PAN_HOLD_BLIP_MS
  /* Snap to pins (2026-09-19, Maps menu toggle, NVS "snap"). A single press that would land on
   * or fly past a pin stops ON it; a press while sitting on a pin jumps to the next pin that
   * way, if one is on screen. Hold-scrolling never snaps. */
  bool     snapPins;        // "Snap to markers" — pins, places and nodes, despite the name

  // ---- the download screen ----
  int      dlSource;        // index into tileSource()
  int      dlRadiusIdx;     // into MAPS_DL_RADII
  int      dlDepth;         // deepest zoom WISHED for (NVS); shown and used clamped to the source (tilePlanDepthShown)
  bool     dlElev;          // "Elevation too: ON/off" (NVS maps/dlelev, default ON): the job fetches /maps/elev as well
  bool     dlHeld;          // the screen is being held awake for the progress display
  uint32_t dlLastMs;        // last progress rebuild
  MenuOption::keyType dlKeep;   // the row to re-select after a progress rebuild
  char     dlWhy[80];       // why the last Start was refused, shown on the form; "" = it was not
  bool     dlShownRunning;  // what the form last drew, so a job that ended off-screen gets a rebuild
  /* ---- 0.9.78, multi-day downloads (P2). Kept together so a merge with the view's work is
   * one hunk. A job that is running or WAITING to resume is shown read-only; Stop is a second
   * press inside MAPS_DL_STOP_ARM_MS; the screen hold lets go MAPS_DL_HOLD_MS after a key. */
  bool     dlShownJob;      // the form last drew a running or waiting job (it re-reads once a second)
  uint32_t dlKeyMs;         // millis() of the last key on the form: the screen hold's clock
  uint32_t dlStopArmMs;     // millis() of the first Stop press; 0 = not armed

  // ---- go to coordinates ----
  char     gotoLat[16], gotoLon[16];
  int      gotoField;       // 0 = latitude, 1 = longitude

  // ---- follow me ----
  /* A latched mode, not a one-shot: while on, every FRESH usable fix re-centres the map, and
   * any scroll switches it off. Losing the fix must NOT move the view (COVEY's field report
   * of 2026-08-22): only a MAPS_SELF_GPS answer moves it, and only when the fix is newer than
   * the one it last centred on. */
  bool     followMe;
  uint32_t followLastStamp; // millis() at which the fix last centred on ARRIVED (millis - age)

  // ---- the ruler ----
  /* COVEY's RUL: an anchor is dropped at the crosshair and PANNING IS THE MEASUREMENT — the
   * bottom strip reads the distance and bearing from the anchor to wherever the crosshair
   * is, and a dashed line joins the two. No new gesture, one menu row on and off. */
  bool     measuring;
  int32_t  measLatI, measLonI;

  // ---- the channel picker ----
  int      chanPending;     // the PUBLIC row awaiting its second press (0 = none)
  int      chanForPin;      // which pin the picked channel is for

  // ════ ALTITUDE (0.9.80): the ground under the crosshair, and against you ════════════════
  /* The elevation layer is /maps/elev on the card (elev_sd.h, docs/maps.md "Altitude"). Every
   * height on this screen is READ ON THE APP TIMER, one sample a tick, and cached here: never
   * in a draw, never in a key handler (a sample is card I/O - up to two tile opens). The draw
   * paths only read these members. A cached point is `valid` for the place it was taken at
   * (and the crosshair's for the VIEW it was taken in); anything else reads as not sampled.
   * 🛑 BOTH HEIGHTS OF A COMPARISON COME FROM THE TILES, never one from the GPS's own altitude:
   * a phone GPS is +-10-20 m vertically and a different datum from the terrain model, so "35m
   * above you" from a GPS height against a tile height is mostly the GPS's error. Two readings
   * of one surface are consistent with each other; that is the question being answered. */
  struct ElevPoint {
    bool     valid;         // sampled at (latI, lonI)
    int32_t  latI, lonI;
    int      rc;            // elevSample: 1 ok, -1 no data, 0 no tile; or ELEV_TXT_IOERR
    double   m;             // metres (rc == 1)
    int      z;             // the layer that answered, -1 none
    uint32_t costUs;        // what the sample took, for the serial `elev`
  };
  bool      altOn;          // Menu "Altitude: ON/off", NVS maps/alt (default ON)
  bool      elevPresent;    // /maps/elev is on the card (checked with the areas: scanAreas)
  ElevPoint elevHere;       // the crosshair's ground...
  int       elevHereZoom;   // ...for THIS view (zoom, cx, cy); zoom -1 = none
  int32_t   elevHereCx, elevHereCy;
  int       elevSeenZoom;   // the view as last seen, and since when: a sample waits until
  int32_t   elevSeenCx, elevSeenCy;   // the view has been still for MAPS_ELEV_SETTLE_MS
  uint32_t  elevSeenMs;
  ElevPoint elevYou;        // "you": selfPosition(), else the reference place (named)
  int       elevYouKind;    // MAPS_SELF_* it was taken for, or MAPS_ELEV_YOU_REF
  char      elevYouName[24];  // the reference's name, when elevYouKind == MAPS_ELEV_YOU_REF
  ElevPoint elevMeas;       // the ruler's anchor
  ElevPoint elevPin;        // the pin on the options screen
  /* A download writing the layer (tileFetchElevWritten) since the map last looked: the layer is
   * there now and a height that a new tile could change is read again - no restart, no Rescan. */
  uint32_t  elevJobSeen;
  bool  elevActive() const;                        // the setting is on and the layer is there
  bool  elevHereCurrent() const;                   // elevHere is for the view on the screen
  int   elevYouNow(int32_t* la, int32_t* lo, char* name, size_t cap) const;
  bool  elevYouUsable(ElevTextIn* in) const;       // fill in's comparison from elevYou, if usable
  enum { ELEV_DUE_NONE = 0, ELEV_DUE_HERE, ELEV_DUE_YOU, ELEV_DUE_MEAS, ELEV_DUE_PIN };
  int   elevDue() const;                           // which sample is owed now (armTimer, the tick)
  bool  elevStep(bool now = false);                // the tick: take ONE owed sample; true = redraw
                                                   // (now: no settle wait, no tile-piece gate - the
                                                   // Follow me re-centre, which is not a thumb)
  void  elevSampleInto(ElevPoint* p, double lat, double lon);
  void  elevNoteView();                            // the settle clock: has the view moved?
  void  elevJobCheck();                            // a download wrote elevation tiles? (no card I/O)
  int   elevRowText(char* out, size_t cap, const char* label, const ElevPoint* p, bool current,
                    int fitW);                     // fitW > 0: fitted to that many px of Akrobat 16
  // ---- setup / teardown ----
  bool  allocSlots();
  void  freeSlots();
  void  scanAreas();
  void  loadPins();
  void  savePins();
  void  restoreView();      // last view, else GPS, else the mesh reference, else the map
  void  saveView();

  bool     tileMemFail;     // no PSRAM for tiles: say so once, stop asking the card
  bool     cardOk;          // the SD card answered at all ("no card" is not "no maps")
  bool     pinsTruncated;   // the pins file held more than MAPS_MAX_PINS: NEVER save over it

  // ---- tiles ----
  /* ── THE STRETCH: THE MOST DETAILED TILE THERE IS, PER POSITION ────────────────────────
   * Every tile POSITION on the screen (a tile at the view `zoom`) is drawn from the deepest
   * level the card has there, walking from min(zoom, area zMax) down to max(area zMin,
   * zoom - MAP_ANCESTOR_MAX_D) (map_tiles.h). One level past the area's deepest tiles
   * (MAP_OVERZOOM_MAX) is the same walk, starting one level up. The VIEW stays at `zoom`:
   * the scale bar, pins, the crosshair readout and panning are geometry at `zoom` and stay
   * exact — only the pixels are borrowed from a level up and stretched. Mixing the two up
   * draws a map that is off by a factor of two, which on forest is not obvious.
   *
   * The loader wants, per position, the FINEST level that is neither in a slot nor known
   * missing; a coarser level is read only once every finer one is known missing. Meanwhile
   * the finest READY level below it is drawn, stretched, and counted as stretched (honest:
   * that is what is on the screen). A miss redraws, so the walk moves a level down at once. */
  int   findSlot(int z, int tx, int ty) const;    // -1 = not cached
  int   claimSlot();                              // LRU victim, never one mid-load
  bool  isKnownMissing(int z, int tx, int ty) const;
  void  rememberMissing(int z, int tx, int ty, bool bad);   // bad = there, but unreadable
  void  forgetMissing();
  int   forgetMissingLevels(int zLo, int zHi);    // a download wrote these levels; returns how many went
  bool  jobTilesLanded();                         // ...has it, since the last look? (MAPS_VIEW tick)
  bool  startLoad(int z, int tx, int ty);
  void  cancelLoad();                             // the zoom or area changed under it
  enum { LOAD_PIECE, LOAD_DONE, LOAD_MISS };      // what one tileLoadStep() did
  int   tileReadFailed(const char* what);         // one retry (LOAD_PIECE), then blacklist (LOAD_MISS)
  int   tileLoadStep();                           // one piece; LOAD_DONE = a tile finished

  // ---- drawing ----
  /* ⚠ EVERY PIXEL THIS APP DRAWS GOES THROUGH fillClipped OR A GUARDED drawString.
   * The screen is one 240x320 sprite shared with the header and the footer, and
   * TFT_eSprite::pushImage clips only the right and bottom edges — a negative x or y walks
   * off the start of the buffer. Marker shapes near the edge of the map are exactly where
   * those coordinates come from, so nothing here is allowed to pass raw coordinates to the
   * display; it goes through the clip. */
  void  fillClipped(int x, int y, int w, int h, uint16_t colour);
  void  drawBlob(int x, int y, int r, uint16_t colour, bool diamond);
  void  reserveBox(int x, int y, int w, int h);   // this space is taken; keep labels off it
  void  drawMap();
  void  drawNoMapPage();
  void  drawOverlays();
  void  drawMarker(int vx, int vy, uint16_t colour, int shape);
  /* Place a label beside a marker, or decline to. Returns whether it was drawn.
   *
   * ⚠ LABELS ARE DRAWN IN A SECOND PASS, IN PRIORITY ORDER, AND A LABEL THAT WOULD LAND ON
   * ONE ALREADY PLACED IS DROPPED. On 240x250 with eight places and twenty nodes in view,
   * unmanaged labels overprint into a grey smear and the screen stops answering the question
   * it exists to answer. Dropping one leaves its MARKER, which is the part that says where
   * somebody is; the name is on the Nodes list either way. Priority runs: this phone, your
   * pins, the mesh's places, then other people, nearest the crosshair first. */
  bool  drawLabel(int vx, int vy, const char* text, uint16_t colour);
  void  resetLabels() { labelCount = 0; }

  struct LabelRect {
    int16_t x, y, w, h;
  };
  /* Space already taken on the map this frame: every MARKER as well as every label. Markers
   * go in during pass 1 so a name cannot be dropped on top of the dot it is naming, which is
   * the one thing a label must never cover. 32 boxes is four labels more than a 240x250
   * screen can legibly hold; once it is full, no further labels are placed at all — a label
   * drawn without a collision test is exactly the overprinting this exists to stop. */
  LabelRect labelRects[32];
  int       labelCount;
  void  drawCrosshair();
  void  drawBottomStrip();
  void  drawChips();

  // ---- actions ----
  /* Where this phone thinks it is, and how sure. One answer, so the marker, its label and the
   * `0` key can never disagree about which of the three they are showing.
   * ⚠ MESH_GPS_FRESH_MS is the bar, and it is the SERVICE'S bar, not a second one invented
   * here (meshService.getSelfFix, self_pos.h's selfPick): a fix from hours ago drawn as a live
   * one is a lie about where YOU are. `src` (optional): SELF_GPS, SELF_PHONE or SELF_NET for a
   * live kind, SELF_NONE for the pin or nothing. `accM` (optional): a Wi-Fi fix's accuracy (the
   * ring the map draws round it, and its label), 0 for every other kind. */
  int   selfPosition(int32_t* latI, int32_t* lonI, uint32_t* ageMs, int* src = NULL,
                     uint16_t* accM = NULL) const;
  /* A dotted ring of radius r px round (x, y), clipped to the map: an APPROXIMATE position (a
   * node that sent precision_bits, mesh_pos.h) is a circle, not a dot. */
  void  drawRing(int x, int y, int r, uint16_t colour);
  void  centreOnMe();
  void  centreOn(double lat, double lon);
  int   pinUnderCrosshair();
  bool  panOnce(int dx, int dy, int step, bool discrete);   // one pan or snap; true = it SNAPPED to a marker
  /* Snap: the markers the arrows stop on — pins, mesh places, other nodes (SNAP_* in the .cpp). */
  int   snapCount(int kind);
  bool  snapMarker(int kind, int idx, int* vx, int* vy);
  bool  snapNear(int radius, MapsSnapHit* out);   // the nearest marker within radius px of the crosshair
  bool  snapLanding();                            // snap if the crosshair has landed near one
  void  snapTo(const MapsSnapHit& h);
  void  armTimer();
  bool  dropPin();
  bool  sharePin(int idx, char* why, size_t whyCap);
  /* Returns whether a SHARED pin's retraction was QUEUED (review M1: queued, not on the air —
   * meshOp then reports whether it left, and a retraction that never does puts the pin back).
   * The local delete happens either way — a person who confirmed a delete has confirmed it —
   * but the caller must be able to say that other people's maps still have it. */
  bool  deletePin(int idx);
  void  beginMeshOp(uint8_t op, uint32_t txId, int idx);
  bool  settleMeshOp(bool final);   // true = the note or a pin changed (redraw)
  /* Is pin idx's retraction still waiting for the radio (mapPinRetractionQueued)? Then nothing
   * may re-share it, and nothing may settle that frame early (review M1 follow-up). */
  bool  retractionQueued(int idx) const;
  int   pinByWaypoint(uint32_t wpId) const;   // -1 when no pin carries it
  void  stepPin(int delta);
  void  scanAreaExtent(MapArea* a);   // scanAreas: the tiles' bounding box at zMin
  void  setArea(int idx);
  void  setNote(const char* fmt, ...);

  // ---- screens ----
  void  freeMenu();
  void  freeWidgets();
  void  enterState(MapsState_t st);
  MenuWidget* newMenu(const char* emptyMessage);
  void  buildMenu();
  void  buildList();
  void  buildPinOpts();
  void  buildRename();
  void  buildAreas();
  void  buildConfirmDelete();
  void  drawHelp();
  void  drawHelpDiagram();  // page -1 of the help: the phone's buttons, drawn
  void  buildDownload();
  void  rescanCard();       // re-read the areas and pins without moving the view
  void  drawGoto();
  void  buildChannels();
  /* The channel a pin is shared on, by the NAME stored with it: NULL when it has none or
   * the channel is no longer on this phone (the picker is offered then). */
  const MeshChannel* pinChannel(int idx) const;
  friend bool mapsSaveOpenView();
  friend bool mapsConsoleStatus(char* out, size_t cap);   // serial `maps`: the areas' boxes
  friend bool mapsConsoleElev(char* out, size_t cap);     // serial `elev`: the cached samples

  appEventResult onMapKey(EventType event);
};

#endif // APP_MAPS_H
