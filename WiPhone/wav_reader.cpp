/*
 * wav_reader.cpp — see wav_reader.h for why the phone plays WAV and not MP3.
 */

#include "wav_reader.h"
#include <string.h>

// ─── header ─────────────────────────────────────────────────────────────────────────

static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static bool tagIs(const uint8_t* p, const char* t) {
  return p[0] == (uint8_t)t[0] && p[1] == (uint8_t)t[1] && p[2] == (uint8_t)t[2] && p[3] == (uint8_t)t[3];
}

#define WAVE_FORMAT_PCM        0x0001
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

bool wavParseHeader(const uint8_t* buf, size_t len, WavInfo* out) {
  if (!out) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->ok = false;
  out->problem = "Not a WAV file";

  if (!buf || len < 12) {
    return false;
  }
  if (!tagIs(buf, "RIFF") || !tagIs(buf + 8, "WAVE")) {
    return false;
  }

  bool haveFmt = false;
  uint16_t fmtTag = 0;

  /* Chunk walk. Every field is bounds-checked against `len` before it is read: this is
   * parsing a file that arrived over an uploader with no extension filter, so a truncated
   * or deliberately malformed header is an expected input, not an edge case. */
  size_t pos = 12;
  while (pos + 8 <= len) {
    const uint8_t* ch = buf + pos;
    uint32_t sz = rd32(ch + 4);
    size_t body = pos + 8;

    if (tagIs(ch, "fmt ")) {
      if (sz < 16 || body + 16 > len) {
        out->problem = "Broken WAV header";
        return false;
      }
      fmtTag         = rd16(buf + body);
      out->channels  = rd16(buf + body + 2);
      out->sampleRate = rd32(buf + body + 4);
      out->bits      = rd16(buf + body + 14);

      /* EXTENSIBLE hides the real format in a GUID at the end of the fmt chunk. Its
       * first two bytes are the format tag, which is all we need. */
      if (fmtTag == WAVE_FORMAT_EXTENSIBLE && sz >= 40 && body + 26 <= len) {
        fmtTag = rd16(buf + body + 24);
      }
      haveFmt = true;

    } else if (tagIs(ch, "data")) {
      if (!haveFmt) {
        out->problem = "WAV data before format";
        return false;
      }
      out->dataOffset = (uint32_t)body;
      /* ⚠ Reported as-is, and the caller MUST clamp it to the real file size. A WAV
       * written to a pipe carries 0xFFFFFFFF or 0 here because the writer did not know
       * the length yet, and this function only ever sees the first KB of the file, so it
       * is in no position to check. app_music clamps against the SD file size. */
      out->dataBytes = sz;

      if (fmtTag == WAVE_FORMAT_IEEE_FLOAT) {
        out->problem = "Float WAV: convert to 16-bit";
        return false;
      }
      if (fmtTag != WAVE_FORMAT_PCM) {
        out->problem = "Compressed WAV: convert to PCM";
        return false;
      }
      if (out->channels < 1 || out->channels > 2) {
        out->problem = "Only mono or stereo";
        return false;
      }
      if (out->bits != 8 && out->bits != 16 && out->bits != 24 && out->bits != 32) {
        out->problem = "Unsupported bit depth";
        return false;
      }
      if (out->sampleRate < 4000 || out->sampleRate > 192000) {
        out->problem = "Odd sample rate";
        return false;
      }
      out->ok = true;
      out->problem = NULL;
      return true;
    }

    /* Chunks are word-aligned: an odd size is followed by a pad byte. Missing this is
     * the classic way a chunk walker ends up reading a tag one byte off and declaring a
     * perfectly good file broken. */
    uint32_t adv = sz + (sz & 1u);
    if (adv > len || body + adv < body) {     // overflow or runs past the buffer
      break;
    }
    pos = body + adv;
  }

  out->problem = haveFmt ? "No audio in WAV" : "Broken WAV header";
  return false;
}

// ─── conversion ─────────────────────────────────────────────────────────────────────

#define FP_ONE 65536u

WavConverter::WavConverter()
  : channels(1), bits(16), stereo(false), step(FP_ONE), frac(0),
    s0L(0), s0R(0), s1L(0), s1R(0), primed(false), havePair(false), direct(true) {}

void WavConverter::begin(const WavInfo& in, uint32_t outRate, bool stereoOut) {
  channels = in.channels ? in.channels : 1;
  bits     = in.bits ? in.bits : 16;
  stereo   = stereoOut;
  frac     = 0;
  s0L = s0R = s1L = s1R = 0;
  primed   = false;
  havePair = false;

  if (outRate == 0) {
    outRate = in.sampleRate ? in.sampleRate : 16000;
  }
  // 16.16 source frames per output frame. 64-bit intermediate: 192000<<16 overflows 32.
  step = (uint32_t)(((uint64_t)in.sampleRate << 16) / outRate);
  if (step == 0) {
    step = FP_ONE;
  }
  direct = (in.sampleRate == outRate) && (bits == 16) &&
           (channels == (stereo ? 2 : 1));
}

/* One source frame as a left/right pair. A mono source yields the same value on both, so
 * a mono file played into headphones is heard in both ears rather than only the left. */
void WavConverter::readFrame(const uint8_t* src, size_t frame, int16_t* l, int16_t* r) const {
  const uint32_t bytes = (uint32_t)(bits / 8);
  const uint8_t* p = src + frame * channels * bytes;

  int32_t v[2] = { 0, 0 };
  for (uint16_t c = 0; c < channels && c < 2; c++) {
    const uint8_t* s = p + c * bytes;
    switch (bits) {
      case 8:
        // 8-bit WAV is UNSIGNED, unlike every other depth. Centre it.
        v[c] = ((int32_t)s[0] - 128) << 8;
        break;
      case 24:
        v[c] = (int32_t)(((uint32_t)s[0] << 8) | ((uint32_t)s[1] << 16) | ((uint32_t)s[2] << 24));
        v[c] >>= 16;          // arithmetic shift keeps the sign
        break;
      case 32:
        v[c] = (int32_t)rd32(s) >> 16;
        break;
      case 16:
      default:
        v[c] = (int16_t)rd16(s);
        break;
    }
  }

  if (channels == 1) {
    v[1] = v[0];
  }
  if (!stereo && channels > 1) {
    // Mono output from a stereo source: average, do not just drop the right channel —
    // anything mixed hard-right would vanish.
    int32_t m = (v[0] + v[1]) / 2;
    v[0] = v[1] = m;
  }

  for (int i = 0; i < 2; i++) {
    if (v[i] > 32767) {
      v[i] = 32767;
    } else if (v[i] < -32768) {
      v[i] = -32768;
    }
  }
  *l = (int16_t)v[0];
  *r = (int16_t)v[1];
}

size_t WavConverter::feed(const uint8_t* src, size_t srcFrames, int16_t* out, size_t outFrames, size_t* framesUsed) {
  size_t i = 0;
  size_t produced = 0;

  if (framesUsed) {
    *framesUsed = 0;
  }
  if (!src || !out || outFrames == 0) {
    return 0;
  }

  /* Establish out = lerp(s0, s1, frac) before producing anything. s1 has to be the frame
   * AFTER s0, which needs a second frame — and on the first call there may only be one,
   * so the pair is completed on whichever call brings the second. */
  if (!primed) {
    if (srcFrames == 0) {
      return 0;
    }
    readFrame(src, 0, &s0L, &s0R);
    s1L = s0L;
    s1R = s0R;
    i = 1;
    frac = 0;
    primed = true;
  }
  if (!havePair) {
    if (i >= srcFrames) {
      if (framesUsed) {
        *framesUsed = i;
      }
      return 0;
    }
    readFrame(src, i++, &s1L, &s1R);
    havePair = true;
  }

  while (produced < outFrames) {
    // Walk forward until the interpolation window straddles the wanted position.
    while (frac >= FP_ONE) {
      if (i >= srcFrames) {
        if (framesUsed) {
          *framesUsed = i;
        }
        return produced;
      }
      s0L = s1L;
      s0R = s1R;
      readFrame(src, i++, &s1L, &s1R);
      frac -= FP_ONE;
    }
    const int32_t t = (int32_t)(frac >> 8);      // 0..255
    int32_t l = (int32_t)s0L + ((((int32_t)s1L - (int32_t)s0L) * t) >> 8);
    if (stereo) {
      int32_t r = (int32_t)s0R + ((((int32_t)s1R - (int32_t)s0R) * t) >> 8);
      out[produced * 2]     = (int16_t)l;
      out[produced * 2 + 1] = (int16_t)r;
    } else {
      out[produced] = (int16_t)l;
    }
    produced++;
    frac += step;
  }

  if (framesUsed) {
    *framesUsed = i;
  }
  return produced;
}
