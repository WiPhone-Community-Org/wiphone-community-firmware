#include "mesh_retain.h"
#include <string.h>

int meshRetainSelect(const MeshRetainKey* keys, int count,
                     int perChat, int total, int maxChats, uint8_t* keep) {
  if (!keys || !keep || count <= 0) {
    return 0;
  }
  memset(keep, 0, (size_t)count);
  if (maxChats <= 0 || maxChats > MESH_RETAIN_MAX_CHATS) {
    maxChats = MESH_RETAIN_MAX_CHATS;
  }

  struct Chat {
    bool     isChannel;
    uint32_t id;
    int      kept;
  } chats[MESH_RETAIN_MAX_CHATS];
  int nchats = 0, kept = 0;

  /* Newest first. The linear scan over `chats` is deliberate: this runs once
   * per save with at most 32 entries, and a hash table here would be more code
   * than the thing it speeds up. */
  for (int i = count - 1; i >= 0; i--) {
    if (total > 0 && kept >= total) {
      break;
    }
    int c = -1;
    for (int j = 0; j < nchats; j++) {
      if (chats[j].isChannel == keys[i].isChannel && chats[j].id == keys[i].id) {
        c = j;
        break;
      }
    }
    if (c < 0) {
      /* More conversations than there is budget for. The ones already holding
       * an allowance are the most recently active, which is the right set to
       * protect — so this drops the older conversation rather than thinning
       * everyone. */
      if (nchats >= maxChats) {
        continue;
      }
      c = nchats++;
      chats[c].isChannel = keys[i].isChannel;
      chats[c].id        = keys[i].id;
      chats[c].kept      = 0;
    }
    if (perChat > 0 && chats[c].kept >= perChat) {
      continue;
    }
    chats[c].kept++;
    kept++;
    keep[i] = 1;
  }
  return kept;
}
