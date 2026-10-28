/*
 * mesh_txq.h — what the LoRa radio sends next, and when. Pure, no Arduino, host-tested by
 * tests/test_txq.cpp.
 *
 * ── WHY THERE IS A QUEUE AT ALL ────────────────────────────────────────────────────────────
 * 🛑 THE 0.6-1.5 s 'mesh' LOOP STALLS WERE LORA TRANSMITS, NOT THE DATABASE SAVE. Until this
 * change MeshPhy::send() put the SX1276 into TX and then busy-waited on TxDone for the frame's
 * whole time on air, on the superloop task — so every relay, ACK, beacon and text froze the
 * keypad, the screen and the WiFi stack for 0.5-2.2 s. The stall values in /health.log sit on
 * the airtime lattice to within a few ms: 166 ms of preamble + (8 + 5k) x 8.192 ms, i.e. steps
 * of 40.96 ms (518, 559, 600, 641 ... 1501 ... 2157), plus the bit-banged FIFO write — +2..+4 ms
 * at 240 MHz, +9..+12 ms at 80 MHz. Phone 1's 2026-09-03 log: 278 of 346 STALL records on the
 * lattice, all at scr=0 (nobody touching it); ~50 more are the ~307 ms WiFi scan, not the mesh.
 * The flood relay alone fires ~20 times an hour (it relays everything with hops left, including
 * LongFast traffic it cannot decrypt), which is why it looked like "the phone stalls when idle".
 * The SD database save it was blamed on is ~50-130 ms since 0.9.19 (the `bench` numbers in
 * meshtastic_service.cpp); the "~1.5 s" was the SPIFFS-era figure.
 *
 * Now the chip is started and LEFT to transmit (MeshPhy::startSend), the loop checks TxDone on
 * each pass (MeshPhy::serviceTx), and everything that wants the air waits its turn here:
 *
 *   OWN queue   frames this phone originated, FIFO — texts, beacons, NodeInfo, neighbour and
 *               replay packets — with our routing ACKs put AHEAD of them (the sender is
 *               waiting on the ACK and retries without it) but never ahead of each other and
 *               never ahead of a frame already on the air.
 *   RELAY slots somebody else's packet, flood-relayed after a random 130-700 ms. Only when the
 *               own queue is empty, earliest-due first, and CANCELLED if the same packet is
 *               heard relayed by somebody else first (stock FloodingRouter does exactly this
 *               for a non-router role).
 *
 * 🔑 ONE START PER PASS. meshTxPick() never picks while the PHY is busy, and the pump calls it
 * once a pass — so "two transmits in one pass" (the 1,985 ms maximum: 1009.7 + 968.7 ms + two
 * FIFO writes) cannot happen by construction, where before it was a rule only the position
 * beacon obeyed.
 *
 * ⚠ STORAGE IS THE CALLER'S. The service hands meshTxqInit() PSRAM (8 frames, ~2 KB); a phone
 * whose PSRAM allocation failed gets a 2-frame internal fallback instead of no radio. Nothing
 * here allocates.
 */

#ifndef MESH_TXQ_H
#define MESH_TXQ_H

#include <stdint.h>
#include <stddef.h>

#define MESH_TXQ_FRAME_MAX     256   // one LoRa frame is at most 255 B (the PHY takes a uint8_t)
#define MESH_TXQ_CAP             8   // own frames waiting (PSRAM)
#define MESH_TXQ_CAP_FALLBACK    2   // ...when PSRAM could not be had (internal)
#define MESH_RELAY_SLOTS         4   // relays waiting out their jitter (was rebroadcast[4])
#define MESH_TXQ_HDR_LEN        16   // the on-air header; mirrors MESH_HEADER_LEN (mesh_packet.h)

enum MeshTxKind : uint8_t {
  MESH_TXK_OWN   = 0,   // this phone originated it: a timeout fails its receipt
  MESH_TXK_ACK   = 1,   // our routing ACK for a DM somebody sent us: jumps the own queue
  MESH_TXK_RELAY = 2,   // somebody else's packet: lives in a relay slot, never in the queue
};

struct MeshTxFrame {
  uint32_t packetId;                  // for the receipt (resolveAck) if TxDone never comes
  uint8_t  len;                       // 1..255
  uint8_t  kind;                      // MeshTxKind
  uint8_t  data[MESH_TXQ_FRAME_MAX];
};

struct MeshTxQueue {
  MeshTxFrame* slots;                 // caller's storage; NULL = no queue (every push refuses)
  uint8_t      cap;
  uint8_t      head;                  // ring index of the front
  uint8_t      count;
  uint8_t      maxDepth;              // deepest it has been since init — the `radio` line
};

void meshTxqInit(MeshTxQueue* q, MeshTxFrame* storage, int cap);

/* Append an own frame. False (and nothing changes) when the queue is full, has no storage, or
 * the frame is empty or longer than one LoRa frame. */
bool meshTxqPushBack(MeshTxQueue* q, const uint8_t* data, size_t len, uint8_t kind,
                     uint32_t packetId);

/* An ACK: ahead of every frame that is NOT an ACK, behind the ACKs already waiting — so ACKs
 * leave in the order the DMs they answer arrived. "The front" is the front of the QUEUE: the
 * frame on the air has already left it (the pump pops a frame when it starts it), so nothing
 * pushed here can pre-empt a transmit in progress. Same refusals as meshTxqPushBack. */
bool meshTxqPushFront(MeshTxQueue* q, const uint8_t* data, size_t len, uint8_t kind,
                      uint32_t packetId);

const MeshTxFrame* meshTxqFront(const MeshTxQueue* q);            // NULL when empty
const MeshTxFrame* meshTxqAt(const MeshTxQueue* q, int i);        // 0 = front; NULL past end
void meshTxqPop(MeshTxQueue* q);                                  // no-op when empty
int  meshTxqCount(const MeshTxQueue* q);
void meshTxqClear(MeshTxQueue* q);

/* ── Relay slots ─────────────────────────────────────────────────────────────────────────── */

struct MeshRelaySlot {
  uint8_t  data[MESH_TXQ_FRAME_MAX];  // the whole frame, hop limit already decremented
  uint8_t  len;
  bool     active;
  uint32_t dueMs;                     // millis() it may go; compared SIGNED (wrap-safe)
};

/* Park a relay in the first free slot. Returns the slot, or -1 when every slot is taken (the
 * packet is simply not relayed — the old rule, unchanged) or the frame is shorter than the
 * header meshRelayCancel() reads. */
int  meshRelaySchedule(MeshRelaySlot* slots, int n, const uint8_t* pkt, size_t len,
                       uint32_t dueMs);

/* Somebody else relayed this packet before we did: drop ours. Matches the frame's own header
 * — sender at bytes 4-7, packet id at 8-11, little-endian on the air — so a packet id reused
 * by a different sender is never cancelled. Returns how many slots were cancelled. */
int  meshRelayCancel(MeshRelaySlot* slots, int n, uint32_t sender, uint32_t packetId);

void meshRelayClear(MeshRelaySlot* slots, int n);

/* The on-air header's sender / packet id, read byte by byte (endian-proof). */
uint32_t meshFrameSender(const uint8_t* frame);
uint32_t meshFramePacketId(const uint8_t* frame);

/* ── What goes next ──────────────────────────────────────────────────────────────────────── */

enum MeshTxPickWhat : uint8_t {
  MESH_PICK_NONE  = 0,   // nothing: the PHY is busy, or nothing is waiting or due
  MESH_PICK_QUEUE = 1,   // the own queue's front
  MESH_PICK_RELAY = 2,   // relay slot `relay`
};

struct MeshTxPick {
  uint8_t what;          // MeshTxPickWhat
  int     relay;         // the slot, for MESH_PICK_RELAY; -1 otherwise
};

/* Never while the PHY is busy; the own queue before any relay; a relay only once it is DUE
 * (signed millis() compare), earliest-due first; an inactive (sent or cancelled) slot never. */
MeshTxPick meshTxPick(bool phyBusy, int queueCount, const MeshRelaySlot* relays, int n,
                      uint32_t now);

/* May a BACKGROUND originator (replay drip, neighbour drip, periodic/answering NodeInfo, the
 * position beacon) queue a frame now? Only into an idle pipeline: nothing on the air and
 * nothing waiting. ⚠ A caller told "no" must NOT advance its deadline or clear its owed flag —
 * it asks again next pass. That is what keeps background traffic from filling the queue a
 * person's text needs, without losing a beacon. */
inline bool meshTxPipelineIdle(bool phyBusy, int queueCount) {
  return !phyBusy && queueCount == 0;
}

/* ── WHAT BECAME OF OUR OWN FRAMES ─────────────────────────────────────────────────────────
 * 🛑 QUEUED IS NOT ON THE AIR. Since the queue, a send returning true means the frame is
 * WAITING for the radio. It leaves within a pass or two — or it does not: TxDone never comes
 * (the woods pack died, a sagging pack browns the SX1276 out on TX), or MESH RADIO LOST drops
 * the queue. A chat message learns that through its receipt (resolveAck). Nothing else did:
 * the map's "Take it off the mesh" forgot the waypoint id on QUEUED, so a retraction that then
 * failed could never be sent again; a pin share drew yellow, Books said "Sent", the pin row
 * said "set" — all for a frame nobody heard (review M1, 2026-09-25; on main each got false
 * from the blocking send's TX timeout and said so).
 *
 * So the pump notes every OWN frame here by packet id — QUEUED when it enters the queue, SENT
 * on TxDone, FAILED on a TX timeout, a bench cut or a dropped queue — and a caller that must
 * not claim "sent" before it is true asks. ACKs and relays are nobody's claim and are not
 * noted. Small and forgetful on purpose: 16 frames (the queue holds 8 plus one on the air). An
 * id older than that reads UNKNOWN, which a caller must treat as "no word" — its safe default
 * — and NEVER as SENT. Storage is the caller's (a file static in the service, 84 bytes). */
enum MeshTxOutcome : uint8_t {
  MESH_TXO_UNKNOWN = 0,   // never noted here, or forgotten (16 newer own frames since)
  MESH_TXO_QUEUED  = 1,   // waiting for the radio, or on the air now
  MESH_TXO_SENT    = 2,   // TxDone: it left whole
  MESH_TXO_FAILED  = 3,   // it did not leave: timed out, cut short, or dropped with the radio
};

#define MESH_TXO_SLOTS  16

struct MeshTxOutcomeRing {
  uint32_t id[MESH_TXO_SLOTS];        // 0 = empty
  uint8_t  state[MESH_TXO_SLOTS];     // MeshTxOutcome
  uint8_t  next;                      // the slot the next new id overwrites (the oldest)
};

void meshTxoInit(MeshTxOutcomeRing* r);

/* Record `outcome` for `packetId`: updates its entry, or takes the oldest slot for a new one.
 * Packet id 0 and MESH_TXO_UNKNOWN are ignored. ⚠ A final outcome (SENT/FAILED) is never
 * turned back into QUEUED: a late QUEUED note for the same id is a bug somewhere else, and
 * "queued" would make a caller wait on a frame that has already finished. */
void    meshTxoNote(MeshTxOutcomeRing* r, uint32_t packetId, uint8_t outcome);
uint8_t meshTxoGet(const MeshTxOutcomeRing* r, uint32_t packetId);   // MeshTxOutcome

#endif // MESH_TXQ_H
