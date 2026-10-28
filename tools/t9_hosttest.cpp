/* Host-side test for the T9 engine. Builds and runs on the laptop, not the phone:
 *
 *     c++ -std=c++11 -I WiPhone -o /tmp/t9test tools/t9_hosttest.cpp WiPhone/t9.cpp && /tmp/t9test
 *
 * t9.cpp deliberately depends on nothing but <stdint.h> and the generated table, which is
 * what makes this possible — and it is the reason the engine was kept free of widgets,
 * ControlState and the keypad. A logic bug found here costs a second; the same bug found
 * after flashing costs a reflash and a reboot.
 *
 * These are behavioural assertions, not a demo: every one of them is a property the input
 * path will rely on.
 */
#include "t9.h"
#include "src/assets/t9_dict.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

static int failures = 0;
static int checks = 0;

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) {
    failures++;
    std::printf("  FAIL  %s\n", what);
  }
}

static void eqs(const char* got, const char* want, const char* what) {
  checks++;
  const bool same = (got == NULL && want == NULL) ||
                    (got != NULL && want != NULL && std::strcmp(got, want) == 0);
  if (!same) {
    failures++;
    std::printf("  FAIL  %s: got %s, want %s\n", what,
                got ? got : "(null)", want ? want : "(null)");
  }
}

// Type a whole digit string into a fresh engine.
static void type(T9Engine& e, const char* digits) {
  e.clear();
  for (const char* p = digits; *p; p++) {
    e.pushDigit(*p);
  }
}

// The digit key of a word, computed independently of the engine so the test is not just
// asserting that t9.cpp agrees with itself.
static void keyOf(const char* w, char* out) {
  int n = 0;
  for (; *w; w++) {
    const char c = *w | 0x20;
    if (c < 'a' || c > 'z') {
      continue;                       // apostrophe
    }
    static const char* rows[8] = { "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz" };
    for (int r = 0; r < 8; r++) {
      if (std::strchr(rows[r], c)) {
        out[n++] = (char)('2' + r);
        break;
      }
    }
  }
  out[n] = '\0';
}

int main() {
  std::printf("T9 engine host test — %d words in table\n", (int)T9_WORD_COUNT);

  // ---- the table's own invariants ------------------------------------------------------
  // Everything below depends on the table being sorted the way the generator claims. Check
  // it here rather than trusting it: a mis-sorted table would make the binary search return
  // plausible-looking wrong answers rather than failing loudly.
  {
    char prev[64] = "", cur[64];
    bool sorted = true;
    int maxLen = 0, maxKey = 0;
    for (int i = 0; i < (int)T9_WORD_COUNT; i++) {
      keyOf(T9_WORDS[i], cur);
      if (std::strcmp(prev, cur) > 0) {
        sorted = false;
        std::printf("  ...out of order at %d: %s (%s) after (%s)\n", i, T9_WORDS[i], cur, prev);
        break;
      }
      const int l = (int)std::strlen(T9_WORDS[i]);
      if (l > maxLen) maxLen = l;
      if ((int)std::strlen(cur) > maxKey) maxKey = (int)std::strlen(cur);
      std::strcpy(prev, cur);
    }
    ok(sorted, "table is sorted by digit key");
    ok(maxLen <= T9_MAX_WORD_LEN, "no word exceeds T9_MAX_WORD_LEN");
    ok(maxKey <= T9_MAX_KEY_LEN, "no key exceeds T9_MAX_KEY_LEN");
  }

  // ---- the digit mapping ---------------------------------------------------------------
  ok(t9DigitForChar('a') == '2' && t9DigitForChar('c') == '2', "abc -> 2");
  ok(t9DigitForChar('s') == '7' && t9DigitForChar('p') == '7', "pqrs -> 7");
  ok(t9DigitForChar('z') == '9', "z -> 9");
  ok(t9DigitForChar('A') == '2', "uppercase maps too");
  ok(t9DigitForChar('\'') == 0, "apostrophe has no digit");
  ok(t9DigitForChar('0') == 0 && t9DigitForChar(' ') == 0, "non-letters have no digit");
  ok(!t9IsWordDigit('0') && !t9IsWordDigit('1'), "0 and 1 are not word digits");
  ok(t9IsWordDigit('2') && t9IsWordDigit('9'), "2..9 are word digits");

  // ---- empty state ---------------------------------------------------------------------
  {
    T9Engine e;
    ok(!e.pending(), "starts with nothing pending");
    ok(e.candidateCount() == 0, "no candidates when empty");
    eqs(e.current(), NULL, "current() is NULL when empty");
    ok(e.candidate(0) == NULL && e.candidate(-1) == NULL, "candidate() bounds-checks");
    ok(!e.popDigit(), "popDigit on empty returns false");
    e.nextCandidate();                       // must not crash or misbehave
    e.prevCandidate();
    ok(e.candidateCount() == 0, "cycling with no candidates is a no-op");
  }

  // ---- rejects what is not a word digit -------------------------------------------------
  {
    T9Engine e;
    ok(!e.pushDigit('0'), "0 rejected");
    ok(!e.pushDigit('1'), "1 rejected");
    ok(!e.pushDigit('a'), "letter rejected");
    ok(!e.pending(), "rejected digits leave no state");
  }

  // ---- every word in the table is findable by its own key -------------------------------
  // The property that matters most: if it shipped, you can type it.
  {
    int unreachable = 0, checked = 0;
    char key[64];
    T9Engine e;
    for (int i = 0; i < (int)T9_WORD_COUNT; i += 7) {   // stride: fast but spread across
      keyOf(T9_WORDS[i], key);
      if ((int)std::strlen(key) > T9_MAX_KEY_LEN) continue;
      type(e, key);
      bool found = false;
      for (int c = 0; c < e.candidateCount(); c++) {
        if (std::strcmp(e.candidate(c), T9_WORDS[i]) == 0) {
          found = true;
          break;
        }
      }
      checked++;
      if (!found) {
        if (unreachable < 3) {
          std::printf("  ...unreachable: %s (key %s, %d candidates)\n",
                      T9_WORDS[i], key, e.candidateCount());
        }
        unreachable++;
      }
    }
    std::printf("  reachability: %d/%d sampled words findable\n", checked - unreachable, checked);
    ok(unreachable == 0, "every sampled word is reachable by its own key");
  }

  // ---- candidates are exact-length matches, never prefixes -------------------------------
  {
    T9Engine e;
    char key[64];
    int bad = 0;
    const char* probes[] = { "2", "22", "228", "2273", "43556", "8447", "76737" };
    for (unsigned p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
      type(e, probes[p]);
      for (int c = 0; c < e.candidateCount(); c++) {
        keyOf(e.candidate(c), key);
        if (std::strcmp(key, probes[p]) != 0) {
          bad++;
          std::printf("  ...%s returned %s whose key is %s\n", probes[p], e.candidate(c), key);
        }
      }
    }
    ok(bad == 0, "every candidate's key equals the typed digits exactly");
  }

  // ---- cycling wraps and stays in range --------------------------------------------------
  {
    T9Engine e;
    type(e, "2273");                          // card / case / base / bare in the placeholder
    const int n = e.candidateCount();
    ok(n > 0, "2273 matches something");
    if (n > 0) {
      const char* first = e.current();
      for (int i = 0; i < n; i++) {
        ok(e.current() != NULL, "current() never NULL while candidates exist");
        e.nextCandidate();
      }
      eqs(e.current(), first, "nextCandidate wraps after a full cycle");
      e.prevCandidate();
      eqs(e.current(), e.candidate(n - 1), "prevCandidate from 0 wraps to the end");
      e.nextCandidate();
      eqs(e.current(), first, "and back again");
    }
  }

  // ---- backspace un-types one keypress ---------------------------------------------------
  {
    T9Engine e;
    type(e, "2273");
    ok(e.digitCount() == 4, "four digits pending");
    e.popDigit();
    ok(e.digitCount() == 3, "popDigit removes one digit");
    eqs(e.digits(), "227", "digit buffer shortened correctly");
    e.popDigit();
    e.popDigit();
    e.popDigit();
    ok(!e.pending(), "popping to empty clears pending");
    ok(e.candidateCount() == 0, "and clears candidates");
    ok(!e.popDigit(), "further pops return false");
  }

  // ---- selection resets when the word changes --------------------------------------------
  // If it did not, typing another letter could leave `sel` pointing past the new, shorter run.
  {
    T9Engine e;
    type(e, "2273");
    if (e.candidateCount() > 1) {
      e.nextCandidate();
      ok(e.selected() != 0, "selection moved");
    }
    e.pushDigit('7');
    ok(e.selected() == 0, "pushDigit resets the selection");
    ok(e.selected() < e.candidateCount() || e.candidateCount() == 0, "selection in range");
    type(e, "2273");
    if (e.candidateCount() > 1) {
      e.nextCandidate();
      e.popDigit();
      ok(e.selected() == 0, "popDigit resets the selection");
    }
  }

  // ---- the buffer cannot be overrun -------------------------------------------------------
  {
    T9Engine e;
    e.clear();
    int accepted = 0;
    for (int i = 0; i < 500; i++) {
      if (e.pushDigit('2')) accepted++;
    }
    ok(accepted <= T9_MAX_KEY_LEN, "pushDigit stops at T9_MAX_KEY_LEN");
    ok(e.digitCount() == accepted, "digit count matches what was accepted");
    ok(e.digits()[e.digitCount()] == '\0', "digit buffer stays NUL-terminated");
    while (e.popDigit()) { }
    ok(!e.pending(), "drains cleanly");
  }

  // ---- a key nothing matches is not an error ----------------------------------------------
  {
    T9Engine e;
    type(e, "99999999");                      // zzzzzzzz — nothing spells that
    ok(e.pending(), "digits still pending");
    ok(e.candidateCount() == 0, "no candidates");
    eqs(e.current(), NULL, "current() is NULL — the multi-tap fallback trigger");
    ok(e.popDigit(), "and it can still be backspaced");
  }

  // ---- apostrophes are typed without the symbols row ---------------------------------------
  {
    T9Engine e;
    char key[64];
    int contractions = 0, reachable = 0;
    for (int i = 0; i < (int)T9_WORD_COUNT; i++) {
      if (!std::strchr(T9_WORDS[i], '\'')) continue;
      contractions++;
      keyOf(T9_WORDS[i], key);
      if ((int)std::strlen(key) > T9_MAX_KEY_LEN) continue;
      type(e, key);
      for (int c = 0; c < e.candidateCount(); c++) {
        if (std::strcmp(e.candidate(c), T9_WORDS[i]) == 0) {
          reachable++;
          break;
        }
      }
    }
    std::printf("  contractions: %d in table, %d reachable without the apostrophe key\n",
                contractions, reachable);
    ok(contractions == reachable, "every contraction is reachable from letters alone");
  }

  // ---- the extra (user) dictionary ---------------------------------------------------------
  // Words the built-in table does not have, sorted by digit key then relevance, exactly as
  // tools/gen_t9_extra.py writes them. These stand in for the SD-loaded file.
  {
    // keys: mechwarrior=6324927749, battletech=2288384382, kerensky=53736759,
    // zzzznotaword is unreachable nonsense used to prove "extra only" lookups.
    static const char* const EXTRA_WORDS[] = {
      "battletech",     // 2288384382
      "kerensky",       // 53736759
      "mechwarrior",    // 6324927749
    };
    static const T9ExtraTable EXTRA = { EXTRA_WORDS, 3 };

    T9Engine e;
    char key[64];

    // Without the extra table these are unreachable.
    keyOf("kerensky", key);
    type(e, key);
    const int before = e.candidateCount();
    bool foundBefore = false;
    for (int c = 0; c < before; c++)
      if (!std::strcmp(e.candidate(c), "kerensky")) foundBefore = true;
    ok(!foundBefore, "extra word is absent before the table is attached");

    e.setExtra(&EXTRA);
    ok(e.candidateCount() >= before, "attaching the table cannot lose candidates");
    bool foundAfter = false;
    int atIndex = -1;
    for (int c = 0; c < e.candidateCount(); c++)
      if (!std::strcmp(e.candidate(c), "kerensky")) { foundAfter = true; atIndex = c; }
    ok(foundAfter, "extra word is reachable once attached");
    ok(atIndex >= before, "extra words rank AFTER every built-in match");

    // setExtra re-runs the lookup on a word already in progress.
    ok(e.pending(), "the word is still pending across setExtra");

    // Detaching removes them again and leaves the selection valid.
    e.setExtra(NULL);
    ok(e.candidateCount() == before, "detaching restores the built-in count");
    ok(e.selected() < e.candidateCount() || e.candidateCount() == 0, "selection stays in range");

    // A common English word must not be displaced by the extra table.
    e.setExtra(&EXTRA);
    keyOf("the", key);
    type(e, key);
    eqs(e.candidate(0), "the", "extra table does not displace candidate #1");

    // Cycling walks both tables and wraps.
    keyOf("kerensky", key);
    type(e, key);
    const int n = e.candidateCount();
    const char* first = e.current();
    for (int i = 0; i < n; i++) {
      ok(e.current() != NULL, "current() never NULL while cycling across both tables");
      e.nextCandidate();
    }
    eqs(e.current(), first, "cycling wraps across both tables");

    // An empty or malformed table must be treated as no table at all.
    static const T9ExtraTable EMPTY = { EXTRA_WORDS, 0 };
    static const T9ExtraTable NOWORDS = { NULL, 5 };
    e.setExtra(&EMPTY);
    keyOf("the", key); type(e, key);
    ok(e.candidateCount() > 0, "a zero-count table is ignored, not fatal");
    e.setExtra(&NOWORDS);
    keyOf("the", key); type(e, key);
    ok(e.candidateCount() > 0, "a NULL-words table is ignored, not fatal");
    ok(e.candidate(e.candidateCount()) == NULL, "one past the end is still NULL");
  }

  // ---- WHAT THE DICTIONARY MUST CONTAIN ----------------------------------------------------
  /* 🔑 THE TEST THAT WOULD HAVE CAUGHT "cat".
   *
   * Everything above asserts that the engine agrees with itself: that words IN the table are
   * reachable, sorted, and cycled correctly. All of it passed while the shipped dictionary
   * was missing cat, bat, cop, bus, cup, art, cry, sat, mat, tip, gym, pop and pub, and while
   * 401 of the 512 three-digit keys matched nothing whatsoever. A generator flag had quietly
   * removed 990 of the 1,115 three-letter words and no assertion here noticed, because none
   * of them was about ENGLISH.
   *
   * This list is. It is deliberately hand-written rather than derived from the corpus — a
   * list generated from the same source that builds the table could never disagree with it.
   * If a word here stops being typable, that is a regression whatever the numbers say. */
  {
    static const char* const MUST_HAVE[] = {
      // The commonest words in the language.
      "the", "and", "you", "for", "are", "have", "this", "that", "with", "from", "they",
      "what", "when", "will", "your", "just", "like", "know", "time", "good", "make",
      // Three letters: the length a rank filter is most likely to eat, and the reason
      // this block exists. Every one of these was missing at some point today.
      "cat", "bat", "act", "cop", "bus", "cup", "art", "cry", "sat", "mat", "tip", "gym",
      "pop", "pub", "dog", "car", "job", "boy", "son", "bad", "big", "old", "new", "day",
      "way", "off", "any", "two", "ten", "six", "hot", "bed", "red", "man", "kid", "sun",
      // Two letters: the ones a keypad can actually produce and a person actually types.
      "am", "an", "as", "at", "be", "by", "do", "go", "he", "hi", "if", "in", "is", "it",
      "me", "my", "no", "of", "oh", "ok", "on", "or", "so", "to", "up", "us", "we",
      // Things people text about.
      "home", "house", "room", "food", "water", "coffee", "dinner", "phone", "text",
      "message", "email", "call", "battery", "charger", "radio", "signal", "morning",
      "night", "today", "tomorrow", "week", "money", "work", "school", "meeting",
      "love", "sorry", "please", "thanks", "hello", "family", "friend", "mother",
      "father", "sister", "brother", "doctor", "hospital", "help", "safe", "rain",
      "snow", "cold", "warm", "left", "right", "near", "town", "road", "map",
      // Using a phone. A 2018 subtitle corpus ranks these far below the cut; the
      // generator promotes them deliberately, and this is what proves it still does.
      "wifi", "bluetooth", "usb", "username", "login", "browser", "screenshot",
      "emoji", "smartphone", "hotspot", "internet", "online", "password", "download",
      // Contractions, which the corpus splits on the apostrophe and the generator repairs.
      // Reachable from letters alone — see the apostrophe test above.
      "don't", "can't", "won't", "i'm", "it's", "that's", "we're", "you're",
      "didn't", "doesn't", "isn't", "i've", "i'll", "he's", "she's", "there's",
      "what's", "let's",
    };
    const int n = (int)(sizeof(MUST_HAVE) / sizeof(MUST_HAVE[0]));

    // Present in the table at all...
    int absent = 0;
    for (int i = 0; i < n; i++) {
      bool found = false;
      for (int j = 0; j < (int)T9_WORD_COUNT && !found; j++) {
        if (!std::strcmp(T9_WORDS[j], MUST_HAVE[i])) found = true;
      }
      if (!found) {
        if (absent < 20) std::printf("  MISSING FROM DICTIONARY: %s\n", MUST_HAVE[i]);
        absent++;
      }
    }
    checks++;
    if (absent) { failures++; std::printf("  FAIL  %d of %d required words absent\n", absent, n); }

    // ...and actually TYPABLE, which is the thing the user cares about. A word can be in
    // the table and still unreachable if the sort and the search ever disagree.
    T9Engine e;
    char key[64];
    int untypable = 0;
    for (int i = 0; i < n; i++) {
      keyOf(MUST_HAVE[i], key);
      if (!key[0] || (int)std::strlen(key) > T9_MAX_KEY_LEN) continue;
      type(e, key);
      bool found = false;
      for (int c = 0; c < e.candidateCount() && !found; c++) {
        if (!std::strcmp(e.candidate(c), MUST_HAVE[i])) found = true;
      }
      if (!found) {
        if (untypable < 20) std::printf("  NOT TYPABLE: %s (key %s)\n", MUST_HAVE[i], key);
        untypable++;
      }
    }
    checks++;
    if (untypable) { failures++; std::printf("  FAIL  %d of %d required words not typable\n", untypable, n); }
    std::printf("  vocabulary: %d required words, %d absent, %d untypable\n", n, absent, untypable);
  }

  // ---- KEY COVERAGE ------------------------------------------------------------------------
  /* The blunt instrument that would have caught the same bug without knowing any English:
   * how many of the 512 three-digit keys match at least one word.
   *
   * Measured: 111 when cat was missing, 373 now, against a ceiling of 433 for the entire
   * 50,000-word corpus — the last 60 keys live in the tail this dictionary deliberately does
   * not carry. So the floor is 350: comfortably under the real figure so ordinary churn does
   * not fail the build, and three times the broken one so a collapse cannot pass.
   * ⚠ 400 was tried first and is NOT reachable; if this ever needs raising, check the
   * ceiling above before picking a number. */
  {
    T9Engine e;
    char key[4] = { 0, 0, 0, 0 };
    int live = 0;
    for (char a = '2'; a <= '9'; a++)
      for (char b = '2'; b <= '9'; b++)
        for (char c = '2'; c <= '9'; c++) {
          key[0] = a; key[1] = b; key[2] = c;
          type(e, key);
          if (e.candidateCount() > 0) live++;
        }
    std::printf("  three-digit keys with at least one word: %d of 512\n", live);
    ok(live >= 350, "three-digit key coverage has not collapsed (>= 350 of 512)");
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
