/*
 * tile_decode.cpp — see tile_decode.h.
 */
#include "tile_decode.h"
#include "tile_png.h"
#include "elev_tiles.h"

#include <string.h>
#include <stdio.h>

#if defined(ARDUINO)
  #include <Arduino.h>
  #include <esp_heap_caps.h>
  extern "C" {
    #include "rom/tjpgd.h"
  }
#endif

TileFormat tileSniff(const uint8_t* d, size_t n) {
  if (!d || n < 8) {
    return TILE_FMT_UNKNOWN;
  }
  if (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
    return TILE_FMT_JPEG;
  }
  static const uint8_t PNG_SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  if (memcmp(d, PNG_SIG, 8) == 0) {
    return TILE_FMT_PNG;
  }
  size_t i = 0;
  while (i < n && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n')) {
    i++;
  }
  if (i < n && d[i] == '<') {
    return TILE_FMT_HTML;
  }
  return TILE_FMT_UNKNOWN;
}

#if defined(ARDUINO)

/* ── JPEG through the ROM decoder, with private state ────────────────────────────────────
 * TJpgDec hands the input callback a request for n bytes (or a skip when buf is NULL) and
 * the output callback one MCU-sized rectangle of packed RGB888 at a time. Both get the JDEC,
 * and the JDEC carries the `device` pointer jd_prepare was given — that is how this stays
 * re-entrant with respect to display::load_jpg_at's globals. */
struct JpegCtx {
  const uint8_t* data;
  size_t         len, pos;
  uint16_t*      out;
  bool           overflow;
};

static UINT jpegIn(JDEC* jd, BYTE* buf, UINT n) {
  JpegCtx* c = (JpegCtx*)jd->device;
  UINT avail = (c->pos + n <= c->len) ? n : (UINT)(c->len - c->pos);
  if (buf) {
    memcpy(buf, c->data + c->pos, avail);
  }
  c->pos += avail;
  return avail;
}

static UINT jpegOut(JDEC* jd, void* bitmap, JRECT* r) {
  JpegCtx* c = (JpegCtx*)jd->device;
  const uint8_t* b = (const uint8_t*)bitmap;
  for (int y = r->top; y <= r->bottom; y++) {
    if (y >= TILE_DECODE_PX) {
      c->overflow = true;
      return 0;                        // abort: the image is taller than it said
    }
    uint16_t* row = c->out + (size_t)y * TILE_DECODE_PX;
    for (int x = r->left; x <= r->right; x++, b += 3) {
      if (x >= TILE_DECODE_PX) {
        c->overflow = true;
        return 0;
      }
      row[x] = tileColor565(b[0], b[1], b[2]);
    }
  }
  return 1;
}

static const char* jpegErr(JRESULT rc) {
  switch (rc) {
  case JDR_INTR: return "decode interrupted";
  case JDR_INP:  return "input ended early";
  case JDR_MEM1: return "work pool too small";
  case JDR_MEM2: return "stream buffer too small";
  case JDR_PAR:  return "bad parameter";
  case JDR_FMT1: return "damaged JPEG";
  case JDR_FMT2: return "unsupported JPEG";
  case JDR_FMT3: return "greyscale or progressive JPEG";
  default:       return "JPEG error";
  }
}

/* 4096 is what display::load_jpg_at has decoded 4032x3024 photos with; a 256x256 tile with
 * full Huffman tables needs less. PSRAM, per call: the pool is a work area the ROM code
 * merely reads and writes, and this chip (rev 3) has no PSRAM-cache bug for it to trip. */
#define JPEG_POOL_BYTES 4096

static bool decodeJpeg(const uint8_t* data, size_t len, uint16_t* out, char* why, size_t whyCap) {
  void* pool = heap_caps_malloc(JPEG_POOL_BYTES, MALLOC_CAP_SPIRAM);
  if (!pool) {
    snprintf(why, whyCap, "no PSRAM for the JPEG pool");
    return false;
  }
  JpegCtx ctx;
  ctx.data = data;
  ctx.len = len;
  ctx.pos = 0;
  ctx.out = out;
  ctx.overflow = false;
  JDEC jd;
  JRESULT rc = jd_prepare(&jd, jpegIn, pool, JPEG_POOL_BYTES, &ctx);
  if (rc != JDR_OK) {
    snprintf(why, whyCap, "%s", jpegErr(rc));
    free(pool);
    return false;
  }
  if (jd.width != TILE_DECODE_PX || jd.height != TILE_DECODE_PX) {
    snprintf(why, whyCap, "JPEG is %ux%u, not 256x256", (unsigned)jd.width, (unsigned)jd.height);
    free(pool);
    return false;
  }
  rc = jd_decomp(&jd, jpegOut, 0);
  free(pool);
  if (rc != JDR_OK) {
    snprintf(why, whyCap, "%s", ctx.overflow ? "JPEG larger than its header said" : jpegErr(rc));
    return false;
  }
  return true;
}

#endif // ARDUINO

TileDecodeResult tileDecode(const uint8_t* data, size_t len, uint16_t* out, char* why, size_t whyCap) {
  if (!why || whyCap == 0) {
    return TILE_DECODE_ERROR;
  }
  why[0] = '\0';
  if (!data || !out || len < 8) {
    snprintf(why, whyCap, "empty body");
    return TILE_DECODE_ERROR;
  }
  switch (tileSniff(data, len)) {
  case TILE_FMT_JPEG:
#if defined(ARDUINO)
    return decodeJpeg(data, len, out, why, whyCap) ? TILE_DECODE_OK : TILE_DECODE_ERROR;   // a JPEG has no alpha
#else
    snprintf(why, whyCap, "JPEG decode is device-only (ROM TJpgDec)");
    return TILE_DECODE_ERROR;
#endif
  case TILE_FMT_PNG: {
    bool blank = false;
    if (!tilePngDecode(data, len, out, TILE_PNG_NODATA_565, why, whyCap, &blank)) {
      return TILE_DECODE_ERROR;
    }
    return blank ? TILE_DECODE_BLANK : TILE_DECODE_OK;
  }
  case TILE_FMT_HTML:
    snprintf(why, whyCap, "the server sent a page, not a tile");
    return TILE_DECODE_ERROR;
  default:
    snprintf(why, whyCap, "not a JPEG or PNG");
    return TILE_DECODE_ERROR;
  }
}

/* One row of a terrarium tile into the .elv bytes (tileDecodeElev). */
static void elevRow(void* ctx, int y, const uint8_t* rgb, const uint8_t* clear) {
  uint8_t* o = (uint8_t*)ctx + (size_t)y * (ELEV_TILE_PX * 2);
  for (int x = 0; x < ELEV_TILE_PX; x++, rgb += 3) {
    const int16_t m = (clear && clear[x]) ? (int16_t)ELEV_NODATA : elevFromTerrarium(rgb[0], rgb[1], rgb[2]);
    const uint16_t u = (uint16_t)m;
    o[x * 2]     = (uint8_t)(u & 0xFF);          // little-endian, whatever the host is
    o[x * 2 + 1] = (uint8_t)(u >> 8);
  }
}

TileDecodeResult tileDecodeElev(const uint8_t* data, size_t len, uint8_t* out, char* why, size_t whyCap) {
  if (!why || whyCap == 0) {
    return TILE_DECODE_ERROR;
  }
  why[0] = '\0';
  if (!data || !out || len < 8) {
    snprintf(why, whyCap, "empty body");
    return TILE_DECODE_ERROR;
  }
  switch (tileSniff(data, len)) {
  case TILE_FMT_PNG: {
    bool blank = false;
    if (!tilePngDecodeRgb(data, len, elevRow, out, why, whyCap, &blank)) {
      return TILE_DECODE_ERROR;
    }
    return blank ? TILE_DECODE_BLANK : TILE_DECODE_OK;
  }
  case TILE_FMT_HTML:
    snprintf(why, whyCap, "the server sent a page, not a tile");
    return TILE_DECODE_ERROR;
  default:
    snprintf(why, whyCap, "not a terrarium PNG");
    return TILE_DECODE_ERROR;
  }
}
