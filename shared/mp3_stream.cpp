/*
 * mp3_stream.cpp — see mp3_stream.h for why each awkward part is here.
 */

#include "mp3_stream.h"
#include "src/audio/helix-mp3/mp3dec.h"
#include "src/audio/helix-mp3/utils/helix_memory.h"
#include <string.h>

// ─── ID3 ────────────────────────────────────────────────────────────────────────────

uint32_t mp3Id3v2Size(const uint8_t* buf, size_t len) {
  if (!buf || len < 10) {
    return 0;
  }
  if (buf[0] != 'I' || buf[1] != 'D' || buf[2] != '3') {
    return 0;
  }
  /* Syncsafe: seven bits per byte, high bit always clear. Any byte with its top bit set
   * means this is not a valid size field and the tag should not be trusted. */
  for (int i = 6; i < 10; i++) {
    if (buf[i] & 0x80) {
      return 0;
    }
  }
  uint32_t sz = ((uint32_t)buf[6] << 21) | ((uint32_t)buf[7] << 14) |
                ((uint32_t)buf[8] << 7)  | (uint32_t)buf[9];

  uint32_t total = sz + 10;              // the header itself
  if (buf[5] & 0x10) {
    total += 10;                         // a footer is present (flag bit 4)
  }
  return total;
}

// ─── frame sync ─────────────────────────────────────────────────────────────────────

/* Enough of the header to reject a false positive. Eleven sync bits alone appear all over
 * album art, so version/layer/bitrate/rate are all checked before this returns true. */
static bool plausibleHeader(const uint8_t* h) {
  if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
    return false;
  }
  const uint8_t verId = (h[1] >> 3) & 0x03;
  if (verId == 1) {
    return false;                        // 01 is a reserved MPEG version
  }
  const uint8_t layer = (h[1] >> 1) & 0x03;
  if (layer == 0) {
    return false;                        // 00 is a reserved layer
  }
  const uint8_t bitrateIdx = (h[2] >> 4) & 0x0F;
  if (bitrateIdx == 0x00 || bitrateIdx == 0x0F) {
    return false;                        // `free` and `bad`
  }
  const uint8_t rateIdx = (h[2] >> 2) & 0x03;
  if (rateIdx == 0x03) {
    return false;                        // reserved sample rate
  }
  return true;
}

int mp3FindFrame(const uint8_t* buf, size_t len, size_t from) {
  if (!buf || len < 4) {
    return -1;
  }
  for (size_t i = from; i + 4 <= len; i++) {
    if (plausibleHeader(buf + i)) {
      return (int)i;
    }
  }
  return -1;
}

// ─── the stream ─────────────────────────────────────────────────────────────────────

Mp3Stream::Mp3Stream() : dec(0), inBuf(0), inLen(0), lastFrame(0), gotFormat(false) {
  memset(&fmt, 0, sizeof(fmt));
}

Mp3Stream::~Mp3Stream() {
  end();
}

bool Mp3Stream::begin() {
  if (dec && inBuf) {
    return true;
  }
  // Both from PSRAM (helix_malloc), for the reason in mp3_stream.h.
  if (!inBuf) {
    inBuf = (uint8_t*)helix_malloc(MP3_INBUF_BYTES);
  }
  if (!dec) {
    dec = (void*)MP3InitDecoder();
  }
  if (!dec || !inBuf) {
    end();                             // partial allocation is not a usable state
    return false;
  }
  reset();
  return true;
}

void Mp3Stream::end() {
  if (dec) {
    MP3FreeDecoder((HMP3Decoder)dec);
    dec = 0;
  }
  if (inBuf) {
    helix_free(inBuf);
    inBuf = 0;
  }
  reset();
}

void Mp3Stream::reset() {
  inLen = 0;
  lastFrame = 0;
  gotFormat = false;
  memset(&fmt, 0, sizeof(fmt));
  if (dec) {
    MP3ResetDecoder((HMP3Decoder)dec);   // see the 🛑 note on reset() in mp3_stream.h
  }
}

size_t Mp3Stream::fill(const uint8_t* data, size_t len) {
  if (!data || len == 0) {
    return 0;
  }
  size_t room = space();
  if (len > room) {
    len = room;
  }
  memcpy(inBuf + inLen, data, len);
  inLen += len;
  return len;
}

void Mp3Stream::discard(size_t n) {
  if (n >= inLen) {
    inLen = 0;
    return;
  }
  memmove(inBuf, inBuf + n, inLen - n);
  inLen -= n;
}

int Mp3Stream::decode(int16_t* out, Mp3Info* info) {
  if (!dec || !out) {
    return 0;
  }

  int at = mp3FindFrame(inBuf, inLen, 0);
  if (at < 0) {
    /* No frame here at all. Keep the last three bytes: a sync word can straddle the
     * boundary between two reads, and dropping the whole buffer would lose it. */
    if (inLen > 3) {
      discard(inLen - 3);
    }
    return 0;
  }
  if (at > 0) {
    discard((size_t)at);               // skip a tag, padding, or junk
  }

  /* Helix needs the WHOLE frame present. Asking it to decode a partial one returns
   * INDATA_UNDERFLOW, which is a request for more bytes and not an error. */
  unsigned char* p = inBuf;
  int left = (int)inLen;
  int rc = MP3Decode((HMP3Decoder)dec, &p, &left, out, 0);

  if (rc == ERR_MP3_INDATA_UNDERFLOW) {
    if (inLen >= MP3_INBUF_BYTES) {
      /* A header claiming a frame bigger than the whole buffer (no real one is: 1,441 bytes
       * at most). More bytes cannot help and "needs input" forever would stall the track, so
       * step past it like any other damage. */
      discard(1);
      return -1;
    }
    return 0;                          // caller should fill() and try again
  }

  const size_t consumed = inLen - (size_t)(left < 0 ? 0 : left);

  /* 🛑 MAINDATA_UNDERFLOW IS NOT A REQUEST FOR BYTES, AND IT WAS HANDLED AS ONE UNTIL 0.9.79.
   * helix returns it AFTER it has consumed the frame (mp3dec.c: *inbuf and *bytesLeft are
   * advanced past it, its main data is appended to the bit reservoir, outbuf is zeroed): it
   * means "this frame's main data starts in a frame I never saw" - the first frame or two
   * after a seek. Returning 0 here without discarding handed the SAME frame back on the next
   * call, helix appended its main data a SECOND time and then "decoded" it against its own
   * duplicate. Replayed on the Mac against the real decoder (2026-09-25): 99.4-100% of
   * resumes hit this, the duplicated frame played on 45-61% of them, and 2-33 of every 500
   * were a burst louder than twice the real frame. Step over it; the reservoir it filled is
   * exactly what the next frame needs. Never seen from the start of a file (0 in 7 tracks). */
  if (rc == ERR_MP3_MAINDATA_UNDERFLOW) {
    discard(consumed > 0 ? consumed : 1);
    lastFrame = consumed;              // it WAS a frame: the feed remembers where it started
    return MP3_SKIPPED_RESERVOIR;
  }

  if (rc != ERR_MP3_NONE) {
    /* A bad frame is survivable — that is the point of a sync word. Step over one byte
     * and let the search resynchronise, so a damaged file costs a crackle rather than
     * the rest of the album. */
    discard(consumed > 0 ? consumed : 1);
    return -1;
  }

  discard(consumed);
  lastFrame = consumed;

  MP3FrameInfo fi;
  MP3GetLastFrameInfo((HMP3Decoder)dec, &fi);

  fmt.sampleRate = fi.samprate;
  fmt.channels   = fi.nChans;
  fmt.bitrate    = fi.bitrate;
  fmt.outputSamps = fi.outputSamps;
  gotFormat = fi.outputSamps > 0 && fi.samprate > 0;

  if (info) {
    *info = fmt;
  }
  return fi.outputSamps;
}
