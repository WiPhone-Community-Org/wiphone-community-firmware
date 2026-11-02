/*
Copyright © 2019, 2020, 2021, 2022 HackEDA, Inc.
Licensed under the WiPhone Public License v.1.0 (the "License"); you
may not use this file except in compliance with the License. You may
obtain a copy of the License at
https://wiphone.io/WiPhone_Public_License_v1.0.txt.

Unless required by applicable law or agreed to in writing, software,
hardware or documentation distributed under the License is distributed
on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/

#include <dummy.h>

#include <HTTPClient.h>
#include <HTTPUpdate.h>


// TODO:
// - check WiFi before calling => checked so issue#69 is fixed
// - check if using strncpy correctly (does not terminate with a nul)

#include "esp32-hal.h"
#include <stdio.h>
#include "GUI.h"
#include "t9_extra.h"        // the user's own T9 words, loaded from the card at boot
#include "tinySIP.h"
#include "config.h"
#include "battery_curve.h"   // SOC from voltage, off phone 1's measured discharge (not the CW2015's guess)
#include "clock.h"
#include "Audio.h"
#include "lwip/api.h"
#include <WiFi.h>
#include "Networks.h"
#include "wifi_diag.h"         // WifiCardGate: the MARK assoc/disassoc card lines, one pair a minute
#include "esp_log.h"
#include <Update.h>
#include "ota.h"
#include "lora.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"   // DIAGNOSTIC: loop-stall watchdog, see setup()
#include "tile_fetch.h"      // the map downloader: DFS hold, pause-for-calls (below)
#include "ai_net.h"          // Menu > AI: the question's worker, folded into the chat each pass
#include "weather_net.h"     // the Almanac's weather: a fetch folded into the cache each pass (0.9.81)
#include "https_worker.h"    // netRequestActive: an AI question, a weather fetch or a Wi-Fi location look on the shared worker
#include "geoloc_net.h"      // Wi-Fi location: geolocSetup / geolocLoop (0.9.82)
#include "app_maps.h"        // gMapsActive: the map owns the side buttons while it is open
#include "prefs_almanac.h"   // gUnits / gLegalRule: loaded at boot beside the GPS switch (0.9.80)
#include "esp_wifi.h"       // esp_wifi_get_ps/set_ps: the modem-sleep invariant in loop()
#include "esp_bt.h"         // esp_bt_controller_mem_release: the Bluetooth reserve, given back at boot
#include "Test.h"
#include "meshtastic_service.h"
#include "music_player.h"
#include "app_gbc_xfer.h"
#include "sms_mirror_poll.h"   // pulls mirrored texts from COVEY over the LAN
#include "kosync_sync.h"      // KOSync: the reading-position sync window + home client
#include "phone_pos.h"        // the hotspot phone's GPS as a location source (NMEA over WiFi)
#include "tz_service.h"       // the clock's time zone: Auto from a measured fix, a named zone, or typed
#include "sms_mirror_rx.h"     // smsMirrorTakeNews: the mirror's arrival announcements
#include "serial_cmd.h"        // debug console on USB serial (uploader on/off, mirror sync)
#include "mp3_stream.h"
#include "src/assets/pop_sound.h"
#include "notify_timing.h"     // the pop's stop timer, derived from sizeof(pop_pcm)
#include "cpu_clock.h"         // the CPU gate's MHz: never a PLL re-lock under a running radio
#include "rtp_watch.h"         // a call's RTP: far-end silence per call, and the orphaned-session clock

static bool been_in_verify = false;

#ifndef WIPHONE_PRODUCTION
#include "Test.h"
#endif
//#define UDP_SIP   no need this here
extern "C" bool verifyOta() {
  log_d("In verify ota");
  been_in_verify = true;
  return true;
}

static Ota ota("");

GUI gui;
uint32_t chipId = 0;

// Legacy WiPhone RadioHead LoRa is disabled when the Meshtastic PHY owns the radio.
#if defined(LORA_MESSAGING) && !defined(MESHTASTIC_PHY)
static Lora lora;
#endif

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  PERIPHERALS  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

SN7326 keypad(SN7326_I2C_ADDR_BASE, I2C_SDA_PIN, I2C_SCK_PIN);
CW2015 gauge(CW2015_I2C_ADDR, I2C_SDA_PIN, I2C_SCK_PIN);

#if defined(MOTOR_DRIVER) && MOTOR_DRIVER == 8833
DRV8833 motorDriver = DRV8833();
#endif

#ifdef USER_SERIAL
HardwareSerial userSerial(2);
int userSerialLastSize = 0;
/* The woods backplate's GPS rides this same UART (GPIO 38 RX / 32 TX) but NOT
 * at the same baud: the GUI path is USER_SERIAL_BAUD, the GPS is gGpsBaud
 * (GPS_SERIAL_BAUD_DEFAULT = 115200, MEASURED — see Hardware.h for why 9600 was
 * wrong and why COVEY's own gps.py had the answer all along). With gGpsNmea on, bytes feed the NMEA reader and the
 * fix lands in meshService; off, the stock user-serial GUI path is untouched,
 * baud included. Persisted as wpmesh/gpsen + wpmesh/gpsbaud; toggled by serial
 * `gps on|off`, retuned by `gps baud <n>`. Default OFF until the plate exists. */
#include "nmea.h"
#include <Preferences.h>
bool       gGpsNmea = false;
uint32_t   gGpsBaud = GPS_SERIAL_BAUD_DEFAULT;
NmeaReader gGpsReader;
/* Set by setup() when the GPS owns the port, cleared by loop()'s first pass once the
 * port has been retuned to gGpsBaud. See the long note at the userSerial.begin() call:
 * the retune CANNOT happen in setup() without risking the INT_WDT panic this avoids. */
bool       gGpsBaudPending = false;

/* The last raw bytes off the wire, for serial `gps raw`. A wrong baud and a
 * module talking binary produce the SAME symptom (bytes climb, sentences stay
 * 0), and only looking at the bytes tells them apart: readable ASCII means the
 * baud is right, 0xB5 0x62 framing means it is speaking UBX, and neither means
 * the rate is still wrong. 64 bytes is under a single NMEA sentence and costs
 * nothing to keep. */
static uint8_t  gGpsRawBuf[64];
static uint8_t  gGpsRawHead = 0;
static uint32_t gGpsRawSeen = 0;

/* Retune the shared UART. Called by `gps on|off`, `gps baud`, the My node
 * toggle, and boot — every path that changes which consumer owns the port. */
void gpsApplyBaud(bool gpsOn) {
  userSerial.updateBaudRate(gpsOn ? gGpsBaud : USER_SERIAL_BAUD);
}

/* Copy the ring out oldest-first. Returns how many bytes landed in `out`. */
int gpsRawSnapshot(uint8_t* out, int cap) {
  int n = (int)(gGpsRawSeen < sizeof(gGpsRawBuf) ? gGpsRawSeen : sizeof(gGpsRawBuf));
  if (n > cap) {
    n = cap;
  }
  int start = (int)(gGpsRawHead + sizeof(gGpsRawBuf) - n) % (int)sizeof(gGpsRawBuf);
  for (int i = 0; i < n; i++) {
    out[i] = gGpsRawBuf[(start + i) % (int)sizeof(gGpsRawBuf)];
  }
  return n;
}
#endif

#ifdef USE_VIRTUAL_KEYBOARD
WiFiUDP *udpKeypad = NULL;
#endif

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  I2S AUDIO  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

Audio* audio;

void audio_test() {
  log_e("AUDIO TEST");
  if (audio->start()) {
    log_d("audio: started");
    audio->playRingtone(&SPIFFS);
    //audio->playFile(&SPIFFS, "/ringtone.mp3");
  } else {
    log_e("audio: failed");
  }
}

/* Description:
 *     Setup ringtone playback and vibration motor.
 *     For the ringtone to work, SPIFFS should have two files:
 *         ringtone.mp3
 *             - VERY preferably 16 KHz (or less) audio file
 *         ringtone.ini
 *             - INI file (compatible with NanoINI) which allows three configurations:
 *               - "vibro_on" - time the vibration motor is ON at a time, milliseconds
 *               - "vibro_off" - time the vibration motor is OFF at a time, milliseconds
 *               - "delay" - delay before the vibration motor turn ON for the first time, milliseconds
 *     TODO: generate both files if they are absent
 */
void notifyPopFinishFor(const char* why);   // defined with the pop, further down

/* ── THE CALL LEVELS, READ FROM WHERE THEY ARE KEPT (review SA-1, 2026-09-25) ────────────────
 * The ring and a call used to INHERIT whatever levels the codec held when they started. The
 * codec also carries every borrower's level while it is borrowed, and one that forgets to give
 * them back sets the next ring's loudness and the next call's earpiece: a game's F1/F2 did,
 * until this round; Development > My App's keys still do. Every owner of the call levels
 * writes them to configs.ini [audio] (Settings > Audio, the call screen's UP/DOWN) and boot
 * applies that file, so the file is where they are kept, and the ring and a call read them
 * from there.
 * ⚠ ONLY AFTER THE POP HAS FINISHED AND MUSIC HAS YIELDED - both put their own snapshots back,
 * which would land on top of these.
 * A missing key keeps what the codec holds; a file that will not load changes nothing and says
 * so. The line that matters on the bench is the other one: "...held X, stored Y - applied"
 * means a borrower left its level behind, and it names who took the device next.
 * ⚠ A level the call screen stepped and has not written yet (callLevelStepped() below) is
 * NEWER than the file, so it wins over the file here. */
static void callLevelsOverlay(int8_t& ear, int8_t& hp, int8_t& loud);   // just below

/* ── THE CALL SCREEN'S UP/DOWN: APPLIED AT ONCE, WRITTEN ONCE, OFF THE AUDIO PATH (review R3-1) ──
 * 🛑 EVERY PRESS USED TO READ AND REWRITE configs.ini ON THE LOOP TASK: ini.load() (SPIFFS.exists()
 * opens the file, then SPIFFS.open()) and ini.store() - three SPIFFS opens, measured at 0.5-1.6 s
 * EACH on these phones. Audio::loop() is pumped only from loop(), so for 1.5-5 s after every volume
 * press nothing played, no microphone packet went out and the far end's RTP overflowed its mailbox:
 * a gap both ways, mid-call, on the key you press BECAUSE you are listening. The same stall 6900e63
 * took out of connect. (Pre-existing on main; SA-1 narrowed what it stored, not what it cost.)
 * Now the press touches only RAM: the call screen steps the level the codec (or music's stash)
 * already holds - the stored levels went in at dial or at the ring - and records it here. It is
 * written ONCE: CALL_LEVEL_SETTLE_MS after the last press, with no call live or imminent, the
 * phone not ringing, and the device neither moving samples nor holding an RTP session - one
 * CriticalFile load + store, with the `loaded` guard 8b93e72 gave the old handler (a file that
 * will not load is never overwritten with three keys). Until then it overrides the file for the
 * next ring or dial (applyStoredCallVolumes), and Settings > Audio writes it first when it opens
 * (it is about to read the file itself, and its Save must be the newer write). */
#define CALL_LEVEL_SETTLE_MS 3000u
static uint8_t  s_callLevelDirty = 0;             // bit per CallLevelRoute (GUI.h)
static int8_t   s_callLevel[3] = {0, 0, 0};       // dB, indexed by CallLevelRoute
static uint32_t s_callLevelStepMs = 0;
static const char* const kCallLevelKey[3] = {"speaker_vol", "headphones_vol", "loudspeaker_vol"};

void callLevelStepped(uint8_t route, int8_t level) {
  if (route > CALL_LEVEL_LOUDSPEAKER) {
    return;
  }
  s_callLevel[route] = level;
  s_callLevelDirty |= (uint8_t)(1u << route);
  s_callLevelStepMs = millis();
}

static void callLevelsOverlay(int8_t& ear, int8_t& hp, int8_t& loud) {
  if (s_callLevelDirty & (1u << CALL_LEVEL_EARPIECE)) {
    ear = s_callLevel[CALL_LEVEL_EARPIECE];
  }
  if (s_callLevelDirty & (1u << CALL_LEVEL_HEADPHONES)) {
    hp = s_callLevel[CALL_LEVEL_HEADPHONES];
  }
  if (s_callLevelDirty & (1u << CALL_LEVEL_LOUDSPEAKER)) {
    loud = s_callLevel[CALL_LEVEL_LOUDSPEAKER];
  }
}

/* The one write. Tried ONCE per batch of presses: a file that will not load is not retried on
 * every pass (each try is a SPIFFS open), and the levels still hold for this boot.
 * noinline: loop() calls it, and the CriticalFile stays out of loop()'s 8 KB-task frame. */
static void __attribute__((noinline)) callLevelsFlush(const char* who) {
  const uint8_t dirty = s_callLevelDirty;
  if (!dirty) {
    return;
  }
  s_callLevelDirty = 0;
  CriticalFile ini(Storage::ConfigsFile);
  const bool loaded = (ini.load() || ini.restore()) && !ini.isEmpty();
  if (!loaded) {
    log_e("AUDIO: %s: configs.ini did not load - the call screen's level is applied, NOT saved "
          "(storing now would have wiped every other setting in the file)", who);
    return;
  }
  if (!ini.hasSection("audio")) {
    ini.addSection("audio");
  }
  for (uint8_t r = 0; r <= CALL_LEVEL_LOUDSPEAKER; r++) {
    if (dirty & (1u << r)) {
      ini["audio"][kCallLevelKey[r]] = (int32_t)s_callLevel[r];   // only the ones that moved (SA-1)
    }
  }
  const bool stored = ini.store();
  log_e("AUDIO: %s: the call screen's level%s %s (ear/hp/loud mask %u)", who,
        (dirty & (dirty - 1)) ? "s" : "", stored ? "saved" : "NOT saved - the store failed",
        (unsigned)dirty);
}

/* Settings > Audio, before it reads the file: never under a call (none can be open there - the
 * call screen owns the keys - but the rule is the rule). */
void callLevelsFlushNow(const char* who) {
  if (!gui.inCall() && !gui.state.ringing) {
    callLevelsFlush(who);
  }
}

/* noinline: its CriticalFile would otherwise sit in its caller's frame - and one caller,
 * callTakesAudioDevice(), is itself inlined into loop(), so a second-hand inline puts it in
 * loop()'s 8 KB-task frame for the whole pass. GCC keeps it out of line today (96 B frame of its
 * own, loop() at 240 B - objdump, 2026-09-26); the attribute is what keeps it there the day
 * startRingtone() stops calling it (tests/check_health_log.py, integration review 3). */
static void __attribute__((noinline)) applyStoredCallVolumes(const char* who) {
  if (!audio) {
    return;
  }
  CriticalFile ini(Storage::ConfigsFile);
  if (!((ini.load() || ini.restore()) && !ini.isEmpty() && ini.hasSection("audio"))) {
    log_e("AUDIO: %s: configs.ini [audio] did not load - the codec's levels are kept", who);
    return;
  }
  int8_t ear, hp, loud;
  audio->getVolumes(ear, hp, loud);
  const int8_t e0 = ear, h0 = hp, l0 = loud;
  ear = ini["audio"].getIntValueSafe("speaker_vol", ear);
  hp = ini["audio"].getIntValueSafe("headphones_vol", hp);
  loud = ini["audio"].getIntValueSafe("loudspeaker_vol", loud);
  callLevelsOverlay(ear, hp, loud);           // a press not written yet is newer than the file
  if (ear == e0 && hp == h0 && loud == l0) {
    return;                                   // the codec already holds them: no bus traffic
  }
  audio->setVolumes(ear, hp, loud);
  audio->getVolumes(ear, hp, loud);           // setVolumes() clamps: the readback is the truth
  if (ear != e0 || hp != h0 || loud != l0) {
    log_e("AUDIO: %s: the codec held %d/%d/%d dB, the stored call levels are %d/%d/%d - applied",
          who, (int)e0, (int)h0, (int)l0, (int)ear, (int)hp, (int)loud);
  }
}

/* ── A CALL TAKES THE AUDIO DEVICE: music yields, the stored levels, THEN the call's route ─────
 * (review SA-2, 2026-09-25.) Nothing chose a CALLER's route: the CallApp constructor's
 * LOUDSPEAKER, meant for the ring, stood for the whole call - the far end came out of the back
 * speaker with the phone at your ear while the key offered "Loud Spkr" - and music's yield,
 * which puts music's own saved route back, could land on top of it, so the same outgoing call
 * played on the loudspeaker or the earpiece depending on whether a track had played. The route
 * is the call screen's key (ControlState::callLoudspeaker, reset per call: the earpiece).
 * Called when dialling, so UP/DOWN before connect steps the level of the route the call will
 * use (SA-1), and again at connect, because a pop can start while the far end rings.
 * ⚠ The caller finishes a pop in flight FIRST (notifyPopFinishFor) - the ring's order: the pop's
 * restore() would put its snapshot back over all of this. This is not the loop-level yield;
 * that one waits for a pop to end, these end it.
 * 🛑 `readStoredLevels` IS FALSE AT CONNECT. applyStoredCallVolumes() opens configs.ini, and a
 * SPIFFS open on these phones has measured 0.5-1.6 s: at connect that stall lands with the RTP
 * port still closed, so the far end's first words ("Hello?") are lost for its length. And it
 * gains nothing there: the same stored levels went in moments earlier - when dialling for a
 * caller (before the INVITE, off the audio path), at the ring for a callee - and nothing in
 * between leaves them changed: the call screen's UP/DOWN applies at once and records the level
 * for the post-call write (callLevelStepped()/callLevelsOverlay()), so the codec already holds
 * it at connect; a pop is finished first and puts back its own snapshot of them, and music's
 * yield gives back the ones it saved. Connect still yields and still chooses the route - both
 * are RAM and bus writes. */
static void callTakesAudioDevice(const char* who, bool readStoredLevels) {
  musicPlayerYieldForCall();
  if (readStoredLevels) {
    applyStoredCallVolumes(who);
  }
  audio->chooseSpeaker(gui.state.callLoudspeaker);
}

void startRingtone() {

  /* ── TAKE THE DEVICE FROM WHOEVER HAS IT, IN THIS ORDER ────────────────────────────────
   * 🛑 The ring used to inherit the MUSIC level: playRingtone() below calls ceasePlayback(),
   * so by the time the main loop's call-wins-over-music check asked musicPlayerIsPlaying() the
   * answer was already "no", musicPlayerPause() was skipped, and restoreCallVolume() never ran.
   * The phone rang at the music level (default -18 dB, as low as -45), the answered call's
   * earpiece stayed there, the stash went stale, and the next F1 restarted the track at 0:00.
   *
   * ⚠ THE POP FIRST, THEN THE MUSIC. A pop in flight holds a snapshot taken while music owned
   * the codec — the MUSIC level — and its restore() puts that back. Finishing it after the
   * music had handed back the call levels would undo them.
   * ⚠ THE LOUDSPEAKER IS CHOSEN HERE, not inherited. Giving music's route back resets it —
   * without the line below the ring would go to the earpiece whenever music had been playing.
   * (The CallApp constructor used to pick it too, BEFORE this; it picks nothing now - SA-2.)
   * ⚠ SO ARE THE LEVELS (review SA-1): the stored call levels, the loudspeaker one being
   * Settings > Audio's "Loudspeaker volume" - not whatever a game or a test app left in the
   * codec. After the yield, which puts music's stash back. */
  notifyPopFinishFor("the phone is ringing");
  musicPlayerYieldForCall();
  applyStoredCallVolumes("ring");

  /* Ringer mode, from Settings > Audio (persisted, loaded at boot by GUI::loadSettings).
   * The motor is driven below regardless of the tone, so "vibrate only" is simply not
   * starting the audio at all — which also avoids turning the codec on for nothing. */
  const uint8_t mode = gui.state.ringerMode;
  const bool playTone = (mode == ControlState::RINGER_RING_AND_VIBRATE);
  const bool doVibrate = (mode != ControlState::RINGER_SILENT);

  if (playTone) {
    audio->chooseSpeaker(true);      // the ring is for the room - see the note at the top

    // Start audio
    audio->start();

    // Start playing ringtone
    if (!audio->playRingtone(&SPIFFS)) {
      log_d("ERROR: could not play file in SPIFFS");
    }
  } else {
    log_d("ringer: tone suppressed (mode %d)", (int)mode);
  }

  // Initialize vibrating
  gui.state.vibroOn = false;
  gui.state.vibroToggledMs = millis();

  // Default configuration
  gui.state.vibroOnPeriodMs   = 500;
  gui.state.vibroOffPeriodMs  = 2500;
  gui.state.vibroDelayMs      = gui.state.vibroOnPeriodMs + gui.state.vibroOffPeriodMs;
  gui.state.vibroNextDelayMs  = gui.state.vibroDelayMs;

  // Initialize the keyboard LED and motor: both OFF
  allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
  allDigitalWrite(KEYBOARD_LED, HIGH);

  // Load vibromotor configuration
  IniFile ini("/ringtone.ini");
  if (ini.load() && !ini.isEmpty()) {
    gui.state.vibroOnPeriodMs  = ini[0].getIntValueSafe("vibro_on", gui.state.vibroOnPeriodMs);
    gui.state.vibroOffPeriodMs = ini[0].getIntValueSafe("vibro_off", gui.state.vibroOffPeriodMs);
    gui.state.vibroDelayMs     = ini[0].getIntValueSafe("delay", gui.state.vibroOnPeriodMs + gui.state.vibroOffPeriodMs);
    gui.state.vibroNextDelayMs = gui.state.vibroDelayMs;

    log_d("vibro on = %d", gui.state.vibroOnPeriodMs);
    log_d("vibro off = %d", gui.state.vibroOffPeriodMs);
    log_d("vibro delay = %d", gui.state.vibroDelayMs);
  }

  // Kickstart ringing
  gui.state.ringing = true;
}

/* Description:
 *     ringtone stops ringing in two cases:
 *       1) user reacted: accepted or declined an incoming call
 *       2) incoming call got cancelled before user reacted
 */
void stopRingtone() {
  audio->shutdown();
  gui.state.ringing = false;
  gui.state.vibroOn = false;
  allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
  allDigitalWrite(KEYBOARD_LED, HIGH);
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  HEADPHONE INTERRUPT  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

volatile bool headphoneEvent = false;

void IRAM_ATTR headphoneInterrupt() {
  headphoneEvent = true;
}

// Function that is called after interrupt occurs (not within interrupt)
void headphoneServiceInterrupt() {
#ifdef HEADPHONE_DETECT_PIN
  bool headphones = allDigitalRead(HEADPHONE_DETECT_PIN);
#else
  bool headphones = false;      // TODO: maybe make headphone detect for version 1.3
#endif // HEADPHONE_DETECT_PIN
  log_d("Headphones event = %d", headphones);
  audio->jackSensed(headphones);          // the ONE writer of the jack's reading: see Audio.h (A5)
  headphoneEvent = false;
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  KEYPAD INTERRUPT & PROCESSING  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

#define KEYBOARD_BUFFER_LENGTH 13         // up to 12 keypresses can be remembered

volatile uint8_t keypadToRead = 0;
uint32_t keypadState = 0;        // 32-bit mask for current state of buttons
// Per-key timestamp of the last "pressed" report. The SN7326 re-reports held
// keys about every 40ms (LONGPRESS_EN), so during gameplay a key that misses
// its heartbeats is a lost release event and gets cleared (stale sweep below).
static uint32_t keyLastSeenMs[32];
/* Per-key timestamp of the last release, for UI debouncing: a "new press" within
 * KEY_BOUNCE_MS of the same key's release is a stale hold re-report, not a human
 * double-tap — humans can't re-tap under ~100ms.
 *
 * ⚠ THIS CLOCK IS TAKEN AT DRAIN TIME, NOT AT EVENT TIME, and the chip carries no
 * timestamps. When several events come out of the FIFO in ONE batch they all read the
 * same millis(), so a release and the press that follows it look SIMULTANEOUS however
 * far apart the finger actually made them — and the press is thrown away as bounce.
 * That is the missed-tap bug (2026-08-22); see releasedThisBatch below, which is the
 * exemption that fixes it. */
static uint32_t keyLastUpMs[32];
/* ⚠ 40 ms UNTIL 2026-08-22, AND IT HAD NEVER ACTUALLY BEEN TESTED. While every release was
 * being applied to CALL this window was armed on the wrong key, so it did nothing for the
 * key being typed; attributing releases correctly armed it FOR THE FIRST TIME. 40 ms was
 * therefore an untested number sitting directly in the path of fast same-key tapping —
 * which is precisely the tentap case. Measured bounces are 6 ms and 13 ms, and no human
 * re-taps a key inside 80 ms, so 25 ms clears the real bounce with margin either side. */
#define KEY_BOUNCE_MS  25u
// Keys the UI considers held down. UI key events are strictly edge-triggered
// off THIS mask: one event per physical press, no matter how long it's held
// and no matter what the chip re-reports meanwhile. Only a real release event
// (or 350ms of heartbeat silence = lost release) re-arms the key. This is
// deliberately separate from keypadState, which other heuristics may clear
// mid-hold (that's what double-clicked and auto-repeated the menus).
static uint32_t uiKeyDown = 0;

/* ── WHY THE KEYPAD IS POLLED AS WELL AS INTERRUPTED ──────────────────────────────────
 * The SN7326's INT is a PULSE, not a level: AUTO_CLEAR_5MS(2) drops it 10 ms after it
 * asserts whether or not anything read the FIFO. The ESP32 side is attached FALLING. So
 * any event the chip produces while INT is ALREADY LOW adds no new edge, and the main
 * loop is never told about it — the event just sits in the FIFO.
 *
 * That is not a rare alignment. A held key re-reports every 40 ms (LONGPRESS_DELAY(1)),
 * each re-report pulsing INT low for 10 ms, so roughly ONE RELEASE IN FOUR lands inside
 * the shadow of the heartbeat before it and is never announced. A lost release leaves
 * uiKeyDown set, and a key whose uiKeyDown is set is DEAF — until the 350 ms stale sweep
 * clears it. A person re-taps in 150–250 ms, which is inside that window. That is
 * exactly what "it misses a tap and I have to tap again" is.
 *
 * The fix is to stop relying on the edge alone: poll the FIFO while input is in flight.
 * Not always — polling an idle phone would cost battery for nothing — but while a key is
 * believed down (which is precisely when a release may be stuck) and for a moment after
 * the last activity. An idle phone still does no I2C at all.
 *
 * ⚠ The game path has polled at 150 ms since the emulator landed, for this same reason.
 * The UI never did, which is why this only ever showed up as "menus miss the odd press".
 */
#define KEYPAD_POLL_MS       40u     // one chip heartbeat
#define KEYPAD_POLL_TAIL_MS  1000u   // keep polling this long after the last event
/* Threshold for "this press is too old to be a hold continuing".
 * ⚠ MEASURED: the chip re-reports a held key every ~58 ms (traced 2026-09-20, phone 2 on
 * the map) — an earlier trace (2026-08-22, menus) read ~109 ms; both are the chip's, and
 * NEITHER is the 40 ms the LONGPRESS_DELAY(1) comment in SN7326.h claims. 100 ms was
 * therefore BELOW the 109 ms reading and every heartbeat read as a fresh press. Kept only
 * as a counter now (see the note at its use), but if anything ever acts on it again it must
 * sit well above the slower heartbeat with room for jitter — and below the 350 ms sweep to
 * be worth anything. The 350 ms sweep is the ONE threshold that acts on the heartbeat, and
 * it is clear of both readings (six beats or three). */
#define KEY_HOLD_GAP_MS      100u
// (named ...Activity, not ...Event: GUI has its own msLastKeypadEvent member for the
// screen-dimming timer and the two are not the same clock.)
static uint32_t msLastKeypadActivity = 0;

/* Input health counters. These exist because the bug above was invisible for two years:
 * a dropped keypress logs nothing and looks like a bad thumb. Read them with `keys` on
 * the serial console. What each one means is written next to where it is incremented. */
static uint32_t kcPollDrained   = 0;   // events the poll recovered that no edge announced
static uint32_t kcBatchRescued  = 0;   // presses saved from the same-batch bounce collapse
static uint32_t kcBounceKilled  = 0;   // presses dropped as bounce (should be near zero)
static uint32_t kcStaleSwept    = 0;   // holds ended by the 350ms sweep = lost releases
static uint32_t kcGapRescued    = 0;   // presses saved from a hold that had silently ended
static uint32_t kcGapMissed     = 0;   // ...and the ones we could not vouch for, still dropped
static uint32_t kcBuffFull      = 0;   // presses dropped because keypadBuff had no room
static uint32_t kcI2cErr        = 0;   // failed reads (each one retries next pass)
static uint32_t kcEmptyPolls    = 0;   // polls that found nothing — the cost of the fix
static uint32_t kcReleaseFixed  = 0;   // releases the chip reported as key 0, re-attributed
static uint32_t kcRelAmbiguous  = 0;   // ...and the ones left alone because 2+ keys were down

/* ── RAW EVENT TRACE ──────────────────────────────────────────────────────────────────
 * The counters say WHAT is going wrong; this says what the CHIP actually sent, which is
 * the only way to settle whether a lost release was never emitted, was emitted and
 * mis-decoded, or was stranded. Each entry is one status byte plus the gap in ms since
 * the one before it. 64 entries is a few seconds of typing — enough to hold a whole
 * "tap, nothing happened, tap again" and its neighbours. Read it with `keys raw`.
 * ⚠ 'P' = the byte carried the PRESSED bit, 'r' = release, '+' = the MORE bit was set,
 * '?' = the code decoded to no key at all (mask 0), which would be its own finding. */
#define KEYTRACE_N 64
static uint8_t  ktByte[KEYTRACE_N];
static uint16_t ktGap[KEYTRACE_N];
static uint8_t  ktFlag[KEYTRACE_N];      // bit0 polled, bit1 unknown-code
static uint8_t  ktHead = 0;
static uint32_t ktLastMs = 0;
static inline void keyTrace(uint8_t b, bool polled, bool unknown, bool dropped = false) {
  (void)dropped;
  const uint32_t nowMs = millis();
  uint32_t d = ktLastMs ? (nowMs - ktLastMs) : 0;
  ktLastMs = nowMs;
  ktByte[ktHead] = b;
  ktGap[ktHead]  = d > 65535 ? 65535 : (uint16_t)d;
  ktFlag[ktHead] = (polled ? 1 : 0) | (unknown ? 2 : 0) | (dropped ? 4 : 0);
  ktHead = (ktHead + 1) % KEYTRACE_N;
}

/* One line of keypad health, for the `keys` serial command and the health log.
 *
 * HOW TO READ IT after a few minutes of ordinary menu use and typing:
 *   drained>0             the INT pulse really is losing events. Theory confirmed; the
 *                         poll is doing the job it was added for.
 *   rescued>0             taps that the drain-time bounce test used to eat. Each one is a
 *                         press the phone would have ignored before this build.
 *   swept  ~0             releases are no longer being lost outright. Before the poll this
 *                         was the count of 350ms deaf spells.
 *   gap    ~0             no evidence of unseen re-presses left over.
 *   killed high           the bounce window is too wide and is eating real taps — lower
 *                         KEY_BOUNCE_MS. Near zero is normal.
 *   full/err >0           the loop is blocking, or the I2C bus is unhappy. Both are their
 *                         own investigation; neither should ever move. */
/* Oldest-first dump of the raw trace. Format per entry: `+123ms 0x4B P +` */
/* One entry of the trace, oldest first: i = 0..KEYTRACE_N-1. False past the last entry
 * written (the caller stops there). ⚠ ONE LINE PER CALL, by design: the trace used to be
 * formatted whole into a 1,400-byte buffer and printed with one say() — whose buffer is 192
 * bytes — so the console showed the OLDEST twelve entries and dropped the rest without a
 * word, and the rest is where a hold went wrong (2026-09-20). A line is at most ~30 bytes. */
bool keypadTraceLine(int i, char* out, int cap) {
  if (i < 0 || i >= KEYTRACE_N) {
    return false;
  }
  const uint8_t k = (ktHead + i) % KEYTRACE_N;
  if (!ktGap[k] && !ktByte[k] && !ktFlag[k]) {
    out[0] = '\0';                                 // never written: an empty line to skip
    return true;
  }
  snprintf(out, cap, "+%ums 0x%02X %c%s%s",
           (unsigned)ktGap[k], ktByte[k],
           (ktByte[k] & SN7326_PRESSED) ? 'P' : 'r',
           (ktByte[k] & SN7326_MORE) ? " +more" : "",
           (ktFlag[k] & 2) ? " ?UNKNOWN" : ((ktFlag[k] & 1) ? " (poll)" : ""));
  return true;
}

int keypadHealth(char* out, int cap) {
  return snprintf(out, cap,
                  "relfix=%lu relamb=%lu killed=%lu gapfix=%lu gapmiss=%lu swept=%lu drained=%lu rescued=%lu full=%lu err=%lu empty=%lu",
                  (unsigned long)kcReleaseFixed, (unsigned long)kcRelAmbiguous,
                  (unsigned long)kcBounceKilled,
                  (unsigned long)kcGapRescued,   (unsigned long)kcGapMissed,
                  (unsigned long)kcStaleSwept,   (unsigned long)kcPollDrained,
                  (unsigned long)kcBatchRescued, (unsigned long)kcBuffFull,
                  (unsigned long)kcI2cErr,       (unsigned long)kcEmptyPolls);
}
/* The boot banner, held until the first health tick can commit it. ⚠ It cannot be written
 * when it is produced: setup() logs the reset reason before the card is known good, so
 * writing there would silently drop the one line that explains a restart. */
static char bootLine[120] = {0};
/* millis() of the last key actually dequeued for the UI. GUI::msLastKeypadEvent is protected
 * and the keypad is drained here anyway, so this is stamped at the source rather than widening
 * GUI's interface for one reader. Used only to hold off the mesh database save while somebody
 * is scrolling (~1.5-2.8 s on SPIFFS, ~0.1 s on the card) — see MeshtasticService::setUiIdle().
 * ⚠ The 0.6-1.5 s 'mesh' stalls were LoRa transmits, not this save (mesh_txq.h). */
static uint32_t gLastKeyMs = 0;

/* Time one named step of the superloop and say so if it blocked. The whole-pass stall detector
 * says THAT a pass froze; this says WHICH call did it. Added chasing the freeze {user} still felt
 * while scrolling after the database save was moved off the input path — the persisted STALL
 * records showed `scr=65 cpu=240 wifi=6`, i.e. screen on, phone active, WiFi already down, so
 * the remaining culprit is somewhere in the reconnect path rather than in storage. */
#define TIME_STEP(name, call)                                                          do {                                                                                   const uint32_t _t0 = millis();                                                       call;                                                                                const uint32_t _d = millis() - _t0;                                                  if (_d > 150) {                                                                        log_e("SLOW STEP: %s took %u ms (wifi=%d) - the whole loop waited on it",                   name, (unsigned)_d, (int)WiFi.status());                                     }                                                                                  } while (0)
// True while the Game Boy emulator app is running. The main loop then skips the
// mesh/LoRa polling so the emulator has the SPI bus and CPU to itself.
volatile bool gGbcActive = false;
// Sticky "pressed since last read" mask for the emulator. Set on every key-down
// here (the main loop sees every press); the emulator ORs it into the held state
// and clears it each frame, so a fast tap isn't missed between its slow polls.
volatile uint32_t gGbcKeyLatch = 0;
RingBuffer<char> keypadBuff(KEYBOARD_BUFFER_LENGTH);        // TODO: maybe used cbuf.h from ESP32 stack?

/* Press a key from somewhere that is not the keypad — the `key` serial command.
 *
 * 🔑 IT PUSHES INTO keypadBuff, WHICH IS THE POINT. Everything downstream — the drain loop,
 * the screen wake, the triple-tap-to-sleep tracker, the easter-egg buffer, the app's own
 * processEvent — then runs EXACTLY as it does for a real press, because it cannot tell the
 * difference. A second dispatch path would be a second thing to keep in step, and the class
 * of bug it would hide is precisely the one that put an untested Photos app in a user's hand.
 *
 * ⚠ Same task as the keypad reader (both run inside loop()), so no locking is needed here —
 * and none would be right anyway: RingBuffer is not reentrant.
 * Returns false if the buffer is full, which is a real answer, not a detail to swallow. */
bool uiInjectKey(char c) {
  if (keypadBuff.full()) {
    return false;
  }
  keypadBuff.put(c);
  return true;
}

/* Is this key STILL physically down? The one fact the map's hold-to-scroll needs, read
 * straight from the keypad reader's own state: uiKeyDown holds the bit from the press until
 * the release (or the 350 ms stale sweep), and keyLastSeenMs[] is stamped by every heartbeat
 * the chip sends while a key is held (~58-109 ms, see KEY_HOLD_GAP_MS). Both are
 * ANDed so a lost release — bit still set, heartbeats gone — reads as "up" within a third
 * of a second, and a map can never scroll away on its own.
 *
 * 🛑 THIS IS A QUESTION, NOT AN EVENT. Held-key re-reports are deliberately suppressed on
 * the UI path (uiSuppress / alreadyDown above): that is what killed the menus' auto-repeat
 * and the double-clicks, and {user}'s word on 2026-09-19 was "for everything but maps, the
 * d-pad works well". So nothing here synthesises a keypress. The map asks this on its own
 * timer and does its own repeating; every other app is untouched.
 *
 * The bench override lets `maps hold <dir> <ms>` prove the repeat from the cable, where no
 * finger is on the key: it answers "held" for that mask until the deadline. Same task as
 * everything else here, so plain variables. */
static uint32_t s_benchHeldMask = 0;
static uint32_t s_benchHeldUntil = 0;
/* The bench's fake of the chatter blip (see uiKeyUpAgeMs): at s_benchBlipAt the next poll
 * is answered "up" once and the release is stamped, exactly what one chip-emitted release
 * followed by a re-press 4 ms later looks like from the map. 0 = no blip. */
static uint32_t s_benchBlipAt = 0;
static uint32_t s_benchBlipUpMs = 0;
bool uiKeyStillHeld(uint32_t mask) {
  if (!mask) {
    return false;
  }
  /* ⚠ A KEY BEING POLLED AS HELD IS A KEY IN USE. gLastKeyMs is what holds the mesh DB save
   * (blocking; ~0.1 s on the card, seconds on SPIFFS) off while the phone is being touched, and
   * it is stamped when a key is DEQUEUED — a hold dequeues nothing after its press, so three
   * seconds into a sweep the save landed under the finger (review, 2026-09-19). Every "held"
   * answer stamps it. */
  if ((s_benchHeldMask & mask) == mask) {
    if ((int32_t)(s_benchHeldUntil - millis()) > 0) {
      if (s_benchBlipAt && (int32_t)(millis() - s_benchBlipAt) >= 0) {
        s_benchBlipAt = 0;               // one "up" answer, then held again
        s_benchBlipUpMs = millis();
        return false;
      }
      gLastKeyMs = millis();
      return true;
    }
    s_benchHeldMask = 0;
    s_benchBlipAt = 0;
  }
  if ((uiKeyDown & mask) != mask) {
    return false;
  }
  const uint8_t b = __builtin_ctz(mask);
  if (millis() - keyLastSeenMs[b] >= 350u) {
    return false;
  }
  gLastKeyMs = millis();
  return true;
}
/* How long ago this key's last RELEASE was decoded, in ms (huge if never).
 *
 * 🛑 THE CHIP EMITS A RELEASE UNDER A HELD FINGER. Traced on phone 2, 2026-09-20, while
 * {user} held RIGHT on the map ("works for one to 3 or 4 jumps before it just stops"):
 *
 *     +58ms 0x5C P     heartbeats every ~58 ms while held...
 *     +124ms 0x00 r    ...two beats of silence, then a RELEASE (code 0)...
 *     +4ms 0x5C P      ...and the same key pressed again 4 ms later, beats resuming.
 *
 * Nothing left the key. It happened at the first re-report slot (123-125 ms after the
 * press, three holds running) and mid-hold (2-4 repeats in, four more), 22 heartbeat gaps
 * in a few minutes. The release cleared uiKeyDown, the map polled "up" and dropped its
 * hold, and the re-press 4 ms later was killed as contact bounce (KEY_BOUNCE_MS, rightly:
 * a menu must not double-step) — so the finger stayed down on a map that had stopped.
 * The map now treats a release undone within MAP_PAN_HOLD_BLIP_MS as the hold continuing,
 * and this is how it measures that. The bench blip above answers the same question. */
uint32_t uiKeyUpAgeMs(uint32_t mask) {
  if (!mask) {
    return 0xFFFFFFFFu;
  }
  if ((s_benchHeldMask & mask) == mask && s_benchBlipUpMs) {
    return millis() - s_benchBlipUpMs;
  }
  const uint8_t b = __builtin_ctz(mask);
  if (!keyLastUpMs[b]) {
    return 0xFFFFFFFFu;
  }
  return millis() - keyLastUpMs[b];
}
void uiKeyBenchHold(uint32_t mask, uint32_t ms, uint32_t blipAtMs) {
  s_benchHeldMask = mask;
  s_benchHeldUntil = millis() + ms;
  s_benchBlipAt = blipAtMs ? millis() + blipAtMs : 0;
  s_benchBlipUpMs = 0;
}
/* For the loop's own hold trackers (F2 for the music player, a held digit, a held '#'): is
 * this key down, or released so recently that the chip's blip (uiKeyUpAgeMs, above) has not
 * had its 4 ms to put it back? `tracking` is the tracker's own "a hold is in progress" state,
 * so a key that was simply up stays up — the tolerance applies only inside a hold. What it
 * costs: a real release is acted on up to KEY_BLIP_MS later. What it saves: a tap of F2
 * skipping two tracks (the blip read as a release, the re-press as a second tap), a held
 * digit typing itself twice. */
#define KEY_BLIP_MS 40u
static inline bool uiKeyDownOrBlip(uint32_t mask, bool tracking) {
  if ((s_benchHeldMask & mask) == mask) {
    /* The bench (`key hold`): the same answer the map gets, blip and deadline included, so
     * a hold can be proven from the cable with no finger on the key. */
    return uiKeyStillHeld(mask) || (tracking && uiKeyUpAgeMs(mask) < KEY_BLIP_MS);
  }
  if (uiKeyDown & mask) {
    return true;
  }
  return tracking && uiKeyUpAgeMs(mask) < KEY_BLIP_MS;
}

/* The keypad bit for a key code — the reverse of the switch in keyboardRead(), for the
 * bench's `key hold <key>`; 0 for a code that is not a key. */
uint32_t uiKeyMaskFor(char c) {
  switch (c) {
  case '0': return WIPHONE_KEY_MASK_0;
  case '1': return WIPHONE_KEY_MASK_1;
  case '2': return WIPHONE_KEY_MASK_2;
  case '3': return WIPHONE_KEY_MASK_3;
  case '4': return WIPHONE_KEY_MASK_4;
  case '5': return WIPHONE_KEY_MASK_5;
  case '6': return WIPHONE_KEY_MASK_6;
  case '7': return WIPHONE_KEY_MASK_7;
  case '8': return WIPHONE_KEY_MASK_8;
  case '9': return WIPHONE_KEY_MASK_9;
  case '*': return WIPHONE_KEY_MASK_ASTERISK;
  case '#': return WIPHONE_KEY_MASK_HASH;
  case WIPHONE_KEY_UP:     return WIPHONE_KEY_MASK_UP;
  case WIPHONE_KEY_DOWN:   return WIPHONE_KEY_MASK_DOWN;
  case WIPHONE_KEY_LEFT:   return WIPHONE_KEY_MASK_LEFT;
  case WIPHONE_KEY_RIGHT:  return WIPHONE_KEY_MASK_RIFHT;
  case WIPHONE_KEY_OK:     return WIPHONE_KEY_MASK_OK;
  case WIPHONE_KEY_BACK:   return WIPHONE_KEY_MASK_BACK;
  case WIPHONE_KEY_SELECT: return WIPHONE_KEY_MASK_SELECT;
  case WIPHONE_KEY_CALL:   return WIPHONE_KEY_MASK_CALL;
  case WIPHONE_KEY_END:    return WIPHONE_KEY_MASK_END;
  case WIPHONE_KEY_F1:     return WIPHONE_KEY_MASK_F1;
  case WIPHONE_KEY_F2:     return WIPHONE_KEY_MASK_F2;
  case WIPHONE_KEY_F3:     return WIPHONE_KEY_MASK_F3;
  case WIPHONE_KEY_F4:     return WIPHONE_KEY_MASK_F4;
  default: return 0;
  }
}

void IRAM_ATTR keyboardInterrupt() {
  // This function is intentionally minimal
  // (for example, adding a Serial output here produces ISR crashes)
  keypadToRead = 1;
}

// Function that is called after interrupt occurs (not within interrupt)
// Connect to keypad scanners via I2C and decode the keyboard events into buffer keypadBuff
//
// `polled` = there was no interrupt; we are looking speculatively. See KEYPAD_POLL_MS.
// It changes exactly two things: an empty FIFO must be recognised rather than decoded,
// and anything found this way is counted, because that count is the proof the INT pulse
// is losing events.
void keyboardRead(bool polled) {
  uint8_t key;
  uint32_t mask;
  uint32_t newState = 0;
  char c;
  bool firstRead = true;
  /* How long since we last looked at the chip AT ALL. This is what makes the stale-hold
   * test below trustworthy: a gap in a key's heartbeats only means the key came up if we
   * were actually watching for those heartbeats. */
  static uint32_t msPrevRead = 0;
  const uint32_t sinceLastRead = msPrevRead ? (millis() - msPrevRead) : 0xFFFFFFFFu;
  msPrevRead = millis();
  /* Keys whose RELEASE was decoded earlier in THIS batch. The bounce filter is skipped
   * for them: see the note on keyLastUpMs. A release and a press of the same key coming
   * out of one FIFO read were separated by at least the chip's own hardware debounce
   * (INPUT_FILTER_EN, 6ms+8ms) — the chip does not emit an event for a bounce at all —
   * so they are two real actions, however identical their drain timestamps look. */
  uint32_t releasedThisBatch = 0;
  keypadToRead = 0;           // claim BEFORE reading: an event arriving mid-read
                              // re-raises the flag and gets serviced next pass
  do {
    key = 0;
    sn7326_err_t err = keypad.readKey(key);
    if (err == SN7326_ERROR_BUSY) {
      log_d("i2c reset");
      keypad.reset();         // this seems to resolve rare event of complete hanging
    }
    if (err) {
      kcI2cErr++;
      // Do NOT decode the garbage byte (key==0 decodes as a CALL release) and
      // do NOT abandon whatever is still in the chip's FIFO: the chip auto-
      // clears its INT line, so a dropped event never announces itself again —
      // that was a missed tap, or a button stuck down until the next press.
      // Leave the flag set so the next loop pass retries the read.
      log_d("keypad err code=%d", err);
      keypadToRead = 1;
      break;
    }

    /* ── AN EMPTY FIFO READS BACK AS 0x00, WHICH IS ALSO A REAL EVENT ────────────────
     * 0x00 = key code 0 with the PRESSED bit clear = "CALL released". The chip gives no
     * way to tell that apart from "nothing to report", so a speculative read of an idle
     * chip decodes as a CALL release: it would arm the bounce filter against a genuine
     * CALL press ~every poll, and drive the newState<keypadState reconciliation below
     * with an empty mask, wiping held keys.
     *
     * The disambiguation: a release of a key that is not down is meaningless anyway. So
     * on a POLLED read, a LEADING 0x00 with CALL not held is treated as an empty FIFO.
     * (Leading only — after a byte with MORE set the chip is asserting there is another
     * event, so a 0x00 there is a real CALL release and is decoded normally.)
     *
     * ⚠ This also fixes the game path, which has polled at 150 ms since the emulator
     * landed and has therefore been decoding a phantom CALL release six times a second
     * the whole time. Harmless there only by luck — CALL is not one of the eight keys
     * readPad() maps. */
    if (polled && firstRead && key == 0 &&
        !((keypadState | uiKeyDown) & WIPHONE_KEY_MASK_CALL)) {
      kcEmptyPolls++;
      /* ⚠ TRACED BEFORE IT IS THROWN AWAY. 0x00 is ambiguous — an empty FIFO and a CALL
       * release read identically — and on hardware EVERY key's release came back as 0x00,
       * so this discard is either harmless or it is eating real releases. The trace has to
       * see the bytes this branch drops or it cannot tell those two apart. */
      keyTrace(key, true, false, true);
      return;                 // nothing here: no decode, and nothing to reconcile
    }
    if (polled && firstRead) {
      /* Reached only when a poll found a REAL event — i.e. one the interrupt never
       * announced, because if it had, keypadToRead would have been set and this read
       * would not have been a poll. This counter IS the evidence for the INT-pulse
       * theory: if it stays at 0 through a session of heavy menu use, the theory is
       * wrong and the missed presses are coming from somewhere else. */
      kcPollDrained++;
    }
    firstRead = false;

    // Decode lower 6 bits for character
    switch (key & B111111) {
#ifndef WIPHONE_KEYBOARD
    // 16-key stock keyboard
    case B001011:
      mask = WIPHONE_KEY_MASK_0;
      break;
    case B000000:
      mask = WIPHONE_KEY_MASK_1;
      break;
    case B001000:
      mask = WIPHONE_KEY_MASK_2;
      break;
    case B010000:
      mask = WIPHONE_KEY_MASK_3;
      break;
    case B000001:
      mask = WIPHONE_KEY_MASK_4;
      break;
    case B001001:
      mask = WIPHONE_KEY_MASK_5;
      break;
    case B010001:
      mask = WIPHONE_KEY_MASK_6;
      break;
    case B000010:
      mask = WIPHONE_KEY_MASK_7;
      break;
    case B001010:
      mask = WIPHONE_KEY_MASK_8;
      break;
    case B010010:
      mask = WIPHONE_KEY_MASK_9;
      break;
    case B000011:
      mask = WIPHONE_KEY_MASK_ASTERISK;
      break;
    case B010011:
      mask = WIPHONE_KEY_MASK_HASH;
      break;
    case B011000:
      mask = WIPHONE_KEY_MASK_UP;
      break;
    case B011001:
      mask = WIPHONE_KEY_MASK_BACK;
      break;
    case B011010:
      mask = WIPHONE_KEY_MASK_OK;
      break;
    case B011011:
      mask = WIPHONE_KEY_MASK_DOWN;
      break;
#else
    // 25-key WiPhone keyboard layout
    case B100001:
      mask = WIPHONE_KEY_MASK_0;
      break;
    case B1000:
      mask = WIPHONE_KEY_MASK_1;
      break;
    case B1001:
      mask = WIPHONE_KEY_MASK_2;
      break;
    case B1010:
      mask = WIPHONE_KEY_MASK_3;
      break;
    case B10000:
      mask = WIPHONE_KEY_MASK_4;
      break;
    case B10001:
      mask = WIPHONE_KEY_MASK_5;
      break;
    case B10010:
      mask = WIPHONE_KEY_MASK_6;
      break;
    case B11000:
      mask = WIPHONE_KEY_MASK_7;
      break;
    case B11001:
      mask = WIPHONE_KEY_MASK_8;
      break;
    case B11010:
      mask = WIPHONE_KEY_MASK_9;
      break;
    case B100000:
      mask = WIPHONE_KEY_MASK_ASTERISK;
      break;
    case B100010:
      mask = WIPHONE_KEY_MASK_HASH;
      break;
    case B10:
      mask = WIPHONE_KEY_MASK_UP;
      break;
    case B100100:
      mask = WIPHONE_KEY_MASK_BACK;
      break;
    case B10100:
      mask = WIPHONE_KEY_MASK_OK;
      break;
    case B1:
      mask = WIPHONE_KEY_MASK_DOWN;
      break;

    case B1100:
      mask = WIPHONE_KEY_MASK_LEFT;
      break;
    case B11100:
      mask = WIPHONE_KEY_MASK_RIFHT;
      break;
    case B100:
      mask = WIPHONE_KEY_MASK_SELECT;
      break;
    case B0:
      mask = WIPHONE_KEY_MASK_CALL;
      break;
    case B11:
      mask = WIPHONE_KEY_MASK_END;
      break;
    case B1011:
      mask = WIPHONE_KEY_MASK_F1;
      break;
    case B10011:
      mask = WIPHONE_KEY_MASK_F2;
      break;
    case B11011:
      mask = WIPHONE_KEY_MASK_F3;
      break;
    case B100011:
      mask = WIPHONE_KEY_MASK_F4;
      break;
#endif
    default:
      mask = 0;   // unknown button detected
    }
    keyTrace(key, polled, mask == 0);

    /* ── THE CHIP DOES NOT SAY WHICH KEY WAS RELEASED ────────────────────────────────
     * MEASURED on hardware 2026-08-22 with the raw trace, and this is the whole bug:
     *
     *     +0ms   0x41 P   DOWN pressed
     *     +141ms 0x00 r   ...and the release comes back as key code ZERO
     *     +1047ms 0x54 P  OK pressed
     *     +115ms 0x00 r   ...zero again
     *
     * Key code 0 is CALL in this keymap, so every release in the phone was being applied
     * to CALL: the key you actually pressed never had its bit cleared, and stayed "held"
     * — and therefore DEAF to its own next press — until the 350ms stale sweep. That is
     * the missed menu press and the missed tap while cycling letters, and it has been
     * there since the first commit.
     *
     * It also explains the DOUBLE presses: because keyLastUpMs was stamped for CALL and
     * not for the real key, the contact bounce visible in that same trace (a re-press 6ms
     * after a release) sailed straight through a bounce filter that was watching the
     * wrong key.
     *
     * ⚠ The vendor code half-knew. `newState < keypadState` below is commented "Some
     * buttons were released silently" — a workaround for this very behaviour, which
     * worked only for keypadState and never for uiKeyDown.
     *
     * A release of a key that is not down is meaningless, so attribute it to what IS
     * down. ⚠ INTERRUPT-DRIVEN READS ONLY: an empty FIFO reads back as 0x00 too, and the
     * only thing separating "nothing to report" from "something was released" is that an
     * interrupt fires only when an event really happened. A polled 0x00 is still nothing. */
    if (!polled && !(key & SN7326_PRESSED) && (key & B111111) == 0) {
      const uint32_t held = uiKeyDown | keypadState;
      /* ⚠ EXACTLY ONE KEY DOWN, OR WE DO NOT GUESS. `held & (held - 1)` clears the lowest
       * set bit, so this is the "single bit" test.
       *
       * The first version attributed the release to ALL held keys, on the reasoning that
       * the release branch could clear a multi-bit mask. It can — and that was the bug:
       * releasing ONE of two held keys marked BOTH of them up, so the key still under a
       * finger was no longer in uiKeyDown, and its next ~109 ms heartbeat arrived looking
       * like a brand-new press. That is a PHANTOM KEYPRESS, and it reaches:
       *   - the Select+Back sleep chord — lift one finger a moment before the other and
       *     the other key fires; if the chord had not yet reached SLEEP_CHORD_MS the
       *     screen is still awake and a spurious Back navigates,
       *   - rolling two-key typing (pressing 4 before releasing 3) — duplicate character,
       *   - the Game Boy, where releasing A while holding RIGHT drops RIGHT out of
       *     keypadState for ~109 ms (app_gbc.cpp reads that mask directly, and the
       *     newState<keypadState repair below is skipped in game mode).
       *
       * With two or more keys down the chip has told us something was released and not
       * what, and there is no honest way to pick. So don't: leave it decoded as CALL,
       * exactly as it always was, and let the 350 ms sweep tidy up. That is no worse than
       * the behaviour this whole fix replaced, and it is a great deal better than firing a
       * keypress nobody made. Single-key use — menus, dialling, typing — is unaffected,
       * which is where every reported symptom lived. */
      if (held && !(held & WIPHONE_KEY_MASK_CALL) && (held & (held - 1)) == 0) {
        mask = held;
        kcReleaseFixed++;
      } else if (held & (held - 1)) {
        kcRelAmbiguous++;       // 2+ keys down: deliberately not guessed
      }
    }

    // Decode "pressed/released" bit
    if (key & SN7326_PRESSED) {
      newState |= mask;         // reported (still) pressed — count it even if already known
      uint32_t sinceSeen = 0;
      if (mask) {
        uint8_t b = __builtin_ctz(mask);
        sinceSeen = millis() - keyLastSeenMs[b];         // BEFORE the heartbeat is stamped
        keyLastSeenMs[b] = millis();                     // heartbeat for the stale sweep
      }
      // GAME edge: keypadState and the emulator's sticky latch.
      if (mask && !(keypadState & mask)) {
        keypadState |= mask;
        gGbcKeyLatch |= mask;   // remember the press for the emulator's next poll
        //Serial.print(c); Serial.println(" pressed");
      }

      /* ── UI edge: uiKeyDown ALONE ───────────────────────────────────────────────────
       * ⚠ This USED TO BE NESTED INSIDE the keypadState edge above, which quietly made
       * keypadState a second veto over every UI keypress — and the comment on uiKeyDown
       * ("UI key events are strictly edge-triggered off THIS mask") was therefore false.
       *
       * That nesting is the other half of the missed-tap bug. Lose one release and
       * keypadState keeps the bit; the stale sweep clears uiKeyDown but LEFT KEYPADSTATE
       * SET in the UI, so the next press never even reached the UI code — the key stayed
       * deaf until some release event for it happened to arrive. In practice that means
       * the press after a lost release is ALWAYS swallowed and the one after it works,
       * which is exactly "it misses a tap and I have to tap again".
       *
       * Held-key re-reports are still suppressed, by uiKeyDown, which is what they were
       * always meant to be suppressed by. The sweep now clears both masks together. */
      {
        /* ── THE HOLD THAT ENDED WITHOUT US SEEING IT ──────────────────────────────────
         * A held key re-reports every 40 ms, so a 'pressed' report arriving more than
         * KEY_HOLD_GAP_MS after this key was last seen CANNOT be the continuation of a
         * hold: the heartbeats stopped, which means the key came up and went down again
         * and its release never reached us. Without this the press is vetoed by
         * `uiKeyDown` and the key stays deaf until the 350 ms sweep — one whole tap
         * thrown away. MEASURED on hardware 2026-08-22: gap=20 in a few minutes of menu
         * use, which is precisely the "menu still skips some inputs" {user} was reporting
         * while typing had already come right.
         *
         * ⚠ THIS WAS FIRST SHIPPED AS A COUNTER ONLY, and the caution was real: if the
         * main loop stalls, heartbeats pile up in the chip and a stale gap is OUR fault,
         * not the key's — acting on it would resurrect the menu auto-repeat that
         * uiKeyDown exists to kill. What makes it safe now is `sinceLastRead`: while
         * uiKeyDown is set the loop polls the chip every 40 ms, so if we have looked
         * recently and STILL seen no heartbeat, the key really is up. If we have not
         * looked recently, we say nothing and let the sweep handle it as before. */
        const bool watching = sinceLastRead <= 120;
        if (mask && (uiKeyDown & mask) && sinceSeen > KEY_HOLD_GAP_MS && watching) {
          /* ⚠ COUNTS ONLY — IT USED TO ACT, AND ACTING WAS A BUG. {user}, on hardware:
           * "push OK then push the star key to unlock, it immediately thinks I'm trying to
           * type the star key in the dialer". The trace showed why:
           *
           *     +567ms 0x60 P   * pressed   <- unlocks, correctly swallowed
           *     +109ms 0x60 P   * pressed AGAIN, with no release in between
           *
           * That second report is the chip re-reporting a HELD key, and the interval is
           * ~109 ms — just past the 100 ms threshold this test used. So every heartbeat
           * of a slightly-long press was being promoted into a second keypress.
           *
           * The rescue existed to save a press whose release had been lost. Since releases
           * are attributed correctly (see the code-0 note above) they are not being lost —
           * `swept` sits at 0 — so this has nothing left to save and one clear way to do
           * harm. The 350 ms sweep remains the backstop, and it is safely clear of the
           * heartbeat. Do not re-arm this without re-measuring the heartbeat interval;
           * anything below it turns a hold into a burst of keypresses. */
          kcGapRescued++;
        }

        const bool alreadyDown = mask && (uiKeyDown & mask);
        const bool sameBatchRetap = mask && (releasedThisBatch & mask);
        const bool looksLikeBounce = mask &&
                                     (millis() - keyLastUpMs[__builtin_ctz(mask)] < KEY_BOUNCE_MS);
        bool uiSuppress = mask == 0 || alreadyDown || looksLikeBounce;
        if (sameBatchRetap && !alreadyDown) {
          /* ⚠ OBSERVED ONLY — it does NOT exempt the press from the bounce filter, and
           * that is a deliberate reversal. The exemption was written for a release and a
           * re-press draining together after being stranded in the FIFO; the raw trace
           * then showed nothing is ever stranded (`drained` stayed 0 through whole
           * sessions) and this counter never moved either. What the trace DID show is a
           * genuine contact bounce — a re-press 6ms after a release — which the exemption
           * would have waved straight through as a second menu step. Keeping the count is
           * useful; keeping the exemption would reintroduce the double press. */
          kcBatchRescued++;
        }
        if (looksLikeBounce) {
          kcBounceKilled++;     // real bounce, or a stale hold re-report. Should be rare.
        }
        if (alreadyDown && sinceSeen > KEY_HOLD_GAP_MS) {
          /* Same shape as the rescue above, but we had NOT looked recently enough to
           * trust it, so the press is still being vetoed. If this climbs while `rescued2`
           * stays flat, the poll is not keeping up and KEYPAD_POLL_MS wants lowering. */
          kcGapMissed++;
        }

        /* A press with somewhere to go and nowhere to be put. The buffer holds 13 and the
         * loop drains it every pass, so this should never move; if it does, the loop is
         * blocking long enough to matter and THAT is the finding.
         *
         * It used to be dropped in silence AND latched into uiKeyDown, so nothing ever
         * retried it — the press was simply gone. Leaving the key unlatched instead means
         * the chip's own 40ms re-report delivers it as soon as there is room. */
        const bool noRoom = !uiSuppress && keypadBuff.full();
        if (noRoom) {
          kcBuffFull++;
        }
        if (mask && !noRoom) {
          uiKeyDown |= mask;
        }

        if (!keypadBuff.full() && !uiSuppress) {
          switch (mask) {
          case WIPHONE_KEY_MASK_0:
            c = '0';
            break;
          case WIPHONE_KEY_MASK_1:
            c = '1';
            break;
          case WIPHONE_KEY_MASK_2:
            c = '2';
            break;
          case WIPHONE_KEY_MASK_3:
            c = '3';
            break;
          case WIPHONE_KEY_MASK_4:
            c = '4';
            break;
          case WIPHONE_KEY_MASK_5:
            c = '5';
            break;
          case WIPHONE_KEY_MASK_6:
            c = '6';
            break;
          case WIPHONE_KEY_MASK_7:
            c = '7';
            break;
          case WIPHONE_KEY_MASK_8:
            c = '8';
            break;
          case WIPHONE_KEY_MASK_9:
            c = '9';
            break;
          case WIPHONE_KEY_MASK_ASTERISK:
            c = '*';
            break;
          case WIPHONE_KEY_MASK_HASH:
            c = '#';
            break;
          case WIPHONE_KEY_MASK_UP:
            c = WIPHONE_KEY_UP;
            break;
          case WIPHONE_KEY_MASK_BACK:
            c = WIPHONE_KEY_BACK;
            break;
          case WIPHONE_KEY_MASK_OK:
            c = WIPHONE_KEY_OK;
            break;
          case WIPHONE_KEY_MASK_DOWN:
            c = WIPHONE_KEY_DOWN;
            break;

          case WIPHONE_KEY_MASK_LEFT:
            c = WIPHONE_KEY_LEFT;
            break;
          case WIPHONE_KEY_MASK_RIFHT:
            c = WIPHONE_KEY_RIGHT;
            break;
          case WIPHONE_KEY_MASK_SELECT:
            c = WIPHONE_KEY_SELECT;
            break;
          case WIPHONE_KEY_MASK_CALL:
            c = WIPHONE_KEY_CALL;
            break;
          case WIPHONE_KEY_MASK_END:
            c = WIPHONE_KEY_END;
            break;
          case WIPHONE_KEY_MASK_F1:
            c = WIPHONE_KEY_F1;
            break;
          case WIPHONE_KEY_MASK_F2:
            c = WIPHONE_KEY_F2;
            break;
          case WIPHONE_KEY_MASK_F3:
            c = WIPHONE_KEY_F3;
            break;
          case WIPHONE_KEY_MASK_F4:
            c = WIPHONE_KEY_F4;
            break;

          default:
            c = 0;
          }
          if (c) {
            keypadBuff.put(c);
          }
        }
      }
    } else {
      if (mask) {   // unconditional: the state bit may have been cleared mid-hold
        keypadState &= ~mask;
        uiKeyDown &= ~mask;                            // re-arm the UI edge
        // Loop, not ctz: an attributed code-0 release above can carry several held keys.
        for (uint32_t m = mask; m; m &= m - 1) {
          keyLastUpMs[__builtin_ctz(m)] = millis();    // for the UI debounce window
        }
        releasedThisBatch |= mask;                     // exempt a re-press in THIS batch
        //Serial.print(c); Serial.println(" released");
      }
    }
  } while (key & SN7326_MORE);       // decode "more" bit

  // Some buttons were "released" silently.
  // NOT during gameplay: this batch only lists keys with NEW events, so pressing
  // (or releasing) button A while HOLDING right produced a batch without "right"
  // and wiped the held key -> held-direction games went run/stop/run. Releases
  // do arrive as real events above, so the held mask stays correct without this.
  if (!gGbcActive && newState < keypadState) {
    keypadState = newState;
  }

  // Something real came out of the chip. Keeps the poll alive for a moment afterwards
  // (KEYPAD_POLL_TAIL_MS) so a stuck event has something to find it.
  if (!firstRead) {
    msLastKeypadActivity = millis();
  }
}

#ifdef USE_VIRTUAL_KEYBOARD
void keyboardUdpRead() {
  if (udpKeypad && udpParsePacketSafe(*udpKeypad) > 0) {
    char buff[1000];
    int cb = udpKeypad->read(buff, sizeof(buff) - 1);
    buff[cb] = 0;
    //log_d("Keypad received: %s", buff);
    for (int i = 0; i < cb && !keypadBuff.full(); i++) {
      if (buff[i] == 10 || buff[i] == 13 || !buff[i]) {
        continue;
      }
      keypadBuff.put(buff[i]);
    }
  }
}
#endif

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  POWER/END BUTTON INTERRUPT  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

// Interrup routines to check for the power button presses, which is not connected to keypad scanner, but via a dedicated GPIO extender pin

bool powerButtonPressed = false;
bool poweringOff = false;
volatile bool gpioExtenderEvent = false;

void IRAM_ATTR gpioExtenderInterrupt() {
  // This function is intentionally minimal
  gpioExtenderEvent = true;
}

// Function that is called after interrupt occurs (not within interrupt)
bool gpioExtenderServiceInterrupt() {
  gpioExtenderEvent = false;
  /* 🛑 A READ THAT FAILED IS NOT A PRESS. digitalRead() answers LOW for a pin it could not
   * read (SparkFunSX1509::readPinChecked), and the power button is active-LOW, so one I2C
   * hiccup here used to latch powerButtonPressed and, 2.5 s later with no release seen in
   * between, POWER THE PHONE OFF. Caught on the bench 2026-09-19: phone 1 shut itself down
   * three seconds into a Game Boy launch, on USB, with nobody near the button - the
   * emulator's two tasks starve the main loop, and the extender read timed out under them.
   * On a failed read the state is kept and the event is dropped; the chip re-raises the
   * line on the next real edge. */
  byte level = HIGH;
  if (!gpioExtender.readPinChecked(POWER_CHECK & ~EXTENDER_FLAG, &level)) {
    log_e("POWER: extender read failed, button state kept (%d)", (int)powerButtonPressed);
    return false;
  }
  bool powerButton = level == LOW;
  //log_d("powerButton = %d", powerButton);
  if (powerButton != powerButtonPressed) {
    powerButtonPressed = powerButton;
    if (powerButton) {
      keypadBuff.put(WIPHONE_KEY_END);
    }
    return true;
  }
  return false;
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  SETUP  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

void setup() {
  // Initialize serial
  const uart_config_t uart_config = {
    .baud_rate = SERIAL_BAUD,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
  };

  int RX_BUF_SIZE =  1024;

  uart_param_config(UART_NUM_0, &uart_config);
  uart_driver_install(UART_NUM_0, RX_BUF_SIZE * 2, 0, 0, NULL, 0);

  for(int i=0; i<17; i=i+8) {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }

  log_i("\r\nChip id: %X %d %d", chipId, ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));
  log_i("Firmware version: %s", FIRMWARE_VERSION);

  /* ── THE BLUETOOTH RESERVE, GIVEN BACK (0.9.74/0.9.75) ─────────────────────────────────
   * This SDK reserves CONFIG_BT_RESERVE_DRAM (0xdb5c = 56 KB) of internal RAM for a
   * Bluetooth controller that nothing in this firmware ever starts, and the linker puts the
   * whole app ABOVE it. arduino-esp32's initArduino() gives it back to the heap before
   * setup() runs — UNLESS btInUse() says otherwise, and the strong btInUse(){return true;}
   * lives in esp32-hal-bt.c, which was linked in by one dead btStop() in Networks::disable()
   * (removed in 0.9.75). So for as long as this firmware has existed the phone ran in the
   * 16-28 KB the reserve left over — every "the internal heap is only ~16 KB" note in this
   * repo — and only the Game Boy, which releases the reserve on its first launch for its own
   * stacks and VRAM, ever saw the rest. What comes back is ~40 KB, not 56 (five regions:
   * 0x3ffae6e0-0x3ffaff10 6,192 B, 0x3ffb0000-0x3ffb6388 25,480 B, 0x3ffb8000-0x3ffb9a20
   * 6,688 B, libbtdm_app's .bss 2,412 B, and 52 B); they are added at the HEAD of the heap
   * list, so they absorb the boot-time allocations made after them and the main region's
   * ~67 KB block stays whole — that block is what a TLS handshake or any large internal
   * request now draws on. Measured idle with WiFi + SIP up: 28 -> 68 KB free, largest block
   * 25 -> 67 KB. This block is the belt to the framework's braces: release it here too if
   * the core did not, and either way write the numbers into the boot log. ⚠ No Bluetooth
   * can start in a boot after this — which has always been true of every phone that ran a
   * game, and nothing here asks for it. */
  {
    const uint32_t fBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t lBefore = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const esp_err_t rel = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    const char* how = (rel == ESP_OK) ? "released here" :
                      (rel == ESP_ERR_INVALID_STATE) ? "already released by the core (initArduino)" : "NOT released";
    log_e("BOOT: BT reserve %s (err %d): internal free %lu -> %lu, largest %lu -> %lu", how, (int)rel,
          (unsigned long)fBefore, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
          (unsigned long)lBefore, (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }

  /* The clock as the app found it: 240 MHz on PLL 480 (sdkconfig DEFAULT_CPU_FREQ 240; the
   * core's initArduino() setCpuFrequencyMhz(240) is then a no-op). Under method NEW it moves to
   * PLL 320 INSIDE the first esp_wifi_start() - wifiState.init() below - before the radio
   * exists; the gate's first pass prints that move (cpu_clock.cpp). */
  cpuClockInit();
  {
    CpuClockStatus cs;
    cpuClockStatus(&cs);
    log_e("BOOT: CPU %lu MHz on PLL %lu%s, clock method %s", (unsigned long)cs.mhz,
          (unsigned long)cs.pllMhz, cs.rated160 ? " (chip RATED 160: 240 is never used)" : "",
          cs.method == CPU_METHOD_NEW ? "new (same PLL while the radio runs)" : "old");
  }

  // Initialize I2C and wake up battery gauge first
  gauge.connect();
#if defined(WIPHONE_BOARD) || defined(WIPHONE_INTEGRATED)
  gui.state.gaugeInited = gauge.configure();
  log_d("\r\nBattery gauge: %s\n", gui.state.gaugeInited ? "OK" : "FAILED");
  delay(10);
#endif // WIPHONE_BOARD || WIPHONE_INTEGRATED

  log_v("Free memory after gauge: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Initialize GPIO extender
#ifdef WIPHONE_INTEGRATED_1_4
  if (gpioExtender.begin()) {
    log_v("extender succ");
    gui.state.extenderInited = true;
    // Input
    allPinMode(POWER_CHECK, INPUT);
    allPinMode(TF_CARD_DETECT_PIN, INPUT);
    allPinMode(BATTERY_CHARGING_STATUS_PIN, INPUT);
    // Output
    allPinMode(KEYBOARD_LED, OUTPUT);
    allPinMode(VIBRO_MOTOR_CONTROL, OUTPUT);
    allPinMode(POWER_CONTROL, OUTPUT);
    // Default state
    allDigitalWrite(POWER_CONTROL, LOW);
    allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
    allDigitalWrite(KEYBOARD_LED, HIGH);
  } else {
    log_e("extender failed");
    gui.state.extenderInited = false;
  }
#else // not WIPHONE_INTEGRATED_1_4
#ifdef WIPHONE_INTEGRATED_1_3
  {
    auto err = gpioExtender.config(
                 // Input pins
                 EXTENDER_PIN_FLAG_A2 | EXTENDER_PIN_FLAG_B1,
                 // Output HIGH
                 EXTENDER_PIN_FLAG_A0 | EXTENDER_PIN_FLAG_A2 | EXTENDER_PIN_FLAG_B0 | EXTENDER_PIN_FLAG_B1 | EXTENDER_PIN_FLAG_B7
               );
    if (err != SN7325_ERROR_OK) {
      log_d("GPIO extender error = %d", err);
    }
    gpioExtender.showState();
  }
#else // not WIPHONE_INTEGRATED_1_3
#ifdef WIPHONE_INTEGRATED_1
  {
    auto err = gpioExtender.config(
                 // Input pins
                 EXTENDER_PIN_FLAG_A2 | EXTENDER_PIN_FLAG_B1,
                 // Output HIGH
                 EXTENDER_PIN_FLAG_A0 | EXTENDER_PIN_FLAG_A1 | EXTENDER_PIN_FLAG_B0 | EXTENDER_PIN_FLAG_B1
               );
    if (err != SN7325_ERROR_OK) {
      log_d("GPIO extender error = %d", err);
    }
    gui.state.extenderInited = (err == SN7325_ERROR_OK);
    err = gpioExtender.setInterrupts(EXTENDER_PIN_FLAG_A2);     // POWER_OFF interrupt
    if (err != SN7325_ERROR_OK) {
      log_d("GPIO extender error = %d", err);
    }
    gpioExtender.showState();
  }
#endif // WIPHONE_INTEGRATED_1
#endif // WIPHONE_INTEGRATED_1_3
#endif // WIPHONE_INTEGRATED_1_4

  log_v("Free memory after integrated: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Fast test of PSRAM presence
  void* p = heap_caps_malloc(100000, MALLOC_CAP_SPIRAM);
  if (p != NULL) {
    gui.state.psramInited = true;
    freeNull((void **) &p);
  }

  log_v("Free memory after psram: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Initialize power
#if defined(POWER_CONTROL) && POWER_CONTROL >= 0
  allPinMode(POWER_CONTROL, OUTPUT);
  allDigitalWrite(POWER_CONTROL, LOW);
#endif

  log_v("Free memory after power: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

#if defined(POWER_CHECK) && POWER_CHECK >= 0
#ifdef WIPHONE_INTEGRATED_1_4
  log_d("enabling interrupt (rev1.4)");
  gpioExtender.enableInterrupt(2, FALLING);
#else // not WIPHONE_INTEGRATED_1_4
#ifdef WIPHONE_INTEGRATED_1_3
  log_d("enabling interrupt input (rev1.3)");
  allPinMode(POWER_CHECK, INPUT_PULLUP);
#endif // WIPHONE_INTEGRATED_1_3
#endif // WIPHONE_INTEGRATED_1_4
#endif // defined(POWER_CHECK) && POWER_CHECK >= 0

  Random.feed(micros());      // TODO: maybe feed microphone data to Random.feed()

  // Mounter internal filesystem
  if (SPIFFS.begin()) {
    log_d("SPI filesystem mounted");
    // Install/refresh the Meshtastic notification "pop" sound (embedded in
    // firmware so no SPIFFS re-flash is needed). Rewritten on each boot so
    // firmware updates to the sample always take effect.
    {
      File pf = SPIFFS.open("/pop.pcm", "w");
      if (pf) {
        pf.write(pop_pcm, sizeof(pop_pcm));
        pf.close();
        log_d("Installed /pop.pcm (%u bytes)", (unsigned)sizeof(pop_pcm));
      }
    }
  } else {
    log_d("SPI filesystem mount FAILED");
  }

  log_v("Free memory after internal fs: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Initialize GUI
  log_d("Initializing screen");
  gui.init(lcdLedOnOff);
  gui.redrawScreen(false, false, true);   // only screen

  log_v("Free memory after gui: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

#if LCD_LED_PIN >= 0
  // Turn on backlight
  log_d("LCD_LED_PIN = %d", LCD_LED_PIN);
  allPinMode(LCD_LED_PIN, OUTPUT);
#if GPIO_EXTENDER == 1509
  gpioExtender.ledDriverInit(LCD_LED_PIN ^ EXTENDER_FLAG);
#else
  allPinMode(LCD_LED_PIN, OUTPUT);
#endif // GPIO_EXTENDER == 1509
  gui.toggleScreen();
#endif

  log_v("Free memory after lcd led: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Mount SD card and SPIFFS (AFTER the screen & SPI initialization)
#if defined(WIPHONE_BOARD) || defined(WIPHONE_INTEGRATED)

  // Init SD card detection pin
  allPinMode(TF_CARD_DETECT_PIN, INPUT);

  // Initilize hardware serial:
  gui.state.battVoltage = gauge.readVoltage();
  /* SOC comes from the VOLTAGE, through phone 1's measured curve — not from the CW2015's
   * `soc` register, whose battery profile is never programmed and which reads 0 % with 1.3 h
   * of runtime left (battery_curve.h). The chip's number is still logged as `soc=` in the
   * health line so the two can be compared on any future run. */
  gui.state.battSoc = gui.state.battVoltage > 0.0f
                      ? (float)batterySocFromVoltage(gui.state.battVoltage)
                      : gauge.readSocPrecise();
  log_d("Voltage = %.2f", gui.state.battVoltage);
  log_d("SOC = %.1f (from voltage)", gui.state.battSoc);
#endif // WIPHONE_BOARD || WIPHONE_INTEGRATED

  log_v("Free memory after sd spiffs: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // If voltage is extremely low, power off immediately
  if (gui.state.battVoltage < 3.1) {
    powerOff();
    gui.processEvent(millis(), POWER_OFF_EVENT);
  }

  // INITIALIZE OTHER I2C DEVICES

  // Battery gauge
#if defined(WIPHONE_BOARD) || defined(WIPHONE_INTEGRATED)
  gauge.showVersion();
#endif // WIPHONE_BOARD

  // Mount SD card

  if (SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_FREQUENCY)) {
    log_d("Card mounted");
    /* The home of the user's API keys (gemini.txt for the AI terminal). Made at boot so the
     * Files app can upload into it before any app needs it: Files > API Keys > [ Upload into
     * this folder ]. Quiet when it already exists. A key is only ever read from the card, never
     * compiled in - this repo is public. */
    SD.mkdir("/API Keys");
  } /*else {
    log_d("Card mount FAILED");
  }*/

  /* ⚠ THE WALLPAPER IS LOADED HERE, AND NOT ONLY IN gui.init(), AND THE ORDER ABOVE IS WHY.
   * gui.init() runs ~50 lines earlier — before the line above — so the SD.exists() inside it
   * asks an UNMOUNTED filesystem and always gets false. The Photos app writes the chosen
   * wallpaper to /background.jpg on the SD card, so until this second call existed, "Set as
   * wallpaper" wrote a perfectly good file that nothing ever read: the phone fell back to the
   * compiled-in default on every boot and the user saw no change and no error ({user},
   * 2026-08-25). Moving SD.begin() earlier is NOT the fix — it shares SPI with the screen and
   * the comment above says that order is deliberate. Ask the phone with `wallpaper`. */
  gui.loadWallpaper();

  /* ⚠ SEED cardPresent HERE, OR EVERY CARD-GATED PATH LIES FOR THE FIRST MINUTE.
   * It is declared `false` in GUI.h and was assigned in exactly one place: the
   * once-a-minute battery tick. So for up to BATTERY_CHECK_PERIOD_MS after every boot
   * the phone believed it had no card. healthDump() bails on `!cardPresent`, which
   * means `health` answered "nothing to read - no card, or no /health.log yet" WITH A
   * PERFECTLY GOOD CARD SEATED — measured 2026-08-23, when the file it was denying held
   * 926 samples and returned in full 80 s later. That is exactly the window in which
   * someone plugs a phone into the cable and asks it what happened.
   * The pin is configured by the allPinMode() above, and gpioExtender.begin() ran far
   * earlier in setup(), so it is readable by the time we get here. */
#if TF_CARD_DETECT_PIN >= 0
  gui.state.cardPresent = allDigitalRead(TF_CARD_DETECT_PIN) == LOW ? true : false;
#endif
  /* Seed battCharged the same way, for the same reason, and re-pick the boot battSoc with the
   * rule the tick uses: the read above set it from the curve with the charger state unknown,
   * and on the charger that is the inflated number (66 % vs the chip's 25 %, on the first
   * 0.9.56 boot). Until the first tick nothing else would correct it. */
  gui.state.battCharged = allDigitalRead(BATTERY_CHARGING_STATUS_PIN) == LOW ? true : false;
  if (gui.state.battCharged) {
    const float bootSoc = gauge.readSocPrecise();
    if (bootSoc > 0.0f) {
      gui.state.battSoc = bootSoc;
    }
  }


  // Initialize keypad
  {
    //keypad.connect();     // sets I2C speed to 400 kHz (as per datasheet)
    sn7326_err_t err = keypad.config();
    if (err != SN7326_ERROR_OK) {
      log_d("keypad error = %d", err);
    }
    gui.state.scannerInited = (err == SN7326_ERROR_OK);
  }

  log_v("Free memory after keypad: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Initialize RMT peripheral to enable the amplifier
#ifdef WIPHONE_INTEGRATED_1_4
  rmtTxInit(AMPLIFIER_SHUTDOWN, false);
#else
#ifdef WIPHONE_INTEGRATED_1_3
  // ?
#endif // WIPHONE_INTEGRATED_1_3
#endif // WIPHONE_INTEGRATED_1_4

  // Initialize audio systems (hardware codec and I2S peripheral)
#ifdef I2S_MCLK_GPIO0
  {
    // Use GPIO_0 for providing MCLK to the audio codec IC
    //SET_PERI_REG_BITS(PIN_CTRL, CLK_OUT1, 0, CLK_OUT1_S);       // esp-adf
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0_CLK_OUT1); // esp-adf
    REG_SET_FIELD(PIN_CTRL, CLK_OUT1, 0);                         // Source: https://esp32.com/viewtopic.php?t=1521
  }
#endif // I2S_MCLK_GPIO0


  log_v("Free memory after config files: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // GPIOs

  /* ⚠ allPinMode, NOT pinMode: BATTERY_CHARGING_STATUS_PIN is EXTENDER_PIN(0) == 64 and the
   * ESP32 has GPIO 0-39, so plain pinMode() configured a pin that does not exist and this
   * line did nothing at all. Benign only because the real configuration happens correctly
   * via allPinMode() in the extender block earlier in setup() — but a line that looks like
   * it sets up the charge-status input, and does not, is exactly how the sibling bug in the
   * battery tick survived so long (see the note there). BATTERY_PPR_PIN is a genuine GPIO
   * (37) and is fine either way; it goes through the same accessor for consistency. */
#ifdef WIPHONE_INTEGRATED
  allPinMode(BATTERY_CHARGING_STATUS_PIN, INPUT);
  allPinMode(BATTERY_PPR_PIN, INPUT);
#endif // WIPHONE_INTEGRATED
#ifdef WIPHONE_BOARD
  allPinMode(BATTERY_CHARGING_STATUS_PIN, INPUT);
  //pinMode(USB_POWER_DETECT_PIN, INPUT);
#endif // WIPHONE_BOARD

#if defined(KEYBOARD_RESET_PIN) && KEYBOARD_RESET_PIN > 0
  pinMode(KEYBOARD_RESET_PIN, OUTPUT);         // keypad RST
  digitalWrite(KEYBOARD_RESET_PIN, HIGH);      // no RST
  delay(1);                    // 1 ms
  digitalWrite(KEYBOARD_RESET_PIN, LOW);       // RST
  delay(1);                    // 1 ms
  digitalWrite(KEYBOARD_RESET_PIN, HIGH);      // no RST
#endif

  pinMode(KEYBOARD_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(KEYBOARD_INTERRUPT_PIN), keyboardInterrupt, FALLING);
  pinMode(GPIO_EXTENDER_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(GPIO_EXTENDER_INTERRUPT_PIN), gpioExtenderInterrupt, FALLING);


#if defined(MOTOR_DRIVER) && MOTOR_DRIVER == 8833
  motorDriver.attachMotorA(AIN1, AIN2);
  motorDriver.attachMotorB(BIN1, BIN2);
  pinMode(MotorEN , OUTPUT);
  digitalWrite(MotorEN , LOW);
#endif

  //esp_pm_config_esp32_t conf = { RTC_CPU_FREQ_240M, 240, RTC_CPU_FREQ_80M, 10, true };
//  esp_pm_config_esp32_t conf = { RTC_CPU_FREQ_240M, RTC_CPU_FREQ_80M, true };
//  esp_err_t err;
//  err = esp_pm_configure((const void*) &conf);
//  log_d("Power management: %d", (int) err);

  /* The Almanac release's two device-wide settings, beside the GPS switch's read below and from
   * the same NVS namespace: metric/US (every app's distances and heights) and the legal-light
   * rule. One home, prefs_almanac.h, so every screen reads the same value. Outside USER_SERIAL:
   * they have nothing to do with the port. */
  almanacPrefsLoad();

#ifdef USER_SERIAL
  //allDigitalWrite(EXTENDER_PIN_B0, HIGH);     // TODO: why do we do this?
  {
    Preferences p;
    p.begin("wpmesh", true);
    gGpsNmea = p.getBool("gpsen", false);
    gGpsBaud = p.getUInt("gpsbaud", GPS_SERIAL_BAUD_DEFAULT);
    p.end();
  }
  /* 🛑 ALWAYS OPEN AT USER_SERIAL_BAUD, EVEN WHEN THE GPS OWNS THIS PORT, AND RETUNE
   * FROM loop() INSTEAD. This used to open straight at gGpsBaud, and the comment here
   * used to say that retuning afterwards "would work too" but dirtied the reader's
   * counters on every boot. It does not merely dirty them — opening at 115200 here
   * CRASHED THE PHONE, and this is the fix for it.
   *
   * 🔑 MEASURED, 2026-08-25, from three reproduction boots with the plate fitted:
   *      Guru Meditation Error: Core 1 panic'ed (Interrupt wdt timeout on CPU1)
   *      setup() -> HardwareSerial::begin -> uartBegin -> uartAttachRx
   *             -> uartEnableInterrupt -> esp_intr_alloc -> vTaskExitCritical
   *             -> _xt_lowint1 -> _uart_isr
   * The woods plate's GPS starts talking the moment it has POWER; it does not wait for
   * us. So by the time begin() installs the RX interrupt handler the line is already
   * saturated at 115200, the ISR fires inside the attach's own critical section, and
   * the CPU1 interrupt watchdog times out. It then reboots into setup() and does it
   * again — a boot loop that presents as a phone with a dark screen and dead buttons.
   *
   * ⚠ THE RATE IS THE WHOLE POINT, not the baud being "wrong": at USER_SERIAL_BAUD the
   * same live line still interrupts us, but ~12x less often (9600/10 vs 115200/10 bytes
   * a second), and that attach survives — it is what every gpsen=false boot has always
   * done. The retune afterwards calls updateBaudRate(), which does NOT re-enter
   * esp_intr_alloc, so it cannot reproduce the fault. That asymmetry is exactly why
   * toggling `gps on` by hand always worked while booting with it on did not.
   *
   * The counters still start clean: gGpsReader.reset() runs at the retune, below. */
  userSerial.begin(USER_SERIAL_BAUD, USER_SERIAL_CONFIG,
                   USER_SERIAL_RX, USER_SERIAL_TX);
  if (gGpsNmea) {
    gGpsBaudPending = true;      // loop() finishes the job, once, on its first pass
  }
#endif


  /* ── WHY DID IT RESTART? ──────────────────────────────────────────────────────────
   * The chip records this across a reboot and it is the difference between guessing and
   * knowing. The ones that matter here:
   *   1  POWERON    the switch, or the battery was pulled
   *   3  SW         esp_restart() -- our own reboot menu item
   *   4  PANIC      a crash: null deref, abort(), assert
   *   5  INT_WDT    an interrupt watchdog -- something blocked with interrupts off
   *   6  TASK_WDT   a task watchdog -- the main loop stopped feeding it
   *   7  WDT        other watchdog
   *   9  BROWNOUT   THE SUPPLY SAGGED. Not software at all -- a tired battery under a
   *                 WiFi transmit peak does this, and it looks exactly like a crash.
   *  10  RTC/SDIO
   * Printed with log_e so it survives into the field log, where log_d does not. */
  log_e("BOOT: reset_reason=%d heap=%u psram=%u",
        (int)esp_reset_reason(), ESP.getFreeHeap(), ESP.getFreePsram());
  /* ⚠ THE BUILD STAMP IS THE POINT OF THIS LINE, not the heap numbers.
   * /health.log records only `up=` minutes — no wall clock, and until now nothing saying WHICH
   * FIRMWARE wrote a sample. On 2026-08-24 that gap made a yes/no question unanswerable from
   * four hours of data: `chg=` had just been fixed to read the right chip, the log contained a
   * long run of chg=1 that tracked discharging perfectly, and there was NO WAY TO TELL whether
   * those samples came from before the fix (when the flag was reading GPIO 32, a UART TX line
   * that idles high) or after it. Both readings fit and neither could be ruled out.
   * __DATE__/__TIME__ is the compiler's stamp, so every build is distinguishable and every run
   * of samples can be attributed to the firmware that produced it. */
  snprintf(bootLine, sizeof(bootLine), "BOOT reset_reason=%d heap=%u psram=%u build=" __DATE__ " " __TIME__,
           (int)esp_reset_reason(), ESP.getFreeHeap(), ESP.getFreePsram());
  printf("\r\nBooting...\r\n");

  uint8_t mac[6];
  wifiState.getMac(mac);

  nvs_stats_t nvs_stats;
  nvs_get_stats(NULL, &nvs_stats);
  log_d("NVS stats: UsedEntries = %d, FreeEntries = %d, AllEntries = %d\n",
        nvs_stats.used_entries, nvs_stats.free_entries, nvs_stats.total_entries);

  gui.loadSettings();
  gui.reloadMessages();

  log_v("Free memory after reload messages: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  if (gui.state.dimming || gui.state.sleeping) {
    uint32_t now = millis();
    if (gui.state.doDimming()) {
      gui.state.scheduleEvent(SCREEN_DIM_EVENT, now + gui.state.dimAfterMs*2);
    }
    if (gui.state.doSleeping()) {
      gui.state.scheduleEvent(SCREEN_SLEEP_EVENT, now + gui.state.sleepAfterMs*2);
    }
  }

  log_v("Free memory after gui sleeping: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Set thread priorities (NOTE: in FreeRTOS, tasks with higher priorities will always be exectuted first, unless they have nothing to do)
  uint32_t mainThreadPrio = ESP_TASK_TCPIP_PRIO >> 1;
  if (tskIDLE_PRIORITY >= mainThreadPrio) {
    mainThreadPrio = tskIDLE_PRIORITY + 1;
  }
  log_i("lwIP thread priority: %d", ESP_TASK_TCPIP_PRIO);
  log_i("Main loop thread priority: %d", mainThreadPrio);
  log_i("Idle task priority: %d", tskIDLE_PRIORITY);
  log_i("Tick period: %d ms", portTICK_PERIOD_MS);
  vTaskPrioritySet(NULL, mainThreadPrio);

  log_v("Free memory after vTaskPrioritySet: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));


  wifiState.init();

  log_v("Free memory after wifi init: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  if (!wifiState.hasPreferredSsid()) {
    wifiState.disable();  // if we don't have a saved SSID to connect to, turn off WiFi to save power
  }

  int counter = 0;
  wifiState.loadPreferred();

  /* 🔑 THE PERSISTED WiFi SWITCH IS APPLIED HERE, and this is the ONLY place it can be:
   * Networks::init() has already done WiFi.mode(WIFI_STA), so reading the pref any later
   * means the radio comes up and is switched off a moment afterwards. The 2026-09-01 audit
   * found that boot never applied a stored OFF at all — any naive "just persist the toggle"
   * would have inherited exactly that bug.
   *
   * Loud at log_e on purpose: a radio that stays off across a power cycle must announce
   * itself somewhere a person will find it. The menu row says so too. */
  if (wifiState.loadRadioOff()) {
    wifiState.disable();
    log_e("WIFI: radio is OFF (your setting, kept across restarts) - Settings > WiFi to turn it on");
  } else if (!wifiState.userDisabled()) {
    wifiState.connectToPreferred();
    while (!wifiState.isConnected() && ++counter < 10) {
      log_v("Waiting for wifi: %d %d", counter, ESP.getFreeHeap());
      wifiState.connectToPreferred();
      delay(500);
    }
  } else {
    /* ⚠ THE PREFERRED NETWORK IS DISCONNECTED (its INI `disabled=true`: Disconnect, then a
     * reboot) — or there is none, which disable() above already handled. Networks::init() has
     * started the station, and until 0.9.79 nothing stopped it: no join, no retry, no scan (all
     * stand down for userDisabled), just a station up and idle for the whole boot. The restore
     * helper says OFF here, and logs that it did. */
    wifiRestoreStation("boot: no saved network, or it is disconnected");
  }

#if OTA_TRANSPORT_AVAILABLE
  /* ⚠ updateExists() is the NETWORK call — it is what opens the TLS connection. It must stay
   * to the RIGHT of the cheap local predicates, because && short-circuits left to right. It
   * used to sit to their LEFT, so the handshake ran on every boot whatever the settings said,
   * and autoUpdateEnabled() defaults to true so the gate would not have saved it either. */
  if (!ota.hasJustUpdated() && ota.userRequestedUpdate()) {
    gui.drawOtaUpdate();
    ota.doUpdate();
  } else if (!ota.hasJustUpdated() && (ota.autoUpdateEnabled() || ota.userRequestedUpdate()) && ota.updateExists()) {
    gui.drawOtaUpdate();
    ota.doUpdate();
  }

  ota.setUserRequestedUpdate(false);
#endif // OTA_TRANSPORT_AVAILABLE

  static Audio audio_local(true, I2S_BCK_PIN, I2S_WS_PIN, I2S_MOSI_PIN, I2S_MISO_PIN);
  audio = &audio_local;
  audio->setMuted(gui.state.audioMuted);   // Settings > Mute all sounds, read by loadSettings()
  gui.state.codecInited = !audio->error();

  // Load phone configs
  bool tzIniHad = false;          // configs.ini [time] zone: the TYPED offset (tz_service.h)
  float tzIniHours = 0;
  {
    CriticalFile ini(Storage::ConfigsFile);
    if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
      if (ini[0].hasKey("v") && !strcmp(ini[0]["v"], "1")) {    // check version of the file format

        // Load audio volume
        if (ini.hasSection("audio")) {
          int8_t speakerVol, headphonesVol, loudspeakerVol;
          audio->getVolumes(speakerVol, headphonesVol, loudspeakerVol);      // default values
          speakerVol = ini["audio"].getIntValueSafe("speaker_vol", speakerVol);
          headphonesVol = ini["audio"].getIntValueSafe("headphones_vol", headphonesVol);
          loudspeakerVol = ini["audio"].getIntValueSafe("loudspeaker_vol", loudspeakerVol);
          audio->setVolumes(speakerVol, headphonesVol, loudspeakerVol);
          log_d("loaded volume: earpiece = %d dB, headphones = %d dB, loudspeaker = %d dB", speakerVol, headphonesVol, loudspeakerVol);
          log_i("loaded volume: earpiece = %d dB, headphones = %d dB, loudspeaker = %d dB", speakerVol, headphonesVol, loudspeakerVol);
        }

        // Load timezone config
        if (ini.hasSection("time")) {
          float tz = ini["time"].getFloatValueSafe("zone", 0);
          ntpClock.setTimeZone(tz);
          tzIniHad = ini["time"].hasKey("zone");
          tzIniHours = tz;
        }

        // Load screen dimming & sleeping config
        if (ini.hasSection("screen")) {
          gui.state.brightLevel = ini["screen"].getIntValueSafe("bright_level", 100);
          /* ⚠ DEFAULT ON. These two fallbacks used to be 0, which disagreed with the
           * missing-SECTION branch a dozen lines below (it defaults both TRUE) and with
           * every expectation of a phone. A config that simply lacks the key got a screen
           * that never dims and never sleeps - full backlight until the battery is flat,
           * silently. The shipped data/configs.ini had the same two zeros, so any
           * `pio run -t uploadfs` handed a device that state. Found in the 2026-08-22
           * battery audit. Both are now ON, matching the else-branch. */
          gui.state.dimming = ini["screen"].getIntValueSafe("dimming", 1) > 0;
          gui.state.dimLevel = ini["screen"].getIntValueSafe("dim_level", 15);
          gui.state.dimAfterMs = ini["screen"].getIntValueSafe("dim_after_s", 20)*1000;
          gui.state.sleeping = ini["screen"].getIntValueSafe("sleeping", 1) > 0;
          gui.state.sleepAfterMs = ini["screen"].getIntValueSafe("sleep_after_s", 30)*1000;
          gui.state.screenBrightness = gui.state.brightLevel-1; // Forces the new brightness setting to be applied
          gui.processEvent(0, 0); // Need to call gui event loop so brightness settings are applied
        } else {
          gui.state.brightLevel = 100;
          gui.state.dimming = true;
          gui.state.dimLevel = 15;
          gui.state.dimAfterMs = 20000;
          gui.state.sleeping = true;
          gui.state.sleepAfterMs = 30000;
        }

        /* ⚠ DEFAULT ON, AND FOR THE SAME REASON THE TWO ABOVE ARE. A missing SECTION has
         * always defaulted this to 1 (the `: 1` below); a section that merely lacks the KEY
         * defaulted to 0 — so two configs that both say nothing about locking gave opposite
         * answers, and the quieter one silently shipped a phone whose screen never locked.
         * That is exactly the disagreement found in [screen] during the 2026-08-22 battery
         * audit, where it produced a phone that never dimmed and never slept. Same shape,
         * same fix: the two branches now agree. (Both phones read 1 today, so this was NOT
         * the cause of the 2026-08-25 "phone 2 never locks" — that was GUI::inCall(). This
         * is the landmine beside it.) */
        gui.state.locking = ini.hasSection("lock") ? ini["lock"].getIntValueSafe("lock_keyboard", 1) : 1;
      }
    }
    gui.setAudio(audio);
  }
  /* The time zone (0.9.82): the setting from NVS - Auto (the default, the upgrade too), a named
   * zone, or the typed offset just applied above. Auto keeps the typed offset until a MEASURED fix
   * finds a zone (tz_auto.h); a zone's DST offset waits for the clock (tzSvcLoop). */
  tzSvcBegin(tzIniHad, tzIniHours);

#ifdef HEADPHONE_DETECT_PIN
  pinMode(HEADPHONE_DETECT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HEADPHONE_DETECT_PIN), headphoneInterrupt, CHANGE);
  bool headphones = allDigitalRead(HEADPHONE_DETECT_PIN);
  audio->jackSensed(headphones);
#endif // HEADPHONE_DETECT_PIN

  log_v("Free memory after audio: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  ntpClock.startUpdates();

  // Setup for LoRa messaging
#if defined(LORA_MESSAGING) && !defined(MESHTASTIC_PHY)
  lora.setup();
#endif

  // Meshtastic background service (owns the LoRa radio when MESHTASTIC_PHY is set)
  /* 🛑 THE CARD VERDICT HAS TO GO IN FIRST. setup() is where the mesh database and the
   * starred-node list are READ, and which filesystem they are read from depends on this.
   * It used to be told only from loop(), i.e. after the read had already happened and
   * already answered "no card" — so both phones read a stale SPIFFS copy every boot while
   * writing a growing one to the SD card. {user}'s "after rebooting I lose the chat history"
   * was exactly that, and it is the reason this line is above the call and not below it. */
  meshService.setCardPresent(gui.state.cardPresent);
  meshService.setup();
  phonePosSetup();      // the hotspot phone's GPS: its custom port (the on/off is the service's)
  geolocSetup();        // Wi-Fi location: the setting (OFF by default) told to the service's slot

  /* The user's own T9 words, if the card has any. Here because the card verdict above is
   * finally trustworthy, and because this is the ONLY place the file is ever read — one
   * boot-time SD read, never the keypress path. Every failure is silent: no card, no file,
   * no PSRAM, and predictive text simply works from the built-in dictionary alone. */
  t9ExtraLoad(gui.state.cardPresent);
  gui.state.t9.setExtra(t9ExtraGet());

  log_d("WiPhone, firmware date = " __DATE__);

#ifdef CONFIG_APP_ROLLBACK_ENABLE
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    log_i("Got partition state: %d %d %d", ota_state, ESP_OTA_IMG_PENDING_VERIFY, been_in_verify);
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      log_i("Committing update");
      ota.commitUpdate();
    }
  }

#endif

  /* ── DIAGNOSTIC (2026-08-25): SUBSCRIBE THE LOOP TASK TO THE TASK WATCHDOG ────────────
   * 🔑 Nothing has ever watched this task, which is why the GPS wedge is SILENT: the loop
   * stops, other FreeRTOS tasks keep printing, and no watchdog notices. The existing
   * LOOP STALL detector (see loop()) cannot see it either — that one measures the gap
   * between two passes, so it only reports a stall the loop RECOVERED from. A stall that
   * never ends produces nothing at all.
   *
   * ⚠ panic=false ON PURPOSE. A timeout then PRINTS the offending task and its backtrace
   * and lets the phone carry on, instead of rebooting and destroying the evidence. That
   * backtrace is the entire point of this build.
   *
   * ⚠ 20 s, not the 5 s default: WiFi.disconnect(true) is measured at ~5 s of blocking in
   * this firmware (docs/HANDOFF.md), and the boot path has legitimate 951 ms passes. A
   * threshold under those would cry wolf and teach us to ignore it.
   *
   * ⚠ Arduino feeds this watchdog once per loop() RETURN — and loop() here never returns,
   * it wraps its own while(1). So the feed has to live inside that loop; it does. */
  esp_task_wdt_init(20, false);
  esp_task_wdt_add(NULL);            // NULL = this task = loopTask (setup runs on it)
  log_e("WDT: loop task subscribed, 20 s, print-not-panic (diagnostic build)");

  printf("\r\nBooted\r\n");

  /* ── MP3_HEAP_PROBE ────────────────────────────────────────────────────────────────
   * Allocates and frees the MP3 decoder once and reports what it cost.
   *
   * This checks the riskiest claim in the music player: that helix's ~29 KB lands in
   * PSRAM and leaves the INTERNAL heap alone. If that is ever wrong the phone does not
   * fail here — it fails minutes later when the WiFi PHY cannot get 2 KB for RF
   * calibration and phy_init aborts, with a backtrace pointing nowhere near audio. That
   * is a whole debugging session, so it is worth one line of proof.
   *
   * Off by default. Build with -DMP3_HEAP_PROBE, read at 500000 baud, then turn it off. */
#ifdef MP3_HEAP_PROBE
  {
    const uint32_t i0 = ESP.getFreeHeap(), b0 = ESP.getMaxAllocHeap(), p0 = ESP.getFreePsram();
    Mp3Stream* probe = new Mp3Stream();
    const bool ok = probe && probe->begin();
    const uint32_t i1 = ESP.getFreeHeap(), b1 = ESP.getMaxAllocHeap(), p1 = ESP.getFreePsram();
    if (probe) {
      delete probe;
    }
    const uint32_t i2 = ESP.getFreeHeap();
    log_e("MP3probe ok=%d internal %u->%u (cost %d) largest %u->%u psram %u->%u (cost %d) after_free %u",
          (int)ok, i0, i1, (int)i0 - (int)i1, b0, b1, p0, p1, (int)p0 - (int)p1, i2);
  }
#endif

  /* ── XFER_AUTOSTART ───────────────────────────────────────────────────────────────
   * Brings the music uploader up at boot so a file can be pushed to the card with no key
   * presses. Bench use only, and deliberately NOT a shipping feature: a server that opens
   * itself every boot is an open write endpoint on whatever network the phone joins.
   *
   * Build with -DXFER_AUTOSTART, then from a computer on the same WiFi:
   *   curl -F "rom=@track.mp3" http://wiphone.local/upload
   * Then rebuild without it. */
#ifdef XFER_AUTOSTART
  {
    static const XferConfig AUTO_CFG = {
      MUSIC_DIR, "Add music", ".mp3,.wav", "tracks", "download.mp3", "WiPhone-Music"
    };
    /* ⚠ WAIT for the station to associate first. xferStart() falls back to bringing up
     * its own access point when WiFi.status() != WL_CONNECTED, and at the end of setup()
     * the association is still in progress — so starting it here without waiting TEARS
     * DOWN the WiFi connection and puts the phone on its own AP instead. */
    for (int i = 0; i < 120 && WiFi.status() != WL_CONNECTED; i++) {
      delay(250);
    }
    log_e("XFER_AUTOSTART: wifi=%d ip=%s", (int)WiFi.status(),
          WiFi.localIP().toString().c_str());
    xferStart(&AUTO_CFG);
    log_e("XFER_AUTOSTART: uploader on at %s ap=%d", xferAddr(), (int)xferUsingAP());
  }
#endif


  /* ── MUSIC_SELFTEST ───────────────────────────────────────────────────────────────
   * Times the MP3 decoder on a real file from the card. Bench build only.
   *
   * This answers the one question the design could not answer from the desktop: helix's
   * state is in PSRAM, which is slower than internal RAM, and Audio::loop() decodes from
   * the MAIN LOOP alongside the screen and WiFi. A frame is 1152 samples = 26.1 ms of
   * audio at 44.1 kHz, so decode must stay well under that or playback stutters.
   *
   * Measures decode alone, with no I2S and no UI, which is the ceiling — whatever
   * headroom shows here is the most there will ever be. */
#ifdef MUSIC_SELFTEST
  {
    musicPlayerBegin();
    const int n = musicPlayerCount();
    log_e("SELFTEST: %d track(s) on the card", n);
    const MusicTrack* t = NULL;
    for (int i = 0; i < n; i++) {
      const MusicTrack* c = musicPlayerTrack(i);
      if (c && c->fmt == MUSIC_FMT_MP3) {
        t = c;
        break;
      }
    }
    if (t) {
      log_e("SELFTEST: %s  (%s)", t->name, t->path);
      File f = SD.open(t->path);
      if (f) {
        Mp3Stream* ms = new Mp3Stream();
        if (ms && ms->begin()) {
          uint8_t hdr[16];
          f.read(hdr, sizeof(hdr));
          uint32_t skip = mp3Id3v2Size(hdr, sizeof(hdr));
          f.seek(skip);
          log_e("SELFTEST: id3 tag %u bytes", skip);

          int16_t* pcm = (int16_t*)ps_malloc(MP3_MAX_FRAME_SAMPLES * sizeof(int16_t));
          Mp3Info info;
          memset(&info, 0, sizeof(info));
          uint32_t frames = 0, bad = 0, usDecode = 0, usRead = 0;
          const uint32_t t0 = millis();
          while (frames < 400 && (millis() - t0) < 20000 && pcm) {
            uint8_t tmp[512];
            size_t room = ms->space();
            if (room > sizeof(tmp)) {
              room = sizeof(tmp);
            }
            if (room > 0 && f.available()) {
              uint32_t r0 = micros();
              int got = f.read(tmp, room);
              usRead += micros() - r0;
              if (got > 0) {
                ms->fill(tmp, (size_t)got);
              }
            }
            uint32_t d0 = micros();
            int samples = ms->decode(pcm, &info);
            usDecode += micros() - d0;
            if (samples > 0) {
              frames++;
            } else if (samples < 0) {
              bad++;
            } else if (!f.available()) {
              break;
            }
          }
          const uint32_t wall = millis() - t0;
          if (frames > 0) {
            const uint32_t perFrame = usDecode / frames;
            const uint32_t budget = (uint32_t)(1152000000ULL / (info.sampleRate ? info.sampleRate : 44100));
            log_e("SELFTEST: %u frames %u bad, %d Hz %d ch %d kbps",
                  frames, bad, info.sampleRate, info.channels, info.bitrate / 1000);
            log_e("SELFTEST: decode %u us/frame, sd read %u us/frame, budget %u us -> %u%% of realtime",
                  perFrame, usRead / frames, budget,
                  (uint32_t)((uint64_t)(perFrame + usRead / frames) * 100 / budget));
            log_e("SELFTEST: %u frames in %u ms wall = %u ms of audio",
                  frames, wall, (uint32_t)((uint64_t)frames * 1152 * 1000 / (info.sampleRate ? info.sampleRate : 44100)));
          } else {
            log_e("SELFTEST: NO FRAMES DECODED");
          }
          if (pcm) {
            free(pcm);
          }
        } else {
          log_e("SELFTEST: decoder would not allocate");
        }
        if (ms) {
          delete ms;
        }
        f.close();
      } else {
        log_e("SELFTEST: could not open %s", t->path);
      }
    }
    log_e("SELFTEST: internal heap free %u largest %u psram %u",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram());
  }
#endif

  gui.state.booted = true;

  log_d("# # # # # # # # # # # # # # # # # # # # # # # # # # # #  END OF SETUP  # # # # # # # # # # # # # # # # # # # # # # # # # # # # ");
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  MAIN LOOP  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

TinySIP sip;
char lastKeys[7];         // TODO: use RingBuffer?
uint32_t msLastKeyPress = 0;        // for any button being pressed
uint32_t msLastKeyInput = 0;        // for the keyboard timeouts during alphanumeric intputs
uint32_t msPowerOffStarted = 0;
uint32_t msHangingUp = 0;
uint32_t msHungUp = 0;
uint32_t msLastRtpPacket = 0;
uint32_t msLastBatt = 0;
uint32_t msLastUsbCheck = 0;
uint32_t msLastMinute = 0;
uint32_t msLastWifiRetry = -WIFI_RETRY_PERIOD_MS;
uint32_t msLastWiFiRssi = -WIFI_CHECK_PERIOD_MS;
/* The far end's RTP silence is counted in Audio.cpp now, per call (rtp_watch.h); the boot-long
 * `rtpSilentCnt` that lived here is gone — it was half of why the second call of a boot died at
 * connect. */
bool keypadLedsOn = false;
bool updateMessageTimes = false;
bool waitingForClockUpdate = true;
uint8_t wifiTerminateSip = 0x0;

/* 🛑 `restoreSpeakerVol, restoreHeadphonesVol, restoreLoudspeakerVol` WERE HERE, AND NOTHING
 * EVER WROTE THEM. The one getVolumes() into them was commented out with the in-call loudspeaker
 * mute it belonged to, but the four setVolumes(restore...) calls at the call ends were not — so
 * every call end, and every END press on any screen, set the codec to 0/0/0 dB (a zero-initialised
 * global): the earpiece off its setting, and the loudspeaker to its MAXIMUM for the next ring.
 * With nothing lowered at call start there is nothing to restore; the in-call levels ARE the
 * configured ones (in-call UP/DOWN persists its own). Removed with all four calls. */

uint32_t usbConnected = 0;        // DEBUG
uint32_t usbConnectedChecks = 0;  // DEBUG

bool lastTurnOff = true;

//uint32_t msProfileStart = 0;
//LinearArray<uint32_t, false> msProfile;

uint32_t last_lora_send = 0;

// Meshtastic new-message popup banner
#define MESH_POPUP_MS 2500u
static bool     meshPopupActive = false;
static uint32_t meshPopupShownMs = 0;

/* HOLD the two top corner keys — Select (top left) + Back (top right) — for two seconds to
 * sleep the screen.
 *
 * This replaces triple-tapping Back, which was tried twice and failed on hardware both times.
 * A repeated tap on a key that ALSO means "go back" is a bad gesture on this phone: the gap
 * has to stay tight or backing out of three nested screens sleeps the phone by accident, and
 * tight means the gesture itself is unreliable — measured at about six presses to register
 * three. It also depended on a "is the user typing?" test that could be poisoned by a stale
 * widget pointer and kill the feature for an entire power-on.
 *
 * A two-key chord has none of that. It cannot be produced by ordinary navigation, so the hold
 * can be long and forgiving; there is no window to miss; and it needs no guess about whether
 * a text field is being edited, because Back alone never triggers it. {user}'s suggestion, and
 * the right one.
 *
 * Built on uiKeyDown, the held-key bitmask the keypad service already maintains (the SN7326
 * re-reports held keys about every 40 ms, and a stale-key sweep clears anything silent for
 * 350 ms), so this needs no new input plumbing. */
#define SLEEP_CHORD_MS   2000u
#define SLEEP_CHORD_MASK (WIPHONE_KEY_MASK_BACK | WIPHONE_KEY_MASK_SELECT)
/* Is a CALL actually happening?
 *
 * ⚠ Listed positively, and that matters. Both callers used to ask "is the state something
 * other than Idle/NotInited/HungUp", which sounds equivalent and is not: with no SIP proxy
 * reachable the phone rests in CallState::Error (12) FOREVER, so "not idle" was
 * permanently true. That silently broke two things — the CPU never dropped out of 240 MHz,
 * and the music-pauses-for-a-call edge had already fired at boot so it could never fire
 * for a real call. Error, Decline and HungUp are not calls; these eight are. */
/* ── THE BLACK BOX ──────────────────────────────────────────────────────────────────
 * Appends a line to /health.log on the card.
 *
 * Serial is no good for the two questions actually being asked. Measuring battery drain
 * needs the phone UNPLUGGED — plugged in it just charges — and an unplanned restart
 * happens when nobody is watching a terminal. So the log has to outlive both the cable
 * and the reboot, which means the card.
 *
 * Read it back with the uploader screen open: http://wiphone.local/log
 *
 * ⚠ Capped and wrapped rather than left to grow. A line a minute is ~90 KB a month, which
 * is nothing on the card, but a file that grows without limit is a slow leak of the one
 * resource the music library also needs. At the cap it starts again with a marker, so the
 * most recent hours always survive. */
#define HEALTH_LOG_PATH "/health.log"
#define HEALTH_LOG_TMP  "/health.tmp"
/* ⚠ RAISED 2026-08-23, 96K/32K -> 256K/128K, because 4 hours was not enough history to
 * answer the question this log exists for. {user} spent a day out with the phone on battery —
 * the first real on-battery discharge run there has ever been — and at the old settings the
 * retained window was ~250 lines, about FOUR HOURS. A drain run plus a car charge plus the
 * drive home does not fit in four hours, so the trim would have eaten the beginning of the
 * very run being measured, on the next boot, silently.
 *
 * The cost is nothing. A line is ~130 bytes and one is written per minute: 256 KB is about
 * 33 hours, ~2 MB a month at the cap, on a card that holds gigabytes and whose other
 * occupant is a 5 MB book. The original 96 K was cautious about a resource that turned out
 * not to be scarce, and the caution cost data twice — once on 2026-08-15 (the reset_reason
 * line for a restart, wrapped 29 minutes later) and nearly again today.
 *
 * ⚠ THE HOURS ABOVE ARE STALE (review F3, 2026-09-25). The HEALTH line is ~232 bytes now, not
 * ~130 (wdis/cr/ssf, pll/csw and clk= were added in 0.9.79 — see hl[] in healthLineTick), so
 * a quiet phone fills 256 KB in ~18 h and a trim keeps ~9 h, not ~16. Anything else that
 * writes here eats that window: the WiFi link lines are rate-limited for exactly that reason
 * (wifi_diag.h WifiCardGate — one LOST/JOIN pair a minute, the rest counted). If ~9 h is
 * ever too short, raise KEEP AND MAX together: each trim copies KEEP bytes on the loop task in
 * 512 B pieces, so a bigger KEEP under the same MAX means both longer and more frequent stalls. */
#define HEALTH_LOG_MAX  (256 * 1024)
#define HEALTH_LOG_KEEP (128 * 1024)  // ~560 lines of ~232 B: ~9 hours retained across a trim

/* ── KEEP THE NEWEST HISTORY, NOT NONE OF IT ──────────────────────────────────────────
 * This used to hit the cap and SD.remove() the whole file. That threw away the most
 * recent hours at the exact moment they mattered: on 2026-08-15 the phone rebooted while
 * {user} was out, the log wrapped 29 minutes later, and the BOOT reset_reason= line for
 * that reboot — the one number the whole restart investigation turns on — was destroyed
 * before anyone could read it.
 *
 * Now it copies the last HEALTH_LOG_KEEP bytes forward and swaps the file in, so there
 * is always several hours of history no matter when you get to it.
 *
 * ⚠ Streamed in a 512-byte stack buffer, never loaded whole: internal heap is this
 * phone's scarcest resource and a 32 KB allocation here would be self-defeating.
 * ⚠ Runs every ~9 hours (MAX - KEEP = 128 KB of growth at ~232 bytes a minute; the numbers
 * this said, 64 KB at ~130 bytes, were for the old caps), copying KEEP = 128 KB each time.
 * Returns false if anything went wrong, and the caller then falls back to deleting —
 * an unbounded log is worse than a lost one.
 * noinline: it has ONE caller, healthLogLine(), which every durable line goes through - GUI's
 * app-open heap probe among them, under an app's constructor. Inlined, buf[512] would be in that
 * frame on EVERY log line, not just the one in ~9 hours that trims. Out of line today (its own
 * 608 B frame, healthLogLine() 80 B - objdump, 2026-09-26); pinned by tests/check_health_log.py. */
static bool __attribute__((noinline)) healthLogTrim() {
  File in = SD.open(HEALTH_LOG_PATH, FILE_READ);
  if (!in) {
    return false;
  }
  const size_t total = in.size();
  if (total <= HEALTH_LOG_KEEP) {
    in.close();
    return true;                    // nothing to do
  }
  in.seek(total - HEALTH_LOG_KEEP);
  while (in.available() && in.read() != '\n') {
    ;                               // step over the partial line we landed in
  }

  SD.remove(HEALTH_LOG_TMP);        // a temp left by an interrupted trim
  File out = SD.open(HEALTH_LOG_TMP, FILE_WRITE);
  if (!out) {
    in.close();
    return false;
  }
  out.println("--- trimmed: older entries dropped, newest kept ---");
  uint8_t buf[512];
  bool ok = true;
  while (in.available()) {
    const size_t n = in.read(buf, sizeof(buf));
    if (n == 0) {
      break;
    }
    if (out.write(buf, n) != n) {
      ok = false;                   // card full or write error: keep the original
      break;
    }
  }
  in.close();
  out.close();
  if (!ok) {
    SD.remove(HEALTH_LOG_TMP);
    return false;
  }
  SD.remove(HEALTH_LOG_PATH);
  return SD.rename(HEALTH_LOG_TMP, HEALTH_LOG_PATH);
}

/* ── READ THE BLACK BOX WITHOUT ENDANGERING IT ─────────────────────────────────────────
 * Streams /health.log to the serial console. `health` on the console; see serial_cmd.cpp.
 *
 * ⚠ WHY THIS EXISTS WHEN http://wiphone.local/log ALREADY SERVES THE SAME FILE: reading it
 * over HTTP needs the WiFi uploader up, and that is precisely what this phone cannot afford
 * when contiguous internal heap is low. Measured 2026-08-23 after a 4.7 h run: free=11448,
 * largest=10560 — BELOW the ~16 KB baseline at which an 11 KB allocation has already
 * aborted the WiFi PHY and rebooted the phone once. The log exists to explain restarts, so
 * fetching it must never be the thing that causes one. It is also why the old note says
 * "one request, then close the screen, never poll".
 *
 * UART allocates nothing: a 256-byte stack buffer straight into the driver. No WiFi, no
 * sockets, no heap, and it works with the radio off and the card the only thing spinning.
 *
 * lastBytes = 0 dumps the whole file; otherwise the tail, rounded UP to a line boundary so
 * the first line is never a fragment that would parse as a bad sample.
 * Returns the file's total size, or -1 if there is no log to read. */
int healthDump(uint32_t lastBytes) {
  if (!gui.state.cardPresent) {
    return -1;
  }
  File f = SD.open(HEALTH_LOG_PATH, FILE_READ);
  if (!f) {
    return -1;
  }
  const uint32_t total = f.size();
  if (lastBytes && total > lastBytes) {
    f.seek(total - lastBytes);
    while (f.available() && f.read() != '\n') { }    // discard the partial first line
  }
  char buf[256];
  while (f.available()) {
    const int n = f.readBytes(buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    uart_write_bytes(UART_NUM_0, buf, (size_t)n);
  }
  f.close();
  /* The trailer is printed HERE, by the file that owns HEALTH_LOG_MAX/KEEP, because the
   * caller cannot know them without restating them — and a restated copy in serial_cmd.cpp
   * went stale within the hour the cap was raised. The size matters as much as the
   * contents: a file near the cap means the next trim is imminent and older samples are
   * about to go, which is exactly how a discharge run gets half-eaten before it is read. */
  const int wrote = snprintf(buf, sizeof(buf),
                             "\n--- end · %u bytes · cap %u · trims to newest %u ---\n",
                             (unsigned)total, (unsigned)HEALTH_LOG_MAX, (unsigned)HEALTH_LOG_KEEP);
  if (wrote > 0) {
    uart_write_bytes(UART_NUM_0, buf, (size_t)wrote);
  }
  return (int)total;
}

/* ── THE MEASUREMENT THE RATCHET STILL NEEDS ──────────────────────────────────────────
 * 0.9.43 removed the app-switching ratchet — 47 app opens across two phones netted `largest`
 * +0 (measured 2026-08-29). But phone 1 STILL lost 2,292 bytes over 174 minutes, in steps
 * that fall BETWEEN app opens. The app-open probe cannot see that by construction, and the
 * DROP instrument says a step happened without saying what caused it.
 *
 * So: mark `largest` at the specific events that are the remaining suspects. If the residual
 * is stepwise and the steps land on these marks, we have the culprit; if `largest` slides
 * smoothly between them, it is something none of these, and the next pass has to instrument
 * allocation SITES rather than events.
 *
 * ⚠ LOOP TASK ONLY. healthLogLine() opens a file on the SD card, so this must never be
 * called from the WiFi event task (4 KB stack, and it would race the loop's own SD access).
 * That is why association is detected by watching WiFi.status() change from the loop rather
 * than by hooking processWiFiEvent(). */
void heapEvent(const char* what) {
  char line[96];
  snprintf(line, sizeof(line), "MARK %-10s largest=%u free=%u up=%lus", what,
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)ESP.getFreeHeap(), (unsigned long)(millis() / 1000));
  log_e("%s", line);
  healthLogLine(line);
}

/* The association marks: every change of WiFi.status() on serial, but on the CARD only a move
 * INTO or OUT OF WL_CONNECTED, and those at most one disassoc/assoc pair a minute — the same
 * gate as Networks.cpp's WIFI LOST/JOIN (wifi_diag.h WifiCardGate; review F3). A flapping link
 * wrote one of these per status change, on top of the LOST/JOIN pair, with no limit at all; and
 * a change between two NOT-connected states (DISCONNECTED <-> NO_SSID_AVAIL in a reconnect
 * storm) was written as a "disassoc" too, which it is not. What the card misses is counted onto
 * its next MARK line ("+N/M serial-only"). noinline: the line buffer stays out of loop()'s frame
 * (review F1). LOOP TASK ONLY, like heapEvent(). */
static void __attribute__((noinline)) wifiMarkLink(int was, int ws, uint32_t now) {
  static WifiCardGate s_gate;
  const bool assoc = (ws == WL_CONNECTED);
  const bool edge = assoc || was == WL_CONNECTED;
  const bool card = edge && (assoc ? s_gate.join(now) : s_gate.lost(now));
  char line[144];               // ~68 for the mark, ~55 for a realistic held count: never cut
  int n = snprintf(line, sizeof(line), "MARK %-10s largest=%u free=%u up=%lus status %d->%d",
                   assoc ? "assoc" : "disassoc",
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                   (unsigned)ESP.getFreeHeap(), (unsigned long)(millis() / 1000), was, ws);
  uint32_t heldL = 0, heldJ = 0, since = 0;
  if (card && s_gate.takeHeld(now, &heldL, &heldJ, &since) && n > 0 && n < (int)sizeof(line)) {
    snprintf(line + n, sizeof(line) - (size_t)n, " (+%lu/%lu disassoc/assoc serial-only since up=%lus)",
             (unsigned long)heldL, (unsigned long)heldJ, (unsigned long)(since / 1000));
  }
  log_e("%s", line);
  if (card) {
    healthLogLine(line);
  }
}

/* The same, but only when it actually moved — for events too frequent to log unconditionally
 * (a mesh TX happens ~30 times an hour). Returns the current largest so a caller can chain. */
uint32_t heapDelta(const char* what, uint32_t before) {
  const uint32_t now = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  const int32_t d = (int32_t)now - (int32_t)before;
  if (d <= -256 || d >= 256) {
    char line[96];
    snprintf(line, sizeof(line), "MARK %-10s largest %u->%u (%+ld)", what,
             (unsigned)before, (unsigned)now, (long)d);
    log_e("%s", line);
    healthLogLine(line);
  }
  return now;
}

/* Not static: GUI.cpp's app-open heap probe writes here too, so the culprit lands in the
 * same durable log everything else is read from. */
void healthLogLine(const char* line) {
  if (!gui.state.cardPresent) {
    return;
  }
  File f = SD.open(HEALTH_LOG_PATH, FILE_APPEND);
  if (!f) {
    return;
  }

  /* ⚠ Trim BEFORE writing the boot line, not after. The size check used to sit below it,
   * so a boot line could be written and then immediately destroyed by the very same call
   * that wrote it — losing the reset reason on exactly the boot that followed a long run. */
  if (f.size() > HEALTH_LOG_MAX) {
    f.close();
    if (!healthLogTrim()) {
      SD.remove(HEALTH_LOG_PATH);   // last resort; never let it grow without bound
    }
    f = SD.open(HEALTH_LOG_PATH, FILE_APPEND);
    if (!f) {
      return;
    }
  }

  if (bootLine[0]) {
    f.println("");
    f.println(bootLine);      // why the last run ended, written once the card is up
    bootLine[0] = '\0';
  }
  f.println(line);
  f.close();
}

/* ── "DOES ANYTHING NEED 240 MHz" IS NOT THE SAME QUESTION AS "IS A CALL UP" ──────────
 * MEASURED 2026-08-15, from the car log: the phone sat at `sip=6` (CallState::HangUp) with
 * the screen off and no network (`wifi=1`) for 19+ consecutive minutes, and the CPU stayed
 * at 240 MHz the whole time because sipCallActive() counts HangUp as a call. At 80 MHz the
 * core draws roughly half as much, so this was about double the idle current — while out of
 * range in a car, which is exactly when the battery matters most.
 *
 * HangUp and HangingUp are TEARDOWN states. HangUp's own comment says it "can be triggered
 * from any other state", and HangingUp is "waiting for confirmation of BYE/CANCEL,
 * resending" — with no proxy reachable that confirmation never comes and the phone rests
 * there indefinitely. Neither has an audio session to protect.
 *
 * ⚠ This is the SAME TRAP the repo already hit once and wrote up: the phone resting forever
 * in a state that the "is a call happening" test counts as active. That fix replaced an
 * exclusion test with a positive list of eight states — correct as far as it went, but two
 * of those eight can also stick.
 *
 * ⚠ DELIBERATELY A SEPARATE PREDICATE, not an edit to sipCallActive(). That one ALSO gates
 * music-pauses-for-a-call, and quietly changing a shared predicate to fix an unrelated
 * caller is the exact bug class that produced three separate faults in this codebase today.
 * The CPU gate asks its own question. */
/* May SIP poll the network this pass?
 *
 * ⚠ A NEW predicate on purpose. sipNeedsFullSpeed() and sipCallActive() already exist and mean
 * different things, and this codebase has been bitten repeatedly by one caller quietly editing
 * a shared predicate another caller depended on. Every consumer states what it needs.
 *
 * WHY: the Game Boy commits 16 KB of INTERNAL RAM that cannot be relocated — an 8,192-byte
 * blit task stack, a 4,096-byte emulator task stack and a 4,096-byte audio buffer. FreeRTOS
 * stacks physically cannot run from PSRAM on the ESP32, so unlike the ROM and both
 * framebuffers (already correctly in PSRAM) none of this can move. Meanwhile every SIP poll
 * calls WiFiUDP::parsePacket(), which wants 1,460 CONTIGUOUS bytes from that same pool. With
 * ~26 KB free, the emulator takes 16 and the two of them fight over what is left.
 *
 * So while the emulator is on screen, SIP stops polling entirely: no UDP reads, no re-REGISTER.
 * Registration lapses after its 60 s expiry and re-establishes when the game exits. The cost is
 * missed calls during a game — the trade the phone's owner explicitly asked for.
 *
 * ⚠ A LIVE CALL OUTRANKS THE EMULATOR. If audio is already flowing, dropping it because the
 * games menu got opened would be worse than the memory pressure it saves. */
static bool sipMayPoll() {
  switch (gui.state.sipState) {
    case CallState::InvitingCallee:
    case CallState::InvitedCallee:
    case CallState::RemoteRinging:
    case CallState::Call:
    case CallState::BeingInvited:
    case CallState::Accept:
    case CallState::HangUp:
    case CallState::HangingUp:
      return true;                 // mid-call or mid-teardown: keep servicing it
    default:
      break;
  }
  return !gui.isAppRunning(GUI_APP_GBC);
}

static bool sipNeedsFullSpeed() {
  switch (gui.state.sipState) {
    case CallState::InvitingCallee:
    case CallState::InvitedCallee:
    case CallState::RemoteRinging:
    case CallState::Call:
    case CallState::BeingInvited:
    case CallState::Accept:
      return true;                 // a live or imminent audio session
    default:
      return false;                // includes HangUp/HangingUp: teardown, and they stick
  }
}

/* sipCallActive() stood here: the six live states PLUS HangUp and HangingUp. Its last caller,
 * the call-wins-over-music pause, moved to gui.inCall() in 0.9.79 — HangUp stuck for hours
 * (no WiFi, or no account after an END press) and that pause kept the music from ever playing.
 * The notes in this file that name it are history: every one of them is about a caller that
 * was moved off it for the same reason. */

#define MUSIC_F2_HOLD_MS 500           // past this, F2 means "previous" rather than "next"
static uint32_t msF2Down = 0;          // when F2 went down, 0 = not held
static bool     f2Fired = false;       // the hold already acted; ignore the release

/* Hold a number key to type the DIGIT rather than the letters on it — the way every phone
 * with a keypad has worked. It is what makes "testing 1 2 3" typable without ever leaving
 * predictive text: the '#' mode cycle is the deliberate, sentence-long escape, and this is
 * the one-character one.
 *
 * 500 ms, matching the F2 hold above, so the two gestures on this phone feel the same. */
#define T9_DIGIT_HOLD_MS 500
static uint32_t msDigitDown = 0;       // when the digit went down, 0 = none held
static int8_t   digitHeld = -1;        // which digit, so a slide to another key re-arms
static bool     digitFired = false;    // one insertion per hold, however long it lasts
static bool     digitLiteralQueued = false;   // a held digit is waiting to be delivered
static uint32_t msHashDown = 0;        // when '#' went down, 0 = not held
static bool     hashFired = false;     // the hold already acted; ignore the rest of it
static bool     hashDeferred = false;  // a '#' press swallowed at key-down, owed on release (mute hold)
/* Is '#' free for the mute hold right now? Not in any focused text field — the dialer's
 * number, a predictive field (where the hold is a capital), a plain one (where a tap is
 * shift) — and not on the map, whose '#' is zoom in. Asked at the press (to swallow it) and
 * on every pass of the tracker; the two must agree, so there is one function. */
static inline bool uiHashMuteArmed() {
  return !FocusableWidget::textFieldFocused() && !gMapsActive;
}
static bool     hashTapPending = false;   // ...and the release came before the hold: deliver it
static bool     muteTogglePending = false; // the hold flipped the mute; the icon has to follow
static bool     redrawWhatPending = false;    // a hold changed something the footer shows
static uint32_t msChordStart = 0;      // when both corners went down, 0 = not held
static bool     chordFired = false;    // one sleep per hold, not one every loop

// Quiet "pop" sound on a new message (one-shot), stopped by timer. How long is arithmetic on
// sizeof(pop_pcm), not a literal: see notify_timing.h for why the flash buffer (plays once)
// and the SPIFFS file (loops) need OPPOSITE margins, and tests/test_notify.cpp for the numbers.
static bool     meshPopPlaying = false;
static bool     meshPopCutMusic = false; // the pop stopped a PLAYING track: its teardown carries it on
static uint32_t meshPopStartMs = 0;
static uint32_t meshPopStopMs  = 0;     // set per pop from notifyPopStopMs()
/* MESH_POP_MS used to be a literal 280u here — the looping file's number, which happened to
 * cut 40 ms of the sound's own trailing silence. Kept as a name so a grep lands here. */
#define MESH_POP_MS (meshPopStopMs)

// Short silent vibration on a new Meshtastic message.
/* MESH_VIBRO_MS used to be a literal 180 here. The notification buzz length is now a user
 * setting — gui.state.notifyVibroMs, Settings > Notifications > Buzz — so the two places that
 * time the pulse read that instead. Kept as a name so a grep for the old constant lands here. */
#define MESH_VIBRO_MS (gui.state.notifyVibroMs)
static bool     meshVibroActive = false;
static uint32_t meshVibroStartMs = 0;
static uint8_t  meshVibroOffFails = 0;  // consecutive passes whose LOW the extender refused
/* How long the running pulse is. The notification sets it to the Buzz setting; the mute
 * hold's confirmation is a fixed short tap, so it cannot be mistaken for a message. */
static uint32_t meshVibroLenMs = 0;
#define MUTE_BUZZ_MS 70u

/* ── THE AUDIO DEVICE IS RELEASED WHEN NOBODY IS USING IT ─────────────────────────────────
 *
 * 🛑 THIS EXISTS BECAUSE `Audio` IS A DEVICE-WIDE SINGLETON WITH NO OWNERSHIP DISCIPLINE, AND
 * THAT HAS NOW PRODUCED THE SAME FAULT FIVE TIMES. Something turns the codec on; nothing turns
 * it off. `Audio::shutdown()` is the ONLY function that calls `codec.shutDown()` and
 * `i2s_stop()` (Audio.cpp:274, :311) — and every one of its call sites is a SIP call teardown
 * or a debug easter egg. There is no idle path. So:
 *
 *   - stopping a track leaves it on: music_player.cpp only ever calls `Audio::stopMusic()`,
 *     which is `playbackFile.close()` + `ceasePlayback()`, and `ceasePlayback()` (Audio.cpp
 *     :673-682) clears the decode buffers and touches neither the codec nor I2S;
 *   - a notification pop leaves it on: the teardown below uses ceasePlayback()+restore() on
 *     purpose, because shutdown()'s codec power-down on the shared I2C bus disturbs the vibro
 *     motor extender;
 *   - and on a phone with no working SIP proxy, the call teardowns never run at all.
 *
 * ⚠ AND IT IS INVISIBLE. After `stopMusic()`, `musicPlayerIsPlaying()` reads FALSE, so the CPU
 * governor drops to 80 MHz and every instrument this project owns reports a perfectly idle
 * phone — while the DAC, the chosen output driver, the external amplifier, VREF/VMID, the left
 * ADC and the MICROPHONE BIAS all stay powered (WM8750.h powerUp() sets POWER1 =
 * VMIDSEL|VREF|AINL|ADCL|MICB regardless of the mask) and I2S keeps running.
 *
 * 🔑 WHY A WATCHDOG RATHER THAN FIXING EACH CALLER: fixing music_player fixes music. The next
 * thing to borrow the device breaks it again, exactly as the previous four did. This closes
 * the class. It is SAFE to do here because `shutdown()` uses `i2s_stop()` and NEVER uninstalls
 * the driver (see the note in Audio::configureI2S) — so releasing and re-acquiring costs no
 * DMA reallocation, which is the one thing that would have made this a bad idea on a phone
 * whose panics are internal-heap fragmentation.
 *
 * ⚠ AND IT IS LOUD, ON PURPOSE. It logs at log_e every time it fires. A watchdog that cleans
 * up silently is a bug-hider; this one is an instrument that names the leak each time it
 * happens. If this line stops appearing after a caller is fixed, that caller was the leak.
 *
 * 🎁 It also closes the hot-mic leak by the same stroke: shutdown() is what clears
 * microphoneOn / microphoneStreamOut / rtpRemotePort (see the 🛑 note at the top of it), so a
 * call that ended without a teardown no longer leaves the microphone armed indefinitely. */
/* ── WIFI MODEM SLEEP IS RE-ASSERTED, BECAUSE THE DRIVER KEEPS THROWING IT AWAY ──────────
 *
 * 🛑 SAME BUG CLASS AS THE AUDIO WATCHDOG BELOW, ON A DIFFERENT SHARED DEVICE: one component
 * sets a mode, another restarts the device and silently inherits the default.
 *
 * `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` appears EXACTLY ONCE in this whole tree —
 * Networks.cpp:174, inside connectToWiFi() — and connectToWiFi() has exactly one caller
 * (connectTo(), Networks.cpp:445). The comment there says why it sits after begin():
 * "the driver resets the power-save mode when the station starts, so setting it earlier is
 * silently undone." That is the whole problem: EVERY OTHER PATH THAT STARTS THE STATION
 * NEVER GOES NEAR IT, so the receiver comes back powered continuously — which the comment
 * two paragraphs above it costs at "tens of milliamps, forever".
 *
 * The paths that restart the station without connectToWiFi(), all verified:
 *   - Networks::bounceRadio() (Networks.cpp:504-513) — disconnect(true) then mode(WIFI_STA).
 *     ⚠ THIS IS THE AUTOMATIC DEAF-RADIO CURE. It fires by itself after two empty scans, so
 *     an ordinary WiFi blip in a pocket silently doubles the radio's idle draw for the rest
 *     of the boot, with nothing on screen and nothing in the log.
 *   - The Settings WiFi toggle turning back ON (GUI.cpp:1839) and the edit screen
 *     (GUI.cpp:6632) — both bare esp_wifi_start(). ⚠ So the one control a user reaches for
 *     to SAVE power leaves the radio drawing MORE once they turn it back on.
 *   - The Game Boy: mode(WIFI_OFF) on entry (app_gbc.cpp:188), mode(WIFI_STA) + reconnect()
 *     on exit (app_gbc.cpp:328-329).
 *   - The ROM uploader: mode(WIFI_STA) + begin() when it stops (app_gbc_xfer.cpp:1766-1772).
 *   - The blocking scan path (Networks.cpp:821).
 *   (0.9.79: the toggle, the edit screen, the Game Boy's exit and the uploader/window teardown
 *   now all go through wifiRestoreStation() — still without esp_wifi_set_ps, so this still
 *   applies to every one of them.)
 *
 * ⚠ NOT GATED ON `wifiOn`. That global (GUI.cpp:45) is a label, not the radio's state, and
 * the audit found three paths that restart the radio without touching it — gating on a flag
 * that can lie is how this kind of guard quietly stops running. esp_wifi_get_ps() fails
 * cleanly when the station is not up, and that error IS the gate.
 * ⚠ Nothing to do for AP mode either: the power-save setting is station-scoped, so the
 * uploader's softAP neither needs it nor is harmed by it.
 * ⚠ LOUD, like the audio watchdog. Every firing names a station restart that did not restore
 * the mode; if this line stops appearing after a caller is fixed, that caller was the leak. */
#define WIFI_PS_RECHECK_MS  5000u
static uint32_t wifiPsCheckMs = 0;

#define AUDIO_IDLE_RELEASE_MS  30000u   // generous: the leak lasts hours, so 30 s recovers ~all of it
static uint32_t audioBusyStampMs = 0;   // last pass on which something was using the device

/* May the audio device be released this pass?
 *
 * ⚠ A NEW PREDICATE ON PURPOSE. sipCallActive(), sipNeedsFullSpeed() and sipMayPoll() already
 * exist and mean three different things, and this codebase has been bitten repeatedly by one
 * caller quietly editing a shared predicate another caller depended on. This one asks its own
 * question: is ANY part of the phone currently entitled to the audio hardware?
 *
 * 🛑 `sipNeedsFullSpeed()` AND **NOT** `sipCallActive()`. This was the other way round for one
 * revision, with a comment arguing that a teardown must not be cut off mid-BYE. That reasoning
 * was WRONG, and wrong against data already in hand: sipCallActive() counts HangUp and
 * HangingUp, and those states STICK on these phones - sip=6 was measured in 1,066 of 1,071
 * health samples on phone 2 and 295 on phone 1. Wiring the watchdog to a permanently-true term
 * meant it could NEVER FIRE on the exact hardware it was written for. A teardown whose RTP is
 * genuinely still flowing is held by movingSamples() anyway (playback == RtpStream), which is
 * the device's own truth rather than a session flag that can latch.
 *
 * ⚠ `screenBrightness > 0` IS A DELIBERATE CATCH-ALL, not laziness. Several consumers acquire
 * the device with no non-sticky flag that says so - the mic-level meters (GUI.cpp:8988, 9076,
 * 9974, 11800) go through turnMicOn(), which sets only `microphoneOn`, and that is cleared
 * NOWHERE but shutdown(). Enumerating them one by one is precisely the game this watchdog
 * exists to stop playing. A lit screen means a person is present, and the leak being hunted is
 * the one that persists for HOURS with the screen dark - so this costs nothing real and buys
 * immunity from every UI consumer nobody has enumerated yet, including future ones.
 *
 * ⚠ The vibro terms are NOT about audio at all — they are the I2C hazard. The codec and the
 * SX1509 extender share the bus, and shutdown()'s codec power-down is documented in the pop
 * teardown below as able to disturb the motor. Never release while the motor is driven. */
static bool audioDeviceBusy() {
  if (!audio) {
    return false;
  }
  return audio->movingSamples()          // the device is pushing or pulling samples
         || sipNeedsFullSpeed()          // a live or imminent audio session - see the note
         || gui.state.ringing            // the phone is ringing
         || meshPopPlaying               // a notification pop is mid-play
         || gGbcActive                   // the emulator owns the device while it runs
         || musicPlayerIsPlaying()       // belt and braces over movingSamples()
         || gui.state.screenBrightness > 0   // somebody is LOOKING at it - see the note
         || meshVibroActive              // I2C hazard: motor is driven
         || gui.state.vibroOn;           // I2C hazard: ringer motor is driven
}

/* May an RTP session (the mic streaming out, or a call's stream playing) exist this pass?
 *
 * ⚠ A NEW PREDICATE ON PURPOSE, like the ones above it: this asks only "does a live call own
 * the RTP session". sipNeedsFullSpeed() is the live-or-imminent set; the session is armed only
 * in the pass that has just moved the state to Call, and every teardown shuts it down in the
 * pass that leaves the set. So this is never false under a real call.
 * ⚠ `hasSipAccount()` IS THE POINT, not decoration. Remove the account mid-call and the SIP
 * block stops running: the state stays Call for good, so the state alone would entitle the
 * session forever. Change the account mid-call and the SIP block re-inits straight to Idle
 * without a shutdown() — that one the state catches. Both were gaps in the 2026-08-16 fix. */
static bool rtpSessionEntitled() {
  return sipNeedsFullSpeed() && gui.state.hasSipAccount();
}
static RtpOrphan rtpOrphan = {0, false};   // the backstop's clock; see rtp_watch.h

/* The `audio` serial command's body. Lives here because the answer needs the SIP state, the
 * emulator flag, the music player and the vibro state, none of which serial_cmd.cpp can see.
 *
 * ⚠ EVERY NUMBER HERE IS THE FIRMWARE'S SHADOW OF THE CHIP, NOT THE CHIP. The WM8750 is
 * write-only over its 2-wire interface (src/drivers/WM8750.h defines setReg and no readReg),
 * so there is no way to ask the codec what it actually has powered. That is precisely why the
 * watchdog releases the device on a timer instead of trusting this bookkeeping — and it is why
 * `aud=` in the health line is worth more than this command: it is sampled once a minute for
 * hours, so it catches the leak arriving even when nobody is at the cable.
 *
 * `route=` is where the output goes — headphones > loudspeaker > earpiece, the precedence
 * Audio::start() and codecReconfig() build the codec's power mask with. It is meaningful with
 * powered=no as well: that is the route the next start() inherits, which is how a game left
 * on the earpiece (the 2026-09-19 fix in app_gbc.cpp) and a game that forgot to put the
 * loudspeaker back would both show. */
int audioStateDump(char* out, int cap) {
  if (!audio) {
    return snprintf(out, cap, "audio: no device object\n");
  }
  const bool on = audio->isOn();
  const bool moving = audio->movingSamples();
  const bool busy = audioDeviceBusy();
  int n = snprintf(out, cap,
                   "audio: powered=%s moving=%s busy=%s route=%s muted=%s idle=%lus (release at %lus)\n"
                   "  entitled: sip=%d ringing=%d pop=%d gbc=%d music=%d scr=%d vibro=%d/%d\n",
                   on ? "YES" : "no", moving ? "YES" : "no", busy ? "BUSY" : "idle",
                   audio->getHeadphones() ? "headphones" : (audio->isLoudspeaker() ? "loudspeaker" : "earpiece"),
                   audio->isMuted() ? "YES" : "no",
                   (unsigned long)(audioBusyStampMs ? (millis() - audioBusyStampMs) / 1000 : 0),
                   (unsigned long)(AUDIO_IDLE_RELEASE_MS / 1000),
                   (int)sipNeedsFullSpeed(), (int)gui.state.ringing, (int)meshPopPlaying,
                   (int)gGbcActive, (int)musicPlayerIsPlaying(),
                   (int)(gui.state.screenBrightness > 0),
                   (int)meshVibroActive, (int)gui.state.vibroOn);
  /* The RTP session and the levels. After ANY call has ended, mic= stream= rtpPort= must all
   * read 0 — that is the hot-mic check. vol= is what the codec holds now (earpiece/headphones/
   * loudspeaker, dB); callvol= is where the call levels are while music holds the codec. */
  int8_t ve = 0, vh = 0, vl = 0, ce = 0, ch = 0, cl = 0;
  audio->getVolumes(ve, vh, vl);
  const bool stashed = musicPlayerCallVolumes(ce, ch, cl);
  if (!stashed) {
    ce = ve;
    ch = vh;
    cl = vl;
  }
  n += snprintf(out + n, cap > n ? cap - n : 0,
                "  rtp: mic=%d stream=%d rtpPort=%u armed=%d owned=%d  vol=%d/%d/%d callvol=%d/%d/%d%s\n",
                (int)audio->micOn(), (int)audio->micStreamOut(), (unsigned)audio->rtpPeerPort(),
                (int)audio->rtpSessionArmed(), (int)rtpSessionEntitled(),
                (int)ve, (int)vh, (int)vl, (int)ce, (int)ch, (int)cl,
                stashed ? " (in music's stash)" : "");
  if (on && !busy) {
    n += snprintf(out + n, cap > n ? cap - n : 0,
                  "  ^ POWERED WITH NOTHING ENTITLED TO IT - this is the leak. Watch for the\n"
                  "    'AUDIO: device was left powered' line; it fires once per leak.\n");
  }
  return n;
}

/* The music feed's counters, for `audio` and `music` (serial). What each one can and cannot
 * claim is at the top of music_feed.h; in one line:
 *   drops   = passes that found the I2S ring CERTAINLY dry (an audible gap; never invented);
 *   minLead = the lowest LOWER bound on the audio queued, at any pass start since the track
 *             (or `music reset`) began — above 0 means there was certainly no gap at all;
 *   maxGap  = the longest the main loop left music unfed (the LOOP STALL line only sees > 250 ms).
 * ⚠ Each line stays under say()'s 192 bytes. */
int musicStateDump(char* out, int cap) {
  if (!audio || cap <= 0) {
    return 0;
  }
  MusicStats st;
  if (!audio->musicStats(&st)) {
    return snprintf(out, cap, "  music: nothing opened since boot\n");
  }
  static const char* const TONES[] = {"old", "clean", "flat"};
  const uint8_t tone = audio->codecTone();
  int n = snprintf(out, cap,
                   "  music: %s %lu Hz %u ch -> %lu Hz mono, ring %lu ms, swap=%s tone=%s | playing=%d paused=%d ended=%d\n",
                   musicPlayerCurrent() >= 0 ? "track" : "-", (unsigned long)st.srcHz, (unsigned)st.srcCh,
                   (unsigned long)st.outHz, (unsigned long)st.capMs, st.swap ? "on" : "off",
                   tone < 3 ? TONES[tone] : "?", (int)musicPlayerIsPlaying(), (int)musicPlayerIsPaused(),
                   (int)st.ended);
  const char* why = audio->musicError();
  n += snprintf(out + n, cap > n ? cap - n : 0,
                "  music: drops=%lu (%lu ms) minLead=%ld ms lead=%ld ms | units=%lu skipped=%lu reservoir=%lu%s%s\n",
                (unsigned long)st.drops, (unsigned long)st.dropMs, (long)st.minLeadMs, (long)st.leadMs,
                (unsigned long)st.units, (unsigned long)st.skipped, (unsigned long)st.reservoir,
                why ? " | error: " : "", why ? why : "");
  n += snprintf(out + n, cap > n ? cap - n : 0,
                "  music: passes=%lu maxGap=%lu ms maxWork=%lu.%lu ms avgWork=%lu us | reads=%lu (%lu KB, %lu failed) inBuf=%lu B\n",
                (unsigned long)st.passes, (unsigned long)st.maxGapMs, (unsigned long)(st.maxWorkUs / 1000),
                (unsigned long)((st.maxWorkUs % 1000) / 100), (unsigned long)st.avgWorkUs,
                (unsigned long)st.reads, (unsigned long)st.readKB, (unsigned long)st.readErrors,
                (unsigned long)st.inBuf);
  return n;
}

/* Serial `audio orphan`: arm an RTP session with NO call, so the backstop can be watched firing
 * without a second phone, an easter egg or a hot mic. RECEIVE side only — playRtpStream() on a
 * local port; the microphone is never switched on and nothing is sent anywhere. Expect the
 * 'AUDIO: RTP session armed with no live call' line within RTP_ORPHAN_RELEASE_MS, then `audio`
 * showing armed=0. Refused during a call (it would be owned, and would steal the call's audio),
 * under the emulator, and while anything is playing. Costs one I2S reinstall to 8 kHz mono and one UDP socket, both given
 * back by the shutdown(). Returns false when refused. */
bool audioBenchOrphan() {
  /* ⚠ Nothing may be PLAYING either: playRtpStream() takes `playback` without ceasePlayback(),
   * so a track under it would lose its file handle for good (shutdown() closes the MP3 only
   * while playback still says LocalMp3). Pause the music first; the refusal says so. */
  if (!audio || gGbcActive || gui.inCall() || audio->rtpSessionArmed() || audio->movingSamples()) {
    return false;
  }
  audio->openRtpConnection(5004);        // a local port, receive only
  audio->playRtpStream(Audio::ALAW_RTP_PAYLOAD);
  return true;
}


/* The brief "you have a message" pop + buzz.
 *
 * ⚠ SHARED BY THE MESH AND SIP PATHS, and that is the whole point of it existing.
 * An incoming SIP TEXT never announced itself — only mesh messages did. That went unnoticed
 * while texts were reaching the phone through the LoRa mirror, because those arrive on the
 * MESH path and chirped as a side effect. Silencing the mirror (which is right: a mirrored
 * copy of a text you already have is not news) therefore took the notification away from
 * real incoming texts too, and {user} noticed immediately: "I seem to have lost the vibration
 * for notifications, and the chirp."
 *
 * So the announcement now belongs to the ARRIVAL, not to the transport that carried it.
 * Both callers share this, and the teardown timers below already service either one. */
static void notifyMessageArrived(uint8_t mode);

/* The mirror's way in. Non-static so meshtastic_service.cpp and the LAN poller can reach it
 * without either of them needing to know about ringer modes or the vibro motor. */
void smsMirrorNotifyArrival() {
  notifyMessageArrived(gui.state.notifySipMode);
}

/* The bench's way in: serial `notify` runs the mesh arrival's announcement — same motor write,
 * same pop, same stamps and the same teardown — without a second phone on the air; `notify sip`
 * runs the text path's. Non-static for serial_cmd.cpp. */
bool notifyBenchArrival(bool sip) {
  if (gGbcActive) {
    return false;                        // see the pop gate: I2S under the emulator's feet
  }
  notifyMessageArrived(sip ? gui.state.notifySipMode : gui.state.notifyMeshMode);
  return true;
}

/* A pop in flight is finished NOW — for the Game Boy, whose startGame() snapshots the audio
 * routing and reconfigures the device: a chirp that started on the ROM picker would hand it
 * the pop's forced loudspeaker as "the phone's route", and the teardown's restore() would
 * then land mid-game (review, 2026-09-19). Same teardown as the loop's, just early.
 *
 * ⚠ THE RING AND A CALL'S AUDIO NEED THE SAME HAND-OVER (0.9.79). The pop gate refuses to START
 * a pop only in CallState::Call, so one can still be mid-play when the phone starts ringing or
 * an outgoing call connects — and the loop's cut-short branch then restore()s the pre-pop state
 * OVER the new owner: the ringtone's 8 kHz PCM clocked at the pre-pop rate (5.5x fast after a
 * 44.1 kHz track) through the pre-pop route and levels, or the call's I2S reconfigured under its
 * microphone. startRingtone() and the call's audio setup finish it first, so
 * their own configuration lands last. `why` names the new owner in the log line.
 * Costs one I2S reinstall only when a pop genuinely overlaps (up to ~0.9 s: a fresh ring's
 * 512 ms lead plus the sound - review 3, A1).
 *
 * ⚠ notifyPopFinishNow() keeps its old no-argument signature: app_gbc.cpp declares it extern
 * by that shape, and a default argument would clash with the .ino prototype generator. */
void notifyPopFinishFor(const char* why) {
  if (meshPopPlaying) {
    audio->ceasePlayback();
    audio->restore();
    meshPopPlaying = false;
    log_e("NOTIFY: pop finished early (%s)", why);
  }
}

void notifyPopFinishNow() {
  notifyPopFinishFor("a game is starting");
}

/* One motor write, checked. allDigitalWrite() on this board is an SX1509 read-modify-write
 * over the I2C bus the codec, keypad and gauge also use, and until now its result was thrown
 * away — a refused HIGH was a notification that never buzzed, a refused LOW a motor left
 * running, and neither left a trace. One retry after 2 ms: the bus is single-master and each
 * transaction is under a millisecond, so a transient fault has cleared by then or is not
 * transient. */
static bool motorWrite(int level) {
  if (allDigitalWrite(VIBRO_MOTOR_CONTROL, level)) {
    return true;
  }
  delay(2);
  if (allDigitalWrite(VIBRO_MOTOR_CONTROL, level)) {
    log_e("NOTIFY: motor %s write failed once, retry OK", level ? "ON" : "OFF");
    return true;
  }
  log_e("NOTIFY: motor %s write FAILED twice%s", level ? "ON" : "OFF",
        level ? "" : " - the motor may still be running");
  return false;
}

static void notifyMessageArrived(uint8_t mode) {
  /* `mode` is this KIND of arrival's own setting from Settings > Notifications — a text and
   * a mesh message are separately configurable, because muting one used to mute the other.
   *
   * ⚠ ONE log_e SURVIVES, DELIBERATELY. "It did not vibrate" was reported three times and
   * guessed at three times; the line that settled it was this path saying what it chose and
   * why. It fires once per arrival, which is rare enough to be worth keeping — and `mode`
   * is the answer to the most common report, since VIBRATE_ONLY looks exactly like a broken
   * chirp. */
  if (mode == ControlState::RINGER_SILENT) {
    log_e("NOTIFY: silent (mode=%d) - badge only", (int)mode);
    return;
  }
  const bool callBusy = (gui.state.sipState == CallState::Call);
  /* THE MOTOR GOES FIRST, before the pop — deliberately reordered. The buzz is a plain
   * GPIO write through the I2C extender; the pop reconfigures and drives the audio codec.
   * If the pop's I2C traffic can ever disturb the extender (the standing suspicion behind
   * "it chirped but never buzzed"), buzzing first removes that whole failure class instead
   * of measuring it. Costs nothing: the pulse runs for the configured Buzz length and the pop starts within a
   * millisecond of it either way.
   *
   * "Vibrate only" still buzzes — that is the whole difference from Silent.
   *
   * ⚠ A SECOND MESSAGE RE-ARMS THE TIMER; IT DOES NOT GET SKIPPED. Guarding this on
   * `!meshVibroActive` silently ate any notification that landed inside the previous pulse
   * pulse, which is exactly what {user} saw: "only heard one buzz out of 3 sent texts", and
   * the log agreed — buzz, SKIPPED, buzz. Restarting the pulse is also what the original
   * code did before the guard was added, and it degrades the right way: a burst of texts
   * becomes one longer buzz rather than one short buzz and silence. */
  if (!gui.state.ringing) {
    meshVibroActive = true;              // even if the HIGH is refused: the teardown's LOW is the safe state
    meshVibroLenMs = MESH_VIBRO_MS;
    motorWrite(HIGH);
    /* 🛑 THE STAMP IS TAKEN HERE, FROM THE CLOCK, NOT PASSED IN. This function used to receive
     * `now` from the top of the main-loop pass, and the mesh arrival reaches it AFTER
     * meshService.loop() — which the health log shows stalling the pass 0.5-1.5 s, 83 times in
     * one retained log, while nobody was touching the phone (blamed on the DB save at the time;
     * they were LoRa transmits, the old blocking send — gone since 0.9.79). The next pass then
     * compared its fresh clock against a stamp already up to 1.5 s old and switched the motor
     * off a few milliseconds after it had started. That was "sometimes the motor just barely
     * starts"; the SIP mirror path, which stamped millis() itself, was the one that behaved.
     * The teardown compares against millis() too, for the same reason. */
    meshVibroStartMs = millis();         // re-arm, so back-to-back arrivals keep it buzzing
    log_e("NOTIFY: buzz %u ms (mode=%d)", (unsigned)MESH_VIBRO_MS, (int)mode);
  } else {
    /* ⚠ SAY SO. The one log line proving this path ran used to sit INSIDE the gate, so a
     * `ringing` flag left latched by any teardown path would kill every later buzz with no
     * trace at all — indistinguishable from the motor being broken. If this line ever shows
     * while the phone is plainly not ringing, the latched flag is the fault to chase. */
    log_e("NOTIFY: suppressed (ringing latched)");
  }
  if (mode == ControlState::RINGER_RING_AND_VIBRATE) {
    /* The pop is genuinely one-at-a-time — it is a PCM file playing through the codec, and
     * restarting it mid-play is how the audio device gets left in the wrong mode (see
     * Audio::preserve()). A burst therefore gets one sound and one long buzz, which is the
     * right shape: you do not want five chirps, but you do want to feel every arrival. */
    /* ...and never while the emulator owns the device: playPop() reinstalls I2S for its
     * 8 kHz mono, under the emu thread's blocking i2s_write — the game goes silent for good
     * (review, 2026-09-19). The mesh block is skipped while gGbcActive, but the SIP text, the
     * mirror and serial `notify` are not; the buzz still happens, only the chirp is held. */
    if (!(callBusy || gui.state.ringing || meshPopPlaying || gGbcActive)) {
      /* Timed, because this call used to hide a SPIFFS open measured at 1.2-1.6 s — with
       * the motor already running, so a chirp made the buzz a second long. The pop now comes
       * from pop_pcm[] in flash and the file is only the fallback; the line says which, and
       * a "took" in the hundreds of ms names the codec/I2S start, not the file. */
      const uint32_t t0 = millis();
      const bool cutsMusic = audio->musicPlaying();   // BEFORE playPop() ceases it
      const bool played = audio->playPop(pop_pcm, sizeof(pop_pcm), &SPIFFS, gui.state.notifyVolume);
      const uint32_t t1 = millis();
      if (played) {
        const bool fromFlash = audio->popFromMemory();
        meshPopPlaying = true;
        meshPopCutMusic = cutsMusic;
        /* 🛑 THE STOP WAITS FOR THE RING'S LEAD (review 3, A1; notify_timing.h). playPop() installs
         * a FRESH ring on almost every pop, and a fresh ring plays itself (zeros) once before the
         * first written sample reaches the DAC: the chirp was due at ~512 ms and the teardown at
         * 360 ms zeroed it first - silence, except when a stall pushed the teardown late. The
         * flash buffer waits the whole trip whatever state the ring was in (a RUNNING ring's lead
         * is under a buffer only once it has idled three: a burst found it drained); the SPIFFS
         * fallback waits none (its open ran the trip before t1). The early finishes (the ring, a
         * call, a track, a game) still cut it whenever they come. */
        const uint32_t leadMs = audio->popLeadMs(!fromFlash);
        meshPopStopMs  = notifyPopTimerMs(sizeof(pop_pcm), NOTIFY_POP_RATE_HZ, NOTIFY_POP_MARGIN_MS,
                                          !fromFlash, leadMs);
        meshPopStartMs = t1;             // AFTER the start, for the motor stamp's reason
        /* ⚠ PUMP ONCE NOW (review, 2026-09-19). playPop() only arms the source; the samples reach
         * I2S in audio->loop() at the BOTTOM of the pass, and the text paths (SIP, the mirror,
         * serial `notify`) announce ABOVE the mesh block. On a RUNNING ring that has idled three
         * buffers this queues the chirp at once, in the next buffer to play. ⚠ On a FRESH ring
         * (the usual case) it queues NOTHING: the free-buffer queue is empty until the first
         * buffer has played out (~128 ms), and the first write after that - whichever pass comes
         * next - puts the chirp in a buffer that plays one trip after the start. Hence the lead.
         * ⚠ A STALL AFTER THAT WRITE cannot cut the chirp, only the padding after it: the buffer
         * is the DMA's now. A STALL BEFORE IT STILL CAN (review 3's repair): a pass held from
         * here until past ~0.77 s after the start - a stall of ~0.65 s or more - loses it. The
         * write then lands in a buffer that comes round after the 872 ms stop, and from 872 ms on
         * the teardown, which sits ABOVE the bottom-of-loop pump, zeroes the ring before the
         * chirp is ever written. This pump runs too early to help. The SIP and mesh arrivals do
         * their SPIFFS work (saveMessage, the mesh DB) before this call, not after it. */
        audio->loop();
        log_e("NOTIFY: pop start took %lu ms (%s, %s ring, lead %lu ms, stop at %lu ms)",
              (unsigned long)(t1 - t0), fromFlash ? "flash" : "SPIFFS fallback",
              audio->popRingState() == NOTIFY_RING_FRESH ? "fresh" :
              (audio->popRingState() == NOTIFY_RING_RESTARTED ? "restarted" : "running"),
              (unsigned long)leadMs, (unsigned long)meshPopStopMs);
      } else {
        log_e("NOTIFY: pop NOT started (%lu ms): %s", (unsigned long)(t1 - t0),
              audio->popError() ? audio->popError() : "no reason recorded");
      }
    }
  }
}


extern void gbcXferHandleClient();   // ROM-transfer web server pump (no-op when off)
extern bool gbcXferOn();             // the UPLOADER is up; the gates below use xferServing()

/* ── RAISE THE CLOCK BEFORE THE WORK, NEVER AFTER ─────────────────────────────────
 * The frequency gate lives at the BOTTOM of loop(), which is correct for deciding to
 * come DOWN but useless for going UP: a keypress is dispatched and the screen repainted
 * hundreds of lines earlier, so the one frame a person actually perceives would render
 * at the low clock and the boost would arrive after it was needed. Moving the gate is
 * NOT the fix - it would invert the wake path, which deliberately repaints before the
 * gate is reached.
 *
 * So the gate keeps its place and its down-path, and this raise-only helper runs at the
 * moment a key is dequeued. It goes through cpu_clock.cpp like the gate, so the two can never
 * disagree about what the hardware is doing. Raising only: nothing here can ever put the
 * phone into a lower state than the gate chose.
 *
 * ⚠ IF THIS IS EVER RE-ENABLED, READ THE DEADLOCK NOTE ON THE `busy` PREDICATE FIRST.
 * Under `cpu method old` a RAISE is a setCpuFrequencyMhz() like any other, so it fires
 * uart_on_apb_change() and can park the loop task forever while the plate's GPS is streaming at
 * 115200 (method new fires no APB callback). It is safe today only because gGpsNmea is in
 * `busy`, which pins the clock at full speed, so this finds nothing to raise. That is
 * load-bearing, not incidental. */
__attribute__((unused)) static void cpuRaiseForUi() {
  cpuClockRaise("key");
}

/* ── LOOP PHASE TRACKER ────────────────────────────────────────────────────────────────────
 * The stall detector below says HOW LONG a pass took and never WHICH PART of it took the time,
 * which is the difference between "62 stalls happened" and "this call blocks". Each major block
 * of the superloop names itself on the way in; the detector then reports the SLOWEST block of
 * the pass that just ended.
 *
 * Deliberately as dumb as it looks: a pointer store and one millis() per phase, no allocation,
 * no formatting, nothing that can itself block — an instrument that can stall is worse than no
 * instrument. The name must be a STRING LITERAL (it is held as a bare pointer across passes,
 * never copied), and `phaseWorst` is read at the top of the next pass, so nothing here may
 * reset it except the detector itself.
 */
static const char* gPhaseNow      = "boot";
static uint32_t    gPhaseAtMs     = 0;
static const char* gPhaseWorst    = "-";
static uint32_t    gPhaseWorstMs  = 0;

static inline void loopPhase(const char* name) {
  const uint32_t t = millis();
  if (gPhaseAtMs) {
    const uint32_t d = t - gPhaseAtMs;
    if (d > gPhaseWorstMs) { gPhaseWorstMs = d; gPhaseWorst = gPhaseNow; }
  }
  gPhaseNow  = name;
  gPhaseAtMs = t;
}

/* ── THE HEALTH LINE, OUT OF loop()'S OWN FRAME (review F1, 0.9.79) ─────────────────────────
 * 🛑 NOINLINE, AND THAT IS THE POINT. The two line buffers here (hl[256], kl[176]) have their
 * addresses passed to snprintf, so while they lived inline in loop() the compiler reserved all
 * 432 bytes in loop()'s frame for the WHOLE pass — under every call loop() makes, including
 * the deepest chain in the firmware: a Books picture page (GUI::redrawScreen -> drawPage 1,120
 * -> drawOneImage -> jpegGreyDecode 4,928 -> greyBlitRows -> pushImage) is ~7.6 KB of the 8 KB
 * loop task on its own, and HANDOFF records a 408 B loop-task floor measured before 0.9.79.
 * MEASURED with objdump (`entry a1, N`): loop() was 560 B on main and 640 B once hl grew 200 ->
 * 256 for this branch's fields; with the buffers here it is 240 B, and this helper is 592 B —
 * which exists only for the moment it runs, under nothing deeper than healthLogLine() (80) and
 * its SD open (FatFS keeps its 512 B long-name buffer on the stack) or trim (608): ~3 KB from
 * the task's top, against ~7.2 KB for the picture page, which is now 400 B shallower.
 * ⚠ Keep it that way: BATTERY_UPDATE_EVENT and the low-battery power-off stay in loop() (the
 * caller) on purpose — they reach app redraws and the book/map/game saves, and inside this
 * helper those would run on top of hl and kl again. And never let the compiler inline it
 * (a `static` function with one caller is exactly what -Os inlines). */
static void __attribute__((noinline)) healthLineTick(uint32_t now, float soc, float v) {
  /* ── HEALTH LINE ────────────────────────────────────────────────────────────
   * One line a minute, at log_e so it is visible in the field. It answers both of
   * the questions that keep coming up, over time rather than in a snapshot:
   *
   *   heap/min   a LEAK shows as `min` sliding down over hours. A steady min with
   *              occasional dips is just normal churn.
   *   largest    fragmentation: `free` can look healthy while the biggest single
   *              block is too small for the WiFi PHY, which is how this phone
   *              rebooted over a book.
   *   soc/v      the drain rate, which is the only honest way to tell whether a
   *              power change actually helped.
   *   up         minutes since boot -- an unplanned restart resets it. */
  static uint32_t s_minHeapEver = 0xFFFFFFFF;
  const uint32_t fh = ESP.getFreeHeap();
  if (fh < s_minHeapEver) {
    s_minHeapEver = fh;
  }
  /* ⚠ hl[] IS NEARLY FULL. Worked out 2026-09-25 (0.9.79) from the formats and real
   * health.log lines: the base line ~170, + wdis/cr/ssf ~28, + pll/csw ~22, + clk= ~12
   * (mesh/NNNN) = ~232 typical, ~253 with five-digit counters everywhere. The appends below
   * each re-measure strlen(), and clk= (the last) is dropped whole rather than cut. The NEXT
   * field needs a bigger hl — it is still on the 8 KB loop task (this helper's frame, only
   * while it runs: see above), so read the "loop-task stack floor" (serial `heap`) first. */
  char hl[256];
  wifi_ps_type_t hlPs;
  int hlLen = snprintf(hl, sizeof(hl),
           /* ⚠ aud= IS THE INSTRUMENT FOR THE AUDIO LEAK, and it is two numbers because one
            * would not have found it: `powered` alone cannot tell a song playing from a
            * codec left on after one stopped. `aud=1/0` — powered, moving no samples — IS
            * the leak signature, and it is the state that reads as a perfectly idle phone
            * everywhere else in this line (cpu=80MHz, scr=0, sip=1). */
           "HEALTH up=%lumin heap=%u min=%u largest=%u psram=%u soc=%d%% est=%d%% v=%.2f chg=%d wifi=%d cpu=%luMHz scr=%d sip=%d aud=%d/%d ps=%d joins=%lu/%lu",
           (unsigned long)(now / 60000), fh, s_minHeapEver, ESP.getMaxAllocHeap(),
           /* est= is ALWAYS the curve, never the displayed value: on the charger the
            * display shows the chip, and logging that twice would throw away the one
            * number a future charge-side correction needs. */
           ESP.getFreePsram(), (int)round(soc), batterySocFromVoltage(v), v,
           (int)gui.state.battCharged, (int)WiFi.status(),
           (unsigned long)getCpuFrequencyMhz(),
           (int)gui.state.screenBrightness, (int)gui.state.sipState,
           audio ? (int)audio->isOn() : 0, audio ? (int)audio->movingSamples() : 0,
           /* ps=1 is MIN_MODEM (what we want), 0 is NONE (receiver at full power),
            * -1 means the station is not up so the question does not apply. */
           (esp_wifi_get_ps(&hlPs) == ESP_OK) ? (int)hlPs : -1,
           /* joins=tried/skipped. Out of range these are THE numbers: `tried` should
            * barely move once the scan gate has evidence, and `skipped` should climb in
            * its place — each skip is ~30 s of radio not spent on empty air. */
           (unsigned long)wifiState.joinsTried(),
           (unsigned long)wifiState.joinsSkipped());
  /* wdis=<disconnect events>/<last reason> cr=<of them answered by the core's own begin()>
   * ssf=<runs of refused scan starts>/<last esp_err hex>. The reason is the number HEALTH
   * never had: `wifi=` is only the core's lossy summary of it (wifi_diag.h). */
  if (hlLen > 0 && hlLen < (int)sizeof(hl)) {
    wifiDiagHealth(hl + hlLen, sizeof(hl) - (size_t)hlLen);
  }
  /* pll=<the PLL the CPU runs from> csw=<same-PLL switches>/<PLL re-locks, radio off>/<PLL
   * re-locks WITH THE RADIO ON>. The third is the event that broke phone 1's WiFi 4 of 4;
   * under `cpu method new` it stays 0 for the whole boot (cpu_clock_policy.h). */
  hlLen = (int)strlen(hl);
  if (hlLen < (int)sizeof(hl) - 1) {
    cpuClockHealth(hl + hlLen, sizeof(hl) - (size_t)hlLen);
  }
  /* clk=<source>/<minutes since it set the clock> (0.9.79): none | ntp | gps | mesh. The
   * log's own timestamps are only as good as this, so a day's health.log can now say which
   * stretches were stamped from GPS, from the mesh, or from nothing at all.
   * ⚠ LAST ON THE LINE, AND WHOLE OR NOT AT ALL. hl[] is nearly full (see its declaration),
   * and snprintf truncates silently: a cut "clk=mesh/12" reads "clk=mesh/1" — a different,
   * perfectly plausible number. So a field that did not fit is rolled back to nothing;
   * a missing clk= (as opposed to clk=none) means the line ran out of room. */
  {
    const size_t used = strlen(hl);
    if (used + 1 < sizeof(hl)) {
      const size_t room = sizeof(hl) - used;
      int n;
      if (ntpClock.isTimeKnown()) {
        n = snprintf(hl + used, room, " clk=%s/%lu",
                     clockSourceName(ntpClock.getSource()),
                     (unsigned long)(ntpClock.msSinceSet(millis()) / 60000u));   // not `now`: a set this pass is later than it
      } else {
        n = snprintf(hl + used, room, " clk=none");
      }
      if (n < 0 || (size_t)n >= room) {
        hl[used] = '\0';           // did not fit whole: drop it rather than log a cut number
      }
    }
  }
  log_e("%s", hl);

  /* ⚠ The CARD gets a line a minute, not one every fifteen seconds like the console.
   * The battery poll runs at 15 s (BATTERY_CHECK_PERIOD_MS) and hanging the SD write
   * off it meant 240 open-write-close cycles an hour — measurable drain from the very
   * instrument meant to measure drain. Serial is free, so it keeps the faster rate;
   * the card does not. */
  static uint32_t s_lastCardLog = 0;
  if (s_lastCardLog == 0 || now - s_lastCardLog >= 60000u) {
    s_lastCardLog = now;
    healthLogLine(hl);

    /* Keypad health rides the same minute tick, but ONLY when something moved.
     * Dropped keypresses happen while a person is holding the phone and nobody is
     * watching a terminal, so the numbers have to reach the card; and a KEYS line
     * every minute forever would just push the interesting hours off the end of a
     * capped log. Silence here means the keypad had a clean minute. */
    char kl[176];
    int n = snprintf(kl, sizeof(kl), "KEYS ");
    keypadHealth(kl + n, (int)sizeof(kl) - n);
    static char s_lastKeys[176] = {0};
    if (strcmp(kl, s_lastKeys)) {
      strlcpy(s_lastKeys, kl, sizeof(s_lastKeys));
      log_e("%s", kl);
      healthLogLine(kl);
    }
  }
}

void loop() {
  while (1) {
    esp_task_wdt_reset();          // DIAGNOSTIC: see setup(). loop() never returns, so the
                                   // Arduino core's own feed never runs — this is the feed.
    uint32_t now = millis();
#ifdef USER_SERIAL
    /* The deferred GPS retune. First pass only, and deliberately HERE rather than at the
     * end of setup(): this is the exact path that has always survived — a live 115200
     * line meeting updateBaudRate() rather than esp_intr_alloc(). See setup(). */
    if (gGpsBaudPending) {
      gGpsBaudPending = false;
      gpsApplyBaud(true);
      gGpsReader.reset();        // the boot's worth of 9600-misframed bytes is not data
      log_e("GPS: NMEA reader ON (user UART %d/%d @ %u) - retuned from loop()",
            USER_SERIAL_RX, USER_SERIAL_TX, (unsigned)gGpsBaud);
    }
#endif
    /* ── SUPERLOOP STALL DETECTOR ──────────────────────────────────────────────────────────
     * {user}, 2026-08-24: "sometimes when scrolling menus, the phone will freeze for a second
     * or two, wifi will drop, then it will unfreeze and WiFi comes back up." Everything in
     * this firmware shares ONE task, so anything that blocks here blocks the keypad, the
     * screen AND the WiFi stack together — which is exactly that symptom, and exactly what
     * the sms-mirror comment below already warns about ("not slow, it is the 5-second freeze
     * bug rebuilt on purpose").
     *
     * A freeze that rare cannot be caught by watching; it has to be caught by the phone. This
     * measures the gap between successive passes — i.e. how long the PREVIOUS pass took — and
     * names it at the compiled-in level so it lands in the serial log and can be correlated
     * with whatever else was happening. Costs one millis() compare per pass.
     * 250 ms is well above any normal pass (the idle tick is 5 ms) and well below the "second
     * or two" being chased, so it should be silent until something real happens. */
    {
      static uint32_t sLastPassMs = 0;
      if (sLastPassMs) {
        const uint32_t took = now - sLastPassMs;
        if (took > 250) {
          log_e("LOOP STALL: %u ms in one pass, worst block '%s' %u ms "
                "(scr=%d cpu=%uMHz wifi=%d) - WiFi/keypad/screen were all frozen for this long",
                (unsigned)took, gPhaseWorst, (unsigned)gPhaseWorstMs,
                (int)gui.state.screenBrightness,
                (unsigned)(getCpuFrequencyMhz()), (int)WiFi.status());
          /* ⚠ AND INTO /health.log, because serial only helps when somebody is watching it.
           * The freeze this exists to catch happens while {user} is USING the phone — walking
           * around, off the cable — and a diagnostic that only fires for a tethered developer
           * would miss every real occurrence. Same argument as the health line itself.
           * Rate-limited to one record a minute: a phone that stalls repeatedly must not spend
           * its stalls writing about stalling. */
          static uint32_t sLastStallLogMs = 0;
          if (!sLastStallLogMs || (uint32_t)(now - sLastStallLogMs) > 60000) {
            sLastStallLogMs = now;
            char stallLine[96];
            snprintf(stallLine, sizeof(stallLine), "STALL %ums %s %ums scr=%d cpu=%u wifi=%d",
                     (unsigned)took, gPhaseWorst, (unsigned)gPhaseWorstMs,
                     (int)gui.state.screenBrightness,
                     (unsigned)(getCpuFrequencyMhz()), (int)WiFi.status());
            healthLogLine(stallLine);
          }
        }
      }
      sLastPassMs = now;
      /* The pass just reported on is over: start the next one's worst-block search clean.
       * Only the detector may clear this — see the tracker's note above. */
      gPhaseWorstMs = 0;
      gPhaseWorst   = "-";
      loopPhase("top");
    }
    /* The mesh database save blocks this task (~1.5-2.8 s on SPIFFS, ~0.1 s on the card), so it
     * is held off while the phone is being used — see MeshtasticService::setUiIdle(). Three
     * seconds after the last key, the user has stopped scrolling and a freeze there costs
     * nothing. This is the only place that can see both the keypad and the mesh service.
     * ⚠ The 0.6-1.5 s STALLs named 'mesh' were LoRa transmits, not this (mesh_txq.h). */
    loopPhase("mesh-uiidle");
    meshService.setUiIdle(gLastKeyMs == 0 || (uint32_t)(now - gLastKeyMs) > 3000);
    meshService.setCardPresent(gui.state.cardPresent);   // the DB lives on SD when there is one
    loopPhase("gbc-xfer");
    gbcXferHandleClient();
    loopPhase("serial-cmd");
    serialCmdLoop();       // USB console: `?` for help. Costs nothing when nothing is typed.

    /* Pull mirrored texts from COVEY over the LAN — one bounded step per pass, never a
     * blocking request. See sms_mirror_poll.h: the UI is one task, so a blocking GET here is
     * not "slow", it is the 5-second freeze bug rebuilt on purpose.
     *
     * ⚠ Called UNCONDITIONALLY, with sipMayPoll() passed IN. Wrapping the call in that gate
     * instead looked equivalent and was not: while the Games app is open the gate is false,
     * so the poller could not read a config file that had just been uploaded — and reported
     * nothing about it, because the reporting was inside the skipped call. The socket is the
     * only part that needs gating. */
    loopPhase("sms-mirror");
    smsMirrorPollLoop(sipMayPoll());

    /* KOSync (kosync_sync.h): the window's hard deadline and health line, a peer's place
     * parked into the booksync inbox, the automatic windows asked for by a book close/open,
     * and one bounded step of the home client. One comparison when none of it is in use.
     * Same socket gate as the mirror; and no automatic window into a live or ringing call. */
    loopPhase("kosync");
    kosyncLoop(sipMayPoll(), sipNeedsFullSpeed());

    /* The hotspot phone's GPS (phone_pos.h): one bounded, non-blocking step at most every
     * 100 ms - a connect polled, or up to 512 B read and fed. Same socket gate as the two above
     * (closed under a Game Boy game). 🛑 Deliberately NOT in `busy`/`hardBusy` below: a 1 Hz
     * trickle needs no full speed, and a clock switch under a running radio is phone 1's WiFi
     * drop. It never sets the clock and never touches gGpsReader. */
    loopPhase("phonepos");
    phonePosLoop(sipMayPoll());

    /* THE MIRROR'S ANNOUNCER — one place, both transports. smsMirrorIngestLine() latches
     * news whenever it stores a text (whether the line rode in over LoRa or the LAN poll
     * above), and this takes it. The split serves two different urgencies:
     *   - the BUZZ is immediate: feeling the arrival late is the same as not feeling it;
     *   - the UI EVENT is coalesced (~700 ms): each NEW_MESSAGE_EVENT makes an open
     *     Messages screen rebuild its snapshots — four 120-deep paged scans — so a
     *     catch-up burst of records must become a handful of rebuilds, not one per text.
     * Before this, the LAN path announced NOTHING (no buzz, no event — a text stored in
     * silence), and a mirrored text arriving while a thread was open stayed invisible
     * until you backed out and re-entered — at which point the sort dropped it mid-list,
     * muddying the ordering bug's report. */
    {
      loopPhase("mirror-ui");
      static bool     s_mirrorUiPending = false;
      static uint32_t s_mirrorUiLastMs  = 0;
      bool inbound = false;
      if (smsMirrorTakeNews(&inbound)) {
        s_mirrorUiPending = true;
        if (inbound) {
          smsMirrorNotifyArrival();
        }
      }
      if (s_mirrorUiPending && elapsedMillis(now, s_mirrorUiLastMs, 700)) {
        s_mirrorUiLastMs = now;
        s_mirrorUiPending = false;
        appEventResult res = gui.processEvent(now, NEW_MESSAGE_EVENT);
        gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
      }
    }

    /* 🔎 RATCHET WATCHPOINT — name the MOMENT `largest` steps down.
     *
     * `largest` is the number that predicts the abort; free and min disagree with it and
     * predict nothing. It steps down permanently rather than drifting, so catching the step
     * — and what the phone was doing at that instant — is the whole game. The HEALTH line
     * samples every 15 s, far too coarse: it says the block shrank somewhere in the last
     * quarter minute but never says during what.
     *
     * Polled at 250 ms, and prints ONLY on a real drop, so an idle phone logs nothing. The
     * high-water mark follows recoveries upward so the next genuine dip still registers.
     *
     * ⚠ heap_caps_get_largest_free_block() walks the free list, so this is not free. It is a
     * DIAGNOSTIC: delete it, or raise DROP_THRESH, once the ratchet is understood. This is the
     * same trap as the health log itself — an instrument that consumes what it measures. */
    {
      /* Downshifted 250 ms → 2 s and 512 → 1024 B now the ratchet is understood and 0.9.3
       * has soaked: the free-list walk four times a second was itself a measurable cost
       * (the handoff's own instruction was to delete it or raise the threshold once
       * trusted). At 2 s it still names the moment of any real step within the window the
       * HEALTH line could never resolve, for an eighth of the walks. */
      loopPhase("heap-watch");
      const uint32_t DROP_THRESH = 1024;
      static uint32_t s_lastLargestCheck = 0;
      static uint32_t s_largestHigh = 0;
      if (elapsedMillis(now, s_lastLargestCheck, 2000)) {
        s_lastLargestCheck = now;
        /* Association marks, detected HERE rather than in processWiFiEvent(): that handler
         * runs on the WiFi event task, and heapEvent() writes to the SD card. Polling the
         * status from the loop at 2 s gets the same transitions somewhere it is safe to
         * touch the filesystem. WiFiUDP::begin() used to fire on exactly this edge. */
        static int s_lastWifiStatus = -1;
        const int ws = (int)WiFi.status();
        if (ws != s_lastWifiStatus) {
          if (s_lastWifiStatus >= 0) {
            wifiMarkLink(s_lastWifiStatus, ws, now);   // card: one pair a minute (review F3)
          }
          s_lastWifiStatus = ws;
        }
        const uint32_t lg = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (s_largestHigh == 0 || lg > s_largestHigh) {
          s_largestHigh = lg;
        } else if (lg + DROP_THRESH < s_largestHigh) {
          log_e("DROP largest %u->%u (-%u) app=%d sip=%d wifi=%d scr=%d cpu=%luMHz up=%lus",
                (unsigned)s_largestHigh, (unsigned)lg, (unsigned)(s_largestHigh - lg),
                (int)gui.currentAppId(), (int)gui.state.sipState, (int)WiFi.status(),
                (int)gui.state.screenBrightness, (unsigned long)getCpuFrequencyMhz(),
                (unsigned long)(now / 1000));
          s_largestHigh = lg;
        }
      }
    }

    /* Stale-held-key sweep, driven by the SN7326's ~40ms held-key re-reports
     * (LONGPRESS_DELAY(1)): a key silent for 350ms lost its release event.
     * If holds ever break rhythmically at ~350ms, the chip's re-report isn't periodic at
     * this setting — revert to DELAY(2) and 5200ms here.
     *
     * ⚠ BOTH MASKS, not just uiKeyDown. It used to clear keypadState in game mode only,
     * on the belief that keypadState was game-side state — but the UI press handler was
     * nested inside the keypadState edge, so a keypadState bit left set after a lost
     * release vetoed every subsequent press of that key. Half-sweeping was therefore
     * worse than not sweeping: it looked like the key had been rearmed when it had not.
     * The UI handler no longer consults keypadState, and this now clears both, so the two
     * masks cannot disagree about which keys are down. */
    /* ⚠ THE SWEEP'S PREMISE IS "WE WOULD HAVE SEEN A HEARTBEAT IF THERE HAD BEEN ONE",
     * and that is only true if we were LOOKING. This runs before the keypad drain in the
     * same pass, so after the loop blocks — an SD write, the uploader, a WiFi call — the
     * heartbeats of a genuinely held key are still sitting in the chip, unread, and
     * keyLastSeenMs is stale through no fault of the key. Sweeping on that evidence
     * releases a key the user is still holding, and the queued heartbeats then decode as
     * a fresh press: auto-repeat, the exact fault uiKeyDown was added to kill.
     *
     * So skip a pass that arrives late and let the drain below refresh the timestamps
     * first. One pass is enough — a single keyboardRead() empties the whole FIFO. */
    loopPhase("key-sweep");
    static uint32_t s_lastSweepMs = 0;
    const bool loopWasStalled = s_lastSweepMs && (now - s_lastSweepMs) > 100;
    s_lastSweepMs = now;
    for (uint32_t st = loopWasStalled ? 0 : (uiKeyDown | keypadState); st; st &= st - 1) {
      int b = __builtin_ctz(st);
      if (now - keyLastSeenMs[b] > 350) {
        /* Every trip through here is a RELEASE THAT WAS NEVER SEEN, and the key was deaf
         * for the whole 350ms getting here. This is the counter the keypad poll is meant
         * to drive to zero: with the poll working, releases are recovered within ~40ms
         * and the sweep should have almost nothing left to clean up. */
        if (uiKeyDown & (1u << b)) {
          kcStaleSwept++;
        }
        uiKeyDown   &= ~(1u << b);
        keypadState &= ~(1u << b);
        /* Deliberately NOT stamping keyLastUpMs here. The stall guard above is what keeps
         * a queued heartbeat from reading as a fresh press; using the bounce window for
         * that job instead would put a 40 ms hole right where the user, having had one
         * tap swallowed, is most likely to be tapping again. */
      }
    }

    /* Sleep the screen on Select + Back held together for two seconds. See the note by
     * SLEEP_CHORD_MS. Checked here, right after the stale-key sweep, so it reads a held-key
     * mask that is already up to date; nothing else in the loop has to know about it.
     *
     * Deliberately does NOT swallow the keys. Both are edge-triggered, so their presses were
     * dispatched the moment they went down and cannot be taken back — Back will have
     * navigated once. That is a fair price for a gesture that works every time, and the
     * screen going off is its own confirmation. */
    /* ── F2: next on a tap, previous on a hold ─────────────────────────────────────
     * Decided on RELEASE, because "short or long" is not knowable when the key goes
     * down — firing next immediately would make every hold skip forward first.
     *
     * Previous is musicPlayerPrev(), which does the thing every music player does:
     * within the first three seconds it goes to the previous track, after that it
     * restarts the current one.
     *
     * Read here, right after the stale-key sweep, so uiKeyDown is already up to date. */
    loopPhase("music");
    /* ⚠ ...and never during a call: Next/Previous start a track, which is the same theft of the
     * call's audio as F1 — see the transport block and Audio::playMusic(). */
    if (!gGbcActive && !(gMapsActive && !gui.state.locked) && !gui.inCall() &&
        musicPlayerCurrent() >= 0) {
      if (uiKeyDownOrBlip(WIPHONE_KEY_MASK_F2, msF2Down != 0)) {
        if (!msF2Down) {
          msF2Down = now;
          f2Fired = false;
        } else if (!f2Fired && now - msF2Down >= MUSIC_F2_HOLD_MS) {
          f2Fired = true;                  // one action per hold, however long it lasts
          musicPlayerPrev();
        }
      } else {
        if (msF2Down && !f2Fired) {
          musicPlayerNext();               // released before the threshold: a tap
        }
        msF2Down = 0;
        f2Fired = false;
      }
    } else {
      msF2Down = 0;
      f2Fired = false;
    }

    /* ── HOLD A DIGIT TO TYPE THE DIGIT ────────────────────────────────────────────
     * Only while a text field that uses predictive text is focused, and never in 123 mode
     * where a short press already types the digit — firing there would type it twice.
     *
     * ⚠ THE SHORT PRESS HAS ALREADY HAPPENED. Keys are edge-triggered and dispatched on
     * key-down, so by the time this fires the digit is already in the pending word (or has
     * started a multi-tap letter). t9LiteralDigit() reverses that rather than trying to
     * predict the future at key-down time, which would mean holding EVERY digit back until
     * release and putting a hold's worth of lag on ordinary typing.
     *
     * Read here, right after the stale-key sweep, so uiKeyDown is already up to date —
     * same placement and same reason as the F2 hold above. */
    /* ⚠ GATED ON LOCKED AND SCREEN-OFF, like the F2 hold above. uiKeyDown is set by the
     * keypad scan whether or not the press ever reached the app, and GUI::processEvent zeroes
     * the event when the phone is locked or the screen is asleep — so without this, a hold
     * during either state fired anyway. Holding '0' on a locked draft queued a BACKSPACE and
     * a '0' straight into the text field: the last character you really typed was deleted and
     * replaced, invisibly, behind the lock screen. */
    if (!gui.state.locked && gui.state.screenBrightness > 0 &&
        gui.state.t9Available() && gui.state.inputMode != ControlState::MODE_123) {
      static const uint32_t digitMask[10] = {
        WIPHONE_KEY_MASK_0, WIPHONE_KEY_MASK_1, WIPHONE_KEY_MASK_2, WIPHONE_KEY_MASK_3,
        WIPHONE_KEY_MASK_4, WIPHONE_KEY_MASK_5, WIPHONE_KEY_MASK_6, WIPHONE_KEY_MASK_7,
        WIPHONE_KEY_MASK_8, WIPHONE_KEY_MASK_9,
      };
      /* ⚠ '1' IS SKIPPED IN T9 MODE, and '0' is a special case.
       * Both already type themselves on the SHORT press — '1' a one, '0' a space — so by
       * the time a hold fires the character is in the widget, and t9LiteralDigit() reverses
       * the prediction buffer, not the text. For '1' that is fine: a short press already
       * gives exactly what a hold would, so the gesture has nothing to add and firing would
       * only type it twice. For '0' it is NOT fine — a short press gives a space, so
       * without a hold there is no way to type a zero in T9 at all (field report). It is
       * handled by retracting the space with a backspace; see t9LiteralDigit().
       *
       * In Abc/ABC nothing has been emitted yet — the press is sitting in a multi-tap cycle
       * waiting to commit — so every digit is reversible there and none of this applies. */
      const bool t9Mode = (gui.state.inputMode == ControlState::MODE_T9);
      int8_t nowHeld = -1;
      for (int8_t d = 0; d < 10; d++) {
        if (t9Mode && d == 1) {
          continue;
        }
        if (uiKeyDownOrBlip(digitMask[d], msDigitDown != 0 && digitHeld == d)) {
          nowHeld = d;
          break;
        }
      }
      if (nowHeld >= 0) {
        if (!msDigitDown || digitHeld != nowHeld) {
          msDigitDown = now;              // a different key: re-arm rather than inherit
          digitHeld = nowHeld;
          digitFired = false;
        } else if (!digitFired && now - msDigitDown >= T9_DIGIT_HOLD_MS) {
          digitFired = true;
          /* Retract only for '0' in T9 mode: the one case where the short press has
           * already committed a character (a space) that the hold has to undo. */
          gui.state.t9LiteralDigit((char)('0' + nowHeld), t9Mode && nowHeld == 0);
          /* The event loop has to be poked so the queued digit is delivered, but
           * redrawWhat does not exist this early in the pass — so raise a flag and let the
           * dispatch below do it. */
          digitLiteralQueued = true;
          /* The footer still shows the digits the SHORT press added; the engine has already
           * dropped them. Without this the strip contradicts the prediction until the next
           * keypress. The '#' hold below already does this. */
          redrawWhatPending = true;
        }
      } else {
        msDigitDown = 0;
        digitHeld = -1;
        digitFired = false;
      }
    } else {
      msDigitDown = 0;
      digitHeld = -1;
      digitFired = false;
    }

    /* ── HOLD '#': ONE CAPITAL IN A TEXT FIELD, THE MUTE EVERYWHERE ELSE ──────────────
     * In a predictive-text field, the half of capitalisation no rule can do: a name in the
     * middle of a sentence. A one-shot, consumed by the next committed word, so it reads as
     * a shift key rather than as another mode to get stuck in.
     *
     * ⚠ A HOLD DOES NOT SWALLOW THE PRESS — the same warning the sleep chord carries. '#'
     * is edge-triggered, so its short press has already cycled the input mode by the time
     * this fires. Stepping the mode back by one is not a workaround for that, it is the
     * only correct response: the user asked for a capital, not for a mode change.
     *
     * OUTSIDE A TEXT FIELD the same hold toggles the master mute (Settings > Mute all
     * sounds) — {user}, 2026-09-21: "holding the pound key mute and unmute. It already has an
     * icon on it that suggests muting... only when you're not entering text or when that
     * key isn't being used for something else." Where '#' is somebody's: a focused text
     * field of any kind (the dialer's number, a non-predictive field where '#' is shift),
     * and the map, whose '#' is zoom in. The Game Boy does not read '#', so a game can be
     * muted from the pad. Same gate as every hold here — awake and unlocked — and the same
     * 500 ms.
     *
     * 🛑 HERE THE PRESS IS SWALLOWED, unlike the capital above. On the clock and in the menus
     * a short '#' opens the dialer with a '#' in it (or Messages, when the envelope shows),
     * so a hold that let the press through would open the dialer AND mute — {user}: "maybe we
     * can stop that behavior if it does". So while the mute hold is armed, the key loop
     * below zeroes the press and owes it (hashDeferred); a release before the threshold
     * pays the debt (hashTapPending → the same processEvent the press would have had, a
     * hundred-odd ms late), the hold fires the mute instead and the debt is forgiven. The
     * F2 music key is decided on release the same way and for the same reason. */
    const bool hashCapital = gui.state.t9Available();
    const bool hashMute = !hashCapital && uiHashMuteArmed();
    if (!gui.state.locked && gui.state.screenBrightness > 0 && (hashCapital || hashMute)) {
      if (uiKeyDownOrBlip(WIPHONE_KEY_MASK_HASH, msHashDown != 0)) {
        if (!msHashDown) {
          msHashDown = now;
          hashFired = false;
        } else if (!hashFired && now - msHashDown >= T9_DIGIT_HOLD_MS) {
          hashFired = true;
          if (hashCapital) {
            gui.state.inputMode = (gui.state.inputMode + 3) & 3;   // undo the short press
            /* ARM, NEVER UN-ARM. A toggle reads as a coin flip to the user, who cannot see the
             * flag — and in practice it only ever produced `true` anyway, because the
             * unconditional reset in t9Commit guaranteed a false input. Asking for a capital
             * twice should give you a capital. */
            gui.state.t9Caps = true;
            /* Abc and ABC never read t9Caps — only the T9 path and the footer do — so in those
             * modes the gesture has to land on inputShift, which is the flag they DO read.
             * Otherwise holding '#' in Abc was a 500 ms no-op that also stepped the mode back. */
            if (gui.state.inputMode == ControlState::MODE_ABC) {
              gui.state.inputShift = true;
            } else if (gui.state.inputMode == ControlState::MODE_ABC_CAPS) {
              gui.state.inputShift = true;
            } else {
              gui.state.inputShift = false;
            }
            redrawWhatPending = true;
          } else {
            const bool muted = !gui.state.audioMuted;
            guiSetMuted(audio, gui.state, muted);      // applied now, stored in configs.ini
            log_e("MUTE: '#' held - %s", muted ? "MUTED" : "sounds on");
            /* One short buzz says it took, since the one thing a mute cannot do is beep.
             * The notification motor path: HIGH now, the teardown below LOWs it after
             * MUTE_BUZZ_MS (see meshVibroLenMs). Not while ringing — the ring owns the
             * motor — and not on top of a buzz already running. */
            if (!gui.state.ringing && !meshVibroActive) {
              meshVibroActive = true;
              meshVibroLenMs = MUTE_BUZZ_MS;
              motorWrite(HIGH);
              meshVibroStartMs = millis();
            }
            muteTogglePending = true;
            hashDeferred = false;                     // the press is forgiven, not owed
          }
        }
      } else {
        if (hashDeferred && !hashFired) {
          hashTapPending = true;                      // released early: it was a tap
        }
        msHashDown = 0;
        hashFired = false;
        hashDeferred = false;
      }
    } else {
      msHashDown = 0;
      hashFired = false;
      hashDeferred = false;       // the screen changed under a swallowed press: dropped
    }

    if ((uiKeyDown & SLEEP_CHORD_MASK) == SLEEP_CHORD_MASK) {
      if (!msChordStart) {
        msChordStart = now;
        chordFired = false;
      } else if (!chordFired && now - msChordStart >= SLEEP_CHORD_MS) {
        chordFired = true;               // one sleep per hold, however long it is held
        if (gui.state.screenBrightness > 0) {
          gui.sleepScreen();
        }
      }
    } else {
      msChordStart = 0;
      chordFired = false;
    }

    // DEBUG
    //uint32_t loopTime = micros();
    //if (!msProfileStart) msProfileStart = loopTime;

    appEventResult redrawWhat = DO_NOTHING;

    /* Deliver a digit that the hold timer above queued. Zero is a safe event here:
     * processEvent's pre-process guards alphanumericInputEvent on `event` being non-zero,
     * and its priming step pulls the character out of the queue instead. */
    if (digitLiteralQueued) {
      digitLiteralQueued = false;
      redrawWhat |= gui.processEvent(now, (EventType)0);
    }
    // A held '#' changed the pending capital; the footer has to say so.
    if (redrawWhatPending) {
      redrawWhatPending = false;
      redrawWhat |= REDRAW_FOOTER;
    }
    /* A '#' the key loop swallowed for the mute hold, released before the threshold: the
     * tap it always was, delivered now. Same call the key loop would have made. */
    if (hashTapPending) {
      hashTapPending = false;
      redrawWhat |= gui.processEvent(now, (EventType)'#');
    }
    /* The mute flipped under a held '#': the crossed speaker lives in the header — or on the
     * clock face, which draws its own icon row and has no header to ask.
     *
     * 🛑 NOT DURING A GAME. The emulator's blit task owns the LCD; a header push from this
     * task on top of it deadlocked the SPI and wedged loopTask — task watchdog, motor left
     * running, phone 2, 2026-09-21, the first time this fired inside a game. The buzz is the
     * whole confirmation there; the icon catches up when the game ends. */
    if (muteTogglePending) {
      muteTogglePending = false;
      if (!gGbcActive) {
        redrawWhat |= gui.isAppRunning(GUI_APP_CLOCK) ? REDRAW_SCREEN : REDRAW_HEADER;
      }
    }

    // Power button check
    if (gpioExtenderEvent) {
      if (gpioExtenderServiceInterrupt()) {
        if (powerButtonPressed) {
          msPowerOffStarted = now;
        } else if (poweringOff) {
          redrawWhat |= gui.processEvent(now, POWER_NOT_OFF_EVENT);
          poweringOff = false;
        }
      }
    }

    // Headphone check
    if (headphoneEvent) {
      headphoneServiceInterrupt();
    }

    // KEYPAD + TICK

    // Check if interrupt occured
    uint8_t toRead = keypadToRead;

    /* Drain the keypad FIFO periodically WITHOUT an interrupt as well. The chip's INT is
     * a 10ms PULSE that auto-clears whether or not anyone read it, so an event arriving
     * while INT is already low (or lost to an I2C error) sits in the FIFO with no edge
     * left to announce it — a missed tap, or a button stuck down until the next press.
     * See the long note by KEYPAD_POLL_MS for why one release in four is exposed to this.
     *
     * WHEN, not always: while a key is believed down — which is exactly when a release
     * may be stuck — and for a second after the last event. An idle phone does no I2C
     * here at all, so this costs nothing on the battery, which is the point.
     *
     * The game keeps its own unconditional 150ms cadence: it runs its own inner loop and
     * cares about held state rather than edges. */
    /* ⚠ THE UI POLL WAS TRIED AND IS RETIRED — MEASURED, NOT ASSUMED. It was added on the
     * theory that the chip's 10ms INT pulse was stranding events in the FIFO; the raw
     * trace refuted that outright, `drained` staying at 0 through whole sessions of menu
     * use while `empty` climbed into the hundreds. So it recovered nothing and spent I2C
     * traffic every 40ms to do it. Worse, it made things AMBIGUOUS: an empty FIFO and a
     * release both read back as 0x00, and only an interrupt proves an event really
     * happened — which is the discriminator the release fix now depends on.
     * The real fault was never transport; it was that the chip does not say WHICH key
     * came up. See the note by the code-0 release attribution above.
     * The game keeps its own 150ms drain: it reads held state rather than edges. */
    static uint32_t msLastKeyDrain = 0;
    bool polledRead = false;
    if (!toRead && gGbcActive && elapsedMillis(now, msLastKeyDrain, 150)) {
      toRead = 1;
      polledRead = true;
    }

    // Read all the keys to buffer
    if (toRead) {
      msLastKeyDrain = now;
      keyboardRead(polledRead);
    }
#ifdef USE_VIRTUAL_KEYBOARD
    keyboardUdpRead();
#endif

    // Process keys buffer
    EventType keyPressed;
    bool anyPressed = false;
    loopPhase("keys");
    while (!keypadBuff.empty()) {
      // Retrieve button from buffer safely
      keyPressed = keypadBuff.get();
      if (keyPressed) {
        gLastKeyMs = millis();     // see setUiIdle() below: holds off the DB save
      }
#if UI_IDLE_DOWNCLOCK
      if (keyPressed) {
        cpuRaiseForUi();     // ⚠ touches the input path — see UI_IDLE_DOWNCLOCK
      }
#endif


      // Process key
      Random.feed(rotate5(now) ^ keyPressed);

      // Shift old characters and remember current character
      for (uint8_t k = sizeof(lastKeys) - 1; k > 0; k--) {
        lastKeys[k] = lastKeys[k - 1];
      }
      lastKeys[0] = keyPressed;

      // Triple-tap the top-right (Back) button to sleep the screen. Only tracked
      // while the screen is awake, so a wake-up tap doesn't count.
      if (!anyPressed && gui.state.inputType == InputType::AlphaNum) {
        msLastKeyInput = now;
      }
      anyPressed = true;

      // Process key
#ifndef STEAL_THE_USER_BUTTONS
      /* ── The four user buttons are the music transport ──────────────────────────
       * F1 play/pause · F2 next (HELD = previous) · F3 louder · F4 quieter
       *
       * Handled here rather than in MusicApp so they work from any screen — which is
       * the point of playback that outlives its app. The keys are SWALLOWED when used
       * (keyPressed = 0), otherwise the screen underneath acts on them too.
       *
       * ⚠ Only while a track is loaded. Books uses F3/F4 to page and the Game Boy uses
       * F1/F2 for its own volume; taking them unconditionally would break both. With
       * nothing loaded these fall through exactly as before.
       *
       * ⚠ F2 is NOT handled here — it fires on RELEASE, next to the sleep chord, because
       * short-versus-held cannot be decided at the moment of the press. */
      /* Two different tests, because two different apps claim these keys.
       *
       * F1/F2 (play-pause, next-prev) are claimed by nothing else, so a LOADED track is
       * enough — that is what lets F1 restart a paused track from any screen.
       *
       * F3/F4 are Books' page keys, so volume only takes them while music is actually
       * PLAYING. Read a book with music paused and paging works exactly as it always
       * did; read one with music playing and the side buttons are volume, which is what
       * you want when there is sound coming out. */
      /* ...and the map (gMapsActive): the top two side buttons are its zoom, the third
       * "centre on me", the fourth the next map type. A zoom key that pauses the music
       * instead would be silently dead. */
      /* ⚠ ...but only while the map can TAKE a key: a locked or dark phone with the map still
       * open underneath must not leave the side buttons dead for music. */
      const bool mapOwnsKeys  = gMapsActive && !gui.state.locked;
      /* 🛑 ...and NOT DURING A CALL. The emulator and the map were guarded; a live call was not.
       * F1 (or F2) mid-call started a track: playMusic() replaced `playback` = RtpStream, so the
       * caller went silent for the rest of the call, and reinstalled I2S at the track's rate, so
       * the microphone packets went out 882 samples long (garble) or 1764 with headphones
       * (dropped: silence). The keys fall through to the call screen, which ignores them.
       * gui.inCall() is the live-or-ringing set; teardown states are not calls. */
      const bool musicLoaded  = !gGbcActive && !mapOwnsKeys && !gui.inCall() &&
                                musicPlayerCurrent() >= 0;
      const bool musicSounding = musicLoaded && musicPlayerIsPlaying();

      if (musicLoaded && keyPressed == WIPHONE_KEY_F1) {
        musicPlayerTogglePause();
        keyPressed = 0;
      }

      if (musicSounding && keyPressed == WIPHONE_KEY_F3) {
        musicPlayerVolumeStep(+1);
        keyPressed = 0;
      }

      if (musicSounding && keyPressed == WIPHONE_KEY_F4) {
        musicPlayerVolumeStep(-1);
        keyPressed = 0;
      }

      if (musicLoaded && keyPressed == WIPHONE_KEY_F2) {
        keyPressed = 0;      // consumed; the action fires on release or on the hold timer
      }

      /* '#' where a hold means mute: owed, not delivered — see the '#' hold tracker. */
      if (keyPressed == '#' && !gui.state.locked && gui.state.screenBrightness > 0 &&
          !gui.state.t9Available() && uiHashMuteArmed()) {
        keyPressed = 0;
        hashDeferred = true;
      }

      /* 🛑 END HANGS UP A CALL — AND ONLY A CALL. It set HangUp from EVERY screen, call or no
       * call, and HangUp is not a harmless label:
       *   - with WiFi and an account, the SIP block's HangUp branch ran stopRingtone() ->
       *     audio->shutdown(), stopping any music and losing its place; zeroed the volumes (the
       *     never-written restore* globals); and called terminateCall() with no call — log_e
       *     "currentCall not set", plus a stray CANCEL on a TCP account;
       *   - with no WiFi, or no SIP account (the firmware is public), nothing ever LEAVES HangUp,
       *     and the call-wins-over-music check counted it as a call, so after one END press the
       *     music could not be played at all until WiFi came back or the phone rebooted.
       * gui.inCall() is live or ringing (BeingInvited included, so END still rejects a ringing
       * call). The call screen's own END sees HangUp first and closes, exactly as before.
       * During a Game Boy session END is the pause-menu button; don't also poke the SIP state
       * machine (it popped call UI over the game / froze).
       *
       * 🛑 ...AND END IN CallState::Error RETRIES SIP, EXPLICITLY. Error is where sip.init()
       * lands when the proxy cannot be reached (booted on COVEY's no-internet hotspot, a captive
       * portal, a DNS miss on the first join), and NOTHING ever moves the state out of it by
       * itself — not a WiFi rejoin (that path only re-inits after a call it tore down), not
       * time. The old END-sets-HangUp-everywhere happened to be the only way out: Error ->
       * HangUp -> terminateCall() fails -> HungUp -> Idle, where checkCall() reconnects to the
       * proxy address resolved at boot (0.0.0.0 after a DNS miss, so not even that always
       * worked). Removing it left such a phone unregistered until a reboot or an account edit.
       * This keeps the escape without any of HangUp's side effects: NotInited runs one init()
       * on the next SIP pass with WiFi up — the boot path: the proxy's name is looked up again
       * (resolveDomain() caches a miss for 60 s, so a press within a minute of one fails at
       * once), then one 500 ms connect — and it goes back to Error if that fails. One attempt
       * per press: no timer, so a phone on a network with no route to the proxy is not stalled
       * by a lookup on a schedule. ⚠ A deliberate behaviour for {user} to confirm (a timed retry
       * out of Error is the alternative); recorded in docs/HANDOFF.md. Pinned by
       * tests/check_call_audio.py. */
      if (keyPressed == WIPHONE_KEY_END && !gGbcActive && gui.inCall()) {
        gui.state.setSipState(CallState::HangUp);
      } else if (keyPressed == WIPHONE_KEY_END && !gGbcActive &&
                 gui.state.sipState == CallState::Error && gui.state.hasSipAccount()) {
        log_e("SIP: END in Error - one re-init attempt (next SIP pass, needs WiFi)");
        gui.state.setSipState(CallState::NotInited);
      }




#else
      if (keyPressed == WIPHONE_KEY_F1) {
        gui.toggleScreen();
        //allDigitalWrite(ENABLE_DAUGHTER_33V, HIGH);
        //gui.longBatteryAnimation();
        allDigitalWrite(VIBRO_MOTOR_CONTROL, HIGH);
        allDigitalWrite(KEYBOARD_LED, LOW);
        delay(500);
        allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
        allDigitalWrite(KEYBOARD_LED, HIGH);
        gui.toggleScreen();
      }

      if (keyPressed == WIPHONE_KEY_F2) {
        audio_test();
      }

      if (keyPressed == WIPHONE_KEY_F3) {
        bool isLoud = !audio->isLoudspeaker();
        log_d("Loudspeaker: %d", isLoud);
        audio->chooseSpeaker(isLoud);
        log_d("WiFi Mode: %d", WiFi.getMode());
        log_d("WiFi Status: %d", WiFi.status());
      }

      if (keyPressed == WIPHONE_KEY_F4) {
        //esp_sleep_enable_ext0_wakeup((gpio_num_t)KEYBOARD_INTERRUPT_PIN,1);
        //esp_sleep_enable_ext1_wakeup(BUTTON_PIN_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)KEYBOARD_INTERRUPT_PIN,0); //1 = Low to High, 0 = High to Low. Pin pulled HIGH
        //esp_sleep_enable_timer_wakeup(5000000);
        log_d("begin delay");
        delay(500); // debounce
        log_d("begin light sleep");
        delay(10); // let print finish
        //esp_deep_sleep_start();
        esp_light_sleep_start();
        log_d("awake!!!");

        //LOG_MEM_STATUS;

        //gui.setDumpRegion();
        //gui.frameToSerial();    // "screenshot"
      }
#endif

      // Process event in GUI
      redrawWhat |= gui.processEvent(now, keyPressed);
      //log_d("redrawWhat = 0x%x", redrawWhat);

      // Check for "Easter eggs"
      // Easter eggs are currently broken, probably because we now use numeric key entry to dial numbers by default, and can't type '*'
      // (this is not the only thing preventing it from working, but after checking the lack of '*' I stopped)
      if (!memcmp(lastKeys, "##", 2)) {
        // check for Easter eggs
        if (!memcmp(lastKeys + 2, "101**", 5)) {    // **101##   +
          log_d("Easter egg = 101: starting an SIP client");
          gui.state.setSipState(CallState::InvitingCallee);
        } else if (!memcmp(lastKeys + 2, "301**", 5)) {    // **103##
          log_d("Easter egg = 103: send register request");
          sip.registration();
        } else if (!memcmp(lastKeys + 2, "601**", 5)) {    // **106##
          //log_d("Easter egg = 106: send message test");
          log_d("Easter egg = 106: send message test. Add a sip account in WiPhone.ino to use this test");
          sip.sendMessage("sip:user@host.com", "Hello from WiPhone");
        } else if (!memcmp(lastKeys + 2, "701", 5)) {    // **107##
          log_d("Easter egg = 107: test motor and blink LED");
          allDigitalWrite(VIBRO_MOTOR_CONTROL, HIGH);
          allDigitalWrite(KEYBOARD_LED, LOW);
          delay(2500);
          allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
          allDigitalWrite(KEYBOARD_LED, HIGH);
        } else if (!memcmp(lastKeys + 2, "801**", 5)) {    // **108## - soft off
          log_d("Easter egg = 108: soft off");
          allDigitalWrite(POWER_CONTROL, HIGH);

          // AUDIO
          /* 🛑 THE AUDIO EGGS ARE A DEVELOPER BUILD'S, NOT A PHONE'S. **202## streams the LIVE
           * MICROPHONE to a hardcoded 192.168.1.15:5000 — on a typical home LAN, some other
           * device — with nothing on screen, on a firmware other people run. The rest record
           * 1 MB into PSRAM, write .pcm files to the card, and arm RTP with no call. The orphan
           * clock (rtp_watch.h) would now cut 202/203 after 3 s, but the right amount of hot mic
           * in a release is none. Build with -DAUDIO_DEBUG_EGGS to have them back. */
#ifdef AUDIO_DEBUG_EGGS
        } else if (!memcmp(lastKeys + 2, "002**", 5)) {    // **200##
          log_d("Easter egg = 200: audio test on");
          audio_test();
        } else if (!memcmp(lastKeys + 2, "102**", 5)) {    // **201##
          log_d("Easter egg = 201: audio shutdown (audio test off)");
          audio->shutdown();
        } else if (!memcmp(lastKeys + 2, "202**", 5)) {    // **202##
          log_d("Easter egg = 202: sending RTP stream from microphone");
          audio->openRtpConnection(5000);
          audio->sendRtpStreamFromMic(Audio::G722_RTP_PAYLOAD, IPAddress(192, 168, 1, 15), 5000);
        } else if (!memcmp(lastKeys + 2, "302**", 5)) {    // **203##   - (receiving from distant server)
          log_d("Easter egg = 203: play incoming RTP stream");
          audio->openRtpConnection(5000);
          audio->playRtpStream(Audio::G722_RTP_PAYLOAD);
        } else if (!memcmp(lastKeys + 2, "402**", 5)) {    // **204##
          log_d("Easter egg = 204: recording mic audio");
          audio->setBitsPerSample(16);
          audio->setSampleRate(16000);
          audio->setMonoOutput(true);
          audio->recordFromMic();
        } else if (!memcmp(lastKeys + 2, "502**", 5)) {    // **205##
          log_d("Easter egg = 205: stop recording WAV");
          char filename[100];
          sprintf(filename, "/audio_%02d%02d%02d%02d%02d%02d.pcm", ntpClock.getYear()-2000, ntpClock.getMonth(), ntpClock.getDay(), ntpClock.getHour(), ntpClock.getMinute(), ntpClock.getSecond());
          log_d("creating file %s", filename);
          audio->saveWavRecord(&SD, filename);
          audio->shutdown();
#endif // AUDIO_DEBUG_EGGS

          // RINGTONE

        } else if (!memcmp(lastKeys + 2, "103**", 5)) {    // **301## - ringtone on
          log_d("Easter egg = 301: ringtone on");
          startRingtone();
        } else if (!memcmp(lastKeys + 2, "203**", 5)) {    // **302## - ringtone off
          log_d("Easter egg = 302: ringtone off");
          stopRingtone();
        }
#ifdef _WIPHONE_TEST_H_
        else {
          anyPressed = easteregg_tests(lastKeys, anyPressed);
        }
#endif
      }
    }

    // Turn on/off the keypad LEDs for 5s
    if (anyPressed) {
      msLastKeyPress = now;       // this is also used to idle keyboard event (when a symbol gets chosen via waiting)
      if (!keypadLedsOn) {
        // Turn on LEDs
        allDigitalWrite(KEYBOARD_LED, LOW);
        keypadLedsOn = true;
      }
    } else if (keypadLedsOn && elapsedMillis(now, msLastKeyPress, KEYPAD_LEDS_ON_MS)) {
      // Turn off LEDs
      allDigitalWrite(KEYBOARD_LED, HIGH);
      keypadLedsOn = false;
    }

    // Connect to WiFi — but never while the ROM transfer server is up (it may
    // be in softAP mode; an STA connect mid-association panics the chip) and
    // never while an auto-switch scan is in flight: connectToWiFi() hard-cycles
    // the radio (disconnect(true) + begin), which aborted every scan and made
    // the auto-switcher look completely dead.
    //
    // ⚠ WITH BACKOFF, mirroring the scanner's _discScans pattern: a hard radio cycle
    // every 20 s forever — plus the driver's own perpetual auto-reconnect between our
    // attempts — kept the radio actively probing/associating for a whole out-of-range
    // day, which is exactly the September hunt. The first five retries stay at 20 s
    // (stepping briefly out of range still rejoins fast); a sustained absence eases to
    // 3 min; the counter resets the moment anything connects; and a screen wake retries
    // immediately, so pulling the phone out of a pocket is never the slow path. After
    // EVERY failed attempt (eased or not — see the note at the quiesce arming), a plain
    // WiFi.disconnect() 30 s later quiesces the driver's auto-reconnect loop (the same
    // move autoSwitchTick already makes), so the radio genuinely rests between attempts
    // instead of chewing mid-association — and so scans read the air honestly.
    {
      loopPhase("wifi-retry");
      /* The drop record first, so a LOST/JOIN line lands BEFORE whatever this pass does about
       * it (Networks::diagTick: the event task only stores; printing and the card are here). */
      wifiState.diagTick(now);
      static uint32_t s_wifiRetryFails = 0;
      static uint32_t s_wifiQuiesceAtMs = 0;
      static bool     s_prevScreenOnWifi = true;
      const bool screenOnNow = gui.state.screenBrightness > 0;
      const bool wokeNow = screenOnNow && !s_prevScreenOnWifi;
      s_prevScreenOnWifi = screenOnNow;

      if (wifiState.isConnected()) {
        s_wifiRetryFails = 0;
        s_wifiQuiesceAtMs = 0;
      }

      uint32_t retryMs = WIFI_RETRY_PERIOD_MS;
      if (s_wifiRetryFails >= 5) {
        retryMs = 180000u;              // clearly not a brief blip: ease off the radio
      }
      /* 🔑 THE BIG ONE, and the same predicate the scan cadence uses so the two cannot drift
       * apart. Every failed attempt leaves the radio associating for up to 30 s before the
       * quiesce below disconnects it, so at 180 s that is still ~20 bursts an hour — roughly
       * an 8 % duty cycle at full radio power for a phone that is simply somewhere without
       * WiFi. Ten minutes in, back off to one attempt every ten minutes.
       * ⚠ The wake branch just below is untouched: picking the phone up still tries at once. */
      if (wifiState.inLongDrySpell()) {
        retryMs = WIFI_DRY_RETRY_PERIOD_MS;
      }
      bool due = elapsedMillis(now, msLastWifiRetry, retryMs);
      if (wokeNow && !wifiState.isConnected() && wifiState.stationAllowed() &&
          wifiRetryMayBegin(now, lastWifiConnectAttemptMs())) {
        /* Someone just picked the phone up: try NOW — unless a join started in the last
         * ten seconds, in which case hard-cycling the radio would abort an association
         * that was about to succeed and make the wake path SLOWER, not faster. */
        due = true;
      }

      /* ⚠ Block reconnect only while the transfer server is in softAP mode — that is the
       * mode where an STA connect mid-association panics the chip. In STA mode the server
       * is just a socket, and GATING RECONNECT STRANDED IT: a hotspot blink mid-upload
       * left the phone at NO_SSID with the page dead and no path back until the user
       * stopped the server (found live, 2026-08-20 — the "books upload locked up"). */
      /* (0.9.79) xferServing(), not gbcXferOn(): a KOSync sync window's hotspot is the same
       * softAP as the uploader's and panics the chip the same way under an STA connect. */
      const bool xferBlocksWifi = xferServing() && xferUsingAP();
      /* 🛑 wifiStationWanted() (0.9.79): the owner's BOTH switches, no game, no live hotspot — the
       * station gate wifiRestoreStation() shares (wifi_policy.h). This gate asked userDisabled() and
       * the hotspot only. loop() runs all through a Game Boy game (its keys are drained here),
       * so a phone that was off WiFi when the game began kept retrying every 20 s: a SPIFFS INI
       * read on the loop the game's keys ride on, then WiFi.begin() — esp_wifi_start() under the
       * emulator. And with the switch OFF but the per-network flag cleared (Edit > Save), it
       * joined while the menu said off. xferBlocksWifi stays for the quiesce below. */
      /* 🛑 wifiRetryMayBegin() (0.9.79 review R2): not within 10 s of ANY join. msLastWifiRetry is
       * stamped only by this retry, and it goes stale while a hotspot or a game holds the radio -
       * so the pass that closed a sync window (kosyncLoop, above) or ended a game (the key
       * handling, above) reached here with `due` already true, straight after the restore's JOIN
       * had begun, and connectToPreferred() re-began over it: an INI read (~1.6 s) on the loop,
       * then disconnect(false) aborting the fresh association. ⚠ That join is stamped AFTER `now`
       * was read at the top of this pass, and `now - stamp` wraps to ~49 days: the helper counts a
       * stamp ahead of `now` as age 0 (wifi_policy.h) - the plain subtraction the wake branch used
       * could not see it. `due` stays true: if the join has not associated in 10 s, this runs. */
      if (wifiStationWanted() && !wifiState.scanBusy() && wifiState.doReconnect() && !wifiState.isConnected() && due &&
          wifiRetryMayBegin(now, lastWifiConnectAttemptMs())) {
        /* 🔑 DON'T SPEND 30 SECONDS OF RADIO ON AIR WE WERE JUST TOLD IS EMPTY. A scan is
         * ~350 ms; a failed join holds the radio associating for up to 30 s before the
         * quiesce below disconnects it, which is the largest remaining out-of-range cost.
         * worthAttemptingJoin() fails open in every uncertain case and lets one attempt
         * through every WIFI_JOIN_BLIND_EVERY skips — see the note on its declaration.
         * ⚠ The retry clock is stamped either way, or a skip would re-ask every loop pass. */
        const bool joinWorthIt = wifiState.worthAttemptingJoin();
        if (!joinWorthIt) {
          msLastWifiRetry = now;        // skipped, but the cadence still advances
        }
        if (joinWorthIt) {
        bool _cp = false;
        TIME_STEP("connectToPreferred", _cp = wifiState.connectToPreferred());
        if (_cp) {
          log_d("Connecting to WiFi");
        } else {
          log_d("Not connecting to WiFi");
        }
        msLastWifiRetry = now;        // TODO: encapsulate into WiFiState
        if (s_wifiRetryFails < 1000) {
          s_wifiRetryFails++;
        }
        /* ⚠ From the FIRST failed attempt, not the fifth (2026-08-27, the booksync wedge).
         * Every begin() that ends NO_AP_FOUND/BEACON_TIMEOUT hands the radio to the
         * framework's event-task auto-reconnect, which retries capless and gapless — and a
         * radio that is perpetually mid-connect completes every scan EMPTY (esp_wifi
         * documents it), which is the deaf-scan state. The first ~100 s of every drop used
         * to run that churn unquenched; 87ddfbc (08-24) made it worse by removing
         * connectToWiFi's disconnect(true) — a 5 s freeze, but also an accidental radio
         * cycle that broke the churn every 20 s. Quiesce 30 s after EVERY failed attempt
         * so scans between retries read the air honestly. */
        s_wifiQuiesceAtMs = now + 30000u;   // 30 s is every chance to associate
        }   // joinWorthIt
      }

      if (s_wifiQuiesceAtMs && (int32_t)(now - s_wifiQuiesceAtMs) >= 0 &&
          !wifiState.isConnected() && !wifiState.scanBusy() && !xferBlocksWifi) {
        /* ⚠ Only quiesce an attempt that is OURS and STALE. connectToWiFi() is also
         * called by the auto-switcher and by a manual join in the networks app, and a
         * deadline armed 30 s ago knows nothing about them — disconnecting here would
         * abort a join that started milliseconds earlier (mid-DHCP still reads as "not
         * connected"). Any attempt younger than 30 s pushes the deadline out instead. */
        const uint32_t lastAttempt = lastWifiConnectAttemptMs();
        /* wifiJoinAgeMs, not `now - lastAttempt` (R2): a restore's JOIN earlier in this pass is
         * stamped after `now`, the subtraction read it as ~49 days old, and this disconnected a
         * join milliseconds old - masked until R2 by the duplicate retry above re-arming the
         * deadline in the same pass. */
        if (wifiJoinAgeMs(now, lastAttempt) < 30000u) {
          s_wifiQuiesceAtMs = lastAttempt + 30000u;
        } else {
          s_wifiQuiesceAtMs = 0;
          TIME_STEP("WiFi.disconnect(quiesce)", WiFi.disconnect());
          log_e("[wifi] eased retry: radio quiesced until the next attempt");
        }
      }
    }

    // WiFi auto-switch: background scan for the strongest saved network
    // (Networks.cpp). Runs whenever the phone isn't gaming / serving ROMs /
    // mid-AUDIO (a scan mid-call would glitch audio). NOTE: an earlier
    // allowlist of NotInited|Idle silently stopped all ticks once SIP left
    // Idle after connecting — block only genuine call-flow states.
    //
    // ⚠ HangUp and HangingUp are deliberately NOT blocking states. They are
    // TEARDOWN, and they STICK when the proxy is unreachable (the END key
    // enters HangUp from anywhere, no call needed — ino:1857): measured
    // 2026-08-15 (car log, 19 min at sip=6) and again 2026-08-20 (SIX HOURS
    // at sip=6, every scan blocked while the user's saved hotspot sat in
    // range — the deadlock: SIP waits for the network, and this guard made
    // the network wait for SIP). sipNeedsFullSpeed() had already recorded
    // which states are genuinely delicate; this guard now agrees.
    //
    // Belt and braces: with WiFi DISCONNECTED no state can be carrying audio,
    // so there is nothing a scan could glitch — the call gate only applies
    // while actually connected. That makes this deadlock structurally
    // impossible for ANY stuck state, present or future.
    {
      loopPhase("audio-gate");
      CallState cs = gui.state.sipState;
      bool audioDelicate = (cs == CallState::InvitingCallee || cs == CallState::InvitedCallee ||
                            cs == CallState::RemoteRinging  || cs == CallState::Call ||
                            cs == CallState::Accept         || cs == CallState::BeingInvited);
      bool callBusy = audioDelicate && wifiState.isConnected();
      // Same softAP-only rule as the reconnect gate above: an STA-mode transfer
      // server must not stop the machinery that would bring its network back.
      if (!gGbcActive && !(xferServing() && xferUsingAP()) && !callBusy) {
        TIME_STEP("autoSwitchTick", wifiState.autoSwitchTick(gui.state.screenBrightness > 0));
      }
    }

#ifdef USE_VIRTUAL_KEYBOARD
    if (udpKeypad == NULL && wifiState.isConnected()) {
      log_d("Setting a connection");
      udpKeypad = new WiFiUDP();
      udpKeypad->begin(VIRTUAL_KEYBOARD_PORT); // need to check this for memory leak... does it get called repeatedly?
      IPAddress ipAddr = WiFi.localIP();
      log_d("Send UDP packets to:\n%d.%d.%d.%d:%d", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3], VIRTUAL_KEYBOARD_PORT);
    }
#endif

    // Trigger periodic event: APP_TIMER_EVENT
    if (gui.state.msAppTimerEventPeriod > 0) {
      if (elapsedMillis(now, gui.state.msAppTimerEventLast, gui.state.msAppTimerEventPeriod)) {
        redrawWhat |= gui.processEvent(now, APP_TIMER_EVENT);
        gui.state.msAppTimerEventLast = now;
      }
    }

    /* The menu marquee: an overflowing SELECTED row scrolls (MenuWidget::marqueeTick, GUI.h).
     * Its own clock, not the app timer — apps own that for their own purposes. Nothing to
     * scroll = one pointer test. Held while the mesh popup is up: the popup is painted on the
     * panel over the sprite, and the repaint this asks for would wipe it early. */
    if (!meshPopupActive && gui.marqueeTick(now)) {
      redrawWhat |= REDRAW_SCREEN;
    }

    // Trigger scheduled events
    EventType evnt;
    loopPhase("events");
    while (evnt = gui.state.popEvent(now)) {
      redrawWhat |= gui.processEvent(now, evnt);
    }

    // Update message times
    if (updateMessageTimes) {
      gui.reloadMessages();
      updateMessageTimes = false;
    }

    /*if (wifiState.isConnected()) {
      const char* host = "phonetester";
      MDNSResponder mdnsResponder; // = new MDNSResponder();
      if (mdnsResponder.begin("WiPhone")) {
        IPAddress addr = mdnsResponder.queryHost(host, 2000);
        log_d("Address: %d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
      }
      // const char* host = "phonetester";
      // resolveMdns(host);
    }*/

    /* The time zone (tz_service.h): the offset re-computed once a second - a DST change, the clock
     * just set, a zone Auto found - and Auto's look at the measured fix every 30 s. `quiet` under a
     * Game Boy game: no NVS write under it (the offset still follows DST). A change repaints what
     * shows the time exactly as the minute tick does; nothing else needs telling, because every
     * reader takes ntpClock's offset at the moment it reads. */
    loopPhase("tz");
    if (tzSvcLoop(now, gGbcActive)) {
      redrawWhat |= gui.processEvent(now, TIME_UPDATE_EVENT);
    }

    // Clock update
    loopPhase("ntp");
    if (ntpClock.isUpdated()) {
      /* The forced message reload: its load() repairs the unknown-time texts of this boot.
       * 🛑 A MESH clock reloads but does NOT disarm it (review SA-3): that repair's stamps are
       * provisional, and it is the reload on the first NTP or GPS set that finalises them
       * (Storage.cpp). Disarming on the mesh set — as it did — let the mesh time stand for
       * good. A mesh clock sets at most once a boot, so this is one reload more, at most. */
      if (waitingForClockUpdate) {
        updateMessageTimes = true;
        waitingForClockUpdate = !ntpClock.isTimeTrusted();
      }
      msLastMinute = now;
      redrawWhat |= gui.processEvent(now, TIME_UPDATE_EVENT);
    } else if (elapsedMillis(now, msLastMinute, TIME_UPDATE_MINUTE_MS) || (ntpClock.getSecond() > 0 && elapsedMillis(now, msLastMinute, TIME_UPDATE_MINUTE_MS - ntpClock.getSecond() * 1000))) {
      // Tick at the beginning of next minute (or tick if not ticked for more than a minute)
      ntpClock.minuteTick(now);
      msLastMinute = now;
      redrawWhat |= gui.processEvent(now, TIME_UPDATE_EVENT);
    }

//    // Turn OFF?
//    {
//      bool turnOff = gpioExtender.digitalRead(EXTENDER_PIN_B1) == HIGH ? true : false;
//      if (turnOff != lastTurnOff) {
//        lastTurnOff = turnOff;
//        log_d("TURN %s", turnOff ? "YES" : "NO");
//        if (! turnOff) {
//          log_d("BYE");
//          delay(2000);
//          allDigitalWrite(EXTENDER_PIN_B0, LOW);
//        }
//      }
//    }

    // Battery update
#ifdef WIPHONE_BOARD

    // Check battery state
    loopPhase("battery");
    if (elapsedMillis(now, msLastBatt, BATTERY_CHECK_PERIOD_MS)) {
      msLastBatt = now;
      float v = gauge.readVoltage();
      float soc = gauge.readSocPrecise();          // the chip's own estimate; always logged as soc=
      if (v > 0.0) {
        gui.state.battVoltage = v;
        /* battSoc is chosen BELOW, after battCharged has been refreshed — see there. */
      }
      gui.state.battUpdated = true;
#if TF_CARD_DETECT_PIN >= 0
      /* ⚠ THESE TWO WERE PLAIN digitalRead() ON GPIO-EXTENDER PINS, WHICH READS THE
       * WRONG CHIP ENTIRELY. Both pins live on the SX1509, not on the ESP32:
       * TF_CARD_DETECT_PIN is EXTENDER_PIN(1) == 65 and BATTERY_CHARGING_STATUS_PIN is
       * EXTENDER_PIN(0) == 64 (EXTENDER_FLAG is 0x40). The ESP32 has GPIO 0-39, so both
       * were out of range — and digitalRead() does not reject them. It calls
       * gpio_get_level(), which for pin >= 32 evaluates `(in1.data >> (pin - 32)) & 1`;
       * the Xtensa shift masks its amount to 5 bits, so `>> 32` becomes `>> 0` and
       * `>> 33` becomes `>> 1`. The two reads therefore returned:
       *
       *     battCharged  <- GPIO 32  (USER_SERIAL_TX / MotorEN)
       *     cardPresent  <- GPIO 33  (I2S_WS_PIN, the I2S word-select clock)
       *
       * ⚠ THIS IS WHY `chg=` IS DEAD IN EVERY HEALTH SAMPLE. It was logged as "its own
       * small mystery" and then as a measured fact (0 across all 807 samples of the
       * 2026-08-23 run, through two unmistakable charging periods). The flag was never
       * reading the charger; it was reading an idle-low pin on the wrong die.
       *
       * ⚠ And cardPresent read the I2S word-select line, which is idle-low — so it
       * reported TRUE whether or not a card was seated, and would FLICKER while audio
       * plays, since WS toggles at the sample rate and the minute tick samples it at an
       * arbitrary phase. A card-dependent path could fail intermittently during music
       * for reasons nothing else could explain.
       *
       * allDigitalRead() is the accessor that honours EXTENDER_FLAG; it has existed in
       * Hardware.cpp all along and every other extender read in this file uses it or
       * goes to gpioExtender directly. */
      gui.state.cardPresent = allDigitalRead(TF_CARD_DETECT_PIN) == LOW ? true : false;
#else
      gui.state.cardPresent = false;
#endif
      /* ⚠ ACTIVE LOW. The charger IC's STAT output is open-drain: it PULLS THE LINE DOWN while
       * charging and releases it (pull-up → high) when it is not. This read was `== HIGH`, so
       * the flag has been inverted for the life of the project — on top of reading the wrong
       * GPIO entirely until 2026-08-24, which is what hid it.
       *
       * MEASURED, one continuous boot, one build stamp, no confound (2026-08-24):
       *     up=20  v=4.20  chg=0     on the charger
       *     up=21  v=4.15  chg=1     <- unplugged; voltage starts falling
       *     up=25  v=4.11  chg=1     still on battery
       *     up=26  v=4.18  chg=0     <- replugged; voltage jumps back
       * chg=1 tracked NOT CHARGING throughout, and the voltage curve says so independently of
       * the flag. ⚠ Do not "correct" this back without repeating that unplug/replug run: it is
       * the only measurement that distinguishes a tapered charger from an inverted pin. */
      gui.state.battCharged = allDigitalRead(BATTERY_CHARGING_STATUS_PIN) == LOW ? true : false;

      /* WHICH NUMBER THE USER SEES (battery_curve.h has the long version):
       *  - OFF the charger: the voltage curve. This is the number that matters — runtime left,
       *    in the field — and the one the curve was measured for. It has no cliff: the chip read
       *    0 % at 3.54 V with 1.3 h left, and the old `if (soc > 0.0)` guard then FROZE the
       *    display at its last non-zero value for that whole 1.3 h.
       *  - ON the charger: the chip. MEASURED 2026-09-03, 45 min into a charge from empty at
       *    3.89-3.91 V: curve 61-65 %, chip 20-25 %. Charge current lifts the terminal voltage
       *    ~100 mV, and a DISCHARGE curve read against it over-reports by 30-40 points — the
       *    wrong direction to be wrong. The chip is the only charge-aware number we have.
       * ⚠ This sits AFTER the battCharged read on purpose. The first 0.9.56 build chose before
       *   it, on the PREVIOUS tick's flag, and a boot on the charger showed the curve's 66 % for
       *   ~30 s before dropping to the chip's 25 % — measured on a screenshot, and exactly the
       *   kind of jump that makes a person stop believing the number.
       * ⚠ On phone 2 `battCharged` is 0 even on USB (the backplate holds the rail — memory note
       *   wiphone-phone2-backplate), so phone 2 shows the curve on the cable. Its internal cell
       *   IS full then, so the curve's 100 % is the right answer there anyway. */
      if (v > 0.0) {
        gui.state.battSoc = (gui.state.battCharged && soc > 0.0f)
                            ? soc
                            : (float)batterySocFromVoltage(v);
      }

      /* The HEALTH and KEYS lines. In a helper of their own, NOT inline here: see its header. */
      healthLineTick(now, soc, v);

      log_d("Voltage/SOC = %.2f/%d%%", v, (int) round(soc));
      log_d("SD card = %d", gui.state.cardPresent);
      log_d("Charged = %d", gui.state.battCharged);
#if defined(POWER_CHECK) && POWER_CHECK >= 0
      /* gpioExtender indexes its OWN pins, so the flag has to come off first - the
       * working call at the top of loop() does `POWER_CHECK & ~EXTENDER_FLAG` and this
       * one did not, so it queried extender pin 66 and logged nonsense. */
      log_d("Power button = %d", gpioExtender.digitalRead(POWER_CHECK & ~EXTENDER_FLAG) == LOW ? 1 : 0);
#endif
      redrawWhat |= gui.processEvent(now, BATTERY_UPDATE_EVENT);

      // Power off at low battery to avoid unexpected behaviours
      if (v <= 3.3 && now >= 30000) {     // allow phone to work on low battery for 30 seconds
        /* Commit the reading position BEFORE the latch drops, not after. powerOff() asserts
         * POWER_CONTROL and does not unwind anything, so the BooksApp destructor — the only
         * thing that would otherwise have flushed — never runs. bookstore.h has asked for
         * this call since it was written; this is it. */
        extern bool booksSaveOpenPosition();   // app_books.h; declared here like healthDump
        booksSaveOpenPosition();
        extern bool mapsSaveOpenView();        // app_maps.h; the map's view, same reason
        mapsSaveOpenView();
        tileFetchPowerOff();                   // tile_fetch.h; a download's cursor, and "not a crash"
        extern bool gbcSaveForPowerOff();      // app_gbc.h; a running game's resume point
        gbcSaveForPowerOff();
        powerOff();
        redrawWhat |= gui.processEvent(now, POWER_OFF_EVENT);
      }
    }

    // Check for changes in WiFi strength
    loopPhase("wifi-rssi");
    if (elapsedMillis(now, msLastWiFiRssi, WIFI_CHECK_PERIOD_MS)) {
      msLastWiFiRssi = now;
      int rssi = WiFi.RSSI();
      wifiDiagNoteRssi(rssi, now);     // so a drop can say what the link looked like just before
      if (GUI::wifiSignalStrength(gui.state.wifiRssi) != GUI::wifiSignalStrength(rssi)) {
        gui.state.wifiRssi = rssi;
        redrawWhat |= gui.processEvent(now, WIFI_ICON_UPDATE_EVENT);
      } else {
        gui.state.wifiRssi = rssi;
      }
    }

    // Check if USB is connected / charging the battery
    loopPhase("usb");
    if (elapsedMillis(now, msLastUsbCheck, USB_CHECK_PERIOD_MS)) {
      msLastUsbCheck = now;
      //bool usbHere = gpioExtender.digitalRead(USB_POWER_DETECT_PIN)==LOW ? true : false;
      bool usbHere = digitalRead(BATTERY_PPR_PIN) == LOW ? true : false;
      usbConnected += usbHere ? 1 : 0;
      usbConnectedChecks += 1;
      if (usbHere != gui.state.usbConnected) {
        // USB cable status changed
        gui.state.usbConnected = usbHere;
        // If USB is connected -> enable blinking
        // otherwise -> disable blinking
        gui.state.battBlinkOn = usbHere;
        redrawWhat |= gui.processEvent(now, USB_UPDATE_EVENT);
      } else if (usbHere && !gui.state.battCharged && gui.state.battSoc < 100) {
#ifndef BATTERY_BLINKING_OFF
        // If blinking enabled -> trigger BATTERY_BLINK_EVENT every second
        gui.state.battBlinkOn = !gui.state.battBlinkOn;
        redrawWhat |= gui.processEvent(now, BATTERY_BLINK_EVENT);
        log_v("blinked");
#endif
      }
      if (!(usbConnectedChecks & 15)) {     // Dump check result to the logs occasionally
        log_d("USB = %d (%.1f%%), checks=%d", gui.state.usbConnected, 100.0 * usbConnected / usbConnectedChecks, usbConnectedChecks);
      }
    }

    // Power OFF
#ifdef WIPHONE_INTEGRATED_1_4
    if (powerButtonPressed && !poweringOff && elapsedMillis(now, msPowerOffStarted, 2500)) {
      /* A 2.5 s hold is confirmed by a fresh read before anything irreversible: the flag
       * above is set from one interrupt-time read and cleared only by another edge, so a
       * misread press (see gpioExtenderServiceInterrupt) would otherwise ride the timer
       * straight into powerOff(). If the button is not down NOW, or the chip cannot be
       * heard, nothing happens and the next real press starts over. */
      byte level = HIGH;
      const bool heard = gpioExtender.readPinChecked(POWER_CHECK & ~EXTENDER_FLAG, &level);
      if (!heard) {
        log_e("POWER: 2.5 s hold - extender read failed, asking again in 500 ms");
        msPowerOffStarted = now - 2000;          // keep the flag: a real hold is still held
      } else if (level != LOW) {
        log_e("POWER: 2.5 s hold not confirmed, the button is up - a release was missed");
        powerButtonPressed = false;
      } else {
      /* The EVERYDAY power-off, and it needs the save just as much as the low-battery one.
       * Every POWER_OFF_EVENT dispatch in this file calls powerOff() FIRST, so an app that
       * saves when it receives the event is already writing after the latch has been pulled.
       * Save here, before it. */
      extern bool booksSaveOpenPosition();
      booksSaveOpenPosition();
      extern bool mapsSaveOpenView();
      mapsSaveOpenView();
      tileFetchPowerOff();
      extern bool gbcSaveForPowerOff();        // a running game: parks it, writes /gbc/<rom>.auto
      gbcSaveForPowerOff();
      powerOff();
      redrawWhat |= gui.processEvent(now, POWER_OFF_EVENT);
      }
    }
#endif // WIPHONE_INTEGRATED_1_4
#endif // WIPHONE_BOARD

    if (gui.state.inputCurKey && msLastKeyInput && elapsedMillis(now, msLastKeyInput, KEYPAD_IDLE_MS)) {
      log_i("keypad idle");
      redrawWhat |= gui.processEvent(now, KEYBOARD_TIMEOUT_EVENT);
      msLastKeyInput = 0;
    }

    // User serial processing
#ifdef USER_SERIAL
    // Read what has arrived to user UART
    loopPhase("user-serial");
    while (userSerial.available() > 0) {
      char ch = userSerial.read();
      if (gGpsNmea) {
        /* GPS mode: NMEA feeds the fix, not the GUI. A completed RMC/GGA lands
         * in meshService, where resolveReference gives it to pos/sun/Places. */
        gGpsRawBuf[gGpsRawHead] = (uint8_t)ch;      // for `gps raw`
        gGpsRawHead = (gGpsRawHead + 1) % sizeof(gGpsRawBuf);
        gGpsRawSeen++;
        if (gGpsReader.feed(ch)) {
          const NmeaFix& fx = gGpsReader.fix();
          /* feed() completes on an RMC or a GGA (fx.lastKind says which). The WHOLE fix goes in,
           * and self_pos.h's selfGpsApply reads only that sentence's own fields: handing over the
           * reader's sticky valid/latI/sats let a run of no-fix GGAs, with the RMCs lost,
           * re-stamp the last position as a fix taken now (review 2026-10-10). */
          meshService.gpsUpdate(fx);
          /* GPS TIME (0.9.79): a new RMC with a fix may set the clock — two consecutive agreeing
           * readings, never over a fresh NTP; the rules are clock_source.h's. millis() here,
           * not the pass's `now`: the stamp is WHEN THE SENTENCE ARRIVED, and this loop drains
           * a whole UART buffer per pass. */
          ntpClock.gpsSentence(fx, millis());
        }
        continue;
      }
      log_d("USER SERIAL: %c", ch);
      gui.state.userSerialBuffer.put(ch);
    }
    if (!gGpsNmea &&
        gui.state.userSerialBuffer.size() > 0 && userSerialLastSize != gui.state.userSerialBuffer.size()) {
      // Trigger UART event
      log_d("User serial: %s", gui.state.userSerialBuffer.getCopy());
      redrawWhat |= gui.processEvent(now, USER_SERIAL_EVENT);
      userSerialLastSize = gui.state.userSerialBuffer.size();
    }
#endif // USER_SERIAL

    // GUI update
    loopPhase("redraw");
    if (redrawWhat & REDRAW_ALL) {
      gui.redrawScreen(redrawWhat & REDRAW_HEADER, redrawWhat & REDRAW_FOOTER, redrawWhat & REDRAW_SCREEN, redrawWhat & LOCK_UNLOCK);
    }

    // SIP CLIENT:

    //Added: check if disconnected from wif during call
    loopPhase("sip-recon");
    if (gui.state.hasSipAccount() && !wifiState.isConnected()) {
      if (sip.isBusy()) {
        log_d("Device disconnected from WIFI");
        log_d("Call will be terminated");

        // terminate audio session
        audio->showAudioStats();
        audio->shutdown();

        sip.wifiTerminateCall();  // wifi is disconnected but need to destroy this dialogue
        /* ⚠ A call that was still RINGING when WiFi dropped left the ringtone playing:
         * every other teardown path calls stopRingtone(), this one never did, so the
         * speaker kept announcing a call that no longer existed until something else
         * happened to stop it. Audio, motor and full-speed CPU, indefinitely. */
        stopRingtone();
        gui.exitCall();
        
        gui.state.setSipState(CallState::HungUp);
        appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
        gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);

        wifiTerminateSip = TERMINATE_OK;

        //gui.state.sipAccountChanged = true;
      } else {
        /*
         * in order to do ping and update staled state.
        */
        //sip.checkCall(now);
      }
    }

    /* The far end's RTP went quiet for RTP_SILENCE_STRIKES whole windows IN THIS CALL (120 s):
     * end it. The counting is Audio.cpp's, per call (rtp_watch.h); RTP_SILENT_ON is the verdict,
     * not a single window. Until 0.9.79 the count lived here and was never reset, so from the
     * second silent window of a boot onward EVERY window ended the call — including the first
     * packet-less pass of the next call. ⚠ sip.rtpSilent() still sends no BYE; out of scope. */
    if (rtpSilentPeriod == RTP_SILENT_ON) {

      rtpSilentPeriod = RTP_SILENT_OFF;

      // Stop media session
      audio->showAudioStats();
      audio->shutdown();

      if (sip.isBusy()) {
        /* log_e, not log_d: only log_e is compiled in, and a call ended by the phone itself with
         * no BYE and no screen of explanation was invisible on serial. */
        log_e("CALL: no RTP from the far end for %u s - call ended (no BYE sent)",
              (unsigned)(RTP_SILENCE_WINDOW_MS * RTP_SILENCE_STRIKES / 1000));

        sip.rtpSilent();
        gui.exitCall();

        gui.state.setSipState(CallState::HungUp);
        appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
        gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);

        wifiTerminateSip = TERMINATE_OK;
      }
    }
    //    This is the "user-agent core", something that binds together SIP library, GUI, audio interfaces and message storage.
    //    It implements transitions between different call states.
    loopPhase("sip");
    if (gui.state.hasSipAccount() && wifiState.isConnected() && sipMayPoll()) {
      if( wifiTerminateSip == TERMINATE_OK ) {
        /* 🛑 THIS LINE WAS `gui.state.sipState == CallState::NotInited;` — A COMPARISON WHOSE
         * RESULT WAS DISCARDED, WHERE AN ASSIGNMENT WAS MEANT. Present since the initial
         * commit, and `platformio.ini` strips -Werror so the unused-value warning could never
         * fail the build. The only effect of this whole block was clearing the flag below, so
         * **the SIP re-init after a network-loss teardown has never once run in this
         * firmware's life** — the test on the next line only fired if sipState happened to be
         * NotInited already, or the account had changed.
         *
         * Written through setSipState() rather than as a raw assignment: every other
         * transition in this firmware goes through it, and it owns the ring-armed and
         * missed-call bookkeeping. A raw `=` would be the author's literal intent and would
         * silently bypass all of that.
         *
         * ⚠ THIS TURNS ON A PATH WITH NO HISTORY. Bench it: with a real account, drop WiFi
         * mid-call, restore it, and watch for "SIP is going to init" followed by registration
         * returning rather than flapping. If it misbehaves, this line is the whole change. */
        gui.state.setSipState(CallState::NotInited);
        wifiTerminateSip = 0x0;
      }
      if (gui.state.sipState == CallState::NotInited  ||  gui.state.sipAccountChanged) {
        log_d("SIP is going to init");
        // Connect to SIP proxy
        uint8_t mac[6];
        wifiState.getMac(mac);
        if (sip.init( gui.state.fromNameDyn,
                      gui.state.fromUriDyn,
                      gui.state.proxyPassDyn,
                      mac )) {
          sip.triedToMakeCallCounter = 0;
          log_d("Connected to SIP");
          gui.state.setSipState(CallState::Idle);
          log_d("caller free (0) = %s", sip.isBusy() ? "NO" : "YES");
        } else {
          // Failed to connect to proxy
          log_e("failed to connect to SIP");
          gui.state.setSipState(CallState::Error);      // permanent error state  TODO
        }
        Random.feed(now);
        gui.state.sipEnabled = true;
        gui.state.sipAccountChanged = false;

      } else if (gui.state.sipState == CallState::Idle) {
        sip.triedToMakeCallCounter = 0;
        bool anySip = false;      // anything received?
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);     // TODO: all of this logic could be reorganized to call checkCall in one place and then process results according to the current state
          if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            anySip = true;
          }
          if (res & TinySIP::EVENT_INCOMING_CALL) {
            gui.state.setRemoteNameUri(sip.getRemoteName(), sip.getRemoteUri());
            gui.becomeCallee();
            gui.state.setSipState(CallState::BeingInvited);
            startRingtone();
          } else if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            log_d("UNPROCESSED CALL STATE (Idle): 0x%x", res);
            gui.state.setSipState(CallState::Idle);
          }
        } while (res & TinySIP::EVENT_MORE_BUFFER);
        bool isRegistered = sip.registrationValid(now);
        if (anySip || isRegistered != gui.state.sipRegistered) {
          log_d("setting reason @ CallState::Idle");
          gui.state.setSipReason(sip.getReason());
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          // Force GUI to update screen if registration status has changed
          // NOTE: SIP registration can happen only in Idle state
          if (isRegistered != gui.state.sipRegistered) {
            // Ingicate successful registration
            gui.state.sipRegistered = isRegistered;
            // Allow GUI display it
            res |= gui.processEvent(now, REGISTRATION_UPDATE_EVENT);       // the app should decide whether to react to registration update
            /* ⚠ log_e, not log_d, and on BOTH edges — only log_e is compiled into this
             * build, so at log_d a phone that registers and one that never does look
             * identical on serial. Same lesson the SMS mirror learned. */
            log_e("SIP REGISTRATION -> %s", gui.state.sipRegistered ? "REGISTERED" : "lost");
          }
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);

        } else if (gui.state.outgoingMessages.size()) {

          // Send queued messages
          MessageData* msg = gui.state.outgoingMessages[0];
          if (msg) {
            auto err = sip.sendMessage(msg->getOtherUri(), msg->getMessageText());
            if (err == TINY_SIP_OK) {
              gui.flash.messages.setSent(*msg);
              delete msg;
              gui.state.outgoingMessages.remove(0);
            } else {
              log_e("message sending FAILED");
            }
          }

        }

      } else if (gui.state.sipState == CallState::BeingInvited) {

        bool anySip = false;      // anything received
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);
          if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            anySip = true;
          }
          if (res & TinySIP::EVENT_CALL_TERMINATED) {
            if (gui.state.sipState != CallState::HungUp) {
              stopRingtone();
              log_d("call terminated @ BeingInvited");
              gui.state.setSipState(CallState::HungUp);
              msHungUp = now;
            }
          } else if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            log_d("UNPROCESSED CALL STATE (BeingInvited): 0x%x", res);
          }
        } while (res & TinySIP::EVENT_MORE_BUFFER);
        if (anySip) {
          log_d("setting reason @ CallState::BeingInvited");
          gui.state.setSipReason(sip.getReason());
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::Accept) {

        log_v("Accepting call");

        stopRingtone();
        int res = sip.acceptCall();
        if (res == TINY_SIP_OK) {
          gui.state.setSipState(CallState::InvitedCallee);
        } else {
          log_e("could not accept call, err = %d", res);
          gui.state.setSipState(CallState::HungUp);
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::Decline) {

        log_d("Declining call");

        stopRingtone();

        int res = sip.declineCall();
        if (res == TINY_SIP_OK) {
          gui.state.setSipState(CallState::HangingUp);
        } else {
          log_d("could not decline = %d", res);
          gui.state.setSipState(CallState::HungUp);
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::InvitingCallee and gui.state.sipRegistered) {

        // Initialize / start call

        log_d("Calling: %s", gui.state.calleeUriDyn);
        if (strchr(gui.state.calleeUriDyn, '@') != NULL and  strlen(gui.state.calleeUriDyn)>0 and gui.state.sipRegistered) {
          /* 🛑 THE CALL TAKES THE AUDIO DEVICE HERE (review SA-2, 2026-09-25): a pop in flight
           * finished, then music, the stored levels and the call's own route - see
           * callTakesAudioDevice(). Before connect, so the call screen's UP/DOWN steps the
           * level of the route this call will use, not a stale one (SA-1). */
          notifyPopFinishFor("a call is being dialled");
          callTakesAudioDevice("dial", true);    // the stored levels read HERE, before the INVITE
          sip.startCall(gui.state.calleeUriDyn, now);
          // Proceed to next state
          gui.state.setSipState(CallState::InvitedCallee);
          gui.redrawScreen(true, true, true, true);         // TODO: one of two special cases of redrawAll
        } else {
          log_e("sip callee unavailable");
          gui.state.setSipState(CallState::Idle);
        }

      } else if (gui.state.sipState == CallState::InvitedCallee) {
        // Audio session configs
        IPAddress rtpRemoteIP((uint32_t) 0);
        int rtpRemotePort = 0;
        uint16_t rtpLocalPort = 0;
        uint8_t audioFormat = TinySIP::NULL_RTP_PAYLOAD;

        bool callEstablished = false;
        bool anySip = false;      // anything received
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);
          if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            anySip = true;
          }
          if (res & TinySIP::EVENT_CALL_CONFIRMED) {
            if (gui.state.sipState != CallState::Call) {  // change state only once
              log_d("call established");
              callEstablished = true;

              // Copy audio session configs
              if (!rtpRemoteIP.fromString(sip.getRemoteAudioAddr())) {
                rtpRemoteIP = resolveDomain(sip.getRemoteAudioAddr());
                if (!(uint32_t) rtpRemoteIP) {
                  log_e("couldn't parse IP address from \"%s\"", sip.getRemoteAudioAddr());
                }
              }
              rtpRemotePort = sip.getRemoteAudioPort();
              audioFormat = sip.getAudioFormat();
              log_d("  RTP rmt addr: %8X", (uint32_t) rtpRemoteIP);
              log_d("  RTP rmt port: %d", rtpRemotePort);
              log_d("  Audio format:  %d", audioFormat);

              if ((uint32_t)rtpRemoteIP && rtpRemotePort && audioFormat != TinySIP::NULL_RTP_PAYLOAD) {
                rtpLocalPort = sip.getLocalAudioPort();
                log_d("  RTP loc port: %d", rtpLocalPort);
              }
              gui.state.setSipState(CallState::Call);
            }
          } else if (res & TinySIP::EVENT_CALL_TERMINATED) {
            if (gui.state.sipState != CallState::HungUp) {
              gui.state.setSipState(CallState::Decline);
              log_d("call terminated @ InvitedCallee = %d", now);
              //gui.state.setSipState(CallState::HungUp);
              msHungUp = now;
            } else {
              gui.state.setSipState(CallState::Decline);
              log_d("call @ InvitedCallee is Declined");
            }
          } else if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            log_d("UNPROCESSED CALL STATE: 0x%x", res);
          }
        } while (res & TinySIP::EVENT_MORE_BUFFER);

        // Update screen to show that a call was started
        //work by techtesh
        if (anySip) {
          log_d("setting reason @ CallState::InvitedCallee");
          gui.state.setSipReason(sip.getReason());
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

        // Start audio only after the screen is updated
        if (callEstablished) {
          // If audio configs are OK -> turn on audio (speaker & microphone) & start listening to audio port
          if ((uint32_t)rtpRemoteIP && rtpRemotePort && audioFormat != TinySIP::NULL_RTP_PAYLOAD) {
            /* ⚠ A POP STILL PLAYING IS FINISHED FIRST. The pop gate only refuses to START one
             * during CallState::Call, so a chirp that began while this call was still ringing
             * out can be mid-play now — and its teardown restore()s the pre-pop rate, channels,
             * route and levels over whatever the lines below configure, in this same pass.
             * Finishing it here puts its snapshot back BEFORE the call sets up on top. */
            notifyPopFinishFor("a call's audio is starting");
            /* 🛑 THEN THE CALL TAKES THE DEVICE: music yields, the stored levels, THEN the call's
             * own route (review SA-2, 2026-09-25) - see callTakesAudioDevice(). A caller's call
             * used to play on the loudspeaker the CallApp constructor picked for the ring. Here,
             * not after: the loop's own yield runs later in this pass and would put music's
             * saved route back over a route chosen before it.
             * ⚠ WITHOUT the stored-level read (`false`): a 0.5-1.6 s SPIFFS open here, with the
             * RTP port still closed, would lose the far end's first words, and the levels it
             * would read went in at dial or at the ring - see callTakesAudioDevice(). */
            callTakesAudioDevice("call", false);
            audio->openRtpConnection(rtpLocalPort);
            audio->sendRtpStreamFromMic(audioFormat, rtpRemoteIP, rtpRemotePort);
            audio->playRtpStream(audioFormat, rtpRemotePort);
          } else {
            log_e("audio session failure");
            gui.state.setSipReason("audio failed");
            // Force GUI to update screen
            appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
            gui.redrawScreen(false, false, true);
          }
        }

      } else if (gui.state.sipState == CallState::Call) {

        // Process any SIP requests quickly when call is established

        bool anySip = false;      // anything received
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);
          if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            anySip = true;
          }
          if (res & TinySIP::EVENT_CALL_TERMINATED) {
            if (gui.state.sipState != CallState::HungUp) {
              audio->showAudioStats();
              audio->shutdown();
              log_d("call terminated by remote @ Call");
              gui.state.setSipState(CallState::HungUp);
              msHungUp = now;
            } else if(gui.state.sipState == CallState::HungUp) {
              log_d("Hang up call before remote party answers it");
              sip.declineCall();
            }
          } else if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            log_d("UNPROCESSED CALL STATE (2): 0x%x", res);
          }
        } while (res & TinySIP::EVENT_MORE_BUFFER);
        if (anySip) {
          log_d("setting reason @ CallState::Call");
          gui.state.setSipReason(sip.getReason());
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::HangingUp) {

        bool anySip = false;      // anything received
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);
          if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            anySip = true;
          }
          if (res & TinySIP::EVENT_CALL_TERMINATED) {
            if (gui.state.sipState != CallState::HungUp) {
              log_d("call terminated @ HangingUp");
              gui.state.setSipState(CallState::HungUp);
              msHungUp = now;
            }
          } else if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            log_d("UNPROCESSED CALL STATE (3): 0x%x", res);
          }
        } while (res & TinySIP::EVENT_MORE_BUFFER);

        // (Timeout &) Update GUI
        if (anySip || elapsedMillis(now, msHangingUp, HANGUP_TIMEOUT_MS)) {
          if (anySip) {
            log_d("setting reason @ CallState::HangingUp");
            gui.state.setSipReason(sip.getReason());
          } else {
            log_d("hang up timeout");
            gui.state.setSipState(CallState::Idle);     // go straight to Idle on timeout
            log_d("caller free (1) = %s", sip.isBusy() ? "NO" : "YES");
          }

          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::HangUp) {

        // User request to hangup call -> send BYE / CANCEL request
        log_d("Terminating call");
        stopRingtone();
        // Stop media session
        audio->showAudioStats();
        audio->shutdown();

        int res = sip.terminateCall(now);
        if (res == TINY_SIP_OK) {
          msHangingUp = now;

          // Proceed to next state
          gui.state.setSipState(CallState::HangingUp);
        } else {
          log_d("terminating error = %d", res);
          gui.state.setSipState(CallState::HungUp);
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }


        // Go back to normal

        /*if (elapsedMillis(now, msHungUp, GUI::HUNGUP_TO_NORMAL_MS)) {

          log_d("hungup timeout: now = %d, msHungUp = %d", now, msHungUp);
          gui.state.setSipState(CallState::Idle);
          log_d("caller free (2) = %s", sip.isBusy() ? "NO" : "YES");
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN, true);       // TODO: one of two special cases of redrawAll
        }*/

      } else if (gui.state.sipState == CallState::HungUp) {

        // Go back to normal

        if (elapsedMillis(now, msHungUp, GUI::HUNGUP_TO_NORMAL_MS)) {

          log_d("hungup timeout: now = %d, msHungUp = %d", now, msHungUp);
          gui.state.setSipState(CallState::Idle);
          log_d("caller free (2) = %s", sip.isBusy() ? "NO" : "YES");
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN, true);       // TODO: one of two special cases of redrawAll
        }

      }

      // Check for incoming messages
      TextMessage* msg = NULL;
      /* The time and WHO set it, in one lock (review SA-3): a SIP arrival stamped off a MESH
       * clock is saved provisional (meshId) and finalised when NTP/GPS arrives. */
      const ClockMsgStamp rxStamp = ntpClock.msgStamp();
      if (msg = sip.checkMessage(now, rxStamp.utc, rxStamp.utc != 0)) {
        log_v("message received");
        // Save message from external RAM into a file (part of message database)
        gui.flash.messages.saveMessage(msg->message, msg->from, msg->to, true,
                                       msg->useTime ? msg->utcTime : 0,    // time == 0 for unknown real time
                                       0, 0, msg->useTime ? rxStamp.meshId : 0);
        delete msg;
        // Pass event to GUI
        appEventResult res = gui.processEvent(now, NEW_MESSAGE_EVENT);
        gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        notifyMessageArrived(gui.state.notifySipMode);   // a text arriving is news
      }
    } else {
      gui.state.sipRegistered = false;
    }

#if defined(LORA_MESSAGING) && !defined(MESHTASTIC_PHY)
    loopPhase("lora");
    if (!gGbcActive && lora.loop()) {
      log_d("Received LoRa message");
      appEventResult res = gui.processEvent(now, NEW_MESSAGE_EVENT);
      gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
    }

    loopPhase("lora-tx");
    if (gui.state.outgoingLoraMessages.size() > 0) {
      log_d("Sending LoRa message");
      // Send queued messages
      MessageData* msg = gui.state.outgoingLoraMessages[0];
      if (msg) {
        lora.send_message(msg->getOtherUri(), msg->getMessageText());
        gui.flash.messages.setSent(*msg);
        delete msg;
        gui.state.outgoingLoraMessages.remove(0);
      }
    }
#endif

    /* Music keeps playing while you are anywhere else in the phone. Audio::loop() is
     * already pumped from here and does the decoding; this only notices the end of a
     * track and starts the next one, which is why it lives outside MusicApp — that app
     * is deleted the moment you back out of its screen.
     *
     * ⚠ Skipped during a Game Boy session for the same reason the mesh is: that path
     * runs its own tight loop and the audio peripheral is handed to the emulator. */
    if (!gGbcActive) {
      musicPlayerLoop();
    }

    /* A call wins over music, always. The codec and I2S are single-occupancy, so ringing
     * or talking while a track plays gives you both at once and neither intelligibly.
     *
     * 🛑 THIS USED TO BE EDGE-TRIGGERED AND THE LATCH COULD STICK FOR HOURS.
     * It was `if (callBusy && !wasCallBusy)` over a static — pause once on the rising edge.
     * But sipCallActive() counts HangUp and HangingUp as a call, and this repo has MEASURED
     * 19+ consecutive minutes parked at sip=6 (HangUp) with the proxy unreachable. While
     * parked there `wasCallBusy` stays true, so an incoming INVITE moves HangUp ->
     * BeingInvited — also true — and there is NO RISING EDGE. The music never paused, and
     * the codec/I2S single-occupancy noted above is the whole reason this exists.
     *
     * ⚠ The fix is here, NOT in sipCallActive(): that predicate is shared, and narrowing a
     * shared predicate to suit one caller is the bug class that produced four of the seven
     * faults in the 2026-08-15 audit. Made idempotent instead — musicPlayerPause() is only
     * called when something is actually playing, so it cannot latch and cannot repeat.
     *
     * Nothing resumes afterwards on purpose: a phone that bursts into music the moment you
     * hang up is worse than one you press play on.
     *
     * 🛑 0.9.79: gui.inCall(), NOT sipCallActive() (now gone). Level-triggering made the stuck
     * HangUp above worse, not better: with no WiFi or no SIP account nothing ever leaves HangUp,
     * so after one END press on any screen every track was paused on the pass it started — the
     * music could not be played at all until WiFi came back or the phone rebooted. Teardown
     * states hold no audio (the HangUp branch has already shut the device down), so they are
     * not a reason to stop music. END also no longer enters HangUp outside a call.
     * ⚠ musicPlayerYieldForCall(), not musicPlayerPause(): the ring and a pop take `playback`
     * away from music BEFORE this runs, and only the yield still gives the call levels back
     * then. It is idempotent (nothing playing + nothing stashed = nothing done), so running it
     * on every pass of a call is two flag reads.
     * 🛑 NOT WHILE A POP IS IN FLIGHT — the same "pop first, then the music" rule startRingtone()
     * follows. A pop that cut a track holds a snapshot of the MUSIC level and route; dial inside
     * its window (up to ~0.9 s) and this yield would hand the call levels back and clear the
     * stash, then the pop's teardown below restore()s the music snapshot over them, and nothing
     * re-applies them — the outgoing call runs at the music level on the loudspeaker. Held off,
     * the yield runs on the pass after the pop's restore(); and if the call connects first, the
     * call-audio setup finishes the pop (notifyPopFinishFor) and this runs in that same pass. */
    if (gui.inCall() && !meshPopPlaying) {
      musicPlayerYieldForCall();
    }

    // Meshtastic background service tick (non-blocking). If a new message
    // arrived, notify the GUI so an open Channel view refreshes live, raise the
    // status-bar unread flag, and show a brief popup banner on any screen.
    /* THREE PHASES, SO A STALL LINE SAYS WHICH HALF: 'mesh' is the service itself (radio, RX,
     * the DB save step), 'mesh-ui' is what an arrival does to the screen (repaint, popup,
     * notify), 'mesh-post' is the buzz/pop/audio teardown and the rest of the block. Until
     * 0.9.79 all three were 'mesh', and that one name sent the 0.6-1.5 s stalls to the wrong
     * suspect for three weeks (they were LoRa transmits). No behaviour change. */
    loopPhase("mesh");
    /* 🛑 A GAME STARTED MID-FRAME MUST NOT LEAVE THE RADIO DEAF FOR THE WHOLE GAME (review M2,
     * 2026-09-25). The mesh loop is skipped while the emulator runs, and after TxDone the SX1276
     * waits in STANDBY for the pass that puts it back in RX — which was the first pass after the
     * game. txComplete() finishes that frame (back to RX, its receipt, its stats) and starts
     * nothing; it drives only the LoRa chip's own bit-banged bus (GPIO 12/13/14/27), never the
     * LCD, the card or I2S, so it cannot collide with the emulator. A bool test when idle. */
    if (gGbcActive) {
      meshService.txComplete();
    }
    const bool meshArrived = !gGbcActive && meshService.loop();
    loopPhase("mesh-ui");
    if (meshArrived) {
      log_d("Received Meshtastic message");
      gui.state.meshUnread = true;
      gui.processEvent(now, NEW_MESSAGE_EVENT);   // let an open Mesh view rebuild
      // Full repaint so the unread icon shows immediately on any screen.
      gui.redrawScreen(true, true, true);
      const MeshMessage* nm = meshService.getMessage(0);   // newest = the arrival
      if (nm) {
        const MeshNode* n = meshService.findNode(nm->from);
        char title[32];
        snprintf(title, sizeof(title), "Mesh: %s", n ? n->name : "new message");
        gui.showMeshPopup(title, nm->text);                // drawn on top of app
        meshPopupActive = true;
        meshPopupShownMs = millis();     // not `now`: this pass may have been long
      }
      notifyMessageArrived(gui.state.notifyMeshMode);
    }

    /* A position or waypoint arrived: refresh an open Places/Nodes view so the
     * user is never sitting on "No places heard yet" after camp has already
     * arrived — the book-sync 'receiver shows nothing' trap in new clothes.
     * QUIET on purpose: no popup, no unread icon, no buzz — places are ambient
     * state, not news. */
    if (!gGbcActive && meshService.takePlacesNews()) {
      /* ⚠ THE RESULT IS ACTED ON HERE. It was discarded for two releases, so the Maps app's
       * "redraw when a position arrives" returned REDRAW_SCREEN into the void and a marker
       * that moved stayed put until the next keypress. The first fix OR'd it into
       * `redrawWhat` — which this pass had already consumed, hundreds of lines above — so
       * it was still discarded (review, 2026-09-19). Same shape as the mirror path now. */
      const appEventResult r = gui.processEvent(now, NEW_MESSAGE_EVENT);
      if (r & REDRAW_ALL) {
        gui.redrawScreen(r & REDRAW_HEADER, r & REDRAW_FOOTER, r & REDRAW_SCREEN);
      }
    }

    /* 🛑 FRESH CLOCK FOR THE TEARDOWN. `now` is from the top of this pass; anything that
     * stalled since — the health log named the mesh block (LoRa transmits, until 0.9.79) —
     * would make both pulses below look older than they are and end them early. The stamps
     * were taken from millis() at the moment of the write (see notifyMessageArrived), so they
     * are compared against millis() here; a stall earlier in the pass can no longer shorten a
     * pulse, and the "after N ms" in the two lines below is the length as actually driven. */
    loopPhase("mesh-post");
    const uint32_t tNow = millis();

    // Stop the notification vibration after its pulse.
    if (meshVibroActive && elapsedMillis(tNow, meshVibroStartMs, meshVibroLenMs)) {
      if (motorWrite(LOW)) {
        meshVibroActive = false;
        meshVibroOffFails = 0;
        log_e("NOTIFY: buzz off after %lu ms (target %u)",
              (unsigned long)(tNow - meshVibroStartMs), (unsigned)meshVibroLenMs);
      } else if (++meshVibroOffFails >= 5) {
        /* A LOW the extender keeps refusing leaves the motor running, so the flag stays set
         * and every pass tries again — but not forever: a bus that dead has taken the keypad
         * and the codec with it, and a log line per pass would bury the evidence. */
        meshVibroActive = false;
        meshVibroOffFails = 0;
        log_e("NOTIFY: motor OFF abandoned after 5 passes - if it is still buzzing, the I2C bus is the fault");
      }
    }

    // One-shot pop: stop it by timer once it has played through — the flash buffer plays
    // once and pads with silence, the SPIFFS fallback loops, and meshPopStopMs was chosen
    // for whichever started (notify_timing.h). Use ceasePlayback (NOT shutdown) — shutdown()'s
    // codec power-down on the shared I2C bus disrupts the vibro motor extender.
    /* ⚠ audio->restore() IS NOT OPTIONAL — see Audio::preserve() for the whole story.
     * playPop() reconfigures six output parameters and this teardown used to put none of
     * them back, so one mesh notification left the phone at 8 kHz, mono, loudspeaker-forced
     * and at full volume permanently. That made music play mono out of the loudspeaker with
     * headphones in, and it is the same monoOut=true that paces the Game Boy at 50%.
     * Restoring is correct in BOTH exits below: it puts back the pre-pop state, and a call
     * starting will then configure whatever it needs on top. */
    if (meshPopPlaying) {
      if (gui.state.sipState == CallState::Call || gui.state.ringing) {
        audio->restore();
        meshPopPlaying = false;
        meshPopCutMusic = false;             // the call has the device: musicPlayerYieldForCall's job
        log_e("NOTIFY: pop cut after %lu ms (call/ringing)", (unsigned long)(tNow - meshPopStartMs));
      } else if (elapsedMillis(tNow, meshPopStartMs, MESH_POP_MS)) {
        if (audio->musicPlaying()) {
          /* The pop does not own the device any more: a track started over it without
           * finishing it first (music_player's startTrack() does finish it; this is the backstop
           * for a caller that does not). ceasePlayback() here stopped THAT track and restore()
           * pulled its ring and levels out from under it. Its configuration stands; only the
           * snapshot goes, or the next pop would restore this one's instead of its own. */
          audio->discardPreserved();
          meshPopCutMusic = false;
          log_e("NOTIFY: pop already replaced by music after %lu ms - snapshot dropped",
                (unsigned long)(tNow - meshPopStartMs));
        } else {
          audio->ceasePlayback();
          audio->restore();
          log_e("NOTIFY: pop stopped after %lu ms", (unsigned long)(tNow - meshPopStartMs));
          /* 🛑 THE TRACK THE POP CUT CARRIES ON - AFTER restore() (the "pop first, then the music"
           * rule: restore() puts the pre-pop levels and ring back, and the resume then installs
           * music's own ring over that, exactly what F1 did). Until 2026-09-26 the cut was kept
           * as a pause at its place and waited for F1 - {user}'s first chirp over music: "it
           * stopped playing the song". Only a track that was PLAYING when the pop started
           * (meshPopCutMusic): a pause the user made stays a pause (music_player's
           * s_pausedByStop), and a call that cut the pop takes the other branch.
           * ⚠ meshPopPlaying goes false BEFORE the resume: startTrack() finishes "a pop in
           * flight" first (notifyPopFinishFor), and with the flag still up that was a second
           * ceasePlayback()+restore() on a pop already torn down (seen on the first bench). */
          meshPopPlaying = false;
          if (meshPopCutMusic) {
            meshPopCutMusic = false;
            if (musicPlayerResumeAfterPop()) {
              log_e("NOTIFY: the track the pop cut carries on at its place");
            }
          }
        }
        meshPopPlaying = false;
      }
    }

    /* Keep WiFi modem sleep applied. See the long note at WIFI_PS_RECHECK_MS: the driver
     * drops it on every station restart and only one code path ever sets it. */
    if (elapsedMillis(now, wifiPsCheckMs, WIFI_PS_RECHECK_MS)) {
      wifiPsCheckMs = now;
      wifi_ps_type_t ps;
      if (esp_wifi_get_ps(&ps) == ESP_OK && ps != WIFI_PS_MIN_MODEM) {
        if (esp_wifi_set_ps(WIFI_PS_MIN_MODEM) == ESP_OK) {
          log_e("WIFI: modem sleep had been reset to %d by a station restart - MIN_MODEM "
                "reapplied. The receiver was running at full power until now.", (int)ps);
        }
      }
    }

    /* Release the audio device once nothing has wanted it for a while. See the long note at
     * audioDeviceBusy(). The stamp is refreshed on every busy pass, so the timeout measures
     * CONTINUOUS idleness, not time since boot. */
    /* 🛑 THE DEVICE-OFF BRANCH IS FIRST, AND IT IS NOT COSMETIC. Without it the stamp survived
     * a shutdown() somebody ELSE performed, so the next acquire that had not yet set its busy
     * flag met a stamp already older than the timeout and was released on its FIRST pass -
     * the watchdog killing the very thing that had just started. Clearing the clock whenever
     * the device is down makes the timeout mean what it says: continuous idleness while
     * POWERED. */
    if (audio) {
      if (!audio->isOn()) {
        audioBusyStampMs = 0;            // device down: there is no idleness to time
      } else if (audioDeviceBusy()) {
        audioBusyStampMs = now;
      } else if (audioBusyStampMs == 0) {
        audioBusyStampMs = now;          // first idle pass after an acquire: start the clock
      } else if (elapsedMillis(now, audioBusyStampMs, AUDIO_IDLE_RELEASE_MS)) {
        audio->shutdown();
        audioBusyStampMs = 0;
        log_e("AUDIO: device was left powered with nothing using it - released after %lu s. "
              "Something acquired it and never called shutdown(); see audioDeviceBusy().",
              (unsigned long)(AUDIO_IDLE_RELEASE_MS / 1000));
      }
    }

    /* The call screen's UP/DOWN, written to configs.ini ONCE the call is over (review R3-1; the
     * note at callLevelStepped()): never with a call live or imminent, never while it rings, never
     * while the device moves samples or holds an RTP session (a 1.5-5 s SPIFFS stall there is a
     * gap in the call, the ring or the track), and CALL_LEVEL_SETTLE_MS after the last press so a
     * run of presses is one write. millis(), not `now`: the press was stamped later in this pass. */
    if (s_callLevelDirty && audio && !gui.inCall() && !gui.state.ringing &&
        !audio->movingSamples() && !audio->rtpSessionArmed() &&
        elapsedMillis(millis(), s_callLevelStepMs, CALL_LEVEL_SETTLE_MS)) {
      callLevelsFlush("after the call");
    }

    /* 🛑 NO RTP SESSION WITHOUT A CALL TO OWN IT — the hot-mic backstop. See rtp_watch.h.
     * The watchdog above cannot do this job: an armed RtpStream counts as moving samples, so an
     * orphaned session looks busy to it forever. Checked whether or not the device is powered —
     * a latched send flag with the codec off is the SAME hazard, armed for the next power-up
     * (that was the original leak). ⚠ LOUD on purpose, like the watchdog: every firing names a
     * path that armed a session and never shut it down, and a real call must NEVER print it.
     * ⚠ HELD WHILE THE MOTOR IS DRIVEN OR A POP IS MID-PLAY, the terms audioDeviceBusy() holds
     * the watchdog above for: shutdown()'s codec power-down on the shared I2C bus disturbs the
     * vibro motor's extender (a motor left running), and a pop's teardown restore()s on top of
     * a device shut down under it. The motor and pop tests come FIRST so rtpOrphanCheck() is
     * not even asked on those passes: its clock keeps the stamp, and it fires on the first
     * pass after the pulse or the pop ends, rather than starting over (a buzz is at most
     * 650 ms, a ring pulse 500 ms by default, a pop up to ~0.9 s; the ring alternates on and off). */
    if (audio && !meshVibroActive && !gui.state.vibroOn && !meshPopPlaying &&
        rtpOrphanCheck(rtpOrphan, audio->rtpSessionArmed(), rtpSessionEntitled(), now)) {
      log_e("AUDIO: RTP session armed with no live call (sip=%d, mic=%d stream=%d port=%u) for "
            "%u ms - shut down. Something armed it and never called shutdown().",
            (int)gui.state.sipState, (int)audio->micOn(), (int)audio->micStreamOut(),
            (unsigned)audio->rtpPeerPort(), (unsigned)RTP_ORPHAN_RELEASE_MS);
      audio->shutdown();
      audioBusyStampMs = 0;
    }

    /* A time zone change, told ONCE (tz_service.h): the mesh popup's banner, held until it can be
     * seen - the screen awake (brightness 0 = asleep: a banner drawn then is never seen), no game
     * (a GUI draw under one deadlocks the LCD's SPI), no mesh banner up. Quiet otherwise: no buzz,
     * no unread mark - the clock changing is news, not a message. */
    if (!meshPopupActive && !gGbcActive && gui.state.screenBrightness > 0 && !gui.state.locked) {
      char tzTitle[40], tzBody[48];
      if (tzSvcTakeNotice(tzTitle, sizeof(tzTitle), tzBody, sizeof(tzBody))) {
        gui.showMeshPopup(tzTitle, tzBody);
        meshPopupActive = true;
        meshPopupShownMs = millis();
      }
    }

    // Auto-dismiss the mesh popup: repaint the screen to erase it after a moment.
    if (meshPopupActive && elapsedMillis(now, meshPopupShownMs, MESH_POPUP_MS)) {
      meshPopupActive = false;
      gui.redrawScreen(true, true, true, true);
    }

//    if (gui.state.ledPleaseTurnOn) {
//      allDigitalWrite(EXTENDER_PIN_B2, HIGH);
//      gui.state.ledPleaseTurnOn = false;
//    }
//    if (gui.state.ledPleaseTurnOff) {
//      allDigitalWrite(EXTENDER_PIN_B2, LOW);
//      gui.state.ledPleaseTurnOff = false;
//    }

    // RINGTONE

    if (gui.state.ringing && gui.state.sipState != CallState::Call) {

      /* ⚠ Both blocks below must respect the ringer mode. isEof() answers TRUE when nothing is
       * playing, so in vibrate-only mode the rewind branch would fire every pass, permanently
       * resetting the vibro state machine and leaving the motor dead — the exact opposite of
       * what "vibrate only" is for. */
      const bool tonePlaying = (gui.state.ringerMode == ControlState::RINGER_RING_AND_VIBRATE);

      // Check if end of ringtone was reached

      if (tonePlaying && audio->isEof()) {

        // Rewind the ringtone file
        audio->rewind();

        // Restart vibro motor logic
        gui.state.vibroOn = false;
        gui.state.vibroToggledMs = now;
        gui.state.vibroNextDelayMs = gui.state.vibroDelayMs;
        allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
        allDigitalWrite(KEYBOARD_LED, HIGH);

      }

      // Control vibro motor

      if (gui.state.ringerMode != ControlState::RINGER_SILENT &&
          elapsedMillis(now, gui.state.vibroToggledMs, gui.state.vibroNextDelayMs)) {

        gui.state.vibroToggledMs = now;
        gui.state.vibroOn = !gui.state.vibroOn;
        gui.state.vibroNextDelayMs = gui.state.vibroOn ? gui.state.vibroOnPeriodMs : gui.state.vibroOffPeriodMs;
        allDigitalWrite(VIBRO_MOTOR_CONTROL, gui.state.vibroOn ? HIGH : LOW);
        allDigitalWrite(KEYBOARD_LED, gui.state.vibroOn ? LOW : HIGH);

      }
    }

    // Audio
    //msProfile.add(micros()-loopTime);
//    uint32_t loopTime = micros();
//    if (!msProfileStart) msProfileStart = loopTime;
    loopPhase("audio-loop");
    audio->loop();

//    // Profiler
//    msProfile.add(micros()-loopTime);
//    if (loopTime - msProfileStart > 2500000) {
//      uint32_t sum = 0, mx = 0, cnt = 0;

//      log_d("Profile:");
//      for (auto it = msProfile.iterator(); it.valid(); ++it) {
//        uint32_t c1 = *it;
//        DEBUG_PRINTF("%d ", c1);      // Before audio
//        ++it;
//        log_d("%d", *it - c1);        // After audio
//        sum += *it;
//        mx = (mx < *it) ? *it : mx;
//        cnt++;
//      }
//      log_d("Loop: avg = %.1f us, max = %d", (float)sum/cnt, mx);
//      msProfile.purge();
//      msProfileStart = 0;

//      log_d("Audio profile:");
//      for (auto it = audio->profile.iterator(); it.valid(); it++) {
//        uint32_t tot = (*it).time[6] - (*it).time[0];
//        sum += tot;
//        mx = (mx < tot) ? tot : mx;
//        cnt++;
//        it->show();
//      }
//      log_d("Audio loop: sum = %d, avg = %.1f us, max = %d, cnt = %d", sum, (float)sum/cnt, mx, cnt);
//      audio->profile.purge();

//      #ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
//      showRunTimeStats();
//      #endif
//    }

    // Theoretically, gives time for modem sleep? Allows to consume less power?
    //delay(1);   // sleep for 1 millisecond
    //vTaskDelay(1);    // sleep for a single tick: allows context switch
    /* ── RUN SLOW WHEN NOBODY IS WATCHING ─────────────────────────────────────────
     * The chip is pinned at 240 MHz by platformio.ini because the Game Boy needs it, and
     * it stayed there in a pocket with the screen off. At 80 MHz the core draws roughly
     * half as much, and the phone spends most of its life doing nothing but listening.
     *
     * Full speed whenever anything would notice (⚠ since 0.9.79 "full speed" is 240 only with
     * the WiFi radio off - with it running it is 160, the most PLL 320 gives, because moving to
     * 240 would re-lock the PLL under the radio: cpu_clock_policy.h):
     *   - the screen is on (someone is looking, and redraws should feel instant)
     *   - music is playing (the decoder has headroom at 240 and less of it at 80)
     *   - a call is up (RTP is a hard 20 ms deadline)
     *   - the Game Boy is running (the reason 240 was pinned in the first place)
     *   - an upload is in progress (throughput is already marginal on a weak link)
     *
     * ⚠ 80 is the floor, not lower: WiFi needs at least 80 MHz to work at all. And the
     * switch only happens on a CHANGE — calling this every pass would be its own drain.
     *
     * ⚠ Not applied while the radio or I2S are mid-transfer by design: everything in the
     * "full speed" list above covers those cases, so the frequency only ever moves when
     * the phone is genuinely idle. */
    loopPhase("cpu-tick");
    bool idleTickStretch = false;
    {
      /* ── TWO PREDICATES, NOT ONE ──────────────────────────────────────────────
       * `busy` used to decide BOTH the CPU frequency AND the idle tick below, and that
       * coupling is a trap: loosening it to save power under a lit screen would also
       * hand a lit screen the 5 ms tick, slowing every pump in the loop. They are
       * separated here deliberately. `busy` keeps its ORIGINAL meaning and remains the
       * ONLY input to the tick. Do not re-merge them.
       *
       * The frequency question is different from the tick question: a lit screen that is
       * merely being STARED AT does not need 240 MHz, while one being redrawn does. On
       * this hardware that is a safe distinction to make, because a redraw is bus-bound,
       * not CPU-bound - the whole 153,600-byte frame goes out over a 40 MHz SPI that does
       * NOT scale with the core clock (in this core `calculateApb()` returns a flat
       * 80 MHz for any CPU frequency >= 80, so an 80<->240 move reprograms no peripheral
       * at all). ⚠ Never target below 80 MHz: there the clock source becomes the XTAL,
       * APB really does follow, and every SPI divider in the phone silently goes wrong.
       *
       * UI_WORK_HOLD_MS is a transition-rate knob, NOT a responsiveness one: the wake
       * path already repaints the screen before this gate is reached, and any keypress
       * raises the clock on the pass that handles it, so the frame a person actually
       * perceives is never the slow one. Set UI_IDLE_DOWNCLOCK to 0 to restore the
       * previous behaviour exactly. */
/* ⚠ 0 = OFF, AND IT IS OFF ON PURPOSE. Tried on hardware 2026-08-22 and BACKED OUT
 * the same evening: {user} reported "the menu is a bit laggy and doesn't pick up every
 * button push". Missed input is not a tuning problem, it is a break, so this does not
 * get dialled down to 160 - it gets turned off until it can be done without touching
 * the input path.
 *
 * WHAT THE EVIDENCE ACTUALLY SAID, because it was not what I expected: the new
 * "screen idle" state NEVER FIRED - zero occurrences in the whole session, only "busy"
 * and "idle". So the phone was never running its menus at 80 MHz, and the lag did not
 * come from the low clock at all. It came from the other half: cpuRaiseForUi() calls
 * setCpuFrequencyMhz() from INSIDE the key-drain loop, and a PLL switch landing in the
 * middle of a keypad read is exactly how a keypress goes missing. The level was never
 * the problem; the SWITCHING was, which is why 160 MHz would not have helped.
 *
 * If this is ever revisited: raise the clock somewhere that is not the input path, and
 * MEASURE the saving first - it was never quantified, and the backlight probably
 * dominates screen-on draw anyway. Trading proven input handling for an unmeasured
 * saving was the wrong bet and this comment is here so nobody repeats it. */
#define UI_IDLE_DOWNCLOCK   0
#define UI_WORK_HOLD_MS     2000
      /* 🛑 gGpsNmea HOLDS THE CLOCK, AND IT IS NOT ABOUT PERFORMANCE — IT AVOIDS A HARD
       * DEADLOCK IN THE ARDUINO CORE. Every HardwareSerial registers uart_on_apb_change()
       * (esp32-hal-uart.c:225). On APB_BEFORE_CHANGE that callback disables the UART RX
       * interrupt and then drains the hardware FIFO into the 256-byte RX queue with
       * xQueueSend(..., 1) — a ONE-TICK BLOCK, and CONFIG_FREERTOS_HZ is 1000, so one
       * millisecond per byte once that queue is full. The queue's only consumer is this
       * loop, which is at that moment inside the callback, so nothing can ever free a slot.
       *
       * 🔑 THE ARITHMETIC IS WHAT DECIDES IT, AND 115200 IS THE WRONG SIDE OF IT:
       *      115200 -> 11.52 bytes arrive per ms, 1 drains per ms -> +10.5/ms, never exits
       *        9600 ->  0.96 bytes arrive per ms, 1 drains per ms -> converges, exits
       * So with the plate's GPS streaming, ANY setCpuFrequencyMhz() call can park this task
       * forever. MEASURED 2026-08-25: the loop stops, other tasks keep printing, CPU 1 sits
       * in IDLE1 (it BLOCKS, it does not spin), and no watchdog fires because only core 0's
       * idle task is checked. ⚠ And note esp32-hal-cpu.c:190-196 fires the callback on ANY
       * frequency change, even 80<->240 where APB does not actually move — the guard there
       * covers only the register write, not the callback.
       *
       * ⚠ Why it looked like "mostly on screen unlock": the wake REPAINT (~30.7 ms of SPI)
       * runs BEFORE the clock is raised, so ~354 bytes pile into a 256-byte queue and the
       * threshold is crossed every time. Screen-OFF is preceded by a backlight write, not a
       * repaint — a short pass, queue not full, which is why the downclock so often survived
       * and sent this hunt after the wrong transition.
       *
       * The cost is idle power while the GPS is on, and that is the right trade against a
       * phone that stops answering its buttons.
       * ⚠ 0.9.79: cpu_clock.cpp's method NEW (the default) switches without arduino's APB
       * callbacks, so it cannot take this deadlock at all - but gGpsNmea STAYS in `busy`, so
       * phone 2's GPS still holds the clock at FULL SPEED. That is no longer "240 as it always
       * was": full speed is 240 with the radio off and 160 on PLL 320 while WiFi runs (method
       * new may not re-lock the PLL under the radio - cpu_clock_policy.h), so with WiFi on
       * phone 2 now runs at 160. `cpu method old` would bring the callback back, which is why
       * it is refused while the reader is on, and `gps on` is refused under it (serial_cmd.cpp). */
      /* The map downloader PAUSES for anything that owns the audio path or the heap: a live
       * or imminent call (its TLS handshake dips internal RAM to ~4 KB, measured), the Game
       * Boy (it owns the card and the SPI bus, and turns WiFi off), and a WiFi that is not
       * associated (a GET into a dead socket is a 15 s timeout per tile, and the rescue below
       * is what brings the network back — it must not be raced). It resumes by itself when
       * they clear. */
      tileFetchPause(sipNeedsFullSpeed(), gGbcActive, !wifiState.isConnected());
      /* ...and a job that STOPPED (a WiFi drop past 20 s, a long call, failures, a flat
       * battery, a restart) is resumed from here, under tile_plan.h's rules. A compare or two
       * per pass; the real work is once a second, and NVS is touched only when a decision
       * fires (tile_fetch.h). */
      {
        TileFetchWorld tw;
        tw.wifiUp = wifiState.isConnected();
        tw.ssid = wifiState.ssid();
        tw.usb = gui.state.usbConnected;
        tw.volts = gui.state.battVoltage;
        tw.card = gui.state.cardPresent;
        tw.filesJob = filesJobActive();
        tileFetchTick(&tw);
      }
      /* Menu > AI: a question the worker has finished goes into the saved chat (and the card) here,
       * on the loop task - whether or not the app is open (ai_net.h). A flag test when idle. */
      aiLoopTick();
      /* The Almanac's weather the same way (weather_net.h): a finished fetch folded into the cache
       * and written to the card here, whether or not the Almanac is open. A flag test when idle. */
      wxLoopTick();
      /* Wi-Fi location (geoloc_net.h): a finished beaconDB look folded into the service's Wi-Fi
       * slot (a flag test when idle), then once a second the decisions - look, ask the
       * auto-switcher for a scan, a dry look. The position is "wanted" while Maps or the Almanac
       * is on a lit screen (and 10 min after). Its handshake rides netRequestActive() below. */
      geolocLoop(gui.state.screenBrightness > 0 &&
                 (gui.isAppRunning(GUI_APP_MAPS) || gui.isAppRunning(GUI_APP_ALMANAC)));
      const bool busy = (gui.state.screenBrightness > 0) ||
                        gGbcActive ||
                        gGpsNmea ||            // see the deadlock note above — NOT perf
                        xferOn() ||            // the UPLOADER; a KOSync window needs no full speed (app_gbc_xfer.cpp)
                        tileFetchActive() ||   // tiles: HTTPS + decode + card, see tile_fetch.h
                        netRequestActive() ||  // an AI question, a weather fetch or a Wi-Fi location look: TLS on the shared worker (https_worker.h)
                        musicPlayerIsPlaying() ||
                        sipNeedsFullSpeed();   // NOT sipCallActive() — see the note on it
      /* Anything with a deadline stays at FULL SPEED REGARDLESS of the screen: the emulator,
       * the transfer server, audio playback and a live SIP session. Only the screen term is
       * relaxed, and only while nothing is being drawn. ⚠ Full speed is what cpu_clock.cpp
       * gives `wantFull`: 240 with the radio off, 160 on PLL 320 while it runs (method new).
       * So the uploader, a SIP call, tiles and music with WiFi up all run at 160 now; 240 is
       * left to work done with the radio off (the Game Boy turns it off first). */
      const bool hardBusy = gGbcActive || gGpsNmea || xferOn() || tileFetchActive() ||
                            netRequestActive() ||  // the handshake at full speed (160 with WiFi on), never 80
                            musicPlayerIsPlaying() || sipNeedsFullSpeed();
      extern volatile uint32_t gUiWorkMs;      // GUI.cpp: stamped by every redraw
      const bool uiWorking = (uint32_t)(millis() - gUiWorkMs) < UI_WORK_HOLD_MS;
#if UI_IDLE_DOWNCLOCK
      const bool wantFull = hardBusy || (gui.state.screenBrightness > 0 && uiWorking);
#else
      const bool wantFull = busy;
#endif
      /* 🛑 FULL/IDLE IS DECIDED HERE; WHAT THAT MEANS IN MHz IS cpu_clock.cpp's (0.9.79 dev).
       * This was setCpuFrequencyMhz(wantFull ? 240 : 80), and on this chip that is not a
       * divider: 240 runs off PLL 480 and 80 off PLL 320 (ESP32 TRM Table 7.2-2 - there is no
       * 480/6), so every change re-locked the BBPLL that the WiFi radio runs from. MEASURED on
       * phone 1, 2026-09-25: 240->80 straight after the uploader's traffic broke WiFi 4 of 4
       * (disassoc ~10 s later, then deaf until a radio bounce); CPU held at 240, 0 of 3; 20
       * switches with an idle radio, 0 of 20. Under method NEW (the default) the PLL is never
       * re-locked while the radio runs: with WiFi on, full = 160 and idle = 80, both off PLL
       * 320, a divider write; with it off, 240/80 exactly as before. `cpu method old` puts the
       * old call back for the A/B, `cpu` says what happened (cpu_clock_policy.h has the rest).
       * Label the reason the ACTIVE predicate gave, not the experimental one - with
       * UI_IDLE_DOWNCLOCK off, a lit screen goes to full speed via `busy`, and calling that
       * "screen idle" in the log is how you mislead the next person reading it. */
      cpuClockGate(wantFull,
#if UI_IDLE_DOWNCLOCK
                   hardBusy ? "busy" : (gui.state.screenBrightness > 0 ? "screen idle" : "idle"));
#else
                   busy ? "busy" : "idle");
#endif
      /* The same predicate decides the TICK below — plus one extra gate: while the LAN
       * mirror poll is mid-transfer its state machine advances one bounded step per pass,
       * so stretching the tick then would slow a live download 5x for no saving worth
       * having. */
      idleTickStretch = !busy && !smsMirrorPollBusy() && !kosyncClientBusy();
    }

    /* ── LET THE CPU ACTUALLY IDLE ────────────────────────────────────────────────
     * This was taskYIELD(), which hands over to any ready task and comes straight back
     * — the main loop spun flat out at 240 MHz forever, even with the screen off and
     * nothing happening. The CPU never got to sleep, which is most of why the battery
     * went nowhere.
     *
     * vTaskDelay(1) blocks for one tick instead, so the FreeRTOS idle task runs and
     * executes the Xtensa `waiti` instruction, halting the core until the next interrupt.
     *
     * ⚠ One TICK, not one millisecond of dead time in anything that matters. Audio
     * decodes ahead in ~24 ms frames, RTP packets are 20 ms, and the keypad is
     * interrupt-driven, so none of them notice. The Game Boy runs its own inner loop and
     * never reaches this line.
     *
     * FIVE ticks when the phone is verifiably idle (same predicate as the CPU-MHz gate,
     * plus the mirror poll being between transfers): every pass still runs SIP socket
     * checks, the mesh poll gate, and a dozen timer compares, so 1,000 passes a second
     * kept the core awake 10-30% of idle time doing nothing. At 5 ms the worst added
     * latency is 4 ms — on keypad wake (interrupt-latched), on a LoRa packet that spent
     * >100 ms on air, on sockets that already ride network jitter. Nothing a person or a
     * protocol can perceive, for roughly a fifth of the idle wakeups. */
    /* TEN ticks while an AI question is in flight (0.9.81). Its TLS handshake is ~4.5 s of
     * software ECC on the AI worker, which shares this core at this task's priority - and
     * Google resets a handshake that takes more than ~10 s. With the 1-tick delay above the two
     * tasks split the core about evenly: MEASURED 2026-10-03 on phone 1, a question took 5.6-10 s
     * a request with the screen off and FAILED 3 of 3 with it on (`start_ssl_client: -80`,
     * connection reset by peer). Sleeping 10 ms a pass hands the worker ~90 % of the core for the
     * few seconds a question lasts; the keypad is interrupt-latched and nothing else that a
     * question allows (aiAsk refuses during a call, a game, a download, the uploader) needs a
     * finer tick. (A question also pauses the music: aiAsk.) Since 0.9.81 the same for a weather
     * fetch on the same worker (https_worker.h: netRequestActive) - two handshakes, the same cost,
     * the same gates (a Refresh pauses the music; an automatic fetch never starts beside it). */
    const bool netNeedsCore = netRequestActive();
    vTaskDelay(netNeedsCore ? 10 : (idleTickStretch ? 5 : 1));

    //esp_sleep_enable_timer_wakeup(1000000); // 0.001 s
    //int ret = esp_light_sleep_start();
    //if (ret != ESP_OK) printf("light sleep error: %d\r\n", ret);
  }
}

void powerOff() {
  log_i("POWER OFF");
  allDigitalWrite(POWER_CONTROL, HIGH);       // produces a power down from software
  msPowerOffStarted = millis();
  poweringOff = true;
}
