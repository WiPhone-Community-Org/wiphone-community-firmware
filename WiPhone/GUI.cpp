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

#include "GUI.h"
#include "tinySIP.h"
#include "ota.h"
#include "Test.h"
#include "app_meshtastic.h"
#include "app_gbc.h"
#include "app_books.h"
#include "app_files.h"
#include "app_photos.h"
#include "app_maps.h"
#include "app_almanac.h"   // Menu > Almanac (0.9.80)
#include "app_ai.h"        // Menu > AI (0.9.81)
#include "sms_mirror_rx.h"   // sipCompleteAddress: bare number -> full SIP URI
#include "app_music.h"
#include "music_player.h"   // Settings > Audio: while music plays, the call levels live in its stash
#include "kosync_sync.h"   // kosyncWindowClose: the WiFi settings screens end any sync window
#include "app_gbc_xfer.h"  // xferStop: ...and any headless uploader's hotspot
extern volatile bool gGbcActive;   // WiPhone.ino: the emulator owns the screen and the audio device
/* WiPhone.ino: end a notification chirp in flight before its device is taken - its restore() would
 * land on top of the new owner (the call screen's volume keys, the mic apps; review 3, A4/A6). */
extern void notifyPopFinishFor(const char* why);
#include "menu_marquee.h"  // the scrolling selected menu row: phase clock + glyph stepping
#include "menu_wrap.h"     // a display-only note broken into rows that fit
#include "tz_service.h"    // Settings > Time zone: Auto / a named zone / the typed offset (0.9.82)

// Defined further down, used by the Messages screens above it.
static const char* sipDisplayLabel(Storage& flash, const char* peer, const char* uri);

// Static images
#include "src/assets/image.h"
#include "src/assets/image_256.h"
#include "src/assets/image_jpg.h"
#include "src/assets/ackman_data.h"
#include <esp_wifi.h>

// TODO:
// - optimization: return DO_NOTHING for irrelevant events
// - optimization: if event was processed by a widget, return false if it is irrelevant (state not changed)
// - migrate from C-string (strcpy, strcmp, strdup) to std::string or to String?

LCD* static_lcd = NULL;
bool UDP_SIP = false;
/* (The call's speaker choice lives in ControlState::callLoudspeaker now - see GUI.h.) */
/* 🛑 DERIVED, NOT STORED. This was a bare `bool wifiOn = true;` that the toggle set and three
 * other code paths did not — so the label could say "off" while the radio was up. It now reads
 * the one persisted switch, and cannot disagree with it. */
#define wifiOn (!wifiState.radioOff())


uint16_t GUI::batteryExtraLength = 0;

GUI::GUI() {
  // Widgets
  widgetsArray = NULL;
  maxWidgets = 0;
  numWidgets = 0;

  header = new HeaderWidget("???", state);
  footer = new FooterWidget("???", "???", state);
  mainMenu = NULL;

  // Cursor position
  xPos = 0;
  yPos = 0;

  // Timing
  mil = 0;

  // Current state
  menuDrawn = false;
  menuNewItems = false;
};

GUI::~GUI() {
  if (header) {
    delete header;
  }
  if (footer) {
    delete footer;
  }
  if (mainMenu) {
    delete mainMenu;
  }

  cleanAppDynamic();
}

void GUI::cleanAppDynamic() {
  // An app's widgets die here, so no pointer to one may outlive this call. See
  // FocusableWidget::clearTextFocus() for what a stale one costs.
  FocusableWidget::clearTextFocus();
  /* 🔑 AND NEITHER MAY THE PREDICTIVE-TEXT CLAIM. t9Field is set by the field that wants T9
   * and otherwise cleared only by setInputState() — which nothing calls on the way out of an
   * app. Left set, T9 stayed live on menus with no text field at all, swallowing digit keys
   * and stopping UP/DOWN/BACK from navigating. Cleared here rather than in each app because
   * this and the EXIT_APP path are the two doors every app leaves by; an app that wants it
   * re-claims it when it builds its field. */
  state.t9Field = false;
  if (runningApp!=NULL) {
    delete runningApp;
    runningApp = NULL;
  }
  if (callApp!=NULL) {
    delete callApp;
    callApp = NULL;
  }

  // Free memory from widgets
  deleteWidgets();
}

void GUI::deleteWidgets() {
  if (maxWidgets) {
    for (uint16_t i=0; i<numWidgets; i++) {
      if (widgetsArray[i]!=NULL) {
        delete widgetsArray[i];
      }
    }
    freeNull((void **) &widgetsArray);
  }
  maxWidgets = 0;
  numWidgets = 0;
}

bool GUI::addWidget(GUIWidget* w) {
  // TODO: can use LinearArray
  // Ensure the array is big enough
  if (numWidgets>=maxWidgets) {
    GUIWidget** p;
    if (maxWidgets>0) {
      if (maxWidgets<=8) {
        maxWidgets <<= 1;
      } else {
        maxWidgets += 8;
      }
      p = (GUIWidget**) realloc(widgetsArray, sizeof(GUIWidget*) * maxWidgets);
    } else {
      maxWidgets = 1;
      p = (GUIWidget**) malloc(sizeof(GUIWidget*) * maxWidgets);
    }
    if (p==NULL) {
      return false;
    }
    widgetsArray = p;
  }
  widgetsArray[numWidgets++] = w;
  return true;
}

void GUI::loadSettings() {
  state.loadSipAccount();

  /* ⚠ Load the ringer mode HERE, at boot. AudioConfigApp reads it too, but only when that
   * screen is opened — so without this the phone would ring at full volume until the user
   * happened to visit Settings > Audio, which defeats the point of the setting entirely. */
  {
    CriticalFile cfg(Storage::ConfigsFile);
    if ((cfg.load() || cfg.restore()) && !cfg.isEmpty() && cfg.hasSection("audio")) {
      state.ringerMode    = (uint8_t) cfg["audio"].getIntValueSafe("ringer_mode", state.ringerMode);
      state.notifySipMode = (uint8_t) cfg["audio"].getIntValueSafe("notify_sip_mode", state.notifySipMode);
      state.notifyMeshMode= (uint8_t) cfg["audio"].getIntValueSafe("notify_mesh_mode", state.notifyMeshMode);
      state.notifyVolume  = (int8_t)  cfg["audio"].getIntValueSafe("notify_vol", state.notifyVolume);
      /* Clamped to the slider's own range. Review 2026-09-02: a hand-edited ini of -1 became
       * 65,535 ms - a 65-second motor run per notification, during which audioDeviceBusy()'s
       * vibro term held the audio device awake too. constrain() keeps a bad file from turning
       * into a bad phone. */
      state.notifyVibroMs = (uint16_t) constrain(cfg["audio"].getIntValueSafe("notify_vibro_ms", state.notifyVibroMs), 50, 650);
      /* The master mute: read here at boot for the same reason as the ringer mode; the Audio
       * object does not exist yet, so WiPhone.ino applies it (audio->setMuted) once it does. */
      state.audioMuted = cfg["audio"].getIntValueSafe("mute", 0) != 0;
      log_d("notify: call=%d sip=%d mesh=%d vol=%d", (int)state.ringerMode,
            (int)state.notifySipMode, (int)state.notifyMeshMode, (int)state.notifyVolume);
    }
  }
  /* ⚠ Read the predictive-text setting HERE, at boot, for the same reason the ringer mode
   * above is read here: the Settings row writes it, but nothing would read it back until
   * that screen was next opened. The fallback is state.t9Enabled, which the constructor has
   * already set from T9_DEFAULT_ENABLED — so a missing [input] section and a present one
   * with the key missing both land on the same answer. */
  {
    CriticalFile cfg(Storage::ConfigsFile);
    if ((cfg.load() || cfg.restore()) && !cfg.isEmpty() && cfg.hasSection("input")) {
      state.t9Enabled = cfg["input"].getIntValueSafe("predictive", state.t9Enabled ? 1 : 0) != 0;
    }
    log_d("t9: predictive = %d", (int)state.t9Enabled);
  }

  log_d("fromName  = %s", state.fromNameDyn);
  log_d("fromUri   = %s", state.fromUriDyn);
  log_d("proxyPass = %s", state.proxyPassDyn);
}

/*
 * Description:
 *     Loads messages twice: at the start and after current time becomes known.
 */
void GUI::reloadMessages() {
  flash.messages.unload();
  /* UTC, like every writer of the store (saveMessage's getExactUtcTime, the SIP and mirror
   * paths) and like dateTimeAgo's "now". load() stamps its repairs of no-time texts with this;
   * the local-shifted getExactUnixTime() it was given since the vendor code put boot-era texts
   * seven hours early at UTC-7 — drawn mid-thread — and in the future east of UTC (review,
   * 2026-09-25). A mesh-set clock is used on purpose: message times follow any KNOWN clock.
   * ⚠ But the store WRITES them, so msgStamp() also says WHEN it is a mesh clock: the repair's
   * stamps are then provisional, and the first load under NTP/GPS moves them by what that
   * replacement moved the clock (review SA-3; clock_source.h has the rules). */
  flash.messages.load(ntpClock.msgStamp());
  state.unreadMessages = flash.messages.hasUnread();
}

/* Pixel dimensions straight out of the JPEG header, without decoding it.
 *
 * Wanted because SIZE IN BYTES SAYS NOTHING ABOUT WHETHER A PHOTO WILL FIT. TJpgDec only
 * halves — 1, 1/2, 1/4, 1/8 and no further — so a 4032x3024 phone photo reduces to 504x378
 * and the screen is 240x320: what lands in the sprite is the TOP-LEFT CORNER of it. That is
 * safe (TFT_eSprite::createSprite allocates one spare pixel precisely so out-of-window
 * pushes have somewhere to go) but it is not what anyone means by "set as wallpaper", and a
 * phone that shows a corner of your picture without saying so is the same class of fault as
 * one that shows nothing. So: measure it, and say it.
 *
 * Walks the marker chain to the first SOFn. Skips SOF4/SOF12 (DHT/DAC, not frame headers).
 */
static bool jpegPixelSize(const uint8_t* d, size_t n, uint16_t* w, uint16_t* h) {
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) {
    return false;
  }
  size_t i = 2;
  while (i + 3 < n) {
    if (d[i] != 0xFF) {
      i++;                                  // resync: fill bytes and padding are legal here
      continue;
    }
    const uint8_t m = d[i + 1];
    if (m == 0xFF) {
      i++;
      continue;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      i += 2;                               // standalone markers carry no length
      continue;
    }
    if (m == 0xDA || m == 0xD9) {
      return false;                         // scan data or end reached with no frame header
    }
    const size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];
    if (len < 2 || i + 2 + len > n) {
      return false;
    }
    // SOF0..SOF15 are frame headers except SOF4 (DHT) and SOF12 (DAC).
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xCC) {
      if (i + 9 >= n) {
        return false;
      }
      *h = ((uint16_t)d[i + 5] << 8) | d[i + 6];
      *w = ((uint16_t)d[i + 7] << 8) | d[i + 8];
      return *w && *h;
    }
    i += 2 + len;
  }
  return false;
}

/* ── FILLING A SCREEN WITH A PHOTO THAT IS NOT THE SHAPE OF THE SCREEN ────────────────────
 *
 * 🔑 MEASURED, and it is the whole reason this exists: every photo on the bench card is
 * 480x270, and plain drawImage() put each of them on a 240x320 screen as a 240x135 BAND
 * ACROSS THE TOP with the old background still showing underneath. The wallpaper HAD
 * changed — the SD-ordering fix above works — and it still looked wrong. Screenshotted
 * 2026-08-25 with `shot`, which is the only reason anybody could see it.
 *
 * ⚠ The cause is that TJpgDec only HALVES. Its scale factor is 1, 1/2, 1/4 or 1/8 and there
 * is nothing in between, so "scaled to fit" lands on whatever power of two happens to be
 * near — 480x270 becomes 240x135, and a 4032x3024 phone photo cannot be reduced past
 * 504x378 and gets its top-left corner cropped out instead. Neither is a wallpaper.
 *
 * So: decode ONCE into a temporary PSRAM sprite at the smallest halving that still COVERS
 * the screen, then resample that into the destination at an arbitrary ratio, cropping to
 * centre. That is the ordinary "cover" rule — the picture fills the screen and the overhang
 * is trimmed evenly from both sides, rather than letterboxed with black bars, which is what
 * a phone wallpaper means everywhere else.
 *
 * ⚠ The temporary sprite is PSRAM (createSprite -> esp32Calloc -> MALLOC_CAP_SPIRAM) and is
 * at most ~504x378x2 = 381 KB, freed before this returns. It must NEVER come out of internal
 * RAM — that is the heap SIP and WiFi fight over, and a wallpaper is a luxury.
 *
 * Returns false if the file is not a measurable JPEG; the caller then falls back to plain
 * drawImage(), which is still right for the compiled-in RLE3/I256 assets.
 */
static bool wallpaperFill(LCD* parent, TFT_eSprite* dst, const uint8_t* d, size_t n,
                          uint16_t* srcW, uint16_t* srcH, bool* cropped) {
  uint16_t iw = 0, ih = 0;
  if (!parent || !dst || !dst->isCreated() || !jpegPixelSize(d, n, &iw, &ih)) {
    return false;
  }
  *srcW = iw;
  *srcH = ih;
  const int DW = dst->width(), DH = dst->height();

  /* Largest halving that still covers the screen in BOTH axes — decode no more pixels than
   * the resample will actually use. Stops at 1/8 because that is tjpgd's floor. */
  int red = 1;
  while (red < 8 && (int)(iw / (red * 2)) >= DW && (int)(ih / (red * 2)) >= DH) {
    red *= 2;
  }
  const int dw = iw / red, dh = ih / red;
  if (dw <= 0 || dh <= 0) {
    return false;
  }

  TFT_eSprite tmp(parent);
  tmp.setColorDepth(16);
  tmp.createSprite(dw, dh);
  if (!tmp.isCreated()) {
    log_e("wallpaper: no PSRAM for a %dx%d decode buffer", dw, dh);
    return false;
  }
  uint16_t ow = 0, oh = 0;
  const bool decoded = display::load_jpg_at(d, (UINT)n, &tmp, 0, 0, (uint16_t)dw, (uint16_t)dh, &ow, &oh) != 0;
  if (!decoded) {
    tmp.deleteSprite();
    return false;
  }

  /* Cover: the larger of the two ratios, so neither axis is left short. Everything is done
   * in the SOURCE frame — for each destination pixel, ask which source pixel it came from —
   * because that visits every destination pixel exactly once and cannot leave gaps, which
   * the forward mapping does as soon as it is scaling up. */
  const float sx = (float)DW / (float)dw, sy = (float)DH / (float)dh;
  const float scale = sx > sy ? sx : sy;
  const float useW = (float)DW / scale, useH = (float)DH / scale;
  const float offX = ((float)dw - useW) * 0.5f, offY = ((float)dh - useH) * 0.5f;
  *cropped = (useW < dw - 0.5f) || (useH < dh - 0.5f);

  for (int y = 0; y < DH; y++) {
    int syi = (int)(offY + ((float)y + 0.5f) / scale);
    if (syi < 0) {
      syi = 0;
    } else if (syi >= dh) {
      syi = dh - 1;
    }
    for (int x = 0; x < DW; x++) {
      int sxi = (int)(offX + ((float)x + 0.5f) / scale);
      if (sxi < 0) {
        sxi = 0;
      } else if (sxi >= dw) {
        sxi = dw - 1;
      }
      dst->drawPixel(x, y, tmp.readPixel(sxi, syi));
    }
  }
  tmp.deleteSprite();
  return true;
}

/* ── THE WALLPAPER LOADER, AND WHY IT IS A FUNCTION RATHER THAN A BLOCK INSIDE init() ─────
 *
 * 🛑 IT USED TO BE INLINE IN init(), AND SO IT COULD NEVER SEE AN SD CARD. setup() mounts
 * SPIFFS, calls gui.init(), and only ~50 lines LATER calls SD.begin() — the card shares the
 * SPI bus with this screen and the comment there says that order is deliberate. But the
 * Photos app writes the chosen wallpaper to /background.jpg **on the SD card**, so the
 * `SD.exists()` test in init() was asking an unmounted filesystem. It answered false, every
 * time, and the phone quietly kept the compiled-in default.
 *
 * That is why "Set as wallpaper" appeared to do nothing — not on that boot and not on any
 * boot after it ({user}, 2026-08-25). ⚠ The failure was SILENT because a rejected override
 * lands in exactly the same fallback as "no wallpaper has ever been chosen": the two states
 * were indistinguishable on screen AND in the log. Hence wallpaperNote, and `wallpaper` on
 * the serial console — the phone can now be asked what it did.
 *
 * So the call sites are three, and each one is load-bearing:
 *   init()    — SPIFFS only, by necessity, but bgImage MUST hold a picture before the very
 *               next line of setup() calls redrawScreen().
 *   setup()   — again, immediately after SD.begin(). This is the one that finds the file.
 *   Photos    — when the user picks one, which is what makes it appear at once rather than
 *               "restart to see it".
 *
 * ⚠ Reads with ONE exact-sized PSRAM allocation, not the old doubling LinearArray: the cap
 * is now 2 MB to match what the Photos viewer will open, and growing to 2 MB by doubling
 * asks PSRAM for 2 MB and 4 MB in the same instant. It also CLOSES the file, which the old
 * block never did — that was a File handle leaked on every boot with a wallpaper set.
 */
bool GUI::loadWallpaper() {
  if (!bgImage || !bgImage->isCreated()) {
    snprintf(wallpaperNote, sizeof(wallpaperNote), "no background layer (sprite not created)");
    return false;
  }

  const bool onSd = SD.exists(GUI::backgroundFile);
  const bool onSpiffs = !onSd && SPIFFS.exists(GUI::backgroundFile);
  bool succ = false;

  if (onSd || onSpiffs) {
    const char* where = onSd ? "SD" : "SPIFFS";
    File f = onSd ? SD.open(GUI::backgroundFile, FILE_READ)
                  : SPIFFS.open(GUI::backgroundFile, FILE_READ);
    if (!f) {
      snprintf(wallpaperNote, sizeof(wallpaperNote), "%s on %s will not open", GUI::backgroundFile, where);
      log_e("wallpaper: %s", wallpaperNote);
    } else {
      const size_t sz = (size_t) f.size();
      if (!sz) {
        snprintf(wallpaperNote, sizeof(wallpaperNote), "%s on %s is empty", GUI::backgroundFile, where);
        log_e("wallpaper: %s", wallpaperNote);
      } else if (sz > (size_t) GUI::backgroundFileMaxSize) {
        /* Say the NUMBER. "too big" with no figure is what sent this hunt looking at the
         * decoder instead of at a size check. */
        snprintf(wallpaperNote, sizeof(wallpaperNote), "%u KB on %s is over the %u KB limit",
                 (unsigned)(sz >> 10), where, (unsigned)(GUI::backgroundFileMaxSize >> 10));
        log_e("wallpaper: %s", wallpaperNote);
      } else {
        uint8_t* buf = (uint8_t*) ps_malloc(sz);
        if (!buf) {
          snprintf(wallpaperNote, sizeof(wallpaperNote), "no PSRAM for %u KB", (unsigned)(sz >> 10));
          log_e("wallpaper: %s", wallpaperNote);
        } else {
          const size_t got = f.read(buf, sz);
          if (got != sz) {
            snprintf(wallpaperNote, sizeof(wallpaperNote), "short read: %u of %u bytes",
                     (unsigned)got, (unsigned)sz);
            log_e("wallpaper: %s", wallpaperNote);
          } else {
            /* Try the fitted path first — it is the one that fills the screen — and fall
             * back to the plain decode for anything that is not a measurable JPEG. Say the
             * PIXELS as well as the kilobytes either way: "49 KB" tells nobody why their
             * picture is the wrong shape. */
            uint16_t iw = 0, ih = 0;
            bool cropped = false;
            if (wallpaperFill(&lcd, bgImage, buf, sz, &iw, &ih, &cropped)) {
              snprintf(wallpaperNote, sizeof(wallpaperNote), "%u KB from %s, %ux%u scaled to fill%s",
                       (unsigned)(sz >> 10), where, (unsigned)iw, (unsigned)ih,
                       cropped ? " (cropped to centre)" : "");
              succ = true;
            } else if (bgImage->drawImage(buf, (int)sz)) {
              snprintf(wallpaperNote, sizeof(wallpaperNote), "%u KB from %s, drawn unscaled",
                       (unsigned)(sz >> 10), where);
              succ = true;
            }
            if (succ) {
              log_d("wallpaper: %s", wallpaperNote);
            } else {
              /* Both paths refused it. TJpgDec rejects progressive and greyscale JPEGs
               * outright — the e-reader met both — and drawImage() only otherwise knows
               * RLE3 and I256. Name that, because the file is perfectly good and the user
               * has no way to guess why their picture is not good enough. */
              snprintf(wallpaperNote, sizeof(wallpaperNote), "%u KB from %s would not decode "
                       "(baseline colour JPEG only)", (unsigned)(sz >> 10), where);
              log_e("wallpaper: %s", wallpaperNote);
            }
          }
          free(buf);
        }
      }
      f.close();
    }
  } else {
    snprintf(wallpaperNote, sizeof(wallpaperNote), "default (no %s on SD or SPIFFS)", GUI::backgroundFile);
  }

  if (!succ) {
    bgImage->drawImage(image_i256, sizeof(image_i256));
  }
  /* Repaint the menu with whatever is now in the sprite. The clone at redrawScreen() is
   * gated on !menuDrawn, so without this a wallpaper set from Photos would sit in the
   * sprite unseen until something else happened to invalidate the menu. */
  menuDrawn = false;
  return succ;
}

void GUI::init(void (*lcdOnOffCallback)(bool)) {
  state.setInputState(InputType::Numeric);

  // Init screen and show GUI splash screen
  lcd.begin();
  lcdOnOff = lcdOnOffCallback;
  static_lcd = &lcd;

  // Create screen sprite
  page = new TFT_eSprite(&lcd);
  page->setColorDepth(16);
  page->createSprite(lcd.width(), lcd.height());
  if (page->isCreated()) {
    screen = page;
  } else {
    screen = &lcd;
    log_d("page sprite not created, falling back to direct on-screen drawing");
  }

  // Load background image
  bgImage = new TFT_eSprite(&lcd);      // create dummy sprite at first (must be fed to ClockApp at least as a placeholder)
  bgImage->setColorDepth(16);
  if (page && page->isCreated()) {
    // Store background image for future use
    bgImage->createSprite(lcd.width(), lcd.height());
    /* ⚠ THIS CALL CANNOT SEE THE SD CARD, AND THAT IS NOT A BUG HERE — IT IS WHY
     * setup() CALLS loadWallpaper() A SECOND TIME. SD.begin() runs ~50 lines after
     * gui.init() (deliberately: the card shares SPI with this screen), so at this
     * point only SPIFFS can answer. The call is still needed: bgImage must hold a
     * picture before the first redrawScreen(), which happens on the next line of
     * setup(). See loadWallpaper(). */
    loadWallpaper();
  }

  /* ⚠ A DUPLICATE MENU ID IS SILENT, AND HAS SHIPPED TWICE.
   *
   * findMenu() matches on ID alone — it never looks at `parent` — and returns the FIRST row
   * that matches. So a second row reusing an ID is not an error: it is simply shadowed. Its
   * icon comes from the wrong entry and selecting it launches the wrong app, while the menu
   * still looks entirely normal.
   *
   * That shipped once as "Music opening Books", was fixed by moving Music to ID 42 — which
   * the WiFi auto-switch row already held — and so shipped again as "WiFi auto-switch opens
   * Music", with a music note drawn beside it ({user}, 2026-08-14). A comment saying the IDs
   * must be unique was added the first time and did not prevent the second. Hence a check.
   *
   * O(n²) over ~40 rows, once, at boot. It costs nothing and it says the ID out loud. */
  for (unsigned a = 0; a < sizeof(menu) / sizeof(GUIMenuItem); a++) {
    if (!menu[a].title || !menu[a].title[0]) {
      continue;                     // zero-filled tail; see the note on the array's size
    }
    for (unsigned b = a + 1; b < sizeof(menu) / sizeof(GUIMenuItem); b++) {
      if (menu[b].title && menu[b].title[0] && menu[a].ID == menu[b].ID) {
        log_e("MENU: DUPLICATE ID %u - \"%s\" (parent %d) shadows \"%s\" (parent %d); "
              "the second is unreachable and draws the first's icon",
              (unsigned)menu[a].ID, menu[a].title, (int)menu[a].parent,
              menu[b].title, (int)menu[b].parent);
      }
    }
  }

  // Enter menu
  enterMenu(1);       // start in Main menu
#ifdef DIAGNOSTICS_ONLY
  enterApp(GUI_APP_DIAGNOSTICS);
  return;
#endif // DIAGNOSTICS_ONLY
  enterApp(GUI_APP_SPLASH);

  // Initialize clock app to show in the locked state
  clockApp = new ClockApp(*screen, *bgImage, state);
};

void GUI::pushScreen(TFT_eSPI* disp) {
  if (disp->isSprite()) {
    ((TFT_eSprite*)disp)->pushSprite(0, 0);
  }
};

void GUI::pushScreenPart(TFT_eSPI* disp, uint16_t yOff, uint16_t height) {
  if (disp->isSprite()) {
    ((TFT_eSprite*)disp)->pushSpritePart(0, yOff, height);
  }
};

void GUI::frameToSerial() {
  if (page && page->isSprite()) {
    for (int y=0; y<page->height(); y++) {
      for (int x=0; x<page->width(); x++) {
        printf(" 0x%04x", page->readPixel(x,y));
      }
      printf("\r\n");
    }
  }
}

/* ── THE LIVE FRAME, OVER THE CABLE ──────────────────────────────────────────────────────
 *
 * base64 of the page sprite — which is what was last pushed to the glass — so a change on
 * the SCREEN can be checked by someone who is not holding the phone.
 *
 * 🔑 THIS IS THE MISSING INSTRUMENT IN THIS REPO, and the wallpaper bug is what proved it.
 * Every screen in Photos needs a key press, serial cannot press keys, and so the app reached
 * a user untried — twice over, because the thing it got wrong (a background that never
 * changed) is invisible to every other diagnostic the phone has. `wallpaper` can now say
 * what the loader decided; only this can say what the user is actually looking at.
 *
 * ⚠ It is a DUMP, not a stream: ~205 KB of base64, about 4 s at 500000 baud, during which
 * the loop task does nothing else. The loop watchdog is 20 s and prints rather than panics,
 * so this is inside the budget — but do not put it on a timer.
 *
 * frameToSerial() above is the vendor's version and prints " 0x%04x" per pixel — 537 KB for
 * the same frame, and no framing to tell a decoder where it starts. Kept, unused.
 */
void GUI::screenshotToSerial() {
  static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (!page || !page->isCreated()) {
    printf("SHOT ERROR no page sprite (direct-to-screen fallback is in use)\r\n");
    return;
  }
  const int w = page->width(), h = page->height();
  printf("SHOT BEGIN %d %d rgb565be\r\n", w, h);

  char line[100];
  int li = 0;
  uint8_t trio[3];
  int ti = 0;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const uint16_t px = page->readPixel(x, y);
      const uint8_t two[2] = { (uint8_t)(px >> 8), (uint8_t)(px & 0xFF) };
      for (int k = 0; k < 2; k++) {
        trio[ti++] = two[k];
        if (ti == 3) {
          line[li++] = B64[trio[0] >> 2];
          line[li++] = B64[((trio[0] & 0x03) << 4) | (trio[1] >> 4)];
          line[li++] = B64[((trio[1] & 0x0F) << 2) | (trio[2] >> 6)];
          line[li++] = B64[trio[2] & 0x3F];
          ti = 0;
          if (li >= 96) {
            line[li] = '\0';
            printf("%s\r\n", line);
            li = 0;
          }
        }
      }
    }
  }
  /* The tail. 240x320x2 is divisible by 3, so this cannot fire on this screen — but a
   * sprite is not required to be, and a decoder that meets a short group is better served
   * by correct padding than by a silently truncated last pixel. */
  if (ti) {
    line[li++] = B64[trio[0] >> 2];
    if (ti == 1) {
      line[li++] = B64[(trio[0] & 0x03) << 4];
      line[li++] = '=';
      line[li++] = '=';
    } else {
      line[li++] = B64[((trio[0] & 0x03) << 4) | (trio[1] >> 4)];
      line[li++] = B64[(trio[1] & 0x0F) << 2];
      line[li++] = '=';
    }
  }
  if (li) {
    line[li] = '\0';
    printf("%s\r\n", line);
  }
  printf("SHOT END\r\n");
  fflush(stdout);
}

void GUI::sleepScreen() {
  // Manual screen-off. Mirrors the auto SCREEN_SLEEP_EVENT: brightness 0 marks
  // the screen as sleeping, so the existing wake-on-keypress logic revives it
  // (and consumes that first key). Locks too, if keyboard locking is enabled.
  log_d("Manual screen sleep");
  lcdLedOnOff(false);
  state.screenBrightness = 0;
  if (state.locking) {
    state.locked = true;
  }
}

void GUI::toggleScreen() {
#ifdef WIPHONE_INTEGRATED
  lcdOn = !lcdOn;
  log_d("Turning %s backlight", lcdOn ? "ON" : "OFF");
  (*lcdOnOff)(lcdOn);

  // Setting brightness from software attempts
  // Variant 1: DISPOFF register - turns the screen into completely white
  //lcd.writecommand(lcdOn ? ST7789_DISPON : ST7789_DISPOFF);
  // Variant 2: brightness register - doesn't work
  //lcd.writecommand(ST7789_WRDISBV);
  //lcd.writedata(lcdOn ? 0b01010001 : 0);

  // Turn on backlight (dimmed with PWM) - WiPhone #2
  //ledcAttachPin(LCD_LED_PIN, 1);        // GPIO, ledChannel
  //ledcSetup(1, 5000, 8);                // ledChannel, freq, resolution (bits: 8 or 16)
  //ledcWrite(1, 0);                      // ledChannel, dutyCycle
#endif // WIPHONE_INTEGRATED  
}

void GUI::longBatteryAnimation() {
  const int extraSections = 19;
  for (GUI::batteryExtraLength=1; GUI::batteryExtraLength<4*extraSections; GUI::batteryExtraLength++) {   // add 20 sections to the battery
    if (runningApp && !runningApp->isWindowed()) {
      this->redrawScreen(false, false, true);  // redraw entire screen
    } else {
      this->redrawScreen(true, false, false);  // redraw header only
    }
  }
  GUI::batteryExtraLength--;
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  CONTROL STATE  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

ControlState::ControlState(void)
  : userSerialBuffer(USER_SERIAL_BUFFER_SIZE) {
  sipState = CallState::NotInited;

  // Initialize dynamic
  // SIP account
  fromNameDyn  = NULL;
  fromUriDyn   = NULL;//strdup("0@00");//NULL;     // SIP URI
  proxyPassDyn = NULL;

  // Callee
  calleeNameDyn = NULL;
  calleeUriDyn  = NULL;//strdup("1@00");//NULL;
  lastReasonDyn = NULL;

  // T9 predictive text. Enabled defaults OFF so an existing phone behaves identically
  // until the setting is deliberately turned on; t9Field is re-set per field anyway.
  t9Enabled = T9_DEFAULT_ENABLED;
  t9Field = false;
  inputMode = MODE_T9;
  t9Caps = true;
  t9PunctSeen = false;
  t9Emit[0] = '\0';
  t9EmitAt = 0;

  // Not subscribed to "any event"
  msAppTimerEventPeriod = 0;
  msAppTimerEventLast = 0;

  // Messages
  unreadMessages = false;

  // Battery
  battUpdated = false;
  battVoltage = 0;
  battSoc = 0;
};

ControlState::~ControlState() {
  clearDynamicSip();
  clearDynamicCallee();
}

void ControlState::clearDynamicSip() {
  freeNull((void **) &fromNameDyn);
  freeNull((void **) &fromUriDyn);
  fromUriDyn = NULL;//strdup("0@00");//NULL;     // SIP URI
  freeNull((void **) &proxyPassDyn);
  freeNull((void **) &lastReasonDyn);
}

void ControlState::clearDynamicCallee() {
  freeNull((void **) &calleeNameDyn);
  freeNull((void **) &calleeUriDyn);
  calleeUriDyn  = NULL;//strdup("1@00");//NULL;
}

/* See the note in GUI.h. Returns without touching anything on an unbalanced release
 * rather than restoring a snapshot nobody took. */
void ControlState::holdScreenAwake(bool hold) {
  if (hold) {
    if (this->screenAwakeHolders++ > 0) {
      return;                      // already stretched; a second snapshot is the bug
    }
    this->heldSavedDimMs = this->dimAfterMs;
    this->heldSavedSleepMs = this->sleepAfterMs;
    if (this->dimAfterMs < 300000) {
      this->dimAfterMs = 300000;     // 5 minutes
    }
    if (this->sleepAfterMs < 600000) {
      this->sleepAfterMs = 600000;   // 10 minutes
    }
  } else {
    if (this->screenAwakeHolders == 0) {
      return;                      // released more times than held: do no harm
    }
    if (--this->screenAwakeHolders > 0) {
      return;                      // somebody else still wants the screen up
    }
    this->dimAfterMs = this->heldSavedDimMs;
    this->sleepAfterMs = this->heldSavedSleepMs;
  }
  // Queued events carry the OLD deadline; re-arm them against the new one.
  this->unscheduleEvent(SCREEN_DIM_EVENT);
  this->unscheduleEvent(SCREEN_SLEEP_EVENT);
  uint32_t now = millis();
  if (this->doDimming()) {
    this->scheduleEvent(SCREEN_DIM_EVENT, now + this->dimAfterMs);
  }
  if (this->doSleeping()) {
    this->scheduleEvent(SCREEN_SLEEP_EVENT, now + this->sleepAfterMs);
  }
}

bool ControlState::loadSipAccount() {
  log_d("loadSipAccount ControlState");

  clearDynamicSip();

  bool foundAccount = false;
  CriticalFile ini(SipAccountsApp::filename);
  if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
    // Check version of the file format
    if (ini[0].hasKey("v") && !strcmp(ini[0]["v"], "1")) {
      log_d("SIP accounts file found");
      IF_LOG(VERBOSE)
      ini.show();
      for (auto si = ini.iterator(); si.valid(); ++si) {
        if (si->hasKey("m")) {       // primary account
          log_d("primary sip account found");
          setSipAccount(si->getValueSafe("d", ""),
                        si->getValueSafe("s", ""),
                        si->getValueSafe("p", ""),
                        si->getValueSafe("u", ""));// u stands for udp-sip tcp-sip selection
          foundAccount = true;
          break;
        }
      }
    } else {
      log_e("SIP accounts file corrup or unknown format");
      IF_LOG(VERBOSE)
      ini.show();
    }
  } else {
    log_d("creating SIP accounts file");
    ini[0]["desc"] = "WiPhone SIP accounts";
    ini[0]["v"] = "1";
    ini.store();
  }
  if (!foundAccount) {
    log_d("SIP account not found");
    setSipAccount("", "", "", "");
  }
  return foundAccount;
}

FocusableWidget* FocusableWidget::s_textFocus = NULL;

/* Queue the pending word for delivery, plus the key that ended it.
 *
 * ⚠ WITH NO DICTIONARY MATCH, THE DIGITS THEMSELVES ARE QUEUED. Typing something the word
 * list does not contain must never lose the keypresses — that is the moment the user reaches
 * for the multi-tap override, and they should be able to see what they typed while they do.
 * It is also what makes "no match" a fallback rather than an error. */
void ControlState::t9Commit(char trailingKey) {
  uint8_t n = 0;
  const char* word = t9.current();
  const char* src = word ? word : t9.digits();
  while (*src && n < sizeof(t9Emit) - 2) {
    t9Emit[n++] = *src++;
  }
  /* The capital goes on here, at the last possible moment, rather than being baked into the
   * dictionary or the candidate list — so cycling through candidates with UP/DOWN cannot
   * lose it, and the same word is stored once in flash however it is capitalised. */
  if (n && t9Caps && t9Emit[0] >= 'a' && t9Emit[0] <= 'z') {
    t9Emit[0] = (char)toupper(t9Emit[0]);
  }
  /* Only a word that was actually committed consumes the capital. Clearing unconditionally
   * meant every no-op commit — and there is one on each '#' press — threw away a capital the
   * user had not yet used. */
  if (n) {
    t9Caps = false;                      // one-shot, whether it came from a rule or from '#'
    t9PunctSeen = false;
  }
  if (trailingKey && n < sizeof(t9Emit) - 1) {
    t9Emit[n++] = trailingKey;
    t9NoteChar(trailingKey);             // ". " after a word still starts a new sentence
  }
  t9Emit[n] = '\0';
  t9EmitAt = 0;
  t9.clear();
}

/* Emit whatever multi-tap letter is armed, if any, and disarm. Returns the character or 0.
 *
 * The multi-tap machine only ever commits its pending letter when the NEXT key reaches the
 * bottom of alphanumericInputEvent — and every T9 path returns long before that. So anything
 * in the T9 layer that ends a multi-tap selection has to do this itself, or the letter is
 * silently lost. Two places need it: the '*' symbols row drained when a T9 path takes over,
 * and the '#' mode cycle. */
char ControlState::t9TakeArmedChar() {
  char c = 0;
  if (inputCurKey && inputCurSel < strlen(inputSeq)) {
    c = inputSeq[inputCurSel];
    if (inputShift) {
      c = (char)toupper(c);
    }
    t9NoteChar(c);
  }
  inputCurKey = 0;
  inputCurSel = 0;
  return c;
}

void ControlState::t9NoteChar(char c) {
  if (c == '.' || c == '!' || c == '?') {
    t9PunctSeen = true;                  // armed, but a sentence does not end until the space
  } else if (c == ' ' || c == '\n') {
    if (t9PunctSeen) {
      t9Caps = true;
      t9PunctSeen = false;
    }
    /* A bare space does NOT clear t9Caps: "Hello. " then a backspace-and-retype should still
     * capitalise, and a manual long-press '#' followed by a space before the word must not
     * silently lose the capital the user asked for. */
  } else {
    /* Any other character means a word is under way, so whatever capital was pending has
     * either been used or no longer applies. */
    t9PunctSeen = false;
    t9Caps = false;
  }
}

void ControlState::t9LiteralDigit(char digit, bool retractOne) {
  if (t9.pending()) {
    t9.popDigit();          // un-type the keypress the short press fed to the prediction
    /* ⚠ AND IF A WORD IS STILL PENDING, IT HAS TO GO IN FIRST. Popping the held digit off
     * leaves the rest of the word sitting in the buffer, and queuing the digit on its own
     * delivered it IMMEDIATELY while the word committed later — typing 4-3-5-5-6 and then
     * holding 7 produced "7hello" instead of "hello7". Anyone typing mp3, covid19 or
     * route66 hits it. t9Commit(digit) queues the word and then the digit, in that order. */
    if (t9.pending()) {
      inputCurKey = 0;
      inputCurSel = 0;
      t9Commit(digit);
      return;
    }
  }
  inputCurKey = 0;          // ...or abandon the multi-tap letter it started
  inputCurSel = 0;
  /* Straight into the emit queue rather than returned as a character, because this is
   * reached from the main loop's hold timer, not from a key event — there is no r1 to put
   * it in. The caller pokes processEvent afterwards to drain it.
   *
   * retractOne queues a BACKSPACE first, for the one case the prediction buffer cannot
   * undo: in T9 mode a short '0' has ALREADY put a space in the widget, and popDigit()
   * reverses the pending word, not the text. So the space is taken back out the same way
   * the user would — a BACK event through the normal widget path. WIPHONE_KEY_BACK is 8,
   * which fits this queue and passes IS_KEYBOARD, and queued events bypass
   * alphanumericInputEvent so the T9 layer never sees it and cannot re-interpret it. */
  uint8_t n = 0;
  if (retractOne) {
    t9Emit[n++] = (char)WIPHONE_KEY_BACK;
  }
  t9Emit[n++] = digit;
  t9Emit[n] = '\0';
  t9EmitAt = 0;
  /* The one emit path that bypasses alphanumericInputEvent entirely, so the tail hook there
   * cannot see it. Without this, holding a digit left the sentence-capital armed and the
   * next word came out capitalised mid-line: hold 2, then type hello, and you got "2Hello".
   * The queued BACKSPACE is a control code and correctly needs no note. */
  t9NoteChar(digit);
}

char ControlState::t9NextEmit() {
  if (t9EmitAt >= sizeof(t9Emit) || !t9Emit[t9EmitAt]) {
    return 0;
  }
  return t9Emit[t9EmitAt++];
}

void ControlState::setInputState(InputType newInputType) {
  inputType = newInputType;
  inputCurKey = 0;
  inputCurSel = 0;
  inputShift = false;
  inputSeq[0] = '\0';
  /* The pending word dies with the field. This function already runs on every focus change
   * and every app entry, which is exactly the set of moments a half-typed word must not
   * survive into somewhere else. t9Field defaults OFF here for the same reason: a screen
   * gets predictive text only by asking for it, so the 20-odd fields nobody has thought
   * about keep today's behaviour. */
  t9.clear();
  t9Emit[0] = '\0';
  t9EmitAt = 0;
  t9Field = false;
  /* A fresh field is the start of a sentence. */
  t9Caps = true;
  t9PunctSeen = false;
  /* Every field starts predictive. Stepping out with '#' is a per-field decision, not a
   * setting — leaving it sticky would mean a phone that quietly stopped predicting because
   * of something the user did in a different app twenty minutes ago. */
  inputMode = MODE_T9;
}

/* Description:
 *    set SIP account settings from function parameters
 */
void ControlState::setSipAccount(const char* dispName, const char* uri, const char* passwd, const char* UDP_TCP_SIP_Selection) {
  clearDynamicSip();

//TODO UDP_SIP set global UDP TCP SIP value on reading ini file.

  // Don't do anything if new credentials (namely SIP URI and password) are the same as existing ones
  bool sipAccountSame = (uri && fromUriDyn && !strcmp(uri, fromUriDyn)) && //Mesut: this should be AND operand
                        (passwd && proxyPassDyn && !strcmp(passwd, proxyPassDyn) &&
                         (UDP_TCP_SIP_Selection && global_UDP_TCP_SIP && !strcmp(UDP_TCP_SIP_Selection, global_UDP_TCP_SIP)));      // a strong condition
  sipAccountChanged = !sipAccountSame;

  log_d("UDP_TCP_SIP_Selection: %s", UDP_TCP_SIP_Selection);
  if(global_UDP_TCP_SIP) {
    log_d("globalUDP_TCP_SIP: %s", global_UDP_TCP_SIP);
  }

  if (sipAccountChanged) {
    fromNameDyn  = (dispName!=NULL) ? strdup(dispName) : strdup("");
    fromUriDyn   = (uri!=NULL) ? strdup(uri) : /*strdup("0@00")*/NULL;//to do put test value here
    proxyPassDyn = (passwd!=NULL) ? strdup(passwd) : strdup("");
    global_UDP_TCP_SIP = (UDP_TCP_SIP_Selection!=NULL) ? strdup(UDP_TCP_SIP_Selection) : strdup("");
    if(!strcmp(global_UDP_TCP_SIP, "UDP-SIP")) {
      UDP_SIP = true;
    } else {
      UDP_SIP = false;
    }
    log_d("new globalUDP_TCP_SIP: %s", global_UDP_TCP_SIP);
  }

  if (sipAccountChanged) {
    log_d("SIP ACCOUNT CHANGED UDP-SIP:%d", UDP_SIP);
    sipRegistered = false;
  }
}

void ControlState::removeSipAccount() {
  sipAccountChanged =  fromNameDyn &&  fromUriDyn &&  proxyPassDyn &&
                       *fromNameDyn && *fromUriDyn && *proxyPassDyn;

  freeNull((void **) &fromNameDyn);
  freeNull((void **) &fromUriDyn);
  freeNull((void **) &proxyPassDyn);
}

/* Description:
 *    set a callee before making a call
 */
void ControlState::setRemoteNameUri(const char* dispName, const char* uri) {
  this->clearDynamicCallee();
  this->calleeNameDyn = (dispName!=NULL) ? strdup(dispName) : strdup("");
  if (uri==NULL) {
    this->calleeUriDyn = NULL;//strdup("1@00");//to do create a default value
  } else if (strchr(uri, '@') == NULL && this->fromUriDyn != NULL && strchr(this->fromUriDyn, '@')!=NULL) {
    // A number provided -> form a SIP URI for the current server
    size_t len = strlen(uri) + strlen(this->fromUriDyn);
    char buff[len+5];
    char* currentServer = strchr(this->fromUriDyn, '@');

    //check whether uri starts with "sips:" or "SIPS:" or "SIP:" which can not be called (tested)
    if(strncmp(uri, "sips:", 5) == 0 || strncmp(uri, "SIPS:", 5) == 0 || strncmp(uri, "SIP:", 4) == 0) {
      //TODO display error popup which says we do not support it and do nothing.
      //now, we set the buff for sake of safety purposes
      sprintf(buff, "%s%s", uri, currentServer);
    } else {
      //check whether uri starts with "sip:"
      if(strncmp(uri, "sip:", 4) == 0) {
        sprintf(buff, "%s%s", uri, currentServer);
      } else {
        sprintf(buff, "sip:%s%s", uri, currentServer);
      }
    }
    this->calleeUriDyn = strdup(buff);
  } else {
    this->calleeUriDyn = strdup(uri);
  }
}

void ControlState::setSipReason(const char* text) {
  freeNull((void **) &lastReasonDyn);
  lastReasonDyn = (text!=NULL) ? strdup(text) : strdup("");
  log_d("setSipReason: %s", lastReasonDyn);
}

void showCallState(CallState state) {
  switch (state) {
  case CallState::NotInited:
    log_d("NotInited");
    break;
  case CallState::Idle:
    log_d("Idle");
    break;
  case CallState::InvitingCallee:
    log_d("InvitingCallee");
    break;
  case CallState::InvitedCallee:
    log_d("InvitedCallee");
    break;
  case CallState::RemoteRinging:
    log_d("RemoteRinging");
    break;
  case CallState::Call:
    log_d("Call");
    break;
  case CallState::HangUp:
    log_d("HangUp");
    break;
  case CallState::HangingUp:
    log_d("HangingUp");
    break;
  case CallState::HungUp:
    log_d("HungUp");
    break;

  case CallState::BeingInvited:
    log_d("BeingInvited");
    break;
  case CallState::Accept:
    log_d("Accept");
    break;
  case CallState::Decline:
    log_d("Decline");
    break;

  case CallState::Error:
    log_d("Error");
    break;
  default:
    log_d("UNKNOWN=");
    log_d("%d", (int) state);
    break;
  }
}

/* MISSED CALLS — observed here because every call transition passes through this one
 * function, so no call-flow logic is touched anywhere. An incoming ring arms the tracker;
 * reaching Call disarms it (answered); returning to rest while still armed is a miss.
 * Statics + accessors, no heap. The clock shows it; opening the dialer or the phonebook
 * clears it (see enterApp) — you looked, so it stops nagging. */
static char s_missedName[64] = {0};   // room for a full SIP URI, not just a {user}name
static int  s_missedCount = 0;
static bool s_ringArmed = false;

int missedCallCount() {
  return s_missedCount;
}

const char* missedCallName() {
  return s_missedName;
}

void missedCallsClear() {
  s_missedCount = 0;
  s_missedName[0] = '\0';
}

void ControlState::setSipState(CallState newState) {
  log_d("CALL STATE TRANSITION: ");
  showCallState(sipState);
  log_d(" -> ");
  showCallState(newState);
  log_d("");

  if (newState == CallState::BeingInvited) {
    s_ringArmed = true;
  } else if (newState == CallState::Call || newState == CallState::Accept ||
             newState == CallState::Decline) {
    /* Answered, accepted, or DECLINED - all mean the user saw the ring and acted.
     * A declined call nagging as "missed" would train people to ignore the nag. */
    s_ringArmed = false;
  } else if (s_ringArmed &&
             (newState == CallState::Idle || newState == CallState::HungUp)) {
    s_ringArmed = false;
    s_missedCount++;
    // The remote's display name, or their URI when there is none. Captured NOW, before
    // the next call overwrites the shared state.
    const char* who = (calleeNameDyn && calleeNameDyn[0]) ? calleeNameDyn
                      : (calleeUriDyn ? calleeUriDyn : "");
    snprintf(s_missedName, sizeof(s_missedName), "%s", who);
  }

  sipState = newState;
}

bool ControlState::scheduleEvent(EventType event, uint32_t msTriggerAt) {
  if (eventQueue.size() >= ControlState::MAX_EVENTS) {
    return false;
  }

  // Find a spot to insert the event into
  uint32_t pos = eventQueue.size();
  for (auto it = eventQueue.iterator(); it.valid(); ++it) {
    if (it->msTriggerAt - msTriggerAt < (UINT32_MAX>>4)) {       // Actually what we want to check is: it->msTriggerAt > msTriggerAt
      pos = (int)it;
      break;
    }
  }

  // Insert the new event
  log_v("schedule event: %x (%u), at %d", event, msTriggerAt, pos);
  QueuedEvent qe;
  qe.event = event;
  qe.msTriggerAt = msTriggerAt;
  eventQueue.insert(pos, qe);
  return true;
}

EventType ControlState::popEvent(uint32_t msNow) {
  if (eventQueue.size() && msNow - eventQueue[0].msTriggerAt < (UINT32_MAX>>4)) {     // Actually what we want to check is: msNow > eventQueue[0].msTriggerAt
    EventType res = eventQueue[0].event;
    eventQueue.remove(0);
    log_v("pop event: 0x%x", res);
    return res;
  }
  return EventType(0);
}

void ControlState::unscheduleEvent(EventType eventType) {
  for (int i = 0; i < eventQueue.size(); i++) {
    if (eventQueue[i].event == eventType) {
      log_d("removed event: %d", i);
      eventQueue.remove(i--);
    }
  }
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  INPUT  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

/* Description:
 *      determine the input and change the input state for displaying
 * Input:
 *      key - button recently pressed
 * Return values:
 *      r1  - first return character
 *      r2  - second return charater (for example, when "a" is currently selected and then DOWN is pressed, r1 will be "a", r2 will be DOWN)
 * Implicit return values (global state variables):
 *      state.inputShift
 *      state.inputCurKey
 *      state.inputCurSel
 *      state.inputSeq
 */
void GUI::alphanumericInputEvent(EventType key, EventType& r1, EventType& r2) {
  log_v("alphanumericInputEvent:", key);

  r1 = 0;
  r2 = 0;
  if (state.inputType!=InputType::AlphaNum) {
    guiError("unsupported");
    return;
  }
  if (key==WIPHONE_SHIFT_KEY) {
    if (state.t9Available()) {
      /* Cycle T9 -> Abc -> ABC -> 123 -> T9. Nothing half-typed is thrown away: a pending
       * PREDICTION commits, and a half-typed MULTI-TAP letter is emitted rather than
       * discarded — clearing inputCurKey without emitting was silently eating it, which the
       * previous version of this comment claimed it did not do.
       *
       * ⚠ t9Commit() ONLY WHEN A WORD IS PENDING. It clears t9Caps unconditionally, so
       * calling it on every '#' press meant looking at the modes and coming back destroyed
       * the automatic capital: a fresh Compose, four presses of '#', then "hello" gave
       * "hello" rather than "Hello". */
      if (state.t9.pending()) {
        state.t9Commit();
      }
      const char armed = state.t9TakeArmedChar();
      if (armed) {
        r1 = armed;
      }
      state.inputMode = (state.inputMode + 1) & 3;
      state.inputShift = (state.inputMode == ControlState::MODE_ABC_CAPS);
      return;
    }
    // Field without predictive text: '#' is the shift toggle it has always been.
    state.inputShift = !state.inputShift;
    return;
  }

  /* 123: the digit types itself. This is the answer to "testing 1 2 3" — no prediction, no
   * multi-tap cycle, no waiting for a timeout, one press one digit. */
  if (state.t9Available() && state.inputMode == ControlState::MODE_123) {
    if (key >= '0' && key <= '9') {
      r1 = key;
      state.t9NoteChar((char)r1);        // early return: the tracker is at the far end
      return;
    }
  }

  /* ── T9 ────────────────────────────────────────────────────────────────────────────────
   * Sits BESIDE the multi-tap machine below, never inside it. Everything here is gated on
   * t9Live(), so with the setting off — or on any field that did not opt in — control falls
   * straight through and the code below runs exactly as it always has.
   *
   * The pending word is shown in the FOOTER and inserted into the widget only when it is
   * committed. That is deliberate and it is the reason this integration is small: nothing
   * has to reach into the text widget to rewrite a half-typed word, so the digit buffer and
   * the on-screen text cannot drift apart. See FooterWidget::redraw.
   *
   * UP/DOWN cycle candidates ONLY while a word is pending; with nothing pending they are not
   * touched and still move focus or the cursor. Same shape as the OK rule further down —
   * consume a key only in the state where it means something new. */
  if (state.t9Live()) {
    /* 🛑 DRAIN AN ARMED SYMBOLS-ROW SELECTION FIRST.
     * Every path below RETURNS, and the multi-tap machine further down is the only code that
     * commits a pending '*' selection. So pressing '*' (arming '.') and then '0' emitted the
     * space and left the '.' armed: on hardware that gave "Hello ." instead of "Hello. ",
     * and if the screen changed before the 2 s idle timeout the full stop was lost outright.
     * It also left inputCurKey set, which blanks the prediction strip in FooterWidget.
     * This is the same commit the multi-tap branch does, hoisted to where T9 can reach it. */
    if (state.inputCurKey && state.inputCurKey != key) {
      const char armed = state.t9TakeArmedChar();
      if (armed) {
        r1 = armed;
      }
    }
    if (t9IsWordDigit(key)) {
      state.t9.pushDigit(key);
      return;                                  // consumed; the footer shows the candidate
    }
    /* 🛑 '*' MUST NOT BE QUEUED, IT MUST FALL THROUGH.
     * The emit queue re-delivers whatever it holds as an EVENT, which is exactly right for
     * the non-printable keys — END still cancels, LEFT still moves the cursor. But '*' is a
     * printable character (0x2A) whose MEANING comes from this function: it opens the
     * symbols row. Queued items bypass this function, so committing "there" and queuing '*'
     * typed a literal asterisk — "Hello there*" on hardware — instead of offering
     * punctuation. Commit the word with no trailing key and let the multi-tap path below
     * handle the keypress as it always has. */
    if (state.t9.pending() && key == WIPHONE_SYMBOLS_KEY) {
      state.t9Commit();
      // deliberately no return: '*' still has its own job to do below
    } else if (state.t9.pending()) {
      switch (key) {
      case WIPHONE_KEY_UP:
        state.t9.prevCandidate();
        return;
      case WIPHONE_KEY_DOWN:
        state.t9.nextCandidate();
        return;
      case WIPHONE_KEY_BACK:
        /* One BACK un-types one KEYPRESS, which is one digit — not one letter of whatever
         * candidate happens to be showing. Deleting a character here instead is the classic
         * way to desynchronise the two. */
        state.t9.popDigit();
        return;
      case WIPHONE_KEY_OK:
        state.t9Commit();                      // accept the word, consume the key
        return;
      case '0':
        state.t9Commit(' ');                   // 0 is space, and space ends a word
        return;
      case '1':
        state.t9Commit('1');                   // '1' spells nothing: end the word, type it
        return;
      default:
        /* Anything else ends the word and then still does its own job: the key is queued
         * BEHIND the word so "word then punctuation" arrives in that order. */
        state.t9Commit((char)key);
        return;
      }
    }
    /* Nothing pending and not a word digit. '0' is space and '1' spells nothing, so both
     * should land on one press rather than entering a multi-tap cycle that needs a second
     * key or a timeout to commit. */
    /* ⚠ THESE RETURN EARLY, so they must feed the capitalisation tracker themselves — the
     * hook at the end of this function is never reached from here. Missing it is why a
     * full stop typed from the symbols row did not capitalise the next word: the '.' was
     * noted correctly, and then the space that completes the sentence was not. */
    /* r2, not r1, when a symbol was just drained above — one keypress can carry two
     * characters and the symbol was typed first. The caller delivers r1 then r2. */
    if (key == '0') {
      (r1 ? r2 : r1) = ' ';
      state.t9NoteChar(' ');
      return;
    }
    if (key == '1') {
      (r1 ? r2 : r1) = '1';
      state.t9NoteChar('1');
      return;
    }
  }

  // Check if any button is "active" now (meaning a button is pressed and selection of its symbols is displayed)
  if (state.inputCurKey && state.inputCurKey == key) {
    // same button is active now -> cycle to next candidate
    state.inputCurSel = (state.inputCurSel + 1) % strlen(state.inputSeq);
  } else {
    if (state.inputCurKey) {
      // different button is active now -> return the current candidate (currently selected character)
      if (state.inputCurSel<strlen(state.inputSeq)) {
        r1 = state.inputSeq[state.inputCurSel];
      }
      if (state.inputShift) {
        r1 = toupper(r1);
      }
    }

    if (key>=32 && key<=126) {
      // make new printable button active
      int8_t i = -1;
      if (key>='0' && key<='9') {
        i = key - '0';
      } else if (key==WIPHONE_SYMBOLS_KEY) {
        i = 10;
      } else {
        guiError("unknown key");
      }
      if (i>=0) {
        strcpy(state.inputSeq, alphNum[i]);
        state.inputCurKey = key;
        state.inputCurSel = 0;
      }
    } else {
      // non-printable button pressed
      state.inputCurKey = 0;    // no button is active

      /* OK COMMITS THE HIGHLIGHTED CANDIDATE AND IS CONSUMED DOING IT.
       * Pressing the same button twice cycles the candidate instead of committing it, so
       * without this "aaa" costs two full KEYPAD_IDLE_MS waits. OK is the old-phone answer:
       * tap, OK, tap, OK, as fast as the fingers go. The timeout still commits on its own —
       * this is an ADDITIONAL way to commit, not a replacement.
       *
       * r1 is non-zero here only when the block above just committed a character, which is
       * exactly when the footer is showing the candidate strip. With nothing pending r1 is 0,
       * so OK falls through to the app unchanged and every screen that uses it to send/save
       * behaves as before. Only WIPHONE_KEY_OK is taken: CALL and SELECT are also
       * LOGIC_BUTTON_OK on this keyboard and still fire outright, so CALL remains a one-press
       * "send now" even with a letter pending. */
      if (key != WIPHONE_KEY_OK || !r1) {
        r2 = key;
      }
    }
  }
  if (!r1) {
    r1 = r2;
    r2 = 0;
  }
  /* The single point every emitted character passes through — multi-tap letters, the
   * symbols row, the space from '0'. Committed T9 words are noted by t9Commit() instead,
   * because they never come back through here. */
  if (r1 >= 32 && r1 <= 126) {
    state.t9NoteChar((char)r1);
  }
}

appEventResult GUI::processEvent(uint32_t now, EventType event) {
  appEventResult res = DO_NOTHING;

  // Pre-process
  EventType keyNext = 0;   // one keypress can generate two inputs sometimes
  if (IS_KEYBOARD(event)) {
    this->msLastKeypadEvent = now;

    // Process unlocking
    log_d("locked = %d, event = %x", state.locked, event);
    if (state.locked) {
      if (inCall()) {
        log_d("IN A CALL. UNLOCKED ON ANY KEY");
        // if we are in a call (probably receiving one), unlock on any key
        state.locked = false;
        res |= REDRAW_SCREEN | LOCK_UNLOCK;
      } else {
        state.unscheduleEvent(UNLOCK_CLEAR_EVENT);
        if (event == WIPHONE_KEY_OK) {
          state.unlockButton1 = event;
          state.scheduleEvent(UNLOCK_CLEAR_EVENT, now + 2500);
          res |= REDRAW_FOOTER;
          log_d("OK pressed: %d", state.unlockButton1);
        } else if (state.unlockButton1 == WIPHONE_KEY_OK) {
          if (event == WIPHONE_UNLOCK_KEY2) {
            // Unlocked successfully
            state.locked = false;
            res |= REDRAW_SCREEN | LOCK_UNLOCK;
          }
          if (state.unlockButton1) {
            state.unlockButton1 = 0;
            res |= REDRAW_FOOTER;
            log_d("state.unlockButton1 cleared");
          }
        } else if (state.unlockButton1) {
          state.unlockButton1 = 0;
          res |= REDRAW_FOOTER;
          log_d("state.unlockButton1 cleared");
        }
        event = keyNext = 0;
      }
    }

    // Restore screen brightness
    if (state.screenBrightness < state.brightLevel) {
      // If screen is completely OFF -> forget this key, just reveal the screen
      if (state.screenBrightness <= 0) {
        event = keyNext = 0;
        state.screenWakeUp = true;
        res |= REDRAW_ALL;
      } else {
        // Maximize screen brightness according to the settings
#if GPIO_EXTENDER == 1509
        lcdLedOnOff(true, conv100to255(state.brightLevel));
#endif // GPIO_EXTENDER == 1509
        state.screenBrightness = state.brightLevel;
      }
    }

    // Reset screen dimming and sleeping events
    state.unscheduleEvent(SCREEN_SLEEP_EVENT);
    state.unscheduleEvent(SCREEN_DIM_EVENT);

    // Schedule screen dimming
    if (state.doDimming()) {
      // Dimming is effectively ON -> schedule dimming event
      state.scheduleEvent(SCREEN_DIM_EVENT, now + state.dimAfterMs);
    }
    if (state.doSleeping()) {
      // Sleeping is effectively ON -> schedule sleep event
      state.scheduleEvent(SCREEN_SLEEP_EVENT, now + state.sleepAfterMs);
    }

    /* Decode button.
     * ⚠ `event` may be ZERO here: IS_KEYBOARD() is `event <= 0x7f`, so 0 enters this block,
     * and several things above deliberately zero it — the unlock, a screen-off wake, and
     * the music transport keys the main loop swallows (`keyPressed = 0`) when a track is
     * loaded. Passing 0 to alphanumericInputEvent() is not harmless: it is not the active
     * key, so the "different button" branch runs and COMMITS the pending multi-tap letter.
     * Nudge the volume while half-way through cycling to a letter and the letter lands
     * early. Everything else above still runs on a swallowed key on purpose — the screen
     * timeout should be reset by a volume press. */
    if (state.inputType!=InputType::Numeric && event) {
      alphanumericInputEvent(event, event, keyNext);
      res |= REDRAW_FOOTER;
    }

  } else if (event == SCREEN_DIM_EVENT) {
    if (state.doDimming()) {
      if (state.screenBrightness > state.dimLevel) {
        // Adjust screen brightness
        state.screenBrightness -= 5;
#if GPIO_EXTENDER == 1509
        log_d("@ SCREEN_DIM_EVENT: %d <- %d", state.dimLevel, state.screenBrightness);
        lcdLedOnOff(true, conv100to255(state.screenBrightness));
#endif // GPIO_EXTENDER == 1509

        // Schedule next event
        if (state.screenBrightness > state.dimLevel) {
          log_d("event scheduled");
          state.scheduleEvent(SCREEN_DIM_EVENT, now + 33);
        }
      } else {
        log_d("SCREEN_DIM_EVENT - no effect");
      }
    } else {
      // Event happened, but dimming is effectively OFF -> reset screen to bright level
      state.screenBrightness = state.brightLevel;

#if GPIO_EXTENDER == 1509
      log_d("@ SCREEN_DIM_EVENT 2");
      lcdLedOnOff(true, conv100to255(state.screenBrightness));
#endif // GPIO_EXTENDER == 1509
    }
    event = EventType(0);
  } else if (event == SCREEN_SLEEP_EVENT) {
    if (state.doSleeping()) {
      // Screen sleep is ON: turn the screen OFF
      log_d("SCREEN OFF @ SCREEN_SLEEP_EVENT");
      lcdLedOnOff(false);
      state.screenBrightness = 0;     // screen is sleeping
      if (state.locking) {
        state.locked = true;
        res |= REDRAW_SCREEN | LOCK_UNLOCK;
      }
    }
  } else if (event == UNLOCK_CLEAR_EVENT) {
    log_d("UNLOCK_CLEAR_EVENT");
    if (state.unlockButton1) {
      state.unlockButton1 = 0;
      res |= REDRAW_FOOTER;
      log_d("state.unlockButton1 cleared");
    }
    event = EventType(0);
  }

  /* 🔑 PRIME THE LOOP FROM THE T9 QUEUE. The key that commits a word is CONSUMED by
   * alphanumericInputEvent — it returns r1 = 0 — so `event` is already zero here and the
   * loop below would never run. The queued word would sit there until the next keypress
   * dragged one character out of it, which is exactly the symptom: footer clears, field
   * stays empty. The drain at the bottom of the loop handles the SECOND character onwards;
   * this handles the first. */
  if (!event) {
    event = (EventType)state.t9NextEmit();
  }

  /* 🔑 PRIME THE LOOP FROM THE T9 QUEUE, or the committed word arrives late.
   * The key that commits a word is CONSUMED by alphanumericInputEvent — it returns r1 = 0 —
   * so `event` is already zero here and the loop below never starts. The word then sat in
   * the queue and was drained ONE CHARACTER PER BACKGROUND EVENT: on hardware "hello"
   * appeared letter by letter over the next couple of seconds as health ticks and timers
   * came through, which looks exactly like a slow phone rather than like a bug.
   * Priming here starts the loop; the drain at the bottom then delivers the rest of the
   * word in the SAME pass. */
  if (!event) {
    event = (EventType)state.t9NextEmit();
  }

  while(event) {

    // Since one keypress can generate two keyboard events, we need a loop to process them both

    // Logging
    if (event != APP_TIMER_EVENT || state.msAppTimerEventPeriod>1000) {
      const char* p = nullptr;
      if (event>=0x20 && event<0x7F) {
        log_i("key=%c", event);  // printable key
      } else if (event<0x7F) {
        // Show non-printable keys
        switch (event) {
        case WIPHONE_KEY_OK:
          p = "OK";
          break;
        case WIPHONE_KEY_UP:
          p = "Up";
          break;
        case WIPHONE_KEY_DOWN:
          p = "Down";
          break;
        case WIPHONE_KEY_BACK:
          p = "Back";
          break;
        case WIPHONE_KEY_LEFT:
          p = "Left";
          break;
        case WIPHONE_KEY_RIGHT:
          p = "Right";
          break;
        case WIPHONE_KEY_SELECT:
          p = "Select";
          break;
        case WIPHONE_KEY_CALL:
          p = "Call";
          break;
        case WIPHONE_KEY_END:
          p = "End";
          break;
        case WIPHONE_KEY_F1:
          p = "F1";
          break;
        case WIPHONE_KEY_F2:
          p = "F2";
          break;
        case WIPHONE_KEY_F3:
          p = "F3";
          break;
        case WIPHONE_KEY_F4:
          p = "F4";
          break;
        case 0:
          p = "NUL";
          break;
        default:
          break;
        }
        if (p != nullptr) {
          log_i("key=%s", p);
        } else {
          log_i("key=0x%x", event);
        }
      } else {
        // Show other events
        switch (event) {
        case KEYBOARD_TIMEOUT_EVENT:
          p = "KEYBOARD_TIMEOUT_EVENT";
          break;
        case APP_TIMER_EVENT:
          p = "APP_TIMER_EVENT";
          break;
        case BATTERY_UPDATE_EVENT:
          p = "BATTERY_UPDATE_EVENT";
          break;
        case CALL_UPDATE_EVENT:
          p = "CALL_UPDATE_EVENT";
          break;
        case WIFI_ICON_UPDATE_EVENT:
          p = "WIFI_ICON_UPDATE_EVENT";
          break;
        case TIME_UPDATE_EVENT:
          p = "TIME_UPDATE_EVENT";
          break;
        case USER_SERIAL_EVENT:
          p = "USER_SERIAL_EVENT";
          break;
        case REGISTRATION_UPDATE_EVENT:
          p = "REGISTRATION_UPDATE_EVENT";
          break;
        case BATTERY_BLINK_EVENT:
          p = "BATTERY_BLINK_EVENT";
          break;
        case USB_UPDATE_EVENT:
          p = "USB_UPDATE_EVENT";
          break;
        default:
          break;
        }
        if (p != nullptr) {
          //log_i("%s", p);
        } else {
          log_i("unnamed event: 0x%x", event);
        }
      }
    }

    bool messageIconAppears = false;
    if (event == POWER_OFF_EVENT) {

      powerOffScreen = true;
      this->drawPowerOff();

    } else if (event == POWER_NOT_OFF_EVENT) {

      log_v("POWER_NOT_OFF_EVENT");
      powerOffScreen = false;

    } else if (event == NEW_MESSAGE_EVENT) {

      if (!state.unreadMessages) {
        messageIconAppears = flash.messages.hasUnread();  // update (show) message icon
      }

      // Reload messages to display in the messages app (this will also set state.unreadMessages)
      this->reloadMessages();

    }

    // Feed event into specific "apps"
    if (callApp!=NULL) {

      // Special case for calling app
      appEventResult appRes = callApp->processEvent(event);
      if (appRes & EXIT_APP) {
        exitCall();
        res |= (!runningApp || runningApp->isWindowed()) ? REDRAW_ALL : REDRAW_SCREEN;
        // TODO: restore header/footer
      } else {
        res |= appRes;
      }

      // Special cases for redrawing
      if ((event == TIME_UPDATE_EVENT || event == WIFI_ICON_UPDATE_EVENT) && callApp->isWindowed()) {
        res |= REDRAW_HEADER;
      } else if (event == POWER_NOT_OFF_EVENT) {
        res |= callApp->isWindowed() ? REDRAW_ALL : REDRAW_SCREEN;  // TODO: maybe just prohibit drawing headers over non-windowed apps
      } else if (messageIconAppears && callApp->isWindowed()) {
        res |= REDRAW_HEADER;
      }

    } else if (runningApp!=NULL) {

      appEventResult appRes = runningApp->processEvent(event);
      if (appRes & EXIT_APP) {
        ActionID_t appID = runningApp->getId();
        // Running app exited
        log_d("deleting app");
        FocusableWidget::clearTextFocus();   // its widgets are about to go: see clearTextFocus
        state.t9Field = false;               // ...and so is any predictive-text claim
        delete runningApp;
        runningApp = NULL;

        // Go back to the main menu
        if (appID == GUI_APP_SPLASH) {
          // Special case: splash screen exited -> go to clock
          log_d("CLOCK, event = %x", event);
          enterApp(GUI_APP_CLOCK);
        } else if (appRes & ENTER_DIAL_APP) {
          log_d("DIALING");
          enterApp(GUI_APP_DIALING);
          if (runningApp) {
            runningApp->processEvent(event);
          }
        } else if (appRes & ENTER_MESSAGES_APP) {
          log_d("MESSAGES (envelope shortcut)");
          enterApp(GUI_APP_MESSAGES);
        } else {
          log_d("MENU, event = %x", event);
          enterApp(GUI_APP_MENU);

          // Restore header and footer text
          int16_t menuIndex = findMenu(curMenuId);
          if (menuIndex>=0) {
            header->setTitle(menu[menuIndex].title);
            footer->setButtons(menu[menuIndex].leftButton, menu[menuIndex].rightButton);
          }
        }

        res |= (runningApp && runningApp->isWindowed()) ? REDRAW_ALL : REDRAW_SCREEN;
      } else {
        res |= appRes;
      }

      // Special cases for redrawing
      if ((event == TIME_UPDATE_EVENT || event == WIFI_ICON_UPDATE_EVENT) && runningApp && runningApp->isWindowed()) {
        res |= REDRAW_HEADER;
      } else if (event == POWER_NOT_OFF_EVENT) {
        res |= runningApp->isWindowed() ? REDRAW_ALL : REDRAW_SCREEN;
      } else if (messageIconAppears && runningApp->isWindowed()) {
        res |= REDRAW_HEADER;
      }

    } else if (curApp == GUI_APP_MENU) {

      if (LOGIC_BUTTON_BACK(event)) {
        if (curMenuId==1) {
          enterApp(GUI_APP_CLOCK);
        } else {
          exitMenu();
        }
        res |= REDRAW_SCREEN;
      } else if (NONKEY_EVENT_ONE_OF(event, TIME_UPDATE_EVENT | BATTERY_UPDATE_EVENT | WIFI_ICON_UPDATE_EVENT | BATTERY_BLINK_EVENT | USB_UPDATE_EVENT)) {
        res |= REDRAW_HEADER;
      } else if (event == POWER_NOT_OFF_EVENT) {
        res |= REDRAW_ALL;
      } else if (messageIconAppears) {
        res |= REDRAW_HEADER;
      } else if (event >= '0' && event <='9' || event == '*' || event == '#') {
        enterApp(GUI_APP_DIALING);
        if (runningApp) {
          runningApp->processEvent(event);
        }
        res |= REDRAW_ALL;
      } else if (mainMenu) {
        mainMenu->processEvent(event);
        uint16_t ID = mainMenu->readChosen();
        if (LOGIC_BUTTON_OK(event)) {
          int16_t ci = findMenu(ID);
          if (ci<0) {
            guiError("menu failed");
            return res;
          }
          if (menu[ci].action == GUI_ACTION_SUBMENU) {
            enterMenu(menu[ci].ID);
          } else if (menu[ci].action & GUI_BASE_APP) {
            enterApp(menu[ci].action);
            if (runningApp) {
              res |= runningApp->isWindowed() ? REDRAW_ALL : REDRAW_SCREEN;  // initialize screen for the newly launched app
            }
          } else if (menu[ci].action == GUI_ACTION_RESTART) {
            /* A download's cursor and a clean stop reason first: a restart asked for from the menu
             * is not a crash, and must not be counted as one (tile_fetch.h). */
            extern void tileFetchPowerOff();
            tileFetchPowerOff();
            ESP.restart();
          } else if (menu[ci].action == GUI_ACTION_T9_TOGGLE) {
            /* ⭐ One press, in Settings, next to the other things that change how the phone
             * behaves rather than what it is connected to.
             *
             * PERSISTED, unlike the WiFi row above — and the difference is deliberate. A
             * radio that stays off across a reboot is a setting you can forget you set, and
             * the failure mode is a phone that silently never connects. A text-entry
             * preference has no such failure mode: it is visible in the footer the moment
             * you type, and '#' steps out of it. Somebody who turns prediction off means it
             * to stay off. */
            state.t9Enabled = !state.t9Enabled;
            {
              CriticalFile ini(Storage::ConfigsFile);
              if (ini.load() || ini.restore()) {
                if (!ini.hasSection("input")) {
                  ini.addSection("input");
                }
                ini["input"]["predictive"] = state.t9Enabled ? 1 : 0;
                if (!ini.store()) {
                  log_e("t9: could not save the predictive-text setting");
                }
              }
            }
            log_e("T9 toggled -> %s (from the menu)", state.t9Enabled ? "ON" : "OFF");
            enterMenu(curMenuId);            // rebuild so the row relabels itself

          } else if (menu[ci].action == GUI_ACTION_WIFI_TOGGLE) {
            /* ⭐ ONE PRESS, IN SETTINGS, leading the WiFi group. The same switch already
             * existed inside the network EDIT form, which meant "turn WiFi off to save
             * power" was: menu, Settings, pick a network, edit, find the choice widget —
             * and it told you nothing about the current state until you got there. It sat
             * on the main menu for a few hours on 2026-08-23 and {user} moved it in here,
             * which is where a radio switch belongs alongside the three rows that assume it.
             *
             * This dispatch and the label builder both key off `action` and never look at
             * the parent, so the row can be re-homed by editing its table line alone.
             *
             * ⚠ NOT PERSISTED, and that is deliberate: WiFi comes back ON after a reboot. A
             * radio that stays off across a power cycle is a setting you can forget you set,
             * and the failure mode is a phone that silently never connects again. */
            const bool turningOn = wifiState.radioOff();
            /* One call: it flips the switch, persists it, and applies it (disable() or
             * resumeReconnect()). The old body did those three things inline and the
             * persistence half did not exist. */
            wifiState.setRadioOff(!turningOn);
            /* wifiRestoreStation(), NOT a bare esp_wifi_start() (0.9.79). esp_wifi_start()
             * restarts whatever mode the DRIVER last had — WiFi.mode(WIFI_OFF) never resets it —
             * so after a sync window's hotspot closed with WiFi off, "WiFi on" here put an
             * unserved soft-AP on the air instead of the station, and Arduino's getMode() went
             * on reading NULL beside it. The helper sets STA first, and rejoins when it may. */
            if (turningOn && !wifiRestoreStation("Settings: WiFi on")) {
              log_e("WIFI can't be started");
              wifiState.setRadioOff(true);   // say what is true, not what was asked for
            }
            enterMenu(curMenuId);            // rebuild so the row relabels itself
          }
        }
        res |= REDRAW_SCREEN;
      }

    } else if (LOGIC_BUTTON_BACK(event)) {
      // Some other app (non-class app)
      enterApp(GUI_APP_MENU);
      res |= REDRAW_ALL;
    }

    // second key (if any)
    event = keyNext;
    keyNext = 0;
    /* ...and then any characters a committed T9 word left queued. A word is longer than the
     * two events one keypress can carry, so it is delivered one character per pass through
     * this loop, down the same path multi-tap's characters take. */
    if (!event) {
      event = (EventType)state.t9NextEmit();
    }
  };
  return res;
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  FONT LOADER  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

FontCollection fonts;

FontCollection::FontCollection() {
  memset(fnt, 0, sizeof(fnt));
};

FontCollection::~FontCollection() {
  for (uint8_t i=0; i<sizeof(fnt)/sizeof(SmoothFont*); i++)
    if (fnt[i]) {
      delete fnt[i];
    }
}

SmoothFont* FontCollection::operator[](int index) {
  if (!fnt[index]) {
    fnt[index] = new SmoothFont;
    switch(index) {
    case OPENSANS_COND_BOLD_20:
      fnt[OPENSANS_COND_BOLD_20]->loadFont(OpenSans_CondBold20);
      break;
    case AKROBAT_BOLD_16:
      fnt[AKROBAT_BOLD_16]->loadFont(Akrobat_Bold16);
      break;
    case AKROBAT_BOLD_18:
      fnt[AKROBAT_BOLD_18]->loadFont(Akrobat_Bold18);
      break;
    case AKROBAT_BOLD_20:
      fnt[AKROBAT_BOLD_20]->loadFont(Akrobat_Bold20);
      break;
    case AKROBAT_BOLD_22:
      fnt[AKROBAT_BOLD_22]->loadFont(Akrobat_Bold22);
      break;
    case AKROBAT_BOLD_24:
      fnt[AKROBAT_BOLD_24]->loadFont(Akrobat_Bold24);
      break;
    case AKROBAT_SEMIBOLD_20:
      fnt[AKROBAT_SEMIBOLD_20]->loadFont(Akrobat_SemiBold20);
      break;
    case AKROBAT_SEMIBOLD_22:
      fnt[AKROBAT_SEMIBOLD_22]->loadFont(Akrobat_SemiBold22);
      break;
    case AKROBAT_EXTRABOLD_22:
      fnt[AKROBAT_EXTRABOLD_22]->loadFont(Akrobat_ExtraBold22);
      break;
    case AKROBAT_BOLD_32:
      fnt[AKROBAT_BOLD_32]->loadFont(Akrobat_Bold32);
      break;
    case AKROBAT_BOLD_90:
      fnt[AKROBAT_BOLD_90]->loadFont(Akrobat_Bold90);
      break;
    }
  }
  return fnt[index];
}

SmoothFont* FontCollection::operator[](FontIndex_t index) {
  return this->operator[]((int) index);
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  DRAWING  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

/* Set by every redraw and read by the CPU-frequency gate in WiPhone.ino. A lit screen
 * that is merely BEING STARED AT does not need 240 MHz; a lit screen that is being
 * redrawn does. Every redraw in the firmware funnels through this one function, so one
 * stamp here covers all twelve call sites and anything added later. */
volatile uint32_t gUiWorkMs = 0;

void GUI::redrawScreen(bool redrawHeader, bool redrawFooter, bool redrawScreen, bool redrawAll) {
  gUiWorkMs = millis();
  //log_v("GUI: %d %d %d %d", redrawHeader, redrawFooter, redrawScreen, redrawAll);
  if (!redrawHeader && !redrawFooter && !redrawScreen && !redrawAll) {
    log_d("nothing to redraw");
    return;
  }

  if (powerOffScreen) {
    return;  // don't draw anything while expecting a power shutdown
  }

  // Step 1: redraw main (middle) part of screen

  bool hfDrawn = false;
  LCD* appScreen = screen;
  if (state.locked && (redrawFooter || redrawHeader) && clockApp!=NULL) {
    redrawScreen = true;
    redrawHeader = redrawFooter = false;
  }

  if (redrawScreen) {

    if (callApp!=NULL) {

      callApp->resetPush();
      callApp->redrawScreen(redrawAll);
      appScreen = &(callApp->getScreen());
      if (redrawAll && callApp->isWindowed()) {
        redrawHeader = redrawFooter = true;
      }

    } else if (state.locked && clockApp!=NULL) {

      clockApp->resetPush();
      clockApp->redrawScreen(redrawAll);
      appScreen = &(clockApp->getScreen());
      if (!clockApp->isWindowed()) {
        hfDrawn = true;
      }

    } else if (runningApp!=NULL) {

      runningApp->resetPush();
      runningApp->redrawScreen(redrawAll);
      appScreen = &(runningApp->getScreen());

      if (!runningApp->isWindowed()) {
        hfDrawn = true;
      } else if (redrawAll && runningApp->isWindowed()) {
        redrawHeader = redrawFooter = true;
      }

    } else if (curApp == GUI_APP_MENU) {

      // Menu appearance: special case to allow background image

      //log_d("REDRAW main menu");
      if (!menuDrawn) {
        if (bgImage && bgImage->isCreated() && screen->isSprite()) {
          bgImage->cloneDataInto((TFT_eSprite*) screen);
        } else {
          // Clear the whole screen (header and footer are repainted on top just
          // below). Clearing only the middle band left a strip uncleared (the
          // old code subtracted footer height twice instead of header+footer),
          // so a full-screen app's leftover pixels bled through on return.
          screen->fillRect(0, 0, lcd.width(), lcd.height(), THEME_BG);
        }
        ((GUIWidget*)header)->redraw(*screen);
        ((GUIWidget*)footer)->redraw(*screen);
        hfDrawn = true;
      }

      if (mainMenu) {
        ((GUIWidget*)mainMenu)->redraw(*screen);
      }

      if (!screen->isSprite()) {
        menuDrawn = true;
        menuNewItems = false;
      }

    } else {

      // Unknown app screen (this should never happen)
      log_d("REDRAW unknown");
      screen->fillScreen(THEME_BG);
      screen->setTextColor(THEME_COLOR, THEME_BG);
      screen->setTextFont(fonts[OPENSANS_COND_BOLD_20]);
      screen->setTextDatum(MC_DATUM);
      char buff[20];
      sprintf(buff, "APP:%d", curApp & (~(GUI_BASE_APP)));
      screen->drawString(buff, screen->width()/2, screen->height()/2);

    }
  }

  // Step 2: redraw header or footer into the screen sprite

  /* 🛑 Never while a game runs: the emulator's blit task is on the LCD's SPI bus from the
   * other core, and a header pushed under it from this task deadlocked both (phone 2,
   * 2026-09-21 — the mute hold's icon refresh; loopTask stopped, task watchdog). A REDRAW_SCREEN
   * still reaches GbcApp::redrawScreen for its own pause menu, which paints the LCD itself
   * with the blit task parked; the header and footer are simply not on screen in a game. */
  if (gGbcActive) {
    redrawHeader = redrawFooter = false;
  }

  if (!hfDrawn) {
    if (redrawHeader) {
      //log_d("REDRAW header");
//      if (callApp != NULL) {
//        if (callApp->isWindowed() && ((WindowedApp*) callApp)->getTitle())
//          this->header->setTitle(((WindowedApp*) callApp)->getTitle());
//      } else if (runningApp != NULL) {
//        if (runningApp->isWindowed() && ((WindowedApp*) runningApp)->getTitle())
//          this->header->setTitle(((WindowedApp*) runningApp)->getTitle());
//      }
      ((GUIWidget*) this->header)->redraw(*screen);
    }
    if (redrawFooter) {
      //log_d("REDRAW footer");
//      if (callApp != NULL) {
//        if (callApp->isWindowed() && (((WindowedApp*) callApp)->getLeftButton() || ((WindowedApp*) callApp)->getRightButton()))
//          this->footer->setButtons(((WindowedApp*) callApp)->getLeftButton(), ((WindowedApp*) callApp)->getRightButton());
//      } else if (runningApp != NULL) {
//        if (runningApp->isWindowed() && (((WindowedApp*) runningApp)->getLeftButton() || ((WindowedApp*) runningApp)->getRightButton()))
//          this->footer->setButtons(((WindowedApp*) runningApp)->getLeftButton(), ((WindowedApp*) runningApp)->getRightButton());
//      }
      ((GUIWidget*) this->footer)->redraw(*screen);
    }
  }

  // Step 3: push relevant parts of screen (if using sprite)

  if (state.screenBrightness != 0 || state.screenWakeUp) {      // Optimization: do not push the screen out if the screen is sleeping

    bool bandOnly = false;
    if (redrawScreen) {
      //log_d("REDRAW pushing");
      // In case screen was redrawn, we just push the entire screen for simplicity (cutting away header and footer doesn't save much time; but TODO: possible optimization)
      if (callApp != NULL) {
        callApp->pushScreen();
      } else if (appScreen->isSprite()) {
        /* The optimisation the TODO above describes, opted into per app (drewInsideBand):
         * the map scrolling under a held arrow repaints ~7 frames a second and the header
         * and footer rows are a fifth of every push. Measured 2026-09-20: ~60 ms a push. */
        if (runningApp != NULL && appScreen == screen && !state.locked && runningApp->isWindowed() &&
            runningApp->drewInsideBand() && header && footer) {
          const int top = ((GUIWidget*)header)->getParentOffY() + ((GUIWidget*)header)->height();
          const int bottom = ((GUIWidget*)footer)->getParentOffY();
          if (top >= 0 && bottom > top && bottom <= (int)screen->height()) {
            this->pushScreenPart(screen, (uint16_t)top, (uint16_t)(bottom - top));
            bandOnly = true;
          }
        }
        if (!bandOnly) {
          this->pushScreen(appScreen);
        }
      }
    }
    // When appScreen is a sprite and entire screen was pushed out above, then no need to draw header and footer parts separately
    if (!redrawScreen || !appScreen->isSprite() || bandOnly) {
      if (redrawHeader) {
        //log_d("REDRAW pushing header");
        this->pushScreenPart(screen, ((GUIWidget*)header)->getParentOffY(), ((GUIWidget*)header)->height());
      }
      if (redrawFooter) {
        //log_d("REDRAW pushing footer");
        this->pushScreenPart(screen, ((GUIWidget*)footer)->getParentOffY(), ((GUIWidget*)footer)->height());
      }
    }
    if (state.screenWakeUp) {
      state.screenWakeUp = false;
      // Maximize screen brightness according to the settings
#if GPIO_EXTENDER == 1509
      lcdLedOnOff(true, conv100to255(state.brightLevel));
#endif // GPIO_EXTENDER == 1509
      state.screenBrightness = state.brightLevel;
    }

  }
};

bool GUI::marqueeTick(uint32_t /*now*/) {
  /* Asleep, locked or powering off: nobody can see the row, and a repaint would only keep
   * the CPU up (every redraw stamps gUiWorkMs, which the frequency gate reads). */
  if (state.screenBrightness == 0 || state.locked || powerOffScreen) {
    return false;
  }
  /* ⚠ millis(), NOT the loop's `now` (review, 2026-09-19). Ownership is stamped with
   * millis() inside a paint, and the SMS-mirror path paints BEFORE this tick in the same
   * pass: a text landing on the Messages list with a long row selected rebuilt the list,
   * stamped it ~40 ms AFTER the pass's `now`, and the unsigned `now - phaseMs` wrapped to
   * "the hold is long over" — one skipped hold and a second full push in the same pass.
   * The same stale-`now` shape the buzz timing fix removed the same day. */
  return MenuWidget::marqueeTick(millis());
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  MENU HELPERS  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

int16_t GUI::findMenu(uint16_t ID) {
  int16_t r;
  for (r=0; r<sizeof(menu)/sizeof(GUIMenuItem); r++) {
    if (menu[r].ID == ID) {
      return r;
    }
  }
  return -1;
};

int16_t GUI::findMenuIcons(uint16_t ID) {
  int16_t r;
  for (r=0; r<sizeof(menuIcons)/sizeof(GUIMenuItemIcons); r++) {
    if (menuIcons[r].ID == ID) {
      return r;
    }
  }
  return -1;
};

int16_t GUI::findSubMenu(uint16_t ID, uint16_t sel) {
  int16_t r, c = 0;
  for (r=0; r<sizeof(menu)/sizeof(GUIMenuItem); r++) {
    if (menu[r].parent == ID) {
      if (sel==c) {
        return r;
      }
      c++;
    }
  }
  return -1;
};

/* Description:
 *      set "automatic" menu variables
 */
void GUI::enterMenu(uint16_t ID) {
  log_d("entering menu = ", ID);

  curMenuId = ID;
  if (ID & GUI_BASE_APP) {
    return;
  }

  if (mainMenu) {
    delete mainMenu;
    mainMenu = NULL;
  };

  int16_t menuIndex = findMenu(ID);
  if (menuIndex<0) {
    guiError("enterMenu failed");
    return;
  }

  // Create and populate new widget
  int i, j;
  MenuOptionIconned* option;
  mainMenu = new MenuWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(),
                            "**EMPTY**", fonts[AKROBAT_EXTRABOLD_22], N_MENU_ITEMS, 8, false);
  mainMenu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, NONE, BLACK, WHITE);    // note: regular background color ignored
  for(i=0; i<sizeof(menu)/sizeof(GUIMenuItem); i++) {
    // A title-less row is a zero-filled tail entry (see the size warning on menu[]), not a
    // menu item: adding it would hand a NULL to MenuOptionIconned.
    if (menu[i].parent == menu[menuIndex].ID && menu[i].title && menu[i].title[0]) {
      j = findMenuIcons(menu[i].ID);
      /* ⭐ The WiFi row reads its own state. MenuOption strdup()s the title, so a stack buffer
       * is enough and the label is a snapshot taken each time the menu is built — which is why
       * the toggle below rebuilds the menu rather than trying to poke the widget. Showing the
       * state ON the row is the whole point: the old WIFI-ON/WIFI-OFF control was real but
       * lived inside a network's EDIT form, several screens down, and told you nothing until
       * you got there. ⚠ This is keyed on the ACTION, not on which menu is being built, so it
       * follows the row wherever it is homed (it lives under Settings now). */
      char wifiRowTitle[32];
      char wifiRowSub[40];
      const char* rowTitle = menu[i].title;
      const char* rowSub = NULL;
      if (menu[i].action == GUI_ACTION_WIFI_TOGGLE) {
        snprintf(wifiRowTitle, sizeof(wifiRowTitle), "WiFi: %s", wifiOn ? "on" : "off");
        /* ⚠ THE SUBTITLE IS THE ANSWER TO THE ORIGINAL "NOT PERSISTED" OBJECTION — see
         * Networks::setRadioOff(). A switch that survives a reboot has to say so where a
         * person will actually read it, or it becomes the setting you forget you set. */
        strlcpy(wifiRowSub, wifiOn ? "scanning and joining as usual"
                                   : "saves power - survives restarts", sizeof(wifiRowSub));
        rowTitle = wifiRowTitle;
        rowSub = wifiRowSub;
      } else if (menu[i].action == GUI_ACTION_T9_TOGGLE) {
        // Same trick, same buffer: say the state on the row rather than behind it.
        snprintf(wifiRowTitle, sizeof(wifiRowTitle), "Predictive text: %s",
                 state.t9Enabled ? "on" : "off");
        rowTitle = wifiRowTitle;
      }
      if (j<0) {
        option = new MenuOptionIconned(menu[i].ID, 1, rowTitle, rowSub);
      } else {
        option = new MenuOptionIconned(menu[i].ID, 1, rowTitle, rowSub, menuIcons[j].icon1, menuIcons[j].iconSize1, menuIcons[j].icon2, menuIcons[j].iconSize2);
      }
      if (option && !mainMenu->addOption(option)) {
        delete option;
        break;
      }
    }
  }

  // Set the automatic values
  menuNewItems = true;
  menuDrawn = false;        // wee need this to update header and footer; TODO: too strong? (optimize with menuNewItems)

  // TODO: remove
  curMenuSel = 0;
  curMenuSize = 0;
  curMenuOffset = 0;

  // Calculate size of the menu
  for (i=0; i<sizeof(menu)/sizeof(GUIMenuItem); i++) {
    if (menu[i].parent == menu[menuIndex].ID && menu[i].title && menu[i].title[0]) {
      curMenuSize++;
    }
  }

  // Update header and footer text
  header->setTitle(menu[menuIndex].title);
  footer->setButtons(menu[menuIndex].leftButton, menu[menuIndex].rightButton);
};

void GUI::exitMenu() {
  int16_t mi = findMenu(curMenuId);
  log_d("exiting menu: %d", curMenuId);
  if (mi<0) {
    guiError("menu failed");
    return;
  }
  if (menu[mi].parent>0) {          // if it is a descendant of Main Menu
    enterMenu(menu[mi].parent);     // switch to parent menu      TODO: get parameters from stack  (exitMenu)
  }
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  APP HELPERS  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

/* Description:
 *     create call app as callee
 */
void GUI::becomeCallee() {
  if (callApp==NULL) {
    // Entering call
    // TODO: preserve header/footer
    callApp = new CallApp(this->audio, this->lcd, this->state, false, this->header, this->footer);     // false for callee
  }
}

void GUI::exitCall() {
  if (callApp!=NULL) {
    delete callApp;
    callApp = NULL;
  }
}

/* ── IS A CALL GENUINELY LIVE OR RINGING? ─────────────────────────────────────────────────
 *
 * 🛑 THIS ANSWERED "YES" FOR TEARDOWN, AND IT SILENTLY DISABLED THE SCREEN LOCK. Its one
 * caller is the unlock path (processEvent), which reads it as "a call is coming in, so let any
 * key answer it" and clears `locked` on the spot. The old test was `not NotInited/Idle/Error`,
 * which is true for HangUp, HangingUp, HungUp and Decline as well — none of which is a call.
 *
 * Two consequences, both reproduced on both phones 2026-08-25:
 *   1. **One press of END unlocks a locked phone.** END sets HangUp from anywhere with no call
 *      needed (WiPhone.ino, in the key handler) and it does so BEFORE the same keypress reaches
 *      this check — so the press that makes inCall() true is the press that gets the free
 *      unlock. END is the red key, and a bag presses it.
 *   2. **While SIP is parked in HangUp/HangingUp, EVERY key unlocks.** Those states STICK when
 *      the proxy is unreachable — this repo has measured 19 minutes (2026-08-15) and SIX HOURS
 *      (2026-08-20) at sip=6. A phone with a SIP account therefore spends real time with no
 *      working lock at all, which is exactly what {user} saw: *"whiphone two doesn't like its
 *      screen to lock ever… I just don't want to be accidentally pushing buttons while it is
 *      in a bag."* The phone WAS locking; the first button press was throwing it away.
 *
 * ⚠ THIS IS THE THIRD GUARD IN THIS CODEBASE TO GET THE SAME CORRECTION, and the other two say
 * so in their own comments: `sipNeedsFullSpeed()` ("includes HangUp/HangingUp: teardown, and
 * they stick") and the WiFi auto-switch gate ("HangUp and HangingUp are deliberately NOT
 * blocking states"). The set below is deliberately the SAME set those two use — a fourth
 * spelling of "in a call" is how they drift apart.
 *
 * ⚠ ANSWERING A RINGING PHONE STILL WORKS ON ANY KEY: BeingInvited is in the set, and that is
 * the case the shortcut existed for. What is no longer in it is hanging up, having hung up,
 * and declining.
 */
bool GUI::inCall() {
  switch (state.sipState) {
  case CallState::InvitingCallee:
  case CallState::InvitedCallee:
  case CallState::RemoteRinging:
  case CallState::Call:
  case CallState::BeingInvited:
  case CallState::Accept:
    return true;               // a live or imminent audio session
  default:
    return false;              // idle, error, and every teardown state
  }
}

/* ── WHICH APP FRAGMENTS THE INTERNAL HEAP? ───────────────────────────────────────────
 * Measured 2026-08-15: `largest` is DEAD FLAT while the screen is off — 12,156 bytes for
 * 78 minutes, then 7,380 for 36 minutes — and takes a permanent step down on every
 * screen-on episode, never recovering. It reached 3,084 and the phone pa{user}ed
 * (reset_reason=4) at 157 minutes, with wifi=3 the whole run, so this is a second cause
 * entirely separate from the WiFi handler leak that was fixed earlier.
 *
 * Every app is `new`d here, and this build has NO malloc->PSRAM diversion
 * (CONFIG_SPIRAM_USE_CAPS_ALLOC without CONFIG_SPIRAM_USE_MALLOC), so every app object and
 * every widget it owns is INTERNAL heap. Rather than guess across thirty app classes, this
 * makes the phone name the one that costs contiguous space.
 *
 * The number that matters is the LAST one: if `largest` does not come back after the app is
 * torn down again, that app fragmented the heap rather than merely using it.
 *
 * Writes to health.log as well as serial, so it is readable over WiFi without a cable.
 * One line per app switch — infrequent, and the log now keeps its newest 32 KB. */
extern void healthLogLine(const char* line);

static void appHeapProbe(int id, uint32_t heapBefore, uint32_t largestBefore) {
  const uint32_t heapAfter = ESP.getFreeHeap();
  const uint32_t largestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  char line[150];
  snprintf(line, sizeof(line),
           "APP id=%d heap %lu->%lu largest %lu->%lu (%+ld)",
           id,
           (unsigned long)heapBefore, (unsigned long)heapAfter,
           (unsigned long)largestBefore, (unsigned long)largestAfter,
           (long)largestAfter - (long)largestBefore);
  log_e("%s", line);
  healthLogLine(line);
}

bool GUI::openAppFromConsole(ActionID_t app) {
  /* A call owns the screen: enterApp() would delete the CallApp and leave the call headless
   * (cleanAppDynamic deletes callApp). The menu can never get here during a call because
   * processEvent feeds callApp first; the console must refuse the same way. ⚠ `callApp` is
   * only the CALLEE's; the caller's CallApp lives inside DialingApp/PhonebookApp, so the SIP
   * state is the test that covers both sides (review, 2026-09-19). */
  switch (state.sipState) {
  case CallState::InvitingCallee:
  case CallState::InvitedCallee:
  case CallState::RemoteRinging:
  case CallState::Call:
  case CallState::BeingInvited:
  case CallState::Accept:
    return false;
  default:
    break;
  }
  if (callApp != NULL) {
    return false;
  }
  /* The emulator is not torn down by enterApp(): `open clock` with a game running entered
   * the clock underneath it and the `audio` dump kept saying gbc=1 (phone 2, 2026-09-19).
   * Refuse; the game's own pause menu (END) is the way out. */
  if (gGbcActive) {
    return false;
  }
  enterApp(app);
  redrawScreen(true, true, true);
  return true;
}

void GUI::enterApp(ActionID_t app) {
  log_d("entering app");

  if (app == GUI_APP_DIALING || app == GUI_APP_PHONEBOOK) {
    missedCallsClear();          // you went where calling back happens: the nag is done
  }

  /* Spans cleanAppDynamic() as well as the construction below, so the number reported is
   * the NET cost of switching to this app — which is the figure that actually predicts the
   * crash. */
  const uint32_t probeHeap0 = ESP.getFreeHeap();
  const uint32_t probeLargest0 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  // Free dynamic memory
  cleanAppDynamic();
  flash.end();

  // Change to new app
  switch(app) {
  case GUI_APP_MYAPP:
    runningApp = new MyApp(audio, *screen, state, header, footer);
    break;
  case GUI_APP_OTA:
    runningApp = new OtaApp(*screen, state, header, footer);
    break;
  case GUI_APP_MENU:
    state.setInputState(InputType::Numeric);
    menuDrawn = false;
    break;
  case GUI_APP_CLOCK:
    runningApp = new ClockApp(*screen, *bgImage, state);
    break;
  case GUI_APP_DIALING:
    // NOTE: passing physical screen `lcd` for creating the CallApp recursively
    runningApp = new DialingApp(audio, *screen, lcd, state, header, footer);
    break;
  case GUI_APP_PHONEBOOK:
    // NOTE: passing physical screen `lcd` for creating the CallApp recursively
    runningApp = new PhonebookApp(audio, *screen, lcd, state, flash, header, footer);
    break;
  case GUI_APP_MESSAGES:
    runningApp = new MessagesApp(*screen, state, flash, header, footer);
    break;
  case GUI_APP_MESHTASTIC:
    runningApp = new MeshtasticApp(*screen, state, header, footer);
    break;
  case GUI_APP_BOOKS:
    runningApp = new BooksApp(*screen, state, header, footer);
    break;
  case GUI_APP_FILES:
    runningApp = new FilesApp(*screen, state, header, footer);
    break;
  case GUI_APP_PHOTOS:
    runningApp = new PhotosApp(*screen, state, header, footer);
    break;
  case GUI_APP_MAPS:
    runningApp = new MapsApp(*screen, state, header, footer);
    break;
  case GUI_APP_ALMANAC:
    runningApp = new AlmanacApp(*screen, state, header, footer);
    break;
  case GUI_APP_AI:
    runningApp = new AiApp(*screen, state, header, footer);
    break;
  case GUI_APP_MUSIC:
    runningApp = new MusicApp(*screen, state, header, footer);
    break;
  case GUI_APP_SIP_ACCOUNTS:
    runningApp = new SipAccountsApp(*screen, state, flash, header, footer);
    break;
  case GUI_APP_NOTEPAD:
    runningApp = new NotepadApp(*screen, state, flash, header, footer);
    break;
  case GUI_APP_CIRCLES:
    runningApp = new CircleApp(*screen, state);       // TODO: process throw
    break;
  case GUI_APP_DIGITAL_RAIN:
    runningApp = new DigitalRainApp(lcd, state);
    break;
  case GUI_APP_GBC:
    runningApp = new GbcApp(lcd, state);
    break;
  case GUI_APP_UART_PASS:
    runningApp = new UartPassthroughApp(lcd, state, header, footer);
    break;
  case GUI_APP_WIDGETS:
    runningApp = new WidgetDemoApp(lcd, state);
    break;
  case GUI_APP_PICS_DEMO:
    runningApp = new PicturesDemoApp(*screen, state);
    break;
  case GUI_APP_FONT_DEMO:
    runningApp = new FontDemoApp(lcd, state);
    break;
  case GUI_APP_DESIGN_DEMO:
    runningApp = new DesignDemoApp(lcd, state);
    break;
  case GUI_APP_MIC_TEST:
    // NOTE: `lcd` - this app draws to the physical screen directly
    runningApp = new MicTestApp(audio, lcd, state, header, footer);
    break;
  case GUI_APP_RECORDER:
    // NOTE: `lcd` - this app draws to the physical screen directly
    runningApp = new RecorderApp(audio, lcd, state, header, footer);
    break;
  case GUI_APP_DIAGNOSTICS:
    // NOTE: `lcd` - this app draws to the physical screen directly
    //runningApp = new DiagnosticsApp(audio, lcd, state, header, footer);
    runningApp = new DiagnosticsApp(audio, lcd, state);
    break;
#ifdef BUILD_GAMES
  case GUI_APP_FIDE_CHESS:
    runningApp = new ChessApp(audio, *screen, state, header, footer);
    break;
  case GUI_APP_CHESS960:
    runningApp = new ChessApp(audio, *screen, state, header, footer, ChessApp::Chess960);
    break;
  case GUI_APP_HILL_CHESS:
    runningApp = new ChessApp(audio, *screen, state, header, footer, ChessApp::KingOfTheHill);
    break;
  case GUI_APP_ACKMAN:
    // NOTE: `lcd` - this app draws to the physical screen directly
    runningApp = new AckmanApp(audio, lcd, state);
    break;
#endif
#ifdef LED_BOARD
  case GUI_APP_LED_MIC:
    runningApp = new LedMicApp(audio, *screen, state, header, footer);
    break;
#endif
  case GUI_APP_WIFI_AUTOSWITCH:
    runningApp = new WifiAutoSwitchApp(*screen, state, header, footer);
    break;
  case GUI_APP_MUTE:
    runningApp = new MuteApp(audio, *screen, state, header, footer);
    break;
  case GUI_APP_EDITWIFI:
    runningApp = new EditNetworkApp(*screen, state, NULL, header, footer);
    break;
  case GUI_APP_TIME_CONFIG:
    runningApp = new TimeConfigApp(*screen, state, header, footer);
    break;
  case GUI_APP_SCREEN_CONFIG:
    runningApp = new ScreenConfigApp(*screen, state, header, footer);
    break;
  case GUI_APP_NETWORKS:
    runningApp = new NetworksApp(*screen, state, header, footer);
    break;
  case GUI_APP_UDP:
    runningApp = new UdpSenderApp(*screen, state, flash, header, footer);
    break;
  case GUI_APP_AUDIO_CONFIG:
    runningApp = new AudioConfigApp(audio, *screen, state, header, footer);
    break;
  case GUI_APP_NOTIFY_CONFIG:
    runningApp = new NotifyConfigApp(audio, *screen, state, header, footer);
    break;
  case GUI_APP_PARCEL:
    runningApp = new ParcelApp(*screen, state, flash, header, footer);
    break;
#ifdef MOTOR_DRIVER
  case GUI_APP_MOTOR:
    runningApp = new MotorDriverApp(*screen, state, header, footer);
    break;
#endif
  case GUI_APP_PIN_CONTROL:
    runningApp = new PinControlApp(*screen, state, header, footer);
    break;
  case GUI_APP_SPLASH:
    runningApp = new SplashApp(*screen, state);
    break;
  default:
    break;
  }

  // Set current app
  curApp = app;
  log_d("entered app: %d", curApp);

  appHeapProbe((int)app, probeHeap0, probeLargest0);
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  APPEARANCE HELPERS  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

void GUI::guiError(const char* s) {
  log_e("guiError: %s", s);
  lcd.setTextColor(WHITE, RED);
  lcd.setTextFont(2);
  lcd.setTextSize(1);
  lcd.setTextDatum(TL_DATUM);
  lcd.drawString(s, 10, 10);
  delay(5000);
};

void GUI::circle(uint16_t x, uint16_t y, uint16_t r, uint16_t col) {
  lcd.fillCircle(x, y, r, col);
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  APP CLASSES  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  WiPhone app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

WiPhoneApp::WiPhoneApp(LCD& lcd, ControlState& state) : lcd(lcd), controlState(state), registeredWidgets(0) {
  anyEventLastStack = controlState.msAppTimerEventLast;
  anyEventPeriodStack = controlState.msAppTimerEventPeriod;

  //controlState.msAppTimerEventPeriod = 0;    // unsubscribe from periodic "anyevent"
};

WiPhoneApp::~WiPhoneApp() {
  log_v("destroy WiPhoneApp");
  controlState.msAppTimerEventLast = anyEventLastStack;
  controlState.msAppTimerEventPeriod = anyEventPeriodStack;

  int cnt = 0;
  for (size_t i=0; i<registeredWidgets.size(); i++) {
    if (registeredWidgets[i]!=NULL) {
      delete registeredWidgets[i];
      registeredWidgets[i] = NULL;
      cnt++;
    }
  }
  if (cnt) {
    log_d("WIDGETS DELETED: %d", cnt);
  }
}

void WiPhoneApp::registerWidget(GUIWidget* w) {
  registeredWidgets.add(w);
}

void WiPhoneApp::pushScreen() {
  if (!pushed && lcd.isSprite()) {
    ((TFT_eSprite&)lcd).pushSprite(0, 0);
    pushed = true;
  }
};

void WiPhoneApp::addLabelInput(uint16_t &yOff, LabelWidget*& label, TextInputWidget*& input, const char* labelText, const uint32_t inputSize, InputType inputType) {
  label = new LabelWidget(0, yOff, lcd.width(), 25, labelText, WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += label->height();
  input = new TextInputWidget(0, yOff, lcd.width(), 35, controlState, inputSize, fonts[AKROBAT_BOLD_20], inputType, 8);
  yOff += input->height();
}

void WiPhoneApp::addLabelPassword(uint16_t &yOff, LabelWidget*& label, PasswordInputWidget*& input, const char* labelText, const uint32_t inputSize, InputType inputType) {
  label = new LabelWidget(0, yOff, lcd.width(), 25, labelText, WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += label->height();
  input = new PasswordInputWidget(0, yOff, lcd.width(), 35, controlState, inputSize, fonts[AKROBAT_BOLD_20], inputType, 8);
  yOff += input->height();
}

void WiPhoneApp::addInlineLabelInput(uint16_t &yOff, uint16_t labelWidth, LabelWidget*& label, TextInputWidget*& input, const char* labelText, const uint32_t inputSize, InputType inputType) {
  label = new LabelWidget(0, yOff, labelWidth, 25, labelText, WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::RIGHT_TO_LEFT, 8);
  input = new TextInputWidget(labelWidth, yOff, lcd.width()-labelWidth, 25, controlState, inputSize, fonts[AKROBAT_BOLD_18], inputType, 3);
  yOff += input->height();
}

void WiPhoneApp::addDoubleLabelInput(uint16_t &yOff, LabelWidget*& label1, TextInputWidget*& input1, const char* labelText1, const uint32_t inputSize1,
                                     LabelWidget*& label2, TextInputWidget*& input2, const char* labelText2, const uint32_t inputSize2, InputType inputType) {
  const auto padX = 2;
  // First set
  label1 = new LabelWidget(0, yOff, lcd.width()/2-padX, 25, labelText1, WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  label2 = new LabelWidget(lcd.width()/2+padX, yOff, lcd.width() - (lcd.width()/2-padX), 25, labelText2, WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += label1->height();
  input1 = new TextInputWidget(0, yOff, lcd.width()/2-2, 35, controlState, inputSize1, fonts[AKROBAT_BOLD_20], inputType, 8);
  input2 = new TextInputWidget(lcd.width()/2+padX, yOff, lcd.width() - (lcd.width()/2-padX), 35, controlState, inputSize2, fonts[AKROBAT_BOLD_20], inputType, 8);
  yOff += input1->height();
}

void WiPhoneApp::addLabelSlider(uint16_t &yOff, LabelWidget*& label, IntegerSliderWidget*& input, const char* labelText, int minVal, int maxVal, const char* unit, int steps) {
  log_d("adding a label and a slider");
  label = new LabelWidget(0, yOff, lcd.width(), 25, labelText, WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += label->height();
  input = new IntegerSliderWidget(0, yOff, lcd.width(), 25, minVal, maxVal, (maxVal-minVal)/steps, true, unit);
  yOff += input->height();
}

void WiPhoneApp::addInlineLabelSlider(uint16_t &yOff, uint16_t labelWidth, LabelWidget*& label, IntegerSliderWidget*& input, const char* labelText,
                                      int minVal, int maxVal, const char* unit, int steps) {
  label = new LabelWidget(0, yOff, labelWidth, 25, labelText, WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::RIGHT_TO_LEFT, 8);
  input = new IntegerSliderWidget(labelWidth, yOff, lcd.width() - labelWidth, 25, minVal, maxVal, (maxVal-minVal)/steps, true, unit);
  yOff += input->height();
}

void WiPhoneApp::addInlineLabelYesNo(uint16_t &yOff, uint16_t labelWidth, LabelWidget*& label, YesNoWidget*& input, const char* labelText) {
  label = new LabelWidget(0, yOff, labelWidth, 25, labelText, WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::RIGHT_TO_LEFT, 8);
  input = new YesNoWidget(labelWidth, yOff, lcd.width()-labelWidth, 25, fonts[AKROBAT_BOLD_18]);
  yOff += input->height();
}

void WiPhoneApp::addRuler(uint16_t &yOff, RulerWidget*& ruler, uint16_t addOffset) {
  ruler = new RulerWidget(5, yOff+addOffset, lcd.width()-10);
  yOff += ruler->height() + 2*addOffset;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Focusable app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void FocusableApp::addFocusableWidget(FocusableWidget* w) {
  if (w==nullptr || !focusableWidgets.add(w)) {
    log_e("registering focusable FAILED");
  }
}

void FocusableApp::nextFocus(bool forward) {
  log_v("nextFocus FocusableApp");
  int i;
  for (i=0; i<focusableWidgets.size(); i++) {
    if (focusableWidgets[i]->getFocus()) {
      break;
    }
  }
  if (i<focusableWidgets.size()) {
    focusableWidgets[i]->setFocus(false);
    // Find next widget to focus at
    for (int c=0; c<focusableWidgets.size(); c++) {
      if (forward) {
        if (++i>=focusableWidgets.size()) {
          i = 0;
        }
      } else {
        if (--i<0) {
          i = focusableWidgets.size()-1;
        }
      }
      if (focusableWidgets[i]->getActive()) {
        break;
      }
    }
    if (focusableWidgets[i]->getActive()) {
      focusableWidgets[i]->setFocus(true);
    }
  }
}

FocusableWidget* FocusableApp::getFocused() {
  log_v("getFocused FocusableApp");
  for (int i=0; i<focusableWidgets.size(); i++) {
    if (focusableWidgets[i]->getFocus()) {
      return focusableWidgets[i];
    }
  }
  return nullptr;
}

void FocusableApp::setFocus(FocusableWidget* w) {
  log_v("setFocus FocusableApp");
  for (int i=0; i<focusableWidgets.size(); i++) {
    focusableWidgets[i]->setFocus((focusableWidgets[i] == w) ? true : false);
  }
}

void FocusableApp::deactivateFocusable() {
  log_v("deactivateFocusable FocusableApp");
  for (int i=0; i<focusableWidgets.size(); i++) {
    focusableWidgets[i]->deactivate();
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Windowed app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

WindowedApp::WindowedApp(LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WiPhoneApp(lcd, state), header(header), footer(footer) {
//  this->title = NULL;
//  this->leftButtonName = NULL;
//  this->rightButtonName = NULL;
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Theaded app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ThreadedApp::ThreadedApp(LCD& lcd, ControlState& state)
  : WiPhoneApp(lcd, state) {
  xHandle = NULL;
  // Example:
  //xTaskCreate(&ThreadedApp::thread, "digitalrain", 8192, this, tskIDLE_PRIORITY, &xHandle);
}

ThreadedApp::~ThreadedApp() {
  if (xHandle != NULL) {
    log_d("deleting task");
    vTaskDelete(xHandle);
    xHandle = NULL;
  }
}

appEventResult ThreadedApp::processEvent(EventType event) {
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }
  // Take event and put into queue
  // TODO
  return DO_NOTHING;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Ota app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/* The page. '#' at the start of a line marks a heading; it is not printed.
 *
 * Every command here was checked against this repo before being written down: the README's
 * flashing section and platformio.ini. Note there is NO --upload-speed option on `pio run`
 * (checked with `pio run --help`) — a slow adapter needs platformio.ini edited, so that is
 * what the text says. Anyone reading this is standing at a computer with a broken phone; do
 * not make them debug an invented flag. */
const char* const OtaApp::helpText[] = {
  "#The easy way: a browser",
  "",
  "On a computer, open Chrome",
  "or Edge and go to:",
  "",
  "  nikguy321.github.io/",
  "    wiphone-meshtastic",
  "",
  "Plug the phone in with a USB",
  "cable, click Install, pick",
  "the port that appears, and",
  "wait about a minute. Nothing",
  "to install on the computer.",
  "",
  "Everything below is the",
  "long way, for when a browser",
  "is not an option.",
  "",
  "#Updates over the air: OFF",
  "",
  "This build cannot install",
  "updates over the air, and it",
  "never could. The secure",
  "download needs about 33 KB of",
  "free internal memory; the",
  "phone has about 19 KB in",
  "total. The connection can",
  "never open. It is a memory",
  "limit, not a broken link.",
  "",
  "#The long way, over USB",
  "",
  "A computer, a USB cable, and",
  "Python 3 installed. About",
  "five minutes.",
  "",
  "#1. Install PlatformIO",
  "",
  "  pip3 install -U platformio",
  "",
  "#2. Get the firmware",
  "",
  "On the computer, open:",
  "",
  "  github.com/Nikguy321/",
  "    wiphone-meshtastic",
  "",
  "Use the green Code button to",
  "download or clone it, then",
  "open a terminal in that",
  "folder.",
  "",
  "#3. Plug the phone in",
  "",
  "Connect it by USB. Windows",
  "and Linux may need the",
  "Silicon Labs CP210x driver.",
  "macOS already has it.",
  "",
  "#4. Flash it",
  "",
  "  pio run -t upload",
  "",
  "That builds and installs the",
  "firmware. The phone reboots",
  "on its own when it is done.",
  "",
  "#5. First time only",
  "",
  "On a phone that has never had",
  "this firmware, also send the",
  "support files (config files,",
  "the ringtone, the wallpaper):",
  "",
  "  pio run -t uploadfs",
  "",
  "#If the upload fails",
  "",
  "Some USB adapters cannot hold",
  "full speed. Open platformio",
  ".ini, change upload_speed to",
  "230400, and try again.",
  "",
  "If no port is found, run",
  "pio device list and add",
  "--upload-port <your-port>.",
  "",
  "#More",
  "",
  "The README on the GitHub page",
  "above has the long version,",
  "and CHANGELOG.md lists what",
  "changed in each release.",
  "",
};
const int OtaApp::helpLines = sizeof(OtaApp::helpText) / sizeof(OtaApp::helpText[0]);

OtaApp::OtaApp(LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer) :
  WindowedApp(lcd, state, header, footer) {
  log_d("OtaApp constructor");

  /* ⚠ SET WHAT THIS SCREEN NEEDS, do not inherit it. msAppTimerEventPeriod is device-wide and
   * apps routinely leave their own value in it (the Game Boy leaves 250 ms, others 25 ms). A
   * page of static text needs no timer at all, and inheriting someone else's would wake the
   * CPU for nothing — this is the shared-state bug class that produced four faults in one day. */
  controlState.msAppTimerEventPeriod = 0;

  header->setTitle("Firmware Update");
  footer->setButtons("", "Back");

  bodyTop = header->height();
  bodyHeight = lcd.height() - header->height() - footer->height();

  const int lh = fonts[AKROBAT_BOLD_16]->height();
  visibleLines = lh > 0 ? bodyHeight / lh : 1;
  if (visibleLines < 1) {
    visibleLines = 1;
  }
}

OtaApp::~OtaApp() {
  // Nothing to free: the page is string literals in flash and the rest is plain ints.
}

appEventResult OtaApp::processEvent(EventType event) {
  const int maxFirst = (helpLines > visibleLines) ? (helpLines - visibleLines) : 0;
  const int before = firstLine;

  if (LOGIC_BUTTON_BACK(event)) {
    // No text field on this screen, so Back is free to mean Back (it is backspace elsewhere).
    return EXIT_APP;
  } else if (event == WIPHONE_KEY_DOWN) {
    firstLine++;
  } else if (event == WIPHONE_KEY_UP) {
    firstLine--;
  } else if (event == WIPHONE_KEY_RIGHT) {
    firstLine += visibleLines;          // page down
  } else if (event == WIPHONE_KEY_LEFT) {
    firstLine -= visibleLines;          // page up
  } else {
    return DO_NOTHING;
  }

  if (firstLine > maxFirst) {
    firstLine = maxFirst;
  }
  if (firstLine < 0) {
    firstLine = 0;
  }

  return firstLine != before ? REDRAW_SCREEN : DO_NOTHING;
}

void OtaApp::redrawScreen(bool redrawAll) {
  SmoothFont* font = fonts[AKROBAT_BOLD_16];
  const int lh = font->height();
  const uint16_t sideMargin = 8;
  const uint16_t barWidth = 3;

  lcd.fillRect(0, bodyTop, lcd.width(), bodyHeight, WP_COLOR_1);

  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);

  for (int row = 0; row < visibleLines; row++) {
    const int i = firstLine + row;
    if (i >= helpLines) {
      break;
    }
    const char* line = helpText[i];

    // A leading '#' marks a heading: drawn in the accent colour, marker not printed.
    colorType color = WP_COLOR_0;
    if (line[0] == '#') {
      color = WP_ACCENT_1;
      line++;
    }
    if (!line[0]) {
      continue;     // blank spacer line: the fillRect above already cleared it
    }

    lcd.setTextColor(color, WP_COLOR_1);
    lcd.drawString(line, sideMargin, bodyTop + row * lh);
  }

  // Scrollbar, so it is obvious there is more below. Rectangles only — no glyph needed.
  if (helpLines > visibleLines) {
    const uint16_t x = lcd.width() - barWidth - 2;
    const int maxFirst = helpLines - visibleLines;

    uint16_t thumb = (uint16_t)((uint32_t)bodyHeight * visibleLines / helpLines);
    if (thumb < 10) {
      thumb = 10;
    }
    const uint16_t y = bodyTop + (uint16_t)((uint32_t)(bodyHeight - thumb) * firstLine / maxFirst);

    lcd.fillRect(x, bodyTop, barWidth, bodyHeight, WP_DISAB_1);
    lcd.fillRect(x, y, barWidth, thumb, WP_ACCENT_1);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  MyApp demo app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

MyApp::MyApp(Audio* audio, LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(2), audio(audio) {
  log_d("MyApp create");
  const char* s;

  // Create and arrange widgets
  header->setTitle("MyApp Demo");
  footer->setButtons("Yes", "No");
  clearRect = new RectWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), WP_COLOR_1);

  // State caption in the middle
  const uint16_t spacing = 4;
  uint16_t yOff = header->height() + 26;
  demoCaption = new LabelWidget(0, yOff, lcd.width(), fonts[AKROBAT_BOLD_20]->height(),
                                "Hello World", WP_ACCENT_S, WP_COLOR_1, fonts[AKROBAT_BOLD_20], LabelWidget::CENTER);
  yOff += demoCaption->height() + (spacing*2);

  // Make an icon
  iconRect = new RectIconWidget((lcd.width()-50)>>1, yOff, 50, 50, WP_ACCENT_1, icon_person_w, sizeof(icon_person_w));
  yOff += iconRect->height() + (spacing*2);

  /*// Name and URI above
  s = controlState.calleeNameDyn!=NULL ? (const char*) controlState.calleeNameDyn : "";
  nameCaption =  new LabelWidget(0, yOff, lcd.width(), fonts[AKROBAT_EXTRABOLD_22]->height(), s, WP_COLOR_0, WP_COLOR_1, fonts[AKROBAT_EXTRABOLD_22], LabelWidget::CENTER);
  yOff += nameCaption->height() + spacing;
  s = controlState.calleeUriDyn!=NULL ? (const char*) controlState.calleeUriDyn : "";
  uriCaption =  new LabelWidget(0, yOff, lcd.width(), fonts[AKROBAT_BOLD_20]->height(), s, WP_DISAB_0, WP_COLOR_1, fonts[AKROBAT_BOLD_20], LabelWidget::CENTER);*/

  // Debug string: shows some simple debug info
  //yOff += uriCaption->height() + 20;
  //s = controlState.lastReasonDyn != NULL ? (const char*) controlState.lastReasonDyn : "";
  s = "I'm Awesome!";
  debugCaption = new LabelWidget(0, yOff, lcd.width(), fonts[AKROBAT_BOLD_16]->height(), s, WP_DISAB_0, WP_COLOR_1, fonts[AKROBAT_BOLD_16], LabelWidget::CENTER);

  //reasonHash = hash_murmur(controlState.lastReasonDyn);
}

MyApp::~MyApp() {
  log_d("destroy MyApp");

  delete demoCaption;
  delete debugCaption;
  //sdelete debugCaption_loudSpkr;
  //delete nameCaption;
  //delete uriCaption;
}

appEventResult MyApp::processEvent(EventType event) {
  log_d("processEvent MyApp");
  appEventResult res = DO_NOTHING;

  if (LOGIC_BUTTON_BACK(event)) {

    demoCaption->setText("Back Button");
    res |= REDRAW_SCREEN;

  } else if (LOGIC_BUTTON_OK(event)) {

    demoCaption->setText("OK Button");
    footer->setButtons("OH", "NO");
    res |= REDRAW_SCREEN | REDRAW_FOOTER;

  } else if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN) {

    int8_t earpieceVol, headphonesVol, loudspeakerVol;
    audio->getVolumes(earpieceVol, headphonesVol, loudspeakerVol);
    int8_t d = event == WIPHONE_KEY_UP ? 6 : -6;
    earpieceVol += d;
    headphonesVol += d;
    loudspeakerVol += d;
    audio->setVolumes(earpieceVol, headphonesVol, loudspeakerVol);
    audio->getVolumes(earpieceVol, headphonesVol, loudspeakerVol);
    char buff[70];
    snprintf(buff, sizeof(buff), "Speaker %d dB, Headphones %d dB, Loudspeaker %d dB",  earpieceVol, headphonesVol, loudspeakerVol);
    debugCaption->setText(buff);
    
  }

  return res;
}

void MyApp::redrawScreen(bool redrawAll) {
  log_d("redrawScreen MyApp");

  if (!screenInited || redrawAll) {
    log_d("redraw all");
    // Initialize screen
    ((GUIWidget*) clearRect)->redraw(lcd);

    ((GUIWidget*) iconRect)->redraw(lcd);
    ((GUIWidget*) demoCaption)->redraw(lcd);
    ((GUIWidget*) debugCaption)->redraw(lcd);
    //((GUIWidget*) nameCaption)->redraw(lcd);
    //((GUIWidget*) uriCaption)->redraw(lcd);

    //lcd.fillRect(95, 240, 50, 20, WP_ACCENT_1);     // DEBUG: very strange bug with white pixels over black border

  } else {

    // Refresh only updated labels
    if (demoCaption->isUpdated()) {
      log_d("stateCaption updated");
      ((GUIWidget*) demoCaption)->redraw(lcd);
    }
    if (debugCaption->isUpdated()) {
      log_d("debugCaption updated");
      ((GUIWidget*) debugCaption)->redraw(lcd);
    }
    /*if (nameCaption->isUpdated()) {
      log_d("nameCaption updated");
      ((GUIWidget*) nameCaption)->redraw(lcd);
    }
    if (uriCaption->isUpdated()) {
      log_d("uriCaption updated");
      ((GUIWidget*) uriCaption)->redraw(lcd);
    }*/
  }
  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  UART passthrough app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -
UartPassthroughApp::UartPassthroughApp(LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(2), screenInited(false), startedSerial(false),
    xHandle0(NULL), xHandle1(NULL) {
  clearRect = new RectWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), WP_COLOR_1);

  controlState.msAppTimerEventLast = millis();
  controlState.msAppTimerEventPeriod = 500;

  // State caption in the middle
  const uint16_t spacing = 4;
  uint16_t yOff = header->height() + 26;

  baudLabel = new LabelWidget(0, yOff, lcd.width(), 25, "Baud:", WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += baudLabel->height();

  baud = new TextInputWidget(0, yOff, lcd.width(), 35, controlState, 100, fonts[AKROBAT_BOLD_20], InputType::AlphaNum, 8);
  yOff += baud->height();

  echoLabel = new LabelWidget(0, yOff, 100, 25, "Echo:", WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);

  echo = new ChoiceWidget(105, yOff, lcd.width()-110, 35);
  echo->addChoice("Yes");
  echo->addChoice("No");
  echo->setValue(1);
  yOff += echo->height();

  startStop = new ButtonWidget(0, yOff, "Start");

  addFocusableWidget(baud);
  addFocusableWidget(echo);
  addFocusableWidget(startStop);

  setFocus(baud);
}

UartPassthroughApp::~UartPassthroughApp() {
  delete baudLabel;
  delete baud;
  delete startStop;
  delete echo;
  delete echoLabel;

  if (xHandle0 != NULL) {
    vTaskDelete(xHandle0);
    xHandle0 = NULL;
  }

  if (xHandle1 != NULL) {
    vTaskDelete(xHandle1);
    xHandle1 = NULL;
  }

  if (startedSerial) {
    uart_driver_delete(UART_NUM_0);
    uart_driver_delete(UART_NUM_1);

    startedSerial = false;
  }

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
}

void UartPassthroughApp::thread(void *pvParam) {
  uartThreadParams *params = (uartThreadParams*)pvParam;

  /* ⚠ CHECKED. Unchecked, a failed allocation handed NULL straight to uart_read_bytes()
   * below, so the fault landed there rather than here — one more panic attributed to the
   * wrong line. PSRAM: this is a 1 KB streaming buffer with no DMA or ISR involvement, and
   * internal heap is the one that kills this phone. */
  uint8_t* data = (uint8_t*) extMalloc(1024);
  if (!data) {
    log_e("UART passthrough: no memory for the 1 KB buffer - thread not started");
    vTaskDelete(NULL);
    return;
  }
  while (1) {
    const int rxBytes = uart_read_bytes(params->rxPort, data, 1024, 1000 / 300);
    if (rxBytes > 0) {
      /* 🛑 `data[rxBytes] = 0;` USED TO BE HERE and wrote data[1024] on a full read — one
       * byte into the next heap block's header. Silent; the abort arrives later, somewhere
       * else, looking exactly like a fragmentation death. The terminator was never needed:
       * the write below sends exactly rxBytes and never treats `data` as a C string. */
      uart_write_bytes(params->txPort, (const char*)data, rxBytes);
    }
  }
  /* ⚠ UNREACHABLE, and known to be: the loop is while(1) and the task is killed from
   * outside by vTaskDelete. That is why every Start/Stop cycle used to lose the buffer
   * permanently (twice over, with Echo=No). It is PSRAM now, so a cycle costs PSRAM rather
   * than the internal heap — but the real fix is an exit handshake so the task frees its
   * own buffer and calls vTaskDelete(NULL). Left for a change that can be tested with a
   * peer on the daughterboard UART. */
  free(data);
}

void UartPassthroughApp::redrawScreen(bool redrawAll) {
  // Initialize screen
  if (redrawAll || !screenInited) {
    ((GUIWidget*) clearRect)->redraw(lcd);

    ((GUIWidget*) baudLabel)->redraw(lcd);
    ((GUIWidget*) baud)->redraw(lcd);
    ((GUIWidget*) startStop)->redraw(lcd);
    ((GUIWidget*) echo)->redraw(lcd);
    ((GUIWidget*) echoLabel)->redraw(lcd);
  } else {
    if (baud->isUpdated()) {
      ((GUIWidget*) baud)->redraw(lcd);
    }
    if (startStop->isUpdated()) {
      ((GUIWidget*) startStop)->redraw(lcd);
    }
    if (echo->isUpdated()) {
      ((GUIWidget*) echo)->redraw(lcd);
    }
  }

  screenInited = true;
}

appEventResult UartPassthroughApp::processEvent(EventType event) {
  appEventResult res = DO_NOTHING;
  FocusableWidget* focusedWidget = getFocused();

  if (LOGIC_BUTTON_OK(event) && focusedWidget == startStop) {
    if (!startedSerial) {
      startedSerial = true;
      ((ButtonWidget*) focusedWidget)->setText("stop");

      /* 🛑 EMPTY FIELD = DIVIDE BY ZERO. The baud input starts with no text, so pressing
       * Start without typing gave atoi("") == 0, and uart_set_baudrate() then computes
       * (UART_CLK_FREQ << 4) / 0 — an immediate Guru Meditation on the first OK press. */
      const int baudRate = atoi(baud->getText());
      if (baudRate <= 0) {
        log_e("UART passthrough: baud '%s' is not a usable rate", baud->getText());
        ((ButtonWidget*) focusedWidget)->setText("start");
        startedSerial = false;
        return REDRAW_ALL;
      }
      const uart_config_t uart_config = {
        .baud_rate = baudRate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
      };

      int RX_BUF_SIZE =  1024;

      uart_param_config(UART_NUM_0, &uart_config);
      uart_driver_install(UART_NUM_0, RX_BUF_SIZE * 2, 0, 0, NULL, 0);

      uart_param_config(UART_NUM_1, &uart_config);
      uart_set_pin(UART_NUM_1, USER_SERIAL_TX, USER_SERIAL_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
      uart_driver_install(UART_NUM_1, RX_BUF_SIZE * 2, 0, 0, NULL, 0);

      switch (echo->getValue()) {
      case 1: // no
        uart0Thread.rxPort = UART_NUM_0;
        uart0Thread.txPort = UART_NUM_1;
        uart1Thread.rxPort = UART_NUM_1;
        uart1Thread.txPort = UART_NUM_0;
        xTaskCreate(&UartPassthroughApp::thread, "uart0", 1024, &uart0Thread, tskIDLE_PRIORITY + 1, &xHandle0);
        xTaskCreate(&UartPassthroughApp::thread, "uart1", 1024, &uart1Thread, tskIDLE_PRIORITY + 1, &xHandle1);
        break;
      case 0: // yes
        uart0Thread.rxPort = UART_NUM_0;
        uart0Thread.txPort = UART_NUM_0;
        xTaskCreate(&UartPassthroughApp::thread, "uart0", 1024, &uart0Thread, tskIDLE_PRIORITY + 1, &xHandle0);
        break;
      }
    } else {
      startedSerial = false;
      ((ButtonWidget*) focusedWidget)->setText("start");

      if (xHandle0 != NULL) {
        vTaskDelete(xHandle0);
        xHandle0 = NULL;
      }

      if (xHandle1 != NULL) {
        vTaskDelete(xHandle1);
        xHandle1 = NULL;
      }
    }

    res |= REDRAW_SCREEN;
  } else if (event == WIPHONE_KEY_END) {
    return EXIT_APP;
  } else if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN) {
    nextFocus(event == WIPHONE_KEY_DOWN);
    res |= REDRAW_SCREEN;
  } else {
    if (focusedWidget != NULL) {
      ((GUIWidget*) focusedWidget)->processEvent(event);
      res |= REDRAW_SCREEN;
    }
  }

  return res;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Digital Rain app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -
DigitalRainApp::DigitalRainApp(LCD& lcd, ControlState& state)
  : ThreadedApp(lcd, state), sprite(&lcd) {
  log_d("DigitalRainApp::DigitalRainApp");
  BaseType_t xStatus = xTaskCreate(&DigitalRainApp::thread, "digitalrain", 4096, this, tskIDLE_PRIORITY + 1, &xHandle);

  if (xStatus != pdPASS) {
    //vTaskDelete(xHandle);
    //xHandle = NULL;
    log_e("xTaskCreate returned (%d)", (int32_t)xStatus);
    return;
  }

  log_d("DigitalRainApp created task");

  sprite.setColorDepth(16);
  sprite.createSprite(6, 8);
  if (!sprite.isCreated()) {
    log_d("error: char sprite not created");
  }
  sprite.setTextDatum(TL_DATUM);
  sprite.setTextSize(1);
  sprite.setTextFont(1);
}

DigitalRainApp::~DigitalRainApp() {
  /* ⚠ TFT_eSprite HAS NO DESTRUCTOR anywhere in the vendored library — the pixel buffer is
   * released only by an explicit deleteSprite(). Without this the buffer was stranded on
   * every open/exit. The small sprites are the stability problem, not the big one:
   * esp32Calloc() tries plain calloc() first, so anything that fits comes from the INTERNAL
   * heap and is left mid-heap, splitting the free space `largest` measures. deleteSprite()
   * early-returns when nothing was created, so this is unconditionally safe. */
  sprite.deleteSprite();
}

void DigitalRainApp::thread(void *pvParam) {
  DigitalRainApp* parent = (DigitalRainApp*) pvParam;
  parent->clear();
  uint32_t cnt = 0;
  while (1) {
    parent->draw();
    if (cnt++ % 25 == 0) {
      log_d("thread");
    }
    vTaskDelay(30);
  }
}

char DigitalRainApp::randPrintable() {
  char chr = Random.random() & 0xFF;
  if (!chr || chr == ' ' || chr == 255) {
    chr = ((Random.random() & 0x7F) | 0x80) - 1;
  }
  return chr;
}

void DigitalRainApp::clear() {
  // Clear screen
  lcd.fillScreen(TFT_BLACK);

  // Initialize "digitalrain"
  for (auto j = 0; j < 39; j++)
    for (auto i = 0; i < 40; i++) {
      text[j][i] = this->randPrintable();
      brightness[j][i] = Random.random() % 65;
    }
}

void DigitalRainApp::drawMirroredChar(char c, uint16_t x, uint16_t y, colorType color) {
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextColor(color, TFT_BLACK);
  sprite.drawChar(c, 0, 0);
  if (c < 128) {
    sprite.mirror();
  }
  sprite.pushSprite(x, y);
}

void DigitalRainApp::draw() {

  // Create new "drops" from the top
  int x = Random.random() % 49;
  if (x < 40) {
    brightness[0][x] = 64;
  }

  // Redraw all characters moving brightness down (rain)
  const uint8_t decay = 4;
  for (int j = 38; j >= 0; j--) {
    for (int i = 0; i < 40; i++) {

      // Change brightness
      bool draw;
      if (j > 0 && brightness[j-1][i] >= 64) {
        draw = true;
        brightness[j][i] = 64;
      } else {
        draw = brightness[j][i] > 0;
        brightness[j][i] = (brightness[j][i] > decay) ? brightness[j][i] - decay : 0;
      }

      // "Corrupt" digital rain
      if (Random.random() % 100 <= 5) {
        // Clean current character from screen
        if (draw) {
          this->drawMirroredChar(text[j][i], i*6, j*8, TFT_BLACK);
        }

        // Assign new character
        text[j][i] = this->randPrintable();
      }

      // Draw character
      if (draw) {
        colorType color = (brightness[j][i] < 64) ? brightness[j][i] << 5 : TFT_WHITE;
        this->drawMirroredChar(text[j][i], i*6, j*8, color);
      }

    }
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Notepad app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

NotepadApp::NotepadApp(LCD& lcd, ControlState& state, Storage& flashParam, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), flash(flashParam) {
  log_i("create NotepadApp");

  header->setTitle("Note Page");
  footer->setButtons("Save", "Clear");

  const int16_t padding =  4;
  textArea = new MultilineTextWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(),
                                     "Empty page", state, NotepadApp::maxNotepadSize, fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum, padding, padding);
  textArea->setColors(WP_COLOR_0, WP_COLOR_1);

  // Get notepad string dynamically
  const char* appNoteStringDyn = NULL;
  flash.loadString(cNotepadFlashPage, "note", appNoteStringDyn);
  if (appNoteStringDyn && *appNoteStringDyn) {
    // Set widget text
    textArea->setText(appNoteStringDyn);
    freeNull((void **) &appNoteStringDyn);
  }

  textArea->setFocus(true);     // to reveal the cursor
  /* A note page is prose, so it predicts — the one other place besides a message body where
   * you are writing sentences rather than an address or a key. AFTER setFocus(), which goes
   * through setInputState() and clears the flag. */
  state.t9Field = true;
}

NotepadApp::~NotepadApp() {
  log_i("destroy NotepadApp");
  delete textArea;
}

appEventResult NotepadApp::processEvent(EventType event) {
  log_i("processEvent NotepadApp: %d", (int)event);
  if (LOGIC_BUTTON_OK(event)) {
    // Save notepad state
    const char* str = textArea->getText();
    if (str!=NULL) {
      log_v("saving note: ", str);
      flash.storeString(cNotepadFlashPage, "note", str);
    }

    return EXIT_APP;
  }
  if (event == WIPHONE_KEY_END) {
    // TODO: confirm dialog box
    return EXIT_APP;
  }
  textArea->processEvent(event);
  return REDRAW_SCREEN;
}

void NotepadApp::redrawScreen(bool redrawAll) {
  log_i("redraw NotepadApp");
  ((GUIWidget*) textArea)->redraw(lcd);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Dialing app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

DialingApp::DialingApp(Audio* audio, LCD& disp, LCD& hardDisp, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(disp, state, header, footer), FocusableApp(1), audio(audio), hardDisp(hardDisp) {
  log_d("create DialingApp");

  header->setTitle("Dialing");
  footer->setButtons("Call", "Clear");

  uint16_t yOff = header->height();
  this->errorLabel = new LabelWidget(0, yOff, lcd.width(), 65, "", TFT_RED, WP_COLOR_0, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += this->errorLabel->height();

  const int16_t xPad =  5;
  const int16_t yPad =  3;
  textArea = new MultilineTextWidget(0, yOff, lcd.width(), lcd.height() - footer->height() - yOff,
                                     NULL, state, 70, fonts[AKROBAT_BOLD_32], InputType::Numeric, xPad, yPad);
  textArea->verticalCentering(true);
  textArea->setColors(WP_COLOR_1, WP_COLOR_0);

  // Focusables
  this->addFocusableWidget(textArea);
  this->setFocus(textArea);   // to reveal the cursor
}

DialingApp::~DialingApp() {
  log_d("destroy DialingApp");
  if (callApp != NULL) {
    delete callApp;
  }
  delete textArea;
}

appEventResult DialingApp::processEvent(EventType event) {
  log_d("processEvent DialingApp: %d", event);
  if (this->callApp != NULL) {
    appEventResult res = this->callApp->processEvent(event);
    if (res & EXIT_APP) {
      delete callApp;
      callApp = NULL;
      screenInited = false;
      header->setTitle("Dialing");
      footer->setButtons("Call", "Clear");
      return REDRAW_ALL;
    }
    return res;
  } else if (LOGIC_BUTTON_OK(event)) {
    if (textArea->getText()[0] == '\0' && controlState.lastDialed[0] != '\0') {
      /* OK on an EMPTY dialer = redial prefill. The number appears for review; the next
       * OK places the call. Two presses on purpose — a one-press instant redial would
       * make pocket OKs call people. */
      textArea->setText(controlState.lastDialed);
      return REDRAW_SCREEN;
    }
    if (controlState.isCallPossible()) {
      // Make a call
      log_d("CALLING %s", textArea->getText());

      snprintf(controlState.lastDialed, sizeof(controlState.lastDialed), "%s",
               textArea->getText());
      controlState.setRemoteNameUri("Dialed number", textArea->getText());
      controlState.setSipReason("");
      controlState.setSipState(CallState::InvitingCallee);

      //this->deactivateFocusable();

      // Create call app as caller
      callApp = new CallApp(this->audio, this->hardDisp, this->controlState, true, this->header, this->footer);       // true for caller

      return REDRAW_ALL;
    } else {
      this->error = true;
      this->errorLabel->setText("Not connected to SIP server");
    }
  } else if (event == WIPHONE_KEY_END) {
    return EXIT_APP;
  }
  if (event == '*') {
    event = '+';  // temporary solution to allow + input
  }
  textArea->processEvent(event);
  return REDRAW_SCREEN;
}

void DialingApp::redrawScreen(bool redrawAll) {
  log_i("redraw DialingApp");
  if (this->callApp != NULL) {
    this->callApp->redrawScreen(redrawAll);
    return;
  }
  if (!this->screenInited || this->error || redrawAll) {
    ((GUIWidget*) errorLabel)->redraw(lcd);
    this->errorLabel->setText("");
    this->error = false;
  }
  ((GUIWidget*) textArea)->redraw(lcd);
  this->screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  UDP sender app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

UdpSenderApp::UdpSenderApp(LCD& lcd, ControlState& state, Storage& pref, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(14), flash(pref) {
  log_d("create UdpSenderApp");

  udp = new WiFiUDP();
  udp->begin(UDP_CLIENT_PORT);

  uint16_t yOff = header->height();
  bgRect = new RectWidget(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(), WP_COLOR_1);

  // Create and arrange widgets
  addLabelInput(yOff, labels[0], inputs[0], "Destination IP:", 16);
  addLabelInput(yOff, labels[1], inputs[1], "Port:", 6);
  addLabelInput(yOff, labels[2], inputs[2], "Text:", 100);
  yOff += 9;
  sendButton = new ButtonWidget(2, yOff, "Send");

  options = NULL;
  memset(shortcutLabels, 0, sizeof(shortcutLabels));
  memset(shortcutInputs, 0, sizeof(shortcutInputs));

  // Load preferences
  const char* ip = NULL;
  const char* text = NULL;
  int32_t port = -1;
  flash.loadUdpSender(ip, port, text);
  if (ip) {
    log_d("Loaded ip:   %s", ip);
  }
  if (port>=0) {
    log_d("Loaded port: %d", port);
  }
  if (text) {
    log_d("Loaded text: %s", text);
  }

  // Set text
  if (ip) {
    inputs[0]->setText(ip);
  }
  if (port>=0) {
    char buff[12];
    buff[0] = '\0';
    if (port>=0) {
      sprintf(buff, "%d", port);
    }
    inputs[1]->setText(buff);
  }
  if (text) {
    inputs[2]->setText(text);
  }

  // Free memory
  freeNull((void **) &ip);
  freeNull((void **) &text);

  // Set focusables
  for(uint16_t i=0; i<sizeof(inputs)/sizeof(TextInputWidget*); i++) {
    addFocusableWidget(inputs[i]);
  }
  addFocusableWidget(sendButton);

  // Start with main screen
  changeState(MAIN);

}

UdpSenderApp::~UdpSenderApp() {
  log_d("destroy UdpSenderApp");

  // Save preferences
  const char* ip   = inputs[0]->getText();
  const char* port = inputs[1]->getText();
  const char* text = inputs[2]->getText();
  int32_t portVal;
  if (port) {
    errno = 0;
    portVal = strtol(port, NULL, 10);
    if (errno) {
      portVal = -1;
    } else {
      portVal = (uint16_t) portVal;
    }
  } else {
    portVal = -1;
  }
  flash.storeUdpSender(ip, portVal, text);

  // Delete dynamic
  udp->stop();
  delete udp;
  delete bgRect;
  delete sendButton;
  for(uint16_t i=0; i<sizeof(labels)/sizeof(LabelWidget*); i++) {
    delete labels[i];
  }
  for(uint16_t i=0; i<sizeof(inputs)/sizeof(TextInputWidget*); i++) {
    delete inputs[i];
  }

  if (options) {
    delete options;
  }
  for(uint16_t i=0; i<sizeof(shortcutLabels)/sizeof(LabelWidget*); i++)
    if (shortcutLabels[i]) {
      delete shortcutLabels[i];
    }
  for(uint16_t i=0; i<sizeof(shortcutInputs)/sizeof(TextInputWidget*); i++)
    if (shortcutInputs[i]) {
      delete shortcutInputs[i];
    }
}

void UdpSenderApp::changeState(UdpSenderState_t newState) {
  if (newState == MAIN) {

    deactivateFocusable();
    for (int k=0; k<3; k++) {
      inputs[k]->activate();
    }
    sendButton->activate();
    setFocus(inputs[0]);

    header->setTitle("UDP sender");
    footer->setButtons("Options", "Clear");

  } else if (newState == OPTIONS) {

    // Create widget
    if (!options) {
      options = new OptionsMenuWidget(0, header->height(), lcd.width(), lcd.height()-header->height()-footer->height());
      options->addOption("Shortcuts");
      addFocusableWidget(options);
    }

    // Deactivate / Activate
    deactivateFocusable();
    options->activate();
    setFocus(options);

    // Change settings
    header->setTitle("Options");
    footer->setButtons(NULL, "Back");

  } else if (newState == SHORTCUTS) {

    // Create widgets
    if (!shortcutLabels[0]) {
      // Create widgets for shortcuts screen
      uint16_t yOff = header->height()+2;
      char buff[3];
      buff[1] = ':';
      buff[2] = '\0';
      for (int k=0; k<9; k++) {
        buff[0] = '1' + k;
        addInlineLabelInput(yOff, 30, shortcutLabels[k], shortcutInputs[k], buff, 50);
        addFocusableWidget(shortcutInputs[k]);
        yOff += 2;
      }
    }

    // Deactivate / Activate
    deactivateFocusable();
    for (int k=0; k<9; k++) {
      shortcutInputs[k]->activate();
    }
    setFocus(shortcutInputs[0]);

    // Change settings
    header->setTitle("Shortcuts");
    footer->setButtons("Back", "Clear");

  }

  screenInited = false;
  appState = newState;
}

appEventResult UdpSenderApp::processEvent(EventType event) {
  log_d("processEvent UdpSenderApp: ", event);
  appEventResult res = REDRAW_SCREEN;

  // TODO: clean up this code (group by state)
  if ((event == WIPHONE_KEY_END && appState != SHORTCUTS) || (appState == OPTIONS && event == WIPHONE_KEY_BACK)) {

    if (appState == MAIN) {
      return EXIT_APP;
    } else if (appState == OPTIONS) {
      changeState(MAIN);
      res |= REDRAW_ALL;
    }

  } else if (appState == SHORTCUTS && (event == WIPHONE_KEY_END || event == WIPHONE_KEY_OK || event == WIPHONE_KEY_SELECT)) {

    changeState(OPTIONS);
    res |= REDRAW_ALL;

  } else if (appState == MAIN && event == WIPHONE_KEY_SELECT) {

    // Enter options menu
    changeState(OPTIONS);
    res |= REDRAW_ALL;

  } else if (appState == MAIN && (event == WIPHONE_KEY_CALL || (getFocused() == sendButton && (LOGIC_BUTTON_OK(event) || (event>='1' && event<='9'))) )) {

    // Send UDP packet

    // What text to send?
    const char* text = NULL;
    if (event>='1' && event<='9') {
      if (shortcutInputs[event-'1']) {
        text = shortcutInputs[event-'1']->getText();
      }
    } else {
      text = inputs[2]->getText();
    }

    if (text && strlen(text)) {

      // Really send UDP packet

      IPAddress ipAddr;
      ipAddr.fromString(inputs[0]->getText());
      const char* portString = inputs[1]->getText();
      uint16_t port = atoi(portString);

      if ((uint32_t)ipAddr != 0) {
        udp->beginPacket(ipAddr, port);
        udp->write((const byte*) text, strlen(text));
        udp->endPacket();

        log_d("UDP sent: %s", text);
      }

    } else {

      log_d("No text to send");

    }

    return DO_NOTHING;

  } else if (appState == OPTIONS && LOGIC_BUTTON_OK(event)) {

    // Enter shortcuts screen
    changeState(SHORTCUTS);
    res |= REDRAW_ALL;

  } else if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {

    nextFocus(event == WIPHONE_KEY_DOWN);
    if (getFocused()==sendButton) {
      controlState.setInputState(InputType::Numeric);  // TODO: automate changing InputType relevantly
    } else {
      controlState.setInputState(InputType::AlphaNum);
    }

  } else {

    // Pass event to a focused widget
    ((GUIWidget*) getFocused())->processEvent(event);

  }
  return res;
}

void UdpSenderApp::redrawScreen(bool redrawAll) {
  log_i("redraw UdpSenderApp");
  if (appState == MAIN) {

    if (!screenInited || redrawAll) {
      ((GUIWidget*) bgRect)->redraw(lcd);
      // Draw labels
      for(uint16_t i=0; i<sizeof(labels)/sizeof(LabelWidget*); i++) {
        ((GUIWidget*) labels[i])->redraw(lcd);
      }
    }

    // Redraw input widgets
    for(uint16_t i=0; i<sizeof(inputs)/sizeof(TextInputWidget*); i++) {
      ((GUIWidget*) inputs[i])->redraw(lcd);
    }

    ((GUIWidget*) sendButton)->redraw(lcd);

  } else if (appState == OPTIONS) {

    if (options) {
      ((GUIWidget*) options)->redraw(lcd);
    }

  } else if (appState == SHORTCUTS) {

    if (!screenInited || redrawAll) {
      ((GUIWidget*) bgRect)->redraw(lcd);
      // Draw labels
      for(uint16_t i=0; i<sizeof(shortcutLabels)/sizeof(LabelWidget*); i++) {
        ((GUIWidget*) shortcutLabels[i])->redraw(lcd);
      }
    }

    // Redraw input widgets
    for(uint16_t i=0; i<sizeof(shortcutInputs)/sizeof(TextInputWidget*); i++) {
      ((GUIWidget*) shortcutInputs[i])->redraw(lcd);
    }

  }
  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Audio config app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

AudioConfigApp::AudioConfigApp(Audio* audio, LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(2), audio(audio), ini(Storage::ConfigsFile) {
  log_d("create AudioConfigApp");

  header->setTitle("Audio settings");
  footer->setButtons("Save", "Back");

  // Create and arrange widgets
  uint16_t yOff = header->height() + 5;
  addLabelSlider(yOff, labels[2], sliders[2], "Loudspeaker volume:", Audio::MuteVolume, Audio::MaxLoudspeakerVolume, "dB");
  yOff += 4;
  addLabelSlider(yOff, labels[1], sliders[1], "Headphones volume:", Audio::MuteVolume, Audio::MaxVolume, "dB");
  yOff += 4;
  addLabelSlider(yOff, labels[0], sliders[0], "Ear speaker volume:", Audio::MuteVolume, Audio::MaxVolume, "dB");
  yOff += 6;

  /* ⚠ The Ringer row MOVED to Settings > Notifications, where it sits beside the same
   * choice for texts and mesh messages. It reads and writes the same stored `ringer_mode`,
   * so nothing was lost — but two screens editing one value is how they drift apart, so
   * this one no longer draws it. */

  // Load preferences
  /* A level the call screen stepped and has not written yet goes into the file FIRST (review
   * R3-1): it is newer than the file this screen is about to read, and a Save here must be the
   * newer write, not one a later write of the older step lands on top of. One store, only when a
   * step is waiting; this screen's own load is the same kind of SPIFFS wait. */
  callLevelsFlushNow("Settings > Audio");

  /* ⚠ THE SEED IS THE CALL LEVELS, NOT WHATEVER THE CODEC HOLDS. While music plays the codec
   * carries the MUSIC level and the call levels sit in the player's stash; seeding from the
   * codec put the music level on these sliders wherever configs.ini lacked a key or would not
   * load — and the branch below then wrote it to the file as the call volume. */
  int8_t earpieceVol, headphonesVol, loudspeakerVol;
  if (!musicPlayerCallVolumes(earpieceVol, headphonesVol, loudspeakerVol)) {
    audio->getVolumes(earpieceVol, headphonesVol, loudspeakerVol);
  }
  if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
    // Check version of the file format
    // if (ini[0].hasKey("v")){ //&& !strcmp(ini[0]["v"], "1")) {
    //   log_d("configs file found");
    //   IF_LOG(VERBOSE)
    //   ini.show();
      if (ini.hasSection("audio")) {
        log_d("getting audio info");
        earpieceVol = ini["audio"].getIntValueSafe(earpieceVolField, earpieceVol);
        headphonesVol = ini["audio"].getIntValueSafe(headphonesVolField, headphonesVol);
        loudspeakerVol = ini["audio"].getIntValueSafe(loudspeakerVolField, loudspeakerVol);
        controlState.ringerMode = (uint8_t) ini["audio"].getIntValueSafe(ringerModeField, controlState.ringerMode);
      }
    //}
     else {
      log_e("configs file corrup or unknown format");
      IF_LOG(VERBOSE)
      ini.show();
    }
  } else {
    /* 🛑 NOTHING IS WRITTEN HERE — the same rule 8b93e72 gave the in-call volume handler. This
     * branch used to build a three-key [audio] and store() it as the WHOLE of configs.ini
     * whenever the file and its NVS backup both failed to load: every other setting in it
     * (notify modes, buzz, mute, predictive text) gone, just for OPENING the screen. The
     * sliders are seeded above and the object is left empty, so the destructor's backup()
     * writes nothing either (it skips an empty file). Only Save writes — a deliberate press
     * on a screen that loaded nothing, the same as Settings > Notifications' Save, and on a
     * fresh phone with no file yet it is what creates one. */
    log_e("AUDIO SETTINGS: configs.ini did not load - showing the current levels; nothing is "
          "written unless you Save");
  }

  // Set values
  sliders[0]->setValue(earpieceVol);
  sliders[1]->setValue(headphonesVol);
  sliders[2]->setValue(loudspeakerVol);

  // Set focusables
  addFocusableWidget(sliders[2]);
  addFocusableWidget(sliders[1]);
  addFocusableWidget(sliders[0]);

  setFocus(sliders[2]);
}

AudioConfigApp::~AudioConfigApp() {
  log_d("destroy AudioConfigApp");

  ini.backup();

  // Delete dynamic
  for(uint16_t i=0; i<sizeof(labels)/sizeof(LabelWidget*); i++) {
    delete labels[i];
  }
  for(uint16_t i=0; i<sizeof(sliders)/sizeof(IntegerSliderWidget*); i++) {
    delete sliders[i];
  }
}

appEventResult AudioConfigApp::processEvent(EventType event) {
  log_d("processEvent AudioConfigApp: %04x", event);
  appEventResult res = REDRAW_SCREEN;

  if (event == WIPHONE_KEY_SELECT) {
    // Save preferences
    int speakerVol = sliders[0]->getValue();
    int headphonesVol = sliders[1]->getValue();
    int loudspeakerVol = sliders[2]->getValue();
    if (!ini.hasSection("audio")) {
      ini.addSection("audio");
    }
    ini["audio"][earpieceVolField] = speakerVol;
    ini["audio"][headphonesVolField] = headphonesVol;
    ini["audio"][loudspeakerVolField] = loudspeakerVol;
    /* ⚠ ringer_mode is NOT written here any more — Settings > Notifications owns it. This
     * screen still READS it above (harmless, and it keeps the file's shape documented in one
     * place), but two writers of one value is exactly how the two screens would disagree. */
    ini.store();
    /* 🛑 NOT STRAIGHT TO THE CODEC WHILE MUSIC HOLDS IT. That wrote the call levels under the
     * playing track (the music jumped — up to ~24 dB on headphones), and when the music stopped
     * the player put its stale stash back over them, so the saved setting did not take until a
     * reboot. While music holds the codec the new levels go into its stash and are applied when
     * it lets go; otherwise the codec holds the call levels and gets them now. */
    if (!musicPlayerSetCallVolumes(speakerVol, headphonesVol, loudspeakerVol)) {
      audio->setVolumes(speakerVol, headphonesVol, loudspeakerVol);
    }
  }

  // TODO: clean up this code (group by state)
  if (event == WIPHONE_KEY_END || event == WIPHONE_KEY_BACK || event == WIPHONE_KEY_SELECT) {
    return EXIT_APP;
  }

  if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {

    nextFocus(event == WIPHONE_KEY_DOWN);

  } else {

    // Pass event to a focused widget
    ((GUIWidget*) getFocused())->processEvent(event);

  }
  return res;
}

void AudioConfigApp::redrawScreen(bool redrawAll) {
  if (!screenInited) {
    redrawAll = true;
  }
  if (redrawAll) {
    // Background
    lcd.fillRect(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), WP_COLOR_1);
    // Draw labels
    for(uint16_t i=0; i<sizeof(labels)/sizeof(LabelWidget*); i++) {
      ((GUIWidget*) labels[i])->redraw(lcd);
    }
  }

  // Redraw input widgets
  for(uint16_t i=0; i<sizeof(sliders)/sizeof(IntegerSliderWidget*); i++) {
    ((GUIWidget*) sliders[i])->refresh(lcd, redrawAll);
  }

  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Notification config app  - - - - - - - - - - - - - - - - - - - - - - -

/* Settings > Notifications. One row per KIND of arrival, plus the pop's own volume.
 *
 * Why per-kind: muting one thing used to mute another. Texts never announced themselves at
 * all — only mesh messages did — and a text arriving over the COVEY mirror chirped only
 * because it rode the mesh path. Silencing the mirror silenced real texts with it. Splitting
 * the choice three ways is what makes "quiet mesh, audible texts" expressible at all.
 */
NotifyConfigApp::NotifyConfigApp(Audio* audio, LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(5), audio(audio), ini(Storage::ConfigsFile) {
  log_d("create NotifyConfigApp");

  header->setTitle("Notifications");
  footer->setButtons("Save", "Back");

  uint16_t yOff = header->height() + 5;

  /* The same three words on every row, deliberately. A row that said "Ring + vibrate" and
   * another that said "Sound + buzz" would read as two different mechanisms when they are
   * one. "Vibrate only" means the motor still pulses and the tone does not play. */
  struct { const char* text; LabelWidget** lab; ChoiceWidget** ch; } rows[3] = {
    { "Calls:",          &callLabel, &callChoice },
    { "Text messages:",  &sipLabel,  &sipChoice  },
    { "Meshtastic:",     &meshLabel, &meshChoice },
  };
  for (int i = 0; i < 3; i++) {
    *rows[i].lab = new LabelWidget(0, yOff, lcd.width(), 25, rows[i].text, WP_ACCENT_1, WP_COLOR_1,
                                   fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
    yOff += (*rows[i].lab)->height();
    *rows[i].ch = new ChoiceWidget(0, yOff, lcd.width(), 35);
    (*rows[i].ch)->addChoice("Sound + vibrate");
    (*rows[i].ch)->addChoice("Vibrate only");
    (*rows[i].ch)->addChoice("Silent");
    yOff += (*rows[i].ch)->height() + 4;
  }

  /* ⚠ THE SCREEN IS FULL. Header 30 + three label/choice rows at 64 each + one stacked
   * label-and-slider at 50 = 277 of the 280 px above the footer. A fifth stacked row does not
   * fit, so both sliders take the INLINE form the Screen config screen uses — label on the
   * left, slider on the right, one 25 px row each — which lands the whole screen back at 277.
   * Same labelWidth as Screen config so the two settings screens line up. */
  const uint16_t labelWidth = 110;
  addInlineLabelSlider(yOff, labelWidth, volLabel, volSlider, "Volume",
                       Audio::MuteVolume, Audio::MaxVolume, "dB");
  /* 50..650 ms in twelve 50 ms steps. 50 rather than 0: a zero-length buzz would just be a
   * worse spelling of "Silent", which the choice above already says plainly. */
  addInlineLabelSlider(yOff, labelWidth, vibroLabel, vibroSlider, "Buzz", 50, 650, "ms");

  /* Read what is already stored. GUI::loadSettings() has read these at boot too — this is
   * the screen catching up with the live values, not a second source of truth. */
  if ((ini.load() || ini.restore()) && !ini.isEmpty() && ini.hasSection("audio")) {
    controlState.ringerMode     = (uint8_t) ini["audio"].getIntValueSafe(ringerModeField, controlState.ringerMode);
    controlState.notifySipMode  = (uint8_t) ini["audio"].getIntValueSafe(sipModeField, controlState.notifySipMode);
    controlState.notifyMeshMode = (uint8_t) ini["audio"].getIntValueSafe(meshModeField, controlState.notifyMeshMode);
    controlState.notifyVolume   = (int8_t)  ini["audio"].getIntValueSafe(notifyVolField, controlState.notifyVolume);
    controlState.notifyVibroMs  = (uint16_t) constrain(ini["audio"].getIntValueSafe(notifyVibroField, controlState.notifyVibroMs), 50, 650);   // same clamp as boot
  }

  callChoice->setValue(controlState.ringerMode);
  sipChoice->setValue(controlState.notifySipMode);
  meshChoice->setValue(controlState.notifyMeshMode);
  volSlider->setValue(controlState.notifyVolume);
  vibroSlider->setValue(controlState.notifyVibroMs);

  addFocusableWidget(callChoice);
  addFocusableWidget(sipChoice);
  addFocusableWidget(meshChoice);
  addFocusableWidget(volSlider);
  addFocusableWidget(vibroSlider);
  setFocus(callChoice);
}

NotifyConfigApp::~NotifyConfigApp() {
  log_d("destroy NotifyConfigApp");
  ini.backup();
  delete callLabel;
  delete callChoice;
  delete sipLabel;
  delete sipChoice;
  delete meshLabel;
  delete meshChoice;
  delete volLabel;
  delete volSlider;
  delete vibroLabel;
  delete vibroSlider;
}

appEventResult NotifyConfigApp::processEvent(EventType event) {
  log_d("processEvent NotifyConfigApp: %04x", event);
  appEventResult res = REDRAW_SCREEN;

  if (event == WIPHONE_KEY_SELECT) {
    // Save. The live ControlState is updated FIRST so the very next arrival obeys the new
    // setting even if the file write were to fail.
    controlState.ringerMode     = (uint8_t) callChoice->getValue();
    controlState.notifySipMode  = (uint8_t) sipChoice->getValue();
    controlState.notifyMeshMode = (uint8_t) meshChoice->getValue();
    controlState.notifyVolume   = (int8_t)  volSlider->getValue();
    controlState.notifyVibroMs  = (uint16_t) vibroSlider->getValue();

    if (!ini.hasSection("audio")) {
      ini.addSection("audio");
    }
    ini["audio"][ringerModeField] = (int) controlState.ringerMode;
    ini["audio"][sipModeField]    = (int) controlState.notifySipMode;
    ini["audio"][meshModeField]   = (int) controlState.notifyMeshMode;
    ini["audio"][notifyVolField]  = (int) controlState.notifyVolume;
    ini["audio"][notifyVibroField] = (int) controlState.notifyVibroMs;
    ini.store();
  }

  if (event == WIPHONE_KEY_END || event == WIPHONE_KEY_BACK || event == WIPHONE_KEY_SELECT) {
    return EXIT_APP;
  }

  if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {
    nextFocus(event == WIPHONE_KEY_DOWN);
  } else {
    ((GUIWidget*) getFocused())->processEvent(event);
  }
  return res;
}

void NotifyConfigApp::redrawScreen(bool redrawAll) {
  if (!screenInited) {
    redrawAll = true;
  }
  if (redrawAll) {
    lcd.fillRect(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), WP_COLOR_1);
    ((GUIWidget*) callLabel)->redraw(lcd);
    ((GUIWidget*) sipLabel)->redraw(lcd);
    ((GUIWidget*) meshLabel)->redraw(lcd);
    ((GUIWidget*) volLabel)->redraw(lcd);
    ((GUIWidget*) vibroLabel)->redraw(lcd);
  }
  ((GUIWidget*) callChoice)->refresh(lcd, redrawAll);
  ((GUIWidget*) sipChoice)->refresh(lcd, redrawAll);
  ((GUIWidget*) meshChoice)->refresh(lcd, redrawAll);
  ((GUIWidget*) volSlider)->refresh(lcd, redrawAll);
  ((GUIWidget*) vibroSlider)->refresh(lcd, redrawAll);
  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Parcel app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ParcelApp::ParcelApp(LCD& lcd, ControlState& state, Storage& pref, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(14), flash(pref) {
  log_d("create ParcelApp");

  //udp = new WiFiUDP();      // TODO

  uint16_t yOff = header->height();
  bgRect = new RectWidget(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(), WP_COLOR_1);

  // Create and arrange widgets
  log_d("creating widgets");
  addLabelInput(yOff, labels[0], inputs[0], "Name:", 16);
  addLabelInput(yOff, labels[1], inputs[1], "Parcel #:", 6);
  yOff += 9;
  sendButton = new ButtonWidget(2, yOff, "Send");

  log_d("null");
  options = NULL;
  memset(configsLabels, 0, sizeof(configsLabels));
  memset(configsInputs, 0, sizeof(configsInputs));

  // Load preferences
  log_d("loading preferences");
  const char* ip = NULL;
  int32_t port = -1;
  flash.loadString(this->storagePage, "ip", ip);
  if (ip) {
    log_d("Loaded ip");
    log_d("Loaded ip: empty = %s", ip[0] ? "true" : "false");
    log_d("Loaded ip: %s", ip);
  }

  log_d("loading port");
  flash.loadInt(this->storagePage, "port", port);
  if (port>0) {
    log_d("Loaded port: %d", port);
  }

  // Set text
  log_d("setting text");
  if (configsInputs[0] && ip) {
    configsInputs[0]->setText(ip);
  }
  if (configsInputs[1] && port>0) {
    char buff[12];
    buff[0] = '\0';
    if (port>=0) {
      sprintf(buff, "%d", port);
    }
    configsInputs[1]->setText(buff);
  }

  // Free memory
  log_d("freeing");
  freeNull((void **) &ip);

  // Set focusables
  log_d("focusables");
  for(uint16_t i=0; i<sizeof(inputs)/sizeof(TextInputWidget*); i++) {
    addFocusableWidget(inputs[i]);
  }
  addFocusableWidget(sendButton);

  // Start with main screen
  changeState(MAIN);
  log_d("init finished");

  yOff += sendButton->height();
  labels[2] = new LabelWidget(0, yOff, lcd.width(), 25, "Status", WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_16], LabelWidget::LEFT_TO_RIGHT, 8);
  log_d("label added");
}

ParcelApp::~ParcelApp() {
  log_d("destroy ParcelApp");

  /*
    // Save preferences
    const char* ip   = inputs[0]->getText();
    const char* port = inputs[1]->getText();
    const char* text = inputs[2]->getText();
    int32_t portVal;
    if (port) {
      errno = 0;
      portVal = strtol(port, NULL, 10);
      if (errno) portVal = -1;
      else portVal = (uint16_t) portVal;
    } else {
      portVal = -1;
    }
    flash.storeUdpSender(ip, portVal, text);
  */
  // Delete dynamic
  //delete udp;
  delete bgRect;
  delete sendButton;
  for(uint16_t i=0; i<sizeof(labels)/sizeof(LabelWidget*); i++) {
    delete labels[i];
  }
  for(uint16_t i=0; i<sizeof(inputs)/sizeof(TextInputWidget*); i++) {
    delete inputs[i];
  }

  if (options) {
    delete options;
  }
  for(uint16_t i=0; i<sizeof(configsLabels)/sizeof(LabelWidget*); i++)
    if (configsLabels[i]) {
      delete configsLabels[i];
    }
  for(uint16_t i=0; i<sizeof(configsInputs)/sizeof(TextInputWidget*); i++)
    if (configsInputs[i]) {
      delete configsInputs[i];
    }
}

void ParcelApp::changeState(ParcelAppState_t newState) {
  /*
  if (newState == MAIN) {

    deactivateFocusable();
    for (int k=0;k<3;k++)
      inputs[k]->activate();
    sendButton->activate();
    setFocus(inputs[0]);

    header->setTitle("UDP sender");
    footer->setButtons("Options", "Clear");

  } else if (newState == OPTIONS) {

    // Create widget
    if (!options) {
      options = new OptionsMenuWidget(0, header->height(), lcd.width(), lcd.height()-header->height()-footer->height());
      options->addOption("Shortcuts");
      addFocusableWidget(options);
    }

    // Deactivate / Activate
    deactivateFocusable();
    options->activate();
    setFocus(options);

    // Change settings
    header->setTitle("Options");
    footer->setButtons(NULL, "Back");

  } else if (newState == SHORTCUTS) {

    // Create widgets
    if (!shortcutLabels[0]) {
      // Create widgets for shortcuts screen
      uint16_t yOff = header->height()+2;
      char buff[3];
      buff[1] = ':'; buff[2] = '\0';
      for (int k=0;k<9;k++) {
        buff[0] = '1' + k;
        addInlineLabelInput(yOff, 30, shortcutLabels[k], shortcutInputs[k], buff, 50);
        addFocusableWidget(shortcutInputs[k]);
        yOff += 2;
      }
    }

    // Deactivate / Activate
    deactivateFocusable();
    for (int k=0;k<9;k++)
      shortcutInputs[k]->activate();
    setFocus(shortcutInputs[0]);

    // Change settings
    header->setTitle("Shortcuts");
    footer->setButtons(NULL, "Clear");

  }

  screenInited = false;
  appState = newState;
  */
}

appEventResult ParcelApp::processEvent(EventType event) {
  log_d("processEvent ParcelApp: ", event);
  appEventResult res = REDRAW_ALL;

  if (event == WIPHONE_KEY_END || (appState == OPTIONS && event == WIPHONE_KEY_BACK)) {

    if (appState == MAIN) {
      return EXIT_APP;
    }
    /*
        else if (appState == OPTIONS) {
          changeState(MAIN);
          res |= REDRAW_ALL;
        } else if (appState == SHORTCUTS) {
          changeState(OPTIONS);
          res |= REDRAW_ALL;
        }
    */
  } else if (appState == MAIN && event == USER_SERIAL_EVENT) {
    char* str = controlState.userSerialBuffer.getCopy();
    controlState.userSerialBuffer.reset();
    /* getCopy() allocates; nothing guaranteed it succeeded. */
    if (!str) {
      return res;
    }
    log_d("====================================================================");
    log_d("%s", str);
    log_d("====================================================================");
    /* 🛑 free() USED TO BE HERE, ABOVE THE READS. Everything below dereferenced a freed
     * block: ESP-IDF writes its free-list pointers into the first bytes, so str[0..7] was
     * already overwritten by the time strncasecmp() looked at it — and setText() then does
     * strlen() on the freed block and allocates that garbage length from the internal heap.
     * A use-after-free that presents as a random-sized allocation is about the worst
     * possible shape on this device. */
    if (!strncasecmp(str, "NAME:", 5)) {
      inputs[0]->setText(str+5);
    } else {
      inputs[1]->setText(str);
    }
    free(str);
    res |= REDRAW_ALL;
  }
  /*
  else if (appState == MAIN && event == WIPHONE_KEY_SELECT) {

    // Enter options menu
    changeState(OPTIONS);
    res |= REDRAW_ALL;

  } else if (appState == MAIN && (event == WIPHONE_KEY_CALL || getFocused() == sendButton && (LOGIC_BUTTON_OK(event) || (event>='1' && event<='9')))) {

    // Send UDP packet

    // What text to send?
    const char* text = NULL;
    if (event>='1' && event<='9') {
      if (shortcutInputs[event-'1'])
        text = shortcutInputs[event-'1']->getText();
    } else {
      text = inputs[2]->getText();
    }

    if (text && strlen(text)) {

      // Really send UDP packet

      IPAddress ipAddr;
      ipAddr.fromString(inputs[0]->getText());
      const char* portString = inputs[1]->getText();
      uint16_t port = atoi(portString);

      udp->begin(UDP_CLIENT_PORT);
      udp->beginPacket(ipAddr, port);
      udp->write((const byte*) text, strlen(text));
      udp->endPacket();

      log_d("UDP sent: %s", text);

    } else {

      log_d("No text to send");

    }

    return DO_NOTHING;

  } else if (appState == OPTIONS && LOGIC_BUTTON_OK(event)) {

    // Enter shortcuts screen
    changeState(SHORTCUTS);
    res |= REDRAW_ALL;

  } else if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {

    nextFocus(event == WIPHONE_KEY_DOWN);
    if (getFocused()==sendButton) controlState.setInputState(InputType::Numeric);        // TODO: automate changing InputType relevantly
    else controlState.setInputState(InputType::AlphaNum);

  } else {

    // Pass event to a focused widget
    ((GUIWidget*) getFocused())->processEvent(event);

  }
  */
  return res;
}

void ParcelApp::redrawScreen(bool redrawAll) {
  log_i("redraw ParcelApp");
  if (appState == MAIN) {

    if (!screenInited || redrawAll) {
      ((GUIWidget*) bgRect)->redraw(lcd);
      // Draw labels
      for(uint16_t i=0; i<sizeof(labels)/sizeof(LabelWidget*); i++) {
        ((GUIWidget*) labels[i])->redraw(lcd);
      }
    }

    // Redraw input widgets
    for(uint16_t i=0; i<sizeof(inputs)/sizeof(TextInputWidget*); i++) {
      ((GUIWidget*) inputs[i])->redraw(lcd);
    }

    ((GUIWidget*) sendButton)->redraw(lcd);

  } else if (appState == OPTIONS) {

    if (options) {
      ((GUIWidget*) options)->redraw(lcd);
    }

  } else if (appState == CONFIGURE) {

    if (!screenInited || redrawAll) {
      ((GUIWidget*) bgRect)->redraw(lcd);
      // Draw labels
      for(uint16_t i=0; i<sizeof(configsLabels)/sizeof(LabelWidget*); i++) {
        ((GUIWidget*) configsLabels[i])->redraw(lcd);
      }
    }

    // Redraw input widgets
    for(uint16_t i=0; i<sizeof(configsInputs)/sizeof(TextInputWidget*); i++) {
      ((GUIWidget*) configsInputs[i])->redraw(lcd);
    }

  }
  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Motor driver app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -
#ifdef MOTOR_DRIVER

MotorDriverApp::MotorDriverApp(LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer) {
  log_d("create MotorDriverApp");

  header->setTitle("Motor driver");
  footer->setButtons(NULL, "Back");

  udp = new WiFiUDP();
  udp->begin(UDP_SERVER_PORT);
  log_d("UDP server on port %d", UDP_SERVER_PORT);

  // Create text widget
  const uint8_t pad = 4;
  uint16_t yOff = header->height();
  text = new MultilineTextWidget(0, yOff, lcd.width(), 58, "Empty",
                                 state, 300, fonts[AKROBAT_BOLD_20], InputType::AlphaNum, pad, pad);
  text->setColors(WP_ACCENT_1, WP_COLOR_1);

  // Set text
  IPAddress ipAddr = WiFi.localIP();
  char buff[100];
  sprintf(buff, "Send UDP messages to:\n%d.%d.%d.%d:%d", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3], UDP_SERVER_PORT);
  text->setText(buff);

  // Create white background
  yOff += text->height();
  bgRect = new RectWidget(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(), WP_COLOR_1);

  // Create traffic sign widget
  setDirection(NEVER_MOVED);

  // Set periodic event
  controlState.msAppTimerEventLast = millis();
  controlState.msAppTimerEventPeriod = 25;

  digitalWrite(MotorEN , HIGH);
}

MotorDriverApp::~MotorDriverApp() {
  log_d("destroy MotorDriverApp");
  digitalWrite(MotorEN , LOW);

  udp->stop();
  delete udp;

  // Delete widgets
  delete bgRect;
  delete text;
  if (sign) {
    delete sign;
  }
}

void MotorDriverApp::setDirection(Direction newDir) {
  if (sign) {
    delete sign;
  }
  log_d("new direction: %d", (int) newDir);
  if (newDir == NEVER_MOVED) {
    sign = new RectIconWidget(30, header->height() + text->height(), 180, 180, WHITE, icon_no_walking, sizeof(icon_no_walking));
  } else if (newDir == STOP) {
    sign = new RectIconWidget(30, header->height() + text->height(), 180, 180, WHITE, icon_stop, sizeof(icon_stop));
  } else if (newDir == FORWARD) {
    sign = new RectIconWidget(30, header->height() + text->height(), 180, 180, WHITE, icon_forward, sizeof(icon_forward));
  } else if (newDir == REVERSE) {
    sign = new RectIconWidget(30, header->height() + text->height(), 180, 180, WHITE, icon_reverse, sizeof(icon_reverse));
  } else if (newDir == LEFT) {
    sign = new RectIconWidget(30, header->height() + text->height(), 180, 180, WHITE, icon_left, sizeof(icon_left));
  } else if (newDir == RIGHT) {
    sign = new RectIconWidget(30, header->height() + text->height(), 180, 180, WHITE, icon_right, sizeof(icon_right));
  }
  direction = newDir;
}

appEventResult MotorDriverApp::processEvent(EventType event) {
  // Exit keys
  appEventResult res = DO_NOTHING;
  if (event == APP_TIMER_EVENT) {

    Direction newDir = direction;
    if (udpParsePacketSafe(*udp)>0) {
      char buff[100];
      int cb = udp->read(buff, sizeof(buff)-1);
      if (cb>0) {
        buff[cb] = '\0';
        log_d("UDP received: %s", buff);
#if defined(MOTOR_DRIVER) && MOTOR_DRIVER == 8833
        if (buff[0]=='f') {
          motorDriver.motorAForward();
        } else if (buff[0]=='r') {
          motorDriver.motorAReverse();
        } else {
          motorDriver.motorAStop();
        }
        if (buff[1]=='f') {
          motorDriver.motorBForward();
        } else if (buff[1]=='r') {
          motorDriver.motorBReverse();
        } else {
          motorDriver.motorBStop();
        }
#endif
        moving = buff[0]=='f' || buff[0]=='r' || buff[1]=='f' || buff[1]=='r';
        if (moving) {
          started = millis();
          if (buff[0]=='f' && buff[1]=='f') {
            newDir = FORWARD;
          } else if (buff[0]=='r' && buff[1]=='r') {
            newDir = REVERSE;
          } else if (buff[0]=='f' || buff[1]=='r') {
            newDir = RIGHT;
          } else if (buff[0]=='r' || buff[1]=='f') {
            newDir = LEFT;
          }
        } else {
          newDir = STOP;
        }
      }
    }
    if (moving && elapsedMillis(millis(), started, 500)) {
      moving = false;
#if defined(MOTOR_DRIVER) && MOTOR_DRIVER == 8833
      motorDriver.motorAStop();
      motorDriver.motorBStop();
#endif
      newDir = STOP;
      log_d("stopped");
    }
    if (newDir != direction) {
      setDirection(newDir);
      res |= REDRAW_SCREEN;
    }

  } else if (LOGIC_BUTTON_BACK(event)) {

    // Just exit
    return EXIT_APP;

  }

  return res;
}

void MotorDriverApp::redrawScreen(bool redrawAll) {
  log_i("redraw MotorDriverApp");
  if (!screenInited) {
    ((GUIWidget*) bgRect)->redraw(lcd);
    ((GUIWidget*) text)->redraw(lcd);
  }
  if (sign) {
    ((GUIWidget*) sign)->redraw(lcd);
  }
  screenInited = true;
}

#endif // MOTOR_DRIVER
// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Pin control app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

PinControlApp::PinControlApp(LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer) {
  log_d("create PinControlApp");

  header->setTitle("UDP On/Off");
  footer->setButtons(NULL, "Back");

  udp = new WiFiUDP();
  udp->begin(UDP_SERVER_PORT);
  log_d("UDP server on port %d", UDP_SERVER_PORT);

  // Create white background
  int yOff = header->height();
  bgRect = new RectWidget(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(), WP_COLOR_0);
  yOff += marginY;
  ledLabel = new LabelWidget(0, yOff, lcd.width(), fonts[AKROBAT_EXTRABOLD_22]->height(), "LED Off", WP_COLOR_1, WP_COLOR_0, fonts[AKROBAT_EXTRABOLD_22], LabelWidget::CENTER);

  // Set periodic event
  controlState.msAppTimerEventLast = millis();
  controlState.msAppTimerEventPeriod = 25;
}

PinControlApp::~PinControlApp() {
  log_d("destroy PinControlApp");
  delete udp;

  // Delete widgets
  delete bgRect;
  delete ledLabel;
}

appEventResult PinControlApp::processEvent(EventType event) {
  // Exit keys
  appEventResult res = DO_NOTHING;
  if (event == APP_TIMER_EVENT) {

    // Check for incoming commands
    if (udpParsePacketSafe(*udp)>0) {
      char buff[100];
      int cb = udp->read(buff, sizeof(buff)-1);
      if (cb>0) {
        buff[cb] = '\0';
        log_d("UDP received: %s", buff);
        if (buff[0]=='N') {

          controlState.ledPleaseTurnOn = true;
          isOn = true;
          res |= REDRAW_ALL;

          delete bgRect;
          delete ledLabel;
          int yOff = header->height();
          bgRect = new RectWidget(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(), WP_COLOR_1);
          yOff += marginY;
          ledLabel = new LabelWidget(0, yOff, lcd.width(), fonts[AKROBAT_EXTRABOLD_22]->height(), "LED On", WP_COLOR_0, WP_COLOR_1, fonts[AKROBAT_EXTRABOLD_22], LabelWidget::CENTER);

        } else if (buff[0]=='F') {

          controlState.ledPleaseTurnOff = true;
          ledLabel->setText("LED: OFF");
          isOn = false;
          res |= REDRAW_ALL;

          delete bgRect;
          delete ledLabel;
          int yOff = header->height();
          bgRect = new RectWidget(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(), WP_COLOR_0);
          yOff += marginY;
          ledLabel = new LabelWidget(0, yOff, lcd.width(), fonts[AKROBAT_EXTRABOLD_22]->height(), "LED Off", WP_COLOR_1, WP_COLOR_0, fonts[AKROBAT_EXTRABOLD_22], LabelWidget::CENTER);

        }
      }
    }

  } else if (LOGIC_BUTTON_BACK(event)) {

    // Just exit
    return EXIT_APP;

  }

  return res;
}

void PinControlApp::redrawScreen(bool redrawAll) {
  ((GUIWidget*) bgRect)->redraw(lcd);
  ((GUIWidget*) ledLabel)->redraw(lcd);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Phonebook app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

PhonebookApp::PhonebookApp(Audio* audio, LCD& lcd, LCD& hardDisp, ControlState& state, Storage& flash, HeaderWidget* header, FooterWidget* footer, bool pick)
  : WindowedApp(lcd, state, header, footer), FocusableApp(2), audio(audio), hardDisp(hardDisp), flash(flash), standAloneApp(!pick), combinedAddress(NULL) {
  log_d("create PhonebookApp");

  // Menu widgets
  menu    = NULL;
  options = NULL;
  confirmDelete = NULL;

  // VIEWING widgets
  const int16_t pad =  8;
  uint16_t yOff = header->height();
  rect   = new RectWidget(0, header->height(), 50 + pad, 50 + (2*pad), WHITE);      // headpic background
  headpic= new RectIconWidget(pad, header->height() + pad, 50, 50, WP_ACCENT_1, icon_person_w, sizeof(icon_person_w));
  contactName = new MultilineTextWidget(rect->width(), header->height(), lcd.width()-rect->width(), rect->height(),
                                        "(no name)", state, 200, fonts[AKROBAT_EXTRABOLD_22], InputType::AlphaNum, pad, pad);
  contactName->setColors(WP_COLOR_0, WP_COLOR_1);
  contactName->verticalCentering(true);
  yOff += rect->height();

  phonePic = new RectIconWidget(0, yOff, 36, 46, WHITE, icon_phone_b, sizeof(icon_phone_b));
  addressView = new MultilineTextWidget(phonePic->width(), yOff, lcd.width() - phonePic->width(), 46,
                                        "(no number)", state, 200, fonts[AKROBAT_BOLD_18], InputType::AlphaNum, 4, 4);
  addressView->setColors(WP_COLOR_0, WP_COLOR_1);
  addressView->verticalCentering(true);
  yOff += phonePic->height();

  viewMenu = new MenuWidget(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(),
                            "Phonebook is empty", fonts[AKROBAT_BOLD_20], 3, 8);
  viewMenu->setStyle(MenuWidget::DEFAULT_STYLE, WP_COLOR_0, WP_COLOR_1, WP_COLOR_1, WP_ACCENT_1);
  viewMenu->addOption("Call", NULL, 1001, 1, icon_calling_b, sizeof(icon_calling_b), icon_calling_w, sizeof(icon_calling_w));
  viewMenu->addOption("Send message", NULL, 1002, 1, icon_message_b, sizeof(icon_message_b), icon_message_w, sizeof(icon_message_w));

  // ADDING / EDITING widgets
  yOff = header->height();
  addLabelInput(yOff, dispNameLabel, dispNameInput, "Name:", 100);
  addLabelInput(yOff, sipUriLabel, sipUriInput, "SIP URI or number:", 100);
  addLabelInput(yOff, loraLabel, loraInput, "LoRa address:", 100);

  clearRect = new RectWidget(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(), WP_COLOR_1);

  // Populate menu
  createLoadMenu();

  // Focusables
  addFocusableWidget(dispNameInput);
  addFocusableWidget(sipUriInput);
  addFocusableWidget(loraInput);

  // Initialize state
  changeState(SELECTING);
}

PhonebookApp::~PhonebookApp() {
  log_d("destroy PhonebookApp");

  flash.phonebook.backup();

  if (callApp) {
    delete callApp;
  }

  // SELECTING
  if (menu) {
    delete menu;
  }
  if (emptyLabel) {
    delete emptyLabel;
  }

  // VIEWING
  delete rect;
  delete headpic;
  delete phonePic;
  delete contactName;
  delete addressView;
  delete viewMenu;

  // OPTIONS
  if (options) {
    delete options;
  }
  if (confirmDelete) {
    delete confirmDelete;
  }

  // ADDING / EDITING
  delete clearRect;
  delete dispNameLabel;
  delete dispNameInput;
  delete sipUriLabel;
  delete sipUriInput;
  delete loraLabel;
  delete loraInput;

  if (combinedAddress != NULL) {
    delete combinedAddress;
  }
}

const char* PhonebookApp::getSelectedSipUri() {
  if (currentKey>0 && currentKey <=  flash.phonebook.nSections()) {
    return flash.phonebook[currentKey].getValueSafe("s","");            // return SIP URI
  }
  log_e("wrong currentKey");
  return "";
}

const char* PhonebookApp::getSelectedLoraAddress() {
  if (currentKey>0 && currentKey <=  flash.phonebook.nSections()) {
    return flash.phonebook[currentKey].getValueSafe("l","");  // return LoRa address
  }
  log_e("wrong currentKey");
  return "";
}

appEventResult PhonebookApp::changeState(PhonebookAppState_t newState) {
  log_d("changeState PhonebookApp");
  if (newState == SELECTING) {

    log_d("SELECTING");

    // Deactivate / Activate
    deactivateFocusable();
    menu->activate();
    menu->setDrawOnce();
    setFocus(menu);

    // Change settings
    header->setTitle("Phonebook");
    footer->setButtons("Add", "Back");

  } else if (newState == OPTIONS) {

    log_d("OPTIONS");

    // Deactivate / Activate
    deactivateFocusable();
    if (!options) {
      options = new OptionsMenuWidget(0, header->height(), lcd.width(), lcd.height()-header->height()-footer->height());
      if (options) {
        options->addOption("Edit", 0x101);
        options->addOption("Delete", 0x102);
        options->addOption("Call", 0x103);
        options->addOption("Send message", 0x104);
      }
    }
    options->activate();
    setFocus(options);

    // Change settings
    header->setTitle("Options");
    footer->setButtons(NULL, "Back");

  } else if (newState == CONFIRM_DELETE) {

    // Deactivate / Activate
    deactivateFocusable();
    /* Rebuilt EVERY time, not lazily reused: the widget remembers its highlight, so after
     * one real delete the next confirm would open with "Delete this contact" already
     * selected - a single hasty OK erasing a contact is the exact thing this state exists
     * to prevent (review caught it). Two options cost nothing to rebuild. */
    if (confirmDelete) {
      delete confirmDelete;
    }
    confirmDelete = new OptionsMenuWidget(0, header->height(), lcd.width(),
                                          lcd.height()-header->height()-footer->height());
    if (confirmDelete) {
      confirmDelete->addOption("Cancel", 0x201);            // first: a hasty OK is harmless
      confirmDelete->addOption("Delete this contact", 0x202);
      confirmDelete->activate();
      setFocus(confirmDelete);
    }

    header->setTitle("Delete contact?");
    footer->setButtons(NULL, "Back");

  } else if (newState == CALLING) {

    // Do nothing, all the call initiation is done becomeCaller()

  } else {      // ADDING / VIEWING / EDITING

    // Load from flash
    if (currentKey>0) {
      log_d("viewing / editing -> load data from flash");

      // VIEWING / EDITING

      // Load data from flash
      if (currentKey <=  flash.phonebook.nSections()) {
        // Name
        const char* name = flash.phonebook[currentKey].getValueSafe("n","");
        dispNameInput->setText(name);
        contactName->setText(name);
        // SIP URI
        const char* uri = flash.phonebook[currentKey].getValueSafe("s","");
        sipUriInput->setText(uri);
        addressView->setText(uri);

        // LoRa address
        const char* lora = flash.phonebook[currentKey].getValueSafe("l","");
        loraInput->setText(lora);
      }

      // Reset selection in the viewing menu
      if (newState==VIEWING) {
        viewMenu->reset();
      }

    } else {

      // ADDING
      dispNameInput->setText("");
      sipUriInput->setText("");
      loraInput->setText("");
    }

    // Deactivate
    menu->deactivate();

    if (newState == ADDING || newState == EDITING) {
      log_d("ADDING / EDITING");

      footer->setButtons("Save", "Clear");

      // Activate / deactivate
      dispNameInput->activate();
      sipUriInput->activate();
      loraInput->activate();

      // Change settings
      header->setTitle(newState == EDITING ? "Edit contact" : "Create contact");
      setFocus(dispNameInput);

    } else if (newState == VIEWING) {
      log_d("VIEWING");

      footer->setButtons("Options", "Back");

      // Activate / deactivate
      dispNameInput->deactivate();
      sipUriInput->deactivate();
      loraInput->deactivate();

      // Change settings
      header->setTitle("View contact");

    }
  }
  appState = newState;
  screenInited = false;
  return REDRAW_ALL;
}

void PhonebookApp::createLoadMenu() {
  log_d("createLoadMenu PhonebookApp");

  // Create new menu widget
  if (menu!=NULL) {
    delete menu;
  }
  menu = new MenuWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(),
                        "Phonebook is empty", fonts[AKROBAT_EXTRABOLD_22], N_MENU_ITEMS);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WP_COLOR_0, WP_COLOR_1, WP_COLOR_1, WP_ACCENT_1);   // in original design it used WP_ACCENT_0, but this doesn't make sense: too bright, text cannot be read

  // Add all individual addresses
  if (flash.phonebook.isLoaded() || flash.loadPhonebook()) {
    for (auto si = flash.phonebook.iterator(1); si.valid(); ++si) {
      MenuOptionPhonebook* option = new MenuOptionPhonebook((int)si, 1, si->getValueSafe("n", ""), si->getValueSafe("s", ""));
      if (option && !menu->addOption(option)) {
        delete option;
        break;
      }
    }
  }

}

appEventResult PhonebookApp::processEvent(EventType event) {
  log_i("processEvent PhonebookApp");

  appEventResult res = DO_NOTHING;

  if (messageApp != NULL) {

    if ((res = messageApp->processEvent(event)) & EXIT_APP) {
      changeState(appState);    // appState doesn't change, we just use this to update Header and Footer widgets
      delete messageApp;
      messageApp = NULL;
      res = REDRAW_ALL;
    }

  } else if (appState==SELECTING) {
    // Exit keys
    if (LOGIC_BUTTON_BACK(event)) {
      return EXIT_APP;
    }

    if (event == WIPHONE_KEY_CALL) {
      MenuOption::keyType sel = menu->currentKey();
      if (sel>0) {
        currentKey = sel;
        if (this->standAloneApp) {
          this->becomeCaller();
          res |= REDRAW_ALL;
        } else {
          return EXIT_APP;
        }
      }
    } else if (event == WIPHONE_KEY_SELECT) {
      currentKey = 0;   // empty / new
      res |= changeState(ADDING);
    } else {
      // Probably navigation or search
      menu->processEvent(event);
      MenuOption::keyType sel = menu->readChosen();
      if (sel>0) {
        currentKey = sel;
        if (this->standAloneApp) {
          res |= changeState(VIEWING);
        } else {
          return EXIT_APP;
        }
      }
      res |= REDRAW_SCREEN;
    }

  } else if (appState==CALLING) {

    if(!wifiState.isConnected() || WiFi.status() != WL_CONNECTED) {
      if (callApp!=NULL) {
        callApp->setStateCaption("No WiFi Conn");
        callApp->redrawScreen(true);
        delay(1000);
        delete callApp;
        callApp = NULL;
      }
      log_e("WIPHONE_KEY_CALL: call not possible due to wifi lost");
      return EXIT_APP;
    } else if (!controlState.isCallPossible()) {
      if (callApp!=NULL) {
        callApp->setStateCaption("No SIP Conn");
        callApp->redrawScreen(true);
        delay(1000);
        delete callApp;
        callApp = NULL;
      }
      log_e("WIPHONE_KEY_CALL: call not possible due to that no SIP conn.");
      changeState(SELECTING);     // TODO: remove CALLING state, use callApp as an indication of call; "change" to current appState

      //res |= REDRAW_ALL;

      return EXIT_APP;
    }
    if (callApp!=NULL) {
      if ((res = callApp->processEvent(event)) & EXIT_APP) {
        // Idle -> Exit
        changeState(SELECTING);     // TODO: remove CALLING state, use callApp as an indication of call; "change" to current appState
        delete callApp;
        callApp = NULL;
        res |= REDRAW_ALL;
      }
    }

  } else if (appState==OPTIONS) {

    res |= REDRAW_SCREEN;
    if (LOGIC_BUTTON_BACK(event)) {
      res |= changeState(VIEWING);
    } else {
      options->processEvent(event);
      MenuOption::keyType sel = options->readChosen();
      if (sel>0) {
        if (sel==0x101) {
          // "Edit" option selected
          res |= changeState(EDITING);
        } else if (sel==0x102) {
          // "Delete" option selected — ask first. One OK here used to erase the contact
          // AND persist it to flash, no confirmation, no undo, right under "Edit".
          res |= changeState(CONFIRM_DELETE);
        } else if (sel==0x103) {
          // "Call" option selected
          this->becomeCaller();
          res |= REDRAW_ALL;
        } else if (sel==0x104) {
          // "Send message" option selected
          this->sendMessage();
          res |= REDRAW_ALL;
        }
      }
    }

  } else if (appState == CONFIRM_DELETE) {

    res |= REDRAW_SCREEN;
    if (LOGIC_BUTTON_BACK(event)) {
      res |= changeState(VIEWING);
    } else {
      confirmDelete->processEvent(event);
      MenuOption::keyType sel = confirmDelete->readChosen();
      if (sel == 0x202) {
        if (flash.phonebook.removeSection(currentKey)) {
          flash.phonebook.store();
        }
        createLoadMenu();
        res |= changeState(SELECTING);
      } else if (sel == 0x201) {
        res |= changeState(VIEWING);
      }
    }

  } else if (appState == VIEWING) {

    if (LOGIC_BUTTON_BACK(event)) {

      // Exit
      res |= changeState(SELECTING);

    } else if (event == WIPHONE_KEY_SELECT) {

      // Show options for this phonebook entry
      res |= changeState(OPTIONS);

    } else {

      res |= REDRAW_SCREEN;
      viewMenu->processEvent(event);
      MenuOption::keyType sel = viewMenu->readChosen();
      if (sel>0) {
        if (sel==1001) {
          // "Call" menu item
          this->becomeCaller();
          res |= REDRAW_ALL;
        } else if (sel==1002) {
          // Send message
          this->sendMessage();
          res |= REDRAW_ALL;
        }
      }

    }

  } else if (appState==ADDING || appState==EDITING) {      // ADDING / EDITING

    if (LOGIC_BUTTON_OK(event)) {

      log_v("modifying phonebook");
      if (currentKey) {
        flash.phonebook.removeSection(currentKey);
      }
      flash.phonebook.addSection();
      flash.phonebook[-1]["n"] = dispNameInput->getText();

      /* A contact typed as a bare number is completed from the phone's own account before
       * the sip: normalisation below — ten-tap typing "@seattle1.voip.ms" per contact was
       * the whole complaint, and a stored bare number would call fine but silently fail
       * to text (sendMessage uses addresses verbatim). */
      {
        char completedAddr[128];
        if (sipCompleteAddress(sipUriInput->getText(), completedAddr, sizeof(completedAddr))) {
          sipUriInput->setText(completedAddr);
        }
      }

      //check whether uri starts with "sips:" or "SIPS:" or "SIP:" which cannot be called (tested)
      if(strncmp(sipUriInput->getText(), "sips:", 4) == 0 || strncmp(sipUriInput->getText(), "SIPS:", 4) == 0 || strncmp(sipUriInput->getText(), "SIP:", 4) == 0) {
        //TODO display error popup which says we do not support it.
      } else {
        //add "sip:" to sip-uri by default
        if( ! (strncmp(sipUriInput->getText(), "sip:", 4) == 0)) {
          std::string tmpStr(sipUriInput->getText());
          tmpStr = std::string("sip:") + tmpStr;
          sipUriInput->setText(tmpStr.c_str());
        }
      }
      flash.phonebook[-1]["s"] = sipUriInput->getText();
      flash.phonebook[-1]["l"] = loraInput->getText();
      flash.phonebook.reorderLast(1, &(Storage::phonebookCompare));

      // Save phonebook
      log_v("saving");
      bool saved = flash.phonebook.store();

      if (saved) {
        log_v("saved -> ressetting");
        dispNameInput->setText("");
        sipUriInput->setText("");
        loraInput->setText("");
        createLoadMenu();
        res |= changeState(appState==EDITING ? VIEWING : SELECTING);
      } else {
        // TODO: display something to the user on failure
      }

    } else if (event == WIPHONE_KEY_END) {

      res |= changeState(appState==EDITING ? VIEWING : SELECTING);

    } else if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {

      // Change focus
      nextFocus();
      res |= REDRAW_SCREEN;

    } else {

      // Pass button to whatever is focused
      GUIWidget* focused = getFocused();
      if (focused) {
        focused->processEvent(event);
      }
      res |= REDRAW_SCREEN;

    }
  }

  return res;
}

void PhonebookApp::becomeCaller() {
  // Load callee address from flash
  if (currentKey > 0 && currentKey < flash.phonebook.nSections() && *flash.phonebook[currentKey].getValueSafe("s","")!='\0') {
    // Kick-start a call
    snprintf(controlState.lastDialed, sizeof(controlState.lastDialed), "%s",
             (const char*)flash.phonebook[currentKey]["s"]);   // redial covers phonebook calls too
    controlState.setRemoteNameUri(flash.phonebook[currentKey].getValueSafe("n",""),
                                  flash.phonebook[currentKey]["s"]);
    controlState.setSipReason("");
    controlState.setSipState(CallState::InvitingCallee);
  } else {
    //TODO show popup
    log_e("cannot call without sip info!");

    //this callApp object is just created for only showing a popup
    callApp = new CallApp(this->audio, this->hardDisp, this->controlState, true, this->header, this->footer);

    if (callApp!=NULL) {
      callApp->setStateCaption("No SIP URI");
      callApp->redrawScreen(true);
      delay(1000);
      delete callApp;
      callApp = NULL;
    }


    return;
  }

  // GUI
  log_d("CALLING");
  deactivateFocusable();

  // Create call app as caller
  callApp = new CallApp(this->audio, this->hardDisp, this->controlState, true, this->header, this->footer);       // true for caller

  /*if(!controlState.calleeUriDyn || controlState.calleeUriDyn[0] == 0)
  {
    //TODO show popup
    log_e("cannot call without sip info!");
    return;
  }*/


  changeState(CALLING);
}

const char* PhonebookApp::getCombinedAddress() {
  const char* sip  = this->getSelectedSipUri();
  const char* lora = this->getSelectedLoraAddress();

  size_t slen = strlen(sip);
  size_t llen = strlen(lora);

  if (slen > 3 && llen > 3) {
    if (combinedAddress != NULL) {
      free(combinedAddress);
    }

    size_t clen = slen+llen+10;
    combinedAddress = (char*)malloc(clen);
    memset(combinedAddress, 0x00, clen);

    strlcpy(combinedAddress, "LORA:", clen);
    strlcat(combinedAddress, lora, clen);
    strlcat(combinedAddress, "!", clen);
    strlcat(combinedAddress, sip, clen);

    return combinedAddress;
  } else if (slen > 3) {
    return sip;
  } else {
    return lora;
  }
}

void PhonebookApp::sendMessage() {
  messageApp = new CreateMessageApp(lcd, controlState, flash, header, footer, this->getCombinedAddress());
}

void PhonebookApp::redrawScreen(bool redrawAll) {
  log_d("redrawScreen PhonebookApp");

  if (messageApp!=NULL) {
    messageApp->redrawScreen(redrawAll);

  } else if (appState==SELECTING || appState==OPTIONS || appState==CONFIRM_DELETE) {
    if (!screenInited || redrawAll) {
      ((GUIWidget*) rect)->redraw(lcd);
    }
    if (appState==SELECTING) {
      ((GUIWidget*) menu)->redraw(lcd);
    } else if (appState==OPTIONS) {
      ((GUIWidget*) options)->redraw(lcd);
    } else {
      ((GUIWidget*) confirmDelete)->redraw(lcd);
    }
  } else if (appState==CALLING) {

    if (callApp!=NULL) {
      callApp->redrawScreen(redrawAll);
    }

  } else if (appState==VIEWING) {
    if (!screenInited || redrawAll) {
      ((GUIWidget*) rect)->redraw(lcd);
      ((GUIWidget*) headpic)->redraw(lcd);
      ((GUIWidget*) contactName)->redraw(lcd);
      ((GUIWidget*) phonePic)->redraw(lcd);
      ((GUIWidget*) addressView)->redraw(lcd);
    }
    ((GUIWidget*) viewMenu)->redraw(lcd);

  } else {    // ADDING / EDITING
    ((GUIWidget*) dispNameLabel)->redraw(lcd);
    ((GUIWidget*) sipUriLabel)->redraw(lcd);
    ((GUIWidget*) dispNameInput)->redraw(lcd);
    ((GUIWidget*) sipUriInput)->redraw(lcd);
    ((GUIWidget*) loraInput)->redraw(lcd);
    ((GUIWidget*) loraLabel)->redraw(lcd);

    if (!screenInited || redrawAll) {
      ((GUIWidget*) clearRect)->redraw(lcd);
    }
  }

  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Sip Accounts app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/* Widgets go to PSRAM — see the long note on AbstractWidget in GUI.h for why this is at the
 * root of the hierarchy rather than per-app. */
void* AbstractWidget::operator new(size_t n) {
  void* p = ps_malloc(n);
  return p ? p : malloc(n);
}

void AbstractWidget::operator delete(void* p) {
  free(p);
}

/* Apps go to PSRAM — see the note on WiPhoneApp in GUI.h. At the root of the app hierarchy
 * for the same reason AbstractWidget::operator new is at the root of the widget one. */
void* WiPhoneApp::operator new(size_t n) {
  void* p = ps_malloc(n);
  return p ? p : malloc(n);
}

void WiPhoneApp::operator delete(void* p) {
  free(p);
}

/* See the note in GUI.h: measured at −4,236 bytes of contiguous internal heap per open, which
 * pa{user}ed the phone twice within minutes of the first SIP account being configured.
 * Redundant since WiPhoneApp gained the same pair above, and kept for the measurement. */
void* SipAccountsApp::operator new(size_t n) {
  void* p = ps_malloc(n);
  return p ? p : malloc(n);
}

void SipAccountsApp::operator delete(void* p) {
  free(p);
}

SipAccountsApp::SipAccountsApp(LCD& lcd, ControlState& state, Storage& flash, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(4), ini(filename) {
  log_d("create SipAccountsApp");

  udpTcpSipSelection = nullptr;
  // Load SIP accounts from flash
  if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
    // Check version of the file format
    if (!ini[0].hasKey("v") || strcmp(ini[0]["v"], "1")) {
      log_e("file corrupt or unknown file version");
      IF_LOG(VERBOSE)
      ini.show();
      //ini.remove();
    }
  }

  if (ini.isEmpty()) {
    // Create new one:
    //    section [0] is a header.
    //    unnamed sections [1] ... [N] are different SIP accounts havig these fields:
    //      "s=..." SIP URI
    //      "d=..." display name
    //      "p=..." password
    ini.addSection();
    ini[0]["desc"] = "WiPhone SIP accounts";   // comment
    ini[0]["v"] = "1";                         // version
    if (ini.store()) {
      log_d("new file created");
    }
  }

  // Menu widgets
  menu     = NULL;
  viewMenu = NULL;

  // VIEWING widgets
  const int16_t pad =  8;
  uint16_t yOff = header->height();
  rect   = new RectWidget(0, header->height(), 50 + pad, 50 + (2*pad), WHITE);      // headpic background
  headpic= new RectIconWidget(pad, header->height() + pad, 50, 50, WP_ACCENT_1, icon_person_w, sizeof(icon_person_w));
  contactName = new MultilineTextWidget(rect->width(), header->height(), lcd.width()-rect->width(), rect->height(),
                                        "(no name)", state, 200, fonts[AKROBAT_EXTRABOLD_22], InputType::AlphaNum, pad, pad);
  contactName->setColors(WP_COLOR_0, WP_COLOR_1);
  contactName->verticalCentering(true);
  yOff += rect->height();

  phonePic = new RectIconWidget(0, yOff, 36, 46, WHITE, icon_phone_b, sizeof(icon_phone_b));
  addressView = new MultilineTextWidget(phonePic->width(), yOff, lcd.width() - phonePic->width(), 46,
                                        "(no number)", state, 200, fonts[AKROBAT_BOLD_18], InputType::AlphaNum, 4, 4);
  addressView->setColors(WP_COLOR_0, WP_COLOR_1);
  addressView->verticalCentering(true);
  yOff += phonePic->height();

  viewMenu = new MenuWidget(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(),
                            "No SIP accounts", fonts[AKROBAT_BOLD_20], 3, 8);
  viewMenu->setStyle(MenuWidget::DEFAULT_STYLE, WP_COLOR_0, WP_COLOR_1, WP_COLOR_1, WP_ACCENT_1);

  // ADDING / EDITING widgets
  yOff = header->height();

  // - create and arrange widgets
  clearRect = new RectWidget(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(), WP_COLOR_1);
  addLabelInput(yOff, inputLabels[0], inputs[0], "Name:", 100);
  addDoubleLabelInput(yOff, inputLabels[1], inputs[1], "User:", 50, inputLabels[2], inputs[2], "Server:", 50);
  addLabelInput(yOff, inputLabels[3], inputs[3], "SIP URI:", 100);
  addLabelPassword(yOff, inputLabels[4], passwordInput, "Password:", lcd.width()/2);

  udpTcpSipSelection = new ChoiceWidget(lcd.width()/2, yOff-passwordInput->height(), lcd.width()/2, 35);
  udpTcpSipSelection->addChoice("UDP-SIP");
  udpTcpSipSelection->addChoice("TCP-SIP");
  yOff += udpTcpSipSelection->height();

  // Populate menu
  createLoadMenu();

  // Focusables
  for (int i=0; i<sizeof(inputs)/sizeof(inputs[0])-1; i++) {   // don't add SIP URI into focusable - it's autofilled
    addFocusableWidget(inputs[i]);
  }
  addFocusableWidget(passwordInput);
  addFocusableWidget(udpTcpSipSelection);

  // Initialize state
  changeState(SELECTING);
}

SipAccountsApp::~SipAccountsApp() {
  log_d("destroy SipAccountsApp");

  ini.backup();

  // SELECTING
  if (menu) {
    delete menu;
  }
  if (emptyLabel) {
    delete emptyLabel;
  }

  // VIEWING
  delete rect;
  delete headpic;
  delete phonePic;
  delete contactName;
  delete addressView;
  delete viewMenu;

  // ADDING / EDITING
  delete clearRect;
  for (int i=0; i<sizeof(inputLabels)/sizeof(LabelWidget*); i++) {
    delete inputLabels[i];
  }
  for (int i=0; i<sizeof(inputs)/sizeof(TextInputWidget*); i++) {
    delete inputs[i];
  }
  delete passwordInput;
  if(udpTcpSipSelection) {
    delete udpTcpSipSelection;
  }

}

void SipAccountsApp::changeState(SipAccountsAppState_t newState) {
  log_d("changeState SipAccountsApp");
  if (newState == SELECTING) {

    log_d("SELECTING");

    // Deactivate / Activate
    deactivateFocusable();
    menu->activate();
    menu->setDrawOnce();
    setFocus(menu);

    // Change settings
    header->setTitle("SIP accounts");
    footer->setButtons("Add", "Back");

  } else {      // ADDING / VIEWING / EDITING

    // Load from flash
    if (currentKey>0) {

      // VIEWING / EDITING

      // Display account data
      bool primary = false;
      if (currentKey < ini.nSections()) {
        if (ini[currentKey].hasKey("d")) {
          // Display name
          inputs[0]->setText(ini[currentKey]["d"]);
          contactName->setText(ini[currentKey]["d"]);
        }
        if (ini[currentKey].hasKey("s")) {
          // SIP URI
          const char* sipUri = ini[currentKey]["s"];
          addressView->setText(sipUri);
          AddrSpec spec = AddrSpec(sipUri);
          inputs[1]->setText(spec.userinfo());
          inputs[2]->setText(spec.hostPort());
          inputs[3]->setText(sipUri);
        }
        if (ini[currentKey].hasKey("p")) {
          // Password
          passwordInput->setText(ini[currentKey]["p"]);
        }
        //read the UDP-SIP selection from ini (as backward-compatible)
        bool tmpUDP_SIP = false;
        if (ini[currentKey].hasKey("u")) {
          if(strcmp(ini[currentKey]["u"], "UDP-SIP") == 0) {
            tmpUDP_SIP = true;
          } else {
            tmpUDP_SIP = false;
          }
        } else {
          tmpUDP_SIP = false;
        }
        if(tmpUDP_SIP) {
          udpTcpSipSelection->setValue(0);
        } else {
          udpTcpSipSelection->setValue(1);
        }
        primary = ini[currentKey].hasKey("m");
      }

      // Create new viewing menu
      if (newState==VIEWING) {
        viewMenu->deleteAll();
        viewMenu->addOption("Edit", NULL, 1003, 1, icon_edit_b, sizeof(icon_edit_b), icon_edit_w, sizeof(icon_edit_w));
        viewMenu->addOption(primary ? "Unmake primary" : "Make primary", NULL, 1004, 1, icon_edit_b, sizeof(icon_edit_b), icon_edit_w, sizeof(icon_edit_w));
        viewMenu->addOption("Delete", NULL, 1009, 1, icon_delete_r, sizeof(icon_delete_r), icon_delete_w, sizeof(icon_delete_w));
      }

    } else {

      // ADDING
      for (int i=0; i<sizeof(inputs)/sizeof(TextInputWidget*); i++) {
        inputs[i]->setText("");
      }
      passwordInput->setText("");
      udpTcpSipSelection->setValue(1); //TCP-SIP

    }

    // Deactivate
    menu->deactivate();

    if (newState == ADDING || newState == EDITING) {
      log_d("ADDING / EDITING");

      footer->setButtons("Save", "Clear");

      // Activate / deactivate
      for (int i=0; i<sizeof(inputs)/sizeof(inputs[0])-1; i++) {   // don't add SIP URI into focusable - it's autofilled
        inputs[i]->activate();
      }
      passwordInput->activate();
      udpTcpSipSelection->activate();

      // Change settings
      header->setTitle(newState == EDITING ? "Edit account" : "Create account");
      setFocus(inputs[0]);

    } else if (newState == VIEWING) {
      log_d("VIEWING");

      footer->setButtons("Select", "Back");

      // Activate / deactivate
      for (int i=0; i<sizeof(inputs)/sizeof(TextInputWidget*)-1; i++) {   // don't add SIP URI into focusable - it's autofilled
        inputs[i]->deactivate();
      }
      passwordInput->deactivate();
      udpTcpSipSelection->deactivate();

      // Change settings
      header->setTitle("View account");

    }
  }
  appState = newState;
  screenInited = false;
}

void SipAccountsApp::createLoadMenu() {
  log_d("createLoadMenu SipAccountsApp");

  // Create new menu widget
  if (menu!=NULL) {
    delete menu;
  }
  menu = new MenuWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(),
                        "No SIP accounts", fonts[AKROBAT_EXTRABOLD_22], N_MENU_ITEMS);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WP_COLOR_0, WP_COLOR_1, WP_COLOR_1, WP_ACCENT_1);   // in original design it used WP_ACCENT_0, but this doesn't make sense: too bright, text cannot be read

  // Add all individual addresses
  for (auto si = ini.iterator(1); si.valid(); ++si) {
    MenuOptionIconned* option = new MenuOptionIconned((int)si, 1, si->getValueSafe("d"), si->getValueSafe("s"),
        icon_person_b, sizeof(icon_person_b), icon_person_w, sizeof(icon_person_w),
        7,  si->hasKey("m") ? (controlState.sipRegistered ? WP_ACCENT_G : WP_ACCENT_S) : WP_ACCENT_0 );
    if (option && !menu->addOption(option)) {
      delete option;
      break;
    }
  }
}

appEventResult SipAccountsApp::processEvent(EventType event) {
  //log_d("processEvent SipAccountsApp");

  appEventResult res = REDRAW_SCREEN;     // TODO: return DO_NOTHING on irrelevant events

  if (appState==SELECTING) {
    // Exit keys
    if (LOGIC_BUTTON_BACK(event)) {
      return EXIT_APP;
    }

    if (event == WIPHONE_KEY_CALL) {
      MenuOption::keyType sel = menu->currentKey();
      if (sel>0) {
        currentKey = sel;
        // TODO: make this server primary and connect to this server?
      }
    } else if (event == WIPHONE_KEY_SELECT) {
      currentKey = 0;   // empty / new
      changeState(ADDING);
      res |= REDRAW_ALL;
    } else if (event == REGISTRATION_UPDATE_EVENT) {
      // TODO: this can optimized to not recreate menu from scratch
      this->createLoadMenu();
      MenuOption::keyType sel = menu->currentKey();
    } else {
      // Probably navigation or search
      menu->processEvent(event);
      MenuOption::keyType sel = menu->readChosen();
      if (sel>0) {
        currentKey = sel;
        changeState(VIEWING);
        res |= REDRAW_ALL;
      }
    }

  } else if (appState == VIEWING) {

    if (LOGIC_BUTTON_BACK(event)) {

      // Exit
      changeState(SELECTING);
      res |= REDRAW_ALL;

    } else {

      viewMenu->processEvent(event);
      MenuOption::keyType sel = viewMenu->readChosen();

      if (sel>0) {

        if (sel==1003) {

          // "Edit" option selected for a SIP account

          changeState(EDITING);
          res |= REDRAW_ALL;

        } else if (sel==1004) {

          // "Primary" toggle for a SIP account

          if (currentKey < ini.nSections()) {

            bool primary = ini[currentKey].hasKey("m");

            // Remore primary flag from all the records
            ini.clearUniqueFlag("m");

            // Set a primary flag for current record (if it was absent before)
            if (!primary) {
              ini[currentKey]["m"] = "y";  // make current record primary ("main = yes")
            }

            // Change current SIP account in memory
            controlState.setSipAccount(ini[currentKey].getValueSafe("d", ""),
                                       ini[currentKey].getValueSafe("s", ""),
                                       ini[currentKey].getValueSafe("p", ""),
                                       ini[currentKey].getValueSafe("u", ""));
            res |= REDRAW_HEADER;   // redraw SIP icon (if any)

            // Store changes
            if (ini.store()) {
              log_v("saved");
            } else {
              log_e("failed to save");
            }
            createLoadMenu();
            changeState(VIEWING);
            res |= REDRAW_ALL;
          }
        } else if (sel==1009) {
          // "Delete" option selected
          if (currentKey < ini.nSections() && ini[currentKey].hasKey("m")) {
            // This is a primary account -> remove it from memory, don't reconnect
            controlState.removeSipAccount();
          }
          if (ini.removeSection(currentKey)) {
            if (ini.store()) {
              log_v("saved");
            } else {
              log_e("failed to save");
            }
          }
          createLoadMenu();
          changeState(SELECTING);
          res |= REDRAW_ALL;
        }
      }

    }

  } else if (appState==ADDING || appState==EDITING) {      // ADDING / EDITING

    if (LOGIC_BUTTON_OK(event)) {
      //get current UDP_SIP value (UDP - TCP selection choicewidget)
      bool tmpUDP_SIP = false;
      if (udpTcpSipSelection != NULL) {
        log_e("udptcpsipselection: %d", udpTcpSipSelection->getValue());
        switch (udpTcpSipSelection->getValue()) {
        case 0: // UDP-SIP
          tmpUDP_SIP = true;
          break;
        case 1: // TCP-SIP
          tmpUDP_SIP = false;
          break;
        default:
          log_e("Unknown UDP-SIP - TCP-SIP selection: %d", udpTcpSipSelection->getValue());
          tmpUDP_SIP = false;
        }
      }

      log_d("saving SIP accounts");
      bool saved = false;
      if (!currentKey) {
        // Save a new account
        int s = ini.addSection();
        ini[s]["d"] = inputs[0]->getText() ? inputs[0]->getText() : "";
        ini[s]["s"] = inputs[3]->getText() ? inputs[3]->getText() : "";
        ini[s]["p"] = passwordInput->getText() ? passwordInput->getText() : "";
        //set udp-tcp selection
        if(tmpUDP_SIP) {
          ini[s]["u"] = "UDP-SIP";
        } else {
          ini[s]["u"] = "TCP-SIP";
        }
        saved = true;
      } else if (currentKey < ini.nSections()) {

        // Replace existing account
        ini[currentKey]["d"] = inputs[0]->getText() ? inputs[0]->getText() : "";
        ini[currentKey]["s"] = inputs[3]->getText() ? inputs[3]->getText() : "";
        ini[currentKey]["p"] = passwordInput->getText() ? passwordInput->getText() : "";
        //set udp-tcp selection
        if(tmpUDP_SIP) {
          ini[currentKey]["u"] = "UDP-SIP";
          log_e("ini[currentKey][u] = UDP-SIP");
        } else {
          ini[currentKey]["u"] = "TCP-SIP";
          log_e("ini[currentKey][u] = TCP-SIP");
        }

        // Change current SIP account in memory
        if (ini[currentKey].hasKey("m")) {
          // this is a primary account -> update it in memory
          controlState.setSipAccount(ini[currentKey].getValueSafe("d", ""),
                                     ini[currentKey].getValueSafe("s", ""),
                                     ini[currentKey].getValueSafe("p", ""),
                                     ini[currentKey].getValueSafe("u", ""));
          res |= REDRAW_HEADER;   // redraw SIP icon (if any)
        }

        // TODO: sort and update currentKey to newKey
        saved = true;
      }
      if (saved) {
        // Save the accounts to persistent storage
        saved = ini.store();
        if (saved) {
          log_v("saved");
        } else {
          log_e("failed to save");
        }
      }

      if (saved) {
        log_d("saved -> ressetting");
        for (int i=0; i<sizeof(inputs)/sizeof(inputs[0]); i++) {
          inputs[i]->setText("");
        }
        passwordInput->setText("");
        udpTcpSipSelection->setValue(0);//TCP-SIP
        createLoadMenu();
        changeState(appState==EDITING ? VIEWING : SELECTING);
        res |= REDRAW_ALL;
      }

    } else if (event == WIPHONE_KEY_END) {

      changeState(appState==EDITING ? VIEWING : SELECTING);
      res |= REDRAW_ALL;

    } else if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {

      // Change focus
      nextFocus(event == WIPHONE_KEY_DOWN);

    } else {

      // Pass event to whatever widget is focused
      ((TextInputWidget*) getFocused())->processEvent(event);
      if (getFocused()==inputs[1] || getFocused()==inputs[2]) {
        // user name or server name changed -> update SIP URI
        const char* userName = inputs[1]->getText();
        const char* server = inputs[2]->getText();
        if (userName && server && *userName && *server) {
          char buff[100];
          snprintf(buff, sizeof(buff)/sizeof(buff[0]), "sip:%s@%s", userName, server);
          inputs[3]->setText(buff);
        } else if (userName && *userName) {
          char buff[100];
          snprintf(buff, sizeof(buff)/sizeof(buff[0]), "sip:%s", userName);
          inputs[3]->setText(buff);
        } else if (server && *server) {
          char buff[100];
          snprintf(buff, sizeof(buff)/sizeof(buff[0]), "@%s", server);
          inputs[3]->setText(buff);
        } else {
          inputs[3]->setText("");
        }
      }

    }
  }
  return res;
}

void SipAccountsApp::redrawScreen(bool redrawAll) {
  //log_d("redrawScreen SipAccountsApp");

  if (appState==SELECTING) {
    if (!screenInited || redrawAll) {
      ((GUIWidget*) rect)->redraw(lcd);
    }
    ((GUIWidget*) menu)->redraw(lcd);

  } else if (appState==VIEWING) {

    if (!screenInited || redrawAll) {
      ((GUIWidget*) rect)->redraw(lcd);
      ((GUIWidget*) headpic)->redraw(lcd);
      ((GUIWidget*) contactName)->redraw(lcd);
      ((GUIWidget*) phonePic)->redraw(lcd);
      ((GUIWidget*) addressView)->redraw(lcd);
    }
    ((GUIWidget*) viewMenu)->redraw(lcd);

  } else {    // ADDING / EDITING

    if (!screenInited || redrawAll) {
      ((GUIWidget*) clearRect)->redraw(lcd);
      for (int i=0; i<sizeof(inputLabels)/sizeof(inputLabels[0]); i++) {
        ((GUIWidget*) inputLabels[i])->redraw(lcd);
      }
    }

    // Redraw input widgets
    for (int i=0; i<sizeof(inputs)/sizeof(inputs[0]); i++) {
      ((GUIWidget*) inputs[i])->redraw(lcd);
    }
    ((GUIWidget*) passwordInput)->redraw(lcd);
    ((GUIWidget*) udpTcpSipSelection)->redraw(lcd);

  }


  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Call app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

CallApp::CallApp(Audio* audio, LCD& lcd, ControlState& state, bool isCaller, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(2), audio(audio), caller(isCaller) {
  log_d("CallApp create");
  const char* s;

  // Create and arrange widgets
  header->setTitle(isCaller ? "Calling" : "Call");
  footer->setButtons(isCaller ? "Loud Spkr" : "Accept", isCaller ? "Hang up" : "Reject");
  clearRect = new RectWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), WP_COLOR_1);

  // State caption in the middle
  const uint16_t spacing = 4;
  uint16_t yOff = header->height() + 26;
  stateCaption = new LabelWidget(0, yOff, lcd.width(), fonts[AKROBAT_BOLD_20]->height(),
                                 isCaller ? "Making a call..." : "Inbound call...", isCaller ? WP_ACCENT_1 : WP_ACCENT_S, WP_COLOR_1, fonts[AKROBAT_BOLD_20], LabelWidget::CENTER);
  yOff += stateCaption->height() + (spacing*2);

  log_i("CallApp LastReason icon_person_w");
  // Headpic icon
  iconRect = new RectIconWidget((lcd.width()-50)>>1, yOff, 50, 50, isCaller ? WP_ACCENT_1 : WP_ACCENT_S, icon_person_w, sizeof(icon_person_w));
  yOff += iconRect->height() + (spacing*2);

  log_i("CallApp Name and URI above");
  // Name and URI above
  s = controlState.calleeNameDyn!=NULL ? (const char*) controlState.calleeNameDyn : "";
  nameCaption =  new LabelWidget(0, yOff, lcd.width(), fonts[AKROBAT_EXTRABOLD_22]->height(), s, WP_COLOR_0, WP_COLOR_1, fonts[AKROBAT_EXTRABOLD_22], LabelWidget::CENTER);
  yOff += nameCaption->height() + spacing;
  s = controlState.calleeUriDyn!=NULL ? (const char*) controlState.calleeUriDyn : "";
  uriCaption =  new LabelWidget(0, yOff, lcd.width(), fonts[AKROBAT_BOLD_20]->height(), s, WP_DISAB_0, WP_COLOR_1, fonts[AKROBAT_BOLD_20], LabelWidget::CENTER);

  log_i("CallApp Name and uriCaption");
  // Debug string: shows SIP response messages and volume
  yOff += uriCaption->height() + 20;
  s = controlState.lastReasonDyn != NULL ? (const char*) controlState.lastReasonDyn : "";
  debugCaption = new LabelWidget(0, yOff, lcd.width(), fonts[AKROBAT_BOLD_16]->height(), s, WP_DISAB_0, WP_COLOR_1, fonts[AKROBAT_BOLD_16], LabelWidget::CENTER);
  
  reasonHash = hash_murmur(s);
  log_i("hash_murmur");
  /* 🛑 NO ROUTE IS CHOSEN HERE ANY MORE (review SA-2, 2026-09-25). This picked LOUDSPEAKER for
   * every call - meant for the ring - and for a CALLER nothing ever moved it: the far end came
   * out of the back speaker with the phone at your ear, while the footer offered "Loud Spkr".
   * It also ran BEFORE music gave the device back, whose yield puts music's own saved route
   * back over it, so the same outgoing call landed on the earpiece or the loudspeaker depending
   * on whether a track had played. The ring picks its route itself (startRingtone(), after the
   * yield), and a call takes its route where it takes the device, in WiPhone.ino (dialling and
   * connect, after the yield: callTakesAudioDevice), from the key reset below. Nothing is
   * chosen for the "No SIP URI" popup, which this constructor also builds. */
  controlState.callLoudspeaker = false;      // every call starts on the earpiece: "Loud Spkr"
}

CallApp::~CallApp() {
  log_d("destroy CallApp");

  delete stateCaption;
  delete debugCaption;
  //delete debugCaption_loudSpkr;
  delete nameCaption;
  delete uriCaption;
}

appEventResult CallApp::processEvent(EventType event) {
  log_d("processEvent CallApp");
  appEventResult res = DO_NOTHING;
  if (event == WIPHONE_KEY_END) {
    if(!controlState.sipRegistered) {
      log_i("processEvent EXIT_APP");
      return EXIT_APP;
    }

    if (controlState.sipState == CallState::Call or controlState.sipState == CallState::InvitingCallee) {
      log_i("processEvent1 HungUp");
      controlState.setSipState(CallState::HungUp);
      delay(10);
    } else {
      log_i("processEvent EXIT_APP");
      return EXIT_APP;
    }
  }

  if (LOGIC_BUTTON_BACK(event)) {
    log_i("LOGIC_BUTTON_BACK CallApp");
    if(!controlState.sipRegistered) {
      log_i("processEvent EXIT_APP");
      return EXIT_APP;
    }
    // Reject / Hang up
    if (controlState.sipState == CallState::BeingInvited) {
      stateCaption->setText("Declining");
      controlState.setSipState(CallState::Decline);
      res |= REDRAW_SCREEN;
    } else if (controlState.sipState != CallState::Idle && controlState.sipState != CallState::HangUp &&
               controlState.sipState != CallState::HangingUp && controlState.sipState != CallState::HungUp) {
      stateCaption->setText("Hanging up");
      footer->setButtons(NULL, "Hanging");
      controlState.setSipState(CallState::HangUp);
      res |= REDRAW_SCREEN;
    }

  } else if (LOGIC_BUTTON_OK(event)) {

    // Accept call
    if (controlState.sipState == CallState::BeingInvited) {
      stateCaption->setText("Accepting");
      footer->setButtons("Loud Spkr", "Hang up");
      controlState.setSipState(CallState::Accept);
      res |= REDRAW_SCREEN | REDRAW_FOOTER;
      audio->chooseSpeaker(EARSPEAKER);
    }

  } else if (event == CALL_UPDATE_EVENT) {

    uint32_t hash;

    if(controlState.lastReasonDyn != NULL) {
      hash = hash_murmur(controlState.lastReasonDyn);
    }

    if (reasonHash != hash) {
      debugCaption->setText(controlState.lastReasonDyn);
      reasonHash = hash;
      res |= REDRAW_SCREEN;
    }

    if (controlState.sipState == CallState::Idle) {

      log_d("exiting call app");
      return EXIT_APP;

    } else if (controlState.sipState == CallState::Call) {

      // Notify about start of the call
      stateCaption->setText("Call in progress");
      res |= REDRAW_SCREEN;

    } else if (controlState.sipState == CallState::HungUp) {
      log_i("Hung up");
      // Notify about termination of the call
      stateCaption->setText("Hung up");
      res |= REDRAW_SCREEN;

    }

    if (!screenInited) {
      res |= REDRAW_ALL;
    }

  } else if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN) {

    /* 🛑 NO FILE HERE (review R3-1). Every press used to ini.load() and ini.store() configs.ini on
     * the loop task - three SPIFFS opens at 0.5-1.6 s each, and Audio::loop() is pumped only from
     * loop(): 1.5-5 s of silence both ways, mid-call, after every volume press (the far end's RTP
     * overflowing its mailbox, no microphone packets out). The same stall 6900e63 took out of
     * connect. The stored levels are already in the codec - applied at dial or at the ring
     * (SA-1) - so the press steps what the device holds and hands the one level that moved to
     * callLevelStepped() (WiPhone.ino), which writes it to configs.ini ONCE, after the call and
     * off the audio path, with the `loaded` guard (a file that will not load is never replaced
     * by three keys - the 0.9.52 review's find). RAM only here, so nothing here can fail.
     *
     * ⚠ A NOTIFICATION POP IS FINISHED FIRST (review 3, A4) - the pop-first rule every owner of
     * the device follows. A message that arrives while the far end rings plays a pop, which
     * forces the loudspeaker (headphones off) and holds the pre-pop levels in its snapshot for up
     * to ~0.9 s; a press inside it read the POP's levels, and the pop's restore() then wrote the
     * old levels back over it (connect reads no file, so the press did nothing for the call). */
    notifyPopFinishFor("the call screen's volume key");

    /* ⚠ THE CALL LEVELS ARE WHERE MUSIC KEEPS THEM (review 3, A3). After a call ends this screen
     * stays up with no call owning the device (HungUp 2.65 s; HangUp stuck with no WiFi or no
     * account, indefinitely), and F1 may resume a track under it: the codec then holds the MUSIC
     * level and the call levels sit in the player's stash. A press wrote all three stored levels
     * into the codec under the playing track (up to ~24 dB in the ears on headphones - the jump
     * 9213fc0 fixed for Settings > Audio) and the stash later undid it. Settings > Audio's rule:
     * while music holds the codec the call levels are read from, and stepped into, its stash, and
     * the codec is left alone. Seeded before anything else: never from stack garbage. */
    int8_t earpieceVol, headphonesVol, loudspeakerVol;
    if (!musicPlayerCallVolumes(earpieceVol, headphonesVol, loudspeakerVol)) {
      audio->getVolumes(earpieceVol, headphonesVol, loudspeakerVol);
    }
    log_d("Volumes are earspkr %d headphone %d loudspkr %d", earpieceVol,headphonesVol,loudspeakerVol );

    /* 🛑 ONE LEVEL: THE ROUTE YOU ARE LISTENING ON (review SA-1, 2026-09-25). These keys used
     * to move the earpiece, headphones AND loudspeaker levels together and store all three -
     * and the loudspeaker level is the RING's (startRingtone() rings on the loudspeaker at the
     * stored call levels; the ring follows Settings > Audio's "Loudspeaker volume"). Two
     * presses down on a loud earpiece left every later ring 12 dB quieter, across reboots,
     * with nothing on the call screen saying so; a third made it -18. It stayed hidden only
     * while every call end reset the codec to 0/0/0 dB (the never-written restore* globals,
     * removed in 9213fc0). ONLY that level is stepped, and only it is recorded for the file.
     * ⚠ THE ROUTE IS THE CALL'S, NOT THE CODEC'S (review 3, A3/A4): headphones from the jack
     * (Audio's precedence - headphones win; after the pop's finish the flag is the jack's), then
     * the ring's loudspeaker while it rings, then the call screen's own Loud Spkr key. The
     * codec's speaker flag belongs to whoever holds the device NOW - a pop's forced loudspeaker,
     * a resumed track's - and reading it stepped and stored the ring's level with the phone at
     * your ear, labelled "Loudspeaker". */
    const bool onHeadphones = audio->getHeadphones();
    const bool onLoudspeaker = !onHeadphones && (controlState.ringing || controlState.callLoudspeaker);
    int8_t& level = onHeadphones ? headphonesVol : (onLoudspeaker ? loudspeakerVol : earpieceVol);
    const uint8_t levelRoute = onHeadphones ? CALL_LEVEL_HEADPHONES :
                               (onLoudspeaker ? CALL_LEVEL_LOUDSPEAKER : CALL_LEVEL_EARPIECE);
    level += (event == WIPHONE_KEY_UP) ? 6 : -6;
    /* Clamped HERE, the way Audio::setVolumes() clamps: music's stash does not, and what is
     * recorded for the file must be what the driver accepts (a held key once grew the stored
     * number without bound, int8 wrap and all, while the driver clamped silently). */
    const int8_t levelMax = onLoudspeaker ? (int8_t)Audio::MaxLoudspeakerVolume : (int8_t)Audio::MaxVolume;
    if (level > levelMax) {
      level = levelMax;
    }
    if (level < (int8_t)Audio::MuteVolume) {
      level = (int8_t)Audio::MuteVolume;
    }
    uint8_t precentage = 0x0;
    uint8_t precentageLoud = 0x0;
    char buff[50];

    /* Apply: into music's stash while it holds the codec (applied when it lets go), otherwise to
     * the codec, whose readback is the clamped truth. Then record the one level that moved. */
    if (!musicPlayerSetCallVolumes(earpieceVol, headphonesVol, loudspeakerVol)) {
      audio->setVolumes(earpieceVol, headphonesVol, loudspeakerVol);
      audio->getVolumes(earpieceVol, headphonesVol, loudspeakerVol);
    }
    log_d("New Volumes are earspkr %d headphone %d loudspkr %d", earpieceVol,headphonesVol,loudspeakerVol );
    callLevelStepped(levelRoute, level);    // RAM only: WiPhone.ino writes it once, after the call
    
    if (level == -69){
      precentage = 0x0;
      precentageLoud = 0x0;
    } 
    if ((level == -66) || (level == -63) ) {
      precentage = 0x04;
      precentageLoud = 8;
    } 
    if ((level == -60) || (level == -57)) {
      precentage = 12;
      precentageLoud = 16;
    } 
    if ((level == -54) || (level == -51)) {
      precentage = 20;
      precentageLoud = 24;
    } 
    if ((level == -48) || (level == -45)) {
      precentage = 28;
      precentageLoud = 32;
    } 
    if ((level == -42) || (level == -39)) {
      precentage = 36;
      precentageLoud = 40;
    } 
    if ((level == -36) || (level == -33)) {
      precentage = 44;
      precentageLoud = 48;
    } 
    if ((level == -30) || (level == -27)) {
      precentage = 52;
      precentageLoud = 56;
    } 
    if ((level == -24) || (level == -21)) {
      precentage = 60;
      precentageLoud = 64;
    } 
    if ((level == -18) || (level == -15)) {
      precentage = 68;
      precentageLoud = 72;
    } 
    if ((level == -12) || (level == -9)) {
      precentage = 76;
      precentageLoud = 80;
    } 
    if ((level == -6) || (level == -3)) {
      precentage = 84;
      precentageLoud = 90;
    } 
    if ((level == 0) || (level == 3)) {
      precentage = 92;
      precentageLoud = 100;
    } 
    if (level == 6) {
      precentage = 100;
      precentageLoud = 100;
    }
    log_d("precentage is %d %%", precentage);
    log_d("level is %d dB", level);
    /* The route that moved, named - not "Speaker %, Headphones %" off a stale flag. */
    if (onLoudspeaker) {
      snprintf(buff, sizeof(buff), "    Loudspeaker %d %%",  precentageLoud);
    } else {
      snprintf(buff, sizeof(buff), "%s %d %%", onHeadphones ? "Headphones" : "Earpiece", precentage);
    }
    
    precentage = 0x0;
    precentageLoud = 0x0;
    debugCaption->setText(buff);
    //debugCaption_loudSpkr->setText(loudSpkrBuff);

    res |= REDRAW_SCREEN;
  } 
  
  if (event == WIPHONE_KEY_SELECT) {
    if (controlState.sipState == CallState::Call) {
      if (controlState.callLoudspeaker == false){
        footer->setButtons("Ear Spkr", "Hang up");
        res |= REDRAW_SCREEN | REDRAW_FOOTER;
        audio->chooseSpeaker(!EARSPEAKER);
        controlState.callLoudspeaker = true;
      } else {
        footer->setButtons("Loud Spkr", "Hang up");
        res |= REDRAW_SCREEN | REDRAW_FOOTER;
        audio->chooseSpeaker(EARSPEAKER);
        controlState.callLoudspeaker = false;
      }
      
    }
  }
  log_d("res inisde processevent is %x", res);
  return res;
}

void CallApp::redrawScreen(bool redrawAll) {
  log_d("redrawScreen CallApp");

  if (!screenInited || redrawAll) {
    log_d("redraw all");
    // Initialize screen
    ((GUIWidget*) clearRect)->redraw(lcd);

    ((GUIWidget*) iconRect)->redraw(lcd);
    ((GUIWidget*) stateCaption)->redraw(lcd);
    //((GUIWidget*) debugCaption_loudSpkr)->redraw(lcd);
    ((GUIWidget*) debugCaption)->redraw(lcd);
    
    ((GUIWidget*) nameCaption)->redraw(lcd);
    ((GUIWidget*) uriCaption)->redraw(lcd);

    //lcd.fillRect(95, 240, 50, 20, WP_ACCENT_1);     // DEBUG: very strange bug with white pixels over black border

  } else {

    // Refresh only updated labels
    if (stateCaption->isUpdated()) {
      log_d("stateCaption updated");
      ((GUIWidget*) stateCaption)->redraw(lcd);     // TODO: either remove isUpdated, or replace with refresh
    }
    if (debugCaption->isUpdated()) {
      log_d("debugCaption updated");
      ((GUIWidget*) debugCaption)->redraw(lcd);
      //char updateBuff[30] = {0x0};
      //(const char*)updateBuff = controlState.lastReasonDyn != NULL ? (const char*) controlState.lastReasonDyn : "";
      // debugCaption->setText((controlState.lastReasonDyn != NULL ? (const char*) controlState.lastReasonDyn : ""));
      // ((GUIWidget*) debugCaption)->redraw(lcd);
    }
    // if (debugCaption_loudSpkr->isUpdated()) {
    //   log_d("debugCaption_loudSpkr updated");
    //   ((GUIWidget*) debugCaption_loudSpkr)->redraw(lcd);
      
    //   debugCaption_loudSpkr->setText("");
    //   ((GUIWidget*) debugCaption_loudSpkr)->redraw(lcd);
    // }
    if (nameCaption->isUpdated()) {
      log_d("nameCaption updated");
      ((GUIWidget*) nameCaption)->redraw(lcd);
    }
    if (uriCaption->isUpdated()) {
      log_d("uriCaption updated");
      ((GUIWidget*) uriCaption)->redraw(lcd);
    }
  }
  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  WifiAutoSwitch app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

WifiAutoSwitchApp::WifiAutoSwitchApp(LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(1) {
  log_d("WifiAutoSwitchApp");

  header->setTitle("WiFi auto-switch");
  footer->setButtons(NULL, "Back");

  clearRect = new RectWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), WP_COLOR_1);

  uint16_t yOff = header->height() + 8;
  captionLabel = new LabelWidget(0, yOff, lcd.width(), 25, "Auto-connect to the strongest", WP_COLOR_0, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += captionLabel->height();
  hintLabel = new LabelWidget(0, yOff, lcd.width(), 25, "saved network (scans ~10 min)", WP_COLOR_0, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += hintLabel->height() + 8;

  choice = new ChoiceWidget(0, yOff, lcd.width(), 35);
  choice->addChoice("ON");
  choice->addChoice("OFF");
  choice->setValue(wifiState.autoSwitchEnabled() ? 0 : 1);

  addFocusableWidget(choice);
  setFocus(choice);
  screenInited = false;
}

WifiAutoSwitchApp::~WifiAutoSwitchApp() {
  log_d("destroy WifiAutoSwitchApp");
  delete clearRect;
  delete captionLabel;
  delete hintLabel;
  delete choice;
}

appEventResult WifiAutoSwitchApp::processEvent(EventType event) {
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }
  choice->processEvent(event);          // left/right cycles ON/OFF
  bool enabled = (choice->getValue() == 0);
  if (enabled != wifiState.autoSwitchEnabled()) {
    wifiState.setAutoSwitch(enabled);   // persists to networks.ini
  }
  return REDRAW_SCREEN;
}

void WifiAutoSwitchApp::redrawScreen(bool redrawAll) {
  if (!screenInited || redrawAll) {
    ((GUIWidget*) clearRect)->redraw(lcd);
    ((GUIWidget*) captionLabel)->redraw(lcd);
    ((GUIWidget*) hintLabel)->redraw(lcd);
  }
  ((GUIWidget*) choice)->redraw(lcd);
  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Mute app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool guiSetMuted(Audio* audio, ControlState& state, bool muted) {
  state.audioMuted = muted;
  if (audio) {
    audio->setMuted(muted);
  }
  CriticalFile ini(Storage::ConfigsFile);
  if (!(ini.load() || ini.restore())) {
    log_e("MUTE: configs file unreadable - %s now, not remembered", muted ? "muted" : "unmuted");
    return false;
  }
  if (!ini.hasSection("audio")) {
    ini.addSection("audio");
  }
  ini["audio"]["mute"] = muted ? 1 : 0;
  if (!ini.store()) {
    log_e("MUTE: configs file not written - %s now, not remembered", muted ? "muted" : "unmuted");
    return false;
  }
  return true;
}

MuteApp::MuteApp(Audio* audio, LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(1), audio(audio) {
  log_d("MuteApp");

  header->setTitle("Mute all sounds");
  footer->setButtons(NULL, "Back");

  clearRect = new RectWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), WP_COLOR_1);

  uint16_t yOff = header->height() + 8;
  captionLabel = new LabelWidget(0, yOff, lcd.width(), 25, "Nothing out of the loudspeaker:", WP_COLOR_0, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += captionLabel->height();
  hintLabel = new LabelWidget(0, yOff, lcd.width(), 25, "ring, chirps, music, Game Boy.", WP_COLOR_0, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += hintLabel->height();
  hint2Label = new LabelWidget(0, yOff, lcd.width(), 25, "Earpiece, headphones, vibrate work.", WP_COLOR_0, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += hint2Label->height();
  hint3Label = new LabelWidget(0, yOff, lcd.width(), 25, "Or hold # where nothing is typed.", WP_COLOR_0, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += hint3Label->height() + 8;

  choice = new ChoiceWidget(0, yOff, lcd.width(), 35);
  choice->addChoice("Sounds ON");
  choice->addChoice("MUTED");
  choice->setValue(controlState.audioMuted ? 1 : 0);

  addFocusableWidget(choice);
  setFocus(choice);
  screenInited = false;
}

MuteApp::~MuteApp() {
  log_d("destroy MuteApp");
  delete clearRect;
  delete captionLabel;
  delete hintLabel;
  delete hint2Label;
  delete hint3Label;
  delete choice;
}

appEventResult MuteApp::processEvent(EventType event) {
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }
  choice->processEvent(event);          // left/right cycles ON/MUTED
  const bool muted = (choice->getValue() == 1);
  if (muted != controlState.audioMuted) {
    guiSetMuted(audio, controlState, muted);   // applied now, stored in configs.ini
    return REDRAW_ALL;                          // the header's crossed speaker
  }
  return REDRAW_SCREEN;
}

void MuteApp::redrawScreen(bool redrawAll) {
  if (!screenInited || redrawAll) {
    ((GUIWidget*) clearRect)->redraw(lcd);
    ((GUIWidget*) captionLabel)->redraw(lcd);
    ((GUIWidget*) hintLabel)->redraw(lcd);
    ((GUIWidget*) hint2Label)->redraw(lcd);
    ((GUIWidget*) hint3Label)->redraw(lcd);
  }
  ((GUIWidget*) choice)->redraw(lcd);
  screenInited = true;
}

/* 🛑 Settings > WiFi scans every 5 s and joins networks; under a live soft-AP that is AP+STA
 * channel hopping and, on a join, the station connect WiPhone.ino's reconnect gate calls a chip
 * panic. kosyncWindowClose() ends a sync window's hotspot; this ends the other kind (0.9.79). No
 * UI uploader can be up here — cleanAppDynamic() deleted the screen that owned it and its
 * destructor called xferStop() — so this only ever meets a HEADLESS one (serial `up on`,
 * wiphone_send.py, -DXFER_AUTOSTART). Said out loud: someone may be mid-upload from a computer. */
static void wifiScreenStopsHotspotUploader(const char* screen) {
  if (xferServing() && xferUsingAP()) {
    log_e("XFER: %s opened - stopping the uploader's hotspot '%s' (a scan or join under it)",
          screen, xferApName());
    xferStop();
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  EditNetwork app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

EditNetworkApp::EditNetworkApp(LCD& lcd, ControlState& state, const char* SSID, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(5), ini(Networks::filename) {
  log_d("EditNetworkApp");
  // It joins networks (connectTo); see the note in NetworksApp's constructor. A no-op when
  // NetworksApp opened this screen, which already closed it.
  kosyncWindowClose("Settings > WiFi opened");
  wifiScreenStopsHotspotUploader("Settings > edit network");

  if (ini.load() || ini.restore()) {
    if (ini.isEmpty() || !ini[0].hasKey("v") || strcmp(ini[0]["v"], "1")) {
      log_d("unknown version or corrupt \"%s\" file", ini.filename());
    }
  } else {
    ini[0]["desc"] = "WiPhone WiFi networks";
    ini[0]["v"] = "1";
  }
  IF_LOG(VERBOSE)
  ini.show();

  // if SSID is NULL - it is run as a standalone app (if yes - edit the current network)
  standAloneApp = false;
  if (SSID == NULL) {
    standAloneApp = true;

    // Load current network name (connected or previously connected)
    if (wifiState.ssid() != NULL) {
      SSID = wifiState.ssid();
      log_v("SSID: %s", SSID);
    }
  }

  // Is this network connected?
  connectedNetwork = false;
  if ( SSID != NULL && (SSID == wifiState.ssid() || (wifiState.ssid()!=NULL && !strcmp(wifiState.ssid(), SSID)) ) ) {       // network name coincides
    if (wifiState.isConnected()) {
      log_d("network is connected");
      connectedNetwork = true;
    }
  }

  // Is this network in list of known networks?
  knownNetwork = ini.query("s", SSID) >= 0;         // find a section with field "s" (SSID) equal to the current network SSID

  // Create and arrange general widgets
  header->setTitle("Edit Network");
  footer->setButtons("Connect", "Clear");

  clearRect = new RectWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), WP_COLOR_1);

  uint16_t yOff = header->height() + 5;

  // Add form
  this->addLabelInput(yOff, ssidLabel, ssidInput, "SSID:", 50);
  this->addLabelInput(yOff, passLabel, passInput, "Password:", 100);

  const uint16_t spacing = 6; // 4
  const uint16_t xOff = 2;
  yOff += spacing*2;
  lcd.setTextFont(fonts[OPENSANS_COND_BOLD_20]);
  uint16_t buttonSize1 = lcd.textWidth("Forget") + BUTTON_PADDING;
  uint16_t buttonSize2 = lcd.textWidth("Disconnect ") + BUTTON_PADDING;
  saveButton = new ButtonWidget(xOff, yOff, "Save");

  forgetButton = NULL;
  connectionButton = NULL;
  wifiOnOff = NULL;
  if (knownNetwork) {
    forgetButton = new ButtonWidget(xOff + saveButton->width() + 2*spacing, yOff, "Forget");

    yOff += saveButton->height() + spacing*2;
    connectionButton = new ButtonWidget(xOff, yOff, connectedNetwork ? "Disconnect" : "Connect", ButtonWidget::textWidth("Connecting")+18);

    wifiOnOff = new ChoiceWidget(0, yOff+connectionButton->height(), lcd.width(), 35);
    wifiOnOff->addChoice("WIFI-ON");
    wifiOnOff->addChoice("WIFI-OFF");
    if(wifiOn){
      wifiOnOff->setValue(0);
    } else {
      wifiOnOff->setValue(1);
    }

    yOff += wifiOnOff->height();
  
  } else {
    
    wifiOnOff = new ChoiceWidget(0, yOff+saveButton->height(), lcd.width(), 35);
    wifiOnOff->addChoice("WIFI-ON");
    wifiOnOff->addChoice("WIFI-OFF");
    if(wifiOn){
      wifiOnOff->setValue(0);
    } else {
      wifiOnOff->setValue(1);
    }

    yOff += wifiOnOff->height();
  
  }

  // Load password / populate text
  if (SSID != NULL) {
    ssidInput->setText(SSID);
    int index = ini.query("s", SSID);           // "s" - stands for "SSID"
    if (index>=0 && ini[index].hasKey("p")) {   // "p" - stands for "password"
      passInput->setText(ini[index]["p"]);
    }
  } else {
    ssidInput->setText("");
    passInput->setText("");
  }

  // Focusables
  addFocusableWidget(ssidInput);
  addFocusableWidget(passInput);
  addFocusableWidget(saveButton);
  if (forgetButton != NULL) {
    addFocusableWidget(forgetButton);
  }
  if (connectionButton != NULL) {
    addFocusableWidget(connectionButton);
  }
  if (wifiOnOff != NULL) {
    addFocusableWidget(wifiOnOff);
  }

  lastWifiOnOff = wifiOnOff->getValue();
  setFocus(ssidInput);
  screenInited = false;
}

EditNetworkApp::~EditNetworkApp() {
  log_d("destroy EditNetworkApp");

  ini.backup();

  delete clearRect;
  delete ssidLabel;
  delete ssidInput;
  delete passLabel;
  delete passInput;
  delete saveButton;
  if(wifiOnOff){
    delete wifiOnOff;
  }
  
}

appEventResult EditNetworkApp::processEvent(EventType event) {
  log_d("processEvent EditNetworkApp");

  bool quit = false;

  FocusableWidget* focusedWidget = getFocused();

  if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {

    // Change focus
    nextFocus(event == WIPHONE_KEY_DOWN);

  } else if (event == WIPHONE_KEY_END) {

    quit = true;

  } else if (LOGIC_BUTTON_OK(event) && focusedWidget == saveButton &&
             (ssidInput->getText() == NULL || !ssidInput->getText()[0])) {

    /* 🛑 NO SAVE WITH A BLANK SSID (0.9.79 review). The save below queries the INI for "", finds
     * nothing, adds a section s="" p="" and MOVES THE PREFERRED FLAG ("m") ONTO IT: the saved
     * network stops being the one the phone boots onto, and networks.ini keeps an empty network
     * for good. The standalone screen opens blank whenever the driver has no current SSID — after
     * a reboot with the network Disconnected, for one (wifiSsidDyn is only set by a join or a
     * save) — so one OK on Save from there was enough. Nothing is written; the screen stays. */
    log_e("WIFI: Save refused - the SSID is blank (type the network's name first)");

  } else if (LOGIC_BUTTON_OK(event) && focusedWidget == saveButton) {

    // If "OK" was pressed while one of saveButton selected

    // Save new WiFi credentials to NVS
    log_d("save button pressed");

    // Reflect changes to NanoINI
    int index = ini.query("s", ssidInput->getText());       // "s" key stands for "SSID"
    if (index >= 0) {
      ini[index]["p"] = passInput->getText();               // update password for a known network ("p" key")
    } else {
      int i = ini.addSection();
      ini[i]["s"] = ssidInput->getText();
      ini[i]["p"] = passInput->getText();
      // TODO: maybe we don't always want to set the network as preferred?
      ini.setUniqueFlag(i, "m");                            // "m" (for "main") is the preferred network flag
    }

    // Save to file, reload current network
    {
      ini.store();
      log_d("saved network");

      log_d("disconnecting");
      wifiState.disconnect();

      // Update the WiFi credentials from NVS
      // TODO: clean it up
      wifiState.loadPreferred();
      wifiState.loadNetworkSettings(ssidInput->getText());

      // Quit from the app
      quit = true;
    }

  } else if (LOGIC_BUTTON_OK(event) && forgetButton!=NULL && focusedWidget == forgetButton) {

    log_d("forget button pressed");
    int i = ini.query("s", ssidInput->getText());     // TODO: consider that there might be multiple networks with this name
    if (i>=0) {
      bool removed = false;
      if (ini.removeSection(i)) {
        log_d("Network forgotten: %s", ssidInput->getText());
        removed = ini.store();
      }
      if (!removed) {
        log_d("COULD NOT BE REMOVED: %s", ssidInput->getText());
      }
      quit = true;
    }
    wifiState.disable();

  } else if (event==WIPHONE_KEY_CALL || event==WIPHONE_KEY_SELECT || (LOGIC_BUTTON_OK(event) && connectionButton!=NULL && focusedWidget == connectionButton) ) {

    // Conect to the network event

    // TODO: move actual connecting / unconnecting to the main cycle?
    // TODO: after the credentials have changed, disable "Connect" button

    log_d("connection button pressed");
    if (connectedNetwork) {
      log_d("disconnecting");
      //wifiState.disconnect();
      wifiState.disable();
      quit = true;

      int index = ini.query("s", ssidInput->getText());       // "s" key stands for "SSID"
      if (index >= 0) {
        ini[index]["disabled"] = "true";
        ini.store();
      } 
    } else {
      /* ⚠ A JOIN THAT STARTS TURNS THE SWITCH ON (0.9.79). connectTo() clears userDisabled but
       * never radioOff, so Connect with WiFi off joined while the menu said "off" — and then the
       * next game exit or window close honoured the stale switch and dropped the network just
       * joined, and the next boot came up off. A join IS switching WiFi on, so say so: persisted,
       * and the WIFI-ON/OFF choice below shows it (its handler then sees no change).
       * 🛑 ONLY ONCE connectTo() HAS STARTED ONE (review). SELECT and CALL reach this branch from
       * any field, saved network or not, Connect button or not, so switching on FIRST meant a
       * Connect on a typed-in, unsaved SSID (or a blank one) failed here — nothing in
       * networks.ini to join — yet left WiFi on, persisted, with the retry re-armed: the loop
       * joined the OLD preferred network ~20 s later while the owner saw a Connect that did
       * nothing. Now a failed Connect does nothing, as it did before 0.9.79. The order is safe:
       * connectToWiFi() does not ask radioOff, and setRadioOff(false) only re-arms the flags
       * (resumeReconnect) that the successful connectTo() has just set anyway. */
      const bool wasOff = wifiState.radioOff();
      if (wifiState.connectTo(ssidInput->getText())) {
        log_d("connecting: %s", ssidInput->getText());
        if (wasOff) {
          wifiState.setRadioOff(false);
          if (wifiOnOff != NULL) {
            wifiOnOff->setValue(0);
            lastWifiOnOff = 0;
          }
        }

        int index = ini.query("s", ssidInput->getText());       // "s" key stands for "SSID"
        if (index >= 0) {
          ini[index]["disabled"] = "false";
          ini.store();
        } 

        /* Change button appearance. ⚠ NULL-CHECKED (0.9.79 review): the button exists only when
         * the SSID the screen OPENED with was saved, but SELECT/CALL join whatever is typed now —
         * a saved name typed into the blank standalone screen (a reboot with the network
         * Disconnected opens it that way) joined and then dereferenced NULL here. */
        if (connectionButton != NULL) {
          connectionButton->setText("Connecting");
        }
        //((GUIWidget*) connectionButton)->redraw(lcd);         // TODO: works, but doesn't separate event processing from redrawing well (move connecting logic elsewhere)

        int i = ini.query("s", ssidInput->getText());                   // "s" for "SSID"
        if (i >= 0 && ini.setUniqueFlag(i, "m") && ini.store()) {       // "m" for "main" (preferred network)
          log_d("set as preferred network");
        }

        // Wait for result
        // TODO: move actual connecting elsewhere
        log_d("waiting for connectionEvent");
        for (uint8_t j=0; j<50 && !wifiState.isConnectionEvent(); j++) {
          delay(100);
        }

        // Quit or stay
        if (wifiState.isConnectionEvent()) {
          log_d("connection event happened");
          delay(100);
          quit = true;
        } else {
          log_d("connection timeout");

          // Restore button appearance (NULL-checked: see "Connecting" above)
          if (connectionButton != NULL) {
            connectionButton->setText("Connect");
          }
          //((GUIWidget*) connectionButton)->redraw(lcd);       // Works, but doesn't separate event processing from redrawing well
        }

      } else {
        log_d("could not connect: %s", ssidInput->getText());
      }
    }

  } else {

    // Pass button to whatever is focused

    if (focusedWidget != NULL) {
      focusedWidget->processEvent(event);
    }

  }

  
  // Apply the WIFI-ON/OFF toggle ONLY when its value actually changes. This
  // block used to run on EVERY event (key or background tick): with the toggle
  // at ON it disconnected + reconnected WiFi and blocked 5s waiting, per event
  // — which is why this screen froze and ate all input. It also dereferenced
  // connectionButton, which is NULL for unknown networks.
  if (wifiOnOff != NULL && wifiOnOff->getValue() != lastWifiOnOff) {
    lastWifiOnOff = wifiOnOff->getValue();
    if (lastWifiOnOff == 0) {           // WIFI-ON
      wifiState.setRadioOff(false);
      // wifiRestoreStation(), not a bare esp_wifi_start(): see the Settings toggle (0.9.79).
      if (!wifiRestoreStation("Settings > edit network: WIFI-ON")) {
        log_e("WIFI can't be started");
      } else {
        /* ON means "managed again" (2026-08-27): with no SSID below, the retry and
         * auto-switch loops do the joining, and they gate on the very flags disable()
         * cleared — without this, OFF→ON here left auto-rejoin dead until a join or
         * reboot. With an SSID, connectTo() sets the same flags; harmless prelude. */
        wifiState.resumeReconnect();
        if (ssidInput->getText() != NULL && ssidInput->getText()[0]) {
          // Reconnect asynchronously; the main loop's reconnect/auto-switch
          // logic takes it from here (no blocking waits in the UI).
          if (wifiState.connectTo(ssidInput->getText())) {
            log_d("connecting: %s", ssidInput->getText());
            if (connectionButton != NULL) {
              connectionButton->setText("Connecting");
            }
          }
        }
      }
    } else {                            // WIFI-OFF
      wifiState.setRadioOff(true);      // persists and disables in one call
      log_d("disconnecting, WiFi off");
      connectedNetwork = false;
      if (connectionButton != NULL) {
        connectionButton->setText("Connect");
      }
    }
  }

  return quit ? EXIT_APP : REDRAW_ALL;
}

void EditNetworkApp::redrawScreen(bool redrawAll) {
  log_d("redrawScreen EditNetworkApp");

  if (!screenInited || redrawAll) {
    ((GUIWidget*) clearRect)->redraw(lcd);
    ((GUIWidget*) ssidLabel)->redraw(lcd);
    ((GUIWidget*) passLabel)->redraw(lcd);
  }
  ((GUIWidget*) ssidInput)->redraw(lcd);
  ((GUIWidget*) passInput)->redraw(lcd);
  ((GUIWidget*) saveButton)->redraw(lcd);
  if (forgetButton != NULL) {
    ((GUIWidget*) forgetButton)->redraw(lcd);
  }
  if (connectionButton != NULL) {
    ((GUIWidget*) connectionButton)->redraw(lcd);
  }
  if (wifiOnOff != NULL) {
    ((GUIWidget*) wifiOnOff)->redraw(lcd);
  }
  
  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  TimeConfig app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/* Settings > Time zone (0.9.82; "Time offset" before it). The first row chooses:
 *   Auto        the zone of a MEASURED position - this phone's GPS or the hotspot phone's - by
 *               tz_auto.h's rules (never a bench fix, never near a zone line, two looks 2 min
 *               apart); until one is found, the typed offset below, so an upgrade changes nothing
 *   Pacific ... Saskatchewan   a named zone with its DST rule: the clock changes by itself
 *   Custom      the typed offset below, no DST - exactly the old screen
 * The typed offset is the second row, always there: Custom uses it, Auto falls back to it. Under a
 * ruler, what is IN FORCE (which may not be what the first row shows until Save): the zone and
 * offset, where it came from, the next DST change, and Auto's last look. Then the zone map's ODbL
 * credit (THIRD-PARTY-NOTICES.md): this is the screen that shows a zone the map found.
 * Every line was measured against the firmware's Akrobat Bold 16 on 228 px (test_tzauto). */

// The typed offset as Clock::parseTimeZone reads it back exactly: "-7", "-3.5", "5.75".
static void timeConfigFmtTyped(int minutes, char* out, size_t cap) {
  if (minutes % 60 == 0) {
    snprintf(out, cap, "%d", minutes / 60);
    return;
  }
  char b[16];
  snprintf(b, sizeof(b), "%.2f", minutes / 60.0);
  size_t n = strlen(b);
  while (n && b[n - 1] == '0') {
    b[--n] = '\0';
  }
  if (n && b[n - 1] == '.') {
    b[--n] = '\0';
  }
  strlcpy(out, b, cap);
}

TimeConfigApp::TimeConfigApp(LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(2), ini(Storage::ConfigsFile) {
  log_d("TimeConfigApp");

  if (ini.load() || ini.restore()) {
    if (ini.isEmpty() || !ini[0].hasKey("v") || strcmp(ini[0]["v"], "1")) {
      log_d("unknown version or corrupt \"%s\" file", ini.filename());
    }
  } else {
    ini[0]["desc"] = "WiPhone general configs";
    ini[0]["v"] = "1";
  }
  IF_LOG(VERBOSE)
  ini.show();

  if (!ini.hasSection("time")) {
    log_e("adding section `time`");
    ini.addSection("time");
    ini["time"]["zone"] = "-0";
  }

  // Create and arrange general widgets (the footer's words follow the focus: footerForFocus)
  header->setTitle("Time zone");

  clearRect = new RectWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), WP_COLOR_1);

  uint16_t yOff = header->height() + 4;
  const uint16_t labelW = 100;
  SmoothFont* f16 = fonts[AKROBAT_BOLD_16];

  // Add form
  zoneLabel = new LabelWidget(0, yOff, labelW, 25, "Time zone", WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::RIGHT_TO_LEFT, 8);
  zoneChoice = new ChoiceWidget(labelW, yOff, lcd.width() - labelW, 25, fonts[AKROBAT_BOLD_18]);
  for (int c = 0; c < TZ_CHOICE_COUNT; c++) {
    zoneChoice->addChoice(tzChoiceName(c));
  }
  zoneChoice->setValue(tzChoiceOf(tzSvcMode(), tzSvcNamed()));
  yOff += 25;
  hintLabel = new LabelWidget(0, yOff, lcd.width(), 20, "", WP_DISAB_0, WP_COLOR_1, f16, LabelWidget::LEFT_TO_RIGHT, 6);
  yOff += 22;
  this->addInlineLabelInput(yOff, labelW, timeZoneLabel, timeZoneInput, "Time offset", 9);
  this->addRuler(yOff, ruler1, 3);
  nowZoneLabel = new LabelWidget(0, yOff, lcd.width(), 20, "", WP_COLOR_0, WP_COLOR_1, f16, LabelWidget::LEFT_TO_RIGHT, 6);
  yOff += 20;
  nowSourceLabel = new LabelWidget(0, yOff, lcd.width(), 20, "", WP_COLOR_0, WP_COLOR_1, f16, LabelWidget::LEFT_TO_RIGHT, 6);
  yOff += 20;
  nextLabel = new LabelWidget(0, yOff, lcd.width(), 20, "", WP_COLOR_0, WP_COLOR_1, f16, LabelWidget::LEFT_TO_RIGHT, 6);
  yOff += 20;
  autoLabel = new LabelWidget(0, yOff, lcd.width(), 20, "", WP_DISAB_0, WP_COLOR_1, f16, LabelWidget::LEFT_TO_RIGHT, 6);
  yOff += 20;
  this->addRuler(yOff, ruler2, 3);
  creditLabel1 = new LabelWidget(0, yOff, lcd.width(), 18, "Zone map (c) OpenStreetMap", WP_DISAB_0, WP_COLOR_1, f16, LabelWidget::LEFT_TO_RIGHT, 6);
  yOff += 18;
  creditLabel2 = new LabelWidget(0, yOff, lcd.width(), 18, "contributors, ODbL 1.0", WP_DISAB_0, WP_COLOR_1, f16, LabelWidget::LEFT_TO_RIGHT, 6);
  yOff += 20;
  this->errorLabel = new LabelWidget(0, yOff, lcd.width(), 25, "", TFT_RED, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += this->errorLabel->height();

  /* The typed offset as tz_service holds it: NVS "cust", which boot keeps equal to configs.ini's
   * [time] zone (the INI wins when the two differ). */
  char typed[16];
  timeConfigFmtTyped(tzSvcCustomMinutes(), typed, sizeof(typed));
  timeZoneInput->setText(typed);

  // Focusables
  addFocusableWidget(zoneChoice);
  addFocusableWidget(timeZoneInput);
  setFocus(zoneChoice);
  footerForFocus();

  shownChoice = 0xFFFF;
  refreshHint();
  refreshInfo();
  screenInited = false;
}

TimeConfigApp::~TimeConfigApp() {
  log_d("destroy TimeConfigApp");

  ini.backup();

  delete zoneLabel;
  delete zoneChoice;
  delete hintLabel;
  delete timeZoneLabel;
  delete timeZoneInput;
  delete ruler1;
  delete nowZoneLabel;
  delete nowSourceLabel;
  delete nextLabel;
  delete autoLabel;
  delete ruler2;
  delete creditLabel1;
  delete creditLabel2;
  delete errorLabel;
}

void TimeConfigApp::refreshHint() {
  const uint16_t c = zoneChoice->getValue();
  if (c == shownChoice) {
    return;
  }
  shownChoice = c;
  char b[64];
  tzChoiceHint(c, ntpClock.isTimeKnown(), (int64_t)ntpClock.getExactUtcTime(), b, sizeof(b));
  hintLabel->setText(b);
}

/* The right soft key is BACK, and what it does depends on the row: on the zone choice it leaves
 * without saving, in the offset field it deletes a character. The footer says which (review,
 * 2026-10-10: it said "Clear" over the choice row, where pressing it threw the choice away). */
void TimeConfigApp::footerForFocus() {
  footer->setButtons("Save", getFocused() == (FocusableWidget*) zoneChoice ? "Back" : "Clear");
}

void TimeConfigApp::refreshInfo() {
  TzEff e;
  tzSvcEffective(&e);
  char b[64];
  tzZoneText(&e, b, sizeof(b));
  nowZoneLabel->setText(b);
  tzSourceText(&e, tzSvcAuto()->zoneSrc, b, sizeof(b));
  nowSourceLabel->setText(b);
  tzNextText(&e, b, sizeof(b));
  nextLabel->setText(b);
  if (tzSvcMode() == TZ_MODE_AUTO) {
    tzaWhyText(tzSvcAuto(), b, sizeof(b));
  } else {
    b[0] = '\0';
  }
  autoLabel->setText(b);
}

appEventResult TimeConfigApp::processEvent(EventType event) {
  log_v("<-- enter function");

  bool quit = false;
  appEventResult res = REDRAW_SCREEN;

  FocusableWidget* focusedWidget = getFocused();

  if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {

    // Change focus, and the right soft key's word with it ("Back" on the choice, "Clear" below)
    nextFocus(event == WIPHONE_KEY_DOWN);
    footerForFocus();
    res |= REDRAW_FOOTER;

  } else if (event == WIPHONE_KEY_END ||
             (event == WIPHONE_KEY_BACK && focusedWidget == (FocusableWidget*) zoneChoice)) {

    /* END leaves without saving, as it always did. So does Back on the zone row - the footer
     * says "Back" there (footerForFocus): the screen now opens on that row, and Back on a choice
     * has nothing else to do (in the offset field it is still the backspace, "Clear"). */
    quit = true;

  } else if (event == TIME_UPDATE_EVENT) {

    // The minute tick, or tz_service changed the offset (a DST change, a zone Auto found)
    refreshInfo();
    shownChoice = 0xFFFF;               // a named zone's hint says its offset NOW
    refreshHint();

  } else if (LOGIC_BUTTON_OK(event)) {

    log_d("Save button pressed");

    uint8_t mode = TZ_MODE_AUTO;
    int named = -1;
    tzChoiceTo(zoneChoice->getValue(), &mode, &named);

    // Convert/Check user input: the typed offset (Custom uses it, Auto falls back to it)
    const char* tzText = timeZoneInput->getText();
    const char* error = NULL;
    float tz = -25;
    const bool parsed = Clock::parseTimeZone(tzText, tz, error);
    const int minutes = parsed ? tzHoursToMinutes(tz) : 0;
    const bool typedOk = parsed && tzCustomOk(minutes);
    /* A named zone never reads the typed offset, so a field left empty or wrong does not stop it
     * being saved (the old typed offset stays); Auto and Custom need a good one. */
    if (!typedOk && mode != TZ_MODE_NAMED) {

      // Error message
      errorLabel->setText(parsed ? "Offset: -12 to +14 hours" : error);

    } else {

      if (typedOk) {
        /* Save user input: configs.ini as always (older firmware reads it), and tz_service's NVS.
         * NVS takes it either way; tzSvcIniStored only when the file write worked, so a failed
         * write leaves NVS the newer one and the boot keeps it (tz_service.h, THE UPGRADE). */
        ini["time"]["zone"] = tz;
        const bool stored = ini.store();
        tzSvcSetCustomMinutes(minutes, false);
        if (stored) {
          tzSvcIniStored(minutes);
        } else {
          log_e("TZ: configs.ini not written - the typed offset is kept in NVS (and at boot)");
        }
      }
      tzSvcSetMode(mode, named);           // saved and applied now: no reboot

      quit = true;
    }

  } else {

    // Pass button to whatever is focused

    if (focusedWidget != NULL) {
      focusedWidget->processEvent(event);
    }
    refreshHint();

  }

  return quit ? EXIT_APP : res;
}

void TimeConfigApp::redrawScreen(bool redrawAll) {
  log_d("redrawScreen TimeConfigApp");

  if (!screenInited) {
    redrawAll = true;
  }
  if (redrawAll) {
    ((GUIWidget*) clearRect)->redraw(lcd);
    ((GUIWidget*) zoneLabel)->redraw(lcd);
    ((GUIWidget*) timeZoneLabel)->redraw(lcd);
    ((GUIWidget*) ruler1)->redraw(lcd);
    ((GUIWidget*) ruler2)->redraw(lcd);
    ((GUIWidget*) creditLabel1)->redraw(lcd);
    ((GUIWidget*) creditLabel2)->redraw(lcd);
  }
  ((GUIWidget*) zoneChoice)->refresh(lcd, redrawAll);
  ((GUIWidget*) hintLabel)->refresh(lcd, redrawAll);
  ((GUIWidget*) timeZoneInput)->refresh(lcd, redrawAll);
  ((GUIWidget*) nowZoneLabel)->refresh(lcd, redrawAll);
  ((GUIWidget*) nowSourceLabel)->refresh(lcd, redrawAll);
  ((GUIWidget*) nextLabel)->refresh(lcd, redrawAll);
  ((GUIWidget*) autoLabel)->refresh(lcd, redrawAll);
  ((GUIWidget*) errorLabel)->refresh(lcd, redrawAll);

  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  ScreenConfig app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ScreenConfigApp::ScreenConfigApp(LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), FocusableApp(5), ini(Storage::ConfigsFile) {
  log_d("ScreenConfigApp");

  if (ini.load() || ini.restore()) {
    if (ini.isEmpty() || !ini[0].hasKey("v") || strcmp(ini[0]["v"], "1")) {
      log_d("unknown version or corrupt \"%s\" file", ini.filename());
    }
  } else {
    ini[0]["desc"] = "WiPhone general configs";
    ini[0]["v"] = "1";
  }
  IF_LOG(VERBOSE)
  ini.show();

  if (!ini.hasSection("screen")) {
    log_e("adding section `screen`");
    ini.addSection("screen");
    ini["screen"]["bright_level"] = "100";

    ini["screen"]["dimming"] = "1";
    ini["screen"]["dim_level"] = "15";
    ini["screen"]["dim_after_s"] = "20";

    ini["screen"]["sleeping"] = "1";
    ini["screen"]["sleep_after_s"] = "30";
  }
  if (!ini.hasSection("lock")) {
    log_e("adding section `lock`");
    ini.addSection("lock");
    ini["lock"]["lock_keyboard"] = "1";
  }

  // Create and arrange general widgets
  header->setTitle("Screen settings");
  footer->setButtons("Save", "Clear");

  clearRect = new RectWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), WP_COLOR_1);

  uint16_t yOff = header->height() + 5, rulerOff = 5;

  // Add form
  const uint16_t labelWidth = 110;
  // - brightness
  this->addInlineLabelSlider(yOff, labelWidth, brightLevelLabel, brightLevelSlider, "Brightness", 5, 100, "%", 19);
  this->addRuler(yOff, ruler1, rulerOff);

  // - dimming
  this->addInlineLabelYesNo(yOff, labelWidth, dimmingLabel, dimmingChoice, "Dim screen");
  this->addInlineLabelSlider(yOff, labelWidth, dimLevelLabel, dimLevelSlider, "Dim level", 5, 100, "%", 19);
  this->addInlineLabelInput(yOff, labelWidth, dimAfterLabel, dimAfterInput, "Dim after, s", 6, InputType::Numeric);
  this->addRuler(yOff, ruler2, rulerOff);

  // - screen sleeping & lock
  this->addInlineLabelYesNo(yOff, labelWidth, sleepingLabel, sleepingChoice, "Sleep screen");
  yOff += 1;
  this->addInlineLabelInput(yOff, labelWidth, sleepAfterLabel, sleepAfterInput, "Sleep after, s", 6, InputType::Numeric);
  yOff += 1;
  this->addInlineLabelYesNo(yOff, labelWidth, lockingLabel, lockingChoice, "Lock screen");
  this->addRuler(yOff, ruler3, rulerOff);

  this->errorLabel = new LabelWidget(0, yOff, lcd.width(), 25, "", TFT_RED, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += this->errorLabel->height();

  // Populate form
  /* 🛑 THESE DEFAULTS ARE THE BOOT LOADER'S, AND THEY DID NOT USED TO BE.
   *
   * WiPhone.ino:1492/1495/1517 fall back to 1/1/1 (and dim_level to 15) for a MISSING key.
   * This screen is a THIRD reader of the same keys and still fell back to 0/0/0 and 100 —
   * and Save at the bottom writes the widget values back, so opening Screen settings on a
   * config missing one of these keys and pressing Save PERSISTED dimming=0, sleeping=0,
   * lock_keyboard=0. The lock rides on sleep (GUI.cpp:1216 sets `locked` inside
   * SCREEN_SLEEP_EVENT), so turning sleep off silently took the lock with it.
   * The 0.9.16/0.9.17 work fixed the boot reader and the shipped config and called it
   * "all three now agree" — this reader was never swept. */
  lockingChoice->setValue((bool) ini["lock"].getIntValueSafe("lock_keyboard", 1));
  dimmingChoice->setValue((bool) ini["screen"].getIntValueSafe("dimming", 1));
  sleepingChoice->setValue((bool) ini["screen"].getIntValueSafe("sleeping", 1));

  brightLevelSlider->setValue(ini["screen"].getIntValueSafe("bright_level", 100));
  dimLevelSlider->setValue(ini["screen"].getIntValueSafe("dim_level", 15));

  dimAfterInput->setText(ini["screen"].getValueSafe("dim_after_s", ""));
  sleepAfterInput->setText(ini["screen"].getValueSafe("sleep_after_s", ""));

  // Preserve old values
  this->oldDimAfter = ini["screen"].getIntValueSafe("dim_after_s", 20);
  this->oldSleepAfter = ini["screen"].getIntValueSafe("sleep_after_s", 30);

  // Check form
  int32_t dimAfter, sleepAfter;
  this->checkForm(dimAfter, sleepAfter);

  // Focusables
  this->addFocusableWidget(brightLevelSlider);

  this->addFocusableWidget(dimmingChoice);
  this->addFocusableWidget(dimLevelSlider);
  this->addFocusableWidget(dimAfterInput);

  this->addFocusableWidget(sleepingChoice);
  this->addFocusableWidget(sleepAfterInput);
  this->addFocusableWidget(lockingChoice);

  this->setFocus(brightLevelSlider);

  this->screenInited = false;
}

ScreenConfigApp::~ScreenConfigApp() {
  log_d("destroy ScreenConfigApp");

  ini.backup();

  delete lockingLabel;
  delete lockingChoice;
  delete dimmingLabel;
  delete dimmingChoice;
  delete brightLevelLabel;
  delete brightLevelSlider;
  delete dimLevelLabel;
  delete dimLevelSlider;
  delete dimAfterLabel;
  delete dimAfterInput;
  delete sleepAfterLabel;
  delete sleepAfterInput;
  delete errorLabel;
  delete ruler1;
  delete ruler2;
  delete ruler3;
}

bool ScreenConfigApp::checkForm(int32_t &dimAfter, int32_t &sleepAfter, bool autocorrect) {
  bool correct = true;

  // - check dimAfter value
  if (dimAfterInput->getInt(dimAfter)) {
    if (dimAfter < 5) {
      errorLabel->setText("Dimming delay too small");
      correct = false;
    }
  } else {
    errorLabel->setText("Incorrect dimming delay");
    correct = false;
  }
  if (!correct && autocorrect) {
    dimAfter = oldDimAfter;
    dimAfterInput->setInt(dimAfter);
    correct = true;
  }

  // - check sleepAfter value
  if (sleepAfterInput->getInt(sleepAfter)) {
    if (sleepAfter < 5) {
      errorLabel->setText("Sleep delay too small");
      correct = false;
    } else if (correct && dimAfter > sleepAfter) {
      errorLabel->setText("Error: sleep before dimming");
      correct = false;
    }
  } else {
    errorLabel->setText("Incorrect sleep delay");
    correct = false;
  }
  if (!correct && autocorrect) {
    sleepAfter = oldSleepAfter;
    sleepAfterInput->setInt(sleepAfter);
    correct = true;
  }
  if (correct) {
    errorLabel->setText("");
  }
  return correct;
}

appEventResult ScreenConfigApp::processEvent(EventType event) {
  log_v("ScreenConfigApp::processEvent");

  bool quit = false;

  FocusableWidget* focusedWidget = this->getFocused();

  if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {

    if (focusedWidget == sleepAfterInput || focusedWidget == dimAfterInput) {
      // Enact changes of delays
      int32_t dimAfter, sleepAfter;
      bool correct = this->checkForm(dimAfter, sleepAfter);
      if (correct) {
        if (focusedWidget == dimAfterInput) {
          controlState.dimAfterMs = dimAfter*1000;
        } else if (focusedWidget == sleepAfterInput) {
          controlState.sleepAfterMs = sleepAfter*1000;
        }
      }
    }

    // Change focus
    this->nextFocus(event == WIPHONE_KEY_DOWN);

  } else if (event == WIPHONE_KEY_END) {

    // Restore values

    // - restore brightness
    int32_t brightness = ini["screen"].getIntValueSafe("bright_level", 100);
    controlState.screenBrightness = controlState.brightLevel = brightness;

#if GPIO_EXTENDER == 1509
    log_d("WIPHONE_KEY_END");
    lcdLedOnOff(true, conv100to255(brightness));
#endif // GPIO_EXTENDER == 1509

    // - restore other variables
    controlState.dimLevel = ini["screen"].getIntValueSafe("dim_level", 15);
    controlState.dimAfterMs = ini["screen"].getIntValueSafe("dim_after_s", 20)*1000;
    controlState.sleepAfterMs = ini["screen"].getIntValueSafe("sleep_after_s", 30)*1000;
    controlState.dimming = (bool)ini["screen"].getIntValueSafe("dimming", 1);
    controlState.sleeping = (bool)ini["screen"].getIntValueSafe("sleeping", 1);

    quit = true;

  } else if (LOGIC_BUTTON_OK(event)) {

    // Verify the form
    int32_t dimAfter = -1;
    int32_t sleepAfter = -1;
    bool correct = this->checkForm(dimAfter, sleepAfter, true);

    // If correct -> save
    if (correct) {
      log_v("Save button pressed");
      ini["screen"]["sleeping"] = (int32_t)sleepingChoice->getValue();
      ini["screen"]["dimming"] = (int32_t)dimmingChoice->getValue();
      ini["lock"]["lock_keyboard"] = (int32_t)lockingChoice->getValue();
      ini["screen"]["dim_level"] = (int32_t)dimLevelSlider->getValue();
      ini["screen"]["bright_level"] = (int32_t)brightLevelSlider->getValue();
      ini["screen"]["dim_after_s"] = dimAfter;
      ini["screen"]["sleep_after_s"] = sleepAfter;
      ini.store();
      quit = true;
    }

  } else {

    // Pass button to whatever is focused

    if (focusedWidget != NULL) {
      bool relevant = focusedWidget->processEvent(event);

      // Enact changes instantly

      if (relevant) {
        if (focusedWidget == brightLevelSlider) {

          // Change current screen brightness
          int32_t brightness = (int32_t)brightLevelSlider->getValue();
          controlState.screenBrightness = controlState.brightLevel = brightness;

#if GPIO_EXTENDER == 1509
          log_d("@ focusedWidget == brightLevelSlider");
          lcdLedOnOff(true, conv100to255(brightness));
#endif // GPIO_EXTENDER == 1509

        } else if (focusedWidget == dimLevelSlider) {

          // Change dim level
          controlState.dimLevel = (int32_t)dimLevelSlider->getValue();

        } else if (focusedWidget == dimmingChoice) {

          controlState.dimming = (int32_t)dimmingChoice->getValue();
          if (controlState.dimming) {
            controlState.scheduleEvent(SCREEN_DIM_EVENT, millis() + controlState.dimAfterMs);
          }

        } else if (focusedWidget == sleepingChoice) {

          controlState.sleeping = (int32_t)sleepingChoice->getValue();
          if (controlState.sleeping) {
            controlState.scheduleEvent(SCREEN_SLEEP_EVENT, millis() + controlState.sleepAfterMs);
          }

        } else if (focusedWidget == lockingChoice) {

          controlState.locking = (int32_t)lockingChoice->getValue();

        }

      }
    }

  }

  return quit ? EXIT_APP : REDRAW_SCREEN;
}

void ScreenConfigApp::redrawScreen(bool redrawAll) {
  log_d("redrawScreen ScreenConfigApp");

  if (!screenInited) {
    redrawAll = true;
  }
  if (redrawAll) {
    ((GUIWidget*) clearRect)->redraw(lcd);
    ((GUIWidget*) ruler1)->redraw(lcd);
    ((GUIWidget*) ruler2)->redraw(lcd);
    ((GUIWidget*) ruler3)->redraw(lcd);
    ((GUIWidget*) lockingLabel)->redraw(lcd);
    ((GUIWidget*) dimmingLabel)->redraw(lcd);
    ((GUIWidget*) brightLevelLabel)->redraw(lcd);
    ((GUIWidget*) dimLevelLabel)->redraw(lcd);
    ((GUIWidget*) sleepingLabel)->redraw(lcd);
    ((GUIWidget*) dimAfterLabel)->redraw(lcd);
    ((GUIWidget*) sleepAfterLabel)->redraw(lcd);
  }
  ((GUIWidget*) lockingChoice)->refresh(lcd, redrawAll);
  ((GUIWidget*) dimmingChoice)->refresh(lcd, redrawAll);
  ((GUIWidget*) sleepingChoice)->refresh(lcd, redrawAll);
  ((GUIWidget*) brightLevelSlider)->refresh(lcd, redrawAll);
  ((GUIWidget*) dimLevelSlider)->refresh(lcd, redrawAll);
  ((GUIWidget*) dimAfterInput)->refresh(lcd, redrawAll);
  ((GUIWidget*) sleepAfterInput)->refresh(lcd, redrawAll);
  ((GUIWidget*) errorLabel)->refresh(lcd, redrawAll);

  this->screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Networks app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

NetworksApp::NetworksApp(LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), ini(Networks::filename) {
  log_i("create NetworksApp");

  this->loadIni();

  // Create and arrange general widgets
  menu = NULL;
  editNetwork = NULL;

  /* ⚠ 5 s, not 1 s. Every one of these ticks DELETES AND REBUILDS the whole MenuWidget and
   * kicks a fresh WiFi scan, and both come out of the internal heap. Measured on hardware:
   * `largest` fell 17,480 -> 13,312 while this screen was open. It is churn rather than a
   * leak — scanNetworks() frees the previous results itself (WiFiScan.cpp:68), so an earlier
   * guess that a scanDelete() was missing here was WRONG — but repeatedly freeing and
   * reallocating a MenuWidget and a scan-result array is exactly what fragments this phone
   * into a reset_reason=4 panic.
   *
   * 1000 ms was never achievable anyway: the rescan below asks for **750 ms per channel**, so
   * a scan cannot possibly finish between ticks. Nothing is lost by asking less often — WiFi
   * networks do not appear and vanish within a second — and the list flickers less. */
  controlState.msAppTimerEventLast = millis();
  controlState.msAppTimerEventPeriod = 5000;

  /* 🛑 A KOSync SYNC WINDOW ENDS HERE (0.9.79). This screen scans every 5 s and its edit
   * screen joins networks; under a live 'WiPhone-Books' hotspot that is AP+STA channel
   * hopping (the X4 loses the phone mid-sync) and, on a join, a station connect under a live
   * softAP — what WiPhone.ino's reconnect gate calls a chip panic. Closing it here, and in
   * EditNetworkApp's constructor (its other entry point, GUI_APP_EDITWIFI), and refusing a
   * new one while either screen is up (kosyncWindowOpen) covers every scan and join these
   * screens make. */
  kosyncWindowClose("Settings > WiFi opened");
  wifiScreenStopsHotspotUploader("Settings > WiFi");

  // Start ASYNC scan
  log_v("scanning");
  wifiState.disconnect();   // disconnect and do not reconnect        TODO: after connecting to a network, don't go back to scanning view
  delay(100);

  screenInited = false;

  setHeaderFooter();
}

NetworksApp::~NetworksApp() {
  log_v("destroy NetworksApp");
  if (menu) {
    delete menu;
  }
  if (editNetwork) {
    delete editNetwork;
  }
  /* THE PEEK WART (2026-08-27): the constructor's disconnect() clears `reconnect` so a
   * background rejoin cannot race the user's own choice while the list is open — but
   * leaving WITHOUT joining used to leave it cleared, so merely LOOKING at the WiFi list
   * silently killed auto-rejoin (and the radio) until the next join or reboot. Put it
   * back on the way out. Gated on stationAllowed(): an explicit Disconnect / Remove /
   * WIFI-OFF in this visit went through disable(), which now sets userDisabled live — the
   * user said off, and backing out of the screen must not overrule them. A join needs no
   * help here: connectTo() already restored both flags.
   * ⚠ BOTH switches (0.9.79), not userDisabled() alone: with WiFi off, an Edit > Save clears
   * userDisabled from the INI (loadPreferred) while the switch stays off, and this re-armed the
   * retry, which joined within 20 s under a menu row that still said "off". */
  if (wifiState.stationAllowed()) {
    wifiState.resumeReconnect();
  }
  /* ...and the radio goes back to how its owner has it (0.9.79). This screen's rescans start
   * the station (scanNetworks -> enableSTA) on a phone whose network is Disconnected, or that
   * has none saved — which is how that phone FINDS one, so they stay — but nothing put it down
   * again: the station stayed up idle for the rest of the boot. The helper says OFF there,
   * and rejoins (or leaves an association alone) for a phone with WiFi on. */
  wifiRestoreStation("left Settings > WiFi");
}

void NetworksApp::setHeaderFooter() {
  header->setTitle("Networks");
  footer->setButtons("Select", "Back");
}

void NetworksApp::loadIni() {
  ini.unload();
  if (ini.load() || ini.restore()) {
    if (ini.isEmpty() || !ini[0].hasKey("v") || strcmp(ini[0]["v"], "1")) {
      log_e("unknown version or corrupt \"%s\" file", ini.filename());
    }
  } else {
    ini[0]["desc"] = "WiPhone WiFi networks";
    ini[0]["v"] = "1";
  }
  IF_LOG(VERBOSE)
  ini.show();
}

appEventResult NetworksApp::processEvent(EventType event) {
  log_v("processEvent NetworksApp");

  appEventResult res = DO_NOTHING;
  if (editNetwork != NULL) {

    // Editing a network

    if (event == APP_TIMER_EVENT) {

      int16_t rn = WiFi.scanComplete();
      if (rn != WIFI_SCAN_RUNNING) {
        log_v("scan complete: %d", rn);
        WiFi.scanDelete();
        controlState.msAppTimerEventPeriod = 0;     // unsubscribe from periodic "anyevent"
      }

    } else if ((res |= editNetwork->processEvent(event)) & EXIT_APP) {      // passing event to EditNetwork
      log_v("exited from EditNetwork");
      delete editNetwork;
      editNetwork = NULL;
      screenInited = false;
      delete menu;
      menu = NULL;
      controlState.msAppTimerEventPeriod = 1000;    // resume periodic checks
      this->loadIni();
      setHeaderFooter();
      res = (res | REDRAW_ALL) & ~(appEventResult)EXIT_APP;
    }

  } else if (LOGIC_BUTTON_BACK(event)) {

    res |= EXIT_APP;

  } else if (event == APP_TIMER_EVENT) {

    log_v("processing timer event");
    int16_t rn = WiFi.scanComplete();
    log_v("networks: ", rn, DEC);
    if (rn>=0) {
      // Remember currently selected network
      char* selected = NULL;
      if (menu!=NULL) {
        selected = (char *) menu->getSelectedTitle();
        if (selected!=NULL) {
          log_v("selected: ", selected);
          selected += strcspn(selected, ")") + 2;       // TRICKY: skip the RSSI prefix
          selected = strdup(selected);
        } else {
          log_v("nothing selected");
        }
      }

      // Create new menu
      if (menu!=NULL) {
        delete menu;
      }
      menu = new MenuWidget(0, header->height()+menuTopPadding, lcd.width(), lcd.height() - header->height() - footer->height() - menuTopPadding,
                            "No networks", fonts[OPENSANS_COND_BOLD_20], N_MAX_ITEMS, 1);
      menu->setStyle(MenuWidget::ALTERNATE_STYLE, WP_ACCENT_1, WP_COLOR_1, WP_COLOR_0, WP_ACCENT_0);
      char text[50];

      // Populate menu with new data
      for (uint16_t i=0; i<rn; i++) {
        String ssid = WiFi.SSID(i);
        bool openNetwork = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);        // network doesn't require a password
        bool knownNetwork = (ini.query("s", ssid.c_str()) >= 0);              // find a section with field "s" (SSID) equal to the current network SSID
        uint16_t len = snprintf(text, sizeof(text), "%c (%d) ", (knownNetwork || openNetwork) ? '+' : ' ', WiFi.RSSI(i));
        if (sizeof(text)>len+1)
          // Add SSID if fits
        {
          snprintf(text + len, sizeof(text) - len, "%s", ssid.c_str());
        }
        log_v("adding option: %s", text);
        bool connectedNetwork = false;
        if (wifiState.isConnected() && wifiState.ssid() != NULL && !strcmp(wifiState.ssid(), ssid.c_str())) {
          connectedNetwork = true;
        }
        menu->addOption(text, i+1, connectedNetwork ? 2 : 1);

        // Restore selection
        if (selected!=NULL && !strcmp(ssid.c_str(), selected)) {
          menu->selectLastOption();
        }
      }
      log_v("deleting selected");
      freeNull((void **) &selected);
      res |= REDRAW_SCREEN;
    } else if (rn == WIFI_SCAN_FAILED) {
      log_e("ERROR: scanning networks failed");
    }
    if (rn > 0) {
      wifiScanNoteResult(rn);       // this screen sees the crowded-air case first
    }

    // Rescan
    log_v("rescanning");
    /* Guarded like every other scan site: _scanDone()'s `new wifi_ap_record_t[n]` throws on
     * a fragmented heap and aborts the phone from the WiFi event task. This screen re-scans
     * on a loop while it is open, so it is the site a user can hold open the longest. When
     * refused the list simply keeps the results it already has. See helpers.h. */
    if (wifiScanMemoryOk("wifi screen")) {
      /* ⚠ scanNetworks() opens with enableSTA(true) -> esp_wifi_start(), so this rescan
       * restarted a radio the user had switched off — the third of the three paths found in
       * the 2026-09-01 audit. Someone standing on the WiFi list screen with the radio off is
       * shown the list they already have rather than being silently put back on air. */
      /* (0.9.79) Nor under a live hotspot: the constructor stopped any that was up, but serial
       * `up on` can start one while this screen is open, and a scan under it is AP+STA channel
       * hopping in the middle of someone's upload. */
      if (!wifiState.radioOff() && !(xferServing() && xferUsingAP())) {
        WiFi.scanNetworks(true, false, false, 750);
      }
    }

  } else if (IS_KEYBOARD(event) && menu!=NULL) {

    log_v("menu process");
    menu->processEvent(event);
    const char* chosen = menu->readChosenTitle();
    res |= REDRAW_SCREEN;
    if (chosen != NULL) {
      log_v("chosen title: %s", chosen);
      editNetwork = new EditNetworkApp(lcd, controlState, chosen+strcspn(chosen, ")")+2, header, footer);    // TRICKY: skipping RSSI prefix
      res |= REDRAW_ALL;
    }
  }

  return res;
}

void NetworksApp::redrawScreen(bool redrawAll) {
  log_v("redrawScreen NetworksApp");

  if (editNetwork != NULL) {

    editNetwork->redrawScreen(redrawAll);

  } else {

    if (!screenInited || redrawAll) {
      GUIWidget::corrRect(lcd, 0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), BLACK);
      lcd.setTextColor(THEME_TEXT_COLOR, THEME_BG);
      lcd.setTextFont(fonts[OPENSANS_COND_BOLD_20]);
      lcd.setTextDatum(TL_DATUM);
      lcd.drawString("Scanning...", 1, header->height() + menuTopPadding);
    }
    if (menu != NULL) {
      ((GUIWidget*) menu)->redraw(lcd);
    }

    screenInited = true;

  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Circle app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

CircleApp::CircleApp(LCD& lcd, ControlState& state) : WiPhoneApp(lcd, state) {
  log_d("create CircleApp");

  anyPressed = false;

  SmoothFont* font = fonts[OPENSANS_COND_BOLD_20];
  lcd.fillScreen(BLACK);
  lcd.setTextColor(WHITE, BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TC_DATUM);
  lcd.drawString("Circles App", lcd.width()/2, (lcd.height()-font->height())/2);
}

CircleApp::~CircleApp() {
  log_d("destroy CircleApp");
}

appEventResult CircleApp::processEvent(EventType event) {
  log_d("processEvent CircleApp");
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }
  anyPressed = IS_KEYBOARD(event) ? true : false;
  return REDRAW_SCREEN;
}

void CircleApp::redrawScreen(bool redrawAll) {
  log_i("redraw CircleApp");

  // Draw random circles
  if (anyPressed) {
    anyPressed = false;

    uint32_t r = Random.random();     // global random number generator

    uint16_t x = r % lcd.width();
    uint16_t y = (r>>8) % lcd.height();
    uint16_t rad = 1 + ((r>>16) % 64);
    uint16_t col = r >> 3;

    // Make sure the colors are bright
    if (GETRED(col) < 4 && GETGREEN(col) < 4 && GETBLUE(col) < 4) {
      col = BLACK;
    } else if (GETRED(col) < GETGREEN(col)) {
      if (GETRED(col) < GETBLUE(col)) {
        col |= RED;
      } else {
        col |= BLUE;
      }
    } else {
      if (GETGREEN(col) < GETBLUE(col)) {
        col |= GREEN;
      } else {
        col |= BLUE;
      }
    }

    log_d("x = ", x);
    log_d("y = ", y);
    log_d("r = ", rad);
    log_d("color = ", col);

    lcd.fillCircle(x, y, rad, col);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Widgets Demo app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

WidgetDemoApp::WidgetDemoApp(LCD& lcd, ControlState& state) : WiPhoneApp(lcd, state) {
  log_d("create WidgetDemoApp");

  widgets[0] = new RectWidget(0, 0, lcd.width(), THEME_HEADER_SIZE, GREEN);
  widgets[1] = new RectWidget(0, THEME_HEADER_SIZE, lcd.width(), lcd.height() - THEME_HEADER_SIZE - THEME_FOOTER_SIZE, BLUE);
  widgets[2] = new RectWidget(0, lcd.height() - THEME_FOOTER_SIZE, lcd.width(), THEME_FOOTER_SIZE, RED);

  label = new LabelWidget(0, (lcd.height()-fonts[AKROBAT_EXTRABOLD_22]->height())/2, lcd.width(), fonts[AKROBAT_EXTRABOLD_22]->height(),
                          "Demo", WHITE, BLACK, fonts[AKROBAT_EXTRABOLD_22], LabelWidget::CENTER);
}

WidgetDemoApp::~WidgetDemoApp() {
  log_d("destroy WidgetDemoApp");

  delete widgets[0];
  delete widgets[1];
  delete widgets[2];
  delete label;
}

appEventResult WidgetDemoApp::processEvent(EventType event) {
  log_d("processEvent WidgetDemoApp");
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }
  return REDRAW_SCREEN;
}

void WidgetDemoApp::redrawScreen(bool redrawAll) {
  log_i("redraw WidgetDemoApp");
  widgets[0]->redraw(lcd);
  widgets[1]->redraw(lcd);
  widgets[2]->redraw(lcd);

  ((GUIWidget *) label)->redraw(lcd);

  lcd.setTextDatum(TL_DATUM);

  lcd.setSmoothTransparency(true);
  lcd.setTextColor(WHITE, BLUE);
  lcd.setTextFont(fonts[AKROBAT_BOLD_16]);
  // WARNING: the following might produce deformed text due to hardware problems while drawing horizontal 1px lines
  // SOLUTIONS: 1) use only non-transparent font so that window is never set to 1px height (as in the following)
  //            2) use sprites
  //            3) use SPI_MODE3
  lcd.drawString("Quick lazy", 20, 43);
  lcd.setSmoothTransparency(false);
  lcd.drawString("Quick lazy", 20, 43 + lcd.fontHeight() + 5);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Pictures Demo app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

PicturesDemoApp::PicturesDemoApp(LCD& lcd, ControlState& state) : WiPhoneApp(lcd, state) {
  log_d("create PicturesDemoApp");
  pic = 1;
}

PicturesDemoApp::~PicturesDemoApp() {
  log_d("destroy PicturesDemoApp");
}

appEventResult PicturesDemoApp::processEvent(EventType event) {
  log_d("processEvent PicturesDemoApp");
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }
  pic++;
  if (pic>3) {
    pic = 1;
  }
  return REDRAW_SCREEN;
}

void PicturesDemoApp::redrawScreen(bool redrawAll) {
  log_i("redraw PicturesDemoApp");
  int t = micros();
  if (pic==1) {
    lcd.drawImage(rle3_image, sizeof(rle3_image), 0, 0);
  } else if (pic==2) {
    lcd.drawImage(image_i256, sizeof(image_i256), 0, 0);
  } else if (pic>2) {
    lcd.drawImage(image_jpg, sizeof(image_jpg));
  } else {
    lcd.fillScreen(BLACK);
    lcd.setTextColor(RED, BLACK);
    lcd.setTextSize(2);
    lcd.drawString("Error", lcd.width()/2, lcd.height()/2, 2);
  }
  log_d("time: %lu", micros()-t);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Font Demo app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FontDemoApp::FontDemoApp(LCD& lcd, ControlState& state) : WiPhoneApp(lcd, state) {
  log_d("create FontDemoApp");
  log_d("font index: ", curFontIndex);
  lcd.setTextFont(fonts[curFontIndex]);
}

FontDemoApp::~FontDemoApp() {
  log_d("destroy FontDemoApp");
}

appEventResult FontDemoApp::processEvent(EventType event) {
  log_d("processEvent FontDemoApp");
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }
  if (!smooth) {
    smooth = true;
  } else {
    curFontIndex = (curFontIndex + 1) % fonts.length();
    smooth = false;
  }
  log_d("font Index: %d", curFontIndex);
  return REDRAW_SCREEN;
}

void FontDemoApp::redrawScreen(bool redrawAll) {
  log_i("redraw FontDemoApp");
  lcd.fillScreen(BLACK);
  uint32_t t = micros();
  if (smooth) {
    lcd.setTextFont(fonts[curFontIndex]);
  } else {
    lcd.setTextFont(curFontIndex);
  }
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setCursor(0,40);

  char str[2] = "\0";
  for(uint8_t c=0x21; c<0x7E; c++) {
    str[0] = c;
    lcd.print(str);
  }
  t = micros()-t;
  lcd.setCursor(0,200);
  lcd.print("Time: ");
  lcd.println(t);
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Clock app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ClockApp::ClockApp(LCD& lcd, TFT_eSprite& bgImg, ControlState& state) : WiPhoneApp(lcd, state), bgImg(bgImg) {
  log_d("create ClockApp");
}

ClockApp::~ClockApp() {
  log_d("destroy ClockApp");
}


appEventResult ClockApp::processEvent(EventType event) {
  //log_d("processEvent ClockApp");
  //LOG_MEM_STATUS;
  if (LOGIC_BUTTON_BACK(event) || LOGIC_BUTTON_OK(event) || event==WIPHONE_KEY_DOWN || event==WIPHONE_KEY_UP) {
    return EXIT_APP;
  }
  if (controlState.locked) {
    return DO_NOTHING;
  }
  if (NONKEY_EVENT_ONE_OF(event, TIME_UPDATE_EVENT | WIFI_ICON_UPDATE_EVENT | BATTERY_UPDATE_EVENT | REGISTRATION_UPDATE_EVENT | BATTERY_BLINK_EVENT | USB_UPDATE_EVENT)) {
    // TODO: optimize this app to only redraw small portion on events like WIFI_ICON_UPDATE_EVENT, BATTERY_UPDATE_EVENT, REGISTRATION_UPDATE_EVENT, BATTERY_BLINK_EVENT
    return REDRAW_SCREEN;
  } else if (event == NEW_MESSAGE_EVENT && !messageIconShown) {
    // Redraw screen only if message icon is not shown yet, but a new message was received
    return REDRAW_SCREEN;
  }
  if (event == '#' && (controlState.unreadMessages || controlState.meshUnread)) {
    /* The envelope is showing and # is not a digit anyone starts a number with: one press
     * from the clock straight to Messages, instead of Menu > WiPhone > Messages. Sits
     * below the locked guard like the dial shortcut, so a pocket cannot trigger it. */
    return EXIT_APP | ENTER_MESSAGES_APP;
  }
  if (event >= '0' && event <='9' || event == '*' || event == '#') {
    return EXIT_APP | ENTER_DIAL_APP;
  }
  return DO_NOTHING;

}

/* One line of centred text on the wallpaper, with a plate sized to the words.
 *
 * ⚠ `bottomY` is a BC_DATUM baseline — the bottom of the text, which is how the clock face
 * places everything. Metrics come from whatever font is currently set, so set it first. */
static void clockScrimLine(LCD &lcd, const char* s, int16_t cx, int16_t bottomY) {
  if (!s || !*s) {
    return;
  }
  const uint16_t padX = 8, padY = 3;
  const uint16_t fh = lcd.fontHeight();
  uint16_t w = lcd.textWidth(s) + 2 * padX;
  if (w > lcd.width()) {
    w = lcd.width();
  }
  int16_t x = (int16_t)cx - (int16_t)(w >> 1);
  int16_t y = bottomY - (int16_t)fh - (int16_t)padY;
  if (x < 0) {
    x = 0;
  }
  if (y < 0) {
    y = 0;
  }
  guiDrawScrim(lcd, x, y, w, fh + 2 * padY);
}

void ClockApp::redrawScreen(bool redrawAll) {
  //log_i("redraw ClockApp");

  // Paste background image
  if (bgImg.isCreated() && lcd.isSprite()) {
    bgImg.cloneDataInto((TFT_eSprite*) &lcd);
  } else {
    // Clear screen
    lcd.fillScreen(THEME_BG);
  }

  // Draw icons
  GUI::drawWifiIcon(lcd, controlState, 3, 5);
  auto w = GUI::drawSipIcon(lcd, controlState, 24, 5);
  const uint16_t mw = GUI::drawMessageIcon(lcd, controlState, 26 + w, 5);
  messageIconShown = mw > 0;
  /* The crossed speaker, as the header shows it on every other screen. The clock draws its
   * own row, so it has to ask too — and this is the screen a held '#' mutes from. */
  GUI::drawMuteIcon(lcd, controlState, 26 + w + mw + (mw ? 4 : 0), 4);
  {
    /* The number next to the icon, on the clock only. Five coarse icon levels meant "will
     * it last the evening?" had no answer anywhere in the normal UI — the exact question
     * a day in the field keeps asking. */
    uint16_t bw = GUI::drawBatteryIcon(lcd, controlState, -1, lcd.width()-3, 7);
    if (bw > 0 && controlState.battVoltage > 0) {
      char soc[8];
      snprintf(soc, sizeof(soc), "%d%%", (int)(controlState.battSoc + 0.5f));
      lcd.setTextFont(fonts[AKROBAT_BOLD_16]);
      lcd.setTextDatum(TR_DATUM);
      lcd.setTextColor(WHITE, BLACK);
      // Transparent glyphs, like the clock text below: opaque smooth-font cells would
      // paint a black box over the wallpaper this screen draws behind everything.
      lcd.setSmoothTransparency(true);
      lcd.drawString(soc, lcd.width() - 3 - bw - 4, 7);
      lcd.setSmoothTransparency(false);
    }
  }

  // Print clock and date
  uint16_t yOff = 158;
  uint16_t cx = lcd.width()>>1;
  lcd.setTextDatum(BC_DATUM);
  lcd.setTextColor(WHITE, BLACK);
  lcd.setSmoothTransparency(true);

  /* ⚠ ONE PLATE FOR BOTH LINES, NOT ONE EACH. The date sits directly under the clock and the
   * two bands overlap by a pixel or two — and two translucent plates over the same pixel blend
   * TWICE, which paints a visibly darker stripe exactly where the eye is drawn. So the strings
   * are measured first, one union rect is laid down, and only then is anything drawn.
   *
   * {user} asked for this after the menus (2026-08-25): the clock face draws straight onto the
   * wallpaper, and over a bright photo "Network: waiting NTP" was close to invisible. Only the
   * words are covered — a plate across the whole idle screen would defeat the point of picking
   * a picture at all. */
  char bigLine[20], subLine[40];
  if (ntpClock.isTimeKnown()) {
    snprintf(bigLine, sizeof(bigLine), "%02d:%02d", ntpClock.getHour(), ntpClock.getMinute());
    snprintf(subLine, sizeof(subLine), "%d %s %d", ntpClock.getDay(), ntpClock.getMonth3(), ntpClock.getYear());
  } else {
    snprintf(bigLine, sizeof(bigLine), "00:00");
    snprintf(subLine, sizeof(subLine), "Network: waiting NTP");
  }
  lcd.setTextFont(fonts[AKROBAT_BOLD_90]);
  const uint16_t clockH = lcd.fontHeight();
  const uint16_t clockW = lcd.textWidth(bigLine);
  lcd.setTextFont(fonts[AKROBAT_BOLD_24]);
  const uint16_t subH = lcd.fontHeight();
  const uint16_t subW = lcd.textWidth(subLine);
  const uint16_t subBottom = yOff + subH + 3;
  {
    const uint16_t padX = 8, padY = 3;
    uint16_t w = (clockW > subW ? clockW : subW) + 2 * padX;
    if (w > lcd.width()) {
      w = lcd.width();
    }
    int16_t x = (int16_t)cx - (int16_t)(w >> 1);
    int16_t top = (int16_t)yOff - (int16_t)clockH - (int16_t)padY;
    if (x < 0) {
      x = 0;
    }
    if (top < 0) {
      top = 0;
    }
    guiDrawScrim(lcd, x, top, w, (uint16_t)((int16_t)subBottom + (int16_t)padY - top));
  }
  lcd.setTextFont(fonts[AKROBAT_BOLD_90]);
  lcd.drawString(bigLine, cx, yOff);
  lcd.setTextFont(fonts[AKROBAT_BOLD_24]);
  lcd.drawString(subLine, cx, subBottom);
  yOff += lcd.fontHeight()+21;

  // Draw lock icon
  if (controlState.locked) {
    IconRle3 *iconObj = new IconRle3(icon_lock, sizeof(icon_lock));
    lcd.drawImage(*iconObj, cx-iconObj->width()/2, yOff);
    delete iconObj;
  }

  /* A missed call announces itself where eyes actually land, and stays until you act:
   * opening the dialer or the phonebook clears it (see enterApp). Amber, BELOW the lock
   * icon's row (review caught the first draft painting the icon over the name on a locked
   * phone - the normal state a call gets missed in), fitted so a long URI cannot run off
   * both screen edges, and the ambient font is untouched by the time yOff was advanced. */
  if (missedCallCount() > 0) {
    char mline[96];
    if (missedCallCount() == 1) {
      snprintf(mline, sizeof(mline), "Missed call: %s", missedCallName());
    } else {
      snprintf(mline, sizeof(mline), "%d missed calls (last: %s)",
               missedCallCount(), missedCallName());
    }
    lcd.setTextFont(fonts[AKROBAT_BOLD_20]);
    lcd.setTextColor(TFT_ORANGE, BLACK);
    clockScrimLine(lcd, mline, cx, yOff + 64);      // amber on a bright photo is no better off
    lcd.drawFitString(mline, lcd.width() - 12, cx, yOff + 64);
    lcd.setTextColor(WHITE, BLACK);
  }

  // Print center button text
  const char* msg = "Menu";
  if (controlState.locked) {
    // Locked text
    msg = (controlState.unlockButton1 == WIPHONE_KEY_OK) ? "Press * to unlock" : "Locked. Press OK";
  }
  /* The softkey label is on the wallpaper too, and fixing two of the three lines on one screen
   * would just move the complaint. ⚠ Its FONT is whatever the block above left set — 20pt when
   * a missed call is showing, 24pt otherwise. Pre-existing and cosmetic; measuring the scrim
   * from the current font means this follows it either way rather than guessing. */
  clockScrimLine(lcd, msg, cx, lcd.height()-7);
  lcd.drawString(msg, cx, lcd.height()-7);
  lcd.setSmoothTransparency(false);
  //log_v("- done");

}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Splash screen app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

SplashApp::SplashApp(LCD& lcd, ControlState& state) : WiPhoneApp(lcd, state) {
  // Subscribe to app timer event
  controlState.msAppTimerEventLast = millis();
  controlState.msAppTimerEventPeriod = 300;
}

SplashApp::~SplashApp() {}

appEventResult SplashApp::processEvent(EventType event) {
  log_d("processEvent SplashApp");
  if (LOGIC_BUTTON_BACK(event) || LOGIC_BUTTON_OK(event)) {
    return EXIT_APP;
  }
  if (event==APP_TIMER_EVENT) {
    return ++screenNo>=8 ? EXIT_APP : REDRAW_SCREEN;
  }
  return DO_NOTHING;
}

void SplashApp::redrawScreen(bool redrawAll) {
  // TODO: blink the LED from here
  IconRle3 *icon;
  uint8_t screen = screenNo % 4;

  // Draw base icon
  if (screen == 0) {
    icon = new IconRle3(icon_splash_base, sizeof(icon_splash_base));
    lcd.drawImage(*icon, 0, 0);
    // Version
    lcd.setTextColor(TFT_BLUE, TFT_WHITE);
    lcd.setTextDatum(TL_DATUM);
    lcd.setTextSize(1);
    lcd.setTextFont(1);
    lcd.drawString("Custom firmware by {user} H.", 5, 298);   // see FIRMWARE_VERSION's "-NH"
    lcd.drawString("Ver. " FIRMWARE_VERSION, 5, 310);
    delete icon;
  }

  // Draw wifi icon
  if (screen == 0) {
    icon = new IconRle3(icon_splash_1, sizeof(icon_splash_1));
  } else if (screen == 1) {
    icon = new IconRle3(icon_splash_2, sizeof(icon_splash_2));
  } else if (screen == 2) {
    icon = new IconRle3(icon_splash_3, sizeof(icon_splash_3));
  } else {
    icon = new IconRle3(icon_splash_4, sizeof(icon_splash_4));
  }
  lcd.drawImage(*icon, (lcd.width()-icon->width())>>1, 98);
  delete icon;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Messages app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

MessagesApp::MessagesApp(LCD& lcd, ControlState& state, Storage& flash, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), flash(flash) {
  log_d("create MessagesApp");

  // Load messages database
  if (!flash.messages.isLoaded()) {
    flash.messages.load(ntpClock.msgStamp());   // UTC, and mesh = provisional: see GUI::reloadMessages()
  }
  /* And the phonebook, because the rows are labelled with NAMES where there are any.
   * Here rather than at the first label lookup: this is already the screen's slow
   * moment, and loading it lazily would put a SPIFFS read inside a menu redraw.
   * Failure is not fatal — sipContactName() then finds nothing and the address shows,
   * exactly as it did before names existed. */
  if (!flash.phonebook.isLoaded()) {
    flash.loadPhonebook();
  }

  chatsMenu = NULL;
  threadText = NULL;
  subApp = NULL;

  this->buildChats();
  enterState(CHATS);
}

MessagesApp::~MessagesApp() {
  log_d("destroy MessagesApp");

  if (chatsMenu) {
    delete chatsMenu;
  }
  if (threadText) {
    delete threadText;
  }
  if (subApp) {
    delete subApp;
  }
  // The open thread holds a PSRAM block for its text; nothing else will free it.
  sipThreadsRelease();
}

void MessagesApp::enterState(MessagesState_t state) {
  log_i("enterState %d", (int)state);
  if (state == CHATS) {
    header->setTitle("Messages");
    footer->setButtons("Open", "Back");
  } else if (state == THREAD) {
    /* The correspondent is the title: in a thread, who you are talking to is
     * the context. threadLabel is their phonebook name where there is one, else
     * their address — a phone number's `threadPeer` (bare digits) is the nicer
     * form, but for a digit-free SIP address it is the identity string, which
     * identityOf() truncated to 19 chars, so that case shows the FULL uri and
     * lets the header ellipsize it honestly. (Widening the identity itself
     * would cost static RAM in the thread table and the mirror record.)
     * ⚠ setTitle keeps the POINTER: it must be threadLabel, which outlives this
     * call, and never sipDisplayLabel()'s return, which can point into the
     * phonebook. */
    header->setTitle(threadLabel[0] ? threadLabel : "Messages");
    footer->setButtons("Reply", "Back");
  }
  appState = state;
}

/* Mark every unread INCOMING message in the open thread as read.
 *
 * ⚠ Each setRead() locates the message in its partition and rewrites the file, so this is a
 * handful of SD writes — bounded by how many were unread, which is small in practice.
 * Done on OPEN rather than per-message, because a conversation view shows the messages; there
 * is no separate "I read this one" moment to hang it on. */
void MessagesApp::markThreadRead() {
  if (!threadPeer[0] || !flash.messages.isLoaded()) {
    return;
  }
  const int32_t total = flash.messages.inboxTotalSize();
  if (total <= 0) {
    return;
  }
  /* Full depth while ANYTHING is unread, capped otherwise. The cap alone left a
   * trap: a re-mirrored history buries its unread flags deeper than 120, so the
   * white icon stayed lit and opening the thread could never clear it — "new
   * message" with nothing visibly new. Scanning the whole inbox costs seconds
   * once, only while unread exist; every later open is capped and fast again. */
  int32_t depth = flash.messages.hasUnread()
                  ? total
                  : (total < SIP_THREADS_SCAN ? total : SIP_THREADS_SCAN);

  /* ⚠ PAGED, for the same reason sip_threads.cpp's scans are — and this was the call site
   * that still crashed after those were fixed. `preload()` DEEP-COPIES each message's INI
   * section into internal RAM; asking for 120 at once meant 120 live copies, and the
   * allocation behind them (`operator new`) THROWS on failure, which nothing catches:
   * terminate() -> abort() -> reset_reason=4. Decoded from a real backtrace, twice.
   *
   * ⚠ IF YOU ADD ANOTHER preload() CALLER, PAGE IT. The whole-window form is a trap: it
   * works on a small store and aborts on a big one, so it survives testing and dies in use. */
  const int32_t PAGE = 10;
  bool any = false;
  for (int32_t done = 0; done < depth; done += PAGE) {
    const int32_t cnt = (depth - done < PAGE) ? (depth - done) : PAGE;
    const int32_t off = -1 - done;
    flash.messages.clearPreloaded();
    flash.messages.preload(INCOMING, off, cnt);
    for (auto it = flash.messages.iteratorCount(off, cnt); it.valid(); ++it) {
      MessageData& m = *it;
      if (m.isRead()) {
        continue;
      }
      /* ONE identity rule, shared with the thread builder. This was a private
       * copy of it; the two had to agree exactly or messages here would never
       * match the open thread and would never be marked read. */
      char id[SMS_MIRROR_PEER_MAX];
      sipThreadIdentity(m.getOtherUri(), id, sizeof(id));
      if (strcmp(id, threadPeer)) {
        continue;
      }
      flash.messages.setRead(m);
      any = true;
    }
  }
  flash.messages.clearPreloaded();
  if (any) {
    controlState.unreadMessages = flash.messages.hasUnread();
  }
}

void MessagesApp::openThread(const char* peer, const char* uri) {
  snprintf(threadPeer, sizeof(threadPeer), "%s", peer ? peer : "");
  snprintf(threadUri, sizeof(threadUri), "%s", uri ? uri : "");
  /* Resolve the name ONCE, here, rather than at each of the places that draw it:
   * the header and every incoming line in buildThread() want the same answer, and
   * buildThread() runs again on every new message. */
  snprintf(threadLabel, sizeof(threadLabel), "%s", sipDisplayLabel(flash, threadPeer, threadUri));
  this->markThreadRead();
  this->buildThread();
  enterState(THREAD);
}

appEventResult MessagesApp::processEvent(EventType event) {
  log_i("processEvent MessagesApp %d", event);

  appEventResult res = REDRAW_SCREEN;

  if (event == NEW_MESSAGE_EVENT) {

    /* Rebuild whichever list is on screen. Both are snapshots (see sip_threads.h), so a new
     * message does not appear until one is rebuilt — this is that moment. */
    this->buildChats();
    if (appState == THREAD) {
      this->buildThread();
    }
    res |= REDRAW_ALL;

    // We assume that header will be redrawn by GUI class for this event.

  } else if (subApp) {
    if ((res = subApp->processEvent(event)) & EXIT_APP) {
      /* The composer can exit asking for a conversation to be OPENED rather than started:
       * you picked someone from the phonebook you already have a thread with. Copied out
       * before the delete — the strings belong to the sub-app. */
      char jumpPeer[SIP_THREAD_URI_MAX] = {0};
      char jumpUri[SIP_THREAD_URI_MAX]  = {0};
      if (subApp->getId() == GUI_APP_CREATE_MESSAGE) {
        CreateMessageApp* composer = (CreateMessageApp*)subApp;
        snprintf(jumpPeer, sizeof(jumpPeer), "%s", composer->getJumpPeer());
        snprintf(jumpUri,  sizeof(jumpUri),  "%s", composer->getJumpUri());
      }

      /* Whatever the sub-app did — sent a message, deleted one — the snapshots are now
       * stale. Rebuilding both is cheap next to being wrong. */
      flash.messages.clearPreloaded();
      this->buildChats();

      delete subApp;
      subApp = NULL;

      if (jumpPeer[0]) {
        /* Straight into that conversation. Back from it belongs on the chats list, which
         * is where New Message was pressed, so returnState stays CHATS and the row is
         * selected on the way past — coming back should land on the person you just
         * opened, not at the top of the list. */
        for (int i = 0; i < sipThreadsCount(); i++) {
          const SipThread* t = sipThreadAt(i);
          if (t && !strcmp(t->peer, jumpPeer)) {
            chatSelected = i;
            chatsMenu->select((MenuOption::keyType)(i + 2));
            break;
          }
        }
        this->openThread(jumpPeer, jumpUri);
        return REDRAW_ALL;
      }

      /* returnState, not appState: while the composer is up appState is COMPOSING, so the
       * old `appState == THREAD` test could never be true here and the thread was never
       * rebuilt on the way back — which is also why Reply exited to the chats list. */
      if (appState == THREAD || (appState == COMPOSING && returnState == THREAD)) {
        this->buildThread();
      }
      enterState(appState == COMPOSING ? returnState : appState);

      res = REDRAW_ALL;
    }

  } else if (appState == CHATS) {

    if (LOGIC_BUTTON_BACK(event)) {
      return EXIT_APP;
    }

    chatsMenu->processEvent(event);

    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = chatsMenu->readChosen();
      if (sel == 1) {
        /* New message: no correspondent yet, so the composer asks for one — and if the
         * one you pick already has a thread, it hands it back for us to open instead of
         * starting a duplicate conversation with the same person. */
        returnState = CHATS;
        appState = COMPOSING;
        subApp = new CreateMessageApp(lcd, controlState, flash, header, footer, NULL, true);
      } else if (sel >= 2) {
        const SipThread* t = sipThreadAt((int)sel - 2);
        if (t) {
          chatSelected = (int32_t)sel - 2;
          this->openThread(t->peer, t->uri);
        }
      }
      res |= REDRAW_ALL;
    }

  } else if (appState == THREAD) {

    if (LOGIC_BUTTON_BACK(event)) {
      /* Back goes to the chats list, and the list is rebuilt on the way: this thread's
       * unread count just went to zero, and a stale badge is exactly the kind of small lie
       * that makes a phone feel broken. */
      sipThreadsRelease();
      this->buildChats();
      enterState(CHATS);
      res |= REDRAW_ALL;
    } else if (LOGIC_BUTTON_OK(event)) {
      // Reply, pre-addressed. threadUri is a FULL URI on purpose — TinySIP::sendMessage()
      // uses the address verbatim, so a bare number would call fine and silently not text.
      returnState = THREAD;             // replying belongs to the conversation you were in
      appState = COMPOSING;
      subApp = new CreateMessageApp(lcd, controlState, flash, header, footer,
                                    threadUri[0] ? threadUri : threadPeer);
      res |= REDRAW_ALL;
    } else if (threadText && (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN)) {
      /* Scroll the conversation. Only up/down are forwarded: the widget is a text INPUT and
       * would happily let you type into a received message otherwise. */
      threadText->processEvent(event);
      res |= REDRAW_ALL;
    }

  }

  return res;
}

/* One row per correspondent, newest first, with "New Message" pinned at the top.
 *
 * Key 1 is New Message; a thread at index i is key i + 2. Nothing persists these, so the
 * numbering is free to change — but it must stay in step with processEvent(). */
void MessagesApp::buildChats() {
  MenuOption::keyType keep = chatsMenu ? chatsMenu->currentKey() : 0;
  if (chatsMenu) {
    delete chatsMenu;
  }

  chatsMenu = new MenuWidget(0, header->height(), lcd.width(),
                             lcd.height() - header->height() - footer->height(),
                             "No messages yet", fonts[AKROBAT_EXTRABOLD_22], N_MENU_ITEMS, 8);
  chatsMenu->setStyle(MenuWidget::DEFAULT_STYLE,   BLACK, GRAY_85, GRAY_95, WP_ACCENT_1);
  chatsMenu->setStyle(MenuWidget::ALTERNATE_STYLE, BLACK, WHITE,   WHITE,   WP_ACCENT_S);

  chatsMenu->addOption("New Message", NULL, 1, 1,
                       icon_Write_b, sizeof(icon_Write_b),
                       icon_Write_w, sizeof(icon_Write_w));

  const int n = sipThreadsBuild(flash.messages);
  for (int i = 0; i < n; i++) {
    const SipThread* t = sipThreadAt(i);
    if (!t) {
      continue;
    }
    char sub[40];
    if (t->unread) {
      snprintf(sub, sizeof(sub), "%u unread of %u", (unsigned)t->unread, (unsigned)t->count);
    } else {
      snprintf(sub, sizeof(sub), "%u message%s", (unsigned)t->count, t->count == 1 ? "" : "s");
    }
    /* Show the ADDRESS, not the identity: `peer` is bare digits for a phone
     * number (the nice label) but for a long SIP address it is a bounded,
     * hash-suffixed grouping key that nobody should ever read. `uri` is the
     * real thing, and the row ellipsizes it honestly. */
    const char* label = sipDisplayLabel(flash, t->peer, t->uri);
    MenuOptionIconnedTimed* option = new MenuOptionIconnedTimed(
      (MenuOption::keyType)(i + 2),
      t->unread ? MenuWidget::ALTERNATE_STYLE : MenuWidget::DEFAULT_STYLE,
      label, sub, t->lastTime);
    if (option) {
      chatsMenu->addOption(option);
    }
  }

  if (keep) {
    chatsMenu->select(keep);
  }
}

/* One correspondent, both directions, oldest at the top — as WRAPPED TEXT you can read.
 *
 * Every message is a "who + when" line followed by its full body, wrapped by the widget:
 *
 *     You  ·  2 min ago
 *     on my way, be there in ten
 *
 *     4255550142  ·  1 min ago
 *     see you there
 *
 * ⚠ BOUNDED BY CHARACTERS, NOT BY MESSAGE COUNT, and it is the newest that survive.
 * MultilineTextWidget allocates a buffer per wrapped ROW from internal heap, so what costs
 * memory here is total text, not how many messages it came in. A budget in characters is
 * therefore the honest limit; a message cap would let forty long texts blow past it.
 * When older messages are cut, the top of the view says so rather than pretending the
 * conversation starts there. */
void MessagesApp::buildThread() {
  if (threadText) {
    delete threadText;
    threadText = NULL;
  }

  const int16_t padding = 4;
  threadText = new MultilineTextWidget(0, header->height(), lcd.width(),
                                       lcd.height() - header->height() - footer->height(),
                                       "No messages yet", controlState, THREAD_TEXT_MAX,
                                       fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum,
                                       padding, padding);
  threadText->setColors(WP_COLOR_1, WP_COLOR_0);       // white on black, matching the theme

  const int n = sipThreadOpen(flash.messages, threadPeer);
  if (n <= 0) {
    threadText->cursorToStart();
    threadText->setFocus(true);
    return;
  }

  /* Walk BACKWARD from the newest, taking messages until the budget is spent, so the part
   * you can read is the part you just received rather than the oldest thing in the store. */
  int first = n;
  size_t used = 0;
  while (first > 0) {
    const SipThreadMsg* m = sipThreadMsgAt(first - 1);
    size_t cost = (m && m->text ? strlen(m->text) : 0) + 48;   // + the "who · when" line
    if (used + cost > THREAD_TEXT_BUDGET && first < n) {
      break;                          // always keep at least the newest, however long it is
    }
    used += cost;
    first--;
  }

  char* buf = (char*)ps_malloc(THREAD_TEXT_MAX);
  if (!buf) {
    log_e("MessagesApp: no PSRAM for the thread text");
    threadText->cursorToStart();
    return;
  }
  size_t w = 0;
  buf[0] = '\0';

  if (first > 0) {
    w += snprintf(buf + w, THREAD_TEXT_MAX - w, "... %d older message%s not shown\n\n",
                  first, first == 1 ? "" : "s");
  }

  for (int i = first; i < n && w < THREAD_TEXT_MAX - 1; i++) {
    const SipThreadMsg* m = sipThreadMsgAt(i);
    if (!m) {
      continue;
    }
    char ago[24];
    ago[0] = '\0';
    if (m->time) {
      ntpClock.dateTimeAgo(m->time, ago);
    }
    // "You" on the ones you sent. Who said what has to be unambiguous before anything else.
    w += snprintf(buf + w, THREAD_TEXT_MAX - w, "%s%s%s\n%s\n%s",
                  m->incoming ? threadLabel : "You",
                  ago[0] ? "  ·  " : "", ago,
                  m->text ? m->text : "",
                  (i + 1 < n) ? "\n" : "");
  }

  threadText->setText(buf);
  free(buf);
  /* ⚠ END, not start, and FOCUS. Two separate bugs {user} hit at once: the thread opened at the
   * oldest message, and up/down did nothing. The widget scrolls by moving its cursor, and it
   * only takes cursor keys when focused — so without setFocus() a long conversation was
   * frozen at the top with no way down. */
  threadText->cursorToEnd();
  threadText->setFocus(true);
}

void MessagesApp::redrawScreen(bool redrawAll) {
  log_i("redraw MessagesApp");

  if (subApp) {
    subApp->redrawScreen(redrawAll);
    return;
  }

  if (appState == CHATS && chatsMenu) {
    ((GUIWidget*)chatsMenu)->redraw(lcd);
  } else if (appState == THREAD && threadText) {
    ((GUIWidget*)threadText)->redraw(lcd);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Create Message app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

CreateMessageApp::CreateMessageApp(LCD& lcd, ControlState& state, Storage& flash, HeaderWidget* header, FooterWidget* footer, const char* sipUri, bool jumpToThread)
  : WindowedApp(lcd, state, header, footer), FocusableApp(2), flash(flash), jumpToThread(jumpToThread) {
  log_d("create CreateMessageApp");

  label1 = NULL;
  addr = NULL;
  sendMessageAs = NULL;
  label2 = NULL;
  text = NULL;

  this->setupUI(sipUri, hasSipAndLora(sipUri));
  this->setHeaderFooter();

  subApp = NULL;
}

void CreateMessageApp::setupUI(const char* sipUri, bool showMessageType) {
  log_d("CreateMessageApp::createUI");

  /* ⚠ WHAT YOU HAVE TYPED SURVIVES A CHANGE OF RECIPIENT. This function rebuilds every
   * widget on the screen — it is how the phonebook pick fills "To:" in — and the fresh
   * body widget came back empty, so choosing a recipient after writing the message
   * silently binned the message. Copied out before deleteUI(), which frees the widget
   * the string belongs to. PSRAM (extStrdup): the body runs to 1000 characters and
   * internal contiguous heap is the scarce thing on this phone. */
  char* keepBody = NULL;
  if (text) {
    const char* body = text->getText();
    if (body && body[0]) {
      keepBody = extStrdup(body);
    }
  }

  this->deleteUI();
  focusableWidgets.clear();

  uint16_t yOff = header->height();

  label1 = new LabelWidget(0, yOff, lcd.width(), 25, "To:", WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += label1->height();

  addr = new TextInputWidget(0, yOff, lcd.width(), 35, controlState, 100, fonts[AKROBAT_BOLD_20], InputType::AlphaNum, 8);
  addr->setText(sipUri ? sipUri : "");
  yOff += addr->height();

  if (showMessageType) {
    sendMessageAs = new ChoiceWidget(0, yOff, lcd.width(), 35);
    sendMessageAs->addChoice("sip");
    sendMessageAs->addChoice("LoRa");
    yOff += sendMessageAs->height();
  } else {
    sendMessageAs = NULL;
  }

  label2 = new LabelWidget(0, yOff, lcd.width(), 25, "Message:", WP_ACCENT_1, WP_COLOR_1, fonts[AKROBAT_BOLD_18], LabelWidget::LEFT_TO_RIGHT, 8);
  yOff += label1->height();

  text = new MultilineTextWidget(0, yOff, lcd.width(), lcd.height()-yOff-footer->height(),
                                 "type your message", controlState, 1000, fonts[AKROBAT_BOLD_20], InputType::AlphaNum, 8, 5);
  text->setText(keepBody ? keepBody : "");    // TODO: allow a template like "Hello!\nHow are you doing?"
  if (keepBody) {
    free(keepBody);                           // free() is region-agnostic; see extStrdup
  }


  addFocusableWidget(addr);
  if (showMessageType) {
    addFocusableWidget(sendMessageAs);
  }
  addFocusableWidget(text);

  setFocus(addr);
  syncPredictive();          // starts on the address field, so: off
}

/* Predictive text belongs to the message body and to nothing else on this screen. */
void CreateMessageApp::syncPredictive() {
  controlState.t9Field = (this->getFocused() == (FocusableWidget*)text);
}

void CreateMessageApp::deleteUI() {
  if(label1) {
    delete label1;
  }
  if(label2) {
    delete label2;
  }
  if(addr) {
    delete addr;
  }
  if(text) {
    delete text;
  }
  if(sendMessageAs) {
    delete sendMessageAs;
  }
}

CreateMessageApp::~CreateMessageApp() {
  log_d("destroy CreateMessageApp");

  this->deleteUI();

  if(subApp) {
    delete subApp;
  }
}

void CreateMessageApp::setHeaderFooter() {
  header->setTitle("New Message");
  footer->setButtons(this->getFocused()==addr ? "Choose" : "Send", "Clear");
}

bool CreateMessageApp::isSipAddress(const char* address) {
  log_d("#### checking address type: %s", address);
  if (strncmp(address, "LORA:", 5) == 0) {
    return false;
  }

  if (strlen(address) == 6) {
    bool valid = true;
    for (int i = 0; i < strlen(address); ++i) {
      char c = address[i];

      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
        valid = false;
        break;
      }
    }

    if (valid) {
      return false;
    }
  }

  return true;
}

bool CreateMessageApp::hasSipAndLora(const char* address) {
  if (address == NULL) {
    return false;
  }

  if (strncmp(address, "LORA:", 5) != 0) {
    return false;
  }

  for (int i = 0; i < strlen(address); ++i) {
    if (address[i] == '!') {
      return true;
    }
  }

  return false;
}

const char* CreateMessageApp::extractAddress(const char* address, MessageType_t type) {
  static char ra[400] = {0};
  memset(ra, 0x00, sizeof(ra));
  switch (type) {
  case SIP:
    if (strncmp(address, "LORA:", 5) == 0) {
      int start = 0;
      for (start = 0; start < strlen(address); ++start) {
        if (address[start] == '!') {
          break;
        }
      }
      memcpy(ra, address+start+1, strlen(address)-start-1);
    } else {
      strlcpy(ra, address, sizeof(ra));
    }
    break;
  case LORA: {
    if (strncmp(address, "LORA:", 5) == 0) {
      int end = 0;
      for (end = 0; end < strlen(address); ++end) {
        if (address[end] == '!') {
          break;
        }
      }

      if (end > 0 && sizeof(ra) > end-5) {
        memcpy(ra, address+5, end-5);
      }
    }
  }
  break;
  }

  return ra;
}

extern uint32_t chipId;

/* Does the correspondent just picked already have a conversation? If so, record it for
 * the parent and say so; the caller then exits instead of composing.
 *
 * ⚠ READS THE THREAD SNAPSHOT, DOES NOT REBUILD IT. MessagesApp::buildChats() built it
 * on the way to this screen and nothing since has invalidated it, so this costs a walk
 * of at most SIP_THREADS_MAX entries. Rebuilding here would re-scan the message store
 * (and the parent would only throw the result away and rebuild again on the way back).
 *
 * ⚠ Compared on the grouping IDENTITY, not the string: the phonebook's spelling of a
 * number is rarely the store's. Same rule as sipContactName(), for the same reason.
 *
 * Both addresses are tried because a phonebook entry can carry both, and which one you
 * have history on is exactly the question — SIP first, since that is what the "To:" field
 * defaults to sending as when an entry has both. */
bool CreateMessageApp::takeThreadJump(const char* sipUri, const char* loraAddress) {
  if (!jumpToThread) {
    return false;
  }
  /* ⚠ ONLY WITH AN EMPTY BODY. Choose is reachable after typing — move focus back up to
   * "To:" and press OK — and someone who has already written the message meant to send
   * it, not to go read history. setupUI() now carries the body across the pick, so
   * staying put keeps what they wrote; jumping would throw the screen away with it. */
  const char* body = text ? text->getText() : NULL;
  if (body && body[0]) {
    return false;
  }

  const char* tries[2] = { sipUri, loraAddress };
  for (int i = 0; i < 2; i++) {
    if (!tries[i] || !tries[i][0]) {
      continue;
    }
    char want[SMS_MIRROR_PEER_MAX];
    sipThreadIdentity(tries[i], want, sizeof(want));
    if (!want[0]) {
      continue;
    }
    for (int t = 0; t < sipThreadsCount(); t++) {
      const SipThread* th = sipThreadAt(t);
      if (!th || strcmp(th->peer, want)) {
        continue;
      }
      snprintf(jumpPeer, sizeof(jumpPeer), "%s", th->peer);
      snprintf(jumpUri,  sizeof(jumpUri),  "%s", th->uri);
      log_i("jump to existing thread with %s", jumpPeer);
      return true;
    }
  }
  return false;
}

appEventResult CreateMessageApp::processEvent(EventType event) {
  log_i("processEvent CreateMessageApp");

  appEventResult res = DO_NOTHING;
  if (subApp) {
    if ((res |= subApp->processEvent(event)) & EXIT_APP) {
      const char* sipUri = ((PhonebookApp*)subApp)->getSelectedSipUri();
      const char* loraAddress = ((PhonebookApp*)subApp)->getSelectedLoraAddress();
      delete subApp;
      subApp = NULL;

      /* Already talking to this person? Then that conversation is where you meant to go —
       * hand it to the parent and get out of the way. Nothing happens here for a
       * correspondent with no history, which is the case this screen is actually for. */
      if (this->takeThreadJump(sipUri, loraAddress)) {
        return EXIT_APP | REDRAW_ALL;
      }

      if (sipUri && *sipUri && loraAddress && *loraAddress) {
        char tmp[800] = {0};
        snprintf(tmp, sizeof(tmp), "LORA:%s!%s", loraAddress, sipUri);
        setupUI(tmp, true);
      } else if (sipUri && *sipUri) {
        setupUI(sipUri, false);
      } else if (loraAddress && *loraAddress) {
        char tmp[300] = {0};
        snprintf(tmp, sizeof(tmp), "LORA:%s", loraAddress);
        setupUI(tmp, false);
      } else {
        log_e("empty SIP URI");
      }

      // Restore Title and Header widgets
      this->setHeaderFooter();

      res = REDRAW_ALL;
    }
  } else if (event == WIPHONE_KEY_END) {     // the button below BACK, which, it turn, is used as backspace
    return EXIT_APP;
  } else if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN) {

    bool cursorMoved = false;
    if (this->getFocused() == text) {
      int32_t cursRow = text->getCursorRow();
      text->processEvent(event);
      if (cursRow != text->getCursorRow()) {
        cursorMoved = true;
        res |= REDRAW_SCREEN;
      }
    }
    if (!cursorMoved) {
      nextFocus(event == WIPHONE_KEY_DOWN);
      syncPredictive();       // the body predicts, the address does not
      this->setHeaderFooter();
      res |= REDRAW_ALL;
    }

  } else if (LOGIC_BUTTON_OK(event)) {

    if (this->getFocused() == addr) {       // TextInputWidget has focus

      // Select the receiver address. The D-pad centre is fine here — opening the phonebook
      // is not something you have to take back.

      subApp = new PhonebookApp(NULL, lcd, lcd, controlState, flash, header, footer, true);
      res |= REDRAW_ALL;

    } else if (!LOGIC_BUTTON_SEND(event)) {

      /* ⚠ THE D-PAD CENTRE MUST NOT SEND. On the message body it is a TYPING key — it
       * commits the highlighted multi-tap letter — so the press after your last letter was
       * sending the message half-written. Reported by {user} 2026-08-17.
       *
       * Swallowed rather than passed on: a centre press that reaches here had no pending
       * letter left to commit, so there is nothing to do. Send is the top-left soft key,
       * or CALL. */
      return DO_NOTHING;

    } else {

      // Send message

      // Collect message data
      const char* fromUri  = controlState.fromUriDyn;
      const char* toUri    = addr->getText();
      /* A bare typed number becomes a full SIP address from the phone's own account —
       * before classification, so "4255550142" counts as SIP rather than falling through
       * to the LoRa branch. Anything with '@', letters or a LORA: prefix is untouched. */
      char completedAddr[128];
      if (sipCompleteAddress(toUri, completedAddr, sizeof(completedAddr))) {
        toUri = completedAddr;
      }
      const char* message  = text->getText();
      bool        incoming = false;
      char loraAddress[100] = {0};
      bool sipMessage = isSipAddress(toUri);

      if (sendMessageAs != NULL) {
        switch (sendMessageAs->getValue()) {
        case 0: // Sip
          sipMessage = true;
          toUri = extractAddress(toUri, MessageType_t::SIP);
          break;
        case 1: // LoRa
          sipMessage = false;
          toUri = extractAddress(toUri, MessageType_t::LORA);
          break;
        default:
          log_e("Unknown message type: %d", sendMessageAs->getValue());
        }
      }

      if (!sipMessage) {
        // LoRa message
        snprintf(loraAddress, sizeof(loraAddress), "%X", chipId);
        fromUri = loraAddress;

        if (strncmp(toUri, "LORA:", 5) == 0) {
          toUri = extractAddress(toUri, MessageType_t::LORA);
        }
      }

      log_d("To address: %s", toUri);

      /* 0 = unknown time. msgStamp() reads the time and WHO set it in one lock: a stamp off a
       * mesh clock is saved PROVISIONAL (its meshId), and NTP/GPS finalises it (review SA-3). */
      const ClockMsgStamp stamp = ntpClock.msgStamp();
      uint32_t time        = stamp.utc;    // never 0 when the clock is known (msgStamp)

      // Save to message database
      flash.messages.saveMessage(message, fromUri, toUri, incoming, time, 0, 0, stamp.meshId);

      // Queue for sending
      log_d("adding message to send queue: %d %s %s", sipMessage, fromUri, toUri);
      MessageData* msg = new MessageData(fromUri, toUri, message, time, incoming);

      if (sipMessage) {
        controlState.outgoingMessages.add(msg);
      } else {
        controlState.outgoingLoraMessages.add(msg);
      }

      return EXIT_APP | REDRAW_ALL;       // special signal for the parent that a message was sent
    }

  } else {
    FocusableWidget *widg = this->getFocused();
    if (widg!=NULL) {
      widg->processEvent(event);
    }
    res |= REDRAW_SCREEN;
  }

  return res;
}

void CreateMessageApp::redrawScreen(bool redrawAll) {
  log_i("redraw CreateMessageApp");

  if (subApp) {
    subApp->redrawScreen(redrawAll);
    return;
  }

  ((GUIWidget*)addr)->redraw(lcd);
  ((GUIWidget*)text)->redraw(lcd);

  ((GUIWidget*)label1)->redraw(lcd);
  ((GUIWidget*)label2)->redraw(lcd);

  if (sendMessageAs != NULL) {
    ((GUIWidget*)sendMessageAs)->redraw(lcd);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  View Message app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ViewMessageApp::ViewMessageApp(int32_t messageOffset, LCD& lcd, ControlState& state, Storage& flash, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), flash(flash), messageOffset(messageOffset) {
  log_i("create ViewMessageApp: messageOffset = %d", messageOffset);

  const int16_t padding =  4;
  textArea = new MultilineTextWidget(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(),
                                     "Empty message", state, 10000, fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum, padding, padding);
  textArea->setColors(WP_COLOR_0, WP_COLOR_1);

  // Loop over a single element in preloaded messages array: a little trick to use MessageData interface to access the message
  for (auto it = flash.messages.iteratorCount(messageOffset, 1); it.valid(); ++it) {
    textArea->setText(it->getMessageText());
    const char* format = "%s\n\n--\nFrom:\n%s\nTo:\n%s\nTime:\n%s\n";
    char msgTime[30];
    Clock::unixToHuman(it->getTime(), msgTime);
    char msg[strlen(it->getMessageText()) + strlen(it->getOtherUri()) + strlen(it->getOwnUri()) + strlen(format) + strlen(msgTime)];
    sprintf(msg, format, it->getMessageText(), it->getOwnUri(), it->getOtherUri(), msgTime);
    textArea->setText(msg);
    textArea->cursorToStart();

    IF_LOG(VERBOSE)
    it->show();

    // Remove "unread" status
    if (!it->isRead()) {
      flash.messages.setRead(*it);
      state.unreadMessages = flash.messages.hasUnread();
    }
  }
  textArea->setFocus(true);     // to reveal the cursor
  this->changeState(MAIN);
}

ViewMessageApp::~ViewMessageApp() {
  if (textArea) {
    delete textArea;
  }
  if (options) {
    delete options;
  }
  if (subApp) {
    delete subApp;
  }
}

appEventResult ViewMessageApp::processEvent(EventType event) {
  log_i("processEvent ViewMessageApp: %d", (int)event);

  appEventResult res = DO_NOTHING;
  if (subApp) {

    if ((res = subApp->processEvent(event)) & EXIT_APP) {
      this->messageSent = ((res & REDRAW_ALL) == REDRAW_ALL);

      // After creating a message `appState` stays the same, just need to change Title and Header widgets
      changeState(appState);

      delete subApp;
      subApp = NULL;

      res = REDRAW_ALL;
    }

  } else if (appState == MAIN) {
    if (LOGIC_BUTTON_BACK(event)) {
      return EXIT_APP | (this->messageSent ? REDRAW_ALL : 0);  // special signal for the MessagesApp to reload the menus if a message was sent (or deleted)
    }

    if (event == WIPHONE_KEY_SELECT) {
      res |= this->changeState(OPTIONS);
    } else if (IS_KEYBOARD(event)) {
      textArea->processEvent(event);
      res |= REDRAW_SCREEN;
    }
  } else if (appState == OPTIONS) {

    res |= REDRAW_SCREEN;
    if (LOGIC_BUTTON_BACK(event)) {
      res |= changeState(MAIN);
    } else {
      options->processEvent(event);
      MenuOption::keyType sel = options->readChosen();
      if (sel>0) {
        if (sel==111) {
          // "Reply" option selected

          // Loop over a single element in preloaded messages array: a little trick to use MessageData interface to access the message's otherUri
          const char* otherUri = "";
          for (auto it = flash.messages.iteratorCount(messageOffset, 1); it.valid(); ++it) {
            otherUri = it->getOtherUri();
          }

          // Create message app
          subApp = new CreateMessageApp(lcd, controlState, flash, header, footer, otherUri);
          res |= REDRAW_ALL;

        } else if (sel==222) {
          // "Delete" option selected
          flash.messages.deleteMessage(this->messageOffset);
          res = EXIT_APP | REDRAW_ALL;       // special signal for the MessagesApp to reload the menus if a message was deleted (or sent)
        }
      }
    }

  }

  return res;
}

appEventResult ViewMessageApp::changeState(ViewMessageState_t newState) {
  if (newState == OPTIONS) {
    header->setTitle("Options");
    footer->setButtons("Select", "Back");
    if (options == NULL) {
      options = new OptionsMenuWidget(0, header->height(), lcd.width(), lcd.height()-header->height()-footer->height());
      if (options) {
        options->addOption("Reply",  111);
        options->addOption("Delete", 222);
      }
    }
  } else if (newState == MAIN) {
    header->setTitle("Message");
    footer->setButtons("Options", "Back");
  }
  appState = newState;
  return REDRAW_ALL;
}

void ViewMessageApp::redrawScreen(bool redrawAll) {
  log_i("redraw ViewMessageApp");
  if (subApp) {
    subApp->redrawScreen(redrawAll);
    return;
  } else if (appState == MAIN) {
    ((GUIWidget*) textArea)->redraw(lcd);
  } else if (appState == OPTIONS) {
    ((GUIWidget*) options)->redraw(lcd);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Design Demo app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

DesignDemoApp::DesignDemoApp(LCD& lcd, ControlState& state) : WiPhoneApp(lcd, state) {
  log_d("create DesignDemoApp");
}

DesignDemoApp::~DesignDemoApp() {
  log_d("destroy DesignDemoApp");
}

appEventResult DesignDemoApp::processEvent(EventType event) {
  log_d("processEvent DesignDemoApp");
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }
  screenNo = (screenNo + 1) % 2;
  return REDRAW_SCREEN;
}

void DesignDemoApp::redrawScreen(bool redrawAll) {
  log_i("redraw DesignDemoApp");

  uint32_t t = micros();

  // Screen sprite
  TFT_eSprite screen = TFT_eSprite(&lcd);
  screen.setColorDepth(16);
  screen.createSprite(lcd.width(), lcd.height());
  if (!screen.isCreated()) {
    log_d("screen sprite not created");
  }

  // Test 256-color image
  if (!screenNo) {

    screen.drawImage(image_i256, sizeof(image_i256));
    //screen.drawImage(icon_skull_5, sizeof(icon_skull_5), 10, 10);
    //screen.drawImage(icon_skull_10, sizeof(icon_skull_10), 10, 60);
    //screen.drawImage(icon_skull_15, sizeof(icon_skull_15), 10, 150);
    screen.drawImage(icon_batt_w_5, sizeof(icon_batt_w_5), 120, 18);
    screen.drawImage(icon_wifi_w_3, sizeof(icon_wifi_w_3), 100, 18);
    IconRle3* icon = new IconRle3(icon_Games_w, sizeof(icon_Games_w));
    screen.drawImage(*icon, 160, 10);
    delete icon;
    icon = new IconRle3(icon_Games_b, sizeof(icon_Games_b));
    screen.drawImage(*icon, 160, 60);
    delete icon;
    screen.pushSprite(0, 0);

  } else {

    screen.fillSprite(TFT_BLACK);

    // Header sprite
    TFT_eSprite sprite = TFT_eSprite(&screen);

    sprite.setColorDepth(16);
    sprite.createSprite(lcd.width(), 30);
    sprite.fillSprite(TFT_BLACK);

    // - Title
    sprite.setTextColor(sprite.color565(0xFF, 0x6F, 0x00), TFT_BLACK);
    sprite.setTextFont(fonts[AKROBAT_BOLD_18]);
    sprite.setCursor(8, 3);
    sprite.printToSprite("Messages", 8);

    // - Icons
    sprite.drawImage(icon_batt_w_5, sizeof(icon_batt_w_5), 213, 7);
    sprite.drawImage(icon_wifi_w_3, sizeof(icon_wifi_w_3), 189, 5);

    // - Time
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setTextFont(fonts[AKROBAT_BOLD_16]);
    sprite.setTextDatum(TR_DATUM);
    //sprite.setCursor(179, 5);
    sprite.drawString("12:55", 180, 6);

    sprite.pushSprite(0, 0);
    sprite.deleteSprite();

    // Footer sprite
    sprite.createSprite(lcd.width(), 40);
    sprite.fillSprite(TFT_BLACK);

    sprite.setCursor(92, 4+7);
    sprite.setTextFont(fonts[AKROBAT_BOLD_24]);
    sprite.printToSprite("Select", 6);

    sprite.setCursor(192, 7+7);
    sprite.setTextFont(fonts[AKROBAT_SEMIBOLD_22]);
    sprite.printToSprite("Back", 4);

    sprite.setCursor(8, 7+7);
    sprite.setTextFont(fonts[AKROBAT_SEMIBOLD_22]);
    sprite.printToSprite("Options", 7);

    sprite.pushSprite(0, lcd.height()-40);
    sprite.deleteSprite();

    // Direct to screen
    screen.fillRect(0, 30, 240, 8, TFT_WHITE);
    screen.fillRect(0, 38, 240, 35, sprite.color565(0x63, 0xD9, 0x67));
    screen.fillRect(0, 73, 240, 8, TFT_WHITE);
    screen.setTextFont(fonts[AKROBAT_BOLD_20]);
    screen.setTextColor(TFT_WHITE, sprite.color565(0x63, 0xD9, 0x67));
    //screen.setCursor(8, 42+4);
    //screen.printToSprite("+ Create New Message", 20);
    screen.drawString("+ Create New Message", 8, 42+4);
    screen.drawImage(icon_batt_w_4, sizeof(icon_batt_w_4), 200, 48);

    // Menu sprite
    sprite.createSprite(lcd.width(), 199);    // menu sprite
    sprite.fillSprite(TFT_WHITE);

    // Menu item sprites
    uint16_t yOff = 0;
    const char* names[4] = { "Yemi Ajibade", "John Doe", "Skynet Logistics", "Zuck-dawg" };
    const char* dates[4] = { "05-09-2020", "27-08-2018", "01-07-2029", "04-08-2018" };
    const char* texts[4] = { "Temporarily out of funds...", "Lorem Ipsum", "Did you get the package? ", "Need to poo, can't reach your timeline!" };
    TFT_eSprite spr = TFT_eSprite(&sprite);

    for (uint8_t x=0; x<4; x++) {
      spr.createSprite(lcd.width(), 51);
      spr.drawFastHLine(0, 0, 240, TFT_BLACK);
      spr.fillRect(0, 1, 240, 50, TFT_WHITE);

      spr.setTextFont(fonts[AKROBAT_EXTRABOLD_22]);
      spr.setCursor(8, 5);
      spr.setTextColor(TFT_BLACK, TFT_WHITE);
      spr.printToSprite(names[x], strlen(names[x]));

      spr.setTextFont(fonts[AKROBAT_BOLD_18]);
      //spr.setCursor(156, 6);
      //spr.printToSprite(dates[x], strlen(dates[x]));
      spr.setTextDatum(TR_DATUM);
      spr.drawString(dates[x], 232, 7);

      spr.setTextFont(fonts[AKROBAT_BOLD_16]);
      spr.setTextDatum(TL_DATUM);
      spr.setTextColor(spr.color565(0x64, 0x64, 0x64), TFT_WHITE);
      spr.setCursor(8, 32);
      spr.printToSprite(texts[x], strlen(texts[x]));

      spr.pushSprite(0, yOff);
      spr.deleteSprite();

      yOff += 51;
    }

    sprite.pushSprite(0, 81);
    sprite.deleteSprite();

    // Overlay image
    screen.drawImage(rle3_image, sizeof(rle3_image));

    // Draw entire screen
    screen.pushSprite(0, 0);
    screen.deleteSprite();
  }
  t = micros()-t;

  log_d("Drawing & pushing sprite: %d us\r\n", t);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Mic Test app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

//MicTestApp::MicTestApp(Audio* audio, LCD& lcd, ControlState& state, FooterWidget* footer) : WiPhoneApp(lcd, state), audio(audio) {
MicTestApp::MicTestApp(Audio* audio, LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer) : WindowedApp(lcd, state, header, footer), audio(audio) {
  state.msAppTimerEventLast = millis();
  state.msAppTimerEventPeriod = 33;    // 30 fps

  /* A pop first, then music (review 3, A6): a notification chirp in flight holds a snapshot of
   * the pre-pop rate, format, route and levels, and its teardown's restore() re-clocked this
   * app's ring under it (32 kHz stereo after a game). The pop-first rule the ring, a call, a
   * track and a game already follow.
   * Music first (review, 2026-09-25): start() restarts I2S and turnMicOn() swaps music's own
   * ring for the microphone's under a playing track. Paused, so it keeps its place. */
  notifyPopFinishFor("the Mic test");
  musicPlayerPause();
  audio->start();
  audio->turnMicOn();

  redrawScreen(true);
}

MicTestApp::~MicTestApp() {
  audio->shutdown();
}

appEventResult MicTestApp::processEvent(EventType event) {
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }
  if (event == APP_TIMER_EVENT) {
    return REDRAW_SCREEN;
  }
  return DO_NOTHING;
}

void MicTestApp::redrawScreen(bool redrawAll) {
  uint32_t val = audio->getMicAvg();

  if ( redrawAll ) {
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.setTextFont(2);
    lcd.drawString("Front Mic", 10, 40);
    lcd.drawString("Rear Mic", 10, 110);
    lcd.setTextSize(1);

    header->setTitle("Mic Test");
    footer->setButtons("", "Back");

  }

  uint8_t stp = 0;
  uint16_t xOff = 0;
  uint16_t colorStep = 17;
  uint16_t xStep = 16;

  // draw the front mic line
  do {
    uint8_t R = colorStep*stp;
    lcd.drawFastHLine(xOff, 80, xStep, lcd.color565(R, 255-R, 0));
    xOff += xStep;
    val >>= 1;
    stp++;
  } while (val && xOff < lcd.width()-xStep);
  lcd.drawFastHLine(xOff, 80, lcd.width()-xOff, TFT_BLACK);

  // draw the rear mic line
  xOff = 0;
  stp = 0;
  //val = audio->getRearMicAvg();
  val = audio->getMicAvg();
  do {
    uint8_t R = colorStep*stp;
    lcd.drawFastHLine(xOff, 100, xStep, lcd.color565(R, 255-R, 0));
    xOff += xStep;
    val >>= 1;
    stp++;
  } while (val && xOff < lcd.width()-xStep);
  lcd.drawFastHLine(xOff, 100, lcd.width()-xOff, TFT_BLACK);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Audio recorder app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

RecorderApp::RecorderApp(Audio* audio, LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), audio(audio), sprite(&lcd) {
  state.msAppTimerEventLast = millis();
  state.msAppTimerEventPeriod = 0;    // 30 fps

  //sprite.setColorDepth(3);        // it's possible to use 3-bit depth here, but it is too slow
  sprite.setColorDepth(16);
  sprite.createSprite(160, 100);
  if (sprite.isCreated()) {
    sprite.fillSprite(TFT_RED);
  } else {
    log_d("error: screen sprite not created");
  }

  header->setTitle("Recoder");
  footer->setButtons("Record", "Back");

  label = new LabelWidget(0, 195, lcd.width(), 35, "Not recording", WP_COLOR_1, WP_COLOR_0, fonts[AKROBAT_BOLD_22], LabelWidget::CENTER, 8);

  /* A pop first (review 3, A6 - see MicTestApp), then music (review, 2026-09-25): start()
   * restarts I2S and turnMicOn() swaps music's own ring for the microphone's under a playing
   * track. Paused, so it keeps its place. */
  notifyPopFinishFor("the Recorder");
  musicPlayerPause();
  audio->start();
  audio->turnMicOn();
}

RecorderApp::~RecorderApp() {
  /* ⚠ TFT_eSprite HAS NO DESTRUCTOR anywhere in the vendored library — the pixel buffer is
   * released only by an explicit deleteSprite(). Without this the buffer was stranded on
   * every open/exit. The small sprites are the stability problem, not the big one:
   * esp32Calloc() tries plain calloc() first, so anything that fits comes from the INTERNAL
   * heap and is left mid-heap, splitting the free space `largest` measures. deleteSprite()
   * early-returns when nothing was created, so this is unconditionally safe. */
  sprite.deleteSprite();
  if (label) {
    delete label;   // PSRAM, but it was never released either
  }
  audio->shutdown();
}

appEventResult RecorderApp::processEvent(EventType event) {
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }

  appEventResult res = DO_NOTHING;

  // Update sprite
  if (event == WIPHONE_KEY_SELECT && this->recorded && !this->recording) {

    audio->playRecord();

  } else if (event == WIPHONE_KEY_SELECT || event == WIPHONE_KEY_OK || recording && event == APP_TIMER_EVENT && audio->isRecordingFinished()) {

    recording = !recording;
    if (recording) {
      sprintf(this->filename, "/audio_%02d%02d%02d_%02d%02d%02d.pcm", ntpClock.getYear()-2000, ntpClock.getMonth(), ntpClock.getDay(), ntpClock.getHour(), ntpClock.getMinute(), ntpClock.getSecond());
      audio->setBitsPerSample(16);
      audio->setSampleRate(16000);
      audio->setMonoOutput(true);
      if (audio->recordFromMic()) {
        for (int i = 0; i < sizeof(microphoneValues)/sizeof(microphoneValues[0]); i++) {
          microphoneValues[i] = 1;
        }
        label->setText("Recording...");
        label->setColors(TFT_RED, WP_COLOR_0);
        controlState.msAppTimerEventPeriod = 33;    // 30 fps
        footer->setButtons("Stop", "Back");
        res |= REDRAW_FOOTER;
      } else {
        label->setColors(WP_COLOR_1, WP_COLOR_0);
        label->setText("ERROR: not enough RAM?");
        recording = !recording;
      }
    } else {
      footer->setButtons(NULL, "Wait");
      label->setText("Writing file...");
      label->setColors(WP_COLOR_1, WP_COLOR_0);
      ((GUIWidget*) label)->redraw(lcd);
      ((GUIWidget*) footer)->redraw(lcd);

      if (audio->saveWavRecord(&SD, filename)) {
        label->setText(filename+1);
      } else {
        label->setText("Couldn't save the file");
      }
      this->recorded = true;
      footer->setButtons("Play", "Back");
      res |= REDRAW_FOOTER;
      controlState.msAppTimerEventPeriod = 0;
    }
    spriteUpdated = true;
    res |= REDRAW_SCREEN;

  } else if (event == APP_TIMER_EVENT) {

    // Get microphone (averaged) value
    int32_t val = audio->getMicAvg();

    // Scale
    const float scaleDown = 80;
    val = log(val/scaleDown)/log(1.04);
    if (val > 100) {
      val = 100;
    }
    if (val <= 0) {
      val = 1;
    }

    // Remember
    int prevVal = curVal == 0 ? 159 : curVal - 1;
    microphoneValues[prevVal] = val;

    // Draw to sprite
    int x = curVal;
    for (int i=0; i<160; i++) {
      sprite.drawFastVLine(i, 0, 100-microphoneValues[x], TFT_BLACK);
      sprite.drawFastVLine(i, 100-microphoneValues[x], 100, TFT_RED);
      x = (x + 1) % 160;
    };
    spriteUpdated = true;
    curVal = (curVal + 1) % 160;
    res |= REDRAW_SCREEN;
  }

  return res;
}

void RecorderApp::redrawScreen(bool redrawAll) {
  uint32_t val = audio->getMicAvg();

  if (!screenInited || redrawAll) {
    lcd.fillRect(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), TFT_BLACK);
  }

  if (spriteUpdated || !screenInited || redrawAll) {
    if (recording) {
      sprite.pushSprite(40, 50);
      spriteUpdated = false;
    } else {
      lcd.fillRect(40, 50, 160, 100, TFT_BLACK);
      lcd.fillCircle(120, 114, 35, TFT_RED);
    }
  }

  if (label->isUpdated() || !screenInited || redrawAll) {
    ((GUIWidget*) label)->redraw(lcd);
  }

  screenInited = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Diagnostics app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// The diagnostcs app draws directly to the screen to avoid issues if the external RAM is not available.
// If you want to avoid flickering you'll need to only redraw if the element changed.

//DiagnosticsApp::DiagnosticsApp(Audio* audio, LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
//  : WindowedApp(lcd, state, header, footer), FocusableApp(8), audio(audio), lastVoltages(3), lastSocs(3) {
DiagnosticsApp::DiagnosticsApp(Audio* audio, LCD& lcd, ControlState& state)
  : WiPhoneApp(lcd, state), audio(audio), lastVoltages(3), lastSocs(3) {
  lastVoltages.zero();
  lastSocs.zero();

  //header->setTitle("Diagnostics");
  //footer->setButtons(NULL, "Back");

  // Create all the widgets
  const uint16_t spacing = 1;
  uint16_t xOff = spacing;
  uint16_t yOff = 15; //header->height();

  // Row
  bVersion = new ButtonWidget(xOff, yOff, "ver. " FIRMWARE_VERSION, 0, 30, TFT_BLACK, greyBg, greyBorder);
  this->registerWidget(bVersion);
  xOff += bVersion->width() + spacing;
  //printf("Firmware Version: %s\r\n", FIRMWARE_VERSION);

  bUptime = new ButtonWidget(xOff, yOff, "Up:00d00:00", 0, 30, TFT_BLACK, greyBg, greyBorder);
  this->registerWidget(bUptime);
  xOff = spacing;
  yOff += bUptime->height() + spacing;

  // Row
  bVoltage = new ButtonWidget(xOff, yOff, "0.00V", 0, 30, TFT_BLACK, greyBg, greyBorder);
  this->registerWidget(bVoltage);
  xOff += bVoltage->width() + spacing;

  bStateOfCharge = new ButtonWidget(xOff, yOff, "000%", 0, 30, TFT_BLACK, greyBg, greyBorder);
  this->registerWidget(bStateOfCharge);
  xOff += bStateOfCharge->width() + spacing;

  bCardPresence = new ButtonWidget(xOff, yOff, "SD", 0, 30, TFT_BLACK, greyBg, greyBorder);
  this->registerWidget(bCardPresence);
  xOff += bCardPresence->width() + spacing;

  bUsbPresence = new ButtonWidget(xOff, yOff, "USB", 0, 30, TFT_BLACK, greyBg, greyBorder);
  this->registerWidget(bUsbPresence);
  xOff = spacing;
  yOff += bUsbPresence->height() + spacing;

  // Row
  bCharging = new ButtonWidget(xOff, yOff, "Discharging?", 0, 30, TFT_BLACK, greyBg, greyBorder);
  this->registerWidget(bCharging);
  xOff += bCharging->width() + spacing;

  lastAutonomous = !controlState.usbConnected;
  bAutonomous = new ButtonWidget(xOff, yOff, "Autonomous", 0, 30, TFT_BLACK, lastAutonomous ? greenBg : greyBg, lastAutonomous ? greenBorder : greyBorder);
  this->registerWidget(bAutonomous);
  xOff = spacing;
  yOff += bAutonomous->height() + spacing;
  //printf("USB State: %s\r\n", lastAutonomous ? "Unplugged" : "Connected");

  // Row
  bool inited = controlState.gaugeInited;
  //printf("Battery Gauge Inited: %s\r\n", inited? "yes" : "no");
  bBatteryGauge = new ButtonWidget(xOff, yOff, "Gauge", 0, 30, TFT_BLACK, inited ? greenBg : redBg, inited ? greenBorder : redBorder);
  this->registerWidget(bBatteryGauge);
  xOff += bBatteryGauge->width() + spacing;

  inited = controlState.extenderInited;
  //printf("GPIO Extender Inited: %s\r\n", inited? "yes" : "no");
  bGpioExtender = new ButtonWidget(xOff, yOff, "Extender", 0, 30, TFT_BLACK, inited ? greenBg : redBg, inited ? greenBorder : redBorder);
  this->registerWidget(bGpioExtender);
  xOff = spacing;
  yOff += bGpioExtender->height() + spacing;

  // Row
  inited = controlState.scannerInited;
  //printf("Key Scanner Inited: %s\r\n", inited? "yes" : "no");
  bKeyScanner = new ButtonWidget(xOff, yOff, "Scanner", 0, 30, TFT_BLACK, inited ? greenBg : redBg, inited ? greenBorder : redBorder);
  this->registerWidget(bKeyScanner);
  xOff += bKeyScanner->width() + spacing;

  inited = controlState.codecInited;
  //printf("Audio Codec Inited: %s\r\n", inited? "yes" : "no");
  bCodec = new ButtonWidget(xOff, yOff, "Codec", 0, 30, TFT_BLACK, inited ? greenBg : redBg, inited ? greenBorder : redBorder);
  this->registerWidget(bCodec);
  xOff += bCodec->width() + spacing;

  inited = controlState.psramInited;
  //printf("PSRAM Inited: %s\r\n", inited? "yes" : "no");
  bSpiRam = new ButtonWidget(xOff, yOff, "PSRAM", 0, 30, TFT_BLACK, inited? greenBg : redBg, inited ? greenBorder : redBorder);
  this->registerWidget(bSpiRam);
  xOff = spacing;
  yOff += bSpiRam->height() + spacing;

  // Row
  char buff[25];
  uint8_t mac[6];
  wifiState.getMac(mac);
  //printf("Chip ID: %02X:%02X:%02X:%02X:%02X:%02X\r\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(buff, sizeof(buff), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  bMacAddress = new ButtonWidget(xOff, yOff, buff, 0, 30, TFT_BLACK, greenBg, greenBorder);
  this->registerWidget(bMacAddress);
  xOff = spacing;
  yOff += bMacAddress->height() + spacing;

  // Row
  IPAddress ipAddr = WiFi.localIP();
  lastIpAddr = WiFi.localIP();
  //printf("IP Address: %d.%d.%d.%d\r\n", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3]);
  snprintf(buff, sizeof(buff), "%d.%d.%d.%d", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3]);
  bIpAddress = new ButtonWidget(xOff, yOff, "000.000.000.000", 0, 30, TFT_BLACK, (uint32_t) ipAddr ? greenBg : greyBg, (uint32_t) ipAddr ? greenBorder : greyBorder);
  bIpAddress->setText(buff);
  this->registerWidget(bIpAddress);
  xOff = spacing;
  yOff += bIpAddress->height() + spacing;

  // Row
  bRSSI = new ButtonWidget(xOff, yOff, "RSSI: -100", 0, 30, TFT_BLACK, greyBg, greyBorder);
  this->registerWidget(bRSSI);
  xOff = spacing;
  yOff += bUptime->height() + spacing;

  // NETWORKS
  xOff = spacing;
  yOff = 15; //header->height();
  for (int i=0; i<sizeof(bbPings)/sizeof(bbPings[0]); i++) {
    bbPings[i] = new ButtonWidget(xOff, yOff, "Pinging...", lcd.width()-spacing, 30, TFT_BLACK, greyBg, greyBorder);
    this->registerWidget(bbPings[i]);
    yOff += bbPings[i]->height() + spacing;
  }

  // FILESYSTEMS
  // currently the SD card is tested in main

  // AUDIO
  /*
  yOff = header->height()+75;
  testEarSpeaker = new ButtonWidget(1, yOff, "Front", 115, 30, TFT_BLACK, greyBg, greyBorder);
  testLoudSpeaker = new ButtonWidget(81, yOff, "Back", 115, 30, TFT_BLACK, greyBg, greyBorder);
  testHeadphone = new ButtonWidget(161, yOff, "Jack", 115, 30, TFT_BLACK, greyBg, greyBorder);

  this->registerWidget(testEarSpeaker);
  this->registerWidget(testLoudSpeaker);
  this->registerWidget(testHeadphone);
  */

  // SCREEN

  // KEYPAD
  // - row 1
  yOff = 75; //header->height()+15;
  bbKeys[0] = new ButtonWidget(100, yOff, "U", 40, 30, TFT_BLACK, greyBg, greyBorder);
  yOff += bbKeys[0]->height() + spacing;

  // - row 2
  bbKeys[1] = new ButtonWidget(1, yOff, "S", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[2] = new ButtonWidget(59, yOff, "L", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[3] = new ButtonWidget(100, yOff, "K", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[4] = new ButtonWidget(141, yOff, "R", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[5] = new ButtonWidget(199, yOff, "B", 40, 30, TFT_BLACK, greyBg, greyBorder);
  yOff += bbKeys[1]->height() + spacing;

  // - row 3
  bbKeys[6] = new ButtonWidget(1, yOff, "C", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[7] = new ButtonWidget(100, yOff, "D", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[8] = new ButtonWidget(199, yOff, "E", 40, 30, TFT_BLACK, greyBg, greyBorder);
  yOff += bbKeys[1]->height() + spacing;

  // - row 4
  bbKeys[9] = new ButtonWidget(1, yOff, "1", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[10] = new ButtonWidget(67, yOff, "2", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[11] = new ButtonWidget(133, yOff, "3", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[12] = new ButtonWidget(199, yOff, "F1", 40, 30, TFT_BLACK, greyBg, greyBorder);
  yOff += bbKeys[1]->height() + spacing;

  // - row 5
  bbKeys[13] = new ButtonWidget(1, yOff, "4", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[14] = new ButtonWidget(67, yOff, "5", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[15] = new ButtonWidget(133, yOff, "6", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[16] = new ButtonWidget(199, yOff, "F2", 40, 30, TFT_BLACK, greyBg, greyBorder);
  yOff += bbKeys[1]->height() + spacing;

  // - row 6
  bbKeys[17] = new ButtonWidget(1, yOff, "7", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[18] = new ButtonWidget(67, yOff, "8", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[19] = new ButtonWidget(133, yOff, "9", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[20] = new ButtonWidget(199, yOff, "F3", 40, 30, TFT_BLACK, greyBg, greyBorder);
  yOff += bbKeys[1]->height() + spacing;

  // - row 7
  bbKeys[21] = new ButtonWidget(1, yOff, "*", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[22] = new ButtonWidget(67, yOff, "0", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[23] = new ButtonWidget(133, yOff, "#", 40, 30, TFT_BLACK, greyBg, greyBorder);
  bbKeys[24] = new ButtonWidget(199, yOff, "F4", 40, 30, TFT_BLACK, greyBg, greyBorder);
  yOff += bbKeys[1]->height() + spacing;

  for (int i=0; i<sizeof(bbKeys)/sizeof(bbKeys[0]); i++) {
    this->registerWidget(bbKeys[i]);
  }

  // Initialize widget text
  this->updateVoltage();
  this->updateUsb();
  this->updateIP();
  this->updateRSSI();
  this->updateScannerAndCodec();
  this->changeState(MAIN);
}

DiagnosticsApp::~DiagnosticsApp() {
  // all widgets must be registered and are deleted by WiPhoneApp destructor
  audio->shutdown();
  allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
}

void DiagnosticsApp::updateVoltage() {
  char buff[10];

  // Update battery gauge info
  auto& volt = controlState.battVoltage;
  auto& soc = controlState.battSoc;
  snprintf(buff, sizeof(buff), "%.2fV", controlState.battVoltage);
  bVoltage->setText(buff);
  snprintf(buff, sizeof(buff), "%.0f%%", controlState.battSoc);
  bStateOfCharge->setText(buff);
  if (controlState.battVoltage >= 3.7 || controlState.battSoc >= 80) {
    bVoltage->setColors(TFT_BLACK, greenBg, greenBorder);
    bStateOfCharge->setColors(TFT_BLACK, greenBg, greenBorder);
  } else if (controlState.battVoltage <= 3.3 || controlState.battSoc < 20) {
    bVoltage->setColors(TFT_BLACK, greenBg, greenBorder);
    bStateOfCharge->setColors(TFT_BLACK, greenBg, greenBorder);
  } else {
    bVoltage->setColors(TFT_BLACK, yellowBg, yellowBorder);
    bStateOfCharge->setColors(TFT_BLACK, yellowBg, yellowBorder);
  }

  //log_d("Voltages: %.3f -> %.3f -> %.3f (%.3f)", lastVoltages[-3], lastVoltages[-2], lastVoltages[-1], volt);
  //log_d("SOCs: %.2f -> %.2f -> %.2f (%.2f)", lastSocs[-3], lastSocs[-2], lastSocs[-1], soc);
  if (abs(volt-lastVoltages[-1])>=0.005) {    // voltage needs to change by at least 5 mv at a time
    lastVoltages.forcePut(volt);
    log_d("Voltage: %.2fV", volt);
  }
  if (abs(soc-lastSocs[-1])>=0.05) {          // SOC needs to change by at least 0.05% at a time (this is especially sensitive towards the end)
    lastSocs.forcePut(soc);
    log_d("State of Charge: %s", buff);
  }
  //log_d("Voltages: %.3f -> %.3f -> %.3f (%.3f)", lastVoltages[-3], lastVoltages[-2], lastVoltages[-1], volt);
  //log_d("SOCs: %.2f -> %.2f -> %.2f (%.2f)", lastSocs[-3], lastSocs[-2], lastSocs[-1], soc);

  bool vRising  = lastVoltages[-3] != 0.0 && lastVoltages[-1] > lastVoltages[-2] && lastVoltages[-2] > lastVoltages[-3];
  bool vFalling = lastVoltages[-1] < lastVoltages[-2] && lastVoltages[-2] < lastVoltages[-3];
  bool sRising  = lastSocs[-3] != 0.0 && lastSocs[-1] > lastSocs[-2] && lastSocs[-2] > lastSocs[-3];
  bool sFalling = lastSocs[-1] < lastSocs[-2] && lastSocs[-2] < lastSocs[-3];
  char usbState[16] = "Discharging";
  if ((vRising || sRising) && !(vFalling || sFalling)) {
    if (lastCharging != 1) {
      strcpy(usbState, "Charging");
      log_d("USB State: %s", usbState);
      bCharging->setText("Charging");
      bCharging->setColors(TFT_BLACK, greenBg, greenBorder);
      lastCharging = 1;
    }
  } else if (!(vRising || sRising) && (vFalling || sFalling)) {
    if (lastCharging != -1) {
      strcpy(usbState, "Discharging");
      log_d("USB State: %s", usbState);
      bCharging->setText("Discharging");
      bCharging->setColors(TFT_BLACK, redBg, redBorder);
      lastCharging = -1;
    }
  } else {
    if (lastCharging != 0) {
      strcpy(usbState, (lastSocs[-1] > lastSocs[-2] && lastSocs[-2] != 0) ? "Charging?" : "Discharging?");
      log_d("USB State: %s", usbState);
      bCharging->setText(usbState);
      bCharging->setColors(TFT_BLACK, yellowBg, yellowBorder);
      lastCharging = 0;
    }
  }

  // note: the SD card holder in the first batches of boards has a permanantly grounded card detect pin
  //       those boards will always report a card present
  //       additionally, this test only checks the detect pin state.
  //       better to test by using an actual write and read (see test_sd_card() below)
  //if (lastSd != (int8_t) controlState.cardPresent) {
  //  bool &sd = controlState.cardPresent;
  //  log_d("SD card: %s", sd? "present" : "none");
  //  bCardPresence->setColors(TFT_BLACK, sd ? greenBg : greyBg, sd ? greenBorder : greyBorder);
  //  lastSd = (int8_t) sd;
  //}

  if (lastSd != true) {
    bool sd = test_sd_card();
    bCardPresence->setColors(TFT_BLACK, sd ? greenBg : greyBg, sd ? greenBorder : greyBorder);
    lastSd = (int8_t) sd;
  }
}

void DiagnosticsApp::updateUsb() {
  if (lastUsb != (int8_t) controlState.usbConnected) {
    bool &usb = controlState.usbConnected;
    log_d("USB State: %s", usb ? "Unplugged" : "Connected");
    bUsbPresence->setColors(TFT_BLACK, usb ? greenBg : greyBg, usb ? greenBorder : greyBorder);
    lastUsb = (int8_t) usb;
  }
  if (lastAutonomous != !controlState.usbConnected) {
    bAutonomous->setColors(TFT_BLACK, greenBg, greenBorder);
    lastAutonomous = true;
  }
}

void DiagnosticsApp::updateIP() {
  IPAddress ipAddr = WiFi.localIP();
  if (lastIpAddr != ipAddr) {
    log_d("IP Address: %d.%d.%d.%d", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3]);
    char buff[16];
    snprintf(buff, sizeof(buff), "%d.%d.%d.%d", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3]);
    bIpAddress->setColors(TFT_BLACK, (uint32_t) ipAddr ? greenBg : greyBg, (uint32_t) ipAddr ? greenBorder : greyBorder);
    bIpAddress->setText(buff);
    lastIpAddr = ipAddr;
  }
}

void DiagnosticsApp::updateRSSI() {
  if (controlState.wifiRssi != lastRssi) {
    log_d("RSSI: %d", controlState.wifiRssi);
    char buff[10];
    snprintf(buff, sizeof(buff), "RSSI: %d", controlState.wifiRssi);
    bRSSI->setText(buff);
    if (controlState.wifiRssi <= -70) {
      bRSSI->setColors(TFT_BLACK, yellowBg, yellowBorder);
    } else if (controlState.wifiRssi == 0) {
      bRSSI->setColors(TFT_BLACK, greyBg, greyBorder);
    } else {
      bRSSI->setColors(TFT_BLACK, greenBg, greenBorder);
    }
    lastRssi = controlState.wifiRssi;
  }
}

void DiagnosticsApp::updateScannerAndCodec() {
  if (controlState.scannerInited != lastScannerInited) {
    log_d("Key Scanner Inited: yes");
    bKeyScanner->setColors(TFT_BLACK, controlState.scannerInited ? greenBg : redBg, controlState.scannerInited ? greenBorder : redBorder);
    lastScannerInited = controlState.scannerInited;
  }
  if (controlState.codecInited != lastCodecInited) {
    log_d("Audio Codec Inited: yes");
    bCodec->setColors(TFT_BLACK, controlState.codecInited ? greenBg : redBg, controlState.scannerInited ? greenBorder : redBorder);
    lastCodecInited = controlState.codecInited;
  }
}

void DiagnosticsApp::updateUptime() {
  uint32_t uptime = millis()/1000, closing;
  bool change = false;
  char buff[30];
  if (uptime < 3600) {
    closing = uptime % 60;    // seconds
    if (closing != lastUptimeClosing) {
      snprintf(buff, sizeof(buff), "Up: %02d:%02d", (uptime%3600)/60, closing);
      lastUptimeClosing = closing;
      change = true;
    }
  } else if (uptime < 3600*24) {
    closing = uptime % 60;    // seconds
    if (closing != lastUptimeClosing) {
      snprintf(buff, sizeof(buff), "Up:%2d:%02d:%02d", uptime / 3600, (uptime%3600)/60, closing);
      lastUptimeClosing = closing;
      change = true;
    }
  } else {
    closing = (uptime%3600)/60;   // minutes
    if (closing != lastUptimeClosing) {
      snprintf(buff, sizeof(buff), "Up:%2dd%02d:%02d", uptime/(24*3600), (uptime%(24*3600) / 3600), closing);
      lastUptimeClosing = closing;
      change = true;
    }
  }
  if (change) {
    bUptime->setText(buff);
  }
}

void DiagnosticsApp::updateDB() {
  // we should probably do something reasonable about making sure we aren't going to conflict
  // with something else already using these differently, but for now ignore that possibility
  allPinMode(38, INPUT);

  allPinMode(EXTENDER_PIN(10), OUTPUT);
  allPinMode(EXTENDER_PIN(11), OUTPUT);
  allPinMode(EXTENDER_PIN(12), OUTPUT);
  allPinMode(EXTENDER_PIN(13), OUTPUT);
  allPinMode(EXTENDER_PIN(14), OUTPUT);
  allPinMode(EXTENDER_PIN(15), OUTPUT);

  //allPinMode(25, OUTPUT);
  //allPinMode(15, OUTPUT);
  allPinMode(12, OUTPUT);
  allPinMode(27, OUTPUT);
  allPinMode(32, OUTPUT);
  allPinMode(13, OUTPUT);
  allPinMode(14, OUTPUT);

  if (dbCounter < 1 || allDigitalRead(38)) {
    allDigitalWrite(EXTENDER_PIN(10), HIGH);
    allDigitalWrite(EXTENDER_PIN(11), HIGH);
    allDigitalWrite(EXTENDER_PIN(12), HIGH);
    allDigitalWrite(EXTENDER_PIN(13), HIGH);
    allDigitalWrite(EXTENDER_PIN(14), HIGH);
    allDigitalWrite(EXTENDER_PIN(15), HIGH);
    //allDigitalWrite(25, HIGH); used by I2C, can't easily override
    //allDigitalWrite(15, HIGH);
    allDigitalWrite(12, HIGH);
    allDigitalWrite(27, HIGH);
    allDigitalWrite(32, HIGH);
    allDigitalWrite(13, HIGH);
    allDigitalWrite(14, HIGH);
    dbCounter++;
  } else {
    uint32_t val = audio->getMicAvg();
    val = val>>7;
    printf("mic: %u\r\n", val);

    allDigitalWrite(EXTENDER_PIN(10), LOW);
    allDigitalWrite(EXTENDER_PIN(11), LOW);
    allDigitalWrite(EXTENDER_PIN(12), LOW);
    allDigitalWrite(EXTENDER_PIN(13), LOW);
    allDigitalWrite(EXTENDER_PIN(14), LOW);
    allDigitalWrite(EXTENDER_PIN(15), LOW);


    //allDigitalWrite(25, LOW);
    //allDigitalWrite(15, LOW);

    allDigitalWrite(12, LOW);
    allDigitalWrite(27, LOW);
    allDigitalWrite(32, LOW);
    allDigitalWrite(13, LOW);
    allDigitalWrite(14, LOW);
    if (dbCounter > 0) {
      dbCounter = 0;
    }
  }
}

void DiagnosticsApp::updatePing() {
  if (this->pingedAll) {
    log_d("pinged all");
    return; // false;
  }
  const char* host = NULL;
  IPAddress addr((uint32_t)0);
  int i = this->nextToPing++;
  /* 🛑 BOUND THE INDEX. `nextToPing++` is unconditional but `pingedAll` was only ever set
   * inside the `wifiRssi != 0` branch below — so with WiFi down or out of range the
   * sequence never terminated, and `bbPings[i]->setText()` at the bottom ran with i past
   * the end of a two-element array. At i==2 that reads the next member and calls setText()
   * on a NULL `this`, which immediately dereferences it: LoadProhibited, reboot, about
   * three seconds after one press of DOWN in Diagnostics. Deterministic. */
  if (i >= (int)(sizeof(bbPings) / sizeof(bbPings[0]))) {
    this->nextToPing = 0;
    this->pingedAll = true;
    return;
  }
  if (controlState.wifiRssi != 0) {
    switch(i) {
    case 0:
      addr = WiFi.localIP();
      if ((uint32_t)addr != 0) {
        addr[3] = 1;
      }
      break;

    case 1:
      host = "bing.com";

    //  break;
    //case 2:
    //  host = "sanjose2.voip.ms";
    //  break;
    //case 3:
    //  host = "amsterdam1.voip.ms";
    //  break;
    //case 4:
    //  host = "sip.antisip.com";
    //  break;
    //case 5:
    //  host = "sip.lax.didlogic.net";
    //  break;
    //case 6:
    //  host = "sip.nyc.didlogic.net";
    //  break;
    //case 7:
    //  host = "voip.ms";


    // fallthrough for last
    default:
      this->nextToPing = 0;
      this->pingedAll = true;
      break;
    }
  }

  log_d("PINGING...");
  /* Initialised: on the no-network path nothing below writes it before strlen()/extStrdup()
   * read it. */
  char buff[100] = "";
  bool res = false;
  int received = 0;
  if (addr || host && addr.fromString(host)) {
    snprintf(buff, sizeof(buff), "Pinging %d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
    log_d("%s", buff);
    ping_start(addr, 3, 1, 332, 1);
    res = (received = ping_get_received()) == 3;
  } else if (host) {
    snprintf(buff, sizeof(buff), "Pinging %s", host);
    log_d("%s", buff);
    IPAddress addr = resolveDomain(host);
    ping_start(addr, 3, 1, 332, 1);
    res = (received = ping_get_received()) == 3;
  }
  log_d(" - DONE");

  if (addr) {
    snprintf(buff, sizeof(buff), "%d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
  } else if (host) {
    snprintf(buff, sizeof(buff), "%s", host);
  }
  if (received) {
    if (!res) {
      snprintf(buff+strlen(buff), sizeof(buff), " %d/3", received);
    }
    int avgTime = ping_get_mean();
    snprintf(buff+strlen(buff), sizeof(buff), " %dms", avgTime);
  }
  bbPings[i]->setText(buff);
  if (received & res) {
    bbPings[i]->setColors(TFT_BLACK, greenBg, greenBorder);
  } else {
    bbPings[i]->setColors(TFT_BLACK, redBg, redBorder);
  }

  // Mark as having no address
  if (nextToPing == 0 && i!=sizeof(bbPings)/sizeof(bbPings[0])-1) {
    for (int j=i+1; j<sizeof(bbPings)/sizeof(bbPings[0]); j++) {
      if (bbPings[j]) {
        bbPings[j]->setText("No address to ping");
      }
    }
    if (this->pingedAll) {
      log_d("pinged all (2)");
      return; // true;
    }
  }

  return; // true;
}

void DiagnosticsApp::updateMic(void) {
  // flash the keypad LEDs based on the mic level
  // this is intended to let us test if the mic is soldered
  // correctly even before the screen or speakers are installed
  uint32_t val = audio->getMicAvg();
  val = val>>7;
  //allAnalogWrite(KEYBOARD_LED, val);// Why doesn't this work? Supposedly each pin on the GPIO extender supports PWM.
  allDigitalWrite(KEYBOARD_LED, val > 40 ? LOW : HIGH);
}

void DiagnosticsApp::toggleSpeaker(void) {
  static bool toggle = true;
  audio->chooseSpeaker(toggle);
  //allDigitalWrite(KEYBOARD_LED, LOW);

  //allDigitalWrite(VIBRO_MOTOR_CONTROL, toggle);

  //allDigitalWrite(KEYBOARD_LED, HIGH);
  //allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
  toggle = !toggle;
  printf("toggled speaker\r\n");
}

bool DiagnosticsApp::selfTest(void) {
  bool ip_ok = false;
  bool gauge_ok = false;
  bool extender_ok = false;
  bool scanner_ok = false;
  bool codec_ok = false;
  bool psram_ok = false;
  bool sd_ok = false;
  bool memtest_ok = false;
  bool self_check_passed = false;

  printf("\r\n\r\n SELF TEST BEGIN\r\n\r\n");

  char buff[25];
  uint8_t mac[6];
  wifiState.getMac(mac);
  printf("Chip ID: %02X:%02X:%02X:%02X:%02X:%02X\r\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  IPAddress ipAddr = WiFi.localIP();
  ip_ok = (ipAddr[0] != 0)? true : false;
  printf("IP Address: %d.%d.%d.%d\r\n", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3]);
  if (ip_ok) {
    printf("Has IP: ok\r\n");
  } else {
    printf("No IP: failed\r\n");
  }
  printf("USB State: %s\r\n", controlState.usbConnected ? "Connected" : "Unplugged");
  gauge_ok = controlState.gaugeInited;
  printf("Battery Gauge Inited: %s\r\n", gauge_ok? "yes" : "no");
  extender_ok = controlState.extenderInited;
  printf("GPIO Extender Inited: %s\r\n", extender_ok? "yes" : "no");
  scanner_ok = controlState.scannerInited;
  printf("Key Scanner Inited: %s\r\n", scanner_ok? "yes" : "no");
  codec_ok = controlState.codecInited;
  printf("Audio Codec Inited: %s\r\n", codec_ok? "yes" : "no");
  psram_ok = controlState.psramInited;
  printf("PSRAM Inited: %s\r\n", psram_ok? "yes" : "no");

  sd_ok = test_sd_card();
  printf("SD OK: %s\r\n", sd_ok? "yes" : "no");

  memtest_ok = test_memory();

  if(ip_ok && gauge_ok && extender_ok && scanner_ok && codec_ok && psram_ok && sd_ok && memtest_ok) {
    self_check_passed = true;
    printf("\r\n SELF TEST PASSED\r\n\r\n");
    return true;
  }
  printf("\r\n SELF TEST FAILED\r\n\r\n");
  return false;
}

void DiagnosticsApp::changeState(DiagnosticsView_t newState) {
  log_d("DiagnosticsApp::changeState %d", (int) newState);
  if (newState == MAIN) {

    controlState.msAppTimerEventLast = millis();
    controlState.msAppTimerEventPeriod = 100;    // 10 updates per second, but this will be cut down to a display update rate of 1 fps elsewhere

    this->updateVoltage();
    this->updateIP();
    this->updateRSSI();
    this->updateUsb();
    this->updateScannerAndCodec();
    this->updateDB();
    this->selfTest();

  } else if (newState == NETWORKS) {
    audio->ceasePlayback();

    controlState.msAppTimerEventLast = millis();
    controlState.msAppTimerEventPeriod = 1000;    // 1 second delay before doing next ping

    this->nextToPing = 0;
    this->pingedAll = false;

    for (int i=0; i<sizeof(bbPings)/sizeof(bbPings[0]); i++) {
      if (bbPings[i]) {
        bbPings[i]->setText("Pinging...");
        bbPings[i]->setColors(TFT_BLACK, greyBg, greyBorder);
      }
    }

  } else if (newState == KEYPAD) {

    controlState.msAppTimerEventPeriod = 0;
    for (int i=0; i<sizeof(bbKeys)/sizeof(bbKeys[0]); i++) {
      bbKeys[i]->setColors(TFT_BLACK, greyBg, greyBorder);
    }
    memset(keyPressed, 0, sizeof(keyPressed));
    anyKeyPressed = false;

  } else if (newState == AUDIO) {
    controlState.msAppTimerEventLast = millis();
    controlState.msAppTimerEventPeriod = 33;    // 30 fps

  } else if (newState == SCREEN) {
    audio->ceasePlayback();
    printf("testing screen\r\n");
    controlState.msAppTimerEventLast = millis();
    controlState.msAppTimerEventPeriod = 500;    // 2 fps
  }
  appState = newState;
  screenInited = false;
}

appEventResult DiagnosticsApp::processEvent(EventType event) {

  if (!controlState.booted) {
    return DO_NOTHING;
  }

  static bool testPassed = false;
  static bool audioOn = false;
  static int splitter = 0;
  if (splitter > 29) {
    splitter = 0;
  }

  if (audioOn == true) {
    audio->loop();
    this->updateMic();
  }

#ifndef DIAGNOSTICS_ONLY
  if (LOGIC_BUTTON_BACK(event) && !(appState == KEYPAD && anyKeyPressed && keyPressed[5]<EXIT_CNT && keyPressed[8]<EXIT_CNT)) {
    return EXIT_APP;
  }
#endif
  appEventResult res = DO_NOTHING;
  if (event == WIPHONE_KEY_DOWN && !(appState == KEYPAD && anyKeyPressed && keyPressed[7]<EXIT_CNT)) {
    // Change current state
    DiagnosticsView_t newState = MAIN;
    allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
    switch(appState) {
    case MAIN:
      newState = NETWORKS;
      break;
    case NETWORKS:
      newState = AUDIO;
      break;
    case AUDIO:
      newState = SCREEN;
      screenStep = 0;
      break;
    case SCREEN:
      newState = KEYPAD;
      break;
    default:
      break;  // just loop back to MAIN
    }
    this->changeState(newState);
    res |= REDRAW_SCREEN;
  } else if (appState == MAIN) {
    if (event == BATTERY_UPDATE_EVENT) {
      this->updateVoltage();
      res |= REDRAW_SCREEN;
    } else if (event == APP_TIMER_EVENT || event == WIFI_ICON_UPDATE_EVENT) {      // NOTE: rssi is update once every two seconds, app timer event is once every second
      //if (controlState.codecInited && splitter > 28 ) {
      if (controlState.booted && controlState.codecInited) {
        //if (!audio->isOn()) {
        if ( !audioOn ) {
#ifdef DIAGNOSTICS_ONLY
          /* 🛑 12,604 BYTES OF INTERNAL DRAM — and until 2026-08-16 this was compiled into
           * EVERY build, including production, where it can never run.
           *
           * The workaround itself is fine and stays for the mode it was written for: in
           * DIAGNOSTICS_ONLY builds the global audio object isn't wired up, so this screen
           * makes its own. But `static` means the compiler reserves the object's BSS
           * unconditionally, whether or not the branch is reachable — and sizeof(Audio) is
           * 0x313c. `xtensa-esp32-elf-nm --size-sort` showed TWO 12,604-byte Audio objects in
           * the image: setup()'s, which is the real one, and this one.
           *
           * This phone has ~20 KB of free internal heap with WiFi and SIP up, which is what
           * every crash today has come down to. Handing 12.6 KB of it to an object that
           * production can never reach was most of the reason the margin was so thin.
           *
           * ⚠ It also reassigned the GLOBAL `audio` pointer, so in a production build merely
           * opening Diagnostics would have swapped the whole phone's audio device to a second
           * instance — the shared-state bug class that has bitten this codebase repeatedly. */
          static Audio audio_local(true, I2S_BCK_PIN, I2S_WS_PIN, I2S_MOSI_PIN, I2S_MISO_PIN);
          audio = &audio_local;
#endif // DIAGNOSTICS_ONLY

          audio->shutdown();

          if (audio->start()) {
            audio->turnMicOn();
            audio->playRingtone(&SPIFFS);
            audioOn = true;
          } else {
            printf("audio: failed\r\n");
          }
        }

        if (splitter == 0) {
          this->updateIP();
          this->updateRSSI();
          this->updateScannerAndCodec();
          this->updateUptime();
          testPassed = this->selfTest();
        }
        if (splitter % 5 == 0) {
          this->updateDB();
        }
        if (splitter == 15 && audioOn) {
          this->toggleSpeaker();
        }

        // turn the motor on once every second if we fail
        // or once each 3 seconds if we pass
        int count = splitter;
        if ( !testPassed ) {
          count = splitter%10;
        }
        if ( count == 3 ) {
          printf("motor on\r\n");
          allDigitalWrite(VIBRO_MOTOR_CONTROL, HIGH);
        }
        if ( count == 7 ) {
          printf("motor off\r\n");
          allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
        }

        if (splitter%10 == 0) { // redraw the screen only once each 10 updates (once per second)
          res |= REDRAW_SCREEN;
        }
      }
    } else if (event == USB_UPDATE_EVENT) {
      this->updateUsb();
      res |= REDRAW_SCREEN;
    }
  } else if (appState == NETWORKS) {
    if (event == APP_TIMER_EVENT) {
      this->updatePing();
      res |= REDRAW_SCREEN;
    }
  } else if (appState == AUDIO) {
    if (event == APP_TIMER_EVENT) {
      res |= REDRAW_SCREEN;
    }
    if (event == '1') {
      if (!audio->getHeadphones()) {
        audio->chooseSpeaker(false);
        audio->playRingtone(&SPIFFS);
        printf("playing from ear speaker\r\n");
      } else {
        printf("headphones connected, ignoring\r\n");
      }
    }
    if (event == '2') {
      if (!audio->getHeadphones()) {
        audio->chooseSpeaker(true);
        audio->playRingtone(&SPIFFS);
        printf("playing from loudspeaker\r\n");
      } else {
        printf("headphones connected, ignoring\r\n");
      }
    }
    if (event == '3') {
      if (audio->getHeadphones()) {
        audio->chooseSpeaker(false);
        audio->playRingtone(&SPIFFS);
        printf("playing from headphone jack\r\n");
      } else {
        printf("no headphones connected, ignoring\r\n");
      }
    }
    if (event == '4') {
      audio->ceasePlayback();
    }
  } else if (appState == SCREEN) {
    if (event == APP_TIMER_EVENT) {
      res |= REDRAW_SCREEN;
    }
  } else if (appState == KEYPAD) {
    if (IS_KEYBOARD(event)) {
      anyKeyPressed = true;
      unsigned int i = 0xeeee;
      log_d("Keypad Test, Pressed: %d", event);
      switch(event) {
      case WIPHONE_KEY_UP:
        i=0;
        break;
      case WIPHONE_KEY_SELECT:
        i=1;
        break;
      case WIPHONE_KEY_LEFT:
        i=2;
        break;
      case WIPHONE_KEY_OK:
        i=3;
        break;
      case WIPHONE_KEY_RIGHT:
        i=4;
        break;
      case WIPHONE_KEY_BACK:
        i=5;
        break;
      case WIPHONE_KEY_CALL:
        i=6;
        break;
      case WIPHONE_KEY_DOWN:
        i=7;
        break;
      case WIPHONE_KEY_END:
        i=8;
        break;
      case '1':
        i=9;
        break;
      case '2':
        i=10;
        break;
      case '3':
        i=11;
        break;
      case WIPHONE_KEY_F1:
        i=12;
        break;
      case '4':
        i=13;
        break;
      case '5':
        i=14;
        break;
      case '6':
        i=15;
        break;
      case WIPHONE_KEY_F2:
        i=16;
        break;
      case '7':
        i=17;
        break;
      case '8':
        i=18;
        break;
      case '9':
        i=19;
        break;
      case WIPHONE_KEY_F3:
        i=20;
        break;
      case '*':
        i=21;
        break;
      case '0':
        i=22;
        break;
      case '#':
        i=23;
        break;
      case WIPHONE_KEY_F4:
        i=24;
        break;
      default:
        break;
      }
      if (i<sizeof(keyPressed)/sizeof(keyPressed[0])) {
        keyPressed[i]++;
        if (keyPressed[i]==1) {
          bbKeys[i]->setColors(TFT_BLACK, blueBg, blueBorder);
        } else if (keyPressed[i]==2) {
          bbKeys[i]->setColors(TFT_BLACK, yellowBg, yellowBorder);
        } else if (keyPressed[i]==3) {
          bbKeys[i]->setColors(TFT_BLACK, greenBg, greenBorder);
        }
        res |= REDRAW_SCREEN;
      }
    }
  }
  splitter++;
  return res;
}

void DiagnosticsApp::redrawScreen(bool redrawAll) {
  if ((!screenInited || redrawAll) && (appState != SCREEN)) {
    //lcd.fillRect(0, header->height(), lcd.width(), lcd.height() - header->height() - footer->height(), TFT_BLACK);
    lcd.fillRect(0, 0, lcd.width(), lcd.height(), TFT_BLACK);
  }

  if (appState == MAIN) {

    ((GUIWidget*) bVoltage)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bStateOfCharge)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bUsbPresence)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bCardPresence)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bCharging)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bAutonomous)->refresh(lcd, redrawAll || !screenInited);

    ((GUIWidget*) bBatteryGauge)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bGpioExtender)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bKeyScanner)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bSpiRam)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bCodec)->refresh(lcd, redrawAll || !screenInited);

    ((GUIWidget*) bRSSI)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bUptime)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bVersion)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bMacAddress)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) bIpAddress)->refresh(lcd, redrawAll || !screenInited);

    if (!screenInited) {
      lcd.setTextColor(TFT_WHITE, TFT_BLACK);
      lcd.setTextSize(1.5);
      lcd.setTextFont(2);
      lcd.drawString("Press down for more tests", 75, 280);
    }

  } else if (appState == NETWORKS) {

    for (int i=0; i<sizeof(bbPings)/sizeof(bbPings[0]); i++) {
      if (bbPings[i]) {
        ((GUIWidget*) bbPings[i])->refresh(lcd, redrawAll || !screenInited);
      }
    }
  } else if (appState == AUDIO) {
    if (!screenInited || redrawAll) {
      //lcd.fillScreen(TFT_BLACK);
      lcd.setTextColor(TFT_WHITE, TFT_BLACK);
      lcd.setTextSize(2);
      lcd.setTextFont(2);
      lcd.drawString("Front Mic", 10, 40);
      lcd.drawString("Audio Out", 10, 90);

      lcd.setTextSize(1.3);
      //lcd.setTextFont(2);
      lcd.drawString("Press this key to test:", 10, 110);
      lcd.drawString("#1: Front Speaker", 25, 130);
      lcd.drawString("#2: Rear Speaker", 25, 150);
      lcd.drawString("#3: Headphones", 25, 170);
      lcd.drawString("#4: Stop", 25, 190);
    }
    uint32_t val = audio->getMicAvg();
    uint8_t stp = 0;
    uint16_t xOff = 0;
    uint16_t colorStep = 17;
    uint16_t xStep = 16;
    do {
      uint8_t R = colorStep*stp;
      lcd.drawFastHLine(xOff, 60, xStep, lcd.color565(R, 255-R, 0));
      xOff += xStep;
      val >>= 1;
      stp++;
    } while (val && xOff < lcd.width()-xStep);
    lcd.drawFastHLine(xOff, 60, lcd.width()-xOff, TFT_BLACK);

    /*bool hp = audio->getHeadphones();
    testEarSpeaker->setColors(TFT_BLACK, greyBg, greyBorder);

    ((GUIWidget*) testEarSpeaker)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) testLoudSpeaker)->refresh(lcd, redrawAll || !screenInited);
    ((GUIWidget*) testHeadphone)->refresh(lcd, redrawAll || !screenInited);
    */

  } else if (appState == SCREEN) {
    switch (screenStep) {
    case 0:
      lcd.fillRect(0, 0, lcd.width(), lcd.height(), TFT_BLACK);
      screenStep++;
      break;
    case 1:
      allAnalogWrite(LCD_LED_PIN, 0);
      screenStep++;
      break;
    case 2:
      lcd.fillRect(0, 0, lcd.width(), lcd.height(), TFT_WHITE);
      screenStep++;
      break;
    case 3:
      allAnalogWrite(LCD_LED_PIN, 255);
      screenStep++;
      break;
    case 4:
      lcd.drawFastHLine(5, 5, lcd.width()-10, TFT_BLACK);
      lcd.drawFastHLine(5, lcd.height()-5, lcd.width()-10, TFT_BLACK);
      lcd.drawFastVLine(5, 5, lcd.height()-10, TFT_BLACK);
      lcd.drawFastVLine(lcd.width()-5, 5, lcd.height()-10, TFT_BLACK);
      screenStep++;
      break;
    case 5:
      screenStep++;
      break;
    case 6:
      screenStep++;
      break;
    case 7:
      lcd.setTextColor(TFT_BLACK, TFT_WHITE);
      lcd.setTextSize(2);
      lcd.setTextFont(2);
      lcd.drawString("LCD TEST", 120, 150);
      screenStep = 2;
      break;
    default:
      screenStep = 0;
      break;
    }

  } else if (appState == KEYPAD) {
    for (int i=0; i<sizeof(bbKeys)/sizeof(bbKeys[0]); i++) {
      ((GUIWidget*) bbKeys[i])->refresh(lcd, redrawAll || !screenInited);
    }
  }
  screenInited = true;
}
#ifdef BUILD_GAMES
// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Chess app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ChessApp::ChessApp(Audio* audio, LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer, ChessVariant_t variant)
  : WindowedApp(lcd, state, header, footer), audio(audio), variant(variant) {
  state.msAppTimerEventLast = millis();
  state.msAppTimerEventPeriod = 250;    // 4 fps

  const char* p;
  switch (variant) {
  case Normal:
    p = "Chess";
    break;
  case KingOfTheHill:
    p = "King of the Hill";
    break;
  case Chess960:
    p = "Fischer Random";
    break;
  default:
    p = "WiPhone Chess";
    break;
  }
  header->setTitle(p);
  footer->setButtons("Black", "Back");    // TODO: implement black button

  // Prepare icons
  cell_black = new IconRle3(icon_cell_0, sizeof(icon_cell_0));
  cell_white = new IconRle3(icon_cell_1, sizeof(icon_cell_1));
  sel_black = new IconRle3(icon_sel_0, sizeof(icon_sel_0));
  sel_white = new IconRle3(icon_sel_1, sizeof(icon_sel_1));
  cursor_frame = new IconRle3(icon_select_piece, sizeof(icon_select_piece));

  pieces_b[PAWN] = new IconRle3(icon_pawn_0, sizeof(icon_pawn_0));
  pieces_b[KNGT] = new IconRle3(icon_knight_0, sizeof(icon_knight_0));
  pieces_b[BISH] = new IconRle3(icon_bishop_0, sizeof(icon_bishop_0));
  pieces_b[ROOK] = new IconRle3(icon_rook_0, sizeof(icon_rook_0));
  pieces_b[QUEN] = new IconRle3(icon_queen_0, sizeof(icon_queen_0));
  pieces_b[KING] = new IconRle3(icon_king_0, sizeof(icon_king_0));

  pieces_w[PAWN] = new IconRle3(icon_pawn_1, sizeof(icon_pawn_1));
  pieces_w[KNGT] = new IconRle3(icon_knight_1, sizeof(icon_knight_1));
  pieces_w[BISH] = new IconRle3(icon_bishop_1, sizeof(icon_bishop_1));
  pieces_w[ROOK] = new IconRle3(icon_rook_1, sizeof(icon_rook_1));
  pieces_w[QUEN] = new IconRle3(icon_queen_1, sizeof(icon_queen_1));
  pieces_w[KING] = new IconRle3(icon_king_1, sizeof(icon_king_1));

  engine = new FairyMax::FairyMax(&this->post);
  if (engine != NULL) {
    log_d("xboard");
    engine->exchange("xboard");
    log_d("protover");
    engine->exchange("protover");
    log_d("post");
    engine->exchange("post");
    log_d("st 5");
    engine->exchange("st 5");        // maximum search time (seconds)
    log_d("sd 4");
    engine->exchange("sd 4");         // maximum search depth
    log_d("new");
    engine->exchange("new");

    // NOTE: "variant" should be called after "new" (the latter command loads normal variant)
    if (variant == Chess960) {
      log_d("variant");
      engine->exchange("variant fischerandom");     // TODO: currently not implemented in the Fairy-Max engine
    } else if (variant == KingOfTheHill) {
      log_d("variant");
      engine->exchange("variant king-of-the-hill");
    }

    log_d("board");
    engine->exchange("board");

    log_d("running");
    engineRunning = true;
  }
}

ChessApp::~ChessApp() {
  log_d("deleting engine");
  if (engine) {
    delete engine;
  }

  log_d("deleting icons");
  for (int i=0; i<6; i++) {
    delete pieces_w[i];
    delete pieces_b[i];
  }
  delete cell_black;
  delete cell_white;
  delete sel_black;
  delete sel_white;
  delete cursor_frame;
}

void ChessApp::post(const char* feedback) {
  if (static_lcd && feedback[0]!='\0') {
    int yOff = 270;   // hardcoded
    static_lcd->fillRect(0, yOff, static_lcd->width(), 10, BLACK);      // hardcoded height
    // Draw info message
    static_lcd->setTextColor(GREEN, TFT_BLACK);
    static_lcd->setTextDatum(TL_DATUM);
    static_lcd->setTextSize(1);
    static_lcd->setTextFont(1);
    static_lcd->drawString(feedback, 5, yOff + 1);
    //if (feedback[0]=='M') delay(400);     // delay for MAX.STACK events for debugging
  }
}

appEventResult ChessApp::processEvent(EventType event) {
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }
  if (engineRunning && event == APP_TIMER_EVENT) {
    log_d("- exchange");
    if (processEngine("")) {
      return REDRAW_SCREEN;
    }
  } else if (IS_KEYBOARD(event)) {
    if (!(event == WIPHONE_KEY_OK || event == WIPHONE_KEY_CALL)) {
      info = "";  // reset user message
    }
    bool unknown = false;
    if (event == WIPHONE_KEY_UP) {
      if (cursor >= 8) {
        cursor -= 8;
      }
    } else if (event == WIPHONE_KEY_DOWN) {
      if (cursor < 56) {
        cursor += 8;
      }
    } else if (event == WIPHONE_KEY_LEFT) {
      if ((cursor % 8) > 0) {
        cursor--;
      }
    } else if (event == WIPHONE_KEY_RIGHT) {
      if ((cursor % 8) < 7) {
        cursor++;
      }
    } else if (event == WIPHONE_KEY_SELECT) {
      footer->setButtons("Go", "Back");         // TODO: pressing Go a few times crashes the system
      if (processEngine("go")) {
        return REDRAW_SCREEN;
      }
    } else if (event == WIPHONE_KEY_OK || event == WIPHONE_KEY_CALL) {
      if (src < 0) {
        if (board[cursor] != EMPTY) {
          src = cursor;
        }
      } else if (src >= 0) {
        if (cursor == src) {
          src = -1;
        } else {
          makeMove(src, cursor);
        }
      }
    } else {
      unknown = true;
    }
    if (!unknown) {
      return REDRAW_SCREEN | REDRAW_FOOTER;
    }
  }
  return DO_NOTHING;
}

bool ChessApp::processEngine(const char* msg) {
  switch(engine->exchange(msg)) {
  case FairyMax::XboardChessEngine::Quit:
    engineRunning = false;
    log_d("- engine: quit");
    break;
  case FairyMax::XboardChessEngine::EmptyInput:
    log_d("- engine: empty");
    break;
  case FairyMax::XboardChessEngine::Continue:
    log_d("- engine: cont");
    break;
  default:
    break;
  }
  info = "";
  bool redraw = false;
  if (engine->output.length()) {
    const char* cstr = engine->output.c_str();
    log_d("Engine output: %s", cstr);
    while (*cstr) {
      if (!strncmp(cstr, "move ", 5)) {
        // Implement computer move
        cstr += 4;                          // skip "move"
        cstr += strspn(cstr, " \t");        // skip whitespace
        int len = strcspn(cstr, " \t\n");   // length of the move token
        char mov[7];
        strncpy(mov, cstr, len);
        mov[len] = '\0';
        log_d("computer move: %s", mov);

        // Show message to the user
        info = "Computer move: ";
        info += mov;

        // Execute computer move
        char promotion = (len>4) ? mov[4] : '\0';
        int8_t src, dst;
        decodeMove(mov, src);
        decodeMove(mov+2, dst);
        makeMove(src, dst, true, promotion);

        redraw = true;
        cstr += len;
      } else if (!strncmp(cstr, "Illegal move:", 13)) {
        // Show message to the user
        cstr += 13;
        int len = strcspn(cstr, "\n");
        char str[len+1];
        strncpy(str, cstr, len);
        str[len] = '\0';
        info = "Illegal move: ";
        info += str;
        cstr += len;
        // Restore state
        memcpy(board, board_backup, sizeof(board));
        redraw = true;
      } else if (!strncmp(cstr, "0-1", 3) || !strncmp(cstr, "1-0", 3) || !strncmp(cstr, "1/2-1/2", 7) || !strncmp(cstr, "resign", 6)) {       // TODO: doesn't show the "resign" message
        // Show message to the user
        int len = strcspn(cstr, "\n");
        char str[len+1];
        strncpy(str, cstr, len);
        str[len] = '\0';
        info = str;
        cstr += len;
      } else {
        int len = strcspn(cstr, "\n");
        if (*cstr != '#' && !info.length()) {
          char str[len + 1];
          strncpy(str, cstr, len);
          str[len] = '\0';
          info = str;
        }
        // skip this line
        cstr += len;
      }
      if (*cstr=='\n') {
        cstr++;
      }
    }
    engine->output = "";
  }
  if (info.length()>0 && !redraw) {
    this->post(info.c_str());  // special case for when there is only message, but the board is not to be redrawn
  }
  return redraw;
}

void ChessApp::encodeMove(int8_t lin, char& file, char& rank) {
  file = 'a' + (lin % 8);
  rank = '1' + (7 - (lin / 8));
}

void ChessApp::decodeMove(const char* mov, int8_t& lin) {
  lin = (mov[0]-'a') + (7-(mov[1]-'1'))*8;
}

void ChessApp::makeMove(uint8_t frm, uint8_t to, bool engineMove, char promotion) {
  if (!engineMove)
    // Preserve state for unmove
  {
    memcpy(board_backup, board, sizeof(board));
  }

  // Convert into coordinate notation
  char mov[13];
  char r1, f1, r2, f2;
  encodeMove(frm, f1, r1);
  encodeMove(to, f2, r2);
  sprintf(mov, "%c%c%c%c\n", f1, r1, f2, r2);
  log_d("move: %s", mov);

  // Check for special cases
  if ((board[frm] & 0xF) == PAWN && (r2 == '1' || r2 == '8')) {
    //  - moving a pawn to last ranks -> promotion
    // TODO: allow underpromotions for the user
    uint8_t typ;
    char prom;
    switch(tolower(promotion)) {
    case 'n':
      typ = KNGT;
      prom = 'n';
      break;
    case 'b':
      typ = BISH;
      prom = 'b';
      break;
    case 'r':
      typ = ROOK;
      prom = 'r';
      break;
    default:
      typ = QUEN;
      prom = 'q';
      break;
    }
    board[frm] = (board[frm] & 0xF0) | typ;       // update pawn to a higher piece (typically queen)
    sprintf(mov, "%c%c%c%c%c\n", f1, r1, f2, r2, prom);
    if (engineMove) {
      info += "; promotion";
    } else {
      info == "Promotion";
    }
  } else if (*mov=='e' && (board[frm] & 0xF) == KING) {
    // - castling: check for one of four possible moves and move the rook accordingly
    bool castling = false;
    if (!strncmp(mov, "e1g1", 4)) {
      board[61] = board[63];
      board[63] = EMPTY;
      castling = true;
    } else if (!strncmp(mov, "e1c1", 4)) {
      board[59] = board[56];
      board[56] = EMPTY;
      castling = true;
    } else if (!strncmp(mov, "e8g8", 4)) {
      board[5] = board[7];
      board[7] = EMPTY;
      castling = true;
    } else if (!strncmp(mov, "e8c8", 4)) {
      board[3] = board[0];
      board[0] = EMPTY;
      castling = true;
    }
    if (castling) {
      if (engineMove) {
        info += "; castling";
      } else {
        info = "Casting";
      }
    }
  } else if ( (board[frm] & 0xF) == PAWN && board[to]==EMPTY &&
              ( ((to - frm == 7 || to - frm == 9) && (board[frm] & BL)) || ((to - frm == -7 || to - frm == -9) && !(board[frm] & BL)) ) )   {
    // - moving a pawn to an empty square diagonally -> en-passant, kill the passing pawn
    if (to - frm == -7 || to - frm == 9) {
      board[frm + 1] = EMPTY;
    } else {
      board[frm - 1] = EMPTY;
    }
    if (engineMove) {
      info += "; en passant";
    } else {
      info = "En passant";
    }
  }

  // Show move on board;
  board[to] = board[frm];
  board[frm] = EMPTY;
  src = -1;
  cursor = to;

  if (!engineMove) {
    log_d("sending move to the engine: %s", mov);
    processEngine(mov);
  }
}

void ChessApp::redrawScreen(bool redrawAll) {
  log_d("redrawScreen ChessApp");

  // Draw chess board
  int cellSize = ((lcd.width() < lcd.height()) ? lcd.width() : lcd.height()) / 8;
  int yOff = header->height();
  bool white = true;
  for (int i=0; i<64; i+=8) {
    int xOff = 0;
    for (int j=0; j<8; j++) {
      int8_t lin = i + j;
      if (lin != src) {
        lcd.drawImage(white ? *cell_white : *cell_black, xOff, yOff);
      } else {
        lcd.drawImage(white ? *sel_white : *sel_black, xOff, yOff);
      }
      uint8_t piece = board[lin];
      if (!(piece & EMPTY)) {
        lcd.drawImage( **(((piece & BL) ? pieces_b : pieces_w) + (piece & typeMask)), xOff+2, yOff+2);
      }
      if (lin == cursor) {
        lcd.drawImage(*cursor_frame, xOff, yOff);
      }
      xOff += cellSize;
      white = !white;
    }
    yOff += cellSize;
    white = !white;
  }
  if (variant == KingOfTheHill) {
    lcd.drawRect(3*cellSize, header->height()+3*cellSize, 2*cellSize, 2*cellSize, TFT_BLACK);
  }

  // Draw black padding below
  if (lcd.height() - yOff - footer->height() > 0) {
    lcd.fillRect(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(), BLACK);
  }

  // Draw info message
  if (info.length() > 0) {
    lcd.setTextColor(GREEN, TFT_BLACK);
    lcd.setTextDatum(TL_DATUM);
    lcd.setTextSize(1);
    lcd.setTextFont(1);
    lcd.drawString(info.c_str(), 5, yOff+1);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Ackman game  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

AckmanApp::AckmanApp(Audio* audio, LCD& lcd, ControlState& state) : WiPhoneApp(lcd, state), audio(audio), sprite(&lcd) {

  assert(AckmanApp::cellSize*AckmanApp::width  <= lcd.width());
  assert(AckmanApp::cellSize*AckmanApp::height <= lcd.height() - 3);
  this->gridXOff = (lcd.width()  - AckmanApp::cellSize*AckmanApp::width)  / 2;
  this->gridYOff = (lcd.height() - AckmanApp::cellSize*AckmanApp::height) - 5;

  // Restore high score from flash
  IniFile ini(AckmanApp::filename);
  if (ini.load() && !ini.isEmpty()) {
    this->highScore = ini[0].getIntValueSafe(AckmanApp::highField, 0);
  }

  sprite.setColorDepth(16);
  sprite.createSprite(AckmanApp::agentSize, AckmanApp::agentSize);
  if (!sprite.isCreated()) {
    log_e("sprite not created");
  }

  this->resetGame();
  this->parseLevel(ackmanLevels[this->level]);
  this->startGame();
  //audio->start();
}

AckmanApp::~AckmanApp() {
  /* ⚠ TFT_eSprite HAS NO DESTRUCTOR anywhere in the vendored library — the pixel buffer is
   * released only by an explicit deleteSprite(). Without this the buffer was stranded on
   * every open/exit. The small sprites are the stability problem, not the big one:
   * esp32Calloc() tries plain calloc() first, so anything that fits comes from the INTERNAL
   * heap and is left mid-heap, splitting the free space `largest` measures. deleteSprite()
   * early-returns when nothing was created, so this is unconditionally safe. */
  sprite.deleteSprite();
  //audio->shutdown();
  if (this->score > this->highScore) {
    this->saveHighScore(this->score);
  }
}

void AckmanApp::parseLevel(const char* level) {
  this->foodCnt = 0;
  this->warpCnt = 0;
  this->screenInited = false;
  this->nextAckmanDir = None;
  this->chewingTime = 0;
  this->moveCnt = 0;
  this->scaredTimer = 0;

  for (auto i = 0; i < sizeof(agents)/sizeof(agents[0]); i++) {
    agents[i].moving = false;
    agents[i].state = Absent;
    agents[i].outside = i <= 1;     // only ackman and the first enemy is outside
    agents[i].dirOffset = 0.0;
    agents[i].x = agents[i].origX = 0;
    agents[i].y = agents[i].origY = 0;
  }

  // Parse textual level representation
  memset(grid, 0, sizeof(grid));
  for (int j=0; j<AckmanApp::height; j++) {
    for (int i=0; i<AckmanApp::width; i++) {
      switch (*level) {
      case '.':
        grid[j][i] = crumbFlag;
        this->foodCnt++;
        break;
      case 'o':
        grid[j][i] = breadFlag;
        this->foodCnt++;
        break;
      case '-':
        grid[j][i] = doorFlag;
        break;
      case 'X':
        grid[j][i] = wallFlag;
        break;
      case '<': // if you want to have two sets of warping tunnels, they have to be opposite of one another for this to work right
      case '{':
        grid[j][i] = warpLeftFlag;
        warps[warpCnt].x = i;
        warps[warpCnt++].y = j;
        break;
      case '>':
      case '}':
        grid[j][i] = warpRightFlag;
        warps[warpCnt].x = i;
        warps[warpCnt++].y = j;
        break;
      case 'P':
        agents[0].origX = i;
        agents[0].origY = j;
        agents[0].state = Normal;
        break;
      case 'B':
        agents[1].origX = i;
        agents[1].origY = j;
        agents[1].state = Normal;
        break; // TODO: hardcoded positions in the array
      case 'R':
        agents[2].origX = i;
        agents[2].origY = j;
        agents[2].state = Normal;
        break;
      case 'M':
        agents[3].origX = i;
        agents[3].origY = j;
        agents[3].state = Normal;
        break;
      case 'S':
        agents[4].origX = i;
        agents[4].origY = j;
        agents[4].state = Normal;
        break;
      case ' ':
      default:
        break;
      }
      level++;
    }
  }

  // Calculate positions of "nodes" (crossings between different paths)
  for (int j=0; j<AckmanApp::height; j++) {
    for (int i=0; i<AckmanApp::width; i++) {
      if (grid[j][i] & (wallFlag | doorFlag)) {
        continue;
      }
      bool horiz = (i && !(grid[j][i-1] & wallFlag)) || (i+1 < AckmanApp::width  && !(grid[j][i+1] & wallFlag));
      bool verti = (j && !(grid[j-1][i] & wallFlag)) || (j+1 < AckmanApp::height && !(grid[j+1][i] & wallFlag));
      if (horiz && verti) {
        grid[j][i] |= nodeFlag;
      }
    }
  }

  // Reset enemy states
  for (auto i = 0; i < sizeof(agents)/sizeof(agents[0]); i++) {
    if (agents[i].state == Absent) {
      continue;
    }
    agents[i].moving = true;
    this->respawn(&agents[i]);
  }

}

void AckmanApp::respawn(struct Agent* agent) {
  agent->x = agent->origX;
  agent->y = agent->origY;
  agent->state = Normal;
  agent->outside = agent->typ == Bloody;      // only Bloody's original position is outside (by convention)

  // Pick a random direction which makes sense
  auto off = Random.random() % 4;
  for (auto d = 0; d < 4; d++) {
    AgentDirection_t dir = (AgentDirection_t) ((d + off) % 4);
    if (this->isRelevantDir(agent, dir)) {
      agent->dir = dir;
      break;
    }
  }
}

void AckmanApp::updateAgentPosition(struct Agent* agent) {

  // Get position of the agent on screen
  auto& x = agent->screenX;
  auto& y = agent->screenY;

  // - calculate center of the "cell" (that's where a crumb would be drawn)
  x = this->gridXOff + AckmanApp::cellSize/2 + AckmanApp::cellSize * agent->x;
  y = this->gridYOff + AckmanApp::cellSize/2 + AckmanApp::cellSize * agent->y;

  // - calculate offset of the agent in pixels
  float delta = (agent->dirOffset < 1.0 ? agent->dirOffset : 1.0) * AckmanApp::cellSize;
  switch (agent->dir) {
  case North:
    y = round(y - delta);
    break;
  case East:
    x = round(x + delta);
    break;
  case South:
    y = round(y + delta);
    break;
  case West:
    x = round(x - delta);
    break;
  }

}

void AckmanApp::drawAgent(struct Agent* agent, bool draw) {

  this->updateAgentPosition(agent);

  auto& x = agent->screenX;
  auto& y = agent->screenY;
  const auto R = AckmanApp::agentSize / 2;

  // Do the actual screen modifications
  if (draw) {

    sprite.fillSprite(transparent);

    if (agent->typ == Ackman) {

      // Draw ackman
      // NOTE: sprite for ackman is needed to avoid blinking while standing
      sprite.fillCircle(R, R, R, TFT_YELLOW);

      // Draw ackman's mouth
      int32_t t = abs(this->chewingTime - AckmanApp::chewingPeriod / 2);    // how open the mouth is? (3 - fully open, 0 - closed)

      if (t>=1) {
        t++;  // skip t==1 as it is ugly (looks like a thick line)
      }

      // - default calculation: mouth facing westward
      int32_t dx1 = (t != 0 ? (AckmanApp::cellSize - 4)/2 : 0) - (t == AckmanApp::chewingPeriod / 2 ? 1 : 0);
      int32_t dy1 = 0;
      int32_t dx2 = -R;
      int32_t dy2 = t;
      int32_t dx3 = -R;
      int32_t dy3 = -t;

      // - mirroring and rotation based on direction
      switch (agent->dir) {
      case East:
        dx1 = -dx1;
        dx2 = -dx2;
        dx3 = -dx3;
        break;
      case North:
      case South:
        t = dx1;
        dx1 = dy1;
        dy1 = t;
        t = dx2;
        dx2 = dy2;
        dy2 = t;
        t = dx3;
        dx3 = dy3;
        dy3 = t;
        break;
      }
      if (agent->dir == South) {
        dy1 = -dy1;
        dy2 = -dy2;
        dy3 = -dy3;
      }

      // - actually draw the mouth
      sprite.fillTriangle(R + dx1, R + dy1,
                          R + dx2, R + dy2,
                          R + dx3, R + dy3,
                          transparent);

    } else {

      // Draw enemy body
      if (agent->state != Eaten) {
        colorType color;
        if (agent->state != Scared) {
          switch (agent->typ) {
          case Bloody:
            color = 0xF800;
            break;     // Red
          case Rosy:
            color = 0xFB56;
            break;     // Hot Pink
          case Moody:
            color = 0x64BD;
            break;     // Cornflower Blue
          case Sunny:
            color = 0xFD20;
            break;     // Orange
          }
        } else {
          color = 0x0011;   // Dark Blue
        }
        // NOTE: sprite is used to reduce blinking in scared enemies' eyes
        sprite.fillRoundRect(0, 0, AckmanApp::agentSize, AckmanApp::agentSize, 3, color);
      }

      // Draw enemy eyes
      const auto d = 3;         // distance between eyes
      int dx = 0;               // eye offset
      int ddx = 0, ddy = 0;     // iris offset
      switch (agent->dir) {
      case North:
        ddy = -1;
        break;
      case East:
        ddx =  1;
        break;
      case South:
        ddy =  1;
        break;
      case West:
        ddx = dx = -1;
        break;      //  for shifting entire eyes to the left when moving west
      }
      if (agent->state != Scared) {

        // Eyes look normal
        sprite.fillCircle(R - d + dx, R - d, 2, TFT_WHITE);
        sprite.fillCircle(R + d + dx, R - d, 2, TFT_WHITE);
        sprite.fillCircle(R - d + dx + ddx, R - d + ddy, 1, TFT_BLACK);
        sprite.fillCircle(R + d + dx + ddx, R - d + ddy, 1, TFT_BLACK);

      } else {

        // Eyes look like crosses
        const auto er = 2;
        sprite.drawLine(R - d + dx - er, R - d - er, R - d + dx + er, R - d + er,  GRAY_75);
        sprite.drawLine(R - d + dx - er, R - d + er, R - d + dx + er, R - d - er,  GRAY_75);
        sprite.drawLine(R + d + dx - er, R - d - er, R + d + dx + er, R - d + er,  GRAY_75);
        sprite.drawLine(R + d + dx - er, R - d + er, R + d + dx + er, R - d - er,  GRAY_75);

      }

      // Draw mouth
      // TODO
    }

    // Push agent sprite
    sprite.pushSprite(x - R, y - R, transparent);

  } else {
    // Clear: draw black rectangle over it
    int32_t x0 = x - R;
    lcd.fillRect(x0 >= 0 ? x0 : 0, y - R,
                 AckmanApp::agentSize + (x0 >= 0 ? 0 : x0),
                 AckmanApp::agentSize,
                 TFT_BLACK);
  }
}

appEventResult AckmanApp::processEvent(EventType event) {
  if (LOGIC_BUTTON_BACK(event)) {
    return EXIT_APP;
  }

  if (event == APP_TIMER_EVENT) {

    if (this->gameState == Playing) {

      // New move
      this->moveCnt++;

      // Update timers
      if (this->scaredTimer) {
        this->scaredTimer--;
        if (!this->scaredTimer) {
          for (auto i = 1; i < sizeof(agents)/sizeof(agents[0]); i++)
            if (agents[i].state == Scared) {
              agents[i].state = Normal;
            }
        }
      }

      // Process each agent individually
      bool highScore = false;
      struct Agent *agent = agents;
      for (int i = 0; i < sizeof(agents)/sizeof(agents[0]); i++, agent++) {
        if (!agent->moving) {
          this->drawAgent(agent, true);     // redraw ackman in case it was cleared by an eaten enemy
          continue;
        } else if (!i) {
          this->chewingTime = (this->chewingTime + 1) % AckmanApp::chewingPeriod;
        }

        // Clear on screen
        this->drawAgent(agent, false);

        // Redraw objects
        if (grid[agent->y][agent->x] & (crumbFlag | breadFlag | doorFlag)) {
          if (grid[agent->y][agent->x] & (crumbFlag | breadFlag)) {
            this->drawFood(agent->x, agent->y);
          } else {
            this->drawDoors();
          }
        }

        // Move
        switch (agent->state) {
        case Normal:
          agent->dirOffset += AckmanApp::normalSpeed;
          break;
        case Scared:
          agent->dirOffset += AckmanApp::normalSpeed*0.4;
          break;
        case Eaten:
          agent->dirOffset += AckmanApp::normalSpeed*2;
          break;
        }

        // Check food collisions
        if (agent->typ == Ackman) {
          uint8_t tx, ty;
          this->getDest(agent, tx, ty);
          if ((grid[ty][tx] & crumbFlag) && agent->dirOffset >= 0.4) {
            this->drawFood(tx, ty, true);   // clear food
            this->foodCnt--;
            grid[ty][tx] &= ~crumbFlag;
            this->score += 1;
            this->drawScore(false);
          } else if ((grid[ty][tx] & breadFlag) && agent->dirOffset >= 0.3) {
            this->drawFood(tx, ty, true);   // clear food
            this->foodCnt--;
            grid[ty][tx] &= ~breadFlag;
            this->score += 5;
            this->drawScore(false);
            this->scaredTimer = AckmanApp::scaredPeriod;
            for (auto i = 1; i < sizeof(agents)/sizeof(agents[0]); i++)
              if (agents[i].state == Normal) {
                agents[i].state = Scared;
              }
          }
          if (!this->foodCnt) {
            // Proceed to next level
            this->freezeGame();
            this->level = (this->level + 1) % (sizeof(ackmanLevels)/sizeof(ackmanLevels[0]));
            this->setState(LevelOver);
          }
        }

        // Check if direction of movement needs to be changed
        while (agent->dirOffset >= 1.0) {

          // reached new cell -> update state
          agent->dirOffset -= 1.0;
          this->nextCell(agent);

          if (agent->typ == Ackman) {

            // Set new direction if relevant
            if (this->nextAckmanDir != None && this->isRelevantDir(agent, this->nextAckmanDir)) {
              agent->dir = this->nextAckmanDir;
              this->nextAckmanDir = None;
            }

            // Check if still can move in current direction
            if (!this->isRelevantDir(agent, agent->dir)) {
              agent->moving = false;
              agent->dirOffset = 0.0;
              this->nextAckmanDir = None;       // previous direction was irrelevant
            }

          } else {

            // Calculate new direction for an enemy
            if (!this->isRelevantDir(agent, agent->dir) || grid[agent->y][agent->x] & nodeFlag) {
              this->newEnemyDirection(agent);
            }

          }
        }

        // Calculate screen position
        this->updateAgentPosition(agent);

        // Check enemy collision
        if (agent->typ != Ackman) {
          auto dist = abs(agent->x - agents[0].x) + abs(agent->y - agents[0].y);    // distance to ackman
          if (dist <= 2) {
            // TODO: better collision detection (by drawing in a sprite and seeing if figures are connected)
            //          float screenDist = this->agentDistance(0, i);
            //          bool caught = false;
            //          if (agent->x == agents[0].x || agent->y == agents[0].y)
            //            caught = screenDist <= 0.9*AckmanApp::agentSize;
            //          else
            //            caught = screenDist <= 1.2*AckmanApp::agentSize;
            if (this->agentDistance(0, i) <= 0.75*AckmanApp::agentSize) {     // Ms. Pac-Man allows significant overlap before actual collision

              if (agent->state == Normal) {

                // Game over
                this->freezeGame();
                this->setState(GameOver);

                if (this->score > this->highScore) {
                  this->highScore = this->score;
                  this->drawScore(true, true);
                  highScore = true;
                }

              } else if (agent->state == Scared) {

                // Enemy is eaten
                this->score += 10;
                agent->state = Eaten;

              }

            }
          }
        }

        // Draw on screen
        this->drawAgent(agent, true);

      }

      // Save high score to flash only after all agents are redrawn (to avoid visible lag)
      if (highScore) {
        this->saveHighScore(this->highScore);
      }

      return DO_NOTHING;

    } else if (this->gameState == Ready) {

      this->setState(Playing);

    } else if (this->gameState == LevelOver || this->gameState == GameOver) {

      // Restart level
      if (this->gameState == GameOver) {
        this->resetGame();
      }
      this->parseLevel(ackmanLevels[this->level]);
      this->startGame();
      return REDRAW_SCREEN;

    }
  } else if (IS_KEYBOARD(event)) {

    if (this->gameState != GameOver) {

      // Process ackman movement

      if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_RIGHT || event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_LEFT) {
        switch (event) {
        case WIPHONE_KEY_UP:
          this->nextAckmanDir = North;
          break;
        case WIPHONE_KEY_RIGHT:
          this->nextAckmanDir = East;
          break;
        case WIPHONE_KEY_DOWN:
          this->nextAckmanDir = South;
          break;
        case WIPHONE_KEY_LEFT:
          this->nextAckmanDir = West;
          break;
        }

        if (agents[0].moving) {
          // Check if can change direction immediately, otherwise: wait till move is finished (arrived to the center of the next cell)
          bool opposing = this->nextAckmanDir == North && agents[0].dir == South ||
                          this->nextAckmanDir == South && agents[0].dir == North ||
                          this->nextAckmanDir == West && agents[0].dir == East ||
                          this->nextAckmanDir == East && agents[0].dir == West;
          if (opposing) {
            this->nextCell(&agents[0]);
            agents[0].dir = this->nextAckmanDir;
            agents[0].dirOffset = 1.0 - agents[0].dirOffset;
            this->nextAckmanDir = None;
          }
        } else {
          // If Ackman is not moving, check if the selected movement makes sense, if it does - start moving
          if (this->isRelevantDir(agents, this->nextAckmanDir)) {
            agents[0].dir = this->nextAckmanDir;
            agents[0].moving = true;
          }
          this->nextAckmanDir = None;
        }
      }

    }
  }
  return DO_NOTHING;
}

void AckmanApp::saveHighScore(int32_t highScore) {
  // Save high score to flash
  IniFile ini(AckmanApp::filename);
  ini[0][AckmanApp::highField] = highScore;
  ini.store();
}

float AckmanApp::agentDistance(uint8_t i, uint8_t j) {
  int a = agents[i].screenX - agents[j].screenX;
  int b = agents[i].screenY - agents[j].screenY;
  return sqrt(a*a + b*b);
}

// Move agent to the given direction (or to his own direction)
void AckmanApp::nextCell(struct Agent* agent, AgentDirection_t dir) {
  if (dir == None) {
    dir = agent->dir;
  }
  this->getDest(agent, dir, agent->x, agent->y);
  if (grid[agent->y][agent->x] & doorFlag) {
    // Passing a door
    if (agent->outside && agent->state == Eaten) {
      agent->state = Normal;
    }
    agent->outside = !agent->outside;
  }
}

// Where is this agent going?
void AckmanApp::getDest(struct Agent* agent, uint8_t& x, uint8_t& y) {
  this->getDest(agent, agent->dir, x, y);
}

void AckmanApp::getDest(struct Agent* agent, AgentDirection_t dir, uint8_t& x, uint8_t& y) {
  x = agent->x;
  y = agent->y;
  switch (dir) {
  case North:
    y--;
    break;
  case South:
    y++;
    break;
  case East:
    if (grid[y][x] & warpRightFlag) {
      this->moveToWarp(x, y);
    } else {
      x++;
    }
    break;
  case West:
    if (grid[y][x] & warpLeftFlag) {
      this->moveToWarp(x, y);
    } else {
      x--;
    }
    break;
  }
}

void AckmanApp::startGame() {
  this->setState(Ready);
}

void AckmanApp::freezeGame() {
  for (auto i = 0; i < sizeof(agents)/sizeof(agents[0]); i++) {
    agents[i].moving = false;
  }
}

void AckmanApp::resetGame() {
  // Reset variables to mark beginning of the game (score, lives, level, etc.)
  this->score = 0;
  this->level = 0;
}

void AckmanApp::setState(GameState_t state) {
  this->gameState = state;
  switch (state) {
  case Ready:
    controlState.msAppTimerEventPeriod = 2000;
    break;
  case LevelOver:
    controlState.msAppTimerEventPeriod = 2500;
    break;
  case GameOver:
    this->drawMessage(true, false);
    controlState.msAppTimerEventPeriod = 3500;
    break;
  case Playing:
    this->drawMessage(false, true);   // clear Ready! message
    controlState.msAppTimerEventPeriod = 33;
    break;     // 30 fps
  }
  controlState.msAppTimerEventLast = millis();
  this->gameState = state;
}

void AckmanApp::moveToWarp(uint8_t& x, uint8_t& y) {
  for (auto i = 0; i < warpCnt; i++) {
    if (warps[i].x == x && warps[i].y == y) {
      i = (i & 1) ? i - 1 : i + 1;
      x = warps[i].x;
      y = warps[i].y;
      return;
    }
  }
  log_e("warp not found");
}

void AckmanApp::newEnemyDirection(struct Agent* agent) {

  // Choose current movement type for this agent
  AgentType_t movementType = agent->typ;
  if (!agent->outside) {
    movementType = Sunny;       // enemies move randomly "inside" their home
  } else if (agent->state == Scared || agent->state == Eaten) {
    movementType = Bloody;      // scared enemies act the same trying to move away from ackman semirandomly
  } else if (agent->state == Normal && this->moveCnt <= AckmanApp::confusedPeriod && agent->typ != Moody) {
    movementType = Sunny;       // Bloody and Rosy act randomly the first few seconds
  } else if (agent->typ == Moody) {
    switch (Random.random() % 4) {
    case 0:
      movementType = Bloody;
      break;  // chase
    case 1:
      movementType = Rosy;
      break;  // chase
    default:
      movementType = Sunny;
      break;  // wander
    }
  }

  // Don't go back inside, unless Eaten
  uint8_t obstr = wallFlag;
  if (agent->outside && agent->state != Eaten) {
    obstr |= doorFlag;
  }

  // See what directions are possible
  AgentDirection_t moves[4];
  uint32_t chance[4];
  for (auto i = 0; i < 4; i++) {
    uint8_t ncell = 0;
    chance[i] = 0;
    moves[i] = (AgentDirection_t) i;
    if ((AgentDirection_t) i == North) {
      if (!agent->y) {
        continue;
      }
      ncell = grid[agent->y - 1][agent->x];
    } else if ((AgentDirection_t) i == East) {
      if (agent->x + 1 >= AckmanApp::width) {
        continue;
      }
      ncell = grid[agent->y][agent->x + 1];
    } else if ((AgentDirection_t) i == South) {
      if (agent->y + 1 >= AckmanApp::height) {
        continue;
      }
      ncell = grid[agent->y + 1][agent->x];
    } else if ((AgentDirection_t) i == West) {
      if (!agent->x) {
        continue;
      }
      ncell = grid[agent->y][agent->x - 1];
    }
    if (!(ncell & obstr)) {
      chance[i] = (ncell & doorFlag) ? 10 : 1;  // strongly prefer to cross the door if possible
    }
  }

  // Calculate next move chances based on movement type

  if (movementType == Bloody || movementType == Rosy) {

    // Bloody chases Ackman, Rosy tries to get in front of Ackman

    // What is the target cell?
    uint8_t tx, ty;
    if (movementType == Bloody) {
      if (agent->state == Normal || agent->state == Scared) {
        // for Bloody movement type in Normal state -> "target" is the current ackman position
        tx = agents[0].x;
        ty = agents[0].y;
      } else {
        // for Bloody movement type in Eaten state -> target is the original position
        tx = agent->origX;
        ty = agent->origY;
      }
    } else {
      // for Rosy -> projected ackman position
      this->projectAgent(&agents[0], agents[0].dir, tx, ty);
    }

    // Modify chances of each move based on the target cell
    const uint32_t factor = 1000;     // the bigger it is, the more best move will be preferred, the more it will be predictable
    uint32_t mn = factor;
    for (auto i = 0; i < 4; i++) {
      if (chance[i]) {
        uint8_t nx, ny;
        this->projectAgent(agent, (AgentDirection_t) i, nx, ny);
        chance[i] *= factor / (abs(nx - tx) + abs(ny - ty) + 1);
        if (!chance[i]) {
          chance[i] = 1;
        }
        if (chance[i] < mn) {
          mn = chance[i];
        }
      }
    }
    // - cutoff (makes the worst moves less possible)
    mn -= mn/4;
    for (auto i = 0; i < 4; i++)
      if (chance[i]) {
        chance[i] -= mn;
      }

  } else if (movementType == Sunny) {

    // Random movement while preferring to move in same direction, avoiding moving backwards
    uint32_t curDir = (uint32_t) agent->dir;
    for (auto i = 0; i < 4; i++) {
      if (chance[i]) {
        chance[i] *= (i != (curDir + 2) % 4) ? (curDir == i ? 200 : 100) : 16;
      }
    }

  }

  // "Reverse" chances, if the enemy is scared
  if (agent->state == Scared) {
    uint32_t sum = chance[0] + chance[1] + chance[2] + chance[3];
    for (auto i = 0; i < 4; i++) {
      if (chance[i]) {
        chance[i] = sum / chance[i];
        if (!chance[i]) {
          chance[i] = 1;
        }
      }
    }
  }

  // Select move randomly
//  switch (agent->typ) {
//    case Bloody: DEBUG_PRINTF("BLOODY"); break;
//    case Rosy:   DEBUG_PRINTF("ROSY"); break;
//    case Moody:  DEBUG_PRINTF("MOODY"); break;
//    case Sunny:  DEBUG_PRINTF("SUNNY"); break;
//  }
//  switch (movementType) {
//    case Bloody: DEBUG_PRINTF("/BLOODY"); break;
//    case Rosy:   DEBUG_PRINTF("/ROSY"); break;
//    case Sunny:  DEBUG_PRINTF("/SUNNY"); break;
//  }
  auto draw = Random.random() % (chance[0] + chance[1] + chance[2] + chance[3]);
//  DEBUG_PRINTF(" %d,%d,%d,%d", chance[0], chance[1], chance[2], chance[3]);
//  DEBUG_PRINTF(", rnd=%d", draw);
  for (auto i = 0; i < 4; i++) {
    if (chance[i] && draw <= chance[i]) {
      agent->dir = moves[i];
//      log_d(", m=%d", i);
      break;
    } else {
      draw -= chance[i];
    }
  }
}

bool AckmanApp::isRelevantDir(struct Agent* agent, AgentDirection_t dir) {
  uint8_t obstr = wallFlag;
  if (agent->typ == Ackman || agent->outside && agent->state != Eaten) {
    obstr |= doorFlag;
  }
  switch (dir) {
  case North:
    if (!agent->y) {
      return false;
    }
    if (grid[agent->y-1][agent->x] & obstr) {
      return false;
    }
    break;
  case East:
    if (grid[agent->y][agent->x] & warpRightFlag) {
      return true;
    }
    if (agent->x + 1 >= AckmanApp::width) {
      return false;
    }
    if (grid[agent->y][agent->x+1] & obstr) {
      return false;
    }
    break;
  case South:
    if (agent->y + 1 >= AckmanApp::height) {
      return false;
    }
    if (grid[agent->y+1][agent->x] & obstr) {
      return false;
    }
    break;
  case West:
    if (grid[agent->y][agent->x] & warpLeftFlag) {
      return true;
    }
    if (!agent->x) {
      return false;
    }
    if (grid[agent->y][agent->x-1] & obstr) {
      return false;
    }
    break;
  }
  return true;
}

void AckmanApp::projectAgent(struct Agent* agent, AgentDirection_t dir, uint8_t& x, uint8_t& y) {
  struct Agent futureAgent = *agent;
  while (this->isRelevantDir(&futureAgent, dir)) {
    this->nextCell(&futureAgent, dir);
    if (grid[futureAgent.y][futureAgent.x] & nodeFlag) {
      break;
    }
  }
  x = futureAgent.x;
  y = futureAgent.y;
}

void AckmanApp::redrawScreen(bool redrawAll) {
  if (!this->screenInited) {
    redrawAll = true;
  }
  if (redrawAll) {

    // Clear screen
    lcd.fillScreen(TFT_BLACK);

    // Draw text
    this->drawScore(true);
    lcd.setTextSize(2);
    lcd.drawString("Ackman", 10, 1);
    if (this->gameState == GameOver || this->gameState == Ready) {
      this->drawMessage(true, this->gameState == Ready);
    }

    // Draw border
    auto k = this->level % (sizeof(ackmanLevels)/sizeof(ackmanLevels[0]));
    colorType mazeColor = wallColors[k % (sizeof(wallColors)/sizeof(wallColors[0]))];
    lcd.drawRoundRect(this->gridXOff - AckmanApp::cellSize/2,
                      this->gridYOff - AckmanApp::cellSize/2,
                      (AckmanApp::width + 1) * AckmanApp::cellSize,
                      (AckmanApp::height + 1) * AckmanApp::cellSize,
                      4, mazeColor);

    // Draw food & maze
    for (int j=0; j<AckmanApp::height; j++) {
      for (int i=0; i<AckmanApp::width; i++) {
        if (grid[j][i] & (crumbFlag | breadFlag)) {
          this->drawFood(i, j);
        } else if (grid[j][i] & wallFlag) {
          lcd.fillCircle(getX(i), getY(j), 1, mazeColor);
          //lcd.fillRoundRect(getX(i) - 2, getY(j) - 2, 4, 4, 2, mazeColor);
          if (i && grid[j][i-1] & wallFlag || !i) {
            this->drawLine(i-1, j, i, j, mazeColor);  // connect to left
          }
          if (j && grid[j-1][i] & wallFlag || !j) {
            this->drawLine(i, j-1, i, j, mazeColor);  // connect up
          }
          if (i+1 == AckmanApp::width) {
            this->drawLine(i, j, i+1, j, mazeColor);  // connect to right
          }
          if (j+1 == AckmanApp::height) {
            this->drawLine(i, j, i, j+1, mazeColor);  // connect down
          }
        } else if (grid[j][i] & (warpLeftFlag | warpRightFlag)) {
          if (grid[j][i] & warpLeftFlag)
            lcd.drawFastVLine(this->getX(i) - AckmanApp::cellSize,
                              this->getY(j) - AckmanApp::cellSize + 2,
                              2*AckmanApp::cellSize - 3, TFT_BLACK);
          else
            lcd.drawFastVLine(this->getX(i) + AckmanApp::cellSize - 1,
                              this->getY(j) - AckmanApp::cellSize + 2,
                              2*AckmanApp::cellSize - 3, TFT_BLACK);
        }
      }
    }
    this->drawDoors();

    // Draw agents
    struct Agent* agent = agents;
    for (int i = 0; i < sizeof(agents)/sizeof(agents[0]); i++, agent++) {
      this->drawAgent(agent, true);
    }

    this->screenInited = true;
  }
}

void AckmanApp::drawFood(uint8_t i, uint8_t j, bool clear) {
  lcd.fillCircle(getX(i), getY(j), grid[j][i] & crumbFlag ? 1 : 3, clear ? TFT_BLACK : AckmanApp::foodColor);
}

void AckmanApp::drawDoors() {
  // Draw doors
  for (int j=0; j<AckmanApp::height; j++) {
    for (int i=0; i<AckmanApp::width; i++) {
      if (grid[j][i] & doorFlag) {
        lcd.fillCircle(getX(i), getY(j), 1, AckmanApp::doorColor);
        if (i && grid[j][i-1] & (doorFlag | wallFlag)) {
          this->drawLine(i-1, j, i, j, AckmanApp::doorColor);  // connect to left
        }
        if (j && grid[j-1][i] & (doorFlag | wallFlag)) {
          this->drawLine(i, j-1, i, j, AckmanApp::doorColor);  // connect up
        }
        if (i+1 < AckmanApp::width && grid[j][i+1] & (doorFlag | wallFlag)) {
          this->drawLine(i, j, i+1, j, AckmanApp::doorColor);  // connect to right
        }
        if (j+1 < AckmanApp::height && grid[j+1][i] & (doorFlag | wallFlag)) {
          this->drawLine(i, j, i, j+1, AckmanApp::doorColor);  // connect down
        }
      }
    }
  }
}

void AckmanApp::drawScore(bool redrawAll, bool newHigh) {
  lcd.setTextColor(newHigh ? TFT_GREEN : TFT_WHITE, TFT_BLACK);
  lcd.setTextFont(2);
  lcd.setTextSize(1);
  lcd.setTextDatum(TL_DATUM);

  char str[9];
  snprintf(str, sizeof(str), "%u", this->score);
  lcd.drawString(str, 56, 31);

  if (redrawAll) {
    lcd.setTextDatum(TR_DATUM);
    lcd.drawString("Score:", 49, 31);
    if (this->highScore) {
      lcd.drawString("High:", 153, 31);
      lcd.setTextDatum(TL_DATUM);
      snprintf(str, sizeof(str), "%u", this->highScore);
      lcd.drawString(str, 160, 31);
    }
  }
}

void AckmanApp::drawMessage(bool draw, bool ready) {
  if (draw) {
    lcd.setTextColor(ready ? TFT_YELLOW : TFT_RED);
    lcd.setTextFont(2);
    lcd.setTextSize(1);
    lcd.setTextDatum(TR_DATUM);
    lcd.drawString(ready ? "Ready!" : "Game Over!", 230, 10);
  } else {
    lcd.fillRect(162, 10, 68, 16, TFT_BLACK);
  }
}

inline uint16_t AckmanApp::getX(int8_t i) {
  return this->gridXOff + AckmanApp::cellSize/2 + AckmanApp::cellSize*i;
}

inline uint16_t AckmanApp::getY(int8_t j) {
  return this->gridYOff + AckmanApp::cellSize/2 + AckmanApp::cellSize*j;
}

void AckmanApp::drawLine(uint8_t i1, uint8_t j1, uint8_t i2, uint8_t j2, colorType color) {
  lcd.drawLine(getX(i1), getY(j1), getX(i2), getY(j2), color);
}

#endif

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  LED board app  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#ifdef LED_BOARD
APA102<LED_BOARD_DATA, LED_BOARD_CLOCK> ledBoard;

LedMicApp::LedMicApp(Audio* audio, LCD& lcd, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(lcd, state, header, footer), audio(audio), FocusableApp(2) {
  log_d("create LedMicApp");

  // Set timer
  state.msAppTimerEventLast = millis();
  state.msAppTimerEventPeriod = 33;     // 30 fps

  // Set titles
  header->setTitle("LED microphone");
  footer->setButtons(NULL, "Clear");

  // Background
  uint16_t yOff = header->height();
  bgRect = new RectWidget(0, yOff, lcd.width(), lcd.height() - yOff - footer->height(), WP_COLOR_1);

  // Create and arrange widgets
  addLabelInput(yOff, labels[0], inputs[0], "Scale down:", 16);
  addLabelInput(yOff, labels[1], inputs[1], "Step:", 6);

  // Set text
  inputs[0]->setText("100");
  inputs[1]->setText("1.5");
  takeInputs();

  // Initialize variables
  this->time = Random.random();

  // Precompute: colors
  for (uint16_t hue = 0; hue<360; hue++) {
    hue2rgb[hue] = LedMicApp::hsvToRgb(hue, 255, 255);
  }

  // Turn on microphone
  /* A pop first (review 3, A6 - see MicTestApp), then music (review, 2026-09-25): start()
   * restarts I2S and turnMicOn() swaps music's own ring for the microphone's under a playing
   * track. Paused, so it keeps its place. */
  notifyPopFinishFor("the LED mic app");
  musicPlayerPause();
  audio->setSampleRate(16000);         // (the rate change reached a playing track too: 0.73x)
  audio->start();
  audio->turnMicOn();

  allDigitalWrite(LED_BOARD_ENABLE, HIGH);

  // Set focusables
  for(uint16_t i=0; i<sizeof(inputs)/sizeof(TextInputWidget*); i++) {
    addFocusableWidget(inputs[i]);
  }

  setFocus(inputs[0]);
}

LedMicApp::~LedMicApp() {
  log_d("destroy LedMicApp");

  allDigitalWrite(LED_BOARD_ENABLE, LOW);

  for(uint16_t i=0; i<sizeof(labels)/sizeof(LabelWidget*); i++) {
    delete labels[i];
  }
  for(uint16_t i=0; i<sizeof(inputs)/sizeof(TextInputWidget*); i++) {
    delete inputs[i];
  }

  audio->shutdown();
}

void LedMicApp::takeInputs() {
  const char* sScaleDown = inputs[0]->getText();
  const char* sStep = inputs[1]->getText();

  scaleDown = sScaleDown && *sScaleDown ? atof(sScaleDown) : 0.0;
  step = sStep && *sStep ? atof(sStep) : 0.0;

  log_d("scale down: %f", scaleDown);
  log_d("step: %f", step);

  scale[0] = 1;
  for(int i=1; i<12; i++) {
    scale[i] = step*scale[i-1];
  }
}

/* Converts a color from HSV to RGB.
 * h is hue, as a number between 0 and 360.
 * s is the saturation, as a number between 0 and 255.
 * v is the value, as a number between 0 and 255. */
rgb_color LedMicApp::hsvToRgb(uint16_t h, uint8_t s, uint8_t v) {
  uint8_t f = (h % 60) * 255 / 60;
  uint8_t p = (255 - s) * (uint16_t)v / 255;
  uint8_t q = (255 - f * (uint16_t)s / 255) * (uint16_t)v / 255;
  uint8_t t = (255 - (255 - f) * (uint16_t)s / 255) * (uint16_t)v / 255;
  uint8_t r = 0, g = 0, b = 0;
  switch((h / 60) % 6) {
  case 0:
    r = v;
    g = t;
    b = p;
    break;
  case 1:
    r = q;
    g = v;
    b = p;
    break;
  case 2:
    r = p;
    g = v;
    b = t;
    break;
  case 3:
    r = p;
    g = q;
    b = v;
    break;
  case 4:
    r = t;
    g = p;
    b = v;
    break;
  case 5:
    r = v;
    g = p;
    b = q;
    break;
  }
  return rgb_color(r, g, b);
}

appEventResult LedMicApp::processEvent(EventType event) {
  if (event == APP_TIMER_EVENT) {

    // Get average microprohone volume
    int32_t val = audio->getMicAvg();

    // Make it close to zero
    val /= this->scaleDown>1 ? this->scaleDown : 1;
    if (!val) {
      val++;
    }

    // Update LEDs
    uint16_t hue = time % 360;
    for(uint16_t i = 0; i < LED_BOARD_COUNT; i++) {
      uint16_t j = i % 12;
      if (((i / 12) & 1)) {
        j = 11 - j;
      }

      if (val>=scale[j]) {
        colors[i] = hue2rgb[hue];
      } else {
        colors[i] = rgb_color(0, 0, 0);
      }

      hue += 360 / LED_BOARD_COUNT;
      if (hue>=360) {
        hue -= 360;
      }
    }
    ledBoard.write(colors, LED_BOARD_COUNT, LED_BOARD_BRIGHTNESS);

    // Update
    time += 2;
    return DO_NOTHING;

  } else if (event == WIPHONE_KEY_END) {

    return EXIT_APP;

  } else if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {

    nextFocus(event == WIPHONE_KEY_DOWN);
    return REDRAW_SCREEN;

  } else if (IS_KEYBOARD(event)) {

    // Pass event to a focused widget
    ((GUIWidget*) getFocused())->processEvent(event);
    takeInputs();
    return REDRAW_SCREEN;

  }

  return DO_NOTHING;
}

void LedMicApp::redrawScreen(bool redrawAll) {
  if (!screenInited || redrawAll) {
    ((GUIWidget*) bgRect)->redraw(lcd);
    // Draw labels
    for(uint16_t i=0; i<sizeof(labels)/sizeof(LabelWidget*); i++) {
      ((GUIWidget*) labels[i])->redraw(lcd);
    }
  }

  // Redraw input widgets
  for(uint16_t i=0; i<sizeof(inputs)/sizeof(TextInputWidget*); i++) {
    ((GUIWidget*) inputs[i])->redraw(lcd);
  }

  screenInited = true;
}

#endif // LED_BOARD

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  GUI WIDGETS  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

void GUIWidget::clear(LCD &lcd, uint16_t col) {
  corrRect(lcd, parentOffX, parentOffY, widgetWidth, widgetHeight, col);
}

void GUIWidget::corrRect(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight, uint16_t color) {
  lcd.fillRect(screenOffX, screenOffY,
               windowWidth + (lcd.width() == windowWidth ? 1 : 0),
               windowHeight + (lcd.height() == windowHeight ? 1 : 0),
               color);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  RectWidget - - - - - - - - - - - - - - - - - - - - - - - - - - - -

RectWidget::RectWidget(uint16_t posX, uint16_t posY, uint16_t width, uint16_t height, uint16_t color)
  : NonFocusableWidget(posX, posY, width, height), color(color) {
}

void RectWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  this->corrRect(lcd, screenOffX, screenOffY, windowWidth, windowHeight, color);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  RectIconWidget - - - - - - - - - - - - - - - - - - - - - - - - - - - -

RectIconWidget::RectIconWidget(uint16_t posX, uint16_t posY, uint16_t width, uint16_t height, uint16_t color, const uint8_t* iconData, const size_t iconSize)
  : RectWidget(posX, posY, width, height, color) {
  if (iconData) {
    icon = new IconRle3(iconData, iconSize);
  }
}

RectIconWidget::~RectIconWidget() {
  if (icon) {
    delete icon;
  }
}

void RectIconWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  this->corrRect(lcd, screenOffX, screenOffY, windowWidth, windowHeight, color);
  if (icon) {
    lcd.drawImage(*icon, screenOffX + (windowWidth - icon->width())/2, screenOffY + (windowHeight - icon->height())/2);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  RulerWidget - - - - - - - - - - - - - - - - - - - - - - - - - - - -

RulerWidget::RulerWidget(uint16_t posX, uint16_t posY, uint16_t width, uint16_t color)
  : NonFocusableWidget(posX, posY, width, 1), color(color) {
}

void RulerWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  this->corrRect(lcd, screenOffX, screenOffY, windowWidth, 1, color);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  LabelWidget - - - - - - - - - - - - - - - - - - - - - - - - - - - -

LabelWidget::LabelWidget(uint16_t posX, uint16_t posY, uint16_t width, uint16_t height, const char* str,
                         uint16_t col, uint16_t bg, SmoothFont* font, TextDirection_t orient, uint16_t xPadding)
  : NonFocusableWidget(posX, posY, width, height), widgetFont(font), textColor(col), bgColor(bg), textDirection(orient), xPadding(xPadding) {
  //log_d("create LabelWidget");

  /* Widget-lifetime text follows the widget into PSRAM — see MenuOption in GUI.h. The
   * object is already there (AbstractWidget::operator new); the string was not. */
  textDyn = extStrdup(str);
  updated = true;
  if (font==NULL) {
    widgetFont = fonts[OPENSANS_COND_BOLD_20];
  }
};

LabelWidget::~LabelWidget() {
  freeNull((void **) &textDyn);
}

void LabelWidget::setText(const char* str) {
  freeNull((void **) &textDyn);
  if (str!=NULL) {
    textDyn = extStrdup(str);
  } else {
    textDyn = extStrdup("");
  }
  updated = true;
}

void LabelWidget::setColors(colorType text, colorType bg) {
  textColor = text;
  bgColor = bg;
  updated = true;
}

void LabelWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  uint16_t off = 0;

  // Redraw background
  this->clear(lcd, bgColor);      // TODO: can be optimized to avoid double redrawing

  // Draw actual text
  lcd.setTextColor(textColor, bgColor);
  lcd.setTextFont(widgetFont);
  /* Ellipsized rather than drawFitString's hard cut — labels carry the caller's
   * name and SIP URI on the call screen, and a URI sheared mid-domain reads as
   * a whole (wrong) address. guiDrawEllipsized honours the datum set below. */
  // TODO: change text direction to datum
  if (textDirection==0) {   // left to right
    lcd.setTextDatum(ML_DATUM);
    guiDrawEllipsized(lcd, textDyn, windowWidth-(xPadding*2), screenOffX+xPadding, screenOffY+windowHeight/2);
  } else if (textDirection==1) {      // right to left
    lcd.setTextDatum(MR_DATUM);
    guiDrawEllipsized(lcd, textDyn, windowWidth-(xPadding*2), screenOffX+windowWidth-xPadding, screenOffY+windowHeight/2);
  } else if (textDirection==2) {      // center
    lcd.setTextDatum(MC_DATUM);
    guiDrawEllipsized(lcd, textDyn, windowWidth-(xPadding*2), screenOffX + windowWidth/2, screenOffY+windowHeight/2);
  }
  updated = false;
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  ChoiceWidget  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ChoiceWidget::ChoiceWidget(uint16_t posX, uint16_t posY, uint16_t width, uint16_t height, SmoothFont* font,
                           colorType textColor, colorType bgColor, colorType regColor, colorType selColor)
  : FocusableWidget(posX, posY, width, height), textColor(textColor), bgColor(bgColor), regColor(regColor), selColor(selColor) {
  widgetFont = font ? font : fonts[OPENSANS_COND_BOLD_20];
}

ChoiceWidget::~ChoiceWidget() {
  for (auto it = choices.iterator(); it.valid(); ++it) {
    free(*it);
  }
  choices.clear();
}

void ChoiceWidget::addChoice(const char* name) {
  bool succ = false;
  char* copy = extStrdup(name);
  if (copy != NULL && choices.add(copy)) {
    this->setValue(choices.size()-1);
    succ = true;
  }
  if (!succ) {
    log_e("failed to add a choice");
  }
}

void ChoiceWidget::setValue(ChoiceValue val) {
  if (this->curChoice != val) {
    this->curChoice = val;
    this->updated = true;
  }
}

bool ChoiceWidget::processEvent(EventType event) {
  /* No logging here on purpose: log_e is the only level compiled in, and this fires on
   * every left/right keypress — leftover debug spam ("MESUT") was flooding the one log
   * that has to stay readable in the field. */
  if (!this->choices.size()) {
    return false;  // no choices
  }
  if (event == WIPHONE_KEY_LEFT || event == WIPHONE_KEY_RIGHT) {
    if (event == WIPHONE_KEY_LEFT) {
      if (this->curChoice) {
        this->curChoice--;
      } else {
        this->curChoice = this->choices.size()-1;
      }
    } else {
      this->curChoice++;
      if (this->curChoice >= this->choices.size()) {
        this->curChoice = 0;
      }
    }
    return (this->updated = true);
  }
  return false;
}

void ChoiceWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  //log_d("redraw: at %d,%d, focuced = %d, color = %x", screenOffX, screenOffY, this->getFocus(), this->getFocus() ? selColor : regColor);

  this->corrRect(lcd, screenOffX, screenOffY, windowWidth, windowHeight, bgColor);
  auto th = widgetFont->height();
  auto cy = screenOffY + windowHeight/2;

  // Left arrow
  lcd.fillTriangle(screenOffX, cy,
                   screenOffX + arrowWidth, cy - th/2,
                   screenOffX + arrowWidth, cy + th/2 + 1,
                   this->getFocus() ? selColor : regColor);

  // Right arrow
  lcd.fillTriangle(screenOffX + windowWidth - 1, cy,
                   screenOffX + windowWidth - 1 - arrowWidth, cy - th/2,
                   screenOffX + windowWidth - 1 - arrowWidth, cy + th/2 + 1,
                   this->getFocus() ? selColor : regColor);

  // Draw text for the current choice
  lcd.setTextDatum(MC_DATUM);
  lcd.setTextFont(this->widgetFont);
  lcd.setTextColor(this->textColor, this->bgColor);
  lcd.drawFitString(this->choices[this->curChoice], windowWidth - 2*arrowWidth - 2, screenOffX + windowWidth/2, cy);

}

YesNoWidget::YesNoWidget(uint16_t posX, uint16_t posY, uint16_t width, uint16_t height, SmoothFont* font,
                         colorType textColor, colorType bgColor, colorType regularColor, colorType selectedColor)
  : ChoiceWidget(posX, posY, width, height, font, textColor, bgColor, regularColor, selectedColor) {
  this->addChoice("No");
  this->addChoice("Yes");
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  TextInputAbstract  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

TextInputAbstract::TextInputAbstract(uint16_t xPos, uint16_t yPos, uint16_t width, uint16_t height,
                                     ControlState& state,
                                     SmoothFont* font, uint32_t maxInputSize, InputType typ)
  : FocusableWidget(xPos, yPos, width, height), maxInputSize(maxInputSize), controlState(state), inputType(typ) {
  widgetFont = font ? font : fonts[OPENSANS_COND_BOLD_20];
}

void TextInputAbstract::setFocus(bool focus) {
  controlState.setInputState(inputType);
  /* Delegate the focus bookkeeping instead of repeating it.
   *
   * This used to assign this->focused and this->updated directly, which is byte-for-byte
   * what the base does — and that duplication silently defeated the first attempt at
   * suppressing triple-tap-to-sleep inside text fields: setFocus is pure-virtual in
   * AbstractWidget, so this override meant FocusableWidget::setFocus never ran for the one
   * class of widget that needed it, and backspacing a message still slept the phone. */
  FocusableWidget::setFocus(focus);
};

void TextInputAbstract::drawCursor(LCD &lcd, uint16_t posX, uint16_t posY, uint16_t charHeight, uint16_t color) {
  lcd.drawLine(posX, posY, posX, posY+charHeight-1, color);
}

void TextInputAbstract::setColors(colorType fg, colorType bg) {
  fgColor = fg;
  bgColor = bg;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  TextInputBase - - - - - - - - - - - - - - - - - - - - - - - - - - - -

TextInputBase::TextInputBase(uint16_t xPos, uint16_t yPos, uint16_t width, uint16_t height,
                             ControlState& state,
                             SmoothFont* font, uint32_t maxInputSize, InputType typ)
  : TextInputAbstract(xPos, yPos, width, height, state, font, maxInputSize, typ) {
  inputStringDyn = NULL;
  inputStringSize = 0;      // allocated size (bytes)
  textOffset = 0;
  cursorOffset = 0;
}

// Return: true if space was allocated in FULL extent
bool TextInputBase::allocateMore(uint32_t minSize) {    // bytes
  log_v("allocMore TextInputBase: %d", minSize);
  uint32_t sz = 2*inputStringSize;        // double the space by default
  if (!sz || minSize>sz) {
    sz = minSize;  // increase if still not enough
  }
  if (!sz) {
    sz = 8;  // minimum number of bytes to allocate for any TextInput
  }
  if (sz > maxInputSize) {
    sz = maxInputSize;  // do no go over the limit
  }
  if (sz > inputStringSize) {
    //log_d("realloc %d", sz);
    /* extRealloc: this is the buffer holding whatever the user is typing, it grows by
     * DOUBLING, and its ceiling is maxInputSize - 64,000 bytes for some callers. That is not
     * a thing to grow on the ~20 KB internal heap. MultilineTextWidget::allocateMore already
     * uses extRealloc for the same reason; this one was missed. Falls back to internal by
     * itself, and free() is region-agnostic.
     * PasswordInputWidget::allocateMore is deliberately NOT changed: those buffers are tiny
     * (a WiFi key is 63 characters) so they cost the heap nothing, and a secret is better off
     * in internal RAM than on an external PSRAM die. */
    char *p = (char*) extRealloc(inputStringDyn, sz);
    if (p!=NULL) {
      //log_d("realloc done %d", sz);
      inputStringDyn = p;
      inputStringSize = sz;
      if (inputStringSize>=minSize) {
        return true;  // enough space allocated to cover the request (and at least one byte more)
      }
    }
  }
  return false;
}

void TextInputBase::setText(const char* str) {
  log_v("setText TextInputBase");
  if (str != NULL) {
    uint32_t len = strlen(str);
    if (len+1<=inputStringSize || allocateMore(len+1)) {
      // string can fit into inputStringDyn
      memcpy(inputStringDyn, str, len+1);
      cursorOffset = len;
      this->updated = true;
    } else if (inputStringSize>0) {
      // string is too big for inputStringDyn
      memcpy(inputStringDyn, str, inputStringSize-1);
      inputStringDyn[inputStringSize-1] = '\0';
      cursorOffset = inputStringSize-1;
      this->updated = true;
    }
  }
}

TextInputBase::~TextInputBase() {
  freeNull((void **) &inputStringDyn);
}

bool TextInputBase::getInt(int32_t &i) {
  const char* text = this->getText();
  char *endptr = NULL;
  errno = 0;
  i = strtol(text, &endptr, 10);
  if (!*text || !endptr || *endptr || errno) {
    return false;
  }
  return true;
}

void TextInputBase::setInt(int32_t i) {
  char s[12];
  sprintf(s, "%d", i);
  this->setText(s);
}
// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Multiline text input widget  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

MultilineTextWidget::MultilineTextWidget(uint16_t xPos, uint16_t yPos, uint16_t width, uint16_t height,
    const char* emptyText, ControlState& state,
    uint32_t maxInputSize, SmoothFont* font, InputType typ, uint16_t xPadding, uint16_t yPadding)
  : TextInputAbstract(xPos, yPos, width, height, state, font, maxInputSize, typ),
    xPadding(xPadding), yPadding(yPadding) {
  rowsDyn = NULL;
  retTextDyn = NULL;
  emptyTextDyn = emptyText ? extStrdup(emptyText) : NULL;

  firstVisibleRow = 0;
  visibleRows = (height-(yPadding*2)) / font->height();

  cursRow = 0;
  maxRows = 0;
  cursOffset = 0;

  allocateMore(10);
}

MultilineTextWidget::~MultilineTextWidget() {
  freeNull((void **) &rowsDyn);
  freeNull((void **) &retTextDyn);
  freeNull((void **) &emptyTextDyn);
}

// Return: true if space was allocated in *full* extent
bool MultilineTextWidget::allocateMore(int minSize) {    // items
  log_i("allocMore MultilineTextWidget: cur rows=%d, min rows=%d", maxRows, minSize);

  // Determine the size to be allocated
  uint32_t rows = 2*maxRows;                      // double the current space by default
  if (!rows || minSize>rows) {
    rows = minSize;  // increase if still not enough
  }
  if (!rows) {
    rows = 1;  // minimum number of rows is 1
  }
  if (rows > maxInputSize) {
    rows = maxInputSize;  // no need for more rows than maximum amount of characters
  }

  // Reallocate (if still makes sense)
  if (rows > maxRows) {
    log_v("realloc, rows=%d", rows);
    // extRealloc: the row-pointer block belongs in PSRAM with the rows themselves — it is
    // only ever walked by the CPU, and every internal-heap byte here is one the SIP stack
    // and the emulator cannot have.
    char **p = (char**) extRealloc(rowsDyn, rows*sizeof(char*));
    if (p!=NULL) {
      memset(p + maxRows, 0, sizeof(char*)*(rows - maxRows));
      log_v("inited, new rows=%d", rows - maxRows);
      rowsDyn = p;
      maxRows = rows;
      if (maxRows>=minSize) {
        return true;  // enough space allocated to cover the request (and at least one byte more)
      }
    }
  } else if (rows == maxInputSize) {
    log_i("failed to allocate: max size reached");
  }
  return false;
}

void MultilineTextWidget::appendText(const char* str) {
  const char* t = getText();
  int ntextLen = strlen(str);
  int otextLen = strlen(t);
  int bufferLen = ntextLen + otextLen + 1;

  char* buffer = (char*)extMalloc(bufferLen);
  if (!buffer) {
    log_e("appendText: no memory for %d bytes, append dropped", bufferLen);
    return;                              // the old text is still intact; losing the append
                                         // beats memset(NULL) taking the phone down
  }
  memset(buffer, 0x00, bufferLen);

  strlcpy(buffer, t, bufferLen);
  strlcat(buffer, str, bufferLen);
  setText(buffer);
  free(buffer);
}

void MultilineTextWidget::setText(const char* str) {
  log_i("MultilineTextWidget::setText");
  log_v("Text: \"%s\"", str);
  const char* p = str;
  const char* e = str + strlen(str);

  // Reset memory of all the rows
  for (int i=0; i<maxRows; i++) {
    freeNull((void **) &rowsDyn[i]);
  }

  // Fitting text
  cursRow = -1;
  const uint16_t horizontalSpace = widgetWidth - 2*xPadding;
  uint16_t fit = 0;
  while (p < e) {
    cursRow++;
    if (cursRow>=maxRows) {
      /* ⚠ CHECKED. allocateMore() can fail (realloc NULL, or maxInputSize reached), and
       * this loop used to write rowsDyn[cursRow] regardless — with cursRow == maxRows that
       * store lands 4 bytes past the end of the pointer block, so an out-of-memory moment
       * became silent heap corruption and a reboot with no decodable backtrace. Truncating
       * the text is the honest degradation: everything fitted so far still draws. */
      if (!allocateMore()) {
        log_e("setText: no room for row %d, text truncated", cursRow);
        cursRow--;
        fit = 0;      // skip the trailing-newline special case: it indexes by `fit`, which
                      // belongs to the row that never got allocated
        break;
      }
    }
    fit = widgetFont->fitWordsLength(p, horizontalSpace);
    if (fit) {
      /* extStrndup, not strndup: a thread view holds ~90-115 of these rows at once, and
       * they were all coming out of the ~20 KB contiguous internal heap the SIP stack and
       * the emulator fight over. PSRAM has 3.6 MB spare; the rows are only read by
       * drawString. free() is region-agnostic, so the existing freeNull teardown and the
       * typing paths' plain-malloc rows coexist with these safely. */
      rowsDyn[cursRow] = extStrndup(p, fit);
      if (!rowsDyn[cursRow]) {
        log_e("setText: row alloc failed, text truncated");
        cursRow--;
        fit = 0;      // same reason as the allocateMore failure above
        break;
      }
      p += fit;
    } else {
      log_e("could not fit text");      // TODO: break a word into head and tail
      break;
    }
  }
  // Was the last character a new line? (special case)
  if (cursRow>=0 && fit>0 && rowsDyn[cursRow][fit-1]=='\n') {
    // Set cursor to beginning of the next row
    cursRow++;
    cursOffset = 0;
  } else if (cursRow>=0) {
    // Set cursor to end of the string
    cursOffset = strlen(rowsDyn[cursRow]);
  } else if (cursRow<0) {
    // Special case for no text
    cursRow = 0;
    if ((cursRow < maxRows || allocateMore()) && (rowsDyn[cursRow] = extStrdup("")) == NULL) {
      log_e("setText: empty-row alloc failed");
    }
  }
  revealCursor();
  this->updated = true;
}

void MultilineTextWidget::cursorToStart() {
  cursRow = 0;
  cursOffset = 0;
  revealCursor();
}

void MultilineTextWidget::cursorToEnd() {
  cursRow = 0;
  for (int i = 0; i < maxRows; i++) {
    if (rowsDyn[i] && rowsDyn[i][0]) {
      cursRow = i;
    }
  }
  cursOffset = rowLen(cursRow);
  revealCursor();
}

void MultilineTextWidget::revealCursor() {
  if (cursRow >=0 && cursRow < firstVisibleRow) {
    firstVisibleRow = cursRow;
  } else if (cursRow >= firstVisibleRow + visibleRows) {
    firstVisibleRow = cursRow - visibleRows + 1;
  }
}

const char* MultilineTextWidget::getText() {
  // Merge all lines into a single dynamic variable
  // - calculate final length
  int len = 0, i;
  for (i=0; i<maxRows; i++) {
    if (rowsDyn[i] && rowsDyn[i][0]) {
      len += strlen(rowsDyn[i]);
    }
  }
  // - reallocate memory
  freeNull((void **) &retTextDyn);
  retTextDyn = (char*) malloc(len+1);
  if (retTextDyn) {
    // - merge the contents
    int l = 0;
    if (len) {
      for (i=0; i<maxRows; i++) {
        if (rowsDyn[i] && rowsDyn[i][0]) {
          uint16_t lr = strlen(rowsDyn[i]);
          memcpy(retTextDyn + l, rowsDyn[i], lr);
          l += lr;
        }
      }
    }
    retTextDyn[l] = '\0';
    return retTextDyn;
  } else {
    // - memory allocation failed
    return NULL;
  }
}

bool MultilineTextWidget::processEvent(EventType event) {
  // TODO: add this->updated = true; where relevant (for efficient redrawing)
  log_i("processEvent MultilineTextWidget");

  if (event>=32 && event<=126) {      // printable ASCII character (TODO: allow newline and maybe tab)

    // Check maximum length
    int len = 0;
    for (int i=0; i<this->maxRows; i++) {
      if (this->rowsDyn[i] && this->rowsDyn[i][0]) {
        len += strlen(rowsDyn[i]);
      }
    }
    if (len>=maxInputSize) {
      log_d("character limit reached");
      return false;
    }

    // Insert character
    log_d("insert character: %d, row: %d", event, cursRow);

    // Insert character in current row
    len = rowsDyn[cursRow] && rowsDyn[cursRow][0] ? strlen(rowsDyn[cursRow]) : 0;
    /* extRealloc: this grows a rowsDyn[] entry, and the rest of this widget already
   * allocates those from PSRAM (extRealloc/extStrndup/extStrdup above). A plain realloc
   * here quietly pulled edited rows back onto the internal heap. */
  char* p = (char*) extRealloc(rowsDyn[cursRow], len+2);
    if (p) {
      rowsDyn[cursRow] = p;
      for (int i=len; i>cursOffset; i--) {
        p[i] = p[i-1];
      }
      p[len+1] = '\0';
      p[cursOffset] = event;     // new character
      cursOffset++;
      len++;
    }

    // Split if needed
    int row = cursRow;
    uint16_t horizontalSpace = widgetWidth - 2*xPadding;
    do {
      uint16_t fit = widgetFont->fitWordsLength(rowsDyn[row], horizontalSpace);
      if (fit<len) {

        // Next row size
        uint16_t len2 = (row+1 < maxRows && rowsDyn[row+1] && rowsDyn[row+1][0]) ? strlen(rowsDyn[row+1]) : 0;

        log_d("added: %s", rowsDyn[row]);
        if (len2) {
          log_d("next:  %s", rowsDyn[row+1]);
        }

        // Allocate memory for new row if needed
        if (!len2 && row+1 >= maxRows)
          if (!allocateMore())
            if (!allocateMore(maxRows+1)) {
              break;  // could not allocate more memory for one more row
            }

        // Allocate memory for new strings
        // Both become rowsDyn[] entries below — PSRAM, like every other row.
        char* s1 = (char*) extMalloc(fit + 1);
        char* s2 = (char*) extMalloc(len-fit + len2 + 1);
        if (s1 && s2) {
          // String with removed characters from the end
          memcpy(s1, rowsDyn[row], fit);
          s1[fit] = '\0';
          log_d("s1: %s (%d)", s1, fit);

          // Added characters to beginning
          memcpy(s2, rowsDyn[row]+fit, len-fit);
          if (len2) {
            memcpy(s2+len-fit, rowsDyn[row+1], len2);
          }
          s2[len-fit+len2] = '\0';
          log_d("s2: %s (%d)", s2, len-fit+len2);
          log_d("offset = %d", cursOffset);

          // Forget old strings
          free(rowsDyn[row]);
          freeNull((void **) &rowsDyn[row+1]);

          // New strings
          rowsDyn[row] = s1;
          rowsDyn[row+1] = s2;

          // Fix cursor position
          if (row==cursRow && cursOffset>fit) {
            cursRow++;
            cursOffset -= fit;
          }

          // Proceed to next row
          row++;
          len = len-fit+len2;

        } else {
          // Error: could not allocate memory for string manipulation
          freeNull((void **) &s1);
          freeNull((void **) &s2);
          break;
        }
      } else {
        break;  // all characters fit
      }
    } while (1);

    revealCursor();
    return true;

  } else if (event == WIPHONE_KEY_BACK) {

    // Backspace ("Clear")

    // Remove one character
    if (cursOffset>0) {
      // Remove character from this row
      if (rowsDyn[cursRow]) {
        int len = strlen(rowsDyn[cursRow]);
        for (int i=cursOffset; i<=len; i++) {
          rowsDyn[cursRow][i-1] = rowsDyn[cursRow][i];
        }
      }
      cursOffset--;
    } else if (cursRow>0) {
      // Remove character from previous row
      uint16_t l = strlen(rowsDyn[cursRow-1]);
      if (l) {
        rowsDyn[cursRow-1][l-1] = '\0';
      }
      log_d("cursor = %d, %d, len = %d, string = \"%s\"", cursRow, cursOffset, l, rowsDyn[cursRow-1]);
      cursRow -= 1;
      cursOffset = l-1;
      log_d("cursor = %d, %d", cursRow, cursOffset);
    }

    // Realign: try to fit more characters into current row
    int row = cursRow;
    uint16_t horizontalSpace = widgetWidth - 2*xPadding;
    while (row+1 < maxRows && rowsDyn[row+1] && rowsDyn[row+1][0]) {
      // Merge current row with next one, then split it in two until no changes needed

      // - merge two rows into one
      uint16_t l1 = strlen(rowsDyn[row]);
      uint16_t l2 = strlen(rowsDyn[row+1]);
      char* dyn = (char*) malloc(l1 + l2 + 1);
      if (dyn) {
        memcpy(dyn, rowsDyn[row], l1);
        memcpy(dyn+l1, rowsDyn[row+1], l2+1);
      } else {
        break;      // merging failed: quit silently
      }
      log_d("merged string: %s", dyn);

      // - break the merged string in two
      bool quit = false;
      uint16_t fit = widgetFont->fitWordsLength(dyn, horizontalSpace);
      if (fit != l1) {
        // Same again on the merge path: these replace rowsDyn[row] and rowsDyn[row+1].
        char* s2 = extStrdup(dyn+fit);
        dyn[fit] = '\0';
        char* s1 = extStrdup(dyn);
        if (s1 && s2) {
          // - replace original two rows with new ones
          free(rowsDyn[row]);
          free(rowsDyn[row+1]);
          rowsDyn[row] = s1;
          rowsDyn[row+1] = s2;
          log_d("new break up: %s / %s", s1, s2);
        }
        // - fix offset
        if (row==cursRow && fit < cursOffset) {
          log_d("cursor = %d, %d", cursRow, cursOffset);
          cursRow++;
          cursOffset -= fit;
          log_d("cursor = %d, %d", cursRow, cursOffset);
        }
        // - proceed realignment to next row
        row++;
      } else {
        quit = true;
      }

      free(dyn);
      if (quit) {
        break;
      }
    }
    revealCursor();
    return true;

  } else if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_LEFT || event == WIPHONE_KEY_RIGHT) {

    // Cursor movements

    if (event == WIPHONE_KEY_UP) {
      if (cursRow>0) {
        cursRow--;
        if (cursOffset>rowLen(cursRow)) {
          cursOffset = rowLen(cursRow);  // TODO: remember from what column the cursor started moving
        }
        revealCursor();
      }
    } else if (event == WIPHONE_KEY_DOWN) {
      if (cursRow < maxRows-1 && (notEmptyRow(cursRow+1) || newLineRow(cursRow))) {
        cursRow++;
        if (emptyRow(cursRow)) {
          cursOffset = 0;
        } else if (cursOffset>rowLen(cursRow)) {
          cursOffset = rowLen(cursRow);
        }
        revealCursor();
      }
    } else if (event == WIPHONE_KEY_LEFT) {
      // TODO: cycle to the end
      if (cursOffset>0) {
        cursOffset--;
      } else if (cursRow>0) {
        cursRow--;
        cursOffset = rowLen(cursRow);
        revealCursor();
      }
    } else if (event == WIPHONE_KEY_RIGHT) {
      // TODO: cycle to the beginning
      log_d("cursor right");
      if (notEmptyRow(cursRow)) {
        // TODO: this can be optimized
        if (cursOffset<rowLen(cursRow) + (newLineRow(cursRow) ? -1 : 0)) {
          log_d("next character");
          // Next character in current row
          cursOffset++;
        } else if (notEmptyRow(cursRow+1) || newLineRow(cursRow)) {
          log_d("next line");
          // Proceed to next row
          cursRow++;
          cursOffset = 0;
          revealCursor();
        }
      }
    }

    // Always set cursor before the newline character at the end of row
    if (newLineRow(cursRow) && cursOffset==strlen(rowsDyn[cursRow])) {
      cursOffset--;
    }

    return true;
  }
  return false;
}

void MultilineTextWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  log_i("redraw MultilineTextWidget");

  if (windowWidth != widgetWidth || windowHeight != widgetHeight) {
    return;  // this widget can only be rendered in full
  }
  lcd.fillRect(screenOffX, screenOffY, windowWidth, windowHeight, bgColor);

  uint16_t yOff = yPadding;
  lcd.setTextFont(widgetFont);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(fgColor, bgColor);

  // Vertical centering
  if (centering) {
    // Count actual rows present
    int actualRows = 0;
    for (int i=firstVisibleRow; i<firstVisibleRow + visibleRows; i++) {
      if (!rowsDyn[i]) {
        break;
      }
      actualRows++;
    }
    // Add offset
    uint16_t freeSpace = (windowHeight - (yPadding*2)) - widgetFont->height()*actualRows;
    yOff += (freeSpace/2);
  }

  // Draw actual lines
  bool anyText = false;
  for (int i=firstVisibleRow; i<firstVisibleRow + visibleRows; i++) {
    // Draw text of each line
    if (i<maxRows) {
      if (rowsDyn[i] && rowsDyn[i][0]) {
        anyText = true;

        // Draw string without trailing newline
        int len = strlen(rowsDyn[i]);
        if (len && rowsDyn[i][len-1] != '\n') {
          lcd.drawString(rowsDyn[i], screenOffX + xPadding, screenOffY + yOff);
        } else {
          /* In-place trim, NOT strdup. This ran up to 13 times per redraw — on every
           * scroll keypress in a thread view — and the strdup was both an internal-heap
           * churn at the worst possible moment and an UNCHECKED one: dup[len-1] on a NULL
           * return is a crash whose trigger is literally "while scrolling". The row is our
           * own mutable buffer, so blank the newline, draw, put it back. Zero allocations,
           * no failure path at all. */
          rowsDyn[i][len-1] = '\0';
          lcd.drawString(rowsDyn[i], screenOffX + xPadding, screenOffY + yOff);
          rowsDyn[i][len-1] = '\n';
        }
      }
    }

    // Draw cursor
    if (this->focused && cursRow == i) {
      uint16_t  curPosX = 0;
      if (i < maxRows && rowsDyn[i] && rowsDyn[i][0]) {
        // If there is text in this row -> calculate cursor offset
        if (cursOffset < strlen(rowsDyn[i])) {
          /* Same in-place trick as the newline trim above: a prefix width needs the string
           * terminated at the cursor, not a heap copy of it (the old strdup here was also
           * dereferenced unchecked). */
          const char saved = rowsDyn[i][cursOffset];
          rowsDyn[i][cursOffset] = '\0';
          curPosX = lcd.textWidth(rowsDyn[i]);
          rowsDyn[i][cursOffset] = saved;
        } else {
          curPosX = lcd.textWidth(rowsDyn[i]);
        }
      }
      /* ⚠ fgColor, NOT a hardcoded WP_COLOR_0. The cursor used to be drawn black whatever the
       * widget's theme was, so on every screen that sets a DARK background — mesh compose, the
       * name editors, Books — it was black on black and simply could not be seen. Moving it
       * with left/right worked all along; there was no way to tell. Drawing it in the text
       * colour is right by construction: the text is legible against this background, so the
       * cursor is too. On light screens fgColor IS WP_COLOR_0, so nothing there changes. */
      drawCursor(lcd, screenOffX + xPadding + curPosX, screenOffY + yOff, widgetFont->height(), fgColor);
    }

    yOff += widgetFont->height();
  }

  // Empty?
  if (!anyText && emptyTextDyn != NULL) {
    yOff = yPadding;
    for (int i=0; i<maxRows; i++) {
      if (rowsDyn[i] && rowsDyn[i][0]) {
        anyText = true;
        break;
      }
    }
    if (!anyText) {
      lcd.setTextColor(GRAY_50, bgColor);
      lcd.drawString(emptyTextDyn, screenOffX + xPadding + 1, screenOffY + yOff);
    }
  }

}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  TextInputWidget  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

TextInputWidget::TextInputWidget(uint16_t xPos, uint16_t yPos, uint16_t width, uint16_t height,
                                 ControlState& state,
                                 uint32_t maxInputSize, SmoothFont* font, InputType typ, uint16_t sidePadding)
  : TextInputBase(xPos, yPos, width, height, state, font,  maxInputSize, typ), xPad(sidePadding) {
  //log_i("create TextInputWidget");
  focused = false;
}

void TextInputWidget::shiftCursor(int16_t shift) {
  log_d("cursor offset: %d", cursorOffset);
  if (shift < 0) {
    if (cursorOffset >= -shift) {
      cursorOffset += shift;
      this->updated = true;
    }
    //else
    //  cursorOffset = (inputStringDyn!=NULL) ? strlen(inputStringDyn) : 0;     // cycle to the end
  } else if (shift > 0) {
    uint16_t len = (inputStringDyn!=NULL) ? strlen(inputStringDyn) : 0;
    if (cursorOffset+shift<=len) {
      cursorOffset += shift;
      this->updated = true;
    }
  }
}

bool TextInputWidget::insertCharacter(char c) {
  log_d("insertCharacter TextInputWidget");
  if (inputStringDyn==NULL) {
    /* 🛑 allocateMore() CAN FAIL AND LEAVE THIS NULL. It returns bool and nothing read it,
     * so the next line wrote to address 0 — a StoreProhibited panic on the typing path, one
     * failed realloc away, on a phone whose internal heap has been measured at 2,276 bytes
     * free at its worst. Refusing the character is the correct outcome: the caller already
     * treats false as "not inserted". */
    if (!allocateMore() || inputStringDyn==NULL) {
      return false;
    }
    inputStringDyn[0] = '\0';
  }
  uint32_t len = strlen(inputStringDyn);
  if (len+1>=inputStringSize) {
    allocateMore();
  }
  if (len+1<inputStringSize) {
    // Free space
    for (uint32_t i=len+1; i>0 && i>cursorOffset; i--) {
      inputStringDyn[i] = inputStringDyn[i-1];
    }

    // Insert character
    inputStringDyn[cursorOffset] = c;

    this->updated = true;
    return true;
  }
  return false;
}

bool TextInputWidget::processEvent(EventType event) {
  log_d("processEvent TextInputWidget");
  if (event>=32 && event<=126) {
    if (this->insertCharacter(event)) {
      cursorOffset++;
    }
    return true;
  } else if (event==WIPHONE_KEY_LEFT) {
    this->shiftCursor(-1);
    return true;
  } else if (event==WIPHONE_KEY_RIGHT) {
    this->shiftCursor(1);
    return true;
  } else if (event==WIPHONE_KEY_BACK) {
    // Backspace
    if (cursorOffset>0) {
      uint32_t len = strlen(inputStringDyn);
      for(int i=cursorOffset; i<=len; i++) {
        inputStringDyn[i-1] = inputStringDyn[i];
      }
      cursorOffset--;
      this->updated = true;
    }
    return true;
  }
  return false;
}

void TextInputWidget::revealCursor() {
  log_d("revealCursor TextInputWidget");
  uint16_t len = inputStringDyn != NULL ? strlen(inputStringDyn) : 0;
  if (len>0) {
    uint16_t padding = (xPad*2);

    // Ensure the text offset is not past the last character
    if (textOffset>=len) {
      textOffset = len>0 ? len-widgetFont->fitTextLength(inputStringDyn, widgetWidth-padding, -1) : 0;
    }

    if (cursorOffset<textOffset) {
      // Cursor is to the left from the visible text
      uint16_t shift = textOffset - cursorOffset;
      textOffset -= shift;
    } else {
      // Reveal cursor if it is to the right from the visible area
      if (cursorOffset > len) {
        cursorOffset = len;  // Sanity check
      }
      int visibleChars = widgetFont->fitTextLength(inputStringDyn + textOffset, widgetWidth-padding);
      if (cursorOffset > textOffset + visibleChars) {
        log_d("revealing cursor on the right");
        char* dup = strdup(inputStringDyn + textOffset);
        if (dup) {
          log_d("dup: %s", dup);
          dup[cursorOffset-textOffset] = '\0';
          log_d("dup: %s", dup);
          int fit = widgetFont->fitTextLength(dup, widgetWidth-padding, -1);
          textOffset += cursorOffset-textOffset-fit;
          free(dup);
        }
      }
    }
  } else {
    textOffset = cursorOffset = 0;
  }
}

void TextInputWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  log_d("redraw TextInputWidget ", (uint8_t) focused);
  if (windowWidth != widgetWidth || windowHeight != widgetHeight) {
    return;  // this widget can only be rendered in full
  }

  // Draw background
  this->corrRect(lcd, screenOffX, screenOffY, windowWidth, windowHeight, bgColor);      // TODO: enabled color?

  // Draw text
  revealCursor();
  int len=(inputStringDyn!=NULL) ? strlen(inputStringDyn) : 0;
  if (len>0) {
    lcd.setTextFont(widgetFont);
    lcd.setTextColor(fgColor, bgColor);
    lcd.setTextDatum(ML_DATUM);
    int fit = lcd.drawFitString(inputStringDyn + textOffset, windowWidth-(xPad*2), screenOffX + xPad, screenOffY + windowHeight/2);

    // Draw cursor
    if (this->focused) {
      uint16_t  curPosX = 0;
      char* dup = strdup(inputStringDyn + textOffset);
      if (dup && fit<=strlen(dup)) {   // sanity check (and strdup can return NULL: strlen(NULL) crashes)
        dup[fit] = '\0';
        if (cursorOffset - textOffset <=  fit) {
          dup[cursorOffset - textOffset] = '\0';
          curPosX = lcd.textWidth(dup);
          drawCursor(lcd, screenOffX + curPosX + xPad, screenOffY + (windowHeight-widgetFont->height())/2, widgetFont->height(), fgColor);
        }
      }
      free(dup);
    }
  } else {
    if (focused) {
      drawCursor(lcd, screenOffX+xPad+1, screenOffY+(windowHeight-widgetFont->height())/2, widgetFont->height(), fgColor);
    }
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  PasswordInputWidget  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

PasswordInputWidget::PasswordInputWidget(uint16_t xPos, uint16_t yPos, uint16_t width, uint16_t height,
    ControlState& state,
    uint32_t maxInputSize, SmoothFont* font, InputType typ, uint16_t sidePadding)
  : TextInputBase(xPos, yPos, width, height, state, font,  maxInputSize, typ), xPad(sidePadding) {
  outputStringDyn = nullptr;
  focused = false;
}

PasswordInputWidget::~PasswordInputWidget() {
  freeNull((void **) &outputStringDyn);
}

// Return: true if space was allocated in FULL extent
bool PasswordInputWidget::allocateMore(uint32_t minSize) {    // bytes
  log_d("allocMore PasswordInputWidget: %d", minSize);
  uint32_t sz = 2*inputStringSize;        // double the space by default
  if (!sz || minSize>sz) {
    sz = minSize;  // increase if still not enough
  }
  if (!sz) {
    sz = 8;  // minimum number of bytes to allocate for any TextInput
  }
  if (sz > maxInputSize) {
    sz = maxInputSize;  // do no go over the limit
  }
  if (sz > inputStringSize) {
    //log_d("realloc %d", sz);
    char *p = (char*) realloc(inputStringDyn, sz);
    char *p2 = (char*) realloc(outputStringDyn, sz);
    if (p!=NULL && p2!=NULL) {
      //log_d("realloc done %d", sz);
      inputStringDyn = p;
      outputStringDyn = p2;
      inputStringSize = sz;
      if (inputStringSize>=minSize) {
        return true;  // enough space allocated to cover the request (and at least one byte more)
      }
    }
  }
  return false;
}

void PasswordInputWidget::setText(const char* str) {
  log_d("setText PasswordInputWidget");
  if (str != NULL) {
    uint32_t len = strlen(str);
    if (len+1<=inputStringSize || allocateMore(len+1)) {
      // string can fit into inputStringDyn
      memcpy(inputStringDyn, str, len+1);
      memset(outputStringDyn, '*', len);
      outputStringDyn[len] = '\0';
      cursorOffset = len;
      this->updated = true;
    } else if (inputStringSize>0) {
      // string is too big for inputStringDyn
      memcpy(inputStringDyn, str, inputStringSize-1);
      memset(outputStringDyn, '*', inputStringSize-1);
      inputStringDyn[inputStringSize-1] = '\0';
      outputStringDyn[inputStringSize-1] = '\0';
      cursorOffset = inputStringSize-1;
      this->updated = true;
    }
  }
}

void PasswordInputWidget::shiftCursor(int16_t shift) {
  if (shift < 0) {
    if (cursorOffset >= -shift) {
      cursorOffset += shift;
      this->updated = true;
    }
  } else if (shift > 0) {
    uint16_t len = (inputStringDyn!=NULL) ? strlen(inputStringDyn) : 0;
    if (cursorOffset+shift<=len) {
      cursorOffset += shift;
      this->updated = true;
    }
  }
}

bool PasswordInputWidget::insertCharacter(char c) {
  if (inputStringDyn==NULL) {
    // Same NULL-after-failed-allocateMore panic as TextInputWidget above.
    if (!allocateMore() || inputStringDyn==NULL || outputStringDyn==NULL) {
      return false;
    }
    inputStringDyn[0] = '\0';
  }
  uint32_t len = strlen(inputStringDyn);
  if (len+1>=inputStringSize) {
    allocateMore();
  }
  if (len+1<inputStringSize) {
    // Shift the characters to the right freeing space for one more character
    for (uint32_t i=len+1; i>0 && i>cursorOffset; i--) {
      inputStringDyn[i] = inputStringDyn[i-1];
    }

    // Insert character
    inputStringDyn[cursorOffset] = c;

    // Just add one more asterisk
    outputStringDyn[len] = '*';
    outputStringDyn[len+1] = '\0';

    this->updated = true;
    return true;
  }
  return false;
}

bool PasswordInputWidget::processEvent(EventType event) {
  if (event>=32 && event<=126) {
    if (insertCharacter(event)) {
      cursorOffset++;
    }
    return true;
  } else if (event==WIPHONE_KEY_LEFT) {
    shiftCursor(-1);
    return true;
  } else if (event==WIPHONE_KEY_RIGHT) {
    shiftCursor(1);
    return true;
  } else if (event==WIPHONE_KEY_BACK) {
    // Backspace
    if (cursorOffset>0) {
      // Remove one character, shifting the following ones to the left
      uint32_t len = strlen(inputStringDyn);
      for(int i=cursorOffset; i<=len; i++) {
        inputStringDyn[i-1] = inputStringDyn[i];
      }
      cursorOffset--;
      // Remove one asterisk
      if (len>0) {
        outputStringDyn[len-1] = '\0';
      }
      this->updated = true;
    }
    return true;
  }
  return false;
}

void PasswordInputWidget::revealCursor() {
  uint16_t len = outputStringDyn != NULL ? strlen(outputStringDyn) : 0;
  if (len>0) {
    uint16_t padding = (xPad*2);

    // Ensure the text offset is not past the last character
    if (textOffset>=len) {
      textOffset = len>0 ? len - widgetFont->fitTextLength(outputStringDyn, widgetWidth-padding, -1) : 0;
    }

    if (cursorOffset<textOffset) {
      // Cursor is to the left from the visible text
      uint16_t shift = textOffset - cursorOffset;
      textOffset -= shift;
    } else {
      // Reveal cursor if it is to the right from the visible area
      if (cursorOffset > len) {
        cursorOffset = len;  // Sanity check
      }
      int visibleChars = widgetFont->fitTextLength(outputStringDyn + textOffset, widgetWidth-padding);
      if (cursorOffset > textOffset + visibleChars) {
        log_d("revealing cursor on the right");
        char* dup = strdup(outputStringDyn + textOffset);
        if (dup) {
          log_d("dup: %s", dup);
          dup[cursorOffset-textOffset] = '\0';
          log_d("dup: %s", dup);
          int fit = widgetFont->fitTextLength(dup, widgetWidth-padding, -1);
          textOffset += cursorOffset-textOffset-fit;
          free(dup);
        }
      }
    }
  } else {
    textOffset = cursorOffset = 0;
  }
}

void PasswordInputWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  if (windowWidth != widgetWidth || windowHeight != widgetHeight) {
    return;  // this widget can only be rendered in full
  }

  // Draw background
  this->corrRect(lcd, screenOffX, screenOffY, windowWidth, windowHeight, bgColor);      // TODO: enabled color?

  // Draw text
  revealCursor();
  int len=(outputStringDyn!=NULL) ? strlen(outputStringDyn) : 0;
  if (len>0) {
    lcd.setTextFont(widgetFont);
    lcd.setTextColor(fgColor, bgColor);
    lcd.setTextDatum(ML_DATUM);
    int fit = lcd.drawFitString(outputStringDyn + textOffset, windowWidth-(xPad*2), screenOffX + xPad, screenOffY + windowHeight/2);

    // Draw cursor
    if (this->focused) {
      uint16_t  curPosX = 0;
      char* dup = strdup(outputStringDyn + textOffset);
      if (dup && fit<=strlen(dup)) {   // sanity check (and strdup can return NULL: strlen(NULL) crashes)
        dup[fit] = '\0';
        if (cursorOffset - textOffset <=  fit) {
          dup[cursorOffset - textOffset] = '\0';
          curPosX = lcd.textWidth(dup);
          drawCursor(lcd, screenOffX + curPosX + xPad, screenOffY+(windowHeight-widgetFont->height())/2, widgetFont->height(), fgColor);
        }
      }
      free(dup);
    }
  } else {
    // Empty input -> draw only cursor if necessary
    if (focused) {
      drawCursor(lcd, screenOffX+xPad+1, screenOffY+(windowHeight-widgetFont->height())/2, widgetFont->height(), fgColor);
    }
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Header & Footer widgets  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

uint16_t GUI::drawBatteryIcon(TFT_eSPI &lcd, ControlState &controlState, int16_t xLeft, int16_t xRight, uint16_t y) {
  if (controlState.battVoltage>0) {

    uint8_t SOC = controlState.battSoc;
    bool showBlink = controlState.battBlinkOn;
    //bool fullyCharged = SOC>=100 || controlState.battCharged;   // NOTE: controlState.battCharged is more strict: it can be FALSE if WiFi is working and SOC is 100,
    //       but it can also be true when SOC is not 100
    bool fullyCharged = SOC>=100;

    if (!GUI::batteryExtraLength) {

      // Draw battery as a single icon

      // Determine which icon to draw right now
      const unsigned char *icon = NULL;
      int iconSize = 0;
      if (controlState.usbConnected && fullyCharged) {
        icon = icon_batt_w_full;
        iconSize = sizeof(icon_batt_w_full);
      } else if (SOC < 10 && !showBlink) {
        icon = icon_batt_w_0;
        iconSize = sizeof(icon_batt_w_0);
      } else if (SOC < 10 || (SOC < 30 && !showBlink) ) {
        icon = icon_batt_w_1;
        iconSize = sizeof(icon_batt_w_1);
      } else if (SOC < 30 || (SOC < 50 && !showBlink) ) {
        icon = icon_batt_w_2;
        iconSize = sizeof(icon_batt_w_2);
      } else if (SOC < 50 || (SOC < 70 && !showBlink) ) {
        icon = icon_batt_w_3;
        iconSize = sizeof(icon_batt_w_3);
      } else if (SOC < 70 || (!showBlink && (controlState.usbConnected || SOC < 90)) ) {
        icon = icon_batt_w_4;
        iconSize = sizeof(icon_batt_w_4);
      } else {
        icon = icon_batt_w_5;
        iconSize = sizeof(icon_batt_w_5);
      }

      // Draw icon
      if (icon && iconSize) {
        IconRle3 *iconObj = new IconRle3(icon, iconSize);
        if (xLeft<0) {
          // Draw with right anchor
          lcd.drawImage(*iconObj, xRight-iconObj->width(), y);
        } else {
          // Draw with left anchor
          lcd.drawImage(*iconObj, xLeft, y);
        }
        uint16_t w = iconObj->width();
        delete iconObj;
        return w;
      }

    } else {

      // Draw battery icon out of components (can be any length)

      uint8_t SOC = controlState.battSoc;
      bool showBlink = controlState.battBlinkOn;
      bool fullyCharged = SOC>=100;

      // Determine how many sections would be drawn normally?
      int sections = 0;
      if (controlState.usbConnected && fullyCharged) {
        sections = 5;
      } else if (SOC < 10 && !showBlink) {
        sections = 0;
      } else if (SOC < 10 || (SOC < 30 && !showBlink) ) {
        sections = 1;
      } else if (SOC < 30 || (SOC < 50 && !showBlink) ) {
        sections = 2;
      } else if (SOC < 50 || (SOC < 70 && !showBlink) ) {
        sections = 3;
      } else if (SOC < 70 || (!showBlink && (controlState.usbConnected || SOC < 90))  ) {
        sections = 4;
      } else {
        sections = 5;
      }

      IconRle3 *sectionIcon = new IconRle3(icon_batt_s, sizeof(icon_batt_s));
      sections += (GUI::batteryExtraLength+1) / (sectionIcon->width()+1);

      // Draw icon
      uint16_t w = 0;
      if (xLeft<0) {
        // Draw with right anchor

        // - right part of the battery
        IconRle3 *icon = new IconRle3(icon_batt_r, sizeof(icon_batt_r));
        lcd.drawImage(*icon, xRight-icon->width(), y);
        w = icon->width();
        delete icon;

        // - draw sections
        for (int sec=0; sec<sections; sec++) {
          lcd.drawImage(*sectionIcon, xRight - (sectionIcon->width() + 1) * (sec + 1) - w + 1, y + 2);
        }

        // - draw two lines
        lcd.drawLine(xRight-20-w-GUI::batteryExtraLength, y, xRight-w, y, TFT_WHITE);
        lcd.drawLine(xRight-20-w-GUI::batteryExtraLength, y+12, xRight-w, y+12, TFT_WHITE);

        // - left part of the battery
        icon = new IconRle3(icon_batt_l, sizeof(icon_batt_l));
        lcd.drawImage(*icon, xRight - icon->width()-20-w-GUI::batteryExtraLength, y);
        w += icon->width() + 20 + GUI::batteryExtraLength;
        delete icon;


      } else {
        // Not implemented
      }

      delete sectionIcon;

      return w;

    }
  }
  return 0;
}

uint8_t GUI::wifiSignalStrength(int rssi) {
  if (rssi > -60) {
    return 3;  // ..-59
  }
  if (rssi > -70) {
    return 2;  // -60..-69
  }
  if (rssi > -80) {
    return 1;  // -70..-79
  }
  return 0;
}

uint16_t GUI::drawWifiIcon(TFT_eSPI &lcd, ControlState &controlState, uint16_t x, uint16_t y) {
  if (wifiState.isConnected() && WiFi.status() == WL_CONNECTED) {
    int iconSize = 0;
    const unsigned char *icon;
    int wifiLevel = GUI::wifiSignalStrength(controlState.wifiRssi);
    if (wifiLevel >= 3)        {
      icon = icon_wifi_w_3;
      iconSize = sizeof(icon_wifi_w_3);
    } else if (wifiLevel >= 2) {
      icon = icon_wifi_w_2;
      iconSize = sizeof(icon_wifi_w_2);
    } else if (wifiLevel >= 1) {
      icon = icon_wifi_w_1;
      iconSize = sizeof(icon_wifi_w_1);
    } else                     {
      icon = icon_wifi_w_0;
      iconSize = sizeof(icon_wifi_w_0);
    }
    lcd.drawImage(icon, iconSize, x, y);
    return 17;    // TODO: hardcoded width
  }
  return 0;
}

uint16_t GUI::drawSipIcon(TFT_eSPI &lcd, ControlState &controlState, uint16_t x, uint16_t y) {
  if (wifiState.isConnected() && WiFi.status() == WL_CONNECTED && controlState.sipEnabled) {
    // Draw SIP icon only if WiFi is connected
    if (controlState.sipRegistered) {
      lcd.drawImage(icon_phone_small_w, sizeof(icon_phone_small_w), x, y);
      return 11;      // TODO: hardcoded width
    } else {
      lcd.drawImage(icon_phone_small_w_crossed, sizeof(icon_phone_small_w_crossed), x, y);
      return 17;      // TODO: hardcoded width
    }
  }
  return 0;
}

/* Status bar unread indicator — one glyph per message TYPE.
 *
 *   WHITE  = unread SIP text        (controlState.unreadMessages)
 *   GREEN  = unread Meshtastic text (controlState.meshUnread)
 *   BOTH   = green with the white one overlapping it
 *
 * Because it reads straight off the two unread flags, checking one kind updates the icon by
 * itself: clear the mesh thread and the pair collapses to the plain white envelope, so the bar
 * says what is STILL waiting rather than merely that something is.
 *
 * Both glyphs are the same 18x17 RLE3 image; the green one is generated from the white one by
 * recolouring its palette (see icons.h). The return value is the width consumed, which the
 * header uses to place what it draws next — so the overlapped pair must report its real width. */
/* A small crossed speaker, 14 px wide, drawn only while the master mute is on — the one
 * place a silent phone says so. Primitives, not an image: two rectangles for the speaker's
 * body and cone, a diagonal through it. */
uint16_t GUI::drawMuteIcon(TFT_eSPI &lcd, ControlState &controlState, uint16_t x, uint16_t y) {
  if (!controlState.audioMuted) {
    return 0;
  }
  const uint16_t c = WP_ACCENT_1;
  lcd.fillRect(x, y + 5, 4, 6, c);                        // body
  lcd.fillTriangle(x + 4, y + 8, x + 9, y + 2, x + 9, y + 14, c);   // cone
  lcd.drawLine(x, y + 15, x + 13, y + 1, TFT_RED);        // the cross
  lcd.drawLine(x + 1, y + 15, x + 14, y + 1, TFT_RED);
  return 14;
}

uint16_t GUI::drawMessageIcon(TFT_eSPI &lcd, ControlState &controlState, uint16_t x, uint16_t y) {
  const bool sip  = controlState.unreadMessages;
  const bool mesh = controlState.meshUnread;
  if (!sip && !mesh) {
    return 0;
  }

  const uint16_t iconW   = 19;      // TODO: hardcoded width, as before
  const uint16_t overlap = 7;       // how far the white one sits over the green one

  if (sip && mesh) {
    // Green first, then white on top and offset right, so both stay readable.
    lcd.drawImage(icon_incoming_message_g, sizeof(icon_incoming_message_g), x, y);
    lcd.drawImage(icon_incoming_message_w, sizeof(icon_incoming_message_w), x + overlap, y);
    return iconW + overlap;
  }

  if (mesh) {
    lcd.drawImage(icon_incoming_message_g, sizeof(icon_incoming_message_g), x, y);
  } else {
    lcd.drawImage(icon_incoming_message_w, sizeof(icon_incoming_message_w), x, y);
  }
  return iconW;
}

// Brief overlay banner shown when a new Meshtastic message arrives (any screen).
// Drawn directly to the physical LCD, on top of the current app; the main loop
// repaints the screen after a short delay to erase it.
void GUI::showMeshPopup(const char* title, const char* body) {
  const int16_t x = 6;
  const int16_t w = lcd.width() - 12;
  const int16_t y = 26;
  const int16_t h = 48;

  lcd.fillRoundRect(x, y, w, h, 6, WP_ACCENT_1);
  lcd.drawRoundRect(x, y, w, h, 6, WHITE);
  lcd.setTextColor(WHITE, WP_ACCENT_1);
  lcd.setTextDatum(TL_DATUM);

  /* Both lines ellipsized to the INSIDE of the box. They were plain
   * drawStrings: a 23-char node name in the title, or any ordinary sentence in
   * the body, painted straight through the border and across the screen. The
   * body's 41-char buffer was never the real limit — ~26 chars is what fits. */
  const uint16_t inner = w - 16;
  lcd.setTextFont(fonts[AKROBAT_BOLD_18]);
  guiDrawEllipsized(lcd, title ? title : "New message", inner, x + 8, y + 6);

  /* The body is RAW text off the air, and a real message can contain newlines
   * or other control bytes. This is a one-line banner drawn with drawString,
   * which has no notion of them — they come out as stray glyphs. Flatten to
   * spaces so the preview reads as the one line it is. */
  char flat[64];
  strlcpy(flat, body ? body : "", sizeof(flat));
  for (char* p = flat; *p; p++) {
    if ((unsigned char)*p < 0x20) {
      *p = ' ';
    }
  }
  lcd.setTextFont(fonts[AKROBAT_BOLD_16]);
  guiDrawEllipsized(lcd, flat, inner, x + 8, y + 26);
}

void GUI::drawOtaUpdate() {
  lcd.fillRect(0, 0, lcd.width(), lcd.height(), THEME_BG);
  lcd.setTextColor(WP_ACCENT_0, WP_COLOR_0);
  lcd.setTextFont(fonts[AKROBAT_BOLD_18]);
  lcd.setTextDatum(ML_DATUM);
  lcd.drawString("Installing firmware update", 5, lcd.height()/2);
}

void GUI::drawPowerOff() {
  // Just a blank screen
  lcd.fillScreen(TFT_BLACK);
}

void HeaderWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  //log_i("redraw HeaderWidget");

  lcd.fillRect(screenOffX, screenOffY, windowWidth+1, windowHeight, WP_COLOR_0);

  /* ⚠ THE TITLE IS DRAWN LAST, not first — see the block after the clock.
   * It used to be drawn here with a plain lcd.drawString, which does not clip:
   * a long title (a file path, a SIP URI, a node name) ran the full width and
   * the icons and clock were then painted ON TOP of it. Sizing it needs the
   * width those right-aligned things actually consume, so it waits for them. */

  // - Battery state & WiFi icons
  const uint16_t space = 3;
  uint16_t xOff = space, w;
  xOff += (w = GUI::drawBatteryIcon(lcd, controlState, -1, screenOffX + windowWidth - xOff, screenOffY + 7));
  if (w) {
    xOff += space;
  }
  xOff += (w = GUI::drawWifiIcon(lcd, controlState, screenOffX + windowWidth - xOff - 20, screenOffY + 5));
  if (w) {
    xOff += space + 3;
  }
  xOff += (w = GUI::drawMessageIcon(lcd, controlState, screenOffX + windowWidth - xOff - 20, screenOffY + 6));
  if (w) {
    xOff += space;
  }
  xOff += (w = GUI::drawMuteIcon(lcd, controlState, screenOffX + windowWidth - xOff - 14, screenOffY + 7));
  if (w) {
    xOff += space;
  }

  // - Time
  if (ntpClock.isTimeKnown()) {
    lcd.setTextColor(TFT_WHITE, WP_COLOR_0);
    lcd.setTextFont(fonts[AKROBAT_BOLD_18]);      // original desing: AKROBAT_BOLD_16
    lcd.setTextDatum(MR_DATUM);
    char buff[6];
    sprintf(buff, "%02d:%02d", ntpClock.getHour(), ntpClock.getMinute());
    lcd.drawString(buff, screenOffX+windowWidth-xOff-3, screenOffY+windowHeight/2);
    xOff += 3 + lcd.textWidth(buff);              // the clock is right-side furniture too
  }

  /* - Title, sized against the space the icons and clock LEFT (see the note
   *   where it used to be drawn). titleDrawnW is what actually landed on the
   *   screen — the SSID block below needs the real width, not strlen's idea. */
  uint16_t titleDrawnW = 0;
  if (title) {
    lcd.setTextColor(WP_ACCENT_0, WP_COLOR_0);
    lcd.setTextFont(fonts[AKROBAT_BOLD_18]);
    lcd.setTextDatum(ML_DATUM);
    const int16_t avail = (int16_t)windowWidth - 8 - (int16_t)xOff - 6;
    if (avail > 0) {
      titleDrawnW = guiDrawEllipsized(lcd, title, (uint16_t)avail,
                                      screenOffX + 8, screenOffY + windowHeight/2);
    }
  }

  // - Current WiFi network name: small grey text between the title and the
  //   clock, right-aligned. Too-long names shorten to "start.X" (a single dot
  //   plus the LAST character) until they fit; skipped if there's no room.
  if (wifiState.isConnected() && wifiState.ssid() != NULL && wifiState.ssid()[0]) {
    /* titleDrawnW, not textWidth(title): the title is ellipsized now, so its
     * real width can be far less than the string's — measuring the string
     * would hide the SSID behind a title that is not actually that wide.
     * (No setTextFont needed to measure the neighbours any more: the title
     * measured itself as it drew, and the clock's width is already in xOff.)
     * ⚠ xOff ALREADY includes the clock, so do NOT subtract it again here. */
    uint16_t leftBound = screenOffX + 8 + titleDrawnW + 10;
    // Guarded subtraction: xOff is unsigned, and an underflow here would make
    // rightBound enormous and paint the SSID straight over the icons.
    uint16_t rightBound = ((uint32_t)xOff + 8 < (uint32_t)windowWidth)
                          ? (uint16_t)(screenOffX + windowWidth - xOff - 8)
                          : screenOffX;
    lcd.setTextFont(fonts[AKROBAT_BOLD_16]);
    if (rightBound > leftBound + 24) {
      uint16_t avail = rightBound - leftBound;
      const char* ssid = wifiState.ssid();
      char name[44];
      snprintf(name, sizeof(name), "%s", ssid);
      if (lcd.textWidth(name) > avail) {
        int len = strlen(name);
        bool fits = false;
        for (int keep = len - 2; keep >= 1 && !fits; keep--) {
          snprintf(name, sizeof(name), "%.*s.%c", keep, ssid, ssid[len - 1]);
          fits = lcd.textWidth(name) <= avail;
        }
        if (!fits) {
          name[0] = 0;                            // hopeless: draw nothing
        }
      }
      if (name[0]) {
        lcd.setTextColor(TFT_LIGHTGREY, WP_COLOR_0);
        lcd.setTextDatum(MR_DATUM);
        lcd.drawString(name, rightBound, screenOffY + windowHeight / 2);
      }
    }
  }
}

void FooterWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  log_d("redraw footer");
  /* Here's what we are drawing (in horizontal sense):
   *     +             +                 +                +                 +              +
   *     | Left button |                 | Input sequence |                 | Right button |
   *     +            x0              off0              off                x1
   *     <------------->
   *     <------------------------------->
   *     <------------------------------------------------>
   *     <------------------------------------------------------------------>
   */
  uint16_t xPad = 8;
  uint16_t x0 = screenOffX + xPad, x1 = screenOffX + windowWidth - xPad;   // button boundaries

  lcd.fillRect(screenOffX, screenOffY, windowWidth+1, windowWidth+1, WP_COLOR_0);

  if (!controlState.locked) {
    // Draw button text
    lcd.setTextFont(fonts[AKROBAT_SEMIBOLD_22]);
    lcd.setTextColor(WP_COLOR_1, WP_COLOR_0);
    if(leftButtonName != NULL) {
      //lcd.setTextDatum(TL_DATUM);
      //x0 += lcd.drawString(leftButtonName, screenOffX+1, screenOffY+1);
      lcd.setTextDatum(ML_DATUM);
      x0 += lcd.drawString(leftButtonName, x0, screenOffY+windowHeight/2);
    }
    if (rightButtonName != NULL) {
      //lcd.setTextDatum(TR_DATUM);
      //x1 -= lcd.drawString(rightButtonName, screenOffX+windowWidth-1, screenOffY+1);
      lcd.setTextDatum(MR_DATUM);
      x1 -= lcd.drawString(rightButtonName, x1, screenOffY+windowHeight/2);
    }

    // Display input characters
    bool inputSeq = false;

    /* ── THE PENDING T9 WORD ────────────────────────────────────────────────────────────
     * This is the whole T9 display. The word being predicted lives HERE, in the footer,
     * not inline in the text field — which is why the integration needed no changes to
     * TextInputWidget or MultilineTextWidget and cannot desynchronise from them. The word
     * is inserted into the field only when it is committed.
     *
     * "n/m" appears only when there is actually a choice, so the common case (91.9% of
     * digit keys match exactly one word) shows a bare word and no clutter. ASCII only —
     * the Akrobat faces are generated bitmaps with no glyph beyond it, and anything else
     * renders as a hollow box. */
    if (controlState.t9Available() && !controlState.inputCurKey) {
      /* Two things share this slot, and never at the same time: the word being predicted,
       * and — when nothing is pending — which mode '#' has left you in. The mode has to be
       * visible or it is invisible state: a user who cycled to 123 and walked away has no
       * way to know why letters stopped appearing. While MULTI-TAP is mid-letter,
       * inputCurKey is set and the character strip below wins, because that is the more
       * urgent thing to be looking at. */
      inputSeq = true;
      char strip[40];
      const char* word = controlState.t9.pending() ? controlState.t9.current() : NULL;
      if (!controlState.t9.pending()) {
        // Idle: just say where we are. "T9" included — knowing prediction is ON matters
        // as much as knowing it is off.
        snprintf(strip, sizeof(strip), "%s", controlState.inputModeName());
      } else if (!word) {
        // Nothing in the dictionary matches. Show the digits so the keypresses are visible
        // while the user decides to backspace or press # for multi-tap.
        snprintf(strip, sizeof(strip), "%s ?", controlState.t9.digits());
      } else if (controlState.t9.candidateCount() > 1) {
        snprintf(strip, sizeof(strip), "%s  %d/%d", word,
                 controlState.t9.selected() + 1, controlState.t9.candidateCount());
      } else {
        snprintf(strip, sizeof(strip), "%s", word);
      }
      if (controlState.t9.pending() &&
          (controlState.inputShift || controlState.t9Caps) &&
          strip[0] >= 'a' && strip[0] <= 'z') {
        /* Show the capital BEFORE it is committed, so a long press of '#' has visible
         * feedback and the user can see the rule fire. Never the mode tag: "Abc" is a
         * label, not a word. */
        strip[0] = toupper(strip[0]);
      }
      lcd.setTextDatum(TL_DATUM);
      const int fh = fonts[AKROBAT_SEMIBOLD_22]->height();
      int wpx = fonts[AKROBAT_SEMIBOLD_22]->textWidth(strip);
      if (wpx > (int)windowWidth) {
        wpx = windowWidth;
      }
      lcd.drawString(strip, screenOffX + (windowWidth - wpx) / 2,
                     screenOffY + (windowHeight - fh) / 2);
    } else if (controlState.inputCurKey) {
      uint8_t len = strlen(controlState.inputSeq);
      if (len) {
        inputSeq = true;
        uint16_t off0, off;
        // TODO: consider upper letters here if shift is pressed (controlState.inputShift); currently uppercase letters are not cetered
        off = off0 = (windowWidth-lcd.textWidth(controlState.inputSeq))/2;     // free pixels halved
        lcd.setTextDatum(TL_DATUM);
        int fontHeight = fonts[AKROBAT_SEMIBOLD_22]->height();

        // Draw each character separately
        bool currentCharacter;
        char s[2];
        s[1] = '\0';
        for (uint8_t i=0; i<len; i++) {

          // Shift pressed?
          s[0] = controlState.inputSeq[i];
          if (controlState.inputShift) {
            s[0] = toupper(s[0]);
          }

          // Character width
          currentCharacter = (i==controlState.inputCurSel);
          int charWidth = fonts[AKROBAT_SEMIBOLD_22]->textWidth(s);

          // Keep within visible window boundaries
          if (off>=0 && off<=windowWidth) {

            // Character background
            colorType bgColor = currentCharacter ? SALAD : WHITE;
            lcd.fillRect(screenOffX+off, screenOffY, charWidth, fontHeight, bgColor);

            // Actually draw character
            lcd.setTextColor(BLACK, bgColor);
            lcd.drawString(s, screenOffX + off, screenOffY+1);

          }

          off += charWidth;

        }

        // Draw background from x0 to off0
        if (x0<off0) {
          lcd.fillRect(x0, screenOffY, off0-x0, windowHeight, WP_COLOR_0);
        }

        // Draw background from off to x1
        if (off<x1) {
          lcd.fillRect(off, screenOffY, 1, windowHeight, currentCharacter ? SALAD : WHITE);      // add 1px to make "1" more visible
          lcd.fillRect(off+1, screenOffY, x1-off-1, windowHeight, WP_COLOR_0);
        }

        // Filler below characters (from x0 to x1)
        lcd.fillRect(x0, screenOffY+fontHeight, x1-x0, windowHeight-fontHeight, WP_COLOR_0);
      }
    }
    if (!inputSeq) {
      // Theme filler between left and right button texts (from x0 to x1)
      lcd.fillRect(x0, screenOffY+1, x1-x0, windowHeight, WP_COLOR_0);
    }
  } else {
    // Display only unlock text
    lcd.setTextFont(fonts[AKROBAT_SEMIBOLD_22]);
    lcd.setTextColor(WP_COLOR_1, WP_COLOR_0);
    lcd.setTextDatum(MC_DATUM);
    const char* msg = (controlState.unlockButton1 == WIPHONE_KEY_OK) ? "Press * to unlock" : "Locked. Press OK";
    lcd.drawString(msg, windowWidth/2, screenOffY + windowHeight/2);
    log_d("footer draw: %d", controlState.unlockButton1);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Menu widget  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/* THE MARQUEE'S ONE INSTANCE. See the note on MenuWidget::marqueeTick in GUI.h for the idea;
 * this is the state. `owner`/`row`/`clock` persist between passes; the rest is scratch that
 * MenuWidget::redraw sets before painting its selected row and drawRowText fills in while
 * painting it. Plain statics on purpose: 24 bytes, no allocation, no constructor order. */
struct MenuMarqueeState {
  MenuWidget*  owner;        // the list whose selected row overflows, or NULL (no ticking)
  uint16_t     row;          // its optionSelectedIndex when ownership was taken
  MarqueeClock clock;        // offset in glyphs + phase, menu_marquee.h
  bool         drawn;        // liveness: the owner's row was painted since the last tick
  // Scratch for the row being painted right now:
  bool         probing;      // the selected row of a MenuWidget is being painted
  uint16_t     drawOffset;   // glyphs the texts start from (0 unless this is the owner)
  bool         overflow;     // some text on the row is wider than its space
  bool         moreToCome;   // some text has not shown its end at drawOffset
};
static MenuMarqueeState gMarquee = { NULL, 0, { 0, MARQUEE_HOLD_START, 0 }, false, false, 0, false, false };

MenuWidget::MenuWidget(uint16_t xPos, uint16_t yPos, uint16_t width, uint16_t height, const char* empty,
                       SmoothFont* font, uint8_t itemsPerScreen, uint16_t leftOffset, bool opaque)
  : FocusableWidget(xPos, yPos, width, height), options(), opaque(opaque), leftOffset(leftOffset), widgetFont(font) {
  if (font==NULL) {
    widgetFont = fonts[OPENSANS_COND_BOLD_20];
  }
  if (empty) {
    this->emptyMessageDyn = extStrdup(empty);
  }
  optionSelectedIndex = 0;
  optionOffsetIndex = 0;
  optionHeight = itemsPerScreen ? height / itemsPerScreen : widgetFont->height() + spacing;
  optionsVisible = optionHeight ? height / optionHeight : 0;
  chosenKey = 0;
  drawOnce = true;    // redraw entirely once
}

void MenuWidget::deleteAll() {
  /* 🔑 DELETE THE ROWS, NOT JUST THE POINTERS TO THEM. options.clear() frees the
   * LinearArray's pointer block and nothing else, so every MenuOption this widget owned was
   * leaked - the object, its title, its subtitle and its two IconRle3s. ~MenuWidget already
   * does this correctly in its own loop; deleteAll() simply never adopted it.
   *
   * The one caller is the SIP accounts VIEWING screen (see the rebuild below it), which
   * re-enters every time an account is opened - so this leaked three iconned rows per open,
   * out of the ~20 KB internal heap, for the life of the boot. Found while tracing what the
   * Nodes screen spends internal RAM on: the rows are now PSRAM (see MenuOption in GUI.h),
   * but a leak of them is still a leak, and the icons were never covered by that at all. */
  for (uint16_t i = 0; i < options.size(); i++) {
    delete options[i];
  }
  options.clear();
  optionSelectedIndex = 0;
  optionOffsetIndex = 0;
  chosenKey = 0;
  drawOnce = true;
  marqueeForget(this);       // the row being scrolled no longer exists
}

MenuWidget::~MenuWidget() {
  // Delete each of the options one-by-one
  for(uint16_t i=0; i<options.size(); i++) {
    delete options[i];
  }
  freeNull((void **) &this->emptyMessageDyn);
  /* ⚠ Not optional: the next MenuWidget can land at this very address (the lists are all
   * the same size and PSRAM hands the block straight back), and a stale owner would then
   * match `this` by coincidence and apply an old offset to a new list. */
  marqueeForget(this);
}

bool MenuWidget::processEvent(EventType event) {
  if (!options.size()) {
    return false;
  }
  if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {
    // Move selection up or down the list
    if (event == WIPHONE_KEY_DOWN) {
      // Move selection down
      optionSelectedIndex = (optionSelectedIndex + 1) % options.size();
    } else if (event == WIPHONE_KEY_UP) {
      // Move selection up
      optionSelectedIndex = (optionSelectedIndex > 0) ? optionSelectedIndex - 1 : options.size() - 1;
    }
    this->revealSelected();
    return true;
  } else if (LOGIC_BUTTON_OK(event) || event==WIPHONE_KEY_RIGHT) {
    if (options.size()) {
      chosenKey = options[optionSelectedIndex]->id;
      log_v("menu: chosen: %d", chosenKey);
    }
    return true;
  }
  return false;
}

void MenuWidget::revealSelected() {
  // Ensure selection is shown
  if (optionSelectedIndex >= optionOffsetIndex + optionsVisible) {
    optionOffsetIndex += optionSelectedIndex - optionOffsetIndex - optionsVisible + 1;
    drawScroll = true;
  } else if (optionOffsetIndex > optionSelectedIndex) {
    optionOffsetIndex = optionSelectedIndex;
    drawScroll = true;
  }
}

const char* MenuWidget::getSelectedTitle() {
  log_d("getSelectedTitle MenuWidget");
  if (options.size() && optionSelectedIndex < options.size()) {
    return (const char*) options[optionSelectedIndex]->titleDyn;
  }
  return NULL;
}

void MenuWidget::setStyle(uint8_t styleNum, colorType textCol, colorType bgCol, colorType selTextCol, colorType selBgCol) {
  if (styleNum==1) {
    style1TextColor = textCol;
    style1BgColor = bgCol;
    style1SelTextColor = selTextCol;
    style1SelBgColor = selBgCol;
  } else if (styleNum==2) {
    style2TextColor = textCol;
    style2BgColor = bgCol;
    style2SelTextColor = selTextCol;
    style2SelBgColor = selBgCol;
  }
}

void MenuWidget::setStyle(uint8_t styleNum, colorType textCol, colorType bgCol, colorType selBgCol) {
  this->setStyle(styleNum, textCol, bgCol, textCol, selBgCol);
}

void MenuWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  // Fill background
  //corrRect(lcd, screenOffX, screenOffY, windowWidth, windowHeight, BLACK);

  // Print option names
  if (options.size() > 0) {
    lcd.setTextDatum(CL_DATUM);
    if (!widgetFont) {
      widgetFont = fonts[OPENSANS_COND_BOLD_20];
    }
    uint8_t maxChars = windowWidth / lcd.textWidth("M");      // TODO: remove maxChars
    uint16_t yOff = 0, xOff;
    for(uint16_t i=optionOffsetIndex; i<options.size() && i<optionOffsetIndex+optionsVisible; i++) {
      if (yOff+optionHeight > windowHeight) {
        if (yOff < windowHeight) {
          // TODO: there is extra place -> display an arrow
        }
        break;
      }

      // Choose colors (one of four options)
      uint16_t textColor, bgColor;
      if (i!=optionSelectedIndex) {
        textColor = options[i]->style==1 ? style1TextColor : style2TextColor;
        bgColor   = options[i]->style==1 ? style1BgColor : style2BgColor;
      } else {
        textColor = options[i]->style==1 ? style1SelTextColor : style2SelTextColor;
        bgColor   = options[i]->style==1 ? style1SelBgColor : style2SelBgColor;
      }

      /* The selected row is the marquee candidate. `mine` = it is the row we are already
       * scrolling; anything else (another list, a moved selection, a rebuilt list under the
       * same index) paints from the start and, if it overflows, takes over with a fresh
       * clock — the old offset is never applied to a different text. */
      const bool probe = (i == optionSelectedIndex);
      bool mine = false;
      if (probe) {
        mine = (gMarquee.owner == this && gMarquee.row == optionSelectedIndex);
        gMarquee.probing = true;
        gMarquee.drawOffset = mine ? gMarquee.clock.offset : 0;
        gMarquee.overflow = false;
        gMarquee.moreToCome = false;
      }

      options[i]->redraw(lcd, screenOffX, screenOffY + yOff, windowWidth, optionHeight, textColor, bgColor, opaque, i==optionSelectedIndex, widgetFont, leftOffset);

      if (probe) {
        gMarquee.probing = false;
        if (gMarquee.overflow) {
          if (!mine) {
            gMarquee.owner = this;
            gMarquee.row = optionSelectedIndex;
            marqueeReset(&gMarquee.clock, millis());
          }
          gMarquee.drawn = true;
        } else if (gMarquee.owner == this) {
          gMarquee.owner = NULL;        // the selected row fits: nothing to scroll, no ticks
        }
      }

      yOff += optionHeight;
    }
    lcd.setSmoothTransparency(false);

    // Print blacks
    if (opaque && yOff < windowHeight) {
      if (drawOnce || drawItems) {
        corrRect(lcd, screenOffX, screenOffY + yOff, windowWidth, windowHeight - yOff, style1BgColor);
      }
    }
  } else if (this->emptyMessageDyn) {
    if (drawOnce || drawItems) {
      if (opaque) {
        corrRect(lcd, screenOffX, screenOffY, windowWidth, windowHeight, style1BgColor);
      }
      lcd.setTextFont(fonts[AKROBAT_BOLD_18]);
      lcd.setTextDatum(TL_DATUM);
      lcd.setTextColor(GRAY, style1BgColor);
      lcd.drawFitString(this->emptyMessageDyn, windowWidth-10, screenOffX + 5, screenOffY + 5);
    }
  }

  if (!lcd.isSprite() || !opaque) {
    drawOnce = drawItems = drawScroll = false;
  }
}

MenuOption::keyType MenuWidget::readChosen() {
  MenuOption::keyType r = chosenKey;
  chosenKey = 0;
  return r;
}

const char* MenuWidget::readChosenTitle() {
  MenuOption::keyType r = chosenKey;
  chosenKey = 0;

  // Extract title
  for (uint16_t i=0; i<options.size(); i++) {
    if (options[i]->id == r) {
      return options[i]->titleDyn;
    }
  }
  return NULL;
}

bool MenuWidget::addOption(MenuOption* option) {
  if (options.add(option)) {
    drawItems = true;
    return true;
  }
  return false;
}

void MenuWidget::addOption(const char* title) {
  this->addOption(title, options.size()+1, 1);
}

void MenuWidget::addNote(const char* title, uint16_t style) {
  this->addOption(title, MENU_ROW_NOTE, style);
}

struct NoteWrapCtx {
  MenuWidget* menu;
  SmoothFont* font;
  uint16_t maxW;
  uint16_t style;
};

static size_t noteWrapFit(const char* s, void* ctx) {
  NoteWrapCtx* c = (NoteWrapCtx*)ctx;
  return (size_t)c->font->fitTextLength(s, c->maxW, 1);   // bytes that fit, UTF-8 aware
}

static void noteWrapRow(const char* s, size_t len, void* ctx) {
  NoteWrapCtx* c = (NoteWrapCtx*)ctx;
  char line[96];
  if (len >= sizeof(line)) {
    len = sizeof(line) - 1;
  }
  memcpy(line, s, len);
  line[len] = 0;
  c->menu->addNote(line, c->style);
}

void MenuWidget::addNoteWrapped(const char* text, uint16_t style) {
  if (!text || !*text || !widgetFont) {
    return;
  }
  // The row draws from leftOffset; keep a few pixels off the right edge, as the glyphs do.
  NoteWrapCtx c;
  c.menu = this;
  c.font = widgetFont;
  c.maxW = (widgetWidth > leftOffset + 12) ? (uint16_t)(widgetWidth - leftOffset - 12) : widgetWidth;
  c.style = style;
  wrapNote(text, noteWrapFit, noteWrapRow, &c, 8);   // 8 rows is more than any note deserves
}

void MenuWidget::addOption(const char* title, MenuOption::keyType key, uint16_t style) {
  if (key) {
    MenuOption* option = new MenuOption(key, style, title);
    if (!this->addOption(option)) {
      delete option;
    }
  } else {
    /* Naming the fix in the message: this log line was on the phone for the whole time the
     * Photos, Books, Files and Music result rows were silently missing, and by itself it told
     * nobody what to do about it. */
    log_e("menu option key is 0 - row DROPPED, not added. For a display-only row use addNote()");
  }
}

void MenuWidget::addOption(const char* title, const char* subTitle, MenuOption::keyType key, uint16_t style,
                           const unsigned char* iconData, const uint16_t iconSize,
                           const unsigned char* selIconData, const uint16_t selIconSize) {
  MenuOptionIconned* option = new MenuOptionIconned(key, style, title, subTitle, iconData, iconSize, selIconData, selIconSize);
  if (!addOption(option)) {
    delete option;
  }
}

void MenuWidget::select(MenuOption::keyType key) {
  for (int i=0; i<options.size(); i++)
    if (options[i]->id == key) {
      optionSelectedIndex = i;
      this->revealSelected();
      break;
    }
}

void MenuWidget::selectLastOption() {
  optionSelectedIndex = options.size()-1;
}

bool MenuWidget::marqueeTick(uint32_t now) {
  if (!gMarquee.owner) {
    return false;                 // the common case: no row overflows, one pointer test
  }
  if (!gMarquee.drawn) {
    /* We asked for a repaint and the owner's row was not in it: the app is showing
     * something else (a text view, a call, a dialog) while the list object lives on. Stop
     * asking — the row re-arms itself the next time it is actually painted. Without this a
     * Meshtastic compose screen would be repainted 5 times a second by a list behind it. */
    gMarquee.owner = NULL;
    return false;
  }
  if (!marqueeAdvance(&gMarquee.clock, now, gMarquee.moreToCome)) {
    return false;
  }
  gMarquee.drawn = false;         // the repaint we are asking for must set it again
  return true;
}

void MenuWidget::marqueeForget(MenuWidget* w) {
  if (gMarquee.owner == w) {
    gMarquee.owner = NULL;
  }
}

//void MenuWidget::removeOption(uint16_t key) {
//  // TODO
//}
//
//void MenuWidget::sortOptions() {
//  // TODO
//}
//
//void MenuWidget::restoreOrderLast(uint16_t i) {
//  // TODO
//}

OptionsMenuWidget::OptionsMenuWidget(uint16_t xPos, uint16_t yPos, uint16_t width, uint16_t height)
  : MenuWidget(xPos, yPos, width, height, "No options available", fonts[AKROBAT_BOLD_20], N_OPTION_ITEMS, 8, true) {
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Menu option  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

MenuOption::MenuOption() {
  id = 0;
  style = 0;
  titleDyn = NULL;
}

MenuOption::~MenuOption() {
  freeNull((void **) &titleDyn);
};

/* Menu rows go to PSRAM — see the note on MenuOption in GUI.h. Mirrors
 * AbstractWidget::operator new exactly, including the internal-heap fallback for the case
 * where PSRAM is unavailable or full, and free() serves both regions. */
void* MenuOption::operator new(size_t n) {
  void* p = ps_malloc(n);
  return p ? p : malloc(n);
}

void MenuOption::operator delete(void* p) {
  free(p);
}

MenuOption::MenuOption(MenuOption::keyType pId, uint16_t pStyle, const char* title)
  : id(pId), style(pStyle) {
  /* extStrdup, not strdup: the OBJECT moving to PSRAM is only half of a row. The title is a
   * second internal-heap allocation of its own, and on the Nodes screen it is the node name
   * for every node in the database. Falls back to the internal heap by itself, and
   * ~MenuOption's freeNull() -> free() is region-agnostic. */
  titleDyn = title ? extStrdup(title) : NULL;
};

/* Draw `s` within maxW, with ".." marking anything cut off; returns the width
 * actually drawn. drawFitString's hard cut reads as COMPLETE text — "Measure
 * distances from h" looked whole on the 240px screen and the field report was
 * that the label just said something odd (2026-08-19). And a plain drawString
 * does not clip AT ALL: it paints straight off the edge, or under the icons
 * that get drawn on top of it afterwards.
 *
 * ".." not "…": plain ASCII glyphs exist in every font we load.
 *
 * Cost: a textWidth() per trimmed char, only on strings that actually overflow,
 * only on redraws (keypress-driven) — nothing here runs per frame. */
/* The phonebook name for a correspondent, or NULL when they are not in it.
 *
 * ⚠ MATCHED ON THE GROUPING IDENTITY, NEVER ON THE STRING. The same person reaches
 * the message store as `14255550142@seattle1.voip.ms`, `+14255550142` or a bare
 * `4255550142` depending on which way the message came in, and the phonebook holds
 * a fourth spelling again. sipThreadIdentity() is the one normaliser that makes all
 * of those one person — the same one the threads are grouped by, so a name can never
 * attach to a row the identity says is somebody else.
 *
 * Both address fields are checked: a contact reachable only over LoRa has just `l`,
 * and its messages are stored under that hex id.
 *
 * Cost is one scan of the phonebook per label, on menu builds and screen entry only —
 * nothing here runs per frame. The returned pointer lives in the phonebook's PSRAM;
 * copy it if you intend to keep it (HeaderWidget::setTitle does NOT copy).
 */
static const char* sipContactName(Storage& flash, const char* peer, const char* uri) {
  const char* addr = (peer && peer[0]) ? peer : uri;
  if (!addr || !addr[0]) {
    return NULL;
  }
  /* ⚠ NEVER LOADS. The phonebook comes off SPIFFS, and SPIFFS on this board takes its
   * stalls in one indivisible piece — see the bench numbers in docs. A lazy load here
   * would drop that stall in the middle of drawing a menu row. MessagesApp's constructor
   * loads it once, at the moment the screen is already busy loading the message store;
   * an unloaded phonebook simply means no names, which is what the old code showed. */
  if (flash.phonebook.nSections() < 2) {
    return NULL;
  }

  char want[SMS_MIRROR_PEER_MAX];
  sipThreadIdentity(addr, want, sizeof(want));
  if (!want[0]) {
    return NULL;
  }

  for (auto si = flash.phonebook.iterator(1); si.valid(); ++si) {
    const char* name = si->getValueSafe("n", "");
    if (!name[0]) {
      continue;                       // an entry with no name has nothing to contribute
    }
    static const char* const FIELDS[] = { "s", "l" };
    for (int f = 0; f < 2; f++) {
      const char* have = si->getValueSafe(FIELDS[f], "");
      if (!have[0]) {
        continue;
      }
      char id[SMS_MIRROR_PEER_MAX];
      sipThreadIdentity(have, id, sizeof(id));
      if (id[0] && !strcmp(id, want)) {
        return name;
      }
    }
  }
  return NULL;
}

/* The human label for a SIP correspondent: their name if you have one, else the address.
 *
 * The name is the point — {user}, 2026-08-28: "I don't have each contact member number
 * memorized, I have to keep clicking the conversation to see who is who." A row that
 * says `4255550142` is a lookup task; one that says `Covey` is an answer.
 *
 * `peer` is the GROUPING IDENTITY (sipThreadIdentity): bare digits for a phone
 * number — the nice form — but for a long address it is a bounded, hash-suffixed
 * key like "sip:alice.wo#hxafym" that nobody should ever be shown. In that case
 * the full `uri` is the truth, and the draw site ellipsizes it honestly. */
static const char* sipDisplayLabel(Storage& flash, const char* peer, const char* uri) {
  const char* name = sipContactName(flash, peer, uri);
  if (name) {
    return name;
  }
  const bool digits = peer && peer[0] && strspn(peer, "0123456789") == strlen(peer);
  if (digits || !uri || !uri[0]) {
    return (peer && peer[0]) ? peer : (uri ? uri : "");
  }
  return uri;
}

uint16_t guiDrawEllipsized(LCD &lcd, const char* s, uint16_t maxW, int16_t x, int16_t y) {
  if (!s || !*s) {
    return 0;
  }
  uint16_t full = lcd.textWidth(s);
  if (full <= maxW) {
    lcd.drawString(s, x, y);
    return full;
  }
  char buf[96];
  size_t n = strlen(s);
  if (n > sizeof(buf) - 3) {
    n = sizeof(buf) - 3;
  }
  buf[0] = '\0';
  while (n > 0) {
    /* Never cut inside a UTF-8 sequence: a lone continuation byte (10xxxxxx)
     * draws as a garbage glyph, and book titles and track names really do
     * carry accents. Step back to the start of the character. */
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) {
      n--;
    }
    if (n == 0) {
      break;
    }
    memcpy(buf, s, n);
    buf[n] = '.';
    buf[n + 1] = '.';
    buf[n + 2] = '\0';
    if (lcd.textWidth(buf) <= maxW) {
      break;
    }
    n--;
  }
  if (n == 0) {
    return 0;                 // not even "x.." fits: draw nothing, never overflow
  }
  lcd.drawString(buf, x, y);
  return lcd.textWidth(buf);
}

static int marqueeMeasure(void* ctx, const char* s) {
  return ((LCD*)ctx)->textWidth(s);
}

/* The mid-scroll cut: the longest prefix of `s` that fits in maxW, drawn hard-cut on the
 * right. ⚠ noinline ON PURPOSE (review, 2026-09-19): this frame carries a 160-byte buffer,
 * and drawRowText() is on the path of EVERY row of EVERY list paint. Inlined, the buffer
 * sat under the plain ellipsis path too — 224 B deeper for every unselected row, on a loop
 * task whose recorded floor is 408 B (HANDOFF, 0.9.63 after 22 h). Kept out of line, the
 * buffer is live only on the one path that needs it. 160 bytes covers a 232 px row of the
 * narrowest glyphs with room for UTF-8; a longer fit is cut back to a glyph boundary. */
static void __attribute__((noinline)) drawRowTextCut(LCD &lcd, SmoothFont* font, const char* s,
                                                     uint16_t maxW, int16_t x, int16_t y) {
  const int16_t fit = font ? font->fitTextLength(s, maxW) : 0;
  if (fit <= 0) {
    return;
  }
  char buf[160];
  size_t n = (size_t)fit;
  if (n > sizeof(buf) - 1) {
    n = sizeof(buf) - 1;
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) {
      n--;
    }
  }
  memcpy(buf, s, n);
  buf[n] = '\0';
  lcd.drawString(buf, x, y);
}

/* THE ONE TEXT PRIMITIVE FOR MENU ROWS. An unselected row, or a selected one whose text
 * fits, gets exactly what guiDrawEllipsized gave it. The SELECTED row of the list that
 * MenuWidget::redraw is probing is the marquee: it reports that it overflowed, paints from
 * the current glyph offset, and says whether there is more to come — the widget and the tick
 * do the rest. `font` is the face already set on `lcd`; it is needed here because TFT_eSPI
 * keeps smoothFont protected and fitting whole glyphs without drawFitString's malloc needs
 * the font itself. Holding at offset 0 paints the ".." form, so nothing jumps when the
 * selection lands on a long row; mid-scroll is a hard cut on the right, which reads as
 * motion. Never hands drawString more than maxW of text (menu_marquee.h, "both ends"). */
static void drawRowText(LCD &lcd, SmoothFont* font, const char* s, uint16_t maxW, int16_t x, int16_t y, bool selected) {
  if (!s || !*s) {
    return;
  }
  if (!selected || !gMarquee.probing) {
    guiDrawEllipsized(lcd, s, maxW, x, y);
    return;
  }
  if (lcd.textWidth(s) <= maxW) {
    lcd.drawString(s, x, y);
    return;
  }
  gMarquee.overflow = true;
  if (gMarquee.drawOffset == 0) {
    gMarquee.moreToCome = true;
    guiDrawEllipsized(lcd, s, maxW, x, y);
    return;
  }
  bool atEnd = false;
  const size_t b = marqueeStart(s, gMarquee.drawOffset, maxW, marqueeMeasure, &lcd, &atEnd);
  if (atEnd) {
    lcd.drawString(s + b, x, y);          // the whole tail fits: measured above
    return;
  }
  gMarquee.moreToCome = true;
  drawRowTextCut(lcd, font, s + b, maxW, x, y);
}

uint16_t gScrimColor = THEME_SCRIM_COLOR;
uint8_t  gScrimAlpha = THEME_SCRIM_ALPHA;

void guiDrawScrim(LCD &lcd, int16_t x, int16_t y, uint16_t w, uint16_t h) {
  if (!w || !h || !gScrimAlpha) {
    return;                     // alpha 0 is "no scrim at all" — the pre-2026-08-25 look
  }
  lcd.setWindow(x, y, x + w - 1, y + h - 1);
  lcd.pushTransparent(gScrimColor, gScrimAlpha, (int32_t)w * (int32_t)h);
}

void MenuOption::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight,
                        uint16_t textColor, uint16_t bgColor, bool opaque, bool selected, SmoothFont* font, uint16_t leftOffset) {
  // Draw background if needed
  if (opaque || selected) {
    lcd.fillRect(screenOffX, screenOffY, windowWidth, windowHeight, bgColor);
    lcd.setSmoothTransparency(false);
  } else {
    /* ⚠ NOT opaque means this row is sitting on the WALLPAPER, and white-on-anything is not
     * a contrast guarantee. Lay a translucent grey plate first so the words have a bounded
     * background whatever the user's picture happens to contain. See THEME_SCRIM_* in GUI.h.
     * Transparency stays ON afterwards so the glyph edges blend into the plate rather than
     * against a colour this row does not know. */
    guiDrawScrim(lcd, screenOffX, screenOffY, windowWidth, windowHeight);
    lcd.setSmoothTransparency(true);
  }

  // Print text
  lcd.setTextFont(font);
  lcd.setTextColor(textColor, bgColor);
  drawRowText(lcd, font, titleDyn, windowWidth - leftOffset, screenOffX + leftOffset, screenOffY + windowHeight/2, selected);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Menu option with icon & subtitle - - - - - - - - - - - - - - - - - - - - - - - - - - - -

MenuOptionIconned::MenuOptionIconned(MenuOption::keyType pId, uint16_t pStyle, const char* title,
                                     const char* subTitle,
                                     const unsigned char* iconData, const uint16_t iconSize,
                                     const unsigned char* selIconData, const uint16_t selIconSize,
                                     uint8_t textOffset, colorType selBgColor)
  : MenuOption(pId, pStyle, title), selectedBgColor(selBgColor) {
  if (subTitle) {
    /* The THIRD internal allocation per row, and the biggest of them — on Nodes this is the
     * whole "1.2km NE of camp, 4 min ago" line. To PSRAM with the other two. */
    subTitleDyn = extStrdup(subTitle);
  } else {
    subTitleDyn = NULL;
  }
  if (iconData && iconSize) {
    icon = new IconRle3(iconData, iconSize);
  }
  if (selIconData && selIconSize) {
    iconSelected = new IconRle3(selIconData, selIconSize);
  }
  textLeftOffset = textOffset;
}

MenuOptionIconned::~MenuOptionIconned() {
  freeNull((void **) &subTitleDyn);
  if (icon) {
    delete icon;
  }
  if (iconSelected) {
    delete iconSelected;
  }
}

void MenuOptionIconned::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight,
                               uint16_t textColor, uint16_t bgColor, bool opaque, bool selected, SmoothFont* font, uint16_t leftOffset) {
  // Draw background if needed
  if (opaque || selected) {
    lcd.fillRect(screenOffX, screenOffY, windowWidth, windowHeight, bgColor);
    lcd.setSmoothTransparency(false);
  } else {
    /* ⚠ NOT opaque means this row is sitting on the WALLPAPER, and white-on-anything is not
     * a contrast guarantee. Lay a translucent grey plate first so the words have a bounded
     * background whatever the user's picture happens to contain. See THEME_SCRIM_* in GUI.h.
     * Transparency stays ON afterwards so the glyph edges blend into the plate rather than
     * against a colour this row does not know. */
    guiDrawScrim(lcd, screenOffX, screenOffY, windowWidth, windowHeight);
    lcd.setSmoothTransparency(true);
  }

  // Draw icons
  uint16_t iconOffset = 0;
  if (selected && iconSelected) {
    if (selected && selectedBgColor!=IGNORED_COLOR) {
      lcd.fillRect(screenOffX, screenOffY, iconSelected->width(), iconSelected->height(), selectedBgColor);
    }
    lcd.drawImage(*iconSelected, screenOffX + leftOffset, screenOffY + (windowHeight - iconSelected->height())/2 );
    iconOffset = iconSelected->width();
  } else if (icon) {
    lcd.drawImage(*icon, screenOffX + leftOffset, screenOffY + (windowHeight - icon->height())/2 );
    iconOffset = icon->width();
  }
  if (iconOffset) {
    iconOffset += textLeftOffset;
  }

  // Print text
  lcd.setTextColor(textColor, bgColor);
  lcd.setTextDatum(ML_DATUM);
  uint16_t allotted = windowWidth - iconOffset - leftOffset;
  if (subTitleDyn == NULL) {
    lcd.setTextFont(font);
    drawRowText(lcd, font, titleDyn, allotted, screenOffX + leftOffset + iconOffset, screenOffY + windowHeight/2, selected);
  } else {
    lcd.setTextFont(font);
    drawRowText(lcd, font, titleDyn, allotted, screenOffX + leftOffset + iconOffset, screenOffY + (windowHeight-fonts[AKROBAT_BOLD_16]->height())/2, selected);
    lcd.setTextFont(fonts[AKROBAT_BOLD_16]);
    drawRowText(lcd, fonts[AKROBAT_BOLD_16], subTitleDyn, allotted, screenOffX + leftOffset + iconOffset, screenOffY + font->height() + (windowHeight-font->height())/2, selected);
  }
}

MenuOptionIconnedTimed::MenuOptionIconnedTimed(MenuOption::keyType pId, uint16_t pStyle, const char* title, const char* subTitle, uint32_t zeit, uint16_t globalBg,
    const unsigned char* iconData, const uint16_t iconSize,
    const unsigned char* selIconData, const uint16_t selIconSize)
  : MenuOptionIconned(pId, pStyle, title, subTitle, iconData, iconSize, selIconData, selIconSize), zeit(zeit), globalBgColor(globalBg) {
};

void MenuOptionIconnedTimed::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight,
                                    uint16_t textColor, uint16_t bgColor, bool opaque, bool selected, SmoothFont* font, uint16_t leftOffset) {
  // Draw background if needed
  if (opaque || selected) {
    lcd.fillRect(screenOffX, screenOffY, windowWidth, windowHeight, bgColor);
    lcd.setSmoothTransparency(false);
  } else {
    /* ⚠ NOT opaque means this row is sitting on the WALLPAPER, and white-on-anything is not
     * a contrast guarantee. Lay a translucent grey plate first so the words have a bounded
     * background whatever the user's picture happens to contain. See THEME_SCRIM_* in GUI.h.
     * Transparency stays ON afterwards so the glyph edges blend into the plate rather than
     * against a colour this row does not know. */
    guiDrawScrim(lcd, screenOffX, screenOffY, windowWidth, windowHeight);
    lcd.setSmoothTransparency(true);
  }

  // Draw icons
  uint16_t iconOffset = 0;
  if (selected && iconSelected) {
    lcd.drawImage(*iconSelected, screenOffX + leftOffset, screenOffY + (windowHeight - iconSelected->height())/2 );
    iconOffset = iconSelected->width();
  } else if (icon) {
    lcd.drawImage(*icon, screenOffX + leftOffset, screenOffY + (windowHeight - icon->height())/2 );
    iconOffset = icon->width();
  }
  if (iconOffset) {
    iconOffset += 12;  // hardcoded parameter (same effect as textLeftOffset)
  }

  // Print text
  lcd.setTextColor(textColor, bgColor);
  // - Date / Ago
  char ago[20];
  const uint16_t rightOffset = 8;
  ntpClock.dateTimeAgo(zeit, ago);
  lcd.setTextFont(fonts[AKROBAT_BOLD_18]);
  lcd.setTextDatum(MR_DATUM);
  uint16_t dateWidth = lcd.drawString(ago, windowWidth - rightOffset, screenOffY + (windowHeight-fonts[AKROBAT_BOLD_16]->height())/2);
  /* - Name. ⚠ THIS CLASS HAS ITS OWN DRAW PATH — ellipsizing MenuOption and
   *   MenuOptionIconned does NOT cover the chats list, which is built from
   *   MenuOptionIconnedTimed. Missing that is how the chats rows kept a hard
   *   cut while every other list had "..". */
  lcd.setTextFont(font);
  lcd.setTextDatum(ML_DATUM);
  drawRowText(lcd, font, titleDyn, windowWidth - leftOffset - iconOffset - rightOffset - dateWidth, screenOffX + leftOffset + iconOffset, screenOffY + (windowHeight-fonts[AKROBAT_BOLD_16]->height())/2, selected);
  // - Subtitle (message text)
  if (subTitleDyn != NULL) {
    lcd.setTextFont(fonts[AKROBAT_BOLD_16]);
    lcd.setTextColor(selected ? textColor : WP_DISAB_0, bgColor);
    uint16_t allotted = windowWidth - iconOffset - leftOffset;
    drawRowText(lcd, fonts[AKROBAT_BOLD_16], subTitleDyn, allotted, screenOffX + iconOffset + leftOffset, screenOffY + font->height() + (windowHeight-font->height())/2, selected);
  }
}

MenuOptionPhonebook::MenuOptionPhonebook(MenuOption::keyType pId, uint16_t pStyle, const char* title, const char* subTitle)
  : MenuOptionIconned(pId, pStyle, title, subTitle, icon_person_b, sizeof(icon_person_b), icon_person_w, sizeof(icon_person_w)) {
};

void MenuOptionPhonebook::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight,
                                 uint16_t textColor, uint16_t bgColor, bool opaque, bool selected, SmoothFont* font, uint16_t leftOffset) {
  // Load icons
  uint16_t leftIconOff = 0;
  uint16_t rightIconOff = 0;
  IconRle3 *leftIcon;
  IconRle3 *rightIcon;
  if (selected) {
    leftIcon = iconSelected;
    rightIcon = new IconRle3(icon_phone_w, sizeof(icon_phone_w));
  } else {
    leftIcon = icon;
    rightIcon = new IconRle3(icon_phone_b, sizeof(icon_phone_b));
  }

  // Draw background if needed
  if (opaque || selected) {
    lcd.fillRect(screenOffX + (leftIcon && selected ? leftIcon->width() : 0), screenOffY, windowWidth, windowHeight, bgColor);
    lcd.setSmoothTransparency(false);
  } else {
    lcd.setSmoothTransparency(true);
  }

  // Draw icons
  if (leftIcon) {
    if (selected) {
      lcd.fillRect(screenOffX, screenOffY, leftIcon->width(), leftIcon->height(), WP_ACCENT_0);
    }
    lcd.drawImage(*leftIcon, screenOffX, screenOffY + (windowHeight - leftIcon->height())/2 );
    leftIconOff = leftIcon->width();
    // delete leftIcon; // you might think this should be deleted here, but you would be wrong.
  }
  if (rightIcon) {
    rightIconOff = rightIconOffset + rightIcon->width();
    lcd.drawImage(*rightIcon, screenOffX + windowWidth - rightIconOff, screenOffY + (windowHeight - rightIcon->height())/2 );
    delete rightIcon;
  }

  // Print text
  if (leftIconOff) {
    leftIconOff += 7;
  }
  uint16_t allotted = windowWidth - leftIconOff - rightIconOff;
  lcd.setTextColor(textColor, bgColor);
  lcd.setTextDatum(ML_DATUM);
  /* These were drawFitString — the phonebook was the one list the 2026-08-19 ellipsis sweep
   * missed, so a long contact name or address ended in a hard cut that read as complete.
   * Now the same primitive as every other row: ".." unselected, scrolling when selected. */
  if (subTitleDyn == NULL) {
    lcd.setTextFont(font);
    drawRowText(lcd, font, titleDyn, allotted, screenOffX + leftIconOff, screenOffY + windowHeight/2, selected);
  } else {
    lcd.setTextFont(font);
    drawRowText(lcd, font, titleDyn, allotted, screenOffX + leftIconOff, screenOffY + (windowHeight-fonts[AKROBAT_BOLD_16]->height())/2, selected);
    lcd.setTextFont(fonts[AKROBAT_BOLD_16]);
    drawRowText(lcd, fonts[AKROBAT_BOLD_16], subTitleDyn, allotted, screenOffX + leftIconOff, screenOffY + font->height() + (windowHeight-font->height())/2, selected);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Button widget  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ButtonWidget::ButtonWidget(uint16_t xPos, uint16_t yPos, const char* title,
                           uint16_t width, uint16_t height,
                           uint16_t col, uint16_t bgCol, uint16_t border, uint16_t sel, uint16_t selBg)
  : FocusableWidget(xPos, yPos, width, height),
    textColor(col), bgColor(bgCol), borderColor(border), selTextColor(sel), selBgColor(selBg) {
  pressed = false;
  titleDyn = extStrdup(title);
  if (!widgetWidth) {
    widgetWidth = fonts[OPENSANS_COND_BOLD_20]->textWidth(title) + 18;
  }
  updated = true;
}

ButtonWidget::~ButtonWidget() {
  if (titleDyn) {
    freeNull((void **) &titleDyn);
  }
}

void ButtonWidget::setText(const char* str) {
  freeNull((void **) &titleDyn);
  titleDyn = extStrdup(str);
  updated = true;
}

void ButtonWidget::setColors(colorType fg, colorType bg, colorType border, colorType sel, colorType selBg) {
  textColor = fg;
  bgColor = bg;
  borderColor = border;
  selTextColor = sel;
  selBgColor = selBg;
  updated = true;
}

int32_t ButtonWidget::textWidth(const char* str) {
  return fonts[OPENSANS_COND_BOLD_20]->textWidth(str);
}

bool ButtonWidget::processEvent(EventType event) {
  if (LOGIC_BUTTON_OK(event)) {
    pressed = true;
    return true;
  }
  return false;
}

bool ButtonWidget::readPressed() {
  bool res = pressed;
  pressed = false;
  return res;
}

void ButtonWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  if (windowWidth != widgetWidth || windowHeight != widgetHeight) {
    return;  // this widget can only be rendered in full
  }

  // Draw border
  lcd.drawRect(screenOffX, screenOffY, windowWidth, windowHeight, borderColor);

  // Draw background
  lcd.fillRect(screenOffX+1, screenOffY+1, windowWidth-2, widgetHeight-2, focused ? selBgColor : bgColor);

  // Draw text
  if (!focused) {
    lcd.setTextColor(textColor, bgColor);
  } else {
    lcd.setTextColor(selTextColor, selBgColor);
  }
  lcd.setTextFont(fonts[OPENSANS_COND_BOLD_20]);
  lcd.setTextDatum(MC_DATUM);
  lcd.drawString(titleDyn, screenOffX + windowWidth/2, screenOffY + windowHeight/2);
  updated = false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Slider widget  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

SliderWidget::SliderWidget(uint16_t xPos, uint16_t yPos,
                           uint16_t width, uint16_t height,
                           colorType col, colorType selCol, colorType bgCol, colorType textCol)
  : FocusableWidget(xPos, yPos, width, height),
    mainColor(col), selectedColor(selCol), bgColor(bgCol), textColor(textCol) {
}

void SliderWidget::drawSlider(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight, colorType color, float pos) {
  //lcd.drawRect(screenOffX, screenOffY, windowWidth, windowHeight, TFT_BLACK);     // debug

  // Draw horizontal rounded rect
  const uint16_t lineRadius = lineHeight >= 3 ? lineHeight/2 : 1;
  const uint16_t mid = screenOffY + windowHeight/2;
  const uint16_t lineStartX = screenOffX + dotRadius;
  const uint16_t lineStartY = mid - lineHeight/2;
  const uint16_t lineEndX = screenOffX + windowWidth - dotRadius - 1;
  if (lineEndX > dotRadius*2) {
    lcd.fillRoundRect(lineStartX, lineStartY, lineEndX-lineStartX, lineHeight, lineRadius, color);
  }

  // Draw circle
  const uint16_t posX = lineStartX + ((lineEndX > lineStartX) ? (int)((lineEndX-lineStartX)*pos) : 0);
  log_d("%.2f%% -> %d, max = %d", pos*100, ((lineEndX > lineStartX) ? (int)((lineEndX-lineStartX)*pos) : 0),  lineEndX-lineStartX);
  lcd.fillCircle(posX, mid, dotRadius, color);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - -  Integer slider widget  - - - - - - - - - - - - - - - - - - - - - - - - - - - -

IntegerSliderWidget::IntegerSliderWidget(uint16_t posX, uint16_t posY,
    uint16_t width, uint16_t height,
    int minValue, int maxValue, int step, bool showText, const char* unit,
    colorType color, colorType selectedColor, colorType bgColor, colorType textColor)
  : SliderWidget(posX, posY, width, height, color, selectedColor, bgColor, textColor), minVal(minValue), maxVal(maxValue), val(minValue), step(step), unit(unit) {
  log_d("creating IntegerSliderWidget");
  if (showText) {
    log_d("showText");
    bool minus = minValue < 0;
    uint8_t minExp, maxExp;
    for (minExp=1; minValue/=10; minExp++);
    for (maxExp=1; maxValue/=10; maxExp++);
    if (maxExp<minExp) {
      maxExp = minExp;
    }
    int unitLen = unit ? strlen(unit) : 0;
    char tmp[12+1+unitLen];
    memset(tmp, '6', sizeof(tmp));      // NOTE: we are assuming that "6" is the fattest digit in the font, which might not be true
    if (minus) {
      tmp[0] = '-';
      maxExp++;
    }
    if (unitLen) {
      sprintf(tmp + maxExp, " %s", unit);
    } else {
      tmp[maxExp] = '\0';
    }
    maxTextWidth = fonts[smoothFont]->textWidth(tmp);
    log_d("maxTextWidth = %d, maxExp = %d\n", maxTextWidth, maxExp);
  } else {
    maxTextWidth = 0;
  }
}

IntegerSliderWidget::~IntegerSliderWidget() {

}

bool IntegerSliderWidget::processEvent(EventType event) {
  if (event == WIPHONE_KEY_LEFT) {
    this->setValue(this->val - step);
    return true;
  } else if (event == WIPHONE_KEY_RIGHT) {
    this->setValue(this->val + step);
    return true;
  }
  return false;
}

void IntegerSliderWidget::redraw(LCD &lcd, uint16_t screenOffX, uint16_t screenOffY, uint16_t windowWidth, uint16_t windowHeight) {
  log_d("drawing integer slider");

  // Clear background
  lcd.fillRect(screenOffX, screenOffY, windowWidth, windowHeight, bgColor);

  // Draw slider
  const uint16_t offset = 4;
  const float pos = ((float)val - minVal)/(maxVal - minVal);
  const colorType color = this->focused ? selectedColor : mainColor;
  drawSlider(lcd, screenOffX + offset, screenOffY, windowWidth - 2*offset - this->maxTextWidth - offset, windowHeight, color, pos);

  // Draw text
  if (this->maxTextWidth) {
    char buff[12 + 1 + (unit!=NULL ? strlen(unit) : 0)];
    if (unit!=NULL) {
      sprintf(buff, "%d %s", this->val, unit);
    } else {
      sprintf(buff, "%d", this->val);
    }
    lcd.setTextColor(textColor, bgColor);
    lcd.setTextDatum(MC_DATUM);
    lcd.setTextFont(fonts[smoothFont]);
    lcd.drawFitString(buff, this->maxTextWidth+2*offset, screenOffX + windowWidth - offset - this->maxTextWidth/2, screenOffY + windowHeight/2);     //
    //lcd.drawRect(windowWidth - offset - this->maxTextWidth, screenOffY, this->maxTextWidth, windowHeight, TFT_BLACK);   // debug
  }
}

void IntegerSliderWidget::setValue(int value) {
  if (value > this->maxVal) {
    value = this->maxVal;
  } else if (value < this->minVal) {
    value = this->minVal;
  }
  if (this->val != value) {
    this->val = value;
    this->updated = true;
  }
}
