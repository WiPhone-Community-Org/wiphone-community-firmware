/*
 * mesh_retain.h — which Meshtastic messages survive a reboot.
 *
 * The ring in RAM holds far more than is worth writing to storage every time it
 * changes (MESH_MSG_CAP is 1000, and a save blocks the whole superloop for as
 * long as the write takes). This picks the subset that goes into the file: the
 * newest N of each conversation, under an overall ceiling.
 *
 * Pure arithmetic, no Arduino headers, so tests/test_retain.cpp exercises it on
 * the host. A retention rule is exactly the sort of thing that is easy to get
 * subtly wrong — keeping the oldest instead of the newest, letting one busy
 * channel starve a quiet one — and impossible to watch going wrong on a handset.
 */

#ifndef MESH_RETAIN_H
#define MESH_RETAIN_H

#include <stdint.h>
#include <stddef.h>

/* One conversation, keyed the way storeMessage() keys them: a channel by its
 * hash, a DM by the other node.
 * ⚠ NOT chatKeyOf(), which folds EVERY broadcast into 0. Using that here would
 * put LongFast, hunt-group, booksync and smsmirror in one bucket sharing a
 * single allowance, so a chatty channel would evict a quiet one's whole
 * history. storeMessage()'s own per-chat cap already separates them by
 * channelHash and this has to agree with it. */
typedef struct {
  bool     isChannel;
  uint32_t id;              // channel hash, or the other node's number
} MeshRetainKey;

/* The most conversations that can hold an allowance at once. Above the app's
 * own MESH_APP_MAX_CHATS (24) so the list a person can see always fits. */
#define MESH_RETAIN_MAX_CHATS  32

/*
 * Mark which messages to write.
 *
 *   keys      one per message, OLDEST FIRST — the order the ring is stored in
 *   count     how many
 *   perChat   newest messages kept per conversation (<= 0: no per-chat limit)
 *   total     overall ceiling (<= 0: no overall limit)
 *   maxChats  how many distinct conversations get an allowance; clamped to
 *             MESH_RETAIN_MAX_CHATS
 *   keep      OUT, one byte per message: 1 = write it, 0 = drop it
 *
 * Returns how many were marked.
 *
 * Walks NEWEST first, which is the whole point: when an allowance runs out it
 * is the OLDEST messages that go, and a conversation nobody has used in a week
 * still keeps its tail until the overall cap bites.
 */
int meshRetainSelect(const MeshRetainKey* keys, int count,
                     int perChat, int total, int maxChats, uint8_t* keep);

#endif // MESH_RETAIN_H
