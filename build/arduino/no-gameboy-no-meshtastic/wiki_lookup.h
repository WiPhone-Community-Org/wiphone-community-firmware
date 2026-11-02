/*
 * wiki_lookup.h - the pure half of Menu > AI's Wikipedia lookup (2026-10-11): what the phone asks
 * en.wikipedia.org before each question, and what it makes of the answer - the keywords, the URL,
 * the response, the background that goes in the question's turn and the sources line under the
 * answer. The wire is ai_net.cpp's (aiWikiLookup: ONE GET on the shared HTTPS worker, pinned to
 * the ISRG roots). tests/test_wiki.cpp proves this half.
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header. Keep it that way.
 *
 * WHY THE PHONE LOOKS IT UP ITSELF (gemini.h, "CURRENT FACTS"): the free tier has nothing that
 * searches - Search grounding is paid, the url_context tool kept 3.8 Flash silent for 150 s and
 * lite fetched nothing - and the model's own knowledge is a year or more old. Wikipedia's API is
 * free, needs no key and answered in ~0.7 s (from the Mac). Measured live, 2026-10-11:
 *   - RAW QUESTIONS SEARCH BADLY, KEYWORDS WELL. "has the new tomb raider legacy of atlantis come
 *     out" -> "tomb raider legacy atlantis" -> Tomb Raider: Legacy of Atlantis ("an upcoming
 *     action-adventure game ..."); "who won the 2026 world cup" -> "won 2026 world cup" -> 2026
 *     FIFA World Cup ("... concluded on July 19 with Spain winning ..."). So the question is cut to
 *     its keywords (wikiKeywords) - which is also the privacy rule: ONLY the keywords go to
 *     Wikipedia, never the question, the key or the chat.
 *   - ONE GET A QUESTION, NEVER RETRIED: Wikipedia answered 429 after ~8 quick requests from one
 *     address with a generic User-Agent. The request carries a policy User-Agent (a name and a
 *     contact URL: WIKI_USER_AGENT), and any failure is SILENT and harmless - no background, one
 *     log line, and the question goes to Gemini as it would have anyway.
 *   - THE RESPONSE (4 real GETs, the URL wikiUrl builds): formatversion=2 gives
 *       {"batchcomplete":true,"continue":{..},"query":{"pages":[{"pageid":..,"ns":0,"title":"..",
 *        "index":2,"extract":"..","description":"..","descriptionsource":"local"},..]}}
 *     with the pages in NO order - "index" is the search rank, and the first page that came back
 *     was index 2 - "description" sometimes "", extracts with "\n" between paragraphs and up to
 *     ~1.3 KB, non-ASCII in them (an em dash, accents), and no "query" at all when nothing
 *     matched ({"batchcomplete":true}). Chunked one time, Content-Length the next (https_worker
 *     reads both).
 *   - THE CHAIN: en.wikipedia.org serves leaf (P-256) <- Let's Encrypt YE2 <- Root YE (cross-signed
 *     by X2: SHA-256 0F:C0:90:1C...C4:85:F0, byte-identical to beaconDB's) <- X2 (cross-signed by
 *     X1). The X1 + X2 + YE pins Wi-Fi location carries (isrg_roots.h) verify it - checked with
 *     openssl against exactly those PEMs, Root YE alone included - and TLS 1.2
 *     ECDHE-ECDSA-AES128-GCM-SHA256 on P-256 (what mbedTLS 2.16 offers first) is served.
 *
 * WHAT GEMINI GETS. The question's own turn (and only it: the chat keeps the plain question, so no
 * context turn ever carries background) becomes
 *     Background from Wikipedia, looked up just now (use it only if it is about this question):
 *     - <Title> (<description>): <the extract, cut to ~700 characters at a sentence end>
 *     - <Title>: <...>                                  (no description: no brackets)
 *
 *     Question: <the question>
 * - wikiBackground makes the first lines (at most WIKI_BG_MAX, the header included),
 * geminiBuildRequest the last two. The shown answer ends "Wikipedia: <Title>; <Title>" (plain
 * ASCII, at most WIKI_SRC_MAX), naming exactly the pages that went. Only pages ABOUT the keywords
 * go (round 3: a keyword in the title or description - Wikipedia's search also matches article
 * text, so a page that merely mentions them came back too), and with no page left, or no title the
 * line can show, nothing goes at all (wikiCompose).
 */
#ifndef WIKI_LOOKUP_H
#define WIKI_LOOKUP_H

#include <stddef.h>
#include <stdint.h>

#define WIKI_HOST          "en.wikipedia.org"
/* Wikimedia's User-Agent policy: a name/version and a way to reach the operator. */
#define WIKI_USER_AGENT    "NH-devices/1.0 (https://github.com/Nikguy321/wiphone-meshtastic) WiPhone"
#define WIKI_KW_MAX        8        // keywords searched at most
#define WIKI_WORD_MAX      40       // a longer "word" is no search term: skipped
#define WIKI_KW_CAP        (WIKI_KW_MAX * (WIKI_WORD_MAX + 1))   // the keywords, space-separated, NUL included
#define WIKI_URL_MAX       1024     // the request URL, NUL included (the longest keywords fit)
#define WIKI_PAGES         2        // gsrlimit: the top two pages
#define WIKI_TITLE_MAX     128      // a title as kept, NUL included
#define WIKI_DESC_MAX      128
#define WIKI_EXTRACT_MAX   1280     // an extract as read (cut again, at a sentence, below)
#define WIKI_EXTRACT_CUT   700      // ...as sent: at a sentence end at or before this many bytes
#define WIKI_EXTRACT_MIN   120      // a later page with less room than this is left out
#define WIKI_BG_MAX        1100     // the whole background, its first line included (the phone's buffers)
#define WIKI_SRC_MAX       160      // the sources line, "Wikipedia: " included (NUL not counted)
#define WIKI_RESPONSE_CAP  (24u * 1024u)   // the response read at most: 2 pages x 6 sentences is a few KB
#define WIKI_BG_HEAD       "Background from Wikipedia, looked up just now (use it only if it is about this question):\n"

/* The search words of `question`, THE SHARED RULE (COVEY's gemini.py and the X4 give the same
 * words for the same text - keep the three alike): FOLDED first with gemini.cpp's table
 * (geminiFoldNext - COVEY and the X4 port it, 2026-10-11: "–" -> "-", "ß" -> "ss", "ø" -> "o", "ł"
 * -> "l", "æ" -> "ae", "é" -> "e", "²" -> "^2", a character the table does not know dropped) and
 * lowercased; then every character that is not [a-z0-9'] a SEPARATOR - never deleted:
 * "navier–stokes" is "navier stokes", "(1959–2015)" "1959 2015" - so the words are runs of
 * [a-z0-9'] (digits kept: "2026", "3"); an apostrophe at either end of a word dropped, then ONE
 * contraction ending off - 's 'm 're 'll 've 'd ("what's" -> "what", "who's" -> "who", "i'm" ->
 * "i", "baldur's" -> "baldur"; 2026-10-11 round 3: the word is its stem from here on) - THEN a stem
 * over WIKI_WORD_MAX skipped, THEN the stopwords out (compared without apostrophes: "whats" typed
 * bare is one too), duplicates once, at most WIKI_KW_MAX.
 * A FOLLOW-UP: when the question has fewer than 2 of its own, the keywords of the NEWEST of
 * `earlier` that has 2 or more of ITS own go IN FRONT (those not already there, as many as leave
 * room for this question's own: the total is still WIKI_KW_MAX). `earlier` = the current topic's
 * answered questions, OLDEST first (geminiChatContext's order: failed ones are not in it), nEarlier
 * of them (0 / NULL when the question starts a topic). Walking back past a follow-up to the question
 * that named the subject: "tomb raider legacy of atlantis" -> "and when?" -> "how long is it" still
 * searches Tomb Raider. None with 2: nothing is borrowed. Space-separated into `out` (cap
 * WIKI_KW_CAP is always enough); returns how many. 0 (and "") = no lookup. */
int    wikiKeywords(const char* question, const char* const* earlier, int nEarlier, char* out, size_t cap);

/* The one GET: https://en.wikipedia.org/w/api.php?action=query&format=json&formatversion=2&
 * redirects=1&generator=search&gsrsearch=<keywords, percent-encoded>&gsrlimit=2&
 * prop=extracts%7Cdescription&exintro=1&explaintext=1&exsentences=6. Only the keywords vary.
 * Returns the length (NUL not counted) whatever `cap` is; `out` holds it only when it is < cap
 * (else ""). 0 and "" for no keywords. */
size_t wikiUrl(const char* keywords, char* out, size_t cap);

typedef struct {
  int  index;                       // the search rank ("index"); a page without one ranks last
  char title[WIKI_TITLE_MAX];
  char desc[WIKI_DESC_MAX];         // "" when Wikipedia has none
  char extract[WIKI_EXTRACT_MAX];   // as decoded (UTF-8); never empty in a kept page
} WikiPage;
typedef struct {
  int      n;                       // pages kept, best rank first (0..WIKI_PAGES)
  WikiPage page[WIKI_PAGES + 1];    // + the one being read
} WikiResult;
enum {
  WIKI_OK = 0,      // at least one page with an extract
  WIKI_NONE,        // a good answer with nothing usable: no "query" (nothing matched), or empty extracts
  WIKI_ERROR,       // the API's own {"error":{...}}
  WIKI_BAD,         // not JSON this reads, or cut short
  // Never from wikiParse - wikiCompose's reasons for no background (wikiSkipWhy words them):
  WIKI_OFF_TOPIC,   // no page's title or description has a keyword in it (wikiRelevant)
  WIKI_NO_TITLE,    // the sources line would name no page (every title non-ASCII)
};
/* Parse a whole response: query.pages[] (an array; an object keyed by page id - formatversion=1 -
 * too), ordered by "index" (they come back in no order), the best WIKI_PAGES kept; a page with an
 * empty (or all-blank: whitespace and control characters only) extract or title, or "missing" /
 * "invalid", is skipped. Returns WIKI_*; out->n is 0 unless WIKI_OK. */
int    wikiParse(const char* body, size_t len, WikiResult* out);

/* RELEVANCE (2026-10-11 round 3, the shared rule): keep only the pages whose title or description -
 * through THE SAME FOLD as the keywords (wikiKeywords: "Pokémon" -> "pokemon", "Øresund" ->
 * "oresund", "Straße" -> "strasse", "Navier–Stokes" -> "navier stokes"), every character that is
 * not [a-z0-9] a space - holds at least one of `keywords` (wikiKeywords' space-separated list) as
 * a WHOLE WORD; a keyword with an apostrophe in it ("o'neill") is matched the same way, as the
 * words it splits into ("o neill", in that order). "tomb raider legacy atlantis" keeps "Tomb
 * Raider: Catalyst" (raider) and drops "Lara Croft" (Wikipedia's search also matches the article
 * TEXT). In place, the order kept; returns how many are left (r->n). */
int    wikiRelevant(WikiResult* r, const char* keywords);

/* The background block: WIKI_BG_HEAD, then "- <Title> (<description>): <extract>" a page, one
 * line each ("\n" between them; whitespace in all three collapsed to single spaces), each extract
 * cut to WIKI_EXTRACT_CUT at the last sentence end (". ", "! ", "? " or its end) - or at a word,
 * with "...", when no sentence ends in the first third - and the whole at most `maxChars` bytes
 * (and < cap): a page left with less than WIKI_EXTRACT_MIN of room is left out, and so is every
 * page after it. UTF-8 kept (never cut inside a character). *used = the pages that went (the
 * first *used of r). Returns the length; 0 and "" when none fits. */
size_t wikiBackground(const WikiResult* r, size_t maxChars, char* out, size_t cap, int* used);

/* The sources line for the first `used` pages: "Wikipedia: Title A; Title B" - plain ASCII
 * (accents folded, other non-ASCII dropped, as the answer is), at most WIKI_SRC_MAX characters
 * (a title that would pass it cut with "..."; one with no room left out). 0 and "" when used <= 0. */
size_t wikiSources(const WikiResult* r, int used, char* out, size_t cap);

/* A parsed response (WIKI_OK) to what goes with the question - the ONE path the phone takes
 * (ai_net.cpp's aiWikiLookup): wikiRelevant (r is filtered in place), wikiBackground into `bg`
 * (at most maxChars), then wikiSources into `src` for exactly the pages that went. ALL OR NOTHING:
 * when no page is about the keywords (*why = WIKI_OFF_TOPIC), none fits (WIKI_OK: "no page fit"),
 * or the sources line would be empty (WIKI_NO_TITLE - never background the answer cannot name),
 * both are "" and 0 is returned. Else the pages that went (> 0), *why = WIKI_OK. */
int    wikiCompose(WikiResult* r, const char* keywords, size_t maxChars, char* bg, size_t bgCap, char* src,
                   size_t srcCap, int* why);

/* Why there is no background, for the log line ("AI: Wikipedia lookup skipped: <this>"): the
 * HTTP status ("HTTP 429"), else the network's failure (gemini.h's GEM_NET_*: "no DNS",
 * "timed out", ...), else - a 200 - the parse or wikiCompose (`parsed`, WIKI_*: "nothing found",
 * "no page was about the keywords", ...). */
const char* wikiSkipWhy(int httpCode, int net, int parsed, char* buf, size_t cap);

#endif // WIKI_LOOKUP_H
