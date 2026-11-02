/*
 * tile_png.h — the smallest PNG decoder that covers map tiles, on top of the inflater the
 * firmware already has.
 *
 * ── WHY IN-REPO AND NOT A LIBRARY ────────────────────────────────────────────────────────
 * The sources are known: OpenTopoMap serves 256x256 8-bit PALETTE PNGs (measured
 * 2026-09-18), and USGS's "MIXED" services can hand back an RGBA PNG at a coverage edge.
 * That is: a chunk walk, one zlib stream, five row filters, and a colour-type switch —
 * about three hundred lines whose behaviour can be pinned on the host against Pillow.
 * PNGdec / pngle / lodepng each bring a second inflater, an allocator to redirect into PSRAM
 * and a licence; the EPUB parser already proved the ROM `tinfl` here (epub_parse.cpp:99-113,
 * raw deflate into a whole PSRAM output buffer, ~11 KB of state).
 *
 * ── WHAT IS SUPPORTED ────────────────────────────────────────────────────────────────────
 *   colour types 0 (grey), 2 (RGB), 3 (palette), 4 (grey+alpha), 6 (RGBA); bit depth 8 for
 *   all, plus 1/2/4 for palette and grey; non-interlaced; exactly 256x256. A tRNS chunk on a
 *   palette image is honoured the same way alpha is: a pixel more transparent than half
 *   becomes `nodata` (the caller's "no tile here" colour), so a source's transparent edges
 *   look like the edge of the map rather than like black ground.
 * ── WHAT IS REFUSED, WITH A REASON ──────────────────────────────────────────────────────
 *   16-bit samples, interlacing, any other size, a zlib stream with a preset dictionary, a
 *   truncated or over-long IDAT, a bad filter byte. Every refusal fills `why`.
 *
 * Memory is the caller's problem only for `out`; the decoder allocates its own scratch
 * (the concatenated IDAT, the inflated rows, the inflater state) through tilePngAlloc —
 * PSRAM on the device, malloc on the host — and frees it before returning.
 */
#ifndef TILE_PNG_H
#define TILE_PNG_H

#include <stdint.h>
#include <stddef.h>

/* The "no data" colour transparent pixels become. Mid grey: the viewer's own "not on the
 * card" square is grey too, and transparent ground should read as absent, not as terrain. */
#define TILE_PNG_NODATA_565  0x8410

/* `blank` (may be NULL) is set true when EVERY pixel was transparent — alpha under 128, or the
 * tRNS colour/entry — counted while unpacking, never read back from the output: an opaque
 * tile whose ground happens to be mid grey (0x8410, the nodata colour) is a real tile.
 * OpenTopoMap answers its z18 with exactly such an all-transparent 200 (measured 2026-09-23):
 * the downloader calls that "no tile" and writes nothing. */
bool tilePngDecode(const uint8_t* data, size_t len, uint16_t* out, uint16_t nodata,
                   char* why, size_t whyCap, bool* blank = NULL);

/* ── THE SAME DECODE, AS RGB ROWS (0.9.80, the elevation download) ─────────────────────────
 * For a caller that needs the samples, not a 565: a terrarium elevation tile is metres packed
 * as R*256 + G + B/256, and a 565 keeps five bits of R. Same formats, same refusals, same
 * `blank` rule as tilePngDecode (tests/test_tilepng.cpp proves the two agree pixel for pixel).
 * `row` is called once per image row, top-down, y = 0..255, with 256 pixels of R,G,B (768
 * bytes) and `clear`: NULL when no pixel of that row is transparent, else 256 flags (1 = the
 * pixel is transparent by the 565 path's rule). Both pointers are good only for that call.
 * ⚠ On a false return some rows may already have been delivered: discard what they wrote.
 * An 8-bit RGB PNG without tRNS (every terrarium tile) is handed over with no copy; any other
 * type costs one 1 KB scratch row per decode (PSRAM on the phone, like the rest of the scratch). */
typedef void (*TilePngRowFn)(void* ctx, int y, const uint8_t* rgb, const uint8_t* clear);
bool tilePngDecodeRgb(const uint8_t* data, size_t len, TilePngRowFn row, void* ctx,
                      char* why, size_t whyCap, bool* blank = NULL);

#endif // TILE_PNG_H
