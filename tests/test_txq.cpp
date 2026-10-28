/*
 * test_txq.cpp — mesh_txq.cpp: what the LoRa radio sends next (WiPhone/mesh_txq.h).
 *
 * WHY THIS IS WORTH A SUITE: the queue replaced a transmit that held the whole superloop for
 * the frame's time on air (0.5-2.2 s, ~20 times an hour — the 'mesh' STALL lines). Every rule
 * below is one whose failure is SILENT on the phone: an ACK stuck behind a beacon (the sender
 * retries and the DM shows twice), a relay sent after somebody else already relayed it (airtime
 * for nothing), a relay picked before its jitter (collides with the other relays), two frames
 * started in one pass (the 1,985 ms stall again), a queue that wraps wrong (a text sent twice
 * or never).
 *
 * The pump at the bottom drives the same pick/start/pop sequence MeshtasticService::txPump()
 * runs against a fake PHY that takes N passes per frame, so "one start per pass, never while
 * busy" is checked over a whole run rather than asserted of one call.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

#include "../WiPhone/mesh_txq.h"

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

/* A frame whose first byte names it, so order can be read back. */
static void frame(uint8_t* buf, uint8_t tag, size_t len) {
  memset(buf, 0, len);
  buf[0] = tag;
}

/* A 16-byte header + payload, sender and packet id little-endian at 4..11, as on the air. */
static size_t relayFrame(uint8_t* buf, uint32_t sender, uint32_t id, size_t len) {
  memset(buf, 0xA5, len);
  for (int i = 0; i < 4; i++) {
    buf[4 + i] = (uint8_t)(sender >> (8 * i));
    buf[8 + i] = (uint8_t)(id >> (8 * i));
  }
  return len;
}

static std::vector<uint8_t> order(const MeshTxQueue* q) {
  std::vector<uint8_t> v;
  for (int i = 0; i < meshTxqCount(q); i++) {
    v.push_back(meshTxqAt(q, i)->data[0]);
  }
  return v;
}

static bool orderIs(const MeshTxQueue* q, const char* want) {
  std::vector<uint8_t> v = order(q);
  if (v.size() != strlen(want)) {
    return false;
  }
  for (size_t i = 0; i < v.size(); i++) {
    if (v[i] != (uint8_t)want[i]) {
      return false;
    }
  }
  return true;
}

int main() {
  printf("test_txq\n");
  MeshTxFrame store[MESH_TXQ_CAP];
  MeshTxQueue q;
  uint8_t f[MESH_TXQ_FRAME_MAX];

  group("the own queue: FIFO, and the refusals");
  {
    meshTxqInit(&q, store, MESH_TXQ_CAP);
    ok(meshTxqCount(&q) == 0 && meshTxqFront(&q) == NULL, "starts empty, front NULL");
    frame(f, 'A', 40);
    ok(meshTxqPushBack(&q, f, 40, MESH_TXK_OWN, 11), "push A");
    frame(f, 'B', 41);
    ok(meshTxqPushBack(&q, f, 41, MESH_TXK_OWN, 12), "push B");
    frame(f, 'C', 42);
    ok(meshTxqPushBack(&q, f, 42, MESH_TXK_OWN, 13), "push C");
    ok(orderIs(&q, "ABC"), "FIFO: A B C in the order pushed");
    const MeshTxFrame* fr = meshTxqFront(&q);
    ok(fr && fr->len == 40 && fr->packetId == 11 && fr->kind == MESH_TXK_OWN,
       "the front carries its own length, id and kind");
    meshTxqPop(&q);
    ok(orderIs(&q, "BC") && meshTxqFront(&q)->packetId == 12, "pop takes A, B is next");
    meshTxqClear(&q);
    ok(meshTxqCount(&q) == 0, "clear empties it");
    meshTxqPop(&q);
    ok(meshTxqCount(&q) == 0, "pop on empty is a no-op");

    frame(f, 'x', 10);
    ok(!meshTxqPushBack(&q, f, 0, MESH_TXK_OWN, 1), "an empty frame is refused");
    ok(!meshTxqPushBack(&q, NULL, 10, MESH_TXK_OWN, 1), "a NULL frame is refused");
    ok(!meshTxqPushBack(&q, f, 256, MESH_TXK_OWN, 1),
       "256 B is refused (the PHY length is a uint8_t: it would wrap to 0)");
    uint8_t big[MESH_TXQ_FRAME_MAX];
    frame(big, 'M', 255);
    ok(meshTxqPushBack(&q, big, 255, MESH_TXK_OWN, 1) && meshTxqFront(&q)->len == 255,
       "255 B, the largest LoRa frame, is taken whole");
    meshTxqClear(&q);

    MeshTxQueue none;
    meshTxqInit(&none, NULL, 8);
    ok(!meshTxqPushBack(&none, f, 10, MESH_TXK_OWN, 1) && meshTxqCount(&none) == 0,
       "no storage (both allocations failed): every push refuses, nothing crashes");
    ok(meshTxqFront(&none) == NULL, "...and front is NULL");
  }

  group("a full queue refuses and changes nothing");
  {
    meshTxqInit(&q, store, MESH_TXQ_CAP);
    char want[MESH_TXQ_CAP + 1] = {0};
    for (int i = 0; i < MESH_TXQ_CAP; i++) {
      frame(f, (uint8_t)('a' + i), 20);
      meshTxqPushBack(&q, f, 20, MESH_TXK_OWN, (uint32_t)(100 + i));
      want[i] = (char)('a' + i);
    }
    ok(meshTxqCount(&q) == MESH_TXQ_CAP, "eight frames fill it");
    frame(f, 'Z', 20);
    ok(!meshTxqPushBack(&q, f, 20, MESH_TXK_OWN, 999), "a ninth own frame is refused");
    ok(!meshTxqPushFront(&q, f, 20, MESH_TXK_ACK, 999), "an ACK into a full queue is refused too");
    ok(orderIs(&q, want), "the refused frames changed nothing");
    ok(q.maxDepth == MESH_TXQ_CAP, "maxDepth records the high-water mark");
  }

  group("the ring wraps without losing or repeating a frame");
  {
    meshTxqInit(&q, store, 3);
    uint8_t next = 0, expect = 0;
    bool fine = true;
    for (int round = 0; round < 50; round++) {
      while (meshTxqCount(&q) < 3 && (round % 3) != 2) {     // uneven fill/drain pattern
        frame(f, next, 5);
        if (!meshTxqPushBack(&q, f, 5, MESH_TXK_OWN, next)) { fine = false; }
        next++;
        if ((next % 2) == 0) break;
      }
      if (meshTxqCount(&q)) {
        const MeshTxFrame* fr = meshTxqFront(&q);
        if (fr->data[0] != expect || fr->packetId != expect) { fine = false; }
        expect++;
        meshTxqPop(&q);
      }
    }
    while (meshTxqCount(&q)) {
      if (meshTxqFront(&q)->data[0] != expect) { fine = false; }
      expect++;
      meshTxqPop(&q);
    }
    ok(fine && expect == next, "every frame out exactly once, in order, across 50 wraps of a 3-ring");
  }

  group("ACKs: ahead of the own queue, in their own order, never ahead of the air");
  {
    meshTxqInit(&q, store, MESH_TXQ_CAP);
    frame(f, 'A', 30); meshTxqPushBack(&q, f, 30, MESH_TXK_OWN, 1);
    frame(f, 'B', 30); meshTxqPushBack(&q, f, 30, MESH_TXK_OWN, 2);
    /* The pump starts A: it leaves the queue and goes on the air. */
    const MeshTxFrame* onAir = meshTxqFront(&q);
    ok(onAir->data[0] == 'A', "A is the one started");
    meshTxqPop(&q);
    frame(f, '1', 20);
    ok(meshTxqPushFront(&q, f, 20, MESH_TXK_ACK, 91), "ACK 1 while A is on the air");
    ok(orderIs(&q, "1B"), "ACK 1 goes ahead of B - and A, already on the air, is not in the queue to pre-empt");
    ok(!meshTxPick(true, meshTxqCount(&q), NULL, 0, 0).what, "...and nothing is picked while A is still on the air");
    frame(f, '2', 20);
    meshTxqPushFront(&q, f, 20, MESH_TXK_ACK, 92);
    ok(orderIs(&q, "12B"), "ACK 2 lands BEHIND ACK 1: ACKs leave in the order their DMs came");
    frame(f, 'C', 30);
    meshTxqPushBack(&q, f, 30, MESH_TXK_OWN, 3);
    frame(f, '3', 20);
    meshTxqPushFront(&q, f, 20, MESH_TXK_ACK, 93);
    ok(orderIs(&q, "123BC"), "ACK 3 behind 1 and 2, ahead of B and C");
    ok(meshTxqAt(&q, 2)->kind == MESH_TXK_ACK && meshTxqAt(&q, 3)->kind == MESH_TXK_OWN,
       "kinds travel with their frames");

    /* Across a wrap: the shift must follow the ring, not the array. */
    meshTxqInit(&q, store, 4);
    frame(f, 'p', 5); meshTxqPushBack(&q, f, 5, MESH_TXK_OWN, 0);
    frame(f, 'q', 5); meshTxqPushBack(&q, f, 5, MESH_TXK_OWN, 0);
    frame(f, 'r', 5); meshTxqPushBack(&q, f, 5, MESH_TXK_OWN, 0);
    meshTxqPop(&q); meshTxqPop(&q);                          // head now at 2: q r p... wraps
    frame(f, 's', 5); meshTxqPushBack(&q, f, 5, MESH_TXK_OWN, 0);
    frame(f, 't', 5); meshTxqPushBack(&q, f, 5, MESH_TXK_OWN, 0);
    frame(f, 'K', 5);
    ok(meshTxqPushFront(&q, f, 5, MESH_TXK_ACK, 0) && orderIs(&q, "Krst"),
       "an ACK pushed into a WRAPPED ring still lands at the front, nothing lost");
  }

  group("relay slots: schedule, cancel on a duplicate, clear");
  {
    MeshRelaySlot r[MESH_RELAY_SLOTS];
    memset(r, 0, sizeof(r));
    uint8_t p[64];
    relayFrame(p, 0x33646708u, 0xDEADBEEFu, 40);
    ok(meshFrameSender(p) == 0x33646708u && meshFramePacketId(p) == 0xDEADBEEFu,
       "the header is read little-endian, byte by byte, as it is on the air");
    ok(meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 40, 1000) == 0, "first relay takes slot 0");
    ok(r[0].active && r[0].len == 40 && r[0].dueMs == 1000 && memcmp(r[0].data, p, 40) == 0,
       "the whole frame, its length and due time are kept");
    ok(meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 15, 1000) == -1,
       "a frame shorter than the 16-byte header is refused (cancel reads bytes 4-11)");
    relayFrame(p, 0x11111111u, 0xDEADBEEFu, 40);
    ok(meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 40, 1100) == 1, "same packet id, OTHER sender: slot 1");
    relayFrame(p, 0x33646708u, 0x00000001u, 40);
    ok(meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 40, 1200) == 2, "same sender, other id: slot 2");
    relayFrame(p, 0x22222222u, 0x2u, 40);
    ok(meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 40, 1300) == 3, "slot 3");
    ok(meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 40, 1400) == -1,
       "all four taken: the fifth is not relayed (the old rule)");

    ok(meshRelayCancel(r, MESH_RELAY_SLOTS, 0x33646708u, 0xDEADBEEFu) == 1,
       "a duplicate of (!33646708, 0xDEADBEEF) cancels exactly one relay");
    ok(!r[0].active && r[1].active && r[2].active,
       "...the matching one: not the same id from another sender, not the same sender's other packet");
    ok(meshRelayCancel(r, MESH_RELAY_SLOTS, 0x33646708u, 0xDEADBEEFu) == 0,
       "cancelling again finds nothing");
    ok(meshRelayCancel(r, MESH_RELAY_SLOTS, 0x99999999u, 0x12345678u) == 0,
       "a packet we never scheduled cancels nothing");
    meshRelayClear(r, MESH_RELAY_SLOTS);
    ok(!r[0].active && !r[1].active && !r[2].active && !r[3].active, "clear frees every slot");
  }

  group("the pick: never while busy, own before relays, relays only when due");
  {
    MeshRelaySlot r[MESH_RELAY_SLOTS];
    memset(r, 0, sizeof(r));
    uint8_t p[64];
    relayFrame(p, 1, 1, 30);
    meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 30, 5000);
    relayFrame(p, 2, 2, 30);
    meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 30, 4000);

    MeshTxPick k = meshTxPick(true, 3, r, MESH_RELAY_SLOTS, 9000);
    ok(k.what == MESH_PICK_NONE && k.relay == -1, "PHY busy: nothing, however much is waiting");
    k = meshTxPick(false, 1, r, MESH_RELAY_SLOTS, 9000);
    ok(k.what == MESH_PICK_QUEUE, "own frame waiting + due relays: the own frame goes first");
    k = meshTxPick(false, 0, r, MESH_RELAY_SLOTS, 3999);
    ok(k.what == MESH_PICK_NONE, "no relay is due at 3999 (earliest is 4000): nothing");
    k = meshTxPick(false, 0, r, MESH_RELAY_SLOTS, 4000);
    ok(k.what == MESH_PICK_RELAY && k.relay == 1, "at 4000 the slot due at 4000 goes");
    k = meshTxPick(false, 0, r, MESH_RELAY_SLOTS, 6000);
    ok(k.what == MESH_PICK_RELAY && k.relay == 1,
       "both due: the EARLIEST-due goes first, not the lowest slot");
    r[1].active = false;                                      // cancelled (or sent)
    k = meshTxPick(false, 0, r, MESH_RELAY_SLOTS, 6000);
    ok(k.what == MESH_PICK_RELAY && k.relay == 0, "a cancelled relay is never picked; the other one is");
    r[0].active = false;
    ok(meshTxPick(false, 0, r, MESH_RELAY_SLOTS, 6000).what == MESH_PICK_NONE,
       "all cancelled: nothing");
    ok(meshTxPick(false, 0, NULL, 0, 6000).what == MESH_PICK_NONE, "no relay table: nothing");

    /* millis() wraps every 49.7 days. A relay scheduled 200 ms before the wrap is due 300 ms
     * after it; a plain unsigned compare would call it due at once (or never). */
    memset(r, 0, sizeof(r));
    const uint32_t due = 0x00000064u;                        // 100 ms past the wrap
    relayFrame(p, 3, 3, 30);
    meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 30, due);
    ok(meshTxPick(false, 0, r, MESH_RELAY_SLOTS, 0xFFFFFF38u).what == MESH_PICK_NONE,
       "wrap: 200 ms BEFORE the wrap, a relay due 100 ms after it is not due");
    ok(meshTxPick(false, 0, r, MESH_RELAY_SLOTS, 0x00000063u).what == MESH_PICK_NONE,
       "wrap: 1 ms before its due time, still not due");
    ok(meshTxPick(false, 0, r, MESH_RELAY_SLOTS, 0x00000064u).what == MESH_PICK_RELAY,
       "wrap: due exactly on time");
    relayFrame(p, 4, 4, 30);
    meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 30, 0xFFFFFFF0u);   // due BEFORE the wrap
    k = meshTxPick(false, 0, r, MESH_RELAY_SLOTS, 0x00000070u);
    ok(k.what == MESH_PICK_RELAY && k.relay == 1,
       "wrap: of two due relays, the one due before the wrap is the earlier (signed compare)");
  }

  group("background originators wait for an idle pipeline");
  {
    ok(meshTxPipelineIdle(false, 0), "nothing on the air, nothing waiting: idle");
    ok(!meshTxPipelineIdle(true, 0), "a frame on the air: not idle");
    ok(!meshTxPipelineIdle(false, 1), "a frame waiting: not idle");
  }

  /* Review M1 (2026-09-25): QUEUED is not on the air. The map, Books and the pin row now wait
   * on this ring before they say "sent", so the ways it can lie are the ways they would. */
  group("what became of our own frames: queued -> sent / failed, and forgetting safely");
  {
    MeshTxOutcomeRing r;
    meshTxoInit(&r);
    ok(meshTxoGet(&r, 0x1234) == MESH_TXO_UNKNOWN, "an id never noted reads UNKNOWN");
    meshTxoNote(&r, 0x1234, MESH_TXO_QUEUED);
    ok(meshTxoGet(&r, 0x1234) == MESH_TXO_QUEUED, "queued reads QUEUED - not sent");
    meshTxoNote(&r, 0x1234, MESH_TXO_SENT);
    ok(meshTxoGet(&r, 0x1234) == MESH_TXO_SENT, "TxDone turns it SENT");
    meshTxoNote(&r, 0x1234, MESH_TXO_QUEUED);
    ok(meshTxoGet(&r, 0x1234) == MESH_TXO_SENT, "a late QUEUED never un-finishes a frame");
    meshTxoNote(&r, 0x5678, MESH_TXO_QUEUED);
    meshTxoNote(&r, 0x5678, MESH_TXO_FAILED);
    ok(meshTxoGet(&r, 0x5678) == MESH_TXO_FAILED, "a timeout or a dropped queue reads FAILED");
    ok(meshTxoGet(&r, 0x1234) == MESH_TXO_SENT, "...and does not touch another frame's outcome");
    meshTxoNote(&r, 0, MESH_TXO_SENT);
    ok(meshTxoGet(&r, 0) == MESH_TXO_UNKNOWN, "packet id 0 is nobody: never recorded");
    meshTxoNote(&r, 0x9999, MESH_TXO_UNKNOWN);
    ok(meshTxoGet(&r, 0x9999) == MESH_TXO_UNKNOWN, "an UNKNOWN note records nothing");
    meshTxoNote(&r, 0xAAAA, MESH_TXO_SENT);
    ok(meshTxoGet(&r, 0xAAAA) == MESH_TXO_SENT,
       "an outcome for an id already forgotten is still recorded (a late TxDone)");

    /* A full queue (8) + one on the air + a burst of beacons: the oldest is forgotten, and
     * forgotten reads UNKNOWN - the caller's "no word", never a made-up SENT. */
    MeshTxOutcomeRing w;
    meshTxoInit(&w);
    meshTxoNote(&w, 1, MESH_TXO_QUEUED);
    for (uint32_t id = 2; id <= MESH_TXO_SLOTS; id++) {
      meshTxoNote(&w, id, MESH_TXO_QUEUED);
    }
    ok(meshTxoGet(&w, 1) == MESH_TXO_QUEUED, "16 frames in flight: the first is still known");
    meshTxoNote(&w, MESH_TXO_SLOTS + 1, MESH_TXO_QUEUED);
    ok(meshTxoGet(&w, 1) == MESH_TXO_UNKNOWN, "the 17th forgets the oldest - UNKNOWN, not SENT");
    ok(meshTxoGet(&w, 2) == MESH_TXO_QUEUED && meshTxoGet(&w, MESH_TXO_SLOTS + 1) == MESH_TXO_QUEUED,
       "...and only the oldest");
    meshTxoNote(&w, 5, MESH_TXO_SENT);                  // an update in place takes no new slot
    meshTxoNote(&w, MESH_TXO_SLOTS + 2, MESH_TXO_QUEUED);
    ok(meshTxoGet(&w, 5) == MESH_TXO_SENT && meshTxoGet(&w, 2) == MESH_TXO_UNKNOWN,
       "updating an entry does not move the eviction order");
    ok(sizeof(MeshTxOutcomeRing) <= 96, "the ring is small enough for internal RAM (<= 96 B)");
  }

  group("the pump over a whole run: one start per pass, never two frames on the air");
  {
    /* The shape of MeshtasticService::txPump(): service the PHY, then (if it is idle) pick and
     * start ONE frame, popping it from the queue or clearing its relay slot. The fake PHY is on
     * the air for `air` passes per frame. */
    meshTxqInit(&q, store, MESH_TXQ_CAP);
    MeshRelaySlot r[MESH_RELAY_SLOTS];
    memset(r, 0, sizeof(r));
    uint8_t p[64];
    for (int i = 0; i < 3; i++) {
      frame(f, (uint8_t)('A' + i), 30);
      meshTxqPushBack(&q, f, 30, MESH_TXK_OWN, (uint32_t)i);
    }
    relayFrame(p, 7, 70, 30);
    meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 30, 20);        // due at pass 20
    relayFrame(p, 8, 80, 30);
    meshRelaySchedule(r, MESH_RELAY_SLOTS, p, 30, 25);        // cancelled at pass 10

    int busyLeft = 0, startsThisPass = 0, maxStarts = 0, overlaps = 0;
    std::vector<char> sent;
    for (uint32_t pass = 0; pass < 200; pass++) {
      startsThisPass = 0;
      if (busyLeft > 0) {
        busyLeft--;                                           // serviceTx: still on the air
      }
      if (pass == 3) {                                        // a DM arrives mid-transmit
        frame(f, 'K', 20);
        meshTxqPushFront(&q, f, 20, MESH_TXK_ACK, 99);
      }
      if (pass == 10) {                                       // somebody else relayed !8's packet
        meshRelayCancel(r, MESH_RELAY_SLOTS, 8, 80);
      }
      const MeshTxPick k = meshTxPick(busyLeft > 0, meshTxqCount(&q), r, MESH_RELAY_SLOTS, pass);
      if (k.what == MESH_PICK_QUEUE) {
        if (busyLeft) overlaps++;
        sent.push_back((char)meshTxqFront(&q)->data[0]);
        meshTxqPop(&q);
        busyLeft = 6;
        startsThisPass++;
      } else if (k.what == MESH_PICK_RELAY) {
        if (busyLeft) overlaps++;
        sent.push_back((char)('0' + r[k.relay].data[4]));
        r[k.relay].active = false;
        busyLeft = 6;
        startsThisPass++;
      }
      if (startsThisPass > maxStarts) maxStarts = startsThisPass;
    }
    const std::string got(sent.begin(), sent.end());
    printf("       sent: %s\n", got.c_str());
    ok(maxStarts == 1, "never more than ONE start in a pass (the 1,985 ms two-transmit stall)");
    ok(overlaps == 0, "never a start while a frame is on the air");
    ok(got == "AKBC7", "A (already on the air), then the ACK that arrived mid-transmit, then B, C, "
                       "then the due relay; the cancelled relay never goes");
  }

  printf("\n%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
