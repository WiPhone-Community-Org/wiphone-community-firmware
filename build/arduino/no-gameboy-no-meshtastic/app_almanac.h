/*
 * app_almanac.h - Menu > Almanac: sun, moon, solunar, position and date, offline
 * (docs/almanac.md, "On the phone"; README "Almanac").
 *
 * Seven screens, all MenuWidget lists built from almanac_lines.cpp - the same lines the serial
 * `almanac` prints: TODAY (the first), SUN, MOON, SOLUNAR, POSITION, DATE, SETTINGS - and an
 * eighth since 0.9.81, WEATHER (weather_lines.cpp's rows, the same ones serial `wx show` prints).
 * Keys: Up/Down scroll; Left/Right step the day (-365..+365) on TODAY/SUN/MOON/SOLUNAR; OK
 * opens an entry or toggles a setting (and on SUN/MOON/SOLUNAR the left soft key goes back to
 * today); Back goes up, then exits.
 *
 * ── WHAT COSTS WHAT, AND WHERE IT RUNS ──────────────────────────────────────────────────────
 * The astronomy is software double on the ESP32 (~26.5 us a libm call with its arithmetic at
 * 160 MHz, the clock whenever WiFi is on - the app cannot raise it). MEASURED on phone 2,
 * 2026-09-27, before this design: the core day (sun 1,843 + moon 3,668 + solunar 91 libm calls)
 * took 148,485 us in ONE pass, the TODAY build on top of it 26 ms more (all of it
 * astroMoonPhaseAt, 956 calls, 740 of them the search for the last new moon - for an age TODAY
 * does not show): `almanac cost` said 174 ms, and one pass logged a 334 ms LOOP STALL.
 *   - The day's tables are computed IN SLICES on the app timer (almanac_lines.h, almDayWorkRun):
 *     a slice works until ALM_SLICE_US (25 ms) has gone, a unit at a time (a node or a 5-minute
 *     sample of a sun or moon day, <= ~5 ms; a quarter's or a new moon's search ~20 ms, only
 *     ever as a slice's FIRST unit), then gives the loop back for ALM_SLICE_GAP_MS - counted
 *     from the slice's END (review 2026-09-27: WiPhone.ino re-arms the app timer from the pass's
 *     START, so a 30 ms slice with a 15 ms period ran again on the very next pass; the period is
 *     now the slice's own length + the gap). Estimated worst slice ~25 + 5 ms.
 *     ONE THING A PASS: a slice that lands a part does not rebuild or repaint - the NEXT timer
 *     pass does that and runs no slice (~6-12 ms of rows + the menu drawn + the band pushed,
 *     ~48 ms estimated: drewInsideBand, the full-screen push is ~60). Before, a landing pass was
 *     slice + rebuild + full push, ~100-120 ms estimated, 3-5 times a day computed.
 *     The core is ~6 slices, ~0.4 s from a key press to whole; the screen shows each part's rows
 *     as it lands, "Computing..." until then.
 *   - Kept per (shown day, the day's offset, the place WHERE IT WAS KEYED to within almSamePlace's
 *     0.001 deg ~ 100 m - a tolerance, not a grid: GPS noise across a grid line re-keyed the day)
 *     in two slots, TODAY's and the stepped day's, so stepping away and back does not recompute.
 *   - Key handlers compute NO table and do no search or scan: Left/Right and OK only (re)key a
 *     slot and build from what is there; onto a day not computed yet, no astronomy at all.
 *   - The minute tick (TIME_UPDATE_EVENT) rebuilds the open screen from the caches: the
 *     countdown, "Sun now" (~27 libm calls), "Moon now" (~215), the moon's phase at now without a
 *     search (~215; MOON's age from the cached new moons, AlmMoonAge). Estimated <= ~12 ms.
 *   - POSITION is the only screen with a 1 Hz timer; it reads the elevation card (elevSampleCard)
 *     ON THAT TIMER only, never in a key handler, and only when the place has moved ~10 m.
 *   - A rebuild keeps the list where it was scrolled (almMenuTop) and the highlight on the row
 *     with the same words, else the same position.
 * Every slice, every build, every paint and each part of the day is timed with micros() into
 * gAlmanacCost: `almanac cost` prints them; `almanac bench` times each piece now, in one pass.
 *
 * ── THE WEATHER (0.9.81; weather_net.h has the fetch, weather_lines.h the words) ─────────────
 *   - The cache is read from the card on the app timer's FIRST pass (once a boot - later opens
 *     find it in memory), never in a key handler or the constructor.
 *   - A fetch is decided on the timer too, and only once the open screen's day has landed (its
 *     ~6 slices share core 1 with the TLS worker): on opening the Almanac or WEATHER (wxFetchDue's
 *     automatic rule: WiFi, stale or far, no music, not within 10 min of a failure) or on the
 *     Refresh soft key (WEATHER's OK). The KEY only sets wxWant; the timer pass that follows reads
 *     the ground under the place (the elevation tiles: card I/O) and queues the job - no SD, HTTP
 *     or TLS call in a key handler, a build or an entry (tests/check_almanac_review.py).
 *   - While a fetch is out the timer polls weather_net's generation every ALM_WX_POLL_MS; a new
 *     one is a `landed` part like any other (ONE THING A PASS: the next pass rebuilds).
 *   - Alert rows carry their colour (ALM_ROW_WARN yellow, ALM_ROW_DANGER red) on every wrapped row.
 */
#ifndef APP_ALMANAC_H
#define APP_ALMANAC_H

#include "GUI.h"
#include "almanac_lines.h"
#include "weather_lines.h"     // WxView: the weather the screens are shown

/* What the Almanac measured on this phone (serial `almanac cost`). Microseconds; 0 = never. */
struct AlmanacCost {
  uint32_t slices;          // slices run since boot
  uint32_t sliceMaxUs;      // the longest slice since boot...
  uint32_t sliceMaxParts;   // ...and the parts it worked on (ALM_* bits)
  uint32_t dayCpuUs;        // the last day whose core finished: the CPU its slices took,
  uint32_t daySlices;       // ...in how many slices,
  uint32_t dayMaxUs;        // ...the longest of them,
  uint32_t dayWallMs;       // ...and how long after it was begun it was whole
  uint32_t partUs[7];       // the last whole run of each part, by ALM_* bit: day before, day after,
                            // quarters, sun, moon, solunar, moon age
  uint32_t seasonsUs;       // DATE's four seasons (one unit)
  uint32_t buildUs;         // the last screen build: rows + menu, no tables, "now" rows included
  uint32_t rowsUs;          // ...of which the row builder
  uint32_t tickBuildUs;     // the last minute-tick rebuild
  uint32_t buildMaxUs;      // the longest build since boot
  uint32_t paintUs;         // the last paint: the menu drawn into the sprite (the band's push to
  uint32_t paintMaxUs;      // ...the glass follows it, ~48 ms estimated) - and the longest
  uint32_t cpuMhz;          // the CPU clock at the last slice
  uint32_t days;            // days (re)begun since boot
  uint32_t atMs;            // millis() of the last slice
  uint32_t serialUs;        // the last serial `almanac` run, end to end (one pass)
  uint32_t serialMhz;
};
extern AlmanacCost gAlmanacCost;

/* The serial `almanac [lat,lon] [+N|-N]` (every line the app shows for that day, then the exact
 * instants as UNIX seconds for the bench's comparison with PyEphem), `almanac cost` (what the app
 * measured) and `almanac bench` (each piece timed now). `almanac` and `almanac bench` are ONE
 * pass each - bench commands, ~0.4 s at 160 MHz, and they print their own time; the app never
 * works that way. `out` is called once per line, each under 190 bytes. `almanac` reads the
 * elevation card once (a console command, never a key handler). */
void almanacConsole(const char* args, void (*out)(const char* line));

/* Today's legal light at one place - the ONE computation behind the Meshtastic "Sun & legal
 * light" screen, the serial `sun` and the Almanac (astro.cpp + the gLegalRule setting; the old
 * sun_times.cpp is gone). ~1,500 libm calls, ~3,000 once today's light has ended (tomorrow's
 * first light for "Dark - first light 06:35"). */
enum { ALM_SUN_OK = 0, ALM_SUN_NO_CLOCK, ALM_SUN_BAD_PLACE, ALM_SUN_BAD_DATE };
struct AlmanacLegal {
  int64_t     now;             // UTC
  int         tzS;             // local - UTC
  int         rule;            // ASTRO_LEGAL_30MIN / ASTRO_LEGAL_CIVIL (from gLegalRule)
  int         y, m, d;         // the local date
  AstroSunDay sun;             // today's events (0 = none)
  int64_t     first, last;     // today's legal light by the rule (0 = none)
  char        countdown[48];   // "LEGAL LIGHT: 3h 12m left" / "First light in 5h 02m" / "Dark - ..."
};
int almanacLegalToday(double lat, double lon, AlmanacLegal* out);   // ALM_SUN_*

/* The clock's UTC offset now, in seconds (two clock reads, rounded to the minute). */
int almanacTzOffsetS();

/* (0.9.81) Serial `wx`, `wx fetch`, `wx show`, `wx clear` (weather_net.h; `wx show` prints the
 * WEATHER screen's rows through the same builder). `out` once per line, under 190 bytes. */
void almanacWeatherConsole(const char* args, void (*out)(const char* line));
/* Serial `open weather`: the next Almanac opens on WEATHER (Back still goes to TODAY); false undoes it. */
void almanacOpenAtWeather(bool on);

class AlmanacApp : public WindowedApp {
public:
  AlmanacApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~AlmanacApp();

  ActionID_t getId() {
    return GUI_APP_ALMANAC;
  };
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll = false);
  /* Every Almanac paint is its menu, between the header and the footer: GUI pushes only that
   * band (~4/5 of a 240x320 push; the full one is ~60 ms). The header and footer go on their
   * own when they were redrawn (REDRAW_HEADER / REDRAW_ALL). */
  bool drewInsideBand() {
    return true;
  }
  /* The AlmEmitFn every builder writes through (ctx = this app): one row into the open menu.
   * Public for the wrap callback, which feeds the pieces of a long sentence back through it. */
  static void emitRow(void* ctx, int kind, const char* text);
  /* One piece of a wrapped sentence, in `style` (ALM_STYLE_*: plain, an alert's yellow or red). */
  static void wrapPiece(void* ctx, int style, const char* text);

  /* One shown day: its tables, the slices computing them, and the key they are for. */
  struct Slot {
    AlmDay      day;
    AlmDayWork  work;
    bool        keyed;
    int64_t     t0;
    int         tz;
    double      lat, lon;          // the place it was keyed for: kept while almSamePlace (~100 m)
    uint32_t    begunMs;           // when this key was begun
    uint32_t    cpuUs, slices, maxUs;   // what its slices have cost so far
    bool        coreDone;          // the core's cost has been recorded
    uint32_t    partAccUs[7];      // CPU per part so far (ALM_* bit order)
  };

protected:
  int         screen;              // ALM_SCREEN_*
  int         dayOffset;           // -ALM_DAY_OFFSET_MAX..+ALM_DAY_OFFSET_MAX
  MenuWidget* menu;
  /* HeaderWidget::setTitle KEEPS the pointer: the title lives here, never on a stack. */
  char        title[24];

  // The place, as almPickPlace gave it at the last build.
  int         placeKind;           // ALM_PLACE_*
  char        placeName[24];
  int32_t     placeLatI, placeLonI;
  uint32_t    placeAgeMs;          // ALM_PLACE_LAST_GPS: the fix's age
  bool        viewOk;              // the Maps app's saved view (NVS, read once when this opens)
  int32_t     viewLatI, viewLonI;

  // TODAY's day and the stepped one (PSRAM, with the app); the moon's age for MOON.
  Slot        slotToday, slotOther;
  AlmMoonAge  moonAge;

  // DATE's next seasons, for one local date.
  int64_t     seasons[4];
  int64_t     seasonsDay;          // the local day number they were computed on; -1 = none

  // POSITION's ground height, read on the timer.
  int         elevState;           // ALM_ELEV_*
  double      elevM;
  int         elevZ;
  int32_t     elevLatI, elevLonI;  // where it was read
  uint32_t    elevAtMs;            // when (a failed read is retried after a while)

  int         infoKey;             // the next key for a display row in the build under way
  uint32_t    rowsHash;            // FNV-1a of the rows the build under way emitted...
  uint32_t    shownHash;           // ...and of the ones on the glass: a timer rebuild that changed
                                   // nothing asks for no repaint
  bool        owed;                // the open screen still has slices to run (the last build said)
  bool        landed;              // a slice finished a part: the NEXT timer pass rebuilds and
                                   // repaints, and runs no slice (review 2026-09-27, finding 3)

  // The weather (0.9.81).
  WxView      wxView;              // what the builders are shown (filled by fillCtx)
  bool        wxLoaded;            // the timer has asked weather_net for the card's cache (this open)
  int         wxLoadTries;         // ...passes that asked and got no answer from the card
  int         wxWant;              // ALM_WX_*: a fetch to decide on the timer (none, automatic, Refresh)
  uint32_t    wxShownGen;          // weather_net's generation the rows were built from
  char        wxNote[96];          // why the last Refresh did not start ("" = none)

  void        freeMenu();
  MenuWidget* newMenu();
  void        enter(int newScreen, MenuOption::keyType selectKey);
  void        build();                          // the open screen, from the caches: no tables
  bool        rebuildKeepingSelection();         // true = the rows changed
  void        setHeader();
  bool        refreshPlace();                   // false = no place
  void        fillCtx(AlmCtx* c);
  Slot*       bindDay(AlmCtx* c);               // the shown day's slot, keyed (no maths)
  bool        owedFor(const AlmCtx* c, const Slot* s);
  void        armTimer(uint32_t workedMs = 0);  // slices while owed; POSITION's 1 Hz; else off
  appEventResult slice();                       // APP TIMER: one slice, OR the rebuild after one
  void        sampleElevation();                // APP TIMER only (card I/O)
  bool        wxStep(const AlmCtx* c);          // APP TIMER only: decide / queue a fetch, or see one land
  bool        wxTimerWanted();                  // the weather needs the timer (load, a decision, a poll)
  void        addRow(int kind, const char* text);
  static bool isDayScreen(int s) {
    return s == ALM_SCREEN_TODAY || s == ALM_SCREEN_SUN || s == ALM_SCREEN_MOON ||
           s == ALM_SCREEN_SOLUNAR;
  }
};

#endif // APP_ALMANAC_H
