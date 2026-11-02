/*
 * phone_pos.cpp — see phone_pos.h. Every decision (which port, when to ask again, whether a
 * sentence is current, whether a GGA is a fix worth keeping) is self_pos.cpp's and is proven on
 * the host by tests/test_selfpos.cpp; what is here is the socket and the glue, kept thin.
 */
#include "phone_pos.h"
#include "self_pos.h"
#include "meshtastic_service.h"   // phoneUpdate / setPhoneEnabled: the PHONE slot
#include "clock.h"                // ntpClock: only to ASK whether the clock is trusted (never set)
#include <esp_wifi.h>               // esp_wifi_sta_get_ap_info: is the joined network a phone hotspot
#include "Networks.h"             // wifiState.radioOff, lastWifiLinkUpMs: one cache per association
#include "app_gbc_xfer.h"         // xferUsingAP: our own hotspot is not a network with a phone on it

#include <Arduino.h>
#include <WiFi.h>                 // WiFi.status, WiFi.gatewayIP
#include <Preferences.h>
#include <lwip/sockets.h>         // the polled connect and the non-blocking reads
#include <errno.h>
#include <string.h>

/* At most this much off the socket a poll (a 128 B stack buffer, refilled): at 1 Hz an app sends
 * ~150 B a second, so 512 B every 100 ms drains anything a phone could have queued — the stale
 * burst included — in one or two polls, and a poll stays far under a millisecond of feeding. */
#define PP_READ_BUDGET   512
#define PP_CHUNK         128

enum { PP_IDLE = 0, PP_CONNECTING, PP_STREAM };

static int       s_state = PP_IDLE;
static int       s_fd = -1;
static uint32_t  s_pollMs = 0;
static uint32_t  s_connStartMs = 0;
static uint16_t  s_port = 0;            // the port of the attempt or stream in hand
static bool      s_delivered = false;   // ...and whether it has delivered a checksum-good sentence
static SelfProbe s_probe;               // zero = no association yet (selfProbeStart on the first)
static SelfPhoneStream s_stream;        // ~0.3 KB BSS: its own NmeaReader, never the woods UART's
static bool      s_init = false;
static uint16_t  s_customPort = 0;      // NVS wpmesh/phport, 0 = none
static bool      s_benchOn = false;     // `phonepos at`: RAM only
static uint32_t  s_benchIp = 0;         // in the byte order (uint32_t)IPAddress gives
static uint16_t  s_benchPort = 0;
static int       s_link = SELF_LINK_OFF;
static bool      s_saidNoApp = false;   // "no app answered" logged once per association
/* The association whose GATEWAY delivered a checksum-good sentence (0 = none): a phone's hotspot,
 * whose own access point travels with it (phonePosGatewayServedNmea, for Wi-Fi location). STICKY
 * for the association on purpose - NOT s_probe.port, which three refused reconnects, a new custom
 * port or Phone location switched off all clear (the review, 2026-10-10): a hotspot whose app
 * was stopped is still a hotspot, and its BSSID must not then reach beaconDB or a fix's yardstick
 * (a device that travels with the phone always counts as "still heard"). */
static uint32_t  s_nmeaAssoc = 0;
// Since boot, for `phonepos` (no coordinates anywhere in here):
static uint32_t  s_bytes = 0, s_connects = 0, s_drops = 0, s_refused = 0;

static void ensureInit() {
  if (!s_init) {
    selfPhoneInit(&s_stream);
    memset(&s_probe, 0, sizeof(s_probe));
    s_init = true;
  }
}

void phonePosSetup() {
  ensureInit();
  Preferences p;
  p.begin("wpmesh", true);
  s_customPort = (uint16_t)p.getUShort("phport", 0);
  p.end();
}

// The port list's third entry: the bench's port while `phonepos at` gives one, else the user's.
static uint16_t customPort() {
  return (s_benchOn && s_benchPort) ? s_benchPort : s_customPort;
}

/* Close whatever is open and go idle, WITHOUT judging the app: the reason is ours (off WiFi, a
 * game, the setting), so the probe's memory of which port answered is kept. */
static void dropSocket() {
  if (s_fd >= 0) {
    close(s_fd);
    s_fd = -1;
  }
  s_state = PP_IDLE;
}

// Start a non-blocking connect. False: no socket, or refused on the spot.
/* Does the joined network look like a phone's hotspot? Its BSSID's locally administered bit is
 * the shape Android and iOS hotspots use (a home router's is a universal address). Only decides
 * how soon a dead round is asked again (SelfProbe.hotspot) - never what is trusted. */
static bool joinedLooksLikeHotspot() {
  wifi_ap_record_t info;
  return WiFi.status() == WL_CONNECTED && esp_wifi_sta_get_ap_info(&info) == ESP_OK &&
         (info.bssid[0] & 0x02) != 0;
}

static void probeRestart(uint32_t assoc) {
  selfProbeStart(&s_probe, assoc);
  selfProbeSetHotspot(&s_probe, joinedLooksLikeHotspot());
}

static bool startConnect(uint32_t ip, uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  /* Small, as the review asked (CONFIG_LWIP_SO_RCVBUF=1 here). lwIP enforces it on datagram
   * sockets; what bounds a TCP stream is its window and the 100 ms reads — so the socket is
   * also closed whenever it is not being read (dropSocket). */
  int rcv = 1024;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  memcpy(&a.sin_addr.s_addr, &ip, 4);          // (uint32_t)IPAddress is already network order
  a.sin_port = htons(port);
  const int r = lwip_connect_r(fd, (struct sockaddr*)&a, sizeof(a));
  if (r < 0 && errno != EINPROGRESS) {
    close(fd);
    return false;
  }
  s_fd = fd;
  return true;
}

// A connect refused or not answered: the next port, or the back-off (self_pos.h, SelfProbe).
static void connectFailed(uint32_t now) {
  dropSocket();
  s_refused++;
  const bool before = selfProbeNoApp(&s_probe);
  selfProbeFailed(&s_probe, customPort(), now);
  if (!before && selfProbeNoApp(&s_probe) && !s_saidNoApp) {
    s_saidNoApp = true;
    /* Once per association: on a home router this is the normal answer, and the back-off
     * means it is asked a few times and then once every 5 minutes. */
    log_e("PHONE LOCATION: no location app answered on this network - asking again later");
  }
}

// The stream ended (closed, reset, or silent): judge it by whether it had delivered.
static void streamEnded(uint32_t now, const char* why) {
  dropSocket();
  s_drops++;
  if (s_delivered) {
    selfProbeDropped(&s_probe, now);
    log_e("PHONE LOCATION: stream from the app ended (%s) - reconnecting", why);
  } else {
    selfProbeFailed(&s_probe, customPort(), now);
  }
  s_delivered = false;
}

static void readStream(uint32_t now) {
  /* The trusted UTC second of the day, or -1: a sentence's own time is judged only against a
   * clock that NTP or the woods plate set (self_pos.h's stale guard). Asked, never told. */
  const uint32_t utc = ntpClock.getTrustedUtcTime();
  const int32_t utcSod = utc ? (int32_t)(utc % 86400u) : -1;
  const uint32_t sent0 = selfPhoneSentences(&s_stream);
  int budget = PP_READ_BUDGET;
  const char* ended = NULL;
  while (budget > 0) {
    uint8_t buf[PP_CHUNK];
    const int want = budget < PP_CHUNK ? budget : PP_CHUNK;
    const int r = recv(s_fd, buf, want, MSG_DONTWAIT);
    if (r > 0) {
      s_bytes += (uint32_t)r;
      budget -= r;
      SelfPhoneOut out;
      /* Kept only when it agrees with the fix before it: a single well-formed sentence can be
       * kilometres off (self_pos.h, selfPhoneKeep). When it is not kept the stream's verdict says
       * so, and the screen's line with it — never "fix" over an empty slot. */
      if (selfPhoneFeed(&s_stream, buf, (size_t)r, now, utcSod, &out) &&
          selfPhoneKeep(&s_stream, &out, now)) {
        meshService.phoneUpdate(out.latI, out.lonI, out.sats, out.hdopX10, s_benchOn);
      }
      continue;
    }
    if (r == 0) {
      ended = "closed by the app";
    } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
      ended = "connection lost";
    }
    break;                                     // r < 0 with EWOULDBLOCK: nothing more yet
  }
  if (!s_delivered && selfPhoneSentences(&s_stream) != sent0) {
    s_delivered = true;
    selfProbeGood(&s_probe, s_port);           // remembered for this association
    if (!s_benchOn) {
      s_nmeaAssoc = s_probe.assoc;             // the gateway itself answered (not a bench address)
    }
    log_e("PHONE LOCATION: a location app answers on port %u", (unsigned)s_port);
  }
  if (!ended && selfPhoneSilent(&s_stream, now)) {
    ended = "silent 10 s";
  }
  if (ended) {
    streamEnded(now, ended);
  }
}

void phonePosLoop(bool mayUseNetwork) {
  const uint32_t now = millis();
  if ((uint32_t)(now - s_pollMs) < SELF_PHONE_POLL_MS) {
    return;
  }
  s_pollMs = now;
  ensureInit();
  if (!meshService.isPhoneEnabled()) {
    s_link = SELF_LINK_OFF;
    dropSocket();
    return;
  }
  const bool sta = WiFi.status() == WL_CONNECTED;
  if (!mayUseNetwork || !selfPhoneMayRun(true, sta, xferUsingAP(), wifiState.radioOff())) {
    s_link = SELF_LINK_NO_WIFI;
    dropSocket();
    return;
  }
  /* One cache per association (the GOT_IP stamp): a new network, or the same one rejoined, may
   * have a different phone — or an app on a different port — behind its gateway. */
  const uint32_t assoc = lastWifiLinkUpMs();
  if (s_probe.assoc != assoc) {
    dropSocket();
    probeRestart(assoc);
    s_saidNoApp = false;
  }

  if (s_state == PP_IDLE) {
    s_link = selfProbeNoApp(&s_probe) ? SELF_LINK_NO_APP : SELF_LINK_LOOKING;
    if (!selfProbeDue(&s_probe, now)) {
      return;
    }
    s_port = selfProbePort(&s_probe, customPort());
    /* The GATEWAY, by address: on a hotspot the DHCP gateway is the phone, wherever Android put
     * its random subnet. No name is looked up, ever. 0 = no gateway (a static setup without
     * one): nobody to ask. */
    const uint32_t ip = s_benchOn ? s_benchIp : (uint32_t)WiFi.gatewayIP();
    if (ip == 0 || s_port == 0 || !startConnect(ip, s_port)) {
      connectFailed(now);
      return;
    }
    s_connStartMs = now;
    s_state = PP_CONNECTING;
    return;
  }

  if (s_state == PP_CONNECTING) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s_fd, &w);
    struct timeval tv = { 0, 0 };              // a look, not a wait
    const int r = select(s_fd + 1, NULL, &w, NULL, &tv);
    if (r < 0) {
      connectFailed(now);
      return;
    }
    if (r == 0) {
      if ((uint32_t)(now - s_connStartMs) >= SELF_PHONE_CONNECT_MS) {
        connectFailed(now);                    // a gateway that drops SYNs: given up on
      }
      return;
    }
    int err = 0;
    socklen_t len = (socklen_t)sizeof(err);
    if (getsockopt(s_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
      connectFailed(now);                      // ECONNREFUSED: nothing listens on that port
      return;
    }
    /* Connected. The socket STAYS non-blocking (KOSync hands its socket to an Arduino client here;
     * nothing of that kind is wanted for a read-only trickle). A new connection starts a clean
     * reader and the 2 s settle that drops gpsdRelay's backlog (self_pos.h). */
    selfPhoneBegin(&s_stream, now);             // ...its first fix waits for a second to agree
    selfPhoneSetPort(&s_stream, s_port);        // GPS 2 IP's stand-in numbers (self_pos.h)
    s_delivered = false;
    s_connects++;
    s_state = PP_STREAM;
    s_link = SELF_LINK_STREAM;
    return;
  }

  s_link = SELF_LINK_STREAM;
  readStream(now);
}

bool phonePosAuto() {
  return meshService.isPhoneEnabled();
}

void phonePosSetAuto(bool on) {
  Preferences p;
  p.begin("wpmesh", false);
  p.putBool("phloc", on);
  p.end();
  meshService.setPhoneEnabled(on);
  if (!on) {
    dropSocket();
    s_link = SELF_LINK_OFF;
  }
  log_e("PHONE LOCATION %s", on ? "AUTO" : "OFF");
}

uint16_t phonePosCustomPort() {
  return s_customPort;
}

void phonePosSetCustomPort(uint16_t port) {
  s_customPort = port;
  Preferences p;
  p.begin("wpmesh", false);
  p.putUShort("phport", port);
  p.end();
  // Ask again from the top on the next poll: the new port may be the one that answers.
  dropSocket();
  probeRestart(s_probe.assoc);
}

void phonePosBenchAt(const uint8_t ip[4], uint16_t port) {
  memcpy(&s_benchIp, ip, 4);                   // written order = the order IPAddress keeps
  s_benchPort = port;
  s_benchOn = true;
  dropSocket();
  probeRestart(s_probe.assoc);
  s_saidNoApp = false;
}

void phonePosBenchClear() {
  s_benchOn = false;
  s_benchIp = 0;
  s_benchPort = 0;
  dropSocket();
  probeRestart(s_probe.assoc);
  s_saidNoApp = false;
}

bool phonePosBenchOn() {
  return s_benchOn;
}

bool phonePosStreamLive() {
  return meshService.isPhoneEnabled() && s_state == PP_STREAM && s_delivered &&
         !selfPhoneSilent(&s_stream, millis());
}

bool phonePosGatewayServedNmea() {
  /* Set by the gateway's first checksum-good sentence on this association, and kept for as long
   * as the association lasts - whatever the probe forgets meanwhile (s_nmeaAssoc, above). A new
   * GOT_IP (or the drop that ends a spell) moves lastWifiLinkUpMs(), and the answer is no again. */
  return s_nmeaAssoc != 0 && s_nmeaAssoc == lastWifiLinkUpMs();
}

size_t phonePosStatusLine(char* out, size_t cap) {
  ensureInit();
  int link = s_link;
  if (!meshService.isPhoneEnabled()) {
    link = SELF_LINK_OFF;
  }
  return selfPhoneStatusText(out, cap, link, &s_stream, millis());
}

void phonePosDumpStatus(void (*sayLine)(const char*)) {
  ensureInit();
  char l[160];
  const uint32_t now = millis();
  static const char* const STATE[] = { "idle", "connecting", "streaming" };
  snprintf(l, sizeof(l), "phonepos: Phone location %s; %s, asking %s",
           meshService.isPhoneEnabled() ? "AUTO" : "OFF",
           STATE[(s_state < PP_IDLE || s_state > PP_STREAM) ? PP_IDLE : s_state],
           s_benchOn ? "the BENCH address (phonepos at; RAM only)" : "the network's gateway");
  sayLine(l);
  char st[64];
  phonePosStatusLine(st, sizeof(st));
  snprintf(l, sizeof(l), "phonepos: %s", st);
  sayLine(l);
  char ports[32];
  if (customPort()) {
    snprintf(ports, sizeof(ports), "10110, 11123, %u", (unsigned)customPort());
  } else {
    strlcpy(ports, "10110, 11123", sizeof(ports));
  }
  if (s_probe.port) {
    snprintf(l, sizeof(l), "phonepos: ports %s; port %u answered on this network",
             ports, (unsigned)s_probe.port);
  } else if (s_state != PP_IDLE) {
    snprintf(l, sizeof(l), "phonepos: ports %s; trying %u", ports, (unsigned)s_port);
  } else {
    snprintf(l, sizeof(l), "phonepos: ports %s; none has answered on this network yet", ports);
  }
  sayLine(l);
  snprintf(l, sizeof(l),
           "phonepos: bytes %lu, sentences %lu, bad checksums %lu, stale dropped %lu",
           (unsigned long)s_bytes, (unsigned long)selfPhoneSentences(&s_stream),
           (unsigned long)selfPhoneBadChecksums(&s_stream), (unsigned long)s_stream.nStale);
  sayLine(l);
  snprintf(l, sizeof(l),
           "phonepos: GGA verdicts: fix %lu, poor %lu, no fix %lu, estimate %lu, garbled %lu, "
           "disagreeing %lu; this connection RMC %lu GGA %lu",
           (unsigned long)s_stream.nFix, (unsigned long)s_stream.nPoor,
           (unsigned long)s_stream.nNoFix, (unsigned long)s_stream.nNotMeasured,
           (unsigned long)s_stream.nGarbled, (unsigned long)s_stream.nDisagree,
           (unsigned long)s_stream.rmcSeen, (unsigned long)s_stream.ggaSeen);
  sayLine(l);
  SelfFix fx;
  char age[40];
  if (meshService.getSourceFix(SELF_PHONE, &fx)) {
    snprintf(age, sizeof(age), "%lus ago (%s%s)", (unsigned long)(fx.ageMs / 1000u),
             selfStateName(fx.state), fx.injected ? ", BENCH" : "");
  } else {
    strlcpy(age, "none since boot", sizeof(age));
  }
  const uint32_t waitMs = (s_state == PP_IDLE && s_probe.wait &&
                           (int32_t)(s_probe.nextMs - now) > 0) ? s_probe.nextMs - now : 0;
  snprintf(l, sizeof(l),
           "phonepos: last fix %s; connects %lu, ended %lu, refused/unanswered %lu; "
           "failed rounds %u; next try in %lus",
           age, (unsigned long)s_connects, (unsigned long)s_drops, (unsigned long)s_refused,
           (unsigned)s_probe.deadRounds, (unsigned long)(waitMs / 1000u));
  sayLine(l);
}
