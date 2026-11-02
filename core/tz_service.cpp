/*
 * tz_service.cpp - see tz_service.h. The decisions are tz_auto.cpp's (test_tzauto); this only
 * keeps them, saves them and applies them. Runs on the loop task only (the loop, the Settings
 * app and the serial console all do), so it takes no lock of its own; ntpClock's offset is set
 * under the clock's lock.
 * 🛑 No log line here carries a coordinate - only zone names, offsets and sources.
 */
#include "tz_service.h"
#include "clock.h"
#include "meshtastic_service.h"
#include "Storage.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

static const char NS[] = "wptime";

static uint8_t  s_mode = TZ_MODE_AUTO;
static int      s_named = -1;
static int      s_custom = 0;              // minutes
static uint32_t s_azAtUtc = 0;
static TzAuto   s_auto;
static bool     s_begun = false;

static uint32_t s_applyMs = 0;             // the last once-a-second evaluation
static uint32_t s_checkMs = 0;             // the last Auto check
static bool     s_known = false;           // ntpClock.isTimeKnown() at the last evaluation
static uint32_t s_changeMs = 0;
static int      s_changeFrom = TZ_NO_OFFSET;
/* The offset changed and what shows the time has not been told yet: set by EVERY change (the
 * loop's, and Settings' or the console's, which apply at once from their own call), handed to the
 * loop's repaint by the next tzSvcLoop(). */
static bool     s_repaint = false;

static bool     s_notice = false;
static char     s_noticeTitle[40];
static char     s_noticeBody[48];

#define TZSVC_APPLY_MS 1000UL

// ---------------------------------------------------------------- NVS
static void putU8(const char* key, uint8_t v) {
  Preferences p;
  if (p.begin(NS, false)) {
    p.putUChar(key, v);
    p.end();
  } else {
    log_e("TZ: could not open NVS '%s' to save %s (kept for this boot only)", NS, key);
  }
}

static void putI32(const char* key, int32_t v) {
  Preferences p;
  if (p.begin(NS, false)) {
    p.putInt(key, v);
    p.end();
  } else {
    log_e("TZ: could not open NVS '%s' to save %s (kept for this boot only)", NS, key);
  }
}

static void putStr(const char* key, const char* v) {
  Preferences p;
  if (p.begin(NS, false)) {
    if (v && *v) {
      p.putString(key, v);
    } else {
      p.remove(key);
    }
    p.end();
  } else {
    log_e("TZ: could not open NVS '%s' to save %s (kept for this boot only)", NS, key);
  }
}

static void saveAutoZone() {
  const TzZone* z = tzGridZone(s_auto.zone);
  Preferences p;
  if (!p.begin(NS, false)) {
    log_e("TZ: could not open NVS '%s' to save the zone (kept for this boot only)", NS);
    return;
  }
  if (z) {
    p.putString("az", z->iana);
    p.putUChar("azsrc", s_auto.zoneSrc);
    p.putUInt("azat", s_azAtUtc);
  } else {
    p.remove("az");
    p.remove("azsrc");
    p.remove("azat");
  }
  p.end();
}

/* configs.ini [time] zone, in hours, as TimeConfigApp has always written it. A person's action
 * only (see the header): this is a SPIFFS write. The NVS backup of the file is refreshed too, as
 * every settings screen's destructor does, so a configs.ini that will not load comes back from
 * restore() with this offset rather than an older one (which the boot would take for an offset
 * typed elsewhere - tzSvcBegin). */
static bool storeIniOffset(int minutes) {
  CriticalFile ini(Storage::ConfigsFile);
  if (!(ini.load() || ini.restore())) {
    ini[0]["desc"] = "WiPhone general configs";
    ini[0]["v"] = "1";
  }
  if (!ini.hasSection("time")) {
    ini.addSection("time");
  }
  ini["time"]["zone"] = (float)minutes / 60.0f;
  const bool ok = ini.store();
  ini.backup();
  return ok;
}

/* `tz forget`: drop the zone Auto found, RAM and NVS. No offset applied here. */
static void dropAutoZone() {
  tzaForget(&s_auto);
  s_azAtUtc = 0;
  saveAutoZone();
}

/* The setting changed INTO Auto (tzaEnterAuto: why): the zone found before the override is
 * dropped, and Auto starts on what the clock already shows - the named zone it came from
 * ("set by hand"), or the typed offset - RAM and NVS. No offset applied here. */
static void enterAuto(uint8_t fromMode, int fromNamed) {
  tzaEnterAuto(&s_auto, fromMode, fromNamed);
  s_azAtUtc = (s_auto.zone >= 0 && ntpClock.isTimeKnown()) ? ntpClock.getExactUtcTime() : 0;
  saveAutoZone();
}

// ---------------------------------------------------------------- the offset
static bool clockKnown() {
  return ntpClock.isTimeKnown();
}

void tzSvcEffective(TzEff* out) {
  tzEffective(s_mode, s_named, s_auto.zone, s_custom, clockKnown(),
              (int64_t)ntpClock.getExactUtcTime(), out);
}

/* Apply the offset in force now; true when the clock's offset changed. */
static bool applyNow(uint32_t nowMs, const char* why) {
  TzEff e;
  tzSvcEffective(&e);
  if (!e.ok) {
    return false;                         // a DST zone with no date yet: leave it as it is
  }
  const int was = ntpClock.getOffsetMinutes();
  if (e.minutes == was) {
    return false;
  }
  ntpClock.setOffsetMinutes(e.minutes);
  s_changeMs = nowMs ? nowMs : 1;
  s_changeFrom = was;
  s_repaint = true;
  char a[16], b[16], st[64];
  tzFmtOffset(was, a, sizeof(a));
  tzFmtOffset(e.minutes, b, sizeof(b));
  tzStatusText(&e, s_auto.zoneSrc, st, sizeof(st));
  log_e("TZ: clock offset %s -> %s (%s; %s)", a, b, st, why);
  return true;
}

// ---------------------------------------------------------------- boot
void tzSvcBegin(bool iniHad, float iniHours) {
  int iniMin = iniHad ? tzHoursToMinutes(iniHours) : 0;
  if (!tzCustomOk(iniMin)) {
    log_e("TZ: the typed offset in configs.ini is out of range (%d min): UTC used", iniMin);
    iniMin = 0;
  }
  bool fresh = true;
  char named[24] = "", az[48] = "";
  uint8_t azsrc = SELF_NONE;
  int32_t cust = iniMin;
  bool hasIniRec = false;
  int32_t iniRec = 0;
  Preferences p;
  if (p.begin(NS, true)) {               // read-only begin fails on a never-written namespace
    if (p.isKey("mode")) {
      fresh = false;
      s_mode = p.getUChar("mode", TZ_MODE_AUTO);
      cust = p.getInt("cust", iniMin);
      if (p.isKey("named")) p.getString("named", named, sizeof(named));
      if (p.isKey("az")) p.getString("az", az, sizeof(az));
      azsrc = p.getUChar("azsrc", SELF_NONE);
      s_azAtUtc = p.getUInt("azat", 0);
      hasIniRec = p.isKey("ini");
      iniRec = p.getInt("ini", 0);
    }
    p.end();
  }
  /* The saved mode, before the fallbacks below: a zone Auto found is only kept across a boot that
   * was IN Auto (tzaEnterAuto says why). */
  const uint8_t savedMode = s_mode;
  if (s_mode > TZ_MODE_CUSTOM) {
    s_mode = TZ_MODE_AUTO;
  }
  s_named = tzNamedFind(named);
  if (s_mode == TZ_MODE_NAMED && s_named < 0) {
    log_e("TZ: the named zone '%s' is unknown here: Auto", named);
    s_mode = TZ_MODE_AUTO;
  }
  s_custom = tzCustomOk(cust) ? cust : iniMin;
  if (fresh) {
    /* The upgrade (or a new phone): Auto, with the typed offset standing until a measured fix
     * finds a zone - the clock reads what it read before this firmware. */
    s_mode = TZ_MODE_AUTO;
    s_custom = iniMin;
    putI32("cust", s_custom);
    putU8("mode", s_mode);
    log_e("TZ: first boot with time zones: Auto, typed offset %d min until a fix finds a zone", s_custom);
  } else if (iniHad && iniMin != s_custom) {
    if (hasIniRec && iniRec == iniMin) {
      /* configs.ini still holds the offset this firmware last wrote (or read) there, so nobody
       * changed it since: the newer typed offset is NVS's, and the INI write that should have
       * carried it failed (Settings and `tz custom` said "kept in NVS"). NVS wins - before
       * 2026-10-10 the stale INI did, and the offset the user was told was saved was lost at
       * the next reboot (review). */
      log_e("TZ: configs.ini still has %d min (its write failed): the typed %d min in NVS is kept",
            iniMin, s_custom);
    } else {
      /* The INI changed since this firmware last saw it (or there is no record yet, as on a
       * phone that ran an earlier 0.9.82 build): it was typed elsewhere - an older firmware after
       * a downgrade, or the file edited - and the newest typed offset wins. */
      log_e("TZ: configs.ini's offset (%d min) differs from the saved one (%d min): the INI's is used",
            iniMin, s_custom);
      s_custom = iniMin;
      putI32("cust", s_custom);
    }
  }
  if (iniHad && (!hasIniRec || iniRec != iniMin)) {
    putI32("ini", iniMin);               // what configs.ini holds now, as this firmware saw it
  }
  int zone = tzaZoneFromSaved(az);
  if (az[0] && zone < 0) {
    log_e("TZ: the saved zone '%s' is not in this firmware's grid: forgotten", az);
  }
  if (zone >= 0 && savedMode != TZ_MODE_AUTO) {
    /* The saved setting is not Auto, so nothing has looked at a fix since this zone was found.
     * A named zone or Custom does not use it, and the next change into Auto would drop it
     * (tzSvcSetMode); a named zone this firmware does not know, or a corrupt mode, falls back
     * to Auto above - itself a change INTO Auto, which never brings the old zone back. */
    log_e("TZ: the setting was not Auto: the zone Auto found before it is forgotten");
    zone = -1;
  }
  tzaInit(&s_auto, zone, azsrc);
  if (az[0] && zone < 0) {
    s_azAtUtc = 0;
    saveAutoZone();                      // gone from NVS too, as `tz forget` does
  }
  s_begun = true;
  s_known = clockKnown();
  applyNow(millis(), "boot");
  TzEff e;
  tzSvcEffective(&e);
  char st[64];
  tzStatusText(&e, s_auto.zoneSrc, st, sizeof(st));
  log_e("TZ: %s mode, %s%s", s_mode == TZ_MODE_AUTO ? "Auto" : s_mode == TZ_MODE_NAMED ? "named" : "Custom",
        st, e.ok ? "" : " (offset waits for the clock)");
}

// ---------------------------------------------------------------- the loop
bool tzSvcLoop(uint32_t nowMs, bool quiet) {
  if (!s_begun) {
    return false;
  }
  const bool known = clockKnown();
  if (known != s_known || (uint32_t)(nowMs - s_applyMs) >= TZSVC_APPLY_MS) {
    const bool becameKnown = known && !s_known;
    s_known = known;
    s_applyMs = nowMs;
    applyNow(nowMs, becameKnown ? "the clock is set" : "DST or a zone");
  }
  if (!quiet && (s_checkMs == 0 || (uint32_t)(nowMs - s_checkMs) >= TZA_CHECK_MS)) {
    s_checkMs = nowMs ? nowMs : 1;
    /* The look's own millis(), not the loop pass's `now`: earlier in this same pass
     * phonePosLoop() may have stamped the PHONE slot with a LATER millis(), and the age
     * nowMs - fixMs would wrap to ~4.29e9 - a fix a few ms old read as TZA_WHY_OLD, a look
     * lost (review). Every other reader of the slots (getSelfFix, getSourceFix) does the same. */
    const uint32_t lookMs = millis();
    if (tzaStep(&s_auto, s_mode, meshService.getSelfSlots(), lookMs)) {
      s_azAtUtc = ntpClock.isTimeKnown() ? ntpClock.getExactUtcTime() : 0;
      saveAutoZone();
      TzEff e;
      tzSvcEffective(&e);
      const TzZone* z = tzGridZone(s_auto.zone);
      tzNoticeText(s_auto.zone, &e, s_auto.zoneSrc, s_noticeTitle, sizeof(s_noticeTitle),
                   s_noticeBody, sizeof(s_noticeBody));
      s_notice = true;                    // a newer change replaces one not shown yet
      log_e("TZ: Auto found %s (%s), %s - two looks at least %u s apart", z ? z->display : "?",
            z ? z->iana : "?", tzSrcWords(s_auto.zoneSrc), (unsigned)(TZA_CONFIRM_MS / 1000));
      applyNow(nowMs, "a new zone");
    }
  }
  const bool changed = s_repaint;
  s_repaint = false;
  return changed;
}

bool tzSvcTakeNotice(char* title, size_t titleCap, char* body, size_t bodyCap) {
  if (!s_notice || !title || !body || !titleCap || !bodyCap) {
    return false;
  }
  strlcpy(title, s_noticeTitle, titleCap);
  strlcpy(body, s_noticeBody, bodyCap);
  s_notice = false;
  return true;
}

// ---------------------------------------------------------------- the setting
uint8_t tzSvcMode() { return s_mode; }
int tzSvcNamed() { return s_named; }
int tzSvcCustomMinutes() { return s_custom; }
const TzAuto* tzSvcAuto() { return &s_auto; }
uint32_t tzSvcAutoSetUtc() { return s_azAtUtc; }
uint32_t tzSvcLastChangeMs() { return s_changeMs; }
int tzSvcLastChangeFrom() { return s_changeFrom; }

bool tzSvcSetMode(uint8_t mode, int namedIdx) {
  if (mode > TZ_MODE_CUSTOM || (mode == TZ_MODE_NAMED && !tzNamed(namedIdx))) {
    return false;
  }
  const bool modeChanged = mode != s_mode;
  const uint8_t fromMode = s_mode;
  const int fromNamed = s_named;
  s_mode = mode;
  if (mode == TZ_MODE_NAMED) {
    s_named = namedIdx;
    putStr("named", tzNamed(namedIdx)->key);
  }
  if (modeChanged) {
    putU8("mode", s_mode);
  }
  if (modeChanged && mode == TZ_MODE_AUTO) {
    /* INTO Auto from a named zone or Custom: the zone Auto found before is NOT put back on the
     * clock (review, 2026-10-10). Nothing looked at a fix while the setting was not Auto, so it
     * says where the phone was then - found at home, it jumped a clock set by hand to Eastern
     * in Ohio back to Pacific, 3 h wrong, labelled "From phone GPS", and stayed there with no
     * fix. Nor does the clock fall to the typed offset (it could be the same 3 h wrong): Auto
     * starts on the zone just set by hand (or, from Custom, the typed offset it was showing),
     * so this Save moves nothing, and only two looks that find another clock change it. */
    enterAuto(fromMode, fromNamed);
  }
  /* Auto's next check comes at once, so its line says something current; a zone waiting to be
   * confirmed was dropped by tzaStep while the setting was not Auto. */
  s_checkMs = 0;
  applyNow(millis(), "the setting");
  return true;
}

bool tzSvcSetCustomMinutes(int minutes, bool storeIni) {
  if (!tzCustomOk(minutes)) {
    return false;
  }
  if (minutes != s_custom) {
    s_custom = minutes;
    putI32("cust", s_custom);
  }
  bool ok = true;
  if (storeIni) {
    ok = storeIniOffset(minutes);
    if (ok) {
      tzSvcIniStored(minutes);
    } else {
      log_e("TZ: configs.ini not written - the typed offset is saved in NVS only (kept at boot)");
    }
  }
  applyNow(millis(), "a typed offset");
  return ok;
}

void tzSvcIniStored(int minutes) {
  putI32("ini", minutes);
}

void tzSvcForgetAuto() {
  dropAutoZone();
  s_checkMs = 0;
  applyNow(millis(), "Auto's zone forgotten");
}

bool tzSvcZoneRules(int* stdMinutes, uint8_t* rule, const char** name, int64_t* sinceUtc) {
  TzEff e;
  tzSvcEffective(&e);
  if (e.kind != TZK_NAMED && e.kind != TZK_AUTO) {
    return false;
  }
  if (stdMinutes) *stdMinutes = e.stdMinutes;
  if (rule) *rule = e.rule;
  if (name) *name = e.name;
  if (sinceUtc) {
    const TzZone* z = e.kind == TZK_AUTO ? tzGridZone(s_auto.zone) : NULL;
    *sinceUtc = z ? z->sinceUtc : 0;
  }
  return true;
}
