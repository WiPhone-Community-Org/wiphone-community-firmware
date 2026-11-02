/*
 * sms_mirror_poll.h — fetching mirrored texts from COVEY over the LAN.
 *
 * The second of two transports (see sms_mirror.h for the record and for why COVEY relays at
 * all). They are NOT redundant:
 *
 *   LoRa  — works anywhere in radio range, but airtime is the budget, so COVEY pushes only
 *           messages NEWER than the moment it started. Nothing historical ever crosses.
 *   LAN   — the whole history, bounded only by a batch cap, and it carries the rare text
 *           too long to fit a packet. This is the "catch up when you get home" path.
 *
 * Both carry identical CSM1 lines and both end in smsMirrorIngestLine(), which dedups by
 * VoIP.ms message id — so running both costs nothing but a little airtime.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * ⚠ WHY THIS IS A STATE MACHINE AND NOT AN HTTPClient CALL
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * The whole UI is ONE task. A blocking request from the main loop is not slow, it is a
 * FROZEN PHONE — and this build already shipped that bug once: `resolveDomain()` blocked on
 * mDNS and lwip for SECONDS and presented as "the UI freezes for 5 seconds at random". A
 * 2.5 s HTTPClient GET every 60 s would be that bug rebuilt deliberately.
 *
 * A FreeRTOS task is the other obvious answer and it is worse here: task stacks CANNOT live
 * in PSRAM on the ESP32, and internal contiguous heap — ~20.5 KB on the plateau — is the
 * scarcest thing on this phone. Spending 4 KB of it to avoid writing a state machine is the
 * wrong trade when the same internal heap is what the SIP stack and the emulator fight over.
 *
 * So: one bounded step per main-loop pass. The only call that can block at all is
 * `connect()`, capped at CONNECT_TIMEOUT_MS, and it only happens once per poll interval.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * CONFIGURATION — a file on the SD card, not a settings screen
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * `smsmirror.txt` on the SD card, two lines:
 *
 *     192.168.1.55            <- COVEY's address, or a hostname
 *     <the token>             <- COVEY's prefs.json `smsmirror_token`
 *
 * Optional third line overrides the port (default 8087).
 *
 * ⚠ LOOKED FOR AT `/smsmirror.txt` AND `/roms/smsmirror.txt`, and the second is the one that
 * matters: the WiFi uploader writes to `/roms`, not to the card's root, so `/roms` is the
 * only one of the two that can be put there WITHOUT pulling the SD card out of the phone.
 * The uploader has no extension filter (that is how the test EPUB got on there), so a plain
 * .txt goes across fine.
 *
 * Deliberately a file rather than a settings screen: it needs no new UI to be usable, and it
 * keeps a shared secret out of the firmware image and out of this repo — which is PUBLIC.
 * If the file is absent the poller never runs, which is the right default for something that
 * talks to a machine most phones will never have.
 *
 * ⚠ PREFER AN IP. A hostname costs a DNS lookup, and `covey.local` costs a 500 ms BLOCKING
 * mDNS query that `resolveDomain()` deliberately does not cache (the `wiphone.local`
 * staleness trap). This module therefore caches the resolved address itself and re-resolves
 * only when a connection fails — see HOST_CACHE_MS.
 */
#ifndef SMS_MIRROR_POLL_H
#define SMS_MIRROR_POLL_H

#include <stdint.h>

/* Read /smsmirror.txt. Safe to call repeatedly; it only touches the SD card when it has not
 * successfully loaded yet. Returns true if a usable host and token are configured. */
bool smsMirrorPollConfigured();

/* True while a poll is mid-transfer (connected / reading). The main loop keeps its 1 ms
 * tick during that window so the one-step-per-pass state machine drains at full speed;
 * between polls the tick stretches to 5 ms — see the loop tail in WiPhone.ino. */
bool smsMirrorPollBusy();

/* One bounded step of the poll. Call from the main loop, unconditionally.
 *
 * `mayUseNetwork` is the caller's "is now a sensible time to open a socket" answer — pass
 * sipMayPoll(), which is false while the Game Boy owns its 16 KB of unmovable internal RAM.
 *
 * ⚠ THE GATE IS A PARAMETER, AND IT IS CHECKED HERE RATHER THAN AT THE CALL SITE, because
 * gating the whole call hid a bug worth not repeating: reading the config file and reporting
 * status are not network work, and skipping them while the Games app happened to be open
 * meant the poller could not even notice a config that had just been uploaded — and said
 * nothing, because the code that would have said something was skipped too. Only the socket
 * needs gating; knowing what is configured never does.
 *
 * Returns the number of NEW texts stored on this call (almost always 0 — a whole poll is
 * spread over many calls, and only the pass that reads a record's last byte can be
 * non-zero).
 */
int smsMirrorPollLoop(bool mayUseNetwork);

/* Ask for a poll on the next loop, ignoring the interval. For a "sync now" action. */
void smsMirrorPollRequestNow();

/* Human-readable state for a diagnostics screen: "idle", "connecting", "reading",
 * "off (no /smsmirror.txt)", or the last error. Never NULL. */
const char* smsMirrorPollStatus();

#endif // SMS_MIRROR_POLL_H
