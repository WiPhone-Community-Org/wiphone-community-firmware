#include "serial_cmd.h"
#include "app_gbc_xfer.h"
#include "sms_mirror_poll.h"
#include "app_books.h"       // booksDebugDumpPage, the `bookpage` command
#include "music_player.h"    // musicPlayerIsPlaying/Paused: `mv` leaves a loaded track alone
#include "kosync_sync.h"     // the `kosync` command: the window, the home client, the config
#include "phone_pos.h"       // the `phonepos` command: the hotspot phone's GPS (no coordinates)
#include "geoloc.h"          // the `netloc` command: Wi-Fi location's setting and verdict names
#include "geoloc_net.h"      // ...its status, `netloc now`, the bench's canned answers
#include "app_photos.h"      // photosSetWallpaper, the `wallpaper set` command
#include "app_maps.h"        // mapsConsoleStatus/Goto, the `maps` command
#include "elev_sd.h"         // elevSampleCard, the `elev <lat> <lon>` command
#include "elev_text.h"       // elevSampleText: the console's words for one sample
#include "units.h"           // unitsFmtDist/Alt: distances and heights in the chosen units
#include "prefs_almanac.h"   // gUnits, unitsSetPref: the `units` command
#include "tile_fetch.h"      // the `tlstest` bench: TLS from a task with mbedTLS in PSRAM
#include "ai_net.h"          // the `ai` command: Menu > AI from the cable (never the key)
#include "config.h"          // WIPHONE_KEY_*, the `key` command
#include "Storage.h"         // CriticalFile / ConfigsFile, the `lock` command

/* Defined in WiPhone.ino next to keypadBuff — see the note there on why this is a real press. */
extern bool uiInjectKey(char c);
extern void uiKeyBenchHold(uint32_t mask, uint32_t ms, uint32_t blipAtMs);   // WiPhone.ino: `maps hold`, the hold-to-scroll bench
extern uint32_t uiKeyMaskFor(char code);                                    // WiPhone.ino: key code -> keypad bit, for `key hold`
#include "meshtastic_service.h"
#include "t9_extra.h"            // the `t9` command reports the extra dictionary   // applyChannelUrl, the `chan` command
#include "mesh_pos.h"             // distance/bearing for the `pos` command
#include "app_almanac.h"          // almanacLegalToday / almanacConsole / almanacWeatherConsole: `sun`, `almanac`, `wx`
#include "clock.h"                // ntpClock
#include "tz_service.h"           // the `tz` command: Auto / a named zone / the typed offset
#include "GUI.h"                  // gui.state, the `sip` command
#include <SD.h>                   // the `wallpaper` command reads both filesystems
#include "mesh_phy.h"             // meshPhy.benchSleep, the `power lora` toggle
#include "mesh_airtime.h"         // meshLoraAirtimeMs, the `radio` line's prediction
#include "Networks.h"             // wifiState.radioOff(), the `power sleep` message
#include "Hardware.h"             // KEYBOARD_INTERRUPT_PIN, the `power sleep` wake pin
#include "Audio.h"                // audio->isOn(), the `power` guards
#include "rtp_watch.h"            // RTP_ORPHAN_RELEASE_MS, the `audio orphan` bench
#include <driver/i2s.h>           // i2s_stop/i2s_start, the `power i2s` toggle
#include <esp_sleep.h>            // esp_light_sleep_start, the `power sleep` floor
#include <SPIFFS.h>
#include <WiFi.h>

#include <Arduino.h>
#include <driver/uart.h>
#include <esp_heap_caps.h>   // multi_heap_info_t, the `heap` command
#include <esp_phy_init.h>    // esp_phy_erase_cal_data_in_nvs, `wifi calreset`
#include <esp_wifi.h>        // esp_wifi_restore, `wifi restore`
#include <esp_wifi_internal.h>   // the blob's own log level, `wifi log on|off`
#include <esp_log.h>
#include "wifi_diag.h"       // wifiErrName, `wifi scan`
#include "cpu_clock.h"       // the `cpu` command: MHz, PLL, method, switch counts, the cycle bench
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>     // strtoul, the `dm` command's node number
#include <string.h>
#include <strings.h>

#ifdef USER_SERIAL      // (Hardware.h, via GUI.h) — the `gps` command's plumbing
#include "nmea.h"
#include <Preferences.h>
extern bool       gGpsNmea;      // WiPhone.ino: routes the user UART to the NMEA reader
extern uint32_t   gGpsBaud;      // ...at THIS rate; USER_SERIAL_BAUD is the GUI path's
extern NmeaReader gGpsReader;
extern void       gpsApplyBaud(bool gpsOn);            // WiPhone.ino owns the UART object
extern int        gpsRawSnapshot(uint8_t* out, int cap);
#endif

/* ⚠ THE IDF UART API, NOT `Serial`. THIS IS THE WHOLE REASON THE FIRST VERSION READ NOTHING.
 *
 * `setup()` configures UART0 with `uart_param_config()` + `uart_driver_install(UART_NUM_0,
 * ...)` and NEVER CALLS `Serial.begin()`. So the Arduino `HardwareSerial` object is
 * uninitialised: `Serial.available()` always answers 0 and `Serial.read()` never returns a
 * byte, while log output still appears on the port perfectly — because the ESP log path
 * writes to UART0 through the IDF driver, not through `Serial`.
 *
 * That combination is nastily convincing: the port is plainly alive in one direction, so a
 * console built on `Serial` looks like "the phone is ignoring my commands" rather than "this
 * object was never started". Reading through the same driver that owns the port is correct
 * AND unambiguous. The driver is installed with a 2 KB RX ring, so typed input is buffered
 * for us between loop passes.
 */
static const uart_port_t PORT = UART_NUM_0;

/* 512, not 48: `chan <url>` carries a Meshtastic invite URL, and a complete four-channel
 * invite runs ~370 characters. Truncated input is not run (see below), so a small buffer
 * would make the one command this console exists to save you with silently impossible. */
static char     s_buf[512];
static uint16_t s_len = 0;

static void say(const char* fmt, ...) {
  char out[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out, sizeof(out), fmt, ap);
  va_end(ap);
  if (n > 0) {
    uart_write_bytes(PORT, out, (size_t)(n < (int)sizeof(out) ? n : (int)sizeof(out) - 1));
  }
}

static void sayLine(const char* line) {
  say("%s\n", line);
}

/* ── EVERY REBOOT FROM THE CABLE GOES THROUGH HERE ─────────────────────────────────────────
 * 🛑 THE BENCH RUNS ON {user}'S DAILY PHONES. `meshdb cut`, `wifi calreset` and `wifi restore`
 * each ended in a bare ESP.restart(), which skips everything the two power-off paths save
 * first (WiPhone.ino: the low-battery cut and the 2.5 s hold): an open book's page turns since
 * its last periodic save (BOOKS_SAVE_EVERY) and KOSync's last-move stamps, the map's view and
 * dirty pins, a running download's cursor ("not a crash"), a running game's resume point.
 * Review M3, 2026-09-25. The SAME four calls, in the same order, so a bench reboot leaves the
 * phone exactly as a power-off would — and there is one place to add the next one.
 * ⚠ Nothing here touches /meshdb.*: `meshdb cut` relies on no DB save running after it. */
static void benchReboot(const char* who) {
  booksSaveOpenPosition();          // app_books.h: the page you are on + KOSync's stamps
  mapsSaveOpenView();               // app_maps.h: the view, and pins not yet written
  tileFetchPowerOff();              // tile_fetch.h: a download's cursor, and "not a crash"
  extern bool gbcSaveForPowerOff(); // app_gbc.h: a running game parks and writes its resume point
  gbcSaveForPowerOff();
  say("%s: saved what a power-off saves - rebooting\n", who);
  delay(300);                       // let the lines above leave the UART
  ESP.restart();
}

/* A multi-line dump, one say() per line (say()'s 192-byte buffer; see help()). Cuts `buf`. */
static void sayLines(char* buf) {
  for (char* p = buf; *p;) {
    char* nl = strchr(p, '\n');
    if (nl) {
      *nl = '\0';
    }
    sayLine(p);
    if (!nl) {
      break;
    }
    p = nl + 1;
  }
}

/* ⚠ One say() per line. say()'s buffer is 192 bytes and this text is ~750:
 * a single call TRUNCATED the help mid-list, so every command added after
 * `chan` was invisible to `?` — which is the one place a stranded user looks.
 * (Found by the adversarial review; it had been silently cut for releases.) */
static void help() {
  static const char* LINES[] = {
    "",
    "WiPhone serial commands:",
    "  ?          this help",
    "  up on      start the WiFi uploader (files land in /roms; refused during a Game Boy game)",
    "  up on books|photos  same, into /books or /photos",
    "  up on maps          same, into /maps - accepts <area>/<z>/<x>/<y>.565 paths",
    "  up on keys          same, into /API Keys (gemini.txt)",
    "  up off     stop the uploader",
    "  up         where to point a browser",
    "  sync       poll COVEY for mirrored texts now",
    "  mirror     mirror poller state",
    "  notify [sip]  fire the mesh (or text) arrival announcement - buzz + pop through the",
    "             real path, with the NOTIFY: lines that time them; no second phone needed",
    "  sip        SIP account state: loaded, registered, WiFi - one line",
    "  bookpage   dump the open reader page's layout + rendering",
    "  kosync     KOSync (reading-position sync with an X4/COVEY): config, window, home client;",
    "             home= by name adds where it was found, on which WiFi, and the longest lookup pass",
    "  kosync open [secs]  open a sync window for the book open in the reader (default 300)",
    "  kosync close|push|pull|sync|reload  end the window / send home / ask home /",
    "             read home then offer-or-send (Sync my place) / re-read /books/kosync.txt",
    "             (push/pull/sync need WiFi + home=, and a book open; reload also clears",
    "             the last window's ! warnings)",
    "  keys       keypad health: why a press went missing (drained/rescued/swept)",
    "  health     dump /health.log over the CABLE (battery + restart black box)",
    "  health all dump the whole file, not just the last 24 KB",
    "  chan <url> apply a Meshtastic channel invite URL",
    "  chans      list the channels this phone has",
    "  meshdb     chat history: which filesystem, what loaded, what the next save keeps",
    "  meshdb cut BENCH: save, set /meshdb.bin aside as /meshdb.cut WITHOUT the rename (the state",
    "             a power cut mid-save leaves), then REBOOT - the boot log must say RECOVERED.",
    "             Card only; refuses while an old /meshdb.cut is there (it may BE the database),",
    "             or a frame waits for the radio, a server/window or a call is up. Runs the power-off",
    "             saves (book place, map view, download cursor, game) before it reboots",
    "  wifi drop  simulate a hotspot blip, to measure the reconnect path",
    "  wifi off|on  the Settings \"WiFi\" switch from the cable (persisted, same calls)",
    "  wifi why   (or just `wifi`) why it dropped: disconnect reasons, the AP/channel/RSSI,",
    "             the driver's STA config, refused scan starts, the last 15 WiFi events,",
    "             and the last `WIFI restore` (who gave the radio back, and what it was allowed)",
    "  wifi log on|off  the WiFi driver's OWN state lines (beacon timeout, state changes) -",
    "             bench only, RAM only, off again at reboot",
    "  wifi scan  what the radio can actually hear - deaf radio vs absent AP (with WiFi off",
    "             the radio goes back off after it; refused under a game or a live hotspot)",
    "  wifi bounce  radio off/on without a reboot - the deaf-scan cure, keeps saved APs",
    "             (refused with WiFi off/disabled, under a game or a live hotspot)",
    "  wifi calreset  erase the stored RF calibration and reboot - the deaf-radio probe",
    "  star [<!node>]  list starred nodes, or toggle one (top of list, evicted last)",
    "  send <i> <text>  send a channel text (index from `chans`) - proves the broadcast receipt",
    "  pki        DM crypto state: our key, who has keys, stack headroom",
    "  pki forget <!node>  drop ONE node's key (a MISMATCH after it re-keyed) - not Clear nodes",
    "  announce   broadcast NodeInfo now, asking others to answer with theirs",
    "  dm <!node> <text>  send a direct message (PKI when the key is known)",
    "  heap       memory truth: internal/DMA/PSRAM free+largest+floor",
    "  audio      audio device: powered? moving samples? who is entitled to it?",
    "             + the RTP session (mic/stream/port: all 0 after any call) and the levels",
    "  audio orphan  arm an RTP receive with NO call (no mic, nothing sent) - the hot-mic",
    "             backstop must shut it down within 3 s with an 'AUDIO: RTP session armed' line",
    "  audio tone [old|clean|flat]  the codec's bass/treble/de-emphasis, live, not stored",
    "             (clean = default: adaptive bass, no treble cut, no de-emphasis; old = 0.9.78)",
    "  music      the music feed: drops (ring certainly ran dry), minLead (>0 = certainly no",
    "             gap), lead, the loop's longest gap, reads. `music reset` zeroes the counters,",
    "             `music swap on|off` flips the mono sample-pair swap for an A/B by ear",
    "  replay     history-replay state: ring occupancy, pending tx, last served",
    "  radio      the LoRa send queue: rx/tx, queued + relays waiting, frames finished (own/relay),",
    "             timeouts, relays cancelled, the last frame's air time seen vs predicted,",
    "             and the longest the chip sat deaf after a frame (TxDone to back in RX)",
    "  nbr        neighbours heard DIRECTLY + announce state (My node > Neighbor info)",
    "  nbr on|4h|off|now  set the announce cadence (1h/4h) or announce right now",
    "  pos        positions: waypoints, node fixes, our pin, the reference, where this phone",
    "             is (each source: GPS, the hotspot phone), our beacon",
    "  pos every <secs>|off  position reporting cadence (persists; 300s floor; refused",
    "             while it would go to a PUBLIC or missing channel - arm that on the phone)",
    "  pos now    force one beacon: obeys the channel/fix/spacing rules but NOT",
    "             the on/off switch - it sends with reporting OFF. Never to a public",
    "             channel; a bench fix goes out logged INJECTED, as MANUAL",
    "  pos inject gps|phone <lat> <lon> [sats hdop]  a BENCH fix for that source, RAM only:",
    "             drawn and measured from, never sent by the schedule (use dummy",
    "             coordinates, e.g. 40.0 -100.0); `pos inject clear` drops them",
    "  phonepos   the hotspot phone's GPS: state, port, bytes, sentences, bad checksums,",
    "             stale lines dropped, last fix age, connects (never a coordinate or address)",
    "  phonepos auto|off  My node > Phone location (persists; auto is refused here while",
    "             reporting goes to a PUBLIC channel - that is chosen on the phone)",
    "  phonepos port <n>  an extra port to try after 10110 and 11123 (0 = none; persists)",
    "  phonepos at <ip>[:port] | at off  BENCH, RAM only: ask that address instead of the",
    "             gateway (tools/nmea_phone_stub.py on a Mac); its fixes are bench fixes",
    "  netloc     Wi-Fi location (beaconDB): the setting, the last scan (counts only), the last",
    "             look's verdict and accuracy, the fix, each half's HTTP/handshake (never a MAC",
    "             or a coordinate)",
    "  netloc off|map  My node > Wi-Fi location (persists). `map + mesh` is chosen on the",
    "             phone only: it puts a position on other people's maps",
    "  netloc now  look as soon as the gates allow (2-min gap, 3 attempts an hour; may pause",
    "             the music, as a weather Refresh does)",
    "  netloc answer <A> <B>  BENCH: fold two canned halves - each {...} as beaconDB's 200",
    "             body, a 3-digit HTTP status, or - (not asked); the fix is a bench fix",
    "  gps        woods-plate GPS state: fix, sats, reader counters",
    "  gps on|off route the user UART (38/32) to the NMEA reader (persists; `on` is refused",
    "             while reporting is armed to a PUBLIC channel - switch it on the phone)",
    "  gps baud <n>  GPS baud, persists (115200 = the M100 Mini, measured; not 9600)",
    "  gps raw    hex+ASCII of the last bytes off the wire - tells wrong-baud from binary",
    "  sun        legal light at the reference place: first light/sunrise/sunset/last light",
    "             by the Almanac's legal-light rule (30 min, or civil twilight), and the countdown",
    "  almanac [lat,lon] [+N|-N]  everything Menu > Almanac shows for that day (default: the",
    "             app's place, today), then the exact instants in UNIX seconds. ONE pass, ~0.4 s",
    "             at 160 MHz (a bench command: the app computes in slices); prints its own time",
    "  almanac cost  what the Almanac measured: its slices, each part of the day, its builds",
    "  almanac bench  each piece of the Almanac timed now (one pass, ~0.3 s)",
    "  wx         the Almanac's weather: cache age, place source, rounded point, each host's",
    "             status/code/handshake ms, worker stack and heap floors (never a URL)",
    "  wx fetch | wx show | wx clear  Refresh's path / the WEATHER screen's rows / delete the cache",
    "  ai         Menu > AI: the key file (its LENGTH, never the key), the chat, the daily",
    "             limits, the last question's HTTP code/model/time/stack floor",
    "  ai ask <q> ask Gemini through the app's own path (500 chars); `ai` shows progress",
    "  ai last    the newest question and its answer, in full",
    "  ai topic | ai clear | ai reload  new topic / delete the saved chat / re-read gemini.txt",
    "  ai retry on|off  Menu > AI's \"Retry if it times out\" (NVS; on by default)",
    "  clock      (or `time`) the time, WHO set it (ntp/gps/mesh) and how long ago, and",
    "             what the GPS and mesh paths last decided about it - see clock_source.h",
    "  tz         the time zone: setting, zone, offset, where it came from, Auto's last look",
    "             and why, the next DST change (never a coordinate)",
    "  tz auto|custom [offset]|<zone>  Auto (from a measured fix; until one, the zone or offset",
    "             the clock had - never one found before), the typed offset (-7, -3:30;",
    "             persists in configs.ini too), or a named zone: pacific mountain arizona central",
    "             eastern atlantic newfoundland alaska hawaii saskatchewan (each with its DST)",
    "  tz test <lat> <lon>  look a place up: zone, border or not covered, its offset now, and",
    "             whether a phone fix there would count (10 km margin). Changes nothing",
    "  tz forget  drop the zone Auto found (Auto uses the typed offset until a fix finds one)",
    "  maps       what the card holds under /maps, the saved view, and the pins file",
    "  maps goto <lat> <lon> [z] [area]  set where the Maps app opens next (persists)",
    "  maps hold up|down|left|right [ms [blip]]  press an arrow and hold it (hold-to-scroll bench)",
    "  maps dl [src lat lon km zmax [elev 1|0] | stop]  the tile download: its status (map tiles and",
    "             the elevation layer's own counts), start one, or stop it (elev: /maps/elev too)",
    "  maps dlurl <template> [area] | clear  source 3 = a plain-http relay (a Mac's tile pool)",
    "             writing into /maps/<area> (default custom; otm = the OpenTopoMap map; never elev)",
    "  elev       the map's altitude readout: /maps/elev on the card?, the setting, and (Maps",
    "             open) the crosshair's and your last samples, which layer answered, what they cost",
    "  elev <lat> <lon>  sample the elevation tiles at any point, timed (z13, else coarse z10)",
    "  units [metric|us]  distances and heights: metric (m, km) or US (ft, mi); persists",
    "  key hold <key> [ms [blip]]  press any key and HOLD it (the F2/digit/'#' holds, from the cable)",
    "  gbc        the Game Boy: is a game up, which ROM, Screen mode, its two state files",
    "  gbc autosave  write the running game's resume point the way power-off does (parks it)",
    "  lock       why the screen does or does not lock: setting, sleep gate, and the card",
    "  ver        firmware version and build time of the binary actually running",
    "  scrim [<alpha> [hex]]  the grey plate under menu text over a wallpaper (RAM only)",
    "  shot       dump the live screen as base64 (tools/shot.py turns it into a PNG)",
    "  ls [<dir>] list a card folder, HIDDEN entries included (marked *) - the one view",
    "             that shows dotfiles; the Files app and the pickers never do",
    "  rm </path> delete ONE file by full path (refuses folders and relative names)",
    "  mv </from> </to>  rename ONE file in place (quote paths with spaces; never",
    "             overwrites; Books/Files/Photos/Game Boy closed, no track loaded,",
    "             uploader off); a book's KOSync memory follows it to the new name",
    "  power      the USB-power-meter bench: state of every switch below, and the method",
    "  power lcd sleep|wake   ST7789 DISPOFF+SLPIN / SLPOUT+DISPON (backlight untouched)",
    "  power i2s stop|start   the I2S peripheral+DMA that runs from boot whether or not",
    "             anything ever played",
    "  power lora sleep|rx    park the SX1276 in SLEEP (mesh DEAF) / back to RX-continuous",
    "  power sleep <secs>     light-sleep the ESP32 for N s (WiFi must be OFF): the floor",
    "             (both sleeps refuse while a mesh frame is on the air or waiting)",
    "  cpu        the CPU clock: MHz, the PLL it runs from, radio on/off, method, and the switch",
    "             counts (same-PLL / PLL re-locks radio off / re-locks WITH THE RADIO ON)",
    "  cpu method old|new  new (default) never re-locks the PLL under a running radio: 160/80",
    "             with WiFi on, 240/80 with it off. old = setCpuFrequencyMhz 240/80, the 4/4",
    "             WiFi break. RAM only - a reboot is new again",
    "  cpu cycle <n> <ms>  n down/up switch pairs through the gate's own code, one per <ms>",
    "             (`cpu cycle 0` stops) - the A/B for the WiFi drop",
    "  key <names>  press keys: select/menu back ok up down left right call end f1-f4,",
    "             or a single character. `key menu`, `key down down ok`. Real presses -",
    "             they go into the keypad buffer, so the whole UI path runs unchanged",
    "  open <app>  jump into an app: maps almanac weather ai photos books music mesh gbc files",
    "             clock (`weather` = the Almanac opened on Weather), or",
    "             wifi (Settings > WiFi's list) / wifiedit (edit the current network - there",
    "             `key select` is its Connect/Disconnect). Leaving (`open clock`) runs the exit",
    "             path Back does - for the WiFi screens, the `WIFI restore` line",
    "  wallpaper  what the background loader found, and why it did or did not use it",
    "  wallpaper reload|list|clear  re-read it / list /photos / drop the override",
    "  wallpaper set <name>  set /photos/<name> as the wallpaper - the SAME code the",
    "             Photos menu runs, so it can be proven over the cable",
    "  unread     recount unread texts, repair the counter, name the threads",
    "  unread clear  mark EVERYTHING read (orphaned threads included)",
    "",
  };
  for (size_t i = 0; i < sizeof(LINES) / sizeof(LINES[0]); i++) {
    say("%s\n", LINES[i]);
  }
}

/* 🛑 THE CONSENT RULE, for every serial line that could START a location going out (review
 * 2026-10-10). What makes a location beacon safe is the picker's two-press confirm on the
 * screen, where PUBLIC is in the large font; `pos chan` is refused for that reason, and so is
 * anything here that arms reporting (`pos every`) or switches a source on (`gps on`, `phonepos
 * auto`) while reporting is aimed at a PUBLIC channel — or at one gone from this phone (unknown
 * fails safe, as channelIsPublic(NULL) does). No channel named is safe: nothing goes out until
 * one is chosen ON the phone. pospub (a public channel confirmed once) survives "Position: off"
 * on the screen, so without this a cable line re-armed a broadcast the owner had stopped.
 * NULL = allowed; else the reason to print. */
static const char* serialPosConsentRefusal() {
  if (!meshService.getPosChannelName()[0]) {
    return NULL;
  }
  const MeshChannel* pc = meshService.getPosChannel();
  if (!pc) {
    return "the reporting channel is gone from this phone";
  }
  return meshService.channelIsPublic(pc) ? "position reporting goes to a PUBLIC channel" : NULL;
}

/* `pos`'s "where is THIS phone" block (self_pos.h): which source the screens use now, and one
 * line per source with why it is or is not used. 🛑 No coordinates here — what is wanted on the
 * bench is WHICH and WHY, and these lines get pasted into notes that live in a public repo. */
static void reportSelfSources() {
  SelfFix fx;
  const int src = meshService.getSelfFix(&fx, false);
  SelfFix tx;
  const int txSrc = meshService.getSelfFix(&tx, true);
  if (src != SELF_NONE) {
    say("pos: source = %s%s (%s on the map); a beacon now would send %s\n",
        selfSrcName(src), fx.injected ? " [BENCH]" : "", selfMapLabel(src),
        txSrc != SELF_NONE ? selfBeaconTag(txSrc) : "nothing (see below)");
  } else {
    say("pos: source = none live (the pin, or nothing, stands in)\n");
  }
  for (int s = SELF_GPS; s <= SELF_NET; s++) {
    SelfFix f;
    if (!meshService.getSourceFix(s, &f)) {
      const bool on = (s == SELF_GPS) ? meshService.isGpsEnabled()
                    : (s == SELF_PHONE) ? meshService.isPhoneEnabled()
                    : geolocSetting() != GEO_SET_OFF;
      say("pos:   %-5s %s\n", selfSrcName(s), on ? "no fix yet" : "off");
      continue;
    }
    if (s == SELF_NET) {
      /* The accuracy and the sending state, never where (`netloc` has the rest). */
      SelfFix t;
      const bool txKnown = selfSource(meshService.getSelfSlots(), SELF_NET, millis(), SELF_PICK_TX, &t);
      say("pos:   %-5s %s, +/-%u m, last confirming scan %lus ago; to send: %s%s\n", selfSrcName(s),
          selfStateName(f.state), (unsigned)f.accM, (unsigned long)(f.ageMs / 1000u),
          txKnown ? selfStateName(t.state) : "-", f.injected ? " [BENCH: netloc answer]" : "");
      continue;
    }
    char q[40];
    if (f.sats >= 0 && f.hdopX10 >= 0) {
      snprintf(q, sizeof(q), "%d sats, HDOP %d.%d", f.sats, f.hdopX10 / 10, f.hdopX10 % 10);
    } else if (f.sats >= 0) {
      snprintf(q, sizeof(q), "%d sats", f.sats);
    } else if (f.hdopX10 >= 0) {
      snprintf(q, sizeof(q), "HDOP %d.%d", f.hdopX10 / 10, f.hdopX10 % 10);
    } else {
      strlcpy(q, "sats/HDOP not said", sizeof(q));
    }
    say("pos:   %-5s %s, %lus old, %s%s\n", selfSrcName(s), selfStateName(f.state),
        (unsigned long)(f.ageMs / 1000u), q, f.injected ? " [BENCH: pos inject / phonepos at]" : "");
  }
}

/* `pos` — the whole positions picture in one paste: every waypoint heard, every
 * node with a fix (age in minutes), our own pin, which reference distances are
 * measured from, and — since the woods plate — what this phone is telling the
 * world about ITSELF. That last block is the one worth pasting into a bug
 * report: it answers "is my location going out, to whom, and when did it last
 * go" in five lines. */
static void reportPos() {
  int32_t refLat = 0, refLon = 0;
  char refName[20] = "";
  bool haveRef = meshService.resolveReference(&refLat, &refLon, refName, sizeof(refName));
  /* Name the TRUE missing piece: printing "no waypoints heard" directly above a listed
   * waypoint misleads — with waypoints in the DB the fix is CHOOSING one (2026-08-20). */
  say("pos: reference = %s\n",
      haveRef ? refName
      : meshService.getWaypointCount() > 0
          ? "NONE - waypoints heard but none chosen (Places > pick > set reference)"
          : "NONE (no waypoints heard, no pin set)");

  reportSelfSources();

  for (int i = 0; i < meshService.getWaypointCount(); i++) {
    const MeshWaypoint* w = meshService.getWaypoint(i);
    if (!w) continue;
    say("  wp '%s' id=%u  %d.%05d,%d.%05d%s\n", w->name, (unsigned)w->id,
        (int)(w->latI / 10000000), abs((int)((w->latI % 10000000) / 100)),
        (int)(w->lonI / 10000000), abs((int)((w->lonI % 10000000) / 100)),
        w->expire ? " (expires)" : "");
  }

  uint32_t nowMs = millis();
  for (int i = 0; i < meshService.getNodeCount(); i++) {
    const MeshNode* n = meshService.getNode(i);
    if (!n || n->posHeardMs == 0) continue;
    char dist[16] = "";
    char extra[40] = "";
    if (haveRef) {
      double m = meshPosDistanceM(refLat, refLon, n->latI, n->lonI);
      unitsFmtDist(m, gUnits, dist, sizeof(dist));
      snprintf(extra, sizeof(extra), "  %s %s of %s", dist,
               meshPosCompass8(meshPosBearingDeg(refLat, refLon, n->latI, n->lonI)), refName);
    }
    char age[16];
    if (n->posHeardMs == 1) {
      // The restored-from-flash sentinel: the fix is real, its age is unknown.
      snprintf(age, sizeof(age), "old");
    } else {
      snprintf(age, sizeof(age), "%um ago", (unsigned)((nowMs - n->posHeardMs) / 60000u));
    }
    char approx[24] = "";
    if (meshPosIsApprox(n->posPrec)) {
      // The sender cut its position to a cell (precision_bits): the point is its middle.
      snprintf(approx, sizeof(approx), " approx +-%.0fm", meshPosPrecisionRadiusM(n->posPrec));
    }
    say("  !%08x '%s'  %d.%05d,%d.%05d%s  (%s)%s\n", (unsigned)n->nodeNum, n->name,
        (int)(n->latI / 10000000), abs((int)((n->latI % 10000000) / 100)),
        (int)(n->lonI / 10000000), abs((int)((n->lonI % 10000000) / 100)),
        extra, age, approx);
  }

  int32_t pinLat, pinLon;
  uint32_t pinAt;
  if (meshService.getMyPin(&pinLat, &pinLon, &pinAt)) {
    say("pos: our pin %d.%05d,%d.%05d (announced to the mesh)\n",
        (int)(pinLat / 10000000), abs((int)((pinLat % 10000000) / 100)),
        (int)(pinLon / 10000000), abs((int)((pinLon % 10000000) / 100)));
  } else {
    say("pos: no pin (Meshtastic > Places > pick one > I'm here)\n");
  }

  // ---- what this phone broadcasts about itself -------------------------------
  const uint32_t pi = meshService.getPosInterval();
  if (pi == 0) {
    say("pos: reporting OFF (My node > Report position)\n");
  } else {
    const char* chn = meshService.getPosChannelName();
    const MeshChannel* pc = meshService.getPosChannel();
    say("pos: reporting every %lus to '%s'%s%s\n", (unsigned long)pi,
        chn[0] ? chn : "(no channel)",
        !chn[0] ? "" : (pc ? "" : " [GONE from this phone]"),
        (pc && meshService.channelIsPublic(pc))
            ? (meshService.posChannelWasPublic() ? " [PUBLIC - confirmed]"
                                                 : " [PUBLIC - REFUSED]")
            : "");
    const char* why = meshService.posBlockedReason();
    say("pos: %s\n", why ? why : "armed and clear to send");
    /* ⚠ "A slot is owed" is a DIFFERENT state from "blocked", and the difference is the whole
     * point of the 2026-08-25 change: blocked says why nothing can go now, this says the phone
     * is holding a slot open and will send the instant a fix arrives. Without this line the
     * two are indistinguishable on the console. */
    if (meshService.posBeaconDue()) {
      say("pos: A SLOT IS OWED - it will send the moment a fresh fix arrives\n");
    }
    if (meshService.getPosLastTxMs() == 0) {
      say("pos: never beaconed (first send is a full interval after arming)\n");
    } else {
      /* QUEUED is not SENT (review M1): the word is what the radio has reported so far. */
      const uint8_t o = meshService.posLastOutcome();
      say("pos: last beacon %lus ago, %s, %d slot(s) skipped since\n",
          (unsigned long)((millis() - meshService.getPosLastTxMs()) / 1000UL),
          o == MESH_TXO_SENT ? "SENT" : o == MESH_TXO_QUEUED ? "QUEUED (not on the air yet)"
                             : o == MESH_TXO_FAILED ? "FAILED" : "queued (no word since)",
          meshService.getPosSkipRuns());
    }
  }
}

/* Base64 (std alphabet, padded) — how the Meshtastic apps display public keys,
 * so {user} can eyeball-compare against what COVEY shows for this phone. */
static void b64enc(const uint8_t* in, int len, char* out, int outCap) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int o = 0;
  for (int i = 0; i < len && o + 5 < outCap; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < len) v |= in[i + 2];
    out[o++] = T[(v >> 18) & 63];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < len) ? T[v & 63] : '=';
  }
  out[o] = '\0';
}

/* `pki` — everything needed to see WHY a DM does or does not flow, in one paste:
 * our announced key (compare against COVEY's node list), each node's key state
 * (no key = DMs to them go legacy and 2.5+ nodes drop them), MISMATCH flags
 * (somebody re-keyed; Clear nodes to re-trust), and the loop task's stack
 * high-water mark (the X25519 derive is the deepest stack user we added). */
static void reportPki() {
  char b64[48];
  if (!meshService.pkiIsReady()) {
    say("pki: NOT READY - no keypair (NVS trouble?)\n");
    return;
  }
  b64enc(meshService.pkiPublicKey(), 32, b64, sizeof(b64));
  say("pki: our key %s\n", b64);
  int keyed = 0;
  for (int i = 0; i < meshService.getNodeCount(); i++) {
    const MeshNode* n = meshService.getNode(i);
    if (!n || n->nodeNum == meshService.getMyNodeNum()) {
      continue;
    }
    if (n->pkiFlags & MESH_NODE_HAS_KEY) {
      keyed++;
      b64enc(n->pubKey, 32, b64, sizeof(b64));
      say("  !%08x '%s' key %s%s\n", (unsigned)n->nodeNum, n->name, b64,
          (n->pkiFlags & MESH_NODE_KEY_MISMATCH) ? "  [MISMATCH SEEN - Clear nodes to re-trust]" : "");
    } else {
      say("  !%08x '%s' NO KEY - their DMs to us fail, ours to them go legacy\n",
          (unsigned)n->nodeNum, n->name);
    }
  }
  say("pki: %d node(s) with keys | loop-task stack floor %u bytes free\n",
      keyed, (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
}

/* `heap` — the memory instrument (upload-redesign brief: first deliverable).
 * Internal and PSRAM tell different stories on this phone: WiFi RX buffers,
 * lwIP, and every operator-new live INTERNAL-only, so "plenty of PSRAM free"
 * is never the number that saves you. min-ever is the low-water mark since
 * boot — if it sits far below current free, something already had a bad
 * moment this run. DMA is the subset the radio actually allocates from. */
static void reportHeap() {
  multi_heap_info_t h;
  heap_caps_get_info(&h, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  say("heap: internal free=%u largest=%u min-ever=%u  blocks used/free=%u/%u\n",
      (unsigned)h.total_free_bytes, (unsigned)h.largest_free_block,
      (unsigned)h.minimum_free_bytes,
      (unsigned)h.allocated_blocks, (unsigned)h.free_blocks);
  heap_caps_get_info(&h, MALLOC_CAP_DMA);
  say("heap: dma      free=%u largest=%u  (the pool WiFi RX buffers draw from)\n",
      (unsigned)h.total_free_bytes, (unsigned)h.largest_free_block);
  heap_caps_get_info(&h, MALLOC_CAP_SPIRAM);
  say("heap: psram    free=%u largest=%u min-ever=%u\n",
      (unsigned)h.total_free_bytes, (unsigned)h.largest_free_block,
      (unsigned)h.minimum_free_bytes);
  say("heap: loop-task stack floor %u bytes free\n",
      (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
}

static void reportUploader() {
  if (!gbcXferOn()) {
    say("uploader: off\n");
    return;
  }
  say("uploader: ON  http://%s/  (or http://wiphone.local/)%s\n",
      xferAddr(), xferUsingAP() ? "  [own hotspot]" : "");
  say("          legacy (no-JS / curl -F / log): http://%s:8080/\n", xferAddr());
  say("          hotspot SSID if used: %s%s   files added: %d\n",
      xferApName(),
      xferApProtected() ? " (WPA2: a KOSync window's hotspot_pass - see /books/kosync.txt)" : "",
      xferFilesAdded());
}

// "12s" / "4m12s" / "3h05m" / "2d04h"
static void fmtAge(uint32_t ms, char* out, size_t cap) {
  const unsigned long s = ms / 1000;
  if (s < 60) {
    snprintf(out, cap, "%lus", s);
  } else if (s < 3600) {
    snprintf(out, cap, "%lum%02lus", s / 60, s % 60);
  } else if (s < 86400) {
    snprintf(out, cap, "%luh%02lum", s / 3600, (s / 60) % 60);
  } else {
    snprintf(out, cap, "%lud%02luh", s / 86400, (s / 3600) % 24);
  }
}

/* `clock` / `time` — see the dispatch in run(). Everything here is read from ntpClock; the
 * GPS and mesh lines are the Diag the loop task writes (clock.h). */
static void reportClock() {
  const uint32_t nowMs = millis();
  const Clock::Diag& d = ntpClock.diag();
  char age[16];
  if (!ntpClock.isTimeKnown()) {
    say("clock: NOT SET - waiting for NTP (WiFi), a GPS fix (`gps on`, open sky), or mesh time\n");
  } else {
    const uint32_t utc = ntpClock.getExactUtcTime();
    const uint32_t loc = ntpClock.getExactUnixTime();
    const int tzMin = (int)(((int64_t)loc - (int64_t)utc) / 60);
    char u[24], l[24];
    Clock::unixToHuman(utc, u);
    Clock::unixToHuman(loc, l);
    say("clock: %s UTC  (local %s, UTC%+d:%02d)\n", u, l, tzMin / 60, abs(tzMin % 60));
    {
      TzEff e;
      tzSvcEffective(&e);
      char st[72];
      tzStatusText(&e, tzSvcAuto()->zoneSrc, st, sizeof(st));
      say("clock: time zone: %s (`tz` for why)\n", st);
    }
    fmtAge(ntpClock.msSinceSet(nowMs), age, sizeof(age));
    say("clock: source %s, set %s ago - %s\n", clockSourceName(ntpClock.getSource()), age,
        ntpClock.isTimeTrusted()
            ? "trusted"
            : "LOWER TRUST: shown, but KOSync, waypoint expiry and all it transmits treat it as unknown");
  }
  /* The SIP message store under a mesh clock (review SA-3; clock_source.h): what it is doing
   * with the stamps it writes, and the offset that will move (or moved) the provisional ones. */
  const ClockMsgStamp ms = ntpClock.msgStamp();
  if (ms.meshId) {
    say("clock: messages: stamps written now are PROVISIONAL (mesh set %08x) - NTP/GPS will move them\n",
        (unsigned)ms.meshId);
  } else if (ms.corrId) {
    say("clock: messages: NTP/GPS replaced mesh set %08x, which was off by %+ld s - its provisional "
        "stamps move by that (serial `MSG: finalised ...`)\n",
        (unsigned)ms.corrId, (long)ms.corrS);
  }
  const uint32_t sinceNtp = ntpClock.msSinceNtp(nowMs);
  if (sinceNtp == CLOCK_NEVER_MS) {
    say("clock: ntp: has not answered since boot\n");
  } else {
    fmtAge(sinceNtp, age, sizeof(age));
    say("clock: ntp: last set it %s ago - %s\n", age,
        sinceNtp < CLOCK_NTP_FRESH_MS ? "fresh: GPS will not override it"
                                      : "stale (30 min+): GPS may correct it past 2 s");
  }
#ifdef USER_SERIAL
  const bool gpsOn = gGpsNmea;
#else
  const bool gpsOn = false;
#endif
  if (d.gpsVerdict == CLK_NONE) {
    say("clock: gps: %s\n", gpsOn ? "receiver on, no RMC judged yet" : "receiver off (`gps on`)");
  } else {
    fmtAge(nowMs - d.gpsVerdictMs, age, sizeof(age));
    say("clock: gps: last RMC %s ago: %s%s\n", age, clockVerdictText(d.gpsVerdict),
        gpsOn ? "" : "  [receiver now off]");
  }
  if (d.gpsHaveDiff) {
    fmtAge(nowMs - d.gpsDiffAtMs, age, sizeof(age));
    say("clock: gps: gps minus clock = %+ld ms (%s ago); set the clock %lu time(s) since boot\n",
        (long)d.gpsDiffMs, age, (unsigned long)d.gpsSets);
  } else if (d.gpsSets) {
    say("clock: gps: set the clock %lu time(s) since boot\n", (unsigned long)d.gpsSets);
  }
  if (d.meshNode) {
    say("clock: mesh: SET it from !%08lx on '%s'\n", (unsigned long)d.meshNode, d.meshChan);
  }
  if (d.meshVerdict == CLK_NONE) {
    say("clock: mesh: no position carrying a time heard yet\n");
  } else {
    fmtAge(nowMs - d.meshVerdictMs, age, sizeof(age));
    say("clock: mesh: last timed position %s ago: %s (%lu held)\n", age,
        clockVerdictText(d.meshVerdict), (unsigned long)d.meshHeld);
  }
}

/* `tz` - the clock's time zone (tz_service.h; the rules are tz_auto.h's). 🛑 No coordinate in any
 * line: the zone, the offset and the SOURCE say enough, and console captures land in notes. */
static const char* tzModeWord(uint8_t m) {
  return m == TZ_MODE_AUTO ? "Auto" : m == TZ_MODE_NAMED ? "named zone" : "Custom";
}

static void reportTz() {
  const uint32_t nowMs = millis();
  TzEff e;
  tzSvcEffective(&e);
  const TzAuto* a = tzSvcAuto();
  char st[72], off[16], age[16];
  tzStatusText(&e, a->zoneSrc, st, sizeof(st));
  say("tz: %s - %s%s\n", tzModeWord(tzSvcMode()), st,
      e.ok ? "" : " (the offset waits for the clock: a DST zone needs the date)");
  if (e.iana) {
    const TzZone* gz = tzGridZone(tzGridFindZone(e.iana));
    say("tz: zone %s, %s now%s\n", e.iana,
        !e.ok ? "?" : (gz ? (e.dst ? gz->abbrDst : gz->abbrStd) : "-"),
        e.rule == TZ_RULE_USCAN ? ", US/Canada DST rule" : ", no DST");
  }
  if (e.kind == TZK_AUTO && a->zoneSrc == TZA_SRC_HAND) {
    say("tz: Auto started on the zone set by hand before it (saved); two looks that find another"
        " clock change it\n");
  } else if (e.kind == TZK_AUTO) {
    const uint32_t at = tzSvcAutoSetUtc();
    const uint32_t utc = ntpClock.isTimeKnown() ? ntpClock.getExactUtcTime() : 0;
    if (at && utc >= at) {
      fmtAge((utc - at) > 4000000u ? 4000000000u : (utc - at) * 1000u, age, sizeof(age));
      say("tz: Auto found it %s, %s ago (saved: kept across a reboot)\n", tzSrcWords(a->zoneSrc), age);
    } else {
      say("tz: Auto found it %s (saved: kept across a reboot)\n", tzSrcWords(a->zoneSrc));
    }
  }
  tzFmtOffset(ntpClock.getOffsetMinutes(), off, sizeof(off));
  if (tzSvcLastChangeMs()) {
    char was[16];
    tzFmtOffset(tzSvcLastChangeFrom(), was, sizeof(was));
    fmtAge(nowMs - tzSvcLastChangeMs(), age, sizeof(age));
    say("tz: clock offset %s (changed from %s %s ago)\n", off, was, age);
  } else {
    say("tz: clock offset %s (not changed since boot)\n", off);
  }
  char nx[64];
  tzNextText(&e, nx, sizeof(nx));
  if (e.ok && e.next) {
    char u[24];
    Clock::unixToHuman((uint32_t)e.next, u);
    say("tz: next: %s, at %s UTC\n", nx, u);
  } else if (nx[0]) {
    say("tz: next: %s\n", nx);
  }
  if (a->whyMs) {
    fmtAge(nowMs - a->whyMs, age, sizeof(age));
    char w[64];
    tzaWhyText(a, w, sizeof(w));
    say("tz: last look %s ago: %s [%s]%s%s\n", age, w, tzaWhyName(a->why),
        a->lastSrc == SELF_GPS ? " - GPS fix" : a->lastSrc == SELF_PHONE ? " - phone fix (10 km margin" : "",
        a->lastSrc != SELF_PHONE ? "" : a->lastUnrated ? ", no accuracy given)" : ")");
  } else {
    say("tz: Auto has not looked yet (every %lus)\n", (unsigned long)(TZA_CHECK_MS / 1000));
  }
  const TzZone* cz = tzGridZone(a->cand);
  if (cz) {
    fmtAge(nowMs - a->candFirstMs, age, sizeof(age));
    const uint32_t seen = nowMs - a->candFirstMs;
    say("tz: waiting: %s first seen %s ago - switches on a look %lus or more after that\n",
        cz->display, age, (unsigned long)((seen >= TZA_CONFIRM_MS ? 0 : TZA_CONFIRM_MS - seen) / 1000));
  }
  char typed[16];
  tzFmtOffset(tzSvcCustomMinutes(), typed, sizeof(typed));
  const TzNamed* nz = tzNamed(tzSvcNamed());
  say("tz: typed offset %s (configs.ini + NVS); named zone %s; %lu looks, %lu switches since boot\n",
      typed, nz ? nz->key : "none", (unsigned long)a->checks, (unsigned long)a->switches);
  const TzGridInfo* g = tzGridInfo();
  say("tz: zone map timezone-boundary-builder %s, tzdata %s, (c) OpenStreetMap contributors, ODbL 1.0\n",
      g->release, g->tzdata);
}

/* `tz test <lat> <lon>`: the lookup alone. Prints the place to 2 decimals only (~1 km). */
static void tzTest(const char* arg) {
  while (*arg == ' ') arg++;
  char* end = NULL;
  const double lat = strtod(arg, &end);
  const char* p = end;
  while (p && (*p == ' ' || *p == ',')) p++;
  char* end2 = NULL;
  const double lon = (p && *p) ? strtod(p, &end2) : 0.0;
  if (!end || end == arg || !end2 || end2 == p || lat < -90 || lat > 90 || lon < -180 || lon > 180) {
    say("tz: usage tz test <lat> <lon>  (decimal degrees, e.g. tz test 40.26 -76.88)\n");
    return;
  }
  const int32_t la = (int32_t)(lat * 1e7 + (lat >= 0 ? 0.5 : -0.5));
  const int32_t lo = (int32_t)(lon * 1e7 + (lon >= 0 ? 0.5 : -0.5));
  uint8_t why = 0;
  const int z = tzGridLookup(la, lo, &why);
  if (z < 0) {
    say("tz test %.2f,%.2f: %s - Auto would change nothing there\n", lat, lon, tzWhyText(why));
    return;
  }
  const TzZone* zz = tzGridZone(z);
  TzEff e;
  const bool known = ntpClock.isTimeKnown();
  tzEffective(TZ_MODE_AUTO, -1, z, 0, known, (int64_t)ntpClock.getExactUtcTime(), &e);
  char off[16], nx[64];
  tzFmtOffset(e.ok ? e.minutes : zz->stdMinutes, off, sizeof(off));
  tzNextText(&e, nx, sizeof(nx));
  say("tz test %.2f,%.2f: %s (%s), %s %s%s%s\n", lat, lon, zz->display, zz->iana, off,
      e.ok ? "now" : "standard (clock not set)", nx[0] ? "; " : "", nx);
  const int m = tzGridLookupMargin(la, lo, TZA_PHONE_MARGIN_M, NULL);
  say("tz test: a GPS fix there would count; a phone fix (any app) %s\n",
      m >= 0 ? "too - no other clock within 10 km" : "would NOT: another clock or the map's edge within 10 km");
}

static void run(char* line) {
  while (*line == ' ') {
    line++;
  }
  if (!*line || !strcmp(line, "?") || !strcasecmp(line, "help")) {
    help();
    return;
  }

  if (!strcasecmp(line, "up")) {
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up on books")) {
    xferStart(xferBooksConfig());       // the Books uploader, no hands on the phone
    if (!gbcXferOn() && xferStartError()) {
      say("up: NOT started - %s\n", xferStartError());
    }
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up on photos")) {
    xferStart(xferPhotosConfig());      // the gallery, same reason as books
    if (!gbcXferOn() && xferStartError()) {
      say("up: NOT started - %s\n", xferStartError());
    }
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up on t9")) {
    /* Upload a T9 word list, then `t9 reload` to pick it up without a reboot. */
    xferStart(xferT9Config());
    if (!gbcXferOn() && xferStartError()) {
      say("up: NOT started - %s\n", xferStartError());
    }
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up on keys")) {
    /* API keys (gemini.txt) into /API Keys: tools/wiphone_send.py --app keys gemini.txt. */
    xferStart(xferKeysConfig());
    if (!gbcXferOn() && xferStartError()) {
      say("up: NOT started - %s\n", xferStartError());
    }
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up on maps")) {
    /* Map tiles: a converted area pushed as a TREE (tools/wiphone_send.py --app maps --tree). */
    xferStart(xferMapsConfig());
    if (!gbcXferOn() && xferStartError()) {
      say("up: NOT started - %s\n", xferStartError());
    }
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up on")) {
    if (gbcXferOn()) {
      say("uploader already on\n");
    } else {
      gbcXferStart();
      /* A refusal is not a silent no-op: the bench needs the reason as much as
       * the screen does. See the heap guard in xferStart(). */
      if (!gbcXferOn() && xferStartError()) {
        say("up: NOT started - %s\n", xferStartError());
      }
    }
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up off")) {
    /* ⚠ Worth taking down rather than leaving up, and not only to save power: while the
     * uploader is on screen the Games app is running, and sipMayPoll() is false — so the
     * SMS mirror poller will not open a socket. Uploading its config and then leaving the
     * uploader up looks exactly like the poller being broken. That is what this was for. */
    if (!gbcXferOn()) {
      say("uploader already off\n");
    } else {
      xferStop();
      say("uploader: off\n");
    }
    return;
  }

  if (!strcasecmp(line, "sync")) {
    if (!smsMirrorPollConfigured()) {
      say("mirror: not configured (%s)\n", smsMirrorPollStatus());
      return;
    }
    smsMirrorPollRequestNow();
    say("mirror: poll requested; watch for a SMSMIRROR line\n");
    return;
  }
  if (!strcasecmp(line, "mirror")) {
    say("mirror: %s (%sconfigured)\n", smsMirrorPollStatus(),
        smsMirrorPollConfigured() ? "" : "not ");
    return;
  }

  /* `notify` — the arrival announcement without an arrival. "Sometimes the motor barely
   * starts, sometimes the sound does not play" needed a second phone and a mesh message per
   * try; this is one line on the cable, through the SAME notifyMessageArrived() the mesh and
   * text paths call, so the buzz-off/pop-stopped lines it prints are the ones a real message
   * would print. It cannot reproduce the DB-save stall that made the real path lie — that
   * still needs a message landing while the phone is idle. */
  if (!strcasecmp(line, "notify") || !strcasecmp(line, "notify sip")) {
    extern bool notifyBenchArrival(bool sip);
    const bool sip = (line[6] != '\0');
    if (!notifyBenchArrival(sip)) {
      say("notify: refused - the emulator owns the audio device (quit the game first)\n");
      return;
    }
    say("notify: %s arrival fired - watch for NOTIFY: buzz / pop start / buzz off / pop stopped\n",
        sip ? "text" : "mesh");
    return;
  }
  if (!strcasecmp(line, "bookpage")) {
    booksDebugDumpPage();
    return;
  }

  /* `kosync` — the KOSync window and home client from the cable. `open` is the bench's way
   * to put the phone in front of an X4 without pressing through the reader menu; the status
   * lines are the same ones the Sync settings screen shows, plus the heap. */
  if (!strcasecmp(line, "kosync") || !strncasecmp(line, "kosync ", 7)) {
    const char* arg = line + 6;
    while (*arg == ' ') {
      arg++;
    }
    char out[128];
    if (!*arg || !strcasecmp(arg, "status")) {
      kosyncDumpStatus(sayLine);
    } else if (!strcasecmp(arg, "reload")) {
      kosyncReloadConfig(true);          // asked for: last window's warnings go too
      kosyncDumpStatus(sayLine);
    } else if (!strcasecmp(arg, "close")) {
      if (kosyncWindowActive()) {
        kosyncWindowClose("closed from serial");
        say("kosync: window closed\n");
      } else {
        say("kosync: no window open\n");
      }
    } else if (!strncasecmp(arg, "open", 4)) {
      const uint32_t secs = (uint32_t)strtoul(arg + 4, NULL, 10);
      const bool ok = booksKosyncBench("open", secs > 0 && secs <= 3600 ? secs : 0, out, sizeof(out));
      say("kosync: %s%s\n", ok ? "" : "NOT opened - ", out);
    } else if (!strcasecmp(arg, "push") || !strcasecmp(arg, "pull") || !strcasecmp(arg, "sync")) {
      const char* verb = !strcasecmp(arg, "push") ? "push" : !strcasecmp(arg, "pull") ? "pull" : "sync";
      const bool ok = booksKosyncBench(verb, 0, out, sizeof(out));
      say("kosync: %s%s%s\n", ok ? "" : "NOT started - ", out,
          ok ? "  (watch for a KOSYNC home line)" : "");
    } else {
      say("kosync: status | open [secs] | close | push | pull | sync | reload\n");
    }
    return;
  }

  /* `keys` — why a keypress went missing. A dropped press logs nothing and is
   * indistinguishable from a bad thumb, which is how the SN7326's 10ms INT pulse stayed
   * hidden behind "the menus miss the odd button" for two years. Each counter's meaning
   * is written above keypadHealth() in WiPhone.ino. */

  /* `health` — the battery/restart black box, over the cable rather than over WiFi.
   * See healthDump() in WiPhone.ino for why the HTTP route is the dangerous one. */
  if (!strcasecmp(line, "health") || !strcasecmp(line, "health all")) {
    extern int healthDump(uint32_t lastBytes);
    /* Exact matches, not a prefix test: a mistyped argument should reach the "unknown
     * command" reply rather than quietly dumping the whole file over the cable. */
    const bool all = !strcasecmp(line, "health all");
    say("--- health.log %s ---\n", all ? "whole file" : "last 24 KB");
    const int total = healthDump(all ? 0u : 24u * 1024u);
    if (total < 0) {
      say("health: nothing to read - no card, or no /health.log yet\n");
    }
    /* ⚠ The trailer (size, cap, trim point) is printed by healthDump() itself, NOT here.
     * The first version restated the cap as literals in this file and they were STALE the
     * same hour — the cap was raised in WiPhone.ino and this line went on confidently
     * reporting the old one. A diagnostic that misreports the thing it exists to report is
     * worse than no diagnostic. Only the file that owns the constants may print them. */
    return;
  }

  if (!strcasecmp(line, "keys")) {
    extern int keypadHealth(char* out, int cap);
    char buf[200];
    keypadHealth(buf, sizeof(buf));
    say("keys: %s\n", buf);
    return;
  }

  /* `audio` — is the codec powered, and is anything actually using it?
   *
   * ⚠ THIS REPORTS THE FIRMWARE'S SHADOW STATE, NOT THE CHIP'S. The WM8750 is addressed
   * write-only over its 2-wire interface (src/drivers/WM8750.h has setReg and no readReg), so
   * the registers cannot be read back to check. If the shadow and the chip ever disagree, this
   * command will say the comfortable thing — which is exactly why the idle watchdog in
   * WiPhone.ino releases the device on a timer rather than trusting anyone's bookkeeping.
   *
   * The line to look for is `powered=YES moving=no`: that is the leak. */
  /* `mute on|off` — the master mute (Settings > Mute all sounds), from the cable: the same
   * path the screen uses, so it is applied at once and stored. `mute` alone reports. */
  if (!strncasecmp(line, "mute", 4) && (line[4] == '\0' || line[4] == ' ')) {
    extern Audio* audio;
    extern GUI gui;
    const char* arg = line + 4;
    while (*arg == ' ') arg++;
    if (!*arg) {
      say("mute: %s\n", gui.state.audioMuted ? "ON (nothing out of the loudspeaker; earpiece and headphones play)" : "off");
      return;
    }
    const bool on = !strcasecmp(arg, "on");
    if (!on && strcasecmp(arg, "off")) {
      say("mute: usage mute on|off\n");
      return;
    }
    const bool stored = guiSetMuted(audio, gui.state, on);
    say("mute: %s%s\n", on ? "ON" : "off", stored ? ", stored" : " (NOT stored - configs file)");
    return;
  }

  if (!strcasecmp(line, "audio")) {
    /* Same shape as `keys` and `health`: the state lives in WiPhone.ino, where the SIP,
     * emulator, music and vibro state are all visible; this just prints what it hands back.
     * ⚠ ONE say() PER LINE, like help(): say()'s buffer is 192 bytes and this dump is ~260 with
     * the rtp line (0.9.79) and ~420 with the leak warning. As one say() the rtp line — the
     * hot-mic check — was the part that got cut. */
    extern int audioStateDump(char* out, int cap);
    extern int musicStateDump(char* out, int cap);
    char buf[512];
    audioStateDump(buf, sizeof(buf));
    sayLines(buf);
    /* The music feed's three lines after it, through the same buffer (not appended: 512 is
     * what this task's stack was given for this, and the two together run past it). */
    musicStateDump(buf, sizeof(buf));
    sayLines(buf);
    return;
  }
  /* `music` — the feed alone; `music reset` zeroes its counters (a clean window for a bench:
   * reset, wait, read); `music swap on|off` flips the mono sample-pair swap, live, for an A/B
   * by ear (see MusicFeed::swapPairs). Nothing here is stored. */
  if (!strncasecmp(line, "music", 5) && (line[5] == '\0' || line[5] == ' ')) {
    extern Audio* audio;
    extern int musicStateDump(char* out, int cap);
    const char* arg = line + 5;
    while (*arg == ' ') arg++;
    if (!strcasecmp(arg, "reset")) {
      audio->musicResetStats();
      say("music: counters zeroed\n");
    } else if (!strncasecmp(arg, "swap", 4)) {
      const char* v = arg + 4;
      while (*v == ' ') v++;
      if (!strcasecmp(v, "on") || !strcasecmp(v, "off")) {
        audio->musicSetSwap(!strcasecmp(v, "on"));
      } else if (*v) {
        say("music: usage music swap on|off\n");
        return;
      }
      say("music: pair swap %s (from the next frame decoded)\n", audio->musicSwap() ? "ON" : "off");
      return;
    } else if (*arg) {
      say("music: usage music | music reset | music swap on|off\n");
      return;
    }
    char buf[512];
    musicStateDump(buf, sizeof(buf));
    sayLines(buf);
    return;
  }
  /* `audio tone old|clean|flat` — the codec's bass boost / treble shelf / de-emphasis, live
   * (a codec reconfigure if it is powered), NOT stored: a reboot is back to `clean`. For the
   * A/B by ear that picks the default; see WM8750::setTone() for why it changed. */
  if (!strncasecmp(line, "audio tone", 10) && (line[10] == '\0' || line[10] == ' ')) {
    extern Audio* audio;
    static const char* const TONES[] = {"old", "clean", "flat"};
    const char* arg = line + 10;
    while (*arg == ' ') arg++;
    if (*arg) {
      int t = -1;
      for (int i = 0; i < 3; i++) {
        if (!strcasecmp(arg, TONES[i])) {
          t = i;
        }
      }
      if (t < 0) {
        say("audio tone: usage audio tone old|clean|flat\n");
        return;
      }
      audio->setCodecTone((uint8_t)t);
    }
    const uint8_t now = audio->codecTone();
    say("audio tone: %s%s\n", now < 3 ? TONES[now] : "?",
        now == 0 ? " (0.9.78: linear +9 dB bass, -6 dB treble, 44.1k de-emphasis)" :
        now == 1 ? " (adaptive bass boost, no treble cut, no de-emphasis)" : " (no tone control)");
    return;
  }
  /* `audio orphan` — arm an RTP session with no call (receive only: no microphone, nothing
   * sent) so the hot-mic backstop can be watched firing. See audioBenchOrphan(). */
  if (!strcasecmp(line, "audio orphan")) {
    extern bool audioBenchOrphan();
    if (!audioBenchOrphan()) {
      say("audio orphan: refused - a call is live or ringing, a game owns the device, a "
          "session is already armed, or something is playing (pause the music first)\n");
      return;
    }
    say("audio orphan: RTP receive armed on port 5004 with NO call - expect 'AUDIO: RTP session "
        "armed with no live call' within %u ms, then `audio` armed=0\n",
        (unsigned)RTP_ORPHAN_RELEASE_MS);
    return;
  }
  /* `keys raw` — what the CHIP actually sent, oldest first. The counters say a release
   * went missing; only this says whether it was never emitted, mis-decoded, or stranded. */
  if (!strcasecmp(line, "keys raw")) {
    /* ⚠ ONE say() PER ENTRY, like help(): say()'s buffer is 192 bytes, and the trace as one
     * string showed the OLDEST twelve of 64 entries and dropped the rest without a word —
     * the part of a hold where it went wrong (found chasing the map's hold, 2026-09-20). */
    extern bool keypadTraceLine(int i, char* out, int cap);
    char buf[48];
    int shown = 0;
    sayLine("keys raw (oldest first):");
    for (int i = 0; keypadTraceLine(i, buf, sizeof(buf)); i++) {
      if (buf[0]) {
        sayLine(buf);
        shown++;
      }
    }
    if (!shown) {
      sayLine("(nothing recorded - press some keys)");
    }
    return;
  }

  /* `chan <url>` — apply a channel invite straight over the cable. Born of a real jam:
   * a chip erase cost the phone its channels, and re-sharing them over the mesh was
   * impossible — the RAK's 2.7 firmware NAKs every DM to this phone with
   * PKI_SEND_FAIL_PUBLIC_KEY (the phone implements no PKC), and an invite broadcast on
   * the primary channel would hand the PSK to everyone in RF range. Serial has neither
   * problem: private by cable, works when the mesh is exactly the broken part. */
  if (!strncasecmp(line, "chan ", 5)) {
    const char* url = line + 5;
    while (*url == ' ') {
      url++;
    }
    int added = meshService.applyChannelUrl(url);
    say("chan: %d channel(s) added\n", added);
    return;
  }
  /* `sip` — is an account actually LOADED and registered? Added while chasing "typing a
   * number doesn't complete": the completion (and the mirror, and texting) all hang off
   * the ACTIVE account's URI, and an account that was typed into the list but never
   * SELECTED is invisible to all of them. This prints the truth in one line. */
  if (!strcasecmp(line, "sip")) {
    extern GUI gui;
    const char* uri = gui.state.fromUriDyn;
    say("sip: account %s%s%s | registered: %s | wifi: %s\n",
        (uri && uri[0]) ? "LOADED (" : "NOT LOADED",
        (uri && uri[0]) ? uri : "",
        (uri && uri[0]) ? ")" : " - open SIP accounts and SELECT one",
        gui.state.sipRegistered ? "yes" : "no",
        (WiFi.status() == WL_CONNECTED) ? "up" : "DOWN");
    return;
  }
  if (!strcasecmp(line, "pki")) {
    reportPki();
    return;
  }
  /* `pki forget <!node>` — re-trust ONE node's key (MeshtasticService::forgetNodeKey) instead of
   * "Clear nodes", which wipes every node, name and star to fix one MISMATCH. */
  if (!strncasecmp(line, "pki forget", 10)) {
    const char* a = line + 10;
    while (*a == ' ') {
      a++;
    }
    if (*a == '!') {
      a++;
    }
    char* end = NULL;
    const unsigned long node = strtoul(a, &end, 16);
    if (!*a || !end || *end || node == 0) {
      say("pki forget: usage `pki forget !xxxxxxxx` (a node from `pki`)\n");
    } else if (meshService.forgetNodeKey((uint32_t)node)) {
      say("pki forget: !%08lx's key dropped - the next NodeInfo it sends is trusted "
          "(`announce` on that phone sends one now)\n", node);
    } else {
      say("pki forget: !%08lx is not in the node list\n", node);
    }
    return;
  }
  /* Predictive text on/off from the console. The Settings row does the same thing; this
   * exists because the feature has to be testable over the serial bridge without a finger
   * on the phone, and because turning it off in the field must not need a reflash. */
  if (!strncasecmp(line, "t9", 2) && (line[2] == '\0' || line[2] == ' ')) {
    extern GUI gui;
    const char* arg = line[2] ? line + 3 : "";
    while (*arg == ' ') {
      arg++;
    }
    if (!strcasecmp(arg, "on")) {
      gui.state.t9Enabled = true;
    } else if (!strcasecmp(arg, "off")) {
      gui.state.t9Enabled = false;
    } else if (!strcasecmp(arg, "reload")) {
      /* Re-read the card after an upload. The alternative is a reboot, which loses the
       * uploader session you just used to put the file there. */
      const int n = t9ExtraLoad(gui.state.cardPresent);
      gui.state.t9.setExtra(t9ExtraGet());
      say("t9: reloaded, %d extra words (%s)\n", n, t9ExtraStatus());
      return;
    } else if (*arg) {
      say("t9: say `t9 on`, `t9 off`, `t9 reload`, or `t9` to see the state\n");
      return;
    }
    say("t9: predictive text %s  (this field opted in: %s, word pending: %s)\n",
        gui.state.t9Enabled ? "ON" : "OFF",
        gui.state.t9Field ? "yes" : "no",
        gui.state.t9.pending() ? gui.state.t9.digits() : "no");
    /* The extra dictionary is invisible until it is wrong, so say what happened to it. A file
     * that failed to load looks exactly like a file with none of your words in it. */
    say("  extra words: %d from %s  (%s)\n", t9ExtraCount(), T9_EXTRA_PATH, t9ExtraStatus());
    return;
  }

  if (!strcasecmp(line, "heap")) {
    reportHeap();
    return;
  }
  /* `tlstest <url> [n]` — the TLS-in-PSRAM experiment (tile_fetch.h). `tlstest` alone
   * prints the last run's numbers. */
  if (!strncasecmp(line, "tlstest", 7) && (line[7] == '\0' || line[7] == ' ')) {
    const char* p = line + 7;
    while (*p == ' ') p++;
    if (!*p) {
      tileFetchBenchReport(sayLine);
      return;
    }
    char url[512];
    size_t n = 0;
    while (*p && *p != ' ' && n < sizeof(url) - 1) url[n++] = *p++;
    url[n] = '\0';
    while (*p == ' ') p++;
    const int count = *p ? atoi(p) : 1;
    if (tileFetchBenchStart(url, count)) {
      say("tlstest: started, %d GET(s) of %s - `tlstest` for the result\n", count < 1 ? 1 : count, url);
    } else {
      say("tlstest: NOT started -\n");
      tileFetchBenchReport(sayLine);
    }
    return;
  }
  /* `ai` / `ai last` / `ai ask <question>` / `ai topic` / `ai clear` / `ai reload` - Menu > AI
   * from the cable (ai_net.h). `ai ask` takes the app's own path (the gates, the worker, the
   * saved chat); `ai` then says how it went, `ai last` prints the answer. 🛑 NOTHING HERE TAKES
   * OR PRINTS A KEY: panicwatch keeps every console byte in /tmp for good. `ai` gives the key's
   * LENGTH and where the file is, and the question fits the console's 512-byte line. */
  if (!strncasecmp(line, "ai", 2) && (line[2] == '\0' || line[2] == ' ')) {
    const char* p = line + 2;
    while (*p == ' ') p++;
    if (!*p || !strcasecmp(p, "reload")) {
      if (*p) {
        aiKeyReload();
      }
      aiReport(sayLine);
      return;
    }
    if (!strcasecmp(p, "last")) {
      aiReportLast(sayLine);
      return;
    }
    if (!strcasecmp(p, "topic")) {
      aiChatNewTopic();
      say("ai: the next question starts a new topic\n");
      return;
    }
    if (!strcasecmp(p, "clear")) {
      aiChatClear();
      say("ai: chat cleared, %s removed\n", AI_CHAT_FILE);
      return;
    }
    if (!strcasecmp(p, "retry on") || !strcasecmp(p, "retry off")) {
      aiSetRetryTimeouts(!strcasecmp(p, "retry on"));      // the menu row's setting, saved the same way
      say("ai: retry if it times out: %s (the next question)\n", aiRetryTimeouts() ? "on" : "off");
      return;
    }
    if (!strncasecmp(p, "ask ", 4)) {
      const char* q = p + 4;
      char why[96];
      if (aiAsk(q, why, sizeof(why))) {
        say("ai: asking (%u chars) - `ai` for progress, `ai last` for the answer\n", (unsigned)strlen(q));
      } else {
        say("ai: NOT asked - %s\n", why);
      }
      return;
    }
    say("ai: ai | ai ask <question> | ai last | ai topic | ai clear | ai reload | ai retry on|off\n");
    return;
  }
  /* `replay` — the mesh-history replay's whole state in one line (the feature
   * is otherwise invisible on this phone, which is the point: it exists so
   * COVEY can ask what it missed — docs/replay-spec.md). */
  /* `nbr` — who is in DIRECT earshot, with signal and age. The map-building
   * diagnostic: a node listed here is one hop away, and anything NOT listed we
   * only ever heard through a relay. */
  /* `nbr on|4h|off` mirrors the My node > Neighbor info row (same pref), and
   * `nbr now` forces one announce — the bench cannot wait an hour to find out
   * whether the packet is well formed. */
  if (!strncasecmp(line, "nbr ", 4)) {
    const char* arg = line + 4;
    while (*arg == ' ') arg++;
    if (!strcasecmp(arg, "on") || !strcasecmp(arg, "1h")) {
      meshService.setNeighborInterval(3600);
      say("nbr: announcing every 1h\n");
    } else if (!strcasecmp(arg, "4h")) {
      meshService.setNeighborInterval(14400);
      say("nbr: announcing every 4h\n");
    } else if (!strcasecmp(arg, "off")) {
      meshService.setNeighborInterval(0);
      say("nbr: announcing OFF\n");
    } else if (!strcasecmp(arg, "now")) {
      const int n = meshService.announceNeighborsNow();
      say("nbr: queued on %d private channel(s)%s\n", n,
          n ? " - they go out one after another (`radio`)" : " - none configured, nothing sent");
    } else {
      say("nbr: usage nbr on|4h|off|now\n");
    }
    return;
  }
  if (!strcasecmp(line, "nbr")) {
    const uint32_t iv = meshService.getNeighborInterval();
    const char* chn = meshService.neighborChannelName();
    say("nbr: announce %s%s | channel %s | %d direct neighbour(s)\n",
        iv ? "every " : "OFF", iv ? (iv == 3600 ? "1h" : "4h") : "",
        chn ? chn : "NONE (primary is public - nothing sent)",
        meshService.getDirectNeighborCount());
    for (int i = 0; i < meshService.getDirectNeighborCount(); i++) {
      uint32_t node = 0, age = 0;
      int snr = 0;
      if (meshService.getDirectNeighbor(i, &node, &snr, &age)) {
        say("  !%08x  snr %d dB  heard %lum ago\n", (unsigned)node, snr,
            (unsigned long)(age / 60000UL));
      }
    }
    if (meshService.lastNeighborTxMs()) {
      say("nbr: last announce %lum ago, %d neighbour(s) in it\n",
          (unsigned long)((millis() - meshService.lastNeighborTxMs()) / 60000UL),
          meshService.lastNeighborTxCount());
    } else {
      say("nbr: never announced yet\n");
    }
    return;
  }
  /* `radio` — what the LoRa radio has done this boot. Exists because transmits stopped blocking
   * the loop in 0.9.79 (mesh_txq.h): before, every frame was visible as a STALL line; now a
   * frame is invisible unless something counts it. `last` is the proof on the bench — the air
   * time the loop SAW (start to the pass that read TxDone: one pass, ~5 ms, over the truth)
   * against meshLoraAirtimeMs(), which is exact. They should agree to a few ms. The difference
   * is time the chip spent deaf in STANDBY; `longest deaf gap` is the worst of it this boot. */
  if (!strcasecmp(line, "radio")) {
    const MeshTxStats& t = meshService.txStats();
    static const char* const KIND[] = { "own", "ACK", "relay" };
    say("radio: %s | queue %d/%d (deepest %d) | relays waiting %d\n",
        !meshPhy.isReady() ? "absent"
          : (meshPhy.benchAsleep() ? "SLEEP(bench)" : (meshPhy.txBusy() ? "tx" : "rx")),
        meshService.txQueued(), meshService.txQueueCap(), meshService.txQueueMaxDepth(),
        meshService.relaysPending());
    say("radio: finished own=%lu relay=%lu | timeout=%lu | relays cancelled=%lu | "
        "refused (queue full)=%lu\n",
        (unsigned long)t.own, (unsigned long)t.relay, (unsigned long)t.timeout,
        (unsigned long)t.relayCancelled, (unsigned long)t.refused);
    if (t.lastLen) {
      say("radio: on air %lu ms this boot (computed) | last: %s %u B, %lu ms seen vs %lu ms "
          "predicted\n",
          (unsigned long)t.airMsTotal, t.lastKind < 3 ? KIND[t.lastKind] : "?",
          (unsigned)t.lastLen, (unsigned long)t.lastAirMs,
          (unsigned long)meshLoraAirtimeMs(t.lastLen));
      say("radio: longest deaf gap after a frame this boot %lu ms (TxDone to back in RX; an idle "
          "pass is ~5, a map frame up to ~100)\n", (unsigned long)t.worstDeafMs);
    } else {
      say("radio: nothing has finished on the air yet this boot\n");
    }
    say("radio: health reads garbled then right on retry: %lu this boot (bus glitches, NOT a lost "
        "radio - see MeshPhy::healthCheck)\n", (unsigned long)meshPhy.healthGlitches());
    return;
  }
  if (!strcasecmp(line, "replay")) {
    if (meshService.replayLastServedMs()) {
      say("replay: ring %d heard | pending tx %d pkt(s) | last served %d rec(s) %lus ago\n",
          meshService.replayRingCount(), meshService.replayPendingPackets(),
          meshService.replayLastServedN(),
          (unsigned long)((millis() - meshService.replayLastServedMs()) / 1000));
    } else {
      say("replay: ring %d heard | pending tx %d pkt(s) | never served a request\n",
          meshService.replayRingCount(), meshService.replayPendingPackets());
    }
    return;
  }
  if (!strcasecmp(line, "pos")) {
    reportPos();
    return;
  }
  if (!strncasecmp(line, "pos ", 4)) {
    const char* arg = line + 4;
    while (*arg == ' ') {
      arg++;
    }
    if (!strncasecmp(arg, "every", 5)) {
      const char* n = arg + 5;
      while (*n == ' ') {
        n++;
      }
      if (!strcasecmp(n, "off")) {
        meshService.setPosInterval(0);
        say("pos: reporting OFF\n");
        return;
      }
      const uint32_t s = (uint32_t)strtoul(n, NULL, 10);
      /* 🔑 300 s floor, RAISED FROM 60 s: the bench must not be able to persist a
       * cadence the GUI cannot represent or undo. At 60 s this is 518 ms of air
       * every minute — 0.86% duty, ~15x the most aggressive GUI option (300 s,
       * 0.173%) and ~5x its default — on a band shared with COVEY and everything
       * else in range. Worse, it SURVIVES A REBOOT in NVS while the menu renders
       * it as "every 1 min" and collapses it to off on the next press, so the
       * user cannot see it or fix it from the phone. 300 s is the most aggressive
       * value the GUI itself offers; the bench does not get to invent a faster one. */
      if (s < 300 || s > 86400) {
        say("pos: interval out of range (300..86400 seconds, or off)\n");
        return;
      }
      const char* no = serialPosConsentRefusal();
      if (no) {
        say("pos: refused - %s;\n", no);
        say("pos: set reporting on the phone (Meshtastic > My node > Position)\n");
        return;
      }
      meshService.setPosInterval(s);
      say("pos: reporting every %lus - first send is a full interval away\n",
          (unsigned long)s);
      return;
    }
    /* 🛑 `pos chan` IS REFUSED, DELIBERATELY. The consent that makes a location
     * beacon safe is the two-press confirm on the picker screen, where the word
     * PUBLIC is in the large font. A serial one-liner is not that consent, and
     * a bench command that can silently retarget somebody's live location
     * defeats every safeguard on the screen. Read-only here, on purpose. */
    if (!strncasecmp(arg, "chan", 4)) {
      say("pos: refused - choose the channel on the phone\n");
      say("pos: Meshtastic > My node > Send to (the public channel needs two presses)\n");
      return;
    }
    /* `pos now` — bench only: force one beacon regardless of the interval and
     * the movement gate. It still obeys every SAFETY rule (a source on, channel
     * named, channel present, channel not silently public, fix fresh enough),
     * because those are what the feature is for — and since 0.9.82 it never
     * sends to a public channel at all, confirmed or not: the cable is not the
     * on-screen consent (meshService.posNowBlockedReason).
     * It QUEUES the frame (mesh_txq.h); it no longer blocks ~518 ms in a send. */
    if (!strcasecmp(arg, "now")) {
      bool bench = false;
      const bool ok = meshService.sendSelfPositionNow(&bench);
      say("pos: forced beacon %s%s%s\n", ok ? "QUEUED" : "NOT SENT",
          ok && bench ? " - an INJECTED bench fix, sent as MANUAL (not a measurement)" : "",
          !ok ? "" : bench ? " (39 B on air with the clock set - see the MESH POSITION log line, then `radio`)"
                           : " (42 B on air with the clock set - see the MESH POSITION log line, then `radio`)");
      if (!ok) {
        const char* why = meshService.posNowBlockedReason();
        say("pos: %s\n", why ? why : "radio not ready, or no channels");
      }
      return;
    }
    /* `pos inject` — a BENCH fix (self_pos.h's `injected`): the map, the distances, the Almanac
     * and `pos now` on a private channel can be exercised with no sky and no phone. RAM only.
     * 🛑 The schedule never sends one, and `pos now` refuses a public channel with one: a
     * serial line is not the on-screen consent a location on a public channel needs. Nothing
     * typed here is echoed back — use dummy coordinates anyway (40.0 -100.0). */
    if (!strncasecmp(arg, "inject", 6)) {
      const char* a = arg + 6;
      while (*a == ' ') {
        a++;
      }
      if (!strcasecmp(a, "clear")) {
        say("pos: %d bench fix(es) dropped\n", meshService.clearInjectedFixes());
        return;
      }
      int src = SELF_NONE;
      if (!strncasecmp(a, "gps ", 4)) {
        src = SELF_GPS;
        a += 4;
      } else if (!strncasecmp(a, "phone ", 6)) {
        src = SELF_PHONE;
        a += 6;
      }
      char* e = NULL;
      const double lat = src ? strtod(a, &e) : 0.0;
      const bool latOk = src && e && e != a;
      const char* b = latOk ? e : a;
      const double lon = latOk ? strtod(b, &e) : 0.0;
      const bool lonOk = latOk && e && e != b;
      int sats = -1, hdopX10 = -1;
      if (lonOk) {
        const char* c = e;
        char* e2 = NULL;
        const long sv = strtol(c, &e2, 10);
        if (e2 && e2 != c) {
          sats = (int)sv;
          const char* d = e2;
          const double hv = strtod(d, &e2);
          if (e2 && e2 != d && hv >= 0.0 && hv < 100.0) {
            hdopX10 = (int)(hv * 10.0 + 0.5);
          }
        }
      }
      if (!lonOk || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0 ||
          (lat == 0.0 && lon == 0.0) || sats < -1 || sats > 99) {
        say("pos: usage - pos inject gps|phone <lat> <lon> [sats hdop] | pos inject clear\n");
        return;
      }
      const bool on = (src == SELF_GPS) ? meshService.isGpsEnabled() : meshService.isPhoneEnabled();
      if (!on) {
        say("pos: refused - %s is off (`%s` first)\n",
            src == SELF_GPS ? "the GPS" : "Phone location",
            src == SELF_GPS ? "gps on" : "phonepos auto");
        return;
      }
      meshService.injectSelfFix(src, (int32_t)(lat * 1e7 + (lat < 0 ? -0.5 : 0.5)),
                                (int32_t)(lon * 1e7 + (lon < 0 ? -0.5 : 0.5)), sats, hdopX10);
      say("pos: bench %s fix set (RAM only; never sent by the schedule; `pos inject clear`)\n",
          selfSrcName(src));
      return;
    }
    say("pos: usage - pos | pos every <secs>|off | pos now | pos inject ...  (channel: on the phone)\n");
    return;
  }
  /* `netloc` — Wi-Fi location (geoloc_net.h). Status prints counts, verdicts and accuracies only:
   * never a MAC, an SSID or a coordinate.
   * 🛑 THE CONSENT RULE: the cable may set only off or map (geoSettingSerialOk). "map + mesh" puts
   * a position on other people's maps and is chosen on the phone's screen, like `pos chan`. */
  if (!strcasecmp(line, "netloc") || !strncasecmp(line, "netloc ", 7)) {
    const char* arg = line[6] ? line + 7 : "";
    while (*arg == ' ') {
      arg++;
    }
    if (!*arg) {
      geolocReport(sayLine);
      return;
    }
    if (!strncasecmp(arg, "answer", 6) && (arg[6] == ' ' || !arg[6])) {
      int v = GEO_V_NONE;
      char why[96];
      if (!geolocBenchAnswer(arg + 6, &v, why, sizeof(why))) {
        say("netloc: %s\n", why);
        return;
      }
      say("netloc: BENCH answer folded - %s%s\n", geoVerdictName(v),
          v == GEO_V_LOCATED ? " (a bench fix: on the map, never sent by the schedule)" : "");
      return;
    }
    if (!strcasecmp(arg, "now")) {
      if (geolocSetting() == GEO_SET_OFF) {
        say("netloc: Wi-Fi location is off - `netloc map` first\n");
        return;
      }
      geolocAskNow();
      say("netloc: asked - a look runs when the gates allow (`netloc` to watch)\n");
      return;
    }
    const int v = geoSettingParse(arg);
    if (v < 0) {
      say("netloc: usage - netloc | off | map | now | answer <A> <B>\n");
      return;
    }
    if (!geoSettingSerialOk(v)) {
      say("netloc: refused - `map + mesh` is chosen on the phone (Meshtastic > My node >\n");
      say("netloc: Wi-Fi location): it puts a position on other people's maps\n");
      return;
    }
    geolocSetSetting(v);
    say("netloc: Wi-Fi location %s (persists)%s\n", geoSettingName(v),
        v == GEO_SET_OFF ? "" : " - sends nearby Wi-Fi IDs to beaconDB when a position is wanted");
    return;
  }
  /* `phonepos` — the hotspot phone's GPS (phone_pos.h). Status prints counters and words only:
   * never a coordinate, an SSID or the gateway's address. */
  if (!strcasecmp(line, "phonepos") || !strncasecmp(line, "phonepos ", 9)) {
    const char* arg = line[8] ? line + 9 : "";
    while (*arg == ' ') {
      arg++;
    }
    if (!*arg) {
      phonePosDumpStatus(sayLine);
      return;
    }
    if (!strcasecmp(arg, "off")) {
      phonePosSetAuto(false);
      say("phonepos: Phone location OFF (persists)\n");
      return;
    }
    if (!strcasecmp(arg, "auto") || !strcasecmp(arg, "on")) {
      /* 🛑 THE CONSENT RULE (serialPosConsentRefusal). With reporting armed to a PUBLIC channel,
       * switching a source on can start a location going out in the clear: that is decided on
       * the phone, where My node says PUBLIC in the large font, never by a line on the cable. */
      const char* no = (!meshService.isPhoneEnabled() && meshService.getPosInterval() != 0)
                           ? serialPosConsentRefusal() : NULL;
      if (no) {
        say("phonepos: refused - reporting is armed and %s;\n", no);
        say("phonepos: switch Phone location on from the phone (Meshtastic > My node)\n");
        return;
      }
      phonePosSetAuto(true);
      say("phonepos: Phone location AUTO (persists) - asks the gateway on 10110, 11123%s\n",
          phonePosCustomPort() ? " and your port" : "");
      return;
    }
    if (!strncasecmp(arg, "port", 4)) {
      const char* n = arg + 4;
      while (*n == ' ') {
        n++;
      }
      if (!*n) {
        say("phonepos: custom port %u%s\n", (unsigned)phonePosCustomPort(),
            phonePosCustomPort() ? "" : " (none)");
        return;
      }
      char* e = NULL;
      const long v = strtol(n, &e, 10);
      if (!e || e == n || *e || v < 0 || v > 65535) {
        say("phonepos: port out of range (0..65535; 0 = none)\n");
        return;
      }
      phonePosSetCustomPort((uint16_t)v);
      say("phonepos: custom port %ld%s (persists) - asking again from the top\n", v,
          v ? "" : " (none)");
      return;
    }
    if (!strncasecmp(arg, "at", 2) && (arg[2] == ' ' || !arg[2])) {
      const char* a = arg + 2;
      while (*a == ' ') {
        a++;
      }
      if (!strcasecmp(a, "off") || !*a) {
        phonePosBenchClear();
        say("phonepos: bench address cleared - asking the network's gateway again\n");
        return;
      }
      uint8_t ip[4];
      uint16_t port = 0;
      if (!selfPhoneParseAt(a, ip, &port)) {
        say("phonepos: usage - phonepos at <a.b.c.d>[:port] | phonepos at off\n");
        return;
      }
      phonePosBenchAt(ip, port);
      // The address was typed by whoever is at the cable; it is not repeated back.
      say("phonepos: BENCH address set (RAM only)%s; its fixes are bench fixes - `phonepos` to watch\n",
          port ? ", its port tried with 10110/11123" : "");
      return;
    }
    say("phonepos: usage - phonepos | auto | off | port <n> | at <ip>[:port] | at off\n");
    return;
  }
  /* `gps` — the woods backplate's GPS half, dormant until `gps on`. The status
   * line leads with the counters because they answer the first bench question:
   * bytes rising + sentences 0 = wrong baud; both 0 = wiring; sentences rising
   * with no fix = give the antenna sky. */
  if (!strcasecmp(line, "gps") || !strncasecmp(line, "gps ", 4)) {
#ifdef USER_SERIAL
    const char* arg = line[3] ? line + 4 : "";
    while (*arg == ' ') {
      arg++;
    }
    if (!strcasecmp(arg, "on") || !strcasecmp(arg, "off")) {
      /* 🛑 Not under `cpu method old` (0.9.79 dev): typed with the screen off the clock is at
       * 80, and the gate's next pass (busy: gGpsNmea) would setCpuFrequencyMhz(240) into the
       * APB-callback deadlock (WiPhone.ino, the gGpsNmea note). `cpu method old` refuses the
       * other order. The mesh app's GPS row needs no guard: it is toggled with the screen lit,
       * i.e. already at full speed, so no switch follows. */
      if (!strcasecmp(arg, "on") && !gGpsNmea && cpuClockMethod() == CPU_METHOD_OLD) {
        say("gps on: refused - cpu method is OLD, whose next clock switch would take the APB-callback\n");
        say("  deadlock with the reader streaming (WiPhone.ino, the gGpsNmea note) - `cpu method new` first\n");
        return;
      }
      /* 🛑 The consent rule (serialPosConsentRefusal): the receiver is a location source, and a
       * beacon armed to a public channel that "GPS is off" was holding must not start from here. */
      if (!strcasecmp(arg, "on") && !gGpsNmea && meshService.getPosInterval() != 0) {
        const char* no = serialPosConsentRefusal();
        if (no) {
          say("gps on: refused - reporting is armed and %s;\n", no);
          say("  switch the GPS on from the phone (Meshtastic > My node), or `pos every off` first\n");
          return;
        }
      }
      gGpsNmea = !strcasecmp(arg, "on");
      Preferences p;
      p.begin("wpmesh", false);
      p.putBool("gpsen", gGpsNmea);
      p.end();
      gpsApplyBaud(gGpsNmea);      // the two consumers do NOT share a baud
      /* ...and tell the service, or resolveReference() keeps answering "GPS"
       * with coordinates from a receiver that just stopped. One bool, no I/O. */
      meshService.setGpsEnabled(gGpsNmea);
      gGpsReader.reset();          // counters answer "since when", so start clean
      say("gps: NMEA reader %s (user UART rx=%d tx=%d @ %u)%s\n",
          gGpsNmea ? "ON" : "OFF", USER_SERIAL_RX, USER_SERIAL_TX,
          (unsigned)(gGpsNmea ? gGpsBaud : USER_SERIAL_BAUD),
          gGpsNmea ? "" : " - user-serial GUI path restored");
      return;
    }
    /* `gps baud <n>` — the bench's answer to a wrong-rate module, without a
     * reflash per guess. Persisted, and applied to the live port only while the
     * reader owns it. */
    if (!strncasecmp(arg, "baud", 4)) {
      const char* n = arg + 4;
      while (*n == ' ') {
        n++;
      }
      if (!*n) {
        say("gps: baud %u (default %u)\n", (unsigned)gGpsBaud,
            (unsigned)GPS_SERIAL_BAUD_DEFAULT);
        return;
      }
      uint32_t b = strtoul(n, NULL, 10);
      if (b < 1200 || b > 921600) {
        say("gps: baud out of range (1200..921600)\n");
        return;
      }
      gGpsBaud = b;
      Preferences p;
      p.begin("wpmesh", false);
      p.putUInt("gpsbaud", gGpsBaud);
      p.end();
      if (gGpsNmea) {
        gpsApplyBaud(true);
        gGpsReader.reset();
      }
      say("gps: baud %u%s\n", (unsigned)gGpsBaud,
          gGpsNmea ? " - applied, counters reset" : " - saved (reader is off)");
      return;
    }
    /* `gps raw` — bytes climbing with sentences at 0 has TWO causes and the
     * status line cannot tell them apart. These bytes can: readable ASCII means
     * the baud is right and something else is wrong, `b5 62` means the module is
     * talking UBX binary, and unreadable non-UBX means the rate is still off. */
    if (!strcasecmp(arg, "raw")) {
      uint8_t buf[64];
      int n = gpsRawSnapshot(buf, sizeof(buf));
      if (!n) {
        say("gps: no bytes seen%s\n", gGpsNmea ? "" : " (reader is off)");
        return;
      }
      say("gps: last %d bytes @ %u baud\n", n, (unsigned)gGpsBaud);
      for (int i = 0; i < n; i += 16) {
        char hex[3 * 16 + 1], asc[17];
        int m = (n - i < 16) ? n - i : 16;
        for (int j = 0; j < m; j++) {
          snprintf(hex + 3 * j, 4, "%02x ", buf[i + j]);
          asc[j] = (buf[i + j] >= 0x20 && buf[i + j] < 0x7f) ? (char)buf[i + j] : '.';
        }
        hex[3 * m] = 0;
        asc[m] = 0;
        say("  %-48s |%s|\n", hex, asc);
      }
      return;
    }
    say("gps: reader %s @ %u  bytes=%u sentences=%u badck=%u overrun=%u\n",
        gGpsNmea ? "ON" : "OFF (gps on to start)",
        (unsigned)(gGpsNmea ? gGpsBaud : USER_SERIAL_BAUD),
        gGpsReader.bytes(), gGpsReader.sentences(),
        gGpsReader.badChecksum(), gGpsReader.overruns());
    if (gGpsNmea && gGpsReader.bytes() > 200 && gGpsReader.sentences() == 0) {
      say("gps: bytes flow, no sentences - wrong baud, or a module talking binary.\n");
      say("gps: `gps raw` decides it; `gps baud 9600|38400|57600|115200` retunes.\n");
    }
    int32_t la = 0, lo = 0;
    uint32_t age = 0;
    int sats = -1, hdop = -1, skySats = -1, skyHdop = -1;
    /* Two different numbers since 0.9.82 (review 2026-10-11): the fix's own rating - what the
     * map, the beacon and the Almanac judge it by - and the last GGA's word on the sky now. */
    meshService.getGpsSky(&skySats, &skyHdop);
    if (meshService.getGpsFix(&la, &lo, &age, &sats, &hdop)) {
      say("gps: fix %d.%05d,%d.%05d  age %lus  sats=%d hdop=%d.%d\n",
          (int)(la / 10000000), abs((int)((la % 10000000) / 100)),
          (int)(lo / 10000000), abs((int)((lo % 10000000) / 100)),
          (unsigned long)(age / 1000), sats,
          hdop >= 0 ? hdop / 10 : -1, hdop >= 0 ? hdop % 10 : 0);
      say("gps: sky now (last GGA): sats=%d hdop=%d.%d\n", skySats,
          skyHdop >= 0 ? skyHdop / 10 : -1, skyHdop >= 0 ? skyHdop % 10 : 0);
    } else {
      say("gps: no fix yet (sats in view: %d)\n", skySats);
    }
    /* GPS TIME (0.9.79): what the last RMC did to the clock, in one line. `clock` has the rest. */
    say("gps: time -> %s (`clock` for the rest)\n",
        ntpClock.diag().gpsVerdict != CLK_NONE ? clockVerdictText(ntpClock.diag().gpsVerdict)
                                               : "no RMC judged yet");
#else
    say("gps: USER_SERIAL not compiled into this build\n");
#endif
    return;
  }
  /* `clock` (or `time`) — what time the phone thinks it is, WHO said so, and what the GPS and
   * mesh paths last decided (0.9.79; the rules are clock_source.h's). The first thing to paste
   * when a time looks wrong, and the hardware proof of GPS time: outdoors with `gps on`, the
   * gps line goes "one good reading..." -> "SET - the clock was not set" about a second after
   * the first fix, and then sits on "kept - agrees within 2 s". */
  if (!strcasecmp(line, "clock") || !strcasecmp(line, "time")) {
    reportClock();
    return;
  }
  /* `tz` — the clock's time zone (0.9.82): Auto from a MEASURED fix (never a bench fix, never near
   * a zone line, two looks 2 min apart), a named zone with its DST rule, or the typed offset. The
   * setting commands are the Settings screen's own calls (tzSvcSetMode / tzSvcSetCustomMinutes):
   * saved, and applied at once. `tz test` is the lookup alone; it changes nothing. */
  if (!strcasecmp(line, "tz") || !strncasecmp(line, "tz ", 3)) {
    const char* arg = line + 2;
    while (*arg == ' ') {
      arg++;
    }
    if (!*arg) {
      reportTz();
      return;
    }
    if (!strncasecmp(arg, "test", 4) && (arg[4] == '\0' || arg[4] == ' ')) {
      tzTest(arg + 4);
      return;
    }
    if (!strcasecmp(arg, "auto")) {
      tzSvcSetMode(TZ_MODE_AUTO, -1);
      say("tz: Auto - saved. The zone comes from a measured fix; until one, what the clock showed"
          " (a named zone stays, Custom's typed offset stays)\n");
      reportTz();
      return;
    }
    if (!strcasecmp(arg, "forget")) {
      tzSvcForgetAuto();
      say("tz: Auto's zone forgotten (NVS too)\n");
      reportTz();
      return;
    }
    if (!strncasecmp(arg, "custom", 6) && (arg[6] == '\0' || arg[6] == ' ')) {
      const char* off = arg + 6;
      while (*off == ' ') {
        off++;
      }
      if (*off) {
        float h = 0;
        const char* err = NULL;
        const int m = Clock::parseTimeZone(off, h, err) ? tzHoursToMinutes(h) : 1000000;
        if (!tzCustomOk(m)) {
          say("tz: '%s' is not an offset (-12 to +14 hours: -7, -3:30, 5.75)%s%s\n", off,
              err ? " - " : "", err ? err : "");
          return;
        }
        const bool stored = tzSvcSetCustomMinutes(m, true);
        say("tz: typed offset saved%s\n", stored ? " (configs.ini + NVS)" : " in NVS only - configs.ini not written");
      }
      tzSvcSetMode(TZ_MODE_CUSTOM, -1);
      say("tz: Custom - the typed offset, no DST\n");
      reportTz();
      return;
    }
    const int n = tzNamedFind(arg);
    if (n >= 0) {
      tzSvcSetMode(TZ_MODE_NAMED, n);
      say("tz: %s - saved; its DST changes happen by themselves\n", tzNamed(n)->display);
      reportTz();
      return;
    }
    say("tz: usage tz | tz auto | tz custom [offset] | tz <zone> | tz test <lat> <lon> | tz forget\n");
    say("tz: zones: pacific mountain arizona central eastern atlantic newfoundland alaska hawaii saskatchewan\n");
    return;
  }
  /* `sun` — legal light at the reference place, offline. The most-asked
   * question of a hunting day, answered from pure math + the coordinates the
   * mesh already delivered. Times print in LOCAL clock (the tz offset is
   * whatever the phone's clock is configured with). Since 0.9.80 the same computation as the
   * Almanac and the Meshtastic Sun screen (almanacLegalToday: astro.cpp + the legal-light rule
   * of Almanac > Settings, which the header line names); `almanac` prints the whole day. */
  if (!strcasecmp(line, "sun") || !strncasecmp(line, "sun ", 4)) {
    if (!ntpClock.isTimeKnown()) {
      say("sun: clock not set yet (needs NTP on WiFi, a GPS fix, or mesh time - see `clock`)\n");
      return;
    }
    if (ntpClock.getSource() == CLOCK_SRC_MESH) {
      /* Legal light is the one number here someone might act on. A mesh clock is adopted from
       * unauthenticated packets (clock_source.h) — usually right, never vouched for. */
      say("sun: WARNING the clock came from the MESH (lower trust) - check it before relying on this\n");
    }
    int32_t refLat, refLon;
    char refName[20];
    double la = 0, lo = 0;
    if (line[3] == ' ') {
      // `sun 47.6062,-122.3321` — explicit coordinates, for the bench.
      if (sscanf(line + 4, "%lf,%lf", &la, &lo) != 2) {
        say("sun: usage sun [lat,lon]\n");
        return;
      }
      strlcpy(refName, "given", sizeof(refName));
    } else if (!meshService.resolveReference(&refLat, &refLon, refName, sizeof(refName))) {
      say("sun: no place to compute for - hear a waypoint or set a pin first\n");
      return;
    } else {
      la = refLat * 1e-7;
      lo = refLon * 1e-7;
    }
    AlmanacLegal L;
    const int rc = almanacLegalToday(la, lo, &L);
    if (rc == ALM_SUN_BAD_DATE) {
      say("sun: the clock's date is outside %d-%d - refused\n", ALM_YEAR_MIN, ALM_YEAR_MAX);
      return;
    }
    if (rc != ALM_SUN_OK) {
      say("sun: computation refused (bad coordinates?)\n");
      return;
    }
    const int tzMin = L.tzS / 60;
    say("sun: %04d-%02d-%02d at '%s' (local, UTC%+d:%02d) - legal rule: %s\n", L.y, L.m, L.d,
        refName, tzMin / 60, abs(tzMin % 60),
        L.rule == ASTRO_LEGAL_CIVIL ? "civil twilight" : "30 min before/after the sun");
    struct Row { const char* label; int64_t at; int round; };
    const Row rows[4] = { { "first light", L.first, ALM_ROUND_UP },
                          { "sunrise", L.sun.rise, ALM_ROUND_NEAREST },
                          { "sunset", L.sun.set, ALM_ROUND_NEAREST },
                          { "last light", L.last, ALM_ROUND_DOWN } };
    for (int i = 0; i < 4; i++) {
      if (!rows[i].at) {
        say("  %-11s (does not occur today)\n", rows[i].label);
      } else {
        char hm[8];
        almFmtClock(rows[i].at, L.tzS, rows[i].round, hm, sizeof(hm));
        say("  %-11s %s\n", rows[i].label, hm);
      }
    }
    say("sun: %s\n", L.countdown);
    return;
  }
  /* `almanac [lat,lon] [+N|-N]` — everything the Almanac app shows for that day (TODAY, SUN, MOON,
   * SOLUNAR, POSITION, DATE), then the exact instants as UNIX seconds: the bench's proof against
   * PyEphem - in ONE pass (a bench command; the app computes in slices). `almanac cost` — what the
   * app's slices and builds measured on this phone; `almanac bench` — each piece timed now
   * (app_almanac.h). */
  if (!strcasecmp(line, "almanac") || !strncasecmp(line, "almanac ", 8)) {
    almanacConsole(line + 7, sayLine);
    return;
  }
  /* `wx` / `wx fetch` / `wx show` / `wx clear` — the Almanac's weather from the cable (0.9.81,
   * weather_net.h). `wx`: the cache's age, the place source and the ROUNDED point it was fetched
   * for, each host's status and last code, the handshake ms per host, the worker's stack and the
   * heap floors. `wx fetch` takes the Refresh path (the gates, the ground read, the worker) - the
   * line it prints never holds the URL or the coordinates; `wx show` prints the WEATHER screen's
   * rows through the same builder; `wx clear` deletes /wx/weather.txt. */
  if (!strncasecmp(line, "wx", 2) && (line[2] == '\0' || line[2] == ' ')) {
    almanacWeatherConsole(line + 2, sayLine);
    return;
  }
  /* `unread` — why is the white message icon lit? Counts the truth from the
   * message records themselves, repairs the three derived counters when they
   * disagree (drift keeps the icon lit with nothing to read), and NAMES a
   * thread that still holds unread so there is somewhere to go. */
  /* `unread clear` — mark EVERYTHING read. The one honest lever when unread
   * flags belong to a conversation that no longer exists on the phone (wiped
   * thread, remirrored history): there is no thread to open, so nothing else
   * can ever clear them. The user is asserting "I have seen everything". */
  if (!strcasecmp(line, "unread clear")) {
    extern GUI gui;
    int32_t n = gui.flash.messages.markAllRead();
    if (n < 0) {
      say("unread: message store not loaded\n");
      return;
    }
    gui.flash.messages.clearPreloaded();
    gui.state.unreadMessages = gui.flash.messages.hasUnread();
    say("unread: cleared %d - icon %s\n", (int)n,
        gui.state.unreadMessages ? "STILL LIT (report this)" : "off");
    return;
  }
  if (!strcasecmp(line, "unread")) {
    extern GUI gui;
    char from[64];
    int32_t n = gui.flash.messages.recountUnread(true, from, sizeof(from));
    if (n < 0) {
      say("unread: message store not loaded\n");
      return;
    }
    gui.state.unreadMessages = gui.flash.messages.hasUnread();
    say("unread: %d actually unread (counters repaired where they disagreed)\n", (int)n);
    if (n > 0) {
      say("unread: oldest unread is from %s - open that thread to clear it\n", from);
    } else {
      say("unread: icon %s\n", gui.state.unreadMessages ? "STILL LIT (report this)" : "now off");
    }
    return;
  }
  /* `announce` — one NodeInfo broadcast with want_response. The fast path for key
   * exchange: our packet carries our public key, and every hearer is asked to answer
   * with its own NodeInfo — which carries THEIR key. (Replies are damped by stock
   * firmware; give it a minute, then run `pki` to see what was learned.) */
  if (!strcasecmp(line, "announce")) {
    meshService.announceNodeInfo(true);
    say("announce: queued (watch for MESH ANNOUNCE above; `pki` in a minute to see keys)\n");
    return;
  }
  /* `dm !62b8d2fd hello` — send a direct message from the cable. Exists so PKC can be
   * proven end to end without touching the screen: the ACK the peer sends back (it only
   * ACKs what it DECODED) appears in this log as `MESH DM ACK ... err=0`. */
  if (!strncasecmp(line, "dm ", 3)) {
    const char* p = line + 3;
    while (*p == ' ') p++;
    if (*p == '!') p++;
    char* end = NULL;
    uint32_t node = (uint32_t)strtoul(p, &end, 16);
    if (node == 0 || !end || *end != ' ') {
      say("dm: usage dm <!nodehex> <text>\n");
      return;
    }
    while (*end == ' ') end++;
    if (!*end) {
      say("dm: empty message\n");
      return;
    }
    const MeshNode* peer = meshService.findNode(node);
    bool pki = peer && (peer->pkiFlags & MESH_NODE_HAS_KEY);
    bool ok = meshService.sendDirectMessage(node, end);
    say("dm: %s to !%08x (%s) - watch for 'MESH DM ACK ... err=0' = delivered\n",
        ok ? "queued" : "REFUSED", (unsigned)node,
        pki ? "PKI" : "LEGACY - no key, 2.5+ nodes drop it");
    if (!ok) {
      say("dm: reason: %s\n", meshService.lastSendError() ? meshService.lastSendError() : "?");
    }
    return;
  }
  /* `send <idx> <text>` — a CHANNEL text from the cable. Born of a gap in the receipt work:
   * `dm` could prove the DM receipt end to end (`MESH DM ACK ... err=0` -> "delivered"), but
   * the BROADCAST receipt had no way to be exercised without typing on the handset, so
   * "in mesh" shipped as the one unproven state. A broadcast is acknowledged implicitly —
   * hearing our own packet rebroadcast is the ack — so watch for `MESH RECEIPT: ... -> in
   * mesh` a second or two after this, which also proves somebody out there relayed it.
   * Index, not hash: `chans` prints the indices and a hash is not something anyone can type. */
  if (!strncasecmp(line, "send ", 5)) {
    const char* p = line + 5;
    while (*p == ' ') {
      p++;
    }
    char* end = NULL;
    long idx = strtol(p, &end, 10);
    if (!end || end == p || *end != ' ') {
      say("send: usage send <chan-index> <text>   (see `chans`)\n");
      return;
    }
    while (*end == ' ') {
      end++;
    }
    if (!*end) {
      say("send: empty message\n");
      return;
    }
    const MeshChannel* c = meshService.getChannel((int)idx);
    if (!c) {
      say("send: no channel at index %ld (see `chans`)\n", idx);
      return;
    }
    bool ok = meshService.sendChannelMessage(c->hash, end);
    if (ok) {
      say("send: queued on [%ld] '%s' - watch for 'MESH RECEIPT: ... -> in mesh'\n", idx, c->name);
    } else {
      say("send: REFUSED on [%ld] '%s'\n", idx, c->name);
      /* The same literal the compose screen shows after "Not sent: " — the bench sees what
       * the thumb would. `send 0 <233 x's>` is the way to prove the cap from the cable. */
      say("send: reason: %s\n", meshService.lastSendError() ? meshService.lastSendError() : "?");
    }
    return;
  }
  /* `star [<!nodehex>]` — bare, it lists what is starred; with a node, it toggles. Exists for
   * the same reason `send` does: the UI path is a key press on a screen, and a feature that
   * can only be exercised by a thumb cannot be proven from the cable. */
  if (!strncasecmp(line, "star", 4) && (line[4] == 0 || line[4] == ' ')) {
    const char* p = line + 4;
    while (*p == ' ') {
      p++;
    }
    if (!*p) {
      int shown = 0;
      for (int i = 0; i < meshService.getNodeCount(); i++) {
        const MeshNode* n = meshService.getNode(i);
        if (n && (n->pkiFlags & MESH_NODE_FAVOURITE)) {
          say("  * !%08x '%s'\n", (unsigned)n->nodeNum, n->name);
          shown++;
        }
      }
      say("star: %d starred (they sort to the top and are evicted last)\n", shown);
      return;
    }
    if (*p == '!') {
      p++;
    }
    uint32_t node = (uint32_t)strtoul(p, NULL, 16);
    if (!node) {
      say("star: usage star            (list)\n      star <!nodehex>  (toggle)\n");
      return;
    }
    bool on = meshService.toggleFavourite(node);
    say("star: !%08x is now %s\n", (unsigned)node, on ? "STARRED" : "not starred");
    return;
  }
  /* `bench` — time the SAME database-shaped workload on SPIFFS and on the SD card, on this
   * exact hardware, before deciding to move the database. The case for moving it rested on
   * "/health.log lives on SD and never stalls" — but that is ~130 bytes a minute, which says
   * nothing about an 8 KB write plus a create and a rename. And cheap SD cards run their own
   * wear levelling, which is a well-known source of unpredictable multi-hundred-ms pauses; SD
   * could easily just move the problem. Measure, do not reason. */
  if (!strcasecmp(line, "bench")) {
    extern GUI gui;
    const size_t N = 8192;
    uint8_t* buf = (uint8_t*)ps_malloc(N);
    if (!buf) {
      say("bench: no PSRAM\n");
      return;
    }
    memset(buf, 0xA5, N);
    for (int pass = 0; pass < 3; pass++) {
      // ---- SPIFFS ----
      uint32_t t0 = millis();
      File f = SPIFFS.open("/bench.tmp", "w");
      uint32_t tOpen = millis();
      if (f) {
        f.write(buf, N);
        uint32_t tWrite = millis();
        f.close();
        SPIFFS.remove("/bench.bin");
        uint32_t tRm = millis();
        SPIFFS.rename("/bench.tmp", "/bench.bin");
        say("bench spiffs[%d]: total=%ums open=%u write=%u rm=%u rename=%u\n", pass,
            (unsigned)(millis() - t0), (unsigned)(tOpen - t0), (unsigned)(tWrite - tOpen),
            (unsigned)(tRm - tWrite), (unsigned)(millis() - tRm));
      } else {
        say("bench spiffs[%d]: open FAILED\n", pass);
      }
      // ---- SD ----
      if (!gui.state.cardPresent) {
        say("bench sd[%d]: no card\n", pass);
        continue;
      }
      t0 = millis();
      File g = SD.open("/bench.tmp", FILE_WRITE);
      uint32_t gOpen = millis();
      if (g) {
        g.write(buf, N);
        uint32_t gWrite = millis();
        g.close();
        SD.remove("/bench.bin");
        uint32_t gRm = millis();
        SD.rename("/bench.tmp", "/bench.bin");
        say("bench sd[%d]:     total=%ums open=%u write=%u rm=%u rename=%u\n", pass,
            (unsigned)(millis() - t0), (unsigned)(gOpen - t0), (unsigned)(gWrite - gOpen),
            (unsigned)(gRm - gWrite), (unsigned)(millis() - gRm));
      } else {
        say("bench sd[%d]: open FAILED\n", pass);
      }
    }
    SPIFFS.remove("/bench.bin");
    if (gui.state.cardPresent) {
      SD.remove("/bench.bin");
    }
    free(buf);
    say("bench: done (8192 bytes per pass, the shape a real save has)\n");
    return;
  }
  /* `wifi drop` — simulate a hotspot blip: disconnect WITHOUT marking the radio user-disabled,
   * so the main loop's retry path engages exactly as it does in the field. Exists because the
   * 5-second freeze {user} feels while scrolling only happens on a reconnect, and a bug that
   * needs someone else's access point to misbehave cannot be measured on demand otherwise. */
  if (!strcasecmp(line, "wifi drop")) {
    WiFi.disconnect();
    say("wifi: dropped (not user-disabled) - the retry path will now run; watch for SLOW WIFI\n");
    return;
  }

  /* `wifi off` / `wifi on` — the Settings row "WiFi: on/off" from the cable (0.9.79), the SAME two
   * calls it makes (GUI.cpp, GUI_ACTION_WIFI_TOGGLE): setRadioOff() persists and applies the
   * switch, and ON hands the radio back through wifiRestoreStation(). Exists for the restore
   * bench, whose every scenario starts from one switch position or the other — and a `key` walk
   * to a menu row is the fragile part of a bench (see `open`). Refused where the menu row cannot
   * be reached either: under a game, and under a live hotspot (switching off there stops the
   * radio beneath a transport that goes on reporting itself up). */
  if (!strcasecmp(line, "wifi off") || !strcasecmp(line, "wifi on")) {
    const bool on = !strcasecmp(line, "wifi on");
    if (const char* by = wifiStationBlockedBy()) {
      say("wifi %s: REFUSED - %s\n", on ? "on" : "off", by);
      return;
    }
    if (on == !wifiState.radioOff()) {
      say("wifi: already %s\n", on ? "on" : "off");
      return;
    }
    wifiState.setRadioOff(!on);
    if (on && !wifiRestoreStation("serial wifi on")) {
      wifiState.setRadioOff(true);    // say what is true, as the Settings row does
      say("wifi on: the radio would not start - left OFF\n");
      return;
    }
    say("wifi: switched %s (persisted, exactly as the Settings row does)\n", on ? "ON" : "OFF");
    return;
  }

  /* `wifi why` — the drop record (Networks::diagPrint). Written straight to the UART rather
   * than through say(), whose 192-byte buffer would clip the longer lines. */
  if (!strcasecmp(line, "wifi") || !strcasecmp(line, "wifi why")) {
    wifiState.diagPrint([](const char* l) {
      uart_write_bytes(PORT, l, strlen(l));
      uart_write_bytes(PORT, "\n", 1);
    });
    return;
  }

  /* `wifi log on|off` — the WiFi DRIVER's own log lines ("state: run -> init (c800)",
   * "bcn_timout,ap_probe_send_start", "recv deauth, reason=..."), which this build filters out
   * twice: the blob's own level (g_log_level, read by wifi_log()) and esp_log's level for the
   * "wifi" tag (CONFIG_LOG_DEFAULT_LEVEL 1 = errors only). Both are RAM-only — the blob setter
   * is a single store (disassembled: esp_wifi_internal_set_log_level) — so a reboot turns it
   * off. ⚠ Bench only: the driver prints from its own task while on. Best effort: if the blob's
   * module mask also filters, nothing appears, and that is the whole cost. */
  if (!strcasecmp(line, "wifi log on") || !strcasecmp(line, "wifi log off")) {
    static bool             s_blobSaved = false;
    static wifi_log_level_t s_blobLevel = WIFI_LOG_WARNING;
    const bool on = !strcasecmp(line, "wifi log on");
    if (on) {
      if (!s_blobSaved) {
        /* ⚠ The blob memcpy()s 20 BYTES into the second argument, whatever the header's
         * `uint32_t *log_mod` suggests (esp_wifi_internal_get_log, disassembled) — a single
         * uint32_t here would be a stack overwrite. */
        uint32_t mods[5] = {0, 0, 0, 0, 0};
        wifi_log_level_t lv;
        if (esp_wifi_internal_get_log(&lv, mods) == ESP_OK) {
          s_blobLevel = lv;
          s_blobSaved = true;
        }
      }
      esp_wifi_internal_set_log_level(WIFI_LOG_INFO);
      esp_log_level_set("wifi", ESP_LOG_INFO);
    } else {
      if (s_blobSaved) {
        esp_wifi_internal_set_log_level(s_blobLevel);
      }
      esp_log_level_set("wifi", ESP_LOG_ERROR);
    }
    say("wifi log: driver state lines %s (blob level was %d)\n", on ? "ON" : "off",
        s_blobSaved ? (int)s_blobLevel : -1);
    return;
  }

  /* `wifi bounce` — radio OFF, then STA back on. THE cure for the deaf-scan state
   * (2026-08-27): after hours of disconnected retry churn the driver can reach a state
   * where every scan COMPLETES with 0 results while the AP is on air at -50 dBm (the twin
   * phone as witness) — screen wake, 240 MHz, quiesced windows, nothing helps except a
   * radio restart. "Open the WiFi screen and rescan brings it back" was this exact cycle
   * hiding in NetworksApp's constructor (disconnect(true,true)); this command is the same
   * medicine WITHOUT eraseap, so the driver's remembered AP survives. The retry loop and
   * auto-switcher take it from here — with the AP in range, expect a rejoin in seconds.
   * autoSwitchTick() now performs this on its own after 2 consecutive empty scans; this
   * command exists so a bench session can apply and PROVE the cure without a reboot. */
  if (!strcasecmp(line, "wifi bounce")) {
    /* (0.9.79) Not over anyone who holds the radio, and not over the owner's "off": bounceRadio()
     * ends in WiFi.mode(WIFI_STA), which under a live hotspot switches AP -> STA (the hotspot
     * dies while the uploader/window go on saying "up"), under a game starts the radio beside
     * the emulator, and with WiFi off or the network Disconnected leaves the station up for the
     * rest of the boot. A deaf radio on such a phone has nothing to be deaf to. */
    if (const char* by = wifiStationBlockedBy()) {
      say("wifi bounce: REFUSED - %s\n", by);
      return;
    }
    if (!wifiState.stationAllowed()) {
      say("wifi bounce: REFUSED - WiFi is %s (menu > WiFi / Settings > WiFi to turn it on)\n",
          wifiState.radioOff() ? "switched OFF" : "disabled for this network");
      return;
    }
    wifiState.bounceRadio();
    say("wifi bounce: radio cycled off/on - retry/auto-switch take it from here\n");
    return;
  }

  /* `wifi scan` — a BLOCKING scan, listing what the radio can actually hear right now.
   *
   * 🔑 EXISTS BECAUSE ONE AFTERNOON WAS SPENT NOT KNOWING THIS. Phone 1 kept losing its
   * association and the auto-switch log showed `scan done: n=0` over and over. There are two
   * completely different faults behind that line — a radio that has gone deaf, or an access
   * point that has stopped advertising — and nothing on the phone could tell them apart. The
   * whole hunt went after the CPU downclock on that ambiguity, and the answer turned out to be
   * the access point (an iPhone hotspot, which stops broadcasting when it is not actively
   * serving anyone). ⚠ On a normal router the same phone is 9 for 9 clean.
   *
   * So: if this prints a list, the radio is fine and the SSID you want is simply not on the
   * air. If it prints 0 while other devices in the room are associated, THEN suspect the
   * phone. ⚠ Blocks for a couple of seconds - it is a bench command, not a loop call. */
  if (!strcasecmp(line, "wifi scan")) {
    /* Guarded like the rest — and this one says so out loud, because a bench command that
     * silently did nothing would be read as "the radio is dead" and send the next hour in
     * the wrong direction entirely. */
    if (!wifiScanMemoryOk("serial")) {
      say("wifi scan: REFUSED - not enough contiguous internal heap to survive the result.\n"
          "           This is the guard, not the radio. Run `heap` and look at `largest`.\n");
      return;
    }
    /* (0.9.79) A scan starts the station (enableSTA): under a hotspot that is AP+STA channel
     * hopping mid-transfer, under a game the radio beside the emulator. */
    if (const char* by = wifiStationBlockedBy()) {
      say("wifi scan: REFUSED - %s\n", by);
      return;
    }
    /* With WiFi off (or the network Disconnected) the scan still runs — "can this radio hear
     * anything" is a fair bench question either way — but the radio goes back down after it.
     * Only when the scan was what brought the station up: a station that was already running
     * is left exactly as it was, so this never starts a join on a phone being diagnosed. */
    const bool staWasUp = (WiFi.getMode() & WIFI_MODE_STA) != 0;
    /* wifiScanStart, not WiFi.scanNetworks: the same scan, but -2 now says WHICH -2 (2026-09-25 —
     * the night before, a -2 could not be told apart from a 10 s no-show). */
    int32_t why = 0;
    const uint32_t t0 = millis();
    const int n = wifiScanStart(false, &why);
    const unsigned long took = (unsigned long)(millis() - t0);
    wifiScanNoteResult(n);
    if (n == WIFI_SCAN_RUNNING) {
      say("wifi scan: another scan is already in flight (the auto-switcher's?) - try again in a few s\n");
    } else if (n == WIFI_SCAN_FAILED) {
      say("wifi scan: -2 after %lu ms - %s: 0x%lx %s\n", took,
          why == (int32_t)ESP_ERR_TIMEOUT ? "STARTED but no SCAN_DONE in 10 s" : "REFUSED at the start",
          (unsigned long)(uint32_t)why, wifiErrName(why));
      if (why == (int32_t)ESP_ERR_WIFI_STATE) {
        say("           = the station is mid-connect (a join, or the core's auto-reconnect after\n"
            "           a NO_AP_FOUND/BEACON_TIMEOUT). `wifi why` shows the disconnect train.\n");
      }
    } else if (n <= 0) {
      say("wifi scan: %d networks - the radio hears NOTHING. If other devices are associated\n"
          "           in this room, that points at the phone; otherwise the AP is not on air.\n", n);
    } else {
      say("wifi scan: %d network(s) - the radio is fine; check whether YOUR ssid is listed\n", n);
      for (int i = 0; i < n && i < 20; i++) {
        say("  %-32s %d dBm ch%d\n", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i));
      }
    }
    WiFi.scanDelete();
    if (!staWasUp) {
      wifiRestoreStation("serial wifi scan");   // the log line says what the radio went back to
    }
    return;
  }

  /* `wifi calreset` — erase the RF calibration data in NVS and reboot, forcing the PHY to
   * recalibrate from scratch on the way up. EXISTS BECAUSE OF 2026-08-27 MORNING: phone 1
   * booted DEAF — `wifi scan` 0/0/-2 (-2 = the scan API itself failing) on a FRESH boot
   * with largest=26 K, while phone 2 on the same desk heard {user}-wifi at -60 dBm and was
   * associated. Fresh heap rules out fragmentation; phone 2 rules out the air; a PCB
   * antenna rules out a knocked cable. What survives a reboot and steers the radio is the
   * stored calibration, so this is the probe — and if it cures a deaf radio, the cure is
   * now one serial command instead of a working theory. ⚠ Only the "phy" NVS namespace is
   * touched; WiFi credentials and everything else stay. Reboots when done. */
  if (!strcasecmp(line, "wifi calreset")) {
    const esp_err_t e = esp_phy_erase_cal_data_in_nvs();
    say("wifi calreset: erase %s - rebooting to recalibrate\n",
        e == ESP_OK ? "OK" : "FAILED (will still reboot; cal may simply be absent)");
    benchReboot("wifi calreset");
    return;
  }

  /* `wifi restore` — esp_wifi_restore(): wipe the WiFi DRIVER's persisted state in NVS
   * (namespace nvs.net80211) and reboot. The step past calreset on the same 2026-08-27
   * deaf radio: with heap, air, firmware and calibration all ruled out, the one thing
   * left that survives reboots is the driver's own stored config. ⚠ Only the driver
   * namespace is touched — the app's Preferences (mesh keypair, book positions) and the
   * phone's own saved-network profiles are elsewhere and re-write driver config on the
   * next connect. */
  if (!strcasecmp(line, "wifi restore")) {
    const esp_err_t e = esp_wifi_restore();
    say("wifi restore: %s - rebooting\n", e == ESP_OK ? "OK" : "FAILED");
    benchReboot("wifi restore");
    return;
  }
  if (!strcasecmp(line, "chans")) {
    for (int i = 0; i < meshService.getChannelCount(); i++) {
      const MeshChannel* c = meshService.getChannel(i);
      if (c) {
        say("  [%d] '%s' keyLen=%d\n", i, c->name, (int)c->keyLen);
      }
    }
    return;
  }

  /* `meshdb` — where the chat history lives, what loaded, and what the next save will write.
   *
   * 🔑 EXISTS BECAUSE THE FAULT IT DIAGNOSES WAS INVISIBLE FROM EVERY ANGLE. The phone read
   * SPIFFS at boot and wrote the SD card from the first loop pass onward, so the chat list
   * came up empty while a perfectly good database sat on the card — and nothing on screen,
   * on the console or in the log said which file anything had touched. Two file sizes side by
   * side is the whole diagnosis, and neither number was obtainable before this.
   *
   * ⚠ The sizes are read live from BOTH filesystems every time, never cached: which one is
   * in use is the exact thing in question. */
  /* `meshdb cut` — prove the boot recovery of a save cut between its remove() and its rename()
   * (mesh_dbfile.h) on the phone's own filesystem, without pulling a battery at the right
   * millisecond. The real file is renamed ASIDE, never deleted: if the recovery fails, the
   * database is /meshdb.cut on the card, one rename (on a computer) from being back. So a
   * leftover /meshdb.cut REFUSES the next run rather than being cleared (benchCutSave()). */
  if (!strcasecmp(line, "meshdb cut")) {
    extern volatile bool gGbcActive;
    extern GUI gui;
    if (gGbcActive || tileFetchActive()) {
      say("meshdb cut: refused - a game or a map download owns the card right now\n");
      return;
    }
    /* 🛑 SAFE TO TYPE ON A DAILY PHONE (review M3). The reboot below would end a transfer, a
     * KOSync window (a reading position on its way in or out) or a call mid-way, and lose every
     * frame still waiting for the radio — texts whose messages the save just wrote to the card
     * as outgoing with no receipt, so after the reboot they would read "sent" forever. Wait for
     * all of that, then type it again. */
    if (xferServing()) {
      say("meshdb cut: refused - the uploader or a KOSync window is up (`up off` / let it close)\n");
      return;
    }
    if (gui.inCall()) {
      say("meshdb cut: refused - a call is up\n");
      return;
    }
    if (!meshService.txPipelineIdle()) {
      say("meshdb cut: refused - %d frame(s) waiting for the radio%s; try again in a few seconds\n",
          meshService.txQueued(), meshPhy.txBusy() ? " and one on the air" : "");
      return;
    }
    const char* why = meshService.benchCutSave();
    if (why) {
      say("meshdb cut: NOT done - %s\n", why);
      return;
    }
    say("meshdb cut: /meshdb.tmp written whole, /meshdb.bin set aside as /meshdb.cut, NO rename.\n");
    say("  REBOOTING NOW (so no save can heal it first). The boot log must say 'MESH DB: RECOVERED',\n");
    say("  `meshdb` must show the same counts, and `rm /meshdb.cut` tidies up. No RECOVERED line:\n");
    say("  the database is /meshdb.cut on the card - STOP and rename it back on a computer.\n");
    /* The power-off saves run AFTER the cut, not before: a refused cut then costs nothing, and
     * none of them touches /meshdb.* (benchReboot()). */
    benchReboot("meshdb cut");
    return;
  }
  if (!strcasecmp(line, "meshdb")) {
    const bool card = meshService.dbOnCard();
    say("meshdb: in use %s   loaded from %s at boot\n",
        card ? "SD" : "SPIFFS", meshService.dbLoadedFrom());
    say("  in RAM: %d nodes, %d msgs, %d places\n",
        meshService.getNodeCount(), meshService.getMessageCount(),
        meshService.getWaypointCount());
    const int keep = meshService.persistedMessageCount();
    say("  next save writes %d of %d msgs (caps: %d per chat, %d total on %s)\n",
        keep, meshService.getMessageCount(),
        card ? MESH_PERSIST_PER_CHAT_SD : MESH_PERSIST_PER_CHAT_FLASH,
        card ? MESH_PERSIST_TOTAL_SD : MESH_PERSIST_TOTAL_FLASH,
        card ? "SD" : "SPIFFS");
    for (int i = 0; i < 2; i++) {
      fs::FS& f = i ? (fs::FS&)SPIFFS : (fs::FS&)SD;
      const char* nm = i ? "SPIFFS" : "SD    ";
      File h = f.open("/meshdb.bin", "r");
      if (h) {
        say("  %s /meshdb.bin  %u bytes%s\n", nm, (unsigned)h.size(),
            (!i && card) || (i && !card) ? "   <- the one in use" : "");
        h.close();
      } else {
        say("  %s /meshdb.bin  absent\n", nm);
      }
    }
    return;
  }

  /* `lock` — why the screen does or does not lock, in one paste.
   *
   * 🔑 THE LOCK IS NOT ITS OWN TIMER. It happens INSIDE the SCREEN_SLEEP_EVENT branch
   * (GUI.cpp, processEvent), so it needs BOTH `locking` and a screen that actually sleeps —
   * `doSleeping()` is `sleeping && 0 < sleepAfterMs <= 86400000`. A phone that dims but never
   * sleeps therefore never locks either, and the dimming makes it look like the timeout is
   * working. That is two different reasons for one symptom, so this prints both, plus what is
   * on the CARD as against what is in RAM — a setting that saved and a setting that loaded are
   * not the same claim.
   *
   * ⚠ `lock_keyboard` DEFAULTS TO 0 WHEN THE [lock] SECTION EXISTS but the key does not, while
   * a missing SECTION defaults to 1 (WiPhone.ino). The same disagreement in [screen] shipped a
   * phone that never dimmed and never slept — see the 2026-08-22 battery audit. */
  if (!strcasecmp(line, "lock")) {
    extern GUI gui;
    ControlState& st = gui.state;
    say("lock: locking=%s  locked=%s  (locking is the SETTING, locked is right now)\n",
        st.locking ? "ON" : "OFF", st.locked ? "yes" : "no");
    say("  sleep: sleeping=%s after=%lus -> doSleeping()=%s\n",
        st.sleeping ? "ON" : "OFF", (unsigned long)(st.sleepAfterMs / 1000),
        st.doSleeping() ? "YES" : "NO  <-- nothing can lock without this");
    say("  dim:   dimming=%s after=%lus level=%d  (dimming alone never locks)\n",
        st.dimming ? "ON" : "OFF", (unsigned long)(st.dimAfterMs / 1000), (int)st.dimLevel);
    /* ⚠ THE SIP STATE IS PART OF THE LOCK, and that is not obvious from anywhere else.
     * While inCall() is true the unlock path clears `locked` on ANY key — so a phone parked
     * in a teardown state has no working lock. See GUI::inCall(). */
    say("  sip:   state=%d  inCall()=%s%s\n", (int)st.sipState,
        gui.inCall() ? "YES" : "no",
        gui.inCall() ? "  <-- ANY key unlocks while this is YES" : "");

    CriticalFile ini(Storage::ConfigsFile);
    if (!ini.load() && !ini.restore()) {
      say("  card:  %s WILL NOT LOAD - the phone is running on built-in defaults\n",
          Storage::ConfigsFile);
      return;
    }
    if (!ini.hasSection("lock")) {
      say("  card:  [lock] section ABSENT -> boot defaults locking ON\n");
    } else {
      const int32_t v = ini["lock"].getIntValueSafe("lock_keyboard", -1);
      say("  card:  [lock] lock_keyboard = %s%s\n",
          v < 0 ? "(key MISSING)" : (v ? "1" : "0"),
          v < 0 ? "  <-- section present but key absent: boot reads this as OFF" : "");
    }
    if (!ini.hasSection("screen")) {
      say("  card:  [screen] section ABSENT -> boot defaults dim+sleep ON\n");
    } else {
      say("  card:  [screen] sleeping=%d sleep_after_s=%d dimming=%d dim_after_s=%d\n",
          (int)ini["screen"].getIntValueSafe("sleeping", -1),
          (int)ini["screen"].getIntValueSafe("sleep_after_s", -1),
          (int)ini["screen"].getIntValueSafe("dimming", -1),
          (int)ini["screen"].getIntValueSafe("dim_after_s", -1));
    }
    return;
  }

  /* `ver` — which binary is actually on this phone. Two builds wore 0.9.14 during the
   * 2026-08-25 session before this existed; asking the phone is cheaper than remembering. */
  if (!strcasecmp(line, "ver") || !strcasecmp(line, "version")) {
    say("firmware %s, built %s %s\n", FIRMWARE_VERSION, __DATE__, __TIME__);
    return;
  }

  /* `scrim` — the translucent plate under menu text, tunable against a real photo.
   * ⚠ RAM only. The shipped value is THEME_SCRIM_* in GUI.h; a reboot restores it. */
  if (!strncasecmp(line, "scrim", 5) && (line[5] == '\0' || line[5] == ' ')) {
    extern GUI gui;
    const char* a = line + 5;
    while (*a == ' ') {
      a++;
    }
    if (*a) {
      char* end = NULL;
      const unsigned long alpha = strtoul(a, &end, 10);
      if (end == a || alpha > 255) {
        say("scrim: alpha must be 0-255 (0 = off). `scrim 190`, `scrim 190 39C7`\n");
        return;
      }
      gScrimAlpha = (uint8_t)alpha;
      while (*end == ' ') {
        end++;
      }
      if (*end) {
        gScrimColor = (uint16_t)strtoul(end, NULL, 16);
      }
      gui.redrawScreen(true, true, true, true);
    }
    say("scrim: alpha=%u color=0x%04X (shipped default %u / 0x%04X; RAM only, a reboot resets it)\n",
        (unsigned)gScrimAlpha, (unsigned)gScrimColor,
        (unsigned)THEME_SCRIM_ALPHA, (unsigned)THEME_SCRIM_COLOR);
    return;
  }

  /* `key` — press keys on a phone nobody is holding.
   *
   * 🔑 THE COMPANION TO `shot`, AND THE OTHER HALF OF THE SAME GAP. A cable could already SEE
   * the screen; it could not change what was on it, so every claim about a screen you have to
   * navigate to was still unverifiable. Photos shipped to a user untested for exactly that
   * reason, and the menu-contrast complaint that followed lives three key presses from the
   * clock face.
   *
   * ⚠ It injects into keypadBuff — the real keypad buffer — so the wake, the drain loop, the
   * easter-egg tracker and the app's own processEvent all run as they do for a thumb. It is
   * not a simulation of a press; it IS one, from a different source. */
  if (!strncasecmp(line, "key", 3) && (line[3] == '\0' || line[3] == ' ')) {
    static const struct {
      const char* name;
      char code;
    } NAMES[] = {
      { "select", WIPHONE_KEY_SELECT }, { "back",  WIPHONE_KEY_BACK  },
      { "ok",     WIPHONE_KEY_OK     }, { "up",    WIPHONE_KEY_UP    },
      { "down",   WIPHONE_KEY_DOWN   }, { "left",  WIPHONE_KEY_LEFT  },
      { "right",  WIPHONE_KEY_RIGHT  }, { "call",  WIPHONE_KEY_CALL  },
      { "end",    WIPHONE_KEY_END    }, { "f1",    WIPHONE_KEY_F1    },
      { "f2",     WIPHONE_KEY_F2     }, { "f3",    WIPHONE_KEY_F3    },
      { "f4",     WIPHONE_KEY_F4     },
      /* The two softkeys under the screen, by what they DO rather than by their wiring —
       * "menu" is the word printed on the screen above the left one, and looking up which
       * hardware key that is should not be the reader's problem. */
      { "menu",   WIPHONE_KEY_SELECT }, { "sel",   WIPHONE_KEY_SELECT },
    };
    const char* p = line + 3;
    while (*p == ' ') {
      p++;
    }
    /* `key hold <key> [ms [blip]]` — press the key and keep it "down" for <ms> (default
     * 1000) with no finger on it, the way `maps hold` does for the arrows: the loop's hold
     * trackers (F2 previous-track, a held digit, a held '#' for a capital or the mute) read
     * uiKeyDownOrBlip(), which answers from the bench while this runs. <blip> ms in, fake the
     * chip's release-and-re-press under a held finger (see uiKeyUpAgeMs). */
    if (!strncasecmp(p, "hold", 4) && (p[4] == '\0' || p[4] == ' ')) {
      p += 4;
      while (*p == ' ') {
        p++;
      }
      char tok[16];
      size_t n = 0;
      while (*p && *p != ' ' && n < sizeof(tok) - 1) {
        tok[n++] = *p++;
      }
      tok[n] = '\0';
      char code = 0;
      for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (!strcasecmp(tok, NAMES[i].name)) {
          code = NAMES[i].code;
          break;
        }
      }
      if (!code && n == 1) {
        code = tok[0];
      }
      const uint32_t mask = code ? uiKeyMaskFor(code) : 0;
      if (!mask) {
        say("key hold: usage key hold <key> [ms [blip]]  (a key name as for `key`)\n");
        return;
      }
      while (*p == ' ') {
        p++;
      }
      char* end = (char*)p;
      const long ms = *p ? strtol(p, &end, 10) : 1000;
      if (*p && (end == p || (*end && *end != ' '))) {
        say("key hold: <ms> must be a number\n");
        return;
      }
      if (ms < 100 || ms > 10000) {
        say("key hold: <ms> must be 100..10000\n");
        return;
      }
      while (*end == ' ') {
        end++;
      }
      long blip = 0;
      if (*end) {
        char* end2 = end;
        blip = strtol(end, &end2, 10);
        while (*end2 == ' ') {
          end2++;
        }
        if (end2 == end || *end2 || blip < 0 || blip >= ms) {
          say("key hold: <blip> must be a number of ms below <ms>\n");
          return;
        }
      }
      /* Inject first, arm second — same order and same reason as `maps hold`. */
      if (!uiInjectKey(code)) {
        say("key hold: key buffer full\n");
        return;
      }
      uiKeyBenchHold(mask, (uint32_t)ms, (uint32_t)blip);
      if (blip) {
        say("key hold: %s pressed, held for %ld ms, a release blip at %ld ms\n", tok, ms, blip);
      } else {
        say("key hold: %s pressed, held for %ld ms\n", tok, ms);
      }
      return;
    }
    int sent = 0, refused = 0;
    char what[96] = "";
    while (*p) {
      while (*p == ' ') {
        p++;
      }
      if (!*p) {
        break;
      }
      char tok[16];
      size_t n = 0;
      while (*p && *p != ' ' && n < sizeof(tok) - 1) {
        tok[n++] = *p++;
      }
      tok[n] = '\0';
      char code = 0;
      for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (!strcasecmp(tok, NAMES[i].name)) {
          code = NAMES[i].code;
          break;
        }
      }
      /* A single character is itself — digits, * and # are what the keypad actually sends. */
      if (!code && n == 1) {
        code = tok[0];
      }
      if (!code) {
        say("key: don't know '%s' (names: select/menu back ok up down left right call end f1-f4, or one character)\n", tok);
        return;
      }
      if (uiInjectKey(code)) {
        sent++;
        snprintf(what + strlen(what), sizeof(what) - strlen(what), "%s%s", sent > 1 ? " " : "", tok);
      } else {
        refused++;
      }
    }
    if (!sent && !refused) {
      say("key: nothing to press. `key menu`, `key down down ok`, `key 5`\n");
      return;
    }
    /* ⚠ The presses are QUEUED, not applied — loop() drains keypadBuff on its next pass, and
     * a screenshot taken in the same breath would catch the OLD screen. Anything reading this
     * must let a pass happen before it looks. tools/shot.py --cmd does. */
    say("key: queued %d (%s)%s - drains on the next loop pass\n",
        sent, what, refused ? ", SOME DROPPED (buffer full)" : "");
    return;
  }

  /* `shot` — the live frame, base64, for tools/shot.py. See GUI::screenshotToSerial(). */
  if (!strcasecmp(line, "shot") || !strcasecmp(line, "screenshot")) {
    extern GUI gui;
    gui.screenshotToSerial();
    return;
  }

  /* `ls [dir]` — EVERY entry in a card folder, hidden ones included, with sizes. It exists
   * because the on-phone listers disagreed about dotfiles: the Files app hides any name that
   * starts with '.', while the Game Boy / Books / Music pickers listed everything with the
   * right extension — so a macOS AppleDouble sidecar ('._Game.gbc', written by Finder next to
   * every file it copies onto a FAT card) showed up as a duplicate ROM that the Files app
   * could not find ({user}, phone 2, 2026-09-03). The picker draws straight to the LCD, so
   * `shot` cannot show it either. This is the one view of the card as it actually is: hidden
   * entries are marked '*', folders end in '/'. */
  if (!strncasecmp(line, "ls", 2) && (line[2] == '\0' || line[2] == ' ')) {
    const char* arg = line + 2;
    while (*arg == ' ') {
      arg++;
    }
    const char* path = *arg ? arg : "/";
    File dir = SD.open(path);
    if (!dir) {
      say("ls: %s: cannot open (no card, or no such folder)\n", path);
      return;
    }
    if (!dir.isDirectory()) {
      say("ls: %s is a file, %u bytes\n", path, (unsigned)dir.size());
      dir.close();
      return;
    }
    File f;
    int n = 0, hidden = 0;
    while ((f = dir.openNextFile())) {
      /* ⚠ On this core File::name() returns the FULL path — basename it (same as app_files). */
      const char* nm = f.name();
      const char* base = strrchr(nm, '/');
      base = base ? base + 1 : nm;
      const bool hid = (base[0] == '.');
      if (hid) {
        hidden++;
      }
      if (f.isDirectory()) {
        say("  %s %s/\n", hid ? "*" : " ", base);
      } else {
        say("  %s %-40s %u\n", hid ? "*" : " ", base, (unsigned)f.size());
      }
      n++;
      f.close();
    }
    dir.close();
    say("ls: %d entries in %s, %d hidden (*)\n", n, path, hidden);
    return;
  }

  /* `rm </path/file>` — delete ONE file by full path; the pair to `ls`, for the entries the
   * Files app cannot show. Refuses folders and relative names so a typo cannot take out a
   * directory. Same SD.remove() the Files app's Delete and the picker's Back-del run. */
  if (!strncasecmp(line, "rm ", 3)) {
    const char* path = line + 3;
    while (*path == ' ') {
      path++;
    }
    if (path[0] != '/') {
      say("rm: give a full path starting with / (see `ls`)\n");
      return;
    }
    File f = SD.open(path);
    if (!f) {
      say("rm: %s: not found\n", path);
      return;
    }
    const bool isDir = f.isDirectory();
    f.close();
    if (isDir) {
      say("rm: %s is a folder - refused\n", path);
      return;
    }
    say("rm: %s: %s\n", path, SD.remove(path) ? "deleted" : "FAILED");
    return;
  }

  /* `mv </path/from> </path/to>` — rename ONE file in place: a FAT rename, so the bytes never
   * move and nothing is deleted. Quote a path that has spaces ("..."). Refuses folders, relative
   * names and an existing destination (never overwrites). An app that holds card files open
   * (Books, Music, Files, Photos, the Game Boy) must be closed first: FatFs must not rename an
   * open file. Added 2026-10-09 to bring a book's file name in line with COVEY's and the X4's
   * copy - KOSync's second document id is the md5 of the file name, so two readers only match
   * on it when the names agree. Reading places survive: the position store keys on epub ids.
   * KOSync's own per-book memory (last move, answered offer, a move home never had) is keyed by
   * the file name, so it is carried to the new name (kosyncBookRenamed). Also refused while a
   * track is loaded in the music player (it outlives its screen and reopens by path) and while
   * the uploader is up (a chunked upload holds its file open between passes). */
  if (!strncasecmp(line, "mv ", 3)) {
    char from[160], to[160];
    const char* p = line + 3;
    char* outs[2] = {from, to};
    for (int k = 0; k < 2; k++) {
      while (*p == ' ') {
        p++;
      }
      const bool quoted = *p == '"';
      if (quoted) {
        p++;
      }
      size_t n = 0;
      while (*p && (quoted ? *p != '"' : *p != ' ') && n + 1 < sizeof(from)) {
        outs[k][n++] = *p++;
      }
      outs[k][n] = '\0';
      if (quoted) {
        if (*p != '"') {
          say("mv: a quoted path has no closing \" (or is too long)\n");
          return;
        }
        p++;
      } else if (*p && *p != ' ') {
        say("mv: a path is too long (%u characters at most)\n", (unsigned)(sizeof(from) - 1));
        return;
      }
    }
    while (*p == ' ') {
      p++;
    }
    if (from[0] != '/' || to[0] != '/' || *p) {
      say("usage: mv </path/from> </path/to>   (full paths; quote one with spaces: \"/books/a b.epub\")\n");
      return;
    }
    extern GUI gui;
    if (gui.isAppRunning(GUI_APP_BOOKS) || gui.isAppRunning(GUI_APP_MUSIC) || gui.isAppRunning(GUI_APP_FILES) ||
        gui.isAppRunning(GUI_APP_PHOTOS) || gui.isAppRunning(GUI_APP_GBC)) {
      say("mv: refused - an app with card files open is on screen (`open clock` first)\n");
      return;
    }
    if (musicPlayerIsPlaying() || musicPlayerIsPaused()) {
      say("mv: refused - the music player holds a track (stop it first)\n");
      return;
    }
    if (gbcXferOn()) {
      say("mv: refused - the uploader is running (`up off` first)\n");
      return;
    }
    File f = SD.open(from);
    if (!f) {
      say("mv: %s: not found\n", from);
      return;
    }
    const bool isDir = f.isDirectory();
    f.close();
    if (isDir) {
      say("mv: %s is a folder - refused\n", from);
      return;
    }
    if (SD.exists(to)) {
      say("mv: %s already exists - refused (never overwrites)\n", to);
      return;
    }
    const bool ok = SD.rename(from, to);
    say("mv: %s\n", ok ? "renamed" : "FAILED");   // the verdict first: say() cuts long lines
    say("  %s\n", from);
    say("  -> %s\n", to);
    if (ok) {
      const char* a = strrchr(from, '/') + 1;
      const char* b = strrchr(to, '/') + 1;
      if (strcmp(a, b)) {
        say("mv: KOSync memory for this file %s\n",
            kosyncBookRenamed(a, b) ? "moved to the new name" : "- none kept (nothing to move)");
      }
    }
    return;
  }

  /* `cpu` — THE CPU CLOCK AND THE PLL UNDER IT (0.9.79 dev, 2026-09-25).
   *
   * Phone 1's WiFi went deaf 4 times in 4 when the gate dropped 240->80 straight after the
   * uploader's traffic, because on this chip that move re-locks the BBPLL the radio runs from
   * (cpu_clock_policy.h). This is the instrument for the fix: what the clock is, which PLL it
   * runs from, whether the radio is up, and - the number that matters - how many PLL re-locks
   * happened WITH THE RADIO ON (method new must keep it at 0). `cpu method old|new` is the A/B;
   * `cpu cycle` drives the gate's own switch on demand so the A/B needs no screen timeouts. */
  if (!strncasecmp(line, "cpu", 3) && (line[3] == '\0' || line[3] == ' ')) {
    extern volatile bool gGbcActive;
    const char* arg = line + 3;
    while (*arg == ' ') {
      arg++;
    }
    CpuClockStatus cs;
    if (!strncasecmp(arg, "method", 6)) {
      const char* m = arg + 6;
      while (*m == ' ') {
        m++;
      }
      if (!strcasecmp(m, "old")) {
#ifdef USER_SERIAL
        /* 🛑 NOT WITH THE GPS READER ON (review, 2026-09-25). Under method new with WiFi up,
         * phone 2 runs at 160 on PLL 320; the gate's next pass under OLD (busy: gGpsNmea) would
         * call setCpuFrequencyMhz(240), which fires uart_on_apb_change - the gGpsNmea deadlock
         * that parks the loop for good (WiPhone.ino) - AND re-locks the PLL under the radio.
         * On 0.9.78 phone 2 never called setCpuFrequencyMhz after boot; this switch must not
         * hand it a new way to hang. Same refusal as `cpu cycle`'s, and `gps on` refuses the
         * other order. */
        if (gGpsNmea) {
          say("cpu method old: refused - with the GPS reader on, method OLD's next switch would take\n");
          say("  the APB-callback deadlock (WiPhone.ino, the gGpsNmea note) - `gps off` first\n");
          return;
        }
#endif
        cpuClockSetMethod(CPU_METHOD_OLD);
      } else if (!strcasecmp(m, "new")) {
        cpuClockSetMethod(CPU_METHOD_NEW);
      } else if (*m) {
        say("cpu method old|new\n");
        return;
      }
      cpuClockStatus(&cs);
      say("cpu method: %s (RAM only; every boot starts new)\n",
          cs.method == CPU_METHOD_NEW ? "new - the PLL is never re-locked while the radio runs"
                                      : "OLD - setCpuFrequencyMhz 240/80, a PLL re-lock on every change");
      if (cs.method == CPU_METHOD_NEW && cs.radioOn && cs.pllMhz == 480) {
        say("  the PLL is at 480 with the radio on, so the clock stays at 240 until the radio next\n");
        say("  stops (`wifi bounce` does it): only then can it move to PLL 320 without a re-lock under it\n");
      }
      return;
    }
    if (!strncasecmp(arg, "cycle", 5)) {
      char* end = NULL;
      const long n = strtol(arg + 5, &end, 10);
      const bool gotN = end != arg + 5;
      const long ms = (gotN && *end) ? strtol(end, &end, 10) : 0;
      if (gotN && n == 0) {
        cpuClockCycleStart(0, 0);
        say("cpu cycle: stopped - the gate has the clock back\n");
        return;
      }
      if (n < 1 || n > 500 || ms < 50 || ms > 60000) {
        say("cpu cycle <1..500 pairs> <50..60000 ms per switch>   (`cpu cycle 0` stops)\n");
        return;
      }
      if (gGbcActive) {
        say("cpu cycle: a Game Boy game owns the clock - refused\n");
        return;
      }
      cpuClockStatus(&cs);
#ifdef USER_SERIAL
      if (cs.method == CPU_METHOD_OLD && gGpsNmea) {
        say("cpu cycle: method OLD with the GPS reader on would take the APB-callback deadlock\n");
        say("  (WiPhone.ino, the gGpsNmea note) - `gps off` first, or use method new\n");
        return;
      }
#endif
      const uint32_t lo = cpuGateTarget(cs.method, false, cs.believedMhz, cs.radioOn, cs.rated160);
      const uint32_t hi = cpuGateTarget(cs.method, true, lo, cs.radioOn, cs.rated160);
      const bool relock = cpuSwitchRelocksPll(lo, hi);
      cpuClockCycleStart((uint32_t)n, (uint32_t)ms);
      say("cpu cycle: %ld down/up pairs, one switch every %ld ms, method %s, radio %s\n", n, ms,
          cs.method == CPU_METHOD_NEW ? "new" : "OLD", cs.radioOn ? "ON" : "off");
      say("  expect %lu <-> %lu MHz: %s\n", (unsigned long)hi, (unsigned long)lo,
          lo == hi ? "NO switch (held - see `cpu`)"
                   : !relock ? "same PLL, a divider write"
                             : cs.radioOn ? "a PLL RE-LOCK WITH THE RADIO ON each time (the 4/4 break)"
                                          : "a PLL re-lock each time, radio off (allowed)");
      say("  the gate gets the clock back after the last (up) switch; `CPU cycle done` gives the counts\n");
      return;
    }
    if (*arg) {
      say("cpu: unknown - `cpu`, `cpu method old|new`, `cpu cycle <n> <ms>`\n");
      return;
    }
    cpuClockStatus(&cs);
    say("cpu: %lu MHz on PLL %lu, radio %s, method %s%s\n", (unsigned long)cs.mhz,
        (unsigned long)cs.pllMhz, cs.radioOn ? "ON" : "off",
        cs.method == CPU_METHOD_NEW ? "new (same PLL while the radio runs)" : "OLD (setCpuFrequencyMhz)",
        cs.rated160 ? ", chip RATED 160" : "");
    say("  switches this boot: %lu same-PLL (divider only), %lu PLL re-locks radio off, %lu WITH THE RADIO ON\n",
        (unsigned long)cs.samePll, (unsigned long)cs.relockOff, (unsigned long)cs.relockOn);
    say("  240->160 before the radio started: %lu   idle refused (PLL 480 + radio on): %lu\n",
        (unsigned long)cs.preRadio, (unsigned long)cs.held);
    if (cs.anySwitch) {
      say("  last gate switch: %lu -> %lu MHz (%s) %s, radio %s, %lu s ago\n",
          (unsigned long)cs.lastFrom, (unsigned long)cs.lastTo, cs.lastWhy,
          cs.lastRelock ? "PLL RE-LOCK" : "same PLL", cs.lastRadioOn ? "ON" : "off",
          (unsigned long)(cs.lastAgoMs / 1000));
    } else {
      say("  last gate switch: none this boot\n");
    }
    if (cs.believedMhz != cs.mhz) {
      say("  WARNING: the clock module set %lu MHz but the hardware reads %lu - something switched\n",
          (unsigned long)cs.believedMhz, (unsigned long)cs.mhz);
      say("  behind it (only cpu_clock.cpp may change the CPU frequency)\n");
    }
    if (cs.cycleLeft) {
      say("  cycle: %lu switches left, one per %lu ms\n", (unsigned long)cs.cycleLeft,
          (unsigned long)cs.cyclePeriodMs);
    }
    return;
  }

  /* `power` — THE USB-POWER-METER INSTRUMENT (2026-09-03).
   *
   * Phone 1's full-to-empty run (9h07m on battery, WiFi off, screen off, 80 MHz) implies
   * ~80-100 mA of average draw, and a datasheet-typical sum of everything the firmware leaves
   * powered (ESP32 at 80 MHz never light-sleeping ~20-31, SX1276 RX-continuous ~11, ST7789
   * awake behind a dark backlight 1-8?, I2S+APLL running from boot 1-3?) reaches only
   * ~35-58 mA. The cell is soldered, so battery current cannot be metered, and a drain run
   * resolves 10 mA only after hours at matched voltage (10 mV log quantisation). {user}'s USB
   * power meter resolves 1 mA in seconds — IF the phone is FULL (charger tapered: chg=1 and
   * v>=4.19 for 30+ min, so the meter reads the board and not the charge). Absolute readings
   * include the charger IC and the CP2104; only DELTAS mean anything. Each subcommand flips
   * exactly ONE thing so one reading answers one question:
   *   power lcd sleep     the panel controller's own draw (backlight is already off)
   *   power i2s stop      the I2S module + DMA (the APLL stays; see Audio.cpp:111-121)
   *   power lora sleep    the radio's RX-continuous draw (~11 mA typical: also calibrates the method)
   *   power sleep 20      everything EXCEPT the ESP32 core — the light-sleep floor, i.e. the
   *                       most the CPU lever could ever give (WiFi must be off: light sleep
   *                       with the station up drops the association)
   * Bench only — none of this is a power policy; the numbers it produces decide what becomes one. */
  if (!strncasecmp(line, "power", 5) && (line[5] == '\0' || line[5] == ' ')) {
    extern GUI gui;
    extern Audio* audio;
    extern volatile bool gGbcActive;
    static bool s_lcdAsleep = false;
    static bool s_i2sStopped = false;
    const char* arg = line + 5;
    while (*arg == ' ') {
      arg++;
    }
    const bool audioOn = audio && audio->isOn();
    if (!*arg) {
      say("power: cpu=%luMHz screen=%d wifi=%s lora=%s lcd=%s i2s=%s audio=%s gbc=%d\n",
          (unsigned long)getCpuFrequencyMhz(), (int)gui.state.screenBrightness,
          wifiState.radioOff() ? "OFF" : "on",
          !meshPhy.isReady() ? "absent"
            : (meshPhy.benchAsleep() ? "SLEEP(bench)" : (meshPhy.txBusy() ? "tx" : "rx")),
          s_lcdAsleep ? "SLPIN(bench)" : "normal", s_i2sStopped ? "stopped(bench)" : "running",
          audioOn ? "ON" : "off", (int)gGbcActive);
      say("  method: cell FULL (chg=1, v>=4.19 for 30+ min), screen asleep, WiFi off. Read the\n");
      say("  meter; flip ONE switch; read again. Only deltas mean anything (charger+CP2104 ride along).\n");
      return;
    }
    if (!strcasecmp(arg, "lcd sleep") || !strcasecmp(arg, "lcd wake")) {
      const bool sleep = !strcasecmp(arg, "lcd sleep");
      if (!static_lcd || gGbcActive) {
        say("power lcd: the emulator owns the panel (or no LCD) - refused\n");
        return;
      }
      if (sleep) {
        static_lcd->writecommand(0x28);          // DISPOFF
        static_lcd->writecommand(0x10);          // SLPIN: oscillator, charge pumps, scan stop
      } else {
        static_lcd->writecommand(0x11);          // SLPOUT: the panel needs >=120 ms to settle
        delay(120);
        static_lcd->writecommand(0x29);          // DISPON
      }
      s_lcdAsleep = sleep;
      say("power lcd: %s (frame memory kept; the GUI repaints on the next screen wake)\n",
          sleep ? "DISPOFF+SLPIN" : "SLPOUT+DISPON");
      return;
    }
    if (!strcasecmp(arg, "i2s stop") || !strcasecmp(arg, "i2s start")) {
      const bool stop = !strcasecmp(arg, "i2s stop");
      if (stop && audioOn) {
        say("power i2s: audio is ON - stop it first\n");
        return;
      }
      if (!audio || !audio->i2sReady()) {       // IDF 3.3's i2s_stop/start dereference NULL
        say("power i2s: no I2S driver installed (a failed install) - the next Audio::start() installs one\n");
        return;
      }
      const esp_err_t r = stop ? i2s_stop(I2S_NUM_0) : i2s_start(I2S_NUM_0);
      if (r == ESP_OK) {
        s_i2sStopped = stop;
      }
      say("power i2s: %s %s (Audio::start() calls i2s_start itself, so audio still works after a stop)\n",
          stop ? "stop" : "start", r == ESP_OK ? "ok" : "FAILED");
      return;
    }
    if (!strcasecmp(arg, "lora sleep") || !strcasecmp(arg, "lora rx")) {
      if (!meshPhy.isReady()) {
        say("power lora: no radio (not detected at boot)\n");
        return;
      }
      const bool sleep = !strcasecmp(arg, "lora sleep");
      /* 🛑 NOT OVER A FRAME (review M3). Sleep cuts the frame on the air — somebody's text,
       * truncated on the air — and the next queued frame would wake the chip straight back up
       * (a send ends the bench state), so the meter would read RX while this said SLEEP. */
      if (sleep && !meshService.txPipelineIdle()) {
        say("power lora sleep: refused - %d frame(s) waiting%s; try again in a few seconds\n",
            meshService.txQueued(), meshPhy.txBusy() ? " and one on the air" : "");
        return;
      }
      const bool cut = meshPhy.benchSleep(sleep);
      say("power lora: %s%s%s\n", sleep ? "SLEEP - the mesh is DEAF until `power lora rx`" : "RX-continuous",
          sleep ? " (a send or a radio re-init also ends it)" : "",
          cut ? " - a frame on the air was CUT (its receipt says so; see `radio`)" : "");
      return;
    }
    if (!strncasecmp(arg, "sleep", 5)) {
      const int secs = atoi(arg + 5);
      if (secs < 1 || secs > 600) {
        say("power sleep <1..600 seconds>\n");
        return;
      }
      /* "WiFi: off" does not mean the radio is off: a KOSync window (or the uploader) can be
       * hosting a hotspot with the switch off — that is the woods case it exists for. Asked
       * first, so the radio check below is left with the cases nothing else explains. */
      if (xferServing()) {
        say("power sleep: a server/hotspot is up (uploader or KOSync window) - refused\n");
        return;
      }
      /* 🛑 THE RADIO, NOT THE SWITCH (0.9.79 review R1). "WiFi: off" is what the owner asked for;
       * cpuClockRadioOn() is what the driver is doing (the esp_wifi_start/stop wrappers). They can
       * disagree: a NO_AP_FOUND handled after disable() used to restart the station behind "off"
       * (the core's auto-reconnect, now disarmed there), and a phone with no saved network is
       * off with the switch ON. Light sleep over a running radio is the PLL re-lock under the
       * radio (IDF 3.3 turns the PLL off) - the class that dropped phone 1's WiFi 4/4. */
      if (cpuClockRadioOn()) {
        if (wifiState.radioOff()) {
          say("power sleep: WiFi reads OFF but the RADIO IS RUNNING - refused (`wifi why`: mode=, and\n");
          say("  who restarted it); switch WiFi on and off again to stop it\n");
        } else {
          say("power sleep: WiFi is ON - switch it off first (menu > WiFi: off); light sleep with the\n");
          say("  station up drops the association and the reading would include the reconnect\n");
        }
        return;
      }
      if (audioOn || gGbcActive) {
        say("power sleep: audio or the emulator is running - refused\n");
        return;
      }
      /* 🛑 NOT WITH A FRAME ON THE AIR OR WAITING (review M3). Light sleep stops the loop, not
       * the SX1276: a frame on the air finishes on its own and the chip then sits in STANDBY
       * (~1.6 mA, not RX's ~11) until the loop comes back — so the floor reading is ~9 mA low,
       * and any ACK owed waits out the sleep. A waiting frame would go out on waking and put its
       * transmit in the next reading. Refused, not waited for: this is typed at a desk. */
      if (!meshService.txPipelineIdle()) {
        say("power sleep: refused - %d frame(s) waiting for the radio%s; try again in a few seconds\n",
            meshService.txQueued(), meshPhy.txBusy() ? " and one on the air" : "");
        return;
      }
      say("power sleep: %d s. Keypad (GPIO%d low) wakes early; serial typed meanwhile is lost.\n",
          secs, KEYBOARD_INTERRUPT_PIN);
      uart_wait_tx_done(PORT, pdMS_TO_TICKS(200));           // let that line leave before the clocks stop
      /* Flash and PSRAM stay powered through light sleep on this IDF (3.3): sleep_modes.c only
       * powers VDD_SDIO down under `#ifndef CONFIG_SPIRAM_SUPPORT`, and this build has PSRAM
       * on, so the heap (which lives in PSRAM) survives. There is no ESP_PD_DOMAIN_VDDSDIO to
       * pin here (that arrived in IDF 4). Load-bearing on an SDK bump: light sleep must keep
       * VDD_SDIO ON or the phone wakes into garbage. */
      esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
      esp_sleep_enable_ext0_wakeup((gpio_num_t)KEYBOARD_INTERRUPT_PIN, 0);   // SN7326 INT idles high
      const uint32_t t0 = millis();
      const esp_err_t r = esp_light_sleep_start();
      const uint32_t dt = millis() - t0;
      const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
      say("power sleep: back after %lu ms (%s), woke by %s\n", (unsigned long)dt,
          r == ESP_OK ? "ok" : "esp_light_sleep_start FAILED",
          cause == ESP_SLEEP_WAKEUP_TIMER ? "timer" : cause == ESP_SLEEP_WAKEUP_EXT0 ? "keypad" : "something else");
      return;
    }
    say("power: unknown switch - `?` lists them\n");
    return;
  }

  /* `maps` — the Maps app over a cable.
   *
   * ⚠ A MAP'S FAILURE MODE IS SHOWING THE WRONG GROUND, and that looks exactly like showing
   * the right ground: no screenshot, and no amount of looking at the phone, can tell a tile
   * tree that is one column out from one that is correct. So the console reports what the
   * card ACTUALLY holds and what the app will open on, and `maps goto` writes the same saved
   * view the app itself writes — which means a known coordinate can be put in over the cable
   * and the screen compared against a map on the computer. */
  /* `hold on|off` — keep the screen awake (and therefore unlocked) for a bench session.
   * Injected keys do not reset the 30 s sleep timer — only the keypad hardware does — so a
   * scripted walk through a screen goes dark and locked half way, and the lock state machine
   * then eats the next presses. This is gui.state.holdScreenAwake, the same hold the transfer
   * server and the map's progress screen take; `hold off` releases it. */
  if (!strncasecmp(line, "hold", 4) && (line[4] == '\0' || line[4] == ' ')) {
    extern GUI gui;
    static bool held = false;
    const char* arg = line + 4;
    while (*arg == ' ') arg++;
    const bool on = !strcasecmp(arg, "on") || !*arg;
    if (on != held) {
      gui.state.holdScreenAwake(on);
      held = on;
    }
    say("hold: screen %s\n", held ? "held awake" : "released");
    return;
  }
  /* `open <app>` — jump straight into an app, for the bench. `key` can walk the menus, but a
   * walk is only as good as its starting point: a phone that locked, woke into the dialer or
   * sat on a different menu row eats or misroutes the first presses, and a screenshot a
   * minute later shows the wrong screen with no way to tell which key went astray
   * (2026-09-18, an evening of it). This lands where it says. */
  if (!strncasecmp(line, "open", 4) && (line[4] == '\0' || line[4] == ' ')) {
    extern GUI gui;
    const char* arg = line + 4;
    while (*arg == ' ') arg++;
    ActionID_t app = GUI_APP_CLOCK;
    if (!strcasecmp(arg, "maps"))        app = GUI_APP_MAPS;
    else if (!strcasecmp(arg, "almanac")) app = GUI_APP_ALMANAC;
    else if (!strcasecmp(arg, "weather")) {               // the Almanac, opened on WEATHER (0.9.81)
      almanacOpenAtWeather(true);
      app = GUI_APP_ALMANAC;
    }
    else if (!strcasecmp(arg, "ai"))     app = GUI_APP_AI;
    else if (!strcasecmp(arg, "photos")) app = GUI_APP_PHOTOS;
    else if (!strcasecmp(arg, "books"))  app = GUI_APP_BOOKS;
    else if (!strcasecmp(arg, "music"))  app = GUI_APP_MUSIC;
    else if (!strcasecmp(arg, "mesh"))   app = GUI_APP_MESHTASTIC;
    else if (!strcasecmp(arg, "gbc"))    app = GUI_APP_GBC;     // the ROM picker; `key ok` starts the first game
    else if (!strcasecmp(arg, "files"))  app = GUI_APP_FILES;
    /* The two WiFi settings screens (0.9.79), for the restore bench: their constructors close a
     * sync window and stop a hotspot uploader, the list's exit hands the radio back
     * (wifiRestoreStation), and the edit screen's SELECT is Connect/Disconnect whatever has
     * focus. Landing on them by `key` walks through Settings was the fragile part. */
    else if (!strcasecmp(arg, "wifi"))   app = GUI_APP_NETWORKS;
    else if (!strcasecmp(arg, "wifiedit")) app = GUI_APP_EDITWIFI;
    else if (!strcasecmp(arg, "clock") || !*arg) app = GUI_APP_CLOCK;
    else {
      say("open: maps | almanac | weather | ai | photos | books | music | mesh | gbc | files | wifi | wifiedit | clock\n");
      return;
    }
    if (gui.openAppFromConsole(app)) {
      say("open: %s\n", arg[0] ? arg : "clock");
    } else {
      almanacOpenAtWeather(false);                        // a refused open leaves no WEATHER for later
      extern volatile bool gGbcActive;   // a game cannot coexist with a call: startGame turns WiFi off
      say("open: refused - %s\n", gGbcActive ? "a game is running (END, then Quit)" : "a call is up");
    }
    return;
  }
  /* `gbc` / `gbc autosave` — the Game Boy's resume point from the cable. `autosave` calls the
   * SAME function the two power-off paths call (gbcSaveForPowerOff), so the bench proves that
   * path without a thumb on the power button: it parks the game, writes /gbc/<rom>.auto,
   * and leaves the pause menu up (Resume to carry on). */
  if (!strncasecmp(line, "gbc", 3) && (line[3] == '\0' || line[3] == ' ')) {
    const char* arg = line + 3;
    while (*arg == ' ') {
      arg++;
    }
    extern bool gbcSaveForPowerOff();
    extern void gbcStatus(char* out, size_t n);
    if (!strcasecmp(arg, "autosave")) {
      const uint32_t t0 = millis();
      const bool ok = gbcSaveForPowerOff();
      say("gbc autosave: %s in %lu ms\n", ok ? "ok" : "NOT written (no game running, or the write failed - see GBC: lines)",
          (unsigned long)(millis() - t0));
      return;
    }
    char st[200];
    gbcStatus(st, sizeof(st));
    say("gbc: %s\n", st);
    return;
  }
  if (!strncasecmp(line, "maps", 4) && (line[4] == '\0' || line[4] == ' ')) {
    const char* arg = line + 4;
    while (*arg == ' ') {
      arg++;
    }
    /* `maps dl` — the on-phone tile downloader, driven without a thumb (tile_fetch.h).
     *   maps dl                                  status of the last/current run
     *   maps dl <src> <lat> <lon> <km> <zmax> [elev]  start: src 0=USGS Topo 1=USGS Aerial 2=OTM 3=custom
     *                                            (zmax is capped at the source's deepest: USGS 16,
     *                                            OTM 17); the job is kept and resumes by itself.
     *                                            elev 1/0 = the elevation layer too, or not; left
     *                                            out = the form's "Elevation too" (NVS maps/dlelev, ON)
     *   maps dl stop                             finish the current tile and exit; the job is
     *                                            forgotten (it will not resume)
     *   maps dlurl <template> [area]|clear       set the custom source (a plain-http relay) and the
     *                                            map area it fills (default "custom"; "otm" = the
     *                                            phone's OpenTopoMap map, from a Mac's tile pool)
     *   maps hold up|down|left|right <ms> [blip] press an arrow and HOLD it for <ms> — the
     *                                            bench for hold-to-scroll, where no finger
     *                                            is on the key (uiKeyBenchHold); <blip> ms
     *                                            in, fake the chip's release-and-re-press
     *                                            under a held finger (see uiKeyUpAgeMs) */
    if (!strncasecmp(arg, "hold", 4) && (arg[4] == '\0' || arg[4] == ' ')) {
      arg += 4;
      while (*arg == ' ') arg++;
      uint32_t mask = 0;
      char key = 0;
      if (!strncasecmp(arg, "up", 2))         { mask = WIPHONE_KEY_MASK_UP;    key = WIPHONE_KEY_UP;    arg += 2; }
      else if (!strncasecmp(arg, "down", 4))  { mask = WIPHONE_KEY_MASK_DOWN;  key = WIPHONE_KEY_DOWN;  arg += 4; }
      else if (!strncasecmp(arg, "left", 4))  { mask = WIPHONE_KEY_MASK_LEFT;  key = WIPHONE_KEY_LEFT;  arg += 4; }
      else if (!strncasecmp(arg, "right", 5)) { mask = WIPHONE_KEY_MASK_RIFHT; key = WIPHONE_KEY_RIGHT; arg += 5; }
      if (!mask) {
        say("maps hold: usage maps hold up|down|left|right [ms [blip]]\n");
        return;
      }
      while (*arg == ' ') arg++;
      /* Two numbers, both optional, nothing else: "1500x" or "1500 x" is a mistake to be
       * told about, not a plain hold to be run as though it were the blip test asked for. */
      char* end = (char*)arg;
      const long ms = *arg ? strtol(arg, &end, 10) : 1000;
      if (*arg && (end == arg || (*end && *end != ' '))) {
        say("maps hold: <ms> must be a number\n");
        return;
      }
      if (ms < 100 || ms > 10000) {
        say("maps hold: <ms> must be 100..10000\n");
        return;
      }
      while (*end == ' ') end++;
      long blip = 0;
      if (*end) {
        char* end2 = end;
        blip = strtol(end, &end2, 10);
        while (*end2 == ' ') end2++;
        if (end2 == end || *end2) {
          say("maps hold: <blip> must be a number of ms\n");
          return;
        }
        if (blip < 0 || blip >= ms) {
          say("maps hold: <blip> must be 0..<ms>\n");
          return;
        }
      }
      /* Inject first, arm second: an armed bench with no press dispatched would answer
       * "held" to the next real tap of that arrow for up to ten seconds. */
      if (!uiInjectKey(key)) {
        say("maps hold: key buffer full\n");
        return;
      }
      uiKeyBenchHold(mask, (uint32_t)ms, (uint32_t)blip);
      if (blip) {
        say("maps hold: pressed, held for %ld ms, a release blip at %ld ms\n", ms, blip);
      } else {
        say("maps hold: pressed, held for %ld ms\n", ms);
      }
      return;
    }
    if (!strncasecmp(arg, "dlurl", 5)) {
      arg += 5;
      while (*arg == ' ') arg++;
      char why[80];
      if (!*arg || !strcasecmp(arg, "clear")) {
        if (tileSetCustomUrl("", NULL, why, sizeof(why))) {
          say("maps dlurl: custom source cleared\n");
        } else {
          say("maps dlurl: NOT cleared - %s\n", why);
        }
        return;
      }
      /* `<template> [area]` (tilePlanParseDlurl, host-tested): the area is the map the relay's
       * tiles go into - "otm" fills the real OpenTopoMap map - and "custom" when left out. */
      char tmpl[200], area[32];
      if (!tilePlanParseDlurl(arg, tmpl, sizeof(tmpl), area, sizeof(area))) {
        say("usage: maps dlurl <http://host:port/.../{z}/{x}/{y}.png> [area]   (area: a map folder, not elev)\n");
        return;
      }
      if (tileSetCustomUrl(tmpl, area, why, sizeof(why))) {
        say("maps dlurl: custom source = %s into /maps/%s (source index %d)\n", tmpl, area, TILE_SRC_CUSTOM);
      } else {
        say("maps dlurl: REFUSED - %s\n", why);
      }
      return;
    }
    if (!strncasecmp(arg, "dl", 2) && (arg[2] == '\0' || arg[2] == ' ')) {
      arg += 2;
      while (*arg == ' ') arg++;
      if (!*arg) {
        tileFetchReport(sayLine);
        return;
      }
      if (!strcasecmp(arg, "stop")) {
        tileFetchStop();
        say("maps dl: stop requested; the job is forgotten and will not resume\n");
        return;
      }
      TileJobSpec spec;
      memset(&spec, 0, sizeof(spec));
      /* The five numbers, then the elevation tail through tilePlanParseElevArg (host-tested):
       * "[elev] 1|0" or nothing. Anything else is the usage line - never quietly "absent", which
       * is how `... 15 elev 0` used to download elevation (review 2026-09-27). */
      int used = 0;
      const int got = sscanf(arg, "%d %lf %lf %d %d%n", &spec.source, &spec.lat, &spec.lon,
                             &spec.radiusKm, &spec.zMax, &used);
      const int elevArg = (got == 5) ? tilePlanParseElevArg(arg + used) : -2;
      if (got < 5 || elevArg == -2) {
        say("usage: maps dl <src> <lat> <lon> <radiusKm> <zmax> [elev 1|0]   (maps dl | maps dl stop)\n");
        return;
      }
      if (elevArg >= 0) {
        spec.elev = elevArg == 1;
      } else {
        Preferences p;                     // the form's own choice ("Elevation too"), ON by default
        spec.elev = true;
        if (p.begin("maps", true)) {
          spec.elev = p.getInt("dlelev", 1) != 0;
          p.end();
        }
      }
      uint64_t card = 0, net = 0;
      int elevTiles = 0;
      const int n = tileFetchEstimate(&spec, &card, &net, &elevTiles);
      char why[96];
      if (tileFetchStart(&spec, why, sizeof(why))) {
        const TileSource* src = tileSource(spec.source);
        /* The depth USED: a z17 asked of USGS fetches to z16, and says so. */
        say("maps dl: started %s to z%d - %d tiles%s, ~%u MB down, %u MB on the card\n",
            src->label, spec.zMax > src->zMax ? src->zMax : spec.zMax, n,
            spec.elev ? " + elevation" : ", no elevation", (unsigned)(net >> 20), (unsigned)(card >> 20));
        if (spec.elev) {
          say("maps dl: elevation first - %d tiles (z13 + z10) to /maps/elev\n", elevTiles);
        }
      } else {
        say("maps dl: NOT started - %s\n", why);
      }
      return;
    }
    if (!strncasecmp(arg, "goto", 4)) {
      arg += 4;
      while (*arg == ' ') {
        arg++;
      }
      char area[32];
      area[0] = '\0';
      double lat = 0, lon = 0;
      int z = 15;
      const int got = sscanf(arg, "%lf %lf %d %31s", &lat, &lon, &z, area);
      if (got < 2) {
        say("usage: maps goto <lat> <lon> [zoom] [area]\n");
        return;
      }
      char why[96];
      const bool ok = mapsConsoleGoto(lat, lon, z, area, why, sizeof(why));
      say("maps goto: %s%s\n", ok ? "" : "REFUSED - ", why);
      return;
    }
    char buf[320];
    if (mapsConsoleStatus(buf, sizeof(buf))) {
      sayLines(buf);                       // one say() per line: say() holds 192 bytes
    } else {
      say("maps: could not read the card\n");
    }
    return;
  }

  /* `elev` — the map's altitude readout from its cache (0.9.80, docs/maps.md "Altitude"), and
   * `elev <lat> <lon>` — the elevation tiles sampled at any point, through the same reader the
   * map uses (elevSampleCard), timed: the one place the cost of a sample on this card is
   * measured. Card I/O from the console is fine (a bench command on the loop, as `ls` is). */
  if (!strcasecmp(line, "elev") || !strncasecmp(line, "elev ", 5)) {
    const char* arg = line + 4;
    while (*arg == ' ') {
      arg++;
    }
    if (!*arg) {
      /* ~500 bytes of report: PSRAM for the moment it takes, not the loop task's stack (8 KB,
       * a few hundred bytes of floor) and not a static in internal RAM. */
      const size_t cap = 640;
      char* buf = (char*)ps_malloc(cap);
      if (!buf) {
        say("elev: no memory for the report\n");
        return;
      }
      if (mapsConsoleElev(buf, cap)) {
        sayLines(buf);
      }
      free(buf);
      return;
    }
    char tmp[64];
    strlcpy(tmp, arg, sizeof(tmp));
    for (char* q = tmp; *q; q++) {
      if (*q == ',') {
        *q = ' ';                          // "47.49643,-121.79" or "47.49643 -121.79"
      }
    }
    double la = 0, lo = 0;
    char extra = 0;
    if (sscanf(tmp, "%lf %lf %c", &la, &lo, &extra) != 2 || la < -90 || la > 90 || lo < -180 ||
        lo > 180) {
      say("elev: usage elev [<lat> <lon>]  (degrees, e.g. elev 47.49643 -121.79)\n");
      return;
    }
    double m = 0;
    int z = -1;
    bool ioErr = false;
    const uint32_t t0 = micros();
    int rc = elevSampleCard(la, lo, &m, &z, &ioErr);
    const uint32_t us = micros() - t0;
    if (rc != 1 && ioErr) {
      rc = ELEV_TXT_IOERR;
    }
    char t[48], alt[24];
    elevSampleText(t, sizeof(t), rc, m, z);
    alt[0] = '\0';
    if (rc == 1 && gUnits == UNITS_US) {
      alt[0] = ',';
      alt[1] = ' ';
      unitsFmtAlt(m, UNITS_US, false, alt + 2, sizeof(alt) - 2);
    }
    say("elev: %.5f,%.5f -> %s%s [%lu.%lu ms%s]%s\n", la, lo, t, alt, (unsigned long)(us / 1000u),
        (unsigned long)((us % 1000u) / 100u), ioErr ? ", a read failed" : "",
        (rc == 0 && !elevCardPresent()) ? " - there is no /maps/elev on the card" : "");
    return;
  }

  /* `units` / `units metric|us` — the one device-wide setting (prefs_almanac.h): the map's scale
   * bar and distances, its altitude row, the Meshtastic lists, this console and the Almanac. */
  if (!strcasecmp(line, "units") || !strncasecmp(line, "units ", 6)) {
    const char* arg = line + 5;
    while (*arg == ' ') {
      arg++;
    }
    if (*arg) {
      if (!strcasecmp(arg, "metric") || !strcasecmp(arg, "m")) {
        unitsSetPref(UNITS_METRIC);
      } else if (!strcasecmp(arg, "us") || !strcasecmp(arg, "imperial")) {
        unitsSetPref(UNITS_US);
      } else {
        say("units: usage units [metric|us]\n");
        return;
      }
      say("units: now %s - saved (NVS wpmesh/units); an open screen shows it at its next redraw\n",
          gUnits == UNITS_US ? "US (ft, mi, mph, F)" : "metric (m, km, km/h, C)");
      return;
    }
    char d[16], a[24];
    unitsFmtDist(1400.0, gUnits, d, sizeof(d));
    unitsFmtAlt(412.0, gUnits, false, a, sizeof(a));
    say("units: %s - 1400 m reads \"%s\", 412 m of height \"%s\" (`units %s` to switch)\n",
        gUnits == UNITS_US ? "US" : "metric", d, a, gUnits == UNITS_US ? "metric" : "us");
    return;
  }

  /* `wallpaper` — the background loader, out loud, and the one way to drive Photos'
   * wallpaper path without a thumb. It exists because the failure it reports was INVISIBLE:
   * a rejected /background.jpg fell into the same fallback as "no wallpaper chosen", so the
   * screen and the log looked identical either way and "Set as wallpaper does nothing" had
   * no thread to pull ({user}, 2026-08-25). `set` runs the SAME function the menu runs — see
   * photosSetWallpaper() — so this cannot drift away from the feature it tests. */
  if (!strncasecmp(line, "wallpaper", 9) && (line[9] == '\0' || line[9] == ' ')) {
    extern GUI gui;
    const char* arg = line + 9;
    while (*arg == ' ') {
      arg++;
    }
    if (!strcasecmp(arg, "reload")) {
      gui.loadWallpaper();
    } else if (!strcasecmp(arg, "clear")) {
      /* The same operation as "[ Restore default wallpaper ]": delete the override and let
       * the compiled-in fallback surface. Nothing is copied in, so nothing can fail. */
      if (SD.exists(GUI::backgroundFile)) {
        say("wallpaper: removing %s from SD\n", GUI::backgroundFile);
        SD.remove(GUI::backgroundFile);
      } else {
        say("wallpaper: no override on SD to remove\n");
      }
      gui.loadWallpaper();
    } else if (!strcasecmp(arg, "list")) {
      File dir = SD.open("/photos");
      if (!dir || !dir.isDirectory()) {
        say("wallpaper: /photos is missing on the card\n");
      } else {
        File f;
        int n = 0;
        while ((f = dir.openNextFile())) {
          const char* nm = f.name();
          const char* base = strrchr(nm, '/');
          base = base ? base + 1 : nm;
          if (base[0] && base[0] != '.' && !f.isDirectory()) {
            say("  %-40s %u bytes\n", base, (unsigned)f.size());
            n++;
          }
          f.close();
        }
        dir.close();
        say("wallpaper: %d file(s) in /photos\n", n);
      }
      return;
    } else if (!strncasecmp(arg, "set ", 4)) {
      const char* name = arg + 4;
      while (*name == ' ') {
        name++;
      }
      char why[112] = "";
      const bool ok = photosSetWallpaper(name, why, sizeof(why));
      say("wallpaper set %s: %s - %s\n", name, ok ? "OK" : "REFUSED", why);
    } else if (*arg) {
      say("wallpaper: try `wallpaper`, `reload`, `list`, `set <name>`, `clear`\n");
      return;
    }

    /* ⚠ LOADING IT IS NOT SHOWING IT, and a screenshot taken without this line will quietly
     * show the PREVIOUS wallpaper and look like the load failed. ClockApp clones bgImage on
     * every redraw (GUI.cpp, ClockApp::redrawScreen) — but with no NTP the clock face has no
     * reason to redraw, so nothing repaints until something else happens. The Photos app does
     * not need this: it returns REDRAW_ALL. The console has to ask. */
    if (strcasecmp(arg, "list")) {
      gui.redrawScreen(true, true, true, true);
    }

    const bool onSd = SD.exists(GUI::backgroundFile);
    const bool onSpiffs = SPIFFS.exists(GUI::backgroundFile);
    say("wallpaper: %s\n", gui.getWallpaperNote());
    /* Name BOTH filesystems every time. SD is the one Photos writes and the one that was
     * unreadable at load time; SPIFFS is the one that always worked and therefore the one
     * that masked the fault. Printing only the winner hides exactly that asymmetry. */
    if (onSd) {
      File f = SD.open(GUI::backgroundFile, FILE_READ);
      say("  SD     %s  %u bytes\n", GUI::backgroundFile, f ? (unsigned)f.size() : 0u);
      if (f) {
        f.close();
      }
    } else {
      say("  SD     %s  absent\n", GUI::backgroundFile);
    }
    if (onSpiffs) {
      File f = SPIFFS.open(GUI::backgroundFile, FILE_READ);
      say("  SPIFFS %s  %u bytes  (SD wins when both exist)\n",
          GUI::backgroundFile, f ? (unsigned)f.size() : 0u);
      if (f) {
        f.close();
      }
    } else {
      say("  SPIFFS %s  absent\n", GUI::backgroundFile);
    }
    say("  limit  %u KB, baseline colour JPEG only (no progressive, no greyscale)\n",
        (unsigned)(GUI::backgroundFileMaxSize >> 10));
    return;
  }

  // %.40s: a pasted invite URL is ~370 chars and say()'s buffer is 192, so the
  // echo ate the "(try ?)" hint — the only useful half of this line.
  say("? unknown command: '%.40s'  (try ?)\n", line);
}

void serialCmdLoop() {
  uint8_t c;
  // 0 ticks = poll, never block. This runs every main-loop pass.
  while (uart_read_bytes(PORT, &c, 1, 0) == 1) {
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      s_buf[s_len] = '\0';
      if (s_len) {
        run(s_buf);
      }
      s_len = 0;
      continue;
    }
    if (s_len < sizeof(s_buf) - 1) {
      s_buf[s_len++] = (char)c;
    }
    // Over-long input is truncated rather than run in pieces: half a command is not a
    // command, and this is a debug console, not a protocol.
  }
}
