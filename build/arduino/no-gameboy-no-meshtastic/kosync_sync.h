/*
 * kosync_sync.h — reading-position sync over KOSync: the WINDOW and the home CLIENT.
 *
 * The protocol itself (routes, JSON, auth, the clock, the config file) is kosync.h and is
 * host-tested; this is the part that needs sockets, the radio and the card.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * WHAT A PERSON SEES
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * Nothing at all until /books/kosync.txt exists (see kosync.h for its lines). Then:
 *
 *   "Sync my place" (reader menu) opens a WINDOW for 5 minutes: the phone serves KOSync for
 *     THIS book — on its own open hotspot 'WiPhone-Books' (http://192.168.4.1) when it is not
 *     on WiFi, or on its WiFi address when it is. On WiFi with home= set it FIRST READS the
 *     home server and then PUTs the place there — unless the server holds a place from another
 *     device that the offer rule (kosync.h, D1) says is news: that is OFFERED as the card
 *     instead, and ours is not sent over it (pressing again after declining it sends ours) —
 *     and the window opens once that job has its verdict, or after 25 s (kosync.h,
 *     kosyncWindowWaitsForHome: "Asking home first - the window opens after").
 *     While a KOSync place for the book is still on the card, unanswered, it sends nothing.
 *     An X4 (or anything that speaks KOSync) joins and syncs; the
 *     window closes 10 s after a PUT for the book, 120 s after a GET (the peer may be waiting
 *     on a person), or at the deadline — whichever is first.
 *   A PUT from the peer lands as the ORDINARY sync card ("CrossPoint says: they are at N%"),
 *     with its undo, arming and backward warning — it is parked in the booksync inbox as a
 *     locally-signed CBS1 record, so none of that machinery knows KOSync exists.
 *   auto=on         closing a book (or leaving the reader) opens the same 5-minute window
 *                   and pushes home — the push ONLY if the place moved since the book was
 *                   opened or last pushed (kosync.h, D2): an unmoved place must not overwrite
 *                   a newer one another device sent.
 *   open_window=on  opening a book while NOT on WiFi opens a 60 s window, so an X4 already
 *                   waiting in its own "sync" can find the phone.
 *   Opening a book while ON WiFi with home= set asks the home server (partial MD5 first,
 *   then the file-name id) and offers the card per the offer rule. Opened BEFORE the WiFi
 *   came up (right after power-on, say), the same ask is made once it does, while the book
 *   is still open.
 *   The home client never blocks the loop: its connect is polled, and a push or pull that
 *   gets no answer is tried again (KOSYNC_CLIENT_TRIES in all, 1 s and 4 s apart) before it
 *   is given up — and then said so.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * THE RULES THIS MODULE KEEPS
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * 🛑 THE PHONE ONLY HOSTS. It never scans for, and never joins, anybody else's hotspot:
 *    joining one fires STA_GOT_IP, and SIP, NTP, the SMS mirror and the tile downloader all
 *    wake up into a network with no route. The X4 does the joining.
 * 🛑 NO POSITION IS STORED DIFFERENTLY. The window and the client work on a SNAPSHOT
 *    (KosyncBook) taken from the reader; a peer's place goes into the inbox and is taken, or
 *    not, by a human on the card. positions.cbs, the reading spine and the LoRa record are
 *    untouched.
 * ⚠ OWNED AT FILE SCOPE. BooksApp is new'd on entry and deleted on exit; a window opened
 *    on book close has to outlive it (the same lesson as the booksync inbox), so the window,
 *    its book and the client all live here.
 * ⚠ The window gates the WiFi rescue exactly as the uploader's hotspot does (xferServing() in
 *    WiPhone.ino) — for at most 5 minutes, on a hard millis() deadline that nothing can
 *    extend past what was asked for. It does NOT hold the screen or pin full speed (the uploader
 *    does): it serves with the screen dark, at 80 MHz. It never opens during a Game Boy game
 *    (and a game closes it), nor under Settings > WiFi (which closes it too).
 */
#ifndef KOSYNC_SYNC_H
#define KOSYNC_SYNC_H

#include <stddef.h>
#include <stdint.h>
#include "epub_parse.h"
#include "kosync.h"

#define KOSYNC_CONFIG_FILE  "/books/kosync.txt"
#define KOSYNC_AP_NAME      "WiPhone-Books"     // the same SSID the Books uploader hosts

/* Everything the window and the client need about one book, copied out of the reader so it
 * survives the reader. ~4.5 KB: always PSRAM, never a stack local. */
struct KosyncBook {
  char     title[48];
  char     partial[EPUB_KOSYNC_ID_CHARS];      // KOReader's partial MD5 ("" if unreadable)
  char     byName[EPUB_KOSYNC_ID_CHARS];       // MD5 of the file name
  char     ids[3][EPUB_ID_MAX];                // the booksync ids (what the inbox matches)
  int      nIds;
  int      nRead;                              // the reading spine's length
  bool     pctOk;                              // false: this place cannot be expressed
  double   pct;                                // the phone's place as a KOSync percentage
  /* UTC of the reader's last real MOVE in this book (kosyncMovedAt), 0 = unknown. 🛑 Not the
   * CBS1 turnedAt: that one is stamped on every save and close, and a record compared against
   * it hid a newer X4 place behind an open-and-close that read nothing. */
  uint32_t movedAt;
  EpubKosyncMap map;                           // for turning a peer's percentage back
};

// ---------------------------------------------------------------- config
/* Re-read the card now; true = configured. `asked`: a person asked for it (serial `kosync
 * reload`), which also clears the last window's warnings — as does a re-read that finds the
 * file changed. The re-read Books makes on every entry keeps them (kosyncReloadClears). */
bool kosyncReloadConfig(bool asked = false);
bool kosyncConfigured();                       // reads the card once if it never has
const KosyncConfig* kosyncConfig();
const char* kosyncConfigNote();                // "off (no /books/kosync.txt)", a warning, or ""
const char* kosyncMyDevice();                  // device= or "WiPhone-<booksync name>"
const char* kosyncMyDeviceId();

// ---------------------------------------------------------------- from the reader
/* "Sync my place". On WiFi with a home= it queues the home job FIRST and the 5-minute window
 * opens from the loop once that job has its verdict (or after KOSYNC_WINDOW_AFTER_HOME_MAX_MS),
 * with `note` "Asking home first - the window opens after"; otherwise it opens the window now.
 * False with `note` saying why when nothing could be done; on success with no wait `note` is
 * left empty — the live lines (kosyncWindowLine/kosyncClientLine) say what is happening. */
bool kosyncSyncMyPlace(const KosyncBook* b, char* note, size_t cap);
void kosyncBookOpened(const KosyncBook* b);   // pull (on WiFi, or once it comes up) / window
void kosyncBookClosed(const KosyncBook* b);   // auto=on: window + push (if moved, or unsent)
/* The reader's place in this book actually CHANGED (a page turn, a jump, a card taken) —
 * not a save. `nowUtc` 0 (no clock) keeps the previous stamp, which is still no later than
 * the real last move, so it can only ever let MORE offers through. */
void     kosyncNoteMoved(const char* byName, uint32_t nowUtc);
uint32_t kosyncMovedAt(const char* byName);    // 0 = unknown
// Write the per-book memo to NVS now if it changed (a book close does this itself).
void     kosyncSaveState();
/* A book file was renamed (the console's `mv`; basenames, not paths): the per-book memo and any
 * parked offer follow it to the new file-name id, and the memo is saved. True when anything moved. */
bool     kosyncBookRenamed(const char* oldBasename, const char* newBasename);
// Someone wants the live place of this book: a window serving it, or an ask waiting for WiFi.
bool kosyncWantsPosition();
// The reader moved: a window serving this book answers GETs with the new place.
void kosyncNotePosition(const char* partial, const char* byName, double pct, bool pctOk,
                        uint32_t movedAt);

// ---------------------------------------------------------------- the window
bool     kosyncWindowOpen(const KosyncBook* b, uint32_t durationMs, char* note, size_t cap);
void     kosyncWindowClose(const char* why);
bool     kosyncWindowActive();
/* Called by the raw pump (app_gbc_xfer.cpp) for every KOSync request while a window is up.
 * Returns the JSON reply (a buffer owned here) and sets *code. */
const char* kosyncWindowServe(const char* method, const char* path, const char* hdrs,
                              const char* body, size_t bodyLen, int* code);

// ---------------------------------------------------------------- the home client
bool kosyncPush(const KosyncBook* b, char* note, size_t cap);   // PUT under both ids
bool kosyncPull(const KosyncBook* b, char* note, size_t cap);   // GET partial, then name
/* "Sync my place"'s home half: GET both ids; someone else's newer place is OFFERED and ours
 * is not sent over it; otherwise PUT under both ids. */
bool kosyncSyncHome(const KosyncBook* b, char* note, size_t cap);
bool kosyncClientBusy();                                         // mid-request: no idle tick

/* "hotspot: WPA2" / "hotspot: open" / "hotspot: open (uploader's)" / "hotspot: open -
 * hotspot_pass ignored: 8-63 characters". The live hotspot while a window is open, the
 * configured one otherwise. 🛑 Never the password: this is all anything ever says of it. */
size_t kosyncHotspotLine(char* out, size_t cap);

/* The peer's OWN KOSync percentage for a parked offer (by inbox id), so the sync card can
 * show the number the X4 shows beside this phone's. False for a LoRa offer. */
bool kosyncPeerPctFor(uint32_t inboxId, double* pct);
/* The person ANSWERED the sync card for inbox record `inboxId`: "Go there" (applyPending) or
 * "Stay where I am" (Back). 🛑 ONLY THOSE TWO — not the press the arming window swallows, not a
 * card left up, not a park dropped as a side effect of answering another card: a home record
 * becomes "declined" (not offered again) here and nowhere else (kosync.h, KS-2). A LoRa card, or
 * no KOSync at all: nothing, without touching NVS. */
void kosyncCardAnswered(uint32_t inboxId);

// ---------------------------------------------------------------- status
// One line each for the reader menu and Sync settings; "" when there is nothing to say.
// Whole minutes only, so the text (and the menu rebuilt around it) changes rarely.
size_t kosyncWindowLine(char* out, size_t cap);
size_t kosyncClientLine(char* out, size_t cap);
/* What went wrong in the current (or the last) window that the X4 would not tell anyone: a
 * place for a different book, the wrong user/password. "" when nothing did. */
size_t kosyncProblemLine(char* out, size_t cap);
void   kosyncDumpStatus(void (*emit)(const char* line));        // serial `kosync`

// One bounded step per main-loop pass. `callActive`: a live or imminent SIP call — no
// automatic window is opened into one (a hotspot would take the phone off its WiFi).
void kosyncLoop(bool mayUseNetwork, bool callActive);

#endif // KOSYNC_SYNC_H
