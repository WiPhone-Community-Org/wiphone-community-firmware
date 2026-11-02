/*
 * tz_service.h - the clock's time zone, on the phone: the glue around tz_auto.h (pure, host-
 * tested). It keeps the setting in NVS ("wptime"), runs Auto's check every TZA_CHECK_MS over
 * meshService's position slots, applies the offset to ntpClock, and holds the one notice a zone
 * change earns. Everything that shows a time reads ntpClock's offset, so a change here reaches
 * the status bar, the Almanac, the weather, message and call times at once, with no reboot.
 *
 * WHERE THINGS LIVE (NVS namespace "wptime"; the INI is SPIFFS, NVS is a few ms):
 *   mode   u8      TZ_MODE_* (absent = this firmware has never run: Auto, migrated below)
 *   named  string  a tzNamed() key ("eastern") for TZ_MODE_NAMED
 *   cust   i32     the typed offset, minutes - ALSO in configs.ini [time] zone (hours), which
 *                  every older firmware and this one's boot read; the two are written together
 *   ini    i32     the offset (minutes) this firmware last saw in configs.ini: written when its
 *                  own INI write SUCCEEDED, and at boot when the INI differs from it. It is how
 *                  the boot tells "typed elsewhere" from "this firmware's INI write failed"
 *   az     string  the zone Auto found, by IANA name ("America/New_York"): DST keeps applying
 *                  after a reboot IN AUTO with no fix (tzaZoneFromSaved maps it back, or
 *                  refuses it). Replaced on any change into Auto (tzaEnterAuto says why)
 *   azsrc  u8      SELF_GPS / SELF_PHONE: which source found it ("from phone GPS");
 *                  TZA_SRC_HAND: the named zone Auto started on ("set by hand")
 *   azat   u32     when, UTC (0 = the clock was not known then)
 * 🛑 NOTHING HERE WRITES THE SPIFFS INI FROM THE LOOP. An automatic change (a zone found, a DST
 * change) touches NVS and RAM only: SPIFFS is ~1.6 s just to open (HANDOFF), a LOOP STALL that
 * drops WiFi and eats keys. The INI keeps the TYPED offset - its meaning since the vendor - and
 * is written only when a person types one (Settings, serial `tz custom <offset>`). An older
 * firmware flashed over this one therefore comes up on the typed offset, exactly as before.
 *
 * THE UPGRADE: the first boot with no "mode" makes it Auto, with the INI's offset as the typed
 * one - so the clock reads exactly what it did until a measured fix says where the phone is.
 * WHICH TYPED OFFSET WINS AT BOOT, when configs.ini and "cust" differ:
 *   the INI = "ini"   nothing touched the file since this firmware last saw it, so "cust" is the
 *                     newer one - this firmware's own INI write failed after NVS took it (both
 *                     paths said "kept in NVS"). "cust" wins; the INI catches up at the next
 *                     typed offset.
 *   the INI != "ini"  (or no "ini" yet) it was typed under another firmware (a downgrade, an
 *                     edit, a flash back): it wins, and becomes "cust" and "ini".
 * Every write of configs.ini here also refreshes its NVS backup (restore()), so a file that will
 * not load comes back with the newest offset, not one the boot would take for an edit.
 */
#ifndef TZ_SERVICE_H
#define TZ_SERVICE_H

#include <stddef.h>
#include <stdint.h>
#include "tz_auto.h"

/* Boot, after WiPhone.ino has read configs.ini's [time] zone (iniHad false: no such key) and set
 * the clock from it. Applies the zone's offset at once when it needs no date (a typed offset,
 * a no-DST zone); a DST zone waits for the clock (tzSvcLoop). */
void tzSvcBegin(bool iniHad, float iniHours);

/* Every loop pass (cheap: one compare when nothing is due). Re-computes the offset once a second
 * and at once when the clock becomes known; runs Auto's check every TZA_CHECK_MS unless `quiet`
 * (a Game Boy game: no NVS write under it; the offset still follows DST). True when the clock's
 * offset changed since the last call - here, or by Settings / the console, which apply at once -
 * so the caller repaints what shows the time. */
bool tzSvcLoop(uint32_t nowMs, bool quiet);

/* The one notice a zone change earns ("Time zone: Eastern" / "UTC-4, from phone GPS"), taken
 * once by whoever can show it (WiPhone.ino's popup, with the screen awake). False when none. */
bool tzSvcTakeNotice(char* title, size_t titleCap, char* body, size_t bodyCap);

// ---- the setting (Settings > Time zone, serial `tz`)
uint8_t tzSvcMode();
int     tzSvcNamed();                 // tzNamed() index, -1 = none
int     tzSvcCustomMinutes();         // the typed offset
/* Auto / a named zone / Custom: saved (NVS) and applied now. Custom uses the typed offset as it
 * is. A change INTO Auto never brings back the zone Auto found before (tzaEnterAuto): it starts
 * on the named zone it came from, or the typed offset, so the clock does not move until two
 * looks find another clock. False for a bad argument. */
bool    tzSvcSetMode(uint8_t mode, int namedIdx);
/* A newly typed offset: NVS "cust", applied now when it is in use. storeIni = also write
 * configs.ini [time] zone (the serial path; Settings writes its own open INI and then calls
 * tzSvcIniStored when that write succeeded). False when storeIni and the INI write failed: the
 * offset is still kept, in NVS, and still wins at the next boot (see THE UPGRADE above). */
bool    tzSvcSetCustomMinutes(int minutes, bool storeIni);
/* configs.ini [time] zone now holds `minutes`: Settings wrote it itself and store() succeeded.
 * NVS "ini" - never call it for a write that failed, or a stale INI would beat NVS at boot. */
void    tzSvcIniStored(int minutes);
/* `tz forget`: drop the zone Auto found or started on (NVS too) - Auto falls back to the typed
 * offset until a fix finds one again. */
void    tzSvcForgetAuto();

// ---- what is in force
void    tzSvcEffective(TzEff* out);   // now, by ntpClock
const TzAuto* tzSvcAuto();
uint32_t tzSvcAutoSetUtc();           // when Auto set its zone (UTC), 0 = unknown
uint32_t tzSvcLastChangeMs();         // millis() the clock's offset last changed here, 0 = never
int      tzSvcLastChangeFrom();       // ...from (minutes); TZ_NO_OFFSET = none
/* The zone in force when it has RULES the Almanac can use (a named zone, or Auto's zone): its
 * standard offset, rule, name, and the instant the rule took over (an Auto zone's sinceUtc, else
 * 0). False for a typed offset (Custom, or Auto with none found). Any out may be NULL. */
bool    tzSvcZoneRules(int* stdMinutes, uint8_t* rule, const char** name, int64_t* sinceUtc);

#endif // TZ_SERVICE_H
