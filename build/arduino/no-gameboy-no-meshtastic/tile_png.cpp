/*
 * tile_png.cpp — see tile_png.h.
 *
 * The shape follows the PNG spec's decoding order (ISO/IEC 15948 §9-11): chunk walk, one
 * concatenated zlib stream, per-row unfiltering against the previous row, then sample
 * unpacking. The inflate is the SAME call the EPUB parser makes (epub_parse.cpp
 * rawInflateEx) — raw deflate into a whole output buffer — so the one thing this file does
 * not do is trust the ROM's zlib-header parsing: the two-byte header is checked and stripped
 * here, the four-byte Adler trailer is ignored, and the ROM inflater sees a stream it has
 * already been proven on.
 */
#include "tile_png.h"
#include "tile_decode.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(ARDUINO)
  #include <Arduino.h>
  #include <esp_heap_caps.h>
  #include "rom/miniz.h"
  static void* pngAlloc(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);   // PSRAM or nothing: never raid the internal heap
  }
  static void pngFree(void* p) { free(p); }
#else
  #include <zlib.h>
  static void* pngAlloc(size_t n) { return malloc(n); }
  static void pngFree(void* p) { free(p); }
#endif

static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Raw deflate `src` into `dst`. Returns bytes produced, or -1. Whole-stream semantics:
 * anything short of TINFL_STATUS_DONE / Z_STREAM_END is a failure, because a tile with a
 * missing bottom is not a tile. */
static int rawInflate(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap) {
#if defined(ARDUINO)
  tinfl_decompressor* d = (tinfl_decompressor*)pngAlloc(sizeof(tinfl_decompressor));
  if (!d) {
    return -1;
  }
  tinfl_init(d);
  size_t inLen = srcLen, outLen = dstCap;
  tinfl_status st = tinfl_decompress(d, src, &inLen, dst, dst, &outLen,
                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  pngFree(d);
  return (st == TINFL_STATUS_DONE) ? (int)outLen : -1;
#else
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
    return -1;
  }
  zs.next_in = (Bytef*)src;
  zs.avail_in = (uInt)srcLen;
  zs.next_out = dst;
  zs.avail_out = (uInt)dstCap;
  const int rc = inflate(&zs, Z_FINISH);
  const int produced = (int)zs.total_out;
  inflateEnd(&zs);
  return (rc == Z_STREAM_END) ? produced : -1;
#endif
}

static inline uint8_t paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = p > a ? p - a : a - p;
  const int pb = p > b ? p - b : b - p;
  const int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) {
    return (uint8_t)a;
  }
  return (pb <= pc) ? (uint8_t)b : (uint8_t)c;
}

/* Undo one row's filter in place. `prev` is the already-unfiltered row above (NULL for the
 * first row = all zeros), `bpp` the bytes per complete pixel (1 for sub-byte formats). */
static bool unfilterRow(uint8_t type, uint8_t* row, const uint8_t* prev, size_t n, size_t bpp) {
  switch (type) {
  case 0:
    return true;
  case 1:
    for (size_t i = bpp; i < n; i++) {
      row[i] = (uint8_t)(row[i] + row[i - bpp]);
    }
    return true;
  case 2:
    if (prev) {
      for (size_t i = 0; i < n; i++) {
        row[i] = (uint8_t)(row[i] + prev[i]);
      }
    }
    return true;
  case 3:
    for (size_t i = 0; i < n; i++) {
      const int a = i >= bpp ? row[i - bpp] : 0;
      const int b = prev ? prev[i] : 0;
      row[i] = (uint8_t)(row[i] + ((a + b) >> 1));
    }
    return true;
  case 4:
    for (size_t i = 0; i < n; i++) {
      const int a = i >= bpp ? row[i - bpp] : 0;
      const int b = prev ? prev[i] : 0;
      const int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
      row[i] = (uint8_t)(row[i] + paeth(a, b, c));
    }
    return true;
  default:
    return false;
  }
}

/* What the chunk walk found, for the row stage. `plte`/`trns` point into the caller's data. */
struct PngInfo {
  uint32_t       w, h;
  uint8_t        depth, ctype;
  const uint8_t* plte;
  size_t         plteN;          // palette ENTRIES (3 bytes each)
  const uint8_t* trns;
  size_t         trnsN;          // bytes
  size_t         rowBytes;       // one row's samples, without its filter byte
  size_t         bpp;            // bytes per complete pixel (1 for sub-byte formats)
};

/* Everything up to the rows, shared by both outputs: the signature, the chunk walk, the checks,
 * the IDAT gathered into one stream and inflated. Returns the FILTERED image — h rows of
 * (1 filter byte + rowBytes) — in a buffer from pngAlloc that the caller frees with pngFree;
 * NULL with `why` filled on any refusal. */
static uint8_t* pngInflateImage(const uint8_t* data, size_t len, PngInfo* pi, char* why, size_t whyCap) {
  if (!data || len < 8 + 25 + 12) {
    snprintf(why, whyCap, "PNG too short");
    return NULL;
  }
  static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  if (memcmp(data, SIG, 8) != 0) {
    snprintf(why, whyCap, "not a PNG");
    return NULL;
  }

  // ---- the chunk walk: IHDR, PLTE, tRNS, and the total IDAT length ----
  uint32_t w = 0, h = 0;
  uint8_t  depth = 0, ctype = 0, interlace = 0;
  bool     haveIhdr = false;
  const uint8_t* plte = NULL;
  size_t   plteN = 0;
  const uint8_t* trns = NULL;
  size_t   trnsN = 0;
  size_t   idatTotal = 0;
  size_t   pos = 8;
  while (pos + 12 <= len) {
    const uint32_t clen = be32(data + pos);
    const uint8_t* ctyp = data + pos + 4;
    if (clen > len - pos - 12) {
      snprintf(why, whyCap, "PNG chunk runs past the end");
      return NULL;
    }
    const uint8_t* body = data + pos + 8;
    if (!memcmp(ctyp, "IHDR", 4)) {
      if (clen != 13) {
        snprintf(why, whyCap, "bad IHDR");
        return NULL;
      }
      w = be32(body);
      h = be32(body + 4);
      depth = body[8];
      ctype = body[9];
      interlace = body[12];
      if (body[10] != 0 || body[11] != 0) {
        snprintf(why, whyCap, "unknown PNG compression/filter method");
        return NULL;
      }
      haveIhdr = true;
    } else if (!memcmp(ctyp, "PLTE", 4)) {
      plte = body;
      plteN = clen / 3;
    } else if (!memcmp(ctyp, "tRNS", 4)) {
      trns = body;
      trnsN = clen;
    } else if (!memcmp(ctyp, "IDAT", 4)) {
      idatTotal += clen;
    } else if (!memcmp(ctyp, "IEND", 4)) {
      break;
    }
    pos += 12 + clen;
  }
  if (!haveIhdr) {
    snprintf(why, whyCap, "no IHDR");
    return NULL;
  }
  if (w != TILE_DECODE_PX || h != TILE_DECODE_PX) {
    snprintf(why, whyCap, "PNG is %ux%u, not 256x256", (unsigned)w, (unsigned)h);
    return NULL;
  }
  if (interlace) {
    snprintf(why, whyCap, "interlaced PNG");
    return NULL;
  }
  if (depth == 16) {
    snprintf(why, whyCap, "16-bit PNG");
    return NULL;
  }
  int channels;
  switch (ctype) {
  case 0: channels = 1; break;
  case 2: channels = 3; break;
  case 3: channels = 1; break;
  case 4: channels = 2; break;
  case 6: channels = 4; break;
  default:
    snprintf(why, whyCap, "PNG colour type %d", (int)ctype);
    return NULL;
  }
  if (!(depth == 8 || ((ctype == 0 || ctype == 3) && (depth == 1 || depth == 2 || depth == 4)))) {
    snprintf(why, whyCap, "PNG colour type %d at %d bits", (int)ctype, (int)depth);
    return NULL;
  }
  if (ctype == 3 && (!plte || plteN == 0)) {
    snprintf(why, whyCap, "palette PNG without a palette");
    return NULL;
  }
  if (idatTotal < 8) {
    snprintf(why, whyCap, "no image data");
    return NULL;
  }

  // ---- gather the IDAT chunks into one stream ----
  uint8_t* idat = (uint8_t*)pngAlloc(idatTotal);
  if (!idat) {
    snprintf(why, whyCap, "no memory for the PNG stream");
    return NULL;
  }
  size_t got = 0;
  pos = 8;
  while (pos + 12 <= len) {
    const uint32_t clen = be32(data + pos);
    const uint8_t* ctyp = data + pos + 4;
    if (clen > len - pos - 12) {
      break;
    }
    if (!memcmp(ctyp, "IDAT", 4)) {
      memcpy(idat + got, data + pos + 8, clen);
      got += clen;
    } else if (!memcmp(ctyp, "IEND", 4)) {
      break;
    }
    pos += 12 + clen;
  }
  /* The zlib wrapper: CMF (method 8, window <= 32 KB) and FLG (no preset dictionary),
   * then the raw deflate stream, then a 4-byte Adler-32 this decoder does not check. */
  if ((idat[0] & 0x0F) != 8 || (idat[1] & 0x20) || (((idat[0] << 8) | idat[1]) % 31) != 0) {
    pngFree(idat);
    snprintf(why, whyCap, "unsupported zlib header");
    return NULL;
  }

  const size_t bitsPerPx = (size_t)channels * depth;
  const size_t rowBytes  = (bitsPerPx * w + 7) / 8;
  const size_t bpp       = bitsPerPx < 8 ? 1 : bitsPerPx / 8;
  const size_t rawBytes  = (rowBytes + 1) * h;
  uint8_t* raw = (uint8_t*)pngAlloc(rawBytes);
  if (!raw) {
    pngFree(idat);
    snprintf(why, whyCap, "no memory for the PNG rows");
    return NULL;
  }
  const int produced = rawInflate(idat + 2, got - 6, raw, rawBytes);
  pngFree(idat);
  if (produced != (int)rawBytes) {
    pngFree(raw);
    snprintf(why, whyCap, produced < 0 ? "PNG inflate failed" : "PNG inflated to the wrong size");
    return NULL;
  }
  pi->w = w;
  pi->h = h;
  pi->depth = depth;
  pi->ctype = ctype;
  pi->plte = plte;
  pi->plteN = plteN;
  pi->trns = trns;
  pi->trnsN = trnsN;
  pi->rowBytes = rowBytes;
  pi->bpp = bpp;
  return raw;
}

bool tilePngDecode(const uint8_t* data, size_t len, uint16_t* out, uint16_t nodata,
                   char* why, size_t whyCap, bool* blank) {
  if (blank) {
    *blank = false;
  }
  if (!why || whyCap == 0) {
    return false;
  }
  why[0] = '\0';
  if (!data || !out || len < 8 + 25 + 12) {
    snprintf(why, whyCap, "PNG too short");
    return false;
  }
  PngInfo pi;
  uint8_t* raw = pngInflateImage(data, len, &pi, why, whyCap);
  if (!raw) {
    return false;
  }
  const uint32_t w = pi.w, h = pi.h;
  const uint8_t depth = pi.depth, ctype = pi.ctype;
  const uint8_t* plte = pi.plte;
  const size_t plteN = pi.plteN;
  const uint8_t* trns = pi.trns;
  const size_t trnsN = pi.trnsN;
  const size_t rowBytes = pi.rowBytes, bpp = pi.bpp;

  // ---- unfilter, then unpack into 565 ----
  const uint8_t* prev = NULL;
  uint32_t clear = 0;             // pixels that were transparent (see `blank` in tile_png.h)
  for (uint32_t y = 0; y < h; y++) {
    uint8_t* line = raw + (size_t)y * (rowBytes + 1);
    const uint8_t ftype = line[0];
    uint8_t* row = line + 1;
    if (!unfilterRow(ftype, row, prev, rowBytes, bpp)) {
      pngFree(raw);
      snprintf(why, whyCap, "bad PNG filter %d on row %u", (int)ftype, (unsigned)y);
      return false;
    }
    uint16_t* o = out + (size_t)y * TILE_DECODE_PX;
    switch (ctype) {
    case 2:
      for (uint32_t x = 0; x < w; x++) {
        const uint8_t* p = row + x * 3;
        /* A tRNS on an RGB image names ONE colour (three 16-bit samples) as transparent. */
        if (trns && trnsN >= 6 && p[0] == trns[1] && p[1] == trns[3] && p[2] == trns[5]) {
          o[x] = nodata;
          clear++;
        } else {
          o[x] = tileColor565(p[0], p[1], p[2]);
        }
      }
      break;
    case 6:
      for (uint32_t x = 0; x < w; x++) {
        const uint8_t* p = row + x * 4;
        if (p[3] < 128) {
          o[x] = nodata;
          clear++;
        } else {
          o[x] = tileColor565(p[0], p[1], p[2]);
        }
      }
      break;
    case 4:
      for (uint32_t x = 0; x < w; x++) {
        const uint8_t* p = row + x * 2;
        if (p[1] < 128) {
          o[x] = nodata;
          clear++;
        } else {
          o[x] = tileColor565(p[0], p[0], p[0]);
        }
      }
      break;
    case 0:
    case 3: {
      /* 1/2/4/8-bit samples, MSB first within a byte. For grey a sample is scaled to 8 bits
       * (the spec's "left-shift replicate"); for palette it is an index. */
      const int perByte = 8 / depth;
      const uint8_t mask = (uint8_t)((1u << depth) - 1u);
      for (uint32_t x = 0; x < w; x++) {
        const uint8_t byte = row[x / (uint32_t)perByte];
        const int shift = 8 - depth * (int)(x % (uint32_t)perByte + 1);
        const uint8_t v = (uint8_t)((byte >> shift) & mask);
        if (ctype == 3) {
          if (v >= plteN) {
            pngFree(raw);
            snprintf(why, whyCap, "palette index %d past the palette", (int)v);
            return false;
          }
          if (trns && v < trnsN && trns[v] < 128) {
            o[x] = nodata;
            clear++;
          } else {
            o[x] = tileColor565(plte[v * 3], plte[v * 3 + 1], plte[v * 3 + 2]);
          }
        } else if (trns && trnsN >= 2 && v == trns[1] && trns[0] == 0) {
          o[x] = nodata;                   // tRNS on grey: one sample value (16-bit, high byte 0 at these depths)
          clear++;
        } else {
          uint8_t g;
          switch (depth) {
          case 1: g = v ? 255 : 0; break;
          case 2: g = (uint8_t)(v * 85); break;
          case 4: g = (uint8_t)(v * 17); break;
          default: g = v; break;
          }
          o[x] = tileColor565(g, g, g);
        }
      }
      break;
    }
    default:
      break;
    }
    prev = row;
  }
  pngFree(raw);
  if (blank) {
    *blank = (clear == w * h);
  }
  return true;
}

/* ── The same image as RGB rows (tile_png.h: tilePngDecodeRgb) ─────────────────────────────
 * The 565 loop above is left exactly as it was; this is its twin for a caller that wants the
 * samples themselves (an elevation tile's metres are R*256 + G + B/256 - a 565 would keep five
 * bits of R). The transparency rules are the 565 path's, pixel for pixel: alpha under 128, the
 * tRNS colour of an RGB or grey image, a tRNS entry under 128 on a palette. An 8-bit RGB image
 * without tRNS - a terrarium tile - is handed over straight from the unfiltered row, with no
 * copy; every other type is unpacked into one PSRAM scratch row (768 + 256 bytes) per decode. */
bool tilePngDecodeRgb(const uint8_t* data, size_t len, TilePngRowFn rowFn, void* ctx,
                      char* why, size_t whyCap, bool* blank) {
  if (blank) {
    *blank = false;
  }
  if (!why || whyCap == 0) {
    return false;
  }
  why[0] = '\0';
  if (!data || !rowFn || len < 8 + 25 + 12) {
    snprintf(why, whyCap, "PNG too short");
    return false;
  }
  PngInfo pi;
  uint8_t* raw = pngInflateImage(data, len, &pi, why, whyCap);
  if (!raw) {
    return false;
  }
  const bool rgbTrns = pi.trns && pi.trnsN >= 6;
  const bool direct = (pi.ctype == 2 && !rgbTrns);
  uint8_t* rgb = NULL;
  uint8_t* clr = NULL;
  if (!direct) {
    rgb = (uint8_t*)pngAlloc(TILE_DECODE_PX * 3 + TILE_DECODE_PX);
    if (!rgb) {
      pngFree(raw);
      snprintf(why, whyCap, "no memory for a PNG row");
      return false;
    }
    clr = rgb + TILE_DECODE_PX * 3;
  }
  const uint8_t* prev = NULL;
  uint32_t clear = 0;
  bool okAll = true;
  for (uint32_t y = 0; y < pi.h && okAll; y++) {
    uint8_t* line = raw + (size_t)y * (pi.rowBytes + 1);
    const uint8_t ftype = line[0];
    uint8_t* row = line + 1;
    if (!unfilterRow(ftype, row, prev, pi.rowBytes, pi.bpp)) {
      snprintf(why, whyCap, "bad PNG filter %d on row %u", (int)ftype, (unsigned)y);
      okAll = false;
      break;
    }
    prev = row;
    if (direct) {
      rowFn(ctx, (int)y, row, NULL);
      continue;
    }
    bool anyClear = false;
    for (uint32_t x = 0; x < pi.w; x++) {
      uint8_t r = 0, g = 0, b = 0, c = 0;
      switch (pi.ctype) {
      case 2: {
        const uint8_t* p = row + x * 3;
        r = p[0]; g = p[1]; b = p[2];
        c = (rgbTrns && r == pi.trns[1] && g == pi.trns[3] && b == pi.trns[5]) ? 1 : 0;
        break;
      }
      case 6: {
        const uint8_t* p = row + x * 4;
        r = p[0]; g = p[1]; b = p[2];
        c = p[3] < 128 ? 1 : 0;
        break;
      }
      case 4: {
        const uint8_t* p = row + x * 2;
        r = g = b = p[0];
        c = p[1] < 128 ? 1 : 0;
        break;
      }
      default: {                          // 0 and 3: 1/2/4/8-bit samples, MSB first
        const int perByte = 8 / pi.depth;
        const uint8_t mask = (uint8_t)((1u << pi.depth) - 1u);
        const uint8_t byte = row[x / (uint32_t)perByte];
        const int shift = 8 - pi.depth * (int)(x % (uint32_t)perByte + 1);
        const uint8_t v = (uint8_t)((byte >> shift) & mask);
        if (pi.ctype == 3) {
          if (v >= pi.plteN) {
            snprintf(why, whyCap, "palette index %d past the palette", (int)v);
            okAll = false;
            break;
          }
          r = pi.plte[v * 3]; g = pi.plte[v * 3 + 1]; b = pi.plte[v * 3 + 2];
          c = (pi.trns && v < pi.trnsN && pi.trns[v] < 128) ? 1 : 0;
        } else {
          uint8_t gv;
          switch (pi.depth) {
          case 1: gv = v ? 255 : 0; break;
          case 2: gv = (uint8_t)(v * 85); break;
          case 4: gv = (uint8_t)(v * 17); break;
          default: gv = v; break;
          }
          r = g = b = gv;
          c = (pi.trns && pi.trnsN >= 2 && v == pi.trns[1] && pi.trns[0] == 0) ? 1 : 0;
        }
        break;
      }
      }
      if (!okAll) {
        break;
      }
      rgb[x * 3] = r;
      rgb[x * 3 + 1] = g;
      rgb[x * 3 + 2] = b;
      clr[x] = c;
      if (c) {
        anyClear = true;
        clear++;
      }
    }
    if (okAll) {
      rowFn(ctx, (int)y, rgb, anyClear ? clr : NULL);
    }
  }
  pngFree(rgb);
  pngFree(raw);
  if (!okAll) {
    return false;
  }
  if (blank) {
    *blank = (clear == pi.w * pi.h);
  }
  return true;
}
