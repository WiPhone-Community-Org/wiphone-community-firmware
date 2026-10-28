/* test_gbrom.cpp — the cartridge arithmetic that decides whether a file is a runnable ROM.
 *
 * WHY THIS IS WORTH A SUITE: the failure it guards is SILENT. gnuboy's only size check is
 * `size < 0x200`, and it takes the bank count from the file's own header rather than from how
 * many bytes actually arrived. Its bank-mapping loop runs while `size - pos >= 16 KB`, so a
 * buffer smaller than one bank leaves the entire bank table NULL, `gb_hw_updatemap()` then
 * allocates an UNINITIALISED PSRAM bank, and the emulated CPU executes whatever was in that
 * memory. No panic, no message, no clue — just a game that does not work.
 *
 * It is reachable two ways, both of which have actually happened on this phone: a truncated
 * upload (a WiFi transfer that stopped early), and the 4,096-byte macOS AppleDouble sidecars
 * (`._Name.gbc`) that a Finder folder-copy scatters across the card — twelve of them reached a
 * card and showed up as duplicate ROMs before 0.9.57 taught the picker to hide them.
 *
 * The functions under test are the REAL ones the firmware calls: `gnuboy.c` includes
 * `gb_romsize.h` and so does this file. `gnuboy.c` itself cannot be compiled here (it pulls in
 * esp_heap_caps.h, hw.h, cpu.h, sound.h and lcd.h), which is exactly why the arithmetic was
 * moved into a header of its own rather than re-implemented in a test.
 */
#include <cstdio>
#include <cstring>
#include <cstddef>

#include "../WiPhone/gnuboy/gb_romsize.h"

static int failures = 0;
static int checks = 0;

static void group(const char *name) {
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char *what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  } else {
    printf("  ok  %s\n", what);
  }
}

int main() {
  group("gbBanksFromHeader: header byte 0x0148 -> 16 KB banks");
  ok(gbBanksFromHeader(0x00) == 2, "0x00 is the smallest real cart: 2 banks (32 KB)");
  ok(gbBanksFromHeader(0x01) == 4, "0x01 is 4 banks (64 KB)");
  ok(gbBanksFromHeader(0x05) == 64, "0x05 is 64 banks (1 MB)");
  ok(gbBanksFromHeader(0x08) == 512, "0x08 is the largest doubling entry: 512 banks (8 MB)");
  ok(gbBanksFromHeader(0x52) == 128 && gbBanksFromHeader(0x53) == 128
     && gbBanksFromHeader(0x54) == 128,
     "the three odd sizes 0x52-0x54 all map to 128, as this emulator has always done");
  ok(gbBanksFromHeader(0x09) == 0, "0x09 names no cartridge and is refused");
  ok(gbBanksFromHeader(0x51) == 0, "0x51 is below the odd-size window and is refused");
  ok(gbBanksFromHeader(0x55) == 0, "0x55 is above it and is refused");
  ok(gbBanksFromHeader(0xFF) == 0, "0xFF - a blank or erased header - is refused");
  ok(gbBanksFromHeader(-1) == 0, "a negative byte cannot shift: refused, not undefined");

  group("gbRomTruncated: the buffer must hold every bank the header claims");
  /* The exact file that started this: a macOS AppleDouble sidecar is ~4 KB and carries
   * whatever bytes happen to sit at 0x0148. Even the smallest possible cart needs 32 KB. */
  ok(gbRomTruncated(4096, 2), "a 4 KB sidecar against the smallest 32 KB cart is truncated");
  ok(gbRomTruncated(512, 2), "the 512-byte floor gnuboy accepts is still truncated");
  ok(gbRomTruncated(32767, 2), "one byte short of a 2-bank cart is truncated");
  ok(!gbRomTruncated(32768, 2), "exactly 32 KB for a 2-bank cart is complete");
  ok(!gbRomTruncated(1048576, 64), "a full 1 MB 64-bank cart is complete");
  ok(gbRomTruncated(1048575, 64), "...and one byte short of it is not");
  ok(!gbRomTruncated(2097152, 64), "a buffer LARGER than the header claims is fine, not an error");

  /* 🛑 A header byte that names no cartridge must never be read as "0 banks needed", which
   * would make every size look complete. gbBanksFromHeader returns 0 for those, so the
   * predicate has to refuse on the count alone. */
  ok(gbRomTruncated(8 * 1024 * 1024, 0), "an unrecognised size byte is refused whatever the length");
  ok(gbRomTruncated(0, 2), "an empty buffer is truncated");

  group("the two together, as gnuboy calls them");
  struct { size_t size; int hdr; bool refused; const char *what; } cases[] = {
    {4096,        0x00, true,  "._Pokemon.gbc sidecar (4 KB, header claims 32 KB)"},
    {16384,       0x00, true,  "a half-finished upload of a 32 KB cart"},
    {32768,       0x00, false, "a complete 32 KB cart loads"},
    {2097152,     0x06, false, "a complete 2 MB cart loads"},
    {2097152,     0x09, true,  "a corrupt size byte is refused even at the right length"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const int banks = gbBanksFromHeader(cases[i].hdr);
    const bool refused = gbRomTruncated(cases[i].size, banks);
    ok(refused == cases[i].refused, cases[i].what);
  }

  printf("\n%s%d passed, %d failed\033[0m\n",
         failures ? "\033[31m" : "\033[32m", checks - failures, failures);
  return failures ? 1 : 0;
}
