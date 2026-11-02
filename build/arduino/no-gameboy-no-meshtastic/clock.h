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

#ifndef _NTP_CLOCK_
#define _NTP_CLOCK_

#include <time.h>
#include "config.h"
#include "Networks.h"
#include "clock_source.h"   // who set the clock, and the GPS/mesh rules (pure, host-tested)
#include "nmea.h"

#define NTP_PACKET_SIZE         48
#define NTP_DEFAULT_LOCAL_PORT  1337
#define NTP_REMOTE_PORT         123
#define SEVENTY_YEARS           2208988800UL

class Clock {
public:
  Clock(long timeZoneOffset = DEFAULT_TIME_OFFSET);

  void setTimeZone(float tz)                  {
    setTimeOffset(tz*ONE_HOUR_IN_SECONDS);
  }
  /* The offset in whole minutes east of UTC (tz_service.cpp: a zone's offset is computed, never
   * typed). Applied under the lock like setTimeZone, so every reader - the status bar, the
   * Almanac, message times, getExactUnixTime() - has it on its next read, without a reboot. */
  void setOffsetMinutes(int minutes)          {
    setTimeOffset((long)minutes * 60);
  }
  int getOffsetMinutes()                      {
    return (int)(int32_t)timeOffsetSeconds / 60;     // one aligned 32-bit read: atomic here
  }

  void begin(int port = NTP_DEFAULT_LOCAL_PORT);
  void sendNtpPacket();
  bool update(const uint32_t& nowMillis);
  void minuteTick(const uint32_t& nowMillis);

  /* ── KNOWN vs TRUSTED (0.9.79) ───────────────────────────────────────────────────────────
   * KNOWN: any source set it — ntp, gps, or mesh. Right for what this phone SHOWS: the clock,
   * message times, "5 min ago", the sun times. (The SIP store WRITES message times: it takes
   * msgStamp(), which marks a mesh clock's stamps provisional for NTP/GPS to correct - SA-3.)
   * TRUSTED: ntp or gps. 🛑 Required by everything that LEAVES the phone, DESTROYS or SILENCES
   * something on the strength of the time — those all needed a known clock before 0.9.79,
   * which meant NTP, and a mesh time must not quietly lower that bar:
   *   the position beacon's time (a second-hand mesh time on the air would come back as an
   *     "independent" witness for somebody else's mesh vote), booksync's turnedAt (x2, compared
   *     on COVEY), the "their clock looks wrong" test, the replay ring's stamps (served to
   *     COVEY), waypoint expiry (deletes camp), KOSync (its "provably older" HIDES a place,
   *     and it stamps the time it serves to the X4), and the mirrored-text buzz's "is it
   *     recent" (sms_mirror_rx.cpp: a clock ahead would hush real arrivals).
   * ⚠ A NEW CALLER THAT ACTS ON THE TIME ASKS isTimeTrusted(). The buzz gate was missed in
   * the first sweep precisely because isTimeKnown() still compiles and still reads right.
   * A mesh clock is KNOWN and not TRUSTED, so for all of those it is exactly the unknown clock
   * it replaced: nothing another device sees changes. See clock_source.h. */
  bool isTimeKnown()      {
    return source != CLOCK_SRC_NONE;
  }
  bool isTimeTrusted()    {
    return clockSourceTrusted(source);
  }
  uint32_t getTrustedUtcTime() {                  // UTC, or 0 when not trusted: the on-air form
    return isTimeTrusted() ? getExactUtcTime() : 0;
  }
  int getSource()         {
    return source;                                // ClockSource
  }
  /* The SIP message store's view of the clock, read in ONE lock (clock_source.h, "THE SIP
   * MESSAGE STORE UNDER A MESH CLOCK"): now in UTC (0 = unknown), whether that is a mesh
   * clock's (its set's id: the stamp is provisional), and the offset NTP/GPS found when it
   * replaced this boot's mesh clock. Every Messages::load() and every stamp the phone itself
   * writes into the store takes this, never getExactUtcTime() + isTimeKnown() separately. */
  ClockMsgStamp msgStamp();
  uint32_t msSinceSet(uint32_t nowMs);            // since the current source set it; 0 = unset
  uint32_t msSinceNtp(uint32_t nowMs);            // since NTP last set it; CLOCK_NEVER_MS = never

  /* GPS time. Called from the loop after EVERY sentence the NMEA reader completes (RMC or
   * GGA); acts only on a NEW RMC. `rxMs` = millis() when it completed. The rules are all in
   * clock_source.h (clockGpsStep); this takes the lock, asks, and applies. */
  void gpsSentence(const NmeaFix& fx, uint32_t rxMs);
  /* Mesh time: one received Position's `time` (0 = none). `privateChannel` is
   * !MeshtasticService::channelIsPublic(ch). Only ever fills an UNKNOWN clock. */
  void meshPositionTime(uint32_t node, bool privateChannel, uint32_t t, uint32_t rxMs,
                        const char* chanName);

  /* What the serial `clock` command prints. Written by the loop task only (the GPS and mesh
   * paths); the NTP thread never touches it. */
  struct Diag {
    int      gpsVerdict;       // the last GPS verdict (CLK_*); CLK_NONE before the first RMC
    uint32_t gpsVerdictMs;     // millis() of it
    bool     gpsHaveDiff;      // gpsDiffMs is a real measurement
    int32_t  gpsDiffMs;        // gps - clock at the last agreeing pair (clamped to +-24 days)
    uint32_t gpsDiffAtMs;
    uint32_t gpsSets;          // times GPS set the clock since boot
    int      meshVerdict;      // the last mesh verdict (CLK_*)
    uint32_t meshVerdictMs;
    uint32_t meshHeld;         // public-channel candidates held after that offer
    uint32_t meshNode;         // the node whose time SET the clock (0 = none)
    char     meshChan[16];     // ...the channel it came on
  };
  const Diag& diag() const { return dg; }
  bool isUpdated()        {
    bool res = updated;
    updated = false;
    return res;
  }

  uint8_t getHour()       {
    return datetime.tm_hour;
  }
  uint8_t getMinute()     {
    return datetime.tm_min;
  }
  uint8_t getSecond()     {
    return datetime.tm_sec + (millis() - lastMillis) / 1000 % 100;
  }

  uint8_t  getDay()       {
    return datetime.tm_mday;
  }
  uint8_t  getMonth()     {
    return datetime.tm_mon+1;
  }
  uint16_t getYear()      {
    return datetime.tm_year+1900;
  }

  uint32_t getUtcTime()   {
    return utcTime;
  }
  uint32_t getUnixTime()  {
    return utcTime + timeOffsetSeconds;
  }

  uint32_t getExactUtcTime();
  uint32_t getExactUnixTime() {
    return getExactUtcTime() + timeOffsetSeconds;
  }

  static const char* getMonth3(uint8_t month);    // three-letter month name
  const char* getMonth3();                        // three-letter month name

  static void shortDate(const uint32_t& epochTime, char* s);
  static void longDate(const uint32_t& epochTime, char* s);
  static bool parseTimeZone(const char* text, float& res, const char*& error);
  void dateTimeAgo(const uint32_t& utcTime, char* s);

  static void unixToHuman(uint32_t epochTime, char* str);

  void hello();
  void startUpdates();

protected:
  static constexpr const char* defaultNtpServer = "pool.ntp.org";
  static const uint32_t ipAddressValidMillis = 86400000;  // 86400000ms is 24 hours
  static const uint32_t NTP_REQUEST_VALID_MS = 2500;

  WiFiUDP*      udpTime = NULL;                 // socket for UDP
  IPAddress     ntpServerIp;                    // IP resolved for the default NTP server

  SemaphoreHandle_t mux = NULL;                 // lock for making the clock updates atomic

  /* NOTE: the following attributes are modified and used in both the updating thread and time retrieval functions:
   *    - timeOffsetSeconds     - modified in setTimeOffset()
   *    - utcTime               - modified in minuteTick(), update()
   *    - datetime              - modified in unixToHuman()
   *    - lastMillis            - modified in minuteTick(), update()
   *  They should be modified atomically to ensure consistency with each other.
   */

  bool          sentRequest = false;            // are we waiting for NTP server's response?
  bool          everUpdated = false;            // did we ever get a response from NTP server? (NOT "is the time known" since 0.9.79: see `source`)
  volatile uint8_t source = CLOCK_SRC_NONE;     // who set the clock last (ClockSource); written under `mux`
  uint32_t      setMillis = 0;                  // millis() when `source` set it
  uint32_t      ntpSetMillis = 0;               // millis() of the last NTP set (valid when everUpdated)
  uint32_t      gpsRmcSeen = 0;                 // NmeaFix::rmcCount already judged (a GGA leaves it)
  uint32_t      gpsDisagreeLogMs = 0;           // rate limit for "GPS disagrees with fresh NTP"
  ClockGpsPair  gpsPair;                        // the previous good RMC (loop task only)
  ClockMeshVote meshVote;                       // public-channel mesh candidates (loop task only)
  uint32_t      meshSetId = 0;                  // random id of this boot's mesh set (0 = none); under `mux`
  uint32_t      meshCorrId = 0;                 // the mesh set NTP/GPS replaced this boot (0 = none)...
  int32_t       meshCorrS = 0;                  // ...and how far that moved the clock (s, trusted - mesh)
  Diag          dg;
  bool          updated = false;                // updated recently?
  uint32_t      timeOffsetSeconds = 0;          // timezone offset

  uint32_t      utcTime = 0;                    // inferred current time (system clock time), updated only every minute or so
  struct tm     datetime;                       // calendar date and time broken down into its components
  uint32_t      lastNtpTime = 0;                // last good NTP timestamp received from the server
  byte          ntpBuff[NTP_PACKET_SIZE];       // buffer: allocated statically to ensure that there is always enough memory for this

  // CPU clock's millis() values:

  uint32_t      lastMillis = 0;                 // last time `utcTime` variable was changed
  uint32_t      extraMillis = 0;                // accumulated delay for the minuteTick() function - this will be added to the time to make the clock more precise
  uint32_t      sentMillis;                     // when was the last request sent to the NTP server?
  uint32_t      lastDnsResolvedMillis;          // when was the last time the NTP server domain got resolved?

  void          unixToHuman();                  // convert unix epoch into human-readable values (simply converts into "struct tm")
  int64_t       utcMsAtLocked(uint32_t atMs);   // what the clock reads at millis() == atMs, in ms (hold `mux`)
  uint32_t      exactUtcLocked();               // getExactUtcTime()'s arithmetic (hold `mux`)
  void          noteMeshReplacedLocked(int64_t trustedMsAt, uint32_t atMs);   // before a trusted set (hold `mux`)
  void          applyLocked(uint8_t src, int64_t utcMsAtRx, uint32_t rxMs);   // set it (hold `mux`)

  static void  thread(void *pvParam);

  void  setTimeOffset(long offsetSeconds);
};

extern Clock ntpClock;

#endif // _NTP_CLOCK_
