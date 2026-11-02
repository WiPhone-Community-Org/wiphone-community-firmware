/*
 * tz_auto.h - THE CLOCK FOLLOWS THE OWNER ACROSS TIME ZONES: the setting (Auto / a named zone /
 * a custom offset), the Auto decision, the offset the clock should have at any instant, and the
 * words for Settings and the serial `tz`. Pure: no Arduino, no NVS, no logging, no heap; the glue
 * (tz_service.cpp) keeps the state, saves it, applies the offset and tells the user. Proven on
 * the Mac by tests/test_tzauto.cpp.
 *
 * WHY. Until 0.9.82 the clock was a fixed number typed in Settings, with no DST rules. Carried
 * across the country, the phone kept the old zone's time, and the Almanac's sunrise, sunset and
 * legal light came out hours off. Now the zone comes from a MEASURED position - the woods plate's GPS
 * or the hotspot phone's GPS (self_pos.h) - looked up in tz_grid.h, and DST changes by itself.
 *
 * 🛑 THE OWNER'S RULE: A WRONG ANSWER IS WORSE THAN NONE. So Auto changes NOTHING unless every
 * one of these holds; otherwise the zone in use (or, before any, the typed offset) stays:
 *   1. A LIVE fix by the 30 s bar beaconing uses (SELF_TX_FRESH_MS), GPS ranked first. Never a
 *      bench fix (`pos inject`, `phonepos at`): SELF_ST_INJECTED. Never a POOR one (sats < 4 or
 *      HDOP > 10, the bar every screen uses). Never the Wi-Fi slot (SELF_NET: tzaPickFix asks
 *      GPS and PHONE only, however live and sendable a Wi-Fi fix is) and never an IP guess -
 *      there is none anywhere in this firmware (self_pos.h).
 *   2. The grid answers a ZONE: not TZ_BORDER (within one cell, >= 5.6 km N-S, of a change of
 *      clock), not TZ_NOT_COVERED (sea, outside the grid, Cuba and Greenland's rules).
 *   3. ⚠ EVERY PHONE FIX COUNTS ONLY WELL INSIDE A ZONE - rated or not. A phone fix can be
 *      kilometres off with nothing in its sentence to say so (self_pos.h):
 *        - UNRATED (GPS 2 IP on an iPhone: no satellites, no HDOP - "shown, not sent"): a
 *          CoreLocation position, which can be a Wi-Fi/cell guess, or kilometres off on purpose
 *          with Precise Location off;
 *        - RATED but not measured as rated: GPS 2 IP's stand-in 8 sats / HDOP 0.9 is only known
 *          for what it is at once on port 11123 - on any other port a connection's first few
 *          GGAs are LIVE and rated, and with "Disable when Static" the last of them can sit in
 *          the slot for the whole 30 s bar;
 *        - RATED and wrong: gpsdRelay's GENERATED GGA (quality 1, 12 sats, HDOP <= 10) is 1-9
 *          arc-minutes (1.9-17 km) off within ~1.85 m of a whole minute, and a phone standing
 *          still in that strip repeats the same glitch every second, so selfPhoneKeep's "two in
 *          a row agree" and this module's two looks both pass it.
 *      For the CLOCK a town-sized error changes nothing - the zone is the same for kilometres
 *      around - except near a zone line, where it can put the phone on the wrong side. So a
 *      SELF_PHONE fix answers a zone only when EVERY grid cell within TZA_PHONE_MARGIN_M
 *      (10 km) of it is a zone with the same clock (tzGridLookupMargin); else PHONE_NEAR,
 *      nothing changes. Every answered cell is itself at least one cell from any change of
 *      clock, so a phone fix is used only 10 km + one cell (1.7-5.6 km) or more from the
 *      nearest one. 10 km is a judgement, and it was measured (test_tzauto): 15 km also refused
 *      Honolulu, Hilo, San Juan, Bermuda, Nome and Mazatlan - the edge of territorial waters is
 *      a refusal too, and a coastal city's box reached it - while 10 km keeps every coastal
 *      capital and still refuses the places near a real line (Knox IN, the Hopi villages,
 *      Victoria BC across Haro Strait, Atikokan, Dawson Creek).
 *      ⚠ ACCEPTED, NOT COVERED (review, 2026-10-10): gpsdRelay's generation glitch reaches
 *      17 km and the margin covers 11.7-15.6 km. Left open because it needs all of: generation
 *      ON (phone-location.md says to relay the receiver's own NMEA instead, and moving with it
 *      shows "fixes jump about"), standing still inside a ~3.7 m strip, 11.7-17 km from a change
 *      of clock, the glitch pointing across it - while a 17 km margin would refuse every phone
 *      fix in the coastal places above, for every phone app, all the time.
 *      The woods plate's GPS (SELF_GPS) is this phone's own receiver, metres off: the grid's own
 *      one-cell refusal band covers it, so it gets the plain lookup.
 *   4. STABLE: the same new zone (same clock) on two checks at least TZA_CONFIRM_MS (2 min)
 *      apart, with no other answer between them (a different zone, a border, the sea or outside
 *      all start over) and no gap over TZA_CAND_GAP_MS (30 min). The fix bar is 30 s, so the two
 *      checks can never read the same fix: two independent measurements agree.
 * A zone with the SAME clock as the one in use (Detroit -> Toronto, Phoenix -> Vancouver since
 * 2026) is not a change: nothing is switched, saved or announced.
 *
 * THE OFFSET. A zone in use is a standard offset + a rule (TZ_RULE_NONE or TZ_RULE_USCAN), so
 * the offset at any instant is computed (tz_grid.h, from first principles): DST changes happen by
 * themselves, after a reboot too with no fix (the glue saves the zone's IANA name in NVS, and
 * tzaZoneFromSaved maps it back). A rule with DST needs the date: with the clock not known yet,
 * TzEff.ok is false and the glue leaves the offset as it is (nothing shows a time then anyway).
 * An Auto zone also refuses an instant before its sinceUtc (BC/Alberta's 2026 change).
 *
 * THE SETTING. AUTO (the default, an upgraded phone included): the zone Auto found, falling back
 * to the typed offset until one is found - so nothing changes until a measured fix says where the
 * phone is. Choosing Auto again after a named zone or Custom never brings back the zone found
 * before the override (tzaEnterAuto): Auto starts on what the clock already shows - that named
 * zone, or the typed offset - until a measurement finds another clock. NAMED: one of ten zones,
 * each with its rule. CUSTOM: the typed offset, as before, no DST (the Almanac's reminder still
 * says when to change it).
 */
#ifndef TZ_AUTO_H
#define TZ_AUTO_H

#include <stddef.h>
#include <stdint.h>
#include "tz_grid.h"
#include "self_pos.h"

// ---------------------------------------------------------------- the setting
enum TzMode : uint8_t {
  TZ_MODE_AUTO   = 0,     // from a measured position (the default)
  TZ_MODE_NAMED  = 1,     // one of tzNamed()
  TZ_MODE_CUSTOM = 2,     // the typed offset, no DST
};

struct TzNamed {
  const char* key;        // "pacific": serial `tz pacific`, and what NVS keeps
  const char* display;    // "Pacific": Settings
  const char* iana;       // the zone it stands for (test_tzauto checks it against the grid)
  int16_t stdMinutes;     // minutes east of UTC
  uint8_t rule;           // TZ_RULE_NONE / TZ_RULE_USCAN
};
#define TZ_NAMED_COUNT 10
const TzNamed* tzNamed(int i);                // NULL out of range
int tzNamedFind(const char* name);            // key or display, any case; -1

/* A typed offset: every real one is within -12:00..+14:00. */
#define TZ_CUSTOM_MIN_MINUTES  (-720)
#define TZ_CUSTOM_MAX_MINUTES    840
bool tzCustomOk(int minutes);
/* Clock::parseTimeZone's hours (-7, -3.5, 5.75) to whole minutes, rounded to the nearest. */
int  tzHoursToMinutes(float hours);

/* Settings' one row: 0 = Auto, 1..TZ_NAMED_COUNT = a named zone, then Custom. */
#define TZ_CHOICE_AUTO    0
#define TZ_CHOICE_CUSTOM  (TZ_NAMED_COUNT + 1)
#define TZ_CHOICE_COUNT   (TZ_NAMED_COUNT + 2)
int  tzChoiceOf(uint8_t mode, int namedIdx);
void tzChoiceTo(int choice, uint8_t* mode, int* namedIdx);
const char* tzChoiceName(int choice);         // "Auto", "Pacific", ..., "Custom"

// ---------------------------------------------------------------- the Auto decision
#define TZA_CHECK_MS          30000UL     // the glue looks this often
#define TZA_CONFIRM_MS       120000UL     // the same new zone on two checks this far apart
#define TZA_CAND_GAP_MS     1800000UL     // a new zone not seen again for 30 min is forgotten
#define TZA_PHONE_MARGIN_M      10000     // every PHONE fix: this far from any change of clock

enum TzaWhy : uint8_t {
  TZA_WHY_NONE = 0,       // not checked yet
  TZA_WHY_MANUAL,         // the setting is not Auto: nothing looked at
  TZA_WHY_NO_FIX,         // no source on, or none has a fix
  TZA_WHY_OLD,            // the newest fix is older than 30 s
  TZA_WHY_POOR,           // its own sentence said sats < 4 or HDOP > 10
  TZA_WHY_BENCH,          // a bench fix (`pos inject`, `phonepos at`): never for the clock
  TZA_WHY_BORDER,         // the grid: within a cell of a change of clock
  TZA_WHY_WATER,          // the grid: at sea
  TZA_WHY_OUTSIDE,        // the grid: outside it
  TZA_WHY_UNKNOWN_RULE,   // the grid: a zone whose rules are not modelled
  TZA_WHY_BAD_INPUT,      // no valid position
  TZA_WHY_PHONE_NEAR,     // a phone fix (rated or not) within 10 km of a change of clock
  TZA_WHY_SAME,           // the zone in use, confirmed
  TZA_WHY_WAITING,        // a new zone seen: waiting for the second look
  TZA_WHY_SWITCHED,       // changed to a new zone
};
const char* tzaWhyName(int why);              // short, for the serial `tz`: "border", "waiting"

struct TzAuto {
  // The zone in use. The glue saves both in NVS on a switch and hands them back to tzaInit.
  int16_t  zone;          // grid index (tz_grid.h), -1 = none found yet
  uint8_t  zoneSrc;       // SELF_GPS / SELF_PHONE: the source that set it; TZA_SRC_HAND: the
                          // named zone Auto started on (tzaEnterAuto)
  // A new zone seen, waiting to be confirmed (RAM only):
  int16_t  cand;          // -1 = none
  uint8_t  candSrc;
  uint32_t candFirstMs;   // millis() it was first seen
  uint32_t candLastMs;    // ...and last seen
  // The last check (the serial `tz`, Settings' second line):
  uint8_t  why;           // TZA_WHY_*
  uint32_t whyMs;         // millis() of it; 0 = never (a stamp of 0 is written as 1)
  int16_t  lastLookup;    // that check's answer: zone, TZ_BORDER or TZ_NOT_COVERED (-3 = none)
  uint8_t  lastSrc;       // SELF_* of that check's fix (SELF_NONE = none)
  bool     lastUnrated;   // ...it was an UNRATED phone fix (no accuracy given; every phone fix,
                          //    this one included, had the 10 km margin)
  uint32_t checks;        // since boot
  uint32_t switches;      // since boot
};
#define TZA_LOOKUP_NONE (-3)

/* TzAuto.zoneSrc for a zone Auto did not FIND but STARTED ON: the named zone in force when the
 * setting changed into Auto (tzaEnterAuto). Not a SelfSrc - no fix is behind it - and the words
 * say so ("set by hand"), never "from phone GPS". Saved in NVS like the others. */
#define TZA_SRC_HAND 8

/* Boot: the remembered zone (tzaZoneFromSaved), or -1; zoneSrc SELF_GPS, SELF_PHONE or
 * TZA_SRC_HAND (anything else reads as SELF_NONE). */
void tzaInit(TzAuto* a, int zone, int zoneSrc);
/* `tz forget`: drop the zone Auto found and any zone waiting to be confirmed - Auto falls back
 * to the typed offset until two fresh looks find one. The counters (checks, switches) and the
 * last look are kept. */
void tzaForget(TzAuto* a);
/* The setting changed INTO Auto (review, 2026-10-10). While it was a named zone or Custom nothing
 * looked at a fix, so the zone Auto had found says where the phone WAS - perhaps weeks and states
 * ago - and putting it back on the clock would be a wrong answer with no measurement behind it,
 * shown as "From phone GPS" (found at home, picked Eastern by hand in Ohio, then Auto: the clock
 * jumped back to Pacific, 3 h wrong, until a fix). It is dropped. What the clock shows instead
 * is what it showed a moment before - the owner's "change nothing":
 *   from a NAMED zone   Auto starts on that zone (zoneSrc TZA_SRC_HAND): the same offset, the
 *                       same DST rule, until two looks find another clock (then a switch and its
 *                       banner, as always). A same-clock look leaves it be.
 *   from CUSTOM         no zone: the typed offset, which is what Custom was showing.
 * fromNamed = the tzNamed() index in force (used only when fromMode is TZ_MODE_NAMED). Only a
 * reboot IN Auto keeps a found zone: nobody has said the phone moved. */
void tzaEnterAuto(TzAuto* a, uint8_t fromMode, int fromNamed);
/* The saved IANA name back to a grid index: -1 for NULL/"", a name the grid does not have (a
 * regenerated grid dropped it), or a zone whose rules are not modelled. -1 means "none found
 * yet": the typed offset, never a guess. */
int  tzaZoneFromSaved(const char* iana);

/* The fix the clock may use: GPS first, then the phone; LIVE by the 30 s bar or UNRATED (out->
 * state says which). SELF_NONE when none; *why (may be NULL) then says the most telling refusal
 * among the sources that are on (BENCH > POOR > OLD > NO_FIX). */
int  tzaPickFix(const SelfSlot slots[SELF_SRC_COUNT], uint32_t nowMs, SelfFix* out, uint8_t* why);

/* The grid's answer for every cell within marginM metres of the point: the point's zone when all
 * of them are zones keeping the same clock, else TZ_BORDER (why = TZ_WHY_BORDER) - or the point's
 * own refusal when the point itself is not a zone. marginM <= 0 = the plain lookup. */
int  tzGridLookupMargin(int32_t latE7, int32_t lonE7, int32_t marginM, uint8_t* why);

/* The lookup this module trusts for one fix: plain for the woods plate's GPS, the 10 km margin
 * for every PHONE fix (rule 3). *why (may be NULL) = TZA_WHY_* (a zone: TZA_WHY_NONE). */
int  tzaLookup(const SelfFix* fx, uint8_t* why);

/* One answer into the decision: `zone` >= 0, or < 0 with the refusal `why` (TZA_WHY_*). Returns
 * true when a->zone changed: the glue saves it, applies the offset and tells the user once. */
bool tzaDecide(TzAuto* a, int zone, uint8_t why, int src, uint32_t nowMs);

/* One check: the setting, the fix, the lookup, the decision. False (and nothing looked at) when
 * mode is not Auto - a waiting zone is dropped then. */
bool tzaStep(TzAuto* a, uint8_t mode, const SelfSlot slots[SELF_SRC_COUNT], uint32_t nowMs);

// ---------------------------------------------------------------- the offset
enum TzKind : uint8_t {
  TZK_CUSTOM = 0,         // the typed offset (Custom)
  TZK_NAMED,              // a named zone
  TZK_AUTO,               // Auto, with the zone it found
  TZK_AUTO_WAIT,          // Auto, no zone found yet: the typed offset
};

struct TzEff {
  uint8_t     kind;       // TzKind
  bool        ok;         // `minutes` is the offset now; false = leave the clock as it is
  int         minutes;    // minutes east of UTC
  int         stdMinutes; // the zone's standard offset (= minutes for a typed offset)
  uint8_t     rule;       // TZ_RULE_NONE for a typed offset
  bool        dst;        // in daylight time now (ok only)
  const char* name;       // "Eastern", "Mountain, no DST"; NULL for a typed offset
  const char* iana;       // NULL for a typed offset
  int64_t     next;       // UTC s of the next change of offset; 0 = none, or not known
  int         nextMinutes;
};

/* The offset the clock should have at utcNow. clockKnown false: a zone with DST is not ok (the
 * date is unknown); a typed offset or a no-DST zone still is. An Auto zone before its sinceUtc is
 * not ok. next/nextMinutes are filled when ok. */
void tzEffective(uint8_t mode, int namedIdx, int autoZone, int customMinutes, bool clockKnown,
                 int64_t utcNow, TzEff* out);

// ---------------------------------------------------------------- the words
/* "UTC-4", "UTC+5:30", "UTC-3:30", "UTC" (minutes east of UTC). */
size_t tzFmtOffset(int minutes, char* out, size_t cap);
const char* tzSrcWords(int selfSrc);          // "from GPS", "from phone GPS", "set by hand"
                                              // (TZA_SRC_HAND), "" otherwise

/* One line: the serial `tz` head and the log.
 *   Auto, a zone:   "Eastern, UTC-4, from phone GPS"
 *   Auto, waiting:  "No zone yet: UTC-7 (typed)"
 *   Named:          "Pacific, UTC-7, set by hand"
 *   Custom:         "UTC-7 custom"
 *   a DST zone, clock unknown: "Eastern, clock not set" */
size_t tzStatusText(const TzEff* e, int zoneSrc, char* out, size_t cap);
/* The same in Settings' two lines (Akrobat Bold 16 on 228 px, measured by the test: the one-line
 * form is 263 px for "Newfoundland, UTC-3:30, from phone GPS"):
 *   tzZoneText:   "Eastern, UTC-4"  "Mountain, no DST, UTC-7"  "UTC-7 custom"  "No zone yet: UTC-7"
 *                 "Eastern: clock not set"
 *   tzSourceText: "From phone GPS"  "From GPS"  "Set by hand"  "Typed offset, no DST"
 *                 "Typed offset until a fix"  "Set by hand, Auto checks it" (Auto on the named
 *                 zone it started on, TZA_SRC_HAND) */
size_t tzZoneText(const TzEff* e, char* out, size_t cap);
size_t tzSourceText(const TzEff* e, int zoneSrc, char* out, size_t cap);
/* The Auto line: what the last check found ("Auto: near a time zone line", "Auto: checking
 * Central", "Auto: confirmed by phone GPS"). Settings shows it in Auto only. */
size_t tzaWhyText(const TzAuto* a, char* out, size_t cap);
/* The next change: "DST ends Sun Nov 1 (UTC-5)", "DST starts Sun Mar 14 (UTC-4)", "No DST: the
 * offset never changes", "Custom: change it by hand"; "" for a DST zone with no clock. */
size_t tzNextText(const TzEff* e, char* out, size_t cap);
/* The hint under Settings' choice, for the choice being looked at (not saved yet):
 *   Auto "From GPS or the phone's GPS"; Pacific "UTC-7 now, DST by itself" (clock known) or
 *   "UTC-8, DST by itself"; Arizona "UTC-7 all year, no DST"; Custom "The offset below, no DST". */
size_t tzChoiceHint(int choice, bool clockKnown, int64_t utcNow, char* out, size_t cap);

/* The one notice a zone change earns (WiPhone.ino's banner, GUI::showMeshPopup: the title in
 * Akrobat Bold 18 and the body in Bold 16, each on 212 px - measured by the test):
 *   "Time zone: Eastern" / "Now UTC-4, from phone GPS" ("Clock not set yet" without one) */
void tzNoticeText(int zone, const TzEff* e, int zoneSrc, char* title, size_t titleCap, char* body,
                  size_t bodyCap);

#endif // TZ_AUTO_H
