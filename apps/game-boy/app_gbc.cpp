/*
 * app_gbc.cpp — WiPhone Game Boy Color emulator (Phase 6: dual-core overlap, diagnostic)
 *
 * Two tasks + two framebuffers overlap emulation with the screen push:
 *   - emuThread (core 1): poll keypad, emulate+render into the back buffer, hand
 *     it to the blit task, switch buffers. Never touches the LCD or SD.
 *   - blitTask: owns ALL LCD and SD. Pushes finished frames while the emulator
 *     renders the next; when paused, runs the menu + save/load here too so the two
 *     cores never hit the shared SPI bus at the same time.
 *
 * Both tasks are left unpinned (pinning to core 1 starved them) and the core
 * watchdogs are disabled while playing (the two tasks saturate both cores). The
 * emulator paces to 60fps and only renders frames the blit can show, so game
 * speed and input stay correct (and authentic) even when the display can't keep up.
 */

#include "app_gbc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"     // esp_reset_reason (transfer-screen diagnostics)
#include "esp_bt.h"
#include "Networks.h"   // wifiRestoreStation(): the station back after a game, as its owner has it
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "WiFi.h"
#include "SD.h"
#include <Preferences.h>    // the Screen 1:1/Fill choice, remembered across games
#include "driver/i2s.h"
#include "Audio.h"
#include "music_player.h"
#include "src/gnuboy/gnuboy.h"
#include "gbc_test_rom.h"   // embedded public-domain ROM (flash) used when no SD ROM
#include "app_gbc_xfer.h"   // ROM transfer web server (UI lives on our Transfer screen)
#include "kosync_sync.h"   // kosyncWindowClose: a game ends any sync window
#include "cpu_clock.h"     // cpuClockRaise: 240 MHz the moment the radio is off

// Global held-button mask, updated by the main loop's keypad scanner.
extern uint32_t keypadState;
// The phone's shared audio subsystem (I2S + codec + amp). We borrow it during play.
extern Audio* audio;
// When true, the main loop stops polling the mesh/LoRa radio (frees the SPI bus).
extern volatile bool gGbcActive;
extern void notifyPopFinishNow();   // WiPhone.ino: end a notification chirp before the device is borrowed
// Sticky press mask so fast taps between the emulator's frame-rate polls survive.
extern volatile uint32_t gGbcKeyLatch;

// The I2S/codec runs at 32000. The emulator's APU rate is chosen slightly HIGHER
// on purpose: gnuboy rounds its internal step to (1<<21)/rate = 65, producing
// 32264 samples per emulated second — a ~0.8% surplus over the DAC. That keeps
// the DMA buffer full, so the blocking i2s_write paces the game to the DAC
// (continuous audio, ~99% speed) instead of slowly draining into underruns.
#define GBC_I2S_RATE         32000
#define GBC_EMU_SAMPLERATE   32264
#define GBC_EMU_AUDIO_LEN    2048   // int16 samples in the emulator's audio buffer
// In-game volume step (F1/F2). Codec volume is in dB-ish units (see Audio.h).
#define GBC_VOL_STEP         3

// Picker rows 0..GBC_PICKER_ACTIONS-1 are actions (Transfer, Help); ROMs follow.
#define GBC_PICKER_ACTIONS 2
// Names longer than this scroll (marquee) while selected. ~What fits from x=28
// in the condensed bold-20 font on the 240px screen.
#define GBC_PICKER_VIS_CHARS 18

// Pause-menu items and pending actions.
#define GBC_MENU_RESUME  0
#define GBC_MENU_SAVE    1
#define GBC_MENU_LOAD    2
#define GBC_MENU_CLEAR   3
#define GBC_MENU_SCREEN  4
#define GBC_MENU_QUIT    5
#define GBC_MENU_COUNT   6
// One SD job at a time for the blit task (runAction). The menu's OK is ignored while one
// is in flight, so a job is never overwritten mid-write.
#define GBC_ACT_NONE     0
#define GBC_ACT_SAVE     1   // menu: write the manual slot
#define GBC_ACT_LOAD     2   // menu: load the manual slot, resume on success
#define GBC_ACT_CLEAR    3   // menu (confirmed): delete both slots, hard-reset, resume
#define GBC_ACT_RESUME   4   // launch: load the resume point if there is one, then run
#define GBC_ACT_AUTOSAVE 5   // quit / power-off: write the resume point, stay parked

// Two files per game under /gbc/, from the sanitized ROM name (buildStatePath):
//   <rom>.state  the MANUAL slot — Save state / Load state, a checkpoint a quit never touches
//   <rom>.auto   the RESUME POINT — written on quit and power-off, loaded on the next launch
// Separate on purpose (RetroArch keeps <rom>.state.auto beside its numbered slots, and that
// is what COVEY runs): with one file a manual save would be pointless, since quitting
// overwrites it anyway. Written via <file>.tmp + rename so a power-off mid-write leaves the
// previous file whole instead of a torn one that loads as garbage.
#define GBC_EXT_STATE    ".state"
#define GBC_EXT_AUTO     ".auto"
// How long the main thread waits for the blit task to write the resume point. A CGB state
// with 32 KB of cart RAM is 21 x 4 KB blocks; an MBC5 cart with 128 KB is 45 (180 KB). The
// card writes at a few hundred KB/s over SPI, so a second is typical; the ceiling is for a
// card having a bad day. The first measured figures are in CHANGELOG 0.9.67.
#define GBC_AUTOSAVE_WAIT_MS  6000u
// NVS: the Screen toggle (int "fill", default 1).
static const char GBC_NVS[] = "gbc";

// Fill mode: nearest-neighbor 1.5x upscale of 160x144 -> 240x216.
#define GBC_FILL_W  240
#define GBC_FILL_H  216

// setup() error codes (negative), surfaced on the error screen.
#define GBC_ERR_ALLOC   -2
#define GBC_ERR_INIT    -3
#define GBC_ERR_LOAD    -4
#define GBC_ERR_ROMBIG  -5   // ROM larger than the largest free PSRAM block
#define GBC_ERR_READ    -6   // SD open/read failed
#define GBC_ERR_TASK    -9

// The app on screen, for gbcSaveForPowerOff() (the maps app's s_instance pattern).
static GbcApp*   s_instance = NULL;

// gnuboy uses global state and gb_hw_init() has no matching free, so the core and
// framebuffers are set up once per boot and reused across launches.
static bool      s_gnuboyInited = false;
static uint16_t* s_emuFb[2] = { NULL, NULL };  // double buffer, RGB565 big-endian (PSRAM)
static uint16_t  s_xmap[GBC_FILL_W];           // precomputed nearest-neighbor column map
static int16_t*  s_emuAudio = NULL;
static uint8_t*  s_emuRom   = NULL;
static bool      s_romLoaded = false;   // gnuboy currently holds a loaded ROM

// Release the current ROM: gnuboy's references first, then the PSRAM buffer they
// point into. Idempotent, so it's safe on exit and before loading a new ROM.
static void gbcUnloadRom() {
  if (s_romLoaded) {
    gnuboy_free_rom();
    s_romLoaded = false;
  }
  if (s_emuRom) {
    free(s_emuRom);
    s_emuRom = NULL;
  }
}

// Free everything the emulator keeps resident (gnuboy WRAM/VRAM, task stacks,
// audio buffer, framebuffers, loaded ROM). Called by the transfer screen (the
// WiFi/web stack needs the RAM) and, since 0.9.75, by the destructor — a quit
// gives the phone its RAM back. The next game launch re-allocates and re-inits
// everything from scratch.
static void gbcReleaseEmulator();

// Serializes the SD card (ROM bank streaming, from the emu task) against the
// LCD (blit task) — they share one SPI bus. Created once, never deleted.
static SemaphoreHandle_t s_spiBusLock = NULL;

static void gbcSpiLock() {
  if (s_spiBusLock) {
    xSemaphoreTake(s_spiBusLock, portMAX_DELAY);
  }
}
static void gbcSpiUnlock() {
  if (s_spiBusLock) {
    xSemaphoreGive(s_spiBusLock);
  }
}

// Per-launch task handoff.
static SemaphoreHandle_t s_blitGo   = NULL;   // emu -> blit: buffer s_blitIdx is ready
static TaskHandle_t      s_blitTask = NULL;
static volatile int      s_blitIdx = 0;
static volatile bool     s_blitBusy = false;  // true while the blit task is pushing a frame
static volatile bool     s_running = false;
static volatile bool     s_blitExited = false;
static volatile bool     s_emuExited = false;

// Task stacks/TCBs allocated once and reused every game (xTaskCreateStatic), so
// launching a game never allocates — deleting an unpinned task defers its stack
// free to the (starved) idle task, which otherwise runs the phone out of RAM
// after one game ("could not start task").
// NOTE: StackType_t is 1 byte on ESP-IDF FreeRTOS, so these are sizes in BYTES.
// After gnuboy_init claims WRAM/VRAM, free internal RAM is ~32KB with a largest
// contiguous block of ~9.6KB (measured on-device) — so only ONE 8KB block exists.
// The blit task gets it: it runs the deep fopen->FATFS->SD save/load path that
// overflowed the old 2KB stacks and rebooted the phone. The emulator loop is
// shallow (it ran indefinitely on 2KB without crashing), so 4KB is a 2x margin.
#define GBC_EMU_STACK_BYTES   4096
#define GBC_BLIT_STACK_BYTES  8192
static StaticTask_t s_emuTcb;
static StaticTask_t s_blitTcb;
// Stacks live in internal RAM but are allocated from the heap at launch (after gaming
// mode has freed WiFi) and freed again when the app closes (gbcReleaseEmulator, 0.9.75).
// Keeping them out of BSS matters: ~20KB of permanent static internal RAM
// starves the boot-time WiFi allocations and boot-loops this RAM-tight board.
static StackType_t* s_emuStack  = NULL;
static StackType_t* s_blitStack = NULL;

static void gbcReleaseEmulator() {
  gbcUnloadRom();
  if (s_gnuboyInited) {
    gnuboy_deinit();
    s_gnuboyInited = false;
  }
  if (s_emuAudio)  { free(s_emuAudio);  s_emuAudio  = NULL; }
  if (s_emuFb[0])  { free(s_emuFb[0]);  s_emuFb[0]  = NULL; }
  if (s_emuFb[1])  { free(s_emuFb[1]);  s_emuFb[1]  = NULL; }
  if (s_emuStack)  { free(s_emuStack);  s_emuStack  = NULL; }
  if (s_blitStack) { free(s_blitStack); s_blitStack = NULL; }
}

GbcApp::GbcApp(LCD& disp, ControlState& state) : ThreadedApp(disp, state) {
  log_d("create GbcApp");
  s_instance = this;
  scanRoms();               // build the picker list; the game starts on selection
  romSel = GBC_PICKER_ACTIONS;   // land on the first game, not the action rows
  {
    /* Screen: Fill unless the user switched to 1:1 last time. Opened read-WRITE: a read-only
     * open of a namespace nothing has written yet fails with an [E] NOT_FOUND line on every
     * picker open; read-write creates it once. */
    Preferences p;
    if (p.begin(GBC_NVS, false)) {
      scaled = p.getInt("fill", 1) != 0;
      p.end();
    }
  }
  // ~4Hz APP_TIMER_EVENT drives the picker's marquee for long ROM names (the
  // GUI saves/restores the previous period around this app's lifetime).
  controlState.msAppTimerEventPeriod = 250;
}

// Enter gaming mode and launch the selected ROM on the two emulator tasks.
void GbcApp::startGame() {
  // Gaming mode: stop mesh/LoRa polling, turn WiFi off (frees CPU + the SPI bus),
  // and disable the core watchdogs (the two tasks saturate both cores).
  gGbcActive = true;
  enteredGaming = true;
  /* 🛑 THE CORE'S AUTO-RECONNECT IS OFF FOR THE WHOLE GAME (0.9.79 dev, review 2026-09-25).
   * The core's event task answers a STA_DISCONNECTED with reason >= 200 (not 202) by
   * `WiFi.disconnect(); WiFi.begin();` (WiFiGeneric.cpp:404-410), and begin() on a radio that
   * disconnect(true) below has just stopped STARTS IT AGAIN (_esp_wifi_started is already false,
   * so mode(STA) runs esp_wifi_start). One such event still queued when the game starts - an
   * out-of-range NO_AP_FOUND storm is exactly that - put the radio back up under the emulator:
   * an old race (WiFi competing for the internal RAM the emulator just took), and since the
   * cpu_clock wrapper, also a PLL re-lock 240->160 with the emulator and I2S running, after
   * which the game stays at 160 (the gate holds PLL 320 while the radio runs). Held BEFORE the
   * disconnect and the KOSync teardown so no event handled after them can restart it; released
   * in ~GbcApp before the station is brought back.
   * ⚠ A HOLD, NOT A SAVED COPY (review R1). This used to save getAutoReconnect() and put it back.
   * A sync window's hotspot now holds the same flag, and the kosyncWindowClose() below ends the
   * window's scope INSIDE the game's: a saved copy would be re-armed mid-game by the window and
   * then "restored" to the window's cleared value for the rest of the boot. The flag is computed
   * from the holds (wifi_policy.h), so the order the scopes end in cannot matter. */
  wifiAutoReconnectHold(WIFI_AR_HOLD_GAME);
  /* 🛑 A KOSync sync window ends HERE, before the radio goes off (0.9.79). Left open it would
   * count down over a dead radio and, at its deadline, take its hotspot down with a
   * WiFi.begin() in the middle of the game — with the watchdogs off and the emulator holding
   * the internal RAM. After gGbcActive on purpose: the teardown sees the game and leaves the
   * station OFF (app_gbc_xfer.cpp transportDown); the game's exit below restores it. */
  kosyncWindowClose("a Game Boy game started");
  /* ...and so does an UPLOADER (0.9.79). No screen can have one up here — the picker's
   * Transfer row stops its own on Back — but serial `up on` / wiphone_send.py start one
   * HEADLESS, and it used to survive the game: the radio went off under its hotspot while
   * xferServing()/xferUsingAP() went on saying "up" for the whole game. Same order, same
   * reason as the window: gGbcActive is already set, so the teardown's restore says OFF. */
  gbcXferStop();
  WiFi.scanDelete();          // drop any lingering auto-switch scan results
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  /* 🛑 240 MHz NOW, WITH THE RADIO JUST STOPPED - not at the bottom of the next loop pass
   * (0.9.79 dev). With WiFi on, the clock gate's "full speed" is 160 on PLL 320 (it may not
   * re-lock the PLL under a running radio: cpu_clock_policy.h), and the move to 240 is a
   * re-lock, allowed only once the radio is off - which is this line. Done before the
   * emulator and its audio start, so the ~100 us XTAL window of the re-lock lands on nothing.
   * The gate agrees on every pass after (gGbcActive is in `busy`). No-op under method old,
   * where the lit screen already has it at 240. */
  cpuClockRaise("gbc");
  disableCore0WDT();
  disableCore1WDT();
  reclaimInternalRam();       // a no-op since 0.9.74 (the reserve is released at boot); harmless

  // Allocate the task stacks FIRST, while internal RAM is at its emptiest and
  // least fragmented (WiFi/BT just freed, gnuboy hasn't claimed WRAM/VRAM yet,
  // the ROM loader hasn't touched the heap). Allocating them last failed with
  // "largest block 6.6KB" once background WiFi scans fragmented the heap.
  // Blit stack first: it needs the largest contiguous block.
#if defined(configSUPPORT_STATIC_ALLOCATION) && configSUPPORT_STATIC_ALLOCATION
  if (!s_blitStack) {
    s_blitStack = (StackType_t*)heap_caps_malloc(GBC_BLIT_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!s_emuStack) {
    s_emuStack = (StackType_t*)heap_caps_malloc(GBC_EMU_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!s_emuStack || !s_blitStack) {
    dbgWhere = 2;
    dbgFreeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    dbgLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    initErr = GBC_ERR_ALLOC;
    return;
  }
#endif

  if (!setupEmulator(roms[romSel - GBC_PICKER_ACTIONS])) {
    return;   // initErr set; redrawScreen() shows the error
  }

  // Borrow the phone's audio path for the emulator's APU output. Best-effort:
  // if it won't start, the game just runs silent (never block gameplay on it).
  if (audio) {
    /* ⚠ Stop the music FIRST. This emulator drives the DAC with its own i2s_write()
     * calls, bypassing Audio's playback state machine entirely, so a track still
     * playing would interleave its frames with the game's — and both would sound like
     * noise. It also reconfigures the sample rate below and calls audio->shutdown() on
     * the way out, which would leave the player pointing at a dead I2S.
     *
     * Paused rather than stopped, so the track is still there to resume afterwards.
     *
     * 🛑 A CHIRP STILL IN FLIGHT IS FINISHED BEFORE ANY OF IT (review, 2026-09-25) — the ring's
     * and a call's order: the pop first, then the music, then this app's own settings. It used
     * to be finished further down, AFTER the rate and channel format below, so its restore()
     * put the pre-pop rate and mono back over the game's — and after a music session that is
     * 22.05 kHz mono: the 50%-speed game this block exists to prevent. */
    notifyPopFinishNow();
    musicPlayerPause();
    audio->setSampleRate(GBC_I2S_RATE);

    /* ⚠ SET THE CHANNEL FORMAT TOO, DO NOT INHERIT IT — THIS IS A 2x SPEED BUG.
     *
     * gnuboy_get_audio_count() returns a STEREO INTERLEAVED int16 count, and the blocking
     * i2s_write below is deliberately "the de facto clock" for the whole emulator. So the
     * game's speed is set by how fast the DAC drains, which means the I2S CHANNEL FORMAT
     * is a timing parameter, not a cosmetic one.
     *
     * setSampleRate() only calls i2s_set_sample_rates(); it does NOT touch
     * channel_format, which configureI2S() derives from Audio::monoOut. This code used to
     * set the rate and inherit whatever monoOut some earlier user of the audio path had
     * left behind. Audio::monoOut defaults to false, so for a long time it was stereo by
     * luck and the game ran at 100%.
     *
     * Then the music player started following the headphone jack (stereo on headphones,
     * mono to the loudspeaker) and began leaving monoOut = true. With
     * I2S_CHANNEL_FMT_ONLY_LEFT the DAC consumes 32000 x 1 x 2 = 64,000 B/s while the
     * emulator produces 32264 x 2 x 2 = 129,056 B/s, so the write blocks for twice as
     * long and the game is paced at 50.4%. {user} measured 50% on Metroid II.
     *
     * ⚠ Frameskip CANNOT rescue this and will hide the cause: the adaptive skip below
     * only drops DISPLAY frames, so at 50% it just pins itself at maximum skip while the
     * game stays in slow motion. Do not read a maxed-out skip as "the CPU is too slow".
     *
     * Forcing false restores exactly the configuration this ran at when it was at 100%.
     * Every other consumer (music, calls) sets what it needs on the way in, so there is
     * nothing to restore on the way out. */
    audio->setMonoOutput(false);
    /* The ring setSampleRate()/setMonoOutput() above (re)installed was placed with the stacks, VRAM
     * and audio buffer already in the heap; ~GbcApp reinstalls it once they are freed, or it
     * splits the phone's largest block for the rest of the boot (Audio::reseatI2S()). */
    ringReseat = true;

    /* ⚠ CHOOSE THE OUTPUT TOO, FOR THE SAME REASON: NOTHING HERE CHOSE, SO A GAME INHERITED
     * WHATEVER THE LAST USER LEFT. start() builds the codec's power mask from
     * Audio::loudspeaker (headphones > loudspeaker > earpiece) and switches on the separate
     * amplifier IC only for the loudspeaker; shutdown() never resets the flag, and the
     * phone's default is the EARPIECE. Right for a call, where the phone is against your
     * head; for a game held at arm's length it is the tiny one-ear speaker, and the sound
     * read as missing. {user}, 2026-09-19: the sound plays through the earpiece.
     *
     * Headphones still win: the precedence lives in Audio::start() and codecReconfig()
     * (headphones > loudspeaker > earpiece, amplifier only for !headphones && loudspeaker),
     * NOT in a guard here. ⚠ It was a guard here first — `if (!getHeadphones())` — and that
     * meant a game STARTED with headphones in never set the flag, so pulling the plug
     * mid-game dropped it to the earpiece: the bug being fixed, reached by the jack (review,
     * 2026-09-19). Set unconditionally, the flag is inert while the jack is occupied and an
     * unplug goes to the loudspeaker. setHeadphones() — the jack interrupt, WiPhone.ino —
     * reconfigures the LIVE codec by itself, so nothing more is needed from here. Volume
     * needs nothing either: setVolumes() already clamps the loudspeaker to
     * MaxLoudspeakerVolume, so F1/F2 (adjustVolume) stay safe on it.
     *
     * A notification chirp still in flight was finished FIRST (above): it forces the
     * loudspeaker for its 300 ms, and a snapshot taken inside that window would hand the
     * destructor the pop's route as the phone's — and the pop's own teardown would restore()
     * mid-game.
     *
     * Saved, and PUT BACK in the destructor: a borrower leaves the device as it found it
     * (music_player's restoreCallVolume and Audio::preserve/restore, for the same reason),
     * so whatever starts the codec next inherits the phone's route, not the game's. Its
     * own flag rather than soundOn: the emu thread clears soundOn when I2S starves, and
     * start() can fail, and the route has to go back on both of those paths. */
    savedLoudspeaker = audio->isLoudspeaker();
    /* 🛑 AND THE LEVELS (review SA-1, 2026-09-25). F1/F2 (adjustVolume) move the earpiece,
     * headphones and loudspeaker levels together, and only the route used to go back: a game
     * turned down left the next ring (the loudspeaker level) and the next call's earpiece
     * quieter until a reboot. The ring and a call read the stored levels now anyway
     * (WiPhone.ino, applyStoredCallVolumes), but a borrower leaves the device as it found it.
     * So the game's own F1/F2 level lasts for that game, not for the next one. */
    audio->getVolumes(savedEar, savedHp, savedLoud);
    routeSaved = true;
    audio->chooseSpeaker(true);

    soundOn = audio->start();
    /* Its own flag, for the quit: the emu thread clears soundOn when I2S starves, and the device is
     * still ON then - ~GbcApp used to skip its shutdown() on that path, so the ring could not be
     * reseated and the heap stayed split (review of the 0.9.79 reseat). */
    soundStarted = soundOn;
    audioStarve = 0;
  }

  s_blitGo     = xSemaphoreCreateBinary();
  s_running    = true;
  s_blitBusy   = false;
  s_blitExited = false;
  s_emuExited  = false;
  if (!s_blitGo) {
    initErr = GBC_ERR_ALLOC;
    return;
  }
  /* The game starts PARKED with the resume job queued: the blit task's first act is to load
   * /gbc/<rom>.auto if there is one (runAction, on the task with the stack for it), and to
   * un-pause either way. No menu is drawn for it — menuDirty stays false — so a resumed game
   * simply appears where it was left. */
  paused        = true;
  emuIdle       = false;
  menuDirty     = false;
  confirmClear  = false;
  statusMsg[0]  = 0;
  pendingAction = GBC_ACT_RESUME;
  // Static stacks (reused every game) and UNPINNED (like DigitalRainApp): the SMP
  // scheduler runs the two ready high-priority tasks on the two cores.
#if defined(configSUPPORT_STATIC_ALLOCATION) && configSUPPORT_STATIC_ALLOCATION
  xHandle = xTaskCreateStatic(&GbcApp::emuThread, "gbc", GBC_EMU_STACK_BYTES, this,
                              tskIDLE_PRIORITY + 2, s_emuStack, &s_emuTcb);
  s_blitTask = xTaskCreateStatic(&GbcApp::blitTask, "gbcblit", GBC_BLIT_STACK_BYTES, this,
                                 tskIDLE_PRIORITY + 2, s_blitStack, &s_blitTcb);
#else
  // Some Arduino-ESP32 builds disable FreeRTOS static allocation. Use the
  // dynamic API in those builds while keeping the static path on cores that support it.
  BaseType_t emuCreated = xTaskCreate(&GbcApp::emuThread, "gbc", GBC_EMU_STACK_BYTES,
                                      this, tskIDLE_PRIORITY + 2, &xHandle);
  BaseType_t blitCreated = xTaskCreate(&GbcApp::blitTask, "gbcblit", GBC_BLIT_STACK_BYTES,
                                       this, tskIDLE_PRIORITY + 2, &s_blitTask);
  if (emuCreated != pdPASS) xHandle = NULL;
  if (blitCreated != pdPASS) s_blitTask = NULL;
#endif
  if (xHandle && s_blitTask) {
    playing = true;
    return;
  }
  // Shouldn't happen with static allocation, but stay safe.
  s_running = false;
  if (s_blitGo) {
    xSemaphoreGive(s_blitGo);
  }
  vTaskDelay(pdMS_TO_TICKS(40));
  if (xHandle)    { vTaskDelete(xHandle); }
  if (s_blitTask) { vTaskDelete(s_blitTask); }
  xHandle = NULL;
  s_blitTask = NULL;
  initErr = GBC_ERR_TASK;
}

GbcApp::~GbcApp() {
  log_d("destroy GbcApp");
  s_instance = NULL;
  if (playing) {
    /* The resume point, before the tasks are torn down: this is the "save state upon exit".
     * autoSaveNow parks the emulator, hands the write to the blit task and waits for it. */
    autoSaveNow(GBC_AUTOSAVE_WAIT_MS);
    /* 🛑 NEVER DELETE THE BLIT TASK UNDER A JOB. A write on the card holds the SPI bus mutex
     * the panel shares and FATFS's volume lock; a task deleted inside one takes both with it
     * and the next screen draw blocks forever (review, 2026-09-19). If the wait above timed
     * out on a slow card, the job is still running here: wait it out, however long. */
    for (uint32_t i = 0; pendingAction != GBC_ACT_NONE; i++) {
      vTaskDelay(pdMS_TO_TICKS(50));
      if (i && (i % 100) == 0) {
        log_e("GBC: still writing the resume point after %lu s", (unsigned long)(i / 20));
      }
    }
    s_running = false;
    if (s_blitGo) {
      xSemaphoreGive(s_blitGo);   // wake the blit task if it's waiting
    }
    // Wait for both loops to exit, then a bit more so each task actually reaches
    // its parked vTaskDelay() (blocked). Deleting a *blocked* task frees its stack
    // immediately; deleting a still-running one only defers the free to idle,
    // which would leave no RAM for the next game ("could not start task").
    // Unbounded, for the reason above; with no job running it is one 20-30 ms turn.
    for (uint32_t i = 0; !(s_emuExited && s_blitExited); i++) {
      vTaskDelay(pdMS_TO_TICKS(10));
      if (i && (i % 100) == 0) {
        log_e("GBC: tasks not out after %lu s (emu=%d blit=%d)", (unsigned long)(i / 100),
              (int)s_emuExited, (int)s_blitExited);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    if (xHandle)    { vTaskDelete(xHandle);    xHandle = NULL; }
    if (s_blitTask) { vTaskDelete(s_blitTask); s_blitTask = NULL; }
    lcd.setSwapBytes(false);
    vTaskDelay(pdMS_TO_TICKS(30));   // let the idle task reclaim any deferred frees
  }
  // Always release these (also covers a failed startGame that created the
  // semaphore / loaded a ROM before bailing).
  if (s_blitGo) { vSemaphoreDelete(s_blitGo); s_blitGo = NULL; }
  /* 🛑 EVERYTHING, not just the ROM (0.9.75). Until now only the "Transfer ROMs" row called
   * gbcReleaseEmulator(); a quit left the two task stacks (12 KB), VRAM (16 KB), the audio
   * buffer (4 KB) — and WRAM (32 KB) when it had landed internal — resident until the next
   * reboot, so the first game of a boot cost the rest of the phone 32-64 KB of internal RAM
   * for the day (review, 2026-09-21; measured: 66 KB idle before a game, 34 after). The
   * tasks are gone by here (deleted above, 30 ms settle), so the stacks are free to free;
   * startGame re-allocates everything on the next launch, as the first launch of every
   * boot always has. */
  gbcReleaseEmulator();

  /* Hand the audio path back to the phone. 🛑 ON soundStarted, NOT soundOn (review of the 0.9.79
   * reseat). The emu thread clears soundOn when I2S starves (60 short writes in a row) and leaves
   * the device ON, and this used to skip the shutdown on that path: the reseat below then refused
   * a powered codec ("LEFT where it is"), the heap stayed split, and the codec stayed powered until
   * the idle watchdog - which a lit screen holds off. While gGbcActive is set the game is the
   * device's only user (the pop is held, music is paused, the radio is off), so turning it off is
   * the game's job on both paths. */
  if (soundStarted && audio) {
    audio->shutdown();
    soundOn = false;
    soundStarted = false;
  }
  if (routeSaved && audio) {
    /* Put the output back where startGame found it (see the note there). After shutdown()
     * on purpose: with the device off this is a flag write that the next start() reads.
     * One path skips shutdown(): start() failed, so the game never turned the device on and
     * this is the flag write again (or a live reconfigure of a device something else left on,
     * which the idle watchdog in WiPhone.ino releases once gGbcActive drops below). */
    audio->chooseSpeaker(savedLoudspeaker);
    audio->setVolumes(savedEar, savedHp, savedLoud);   // route first, then levels (Audio::restore's order)
    routeSaved = false;
  }
  /* 🛑 THE I2S RING IS REINSTALLED HERE, AFTER THE EMULATOR'S RAM IS FREED AND THE DEVICE IS OFF
   * (0.9.79). startGame's setMonoOutput() installed it with the stacks, VRAM and audio buffer
   * (32 KB) already at the bottom of the phone's big free block, and best fit put it straight on
   * top of them: gbcReleaseEmulator() above freed them BELOW the ring and the block stayed cut in
   * two for the rest of the boot - largest 63,716 -> 32,816 (phone 1), 64,152 -> 28,732 (phone 2),
   * free back to normal. Reinstalled now, best fit sees the heap as it was before the game and the
   * ring goes back into the hole the old one left (Audio::reseatI2S() has the numbers).
   * ⚠ After gbcReleaseEmulator() - before it, the ring is placed over the same blocks again.
   * ⚠ After shutdown() - reseatI2S() refuses a powered device, and the shutdown above now runs on
   *   the starved path too (soundStarted), so "LEFT where it is" means something ELSE had it on.
   * ⚠ Before wifiRestoreStation() - the station's own allocations then find the heap they left.
   * One reinstall, freed before it allocates: free RAM never dips below where it stands here.
   * READING THE LINE: a game that set the ring up at start reads `~32,816 -> ~L` (phone 1). A game
   * that found it already matching (a second game, nothing sounded between) reinstalled nothing at
   * start - the emulator's blocks sat below free space and the release made the big block whole -
   * so it reads `~L -> ~L`: a reinstall with nothing to fix, harmless at the loosest the heap gets,
   * and it hands the next start() a fresh queue instead of shutdown()'s stale one. `X -> X` with X
   * well below L means something ELSE was left in the big block, which no reseat can move. */
  if (ringReseat && audio) {
    ringReseat = false;
    const unsigned before = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const bool hadDriver = audio->i2sReady();
    const bool moved = audio->reseatI2S();
    log_e("GBC: I2S ring %s: largest %u -> %u (free %u)",
          moved             ? "reinstalled after the emulator's release"
          : audio->isOn()   ? "LEFT where it is - device still on"
          : !hadDriver      ? "not moved - there was no driver"
          :                   "reinstall FAILED - no driver until the next start()",
          before, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }
  gbcXferStop();            // in case the app dies while the transfer screen is up

  flushScreenPref();

  if (enteredGaming) {   // restore what gaming mode turned off
    enableCore0WDT();
    enableCore1WDT();
    gGbcActive = false;
    /* The game's hold on the core's auto-reconnect released first (startGame took it), so a
     * NO_AP_FOUND from the reconnect below is retried as it always was - if the owner's policy
     * has it armed (disable() disarms it: WiFi off stays off) and no hotspot holds it too. */
    wifiAutoReconnectRelease(WIFI_AR_HOLD_GAME);
    /* 🛑 THROUGH wifiRestoreStation(), AFTER gGbcActive drops (0.9.79). This used to ask the
     * off switch alone: `if (!radioOff) { WiFi.mode(WIFI_STA); WiFi.reconnect(); }` — and a phone
     * with NO saved network, or after Disconnect or Forget, is userDisabled() but not
     * radioOff(), with its STA config ERASED. Every game exit on it started the station and
     * esp_wifi_connect()ed an empty SSID, and nothing ever quieted it (the loop's retry, quiesce
     * and auto-switcher all stand down for a userDisabled phone): the radio stayed up until the
     * next reboot. Before that (2026-09-01) it restored unconditionally, undoing "WiFi: off".
     * Now both switches, the retry's arming and the dry spell are asked in one place, and the
     * log line says what it was allowed to do: `WIFI restore (Game Boy game ended): ...`. */
    wifiRestoreStation("Game Boy game ended");
  }
}

// Free the internal RAM the Bluetooth controller reserves (~60KB; BT is unused).
void GbcApp::reclaimInternalRam() {
  esp_bt_controller_status_t st = esp_bt_controller_get_status();
  if (st == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    esp_bt_controller_disable();
  }
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
    esp_bt_controller_deinit();
  }
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
}

// Case-insensitive check for a .gb / .gbc extension.
static bool hasRomExt(const char* name) {
  const char* dot = strrchr(name, '.');
  if (!dot) {
    return false;
  }
  char ext[5] = {0};
  for (int i = 0; i < 4 && dot[i]; i++) {
    char c = dot[i];
    ext[i] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
  }
  return (strcmp(ext, ".gb") == 0 || strcmp(ext, ".gbc") == 0);
}

void GbcApp::scanRoms() {
  romCount = 0;
  // The built-in game is always available (works with no SD card).
  snprintf(roms[romCount].name, sizeof(roms[0].name), "uCity (built-in)");
  roms[romCount].path[0] = 0;
  roms[romCount].embedded = true;
  romCount++;

  const char* dirs[] = { "/roms", "/" };
  for (int d = 0; d < 2 && romCount < GBC_MAX_ROMS; d++) {
    File dir = SD.open(dirs[d]);
    if (!dir) {
      continue;
    }
    if (!dir.isDirectory()) {
      dir.close();
      continue;
    }
    File f;
    while (romCount < GBC_MAX_ROMS && (f = dir.openNextFile())) {
      if (!f.isDirectory()) {
        const char* nm = f.name();
        const char* slash = strrchr(nm, '/');
        const char* base = slash ? slash + 1 : nm;
        /* ⚠ base[0] != '.' — HIDDEN FILES STAY HIDDEN, the same rule the Files app, Books,
         * Photos and the serial listers apply. Without it this picker listed macOS's
         * AppleDouble sidecars: Finder writes a '._Game.gbc' beside every file it copies onto
         * a FAT card, it has the ROM extension, and it is 4 KB of resource-fork metadata, not a
         * ROM. Phone 2 showed every game twice, once as '._…', and the Files app — which hides
         * dotfiles — could not find the twins to delete them ({user}, 2026-09-03). Serial `ls`
         * shows them; `rm` removes them. */
        if (base[0] != '.' && hasRomExt(base)) {
          snprintf(roms[romCount].name, sizeof(roms[0].name), "%s", base);
          snprintf(roms[romCount].path, sizeof(roms[0].path), "%s", nm);
          roms[romCount].embedded = false;
          romCount++;
        }
      }
      f.close();
    }
    dir.close();
  }
}

bool GbcApp::setupEmulator(const Rom& rom) {
  gbcUnloadRom();                    // free whatever was loaded before (clean slate)
  // (BT RAM reclaim happens in startGame, before the stacks are allocated.)

  // Load the ROM. Small ROMs are held whole in PSRAM (fast, hitchless); ROMs
  // too big for one contiguous PSRAM block (e.g. 4MB carts) are streamed from
  // the SD card instead: gnuboy preloads what fits and faults the remaining
  // 16KB banks in on demand (serialized with the LCD via s_spiBusLock).
  const uint8_t* romData = NULL;
  size_t romSize = 0;
  bool streamRom = false;
  char streamPath[120];
  if (!rom.embedded) {
    snprintf(romName, sizeof(romName), "%s", rom.name);
    File f = SD.open(rom.path);
    if (!f) {
      initErr = GBC_ERR_READ;
      return false;
    }
    romSize = f.size();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (romSize + 65536 > largest) {           // keep some PSRAM headroom
      f.close();
      streamRom = true;
      snprintf(streamPath, sizeof(streamPath), "/sd%s", rom.path);   // POSIX path for fopen
    } else {
      s_emuRom = (uint8_t*)ps_malloc(romSize);
      if (!s_emuRom) {
        f.close();
        initErr = GBC_ERR_ALLOC;
        return false;
      }
      size_t rd = 0;
      while (rd < romSize) {
        int n = f.read(s_emuRom + rd, romSize - rd);
        if (n <= 0) {
          break;
        }
        rd += n;
      }
      f.close();
      if (rd != romSize) {                     // truncated read -> discard
        free(s_emuRom);
        s_emuRom = NULL;
        initErr = GBC_ERR_READ;
        return false;
      }
      romData = s_emuRom;
    }
  } else {
    romData = gbc_test_rom;
    romSize = gbc_test_rom_len;
    snprintf(romName, sizeof(romName), "uCity");
  }

  if (!s_gnuboyInited) {
    s_emuAudio = (int16_t*)malloc(GBC_EMU_AUDIO_LEN * sizeof(int16_t));
    size_t fbBytes = (size_t)GBC_SCREEN_W * GBC_SCREEN_H * sizeof(uint16_t);
    s_emuFb[0] = (uint16_t*)ps_malloc(fbBytes);
    s_emuFb[1] = (uint16_t*)ps_malloc(fbBytes);
    if (!s_emuFb[0] || !s_emuFb[1] || !s_emuAudio) {
      dbgWhere = 1;
      dbgFreeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      dbgLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
      initErr = GBC_ERR_ALLOC;
      return false;
    }
    for (int ox = 0; ox < GBC_FILL_W; ox++) {
      s_xmap[ox] = (uint16_t)((ox * GBC_SCREEN_W) / GBC_FILL_W);
    }
    if (gnuboy_init(GBC_EMU_SAMPLERATE, GB_AUDIO_STEREO_S16, GB_PIXEL_565_BE, NULL, NULL) < 0) {
      initErr = GBC_ERR_INIT;
      return false;
    }
    gnuboy_set_soundbuffer(s_emuAudio, GBC_EMU_AUDIO_LEN);
    if (!s_spiBusLock) {
      s_spiBusLock = xSemaphoreCreateMutex();
    }
    gnuboy_set_io_lock(gbcSpiLock, gbcSpiUnlock);
    s_gnuboyInited = true;
  }

  if (streamRom) {
    // Preloading a couple MB from SD takes a few seconds; say so.
    SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
    lcd.fillScreen(BLACK);
    lcd.setTextFont(font);
    lcd.setTextDatum(TL_DATUM);
    lcd.setTextColor(TFT_GREENYELLOW, BLACK);
    lcd.drawString("Loading big ROM...", 8, 8);
    lcd.setTextColor(TFT_WHITE, BLACK);
    lcd.drawString("(a few seconds)", 8, 8 + font->height() + 6);
    if (gnuboy_load_rom_file(streamPath) < 0) {
      initErr = GBC_ERR_LOAD;
      return false;
    }
  } else if (gnuboy_load_rom(romData, romSize) < 0) {
    initErr = GBC_ERR_LOAD;
    return false;
  }
  s_romLoaded = true;
  gnuboy_reset(true);
  return true;
}

int GbcApp::readPad() {
  uint32_t ks = keypadState | gGbcKeyLatch;
  gGbcKeyLatch = 0;
  int pad = 0;
  if (ks & WIPHONE_KEY_MASK_UP)     pad |= GB_PAD_UP;
  if (ks & WIPHONE_KEY_MASK_DOWN)   pad |= GB_PAD_DOWN;
  if (ks & WIPHONE_KEY_MASK_LEFT)   pad |= GB_PAD_LEFT;
  if (ks & WIPHONE_KEY_MASK_RIFHT)  pad |= GB_PAD_RIGHT;   // (typo'd mask name in Hardware.h)
  if (ks & WIPHONE_KEY_MASK_F4)     pad |= GB_PAD_A;        // bottom-right user key
  if (ks & WIPHONE_KEY_MASK_F3)     pad |= GB_PAD_B;        // user key above A
  if (ks & WIPHONE_KEY_MASK_BACK)   pad |= GB_PAD_START;    // top-right
  if (ks & WIPHONE_KEY_MASK_SELECT) pad |= GB_PAD_SELECT;   // top-left
  return pad;
}

void GbcApp::blitBuffer(int idx) {
  const uint16_t* fb = s_emuFb[idx];
  if (!scaled) {
    // 1:1, but bounce rows through internal RAM in 6-row chunks: pushImage
    // straight from the PSRAM framebuffer CPU-feeds the SPI with per-pixel PSRAM
    // reads that thrash the cache BOTH cores share (measured: direct-PSRAM 1:1
    // ran at 75% game speed while the row-buffered Fill ran at 96%).
    uint16_t rowbuf[GBC_SCREEN_W * 6];   // 1.9KB on the blit task's stack
    const int xoff = (lcd.width()  - GBC_SCREEN_W) / 2;
    const int yoff = (lcd.height() - GBC_SCREEN_H) / 2;
    for (int y = 0; y < GBC_SCREEN_H; y += 6) {   // 144 rows = 24 whole chunks
      memcpy(rowbuf, fb + y * GBC_SCREEN_W, sizeof(rowbuf));
      lcd.pushImage(xoff, yoff + y, GBC_SCREEN_W, 6, rowbuf);
    }
    return;
  }
  // Scale row-by-row through a small buffer on this task's stack (internal RAM)
  // instead of a full-frame PSRAM scale buffer. The old triple PSRAM round-trip
  // (read fb, write 101KB, read it back for SPI) thrashed the cache BOTH cores
  // share and audibly slowed emulation on the other core. The 1.5x row map
  // repeats every 3rd output row, so a third of the rows reuse the previous scale.
  uint16_t rowbuf[GBC_FILL_W];   // 480 bytes
  const int xoff = (lcd.width()  - GBC_FILL_W) / 2;
  const int yoff = (lcd.height() - GBC_FILL_H) / 2;
  int prevSy = -1;
  for (int oy = 0; oy < GBC_FILL_H; oy++) {
    int sy = (oy * GBC_SCREEN_H) / GBC_FILL_H;
    if (sy != prevSy) {
      const uint16_t* src = fb + sy * GBC_SCREEN_W;
      for (int ox = 0; ox < GBC_FILL_W; ox++) {
        rowbuf[ox] = src[s_xmap[ox]];
      }
      prevSy = sy;
    }
    lcd.pushImage(xoff, yoff + oy, GBC_FILL_W, 1, rowbuf);
  }
}

// The ROM name with everything but [A-Za-z0-9] dropped: the stem of its state files.
static void gbcCleanName(const char* name, char* clean, size_t n) {
  size_t j = 0;
  for (size_t i = 0; name[i] && j < n - 1; i++) {
    char c = name[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      clean[j++] = c;
    }
  }
  clean[j] = 0;
}

/* "/sd/gbc/<clean>-<cartid><ext>" — the POSIX path gnuboy's fopen wants (the Arduino SD
 * object wants it without the "/sd": gbcSdPath). The cart id (gnuboy_cart_id: header
 * checksum + size codes) is in the name because the name alone is not an identity: the
 * sanitizer folds "Pokemon Red" and "Pokemon_Red" together, the uploader overwrites a
 * same-named file without a word, and a resume is now automatic and silent — a state
 * written from one cart must never be loaded into another (review, 2026-09-19). Files
 * from before 0.9.67 were named "<clean><ext>"; loadState adopts one of those once. */
void GbcApp::buildStatePath(char* out, size_t n, const char* ext) {
  char clean[40];
  gbcCleanName(romName, clean, sizeof(clean));
  snprintf(out, n, "/sd/gbc/%s-%08lx%s", clean, (unsigned long)gnuboy_cart_id(), ext);
}

// The pre-0.9.67 name of the same file, for the one-time adoption.
void GbcApp::buildLegacyStatePath(char* out, size_t n, const char* ext) {
  char clean[40];
  gbcCleanName(romName, clean, sizeof(clean));
  snprintf(out, n, "/sd/gbc/%s%s", clean, ext);
}

// The Arduino SD object's view of a "/sd/..." VFS path.
static inline const char* gbcSdPath(const char* vfsPath) {
  return vfsPath + 3;
}

// remove() logs an [E] line for a file that is not there — and that is the common case
// for one of the two slots — so every removal here is guarded by exists(), which is silent.
static bool gbcRemoveIfThere(const char* vfsPath) {
  const char* sd = gbcSdPath(vfsPath);
  if (!SD.exists(sd)) {
    return true;
  }
  return SD.remove(sd);
}

static size_t gbcFileSize(const char* vfsPath) {
  if (!SD.exists(gbcSdPath(vfsPath))) {      // open() logs an [E] line for a missing file
    return 0;
  }
  File f = SD.open(gbcSdPath(vfsPath));
  if (!f) {
    return 0;
  }
  const size_t n = f.size();
  f.close();
  return n;
}

static bool gbcIsHex8(const char* s) {
  for (int i = 0; i < 8; i++) {
    const char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// Does this /gbc entry belong to the ROM whose sanitized name is `clean`? Either spelling:
// "<clean><ext>" (pre-0.9.67) or "<clean>-<8 hex><ext>", ext one of the two slots or its .tmp.
static bool gbcStateBelongsTo(const char* base, const char* clean) {
  const size_t cl = strlen(clean);
  if (strncmp(base, clean, cl) != 0) {
    return false;
  }
  const char* rest = base + cl;
  if (rest[0] == '-') {
    if (!gbcIsHex8(rest + 1)) {
      return false;
    }
    rest += 9;
  }
  static const char* const exts[] = { GBC_EXT_STATE, GBC_EXT_AUTO, GBC_EXT_STATE ".tmp", GBC_EXT_AUTO ".tmp" };
  for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
    if (strcmp(rest, exts[i]) == 0) {
      return true;
    }
  }
  return false;
}

/* Every state file of one ROM, whatever cart id it was written under and whichever
 * spelling: the directory is read for them (the ids are not knowable from the picker,
 * where a ROM being deleted is not loaded). Collected first, removed after the directory
 * is closed — FATFS skips entries when one is unlinked under an open readdir. */
static void gbcRemoveStatesFor(const char* name) {
  char clean[40];
  gbcCleanName(name, clean, sizeof(clean));
  if (!clean[0]) {
    return;
  }
  char hits[12][72];
  int n = 0;
  File dir = SD.open("/gbc");
  if (!dir) {
    return;
  }
  File f;
  while (n < 12 && (f = dir.openNextFile())) {
    const char* nm = f.name();
    const char* slash = strrchr(nm, '/');
    const char* base = slash ? slash + 1 : nm;
    if (!f.isDirectory() && gbcStateBelongsTo(base, clean)) {
      snprintf(hits[n++], sizeof(hits[0]), "%s", nm[0] == '/' ? nm : base);
    }
    f.close();
  }
  dir.close();
  for (int i = 0; i < n; i++) {
    const char* sd = hits[i][0] == '/' ? hits[i] : NULL;
    char full[72];
    if (!sd) {
      snprintf(full, sizeof(full), "/gbc/%s", hits[i]);
      sd = full;
    }
    if (SD.exists(sd)) {
      SD.remove(sd);
    }
  }
}

/* Write a state ATOMICALLY: gnuboy writes <path>.tmp, its size is checked against what the
 * core says a state of this cart is, then the old file is removed and the new one renamed
 * over it. What this closes is a power-off mid-write, which used to leave a partial file
 * that gnuboy_load_state reads block by block INTO the emulator's RAM before it discovers
 * the file ends early — and the size check closes the quiet version of the same thing, a
 * full card: do_save_load's `fwrite(...) < 1` only notices a write that moved NOTHING. The
 * gap left is remove-then-rename (microseconds); a stranded .tmp is adopted by loadState.
 * 0 on success; gnuboy's -1 (open/write, lastErrno says why) / -2 (staging buffer); -3
 * the rename failed (the .tmp is KEPT for loadState to adopt); -4 short file. */
int GbcApp::writeState(const char* vfsPath) {
  char tmp[96];
  snprintf(tmp, sizeof(tmp), "%s.tmp", vfsPath);
  SD.mkdir("/gbc");                          // quiet when it already exists
  errno = 0;
  int r = gnuboy_save_state(tmp);
  lastErrno = errno;                         // before anything below can clobber it
  if (r == 0) {
    const size_t want = gnuboy_state_size();
    const size_t got = gbcFileSize(tmp);
    if (got != want) {
      log_e("GBC: short state %s: %u of %u bytes", gbcSdPath(tmp), (unsigned)got, (unsigned)want);
      r = -4;
    }
  }
  if (r != 0) {
    gbcRemoveIfThere(tmp);
    return r;
  }
  gbcRemoveIfThere(vfsPath);                 // FATFS will not rename over an existing file
  if (!SD.rename(gbcSdPath(tmp), gbcSdPath(vfsPath))) {
    log_e("GBC: rename %s failed, keeping the .tmp", gbcSdPath(tmp));
    return -3;
  }
  return 0;
}

/* Load a state into the core. 1 = there is no such file (the ordinary case, nothing
 * touched); 0 = loaded; <0 = a file that exists but would not load — the core has been
 * HARD RESET, because do_save_load has by then copied the file block by block into the
 * emulator's RAM up to where it failed, and what is left is neither the game that was
 * running nor the save. A wrong-sized file (a torn write, or a state from another cart
 * with the same name) is refused before it can touch anything. A stranded <path>.tmp — the
 * rename never happened — is adopted first. */
int GbcApp::loadState(const char* vfsPath) {
  char tmp[96];
  snprintf(tmp, sizeof(tmp), "%s.tmp", vfsPath);
  if (!SD.exists(gbcSdPath(vfsPath)) && SD.exists(gbcSdPath(tmp))) {
    if (SD.rename(gbcSdPath(tmp), gbcSdPath(vfsPath))) {
      log_e("GBC: adopted stranded %s", gbcSdPath(tmp));
    }
  }
  if (!SD.exists(gbcSdPath(vfsPath))) {
    /* A file under the pre-0.9.67 name — "<clean>.state", no cart id — is taken over
     * once, if it is the right size for this cart. The next write uses the new name. */
    const char* ext = strrchr(vfsPath, '.');
    char legacy[96];
    if (ext) {
      buildLegacyStatePath(legacy, sizeof(legacy), ext);
      if (SD.exists(gbcSdPath(legacy)) && gbcFileSize(legacy) == gnuboy_state_size() &&
          SD.rename(gbcSdPath(legacy), gbcSdPath(vfsPath))) {
        log_e("GBC: adopted %s as %s", gbcSdPath(legacy), gbcSdPath(vfsPath));
      }
    }
  }
  if (!SD.exists(gbcSdPath(vfsPath))) {
    return 1;
  }
  const size_t want = gnuboy_state_size();
  const size_t got = gbcFileSize(vfsPath);
  if (got != want) {
    log_e("GBC: %s is %u bytes, a state of this cart is %u: not loaded", gbcSdPath(vfsPath),
          (unsigned)got, (unsigned)want);
    return -5;
  }
  const int r = gnuboy_load_state(vfsPath);
  if (r != 0) {
    gnuboy_reset(true);
  }
  return r;
}

/* One SD job, on the blit task. Reads pendingAction's value from the caller, clears it LAST
 * (the main thread polls that to know the job is done), and never touches the LCD except
 * through menuDirty / needsClear. The emulator must be PARKED first: paused is set before
 * every job, and emuIdle is the emu thread's acknowledgement that it is out of gnuboy_run —
 * a state read mid-frame would be torn. The wait is unbounded on purpose (a stall line every
 * second): falling through after a deadline would be exactly the torn read it exists to
 * prevent, and a frame is 16 ms. */
void GbcApp::runAction(int act) {
  for (uint32_t i = 0; !emuIdle && !s_emuExited; i++) {
    vTaskDelay(pdMS_TO_TICKS(2));
    if (i && (i % 500) == 0) {
      log_e("GBC: job %d waiting %lu s for the emulator to park", act, (unsigned long)(i / 500));
    }
  }
  char path[96];
  int r = 0;
  const uint32_t t0 = millis();
  switch (act) {
    case GBC_ACT_SAVE:
      buildStatePath(path, sizeof(path), GBC_EXT_STATE);
      r = writeState(path);
      if (r == 0) {
        snprintf(statusMsg, sizeof(statusMsg), "Saved");
      } else {
        snprintf(statusMsg, sizeof(statusMsg), "Save fail %d e%d", r, lastErrno);
      }
      log_e("GBC: save %s to %s (%d e%d, %lu ms)", r == 0 ? "ok" : "FAILED", gbcSdPath(path), r,
            r == 0 ? 0 : lastErrno, (unsigned long)(millis() - t0));
      menuDirty = true;
      break;

    case GBC_ACT_LOAD:
      buildStatePath(path, sizeof(path), GBC_EXT_STATE);
      r = loadState(path);
      if (r == 0) {
        needsClear = true;                   // the menu box reaches past the frame
        paused = false;
      } else if (r == 1) {
        snprintf(statusMsg, sizeof(statusMsg), "No save found");
        menuDirty = true;
      } else {
        snprintf(statusMsg, sizeof(statusMsg), "Load fail %d: restarted", r);
        menuDirty = true;
      }
      log_e("GBC: load %s from %s (%d, %lu ms)", r == 0 ? "ok" : r == 1 ? "none" : "FAILED, reset",
            gbcSdPath(path), r, (unsigned long)(millis() - t0));
      break;

    case GBC_ACT_CLEAR: {
      /* Both slots go (and any .tmp), then a hard reset: the title screen appearing is the
       * confirmation. Checked by existence afterwards, not by remove()'s return. */
      char autoPath[96];
      buildStatePath(path, sizeof(path), GBC_EXT_STATE);
      buildStatePath(autoPath, sizeof(autoPath), GBC_EXT_AUTO);
      gbcRemoveStatesFor(romName);
      if (SD.exists(gbcSdPath(path)) || SD.exists(gbcSdPath(autoPath))) {
        r = -1;
        snprintf(statusMsg, sizeof(statusMsg), "Clear failed");
        menuDirty = true;
        log_e("GBC: clear FAILED for %s", gbcSdPath(autoPath));
        break;
      }
      gnuboy_reset(true);
      needsClear = true;                     // the old frame is gone with the state
      paused = false;
      log_e("GBC: cleared %s and %s, hard reset", gbcSdPath(path), gbcSdPath(autoPath));
      break;
    }

    case GBC_ACT_RESUME:
      /* Launch: the resume point if there is one; failing that the manual bookmark — the
       * state a phone updated to this build already has, or the only one left after a
       * crash. The next Quit writes the resume point and the fallback never fires again.
       * Not finding either is the ordinary first launch and says nothing. */
      buildStatePath(path, sizeof(path), GBC_EXT_AUTO);
      r = loadState(path);
      if (r == 1) {
        buildStatePath(path, sizeof(path), GBC_EXT_STATE);
        r = loadState(path);
      }
      if (r != 1) {
        log_e("GBC: resume %s from %s (%d, %lu ms)", r == 0 ? "ok" : "FAILED, fresh start",
              gbcSdPath(path), r, (unsigned long)(millis() - t0));
      }
      needsClear = true;
      paused = false;
      break;

    case GBC_ACT_AUTOSAVE:
      buildStatePath(path, sizeof(path), GBC_EXT_AUTO);
      r = writeState(path);
      if (r != 0) {
        snprintf(statusMsg, sizeof(statusMsg), "Save FAILED %d e%d", r, lastErrno);
        menuDirty = true;                    // the caller holds the screen so this is seen
      }
      log_e("GBC: auto-save %s to %s (%d e%d, %lu ms)", r == 0 ? "ok" : "FAILED",
            gbcSdPath(path), r, r == 0 ? 0 : lastErrno, (unsigned long)(millis() - t0));
      break;                                 // stays parked: the caller is leaving

    default:
      break;
  }
  actionResult = r;
  pendingAction = GBC_ACT_NONE;
}

/* Park the game and have the blit task write the resume point; true when it did. Main
 * thread only. The menu shows "Saving..." for the duration so the wait is not a freeze —
 * it is drawn by the blit task from menuDirty, so the flag goes up a couple of its 20 ms
 * turns before the job does, or the write would run first and the message paint after.
 * The timeout bounds THIS WAIT ONLY: a job still running when it expires is left to
 * finish (see the destructor — a task is never deleted under one). A failure is held on
 * the screen for two seconds, since the app is about to leave and take the menu with it. */
bool GbcApp::autoSaveNow(uint32_t timeoutMs) {
  if (!playing || s_blitExited) {
    return false;
  }
  const uint32_t t0 = millis();
  while (pendingAction != GBC_ACT_NONE && millis() - t0 < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(10));           // a manual save in flight (power-off only)
  }
  if (pendingAction != GBC_ACT_NONE) {
    return false;
  }
  paused = true;
  confirmClear = false;
  menuSel = GBC_MENU_RESUME;                 // the menu it parks under: Resume is one press
  snprintf(statusMsg, sizeof(statusMsg), "Saving...");
  menuDirty = true;
  vTaskDelay(pdMS_TO_TICKS(50));
  actionResult = -100;
  pendingAction = GBC_ACT_AUTOSAVE;
  while (pendingAction != GBC_ACT_NONE && millis() - t0 < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (pendingAction != GBC_ACT_NONE) {
    log_e("GBC: auto-save did not finish in %lu ms", (unsigned long)timeoutMs);
    return false;
  }
  if (actionResult != 0) {
    vTaskDelay(pdMS_TO_TICKS(2000));         // "Save FAILED ..." is on the menu: let it be read
    return false;
  }
  return true;
}

/* The Screen toggle, written at the two exits and not at the toggle itself: a flash write
 * from the pause menu would run with the emulator's tasks live on both cores. From the
 * destructor the tasks are gone; from the power-off path the game is parked and the maps
 * app writes NVS from the same spot already. */
void GbcApp::flushScreenPref() {
  if (!scaledDirty) {
    return;
  }
  Preferences p;
  if (p.begin(GBC_NVS, false)) {
    p.putInt("fill", scaled ? 1 : 0);
    p.end();
  }
  scaledDirty = false;
}

bool GbcApp::saveForPowerOff() {
  /* The game stays parked afterwards on purpose: the phone is going down, and if it were
   * somehow to stay up (a cable holding the rail) the state on the card must keep matching
   * the game on the screen — the pause menu is up, Resume is one press away. A job still
   * writing when the wait expires is not stopped: powerOff() drops the rail under it, and
   * the .tmp it was writing is what dies; the previous resume point is whole. */
  const bool ok = autoSaveNow(GBC_AUTOSAVE_WAIT_MS);
  flushScreenPref();
  return ok;
}

bool gbcSaveForPowerOff() {
  return s_instance ? s_instance->saveForPowerOff() : false;
}

// Serial `gbc`: one line of what the bench needs to know.
void gbcStatus(char* out, size_t n) {
  if (!s_instance) {
    snprintf(out, n, "app not open");
    return;
  }
  s_instance->status(out, n);
}

void GbcApp::status(char* out, size_t n) {
  if (!playing) {
    const int r = romSel - GBC_PICKER_ACTIONS;
    snprintf(out, n, "picker up, %d ROM(s), row %d = '%s', screen=%s", romCount, romSel,
             r >= 0 && r < romCount ? roms[r].name : (romSel == 0 ? "Transfer ROMs" : "Help"),
             scaled ? "fill" : "1:1");
    return;
  }
  char a[96], m[96];
  buildStatePath(a, sizeof(a), GBC_EXT_AUTO);
  buildStatePath(m, sizeof(m), GBC_EXT_STATE);
  /* wram/vram: where gnuboy's work RAM landed (hw.c prefers internal for speed, falls back to
   * PSRAM). Worth a look after any change to the internal heap's layout — the Bluetooth
   * reserve is released at boot since 0.9.74, not on the first game. */
  extern int gb_hw_wram_internal, gb_hw_vram_internal;
  snprintf(out, n, "running '%s' paused=%d job=%d screen=%s speed=%d%% wram=%s vram=%s | %s: %u B | %s: %u B | state size %u",
           romName, (int)paused, (int)pendingAction, scaled ? "fill" : "1:1", speedPct,
           gb_hw_wram_internal ? "internal" : "PSRAM", gb_hw_vram_internal ? "internal" : "PSRAM",
           gbcSdPath(a), (unsigned)gbcFileSize(a), gbcSdPath(m), (unsigned)gbcFileSize(m),
           (unsigned)gnuboy_state_size());
}

// F1/F2 during play: nudge the codec volume. setVolumes() clamps each output to
// its own valid range, so stepping all three together is safe (the codec applies
// whichever one is active: loudspeaker / earpiece / headphones). All three are put
// back when the game ends (~GbcApp, savedEar/savedHp/savedLoud): they are the call
// levels, and the loudspeaker one is the ring's (review SA-1).
void GbcApp::adjustVolume(int delta) {
  if (!audio) {
    return;
  }
  int8_t ear, hp, loud;
  audio->getVolumes(ear, hp, loud);
  audio->setVolumes(ear + delta, hp + delta, loud + delta);
}

// Fill out[0..outLen-2] with the marquee window of name at scroll: the name
// cycles with a 3-space gap ("LONG NAME   LONG NAME..."), one char per tick.
static void gbcMarqueeWindow(const char* name, int scroll, char* out, int outLen) {
  int n = (int)strlen(name);
  int cycle = n + 3;
  int pos = scroll % cycle;
  for (int i = 0; i < outLen - 1; i++) {
    int p = (pos + i) % cycle;
    out[i] = (p < n) ? name[p] : ' ';
  }
  out[outLen - 1] = 0;
}

// Repaint only the selected row's text: called on each APP_TIMER_EVENT tick to
// advance the marquee without a full drawPicker (a full redraw flashes black).
void GbcApp::drawPickerRow() {
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  const int lh = font->height() + 8;
  const int y = 40 + (romSel - romTop) * lh;
  char line[GBC_PICKER_VIS_CHARS + 1];
  gbcMarqueeWindow(roms[romSel - GBC_PICKER_ACTIONS].name, selScroll, line, sizeof(line));
  lcd.fillRect(28, y, lcd.width() - 28, font->height(), BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(TFT_YELLOW, BLACK);
  lcd.drawString(line, 28, y);
}

void GbcApp::drawPicker() {
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  lcd.fillScreen(BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(TFT_GREENYELLOW, BLACK);
  lcd.drawString("Select a game", 8, 8);

  // Rows 0 and 1 are the "Transfer ROMs" / "Help" actions (always on top, no
  // scrolling past a long game list to reach them); ROM i is at row i + 2.
  const int total = romCount + 2;
  const int lh = font->height() + 8;
  const int top = 40;
  int visible = (lcd.height() - top - 26) / lh;
  if (visible < 1) {
    visible = 1;
  }
  if (romSel < romTop) {
    romTop = romSel;
  }
  if (romSel >= romTop + visible) {
    romTop = romSel - visible + 1;
  }

  int y = top;
  for (int i = romTop; i < total && i < romTop + visible; i++) {
    bool sel = (i == romSel);
    lcd.setTextColor(sel ? TFT_YELLOW : TFT_WHITE, BLACK);
    lcd.drawString(sel ? ">" : " ", 8, y);
    if (i >= GBC_PICKER_ACTIONS) {
      const char* nm = roms[i - GBC_PICKER_ACTIONS].name;
      char line[GBC_PICKER_VIS_CHARS + 1];
      if (sel && (int)strlen(nm) > GBC_PICKER_VIS_CHARS) {
        gbcMarqueeWindow(nm, selScroll, line, sizeof(line));   // scrolling window
      } else {
        snprintf(line, sizeof(line), "%s", nm);                // truncate to fit
      }
      lcd.drawString(line, 28, y);
    } else {                       // action rows, tinted to stand apart from games
      lcd.setTextColor(sel ? TFT_YELLOW : TFT_CYAN, BLACK);
      lcd.drawString(i == 0 ? "Transfer ROMs..." : "Help...", 28, y);
    }
    y += lh;
  }

  if (confirmDelete) {
    lcd.setTextColor(TFT_RED, BLACK);
    lcd.drawString("Delete this ROM?", 8, lcd.height() - 46);
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("OK: delete   any: cancel", 8, lcd.height() - 22);
  } else {
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("OK play  Back del  End exit", 8, lcd.height() - 22);
  }
}

// "Transfer ROMs" screen: instructions when the server is off, address +
// live "ROMs added" count while it's on. The server itself is app_gbc_xfer.cpp.
void GbcApp::drawXfer() {
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  if (!xferClean) {              // full clear only on entry/layout change; the
    lcd.fillScreen(BLACK);       // 1Hz live refresh just overdraws the text
    xferClean = true;            // in place (a full clear flashed black visibly)
  }
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  int lh = font->height() + 4;
  int y = 8;

  lcd.setTextColor(TFT_GREENYELLOW, BLACK);
  lcd.drawString("Transfer ROMs", 8, y);
  y += lh + 4;

  lcd.setTextColor(TFT_WHITE, BLACK);
  if (!gbcXferOn()) {
    lcd.drawString("Put Game Boy ROMs on the", 8, y); y += lh;
    lcd.drawString("SD card from a computer:", 8, y); y += lh + 4;
    lcd.drawString("1. Connect phone to WiFi", 8, y); y += lh;
    lcd.drawString("2. Press OK to start", 8, y); y += lh;
    lcd.drawString("3. On your computer open", 8, y); y += lh;
    lcd.setTextColor(TFT_YELLOW, BLACK);
    lcd.drawString("   wiphone.local", 8, y); y += lh;
    lcd.setTextColor(TFT_WHITE, BLACK);
    lcd.drawString("4. Drag ROMs in or paste", 8, y); y += lh;
    lcd.drawString("   a link. Done!", 8, y); y += lh + 8;
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("OK: start   Back: games", 8, lcd.height() - 24);
  } else {
    lcd.setTextColor(TFT_GREEN, BLACK);
    lcd.drawString("Server ON", 8, y); y += lh + 4;
    lcd.setTextColor(TFT_WHITE, BLACK);
    lcd.drawString("On your computer, open:", 8, y); y += lh;
    lcd.setTextColor(TFT_YELLOW, BLACK);
    lcd.drawString("  wiphone.local", 8, y); y += lh;
    char line[40];
    snprintf(line, sizeof(line), "  or http://%s", gbcXferAddr());
    lcd.drawString(line, 8, y); y += lh + 4;
    lcd.setTextColor(TFT_WHITE, BLACK);
    if (gbcXferUsingAP()) {
      lcd.drawString("(join WiFi 'WiPhone-ROMs'", 8, y); y += lh;
      lcd.drawString(" first, no password)", 8, y); y += lh + 4;
    } else {
      lcd.drawString("(same WiFi as the phone)", 8, y); y += lh + 4;
    }
    snprintf(line, sizeof(line), "ROMs added: %d   ", gbcXferRomsAdded());   // pad: drawn in place
    lcd.drawString(line, 8, y); y += lh;

    // Diagnostics (refreshed ~1Hz): free heap, its low-water mark, last reset
    // reason (6=task WDT, 4=panic, 9=brownout, 1=power on), WiFi status. Drawn
    // in the text flow — a fixed bottom position collided with the key hints.
    char diag[56];
    snprintf(diag, sizeof(diag), "mem %u min %u rst %d wf %d   ",   // pad: drawn in place
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
             (int)esp_reset_reason(), (int)WiFi.status());
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString(diag, 8, y);

    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("OK: stop   Back: games", 8, lcd.height() - 24);
  }
}

// Help lines. NULL-string entries render as section headers (green).
static const char* const s_helpLines[] = {
  "@CONTROLS (in game)",
  "D-pad: move",
  "Bottom right key: A",
  "Key above it: B",
  "Back (top right): Start",
  "Select (top left): Select",
  "Top 2 right keys: volume",
  "End (hang up): pause",
  "@PAUSE MENU",
  "Quit saves your place;",
  " the game resumes there",
  " next time, by itself.",
  "Save state: a bookmark",
  " quitting never touches",
  "Load state: jump back to",
  " the bookmark",
  "Clear state: wipe both,",
  " the game's own saves too,",
  " and start over (asks 1st)",
  "Screen 1:1 or Fill: size",
  "Number = game speed %",
  "@ADDING GAMES",
  "Pick 'Transfer ROMs...'",
  "in the game list, then",
  "follow the steps shown.",
  "Files ending .gb / .gbc",
  "land in the game list.",
  "Back key deletes a game",
  "(asks first).",
  "@GOOD TO KNOW",
  "WiFi + calls are off",
  "while a game runs; quit",
  "the game to reconnect.",
  "Saves live on the SD",
  "card in /gbc/: .auto is",
  "your place, .state the",
  "bookmark. Deleting a",
  "game deletes its saves.",
  "Power-off saves too.",
  "Built-in game: uCity by",
  "AntonioND (GPL).",
};
static const int s_helpCount = sizeof(s_helpLines) / sizeof(s_helpLines[0]);

void GbcApp::drawHelp() {
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  lcd.fillScreen(BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  const int lh = font->height() + 4;
  const int top = 40;
  const int visible = (lcd.height() - top - 26) / lh;

  lcd.setTextColor(TFT_GREENYELLOW, BLACK);
  lcd.drawString("Game Boy help", 8, 8);
  if (helpTop > s_helpCount - visible) {
    helpTop = s_helpCount - visible;
  }
  if (helpTop < 0) {
    helpTop = 0;
  }
  int y = top;
  for (int i = helpTop; i < s_helpCount && i < helpTop + visible; i++) {
    const char* ln = s_helpLines[i];
    if (ln[0] == '@') {           // section header
      lcd.setTextColor(TFT_GREEN, BLACK);
      lcd.drawString(ln + 1, 8, y);
    } else {
      lcd.setTextColor(TFT_WHITE, BLACK);
      lcd.drawString(ln, 8, y);
    }
    y += lh;
  }
  lcd.setTextColor(TFT_DARKGREY, BLACK);
  lcd.drawString("Up/Down scroll   Back: games", 8, lcd.height() - 22);
}

void GbcApp::drawPauseMenu() {
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  /* Sized from the font: six rows now, and the old fixed 176 px put the fifth row under the
   * status line. Title band 46, rows of height+6, then a gap, the status line, a margin. */
  const int rowH = font->height() + 6;
  const int bw = 216;
  const int bh = 46 + GBC_MENU_COUNT * rowH + 8 + font->height() + 10;
  const int bx = (lcd.width() - bw) / 2;
  const int by = (lcd.height() - bh) / 2;
  lcd.fillRect(bx, by, bw, bh, TFT_DARKGREY);
  lcd.drawRect(bx, by, bw, bh, TFT_WHITE);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(TFT_WHITE, TFT_DARKGREY);
  lcd.drawString("PAUSED", bx + 14, by + 10);
  if (speedPct > 0) {              // measured game speed (100 = full speed)
    char spd[12];
    snprintf(spd, sizeof(spd), "%d%%", speedPct);
    lcd.setTextColor(speedPct >= 97 ? TFT_GREENYELLOW : TFT_ORANGE, TFT_DARKGREY);
    lcd.drawString(spd, bx + bw - 52, by + 10);
  }

  const char* items[GBC_MENU_COUNT] = {
    "Resume", "Save state", "Load state", "Clear state",
    scaled ? "Screen: Fill" : "Screen: 1:1", "Quit"
  };
  int y = by + 46;
  for (int i = 0; i < GBC_MENU_COUNT; i++) {
    bool sel = (i == menuSel);
    lcd.setTextColor(sel ? TFT_YELLOW : TFT_WHITE, TFT_DARKGREY);
    lcd.drawString(sel ? ">" : " ", bx + 12, y);
    lcd.drawString(items[i], bx + 34, y);
    y += rowH;
  }
  if (statusMsg[0]) {
    // Orange for the one that asks a question (Clear state's "OK again"), green for news.
    lcd.setTextColor(confirmClear ? TFT_ORANGE : TFT_GREENYELLOW, TFT_DARKGREY);
    lcd.drawString(statusMsg, bx + 14, by + bh - 10 - font->height());
  }
}

// Core 0: owns the LCD and SD. Pushes finished frames; when paused, runs the
// menu and save/load here (so SD and LCD never contend on the SPI bus).
void GbcApp::blitTask(void* pvParam) {
  GbcApp* app = (GbcApp*)pvParam;
  app->lcd.setSwapBytes(false);
  app->lcd.fillScreen(BLACK);

  while (s_running) {
    if (app->paused) {
      /* Menu first, then the job: a status set BEFORE a job ("Saving...") is on the screen
       * while the card is written, not after. A job's own message goes up through
       * menuDirty and paints on the next turn, 20 ms later. */
      if (app->menuDirty) {
        app->drawPauseMenu();
        app->menuDirty = false;
        app->needsClear = true;              // the box reaches past the frame: clear on resume
      }
      const int act = app->pendingAction;
      if (act != GBC_ACT_NONE) {
        app->runAction(act);                 // clears pendingAction when done
      }
      s_blitBusy = false;
      gGbcKeyLatch = 0;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // Running: push the latest rendered frame the emulator handed us.
    if (xSemaphoreTake(s_blitGo, pdMS_TO_TICKS(30)) == pdTRUE) {
      if (!s_running) {
        break;
      }
      gbcSpiLock();               // never overlap an SD bank read (same SPI bus)
      if (app->needsClear) {
        app->lcd.fillScreen(BLACK);
        app->needsClear = false;
      }
      app->blitBuffer(s_blitIdx);
      gbcSpiUnlock();
      s_blitBusy = false;
    }
  }
  s_blitExited = true;
  for (;;) {
    vTaskDelay(portMAX_DELAY);   // park until the destructor deletes us
  }
}

// Emulate at a steady ~60fps (correct game speed + responsive input). Render only
// the frames the blit task can actually show; when the blit is busy, keep
// emulating (no render) so the game never slows down — display frames just drop.
void GbcApp::emuThread(void* pvParam) {
  GbcApp* app = (GbcApp*)pvParam;
  int emuBuf = 0;
  uint32_t start = millis();
  uint32_t frames = 0;
  uint32_t winStart = start;      // rolling 1s window for the speed readout
  uint32_t winFrames = 0;
  int skip = 1;                   // adaptive frameskip: render every skip-th frame
  int skipPhase = 0;              // (heavy games: drop DISPLAY frames, not game speed)
  while (s_running) {
    /* emuIdle is the promise the blit task's save/load/reset rests on: "the core is not
     * being run". Dropped BEFORE the paused check, raised only inside the parked branch —
     * so it can never read true while a frame is in flight, and a requester that sets
     * paused and then waits for it is waiting for this task's next pass, not an old one. */
    app->emuIdle = false;
    if (app->paused) {
      app->emuIdle = true;
      vTaskDelay(pdMS_TO_TICKS(20));
      start = millis();           // reset the pacing baseline for resume
      frames = 0;
      winStart = start;
      winFrames = 0;
      continue;
    }
    gnuboy_set_pad(app->readPad());
    // Render only when the blit is free AND the frameskip phase comes up.
    // Scanline rendering runs on this core, so skipping it is the single
    // biggest relief for heavy games — the game logic stays at full speed
    // and only the display rate drops.
    bool willDraw = !s_blitBusy && (++skipPhase >= skip);
    if (willDraw) {
      skipPhase = 0;
    }
    gnuboy_set_framebuffer(s_emuFb[emuBuf]);
    gnuboy_run(willDraw);
    frames++;
    winFrames++;
    uint32_t winMs = millis() - winStart;
    if (winMs >= 1000) {          // GB runs 59.73 fps; ~60 emulated frames/s = 100%
      app->speedPct = (int)(winFrames * 100 * 1000 / 60 / winMs);
      winFrames = 0;
      winStart = millis();
      // Adapt the frameskip to hold 100% game speed: falling behind -> skip
      // more display frames (up to 2/3 dropped); comfortably at speed -> claw
      // display smoothness back.
      if (app->speedPct < 96 && skip < 3) {
        skip++;
      } else if (app->speedPct >= 99 && skip > 1) {
        skip--;
      }
    }
    if (willDraw) {
      s_blitIdx = emuBuf;
      s_blitBusy = true;
      xSemaphoreGive(s_blitGo);
      emuBuf = 1 - emuBuf;        // render the next frame into the other buffer
    }
    // Audio: push this frame's APU samples. Bounded wait: with the DMA full
    // (game ahead of the DAC) the write blocks ~one frame and becomes the de
    // facto clock, locked to the DAC with zero drift. If I2S misbehaves (wrong
    // rate after some phone sound reconfigured it, etc.) the timeout caps the
    // damage and the starve counter turns sound off entirely — silence beats
    // slow-motion (audio was throttling games to 13% before this guard).
    if (app->soundOn) {
      size_t n = gnuboy_get_audio_count();       // int16 count (stereo interleaved)
      if (n) {
        size_t want = n * sizeof(int16_t);
        size_t wrote = 0;
        i2s_write(I2S_NUM_0, s_emuAudio, want, &wrote, pdMS_TO_TICKS(25));
        if (wrote < want) {
          if (++app->audioStarve >= 60) {        // a full second of failed writes
            app->soundOn = false;
          }
        } else {
          app->audioStarve = 0;
        }
      }
    }
    // Wall-clock floor: never run faster than 60fps. When the blocking write
    // above already paced this frame, we're at/behind schedule and this is a
    // no-op; it always yields at least one tick for the main loop's keypad.
    uint32_t target = start + (uint32_t)((uint64_t)frames * 1000 / 60);
    int32_t ahead = (int32_t)(target - millis());
    vTaskDelay(ahead > 1 ? pdMS_TO_TICKS(ahead) : 1);
  }
  s_emuExited = true;
  for (;;) {
    vTaskDelay(portMAX_DELAY);
  }
}

appEventResult GbcApp::processEvent(EventType event) {
  if (initErr) {
    return LOGIC_BUTTON_BACK(event) ? EXIT_APP : DO_NOTHING;
  }

  if (!playing) {
    if (uiMode == UI_XFER) {
      // Transfer ROMs screen. OK toggles the server; Back returns to the list
      // (rescanning it so anything just uploaded appears immediately).
      if (LOGIC_BUTTON_OK(event)) {
        if (gbcXferOn()) {
          gbcXferStop();
        } else {
          gbcXferStart();
        }
        xferClean = false;        // layout changes: repaint from scratch
        return REDRAW_SCREEN;
      }
      if (LOGIC_BUTTON_BACK(event)) {
        gbcXferStop();
        scanRoms();
        if (romSel >= romCount + GBC_PICKER_ACTIONS) {
          romSel = romCount + GBC_PICKER_ACTIONS - 1;
        }
        uiMode = UI_PICKER;
        return REDRAW_SCREEN;
      }
      if (!IS_KEYBOARD(event) && gbcXferOn()) {
        // Live "ROMs added"/heap refresh — but at most once a second. Background
        // events can arrive much faster than 1Hz, and redrawing the whole screen
        // per event kept the main loop busy on SPI instead of pumping the web
        // server (the page then never loaded).
        uint32_t nowMs = millis();
        if (nowMs - xferDrawMs >= 1000) {
          xferDrawMs = nowMs;
          return REDRAW_SCREEN;
        }
      }
      return DO_NOTHING;
    }

    if (uiMode == UI_HELP) {
      switch (event) {
        case WIPHONE_KEY_UP:
          helpTop--;                            // drawHelp clamps
          return REDRAW_SCREEN;
        case WIPHONE_KEY_DOWN:
          helpTop++;
          return REDRAW_SCREEN;
        case WIPHONE_KEY_BACK:
        case WIPHONE_KEY_END:
          uiMode = UI_PICKER;
          return REDRAW_SCREEN;
        default:
          return DO_NOTHING;
      }
    }

    // ROM picker: rows 0/1 are Transfer/Help, ROM i sits at row i + 2.
    if (confirmDelete) {
      if (!IS_KEYBOARD(event)) {
        return DO_NOTHING;                      // ignore background ticks so the prompt stays up
      }
      if (event == WIPHONE_KEY_OK) {            // OK confirms; any other key cancels
        int r = romSel - GBC_PICKER_ACTIONS;
        if (r >= 0 && r < romCount && !roms[r].embedded) {
          SD.remove(roms[r].path);
          /* And its states: the files are keyed by the sanitized name only, so a different
           * ROM uploaded later under the same name would otherwise resume, silently, into
           * this one's memory (review, 2026-09-19). */
          gbcRemoveStatesFor(roms[r].name);
        }
        scanRoms();
        if (romSel >= romCount + GBC_PICKER_ACTIONS) {
          romSel = romCount + GBC_PICKER_ACTIONS - 1;
        }
      }
      confirmDelete = false;
      return REDRAW_SCREEN;
    }
    if (!IS_KEYBOARD(event)) {
      // Advance the marquee if the selected game's name is too long to fit,
      // repainting only that row (full redraws flash black). Time-based, NOT
      // per-event: background events can arrive far faster than the 250ms app
      // timer (WiFi/SIP activity), which once spun the scroll unreadably fast.
      uint32_t nowMs = millis();
      if (nowMs - marqueeMs >= 250 &&
          romSel >= GBC_PICKER_ACTIONS &&
          (int)strlen(roms[romSel - GBC_PICKER_ACTIONS].name) > GBC_PICKER_VIS_CHARS) {
        marqueeMs = nowMs;
        selScroll++;
        drawPickerRow();
      }
      return DO_NOTHING;
    }
    switch (event) {
      case WIPHONE_KEY_UP:
        if (romSel > 0) { romSel--; }
        selScroll = 0;
        return REDRAW_SCREEN;
      case WIPHONE_KEY_DOWN:
        if (romSel < romCount + GBC_PICKER_ACTIONS - 1) { romSel++; }
        selScroll = 0;
        return REDRAW_SCREEN;
      case WIPHONE_KEY_OK:
        if (romSel == 0) {                      // "Transfer ROMs..."
          // Give the WiFi/web stack RAM to breathe: drop anything a previous
          // game left resident AND the unused BT controller's ~60KB reserve.
          // The phone idles at ~9KB free internal — the web server's TCP
          // buffers starve mid-page at that level (white half-loaded page).
          gbcReleaseEmulator();
          reclaimInternalRam();
          uiMode = UI_XFER;
          xferClean = false;                    // full screen draw on entry
          return REDRAW_SCREEN;
        }
        if (romSel == 1) {                      // "Help..."
          uiMode = UI_HELP;
          helpTop = 0;
          return REDRAW_SCREEN;
        }
        startGame();
        return initErr ? REDRAW_SCREEN : DO_NOTHING;   // tasks own the screen once playing
      case WIPHONE_KEY_BACK:                    // top-right button: delete an SD ROM
        if (romSel >= GBC_PICKER_ACTIONS && !roms[romSel - GBC_PICKER_ACTIONS].embedded) {
          confirmDelete = true;
          return REDRAW_SCREEN;
        }
        return DO_NOTHING;
      case WIPHONE_KEY_END:
        return EXIT_APP;
      default:
        return DO_NOTHING;
    }
  }

  if (!paused) {
    if (event == WIPHONE_KEY_END) {
      statusMsg[0] = 0;
      confirmClear = false;
      menuSel = GBC_MENU_RESUME;
      menuDirty = true;
      paused = true;
    } else if (event == WIPHONE_KEY_F1) {   // top user button: volume up
      adjustVolume(+GBC_VOL_STEP);
    } else if (event == WIPHONE_KEY_F2) {   // second user button: volume down
      adjustVolume(-GBC_VOL_STEP);
    }
    return DO_NOTHING;
  }

  /* The pause menu. A job on the card (pendingAction) owns the menu until it is done: keys
   * are dropped rather than queued, so a job is never replaced mid-write and Quit cannot
   * tear the tasks down under one. The launch's RESUME job is the same case — a key in the
   * first moments of a game lands on the parked menu with nothing drawn, and is dropped. */
  if (!IS_KEYBOARD(event)) {
    return DO_NOTHING;
  }
  if (pendingAction != GBC_ACT_NONE) {
    return DO_NOTHING;
  }
  // "Clear state" asks once; any key but a second OK on the same row withdraws the question.
  if (confirmClear && !(event == WIPHONE_KEY_OK && menuSel == GBC_MENU_CLEAR)) {
    confirmClear = false;
    statusMsg[0] = 0;
    menuDirty = true;
  }
  switch (event) {
    case WIPHONE_KEY_UP:
      menuSel = (menuSel + GBC_MENU_COUNT - 1) % GBC_MENU_COUNT;
      menuDirty = true;
      break;
    case WIPHONE_KEY_DOWN:
      menuSel = (menuSel + 1) % GBC_MENU_COUNT;
      menuDirty = true;
      break;
    case WIPHONE_KEY_END:
      needsClear = true;                     // the menu box reaches past the frame
      paused = false;
      break;
    case WIPHONE_KEY_OK:
      switch (menuSel) {
        case GBC_MENU_RESUME: needsClear = true;
                              paused = false;               break;
        case GBC_MENU_SAVE:   pendingAction = GBC_ACT_SAVE;  break;
        case GBC_MENU_LOAD:   pendingAction = GBC_ACT_LOAD;  break;
        case GBC_MENU_CLEAR:
          if (!confirmClear) {
            confirmClear = true;
            snprintf(statusMsg, sizeof(statusMsg), "OK again: wipes all saves");   // 194 px of 202
            menuDirty = true;
          } else {
            confirmClear = false;
            statusMsg[0] = 0;
            pendingAction = GBC_ACT_CLEAR;
          }
          break;
        case GBC_MENU_SCREEN: scaled = !scaled;
                              scaledDirty = true;   // remembered by the destructor, see there
                              needsClear = true;
                              menuDirty = true;              break;
        case GBC_MENU_QUIT:   return EXIT_APP;   // the destructor writes the resume point
      }
      break;
    default:
      break;
  }
  return DO_NOTHING;
}

void GbcApp::redrawScreen(bool redrawAll) {
  if (!initErr) {
    if (!playing) {                    // picker/transfer/help until a game starts
      if (uiMode == UI_XFER) {
        drawXfer();
      } else if (uiMode == UI_HELP) {
        drawHelp();
      } else {
        drawPicker();
      }
    }
    return;                            // once playing, the emulator tasks own the screen
  }
  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  lcd.fillScreen(BLACK);
  lcd.setTextColor(RED, BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);

  const char* msg = "emulator setup failed";
  if (initErr == GBC_ERR_ALLOC) {
    msg = "out of memory";
  } else if (initErr == GBC_ERR_INIT) {
    msg = "gnuboy init failed";
  } else if (initErr == GBC_ERR_LOAD) {
    msg = "bad ROM";
  } else if (initErr == GBC_ERR_ROMBIG) {
    msg = "ROM too big for RAM";
  } else if (initErr == GBC_ERR_READ) {
    msg = "SD read failed";
  } else if (initErr == GBC_ERR_TASK) {
    msg = "could not start task";
  }
  lcd.drawString(msg, 6, 40);
  lcd.setTextColor(WHITE, BLACK);
  int y = 40 + font->height() + 6;
  if ((initErr == GBC_ERR_ALLOC || initErr == GBC_ERR_ROMBIG) && dbgWhere) {
    char line[48];
    snprintf(line, sizeof(line), "where:%d free:%u", dbgWhere, (unsigned)dbgFreeInt);
    lcd.drawString(line, 6, y);
    y += font->height() + 6;
    snprintf(line, sizeof(line), "largest:%u", (unsigned)dbgLargest);
    lcd.drawString(line, 6, y);
    y += font->height() + 6;
  }
  lcd.drawString("Back/hang-up to exit", 6, y);
}
