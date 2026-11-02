/*
 * gemini.h - the pure half of the AI app (Menu > AI): everything about talking to Google's
 * Gemini API that can be decided without a network, a card or a screen, so the host suite
 * (tests/test_gemini.cpp) can prove it. The device half - the worker task, TLS, the card and
 * the gates - is ai_net.cpp; the screens are app_ai.cpp.
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header. Keep it that way.
 *
 * WHAT GOOGLE ACTUALLY DOES (measured from the Mac with a real key, 2026-10-03):
 *   - POST https://generativelanguage.googleapis.com/v1beta/models/<model>:generateContent with
 *     the key in an `x-goog-api-key` HEADER (never in the URL: a URL ends up in logs).
 *   - Every answer, 200 or error, is Transfer-Encoding: chunked with no Content-Length - hence
 *     geminiDechunkFeed below (tile_fetch's fetchOne refuses such a body outright).
 *   - gemini-flash-latest is a THINKING model. Its thoughts come out of maxOutputTokens: with a
 *     small cap it answered 200, finishReason MAX_TOKENS and NO text. So the phone asks for a
 *     BOUNDED think (thinkingBudget 1024, the owner's "some thinking") inside a 4096-token cap:
 *     ~6-12 s an answer, measured. gemini-flash-lite-latest REJECTS thinkingConfig with 400
 *     INVALID_ARGUMENT, so it is never sent one, and a model seen to reject it is remembered.
 *   - gemini-flash-latest answered 503 UNAVAILABLE on 2 of 4 calls minutes apart; a retry worked.
 *   - A FREE key gets 20 gemini-flash-latest answers a DAY (429 RESOURCE_EXHAUSTED, a QuotaFailure
 *     whose quotaId says PerDay, a RetryInfo retryDelay - on this key it ran to 00:00 UTC). Quotas are
 *     per MODEL: flash-lite still answered after that. Retrying cannot help a daily limit, so the
 *     ladder goes straight to the fallback and the phone REMEMBERS it until the delay is up.
 *
 * CURRENT FACTS (2026-10-11): asked about a game announced after its training data, the model called
 * it "rumoured, may come out in 2025" - a year already over (the owner, on COVEY). Two things fix
 * that:
 *   - the DATE: the system instruction starts with today's local date and time (geminiSystemText),
 *     built when the question is sent and only from a TRUSTED clock (NTP or GPS) - a wrong date is
 *     worse than none;
 *   - a WIKIPEDIA LOOKUP the phone makes ITSELF before each question (wiki_lookup.h): the
 *     question's keywords - never the question - searched on en.wikipedia.org, and the first
 *     sentences of the top two pages sent in the question's OWN turn, labelled as background
 *     (geminiBuildRequest's `background`; never the system instruction: it is data, not
 *     instructions). The shown answer ends with a "Wikipedia: A; B" line (GEM_SRC_SEP).
 *   What did NOT work, measured live the same day: Google Search grounding is not free on current
 *   models (429; the 2.5 models that had it answer 404 to new users); the url_context tool made
 *   gemini-flash-latest (3.8 Flash) send NOTHING - not even headers - for 150 s, twice, and lite
 *   fetched nothing unless the question held a URL, and made things up. So NO tool is sent, and
 *   none is ever NAMED: an instruction that mentioned a "URL tool" it did not declare came back
 *   after 84 s as finishReason MALFORMED_FUNCTION_CALL with no text (the ladder asks once more).
 *   Free-tier Flash answers in 30-90 s these days: the phone waits up to 150 s (ai_net.cpp), and -
 *   "Retry if it times out", on by default (round 3) - asks once more, then the fallback once, when
 *   nothing came (the ladder's rule 6).
 *
 * THE KEY FILE (gemini.txt, a superset of the T-Deck firmware's parser - its file works here
 * unchanged): `key=` and optional `model=` (names in any case, spaces around '=' allowed), '#'
 * comment lines, blank lines, LF or CRLF, a UTF-8 BOM, trailing whitespace trimmed, surrounding
 * quotes stripped, a lone bare key line accepted as the key, the last occurrence wins, `key=`
 * empty means no key. Lines only this phone and COVEY read: `thinking=<0..8192>` (0 = off),
 * `fallback=<model>` (or `none`) and `lookup=off` (no Wikipedia lookup; `on`, the default, too).
 * The canonical form is `key=<key>\n[model=<name>\n]`.
 * Real keys come in more than one shape (a newer style is 53 characters with a '.' in it, not
 * AIza.../39), so the SHAPE check ([A-Za-z0-9._-], 20-200) only ever WARNS. What is refused is
 * what cannot go into an HTTP header safely: a space, a control character or a non-ASCII byte.
 *
 * THE CHAT (decided 2026-10-03, the same rules as COVEY's): saved on the card (/ai/chat.txt, the
 * last GEM_CHAT_MAX exchanges or GEM_CHAT_FILE_MAX bytes, whichever is smaller), so it survives
 * a restart. What Google is SENT is only the current TOPIC: the last GEM_CONTEXT exchanges since
 * the last topic break. A topic breaks by itself when the previous exchange is more than two
 * hours old, and by hand ("New topic"). A failed question stays in the transcript, marked, and
 * is never sent as context.
 */
#ifndef GEMINI_H
#define GEMINI_H

#include <stddef.h>
#include <stdint.h>

#define GEM_KEY_MAX        208    // a key, NUL included (the shape check's 200, rounded)
#define GEM_MODEL_MAX      64
#define GEM_LABEL_MAX      64     // "Flash-Lite", and a note: what the transcript prints under an answer
#define GEM_Q_MAX          512    // a question, NUL included: the compose cap (500) and `ai ask`
#define GEM_Q_CAP          500    // ...what may be typed
#define GEM_A_MAX          4096   // a stored answer, NUL included (the WiPhone cap)
#define GEM_CONTEXT        4      // exchanges of the current topic sent with a question
#define GEM_SEND_TEXT_CAP  6144   // context + question text sent, before JSON escaping
#define GEM_MAX_TOKENS     4096   // maxOutputTokens: the thoughts AND the answer
#define GEM_THINKING       1024   // the primary model's thinkingBudget unless gemini.txt says
#define GEM_THINKING_MAX   8192
#define GEM_TEMPERATURE    "0.7"
#define GEM_CHAT_MAX       20     // exchanges kept (and saved)
#define GEM_CHAT_FILE_MAX  16384  // ...and the saved file's size cap
#define GEM_TOPIC_GAP_S    7200   // two hours since the last exchange: a new topic
#define GEM_SYS_MAX        1024   // the system instruction as geminiSystemText builds it, NUL included
/* The sources line under an answer that went with Wikipedia background: "Wikipedia: A; B"
 * (wiki_lookup.cpp builds it), after a blank line - and cut off again before the answer goes back
 * as context (the model would learn to end answers with one, naming pages it was never given). */
#define GEM_SRC_PREFIX     "Wikipedia: "
#define GEM_SRC_SEP        "\n\n" GEM_SRC_PREFIX
/* Between the background and the question in the question's own turn. */
#define GEM_BG_QUESTION    "\n\nQuestion: "
#define GEM_DEFAULT_MODEL  "gemini-flash-latest"
#define GEM_LITE_MODEL     "gemini-flash-lite-latest"
#define GEM_HOST           "generativelanguage.googleapis.com"
#define GEM_URL_PREFIX     "https://" GEM_HOST "/v1beta/models/"
#define GEM_URL_SUFFIX     ":generateContent"

// ── the key file ─────────────────────────────────────────────────────────────────────────
enum {
  GEM_CFG_OK = 0,
  GEM_CFG_NO_KEY,         // no key line, or `key=` empty
  GEM_CFG_BAD_KEY,        // a key with a space, a control character or a non-ASCII byte in it
};
enum {
  GEM_WARN_SHAPE    = 1,  // outside [A-Za-z0-9._-] or 20-200 characters: sent anyway
  GEM_WARN_MODEL    = 2,  // a model= that is not a model name: the default is used instead
  GEM_WARN_TDECK    = 4,  // 80+ characters: the T-Deck firmware keeps 79, so the card will not work there
  GEM_WARN_THINKING = 8,  // a thinking= that is not 0..8192: the default is used
  GEM_WARN_FALLBACK = 16, // a fallback= that is not a model name: the default is used
  GEM_WARN_LOOKUP   = 32, // a lookup= that is neither on nor off: the default (on) is used
};
typedef struct {
  char key[GEM_KEY_MAX];        // ⚠ the secret. Never printed, logged or drawn.
  int  keyLen;
  char model[GEM_MODEL_MAX];    // the configured model, or GEM_DEFAULT_MODEL
  bool modelSet;                // the file named one
  char fallback[GEM_MODEL_MAX]; // the busy/daily-limit fallback; "" = none (`fallback=none`)
  int  thinking;                // the primary's thinkingBudget, 0 = thinking off
  bool lookup;                  // the Wikipedia lookup before each question (`lookup=off`: false)
  int  warn;                    // GEM_WARN_* bits
  const char* problem;          // why GEM_CFG_BAD_KEY (a literal), else NULL
} GeminiConfig;
/* Parse a gemini.txt already in memory. Always fills `out` (defaults where the file is silent).
 * Returns GEM_CFG_*. `out->key` is empty unless GEM_CFG_OK. */
int  geminiParseConfig(const char* text, size_t len, GeminiConfig* out);
/* A model NAME: [a-z0-9][a-z0-9.-]*, 1..63 characters. Lowercase only, so a key put on the
 * model= line (keys are mixed case) is never taken for one - nor sent in the URL, drawn or
 * printed. model= and fallback= values that fail it, or that equal the key, are DROPPED by
 * geminiParseConfig (GEM_WARN_MODEL / GEM_WARN_FALLBACK, the default used). */
bool geminiModelNameOk(const char* m);
/* One line, NEVER the key: "key: set (53 chars), model gemini-flash-latest, thinking 1024,
 * fallback gemini-flash-lite-latest" (+ ", Wikipedia lookup off") and its warnings. Returns the
 * whole line's length; a line that does not fit `cap` is CUT with "..." (never left empty: review
 * 2026-10-11 - two long model names and the lookup note passed Key info's 200 bytes). */
size_t geminiConfigLine(const GeminiConfig* c, int parsed, char* out, size_t cap);
/* "gemini-flash-latest" -> "Flash", "gemini-flash-lite-latest" -> "Flash-Lite",
 * "gemini-2.5-flash" -> "2.5-Flash": what the transcript prints under an answer. */
void geminiModelLabel(const char* model, char* out, size_t cap);

// ── the chat ─────────────────────────────────────────────────────────────────────────────
enum {
  GEM_EX_FAILED = 1,      // no answer: `a` says why. Shown, never sent as context.
  GEM_EX_TOPIC  = 2,      // a topic starts here: the transcript draws a "New topic" line above it
};
typedef struct {
  int64_t t;                    // UTC seconds when it was asked; 0 = the clock was not known
  uint8_t flags;                // GEM_EX_*
  char    label[GEM_LABEL_MAX]; // the model that answered ("Flash"), and any note; "" when failed
  char    q[GEM_Q_MAX];
  char    a[GEM_A_MAX];         // the answer (clean ASCII), or why there is none
} GeminiExchange;
typedef struct {
  int            n;                     // 0..GEM_CHAT_MAX, oldest first
  bool           breakPending;          // "New topic" was asked for: the next exchange starts one
  GeminiExchange ex[GEM_CHAT_MAX];
} GeminiChat;
void geminiChatClear(GeminiChat* c);
/* Does a question asked at `now` (UTC s, 0 = unknown) start a new topic? Yes after "New topic",
 * for the first exchange, and when the last exchange is over GEM_TOPIC_GAP_S old (both times
 * known; with an unknown clock only "New topic" breaks one). */
bool geminiChatTopicDue(const GeminiChat* c, int64_t now);
/* Append one exchange (texts are cut to fit), clearing breakPending. Then the oldest go until
 * there are at most GEM_CHAT_MAX and the saved form fits GEM_CHAT_FILE_MAX (the newest always
 * stays). `topic` is what geminiChatTopicDue said when the question was ASKED. */
void geminiChatAdd(GeminiChat* c, int64_t t, bool topic, bool failed, const char* label,
                   const char* q, const char* a);
void geminiChatNewTopic(GeminiChat* c);  // breakPending, when there is anything to break from
/* A question that was IN FLIGHT lands: geminiChatAdd, then - when "New topic" was asked for
 * while it was out (`breakAfter`) - the break again, after it: it was asked in the old topic and
 * the user's break still stands (geminiChatAdd alone would wipe it). */
void geminiChatFold(GeminiChat* c, int64_t t, bool topic, bool failed, const char* label,
                    const char* q, const char* a, bool breakAfter);
/* The context for a question that does NOT start a topic: up to GEM_CONTEXT answered
 * exchanges since the last topic break, oldest first (failed ones skipped). Returns how many;
 * idx[] gets their indexes in c->ex. A question that starts a topic sends none. */
int  geminiChatContext(const GeminiChat* c, int idx[GEM_CONTEXT]);
/* The saved form (a versioned text file, the same length rule as geminiBuildRequest) and its
 * reader. A file that is not exactly that - a cut-off write, another version, junk - loads as
 * an EMPTY chat (false), never a half one. */
size_t geminiChatSave(const GeminiChat* c, char* out, size_t cap);
bool   geminiChatLoad(const char* text, size_t len, GeminiChat* out);

// ── the request ──────────────────────────────────────────────────────────────────────────
/* The system instruction for a 240x320 keypad phone: plain ASCII, no markdown, a few short
 * paragraphs at most unless asked. The fixed part; what is SENT is geminiSystemText's. */
extern const char GEM_SYSTEM_INSTRUCTION[];
/* The system instruction as sent, built for each request (2026-10-11):
 *   "Today is Sunday 11 October 2026, about 2:05 PM Pacific time (UTC-7). " - only when nowUtc is
 *     a TRUSTED clock's (the caller passes 0 otherwise: no date beats a wrong one); local by
 *     tzOffsetS; `zone` is the zone's name ("Pacific"; cut at a comma, "Mountain, no DST" ->
 *     "Mountain"; NULL, "", or a name that is itself an offset - a digit in it, or "UTC..."
 *     ("UTC+12") - is "local time"),
 *   then: the model's own knowledge ends earlier than today; background from Wikipedia that comes
 *   with a question is preferred for anything recent and ignored when it is about something else;
 *   what cannot be confirmed is said so, never a guess presented as a fact - and NO tool, URL or
 *   function is ever named (one that was not declared came back MALFORMED_FUNCTION_CALL),
 *   then GEM_SYSTEM_INSTRUCTION. Plain ASCII. Same length rule as geminiBuildRequest; the longest
 *   form fits GEM_SYS_MAX. */
size_t geminiSystemText(int64_t nowUtc, int tzOffsetS, const char* zone, char* out, size_t cap);
typedef struct {
  const char* q;
  const char* a;
} GeminiTurnPair;
/* The generateContent body: system_instruction, contents = the context as alternating
 * user/model turns (the newest that fit GEM_SEND_TEXT_CAP together with the question, oldest
 * dropped first; an answer's own last "Wikipedia: ..." line (GEM_SRC_SEP) is never sent back)
 * ending with the question's turn - `background` + GEM_BG_QUESTION + `question` when there is
 * background (wikiBackground: ONLY this turn ever carries it - the context turns are the chat's,
 * which keeps the plain question), else the question alone - then generationConfig
 * {maxOutputTokens GEM_MAX_TOKENS, temperature} plus - when thinkingBudget >= 0 - thinkingConfig
 * {thinkingBudget}. Never "tools". Returns the body's length (NUL not counted) whatever `cap` is;
 * `out` holds it only when length < cap (otherwise it is left EMPTY, never half a body). So:
 * n = build(NULL, 0); buf = alloc(n + 1); build(buf, n + 1). */
size_t geminiBuildRequest(const GeminiTurnPair* ctx, int nCtx, const char* question,
                          const char* background, const char* sysInstr, int thinkingBudget,
                          char* out, size_t cap);
/* JSON string body (no quotes): " and \ escaped, \n kept as \n, \t as \t, \r dropped, other
 * control characters as \u00XX, bytes >= 0x80 passed through. Same length rule as above. */
size_t geminiJsonEscape(const char* in, char* out, size_t cap);

// ── the answer ───────────────────────────────────────────────────────────────────────────
enum {
  GEM_REPLY_TEXT = 0,     // candidates[0] had text (finish may still be MAX_TOKENS)
  GEM_REPLY_EMPTY,        // a candidate with no text: `finish` says why
  GEM_REPLY_BLOCKED,      // promptFeedback.blockReason (no candidate)
  GEM_REPLY_ERROR,        // {"error":{code,message,status}}
  GEM_REPLY_BAD,          // not JSON we understand
};
typedef struct {
  int  kind;              // GEM_REPLY_*
  char finish[24];        // candidates[0].finishReason ("STOP", "MAX_TOKENS", "SAFETY"...)
  char block[32];         // promptFeedback.blockReason
  int  errCode;           // error.code
  char errStatus[32];     // error.status ("INVALID_ARGUMENT", "UNAVAILABLE"...)
  char errReason[32];     // the first error.details[].reason ("API_KEY_INVALID")
  char errMessage[200];   // error.message, decoded, UNSCRUBBED - pass it through geminiFailText
  bool quotaPerDay;       // a QuotaFailure violation whose quotaId names a PerDay limit
  int32_t retryDelayS;    // RetryInfo.retryDelay rounded up to seconds; -1 = none given
  bool textTruncated;     // the joined text did not fit `textCap`
} GeminiReply;
/* Parse a whole (de-chunked) body. Joins candidates[0].content.parts[*].text, skipping parts
 * with "thought": true, decoding every JSON escape (\uXXXX and surrogate pairs to UTF-8) into
 * `text` (raw UTF-8, NUL-terminated). Returns out->kind. */
int  geminiParseReply(const char* body, size_t len, char* text, size_t textCap, GeminiReply* out);
/* For the 240x320 screen and its ASCII fonts: NULs and control characters out (but \n; \t ->
 * space), markdown markers out (**, __, backticks, ``` fence lines, a leading '#'s or '>'), a
 * leading "* ", "- " or "+ " bullet -> "- ", curly quotes/dashes/ellipsis/NBSP/common accents
 * and symbols to ASCII and every other non-ASCII character dropped, runs of blank lines down
 * to one, line ends and both ends trimmed. Returns the length; *truncated when it did not fit. */
size_t geminiCleanText(const char* in, char* out, size_t cap, bool* truncated);
/* THE FOLD, one character at a time: the table geminiCleanText folds with, and THE SHARED FOLD the
 * Wikipedia keywords and relevance read through (wiki_lookup.cpp, 2026-10-11 - COVEY and the X4
 * port this same table). The next UTF-8 character of *s as ASCII, *s moved past it: an ASCII byte
 * is itself (in `one`; a control character too - the caller decides what it is), a character the
 * table knows its ASCII ("ß" -> "ss", "Ø" -> "O", "ł" -> "l", "æ" -> "ae", "–" -> "-", "²" -> "^2",
 * "™" -> "(TM)", a zero-width space ""), any other non-ASCII character "" (dropped: an emoji, a
 * combining accent), and so is a stray continuation byte or a cut sequence's lead byte. NULL at
 * the end (*s left on its NUL). */
const char* geminiFoldNext(const char** s, char one[2]);
/* The answer as it is shown and kept: the cleaned text, plus " (cut short)" when the model hit
 * MAX_TOKENS or the text did not fit, plus - when `sources` is a line (wikiSources: background
 * went with the question) - that line last, after a blank one (GEM_SRC_SEP; its room is kept, so
 * the cap cuts the answer, never the line). Empty when the reply has no text. */
size_t geminiAnswerText(const GeminiReply* r, const char* text, const char* sources, char* out,
                        size_t cap);
/* Why there is no answer, for a 200 without text: "Gemini declined to answer (SAFETY)" and its
 * like; MALFORMED_FUNCTION_CALL (no tool is ever sent - the model stumbled) is "an empty answer". */
size_t geminiNoTextMessage(const GeminiReply* r, char* out, size_t cap);

// ── which model, and what is used up for today ────────────────────────────────────────────
/* Models seen to reject thinkingConfig, remembered for the session. gemini-flash-lite-latest is
 * seeded: it was measured to (400 INVALID_ARGUMENT, 2026-10-03), so it is never sent one. */
typedef struct {
  int  n;
  char noThink[4][GEM_MODEL_MAX];
} GeminiSession;
void geminiSessionInit(GeminiSession* s);
bool geminiSessionNoThink(const GeminiSession* s, const char* model);
void geminiSessionAddNoThink(GeminiSession* s, const char* model);

/* "<model> is used up until <time>": a daily free limit, remembered so later questions go
 * straight to the fallback. A deadline is kept two ways: in UTC when the clock is known (and
 * only those are saved on the card, /ai/state.txt) and on the boot's millisecond clock (so an
 * unknown clock still remembers it until a restart). */
#define GEM_QUOTA_SLOTS 4
typedef struct {
  char     model[GEM_MODEL_MAX];
  int64_t  untilUtc;      // 0 = not known in UTC
  uint32_t setMs, durMs;  // durMs 0 = no boot-clock deadline (loaded from the card)
  bool     guess;         // Google gave no retryDelay: GEM_DAY_GUESS_S, and never shown as a time
} GeminiQuotaEntry;
typedef struct {
  int              n;
  GeminiQuotaEntry e[GEM_QUOTA_SLOTS];
} GeminiQuota;
void geminiQuotaClear(GeminiQuota* q);
bool geminiQuotaOut(const GeminiQuota* q, const char* model, int64_t nowUtc, uint32_t nowMs);
/* When `model` comes back, UTC (0 = not known / not out). */
int64_t geminiQuotaUntil(const GeminiQuota* q, const char* model, int64_t nowUtc, uint32_t nowMs);
/* What the screen may say about a used-up model: *untilUtc (a deadline Google gave, on a trusted
 * clock; else 0) and *remainS (the seconds left; 0 = a guess, no time to show). Returns
 * geminiQuotaOut. */
bool geminiQuotaWhen(const GeminiQuota* q, const char* model, int64_t nowUtc, uint32_t nowMs,
                     int64_t* untilUtc, uint32_t* remainS);
/* ...and when the FIRST of the two comes back, for "both used up" (0, 0 when either is a guess). */
void geminiQuotaBack(const GeminiQuota* q, const char* primary, const char* fallback,
                     int64_t nowUtc, uint32_t nowMs, int64_t* untilUtc, uint32_t* remainS);
/* `guess`: the delay is the phone's own (no RetryInfo came), not Google's. */
void geminiQuotaMark(GeminiQuota* q, const char* model, uint32_t delayS, bool guess,
                     int64_t nowUtc, uint32_t nowMs);
void geminiQuotaMerge(GeminiQuota* into, const GeminiQuota* from);   // the later deadline wins
/* The card form (expired and UTC-less entries left out) and its reader (junk -> empty, false). */
size_t geminiQuotaSave(const GeminiQuota* q, int64_t nowUtc, char* out, size_t cap);
bool   geminiQuotaLoad(const char* text, size_t len, GeminiQuota* out);
/* The deadline is the one GOOGLE gave (on this key a per-day RetryInfo pointed at 00:00 UTC, not
 * midnight Pacific), in the phone's HH:MM:
 *   a deadline + a trusted clock (untilUtc, nowUtc != 0):
 *     "Daily free limit for Flash reached - using Flash-Lite until about 17:00" (+ " tomorrow"
 *     when that is another local day)
 *   only a duration (remainS != 0):  "... using Flash-Lite for about 4 h"
 *   neither (a guess):               "... using Flash-Lite for now"
 * With no fallback: "... reached - try again after about 17:00" / "in about 4 h" / "later". */
size_t geminiQuotaNotice(const char* modelLabel, const char* fallbackLabel, int64_t untilUtc,
                         int64_t nowUtc, int tzOffsetS, uint32_t remainS, char* out, size_t cap);
/* "Today's free Gemini limit is used up - try again after about 17:00" / "... in about 4 h" /
 * "... later". */
size_t geminiQuotaAllOut(int64_t untilUtc, int64_t nowUtc, int tzOffsetS, uint32_t remainS,
                         char* out, size_t cap);

// ── the ladder (a loop on the worker, never recursion) ───────────────────────────────────
typedef struct {
  char primary[GEM_MODEL_MAX];  // the configured model
  char fallback[GEM_MODEL_MAX]; // "" = none
  int  thinking;                // the primary's thinkingBudget (0 = off, still SENT as 0)
  // the attempt to make next:
  char model[GEM_MODEL_MAX];
  int  budget;                  // thinkingBudget to send; -1 = no thinkingConfig at all
  bool onFallback;
  int  busyTries;               // 5xx (and a bare 429) retries spent on THIS model (2 each)
  bool minuteRetried;           // a per-minute 429 waited out once on THIS model
  bool droppedThinking;         // the 400 rule used (once)
  bool tried404;                // the 404 rule used (once)
  bool askedAgainEmpty;         // rule 0 used (once a question)
  bool allOut;               // FAIL because every model is used up for today
  bool quotaSwitch;             // on the fallback because the primary's daily limit is reached
  int64_t primaryUntil;         // ...until then (UTC, 0 = unknown)
  uint32_t primaryRemainS;      // ...or for this long (0 = a guess: "for now")
  int64_t backUntil;            // allOut: when the first model comes back (geminiQuotaBack)
  uint32_t backRemainS;
  int64_t nowUtc;               // the clock at geminiLadderStart (0 = not trusted)
  int  attempts;                // requests made for this question
  /* "Retry if it times out" (Menu > AI's menu, round 3): rule 6. geminiLadderStart sets it TRUE -
   * the setting's default - and the caller sets it from the setting after the start. */
  bool retryTimeouts;
  bool timeoutRetried;          // rule 6's same-model retry used (once a QUESTION)
  bool timeoutSwitch;           // ...and its move to the fallback (once a question)
  char note[GEM_LABEL_MAX + 24];  // a *stage the ladder words itself ("Asking Flash-Lite instead...")
} GeminiLadder;
enum { GEM_STEP_DONE = 0, GEM_STEP_RETRY, GEM_STEP_FAIL };
#define GEM_BUSY_RETRY_MS   2500u
#define GEM_LITE_DELAY_MS   500u
#define GEM_MINUTE_MAX_S    120     // a 429 retryDelay up to this is a per-MINUTE limit
#define GEM_DAY_GUESS_S     3600    // a PerDay 429 that gives no retryDelay is remembered this long
#define GEM_MAX_ATTEMPTS    10
/* Rule 6's words: the setting's row (app_ai.cpp adds ": On" / ": Off") and its waiting line. */
#define GEM_RETRY_LABEL     "Retry if it times out"
#define GEM_TIMEOUT_AGAIN   "No answer in time - trying again (2 of 2)..."
/* EVERY new question starts at the configured model (nothing is sticky across questions) -
 * except a model remembered as used up for today (`q`): then the fallback, and when that is out
 * too GEM_STEP_FAIL with allOut set (nothing is sent). Returns GEM_STEP_RETRY (go) or FAIL. */
int  geminiLadderStart(GeminiLadder* l, const GeminiConfig* cfg, const GeminiSession* s,
                       const GeminiQuota* q, int64_t nowUtc, uint32_t nowMs);
/* After an attempt answered `httpCode` (<= 0 = no HTTP answer: `net`, GEM_NET_*, says why - only
 * rule 6 retries one). 200 is DONE - but for rule 0. RETRY: *delayMs to wait, *stage what to show,
 * and `l` already holds the next attempt. The rules, in order:
 *   0. 200 with NO text and finishReason MALFORMED_FUNCTION_CALL (`r`'s kind EMPTY) -> once more,
 *      the same model, at once (it counts as an attempt); a second one is DONE - an empty answer
 *      (2026-10-11: 3.8 Flash answered that way after 84 s; no tool is sent, so it stumbled).
 *   1. 400 INVALID_ARGUMENT with thinkingConfig sent (and not a key complaint) -> once without
 *      it, and the model is remembered in `s` as one that rejects it.
 *   2. 404 on the configured model, when it is not gemini-flash-latest -> once with that.
 *   3. 429 naming a PerDay quota (or a retryDelay over GEM_MINUTE_MAX_S): no retry - the model
 *      is marked used up in `q` and the fallback asked at once (unless it is out too: allOut).
 *   4. 429 with a retryDelay up to GEM_MINUTE_MAX_S: wait it out once, then the fallback.
 *   5. Any 5xx (500-599: 503 UNAVAILABLE, 500 INTERNAL, 502, 504 DEADLINE_EXCEEDED - shared with
 *      COVEY and the X4, 2026-10-11), or a 429 that says neither: two retries 2.5 s apart, then the
 *      fallback (no thinkingConfig) with its own two.
 *   6. (round 3, "Retry if it times out", l->retryTimeouts) NO answer text because nothing came in
 *      time or the connection was lost - httpCode <= 0 with `net` GEM_NET_TIMEOUT or GEM_NET_LOST
 *      (the first-byte wait ran out, the body stalled or was cut; never a Cancel or a call: the
 *      caller ends the question on those first) -> once more on the SAME model at once
 *      (GEM_TIMEOUT_AGAIN); a second one -> the fallback once, at once ("Asking Flash-Lite
 *      instead...", l->note), unless the model already IS the fallback, there is none, or it is used up for
 *      today (rule 3's memory still holds); then FAIL - "Gemini did not answer in time". Both
 *      once a QUESTION, inside GEM_MAX_ATTEMPTS, and a timeout is never booked as a daily limit.
 *      Off: FAIL at once, as before. (COVEY streams: there, text that DID come is kept and marked
 *      cut short, never retried. A non-streamed answer here is whole or nothing.)
 * Everything else is FAIL (geminiFailText says what). */
int  geminiLadderNext(GeminiLadder* l, GeminiSession* s, GeminiQuota* q, int httpCode, int net,
                      const GeminiReply* r, int64_t nowUtc, uint32_t nowMs,
                      uint32_t* delayMs, const char** stage);

// ── what the screen says when there is no answer ──────────────────────────────────────────
enum {
  GEM_NET_OK = 0,         // an HTTP answer came back
  GEM_NET_DNS,            // the name did not resolve: no internet
  GEM_NET_CONNECT,        // TCP connect/handshake timed out or was refused
  GEM_NET_TLS,            // the certificate did not verify
  GEM_NET_TLS_OTHER,      // another TLS failure
  GEM_NET_NOMEM,          // the phone could not allocate the connection
  GEM_NET_TIMEOUT,        // no answer within the read timeout
  GEM_NET_LOST,           // the connection dropped mid-answer
  GEM_NET_TOO_BIG,        // the body passed the sink's cap
  GEM_NET_BAD_BODY,       // a malformed chunked body
};
/* One sentence for a failed question. Google's own error.message is kept (shortened, every
 * occurrence of `key` scrubbed, ASCII only) - never is every 400 "key rejected". `l` (may be
 * NULL) says when the day's free limit is the reason (geminiQuotaAllOut, in `tzOffsetS`). */
size_t geminiFailText(int httpCode, int net, const GeminiReply* r, const GeminiLadder* l,
                      const char* key, int tzOffsetS, char* out, size_t cap);
/* Replace every occurrence of `key` (8+ chars) in `s` with "[key]", in place. */
void geminiScrub(char* s, size_t cap, const char* key);
bool geminiLooksLikeKeyError(const GeminiReply* r);

// ── the chunked body ─────────────────────────────────────────────────────────────────────
/* Transfer-Encoding: chunked, decoded as it arrives (any split of the bytes gives the same
 * result) into the caller's buffer - PSRAM on the phone, so no 1460-byte internal malloc per
 * block the way HTTPClient::writeToStream does it. */
typedef struct {
  int      state;
  uint32_t remain;        // bytes left in the current chunk
  int      digits;        // hex digits read for the current size line
  bool     done;          // the zero chunk and its trailer are through
  bool     error;         // the stream is not chunked HTTP
  bool     overflow;      // body bytes beyond the cap were dropped (still decoded to the end)
} GeminiDechunk;
void geminiDechunkInit(GeminiDechunk* d);
/* Feed `n` raw bytes; body bytes are appended at out[*outLen] while they fit `cap` (no NUL is
 * written). Returns false once the stream is malformed. Bytes after `done` are ignored. */
bool geminiDechunkFeed(GeminiDechunk* d, const uint8_t* in, size_t n, char* out, size_t cap,
                       size_t* outLen);

#endif // GEMINI_H
