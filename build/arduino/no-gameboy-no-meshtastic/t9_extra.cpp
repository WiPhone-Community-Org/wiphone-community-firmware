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

#include "t9_extra.h"

#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>

/* One PSRAM blob holding the whole file with its newlines turned into NULs, plus one PSRAM
 * array of pointers into it. Two allocations for the entire dictionary — the words are never
 * copied individually, which is why an 8,700-word file costs one read and no per-word work.
 *
 * ⚠ HARD CAP. A corrupt or absurd file must not be able to eat PSRAM the emulator and the
 * photo decoder need. 512 KB is about seven times the size of a full BattleTech harvest and
 * still a rounding error against 3.6 MB. */
#define T9_EXTRA_MAX_BYTES  (512u * 1024u)
#define T9_EXTRA_MAX_WORDS  60000

static char*        s_blob = NULL;
static const char** s_words = NULL;
static uint16_t     s_count = 0;
static const char*  s_status = "not loaded";
static T9ExtraTable s_table = { NULL, 0 };

static void t9ExtraFree() {
  if (s_blob) {
    free(s_blob);
    s_blob = NULL;
  }
  if (s_words) {
    free(s_words);
    s_words = NULL;
  }
  s_count = 0;
  s_table.words = NULL;
  s_table.count = 0;
}

const T9ExtraTable* t9ExtraGet() {
  return s_table.count ? &s_table : NULL;
}

int t9ExtraCount() {
  return (int)s_count;
}

const char* t9ExtraStatus() {
  return s_status;
}

int t9ExtraLoad(bool cardPresent, const char* path) {
  t9ExtraFree();

  /* Checked rather than left to SD.open failing politely: the card being out is the normal
   * case for most phones, and it deserves to be NAMED rather than reported as a missing
   * file. A status that says "no SD card" sends someone to the right place. */
  if (!cardPresent) {
    s_status = "no SD card";
    return 0;
  }

  File f = SD.open(path, "r");
  if (!f) {
    s_status = "no " T9_EXTRA_PATH;
    return 0;
  }
  const size_t size = f.size();
  if (!size) {
    f.close();
    s_status = T9_EXTRA_PATH " is empty";
    return 0;
  }
  if (size > T9_EXTRA_MAX_BYTES) {
    f.close();
    s_status = T9_EXTRA_PATH " is too big (max 512 KB)";
    return 0;
  }

  /* PSRAM explicitly, NOT ps_malloc's silent fallback to the internal heap: 70 KB of internal
   * RAM does not exist on this phone, and getting it would be worse than not loading at all. */
  s_blob = (char*)heap_caps_malloc(size + 1, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
  if (!s_blob) {
    f.close();
    s_status = "not enough PSRAM for the word list";
    return 0;
  }

  const size_t got = f.read((uint8_t*)s_blob, size);
  f.close();
  if (got != size) {
    log_e("T9 extra: short read, %u of %u bytes", (unsigned)got, (unsigned)size);
    t9ExtraFree();
    s_status = "could not read the whole file";
    return 0;
  }
  s_blob[size] = '\0';

  /* Pass one: count the lines worth keeping, so the pointer array is allocated once at the
   * right size rather than grown. */
  int n = 0;
  for (const char* p = s_blob; *p; ) {
    const char* line = p;
    while (*p && *p != '\n' && *p != '\r') {
      p++;
    }
    const size_t len = (size_t)(p - line);
    while (*p == '\n' || *p == '\r') {
      p++;
    }
    if (len && *line != '#') {
      n++;
    }
  }
  if (!n) {
    t9ExtraFree();
    s_status = T9_EXTRA_PATH " has no words in it";
    return 0;
  }
  if (n > T9_EXTRA_MAX_WORDS) {
    n = T9_EXTRA_MAX_WORDS;              // keep the first N; the file is sorted, so this is
  }                                      // a truncation of the tail, not a random subset

  s_words = (const char**)heap_caps_malloc((size_t)n * sizeof(char*),
                                           MALLOC_CAP_32BIT | MALLOC_CAP_SPIRAM);
  if (!s_words) {
    t9ExtraFree();
    s_status = "not enough PSRAM for the word index";
    return 0;
  }

  /* Pass two: terminate each line in place and record where it starts. Nothing is copied. */
  int i = 0;
  for (char* p = s_blob; *p && i < n; ) {
    char* line = p;
    while (*p && *p != '\n' && *p != '\r') {
      p++;
    }
    char* end = p;
    while (*p == '\n' || *p == '\r') {
      p++;
    }
    if (end == line || *line == '#') {
      continue;                          // blank line or a comment
    }
    *end = '\0';
    s_words[i++] = line;
  }

  s_count = (uint16_t)i;
  s_table.words = s_words;
  s_table.count = s_count;
  s_status = "loaded";
  log_e("T9 extra: %d words from %s (%u bytes, PSRAM)", (int)s_count, path, (unsigned)size);
  return (int)s_count;
}
