/*
 * ai_net.cpp - see ai_net.h. The decisions (the key file's format, the body, the answer, the
 * ladder, the chat's rules) are gemini.cpp's and host-tested; this file is the card, the gates,
 * the worker and the wire.
 */
#include "ai_net.h"
#include "gemini.h"
#include "wiki_lookup.h"        // the Wikipedia lookup before each question: keywords, URL, parse, background
#include "isrg_roots.h"         // ...and the roots en.wikipedia.org chains to (X1 + X2 + Root YE)
#include "https_worker.h"       // the worker, shared with the weather since 0.9.81, and netHttps
#include "weather_net.h"        // wxRequestActive: a question asked behind a weather fetch
#include "tile_fetch.h"         // tlsInstallPsramHook, tileFetchActive, tileFetchCallRecent, filesJobActive
#include "app_gbc_xfer.h"       // xferOn / xferWindowUp: the uploader or a sync window holds the radio
#include "kosync_sync.h"        // kosyncWindowActive
#include "Networks.h"           // wifiState.radioOff()
#include "clock.h"              // ntpClock: the exchanges' times, the daily limits' deadlines
#include "tz_service.h"         // tzSvcEffective: the zone's name in the request's date sentence
#include "music_player.h"       // a question pauses the music (it needs the core for the handshake)

#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <Preferences.h>        // "Retry if it times out" (NVS, the firmware's "wpmesh" namespace)
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

extern volatile bool gGbcActive;          // WiPhone.ino: the emulator owns the card and turns WiFi off

/* The worker's stack: internal RAM, carved on the first question and kept (tile_fetch.cpp's rule
 * and its reasons) - since 0.9.81 the shared HTTPS worker's (https_worker.h, NET_STACK_BYTES), the
 * weather's too. The tile worker's deepest path - a handshake, then a tile through decode and
 * the card - used ~5.2 KB of its 8 KB; this one does a handshake against a PINNED root (an
 * RSA-4096 signature check the insecure tile client never makes) and parses JSON that is read
 * with no recursion (gemini.cpp). `ai` prints the floor after every question: under ~1,500 B,
 * raise it. The de-chunked body goes into the worker's shared PSRAM sink (NET_SINK_CAP, 64 KB: a
 * 4096-token answer is ~16 KB of JSON-escaped text). */
#define AI_SINK_CAP         NET_SINK_CAP
#define AI_TEXT_CAP         (24u * 1024u)   // the joined answer before cleaning (cleaned and cut to GEM_A_MAX)
#define AI_KEY_READ_MAX     2048u
#define AI_CHAT_READ_MAX    (GEM_CHAT_FILE_MAX + 4096u)
#define AI_CONNECT_MS       10000           // TCP connect AND the TLS handshake (WiFiClientSecure's handshake_timeout)
/* The wait for the answer's headers (2026-10-11: was 60 s, the most HTTPClient's uint16_t timeout can
 * hold). A non-streaming answer's headers come only once the WHOLE answer is written: a thinking
 * model took ~6-12 s on 2026-10-03, but free-tier 3.8 Flash now takes 30-90 s (84 s for one
 * sentence, measured 2026-10-11) and COVEY logged two timeouts at its 60 s. https_worker's client
 * waits for the first byte itself, in AI_SLICE_MS slices that honour Cancel and a call
 * (NetHttpReq.waitSliceMs, .stopFn), so 150 s works. Nothing else watches the wait: the worker is
 * on no task watchdog (only the loop task and core 0's idle task are) and sleeps between slices,
 * the loop runs on beside it (LOOP STALL times the loop's own passes), and the WAITING screen only
 * counts the seconds. */
#define AI_TIMEOUT_MS       150000u
#define AI_BODY_IDLE_MS     30000u          // the body arrives in one go once the headers have; this is a dead socket
#define AI_SLICE_MS         200u            // every wait is cut into these, and honours Cancel
/* The internal heap a handshake may start from: tile_fetch.cpp's TF_CONNECT_LARGEST/FREE (the
 * handshake dips ~12 KB of lwIP buffers), plus the stack while it is not carved yet
 * (https_worker.h: NET_CONNECT_LARGEST / NET_CONNECT_FREE, netHeapOkForHandshake). */
#define AI_USER_AGENT       "WiPhone-ai/0.1 (wiphone-meshtastic)"

/* THE WIKIPEDIA LOOKUP (2026-10-11, wiki_lookup.h): one GET to en.wikipedia.org before each question,
 * never retried, its failures silent. Its own budget: 10 s for TCP + the handshake (the weather's and
 * beaconDB's cap), 20 s for the whole of it, DNS to the last byte (NetHttpReq.totalMs) - a question
 * is not held up long by a lookup that is not coming. The response is read into the shared sink up
 * to WIKI_RESPONSE_CAP (24 KB; two pages of six sentences is ~2 KB) and parsed before Gemini's
 * request reuses the sink. The roots are the ones Wi-Fi location pins (isrg_roots.h; the chain is
 * in wiki_lookup.h) - a literal, so the linker can share it with geoloc_net.cpp's. */
#define AI_WIKI_CONNECT_MS  10000u
#define AI_WIKI_TOTAL_MS    20000u
#define AI_WIKI_IDLE_MS     5000u           // a body that stops this long (it comes in one go)
#define AI_WIKI_ROOTS_PEM   ISRG_ROOT_X1_PEM ISRG_ROOT_X2_PEM ISRG_ROOT_YE_PEM

/* GOOGLE'S ROOTS, PINNED. generativelanguage.googleapis.com chains leaf <- WR2 <- GTS Root R1
 * (checked live with openssl, ECDHE-ECDSA and ECDHE-RSA, 2026-10); R4 (P-384) is here in case the
 * API moves to the WE* ECDSA intermediates. mbedTLS takes the two PEMs concatenated
 * (ssl_client.cpp: mbedtls_x509_crt_parse of the whole buffer). Exported from the macOS system
 * keychain (SystemRootCertificates) and checked against certifi's bundle - the same bytes:
 *   GTS Root R1  SHA-256 D9:47:43:2A:BD:E7:B7:FA:90:FC:2E:6B:59:10:1B:12:80:E0:E1:C7:E4:E4:0F:A3:C6:88:7F:FF:57:A7:F4:CF
 *   GTS Root R4  SHA-256 34:9D:FA:40:58:C5:E2:63:12:3B:39:8A:E7:95:57:3C:4E:13:13:C8:3F:E6:8F:93:55:6C:D5:E8:03:1B:3C:7D
 * Both valid 2016-06-22 to 2036-06-22. This build never checks certificate DATES
 * (CONFIG_MBEDTLS_HAVE_TIME_DATE is unset), so a question works before the clock is set.
 * ⚠ THESE ARE THE ONLY ROOTS, ON PURPOSE: no override file on the card. The uploader into
 * /API Keys has no password and can run on the phone's own OPEN hotspot, so a "gemini.pem"
 * override (the first cut had one, after ota.cpp's /user.pem) let anyone nearby plant their own
 * root and then read the key off the wire. If Google ever leaves both roots (they run to 2036),
 * that is a firmware update. */
static const char AI_ROOTS_PEM[] =
  // GTS Root R1
  "-----BEGIN CERTIFICATE-----\n"
  "MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
  "CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
  "MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
  "MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
  "Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
  "A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
  "27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
  "Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
  "TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
  "qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
  "szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
  "Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
  "MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
  "wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
  "aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
  "VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
  "AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
  "FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
  "C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
  "QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
  "h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
  "7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
  "ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
  "MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
  "Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n"
  "6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
  "0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
  "2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
  "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
  "-----END CERTIFICATE-----\n"
  // GTS Root R4
  "-----BEGIN CERTIFICATE-----\n"
  "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
  "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
  "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
  "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
  "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
  "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
  "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
  "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
  "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
  "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
  "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
  "-----END CERTIFICATE-----\n";

// ── module state ─────────────────────────────────────────────────────────────────────────

/* The question in flight. Written on the loop by aiAsk BEFORE the semaphore is given; the
 * worker's until s_done; read again by the loop only in aiLoopTick. PSRAM. */
struct AiReq {
  GeminiConfig cfg;                       // ⚠ a copy of the key: wiped when the question ends
  char         question[GEM_Q_MAX];
  int          nCtx;
  char         ctxQ[GEM_CONTEXT][GEM_Q_MAX];
  char         ctxA[GEM_CONTEXT][GEM_A_MAX];
  bool         topic;                     // this question starts a topic (nothing earlier sent)
  int64_t      askedUtc;                  // 0 = the clock is not trusted
  int          tzS;
  char         zone[24];                  // the zone's name for the date sentence ("Pacific"), "" = none
  GeminiQuota  quota;                     // the remembered limits in; the worker marks it; folded back
  GeminiQuota  quotaIn;                   // ...as it went in (a changed one is saved)
  bool         musicPaused;               // aiAsk paused a playing track for this question
  bool         retryTimeouts;             // "Retry if it times out" as it was when asked (the ladder's rule 6)
  /* Asked while a weather fetch held the worker (0.9.81): aiAsk could not judge the heap then (the
   * weather's handshake was dipping it), so the worker checks the bar itself before the first
   * request. Otherwise aiAsk's check stands, as it always has. */
  bool         heapCheckAtStart;
};
/* What the worker found. The worker's while s_busy; the loop's after (it stays for `ai`). */
struct AiRes {
  bool     ok, cancelled;
  char     answer[GEM_A_MAX];
  char     label[GEM_LABEL_MAX];
  char     fail[256];
  int      lastCode, net, attempts;
  uint32_t ms;                            // ask to result
  uint32_t lastMs;                        // the last request alone
  uint32_t bodyBytes;
  bool     wikiAsked;                     // the Wikipedia lookup's GET was made
  int      wikiPages;                     // pages that went with the question (0 = no background)
  uint32_t wikiMs;                        // the lookup, start to end
  char     wikiWhy[48];                   // why it gave no background ("" = it did, or none was asked)
  /* ...and why no GET was made (a literal; `ai` prints it - review 2026-10-11: a question that ended
   * before its lookup read "lookup off, or no keywords"). aiRun sets the "ended before" one first. */
  const char* wikiNot;
  uint32_t largestFloor, minEver, stackFloor;
};
/* The worker's buffers - PSRAM, so its 8 KB stack is left to the handshake. (The body itself goes
 * into the shared worker's sink: netWorkerSink().) */
struct AiWork {
  char         text[AI_TEXT_CAP];
  GeminiLadder ladder;
  char         label[GEM_LABEL_MAX];
  char         primaryLabel[GEM_LABEL_MAX];
  char         stage[128];
  char         sys[GEM_SYS_MAX];            // the system instruction, built for each request
  char         kw[WIKI_KW_CAP];             // the question's keywords: ALL that goes to Wikipedia
  char         wikiUrl[WIKI_URL_MAX];
  WikiResult   wiki;                        // the pages read (~3.9 KB)
  char         bg[WIKI_BG_MAX + 1];         // the background for the question's turn ("" = none)
  char         src[WIKI_SRC_MAX + 1];       // "Wikipedia: A; B" under the answer (with bg only)
};

static AiReq*   s_req  = NULL;
static AiRes*   s_res  = NULL;
static AiWork*  s_work = NULL;
static char*    s_body = NULL;            // the request body, grown as needed (PSRAM)
static size_t   s_bodyCap = 0;

static volatile bool     s_busy   = false;   // asked; the worker has it (or will: queued behind a weather fetch)
static volatile bool     s_done   = false;   // the worker finished; the loop has not folded it yet
static volatile bool     s_cancel = false;
static bool              s_haveRes = false;  // s_res holds a finished question (for `ai`)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* The module's small state, in PSRAM rather than ~1.1 KB of the internal .bss (the internal heap
 * is what this phone runs out of). Allocated on first use; every entry point asks memOk(). */
struct AiMem {
  AiProgress    prog;                    // published under s_mux
  AiKeyInfo     key;                     // what the last read found
  GeminiQuota   quota;                   // the remembered daily limits
  GeminiSession session;                 // models that refuse thinkingConfig (this boot)
  bool          retryLoaded;             // "Retry if it times out" read from NVS (once)
  bool          retryOn;                 // ...and what it says (absent = ON)
};
static AiMem* s_m = NULL;

static bool memOk() {
  if (!s_m) {
    s_m = (AiMem*)heap_caps_calloc(1, sizeof(AiMem), MALLOC_CAP_SPIRAM);
  }
  return s_m != NULL;
}

static GeminiConfig* s_cfg = NULL;           // the last GOOD gemini.txt (PSRAM)
static bool          s_haveGood = false;
static int           s_keyLogged = -1;       // the state last logged (log once per change)
static int           s_keyMisses = 0;        // reloads in a row that found no gemini.txt

static GeminiChat*   s_chat = NULL;          // PSRAM, ~94 KB
static bool          s_chatLoaded = false;
/* /ai/chat.txt is there but the card would not give it up: NOT an empty chat. Never saved over
 * while this holds; what lands meanwhile is kept in memory and added to it once it reads. */
static bool          s_chatUnread = false;
/* Asked for while a question was still out (aiLoopTick applies them when it lands): "New topic"
 * goes after it, "Clear chat" drops it. */
static bool          s_breakAfter = false;
static bool          s_clearAfter = false;
static uint32_t      s_chatGen = 1;

static bool          s_quotaLoaded = false;
static bool          s_sessionInit = false;

static uint32_t s_asked = 0, s_answered = 0, s_failed = 0;

// ── small helpers ────────────────────────────────────────────────────────────────────────

static void heapNow(uint32_t* freeB, uint32_t* largest, uint32_t* minEver) {
  netHeapNow(freeB, largest, minEver);
}

static int64_t utcNow() {
  return (int64_t)ntpClock.getTrustedUtcTime();      // 0 = not trusted: no topic gaps, no saved deadlines
}

static int tzNow() {
  const int32_t d = (int32_t)(ntpClock.getExactUnixTime() - ntpClock.getExactUtcTime());
  const int32_t m = (d >= 0) ? (d + 30) / 60 : -((-d + 30) / 60);
  return (int)(m * 60);
}

/* "Phone low on memory (11 KB block, 30 KB free) - reboot first": aiAsk's refusal, and the worker's
 * when the heap bar or a handshake's allocation failed later in a question (review 2026-10-11: after
 * the Wikipedia lookup's handshake a fragmented heap read as a generic failure). The numbers now. */
static void lowMemText(char* out, size_t cap) {
  uint32_t f = 0, lg = 0;
  netHeapOkForHandshake(&f, &lg);
  snprintf(out, cap, "Phone low on memory (%u KB block, %u KB free) - reboot first", (unsigned)(lg / 1024),
           (unsigned)(f / 1024));
}

/* A small file into buf (NUL-terminated), retried once after 20 ms: the card shares the SPI bus
 * with the screen, and a failed open must never be read as "no file". -1 = unreadable. */
static int readSmall(const char* path, char* buf, size_t cap) {
  File f = SD.open(path, FILE_READ);
  if (!f) {
    delay(20);
    f = SD.open(path, FILE_READ);
  }
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

/* Write a whole file as <path>.tmp-style then rename into place (FAT cannot rename over a file). */
static bool writeFile(const char* path, const char* tmp, const char* data, size_t len) {
  if (!SD.exists(AI_DIR)) {
    SD.mkdir(AI_DIR);
  }
  if (SD.exists(tmp)) {
    SD.remove(tmp);                      // exists() first: remove() logs an error line for a missing file
  }
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) {
    return false;
  }
  const size_t w = f.write((const uint8_t*)data, len);
  f.close();
  if (w != len) {
    SD.remove(tmp);
    return false;
  }
  if (SD.exists(path)) {
    SD.remove(path);
  }
  return SD.rename(tmp, path);
}

static void progSet(const char* stage, const char* label, uint32_t waitUntil, int attempt) {
  if (!s_m) {
    return;
  }
  portENTER_CRITICAL(&s_mux);
  if (stage) {
    strlcpy(s_m->prog.stage, stage, sizeof(s_m->prog.stage));
  }
  if (label) {
    strlcpy(s_m->prog.label, label, sizeof(s_m->prog.label));
  }
  s_m->prog.waitUntilMs = waitUntil;
  if (attempt >= 0) {
    s_m->prog.attempt = attempt;
  }
  s_m->prog.gen++;
  portEXIT_CRITICAL(&s_mux);
}

// ── the key ──────────────────────────────────────────────────────────────────────────────

static void keyInfoFrom(const GeminiConfig* c, int rc) {
  geminiConfigLine(c, rc, s_m->key.line, sizeof(s_m->key.line));
  strlcpy(s_m->key.model, c->model, sizeof(s_m->key.model));
  strlcpy(s_m->key.fallback, c->fallback, sizeof(s_m->key.fallback));
  s_m->key.thinking = c->thinking;
  s_m->key.lookup = c->lookup;
}

void aiKeyReload() {
  if (!memOk()) {
    return;
  }
  if (!s_cfg) {
    s_cfg = (GeminiConfig*)heap_caps_calloc(1, sizeof(GeminiConfig), MALLOC_CAP_SPIRAM);
    if (!s_cfg) {
      s_m->key.state = AI_KEY_UNREADABLE;
      s_m->key.usable = false;
      strlcpy(s_m->key.line, "no memory to read the key file", sizeof(s_m->key.line));
      return;
    }
  }
  /* FAT is case-blind: "Gemini.txt" from a Mac is the same name. exists() first: in this core
   * SD.open() of a missing file prints "[E] ... does not exist". exists() is itself an open with
   * no retry, so "no file" is asked twice, 20 ms apart, as readSmall does. */
  const char* path = NULL;
  for (int tries = 0; tries < 2 && !path; tries++) {
    if (tries) {
      delay(20);
    }
    if (SD.exists(AI_KEY_FILE)) {
      path = AI_KEY_FILE;
    } else if (SD.exists(AI_KEY_FILE_ROOT)) {
      path = AI_KEY_FILE_ROOT;
    }
  }
  s_keyMisses = path ? 0 : (s_keyMisses < 2 ? s_keyMisses + 1 : 2);
  if (!path && s_haveGood && s_keyMisses < 2) {
    /* A good key, and now no file: a card hiccup until the next reload agrees (the spec: a failed
     * read is never "no key"). Two in a row (opening Key info reads twice) and it really is gone. */
    s_m->key.state = AI_KEY_UNREADABLE;
    s_m->key.usable = true;
    keyInfoFrom(s_cfg, GEM_CFG_OK);
  } else if (!path) {
    s_haveGood = false;
    geminiParseConfig("", 0, s_cfg);       // no key; the defaults, for the line
    keyInfoFrom(s_cfg, GEM_CFG_NO_KEY);
    s_m->key.state = AI_KEY_NONE;
    s_m->key.usable = false;
    s_m->key.path = NULL;
  } else {
    // PSRAM, and wiped after: the key is in these bytes.
    char* buf = (char*)heap_caps_malloc(AI_KEY_READ_MAX, MALLOC_CAP_SPIRAM);
    const int n = buf ? readSmall(path, buf, AI_KEY_READ_MAX) : -1;
    if (n < 0) {
      /* The card would not give it up. NOT "no key": the last good one stays (the T-Deck latched
       * "no key" for a session on one failed early read). */
      s_m->key.state = AI_KEY_UNREADABLE;
      s_m->key.usable = s_haveGood;
      s_m->key.path = path;
      if (s_haveGood) {
        keyInfoFrom(s_cfg, GEM_CFG_OK);
      } else {
        strlcpy(s_m->key.line, "key: the file could not be read - try again", sizeof(s_m->key.line));
      }
    } else {
      GeminiConfig* c = (GeminiConfig*)heap_caps_calloc(1, sizeof(GeminiConfig), MALLOC_CAP_SPIRAM);
      if (!c) {
        s_m->key.state = AI_KEY_UNREADABLE;
        s_m->key.usable = s_haveGood;
      } else {
        const int rc = geminiParseConfig(buf, (size_t)n, c);
        keyInfoFrom(c, rc);
        s_m->key.path = path;
        if (rc == GEM_CFG_OK) {
          memcpy(s_cfg, c, sizeof(*s_cfg));
          s_haveGood = true;
          s_m->key.state = AI_KEY_OK;
          s_m->key.usable = true;
        } else {
          memset(s_cfg, 0, sizeof(*s_cfg));
          s_haveGood = false;
          s_m->key.state = rc == GEM_CFG_NO_KEY ? AI_KEY_NONE : AI_KEY_BAD;
          s_m->key.usable = false;
        }
        memset(c, 0, sizeof(*c));
        free(c);
      }
    }
    if (buf) {
      memset(buf, 0, AI_KEY_READ_MAX);
      free(buf);
    }
  }
  if (s_m->key.state != s_keyLogged) {
    s_keyLogged = s_m->key.state;
    // ⚠ Never the key: its length, the model and where the file is.
    log_e("AI: %s (%s)", s_m->key.line, s_m->key.path ? s_m->key.path : "no gemini.txt");
  }
}

void aiKeyInfo(AiKeyInfo* out) {
  if (!memOk()) {
    memset(out, 0, sizeof(*out));
    strlcpy(out->line, "no memory", sizeof(out->line));
    return;
  }
  *out = s_m->key;
}

// ── the chat and the remembered limits ────────────────────────────────────────────────────

static void chatSave();

/* The saved chat, read on first use. Three outcomes, kept apart (review 2026-10-03): no file
 * (an empty chat), a file that is not a chat (empty - and the file set aside as chat.bad, never
 * just overwritten), and a file the card would not give up (NOT empty: s_chatUnread, read again
 * on the next call, and nothing is saved over it meanwhile). */
static GeminiChat* chatGet() {
  if (s_chatLoaded) {
    return s_chat;
  }
  if (!s_chat) {
    s_chat = (GeminiChat*)heap_caps_calloc(1, sizeof(GeminiChat), MALLOC_CAP_SPIRAM);
    if (!s_chat) {
      return NULL;
    }
    geminiChatClear(s_chat);
  }
  const char* path = NULL;
  for (int tries = 0; tries < 2 && !path; tries++) {
    if (tries) {
      delay(20);                           // exists() is an open with no retry (readSmall's rule)
    }
    path = SD.exists(AI_CHAT_FILE) ? AI_CHAT_FILE : (SD.exists(AI_CHAT_TMP) ? AI_CHAT_TMP : NULL);
  }
  if (!path) {
    s_chatLoaded = true;                   // no chat yet: what is in memory is the chat
    s_chatUnread = false;
    s_chatGen++;
    return s_chat;
  }
  char* buf = (char*)heap_caps_malloc(AI_CHAT_READ_MAX, MALLOC_CAP_SPIRAM);
  const int n = buf ? readSmall(path, buf, AI_CHAT_READ_MAX) : -1;
  if (n < 0) {
    if (!s_chatUnread) {
      log_e("AI: %s unreadable just now - kept, and read again later", path);
    }
    s_chatUnread = true;
    free(buf);
    return s_chat;
  }
  /* Exchanges that landed while the file could not be read go on after what it holds. */
  GeminiChat* held = (s_chatUnread && s_chat->n > 0) ? s_chat : NULL;
  GeminiChat* into = s_chat;
  if (held) {
    into = (GeminiChat*)heap_caps_calloc(1, sizeof(GeminiChat), MALLOC_CAP_SPIRAM);
    if (!into) {
      free(buf);
      return s_chat;                       // still unread: tried again on the next call
    }
  }
  if (!geminiChatLoad(buf, (size_t)n, into)) {
    log_e("AI: %s is not a chat this firmware reads - set aside as %s", path, AI_CHAT_BAD);
    geminiChatClear(into);
    if (SD.exists(AI_CHAT_BAD)) {
      SD.remove(AI_CHAT_BAD);
    }
    SD.rename(path, AI_CHAT_BAD);
  }
  free(buf);
  s_chatLoaded = true;
  s_chatUnread = false;
  if (held) {
    for (int i = 0; i < held->n; i++) {
      const GeminiExchange* e = &held->ex[i];
      geminiChatAdd(into, e->t, (e->flags & GEM_EX_TOPIC) != 0, (e->flags & GEM_EX_FAILED) != 0,
                    e->label, e->q, e->a);
    }
    if (held->breakPending) {
      geminiChatNewTopic(into);
    }
    free(held);
    s_chat = into;
    chatSave();
  }
  s_chatGen++;
  return s_chat;
}

static void chatSave() {
  if (!s_chat) {
    return;
  }
  if (s_chatUnread) {
    log_e("AI: %s not saved - the one on the card has not been read yet", AI_CHAT_FILE);
    return;
  }
  const size_t n = geminiChatSave(s_chat, NULL, 0);
  char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (!buf) {
    log_e("AI: no memory to save the chat");
    return;
  }
  geminiChatSave(s_chat, buf, n + 1);
  if (!writeFile(AI_CHAT_FILE, AI_CHAT_TMP, buf, n)) {
    log_e("AI: the card refused %s", AI_CHAT_FILE);
  }
  free(buf);
}

const GeminiChat* aiChat() {
  return chatGet();
}

uint32_t aiChatGen() {
  return s_chatGen;
}

void aiChatNewTopic() {
  GeminiChat* c = chatGet();
  if (aiRequestActive()) {
    s_breakAfter = true;                   // the question out was asked BEFORE the break
  }
  if (c && c->n > 0 && !c->breakPending) {
    geminiChatNewTopic(c);
    chatSave();
    s_chatGen++;
  }
}

void aiChatClear() {
  if (aiRequestActive()) {
    s_clearAfter = true;                   // the question out is dropped when it lands
  }
  if (!s_chat) {
    s_chat = (GeminiChat*)heap_caps_calloc(1, sizeof(GeminiChat), MALLOC_CAP_SPIRAM);
  }
  if (s_chat) {
    geminiChatClear(s_chat);
  }
  s_chatLoaded = s_chat != NULL;           // the user's word: whatever the card held is gone
  s_chatUnread = false;
  if (SD.exists(AI_CHAT_FILE)) SD.remove(AI_CHAT_FILE);
  if (SD.exists(AI_CHAT_TMP))  SD.remove(AI_CHAT_TMP);
  s_chatGen++;
}

static void quotaLoad() {
  if (s_quotaLoaded || !memOk()) {
    return;
  }
  s_quotaLoaded = true;
  geminiQuotaClear(&s_m->quota);
  /* state.tmp when state.txt is missing, as the chat does: writeFile removes the old file before
   * the rename, so a power cut between the two leaves only the .tmp - whole, and the newest. */
  const char* path = SD.exists(AI_STATE_FILE) ? AI_STATE_FILE : (SD.exists(AI_STATE_TMP) ? AI_STATE_TMP : NULL);
  if (!path) {
    return;
  }
  char* buf = (char*)heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
  const int n = buf ? readSmall(path, buf, 512) : -1;
  if (n < 0 || !geminiQuotaLoad(buf, (size_t)n, &s_m->quota)) {
    geminiQuotaClear(&s_m->quota);
  }
  free(buf);
}

static void quotaSave() {
  if (!s_m) {
    return;
  }
  char* buf = (char*)heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
  if (!buf) {
    return;
  }
  const size_t n = geminiQuotaSave(&s_m->quota, utcNow(), buf, 512);
  if (n < 512 && !writeFile(AI_STATE_FILE, AI_STATE_TMP, buf, n)) {
    log_e("AI: the card refused %s", AI_STATE_FILE);
  }
  free(buf);
}

size_t aiQuotaNote(char* out, size_t cap) {
  if (!out || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  quotaLoad();
  if (!s_m) {
    return 0;
  }
  const char* model = s_m->key.model[0] ? s_m->key.model : GEM_DEFAULT_MODEL;
  const int64_t now = utcNow();
  const uint32_t ms = millis();
  int64_t until;
  uint32_t remain;
  if (!geminiQuotaWhen(&s_m->quota, model, now, ms, &until, &remain)) {
    return 0;
  }
  if (s_m->key.fallback[0] && geminiQuotaOut(&s_m->quota, s_m->key.fallback, now, ms)) {
    geminiQuotaBack(&s_m->quota, model, s_m->key.fallback, now, ms, &until, &remain);
    return geminiQuotaAllOut(until, now, tzNow(), remain, out, cap);
  }
  char a[GEM_LABEL_MAX], b[GEM_LABEL_MAX];
  geminiModelLabel(model, a, sizeof(a));
  geminiModelLabel(s_m->key.fallback, b, sizeof(b));
  return geminiQuotaNotice(a, s_m->key.fallback[0] ? b : "", until, now, tzNow(), remain, out, cap);
}

// ── "Retry if it times out" (round 3) ────────────────────────────────────────────────────

#define AI_PREFS_NS     "wpmesh"                 // the firmware's settings namespace (prefs_almanac.cpp)
#define AI_PREFS_RETRY  "ai_retry"               // uchar: 1 on, 0 off; absent = ON

bool aiRetryTimeouts() {
  if (!memOk()) {
    return true;                                   // the default
  }
  if (!s_m->retryLoaded) {
    s_m->retryLoaded = true;
    s_m->retryOn = true;
    Preferences p;
    /* Read-only begin() FAILS when the namespace was never written (a new phone): ON stands. */
    if (p.begin(AI_PREFS_NS, true)) {
      s_m->retryOn = p.getUChar(AI_PREFS_RETRY, 1) != 0;
      p.end();
    }
  }
  return s_m->retryOn;
}

void aiSetRetryTimeouts(bool on) {
  if (!memOk()) {
    return;
  }
  s_m->retryLoaded = true;
  s_m->retryOn = on;
  Preferences p;
  if (p.begin(AI_PREFS_NS, false)) {
    p.putUChar(AI_PREFS_RETRY, on ? 1 : 0);
    p.end();
  } else {
    log_e("AI: could not open NVS '%s' to save %s=%d (kept for this boot only)", AI_PREFS_NS, AI_PREFS_RETRY,
          on ? 1 : 0);
  }
}

// ── the wire (WORKER) ────────────────────────────────────────────────────────────────────

/* One POST, through the shared worker's exchange (https_worker.cpp: netHttps - the code that was
 * this file's postOnce/readBody until 0.9.81, the same steps in the same order: DNS first, a new
 * WiFiClientSecure with Google's roots, HTTPClient's own connect under AI_CONNECT_MS, the key in a
 * header, the chunked body de-chunked by hand into the PSRAM sink). The HTTP code (> 0), or 0 with
 * *net saying what failed. A body that could not be read is a failure even under a 200, and so is
 * one past the sink (GEM_NET_TOO_BIG, as before). */
static int postOnce(const char* model, size_t bodyLen, size_t* got, int* net) {
  *got = 0;
  *net = GEM_NET_OK;
  char url[sizeof(GEM_URL_PREFIX) + GEM_MODEL_MAX + sizeof(GEM_URL_SUFFIX)];
  snprintf(url, sizeof(url), "%s%s%s", GEM_URL_PREFIX, model, GEM_URL_SUFFIX);
  NetHttpReq q;
  memset(&q, 0, sizeof(q));
  q.method = "POST";
  q.host = GEM_HOST;
  q.url = url;
  q.rootsPem = AI_ROOTS_PEM;
  q.userAgent = AI_USER_AGENT;
  q.hdrName[0] = "Content-Type";
  q.hdrValue[0] = "application/json";
  q.hdrName[1] = "x-goog-api-key";               // ⚠ a header, never the URL
  q.hdrValue[1] = s_req->cfg.key;
  q.body = (const uint8_t*)s_body;
  q.bodyLen = bodyLen;
  q.connectMs = AI_CONNECT_MS;
  /* The heap bar again right before EVERY Gemini handshake (review 2026-10-11): the Wikipedia
   * lookup's handshake - or a retry's wait - can leave the heap fragmented since aiAsk looked, and
   * a handshake started under the bar fails as a generic TLS error. Under it: GEM_NET_NOMEM, and
   * aiRun says "reboot first" with the numbers. */
  q.heapCheck = true;
  q.timeoutMs = AI_TIMEOUT_MS;
  q.waitSliceMs = AI_SLICE_MS;                   // the wait for the answer: the client's own, cancellable
  q.bodyIdleMs = AI_BODY_IDLE_MS;
  q.cancel = &s_cancel;
  /* ...and a CALL ends it too (review 2026-10-11): tileFetchCallRecent is the loop's per-pass
   * flag from the SIP state (a live or ringing call, or the minute after one - which aiAsk's
   * gate already refused, so true here means one rang in). Up to 150 s behind a ringing phone
   * was the alternative; aiRun says "A call came in". */
  q.stopFn = tileFetchCallRecent;
  q.sink = netWorkerSink();
  q.cap = AI_SINK_CAP;
  NetHttpRes r;
  int code = netHttps(&q, &r);
  if (r.largestAfterSend && r.largestAfterSend < s_res->largestFloor) {
    s_res->largestFloor = r.largestAfterSend;
  }
  *net = r.net;
  *got = r.got;
  if (code > 0 && r.overflow) {
    *net = GEM_NET_TOO_BIG;
    code = 0;
  }
  return code;
}

static bool waitSliced(uint32_t ms) {
  const uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (s_cancel) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(AI_SLICE_MS));
  }
  return !s_cancel;
}

/* aiAsk's gates, and its reasons: WiFi, then the gates of a map download's Start (tile_fetch.cpp):
 * a running download keeps a TLS session of its own (two handshakes ~24 KB); the uploader and a
 * sync window run the radio as a server; a call's audio cannot share the handshake's heap dip; a
 * game owns the card and the SPI bus; a Files folder job is the card's too. NULL = clear. Cheap
 * flag reads: aiAsk asks on the loop, and the worker again before the first request of a question
 * that waited behind a weather fetch (final review 2026-10-03: that wait can be a minute). */
static const char* aiGateWhy() {
  if (wifiState.radioOff()) {
    return "WiFi is off (Settings > WiFi)";
  }
  if (WiFi.status() != WL_CONNECTED) {
    return "No WiFi - join a network first";
  }
  if (tileFetchActive()) {
    return "A map download is running";
  }
  if (xferOn()) {
    return "The WiFi uploader is on - stop it first";
  }
  if (kosyncWindowActive() || xferWindowUp()) {
    return "A book sync window is open";
  }
  if (tileFetchCallRecent()) {
    return "A call is on, or ended under a minute ago";
  }
  if (gGbcActive) {
    return "A game is running";
  }
  if (filesJobActive()) {
    return "A Files folder job is running - let it finish";
  }
  return NULL;
}

/* The topic's earlier questions - the context aiAsk copied, oldest first - as pointers for
 * wikiKeywords. Returns how many (0 when the question starts a topic). */
static int topicQuestions(const AiReq* q, const char* out[GEM_CONTEXT]) {
  int n = 0;
  for (int i = 0; i < q->nCtx && i < GEM_CONTEXT; i++) {
    out[n++] = q->ctxQ[i];
  }
  return n;
}

/* THE WIKIPEDIA LOOKUP (WORKER; once a question, before its first request - never per retry; see
 * wiki_lookup.h). ONE GET, never retried whatever happens: Wikipedia answered 429 after ~8 quick
 * requests, and the question is worth asking without it. PRIVACY: the URL is built from the
 * question's KEYWORDS alone - no key (no header but the User-Agent), no whole question, no chat.
 * Fills s_work->bg and ->src, or leaves both "" with r->wikiWhy saying why (aiLoopTick logs it):
 * any failure is silent and harmless, and the question goes to Gemini as it would have.
 * noinline ON PURPOSE: inlined (as it was, into aiRun), its request and response would sit in
 * aiRun's frame under every Gemini handshake that follows, on the worker's 8 KB stack. */
static void __attribute__((noinline)) aiWikiLookup(const AiReq* q, AiRes* r) {
  s_work->bg[0] = '\0';
  s_work->src[0] = '\0';
  r->wikiWhy[0] = '\0';
  r->wikiPages = 0;
  if (!q->cfg.lookup) {
    r->wikiNot = "lookup=off in gemini.txt";
    return;                                        // gemini.txt: lookup=off
  }
  /* A follow-up ("and when?") borrows from the topic's newest question that named something
   * (round 3: wikiKeywords walks back) - the questions sent as context, oldest first; none when
   * this question starts a topic. */
  const char* earlier[GEM_CONTEXT];
  const int nEarlier = topicQuestions(q, earlier);
  if (wikiKeywords(q->question, earlier, nEarlier, s_work->kw, sizeof(s_work->kw)) == 0) {
    r->wikiNot = "no keywords in the question";
    return;                                        // nothing to search for: no lookup at all
  }
  if (wikiUrl(s_work->kw, s_work->wikiUrl, sizeof(s_work->wikiUrl)) >= sizeof(s_work->wikiUrl)) {
    r->wikiNot = "the keywords did not fit the URL";
    strlcpy(r->wikiWhy, r->wikiNot, sizeof(r->wikiWhy));
    return;
  }
  progSet("Looking up Wikipedia...", NULL, 0, -1);
  const uint32_t t0 = millis();
  NetHttpReq w;
  memset(&w, 0, sizeof(w));
  w.method = "GET";
  w.host = WIKI_HOST;
  w.url = s_work->wikiUrl;                         // ⚠ never logged: the question's words are in it
  w.rootsPem = AI_WIKI_ROOTS_PEM;
  w.userAgent = WIKI_USER_AGENT;                   // Wikimedia's policy: a name and a contact
  w.connectMs = AI_WIKI_CONNECT_MS;
  w.timeoutMs = AI_WIKI_TOTAL_MS;
  w.waitSliceMs = AI_SLICE_MS;                     // the client's own first-byte wait: Cancel and a call end it
  w.bodyIdleMs = AI_WIKI_IDLE_MS;
  w.totalMs = AI_WIKI_TOTAL_MS;                    // the whole lookup, DNS included
  w.connectFirst = true;                           // the handshake timed alone, under its cap
  w.heapCheck = true;                              // the bar again before THIS handshake
  w.cancel = &s_cancel;
  w.stopFn = tileFetchCallRecent;                  // a call rang in: stop (aiRun then says so)
  w.sink = netWorkerSink();
  w.cap = WIKI_RESPONSE_CAP;
  NetHttpRes res;
  int code = netHttps(&w, &res);
  r->wikiAsked = true;
  r->wikiMs = millis() - t0;
  if (res.largestAfterSend && res.largestAfterSend < r->largestFloor) {
    r->largestFloor = res.largestAfterSend;
  }
  int net = res.net;
  if (code > 0 && res.overflow) {
    code = 0;
    net = GEM_NET_TOO_BIG;
  }
  if (s_cancel) {
    strlcpy(r->wikiWhy, "cancelled", sizeof(r->wikiWhy));
    return;
  }
  if (tileFetchCallRecent()) {
    strlcpy(r->wikiWhy, "a call came in", sizeof(r->wikiWhy));
    return;
  }
  if (code != 200) {
    wikiSkipWhy(code, net, WIKI_BAD, r->wikiWhy, sizeof(r->wikiWhy));
    return;
  }
  const int parsed = wikiParse(netWorkerSink(), res.got, &s_work->wiki);
  if (parsed != WIKI_OK) {
    wikiSkipWhy(200, GEM_NET_OK, parsed, r->wikiWhy, sizeof(r->wikiWhy));
    return;
  }
  /* Only the pages ABOUT the keywords (round 3), the background, and the sources line naming exactly
   * them - all or nothing: no page left, none that fits, or no title the line can show, and neither
   * goes (wikiCompose). */
  int why = WIKI_OK;
  const int used = wikiCompose(&s_work->wiki, s_work->kw, WIKI_BG_MAX, s_work->bg, sizeof(s_work->bg), s_work->src,
                               sizeof(s_work->src), &why);
  if (used <= 0) {
    s_work->bg[0] = '\0';
    s_work->src[0] = '\0';
    wikiSkipWhy(200, GEM_NET_OK, why, r->wikiWhy, sizeof(r->wikiWhy));
    return;
  }
  r->wikiPages = used;
}

/* The ladder for one question: a LOOP (the T-Deck's first retry called itself mid-TLS and died). */
static void aiRun() {
  AiReq* q = s_req;
  AiRes* r = s_res;
  const uint32_t t0 = millis();
  heapNow(NULL, &r->largestFloor, NULL);
  r->wikiNot = "the question ended before the lookup";   // aiWikiLookup says otherwise once it runs
  GeminiTurnPair ctx[GEM_CONTEXT];
  for (int i = 0; i < q->nCtx; i++) {
    ctx[i].q = q->ctxQ[i];
    ctx[i].a = q->ctxA[i];
  }
  if (q->heapCheckAtStart) {
    /* Asked while a weather fetch held the worker: aiAsk's gates again (the wait was long enough for
     * a call to ring in or a download to start), then its heap gate, here, now that it is done. */
    const char* gate = aiGateWhy();
    if (gate) {
      strlcpy(r->fail, gate, sizeof(r->fail));
      r->ms = millis() - t0;
      return;
    }
    if (!netHeapOkForHandshake(NULL, NULL)) {
      r->net = GEM_NET_NOMEM;
      lowMemText(r->fail, sizeof(r->fail));
      r->ms = millis() - t0;
      return;
    }
  }
  char* label = s_work->label;
  char* primaryLabel = s_work->primaryLabel;
  char* stageBuf = s_work->stage;
  const size_t stageCap = sizeof(s_work->stage);
  geminiModelLabel(q->cfg.model, primaryLabel, GEM_LABEL_MAX);
  GeminiLadder& l = s_work->ladder;
  int step = geminiLadderStart(&l, &q->cfg, &s_m->session, &q->quota, utcNow(), millis());
  l.retryTimeouts = q->retryTimeouts;     // "Retry if it times out" (the ladder's rule 6): the setting
  if (step == GEM_STEP_FAIL) {
    geminiFailText(0, GEM_NET_OK, NULL, &l, q->cfg.key, q->tzS, r->fail, sizeof(r->fail));
    r->ms = millis() - t0;
    return;
  }
  geminiModelLabel(l.model, label, GEM_LABEL_MAX);
  /* What Wikipedia says, looked up once before the first request (a model is there to ask: the
   * ladder started). A Cancel or a call during it ends the question here. */
  progSet(NULL, label, 0, 0);
  aiWikiLookup(q, r);
  if (s_cancel) {
    r->cancelled = true;
    r->ms = millis() - t0;
    return;
  }
  if (tileFetchCallRecent()) {
    strlcpy(r->fail, "A call came in - ask again after it", sizeof(r->fail));
    r->ms = millis() - t0;
    return;
  }
  if (l.quotaSwitch) {
    geminiQuotaNotice(primaryLabel, label, l.primaryUntil, l.nowUtc, q->tzS, l.primaryRemainS, stageBuf, stageCap);
    progSet(stageBuf, label, 0, 0);
  } else {
    progSet(q->musicPaused ? "Music paused - asking Gemini..." : "Asking Gemini...", label, 0, 0);
  }
  GeminiReply* rep = (GeminiReply*)heap_caps_calloc(1, sizeof(GeminiReply), MALLOC_CAP_SPIRAM);
  if (!rep) {
    r->net = GEM_NET_NOMEM;
    geminiFailText(0, GEM_NET_NOMEM, NULL, NULL, q->cfg.key, q->tzS, r->fail, sizeof(r->fail));
    return;
  }
  for (;;) {
    if (s_cancel) {
      r->cancelled = true;
      break;
    }
    /* aiAsk's call gate, again before every RETRY: a 503's 2.5 s or a per-minute wait is long
     * enough for a call to ring in, and a call's audio cannot share the handshake's heap dip. */
    if (r->attempts > 0 && tileFetchCallRecent()) {
      strlcpy(r->fail, "A call came in - ask again after it", sizeof(r->fail));
      break;
    }
    /* The system instruction NOW (2026-10-11): today's date from the clock as it is when this request
     * goes - only a trusted one (utcNow() is 0 otherwise: no date beats a wrong one). The lookup's
     * background goes in the QUESTION'S turn (s_work->bg), never in here; the context turns are the
     * chat's plain questions. */
    geminiSystemText(utcNow(), q->tzS, q->zone, s_work->sys, sizeof(s_work->sys));
    const size_t n = geminiBuildRequest(ctx, q->nCtx, q->question, s_work->bg, s_work->sys, l.budget,
                                        NULL, 0);
    if (n + 1 > s_bodyCap) {
      free(s_body);
      s_bodyCap = 0;
      s_body = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
      if (!s_body) {
        r->net = GEM_NET_NOMEM;
        geminiFailText(0, GEM_NET_NOMEM, NULL, NULL, q->cfg.key, q->tzS, r->fail, sizeof(r->fail));
        break;
      }
      s_bodyCap = n + 1;
    }
    geminiBuildRequest(ctx, q->nCtx, q->question, s_work->bg, s_work->sys, l.budget, s_body, s_bodyCap);
    const uint32_t ta = millis();
    size_t got = 0;
    int net = GEM_NET_OK;
    const int code = postOnce(l.model, n, &got, &net);
    r->lastMs = millis() - ta;
    r->lastCode = code;
    r->net = net;
    r->attempts++;
    r->bodyBytes = (uint32_t)got;
    progSet(NULL, NULL, 0, r->attempts);
    if (code <= 0 && s_cancel) {
      /* Ended by Cancel (the client stop()s and reads LOST): the user's word - never a timeout
       * for rule 6 to retry. */
      r->cancelled = true;
      break;
    }
    if (code <= 0 && !s_cancel && tileFetchCallRecent()) {
      /* The wait was ended by a call (postOnce's stopFn) - or one rang in as the request failed:
       * no answer is coming, and no retry is made under a call. */
      strlcpy(r->fail, "A call came in - ask again after it", sizeof(r->fail));
      break;
    }
    if (code > 0) {
      geminiParseReply(netWorkerSink(), got, s_work->text, sizeof(s_work->text), rep);
    } else {
      memset(rep, 0, sizeof(*rep));
      rep->retryDelayS = -1;
    }
    uint32_t delayMs = 0;
    const char* stage = NULL;
    const bool wasFallback = l.onFallback;
    /* `net` says why there was no HTTP answer: a timeout or a lost connection is rule 6's (once more,
     * then the fallback once - with the background found above, never a second lookup). */
    step = geminiLadderNext(&l, &s_m->session, &q->quota, code, net, rep, utcNow(), millis(), &delayMs, &stage);
    if (step == GEM_STEP_DONE) {
      /* The answer, and - when background went with the question - the "Wikipedia: A; B" line
       * under it (the pages that went; cut off again before it is ever sent back as context). */
      if (rep->kind == GEM_REPLY_TEXT &&
          geminiAnswerText(rep, s_work->text, s_work->bg[0] ? s_work->src : NULL, r->answer,
                           sizeof(r->answer)) > 0) {
        r->ok = true;
        /* Google's text is drawn, saved and sent back: the scrub every other text from Google gets
         * (the key never goes in a request's text - but a model can echo what it was sent). */
        geminiScrub(r->answer, sizeof(r->answer), q->cfg.key);
        /* Just the model under each answer: the daily-limit notice was already on the waiting
         * screen, and "Flash-Lite - daily Flash limit reached" took two rows under EVERY answer. */
        geminiModelLabel(l.model, r->label, sizeof(r->label));
      } else {
        geminiNoTextMessage(rep, r->fail, sizeof(r->fail));
      }
      break;
    }
    if (step == GEM_STEP_FAIL) {
      if (code <= 0 && net == GEM_NET_NOMEM) {
        lowMemText(r->fail, sizeof(r->fail));    // the heap bar (q.heapCheck) or the handshake's alloc
      } else {
        geminiFailText(code, net, rep, &l, q->cfg.key, q->tzS, r->fail, sizeof(r->fail));
      }
      break;
    }
    geminiModelLabel(l.model, label, GEM_LABEL_MAX);
    if (l.quotaSwitch && l.onFallback && !wasFallback) {
      geminiQuotaNotice(primaryLabel, label, l.primaryUntil, l.nowUtc, q->tzS, l.primaryRemainS, stageBuf, stageCap);
      stage = stageBuf;
    }
    /* A quota switch's notice (no wait) stays up through the fallback's request; a busy wait
     * counts down, then says it is asking again. */
    progSet(stage ? stage : "Asking Gemini...", label, delayMs ? millis() + delayMs : 0, -1);
    if (delayMs) {
      if (!waitSliced(delayMs)) {
        r->cancelled = true;
        break;
      }
      progSet("Asking again...", NULL, 0, -1);
    }
  }
  memset(rep, 0, sizeof(*rep));           // Google's message may quote what was sent
  free(rep);
  r->ms = millis() - t0;
}

/* The AI's job on the shared worker (https_worker.h): the question's ladder, then the key wiped -
 * exactly what the AI's own worker did per question before 0.9.81. */
static void aiJob() {
  aiRun();                                // returns, so its locals are destructed
  memset(s_req->cfg.key, 0, sizeof(s_req->cfg.key));
  s_res->stackFloor = netWorkerStackFloor();
  heapNow(NULL, NULL, &s_res->minEver);
  s_done = true;                          // before s_busy drops: aiRequestActive never blinks
  s_busy = false;                         // the worker goes on to a queued weather fetch, or sleeps
}

// ── the loop side ────────────────────────────────────────────────────────────────────────

bool aiRequestActive() {
  return s_busy || s_done;
}

void aiCancel() {
  if (s_busy) {
    s_cancel = true;
    progSet("Cancelling...", NULL, 0, -1);
  }
}

void aiProgress(AiProgress* out) {
  if (!s_m) {
    memset(out, 0, sizeof(*out));
    return;
  }
  portENTER_CRITICAL(&s_mux);
  *out = s_m->prog;
  portEXIT_CRITICAL(&s_mux);
  out->active = aiRequestActive();
  out->cancelled = s_cancel;
}

const char* aiPendingQuestion() {
  return (aiRequestActive() && s_req) ? s_req->question : NULL;
}

void aiLoopTick() {
  /* ⚠ Not while a game runs: the fold writes the card, and the game's blit task owns the SPI bus
   * the card shares with the screen (a loop-task LCD push during a game deadlocked both cores -
   * GUI.cpp). aiRequestActive() stays true meanwhile; it lands on the first pass after. */
  if (!s_done || gGbcActive) {
    return;
  }
  const bool cancelled = s_res->cancelled || s_cancel;
  const bool ok = s_res->ok && !cancelled;
  GeminiChat* c = s_clearAfter ? NULL : chatGet();
  if (c) {
    const char* a = cancelled ? "Cancelled" : (ok ? s_res->answer : s_res->fail);
    geminiChatFold(c, s_req->askedUtc, s_req->topic, !ok, s_res->label, s_req->question, a,
                   s_breakAfter);
    chatSave();
    s_chatGen++;
  }
  s_breakAfter = false;
  s_clearAfter = false;
  if (memcmp(&s_req->quotaIn, &s_req->quota, sizeof(s_req->quota)) != 0) {
    geminiQuotaMerge(&s_m->quota, &s_req->quota);
    quotaSave();
  }
  memset(s_req->cfg.key, 0, sizeof(s_req->cfg.key));
  if (ok) {
    s_answered++;
  } else {
    s_failed++;
  }
  s_haveRes = true;
  /* ⚠ Never the key, the question, its keywords or the answer: sizes, codes, the model, the time. */
  if (s_res->wikiWhy[0]) {
    log_e("AI: Wikipedia lookup skipped: %s", s_res->wikiWhy);   // one line; the question went without it
  }
  log_e("AI: %s in %u ms, %d request(s), HTTP %d, %s, question %u chars, answer %u chars, "
        "Wikipedia %d page(s) in %u ms, stack floor %u",
        cancelled ? "cancelled" : (ok ? "answered" : "failed"), (unsigned)s_res->ms, s_res->attempts,
        s_res->lastCode, s_res->label[0] ? s_res->label : "-", (unsigned)strlen(s_req->question),
        (unsigned)(ok ? strlen(s_res->answer) : 0), s_res->wikiPages, (unsigned)s_res->wikiMs,
        (unsigned)s_res->stackFloor);
  s_cancel = false;
  s_done = false;
}

bool aiAsk(const char* question, char* why, size_t whyCap) {
  if (!why || whyCap == 0) {
    return false;
  }
  why[0] = '\0';
  aiLoopTick();                           // a result still waiting goes into the chat first
  if (aiRequestActive()) {
    strlcpy(why, s_cancel ? "Still cancelling the last question" : "A question is already being asked",
            whyCap);
    return false;
  }
  if (!s_req) s_req = (AiReq*)heap_caps_calloc(1, sizeof(AiReq), MALLOC_CAP_SPIRAM);
  if (!s_res) s_res = (AiRes*)heap_caps_calloc(1, sizeof(AiRes), MALLOC_CAP_SPIRAM);
  if (!s_work) s_work = (AiWork*)heap_caps_calloc(1, sizeof(AiWork), MALLOC_CAP_SPIRAM);
  if (!s_req || !s_res || !s_work) {
    strlcpy(why, "No memory for the question", whyCap);
    return false;
  }
  /* The question, straight into the (idle) request - not on the loop's stack: printable ASCII
   * (the compose field's own rule), ends trimmed, 500 at most. */
  {
    const char* p = question ? question : "";
    while (*p == ' ') p++;
    const size_t len = strlen(p);
    if (len > GEM_Q_CAP) {
      strlcpy(why, "Too long - 500 characters at most", whyCap);
      return false;
    }
    char* q = s_req->question;
    size_t n = 0;
    for (; *p; p++) {
      const unsigned char ch = (unsigned char)*p;
      q[n++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : ' ';
    }
    while (n > 0 && q[n - 1] == ' ') n--;
    q[n] = '\0';
    if (n == 0) {
      strlcpy(why, "Type a question first", whyCap);
      return false;
    }
  }
  aiKeyReload();                          // a new or fixed key works without a reboot
  if (!s_m) {
    strlcpy(why, "No memory for the question", whyCap);
    return false;
  }
  if (!s_m->key.usable) {
    if (s_m->key.state == AI_KEY_BAD) {
      strlcpy(why, "The key file was refused - see Key info", whyCap);
    } else if (s_m->key.state == AI_KEY_UNREADABLE) {
      strlcpy(why, "Could not read the key file - try again", whyCap);
    } else {
      strlcpy(why, "No key - put gemini.txt in API Keys (see Key info)", whyCap);
    }
    return false;
  }
  {
    const char* gate = aiGateWhy();
    if (gate) {
      strlcpy(why, gate, whyCap);
      return false;
    }
  }
  /* The heap bar - unless a weather fetch holds the worker: its handshake is dipping the heap right
   * now, so the reading would refuse a question that would have been fine. Then the worker checks
   * the bar itself when the question's turn comes (heapCheckAtStart). */
  const bool behindWeather = wxRequestActive();
  if (!behindWeather && !netHeapOkForHandshake(NULL, NULL)) {
    lowMemText(why, whyCap);
    return false;
  }
  GeminiChat* chat = chatGet();
  if (!chat) {
    strlcpy(why, "No memory for the question", whyCap);
    return false;
  }
  if (!netWorkerReady(why, whyCap)) {
    return false;
  }
  if (!s_sessionInit) {
    geminiSessionInit(&s_m->session);
    s_sessionInit = true;
  }
  quotaLoad();

  // The request: everything the worker will read, copied now (the chat may change meanwhile).
  memcpy(&s_req->cfg, s_cfg, sizeof(s_req->cfg));
  s_req->askedUtc = utcNow();
  s_req->tzS = tzNow();
  {
    /* The zone's name for the date sentence - only when it is the offset the clock is on now (a
     * zone whose DST waits for a clock, or a typed offset, gives just "local time (UTC-7)"). */
    TzEff e;
    tzSvcEffective(&e);
    s_req->zone[0] = '\0';
    if (e.ok && e.name && e.minutes * 60 == s_req->tzS) {
      strlcpy(s_req->zone, e.name, sizeof(s_req->zone));
    }
  }
  s_req->topic = geminiChatTopicDue(chat, s_req->askedUtc);
  s_req->nCtx = 0;
  if (!s_req->topic) {
    int idx[GEM_CONTEXT];
    const int k = geminiChatContext(chat, idx);
    for (int i = 0; i < k; i++) {
      strlcpy(s_req->ctxQ[i], chat->ex[idx[i]].q, sizeof(s_req->ctxQ[i]));
      strlcpy(s_req->ctxA[i], chat->ex[idx[i]].a, sizeof(s_req->ctxA[i]));
    }
    s_req->nCtx = k;
  }
  s_req->quota = s_m->quota;
  s_req->quotaIn = s_m->quota;
  s_req->heapCheckAtStart = behindWeather;
  s_req->retryTimeouts = aiRetryTimeouts();       // NVS, read once: the setting as the question is asked
  memset(s_res, 0, sizeof(*s_res));
  s_haveRes = false;
  /* MUSIC PAUSES FOR A QUESTION ({user}, 2026-10-03: "make music turn off when doing ai"). The
   * TLS handshake is ~4.5 s of software ECC on this core, and Google resets one that takes over
   * ~10 s: with a track decoding beside it, MEASURED on phone 1, 2 of 2 questions failed that
   * way (`start_ssl_client: -80`), and even with the loop's 10-tick yield the first try still
   * failed. Paused, not stopped: the track and its place stay, and Music's Play carries on. */
  const bool pausedMusic = musicPlayerIsPlaying();
  if (pausedMusic) {
    musicPlayerPause();
  }
  s_req->musicPaused = pausedMusic;
  portENTER_CRITICAL(&s_mux);
  memset(&s_m->prog, 0, sizeof(s_m->prog));
  s_m->prog.startMs = millis();
  strlcpy(s_m->prog.stage, behindWeather ? "Waiting for the weather fetch..."
                           : (pausedMusic ? "Music paused - connecting..." : "Connecting..."),
          sizeof(s_m->prog.stage));
  s_m->prog.gen = 1;
  portEXIT_CRITICAL(&s_mux);
  tlsInstallPsramHook();                  // BEFORE the first WiFiClientSecure (tile_fetch.h)
  s_asked++;
  s_cancel = false;
  s_done = false;
  s_busy = true;
  netWorkerSubmit(NET_JOB_AI, aiJob);
  return true;
}

// ── the bench ────────────────────────────────────────────────────────────────────────────

void aiReport(void (*emit)(const char* line)) {
  char l[192];
  if (!memOk()) {
    emit("ai: no PSRAM for the AI's state");
    return;
  }
  if (!aiRequestActive()) {
    aiKeyReload();
  }
  snprintf(l, sizeof(l), "ai: %s", s_m->key.line);
  emit(l);
  snprintf(l, sizeof(l), "    file %s, roots built-in GTS R1+R4",
           s_m->key.path ? s_m->key.path : "(none: " AI_KEY_FILE " or " AI_KEY_FILE_ROOT ")");
  emit(l);
  const GeminiChat* c = aiRequestActive() ? s_chat : chatGet();
  if (c) {
    const int64_t now = utcNow();
    int idx[GEM_CONTEXT];
    const bool topic = geminiChatTopicDue(c, now);
    snprintf(l, sizeof(l), "    chat: %d exchange%s saved (%u bytes), the next question %s",
             c->n, c->n == 1 ? "" : "s", (unsigned)geminiChatSave(c, NULL, 0),
             topic ? "starts a new topic" : "");
    if (!topic) {
      const size_t at = strlen(l);
      snprintf(l + at, sizeof(l) - at, "sends %d earlier exchange(s)", geminiChatContext(c, idx));
    }
    emit(l);
  }
  quotaLoad();
  bool anyOut = false;
  for (int i = 0; i < s_m->quota.n; i++) {
    const int64_t until = geminiQuotaUntil(&s_m->quota, s_m->quota.e[i].model, utcNow(), millis());
    if (geminiQuotaOut(&s_m->quota, s_m->quota.e[i].model, utcNow(), millis())) {
      anyOut = true;
      if (until) {
        snprintf(l, sizeof(l), "    daily limit: %s used up for another %lld s%s", s_m->quota.e[i].model,
                 (long long)(until - utcNow()), s_m->quota.e[i].guess ? " (a guess: Google gave no time)" : "");
      } else {
        snprintf(l, sizeof(l), "    daily limit: %s used up until a restart (no trusted clock)",
                 s_m->quota.e[i].model);
      }
      emit(l);
    }
  }
  if (!anyOut) {
    emit("    daily limits: none reached");
  }
  emit(s_m->key.lookup ? "    Wikipedia lookup: on (keywords only; gemini.txt lookup=off stops it)"
                       : "    Wikipedia lookup: OFF (gemini.txt lookup=off)");
  emit(aiRetryTimeouts() ? "    " GEM_RETRY_LABEL ": on (once more, then the fallback once; `ai retry off`)"
                         : "    " GEM_RETRY_LABEL ": OFF (a timeout is shown at once; `ai retry on`)");
  if (aiRequestActive()) {
    AiProgress p;
    aiProgress(&p);
    snprintf(l, sizeof(l), "    NOW: %s (%s, %u s, request %d)%s", p.stage, p.label[0] ? p.label : "-",
             (unsigned)((millis() - p.startMs) / 1000), p.attempt, p.cancelled ? " - cancelling" : "");
    emit(l);
  } else if (s_haveRes && s_res) {
    snprintf(l, sizeof(l), "    last: %s, HTTP %d, %s, %u ms (last request %u ms), %d request(s), body %u bytes",
             s_res->cancelled ? "cancelled" : (s_res->ok ? "answered" : "FAILED"), s_res->lastCode,
             s_res->label[0] ? s_res->label : "-", (unsigned)s_res->ms, (unsigned)s_res->lastMs,
             s_res->attempts, (unsigned)s_res->bodyBytes);
    emit(l);
    if (!s_res->wikiAsked) {
      snprintf(l, sizeof(l), "    Wikipedia: not asked (%s)", s_res->wikiNot ? s_res->wikiNot : "-");
      emit(l);
    } else if (s_res->wikiPages > 0) {
      snprintf(l, sizeof(l), "    Wikipedia: %d page(s) went with it, %u ms", s_res->wikiPages,
               (unsigned)s_res->wikiMs);
      emit(l);
    } else {
      snprintf(l, sizeof(l), "    Wikipedia: skipped after %u ms - %s", (unsigned)s_res->wikiMs, s_res->wikiWhy);
      emit(l);
    }
    if (s_res->ok) {
      snprintf(l, sizeof(l), "    answer (%u chars): %.100s", (unsigned)strlen(s_res->answer), s_res->answer);
      for (char* p = l; *p; p++) {
        if (*p == '\n') *p = ' ';
      }
    } else {
      snprintf(l, sizeof(l), "    why: %.170s", s_res->fail);
    }
    emit(l);
    snprintf(l, sizeof(l), "    worker stack floor %u of %u | internal largest floor %u, min-ever %u",
             (unsigned)s_res->stackFloor, (unsigned)NET_STACK_BYTES, (unsigned)s_res->largestFloor,
             (unsigned)s_res->minEver);
    emit(l);
  } else {
    emit("    nothing asked since boot. ai ask <question>");
  }
  uint32_t f, lg, m;
  heapNow(&f, &lg, &m);
  snprintf(l, sizeof(l), "    internal now free %u largest %u min-ever %u | psram free %u | asked %u, answered %u, failed %u",
           (unsigned)f, (unsigned)lg, (unsigned)m, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)s_asked, (unsigned)s_answered, (unsigned)s_failed);
  emit(l);
}

static void emitWrapped(void (*emit)(const char*), const char* prefix, const char* text) {
  char l[192];
  const size_t room = 170;
  const char* p = text;
  bool first = true;
  while (*p || first) {
    size_t n = 0;
    while (p[n] && p[n] != '\n' && n < room) n++;
    snprintf(l, sizeof(l), "%s%.*s", first ? prefix : "    ", (int)n, p);
    emit(l);
    first = false;
    p += n;
    if (*p == '\n') p++;
  }
}

void aiReportLast(void (*emit)(const char* line)) {
  const GeminiChat* c = aiRequestActive() ? s_chat : chatGet();
  if (!c || c->n == 0) {
    emit("ai last: the chat is empty");
    return;
  }
  const GeminiExchange* e = &c->ex[c->n - 1];
  char l[96];
  snprintf(l, sizeof(l), "ai last: %s%s%s", (e->flags & GEM_EX_FAILED) ? "FAILED" : "answered by ",
           (e->flags & GEM_EX_FAILED) ? "" : e->label, (e->flags & GEM_EX_TOPIC) ? " (a new topic)" : "");
  emit(l);
  emitWrapped(emit, "  Q: ", e->q);
  emitWrapped(emit, "  A: ", e->a);
}
