/*
 * sms_mirror_rx.h — taking a mirrored text from COVEY and putting it in the message store.
 *
 * One entry point, deliberately, because BOTH transports end here: a CSM1 line off the radio
 * and a CSM1 line out of an HTTP response are the same record and must be folded in the same
 * way. See sms_mirror.h for why COVEY relays at all (the phone has no working TLS and cannot
 * reach the VoIP.ms API), and Storage.h's ingestMirrored() for how a duplicate is recognised.
 *
 * ⚠ THIS IS NOT A CHAT MESSAGE and must never be shown as one. On the mesh side it is
 * intercepted by prefix in meshtastic_service.cpp, in the same place and for the same reason
 * book-sync traffic is: a person did not send it to you, so it must not chime, vibrate, raise
 * an unread badge for the mesh app, or appear in the Chats list. What it does raise is the
 * SIP message state, because the text itself IS something to read.
 */
#ifndef SMS_MIRROR_RX_H
#define SMS_MIRROR_RX_H

#include <stddef.h>   // size_t, for sipCompleteAddress

/* Fold one CSM1 line into the SIP message store.
 *
 *   returns  1  a new text was stored
 *            0  we already had it (by VoIP.ms id, or paired with the phone's own copy)
 *           -1  the line was not a usable record, or there is no SIP account to file it under
 *
 * `wasIncoming`, when given, says whether that new message was one you RECEIVED. Callers need
 * it to decide whether to announce: a mirrored copy of a text you sent from COVEY is not news,
 * and a duplicate is not news either — but a text somebody sent you, arriving here first over
 * the radio, is exactly as much news as one arriving over SIP.
 *
 * ⚠ NEEDS A SIP ACCOUNT, and refuses without one. A mirrored text has to be stored against a
 * correspondent URI the phone would itself produce, or replying to it would build a different
 * address and the conversation would split in two. The account supplies the host half.
 */
int smsMirrorIngestLine(const char* line, bool* wasIncoming = 0);

/* True if `text` is mirror traffic — checked by prefix, before anything else, so that a
 * malformed or hostile packet is dropped rather than displayed. */
bool smsMirrorIsMirrorLine(const char* text);

/* The announcement handshake with the main loop — ONE place for BOTH transports.
 *
 * smsMirrorIngestLine() latches these whenever it stores something new; the main loop takes
 * them once per pass and does the announcing (buzz for an inbound arrival, NEW_MESSAGE_EVENT
 * so an open Messages screen rebuilds). The flags exist because the two transports used to
 * disagree: the LoRa path notified from inside meshService.loop() and the LAN path notified
 * NOWHERE — an arriving text stored silently, and because ingest dedups by id, a LAN-first
 * delivery permanently suppressed the LoRa copy's buzz too. Latching at the single ingest
 * point makes silence structurally impossible for any transport that ever calls it.
 *
 * Returns true if anything new was stored since the last take; *inbound is set true if at
 * least one of those was a text somebody sent YOU (the only kind worth a buzz). Both reset
 * on read. */
bool smsMirrorTakeNews(bool* inbound);

/* Complete a BARE PHONE NUMBER into a full SIP address using the phone's OWN account.
 *
 *   "4255550142"              -> "14255550142@seattle1.voip.ms"   (host from YOUR account)
 *   "alice@sip.example.com"   -> untouched (returns false)
 *   "LORA:00449040" / letters -> untouched (returns false)
 *
 * Returns true only when it wrote a completed address into `out`. This is what lets the
 * phonebook and the composer accept a plain number: TinySIP::sendMessage() uses addresses
 * VERBATIM, so a bare number used to call fine and silently fail to text. Adaptive by
 * construction, with no setting to configure: the suffix is whatever host the user's own
 * SIP account registered with, so a different provider gets ITS host appended — and any
 * input that is not purely digits passes through untouched, which is how non-number SIP
 * services keep working. Number formatting reuses buildPeerUri(), the mirror's measured
 * known-good form, so texting, calling and thread grouping can never disagree. */
bool sipCompleteAddress(const char* input, char* out, size_t cap);

#endif // SMS_MIRROR_RX_H
