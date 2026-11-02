/*
 * elev_sd.h - the ground's height under a point, read from the elevation tiles on the SD card.
 * The device half of elev_tiles.h: an ElevReadFn over the card's POSIX mount, and the two calls
 * the map and the console make. docs/almanac.md, "Elevation"; docs/maps.md, "Altitude".
 *
 * Files: /maps/elev/<z>/<x>/<y>.elv (ELEV_DIR under MAPS_ROOT), 131072 bytes each. A file of any
 * other length is not a tile and reads as "no tile" (the map's own rule for a .565: the length
 * is the whole format check).
 *
 * ── WHAT ONE SAMPLE COSTS, AND WHERE IT MAY RUN ─────────────────────────────────────────────
 * elevSample() tries z13 and, when that has no tile or no data at the point, z10: at most two
 * tiles, and in each ONE open (a path walk of five folders), one fstat, and one or two 4-byte
 * reads at a seek (one per pixel row). The file is opened once per tile per call and CLOSED
 * before this returns - no descriptor outlives the call, so a card pulled between samples, or
 * the app torn down, has nothing to trip over. Not measured on the phone yet; estimated from
 * the tile loader's numbers (a 32 KB piece is 25-60 ms, most of it the transfer) at a few ms
 * per open + read, so ~5-20 ms a layer and ~40 ms at worst for both. `elev <lat> <lon>` on the
 * serial console prints the real figure. 🛑 IT IS CARD I/O: never from a draw, never from a
 * key handler - the map calls it from its app timer, one sample per tick (app_maps.cpp).
 * ⚠ The FATFS here keeps its long-file-name buffer ON THE STACK (CONFIG_FATFS_LFN_STACK, 512 B):
 * call it at the depth the tile loader already reads the card from, not deeper.
 */
#ifndef ELEV_SD_H
#define ELEV_SD_H

#include "elev_tiles.h"

/* The ground at a point, through elevSample(): 1 = ok (*metres, *zUsed = ELEV_Z or
 * ELEV_Z_COARSE), -1 = a tile is there but has no data at that point, 0 = no tile. *ioErr (may
 * be NULL) is set when a tile could not be READ - the card answered with something other than
 * "no such file" (pulled, busy, out of file handles), or a read came back short - so the caller
 * can tell "not on the card" from "the card had a moment" and ask again later. */
int  elevSampleCard(double lat, double lon, double* metres, int* zUsed, bool* ioErr = NULL);

/* Is there an elevation layer on the card at all: /maps/elev is a folder. One stat; the caller
 * caches it (the map at its card scan, alongside the areas). */
bool elevCardPresent();

#endif // ELEV_SD_H
