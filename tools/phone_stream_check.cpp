/* phone_stream_check - read a phone app's NMEA stream on the Mac through the WiPhone's OWN gate.
 *
 * Links the firmware's self_pos.cpp / nmea.cpp / mesh_pos.cpp unchanged and does what the
 * phone_pos.cpp glue does with each chunk: selfPhoneFeed (checksums, the settle window, the
 * stale-time guard, the GGA/RMC pairing, sats/HDOP) and then selfPhoneKeep (two fixes in a row
 * must agree). So "kept" here means the WiPhone would have written the fix into its PHONE slot.
 *
 *   c++ -std=c++11 -O1 -IWiPhone tools/phone_stream_check.cpp WiPhone/self_pos.cpp \
 *       WiPhone/mesh_pos.cpp WiPhone/nmea.cpp -o /tmp/phone_stream_check
 *   /tmp/phone_stream_check <host> [port=10110] [seconds=30] [--show] [--port-as <n>]
 *
 * Exit 0 when at least one fix was kept and the link never went silent for SELF_PHONE_SILENT_MS.
 * Coordinates are NOT printed unless --show (a real phone's stream is someone's position):
 * without it each kept fix prints only its metres from the first kept fix.
 * --port-as tells the gate which port to believe it is on (11123 = GPS 2 IP's stand-in rule). */
#include "self_pos.h"
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint32_t msNow(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
}

static double metres(int32_t aLat, int32_t aLon, int32_t bLat, int32_t bLon) {
  const double k = M_PI / 180e7;
  const double x = (bLon - aLon) * k * cos((aLat + bLat) * 0.5 * k);
  const double y = (bLat - aLat) * k;
  return 6371000.0 * sqrt(x * x + y * y);
}

int main(int argc, char** argv) {
  const char* host = NULL;
  int port = 10110, seconds = 30, portAs = -1;
  bool show = false;
  int pos = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--show") == 0) {
      show = true;
    } else if (strcmp(argv[i], "--port-as") == 0 && i + 1 < argc) {
      portAs = atoi(argv[++i]);
    } else if (pos == 0) {
      host = argv[i];
      pos++;
    } else if (pos == 1) {
      port = atoi(argv[i]);
      pos++;
    } else {
      seconds = atoi(argv[i]);
      pos++;
    }
  }
  if (!host) {
    fprintf(stderr, "usage: %s <host> [port] [seconds] [--show] [--port-as <n>]\n", argv[0]);
    return 2;
  }
  struct addrinfo hints, *ai = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char portStr[8];
  snprintf(portStr, sizeof portStr, "%d", port);
  if (getaddrinfo(host, portStr, &hints, &ai) != 0 || !ai) {
    fprintf(stderr, "cannot resolve %s\n", host);
    return 2;
  }
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
    fprintf(stderr, "connect %s:%d: %s\n", host, port, strerror(errno));
    return 2;
  }
  freeaddrinfo(ai);
  struct timeval tv = {0, 200000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  static SelfPhoneStream s;
  selfPhoneInit(&s);
  const uint32_t t0 = msNow();
  selfPhoneBegin(&s, t0);
  selfPhoneSetPort(&s, (uint16_t)(portAs > 0 ? portAs : port));

  uint32_t kept = 0, bytes = 0, lastPrint = t0, maxGap = 0, lastData = t0;
  bool haveFirst = false, wentSilent = false;
  int32_t firstLat = 0, firstLon = 0;
  for (;;) {
    const uint32_t now = msNow();
    if ((int32_t)(now - t0) >= seconds * 1000) {
      break;
    }
    uint8_t buf[512];
    const ssize_t r = recv(fd, buf, sizeof buf, 0);
    const uint32_t at = msNow();
    if (r == 0) {
      printf("%6.1f s  closed by the app\n", (at - t0) / 1000.0);
      break;
    }
    if (r < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        printf("%6.1f s  connection lost: %s\n", (at - t0) / 1000.0, strerror(errno));
        break;
      }
    } else {
      bytes += (uint32_t)r;
      if (at - lastData > maxGap) {
        maxGap = at - lastData;
      }
      lastData = at;
      const time_t utc = time(NULL);
      SelfPhoneOut out;
      if (selfPhoneFeed(&s, buf, (size_t)r, at, (int32_t)(utc % 86400), &out) &&
          selfPhoneKeep(&s, &out, at)) {
        kept++;
        if (!haveFirst) {
          haveFirst = true;
          firstLat = out.latI;
          firstLon = out.lonI;
        }
        if (show) {
          printf("%6.1f s  KEPT %s %.6f %.6f sats %d hdop %.1f\n", (at - t0) / 1000.0,
                 selfPhoneVerdictName(out.verdict), out.latI / 1e7, out.lonI / 1e7, out.sats,
                 out.hdopX10 < 0 ? -1.0 : out.hdopX10 / 10.0);
        } else {
          printf("%6.1f s  KEPT %s %.1f m from the first, sats %d hdop %.1f\n",
                 (at - t0) / 1000.0, selfPhoneVerdictName(out.verdict),
                 metres(firstLat, firstLon, out.latI, out.lonI), out.sats,
                 out.hdopX10 < 0 ? -1.0 : out.hdopX10 / 10.0);
        }
      }
    }
    if (selfPhoneSilent(&s, at)) {
      wentSilent = true;
    }
    if (at - lastPrint >= 5000) {
      lastPrint = at;
      char line[96];
      selfPhoneStatusText(line, sizeof line, SELF_LINK_STREAM, &s, at);
      printf("%6.1f s  status: %s | verdict %s | %u bytes\n", (at - t0) / 1000.0, line,
             selfPhoneVerdictName(s.lastVerdict), bytes);
    }
  }
  close(fd);
  printf("summary: kept %u | gate fix %u poor %u nofix %u not-measured %u garbled %u stale %u "
         "disagree %u | sentences %u bad-checksum %u | longest gap %u ms%s%s\n",
         kept, s.nFix, s.nPoor, s.nNoFix, s.nNotMeasured, s.nGarbled, s.nStale, s.nDisagree,
         selfPhoneSentences(&s), selfPhoneBadChecksums(&s), maxGap,
         wentSilent ? " | WENT SILENT" : "", s.placeholder ? " | PLACEHOLDER NUMBERS" : "");
  return (kept > 0 && !wentSilent) ? 0 : 1;
}
