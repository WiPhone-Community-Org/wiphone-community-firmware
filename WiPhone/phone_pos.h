/*
 * phone_pos.h — this phone's position from the GPS of the phone whose HOTSPOT it has joined.
 *
 * WHAT A PERSON SEES: Meshtastic > My node > "Phone location: auto" (the default on a new
 * phone; an upgraded phone whose beacon was armed but held by "GPS is off" comes up OFF, see
 * meshService.loadPosSettings). Join an Android or iPhone hotspot that runs an NMEA app, step
 * outside, and the map's "me" is the phone's GPS ("me (phone)"), the Almanac and the distances
 * use it, and — only if Position reporting is armed and a channel picked, as before — the
 * beacon sends it, marked EXTERNAL. The small line under the row says what is happening:
 * "this network: fix, 9 sats", "no app found on this network", "no fix (indoors?)".
 *
 * THE APPS, AND HOW THEY ARE FOUND (no configuration):
 *   gpsdRelay (Android, F-Droid)   TCP server — set it to port 10110, relaying the receiver's
 *   Share GPS (Android, Play)      own NMEA (gpsdRelay: generation off); 10110 is IANA's
 *                                  nmea-0183 port, so the instructions say 10110
 *   GPS 2 IP  (iPhone)             socket/server mode; its own default port 11123
 * While joined to ANY station network (not this phone's own hotspot, radio not off), the client
 * asks the network's DHCP gateway — on a hotspot, the phone itself — on 10110, then 11123, then
 * the user's own port (NVS wpmesh/phport, `phonepos port <n>`; 0 = none). The port that answers
 * is remembered for that association; a gateway that answers on none backs off to once every
 * 5 minutes (self_pos.h, SelfProbe). No SSID is stored and nothing is typed. Data is taken from
 * the gateway's address only (it is a TCP connection we open TO it).
 *
 * ⚠ THE TRUST MODEL, AND ITS PRICE (review 2026-10-10). Zero configuration means the gateway of
 * ANY network this phone joins is asked — a hotel, a campground, a friend's router, a rogue DHCP
 * server, or a boat/RV NMEA-over-WiFi gateway (those use 10110 too). Whoever answers there with
 * well-formed, current, agreeing GGAs (4+ sats, HDOP <= 10) CAN SET WHERE THIS PHONE THINKS IT
 * IS: the map's "me (phone)", every Here/Automatic distance and bearing, the Almanac's place
 * (legal light, the weather), and — if reporting is armed with a channel and the woods plate has
 * no live fix — the beacon, sent as EXTERNAL / 32 bits with the current trusted time, which
 * COVEY and stock receivers cannot tell from a real measurement. What it can NOT touch: the
 * clock (never set from here), NVS (the slot is RAM only), and the woods plate's own live fix,
 * which always outranks this source. The original design paired the source to one SSID; the
 * zero-config decision traded that away knowingly, and a per-network "send this network's phone
 * GPS?" confirm is the known next step if it matters (a RAM flag per association would stop the
 * beacon silently after every WiFi drop on phone 1, which is why it is not done blindly).
 * The screen says "this network: fix", never "on hotspot", for exactly this reason.
 *
 * THE RULES THIS MODULE KEEPS (tests/check_phonepos_review.py pins the first four):
 * 🛑 NEVER SETS THE CLOCK: no ntpClock.gpsSentence(). Unauthenticated bytes from a LAN do not get
 *    to be a TRUSTED time (clock_source.h); NTP already runs on any network with a gateway.
 * 🛑 NEVER TOUCHES the woods plate's reader (gGpsNmea / gGpsReader): its own NmeaReader lives in
 *    self_pos's SelfPhoneStream.
 * 🛑 NEVER BLOCKS THE LOOP: a raw lwIP socket, O_NONBLOCK, connect polled with a zero-timeout
 *    select() + SO_ERROR (the KOSync home client's pattern), recv(MSG_DONTWAIT), at most 512 B a
 *    poll. No WiFiClient (its connect waits), no name lookup at all.
 * 🛑 NO NEW TASK and NO CPU HOLD: polled every SELF_PHONE_POLL_MS (100 ms) from loop(), never in
 *    `busy`/`hardBusy` — the trickle (~150 B/s at 1 Hz) runs fine at 80 MHz, and a clock switch
 *    under a running radio is phone 1's known WiFi-drop trigger (cpu_clock_policy.h).
 * ⚠ The socket is CLOSED whenever it is not being read (off WiFi, our own hotspot up, radio off,
 *    a Game Boy game, the setting off): unread TCP data holds WiFi receive buffers.
 * ⚠ Logs and the console never print a coordinate, an SSID or the gateway's address.
 */
#ifndef PHONE_POS_H
#define PHONE_POS_H

#include <stddef.h>
#include <stdint.h>

/* setup(), after meshService.setup(): the custom port from NVS. (The on/off switch itself is
 * read by meshService.loadPosSettings(), from the same key, like `gpsen`.) */
void     phonePosSetup();
/* Every loop pass, after kosyncLoop(); it does its work at most every 100 ms. mayUseNetwork =
 * sipMayPoll() (false under a Game Boy game: the socket is closed). */
void     phonePosLoop(bool mayUseNetwork);

// "Phone location: auto | off" — persists (NVS wpmesh/phloc) and tells meshService.
bool     phonePosAuto();
void     phonePosSetAuto(bool on);
// The user's own port, tried after 10110 and 11123 (0 = none). Persists (NVS wpmesh/phport).
uint16_t phonePosCustomPort();
void     phonePosSetCustomPort(uint16_t port);
/* `phonepos at <ip>[:port]` — BENCH, RAM only: ask this address instead of the gateway, so a
 * Mac on the home network (tools/nmea_phone_stub.py) can stand in for the phone. port 0 = the
 * usual ports. Cleared by phonePosBenchClear() or a reboot. 🛑 Its fixes are BENCH fixes
 * (meshService.phoneUpdate's `bench`): shown, never sent by the schedule, never public. */
void     phonePosBenchAt(const uint8_t ip[4], uint16_t port);
void     phonePosBenchClear();
bool     phonePosBenchOn();

/* For Wi-Fi location (geoloc_net.cpp) and the auto-switcher (Networks.cpp), 0.9.82:
 *  - phonePosStreamLive(): a stream is connected and has delivered checksum-good sentences (the
 *    gateway's, or the bench address's). The auto-switcher does not hop to a stronger saved
 *    network while it is (geoloc.h, geoSwitcherMode): that would throw away the better source;
 *  - phonePosGatewayServedNmea(): THIS association's gateway (not a bench address) has answered
 *    with NMEA - it is a phone's hotspot, so its own access point travels with it and is left out
 *    of every Wi-Fi location lookup (geoJoinedLeftOut). STICKY for the association: the app being
 *    stopped, the probe forgetting its port, or Phone location switched off afterwards do not
 *    make the hotspot local. Never asked while Phone location was off (a hotspot's usual locally
 *    administered BSSID is left out anyway). */
bool     phonePosStreamLive();
bool     phonePosGatewayServedNmea();
// The small line under "Phone location" (self_pos.h's words; no coordinates, SSID or address).
size_t   phonePosStatusLine(char* out, size_t cap);
// `phonepos`: state, port, counters. Coordinates, SSIDs and the gateway's address never appear.
void     phonePosDumpStatus(void (*sayLine)(const char*));

#endif // PHONE_POS_H
