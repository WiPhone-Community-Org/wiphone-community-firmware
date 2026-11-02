/*
 * mesh_airtime.cpp — see mesh_airtime.h. Pure arithmetic, no Arduino, host-tested by
 * tests/test_airtime.cpp against numbers worked by hand from Semtech AN1200.13.
 */

#include "mesh_airtime.h"

uint32_t meshLoraAirtimeUs(size_t frameLen, unsigned sf, uint32_t bwHz, unsigned cr,
                           unsigned preambleSyms, bool crcOn, bool explicitHeader, bool ldro) {
  if (sf < 6 || sf > 12 || bwHz == 0) {
    return 0;
  }
  /* Preamble: (n + 4.25) symbols, carried as quarter-symbols so the arithmetic stays
   * integer: 4n + 17. */
  const uint32_t preambleQuarterSyms = 4u * preambleSyms + 17u;

  /* Payload symbols, AN1200.13. The numerator can go non-positive for a tiny frame at a high
   * SF, where the datasheet's max(..., 0) applies — an integer ceil of a negative number is
   * not what it means, so the sign is tested first. */
  const int32_t num = 8 * (int32_t)frameLen - 4 * (int32_t)sf + 28 + (crcOn ? 16 : 0)
                      - (explicitHeader ? 0 : 20);
  const int32_t den = 4 * ((int32_t)sf - (ldro ? 2 : 0));
  const int32_t blocks = num > 0 ? (num + den - 1) / den : 0;
  const uint32_t payloadSyms = 8u + (uint32_t)blocks * (cr + 4u);

  const uint64_t quarterSyms = preambleQuarterSyms + 4ull * payloadSyms;
  /* Tsym = 2^sf / bw seconds = (10^6 << sf) / bw microseconds; a quarter of that per unit. */
  const uint64_t us = (quarterSyms * (1000000ull << sf)) / (4ull * bwHz);
  return (uint32_t)us;
}

uint32_t meshLoraAirtimeMs(size_t frameLen) {
  const uint32_t us = meshLoraAirtimeUs(frameLen, MESH_LORA_SF, MESH_LORA_BW_HZ, MESH_LORA_CR,
                                        MESH_LORA_PREAMBLE_SYMS, MESH_LORA_CRC_ON,
                                        MESH_LORA_EXPLICIT_HDR, MESH_LORA_LDRO);
  return (us + 999u) / 1000u;                   // round UP: a wait must cover the frame
}

uint32_t meshLoraTxTimeoutMs(size_t frameLen) {
  const uint32_t air = meshLoraAirtimeMs(frameLen);
  const uint32_t wait = air + air / 4u + 300u;
  return wait < 1000u ? 1000u : wait;
}

size_t meshDataOverhead(size_t payloadLen) {
  /* 0x08 0x01 (portnum TEXT_MESSAGE, one-byte varint) + 0x48 0x00 (bitfield) = 4, then the
   * payload field: tag 0x12 + a length varint of one byte below 128, two from 128 up. An
   * empty payload is omitted entirely — proto3 canonical, and what meshBuildData does. */
  size_t n = 4;
  if (payloadLen) {
    n += 1 + (payloadLen < 128 ? 1 : 2);
  }
  return n;
}

size_t meshTextBudget(bool pki) {
  const size_t room = (size_t)MESH_LORA_FRAME_MAX - (size_t)MESH_HEADER_LEN
                      - (pki ? (size_t)MESH_PKI_OVERHEAD : 0);
  /* Walk down from "all of it is text" until the text plus its own envelope fits. The
   * envelope shrinks by a byte at 127, so this converges in a handful of steps and cannot
   * be wrong by an off-by-one the way a closed form with the varint boundary can. */
  size_t t = room;
  while (t > 0 && t + meshDataOverhead(t) > room) {
    t--;
  }
  return t;
}

size_t meshComposeCap(bool pki) {
  const size_t budget = meshTextBudget(pki);
  return budget < (size_t)MESH_TEXT_CLIENT_CAP ? budget : (size_t)MESH_TEXT_CLIENT_CAP;
}
