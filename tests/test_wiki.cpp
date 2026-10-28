/* test_wiki.cpp - the pure half of Menu > AI's Wikipedia lookup (WiPhone/wiki_lookup.cpp): the
 * question's keywords (the stopwords, digits, the previous question's for a follow-up), the one
 * URL, Wikipedia's response (in the shape measured with 4 real GETs on 2026-10-11 - the pages in
 * NO order, "index" the rank, a "" description, "\n" in extracts, no "query" when nothing
 * matched; the texts here are made up), the background block and its caps, the sources line, the
 * log line's reasons - and, with gemini.cpp, the round trip: the background in the question's
 * turn only, never in the history, the sources line shown and never sent back. Round 3 (the rules
 * COVEY and the X4 share): contractions to their stems before the stopword check, a follow-up
 * borrowing from the topic's NEWEST question with 2+ keywords of its own, and relevance - a page
 * kept only with a keyword in its title or description - with wikiCompose's all or nothing. Then
 * THE SHARED FOLD (2026-10-11): gemini.cpp's table before the keywords AND the relevance match
 * ("navier–stokes" -> "navier stokes", "gauß" -> "gauss", every other non-[a-z0-9'] a
 * separator, never deleted), the contraction off before the 40-letter limit, and the vectors all
 * three devices share (COVEY's round-5 table among them).
 *
 * WHY THIS IS WORTH A SUITE: every failure here is quiet on the phone. A keyword list that keeps
 * "the" and "is" searches for the wrong page and Gemini is handed a confident wrong background;
 * a parser that trusts the order shows the second-best page first; a background that grows past
 * its buffer is cut mid-character into the request; a sources line that went back as context
 * would teach the model to invent one. No network: fixtures only.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

#include "../WiPhone/wiki_lookup.h"
#include "../WiPhone/gemini.h"
#include "../WiPhone/json_read.h"

static int failures = 0;
static int checks = 0;

static void group(const char* name) {
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  } else {
    printf("  ok  %s\n", what);
  }
}

/* The keywords of `q`; `prev` = the topic's one earlier question (NULL: the question starts one). */
static std::string kw(const char* q, const char* prev = NULL, int* n = NULL) {
  char out[WIKI_KW_CAP];
  memset(out, 'Z', sizeof(out));
  const char* earlier[1] = { prev };
  const int k = wikiKeywords(q, prev ? earlier : NULL, prev ? 1 : 0, out, sizeof(out));
  if (n) *n = k;
  return out;
}

/* ...with the topic's earlier questions, OLDEST first (as geminiChatContext gives them). */
static std::string kwTopic(const char* q, std::initializer_list<const char*> earlier, int* n = NULL) {
  char out[WIKI_KW_CAP];
  memset(out, 'Z', sizeof(out));
  const char* e[8];
  int k = 0;
  for (const char* s : earlier) {
    if (k < 8) e[k++] = s;
  }
  const int got = wikiKeywords(q, e, k, out, sizeof(out));
  if (n) *n = got;
  return out;
}

/* Well-formed UTF-8, every sequence whole. */
static bool utf8Ok(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    size_t n = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 0)));
    if (n == 0 || i + n > s.size()) return false;
    for (size_t k = 1; k < n; k++) {
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
    }
    i += n;
  }
  return true;
}

static bool plainAscii(const char* s) {
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    if (*p < 0x20 || *p > 0x7E) return false;
  }
  return true;
}

static int count(const std::string& hay, const std::string& needle) {
  int k = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) k++;
  return k;
}

// ── fixtures: the measured shapes, the texts made up ──────────────────────────────────────

/* "tomb raider legacy atlantis" (GET 1, chunked): two pages, the WORSE rank first. */
static const char W_TOMB[] =
  "{\"batchcomplete\":true,\"continue\":{\"gsroffset\":2,\"continue\":\"gsroffset||\"},\"query\":{\"pages\":["
  "{\"pageid\":81823402,\"ns\":0,\"title\":\"Tomb Raider: Catalyst\",\"index\":2,\"extract\":\"Tomb Raider: "
  "Catalyst is an upcoming action-adventure game. It is set in northern India. It is planned for a 2028 "
  "release.\",\"description\":\"2028 video game\",\"descriptionsource\":\"local\"},"
  "{\"pageid\":81823403,\"ns\":0,\"title\":\"Tomb Raider: Legacy of Atlantis\",\"index\":1,\"extract\":"
  "\"Tomb Raider: Legacy of Atlantis is an upcoming action-adventure game from Crystal Dynamics. It remakes "
  "the first Tomb Raider game from 1996. Lara Croft searches for an artifact of lost Atlantis. It was "
  "announced at an awards show in 2025.\",\"description\":\"Upcoming video game\",\"descriptionsource\":"
  "\"local\"}]}}";

/* "won 2026 world cup" (GET 2, Content-Length): a "" description, "\n" between paragraphs, a raw
 * UTF-8 em dash. */
static const char W_CUP[] =
  "{\"batchcomplete\":true,\"continue\":{\"gsroffset\":2,\"continue\":\"gsroffset||\"},\"query\":{\"pages\":["
  "{\"pageid\":36581929,\"ns\":0,\"title\":\"2026 FIFA World Cup\",\"index\":1,\"extract\":\"The 2026 FIFA "
  "World Cup was a men's football tournament. It ran from June 11 to July 19, 2026 \xE2\x80\x94 in three "
  "countries.\\nIt was the first with 48 teams.\",\"description\":\"Football tournament\",\"descriptionsource\":"
  "\"local\"},{\"pageid\":81667005,\"ns\":0,\"title\":\"2026 FIFA World Cup knockout stage\",\"index\":2,"
  "\"extract\":\"The knockout stage was the second and final stage.\\n\\nIt ended with the final.\","
  "\"description\":\"\",\"descriptionsource\":\"local\"}]}}";

/* GET 3: nothing matched. */
static const char W_NONE[] = "{\"batchcomplete\":true}";



static int parse(const char* body, WikiResult* r) {
  return wikiParse(body, strlen(body), r);
}

// ── the keywords ─────────────────────────────────────────────────────────────────────────

static void testKeywords() {
  group("the keywords: what a question is searched for (measured: raw questions search badly)");
  int n = 0;
  ok(kw("has the new tomb raider legacy of atlantis come out", NULL, &n) == "tomb raider legacy atlantis" && n == 4,
     "\"has the new tomb raider legacy of atlantis come out\" -> \"tomb raider legacy atlantis\" (the live example)");
  ok(kw("who won the 2026 world cup") == "won 2026 world cup", "\"who won the 2026 world cup\" -> \"won 2026 world cup\" (digits kept)");
  ok(kw("how do I get the best ending in witcher 3") == "get best ending witcher 3",
     "\"how do I get the best ending in witcher 3\" -> \"get best ending witcher 3\" (a lone digit kept)");
  ok(kw("WHO WON THE 2026 WORLD CUP?") == "won 2026 world cup", "any case; punctuation is a separator");
  ok(kw("tomb-raider: legacy/atlantis!! (2026)") == "tomb raider legacy atlantis 2026", "hyphens, colons, slashes, brackets split words");
  ok(kw("What's Lara Croft's age? It's 'old'") == "lara croft age old",
     "apostrophes: \"what's\" and \"it's\" are stopwords by their stems, \"croft's\" is \"croft\", quotes off");
  ok(kw("whats new hows it going") == "going", "the owner's \"whats\" / \"hows\" (typed without the apostrophe)");
  ok(kw("is there any new latest one out yet, still?") == "one", "the \"is it out yet\" words all go");
  ok(kw("what is it", NULL, &n) == "" && n == 0, "only stopwords: none (no lookup)");
  ok(kw("", NULL, &n) == "" && n == 0 && kw(NULL, NULL, &n) == "" && n == 0, "empty / NULL: none");
  ok(kw("'' ' '''") == "", "apostrophes alone are no word");
  ok(kw("cup Cup world CUP world") == "cup world", "duplicates once (after lowercasing)");
  ok(kw("one two three four five six seven eight nine ten", NULL, &n) == "one two three four five six seven eight" &&
     n == WIKI_KW_MAX, "at most 8, the first 8");
  std::string w40(40, 'x'), w41(41, 'y');
  ok(kw((w41 + " zelda " + w40).c_str()) == "zelda " + w40, "a 41-letter \"word\" skipped, 40 kept");
  ok(kw("pok\xC3\xA9mon legends z-a") == "pokemon legends z", "accents folded (the shared table); \"z-a\" is z and a (a stopword)");
  ok(kw("I me my you your we our they their he she him her tell know please") == "", "the pronoun stopwords");
  ok(kw("a an the of in on at to for from by with about into over under and or but if then than so as") == "",
     "the article / preposition stopwords");
  ok(kw("can could should shall may might must will would do does did be been being") == "", "the auxiliaries");

  group("contractions (round 3, the shared rule): one ending off BEFORE the stopword check");
  ok(kw("what's new") == "" && kw("who's playing") == "playing" && kw("i'm here") == "",
     "\"what's\" -> what, \"who's\" -> who, \"i'm\" -> i: all stopwords now (no \"whos\" in the list)");
  ok(kw("baldur's gate 3", NULL, &n) == "baldur gate 3" && n == 3, "\"baldur's\" -> \"baldur\" (kept, as the stem)");
  ok(kw("they're we'll i've he'd you'd") == "", "'re 'll 've 'd: their stems are stopwords");
  ok(kw("WHAT'S BALDUR'S GATE") == "baldur gate", "any case (\"'S\")");
  ok(kw("tell me about baldur's") == "baldur" && kw("baldur's'") == "baldur",
     "the ending at a word's end, also after an end apostrophe was trimmed");
  ok(kw("don't stop") == "don't stop" && kw("rock'n'roll") == "rock'n'roll" && kw("o'neill's book") == "o'neill book",
     "'t is no contraction ending here; an apostrophe INSIDE a word stays; only the last ending goes");
  ok(kw("'s s's") == "s" && kw("x's") == "x", "something must come before the ending (\"'s\" alone is \"s\")");
  ok(kw("croft's croft") == "croft", "the stem counts as the word for duplicates");
  std::string w40s = std::string(40, 'k') + "'s";
  ok(kw(w40s.c_str()) == std::string(40, 'k'), "the length is checked on the stem: 40 letters + 's is kept");

  group("THE SHARED FOLD (2026-10-11): gemini.cpp's table, then every non-[a-z0-9'] a SEPARATOR");
  ok(kw("what is the navier\xE2\x80\x93stokes equation") == "navier stokes equation",
     "\"navier–stokes\": the en dash a separator, never deleted (not \"navierstokes\")");
  ok(kw("who is gau\xC3\x9F") == "gauss", "\"gauß\" -> \"gauss\" (sharp s: ss)");
  ok(kw("tell me about \xC5\x82\xC3\xB3" "d\xC5\xBA") == "lodz", "\"łódź\" -> \"lodz\" (l with a stroke, the accents)");
  ok(kw("what does e=mc\xC2\xB2 mean") == "e mc 2 mean", "\"e=mc²\" -> e, mc, 2 (the table's \"^2\": '^' a separator)");
  ok(kw("is pok\xC3\xA9mon legends z-a out") == "pokemon legends z", "\"is pokémon legends z-a out\" -> \"pokemon legends z\"");
  ok(kw("what's the israel\xE2\x80\x93hamas ceasefire status") == "israel hamas ceasefire status",
     "\"what's the israel–hamas ceasefire status\" -> \"israel hamas ceasefire status\"");
  ok(kw("navier\xE2\x80\x94stokes (1959\xE2\x80\x94" "2015)") == "navier stokes 1959 2015", "an em dash too: \"1959 2015\"");
  ok(kw("\xC3\x98rsted \xC3\xB8resund Stra\xC3\x9F" "e \xC3\x86sir \xC5\x92uvre") == "orsted oresund strasse aesir oeuvre",
     "Ø/ø -> o, ß -> ss, Æ -> ae, Œ -> oe (lowercased after the fold)");
  ok(kw("Baldur\xE2\x80\x99s Gate 3") == "baldur gate 3", "a curly apostrophe is an apostrophe: the contraction rule takes it");
  ok(kw("zel\xF0\x9F\x98\x80" "da na\xC2\xAD" "vier") == "zelda navier",
     "what the table drops (an emoji it does not know, a soft hyphen) is not there at all, as gFold drops it");
  ok(kw("tomb\x01raider\tlegacy") == "tomb raider legacy", "an ASCII control character is a separator too");
  ok(kw("gau\xC3 \x80x") == "gau x", "broken UTF-8: its bytes dropped, never a crash");
  std::string w39s = std::string(39, 'x') + "'s ok", w41s = std::string(41, 'x') + "'s ok";
  std::string w40ll = std::string(40, 'q') + "'ll", w41ll = std::string(41, 'q') + "'ll ok";
  ok(kw(w39s.c_str()) == std::string(39, 'x') + " ok" && kw(w41s.c_str()) == "ok",
     "THE SHARED ORDER: the ending off FIRST, then the 40 limit (39 + 's is kept, 41 + 's is not)");
  ok(kw(w40ll.c_str()) == std::string(40, 'q') && kw(w41ll.c_str()) == "ok", "...the longest ending too (40 + 'll kept, 41 not)");
  std::string sharp20;
  for (int i = 0; i < 20; i++) sharp20 += "\xC3\x9F";      // 20 x sharp s: 40 bytes, 40 letters folded
  ok(kw((sharp20 + " " + sharp20 + "\xC3\x9F").c_str()) == std::string(40, 's'),
     "the limit counts FOLDED letters (20 sharp s are 40, 21 are 42: skipped)");
  ok(kw("''\xE2\x80\x99'' x''''") == "x", "apostrophes alone (curly ones folded) are no word; trailing ones off");

  group("the shared vectors (COVEY's round-5 table, its tests/test_gemini.py: the same words on all three)");
  {
    struct Case { const char* q; std::initializer_list<const char*> earlier; const char* want; };
    const char* TR = "has the new tomb raider legacy of atlantis come out";
    const std::string a41 = std::string(41, 'a') + " real word";
    const Case CASES[] = {
      { TR, {}, "tomb raider legacy atlantis" },
      { "Who won the 2026 World Cup?", {}, "won 2026 world cup" },
      { "How do I get the best ending in Witcher 3?", {}, "get best ending witcher 3" },
      { "What's the Witcher's best ending?", {}, "witcher best ending" },
      { "when is it coming out?", { TR }, "tomb raider legacy atlantis" },
      { "and the price?", { TR }, "tomb raider legacy atlantis price" },
      { "is it out yet", {}, "" },
      { "is it out yet", { "" }, "" },
      { "one two three four five six seven eight nine ten", {}, "one two three four five six seven eight" },
      { "price?", { "alpha bravo charlie delta echo foxtrot golf hotel india" },
        "alpha bravo charlie delta echo foxtrot golf price" },
      { "price of tomb raider", { "the witcher" }, "price tomb raider" },
      { "Price PRICE price", {}, "price" },
      { "Pokemon Legends Z-A release date", {}, "pokemon legends z release date" },
      { "Rock 'n' roll hall of fame", {}, "rock n roll hall fame" },
      { "tell me about the moon", { "what is the sun" }, "moon" },
      { "and the moon?", { "and the moon?" }, "moon" },
      { a41.c_str(), {}, "real word" },
      { "it's 3 o'clock", {}, "3 o'clock" },
      { "don't stop believin' lyrics", {}, "don't stop believin lyrics" },
      { "   ", { "the witcher 3" }, "witcher 3" },
      { "Who's the CEO of Nintendo?", {}, "ceo nintendo" },
      { "I'm looking for Baldur's Gate 3 news", {}, "looking baldur gate 3 news" },
      { "what's it's they're we'll you've he'd i'm", {}, "" },
      { "O'Neill's 1990's films", {}, "o'neill 1990 films" },
      { "rock'n'roll's best", {}, "rock'n'roll best" },
      { "Baldur's baldur BALDUR'S", {}, "baldur" },
      { "don't can't won't", {}, "don't can't won't" },
      { "whats hows", {}, "" },
      { "and the sequel?", { "Tell me about Baldur's Gate 3", "when was it released?" }, "baldur gate 3 sequel" },
      { "price?", { "tomb raider", "witcher 3" }, "witcher 3 price" },
      { "price?", { "witcher 3", "skyrim" }, "witcher 3 price" },
      { "when?", { "Skyrim?" }, "" },
      { "the price?", { "Skyrim?", "what?" }, "price" },
      { "price of skyrim", { "witcher 3" }, "price skyrim" },
      { "and when?", {}, "" },
      { "Pok\xC3\xA9mon\xE2\x80\x99s newest L\xC3\xA9gendes", {}, "pokemon newest legendes" },
    };
    int bad = 0;
    for (const Case& c : CASES) {
      const std::string got = kwTopic(c.q, c.earlier);
      if (got != c.want) {
        bad++;
        printf("      \"%s\" -> \"%s\", want \"%s\"\n", c.q, got.c_str(), c.want);
      }
    }
    char what[96];
    snprintf(what, sizeof(what), "every shared case gives the shared keywords (%d cases)",
             (int)(sizeof(CASES) / sizeof(CASES[0])));
    ok(bad == 0, what);
  }

  group("a follow-up borrows the topic's previous question");
  const char* prev = "has the new tomb raider legacy of atlantis come out";
  ok(kw("and when?", prev, &n) == "tomb raider legacy atlantis" && n == 4,
     "\"and when?\" (no keywords): the previous question's");
  ok(kw("how long is it", prev, &n) == "tomb raider legacy atlantis long" && n == 5,
     "\"how long is it\" (1 keyword): the previous question's IN FRONT, then its own");
  ok(kw("and atlantis?", prev) == "tomb raider legacy atlantis", "...no word twice");
  ok(kw("is the witcher 3 good", prev) == "witcher 3 good", "2 or more of its own: the previous question is not used");
  ok(kw("and when?", NULL) == "" && kw("and when?", "") == "", "no previous question (a new topic): none");
  ok(kw("and when?", "what is it about") == "", "a previous question of stopwords: still none");
  ok(kw("price?", "one two three four five six seven eight nine", &n) == "one two three four five six seven price" &&
     n == WIKI_KW_MAX, "still 8 at most - the question's own word always kept");
  ok(kw("one two", "alpha beta") == "one two", "exactly 2: its own only");

  group("...the NEWEST earlier question that named something (2+ keywords of its own; round 3)");
  const char* tomb = "has the new tomb raider legacy of atlantis come out";
  ok(kwTopic("how long is it", { tomb, "and when?" }, &n) == "tomb raider legacy atlantis long" && n == 5,
     "a follow-up of a follow-up: past \"and when?\" (0 of its own) to the question that named the game");
  ok(kwTopic("price?", { tomb, "and the witcher 3?" }) == "witcher 3 price",
     "the newest with 2 or more wins (\"and the witcher 3?\" has 2), not the oldest");
  ok(kwTopic("price?", { tomb, "and zelda?" }) == "tomb raider legacy atlantis price",
     "one with ONE keyword of its own is walked past too");
  ok(kwTopic("and when?", { "zelda?", "price?" }, &n) == "" && n == 0,
     "none with 2: nothing borrowed (no lookup for \"and when?\")");
  ok(kwTopic("cost?", { "zelda?" }) == "cost", "...one keyword of its own still searched alone");
  ok(kwTopic("and when?", {}, &n) == "" && n == 0, "no earlier questions (a new topic): none");
  ok(kwTopic("is it out", { "what is it", tomb, "what is it" }) == "tomb raider legacy atlantis",
     "questions of stopwords are walked past");
  ok(kwTopic("the witcher 3 then", { tomb }) == "witcher 3",
     "2 of its own: nothing borrowed, however good the earlier question");
  {
    char out[WIKI_KW_CAP];
    const char* e[2] = { tomb, NULL };
    ok(wikiKeywords("and when?", e, 2, out, sizeof(out)) == 4 && !strcmp(out, "tomb raider legacy atlantis"),
       "a NULL in the list is no question (walked past)");
    ok(wikiKeywords("and when?", NULL, 3, out, sizeof(out)) == 0, "a NULL list: none borrowed, no crash");
  }

  group("the keyword buffer");
  char tiny[12];
  memset(tiny, 'Q', sizeof(tiny));
  n = wikiKeywords("tomb raider legacy atlantis", NULL, 0, tiny, sizeof(tiny));
  ok(!strcmp(tiny, "tomb raider") && n == 2, "a small buffer: whole words that fit, never half a word");
  std::string longest;
  for (int i = 0; i < 8; i++) longest += std::string(40, (char)('a' + i)) + " ";
  ok(kw(longest.c_str(), NULL, &n).size() == WIKI_KW_CAP - 1 && n == 8, "8 words of 40: exactly WIKI_KW_CAP");
}

// ── the URL ──────────────────────────────────────────────────────────────────────────────

static std::string url(const char* k) {
  const size_t n = wikiUrl(k, NULL, 0);
  std::string s(n + 1, '\0');
  const size_t n2 = wikiUrl(k, &s[0], n + 1);
  ok(n2 == n && strlen(s.c_str()) == n, "url(NULL,0) is the exact length");
  s.resize(n);
  return s;
}

static void testUrl() {
  group("the ONE request (only the keywords vary)");
  std::string u = url("tomb raider legacy atlantis");
  printf("      %s\n", u.c_str());
  ok(u == "https://en.wikipedia.org/w/api.php?action=query&format=json&formatversion=2&redirects=1"
          "&generator=search&gsrsearch=tomb%20raider%20legacy%20atlantis&gsrlimit=2"
          "&prop=extracts%7Cdescription&exintro=1&explaintext=1&exsentences=6",
     "exactly the URL measured live (formatversion=2, the top 2, intro extracts of 6 sentences)");
  ok(url("lara croft's age").find("gsrsearch=lara%20croft%27s%20age&") != std::string::npos,
     "an apostrophe is %27, a space %20");
  ok(url("a.b-c_d~e").find("gsrsearch=a.b-c_d~e&") != std::string::npos, "unreserved characters as they are");
  ok(url("x+y&z=1#f?").find("gsrsearch=x%2By%26z%3D1%23f%3F&") != std::string::npos,
     "anything else encoded: no keyword can add a parameter");
  ok(url("") == "" && wikiUrl(NULL, NULL, 0) == 0, "no keywords: no URL (no lookup)");
  bool safe = true;
  for (char c : url("tomb raider legacy atlantis")) {
    safe = safe && (isalnum((unsigned char)c) || strchr("-._~%&=?/:", c));
  }
  ok(safe, "every byte is URL-safe");
  char small[40];
  memset(small, 'Z', sizeof(small));
  ok(wikiUrl("tomb raider", small, sizeof(small)) > sizeof(small) && small[0] == '\0',
     "a buffer too small is left EMPTY, never half a URL");
  std::string longest;
  for (int i = 0; i < 8; i++) longest += std::string(i ? " " : "") + std::string(40, 'q');
  std::string longest2;
  for (int i = 0; i < 8; i++) {
    std::string w;
    for (int k = 0; k < 20; k++) w += "q'";
    longest2 += std::string(i ? " " : "") + w;
  }
  ok(wikiUrl(longest.c_str(), NULL, 0) < WIKI_URL_MAX && wikiUrl(longest2.c_str(), NULL, 0) < WIKI_URL_MAX,
     "the longest keywords (8 x 40, half of them apostrophes) fit WIKI_URL_MAX");

  group("privacy: only the keywords leave the phone");
  const char* q = "what is the date my friend said the new zelda comes out";
  char k[WIKI_KW_CAP];
  wikiKeywords(q, NULL, 0, k, sizeof(k));
  u = url(k);
  const std::string search = u.substr(u.find("gsrsearch=") + 10, u.find("&gsrlimit") - u.find("gsrsearch=") - 10);
  ok(search == "date%20friend%20said%20zelda%20comes", "the search is the keywords and nothing else");
  ok(u.find("what") == std::string::npos && u.find("%20the%20") == std::string::npos && u.find("my") == std::string::npos,
     "...none of the question's other words");
  ok(u.find("key") == std::string::npos && u.find("gemini") == std::string::npos, "...and no key, no Gemini anything");
}

// ── the response ─────────────────────────────────────────────────────────────────────────

static void testParse() {
  group("Wikipedia's answer (the shapes measured live; the texts made up)");
  WikiResult r;
  ok(parse(W_TOMB, &r) == WIKI_OK && r.n == 2, "two pages");
  ok(!strcmp(r.page[0].title, "Tomb Raider: Legacy of Atlantis") && r.page[0].index == 1 &&
     !strcmp(r.page[1].title, "Tomb Raider: Catalyst") && r.page[1].index == 2,
     "ordered by \"index\" (they came back rank 2 first), not by position");
  ok(!strcmp(r.page[0].desc, "Upcoming video game") && strstr(r.page[0].extract, "remakes the first Tomb Raider"),
     "title, description, extract");
  ok(parse(W_CUP, &r) == WIKI_OK && r.n == 2 && r.page[1].desc[0] == '\0', "a \"\" description is kept as \"\"");
  ok(strstr(r.page[0].extract, "2026 \xE2\x80\x94 in three countries.\nIt was") != NULL,
     "\"\\n\" decoded, raw UTF-8 kept");
  ok(parse(W_NONE, &r) == WIKI_NONE && r.n == 0, "{\"batchcomplete\":true} (nothing matched): NONE");
  ok(parse("{\"query\":{\"pages\":[]}}", &r) == WIKI_NONE && r.n == 0, "no pages: NONE");

  group("pages that do not count");
  ok(parse("{\"query\":{\"pages\":[{\"title\":\"A\",\"index\":1,\"extract\":\"\"},"
           "{\"title\":\"B\",\"index\":2,\"extract\":\" \\n\\t \"},{\"title\":\"C\",\"index\":3,\"extract\":\"Text.\"}]}}",
           &r) == WIKI_OK && r.n == 1 && !strcmp(r.page[0].title, "C"),
     "an empty extract and an all-blank one skipped: the third page is the one");
  ok(parse("{\"query\":{\"pages\":[{\"title\":\"A\",\"index\":1,\"extract\":\"\\u0001\\u0002 \"},"
           "{\"title\":\"\\u0003\",\"index\":2,\"extract\":\"x.\"},{\"title\":\"C\",\"index\":3,\"extract\":\"Text.\"}]}}",
           &r) == WIKI_OK && r.n == 1 && !strcmp(r.page[0].title, "C"),
     "an extract or title of control characters only is blank too (it collapses to nothing)");
  ok(parse("{\"query\":{\"pages\":[{\"title\":\"A\",\"index\":1,\"extract\":\"\"},{\"title\":\"B\",\"index\":2}]}}",
           &r) == WIKI_NONE && r.n == 0, "every extract empty or missing: NONE");
  ok(parse("{\"query\":{\"pages\":[{\"title\":\"Gone\",\"missing\":true,\"index\":1,\"extract\":\"x\"},"
           "{\"index\":2,\"extract\":\"No title.\"},{\"title\":\"Ok\",\"index\":3,\"extract\":\"Yes.\"}]}}", &r) == WIKI_OK &&
     r.n == 1 && !strcmp(r.page[0].title, "Ok"), "\"missing\" and a page with no title skipped");
  ok(parse("{\"query\":{\"pages\":[{\"title\":\"NoRank\",\"extract\":\"a.\"},{\"title\":\"Two\",\"index\":2,\"extract\":\"b.\"},"
           "{\"title\":\"Three\",\"index\":3,\"extract\":\"c.\"},{\"title\":\"One\",\"index\":1,\"extract\":\"d.\"}]}}", &r) == WIKI_OK &&
     r.n == 2 && !strcmp(r.page[0].title, "One") && !strcmp(r.page[1].title, "Two"),
     "four pages (more than asked): the best two by rank; one with no rank last");
  ok(parse("{\"query\":{\"pages\":[{\"title\":\"Tie A\",\"index\":1,\"extract\":\"a.\"},"
           "{\"title\":\"Tie B\",\"index\":1,\"extract\":\"b.\"}]}}", &r) == WIKI_OK &&
     !strcmp(r.page[0].title, "Tie A"), "a tie keeps the order they came in");
  ok(parse("{\"query\":{\"pages\":{\"81823403\":{\"title\":\"Second\",\"index\":2,\"extract\":\"x.\"},"
           "\"99\":{\"title\":\"First\",\"index\":1,\"extract\":\"y.\"}}}}", &r) == WIKI_OK && r.n == 2 &&
     !strcmp(r.page[0].title, "First"), "formatversion=1's object of pages read too");
  ok(parse("{\"query\":{\"redirects\":[{\"from\":\"a\",\"to\":\"b\"}],\"pages\":[{\"title\":\"P\\u00e9\",\"index\":1,"
           "\"extract\":\"x.\",\"extra\":{\"deep\":[1,{\"x\":\"]\"}]}}]},\"warnings\":{\"main\":{\"*\":\"w\"}}}", &r) == WIKI_OK &&
     !strcmp(r.page[0].title, "P\xC3\xA9"), "redirects, warnings and unknown nested keys skipped; \\u escapes decoded");

  group("answers that are not pages");
  ok(parse("{\"error\":{\"code\":\"badvalue\",\"info\":\"Unrecognized value\"},\"servedby\":\"mw\"}", &r) == WIKI_ERROR &&
     r.n == 0, "the API's own error: ERROR");
  ok(parse("{\"error\":{\"code\":\"x\"},\"query\":{\"pages\":[{\"title\":\"A\",\"index\":1,\"extract\":\"a.\"}]}}", &r) ==
     WIKI_ERROR && r.n == 0, "...even with pages beside it: none used");
  std::string cut(W_TOMB);
  cut.resize(cut.size() - 30);
  ok(wikiParse(cut.c_str(), cut.size(), &r) == WIKI_BAD && r.n == 0, "cut short (the 24 KB cap, a dropped socket): BAD, no page");
  ok(parse("<!DOCTYPE html><html>Too many requests</html>", &r) == WIKI_BAD && parse("", &r) == WIKI_BAD &&
     wikiParse(NULL, 0, &r) == WIKI_BAD && parse("[]", &r) == WIKI_BAD, "HTML (a 429's body), empty, NULL, not an object: BAD");
  ok(parse("{\"query\":{\"pages\":[{\"title\":\"A\",\"extract\":\"a\\q\"}]}}", &r) == WIKI_BAD, "a bad escape: BAD");

  group("sizes");
  std::string longX(3000, 'e');
  std::string body = std::string("{\"query\":{\"pages\":[{\"title\":\"") + std::string(300, 'T') +
                     "\",\"index\":1,\"description\":\"" + std::string(300, 'D') + "\",\"extract\":\"" + longX + "\"}]}}";
  ok(parse(body.c_str(), &r) == WIKI_OK && strlen(r.page[0].extract) == WIKI_EXTRACT_MAX - 1 &&
     strlen(r.page[0].title) == WIKI_TITLE_MAX - 1 && strlen(r.page[0].desc) == WIKI_DESC_MAX - 1,
     "a long extract, title and description: cut to their buffers");
  std::string accents;
  for (int i = 0; i < 1000; i++) accents += "\xC3\xA9";
  body = std::string("{\"query\":{\"pages\":[{\"title\":\"A\",\"index\":1,\"extract\":\"") + accents + "\"}]}}";
  ok(parse(body.c_str(), &r) == WIKI_OK && utf8Ok(r.page[0].extract), "...never inside a character");
  printf("      sizeof(WikiResult) = %u bytes (PSRAM, the worker's)\n", (unsigned)sizeof(WikiResult));
}

static WikiResult pages(const char* t1, const char* d1, const std::string& x1, const char* t2 = NULL,
                        const char* d2 = "", const std::string& x2 = "") {
  WikiResult r;
  memset(&r, 0, sizeof(r));
  r.n = 1;
  snprintf(r.page[0].title, sizeof(r.page[0].title), "%s", t1);
  snprintf(r.page[0].desc, sizeof(r.page[0].desc), "%s", d1);
  snprintf(r.page[0].extract, sizeof(r.page[0].extract), "%s", x1.c_str());
  if (t2) {
    r.n = 2;
    snprintf(r.page[1].title, sizeof(r.page[1].title), "%s", t2);
    snprintf(r.page[1].desc, sizeof(r.page[1].desc), "%s", d2);
    snprintf(r.page[1].extract, sizeof(r.page[1].extract), "%s", x2.c_str());
  }
  return r;
}

// ── relevance ────────────────────────────────────────────────────────────────────────────

/* "lara croft tomb raider" (made up, the measured shape): the BEST-ranked page is about something
 * else - Wikipedia's search matches article text, and this one only mentions the game. */
static const char W_OFFTOPIC[] =
  "{\"batchcomplete\":true,\"query\":{\"pages\":["
  "{\"pageid\":1,\"ns\":0,\"title\":\"Crystal Dynamics\",\"index\":1,\"extract\":\"Crystal Dynamics is an "
  "American video game developer. It made the Tomb Raider reboot.\",\"description\":\"American video game "
  "developer\"},"
  "{\"pageid\":2,\"ns\":0,\"title\":\"Tomb Raider: Catalyst\",\"index\":2,\"extract\":\"Tomb Raider: Catalyst is "
  "an upcoming game.\",\"description\":\"2028 video game\"}]}}";

static int relevant(WikiResult* r, const char* keywords) {
  return wikiRelevant(r, keywords);
}

static void testRelevance() {
  group("relevance: a keyword in the title or description, as a whole word (round 3, the shared rule)");
  WikiResult r;
  parse(W_TOMB, &r);
  ok(relevant(&r, "tomb raider legacy atlantis") == 2 && r.n == 2, "both Tomb Raider pages are about the keywords");
  parse(W_OFFTOPIC, &r);
  ok(relevant(&r, "tomb raider legacy atlantis") == 1 && r.n == 1 && !strcmp(r.page[0].title, "Tomb Raider: Catalyst"),
     "\"Crystal Dynamics\" (only its EXTRACT says Tomb Raider) dropped; the one left moves up");
  parse(W_OFFTOPIC, &r);
  ok(relevant(&r, "american") == 1 && !strcmp(r.page[0].title, "Crystal Dynamics"),
     "a keyword in the DESCRIPTION alone keeps a page");
  parse(W_OFFTOPIC, &r);
  ok(relevant(&r, "zelda breath wild") == 0 && r.n == 0, "no page about any keyword: none left");
  parse(W_OFFTOPIC, &r);
  ok(relevant(&r, "video 2028") == 2 && !strcmp(r.page[0].title, "Crystal Dynamics"), "both kept: the rank order stays");
  WikiResult p = pages("Tomb Raiders", "", "x.");
  ok(relevant(&p, "raider") == 0, "a whole word: \"raider\" is not in \"Tomb Raiders\"");
  p = pages("Cupboard", "furniture", "x.");
  ok(relevant(&p, "cup") == 0, "...nor \"cup\" in \"Cupboard\"");
  p = pages("2023 in film", "", "x.");
  ok(relevant(&p, "3") == 0, "...nor \"3\" in \"2023\"");
  p = pages("The Witcher 3: Wild Hunt", "2015 video game", "x.");
  ok(relevant(&p, "witcher 3") == 1 && (p = pages("The Witcher 3: Wild Hunt", "", "x."), relevant(&p, "3")) == 1,
     "\"3\" in \"Witcher 3:\": punctuation is a space");
  p = pages("Pok\xC3\xA9mon Legends: Z-A", "", "x.");
  ok(relevant(&p, "pokemon") == 1 && (p = pages("Pok\xC3\xA9mon Legends: Z-A", "", "x."), relevant(&p, "z")) == 1,
     "accents folded (\"Pokemon\"), \"Z-A\" is the words z and a");
  p = pages("Baldur's Gate 3", "2023 video game", "x.");
  ok(relevant(&p, "baldur") == 1, "\"Baldur's\" holds the word \"baldur\" (with the contraction rule, the match)");
  p = pages("Tip O'Neill", "American politician", "x.");
  ok(relevant(&p, "o'neill") == 1, "a keyword with an apostrophe: its words in order (\" o neill \")");
  p = pages("Oneill", "", "x.");
  ok(relevant(&p, "o'neill") == 0, "...not run together");
  p = pages("TOMB RAIDER", "", "x.");
  ok(relevant(&p, "raider") == 1, "any case");
  p = pages("Tomb Raider", "", "x.");
  ok(relevant(&p, "") == 0 && (p = pages("Tomb Raider", "", "x."), relevant(&p, NULL)) == 0 && relevant(NULL, "x") == 0,
     "no keywords (or no result): nothing is about them");
  p = pages("\xE6\x97\xA5\xE6\x9C\xAC", "\xE5\x9B\xBD", "x.");
  ok(relevant(&p, "japan") == 0, "a title and description with no ASCII: nothing to match");

  group("relevance through THE SHARED FOLD (2026-10-11: the same vectors on all three devices)");
  const struct { const char* kw; const char* title; const char* desc; int kept; const char* what; } REL[] = {
    { "navier stokes equation", "Navier\xE2\x80\x93Stokes equations", "", 1,
      "[navier stokes equation] vs \"Navier–Stokes equations\": kept (the en dash a space)" },
    { "israel hamas ceasefire", "Israel\xE2\x80\x93Hamas war", "", 1, "[israel hamas ceasefire] vs \"Israel–Hamas war\": kept" },
    { "oresund", "\xC3\x98resund", "", 1, "[oresund] vs \"Øresund\": kept" },
    { "strasse", "Stra\xC3\x9F" "e", "", 1, "[strasse] vs \"Straße\": kept" },
    { "orsted", "\xC3\x98rsted (company)", "", 1, "[orsted] vs \"Ørsted (company)\": kept" },
    { "gate", "Watergate scandal", "", 0, "[gate] vs \"Watergate scandal\": dropped (a whole word only)" },
    { "3", "1930s", "", 0, "[3] vs \"1930s\": dropped" },
    // COVEY's round-5 relevance cases (its tests/test_gemini.py), the same answers here:
    { "baldur sequel", "Baldur's Gate 3", "2023 video game", 1, "[baldur sequel] vs \"Baldur's Gate 3\": kept" },
    { "developer", "Larian Studios", "Belgian video game developer", 1, "[developer]: in the description: kept" },
    { "gate", "Watergate scandal", "Political scandal", 0, "[gate] vs \"Watergate scandal\" (Political scandal): dropped" },
    { "3", "Gate 3", "", 1, "[3] vs \"Gate 3\": kept" },
    { "3", "The 1930s", "Decade", 0, "[3] vs \"The 1930s\" (Decade): dropped" },
    { "beyonce tour", "Beyonc\xC3\xA9", "American singer", 1, "[beyonce tour] vs \"Beyoncé\": kept" },
    { "o'neal", "Shaquille O'Neal", "", 1, "[o'neal] vs \"Shaquille O'Neal\": kept" },
    { "nasa", "NASA", "", 1, "[nasa] vs \"NASA\": kept" },
    { "witcher 3", "Tomb Raider", "Video game franchise", 0, "[witcher 3] vs \"Tomb Raider\": dropped" },
    // ...and what the fold makes of the rest of a title:
    { "deg", "50\xC2\xB0 north", "", 1, "the table's spelling counts: \"50°\" holds the word \"deg\"" },
    { "zelda", "Zel\xF0\x9F\x98\x80" "da", "", 1, "an emoji the table drops: \"Zel(emoji)da\" is \"zelda\"" },
    { "tomb", "Tomb\x01Raider", "", 1, "a control character in a title: a space" },
  };
  for (const auto& c : REL) {
    p = pages(c.title, c.desc, "x.");
    ok(relevant(&p, c.kw) == c.kept, c.what);
  }
  {
    // The fold's growth never passes the buffers: a 126-byte title of what grows most.
    std::string deg, tm;
    for (int i = 0; i < 63; i++) deg += "\xC2\xB0";        // " deg" each: 252 folded
    for (int i = 0; i < 42; i++) tm += "\xE2\x84\xA2";     // "(TM)" each: 168 folded
    p = pages(deg.c_str(), tm.c_str(), "x.");
    ok(relevant(&p, "deg") == 1 && (p = pages(tm.c_str(), deg.c_str(), "x."), relevant(&p, "tm")) == 1,
       "a title and description that fold to twice their bytes: matched, inside the buffers (ASan)");
  }
}

// ── the background ───────────────────────────────────────────────────────────────────────

static std::string bg(const WikiResult* r, size_t maxChars, int* used = NULL, size_t cap = 4096) {
  std::string s(cap, '\0');
  int u = -1;
  const size_t n = wikiBackground(r, maxChars, &s[0], cap, &u);
  if (used) *used = u;
  ok(n == strlen(s.c_str()), "the length returned is the length written");
  s.resize(n);
  return s;
}

/* n numbered sentences of ~40 bytes: "Sentence 07 is in the extract here. " */
static std::string sentences(int n) {
  std::string s;
  char b[64];
  for (int i = 1; i <= n; i++) {
    snprintf(b, sizeof(b), "Sentence %02d is in the extract here. ", i);
    s += b;
  }
  s.resize(s.size() - 1);
  return s;
}

static void testBackground() {
  group("the background block (in the question's turn; WIKI_BG_MAX on the phone)");
  WikiResult r;
  parse(W_TOMB, &r);
  int used = 0;
  std::string b = bg(&r, WIKI_BG_MAX, &used);
  printf("      ---\n%s\n      ---\n", b.c_str());
  ok(b == "Background from Wikipedia, looked up just now (use it only if it is about this question):\n"
          "- Tomb Raider: Legacy of Atlantis (Upcoming video game): Tomb Raider: Legacy of Atlantis is an upcoming "
          "action-adventure game from Crystal Dynamics. It remakes the first Tomb Raider game from 1996. Lara Croft "
          "searches for an artifact of lost Atlantis. It was announced at an awards show in 2025.\n"
          "- Tomb Raider: Catalyst (2028 video game): Tomb Raider: Catalyst is an upcoming action-adventure game. It "
          "is set in northern India. It is planned for a 2028 release.",
     "the labelled header, then \"- Title (description): extract\" a page, best rank first");
  ok(used == 2, "...both pages used");
  ok(b.find("Question") == std::string::npos && b.find('\r') == std::string::npos, "no \"Question:\" (geminiBuildRequest adds it)");
  parse(W_CUP, &r);
  b = bg(&r, WIKI_BG_MAX, &used);
  ok(b.find("\n- 2026 FIFA World Cup knockout stage: The knockout stage was the second and final stage. It ended "
            "with the final.") != std::string::npos, "a \"\" description: no brackets; \"\\n\\n\" collapsed to a space");
  ok(b.find("2026 \xE2\x80\x94 in three countries. It was the first with 48 teams.") != std::string::npos && utf8Ok(b),
     "UTF-8 kept for Gemini (the em dash), one line a page");
  ok(count(b, "\n") == 2, "the header line and one line per page");

  group("an extract cut at a sentence end (~700)");
  std::string s20 = sentences(20);            // ~740 bytes
  r = pages("Long", "", s20);
  b = bg(&r, WIKI_BG_MAX, &used);
  const std::string x = b.substr(b.find("- Long: ") + 8);
  ok(x.size() <= WIKI_EXTRACT_CUT && x[x.size() - 1] == '.' && s20.compare(0, x.size(), x) == 0,
     "over 700: cut after the last whole sentence that fits");
  printf("      %u bytes: ...%s\n", (unsigned)x.size(), x.substr(x.size() - 40).c_str());
  ok(x.size() > WIKI_EXTRACT_CUT - 45, "...the last one that fits, not an earlier one");
  r = pages("Short", "d", sentences(3));
  ok(bg(&r, WIKI_BG_MAX).find(sentences(3)) != std::string::npos, "a short extract: whole");
  std::string oneLong;
  for (int i = 0; i < 160; i++) oneLong += "word ";
  oneLong += "end.";
  r = pages("Run-on", "", oneLong);
  b = bg(&r, WIKI_BG_MAX);
  const std::string y = b.substr(b.find("- Run-on: ") + 10);
  ok(y.size() <= WIKI_EXTRACT_CUT && y.substr(y.size() - 3) == "..." && y[y.size() - 4] != ' ' &&
     y.compare(0, y.size() - 3, oneLong.substr(0, y.size() - 3)) == 0,
     "no sentence end in reach: cut at a word, \"...\"");
  r = pages("Early", "", std::string("A b. ") + std::string(900, 'x'));
  b = bg(&r, WIKI_BG_MAX);
  ok(b.find("- Early: A b. x") != std::string::npos && b.substr(b.size() - 3) == "...",
     "a sentence end only in the first third: not used (too little kept) - cut later, \"...\"");
  r = pages("U.S. style", "", "It is 3.5 m tall. The U.S. version came later. " + sentences(20));
  b = bg(&r, WIKI_BG_MAX);
  ok(b.find("3.5 m tall") != std::string::npos && b[b.size() - 1] == '.', "a \".\" inside a number is no sentence end");
  std::string utf;
  for (int i = 0; i < 400; i++) utf += "\xE2\x80\x94";   // 1200 bytes of em dashes, no space
  r = pages("Dashes", "", utf);
  b = bg(&r, WIKI_BG_MAX);
  ok(utf8Ok(b) && b.substr(b.size() - 3) == "...", "no space at all: cut at a character, never inside one");

  group("the whole background at most WIKI_BG_MAX");
  r = pages("First", "one", sentences(25), "Second", "two", sentences(25));
  b = bg(&r, WIKI_BG_MAX, &used);
  ok(b.size() <= WIKI_BG_MAX && used == 2, "two long pages: both, within 1100");
  printf("      %u of %u bytes, %d page(s)\n", (unsigned)b.size(), (unsigned)WIKI_BG_MAX, used);
  ok(b.find("- Second (two): ") != std::string::npos && b[b.size() - 1] == '.',
     "...the second cut shorter, still at a sentence end");
  b = bg(&r, 900, &used);
  ok(b.size() <= 900 && used == 1 && b.find("Second") == std::string::npos,
     "less room: a second page with under WIKI_EXTRACT_MIN left is LEFT OUT, not squeezed");
  ok(bg(&r, 150, &used) == "" && used == 0, "room for the header but not a page: no background at all");
  ok(bg(&r, 50, &used) == "" && used == 0, "not even the header: none");
  b = bg(&r, WIKI_BG_MAX, &used, 600);
  ok(b.size() <= 599 && used == 1, "a buffer smaller than maxChars bounds it too");
  memset(&r, 0, sizeof(r));
  ok(bg(&r, WIKI_BG_MAX, &used) == "" && used == 0 && wikiBackground(NULL, 100, NULL, 0, NULL) == 0,
     "no pages / nothing: \"\"");
  r = pages("T\nitle\twith\x01 controls", "desc\r\nhere", "Extract\x02 text.");
  b = bg(&r, WIKI_BG_MAX);
  ok(b.find("- T itle with controls (desc here): Extract text.") != std::string::npos,
     "control characters in a title, description or extract: one space");
}

// ── the sources line ─────────────────────────────────────────────────────────────────────

static std::string src(const WikiResult* r, int used, size_t cap = 512) {
  std::string s(cap, '\0');
  const size_t n = wikiSources(r, used, &s[0], cap);
  s.resize(n);
  return s;
}

static void testSources() {
  group("the sources line under the answer (plain ASCII, WIKI_SRC_MAX)");
  WikiResult r;
  parse(W_TOMB, &r);
  ok(src(&r, 2) == "Wikipedia: Tomb Raider: Legacy of Atlantis; Tomb Raider: Catalyst", "\"Wikipedia: A; B\", best first");
  ok(src(&r, 1) == "Wikipedia: Tomb Raider: Legacy of Atlantis", "only the pages that went (used = 1)");
  ok(src(&r, 0) == "" && src(NULL, 2) == "", "none used: \"\"");
  ok(src(&r, 2).find(GEM_SRC_PREFIX) == 0, "starts with GEM_SRC_PREFIX (what the context strip looks for)");
  r = pages("Pok\xC3\xA9mon Legends: Z-A", "", "x.", "Caf\xC3\xA9 \xF0\x9F\x98\x80 Society; Inc", "", "y.");
  std::string s = src(&r, 2);
  ok(s == "Wikipedia: Pokemon Legends: Z-A; Cafe  Society  Inc" && plainAscii(s.c_str()),
     "accents folded, an emoji dropped, a ';' in a title made a space");
  r = pages(std::string(120, 'A').c_str(), "", "x.", std::string(120, 'B').c_str(), "", "y.");
  s = src(&r, 2);
  ok(s.size() == WIKI_SRC_MAX && s.substr(s.size() - 3) == "..." && s.find("; BBB") != std::string::npos,
     "two long titles: the second cut with \"...\" at WIKI_SRC_MAX");
  r = pages(std::string(127, 'A').c_str(), "", "x.", std::string(50, 'B').c_str(), "", "y.");
  s = src(&r, 2);
  ok(s.size() <= WIKI_SRC_MAX && s.find("; B") != std::string::npos, "...the second still named when it can be");
  r = pages(std::string(127, 'A').c_str(), "", "x.");
  memcpy(r.page[0].title + 100, "\xC3\xA9", 2);
  s = src(&r, 1, 40);
  ok(s.size() <= 39 && s.substr(s.size() - 3) == "..." && plainAscii(s.c_str()), "a small buffer: cut with \"...\"");
}

// ── what goes with the question (all or nothing) ─────────────────────────────────────────

static void testCompose() {
  group("wikiCompose: the relevant pages, their background, the line naming exactly them");
  WikiResult r;
  char b[WIKI_BG_MAX + 1], s[WIKI_SRC_MAX + 1];
  int why = -1;
  parse(W_TOMB, &r);
  ok(wikiCompose(&r, "tomb raider legacy atlantis", WIKI_BG_MAX, b, sizeof(b), s, sizeof(s), &why) == 2 &&
     why == WIKI_OK && !strcmp(s, "Wikipedia: Tomb Raider: Legacy of Atlantis; Tomb Raider: Catalyst") &&
     strstr(b, "- Tomb Raider: Catalyst (2028 video game): "), "two relevant pages: both go, both named");
  parse(W_OFFTOPIC, &r);
  ok(wikiCompose(&r, "tomb raider legacy atlantis", WIKI_BG_MAX, b, sizeof(b), s, sizeof(s), &why) == 1 &&
     !strcmp(s, "Wikipedia: Tomb Raider: Catalyst") && !strstr(b, "Crystal") && strstr(b, "- Tomb Raider: Catalyst"),
     "an off-topic page is neither SENT nor NAMED (the line names the pages that went, all relevant)");
  parse(W_OFFTOPIC, &r);
  memset(b, 'Z', sizeof(b));
  memset(s, 'Z', sizeof(s));
  ok(wikiCompose(&r, "zelda breath wild", WIKI_BG_MAX, b, sizeof(b), s, sizeof(s), &why) == 0 &&
     why == WIKI_OFF_TOPIC && b[0] == '\0' && s[0] == '\0', "no page about the keywords: no background, no line");
  r = pages("\xE6\x97\xA5\xE6\x9C\xAC", "japan", "Text.", "\xE5\x9B\xBD", "japan", "More.");
  ok(wikiCompose(&r, "japan", WIKI_BG_MAX, b, sizeof(b), s, sizeof(s), &why) == 0 && why == WIKI_NO_TITLE &&
     b[0] == '\0' && s[0] == '\0', "relevant, but every title non-ASCII (an empty line): NO background either");
  r = pages("\xE6\x97\xA5\xE6\x9C\xAC", "japan", "Text.", "Japan", "country", "More.");
  ok(wikiCompose(&r, "japan", WIKI_BG_MAX, b, sizeof(b), s, sizeof(s), &why) == 2 && !strcmp(s, "Wikipedia: Japan"),
     "...one title it can show: the line names that one (both went)");
  parse(W_TOMB, &r);
  ok(wikiCompose(&r, "tomb raider", 150, b, sizeof(b), s, sizeof(s), &why) == 0 && why == WIKI_OK && b[0] == '\0' &&
     s[0] == '\0', "no room for a page: none (\"no page fit\")");
  ok(wikiCompose(NULL, "x", WIKI_BG_MAX, b, sizeof(b), s, sizeof(s), &why) == 0 && why == WIKI_OFF_TOPIC,
     "no result: none");
}

// ── the log line ─────────────────────────────────────────────────────────────────────────

static void testSkipWhy() {
  group("why there was no background (the one log line)");
  char b[64];
  ok(!strcmp(wikiSkipWhy(429, GEM_NET_OK, WIKI_BAD, b, sizeof(b)), "HTTP 429 (too many requests)"), "429");
  ok(!strcmp(wikiSkipWhy(503, GEM_NET_OK, WIKI_BAD, b, sizeof(b)), "HTTP 503") &&
     !strcmp(wikiSkipWhy(301, GEM_NET_OK, WIKI_BAD, b, sizeof(b)), "HTTP 301"), "any other status (a redirect is never followed)");
  ok(!strcmp(wikiSkipWhy(0, GEM_NET_TIMEOUT, WIKI_BAD, b, sizeof(b)), "timed out") &&
     !strcmp(wikiSkipWhy(0, GEM_NET_DNS, WIKI_BAD, b, sizeof(b)), "no DNS") &&
     !strcmp(wikiSkipWhy(0, GEM_NET_TLS, WIKI_BAD, b, sizeof(b)), "the certificate did not verify") &&
     !strcmp(wikiSkipWhy(0, GEM_NET_TOO_BIG, WIKI_BAD, b, sizeof(b)), "the answer was over 24 KB") &&
     !strcmp(wikiSkipWhy(0, GEM_NET_NOMEM, WIKI_BAD, b, sizeof(b)), "low on memory"),
     "the network's failures in words");
  ok(!strcmp(wikiSkipWhy(200, GEM_NET_OK, WIKI_NONE, b, sizeof(b)), "nothing found") &&
     !strcmp(wikiSkipWhy(200, GEM_NET_OK, WIKI_BAD, b, sizeof(b)), "not JSON it reads") &&
     !strcmp(wikiSkipWhy(200, GEM_NET_OK, WIKI_ERROR, b, sizeof(b)), "Wikipedia answered an error"),
     "a 200 that gave nothing: the parse says why");
  ok(!strcmp(wikiSkipWhy(200, GEM_NET_OK, WIKI_OFF_TOPIC, b, sizeof(b)), "no page was about the keywords") &&
     !strcmp(wikiSkipWhy(200, GEM_NET_OK, WIKI_NO_TITLE, b, sizeof(b)), "no page title it can show"),
     "...or wikiCompose (round 3)");
  char t[8];
  ok(strlen(wikiSkipWhy(429, GEM_NET_OK, WIKI_BAD, t, sizeof(t))) == 7, "a small buffer: cut, terminated");
}

// ── the round trip, with gemini.cpp ──────────────────────────────────────────────────────

static void testRoundTrip() {
  group("question -> keywords -> URL -> response -> the question's turn -> the answer -> the chat");
  const char* q = "has the new tomb raider legacy of atlantis come out";
  char k[WIKI_KW_CAP];
  ok(wikiKeywords(q, NULL, 0, k, sizeof(k)) == 4, "keywords");
  char u[WIKI_URL_MAX];
  ok(wikiUrl(k, u, sizeof(u)) < sizeof(u) && strstr(u, "gsrsearch=tomb%20raider%20legacy%20atlantis&"), "the URL");
  WikiResult r;
  ok(wikiParse(W_TOMB, strlen(W_TOMB), &r) == WIKI_OK, "the response");
  char bgb[WIKI_BG_MAX + 1], sb[WIKI_SRC_MAX + 1];
  int why = -1;
  ok(wikiCompose(&r, k, WIKI_BG_MAX, bgb, sizeof(bgb), sb, sizeof(sb), &why) == 2 && why == WIKI_OK && bgb[0] && sb[0],
     "the background and the sources line (wikiCompose: the phone's path), in the phone's own buffer sizes");

  // The chat as the phone keeps it: an earlier exchange in this topic, answered with background.
  GeminiChat* chat = (GeminiChat*)calloc(1, sizeof(GeminiChat));
  GeminiReply rep;
  memset(&rep, 0, sizeof(rep));
  strcpy(rep.finish, "STOP");
  char ans[GEM_A_MAX];
  geminiAnswerText(&rep, "It is due in 2027.", sb, ans, sizeof(ans));
  ok(std::string(ans) == std::string("It is due in 2027.\n\n") + sb, "the shown answer ends with the sources line");
  geminiChatAdd(chat, 1791752700, true, false, "Flash", q, ans);
  ok(!strcmp(chat->ex[0].q, q), "the chat keeps the PLAIN question (never the background)");

  // The follow-up: borrows the topic's question for its keywords, sends the background in ITS turn.
  const char* q2 = "and when?";
  int idx[GEM_CONTEXT];
  const int nc = geminiChatContext(chat, idx);
  ok(nc == 1, "one exchange of context");
  const char* earlierQ[GEM_CONTEXT];
  for (int i = 0; i < nc; i++) earlierQ[i] = chat->ex[idx[i]].q;
  ok(wikiKeywords(q2, earlierQ, nc, k, sizeof(k)) == 4 && !strcmp(k, "tomb raider legacy atlantis"),
     "\"and when?\" searches the topic's words");
  GeminiTurnPair ctx[GEM_CONTEXT];
  for (int i = 0; i < nc; i++) {
    ctx[i].q = chat->ex[idx[i]].q;
    ctx[i].a = chat->ex[idx[i]].a;
  }
  char sys[GEM_SYS_MAX];
  geminiSystemText(1791752700, -7 * 3600, "Pacific", sys, sizeof(sys));
  const size_t n = geminiBuildRequest(ctx, nc, q2, bgb, sys, 1024, NULL, 0);
  std::string body(n + 1, '\0');
  geminiBuildRequest(ctx, nc, q2, bgb, sys, 1024, &body[0], n + 1);
  body.resize(n);
  ok(count(body, "Background from Wikipedia") == 1 && body.find("\\n\\nQuestion: and when?\"}]}]") != std::string::npos,
     "the background ONCE, in the last turn, before \"Question: and when?\"");
  ok(body.find("{\"role\":\"user\",\"parts\":[{\"text\":\"has the new tomb raider legacy of atlantis come out\"}]}") !=
     std::string::npos, "the context's question is the plain one");
  ok(body.find("{\"role\":\"model\",\"parts\":[{\"text\":\"It is due in 2027.\"}]}") != std::string::npos &&
     count(body, "Wikipedia: Tomb Raider") == 0, "the context's answer WITHOUT its sources line");
  ok(body.find("Background from Wikipedia") > body.find("\"contents\""), "nothing from Wikipedia in the system instruction");
  ok(body.find("\"tools\"") == std::string::npos, "and no tool");
  Js j = { body.c_str(), body.c_str() + body.size() };
  ok(jsSkip(&j) && j.p == j.e, "the body is one whole JSON value (json_read skips it end to end)");

  // ...and when it lands, the chat again keeps only the plain question.
  geminiAnswerText(&rep, "Early 2027.", sb, ans, sizeof(ans));
  geminiChatAdd(chat, 1791752800, false, false, "Flash", q2, ans);
  char* saved = (char*)malloc(GEM_CHAT_FILE_MAX + 1);
  const size_t sn = geminiChatSave(chat, saved, GEM_CHAT_FILE_MAX + 1);
  ok(sn < GEM_CHAT_FILE_MAX && strstr(saved, "Background from") == NULL && strstr(saved, "q and when?\n"),
     "the saved chat: plain questions, never background");
  free(saved);
  free(chat);
}

int main() {
  testKeywords();
  testUrl();
  testParse();
  testRelevance();
  testBackground();
  testSources();
  testCompose();
  testSkipWhy();
  testRoundTrip();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
