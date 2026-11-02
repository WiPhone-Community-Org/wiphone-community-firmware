/*
 * https_worker.h - the ONE task that talks HTTPS for the phone's apps (0.9.81): Menu > AI's
 * questions (ai_net.cpp), the Almanac's weather (weather_net.cpp) and, since 0.9.82, Wi-Fi
 * location's beaconDB lookups (geoloc_net.cpp), as JOB KINDS on one worker.
 * The map downloader keeps its own (tile_fetch.cpp: a map job holds that one for days).
 *
 * WHY ONE, SHARED (generalised from the AI's own worker, 2026-10-03):
 *   - every worker costs an 8 KB INTERNAL stack carved once and kept for life, and internal RAM is
 *     what this phone runs out of;
 *   - two TLS handshakes at once dip the internal heap ~24 KB (~12 KB of lwIP buffers each): one
 *     worker serialises them for free - the AI and the weather QUEUE behind each other, never
 *     two handshakes at once (a job submitted while the other kind runs waits; the AI goes first
 *     when both wait - it is the one a person is staring at);
 *   - every loop hook keys on one predicate: netRequestActive() puts a job in WiPhone.ino's `busy`,
 *     its `hardBusy` (the handshake runs at 160 MHz with WiFi on, never at the 80 MHz idle clock)
 *     and the loop's 10-tick sleep (the worker gets ~90 % of the core: a 1-tick loop beside a
 *     handshake failed 3 of 3 on phone 1).
 * The worker is pinned to core 1 at priority 1 (the loop's core and priority), waits on a binary
 * semaphore, and runs each queued job's function to its end. Never a per-run task: a static one
 * races its own TCB on this FreeRTOS, a dynamic one fragmented `largest` 26 -> 11 KB on phone 1.
 *
 * THE REQUEST (netHttps): one HTTPS exchange on a FRESH WiFiClientSecure - HTTPClient reuses any
 * connected socket whatever host the next URL names (tile_fetch.h), so a socket never outlives its
 * request. DNS first (hostByName: "no internet" told apart from a refusal); then, when asked, the
 * internal-heap bar is checked again right before the handshake (10 KB largest / 14 KB free:
 * tile_fetch's TF_CONNECT_*); then either HTTPClient connects (the AI: its path as it always was)
 * or the client connects FIRST - c->connect(host, 443, timeout) - so the handshake is timed alone,
 * AFTER http.begin() (begin on a connected client stop()s it: a second, untimed handshake) and
 * sendRequest rides that connection. setConnectTimeout caps TCP + the handshake (WiFiClientSecure
 * passes it on as handshake_timeout, 120 s by default). The body - chunked de-chunked by hand, or a
 * Content-Length one - goes into the caller's PSRAM sink, never getString() (which grows on the
 * internal heap), and is READ TO ITS END even past the sink's cap (the cap's excess is dropped and
 * flagged; a tap sees every byte - the weather counts alerts it cannot keep). mbedTLS allocates
 * from PSRAM through tile_fetch's hook, installed before the first WiFiClientSecure. A request that
 * must wait longer than HTTPClient's uint16 header timeout (the AI's, 2026-10-11) has its client
 * wait for the answer's first byte instead, in slices that honour `cancel` and `stopFn`
 * (https_worker.cpp, NetClient); a request with a `totalMs` budget (the AI's Wikipedia lookup) has
 * its connect shortened and its waits ended to fit it. Every other request is exactly what it was.
 */
#ifndef HTTPS_WORKER_H
#define HTTPS_WORKER_H

#include <stddef.h>
#include <stdint.h>

#define NET_STACK_BYTES       8192
#define NET_SINK_CAP          (64u * 1024u)   // one body: a 4096-token answer is ~16 KB; ~7 NWS alerts as served
/* The internal heap a handshake may start from (tile_fetch.cpp's TF_CONNECT_LARGEST/FREE), plus
 * NET_STACK_BYTES while the worker's stack is not carved yet. */
#define NET_CONNECT_LARGEST   (10u * 1024u)
#define NET_CONNECT_FREE      (14u * 1024u)
#define NET_HTTP_TIMEOUT_MAX  65535u          // HTTPClient::setTimeout(uint16_t): its header wait's ceiling

/* The worker runs the lowest queued kind first: a question a person is staring at, then the
 * weather they asked for, then Wi-Fi location (geoloc_net.h, 0.9.82: nobody waits on it). */
enum { NET_JOB_AI = 0, NET_JOB_WX, NET_JOB_GEO, NET_JOB_KINDS };

typedef void (*NetJobFn)(void);

/* LOOP TASK. Carve the stack (internal, kept), the semaphore, the task and the shared PSRAM sink.
 * False with `why` (one short sentence) when it cannot. Idempotent. */
bool netWorkerReady(char* why, size_t whyCap);
bool netWorkerCarved();                    // the stack exists (the heap bar needs 8 KB more until it does)
/* LOOP TASK. Queue `kind` (its function runs on the worker, which sets the module's own done flags
 * at its end). Only one job of a kind is ever queued: the module keeps its own busy flag. */
void netWorkerSubmit(int kind, NetJobFn run);
int  netWorkerRunning();                   // the kind on the worker now, -1 = none
bool netWorkerQueued(int kind);            // submitted, not started yet
/* Anything asked and not yet folded by its module: aiRequestActive() || wxRequestActive() ||
 * geoRequestActive(). WiPhone.ino's busy, hardBusy and the 10-tick sleep. */
bool netRequestActive();

/* The internal heap now (MALLOC_CAP_INTERNAL | 8BIT). */
void netHeapNow(uint32_t* freeB, uint32_t* largest, uint32_t* minEver);
/* The bar a handshake may start from: NET_CONNECT_LARGEST / NET_CONNECT_FREE (+ the stack while
 * it is not carved). False = under it (the numbers in *freeB / *largest). */
bool netHeapOkForHandshake(uint32_t* freeB, uint32_t* largest);

/* WORKER. The shared body buffer (NET_SINK_CAP + 1, PSRAM): one job runs at a time. */
char* netWorkerSink();
/* WORKER. The worker's stack high-water mark so far, bytes left (the floor `ai` / `wx` print). */
uint32_t netWorkerStackFloor();

// ── one HTTPS exchange (WORKER) ──────────────────────────────────────────────────────────
typedef void (*NetTapFn)(void* ctx, const char* p, size_t n);
typedef bool (*NetStopFn)(void);
struct NetHttpReq {
  const char*  method;           // "GET" / "POST"
  const char*  host;             // for DNS and, with connectFirst, the handshake
  const char*  url;              // ⚠ never logged (the weather's has the place in it)
  const char*  rootsPem;         // the pinned roots (concatenated PEMs); never NULL - no insecure path
  const char*  userAgent;
  const char*  hdrName[3];       // extra request headers (NULL-terminated pairs)
  const char*  hdrValue[3];
  const uint8_t* body;           // POST body, NULL for a GET
  size_t       bodyLen;
  uint32_t     connectMs;        // TCP connect + the TLS handshake
  /* The wait for the response headers. HTTPClient's own is a uint16_t (NET_HTTP_TIMEOUT_MAX) and
   * never sees `cancel`; with waitSliceMs set, or a timeoutMs past that, the CLIENT waits for the
   * answer's first byte itself - in waitSliceMs slices (10 ms when 0), honouring `cancel` and
   * `stopFn` - up to timeoutMs (2026-10-11, for Gemini's long answers: the AI asks 150 s). */
  uint32_t     timeoutMs;
  uint32_t     waitSliceMs;
  uint32_t     bodyIdleMs;       // a body that stops arriving this long is a dead socket
  /* The whole exchange, DNS to the last body byte, at most this (0 = no budget): checked after
   * DNS (none left: GEM_NET_TIMEOUT, nothing sent), the connect shortened to what is left, checked
   * again before the request is written, HTTPClient's own header wait clamped to what is left right
   * before it (round-2 review), and - armed - the first-byte wait, the header lines (NetClient's
   * reading guard) and the body ended at it (GEM_NET_TIMEOUT). DNS itself cannot be cut short
   * (lwIP's own ~14 s), nor the handshake once started - hence "shortened", not "interrupted". */
  uint32_t     totalMs;
  bool         connectFirst;     // connect the client before http.begin(): the handshake timed alone
  bool         heapCheck;        // the internal-heap bar again, right before the handshake
  volatile bool* cancel;         // set from the loop: the request ends (GEM_NET_LOST)
  /* A second reason to end it, polled with `cancel` - after DNS, in the client's first-byte wait
   * and while the answer's header lines are read (when armed), and between body reads; true =
   * GEM_NET_LOST, as a Cancel. Called on the WORKER: a cheap flag read only (the AI passes
   * tileFetchCallRecent: a call rang in). May be NULL. */
  NetStopFn    stopFn;
  char*        sink;             // PSRAM; NUL-terminated on success
  size_t       cap;              // body bytes kept (the rest read and dropped: res.overflow)
  NetTapFn     tap;              // every DE-CHUNKED body byte, kept or not (may be NULL)
  void*        tapCtx;
};
struct NetHttpRes {
  int      code;                 // the HTTP status (> 0), or 0 with `net` saying what failed
  int      net;                  // GEM_NET_* (gemini.h: one vocabulary for both kinds)
  size_t   got;                  // body bytes in the sink
  bool     overflow;             // the body was longer than `cap` (read to its end all the same)
  uint32_t dnsMs, hsMs, ms;      // DNS; TCP + handshake (connectFirst only, else 0); the whole request
  int      tlsErr;               // WiFiClientSecure::lastError() when the connect failed, else 0
  uint32_t largestAfterSend;     // the internal heap's largest block right after the request went
                                 // (the handshake's dip; 0 = never got that far) - `ai`'s floor
  char     date[40];             // the response's Date header ("" none): the fetch-time fallback
};
/* The whole exchange. Returns res->code. A body that could not be read whole is a failure even
 * under a 200 (code 0, net LOST / TIMEOUT / BAD_BODY) - EXCEPT an overflow, which keeps the code
 * and sets res->overflow (the caller decides: the AI calls it TOO_BIG, the weather "too many"). */
int netHttps(const NetHttpReq* q, NetHttpRes* r);

#endif // HTTPS_WORKER_H
