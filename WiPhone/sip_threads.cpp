#include "sip_threads.h"
#include "Storage.h"
#include "config.h"
#include "helpers.h"          // hash_murmur, for the identity of a long address

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

/* How many messages are held in RAM at once while scanning. Each one is a deep-copied INI
 * section, so this is the number that decides whether the scan fits — not SIP_THREADS_SCAN,
 * which only says how far back to look. Ten is roughly what one menu page already costs. */
static const int32_t SCAN_PAGE = 10;

static SipThread     s_threads[SIP_THREADS_MAX];
static int           s_threadCount = 0;
static bool          s_truncated   = false;

static SipThreadMsg  s_msgs[SIP_THREAD_MSGS_MAX];
static int           s_msgCount = 0;
static char*         s_textPool = NULL;      // PSRAM: all of a thread's text, one block
static size_t        s_textUsed = 0;
static const size_t  TEXT_POOL_BYTES = SIP_THREAD_MSGS_MAX * (SMS_MIRROR_TEXT_MAX + 8);

int  sipThreadsCount()            { return s_threadCount; }
int  sipThreadMsgCount()          { return s_msgCount; }
bool sipThreadsTruncated()        { return s_truncated; }

const SipThread* sipThreadAt(int i) {
  return (i >= 0 && i < s_threadCount) ? &s_threads[i] : NULL;
}

const SipThreadMsg* sipThreadMsgAt(int i) {
  return (i >= 0 && i < s_msgCount) ? &s_msgs[i] : NULL;
}

void sipThreadsRelease() {
  if (s_textPool) {
    free(s_textPool);
    s_textPool = NULL;
  }
  s_textUsed = 0;
  s_msgCount = 0;
}

/* The identity of a correspondent, for grouping.
 *
 * Digits where there are any, and the raw string otherwise — a LoRa peer is a hex chip id
 * with no phone number in it, and reducing every one of those to "" would collapse them all
 * into a single nameless thread.
 *
 * ⚠ The no-digits branch used to be a plain snprintf into this 20-byte buffer, i.e. the
 * FIRST 19 CHARACTERS of the address. "sip:" eats four of those, so two correspondents at
 * the same long host — sip:alice.wonderland@a.com and sip:alice.wonderland@b.net — produced
 * the SAME identity, and their two conversations interleaved in ONE thread. The tail is
 * folded into a hash instead, which tells them apart inside the same 20 bytes. This string
 * is NOT for reading: callers display SipThread::uri, the full address.
 *
 * ⚠ SHARED ON PURPOSE. MessagesApp::markThreadRead derives the same identity to decide
 * which stored messages belong to the open thread. While that logic was duplicated there,
 * any change here would silently stop messages being marked read. */
void sipThreadIdentity(const char* uri, char* out, size_t cap) {
  if (cap == 0) {
    return;
  }
  if (smsMirrorDigits(uri, out, cap)) {
    return;
  }
  if (!uri || !uri[0]) {
    out[0] = '\0';
    return;
  }
  if (strlen(uri) < cap) {
    snprintf(out, cap, "%s", uri);          // fits whole: nothing to disambiguate
    return;
  }

  /* Too long for the buffer: keep a readable head and fold the whole address
   * into the tail so two long look-alikes stay apart.
   *
   * ⚠ LETTERS, NOT HEX. This function must be IDEMPOTENT — sipThreadOpen() is
   * handed an identity (from SipThread::peer) and derives the identity of THAT
   * to find the thread. smsMirrorDigits() scans for digits up to the first '@',
   * so a hex suffix like "#a1b2c3" would be mined for "123" on the second pass:
   * the identity would change under us and the thread would open EMPTY. With
   * a-z there are no digits to find, the second pass hits the length test
   * above, and the identity comes back unchanged. */
  char sfx[7];
  uint32_t h = hash_murmur(uri);
  for (int i = 0; i < 6; i++) {
    sfx[i] = (char)('a' + (h % 26));
    h /= 26;
  }
  sfx[6] = '\0';
  snprintf(out, cap, "%.*s#%s", (int)(cap > 8 ? cap - 8 : 1), uri, sfx);
}

static inline void identityOf(const char* uri, char* out, size_t cap) {
  sipThreadIdentity(uri, out, cap);
}

// ---------------------------------------------------------------- the list
static int findThread(const char* id) {
  for (int i = 0; i < s_threadCount; i++) {
    if (!strcmp(s_threads[i].peer, id)) {
      return i;
    }
  }
  return -1;
}

/* Fold one direction's newest messages into the thread table. */
static void scanDirection(Messages& msgs, bool incoming) {
  const int32_t total = incoming ? msgs.inboxTotalSize() : msgs.sentTotalSize();
  if (total <= 0) {
    return;
  }
  int32_t depth = total < SIP_THREADS_SCAN ? total : SIP_THREADS_SCAN;
  if (depth < total) {
    s_truncated = true;
  }

  /* ⚠ PAGED, AND THIS IS THE CRASH FIX. `preload()` DEEP-COPIES each message's INI section
   * into internal RAM, so asking for 120 at once meant 120 live section copies — and
   * `NanoIni::Section::deepCopy` allocates with `operator new`, which THROWS on failure.
   * Nothing catches it, so it goes std::terminate -> abort -> reset_reason=4. Decoded from
   * a real backtrace 2026-08-17; it is the same bug class as the WiFiUDP::parsePacket abort
   * already written up in the handoff.
   *
   * Clearing between pages is what bounds the peak: SCAN_PAGE sections live at a time
   * instead of the whole window. The scan still covers the same depth. */
  for (int32_t done = 0; done < depth; done += SCAN_PAGE) {
    const int32_t cnt = (depth - done < SCAN_PAGE) ? (depth - done) : SCAN_PAGE;
    const int32_t off = -1 - done;
    msgs.clearPreloaded();
    msgs.preload(incoming, off, cnt);
    for (auto it = msgs.iteratorCount(off, cnt); it.valid(); ++it) {
    MessageData& m = *it;

    char id[SMS_MIRROR_PEER_MAX];
    identityOf(m.getOtherUri(), id, sizeof(id));
    if (!id[0]) {
      continue;
    }

    int idx = findThread(id);
    if (idx < 0) {
      if (s_threadCount >= SIP_THREADS_MAX) {
        s_truncated = true;
        continue;                       // table full: say so rather than silently drop
      }
      idx = s_threadCount++;
      memset(&s_threads[idx], 0, sizeof(s_threads[idx]));
      snprintf(s_threads[idx].peer, sizeof(s_threads[idx].peer), "%s", id);
    }

    SipThread& t = s_threads[idx];
    t.count++;
    if (incoming && !m.isRead()) {
      t.unread++;
    }
    const uint32_t when = (uint32_t)m.getTime();
    if (when >= t.lastTime) {
      t.lastTime = when;
      // The URI to reply to comes from the NEWEST message, so it follows the correspondent
      // if their spelling ever changes — and it is a full URI, which is what
      // TinySIP::sendMessage() needs (it uses the address verbatim; a bare number
      // silently fails to send).
      snprintf(t.uri, sizeof(t.uri), "%s", m.getOtherUri());
    }
    }
  }
  msgs.clearPreloaded();
}

static int threadCompare(const void* a, const void* b) {
  const SipThread* x = (const SipThread*)a;
  const SipThread* y = (const SipThread*)b;
  if (x->lastTime == y->lastTime) {
    return strcmp(x->peer, y->peer);              // deterministic under an unstable qsort
  }
  return (x->lastTime > y->lastTime) ? -1 : 1;    // newest first
}

int sipThreadsBuild(Messages& msgs) {
  s_threadCount = 0;
  s_truncated = false;
  if (!msgs.isLoaded()) {
    return 0;
  }
  scanDirection(msgs, true);
  scanDirection(msgs, false);
  msgs.clearPreloaded();                          // leave the shared cache as we found it
  qsort(s_threads, s_threadCount, sizeof(s_threads[0]), threadCompare);
  return s_threadCount;
}

// ---------------------------------------------------------------- one thread
static void addMsg(MessageData& m, bool incoming) {
  if (s_msgCount >= SIP_THREAD_MSGS_MAX || !s_textPool) {
    return;
  }
  const char* text = m.getMessageText();
  if (!text) {
    /* ⚠ getMessageText() can return the c_str() of an EMPTY std::string member, and every
     * caller in this file assumed non-NULL. Cheap to check, and a NULL here would be exactly
     * the data-dependent crash one particular conversation would produce. */
    log_e("THREAD: message with NULL text, skipped");
    return;
  }
  size_t len = strlen(text);
  if (s_textUsed + len + 1 > TEXT_POOL_BYTES) {
    return;                                       // pool full; the cap already bounds this
  }
  char* dst = s_textPool + s_textUsed;
  memcpy(dst, text, len);
  dst[len] = '\0';
  s_textUsed += len + 1;

  SipThreadMsg& e = s_msgs[s_msgCount++];
  e.time = (uint32_t)m.getTime();
  e.incoming = incoming;
  e.unread = incoming && !m.isRead();
  e.text = dst;
  e.vid = strtoll(m.getValueSafe("v", "0"), NULL, 10);   // 0 = never met the mirror
  e.seq = (uint16_t)(s_msgCount - 1);
}

static void collect(Messages& msgs, bool incoming, const char* wantId) {
  const int32_t total = incoming ? msgs.inboxTotalSize() : msgs.sentTotalSize();
  if (total <= 0) {
    return;
  }
  int32_t depth = total < SIP_THREADS_SCAN ? total : SIP_THREADS_SCAN;

  // Paged for the same reason as scanDirection() above — see the note there.
  for (int32_t done = 0; done < depth; done += SCAN_PAGE) {
    const int32_t cnt = (depth - done < SCAN_PAGE) ? (depth - done) : SCAN_PAGE;
    const int32_t off = -1 - done;
    msgs.clearPreloaded();
    msgs.preload(incoming, off, cnt);
    for (auto it = msgs.iteratorCount(off, cnt); it.valid(); ++it) {
      MessageData& m = *it;
      char id[SMS_MIRROR_PEER_MAX];
      identityOf(m.getOtherUri(), id, sizeof(id));
      if (strcmp(id, wantId)) {
        continue;
      }
      addMsg(m, incoming);
    }
  }
  msgs.clearPreloaded();
}

static int msgCompare(const void* a, const void* b) {
  const SipThreadMsg* x = (const SipThreadMsg*)a;
  const SipThreadMsg* y = (const SipThreadMsg*)b;
  if (x->time != y->time) {
    return (x->time < y->time) ? -1 : 1;          // oldest first: reading order
  }
  /* Equal times: strictly lexicographic on (has-a-vid, vid, seq), and the SHAPE matters
   * as much as the keys. An earlier draft applied the vid rule only when both sides had
   * one and fell to seq otherwise — which is intransitive across a mixed block (A>C by
   * vid, C>B by seq, B>A by seq), and an inconsistent comparator makes qsort's output
   * unspecified: the exact nondeterminism this comparator exists to remove. Grouping
   * vid-bearing messages first, then by vid (true provider order — this is also what
   * puts a COVEY catch-up burst stamped with one clamped timestamp back in order), then
   * by seq, is a total order: never 0, never cyclic. */
  const bool xv = (x->vid != 0);
  const bool yv = (y->vid != 0);
  if (xv != yv) {
    return xv ? -1 : 1;                           // vid-bearing group first, uniformly
  }
  if (xv && x->vid != y->vid) {
    return (x->vid < y->vid) ? -1 : 1;
  }
  return (x->seq < y->seq) ? -1 : 1;
}

int sipThreadOpen(Messages& msgs, const char* peerOrUri) {
  sipThreadsRelease();
  if (!msgs.isLoaded() || !peerOrUri) {
    return 0;
  }

  /* One PSRAM block for every message's text, rather than a malloc per message.
   * ⚠ PSRAM deliberately: ~7 KB of internal heap is a third of the largest contiguous block
   * on this phone, and this is exactly the kind of allocation that made the WiFi PHY abort
   * once already. */
  s_textPool = (char*)ps_malloc(TEXT_POOL_BYTES);
  if (!s_textPool) {
    log_e("sip_threads: no PSRAM for %u bytes", (unsigned)TEXT_POOL_BYTES);
    return 0;
  }
  s_textUsed = 0;

  char want[SMS_MIRROR_PEER_MAX];
  identityOf(peerOrUri, want, sizeof(want));
  if (!want[0]) {
    return 0;
  }

  collect(msgs, true, want);
  collect(msgs, false, want);
  msgs.clearPreloaded();
  qsort(s_msgs, s_msgCount, sizeof(s_msgs[0]), msgCompare);
  return s_msgCount;
}
