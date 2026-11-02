/* app_photos.cpp — see app_photos.h for the design and, in particular, for why the format list
 * stops where it does and why the default wallpaper is not a file. */
#include "app_photos.h"
#include "jpeg_grey.h"
#include <SD.h>
#include <SPIFFS.h>

/* The one global this app needs. Setting a wallpaper is not finished when the bytes are on
 * the card — the picture has to reach GUI's bgImage sprite, and only GUI can put it there.
 * Asking it also lets this app REPORT what happened instead of guessing. */
extern GUI gui;

static const int  PHOTOS_MAX_ENTRIES = 200;
static const char PHOTOS_DIR[]       = "/photos";
static const char PHOTOS_LOCKS[]     = "/photos/.locks";
/* Must match GUI::backgroundFile — that constant is what the loader in GUI.cpp actually reads,
 * and a second copy of the path here is how the two silently drift apart. */
#define PHOTOS_WALLPAPER   GUI::backgroundFile

/* Row keys. 1..entryCount are photos (index + 1); actions live above ROW_ACTION.
 * ⚠ NOT NEGATIVE, though negative reads more naturally: MenuOption::keyType is **uint32_t**.
 * Negative constants do survive the round trip through two's complement — a -1 stored as
 * 0xFFFFFFFF casts back to -1 — but only as long as every comparison remembers to cast, and
 * the day one forgets, the row silently matches nothing. High positive values cannot be got
 * wrong by omission. */
enum {
  /* 🛑 NOT 0, AND THAT IS THE WHOLE POINT. `ROW_INERT = 0` read perfectly and did nothing:
   * MenuWidget::addOption() REFUSES a key of 0 outright — it logs "menu option key is 0" and
   * adds no row at all. So every result message this app has ever produced was silently
   * dropped on the way to the screen: "Deleted", "Renamed to X", "Wallpaper set",
   * "Not set - <reason>", "X is locked - unlock it first", and the truncation warning below.
   * Proven on the handset 2026-08-25 — a successful delete redrew the list with no message,
   * and the log carried exactly one `[E] addOption(): menu option key is 0`.
   *
   * ⚠ THIS QUIETLY UNDID THE 0.9.13 WALLPAPER WORK. setAsWallpaper() was rewritten that
   * morning to stop claiming success and report what the loader actually said — and the line
   * it reports through is this one. It was only ever visible over the serial cable, which is
   * exactly why the gap survived being "verified".
   *
   * These two keys are high enough that nothing acts on them: the list handler matches
   * ROW_RESTORE_WALL or `k <= entryCount` (max 200), so OK on either row does nothing, which
   * is the behaviour "inert" was reaching for in the first place. */
  ROW_ACTION       = 10000,
  ROW_RESTORE_WALL = ROW_ACTION + 1,
  ROW_OPT_WALL     = ROW_ACTION + 10,
  ROW_OPT_RENAME   = ROW_ACTION + 11,
  ROW_OPT_LOCK     = ROW_ACTION + 12,
  ROW_OPT_DELETE   = ROW_ACTION + 13,
  ROW_OPT_CANCEL   = ROW_ACTION + 14,
  ROW_NOTE         = ROW_ACTION + 30,   // display-only rows; see the note above
  ROW_TRUNCATED    = ROW_ACTION + 31,
  ROW_DEL_YES      = ROW_ACTION + 20,
  ROW_DEL_NO       = ROW_ACTION + 21,
};

// ---------------------------------------------------------------- helpers

static bool hasExt(const char* name, const char* ext) {
  const size_t n = strlen(name), e = strlen(ext);
  return n > e && !strcasecmp(name + n - e, ext);
}

/* Is this a file we can actually put on the screen? Listing anything else would be offering
 * the user a file and then showing them a grey rectangle. */
static bool isViewable(const char* name) {
  return hasExt(name, ".jpg") || hasExt(name, ".jpeg") || hasExt(name, ".bmp");
}

/* Whole file into PSRAM. Photos are megabytes and internal RAM is what panics this phone, so
 * there is deliberately NO internal fallback: a photo is a luxury and must never compete with
 * SIP or WiFi for real memory. Caller frees. */
static uint8_t* slurp(const char* path, size_t* outLen, size_t cap) {
  *outLen = 0;
  File f = SD.open(path, FILE_READ);
  if (!f) {
    return NULL;
  }
  const size_t sz = (size_t)f.size();
  if (!sz || sz > cap) {
    f.close();
    return NULL;
  }
  uint8_t* buf = (uint8_t*)ps_malloc(sz);
  if (!buf) {
    f.close();
    return NULL;
  }
  const size_t got = f.read(buf, sz);
  f.close();
  if (got != sz) {
    free(buf);
    return NULL;
  }
  *outLen = sz;
  return buf;
}

/* 24/32-bit uncompressed BMP, nearest-neighbour scaled to fit the box. Written here rather
 * than pulled in, because the whole format is a 54-byte header and bottom-up rows — the cost
 * of supporting it is this function, and screenshots and simple exports tend to be BMP.
 * ⚠ BMP rows are padded to a 4-byte boundary and stored BOTTOM-UP unless the height is
 * negative. Both catch people out; both are handled. */
static bool drawBmpFitted(LCD& lcd, const uint8_t* d, size_t len,
                          int16_t bx, int16_t by, uint16_t bw, uint16_t bh) {
  if (len < 54 || d[0] != 'B' || d[1] != 'M') {
    return false;
  }
  const uint32_t off = (uint32_t)d[10] | ((uint32_t)d[11] << 8) | ((uint32_t)d[12] << 16) | ((uint32_t)d[13] << 24);
  const int32_t  w   = (int32_t)((uint32_t)d[18] | ((uint32_t)d[19] << 8) | ((uint32_t)d[20] << 16) | ((uint32_t)d[21] << 24));
  const int32_t  hRaw= (int32_t)((uint32_t)d[22] | ((uint32_t)d[23] << 8) | ((uint32_t)d[24] << 16) | ((uint32_t)d[25] << 24));
  const uint16_t bpp = (uint16_t)((uint16_t)d[28] | ((uint16_t)d[29] << 8));
  const uint32_t comp= (uint32_t)d[30] | ((uint32_t)d[31] << 8) | ((uint32_t)d[32] << 16) | ((uint32_t)d[33] << 24);
  if (w <= 0 || hRaw == 0 || comp != 0 || (bpp != 24 && bpp != 32)) {
    return false;                       // compressed or paletted: out of scope, say so honestly
  }
  const bool topDown = hRaw < 0;
  const int32_t h = topDown ? -hRaw : hRaw;
  const uint32_t bytesPP = bpp / 8;
  const uint32_t stride = ((uint32_t)w * bytesPP + 3u) & ~3u;
  if (off + stride * (uint32_t)h > len) {
    return false;
  }
  // Integer scale-down that fits the box; never scale up (a blown-up photo just looks broken).
  uint32_t den = 1;
  while ((uint32_t)w / den > bw || (uint32_t)h / den > bh) {
    den++;
  }
  const int outW = (int)((uint32_t)w / den), outH = (int)((uint32_t)h / den);
  const int ox = bx + ((int)bw - outW) / 2, oy = by + ((int)bh - outH) / 2;

  uint16_t* row = (uint16_t*)ps_malloc(sizeof(uint16_t) * outW);
  if (!row) {
    return false;
  }
  lcdNativePixels(lcd, true);                        // native color565 words: see GUI.h
  for (int y = 0; y < outH; y++) {
    const int32_t sy = (int32_t)y * (int32_t)den;
    const int32_t srcRow = topDown ? sy : (h - 1 - sy);
    const uint8_t* p = d + off + (uint32_t)srcRow * stride;
    for (int x = 0; x < outW; x++) {
      const uint8_t* q = p + (uint32_t)x * den * bytesPP;
      row[x] = lcd.color565(q[2], q[1], q[0]);        // BMP is BGR
    }
    lcd.pushImage(ox, oy + y, outW, 1, row);
  }
  lcdNativePixels(lcd, false);
  free(row);
  return true;
}

/* jpeg_grey streams eight rows at a time instead of handing back a framebuffer, so this blits
 * each band as it arrives. Same shape the e-reader's cover loader uses. */
struct GreyBlit {
  LCD* lcd;
  int  x, y;
};
static void greyRowsToScreen(void* ctx, int y0, int rows, const uint8_t* band, int width) {
  GreyBlit* g = (GreyBlit*)ctx;
  uint16_t* line = (uint16_t*)ps_malloc(sizeof(uint16_t) * width);
  if (!line) {
    return;
  }
  lcdNativePixels(*g->lcd, true);                    // native color565 words: see GUI.h
  for (int r = 0; r < rows; r++) {
    const uint8_t* src = band + (size_t)r * width;
    for (int x = 0; x < width; x++) {
      line[x] = g->lcd->color565(src[x], src[x], src[x]);
    }
    g->lcd->pushImage(g->x, g->y + y0 + r, width, 1, line);
  }
  lcdNativePixels(*g->lcd, false);
  free(line);
}

// ---------------------------------------------------------------- lifecycle

PhotosApp::PhotosApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(disp, state, header, footer) {
  log_d("create PhotosApp");
  note[0] = '\0';
  entries = (PEntry*)ps_malloc(sizeof(PEntry) * PHOTOS_MAX_ENTRIES);
  if (!entries) {
    log_e("PHOTOS: no PSRAM for the entry table");
  }
  if (!SD.exists(PHOTOS_DIR)) {
    SD.mkdir(PHOTOS_DIR);        // first run: give the folder a body so the note can name it
  }
  scanDir();
  enterState(PHOTOS_LIST);
}

PhotosApp::~PhotosApp() {
  log_d("destroy PhotosApp");
  delete menu;
  delete textArea;
  free(entries);
}

// ---------------------------------------------------------------- the folder

void PhotosApp::scanDir() {
  entryCount = 0;
  truncated = false;
  if (!entries) {
    return;
  }
  File dir = SD.open(PHOTOS_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return;
  }
  File f;
  while ((f = dir.openNextFile())) {
    // ⚠ On this core File::name() returns the FULL path — basename it ourselves.
    const char* n = f.name();
    const char* base = strrchr(n, '/');
    base = base ? base + 1 : n;
    if (base[0] && base[0] != '.' && !f.isDirectory() && isViewable(base)) {
      if (entryCount >= PHOTOS_MAX_ENTRIES) {
        truncated = true;
        f.close();
        break;
      }
      PEntry& e = entries[entryCount++];
      snprintf(e.name, sizeof(e.name), "%s", base);
      e.size = (uint32_t)f.size();
      e.locked = false;
    }
    f.close();
  }
  dir.close();

  // Case-insensitive alphabetical: a stable order is what makes LEFT/RIGHT in the viewer mean
  // something. Insertion sort — the list is small and usually nearly sorted already.
  for (int i = 1; i < entryCount; i++) {
    PEntry key = entries[i];
    int j = i - 1;
    while (j >= 0 && strcasecmp(entries[j].name, key.name) > 0) {
      entries[j + 1] = entries[j];
      j--;
    }
    entries[j + 1] = key;
  }
  loadLocks();
  if (sel >= entryCount) {
    sel = entryCount ? entryCount - 1 : 0;
  }
}

/* ── LOCKS LIVE IN THEIR OWN SIDECAR FILE ────────────────────────────────────────────────
 * Same argument as the starred mesh nodes: the folder listing is machine data — rescan and it
 * comes back — but a lock is the user's stated intent about which photos must not be lost, and
 * nothing can rederive that. One name per line, rewritten only when a lock changes. */
void PhotosApp::loadLocks() {
  for (int i = 0; i < entryCount; i++) {
    entries[i].locked = false;
  }
  File f = SD.open(PHOTOS_LOCKS, FILE_READ);
  if (!f) {
    return;
  }
  char line[80];
  while (f.available()) {
    const size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    char* e = line + strlen(line);
    while (e > line && (e[-1] == '\r' || e[-1] == ' ')) {
      *--e = '\0';
    }
    if (!line[0]) {
      continue;
    }
    for (int i = 0; i < entryCount; i++) {
      if (!strcmp(entries[i].name, line)) {
        entries[i].locked = true;
        break;
      }
    }
  }
  f.close();
}

void PhotosApp::saveLocks() {
  SD.remove(PHOTOS_LOCKS);
  File f = SD.open(PHOTOS_LOCKS, FILE_WRITE);
  if (!f) {
    return;
  }
  for (int i = 0; i < entryCount; i++) {
    if (entries[i].locked) {
      f.print(entries[i].name);
      f.print('\n');
    }
  }
  f.close();
}

bool PhotosApp::isLocked(const char* name) const {
  for (int i = 0; i < entryCount; i++) {
    if (!strcmp(entries[i].name, name)) {
      return entries[i].locked;
    }
  }
  return false;
}

// ---------------------------------------------------------------- screens

void PhotosApp::enterState(PhotosState_t st) {
  appState = st;
  switch (st) {
  case PHOTOS_LIST:
    footer->setButtons("Select", "Back");
    buildList();
    break;
  case PHOTOS_VIEW:
    /* "Options" rather than "Select": on this screen OK does not choose anything, it opens the
     * actions. Naming the key after what it does is the cheapest documentation there is. */
    footer->setButtons("Options", "Back");
    snprintf(headerTitle, sizeof(headerTitle), "%d/%d  %s",
             sel + 1, entryCount, entryCount ? entries[sel].name : "");
    header->setTitle(headerTitle);
    break;
  case PHOTOS_OPTIONS:
    footer->setButtons("Select", "Back");
    buildOptions();
    break;
  case PHOTOS_CONFIRM_DEL:
    footer->setButtons("Select", "Back");
    buildConfirmDelete();
    break;
  case PHOTOS_RENAME:
    footer->setButtons("Save", "Back");
    buildRename();
    break;
  }
}

void PhotosApp::buildList() {
  delete menu;
  delete textArea;
  textArea = NULL;
  menu = new MenuWidget(0, header->height(), lcd.width(),
                        lcd.height() - header->height() - footer->height(),
                        "No photos in /photos", fonts[AKROBAT_BOLD_18], 9, 8);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);

  snprintf(headerTitle, sizeof(headerTitle), "Photos (%d)", entryCount);
  header->setTitle(headerTitle);

  if (note[0]) {
    menu->addOption(note, ROW_NOTE, 1);       // what the last action did; see ROW_NOTE
  }
  /* Offered ONLY when there is an override to remove. A button that says it will restore
   * something already in place is a button that teaches people their actions do nothing. */
  if (SD.exists(PHOTOS_WALLPAPER)) {
    menu->addOption("[ Restore default wallpaper ]", ROW_RESTORE_WALL, 1);
  }
  char label[96];
  for (int i = 0; i < entryCount; i++) {
    /* "*" for locked, ASCII on purpose: the Akrobat faces are generated bitmap glyphs and
     * nothing in this firmware renders a non-ASCII character — a padlock would be a box.
     * Same finding as the mesh receipts and the starred nodes. */
    const char* lockMark = entries[i].locked ? "* " : "";
    if (entries[i].size >= 10240) {
      snprintf(label, sizeof(label), "%s%s  %luK", lockMark, entries[i].name,
               (unsigned long)(entries[i].size / 1024));
    } else {
      snprintf(label, sizeof(label), "%s%s  %luB", lockMark, entries[i].name,
               (unsigned long)entries[i].size);
    }
    menu->addOption(label, (MenuOption::keyType)(i + 1), 1);
  }
  if (truncated) {
    menu->addOption("(more than 200 photos - list truncated)", ROW_TRUNCATED, 1);
  }
}

void PhotosApp::buildOptions() {
  delete menu;
  menu = new MenuWidget(0, header->height(), lcd.width(),
                        lcd.height() - header->height() - footer->height(),
                        "", fonts[AKROBAT_BOLD_18], 9, 8);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  snprintf(headerTitle, sizeof(headerTitle), "%s", entries[sel].name);
  header->setTitle(headerTitle);

  menu->addOption("Set as wallpaper", ROW_OPT_WALL, 1);
  menu->addOption(entries[sel].locked ? "Unlock" : "Lock (protect)", ROW_OPT_LOCK, 1);
  /* A locked photo still SHOWS rename and delete, greyed by wording rather than hidden. A
   * disappearing menu item reads as a bug; one that says why it will not act teaches the lock. */
  menu->addOption(entries[sel].locked ? "Rename (locked)" : "Rename", ROW_OPT_RENAME, 1);
  menu->addOption(entries[sel].locked ? "Delete (locked)" : "Delete", ROW_OPT_DELETE, 1);
  menu->addOption("Cancel", ROW_OPT_CANCEL, 1);
}

void PhotosApp::buildConfirmDelete() {
  delete menu;
  menu = new MenuWidget(0, header->height(), lcd.width(),
                        lcd.height() - header->height() - footer->height(),
                        "", fonts[AKROBAT_BOLD_18], 9, 8);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, RED);
  snprintf(headerTitle, sizeof(headerTitle), "Delete %s?", entries[sel].name);
  header->setTitle(headerTitle);
  /* Cancel FIRST and selected by default: the destructive row must never be the one a
   * mis-timed OK lands on. Same reasoning as the Reboot submenu on the main menu. */
  menu->addOption("Cancel", ROW_DEL_NO, 1);
  menu->addOption("Yes, delete permanently", ROW_DEL_YES, 1);
}

void PhotosApp::buildRename() {
  delete menu;
  menu = NULL;
  delete textArea;
  const int16_t padding = 4;
  textArea = new MultilineTextWidget(0, header->height(), lcd.width(),
                                     lcd.height() - header->height() - footer->height(),
                                     "New name", controlState, sizeof(entries[sel].name) - 1,
                                     fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum,
                                     padding, padding);
  textArea->setColors(WP_COLOR_1, WP_COLOR_0);
  textArea->setText(entries[sel].name);
  textArea->cursorToEnd();
  textArea->setFocus(true);
  controlState.setInputState(InputType::AlphaNum);
  snprintf(headerTitle, sizeof(headerTitle), "Rename");
  header->setTitle(headerTitle);
}

// ---------------------------------------------------------------- the viewer

void PhotosApp::step(int delta) {
  if (entryCount <= 0) {
    return;
  }
  /* Wraps. With no wrap the last photo has a dead key on it, and "why does right do nothing"
   * is a worse question than "it went back to the beginning". */
  sel = (sel + delta % entryCount + entryCount) % entryCount;
  enterState(PHOTOS_VIEW);
}

bool PhotosApp::drawCurrentPhoto() {
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const uint16_t boxW = lcd.width(), boxH = (uint16_t)(bottom - top);
  lcd.fillRect(0, top, lcd.width(), boxH, BLACK);
  if (entryCount <= 0) {
    return false;
  }
  char path[160];
  snprintf(path, sizeof(path), "%s/%s", PHOTOS_DIR, entries[sel].name);

  /* ⚠ AN EMPTY FILE IS NOT AN UNREADABLE ONE, and saying so matters because the usual cause
   * is an upload that died half way — /photos/20260823_093939.jpg on phone 1 is 0 bytes
   * (found 2026-08-25). "Could not read this file" sends the reader to look for a corrupt
   * card; "empty" tells them to upload it again. It is still LISTED and still deletable on
   * purpose: COVEY's gallery learned the hard way that the pictures you cannot open are
   * exactly the ones you then cannot get rid of. */
  if (entries[sel].size == 0) {
    lcd.setTextColor(RED, BLACK);
    lcd.setTextFont(fonts[AKROBAT_BOLD_18]);
    lcd.drawString("This file is empty (0 bytes)", 10, top + 10);
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString("the upload did not finish", 10, top + 34);
    return false;
  }

  size_t len = 0;
  uint8_t* data = slurp(path, &len, (size_t)2 * 1024 * 1024);
  if (!data) {
    lcd.setTextColor(RED, BLACK);
    lcd.setTextFont(fonts[AKROBAT_BOLD_18]);
    lcd.drawString("Could not read this file", 10, top + 10);
    return false;
  }

  bool ok = false;
  if (hasExt(entries[sel].name, ".bmp")) {
    ok = drawBmpFitted(lcd, data, len, 0, top, boxW, boxH);
  } else if (jpegGreyIsGreyBaseline(data, len)) {
    /* ⚠ Greyscale FIRST, before the ROM decoder gets a look: TJpgDec does not merely render
     * these badly, it refuses them outright. The e-reader found that the hard way. */
    GreyBlit g = { &lcd, 0, top };
    uint16_t dw = 0, dh = 0;
    ok = (jpegGreyDecode(data, len, greyRowsToScreen, &g, &dw, &dh) == JPEG_GREY_OK);
  } else {
    uint16_t ow = 0, oh = 0;
    ok = display::load_jpg_at(data, len, &lcd, 0, top, boxW, boxH, &ow, &oh) != 0;
  }
  free(data);

  if (!ok) {
    /* Say WHICH failure this is. "Cannot display" on a .png and on a corrupt .jpg mean very
     * different things to whoever is holding the phone. */
    lcd.setTextFont(fonts[AKROBAT_BOLD_18]);
    lcd.setTextColor(RED, BLACK);
    lcd.drawString("Cannot decode this image", 10, top + 10);
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString("baseline JPEG and BMP only", 10, top + 34);
  }
  return ok;
}

// ---------------------------------------------------------------- actions

/* ── THE WALLPAPER COPY, AS A FREE FUNCTION, ON PURPOSE ───────────────────────────────────
 *
 * PhotosApp::setAsWallpaper() is four lines of glue over this. It is split out because this
 * app CANNOT BE TESTED WITHOUT A THUMB — every screen in it needs a key press and a serial
 * cable cannot press keys, which is exactly how "Set as wallpaper" reached {user}'s hands
 * untried. `wallpaper set <name>` on the console now runs THIS function, so the whole path
 * (extension gate, size gate, copy, loader verify, rollback) is exercised over the cable and
 * it is the SAME code the menu runs. A test hook that re-implements the feature proves
 * nothing; this one cannot drift.
 *
 * `why` always comes back with a sentence in it, pass or fail — the caller shows it verbatim.
 */
bool photosSetWallpaper(const char* photoName, char* why, size_t whyLen) {
  /* ⚠ REFUSED FOR BMP, and refused LOUDLY rather than accepted and then silently ignored.
   * The wallpaper is loaded by GUI.cpp's own drawImage(), which sniffs RLE3 / I256 / JPEG —
   * it has never had a BMP path. Copying a .bmp to /background.jpg would "succeed" here and
   * then fall back to the default at the next boot, which looks exactly like a bug. */
  if (!hasExt(photoName, ".jpg") && !hasExt(photoName, ".jpeg")) {
    snprintf(why, whyLen, "must be a JPEG (%s is not)", photoName);
    return false;
  }
  char src[160];
  snprintf(src, sizeof(src), "%s/%s", PHOTOS_DIR, photoName);
  File in = SD.open(src, FILE_READ);
  if (!in) {
    snprintf(why, whyLen, "could not read %s", photoName);
    return false;
  }
  /* Refuse an over-limit photo BEFORE copying megabytes onto the card to no purpose. The
   * viewer opens up to 2 MB and GUI::backgroundFileMaxSize is the same 2 MB, so this only
   * fires on a photo too big for either — but the two limits were once different, and a
   * picture that viewed and then vanished as a wallpaper is precisely that mismatch. */
  if ((size_t) in.size() > (size_t) GUI::backgroundFileMaxSize) {
    snprintf(why, whyLen, "too big: %u KB, limit %u KB",
             (unsigned)((size_t) in.size() >> 10), (unsigned)(GUI::backgroundFileMaxSize >> 10));
    in.close();
    return false;
  }
  /* COPIED, not pointed at. If the wallpaper were a reference to the photo, deleting the photo
   * would silently take the wallpaper with it — and "delete a picture, lose your background"
   * is precisely the sort of coupling nobody expects. */
  SD.remove(PHOTOS_WALLPAPER);
  File out = SD.open(PHOTOS_WALLPAPER, FILE_WRITE);
  if (!out) {
    in.close();
    snprintf(why, whyLen, "could not open %s for writing", PHOTOS_WALLPAPER);
    return false;
  }
  uint8_t buf[512];
  size_t n;
  bool ok = true;
  size_t written = 0;
  while ((n = in.read(buf, sizeof(buf))) > 0) {
    if (out.write(buf, n) != n) {
      ok = false;
      break;
    }
    written += n;
  }
  in.close();
  out.close();
  if (!ok) {
    SD.remove(PHOTOS_WALLPAPER);       // never leave a half-written wallpaper behind
    snprintf(why, whyLen, "write failed after %u bytes - default kept", (unsigned)written);
    return false;
  }
  /* ⚠ VERIFY, DO NOT ASSUME — and this is the whole lesson of 2026-08-25. Copying the bytes
   * is the easy half; the LOADER can still refuse them. TJpgDec rejects progressive and
   * GREYSCALE JPEGs outright, and a greyscale photo views perfectly on the screen before
   * this one because jpeg_grey.cpp decodes it and the wallpaper path does not. Announcing
   * "Wallpaper set" over a file the loader will drop is exactly the silent lie this change
   * exists to remove. So ask the loader, now, and repeat what it says.
   *
   * It also means the wallpaper appears AT ONCE. The old note said "restart to see it",
   * which was true and which also made a broken feature indistinguishable from a working
   * one until the next boot. */
  if (!gui.loadWallpaper()) {
    char loaderSaid[112];
    snprintf(loaderSaid, sizeof(loaderSaid), "%s", gui.getWallpaperNote());
    SD.remove(PHOTOS_WALLPAPER);
    gui.loadWallpaper();               // back to the default; never leave a rejected override
    snprintf(why, whyLen, "%s", loaderSaid);
    return false;
  }
  snprintf(why, whyLen, "%s", gui.getWallpaperNote());
  return true;
}

bool PhotosApp::setAsWallpaper() {
  char why[112];
  if (!photosSetWallpaper(entries[sel].name, why, sizeof(why))) {
    snprintf(note, sizeof(note), "Not set - %s", why);
    return false;
  }
  snprintf(note, sizeof(note), "Wallpaper set");
  return true;
}

bool PhotosApp::restoreDefaultWallpaper() {
  /* Nothing is copied INTO place, and that is the point. GUI's loader falls back to the
   * compiled-in image when there is no override, so removing the override IS the restore —
   * there is no operation here that can leave the phone without a wallpaper. */
  if (!SD.exists(PHOTOS_WALLPAPER)) {
    snprintf(note, sizeof(note), "Already using the default wallpaper");
    return true;
  }
  if (SD.remove(PHOTOS_WALLPAPER)) {
    gui.loadWallpaper();               // the fallback is compiled in, so this cannot fail
    snprintf(note, sizeof(note), "Default wallpaper restored");
    return true;
  }
  snprintf(note, sizeof(note), "Could not remove the wallpaper override");
  return false;
}

bool PhotosApp::doRename(const char* newName) {
  if (entries[sel].locked) {
    snprintf(note, sizeof(note), "%s is locked - unlock it first", entries[sel].name);
    return false;
  }
  char clean[64];
  snprintf(clean, sizeof(clean), "%s", newName ? newName : "");
  // Trim, and refuse anything that would climb out of the folder.
  char* e = clean + strlen(clean);
  while (e > clean && e[-1] == ' ') {
    *--e = '\0';
  }
  if (!clean[0] || strchr(clean, '/') || !strcmp(clean, ".") || !strcmp(clean, "..")) {
    snprintf(note, sizeof(note), "That is not a usable name");
    return false;
  }
  if (!isViewable(clean)) {
    /* The extension is what the viewer dispatches on, so renaming a .jpg to "holiday" would
     * hide it from this app entirely — the file would still be there and appear to be gone. */
    snprintf(note, sizeof(note), "Keep a .jpg/.jpeg/.bmp ending");
    return false;
  }
  char src[160], dst[160];
  snprintf(src, sizeof(src), "%s/%s", PHOTOS_DIR, entries[sel].name);
  snprintf(dst, sizeof(dst), "%s/%s", PHOTOS_DIR, clean);
  if (!strcmp(src, dst)) {
    return true;                                   // renamed to itself: quietly fine
  }
  if (SD.exists(dst)) {
    snprintf(note, sizeof(note), "%s already exists", clean);
    return false;
  }
  if (!SD.rename(src, dst)) {
    snprintf(note, sizeof(note), "Rename failed");
    return false;
  }
  snprintf(note, sizeof(note), "Renamed to %s", clean);
  return true;
}

bool PhotosApp::doDelete() {
  if (entries[sel].locked) {
    snprintf(note, sizeof(note), "%s is locked - unlock it first", entries[sel].name);
    return false;
  }
  char path[160];
  snprintf(path, sizeof(path), "%s/%s", PHOTOS_DIR, entries[sel].name);
  /* ⚠ A photo that is currently the wallpaper may still be deleted, and that is safe BECAUSE
   * the wallpaper was copied rather than referenced — /background.jpg is its own file. */
  if (SD.remove(path)) {
    snprintf(note, sizeof(note), "Deleted");
    return true;
  }
  snprintf(note, sizeof(note), "Delete failed");
  return false;
}

void PhotosApp::toggleLock() {
  entries[sel].locked = !entries[sel].locked;
  saveLocks();
  snprintf(note, sizeof(note), entries[sel].locked ? "%s locked" : "%s unlocked",
           entries[sel].name);
}

// ---------------------------------------------------------------- events

appEventResult PhotosApp::processEvent(EventType event) {
  switch (appState) {

  case PHOTOS_LIST:
    if (LOGIC_BUTTON_BACK(event)) {
      return EXIT_APP;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType k = menu->currentKey();
      if (k == ROW_RESTORE_WALL) {
        restoreDefaultWallpaper();
        buildList();
        return REDRAW_SCREEN;
      }
      if (k > 0 && k <= (MenuOption::keyType)entryCount) {
        note[0] = '\0';
        sel = (int)k - 1;
        enterState(PHOTOS_VIEW);
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case PHOTOS_VIEW:
    if (LOGIC_BUTTON_BACK(event)) {
      note[0] = '\0';
      enterState(PHOTOS_LIST);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_RIGHT || event == WIPHONE_KEY_DOWN) {
      step(1);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_LEFT || event == WIPHONE_KEY_UP) {
      step(-1);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event) && entryCount > 0) {
      enterState(PHOTOS_OPTIONS);
      return REDRAW_ALL;
    }
    return DO_NOTHING;

  case PHOTOS_OPTIONS:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(PHOTOS_VIEW);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      switch (menu->currentKey()) {
      case ROW_OPT_WALL:
        setAsWallpaper();
        enterState(PHOTOS_LIST);
        return REDRAW_ALL;
      case ROW_OPT_LOCK:
        toggleLock();
        enterState(PHOTOS_OPTIONS);       // stay put: the row's own label just changed
        return REDRAW_ALL;
      case ROW_OPT_RENAME:
        if (entries[sel].locked) {
          snprintf(note, sizeof(note), "%s is locked - unlock it first", entries[sel].name);
          enterState(PHOTOS_LIST);
          return REDRAW_ALL;
        }
        enterState(PHOTOS_RENAME);
        return REDRAW_ALL;
      case ROW_OPT_DELETE:
        if (entries[sel].locked) {
          snprintf(note, sizeof(note), "%s is locked - unlock it first", entries[sel].name);
          enterState(PHOTOS_LIST);
          return REDRAW_ALL;
        }
        enterState(PHOTOS_CONFIRM_DEL);
        return REDRAW_ALL;
      case ROW_OPT_CANCEL:
      default:
        enterState(PHOTOS_VIEW);
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case PHOTOS_CONFIRM_DEL:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(PHOTOS_OPTIONS);
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      if (menu->currentKey() == ROW_DEL_YES) {
        doDelete();
        scanDir();
      }
      enterState(PHOTOS_LIST);
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;

  case PHOTOS_RENAME:
    /* 🛑 CANCEL IS END, NOT BACK — BECAUSE BACK IS BACKSPACE IN A TEXT FIELD.
     *
     * This used to test LOGIC_BUTTON_BACK(), which on the 21-button keyboard is
     * `BACK || END` (Hardware.h:213). Testing it here consumed Back BEFORE the event could
     * reach textArea, so **the rename field had no way to delete a character**. It opens
     * prefilled with the current filename, so the only possible edit was to APPEND: you
     * could turn BT06.JPG into BT06.JPGxyz and nothing else. Renaming a photo was, in
     * practice, impossible.
     *
     * ⚠ Nobody had ever driven this screen — it is the last path in this app that a thumb
     * (or `key`/`shot`) had not reached, and it shipped like this on 2026-08-24.
     * MEASURED on the handset 2026-08-26: four Back presses walked four screens OUT of the
     * app instead of deleting four characters, and `key 7`/`key 2 2` typed 'p' and 'b'
     * perfectly well — so the field worked, and only the erase was missing.
     *
     * Every other text screen in this firmware already had it right and said so:
     * app_books.cpp:1944, and app_meshtastic.cpp:1372/:1396/:1419 with the convention
     * written out at app_meshtastic.cpp:198. Photos was the single exception. This matches
     * them rather than inventing a fourth spelling — the rule from Hardware.h:216 is that
     * every consumer declares what IT needs. */
    if (event == WIPHONE_KEY_END) {        // cancel (Back is backspace in a text field)
      controlState.setInputState(InputType::Numeric);
      enterState(PHOTOS_OPTIONS);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event)) {
      doRename(textArea ? textArea->getText() : NULL);
      controlState.setInputState(InputType::Numeric);
      scanDir();
      enterState(PHOTOS_LIST);
      return REDRAW_ALL;
    }
    if (textArea) {
      textArea->processEvent(event);
    }
    return REDRAW_SCREEN;
  }
  return DO_NOTHING;
}

void PhotosApp::redrawScreen(bool redrawAll) {
  if (appState == PHOTOS_VIEW) {
    drawCurrentPhoto();
    return;
  }
  if (appState == PHOTOS_RENAME) {
    if (textArea) {
      ((GUIWidget*)textArea)->redraw(lcd);
    }
    return;
  }
  if (menu) {
    ((GUIWidget*)menu)->redraw(lcd);
  }
}
