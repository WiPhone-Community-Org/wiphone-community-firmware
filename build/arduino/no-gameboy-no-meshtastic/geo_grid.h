/*
 * geo_grid.h - grid references for a lat/lon: UTM, MGRS, and degrees +
 * decimal minutes. Part of the Almanac (docs/almanac.md, "Grid references").
 *
 * UTM is WGS84 transverse Mercator by the Kruger series in Karney's form
 * (Karney 2011, "Transverse Mercator with an accuracy of a few nanometers"),
 * truncated at n^4: the first omitted term is ~1e-7 m, far below the 1 mm the
 * tests hold it to. The zone rule includes the Norway (32V) and Svalbard
 * (31X/33X/35X/37X) exceptions; the band is C..X without I and O, X spanning
 * 72..84 N. Valid for 80 S <= lat < 84 N; outside that the poles need UPS,
 * which is NOT implemented, so every call refuses (returns false).
 *
 * MGRS is derived from UTM by the 100 km square letters (the "AA" lettering
 * of WGS84) with the digits TRUNCATED to the metre - MGRS never rounds, a
 * grid reference names the square the point is IN.
 *
 * Pure: no Arduino/ESP-IDF headers, no heap, no static mutable state, C++11.
 * tests/test_geogrid.cpp proves it against the `utm` and `mgrs` packages
 * (tests/vectors_almanac.h, ALM_GRID). COVEY's covey_ui/almanac.py ports this
 * file line for line.
 */
#ifndef GEO_GRID_H
#define GEO_GRID_H

#include <stddef.h>

struct GeoUtm {
  int zone;       // 1..60
  char band;      // 'C'..'X' (no 'I', no 'O')
  bool north;     // lat >= 0 (the northing has no 10,000 km false northing)
  double e, n;    // easting, northing in metres
};

/* lat/lon in degrees, north and east positive; lon may be any finite value
 * (it is wrapped to -180..180, and 180 E is zone 1's -180). False, with *out
 * untouched, outside 80 S <= lat < 84 N or for a non-finite input. */
bool geoToUtm(double lat, double lon, GeoUtm* out);

/* MGRS at 1 m, e.g. "10T ET 91134 61042": zone without a leading zero, band,
 * space, column + row letters, space, 5-digit easting, space, 5-digit northing
 * (both truncated). Needs cap >= 20. False (and out = "" when cap > 0) when
 * outside the UTM range or cap < 20. */
bool geoToMgrs(double lat, double lon, char* out, size_t cap);

/* Degrees and decimal minutes, e.g. "47 29.786N" / "121 47.208W" - no degree
 * glyph (the phone's font may lack one; the caller adds it if it can). The
 * minutes are rounded to 3 decimals with the carry taken into the degrees
 * (59.9996' -> the next degree, 00.000). Degrees carry no leading zero.
 * Hemisphere: N/S for isLat, else E/W; a value that ROUNDS to zero is N or E
 * (never "0 00.000S"). A non-finite value or |deg| > 360 writes "--".
 * The longest output is "180 00.000W" (11 chars + NUL): cap 12 always fits.
 * A cap too small for the whole string writes "" - never a cut-off number
 * ("121 4" would read as 121 deg 4'). */
void geoFmtDdm(double deg, bool isLat, char* out, size_t cap);

#endif // GEO_GRID_H
