/*
 * almanac_lines.h - what the Almanac SAYS, line by line, for every screen: TODAY, SUN, MOON,
 * SOLUNAR, POSITION, DATE and SETTINGS (docs/almanac.md, "On the phone") - and, since 0.9.81,
 * WEATHER, whose rows are weather_lines.cpp's (the same emitter, the same measuring).
 *
 * The phone's Almanac (app_almanac.cpp) turns each line into a menu row; the serial `almanac`
 * command prints the SAME lines, one say() each. So the bench proof against PyEphem is a proof
 * of the screen, not of a second formatter that happens to agree with it.
 *
 * Two halves:
 *   - AlmDay: the day's heavy tables (astroSunDay / astroMoonDay / astroSolunar, and on demand
 *     the day before and after and the next four quarters). ~5,600 libm calls for the core,
 *     ~11,000 with everything: software double on the ESP32, 148 ms for the core on phone 2 at
 *     160 MHz. The app computes them ONCE per (day, place, offset), IN SLICES on its timer
 *     (almDayWorkRun below: no pass over ~30 ms of it), and keeps them.
 *   - the line builders: cheap. They read an AlmDay and a context (now, the UTC offset, the
 *     place, the settings) and emit rows; a part not computed yet is a "Computing..." row. The
 *     only astronomy they do themselves is "now" - the sun's position (~27 libm calls, ~0.7 ms on
 *     the phone), the moon's (~215, ~6 ms), the moon's phase at one instant without a search
 *     (astroMoonPhaseWith, ~215) - and POSITION's declination (~180) and grid (~60). No builder
 *     searches or scans when the context says so (AlmCtx.noSearch: the app's), which is what
 *     keeps the minute tick and a key press cheap.
 *
 * Rounding, which is a LEGAL question here, not a cosmetic one: sun and moon times are rounded
 * to the NEAREST minute (the almanac convention). Legal light is rounded INWARD - first legal
 * light UP, last legal light DOWN, and the countdowns likewise ("in" rounds up, "left" rounds
 * down) - so the window this phone shows is never wider than the true one. On a day the true
 * first light is 06:33:20 the screen says 06:34, not 06:33.
 *
 * Pure: no Arduino/ESP-IDF headers, no heap, no static mutable state; stack frames stay small
 * (a few 48-byte line buffers). tests/test_almanac_lines.cpp proves the wordings and measures
 * every row against the phone's own font (AKROBAT_BOLD_20, 232 px) - and every wrapped sentence
 * (ALM_ROW_WRAP: a waypoint's name, the DST notes) broken as the app breaks it, whole, at 220 px.
 */
#ifndef ALMANAC_LINES_H
#define ALMANAC_LINES_H

#include <stddef.h>
#include <stdint.h>
#include "astro.h"

#define ALM_DAY_OFFSET_MAX   365        // Left/Right reach a year either way
#define ALM_YEAR_MIN         1970       // the old sun_times.cpp's range, kept explicitly: outside it
#define ALM_YEAR_MAX         2099       // the screen says "Date out of range" instead of a table

/* What kind of row a builder hands the emitter. Entry and setting kinds ARE the menu keys the
 * app uses (never 0; unique within a screen). */
enum {
  ALM_ROW_INFO = 1,            // one display-only line, measured to fit 232 px of Akrobat Bold 20
  ALM_ROW_WRAP = 2,            // a display-only sentence the screen breaks into rows (serial: 1 line)
  /* (0.9.81) A weather ALERT: a sentence broken into rows as ALM_ROW_WRAP is, every row drawn in
   * the alert's colour - yellow for Moderate/Minor (and an alert of unknown severity), red for
   * Severe/Extreme (app_almanac.cpp: a wrapped alert keeps its colour on every row). */
  ALM_ROW_WARN = 3,
  ALM_ROW_DANGER = 4,
  ALM_ENTRY_SUN = 101,         // "Sun..." - OK opens it
  ALM_ENTRY_MOON = 102,
  ALM_ENTRY_SOLUNAR = 103,
  ALM_ENTRY_POSITION = 104,
  ALM_ENTRY_DATE = 105,
  ALM_ENTRY_SETTINGS = 106,
  ALM_ENTRY_WEATHER = 107,     // "Weather..." (0.9.81)
  ALM_SET_LEGAL = 111,         // Settings: OK toggles the legal-light rule
  ALM_SET_UNITS = 112,         // Settings: OK toggles metric / US
  ALM_SET_DST = 113,           // Settings: OK toggles "US daylight saving" (the reminder, the shift)
};
typedef void (*AlmEmitFn)(void* ctx, int kind, const char* text);

/* Where the place came from (MeshtasticService::resolveReference's four answers, the two
 * fallbacks almPickPlace adds, and the serial command's typed coordinates). */
enum {
  ALM_PLACE_NONE = 0,
  ALM_PLACE_GPS,               // a fresh GPS fix
  ALM_PLACE_LAST_GPS,          // a stale fix of this boot, however old ("last GPS 2h 5m ago")
  ALM_PLACE_PIN,               // this phone's own pin ("me")
  ALM_PLACE_WAYPOINT,          // a waypoint chosen in Meshtastic > Places
  ALM_PLACE_GIVEN,             // typed on the serial console
  ALM_PLACE_MAP_VIEW,          // the Maps app's saved view (NVS maps/lat,lon): nothing better known
  ALM_PLACE_PHONE,             // a fresh fix from the hotspot phone's GPS (0.9.82, self_pos.h)
  ALM_PLACE_LAST_PHONE,        // ...a stale one ("last phone 5m ago")
  ALM_PLACE_NET,               // a live Wi-Fi location (0.9.82, geoloc.h), named with its size
                               // ("Wi-Fi +/-60 m"); never a "last" one - an expired fix is gone
};

/* WHICH PLACE (measured on phone 2, 2026-09-27: no fix since a reboot and no pin, so every
 * screen said "No place known yet" - COVEY's Almanac falls back instead). In order:
 *   1. resolveReference() - a fresh GPS fix, a chosen waypoint, the pin, or the explicit GPS
 *      reference with a stale fix - as every other screen of the phone uses;
 *   2. the newest USABLE fix of this boot, however old: the receiver's ("last GPS 2h 5m ago")
 *      or the hotspot phone's ("last phone GPS") - each judged by its OWN sats/HDOP, so a fresh
 *      poor phone fix never hides an older good GPS one (review 2026-10-10);
 *   3. the map's saved view ("map view") - where the user last looked;
 *   4. nothing: the guidance rows.
 * An almanac for a place a few km off is still right to the minute (1 km moves sunrise ~4 s),
 * and every screen names which place it is. A place off the globe, or exactly 0,0 (the map's
 * "nothing to show" view), is skipped. */
struct AlmPlaceIn {
  bool        refOk;           // resolveReference() answered...
  int         refKind;         // ...as ALM_PLACE_GPS / LAST_GPS / PIN / WAYPOINT / PHONE / LAST_PHONE / NET
  int32_t     refLatI, refLonI;
  const char* refName;         // its name ("GPS", "last GPS", "me", "Camp", "phone", "Wi-Fi +/-60 m")
  uint32_t    refAgeMs;        // PHONE / LAST_PHONE / NET: that fix's age (the GPS kinds use gpsAgeMs)
  bool        gpsFix;          // getGpsFix(): a fix this boot...
  int32_t     gpsLatI, gpsLonI;
  uint32_t    gpsAgeMs;        // ...and its age - ALWAYS the receiver's own (GPS / LAST_GPS age)
  int         gpsSats, gpsHdopX10;   // ...and the receiver's sats / HDOP x10 (-1 = not said)
  bool        phoneFix;        // the hotspot phone's fix of this boot (getSourceFix(SELF_PHONE))...
  int32_t     phoneLatI, phoneLonI;
  uint32_t    phoneAgeMs;      // ...its age
  int         phoneSats, phoneHdopX10;   // ...and ITS sats / HDOP x10, from its own sentence
  bool        viewOk;          // the map's saved view (NVS maps/saved == 1)...
  int32_t     viewLatI, viewLonI;
};
/* Step 2 takes a fix only when meshPosFixUsable(its sats, its HDOP) - the bar resolveReference()
 * and the map's "me" use (review 2026-09-27: a 3-satellite fix measured 20 km off was shown as
 * "At last GPS 0s ago"). Of the two that pass, the newer; a tie keeps the GPS (it outranks the
 * phone everywhere else). A poor fix is no place; the map's view, or the guidance, is next. */
struct AlmPlace {
  int      kind;               // ALM_PLACE_*
  int32_t  latI, lonI;         // 1e-7 deg
  uint32_t ageMs;              // LAST_GPS / PHONE / LAST_PHONE: the fix's age
  char     name[24];
};
void almPickPlace(const AlmPlaceIn* in, AlmPlace* out);

/* "The same place" for a day's tables kept in a cache: within ALM_SAME_PLACE_DEG on each axis of
 * where they were computed (0.001 deg: ~110 m of latitude, ~75 m of longitude at 47 N; sunrise
 * moves ~0.4 s). A tolerance around the KEYED place, never a grid and never exact doubles (review
 * 2026-09-27): a 0.001-degree grid re-keyed the whole day (~148 ms of CPU) whenever GPS noise
 * crossed a line, and the Meshtastic Sun screen's exact-double key never matched a live fix, so
 * every open ran a ~49 ms astroSunDay in its key handler. */
#define ALM_SAME_PLACE_DEG 0.001
bool almSamePlace(double lat1, double lon1, double lat2, double lon2);

/* One local day's tables. t0 = local midnight (UNIX s) under tzS; lat/lon the place they were
 * computed for. The lazy parts carry their own have* flag; almDayEnsure fills them, and the
 * app's slices (almDayWorkRun) fill every part, a piece at a time. */
struct AlmDay {
  bool         valid;          // the core below (sun + moon + solunar) is all there
  int          have;           // ALM_PART_* there (a day whose slices are still running is partial)
  int64_t      t0;
  int          tzS;
  double       lat, lon;
  AstroSunDay  sun;
  AstroMoonDay moon;
  AstroSolunar sol;
  bool         havePrev;       // the day before (day-length change)
  AstroSunDay  prev;
  bool         haveNext;       // the day after (first light after today's dark)
  AstroSunDay  next;
  bool         havePhases;     // the next new / first quarter / full / last quarter...
  int64_t      phaseFrom;      // ...strictly after this instant
  int64_t      phase[4];
};

enum {
  ALM_NEED_PREV = 1, ALM_NEED_NEXT = 2, ALM_NEED_PHASES = 4,   // the lazy parts
  ALM_PART_SUN = 8, ALM_PART_MOON = 16, ALM_PART_SOL = 32,     // the core
  ALM_PART_AGE = 64,           // AlmMoonAge (not the day's: the new moons either side of now)
};
#define ALM_PART_CORE (ALM_PART_SUN | ALM_PART_MOON | ALM_PART_SOL)

/* The core: sunrise..dusk, the moon's four events, the solunar periods. Clears the lazy parts. */
void almDayCompute(AlmDay* d, int64_t t0, int tzS, double lat, double lon);
/* Fill what `needs` asks for and is missing. Phases are kept while no quarter lies in
 * (phaseFrom, phaseFrom'] - "next strictly after" gives the same answer - and recomputed when
 * `phaseFrom` moves before the old one or past the earliest quarter held. Returns the ALM_NEED_*
 * bits it actually computed (0 = everything was already there). */
int  almDayEnsure(AlmDay* d, int needs, int64_t phaseFrom);

/* The moon's age without a search a minute (astroMoonPhaseWith): the new moons either side of one
 * instant `from`, prevNew = astroPrevMoonPhase(from, 0) and nextNew = astroNextMoonPhase(from, 0).
 * No new moon lies strictly between them, so for EVERY t in [prevNew, nextNew) - a whole lunation,
 * whatever the day or the place - astroPrevMoonPhase(t, 0) IS prevNew (test_almanac_lines proves
 * it against the search over three lunations). Zeroed = nothing known. */
struct AlmMoonAge {
  int     step;                // 0 = start over at the next call, 1 = prevNew known, 2 = both
  int64_t from, prevNew, nextNew;
};
bool almMoonAgeCovers(const AlmMoonAge* a, int64_t t);
/* ONE search (~650-830 libm calls, ~20 ms on the phone) towards covering `t`: none when it is
 * covered already. True = covered (after this call). */
bool almMoonAgeWork(AlmMoonAge* a, int64_t t);

/* ── The day in slices (the app's timer: no pass may hold the loop for the whole day) ────────
 * The same arithmetic as almDayCompute + almDayEnsure, cut into units - a node or a 5-minute
 * sample of a sun or moon day (astro.h, AstroDayJob), the solunar table, one quarter's search,
 * one new moon's search - and run until more() says stop. BIT-IDENTICAL to the one-call
 * functions however it is cut (test_almanac_lines checks the whole AlmDay after random cuts).
 * A search (~20 ms on the phone) is only ever the FIRST unit of a call: it opens a slice, never
 * stretches one. Plain data: it lives in the app (PSRAM) between ticks. */
struct AlmDayWork {
  int         part;            // the part under way (one ALM_* bit), 0 = none
  int         lastPart;        // the part whose unit ran last (the app's cost accounting only)
  AstroDayJob job;             // ...its sun or moon day
  int         done;            // the parts finished since almDayWorkBegin
  int         quarter;         // ALM_NEED_PHASES under way: the next quarter to search, 0..3
  int64_t     phaseFrom;       // ...strictly after this
  int64_t     q[4];
};
/* What a screen asks the slices for: `parts` (ALM_PART_* | ALM_NEED_*), the instant the lazy
 * phases count from (almPhaseFrom), the instant whose moon age is shown, and moonFirst (the MOON
 * screen: its own rows before the sun's). */
struct AlmWant {
  int     parts;
  int64_t phaseFrom;
  int64_t ageAt;
  bool    moonFirst;
};
/* A new key for `d` (another day, place or offset). keep = only the place moved within the same
 * day and offset: the parts on the screen stay until their recomputed ones replace them (a
 * difference of seconds). Otherwise the day is emptied. */
void almDayWorkBegin(AlmDay* d, AlmDayWork* w, int64_t t0, int tzS, double lat, double lon,
                     bool keep);
/* One slice. The order: SUN, NEXT, PREV, MOON, SOL, AGE, PHASES (moonFirst: MOON, SUN, SOL, AGE,
 * PHASES, NEXT, PREV); what is under way is finished first. SOL implies SUN and MOON. `age` may
 * be NULL (no ALM_PART_AGE then). Returns the bits completed in this call. */
int  almDayWorkRun(AlmDay* d, AlmDayWork* w, AlmMoonAge* age, const AlmWant* want,
                   AstroMoreFn more, void* ctx);
/* The bits of `want` still to do; 0 = the timer may rest. */
int  almDayWorkLeft(const AlmDay* d, const AlmDayWork* w, const AlmMoonAge* age,
                    const AlmWant* want);

/* Local midnight (UNIX s) of the day `dayOffset` days after the local date of `now`. */
int64_t almMidnight(int64_t now, int tzS, int dayOffset);
/* A place the almanac computes for: finite, |lat| <= 90, |lon| <= 180. (The old sun_times.cpp
 * refused |lat| > 89.9; astro.cpp is fine at the pole, so only a place off the globe - a bug
 * upstream - is refused, and it is refused explicitly rather than computed.) */
bool almPlaceOk(double lat, double lon);
/* The local calendar date of t0 + tzS; false (y/m/d still written) outside ALM_YEAR_MIN..MAX. */
bool almDateOf(int64_t t, int tzS, int* y, int* m, int* d, int* wday, int* yday);

struct WxView;                 // weather_lines.h: the weather the screens show (0.9.81)

/* Everything a builder needs besides the AlmDay. */
struct AlmCtx {
  bool          clockKnown;    // ntpClock.isTimeKnown()
  int64_t       now;           // UTC, UNIX s
  int           tzS;           // local - UTC, s
  const char*   clockSrc;      // clockSourceName(): "ntp" / "gps" / "mesh"
  bool          clockMesh;     // the clock came from the mesh: every screen that acts on time says so
  int           dayOffset;     // the shown day, relative to today (0 = today)
  int           placeKind;     // ALM_PLACE_*
  const char*   placeName;     // almPickPlace's name: "GPS", "last GPS", "me", "Camp", "map view"
  uint32_t      placeAgeMs;    // ALM_PLACE_LAST_GPS: the fix's age
  double        lat, lon;      // meaningful when placeKind != ALM_PLACE_NONE
  int           rule;          // ASTRO_LEGAL_30MIN or ASTRO_LEGAL_CIVIL
  int           units;         // UNITS_METRIC or UNITS_US
  bool          usDst;         // Settings "US daylight saving: yes" (NVS wpmesh/almdst, default yes)
  const AlmDay* day;           // the shown day's tables (maybe partial: AlmDay.have); NULL = none
  const int64_t* seasons;      // DATE: the next of each season after `now`; NULL = not computed
  const AlmMoonAge* moonAge;   // MOON's age without a search; NULL = search (the serial command)
  bool          noSearch;      // the app: never search or scan in a builder - a part not in
                               // `day`/`moonAge` is a "Computing..." row (the timer fills it)
  const WxView* wx;            // the weather (cache + fetch state); NULL = no weather rows at all.
                               // TODAY shows an active alert near the top and a summary row from
                               // it; WEATHER is almLinesWeather (weather_lines.h)
  /* The SERIAL console at a MEASURED place (this phone's or the hotspot phone's fix): POSITION
   * prints it to 2 decimals (~1 km) and leaves out the degrees-minutes, UTM and MGRS rows, which
   * name it to the metre. Console captures get pasted into notes in a PUBLIC repo, and with
   * Phone location on by default the `almanac` line would otherwise carry the home position to
   * ~1 m on the first bench at home (review 2026-10-10). The screen keeps full precision. */
  bool          coordsCoarse;
  /* 0.9.82: the clock follows a time zone with RULES (Settings > Time zone: a named zone, or Auto's
   * zone from a measured fix - tz_service.h), so the phone changes its own offset at the zone's
   * DST changes. Then every DST answer here is the ZONE'S RULE (tz_grid.h), exact to the second,
   * not the US guess on a bare offset; and the reminders say the change happens by itself instead
   * of "set Time offset". false (the memset default): a typed offset, everything as before. */
  bool          tzZoned;
  int           tzStdS;        // the zone's standard offset (s)
  int           tzRule;        // TZ_RULE_NONE / TZ_RULE_USCAN
  int64_t       tzSince;       // UTC s the rule took over (BC/Alberta 2026); earlier: as before (0 = always)
  const char*   tzName;        // "Eastern" (the Settings row); may be NULL
};

/* The device facts only the POSITION screen reads (the GPS receiver and the elevation card). */
enum {
  ALM_ELEV_OK = 1,             // elevSampleCard's 1
  ALM_ELEV_NODATA = -1,        // ...its -1: a tile, but no data at the point
  ALM_ELEV_NOTILE = 0,         // ...its 0
  ALM_ELEV_IOERR = 2,          // the card answered with an error (ioErr)
  ALM_ELEV_NOLAYER = 3,        // no /maps/elev on the card at all
  ALM_ELEV_PENDING = 4,        // not read yet (the read runs on the app timer, never in a key handler)
};
struct AlmPos {
  bool     gpsOn;              // the receiver is switched on (meshService.isGpsEnabled())
  bool     gpsHaveFix;         // a fix has been seen this boot
  uint32_t gpsAgeMs;           // its age (the last RMC or GGA that carried it)
  int      sats, hdopX10;      // -1 = unknown
  int      altM;               // the last GGA's altitude, -10000 = unknown
  uint32_t altAgeMs;           // ...the age of THAT GGA (not of the fix: an RMC refreshes the fix)
  int32_t  speedKnX100;        // the last RMC's speed, -1 = unknown
  int32_t  courseX10;          // the last RMC's course, -1 = unknown
  uint32_t motionAgeMs;        // ...the age of THAT RMC (a GGA refreshes the fix, never the motion)
  int      elevState;          // ALM_ELEV_*
  double   elevM;              // valid with ALM_ELEV_OK
  int      elevZ;              // the layer it came from (ELEV_Z or ELEV_Z_COARSE)
};

#define ALM_GPS_MOTION_FRESH_MS  10000u    // a speed older than this (motionAgeMs) is not "now"
#define ALM_GPS_ALT_FRESH_MS     120000u   // = MESH_GPS_FRESH_MS: an altitude (altAgeMs) still shown
#define ALM_MOVING_KNX100        54        // 0.54 kn = 1.0 km/h: below it is "Stationary"

/* The screens, and what each needs (the app's slices run almScreenWant's parts; the serial
 * command asks almDayEnsure for all of it). */
enum {
  ALM_SCREEN_TODAY = 0, ALM_SCREEN_SUN, ALM_SCREEN_MOON, ALM_SCREEN_SOLUNAR,
  ALM_SCREEN_POSITION, ALM_SCREEN_DATE, ALM_SCREEN_SETTINGS,
  ALM_SCREEN_WEATHER,          // 0.9.81 - appended: the values above are the Back table's indexes
  ALM_SCREEN_COUNT
};
/* Everything the open screen shows, for the slices: the core on a day screen (every day screen
 * asks for it, so Back to TODAY finds it done), NEXT once the sun says the countdown needs it,
 * PREV on SUN, PHASES and AGE on MOON (moonFirst). parts 0 on POSITION, DATE, SETTINGS, WEATHER. */
void    almScreenWant(int screen, const AlmCtx* c, AlmWant* out);
int64_t almPhaseFrom(const AlmCtx* c);                 // "next quarter after": now today, else the day's t0
/* The offset the shown day's times are given in, and that day's local midnight - today's offset,
 * except across a US clock change (almDayTz). The app keys its tables on these. */
int     almDayTzS(const AlmCtx* c);
int64_t almDayT0(const AlmCtx* c);

/* The screens. Each emits its rows in order; none allocates or keeps anything. */
void almLinesToday(const AlmCtx* c, AlmEmitFn emit, void* ctx);
void almLinesSun(const AlmCtx* c, AlmEmitFn emit, void* ctx);
void almLinesMoon(const AlmCtx* c, AlmEmitFn emit, void* ctx);
void almLinesSolunar(const AlmCtx* c, AlmEmitFn emit, void* ctx);
void almLinesPosition(const AlmCtx* c, const AlmPos* p, AlmEmitFn emit, void* ctx);
void almLinesDate(const AlmCtx* c, AlmEmitFn emit, void* ctx);
void almLinesSettings(const AlmCtx* c, AlmEmitFn emit, void* ctx);

/* The pieces, public for the tests and the Meshtastic screen / serial `sun`. Every formatter
 * writes the whole string or "" (never a cut-off number). */
enum { ALM_ROUND_NEAREST = 0, ALM_ROUND_UP = 1, ALM_ROUND_DOWN = 2 };
void almFmtClock(int64_t t, int tzS, int round, char* out, size_t cap);     // "06:33"; t == 0: "--:--"
void almFmtSpan(int64_t secs, int round, char* out, size_t cap);           // "5h 02m", "42m" (secs >= 0)
void almFmtDelta(int64_t secs, char* out, size_t cap);                     // "+2m 51s", "-2m", "+51s", "same"
void almFmtDate(int64_t t, int tzS, char* out, size_t cap);                // "Sun Sep 27"
void almFmtUtcOffset(int tzS, char* out, size_t cap);                      // "UTC-7", "UTC+5:30", "UTC"
const char* almCompass16(double deg);                                      // "N", "NNE", ... "NNW"
/* The countdown for the first line of TODAY (today only). first/last = legal light (0 = none);
 * nextFirst = tomorrow's first legal light (0 = none or not computed); allDay = no bound today
 * because the sun stays above the rule's threshold all day (else: below it all day). */
void almCountdown(int64_t now, int64_t first, int64_t last, int64_t nextFirst, bool allDay,
                  int tzS, char* out, size_t cap);
/* THE countdown from a day's tables - the ONE computation behind the Almanac's TODAY, the
 * Meshtastic Sun screen and serial `sun` (app_almanac.cpp: almanacLegalToday). `today` is
 * astroSunDay(t0, lat, lon); `tomorrow` is astroSunDay(t0 + 86400, lat, lon), or NULL when it was
 * not computed - it is read only when almCountdownNeedsNext says so, and then it must be there. */
bool almCountdownNeedsNext(int64_t now, int64_t first, int64_t last);   // today's light has ended
void almCountdownDay(int64_t now, int tzS, int rule, int64_t t0, double lat, double lon,
                     const AstroSunDay* today, const AstroSunDay* tomorrow, char* out, size_t cap);
/* The US daylight-saving reminder (DATE). The phone's clock is a fixed offset it cannot interpret
 * (-8 on Nov 1 is Pacific done or Alaska not yet; -7 is Pacific or Arizona), so it does NO
 * arithmetic on it (review 2026-09-27: "set Time offset to -9" to a phone already changed to -8):
 *   within 21 days before:  "US clocks go back 1 h Sun Nov 1 - set Time offset then"
 *                           ("go forward" in March)
 *   on the day, from 2 AM:  "US clocks went back 1 h at 2 AM today - check Time offset"
 *   ...before 2 AM:         "US clocks go back 1 h at 2 AM today - set Time offset then"
 * Only with usDst (Settings "US daylight saving: yes", the default; "no" for Hawaii and Arizona,
 * which do not change) and a whole-hour offset a changing US zone can have: before the change
 * its old side's (daylight -4..-9 before November, standard -5..-10 before March), on the day
 * either (-4..-10). False (out = "") otherwise. */
bool almDstReminder(int64_t now, int tzS, bool usDst, char* out, size_t cap);
/* ON the change day only, almDstReminder's sentence ("US clocks went back 1 h at 2 AM today -
 * check Time offset", or "go back ... set Time offset then" before 2 AM): the day screens carry it
 * (TODAY above its legal-light row, SUN/MOON/SOLUNAR under the place). From 2 AM the phone is
 * TAKEN to have been changed (almDayTz), and an offset alone cannot say whether it was: nothing
 * else on those screens would say so (review 2026-09-27). False (out = "") on any other day. */
bool almDstTodayNote(int64_t now, int tzS, bool usDst, char* out, size_t cap);
/* B2 (review 2026-09-27): stepping Left/Right past a US clock change showed that day's times in
 * TODAY's offset - on Oct 27 (UTC-7) Nov 7 read "Legal 07:31-18:10" while on Nov 7 the clock will
 * read 17:10 at that moment: the planned end of legal light an hour LATE. So the shown day's
 * times are given in the offset it WILL have (or had): today's -1 h across the autumn change,
 * +1 h across the spring one - when usDst, today's offset is one a changing US zone has on today's
 * side of the change (daylight -4..-9, standard -5..-10), and the shown day lies on the other
 * side of the change between them. Both changes crossed (a day ~a year away) is the same side.
 * The phone's clock is never touched.
 * TODAY'S SIDE follows almDstReminder's 2 AM rule (review 2026-09-27, again): on the change day
 * before 2 AM the phone is still on the old side (the reminder says "set Time offset then"), so
 * Nov 2 seen at 01:00 on Nov 1 is in -8 and says so; from 2 AM it is taken to be on the new side
 * already (the reminder, and the day screens, say to check). A SHOWN day is on the side its date
 * is: the change day itself on the new one (all but its first two hours are).
 * Returns the offset (s); *changeDay (may be NULL) = the local day number (days since 1970) of the
 * change crossed, 0 = none. */
int  almDayTz(int64_t now, int tzS, int dayOffset, bool usDst, int64_t* changeDay);
/* The offset the phone's clock will have (or had) at the INSTANT t - today's, moved by the US
 * change(s) between now and t, each at its own 2 AM (review 2026-09-27: MOON on Oct 25 at UTC-7
 * listed "New moon Mon Nov 9 00:02", which the clock will show as Sun Nov 8 23:02). The same
 * conditions as almDayTz (usDst, a whole-hour offset a changing US zone has on today's side);
 * otherwise tzS. MOON's quarters and DATE's next season are written in it. */
int  almTzAt(int64_t now, int tzS, bool usDst, int64_t t);
/* The row that says so, under the date (TODAY) or the place (SUN, MOON, SOLUNAR):
 * "Times in UTC-8 (after the Nov 1 change)", "Times in UTC-7 (before the Nov 1 change)".
 * False (out = "") when the day is in today's offset. */
bool almDayTzNote(int64_t now, int tzS, int dayOffset, bool usDst, char* out, size_t cap);

/* ── A ZONED CLOCK (0.9.82, AlmCtx.tzZoned) ──────────────────────────────────────────────────
 * The offset the clock has at the instant t: the zone's rule when tzZoned (and t is not before
 * tzSince), else almTzAt(now, tzS, usDst, t). Every "the offset THEN" on the screens asks this:
 * MOON's quarters, DATE's season, the weather's fetch time. */
int  almCtxTzAt(const AlmCtx* c, int64_t t);
/* The zone's DST sentences - the phone changes its OWN clock, so nothing says to set anything:
 *   DATE, 1..21 days before:  "Clocks go back 1 h Sun Nov 1 - this phone changes by itself"
 *   on the day, before:       "Clocks go back 1 h at 2 AM today - this phone changes by itself"
 *   on the day, after:        "Clocks went back 1 h at 2 AM today - this phone did it"
 *   ("go forward"/"went forward" in March). False for a zone with no DST, or not zoned. */
bool almZoneDstReminder(const AlmCtx* c, char* out, size_t cap);
bool almZoneDstTodayNote(const AlmCtx* c, char* out, size_t cap);   // the change day only
/* almDayTzNote for a zoned clock: the shown day's offset by the rule, the change between named. */
bool almZoneDayTzNote(const AlmCtx* c, char* out, size_t cap);

/* The header's title for a screen (HeaderWidget::setTitle, AKROBAT_BOLD_18). The day screens
 * carry the shown day's date: TODAY "Sep 27 (Sun)", SUN "Sun: Sep 27", MOON "Moon: Sep 27",
 * SOLUNAR "Solunar: Sep 27" (review 2026-09-27: TODAY's "Sun Sep 27" and SUN's "Sun: Sep 27" differed
 * by a colon on a Sunday, and SOLUNAR alone had none); with no clock "Almanac". The others:
 * "Position & GPS", "Date & seasons", "Settings", "Weather". Every title is <= ALM_TITLE_MAX_W px for every
 * date (test_almanac_lines measures them): the room GUI.cpp's header leaves with WiFi, one kind of
 * unread message and the widest clock (240 - 8 - 6 - (3+25+3 battery, 17+6 WiFi, 19+3 message,
 * 3+38 "00:00") = 109). Both message kinds, or the mute icon, take 7 or 17 px more: the header
 * then ends the title in ".." (guiDrawEllipsized). `now`/`tzS`: the phone's clock and offset. */
#define ALM_TITLE_MAX_W 108
void almTitle(int screen, bool clockKnown, int64_t now, int tzS, int dayOffset, char* out,
              size_t cap);

/* A rebuilt list's first visible row (review 2026-09-27: every rebuild - the minute tick, the 1 Hz
 * POSITION refresh, a part landing - started the new list at the top, so a list scrolled down and
 * partly back jumped): the new selection at the same distance from the top of the screen as the
 * old one was, held to the rows there are. oldSel/oldTop: the old list's selected and first
 * visible row; newSel: the new selection; count rows, `visible` per screen. */
int  almMenuTop(int oldSel, int oldTop, int newSel, int count, int visible);

#endif // ALMANAC_LINES_H
