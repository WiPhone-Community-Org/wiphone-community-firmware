/*
 * mesh_txq.cpp — see mesh_txq.h. Pure, no Arduino, host-tested by tests/test_txq.cpp.
 */

#include "mesh_txq.h"
#include <string.h>

void meshTxqInit(MeshTxQueue* q, MeshTxFrame* storage, int cap) {
  if (!q) {
    return;
  }
  q->slots = storage;
  q->cap = (storage && cap > 0) ? (uint8_t)(cap > 255 ? 255 : cap) : 0;
  q->head = 0;
  q->count = 0;
  q->maxDepth = 0;
}

static int ringIndex(const MeshTxQueue* q, int i) {
  return (q->head + i) % q->cap;
}

static bool pushOk(const MeshTxQueue* q, const uint8_t* data, size_t len) {
  return q && q->slots && q->cap && q->count < q->cap && data && len > 0 &&
         len < MESH_TXQ_FRAME_MAX;
}

static void fill(MeshTxFrame* f, const uint8_t* data, size_t len, uint8_t kind,
                 uint32_t packetId) {
  memcpy(f->data, data, len);
  f->len = (uint8_t)len;
  f->kind = kind;
  f->packetId = packetId;
}

static void noteDepth(MeshTxQueue* q) {
  if (q->count > q->maxDepth) {
    q->maxDepth = q->count;
  }
}

bool meshTxqPushBack(MeshTxQueue* q, const uint8_t* data, size_t len, uint8_t kind,
                     uint32_t packetId) {
  if (!pushOk(q, data, len)) {
    return false;
  }
  fill(&q->slots[ringIndex(q, q->count)], data, len, kind, packetId);
  q->count++;
  noteDepth(q);
  return true;
}

bool meshTxqPushFront(MeshTxQueue* q, const uint8_t* data, size_t len, uint8_t kind,
                      uint32_t packetId) {
  if (!pushOk(q, data, len)) {
    return false;
  }
  /* Behind the ACKs already at the front, ahead of everything else: ACKs keep their own
   * arrival order. Shift the tail one place back to open the slot (count < cap, checked). */
  int at = 0;
  while (at < q->count && q->slots[ringIndex(q, at)].kind == MESH_TXK_ACK) {
    at++;
  }
  for (int i = q->count; i > at; i--) {
    q->slots[ringIndex(q, i)] = q->slots[ringIndex(q, i - 1)];
  }
  fill(&q->slots[ringIndex(q, at)], data, len, kind, packetId);
  q->count++;
  noteDepth(q);
  return true;
}

const MeshTxFrame* meshTxqFront(const MeshTxQueue* q) {
  return meshTxqAt(q, 0);
}

const MeshTxFrame* meshTxqAt(const MeshTxQueue* q, int i) {
  if (!q || !q->slots || i < 0 || i >= q->count) {
    return NULL;
  }
  return &q->slots[ringIndex(q, i)];
}

void meshTxqPop(MeshTxQueue* q) {
  if (!q || !q->count) {
    return;
  }
  q->head = (uint8_t)((q->head + 1) % q->cap);
  q->count--;
}

int meshTxqCount(const MeshTxQueue* q) {
  return q ? q->count : 0;
}

void meshTxqClear(MeshTxQueue* q) {
  if (q) {
    q->head = 0;
    q->count = 0;
  }
}

// ---- Relay slots ------------------------------------------------------------------------

uint32_t meshFrameSender(const uint8_t* d) {
  return (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
}

uint32_t meshFramePacketId(const uint8_t* d) {
  return (uint32_t)d[8] | ((uint32_t)d[9] << 8) | ((uint32_t)d[10] << 16) |
         ((uint32_t)d[11] << 24);
}

int meshRelaySchedule(MeshRelaySlot* slots, int n, const uint8_t* pkt, size_t len,
                      uint32_t dueMs) {
  if (!slots || !pkt || len < MESH_TXQ_HDR_LEN || len >= MESH_TXQ_FRAME_MAX) {
    return -1;
  }
  for (int i = 0; i < n; i++) {
    if (!slots[i].active) {
      memcpy(slots[i].data, pkt, len);
      slots[i].len = (uint8_t)len;
      slots[i].dueMs = dueMs;
      slots[i].active = true;
      return i;
    }
  }
  return -1;
}

int meshRelayCancel(MeshRelaySlot* slots, int n, uint32_t sender, uint32_t packetId) {
  int cancelled = 0;
  if (!slots) {
    return 0;
  }
  for (int i = 0; i < n; i++) {
    if (slots[i].active && slots[i].len >= MESH_TXQ_HDR_LEN &&
        meshFrameSender(slots[i].data) == sender &&
        meshFramePacketId(slots[i].data) == packetId) {
      slots[i].active = false;
      cancelled++;
    }
  }
  return cancelled;
}

void meshRelayClear(MeshRelaySlot* slots, int n) {
  if (!slots) {
    return;
  }
  for (int i = 0; i < n; i++) {
    slots[i].active = false;
  }
}

// ---- What goes next ---------------------------------------------------------------------

MeshTxPick meshTxPick(bool phyBusy, int queueCount, const MeshRelaySlot* relays, int n,
                      uint32_t now) {
  MeshTxPick p;
  p.what = MESH_PICK_NONE;
  p.relay = -1;
  if (phyBusy) {
    return p;
  }
  if (queueCount > 0) {
    p.what = MESH_PICK_QUEUE;
    return p;
  }
  if (!relays) {
    return p;
  }
  for (int i = 0; i < n; i++) {
    if (!relays[i].active || (int32_t)(now - relays[i].dueMs) < 0) {
      continue;                              // sent, cancelled, or still in its jitter
    }
    if (p.relay < 0 || (int32_t)(relays[i].dueMs - relays[p.relay].dueMs) < 0) {
      p.relay = i;                           // earliest due first (signed: wrap-safe)
    }
  }
  if (p.relay >= 0) {
    p.what = MESH_PICK_RELAY;
  }
  return p;
}

// ---- What became of our own frames -------------------------------------------------------

void meshTxoInit(MeshTxOutcomeRing* r) {
  if (r) {
    memset(r, 0, sizeof(*r));
  }
}

void meshTxoNote(MeshTxOutcomeRing* r, uint32_t packetId, uint8_t outcome) {
  if (!r || !packetId || outcome == MESH_TXO_UNKNOWN || outcome > MESH_TXO_FAILED) {
    return;
  }
  for (int i = 0; i < MESH_TXO_SLOTS; i++) {
    if (r->id[i] == packetId) {
      if (outcome == MESH_TXO_QUEUED && r->state[i] != MESH_TXO_QUEUED) {
        return;                              // finished stays finished (see the header)
      }
      r->state[i] = outcome;
      return;
    }
  }
  const int at = r->next % MESH_TXO_SLOTS;
  r->id[at] = packetId;
  r->state[at] = outcome;
  r->next = (uint8_t)((at + 1) % MESH_TXO_SLOTS);
}

uint8_t meshTxoGet(const MeshTxOutcomeRing* r, uint32_t packetId) {
  if (!r || !packetId) {
    return MESH_TXO_UNKNOWN;
  }
  for (int i = 0; i < MESH_TXO_SLOTS; i++) {
    if (r->id[i] == packetId) {
      return r->state[i];
    }
  }
  return MESH_TXO_UNKNOWN;
}
