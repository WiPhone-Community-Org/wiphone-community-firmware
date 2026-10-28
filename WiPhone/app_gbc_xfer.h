/*
 * app_gbc_xfer.h — the phone's file-transfer web server (core).
 *
 * Turns the phone into a little upload server so you can put files on the SD card from a
 * computer without typing on the phone. Open http://wiphone.local (or the shown IP) in a
 * browser and drag-and-drop, or paste a direct download URL. Falls back to a WiFi hotspot
 * when the phone isn't joined to a network.
 *
 * ONE server, PARAMETERISED — not one per app. It began as the Game Boy ROM uploader and is
 * now also the book uploader. Copying it was the obvious move and the wrong one: the two
 * things that make it survive on hardware (feeding the task watchdog inside a long POST, and
 * one file per request) are exactly the kind of lesson that rots in a copy. There is also
 * only one port 80, so a second server object would be a bug waiting for someone to open
 * both screens.
 *
 * Each app supplies an XferConfig and owns its own screen; this module owns only the server.
 */

#ifndef APP_GBC_XFER_H
#define APP_GBC_XFER_H

struct XferConfig {
  const char* dir;          // where uploads land, e.g. "/books" (created if missing)
  const char* heading;      // page title, e.g. "Add books"
  const char* accept;       // browser file-picker filter, e.g. ".epub,.txt"
  const char* nounPlural;   // "ROMs" / "books" — used in the page text and the count
  const char* defaultName;  // filename for a URL fetch that has no usable one
  const char* apName;       // hotspot SSID when the phone has no network
  bool        tree;         // accept `a/b/c.ext` names (chunkSafeTreeName) and create the folders;
                            // false = basename only. Trailing so the older aggregate initialisers
                            // stay valid (they zero it). 🛑 A config built FIELD BY FIELD must set
                            // it explicitly — app_files.cpp learned this the hard way.
};

// Pumped from the main loop every iteration (no-op unless the server is running).
// Running the web server in the main loop context — rather than a task — keeps SD writes and
// the rest of the firmware's SPI use from colliding.
void gbcXferHandleClient();

void        xferStart(const XferConfig* cfg);   // bring it up (join WiFi or make an AP)
/* Stop the UPLOADER and drop the AP if we made one — unless a KOSync window still holds the
 * transport, in which case the network and :80 stay up for it (see xferWindowStart). */
void        xferStop();
bool        gbcXferOn();                        // the UPLOADER is up (what the screens show)
bool        xferUsingAP();                      // true if we had to bring up our own hotspot
/* The hotspot on the air is WPA2 (a KOSync window's hotspot_pass). An uploader that started
 * while such a window held 'WiPhone-Books' rides on it, so its screen must not say "no
 * password". Never the password itself — nothing outside the driver ever sees it. */
bool        xferApProtected();
bool        xferApByUploader();                 // the hotspot was brought up by the uploader

/* ── The KOSync sync window's transport (0.9.79; the window itself is kosync_sync.h) ──────
 * The network (the phone's WiFi address, or its own open hotspot `apName`), the raw :80
 * listener and the screen hold — WITHOUT the uploader: no page, no /chunk, no :8081, no
 * WebServer, and (since the 0.9.79 review) no screen hold and no 240 MHz: see the .cpp.
 * Shares a running uploader when it can (same network or same hotspot name). False (and
 * xferWindowError() says why) on low memory, a failed hotspot, or a game running. */
bool        xferWindowStart(const char* apName, const char* pass);   // pass NULL/"" = open
const char* xferWindowError();                  // why the last window start was refused
                                                // (never xferStartError(): that is the uploader's)
void        xferWindowStop();                   // the transport goes only if the uploader is off too
bool        xferWindowUp();                     // a window holds the transport right now
/* ANY server is up — uploader or window. THIS is the term for the softAP WiFi gates in
 * WiPhone.ino (no station join or scan under a live hotspot), which must hold for a window
 * exactly as they do for the uploader's. The busy/240 MHz predicate uses xferOn() — the
 * uploader alone. */
bool        xferServing();
const XferConfig* xferBooksConfig();            // the books cfg, for serial `up on books`
const char* xferAddr();                         // IP address to show next to wiphone.local
const char* xferStartError();                   // why the last start was refused, or NULL
const XferConfig* xferPhotosConfig();            // /photos, for `up on photos`
const XferConfig* xferT9Config();                // /t9, for `up on t9`
const XferConfig* xferMapsConfig();              // /maps, tree mode, for `up on maps`
const XferConfig* xferKeysConfig();              // /API Keys, for `up on keys`
const char* xferApName();                       // SSID of the hotspot, when we made one
int         xferFilesAdded();                   // files added this session (uploads + fetches)

// The Game Boy app's call sites, unchanged.
void gbcXferStart();
inline void gbcXferStop()         { xferStop(); }
inline bool xferOn()              { return gbcXferOn(); }
inline bool gbcXferUsingAP()      { return xferUsingAP(); }
inline const char* gbcXferAddr()  { return xferAddr(); }
inline int  gbcXferRomsAdded()    { return xferFilesAdded(); }

#endif // APP_GBC_XFER_H
