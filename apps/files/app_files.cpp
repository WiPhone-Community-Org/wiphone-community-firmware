/*
 * app_files.cpp — see app_files.h for what this is and what it deliberately is not.
 *
 * Memory discipline (the whole design fits it):
 *   - the entry table is ONE PSRAM block, capped at FILES_MAX_ENTRIES — a folder with ten
 *     thousand files lists the first two hundred and SAYS it truncated;
 *   - the viewer buffer is ONE PSRAM block, capped at FILES_VIEW_MAX — a bigger file shows
 *     its head and says so;
 *   - menu widgets are PSRAM-backed already (AbstractWidget::operator new).
 *   Nothing here allocates from internal heap in a loop.
 */
#include "app_files.h"
#include "app_books.h"     // bookRenderRun + fontMeasure, shared with the e-reader
#include "config.h"
#include "files_paths.h"   // within / join / reroot: the safety of the folder operations
#include "music_player.h"  // a shorter slice while music plays: the I2S DMA holds ~90 ms
#include "tile_fetch.h"    // a running map download owns its area: no delete/move under it
#include "elev_tiles.h"    // ELEV_DIR: ...and /maps/elev when it fetches the elevation layer too
#include "Audio.h"
#include <new>
#include <unistd.h>        // unlink / rmdir straight through the VFS: no stat+open first
#include <errno.h>

#include <Arduino.h>
#include <SD.h>
#include <stdarg.h>

static const int    FILES_MAX_ENTRIES = 200;
static const size_t FILES_PATH_MAX_C  = 216;   // FILES_PATH_MAX + the mount point
static const size_t FILES_VIEW_MAX    = 60 * 1024;
static const int    FILES_MARGIN      = 6;

// Menu keys. 0 is never used (readChosen's "nothing" value).
static const MenuOption::keyType ROW_UPLOAD    = 1;
static const MenuOption::keyType ROW_UP        = 2;
static const MenuOption::keyType ROW_PASTE     = 3;
static const MenuOption::keyType ROW_CLIPCLEAR = 4;
static const MenuOption::keyType ROW_THISDIR   = 5;   // "[ This folder... ]": its options
static const MenuOption::keyType ROW_FIRST     = 10;

/* A job works for this long per timer tick, then hands the loop back: the keypad, WiFi and
 * the mesh keep running between slices, and Back can stop it. Copying streams this much of
 * a file per slice. */
static const uint32_t FILES_JOB_SLICE_MS = 80;
static const uint32_t FILES_JOB_SLICE_MUSIC_MS = 35;   // the music DMA runs dry past ~90 ms
static const size_t   FILES_COPY_CHUNK   = 32 * 1024;  // 128 KB tiles: four chunks each

/* The card's VFS mount point: SD.remove() is a stat, an open, a close and THEN the unlink
 * (vfs_api.cpp), which made a delete ~90 ms a file; the POSIX call is the unlink alone. */
static const char* FILES_SD_MOUNT = "/sd";
extern Audio* audio;       // pumped from inside a slice while music plays (see stepJob)

/* Is a map download writing under (or above) this path — RUNNING, or WAITING to resume there
 * (0.9.78: a stopped download resumes by itself)? Its worker task creates folders and writes
 * tiles the whole time it runs, and it outlives the Maps app (serial `maps dl` needs no app at
 * all): a delete or move racing it would unlink a tile between its write pieces and rmdir a
 * folder it is about to mkdir again (review, 2026-09-20). A waiting one would come back in the
 * middle of the delete. `*waiting` says which, for the words. */
static bool downloadWritesUnder(const char* path, bool* waiting) {
  bool running = false;
  const char* key = tileFetchJobKey(&running);
  if (!key) {
    return false;
  }
  if (waiting) *waiting = !running;
  /* Case-blind: the card is FAT, so the job writing "/maps/elev" writes into a folder spelt
   * "Elev" or "ELEV" just the same (files_paths.h, filePathWithinNoCase). */
  char root[64];
  snprintf(root, sizeof(root), "%s/%s", TILE_MAPS_ROOT, key);
  if (filePathWithinNoCase(root, path) || filePathWithinNoCase(path, root)) {
    return true;
  }
  /* 0.9.80: a job with "Elevation too" writes /maps/elev as well - first in every run, a resume
   * included - so that folder is the job's too while it runs or waits. */
  if (tileFetchJobElev()) {
    snprintf(root, sizeof(root), "%s/%s", TILE_MAPS_ROOT, ELEV_DIR);
    return filePathWithinNoCase(root, path) || filePathWithinNoCase(path, root);
  }
  return false;
}
static const char* downloadRefusal(bool waiting) {
  return waiting ? "A map download will resume there - stop it first (Maps > Download)"
                 : "A map download is writing there - stop it first (Maps menu)";
}

/* A folder copy / move / delete is running: a map download neither starts nor resumes while
 * it does (tile_fetch.h). A COUNT only reads, and does not count. Set by startJob, cleared by
 * freeJob — every way a job ends goes through freeJob. */
static volatile bool s_treeJob = false;
bool filesJobActive() {
  return s_treeJob;
}
static bool sdUnlink(const char* path) {
  char p[FILES_PATH_MAX_C];
  snprintf(p, sizeof(p), "%s%s", FILES_SD_MOUNT, path);
  return ::unlink(p) == 0;
}
static bool sdRmdir(const char* path) {
  char p[FILES_PATH_MAX_C];
  snprintf(p, sizeof(p), "%s%s", FILES_SD_MOUNT, path);
  return ::rmdir(p) == 0;
}

// Options-menu keys.
static const MenuOption::keyType OPT_OPEN   = 1;
static const MenuOption::keyType OPT_COPY   = 2;
static const MenuOption::keyType OPT_MOVE   = 3;
static const MenuOption::keyType OPT_DELETE = 4;
static const MenuOption::keyType OPT_CANCEL = 5;
/* ⚠ NOT 1 and 2: the browse menu uses those for [Upload] and [..]. A confirm that fell back to
 * the browse menu (a failed job start) would otherwise read "[..]" as "Yes" (review). */
static const MenuOption::keyType DEL_NO     = 101;
static const MenuOption::keyType DEL_YES    = 102;

/* THE CLIPBOARD — file-scope statics, deliberately: "a persistent paste that stays until
 * I've pasted" should survive backing out of the app entirely. It is only a remembered
 * PATH: marking a file for Copy or Move touches NOTHING on the card. A Move becomes real
 * only when the paste succeeds — rename first (atomic on FAT), else copy + verify + only
 * then delete — so a mark that is never pasted can never break the file. */
static char    s_clipSrc[208] = {0};
static char    s_clipName[64] = {0};
static uint8_t s_clipMove = 0;         // 0 = nothing held; 1 = copy; 2 = move
static bool    s_clipIsDir = false;    // a folder: paste is a rename (move) or a tree copy

/* File extensions the text viewer renders. Everything else opens as an info page rather
 * than a garbage dump — and a text file with a weird extension still gets sniffed in. */
static bool textishName(const char* name) {
  const char* dot = strrchr(name, '.');
  if (!dot) {
    return false;
  }
  static const char* exts[] = { ".txt", ".log", ".ini", ".md", ".cfg", ".conf", ".json",
                                ".csv", ".xml", ".cue", ".m3u", ".since", ".nfo" };
  for (unsigned i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
    if (!strcasecmp(dot, exts[i])) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- lifecycle

FilesApp::FilesApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(disp, state, header, footer) {
  log_d("create FilesApp");
  strcpy(curPath, "/");
  note[0] = '\0';
  entries = (FEntry*)ps_malloc(sizeof(FEntry) * FILES_MAX_ENTRIES);
  if (!entries) {
    log_e("FILES: no PSRAM for the entry table");
  }
  header->setTitle("Files");
  footer->setButtons("Select", "Back");
  scanDir();
  buildBrowse();
}

FilesApp::~FilesApp() {
  log_d("destroy FilesApp");
  if (appState == FILES_XFER) {
    xferStop();                    // never leave the server up with no screen owning it
  }
  if (job) {
    endJob(true);                  // a job cut short by leaving: handles closed, no orphans
  }
  closeView();
  delete menu;
  free(entries);
}

// ---------------------------------------------------------------- browsing

void FilesApp::scanDir() {
  entryCount = 0;
  truncated = false;
  if (!entries) {
    return;
  }
  File dir = SD.open(curPath);
  if (!dir || !dir.isDirectory()) {
    log_e("FILES: cannot open %s", curPath);
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
    if (base[0] && base[0] != '.') {           // hidden entries stay hidden
      if (entryCount >= FILES_MAX_ENTRIES) {
        truncated = true;
        f.close();
        break;
      }
      FEntry& e = entries[entryCount++];
      snprintf(e.name, sizeof(e.name), "%s", base);
      e.isDir = f.isDirectory();
      e.size = e.isDir ? 0 : (uint32_t)f.size();
    }
    f.close();
  }
  dir.close();

  // Dirs first, then case-insensitive alphabetical — so the listing is stable and a
  // folder never reshuffles under a repeat visit. Insertion sort: 200 entries max.
  for (int i = 1; i < entryCount; i++) {
    FEntry key = entries[i];
    int j = i - 1;
    while (j >= 0 && (( !entries[j].isDir && key.isDir) ||
                      (entries[j].isDir == key.isDir &&
                       strcasecmp(entries[j].name, key.name) > 0))) {
      entries[j + 1] = entries[j];
      j--;
    }
    entries[j + 1] = key;
  }
}

void FilesApp::buildBrowse() {
  delete menu;
  menu = new MenuWidget(0, header->height(), lcd.width(),
                        lcd.height() - header->height() - footer->height(),
                        "(empty folder)", fonts[AKROBAT_BOLD_18], 9, 8);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);

  snprintf(headerTitle, sizeof(headerTitle), "%s", curPath);
  header->setTitle(headerTitle);

  if (note[0]) {
    menu->addNoteWrapped(note);        // the result of the last operation, whole
  }
  if (s_clipSrc[0]) {
    char prow[96];
    snprintf(prow, sizeof(prow), "[ Paste \"%s%s\" here ]", s_clipName, s_clipIsDir ? "/" : "");
    menu->addOption(prow, ROW_PASTE, 1);
    menu->addOption("[ Cancel paste ]", ROW_CLIPCLEAR, 1);
  }
  menu->addOption("[ Upload into this folder ]", ROW_UPLOAD, 1);
  if (strcmp(curPath, "/") != 0) {
    menu->addOption("[..]", ROW_UP, 1);
    /* The folder you are standing in, as a thing you can act on. OK on a folder row enters
     * it (browsing stays one press a level), so this row is where Copy / Move / Delete for
     * a FOLDER live. Not at the root: the card itself is not a thing to delete. */
    menu->addOption("[ This folder... ]", ROW_THISDIR, 1);
  }
  char label[80];
  for (int i = 0; i < entryCount; i++) {
    if (entries[i].isDir) {
      snprintf(label, sizeof(label), "%s/", entries[i].name);
    } else if (entries[i].size >= 10240) {
      snprintf(label, sizeof(label), "%s  %luK", entries[i].name,
               (unsigned long)(entries[i].size / 1024));
    } else {
      snprintf(label, sizeof(label), "%s  %luB", entries[i].name,
               (unsigned long)entries[i].size);
    }
    menu->addOption(label, (MenuOption::keyType)(ROW_FIRST + i), 1);
  }
  if (truncated) {
    menu->addNote("(more files not listed)");
  }
}

void FilesApp::fullPathOf(int idx, char* out, size_t cap) const {
  snprintf(out, cap, "%s%s%s", curPath,
           (strcmp(curPath, "/") == 0) ? "" : "/", entries[idx].name);
}

void FilesApp::setNote(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(note, sizeof(note), fmt, ap);
  va_end(ap);
}

void FilesApp::buildOptions() {
  delete menu;
  menu = new MenuWidget(0, header->height(), lcd.width(),
                        lcd.height() - header->height() - footer->height(),
                        NULL, fonts[AKROBAT_BOLD_18], 9, 8);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  if (folderOptions) {
    snprintf(headerTitle, sizeof(headerTitle), "%s/", filePathBase(curPath));
    header->setTitle(headerTitle);
    menu->addOption("Copy this folder", OPT_COPY, 1);
    menu->addOption("Move this folder", OPT_MOVE, 1);
    menu->addOption("Delete this folder...", OPT_DELETE, 1);
    menu->addOption("Cancel", OPT_CANCEL, 1);
    return;
  }
  snprintf(headerTitle, sizeof(headerTitle), "%s", entries[selIdx].name);
  header->setTitle(headerTitle);
  menu->addOption("Open", OPT_OPEN, 1);
  menu->addOption("Copy", OPT_COPY, 1);
  menu->addOption("Move", OPT_MOVE, 1);
  menu->addOption("Delete...", OPT_DELETE, 1);
  menu->addOption("Cancel", OPT_CANCEL, 1);
}

void FilesApp::buildConfirmDel() {
  delete menu;
  menu = new MenuWidget(0, header->height(), lcd.width(),
                        lcd.height() - header->height() - footer->height(),
                        NULL, fonts[AKROBAT_BOLD_18], 9, 8);
  menu->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  if (folderOptions) {
    snprintf(headerTitle, sizeof(headerTitle), "Delete %s/?", filePathBase(curPath));
    header->setTitle(headerTitle);
    menu->addOption("Cancel", DEL_NO, 1);
    menu->addOption("Yes, delete all of it", DEL_YES, 1);
    /* What "all of it" is, counted while you read the question (the COUNT job; this menu
     * is rebuilt when the numbers move). "Delete home/?" alone does not say 271 files. */
    char row[96];
    if (job && job->kind == JOB_COUNT) {
      const unsigned long kb = (unsigned long)(job->bytes >> 10);
      if (kb >= 1024) {
        snprintf(row, sizeof(row), "%s%lu files, %lu folders, %lu MB", job->done ? "" : "Counting... ",
                 (unsigned long)job->files, (unsigned long)job->dirs, kb >> 10);
      } else {
        snprintf(row, sizeof(row), "%s%lu files, %lu folders, %lu KB", job->done ? "" : "Counting... ",
                 (unsigned long)job->files, (unsigned long)job->dirs, kb);
      }
      menu->addNoteWrapped(row);
      if (job->errors) {
        snprintf(row, sizeof(row), "(%lu could not be read: %s - the count is a floor)",
                 (unsigned long)job->errors, job->err);
        menu->addNoteWrapped(row);
      }
    } else if (!job) {
      // The COUNT could not start (startJob said why in `note`): the question still
      // stands, but the screen has to say why it cannot say what "all of it" is.
      menu->addNoteWrapped(note[0] ? note : "Could not count what is inside");
    }
    return;
  }
  snprintf(headerTitle, sizeof(headerTitle), "Delete %s?", entries[selIdx].name);
  header->setTitle(headerTitle);
  menu->addOption("Cancel", DEL_NO, 1);        // first, so a hasty OK is harmless
  menu->addOption("Yes, delete this file", DEL_YES, 1);
}

void FilesApp::markClipboard(int idx, bool move) {
  if (idx < 0) {
    // The folder itself (the "[ This folder... ]" row): curPath, not an entry.
    snprintf(s_clipSrc, sizeof(s_clipSrc), "%s", curPath);
    snprintf(s_clipName, sizeof(s_clipName), "%s", filePathBase(curPath));
    s_clipIsDir = true;
  } else {
    fullPathOf(idx, s_clipSrc, sizeof(s_clipSrc));
    snprintf(s_clipName, sizeof(s_clipName), "%s", entries[idx].name);
    s_clipIsDir = entries[idx].isDir;
  }
  s_clipMove = move ? 2 : 1;
  setNote("%s marked - open a folder and Paste", move ? "Move" : "Copy");
}

void FilesApp::pasteHere() {
  if (!s_clipSrc[0]) {
    return;
  }
  char dst[208];
  snprintf(dst, sizeof(dst), "%s%s%s", curPath,
           (strcmp(curPath, "/") == 0) ? "" : "/", s_clipName);

  if (!strcmp(dst, s_clipSrc)) {
    setNote("Already here");
    return;
  }
  if (SD.exists(dst)) {
    // Refuse rather than overwrite: clobbering a same-named file is how data dies.
    setNote("\"%s\" already exists here", s_clipName);
    return;
  }
  if (s_clipIsDir) {
    /* A folder. INTO ITSELF is the one paste that can never end (a copy of /maps into
     * /maps/x copies the copy). 🛑 THIS CHECK IS THE ONLY THING THAT STOPS IT: nothing
     * below refuses a rename into its own subtree — VFSImpl::rename checks only that the
     * source exists, and FatFs f_rename has no ancestor check, so /maps renamed to
     * /maps/home/maps would detach the tree from the root and re-parent it under itself,
     * unreachable (review, 2026-09-20). */
    if (filePathWithin(s_clipSrc, curPath)) {
      setNote("Cannot paste a folder into itself");
      return;
    }
    bool waiting = false;
    if (s_clipMove == 2 && downloadWritesUnder(s_clipSrc, &waiting)) {
      setNote("%s", downloadRefusal(waiting));
      return;
    }
    if (!SD.exists(s_clipSrc)) {
      setNote("Source is gone - paste cancelled");
      s_clipSrc[0] = '\0';
      s_clipMove = 0;
      return;
    }
    if (s_clipMove == 2) {
      // One rename: FAT moves a whole tree across folders on the same card in an instant.
      if (SD.rename(s_clipSrc, dst)) {
        setNote("Moved \"%s/\"", s_clipName);
        s_clipSrc[0] = '\0';
        s_clipMove = 0;
        scanDir();
      } else {
        setNote("Move failed - folder untouched (copy it, then delete)");
      }
      return;
    }
    // A copy is a tree walk: the COPY job, with its own screen.
    if (startJob(JOB_COPY, s_clipSrc, dst)) {
      s_clipSrc[0] = '\0';
      s_clipMove = 0;
    }
    return;
  }
  File src = SD.open(s_clipSrc, FILE_READ);
  if (!src) {
    // The source is gone (deleted, moved off-device). Nothing to hold anymore.
    setNote("Source is gone - paste cancelled");
    s_clipSrc[0] = '\0';
    s_clipMove = 0;
    return;
  }
  const uint32_t srcSize = (uint32_t)src.size();

  if (s_clipMove == 2) {
    // Try the atomic path first: FAT rename moves across folders on the same card.
    src.close();
    if (SD.rename(s_clipSrc, dst)) {
      setNote("Moved \"%s\"", s_clipName);
      s_clipSrc[0] = '\0';
      s_clipMove = 0;
      scanDir();
      return;
    }
    src = SD.open(s_clipSrc, FILE_READ);      // rename refused: fall back to copy+delete
    if (!src) {
      setNote("Move failed - file untouched");
      return;
    }
  }

  // Stream copy. On ANY failure the partial destination is removed and the source is
  // left exactly as it was — the mark-never-breaks-the-file rule.
  File out = SD.open(dst, FILE_WRITE);
  if (!out) {
    src.close();
    setNote("Cannot write here");
    return;
  }
  const size_t CHUNK = 8192;
  uint8_t* buf = (uint8_t*)ps_malloc(CHUNK);
  bool ok = (buf != NULL);
  uint32_t done = 0;
  int breather = 0;
  while (ok && done < srcSize) {
    size_t got = src.read(buf, CHUNK);
    if (!got) {
      ok = false;
      break;
    }
    if (out.write(buf, got) != got) {
      ok = false;
      break;
    }
    done += got;
    if ((++breather & 7) == 0) {
      vTaskDelay(1);                 // let WiFi/SIP breathe during a long copy
    }
  }
  free(buf);
  out.close();
  src.close();
  if (!ok || done != srcSize) {
    SD.remove(dst);                  // never leave a half-file lying around
    setNote("Copy failed - original untouched");
    return;
  }

  if (s_clipMove == 2) {
    // The copy is verified complete (byte count matched); only NOW may the source go.
    if (!SD.remove(s_clipSrc)) {
      setNote("Copied; could not remove original");
    } else {
      setNote("Moved \"%s\"", s_clipName);
    }
  } else {
    setNote("Pasted \"%s\"", s_clipName);
  }
  s_clipSrc[0] = '\0';
  s_clipMove = 0;
  scanDir();
}

void FilesApp::deleteEntry(int idx) {
  char path[208];
  fullPathOf(idx, path, sizeof(path));
  if (SD.remove(path)) {
    setNote("Deleted \"%s\"", entries[idx].name);
    if (!strcmp(path, s_clipSrc)) {
      s_clipSrc[0] = '\0';          // the clipboard cannot point at a ghost
      s_clipMove = 0;
    }
  } else {
    setNote("Could not delete \"%s\"", entries[idx].name);
  }
  scanDir();
}

void FilesApp::deleteFolderConfirmed() {
  char target[FILES_PATH_MAX];
  snprintf(target, sizeof(target), "%s", curPath);
  bool waiting = false;
  if (downloadWritesUnder(target, &waiting)) {
    if (job) {
      endJob(true);
    }
    setNote("%s", downloadRefusal(waiting));
    appState = FILES_BROWSE;
    buildBrowse();
    return;
  }
  if (s_clipSrc[0] && filePathWithin(target, s_clipSrc)) {
    s_clipSrc[0] = '\0';            // the clipboard cannot point into a tree being removed
    s_clipMove = 0;
  }
  if (job) {
    endJob(true);                    // the COUNT that ran under the confirm
  }
  upDir();                           // stand in the parent: the folder is about to go
  if (!startJob(JOB_DELETE, target, NULL)) {
    appState = FILES_BROWSE;         // upDir built the browse menu; the note says why
    buildBrowse();
  }
}

// ---------------------------------------------------------------- folder jobs

bool FilesApp::startJob(int kind, const char* src, const char* dst) {
  if (job) {
    endJob(true);
  }
  void* mem = ps_malloc(sizeof(Job));
  if (!mem) {
    setNote("No memory for that");
    return false;
  }
  job = new (mem) Job();             // placement-new: File members need construction
  job->kind = kind;
  snprintf(job->src, sizeof(job->src), "%s", src);
  snprintf(job->dst, sizeof(job->dst), "%s", dst ? dst : "");
  job->depth = 0;
  job->files = job->dirs = job->bytes = job->errors = 0;
  job->copying = false;
  job->fileDone = job->fileSize = 0;
  job->done = false;
  job->err[0] = '\0';
  job->startMs = millis();
  job->lv[0].dir = SD.open(src);
  if (!job->lv[0].dir || !job->lv[0].dir.isDirectory()) {
    setNote("Cannot open \"%s\"", filePathBase(src));
    freeJob();                       // not endJob: nothing started, nothing to report
    return false;
  }
  snprintf(job->lv[0].path, sizeof(job->lv[0].path), "%s", src);
  job->depth = 1;
  if (kind == JOB_COPY && !SD.mkdir(job->dst)) {
    setNote("Cannot create \"%s/\" here", filePathBase(job->dst));
    freeJob();
    return false;
  }
  jobShownFiles = jobShownDirs = jobShownErr = 0;
  jobShownBytes = 0;
  jobShownMs = 0;
  if (kind != JOB_COUNT) {
    appState = FILES_JOB;
    footer->setButtons("", "Stop");
    controlState.holdScreenAwake(true);   // the progress is being watched
    s_treeJob = true;
  }
  controlState.msAppTimerEventPeriod = 30;   // the slices; 0 again in endJob
  return true;
}

void FilesApp::freeJob() {
  s_treeJob = false;
  if (!job) {
    return;
  }
  if (job->copying) {
    job->in.close();
    job->out.close();
    SD.remove(job->outPath);         // a stopped copy leaves no half-file
  }
  for (int i = 0; i < job->depth; i++) {
    job->lv[i].dir.close();
  }
  job->~Job();
  free(job);
  job = NULL;
  controlState.msAppTimerEventPeriod = 0;
}

/* Has anything the screen shows moved since it was last drawn? A sprite push is ~30 ms of
 * the slice's own SPI bus; drawing every tick halved the copy rate. Once a second anyway,
 * for the clock. */
bool FilesApp::jobChanged() {
  if (!job) {
    return false;
  }
  const uint32_t now = millis();
  const uint32_t secs = (now - job->startMs) / 1000u;
  if (job->files == jobShownFiles && job->dirs == jobShownDirs && job->errors == jobShownErr &&
      (job->bytes >> 16) == (jobShownBytes >> 16) && secs == jobShownMs) {
    return false;
  }
  if (!job->done && (uint32_t)(now - jobDrawnAt) < 250u) {
    return false;                    // a delete moves every tick; four pushes a second is plenty
  }
  jobDrawnAt = now;
  jobShownFiles = job->files;
  jobShownDirs = job->dirs;
  jobShownErr = job->errors;
  jobShownBytes = job->bytes;
  jobShownMs = secs;
  return true;
}

/* The FIRST failure is the cause; on a delete every one after it is usually its consequence
 * (a subfolder that could not be opened leaves each ancestor "not empty", up to the root,
 * whose rmdir is always the last thing the walk does), so the note keeps the first and the
 * rest only count (review). `fmt` takes one string. */
void FilesApp::jobFail(const char* fmt, const char* what) {
  job->errors++;
  if (!job->err[0]) {
    snprintf(job->err, sizeof(job->err), fmt, what);
  }
}

/* One slice of the walk. The directory handles stay OPEN across slices (lv[].dir), so the
 * next entry is one readdir, not a rescan from the top — and unlinking the entry just read
 * is safe on FatFs (the entry is marked deleted in place; the directory's read position is
 * independent of it). A directory is removed when its handle runs dry. */
void FilesApp::stepJob() {
  if (!job || job->done) {
    return;
  }
  const uint32_t t0 = millis();
  const uint32_t slice = musicPlayerIsPlaying() ? FILES_JOB_SLICE_MUSIC_MS : FILES_JOB_SLICE_MS;
  static uint8_t* buf = NULL;        // the copy chunk: one PSRAM block, kept for the app's life
  const bool music = musicPlayerIsPlaying();
  while ((uint32_t)(millis() - t0) < slice) {
    if (music && audio) {
      audio->loop();                 // feed the music ring between ops (~0.5 s deep since 0.9.79, ~90 ms before)
    }
    if (job->copying) {
      if (!buf) {
        buf = (uint8_t*)ps_malloc(FILES_COPY_CHUNK);
        if (!buf) {
          jobFail("no memory to copy with%s", "");
          job->in.close();
          job->out.close();
          SD.remove(job->outPath);
          job->copying = false;
          continue;
        }
      }
      size_t want = job->fileSize - job->fileDone;
      if (want > FILES_COPY_CHUNK) {
        want = FILES_COPY_CHUNK;
      }
      const size_t got = want ? job->in.read(buf, want) : 0;
      if (want && (!got || job->out.write(buf, got) != got)) {
        jobFail("write failed: %s", filePathBase(job->outPath));
        job->in.close();
        job->out.close();
        SD.remove(job->outPath);     // never leave a half-file
        job->copying = false;
        continue;
      }
      job->fileDone += got;
      job->bytes += got;
      if (job->fileDone >= job->fileSize) {
        job->in.close();
        job->out.close();
        job->files++;
        job->copying = false;
      }
      continue;
    }
    if (job->depth <= 0) {
      job->done = true;
      break;
    }
    JobLevel& L = job->lv[job->depth - 1];
    errno = 0;
    File f = L.dir.openNextFile();
    if (!f) {
      /* The end of this directory — or an entry that could not be read, which the Arduino
       * layer reports the same way. errno tells them apart (per task in this newlib; the
       * true end leaves it 0): an unreadable entry is counted, so a copy that skipped it
       * cannot report success (review). Then: close it; for a delete, remove it; go up. */
      if (errno) {
        jobFail("cannot read: %s", filePathBase(L.path));
      }
      L.dir.close();
      if (job->kind == JOB_DELETE) {
        if (!sdRmdir(L.path)) {
          jobFail("folder not empty: %s", filePathBase(L.path));
        }
      }
      job->dirs++;
      job->depth--;
      continue;
    }
    char full[FILES_PATH_MAX];
    snprintf(full, sizeof(full), "%s", f.name());    // the FULL path on this core
    const bool isDir = f.isDirectory();
    const uint32_t size = isDir ? 0 : (uint32_t)f.size();
    f.close();
    if (isDir) {
      if (job->depth >= FILES_JOB_DEPTH) {
        jobFail("too deep: %s", filePathBase(full));
        continue;                    // skipped; a delete then finds its parent not empty
      }
      JobLevel& N = job->lv[job->depth];
      if (job->kind == JOB_COPY) {
        char to[FILES_PATH_MAX];
        if (!filePathReroot(job->src, job->dst, full, to, sizeof(to)) || !SD.mkdir(to)) {
          jobFail("cannot make: %s", filePathBase(full));
          continue;
        }
      }
      N.dir = SD.open(full);
      if (!N.dir || !N.dir.isDirectory()) {
        if (N.dir) {
          N.dir.close();
        }
        jobFail("cannot open: %s", filePathBase(full));
        continue;
      }
      snprintf(N.path, sizeof(N.path), "%s", full);
      job->depth++;
      continue;
    }
    // A file.
    if (job->kind == JOB_COUNT) {
      job->files++;
      job->bytes += size;
    } else if (job->kind == JOB_DELETE) {
      if (sdUnlink(full)) {
        job->files++;
      } else {
        jobFail("cannot delete: %s", filePathBase(full));
      }
    } else {                         // JOB_COPY: open the pair; the chunks follow
      if (!filePathReroot(job->src, job->dst, full, job->outPath, sizeof(job->outPath))) {
        jobFail("path too long: %s", filePathBase(full));
        continue;
      }
      job->in = SD.open(full, FILE_READ);
      job->out = job->in ? SD.open(job->outPath, FILE_WRITE) : File();
      if (!job->in || !job->out) {
        if (job->in) {
          job->in.close();
        }
        if (job->out) {
          job->out.close();
        }
        jobFail("cannot copy: %s", filePathBase(full));
        continue;
      }
      job->fileSize = size;
      job->fileDone = 0;
      job->copying = true;
    }
  }
  if (music && audio) {
    audio->loop();                   // ...and before the screen push that follows a slice
  }
  if (job->done && job->kind != JOB_COUNT) {
    endJob(false);
  }
}

void FilesApp::endJob(bool stopped) {
  if (!job) {
    return;
  }
  const int kind = job->kind;
  const uint32_t files = job->files, errors = job->errors;
  const uint32_t secs = (millis() - job->startMs) / 1000u;
  char err[48];
  snprintf(err, sizeof(err), "%s", job->err);
  char name[64];
  snprintf(name, sizeof(name), "%s", filePathBase(job->src));
  freeJob();
  if (kind == JOB_COUNT) {
    return;                          // the confirm screen owns the message
  }
  controlState.holdScreenAwake(false);
  if (kind == JOB_DELETE) {
    if (stopped) {
      setNote("Stopped: %lu files deleted, the rest still there", (unsigned long)files);
    } else if (errors) {
      setNote("Deleted %lu files; %lu could not be (%s)", (unsigned long)files,
              (unsigned long)errors, err);
    } else {
      setNote("Deleted \"%s/\": %lu files in %lus", name, (unsigned long)files, (unsigned long)secs);
    }
  } else {
    if (stopped) {
      setNote("Stopped: %lu files copied; the copy is partial", (unsigned long)files);
    } else if (errors) {
      setNote("Copied %lu files; %lu failed (%s)", (unsigned long)files, (unsigned long)errors, err);
    } else {
      setNote("Copied \"%s/\": %lu files in %lus", name, (unsigned long)files, (unsigned long)secs);
    }
  }
  log_e("FILES: %s %s: %lu files, %lu errors, %lu s%s", kind == JOB_DELETE ? "delete" : "copy",
        name, (unsigned long)files, (unsigned long)errors, (unsigned long)secs,
        stopped ? " (stopped)" : "");
  appState = FILES_BROWSE;
  footer->setButtons("Select", "Back");
  scanDir();
  buildBrowse();
}

void FilesApp::drawJob() {
  SmoothFont* f = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = f->height() + 2;
  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  if (!job) {
    return;
  }
  lcd.setTextFont(f);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(WHITE, BLACK);
  const uint16_t w = lcd.width() - 2 * FILES_MARGIN;
  int y = top + 4;
  char line[96];
  lcd.drawString(job->kind == JOB_DELETE ? "Deleting:" : "Copying:", FILES_MARGIN, y); y += lh;
  guiDrawEllipsized(lcd, job->src, w, FILES_MARGIN, y); y += lh + 4;
  if (job->kind == JOB_COPY) {
    lcd.drawString("into:", FILES_MARGIN, y); y += lh;
    guiDrawEllipsized(lcd, job->dst, w, FILES_MARGIN, y); y += lh + 4;
  }
  snprintf(line, sizeof(line), "%lu files, %lu folders", (unsigned long)job->files, (unsigned long)job->dirs);
  lcd.drawString(line, FILES_MARGIN, y); y += lh;
  if (job->kind == JOB_COPY) {
    snprintf(line, sizeof(line), "%lu KB", (unsigned long)(job->bytes >> 10));
    lcd.drawString(line, FILES_MARGIN, y); y += lh;
  }
  snprintf(line, sizeof(line), "%lu s", (unsigned long)((millis() - job->startMs) / 1000u));
  lcd.drawString(line, FILES_MARGIN, y); y += lh + 4;
  if (job->errors) {
    lcd.setTextColor(0xFD20, BLACK);
    snprintf(line, sizeof(line), "%lu could not be done:", (unsigned long)job->errors);
    lcd.drawString(line, FILES_MARGIN, y); y += lh;
    guiDrawEllipsized(lcd, job->err, w, FILES_MARGIN, y); y += lh + 4;
  }
  lcd.setTextColor(GRAY_50, BLACK);
  lcd.drawString("Back stops it where it is.", FILES_MARGIN, y);
}

void FilesApp::enterDir(const char* name) {
  size_t cur = strlen(curPath);
  // "/" + name, or path + "/" + name — bounded, and refuse rather than truncate a path:
  // a truncated path silently browses the WRONG directory.
  if (cur + 1 + strlen(name) + 1 >= sizeof(curPath)) {
    log_e("FILES: path too deep, not entering %s", name);
    return;
  }
  if (cur > 1) {
    strcat(curPath, "/");
  }
  strcat(curPath, name);
  note[0] = '\0';
  scanDir();
  buildBrowse();
}

void FilesApp::upDir() {
  char* slash = strrchr(curPath, '/');
  if (slash && slash != curPath) {
    *slash = '\0';
  } else {
    strcpy(curPath, "/");
  }
  note[0] = '\0';
  scanDir();
  buildBrowse();
}

// ---------------------------------------------------------------- viewing

void FilesApp::closeView() {
  if (viewText) {
    free(viewText);
    viewText = NULL;
  }
  viewLen = 0;
  viewPage = 0;
}

void FilesApp::openEntry(int idx) {
  if (idx < 0 || idx >= entryCount || entries[idx].isDir || !entries[idx].name[0]) {
    return;
  }
  closeView();
  snprintf(viewName, sizeof(viewName), "%s", entries[idx].name);

  char path[200];
  snprintf(path, sizeof(path), "%s%s%s", curPath,
           (strcmp(curPath, "/") == 0) ? "" : "/", entries[idx].name);

  const uint32_t size = entries[idx].size;
  /* 🛑 A KEY FILE IS NEVER DRAWN: Menu > AI's gemini.txt and anything else in an API Keys folder
   * (files_paths.h, filePathIsKeyFile). A screenshot of it would be a leaked key. It still lists,
   * copies and deletes like any file; only its contents stay off the screen. */
  const bool keyFile = filePathIsKeyFile(path);
  const bool wantText = !keyFile && textishName(viewName);
  bool loaded = false;
  bool clipped = false;

  if (wantText) {
    File f = SD.open(path, FILE_READ);
    if (f) {
      size_t take = size;
      if (take > FILES_VIEW_MAX) {
        take = FILES_VIEW_MAX;
        clipped = true;
      }
      viewText = (char*)ps_malloc(take + 96);
      if (viewText) {
        size_t got = f.read((uint8_t*)viewText, take);
        // Strip CR in place: the layouter speaks '\n' and a CR would render as a glyph.
        size_t w = 0;
        bool binary = false;
        int suspicious = 0;
        for (size_t r = 0; r < got; r++) {
          char c = viewText[r];
          if (c == '\r') {
            continue;
          }
          if ((unsigned char)c == 0) {
            binary = true;
            break;
          }
          if ((unsigned char)c < 9) {
            if (++suspicious > 16) {       // a handful can be junk; a lot means binary
              binary = true;
              break;
            }
          }
          viewText[w++] = c;
        }
        if (binary) {
          free(viewText);
          viewText = NULL;
        } else {
          if (clipped) {
            w += snprintf(viewText + w, 90, "\n\n[showing the first %u KB of %lu KB]",
                          (unsigned)(FILES_VIEW_MAX / 1024), (unsigned long)(size / 1024));
          }
          viewLen = w;
          loaded = true;
        }
      }
      f.close();
    }
  }

  if (!loaded) {
    // The info page: every file opens to SOMETHING that says what it is and why there is
    // no more to see. Far better than a dead OK button or a screen of glyph soup.
    viewText = (char*)ps_malloc(256);
    if (!viewText) {
      return;
    }
    viewLen = snprintf(viewText, 256,
                       "%s\n\n%lu bytes\n\n%s",
                       viewName, (unsigned long)size,
                       keyFile ? "Key file - not shown.\n\nThe apps that need it read it from "
                                 "the card (Menu > AI reads gemini.txt)."
                       : wantText ? "This file could not be read as text."
                                : "No viewer for this file type.\n\nText files "
                                  "(.txt .log .ini .md .json ...) open as pages.");
  }
  viewPage = 0;
  appState = FILES_VIEW;
  snprintf(headerTitle, sizeof(headerTitle), "%s", viewName);
  header->setTitle(headerTitle);
  footer->setButtons("", "Back");
}

int FilesApp::viewLines() const {
  SmoothFont* f = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  return (bottom - top - 4) / (f->height() + 2);
}

int FilesApp::viewWidth() const {
  return (int)lcd.width() - 2 * FILES_MARGIN;
}

// ---------------------------------------------------------------- uploading

void FilesApp::startUpload() {
  snprintf(xferDir, sizeof(xferDir), "%s", curPath);
  snprintf(xferHeading, sizeof(xferHeading), "Upload to %s", curPath);
  xferCfg.dir = xferDir;
  xferCfg.heading = xferHeading;
  xferCfg.accept = "";               // any file: this is the GENERAL uploader
  xferCfg.nounPlural = "files";
  xferCfg.defaultName = "download.bin";
  xferCfg.apName = "WiPhone-Files";
  /* ⚠ SET IT. This config is a member filled field by field, and the object lives in PSRAM
   * that operator new does not zero: an unset `tree` was whatever the allocator left, so the
   * general uploader ran in tree mode (folder-shaped names accepted, folders created) on
   * some boots and not others (review, 2026-09-19). */
  xferCfg.tree = false;
  xferStart(&xferCfg);
  appState = FILES_XFER;
  footer->setButtons("", "Stop");
}

// ---------------------------------------------------------------- events

appEventResult FilesApp::processEvent(EventType event) {
  if (appState == FILES_BROWSE) {
    if (LOGIC_BUTTON_BACK(event)) {
      if (strcmp(curPath, "/") == 0) {
        return EXIT_APP;
      }
      upDir();
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->readChosen();
      /* ⚠ BEFORE the `sel >= ROW_FIRST` test below, not after: MENU_ROW_NOTE minus ROW_FIRST
       * underflows and casts to a NEGATIVE idx, which passes `idx < entryCount` and indexes
       * `entries` out of bounds. A display-only row does nothing. */
      if (sel == MENU_ROW_NOTE) {
        return REDRAW_SCREEN;
      }
      if (sel == ROW_UPLOAD) {
        startUpload();
        return REDRAW_ALL;
      } else if (sel == ROW_UP) {
        upDir();
        return REDRAW_ALL;
      } else if (sel == ROW_PASTE) {
        pasteHere();
        buildBrowse();
        return REDRAW_ALL;
      } else if (sel == ROW_CLIPCLEAR) {
        s_clipSrc[0] = '\0';
        s_clipMove = 0;
        setNote("Paste cancelled - nothing was changed");
        buildBrowse();
        return REDRAW_ALL;
      } else if (sel == ROW_THISDIR) {
        folderOptions = true;
        selIdx = -1;
        buildOptions();
        appState = FILES_OPTIONS;
        return REDRAW_ALL;
      } else if (sel >= ROW_FIRST) {
        int idx = (int)(sel - ROW_FIRST);
        if (idx < entryCount) {
          if (entries[idx].isDir) {
            enterDir(entries[idx].name);
          } else {
            // A file asks what you want ("click a file and it asks"): Open / Copy /
            // Move / Delete. Folders still enter on one press - browsing stays fast;
            // their options are the "[ This folder... ]" row inside.
            selIdx = idx;
            folderOptions = false;
            buildOptions();
            appState = FILES_OPTIONS;
          }
          return REDRAW_ALL;
        }
      }
    }
    return REDRAW_SCREEN;

  } else if (appState == FILES_OPTIONS) {
    if (LOGIC_BUTTON_BACK(event)) {
      appState = FILES_BROWSE;
      buildBrowse();
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->readChosen();
      if (sel == OPT_OPEN) {
        openEntry(selIdx);
        return REDRAW_ALL;
      } else if (sel == OPT_COPY || sel == OPT_MOVE) {
        markClipboard(selIdx, sel == OPT_MOVE);
        appState = FILES_BROWSE;
        buildBrowse();
        return REDRAW_ALL;
      } else if (sel == OPT_DELETE) {
        if (folderOptions) {
          /* Count what is about to go while the question is on the screen: the COUNT job
           * runs under the confirm and its note is rebuilt on every tick. */
          startJob(JOB_COUNT, curPath, NULL);
        }
        buildConfirmDel();
        appState = FILES_CONFIRM_DEL;
        return REDRAW_ALL;
      } else if (sel == OPT_CANCEL) {
        appState = FILES_BROWSE;
        buildBrowse();
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  } else if (appState == FILES_CONFIRM_DEL) {
    if (event == APP_TIMER_EVENT) {
      if (job && job->kind == JOB_COUNT && !job->done) {
        stepJob();
        if (job && job->done) {
          controlState.msAppTimerEventPeriod = 0;
        }
        /* Rebuilt only when the numbers moved (a rebuild resets the marquee and costs a
         * push); the thumb stays where it was: on Cancel unless the user moved it. */
        if (jobChanged() || (job && job->done)) {
          const MenuOption::keyType keep = menu ? menu->currentKey() : DEL_NO;
          buildConfirmDel();
          menu->select(keep);
          return REDRAW_SCREEN;
        }
      }
      return DO_NOTHING;
    }
    if (LOGIC_BUTTON_BACK(event)) {
      if (job) {
        endJob(true);
      }
      appState = FILES_BROWSE;
      buildBrowse();
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->readChosen();
      if (sel == MENU_ROW_NOTE) {
        return REDRAW_SCREEN;        // the count line is read, not chosen (as in browse)
      }
      if (sel == DEL_YES) {
        if (folderOptions) {
          deleteFolderConfirmed();   // enters FILES_JOB with its own screen
          return REDRAW_ALL;
        }
        deleteEntry(selIdx);
      }
      if (job) {
        endJob(true);                // a COUNT left running under a Cancel
      }
      appState = FILES_BROWSE;
      buildBrowse();
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;

  } else if (appState == FILES_JOB) {
    if (event == APP_TIMER_EVENT) {
      stepJob();                     // may endJob() and drop back to browsing
      if (appState != FILES_JOB) {
        return REDRAW_ALL;
      }
      return jobChanged() ? REDRAW_SCREEN : DO_NOTHING;
    }
    if (LOGIC_BUTTON_BACK(event)) {
      endJob(true);                  // stop where it is; the note says what got done
      return REDRAW_ALL;
    }
    return DO_NOTHING;

  } else if (appState == FILES_VIEW) {
    if (LOGIC_BUTTON_BACK(event)) {
      closeView();
      appState = FILES_BROWSE;
      buildBrowse();
      footer->setButtons("Select", "Back");
      return REDRAW_ALL;
    }
    if (viewText && (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_OK)) {
      BookMeasure m = { fonts[AKROBAT_BOLD_18], fontMeasure };
      BookPage pg;
      bookLayoutPage(viewText, viewLen, viewPage, viewWidth(), viewLines(), &m, &pg);
      if (pg.next < viewLen) {
        viewPage = pg.next;
      }
      return REDRAW_SCREEN;
    }
    if (viewText && event == WIPHONE_KEY_UP && viewPage > 0) {
      BookMeasure m = { fonts[AKROBAT_BOLD_18], fontMeasure };
      viewPage = bookLayoutPrevPage(viewText, viewLen, viewPage, viewWidth(), viewLines(), &m);
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;

  } else {                            // FILES_XFER
    if (LOGIC_BUTTON_BACK(event) || LOGIC_BUTTON_OK(event)) {
      xferStop();
      appState = FILES_BROWSE;
      note[0] = '\0';
      scanDir();                      // whatever was uploaded should be visible NOW
      buildBrowse();
      footer->setButtons("Select", "Back");
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;             // any keypress refreshes the files-added count
  }
}

// ---------------------------------------------------------------- drawing

void FilesApp::redrawScreen(bool redrawAll) {
  if (appState == FILES_BROWSE || appState == FILES_OPTIONS || appState == FILES_CONFIRM_DEL) {
    drawBrowse(redrawAll);
  } else if (appState == FILES_VIEW) {
    drawView();
  } else if (appState == FILES_JOB) {
    drawJob();
  } else {
    drawXfer();
  }
}

void FilesApp::drawBrowse(bool all) {
  if (menu) {
    ((GUIWidget*)menu)->redraw(lcd);
  }
}

void FilesApp::drawView() {
  SmoothFont* f = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = f->height() + 2;

  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  if (!viewText) {
    return;
  }

  BookMeasure m = { f, fontMeasure };
  BookPage pg;
  bookLayoutPage(viewText, viewLen, viewPage, viewWidth(), viewLines(), &m, &pg);

  lcd.setTextFont(f);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(WHITE, BLACK);
  char line[448];
  int y = top + 2;
  for (int i = 0; i < pg.nLines; i++) {
    if (!pg.lines[i].blank) {
      bookRenderRun(f, viewText + pg.lines[i].off, pg.lines[i].len, line, sizeof(line));
      lcd.drawString(line, FILES_MARGIN, y);
    }
    y += lh;
  }
}

void FilesApp::drawXfer() {
  SmoothFont* f = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = f->height() + 2;

  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  lcd.setTextFont(f);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(WHITE, BLACK);
  int y = top + 4;
  char line[96];
  /* Two lines, and the PATH gets one to itself. As a single line the ellipsis
   * ate the tail — which is the destination folder, the only thing this line
   * exists to tell you ("Uploading into /Books/BattleTech Gh.." says nothing
   * about where the file lands). The label is fixed and short; the path is the
   * variable part, so it is what gets the room. */
  const uint16_t pathW = lcd.width() - 2 * FILES_MARGIN;
  lcd.drawString("Uploading into:", FILES_MARGIN, y); y += lh;
  guiDrawEllipsized(lcd, curPath, pathW, FILES_MARGIN, y); y += lh + 4;
  if (xferOn()) {
    lcd.drawString("On your computer, open:", FILES_MARGIN, y); y += lh;
    snprintf(line, sizeof(line), "  http://%s/", xferAddr());
    lcd.drawString(line, FILES_MARGIN, y); y += lh;
    lcd.drawString("  (or http://wiphone.local)", FILES_MARGIN, y); y += lh + 4;
    if (xferUsingAP()) {
      snprintf(line, sizeof(line), "Join hotspot: %s", xferApName());
      lcd.drawString(line, FILES_MARGIN, y); y += lh;
    }
    snprintf(line, sizeof(line), "Files added: %d", xferFilesAdded());
    lcd.drawString(line, FILES_MARGIN, y); y += lh + 4;
    lcd.setTextColor(GRAY_50, BLACK);
    lcd.drawString("Any key refreshes the count.", FILES_MARGIN, y); y += lh;
    lcd.drawString("Back stops the server.", FILES_MARGIN, y);
  } else {
    /* ⚠ THIS USED TO SAY "Check WiFi and try again" WHATEVER THE REASON, and the
     * commonest reason turned out not to be WiFi at all: too little contiguous
     * heap to serve from, which sent {user} hunting the hotspot for an hour on
     * 2026-08-25 while the radio was fine. Say the actual reason when there is one. */
    const char* err = xferStartError();
    lcd.drawString("Server did not start.", FILES_MARGIN, y); y += lh;
    if (err) {
      guiDrawEllipsized(lcd, err, pathW, FILES_MARGIN, y);
    } else {
      lcd.drawString("Check WiFi and try again.", FILES_MARGIN, y);
    }
  }
}
