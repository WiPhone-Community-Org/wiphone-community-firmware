/*
 * geoloc_net.h - the device half of WI-FI LOCATION (0.9.82): the setting, the scan snapshot taken
 * from the auto-switcher's own scans, the two beaconDB lookups as one job on the shared HTTPS
 * worker (https_worker.h), the fold into the service's Wi-Fi slot, and the bench (`netloc`).
 * Every decision - which access points, the request bodies, the parser, the two-half agreement,
 * when to look, how long an answer stays true, what may be sent - is geoloc.h's or self_pos.h's,
 * host-tested; this file only asks them.
 *
 * WHAT A PERSON SEES: Meshtastic > My node > "Wi-Fi location: off | map only | map + mesh" (OFF by
 * default: turning it on sends the IDs of nearby access points to beaconDB, which the row says).
 *   map only    the map's "me +/-60 m Wi-Fi" with a ring at that size, the Almanac's place,
 *               distances - this phone only, never sent;
 *   map + mesh  also the position beacon (when armed, to the channel chosen on the screen), as a
 *               ~182 m cell with no location_source, under self_pos.h's sending rules - from the
 *               moment the My node screen is left with it chosen (geolocScreenLeft).
 * 🛑 THE OWNER'S RULE: own GPS > the hotspot phone's GPS > a Wi-Fi location ONLY when it is
 * genuinely local and accurate > NOTHING. Never an internet-address guess (none exists in this
 * firmware: tests/check_geoloc_review.py). The cable may set only off or map (`netloc off|map`):
 * "map + mesh" puts a position on other people's maps and is chosen on the screen.
 *
 * WHEN IT LOOKS (geoloc.h, geoLookDue): the setting on; WiFi up on someone else's network; no live
 * GPS or phone fix; the position wanted (Maps or the Almanac on screen in the last 10 min, a beacon
 * armed with "map + mesh", or `netloc now`); the access points changed (or never looked, or the
 * fix expired); 10 min since the last look (2 for `netloc now`), 3 attempts an hour; the weather's
 * gates; no music playing (only `netloc now` pauses it, as a weather Refresh does). A look needs a
 * scan of THIS association under 2 minutes old: it asks the auto-switcher for one
 * (wifiState.requestGeoScan, at most once per 2 minutes, through every rule that scan obeys).
 *
 * ONE LOOK = UP TO TWO POSTs on the worker, each on its own fresh WiFiClientSecure, the handshake
 * timed alone, the internal-heap bar checked again before EACH: half A, then half B only if A
 * was a tight 200 (geoSecondHalfNeeded). Pinned to ISRG Root X1 + X2 + Root YE (geoloc.h's
 * GEO_ROOTS_PEM). The LOOP folds the answer (geolocLoop, a flag test when idle).
 *
 * 🛑 NOTHING HERE LOGS OR PRINTS A MAC, AN SSID OR A COORDINATE: counts, verdicts, accuracies,
 * timings. A Wi-Fi fix's place is on the map and nowhere else.
 */
#ifndef GEOLOC_NET_H
#define GEOLOC_NET_H

#include <stddef.h>
#include <stdint.h>

// ── LOOP TASK ────────────────────────────────────────────────────────────────────────────
/* setup(), after meshService.setup(): the setting from NVS (wpmesh/netloc), told to the service
 * (its Wi-Fi slot: enabled unless off, sendable only with "map + mesh"). */
void geolocSetup();
int  geolocSetting();                    // GEO_SET_* (geoloc.h)
/* Persists and tells the service; OFF also forgets the fix and the snapshot, and calls off a look
 * that is queued or out (the worker checks before each half: nothing more leaves the phone once
 * the owner said no). The SCREEN may set any value; the console checks geoSettingSerialOk() first
 * (off/map only).
 * 🛑 "map + mesh" IS NOT ON THE AIR UNTIL THE MY NODE SCREEN IS LEFT (geolocScreenLeft): the row
 * cycles off -> map only -> map + mesh -> off, so the way from "map only" to "off" passes through
 * it, and that tap must never send (the review: an owed beacon slot sent a Wi-Fi cell between the
 * two taps). Until then the setting reads "map + mesh" on the row, NVS keeps the old value, and
 * the slot stays map-only (noTx). */
void geolocSetSetting(int v);
/* app_meshtastic.cpp, whenever the My node screen is left (another screen, or the app closed):
 * "map + mesh" chosen there is committed now - persisted, and the slot may send. A no-op
 * otherwise. */
void geolocScreenLeft();
/* Networks.cpp, before it starts a scan Wi-Fi location asked for: false while an AI question, a
 * weather fetch or a look is on the HTTPS worker (netRequestActive) - a scan's allocation inside
 * a handshake's internal-heap dip is the 2026-08-28 reboot. The ask then waits. */
bool geolocScanMayStart();
/* Every pass, after wxLoopTick(): fold a finished look (a flag test when idle), then at most once
 * a second the decisions: clear an expired fix (geoExpire), hold one whose WiFi link dropped or
 * changed (geoOnLink), look, ask for a scan (never while a handshake runs), a dry look. A look
 * never starts while a scan is in flight, nor in the first GEO_JOIN_SETTLE_MS of an association
 * (a hotspot's phone app gets to answer first, so its BSSID is left out). `viewing`: Maps or the
 * Almanac is on a lit screen right now (the position is wanted). */
void geolocLoop(bool viewing);
/* Networks.cpp's autoSwitchTick, for every scan that completed with n > 0, BEFORE
 * WiFi.scanDelete(): the snapshot ({BSSID, RSSI, flags} by WiFi.getScanInfoByIndex - no
 * Strings), its split, and geoOnScan (the fix confirmed, or dropped at once when its access
 * points are gone). Nothing at all - no copy - while the setting is off. */
void geolocOnScan(int n);
/* A look queued, running, or finished and not folded yet: https_worker's netRequestActive()
 * (WiPhone.ino's busy, hardBusy and the 10-tick sleep). */
bool geoRequestActive();

// ── the screen and the bench ─────────────────────────────────────────────────────────────
// The small line under "Wi-Fi location" (geoRowText: plain words, never a MAC or a coordinate).
size_t geolocRowText(char* out, size_t cap);
// `netloc now`: a look as soon as the gates allow (2-minute gap; may pause the music).
void geolocAskNow();
/* `netloc answer <A> <B>`: each half one JSON object as beaconDB would send it under HTTP 200,
 * or a 3-digit HTTP status with no body, or "-" (not asked). Through geoBenchFold: the fix is
 * a BENCH fix (shown, never sent by the schedule). *verdict = GEO_V_*. False with `why` when the
 * line does not parse or the setting is off. */
bool geolocBenchAnswer(const char* args, int* verdict, char* why, size_t whyCap);
// `netloc`: the setting, the snapshot (counts only), the last look, the slot, the job, the heap.
void geolocReport(void (*emit)(const char* line));

#endif // GEOLOC_NET_H
