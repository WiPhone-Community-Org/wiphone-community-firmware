/* test_gemini.cpp - the pure half of Menu > AI (WiPhone/gemini.cpp): the key file, the request
 * body, Google's answers (fixtures copied from the shapes measured live on 2026-10-03, with the
 * text changed), the chunked body, the screen's ASCII, the model ladder with the free tier's
 * daily limit, and the saved chat with its topic breaks. Since 2026-10-11: today's date in the system
 * instruction, NO tool sent or named (url_context kept 3.8 Flash silent for 150 s; a named one came
 * back MALFORMED_FUNCTION_CALL, and rule 0 asks once more), the Wikipedia background in the
 * question's own turn, the "Wikipedia:" sources line, lookup=off. (test_wiki.cpp has the lookup.)
 * Round 3: the ladder's rule 6 ("Retry if it times out") and Key info's line cut, never emptied.
 *
 * WHY THIS IS WORTH A SUITE: every failure here is quiet on the phone. A parser that keeps a
 * trailing space sends a key Google rejects; half a JSON body comes back as a 400 that reads
 * like a bad key; a 429 retried three times burns a twentieth of the day's answers each time; a
 * lost "thought" flag prints the model's private reasoning as its answer. And the key must never
 * reach a screen, a log or the console: every text built here is checked for the fake one.
 *
 * ⚠ Only obviously FAKE keys here (with a '.', the shape of a real newer key). Never a real one.
 */
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../WiPhone/gemini.h"

// The phone's own glyph table, for measuring the WAITING box's rows (as tests/test_elevtext.cpp).
#define PROGMEM
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../WiPhone/src/assets/fonts.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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

static const char FAKE_KEY[] = "FA.fake-key-for-tests_0123456789abcdef0123456789";

static bool noKey(const char* s) {
  return strstr(s, FAKE_KEY) == NULL && strstr(s, "fake-key-for-tests") == NULL;
}

static bool plainAscii(const std::string& s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c > 0x7E) {
      if (c != '\n') return false;
    }
  }
  return s.find('*') == std::string::npos && s.find('#') == std::string::npos;
}

static int parse(const char* text, GeminiConfig* c) {
  return geminiParseConfig(text, strlen(text), c);
}

// ── fixtures: Google's real shapes, texts changed ─────────────────────────────────────────

static const char R_TEXT[] =
  "{\n  \"candidates\": [\n    {\n      \"content\": {\n        \"parts\": [\n          {\n"
  "            \"text\": \"Paris is the capital of France.\\nIt sits on the Seine.\"\n"
  "          }\n        ],\n        \"role\": \"model\"\n      },\n"
  "      \"finishReason\": \"STOP\",\n      \"index\": 0\n    }\n  ],\n"
  "  \"usageMetadata\": {\n    \"promptTokenCount\": 51,\n    \"candidatesTokenCount\": 7,\n"
  "    \"totalTokenCount\": 58,\n    \"promptTokensDetails\": [ { \"modality\": \"TEXT\", "
  "\"tokenCount\": 51 } ],\n    \"thoughtsTokenCount\": 412\n  },\n"
  "  \"modelVersion\": \"gemini-flash-latest\",\n  \"responseId\": \"r-1\"\n}\n";

static const char R_MAXTOK[] =
  "{\"candidates\":[{\"content\":{\"role\":\"model\"},\"finishReason\":\"MAX_TOKENS\",\"index\":0}],"
  "\"usageMetadata\":{\"promptTokenCount\":40,\"totalTokenCount\":551,\"thoughtsTokenCount\":511},"
  "\"modelVersion\":\"gemini-flash-latest\",\"responseId\":\"r-2\"}";

static const char R_THOUGHT[] =
  "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"**Weighing it** the user wants...\","
  "\"thought\":true},{\"text\":\"Two plus two \"},{\"thought\":false,\"text\":\"is four.\"}],"
  "\"role\":\"model\"},\"finishReason\":\"STOP\",\"index\":0}],"
  "\"modelVersion\":\"gemini-flash-latest\"}";

static const char R_400_THINK[] =
  "{\n  \"error\": {\n    \"code\": 400,\n    \"message\": \"Request contains an invalid argument.\",\n"
  "    \"status\": \"INVALID_ARGUMENT\"\n  }\n}\n";

static const char R_400_KEY[] =
  "{\"error\":{\"code\":400,\"message\":\"API key not valid. Please pass a valid API key.\","
  "\"status\":\"INVALID_ARGUMENT\",\"details\":[{\"@type\":\"type.googleapis.com/google.rpc.ErrorInfo\","
  "\"reason\":\"API_KEY_INVALID\",\"domain\":\"googleapis.com\",\"metadata\":{\"service\":"
  "\"generativelanguage.googleapis.com\"}}]}}";

static const char R_404[] =
  "{\"error\":{\"code\":404,\"message\":\"models/gemini-9-flash is not found for API version "
  "v1beta, or is not supported for generateContent. Call ListModels to see the list of available "
  "models and their supported methods.\",\"status\":\"NOT_FOUND\"}}";

static const char R_503[] =
  "{\"error\":{\"code\":503,\"message\":\"This model is currently experiencing high demand. Spikes "
  "in demand are usually temporary. Please try again later.\",\"status\":\"UNAVAILABLE\"}}";

/* The other 5xx answers, in the shape Google's errors take (the texts are its documented ones). */
static const char R_500[] =
  "{\"error\":{\"code\":500,\"message\":\"An internal error has occurred. Please retry or report in "
  "https://developers.generativeai.google/guide/troubleshooting\",\"status\":\"INTERNAL\"}}";
static const char R_504[] =
  "{\"error\":{\"code\":504,\"message\":\"Deadline expired before operation could complete.\",\"status\":"
  "\"DEADLINE_EXCEEDED\"}}";
/* A front end's 502: an HTML page, not JSON at all. */
static const char R_502[] =
  "<!DOCTYPE html>\n<html lang=en><title>Error 502 (Server Error)!!1</title><p><b>502.</b> That's an "
  "error.<p>The server encountered a temporary error and could not complete your request.</html>";

static const char R_429_DAY[] =
  "{\n  \"error\": {\n    \"code\": 429,\n    \"message\": \"You exceeded your current quota, please "
  "check your plan and billing details.\\n* Quota exceeded for metric: generativelanguage."
  "googleapis.com/generate_content_free_tier_requests, limit: 20, model: gemini-flash-latest\\n"
  "Please retry in 6h15m17.4s.\",\n    \"status\": \"RESOURCE_EXHAUSTED\",\n    \"details\": [\n"
  "      {\n        \"@type\": \"type.googleapis.com/google.rpc.QuotaFailure\",\n"
  "        \"violations\": [\n          {\n            \"quotaMetric\": \"generativelanguage."
  "googleapis.com/generate_content_free_tier_requests\",\n            \"quotaId\": "
  "\"GenerateRequestsPerDayPerProjectPerModel-FreeTier\",\n            \"quotaDimensions\": "
  "{ \"location\": \"global\", \"model\": \"gemini-flash-latest\" },\n            \"quotaValue\": "
  "\"20\"\n          }\n        ]\n      },\n      {\n        \"@type\": \"type.googleapis.com/"
  "google.rpc.Help\",\n        \"links\": [ { \"description\": \"Learn more about Gemini API "
  "quotas\", \"url\": \"https://ai.google.dev/gemini-api/docs/rate-limits\" } ]\n      },\n"
  "      {\n        \"@type\": \"type.googleapis.com/google.rpc.RetryInfo\",\n"
  "        \"retryDelay\": \"22517s\"\n      }\n    ]\n  }\n}\n";

/* A PerDay limit with NO RetryInfo: remembered, but the phone has no time to show. */
static const char R_429_DAY_NOTIME[] =
  "{\"error\":{\"code\":429,\"message\":\"You exceeded your current quota.\",\"status\":"
  "\"RESOURCE_EXHAUSTED\",\"details\":[{\"@type\":\"type.googleapis.com/google.rpc.QuotaFailure\","
  "\"violations\":[{\"quotaMetric\":\"generativelanguage.googleapis.com/generate_content_free_tier"
  "_requests\",\"quotaId\":\"GenerateRequestsPerDayPerProjectPerModel-FreeTier\",\"quotaValue\":"
  "\"20\"}]}]}}";

static const char R_429_MIN[] =
  "{\"error\":{\"code\":429,\"message\":\"You exceeded your current quota.\",\"status\":"
  "\"RESOURCE_EXHAUSTED\",\"details\":[{\"@type\":\"type.googleapis.com/google.rpc.QuotaFailure\","
  "\"violations\":[{\"quotaMetric\":\"generativelanguage.googleapis.com/generate_content_free_tier"
  "_requests\",\"quotaId\":\"GenerateRequestsPerMinutePerProjectPerModel-FreeTier\",\"quotaValue\":"
  "\"10\"}]},{\"@type\":\"type.googleapis.com/google.rpc.RetryInfo\",\"retryDelay\":\"37.2s\"}]}}";

static const char R_429_BARE[] =
  "{\"error\":{\"code\":429,\"message\":\"Resource has been exhausted (e.g. check quota).\","
  "\"status\":\"RESOURCE_EXHAUSTED\"}}";

static const char R_BLOCK[] =
  "{\"promptFeedback\":{\"blockReason\":\"PROHIBITED_CONTENT\"},\"usageMetadata\":"
  "{\"promptTokenCount\":9,\"totalTokenCount\":9},\"modelVersion\":\"gemini-flash-latest\"}";

static const char R_SAFETY[] =
  "{\"candidates\":[{\"finishReason\":\"SAFETY\",\"index\":0,\"safetyRatings\":[{\"category\":"
  "\"HARM_CATEGORY_DANGEROUS_CONTENT\",\"probability\":\"HIGH\",\"blocked\":true}]}]}";

static const char R_UNICODE[] =
  "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Caf\\u00e9 \\u2014 \\\"quoted\\\" "
  "\\ud83d\\ude00 end\\nline2 \\u00bd \\/ \\\\ back\\tslash\\u0000!\"}],\"role\":\"model\"},"
  "\"finishReason\":\"STOP\"}]}";

/* 2026-10-11, gemini-flash-latest (3.8 Flash): 84 s for one sentence, then this - finishReason
 * MALFORMED_FUNCTION_CALL and no text - because the system instruction NAMED a tool the request did
 * not declare (none is named now). The shape is a no-text candidate like R_MAXTOK's; the counts
 * are made up. */
static const char R_MALFORMED[] =
  "{\"candidates\":[{\"content\":{},\"finishReason\":\"MALFORMED_FUNCTION_CALL\",\"index\":0}],"
  "\"usageMetadata\":{\"promptTokenCount\":312,\"totalTokenCount\":312},"
  "\"modelVersion\":\"gemini-3.8-flash\",\"responseId\":\"r-m\"}";

static const char R_ECHO_KEY[] =
  "{\"error\":{\"code\":400,\"message\":\"API key not valid: FA.fake-key-for-tests_0123456789abcdef"
  "0123456789 was refused\",\"status\":\"INVALID_ARGUMENT\"}}";

static char g_text[32768];

static int reply(const char* body, GeminiReply* r) {
  return geminiParseReply(body, strlen(body), g_text, sizeof(g_text), r);
}

// ── the phone's font, as SmoothFont reads it (the measurer of tests/test_elevtext.cpp) ─────

struct Font7 {
  int      n;
  uint16_t uni[256];
  uint8_t  w[256], adv[256];
  int8_t   dx[256];
  int      space;
  bool     ok;
};

static uint32_t runDecode(const unsigned char** p) {       // display::runDecodeNumber
  uint32_t v = 0;
  unsigned char more = 0;
  do {
    v <<= 7;
    v |= (**p) & 0x7F;
    more = (**p) & 0x80;
    (*p)++;
  } while (more);
  return v;
}

static void fontLoad(Font7* f, const unsigned char* data) {
  memset(f, 0, sizeof(*f));
  if (data[0] != '7' || data[1] != 'S' || data[2] != 'F') {
    return;
  }
  const unsigned char* p = data + 3;
  f->n = (int)runDecode(&p);
  runDecode(&p);                                           // point size
  const int ascent = (int)runDecode(&p);
  const int descent = (int)runDecode(&p);
  if (f->n <= 0 || f->n > 256) {
    return;
  }
  for (int g = 0; g < f->n; g++) {
    f->uni[g] = (uint16_t)(p[0] | (p[1] << 8));
    p += 4;
    runDecode(&p);                                         // height
    f->w[g] = (uint8_t)runDecode(&p);
    f->adv[g] = (uint8_t)runDecode(&p);
    runDecode(&p);                                         // dY
    f->dx[g] = (int8_t)runDecode(&p);
  }
  f->space = (ascent + descent) * 2 / 7;                   // loadFont's "guess at space width"
  f->ok = true;
}

static bool fontGlyph(const Font7* f, uint16_t u, int* g) {
  for (int i = 0; i < f->n; i++) {
    if (f->uni[i] == u) {
      *g = i;
      return true;
    }
  }
  return false;
}

// SmoothFont::textWidth, for ASCII.
static int textWidth(const Font7* f, const char* s) {
  int w = 0;
  for (const char* p = s; *p; p++) {
    const uint16_t u = (unsigned char)*p;
    if (u == 0x20) {
      w += f->space;
      continue;
    }
    int g = 0;
    if (fontGlyph(f, u, &g)) {
      if (w == 0 && f->dx[g] < 0) {
        w -= f->dx[g];
      }
      w += p[1] ? f->adv[g] : (f->dx[g] + f->w[g]);
    } else {
      w += f->space + 1;
    }
  }
  return w;
}

// SmoothFont::fitTextLength(s, width, 1): how many characters fit, by their advances.
static int fitTextLength(const Font7* f, const char* s, int width) {
  int fit = 0, sw = 0;
  for (const char* p = s; *p; p++) {
    int cw, g = 0;
    if (*p == ' ') {
      cw = f->space;
    } else if (fontGlyph(f, (unsigned char)*p, &g)) {
      cw = f->adv[g];
      if (sw == 0 && f->dx[g] < 0) cw -= f->dx[g];
    } else {
      cw = f->space + 1;
    }
    if (sw + cw > width) break;
    sw += cw;
    fit = (int)(p - s) + 1;
  }
  return fit;
}

static bool allGlyphs(const Font7* f, const char* s) {
  for (const char* p = s; *p; p++) {
    int g = 0;
    if (*p != ' ' && !fontGlyph(f, (unsigned char)*p, &g)) {
      printf("    no glyph for '%c' in \"%s\"\n", *p, s);
      return false;
    }
  }
  return true;
}

static Font7 g_font;
static const int AI_STAGE_W = 240 - 2 * 6;    // lcd.width() - 2 * AI_MARGIN (app_ai.cpp drawStatus)
static const int AI_STAGE_ROWS = 3;           // app_ai.cpp AI_STAGE_ROWS

/* The rows AiApp::drawWrapped needs for `s` with no row limit (it breaks at the last space that
 * fits): more than AI_STAGE_ROWS means the box would ellipsize the notice. */
static int wrapRows(const Font7* f, const char* s, int w) {
  int rows = 0;
  while (*s) {
    const int len = (int)strlen(s);
    const int fit = fitTextLength(f, s, w);
    rows++;
    if (fit <= 0 || fit >= len) {
      break;
    }
    int cut = fit;
    while (cut > 0 && s[cut] != ' ') cut--;
    if (cut == 0) cut = fit;
    s += cut;
    while (*s == ' ') s++;
  }
  return rows;
}

// ── the key file ─────────────────────────────────────────────────────────────────────────

static void testConfig() {
  group("the key file (a superset of the T-Deck's parser)");
  GeminiConfig c;
  std::string canon = std::string("key=") + FAKE_KEY + "\n";
  ok(parse(canon.c_str(), &c) == GEM_CFG_OK && !strcmp(c.key, FAKE_KEY) &&
     c.keyLen == (int)strlen(FAKE_KEY), "the canonical key= line, a '.' in the key");
  ok(!strcmp(c.model, GEM_DEFAULT_MODEL) && !c.modelSet, "no model= -> gemini-flash-latest");
  ok(!strcmp(c.fallback, GEM_LITE_MODEL) && c.thinking == GEM_THINKING,
     "defaults: fallback flash-lite, thinking 1024");
  ok(c.warn == 0, "a 48-char dotted key draws no warning");

  std::string bom = std::string("\xEF\xBB\xBFkey=") + FAKE_KEY + "\r\nmodel=gemini-2.5-flash\r\n";
  ok(parse(bom.c_str(), &c) == GEM_CFG_OK && !strcmp(c.key, FAKE_KEY) &&
     !strcmp(c.model, "gemini-2.5-flash") && c.modelSet, "a UTF-8 BOM and CRLF (Notepad)");
  std::string sp = std::string("  KEY =  \"") + FAKE_KEY + "\"   \n Model = 'models/gemini-flash-lite-latest'\n";
  ok(parse(sp.c_str(), &c) == GEM_CFG_OK && !strcmp(c.key, FAKE_KEY) &&
     !strcmp(c.model, "gemini-flash-lite-latest"),
     "any case, spaces round '=', quotes stripped, models/ dropped");
  std::string bare = std::string("# my Gemini key\n\n") + FAKE_KEY + "\n";
  ok(parse(bare.c_str(), &c) == GEM_CFG_OK && !strcmp(c.key, FAKE_KEY), "a lone bare key line");
  std::string both = std::string("key=AAAAAAAAAAAAAAAAAAAAAAAAA\n") + "key=" + FAKE_KEY + "\n";
  ok(parse(both.c_str(), &c) == GEM_CFG_OK && !strcmp(c.key, FAKE_KEY), "the last key= wins");
  std::string barePlusKey = std::string("key=") + FAKE_KEY + "\nZZZZZZZZZZZZZZZZZZZZZZZZZZZZ\n";
  ok(parse(barePlusKey.c_str(), &c) == GEM_CFG_OK && !strcmp(c.key, FAKE_KEY),
     "a key= line beats a bare line");
  ok(parse("key=\nmodel=gemini-flash-latest\n", &c) == GEM_CFG_NO_KEY && c.key[0] == '\0',
     "key= empty is no key (the unfilled template)");
  ok(parse("# only a comment\n\n", &c) == GEM_CFG_NO_KEY, "comments only: no key");
  ok(parse("", &c) == GEM_CFG_NO_KEY && geminiParseConfig(NULL, 0, &c) == GEM_CFG_NO_KEY,
     "an empty file / nothing");
  std::string cmt = std::string("key=") + FAKE_KEY + " # mine\n";
  ok(parse(cmt.c_str(), &c) == GEM_CFG_BAD_KEY && c.key[0] == '\0' && c.problem &&
     strstr(c.problem, "comment"), "a trailing comment is refused, never folded into the key");
  ok(parse("key=abc\x01" "defghijklmnopqrstuvwxyz\n", &c) == GEM_CFG_BAD_KEY, "a control character");
  ok(parse("key=abc\xC3\xA9" "defghijklmnopqrstuvwxyz\n", &c) == GEM_CFG_BAD_KEY, "a non-ASCII byte");
  ok(parse("key=short.key\n", &c) == GEM_CFG_OK && (c.warn & GEM_WARN_SHAPE),
     "a short key WARNS, never refuses");
  ok(parse("key=AIzaSyA_odd+chars/here0123456789abcd\n", &c) == GEM_CFG_OK && (c.warn & GEM_WARN_SHAPE),
     "odd characters warn too");
  std::string longk = "key=" + std::string(90, 'k') + "\n";
  ok(parse(longk.c_str(), &c) == GEM_CFG_OK && (c.warn & GEM_WARN_TDECK) && !(c.warn & GEM_WARN_SHAPE),
     "80+ chars: the T-Deck warning");
  std::string tool = "key=" + std::string(300, 'k') + "\n";
  ok(parse(tool.c_str(), &c) == GEM_CFG_BAD_KEY, "a key longer than the buffer is refused");
  std::string mbad = std::string("key=") + FAKE_KEY + "\nmodel=gemini flash\n";
  ok(parse(mbad.c_str(), &c) == GEM_CFG_OK && (c.warn & GEM_WARN_MODEL) &&
     !strcmp(c.model, GEM_DEFAULT_MODEL), "a model= with a space: warned, default used");
  std::string th = std::string("key=") + FAKE_KEY + "\nthinking=0\nfallback=none\n";
  ok(parse(th.c_str(), &c) == GEM_CFG_OK && c.thinking == 0 && c.fallback[0] == '\0',
     "thinking=0 (off) and fallback=none");
  std::string th2 = std::string("key=") + FAKE_KEY + "\nthinking=9000\nfallback=gemini-2.5-flash-lite\n";
  ok(parse(th2.c_str(), &c) == GEM_CFG_OK && c.thinking == GEM_THINKING && (c.warn & GEM_WARN_THINKING) &&
     !strcmp(c.fallback, "gemini-2.5-flash-lite"), "thinking out of range: warned; fallback= named");
  ok(parse("model=gemini-flash-latest\nkey=AAAAAAAAAAAAAAAAAAAAAAAA\nnonsense line here\nfoo=bar\n", &c) ==
     GEM_CFG_OK, "unknown lines are ignored");

  group("lookup=off: the Wikipedia lookup's switch (2026-10-11)");
  ok(parse(canon.c_str(), &c) == GEM_CFG_OK && c.lookup && !(c.warn & GEM_WARN_LOOKUP),
     "no lookup= line: ON (the default)");
  std::string lk = std::string("key=") + FAKE_KEY + "\nlookup=off\n";
  ok(parse(lk.c_str(), &c) == GEM_CFG_OK && !c.lookup && c.warn == 0 && !strcmp(c.key, FAKE_KEY),
     "lookup=off: off, the key untouched");
  lk = std::string("key=") + FAKE_KEY + "\r\n  LOOKUP  =  OFF  \r\n";
  ok(parse(lk.c_str(), &c) == GEM_CFG_OK && !c.lookup, "any case, spaces round '=', CRLF");
  lk = std::string("key=") + FAKE_KEY + "\nLookup = \"Off\"\n";
  ok(parse(lk.c_str(), &c) == GEM_CFG_OK && !c.lookup, "quoted");
  lk = std::string("key=") + FAKE_KEY + "\nlookup=off\nlookup=on\n";
  ok(parse(lk.c_str(), &c) == GEM_CFG_OK && c.lookup, "the last lookup= wins (on)");
  lk = std::string("key=") + FAKE_KEY + "\nlookup=\n";
  ok(parse(lk.c_str(), &c) == GEM_CFG_OK && c.lookup && c.warn == 0, "lookup= empty: the default, no warning");
  lk = std::string("key=") + FAKE_KEY + "\nlookup=no\n";
  ok(parse(lk.c_str(), &c) == GEM_CFG_OK && c.lookup && (c.warn & GEM_WARN_LOOKUP),
     "lookup=no (a typo for off): ON, and warned - never a guess at what was meant");
  lk = std::string("key=") + FAKE_KEY + "\n#lookup=off\n";
  ok(parse(lk.c_str(), &c) == GEM_CFG_OK && c.lookup, "a commented-out lookup=off is a comment");
  lk = std::string("lookup=off\n") + FAKE_KEY + "\n";
  ok(parse(lk.c_str(), &c) == GEM_CFG_OK && !c.lookup && !strcmp(c.key, FAKE_KEY),
     "lookup=off beside a bare key line: both read");
  lk = std::string("key=") + FAKE_KEY + "\nmodel=gemini-2.5-flash\nthinking=0\nfallback=none\nlookup=off\n";
  ok(parse(lk.c_str(), &c) == GEM_CFG_OK && !c.lookup && !strcmp(c.model, "gemini-2.5-flash") &&
     c.thinking == 0 && c.fallback[0] == '\0' && c.warn == 0, "...with every other line: the rules unchanged");
  ok(parse("lookup=off\n", &c) == GEM_CFG_NO_KEY && !c.lookup, "a file with only lookup=off: no key");

  char line[300];
  parse(canon.c_str(), &c);
  geminiConfigLine(&c, GEM_CFG_OK, line, sizeof(line));
  ok(strstr(line, "key: set (48 chars)") && strstr(line, "gemini-flash-latest") &&
     strstr(line, "thinking 1024") && noKey(line), "the status line says the LENGTH, never the key");
  ok(!strstr(line, "lookup"), "...and nothing about the lookup while it is on");
  printf("      \"%s\"\n", line);
  parse((std::string("key=") + FAKE_KEY + "\nlookup=off\n").c_str(), &c);
  geminiConfigLine(&c, GEM_CFG_OK, line, sizeof(line));
  ok(strstr(line, ", Wikipedia lookup off") && noKey(line), "lookup=off: \", Wikipedia lookup off\"");
  printf("      \"%s\"\n", line);
  parse((std::string("key=") + FAKE_KEY + "\nlookup=maybe\n").c_str(), &c);
  geminiConfigLine(&c, GEM_CFG_OK, line, sizeof(line));
  ok(strstr(line, "; lookup= must be on or off (on used)") && !strstr(line, "lookup off"),
     "lookup=maybe: the warning, the lookup on");
  ok(strlen(line) < 200, "...and the line fits Key info's 200 (AiKeyInfo.line)");
  {
    // review 2026-10-11: two long model names + the lookup note passed 200 bytes, and the line came out EMPTY.
    const std::string m63 = "gemini-" + std::string(56, 'm'), f63 = "gemini-" + std::string(56, 'f');
    std::string longest = std::string("key=") + FAKE_KEY + "\nmodel=" + m63 + "\nfallback=" + f63 +
                          "\nlookup=off\nthinking=99999\n";
    ok(parse(longest.c_str(), &c) == GEM_CFG_OK && !strcmp(c.model, m63.c_str()) && !strcmp(c.fallback, f63.c_str()),
       "63-character model= and fallback= names are names");
    char l200[200], l256[256], big[600];
    const size_t whole = geminiConfigLine(&c, GEM_CFG_OK, big, sizeof(big));
    geminiConfigLine(&c, GEM_CFG_OK, l200, sizeof(l200));
    geminiConfigLine(&c, GEM_CFG_OK, l256, sizeof(l256));
    printf("      %u bytes whole: \"%s\"\n", (unsigned)whole, big);
    ok(whole > 200 && whole == strlen(big), "...the whole line passes 200 (the length is still the whole one)");
    ok(strlen(l200) == 199 && !strcmp(l200 + 196, "...") && !strncmp(l200, big, 196),
       "into 200: CUT with \"...\", never empty");
    ok(whole < 256 && !strcmp(l256, big), "into 256 (AiKeyInfo.line now): whole");
    std::string worst = std::string("key=") + std::string(190, 'k') + "!\nmodel=" + m63 + "\nfallback=" + f63 +
                        "\nlookup=off\nthinking=-1\n";
    ok(parse(worst.c_str(), &c) == GEM_CFG_OK && geminiConfigLine(&c, GEM_CFG_OK, big, sizeof(big)) > 256,
       "every warning at once passes even 256...");
    geminiConfigLine(&c, GEM_CFG_OK, l256, sizeof(l256));
    ok(strlen(l256) == 255 && !strcmp(l256 + 252, "...") && noKey(l256), "...and is cut there, the key's LENGTH only");
    char tiny[4];
    geminiConfigLine(&c, GEM_CFG_OK, tiny, sizeof(tiny));
    char one[1] = { 'x' };
    geminiConfigLine(&c, GEM_CFG_OK, one, sizeof(one));
    ok(!strcmp(tiny, "...") && one[0] == '\0' && geminiConfigLine(&c, GEM_CFG_OK, NULL, 0) > 256,
       "tiny buffers: \"...\" / \"\" (NUL-terminated), and NULL just counts");
  }
  parse(canon.c_str(), &c);
  int rc = parse(cmt.c_str(), &c);
  geminiConfigLine(&c, rc, line, sizeof(line));
  ok(strstr(line, "refused") && noKey(line), "a refused key's line has no key in it either");

  ok(geminiModelNameOk("gemini-2.5-flash") && !geminiModelNameOk("a/b") && !geminiModelNameOk("") &&
     !geminiModelNameOk("x?y") && !geminiModelNameOk(std::string(64, 'm').c_str()),
     "model names safe for the URL path");
  ok(geminiModelNameOk("gemini-flash-lite-latest") && geminiModelNameOk("9x") &&
     geminiModelNameOk(std::string(63, 'm').c_str()) && !geminiModelNameOk("Gemini-Flash") &&
     !geminiModelNameOk("gemini_flash") && !geminiModelNameOk("-flash") && !geminiModelNameOk(".flash") &&
     !geminiModelNameOk(FAKE_KEY), "lowercase [a-z0-9][a-z0-9.-]* only: a mixed-case key is no model name");

  group("a key on the wrong line is never a model name");
  // The key pasted after model= (and key= left empty): dropped, never kept, drawn or printed.
  std::string wrong = std::string("key=\nmodel=") + FAKE_KEY + "\n";
  rc = parse(wrong.c_str(), &c);
  geminiConfigLine(&c, rc, line, sizeof(line));
  ok(rc == GEM_CFG_NO_KEY && !strcmp(c.model, GEM_DEFAULT_MODEL) && !c.modelSet && (c.warn & GEM_WARN_MODEL),
     "key= empty + model=<a key>: no key, the default model");
  ok(noKey(line) && noKey(c.model) && strstr(line, "key: none") &&
     strstr(line, "model= line ignored (not a model name)"), "...and the status line says so, without it");
  printf("      \"%s\"\n", line);
  std::string wrong2 = std::string("key=OLDKEY.0123456789abcdefXYZ\nmodel=models/") + FAKE_KEY +
                       "\nfallback=" + FAKE_KEY + "\n";
  rc = parse(wrong2.c_str(), &c);
  geminiConfigLine(&c, rc, line, sizeof(line));
  ok(rc == GEM_CFG_OK && !strcmp(c.model, GEM_DEFAULT_MODEL) && !strcmp(c.fallback, GEM_LITE_MODEL) &&
     (c.warn & GEM_WARN_MODEL) && (c.warn & GEM_WARN_FALLBACK), "an old key + the new one on model= and fallback=");
  ok(noKey(line) && strstr(line, "model= line ignored") && strstr(line, "fallback= line ignored (not a model name)"),
     "...each line named, the value in neither");
  ok(memmem(&c, sizeof(c), "fake-key-for-tests", 18) == NULL, "...and nowhere in the parsed config at all");
  // A key that happens to LOOK like a model name (all lowercase) on both lines: still not the model.
  const char lowKey[] = "fa.fake-lowercase-key-0123456789abcdef";
  std::string same = std::string("model=") + lowKey + "\nfallback=" + lowKey + "\nkey=" + lowKey + "\n";
  rc = parse(same.c_str(), &c);
  geminiConfigLine(&c, rc, line, sizeof(line));
  ok(rc == GEM_CFG_OK && !strcmp(c.model, GEM_DEFAULT_MODEL) && !c.modelSet && !strcmp(c.fallback, GEM_LITE_MODEL) &&
     (c.warn & GEM_WARN_MODEL) && (c.warn & GEM_WARN_FALLBACK) && !strstr(line, lowKey),
     "model= / fallback= equal to the key: dropped");
  std::string bareSame = std::string("model=") + lowKey + "\n" + lowKey + "\n";
  ok(parse(bareSame.c_str(), &c) == GEM_CFG_OK && !strcmp(c.model, GEM_DEFAULT_MODEL) && (c.warn & GEM_WARN_MODEL),
     "...against a bare key line too");
  std::string lowCmt = std::string("key=") + lowKey + " # mine\nmodel=" + lowKey + "\nfallback=" + lowKey + "\n";
  rc = parse(lowCmt.c_str(), &c);
  geminiConfigLine(&c, rc, line, sizeof(line));
  ok(rc == GEM_CFG_BAD_KEY && !strcmp(c.model, GEM_DEFAULT_MODEL) && !strcmp(c.fallback, GEM_LITE_MODEL) &&
     !strstr(line, lowKey), "...and against a key refused for a comment after it");
  std::string upper = std::string("key=") + FAKE_KEY + "\nmodel=Gemini-2.5-Flash\n";
  ok(parse(upper.c_str(), &c) == GEM_CFG_OK && !strcmp(c.model, GEM_DEFAULT_MODEL) && (c.warn & GEM_WARN_MODEL),
     "a model= in capitals is not a model name either (Google's are lowercase)");
  char lab[GEM_LABEL_MAX];
  geminiModelLabel("gemini-flash-latest", lab, sizeof(lab));
  ok(!strcmp(lab, "Flash"), "label: gemini-flash-latest -> Flash");
  geminiModelLabel("gemini-flash-lite-latest", lab, sizeof(lab));
  ok(!strcmp(lab, "Flash-Lite"), "label: gemini-flash-lite-latest -> Flash-Lite");
  geminiModelLabel("gemini-2.5-flash", lab, sizeof(lab));
  ok(!strcmp(lab, "2.5-Flash"), "label: gemini-2.5-flash -> 2.5-Flash");
}

// ── the request ──────────────────────────────────────────────────────────────────────────

static std::string build(const GeminiTurnPair* ctx, int n, const char* q, int budget, const char* bg = NULL,
                         const char* sys = GEM_SYSTEM_INSTRUCTION) {
  const size_t len = geminiBuildRequest(ctx, n, q, bg, sys, budget, NULL, 0);
  std::string s(len + 1, '\0');
  const size_t len2 = geminiBuildRequest(ctx, n, q, bg, sys, budget, &s[0], len + 1);
  ok(len2 == len && strlen(s.c_str()) == len, "build(NULL,0) is the exact length");
  s.resize(len);
  return s;
}

/* How many times `needle` is in `hay`. */
static int count(const std::string& hay, const std::string& needle) {
  int k = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) k++;
  return k;
}

static void testRequest() {
  group("the request body");
  std::string b = build(NULL, 0, "What is 2+2?", 1024);
  ok(b.find("{\"system_instruction\":{\"parts\":[{\"text\":\"You are an assistant") == 0,
     "system_instruction first (snake_case, as Google takes it)");
  ok(b.find("\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"What is 2+2?\"}]}]") != std::string::npos,
     "one user turn");
  ok(b.find("\"generationConfig\":{\"maxOutputTokens\":4096,\"temperature\":0.7,\"thinkingConfig\":"
            "{\"thinkingBudget\":1024}}}") != std::string::npos, "4096 tokens, thinking 1024 (round 2)");
  ok(b.find("ASCII") != std::string::npos && b.find("no markdown") != std::string::npos,
     "the system instruction asks for plain ASCII, no markdown");
  b = build(NULL, 0, "hi", -1);
  ok(b.find("thinkingConfig") == std::string::npos, "budget -1: NO thinkingConfig (flash-lite)");
  b = build(NULL, 0, "hi", 0);
  ok(b.find("\"thinkingBudget\":0") != std::string::npos, "thinking=0 still SENT as 0 (turns it off)");

  GeminiTurnPair ctx[2] = { { "first q", "first a" }, { "second \"q\"", "line1\nline2\\" } };
  b = build(ctx, 2, "third", 1024);
  const size_t p1 = b.find("first q"), p2 = b.find("first a"), p3 = b.find("second"), p4 = b.find("third");
  ok(p1 < p2 && p2 < p3 && p3 < p4 && p4 != std::string::npos, "context oldest first, the question last");
  ok(b.find("{\"role\":\"model\",\"parts\":[{\"text\":\"first a\"}]}") != std::string::npos,
     "answers go back as the model's turns");
  ok(b.find("second \\\"q\\\"") != std::string::npos && b.find("line1\\nline2\\\\") != std::string::npos,
     "quotes, newlines and backslashes escaped");

  char esc[64];
  geminiJsonEscape("a\tb\rc\x01" "d\xC3\xA9", esc, sizeof(esc));
  ok(!strcmp(esc, "a\\tb" "c\\u0001d\xC3\xA9"), "tab, CR dropped, \\u0001, UTF-8 passed through");

  std::string big(4000, 'x');
  GeminiTurnPair heavy[3] = { { big.c_str(), "a1" }, { big.c_str(), "a2" }, { "q3", "a3" } };
  b = build(heavy, 3, "now", 1024);
  ok(b.find("a1") == std::string::npos && b.find("a2") != std::string::npos && b.find("a3") != std::string::npos,
     "over the 6 KB text cap: the OLDEST context dropped first");
  char small[16];
  memset(small, 'Z', sizeof(small));
  const size_t n = geminiBuildRequest(NULL, 0, "hello", "- bg", GEM_SYSTEM_INSTRUCTION, 1024, small, sizeof(small));
  ok(n > sizeof(small) && small[0] == '\0', "a buffer too small is left EMPTY, never half a body");

  group("no tool, ever (2026-10-11: url_context sent 3.8 Flash silent for 150 s)");
  const char* const BG = "Background from Wikipedia, looked up just now (use it only if it is about this question):\n"
                         "- Tomb Raider: Legacy of Atlantis (Upcoming video game): An upcoming \"remake\".";
  b = build(ctx, 2, "hi", 1024, BG);
  ok(b.find("\"tools\"") == std::string::npos && b.find("url_context") == std::string::npos &&
     b.find("google_search") == std::string::npos && b.find("function") == std::string::npos,
     "no \"tools\", no url_context, no google_search, no function declarations - with background too");
  ok(b.find("\"}]}],\"generationConfig\":{") != std::string::npos, "contents close straight into generationConfig");

  group("the Wikipedia background: in the question's turn, and ONLY there");
  b = build(NULL, 0, "has the new tomb raider legacy of atlantis come out", 1024, BG);
  ok(b.find("\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"Background from Wikipedia, looked up just now "
            "(use it only if it is about this question):\\n- Tomb Raider: Legacy of Atlantis (Upcoming video game): "
            "An upcoming \\\"remake\\\".\\n\\nQuestion: has the new tomb raider legacy of atlantis come out\"}]}]")
     != std::string::npos, "one user turn: the labelled background, a blank line, \"Question: \" and the question");
  const size_t sysEnd = b.find("\"contents\"");
  ok(b.substr(0, sysEnd).find("Wikipedia, looked up") == std::string::npos &&
     b.substr(0, sysEnd).find("Legacy of Atlantis") == std::string::npos,
     "...never in the system instruction: Wikipedia's text is data");
  b = build(ctx, 2, "and when?", 1024, BG);
  ok(count(b, "Background from Wikipedia") == 1 && count(b, "Question: ") == 1 &&
     b.rfind("Background from Wikipedia") > b.rfind("{\"role\":\"model\""),
     "with context: the background once, in the LAST turn (after every model turn)");
  ok(b.find("{\"role\":\"user\",\"parts\":[{\"text\":\"first q\"}]}") != std::string::npos &&
     b.find("{\"role\":\"user\",\"parts\":[{\"text\":\"second \\\"q\\\"\"}]}") != std::string::npos,
     "...the context turns are the plain questions, as the chat keeps them");
  ok(build(NULL, 0, "q", 1024, "") == build(NULL, 0, "q", 1024, NULL) &&
     build(NULL, 0, "q", 1024, NULL).find("Question:") == std::string::npos,
     "background \"\" or NULL: the plain question, no \"Question:\" label");
  // The background counts against the 6 KB text cap: context goes first, never the background.
  std::string bigBg = std::string("Background from Wikipedia, looked up just now (use it only if it is about this question):\n- ") +
                      std::string(1000, 'w');
  std::string p3000(3000, 'p'), r2400(2400, 'r');
  GeminiTurnPair three[3] = { { p3000.c_str(), "a1" }, { r2400.c_str(), "a2" }, { "q3", "a3" } };
  b = build(three, 3, "now", 1024);
  ok(b.find("a1") != std::string::npos, "(5.4 KB of context and a question: all of it fits the 6 KB cap)");
  b = build(three, 3, "now", 1024, bigBg.c_str());
  ok(b.find("a1") == std::string::npos && b.find("a2") != std::string::npos && b.find(std::string(1000, 'w')) != std::string::npos,
     "...+ 1.1 KB of background: the oldest context dropped, the background kept (it counts)");

  group("an answer's own \"Wikipedia:\" line never goes back as context");
  GeminiTurnPair rd[3] = {
    { "is it out?", "No - it is due in 2027 (cut short)\n\nWikipedia: Tomb Raider: Legacy of Atlantis; Tomb Raider: Catalyst" },
    { "and the list?", "Wikipedia: this line is the model's own\n\nWikipedia: in the middle\nand more text after it" },
    { "who won?", "Spain.\nWikipedia: no blank line before it" },
  };
  b = build(rd, 3, "and the price?", 1024);
  ok(b.find("{\"role\":\"model\",\"parts\":[{\"text\":\"No - it is due in 2027 (cut short)\"}]}") != std::string::npos &&
     b.find("Catalyst") == std::string::npos, "the trailing sources line is cut off the model's turn");
  ok(b.find("Wikipedia: this line is the model's own\\n\\nWikipedia: in the middle\\nand more text after it") != std::string::npos,
     "a \"Wikipedia:\" that is not the LAST line is the answer's own: kept");
  ok(b.find("Spain.\\nWikipedia: no blank line before it") != std::string::npos,
     "...and one with no blank line before it");
}

// ── the system instruction, built per request ─────────────────────────────────────────────

static std::string sysText(int64_t utc, int tz, const char* zone) {
  const size_t n = geminiSystemText(utc, tz, zone, NULL, 0);
  std::string s(n + 1, '\0');
  const size_t n2 = geminiSystemText(utc, tz, zone, &s[0], n + 1);
  ok(n2 == n && strlen(s.c_str()) == n, "sys(NULL,0) is the exact length");
  s.resize(n);
  return s;
}

static bool hasNoCase(const std::string& s, const char* w) {
  std::string a(s), b(w);
  for (auto& c : a) c = (char)tolower((unsigned char)c);
  for (auto& c : b) c = (char)tolower((unsigned char)c);
  return a.find(b) != std::string::npos;
}

static void testSystemText() {
  group("the system instruction: today's date, then what to do with background (2026-10-11)");
  // 2026-10-11 21:05 UTC = 14:05 in Seattle (PDT, UTC-7), a Sunday
  std::string s = sysText(1791752700, -7 * 3600, "Pacific");
  printf("      \"%s\"\n", s.c_str());
  ok(s.find("Today is Sunday 11 October 2026, about 2:05 PM Pacific time (UTC-7). Your own knowledge ends "
            "earlier than today, so things may have changed since.") == 0,
     "the date and time FIRST (local, 12-hour, the zone's name and offset), then the knowledge cut-off");
  ok(s.find("When background from Wikipedia comes with a question, prefer it for anything recent and ignore "
            "it if it is about something else.") != std::string::npos,
     "...background from Wikipedia: preferred for anything recent, ignored when off-topic");
  ok(s.find("If you cannot confirm something recent, say so plainly; never present a guess as a fact.") !=
     std::string::npos, "...say so plainly; never a guess as a fact");
  ok(s.find("never present a guess as a fact. You are an assistant on a small handheld phone") != std::string::npos &&
     s.substr(s.size() - strlen(GEM_SYSTEM_INSTRUCTION)) == GEM_SYSTEM_INSTRUCTION,
     "...and the phone's own text (plain ASCII, short) LAST");
  ok(!hasNoCase(s, "tool") && !hasNoCase(s, "url") && !hasNoCase(s, "function") && !hasNoCase(s, "http") &&
     !hasNoCase(s, "search") && !hasNoCase(s, "browse"),
     "NO tool, URL, function, search or link named (an undeclared one came back MALFORMED_FUNCTION_CALL)");
  ok(plainAscii(s) && s.find('\n') == std::string::npos && noKey(s.c_str()),
     "plain ASCII on one line, no markdown characters, no key");
  ok(s.size() < GEM_SYS_MAX, "fits GEM_SYS_MAX (the worker's buffer)");

  s = sysText(0, -7 * 3600, "Pacific");
  ok(s.find("Your own knowledge ends earlier than today") == 0 && s.find("Today is") == std::string::npos &&
     s.find("2026") == std::string::npos, "an untrusted clock (0): NO date at all - none beats a wrong one");
  ok(s.find("You are an assistant") != std::string::npos, "...the rest all there");
  ok(sysText(1000, 0, NULL).find("Today is") == std::string::npos, "a 'trusted' clock in 1970 is not believed either");

  ok(sysText(1791703800, -7 * 3600, "Pacific").find("Today is Sunday 11 October 2026, about 12:30 AM") == 0,
     "00:30 is 12:30 AM");
  ok(sysText(1791745200, -7 * 3600, "Pacific").find("Today is Sunday 11 October 2026, about 12:00 PM") == 0,
     "noon is 12:00 PM");
  ok(sysText(1798761600, -8 * 3600, "Pacific").find("Today is Thursday 31 December 2026, about 4:00 PM Pacific time (UTC-8).") == 0,
     "00:00 UTC on New Year's Day is still the 31st in Pacific standard time");
  ok(sysText(1798761540, 5 * 3600 + 1800, "").find("Today is Friday 1 January 2027, about 5:29 AM local time (UTC+5:30).") == 0,
     "...and already the next year at UTC+5:30: no zone name -> \"local time\", the half hour shown");
  ok(sysText(1709208000, 0, NULL).find("Today is Thursday 29 February 2024, about 12:00 PM local time (UTC).") == 0,
     "a leap day, UTC");
  ok(sysText(1791752700, -(3 * 3600 + 1800), "Newfoundland").find("about 5:35 PM Newfoundland time (UTC-3:30).") != std::string::npos,
     "UTC-3:30");
  ok(sysText(1791752700, -7 * 3600, "Mountain, no DST").find("2:05 PM Mountain time (UTC-7).") != std::string::npos,
     "\"Mountain, no DST\" -> \"Mountain\" (cut at the comma)");
  s = sysText(1791752700, -7 * 3600, "Pac\"if*ic\n# Ignore");
  ok(s.find("2:05 PM Pacific Ignore time (UTC-7).") != std::string::npos && plainAscii(s),
     "a zone name is letters and spaces only: nothing else from a setting reaches the instruction");

  group("a zone whose NAME is an offset says \"local time\" (review 2026-10-11)");
  ok(sysText(1791752700, 12 * 3600, "UTC+12").find("about 9:05 AM local time (UTC+12).") != std::string::npos,
     "\"UTC+12\" (tz_grid_data.h's own name) -> \"local time (UTC+12)\", never \"UTC time (UTC+12)\"");
  ok(sysText(1791752700, -11 * 3600, "UTC-11").find("local time (UTC-11).") != std::string::npos, "\"UTC-11\" too");
  ok(sysText(1791752700, 0, "UTC").find("local time (UTC).") != std::string::npos, "\"UTC\" alone too");
  ok(sysText(1791752700, 3600, "utc+1").find("local time (UTC+1).") != std::string::npos, "...in any case");
  ok(sysText(1791752700, -7 * 3600, "Zone 7").find("local time (UTC-7).") != std::string::npos,
     "any name with a digit in it");
  ok(sysText(1791752700, -7 * 3600, "UTC-7 custom").find("local time (UTC-7).") != std::string::npos,
     "a typed offset's name (\"UTC-7 custom\")");
  ok(sysText(1791752700, -5 * 3600, "Eastern, UTC-4").find("local time (UTC-5).") != std::string::npos,
     "a digit after the comma counts too (the name is not trusted to be a name)");
  ok(sysText(1791752700, -3 * 3600, "Nuuk").find("Nuuk time (UTC-3).") != std::string::npos &&
     sysText(1791752700, -10 * 3600, "Hawaii").find("Hawaii time (UTC-10).") != std::string::npos,
     "real names are kept (\"Nuuk\", \"Hawaii\")");

  // Wednesday 30 September 2026, 12:59 PM at UTC+13:30, a zone name past the 23 kept
  std::string longest = sysText(1790724540, 13 * 3600 + 1800, "Abcdefghijklmnopqrstuvwxyz");
  ok(longest.find("Today is Wednesday 30 September 2026, about 12:59 PM Abcdefghijklmnopqrstuvw time (UTC+13:30).") == 0,
     "the longest day and month, a 2-digit day and hour, a long name cut to 23");
  ok(longest.size() < GEM_SYS_MAX, "the longest form fits GEM_SYS_MAX");
  printf("      longest: %u of %u bytes\n", (unsigned)longest.size(), (unsigned)GEM_SYS_MAX);
  std::string b = build(NULL, 0, "Is Legacy of Atlantis out yet?", 1024, NULL,
                        sysText(1791752700, -7 * 3600, "Pacific").c_str());
  ok(b.find("{\"system_instruction\":{\"parts\":[{\"text\":\"Today is Sunday 11 October 2026") == 0 &&
     b.find("\"tools\"") == std::string::npos, "the body carries the built instruction - and no tool");
}

// ── the answer ───────────────────────────────────────────────────────────────────────────

static void testReply() {
  group("Google's answers (real shapes)");
  GeminiReply r;
  ok(reply(R_TEXT, &r) == GEM_REPLY_TEXT && !strcmp(g_text, "Paris is the capital of France.\nIt sits on the Seine.") &&
     !strcmp(r.finish, "STOP"), "200 with text (pretty-printed, usageMetadata skipped)");
  ok(reply(R_MAXTOK, &r) == GEM_REPLY_EMPTY && !strcmp(r.finish, "MAX_TOKENS") && g_text[0] == '\0',
     "200, MAX_TOKENS, no text (the thoughts ate the budget)");
  char msg[200];
  geminiNoTextMessage(&r, msg, sizeof(msg));
  ok(strstr(msg, "MAX_TOKENS") != NULL, "...and says why");
  ok(reply(R_THOUGHT, &r) == GEM_REPLY_TEXT && !strcmp(g_text, "Two plus two is four."),
     "thought parts skipped (before and after the text key), the rest joined");
  ok(reply(R_400_THINK, &r) == GEM_REPLY_ERROR && r.errCode == 400 && !strcmp(r.errStatus, "INVALID_ARGUMENT") &&
     !strcmp(r.errMessage, "Request contains an invalid argument.") && !geminiLooksLikeKeyError(&r),
     "400 INVALID_ARGUMENT (flash-lite and thinkingConfig) is not a key error");
  ok(reply(R_400_KEY, &r) == GEM_REPLY_ERROR && !strcmp(r.errReason, "API_KEY_INVALID") &&
     geminiLooksLikeKeyError(&r), "400 API_KEY_INVALID is a key error");
  ok(reply(R_404, &r) == GEM_REPLY_ERROR && r.errCode == 404 && !strcmp(r.errStatus, "NOT_FOUND"), "404");
  ok(reply(R_503, &r) == GEM_REPLY_ERROR && r.errCode == 503 && !strcmp(r.errStatus, "UNAVAILABLE"), "503");
  ok(reply(R_429_DAY, &r) == GEM_REPLY_ERROR && r.errCode == 429 && r.quotaPerDay && r.retryDelayS == 22517,
     "429 per DAY: the QuotaFailure's PerDay quotaId and RetryInfo 22517s");
  ok(reply(R_429_MIN, &r) == GEM_REPLY_ERROR && !r.quotaPerDay && r.retryDelayS == 38,
     "429 per MINUTE: retryDelay 37.2s rounds UP to 38");
  ok(reply(R_429_BARE, &r) == GEM_REPLY_ERROR && !r.quotaPerDay && r.retryDelayS == -1,
     "429 that says neither");
  ok(reply(R_BLOCK, &r) == GEM_REPLY_BLOCKED && !strcmp(r.block, "PROHIBITED_CONTENT"), "promptFeedback.blockReason");
  geminiNoTextMessage(&r, msg, sizeof(msg));
  ok(!strcmp(msg, "Gemini declined to answer (PROHIBITED_CONTENT)"), "...declined, with the reason");
  ok(reply(R_SAFETY, &r) == GEM_REPLY_EMPTY && !strcmp(r.finish, "SAFETY"), "SAFETY, no content");
  geminiNoTextMessage(&r, msg, sizeof(msg));
  ok(!strcmp(msg, "Gemini declined to answer (SAFETY)"), "...declined (SAFETY)");
  ok(reply(R_UNICODE, &r) == GEM_REPLY_TEXT &&
     !strcmp(g_text, "Caf\xC3\xA9 \xE2\x80\x94 \"quoted\" \xF0\x9F\x98\x80 end\nline2 \xC2\xBD / \\ back\tslash!"),
     "\\uXXXX, a surrogate pair to 4-byte UTF-8, \\/ \\\\ \\t; \\u0000 dropped");
  ok(geminiParseReply(R_TEXT, strlen(R_TEXT) - 20, g_text, sizeof(g_text), &r) == GEM_REPLY_BAD,
     "a body cut short is BAD, not a half answer");
  ok(reply("<html>502 Bad Gateway</html>", &r) == GEM_REPLY_BAD && reply("", &r) == GEM_REPLY_BAD,
     "not JSON");
  ok(reply("{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"a\\q\"}]}}]}", &r) == GEM_REPLY_BAD,
     "a bad escape");
  char tiny[8];
  ok(geminiParseReply(R_TEXT, strlen(R_TEXT), tiny, sizeof(tiny), &r) == GEM_REPLY_TEXT &&
     r.textTruncated && !strcmp(tiny, "Paris i"), "text longer than the buffer: cut, and flagged");
  char tiny2[4];
  geminiParseReply(R_UNICODE, strlen(R_UNICODE), tiny2, sizeof(tiny2), &r);
  ok(!strcmp(tiny2, "Caf"), "a cut never splits a UTF-8 sequence");
  group("MALFORMED_FUNCTION_CALL with no text (3.8 Flash, 2026-10-11)");
  ok(reply(R_MALFORMED, &r) == GEM_REPLY_EMPTY && !strcmp(r.finish, "MALFORMED_FUNCTION_CALL") && g_text[0] == '\0',
     "an EMPTY reply (\"content\":{} - no parts), the finishReason kept");
  geminiNoTextMessage(&r, msg, sizeof(msg));
  ok(!strcmp(msg, "Gemini sent an empty answer - ask again"),
     "...an empty answer, not \"declined\" - no tool is sent, the model stumbled");
  ok(reply("{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Half\"}]},\"finishReason\":\"MALFORMED_FUNCTION_CALL\"}]}",
           &r) == GEM_REPLY_TEXT && !strcmp(g_text, "Half"), "...but WITH text it is an answer (kept)");
}

static std::string clean(const char* in, size_t cap = 4096, bool* t = NULL) {
  std::string out(cap, '\0');
  bool tr = false;
  const size_t n = geminiCleanText(in, &out[0], cap, &tr);
  if (t) *t = tr;
  out.resize(n);
  return out;
}

/* geminiFoldNext over a whole string: what the table makes of it, character by character. */
static std::string fold(const char* in) {
  std::string out;
  const char* s = in;
  char one[2];
  for (const char* rep; (rep = geminiFoldNext(&s, one)) != NULL;) out += rep;
  return out;
}

static void testClean() {
  group("the screen's ASCII");
  ok(clean("# Title\n\n**Bold** and __under__ and `code`.") == "Title\n\nBold and under and code.",
     "a heading, bold, underline, backticks");
  ok(clean("Steps:\n* one\n- two\n+ three\n  * nested **bold** item") ==
     "Steps:\n- one\n- two\n- three\n  - nested bold item", "bullets to '- ', nested kept indented");
  ok(clean("Before\n```python\nx = a__b  # keep\n```\nAfter") == "Before\nx = a__b  # keep\nAfter",
     "a code fence: the fence lines go, the code is kept as it is");
  ok(clean("Para one.\n\n\n\n\nPara two.\n\n") == "Para one.\n\nPara two.", "blank runs to one, ends trimmed");
  ok(clean("\xE2\x80\x9CQuote\xE2\x80\x9D \xE2\x80\x94 it\xE2\x80\x99s 20\xC2\xB0" "C\xE2\x80\xA6 caf\xC3\xA9 "
           "na\xC3\xAFve \xC3\x85ngstr\xC3\xB6m \xC3\x9F") ==
     "\"Quote\" - it's 20 degC... cafe naive Angstrom ss", "curly quotes, dashes, degree, ellipsis, accents");
  ok(clean("Smile \xF0\x9F\x98\x80 ok \xE2\x9C\x85 done") == "Smile  ok  done", "emoji dropped");
  ok(clean("a\tb\rc\x01" "d\x7F" "e") == "a bcde", "tab to space; CR, controls and DEL out");
  ok(clean("\xE2\x80\xA2 bullet\n\xC2\xA0\xC2\xA0x \xC2\xBD") == "- bullet\n  x 1/2", "a bullet character, NBSP, a half");
  ok(clean("> quoted line\n>> more") == "quoted line\n> more", "a quote marker");
  ok(clean("2*3 = 6 and *emphasis* kept") == "2*3 = 6 and *emphasis* kept",
     "single asterisks are left alone (arithmetic)");
  ok(clean("bad \xC3 seq \xE2\x80 x \x80 y") == "bad  seq  x  y", "broken UTF-8 skipped");
  bool t = false;
  std::string c = clean("abcdefghij", 6, &t);
  ok(c == "abcde" && t, "truncation flagged");

  group("geminiFoldNext: the table, one character at a time (the shared fold, 2026-10-11)");
  ok(fold("Navier\xE2\x80\x93Stokes (1959\xE2\x80\x94" "2015)") == "Navier-Stokes (1959-2015)",
     "en and em dashes: '-' (the keywords then make it a separator)");
  ok(fold("Gau\xC3\x9F Stra\xC3\x9F" "e") == "Gauss Strasse", "\xC3\x9F (sharp s): 'ss'");
  ok(fold("\xC3\x98resund \xC3\xB8 \xC5\x81\xC3\xB3" "d\xC5\xBA \xC5\x82") == "Oresund o Lodz l",
     "O/o with a stroke: 'O'/'o'; L/l with a stroke: 'L'/'l'; acute accents: the base letter");
  ok(fold("\xC3\x86sir \xC3\xA6 \xC5\x92uvre \xC5\x93") == "AEsir ae OEuvre oe", "ligatures: two letters");
  ok(fold("e=mc\xC2\xB2 x\xC2\xB3 \xC2\xB9") == "e=mc^2 x^3 ^1", "superscripts: '^' and the digit, as the table spells them");
  ok(fold("Pok\xC3\xA9mon\xE2\x84\xA2 Baldur\xE2\x80\x99s") == "Pokemon(TM) Baldur's", "a trade mark, a curly apostrophe");
  ok(fold("zel\xF0\x9F\x98\x80" "da so\xC2\xAD" "ft z\xE2\x80\x8Bw") == "zelda soft zw",
     "what the table does not know (an emoji) and its \"\" entries (soft hyphen, zero-width space): dropped");
  ok(fold("a\tb\x01" "c\n") == "a\tb\x01" "c\n", "ASCII as it is - control characters too (the caller decides)");
  ok(fold("bad \xC3 seq \xE2\x80 x \x80 y \xC3") == "bad  seq  x  y ", "broken UTF-8: the lead byte or stray byte dropped");
  {
    const char* s = "";
    char one[2];
    ok(geminiFoldNext(&s, one) == NULL && geminiFoldNext(&s, one) == NULL && *s == '\0', "NULL at the end, and after it");
    /* The fold at most DOUBLES a character's bytes (" deg" for the 2 of U+00B0): the bound
     * wiki_lookup.cpp's relevance buffers (2 * WIKI_TITLE_MAX + 4) are sized on. */
    bool bound = true, ascii = true;
    for (uint32_t cp = 0x80; cp <= 0xFFFF; cp++) {
      if (cp >= 0xD800 && cp <= 0xDFFF) continue;
      char u[4];
      size_t n = 0;
      if (cp < 0x800) { u[n++] = (char)(0xC0 | (cp >> 6)); }
      else { u[n++] = (char)(0xE0 | (cp >> 12)); u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); }
      u[n++] = (char)(0x80 | (cp & 0x3F));
      u[n] = '\0';
      const char* p = u;
      const char* rep = geminiFoldNext(&p, one);
      bound = bound && rep && p == u + n && strlen(rep) <= 2 * n;
      for (const char* q = rep ? rep : ""; *q; q++) ascii = ascii && (unsigned char)*q >= 0x20 && (unsigned char)*q < 0x7F;
    }
    ok(bound, "every BMP character: its fold is at most twice its UTF-8 bytes, the cursor past it");
    ok(ascii, "...and printable ASCII");
  }

  GeminiReply r;
  memset(&r, 0, sizeof(r));
  strcpy(r.finish, "MAX_TOKENS");
  char out[64];
  geminiAnswerText(&r, "A long **answer**", NULL, out, sizeof(out));
  ok(!strcmp(out, "A long answer (cut short)"), "MAX_TOKENS with text: kept, marked cut short");
  strcpy(r.finish, "STOP");
  std::string longText(200, 'w');
  geminiAnswerText(&r, longText.c_str(), NULL, out, sizeof(out));
  ok(strlen(out) == sizeof(out) - 1 && strstr(out, " (cut short)"), "over the cap: cut to fit WITH the note");
  ok(geminiAnswerText(&r, "  \n ``` \n", NULL, out, sizeof(out)) == 0 && out[0] == '\0', "nothing left: empty");
  ok(geminiAnswerText(&r, "  \n ``` \n", "Wikipedia: A", out, sizeof(out)) == 0 && out[0] == '\0',
     "...even with a sources line: no answer, no line on its own");

  group("the sources line under an answer that went with background");
  const char* const SRC = "Wikipedia: Tomb Raider: Legacy of Atlantis; Tomb Raider: Catalyst";
  char a[GEM_A_MAX];
  memset(&r, 0, sizeof(r));
  strcpy(r.finish, "STOP");
  geminiAnswerText(&r, "Not yet - it is due in **2027**.", SRC, a, sizeof(a));
  ok(!strcmp(a, "Not yet - it is due in 2027.\n\nWikipedia: Tomb Raider: Legacy of Atlantis; Tomb Raider: Catalyst"),
     "the cleaned answer, a blank line, the sources line");
  ok(std::string(a).find(GEM_SRC_SEP) != std::string::npos, "...joined by GEM_SRC_SEP (what the context strip finds)");
  geminiAnswerText(&r, "Plain.", NULL, a, sizeof(a));
  ok(!strcmp(a, "Plain."), "no background (NULL): no line");
  geminiAnswerText(&r, "Plain.", "", a, sizeof(a));
  ok(!strcmp(a, "Plain."), "...nor for \"\"");
  strcpy(r.finish, "MAX_TOKENS");
  std::string huge(GEM_A_MAX * 2, 'w');
  const size_t an = geminiAnswerText(&r, huge.c_str(), SRC, a, sizeof(a));
  const std::string as(a);
  const std::string tail = std::string("\n\n") + SRC;
  ok(an == strlen(a) && an < sizeof(a) && an == sizeof(a) - 1 && as.size() > tail.size() &&
     as.compare(as.size() - tail.size(), tail.size(), tail) == 0 &&
     as.find(" (cut short)\n\nWikipedia: ") != std::string::npos,
     "an answer over the cap: the ANSWER is cut (and says so), the sources line kept whole");
  char tiny[24];
  geminiAnswerText(&r, "Short answer.", SRC, tiny, sizeof(tiny));
  ok(!strcmp(tiny, "Short answe (cut short)"), "a buffer too small for both: the answer (cut), never the line");
  {
    // The saved answer goes back as context WITHOUT its sources line (the round trip).
    strcpy(r.finish, "STOP");
    geminiAnswerText(&r, "Spain won it.", "Wikipedia: 2026 FIFA World Cup", a, sizeof(a));
    GeminiTurnPair rt[1] = { { "who won the 2026 world cup", a } };
    const size_t n = geminiBuildRequest(rt, 1, "and where?", NULL, "S", -1, NULL, 0);
    std::string body(n + 1, '\0');
    geminiBuildRequest(rt, 1, "and where?", NULL, "S", -1, &body[0], n + 1);
    ok(body.find("{\"role\":\"model\",\"parts\":[{\"text\":\"Spain won it.\"}]}") != std::string::npos &&
       body.find("FIFA") == std::string::npos, "shown with the line, sent back without it (geminiAnswerText -> context)");
  }
}

// ── the chunked body ─────────────────────────────────────────────────────────────────────

static void testDechunk() {
  group("Transfer-Encoding: chunked (every Gemini answer)");
  const char* raw = "19\r\n{\"candidates\":[{\"content\"\r\n7;ext=1\r\n:{}}]}\n\r\n0\r\nX-Trailer: 1\r\n\r\n";
  const char* want = "{\"candidates\":[{\"content\":{}}]}\n";
  char out[128];
  size_t n = 0;
  GeminiDechunk d;
  geminiDechunkInit(&d);
  ok(geminiDechunkFeed(&d, (const uint8_t*)raw, strlen(raw), out, sizeof(out), &n) && d.done &&
     n == strlen(want) && !memcmp(out, want, n), "whole: two chunks, an extension, a trailer");
  geminiDechunkInit(&d);
  n = 0;
  bool good = true;
  for (size_t i = 0; raw[i]; i++) {
    good = good && geminiDechunkFeed(&d, (const uint8_t*)raw + i, 1, out, sizeof(out), &n);
  }
  ok(good && d.done && n == strlen(want) && !memcmp(out, want, n), "byte by byte: the same");
  geminiDechunkInit(&d);
  n = 0;
  const char* lf = "5\nhello\n0\n\n";
  ok(geminiDechunkFeed(&d, (const uint8_t*)lf, strlen(lf), out, sizeof(out), &n) && d.done && n == 5,
     "bare LF line ends (lenient)");
  geminiDechunkInit(&d);
  n = 0;
  ok(!geminiDechunkFeed(&d, (const uint8_t*)"zz\r\n", 4, out, sizeof(out), &n) && d.error, "not chunked: error");
  geminiDechunkInit(&d);
  n = 0;
  ok(!geminiDechunkFeed(&d, (const uint8_t*)"3\r\nabcX", 7, out, sizeof(out), &n) && d.error,
     "a chunk longer than it said: error");
  geminiDechunkInit(&d);
  n = 0;
  const char* big = "a\r\n0123456789\r\n0\r\n\r\n";
  ok(geminiDechunkFeed(&d, (const uint8_t*)big, strlen(big), out, 4, &n) && d.done && d.overflow && n == 4,
     "over the sink's cap: kept to the cap, decoded to the end, flagged");
  geminiDechunkInit(&d);
  n = 0;
  ok(!geminiDechunkFeed(&d, (const uint8_t*)"fffffffff\r\n", 11, out, sizeof(out), &n), "an absurd size: error");
}

// ── the ladder ───────────────────────────────────────────────────────────────────────────

struct Ladder {
  GeminiConfig  cfg;
  GeminiSession s;
  GeminiQuota   q;
  GeminiLadder  l;
  uint32_t      delay;
  const char*   stage;
};

static void ladderInit(Ladder* L, const char* extra = "") {
  std::string f = std::string("key=") + FAKE_KEY + "\n" + extra;
  geminiParseConfig(f.c_str(), f.size(), &L->cfg);
  geminiSessionInit(&L->s);
  geminiQuotaClear(&L->q);
}

static int step(Ladder* L, int code, const char* body, int64_t now = 1700000000, uint32_t ms = 1000,
                int net = GEM_NET_OK) {
  GeminiReply r;
  if (body) {
    geminiParseReply(body, strlen(body), g_text, sizeof(g_text), &r);
  } else {
    memset(&r, 0, sizeof(r));
    r.retryDelayS = -1;
  }
  return geminiLadderNext(&L->l, &L->s, &L->q, code, net, &r, now, ms, &L->delay, &L->stage);
}

/* An attempt with no HTTP answer at all: `net` says why (as ai_net.cpp's postOnce does). */
static int noAnswer(Ladder* L, int net, int64_t now = 1700000000, uint32_t ms = 1000) {
  return step(L, 0, NULL, now, ms, net);
}

static std::string failText(Ladder* L, int code, const char* body, int net = GEM_NET_OK, int tz = 0) {
  GeminiReply r;
  if (body) {
    geminiParseReply(body, strlen(body), g_text, sizeof(g_text), &r);
  } else {
    memset(&r, 0, sizeof(r));
  }
  char out[300];
  geminiFailText(code, net, body ? &r : NULL, L ? &L->l : NULL, FAKE_KEY, tz, out, sizeof(out));
  return out;
}

static void testLadder() {
  group("the model ladder (a loop, never recursion)");
  Ladder L;
  ladderInit(&L);
  ok(geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0) == GEM_STEP_RETRY &&
     !strcmp(L.l.model, GEM_DEFAULT_MODEL) && L.l.budget == GEM_THINKING, "starts on flash-latest, thinking 1024");
  ok(step(&L, 200, R_TEXT) == GEM_STEP_DONE, "200: done");

  group("rule 1: a model that rejects thinkingConfig");
  ladderInit(&L, "model=gemini-new-thinker\n");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 400, R_400_THINK) == GEM_STEP_RETRY && L.l.budget == -1 && L.delay == 0 &&
     !strcmp(L.l.model, "gemini-new-thinker"), "400 INVALID_ARGUMENT with thinking: once WITHOUT it");
  ok(geminiSessionNoThink(&L.s, "gemini-new-thinker"), "...and remembered for the session");
  ok(step(&L, 400, R_400_THINK) == GEM_STEP_FAIL, "a second 400: fail (no loop)");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(L.l.budget == -1, "the next question never sends it to that model");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 400, R_400_KEY) == GEM_STEP_FAIL, "a KEY 400 is not retried");
  std::string ft = failText(&L, 400, R_400_KEY);
  ok(ft.find("Key rejected - check API Keys/gemini.txt") == 0 && ft.find("API key not valid") != std::string::npos,
     "...'Key rejected', WITH Google's own words");
  ok(failText(&L, 400, R_400_THINK).find("Gemini error 400: Request contains an invalid argument") == 0,
     "a non-key 400 is NOT called a bad key");
  ladderInit(&L, "model=gemini-flash-lite-latest\n");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(L.l.budget == -1, "flash-lite is never sent thinkingConfig (measured: 400)");

  group("rule 0: MALFORMED_FUNCTION_CALL and no text - once more, the same model (2026-10-11)");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 200, R_MALFORMED) == GEM_STEP_RETRY && !strcmp(L.l.model, GEM_DEFAULT_MODEL) &&
     L.l.budget == GEM_THINKING && L.delay == 0 && !L.l.onFallback && L.stage &&
     !strcmp(L.stage, "Empty answer - asking again..."),
     "a 200 with no text and MALFORMED_FUNCTION_CALL: asked again at once, the same model, thinking kept");
  ok(L.l.attempts == 1, "...it counted as an attempt");
  ok(step(&L, 200, R_TEXT) == GEM_STEP_DONE && L.l.attempts == 2, "...then the answer");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 200, R_MALFORMED) == GEM_STEP_RETRY, "a new question: the rule is there again (once a QUESTION)");
  ok(step(&L, 200, R_MALFORMED) == GEM_STEP_DONE && L.l.attempts == 2,
     "a second one: DONE - the caller shows it as an empty answer, no third request");
  {
    GeminiReply r;
    reply(R_MALFORMED, &r);
    char msg[120];
    geminiNoTextMessage(&r, msg, sizeof(msg));
    ok(!strcmp(msg, "Gemini sent an empty answer - ask again"), "...\"Gemini sent an empty answer - ask again\"");
  }
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 200, R_SAFETY) == GEM_STEP_DONE && step(&L, 200, R_MAXTOK) == GEM_STEP_DONE,
     "SAFETY / MAX_TOKENS with no text are not rule 0: DONE as before (no retry)");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 200, "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Half\"}]},\"finishReason\":"
                   "\"MALFORMED_FUNCTION_CALL\"}]}") == GEM_STEP_DONE, "MALFORMED WITH text: an answer, DONE");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  for (int i = 0; i < 3; i++) step(&L, 503, R_503);
  ok(L.l.onFallback && step(&L, 200, R_MALFORMED) == GEM_STEP_RETRY && L.l.onFallback &&
     !strcmp(L.l.model, GEM_LITE_MODEL) && L.l.budget == -1,
     "on the fallback: asked again on the fallback, still no thinkingConfig");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  L.l.attempts = GEM_MAX_ATTEMPTS - 1;
  ok(step(&L, 200, R_MALFORMED) == GEM_STEP_DONE, "...but never past GEM_MAX_ATTEMPTS");
  ok(geminiLadderNext(&L.l, &L.s, &L.q, 200, GEM_NET_OK, NULL, 1700000000, 0, &L.delay, &L.stage) == GEM_STEP_DONE,
     "a 200 with no parsed reply (NULL): DONE");

  group("rule 2: 404");
  ladderInit(&L, "model=gemini-9-flash\n");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 404, R_404) == GEM_STEP_RETRY && !strcmp(L.l.model, GEM_DEFAULT_MODEL), "404 -> once with flash-latest");
  ok(step(&L, 404, R_404) == GEM_STEP_FAIL, "a second 404: fail");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 404, R_404) == GEM_STEP_FAIL, "404 on flash-latest itself: fail");

  group("rule 5: 503 - two retries, then flash-lite with its own two");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 503, R_503) == GEM_STEP_RETRY && L.delay == 2500 && !L.l.onFallback, "503 #1: 2.5 s");
  ok(step(&L, 503, R_503) == GEM_STEP_RETRY && L.delay == 2500 && !L.l.onFallback, "503 #2: 2.5 s");
  ok(step(&L, 503, R_503) == GEM_STEP_RETRY && L.l.onFallback && !strcmp(L.l.model, GEM_LITE_MODEL) &&
     L.l.budget == -1 && L.stage && strstr(L.stage, "faster"), "503 #3: flash-lite, no thinkingConfig");
  ok(step(&L, 503, R_503) == GEM_STEP_RETRY && step(&L, 503, R_503) == GEM_STEP_RETRY &&
     step(&L, 503, R_503) == GEM_STEP_FAIL, "flash-lite's own two, then fail");
  ok(failText(&L, 503, R_503) == "Google is busy right now - try again in a minute", "...'Google is busy'");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 503, R_503) == GEM_STEP_RETRY && step(&L, 200, R_TEXT) == GEM_STEP_DONE, "a retry that works");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(!strcmp(L.l.model, GEM_DEFAULT_MODEL), "NOT sticky: the next question is on flash-latest again");
  ladderInit(&L, "fallback=none\n");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 503, R_503) == GEM_STEP_RETRY && step(&L, 503, R_503) == GEM_STEP_RETRY &&
     step(&L, 503, R_503) == GEM_STEP_FAIL, "fallback=none: two retries and done");

  group("rule 5: EVERY 5xx is busy - 500, 502, 504 the same ladder as 503 (shared, 2026-10-11)");
  {
    const struct { int code; const char* body; const char* what; } FIVE[] = {
      { 500, R_500, "500 INTERNAL" },
      { 502, R_502, "502 (a proxy's HTML page, no JSON)" },
      { 504, R_504, "504 DEADLINE_EXCEEDED" },
    };
    for (const auto& f : FIVE) {
      ladderInit(&L);
      geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
      const bool r1 = step(&L, f.code, f.body) == GEM_STEP_RETRY && L.delay == 2500 && !L.l.onFallback &&
                      L.stage && !strcmp(L.stage, "Google is busy - trying again...");
      const bool r2 = step(&L, f.code, f.body) == GEM_STEP_RETRY && L.delay == 2500 && !L.l.onFallback;
      const bool fb = step(&L, f.code, f.body) == GEM_STEP_RETRY && L.l.onFallback &&
                      !strcmp(L.l.model, GEM_LITE_MODEL) && L.l.budget == -1 && L.delay == GEM_LITE_DELAY_MS &&
                      L.stage && !strcmp(L.stage, "Busy - trying the faster model...");
      const bool own = step(&L, f.code, f.body) == GEM_STEP_RETRY && step(&L, f.code, f.body) == GEM_STEP_RETRY &&
                       step(&L, f.code, f.body) == GEM_STEP_FAIL && !L.l.allOut && L.l.attempts == 6;
      std::string what = std::string(f.what) + ": two retries 2.5 s apart, flash-lite (no thinkingConfig) with its own two, then fail";
      ok(r1 && r2 && fb && own, what.c_str());
      ladderInit(&L);
      geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
      what = std::string(f.what) + ": a retry that works is DONE";
      ok(step(&L, f.code, f.body) == GEM_STEP_RETRY && step(&L, 200, R_TEXT) == GEM_STEP_DONE, what.c_str());
    }
  }
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 500, R_500) == GEM_STEP_RETRY && step(&L, 504, R_504) == GEM_STEP_RETRY &&
     step(&L, 503, R_503) == GEM_STEP_RETRY && L.l.onFallback, "mixed 5xx share ONE count: 500, 504, 503 -> the fallback");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 501, NULL) == GEM_STEP_RETRY && (geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0),
     step(&L, 599, NULL)) == GEM_STEP_RETRY, "the whole range: 501 and 599 too");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  ok(step(&L, 499, NULL) == GEM_STEP_FAIL && (geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0),
     step(&L, 600, NULL)) == GEM_STEP_FAIL, "...and only it: 499 and 600 fail at once");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  for (int i = 0; i < 6; i++) step(&L, 504, R_504);
  ok(!geminiQuotaOut(&L.q, GEM_DEFAULT_MODEL, 1700000000, 1000) && !geminiQuotaOut(&L.q, GEM_LITE_MODEL, 1700000000, 1000),
     "a 5xx is never booked as a daily limit");
  ok(failText(&L, 504, R_504) == "Gemini error 504: Deadline expired before operation could complete.",
     "...and when the ladder gives up, Google's own words ('Gemini error 504: ...')");
  ok(failText(&L, 500, R_500).find("Gemini error 500: An internal error has occurred") == 0, "...('Gemini error 500: ...')");

  group("rule 3: 429 for the DAY (the free tier's 20 a day)");
  ladderInit(&L);
  const int64_t now = 1700000000;
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 5000);
  ok(step(&L, 429, R_429_DAY, now, 5000) == GEM_STEP_RETRY && L.delay == 0 && L.l.onFallback &&
     !strcmp(L.l.model, GEM_LITE_MODEL) && L.l.quotaSwitch, "no retry: straight to flash-lite");
  ok(geminiQuotaOut(&L.q, GEM_DEFAULT_MODEL, now + 60, 65000) &&
     geminiQuotaUntil(&L.q, GEM_DEFAULT_MODEL, now + 60, 65000) == now + 22517,
     "flash-latest remembered as used up until now + retryDelay");
  ok(step(&L, 200, R_TEXT, now, 6000) == GEM_STEP_DONE, "flash-lite answers");
  ok(geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now + 3600, 0) == GEM_STEP_RETRY && L.l.onFallback &&
     L.l.quotaSwitch && L.l.primaryUntil == now + 22517 && L.l.primaryRemainS == 22517 - 3600,
     "an hour later: the question goes STRAIGHT to flash-lite");
  ok(geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now + 22517 + 1, 0) == GEM_STEP_RETRY && !L.l.onFallback,
     "after Google's deadline: flash-latest again");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now + 100, 0);
  ok(step(&L, 429, R_429_DAY, now + 100, 0) == GEM_STEP_FAIL && L.l.allOut, "flash-lite out for the day too: fail");
  // now = 2023-11-14 22:13:20 UTC; flash comes back first, at 04:28:37 UTC = 21:28:37 at UTC-7.
  ok(L.l.backUntil == now + 22517, "...the first model back is the deadline shown");
  ok(failText(&L, 429, R_429_DAY, GEM_NET_OK, -7 * 3600) ==
     "Today's free Gemini limit is used up - try again after about 21:29", "...'used up', with Google's deadline");
  ok(failText(&L, 429, R_429_DAY, GEM_NET_OK, 0) ==
     "Today's free Gemini limit is used up - try again after about 04:29 tomorrow", "...'tomorrow' when it is");
  ok(geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now + 200, 0) == GEM_STEP_FAIL && L.l.allOut &&
     failText(&L, 0, NULL, GEM_NET_OK, -7 * 3600) ==
     "Today's free Gemini limit is used up - try again after about 21:29", "the next question asks nobody");
  // An unknown clock: remembered on the boot's ms clock instead.
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 0, 1000);
  ok(step(&L, 429, R_429_DAY, 0, 1000) == GEM_STEP_RETRY && L.l.onFallback, "no clock: still to flash-lite");
  ok(geminiQuotaOut(&L.q, GEM_DEFAULT_MODEL, 0, 1000 + 3600000u) &&
     !geminiQuotaOut(&L.q, GEM_DEFAULT_MODEL, 0, 1000 + 22518000u), "...remembered on the boot clock");
  ok(L.l.primaryUntil == 0 && L.l.primaryRemainS == 22517, "...as a duration, not a time of day");
  ok(step(&L, 429, R_429_DAY, 0, 2000) == GEM_STEP_FAIL && L.l.allOut &&
     failText(&L, 429, R_429_DAY) == "Today's free Gemini limit is used up - try again in about 6 h",
     "both used up, no clock: 'in about 6 h'");
  // No RetryInfo: remembered for an hour, and NO time is invented for the screen.
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 1000);
  ok(step(&L, 429, R_429_DAY_NOTIME, now, 1000) == GEM_STEP_RETRY && L.l.onFallback && L.l.quotaSwitch &&
     L.l.primaryUntil == 0 && L.l.primaryRemainS == 0, "a PerDay 429 with no RetryInfo: flash-lite, no deadline");
  ok(geminiQuotaOut(&L.q, GEM_DEFAULT_MODEL, now + GEM_DAY_GUESS_S - 1, 0) &&
     !geminiQuotaOut(&L.q, GEM_DEFAULT_MODEL, now + GEM_DAY_GUESS_S + 1, 0), "...still remembered (the estimate)");
  ok(geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now + 60, 61000) == GEM_STEP_RETRY && L.l.onFallback &&
     L.l.primaryUntil == 0 && L.l.primaryRemainS == 0, "...the next question too: 'for now'");
  ok(step(&L, 429, R_429_DAY, now + 60, 61000) == GEM_STEP_FAIL && L.l.allOut &&
     failText(&L, 429, R_429_DAY, GEM_NET_OK, -7 * 3600) ==
     "Today's free Gemini limit is used up - try again later", "both used up, one a guess: 'later'");

  group("rule 4: 429 per MINUTE - wait it out once, then flash-lite");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  ok(step(&L, 429, R_429_MIN) == GEM_STEP_RETRY && L.delay == 38000 && !L.l.onFallback,
     "wait 38 s (Google's retryDelay), same model");
  ok(!geminiQuotaOut(&L.q, GEM_DEFAULT_MODEL, now, 0), "...and NOT remembered as used up");
  ok(step(&L, 429, R_429_MIN) == GEM_STEP_RETRY && L.l.onFallback, "again: flash-lite");
  ok(step(&L, 429, R_429_MIN) == GEM_STEP_RETRY && L.delay == 38000, "flash-lite waits once too");
  ok(step(&L, 429, R_429_MIN) == GEM_STEP_FAIL, "then fail");
  ok(failText(&L, 429, R_429_MIN) == "Gemini is busy or the free quota is used up - try again later",
     "...'busy or the free quota is used up'");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  ok(step(&L, 429, R_429_BARE) == GEM_STEP_RETRY && L.delay == 2500, "a bare 429: the busy rule");

  group("rule 6: \"Retry if it times out\" (round 3) - once more, then the fallback once");
  ladderInit(&L);
  ok(geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0) == GEM_STEP_RETRY && L.l.retryTimeouts,
     "the ladder starts with it ON (the setting's default)");
  ok(noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_RETRY && !strcmp(L.l.model, GEM_DEFAULT_MODEL) &&
     L.l.budget == GEM_THINKING && !L.l.onFallback && L.delay == 0 && L.stage &&
     !strcmp(L.stage, "No answer in time - trying again (2 of 2)..."),
     "a timeout: the SAME model, thinking kept, at once - \"No answer in time - trying again (2 of 2)...\"");
  ok(noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_RETRY && L.l.onFallback && !strcmp(L.l.model, GEM_LITE_MODEL) &&
     L.l.budget == -1 && L.delay == 0 && L.stage && !strcmp(L.stage, "Asking Flash-Lite instead..."),
     "a second: the fallback (no thinkingConfig), at once - \"Asking Flash-Lite instead...\" stays up");
  ok(noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_FAIL && L.l.attempts == 3 && !L.l.allOut,
     "the fallback's own timeout: FAIL (asked ONCE), three requests in all");
  ok(failText(&L, 0, NULL, GEM_NET_TIMEOUT) == "Gemini did not answer in time", "...\"Gemini did not answer in time\"");
  ok(!geminiQuotaOut(&L.q, GEM_DEFAULT_MODEL, now, 0) && !geminiQuotaOut(&L.q, GEM_LITE_MODEL, now, 0) && L.q.n == 0,
     "a timed-out model is never booked as used up for the day");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  ok(noAnswer(&L, GEM_NET_LOST) == GEM_STEP_RETRY && !L.l.onFallback && noAnswer(&L, GEM_NET_LOST) == GEM_STEP_RETRY &&
     L.l.onFallback, "a LOST connection (a stalled or cut answer) is the same rule");
  ok(step(&L, 200, R_TEXT) == GEM_STEP_DONE && L.l.onFallback, "...and the fallback's answer is the one (its label)");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  ok(noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_RETRY && step(&L, 200, R_TEXT) == GEM_STEP_DONE && !L.l.onFallback,
     "a retry that works: the first model's answer");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  ok(!L.l.timeoutRetried && !L.l.timeoutSwitch, "...and the next question has the rule again (once a QUESTION)");
  // Off.
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  L.l.retryTimeouts = false;
  ok(noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_FAIL && L.l.attempts == 1 &&
     failText(&L, 0, NULL, GEM_NET_TIMEOUT) == "Gemini did not answer in time",
     "the setting OFF: the timeout shows as it did (no retry)");
  // Already on the fallback (the 503 ladder took it there): one retry on it, no "instead".
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  for (int i = 0; i < 3; i++) step(&L, 503, R_503);
  ok(L.l.onFallback && noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_RETRY && L.stage &&
     !strcmp(L.stage, GEM_TIMEOUT_AGAIN) && noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_FAIL,
     "on the fallback already: once more on it, then FAIL - never back to the first model");
  // The daily-limit memory still holds: a used-up fallback is not asked.
  ladderInit(&L);
  geminiQuotaMark(&L.q, GEM_LITE_MODEL, 22517, false, now, 0);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  ok(noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_RETRY && noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_FAIL &&
     !L.l.onFallback && !L.l.allOut && failText(&L, 0, NULL, GEM_NET_TIMEOUT) == "Gemini did not answer in time",
     "the fallback used up for today: not asked; FAIL as a timeout (not \"used up\")");
  ladderInit(&L, "fallback=none\n");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  ok(noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_RETRY && noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_FAIL,
     "fallback=none: the one retry, then FAIL");
  // The primary used up for today: the question starts on the fallback; its timeout retries once there.
  ladderInit(&L);
  geminiQuotaMark(&L.q, GEM_DEFAULT_MODEL, 22517, false, now, 0);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  ok(L.l.onFallback && noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_RETRY && L.l.onFallback &&
     noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_FAIL && !L.l.allOut,
     "a question that STARTS on the fallback (primary used up): one retry there, then a timeout FAIL");
  // Mixed with the other rules: the flags hold for the whole question.
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  ok(noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_RETRY && step(&L, 503, R_503) == GEM_STEP_RETRY &&
     noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_RETRY && L.l.onFallback && noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_FAIL,
     "timeout, 503, timeout: the second timeout moves to the fallback; its timeout ends it");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  ok(noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_RETRY && step(&L, 429, R_429_DAY, now, 5000) == GEM_STEP_RETRY &&
     L.l.onFallback && L.l.quotaSwitch && geminiQuotaOut(&L.q, GEM_DEFAULT_MODEL, now + 60, 65000),
     "timeout, then a PerDay 429: rule 3 as ever (marked, the fallback)");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  L.l.attempts = GEM_MAX_ATTEMPTS - 1;
  ok(noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_FAIL, "never past GEM_MAX_ATTEMPTS");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  int kt = 0;
  while (kt < 50 && noAnswer(&L, kt % 2 ? GEM_NET_LOST : GEM_NET_TIMEOUT) == GEM_STEP_RETRY) kt++;
  ok(kt == 2, "timeouts for ever: two retries in all, then the end (no loop)");
  ladderInit(&L, "fallback=gemini-2.5-flash-lite\n");
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  noAnswer(&L, GEM_NET_TIMEOUT);
  ok(noAnswer(&L, GEM_NET_TIMEOUT) == GEM_STEP_RETRY && !strcmp(L.stage, "Asking 2.5-Flash-Lite instead..."),
     "the fallback's own label in the line (fallback=gemini-2.5-flash-lite)");

  group("no answer at all");
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  ok(step(&L, -1, NULL) == GEM_STEP_FAIL, "no HTTP answer, no reason given: never retried");
  static const int notRetried[] = { GEM_NET_DNS, GEM_NET_CONNECT, GEM_NET_TLS, GEM_NET_TLS_OTHER, GEM_NET_NOMEM,
                                    GEM_NET_TOO_BIG, GEM_NET_BAD_BODY };
  bool none = true;
  for (size_t i = 0; i < sizeof(notRetried) / sizeof(notRetried[0]); i++) {
    geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
    none = none && noAnswer(&L, notRetried[i]) == GEM_STEP_FAIL;
  }
  ok(none, "no DNS, a refused connect, TLS, memory, too big, a bad body: never retried (not rule 6)");
  ok(failText(&L, -1, NULL, GEM_NET_DNS).find("No internet") == 0, "DNS: 'No internet'");
  ok(failText(&L, -1, NULL, GEM_NET_TLS).find("certificate") != std::string::npos, "TLS: the certificate");
  ok(failText(&L, -1, NULL, GEM_NET_TIMEOUT) == "Gemini did not answer in time", "a timeout");
  ok(failText(&L, 500, "{\"error\":{\"code\":500,\"message\":\"Internal error.\",\"status\":\"INTERNAL\"}}") ==
     "Gemini error 500: Internal error.", "anything else: the code and Google's words");
  // The attempt cap: no ladder runs for ever.
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, now, 0);
  int k = 0;
  while (k < 50 && step(&L, 429, R_429_BARE) == GEM_STEP_RETRY) k++;
  ok(k < GEM_MAX_ATTEMPTS, "every path ends");
}

static void testQuotaFile() {
  group("the remembered daily limits (/ai/state.txt)");
  GeminiQuota q, q2;
  geminiQuotaClear(&q);
  geminiQuotaMark(&q, GEM_DEFAULT_MODEL, 22517, false, 1700000000, 0);
  geminiQuotaMark(&q, "gemini-other", 10, false, 0, 0);     // no clock: not saved
  char buf[512];
  geminiQuotaSave(&q, 1700000000, buf, sizeof(buf));
  ok(!strcmp(buf, "wiphone-ai-quota 1\nout 1700022517 gemini-flash-latest\nend\n"), "saved: UTC deadlines only");
  ok(geminiQuotaLoad(buf, strlen(buf), &q2) && q2.n == 1 && geminiQuotaOut(&q2, GEM_DEFAULT_MODEL, 1700000100, 0) &&
     !geminiQuotaOut(&q2, GEM_DEFAULT_MODEL, 1700022518, 0), "loaded back: out until the deadline");
  ok(!geminiQuotaOut(&q2, GEM_DEFAULT_MODEL, 0, 0), "after a restart with no clock it cannot say: tried again");
  ok(!geminiQuotaLoad("wiphone-ai-quota 1\nout 17 gemini-flash-latest\n", 46, &q2) && q2.n == 0,
     "no 'end': empty");
  ok(!geminiQuotaLoad("garbage", 7, &q2) && !geminiQuotaLoad("wiphone-ai-quota 1\nout x y\nend\n", 30, &q2),
     "junk: empty");
  std::string keyLine = std::string("wiphone-ai-quota 1\nout 1700022517 ") + FAKE_KEY + "\nend\n";
  ok(!geminiQuotaLoad(keyLine.c_str(), keyLine.size(), &q2) && q2.n == 0, "a line that is not a model name: empty");
  geminiQuotaSave(&q, 1700022600, buf, sizeof(buf));
  ok(!strcmp(buf, "wiphone-ai-quota 1\nend\n"), "an expired one is not saved");
  // A guess (no RetryInfo) is saved AS a guess, and comes back as one.
  int64_t until = -1;
  uint32_t remain = 1;
  geminiQuotaClear(&q);
  geminiQuotaMark(&q, GEM_DEFAULT_MODEL, GEM_DAY_GUESS_S, true, 1700000000, 0);
  geminiQuotaSave(&q, 1700000000, buf, sizeof(buf));
  ok(!strcmp(buf, "wiphone-ai-quota 1\nest 1700003600 gemini-flash-latest\nend\n"), "a guess is saved as 'est'");
  ok(geminiQuotaLoad(buf, strlen(buf), &q2) && geminiQuotaWhen(&q2, GEM_DEFAULT_MODEL, 1700000100, 0, &until, &remain) &&
     until == 0 && remain == 0, "...and loads as one: out, with no time to show");

  group("the quota wording: Google's deadline, never 'midnight Pacific'");
  // 1700000000 = 2023-11-14 22:13:20 UTC (15:13 at UTC-7); + 22517 s = 04:28:37 UTC on the 15th.
  const int64_t now = 1700000000;
  char n[200];
  geminiQuotaNotice("Flash", "Flash-Lite", now + 22517, now, -7 * 3600, 22517, n, sizeof(n));
  ok(!strcmp(n, "Daily free limit for Flash reached - using Flash-Lite until about 21:29"),
     "a deadline + a trusted clock: the local time it comes back");
  geminiQuotaNotice("Flash", "Flash-Lite", now + 22517, now, 0, 22517, n, sizeof(n));
  ok(!strcmp(n, "Daily free limit for Flash reached - using Flash-Lite until about 04:29 tomorrow"),
     "...'tomorrow' when the local date differs");
  geminiQuotaNotice("Flash", "Flash-Lite", now + 6413, now, -7 * 3600, 6413, n, sizeof(n));
  ok(!strcmp(n, "Daily free limit for Flash reached - using Flash-Lite until about 17:00"),
     "Google's 00:00 UTC is 17:00 at UTC-7 (what this key's RetryInfo pointed at)");
  geminiQuotaNotice("Flash", "Flash-Lite", now + 6413, now, 5 * 3600 + 1800, 6413, n, sizeof(n));
  ok(!strcmp(n, "Daily free limit for Flash reached - using Flash-Lite until about 05:30"),
     "a +5:30 zone, where 'now' (03:43) is already that day: no 'tomorrow'");
  geminiQuotaNotice("Flash", "Flash-Lite", now + 3 * 86400, now, 0, 3 * 86400, n, sizeof(n));
  ok(!strcmp(n, "Daily free limit for Flash reached - using Flash-Lite for about 3 days"),
     "a deadline past tomorrow is a duration, never a time of day called 'tomorrow'");
  geminiQuotaNotice("Flash", "Flash-Lite", 0, 0, -7 * 3600, 22517, n, sizeof(n));
  ok(!strcmp(n, "Daily free limit for Flash reached - using Flash-Lite for about 6 h"),
     "no trusted clock: a duration");
  geminiQuotaNotice("Flash", "Flash-Lite", 0, 0, 0, 14400, n, sizeof(n));
  ok(strstr(n, "for about 4 h") != NULL, "4 h");
  geminiQuotaNotice("Flash", "Flash-Lite", 0, 0, 0, 2000, n, sizeof(n));
  ok(strstr(n, "for about 35 min") != NULL, "under 90 minutes: minutes, in fives");
  geminiQuotaNotice("Flash", "Flash-Lite", 0, 0, 0, 3 * 86400, n, sizeof(n));
  ok(strstr(n, "for about 3 days") != NULL, "days");
  geminiQuotaNotice("Flash", "Flash-Lite", 0, now, -7 * 3600, 0, n, sizeof(n));
  ok(!strcmp(n, "Daily free limit for Flash reached - using Flash-Lite for now"), "no deadline known: 'for now'");
  geminiQuotaNotice("Flash", "", 0, 0, 0, 0, n, sizeof(n));
  ok(!strcmp(n, "Daily free limit for Flash reached - try again later"), "no fallback, no deadline");
  geminiQuotaNotice("Flash", "", now + 6413, now, -7 * 3600, 6413, n, sizeof(n));
  ok(!strcmp(n, "Daily free limit for Flash reached - try again after about 17:00"), "no fallback, a deadline");
  geminiQuotaAllOut(now + 6413, now, -7 * 3600, 6413, n, sizeof(n));
  ok(!strcmp(n, "Today's free Gemini limit is used up - try again after about 17:00"), "both used up: after about");
  geminiQuotaAllOut(0, 0, 0, 14400, n, sizeof(n));
  ok(!strcmp(n, "Today's free Gemini limit is used up - try again in about 4 h"), "both used up, no clock");
  geminiQuotaAllOut(0, now, 0, 0, n, sizeof(n));
  ok(!strcmp(n, "Today's free Gemini limit is used up - try again later"), "both used up, no deadline");
  // The first model back, of two; a guess among them means no time at all.
  geminiQuotaClear(&q);
  geminiQuotaMark(&q, GEM_DEFAULT_MODEL, 22517, false, now, 0);
  geminiQuotaMark(&q, GEM_LITE_MODEL, 9000, false, now, 0);
  geminiQuotaBack(&q, GEM_DEFAULT_MODEL, GEM_LITE_MODEL, now + 10, 0, &until, &remain);
  ok(until == now + 9000 && remain == 8990, "both out: the earlier deadline");
  geminiQuotaMark(&q, GEM_LITE_MODEL, GEM_DAY_GUESS_S, true, now, 0);
  geminiQuotaBack(&q, GEM_DEFAULT_MODEL, GEM_LITE_MODEL, now + 10, 0, &until, &remain);
  ok(until == 0 && remain == 0, "one a guess: no time");

  group("the WAITING box: the quota notice in its 3 rows (Akrobat Bold 16, 228 px, measured)");
  fontLoad(&g_font, Akrobat_Bold16);
  ok(g_font.ok && g_font.n > 90, "the firmware's Akrobat_Bold16 glyph table loads");
  ok(textWidth(&g_font, "grey square  no tile at any level") == 203,
     "the measurer agrees with the 203 px app_maps.cpp measured for a help row");
  static const char* const models[] = { "Flash", "2.5-Flash", "Flash-Lite", "2.5-Flash-Lite" };
  bool fit = true;
  int worst = 0;
  for (int a = 0; a < 4; a++) {
    for (int b = 0; b < 4; b++) {
      for (int form = 0; form < 4; form++) {
        // 20:00 tomorrow: the widest digits a time of day has in this font are measured below too
        const int64_t u = form < 2 ? now + 22517 : 0;
        const uint32_t r = form == 3 ? 0 : (form == 2 ? 45u * 3600u : 22517u);
        geminiQuotaNotice(models[a], models[b], u, form < 2 ? now : 0, form == 0 ? 0 : -7 * 3600, r, n, sizeof(n));
        const int rows = wrapRows(&g_font, n, AI_STAGE_W);
        if (rows > worst) worst = rows;
        if (rows > AI_STAGE_ROWS || !allGlyphs(&g_font, n)) {
          printf("    %d rows: \"%s\"\n", rows, n);
          fit = false;
        }
      }
    }
  }
  ok(fit, "every notice (model labels x time / tomorrow / duration / for now) fits, no hollow boxes");
  printf("      the most rows a notice took: %d of %d\n", worst, AI_STAGE_ROWS);
  geminiQuotaNotice("Flash", "Flash-Lite", now + 22517, now, 0, 22517, n, sizeof(n));
  ok(wrapRows(&g_font, n, AI_STAGE_W) <= AI_STAGE_ROWS, "the default pair with 'tomorrow'");
  printf("      \"%s\": %d rows\n", n, wrapRows(&g_font, n, AI_STAGE_W));
  static const char* const stages[] = {
    "Asking Gemini...", "Retrying without thinking...", "Empty answer - asking again...",
    "Looking up Wikipedia...", "Model not found - trying gemini-flash-latest...",
    "Daily free limit reached - trying the faster model...", "Per-minute limit - waiting...",
    "Still limited - trying the faster model...", "Google is busy - trying again...",
    "Busy - trying the faster model...", GEM_TIMEOUT_AGAIN, "Asking Flash-Lite instead...",
    "Asking 2.5-Flash-Lite instead...",
  };
  fit = true;
  for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
    if (wrapRows(&g_font, stages[i], AI_STAGE_W) > AI_STAGE_ROWS) fit = false;
  }
  ok(fit, "the ladder's own stage lines fit too");

  group("the menu's \"Retry if it times out\" row (Akrobat Bold 20, the AI menu's face, measured)");
  Font7 f20;
  fontLoad(&f20, Akrobat_Bold20);
  const int rowW = 240 - 8 - 12;                  // lcd width - the menu's leftOffset - addNoteWrapped's margin
  const int wOn = textWidth(&f20, GEM_RETRY_LABEL ": On"), wOff = textWidth(&f20, GEM_RETRY_LABEL ": Off");
  printf("      \"%s: Off\" %d px of %d\n", GEM_RETRY_LABEL, wOff, rowW);
  ok(f20.ok && wOn <= rowW && wOff <= rowW && allGlyphs(&f20, GEM_RETRY_LABEL ": Off"),
     "both states fit one row, no hollow boxes (no marquee needed)");
}

// ── the chat ─────────────────────────────────────────────────────────────────────────────

static GeminiChat* newChat() {
  GeminiChat* c = (GeminiChat*)calloc(1, sizeof(GeminiChat));
  geminiChatClear(c);
  return c;
}

static void testChat() {
  group("the saved chat and its topics");
  GeminiChat* c = newChat();
  const int64_t t0 = 1700000000;
  ok(geminiChatTopicDue(c, t0), "the first question starts a topic");
  geminiChatAdd(c, t0, true, false, "Flash", "q1", "a1");
  ok(!geminiChatTopicDue(c, t0 + 60), "a minute later: the same topic");
  geminiChatAdd(c, t0 + 60, false, true, "", "q2 failed", "Google is busy");
  geminiChatAdd(c, t0 + 120, false, false, "Flash-Lite", "q3", "a3");
  int idx[GEM_CONTEXT];
  int n = geminiChatContext(c, idx);
  ok(n == 2 && idx[0] == 0 && idx[1] == 2, "context: answered ones only (the failed one is never sent)");
  ok(c->ex[1].label[0] == '\0' && (c->ex[1].flags & GEM_EX_FAILED), "a failed one is kept, marked");
  ok(geminiChatTopicDue(c, t0 + 120 + GEM_TOPIC_GAP_S + 1), "over two hours later: a new topic");
  ok(!geminiChatTopicDue(c, 0), "an unknown clock never breaks one by itself");
  geminiChatNewTopic(c);
  ok(c->breakPending && geminiChatTopicDue(c, t0 + 130), "'New topic' by hand");
  geminiChatAdd(c, t0 + 130, true, false, "Flash", "q4", "a4");
  ok(!c->breakPending && (c->ex[3].flags & GEM_EX_TOPIC), "the break lands on the next exchange");
  n = geminiChatContext(c, idx);
  ok(n == 1 && idx[0] == 3, "context is only the CURRENT topic");
  /* Review 2026-10-03: "New topic" pressed while a question was still out. The question lands
   * after it and must not wipe the break (geminiChatAdd clears breakPending). */
  {
    GeminiChat* f = newChat();
    geminiChatAdd(f, t0, true, false, "Flash", "a1", "x");
    geminiChatNewTopic(f);                 // pressed while "a2" was in flight
    geminiChatFold(f, t0 + 5, false, false, "Flash", "a2", "y", true);
    ok(f->n == 2 && f->breakPending && !(f->ex[1].flags & GEM_EX_TOPIC) && geminiChatTopicDue(f, t0 + 9),
       "a break made while a question was out still stands after it lands");
    geminiChatFold(f, t0 + 10, true, false, "Flash", "b1", "z", false);
    ok(!f->breakPending && (f->ex[2].flags & GEM_EX_TOPIC), "...and the next question starts the topic");
    free(f);
  }
  for (int i = 0; i < 6; i++) {
    char q[16];
    snprintf(q, sizeof(q), "more%d", i);
    geminiChatAdd(c, t0 + 140 + i, false, false, "Flash", q, "ans");
  }
  n = geminiChatContext(c, idx);
  ok(n == GEM_CONTEXT && !strcmp(c->ex[idx[0]].q, "more2") && !strcmp(c->ex[idx[3]].q, "more5"),
     "at most the last four of the topic");

  // Save and load.
  static char buf[GEM_CHAT_FILE_MAX + 4096];
  geminiChatAdd(c, 0, false, false, "Flash", "multi\nline \\ q", "ans\n\nwith blank\\n literal");
  geminiChatNewTopic(c);
  const size_t len = geminiChatSave(c, buf, sizeof(buf));
  ok(len == strlen(buf) && len < GEM_CHAT_FILE_MAX && !strncmp(buf, "wiphone-ai-chat 1\npending 1\n", 28),
     "saved: versioned, the pending break kept");
  GeminiChat* d = newChat();
  ok(geminiChatLoad(buf, len, d) && d->n == c->n && d->breakPending, "loaded: the same count, the break pending");
  bool same = true;
  for (int i = 0; i < c->n; i++) {
    same = same && d->ex[i].t == c->ex[i].t && d->ex[i].flags == c->ex[i].flags &&
           !strcmp(d->ex[i].label, c->ex[i].label) && !strcmp(d->ex[i].q, c->ex[i].q) &&
           !strcmp(d->ex[i].a, c->ex[i].a);
  }
  ok(same, "...every field round-trips (newlines and backslashes too)");
  ok(!geminiChatLoad(buf, len - 4, d) && d->n == 0, "a write cut short (no 'end'): EMPTY, never half");
  std::string v2 = std::string("wiphone-ai-chat 2") + (buf + 17);
  ok(!geminiChatLoad(v2.c_str(), v2.size(), d) && d->n == 0, "another version: empty");
  ok(!geminiChatLoad("rubbish\n", 8, d) && !geminiChatLoad("", 0, d) && !geminiChatLoad(NULL, 0, d),
     "junk: empty");
  const char bad[] = "wiphone-ai-chat 1\npending 0\nx 5 9 Flash\nq a\na b\nend\n";
  ok(!geminiChatLoad(bad, strlen(bad), d), "an impossible flag: empty");
  const char crlf[] = "wiphone-ai-chat 1\r\npending 0\r\nx 5 0 Flash\r\nq hi\r\na there\r\nend\r\n";
  ok(geminiChatLoad(crlf, strlen(crlf), d) && d->n == 1 && !strcmp(d->ex[0].a, "there"),
     "CRLF (a card edited on Windows) still loads");

  // The caps: 20 exchanges, 16 KB.
  geminiChatClear(c);
  for (int i = 0; i < 30; i++) {
    char q[16];
    snprintf(q, sizeof(q), "q%d", i);
    geminiChatAdd(c, t0 + i, false, false, "Flash", q, "short");
  }
  ok(c->n == GEM_CHAT_MAX && !strcmp(c->ex[0].q, "q10") && !strcmp(c->ex[GEM_CHAT_MAX - 1].q, "q29"),
     "at most 20: the oldest go");
  std::string longA(GEM_A_MAX - 1, 'z');
  for (int i = 0; i < 6; i++) {
    geminiChatAdd(c, t0 + 100 + i, false, false, "Flash", "long", longA.c_str());
  }
  ok(geminiChatSave(c, NULL, 0) <= GEM_CHAT_FILE_MAX && c->n < GEM_CHAT_MAX && c->n >= 3,
     "...and the saved form stays under 16 KB (whichever is smaller)");
  std::string huge(GEM_Q_MAX + 100, 'h');
  geminiChatAdd(c, t0, false, false, "Flash", huge.c_str(), "a");
  ok(strlen(c->ex[c->n - 1].q) == GEM_Q_MAX - 1, "an over-long field is cut, not overflowed");
  geminiChatClear(c);
  geminiChatNewTopic(c);
  ok(!c->breakPending && c->n == 0, "'Clear chat' empties it; 'New topic' on nothing is nothing");
  free(c);
  free(d);
}

// ── the key never shows ──────────────────────────────────────────────────────────────────

static void testScrub() {
  group("the key never reaches a screen or a log");
  char s[200] = "x FA.fake-key-for-tests_0123456789abcdef0123456789 y FA.fake-key-for-tests_0123456789abcdef0123456789";
  geminiScrub(s, sizeof(s), FAKE_KEY);
  ok(!strcmp(s, "x [key] y [key]"), "every occurrence replaced");
  char t[16] = "abc";
  geminiScrub(t, sizeof(t), "ab");
  ok(!strcmp(t, "abc"), "a too-short 'key' scrubs nothing (it would eat ordinary words)");
  Ladder L;
  ladderInit(&L);
  geminiLadderStart(&L.l, &L.cfg, &L.s, &L.q, 1700000000, 0);
  std::string ft = failText(&L, 400, R_ECHO_KEY);
  ok(noKey(ft.c_str()) && ft.find("[key]") != std::string::npos, "Google echoing the key back: scrubbed");
  printf("      \"%s\"\n", ft.c_str());
  ft = failText(&L, 403, "{\"error\":{\"code\":403,\"message\":\"Method doesn't allow unregistered "
                         "callers (callers without established identity). Please use API Key or other "
                         "form of API consumer identity to call this API.\",\"status\":\"PERMISSION_DENIED\"}}");
  ok(ft.find("Key rejected") == 0 && ft.size() < 200 && ft.find("...") != std::string::npos,
     "a 403: 'Key rejected', Google's long message shortened");
  ok(noKey(GEM_SYSTEM_INSTRUCTION), "(and of course not in the system instruction)");
  char sys[GEM_SYS_MAX];
  geminiSystemText(1791752700, -7 * 3600, FAKE_KEY, sys, sizeof(sys));
  ok(noKey(sys), "...nor in the one built per request, even from a zone 'name' holding it");

  /* Review 2026-10-03: the cleanup strips "__" and backticks, so a key holding "__" no longer
   * matched once cleaned - the scrub runs on Google's RAW message first. */
  {
    const char* k2 = "FA.fake__key-for-tests_0123456789abcdef0123456789";
    std::string body = std::string("{\"error\":{\"code\":400,\"message\":\"API key not valid: ") + k2 +
                       "\",\"status\":\"INVALID_ARGUMENT\"}}";
    GeminiReply r;
    geminiParseReply(body.c_str(), body.size(), g_text, sizeof(g_text), &r);
    char out[300];
    geminiFailText(400, GEM_NET_OK, &r, NULL, k2, 0, out, sizeof(out));
    ok(strstr(out, "fake__key") == NULL && strstr(out, "fakekey-for-tests") == NULL &&
       strstr(out, "0123456789abcdef") == NULL && strstr(out, "[key]") != NULL,
       "a key with '__' in it, echoed back: scrubbed before the cleanup could change it");
    printf("      \"%s\"\n", out);
  }
  /* ...and a long key echoed at the start of a message is CUT at errMessage's 200 bytes: the
   * first part of it that is left goes too. */
  {
    std::string k3 = "FA.fake-long-key-for-tests_";
    while (k3.size() < 260) k3 += "0123456789abcdef";
    std::string body = std::string("{\"error\":{\"code\":400,\"message\":\"") + k3 +
                       " is not valid\",\"status\":\"INVALID_ARGUMENT\"}}";
    GeminiReply r;
    geminiParseReply(body.c_str(), body.size(), g_text, sizeof(g_text), &r);
    char out[300];
    geminiFailText(400, GEM_NET_OK, &r, NULL, k3.c_str(), 0, out, sizeof(out));
    ok(strstr(out, "fake-long-key") == NULL && strstr(out, "0123456789abcdef") == NULL,
       "a 260-character key cut short in Google's message: no part of it is left");
    printf("      \"%s\"\n", out);
  }
}

int main() {
  testConfig();
  testRequest();
  testSystemText();
  testReply();
  testClean();
  testDechunk();
  testLadder();
  testQuotaFile();
  testChat();
  testScrub();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
