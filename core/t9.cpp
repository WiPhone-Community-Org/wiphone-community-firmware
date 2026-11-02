/*
Copyright © 2026 WiPhone Meshtastic firmware contributors.
Licensed under the WiPhone Public License v.1.0 (the "License"); you
may not use this file except in compliance with the License. You may
obtain a copy of the License at
https://wiphone.io/WiPhone_Public_License_v1.0.txt.

Unless required by applicable law or agreed to in writing, software,
hardware or documentation distributed under the License is distributed
on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/

#include "t9.h"
#include "src/assets/t9_dict.h"

/* Letter -> keypad digit. A 26-entry table rather than a chain of range tests: this runs
 * once per character per binary-search probe, so about 120 times per keypress, and a lookup
 * is one load where the tests are up to eight branches. */
static const char T9_DIGIT_OF_LETTER[26] = {
  /* a b c */ '2', '2', '2',
  /* d e f */ '3', '3', '3',
  /* g h i */ '4', '4', '4',
  /* j k l */ '5', '5', '5',
  /* m n o */ '6', '6', '6',
  /* p q r s */ '7', '7', '7', '7',
  /* t u v */ '8', '8', '8',
  /* w x y z */ '9', '9', '9', '9',
};

char t9DigitForChar(char c) {
  if (c >= 'a' && c <= 'z') {
    return T9_DIGIT_OF_LETTER[c - 'a'];
  }
  if (c >= 'A' && c <= 'Z') {
    return T9_DIGIT_OF_LETTER[c - 'A'];
  }
  return 0;                      // includes '\'' — see the note in the header
}

bool t9IsWordDigit(char c) {
  return c >= '2' && c <= '9';
}

/* Compare the digit key of `word` against `digits`, strcmp-style, WITHOUT materialising the
 * key anywhere. Returns <0, 0, >0.
 *
 * The comparison is on the key as a STRING, which is what the table is sorted by, so a
 * shorter key sorts before a longer one that shares its prefix ("228" < "2287"). That is
 * exactly the property that makes an exact-length match a contiguous run.
 *
 * Characters with no digit (the apostrophe) are skipped, so "don't" compares as "dont". */
static int t9CompareKey(const char* word, const char* digits) {
  while (*word) {
    const char d = t9DigitForChar(*word++);
    if (!d) {
      continue;                  // apostrophe: not part of the key
    }
    if (!*digits) {
      return 1;                  // word's key is longer, so it sorts after
    }
    if (d != *digits) {
      return d < *digits ? -1 : 1;
    }
    digits++;
  }
  return *digits ? -1 : 0;       // word ran out first => shorter => sorts before
}

void T9Engine::clear() {
  digitBuf[0] = '\0';
  nDigits = 0;
  runStart = 0;
  runLen = 0;
  extraStart = 0;
  extraLen = 0;
  sel = 0;
}

void T9Engine::setExtra(const T9ExtraTable* table) {
  extra = (table && table->words && table->count) ? table : NULL;
  if (nDigits) {
    lookup();          // a word already in progress gains (or loses) its extra candidates
  }
}

/* Built-in matches first, then the extra table's. The order is the feature: jargon is always
 * reachable and never in the way. */
const char* T9Engine::candidate(int i) const {
  if (i < 0) {
    return NULL;
  }
  if (i < (int)runLen) {
    return T9_WORDS[runStart + i];
  }
  const int j = i - (int)runLen;
  if (extra && j < (int)extraLen) {
    return extra->words[extraStart + j];
  }
  return NULL;
}

void T9Engine::nextCandidate() {
  const int n = candidateCount();
  if (n) {
    sel = (uint16_t)((sel + 1) % n);
  }
}

void T9Engine::prevCandidate() {
  const int n = candidateCount();
  if (n) {
    sel = (uint16_t)((sel + n - 1) % n);
  }
}

bool T9Engine::pushDigit(char digit) {
  if (!t9IsWordDigit(digit)) {
    return false;
  }
  /* -1 for the NUL. Refusing past the longest word in the table costs nothing: no key longer
   * than T9_MAX_KEY_LEN can match anything, so the extra digit could only ever produce the
   * same empty result. */
  if (nDigits + 1 >= (int)sizeof(digitBuf) || nDigits >= T9_MAX_KEY_LEN) {
    return false;
  }
  digitBuf[nDigits++] = digit;
  digitBuf[nDigits] = '\0';
  lookup();
  return true;
}

bool T9Engine::popDigit() {
  if (!nDigits) {
    return false;
  }
  digitBuf[--nDigits] = '\0';
  if (!nDigits) {
    clear();
    return true;
  }
  lookup();
  return true;
}

/* Find the contiguous run of words whose key equals `digits`, in one sorted table.
 *
 * Binary search for the LOWER BOUND — the first word whose key is >= the typed digits — then
 * walk forward while the key still matches. The walk is bounded by the worst collision in the
 * table (T9_WORST_RUN, emitted by the generator and measured, not guessed), so it is a handful
 * of comparisons, not a scan.
 *
 * Cost: about log2(N) key comparisons, each of which touches one word. For 25,000 words that
 * is ~15 probes. The built-in table is memory-mapped .rodata and the extra one is PSRAM; the
 * Game Boy emulator reads both the same way millions of times a second, so neither needs to
 * be made incremental or deferred.
 *
 * Factored out because there are now two tables searched identically — the extra one is not a
 * special case, it is the same algorithm over a different array. */
static void t9FindRun(const char* const* words, int count, const char* digits,
                      uint16_t* outStart, uint16_t* outLen) {
  *outStart = 0;
  *outLen = 0;
  if (!words || count <= 0) {
    return;
  }
  int lo = 0, hi = count;                   // [lo, hi)
  while (lo < hi) {
    const int mid = lo + ((hi - lo) >> 1);
    if (t9CompareKey(words[mid], digits) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  *outStart = (uint16_t)lo;
  int n = 0;
  while (lo + n < count && t9CompareKey(words[lo + n], digits) == 0) {
    n++;
  }
  *outLen = (uint16_t)n;
}

void T9Engine::lookup() {
  runStart = 0;
  runLen = 0;
  extraStart = 0;
  extraLen = 0;
  sel = 0;
  if (!nDigits) {
    return;
  }
  t9FindRun(T9_WORDS, (int)T9_WORD_COUNT, digitBuf, &runStart, &runLen);
  if (extra) {
    t9FindRun(extra->words, (int)extra->count, digitBuf, &extraStart, &extraLen);
  }
}
