/*
 * app_files.h — a general, lightweight SD file browser + viewer + uploader.
 *
 * Three screens in one app, deliberately minimal:
 *   BROWSE  folders and files, dirs first, bounded listing (no unbounded heap on a phone
 *           that fights for every contiguous kilobyte).
 *   VIEW    text-ish files, paged with the SAME layout engine and advance-corrected
 *           measure the e-reader uses — so wrapping is correct by construction. A file the
 *           viewer will not render (binary, or huge) still opens: it gets an info page
 *           (name, size, why) instead of a garbage dump.
 *   XFER    the one parameterised upload server (app_gbc_xfer), pointed at the folder you
 *           are standing in. This is the piece that makes the browser more than a toy:
 *           until now /roms and /books were the only folders reachable over WiFi at all.
 *
 * Files delete behind a confirm; FOLDERS can be copied, moved and deleted too (0.9.72): the
 * `[ This folder... ]` row at the top of any folder but the root opens Copy / Move / Delete
 * for the folder you are standing in — that is how a folder is "selected" ({user}, 2026-09-20:
 * "there is no way to select a folder and copy/paste/delete... found it out when trying to
 * delete 'home' in the maps folder"). A tree operation is a JOB: it runs a slice at a time on
 * the app timer with a progress screen, never blocking the phone for the minute a thousand
 * tiles take, and Back stops it where it is (what is done stays done, and the screen says
 * so). A folder move is one rename. The path questions that keep it safe are in
 * files_paths.h, host-tested.
 */
#ifndef APP_FILES_H
#define APP_FILES_H

#include "GUI.h"
#include "book_layout.h"
#include "app_gbc_xfer.h"    // XferConfig: the one parameterised upload server

class FilesApp : public WindowedApp {
public:
  FilesApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~FilesApp();
  ActionID_t getId() {
    return GUI_APP_FILES;
  }
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll = false);

protected:
  typedef enum {
    FILES_BROWSE,
    FILES_OPTIONS,      // OK on a file: Open / Copy / Move / Delete / Cancel; or the folder's
    FILES_CONFIRM_DEL,  // Cancel (default) / Yes, delete
    FILES_VIEW,
    FILES_XFER,
    FILES_JOB,          // a tree count / delete / copy in progress (stepJob on the timer)
  } FilesState_t;

  // ---- folder jobs: a tree walk, a slice at a time
  enum { JOB_NONE = 0, JOB_COUNT, JOB_DELETE, JOB_COPY };
  /* Levels held open at once. Each open directory costs ~700 B of INTERNAL heap (vfs_fat's
   * DIR + the File impl), so this is small on purpose: deleting /maps/home holds three
   * (home/15/5296), the deepest tree on the card needs four. Deeper is skipped and said. */
  static const int FILES_JOB_DEPTH = 6;
  static const size_t FILES_PATH_MAX = 208;
  struct JobLevel {
    File dir;                                  // open directory being walked
    char path[FILES_PATH_MAX];
  };
  struct Job {
    int      kind;
    char     src[FILES_PATH_MAX];
    char     dst[FILES_PATH_MAX];              // COPY: the new root
    JobLevel lv[FILES_JOB_DEPTH];
    int      depth;                            // lv[depth-1] is the directory being read
    uint32_t files, dirs, errors;
    uint64_t bytes;                            // a map area can pass 4 GB
    bool     copying;                          // a file is mid-copy across slices
    File     in, out;
    char     outPath[FILES_PATH_MAX];
    uint32_t fileDone, fileSize;
    bool     done;
    char     err[48];                          // the last thing that went wrong, for the note
    uint32_t startMs;
  };
  Job*  job = NULL;                            // PSRAM, placement-new (File members)
  uint32_t jobShownFiles = 0, jobShownDirs = 0, jobShownErr = 0;   // what the screen last drew
  uint64_t jobShownBytes = 0;
  uint32_t jobShownMs = 0;
  uint32_t jobDrawnAt = 0;                     // millis() of the last progress draw
  bool  jobChanged();                          // has anything the screen shows moved?
  bool  startJob(int kind, const char* src, const char* dst);
  void  freeJob();                             // handles closed, memory freed, nothing said
  void  stepJob();                             // one slice: FILES_JOB_SLICE_MS of work
  void  jobFail(const char* fmt, const char* what);   // count it; keep the FIRST message
  void  endJob(bool stopped);                  // close handles, note, back to browsing
  void  drawJob();
  bool  folderOptions = false;                 // the options menu is about curPath itself

  struct FEntry {
    char     name[64];
    uint32_t size;
    bool     isDir;
  };

  FilesState_t appState = FILES_BROWSE;

  // ---- browsing
  char        curPath[128];
  char        headerTitle[64];      // HeaderWidget keeps the POINTER — must outlive the call
  MenuWidget* menu = NULL;
  FEntry*     entries = NULL;       // PSRAM
  int         entryCount = 0;
  bool        truncated = false;

  // ---- viewing
  char*    viewText = NULL;         // PSRAM
  size_t   viewLen = 0;
  uint32_t viewPage = 0;
  char     viewName[64];

  // ---- uploading
  XferConfig xferCfg;
  char       xferDir[128];
  char       xferHeading[80];

  int  selIdx = -1;                 // the entry the options/confirm menus are about
  char note[96];                    // one-line result message shown atop the browse list

  void scanDir();
  void buildBrowse();
  void buildOptions();
  void buildConfirmDel();
  void enterDir(const char* name);
  void upDir();
  void fullPathOf(int idx, char* out, size_t cap) const;
  void openEntry(int idx);
  void closeView();
  void startUpload();
  void markClipboard(int idx, bool move);
  void pasteHere();
  void deleteEntry(int idx);
  void deleteFolderConfirmed();      // after the confirm: the DELETE job on curPath
  void setNote(const char* fmt, ...);
  void drawBrowse(bool all);
  void drawView();
  void drawXfer();
  int  viewLines() const;
  int  viewWidth() const;
};

#endif // APP_FILES_H
