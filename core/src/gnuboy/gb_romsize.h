/* gb_romsize.h — the two pieces of Game Boy cartridge arithmetic that decide whether a file
 * is a ROM we can run, kept in a header of their own so the HOST SUITE can test the code the
 * firmware actually executes.
 *
 * Why this exists as a separate file: `gnuboy.c` includes `esp_heap_caps.h`, `hw.h`, `cpu.h`,
 * `sound.h` and `lcd.h`, so it cannot be compiled by `tests/run_tests.sh` without shimming the
 * whole emulator. A test that re-implemented this arithmetic would prove only that the copy
 * agrees with itself — which is the failure mode the suite exists to avoid — so the arithmetic
 * moved here instead and BOTH sides include it.
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header. Keep it that way; that constraint is
 * what makes it testable at all.
 */
#ifndef GB_ROMSIZE_H
#define GB_ROMSIZE_H

#include <stddef.h>
#include <stdbool.h>

#define GB_BANK_SIZE 0x4000     /* 16 KB, the Game Boy's ROM bank */

/* Cartridge ROM size, decoded from header byte 0x0148 into a COUNT OF 16 KB BANKS.
 * Returns 0 for a byte that names no real cartridge — the caller refuses the file.
 *
 * The two live ranges are the ones the hardware ever shipped: 0x00-0x08 is 2 banks doubling to
 * 512, and 0x52-0x54 are the three odd sizes (72/80/96 banks) that a handful of carts report.
 * gnuboy has always mapped all three of those to 128, which over-allocates the bank table by a
 * few pointers and is harmless; that behaviour is preserved here rather than "fixed", because
 * changing it would change which files load. */
static inline int gbBanksFromHeader(int romsizeByte) {
  if (romsizeByte >= 0 && romsizeByte < 9) {
    return 2 << romsizeByte;
  }
  if (romsizeByte > 0x51 && romsizeByte < 0x55) {
    return 128;
  }
  return 0;
}

/* Is an IN-MEMORY ROM buffer shorter than the bank count its own header claims?
 *
 * 🛑 THIS IS THE GUARD BEHIND A CLASS OF SILENT DEAD GAME. gnuboy's only size check is
 * `size < 0x200`, so any file over 512 bytes reaches the loader, and the loader takes the bank
 * count from the header rather than from what actually arrived. Its mapping loop runs while
 * `size - pos >= GB_BANK_SIZE`, so a buffer smaller than ONE bank leaves the whole bank table
 * NULL — and `gb_hw_updatemap()` then allocates an UNINITIALISED PSRAM bank and the emulated
 * CPU executes whatever happened to be in that memory. There is no panic and no message: the
 * game is simply dead.
 *
 * Two ways in, both real: any truncated upload (a WiFi transfer that stopped early), and, until
 * 0.9.57 taught the picker to hide them, the 4,096-byte macOS AppleDouble sidecars (`._Name.gbc`)
 * that a Finder copy scatters across the card.
 *
 * ⚠ Applies to the in-memory path ONLY. With a `romFile`, banks are read from the card on
 * demand and a short read is already handled by filling the bank with 0xFF. */
static inline bool gbRomTruncated(size_t size, int banks) {
  return banks <= 0 || size < (size_t)banks * GB_BANK_SIZE;
}

#endif /* GB_ROMSIZE_H */
