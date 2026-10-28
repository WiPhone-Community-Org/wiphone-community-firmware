/*
 * app_gbc.h — WiPhone Game Boy Color emulator
 *
 * On launch the app shows a ROM picker (the built-in game plus any .gb/.gbc on
 * the SD card). Selecting one enters "gaming mode" (WiFi/mesh off) and runs the
 * gnuboy core across both cores. Hang-up (END) opens an in-game pause menu.
 *
 * A game RESUMES WHERE YOU LEFT IT, the way COVEY's RetroArch does (savestate_auto_save /
 * savestate_auto_load): quitting writes /gbc/<rom>.auto and the next launch of that ROM loads
 * it, silently. The pause menu keeps a separate MANUAL slot (/gbc/<rom>.state: Save state /
 * Load state) that a quit never overwrites — a checkpoint — and a "Clear state" row that wipes
 * both files and restarts the game from its title (asks first). Power-off saves too
 * (gbcSaveForPowerOff, called from the two power-off paths in WiPhone.ino before the latch
 * drops). {user}, 2026-09-19: "save state upon exit and open last state upon entrance. That's
 * how covey does it."
 *
 * Input map:
 *   D-pad -> D-pad     F4 (bottom-right key) -> A     F3 (above it) -> B
 *   BACK (top-right) -> Start            SELECT (top-left) -> Select
 *   END (hang-up) -> pause menu / picker back
 */

#ifndef APP_GBC_H
#define APP_GBC_H

#include "GUI.h"

#define GBC_SCREEN_W 160
#define GBC_SCREEN_H 144
#define GBC_MAX_ROMS 48

class GbcApp : public ThreadedApp {
public:
  GbcApp(LCD& disp, ControlState& state);
  virtual ~GbcApp();

  ActionID_t getId() {
    return GUI_APP_GBC;
  };
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll=false);
  bool saveForPowerOff();        // gbcSaveForPowerOff(): the running game, if any
  void status(char* out, size_t n);   // serial `gbc`

protected:
  static void emuThread(void* pvParam);   // emulate + render into the back buffer
  static void blitTask(void* pvParam);    // push the finished buffer to the screen

  // A selectable ROM: either the built-in game or a file on the SD card.
  struct Rom {
    char name[40];
    char path[100];
    bool embedded;
  };

  void scanRoms();               // build the picker list (built-in + SD /roms and root)
  void drawPicker();
  void drawPickerRow();          // repaint just the selected row (marquee tick)
  void drawXfer();               // "Transfer ROMs" screen (WiFi upload server)
  void drawHelp();               // scrollable help/instructions screen
  void startGame();              // enter gaming mode and launch the selected ROM

  void reclaimInternalRam();     // free the (unused) BT controller RAM for the emulator
  bool setupEmulator(const Rom& rom);   // gnuboy init + load the chosen ROM
  int  readPad();                // map the held keypad state to a gb_pad bitmask
  void blitBuffer(int idx);      // push framebuffer[idx] to the screen, centered

  // Pause menu (hang-up during play): Resume / Save / Load / Clear / Screen / Quit.
  void drawPauseMenu();
  void buildStatePath(char* out, size_t n, const char* ext);   // "/sd/gbc/<rom>-<cartid><ext>"
  void buildLegacyStatePath(char* out, size_t n, const char* ext);   // pre-0.9.67: "<rom><ext>"
  void flushScreenPref();        // the Screen toggle -> NVS, if it changed
  // The SD work behind the menu rows and the auto-resume/auto-save. BLIT TASK ONLY: it is
  // the one task that owns the card and the LCD, and the only one with a stack deep
  // enough for the fopen->FATFS->SD path (see GBC_BLIT_STACK_BYTES). Every action first
  // waits for the emulator to park (emuIdle) so the core is never read mid-frame.
  void runAction(int act);
  int  writeState(const char* vfsPath);   // atomic: <path>.tmp, size-checked, then swapped in
  int  loadState(const char* vfsPath);    // 1 = no file, 0 = loaded, <0 = failed (core reset)
  int  lastErrno = 0;                     // errno right after the last gnuboy_save_state
  // Main-thread side of the auto-save: park the game, hand the blit task the write, wait
  // for it. Used by the destructor (Quit) and by gbcSaveForPowerOff().
  bool autoSaveNow(uint32_t timeoutMs);

  // Picker state (main thread only). The list shows the ROMs plus two action
  // rows appended after them: "Transfer ROMs" and "Help".
  enum UiMode { UI_PICKER, UI_XFER, UI_HELP };
  UiMode uiMode = UI_PICKER;
  int  helpTop = 0;              // first visible help line (scrolling)
  uint32_t xferDrawMs = 0;       // last live-refresh of the transfer screen
  bool xferClean = false;        // transfer screen background already drawn
                                 // (live refreshes skip the black fill: no flicker)
  Rom  roms[GBC_MAX_ROMS];
  int  romCount = 0;
  int  romSel = 0;
  int  romTop = 0;               // first visible row (scrolling)
  int  selScroll = 0;            // marquee offset (chars) for a long selected name
  uint32_t marqueeMs = 0;        // last marquee step (time-based: events can storm)
  bool confirmDelete = false;    // picker is asking to confirm a delete
  bool playing = false;          // false = picker on screen, true = game running
  bool enteredGaming = false;    // did we turn WiFi/mesh off? (restore on exit)
  volatile bool soundOn = false; // feed I2S? (the emu thread clears it when I2S starves)
  bool soundStarted = false;     // did startGame turn the device on? (shut it down at quit, starved or not)
  int  audioStarve = 0;          // consecutive under-written I2S frames (emu thread only)
  bool routeSaved = false;       // did startGame move the codec's output? (put it back)
  bool ringReseat = false;       // startGame set the I2S ring up over the emulator's RAM (reseat at quit)
  bool savedLoudspeaker = false; // the output the phone was on before the game
  int8_t savedEar = 0, savedHp = 0, savedLoud = 0;  // its levels too: F1/F2 move all three (SA-1)

  void adjustVolume(int delta);  // F1/F2 in-game volume up/down

  int  initErr = 0;              // 0 = ok, <0 = setup error code
  char romName[40] = {0};
  // Diagnostics captured at an allocation failure, shown on the error screen.
  size_t dbgFreeInt = 0;         // free internal RAM
  size_t dbgLargest = 0;         // largest free internal block
  int    dbgWhere = 0;           // 1 = setupEmulator allocs, 2 = task stacks

  // Shared between processEvent and the emulator tasks.
  volatile bool paused = false;
  volatile bool emuIdle = false;     // emu thread: parked in the paused branch (safe to save)
  volatile int  menuSel = 0;
  volatile bool menuDirty = false;
  volatile int  pendingAction = 0;   // GBC_ACT_*: one SD job for the blit task, NONE when done
  volatile int  actionResult = 0;    // the last action's return (autoSaveNow reads it)
  bool confirmClear = false;         // "Clear state" pressed once; the next OK does it
  // Fill by default ({user}, 2026-09-19: "the default screen to be fill instead of 1:1");
  // the pause-menu toggle is remembered in NVS so a preference for 1:1 sticks.
  volatile bool scaled = true;       // false = crisp 1:1, true = 1.5x fill-width
  bool scaledDirty = false;          // toggled this session: the destructor writes NVS
  volatile bool needsClear = false;  // clear the screen once (after a mode switch)
  volatile int  speedPct = 0;        // measured game speed, % of real time (100 = full)
  char statusMsg[32] = {0};
};

// The power-off paths in WiPhone.ino call this before the latch drops, like
// booksSaveOpenPosition() and mapsSaveOpenView(): writes the running game's resume
// point. False when no game is running (nothing to do) or the write did not finish.
bool gbcSaveForPowerOff();

#endif // APP_GBC_H
