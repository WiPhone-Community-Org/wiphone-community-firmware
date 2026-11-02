/*
 * booksync_inbox.cpp — see booksync_inbox.h.
 */
#include "booksync_inbox.h"

#include <string.h>

static BookSyncInboxItem s_items[BOOKSYNC_INBOX_MAX];
static int s_count = 0;
static uint32_t s_seq = 0;
/* Never reset by Init(), for the same reason s_seq is not: a reader holding an id from
 * before must not be able to match a different packet after. */
static uint32_t s_nextId = 1;

/* ---- tombstones: what has just been retired, so it cannot walk straight back in ----
 *
 * 🛑 COVEY SENDS EVERY RECORD TWICE, 7 SECONDS APART, BYTE-IDENTICAL — SyncSession
 * retransmits while `self._mesh_sends < 2` at books.py, with booksync.py's
 * MESH_REPEAT_S = 7.0, and a resend reuses the same nonce and turnedAt. The dedup loop below
 * only ever scans what is CURRENTLY parked, so once the reader has acted on a packet and it
 * has been removed, the second copy is "new" again and parks itself.
 *
 * ⛔ The visible result is the nag the decline branch exists to prevent: press "stay where I
 * am", and about seven seconds later the identical card is back. Accepting has the same
 * shape — you jump, and the place you just jumped to is offered again.
 *
 * A few hashes of recently retired packets is enough to close it. ⚠ Not cleared by Init():
 * a decline must survive leaving and re-entering Books, which is well inside the 7 s window.
 * ⚠ A 32-bit hash can in principle collide and silently drop one real offer; with eight
 * entries that is remote, and the cost is one missed card against a guaranteed repeat nag.
 * Pressing Sync again always produces a fresh nonce, so a deliberate resend is never a
 * tombstone hit. */
#define BOOKSYNC_INBOX_GONE 8
static uint32_t s_gone[BOOKSYNC_INBOX_GONE];
static int s_gonePos = 0;

static uint32_t textHash(const char* t) {
  uint32_t h = 2166136261u;              // FNV-1a
  for (; *t; t++) {
    h ^= (uint8_t)*t;
    h *= 16777619u;
  }
  return h ? h : 1u;                     // 0 marks an empty slot
}

static bool recentlyRetired(const char* text) {
  uint32_t h = textHash(text);
  for (int i = 0; i < BOOKSYNC_INBOX_GONE; i++) {
    if (s_gone[i] == h) {
      return true;
    }
  }
  return false;
}

static void markRetired(const char* text) {
  s_gone[s_gonePos] = textHash(text);
  s_gonePos = (s_gonePos + 1) % BOOKSYNC_INBOX_GONE;
}

void bookSyncInboxInit() {
  s_count = 0;
  memset(s_items, 0, sizeof(s_items));
  s_seq++;                             // never reset: a reader holding an old value must not match
  /* ⚠ THE TOMBSTONES ARE CLEARED HERE AND NOWHERE ELSE, and that is safe only because
   * NOTHING IN THE FIRMWARE CALLS THIS. The array is file-static, so the C runtime zeroes it
   * before setup(); the one caller that existed — BooksApp's constructor — was the 0.9.46
   * bug that emptied the parking spot at the moment the reader looked in it, and there is a
   * 🛑 comment at app_books.cpp where it was, saying not to put it back. This is a TEST
   * reset. Clearing tombstones is what keeps one test's removals from silently swallowing
   * the next test's pushes; on the device the memory has to outlive a trip out of Books,
   * because the sender's repeat lands ~7 s after the decline. */
  memset(s_gone, 0, sizeof(s_gone));
  s_gonePos = 0;
}

uint32_t bookSyncInboxSeq() {
  return s_seq;
}

bool bookSyncInboxPush(const char* text, uint32_t from, uint32_t rxUnix) {
  if (!text || !bookSyncIsSyncText(text)) {
    return false;
  }
  size_t len = strlen(text);
  if (len >= BOOKSYNC_MESH_TEXT_MAX) {
    return false;                    // longer than this protocol can produce: not ours
  }
  if (recentlyRetired(text)) {
    /* The sender's second copy of something already dealt with. It IS a sync packet — so
     * `true`, keeping it out of the Chats list exactly as before — it is simply not news. */
    return true;
  }

  // A resend of the same packet is the same news. Replace it in place rather than filling the
  // inbox with copies, which would push out positions for OTHER books.
  for (int i = 0; i < s_count; i++) {
    if (strcmp(s_items[i].text, text) == 0) {
      s_items[i].from = from;
      s_items[i].rxUnix = rxUnix;
      return true;                     // deliberately no s_seq bump — see the note in the header
    }
  }

  if (s_count >= BOOKSYNC_INBOX_MAX) {
    for (int i = 0; i + 1 < BOOKSYNC_INBOX_MAX; i++) {
      s_items[i] = s_items[i + 1];   // drop the oldest
    }
    s_count = BOOKSYNC_INBOX_MAX - 1;
  }
  BookSyncInboxItem* it = &s_items[s_count++];
  memset(it, 0, sizeof(*it));
  memcpy(it->text, text, len);
  it->text[len] = '\0';
  it->from = from;
  it->rxUnix = rxUnix;
  it->id = s_nextId++;
  if (s_nextId == 0) {
    s_nextId = 1;                    // 0 means "no packet"; skip it on the (never) wrap
  }
  s_seq++;
  return true;
}

int bookSyncInboxCount() {
  return s_count;
}

const BookSyncInboxItem* bookSyncInboxGet(int i) {
  return (i >= 0 && i < s_count) ? &s_items[i] : NULL;
}

void bookSyncInboxRemove(int i) {
  if (i < 0 || i >= s_count) {
    return;
  }
  markRetired(s_items[i].text);      // every removal path comes through here
  for (int k = i; k + 1 < s_count; k++) {
    s_items[k] = s_items[k + 1];
  }
  s_count--;
  /* Bumped on the way out as well as in, so that acting on one offer makes the reader look
   * again: two devices can each park a position for the same book, and the second must not
   * stay invisible until the next time the book is opened. */
  s_seq++;
}

uint32_t bookSyncInboxIdAt(int i) {
  return (i >= 0 && i < s_count) ? s_items[i].id : 0;
}

bool bookSyncInboxRemoveId(uint32_t id) {
  if (!id) {
    return false;
  }
  for (int i = 0; i < s_count; i++) {
    if (s_items[i].id == id) {
      bookSyncInboxRemove(i);
      return true;
    }
  }
  /* Already gone — evicted while the card was up, or removed by another path. Not an error:
   * the caller wanted it out of the inbox and it is out. */
  return false;
}

int bookSyncInboxDropForBook(const uint8_t key[32], const char* const* ids, int nIds) {
  int dropped = 0;
  /* Backwards, so removing one does not shift an entry we have not looked at yet. */
  for (int i = s_count - 1; i >= 0; i--) {
    BookSyncRecord r;
    if (!bookSyncUnpackMesh(s_items[i].text, key, &r)) {
      continue;                        // not ours to judge: wrong passcode, or not a record
    }
    if (!bookSyncRecordMatchesIds(&r, ids, nIds)) {
      continue;                        // a different book: leave it alone
    }
    bookSyncInboxRemove(i);
    dropped++;
  }
  return dropped;
}

int bookSyncInboxFindFor(const uint8_t key[32], const char* const* ids, int nIds,
                         BookSyncRecord* out, uint32_t* fromOut) {
  int best = -1;
  BookSyncRecord bestRec;
  memset(&bestRec, 0, sizeof(bestRec));

  for (int i = 0; i < s_count; i++) {
    BookSyncRecord r;
    // Verifies the mac as well as parsing: a packet signed with a different passcode simply
    // never matches, which is the whole reason the radio path did not need the key.
    if (!bookSyncUnpackMesh(s_items[i].text, key, &r)) {
      continue;
    }
    if (!bookSyncRecordMatchesIds(&r, ids, nIds)) {
      continue;
    }
    /* Newest by the sender's reading event, not by arrival: a packet delayed by the mesh
     * still describes when the page was turned.
     *
     * 🛑 BUT ONLY WHEN THERE IS A READING EVENT TO COMPARE. `turnedAt` is 0 whenever the
     * SENDER's clock is not trusted — sendMyPlace() stamps `ntpClock.getTrustedUtcTime()`
     * (app_books.cpp): NTP over WiFi, or since 0.9.79 the woods plate's GPS; never a clock
     * set from the mesh (clock_source.h). There is no RTC. A phone with neither since boot
     * therefore stamps EVERY packet it sends 0, and a stack of them all tie.
     *
     * ⛔ WITH A STRICT `>` A TIE LEFT THE FIRST-SEEN WINNER IN PLACE — and the array is
     * insertion-ordered, so first-seen is the OLDEST parked. Syncing repeatedly without
     * checking each one in between therefore offered the EARLIEST place, and every later
     * sync was silently discarded. That is {user}'s *"it sometimes gets confused on which one
     * of the places to sync to"*, and the "sometimes" is whether that sender had NTP.
     *
     * ⚠ The dedup in bookSyncInboxPush() replaces in PLACE, so a rebroadcast of an old
     * packet keeps its old low index rather than being promoted — another way the oldest
     * stayed pinned at the front.
     *
     * So: compare stamps only when BOTH sides carry one and they differ. Otherwise fall back
     * to arrival order, which is the one clock this device can always trust — and because we
     * walk the array oldest-first, taking the tie means the LAST parked wins. Latest news. */
    bool take;
    if (best < 0) {
      take = true;
    } else if (r.turnedAt && bestRec.turnedAt && r.turnedAt != bestRec.turnedAt) {
      take = r.turnedAt > bestRec.turnedAt;   // both stamped, and they disagree: later turn
    } else {
      take = true;                            // unstamped or an exact tie: newer arrival
    }
    if (take) {
      best = i;
      bestRec = r;
    }
  }

  if (best >= 0) {
    if (out) {
      *out = bestRec;
    }
    if (fromOut) {
      *fromOut = s_items[best].from;
    }
  }
  return best;
}
