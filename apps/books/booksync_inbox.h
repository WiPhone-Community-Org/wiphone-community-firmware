/*
 * booksync_inbox.h — where a position from another device waits until you open the book.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * WHY A PARKING SPOT EXISTS AT ALL
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * A LoRa round trip does not fit inside the moment you press "Sync". COVEY solves this by
 * listening continuously and keeping late arrivals for later (`app._book_pending` — D-089),
 * and this is the same idea: the radio takes whatever turns up, whenever it turns up, and the
 * reader looks in here when it opens a book. Without it, "sync now, pick it up later" is not
 * possible and the feature only works if both devices are awake, in the same app, at the same
 * second.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * THE PACKET IS STORED RAW, AND VERIFIED LATER
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * ⚠ Deliberate: the mesh service must not need the booksync passcode. It runs in the radio
 * path and knows nothing about books, so it recognises a sync packet by its PREFIX only —
 * bookSyncIsSyncText(), checked BEFORE the mac, exactly as COVEY does. That ordering matters:
 * a packet signed with somebody else's passcode is still not a chat message, and dumping
 * "CBS1 MFRGG..." into the Chats list would be worse than dropping it.
 *
 * Verification happens in bookSyncInboxFindFor(), called by the reader, which is the only
 * part that holds the key. A packet that fails the mac is simply never matched.
 *
 * This module is plain C++ with no Arduino headers, so the matching logic — the part that can
 * silently do nothing — is exercised by the host tests.
 */
#ifndef BOOKSYNC_INBOX_H
#define BOOKSYNC_INBOX_H

#include "booksync.h"

/* Four is enough for the traffic this ever sees: one position per book per sync, from a
 * couple of devices. It is a static buffer, and internal RAM on this phone is measured in
 * kilobytes — see the heap note in app_books.h. */
#define BOOKSYNC_INBOX_MAX 4

struct BookSyncInboxItem {
  char     text[BOOKSYNC_MESH_TEXT_MAX];   // the packet exactly as received
  uint32_t from;                           // sender node number, for showing who
  uint32_t rxUnix;                         // when we heard it (0 if the clock is unknown)
  uint32_t id;                             // stable while parked; see bookSyncInboxRemoveId
};

void bookSyncInboxInit();

/* Park a packet. Returns false if it is not a sync packet at all. The oldest entry is evicted
 * when full, and a packet identical to one already parked replaces it rather than stacking —
 * a resend is the same news, not more of it. */
bool bookSyncInboxPush(const char* text, uint32_t from, uint32_t rxUnix);

int  bookSyncInboxCount();
const BookSyncInboxItem* bookSyncInboxGet(int i);
void bookSyncInboxRemove(int i);

/* 🛑 AN INDEX IS NOT A HANDLE. Parking a packet can EVICT the oldest, which shifts every
 * remaining entry down one, and bookSyncInboxRemove() shifts too — so an index held across
 * any gap can come to mean a different packet, or none.
 *
 * The reader holds its choice across a wait for a HUMAN KEYPRESS: checkForPending() picks a
 * packet, the sync card goes up, and the removal only happens when {user} presses OK or Back,
 * seconds or minutes later. `meshService.loop()` runs in the same superloop and parks
 * whatever arrives in every one of the thousands of iterations in between, so this is not a
 * race to lose occasionally — it is a guaranteed interleaving. Removing by index there
 * deleted an unrelated position and left the acted-on one parked, which then came back as
 * the next offer: the reader gets walked backwards through places it has already been while
 * the newest one is quietly dropped.
 *
 * So take an id at selection time and remove by that. Ids are never reused within a boot.
 */
uint32_t bookSyncInboxIdAt(int i);          // 0 when i is out of range
bool bookSyncInboxRemoveId(uint32_t id);    // false if it is already gone (which is fine)

/* Drop EVERY parked position for this book. Returns how many went.
 *
 * 🛑 CALL THIS ONCE THE READER HAS ACTED, INSTEAD OF REMOVING THE ONE PACKET IT ACTED ON.
 * Removing only that one leaves its predecessors parked, and bookSyncInboxRemove() bumps the
 * sequence — so the reading screen's tick looks again, finds the SECOND-newest position for
 * the same book, and puts up another card that is indistinguishable from the one just
 * accepted. {user} syncs three times as he reads (10%, 50%, 80%), opens the book, is offered
 * 80% and takes it — and is then walked BACKWARDS to 50%, then to 10%, one confirm at a time.
 * That is the other half of *"it sometimes gets confused on which one of the places to sync
 * to"*, and it needs no clock to go wrong.
 *
 * ⚠ This does discard a position another device parked for the same book. That is the point:
 * selection already picked the best of them, so everything still parked for this book is
 * older news or an untimed tie, and the reader has just said where it is. A position that
 * genuinely arrives LATER still lands normally and is offered normally.
 */
int bookSyncInboxDropForBook(const uint8_t key[32], const char* const* ids, int nIds);

/* Bumped whenever the contents change: a packet parked that was not already here, or one
 * taken out. It exists so the reader can ask "is there anything new?" on a timer for the
 * price of a comparison — bookSyncInboxFindFor() verifies an HMAC per parked packet, which
 * is not something to run once a second for nothing.
 *
 * ⚠ A packet IDENTICAL to one already parked does NOT bump it. A mesh rebroadcast is the
 * same news arriving twice, and re-offering a position the reader is already looking at
 * would turn flood routing into a nag. Pressing Sync again on the sender does produce a
 * different packet, so a deliberate resend still counts.
 *
 * ⛔ THAT USED TO SAY "its own turnedAt AND nonce", AND THE turnedAt HALF WAS FALSE — which
 * mattered, because bookSyncInboxFindFor() orders by turnedAt. Only the NONCE changes: COVEY
 * passes `self._turned_at` into every send and that moves on a PAGE TURN, not on a Sync
 * press (books.py), and a WiPhone with no NTP sends 0 every time. So a stack of Sync taps is
 * a stack of packets that are all distinct and all TIED on the field used to order them. See
 * the tie handling in bookSyncInboxFindFor(). */
uint32_t bookSyncInboxSeq();

/* The newest parked position that VERIFIES under `key` and is about the book with these ids.
 *
 * Returns its index and fills `out`/`fromOut`, or -1. Newest by the sender's own turnedAt,
 * because that is the reading event being described — not by when the packet happened to
 * arrive, which says more about radio conditions than about the reader.
 *
 * Nothing is removed: the reader decides whether to take the jump, and a position it declines
 * should not evaporate. Call bookSyncInboxRemove() once it has been acted on.
 */
int bookSyncInboxFindFor(const uint8_t key[32], const char* const* ids, int nIds,
                         BookSyncRecord* out, uint32_t* fromOut);

#endif // BOOKSYNC_INBOX_H
