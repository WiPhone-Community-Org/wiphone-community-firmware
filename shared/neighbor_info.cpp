/*
 * neighbor_info.cpp — see neighbor_info.h.
 *
 * Hand-rolled protobuf, matching the style of the NodeInfo/Position encoders
 * in meshtastic_service.cpp. The field numbers below come from the shipped
 * protobufs (mesh_pb2 NeighborInfo/Neighbor), and tests/test_neighbor.cpp
 * proves the bytes decode identically in python's protobuf runtime.
 */

#include "neighbor_info.h"
#include <string.h>

/* varint: node numbers are full 32-bit, so this is up to 5 bytes — the
 * single-byte shortcut used elsewhere for portnums would silently corrupt
 * every node id above 127. */
static size_t niVarint(uint8_t* out, uint32_t v) {
  size_t n = 0;
  while (v >= 0x80) {
    out[n++] = (uint8_t)(v | 0x80);
    v >>= 7;
  }
  out[n++] = (uint8_t)v;
  return n;
}

/* proto3 ELIDES default-valued scalars, and the real runtime's bytes are the
 * contract here: emitting `snr = 0` where python emits nothing made our packet
 * 5 bytes longer and byte-unequal (caught by the generated vectors, exactly
 * what they exist for). Zero varints are therefore skipped entirely. */
static size_t niTagVarint(uint8_t* out, int field, uint32_t v) {
  if (v == 0) {
    return 0;
  }
  size_t n = niVarint(out, (uint32_t)(field << 3) | 0);
  n += niVarint(out + n, v);
  return n;
}

/* float, little-endian IEEE754 (protobuf wire type 5). memcpy rather than a
 * pointer cast: the cast is an aliasing violation and gcc is entitled to
 * miscompile it. */
static size_t niTagFloat(uint8_t* out, int field, float f) {
  uint32_t bits;
  memcpy(&bits, &f, 4);
  /* Bit pattern, not `f == 0.0f`: NEGATIVE zero is a distinct pattern and the
   * reference runtime serializes it, so comparing numerically would drop a
   * field python keeps. Only true +0.0 is elided. */
  if (bits == 0) {
    return 0;
  }
  size_t n = niVarint(out, (uint32_t)(field << 3) | 5);
  out[n++] = (uint8_t)(bits >> 0);
  out[n++] = (uint8_t)(bits >> 8);
  out[n++] = (uint8_t)(bits >> 16);
  out[n++] = (uint8_t)(bits >> 24);
  return n;
}

/* One Neighbor submessage: node_id (1) + snr (2). Nothing else travels. */
static size_t niNeighbor(uint8_t* out, const NeighborEntry* e) {
  uint8_t body[16];
  size_t b = niTagVarint(body, 1, e->nodeNum);
  b += niTagFloat(body + b, 2, e->snr);
  size_t n = niVarint(out, (uint32_t)(4 << 3) | 2);   // field 4, length-delimited
  n += niVarint(out + n, (uint32_t)b);
  memcpy(out + n, body, b);
  return n + b;
}

static size_t niHeader(uint8_t* out, uint32_t myNodeNum, uint32_t intervalSecs) {
  size_t n = niTagVarint(out, 1, myNodeNum);            // node_id
  n += niTagVarint(out + n, 2, myNodeNum);              // last_sent_by_id: us, we originated it
  n += niTagVarint(out + n, 3, intervalSecs);           // node_broadcast_interval_secs
  return n;
}

int neighborInfoEncode(uint8_t* out, size_t cap,
                       uint32_t myNodeNum, uint32_t intervalSecs,
                       const NeighborEntry* neighbors, int count) {
  uint8_t hdr[32];
  const size_t h = niHeader(hdr, myNodeNum, intervalSecs);
  if (h > cap) {
    return -1;
  }
  memcpy(out, hdr, h);
  size_t n = h;
  for (int i = 0; i < count; i++) {
    uint8_t entry[32];
    const size_t e = niNeighbor(entry, &neighbors[i]);
    if (n + e > cap) {
      return -1;                    // caller should have trimmed: see capacity()
    }
    memcpy(out + n, entry, e);
    n += e;
  }
  return (int)n;
}

int neighborInfoCapacity(size_t cap, uint32_t myNodeNum, uint32_t intervalSecs) {
  uint8_t hdr[32];
  const size_t h = niHeader(hdr, myNodeNum, intervalSecs);
  if (h >= cap) {
    return 0;
  }
  /* Worst case per neighbour: tag(1) + len(1) + node_id tag(1)+varint(5) +
   * snr tag(1)+float(4) = 13. Using the worst case rather than measuring each
   * means the answer never over-promises. */
  return (int)((cap - h) / 13);
}
