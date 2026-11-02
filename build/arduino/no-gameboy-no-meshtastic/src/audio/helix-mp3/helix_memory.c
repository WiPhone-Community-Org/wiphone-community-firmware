/*
 * helix_memory.c — put the MP3 decoder in PSRAM, because it does not fit anywhere else.
 *
 * ── THE WHOLE REASON THIS FILE EXISTS ───────────────────────────────────────────────
 *
 * Helix allocates about 29 KB of decoder state in seven separate blocks (buffers.c:
 * MP3DecInfo, FrameHeader, SideInfo, ScaleFactorInfo, HuffmanInfo, DequantInfo,
 * IMDCTInfo, SubbandInfo). This phone has roughly 16 KB of INTERNAL heap free once WiFi
 * and SIP are up — measured, not guessed — so a plain malloc cannot succeed, and what
 * makes that dangerous rather than merely inconvenient is HOW it fails.
 *
 * It does not fail at the malloc. It succeeds, quietly eats the margin, and then the
 * WiFi PHY cannot get ~2 KB of internal RAM for its RF calibration data on a periodic
 * auto-switch re-scan, and phy_init calls abort(). The phone reboots a minute or two
 * later with a backtrace pointing at nothing to do with audio. That exact failure is
 * what opening a book used to do, and it cost a long debugging session to attribute.
 *
 * ⚠ The obvious fix — arduino-esp32 diverting large mallocs to PSRAM automatically —
 * DOES NOT WORK HERE. That threshold (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) is 16 KB and
 * every one of helix's seven blocks is smaller than it, the largest being IMDCTInfo at
 * about 9 KB. Seven allocations that are individually "small" and collectively fatal is
 * precisely the case the automatic rule is blind to.
 *
 * So allocation is forced to PSRAM explicitly. There are megabytes spare there, and
 * decoding runs from it fast enough because the ESP32 caches PSRAM reads and the hot
 * inner loops work out of registers and stack, both of which stay internal.
 */

#include "utils/helix_memory.h"
#include <stdlib.h>

#if defined(ESP32) || defined(ESP_PLATFORM)
#include "esp_heap_caps.h"

void* helix_malloc(int size) {
  if (size <= 0) {
    return NULL;
  }
  /* MALLOC_CAP_SPIRAM is explicit rather than advisory: ps_malloc() would fall back to
   * internal RAM if PSRAM were exhausted, which is the one outcome this file exists to
   * prevent. Better to fail the allocation and refuse to play than to reboot the phone
   * ten minutes later for no visible reason. */
  return heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void helix_free(void* ptr) {
  if (ptr) {
    heap_caps_free(ptr);
  }
}

#else   /* host builds: tests/test_mp3.cpp compiles the real decoder on the Mac */

void* helix_malloc(int size) {
  return size > 0 ? malloc((size_t)size) : NULL;
}

void helix_free(void* ptr) {
  free(ptr);
}

#endif
