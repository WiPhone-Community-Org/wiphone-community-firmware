/*
 * jpeg_grey.cpp — see jpeg_grey.h.
 *
 * Structure follows the JPEG spec's own decoding procedure (ITU T.81 Annex F): build Huffman
 * tables from BITS/HUFFVAL, decode each block's DC as a difference and its AC as run/size
 * pairs, dequantise, de-zigzag, inverse DCT, level shift.
 */
#include "jpeg_grey.h"

#include <string.h>
#include <math.h>

#if defined(ARDUINO)
  #include <Arduino.h>
  static void* jgAlloc(size_t n) {
    void* p = ps_malloc(n);           // a band of a 1453-wide cover is 11 KB: not internal RAM
    return p ? p : malloc(n);
  }
#else
  #include <stdlib.h>
  static void* jgAlloc(size_t n) { return malloc(n); }
#endif

static void jgFree(void* p) { free(p); }

// Zigzag order: coefficient i of the stream belongs at ZIGZAG[i] of the natural 8x8 block.
static const uint8_t ZIGZAG[64] = {
   0,  1,  8, 16,  9,  2,  3, 10,
  17, 24, 32, 25, 18, 11,  4,  5,
  12, 19, 26, 33, 40, 48, 41, 34,
  27, 20, 13,  6,  7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36,
  29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46,
  53, 60, 61, 54, 47, 55, 62, 63
};

struct HuffTable {
  bool  present;
  uint8_t vals[256];
  int   mincode[17];
  int   maxcode[18];      // maxcode[l] = -1 when no codes of that length
  int   valptr[17];
};

struct BitReader {
  const uint8_t* p;
  const uint8_t* end;
  uint32_t buf;
  int      cnt;           // bits held in buf
  bool     hitMarker;     // a marker was reached: the entropy segment is over
};

static void brInit(BitReader* b, const uint8_t* p, const uint8_t* end) {
  b->p = p;
  b->end = end;
  b->buf = 0;
  b->cnt = 0;
  b->hitMarker = false;
}

/* One byte of entropy-coded data, undoing the 0xFF00 stuffing. A real marker ends the
 * segment; from then on the reader feeds zero bits, which lets a truncated file finish its
 * last block with rubbish rather than reading off the end of the buffer. */
static int brByte(BitReader* b) {
  if (b->hitMarker || b->p >= b->end) {
    b->hitMarker = true;
    return 0;
  }
  uint8_t c = *b->p++;
  if (c == 0xFF) {
    if (b->p >= b->end) {
      b->hitMarker = true;
      return 0;
    }
    uint8_t n = *b->p;
    if (n == 0x00) {
      b->p++;                      // stuffed: a literal 0xFF
      return 0xFF;
    }
    b->p--;                        // leave the marker in place for the caller to see
    b->hitMarker = true;
    return 0;
  }
  return c;
}

static int brBits(BitReader* b, int n) {
  while (b->cnt < n) {
    b->buf = (b->buf << 8) | (uint32_t)brByte(b);
    b->cnt += 8;
  }
  int v = (int)((b->buf >> (b->cnt - n)) & ((1u << n) - 1));
  b->cnt -= n;
  return v;
}

// F.2.2.1: turn an n-bit magnitude into its signed value.
static int brExtend(int v, int n) {
  return (n && v < (1 << (n - 1))) ? v - (1 << n) + 1 : v;
}

// F.2.2.3: decode one Huffman-coded symbol, one bit at a time.
static int huffDecode(BitReader* b, const HuffTable* h) {
  int code = brBits(b, 1);
  int l = 1;
  while (l <= 16 && (h->maxcode[l] < 0 || code > h->maxcode[l])) {
    code = (code << 1) | brBits(b, 1);
    l++;
  }
  if (l > 16) {
    return -1;
  }
  int idx = h->valptr[l] + code - h->mincode[l];
  if (idx < 0 || idx > 255) {
    return -1;
  }
  return h->vals[idx];
}

static bool huffBuild(HuffTable* h, const uint8_t* bits, const uint8_t* vals, int nVals) {
  memcpy(h->vals, vals, (size_t)nVals);
  int code = 0, k = 0;
  for (int l = 1; l <= 16; l++) {
    h->valptr[l] = k;
    h->mincode[l] = code;
    int n = bits[l];
    if (n) {
      k += n;
      code += n;
      h->maxcode[l] = code - 1;
    } else {
      h->maxcode[l] = -1;
    }
    code <<= 1;
    if (k > 256) {
      return false;
    }
  }
  h->maxcode[17] = 0x7FFFFFFF;
  h->present = true;
  return true;
}

/* A plain separable float IDCT. Not the fastest known, but this runs on a handful of blocks
 * per picture on a chip with an FPU, and being obviously correct matters more here than being
 * clever — it is checked pixel-for-pixel against a reference decoder. */
static void idct8x8(const int* in, uint8_t* out, int outStride) {
  static float cosTab[8][8];
  static bool built = false;
  if (!built) {
    for (int x = 0; x < 8; x++) {
      for (int u = 0; u < 8; u++) {
        cosTab[x][u] = cosf((2.0f * x + 1.0f) * u * 3.14159265358979f / 16.0f) *
                       (u == 0 ? 0.353553390593f : 0.5f);   // 1/(2*sqrt2) : 1/2
      }
    }
    built = true;
  }

  float tmp[64];
  for (int y = 0; y < 8; y++) {              // rows
    for (int x = 0; x < 8; x++) {
      float s = 0.0f;
      for (int u = 0; u < 8; u++) {
        s += cosTab[x][u] * (float)in[y * 8 + u];
      }
      tmp[y * 8 + x] = s;
    }
  }
  for (int x = 0; x < 8; x++) {              // columns
    for (int y = 0; y < 8; y++) {
      float s = 0.0f;
      for (int v = 0; v < 8; v++) {
        s += cosTab[y][v] * tmp[v * 8 + x];
      }
      int p = (int)lrintf(s) + 128;          // level shift
      out[y * outStride + x] = (uint8_t)(p < 0 ? 0 : (p > 255 ? 255 : p));
    }
  }
}

static uint16_t rd16be(const uint8_t* p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

bool jpegGreyIsGreyBaseline(const uint8_t* data, size_t len) {
  if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
    return false;
  }
  size_t i = 2;
  while (i + 3 < len) {
    if (data[i] != 0xFF) {
      i++;
      continue;
    }
    uint8_t m = data[i + 1];
    if (m == 0xFF) {
      i++;
      continue;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      i += 2;
      continue;
    }
    if (m == 0xDA || m == 0xD9) {
      return false;                       // reached the scan with no frame header we liked
    }
    size_t seg = rd16be(data + i + 2);
    if (m == 0xC0 || m == 0xC1) {         // baseline / extended sequential
      return (i + 9 < len) && data[i + 9] == 1;
    }
    if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      return false;                       // progressive, lossless, arithmetic: not ours
    }
    if (seg < 2) {
      return false;
    }
    i += 2 + seg;
  }
  return false;
}

const char* jpegGreyStatusText(JpegGreyStatus s) {
  switch (s) {
    case JPEG_GREY_OK:              return "ok";
    case JPEG_GREY_ERR_NOT_JPEG:    return "not a JPEG";
    case JPEG_GREY_ERR_UNSUPPORTED: return "not a baseline greyscale JPEG";
    case JPEG_GREY_ERR_BAD_DATA:    return "malformed JPEG";
    case JPEG_GREY_ERR_MEMORY:      return "out of memory";
  }
  return "unknown";
}

JpegGreyStatus jpegGreyDecode(const uint8_t* data, size_t len,
                              JpegGreyRowsFn cb, void* ctx,
                              uint16_t* wOut, uint16_t* hOut) {
  if (wOut) {
    *wOut = 0;
  }
  if (hOut) {
    *hOut = 0;
  }
  if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
    return JPEG_GREY_ERR_NOT_JPEG;
  }

  uint16_t qt[4][64];
  memset(qt, 0, sizeof(qt));
  HuffTable hDC[4], hAC[4];
  memset(hDC, 0, sizeof(hDC));
  memset(hAC, 0, sizeof(hAC));

  uint16_t W = 0, H = 0;
  uint8_t qSel = 0, dcSel = 0, acSel = 0;
  uint16_t restart = 0;
  bool haveFrame = false;
  const uint8_t* scan = NULL;

  size_t i = 2;
  while (i + 1 < len) {
    if (data[i] != 0xFF) {
      i++;
      continue;
    }
    uint8_t m = data[i + 1];
    if (m == 0xFF) {
      i++;
      continue;
    }
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      i += 2;
      continue;
    }
    if (m == 0xD9) {
      break;
    }
    if (i + 3 >= len) {
      break;
    }
    size_t seg = rd16be(data + i + 2);
    if (seg < 2 || i + 2 + seg > len) {
      return JPEG_GREY_ERR_BAD_DATA;
    }
    const uint8_t* p = data + i + 4;
    size_t plen = seg - 2;

    if (m == 0xDB) {                                   // DQT
      size_t k = 0;
      while (k < plen) {
        uint8_t pq = p[k] >> 4, tq = p[k] & 15;
        k++;
        if (tq > 3 || k + (pq ? 128u : 64u) > plen) {
          return JPEG_GREY_ERR_BAD_DATA;
        }
        for (int n = 0; n < 64; n++) {
          qt[tq][n] = pq ? rd16be(p + k + n * 2) : p[k + n];
        }
        k += pq ? 128 : 64;
      }
    } else if (m == 0xC4) {                            // DHT
      size_t k = 0;
      while (k + 17 <= plen) {
        uint8_t tc = p[k] >> 4, th = p[k] & 15;
        k++;
        uint8_t bits[17];
        bits[0] = 0;
        int total = 0;
        for (int l = 1; l <= 16; l++) {
          bits[l] = p[k + l - 1];
          total += bits[l];
        }
        k += 16;
        if (th > 3 || total > 256 || k + (size_t)total > plen) {
          return JPEG_GREY_ERR_BAD_DATA;
        }
        HuffTable* t = tc ? &hAC[th] : &hDC[th];
        if (!huffBuild(t, bits, p + k, total)) {
          return JPEG_GREY_ERR_BAD_DATA;
        }
        k += (size_t)total;
      }
    } else if (m == 0xDD) {                            // DRI
      if (plen >= 2) {
        restart = rd16be(p);
      }
    } else if (m == 0xC0 || m == 0xC1) {               // SOF0 / SOF1
      if (plen < 6) {
        return JPEG_GREY_ERR_BAD_DATA;
      }
      if (p[0] != 8) {
        return JPEG_GREY_ERR_UNSUPPORTED;              // 12-bit
      }
      H = rd16be(p + 1);
      W = rd16be(p + 3);
      if (wOut) {
        *wOut = W;
      }
      if (hOut) {
        *hOut = H;
      }
      if (p[5] != 1) {
        return JPEG_GREY_ERR_UNSUPPORTED;              // colour: the ROM decoder handles those
      }
      if (plen < 9) {
        return JPEG_GREY_ERR_BAD_DATA;
      }
      if (p[7] != 0x11) {
        return JPEG_GREY_ERR_UNSUPPORTED;              // subsampled single component
      }
      qSel = p[8] & 3;
      haveFrame = true;
    } else if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      return JPEG_GREY_ERR_UNSUPPORTED;                // progressive / lossless / arithmetic
    } else if (m == 0xDA) {                            // SOS
      if (plen < 4 || p[0] != 1) {
        return JPEG_GREY_ERR_UNSUPPORTED;
      }
      dcSel = p[2] >> 4;
      acSel = p[2] & 15;
      if (dcSel > 3 || acSel > 3) {
        return JPEG_GREY_ERR_BAD_DATA;
      }
      scan = data + i + 2 + seg;
      break;
    }
    i += 2 + seg;
  }

  if (!haveFrame || !scan || !W || !H) {
    return JPEG_GREY_ERR_UNSUPPORTED;
  }
  if (!hDC[dcSel].present || !hAC[acSel].present) {
    return JPEG_GREY_ERR_BAD_DATA;
  }

  const int bw = (W + 7) / 8;                          // blocks across
  const int stride = bw * 8;                           // band width, padded to whole blocks
  uint8_t* band = (uint8_t*)jgAlloc((size_t)stride * 8);
  if (!band) {
    return JPEG_GREY_ERR_MEMORY;
  }

  BitReader br;
  brInit(&br, scan, data + len);
  int dcPred = 0;
  int sinceRestart = 0;
  JpegGreyStatus st = JPEG_GREY_OK;

  for (int by = 0; by < (H + 7) / 8 && st == JPEG_GREY_OK; by++) {
    for (int bx = 0; bx < bw; bx++) {
      /* Restart markers resynchronise a damaged stream: the bit buffer is dropped and the DC
       * predictor starts again. Without handling them, any image that uses them decodes as
       * noise from the first marker on. */
      if (restart && sinceRestart == restart) {
        br.cnt = 0;
        br.hitMarker = false;
        while (br.p + 1 < br.end && !(br.p[0] == 0xFF && br.p[1] >= 0xD0 && br.p[1] <= 0xD7)) {
          br.p++;
        }
        if (br.p + 1 < br.end) {
          br.p += 2;
        }
        dcPred = 0;
        sinceRestart = 0;
      }
      sinceRestart++;

      int coef[64];
      memset(coef, 0, sizeof(coef));

      int t = huffDecode(&br, &hDC[dcSel]);
      if (t < 0 || t > 15) {
        st = JPEG_GREY_ERR_BAD_DATA;
        break;
      }
      int diff = t ? brExtend(brBits(&br, t), t) : 0;
      dcPred += diff;
      coef[0] = dcPred * (int)qt[qSel][0];

      for (int k = 1; k < 64; ) {
        int rs = huffDecode(&br, &hAC[acSel]);
        if (rs < 0) {
          st = JPEG_GREY_ERR_BAD_DATA;
          break;
        }
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
          if (r != 15) {
            break;                                     // EOB
          }
          k += 16;                                     // ZRL: sixteen zeroes
          continue;
        }
        k += r;
        if (k > 63) {
          break;
        }
        int v = brExtend(brBits(&br, s), s);
        coef[ZIGZAG[k]] = v * (int)qt[qSel][k];
        k++;
      }
      if (st != JPEG_GREY_OK) {
        break;
      }
      idct8x8(coef, band + bx * 8, stride);
    }
    if (st != JPEG_GREY_OK) {
      break;
    }
    int rows = H - by * 8;
    if (rows > 8) {
      rows = 8;
    }
    if (cb) {
      cb(ctx, by * 8, rows, band, stride);
    }
  }

  jgFree(band);
  return st;
}
