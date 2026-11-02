/*
 * app_almanac.cpp - Menu > Almanac. See app_almanac.h for the screens, the keys and what costs
 * what; every row's WORDING is almanac_lines.cpp's (host-tested and measured against the font).
 */

#include "app_almanac.h"
#include "almanac_lines.h"
#include "astro.h"
#include "clock.h"              // ntpClock
#include "clock_source.h"       // clockSourceName, CLOCK_SRC_MESH
#include "cpu_clock.h"          // cpuClockRaise: the slices are CPU-bound software double
#include "elev_sd.h"            // elevSampleCard / elevCardPresent (card I/O: the app timer only)
#include "elev_tiles.h"
#include "geo_grid.h"           // `almanac bench`
#include "menu_wrap.h"          // wrapNote: a long sentence broken into rows that fit
#include "meshtastic_service.h" // resolveReference, the GPS fix and its motion
#include "prefs_almanac.h"      // gUnits, gLegalRule, gUsDst and their setters
#include "tz_service.h"         // tzSvcZoneRules: a zone with rules decides DST (0.9.82)
#include "units.h"
#include "weather_lines.h"      // the WEATHER screen's rows, TODAY's two weather rows
#include "weather_net.h"        // the cache, the fetch (the timer only), `wx`
#include "wmm.h"                // `almanac bench`

#include <Preferences.h>        // the Maps app's saved view (A2), read once when the app opens
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

AlmanacCost gAlmanacCost;

/* Row keys. The entries and settings carry almanac_lines.h's kinds (101..113) as their keys;
 * every display row gets a key of its own, counted from ROW_INFO_BASE in the order it was
 * built, so a rebuild (the minute tick, the 1 Hz Position refresh) puts the highlight back on
 * the same row. (addNote's shared MENU_ROW_NOTE would send it to the first note instead.) */
#define ROW_HINT        900u
#define ROW_INFO_BASE   1000u

/* The Almanac's face: AKROBAT_BOLD_20 and as many rows as fit (N_MAX_ITEMS: 11 at 22 px on
 * the 250 px between header and footer) - the face the Maps app's menus use. The Meshtastic
 * lists' EXTRABOLD_22 at five rows a screen would put SUN on three pages; every row this app
 * draws is measured to fit 232 px of this face (tests/test_almanac_lines.cpp). */
#define ALM_FONT        AKROBAT_BOLD_20
#define ALM_WRAP_W      220            // addNoteWrapped's width: 240 - leftOffset 8 - 12

/* The weather (0.9.81). wxWant: what the next timer pass decides. The poll: while a fetch is out,
 * the timer looks for weather_net's new generation this often (the AI app's AI_POLL_MS). */
#define ALM_WX_NONE        0
#define ALM_WX_AUTO        1          // the Almanac or WEATHER opened: wxFetchDue's automatic rule
#define ALM_WX_REFRESH     2          // the Refresh soft key: an explicit ask
#define ALM_WX_POLL_MS     250
#define ALM_WX_LOAD_TRIES  4          // timer passes that ask the card for the cache before giving up
/* Row styles: a plain row, and an alert's two colours (MenuWidget style 2 is the yellow one; the
 * red one is AlmRedOption, below - MenuWidget has two styles and an alert needs three looks). */
#define ALM_STYLE_PLAIN    1
#define ALM_STYLE_WARN     2
#define ALM_STYLE_DANGER   3

#define ALM_ELEV_MOVE_E7   1000        // re-read the ground after ~11 m of movement (1e-4 deg)
#define ALM_ELEV_RETRY_MS  30000u      // ...or 30 s after a card error / no layer

/* A1 (phone 2, 2026-09-27, 160 MHz with WiFi on): the core day in ONE pass was 148,485 us and a
 * pass hit 334 ms (LOOP STALL; > 250 ms drops WiFi and eats keys). The day now comes in slices:
 * a slice runs units (<= ~5 ms each; a search ~20 ms, only ever first) until ALM_SLICE_US has
 * gone, then the loop gets ALM_SLICE_GAP_MS before the next. Worst slice ~ALM_SLICE_US + one
 * moon node (~5 ms) = ~30 ms, a search-led one ~20 + ~25 ms; `almanac cost` shows the real one. */
#define ALM_SLICE_US       25000u
#define ALM_SLICE_GAP_MS   15
/* A landing pass's paint happens after processEvent returns (GUI::redrawScreen: the menu drawn,
 * then the band pushed, ~48 ms estimated from the ~60 ms full push), where the app cannot time
 * it: the next pass's gap counts it in by this estimate. */
#define ALM_PAINT_EST_MS   50

// ---- shared helpers (the app and the serial `almanac`) ------------------------------------------

/* The clock's time zone when it has RULES (tz_service.h, 0.9.82): a named zone, or the zone Auto
 * found from a measured fix. The Almanac then gives every DST answer by the zone's rule, exact to
 * the second, and says the change happens by itself (almanac_lines.h, AlmCtx.tzZoned). A typed
 * offset leaves tzZoned false: the US guess on the bare offset, and Settings' switch, as before. */
static void almCtxZone(AlmCtx* c) {
  int stdMin = 0;
  uint8_t rule = TZ_RULE_NONE;
  const char* name = NULL;
  int64_t since = 0;
  c->tzZoned = tzSvcZoneRules(&stdMin, &rule, &name, &since);
  if (c->tzZoned) {
    c->tzStdS = stdMin * 60;
    c->tzRule = rule;
    c->tzName = name;
    c->tzSince = since;
  }
}

/* The UTC offset in seconds. Two clock reads; a second can tick between them, so the difference
 * is rounded to the minute (every real offset is whole minutes). */
int almanacTzOffsetS() {
  const int32_t d = (int32_t)(ntpClock.getExactUnixTime() - ntpClock.getExactUtcTime());
  const int32_t m = (d >= 0) ? (d + 30) / 60 : -((-d + 30) / 60);
  return (int)(m * 60);
}

/* The Maps app's saved view (A2's third place): the keys and types MapsApp::saveView writes
 * (app_maps.cpp: namespace "maps", "saved" = 1, "lat"/"lon" int 1e-7 deg) and restoreView reads.
 * almPickPlace refuses 0,0 and anything off the globe. One NVS read (flash, not the card). */
static bool almReadMapView(int32_t* la, int32_t* lo) {
  bool ok = false;
  Preferences p;
  if (p.begin("maps", true)) {
    if (p.getInt("saved", 0) == 1) {
      *la = p.getInt("lat", 0);
      *lo = p.getInt("lon", 0);
      ok = true;
    }
    p.end();
  }
  return ok;
}

/* Which place (almanac_lines.h, almPickPlace): resolveReference() and which of its answers it
 * was (its own MESH_REFK_* — the display name is not compared any more: "phone" would have read
 * as the pin); else the newest USABLE fix of this boot, however old (this phone's GPS or the
 * hotspot phone's, each by its own sats/HDOP); else the map's saved view. */
static void almPlaceNow(bool viewOk, int32_t viewLatI, int32_t viewLonI, AlmPlace* out) {
  AlmPlaceIn in;
  memset(&in, 0, sizeof(in));
  char name[24];
  name[0] = '\0';
  uint8_t kind = MESH_REFK_NONE;
  in.refOk = meshService.resolveReferenceEx(&in.refLatI, &in.refLonI, name, sizeof(name), &kind);
  if (in.refOk) {
    switch (kind) {
      case MESH_REFK_GPS:        in.refKind = ALM_PLACE_GPS; break;
      case MESH_REFK_LAST_GPS:   in.refKind = ALM_PLACE_LAST_GPS; break;
      case MESH_REFK_PHONE:      in.refKind = ALM_PLACE_PHONE; break;
      case MESH_REFK_LAST_PHONE: in.refKind = ALM_PLACE_LAST_PHONE; break;
      case MESH_REFK_WAYPOINT:   in.refKind = ALM_PLACE_WAYPOINT; break;
      case MESH_REFK_NET:        in.refKind = ALM_PLACE_NET; break;    // "Wi-Fi +/-60 m"
      default:                   in.refKind = ALM_PLACE_PIN; break;
    }
    in.refName = name;
    SelfFix ph;
    if (kind == MESH_REFK_NET && meshService.getSourceFix(SELF_NET, &ph)) {
      in.refAgeMs = ph.ageMs;    // since its last confirming scan (self_pos.h)
    }
    if (kind == MESH_REFK_PHONE || kind == MESH_REFK_LAST_PHONE) {
      /* "phone" is right on a distance line ("1.4km NE of phone"); as a PLACE ("Weather ... for
       * phone") it needs its noun. */
      in.refName = kind == MESH_REFK_PHONE ? "phone GPS" : "last phone GPS";
      if (meshService.getSourceFix(SELF_PHONE, &ph)) {
        in.refAgeMs = ph.ageMs;
      }
    }
  }
  uint32_t age = 0;
  int sats = -1, hdop = -1;
  in.gpsFix = meshService.getGpsFix(&in.gpsLatI, &in.gpsLonI, &age, &sats, &hdop);
  in.gpsAgeMs = age;              // the receiver's own: a GPS / LAST_GPS place is aged by it
  in.gpsSats = sats;              // almPickPlace refuses a poor fix (meshPosFixUsable) - by THE FIX'S
  in.gpsHdopX10 = hdop;           // own rating (getGpsFix), never the sky's last word (getGpsSky)
  /* Step 2's other candidate, apart from the receiver's (review 2026-10-10: it used to OVERWRITE
   * the GPS fields when newer, so a fresh poor phone fix pushed a usable GPS fix out, and a
   * LAST_GPS reference was aged by the phone). almPickPlace takes the newer USABLE one. */
  SelfFix ph;
  in.phoneFix = meshService.getSourceFix(SELF_PHONE, &ph);
  if (in.phoneFix) {
    in.phoneLatI = ph.latI;
    in.phoneLonI = ph.lonI;
    in.phoneAgeMs = ph.ageMs;
    in.phoneSats = ph.sats;
    in.phoneHdopX10 = ph.hdopX10;
  }
  in.viewOk = viewOk;
  in.viewLatI = viewLatI;
  in.viewLonI = viewLonI;
  almPickPlace(&in, out);
}

static void almFillPos(AlmPos* p, int elevState, double elevM, int elevZ) {
  memset(p, 0, sizeof(*p));
  p->gpsOn = meshService.isGpsEnabled();
  int sats = -1, hdop = -1;
  meshService.getGpsSky(&sats, &hdop);     // the sky now ("Sats 7, HDOP 1.2"), not the fix's rating
  p->sats = sats;
  p->hdopX10 = hdop;
  int alt = -10000;
  int32_t spd = -1, crs = -1;
  uint32_t age = 0, altAge = UINT32_MAX, motionAge = UINT32_MAX;
  p->gpsHaveFix = meshService.getGpsMotion(&alt, &altAge, &spd, &crs, &motionAge, &age);
  p->gpsAgeMs = age;
  p->altM = alt;
  p->altAgeMs = altAge;
  p->speedKnX100 = spd;
  p->courseX10 = crs;
  p->motionAgeMs = motionAge;
  p->elevState = elevState;
  p->elevM = elevM;
  p->elevZ = elevZ;
}

// One elevation read (card I/O) as an ALM_ELEV_* state.
static int almReadElevation(double lat, double lon, double* m, int* z) {
  if (!elevCardPresent()) {
    return ALM_ELEV_NOLAYER;
  }
  bool ioErr = false;
  int zz = -1;
  const int rc = elevSampleCard(lat, lon, m, &zz, &ioErr);
  *z = zz;
  if (rc == 1) {
    return ALM_ELEV_OK;
  }
  if (ioErr) {
    return ALM_ELEV_IOERR;
  }
  return rc < 0 ? ALM_ELEV_NODATA : ALM_ELEV_NOTILE;
}

/* The last day's sun kept for almanacLegalToday (the Meshtastic Sun screen and serial `sun` open
 * it from a key handler): astroSunDay is ~49 ms at 160 MHz, twice after dark. Keyed on the local
 * midnight and the place to within almSamePlace (0.001 deg, ~100 m: sunrise ~0.4 s) - NOT exact
 * doubles (review 2026-09-27: a live GPS fix moves 1e-7 deg every second, so an exact key never
 * hit, and the Almanac's seed, from its slot's place, never matched either). The Almanac seeds it
 * from its own slices (almanacLegalSeed) when TODAY's sun lands. ~120 bytes in PSRAM, allocated on
 * first use (internal RAM is what panics this phone); no PSRAM = no cache. */
struct AlmLegalCache {
  bool        ok;
  int64_t     t0;
  double      lat, lon;
  AstroSunDay sun;
  bool        haveNext;
  AstroSunDay next;
};
static AlmLegalCache* s_legal = NULL;

static AlmLegalCache* almLegalCache() {
  if (!s_legal) {
    s_legal = (AlmLegalCache*)heap_caps_calloc(1, sizeof(AlmLegalCache),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  return s_legal;
}

static bool almLegalCached(int64_t t0, double lat, double lon) {
  return s_legal && s_legal->ok && s_legal->t0 == t0 &&
         almSamePlace(s_legal->lat, s_legal->lon, lat, lon);
}

static void almanacLegalSeed(int64_t t0, double lat, double lon, const AstroSunDay* sun,
                             const AstroSunDay* next) {
  AlmLegalCache* k = almLegalCache();
  if (!k) {
    return;
  }
  if (!almLegalCached(t0, lat, lon)) {
    if (!sun) {
      return;                                     // a day after with no day to belong to
    }
    memset(k, 0, sizeof(*k));
    k->t0 = t0;
    k->lat = lat;
    k->lon = lon;
  }
  if (sun) {
    k->sun = *sun;
    k->ok = true;
  }
  if (next) {
    k->next = *next;
    k->haveNext = true;
  }
}

int almanacLegalToday(double lat, double lon, AlmanacLegal* out) {
  memset(out, 0, sizeof(*out));
  if (!ntpClock.isTimeKnown()) {
    return ALM_SUN_NO_CLOCK;
  }
  /* The old sun_times.cpp refused |lat| > 89.9 and |lon| > 180, and dates outside 1970..2099.
   * astro.cpp computes anywhere and any time, so the refusals are kept here, explicitly
   * (almanac_lines.h: almPlaceOk, almDateOf - both host-tested). */
  if (!almPlaceOk(lat, lon)) {
    return ALM_SUN_BAD_PLACE;
  }
  out->now = (int64_t)ntpClock.getExactUtcTime();
  out->tzS = almanacTzOffsetS();
  out->rule = (gLegalRule == LEGAL_RULE_CIVIL) ? ASTRO_LEGAL_CIVIL : ASTRO_LEGAL_30MIN;
  const int64_t t0 = almMidnight(out->now, out->tzS, 0);
  if (!almDateOf(t0, out->tzS, &out->y, &out->m, &out->d, NULL, NULL)) {
    return ALM_SUN_BAD_DATE;
  }
  if (almLegalCached(t0, lat, lon)) {
    out->sun = s_legal->sun;
  } else {
    astroSunDay(t0, lat, lon, &out->sun);
    almanacLegalSeed(t0, lat, lon, &out->sun, NULL);
  }
  astroLegalLight(&out->sun, out->rule, &out->first, &out->last);
  /* The countdown is almanac_lines' almCountdownDay - the SAME function the Almanac's TODAY
   * calls (review 2026-09-27: it was worked out twice, and only one copy was host-tested).
   * Tomorrow's sun only once today's legal light has ended: it is ~1,850 more libm calls. */
  AstroSunDay next;
  const bool needNext = almCountdownNeedsNext(out->now, out->first, out->last);
  if (needNext) {
    if (almLegalCached(t0, lat, lon) && s_legal->haveNext) {
      next = s_legal->next;
    } else {
      astroSunDay(t0 + 86400, lat, lon, &next);
      almanacLegalSeed(t0, lat, lon, NULL, &next);
    }
  }
  almCountdownDay(out->now, out->tzS, out->rule, t0, lat, lon, &out->sun, needNext ? &next : NULL,
                  out->countdown, sizeof(out->countdown));
  return ALM_SUN_OK;
}

// ---- the app ------------------------------------------------------------------------------------

static bool s_openAtWeather = false;     // serial `open weather`: the next open lands on WEATHER

void almanacOpenAtWeather(bool on) {
  s_openAtWeather = on;
}

AlmanacApp::AlmanacApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(disp, state, header, footer) {
  log_d("create AlmanacApp");
  /* ⚠ SET WHAT THIS SCREEN NEEDS: msAppTimerEventPeriod is device-wide and the last app may
   * have left its own value in it. The slices and POSITION run the timer (armTimer). */
  controlState.msAppTimerEventPeriod = 0;
  screen = ALM_SCREEN_TODAY;
  dayOffset = 0;
  menu = NULL;
  title[0] = '\0';
  placeKind = ALM_PLACE_NONE;
  placeName[0] = '\0';
  placeLatI = placeLonI = 0;
  placeAgeMs = 0;
  viewLatI = viewLonI = 0;
  viewOk = almReadMapView(&viewLatI, &viewLonI);   // NVS, once: only the Maps app writes it
  memset(&slotToday, 0, sizeof(slotToday));
  memset(&slotOther, 0, sizeof(slotOther));
  memset(&moonAge, 0, sizeof(moonAge));
  memset(seasons, 0, sizeof(seasons));
  seasonsDay = -1;
  elevState = ALM_ELEV_PENDING;
  elevM = 0;
  elevZ = -1;
  elevLatI = elevLonI = 0;
  elevAtMs = 0;
  infoKey = ROW_INFO_BASE;
  rowsHash = shownHash = 0;
  owed = false;
  landed = false;
  memset(&wxView, 0, sizeof(wxView));
  wxLoaded = false;                 // the card's cache: the timer's first pass, never here
  wxLoadTries = 0;
  wxWant = ALM_WX_AUTO;             // opening the Almanac is the automatic fetch's moment
  wxShownGen = 0;
  wxNote[0] = '\0';
  if (s_openAtWeather) {
    s_openAtWeather = false;        // serial `open weather`
    enter(ALM_SCREEN_WEATHER, 0);
  } else {
    enter(ALM_SCREEN_TODAY, ALM_ENTRY_SUN);
  }
}

AlmanacApp::~AlmanacApp() {
  log_d("destroy AlmanacApp");
  freeMenu();
  controlState.msAppTimerEventPeriod = 0;    // do not leave the CPU waking for a closed app
}

void AlmanacApp::freeMenu() {
  if (menu) {
    delete menu;
    menu = NULL;
  }
}

/* The Almanac's list: a MenuWidget that says, and is told, where it is scrolled - every rebuild
 * is a NEW widget, which starts at the top (review 2026-09-27: a list scrolled down and partly
 * back jumped on every minute tick, and every second on POSITION). Widgets live in PSRAM
 * (AbstractWidget's operator new), this one too. */
class AlmMenu : public MenuWidget {
public:
  AlmMenu(uint16_t x, uint16_t y, uint16_t w, uint16_t h, SmoothFont* f, uint8_t perScreen,
          uint16_t left)
    : MenuWidget(x, y, w, h, NULL, f, perScreen, left) {}
  int selIndex() const {
    return optionSelectedIndex;
  }
  int topIndex() const {
    return optionOffsetIndex;
  }
  int perScreen() const {
    return optionsVisible;
  }
  int rows() {
    return (int)options.size();
  }
  /* Select the row with key >= minKey whose words are `text` - the one nearest `near` when
   * there are several (two "Computing..." rows). */
  bool selectText(const char* text, MenuOption::keyType minKey, int near) {
    int best = -1;
    for (int i = 0; text && i < (int)options.size(); i++) {
      if (options[i]->id >= minKey && options[i]->titleDyn && !strcmp(options[i]->titleDyn, text) &&
          (best < 0 || abs(i - near) < abs(best - near))) {
        best = i;
      }
    }
    if (best >= 0) {
      optionSelectedIndex = (uint16_t)best;
    }
    return best >= 0;
  }
  void selectIndex(int i) {
    if (i >= 0 && i < (int)options.size()) {
      optionSelectedIndex = (uint16_t)i;
    }
  }
  void setTop(int top) {
    optionOffsetIndex = (uint16_t)(top < 0 ? 0 : top);
    drawScroll = true;
  }
};

MenuWidget* AlmanacApp::newMenu() {
  freeMenu();                    // see app_meshtastic.cpp: an in-place rebuild used to leak rows
  MenuWidget* m = new AlmMenu(0, header->height(), lcd.width(),
                              lcd.height() - header->height() - footer->height(),
                              fonts[ALM_FONT], N_MAX_ITEMS, 8);
  m->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  /* An alert's yellow (Moderate, Minor, unknown severity): style 2's defaults are black on GRAY_95,
   * MAGENTA when selected (GUI.h), so it is set here, explicitly. Red is AlmRedOption's. */
  m->setStyle(MenuWidget::ALTERNATE_STYLE, YELLOW, BLACK, BLACK, YELLOW);
  return m;
}

/* A Severe or Extreme alert's row: red on black, white on red when selected. MenuWidget has only two
 * styles (white and the yellow above), so this row draws its own colours over whatever it is handed. */
class AlmRedOption : public MenuOption {
public:
  AlmRedOption(MenuOption::keyType id, const char* t) : MenuOption(id, MenuWidget::ALTERNATE_STYLE, t) {}
  void redraw(LCD& lcd, uint16_t x, uint16_t y, uint16_t w, uint16_t h, colorType fg, colorType bg,
              bool opaque, bool selected, SmoothFont* font, uint16_t left) override {
    MenuOption::redraw(lcd, x, y, w, h, selected ? WHITE : RED, selected ? RED : BLACK, opaque, selected,
                       font, left);
  }
};

struct AlmWrapCtx {
  AlmanacApp* app;
  SmoothFont* font;
  int         style;             // ALM_STYLE_*: every row of the sentence in it (an alert keeps its colour)
};

static size_t almWrapFit(const char* s, void* ctx) {
  AlmWrapCtx* c = (AlmWrapCtx*)ctx;
  const int16_t f = c->font ? c->font->fitTextLength(s, ALM_WRAP_W, 1) : 0;
  return f > 0 ? (size_t)f : 0;
}

void AlmanacApp::emitRow(void* ctx, int kind, const char* text) {
  ((AlmanacApp*)ctx)->addRow(kind, text);
}

static void almWrapRow(const char* s, size_t len, void* ctx) {
  char line[96];
  if (len >= sizeof(line)) {
    len = sizeof(line) - 1;
  }
  memcpy(line, s, len);
  line[len] = '\0';
  AlmWrapCtx* c = (AlmWrapCtx*)ctx;
  AlmanacApp::wrapPiece(c->app, c->style, line);
}

void AlmanacApp::wrapPiece(void* ctx, int style, const char* text) {
  AlmanacApp* a = (AlmanacApp*)ctx;
  if (style == ALM_STYLE_PLAIN) {
    a->addRow(ALM_ROW_INFO, text);              // as a plain wrap always was
    return;
  }
  if (!a->menu || !text) {
    return;
  }
  uint32_t h = a->rowsHash ^ (uint32_t)(0x100 + style);
  h *= 16777619u;
  for (const char* p = text; *p; p++) {
    h = (h ^ (uint8_t)*p) * 16777619u;
  }
  a->rowsHash = h;
  const MenuOption::keyType key = (MenuOption::keyType)(a->infoKey++);
  if (style == ALM_STYLE_DANGER) {
    MenuOption* o = new AlmRedOption(key, text);
    if (o && !a->menu->addOption(o)) {
      delete o;
    }
  } else {
    a->menu->addOption(text, key, MenuWidget::ALTERNATE_STYLE);
  }
}

void AlmanacApp::addRow(int kind, const char* text) {
  if (!menu || !text) {
    return;
  }
  uint32_t h = rowsHash ^ (uint32_t)kind;
  h *= 16777619u;
  for (const char* p = text; *p; p++) {
    h = (h ^ (uint8_t)*p) * 16777619u;
  }
  rowsHash = h;
  if (kind == ALM_ROW_INFO) {
    menu->addOption(text, (MenuOption::keyType)(infoKey++), 1);
  } else if (kind == ALM_ROW_WRAP || kind == ALM_ROW_WARN || kind == ALM_ROW_DANGER) {
    AlmWrapCtx c;
    c.app = this;
    c.font = fonts[ALM_FONT];
    c.style = kind == ALM_ROW_WARN ? ALM_STYLE_WARN : kind == ALM_ROW_DANGER ? ALM_STYLE_DANGER
                                                                            : ALM_STYLE_PLAIN;
    wrapNote(text, almWrapFit, almWrapRow, &c, 6);
  } else if (kind > 0) {
    menu->addOption(text, (MenuOption::keyType)kind, 1);
  }
}

bool AlmanacApp::refreshPlace() {
  AlmPlace pl;
  almPlaceNow(viewOk, viewLatI, viewLonI, &pl);
  placeKind = pl.kind;
  strlcpy(placeName, pl.name, sizeof(placeName));
  placeLatI = pl.latI;
  placeLonI = pl.lonI;
  placeAgeMs = pl.ageMs;
  return placeKind != ALM_PLACE_NONE;
}

void AlmanacApp::fillCtx(AlmCtx* c) {
  memset(c, 0, sizeof(*c));
  c->clockKnown = ntpClock.isTimeKnown();
  c->now = (int64_t)ntpClock.getExactUtcTime();
  c->tzS = almanacTzOffsetS();
  c->clockSrc = clockSourceName(ntpClock.getSource());
  c->clockMesh = ntpClock.getSource() == CLOCK_SRC_MESH;
  c->dayOffset = dayOffset;
  c->placeKind = placeKind;
  c->placeName = placeName;
  c->placeAgeMs = placeAgeMs;
  c->lat = placeLatI * 1e-7;
  c->lon = placeLonI * 1e-7;
  c->rule = (gLegalRule == LEGAL_RULE_CIVIL) ? ASTRO_LEGAL_CIVIL : ASTRO_LEGAL_30MIN;
  c->units = gUnits;
  c->usDst = gUsDst != 0;
  almCtxZone(c);                 // a zone with rules (Settings > Time zone): DST by the rule
  c->moonAge = &moonAge;
  c->noSearch = true;            // a builder here never searches: the timer's slices fill the caches
  /* The weather as weather_net holds it (memory only: the card was read on the timer, if at all). */
  wxView.d = wxData();
  wxView.unreadable = wxLoaded && !wxCacheLoaded();
  wxView.fetching = wxRequestActive() || wxWant == ALM_WX_REFRESH;
  wxView.clockTrusted = ntpClock.isTimeTrusted();
  wxView.note = wxNote[0] ? wxNote : wxLastNote();
  c->wx = &wxView;
}

/* The shown day's slot, keyed for (the day's local midnight, its offset, the place). A place is
 * the same while it stays within almSamePlace (~100 m) of where the slot was KEYED - a tolerance
 * around that place, not a grid (review 2026-09-27: a still GPS within noise of a 0.001-degree
 * line re-keyed the whole day, ~148 ms of CPU, on a minute tick). A new key only QUEUES the day
 * (almDayWorkBegin: no maths) - the slices compute it. Called from every build, the key handlers'
 * included. NULL = no day screen, no clock, no place, no date. */
AlmanacApp::Slot* AlmanacApp::bindDay(AlmCtx* c) {
  c->day = NULL;
  if (!c->clockKnown || !isDayScreen(screen) || c->placeKind == ALM_PLACE_NONE) {
    return NULL;
  }
  const int64_t t0 = almDayT0(c);
  const int tz = almDayTzS(c);
  if (!almDateOf(t0, tz, NULL, NULL, NULL, NULL, NULL)) {
    return NULL;
  }
  Slot* s = (dayOffset == 0) ? &slotToday : &slotOther;
  const bool sameDay = s->keyed && s->t0 == t0 && s->tz == tz;
  if (!sameDay || !almSamePlace(s->lat, s->lon, c->lat, c->lon)) {
    /* keep = only the place moved (~100 m): the shown rows stay until their replacements land. */
    almDayWorkBegin(&s->day, &s->work, t0, tz, c->lat, c->lon, sameDay);
    s->keyed = true;
    s->t0 = t0;
    s->tz = tz;
    s->lat = c->lat;
    s->lon = c->lon;
    s->begunMs = millis();
    s->cpuUs = s->slices = s->maxUs = 0;
    s->coreDone = false;
    memset(s->partAccUs, 0, sizeof(s->partAccUs));
    gAlmanacCost.days++;
  }
  c->day = &s->day;
  return s;
}

static int64_t almLocalDay(const AlmCtx* c) {
  const int64_t l = c->now + c->tzS;
  return l >= 0 ? l / 86400 : -((-l + 86399) / 86400);
}

/* Is anything owed to the open screen: a part of its day, the moon's age (MOON), the seasons
 * (DATE). Cheap: no maths. */
bool AlmanacApp::owedFor(const AlmCtx* c, const Slot* s) {
  if (s) {
    AlmWant want;
    almScreenWant(screen, c, &want);
    if (almDayWorkLeft(&s->day, &s->work, &moonAge, &want)) {
      return true;
    }
  }
  return screen == ALM_SCREEN_DATE && c->clockKnown && seasonsDay != almLocalDay(c);
}

/* `workedMs`: how long the timer pass that calls this has already held the loop. WiPhone.ino
 * re-arms the app timer from the pass's START (msAppTimerEventLast = the pass's `now`, after
 * processEvent returns), so a 15 ms period after a 30 ms slice fired again on the very next pass
 * (review 2026-09-27): the period is the pass's own work + the gap, and the loop really gets
 * ALM_SLICE_GAP_MS after it. 0 from a key handler or the minute tick. */
void AlmanacApp::armTimer(uint32_t workedMs) {
  if (screen == ALM_SCREEN_POSITION) {
    return;                                       // its own 1 Hz (enter / the timer set it)
  }
  if (owed || landed) {
    if (controlState.msAppTimerEventPeriod == 0) {
      controlState.msAppTimerEventLast = millis();   // off until now: count from here
    }
    controlState.msAppTimerEventPeriod = workedMs + ALM_SLICE_GAP_MS;
  } else if (wxTimerWanted()) {
    /* The weather: the card's cache to read, a fetch to decide (the next pass), or one out - then a
     * poll every ALM_WX_POLL_MS for its result (weather_net folds it on the loop, whatever is open). */
    if (controlState.msAppTimerEventPeriod == 0) {
      controlState.msAppTimerEventLast = millis();
    }
    const bool polling = wxLoaded && wxWant == ALM_WX_NONE && wxGen() == wxShownGen;
    controlState.msAppTimerEventPeriod = polling ? ALM_WX_POLL_MS : workedMs + ALM_SLICE_GAP_MS;
  } else {
    controlState.msAppTimerEventPeriod = 0;
  }
}

bool AlmanacApp::wxTimerWanted() {
  return !wxLoaded || wxWant != ALM_WX_NONE || wxRequestActive() || wxGen() != wxShownGen;
}

/* APP TIMER ONLY: the weather's share of a pass that ran no slice. A decision waiting (wxWant) is
 * made now - the automatic rule or the Refresh - and, when a fetch is due, the ground under the
 * place is read (the elevation tiles: card I/O, so never in the key handler that set wxWant) and
 * the job queued on the shared worker. Otherwise: has a fetch landed (a new generation)? True =
 * the rows change (the NEXT pass rebuilds: ONE THING A PASS). */
bool AlmanacApp::wxStep(const AlmCtx* c) {
  if (wxWant != ALM_WX_NONE) {
    const bool explicitAsk = wxWant == ALM_WX_REFRESH;
    wxWant = ALM_WX_NONE;
    const bool placeOk = placeKind != ALM_PLACE_NONE;
    const int due = wxDecide(c->lat, c->lon, placeOk, explicitAsk);
    if (due != WX_DUE_YES) {
      if (explicitAsk) {
        strlcpy(wxNote, wxDueWhy(due), sizeof(wxNote));   // an automatic refusal says nothing
      }
      return explicitAsk;
    }
    WxAsk a;
    memset(&a, 0, sizeof(a));
    a.lat = c->lat;
    a.lon = c->lon;
    a.placeKind = placeKind;
    a.placeName = placeName;
    double m = 0;
    int z = -1;
    a.haveElev = almReadElevation(c->lat, c->lon, &m, &z) == ALM_ELEV_OK;   // never the GPS altitude
    a.elevM = m;
    char why[96];
    if (!wxRequest(&a, explicitAsk, why, sizeof(why))) {
      if (explicitAsk) {
        strlcpy(wxNote, why, sizeof(wxNote));
      }
      return explicitAsk;
    }
    wxNote[0] = '\0';
    return true;                                  // "Fetching the weather..."
  }
  return wxGen() != wxShownGen;
}

void AlmanacApp::setHeader() {
  /* The shown day on every day screen (Left/Right moves it): almTitle, whose every title is
   * measured against the header's room (test_almanac_lines). The title lives in this app. */
  almTitle(screen, ntpClock.isTimeKnown(), (int64_t)ntpClock.getExactUtcTime(), almanacTzOffsetS(),
           dayOffset, title, sizeof(title));
  header->setTitle(title);
}

/* The open screen from the caches. NO tables, no search, no scan: a part not there yet is a
 * "Computing..." row and `owed` arms the slices. The "now" rows' own maths (<= ~12 ms at 160 MHz:
 * the sun's and moon's position, the moon's phase at now without a search) is all it does. */
void AlmanacApp::build() {
  const uint32_t t = micros();
  landed = false;                  // whatever a slice landed is in the rows this builds
  refreshPlace();
  AlmCtx c;
  fillCtx(&c);
  Slot* s = bindDay(&c);
  if (screen == ALM_SCREEN_DATE && c.clockKnown && seasonsDay == almLocalDay(&c)) {
    c.seasons = seasons;
  }
  menu = newMenu();
  infoKey = ROW_INFO_BASE;
  rowsHash = 2166136261u;
  const uint32_t tr = micros();
  switch (screen) {
  case ALM_SCREEN_TODAY:
    almLinesToday(&c, emitRow, this);
    menu->addOption("Left/Right: another day", ROW_HINT, 1);
    break;
  case ALM_SCREEN_SUN:
    almLinesSun(&c, emitRow, this);
    break;
  case ALM_SCREEN_MOON:
    almLinesMoon(&c, emitRow, this);
    break;
  case ALM_SCREEN_SOLUNAR:
    almLinesSolunar(&c, emitRow, this);
    break;
  case ALM_SCREEN_POSITION: {
    AlmPos p;
    almFillPos(&p, elevState, elevM, elevZ);
    almLinesPosition(&c, &p, emitRow, this);
    break;
  }
  case ALM_SCREEN_DATE:
    almLinesDate(&c, emitRow, this);
    break;
  case ALM_SCREEN_WEATHER:
    almLinesWeather(&c, emitRow, this);
    break;
  default:
    almLinesSettings(&c, emitRow, this);
    break;
  }
  wxShownGen = wxGen();            // the weather these rows were built from
  gAlmanacCost.rowsUs = micros() - tr;
  setHeader();
  owed = owedFor(&c, s);
  gAlmanacCost.buildUs = micros() - t;
  if (gAlmanacCost.buildUs > gAlmanacCost.buildMaxUs) {
    gAlmanacCost.buildMaxUs = gAlmanacCost.buildUs;
  }
}

/* The same screen again (the minute tick, the 1 Hz Position refresh, a setting toggled, another
 * day, a part landed) with the highlight on the row it was on - an entry or a setting by its key,
 * a display row by its words (a row that appeared above it moves it down), else by its position -
 * and that row where it was on the glass (almMenuTop): the list does not jump back to the top. */
bool AlmanacApp::rebuildKeepingSelection() {
  MenuOption::keyType k = 0;
  int oldSel = 0, oldTop = 0;
  char text[96];
  text[0] = '\0';
  if (menu) {
    AlmMenu* o = (AlmMenu*)menu;
    k = menu->currentKey();
    oldSel = o->selIndex();
    oldTop = o->topIndex();
    const char* t = menu->getSelectedTitle();
    if (t) {
      strlcpy(text, t, sizeof(text));
    }
  }
  build();
  if (menu) {
    AlmMenu* m = (AlmMenu*)menu;
    if (k && k < ROW_INFO_BASE) {
      m->select(k);                               // an entry, a setting, the hint: its own key
    } else if (!(text[0] && m->selectText(text, ROW_INFO_BASE, oldSel))) {
      m->selectIndex(oldSel < m->rows() ? oldSel : m->rows() - 1);
    }
    m->setTop(almMenuTop(oldSel, oldTop, m->selIndex(), m->rows(), m->perScreen()));
  }
  const bool changed = rowsHash != shownHash;
  shownHash = rowsHash;
  return changed;
}

void AlmanacApp::enter(int newScreen, MenuOption::keyType selectKey) {
  screen = newScreen;
  if (screen == ALM_SCREEN_POSITION) {
    /* The first ground read soon (a tenth of a second after the screen is up - never in this
     * key handler), then once a second. */
    controlState.msAppTimerEventLast = millis();
    controlState.msAppTimerEventPeriod = 100;
    const int64_t dLat = (int64_t)placeLatI - elevLatI, dLon = (int64_t)placeLonI - elevLonI;
    if (elevState != ALM_ELEV_OK || dLat > ALM_ELEV_MOVE_E7 || dLat < -ALM_ELEV_MOVE_E7 ||
        dLon > ALM_ELEV_MOVE_E7 || dLon < -ALM_ELEV_MOVE_E7) {
      elevState = ALM_ELEV_PENDING;
    }
  } else {
    controlState.msAppTimerEventPeriod = 0;
  }
  if (screen == ALM_SCREEN_WEATHER && wxWant == ALM_WX_NONE) {
    wxWant = ALM_WX_AUTO;          // opening WEATHER: the automatic rule again (WiFi may be up now)
  }
  switch (screen) {
  case ALM_SCREEN_TODAY:    footer->setButtons("Select", "Back"); break;
  case ALM_SCREEN_SETTINGS: footer->setButtons("Change", "Back"); break;
  case ALM_SCREEN_WEATHER:  footer->setButtons("Refresh", "Back"); break;
  case ALM_SCREEN_SUN:
  case ALM_SCREEN_MOON:
  case ALM_SCREEN_SOLUNAR:  footer->setButtons("Today", "Back"); break;
  default:                  footer->setButtons("", "Back"); break;
  }
  build();
  shownHash = rowsHash;
  if (selectKey) {
    menu->select(selectKey);
  }
  armTimer();
}

/* APP TIMER ONLY: the ground under the place, when it has moved ~11 m or was never read (and a
 * card error or a missing layer is asked again after 30 s). One elevSampleCard is at most two
 * tiles: one open, one fstat and one or two 4-byte reads each (elev_sd.h). */
void AlmanacApp::sampleElevation() {
  if (placeKind == ALM_PLACE_NONE) {
    elevState = ALM_ELEV_PENDING;
    return;
  }
  const int64_t dLat = (int64_t)placeLatI - elevLatI, dLon = (int64_t)placeLonI - elevLonI;
  const bool moved = dLat > ALM_ELEV_MOVE_E7 || dLat < -ALM_ELEV_MOVE_E7 ||
                     dLon > ALM_ELEV_MOVE_E7 || dLon < -ALM_ELEV_MOVE_E7;
  const bool retry = (elevState == ALM_ELEV_IOERR || elevState == ALM_ELEV_NOLAYER) &&
                     (uint32_t)(millis() - elevAtMs) > ALM_ELEV_RETRY_MS;
  if (elevState != ALM_ELEV_PENDING && !moved && !retry) {
    return;
  }
  double m = 0;
  int z = -1;
  elevState = almReadElevation(placeLatI * 1e-7, placeLonI * 1e-7, &m, &z);
  elevM = m;
  elevZ = z;
  elevLatI = placeLatI;
  elevLonI = placeLonI;
  elevAtMs = millis();
}

// ---- the slices ---------------------------------------------------------------------------------

/* The more() of one slice: the deadline, and the cost of each part (the time since the last ask
 * goes to the part whose unit ran last: AlmDayWork.lastPart). */
struct AlmSliceAcct {
  uint32_t           startUs, markUs;
  AlmanacApp::Slot*  slot;
};

static int almPartIndex(int bit) {
  for (int i = 0; i < 7; i++) {
    if (bit == (1 << i)) {
      return i;
    }
  }
  return -1;
}

static void almSliceMark(AlmSliceAcct* a, uint32_t now) {
  if (a->slot) {
    const int k = almPartIndex(a->slot->work.lastPart);
    if (k >= 0) {
      a->slot->partAccUs[k] += now - a->markUs;
    }
  }
  a->markUs = now;
}

static bool almSliceMore(void* ctx) {
  AlmSliceAcct* a = (AlmSliceAcct*)ctx;
  const uint32_t now = micros();
  almSliceMark(a, now);
  return (uint32_t)(now - a->startUs) < ALM_SLICE_US;
}

static const char* const ALM_PART_NAMES[7] = { "day before", "day after", "quarters", "sun",
                                               "moon", "solunar", "moon age" };

/* APP TIMER: ONE THING A PASS (review 2026-09-27, finding 3). Either the rebuild and repaint of
 * what the last slice landed - no slice then - or one slice of what the open screen still needs,
 * which repaints nothing: a part that lands sets `landed` and the NEXT timer pass shows it. A
 * landing pass used to be the slice (~30 ms) + the rebuild (~12) + a full-screen push (~60),
 * ~100-120 ms estimated, 3-5 times a day computed. */
appEventResult AlmanacApp::slice() {
  const uint32_t passMs = millis();
  if (landed) {
    const bool changed = rebuildKeepingSelection();   // clears `landed`, sets `owed` again
    armTimer((uint32_t)(millis() - passMs) + (changed ? ALM_PAINT_EST_MS : 0));
    return changed ? REDRAW_SCREEN : DO_NOTHING;
  }
  if (!wxLoaded) {
    /* The weather cache, on the timer's first pass (card I/O; once a boot - weather_net keeps it in
     * memory after). It is this pass's one thing: the next pass shows it. A card that would not
     * answer is asked again on the next passes, ALM_WX_LOAD_TRIES in all (review 2026-10-03: one
     * failed read showed "No weather yet" over a good cache); then the screen says it was not read. */
    wxCacheLoadCard();
    wxLoaded = wxCacheLoaded() || ++wxLoadTries >= ALM_WX_LOAD_TRIES;
    landed = true;
    armTimer((uint32_t)(millis() - passMs));
    return DO_NOTHING;
  }
  refreshPlace();
  AlmCtx c;
  fillCtx(&c);
  Slot* s = bindDay(&c);
  AlmSliceAcct acct;
  acct.slot = s;
  int did = 0;
  bool worked = false;
  cpuClockRaise("almanac");
  acct.startUs = acct.markUs = micros();
  if (s) {
    AlmWant want;
    almScreenWant(screen, &c, &want);
    if (almDayWorkLeft(&s->day, &s->work, &moonAge, &want)) {
      s->work.lastPart = 0;
      did = almDayWorkRun(&s->day, &s->work, &moonAge, &want, almSliceMore, &acct);
      worked = true;
    }
  } else if (screen == ALM_SCREEN_DATE && c.clockKnown && seasonsDay != almLocalDay(&c)) {
    // DATE's four seasons: one unit (~14 ms at 160 MHz).
    for (int k = 0; k < 4; k++) {
      seasons[k] = astroNextSeason(c.now, k);
    }
    seasonsDay = almLocalDay(&c);
    gAlmanacCost.seasonsUs = micros() - acct.startUs;
    did = 1;
    worked = true;
  }
  const uint32_t now = micros();
  almSliceMark(&acct, now);
  const uint32_t us = now - acct.startUs;
  if (worked) {
    gAlmanacCost.slices++;
    gAlmanacCost.atMs = millis();
    gAlmanacCost.cpuMhz = getCpuFrequencyMhz();
    if (us > gAlmanacCost.sliceMaxUs) {
      gAlmanacCost.sliceMaxUs = us;
      gAlmanacCost.sliceMaxParts = (uint32_t)did | (s ? (uint32_t)s->work.lastPart : 0u);
    }
  }
  if (s && worked) {
    s->cpuUs += us;
    s->slices++;
    if (us > s->maxUs) {
      s->maxUs = us;
    }
    for (int k = 0; k < 7; k++) {
      if (did & (1 << k)) {
        gAlmanacCost.partUs[k] = s->partAccUs[k];   // the part's whole run, over its slices
        s->partAccUs[k] = 0;
      }
    }
    // Share today's sun with the Meshtastic Sun screen and serial `sun` (almanacLegalToday).
    if (s == &slotToday && (did & ALM_PART_SUN)) {
      almanacLegalSeed(s->day.t0, s->day.lat, s->day.lon, &s->day.sun, NULL);
    }
    if (s == &slotToday && (did & ALM_NEED_NEXT)) {
      almanacLegalSeed(s->day.t0, s->day.lat, s->day.lon, NULL, &s->day.next);
    }
    if (!s->coreDone && s->day.valid && (s->work.done & ALM_PART_CORE) == ALM_PART_CORE) {
      s->coreDone = true;
      gAlmanacCost.dayCpuUs = s->cpuUs;
      gAlmanacCost.daySlices = s->slices;
      gAlmanacCost.dayMaxUs = s->maxUs;
      gAlmanacCost.dayWallMs = millis() - s->begunMs;
      /* ONE line per day, so the real cost can be read off the device. log_e, not log_i: this
       * build's log level DROPS log_i (docs/HANDOFF.md, the `meshdb` note). The place to 2
       * decimals (~1 km): it is often this phone's own fix now, and logs end up in notes. */
      log_e("ALMANAC: day %+d at %.2f,%.2f whole in %u slices: CPU %u us (sun %u, moon %u, sol %u), "
            "longest slice %u us, %u ms after it was asked (%u MHz)",
            dayOffset, c.lat, c.lon, (unsigned)s->slices, (unsigned)s->cpuUs,
            (unsigned)gAlmanacCost.partUs[3], (unsigned)gAlmanacCost.partUs[4],
            (unsigned)gAlmanacCost.partUs[5], (unsigned)s->maxUs,
            (unsigned)gAlmanacCost.dayWallMs, (unsigned)gAlmanacCost.cpuMhz);
    }
  }
  if (did) {
    landed = true;                                // shown by the next timer pass, not this one
  } else {
    owed = owedFor(&c, s);
    /* Nothing of the day left to compute: the weather's turn (a fetch only once the day's core has
     * landed - the slices and the TLS worker share core 1). */
    if (!worked && !owed && wxStep(&c)) {
      landed = true;
    }
  }
  armTimer((uint32_t)(millis() - passMs));
  return DO_NOTHING;
}

appEventResult AlmanacApp::processEvent(EventType event) {
  if (event == TIME_UPDATE_EVENT) {
    /* The minute tick: the countdown, the "now" rows, the local date's roll-over - from the
     * caches (a new date or place only QUEUES a day for the slices). POSITION has its own second
     * tick; SETTINGS shows no time. */
    if (screen == ALM_SCREEN_POSITION || screen == ALM_SCREEN_SETTINGS) {
      return DO_NOTHING;
    }
    const uint32_t t = micros();
    const bool changed = rebuildKeepingSelection();
    gAlmanacCost.tickBuildUs = micros() - t;
    armTimer();
    /* Unchanged rows (a SUN screen between events): the new widget waits for the next paint
     * with the same picture. The GUI repaints the header's clock on this event itself. */
    return changed ? (REDRAW_SCREEN | REDRAW_HEADER) : DO_NOTHING;
  }
  if (event == APP_TIMER_EVENT) {
    if (screen == ALM_SCREEN_POSITION) {
      controlState.msAppTimerEventPeriod = 1000;
      refreshPlace();
      sampleElevation();
      return rebuildKeepingSelection() ? REDRAW_SCREEN : DO_NOTHING;
    }
    return slice();
  }
  if (!IS_KEYBOARD(event) || !menu) {
    return DO_NOTHING;
  }

  /* ⚠ KEY HANDLERS COMPUTE NO TABLE (A1): they move the screen or the day and rebuild from what is
   * there; a new day is only queued (bindDay) and the timer's slices compute it. */
  if (LOGIC_BUTTON_BACK(event)) {
    if (screen == ALM_SCREEN_TODAY) {
      return EXIT_APP;
    }
    /* Indexed by screen: GROWN with the enum (an index past it falls back to [0], which would
     * land Back from WEATHER on "Sun..."). */
    static const MenuOption::keyType ENTRY_OF[ALM_SCREEN_COUNT] = {
      ALM_ENTRY_SUN, ALM_ENTRY_SUN, ALM_ENTRY_MOON, ALM_ENTRY_SOLUNAR,
      ALM_ENTRY_POSITION, ALM_ENTRY_DATE, ALM_ENTRY_SETTINGS, ALM_ENTRY_WEATHER,
    };
    const MenuOption::keyType back = ENTRY_OF[(screen >= 0 && screen < ALM_SCREEN_COUNT) ? screen : 0];
    enter(ALM_SCREEN_TODAY, back);
    return REDRAW_ALL;
  }

  if (event == WIPHONE_KEY_LEFT || event == WIPHONE_KEY_RIGHT) {
    if (!isDayScreen(screen) || !ntpClock.isTimeKnown()) {
      return DO_NOTHING;         // (and never to the menu: it takes RIGHT as OK)
    }
    int n = dayOffset + (event == WIPHONE_KEY_RIGHT ? 1 : -1);
    if (n > ALM_DAY_OFFSET_MAX) n = ALM_DAY_OFFSET_MAX;
    if (n < -ALM_DAY_OFFSET_MAX) n = -ALM_DAY_OFFSET_MAX;
    if (n == dayOffset) {
      return DO_NOTHING;
    }
    dayOffset = n;
    rebuildKeepingSelection();   // queues the day; onto one not computed yet: no astronomy at all
    armTimer();
    return REDRAW_SCREEN | REDRAW_HEADER;
  }

  if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN) {
    menu->processEvent(event);
    return REDRAW_SCREEN;
  }

  if (LOGIC_BUTTON_OK(event)) {
    if (screen == ALM_SCREEN_WEATHER) {
      /* Refresh (the left soft key, or OK on any row): ONLY queued - the next timer pass decides,
       * reads the ground and starts the fetch. No card, no network, no TLS in a key handler. */
      wxWant = ALM_WX_REFRESH;
      wxNote[0] = '\0';
      rebuildKeepingSelection();                  // "Fetching the weather..."
      armTimer();
      return REDRAW_SCREEN;
    }
    const MenuOption::keyType k = menu->currentKey();
    switch (k) {
    case ALM_ENTRY_SUN:      enter(ALM_SCREEN_SUN, 0);      return REDRAW_ALL;
    case ALM_ENTRY_MOON:     enter(ALM_SCREEN_MOON, 0);     return REDRAW_ALL;
    case ALM_ENTRY_SOLUNAR:  enter(ALM_SCREEN_SOLUNAR, 0);  return REDRAW_ALL;
    case ALM_ENTRY_POSITION: enter(ALM_SCREEN_POSITION, 0); return REDRAW_ALL;
    case ALM_ENTRY_DATE:     enter(ALM_SCREEN_DATE, 0);     return REDRAW_ALL;
    case ALM_ENTRY_SETTINGS: enter(ALM_SCREEN_SETTINGS, 0); return REDRAW_ALL;
    case ALM_ENTRY_WEATHER:  enter(ALM_SCREEN_WEATHER, 0);  return REDRAW_ALL;
    case ALM_SET_LEGAL:
      legalRuleSetPref(gLegalRule == LEGAL_RULE_CIVIL ? LEGAL_RULE_30MIN : LEGAL_RULE_CIVIL);
      rebuildKeepingSelection();
      return REDRAW_SCREEN;
    case ALM_SET_UNITS:
      unitsSetPref(gUnits == UNITS_US ? UNITS_METRIC : UNITS_US);
      rebuildKeepingSelection();
      return REDRAW_SCREEN;
    case ALM_SET_DST:
      usDstSetPref(gUsDst ? 0 : 1);
      rebuildKeepingSelection();
      return REDRAW_SCREEN;
    default:
      break;
    }
    /* Anything else on a day screen: back to today ("Today" is the left soft key on SUN, MOON
     * and SOLUNAR; on TODAY a display row does the same). */
    if (isDayScreen(screen) && dayOffset != 0) {
      dayOffset = 0;
      rebuildKeepingSelection();
      armTimer();
      return REDRAW_SCREEN | REDRAW_HEADER;
    }
    return DO_NOTHING;
  }
  return DO_NOTHING;
}

void AlmanacApp::redrawScreen(bool redrawAll) {
  if (menu) {
    const uint32_t t = micros();
    ((GUIWidget*)menu)->redraw(lcd);
    gAlmanacCost.paintUs = micros() - t;          // into the sprite; the band's push follows
    if (gAlmanacCost.paintUs > gAlmanacCost.paintMaxUs) {
      gAlmanacCost.paintMaxUs = gAlmanacCost.paintUs;
    }
  }
}

// ---- the serial `almanac` -----------------------------------------------------------------------

struct AlmConsoleCtx {
  void (*out)(const char*);
};

static void almConsoleRow(void* ctx, int kind, const char* text) {
  if (kind >= ALM_ENTRY_SUN) {
    return;                                       // the entries are navigation, not content
  }
  char line[160];
  snprintf(line, sizeof(line), "  %s", text);
  ((AlmConsoleCtx*)ctx)->out(line);
}

static void almOutf(void (*out)(const char*), const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void almOutf(void (*out)(const char*), const char* fmt, ...) {
  char line[190];                                 // < say()'s 192 with its newline
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  out(line);
}

// "12.345" (ms, from us)
static const char* almMs(uint32_t us, char* buf, size_t cap) {
  snprintf(buf, cap, "%u.%03u", (unsigned)(us / 1000), (unsigned)(us % 1000));
  return buf;
}

static void almConsoleCost(void (*out)(const char*)) {
  const AlmanacCost& k = gAlmanacCost;
  if (!k.slices && !k.buildUs && !k.serialUs) {
    out("almanac cost: nothing measured yet - open Menu > Almanac, or run `almanac`");
    return;
  }
  char a[16], b[16], c[16], d[16];
  almOutf(out, "almanac cost: the day comes in slices on the app timer (each <= %u ms of work, then "
          "%u ms for the loop); the last slice %u ms ago at %u MHz", (unsigned)(ALM_SLICE_US / 1000),
          (unsigned)ALM_SLICE_GAP_MS,
          k.slices ? (unsigned)(millis() - k.atMs) : 0u, (unsigned)k.cpuMhz);
  char parts[64];
  parts[0] = '\0';
  for (int i = 0; i < 7; i++) {
    if (k.sliceMaxParts & (1u << i)) {
      strlcat(parts, parts[0] ? ", " : "", sizeof(parts));
      strlcat(parts, ALM_PART_NAMES[i], sizeof(parts));
    }
  }
  almOutf(out, "  slices since boot %u, the longest %s ms (%s)", (unsigned)k.slices,
          almMs(k.sliceMaxUs, a, sizeof(a)), parts[0] ? parts : "seasons");
  if (k.daySlices) {
    almOutf(out, "  the last whole day: CPU %s ms in %u slices (longest %s ms), on the screen %u ms "
            "after it was asked for", almMs(k.dayCpuUs, a, sizeof(a)), (unsigned)k.daySlices,
            almMs(k.dayMaxUs, b, sizeof(b)), (unsigned)k.dayWallMs);
  }
  almOutf(out, "  its parts (CPU, over their slices): sun %s, moon %s, solunar %s ms",
          almMs(k.partUs[3], a, sizeof(a)), almMs(k.partUs[4], b, sizeof(b)),
          almMs(k.partUs[5], c, sizeof(c)));
  almOutf(out, "  on demand: day before (SUN) %s, day after (TODAY after dark) %s, quarters (MOON) %s, "
          "moon age (MOON) %s ms", almMs(k.partUs[0], a, sizeof(a)), almMs(k.partUs[1], b, sizeof(b)),
          almMs(k.partUs[2], c, sizeof(c)), almMs(k.partUs[6], d, sizeof(d)));
  almOutf(out, "  next seasons (DATE, one slice) %s ms", almMs(k.seasonsUs, a, sizeof(a)));
  almOutf(out, "  screen builds (no tables; the \"now\" rows included): last %s ms (rows %s), minute "
          "tick %s ms, longest %s ms", almMs(k.buildUs, a, sizeof(a)), almMs(k.rowsUs, b, sizeof(b)),
          almMs(k.tickBuildUs, c, sizeof(c)), almMs(k.buildMaxUs, d, sizeof(d)));
  almOutf(out, "  paints (the menu into the sprite; the band's push, ~48 ms, follows): last %s ms, "
          "longest %s ms. A slice never paints: a part it lands is shown by the next pass",
          almMs(k.paintUs, a, sizeof(a)), almMs(k.paintMaxUs, b, sizeof(b)));
  if (k.serialUs) {
    almOutf(out, "  last serial `almanac` (ONE pass: everything, plus its elevation read): %s ms at %u MHz",
            almMs(k.serialUs, a, sizeof(a)), (unsigned)k.serialMhz);
  }
  out("  `almanac bench` times each piece now (one pass, ~0.3 s at 160 MHz)");
}

// A builder's rows counted, not printed (`almanac bench`).
static void almCountRow(void* ctx, int kind, const char* text) {
  (*(int*)ctx)++;
}

/* `almanac bench`: each piece the Almanac is made of, timed now at this place and this clock -
 * the answer to "what does a build or a slice spend its time on", on the phone itself. ONE pass
 * (~0.3 s at 160 MHz): a bench command, never something the app does. */
static void almConsoleBench(void (*out)(const char*)) {
  if (!ntpClock.isTimeKnown()) {
    out("almanac bench: clock not set yet");
    return;
  }
  int32_t vLa = 0, vLo = 0;
  const bool vOk = almReadMapView(&vLa, &vLo);
  AlmPlace pl;
  almPlaceNow(vOk, vLa, vLo, &pl);
  if (pl.kind == ALM_PLACE_NONE) {
    pl.latI = 474957000;                          // North Bend: any place times the same
    pl.lonI = -1217868000;
  }
  const double lat = pl.latI * 1e-7, lon = pl.lonI * 1e-7;
  const int64_t now = (int64_t)ntpClock.getExactUtcTime();
  const int tz = almanacTzOffsetS();
  const int64_t t0 = almMidnight(now, tz, 0);
  /* ZEROED (review 2026-09-27): the row builders read havePrev/haveNext/havePhases and the
   * tables behind them - the bench fills only the core. */
  AlmDay* d = (AlmDay*)heap_caps_calloc(1, sizeof(AlmDay), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!d) {
    out("almanac bench: no memory");
    return;
  }
  cpuClockRaise("almanac");
  char a[16];
  uint32_t t, total = micros();
  double al = 0, az = 0;
  AstroMoonPhase ph;
  // ~1 km is plenty to say where a timing ran (and the place is often this phone's own fix).
  almOutf(out, "almanac bench at %.2f,%.2f (%s), %u MHz - ONE pass, a bench command:", lat, lon,
          pl.kind == ALM_PLACE_NONE ? "no place: North Bend" : pl.name, (unsigned)getCpuFrequencyMhz());
  t = micros(); astroSunPos(now, lat, lon, &al, &az);
  almOutf(out, "  sun now (astroSunPos, ~27 libm calls)            %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); astroMoonPos(now, lat, lon, &al, &az);
  almOutf(out, "  moon now (astroMoonPos, ~215)                    %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); astroMoonPhaseWith(now, 0, &ph);
  almOutf(out, "  phase at now, no search (TODAY's, ~215)          %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); astroMoonPhaseAt(now, &ph);
  almOutf(out, "  phase with its new-moon search (the old, ~960)   %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); (void)astroNextMoonPhase(now, 1);
  almOutf(out, "  one quarter's search (a slice's unit, ~650-830)  %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros();
  for (int k = 0; k < 4; k++) (void)astroNextSeason(now, k);
  almOutf(out, "  the four seasons (DATE, ~540)                    %s ms", almMs(micros() - t, a, sizeof(a)));
  WmmField f;
  t = micros(); wmmCompute(lat, lon, 0.0, wmmDecimalYear(now), &f);
  almOutf(out, "  declination (wmmCompute, ~180)                   %s ms", almMs(micros() - t, a, sizeof(a)));
  char mg[24];
  t = micros(); geoToMgrs(lat, lon, mg, sizeof(mg));
  almOutf(out, "  MGRS (geoToMgrs, ~30)                            %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); astroSunDay(t0, lat, lon, &d->sun);
  almOutf(out, "  a sun day (astroSunDay, ~1,850)                  %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); astroMoonDay(t0, lat, lon, &d->moon);
  almOutf(out, "  a moon day (astroMoonDay, ~3,650)                %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); astroSolunar(t0, &d->sun, &d->moon, &d->sol);
  almOutf(out, "  the solunar table (~90)                          %s ms", almMs(micros() - t, a, sizeof(a)));
  // The builders on that day, as the app calls them (noSearch; the moon's age cached).
  d->t0 = t0;
  d->tzS = tz;
  d->lat = lat;
  d->lon = lon;
  d->have = ALM_PART_CORE;
  d->valid = true;
  AlmMoonAge age;
  memset(&age, 0, sizeof(age));
  while (!almMoonAgeWork(&age, now)) {}
  AlmCtx c;
  memset(&c, 0, sizeof(c));
  c.clockKnown = true;
  c.now = now;
  c.tzS = tz;
  c.clockSrc = clockSourceName(ntpClock.getSource());
  c.placeKind = pl.kind == ALM_PLACE_NONE ? ALM_PLACE_GIVEN : pl.kind;
  c.placeName = pl.name;
  c.placeAgeMs = pl.ageMs;
  c.lat = lat;
  c.lon = lon;
  c.rule = (gLegalRule == LEGAL_RULE_CIVIL) ? ASTRO_LEGAL_CIVIL : ASTRO_LEGAL_30MIN;
  c.units = gUnits;
  c.usDst = gUsDst != 0;
  almCtxZone(&c);
  c.day = d;
  c.moonAge = &age;
  c.noSearch = true;
  int rows = 0;
  AlmPos p;
  almFillPos(&p, ALM_ELEV_PENDING, 0, -1);
  t = micros(); almLinesToday(&c, almCountRow, &rows);
  almOutf(out, "  TODAY's rows (a minute tick's work)              %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); almLinesSun(&c, almCountRow, &rows);
  almOutf(out, "  SUN's rows                                       %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); almLinesMoon(&c, almCountRow, &rows);
  almOutf(out, "  MOON's rows                                      %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); almLinesSolunar(&c, almCountRow, &rows);
  almOutf(out, "  SOLUNAR's rows                                   %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); almLinesPosition(&c, &p, almCountRow, &rows);
  almOutf(out, "  POSITION's rows (1 Hz)                           %s ms", almMs(micros() - t, a, sizeof(a)));
  t = micros(); almLinesDate(&c, almCountRow, &rows);
  almOutf(out, "  DATE's rows                                      %s ms", almMs(micros() - t, a, sizeof(a)));
  almOutf(out, "almanac bench: %s ms in all (%d rows)", almMs(micros() - total, a, sizeof(a)), rows);
  heap_caps_free(d);
}

void almanacConsole(const char* args, void (*out)(const char*)) {
  while (*args == ' ') args++;
  if (!strcasecmp(args, "cost")) {
    almConsoleCost(out);
    return;
  }
  if (!strcasecmp(args, "bench")) {
    almConsoleBench(out);
    return;
  }
  // [lat,lon] [+N|-N]
  bool given = false;
  double la = 0, lo = 0;
  int off = 0;
  char buf[64];
  strlcpy(buf, args, sizeof(buf));
  char* save = NULL;
  for (char* tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
    if (strchr(tok, ',')) {
      if (sscanf(tok, "%lf,%lf", &la, &lo) != 2 || !almPlaceOk(la, lo)) {
        out("almanac: bad place - `almanac 47.4957,-121.7868` (lat,lon in degrees, no space)");
        return;
      }
      given = true;
    } else if ((tok[0] == '+' || tok[0] == '-') && tok[1] >= '0' && tok[1] <= '9') {
      off = atoi(tok);
      if (off < -ALM_DAY_OFFSET_MAX || off > ALM_DAY_OFFSET_MAX) {
        out("almanac: the day offset runs -365..+365");
        return;
      }
    } else {
      out("almanac: usage `almanac [lat,lon] [+N|-N]`, `almanac cost` or `almanac bench`");
      return;
    }
  }
  if (!ntpClock.isTimeKnown()) {
    out("almanac: clock not set yet (needs NTP on WiFi, a GPS fix, or mesh time - see `clock`)");
    return;
  }

  AlmCtx c;
  memset(&c, 0, sizeof(c));
  AlmPlace pl;
  memset(&pl, 0, sizeof(pl));
  if (given) {
    c.placeKind = ALM_PLACE_GIVEN;
    strlcpy(pl.name, "given", sizeof(pl.name));
    c.lat = la;
    c.lon = lo;
  } else {
    // The app's place, the same fallbacks (A2): reference, last GPS fix, the map's view.
    int32_t vLa = 0, vLo = 0;
    const bool vOk = almReadMapView(&vLa, &vLo);
    almPlaceNow(vOk, vLa, vLo, &pl);
    c.placeKind = pl.kind;
    c.placeAgeMs = pl.ageMs;
    c.lat = pl.latI * 1e-7;
    c.lon = pl.lonI * 1e-7;
    /* A MEASURED place (this phone's GPS or the hotspot phone's) or a Wi-Fi location is printed
     * to 2 decimals here and nowhere finer (AlmCtx.coordsCoarse): a typed place is the bench's
     * own and stays exact, which is what the PyEphem comparison uses (`almanac 40.0,-100.0`). */
    c.coordsCoarse = pl.kind == ALM_PLACE_GPS || pl.kind == ALM_PLACE_LAST_GPS ||
                     pl.kind == ALM_PLACE_PHONE || pl.kind == ALM_PLACE_LAST_PHONE ||
                     pl.kind == ALM_PLACE_NET;
  }
  c.placeName = pl.name;
  c.clockKnown = true;
  c.now = (int64_t)ntpClock.getExactUtcTime();
  c.tzS = almanacTzOffsetS();
  c.clockSrc = clockSourceName(ntpClock.getSource());
  c.clockMesh = ntpClock.getSource() == CLOCK_SRC_MESH;
  c.dayOffset = off;
  c.rule = (gLegalRule == LEGAL_RULE_CIVIL) ? ASTRO_LEGAL_CIVIL : ASTRO_LEGAL_30MIN;
  c.units = gUnits;
  c.usDst = gUsDst != 0;
  almCtxZone(&c);

  // The day's tables are ~330 bytes: PSRAM, not this (loop task) stack.
  AlmDay* d = (AlmDay*)heap_caps_malloc(sizeof(AlmDay), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!d) {
    out("almanac: no memory for the day's tables");
    return;
  }
  // The day in ITS offset (almDayTzS: across a US clock change, the offset that day will have).
  const int dayTz = almDayTzS(&c);
  const int64_t t0 = almDayT0(&c);
  char date[24], tz[16];
  almFmtDate(t0, dayTz, date, sizeof(date));
  almFmtUtcOffset(dayTz, tz, sizeof(tz));
  int y = 0, mo = 0, dd = 0;
  const bool inRange = almDateOf(t0, dayTz, &y, &mo, &dd, NULL, NULL);
  if (c.clockMesh) {
    out("almanac: WARNING the clock came from the MESH (lower trust) - check it before relying on this");
  }
  almOutf(out, c.coordsCoarse ? "almanac: %s %d (%+d days) at %s ~%.2f,%.2f; %s via %s; legal rule %s; units %s"
                               : "almanac: %s %d (%+d days) at %s %.5f,%.5f; %s via %s; legal rule %s; units %s",
          date, y, off, c.placeKind == ALM_PLACE_NONE ? "(no place)" : pl.name, c.lat, c.lon, tz,
          c.clockSrc, c.rule == ASTRO_LEGAL_CIVIL ? "civil twilight" : "30 min",
          c.units == UNITS_US ? "US" : "metric");
  out("almanac: ONE pass (a bench command, ~0.4 s at 160 MHz; the app computes the same tables in "
      "slices on its timer)");

  cpuClockRaise("almanac");
  const uint32_t tStart = micros();
  int64_t seasonsNow[4] = { 0, 0, 0, 0 };
  if (c.placeKind != ALM_PLACE_NONE && inRange) {
    almDayCompute(d, t0, dayTz, c.lat, c.lon);
    almDayEnsure(d, ALM_NEED_PREV | ALM_NEED_NEXT | ALM_NEED_PHASES, almPhaseFrom(&c));
    c.day = d;
  }
  for (int k = 0; k < 4; k++) {
    seasonsNow[k] = astroNextSeason(c.now, k);
  }
  c.seasons = seasonsNow;
  const uint32_t computeUs = micros() - tStart;

  AlmConsoleCtx cc;
  cc.out = out;
  out("-- today --");
  almLinesToday(&c, almConsoleRow, &cc);
  out("-- sun --");
  almLinesSun(&c, almConsoleRow, &cc);
  out("-- moon --");
  almLinesMoon(&c, almConsoleRow, &cc);
  out("-- solunar --");
  almLinesSolunar(&c, almConsoleRow, &cc);
  out("-- position --");
  {
    double m = 0;
    int z = -1;
    const int es = (c.placeKind == ALM_PLACE_NONE) ? ALM_ELEV_PENDING
                                                   : almReadElevation(c.lat, c.lon, &m, &z);
    AlmPos p;
    almFillPos(&p, es, m, z);
    almLinesPosition(&c, &p, almConsoleRow, &cc);
  }
  out("-- date (today, whatever the offset) --");
  almLinesDate(&c, almConsoleRow, &cc);

  // The exact instants, for the bench's comparison against PyEphem (UNIX seconds, UTC).
  if (c.day) {
    const AlmDay& D = *d;
    int64_t f30 = 0, l30 = 0, fc = 0, lc = 0;
    astroLegalLight(&D.sun, ASTRO_LEGAL_30MIN, &f30, &l30);
    astroLegalLight(&D.sun, ASTRO_LEGAL_CIVIL, &fc, &lc);
    almOutf(out, c.coordsCoarse ? "raw: t0=%lld tz=%d lat~%.2f lon~%.2f (UNIX s; 0 = none that day; a "
                                  "measured place is rounded here - type one for the exact run)"
                                : "raw: t0=%lld tz=%d lat=%.6f lon=%.6f (UNIX s; 0 = none that day)",
            (long long)D.t0, D.tzS, D.lat, D.lon);
    almOutf(out, "raw sun: dawn=%lld rise=%lld noon=%lld set=%lld dusk=%lld",
            (long long)D.sun.dawn, (long long)D.sun.rise, (long long)D.sun.noon,
            (long long)D.sun.set, (long long)D.sun.dusk);
    almOutf(out, "raw legal: 30min %lld..%lld  civil %lld..%lld", (long long)f30, (long long)l30,
            (long long)fc, (long long)lc);
    almOutf(out, "raw moon: rise=%lld set=%lld transit=%lld under=%lld", (long long)D.moon.rise,
            (long long)D.moon.set, (long long)D.moon.transit, (long long)D.moon.under);
    almOutf(out, "raw solunar: rating=%d majors=%lld,%lld minors=%lld,%lld", D.sol.rating,
            (long long)(D.sol.nMajor > 0 ? D.sol.majorMid[0] : 0),
            (long long)(D.sol.nMajor > 1 ? D.sol.majorMid[1] : 0),
            (long long)(D.sol.nMinor > 0 ? D.sol.minorMid[0] : 0),
            (long long)(D.sol.nMinor > 1 ? D.sol.minorMid[1] : 0));
    almOutf(out, "raw quarters after %lld: new=%lld first=%lld full=%lld last=%lld",
            (long long)D.phaseFrom, (long long)D.phase[0], (long long)D.phase[1],
            (long long)D.phase[2], (long long)D.phase[3]);
    almOutf(out, "raw day before: rise=%lld set=%lld; day after: dawn=%lld rise=%lld",
            (long long)D.prev.rise, (long long)D.prev.set, (long long)D.next.dawn,
            (long long)D.next.rise);
  }
  almOutf(out, "raw seasons after %lld: mar=%lld jun=%lld sep=%lld dec=%lld", (long long)c.now,
          (long long)seasonsNow[0], (long long)seasonsNow[1], (long long)seasonsNow[2],
          (long long)seasonsNow[3]);
  gAlmanacCost.serialUs = micros() - tStart;
  gAlmanacCost.serialMhz = getCpuFrequencyMhz();
  almOutf(out, "almanac: tables %u.%03u ms, all of it %u.%03u ms at %u MHz, in ONE pass (`almanac cost`)",
          (unsigned)(computeUs / 1000), (unsigned)(computeUs % 1000),
          (unsigned)(gAlmanacCost.serialUs / 1000), (unsigned)(gAlmanacCost.serialUs % 1000),
          (unsigned)gAlmanacCost.serialMhz);
  heap_caps_free(d);
}

// ---- the serial `wx` (0.9.81) -------------------------------------------------------------------

static void wxConsoleRow(void* ctx, int kind, const char* text) {
  if (kind >= ALM_ENTRY_SUN) {
    return;
  }
  char line[160];
  /* The colour is the bench's to see too: the screen draws these rows yellow / red. */
  snprintf(line, sizeof(line), "  %s%s", text,
           kind == ALM_ROW_DANGER ? "   [red]" : kind == ALM_ROW_WARN ? "   [yellow]" : "");
  ((AlmConsoleCtx*)ctx)->out(line);
}

void almanacWeatherConsole(const char* args, void (*out)(const char*)) {
  extern volatile bool gGbcActive;
  while (*args == ' ') args++;
  /* ⚠ Not while a game runs: every sub-command can touch the card (the cache's first read, the
   * clear, the ground read) and the game's blit task owns the SPI bus the card shares with the
   * screen - the console runs on the loop beside it (meshdb cut's rule; review 2026-10-03). A
   * fetch would be refused anyway: the game turns WiFi off. */
  if (gGbcActive) {
    out("wx: refused - a game owns the card right now");
    return;
  }
  if (!*args) {
    wxReport(out);
    return;
  }
  if (!strcasecmp(args, "clear")) {
    wxClear();
    out("wx: cache cleared (" WX_FILE ")");
    return;
  }
  const bool fetch = !strcasecmp(args, "fetch");
  const bool show = !strcasecmp(args, "show");
  if (!fetch && !show) {
    out("wx: wx | wx fetch | wx show | wx clear");
    return;
  }
  // The app's place, the same fallbacks (A2): reference, last GPS fix, the map's view.
  int32_t vLa = 0, vLo = 0;
  const bool vOk = almReadMapView(&vLa, &vLo);
  AlmPlace pl;
  almPlaceNow(vOk, vLa, vLo, &pl);
  const double lat = pl.latI * 1e-7, lon = pl.lonI * 1e-7;
  if (fetch) {
    /* The Refresh path: the same decision (an explicit ask), the same ground read, the same gates. */
    const int due = wxDecide(lat, lon, pl.kind != ALM_PLACE_NONE, true);
    if (due != WX_DUE_YES) {
      almOutf(out, "wx: NOT fetched - %s", wxDueWhy(due));
      return;
    }
    WxAsk a;
    memset(&a, 0, sizeof(a));
    a.lat = lat;
    a.lon = lon;
    a.placeKind = pl.kind;
    a.placeName = pl.name;
    double m = 0;
    int z = -1;
    a.haveElev = almReadElevation(lat, lon, &m, &z) == ALM_ELEV_OK;
    a.elevM = m;
    char why[96];
    if (wxRequest(&a, true, why, sizeof(why))) {
      almOutf(out, "wx: fetching for %s (%s ground) - `wx` for progress, `wx show` for the rows",
              pl.name[0] ? pl.name : "?", a.haveElev ? "with the" : "no");
    } else {
      almOutf(out, "wx: NOT fetched - %s", why);
    }
    return;
  }
  // `wx show`: the WEATHER screen's rows, through the same builder, at the app's place and clock.
  wxCacheLoadCard();
  AlmCtx c;
  memset(&c, 0, sizeof(c));
  c.clockKnown = ntpClock.isTimeKnown();
  c.now = (int64_t)ntpClock.getExactUtcTime();
  c.tzS = almanacTzOffsetS();
  c.clockSrc = clockSourceName(ntpClock.getSource());
  c.clockMesh = ntpClock.getSource() == CLOCK_SRC_MESH;
  c.placeKind = pl.kind;
  c.placeName = pl.name;
  c.placeAgeMs = pl.ageMs;
  c.lat = lat;
  c.lon = lon;
  c.rule = (gLegalRule == LEGAL_RULE_CIVIL) ? ASTRO_LEGAL_CIVIL : ASTRO_LEGAL_30MIN;
  c.units = gUnits;
  c.usDst = gUsDst != 0;
  almCtxZone(&c);
  WxView v;
  memset(&v, 0, sizeof(v));
  v.d = wxData();
  v.unreadable = wxCacheTried() && !wxCacheLoaded();
  v.fetching = wxRequestActive();
  v.clockTrusted = ntpClock.isTimeTrusted();
  v.note = wxLastNote();
  c.wx = &v;
  AlmConsoleCtx cc;
  cc.out = out;
  almOutf(out, "-- weather (units %s) --", c.units == UNITS_US ? "US" : "metric");
  almLinesWeather(&c, wxConsoleRow, &cc);
  out("-- today's weather rows --");
  wxTodayAlertRow(&c, wxConsoleRow, &cc);
  wxTodaySummaryRow(&c, wxConsoleRow, &cc);
}
