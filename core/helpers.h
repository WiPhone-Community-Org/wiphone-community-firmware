/*
Copyright © 2019, 2020, 2021, 2022 HackEDA, Inc.
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

#ifndef __HELPERS_H
#define __HELPERS_H

#include <random>
#include <cstddef>
#include <new>
#include <WiFiUdp.h>
#include <esp_heap_caps.h>
#include "src/digcalc.h"
#include "src/MurmurHash3_32.h"

/* 🛑 NEVER CALL WiFiUDP::parsePacket() DIRECTLY. Use this instead.
 *
 * parsePacket() allocates a 1,460-byte buffer with `new char[1460]` on EVERY call, from the
 * INTERNAL heap, and the framework's own guard immediately underneath it:
 *
 *     char * buf = new char[1460];
 *     if(!buf){ return 0; }            <-- DEAD CODE
 *
 * never fires, because `new[]` THROWS on failure rather than returning null. Nothing catches
 * it, so the throw becomes std::terminate() -> abort() -> reset_reason=4. The framework author
 * wrote a graceful bail-out using malloc semantics and got a device-killing abort instead.
 *
 * This phone has ~20 KB of internal heap in total, so 1,460 CONTIGUOUS bytes is a real ask. It
 * went unnoticed while nothing polled UDP often; the moment a SIP account was registered,
 * TinySIP::checkCall() began polling it from the main loop and the phone pa{user}ed every one to
 * three minutes. Proven on hardware 2026-08-16 by decoding the panic backtrace:
 *
 *     loop -> TinySIP::checkCall -> UDP_SIPConnection::available -> WiFiUDP::parsePacket
 *          -> operator new[] -> __cxa_throw -> std::terminate -> abort
 *
 * Two layers, because either alone is insufficient:
 *   1. Refuse to call it when the largest internal block cannot comfortably hold the buffer.
 *      This is what prevents the abort in practice.
 *   2. Catch std::bad_alloc anyway — WiFi runs on the other core and can take the memory
 *      between our check and the allocation. Rare, but it is precisely the race that would
 *      otherwise kill the device at random.
 *
 * The cost of refusing is ONE dropped packet. UDP is unreliable by definition and every caller
 * here re-polls, so it is re-read next pass or retransmitted by the far end. */
inline int udpParsePacketSafe(UDP& udp) {
  // 1,460 for the buffer, plus headroom for the cbuf parsePacket() allocates when data arrives.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 1460 + 512) {
    return 0;
  }
  try {
    return udp.parsePacket();
  } catch (const std::bad_alloc&) {
    return 0;         // lost the race with the other core: drop the packet, keep the phone
  }
}

/* 🛑 NEVER CALL WiFi.scanNetworks() DIRECTLY. Ask wifiScanMemoryOk() first.
 *
 * The same bug as parsePacket() above, in a different framework file — and this is the one
 * that actually killed a phone. WiFiScanClass::_scanDone() does:
 *
 *     _scanResult = new wifi_ap_record_t[_scanCount];
 *     if (!_scanResult || ...) { _scanCount = 0; }        <-- DEAD CODE, new[] THROWS
 *
 * ⚠ AND IT CANNOT BE CAUGHT AT THE CALL SITE. _scanDone() runs on the WiFi event task, not
 * on the loop, so unlike udpParsePacketSafe() there is no try/catch we can wrap round it.
 * The ONLY lever the firmware has is deciding not to start the scan.
 *
 * MEASURED on phone 1, 2026-08-29, three scans, off the DROP instrument:
 *     n=19 APs -> largest fell 3,648    n=19 -> 3,648    n=18 -> 3,536
 * i.e. ~192 bytes of CONTIGUOUS internal heap per access point. ⚠ That is deliberately NOT
 * sizeof(wifi_ap_record_t) (~80 bytes): the driver allocates alongside the array while the
 * scan runs, and the number that predicts the crash is the total effect on `largest`.
 *
 * THE CRASH, exactly: the phone died with largest = 3,072 and "[autosw] scan started" as the
 * last line in the log. 19 APs needed 3,648. There were 3,072.
 *
 * ⚠ THIS IS A PREDICTION, NOT A GUARANTEE, and it is important to know which. The AP count
 * comes from the PREVIOUS scan; the allocation happens later, on another task, in an
 * environment that may have gained access points since. It turns "certain death in a dense
 * area on a fragmented phone" into "unlikely" — the same trade udpParsePacketSafe() makes,
 * for the same reason, and it is the best available: the count cannot be known in advance.
 *
 * The cost of refusing is ONE skipped scan. The phone stays on the AP it is already using,
 * and the normal backoff brings the next attempt round shortly. */
bool wifiScanMemoryOk(const char* who);

/* Tell the guard how many APs the last completed scan actually found, so the next estimate
 * is sized to THIS environment rather than a guess. Ignored for n <= 0 — an empty scan is
 * the deaf-radio/out-of-range case and says nothing about how crowded the air is. */
void wifiScanNoteResult(int apCount);

uint32_t rotate5(uint32_t x);                                   // rotate by a prime number of bits
uint32_t hash_murmur(const char *str);                          // simple hash function for C-strings. Uses MurmurHash by Austin Appleby. Only 35% slower than DJB2 on ESP32, 27% slower than SDBM.
void md5Compress(const char* str, size_t len, HASHHEX hash);    // convert string into up to 32-character string (either as hexadecimal hash representation or string itself)
uint8_t conv100to255(int x);

// Timer and time difference helpers

unsigned long elapsed(unsigned long now, unsigned long last);
bool elapsedMillis(unsigned long now, unsigned long last, unsigned long period);        // have a `period` of milliseconds passed since `last` millis? (handles overflow)
long timeDiff(unsigned long msTime1, unsigned long msTime2);                            // return assumed time difference between two close events

// Dynamic memory functions

void freeNull(void** p);                      // free pointer and set it to NULL after freeing
char* strrstrip(char* str);                   // Python's .rstrip()

void* intMalloc(size_t size);                 // allocate memory in internal RAM, if failed - in external PSRAM
void* intCalloc(size_t n, size_t size);       // allocate & zero memory in internal RAM, if failed - in external PSRAM
void* intRealloc(void *ptr, size_t size);     // reallocate memory in internal RAM, if failed - in external PSRAM
char* intStrdup(const char* str);
char* intStrndup(const char* str, size_t n);

void* extMalloc(size_t size);                 // allocate memory in external PSRAM, if failed - in internal RAM
void* extCalloc(size_t n, size_t size);       // allocate & zero memory in external PSRAM, if failed - in internal RAM
void* extRealloc(void *ptr, size_t size);     // reallocate memory in external PSRAM, if failed - in internal RAM
char* extStrdup(const char* str);
char* extStrndup(const char* str, size_t n);

template <bool B>
void* wMalloc(size_t size);                   // allocate memory, if parameter is true - identical to extMalloc, false - identical to intMalloc
template <bool B>
void* wCalloc(size_t n, size_t size);         // allocate & zero memory, if parameter is true - identical to extCalloc, false - identical to intCalloc
template <bool B>
void* wRealloc(void *ptr, size_t size);       // reallocate memory, if parameter is true - identical to extRealloc, false - identical to intRealloc
template <bool B>
char* wStrdup(const char* str);

/* Description:
 *     Since SIP protocol relies on random tags, we need a good and fast random number generator.
 *     This structure implements Permuted Congruential Generator (pcg32_fast) algorithm, as well as collects random bits from hardware events for the seed.
 *     In our tests this implementation sometimes significantly outperforms the standard Mersenne Twister (std::19937) on ESP32, in other cases - performs slighly worse.
 *       This is because MT performance is unstable. Not clear why.
 *     This implementation generates 10M numbers in around 2645 ms, while std::mt19937 needs between 2441 and 3398 ms depending on the exact use case.
 */
class RandomNumberGenerator {
public:
  void feed(uint32_t x);
  uint32_t random(void);
  void randChars(char *dest, size_t len);
protected:
  static uint64_t const multiplier = 6364136223846793005u;
  uint64_t mcg_state = 0xFeedCeedCafeF00Du;                   // must be odd
};

extern RandomNumberGenerator Random;
//extern std::random_device randDevice;

#endif // __HELPERS_H
