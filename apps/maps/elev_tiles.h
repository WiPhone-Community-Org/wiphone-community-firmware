/*
 * elev_tiles.h - the ground's height under a lat/lon, from elevation tiles on the card.
 * Part of the Almanac (docs/almanac.md, "Elevation"): the map's altitude readout under the
 * crosshair, and "that ridge is +120 m above you".
 *
 * ── THE FILES ────────────────────────────────────────────────────────────────────────────
 * `<maps root>/elev/<z>/<x>/<y>.elv`: 256 x 256 LITTLE-ENDIAN int16 metres, row-major (row 0
 * is the tile's north edge, column 0 its west edge), 131072 bytes, on the same web-mercator
 * z/x/y grid as the map tiles. Built from the AWS Terrain Tiles ("terrarium" PNG) by
 * tools/make_elev_tiles.py on the Mac, or by the phone's downloader through
 * elevFromTerrarium() below - both round the same way, so a tile made on either is the same
 * bytes. ELEV_NODATA (-32768) marks a pixel with no data; a real height never decodes to it.
 * Two layers: z13 (~13 m pixels at 47 N) and a coarse z10 (~100 m) under the zoomed-out map.
 *
 * ── HOW A POINT IS READ ──────────────────────────────────────────────────────────────────
 * Bilinear between pixel CENTRES: pixel i covers [i, i+1) and its centre is i + 0.5. In a
 * tile's outer half-pixel on any side (px < 0.5 or px >= 255.5, same for py) the edge pixel is
 * used along that axis - a sample NEVER opens the neighbouring tile, so a point costs one tile
 * and at most two 4-byte reads. A no-data neighbour is dropped and the others' weights
 * renormalised. None left, or none carrying any weight (the point sits exactly on a line of
 * no-data pixel centres), is "no data at that point".
 *
 * The card is reached only through ElevReadFn, so this file stays pure: no Arduino, no
 * ESP-IDF, no heap, no static mutable state, C++11. tests/test_elev.cpp proves it on the Mac
 * against the map's own projection (map_tiles.cpp) and synthetic tiles held in memory.
 * COVEY's covey_ui/elevation.py ports it line for line.
 */
#ifndef ELEV_TILES_H
#define ELEV_TILES_H

#include <stddef.h>
#include <stdint.h>

#define ELEV_Z           13          // the fine layer, tried first
#define ELEV_Z_COARSE    10          // the coarse layer, tried when the fine one has nothing
#define ELEV_ZOOM_MAX    19          // = MAP_ZOOM_MAX; a z outside [0, 19] is never read
#define ELEV_DIR         "elev"      // the reserved folder under /maps (never a map area)
#define ELEV_TILE_PX     256
#define ELEV_TILE_BYTES  131072      // ELEV_TILE_PX * ELEV_TILE_PX * 2
#define ELEV_NODATA      (-32768)

/* "<mapsRoot>/elev/<z>/<x>/<y>.elv". Returns the length written, or 0 with out[0] = '\0' when
 * it would not fit, or for a NULL root, z outside [0, ELEV_ZOOM_MAX], or x/y outside
 * [0, 2^z). Never truncates: a truncated path is a path to a DIFFERENT file (mapTilePath's
 * contract). */
int  elevTilePath(char* out, size_t cap, const char* mapsRoot, int z, int x, int y);

/* The tile a point is in at zoom z, and where inside it: 0 <= *px, *py < 256 (fractional
 * pixels from the tile's west/north edge). The same projection as mapLatLonToWorld():
 * longitude wrapped into [-180, 180) (so 180 E is tile column 0), latitude clamped to the
 * mercator limit (+-85.0511287798066; the south limit lands in the last row, py just under
 * 256; +-inf is that limit), NaN read as 0, an infinite longitude read as 0 too (mapWrapLon
 * would give NaN). z is clamped into [0, ELEV_ZOOM_MAX]. Any output pointer may be NULL. */
void elevLocate(double lat, double lon, int z, int* tx, int* ty, double* px, double* py);

/* Read n bytes at byte `offset` of tile z/x/y into buf. Returns the bytes read: n on success,
 * anything else (fewer, or -1) when the tile or those bytes are missing. A caller-supplied
 * reader should refuse a file that is not exactly ELEV_TILE_BYTES long. elevSample asks for
 * n = 4 only, at offsets inside the tile. */
typedef int (*ElevReadFn)(void* ctx, int z, int x, int y, uint32_t offset, uint8_t* buf, int n);

/* The ground at a point, from ONE layer z:
 *    1 = ok, *metres written;
 *    0 = no tile there (or a read failed, or rd is NULL, lat/lon not finite, z out of range);
 *   -1 = the tile is there but has no data at that point.
 * *metres is written only on 1 (it may be NULL). */
int  elevSampleZ(ElevReadFn rd, void* ctx, int z, double lat, double lon, double* metres);

/* The ground at a point: ELEV_Z first, then ELEV_Z_COARSE when z13 has no tile OR no data
 * there. Returns 1 (and *zUsed = the layer the value came from), else -1 if either layer had a
 * tile, else 0; *zUsed = -1 when nothing was found. metres and zUsed may be NULL. */
int  elevSample(ElevReadFn rd, void* ctx, double lat, double lon, double* metres,
                int* zUsed = NULL);

/* One terrarium PNG pixel as a tile value: m = r*256 + g + b/256 - 32768, rounded to the
 * metre HALF AWAY FROM ZERO (exactly as tools/make_elev_tiles.py does, so the phone's own
 * download and the Mac's build give the same bytes), clamped to [-32767, 32767]. -32768 is
 * ELEV_NODATA and is never returned: (0,0,0), which is -32768 m, becomes -32767. Exact
 * integer arithmetic, no floating point. */
int16_t elevFromTerrarium(uint8_t r, uint8_t g, uint8_t b);

#endif // ELEV_TILES_H
