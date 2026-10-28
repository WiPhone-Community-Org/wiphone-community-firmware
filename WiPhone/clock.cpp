/*
Copyright © 2019, 2020, 2021, 2022 HackEDA, Inc.
Licensed under the WiPhone Public License v.1.0 (the "License"); you
may not use this file except in compliance with the License. You may
obtain a copy of the License at
https://wiphone.io/WiPhone_Public_License_v1.0.txt.

Unless required by applicable law or agreed to in writing, software,
hardware or documentation distributed under the License is distributed
on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/

#include "clock.h"
#include "helpers.h"

#define LONG_TIME 0xffff

Clock ntpClock;

// A millisecond difference for a %ld log field (+-23 days is plenty to say "wildly wrong").
static long clampMs(int64_t v) {
  return (long)(v > 2000000000LL ? 2000000000LL : v < -2000000000LL ? -2000000000LL : v);
}

Clock::Clock(long timeOffsetSeconds) : timeOffsetSeconds(timeOffsetSeconds) {
  udpTime = new WiFiUDP();
  ntpServerIp = (uint32_t) 0;
  clockGpsPairReset(&gpsPair);
  clockMeshVoteReset(&meshVote);
  memset(&dg, 0, sizeof(dg));
  unixToHuman();

  mux = xSemaphoreCreateMutex();
  if (mux == NULL) {
    log_e("FAILED TO CREATED MUTEX");
  }
}

void Clock::startUpdates() {
  xTaskCreate(&Clock::thread, "ntp_thread", 8192, this, tskIDLE_PRIORITY, NULL);
}

void Clock::thread(void *pvParam) {
  Clock* clck = (Clock*) pvParam;

  clck->udpTime->begin(NTP_DEFAULT_LOCAL_PORT);
  /* ── BACK OFF WHEN THERE IS NO CLOCK TO BE HAD ─────────────────────────────────
   * This is the vendor's own "TODO: increase delay, if there is no Internet", and it
   * matters more than it looks. On a network where NTP simply never answers - a
   * hotspot or captive portal blocking UDP 123, which is exactly where {user} is as I
   * write this - the unbacked-off loop woke this task TWICE A SECOND FOREVER and
   * re-sent a packet every 2.5 s (NTP_REQUEST_VALID_MS). That is ~24 radio
   * wake-and-transmits a minute, indefinitely, against SIP's ~2. It never gives up
   * because there is nothing to give up on.
   *
   * The fast 500 ms poll is still right at first: it is how the REPLY gets picked up
   * promptly on a working network, where the whole exchange finishes in well under a
   * second. So keep it for the first ~10 s (≈4 sends), then double out to a one-minute
   * ceiling. A working network is unaffected; a dead one costs ~1 wake a minute instead
   * of 120. Recovery stays bounded: when the network returns, the clock is at most a
   * minute behind, which every consumer of it already tolerates (the mesh replay
   * capture, booksync stamps and the buzz-recency gate all handle an unknown clock). */
  uint32_t fails = 0;
  while (1) {
    bool updated = clck->update(millis());
    uint32_t delayMs;
    if (updated) {
      fails = 0;
      delayMs = TIME_UPDATE_DELAY_MS;
    } else {
      if (fails < 1000) {
        fails++;
      }
      delayMs = TIME_UPDATE_RETRY_DELAY_MS;                 // 500 ms: catching the reply
      if (fails > 20) {                                     // ~10 s of honest trying
        const uint32_t steps = (fails - 20) / 10;           // double every ~10 more tries
        delayMs = TIME_UPDATE_RETRY_DELAY_MS << (steps < 7 ? steps : 7);
        if (delayMs > 60000u) {
          delayMs = 60000u;
        }
      }
    }
    vTaskDelay(delayMs / portTICK_PERIOD_MS);
  }

  clck->udpTime->stop();
  vTaskDelete(NULL);
}

// Description:
//     Initially written for cooperative multitasking, it either sends an NTP request and exits or checks for NTP response and exits.
bool Clock::update(const uint32_t& nowMillis) {
  // Check if previous request is still valid, if not - send a new one
  if (sentRequest && udpParsePacketSafe(*udpTime)<=0 && elapsedMillis(nowMillis, sentMillis, NTP_REQUEST_VALID_MS)) {
    sentRequest = false;  // declare old request invalid
  }

  // Send request if not sent already
  if (!sentRequest) {

    // Check if we already know the NTP server IP address
    if ((uint32_t)ntpServerIp==0 || elapsedMillis(nowMillis, lastDnsResolvedMillis, Clock::ipAddressValidMillis)) {        // update NTP server IP address every 24 hours or so
      // TODO: resolveDomain returns only one possible IP (`dig -t A pool.ntp.org` shows much more); change this and use multiple IPs
      // TODO: query multiple IPs from the pool to ensure correct time
      ntpServerIp = resolveDomain(defaultNtpServer);
      if ((uint32_t)ntpServerIp==0) {
        log_i("NTP: could not resolve domain");
        return false;
      }
      log_i("NTP: domain resolved");
      lastDnsResolvedMillis = nowMillis;
    }

    // Send NTP packet
    // Source: https://github.com/arduino-libraries/NTPClient/blob/master/NTPClient.cpp
    // Credits: (c) 2015, Fabrice Weinberg
    memset(ntpBuff, 0, sizeof(ntpBuff));
    // Initialize values needed to form NTP request
    ntpBuff [0] = 0b11100011;   // LI, Version, Mode
    ntpBuff [1] = 0;     // Stratum, or type of clock
    ntpBuff [2] = 6;     // Polling Interval
    ntpBuff [3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    ntpBuff[12] = 49;
    ntpBuff[13] = 0x4E;
    ntpBuff[14] = 49;
    ntpBuff[15] = 52;
    // all NTP fields have been given values, now
    // you can send a packet requesting a timestamp:
    udpTime->beginPacket(ntpServerIp, NTP_REMOTE_PORT);
    udpTime->write(ntpBuff, NTP_PACKET_SIZE);
    udpTime->endPacket();
    log_i("NTP: request sent");

    sentRequest = true;
    sentMillis = nowMillis;
    return false;
  }

  // Request sent -> check if response received
  int cb = udpTime->read(ntpBuff, NTP_PACKET_SIZE);
  if (cb<=0) {
    return false;
  }

  // Response received
  // Extract time (since 1 January 1900)
  log_i("NTP: parsing response");
  uint32_t hi = word(ntpBuff[40], ntpBuff[41]);
  uint32_t lo = word(ntpBuff[42], ntpBuff[43]);
  uint32_t ntpTime = (hi << 16) | lo;
  log_i("%u from NTP, bytes = %d", ntpTime, cb);

  /* 🛑 ONLY A REAL ANSWER FROM THE SERVER ASKED. This used to take any 48 bytes on the port
   * with a non-zero, changed time as a TRUSTED clock: a transmit time before 1970 wrapped to
   * 2036+, and a clock that far ahead hides every KOSync place as "provably older", expires
   * every pin at the next sweep and goes out on the air in beacons — while GPS may not
   * override a fresh NTP (clock_source.h). Mode/LI/stratum/date: clockNtpReplySane(). */
  uint32_t saneUnix = 0;
  if (udpTime->remoteIP() != ntpServerIp || udpTime->remotePort() != NTP_REMOTE_PORT ||
      !clockNtpReplySane(ntpBuff, cb, &saneUnix)) {
    log_e("NTP: answer refused (from %s:%u, byte0=0x%02X stratum=%u, time %u) - clock unchanged",
          udpTime->remoteIP().toString().c_str(), (unsigned)udpTime->remotePort(),
          (unsigned)ntpBuff[0], (unsigned)ntpBuff[1], (unsigned)ntpTime);
    return false;
  }

  // Check for errors
  if (ntpTime==0) {
    return false;  //
  }
  if (ntpTime==lastNtpTime) {
    return false;  // server returned same time as before -> ignore it
  }

  // AVOID CONTEXT SWITCHES: updating time values
  if (xSemaphoreTake(mux, LONG_TIME) == pdTRUE) {

    /* NTP is authoritative: it sets the clock whatever set it before (clock_source.h). What
     * it replaced is kept for one log line below, because "NTP moved a GPS clock by 40 s" is
     * the one observation that would say the GPS path is wrong. */
    const uint8_t was = source;
    const int64_t before = (was != CLOCK_SRC_NONE) ? utcMsAtLocked(nowMillis) : 0;
    noteMeshReplacedLocked((int64_t)(ntpTime - SEVENTY_YEARS) * 1000, nowMillis);

    // No errors detected
    updated = everUpdated = true;
    lastNtpTime = ntpTime;
    lastMillis = nowMillis;
    /* The carried sub-second remainder belonged to the OLD timeline; left in place it put up
     * to 999 ms of the previous clock's phase onto this one. */
    extraMillis = 0;
    utcTime = ntpTime - SEVENTY_YEARS;          // UTC time in Unix epoch format (ntpTime starts at 1900, so at 1970 it would be 70 years)
    source = CLOCK_SRC_NTP;
    setMillis = ntpSetMillis = nowMillis;

    // Convert unix epoch into human-readable values
    unixToHuman();

    // Resume context switches
    xSemaphoreGive(mux);

    if (was == CLOCK_SRC_GPS || was == CLOCK_SRC_MESH) {
      log_e("CLOCK: NTP set the clock (it was %s; moved %ld ms)", clockSourceName(was),
            clampMs((int64_t)(ntpTime - SEVENTY_YEARS) * 1000 - before));
    }

  } else {
    log_e("failed to obtain clock mutex");
  }

  return true;
}

// ---------------------------------------------------------------- GPS and mesh time (0.9.79)
/* All the RULES are in clock_source.cpp, host-tested. What is here is the lock, the apply and
 * the log. Both entry points run on the LOOP task (the NMEA reader and the mesh receive path
 * are both in the superloop); only the lock keeps them consistent with the NTP thread. */

int64_t Clock::utcMsAtLocked(uint32_t atMs) {
  /* Signed: `atMs` can be a few ms BEFORE lastMillis when a set landed after the caller
   * stamped it (the NTP thread runs beside the loop). */
  const int32_t passed = (int32_t)(atMs - lastMillis);
  return (int64_t)utcTime * 1000 + (int64_t)extraMillis + passed;
}

/* Set the clock to `utcMsAtRx` as of millis() == rxMs, aged to NOW. Keeps the sub-second
 * phase: lastMillis goes back by the fraction, so getExactUtcTime() and getSecond() tick over
 * when UTC does, not up to a second late. (NTP keeps whole seconds, as it always has.) */
void Clock::applyLocked(uint8_t src, int64_t utcMsAtRx, uint32_t rxMs) {
  if (clockSourceTrusted(src)) {
    noteMeshReplacedLocked(utcMsAtRx, rxMs);    // before the mesh timeline is overwritten
  }
  const uint32_t nowMs = millis();
  const int64_t utcNowMs = utcMsAtRx + (int64_t)(uint32_t)(nowMs - rxMs);
  utcTime = (uint32_t)(utcNowMs / 1000);
  lastMillis = nowMs - (uint32_t)(utcNowMs % 1000);
  extraMillis = 0;
  source = src;
  setMillis = nowMs;
  updated = true;               // the loop redraws the clock and re-stamps waiting messages
  unixToHuman();
}

/* A TRUSTED source (NTP, GPS) is about to replace the clock. If it is a MESH clock, keep how far
 * it was out, so the SIP store can move every stamp it wrote provisionally under it by exactly
 * that (clock_source.h, "THE SIP MESSAGE STORE UNDER A MESH CLOCK"). `trustedMsAt` is the new
 * time at millis() == atMs. A mesh clock is set at most once a boot (it only fills an UNKNOWN
 * clock), so this records at most one offset; later NTP/GPS sets find a trusted clock and leave
 * it alone. */
void Clock::noteMeshReplacedLocked(int64_t trustedMsAt, uint32_t atMs) {
  if (source != CLOCK_SRC_MESH || meshSetId == 0) {
    return;
  }
  meshCorrId = meshSetId;
  meshCorrS = clockMeshCorrS(trustedMsAt, utcMsAtLocked(atMs));
}

ClockMsgStamp Clock::msgStamp() {
  ClockMsgStamp st = {0, 0, 0, 0};
  const bool locked = xSemaphoreTake(mux, LONG_TIME) == pdTRUE;
  if (!locked) {
    log_e("failed to obtain clock mutex");
  }
  if (source != CLOCK_SRC_NONE) {
    st.utc = exactUtcLocked();
    if (st.utc == 0) {
      st.utc = 1;               // a known clock never reads as "unknown" (cannot happen: sane years)
    }
    if (source == CLOCK_SRC_MESH) {
      st.meshId = meshSetId;    // nonzero whenever the source is mesh (set with it, under this lock)
    }
  }
  st.corrId = meshCorrId;
  st.corrS = meshCorrS;
  if (locked) {
    xSemaphoreGive(mux);
  }
  return st;
}

uint32_t Clock::msSinceSet(uint32_t nowMs) {
  return source == CLOCK_SRC_NONE ? 0 : nowMs - setMillis;
}

uint32_t Clock::msSinceNtp(uint32_t nowMs) {
  if (!everUpdated) {
    return CLOCK_NEVER_MS;
  }
  /* A few ms "negative" means NTP set it after `nowMs` was stamped: just now. Anything else is
   * the plain unsigned age (so an old set never reads as fresh, however long the uptime). */
  const uint32_t age = nowMs - ntpSetMillis;
  return age > 0xFFFF0000u ? 0 : age;
}

void Clock::gpsSentence(const NmeaFix& fx, uint32_t rxMs) {
  if (fx.rmcCount == gpsRmcSeen) {
    return;                     // a GGA completed: no NEW RMC to judge
  }
  gpsRmcSeen = fx.rmcCount;

  int64_t setMs = 0, diff = 0;
  const bool locked = xSemaphoreTake(mux, LONG_TIME) == pdTRUE;
  if (!locked) {
    log_e("failed to obtain clock mutex");
  }
  const uint8_t was = source;
  ClockNow now;
  now.source = source;
  now.utcMs = utcMsAtLocked(rxMs);
  now.msSinceNtp = msSinceNtp(rxMs);
  const int v = clockGpsStep(&gpsPair, &fx, rxMs, &now, &setMs, &diff);
  const bool sets = clockVerdictSets(v);
  if (sets) {
    applyLocked(CLOCK_SRC_GPS, setMs, rxMs);
  }
  const uint32_t newUtc = utcTime;
  if (locked) {
    xSemaphoreGive(mux);
  }

  dg.gpsVerdict = v;
  dg.gpsVerdictMs = rxMs;
  if (v >= CLK_GPS_SET_UNKNOWN && v <= CLK_GPS_KEEP_NTP_FRESH && was != CLOCK_SRC_NONE) {
    dg.gpsHaveDiff = true;
    dg.gpsDiffMs = (int32_t)clampMs(diff);
    dg.gpsDiffAtMs = rxMs;
  }
  /* log_e: the only level compiled in. Once per SET (rare: the first fix, a mesh upgrade, a
   * drift past 2 s) and at most once per 10 min for a fresh-NTP disagreement — never once a
   * second, never for the steady "agrees". */
  if (sets) {
    dg.gpsSets++;
    char when[24];
    unixToHuman(newUtc, when);
    if (was == CLOCK_SRC_NONE) {
      log_e("CLOCK: set from GPS -> %s UTC (%s)", when, clockVerdictText(v));
    } else {
      log_e("CLOCK: set from GPS -> %s UTC (%s; it was %s, gps-clock %ld ms)", when,
            clockVerdictText(v), clockSourceName(was), clampMs(diff));
    }
  } else if (v == CLK_GPS_KEEP_NTP_FRESH &&
             (diff > CLOCK_GPS_STEP_MS || diff < -CLOCK_GPS_STEP_MS) &&
             (gpsDisagreeLogMs == 0 || rxMs - gpsDisagreeLogMs > 600000u)) {
    gpsDisagreeLogMs = rxMs ? rxMs : 1;
    log_e("CLOCK: GPS disagrees with a fresh NTP by %ld ms - NTP kept (see clock_source.h)",
          clampMs(diff));
  }
}

void Clock::meshPositionTime(uint32_t node, bool privateChannel, uint32_t t, uint32_t rxMs,
                             const char* chanName) {
  if (t == 0) {
    return;                     // most positions: stock nodes strip the time (clock_source.h)
  }
  int v;
  uint32_t adopt = 0;
  if (source != CLOCK_SRC_NONE) {
    v = CLK_MESH_CLOCK_KNOWN;   // the everyday case, without the lock (clockMeshOffer re-checks under it)
  } else {
    const bool locked = xSemaphoreTake(mux, LONG_TIME) == pdTRUE;
    if (!locked) {
      log_e("failed to obtain clock mutex");
    }
    v = clockMeshOffer(&meshVote, source, node, privateChannel, t, rxMs, &adopt);
    if (clockVerdictSets(v)) {
      /* A fresh RANDOM id per mesh set, not the adopted time: a replay of the same Position in
       * two boots would give two equal times, and one boot's provisional message stamps would
       * then take the other boot's correction (clock_source.h, SA-3). Never 0 (= "none"). */
      meshSetId = esp_random() | 1u;
      applyLocked(CLOCK_SRC_MESH, (int64_t)adopt * 1000, rxMs);
    }
    if (locked) {
      xSemaphoreGive(mux);
    }
  }
  dg.meshVerdict = v;
  dg.meshVerdictMs = rxMs;
  dg.meshHeld = (uint32_t)clockMeshVoteCount(&meshVote, rxMs);
  if (clockVerdictSets(v)) {
    dg.meshNode = node;
    strlcpy(dg.meshChan, chanName ? chanName : "?", sizeof(dg.meshChan));
    char when[24];
    unixToHuman(adopt, when);
    log_e("CLOCK: set from the MESH -> %s UTC, from !%08x on '%s' (%s). Lower trust: KOSync, "
          "waypoint expiry and everything this phone transmits still treat it as unknown",
          when, (unsigned)node, dg.meshChan, clockVerdictText(v));
  } else if (v == CLK_MESH_WAIT || v == CLK_MESH_BAD_YEAR) {
    log_e("CLOCK: mesh time from !%08x on '%s': %s (%u held)", (unsigned)node,
          chanName ? chanName : "?", clockVerdictText(v), (unsigned)dg.meshHeld);
  }
}

void Clock::unixToHuman() {
  time_t t = utcTime + timeOffsetSeconds;
  localtime_r(&t, &datetime);

  //log_v("Unix timestamp = %u", utcTime + timeOffsetSeconds);
  //log_v("%02d:%02d:%02d %02d-%02d-%04d", getHour(), getMinute(), getSecond(), getDay(), getMonth(), getYear());
}

void Clock::unixToHuman(uint32_t epoch, char* str) {    // static function
  struct tm dt;
  time_t t = epoch;
  localtime_r(&t, &dt);
  sprintf(str, "%04d-%02d-%02d %02d:%02d:%02d", dt.tm_year + 1900, dt.tm_mon+1, dt.tm_mday, dt.tm_hour, dt.tm_min, dt.tm_sec);
}

/*
 * Update time based on the CPU milliseconds clock.
 * Should be called roungly once per minute after each NTP update.
 * If this is not called in a longer time, it will calculate time since last call and update clock accordingly.
 */
void Clock::minuteTick(const uint32_t& nowMillis) {
  log_v("Tick: %d millis %d %d", nowMillis - lastMillis, ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // AVOID CONTEXT SWITCHES: updating time values
  bool locked = xSemaphoreTake(mux, LONG_TIME) == pdTRUE;
  if (!locked) {
    log_e("failed to obtain clock mutex");
  }

  /* Calculate passed time, millis.
   * 🛑 SIGNED, AND "BEFORE THE LAST SET" IS NOTHING PASSED. `nowMillis` is the loop's `now`,
   * stamped at the top of its pass, and a set can land AFTER it — the NTP thread runs beside
   * the loop, and since 0.9.79 a GPS or mesh set happens inside the pass itself — leaving
   * lastMillis a little later than `now`. The old code read that as the millis() wrap and
   * added ~49.7 DAYS. The real wrap is still right (the unsigned subtraction), and lastMillis
   * is left alone so the next tick counts from the set. Only a tick gap over 24.8 days would
   * be misread; the loop ticks every minute. */
  const int32_t passedSigned = (int32_t)(nowMillis - lastMillis);
  if (passedSigned > 0) {
    const uint32_t passedMs = (uint32_t)passedSigned;

    // Propagate passed time
    utcTime += passedMs / 1000;
    extraMillis += passedMs % 1000;
    if (extraMillis >= 1000) {
      utcTime += extraMillis / 1000;
      extraMillis %= 1000;
    }
    lastMillis = nowMillis;
  }

  unixToHuman();

  // Resume context switches
  if (locked) {
    xSemaphoreGive(mux);
  }
}

uint32_t Clock::exactUtcLocked() {
  // Calculate passed time, millis
  uint32_t passedMs = millis();
  if (passedMs >= lastMillis) {
    passedMs -= lastMillis;
  } else
    // Milliseconds overflow (happens once per 24.85 days)
  {
    passedMs += 0xFFFFFFFF - lastMillis;
  }
  return utcTime + passedMs / 1000;
}

uint32_t Clock::getExactUtcTime() {
  // AVOID CONTEXT SWITCHES: updating time values
  bool locked = xSemaphoreTake(mux, LONG_TIME) == pdTRUE;
  if (!locked) {
    log_e("failed to obtain clock mutex");
  }

  uint32_t exactTime = exactUtcLocked();

  // Resume context switches
  if (locked) {
    xSemaphoreGive(mux);
  }

  return exactTime;
}

void Clock::setTimeOffset(long offsetSeconds)  {
  // AVOID CONTEXT SWITCHES: updating time values
  bool locked = xSemaphoreTake(mux, LONG_TIME) == pdTRUE;
  if (!locked) {
    log_e("failed to obtain clock mutex");
  }

  timeOffsetSeconds = offsetSeconds;
  unixToHuman();

  // Resume context switches
  if (locked) {
    xSemaphoreGive(mux);
  }
}

const char* Clock::getMonth3(uint8_t month) {
  switch(month) {
  case 1:
    return "Jan";
  case 2:
    return "Feb";
  case 3:
    return "Mar";
  case 4:
    return "Apr";
  case 5:
    return "May";
  case 6:
    return "Jun";
  case 7:
    return "Jul";
  case 8:
    return "Aug";
  case 9:
    return "Sep";
  case 10:
    return "Oct";
  case 11:
    return "Nov";
  case 12:
    return "Dec";
  default:
    return "N/A";      // suppresses warning
  }

}

const char* Clock::getMonth3() {
  return Clock::getMonth3(getMonth());
}

/* Description:
 *     prints day and month into `s` in "DD MON" format (5-6 characters)
 */
void Clock::shortDate(const uint32_t& epochTime, char* s) {
  time_t tm = epochTime;
  struct tm tmTime;
  localtime_r(&tm, &tmTime);
  sprintf(s, "%d %s", tmTime.tm_mday, getMonth3(tmTime.tm_mon+1));
}

/* Description:
 *     prints day, month and year into `s` in "DD MON YYYY" format (10-11 characters)
 */
void Clock::longDate(const uint32_t& epochTime, char* s) {
  time_t tm = epochTime;
  struct tm tmTime;
  localtime_r(&tm, &tmTime);
  sprintf(s, "%d %s %d", tmTime.tm_mday, getMonth3(tmTime.tm_mon+1), tmTime.tm_year+1900);
}

/* Description:
 *      converts time zone into floating point time offset. Accepts input like "-02:30" and float values like "-2.5" (which are equivalent)
 * Return:
 *      false on failure; error message is in `error`
 *      true otherwise; parsed value in `res` - offset from UTC in hours (e.g., -2, -3.75, 6.5)
 */
bool Clock::parseTimeZone(const char* text, float& res, const char*& error) {
  if (!text || *text=='\0') {
    error = "Empty string: number expected";
    return false;
  }

  // Find the end: skip all the trailing spaces
  size_t len = strlen(text);
  const char *end = text + len - 1;
  while (isspace(*end))
    if (--end<text) {
      break;
    }
  end++;

  // Skip all the spaces and unimportant characters on the left
  text += strspn(text, " +\t");

  if (strchr(text, ':')) {

    // HH:MM format (e.g. +02:30)

    if (text && *text!='\0') {
      char* endptr;

      // Convert hours
      res = strtof(text, &endptr);
      if (*endptr!=':') {
        error = "Hours error";
        return false;
      }

      // Convert and add minutes
      float minutes = strtof(endptr+1, &endptr);
      if (endptr!=end) {
        error = "Minutes error";
        return false;
      }
      minutes /= 60;
      res += (res < 0) ? -minutes : minutes;

      return true;
    }

  } else {

    // Floating point format (e.g. +2.5)

    if (text && *text!='\0') {
      char* endptr;
      res = strtof(text, &endptr);
      if (endptr!=end) {
        error = "Input error: type an integer";
        return false;
      }
      return true;
    }

  }
  error = "Timezone error";
  return false;
}

/* Description:
 *     prints how long ago the supplied time point (`tm`) happened.
 *     Used in the MessagesApp.
 */
void Clock::dateTimeAgo(const uint32_t& tm, char* str) {
  str[0] = '\0';
  if (utcTime>=tm) {
    if (utcTime-tm < 60) {
      strcpy(str, "<1 min");
    } else if (utcTime-tm < 3600) {
      int val = (utcTime-tm)/60;
      sprintf(str, "%d min%s", val, val>1 ? "s" : "");
    } else if (utcTime-tm < 86400) {
      int val = (utcTime-tm)/3600;
      sprintf(str, "%d hour%s", val, val>1 ? "s" : "");
    } else if (utcTime-tm < 86400*30) {
      int val = (utcTime-tm)/86400;
      sprintf(str, "%d day%s", val, val>1 ? "s" : "");
    } else if (utcTime-tm < 86400*365) {
      shortDate(tm, str);
    } else {
      longDate(tm, str);
    }
  }
}
