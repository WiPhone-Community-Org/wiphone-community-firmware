/*
 * mesh_wire.cpp — see mesh_wire.h. Lifted out of meshtastic_service.cpp on 2026-09-02 so
 * the host suite can reach it.
 *
 * ⚠ THE MOVE ITSELF WAS VERBATIM, BUT FOUR THINGS HAVE DELIBERATELY CHANGED SINCE, and an
 * earlier version of this comment claimed "the bytes are unchanged", which was wrong:
 *   1. meshFillHeader takes hopLimit as a parameter instead of reading a file static.
 *   2. meshBuildData omits Data field 2 when the payload is empty (proto3 canonical).
 *   3. meshBuildData writes portnum as a real varint — it was one raw byte, which
 *      truncated any portnum >= 128 to garbage.
 *   4. meshBuildUser omits an empty long_name/short_name, and meshParseUserName now falls
 *      back to short_name when long_name is present but empty.
 * Every one of those is asserted against upstream's runtime in tests/test_wire.cpp.
 */

#include "mesh_wire.h"

#include <string.h>
#include <stdio.h>

size_t meshPbBytes(uint8_t* out, int field, const uint8_t* val, size_t len) {
  size_t d = 0;
  out[d++] = (uint8_t)((field << 3) | 2);
  if (len < 128) {
    out[d++] = (uint8_t)len;
  } else {
    out[d++] = (uint8_t)((len & 0x7f) | 0x80);
    out[d++] = (uint8_t)(len >> 7);
  }
  memcpy(out + d, val, len);
  return d + len;
}

size_t meshPbString(uint8_t* out, int field, const char* s) {
  return meshPbBytes(out, field, (const uint8_t*)s, strlen(s));
}

/* A base-128 varint. ⚠ portnum USED TO BE WRITTEN AS ONE RAW BYTE, which silently
 * truncated: portnum 256 (PRIVATE_APP) encoded as 0x08 0x00 — a Data addressed to
 * portnum ZERO. Every portnum this firmware speaks today is < 128 so nothing on air was
 * ever wrong, but Meshtastic's own enum runs to 511 and the next person to add a portnum
 * would have shipped a silent mis-address. Found by tests/test_wire.cpp's review. */
static size_t pbVarintRaw(uint8_t* out, uint32_t v) {
  size_t d = 0;
  while (v >= 0x80) {
    out[d++] = (uint8_t)(v | 0x80);
    v >>= 7;
  }
  out[d++] = (uint8_t)v;
  return d;
}

size_t meshBuildData(uint8_t* data, int portnum,
                     const uint8_t* payload, size_t payloadLen,
                     bool wantResponse, uint32_t requestId) {
  size_t d = 0;
  data[d++] = 0x08;                          // field 1 portnum (varint)
  d += pbVarintRaw(data + d, (uint32_t)portnum);
  /* ⚠ SKIP AN EMPTY PAYLOAD. proto3 omits a field holding its default, so upstream encodes
   * an empty Data.payload as nothing at all while this used to emit `12 00`. The two decode
   * identically (an absent `bytes` field reads as empty), so no packet was ever misread —
   * but it was two bytes of airtime and a permanent diff against every other node's
   * encoding. Found by tests/test_wire.cpp on the day it was written. */
  if (payloadLen) {
    d += meshPbBytes(data + d, 2, payload, payloadLen);   // field 2 payload
  }
  if (wantResponse) {
    data[d++] = 0x18;                        // field 3 want_response (varint)
    data[d++] = 0x01;
  }
  if (requestId != 0) {
    data[d++] = 0x35;                        // field 6 request_id (fixed32)
    data[d++] = (uint8_t)(requestId >> 0);
    data[d++] = (uint8_t)(requestId >> 8);
    data[d++] = (uint8_t)(requestId >> 16);
    data[d++] = (uint8_t)(requestId >> 24);
  }
  data[d++] = 0x48;                          // field 9 bitfield (varint)
  data[d++] = wantResponse ? 0x02 : 0x00;
  return d;
}

size_t meshBuildUser(uint8_t* out, uint32_t nodeNum, const char* longName,
                     const char* shortName, const uint8_t* pubKey) {
  char id[12];
  snprintf(id, sizeof(id), "!%08x", (unsigned)nodeNum);
  size_t u = 0;
  u += meshPbString(out + u, 1, id);              // field 1 id
  /* ⚠ SAME RULE AS THE PAYLOAD: proto3 omits a string field holding its default, so an
   * empty name must emit NOTHING, not `12 00`. This one also bit our own decoder — see
   * meshParseUserName, where a present-but-empty long_name used to suppress the short_name
   * fallback and the node showed up with no name at all. */
  if (longName && longName[0]) {
    u += meshPbString(out + u, 2, longName);      // field 2 long_name
  }
  if (shortName && shortName[0]) {
    u += meshPbString(out + u, 3, shortName);     // field 3 short_name
  }
  if (pubKey) {
    u += meshPbBytes(out + u, 8, pubKey, MESH_PKI_KEY_LEN);   // field 8 public_key
  }
  return u;
}

void meshFillHeader(MeshPacketHeader* hdr, uint32_t sender, uint32_t dest,
                    uint32_t packetId, uint8_t channelHash, uint8_t hopLimit) {
  hdr->dest        = dest;
  hdr->sender      = sender;
  hdr->packetId    = packetId;
  hdr->flags       = (hopLimit & MESH_FLAGS_HOP_LIMIT_MASK) |
                     (hopLimit << MESH_FLAGS_HOP_START_SHIFT);
  hdr->channelHash = channelHash;
  hdr->nextHop     = 0;
  hdr->relayNode   = (uint8_t)(sender & 0xFF);   // originator is the first relay
}

int meshParseData(const uint8_t* data, size_t dlen,
                  const uint8_t** payloadOut, size_t* payloadLenOut,
                  bool* wantRespOut, uint32_t* requestIdOut) {
  int portnum = -1;
  const uint8_t* payload = NULL;
  size_t payloadLen = 0;
  if (wantRespOut) {
    *wantRespOut = false;
  }
  if (requestIdOut) {
    *requestIdOut = 0;
  }
  size_t i = 0;
  while (i < dlen) {
    uint8_t tag = data[i++];
    uint8_t field = tag >> 3;
    uint8_t wire = tag & 0x07;
    if (wire == 0) {                       // varint
      /* `shift < 32` guards: a 10-byte varint (any negative int32) is legal and would
       * otherwise shift a uint32_t by 35..63 — UB, masked on Xtensa. See rdVarint in
       * mesh_pos.cpp for the full note; same fix, same day. */
      uint32_t v = 0; int shift = 0;
      while (i < dlen && (data[i] & 0x80)) { if (shift < 32) v |= (uint32_t)(data[i] & 0x7f) << shift; shift += 7; i++; }
      if (i < dlen) { if (shift < 32) v |= (uint32_t)(data[i] & 0x7f) << shift; i++; }
      if (field == 1) {
        portnum = (int)v;
      } else if (field == 3 && wantRespOut) {
        *wantRespOut = (v != 0);
      }
    } else if (wire == 2) {                // length-delimited
      uint32_t l = 0; int shift = 0;
      while (i < dlen && (data[i] & 0x80)) { if (shift < 32) l |= (uint32_t)(data[i] & 0x7f) << shift; shift += 7; i++; }
      if (i < dlen) { if (shift < 32) l |= (uint32_t)(data[i] & 0x7f) << shift; i++; }
      if (field == 2) { payload = data + i; payloadLen = l; }
      i += l;
    } else if (wire == 5) {                // fixed32
      if (field == 6 && requestIdOut && i + 4 <= dlen) {
        *requestIdOut = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                        ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
      }
      i += 4;
    }
    else if (wire == 1) { i += 8; }
    else break;
  }
  if (payload) {                           // clamp to the actual buffer
    size_t avail = (size_t)(data + dlen - payload);
    if (payloadLen > avail) payloadLen = avail;
  }
  if (payloadOut)    *payloadOut = payload;
  if (payloadLenOut) *payloadLenOut = payloadLen;
  return portnum;
}

bool meshParseUserName(const uint8_t* data, size_t dlen, char* out, size_t outCap,
                       uint8_t* pubKeyOut, bool* hasKeyOut) {
  const uint8_t* longName = NULL;  size_t longLen = 0;
  const uint8_t* shortName = NULL; size_t shortLen = 0;
  if (hasKeyOut) {
    *hasKeyOut = false;
  }
  size_t i = 0;
  while (i < dlen) {
    uint8_t tag = data[i++];
    uint8_t field = tag >> 3;
    uint8_t wire = tag & 0x07;
    if (wire == 2) {
      uint32_t l = 0; int shift = 0;
      while (i < dlen && (data[i] & 0x80)) { if (shift < 32) l |= (uint32_t)(data[i] & 0x7f) << shift; shift += 7; i++; }
      if (i < dlen) { if (shift < 32) l |= (uint32_t)(data[i] & 0x7f) << shift; i++; }
      if (field == 2)      { longName = data + i;  longLen = l; }
      else if (field == 3) { shortName = data + i; shortLen = l; }
      else if (field == 8 && l == MESH_PKI_KEY_LEN && pubKeyOut && hasKeyOut &&
               i + MESH_PKI_KEY_LEN <= dlen) {
        memcpy(pubKeyOut, data + i, MESH_PKI_KEY_LEN);
        *hasKeyOut = true;
      }
      i += l;
    } else if (wire == 0) {
      while (i < dlen && (data[i] & 0x80)) i++;
      if (i < dlen) i++;
    } else if (wire == 5) { i += 4; }
    else if (wire == 1) { i += 8; }
    else break;
  }
  /* ⚠ `longLen` IS PART OF THE TEST, not just `longName`. A peer that encodes an empty
   * long_name as a present-but-empty field (`12 00` — non-canonical, but legal, and what
   * this firmware itself used to emit) would otherwise select it, hit the srcLen==0 bail
   * below, and be rendered as a bare node number with a perfectly good short_name sitting
   * unread two bytes away. */
  const bool haveLong = (longName != NULL && longLen > 0);
  const uint8_t* src = haveLong ? longName : shortName;
  size_t srcLen      = haveLong ? longLen  : shortLen;
  if (!src || srcLen == 0) {
    return false;
  }
  size_t avail = (size_t)(data + dlen - src);
  size_t n = srcLen < avail ? srcLen : avail;
  if (n > outCap - 1) n = outCap - 1;
  memcpy(out, src, n);
  out[n] = '\0';
  return true;
}
