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

/* The T9 EXTRA dictionary — your own words, from the SD card.
 *
 * `/t9/extra.txt`, one lowercase word per line, already sorted by keypad digit key (see
 * tools/gen_t9_extra.py, which writes it). Loaded ONCE at boot into PSRAM and handed to the
 * engine as a second table whose matches come after the built-in ones. So jargon — unit
 * names, callsigns, place names, a team roster — is always reachable on the candidate list
 * and never displaces an ordinary English word.
 *
 * 🛑 PSRAM, AND READ EXACTLY ONCE. The file is ~70 KB and the pointer array another ~35 KB;
 * that is nothing against 3.6 MB of PSRAM and utterly fatal on the ~20 KB internal heap. The
 * read is at BOOT and never on the keypress path — a filesystem access per keystroke is the
 * 1.5 s freeze this firmware has fought before, and it is why the built-in dictionary lives
 * in flash rather than on the card.
 *
 * EVERY FAILURE IS SILENT AND HARMLESS. No card, no file, a malformed file, no PSRAM: the
 * table stays empty, the engine is never given one, and predictive text works exactly as it
 * does on a phone that never had the file. There is no state in which this can panic, and
 * none in which it can make the phone worse than not having it.
 */
#ifndef _WIPHONE_T9_EXTRA_H
#define _WIPHONE_T9_EXTRA_H

#include "t9.h"

/* Under a directory of its own, like /books, /photos and /roms — so the WiFi uploader
 * (`up on t9`) can create it and drop files in without special-casing the card root. */
#define T9_EXTRA_PATH "/t9/extra.txt"

/* Read the file into PSRAM and build the table. Returns the number of words loaded, 0 for
 * any reason at all. Safe to call more than once; a second call frees the first result.
 *
 * cardPresent is passed IN rather than read from the GUI, so this file depends on the SD
 * library and nothing else — the caller already knows, and it keeps the dependency one-way. */
int t9ExtraLoad(bool cardPresent, const char* path = T9_EXTRA_PATH);

/* The loaded table, or NULL when there is none. Valid until the next t9ExtraLoad(). */
const T9ExtraTable* t9ExtraGet();

/* In words, for the `t9` console command. */
int t9ExtraCount();

/* Why there is no table, in words that name the cause — "no SD card", "no /t9/extra.txt",
 * "out of memory". A static literal, safe to hold. Exists because a dictionary that quietly
 * did not load looks exactly like a dictionary with none of your words in it. */
const char* t9ExtraStatus();

#endif // _WIPHONE_T9_EXTRA_H
