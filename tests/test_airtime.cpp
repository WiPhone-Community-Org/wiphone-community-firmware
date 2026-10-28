/*
 * test_airtime.cpp — mesh_airtime.cpp: LoRa time-on-air at the phone's own registers, and
 * the text budget of one frame.
 *
 * The airtime references were worked BY HAND from Semtech AN1200.13 (not from this code),
 * for SF11, BW 250 kHz, CR 4/5, explicit header, CRC on, LDRO off, 16-symbol preamble:
 *     Tsym = 2^11 / 250000 = 8.192 ms          Tpreamble = 20.25 * 8.192 = 165.888 ms
 *     PL=254: ceil((2032-44+28+16)/44)=47 -> 8+47*5=243 symbols -> 1990.656 + 165.888 = 2156.544 ms
 *     PL=255: ceil(2040/44)=47 -> 243 symbols, the same block -> 2156.544 ms
 *     PL=249: ceil(1992/44)=46 -> 238 symbols -> 1949.696 + 165.888 = 2115.584 ms
 *     PL= 60: ceil( 480/44)=11 ->  63 symbols ->  516.096 + 165.888 =  681.984 ms
 *     PL= 45: ceil( 360/44)= 9 ->  53 symbols ->  434.176 + 165.888 =  600.064 ms
 *     PL= 44: ceil( 352/44)= 8 ->  48 symbols ->  393.216 + 165.888 =  559.104 ms
 *     PL= 42: ceil( 336/44)= 8 ->  48 symbols ->  393.216 + 165.888 =  559.104 ms
 *     PL= 37: ceil( 296/44)= 7 ->  43 symbols ->  352.256 + 165.888 =  518.144 ms
 *     PL= 20: ceil( 160/44)= 4 ->  28 symbols ->  229.376 + 165.888 =  395.264 ms
 *     PL=  1: ceil(   8/44)= 1 ->  13 symbols ->  106.496 + 165.888 =  272.384 ms
 * 37 B is the position beacon this repo's comments have called "518 ms" since 0.9.2x; the
 * hand figure agrees, and the rounded-up millisecond the timeout uses is 519. 42 B is the
 * same beacon once it says where its fix came from (0.9.82: location_source + precision_bits,
 * a 20 B Position, mesh_pos.h's MESH_POS_BUILD_EX_MAX): 559 ms, and 44 B (22 B) still is —
 * 45 B is where the next block of symbols starts.
 *
 * The budget half does not trust the arithmetic in meshDataOverhead(): it builds the Data
 * with the shipping meshBuildData() and measures the frame.
 */
#include "../WiPhone/mesh_airtime.h"
#include "../WiPhone/mesh_wire.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

static void ok(bool cond, const char* what) {
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  }
}

static void checkAir(size_t len, uint32_t expectUs, uint32_t expectMs) {
  char what[96];
  const uint32_t us = meshLoraAirtimeUs(len, MESH_LORA_SF, MESH_LORA_BW_HZ, MESH_LORA_CR,
                                        MESH_LORA_PREAMBLE_SYMS, MESH_LORA_CRC_ON,
                                        MESH_LORA_EXPLICIT_HDR, MESH_LORA_LDRO);
  snprintf(what, sizeof(what), "%u B: %u us (hand: %u)", (unsigned)len, (unsigned)us, (unsigned)expectUs);
  ok(us == expectUs, what);
  const uint32_t ms = meshLoraAirtimeMs(len);
  snprintf(what, sizeof(what), "%u B: %u ms rounded up (hand: %u)", (unsigned)len, (unsigned)ms, (unsigned)expectMs);
  ok(ms == expectMs, what);
}

static void testAirtime() {
  printf("\n\033[1mTime on air at SF11 / BW250 / CR4-5 / 16-symbol preamble (AN1200.13 by hand)\033[0m\n");
  checkAir(254, 2156544u, 2157u);
  checkAir(255, 2156544u, 2157u);   // same 243-symbol block as 254
  checkAir(249, 2115584u, 2116u);   // stock's own worst case: DATA_PAYLOAD_LEN + header
  checkAir(60,   681984u,  682u);
  checkAir(45,   600064u,  601u);   // a 23 B Position: one block more (why no altitude is sent)
  checkAir(44,   559104u,  560u);   // a 22 B Position: the largest at the beacon's airtime
  checkAir(42,   559104u,  560u);   // the beacon with location_source + precision_bits (mesh_pos.h)
  checkAir(37,   518144u,  519u);   // the pin announce: meshPosBuild's 15 B
  checkAir(20,   395264u,  396u);
  checkAir(1,    272384u,  273u);
  checkAir(0,    231424u,  232u);   // numerator 0: the max(...,0) branch, 8 payload symbols

  /* The parameters are wired, not decorative. LDRO halves the useful bits per symbol
   * (den 44 -> 36): PL=254 -> ceil(2032/36)=57 -> 293 symbols -> 2,566,144 us. */
  ok(meshLoraAirtimeUs(254, 11, 250000, 1, 16, true, true, true) == 2566144u,
     "LDRO on: 254 B -> 2566144 us");
  /* Implicit header drops 20 from the numerator. At PL=23 that crosses a block boundary:
   * explicit ceil(184/44)=5 -> 33 symbols, implicit ceil(164/44)=4 -> 28, so one block of
   * (CR+4)=5 symbols = 40,960 us less. */
  ok(meshLoraAirtimeUs(23, 11, 250000, 1, 16, true, true, false)
     - meshLoraAirtimeUs(23, 11, 250000, 1, 16, true, false, false) == 40960u,
     "implicit header: 23 B is five symbols shorter");
  /* CRC off drops 16. At PL=18: with CRC ceil(144/44)=4 -> 28 symbols, without
   * ceil(128/44)=3 -> 23, again one block of 5 symbols. */
  ok(meshLoraAirtimeUs(18, 11, 250000, 1, 16, true, true, false)
     - meshLoraAirtimeUs(18, 11, 250000, 1, 16, false, true, false) == 40960u,
     "CRC off: 18 B is five symbols shorter");
  ok(meshLoraAirtimeUs(10, 5, 250000, 1, 16, true, true, false) == 0, "SF below 6 refused");
  ok(meshLoraAirtimeUs(10, 11, 0, 1, 16, true, true, false) == 0, "BW zero refused");

  bool monotonic = true, covered = true;
  uint32_t prev = 0;
  for (size_t len = 0; len <= 255; len++) {
    const uint32_t ms = meshLoraAirtimeMs(len);
    if (ms < prev) {
      monotonic = false;
    }
    if (meshLoraTxTimeoutMs(len) < ms + 300u) {
      covered = false;
    }
    prev = ms;
  }
  ok(monotonic, "airtime never decreases with length, 0..255");
  ok(covered, "the TX timeout exceeds the airtime by at least 300 ms for every length");
}

static void testTimeout() {
  printf("\n\033[1mTX timeout = airtime + 25%% + 300 ms, floor 1000\033[0m\n");
  ok(meshLoraTxTimeoutMs(254) == 2996u, "254 B: 2157 + 539 + 300 = 2996 ms (~3 s, the new worst-case stall)");
  ok(meshLoraTxTimeoutMs(60)  == 1152u, "60 B: 682 + 170 + 300 = 1152 ms");
  ok(meshLoraTxTimeoutMs(37)  == 1000u, "37 B: 519 + 129 + 300 = 948 -> floor 1000 ms (the beacon waits what it always did)");
  ok(meshLoraTxTimeoutMs(20)  == 1000u, "20 B: floor 1000 ms");
  ok(meshLoraTxTimeoutMs(255) > 2157u,  "255 B: the old flat 2000 ms could never have covered it");
}

static void testOverhead() {
  printf("\n\033[1mmeshDataOverhead() agrees with what meshBuildData() actually emits, 0..250\033[0m\n");
  uint8_t payload[256];
  memset(payload, 'x', sizeof(payload));
  uint8_t out[512];
  bool agree = true;
  size_t firstBad = 0;
  for (size_t len = 0; len <= 250; len++) {
    const size_t d = meshBuildData(out, MESH_PORT_TEXT_MESSAGE, payload, len, false, 0);
    if (d != len + meshDataOverhead(len)) {
      agree = false;
      firstBad = len;
      break;
    }
  }
  if (!agree) {
    printf("  first disagreement at len=%u\n", (unsigned)firstBad);
  }
  ok(agree, "overhead matches the encoder for every length 0..250");
  ok(meshDataOverhead(0) == 4,   "empty text: portnum + bitfield only (payload field omitted)");
  ok(meshDataOverhead(127) == 6, "127 B: one-byte length varint");
  ok(meshDataOverhead(128) == 7, "128 B: two-byte length varint");
}

static void testBudget() {
  printf("\n\033[1mThe longest text one frame holds, measured with meshBuildData()\033[0m\n");
  uint8_t payload[256];
  memset(payload, 'x', sizeof(payload));
  uint8_t out[512];

  const size_t chan = meshTextBudget(false);
  const size_t pki  = meshTextBudget(true);
  ok(chan == 232, "plain channel budget is 232 (255 - 16 header - 7 Data envelope)");
  ok(pki  == 220, "PKI DM budget is 220 (12 more for the CCM tag + extraNonce)");

  size_t d = meshBuildData(out, MESH_PORT_TEXT_MESSAGE, payload, chan, false, 0);
  ok(MESH_HEADER_LEN + d == MESH_LORA_FRAME_MAX, "232 B text builds a frame of exactly 255");
  d = meshBuildData(out, MESH_PORT_TEXT_MESSAGE, payload, chan + 1, false, 0);
  ok(MESH_HEADER_LEN + d == MESH_LORA_FRAME_MAX + 1, "233 B text builds 256: one byte too many (and uint8_t length would wrap to 0)");

  d = meshBuildData(out, MESH_PORT_TEXT_MESSAGE, payload, pki, false, 0);
  ok(MESH_HEADER_LEN + d + MESH_PKI_OVERHEAD == MESH_LORA_FRAME_MAX, "220 B PKI text builds a frame of exactly 255");
  d = meshBuildData(out, MESH_PORT_TEXT_MESSAGE, payload, pki + 1, false, 0);
  ok(MESH_HEADER_LEN + d + MESH_PKI_OVERHEAD == MESH_LORA_FRAME_MAX + 1, "221 B PKI text is one byte too many");

  /* Ecosystem sanity: stock python's sendText passes 233 bytes and a current node NAKs it
   * TOO_LARGE because of the bitfield; 232 is the number that flies. Same answer here. */
  ok(chan == 232, "agrees with upstream: 232 is the most a stock node will put on the air");
}

static void testComposeCap() {
  printf("\n\033[1mThe compose cap: the smaller of the wire budget and the phone apps' 200\033[0m\n");
  ok(MESH_TEXT_CLIENT_CAP == 200, "the Android/iOS limit is 200 UTF-8 bytes");
  ok(meshComposeCap(false) == 200, "channel compose cap 200");
  ok(meshComposeCap(true)  == 200, "DM compose cap 200");
  ok(meshComposeCap(false) <= meshTextBudget(false), "channel cap never exceeds what a frame holds");
  ok(meshComposeCap(true)  <= meshTextBudget(true),  "DM cap never exceeds what a PKI frame holds");
  ok(meshComposeCap(true)  <= meshComposeCap(false), "a DM is never allowed more than a broadcast");
  /* A 200-byte text is 223 B on the air (200 + 7 + 16) — 1.9 s, and the wait for it 2.7 s. */
  ok(meshLoraAirtimeMs(200 + meshDataOverhead(200) + MESH_HEADER_LEN) == 1911u,
     "a maximum compose text is 1911 ms on the air");
}

int main() {
  testAirtime();
  testTimeout();
  testOverhead();
  testBudget();
  testComposeCap();
  printf("\n%s%d passed, %d failed\033[0m\n", g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
