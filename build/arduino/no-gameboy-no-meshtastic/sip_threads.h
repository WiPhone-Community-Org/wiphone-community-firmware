/*
 * sip_threads.h — grouping the SIP message store into conversations.
 *
 * The Messages app was built around an INBOX and an OUTBOX: two flat lists, one per
 * direction, exactly mirroring how `Messages` stores them. That is the storage layout
 * showing through the UI, and it is not how anyone thinks about texting — a conversation is
 * one person, with both halves interleaved. The Meshtastic app on this same phone already
 * works that way, which makes the split doubly odd.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * WHY THIS IS A SNAPSHOT AND NOT A VIEW OVER `Messages`
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * `Messages::preload(incoming, offset, count)` takes ONE direction and fills ONE shared
 * cache. A thread needs both directions at once, so a view built on that API would have to
 * flip the cache back and forth per message and re-read partition files each time.
 *
 * So this copies what it needs — time, direction, unread, text — into PSRAM and leaves the
 * cache alone. PSRAM has ~3.6 MB spare; internal contiguous heap is ~20 KB and is what the
 * SIP stack and the emulator fight over, so the choice is not close. The cost is that a
 * snapshot goes stale: rebuild it when a message arrives or is sent.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * ⚠ IDENTITY IS DIGITS, NOT THE URI STRING
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * The same correspondent reaches this store under several spellings — `14255550142@
 * seattle1.voip.ms` from the phonebook, `+1...` off an inbound SIP MESSAGE, a bare number
 * typed by hand, and whatever COVEY mirrored in. Grouping on the raw string would split one
 * person into three threads. `smsMirrorDigits()` is the one normaliser, shared with the
 * mirror so the two can never disagree about who is who.
 *
 * ⚠ A LoRa message has no phone number at all (its "URI" is a hex chip id). Those reduce to
 * no digits and are grouped under the raw string instead, so they still appear rather than
 * collapsing into one nameless thread.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * ⚠ BOUNDED, AND IT SAYS SO
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * Both builders read only the newest SIP_THREADS_SCAN messages per direction. An unbounded
 * scan means loading every partition on a phone that cannot afford it. The consequence is
 * real and must be surfaced rather than hidden: a correspondent whose last message is older
 * than that window will not appear in the list. `sipThreadsTruncated()` reports it so the UI
 * can say "older messages not shown" instead of quietly looking complete.
 */
#ifndef SIP_THREADS_H
#define SIP_THREADS_H

#include <stdint.h>
#include "sms_mirror.h"        // SMS_MIRROR_PEER_MAX, smsMirrorDigits()

/* ⚠ These sizes are INTERNAL RAM. The tables are BSS, which does not fragment — the thing
 * that actually kills this phone — but it still comes off the ~20 KB of contiguous internal
 * heap the SIP stack and the emulator compete for, so they are kept deliberately small.
 * 16 correspondents and 40 messages is a personal phone's texting, not a limit anyone will
 * meet; the URI is 64 because `14255550142@seattle1.voip.ms` is 28 characters.
 * The message TEXT does not appear here at all — it lives in PSRAM (see the .cpp). */
#define SIP_THREADS_MAX        16    // conversations listed
#define SIP_THREAD_MSGS_MAX    40    // messages shown in one thread
#define SIP_THREADS_SCAN      120    // newest messages read per direction
#define SIP_THREAD_URI_MAX     64

/* How much of a conversation gets RENDERED, in characters.
 *
 * ⚠ A character budget, not a message count: MultilineTextWidget allocates a buffer per
 * wrapped ROW from internal heap, so the cost is total text — forty short messages are
 * cheaper than four long ones. Newest messages win and the view says out loud when older
 * ones were left out.
 *
 * ⚠ THIS WAS CUT TO 600 ON A WRONG DIAGNOSIS AND IS NOW BACK. `largest` really was
 * collapsing when a thread opened, but the cause was `Messages::preload()` deep-copying 120
 * INI sections at once, not these rows — see the paging in sip_threads.cpp and the abort
 * backtrace in the log. Cutting this hid nothing and cost readability. If thread rendering
 * ever DOES look like the memory culprit again, measure before cutting: the app-open probe
 * in GUI::enterApp() prints `largest` either side of opening the screen.
 */
#define THREAD_TEXT_BUDGET   1800
#define THREAD_TEXT_MAX      2600

struct SipThread {
  char     peer[SMS_MIRROR_PEER_MAX];   // digits — the identity used for grouping
  char     uri[SIP_THREAD_URI_MAX];     // a full URI to reply to (from the newest message)
  uint32_t lastTime;
  uint16_t count;
  uint16_t unread;
};

struct SipThreadMsg {
  uint32_t    time;
  bool        incoming;
  bool        unread;
  const char* text;                     // PSRAM, owned by this module
  /* The sort tiebreakers. `vid` is the VoIP.ms message id ("v" in the store, 0 when the
   * message never met COVEY's mirror), assigned monotonically by VoIP.ms — so at EQUAL
   * times it recovers true order, including for a catch-up burst COVEY stamped with one
   * clamped fetch-moment timestamp. `seq` is the insertion number, so the comparator can
   * always return a nonzero answer: msgCompare used to return 0 for equal times, and
   * qsort is not stable, so a block of same-timestamp messages could legally come out in
   * a different order on two openings of the same thread.
   * ⚠ vid is a TIEBREAKER only, never the primary key: a text the mirror has not touched
   * has no id at all, and an id-primary sort would dump all of those to one end. */
  int64_t     vid;
  uint16_t    seq;
};

class Messages;

/* The grouping identity of a correspondent: bare digits for a phone number, else a
 * bounded form of the address (long ones keep a head plus a hash of the whole, so two
 * addresses sharing a long prefix do NOT collapse into one thread).
 *
 * ⚠ Anything deciding "does this message belong to that thread?" must use THIS, not its
 * own copy of the rule — MessagesApp::markThreadRead had a duplicate, and a divergence
 * there means messages silently never get marked read.
 * ⚠ It is an identity, not a label: show SipThread::uri to a human. */
void sipThreadIdentity(const char* uri, char* out, size_t cap);

/* Rebuild the conversation list, newest-first. Returns how many threads there are. */
int  sipThreadsBuild(Messages& msgs);
int  sipThreadsCount();
const SipThread* sipThreadAt(int i);

/* Rebuild the message list for one correspondent, oldest-first (reading order).
 * `peerOrUri` may be digits or any URI spelling. Returns how many messages there are. */
int  sipThreadOpen(Messages& msgs, const char* peerOrUri);
int  sipThreadMsgCount();
const SipThreadMsg* sipThreadMsgAt(int i);

/* True if the last build hit the scan bound, i.e. there are older messages it did not see. */
bool sipThreadsTruncated();

/* Free the PSRAM held by the open thread. Safe to call when nothing is open. */
void sipThreadsRelease();

#endif // SIP_THREADS_H
