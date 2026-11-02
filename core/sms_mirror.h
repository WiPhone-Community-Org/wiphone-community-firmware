/*
 * sms_mirror.h — mirroring SIP texts between COVEY and the WiPhone.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * WHY THIS EXISTS, AND WHY IT IS NOT AN API CLIENT
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * COVEY and this phone share one number (425-320-0782) but reach it two different ways.
 * The phone registers SIP as sub-account `100000_sample`. COVEY deliberately does not —
 * VoIP.ms overwrites a registration when the same sub-account is used twice, so a second
 * registration would TAKE inbound calls and texts off this phone. COVEY therefore reads
 * the account's history through the VoIP.ms REST API instead.
 *
 * The obvious fix for the resulting gap — "let the phone poll the same API and merge" —
 * CANNOT BE BUILT. It is the same memory wall that killed OTA, and it was re-verified
 * on 2026-08-17 before this file was written:
 *
 *   CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN     16384   -> a ~16 KB IN and a ~16 KB OUT buffer
 *   CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC      unset   -> neither may come from PSRAM
 *   free INTERNAL heap, whole phone                -> ~31 KB, largest block ~20.5 KB
 *
 * ~33 KB of internal heap is not available and never will be on this build. And mbedtls
 * here is 2.16.7, which predates MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH, so RFC 6066
 * max-fragment negotiation would NOT shrink those allocations — that escape hatch does
 * not exist. Nor is there a cleartext way round it: http://voip.ms/api/v1/rest.php answers
 * 301 to https. ⚠ Do not re-plan around the phone talking to VoIP.ms directly.
 *
 * So COVEY relays. It already holds the whole picture — `getSMS` returns the ACCOUNT's
 * history, which includes every text this phone sent and received — so COVEY is the
 * source of truth and the phone syncs FROM it. That is worth more than closing the
 * original one-way gap: it also back-fills texts that arrived while the phone was off.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * ONE RECORD, TWO TRANSPORTS
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 *   text   "CSM1 <id> <peer> <dir> <ts> <text...>"
 *   id     VoIP.ms message id, decimal. THE dedup key. Monotonic, assigned by VoIP.ms.
 *   peer   the correspondent as bare digits, no +1, no punctuation
 *   dir    'o' = we sent it, 'i' = we received it
 *   ts     unix time, 8 lowercase hex digits
 *   text   THE REST OF THE LINE, verbatim
 *
 * `text` being the last field is what lets it hold spaces with no escaping and no
 * quoting. Nothing after it needs parsing, so there is nothing for a stray character to
 * corrupt. Worst case on air: 38 bytes of frame plus a 160-character SMS = 198, against
 * MESH_TEXT_LEN 234. A single SMS can never exceed 160 characters (VoIP.ms rejects it
 * with `sms_toolong`, and COVEY splits before sending), so ONE record is always ONE
 * packet — this format never needs chunking or reassembly.
 *
 * ⚠ THE LAN TRANSPORT USES THIS EXACT FORMAT TOO, one record per line, and that is
 * deliberate. Serving JSON would mean writing a JSON parser to run on a phone with 20 KB
 * of contiguous internal heap, and testing two parsers instead of one. The mesh format
 * was already going to be line-oriented and bounded, so the LAN path just reuses it.
 *
 * ⚠ AND THAT IS WHY THE TEXT IS ESCAPED. People send multi-line texts, and one raw
 * newline in a last-position field splits a record in half on a line-oriented transport,
 * corrupting that record AND the one after it. So `\` `\n` `\r` are written `\\` `\n`
 * `\r` (two characters each). The useful invariant this buys: a RAW carriage return or
 * newline anywhere in a received line is always a transport artifact and is always safe
 * to strip, because a real one can no longer look like it.
 *
 * The cost is that a text of 160 newlines escapes to 320 characters and no longer fits a
 * packet. `smsMirrorPack()` fails rather than truncating — a truncated copy would not
 * match the phone's own copy of the same text and would be stored TWICE. The mesh sender
 * skips such a record and the LAN path, which builds into SMS_MIRROR_LINE_MAX, carries
 * it instead. A rare multi-line text therefore syncs when you are next on wifi rather
 * than over the radio. That is the honest trade and it is deliberate.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * NO APP-LAYER MAC, UNLIKE BOOKSYNC — a deliberate difference
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * booksync signs every packet with HMAC-SHA256 under a shared passcode. This does not,
 * and the reason is that the two features do not have the same problem:
 *
 *   * booksync rides a channel whose PSK is COMMITTED TO A PUBLIC REPO
 *     (docs/HANDOFF.md, and the `1111` passcode with it). Its MAC is doing real work.
 *   * this rides a channel created for it, with a fresh random PSK that is not written
 *     down anywhere public. Meshtastic's channel crypto already keeps non-holders out.
 *
 * The cost of a second secret is not zero: EVERY failure mode in booksync is silent, and
 * a passcode mismatch is indistinguishable from a wrong implementation. Halving the
 * number of secrets halves that. The prefix is versioned, so if this ever needs signing
 * it becomes CSM2 and both sides move together.
 *
 * ⚠ The consequence to be honest about: anyone holding the channel PSK can inject a text
 * into your history. That is the same trust boundary as the channel itself.
 */
#ifndef SMS_MIRROR_H
#define SMS_MIRROR_H

#include <stddef.h>
#include <stdint.h>

#define SMS_MIRROR_PREFIX     "CSM1"
#define SMS_MIRROR_PEER_MAX   20      // 10 digits and room for a mis-typed one
/* ⚠ 320, NOT 160, and the difference is inbound vs outbound.
 *
 * VoIP.ms rejects a SEND longer than 160 (`sms_toolong`), and COVEY splits before sending —
 * so anything this phone originates is 160. But a text somebody sends YOU can arrive as a
 * CONCATENATED SMS, which the network reassembles and `getSMS` returns as ONE record well
 * over 160 characters. Sizing this to the send limit meant an inbound long text was silently
 * cut, with the cut invisible: the parse succeeds, the message just quietly loses its end.
 *
 * Long records will not fit a LoRa packet, and that is fine and already handled — the mesh
 * sender skips what does not fit and the LAN path, which has no packet budget, carries it. */
#define SMS_MIRROR_TEXT_MAX   321     // 320 characters + NUL; see above
#define SMS_MIRROR_MESH_MAX   208     // longest line we will send ON AIR, plus slack
#define SMS_MIRROR_LINE_MAX   720     // longest line we can BUILD: every character escaped

struct SmsMirrorRecord {
  int64_t  id;                        // VoIP.ms message id — the dedup key
  char     peer[SMS_MIRROR_PEER_MAX]; // correspondent, bare digits
  bool     out;                       // true = sent by us, false = received
  uint32_t ts;                        // unix time (see the clock warning below)
  char     text[SMS_MIRROR_TEXT_MAX];
};

/* A phone number or SIP URI reduced to comparable digits.
 *
 * ⚠ THIS IS THE ONLY SAFE WAY TO COMPARE TWO CORRESPONDENTS HERE, because the same
 * person reaches this store under at least four spellings: COVEY sends `4255550142`,
 * the phonebook holds `14255550142@seattle1.voip.ms`, an inbound SIP MESSAGE arrives
 * from something like `+14255550142@seattle1.voip.ms`, and a human types `(425) 555-0142`.
 * A leading country-code 1 is dropped so all of those reduce to the same 10 digits.
 * Anything at or past '@' is ignored — a SIP URI's host is not part of the identity.
 *
 * Returns the number of digits written (0 on a NULL or digitless input).
 */
/* How many records have arrived too long for their buffer since boot. Non-zero means some
 * message is missing its tail — see smsMirrorUnpack(). */
extern unsigned smsMirrorTruncations;

size_t smsMirrorDigits(const char* s, char* out, size_t cap);

// True if `text` is a mirror packet. Prefix only — cheap, and checked before anything else.
bool smsMirrorIsMirrorText(const char* text);

// Build the wire line. False if the record is unusable or would not fit in `cap`.
bool smsMirrorPack(const SmsMirrorRecord* r, char* out, size_t cap);

/* Parse one wire line. False on anything malformed.
 *
 * Tolerant in the one way that matters: `text` may be empty, because a zero-length SMS is
 * not worth failing a whole sync over. Strict everywhere else — a bad id, a peer with no
 * digits or a non-hex timestamp all reject the record rather than storing a guess.
 */
bool smsMirrorUnpack(const char* line, SmsMirrorRecord* out);

/* Do two records describe the same text, ignoring the id?
 *
 * ⚠ TIME IS NOT PART OF THIS, AND THAT IS NOT LAZINESS. COVEY measured that VoIP.ms
 * dates do not reconcile to any single offset — with the account default, early messages
 * match the device clock exactly while later ones land up to an HOUR in the future. Any
 * time window tight enough to be useful would therefore reject genuine matches, and one
 * loose enough to be safe (>1 h) is not discriminating. Ordering and identity come from
 * the id; this predicate exists only to pair a mirrored record against the phone's own
 * copy of a text it already has, and peer + direction + exact text is what does that.
 */
bool smsMirrorSameMessage(const SmsMirrorRecord* a, const SmsMirrorRecord* b);

#endif // SMS_MIRROR_H
