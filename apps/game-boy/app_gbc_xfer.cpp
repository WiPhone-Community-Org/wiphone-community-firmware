#include "Networks.h"   // wifiState, wifiRestoreStation(): the station back after a hotspot
/*
 * app_gbc_xfer.cpp — the file-transfer web server (see app_gbc_xfer.h).
 *
 * Robustness notes (learned on hardware):
 * - WebServer::handleClient() processes an entire POST synchronously in the
 *   main loop, so a big upload keeps the loop busy for many seconds. The idle
 *   task starves and the task watchdog reboots the phone. The upload/download
 *   handlers therefore delay(1) periodically to feed it.
 * - The upload page sends files ONE PER REQUEST, sequentially. One giant
 *   multipart POST with several files maximized the blocking window (reboots)
 *   and gave no per-file feedback (browser looked frozen).
 * - The server does NOT filter by extension: `accept` is only a hint to the
 *   browser's file picker. That is deliberate and has already earned its keep —
 *   it is how a 5 MB EPUB reached a phone whose firmware only knew about ROMs.
 */

#include "app_gbc_xfer.h"
#include "chunk_proto.h"
#include "files_paths.h"   // filePathIsKeyFile: the key folder is write-only here
#include "Arduino.h"
#include <stdarg.h>        // windowRefused()'s message
#include "WiFi.h"
#include "WebServer.h"
#include "ESPmDNS.h"
#include "HTTPClient.h"
#include "WiFiClientSecure.h"
#include "SD.h"
#include "GUI.h"
#include "kosync.h"        // kosyncIsPath: which requests belong to a KOSync window
#include "kosync_sync.h"   // kosyncWindowServe: the window's answers
#include "meshtastic_service.h"   // meshService.txComplete(): the radio back in RX mid-upload

extern volatile bool gGbcActive;   // WiPhone.ino: a Game Boy game owns the radio, RAM and cores

static WebServer*   s_server = NULL;
static File         s_uploadFile;
static volatile int s_filesAdded = 0;   // uploads + downloads this session (for status)
/* ── TWO OWNERS OF ONE TRANSPORT (0.9.79) ─────────────────────────────────────────────
 * The network this module brings up (the phone's WiFi address, or its own hotspot), the
 * raw :80 listener and the screen hold used to belong to the uploader alone. A KOSync sync
 * WINDOW (kosync_sync.h) needs exactly those three and nothing else of the uploader's, so
 * they are now shared:
 *
 *   s_on      the transport is up             -> xferServing(): the busy/240 MHz predicate
 *                                                and the WiFi gates in WiPhone.ino
 *   s_upOn    the UPLOADER is up              -> gbcXferOn()/xferOn(): what every
 *                                                "Add books/music/ROMs" screen shows
 *   s_winOn   a KOSync window holds it        -> xferWindowUp()
 *
 * 🛑 xferStop() STOPS THE UPLOADER, NOT THE TRANSPORT. Every app calls it from its destructor
 * ("in case the app dies with the transfer screen up"), and before this split that meant
 * leaving Music, Games, Files — or Books itself — killed any window a book close had just
 * opened. The transport now comes down only when its LAST owner lets go. */
static bool         s_on = false;       // transport up (either owner)
static bool         s_upOn = false;     // the uploader (WebServer, :8081, the page, /chunk)
static bool         s_winOn = false;    // a KOSync window (only the KOSync routes on :80)
static char         s_apName[33] = {0}; // the hotspot we are actually hosting, when s_usingAP
/* Who brought the hotspot up, and how. A hotspot is never RE-configured under whoever is
 * using it: a window that finds the uploader's open 'WiPhone-Books' rides it open, and an
 * uploader that finds a window's WPA2 one rides it protected (and its screens say so). */
static bool         s_apProtected = false;  // WPA2 (a window's hotspot_pass), else open
static bool         s_apByUploader = false; // xferStart() brought it up, not a window

/* ── IDLE AUTO-STOP ────────────────────────────────────────────────────────────────
 * The transfer server is a term in the DFS busy predicate (WiPhone.ino), so while it
 * is up the CPU is PINNED at full speed (240 MHz until 0.9.79; 160 now, since WiFi is up
 * and cpu_clock.cpp may not re-lock the PLL under it) and the idle tick runs 5x faster - roughly
 * DOUBLE idle current - and it also stretches the screen sleep timeout to 10 minutes.
 * Nothing ever released that. There was no idle timeout, no client-gone timeout, no
 * WiFi-loss timeout: it stayed on until something called xferStop().
 *
 * That was survivable while the ONLY way to start it was a transfer SCREEN, because
 * every such screen calls xferStop() from its destructor - the server could not
 * outlive its owner. `up on` / `up on books` (serial_cmd.cpp) start it HEADLESS, with
 * no owner at all, and nothing to press Back on.
 *
 * MEASURED, in /tmp/wiphone-serial.log on the affected phone: `up on books`, last file
 * landed at up=14min, then 328 consecutive health samples reading cpu=240MHz with
 * scr=0 until up=92min. Eighty-two minutes at double idle draw with the screen off,
 * ended only because `up off` was typed by hand.
 *
 * So it now stops itself. The window is generous on purpose - this is a safety net for
 * a FORGOTTEN server, not a policy on slow uploads - and any traffic at all defers it,
 * so a browser sitting on the page between files is never cut off mid-batch. */
#define XFER_IDLE_STOP_MS   (10UL * 60UL * 1000UL)
static uint32_t     s_lastActivityMs = 0;      // any client traffic; 0 = never started
static bool         s_usingAP = false;  // true if we had to bring up our own hotspot

/* ── Keeping the screen alive while the server is up ────────────────────────────────
 *
 * The phone dims at 20 s and sleeps at 30 s, and dragging a 5 MB track in from a
 * computer takes longer than that. Worse, the screen going dark mid-upload looks
 * exactly like a crash, so people reach for the keypad — and Back stops the server.
 *
 * Held HERE rather than in each app's transfer screen, because there are three
 * uploaders and this module is the one place that knows when a server is actually
 * running. Restored by xferStop(), which every app already calls from its destructor,
 * so an app dying with the screen up cannot leave the phone with a ten-minute timeout.
 *
 * ⚠ The timeouts are raised, not disabled. An upload screen left open on a forgotten
 * phone should still eventually sleep. */
extern GUI gui;

static bool     s_heldAwake = false;

static void xferHoldAwake(bool hold) {
  if (hold == s_heldAwake) {
    return;                     // stay idempotent: one owner, at most one count
  }
  gui.state.holdScreenAwake(hold);
  s_heldAwake = hold;
}
static char         s_addr[40] = {0};   // shown address (IP of STA or AP)
/* Breaker state is per-SERVER-SESSION: xferStart() clears it, so a fresh Start
 * (or `up on`) always begins live — the first cut kept it in a function-local
 * static, and one trip outlived every restart. */
/* Why a start was refused, for the screen and the serial console. NULL = no refusal.
 * Set by xferStart() and cleared by the next attempt, so it always describes the
 * most recent try rather than an old one. */
static const char*  s_startErr = NULL;
static bool         s_breakerPaused = false;
static uint32_t     s_breakerFlipMs = 0;
static uint32_t     s_lastChunkMs   = 0;       // when a /chunk piece last arrived (breaker pacing)
static uint32_t     s_lastRawMs     = 0;       // when the raw pump last served ANYTHING (breaker pacing)
static uint32_t     s_lowSinceMs    = 0;       // largest has been under the trip line since (0 = it isn't)

static void xferServerUp();     // create + register + begin (also the breaker's resume)
static void xferServerDown();   // stop + delete (also the breaker's pause; mDNS stays up)
static void rawXferPump();      // the raw transport (:80 + :8081), pumped every pass
static void rawAbort();         // drop any in-flight raw request (transport preemption)
static bool xferFetchUrl(const char* urlIn);   // the shared pull path (raw + legacy)

static const XferConfig ROM_CFG = {
  "/roms", "Add Game Boy ROMs", ".gb,.gbc", "ROMs", "download.gbc", "WiPhone-ROMs"
};
/* The books uploader, startable from the serial console (`up on books`) — the
 * bench needs to feed the reader without hands on the phone. Mirrors
 * app_books.cpp's BOOKS_XFER_CFG (same dir, filter, AP name). */
static const XferConfig SERIAL_BOOKS_CFG = {
  "/books", "Add books", ".epub,.txt", "books", "download.epub", "WiPhone-Books"
};
const XferConfig* xferBooksConfig() { return &SERIAL_BOOKS_CFG; }
/* The photos uploader, same argument as the books one: the bench needs to feed the
 * gallery without hands on the phone, and the Files-app route needs a thumb on the
 * folder first. ⚠ The filter is .jpg/.jpeg/.bmp because that is exactly what
 * app_photos.cpp can DECODE (isPhotoName, app_photos.cpp:44) — PNG has no decoder
 * in this firmware or in the ESP32 ROM, so offering it would put files on the card
 * that the gallery then refuses with "baseline JPEG and BMP only". */
static const XferConfig SERIAL_PHOTOS_CFG = {
  "/photos", "Add photos", ".jpg,.jpeg,.bmp", "photos", "download.jpg", "WiPhone-Photos"
};
const XferConfig* xferPhotosConfig() { return &SERIAL_PHOTOS_CFG; }
/* The T9 extra-dictionary uploader (`up on t9`). Its whole reason for existing is that a
 * word list is a thing you ITERATE on — add a name, re-sort, try it — and doing that by
 * powering the phone down and moving the card each time would make the feature not worth
 * having. The file must be named extra.txt; the uploader keeps the browser's filename.
 * See t9_extra.h for the format and tools/gen_t9_extra.py for what writes it. */
static const XferConfig SERIAL_T9_CFG = {
  "/t9", "Add T9 words", ".txt", "word lists", "extra.txt", "WiPhone-T9"
};
const XferConfig* xferT9Config() { return &SERIAL_T9_CFG; }
/* The API-key uploader (`up on keys`): gemini.txt for the AI terminal, from
 * tools/wiphone_send.py --app keys, into the folder the boot makes (WiPhone.ino). On the phone
 * the same thing is Files > API Keys > [ Upload into this folder ]. A key file is only ever
 * stored on the card; nothing here logs what is in it. */
static const XferConfig SERIAL_KEYS_CFG = {
  "/API Keys", "Add API keys", ".txt", "key files", "gemini.txt", "WiPhone-Keys"
};
const XferConfig* xferKeysConfig() { return &SERIAL_KEYS_CFG; }
/* The map-tile uploader (`up on maps`): the ONE tree-mode config. A converted area is
 * `<area>/<z>/<x>/<y>.565` — hundreds of files in nested folders — and the alternative is
 * pulling the card. tools/wiphone_send.py --app maps --tree <dir> walks a converted tree and
 * sends each file under its relative path; chunkSafeTreeName decides what a path may look
 * like and chunkOpenFor creates the folders. `.txt` is for pins.txt. `.elv` (0.9.80) is the
 * altitude layer: `wiphone_send.py --app maps --tree ~/elev-master/elev` lands at
 * /maps/elev/<z>/<x>/<y>.elv - the same tree shape, 131072 bytes a tile like a .565. (A push
 * is not filtered by this list - see the top of this file - but a URL pull is, and the page
 * shows it.) */
static const XferConfig SERIAL_MAPS_CFG = {
  "/maps", "Add map tiles", ".565,.elv,.txt", "tiles", "tile.565", "WiPhone-Maps", true
};
const XferConfig* xferMapsConfig() { return &SERIAL_MAPS_CFG; }
static const XferConfig* s_cfg = &ROM_CFG;

/* One name rule per uploader: basename-only for everything, a validated relative path for
 * the tree-mode config. Both transports (raw and legacy) go through here. */
static bool xferSafeName(const char* in, char* out, size_t cap) {
  return s_cfg->tree ? chunkSafeTreeName(in, out, cap) : chunkSafeName(in, out, cap);
}

void gbcXferHandleClient() {
  if (!s_on) {
    return;
  }
  /* Forgotten-server watchdog. Checked before the pumps so a server that has already
   * gone quiet costs one comparison rather than a full poll. The UPLOADER's only: a KOSync
   * window carries its own hard deadline (kosyncLoop), five minutes at most. */
  if (s_upOn && s_lastActivityMs && (uint32_t)(millis() - s_lastActivityMs) > XFER_IDLE_STOP_MS) {
    log_e("XFER: idle %lu min with no client - stopping itself (it was holding the CPU at full speed)",
          (unsigned long)(XFER_IDLE_STOP_MS / 60000UL));
    xferStop();
    return;
  }
  /* The raw transport rides ABOVE the breaker: its per-request heap cost is
   * zero (static header buffer, bodies straight to PSRAM, one long-lived
   * connection), so pausing it with the WebServer would only punish the one
   * client that cannot cause the pressure. */
  rawXferPump();
  if (!s_server && !s_breakerPaused) {
    return;                 // not started at all
  }
  /* ── LOW-HEAP CIRCUIT BREAKER ──────────────────────────────────────────────
   * Measured 2026-08-20: back-to-back uploads ground the largest internal block
   * from 13 KB to 3.3 KB, at which point lwIP could not allocate for NEW
   * connections — first the listener died (instant connection-refused), then
   * the whole stack went mute (not even ping answered). The browser reads that
   * as "the page locked up", and 3 KB is the altitude where this firmware's
   * historic operator-new abort lives. The server now pauses ITSELF while
   * memory is tight instead of dragging the network stack (and then the whole
   * phone) down with it: the listener closes, nothing else is lost, and it
   * comes back on its own when heap recovers. Hysteresis so it cannot flap. */
  /* Trip threshold from the 2026-08-20 measurements: sequential uploads ran
   * HAPPILY at 7-8 KB largest; the listener only died below ~5 KB. Trip at 6 KB.
   *
   * ⚠ THE RESUME MUST NOT DEPEND ON A HEAP NUMBER ALONE. Two earlier cuts of
   * this breaker both died of the same catch-22 in the field, the SAME DAY they
   * were written: a paused-but-allocated server pins the largest block below
   * whatever resume threshold was chosen, so one trip = paused forever = "the
   * upload page works once per boot" ({user}, from the kitchen). So the pause now
   * FREES the server outright (delete — that is what lets the heap actually
   * recover; mDNS stays up, see xferServerUp), and resume fires on heap recovery OR a timer, whichever
   * comes first — a stranded pause is structurally impossible. A resume that
   * re-trips within a minute doubles the wait, up to 10 min, so a genuinely
   * starved phone breathes instead of flapping. */
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t now = millis();
  if (!s_breakerPaused) {
    /* Mid-upload, backpressure (handleUpload) is the governor — pausing here
     * would abort the very transfer that is about to release the pressure, so
     * the trip defers to it unless memory is truly catastrophic. Between
     * requests the 6 KB line stands. */
    /* While the raw transport is mid-batch the breaker does NOT trip at all.
     * MEASURED (first 4-book raw batch): dips during streaming are the driver's
     * bounded working set; pausing the IDLE port-80 server relieved nothing,
     * and every down/up cycle (MDNS.end/begin + server delete/new inside tight
     * heap) leaked — the per-file floors stair-stepped 3328 → 1536 → 1032 →
     * 616 across one batch, tracking the cycle count, and recovered the moment
     * cycling stopped. The stock trip rules resume 30 s after the last piece. */
    const bool chunkPacedNow = (uint32_t)(now - s_lastChunkMs) < 30000 ||
                               (uint32_t)(now - s_lastRawMs) < 5000;
    /* ^ raw traffic of ANY kind inhibits the trip, not just chunk pieces.
     * MEASURED 2026-08-26 after the page moved to the raw pump: 20 rapid page
     * GETs dipped largest to 188 B in sub-ms bursts (send-buffer pbufs of 2-3
     * back-to-back connections coexisting) and the breaker cycled the IDLE
     * legacy server four times for it — the exact down/up-inside-tight-heap
     * churn the mid-batch rule below was written against, relieving nothing
     * because the WebServer wasn't serving anything. Raw pressure is bounded
     * by construction; the breaker's job is the WebServer's own traffic. */
    const size_t tripAt = s_uploadFile ? 3072 : 6144;
    /* ── TRIP ON SUSTAINED PRESSURE, NOT ON ONE REQUEST'S TRANSIENT ──────────
     * MEASURED 2026-08-26 (docs/uploader-bench-2026-08-26.md): a SINGLE 829-byte
     * page GET dips largest from ~12 K to 3-4 K for well under a second — the
     * WebServer's own request transient — and the old instant judgment read
     * that as the flood, paused, and served {user} "site cannot be loaded" on
     * nearly every browser visit (2 of 10 rapid GETs answered; backoff 90 →
     * 360 → 600 s). The 2026-08-20 flood this breaker was built for pins
     * largest DOWN for many seconds, so: the 6144 line now has to hold for
     * 1.5 s of consecutive passes before it counts. Below 3072 — the altitude
     * of the historic operator-new/PHY aborts — the trip stays INSTANT, so the
     * panic guard is exactly as strong as before. */
    if (largest >= tripAt) {
      s_lowSinceMs = 0;
    } else if (!s_lowSinceMs) {
      s_lowSinceMs = now;
    }
    const bool tripNow = largest < 3072 ||
                         (s_lowSinceMs && (uint32_t)(now - s_lowSinceMs) > 1500);
    if (!chunkPacedNow && tripNow && (uint32_t)(now - s_breakerFlipMs) > 3000) {
      /* Every trip is now a RECYCLE-THEN-SETTLE, not an instant judgment.
       * MEASURED 2026-08-20 (first chunked hardware run): judging recovery
       * synchronously undersells it — lwIP frees pcbs from its own thread, so
       * a teardown that read "5644 → 5644, pause 600s" was at 10088 seconds
       * later. So: tear down, pause, and let the resume check (every loop
       * pass) catch the recovery the moment it lands.
       *
       * How LONG a failed settle may pause depends on who is talking. The
       * chunked page paces itself one 4 KB piece at a time — its traffic
       * cannot be the flood the old escalation was built for, and a long
       * pause just makes a patient client give up (measured: 600 s pause vs
       * the client's ~100 s of retries). Chunk traffic caps the fallback at
       * 8 s and never escalates; the legacy whole-file path keeps the
       * doubling fuse. */
      xferServerDown();
      s_breakerPaused = true;
      s_breakerFlipMs = now;
      s_lowSinceMs = 0;
      log_e("XFER: PAUSED (largest %u) - legacy server freed; back when largest >= 10240",
            (unsigned)largest);
    } else {
      s_server->handleClient();
    }
    return;
  }
  /* ── RESUME ON RECOVERY, NEVER ON A TIMER (changed 0.9.28) ────────────────
   * The timer-resume existed to escape v1/v2's catch-22, where the PAUSED
   * server's own allocation pinned largest below any threshold. Pause has
   * freed the server outright since v3, so heap-driven resume cannot
   * deadlock — and MEASURED tonight, the timer became the fault instead: it
   * re-planted a ~6 KB WebServer into a 4.5 K-largest heap every 90-180 s,
   * where the sustained rule promptly re-paused it. Allocate/free churn
   * inside tight heap is the documented leak, and the page no longer needs
   * this server anyway (the raw pump serves it). 10240 is the same bar
   * xferStart() demands to start serving at all — one number, one meaning:
   * the framework server LIVES here or it does not run. */
  if (largest >= 10240 && (uint32_t)(now - s_breakerFlipMs) > 3000) {
    s_breakerPaused = false;
    s_breakerFlipMs = now;
    xferServerUp();
    log_e("XFER: RESUMED (largest %u)", (unsigned)largest);
  }
}

bool        gbcXferOn()      { return s_upOn; }
bool        xferServing()    { return s_on; }
bool        xferWindowUp()   { return s_on && s_winOn; }
bool        xferUsingAP()    { return s_usingAP; }
bool        xferApProtected() { return s_on && s_usingAP && s_apProtected; }
bool        xferApByUploader() { return s_on && s_usingAP && s_apByUploader; }
const char* xferAddr()       { return s_addr; }
const char* xferStartError() { return s_startErr; }
// The SSID actually on the air when we host one (a window may have brought it up).
const char* xferApName()     { return (s_usingAP && s_apName[0]) ? s_apName : s_cfg->apName; }
int         xferFilesAdded() { return s_filesAdded; }

void gbcXferStart() {
  xferStart(&ROM_CFG);
}

/* The page served to the computer's browser: drag-and-drop / file-pick upload plus a
 * "download from URL" box. Kept small and dependency-free. Sent in pieces with the config's
 * words spliced in — a printf template would have to escape every '%' in the CSS, which is
 * exactly the kind of edit that breaks a page nobody re-reads. */
/* The page served to a browser. PHONE FIRST: this device is mostly fed from a phone, and
 * the first version was built around drag-and-drop, which does not exist on a touch
 * screen. What was left there was a bare <input type=file> in the middle of a dashed box,
 * easy to miss and awkward to hit.
 *
 * ⚠ NO `accept` ATTRIBUTE, deliberately. Android's file picker takes an extension list
 * like ".mp3,.wav" and commonly greys out EVERY file rather than filtering to those —
 * which looks exactly like a page that does not respond to taps. The server has never
 * filtered by extension anyway (that is what musicIsPlayable and the parsers are for), so
 * claiming to is worse than useless here.
 *
 * Sent in pieces with the config's words spliced in — a printf template would have to
 * escape every '%' in the CSS, which is the kind of edit that breaks a page nobody
 * re-reads. */
static const char PAGE_1[] =
  /* charset FIRST: the server sends no charset in Content-Type, so without
   * this meta the browser guesses Latin-1 and every em dash in the status
   * line renders as mojibake (caught on the pre-flash mock, 2026-08-20). */
  "<!doctype html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>WiPhone</title><style>"
  "body{font-family:sans-serif;max-width:520px;margin:16px auto;padding:0 16px;color:#222}"
  "h1{font-size:20px}h2{margin-top:24px;font-size:16px}"
  "#drop{border:2px dashed #888;border-radius:10px;padding:18px;text-align:center;color:#666}"
  "#drop.over{border-color:#2a7;background:#eafaf1}"
  "input[type=text]{width:100%;padding:12px;box-sizing:border-box;font-size:16px}"
  ".btn{display:block;width:100%;padding:16px;margin-top:10px;border:0;border-radius:8px;"
  "background:#2a7;color:#fff;font-size:17px;text-align:center;box-sizing:border-box}"
  ".btn.alt{background:#456}"
  "#pick{display:none}"          // the real input; the label below is what gets tapped
  "#chosen{margin-top:10px;color:#555;font-size:14px;word-break:break-all}"
  "</style></head><body>"
  "<h1>WiPhone &mdash; ";                                     // heading
static const char PAGE_2[] =
  "</h1>"
  "<form id=f method=POST action=/upload enctype=multipart/form-data>"
  "<input type=file id=pick name=rom multiple>"
  "<label class=btn for=pick>Choose files&hellip;</label>"
  "<div id=chosen>No files chosen yet.</div>"
  "<button class='btn alt' type=submit>Upload</button>"
  "<div id=drop style='margin-top:14px'>&hellip;or drag files here (on a computer)</div>"
  "</form>"
  "<h2>Or paste a download link</h2>"
  "<form method=POST action=/fetch>"
  "<input type=text name=url placeholder='https://... direct link to a file'>"
  "<button class='btn alt' type=submit>Download to phone</button></form>"
  "<p style='color:#888;margin-top:26px'>Saved to ";           // dir
static const char PAGE_3[] =
  " on the SD card.</p>"
  "<script>"
  "var d=document.getElementById('drop'),f=document.getElementById('f'),"
  "inp=document.getElementById('pick'),ch=document.getElementById('chosen');"
  "function names(fs){var a=[];for(var i=0;i<fs.length;i++)a.push(fs[i].name);return a.join(', ');}"
  "inp.addEventListener('change',function(){"
  "ch.textContent=inp.files.length?names(inp.files):'No files chosen yet.';});"
  /* THE CHUNKED SENDER (2026-08-20/21 redesign). One piece per request, each
   * awaiting the phone's ack before the next departs; every piece carries a
   * CRC32 the server verifies before committing, and a failed piece resyncs to
   * the server's held-byte count — so a dropped connection, a breaker pause, or
   * a reboot costs pieces, never files. On load the page probes the RAW port
   * (8081): one long-lived connection, raw text/plain bodies (CORS-simple),
   * 8 KB pieces — the fast path. No raw port answering ⇒ same-origin multipart
   * /chunk at 4 KB — the compatible path. Browsers with JS but no fetch() keep
   * the native form POST to /upload (nothing is preventDefault'ed for them);
   * curl keeps /upload too. */
  // CRC32 (IEEE) — must agree with chunk_proto.h's, or every piece is refused.
  "var CT=null;"
  "function crct(){if(CT)return CT;CT=[];for(var n=0;n<256;n++){var c=n;"
  "for(var k=0;k<8;k++)c=(c&1)?(3988292384^(c>>>1)):(c>>>1);CT[n]=c;}return CT;}"
  "function crc32(u){var t=crct(),c=4294967295;"
  "for(var i=0;i<u.length;i++)c=t[(c^u[i])&255]^(c>>>8);return(c^4294967295)>>>0;}"
  "function hex8(v){var s=v.toString(16);while(s.length<8)s='0'+s;return s;}"
  // Piece sizes are measured, not guessed: 4 KB costs the WebServer path
  // nothing; the raw path's in-flight is window-bounded so 8 KB is free there.
  "var PIECE=4096,BASE='',probed=false;"
  "var RAWB='http://'+location.hostname+':8081';"
  "function probe(cb){"
  /* A page served by the raw server SAYS SO (the spliced RAWPAGE tail): same
   * origin IS the raw transport there, so the probe has nothing to discover —
   * and must not run, because its failure path would select the multipart
   * fallback, which the raw server refuses. */
  "if(window.RAWPAGE&&!probed){probed=true;PIECE=16384;}"
  "if(probed){cb();return;}"
  "var done=false,t=setTimeout(function(){if(!done){done=true;probed=true;cb();}},1500);"
  "fetch(RAWB+'/chunk?name=probe.bin',{cache:'no-store'}).then(function(r){"
  "if(!done){done=true;probed=true;clearTimeout(t);if(r.ok){BASE=RAWB;PIECE=16384;}cb();}"
  "}).catch(function(){if(!done){done=true;probed=true;clearTimeout(t);cb();}});"
  "}"
  "function sendAll(files){"
  "if(!files||!files.length){ch.textContent='No files chosen.';return;}"
  "var fi=0,sent=0,failed=[],skipped=0;"
  "probe(function(){nextFile();});"
  "function finish(){"
  "var m='Done! '+sent+' file(s) on your phone.';"
  "if(skipped)m+=' ('+skipped+' empty file(s) skipped)';"
  "if(failed.length)m='Sent '+sent+' — FAILED: '+failed.join(', ');"
  "m+=' · '+((BASE||window.RAWPAGE)?'fast':'compatible')+' chunked transport';"
  "ch.textContent=m;}"
  "function nextFile(){"
  "if(fi>=files.length){finish();return;}"
  "var file=files[fi],off=0,tries=0,lastSync=-1,stalls=0;"
  "if(!file.size){skipped++;fi++;nextFile();return;}"
  /* ⚠ NAME THE TRANSPORT ON SCREEN. Both paths look identical while they run, and
   * the difference is not cosmetic: the chunked paths cost the heap nothing, while
   * the native form POST to /upload fragments the largest free block ~1.5 KB PER
   * FILE and trips the breaker after about six of them. On 2026-08-25 that ambiguity
   * cost an hour — curl was silently on /upload while the page was chunking, and
   * nothing on either end said which was which. "fast" = raw port 8081, 16 KB
   * pieces; "compatible" = same-origin /chunk, 4 KB. Either is safe; the form POST
   * is the one to notice, and it cannot reach this line at all. */
  "function stat(x){ch.textContent='Uploading '+(fi+1)+' of '+files.length+': '+file.name+"
  "' — '+Math.floor(off*100/file.size)+'%'+(x||'')+' ['+((BASE||window.RAWPAGE)?'fast':'compatible')+' chunked]';}"
  // A file that gives up does NOT sink the batch: note it, move on.
  "function giveUp(why){failed.push(file.name+(why?' ('+why+')':''));fi++;setTimeout(nextFile,300);}"
  "function fail(){tries++;"
  "if(tries>24){giveUp('unreachable');return;}"
  "stat(' (retry '+tries+')');setTimeout(resync,Math.min(500*tries,5000));}"
  // Resync = ask the phone how much it holds, continue from there. A resync
  // that never advances (a dying SD acks buffered pieces, then loses them)
  // trips the stall counter instead of cycling forever.
  "function resync(){fetch(BASE+'/chunk?name='+encodeURIComponent(file.name),{cache:'no-store'})"
  ".then(function(r){return r.text();}).then(function(t){"
  "var n=parseInt(t,10);"
  "if(n>=0&&n<=file.size){"
  "if(n<=lastSync){stalls++;if(stalls>5){giveUp('no progress — SD trouble?');return;}}"
  "else{stalls=0;}"
  "lastSync=n;off=n;}"
  "piece();})"
  ".catch(fail);}"
  "function piece(){"
  "if(off>=file.size){sent++;fi++;setTimeout(nextFile,300);return;}"
  "var end=Math.min(off+PIECE,file.size),fr=new FileReader();"
  "fr.onerror=fail;"
  "fr.onload=function(){"
  "var u=new Uint8Array(fr.result);"
  "var url=BASE+'/chunk?name='+encodeURIComponent(file.name)+'&off='+off+'&crc='+hex8(crc32(u))+"
  "'&last='+(end>=file.size?1:0);"
  /* RAW covers both raw transports: cross-origin :8081 (BASE set) and a RAWPAGE
   * served same-origin by the raw server (BASE=''). ⚠ The first cut keyed this on
   * BASE alone, so a raw-served page fell into the multipart branch — which the
   * raw server refuses (multipart overhead pushes a 16 K piece past the cap).
   * Caught by the first REAL-browser run, 2026-08-27; the bench pushers all
   * spoke Blob directly and could not see it. */
  "var RAW=BASE||window.RAWPAGE;"
  "var opts;"
  "if(RAW){opts={method:'POST',body:new Blob([u],{type:'text/plain'})};}"
  "else{var fd=new FormData();fd.append('p',new Blob([u]),'p');opts={method:'POST',body:fd};}"
  "fetch(url,opts)"
  ".then(function(r){return r.text().then(function(t){return{ok:r.ok,c:r.status,t:t};});})"
  ".then(function(res){"
  "var m=res.t.match(/(\\d+)/),n=m?parseInt(m[1],10):-1;"
  "if(res.ok){tries=0;off=(n>off&&n<=file.size)?n:end;stat('');piece();}"
  "else if(res.c==409&&n>=0&&n<=file.size){off=n;tries++;"
  "if(tries>24){giveUp('kept resyncing');}else{setTimeout(piece,150);}}"
  "else if(res.c==400||res.c==413||res.c==404){giveUp('refused: '+res.t);}"
  "else{fail();}})"
  ".catch(fail);};"
  "fr.readAsArrayBuffer(file.slice(off,end));}"
  "stat('');piece();}"
  "}"
  /* fetch()-less JS browsers never enter the chunked path: no preventDefault,
   * so the form's native POST /upload still works — the fallback the first
   * version of this sender silently killed. */
  "if(window.fetch){"
  "['dragenter','dragover'].forEach(function(e){d.addEventListener(e,function(ev){ev.preventDefault();d.classList.add('over')})});"
  "['dragleave','drop'].forEach(function(e){d.addEventListener(e,function(ev){ev.preventDefault();d.classList.remove('over')})});"
  "d.addEventListener('drop',function(ev){sendAll(ev.dataTransfer.files)});"
  "f.addEventListener('submit',function(ev){ev.preventDefault();sendAll(inp.files)});"
  "}"
  "</script></body></html>";

static void handleRoot() {
  /* ⚠ Connection: close on EVERY response, and it is not cosmetic.
   *
   * This server is single-client and pumped once per main-loop pass. A browser that keeps
   * its socket open — which phone browsers do by default — occupies the one slot, and
   * every later request just hangs. That is the "load it once, then it never loads
   * again" this page used to do. Closing after each response gives the slot straight
   * back. */
  s_server->sendHeader("Connection", "close");
  s_server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_server->send(200, "text/html", "");
  /* _P variants, and it is not pedantry: sendContent(const char*) round-trips
   * every part through a heap String — ~6 KB of copies per page view on top of
   * the framework's own request machinery, deep enough on a 12-15 K largest to
   * reach the breaker's instant tier (measured 2026-08-26: page GETs bottoming
   * at 1.6 K). sendContent_P streams straight from flash through a small
   * chunk buffer; the page costs the heap nearly nothing. */
  s_server->sendContent_P(PAGE_1);
  s_server->sendContent_P(s_cfg->heading);
  s_server->sendContent_P(PAGE_2);
  s_server->sendContent_P(s_cfg->dir);
  s_server->sendContent_P(PAGE_3);
  s_server->sendContent("");            // terminate the chunked response
}

/* Browsers ask for this unprompted, and an unanswered request holds the only client slot
 * until it times out — the same starvation as keep-alive, from a request the user never
 * made. 204 costs nothing and frees the slot immediately. */
static void handleFavicon() {
  s_server->sendHeader("Connection", "close");
  s_server->send(204, "text/plain", "");
}

/* Hands back /health.log so the phone can be run on BATTERY for hours — the only way to
 * measure drain — and read afterwards without a cable. Also survives a reboot, which is
 * the point for chasing an unexplained restart: the reset reason of the run that died is
 * sitting at the top of the next run's entries. */
static void handleLog() {
  s_server->sendHeader("Connection", "close");
  File f = SD.open("/health.log");
  if (!f) {
    s_server->send(404, "text/plain", "no /health.log yet");
    return;
  }

  /* ?tail=N hands back only the LAST N bytes. Without it, read the whole file.
   *
   * Whole-file reads truncate in transit often enough that pulling 20 KB took thirty-odd
   * requests on 2026-08-14, and each attempt is a fresh TCP connection parsed through this
   * server's Strings. Internal heap is the one thing this phone has none of, so the reading
   * of the log was a real part of what the log was measuring: `largest` oscillated between
   * 3,084 and 6,660 bytes while that was going on, and the phone pa{user}ed during it.
   *
   * A truncated response also loses the END of the file — which is exactly where the reset
   * reason of the run that just died is written, so the one thing worth reading is the first
   * thing lost. The tail is both smaller and the part actually wanted.
   *
   * ⚠ Content-Length is taken AFTER the resync below, never from f.size(). A length that
   * disagrees with the body by one byte leaves the client waiting for a remainder that is
   * never sent. */
  const size_t total = f.size();
  if (s_server->hasArg("tail")) {
    const long want = s_server->arg("tail").toInt();
    if (want > 0 && (size_t)want < total) {
      f.seek(total - (size_t)want);
      /* Landing `want` bytes from the end lands mid-line almost every time. Step over the
       * remainder of it so the response starts on a whole line and stays parseable. */
      while (f.available() && f.read() != '\n') {
      }
    }
  }

  s_server->setContentLength(total - (size_t)f.position());
  s_server->send(200, "text/plain", "");
  uint8_t buf[512];
  while (f.available()) {
    const size_t n = f.read(buf, sizeof(buf));
    if (n == 0) {
      break;
    }
    s_server->client().write(buf, n);
    delay(1);                 // the same yield the upload path needs, for the same reason
  }
  f.close();
}

static void handleNotFound() {
  s_server->sendHeader("Connection", "close");
  s_server->send(404, "text/plain", "Not found");
}

// Streams an uploaded multipart file straight to the configured folder on the SD card.
/* Uploads arrive in ~2 KB pieces and used to go straight to the card, one write each.
 * A 6 MB track is then over three thousand small FAT writes, and the slow ones — the
 * cluster allocations — stall the main loop long enough for the phone to stop answering
 * the network at all. Measured mid-upload: ping to the phone went from ~4 ms to 390 ms,
 * throughput fell from 250 KB/s to 64 KB/s, and the connection was eventually reset by
 * the phone. That is the "loaded halfway then crashed" this used to do.
 *
 * So pieces are gathered in PSRAM and committed in 32 KB blocks: a couple of hundred big
 * writes instead of thousands of small ones. PSRAM because the internal heap has no room
 * to spare — the same rule as everything else here. */
#define XFER_SD_BLOCK  (32 * 1024)
static uint8_t* s_sdBuf = NULL;
static size_t   s_sdLen = 0;

/* Per-request transient anatomy (upload-redesign brief): what ONE request
 * costs internal heap is the number that picks the chunk size, and it had
 * never been itemized. Captured here because serial cannot ask mid-POST —
 * handleClient() owns the main loop until the request completes, so the
 * handler is the only place that can watch its own footprint. */
static size_t s_reqLargest0 = 0;   // largest internal block at request START
static size_t s_reqFloor    = 0;   // lowest largest seen during this request

/* ── Chunked stop-and-wait upload — the 2026-08-20 redesign ─────────────────────────
 *
 * MEASURED (this hardware, fast LAN, docs/upload-redesign-brief.md): a whole-file
 * POST lets TCP run at line rate and the WiFi driver floods internal heap with RX
 * buffers below the application — 14 KB → 868 bytes in seconds. Graded probes put
 * numbers on it: a 4 KB body costs NOTHING beyond the ~10 KB request machinery, an
 * 8 KB body dips 4 KB further, 16 KB grinds the largest block to 636 BYTES, and a
 * vanished client mid-POST wedged the whole main loop (the flooded RX window kept
 * the FIN undeliverable, and the parser's read loop spins while `connected()`).
 *
 * So the page now sends ONE small piece per request and waits for the ack before
 * the next departs. That bounds the radio's burst at the piece size BY
 * CONSTRUCTION — no breaker or backpressure on the happy path — and it bounds the
 * wedge too: a piece is smaller than the TCP window, so the window can never fill
 * and a dead client's FIN always gets through. The protocol (offsets, CRC,
 * idempotence, the resync rule) lives in chunk_proto.h, host-tested.
 *
 * The file stays OPEN across pieces and bytes gather in the same 32 KB PSRAM block
 * the legacy path uses — SD sees the identical few-big-writes pattern that fixed
 * the mid-upload network dropouts. An acked piece is therefore in PSRAM or on the
 * card, never lost EXCEPT to a reboot; the client's resync (`GET /chunk`) covers
 * that. The buffer belongs to whichever path has a file open — the two STARTs
 * finalize each other, so the paths cannot interleave mid-file. */
#define CHUNK_PIECE_MAX  8192     // WebServer-path cap (its page sends 4096)
#define CHUNK_FLUSH_AT   16384    // flush when this full: always one RAW piece of room left
static File     s_chunkFile;                // open across the pieces of one file
static char     s_chunkName[64] = {0};      // sanitized name of that file
/* ⚠ Durable bytes are COUNTED here, never stat'd. CONFIRMED in this core's
 * vfs_api.cpp: File::size() after a write stats the PATH, and FatFS updates a
 * file's directory entry only on sync/close — so size() on the open handle
 * reports the size as of the last flush, not the truth. The one moment size()
 * is trustworthy is right after open (the file was closed then, and close
 * syncs). So: read it once at open, then count. */
static size_t   s_chunkDurable = 0;         // bytes on the card through the open handle
static uint32_t s_chunkCrc = 0;             // running CRC of the arriving piece
static uint32_t s_chunkWantCrc = 0;         // the CRC the client claims
static size_t   s_chunkPieceLen = 0;        // bytes of the current piece gathered
static size_t   s_chunkOff = 0;             // offset the client claims
static size_t   s_chunkHeld = 0;            // durable + buffered when the piece began
static bool     s_chunkLast = false;
static bool     s_chunkGather = false;      // this piece lands at the held boundary
static bool     s_chunkSawPiece = false;    // a multipart part actually arrived
static int      s_chunkCode = 500;          // the reply, decided in the upload callback
static char     s_chunkReply[32] = "err";
static uint32_t s_chunkPieces = 0;          // pieces committed to the open file
static size_t   s_fileFloorLargest = 0;     // per-FILE heap floors (the acceptance bar)
static size_t   s_fileFloorFree = 0;

/* 🛑 THE KEY FOLDER IS WRITE-ONLY HERE (2026-10-11). A key file - anything inside an "API Keys"
 * folder, or a gemini.txt anywhere (files_paths.h, filePathIsKeyFile: what the Files viewer will
 * not draw) - is never stat'd, read or served by this server, which may be an OPEN hotspot. No
 * route reads one (the page, /log = /health.log only, /chunk, /fetch, /upload write), but the
 * RESUME machinery did: GET /chunk?name=gemini.txt answered a key's existence and length to anyone
 * on the network (the keys uploader, or the Files uploader in /API Keys or at the card's root), a
 * POST at off > 0 opened it for append and answered "have <its length>" - and a NEW key the same
 * length as the old (an AIza key is always 39 characters) read as "already held", so the browser
 * said sent and sent nothing. Now the resume question is answered from THIS session's upload alone
 * (xferHeld), and a resume of a key file this session is not writing is told "have 0"
 * (chunkOpenFor): a key is always written fresh, from its first byte. */
static bool xferKeyPath(const String& path) {
  return filePathIsKeyFile(path.c_str()) != 0;
}

/* GET /chunk?name= (both transports): how many bytes of <name> the card holds - buffered bytes
 * included for the file being written (an acked piece is as good as written short of a reboot,
 * and after a reboot the buffer is empty so the durable number comes out anyway). A key file the
 * session is not writing: 0, never stat'd. */
static size_t xferHeld(const char* name) {
  if (s_chunkFile && !strcmp(name, s_chunkName)) {
    return s_chunkDurable + s_sdLen;   // counted, not stat'd — size() lies while open
  }
  const String path = String(s_cfg->dir) + "/" + name;
  if (xferKeyPath(path)) {
    return 0;                          // the key folder is write-only
  }
  File f = SD.open(path.c_str());
  const size_t held = f ? (size_t)f.size() : 0;
  if (f) {
    f.close();
  }
  return held;
}

/* Flush the buffered block into the open chunk file. False = the SD lied or
 * failed; the caller must fall back to durable truth (the client resyncs). */
static bool chunkFlush() {
  if (!s_chunkFile || !s_sdBuf || s_sdLen == 0) {
    return true;
  }
  const size_t wrote = s_chunkFile.write(s_sdBuf, s_sdLen);
  const bool ok = (wrote == s_sdLen);
  s_chunkDurable += wrote;      // count what actually landed, even on a short write
  s_sdLen = 0;
  if (ok) {
    /* f_sync per block: the directory entry stays honest (a crash costs at
     * most one block of resend) and it is one sync per 24 KB, not per piece. */
    s_chunkFile.flush();
  }
  return ok;
}

/* Commit one CRC-verified piece of `len` bytes sitting at s_sdBuf[s_sdLen..].
 * The ONE commit path, shared by both transports (WebServer /chunk and the raw
 * port), so the flush policy, the counters, and the reply arithmetic cannot
 * drift apart. Returns the HTTP code; writes the reply text. */
static void chunkFinalize();
static int chunkCommit(size_t len, bool last, char* reply, size_t replyCap) {
  s_sdLen += len;                     // the piece is now buffered file data
  bool ok = true;
  if (last || s_sdLen >= CHUNK_FLUSH_AT) {
    ok = chunkFlush();
  }
  if (!ok) {
    /* The card refused the write. Fall back to durable truth: close out,
     * answer 507; the client resyncs and re-sends what the card lost. */
    log_e("XFER: chunk SD write FAILED ('%s') - answering 507", s_chunkName);
    chunkFinalize();
    strlcpy(reply, "sd", replyCap);
    return 507;
  }
  s_chunkPieces++;                    // counted only past the failure exit: no double-count on a 507 retry
  if (last) {
    s_filesAdded++;
    log_e("XFER: '%s' chunked done (file %d this session) size=%u pieces=%u "
          "file-floor: largest=%u free=%u",
          s_chunkName, s_filesAdded, (unsigned)s_chunkDurable, (unsigned)s_chunkPieces,
          (unsigned)s_fileFloorLargest, (unsigned)s_fileFloorFree);
    s_chunkFile.close();              // close syncs the dir entry; size() is honest again
    s_chunkName[0] = '\0';
  }
  snprintf(reply, replyCap, "ok %u", (unsigned)(s_chunkDurable + s_sdLen));
  return 200;
}

/* Make every acked byte durable WITHOUT closing the batch: flush the buffered
 * block and sync. This is what a breaker pause wants — the FIRST hardware run
 * of the raw transport showed pauses calling full finalize and chopping one
 * 374-piece book into ~17 open/close segments for no benefit (the raw client
 * was not going down; only the port-80 server was). */
static void chunkPersist() {
  if (s_chunkFile) {
    if (!chunkFlush()) {
      log_e("XFER: chunk flush FAILED at persist ('%s') - client will resync", s_chunkName);
    }
    s_chunkFile.flush();
  }
}

/* Close out the open chunk file (server stop, path switch, or batch end).
 * Buffered bytes are flushed first — that is what makes an ack mean "will
 * survive a teardown". A partial file stays on the card; resume finds it. */
static void chunkFinalize() {
  if (s_chunkFile) {
    if (!chunkFlush()) {
      log_e("XFER: chunk flush FAILED at finalize ('%s') - client will resync", s_chunkName);
    }
    s_chunkFile.flush();
    s_chunkFile.close();
  }
  s_chunkName[0] = '\0';
  s_sdLen = 0;
  s_chunkPieceLen = 0;
  s_chunkGather = false;
}

/* Open (or keep) the batch file for `name` with the client claiming offset
 * `off` — the ONE file-lifecycle path, shared by both transports. off==0 is a
 * deliberate fresh start and truncates. False = reply already decided. */
static bool chunkOpenFor(const char* name, size_t off, int* code, char* reply, size_t replyCap) {
  if (!s_sdBuf) {
    s_sdBuf = (uint8_t*)ps_malloc(XFER_SD_BLOCK);
    if (!s_sdBuf) {
      *code = 507;
      strlcpy(reply, "no mem", replyCap);
      return false;
    }
  }
  if (!s_chunkFile || strcmp(name, s_chunkName) || off == 0) {
    String path = String(s_cfg->dir) + "/" + name;
    if (off != 0 && xferKeyPath(path)) {
      /* A key file is never resumed from the card (xferKeyPath): its length is not told, nothing
       * is appended to it. The client starts it again at 0, which truncates. */
      *code = 409;
      strlcpy(reply, "have 0", replyCap);
      return false;
    }
    chunkFinalize();
    strlcpy(s_chunkName, name, sizeof(s_chunkName));
    if (off == 0) {
      SD.mkdir(s_cfg->dir);
      /* Tree mode: create each folder on the way down. SD.mkdir is one level at a time and
       * returns false for a folder that already exists, which is the common case and fine. */
      for (int i = (int)strlen(s_cfg->dir) + 1; i < (int)path.length(); i++) {
        if (path[i] == '/') {
          SD.mkdir(path.substring(0, i).c_str());
        }
      }
      SD.remove(path.c_str());
    }
    /* FILE_APPEND, emphatically not FILE_WRITE: in this core FILE_WRITE is
     * "w", which TRUNCATES an existing file — reopening a partial file to
     * resume it would silently discard every byte already uploaded. Append
     * mode creates-if-missing and positions at the end. */
    s_chunkFile = SD.open(path.c_str(), FILE_APPEND);
    if (!s_chunkFile) {
      s_chunkName[0] = '\0';
      *code = 507;
      strlcpy(reply, "sd open", replyCap);
      return false;
    }
    /* size() is trustworthy HERE and only here — the file was closed until a
     * moment ago (close syncs the dir entry). From now on, count. */
    s_chunkDurable = (off == 0) ? 0 : (size_t)s_chunkFile.size();
    /* Per-SEGMENT telemetry, initialized at EVERY open — a file resumed at
     * off>0 used to inherit floors of 0 (or the previous file's), and the
     * acceptance bar would have been read off garbage. */
    s_chunkPieces = 0;
    s_fileFloorLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_fileFloorFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return true;
}

static bool xferFlushBlock() {
  if (!s_uploadFile || !s_sdBuf || s_sdLen == 0) {
    return true;
  }
  const size_t wrote = s_uploadFile.write(s_sdBuf, s_sdLen);
  const bool ok = (wrote == s_sdLen);
  s_sdLen = 0;
  return ok;
}

/* GET /chunk?name= — how many bytes of <name> this card holds (xferHeld: a key
 * file is never stat'd). The client's resync question. */
static void handleChunkGet() {
  s_server->sendHeader("Connection", "close");
  char name[sizeof(s_chunkName)];
  if (!xferSafeName(s_server->arg("name").c_str(), name, sizeof(name))) {
    s_server->send(400, "text/plain", "bad name");
    return;
  }
  const size_t held = xferHeld(name);
  char out[16];
  snprintf(out, sizeof(out), "%u", (unsigned)held);
  s_server->send(200, "text/plain", out);
}

/* The upload callback for POST /chunk — gathers ONE piece, decides the reply.
 * The verdict logic is chunk_proto.h's; this function only supplies the SD
 * facts and obeys. The reply is sent by the completion handler because the
 * framework calls this mid-parse, before a response may go out. */
static void handleChunkData() {
  HTTPUpload& up = s_server->upload();
  if (up.status == UPLOAD_FILE_START) {
    s_chunkSawPiece = true;
    s_chunkGather = false;
    s_chunkPieceLen = 0;
    s_lastChunkMs = millis();
    s_lastActivityMs = millis();
    s_chunkCode = 500;
    strlcpy(s_chunkReply, "err", sizeof(s_chunkReply));
    s_reqLargest0 = s_reqFloor =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    rawAbort();                     // a WebServer piece takes the shared buffer; raw resyncs
    if (s_uploadFile) {
      s_uploadFile.close();         // a dangling legacy upload must not share the buffer
      s_sdLen = 0;
    }
    char name[sizeof(s_chunkName)];
    if (!xferSafeName(s_server->arg("name").c_str(), name, sizeof(name))) {
      s_chunkCode = 400;
      strlcpy(s_chunkReply, "bad name", sizeof(s_chunkReply));
      return;
    }
    if (!chunkParseHex32(s_server->arg("crc").c_str(), &s_chunkWantCrc)) {
      s_chunkCode = 400;
      strlcpy(s_chunkReply, "bad crc", sizeof(s_chunkReply));
      return;
    }
    s_chunkOff = (size_t)strtoul(s_server->arg("off").c_str(), NULL, 10);
    s_chunkLast = (s_server->arg("last") == "1");
    if (!chunkOpenFor(name, s_chunkOff, &s_chunkCode, s_chunkReply, sizeof(s_chunkReply))) {
      return;
    }
    s_chunkHeld = s_chunkDurable + s_sdLen;
    /* Only a piece at the boundary gathers; the rest is judged at END, when
     * the length is known (a duplicate resend acks, anything else resyncs). */
    if (s_chunkOff == s_chunkHeld) {
      s_chunkGather = true;
      s_chunkCrc = chunkCrc32Init();
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    {
      const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (largest < s_reqFloor) {
        s_reqFloor = largest;
      }
      if (largest < s_fileFloorLargest) {
        s_fileFloorLargest = largest;
      }
      if (freeb < s_fileFloorFree) {
        s_fileFloorFree = freeb;
      }
    }
    if (s_chunkGather) {
      if (s_chunkPieceLen + up.currentSize > CHUNK_PIECE_MAX ||
          s_sdLen + s_chunkPieceLen + up.currentSize > XFER_SD_BLOCK) {
        s_chunkGather = false;
        s_chunkCode = 413;
        strlcpy(s_chunkReply, "too big", sizeof(s_chunkReply));
      } else {
        memcpy(s_sdBuf + s_sdLen + s_chunkPieceLen, up.buf, up.currentSize);
        s_chunkCrc = chunkCrc32Update(s_chunkCrc, up.buf, up.currentSize);
        s_chunkPieceLen += up.currentSize;
      }
    }
    delay(1);                       // the same starvation-feeding yield as the legacy path
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_chunkCode != 500 && !s_chunkGather) {
      return;                       // verdict already made at START (bad args / SD / cap)
    }
    if (s_chunkGather) {
      if (chunkCrc32Final(s_chunkCrc) != s_chunkWantCrc) {
        s_chunkCode = 422;
        strlcpy(s_chunkReply, "crc", sizeof(s_chunkReply));
        s_chunkPieceLen = 0;        // drop the piece; buffered committed bytes stand
        return;
      }
      const size_t pieceLen = s_chunkPieceLen;
      s_chunkPieceLen = 0;
      s_chunkCode = chunkCommit(pieceLen, s_chunkLast, s_chunkReply, sizeof(s_chunkReply));
      return;
    }
    /* Not gathered: judge the piece now that its length is known. */
    switch (chunkDecide(s_chunkHeld, s_chunkOff, up.totalSize)) {
      case CHUNK_DUPLICATE:
        s_chunkCode = 200;
        snprintf(s_chunkReply, sizeof(s_chunkReply), "ok %u", (unsigned)s_chunkHeld);
        break;
      case CHUNK_BAD:
        s_chunkCode = 400;
        strlcpy(s_chunkReply, "empty", sizeof(s_chunkReply));
        break;
      default:
        s_chunkCode = 409;
        snprintf(s_chunkReply, sizeof(s_chunkReply), "have %u", (unsigned)s_chunkHeld);
        break;
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    /* The connection died mid-piece. Committed bytes stand; the batch file
     * stays open — the client retries the piece on its next breath. The
     * saw-piece flag resets HERE because an aborted parse never reaches the
     * completion handler, and a stale flag would hand the NEXT request this
     * request's verdict. */
    s_chunkPieceLen = 0;
    s_chunkGather = false;
    s_chunkSawPiece = false;
  }
}

/* POST /chunk completion — sends whatever verdict the upload callback left.
 * A POST that carried no multipart part at all never ran the callback. */
static void handleChunkDone() {
  s_server->sendHeader("Connection", "close");
  if (!s_chunkSawPiece) {
    s_server->send(400, "text/plain", "no piece");
    return;
  }
  s_chunkSawPiece = false;
  s_server->send(s_chunkCode, "text/plain", s_chunkReply);
}

/* ── THE RAW TRANSPORT (port 8081) — why a second server exists at all ────────────────
 *
 * MEASURED 2026-08-20/21 on hardware: the framework WebServer costs ~10 KB of
 * internal-heap transient and ~1.6-2 KB of fragmentation PER REQUEST, and every
 * request is its own TCP connection (the library hardcodes Connection: close)
 * whose TIME_WAIT pcb haunts the heap for 120 s. At one 4 KB piece per request a
 * book is hundreds of requests, and two real runs — unpaced AND paced to 150 ms —
 * both ground the largest block to ~5 K and died flapping. Pacing does not fix a
 * per-request cost.
 *
 * So the chunk protocol gets a transport built for it: ONE TCP connection carries
 * the whole batch (keep-alive), requests are parsed by a ~150-line incremental
 * state machine pumped from the main loop (never blocks: reads only what is
 * available, with a deadline for dead clients), bodies are RAW bytes read straight
 * into the PSRAM block at OUR pace. In-flight data is bounded by the TCP window
 * (5,744 B) AND by stop-and-wait, so the RX flood that started all this is
 * impossible by construction, and per-request heap cost is ZERO — no Strings, no
 * HTTPUpload, no new connections. The verdicts, the store, and the commit path are
 * the same chunk_proto.h + chunkOpenFor/chunkCommit the WebServer path uses; that
 * path stays as the fallback (and the page probes 8081 and falls back by itself).
 *
 * CORS: the page lives on port 80, so 8081 is cross-origin. Pieces are sent as
 * text/plain (a CORS-simple request — no preflight in any browser that matters)
 * and every reply carries Access-Control-Allow-Origin: *; OPTIONS is answered
 * anyway for the browsers that ask first. */
#define RAW_PORT       8081
/* ── AND SINCE 0.9.28, THE RAW SERVER IS ALSO THE FRONT DOOR ON PORT 80 ─────────────
 * MEASURED 2026-08-26 (docs/uploader-bench-2026-08-26.md): the WebServer's request
 * MACHINERY — not the page bytes; sendContent_P changed nothing — costs a ~10 KB
 * internal-heap transient per fresh connection, which from a 13-15 K idle largest
 * (SIP up) bottoms at 1.6-3 K: the instant breaker tier, the PHY-abort altitude.
 * So nearly every browser VISIT (page + favicon + probe) tripped the breaker and
 * the next visitor got "site cannot be loaded". No tuning fixes a per-request
 * cost that starts below the danger line. The raw state machine serves a request
 * for ~zero heap, so the PAGE now comes from it too: it listens on :80 (browser
 * habit) and :8081 (existing tools), one client at a time, keep-alive. The
 * WebServer still exists for what genuinely needs it — multipart /upload from
 * no-JS browsers and curl -F, /fetch's HTML replies, /log — but on LEGACY_PORT,
 * where its trips can no longer take the page down with them. */
#define LEGACY_PORT    8080
#define RAW_PIECE_MAX  16384     /* fits the shared block: flush-at 16 KB leaves 16 KB of room.
                                  * Piece size is pure round-trip amortization — in-flight
                                  * bytes are bounded by the TCP window (5,744) no matter the
                                  * piece, so bigger pieces cost the heap nothing (measured:
                                  * 5744-vs-8192 scaled with size, not segment alignment). */
#define RAW_HDR_CAP    768
static WiFiServer* s_rawSrv = NULL;
static WiFiServer* s_rawSrv80 = NULL;      // same pump, port 80 — the browser's front door
static WiFiClient  s_rawCli;
static char        s_rawHdr[RAW_HDR_CAP];
static size_t      s_rawHdrLen = 0;
static bool        s_rawBody = false;      // reading a POST body (vs. headers)
static bool        s_rawFetch = false;     // this body is a small POST /fetch form, not a piece
static char        s_rawFetchBody[608];    // fetch form body (a URL + margin); pieces go to PSRAM
static uint32_t    s_rawRxMs = 0;          // last-byte time, for the dead-client deadline
static bool        s_rawGather = false;    // commit this body (vs. consume-and-reply)
static bool        s_rawLast = false;
static size_t      s_rawOff = 0, s_rawCLen = 0, s_rawGot = 0;
static uint32_t    s_rawWantCrc = 0, s_rawCrc = 0;
static int         s_rawCode = 0;          // pre-decided verdict for consume mode
static char        s_rawReply[32];
/* A KOSync PUT/POST body (a few hundred bytes of JSON) is read here, then answered.
 * PSRAM, allocated when a window starts and freed when it ends: KOSync is off for almost
 * every phone, and 2 KB of .bss would be internal RAM spent on nothing. */
#define RAW_KS_BODY_CAP 2048
static bool        s_rawKs = false;        // this body is a KOSync request's
static char*       s_ksBuf = NULL;
/* ⚠ AN ABSOLUTE DEADLINE, not only the 4 s silence rule. There is ONE raw client slot, and a
 * client that trickles a byte every 3 s never trips a silence rule — while it holds the slot,
 * an X4 in a sync window gets nothing. So a request's HEADERS (any request: a real client
 * sends them in one segment) and a whole KOSync request must each finish within this of
 * their first byte. Upload pieces keep the silence rule alone: a 16 KB piece on a weak
 * hotspot can legitimately take longer. */
#define RAW_REQ_ABS_MS  8000
static uint32_t    s_rawReqMs = 0;         // first byte of the current request

static void rawReset() {
  s_rawHdrLen = 0;
  s_rawBody = false;
  s_rawGather = false;
  s_rawFetch = false;
  s_rawKs = false;
  s_rawGot = 0;
}

/* Abort any in-flight raw request (a WebServer transfer is taking the shared
 * buffer, or the client went quiet). Committed/buffered bytes stand — the
 * client's resync picks up from held truth. */
static void rawAbort() {
  if (s_rawCli) {
    s_rawCli.stop();
  }
  rawReset();
}

/* `key=value` out of an &-separated query, URL-decoded. False if absent. */
static bool rawQueryArg(const char* q, const char* key, char* out, size_t cap) {
  const size_t klen = strlen(key);
  while (q && *q) {
    const char* eq = strchr(q, '=');
    const char* amp = strchr(q, '&');
    if (!eq || (amp && eq > amp)) {
      q = amp ? amp + 1 : NULL;
      continue;
    }
    if ((size_t)(eq - q) == klen && !strncmp(q, key, klen)) {
      const char* v = eq + 1;
      const char* end = amp ? amp : v + strlen(v);
      size_t o = 0;
      while (v < end && o + 1 < cap) {
        char c = *v++;
        if (c == '+') {
          c = ' ';
        } else if (c == '%' && v + 1 < end && isxdigit((int)v[0]) && isxdigit((int)v[1])) {
          char hx[3] = { v[0], v[1], 0 };
          c = (char)strtoul(hx, NULL, 16);
          v += 2;
        }
        out[o++] = c;
      }
      out[o] = '\0';
      return true;
    }
    q = amp ? amp + 1 : NULL;
  }
  return false;
}

static void rawReplyEx(int code, const char* ctype, const char* body) {
  const char* st = code == 200 ? "OK" : code == 204 ? "No Content"
                 : code == 400 ? "Bad Request" : code == 404 ? "Not Found"
                 : code == 401 ? "Unauthorized" : code == 402 ? "Payment Required"
                 : code == 403 ? "Forbidden"
                 : code == 409 ? "Conflict" : code == 413 ? "Payload Too Large"
                 : code == 422 ? "Unprocessable Entity"
                 : code == 501 ? "Not Implemented" : "Insufficient Storage";
  char h[192];
  const int bl = (int)strlen(body);
  const int n = snprintf(h, sizeof(h),
                         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                         "Content-Length: %d\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
                         code, st, ctype, bl);
  s_rawCli.write((const uint8_t*)h, (size_t)n);
  if (bl > 0) {
    s_rawCli.write((const uint8_t*)body, (size_t)bl);
  }
}

static void rawReply(int code, const char* body) {
  rawReplyEx(code, "text/plain", body);
}

/* The page, off the raw pump: the exact bytes handleRoot serves, plus a tail that
 * (a) sets RAWPAGE so the page's JS skips the probe and chunks same-origin, and
 * (b) points no-JS browsers at the legacy port, since the raw server does not
 * speak multipart. Written with an exact Content-Length so the connection can
 * stay open — keep-alive is the whole reason this server exists. The five parts
 * total ~5.5 KB: one lands in the 5744-byte socket send buffer, the rest rides
 * the window; a browser drains it immediately. Per-request heap cost: the header
 * and tail on the stack, nothing else. */
static void rawSendPage() {
  char tail[224];
  snprintf(tail, sizeof(tail),
           "<script>var RAWPAGE=1</script>"
           "<p style='color:#888;font-size:13px'>No-JavaScript browser or curl -F? "
           "Legacy uploader: http://%s:%d/</p>", s_addr, LEGACY_PORT);
  const size_t len = strlen(PAGE_1) + strlen(s_cfg->heading) + strlen(PAGE_2) +
                     strlen(s_cfg->dir) + strlen(PAGE_3) + strlen(tail);
  char h[160];
  const int n = snprintf(h, sizeof(h),
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                         "Content-Length: %u\r\n\r\n", (unsigned)len);
  s_rawCli.write((const uint8_t*)h, (size_t)n);
  s_rawCli.write((const uint8_t*)PAGE_1, strlen(PAGE_1));
  s_rawCli.write((const uint8_t*)s_cfg->heading, strlen(s_cfg->heading));
  s_rawCli.write((const uint8_t*)PAGE_2, strlen(PAGE_2));
  s_rawCli.write((const uint8_t*)s_cfg->dir, strlen(s_cfg->dir));
  s_rawCli.write((const uint8_t*)PAGE_3, strlen(PAGE_3));
  s_rawCli.write((const uint8_t*)tail, strlen(tail));
}

/* Headers are complete: decide everything decidable now. Raw bodies mean the
 * piece length is known BEFORE the body arrives (Content-Length), so the
 * verdict is made here and the body is either gathered or drained. */
static void rawOnRequest() {
  s_lastRawMs = millis();
  s_rawHdr[s_rawHdrLen] = '\0';
  char method[8] = {0}, target[320] = {0};
  if (sscanf(s_rawHdr, "%7s %319s", method, target) != 2) {
    rawAbort();
    return;
  }
  // Content-Length, case-insensitively, without strcasestr.
  s_rawCLen = 0;
  for (const char* p = s_rawHdr; (p = strchr(p, '\n')) != NULL; p++) {
    if (!strncasecmp(p + 1, "content-length:", 15)) {
      s_rawCLen = (size_t)strtoul(p + 16, NULL, 10);
      break;
    }
  }
  if (!strcmp(method, "OPTIONS")) {
    char h[224];
    const int n = snprintf(h, sizeof(h),
                           "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                           "Access-Control-Allow-Headers: content-type\r\nContent-Length: 0\r\n\r\n");
    s_rawCli.write((const uint8_t*)h, (size_t)n);
    rawReset();
    return;
  }
  char* qs = strchr(target, '?');
  if (qs) {
    *qs++ = '\0';
  }
  /* ── KOSYNC — a sync window's routes, and ONLY while a window is open ─────────────────
   * Decided before any uploader route so a window can ride on a running uploader (same
   * network, same port). Outside a window these paths are simply not found: the phone is
   * not a KOSync server except for the minutes someone asked it to be one. */
  if (kosyncIsPath(target)) {
    if (!s_winOn) {
      rawReplyEx(404, "application/json", "{\"message\":\"Not found\"}");
      rawReset();
      return;
    }
    if (s_rawCLen > 0) {
      if (!s_ksBuf || s_rawCLen >= RAW_KS_BODY_CAP) {
        rawReplyEx(413, "application/json", "{\"message\":\"Request too large\"}");
        rawAbort();                 // the body is in flight and not worth draining
        return;
      }
      s_rawKs = true;               // read the JSON, answer in rawXferPump
      s_rawGather = false;
      s_rawGot = 0;
      s_rawBody = true;
      return;
    }
    int code = 500;
    const char* reply = kosyncWindowServe(method, target, s_rawHdr, "", 0, &code);
    rawReplyEx(code, "application/json", reply);
    rawReset();
    return;
  }
  /* ⚠ A WINDOW ALONE SERVES NOTHING ELSE. With only a window up the phone may be an OPEN
   * hotspot that opened itself when a book was closed; the page, /chunk and /fetch write to
   * the SD card, and that is only ever offered from a screen the owner is looking at. */
  if (!s_upOn) {
    rawReplyEx(404, "application/json", "{\"message\":\"Not found\"}");
    rawReset();
    return;
  }
  if (!strcmp(target, "/") && !strcmp(method, "GET")) {
    rawSendPage();
    s_lastActivityMs = millis();
    rawReset();
    return;
  }
  if (!strcmp(target, "/favicon.ico")) {
    rawReply(204, "");            // unprompted browser ask; free the pump immediately
    rawReset();
    return;
  }
  if (!strcmp(target, "/fetch") && !strcmp(method, "POST")) {
    /* The page's paste-a-link form, now landing on the raw server. The body is a
     * tiny urlencoded form; gather it into the small buffer and act on completion. */
    if (s_rawCLen == 0 || s_rawCLen >= sizeof(s_rawFetchBody)) {
      rawReplyEx(400, "text/html", "<p>Bad URL. <a href=/>back</a></p>");
      rawAbort();
      return;
    }
    s_rawFetch = true;
    s_rawGather = false;
    s_rawGot = 0;
    s_rawBody = true;
    s_lastActivityMs = millis();
    return;
  }
  if (!strcmp(target, "/upload")) {
    /* Multipart is the one thing this server refuses to speak — that parser is
     * exactly the per-request machinery being escaped. The legacy server keeps it. */
    char msg[160];
    snprintf(msg, sizeof(msg),
             "<p>This port does not take form uploads - use the page's own buttons, "
             "or the legacy uploader at http://%s:%d/</p>", s_addr, LEGACY_PORT);
    rawReplyEx(501, "text/html", msg);
    rawAbort();                   // a multipart body may be in flight; drop it
    return;
  }
  if (!strcmp(target, "/log")) {
    /* The battery-run workflow reads /log by name (docs carry the exact curl).
     * File streaming is framework work — bounce to the legacy port, same query.
     * curl -L and every browser follow it. */
    char h[224];
    const int n = snprintf(h, sizeof(h),
                           "HTTP/1.1 302 Found\r\nLocation: http://%s:%d/log%s%s\r\n"
                           "Content-Length: 0\r\n\r\n",
                           s_addr, LEGACY_PORT, qs ? "?" : "", qs ? qs : "");
    s_rawCli.write((const uint8_t*)h, (size_t)n);
    rawReset();
    return;
  }
  if (strcmp(target, "/chunk") != 0) {
    rawReply(404, "nf");
    rawReset();
    return;
  }
  char rawNameArg[96] = {0};
  char name[sizeof(s_chunkName)];
  rawQueryArg(qs, "name", rawNameArg, sizeof(rawNameArg));
  if (!xferSafeName(rawNameArg, name, sizeof(name))) {
    rawReply(400, "bad name");
    rawReset();
    return;
  }
  if (!strcmp(method, "GET")) {
    const size_t held = xferHeld(name);   // a key file: never stat'd
    char out[16];
    snprintf(out, sizeof(out), "%u", (unsigned)held);
    rawReply(200, out);
    rawReset();
    return;
  }
  if (strcmp(method, "POST") != 0) {
    rawReply(404, "nf");
    rawReset();
    return;
  }
  char argbuf[16];
  if (!rawQueryArg(qs, "crc", argbuf, sizeof(argbuf)) || !chunkParseHex32(argbuf, &s_rawWantCrc)) {
    rawReply(400, "bad crc");
    rawAbort();               // a body may be in flight; the stream is not worth resyncing
    return;
  }
  s_rawOff = rawQueryArg(qs, "off", argbuf, sizeof(argbuf)) ? (size_t)strtoul(argbuf, NULL, 10) : 0;
  s_rawLast = rawQueryArg(qs, "last", argbuf, sizeof(argbuf)) && !strcmp(argbuf, "1");
  if (s_rawCLen == 0) {
    rawReply(400, "empty");
    rawReset();
    return;
  }
  if (s_rawCLen > RAW_PIECE_MAX) {
    rawReply(413, "too big");
    rawAbort();               // cannot drain an oversized body at loop pace
    return;
  }
  int code = 500;
  char reply[32] = "err";
  if (!chunkOpenFor(name, s_rawOff, &code, reply, sizeof(reply))) {
    strlcpy(s_rawReply, reply, sizeof(s_rawReply));
    s_rawCode = code;
    s_rawGather = false;      // drain the body, then deliver the bad news
    s_rawBody = true;
    return;
  }
  s_lastChunkMs = millis();
  s_lastActivityMs = millis();
  const size_t held = s_chunkDurable + s_sdLen;
  switch (chunkDecide(held, s_rawOff, s_rawCLen)) {
    case CHUNK_APPEND:
      s_rawGather = true;
      s_rawCrc = chunkCrc32Init();
      break;
    case CHUNK_DUPLICATE:
      s_rawGather = false;
      s_rawCode = 200;
      snprintf(s_rawReply, sizeof(s_rawReply), "ok %u", (unsigned)held);
      break;
    default:
      s_rawGather = false;
      s_rawCode = 409;
      snprintf(s_rawReply, sizeof(s_rawReply), "have %u", (unsigned)held);
      break;
  }
  s_rawGot = 0;
  s_rawBody = true;
}

static void rawXferPump() {
  if (!s_rawSrv && !s_rawSrv80) {
    return;
  }
  /* Most-recent-wins accept, but never preempt a client mid-request: a page
   * reload or a second tool run takes over an IDLE held connection. Two
   * listeners feed the one slot — :80 (browser) checked first, :8081 (tools). */
  WiFiServer* src = (s_rawSrv80 && s_rawSrv80->hasClient()) ? s_rawSrv80
                    : (s_rawSrv && s_rawSrv->hasClient())   ? s_rawSrv : NULL;
  if (src) {
    if (!s_rawCli || !s_rawCli.connected() || (!s_rawBody && s_rawHdrLen == 0)) {
      if (s_rawCli) {
        s_rawCli.stop();
      }
      s_rawCli = src->available();
      s_rawCli.setNoDelay(true);
      rawReset();
      s_rawRxMs = millis();
      s_lastActivityMs = millis();
    }
  }
  if (!s_rawCli || (!s_rawCli.connected() && !s_rawCli.available())) {
    if (s_rawBody || s_rawHdrLen) {
      rawReset();             // client died mid-request; committed bytes stand
    }
    return;
  }
  if ((s_rawBody || s_rawHdrLen) && (uint32_t)(millis() - s_rawRxMs) > 4000) {
    rawAbort();               // mid-request silence: a dead client must not hold the port
    return;
  }
  if (((!s_rawBody && s_rawHdrLen) || s_rawKs) && (uint32_t)(millis() - s_rawReqMs) > RAW_REQ_ABS_MS) {
    log_e("XFER: request over %u ms from its first byte (%s) - dropped, the slot is freed",
          (unsigned)RAW_REQ_ABS_MS, s_rawKs ? "KOSync body" : "headers");
    rawAbort();
    return;
  }
  if (!s_rawBody) {
    while (s_rawCli.available()) {
      if (s_rawHdrLen >= RAW_HDR_CAP - 1) {
        rawAbort();
        return;
      }
      const int c = s_rawCli.read();
      if (c < 0) {
        break;
      }
      if (s_rawHdrLen == 0) {
        s_rawReqMs = millis();  // this request's clock starts with its first byte
      }
      s_rawHdr[s_rawHdrLen++] = (char)c;
      s_rawRxMs = millis();
      s_lastActivityMs = millis();
      if (s_rawHdrLen >= 4 && !memcmp(s_rawHdr + s_rawHdrLen - 4, "\r\n\r\n", 4)) {
        rawOnRequest();
        break;
      }
    }
  }
  if (s_rawBody) {
    /* Drain with a ~20 ms budget instead of one available() sweep per pass.
     * MEASURED: at loop pace (~10 ms/pass) a piece needed several passes, the
     * book ran at 56 KB/s, and driver RX buffers pooled behind the slow reader
     * (free-heap floor 6 K). Emptying the driver promptly raises the floor AND
     * the rate; the main loop already tolerates longer SD stalls than 20 ms. */
    const uint32_t budget0 = millis();
    while (s_rawGot < s_rawCLen && (uint32_t)(millis() - budget0) < 20) {
      if (!s_rawCli.available()) {
        if (!s_rawCli.connected()) {
          break;
        }
        delay(1);               // mid-piece lull: give the stack a beat, keep the budget
        continue;
      }
      int n;
      if (s_rawGather) {
        size_t want = s_rawCLen - s_rawGot;
        if (want > 1436) {
          want = 1436;
        }
        n = s_rawCli.read(s_sdBuf + s_sdLen + s_rawGot, want);
        if (n <= 0) {
          break;
        }
        s_rawCrc = chunkCrc32Update(s_rawCrc, s_sdBuf + s_sdLen + s_rawGot, (size_t)n);
      } else if (s_rawFetch) {
        size_t want = s_rawCLen - s_rawGot;                   // capped at parse time
        n = s_rawCli.read((uint8_t*)s_rawFetchBody + s_rawGot, want);
        if (n <= 0) {
          break;
        }
      } else if (s_rawKs && s_ksBuf) {
        size_t want = s_rawCLen - s_rawGot;                   // capped at parse time
        n = s_rawCli.read((uint8_t*)s_ksBuf + s_rawGot, want);
        if (n <= 0) {
          break;
        }
      } else {
        uint8_t scratch[256];
        size_t want = s_rawCLen - s_rawGot;
        if (want > sizeof(scratch)) {
          want = sizeof(scratch);
        }
        n = s_rawCli.read(scratch, want);
        if (n <= 0) {
          break;
        }
      }
      s_rawGot += (size_t)n;
      s_rawRxMs = millis();
      s_lastActivityMs = millis();
    }
    {
      // The acceptance bar reads these floors; the raw path samples once per pump.
      const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (largest < s_fileFloorLargest) {
        s_fileFloorLargest = largest;
      }
      if (freeb < s_fileFloorFree) {
        s_fileFloorFree = freeb;
      }
    }
    if (s_rawGot >= s_rawCLen) {
      if (s_rawGather) {
        if (chunkCrc32Final(s_rawCrc) != s_rawWantCrc) {
          rawReply(422, "crc");         // piece dropped; buffered committed bytes stand
        } else {
          char reply[32];
          const int code = chunkCommit(s_rawCLen, s_rawLast, reply, sizeof(reply));
          rawReply(code, reply);
        }
      } else if (s_rawKs && s_ksBuf) {
        /* The request line is still in s_rawHdr (only its LENGTH is reset between requests,
         * and nothing writes it while a body is being read), so it is parsed again here
         * rather than kept in more statics. KOSync paths are short; 96 is plenty. */
        s_ksBuf[s_rawGot] = '\0';
        char method[8] = {0}, target[96] = {0};
        sscanf(s_rawHdr, "%7s %95s", method, target);
        char* q = strchr(target, '?');
        if (q) {
          *q = '\0';
        }
        int code = 500;
        const char* reply = kosyncWindowServe(method, target, s_rawHdr, s_ksBuf, s_rawGot, &code);
        rawReplyEx(code, "application/json", reply);
      } else if (s_rawFetch) {
        s_rawFetchBody[s_rawGot] = '\0';
        char url[512];
        /* The form body is &-separated k=v, exactly what the query parser eats. */
        if (!rawQueryArg(s_rawFetchBody, "url", url, sizeof(url)) || strlen(url) < 8) {
          rawReplyEx(400, "text/html", "<p>Bad URL. <a href=/>back</a></p>");
        } else {
          /* Same synchronous pull the legacy path runs: the loop blocks for the
           * download's duration, bounded by downloadTo's own 8 s stall abort.
           * The proven full-speed path (2026-08-20: 3.9 MB at ~460 KB/s). */
          const bool ok = xferFetchUrl(url);
          rawReplyEx(ok ? 200 : 500, "text/html",
                     ok ? "<p>Downloaded! <a href=/>back</a></p>"
                        : "<p>Download failed (check the link is a direct file link). "
                          "<a href=/>back</a></p>");
        }
      } else {
        rawReply(s_rawCode, s_rawReply);
      }
      rawReset();
    }
  }
}

/* :80 belongs to the TRANSPORT (the page and a KOSync window both answer there); :8081 is
 * the uploader's tools port and comes and goes with it. */
static void rawXferUp80() {
  if (!s_rawSrv80) {
    s_rawSrv80 = new WiFiServer(80);
    s_rawSrv80->begin();
    s_rawSrv80->setNoDelay(true);
  }
}

static void rawXferUp8081() {
  if (!s_rawSrv) {
    s_rawSrv = new WiFiServer(RAW_PORT);
    s_rawSrv->begin();
    s_rawSrv->setNoDelay(true);
  }
}

static void rawXferDown8081() {
  if (s_rawSrv) {
    s_rawSrv->end();
    delete s_rawSrv;
    s_rawSrv = NULL;
  }
}

static void rawXferDown() {
  rawAbort();
  s_rawCli = WiFiClient();
  rawXferDown8081();
  if (s_rawSrv80) {
    s_rawSrv80->end();
    delete s_rawSrv80;
    s_rawSrv80 = NULL;
  }
}

static void handleUpload() {
  HTTPUpload& up = s_server->upload();
  if (up.status == UPLOAD_FILE_START) {
    s_reqLargest0 = s_reqFloor =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    rawAbort();                     // the legacy path preempts an in-flight raw piece too
    if (s_chunkFile) {
      chunkFinalize();              // the legacy path is taking the shared buffer
    }
    if (s_uploadFile) {
      s_uploadFile.close();         // close a handle left over from an aborted upload
    }
    if (!s_sdBuf) {
      s_sdBuf = (uint8_t*)ps_malloc(XFER_SD_BLOCK);
    }
    s_sdLen = 0;
    SD.mkdir(s_cfg->dir);
    String path = String(s_cfg->dir) + "/" + up.filename;
    SD.remove(path.c_str());        // overwrite cleanly (SD write mode appends)
    s_uploadFile = SD.open(path.c_str(), FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    /* ── BACKPRESSURE: the fix for fast links ─────────────────────────────────
     * On a fast LAN (3 ms RTT) TCP opens wide and the radio floods internal
     * heap with RX buffers far faster than the SD drains — measured 2026-08-20
     * evening: heap 14 KB → 868 BYTES inside three seconds of one upload,
     * while the slow phone-hotspot link had masked the whole problem by
     * keeping bursts tiny. Flow control, the old way: while memory is tight,
     * STOP CONSUMING — the TCP window closes, the sender stalls, the radio's
     * buffers drain into the socket, heap comes back, reading resumes. The
     * cap bounds a stall against a dead client; the breaker (which now only
     * trips mid-upload on catastrophe) remains the last line. */
    {
      size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (largest < s_reqFloor) {
        s_reqFloor = largest;           // the anatomy: the request's true low point
      }
      int spins = 0;
      while (largest < 7168 && ++spins <= 600) {
        delay(5);                       // up to ~3 s of deliberate stall
        largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (largest < s_reqFloor) {
          s_reqFloor = largest;
        }
      }
    }
    if (s_uploadFile) {
      if (s_sdBuf) {
        size_t off = 0;
        while (off < up.currentSize) {
          size_t room = XFER_SD_BLOCK - s_sdLen;
          size_t take = up.currentSize - off;
          if (take > room) {
            take = room;
          }
          memcpy(s_sdBuf + s_sdLen, up.buf + off, take);
          s_sdLen += take;
          off += take;
          if (s_sdLen == XFER_SD_BLOCK) {
            xferFlushBlock();
          }
        }
      } else {
        s_uploadFile.write(up.buf, up.currentSize);   // no PSRAM: the old slow path
      }
      /* Yield on EVERY piece, not every fourth. handleClient() owns the main loop for the
       * whole upload, so this is the only chance the WiFi and TCP stacks get to run —
       * starving them is what made the phone drop off the network mid-transfer. */
      delay(1);
      /* ...and the only chance the LoRa radio gets to leave STANDBY: a frame on the air when
       * the upload began would otherwise leave the mesh deaf until it ends (review M2). Finishes
       * that frame and starts nothing — the chip's own bit-banged bus, not the card's. */
      meshService.txComplete();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_uploadFile) {
      xferFlushBlock();             // the tail, which is almost never a whole block
      s_uploadFile.flush();         // commit size/data before it can be read
      s_uploadFile.close();
      s_filesAdded++;
      /* One line per file: the leak-hunting instrument. If these numbers walk
       * down across a session, something in the request path is not giving
       * memory back — measured, not guessed. */
      log_e("XFER: '%s' done (file %d this session)  heap=%u largest=%u  req: start=%u floor=%u",
            up.filename.c_str(), s_filesAdded,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)s_reqLargest0, (unsigned)s_reqFloor);
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    log_e("XFER: '%s' ABORTED  heap=%u largest=%u  req: start=%u floor=%u", up.filename.c_str(),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
          (unsigned)s_reqLargest0, (unsigned)s_reqFloor);
    s_sdLen = 0;                    // drop the partial block rather than commit garbage
    if (s_uploadFile) {
      s_uploadFile.close();         // partial file stays; re-upload overwrites it
    }
  }
}

// Downloads a file from a (possibly https, possibly redirecting) URL to the SD card.
static bool downloadTo(const String& url, const String& path) {
  File f = SD.open(path.c_str(), FILE_WRITE);
  if (!f) {
    return false;
  }
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  WiFiClientSecure secure;
  bool begun;
  if (url.startsWith("https")) {
    secure.setInsecure();
    begun = http.begin(secure, url);
  } else {
    begun = http.begin(url);
  }
  if (!begun) {
    f.close();
    return false;
  }
  bool ok = false;
  if (http.GET() == HTTP_CODE_OK) {
    WiFiClient* stream = http.getStreamPtr();
    int len = http.getSize();
    uint8_t buf[1024];
    uint32_t written = 0;
    uint32_t idle = millis();
    while (http.connected()) {
      size_t avail = stream->available();
      if (avail) {
        int r = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
        if (r > 0) {
          f.write(buf, r);
          idle = millis();
          written += r;
          if (written >= 32768) {   // feed the idle task/watchdog on long downloads
            written = 0;
            delay(1);
          }
          if (len > 0) {
            len -= r;
            if (len <= 0) {
              break;
            }
          }
        }
      } else if (millis() - idle > 8000) {
        break;                      // stalled
      } else {
        delay(2);
      }
    }
    ok = true;
  }
  http.end();
  f.close();
  return ok;
}

// True if `name` ends with one of the comma-separated extensions in `accept`.
static bool nameAccepted(const String& lowName, const char* accept) {
  const char* p = accept;
  while (*p) {
    const char* comma = strchr(p, ',');
    size_t n = comma ? (size_t)(comma - p) : strlen(p);
    if (n > 0 && lowName.endsWith(String(p).substring(0, n))) {
      return true;
    }
    p = comma ? comma + 1 : p + n;
  }
  return false;
}

/* The pull path's core, shared by the raw front door and the legacy WebServer.
 * A fetch whose derived filename collides with the OPEN batch file (the
 * `download.epub` default makes this likelier than it sounds) would open a
 * second FIL on it and truncate under the live append handle — cross-linked
 * FAT clusters. Same rule as handleUpload: the batch yields first. */
static bool xferFetchUrl(const char* urlIn) {
  chunkFinalize();
  String url = urlIn;
  // Derive a filename from the URL; fall back to the config's default if unusable.
  String fn = url;
  int q = fn.indexOf('?');
  if (q >= 0) {
    fn = fn.substring(0, q);
  }
  int sl = fn.lastIndexOf('/');
  if (sl >= 0) {
    fn = fn.substring(sl + 1);
  }
  String low = fn;
  low.toLowerCase();
  if (!nameAccepted(low, s_cfg->accept)) {
    fn = s_cfg->defaultName;
  }
  SD.mkdir(s_cfg->dir);
  bool ok = downloadTo(url, String(s_cfg->dir) + "/" + fn);
  if (ok) {
    s_filesAdded++;
  }
  return ok;
}

static void handleFetch() {
  String url = s_server->arg("url");
  if (url.length() < 8) {
    s_server->send(400, "text/html", "<p>Bad URL. <a href=/>back</a></p>");
    return;
  }
  bool ok = xferFetchUrl(url.c_str());
  s_server->send(ok ? 200 : 500, "text/html",
                 ok ? "<p>Downloaded! <a href=/>back</a></p>"
                    : "<p>Download failed (check the link is a direct file link). <a href=/>back</a></p>");
}

/* Everything needed to serve: WebServer + handlers + mDNS. Factored so the
 * breaker's pause/resume uses the exact same lifecycle as start/stop — the
 * resume path being a lesser copy is how the first cuts went stale. */
static void xferServerUp() {
  /* ── mDNS COMES UP ONCE PER BOOT AND NEVER GOES DOWN ─────────────────────────
   * This core's ESPmDNS leaks by design: begin() registers a WiFi event callback
   * every call and end() never removes it (WiFiGeneric's cbEventList has no
   * dedupe), so every begin/end cycle grows a permanent list entry — and the
   * breaker used to cycle it on EVERY pause/resume, inside tight heap, which is
   * a measured share of the stair-stepping floors (3328 → 616 across one batch;
   * see the raw-transport rationale). One begin, guarded, on first use; nothing
   * ever calls end() again. wiphone.local keeps resolving while the server is
   * off — the connection is then refused, which is honest, and cheaper than a
   * leak that compounds per session. */
  static bool s_mdnsUp = false;
  if (!s_mdnsUp) {
    s_mdnsUp = MDNS.begin("wiphone");   // http://wiphone.local
  }
  if (!s_server) {
    /* LEGACY_PORT since 0.9.28 — the raw pump owns :80 now. What lives here is
     * exactly what needs the framework: multipart /upload (no-JS browsers,
     * curl -F), and /log's file streaming. Its ~10 KB/request transient and its
     * breaker trips no longer touch the page. */
    s_server = new WebServer(LEGACY_PORT);
  }
  s_server->on("/", HTTP_GET, handleRoot);
  s_server->on("/upload", HTTP_POST, []() {
    s_server->sendHeader("Connection", "close");
    s_server->send(200, "text/html", "<p>Uploaded! <a href=/>back</a></p>");
  }, handleUpload);
  s_server->on("/fetch", HTTP_POST, handleFetch);
  s_server->on("/chunk", HTTP_GET, handleChunkGet);
  s_server->on("/chunk", HTTP_POST, handleChunkDone, handleChunkData);
  s_server->on("/favicon.ico", HTTP_GET, handleFavicon);
  s_server->on("/log", HTTP_GET, handleLog);
  s_server->onNotFound(handleNotFound);
  s_server->begin();
}

static void xferServerDown() {
  chunkPersist();           // acked bytes become durable; the batch file STAYS OPEN
                            // (the raw transport rides through this teardown)
  if (s_uploadFile) {
    s_uploadFile.close();   // an aborted upload's handle must not dangle across the pause
  }
  if (s_server) {
    s_server->stop();
    delete s_server;        // the object itself holds internal-heap Strings; free it all
    s_server = NULL;
  }
  // No MDNS.end() — deliberately. See xferServerUp(): end() leaks an event-list
  // entry per cycle in this core, and the responder is cheap to keep.
}

/* ── THE TRANSPORT: a network to be reached on, :80, and the screen held awake ──────────
 * Shared by the uploader and a KOSync window (see the note at the top of this file). The
 * body is what xferStart() always did, moved rather than rewritten; the two changes are
 * marked. `apName` is the hotspot to host when the phone is not on a network. */
/* Putting the station back after a hotspot is wifiRestoreStation() (Networks.cpp, deciding in
 * wifi_policy.h) — the one answer every site that gives the radio back now shares.
 * 🛑 It asks BOTH switches, not radioOff() alone. Networks::disable() (no saved network at boot,
 * Disconnect, Remove network) sets userDisabled, erases the STA config and switches the radio
 * OFF, but does not set radioOff. A bare WiFi.begin() on that phone runs esp_wifi_connect on an
 * empty config: NO_AP_FOUND, and the core's auto-reconnect calls begin() again, for ever — the
 * loop's own reconnect/quiesce all stand down for a userDisabled phone, so nothing ever quiets
 * it. The uploader did this once per manual stop; a sync window that opens on every book close
 * made it routine, and a hunting radio is the biggest drain this phone has measured. A join it
 * does start is stamped (noteWifiJoinStarted), so a window asked for next waits for it (KS-T1). */

/* `pass`: NULL or "" = an open hotspot (every uploader, and a window without hotspot_pass);
 * otherwise 8-63 printable ASCII, validated by kosyncHotspotPassValid() before it gets here —
 * softAP() refuses 1-7 characters outright, which would leave NO hotspot at all.
 * `waitForSta`: the uploader's 2 s grace for an association in flight (see below); a window
 * asks for it only when one genuinely is (kosyncWindowWaitsForSta). */
static bool transportUp(const char* apName, const char* pass, bool waitForSta, const char** err) {
  // Use the joined WiFi network if we have one; otherwise host our own hotspot.
  /* ⚠ GIVE THE STATION A MOMENT BEFORE GIVING UP ON IT.
   *
   * This used to decide instantly, and deciding instantly is how opening the uploader
   * KNOCKED THE PHONE OFF WIFI: if the radio happened to be mid-association — which it
   * is after a boot, after a roam, or any time the link blipped — status() is not yet
   * WL_CONNECTED, so this fell through to softAP() and tore the station connection down
   * to host its own network. The phone then had no WiFi until something else noticed,
   * which is exactly the "no wifi for a while afterwards" that got reported.
   *
   * Two seconds is long enough to cover an association already in flight and short
   * enough not to feel like a hang when there genuinely is no network.
   *
   * (0.9.79) Not at all when the user switched WiFi OFF: nothing can associate, and a sync
   * window in the woods — the case it is for — would otherwise freeze the phone for two
   * seconds every time a book is closed. And (review) not for a WINDOW unless a join is
   * actually under way: out of range, or with no saved network, it waited the full 2 s after
   * every book close/open (LOOP STALL 'kosync'), for a status that was never coming. "Under
   * way" includes a blip or a roam (the station was UP moments ago and the core is rejoining
   * by itself) — exactly the case this wait was written for. */
  for (int i = 0; waitForSta && i < 20 && !wifiState.radioOff() && WiFi.status() != WL_CONNECTED;
       i++) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    s_usingAP = false;
    s_apName[0] = '\0';
    snprintf(s_addr, sizeof(s_addr), "%s", WiFi.localIP().toString().c_str());
  } else {
    s_usingAP = true;
    /* 🛑 THE CORE'S AUTO-RECONNECT IS HELD OFF BEFORE THE STATION GOES (0.9.79 review R1). A
     * hotspot comes up exactly when the station is NOT associated - out of range, often still
     * hunting on NO_AP_FOUND, or a JOIN begun moments ago by the teardown of the window this one
     * replaces. The core's event task answers a NO_AP_FOUND handled after the mode(AP) below with
     * `WiFi.disconnect(); WiFi.begin();`, and begin() is enableSTA(true) = mode(AP+STA): the
     * station hunting beside the live softAP for the whole window (nothing puts the mode back to
     * AP - the loop's retry, quiesce and auto-switch all stand down under a live hotspot). The AP
     * then follows the station's scan channels (the X4 cannot hold 'WiPhone-Books'), and a station
     * connect under a live softAP is the chip panic WiPhone.ino's reconnect gate exists for.
     * Released in transportDown() (and below, if the softAP fails) BEFORE the station is given
     * back.
     *
     * ⚠ THE HOLD NARROWS THE RACE; IT DOES NOT CLOSE IT. The core's handler (WiFiGeneric.cpp,
     * STA_DISCONNECTED) reads getAutoReconnect() FIRST, then calls WiFi.disconnect(), whose
     * esp_wifi_disconnect() waits on the WiFi task. Its "network_event" task outranks this loop
     * (priority 19 against 1, both on core 1), so this line only ever runs while such a handler is
     * blocked - and one blocked there is already past its flag read: its begin() lands after our
     * mode(AP) and makes it AP+STA all the same. About a millisecond per NO_AP_FOUND, one every
     * few seconds while the station hunts. So a softAP() that came up looks at the mode once more
     * and switches a station that came back off (below, logged); a handler slower even than that
     * shows as mode=3 in `wifi why`. No WiFi.disconnect(true) first: a radio stop/start for
     * nothing, and it closes nothing the hold does not - the race is in a handler already running,
     * not in the station's state. */
    wifiAutoReconnectHold(WIFI_AR_HOLD_HOTSPOT);
    WiFi.mode(WIFI_AP);
    const bool wpa2 = pass && pass[0];
    // 🛑 The passphrase goes to the driver and nowhere else: never into a log line.
    if (WiFi.softAP(apName, wpa2 ? pass : NULL)) {
      /* The race the hold cannot close (above), caught when it has already landed. Nothing else
       * turns the station on here: mode(AP) removed it and softAP() only adds the AP, so a
       * station now is the core's begin() from a handler that read the flag before the hold. No
       * client can have joined an AP this young, so even a mode change that restarts it costs
       * nobody anything (not measured: the race has never been seen on the bench). */
      if (WiFi.getMode() & WIFI_MODE_STA) {
        log_e("XFER: the station came back beside the hotspot (the core's auto-reconnect, "
              "already past its flag) - switched off");
        WiFi.enableSTA(false);
      }
      snprintf(s_addr, sizeof(s_addr), "%s", WiFi.softAPIP().toString().c_str());
      snprintf(s_apName, sizeof(s_apName), "%s", apName);
      s_apProtected = wpa2;
    } else {
      snprintf(s_addr, sizeof(s_addr), "AP FAILED");   // surfaced on the phone screen
      *err = "Could not host a hotspot either";
      /* ⚠ FAIL CLEANLY RATHER THAN ACQUIRE. This used to fall through and take the
       * hold anyway: no AP, no client possible, and yet the CPU pinned at 240 MHz and
       * the screen timeouts stretched, with the only evidence a small "AP FAILED" on
       * a screen the serial path does not even show. A server that cannot serve must
       * not cost anything. */
      log_e("XFER: softAP failed - not starting (nothing would be able to connect)");
      /* (0.9.79) ...and do not leave the radio in AP mode with no AP: put the station back
       * exactly as a stop does, or the phone sits off WiFi until something notices. */
      s_usingAP = false;
      wifiAutoReconnectRelease(WIFI_AR_HOLD_HOTSPOT);   // no hotspot: the station's own retry is back
      wifiRestoreStation("hotspot failed to start");
      return false;
    }
  }
  rawXferUp80();
  /* (0.9.79) The screen hold is NOT taken here: it is the UPLOADER's (xferStart), because the
   * uploader is a screen someone is watching. A KOSync window serves just as well with the
   * screen dark — the loop and this pump never stop for a dim or a sleep, which on this phone
   * are backlight-only (see the note on xferWindowStart). */
  s_on = true;   // gbcXferHandleClient() (main loop) now pumps it
  return true;
}

static void transportDown(const char* who) {
  if (!s_on) {
    return;
  }
  rawXferDown();
  if (s_usingAP) {
    WiFi.softAPdisconnect(true);
    s_usingAP = false;           // BEFORE the restore: it asks whether a hotspot is still live
    s_apName[0] = '\0';
    s_apProtected = false;
    s_apByUploader = false;
    /* ⚠ Setting the mode back to STA does NOT reconnect. Without a join the phone sat with
     * no WiFi until some other timer got round to noticing, which felt like the uploader
     * had broken the network on its way out — so the restore rejoins (begin() with no
     * arguments re-uses the credentials already in the driver) when it may.
     *
     * ⚠ AND IT MAY NOT, whatever the hotspot did: 🛑 never under a Game Boy game (the emulator
     * owns both cores and the internal RAM it grabbed with WiFi off; the game's own exit asks
     * again — this used to be a branch of its own here), and never over "WiFi: off" or a
     * Disconnected network (the 2026-09-01 audit's paths). The AP is torn down either way;
     * what is gated is bringing the station back up.
     *
     * The hotspot's hold on the core's auto-reconnect goes first (transportUp took it, R1), so a
     * NO_AP_FOUND on the restore's own join is retried by the core as it always was. Whether the
     * flag actually comes back on is the owner's: disarmed after disable() (WiFi off), still held
     * under a game (startGame holds before it closes a window - this runs inside it). */
    wifiAutoReconnectRelease(WIFI_AR_HOLD_HOTSPOT);
    wifiRestoreStation(who);
  }
  s_on = false;
}

void xferStart(const XferConfig* cfg) {
  s_startErr = NULL;                    // this attempt gets its own verdict
  /* 🛑 NOT UNDER A GAME (0.9.79), the same refusal xferWindowStart() already made. A game has
   * WiFi off, the watchdogs down and the internal RAM; serial `up on` mid-game ran transportUp,
   * found no station, and brought up a SOFT-AP under the emulator (WiFi.mode(WIFI_AP) is an
   * esp_wifi_start()) with the screen hold and the CPU gate's "busy" on top. No screen can start
   * one during a game (the Games picker's Transfer row is unreachable once a game runs), so this
   * only ever answers the bench and a script. */
  if (gGbcActive) {
    s_startErr = "a Game Boy game is running";
    log_e("XFER: refusing to start - %s", s_startErr);
    return;
  }
  /* ── REFUSE TO START WHEN THERE IS NOT ENOUGH CONTIGUOUS HEAP TO SERVE FROM ──
   * 🔑 The breaker below trips at 6144 bytes largest-internal-block, and when it
   * trips the listener closes — which a browser reports as "site cannot be
   * loaded", with nothing on the phone saying why. Starting a server that is
   * already within a few hundred bytes of that line produces exactly that: it
   * comes up, says ON with a URL, and refuses every connection.
   *
   * MEASURED 2026-08-25 on phone 1: at largest=6884 the server reported ON at a
   * valid station IP and refused three connections in a row (ConnectionRefused,
   * 0.6 s) while ping was fine. A reboot took it to largest=19132 and the same
   * server answered 200 immediately. Phone 2 served fine at largest=12940.
   *
   * ⚠ 10240 is CHOSEN, NOT MEASURED: it sits between a measured failure (6884)
   * and a measured success (12940), with room above the 6144 trip line. If a
   * start is ever refused on a phone that would have worked, this is the number
   * to revisit — and the refusal says the figure so it can be argued with. */
  {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (largest < 10240) {
      static char why[72];
      snprintf(why, sizeof(why), "Low memory (%u B free block) - restart the phone",
               (unsigned)largest);
      s_startErr = why;
      log_e("XFER: refusing to start - largest internal block %u B, need 10240", (unsigned)largest);
      return;
    }
  }
  s_breakerPaused = false;              // a fresh session always starts live
  s_breakerFlipMs = 0;
  s_lowSinceMs = 0;
  if (!cfg) {
    cfg = &ROM_CFG;
  }
  if (s_upOn) {
    if (s_cfg == cfg) {
      return;                   // already serving this folder
    }
    xferStop();                 // a different app wants a different folder: there is one port
  }
  /* A KOSync window may already hold the transport. On the phone's WiFi, or on a hotspot with
   * the SAME name (the Books uploader and a window are both 'WiPhone-Books'), the uploader
   * simply joins it. A hotspot with a DIFFERENT name cannot be shared — this screen is about
   * to tell someone to join cfg->apName — so the window ends; kosyncLoop notices and says so. */
  if (s_on && s_usingAP && strcmp(s_apName, cfg->apName) != 0) {
    log_e("XFER: uploader wants hotspot '%s' - ending the sync window on '%s'",
          cfg->apName, s_apName);
    xferWindowStop();
  }
  s_cfg = cfg;
  s_filesAdded = 0;

  if (!s_on) {
    if (!transportUp(s_cfg->apName, NULL, true, &s_startErr)) {
      return;                    // (the uploader's own hotspot is always open, as it always was)
    }
    s_apByUploader = s_usingAP;
  }
  xferServerUp();
  rawXferUp8081();
  xferHoldAwake(true);           // the uploader is a screen being watched (a window is not)
  s_lastActivityMs = millis();   // a server nobody ever visits still ages out
  s_upOn = true;
}

void xferStop() {
  if (!s_upOn) {
    return;
  }
  s_upOn = false;
  xferHoldAwake(false);
  /* Drop any request in flight on the shared raw slot: an uploader piece mid-body would
   * otherwise go on writing into s_sdBuf after chunkFinalize() below has let go of it. A
   * KOSync request that happened to be on the slot at this instant is dropped too, and its
   * client retries — a trade for never touching a freed buffer. */
  rawAbort();
  rawXferDown8081();
  chunkFinalize();          // a real stop DOES close the batch (persist alone is for pauses)
  xferServerDown();
  /* A paused breaker would otherwise RESUME the WebServer from the pump while a window keeps
   * the transport (and therefore the pump) alive — an uploader nobody started. */
  s_breakerPaused = false;
  s_lowSinceMs = 0;
  if (!s_winOn) {
    transportDown("uploader stopped");
  }
}

// ---------------------------------------------------------------- the KOSync window's transport
/* Why the last WINDOW start was refused. Its own string, never s_startErr: that one is what
 * the uploader screens show under "Start", and a window refused five minutes ago must not
 * appear there as the reason an upload did not start. */
static const char* s_winErr = NULL;
static char        s_winWhy[72];
const char* xferWindowError() { return s_winErr; }

static bool windowRefused(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static bool windowRefused(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_winWhy, sizeof(s_winWhy), fmt, ap);
  va_end(ap);
  s_winErr = s_winWhy;
  if (!s_winOn) {
    free(s_ksBuf);          // nothing will read into it: do not keep 2 KB for a refusal
    s_ksBuf = NULL;
  }
  log_e("XFER: sync window refused - %s", s_winWhy);
  return false;
}

/* ⚠ NO SCREEN HOLD AND NO FULL-SPEED CPU FOR A WINDOW (0.9.79 review). Both were copied from the
 * uploader, which needs them: a person is watching its screen, and it streams megabytes
 * through the SD card. A window answers a handful of ~300-byte requests. Checked, not
 * assumed: gbcXferHandleClient() is called on every loop() pass whatever the screen is doing,
 * and "sleep" on this phone is the BACKLIGHT (GUI::toggleScreen / lcdOnOff) — the only
 * esp_light_sleep_start() calls are the STEAL_THE_USER_BUTTONS debug key (compiled out,
 * config.h) and the serial `power sleep` bench, which refuses while anything is serving. At
 * 80 MHz and the 5 ms idle tick a request waits at most a tick. What a window DOES keep is
 * the softAP WiFi gates (xferServing() in WiPhone.ino): those are about not pa{user}ing the
 * chip, not about speed. */
bool xferWindowStart(const char* apName, const char* pass) {
  s_winErr = NULL;
  if (s_winOn && s_on) {
    return true;
  }
  if (gGbcActive) {
    // A game has WiFi off and the watchdogs down; it closed any window as it started.
    return windowRefused("a Game Boy game is running");
  }
  if (!s_ksBuf) {
    s_ksBuf = (char*)ps_malloc(RAW_KS_BODY_CAP);
    if (!s_ksBuf) {
      return windowRefused("No PSRAM for the sync window");
    }
  }
  if (s_on) {
    /* The uploader is up. Its network is ours too — unless it is hosting a hotspot under
     * another name, which the X4 would not be looking for. Say so rather than tearing down
     * an upload someone is in the middle of. */
    if (s_usingAP && strcmp(s_apName, apName) != 0) {
      return windowRefused("Uploader is hosting '%s' - stop it first", s_apName);
    }
    s_winOn = true;
    return true;
  }
  /* ⚠ A LOWER BAR THAN THE UPLOADER'S 10240, ON PURPOSE — AND CHOSEN, NOT MEASURED. A window
   * never creates the WebServer (its ~10 KB per-request transient is what the uploader's bar
   * protects); the raw pump's per-request cost is ~zero. What remains is the AP bring-up and
   * one listening socket. 6144 is the breaker's own trip line: below it this phone is
   * already in the altitude where the WiFi PHY aborts, and nothing should be started. Bench
   * item: log `largest` across a real window (the KOSYNC health line does). */
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (largest < 6144) {
    return windowRefused("Low memory (%u B free block) - restart the phone", (unsigned)largest);
  }
  const char* err = NULL;
  /* The 2 s grace only for an association genuinely in flight: a join started, or the station
   * UP moments ago (a blip or a roam, rejoined by the core itself) — not out of range. */
  const bool wait = kosyncWindowWaitsForSta(wifiState.radioOff(), wifiState.userDisabled(),
                                            (WiFi.getMode() & WIFI_MODE_STA) != 0, millis(),
                                            lastWifiConnectAttemptMs(), lastWifiLinkUpMs());
  if (!transportUp(apName, pass, wait, &err)) {
    return windowRefused("%s", err ? err : "no network");
  }
  s_winOn = true;
  return true;
}

void xferWindowStop() {
  if (!s_winOn) {
    return;
  }
  s_winOn = false;
  if (s_rawKs) {
    rawAbort();             // a KOSync body mid-read must not land in a freed buffer
  }
  if (!s_upOn) {
    transportDown("sync window closed");
  }
  free(s_ksBuf);            // region-agnostic free(), as everywhere in this firmware
  s_ksBuf = NULL;
}
