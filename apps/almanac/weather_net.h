/*
 * weather_net.h - the device half of the Almanac's weather (0.9.81): the cache on the card, the
 * gates, the fetch policy's device facts, and the WX job on the shared HTTPS worker
 * (https_worker.h). The decisions are weather.h's (host-tested); the rows weather_lines.h's; the
 * screen is the Almanac's WEATHER (app_almanac.cpp); the bench is serial `wx`.
 *
 * ONE FETCH = TWO GETS on the worker, each on its own fresh WiFiClientSecure, the handshake timed
 * alone (connect first): Open-Meteo, then NWS - both pinned to ISRG Root X1 + the self-signed Root
 * YR (isrg_roots.h; no override file), the internal-heap bar checked again before EACH handshake.
 * The bodies are parsed ON THE WORKER into a small PSRAM result (weather.cpp; the alerts counted by
 * a streaming tap, so a body too big to keep still says how many), and the LOOP folds the result
 * into the cache and writes the card (wxLoopTick, every pass - a flag test when idle, and never
 * while a game owns the SPI bus). So leaving the Almanac mid-fetch is safe: it lands anyway.
 *
 * WHEN: on opening the Almanac (and the WEATHER screen) with WiFi up, once the day's core has
 * landed, when the forecast is over an hour old or was fetched over 5 km from the place - and on
 * the Refresh soft key. An automatic fetch is skipped while music plays and for 10 minutes after
 * a FAILED attempt (stamped BEFORE the request); a Refresh pauses the music, as an AI question
 * does (a handshake beside a decoding track failed on phone 1). Nothing runs in the background.
 *
 * WHAT IS SENT: the place rounded to 0.01 deg (Open-Meteo) and 0.0001 deg (NWS), the ground
 * elevation under it from the phone's own tiles (to 10 m), a User-Agent naming this project.
 * NOTHING HERE LOGS A URL OR A COORDINATE: one outcome line a fetch ("weather: ok 200/200 4.1 s",
 * "weather: failed tls"); `wx` prints the ROUNDED point the cache holds.
 *
 * THE CARD: /wx/weather.txt (+ .tmp, ai_net's write: tmp, remove, rename - FAT cannot rename over
 * a file; the .tmp is read when the .txt is missing). A file this firmware cannot read is set
 * aside as weather.bad, never written over unread.
 */
#ifndef WEATHER_NET_H
#define WEATHER_NET_H

#include <stddef.h>
#include <stdint.h>
#include "weather.h"

#define WX_DIR       "/wx"
#define WX_FILE      "/wx/weather.txt"
#define WX_TMP       "/wx/weather.tmp"
#define WX_BAD       "/wx/weather.bad"

/* What the Almanac asks a fetch for. */
struct WxAsk {
  double      lat, lon;       // the place (rounded by the request)
  int         placeKind;      // ALM_PLACE_*
  const char* placeName;      // "GPS", "map view"...
  bool        haveElev;       // the ground under it, from the elevation tiles
  double      elevM;
};

// ── LOOP TASK ────────────────────────────────────────────────────────────────────────────
/* Read the cache from the card, once a boot (later calls: nothing). CARD I/O: the Almanac's timer,
 * the console and wxLoopTick only - never a key handler. */
void wxCacheLoadCard();
bool wxCacheTried();                  // the card has been asked
bool wxCacheLoaded();                 // ...and answered (a file read, or none there); wxData() is it
const WxData* wxData();               // the cache in memory; NULL until the card answers / no PSRAM
uint32_t wxGen();                     // changes with every fold, clear, load and fetch start
/* The fetch policy with this phone's facts (WiFi, its own hotspot, music, the cache's age and
 * place, the last attempt): weather.h's wxFetchDue. */
int  wxDecide(double lat, double lon, bool placeOk, bool explicitAsk);
/* Start one: the gates (aiAsk's: WiFi, a map download, the uploader, a sync window, a call or the
 * minute after, a game, a Files job, the internal heap), the music paused for an explicit ask,
 * the URLs built, the attempt stamped, the job queued on the worker. False with `why`. */
bool wxRequest(const WxAsk* a, bool explicitAsk, char* why, size_t whyCap);
/* Queued, running, or finished and not folded yet. */
bool wxRequestActive();
/* Every pass: fold a finished fetch into the cache and write the card (a flag test when idle). */
void wxLoopTick();
/* The last attempt's failure, for the screen ("Weather not updated: no internet (DNS)"); "" when the
 * forecast came through. */
const char* wxLastNote();
/* `wx clear`: the cache in memory and its files. */
void wxClear();

/* Serial `wx`: one line per emit, the rounded point only. */
void wxReport(void (*emit)(const char* line));

#endif // WEATHER_NET_H
