/*
 * weather_lines.h - what the Almanac's weather SAYS (0.9.81): the WEATHER screen's rows, and the
 * two rows TODAY carries (an active alert near the top, a one-line summary over the entries).
 * The same AlmEmitFn and row kinds as almanac_lines.h, so the app's menu, the serial `wx show`
 * and the host test all get the SAME rows; tests/test_weather.cpp measures every one against the
 * phone's Akrobat Bold 20 (232 px a row, a wrapped sentence whole into rows of 220 px) - with all
 * 111 NWS event names among the alerts.
 *
 * THE SCREEN, top to bottom (decided 2026-10-03):
 *   alerts first - each active one in its colour (ALM_ROW_DANGER red for Severe/Extreme,
 *     ALM_ROW_WARN yellow otherwise): "Flood Warning until Sat 18:00"; never a bare "No alerts":
 *     "No alerts at 07:10" / "Alerts: US only (NWS)" / "Alerts not checked (no answer)" (the
 *     last answer's alerts kept, with their own as-of) / "Alerts: 9+ (too many to list)";
 *   now - Open-Meteo's `current` ONLY within an hour of the fetch ("Now: 17C, Mostly clear");
 *     after that the hourly entry covering the phone's now, labelled "Forecast for 14:00"
 *     (model data is never shown as an observation);
 *   the next hours (past ones dropped; every 3 h, a day of them), then the days (each labelled
 *     by the date of its MIDPOINT, t + 43200, in the phone's offset - daily.time is the
 *     LOCATION's midnight, and a phone an hour behind it would otherwise show yesterday's high);
 *     past the last day, one row: "No forecast after Thu Oct 9 - refresh on Wi-Fi";
 *   "As of 07:10 (3h 05m ago) for GPS" - an age only on a trusted clock (NTP/GPS), else the
 *     stored date and time; the place source named (phone 1 has no GPS: its place may be the
 *     map's view); a warning when Open-Meteo's offset for the place is not the phone's
 *     ("Local time here is UTC-7 - check the clock setting" - nothing is ever set from it);
 *   the credit: "Weather: Open-Meteo.com" "(CC BY 4.0); alerts: NWS".
 * Wind is meteorological FROM: "Wind from NW 14 km/h, gusts 30". Temperatures carry no degree
 * glyph ("52F": the Akrobat faces have none). Units: the Settings row (US: F, mph, inHg, in).
 *
 * Pure: no Arduino/ESP-IDF headers, no heap, no static mutable state.
 */
#ifndef WEATHER_LINES_H
#define WEATHER_LINES_H

#include "almanac_lines.h"
#include "weather.h"

/* What the screens are shown (AlmCtx.wx). The app fills it on every build. */
struct WxView {
  const WxData* d;             // the cache in memory; NULL = not read from the card yet
  bool          unreadable;    // ...and the card did not answer when asked (d is NULL)
  bool          fetching;      // a fetch is queued or out
  bool          clockTrusted;  // NTP/GPS: the as-of row gives an age only then
  const char*   note;          // why the last ask was refused or failed - one sentence, "" = none
};

void almLinesWeather(const AlmCtx* c, AlmEmitFn emit, void* ctx);
/* TODAY's rows (c->dayOffset == 0, c->wx set; nothing otherwise). */
void wxTodayAlertRow(const AlmCtx* c, AlmEmitFn emit, void* ctx);
void wxTodaySummaryRow(const AlmCtx* c, AlmEmitFn emit, void* ctx);

/* The pieces, public for the tests. */
/* "18:00" (the same local day as now), "Sat 18:00" (within six days), "Oct 12 18:00". */
void wxFmtWhen(int64_t t, int64_t now, int tzS, char* out, size_t cap);
/* The alert's row: "Flood Warning until Sat 18:00", "Winter Storm Watch from Sat 04:00 to Sun
 * 10:00", "Special Weather Statement" (no end given). */
void wxAlertText(const WxAlert* a, int64_t now, int tzS, char* out, size_t cap);
/* Still in force at `now`: before `ends`, else before `expires` (an alert with neither is). */
bool wxAlertActive(const WxAlert* a, int64_t now);
/* The hourly entry covering t (t in [hour.t, hour.t + 3600)), -1 = none. */
int  wxHourAt(const WxForecast* f, int64_t t);
/* The local date a daily entry stands for: the date of t + 43200 in the phone's offset. */
void wxDayDate(int64_t t, int tzS, int* y, int* m, int* d, int* wday);

#endif // WEATHER_LINES_H
