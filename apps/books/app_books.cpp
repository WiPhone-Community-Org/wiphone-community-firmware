/*
 * app_books.cpp — the WiPhone e-reader (see app_books.h).
 *
 * Structure mirrors app_gbc's picker: a list with action rows on top, and screens hanging off
 * it. The reading screen draws itself rather than using a widget, because a page of prose is
 * not a list and MultilineTextWidget is an editor.
 */

#include "app_books.h"
#include "app_gbc_xfer.h"
#include "kosync_sync.h"          // KOSync: the window, the home client, /books/kosync.txt
#include "meshtastic_service.h"
#include "clock.h"
#include "Arduino.h"
#include "SD.h"
#include <Preferences.h>
#include <strings.h>              // strcasecmp, for finding the channel by name

// Library-list rows that come before the books themselves. Keys are 1-based below these.
#define BOOKS_ROW_ADD      1
#define BOOKS_ROW_MANAGE   2
#define BOOKS_ROW_HELP     3
#define BOOKS_ROW_TIDY     4         // "Move N books into /books"
#define BOOKS_ROW_FIRST    10        // book i has key BOOKS_ROW_FIRST + i

// Reader menu rows
#define BOOKS_MENU_RESUME  1
#define BOOKS_MENU_TOC     2
#define BOOKS_MENU_SIZE    3
#define BOOKS_MENU_INFO    4
#define BOOKS_MENU_CLOSE   5
#define BOOKS_MENU_SYNC    6
#define BOOKS_MENU_PENDING 7
#define BOOKS_MENU_SYNCSET 8
#define BOOKS_MENU_UNDO    9

// Sync settings rows
#define BOOKS_SET_PASS     1
#define BOOKS_SET_DEV      2

#define BOOKS_MARGIN       6         // left/right page margin, pixels
/* Page turns between SD writes of the position. Every turn would mean a 16 KB whole-file
 * write per page; never would mean a flat battery costs your place. Four is roughly a minute
 * of reading, and the position is also flushed on the menu, a chapter change and closing. */
#define BOOKS_SAVE_EVERY   4

/* How long "Go there" stays inert after the sync card appears.
 *
 * The card can replace the page from two directions — opening a book that has a position
 * parked, and the one-second tick while you are reading — and in BOTH the user's next
 * keystroke was aimed at something else. 600 ms is past a reflex double-press and well short
 * of reading two lines of text, so a deliberate press never notices it. "Stay" is not
 * delayed: declining is safe, and making the safe answer feel broken teaches the wrong habit. */
#define BOOKS_SYNCCARD_ARM_MS  600

/* The Reading menu's tick while "Sync my place"'s LoRa frame waits for the radio (armMenuTick):
 * "Queued" becomes "Sent" or "NOT sent" at most this long after the radio answers, and the tick
 * stands down as soon as it has — a second or two, once per press. */
#define BOOKS_SYNC_WATCH_MS    500

// How long "Undo the jump" stays on offer. See the note in app_books.h.
#define BOOKS_UNDO_TTL_MS      (5UL * 60UL * 1000UL)

// Body faces, smallest first. All the phone has are bold faces (see the font note in the
// COVEY/WiPhone handoffs) — this is a size choice, not a weight one.
static const uint8_t BODY_FONTS[] = { AKROBAT_BOLD_16, AKROBAT_BOLD_18, OPENSANS_COND_BOLD_20 };
#define BODY_FONT_COUNT ((int)(sizeof(BODY_FONTS) / sizeof(BODY_FONTS[0])))

static const XferConfig BOOKS_XFER_CFG = {
  BOOKS_DIR, "Add books", ".epub,.txt", "books", "download.epub", "WiPhone-Books"
};

// ---------------------------------------------------------------- SD plumbing

// Random access into the open book file, for epub_parse.
static size_t sdSourceRead(void* ctx, uint64_t off, void* buf, size_t len) {
  File* f = (File*)ctx;
  if (!f || !*f) {
    return 0;
  }
  if (!f->seek((uint32_t)off)) {
    return 0;
  }
  int n = f->read((uint8_t*)buf, len);
  return n > 0 ? (size_t)n : 0;
}

/* Read one candidate. Returns bytes read, or -1 if it could not be opened/read at all. */
static int posReadFile(const char* path, char* buf, size_t cap) {
  File f = SD.open(path, FILE_READ);
  if (!f) {
    return -1;
  }
  int n = f.read((uint8_t*)buf, cap - 1);
  f.close();
  if (n < 0) {
    return -1;
  }
  buf[n] = '\0';
  return n;
}

static bool posLoad(void* ctx, char* buf, size_t cap, size_t* outLen) {
  /* ⚠ FALL BACK ON AN EMPTY FILE TOO, not only on one that will not open. A write that died
   * mid-swap leaves the previous good copy in .bak, but it can just as easily leave a real
   * file of zero length — and the first version of this only tried .bak when SD.open FAILED,
   * so an empty real file shadowed a perfectly good backup and every book read as unsaved.
   * The magic line alone is 9 bytes, so anything shorter cannot be a store. */
  int n = posReadFile(BOOKS_POS_FILE, buf, cap);
  if (n < (int)sizeof(BOOKSTORE_MAGIC)) {
    const int b = posReadFile(BOOKS_POS_FILE ".bak", buf, cap);
    if (b > n) {
      n = b;
    } else if (n < 0) {
      return false;                     // no file yet is a fresh store, not an error
    }
  }
  if (n < 0) {
    return false;
  }
  *outLen = (size_t)n;
  return true;
}

/* Whole file, temp-then-rename. The phone can lose power at any instant — the rail is held by
 * a software latch — so a torn write has to cost the last session's position rather than
 * every book's. */
static bool posStore(void* ctx, const char* buf, size_t len) {
  SD.mkdir(BOOKS_DIR);
  const char* tmp = BOOKS_POS_FILE ".tmp";
  SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) {
    return false;
  }
  size_t w = f.write((const uint8_t*)buf, len);
  f.flush();
  f.close();
  if (w != len) {
    SD.remove(tmp);
    return false;
  }
  /* 🛑 NEVER be without a good copy. This used to be `SD.remove(BOOKS_POS_FILE)` followed by
   * the rename below — so between those two calls no positions file existed at all, and a
   * failed rename (or the rail dropping, which on this phone is a software latch and can
   * happen at any instant) took EVERY book's place with it. Both this function's own comment
   * and bookstore.h promised the opposite: that a torn write costs the last session only.
   * Now the outgoing file is kept as .bak until the new one is in place, and posLoad falls
   * back to it, so there is always exactly one file a reader can come back to. */
  /* 🛑 ONLY VACATE .bak WHEN THERE IS A REAL FILE TO PUT THERE. The first cut of this removed
   * .bak unconditionally — which is fine every ordinary time, and catastrophic in the ONE
   * state this whole mechanism exists for: a store left .bak-only by a torn write has no real
   * file, so the remove deleted the sole readable copy and the rename below then had nothing
   * to move. The recovery path destroyed the thing it was recovering. */
  const bool haveReal = SD.exists(BOOKS_POS_FILE);
  if (haveReal) {
    SD.remove(BOOKS_POS_FILE ".bak");
    SD.rename(BOOKS_POS_FILE, BOOKS_POS_FILE ".bak");
  }
  if (!SD.rename(tmp, BOOKS_POS_FILE)) {
    if (haveReal) {
      SD.rename(BOOKS_POS_FILE ".bak", BOOKS_POS_FILE); // put the old one back if we can
    }
    SD.remove(tmp);
    return false;
  }
  SD.remove(BOOKS_POS_FILE ".bak");
  return true;
}

// ---------------------------------------------------------------- drawable text

/* Characters the phone's fonts do not have, mapped to something they do.
 *
 * A published EPUB is full of typographic punctuation and Akrobat has none of it. Its .notdef
 * glyph is a narrow vertical bar, so on the first real page ever shown on this phone
 * "Arc-Royal’s duke" read as "Arc-Royalls duke" — not obviously a missing glyph at all, which
 * is what makes it worth fixing rather than tolerating.
 *
 * ⚠ This is a RENDERING substitution and must never touch `chapText`. A reading position is a
 * byte offset into that buffer, and rewriting it would move every offset after the first curly
 * quote — silently, and further with every paragraph. */
struct GlyphSub { uint16_t cp; const char* ascii; };
static const GlyphSub GLYPH_SUBS[] = {
  { 0x2018, "'" },  { 0x2019, "'" },  { 0x201A, "," },  { 0x201B, "'" },
  { 0x201C, "\"" }, { 0x201D, "\"" }, { 0x201E, "\"" }, { 0x2032, "'" }, { 0x2033, "\"" },
  { 0x2010, "-" },  { 0x2011, "-" },  { 0x2012, "-" },  { 0x2013, "-" },
  { 0x2014, "-" },  { 0x2015, "-" },  { 0x2026, "..." },
  // Every space Python's str.split() treats as white space, so a line never breaks oddly.
  { 0x00A0, " " },  { 0x1680, " " },  { 0x2002, " " },  { 0x2003, " " }, { 0x2004, " " },
  { 0x2005, " " },  { 0x2006, " " },  { 0x2007, " " },  { 0x2008, " " }, { 0x2009, " " },
  { 0x200A, " " },  { 0x202F, " " },  { 0x205F, " " },  { 0x3000, " " },
  { 0x00AD, "" },   { 0x200B, "" },   { 0x200C, "" },   { 0x200D, "" },  { 0xFEFF, "" },
  { 0x2022, "*" },  { 0x00B7, "." },  { 0x2039, "<" },  { 0x203A, ">" },
  { 0x00AB, "\"" }, { 0x00BB, "\"" }, { 0x2122, "(TM)" }, { 0x00A9, "(c)" }, { 0x00AE, "(R)" },
  { 0x00E0, "a" },  { 0x00E1, "a" },  { 0x00E2, "a" },  { 0x00E4, "a" }, { 0x00E5, "a" },
  { 0x00E8, "e" },  { 0x00E9, "e" },  { 0x00EA, "e" },  { 0x00EB, "e" },
  { 0x00EC, "i" },  { 0x00ED, "i" },  { 0x00EE, "i" },  { 0x00EF, "i" },
  { 0x00F2, "o" },  { 0x00F3, "o" },  { 0x00F4, "o" },  { 0x00F6, "o" }, { 0x00F8, "o" },
  { 0x00F9, "u" },  { 0x00FA, "u" },  { 0x00FB, "u" },  { 0x00FC, "u" },
  { 0x00F1, "n" },  { 0x00E7, "c" },  { 0x00DF, "ss" },
  { 0x00C0, "A" },  { 0x00C4, "A" },  { 0x00C8, "E" },  { 0x00C9, "E" },
  { 0x00D6, "O" },  { 0x00DC, "U" },  { 0x00D1, "N" },  { 0x00C7, "C" },
};
#define GLYPH_SUB_COUNT ((int)(sizeof(GLYPH_SUBS) / sizeof(GLYPH_SUBS[0])))

/* Copy a UTF-8 run, replacing anything `f` cannot draw. The font is ASKED rather than assumed,
 * so a face that does have curly quotes keeps them. Used for BOTH measuring and drawing —
 * measure one string and draw another and the line breaks drift. */
/* Non-static: shared with FilesApp's text viewer (see app_books.h). */
size_t bookRenderRun(SmoothFont* f, const char* s, size_t len, char* out, size_t cap) {
  size_t o = 0, i = 0;
  while (i < len && o + 5 < cap) {
    unsigned char c = (unsigned char)s[i];
    size_t clen = 1;
    uint32_t cp = c;
    if ((c & 0xE0) == 0xC0) {
      clen = 2;
      cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      clen = 3;
      cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      clen = 4;
      cp = c & 0x07u;
    }
    if (i + clen > len) {
      clen = 1;
      cp = c;
    }
    for (size_t k = 1; k < clen; k++) {
      cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3Fu);
    }

    if (cp < 0x80) {
      out[o++] = (char)cp;
      i += clen;
      continue;
    }
    uint16_t gi = 0;
    if (cp <= 0xFFFF && f && f->getUnicodeIndex((uint16_t)cp, &gi)) {
      memcpy(out + o, s + i, clen);          // the font has it: leave it alone
      o += clen;
      i += clen;
      continue;
    }
    const char* rep = "?";                   // visible, rather than silently dropped
    for (int k = 0; k < GLYPH_SUB_COUNT; k++) {
      if (GLYPH_SUBS[k].cp == cp) {
        rep = GLYPH_SUBS[k].ascii;
        break;
      }
    }
    size_t rl = strlen(rep);
    if (o + rl >= cap) {
      break;
    }
    memcpy(out + o, rep, rl);
    o += rl;
    i += clen;
  }
  out[o] = '\0';
  return o;
}

/* Width of one run of UTF-8, for book_layout.
 *
 * ⚠ ADVANCE-CORRECTED, and the correction is the fix for "characters cut off at the
 * margins" ({user}'s photo, 2026-08-18; cornered by the `bookpage` probe). SmoothFont::
 * textWidth bills every glyph its full x-advance EXCEPT THE LAST, which gets only its ink
 * width (gdX+gWidth) — and book_layout calls this measure ONE CHARACTER AT A TIME, so
 * every character was "the last": the summed widths under-counted ~0.4 px per glyph. Over
 * a 40-character line that is ~15 px, the layouter packed one or two characters too many
 * into the 228 px budget (bookpage measured finished lines at 236-245 px), and the draw
 * clamped the overflowing line leftward — beheading its first character on screen while
 * the layout, measured on the host, was provably lossless.
 *
 * The sentinel space makes every real glyph non-final (billed its advance), then the
 * space's own width is subtracted. This errs a hair WIDE on a line's true final glyph,
 * which can only break a line one character early — never clip. */
int fontMeasure(void* ctx, const char* s, size_t len) {
  SmoothFont* f = (SmoothFont*)ctx;
  char tmp[96];
  size_t l = bookRenderRun(f, s, len, tmp, sizeof(tmp));   // measure what will be drawn
  if (l + 1 < sizeof(tmp)) {
    tmp[l] = ' ';
    tmp[l + 1] = '\0';
    return (int)(f->textWidth(tmp) - f->textWidth(" "));
  }
  return (int)f->textWidth(tmp);
}

/* Heap probe — uncomment to bring it back.
 *
 * `log_e` is the only level compiled into this firmware, so a diagnostic has to shout. This
 * one earned its place: the reader was rebooting the phone a minute or two after a book was
 * opened, and the visible symptom was `phy_init: failed to allocate memory for RF calibration
 * data` — the WiFi PHY, not the reader, and nothing in the backtrace pointed here. What it
 * showed was that the INTERNAL heap runs at about 16 KB free on this phone with WiFi and SIP
 * up, and that an 8 KB array inside `new BooksApp` took the margin below what a periodic
 * WiFi re-scan needs. PSRAM was never the constraint: 3.6 MB free throughout.
 *
 * Measure before believing any theory about memory here, and watch `largest` as much as
 * `heap` — the failure was a fragmentation one as much as a volume one. */
//#define BOOKS_HEAP_DEBUG

#ifdef BOOKS_HEAP_DEBUG
#define BOOKS_HEAP(tag)                                                            \
  log_e("books/%s: heap=%u min=%u largest=%u psram=%u", (tag),                     \
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),               \
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),           \
        (unsigned)ESP.getFreePsram())
#else
#define BOOKS_HEAP(tag)  do { } while (0)
#endif

// ---------------------------------------------------------------- lifecycle

/* The live instance, for the serial `bookpage` probe. One reader exists at a time (apps are
 * created on entry and deleted on exit), so a plain pointer is enough. */
static BooksApp* s_liveBooksApp = NULL;

BooksApp::BooksApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(disp, state, header, footer) {
  log_d("create BooksApp");
  s_liveBooksApp = this;
  appState = BOOKS_LIB;
  menu = NULL;
  textArea = NULL;
  editingPass = false;
  bookCount = 0;
  pendingDelete = -1;
  xferClean = false;
  helpTop = 0;
  libNote[0] = '\0';
  headerTitle[0] = '\0';

  isOpen = false;
  chapText = NULL;
  chapLen = 0;
  chapCap = 0;
  spine = 0;
  pageStart = 0;
  histN = 0;
  openIdx = -1;
  fontIdx = 0;
  turnsSinceSave = 0;
  nIds = 0;
  nImages = 0;
  viewImage = -1;
  pendingIdx = -1;
  pendingId = 0;
  pendingFrom = 0;
  pendingClock = false;
  syncNote[0] = '\0';
  syncTxId = 0;
  syncTxCh = syncTxPct = 0;
  ksPartial[0] = '\0';
  ksByName[0] = '\0';
  ksIdsDone = false;
  ksNote[0] = '\0';
  ksLines[0] = '\0';
  ksBook = NULL;
  ksMovedSpine = -1;
  ksMovedOff = 0;
  syncCardMs = 0;
  undoValid = false;
  undoMs = 0;
  undoSpine = 0;
  undoOffset = 0;
  undoPct = 0;
  saveFailed = false;
  memset(&pending, 0, sizeof(pending));
  memset(pageImageKey, 0, sizeof(pageImageKey));

  timeoutsHeld = false;   // the snapshot now lives in ControlState's counted holder

  storeIo.ctx = NULL;
  storeIo.load = posLoad;
  storeIo.store = posStore;

  /* Everything sizeable goes to PSRAM. `new BooksApp` itself is internal-heap memory, and on
   * this phone that heap sits at roughly 16 KB free — see the note in app_books.h. */
  books    = (BookFile*)ps_malloc(sizeof(BookFile) * BOOKS_MAX);
  images   = (EpubImage*)ps_malloc(sizeof(EpubImage) * EPUB_MAX_IMAGES);
  imgBoxes = (BookImageBox*)ps_malloc(sizeof(BookImageBox) * EPUB_MAX_IMAGES);
  ksBook   = (KosyncBook*)ps_malloc(sizeof(KosyncBook));   // ~4.5 KB; NULL = KOSync unavailable
  store    = (BookStore*)ps_malloc(sizeof(BookStore));
  if (store) {
    store->init();
    store->load(&storeIo);
  }

  loadSyncSettings();
  /* Re-read /books/kosync.txt on every entry to Books: it is how an edited file (or one just
   * put on the card over WiFi) takes effect without a reboot. A missing file is one failed
   * open — and means KOSync is simply off. */
  kosyncReloadConfig();
  /* 🛑 DO NOT call bookSyncInboxInit() here, and do not "tidy up" by adding it back.
   *
   * This app is `new`ed on entry to Books and `delete`d on exit (GUI.cpp, GUI_APP_BOOKS), so
   * an init in this constructor threw away every parked position at the exact moment the
   * reader was about to look for one — which defeated the entire point of the parking spot.
   * The symptom on hardware was that a position had to arrive with Books ALREADY OPEN or it
   * was never seen, so the only sequence that worked was: open the book here, press Sync
   * over there, then close and reopen the book. The inbox is a file-static array, zeroed by
   * the C runtime before setup() runs; it needs no initialisation and must outlive this app.
   */
  syncSeqSeen = bookSyncInboxSeq();     // what was already parked before Books was opened
  BOOKS_HEAP("ctor");
  scanBooks();
  BOOKS_HEAP("scanned");
  enterState(BOOKS_LIB);
}

/* ── THE APP OBJECT ITSELF GOES TO PSRAM ──────────────────────────────────────────────
 * MEASURED, not guessed. The app-open heap probe in GUI::enterApp() named this app on real
 * hardware, twice, on two separate runs that each pa{user}ed within a couple of minutes:
 *
 *   APP id=16398 (BOOKS)  largest 8,596 -> 5,732  (-2,864)   ... then PANIC at 3,084
 *   APP id=16398 (BOOKS)  largest 12,284 -> 9,216 (-3,068)
 *
 * Every other app measured +0. Books was the only one with a real cost.
 *
 * The reason is that ~3 KB of this class is fixed members, and `new` puts all of it on the
 * INTERNAL heap — this build has no malloc->PSRAM diversion at any size
 * (CONFIG_SPIRAM_USE_CAPS_ALLOC without CONFIG_SPIRAM_USE_MALLOC), so size alone never saves
 * you. `EpubBook book` alone is 1,584 bytes by value; add `BookSyncRecord pending` (~250),
 * ids[3][72] (216), hist[48] (192), the three sync strings (108), libNote+headerTitle (160)
 * and the base classes, and it lands almost exactly on the measured figures.
 *
 * ⚠ An earlier fix moved this app's ARRAYS to PSRAM (books/store/images/imgBoxes, 22,520
 * bytes) and stopped there, leaving the object itself internal. That is why the phone kept
 * pa{user}ing after opening a book even though the arrays were "already fixed".
 *
 * Overriding operator new for the class moves the entire object in one step, rather than
 * converting a dozen members to pointers and touching every use of them. PSRAM has ~3.6 MB
 * spare and this is a UI app: nothing here is DMA'd, touched from an ISR, or hot enough for
 * the slower access to matter.
 *
 * ⚠ Falls back to internal heap if PSRAM is exhausted — refusing to open Books would be worse
 * than opening it the old way. `free()` on ESP-IDF is region-agnostic, so one delete serves
 * both. */
void* BooksApp::operator new(size_t n) {
  void* p = ps_malloc(n);
  return p ? p : malloc(n);
}

void BooksApp::operator delete(void* p) {
  free(p);
}

BooksApp::~BooksApp() {
  log_d("destroy BooksApp");
  if (s_liveBooksApp == this) {
    s_liveBooksApp = NULL;
  }
  closeBookByUser();             // leaving the reader with a book open IS closing it (auto=on)
  /* 🛑 ONLY IF THE TRANSFER SCREEN WAS UP. This used to be unconditional, which was harmless
   * while the uploader was the only thing that module ran — and then a KOSync window opened
   * on book close died the moment Books was left. xferStop() itself now leaves a window's
   * transport standing too; both halves are deliberate, so a window survives either change
   * being undone alone. */
  if (appState == BOOKS_XFER) {
    xferStop();                  // in case the app dies with the transfer screen up
  }
  holdScreenAwake(false);        // never leave the user's screen timeout stretched
  free(store);
  free(books);
  free(images);
  free(imgBoxes);
  free(ksBook);
  ksBook = NULL;
  store = NULL;
  books = NULL;
  images = NULL;
  imgBoxes = NULL;
  freeWidgets();
}

void BooksApp::freeWidgets() {
  if (menu) {
    delete menu;
    menu = NULL;
  }
  if (textArea) {
    delete textArea;
    textArea = NULL;
  }
}

// One line of typing, for the passcode or this device's name.
void BooksApp::buildSyncEdit() {
  const int16_t pad = 4;
  textArea = new MultilineTextWidget(0, header->height(), lcd.width(),
                                     lcd.height() - header->height() - footer->height(),
                                     editingPass ? "Shared passcode" : "Name for this device",
                                     controlState,
                                     editingPass ? sizeof(syncPass) - 1 : sizeof(syncDev) - 1,
                                     fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum,
                                     pad, pad);
  textArea->setColors(WP_COLOR_1, WP_COLOR_0);
  textArea->setText(editingPass ? syncPass : syncDev);
  textArea->setFocus(true);
  controlState.setInputState(InputType::AlphaNum);
}

MenuWidget* BooksApp::newMenu(const char* emptyMessage, SmoothFont* font, uint8_t perScreen) {
  MenuWidget* m = new MenuWidget(0, header->height(), lcd.width(),
                                 lcd.height() - header->height() - footer->height(),
                                 emptyMessage, font, perScreen, 8);
  m->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  return m;
}

/* The phone dims at 20 s and sleeps at 30 s, which is shorter than a page takes to read. Hold
 * both open while a book is up and put the user's own values back afterwards. */
void BooksApp::holdScreenAwake(bool hold) {
  if (hold == timeoutsHeld) {
    return;                     // stay idempotent: one owner, at most one count
  }
  controlState.holdScreenAwake(hold);
  timeoutsHeld = hold;
}

// ---------------------------------------------------------------- the library

static bool hasExt(const char* name, const char* ext) {
  size_t n = strlen(name), e = strlen(ext);
  if (n <= e) {
    return false;
  }
  for (size_t i = 0; i < e; i++) {
    char c = name[n - e + i];
    if (c >= 'A' && c <= 'Z') {
      c += 32;
    }
    if (c != ext[i]) {
      return false;
    }
  }
  return true;
}

// Case-insensitive "a sorts after b". Local rather than strcasecmp, which lives in <strings.h>
// and is not something Arduino.h promises.
static bool nameAfter(const char* a, const char* b) {
  for (;; a++, b++) {
    char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
    if (ca != cb) {
      return ca > cb;
    }
    if (!ca) {
      return false;
    }
  }
}

/* Books are looked for in /books first, but also in /roms and the card root. That is not
 * tidiness lost — it is how a book that arrived through the Game Boy ROM uploader (the only
 * uploader that existed before this app) still shows up. "Move to /books" is in Manage. */
void BooksApp::scanBooks() {
  bookCount = 0;
  if (!books) {
    return;                     // no PSRAM for the library: the list stays empty, not wrong
  }
  const char* dirs[] = { BOOKS_DIR, "/roms", "/" };
  for (int d = 0; d < 3 && bookCount < BOOKS_MAX; d++) {
    File dir = SD.open(dirs[d]);
    if (!dir) {
      continue;
    }
    if (!dir.isDirectory()) {
      dir.close();
      continue;
    }
    File f;
    while (bookCount < BOOKS_MAX && (f = dir.openNextFile())) {
      if (!f.isDirectory()) {
        const char* nm = f.name();
        const char* slash = strrchr(nm, '/');
        const char* base = slash ? slash + 1 : nm;
        bool txt = hasExt(base, ".txt");
        /* kosync.txt / smsmirror.txt are SETTINGS, with a password in them — never books.
         * Listed, one opened like any .txt and put its secrets on the screen, and with auto=on
         * closing it opened a window and sent its "place" home. */
        if ((hasExt(base, ".epub") || txt) && base[0] != '.' && !(txt && kosyncNotABook(base))) {
          bool dup = false;
          for (int i = 0; i < bookCount; i++) {
            if (strcmp(books[i].name, base) == 0) {
              dup = true;               // same name in two folders: list it once
            }
          }
          if (!dup) {
            snprintf(books[bookCount].name, sizeof(books[0].name), "%s", base);
            snprintf(books[bookCount].path, sizeof(books[0].path), "%s", nm);
            books[bookCount].isTxt = txt;
            bookCount++;
          }
        }
      }
      f.close();
    }
    dir.close();
  }

  // Alphabetical, so the list does not reshuffle itself when a file is added.
  for (int i = 1; i < bookCount; i++) {
    BookFile key = books[i];
    int j = i - 1;
    while (j >= 0 && nameAfter(books[j].name, key.name)) {
      books[j + 1] = books[j];
      j--;
    }
    books[j + 1] = key;
  }
}

void BooksApp::buildLibrary() {
  menu = newMenu(NULL, fonts[AKROBAT_BOLD_18], 9);
  menu->addOption("Add books over WiFi...", BOOKS_ROW_ADD, 1);
  menu->addOption("Manage books...", BOOKS_ROW_MANAGE, 1);
  menu->addOption("How to read...", BOOKS_ROW_HELP, 1);

  /* No "43%" against each row, deliberately. A stored position is keyed by the book's epub
   * ids, and deriving those means opening the file — 90 spine items for one of these — so a
   * library of ten books would take seconds to appear. How far in you are is on the page
   * itself and in Book info. */
  for (int i = 0; i < bookCount; i++) {
    menu->addOption(books[i].name, (MenuOption::keyType)(BOOKS_ROW_FIRST + i), 1);
  }

  if (bookCount == 0) {
    menu->addNote("(no books yet - add some)");
  }
  if (libNote[0]) {
    menu->addNote(libNote);              // e.g. a book that would not open
  }
}

// True for a book that is not in /books — it arrived through the ROM uploader or by hand.
bool BooksApp::isStray(int i) const {
  return i >= 0 && i < bookCount &&
         strncmp(books[i].path, BOOKS_DIR "/", strlen(BOOKS_DIR) + 1) != 0;
}

void BooksApp::buildManage() {
  menu = newMenu("No books on the SD card", fonts[AKROBAT_BOLD_18], 9);
  char line[96];

  /* New books land in /books; these got here another way — most likely through the Game Boy
   * ROM uploader, which was the only one that existed before this app and filters nothing.
   * Offered as one explicit action rather than done silently on startup: moving someone's
   * files without asking is not the reader's business. */
  int stray = 0;
  for (int i = 0; i < bookCount; i++) {
    if (isStray(i)) {
      stray++;
    }
  }
  if (stray > 0) {
    snprintf(line, sizeof(line), "Move %d book%s into /books", stray, stray == 1 ? "" : "s");
    menu->addOption(line, BOOKS_ROW_TIDY, 1);
  }

  for (int i = 0; i < bookCount; i++) {
    bool inBooks = !isStray(i);
    if (pendingDelete == i) {
      snprintf(line, sizeof(line), "Delete %s? (confirm)", books[i].name);
    } else if (inBooks) {
      snprintf(line, sizeof(line), "%s", books[i].name);
    } else {
      snprintf(line, sizeof(line), "%s  [in %s]", books[i].name,
               strncmp(books[i].path, "/roms/", 6) == 0 ? "roms" : "root");
    }
    menu->addOption(line, (MenuOption::keyType)(BOOKS_ROW_FIRST + i), 1);
  }
}

// ---------------------------------------------------------------- opening a book

bool BooksApp::openBook(int idx) {
  closeBook(true);
  if (idx < 0 || idx >= bookCount) {
    return false;
  }

  /* ⏱ MEASURED, NOT GUESSED. Opening a book freezes the whole superloop — keypad, screen
   * AND every piece of the WiFi rescue machinery, all of which is polled from loop() — for
   * 12.4-12.6 s on this phone (four measurements, 2026-08-27). That window is how a WiFi
   * drop gets a head start it never recovers from; see docs/HANDOFF.md, the booksync wedge.
   * The phase breakdown stays in permanently so the next person does not have to re-derive
   * where the time goes, the way SLOW WIFI / SLOW STEP already do for the network paths.
   *
   * ⚠ The guard below reports the FAILED opens too. Four paths return early from here, and
   * a book that grinds for ten seconds and then fails is the single most interesting case
   * for the reason this instrumentation exists — the superloop was frozen for all ten of
   * them — yet it used to print nothing at all, because the success line is the last
   * statement. Being a destructor, it also covers any return added later. */
  struct OpenReport {
    uint32_t t0;
    bool ok;
    ~OpenReport() {
      if (!ok) {
        log_e("BOOK OPEN FAILED after %u ms - the superloop was frozen for it",
              (unsigned)(millis() - t0));
      }
    }
  } _report = { millis(), false };
  const uint32_t _o0 = _report.t0;
  BOOKS_HEAP("open:enter");
  file = SD.open(books[idx].path, FILE_READ);
  if (!file) {
    return false;
  }
  src.ctx = &file;
  src.size = file.size();
  src.read = sdSourceRead;
  const uint32_t _oOpen = millis();

  if (epubOpen(&book, &src, books[idx].name, books[idx].isTxt) != EPUB_OK) {
    file.close();
    return false;
  }
  const uint32_t _oParse = millis();
  BOOKS_HEAP("open:parsed");

  // One chapter buffer for the session. PSRAM: 512 KB of internal RAM does not exist here.
  chapCap = EPUB_MAX_DOC;
  chapText = (char*)ps_malloc(chapCap);
  if (!chapText) {
    chapCap = 128 * 1024;
    chapText = (char*)ps_malloc(chapCap);
  }
  if (!chapText) {
    epubClose(&book);
    file.close();
    chapCap = 0;
    return false;
  }
  /* A .txt is shown CUT at chapCap-1 bytes. epubOpen already refuses KOSync for one bigger
   * than EPUB_MAX_DOC; the smaller fallback buffer just above cuts sooner, so say the same
   * here — a place on cut text is not a place in the book. */
  if (book.isText && book.kosync && !book.kosync->refused && src.size > (uint64_t)(chapCap - 1)) {
    book.kosync->refused = EPUB_KOSYNC_TEXT_TOO_BIG;
  }

  isOpen = true;
  openIdx = idx;
  nIds = epubIds(&book, ids);

  // Where were we? A stored position wins; otherwise the top of the book.
  int startSpine = 0;
  uint32_t startOff = 0;
  openedFromSaved = false;
  openedSpine = 0;
  openedPct = 0;
  if (store) {
    const char* idp[BOOKSYNC_MAX_IDS];
    for (int i = 0; i < nIds; i++) {
      idp[i] = ids[i];
    }
    BookPos pos;
    if (nIds > 0 && store->get(idp, nIds, &pos)) {
      startSpine = (int)pos.spine;
      startOff = pos.offset;
      if (startSpine < 0 || startSpine >= book.nSpine) {
        startSpine = 0;
        startOff = 0;
      } else {
        // Nothing has been written yet this session, so this IS what was on the card.
        openedFromSaved = true;
        openedSpine = pos.spine;
        openedPct = (int)(pos.fraction * 100.0 + 0.5);
      }
    }
  }

  saveFailed = false;      // a failure belongs to the book that had it, not to this one
  const uint32_t _oPos = millis();

  if (!loadChapter(startSpine)) {
    closeBook(false);
    return false;
  }
  /* Skip past chapters with no text. A cover page is a spine item made entirely of an image,
   * so opening this book landed on "(this chapter has no text)" — a reader that looks broken
   * on the first screen. There is no position to lose inside an empty chapter. */
  int _oSkips = 0;
  while (chapLen == 0 && spine + 1 < book.nSpine) {
    if (!loadChapter(spine + 1)) {
      break;
    }
    startOff = 0;
    _oSkips++;
  }
  const uint32_t _oChap = millis();
  gotoOffset(startOff, true);
  turnsSinceSave = 0;
  ksMovedSpine = spine;                // opening is not a move (kosyncTellPosition)
  ksMovedOff = pageStart;
  const uint32_t _oGoto = millis();
  checkForPending();   // a position may have arrived while this book was shut
  const uint32_t _oPend = millis();
  /* KOSync, when configured: the two document ids (12 small reads) and then a pull from home
   * (on WiFi) or a short window (off it). Neither blocks here — the pull is a state machine
   * in kosyncLoop() and the window is opened from there too — so this costs the reads. */
  if (kosyncConfigured()) {
    kosyncIds();
    if (kosyncSnapshot()) {
      kosyncBookOpened(ksBook);
    }
  }
  BOOKS_HEAP("open:done");
  const uint32_t _oEnd = millis();
  log_e("BOOK OPEN %u ms [sdopen=%u epubOpen=%u pos=%u chapter=%u(+%d skips) goto=%u pending=%u ks=%u] spine=%d/%d",
        (unsigned)(_oEnd - _o0), (unsigned)(_oOpen - _o0), (unsigned)(_oParse - _oOpen),
        (unsigned)(_oPos - _oParse), (unsigned)(_oChap - _oPos), _oSkips,
        (unsigned)(_oGoto - _oChap), (unsigned)(_oPend - _oGoto), (unsigned)(_oEnd - _oPend),
        spine, book.nSpine);
  _report.ok = true;                  // reported here; the guard covers every other exit
  return true;
}

void BooksApp::closeBook(bool save) {
  if (!isOpen) {
    return;
  }
  if (save) {
    savePosition(true);
  }
  epubClose(&book);
  if (file) {
    file.close();
  }
  if (chapText) {
    free(chapText);
    chapText = NULL;
  }
  chapCap = 0;
  chapLen = 0;
  isOpen = false;
  openIdx = -1;
  histN = 0;
  undoValid = false;      // an undo belongs to the book that was open when it was taken
  ksIdsDone = false;      // KOSync ids belong to the file that was open
  ksPartial[0] = '\0';
  ksByName[0] = '\0';
  ksMovedSpine = -1;
}

bool BooksApp::loadChapter(int i) {
  if (!isOpen || !chapText || i < 0 || i >= book.nSpine) {
    return false;
  }
  spine = i;
  nImages = 0;
  chapLen = epubChapterTextImages(&book, i, chapText, chapCap,
                                  images, images ? EPUB_MAX_IMAGES : 0, &nImages);
  BOOKS_HEAP("chapter:text");
  loadChapterImages();
  BOOKS_HEAP("chapter:images");
  pageStart = 0;
  histN = 0;
  return true;
}

/* Read each picture's dimensions and work out how many rows of text it will occupy.
 *
 * Only the file HEADER is read — epubReadEntryPrefix touches a few KB whatever the picture
 * weighs, so a 1453x1920 cover costs the same as a 176x176 ornament. The scale mirrors what
 * the decoder will actually do (tjpgd only halves), so the rows reserved here and the pixels
 * drawn later cannot disagree. */
void BooksApp::loadChapterImages() {
  if (!images || !imgBoxes) {
    nImages = 0;
    return;
  }
  const int boxW = pageWidth();
  const int lh = fonts[BODY_FONTS[fontIdx]]->height() + 2;
  const int maxRows = pageLines() - 1 > 0 ? pageLines() - 1 : 1;

  for (int i = 0; i < nImages; i++) {
    uint8_t hdr[1024];
    size_t got = epubReadEntryPrefix(&book, images[i].name, hdr, sizeof(hdr));
    uint16_t w = 0, h = 0;
    uint8_t comps = 0;
    imgGrey[i] = false;
    if (got && epubImageInfo(hdr, got, &w, &h, &comps) && comps == 1) {
      imgGrey[i] = true;               // greyscale: the ROM decoder will not touch it
    }
    if (!got || !epubImageSize(hdr, got, &w, &h) || !w || !h) {
      images[i].w = images[i].h = 0;          // unreadable: reserve one row for the caption
      imgBoxes[i].off = images[i].off;
      imgBoxes[i].rows = 1;
      continue;
    }
    images[i].w = w;
    images[i].h = h;

    int dh = h;
    for (int s = 0; s < 3 && (w >> s) > boxW; s++) {
      dh = h >> (s + 1);                      // the same halving load_jpg_at will pick
    }
    int rows = (dh + lh - 1) / lh + 1;        // +1 for the "[1] press 1" caption row
    if (rows < 2) {
      rows = 2;
    }
    if (rows > maxRows) {
      rows = maxRows;
    }
    imgBoxes[i].off = images[i].off;
    imgBoxes[i].rows = (uint16_t)rows;
  }
}

int BooksApp::imageRows(int i) const {
  return (i >= 0 && i < nImages) ? imgBoxes[i].rows : 1;
}

/* Decode a picture straight from the zip into the given box. The compressed bytes go to
 * PSRAM for the length of the draw and no longer: chapters have one or two pictures, but the
 * cover alone is a megabyte and holding them all would cost more than it buys. */
/* Where a greyscale picture goes on screen, and how much it shrinks on the way.
 *
 * jpeg_grey streams eight rows at a time rather than handing back a framebuffer — the cover
 * of this book is 1453x1920 and a full one would be 5 MB — so scaling happens here, by
 * sampling every Nth row and column. Nearest-neighbour deliberately: on a 240px panel showing
 * line art, averaging blurs exactly the thing that makes it readable. */
struct GreyBlit {
  LCD*     lcd;
  int      x, y;
  int      scale;
  uint16_t outW, outH;
};

static void greyBlitRows(void* ctx, int y0, int rows, const uint8_t* band, int width) {
  GreyBlit* g = (GreyBlit*)ctx;
  uint16_t line[240];
  const int cap = (int)(sizeof(line) / sizeof(line[0]));
  lcdNativePixels(*g->lcd, true);            // native color565 words: see GUI.h
  for (int r = 0; r < rows; r++) {
    int sy = y0 + r;
    if (sy % g->scale) {
      continue;                              // this source row is not one we sample
    }
    int dy = sy / g->scale;
    if (dy >= g->outH) {
      break;
    }
    const uint8_t* src = band + (size_t)r * width;
    int n = g->outW < cap ? g->outW : cap;
    for (int dx = 0; dx < n; dx++) {
      int sx = dx * g->scale;
      uint8_t v = (sx < width) ? src[sx] : 0;
      line[dx] = g->lcd->color565(v, v, v);
    }
    g->lcd->pushImage(g->x, g->y + dy, n, 1, line);
  }
  lcdNativePixels(*g->lcd, false);
}

bool BooksApp::drawOneImage(int i, int x, int y, uint16_t boxW, uint16_t boxH) {
  if (i < 0 || i >= nImages || !isOpen) {
    return false;
  }
  size_t sz = epubEntrySize(&book, images[i].name);
  if (!sz) {
    return false;
  }
  uint8_t* data = (uint8_t*)ps_malloc(sz);
  if (!data) {
    return false;
  }
  bool ok = false;
  if (epubReadEntry(&book, images[i].name, data, sz) == sz) {
    uint16_t pw = images[i].w ? images[i].w : boxW;
    uint16_t ph = images[i].h ? images[i].h : boxH;

    if (jpegGreyIsGreyBaseline(data, sz)) {
      /* Greyscale: our own decoder, because the ESP32's is in ROM and does 3-component
       * YCbCr only. Not an edge case — 33 of the 45 pictures in this book are greyscale.
       * Unlike tjpgd this scales by any integer, not just halves, so it fills the box more
       * closely. */
      GreyBlit g;
      g.lcd = &lcd;
      g.scale = 1;
      while ((pw + g.scale - 1) / g.scale > boxW || (ph + g.scale - 1) / g.scale > boxH) {
        g.scale++;
        if (g.scale > 32) {
          break;
        }
      }
      g.outW = (uint16_t)((pw + g.scale - 1) / g.scale);
      g.outH = (uint16_t)((ph + g.scale - 1) / g.scale);
      g.x = x + (boxW - g.outW) / 2;
      if (g.x < x) {
        g.x = x;
      }
      g.y = y;
      uint16_t dw = 0, dh = 0;
      ok = jpegGreyDecode(data, sz, greyBlitRows, &g, &dw, &dh) == JPEG_GREY_OK;
    } else {
      uint16_t ow = 0, oh = 0;
      // Colour: the ROM decoder. It only halves, so a picture rarely fills its box exactly.
      int scale = 0;
      while (scale < 3 && ((pw >> scale) > boxW || (ph >> scale) > boxH)) {
        scale++;
      }
      int cx = x + (boxW - (pw >> scale)) / 2;
      if (cx < x) {
        cx = x;
      }
      ok = display::load_jpg_at(data, sz, &lcd, (int16_t)cx, (int16_t)y, boxW, boxH, &ow, &oh) != 0;
    }
  }
  free(data);
  BOOKS_HEAP("image:drawn");
  return ok;
}

/* 0.0-1.0 through the whole book. Deliberately NOT epubFraction(): that re-extracts the
 * chapter to measure it, which means a 512 KB allocation and a full inflate on every page
 * turn. The formula is the same one, fed from the chapter we already have in hand. */
double BooksApp::fractionHere() const {
  int n = book.nSpine > 0 ? book.nSpine : 1;
  double within = chapLen ? (double)pageStart / (double)chapLen : 0.0;
  if (within > 1.0) {
    within = 1.0;
  }
  double f = ((double)spine + within) / (double)n;
  return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
}

bool BooksApp::savePosition(bool flush) {
  if (!isOpen || nIds <= 0) {
    return true;                  // nothing to save is not a failure
  }
  /* ⚠ But NO STORE is. `store` is a ps_malloc in the constructor (see the allocation there)
   * and nothing re-tries it, so if it ever came back null every position this session is
   * unwritable — reporting that as a successful save would hide it for ever, which is the
   * whole class of fault this return value was added to expose. */
  if (!store) {
    saveFailed = true;
    return false;
  }
  const char* idp[BOOKSYNC_MAX_IDS];
  for (int i = 0; i < nIds; i++) {
    idp[i] = ids[i];
  }
  /* UTC: this becomes BookSyncRecord::turnedAt, which goes on the air and is compared against
   * COVEY's own UTC stamps. The local-shifted epoch made this phone's page look eight hours
   * old to COVEY, and made COVEY's look eight hours in the future to this phone.
   * TRUSTED (ntp/gps) since 0.9.79: a mesh-set clock stamps 0, exactly as the unknown clock it
   * replaced did — a time nobody vouched for must not win "the later turn" on another device. */
  uint32_t now = ntpClock.getTrustedUtcTime();
  store->put(idp, nIds, (uint32_t)spine, pageStart, fractionHere(), now);
  /* A MOVE is the place changing, not a save: closes, flushes and the OK-to-menu save all come
   * through here with the place where it was. (The store's own turnedAt above is stamped
   * either way — that is LoRa's meaning and it stays.) */
  const bool moved = (spine != ksMovedSpine || pageStart != ksMovedOff);
  if (moved) {
    ksMovedSpine = spine;
    ksMovedOff = pageStart;
  }
  kosyncTellPosition(moved);     // RAM only (plus a stamp): nothing here touches the store
  if (!flush) {
    return true;
  }
  /* 🛑 SAY SO WHEN THE CARD REFUSES. This bool used to be discarded here and at the page-turn
   * flush, and it is the ONLY channel either has: on failure `dirty` stays set, the RAM store
   * goes on answering every read correctly for the rest of the session, and the loss appears
   * the next time a BooksApp is built and loads from the card — the following morning. A
   * whole evening's reading can go that way without one line anywhere. */
  const bool ok = store->saveIfDirty(&storeIo);
  turnsSinceSave = 0;
  if (!ok) {
    log_e("BOOK SAVE FAILED: %s not written (spine=%d pageStart=%lu) - the place on the card "
          "is now STALE and this session will be lost when the book is next opened",
          BOOKS_POS_FILE, spine, (unsigned long)pageStart);
  }
  saveFailed = !ok;
  return ok;
}

// ---------------------------------------------------------------- sync

/* The passcode and this device's name live in NVS under `wpmesh`, the namespace the mesh
 * service already owns for the node name and hop limit — same radio, same settings drawer. */
void BooksApp::loadSyncSettings() {
  Preferences p;
  syncPass[0] = '\0';
  syncDev[0] = '\0';
  if (p.begin("wpmesh", true)) {
    p.getString("bspw", syncPass, sizeof(syncPass));
    p.getString("bsdev", syncDev, sizeof(syncDev));
    p.end();
  }
  if (!syncDev[0]) {
    // Default to the mesh node name, which is already something the user chose.
    const char* n = meshService.getMyLongName();
    snprintf(syncDev, sizeof(syncDev), "%s", (n && n[0]) ? n : "WiPhone");
  }
}

void BooksApp::saveSyncSettings() {
  Preferences p;
  if (p.begin("wpmesh", false)) {
    p.putString("bspw", syncPass);
    p.putString("bsdev", syncDev);
    p.end();
  }
}

/* Broadcast where I am, on the `booksync` channel.
 *
 * ⚠ Found BY NAME, and there is NO fallback to the primary channel. That is not caution for
 * its own sake: a reading position on LongFast is readable by every node in range, and the
 * failure would be invisible — the packet would send happily and look like success. Better to
 * refuse and say why. */
bool BooksApp::sendMyPlace() {
  syncNote[0] = '\0';
  syncTxId = 0;                  // an older frame's answer must not re-word THIS attempt's note
  if (!isOpen || nIds <= 0) {
    snprintf(syncNote, sizeof(syncNote), "No book open");
    return false;
  }

  const MeshChannel* ch = NULL;
  int n = meshService.getChannelCount();
  for (int i = 0; i < n; i++) {
    const MeshChannel* c = meshService.getChannel(i);
    if (c && strcasecmp(c->name, "booksync") == 0) {
      ch = c;
      break;
    }
  }
  if (!ch) {
    snprintf(syncNote, sizeof(syncNote), "No 'booksync' channel - import it first");
    return false;
  }

  const char* idp[BOOKSYNC_MAX_IDS];
  for (int i = 0; i < nIds; i++) {
    idp[i] = ids[i];
  }
  uint32_t now = ntpClock.getTrustedUtcTime();   // UTC: goes on the air - so ntp/gps only, never a mesh clock

  BookSyncRecord r;
  bookSyncMakeRecord(&r, idp, nIds, (uint32_t)spine, pageStart, fractionHere(),
                     now, syncDev, NULL);
  uint8_t key[32];
  bookSyncDeriveKey(syncPass, key);

  char text[BOOKSYNC_MESH_TEXT_MAX];
  if (!bookSyncPackMesh(&r, key, text, sizeof(text))) {
    snprintf(syncNote, sizeof(syncNote), "Could not build the packet");
    return false;
  }
  bool ok = meshService.sendChannelMessage(ch->hash, text);
  /* 🛑 QUEUED, NOT SENT (review M1, 2026-09-25). true means the frame is waiting for the radio;
   * it goes within a second or two, or it does not (the pack died, the radio was lost with it
   * queued). The note says so until the radio has spoken — syncNoteSettle(), on the menu's
   * rebuild and its KOSync tick — and a refusal says WHY, in the compose screen's words. */
  syncTxCh = spine + 1;
  syncTxPct = (int)(fractionHere() * 100.0 + 0.5);
  syncTxId = ok ? meshService.lastQueuedPacketId() : 0;
  if (ok) {
    snprintf(syncNote, sizeof(syncNote), "Queued: ch %d, %d%%", syncTxCh, syncTxPct);
  } else {
    snprintf(syncNote, sizeof(syncNote), "Not sent: %s",
             meshService.lastSendError() ? meshService.lastSendError() : "radio would not send");
  }
  return ok;
}

/* The frame "Sync my place" queued, re-read: "Sent" once it left whole, "NOT sent" if it never
 * did. Still queued leaves "Queued" standing; FORGOTTEN (UNKNOWN: 16 newer own frames since, so
 * the menu sat unopened a long while) stops asking and leaves it too — never a guess at "Sent".
 * True when the note changed, so the menu rebuilds. */
bool BooksApp::syncNoteSettle() {
  if (!syncTxId) {
    return false;
  }
  const uint8_t o = meshService.txOutcome(syncTxId);
  if (o == MESH_TXO_QUEUED) {
    return false;
  }
  syncTxId = 0;
  if (o == MESH_TXO_SENT) {
    snprintf(syncNote, sizeof(syncNote), "Sent: ch %d, %d%%", syncTxCh, syncTxPct);
  } else if (o == MESH_TXO_FAILED) {
    snprintf(syncNote, sizeof(syncNote), "NOT sent - the radio did not send it");
  } else {
    return false;                          // no word: "Queued" stays, and is still true
  }
  return true;
}

/* 🛑 THE READING MENU TICKS WHILE ITS FRAME WAITS, NOT ONLY WHEN KOSYNC IS SET UP (integration
 * review of M1, 2026-09-25). The menu used to tick only for KOSync, so on the ordinary setup —
 * LoRa booksync, no /books/kosync.txt, which is how the phones run until KOSync credentials go
 * in — "Queued: ch N, X%" stood for as long as the menu was open, and "Sent" appeared only
 * after leaving and coming back: the one confirmation the old blocking send gave, lost on the
 * commonest setup. Now: BOOKS_SYNC_WATCH_MS while syncTxId waits, KOSync's 1 s otherwise, and
 * NO tick at all on a settled menu without KOSync (a menu left open must not keep the CPU
 * waking). Called AFTER buildMenu() everywhere — its syncNoteSettle() may already have
 * answered, and a watch armed on a stale syncTxId would never stand down — and on every tick,
 * so the watch ends with the answer. Touches no position: the note and the tick only. */
void BooksApp::armMenuTick() {
  controlState.msAppTimerEventPeriod = syncTxId ? BOOKS_SYNC_WATCH_MS
                                                : (kosyncConfigured() ? 1000 : 0);
}

/* Anything parked for the book that is open? Called on opening one, on the reading screen's
 * one-second tick whenever the inbox has changed, and after a passcode edit — because a LoRa
 * round trip does not fit inside the moment you press Sync, and because the person reading
 * should not have to guess that it landed. */
void BooksApp::checkForPending() {
  /* Stamped even on the early exits: this says "the inbox as of seq N has been looked at",
   * and a book that is not open has been looked at just as conclusively as one that is. */
  syncSeqSeen = bookSyncInboxSeq();
  pendingIdx = -1;
  pendingId = 0;
  if (!isOpen || nIds <= 0) {
    return;
  }
  const char* idp[BOOKSYNC_MAX_IDS];
  for (int i = 0; i < nIds; i++) {
    idp[i] = ids[i];
  }
  uint8_t key[32];
  bookSyncDeriveKey(syncPass, key);
  pendingIdx = bookSyncInboxFindFor(key, idp, nIds, &pending, &pendingFrom);
  /* Take a handle that survives the wait for a keypress. `pending` itself is a by-value copy,
   * so the JUMP was always to the right place — it is the REMOVAL that went to the wrong
   * packet, which left the position just applied parked and ready to be offered all over
   * again while an unrelated (often newer) one was deleted in its place. */
  pendingId = bookSyncInboxIdAt(pendingIdx);
  if (pendingIdx >= 0) {
    /* 🛑 UTC. bookSyncSuspectClock() is `r->turnedAt > nowUnix + 300`, and r->turnedAt is
     * COVEY's real UTC — so against the local-shifted epoch, ANY record COVEY turned in the
     * last ~7.9 hours was painted "(their clock looks wrong)". The warning was almost always
     * about our own arithmetic.
     * TRUSTED: "their clock looks wrong" is only a fair thing to say with a clock of our own
     * that somebody vouched for; against a mesh-set one it stays unsaid (0.9.79). */
    uint32_t now = ntpClock.getTrustedUtcTime();
    pendingClock = now && bookSyncSuspectClock(&pending, now);
  }
}

/* Take the jump. Uses the FRACTION rather than the raw offset, because the two devices do not
 * agree on offsets to the byte: COVEY counts characters where this counts bytes, and the two
 * extractors keep slightly different amounts of markup. epubLocate turns a whole-book fraction
 * back into a position here, which is what makes a jump from COVEY land somewhere sane. */
void BooksApp::applyPending() {
  if (pendingIdx < 0 || !isOpen) {
    return;
  }
  /* Remember what this is about to overwrite. The jump is saved to the card immediately and
   * every parked offer is retired on the way out, so without this there is nothing left to
   * go back to and no record that anything moved. */
  undoSpine = spine;
  undoOffset = pageStart;
  undoPct = (int)(fractionHere() * 100.0 + 0.5);
  undoValid = true;
  undoMs = millis();

  int sp = spine;
  uint32_t off = 0;
  epubLocate(&book, pending.fraction, &sp, &off);
  if (sp < 0 || sp >= book.nSpine) {
    sp = 0;
    off = 0;
  }
  if (sp != spine) {
    loadChapter(sp);
  }
  gotoOffset(off, true);
  savePosition(true);
  /* An ANSWER: a home record parked as this card is now taken, and not offered again. Only
   * here and at "Stay" — a park is "declined" when a person answers it, never when it is
   * parked (kosync.h, KS-2). A LoRa card: nothing. */
  kosyncCardAnswered(pendingId);
  /* Every parked position for THIS book is now history — not just the one taken. Removing
   * only the accepted packet left its predecessors parked, and the removal's own seq bump
   * sent the reading screen straight back to checkForPending(), which offered the
   * second-newest. Accepting three stacked syncs walked you backwards through all three. */
  dropParkedForThisBook();
  pendingIdx = -1;
  pendingId = 0;
}

/* Put back the place the last jump overwrote. One step, this book, this session — the jump
 * it undoes is a mistake noticed within seconds, not tomorrow, so nothing here is persisted
 * beyond the ordinary savePosition() that commits the restored place. */
bool BooksApp::undoOffered() const {
  return undoValid && isOpen && (millis() - undoMs) < BOOKS_UNDO_TTL_MS;
}

void BooksApp::applyUndo() {
  if (!undoOffered()) {
    return;
  }
  if (undoSpine >= 0 && undoSpine < book.nSpine) {
    if (undoSpine != spine) {
      loadChapter(undoSpine);
    }
    gotoOffset(undoOffset, true);
    savePosition(true);
  }
  undoValid = false;
}

/* Shared by "Go there" and "Stay where I am": both are the reader saying where it stands for
 * this book, so both retire every offer parked for it. */
void BooksApp::dropParkedForThisBook() {
  if (nIds <= 0) {
    bookSyncInboxRemoveId(pendingId);        // no ids to match on: at least retire this one
    return;
  }
  const char* idp[BOOKSYNC_MAX_IDS];
  for (int i = 0; i < nIds; i++) {
    idp[i] = ids[i];
  }
  uint8_t key[32];
  bookSyncDeriveKey(syncPass, key);
  if (bookSyncInboxDropForBook(key, idp, nIds) == 0) {
    bookSyncInboxRemoveId(pendingId);        // belt and braces if it no longer verifies
  }
}

// ---------------------------------------------------------------- KOSync (kosync_sync.h)

// The booksync LoRa transport exists on this phone (the channel is found BY NAME, as always).
static bool haveBooksyncChannel() {
  const int n = meshService.getChannelCount();
  for (int i = 0; i < n; i++) {
    const MeshChannel* c = meshService.getChannel(i);
    if (c && strcasecmp(c->name, "booksync") == 0) {
      return true;
    }
  }
  return false;
}

/* The two KOSync document ids of the open file. Once per open book, and only when KOSync is
 * configured: the partial MD5 is 12 small reads spread across the file. A failed read gives
 * no partial id (never a wrong one) and the file-name id still works.
 *
 * ⚠ The file name comes from the PATH, not books[].name: that one is cut at 63 bytes for the
 * list, and an MD5 of a cut name matches nothing on the X4. */
void BooksApp::kosyncIds() {
  if (ksIdsDone || !isOpen || openIdx < 0) {
    return;
  }
  ksIdsDone = true;
  const char* slash = strrchr(books[openIdx].path, '/');
  if (!epubKosyncIds(&src, slash ? slash + 1 : books[openIdx].path, ksPartial, ksByName)) {
    log_e("KOSYNC: could not read %s for its partial MD5 - the file-name id only",
          books[openIdx].path);
  }
}

/* Everything the window and the home client need, copied out of the reader (they outlive
 * it). The percentage is made HERE, from the chapter in hand — the same inputs fractionHere()
 * uses, so it costs no extraction and changes nothing that is stored. */
bool BooksApp::kosyncSnapshot() {
  if (!isOpen || !ksBook || !book.kosync || !ksIdsDone) {
    return false;
  }
  KosyncBook* k = ksBook;
  memset(k, 0, sizeof(*k));
  snprintf(k->title, sizeof(k->title), "%s", book.title);
  snprintf(k->partial, sizeof(k->partial), "%s", ksPartial);
  snprintf(k->byName, sizeof(k->byName), "%s", ksByName);
  for (int i = 0; i < nIds && i < 3; i++) {
    snprintf(k->ids[i], sizeof(k->ids[i]), "%s", ids[i]);
  }
  k->nIds = nIds;
  k->nRead = book.nSpine;
  memcpy(&k->map, book.kosync, sizeof(k->map));
  k->pctOk = epubKosyncPercentAt(&book, spine, pageStart, chapLen, &k->pct);
  /* The last real MOVE (kosyncNoteMoved), UTC or 0 — 🛑 NOT the store's turnedAt, which every
   * save and close restamps: compared against that, an open-and-close that read nothing hid a
   * newer X4 place for good. */
  k->movedAt = kosyncMovedAt(ksByName);
  return k->byName[0] != '\0';
}

void BooksApp::kosyncTellPosition(bool moved) {
  if (!ksIdsDone || !isOpen) {
    return;                      // one test per page turn for everyone without KOSync
  }
  if (moved) {
    kosyncNoteMoved(ksByName, ntpClock.getTrustedUtcTime());   // mesh clock = 0: see kosync_sync.cpp nowUtc()
  }
  if (!kosyncWantsPosition()) {
    return;
  }
  double pct = 0.0;
  const bool ok = epubKosyncPercentAt(&book, spine, pageStart, chapLen, &pct);
  kosyncNotePosition(ksPartial, ksByName, pct, ok, kosyncMovedAt(ksByName));
}

size_t BooksApp::kosyncLiveLines(char* out, size_t cap) {
  out[0] = '\0';
  if (!kosyncConfigured()) {
    return 0;
  }
  char w[100], c[100], p[160];
  kosyncWindowLine(w, sizeof(w));
  kosyncClientLine(c, sizeof(c));
  kosyncProblemLine(p, sizeof(p));
  snprintf(out, cap, "%s\n%s\n%s", w, c, p);
  return strlen(out);
}

void BooksApp::kosyncAddLines(MenuWidget* m) {
  kosyncLiveLines(ksLines, sizeof(ksLines));
  if (!m || !ksLines[0]) {
    return;
  }
  char w[100], c[100], p[160];
  kosyncWindowLine(w, sizeof(w));
  kosyncClientLine(c, sizeof(c));
  kosyncProblemLine(p, sizeof(p));
  if (w[0]) {
    m->addNoteWrapped(w);
  }
  /* 🛑 ON THE SCREEN, not only on serial: a PUT for a different document is answered 200 and
   * discarded, and the X4 then says "Upload complete" — this line is the only place that says
   * the place never landed (and why). */
  if (p[0]) {
    m->addNoteWrapped(p);
  }
  if (c[0]) {
    m->addNoteWrapped(c);
  }
}

bool BooksApp::kosyncLinesChanged() {
  char now[sizeof(ksLines)];
  kosyncLiveLines(now, sizeof(now));
  return strcmp(now, ksLines) != 0;
}

/* "Sync my place".
 *
 * Without /books/kosync.txt this is sendMyPlace() and nothing else — byte for byte the old
 * behaviour, including its "No 'booksync' channel" note. With it, the two transports are
 * independent: LoRa goes out if this phone has the channel (silently skipped if it does not
 * — someone who set up KOSync alone should not be told to import a channel), and KOSync
 * opens its window and, on WiFi, sends home. */
void BooksApp::syncMyPlace() {
  ksNote[0] = '\0';
  if (!kosyncConfigured()) {
    sendMyPlace();
    return;
  }
  if (haveBooksyncChannel()) {
    sendMyPlace();
  } else {
    syncNote[0] = '\0';
    syncTxId = 0;
  }
  kosyncIds();                   // (the place itself was flushed on the way into this menu)
  if (!kosyncSnapshot()) {
    snprintf(ksNote, sizeof(ksNote), "This book can't sync over KOSync: %s",
             epubKosyncWhyNot(book.kosync));
    return;
  }
  kosyncSyncMyPlace(ksBook, ksNote, sizeof(ksNote));
}

void BooksApp::closeBookByUser() {
  if (isOpen && kosyncConfigured()) {
    kosyncIds();
    if (kosyncSnapshot()) {
      kosyncBookClosed(ksBook);  // auto=on: a window (opened from the loop) + a push home
    }
  }
  closeBook(true);
}

bool BooksApp::kosyncBench(const char* verb, uint32_t secs, char* out, size_t cap) {
  if (!kosyncConfigured()) {
    snprintf(out, cap, "KOSync is %s", kosyncConfigNote());
    return false;
  }
  kosyncIds();
  if (!kosyncSnapshot()) {
    snprintf(out, cap, "no KOSync snapshot for the open book");
    return false;
  }
  bool ok = false;
  out[0] = '\0';
  if (!strcmp(verb, "open")) {
    ok = kosyncWindowOpen(ksBook, secs ? secs * 1000UL : KOSYNC_WINDOW_SYNC_MS, out, cap);
    if (ok) {
      kosyncWindowLine(out, cap);
    }
  } else if (!strcmp(verb, "push")) {
    ok = kosyncPush(ksBook, out, cap);
    if (ok) {
      kosyncClientLine(out, cap);
    }
  } else if (!strcmp(verb, "pull")) {
    ok = kosyncPull(ksBook, out, cap);
    if (ok) {
      kosyncClientLine(out, cap);
    }
  } else if (!strcmp(verb, "sync")) {
    ok = kosyncSyncHome(ksBook, out, cap);   // Sync my place's home half: read, then offer or send
    if (ok) {
      kosyncClientLine(out, cap);
    }
  } else {
    snprintf(out, cap, "unknown: %s", verb);
  }
  return ok;
}

// ---------------------------------------------------------------- paging

int BooksApp::pageWidth() const {
  return lcd.width() - 2 * BOOKS_MARGIN;
}

int BooksApp::pageLines() const {
  SmoothFont* f = fonts[BODY_FONTS[fontIdx]];
  int lh = f->height() + 2;
  int statusH = fonts[AKROBAT_BOLD_16]->height() + 2;
  int avail = (int)lcd.height() - (int)footer->height() - (int)header->height() - statusH - 4;
  int n = avail / lh;
  if (n < 1) {
    n = 1;
  }
  if (n > BOOK_MAX_LINES) {
    n = BOOK_MAX_LINES;
  }
  return n;
}

void BooksApp::gotoOffset(uint32_t off, bool clearHistory) {
  if (off > chapLen) {
    off = (uint32_t)chapLen;
  }
  pageStart = bookLayoutSnap(chapText, chapLen, off);
  if (clearHistory) {
    histN = 0;
  }
}

void BooksApp::nextPage() {
  if (!isOpen) {
    return;
  }
  SmoothFont* f = fonts[BODY_FONTS[fontIdx]];
  BookMeasure m = { f, fontMeasure };
  BookPage pg;
  bookLayoutPageImages(chapText, chapLen, pageStart, pageWidth(), pageLines(), &m,
                       imgBoxes, nImages, &pg);

  if (pg.next >= chapLen || pg.next <= pg.start) {
    if (spine + 1 < book.nSpine) {          // fall into the next chapter
      loadChapter(spine + 1);
      savePosition(true);
    }
    return;                                  // end of the book: stay put
  }

  if (histN < BOOKS_HIST) {
    hist[histN++] = pg.start;
  } else {
    memmove(hist, hist + 1, sizeof(hist[0]) * (BOOKS_HIST - 1));
    hist[BOOKS_HIST - 1] = pg.start;
  }
  pageStart = pg.next;

  // An SD write every page would be all we did; savePosition(true) resets the counter.
  savePosition(++turnsSinceSave >= BOOKS_SAVE_EVERY);
}

void BooksApp::prevPage() {
  if (!isOpen) {
    return;
  }
  if (histN > 0) {
    pageStart = hist[--histN];               // exact: this is where we actually came from
    /* ⚠ COUNT BACKWARD TURNS TOO. turnsSinceSave used to be advanced only by nextPage(), so
     * a long stretch of back-paging dirtied the store without ever reaching the card — and
     * BOOKS_SAVE_EVERY's "you lose at most three turns" guarantee quietly did not hold in
     * that direction. The counter is about position changes since the last write. */
    savePosition(++turnsSinceSave >= BOOKS_SAVE_EVERY);
    return;
  }
  if (pageStart == 0) {
    if (spine > 0) {                         // back into the end of the previous chapter
      loadChapter(spine - 1);
      SmoothFont* f = fonts[BODY_FONTS[fontIdx]];
      BookMeasure m = { f, fontMeasure };
      pageStart = bookLayoutPrevPageImages(chapText, chapLen, (uint32_t)chapLen,
                                           pageWidth(), pageLines(), &m, imgBoxes, nImages);
      savePosition(true);
    }
    return;
  }
  SmoothFont* f = fonts[BODY_FONTS[fontIdx]];
  BookMeasure m = { f, fontMeasure };
  pageStart = bookLayoutPrevPageImages(chapText, chapLen, pageStart, pageWidth(), pageLines(),
                                       &m, imgBoxes, nImages);
  savePosition(++turnsSinceSave >= BOOKS_SAVE_EVERY);
}

// ---------------------------------------------------------------- drawing

void BooksApp::drawPage() {
  SmoothFont* f = fonts[BODY_FONTS[fontIdx]];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = f->height() + 2;

  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  if (!isOpen) {
    return;
  }

  BookMeasure m = { f, fontMeasure };
  BookPage pg;
  bookLayoutPageImages(chapText, chapLen, pageStart, pageWidth(), pageLines(), &m,
                       imgBoxes, nImages, &pg);

  lcd.setTextFont(f);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(WHITE, BLACK);
  int y = top + 2;
  char line[448];                        // room for substitutions, which can lengthen a run
  int shown = 0;                         // pictures on this page, numbered from 1
  for (int i = 0; i < nImages; i++) {
    pageImageKey[i] = 0;
  }
  for (int i = 0; i < pg.nLines; i++) {
    int rows = pg.lines[i].rows ? pg.lines[i].rows : 1;
    if (pg.lines[i].image >= 0) {
      int k = pg.lines[i].image;
      int boxH = rows * lh - lh;         // the last row belongs to the caption
      if (boxH < lh) {
        boxH = lh;
      }
      bool drew = drawOneImage(k, BOOKS_MARGIN, y, (uint16_t)pageWidth(), (uint16_t)boxH);
      shown++;
      if (k < EPUB_MAX_IMAGES) {
        pageImageKey[k] = shown;         // pressing this number opens it full-screen
      }
      // A caption under it, so the way to enlarge is visible rather than folklore.
      SmoothFont* cf = fonts[AKROBAT_BOLD_16];
      lcd.setTextFont(cf);
      lcd.setTextColor(drew ? TFT_DARKGREY : TFT_ORANGE, BLACK);
      char cap[72];
      if (drew) {
        snprintf(cap, sizeof(cap), "[%d] press %d to enlarge", shown, shown);
      } else {
        snprintf(cap, sizeof(cap), "[picture %d could not be shown]", shown);
      }
      lcd.drawString(cap, BOOKS_MARGIN, y + boxH);
      lcd.setTextFont(f);
      lcd.setTextColor(WHITE, BLACK);
    } else if (!pg.lines[i].blank) {
      bookRenderRun(f, chapText + pg.lines[i].off, pg.lines[i].len, line, sizeof(line));
      lcd.drawString(line, BOOKS_MARGIN, y);
    }
    y += rows * lh;
  }

  if (pg.nLines == 0) {
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("(this chapter has no text)", BOOKS_MARGIN, top + 2);
  }

  // Status strip: which chapter, and how far through the book.
  SmoothFont* sf = fonts[AKROBAT_BOLD_16];
  int sy = bottom - sf->height() - 2;
  lcd.setTextFont(sf);
  lcd.setTextColor(TFT_DARKGREY, BLACK);
  char st[32];
  snprintf(st, sizeof(st), "ch %d/%d", spine + 1, book.nSpine);
  lcd.setTextDatum(TL_DATUM);
  lcd.drawString(st, BOOKS_MARGIN, sy);
  snprintf(st, sizeof(st), "%d%%", (int)(fractionHere() * 100.0 + 0.5));
  lcd.setTextDatum(TR_DATUM);
  lcd.drawString(st, lcd.width() - BOOKS_MARGIN, sy);
  lcd.setTextDatum(TL_DATUM);
}

static const char* const s_helpLines[] = {
  "@TURNING PAGES",
  "Down / Right: next page",
  "Up / Left: back a page",
  "Pages roll into the next",
  "chapter on their own.",
  "@WHILE READING",
  "OK: menu (chapters, text",
  " size, book info)",
  "Back: close the book",
  "@PICTURES",
  "A picture in the text is",
  "captioned [1], [2]...",
  "Press that number to see",
  "it full screen; any key",
  "goes back to the page.",
  "@YOUR PLACE IS KEPT",
  "Closing a book saves",
  "where you were, and it",
  "survives a flat battery.",
  "Text size does not move",
  "it: a position is a place",
  "in the text, not a page",
  "number, which is what",
  "lets COVEY understand it.",
  "@ADDING BOOKS",
  "Pick 'Add books over",
  "WiFi', press OK, then on",
  "your computer open",
  "wiphone.local and drop",
  ".epub or .txt files in.",
  "@REMOVING BOOKS",
  "'Manage books' deletes a",
  "book you have finished",
  "(it asks first).",
};
static const int s_helpCount = (int)(sizeof(s_helpLines) / sizeof(s_helpLines[0]));

void BooksApp::debugDumpPage() {
  if (!isOpen || !chapText) {
    log_e("BOOKPAGE: no book open");
    return;
  }
  SmoothFont* f = fonts[BODY_FONTS[fontIdx]];
  BookMeasure m = { f, fontMeasure };
  BookPage pg;
  bookLayoutPageImages(chapText, chapLen, pageStart, pageWidth(), pageLines(), &m,
                       imgBoxes, nImages, &pg);
  log_e("BOOKPAGE: spine=%d pageStart=%lu next=%lu width=%dpx lines=%d font=%d",
        spine, (unsigned long)pageStart, (unsigned long)pg.next, pageWidth(),
        (int)pg.nLines, fontIdx);
  char line[448];
  for (int i = 0; i < pg.nLines; i++) {
    if (pg.lines[i].blank) {
      log_e("BOOKPAGE %02d: (blank)", i);
      continue;
    }
    if (pg.lines[i].image >= 0) {
      log_e("BOOKPAGE %02d: (image %d, rows=%d)", i, pg.lines[i].image, (int)pg.lines[i].rows);
      continue;
    }
    const uint32_t off = pg.lines[i].off;
    const uint16_t len = pg.lines[i].len;
    bookRenderRun(f, chapText + off, len, line, sizeof(line));
    if (strlen(line) > 120) {
      line[120] = '\0';                        // log lines have a budget; the head is the evidence
    }
    log_e("BOOKPAGE %02d: off=%lu len=%u w=%dpx raw0=%02x rend0=%02x |%s|",
          i, (unsigned long)off, (unsigned)len, (int)f->textWidth(line),
          (unsigned char)chapText[off], (unsigned char)line[0], line);
  }
}

bool booksSaveOpenPosition() {
  const bool ok = s_liveBooksApp ? s_liveBooksApp->savePosition(true) : true;
  kosyncSaveState();             // this session's last-move stamps (no close is coming)
  return ok;
}

bool booksKosyncBench(const char* verb, uint32_t secs, char* out, size_t cap) {
  if (!s_liveBooksApp || !s_liveBooksApp->isOpen) {
    snprintf(out, cap, "no book open - `open books`, then open one");
    return false;
  }
  return s_liveBooksApp->kosyncBench(verb, secs, out, cap);
}

void booksDebugDumpPage() {
  if (s_liveBooksApp) {
    s_liveBooksApp->debugDumpPage();
  } else {
    log_e("BOOKPAGE: open the reader first (Books > a book), then run this again");
  }
}

void BooksApp::drawHelp() {
  SmoothFont* font = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = font->height() + 2;
  const int visible = (bottom - top - 4) / lh;

  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);

  if (helpTop > s_helpCount - visible) {
    helpTop = s_helpCount - visible;
  }
  if (helpTop < 0) {
    helpTop = 0;
  }
  int y = top + 2;
  for (int i = helpTop; i < s_helpCount && i < helpTop + visible; i++) {
    const char* ln = s_helpLines[i];
    if (ln[0] == '@') {
      lcd.setTextColor(TFT_GREENYELLOW, BLACK);
      lcd.drawString(ln + 1, BOOKS_MARGIN, y);
    } else {
      lcd.setTextColor(WHITE, BLACK);
      lcd.drawString(ln, BOOKS_MARGIN, y);
    }
    y += lh;
  }
}

// Mirrors the Game Boy transfer screen, because it is the same server underneath.
void BooksApp::drawXfer() {
  SmoothFont* font = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = font->height() + 2;

  if (!xferClean) {                 // full clear on entry only; the 1 Hz refresh overdraws
    lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
    xferClean = true;
  }
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  int y = top + 2;

  if (!xferOn()) {
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString("Put .epub or .txt books", BOOKS_MARGIN, y); y += lh;
    lcd.drawString("on the SD card from a", BOOKS_MARGIN, y); y += lh;
    lcd.drawString("computer:", BOOKS_MARGIN, y); y += lh + 4;
    lcd.drawString("1. Join the same WiFi", BOOKS_MARGIN, y); y += lh;
    lcd.drawString("2. Press OK to start", BOOKS_MARGIN, y); y += lh;
    lcd.drawString("3. Open in a browser:", BOOKS_MARGIN, y); y += lh;
    lcd.setTextColor(TFT_YELLOW, BLACK);
    lcd.drawString("   wiphone.local", BOOKS_MARGIN, y); y += lh;
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString("4. Drag books in. Done.", BOOKS_MARGIN, y); y += lh;
  } else {
    lcd.setTextColor(TFT_GREEN, BLACK);
    lcd.drawString("Server ON", BOOKS_MARGIN, y); y += lh + 4;
    lcd.setTextColor(WHITE, BLACK);
    lcd.drawString("On your computer, open:", BOOKS_MARGIN, y); y += lh;
    lcd.setTextColor(TFT_YELLOW, BLACK);
    lcd.drawString("  wiphone.local", BOOKS_MARGIN, y); y += lh;
    char l[48];
    snprintf(l, sizeof(l), "  or http://%s", xferAddr());
    lcd.drawString(l, BOOKS_MARGIN, y); y += lh + 4;
    lcd.setTextColor(WHITE, BLACK);
    if (xferUsingAP()) {
      snprintf(l, sizeof(l), "(join WiFi '%s'", xferApName());
      lcd.drawString(l, BOOKS_MARGIN, y); y += lh;
      /* ⚠ Not always open any more: this uploader shares 'WiPhone-Books' with a KOSync sync
       * window, and one that started while a window held it rides on the window's WPA2
       * hotspot. Telling someone "no password" at a hotspot that asks for one is the kind
       * of screen that gets a person to give up. (Padded: this screen draws in place.) */
      lcd.drawString(xferApProtected() ? " first; it has a password)"
                                       : " first, no password)      ", BOOKS_MARGIN, y);
      y += lh + 4;
    } else {
      lcd.drawString("(same WiFi as the phone)", BOOKS_MARGIN, y); y += lh + 4;
    }
    snprintf(l, sizeof(l), "Books added: %d   ", xferFilesAdded());   // pad: drawn in place
    lcd.drawString(l, BOOKS_MARGIN, y); y += lh;
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    lcd.drawString("Back: stop and re-scan", BOOKS_MARGIN, y);
  }
}

/* What sync matches on. Worth a screen of its own: if COVEY and the phone disagree about a
 * book's identity, nothing is logged on either device — sync simply never happens — and these
 * five lines are the only place the answer is visible. The fingerprint doubles as proof that
 * a file copied over WiFi arrived intact. */
void BooksApp::drawInfo() {
  SmoothFont* font = fonts[AKROBAT_BOLD_16];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = font->height() + 2;

  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  lcd.setTextFont(font);
  lcd.setTextDatum(TL_DATUM);
  int y = top + 2;
  if (!isOpen) {
    return;
  }

  char l[96], shown[EPUB_META_MAX + 64];
  lcd.setTextColor(TFT_GREENYELLOW, BLACK);
  bookRenderRun(font, book.title, strlen(book.title), shown, sizeof(shown));
  lcd.drawFitString(shown, lcd.width() - 2 * BOOKS_MARGIN, BOOKS_MARGIN, y); y += lh;
  if (book.author[0]) {
    lcd.setTextColor(WHITE, BLACK);
    bookRenderRun(font, book.author, strlen(book.author), shown, sizeof(shown));
    lcd.drawFitString(shown, lcd.width() - 2 * BOOKS_MARGIN, BOOKS_MARGIN, y);
  }
  y += lh + 4;

  lcd.setTextColor(WHITE, BLACK);
  snprintf(l, sizeof(l), "%d chapters, %s", book.nSpine, book.isText ? "plain text" : "EPUB");
  lcd.drawString(l, BOOKS_MARGIN, y); y += lh;
  snprintf(l, sizeof(l), "chapter %d: %u chars", spine + 1, (unsigned)chapLen);
  lcd.drawString(l, BOOKS_MARGIN, y); y += lh;
  if (nImages > 0) {
    snprintf(l, sizeof(l), "%d picture%s here", nImages, nImages == 1 ? "" : "s");
    lcd.drawString(l, BOOKS_MARGIN, y); y += lh;
  }
  y += 4;

  /* Where the SD card said you were when this book was opened. On screen rather than in a
   * log, because "did my place survive the reboot?" is a question worth being able to answer
   * without a serial cable. */
  lcd.setTextColor(TFT_DARKGREY, BLACK);
  lcd.drawString("Your place", BOOKS_MARGIN, y); y += lh;
  lcd.setTextColor(openedFromSaved ? TFT_GREEN : TFT_ORANGE, BLACK);
  if (openedFromSaved) {
    snprintf(l, sizeof(l), "opened from saved: ch %u, %d%%",
             (unsigned)(openedSpine + 1), openedPct);
  } else {
    snprintf(l, sizeof(l), "opened at the start (none saved)");
  }
  lcd.drawString(l, BOOKS_MARGIN, y); y += lh;
  lcd.setTextColor(WHITE, BLACK);
  snprintf(l, sizeof(l), "now: ch %d, %d%%", spine + 1, (int)(fractionHere() * 100.0 + 0.5));
  lcd.drawString(l, BOOKS_MARGIN, y); y += lh + 4;
  /* The one state this screen exists to answer and could not: a card that is refusing writes
   * looks EXACTLY like one that is working, all session, because the RAM store answers every
   * read. Without this the loss only shows up the next morning, as a place that went stale. */
  if (saveFailed) {
    lcd.setTextColor(TFT_RED, BLACK);
    lcd.drawString("SAVE FAILED - place not written", BOOKS_MARGIN, y); y += lh;
    lcd.setTextColor(WHITE, BLACK);
  }

  lcd.setTextColor(TFT_DARKGREY, BLACK);
  lcd.drawString("Sync identity", BOOKS_MARGIN, y); y += lh;
  lcd.setTextColor(WHITE, BLACK);
  for (int i = 0; i < nIds; i++) {
    lcd.drawFitString(ids[i], lcd.width() - 2 * BOOKS_MARGIN, BOOKS_MARGIN, y);
    y += lh;
  }
  /* KOSync's id and percentage, when it is on: the X4 and COVEY show the same document id
   * (the partial MD5) and a percentage that should read the same as this one. */
  if (kosyncConfigured() && ksIdsDone) {
    double kp = 0.0;
    if (epubKosyncPercentAt(&book, spine, pageStart, chapLen, &kp)) {
      snprintf(l, sizeof(l), "kosync %.12s.. %.1f%%", ksPartial[0] ? ksPartial : ksByName, kp * 100.0);
    } else if (epubKosyncTotal(book.kosync) == 0) {
      snprintf(l, sizeof(l), "kosync: no - %s", epubKosyncWhyNot(book.kosync));
    } else {
      snprintf(l, sizeof(l), "kosync: this place can't be sent");
    }
    lcd.drawFitString(l, lcd.width() - 2 * BOOKS_MARGIN, BOOKS_MARGIN, y);
    y += lh;
  }
  y += 4;
  lcd.setTextColor(TFT_DARKGREY, BLACK);
  snprintf(l, sizeof(l), "file %s", books[openIdx].path);
  lcd.drawFitString(l, lcd.width() - 2 * BOOKS_MARGIN, BOOKS_MARGIN, y);
}

/* One picture, as big as the panel allows. Worth its own screen: inline it competes with a
 * page of text for an already small display, and some of these are maps. */
void BooksApp::drawPicture() {
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  if (viewImage < 0 || viewImage >= nImages) {
    return;
  }

  SmoothFont* sf = fonts[AKROBAT_BOLD_16];
  const int capH = sf->height() + 2;
  const int boxH = bottom - top - capH - 2;

  bool drew = drawOneImage(viewImage, 0, top + 1, lcd.width(), (uint16_t)boxH);

  lcd.setTextFont(sf);
  lcd.setTextDatum(TL_DATUM);
  char cap[64];
  if (drew) {
    lcd.setTextColor(TFT_DARKGREY, BLACK);
    snprintf(cap, sizeof(cap), "%u x %u", (unsigned)images[viewImage].w,
             (unsigned)images[viewImage].h);
  } else {
    /* Say WHY rather than showing a black rectangle. The ESP32's ROM TJpgDec decodes
     * 3-component baseline JPEG only — greyscale and progressive files are refused, and that
     * is a property of the decoder in silicon, not a fault the reader can fix. */
    /* Greyscale is handled by jpeg_grey now, so what is left here is genuinely out of reach:
     * a progressive JPEG, or a PNG, neither of which either decoder reads. */
    lcd.setTextColor(TFT_ORANGE, BLACK);
    // Short enough to FIT: the 47-char version measured ~330-420px on a 240px
    // panel, so the reason it named was exactly the part that fell off-screen.
    snprintf(cap, sizeof(cap), "can't show (JPEG/PNG type)");
  }
  guiDrawEllipsized(lcd, cap, lcd.width() - 2 * BOOKS_MARGIN, BOOKS_MARGIN, bottom - capH);
}

/* Somebody else's place in this book, offered.
 *
 * ⚠ Offered, never taken. D-089 is explicit about this and the reason is clock skew: these
 * devices routinely have no NTP, so "newest wins" can silently throw away the reading you
 * actually did in favour of a stale packet with a confident timestamp. A person can see at a
 * glance whether 43% is ahead of where they are; an algorithm comparing two unreliable clocks
 * cannot. The suspect-clock line exists for exactly the case where the timestamp is nonsense.
 */
void BooksApp::drawSyncCard() {
  SmoothFont* f = fonts[AKROBAT_BOLD_18];
  SmoothFont* sf = fonts[AKROBAT_BOLD_16];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = f->height() + 2;

  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  if (pendingIdx < 0) {
    return;
  }
  lcd.setTextFont(f);
  lcd.setTextDatum(TL_DATUM);
  int y = top + 4;

  char l[96];
  lcd.setTextColor(TFT_GREENYELLOW, BLACK);
  snprintf(l, sizeof(l), "%s says:", pending.dev[0] ? pending.dev : "Another device");
  lcd.drawString(l, BOOKS_MARGIN, y); y += lh + 2;

  lcd.setTextColor(WHITE, BLACK);
  const double mine = fractionHere();
  const bool backward = pending.fraction < mine;
  int theirPct = (int)(pending.fraction * 100.0 + 0.5);
  /* A KOSync offer also shows the peer's OWN number — the one its screen shows — with this
   * phone's reading of the same place beside it ("61% (48% here)"): the X4 weighs chapters
   * by their size and this reader by their count, so the two are honestly different. The
   * comparisons below (backwards, the button) stay on this phone's scale. Same row, same
   * height; cut to the width rather than run under the margin. */
  double peerPct = 0.0;
  if (kosyncPeerPctFor(pendingId, &peerPct)) {
    snprintf(l, sizeof(l), "they are at %d%% (%d%% here)", (int)(peerPct * 100.0 + 0.5), theirPct);
  } else {
    snprintf(l, sizeof(l), "they are at %d%%", theirPct);
  }
  lcd.drawFitString(l, lcd.width() - 2 * BOOKS_MARGIN, BOOKS_MARGIN, y); y += lh;
  snprintf(l, sizeof(l), "you are at %d%%", (int)(mine * 100.0 + 0.5));
  lcd.drawString(l, BOOKS_MARGIN, y); y += lh + 6;

  lcd.setTextFont(sf);
  /* 🛑 A jump that goes BACKWARDS throws away reading already done, and there is no undo on
   * the wire — applyPending() writes the new place to the card at once. Both percentages were
   * always on screen, but "they are at 12 / you are at 26" reads as information rather than
   * as a warning, and at the end of an evening it is easy to miss entirely. Say it. */
  if (backward) {
    lcd.setTextColor(TFT_RED, BLACK);
    lcd.drawString("this moves you BACKWARDS", BOOKS_MARGIN, y);
    y += sf->height() + 4;
  }
  if (pendingClock) {
    // Flagged rather than refused, exactly as COVEY does: the position may still be the one
    // you want, but its timestamp cannot be trusted to decide that for you.
    lcd.setTextColor(TFT_ORANGE, BLACK);
    lcd.drawString("(their clock looks wrong)", BOOKS_MARGIN, y);
    y += sf->height() + 4;
  }
  lcd.setTextColor(TFT_DARKGREY, BLACK);
  lcd.drawString(backward ? "OK: go BACK to their place" : "OK: go there",
                 BOOKS_MARGIN, y); y += sf->height() + 2;
  lcd.drawString("Back: stay where I am", BOOKS_MARGIN, y);
}

// ---------------------------------------------------------------- states

void BooksApp::buildMenu() {
  menu = newMenu(NULL, fonts[AKROBAT_BOLD_18], 8);
  char l[48];
  menu->addOption("Resume reading", BOOKS_MENU_RESUME, 1);
  snprintf(l, sizeof(l), "Chapters (%d)...", isOpen ? book.nSpine : 0);
  menu->addOption(l, BOOKS_MENU_TOC, 1);
  static const char* SIZES[] = { "small", "medium", "large" };
  snprintf(l, sizeof(l), "Text size: %s", SIZES[fontIdx % BODY_FONT_COUNT]);
  menu->addOption(l, BOOKS_MENU_SIZE, 1);
  menu->addOption("Sync my place", BOOKS_MENU_SYNC, 1);
  if (pendingIdx >= 0) {
    snprintf(l, sizeof(l), "Go to %s's place (%d%%)",
             pending.dev[0] ? pending.dev : "their", (int)(pending.fraction * 100.0 + 0.5));
    menu->addOption(l, BOOKS_MENU_PENDING, 1);
  }
  if (undoOffered()) {
    snprintf(l, sizeof(l), "Undo the jump (back to %d%%)", undoPct);
    menu->addOption(l, BOOKS_MENU_UNDO, 1);
  }
  menu->addOption("Sync settings...", BOOKS_MENU_SYNCSET, 1);
  menu->addOption("Book info", BOOKS_MENU_INFO, 1);
  menu->addOption("Close book", BOOKS_MENU_CLOSE, 1);
  syncNoteSettle();                        // "Queued" -> "Sent"/"NOT sent" once the radio says
  if (syncNote[0]) {
    menu->addNote(syncNote);               // what the last send did, good or bad
  }
  if (ksNote[0]) {
    menu->addNoteWrapped(ksNote);          // why KOSync could not do what was asked
  }
  kosyncAddLines(menu);                    // the window's countdown and the home result
}

void BooksApp::buildToc() {
  menu = newMenu("No chapters", fonts[AKROBAT_BOLD_16], 11);
  if (!isOpen) {
    return;
  }
  SmoothFont* mf = fonts[AKROBAT_BOLD_16];       // the face this menu draws in
  char l[128], t[EPUB_CH_TITLE_MAX], shown[EPUB_CH_TITLE_MAX + 32];
  for (int i = 0; i < book.nSpine; i++) {
    epubChapterTitle(&book, i, t, sizeof(t));
    bookRenderRun(mf, t, strlen(t), shown, sizeof(shown));   // "Monkeys with ’Mechs"
    snprintf(l, sizeof(l), "%s%s", i == spine ? "> " : "", shown);
    menu->addOption(l, (MenuOption::keyType)(i + 1), 1);
  }
  menu->select((MenuOption::keyType)(spine + 1));
}

/* Put the book's title in the header, cut to fit.
 *
 * ⚠ `HeaderWidget::redraw` draws the title with a plain `drawString` and NO width limit, then
 * paints the battery, WiFi and message icons and the clock over the right-hand end of it.
 * Every other app gets away with that by having a short title; a book title does not, and
 * "BattleTech: Ghosts of Timkovichi: (A Ghost Dogs Story)" ran straight under the clock. */
void BooksApp::setHeaderToBookTitle() {
  const char* src = (isOpen && book.title[0]) ? book.title : "Reading";
  SmoothFont* hf = fonts[AKROBAT_BOLD_18];       // the face the header draws in

  char shown[sizeof(headerTitle)];
  bookRenderRun(hf, src, strlen(src), shown, sizeof(shown));

  // 8px left margin, and roughly this much reserved on the right for clock + icons.
  const int budget = (int)lcd.width() - 8 - 104;
  if ((int)hf->textWidth(shown) > budget && budget > 0) {
    int16_t fit = hf->fitTextLength(shown, (uint16_t)(budget - (int)hf->textWidth("..")));
    if (fit < 1) {
      fit = 1;
    }
    if ((size_t)fit < strlen(shown)) {
      shown[fit] = '\0';
      strncat(shown, "..", sizeof(shown) - strlen(shown) - 1);
    }
  }
  snprintf(headerTitle, sizeof(headerTitle), "%s", shown);
  header->setTitle(headerTitle);                 // the header keeps this pointer
}

void BooksApp::enterState(BooksState_t state) {
  appState = state;
  freeWidgets();

  /* ⚠ SET WHAT THIS SCREEN NEEDS, do not inherit it. msAppTimerEventPeriod is device-wide,
   * and apps routinely leave their own value in it (the Game Boy leaves 33 ms, others 25).
   * Defaulted here rather than written into all twelve cases below, so a state added later
   * gets "no timer" instead of whatever the previous screen happened to want — this is the
   * shared-state bug class that produced four faults in one day. The two screens that do
   * need a tick override it. */
  controlState.msAppTimerEventPeriod = 0;

  switch (state) {
  case BOOKS_LIB:
    holdScreenAwake(false);
    header->setTitle("Books");
    footer->setButtons("Select", "Back");
    buildLibrary();
    break;
  case BOOKS_MANAGE:
    pendingDelete = -1;
    header->setTitle("Manage books");
    footer->setButtons("Delete", "Back");
    buildManage();
    break;
  case BOOKS_XFER:
    xferClean = false;
    controlState.msAppTimerEventPeriod = 1000;   // live "books added" count
    header->setTitle("Add books");
    footer->setButtons(xferOn() ? "Stop" : "Start", "Back");
    break;
  case BOOKS_HELP:
    helpTop = 0;
    header->setTitle("How to read");
    footer->setButtons("", "Back");
    break;
  case BOOKS_READ:
    holdScreenAwake(true);
    /* One second, for one comparison: the reading screen watches the booksync inbox so a
     * position that lands while the book is open is offered where you are, rather than
     * waiting for the book to be closed and opened again. The tick does nothing at all
     * unless bookSyncInboxSeq() has moved. */
    controlState.msAppTimerEventPeriod = 1000;
    /* ⚠ Force Numeric. `ControlState::inputType` is a MODE that persists across screens, and
     * the Meshtastic app leaves it on AlphaNum after composing — in which case the GUI runs
     * every key through the multi-tap decoder and pressing 2 yields 'a', not '2'. The number
     * keys that open a picture would then work or not depending on which app you had used
     * earlier, which is the worst kind of bug to be told about. */
    controlState.setInputState(InputType::Numeric);
    setHeaderToBookTitle();
    footer->setButtons("Menu", "Close");
    break;
  case BOOKS_MENU:
    header->setTitle("Reading");
    footer->setButtons("Select", "Back");
    buildMenu();
    armMenuTick();         // KOSync's live lines + the card, or a sync frame's answer (after buildMenu)
    break;
  case BOOKS_TOC:
    header->setTitle("Chapters");
    footer->setButtons("Go", "Back");
    buildToc();
    break;
  case BOOKS_INFO:
    header->setTitle("Book info");
    footer->setButtons("", "Back");
    break;
  case BOOKS_PICTURE:
    header->setTitle("Picture");
    footer->setButtons("", "Back");
    break;
  case BOOKS_SYNCCARD: {
    /* 🛑 HOLD THE SCREEN. This was the only Books screen that did not, and it is the one
     * screen that exists to be READ before answering — so on the library→openBook path (the
     * hold is released entering BOOKS_LIB and BOOKS_READ never runs) it dimmed at 20 s and
     * slept at 30 s while an undecided card sat there. Worse than untidy: in the 20-30 s DIM
     * band a keypress is still delivered to the app (GUI.cpp swallows it only once brightness
     * has reached 0), and by then millis() - syncCardMs is ~20000, far past the arming window
     * — so the press meant only to bring the screen back TOOK THE JUMP. Holding here removes
     * the dim band entirely, which is why the arming window below can stay a simple one.
     * Balanced: BooksApp::holdScreenAwake is idempotent on timeoutsHeld, BOOKS_READ holds too
     * (a no-op from here), and BOOKS_LIB and the destructor release. */
    holdScreenAwake(true);
    header->setTitle("Sync");
    syncCardMs = millis();       // arms "Go there" — see BOOKS_SYNCCARD_ARM_MS
    /* Name the direction on the BUTTON, not just in the body text. A jump that moves you
     * FORWARD is the ordinary case and needs no ceremony; one that moves you BACK throws
     * away reading you have already done, and the two used to be the same word. */
    const bool backward = isOpen && pendingIdx >= 0 && pending.fraction < fractionHere();
    footer->setButtons(backward ? "Go BACK" : "Go there", "Stay");
    break;
  }
  case BOOKS_SYNCSET:
    header->setTitle("Sync settings");
    footer->setButtons("Select", "Back");
    kosyncReloadConfig();              // looking at the settings re-reads the file
    if (kosyncConfigured()) {
      controlState.msAppTimerEventPeriod = 1000;
    }
    buildSyncSettings();
    break;
  case BOOKS_SYNCEDIT:
    header->setTitle(editingPass ? "Passcode" : "Device name");
    footer->setButtons("Save", "Clear");        // Back is backspace here; END cancels
    buildSyncEdit();
    break;
  }
}

/* The two secrets and the name that travels with a position. The passcode is shown in full
 * on purpose: it has to be typed identically into COVEY, and hiding it behind asterisks makes
 * "the two do not match" — a failure that is completely silent on the wire — impossible to
 * diagnose by looking. */
void BooksApp::buildSyncSettings() {
  menu = newMenu(NULL, fonts[AKROBAT_BOLD_18], 8);
  char l[80];
  snprintf(l, sizeof(l), "Passcode: %s", syncPass[0] ? syncPass : "(none)");
  menu->addOption(l, BOOKS_SET_PASS, 1);
  snprintf(l, sizeof(l), "This device: %s", syncDev);
  menu->addOption(l, BOOKS_SET_DEV, 1);

  // Whether the transport even exists. Without the channel there is nowhere legal to send.
  bool haveCh = false;
  int n = meshService.getChannelCount();
  for (int i = 0; i < n; i++) {
    const MeshChannel* c = meshService.getChannel(i);
    if (c && strcasecmp(c->name, "booksync") == 0) {
      haveCh = true;
    }
  }
  menu->addNote(haveCh ? "Channel 'booksync': found"
                       : "Channel 'booksync': MISSING");
  snprintf(l, sizeof(l), "Parked positions: %d", bookSyncInboxCount());
  menu->addNote(l);

  /* KOSync: what /books/kosync.txt configures, SHOWN, never edited here (a file keeps the
   * password out of a public repo's UI, as smsmirror.txt does) — and never the password. */
  const KosyncConfig* kc = kosyncConfig();
  if (!kosyncConfigured()) {
    snprintf(l, sizeof(l), "KOSync: %s", kosyncConfigNote());
    menu->addNoteWrapped(l);
  } else {
    snprintf(l, sizeof(l), "KOSync: on, user %s", kc->user);
    menu->addNoteWrapped(l);
    if (kc->home[0]) {
      snprintf(l, sizeof(l), "Home: %s:%u", kc->home, (unsigned)kc->homePort);
    } else {
      snprintf(l, sizeof(l), "Home: (none - hotspot window only)");
    }
    menu->addNoteWrapped(l);
    snprintf(l, sizeof(l), "KOSync name: %s", kosyncMyDevice());
    menu->addNoteWrapped(l);
    snprintf(l, sizeof(l), "On close: %s. On open off WiFi: %s",
             kc->autoOnClose ? "window + send" : "nothing",
             kc->openWindow ? "60 s window" : "nothing");
    menu->addNoteWrapped(l);
    char hs[80];
    kosyncHotspotLine(hs, sizeof(hs));       // "hotspot: WPA2" - never the password itself
    snprintf(l, sizeof(l), "%c%s", hs[0] >= 'a' && hs[0] <= 'z' ? hs[0] - 32 : hs[0], hs + 1);
    menu->addNoteWrapped(l);
    if (kosyncConfigNote()[0]) {
      snprintf(l, sizeof(l), "! %s", kosyncConfigNote());   // e.g. a short password
      menu->addNoteWrapped(l);
    }
    kosyncAddLines(menu);
  }
}

appEventResult BooksApp::processEvent(EventType event) {
  /* ⚠ THERE IS DELIBERATELY NO POWER_OFF_EVENT ARM HERE. It looks like the natural place to
   * commit the position, and it is useless: all three dispatchers (WiPhone.ino:1195, :3557,
   * :3606) call powerOff() BEFORE processEvent, so anything written from here races a rail
   * that is already dropping — and it duplicated the pre-latch write that now happens at
   * those call sites via booksSaveOpenPosition(). Save before the latch, not after it. */
  switch (appState) {

  case BOOKS_LIB:
    if (LOGIC_BUTTON_BACK(event)) {
      return EXIT_APP;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->currentKey();
      if (sel == MENU_ROW_NOTE) {          // "(no books yet)" / why a book would not open
        return REDRAW_SCREEN;
      }
      if (sel == BOOKS_ROW_ADD) {
        enterState(BOOKS_XFER);
        return REDRAW_ALL;
      }
      if (sel == BOOKS_ROW_MANAGE) {
        enterState(BOOKS_MANAGE);
        return REDRAW_ALL;
      }
      if (sel == BOOKS_ROW_HELP) {
        enterState(BOOKS_HELP);
        return REDRAW_ALL;
      }
      if (sel >= BOOKS_ROW_FIRST) {
        int idx = (int)sel - BOOKS_ROW_FIRST;
        if (openBook(idx)) {
          libNote[0] = '\0';
          /* openBook() has already asked. If somebody's place was waiting, SAY SO: burying
           * it in a menu row meant a position could arrive, be found, and still be invisible
           * to anyone who did not think to press Menu. Declining lands on the page. */
          enterState(pendingIdx >= 0 ? BOOKS_SYNCCARD : BOOKS_READ);
        } else {
          // Say so in the list itself: a book that silently does nothing when you press OK
          // is the worst possible failure, and this one has real causes (a truncated
          // download, a DRM'd file, no PSRAM free).
          snprintf(libNote, sizeof(libNote), "! could not open %s", books[idx].name);
          freeWidgets();
          buildLibrary();
          menu->select(sel);
        }
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case BOOKS_MANAGE:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(BOOKS_LIB);
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->currentKey();
      if (sel == BOOKS_ROW_TIDY) {
        SD.mkdir(BOOKS_DIR);
        for (int i = 0; i < bookCount; i++) {
          if (!isStray(i)) {
            continue;
          }
          char dest[128];
          snprintf(dest, sizeof(dest), "%s/%s", BOOKS_DIR, books[i].name);
          if (!SD.exists(dest)) {          // never overwrite a book already there
            SD.rename(books[i].path, dest);
          }
        }
        pendingDelete = -1;
        scanBooks();
        freeWidgets();
        buildManage();
        return REDRAW_SCREEN;
      }
      int idx = (int)sel - BOOKS_ROW_FIRST;
      if (idx >= 0 && idx < bookCount) {
        if (pendingDelete == idx) {
          SD.remove(books[idx].path);
          pendingDelete = -1;
          scanBooks();
        } else {
          pendingDelete = idx;         // destructive: a second press confirms
        }
        freeWidgets();
        buildManage();
        if (pendingDelete >= 0) {
          menu->select(sel);
        }
        return REDRAW_SCREEN;
      }
    }
    return REDRAW_SCREEN;

  case BOOKS_XFER:
    if (LOGIC_BUTTON_BACK(event)) {
      xferStop();
      controlState.msAppTimerEventPeriod = 0;
      scanBooks();                     // anything that arrived is in the list now
      enterState(BOOKS_LIB);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event)) {
      if (xferOn()) {
        xferStop();
      } else {
        xferStart(&BOOKS_XFER_CFG);
      }
      xferClean = false;
      footer->setButtons(xferOn() ? "Stop" : "Start", "Back");
      return REDRAW_ALL;
    }
    if (event == APP_TIMER_EVENT) {
      return xferOn() ? REDRAW_SCREEN : DO_NOTHING;
    }
    return DO_NOTHING;

  case BOOKS_HELP:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(BOOKS_LIB);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_UP) {
      helpTop--;
      return REDRAW_SCREEN;
    }
    if (event == WIPHONE_KEY_DOWN) {
      helpTop++;
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;

  case BOOKS_READ:
    if (LOGIC_BUTTON_BACK(event)) {
      closeBookByUser();
      scanBooks();
      enterState(BOOKS_LIB);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event)) {
      savePosition(true);            // a natural pause: commit the place to the card
      enterState(BOOKS_MENU);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_RIGHT || event == WIPHONE_KEY_F4) {
      nextPage();
      return REDRAW_SCREEN;
    }
    if (event == WIPHONE_KEY_UP || event == WIPHONE_KEY_LEFT || event == WIPHONE_KEY_F3) {
      prevPage();
      return REDRAW_SCREEN;
    }
    /* A position arriving while you are reading. Nothing here runs unless the inbox has
     * actually changed, and the card is only raised for a position that belongs to THIS book
     * and verifies under the passcode — checkForPending() decides both. Declining it removes
     * the offer (see BOOKS_SYNCCARD), so this cannot turn into a prompt on every page. */
    if (event == APP_TIMER_EVENT) {
      if (syncSeqSeen != bookSyncInboxSeq()) {
        checkForPending();
        if (pendingIdx >= 0) {
          enterState(BOOKS_SYNCCARD);
          return REDRAW_ALL;
        }
      }
      return DO_NOTHING;
    }
    // A number key opens the picture captioned with that number on this page.
    if (event >= '1' && event <= '9') {
      int want = event - '0';
      for (int i = 0; i < nImages && i < EPUB_MAX_IMAGES; i++) {
        if (pageImageKey[i] == want) {
          viewImage = i;
          enterState(BOOKS_PICTURE);
          return REDRAW_ALL;
        }
      }
      return DO_NOTHING;
    }
    return DO_NOTHING;

  case BOOKS_MENU:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(BOOKS_READ);
      return REDRAW_ALL;
    }
    /* The menu's tick (armMenuTick): KOSync's, and "Sync my place"'s frame until the radio
     * answers. With KOSync, a peer's place that lands while this menu is up raises the card
     * HERE — "Sync my place" was just pressed on this very screen, and the reply is what the
     * person is waiting for — and the window's countdown and the home line refresh. Without
     * KOSync the card is left to the reading screen as it always was (the tick here is only
     * the second or two of the frame's watch). The menu is rebuilt only when its text changed. */
    if (event == APP_TIMER_EVENT) {
      if (kosyncConfigured() && syncSeqSeen != bookSyncInboxSeq()) {
        checkForPending();
        if (pendingIdx >= 0) {
          enterState(BOOKS_SYNCCARD);
          return REDRAW_ALL;
        }
      }
      /* The sync note's frame: rebuilt when the radio has reported it (buildMenu() re-words). */
      const bool noteMoved = syncTxId && meshService.txOutcome(syncTxId) != MESH_TXO_QUEUED;
      /* (UNKNOWN counts as moved once: the rebuild's syncNoteSettle() then stops asking.) */
      if (menu && (kosyncLinesChanged() || noteMoved)) {
        const MenuOption::keyType k = menu->currentKey();
        freeWidgets();
        buildMenu();
        menu->select(k);
        armMenuTick();                       // the answer is in: the watch stands down
        return REDRAW_SCREEN;
      }
      armMenuTick();
      return DO_NOTHING;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      switch (menu->currentKey()) {
      case BOOKS_MENU_RESUME:
        enterState(BOOKS_READ);
        return REDRAW_ALL;
      case BOOKS_MENU_TOC:
        enterState(BOOKS_TOC);
        return REDRAW_ALL;
      case BOOKS_MENU_SIZE: {
        /* Changing size re-flows the page but must NOT move the reader: the position is a
         * place in the text, not a page number. That is the same property that lets COVEY
         * make sense of a position from here. The history is dropped because those page
         * starts belong to the old geometry. */
        fontIdx = (fontIdx + 1) % BODY_FONT_COUNT;
        histN = 0;
        freeWidgets();
        buildMenu();
        menu->select(BOOKS_MENU_SIZE);
        return REDRAW_SCREEN;
      }
      case BOOKS_MENU_SYNC:
        syncMyPlace();
        freeWidgets();
        buildMenu();
        menu->select(BOOKS_MENU_SYNC);
        armMenuTick();                       // "Queued" -> "Sent" on THIS visit (KOSync or not)
        return REDRAW_SCREEN;
      case BOOKS_MENU_PENDING:
        if (pendingIdx >= 0) {
          enterState(BOOKS_SYNCCARD);
          return REDRAW_ALL;
        }
        return REDRAW_SCREEN;
      case BOOKS_MENU_UNDO:
        applyUndo();
        enterState(BOOKS_READ);
        return REDRAW_ALL;
      case BOOKS_MENU_SYNCSET:
        enterState(BOOKS_SYNCSET);
        return REDRAW_ALL;
      case BOOKS_MENU_INFO:
        enterState(BOOKS_INFO);
        return REDRAW_ALL;
      case BOOKS_MENU_CLOSE:
        closeBookByUser();
        enterState(BOOKS_LIB);
        return REDRAW_ALL;
      default:
        break;
      }
    }
    return REDRAW_SCREEN;

  case BOOKS_TOC:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(BOOKS_MENU);
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      int i = (int)menu->currentKey() - 1;
      if (i >= 0 && isOpen && i < book.nSpine) {
        loadChapter(i);
        savePosition(true);
        enterState(BOOKS_READ);
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case BOOKS_INFO:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(BOOKS_MENU);
      return REDRAW_ALL;
    }
    return DO_NOTHING;

  case BOOKS_PICTURE:
    if (LOGIC_BUTTON_BACK(event) || LOGIC_BUTTON_OK(event)) {
      viewImage = -1;
      enterState(BOOKS_READ);            // straight back to the page it came from
      return REDRAW_ALL;
    }
    return DO_NOTHING;

  case BOOKS_SYNCCARD:
    if (LOGIC_BUTTON_OK(event)) {
      /* 🛑 NOT the keystroke that put this card on the screen. Opening a book is OK in the
       * library and a parked position lands you here instead of on the page, so the reflex
       * second press was taking a position nobody had read the offer for — and applyPending()
       * writes it to the card and retires every parked offer, so there was nothing left to
       * show it had happened. Swallow anything inside the arming window; a deliberate press
       * is always later than that. */
      if (millis() - syncCardMs < BOOKS_SYNCCARD_ARM_MS) {
        return REDRAW_SCREEN;
      }
      applyPending();                    // only ever from a deliberate press
      enterState(BOOKS_READ);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_BACK(event)) {
      /* Staying put drops the offer rather than leaving it to ask again on every page —
       * a prompt you have already declined is nagging, not syncing. */
      if (pendingIdx >= 0) {
        kosyncCardAnswered(pendingId);   // declined: a home record parked as THIS card (KS-2)
        dropParkedForThisBook();      // declining is a decision about the book, not one packet
        pendingIdx = -1;
        pendingId = 0;
      }
      enterState(BOOKS_READ);
      return REDRAW_ALL;
    }
    return DO_NOTHING;

  case BOOKS_SYNCSET:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(isOpen ? BOOKS_MENU : BOOKS_LIB);
      return REDRAW_ALL;
    }
    if (event == APP_TIMER_EVENT) {       // the KOSync window/home lines, live (see BOOKS_MENU)
      if (menu && kosyncLinesChanged()) {
        const MenuOption::keyType k = menu->currentKey();
        freeWidgets();
        buildSyncSettings();
        menu->select(k);
        return REDRAW_SCREEN;
      }
      return DO_NOTHING;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->currentKey();
      if (sel == BOOKS_SET_PASS || sel == BOOKS_SET_DEV) {
        editingPass = (sel == BOOKS_SET_PASS);
        enterState(BOOKS_SYNCEDIT);
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case BOOKS_SYNCEDIT:
    if (event == WIPHONE_KEY_END) {        // cancel (Back is backspace in a text field)
      enterState(BOOKS_SYNCSET);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event)) {
      const char* t = textArea ? textArea->getText() : NULL;
      if (t) {
        if (editingPass) {
          snprintf(syncPass, sizeof(syncPass), "%s", t);
        } else if (t[0]) {
          snprintf(syncDev, sizeof(syncDev), "%s", t);
        }
        saveSyncSettings();
        checkForPending();     // a new passcode may make a parked packet verify
      }
      enterState(BOOKS_SYNCSET);
      return REDRAW_ALL;
    }
    if (IS_KEYBOARD(event) && textArea) {
      textArea->processEvent(event);
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;
  }

  return DO_NOTHING;
}

void BooksApp::redrawScreen(bool redrawAll) {
  switch (appState) {
  case BOOKS_READ:
    drawPage();
    break;
  case BOOKS_HELP:
    drawHelp();
    break;
  case BOOKS_XFER:
    drawXfer();
    break;
  case BOOKS_INFO:
    drawInfo();
    break;
  case BOOKS_PICTURE:
    drawPicture();
    break;
  case BOOKS_SYNCCARD:
    drawSyncCard();
    break;
  case BOOKS_SYNCEDIT:
    if (textArea) {
      ((GUIWidget*)textArea)->redraw(lcd);
    }
    break;
  default:
    if (menu) {
      ((GUIWidget*)menu)->redraw(lcd);
    }
    break;
  }
}
