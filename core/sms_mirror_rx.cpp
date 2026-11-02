#include "sms_mirror_rx.h"
#include "sms_mirror.h"
#include "GUI.h"
#include "config.h"
#include "clock.h"     // ntpClock: the buzz is gated on the record being RECENT

#include <string.h>
#include <stdio.h>

extern GUI gui;

/* Latched by smsMirrorIngestLine, taken by the main loop — see the note in the header. */
static bool s_newStored  = false;
static bool s_newInbound = false;

static bool buildPeerUri(const char* digits, const char* ownUri, char* out, size_t cap);

bool smsMirrorTakeNews(bool* inbound) {
  const bool stored = s_newStored;
  if (inbound) {
    *inbound = s_newInbound;
  }
  s_newStored = false;
  s_newInbound = false;
  return stored;
}

bool smsMirrorIsMirrorLine(const char* text) {
  return smsMirrorIsMirrorText(text);
}

bool sipCompleteAddress(const char* input, char* out, size_t cap) {
  if (!input || !input[0]) {
    return false;
  }
  /* See through what earlier layers wrap a number in: the phonebook's save normaliser
   * prefixes "sip:" (so a contact stored as a bare number reads "sip:4255550142" — a
   * hostless URI that calls nothing and texts nothing), and people type numbers with a
   * leading '+'. Both still mean "this is a phone number, finish it for me". */
  const char* core = input;
  if (!strncasecmp(core, "sip:", 4)) {
    core += 4;
  }
  if (*core == '+') {
    core++;
  }
  if (!*core || strchr(core, '@')) {
    return false;                     // empty, or already a full address: not ours to touch
  }
  for (const char* p = core; *p; p++) {
    if (*p < '0' || *p > '9') {
      return false;                   // letters or punctuation: some other kind of address
    }
  }
  const char* ownUri = gui.state.fromUriDyn;
  if (!ownUri || !*ownUri) {
    return false;                     // no SIP account: nothing to complete WITH
  }
  return buildPeerUri(core, ownUri, out, cap);
}

/* Build the correspondent's URI in the form THIS PHONE would build it.
 *
 * ⚠ This matters more than it looks. `TinySIP::sendMessage()` uses the address VERBATIM —
 * unlike calling, which auto-appends the server for a bare number — so a stored correspondent
 * of "4255550142" would call fine and silently fail to text. That trap is already written up
 * in the handoff, and storing a bare number here would walk straight back into it.
 *
 * The host comes from the phone's own account rather than from anything COVEY sent, because
 * COVEY reaches VoIP.ms over a REST API and has no SIP host to offer. `100000_sample@
 * seattle1.voip.ms` therefore yields `14255550142@seattle1.voip.ms`, which is exactly the
 * phonebook form that is known to work for both calling and texting.
 */
static bool buildPeerUri(const char* digits, const char* ownUri, char* out, size_t cap) {
  const char* at = ownUri ? strchr(ownUri, '@') : NULL;
  if (!at || !at[1] || !digits || !digits[0]) {
    return false;
  }
  // A 10-digit NANP number is dialled with a leading 1 on this account; anything else is
  // passed through as-is rather than guessed at.
  int w = (strlen(digits) == 10)
          ? snprintf(out, cap, "1%s%s", digits, at)
          : snprintf(out, cap, "%s%s", digits, at);
  return w > 0 && (size_t)w < cap;
}

int smsMirrorIngestLine(const char* line, bool* wasIncoming) {
  if (wasIncoming) {
    *wasIncoming = false;
  }
  SmsMirrorRecord rec;
  if (!smsMirrorUnpack(line, &rec)) {
    return -1;
  }

  const char* ownUri = gui.state.fromUriDyn;
  if (!ownUri || !*ownUri) {
    /* No SIP account: refuse rather than invent a host. Storing under a made-up URI would
     * put the thread somewhere a reply could never reach, and it would not merge with the
     * real thread once an account was configured. */
    log_e("SMS mirror: no SIP account, refusing record %lld", (long long)rec.id);
    return -1;
  }

  char peerUri[128];
  if (!buildPeerUri(rec.peer, ownUri, peerUri, sizeof(peerUri))) {
    log_e("SMS mirror: cannot build a peer URI from '%s'", rec.peer);
    return -1;
  }

  if (!gui.flash.messages.isLoaded()) {
    gui.flash.messages.load(ntpClock.msgStamp());   // UTC, and mesh = provisional: see GUI::reloadMessages()
  }

  int r = gui.flash.messages.ingestMirrored(rec, ownUri, peerUri);

  /* ⚠ The preloaded cache has been disturbed by the scan inside ingestMirrored(). Clearing
   * it here rather than leaving it to the caller keeps the two transports from having to
   * remember: a Messages screen that is open rebuilds its menu from a clean cache on its
   * next NEW_MESSAGE_EVENT, which is the same path a delete already uses. */
  gui.flash.messages.clearPreloaded();

  /* Raise the SIP unread badge here, because the transports deliberately do NOT raise their
   * own. `unreadMessages` is event-driven, not polled — it is refreshed by reloadMessages()
   * and after reading a message, and nowhere else — so without this line an incoming
   * mirrored text would sit in the store with nothing on screen to say it had arrived.
   *
   * ⚠ Only INCOMING messages carry the unread flag ("u" is set by saveMessage only when
   * incoming), so a text mirrored back from something {user} sent on COVEY lights nothing.
   * That is the intended behaviour: he already knows about it. */
  if (r > 0) {
    gui.state.unreadMessages = gui.flash.messages.hasUnread();
    if (wasIncoming) {
      *wasIncoming = !rec.out;      // `out` is COVEY's view: the ACCOUNT sent it
    }
    /* Latch for the main loop's announcer — whichever transport this line rode in on.
     *
     * ⚠ The BUZZ latch additionally requires the record to be RECENT. A full resync
     * (first boot with an empty store, or a deleted since-file) stores hours or days of
     * old inbound texts in one poll, and each would otherwise latch a buzz — the motor
     * running continuously for the length of the catch-up, announcing history. Ten
     * minutes of slack covers every real transport delay (LoRa store-and-forward, a poll
     * gap, COVEY catching up after ITS fix) without letting an archive vibrate. When the
     * clock is not yet known the buzz wins over silence: a wrongly silent real arrival
     * is the exact fault this feature exists to fix. The UI latch is unconditional —
     * the screen should refresh whatever the age.
     * 🛑 TRUSTED (ntp/gps), not merely known (0.9.79): a mesh-set clock is one nobody
     * vouched for (clock_source.h — it can be spoofed AHEAD), and one ten minutes fast would
     * silence every real arrival. It counts as unknown here, so the buzz wins. */
    s_newStored = true;
    if (!rec.out) {
      const bool recent = !ntpClock.isTimeTrusted() || rec.ts == 0 ||
                          /* UTC: rec.ts is COVEY's real UTC stamp. Against the local-shifted
                           * epoch the "ten minutes of slack" above was really EIGHT HOURS ten
                           * minutes, so a full resync buzzed for everything under 8 h. */
                          (int64_t)ntpClock.getExactUtcTime() - (int64_t)rec.ts < 600;
      if (recent) {
        s_newInbound = true;
      }
    }
  }

  return r;
}
