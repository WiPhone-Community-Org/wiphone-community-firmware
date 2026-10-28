/*
 * test_dbfile.cpp — mesh_dbfile.cpp: is a leftover /meshdb.tmp (or /meshfav.tmp) WHOLE?
 *
 * WHY THIS IS WORTH A SUITE: boot now RENAMES a temp file over a missing database when the last
 * save was cut between its remove() and its rename(). Getting "whole" wrong either way is silent:
 * too strict and the one good copy is truncated by the next save, exactly as before; too lax and a
 * half-written image becomes the database — a node table of garbage, messages from nowhere, and
 * it is persisted again on the next save. So every truncation of a real image must fail, the one
 * whole length must pass, and so must nothing else: a byte over, a stale layout, a count that
 * would wrap.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#include "../WiPhone/mesh_dbfile.h"

static int failures = 0;
static int checks = 0;

static void group(const char* name) {
  printf("\n%s\n", name);
}

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  FAIL %s\n", what);
  } else {
    printf("  ok  %s\n", what);
  }
}

static const uint32_t MAGIC = 0x314D5057u;   // "WPM1", as MESH_DB_MAGIC
static const uint16_t VER = 4;
static const uint16_t NS = 80, MS = 248, WS = 44;   // the shapes today's structs have (any work)

static void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
}
static void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back((uint8_t)x);
  v.push_back((uint8_t)(x >> 8));
}

/* An image laid out exactly as saveDb() writes one. */
static std::vector<uint8_t> image(int nc, int mc, int wc, uint16_t ver = VER, uint16_t ns = NS,
                                  uint16_t ms = MS, uint16_t ws = WS) {
  std::vector<uint8_t> v;
  put32(v, MAGIC);
  put16(v, ver);
  put16(v, ns);
  put16(v, ms);
  put16(v, ws);
  put32(v, (uint32_t)nc);
  for (int i = 0; i < nc * ns; i++) v.push_back((uint8_t)(i * 7));
  put32(v, (uint32_t)mc);
  for (int i = 0; i < mc * ms; i++) v.push_back((uint8_t)(i * 13));
  put32(v, (uint32_t)wc);
  for (int i = 0; i < wc * ws; i++) v.push_back((uint8_t)i);
  return v;
}

static bool whole(const std::vector<uint8_t>& v, size_t len) {
  return meshDbImageComplete(v.data(), len, MAGIC, VER, NS, MS, WS);
}

int main() {
  printf("test_dbfile\n");

  group("a whole database image passes; every truncation of it fails");
  {
    std::vector<uint8_t> v = image(3, 5, 2);
    ok(whole(v, v.size()), "3 nodes, 5 messages, 2 places: the exact length is whole");
    bool anyShortPassed = false;
    for (size_t n = 0; n < v.size(); n++) {
      if (whole(v, n)) anyShortPassed = true;
    }
    ok(!anyShortPassed, "not ONE of its shorter lengths passes (a save cut mid-write)");
    std::vector<uint8_t> over = v;
    over.push_back(0);
    ok(!whole(over, over.size()), "one byte over fails (not what the header says)");

    std::vector<uint8_t> empty = image(0, 0, 0);
    ok(whole(empty, empty.size()) && empty.size() == 24, "an empty database (24 bytes) is whole");

    /* The largest a card save writes: 200 nodes, 200 messages, 8 places (~66 KB). */
    std::vector<uint8_t> big = image(200, 200, 8);
    ok(whole(big, big.size()), "the largest card image (~66 KB) is whole");
    ok(!whole(big, big.size() - 1), "...and one byte short of it is not");
    ok(!whole(big, 65536), "...nor is its first 64 KB (one chunk written, then the power went)");
  }

  group("a header this build did not write fails");
  {
    std::vector<uint8_t> v = image(1, 1, 1);
    std::vector<uint8_t> bad = v;
    bad[0] ^= 1;
    ok(!whole(bad, bad.size()), "wrong magic");
    std::vector<uint8_t> v3 = image(1, 1, 1, 3);
    ok(!whole(v3, v3.size()), "an older version (a firmware update in between): not taken");
    std::vector<uint8_t> ns = image(1, 1, 1, VER, 84);
    ok(!whole(ns, ns.size()), "a different node record size");
    std::vector<uint8_t> ms = image(1, 1, 1, VER, NS, 240);
    ok(!whole(ms, ms.size()), "a different message record size");
    std::vector<uint8_t> ws = image(1, 1, 1, VER, NS, MS, 40);
    ok(!whole(ws, ws.size()), "a different waypoint record size");
    ok(!meshDbImageComplete(NULL, 100, MAGIC, VER, NS, MS, WS), "NULL fails");
    ok(!whole(v, 11), "shorter than the 12-byte header fails");
  }

  group("counts that lie fail, and cannot wrap the arithmetic");
  {
    std::vector<uint8_t> v = image(2, 0, 0);
    // node count -> -1
    v[12] = v[13] = v[14] = v[15] = 0xFF;
    ok(!whole(v, v.size()), "a negative node count");
    std::vector<uint8_t> w = image(0, 0, 0);
    // node count -> 0x7FFFFFFF: 2^31 x 80 would wrap a 32-bit offset back into range
    w[12] = 0xFF; w[13] = 0xFF; w[14] = 0xFF; w[15] = 0x7F;
    ok(!whole(w, w.size()), "a huge node count (would wrap a 32-bit offset)");
    /* A count that says fewer records than the file holds: the length no longer matches. */
    std::vector<uint8_t> x = image(3, 0, 0);
    x[12] = 2;
    ok(!whole(x, x.size()), "a count smaller than the records present");
  }

  group("the star list");
  {
    const uint32_t FM = 0x31564146u;   // "FAV1", as MESH_FAV_MAGIC
    std::vector<uint8_t> f;
    put32(f, FM);
    f.push_back(3);
    put32(f, 0x62b8d2fd);
    put32(f, 0x00449334);
    put32(f, 0x33646708);
    ok(meshFavImageComplete(f.data(), f.size(), FM, 32), "3 stars, 17 bytes: whole");
    bool anyShort = false;
    for (size_t n = 0; n < f.size(); n++) {
      if (meshFavImageComplete(f.data(), n, FM, 32)) anyShort = true;
    }
    ok(!anyShort, "no truncation of it passes");
    std::vector<uint8_t> none;
    put32(none, FM);
    none.push_back(0);
    ok(meshFavImageComplete(none.data(), none.size(), FM, 32), "no stars (5 bytes) is whole");
    f[4] = 40;
    ok(!meshFavImageComplete(f.data(), f.size(), FM, 32), "a count over the cap fails");
    f[4] = 3;
    f[0] ^= 1;
    ok(!meshFavImageComplete(f.data(), f.size(), FM, 32), "wrong magic fails");
  }

  group("a save pass writes one chunk; the finish gets a pass of its own");
  {
    /* Drive a save the way saveDbStep() does: one call per loop pass. */
    struct Run { int writes; int finishPass; uint32_t biggest; uint32_t total; };
    auto run = [](uint32_t len, bool onCard) {
      Run r = {0, -1, 0, 0};
      uint32_t off = 0;
      for (int pass = 0; pass < 100; pass++) {
        const uint32_t n = meshSaveChunk(len, off, onCard);
        if (!n) { r.finishPass = pass; break; }
        r.writes++;
        if (n > r.biggest) r.biggest = n;
        r.total += n;
        off += n;
      }
      return r;
    };
    const uint32_t card = 12 + 4 + 200 * 80 + 4 + 200 * 248 + 4 + 8 * 44;   // the largest card image
    Run r = run(card, true);
    ok(r.total == card && r.writes == 5 && r.biggest == MESH_SAVE_CHUNK_SD,
       "the largest card image (65,976 B): five 16 KB-or-less writes, every byte once");
    ok(r.finishPass == 5, "...and the close/remove/rename on a SIXTH pass, not riding on a write");
    const uint32_t flash = 12 + 4 + 200 * 80 + 4 + 60 * 248 + 4 + 8 * 44;   // SPIFFS keeps 60 messages
    r = run(flash, false);
    ok(r.writes == 1 && r.total == flash,
       "the largest SPIFFS image (31,252 B) goes in ONE write (an erase cannot be chunked around)");
    ok(r.finishPass == 1, "...and its finish on the next pass");
    r = run(MESH_SAVE_CHUNK_SD, true);
    ok(r.writes == 1 && r.finishPass == 1, "exactly one chunk: one write, then the finish");
    ok(meshSaveChunk(100, 100, true) == 0 && meshSaveChunk(100, 150, true) == 0,
       "nothing left (or past the end): 0, i.e. finish");
  }

  printf("\n%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
