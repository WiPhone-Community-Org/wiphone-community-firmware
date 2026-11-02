/*
 * weather.h - the pure half of the Almanac's weather (0.9.81): everything that can be decided
 * without a network, a card or a screen, so the host suite (tests/test_weather.cpp) can prove it.
 * The device half - the gates, the worker's two GETs, the card - is weather_net.cpp; the rows are
 * weather_lines.cpp; the screen is the Almanac's WEATHER (app_almanac.cpp).
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header. Keep it that way.
 *
 * THE SOURCES (decided 2026-10-03 with {user}: "Yes and yes!"):
 *   - Open-Meteo's forecast, no key, free for non-commercial use, CC BY 4.0 - the screen credits
 *     "Weather: Open-Meteo.com". One GET, fetched in SI (m/s, C, hPa, mm) and formatted at draw
 *     time in the Units setting, with timeformat=unixtime (every time a UTC epoch) and
 *     timezone=auto (utc_offset_seconds: only ever a WARNING when it differs from the phone's
 *     offset - no clock is ever set from a response). Never sunrise/sunset (the Almanac owns
 *     them), never gzip (the phone's HTTPClient asks for identity; measured: 2.9 KB chunked).
 *   - The NWS's active alerts for the point (US only, public domain). One GET, &status=actual
 *     (a "Test" message is never a warning), a descriptive User-Agent with the project's URL and
 *     no e-mail (no User-Agent = a 403 HTML page from Akamai).
 *
 * MEASURED FROM THE MAC (2026-10-03, public points only - Seattle 47.61,-122.33 and offshore
 * 45,-130; the fixtures in tests/fixtures/wx/ are those, with the echoed point scrubbed):
 *   - Open-Meteo: 200, Transfer-Encoding: chunked, ~2.9 KB for current + 48 hours + 7 days.
 *     `current.time` is the start of a 15-MINUTE MODEL SLOT, never the fetch time; daily.time is
 *     the LOCATION's local midnight as a UTC epoch. Errors are 400 {"error":true,"reason":"..."}.
 *     null may appear in ANY array (a model with no value) - every reader here takes it.
 *   - NWS: an empty answer is ~230 B with Content-Length; one alert ~8.9 KB and two 14.7 KB as
 *     served (pretty-printed); a storm day is unbounded. A point outside the NWS domain (Canada,
 *     the UK) is a 400 whose detail says "out of bounds"; offshore US marine zones answer 200
 *     with NO features - so "200, empty" is "No alerts at 07:10", never "safe on land". Times
 *     carry the issuing office's offset ("2026-10-03T16:18:00-05:00"). `expires` is the MESSAGE's
 *     expiry (it can fall before onset); `ends` is the event's and may be null (Special Weather
 *     Statements): an alert is shown until `ends`, else `expires`. 6 of 415 had urgency "Past".
 *
 * THE FOUR ALERT ANSWERS, never a bare "No alerts": "No alerts at 07:10" (an empty 200);
 * "Alerts: US only (NWS)" (the 400 out of bounds - any OTHER 400 is a failure); "Alerts not
 * checked" (5xx, a timeout, TLS, an HTML page) keeping the previous alerts with their own as-of;
 * and "Alerts: 9+ (too many to list)" when the body outgrew the phone's 64 KB buffer - an empty
 * answer is ~230 B, so a body that big PROVES alerts exist, and the count comes from a streaming
 * counter that saw every byte (wxCountFeed), not from the buffer.
 */
#ifndef WEATHER_H
#define WEATHER_H

#include <stddef.h>
#include <stdint.h>

#define WX_OM_HOST        "api.open-meteo.com"
#define WX_NWS_HOST       "api.weather.gov"
/* NWS wants a User-Agent that names the application; the docs suggest a contact. The repo is
 * PUBLIC: its URL is the contact, never an e-mail. The version comes from config.h's
 * FIRMWARE_VERSION where the request is built (weather_net.cpp, WX_USER_AGENT) - this header is
 * host-tested and does not include config.h. 0.9.81 hard-coded "0.9.81" here (release review). */
#define WX_UA_CONTACT     "(+https://github.com/Nikguy321/wiphone-meshtastic)"

#define WX_HOURS          48     // forecast_hours=48: tonight's fetch still covers tomorrow evening
#define WX_DAYS           7
#define WX_ALERTS         8      // kept (most severe first); more are counted, not listed
#define WX_EVENT_MAX      40     // the longest of NWS's 111 event names is 32
#define WX_HEADLINE_MAX   128    // the longest headline seen nationwide was 119
#define WX_REASON_MAX     96
#define WX_PLACE_MAX      24     // AlmPlace.name

#define WX_STALE_S        3600   // an automatic fetch when the forecast is older than this...
#define WX_MOVED_KM       5.0    // ...or was fetched more than this from the place (a tolerance)
#define WX_RETRY_MS       (10u * 60u * 1000u)   // after a FAILED attempt, the next automatic one
#define WX_NOW_FRESH_S    3600   // `current` is "Now" within this of the fetch; then the hourly
#define WX_NO_ELEV        (-100000)

/* A missing value (null in the body, or none parsed) is NaN; wxHave() asks. */
bool wxHave(float v);

// ── the queries ──────────────────────────────────────────────────────────────────────────
/* The Open-Meteo URL: the point rounded to 2 decimals (~1 km - its grid snaps it anyway), and
 * &elevation=<m rounded to 10> when haveElev (the ground under the place from the phone's own
 * elevation tiles: the forecast is downscaled to it - measured 18.2 C at the grid cell's 147 m
 * against 13.5 C at 1,200 m; never the GPS altitude). Returns the length; `out` holds the WHOLE
 * URL or "" (a cut URL is a different query). */
size_t wxOpenMeteoUrl(double lat, double lon, bool haveElev, double elevM, char* out, size_t cap);
/* The NWS URL: /alerts/active?point=<lat 4dp>,<lon 4dp>&status=actual, trailing zeros stripped
 * ("45,-130", "47.61,-122.33"). Same length rule. */
size_t wxNwsUrl(double lat, double lon, char* out, size_t cap);
/* "47.61" from 47.6149 - the decimals given, trailing zeros (and a bare '.') stripped, never
 * "-0". The URLs' and the cache's spelling of a coordinate. */
void wxFmtCoord(double v, int decimals, char* out, size_t cap);
/* 0.01-degree units, rounded half away from zero (the point the forecast was ASKED for). */
int32_t wxE2(double deg);

// ── what a fetch brings back (SI units) ──────────────────────────────────────────────────
struct WxNow {                 // Open-Meteo's `current` (its own time - a 15-min slot - is not kept)
  float tempC, feelsC, rhPct, pressHpa, windMs, windDirDeg, gustMs;
  int   code;                  // WMO weather code, -1 = none
};
struct WxHour {                // one hourly entry: the hour that STARTS at t
  int64_t t;
  float   tempC, popPct, windMs;
  int     code;
};
struct WxDay {                 // one daily entry: t is the LOCATION's local midnight (UTC epoch)
  int64_t t;
  int     code;
  float   maxC, minC, popPct, precipMm, windMs, gustMs, dirDeg;
};
struct WxForecast {
  bool    haveOffset;
  int32_t utcOffsetS;          // the place's offset at fetch time (Open-Meteo's, for the warning)
  bool    haveNow;
  WxNow   now;
  int     nHours;
  WxHour  hour[WX_HOURS];
  int     nDays;
  WxDay   day[WX_DAYS];
};

enum { WX_SEV_UNKNOWN = 0, WX_SEV_MINOR, WX_SEV_MODERATE, WX_SEV_SEVERE, WX_SEV_EXTREME };
enum { WX_URG_UNKNOWN = 0, WX_URG_PAST, WX_URG_FUTURE, WX_URG_EXPECTED, WX_URG_IMMEDIATE };
enum { WX_MSG_ALERT = 0, WX_MSG_UPDATE, WX_MSG_CANCEL, WX_MSG_OTHER };
struct WxAlert {
  char    event[WX_EVENT_MAX];         // "Flood Warning" (ASCII; anything else folded to '?')
  char    headline[WX_HEADLINE_MAX];   // "" when null
  int     severity, urgency, msgType;  // WX_SEV_* / WX_URG_* / WX_MSG_*
  int64_t onset, ends, expires;        // UTC; 0 = null. Shown until ends, else expires.
};
/* The state of an alerts answer. */
enum {
  WX_AL_UNKNOWN = 0,           // never asked (or the place moved away from the last answer)
  WX_AL_OK,                    // n >= 1 listed
  WX_AL_NONE,                  // an empty 200 (or only "Past" / cancelled ones): "No alerts at 07:10"
  WX_AL_OUTSIDE,               // the 400 "out of bounds": "Alerts: US only (NWS)"
  WX_AL_TOO_MANY,              // the body outgrew the buffer: at least `total` exist
  WX_AL_FAILED,                // no usable answer: "Alerts not checked" (the old ones are kept)
};
struct WxAlerts {
  int     state;               // WX_AL_*
  int     n;                   // listed, most severe first
  WxAlert a[WX_ALERTS];
  int     kept;                // ...of them in force and shown (not Past, not cancelled): > n = "+N more"
  int     total;               // the features the body had (TOO_MANY: at least this many)
  int64_t updated;             // the body's "updated" (UTC), 0 = none: the last fallback stamp
};

// ── parsing (the worker runs these on the de-chunked body) ───────────────────────────────
enum { WX_OM_OK = 0, WX_OM_ERROR, WX_OM_BAD };
/* A 200's body: WX_OM_OK with `out` filled; WX_OM_BAD when it is not Open-Meteo's JSON (a
 * captive portal's HTML page, a body cut short, no hourly/daily) - the caller keeps its cache.
 * An error body ({"error":true,"reason":...}, a 400) is WX_OM_ERROR with the reason, ASCII,
 * in `reason`. `reason` may be NULL. */
int  wxParseOpenMeteo(const char* body, size_t len, WxForecast* out, char* reason, size_t rcap);
/* The NWS answer -> out->state. `overflow`: the body passed the buffer (it was still read to
 * its end); `counted`: the features wxCountFeed saw in ALL of it. Rules: a 200 parses whole (an
 * unparseable 200 is FAILED); urgency "Past" and messageType "Cancel" are dropped; most severe
 * first (Extreme > Severe > Moderate > Minor > unknown), then the most urgent, then the soonest
 * end; the first WX_ALERTS kept. An overflowed 200 is TOO_MANY with total = max(counted, 1) and
 * whatever whole features the buffer held. A 400 whose detail says "out of bounds" is OUTSIDE;
 * every other code FAILED. Returns out->state. */
int  wxParseNwsAlerts(int httpCode, const char* body, size_t len, bool overflow, int counted,
                      WxAlerts* out);
/* The streaming feature counter: `"event"` followed by ':' (whitespace allowed), fed every body
 * byte in any split - a body too big to keep is still counted to its end. */
struct WxEventCounter {
  int matched;                 // characters of "\"event\"" matched so far
  bool afterKey;               // the key is through: a ':' (after whitespace) counts it
  int count;
};
void wxCountInit(WxEventCounter* c);
void wxCountFeed(WxEventCounter* c, const char* p, size_t n);

/* ISO 8601 with an offset ("2026-10-03T16:18:00-05:00", "...+00:00", "...Z", fractional
 * seconds allowed) -> UTC epoch. False for anything else (a local time with no offset is NOT
 * guessed). */
bool wxParseIso(const char* s, int64_t* utc);
/* An HTTP Date header ("Sat, 03 Oct 2026 22:40:22 GMT") -> UTC epoch. */
bool wxParseHttpDate(const char* s, int64_t* utc);

/* "Clear", "Partly cloudy", "Lt rain", "T-storm+hail"... (the short WMO table, the same words
 * on COVEY); an unknown code is "Code 42" in `buf`. Returns a pointer to the words. */
const char* wxCodeText(int code, char* buf, size_t cap);
bool wxCodeIsSnow(int code);   // 71-77, 85-86: "snow", not "rain", in the summary

// ── the cache: one canonical record (SI), /wx/weather.txt ────────────────────────────────
/* The forecast and the alerts are kept APART, each with its own as-of: a fetch whose alerts
 * failed keeps the old alerts (and says "Alerts not checked"), and one whose forecast failed -
 * a captive portal's HTML 200 included - keeps the old forecast. */
enum { WX_HOST_NEVER = 0, WX_HOST_OK, WX_HOST_FAILED };
struct WxData {
  bool       have;             // a forecast is held
  int32_t    latE2, lonE2;     // the point it was ASKED for (0.01 deg)
  int        placeKind;        // ALM_PLACE_* it was fetched for...
  char       placeName[WX_PLACE_MAX];   // ...and its name ("GPS", "map view", "me", "Camp")
  int64_t    fetchedUtc;       // the forecast's fetch time (UTC; 0 = unknown)
  bool       trusted;          // ...from a trusted clock (else the HTTP Date header, else NWS `updated`)
  int32_t    elevM;            // the &elevation= sent, WX_NO_ELEV = none
  WxForecast fc;
  WxAlerts   al;               // the last alerts ANSWER (a failed attempt never replaces it)
  int64_t    alertsUtc;        // ...its as-of
  bool       alertsTrusted;
  int32_t    alLatE2, alLonE2; // ...and the point IT was asked for: a fetch whose forecast failed
                               // brings this place's alerts beside the old place's forecast
  bool       alPointKnown;     // ...known (false: a file from before it, with no forecast either)
  int        omLast;           // WX_HOST_*: the last attempt at each host
  int        nwsLast;          // WX_HOST_OK means al.state is this attempt's answer
};
void   wxDataClear(WxData* d);
/* The card form: "wiphone-wx 1" ... "end", one line per fact, NaN as "-". Same length rule.
 * The alerts' own point rides at the END of the "alerts" line: a file from before it (no point
 * there) loads with the alerts at the forecast's point, and an older loader ignores the two. */
size_t wxCacheSave(const WxData* d, char* out, size_t cap);
/* A file that is not exactly that - another version, no "end", a bad line - loads as EMPTY
 * (false, `out` cleared), never half a forecast. */
bool   wxCacheLoad(const char* text, size_t len, WxData* out);

/* What one fetch brought back (the worker's result, folded on the loop). */
struct WxFetch {
  int32_t    latE2, lonE2;
  int        placeKind;
  char       placeName[WX_PLACE_MAX];
  int32_t    elevM;
  int64_t    askedUtc;         // the TRUSTED clock when it was asked; 0 = not trusted
  int64_t    dateUtc;          // the HTTP Date header of either answer; 0 = none
  bool       omParsed;         // the Open-Meteo body parsed: the forecast is replaced
  WxForecast fc;
  bool       nwsTried;
  WxAlerts   al;               // al.state WX_AL_FAILED = not checked: the old alerts stay
};
/* Fold a fetch into the cache. The stamp: the trusted clock at the ask, else the HTTP Date header,
 * else NWS's `updated` (never Open-Meteo's current.time, a 15-minute model slot). The forecast is
 * replaced only when it parsed; the alerts only when they answered, with the fetch's point as
 * THEIR point (the forecast may stay another place's) - except that alerts kept from a place over
 * WX_MOVED_KM from a new forecast are dropped (they are someone else's). Returns true when
 * anything changed (the caller saves). */
bool wxFold(WxData* d, const WxFetch* f);

// ── when to fetch ────────────────────────────────────────────────────────────────────────
enum {
  WX_DUE_YES = 0,
  WX_DUE_NO_WIFI,              // no WiFi station link
  WX_DUE_HOTSPOT,              // the phone is running its own hotspot
  WX_DUE_NO_PLACE,             // the Almanac knows no place
  WX_DUE_BUSY,                 // a weather fetch is already out
  WX_DUE_FRESH,                // automatic only: the forecast is under WX_STALE_S old and near
  WX_DUE_RETRY_WAIT,           // automatic only: the last attempt FAILED under WX_RETRY_MS ago
  WX_DUE_MUSIC,                // automatic only: a track is playing (Refresh pauses it instead)
};
struct WxDueIn {
  bool     explicitAsk;        // the Refresh soft key (or `wx fetch`); false = on opening
  bool     wifiUp, hotspot, placeOk, busy, musicPlaying;
  bool     haveCache;          // a forecast is held
  int64_t  cacheAgeS;          // its age, -1 = unknown (no trusted clock and not this boot)
  double   movedKm;            // the place from where it was fetched, -1 = unknown
  bool     attempted;          // an attempt this boot...
  bool     lastFailed;         // ...that failed (the forecast host, or both)...
  uint32_t sinceAttemptMs;     // ...this long ago (stamped BEFORE the request)
};
/* An explicit ask needs only WiFi (not the phone's own hotspot), a place and no fetch out; an
 * automatic one also a stale or far forecast, no failure in the last WX_RETRY_MS and no music. */
int  wxFetchDue(const WxDueIn* in);
const char* wxDueWhy(int due);     // one short sentence for the screen ("No WiFi - join a network first")
double wxDistanceKm(double lat1, double lon1, double lat2, double lon2);
/* Alerts are held (an answer kept) at a point over WX_MOVED_KM from the forecast's, or with no
 * forecast: they need their own "N km away", not the forecast's. */
bool   wxAlertsApart(const WxData* d);

#endif // WEATHER_H
