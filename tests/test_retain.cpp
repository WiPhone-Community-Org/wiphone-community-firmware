/*
 * test_retain.cpp — mesh_retain.cpp: which messages survive a reboot.
 *
 * The rule is "the newest N of each conversation, under an overall ceiling".
 * Every check below is about a way that can go wrong quietly: keeping the
 * oldest instead of the newest, letting one busy channel eat a quiet one's
 * allowance, or treating a DM with node 3 as the same thread as channel 3.
 */

#include "../WiPhone/mesh_retain.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

static int failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

// Build a key array from a compact spec: 'c' = channel, 'd' = DM, digit = id.
static std::vector<MeshRetainKey> keysOf(const char* spec) {
  std::vector<MeshRetainKey> v;
  for (const char* p = spec; p[0] && p[1]; p += 2) {
    MeshRetainKey k;
    k.isChannel = (p[0] == 'c');
    k.id = (uint32_t)(p[1] - '0');
    v.push_back(k);
  }
  return v;
}

// Render the keep-mask as a string of 0/1 so a failure prints the actual shape.
static std::string mask(const std::vector<uint8_t>& k) {
  std::string s;
  for (size_t i = 0; i < k.size(); i++) s += (k[i] ? '1' : '0');
  return s;
}

int main() {
  printf("test_retain\n");

  // ---- Nothing to do -------------------------------------------------------
  {
    uint8_t keep[1] = { 0xAA };
    MeshRetainKey k = { true, 0 };
    CHECK(meshRetainSelect(&k, 0, 10, 10, 8, keep) == 0, "count 0 keeps nothing");
    CHECK(meshRetainSelect(NULL, 4, 10, 10, 8, keep) == 0, "NULL keys keeps nothing");
    CHECK(meshRetainSelect(&k, 1, 10, 10, 8, NULL) == 0, "NULL out keeps nothing");
  }

  // ---- Under every cap: everything is kept ---------------------------------
  {
    std::vector<MeshRetainKey> k = keysOf("c1c1c1");
    std::vector<uint8_t> keep(k.size());
    int n = meshRetainSelect(k.data(), (int)k.size(), 40, 200, 32, keep.data());
    CHECK(n == 3 && mask(keep) == "111", "under the caps, all three kept");
  }

  // ---- Per-chat cap keeps the NEWEST, not the oldest -----------------------
  {
    std::vector<MeshRetainKey> k = keysOf("c1c1c1c1c1");     // oldest first
    std::vector<uint8_t> keep(k.size());
    int n = meshRetainSelect(k.data(), (int)k.size(), 2, 200, 32, keep.data());
    CHECK(n == 2 && mask(keep) == "00011", "per-chat cap 2 keeps the two NEWEST");
  }

  // ---- Each conversation gets its own allowance ----------------------------
  {
    // Channel 1 is busy, channel 2 said one thing a long time ago (it is oldest).
    std::vector<MeshRetainKey> k = keysOf("c2c1c1c1c1c1");
    std::vector<uint8_t> keep(k.size());
    int n = meshRetainSelect(k.data(), (int)k.size(), 2, 200, 32, keep.data());
    CHECK(n == 3 && mask(keep) == "100011",
          "a quiet channel keeps its history while a busy one is trimmed");
  }

  // ---- A channel and a DM with the same number are DIFFERENT threads -------
  {
    std::vector<MeshRetainKey> k = keysOf("c3d3c3d3");
    std::vector<uint8_t> keep(k.size());
    int n = meshRetainSelect(k.data(), (int)k.size(), 1, 200, 32, keep.data());
    CHECK(n == 2 && mask(keep) == "0011",
          "channel 3 and DM 3 do not share an allowance");
  }

  // ---- The overall ceiling drops the OLDEST across all conversations -------
  {
    std::vector<MeshRetainKey> k = keysOf("c1c2c1c2c1c2");
    std::vector<uint8_t> keep(k.size());
    int n = meshRetainSelect(k.data(), (int)k.size(), 40, 3, 32, keep.data());
    CHECK(n == 3 && mask(keep) == "000111", "total cap 3 keeps the three newest overall");
  }

  // ---- More conversations than there is budget for -------------------------
  {
    // Four chats, room for two. The two MOST RECENTLY ACTIVE win; the older
    // conversations get nothing rather than everyone getting a sliver.
    std::vector<MeshRetainKey> k = keysOf("c1c2c3c4");
    std::vector<uint8_t> keep(k.size());
    int n = meshRetainSelect(k.data(), (int)k.size(), 40, 200, 2, keep.data());
    CHECK(n == 2 && mask(keep) == "0011", "maxChats 2 protects the two newest threads");
  }

  // ---- maxChats is clamped, never trusted ----------------------------------
  {
    std::vector<MeshRetainKey> k = keysOf("c1c2");
    std::vector<uint8_t> keep(k.size());
    CHECK(meshRetainSelect(k.data(), 2, 40, 200, 9999, keep.data()) == 2,
          "an out-of-range maxChats clamps instead of overrunning");
    CHECK(meshRetainSelect(k.data(), 2, 40, 200, 0, keep.data()) == 2,
          "maxChats 0 means the default, not 'keep nothing'");
  }

  // ---- 0 and negative caps mean "no limit" ---------------------------------
  {
    std::vector<MeshRetainKey> k = keysOf("c1c1c1c1");
    std::vector<uint8_t> keep(k.size());
    CHECK(meshRetainSelect(k.data(), 4, 0, 0, 32, keep.data()) == 4,
          "caps of 0 keep everything");
  }

  // ---- The real shipping numbers, on a realistic ring ----------------------
  {
    // Five channels, 150 messages each — the RAM cap — against the SD budget.
    std::vector<MeshRetainKey> k;
    for (int round = 0; round < 150; round++) {
      for (int ch = 1; ch <= 5; ch++) {
        MeshRetainKey e = { true, (uint32_t)ch };
        k.push_back(e);
      }
    }
    std::vector<uint8_t> keep(k.size());
    int n = meshRetainSelect(k.data(), (int)k.size(), 40, 200, 32, keep.data());
    CHECK(n == 200, "SD budget: a full ring persists exactly 200 messages");
    // 200 total over 5 chats at 40 each: every chat gets its full 40.
    int per[6] = { 0, 0, 0, 0, 0, 0 };
    for (size_t i = 0; i < k.size(); i++) if (keep[i]) per[k[i].id]++;
    CHECK(per[1] == 40 && per[2] == 40 && per[3] == 40 && per[4] == 40 && per[5] == 40,
          "and it is 40 from each of the five, not 200 from the loudest");
    // 200 * 248 bytes is the file this produces — the number the cap was chosen for.
    CHECK(n * 248 <= 64 * 1024, "the persisted image stays under 64 KB");
  }

  printf(failures ? "%d FAILURE(S)\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
