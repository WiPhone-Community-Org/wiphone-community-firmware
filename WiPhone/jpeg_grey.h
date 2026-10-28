/*
 * jpeg_grey.h — a baseline GREYSCALE JPEG decoder, because the ESP32's does not have one.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * WHY THIS EXISTS
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * The JPEG decoder this firmware uses is TJpgDec R0.01c, in the ESP32's ROM (`rom/tjpgd.h`).
 * It decodes 3-component YCbCr only. Measured on the phone, not inferred:
 *
 *     GREYPROBE OEBPS/image/image-0-5.jpg: 199x176 comps=1 -> load_jpg_at=0 (drew 0x0)
 *
 * That would be a curiosity if greyscale were rare. It is not: **33 of the 45 pictures in the
 * book this was written against are 1-component JPEGs** — every chapter ornament. Being in
 * ROM, that decoder cannot be patched, and the alternative to this file is vendoring a newer
 * TJpgDec and renaming its symbols to avoid colliding with the ones already in silicon.
 *
 * Greyscale is the easy case, which is what makes writing it reasonable: one component means
 * no chroma, no upsampling and no interleaving, so an MCU is a single 8x8 block in raster
 * order. What is left is Huffman, dequantise, IDCT.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * WHAT IT DELIBERATELY DOES NOT DO
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * Baseline sequential, one component, 1x1 sampling, 8-bit. Anything else — progressive,
 * arithmetic coding, 12-bit, subsampled single components — returns an error so the caller
 * can fall back and SAY so, rather than drawing something wrong. Colour JPEGs are not handled
 * here at all: the ROM decoder already does those well.
 *
 * Rows are streamed to a callback eight at a time rather than composed into a framebuffer,
 * because the cover of that book is 1453x1920 and a full one would be 5 MB.
 *
 * Correctness is checked against macOS `sips` as a reference decoder — see tests/test_jpeg.cpp,
 * which compares every pixel of real images out of the real book.
 */
#ifndef JPEG_GREY_H
#define JPEG_GREY_H

#include <stddef.h>
#include <stdint.h>

enum JpegGreyStatus {
  JPEG_GREY_OK = 0,
  JPEG_GREY_ERR_NOT_JPEG,     // no SOI, or truncated before anything useful
  JPEG_GREY_ERR_UNSUPPORTED,  // progressive, arithmetic, 12-bit, or not 1x1 greyscale
  JPEG_GREY_ERR_BAD_DATA,     // malformed tables or entropy stream
  JPEG_GREY_ERR_MEMORY,
};

/* Called with each finished band, top to bottom. `band` is `rows` scanlines of `width` bytes,
 * one byte per pixel, 0 = black. The last band may be shorter than 8 rows. */
typedef void (*JpegGreyRowsFn)(void* ctx, int y0, int rows, const uint8_t* band, int width);

/* Decode `data`. `wOut`/`hOut` are filled from the frame header even when decoding then
 * fails, so a caller can report the size of something it could not draw. */
JpegGreyStatus jpegGreyDecode(const uint8_t* data, size_t len,
                              JpegGreyRowsFn cb, void* ctx,
                              uint16_t* wOut, uint16_t* hOut);

// True if this looks like a JPEG this decoder is willing to try: baseline, one component.
bool jpegGreyIsGreyBaseline(const uint8_t* data, size_t len);

const char* jpegGreyStatusText(JpegGreyStatus s);

#endif // JPEG_GREY_H
