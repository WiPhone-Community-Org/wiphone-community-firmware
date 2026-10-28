/*
 * test_neighbor.cpp — the phone's hand-rolled NeighborInfo encoder vs the real
 * Meshtastic protobuf runtime (vectors_neighbor.h, generated).
 *
 * Byte equality is the bar, not "it parses": every other radio in the mesh
 * decodes this with nanopb against the same schema, so a field number or a
 * varint width that drifts makes our neighbour list invisible — the exact
 * failure that would look like "the mesh map just doesn't show the phone".
 */
#include "../WiPhone/neighbor_info.h"
#include "vectors_neighbor.h"

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

static void hexdump(const char* tag, const unsigned char* b, int n) {
  printf("    %s:", tag);
  for (int i = 0; i < n; i++) {
    printf(" %02x", b[i]);
  }
  printf("\n");
}

static void testVectors() {
  printf("\n\033[1mNeighborInfo bytes match the real protobuf runtime\033[0m\n");
  for (int i = 0; i < NBR_VEC_N; i++) {
    const NbrVec* v = &NBR_VEC[i];
    NeighborEntry ents[8];
    for (int k = 0; k < v->count && k < 8; k++) {
      ents[k].nodeNum = v->entries[k].node;
      ents[k].snr = v->entries[k].snr;
    }
    uint8_t out[256];
    const int n = neighborInfoEncode(out, sizeof(out), v->myNode, v->interval,
                                     v->count ? ents : NULL, v->count);
    char what[96];
    snprintf(what, sizeof(what), "'%s' encodes", v->label);
    ok(n >= 0, what);
    snprintf(what, sizeof(what), "'%s' length %d == %d", v->label, n, v->len);
    ok(n == v->len, what);
    snprintf(what, sizeof(what), "'%s' bytes identical", v->label);
    const bool same = (n == v->len) && memcmp(out, v->bytes, (size_t)n) == 0;
    ok(same, what);
    if (!same && n >= 0) {
      hexdump("ours", out, n);
      hexdump("theirs", v->bytes, v->len);
    }
  }
}

static void testCaps() {
  printf("\n\033[1mcapacity and refusal\033[0m\n");
  NeighborEntry e[4] = { { 0x11111111u, 1.0f }, { 0x22222222u, 2.0f },
                         { 0x33333333u, 3.0f }, { 0x44444444u, 4.0f } };
  uint8_t out[256];
  ok(neighborInfoEncode(out, 8, 0x62B8D2FDu, 3600, e, 4) == -1,
     "a payload that cannot fit refuses rather than truncating a neighbour");
  ok(neighborInfoEncode(out, 4, 0x62B8D2FDu, 3600, NULL, 0) == -1,
     "even the header refuses when it cannot fit");
  const int cap = neighborInfoCapacity(200, 0x62B8D2FDu, 3600);
  ok(cap > 0 && cap < 20, "capacity gives a sane count for a 200-byte payload");
  const int n = neighborInfoEncode(out, 200, 0x62B8D2FDu, 3600, e, cap < 4 ? cap : 4);
  ok(n > 0 && n <= 200, "encoding at the advertised capacity fits");
  /* The worst-case estimate must never over-promise: fill to capacity with
   * the widest possible ids and check it still fits. */
  NeighborEntry wide[24];
  const int capw = neighborInfoCapacity(200, 0xFFFFFFFFu, 4294967295u);
  for (int i = 0; i < capw && i < 24; i++) {
    wide[i].nodeNum = 0xFFFFFFFFu;
    wide[i].snr = -0.25f;
  }
  ok(neighborInfoEncode(out, 200, 0xFFFFFFFFu, 4294967295u, wide,
                        capw < 24 ? capw : 24) > 0,
     "capacity holds for the widest ids (worst-case estimate is honest)");
}

int main() {
  testVectors();
  testCaps();
  printf("\n%s%d passed, %d failed\033[0m\n", g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
