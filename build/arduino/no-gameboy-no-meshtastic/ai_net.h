/*
 * ai_net.h - the device half of Menu > AI: the key file on the card, the saved chat, the
 * remembered daily limits, the gates, and the worker that talks to Google. The decisions are
 * gemini.h's (host-tested); the screens are app_ai.cpp's; the bench is serial `ai`.
 *
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * ONE QUESTION AT A TIME, ON A WORKER OF ITS OWN
 * ══════════════════════════════════════════════════════════════════════════════════════════
 * TLS cannot run on the loop task (tile_fetch.h, "WHY A TASK"): an 8 KB stack and mbedTLS's two
 * 16.7 KB record buffers. So, exactly like the map downloader:
 *   - ONE persistent worker ("aiworker"), its 8 KB INTERNAL stack carved on the first question and
 *     never freed, waiting on a semaphore. Never a per-run task (a static one races its own TCB on
 *     this FreeRTOS; a dynamic one fragmented `largest` 26 -> 11 KB on phone 1).
 *   - mbedTLS allocates from PSRAM through tile_fetch's hook (tlsInstallPsramHook), installed
 *     before the first WiFiClientSecure - an AI question may be the first TLS since boot.
 *   - Its OWN worker, not tile_fetch's: a map job holds that one for days. A question is refused
 *     while a download runs (two handshakes would dip the internal heap ~24 KB) - see aiAsk.
 *   - (0.9.81) That worker is now SHARED with the Almanac's weather (https_worker.h: job kinds on
 *     one stack, one request helper, netRequestActive() in the loop's hooks). The AI's job is what
 *     this worker did before - the ladder, the key wipe, the music pause, the gates; a question
 *     asked while a weather fetch holds the worker waits its turn ("Waiting for the weather
 *     fetch...") and the heap bar is checked by the worker when it starts, not by aiAsk.
 * Per question: a new WiFiClientSecure with Google's roots pinned (setCACert: GTS Root R1 + R4,
 * built in and the only ones - no override file), setConnectTimeout(10000) - which also caps the
 * handshake, 120 s by default - and up to 150 s for the answer (2026-10-11: HTTPClient's own
 * timeout is a uint16 and stops at 65.5 s, so https_worker's client waits for the first byte
 * itself, in AI_SLICE_MS slices that honour Cancel - and a call that rings in), the system
 * instruction built per request with today's date (a trusted clock only; gemini.h, "CURRENT
 * FACTS"), the key in an `x-goog-api-key` header, the chunked body de-chunked by hand into PSRAM
 * (never getString(), which grows on the internal heap), then http.end(), the block closes, stop(),
 * delete. The connection is closed after every answer.
 * Before the first request of a question (2026-10-11), unless gemini.txt says `lookup=off`: ONE
 * GET to en.wikipedia.org with the question's KEYWORDS (wiki_lookup.h - never the question, the key
 * or the chat), ISRG X1 + X2 + Root YE pinned, 20 s at most, never retried; what it finds about the
 * keywords goes in the question's own turn as labelled background, and the answer ends "Wikipedia:
 * A; B". Any failure is one log line and no background - the question is asked all the same. A
 * request that gets no answer in time is asked once more, then the fallback once (round 3, "Retry if
 * it times out", on by default: aiRetryTimeouts) - with the background already found, never a
 * second lookup.
 *
 * WHO OWNS WHAT. The question, its context and a copy of the key go into a module-static PSRAM
 * request on the LOOP (aiAsk) and belong to the worker until it finishes; the worker writes only
 * that request, its result and a small progress record published under a spinlock with a
 * generation counter. The loop folds a finished result into the chat and writes the card
 * (aiLoopTick, every pass - a flag test when idle). So leaving the app mid-question is safe: the
 * worker never touches an app, and the answer is in the chat when the app is opened again.
 *
 * THE CARD. gemini.txt is read from /API Keys/gemini.txt, then /gemini.txt (the T-Deck's place),
 * on every app open and before every question - so a new key works without a reboot - and only a
 * GOOD read is kept: a card that fails a read keeps the last good key rather than turning into
 * "no key". The chat is /ai/chat.txt and the remembered limits /ai/state.txt (never inside
 * /API Keys). Nothing here ever prints, logs or draws the key: `ai` says its length.
 */
#ifndef AI_NET_H
#define AI_NET_H

#include <stddef.h>
#include <stdint.h>
#include "gemini.h"

#define AI_KEY_DIR        "/API Keys"
#define AI_KEY_FILE       "/API Keys/gemini.txt"
#define AI_KEY_FILE_ROOT  "/gemini.txt"         // where the T-Deck firmware keeps it
#define AI_DIR            "/ai"
#define AI_CHAT_FILE      "/ai/chat.txt"
#define AI_CHAT_TMP       "/ai/chat.tmp"
#define AI_CHAT_BAD       "/ai/chat.bad"        // a chat.txt this firmware could not read, set aside
#define AI_STATE_FILE     "/ai/state.txt"
#define AI_STATE_TMP      "/ai/state.tmp"

// ── the key ──────────────────────────────────────────────────────────────────────────────
#define AI_KEY_LINE_MAX   256
enum {
  AI_KEY_NOT_READ = 0,
  AI_KEY_NONE,              // no gemini.txt, or no key in it
  AI_KEY_OK,
  AI_KEY_BAD,               // gemini.txt refused (geminiConfigLine says why)
  AI_KEY_UNREADABLE,        // the card would not give it up (the last good key, if any, is kept)
};
typedef struct {
  int         state;        // AI_KEY_*
  bool        usable;       // a key a question can use (OK, or a kept good one)
  const char* path;         // the file it came from, or NULL
  /* geminiConfigLine: the key's LENGTH, the model... never the key. 256 since review 2026-10-11
   * (two 63-character model names and the lookup note passed 200); a longer line is cut "...". */
  char        line[AI_KEY_LINE_MAX];
  char        model[GEM_MODEL_MAX];
  char        fallback[GEM_MODEL_MAX];
  int         thinking;
  bool        lookup;       // the Wikipedia lookup is on (gemini.txt `lookup=off`: false)
} AiKeyInfo;
/* LOOP TASK. Read gemini.txt now. */
void aiKeyReload();
void aiKeyInfo(AiKeyInfo* out);           // what the last read found (no card access)

/* LOOP TASK. "Daily free limit for Flash reached - using Flash-Lite until about 17:00" (the
 * deadline Google gave; "for about 4 h" without a trusted clock, "for now" without a deadline)
 * when the configured model is remembered as used up for today; 0 (and "") when it is not. For
 * Key info. */
size_t aiQuotaNote(char* out, size_t cap);

/* LOOP TASK. "Retry if it times out" (round 3, Menu > AI > Menu): when Gemini sends nothing in time
 * or the connection drops, ask once more, then the fallback once (gemini.h, the ladder's rule 6).
 * ON unless set off. Kept in NVS ("wpmesh" / "ai_retry", the namespace every other on/off setting of
 * this firmware uses - the Almanac's units, Books, Wi-Fi location), so it survives a card swap and a
 * flash; read once, on first use. aiAsk copies it into the question (a change applies to the NEXT
 * question). */
bool aiRetryTimeouts();
void aiSetRetryTimeouts(bool on);

// ── the chat (LOOP TASK) ─────────────────────────────────────────────────────────────────
const GeminiChat* aiChat();               // /ai/chat.txt is read on first use; NULL without PSRAM
uint32_t aiChatGen();                     // changes whenever the chat does
void aiChatNewTopic();                    // and saved
void aiChatClear();                       // the saved file goes too

// ── a question ───────────────────────────────────────────────────────────────────────────
/* LOOP TASK. Start one. False with `why` (one short sentence for the screen) when it cannot:
 * no key, no WiFi, a download / the uploader / a sync window / a call (or the minute after) /
 * a game / a Files folder job, the internal heap under the handshake's bar, one already asked. */
bool aiAsk(const char* question, char* why, size_t whyCap);
void aiCancel();                          // the answer is thrown away when it lands
/* Asked and not yet in the chat (queued behind a weather fetch included). WiPhone.ino puts it -
 * through netRequestActive() - in `busy` and `hardBusy`: the handshake runs at full speed (160 MHz
 * with WiFi on), never at the 80 MHz idle clock. */
bool aiRequestActive();
typedef struct {
  bool     active;
  bool     cancelled;
  uint32_t gen;             // bumped by every change below
  uint32_t startMs;         // when it was asked
  uint32_t waitUntilMs;     // a retry's wait ends then (0 = not waiting): the countdown
  int      attempt;         // requests made so far
  char     stage[128];      // "Asking Gemini...", "Google is busy - trying again..."
  char     label[GEM_LABEL_MAX];   // the model being asked ("Flash")
} AiProgress;
void aiProgress(AiProgress* out);
const char* aiPendingQuestion();          // LOOP TASK: the question in flight, or NULL

/* LOOP TASK, every pass: fold a finished question into the chat and save it (a flag test when
 * there is none). */
void aiLoopTick();

/* Serial `ai`: one line per emit, never the key. */
void aiReport(void (*emit)(const char* line));
/* Serial `ai last`: the newest exchange, the answer in lines under 180 characters. */
void aiReportLast(void (*emit)(const char* line));

#endif // AI_NET_H
