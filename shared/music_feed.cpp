/*
 * music_feed.cpp — see music_feed.h for what this fixes and why it is built this way.
 */

#include "music_feed.h"
#include <string.h>

#if defined(ESP32) || defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
/* PSRAM, explicitly — the same rule as helix_memory.c: a fall-back to internal RAM is the one
 * outcome worse than refusing to play. heap_caps_malloc aligns to at least 4 bytes, which the
 * card read needs (see MUSIC_SCRATCH_BYTES). */
static void* musicAlloc(size_t n) {
  return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void musicFree(void* p) {
  if (p) {
    heap_caps_free(p);
  }
}
#else
#include <stdlib.h>
static void* musicAlloc(size_t n) {
  return malloc(n);
}
static void musicFree(void* p) {
  free(p);
}
#endif

// ─── the lead ───────────────────────────────────────────────────────────────────────────

void MusicLead::begin(const MusicDma& d, uint64_t nowUs) {
  bufLen = d.len;
  capS = (uint32_t)d.bufs * d.len;
  r = d.rate ? d.rate : 1;
  /* A new ring (or the same ring at a new rate): what it holds is unknown, but it cannot be
   * less than nothing or more than full. The next short write pins it down again. */
  calUs = nowUs;
  calW = written;
  calLo = 0;
  calHi = (int32_t)capS;
}

int64_t MusicLead::raw(int32_t calVal, uint64_t nowUs, bool low) const {
  const uint64_t dt = nowUs > calUs ? nowUs - calUs : 0;
  /* The lower bound assumes the most the ring can have drained (rounded up, and a sample for
   * the microsecond the clock is truncated to); the upper bound the least. */
  const uint64_t num = dt * (uint64_t)r;
  const int64_t drained = low ? (int64_t)((num + 999999ULL) / 1000000ULL) + 1 : (int64_t)(num / 1000000ULL);
  return (int64_t)calVal + (int64_t)(written - calW) - drained;
}

int32_t MusicLead::lo(uint64_t nowUs) const {
  int64_t v = raw(calLo, nowUs, true);
  if (v > (int64_t)capS) {
    v = capS;
  }
  if (v < -0x40000000LL) {
    v = -0x40000000LL;
  }
  return (int32_t)v;
}

int32_t MusicLead::hi(uint64_t nowUs) const {
  int64_t v = raw(calHi, nowUs, false);
  if (v > (int64_t)capS) {
    v = capS;
  }
  if (v < -0x40000000LL) {
    v = -0x40000000LL;
  }
  return (int32_t)v;
}

void MusicLead::full(uint64_t nowUs) {
  /* Every buffer but the one playing holds samples we wrote, and the one playing has between
   * nothing and all of itself left. That is the whole of what a short write says — and it is
   * exact to one buffer (512 samples = 23 ms at 22.05 kHz), which is the counter's resolution. */
  calUs = nowUs;
  calW = written;
  calLo = (int32_t)(capS - bufLen);
  calHi = (int32_t)capS;
}

bool MusicLead::check(uint64_t nowUs) {
  const int64_t l = lo(nowUs);
  const int64_t h = hi(nowUs);
  if (l < minLo) {
    minLo = (int32_t)l;
  }
  /* Tolerance: 1 ms, plus 200 ppm of the time since the bounds were last pinned (the APLL is
   * set to a few ppm; this is margin, so a drop is only ever counted when it certainly was one). */
  const uint64_t dt = nowUs > calUs ? nowUs - calUs : 0;
  const int64_t tol = (int64_t)(r / 1000) + (int64_t)(dt * (uint64_t)r / 1000000ULL) / 5000;
  if (h < -tol) {
    drops++;
    dryUs += (uint64_t)(-h) * 1000000ULL / r;
    /* The ring has been dry: nothing of ours is in it. What we write next plays within one
     * ring of now. The next full ring pins it exactly again. */
    calUs = nowUs;
    calW = written;
    calLo = 0;
    calHi = (int32_t)capS;
    return true;
  }
  return false;
}

void MusicLead::normalize(uint64_t nowUs) {
  const int64_t h = raw(calHi, nowUs, false);
  const int64_t l = raw(calLo, nowUs, true);
  if (h > (int64_t)capS || l > (int64_t)capS) {
    /* The ring cannot hold more than full. Re-anchor, or a loose bound (after a drop, or at a
     * start) would stay loose by everything written since, and a later dry spell would be
     * counted late. */
    calUs = nowUs;
    calW = written;
    calHi = (int32_t)(h > (int64_t)capS ? (int64_t)capS : h);
    calLo = (int32_t)(l > (int64_t)capS ? (int64_t)capS : l);
  }
}

// ─── mono, half rate ────────────────────────────────────────────────────────────────────

/* The half-band low-pass the 2:1 decimation runs through: a 23-tap Kaiser-windowed sinc
 * (beta 6), Q15, symmetric, every even offset from the centre zero. Designed and checked with
 * numpy for a 44.1 kHz input: -0.16 dB at 8 kHz, -0.86 at 9 kHz, -2.6 at 10 kHz (the band edge
 * is 11.025), -28 dB from 13.7 kHz and -63 dB from 15.4 kHz — so what folds back below 11 kHz
 * is what was already quiet. Sum = 32768 exactly (DC passes unchanged); sum of |h| = 45864, so
 * a full-scale input cannot overflow the 32-bit accumulator. Six multiplies an output sample. */
static const int32_t HB_C0  = 16384;   // centre
static const int32_t HB_C1  = 10193;   // +-1
static const int32_t HB_C3  = -2826;   // +-3
static const int32_t HB_C5  = 1151;    // +-5
static const int32_t HB_C7  = -434;    // +-7
static const int32_t HB_C9  = 122;     // +-9
static const int32_t HB_C11 = -14;     // +-11

void MusicPcm::begin(bool halfRate) {
  half = halfRate;
  phase = 1;
  memset(hist, 0, sizeof(hist));
}

static inline int16_t sat16(int32_t v) {
  if (v > 32767) {
    return 32767;
  }
  if (v < -32768) {
    return -32768;
  }
  return (int16_t)v;
}

size_t MusicPcm::convert(const int16_t* in, size_t frames, int ch, int16_t* out, int16_t* work) {
  if (!in || !out || !work || frames == 0) {
    return 0;
  }
  /* Down to one channel first, into the work buffer behind the filter's history. (L+R)/2 in
   * 32 bits: it cannot clip, and it is the mix the WAV path's converter uses too. */
  int16_t* m = work + HIST;
  if (ch >= 2) {
    for (size_t i = 0; i < frames; i++) {
      m[i] = (int16_t)(((int32_t)in[2 * i] + (int32_t)in[2 * i + 1]) >> 1);
    }
  } else {
    memcpy(m, in, frames * sizeof(int16_t));
  }
  if (!half) {
    memcpy(out, m, frames * sizeof(int16_t));
    return frames;
  }
  memcpy(work, hist, sizeof(hist));
  size_t n = 0;
  size_t i = phase;
  for (; i < frames; i += 2) {
    const int16_t* x = m + i;                  // x[0] newest, x[-22] oldest
    int32_t acc = HB_C0 * x[-11]
                + HB_C1  * ((int32_t)x[-10] + x[-12])
                + HB_C3  * ((int32_t)x[-8]  + x[-14])
                + HB_C5  * ((int32_t)x[-6]  + x[-16])
                + HB_C7  * ((int32_t)x[-4]  + x[-18])
                + HB_C9  * ((int32_t)x[-2]  + x[-20])
                + HB_C11 * ((int32_t)x[0]   + x[-22]);
    out[n++] = sat16((acc + 16384) >> 15);
  }
  phase = (uint8_t)(i - frames);
  memcpy(hist, work + frames, sizeof(hist));   // the last HIST inputs, for the next unit
  return n;
}

// ─── the feed ───────────────────────────────────────────────────────────────────────────

MusicFeed::MusicFeed() {
  memset(places, 0, sizeof(places));
  memset(&wav, 0, sizeof(wav));
  memset(wavCarry, 0, sizeof(wavCarry));
}

MusicFeed::~MusicFeed() {
  musicFree(scratch);
  musicFree(stageBuf);
  musicFree(work);
}

bool MusicFeed::begin() {
  if (!scratch) {
    scratch = (uint8_t*)musicAlloc(MUSIC_SCRATCH_BYTES);
  }
  if (!stageBuf) {
    stageBuf = (int16_t*)musicAlloc(MUSIC_STAGE_SAMPLES * sizeof(int16_t));
  }
  if (!work) {
    work = (int16_t*)musicAlloc((MusicPcm::HIST + MUSIC_UNIT_FRAMES) * sizeof(int16_t));
  }
  return scratch && stageBuf && work;
}

void MusicFeed::reset() {
  kind = NONE;
  sink = 0;
  why = 0;
  srcPos = 0;
  srcSize = 0;
  readFails = 0;
  readFailed = readHeld = readReopen = readSeek = false;
  srcHz = outHz = 0;
  srcCh = 0;
  srcEof = decodeEof = isEnded = dropNext = false;
  wavLeft = wavStartFrame = 0;
  wavCarryN = 0;
  stageOff = stageLeft = 0;
  outStaged = outWritten = 0;
  zerosAfter = 0;
  zeroTarget = 0;
  ringBase = 0;
  placeHead = placeCount = 0;
  dmaGen = 0;
  lastPassUs = 0;
  lead = MusicLead();
  nUnits = nSkipped = nReservoir = nReads = nPasses = nReadErrors = 0;
  readBytes = sumWorkUs = maxGapUs = maxWorkUs = 0;
  pcm.begin(false);
}

void MusicFeed::remember(uint32_t fileOff) {
  places[placeHead].off = fileOff;
  places[placeHead].out = outStaged;
  placeHead = (uint8_t)((placeHead + 1) % MUSIC_PLACES);
  if (placeCount < MUSIC_PLACES) {
    placeCount++;
  }
}

/* 🛑 A READ THAT RETURNS NOTHING IS THE END OF THE FILE ONLY AT THE END OF THE FILE. Arduino's
 * File::read() gives 0 for an error too, and this took every one for the end: one FatFs error
 * (a CRC, a timeout, the volume lock held by the tile downloader) skipped the rest of the track,
 * and a pulled card sent the player through every track in the queue (review, 2026-09-25). */
void MusicFeed::readError() {
  nReadErrors++;
  readHeld = true;                    // one slow failing read a pass is enough
  readReopen = true;                  // FatFs latched the error on this handle
  if (++readFails >= MUSIC_READ_TRIES) {
    readFailed = true;
    why = "Card read failed";
  }
}

bool MusicFeed::readAgain() {
  if (readFailed || readHeld) {
    return false;
  }
  if (readReopen) {
    readReopen = false;
    readSeek = false;
    if (!src->reopen(srcPos)) {
      readError();
      return false;
    }
  } else if (readSeek) {
    readSeek = false;
    if (!src->seek(srcPos)) {
      readError();
      return false;
    }
  }
  return true;
}

bool MusicFeed::topUp() {
  if (srcEof || !src || !readAgain()) {
    return false;
  }
  size_t room = mp3 ? mp3->space() : 0;
  if (room > MUSIC_SCRATCH_BYTES) {
    room = MUSIC_SCRATCH_BYTES;
  }
  if (room == 0) {
    return false;
  }
  const int n = src->read(scratch, room);
  nReads++;
  if (n <= 0) {
    if (srcPos >= srcSize) {
      srcEof = true;
    } else {
      readError();
    }
    return false;
  }
  readFails = 0;
  readBytes += (uint32_t)n;
  srcPos += (uint32_t)n;
  mp3->fill(scratch, (size_t)n);
  return true;
}

void MusicFeed::stage(const int16_t* in, size_t frames, int ch) {
  if (stageLeft == 0) {
    stageOff = 0;
  }
  int16_t* out = stageBuf + stageOff + stageLeft;
  const size_t n = pcm.convert(in, frames, ch, out, work);
  if (swapPairs) {
    for (size_t k = 0; k + 1 < n; k += 2) {
      const int16_t t = out[k];
      out[k] = out[k + 1];
      out[k + 1] = t;
    }
  }
  stageLeft += n;
  outStaged += n;
}

bool MusicFeed::openMp3(MusicSource* s, Mp3Stream* m, int16_t* decBuf, uint32_t audioStart, uint32_t startAt) {
  reset();
  if (!scratch || !stageBuf || !work) {
    why = "No memory for music";
    return false;
  }
  if (!s || !m || !decBuf) {
    why = "No file";
    return false;
  }
  src = s;
  mp3 = m;
  dec = decBuf;
  const uint32_t size = src->size();
  srcSize = size;
  const bool mid = startAt > audioStart && startAt < size;
  srcPos = mid ? startAt : audioStart;
  if (!src->seek(srcPos)) {
    why = "Cannot seek";
    return false;
  }
  mp3->reset();
  if (!probeMp3(mid)) {
    if (!why) {
      why = "Not playable audio";
    }
    return false;
  }
  kind = MP3;
  return true;
}

/* Decode until TWO frames in a row agree on rate and channels, and only then believe it. */
bool MusicFeed::probeMp3(bool mid) {
  int candHz = 0, candCh = 0;
  bool firstCand = true;
  for (int tries = 0; tries < 96; tries++) {
    if (!srcEof && mp3->buffered() < MUSIC_READ_BELOW_BYTES) {
      topUp();
    }
    Mp3Info info;
    memset(&info, 0, sizeof(info));
    const int rc = mp3->decode(dec, &info);
    chargeDecode(rc, rc > 0 ? (size_t)rc : 0);
    if (rc > 0) {
      const uint32_t off = srcPos - (uint32_t)mp3->buffered() - (uint32_t)mp3->lastFrameBytes();
      if (info.channels < 1 || info.channels > 2) {
        nSkipped++;
        continue;
      }
      if (candHz && info.sampleRate == candHz && info.channels == candCh) {
        remember(off);
        srcHz = (uint32_t)candHz;
        srcCh = (uint8_t)candCh;
        outHz = pcm.halving() ? srcHz / 2 : srcHz;
        stage(dec, (size_t)rc / candCh, candCh);
        nUnits++;
        return true;
      }
      /* A first format, or a different one: whatever was staged for the old candidate goes.
       * ⚠ The FIRST frame of a track is real audio and is kept (dropping it clips every
       * track's start). After a mid-file start, or after a candidate that did not hold, the
       * first frame is the overlap transient and is not. */
      stageOff = stageLeft = 0;
      outStaged = 0;
      placeHead = placeCount = 0;
      pcm.begin(info.sampleRate >= MUSIC_HALF_RATE_FROM_HZ);
      candHz = info.sampleRate;
      candCh = info.channels;
      remember(off);
      if (!mid && firstCand) {
        stage(dec, (size_t)rc / candCh, candCh);
        nUnits++;
      }
      firstCand = false;
      continue;
    }
    if (rc == 0) {
      if (srcEof) {
        if (candHz) {
          /* A file of exactly one frame. Nothing to compare with; it plays as it decoded. */
          srcHz = (uint32_t)candHz;
          srcCh = (uint8_t)candCh;
          outHz = pcm.halving() ? srcHz / 2 : srcHz;
          return true;
        }
        why = "Not playable audio";
        return false;
      }
      if (!topUp() && !srcEof) {
        why = "Card read failed";     // at the open: no retry, the player shows why
        return false;
      }
      if (readBytes > 256u * 1024u) {
        why = "No audio found";       // a quarter megabyte without two agreeing frames
        return false;
      }
      continue;
    }
    if (rc == MP3_SKIPPED_RESERVOIR) {
      nReservoir++;
      remember(srcPos - (uint32_t)mp3->buffered() - (uint32_t)mp3->lastFrameBytes());
    } else {
      nSkipped++;
    }
  }
  why = "Not playable audio";
  return false;
}

bool MusicFeed::openWav(MusicSource* s, const WavInfo& info, int16_t* decBuf, uint32_t startAt) {
  reset();
  if (!scratch || !stageBuf || !work) {
    why = "No memory for music";
    return false;
  }
  if (!s || !decBuf || !info.ok) {
    why = info.problem ? info.problem : "Not playable audio";
    return false;
  }
  src = s;
  dec = decBuf;
  wav = info;
  const uint32_t fb = wavFrameBytes(wav);
  if (fb == 0 || wav.sampleRate == 0) {
    why = "Not playable audio";
    return false;
  }
  /* ⚠ dataBytes comes straight from the file and a WAV written to a pipe carries 0xFFFFFFFF
   * or 0 there. Clamp to what the file actually holds, or playback runs off the end into
   * whatever the SD driver returns. */
  const uint32_t size = src->size();
  const uint32_t avail = size > wav.dataOffset ? size - wav.dataOffset : 0;
  if (wav.dataBytes == 0 || wav.dataBytes > avail) {
    wav.dataBytes = avail;
  }
  uint32_t start = wav.dataOffset;
  if (startAt > start && startAt < start + wav.dataBytes) {
    start = startAt - ((startAt - wav.dataOffset) % fb);   // a whole frame, or the channels swap
  }
  if (!src->seek(start)) {
    why = "Cannot seek";
    return false;
  }
  srcPos = start;
  srcSize = wav.dataOffset + wav.dataBytes;
  wavLeft = wav.dataBytes - (start - wav.dataOffset);
  wavStartFrame = (start - wav.dataOffset) / fb;
  wavConv.begin(wav, wav.sampleRate, false);       // mono at the file's own rate
  srcHz = wav.sampleRate;
  srcCh = (uint8_t)wav.channels;
  pcm.begin(srcHz >= MUSIC_HALF_RATE_FROM_HZ);
  outHz = pcm.halving() ? srcHz / 2 : srcHz;
  kind = WAV;
  return true;
}

bool MusicFeed::produceMp3() {
  for (int tries = 0; tries < 16; tries++) {
    if (!srcEof && mp3->buffered() < MUSIC_READ_BELOW_BYTES) {
      topUp();
    }
    Mp3Info info;
    memset(&info, 0, sizeof(info));
    const int rc = mp3->decode(dec, &info);
    chargeDecode(rc, rc > 0 ? (size_t)rc : 0);
    if (rc > 0) {
      remember(srcPos - (uint32_t)mp3->buffered() - (uint32_t)mp3->lastFrameBytes());
      /* The ring runs at the rate the opening agreed on. A frame at any other rate is a false
       * sync inside the stream (or a file spliced from two); played, it would be noise at the
       * wrong speed. Channels may change (joint/mono frames): the output is mono anyway. */
      if ((uint32_t)info.sampleRate != srcHz || info.channels < 1 || info.channels > 2) {
        nSkipped++;
        continue;
      }
      stage(dec, (size_t)rc / info.channels, info.channels);
      nUnits++;
      return true;
    }
    if (rc == 0) {
      if (srcEof) {
        decodeEof = true;             // the tail of a file: a partial frame, a tag
        return false;
      }
      if (!topUp() && !srcEof) {
        return false;                 // a failed read: try again next pass (readError())
      }
      continue;
    }
    if (rc == MP3_SKIPPED_RESERVOIR) {
      nReservoir++;
      remember(srcPos - (uint32_t)mp3->buffered() - (uint32_t)mp3->lastFrameBytes());
    } else {
      nSkipped++;
    }
  }
  return false;                       // a run of damaged frames: try again next pass
}

bool MusicFeed::produceWav() {
  const uint32_t fb = wavFrameBytes(wav);
  if (fb == 0 || wavLeft < fb * 4) {
    decodeEof = true;
    return false;
  }
  uint32_t maxFrames = MUSIC_SCRATCH_BYTES / fb;
  if (maxFrames > MUSIC_UNIT_FRAMES) {
    maxFrames = MUSIC_UNIT_FRAMES;
  }
  uint32_t frames = wavLeft / fb;
  if (frames > maxFrames) {
    frames = maxFrames;
  }
  frames &= ~3u;                      // halving and the pair swap both want even counts
  if (!readAgain()) {
    return false;
  }
  const int n = src->read(scratch, frames * fb);
  nReads++;
  /* wavLeft >= 4 frames here and is clamped to what the file holds, so a read that brings back
   * fewer than 4 frames is an ERROR, not the end (see readError()). And only whole groups of 4
   * frames are consumed: a short read's odd bytes are read again (readSeek), or every sample
   * after them would be off by a channel or a byte — noise for the rest of the track. */
  const uint32_t got = n > 0 ? (((uint32_t)n / fb) & ~3u) : 0;
  if (got == 0) {
    readError();
    return false;
  }
  readFails = 0;
  const uint32_t take = got * fb;
  if ((uint32_t)n != take) {
    readSeek = true;
  }
  readBytes += (uint32_t)n;
  srcPos += take;
  wavLeft -= take < wavLeft ? take : wavLeft;
  /* 🛑 WHOLE GROUPS OF FOUR SAMPLES INTO stage(), THE REST CARRIED TO THE NEXT UNIT. The
   * converter lags one frame (its first call gives got - 1), and this used to stage `out & ~1`:
   * one sample of every track and every resume thrown away, and at half rate an ODD number of
   * output samples from that first unit — the mono pair swap is done per unit, so from then on
   * every pair was swapped with the wrong partner: 44.1/48 kHz WAVs played the whole track with
   * the swap inverted, the grit `music swap off` exists to hear (review, 2026-09-25). A multiple
   * of 4 in gives an even count out whether or not it is halved, as every MP3 frame already is. */
  const size_t c = wavCarryN;
  memcpy(dec, wavCarry, c * sizeof(int16_t));
  size_t used = 0;
  const size_t out = wavConv.feed(scratch, got, dec + c, 2400 - c, &used);
  chargeDecode(1, out);
  size_t total = c + out;
  wavCarryN = (uint8_t)(total & 3u);
  total -= wavCarryN;
  memcpy(wavCarry, dec + total, wavCarryN * sizeof(int16_t));
  if (total == 0) {
    return false;
  }
  stage(dec, total, 1);
  nUnits++;
  return true;
}

bool MusicFeed::produce() {
  if (kind == MP3) {
    return produceMp3();
  }
  if (kind == WAV) {
    return produceWav();
  }
  return false;
}

bool MusicFeed::pushStaged(bool* full) {
  const size_t n = sink->write(stageBuf + stageOff, stageLeft);
  lead.wrote((uint32_t)n);
  outWritten += n;
  stageOff += n;
  stageLeft -= n;
  if (stageLeft > 0) {
    lead.full(sink->nowUs());
    *full = true;
    return false;
  }
  return true;
}

static const int16_t kZeros[256] = {0};

uint32_t MusicFeed::ringPos() const {
  const uint32_t len = lead.bufSamples();
  return len ? (uint32_t)((lead.writtenTotal() - ringBase) % len) : 0;
}

void MusicFeed::closeRing() {
  /* 🛑 LEAVE THE DRIVER'S CURRENT BUFFER FULL WHEN MUSIC STOPS. IDF 3.3's writer keeps a
   * half-filled buffer as `curr_ptr`; left like that, the ring cycles on and the ISR, finding
   * its free queue full, pushes that same buffer to the queue while the writer still holds it.
   * The next writer then fills it twice over and the next track's first half second plays
   * OUT OF ORDER (tests/test_musicfeed.cpp, "pause and resume on the same ring"). Topping it
   * up to the boundary with zeros is at most 511 samples into room that is already there, so
   * it never waits, and a full curr_ptr is the state the driver handles cleanly. */
  if (kind == NONE || !sink) {
    return;
  }
  const uint32_t len = lead.bufSamples();
  uint32_t left = len ? (len - ringPos()) % len : 0;
  while (left > 0) {
    const size_t want = left > 256 ? 256 : left;
    const size_t n = sink->write(kZeros, want);
    lead.wrote((uint32_t)n);
    if (n < want) {
      break;                          // cannot happen: the room is in the buffer it holds
    }
    left -= (uint32_t)n;
  }
}

void MusicFeed::padSilence() {
  /* A little silence after the last sample, then ended() once the tail has CERTAINLY played.
   *
   * ⚠ NOT A WHOLE RING OF IT. The next track's samples queue behind whatever is in the ring, so
   * a full ring of zeros is a 0.56 s gap between every two tracks. And NOT NONE: with nobody
   * writing, IDF 3.3 keeps cycling the ring, and its tx_desc_auto_clear zeroes a buffer only as
   * the DMA enters it — after the DMA has already fetched the start of it into the FIFO, so the
   * tail would come back as a few-ms snippet at every buffer boundary. MUSIC_END_SILENCE_BUFS of
   * zeros (~116 ms at 22.05 kHz) cover the time from "the tail has played" to the next track's
   * ceasePlayback() (which zeroes the whole ring) with a pass or two to spare. */
  if (zeroTarget == 0) {
    /* ...ending on a buffer boundary, for the reason on closeRing(). */
    const uint32_t len = lead.bufSamples() ? lead.bufSamples() : 1;
    zeroTarget = (uint32_t)MUSIC_END_SILENCE_BUFS * len + (len - ringPos()) % len;
    if (zeroTarget > lead.cap()) {
      zeroTarget = lead.cap();
    }
  }
  const uint32_t target = zeroTarget;
  while (zerosAfter < target) {
    size_t want = target - zerosAfter;
    if (want > 256) {
      want = 256;
    }
    const size_t n = sink->write(kZeros, want);
    lead.wrote((uint32_t)n);
    zerosAfter += (uint32_t)n;
    if (n < want) {
      lead.full(sink->nowUs());
      break;
    }
  }
  /* Everything still in the ring is at most `hi`; when that is no more than the silence we
   * wrote after the last sample, the last sample has played. */
  if (zerosAfter >= target && lead.hi(sink->nowUs()) <= (int32_t)zerosAfter) {
    isEnded = true;
  }
}

void MusicFeed::run(int units) {
  bool full = false;
  for (;;) {
    if (stageLeft > 0 && !pushStaged(&full)) {
      break;                          // the ring is full: we are ahead, which is the goal
    }
    if (decodeEof) {
      padSilence();
      break;
    }
    if (units <= 0) {
      break;
    }
    units--;
    if (!produce()) {
      if (decodeEof) {
        continue;                     // go and pad
      }
      break;
    }
  }
  lead.normalize(sink->nowUs());
}

void MusicFeed::start(MusicSink* s, const MusicDma& dma) {
  if (kind == NONE || !s) {
    return;
  }
  sink = s;
  const uint64_t now = sink->nowUs();
  lead.begin(dma, now);
  dmaGen = dma.gen;
  /* The driver's current buffer is full at a start: freshly installed (none held), or the
   * last music stop topped it up (closeRing()). So the ring position counts from here. */
  ringBase = lead.writtenTotal();
  /* The prefill: decode until the ring refuses. ~12-24 frames at 160 MHz is the "loading"
   * pause at a track start that {user} allowed, and it means the first long pass after pressing
   * play (the screen repaint that follows the key) cannot run the ring dry. */
  run(MUSIC_PREFILL_UNITS);
  lastPassUs = sink->nowUs();
}

/* The budget: normally a few frames, so a pass that finds the ring hungry does not hold the loop
 * for long; more when the ring is low, so a stall is recovered from in a pass or two. It is the
 * same on EVERY pass, the one that follows a long one included: see "...AND THE CATCH-UP STAYS
 * ON THE PASS AFTER A LONG ONE" in music_feed.h before making `gapUs` count for anything. */
int MusicFeed::budgetFor(int32_t lo, uint32_t cap, uint64_t gapUs) {
  (void)gapUs;
  const int64_t lowWater = (int64_t)cap * MUSIC_CATCH_UP_BELOW_PCT / 100;
  return (int64_t)lo < lowWater ? MUSIC_UNITS_CATCH_UP : MUSIC_UNITS_PER_PASS;
}

void MusicFeed::pass(const MusicDma& dma) {
  if (kind == NONE || !sink || readFailed) {
    return;
  }
  readHeld = false;                   // a failed read may be tried again, once, this pass
  const uint64_t now = sink->nowUs();
  if (dma.gen != dmaGen) {
    lead.begin(dma, now);             // someone reinstalled I2S under us: the lead is unknown
    dmaGen = dma.gen;
    ringBase = lead.writtenTotal();   // (a new install holds no half buffer)
  }
  /* How long the rest of the loop held music since this feed's last pass ended. */
  const uint64_t gap = (lastPassUs && now > lastPassUs) ? now - lastPassUs : 0;
  if (gap > maxGapUs) {
    maxGapUs = gap;
  }
  nPasses++;
  if (isEnded) {
    lastPassUs = now;
    return;
  }
  lead.check(now);
  run(budgetFor(lead.lo(now), lead.cap(), gap));
  const uint64_t done = sink->nowUs();
  const uint64_t work = done > now ? done - now : 0;
  sumWorkUs += work;
  if (work > maxWorkUs) {
    maxWorkUs = work;
  }
  lastPassUs = done;
}

uint32_t MusicFeed::playingPos(uint64_t nowUs) const {
  if (kind == NONE) {
    return srcPos;
  }
  int64_t mid = ((int64_t)lead.lo(nowUs) + (int64_t)lead.hi(nowUs)) / 2;
  if (mid < 0) {
    mid = 0;
  }
  mid -= zerosAfter;                  // silence after the last sample is not the track
  if (mid < 0) {
    mid = 0;
  }
  int64_t playing = (int64_t)outWritten - mid;
  if (playing < 0) {
    playing = 0;
  }
  if (kind == WAV) {
    const uint32_t fb = wavFrameBytes(wav);
    const uint64_t srcFrames = (uint64_t)playing * (pcm.halving() ? 2 : 1);
    uint64_t off = (uint64_t)wav.dataOffset + ((uint64_t)wavStartFrame + srcFrames) * fb;
    const uint64_t end = (uint64_t)wav.dataOffset + wav.dataBytes;
    if (off > end) {
      off = end;
    }
    return (uint32_t)off;
  }
  if (placeCount == 0) {
    return srcPos;
  }
  /* Newest place whose output starts at or before the sample playing now... */
  int k = placeCount - 1;             // default: the oldest there is
  for (int i = 0; i < placeCount; i++) {
    const int idx = (placeHead + MUSIC_PLACES - 1 - i) % MUSIC_PLACES;
    if ((int64_t)places[idx].out <= playing) {
      k = i;
      break;
    }
  }
  /* ...and two frames before it: the first after a seek is stepped over for its reservoir or
   * dropped as the overlap transient, so the frame that was playing is the first one heard. */
  k += 2;
  if (k > placeCount - 1) {
    k = placeCount - 1;
  }
  return places[(placeHead + MUSIC_PLACES - 1 - k) % MUSIC_PLACES].off;
}

void MusicFeed::stats(MusicStats* o, uint64_t nowUs) const {
  if (!o) {
    return;
  }
  memset(o, 0, sizeof(*o));
  const uint32_t r = lead.rate() ? lead.rate() : 1;
  o->drops = lead.drops;
  o->dropMs = (uint32_t)(lead.dryUs / 1000);
  int64_t mid = ((int64_t)lead.lo(nowUs) + (int64_t)lead.hi(nowUs)) / 2;
  if (mid < 0) {
    mid = 0;
  }
  o->leadMs = (int32_t)(mid * 1000 / r);
  o->capMs = (uint32_t)((uint64_t)lead.cap() * 1000 / r);
  o->minLeadMs = lead.minLo == 0x7fffffff ? o->leadMs : (int32_t)((int64_t)lead.minLo * 1000 / r);
  o->units = nUnits;
  o->skipped = nSkipped;
  o->reservoir = nReservoir;
  o->reads = nReads;
  o->readErrors = nReadErrors;
  o->readKB = (uint32_t)(readBytes / 1024);
  o->inBuf = (kind == MP3 && mp3) ? (uint32_t)mp3->buffered() : 0;
  o->passes = nPasses;
  o->maxGapMs = (uint32_t)(maxGapUs / 1000);
  o->maxWorkUs = (uint32_t)maxWorkUs;
  o->avgWorkUs = nPasses ? (uint32_t)(sumWorkUs / nPasses) : 0;
  o->srcHz = srcHz;
  o->outHz = outHz;
  o->srcCh = srcCh;
  o->swap = swapPairs;
  o->ended = isEnded;
}

void MusicFeed::resetStats(uint64_t nowUs) {
  (void)nowUs;
  lead.drops = 0;
  lead.dryUs = 0;
  lead.minLo = 0x7fffffff;
  nUnits = nSkipped = nReservoir = nReads = nPasses = nReadErrors = 0;
  readBytes = sumWorkUs = maxGapUs = maxWorkUs = 0;
  lastPassUs = 0;                     // the next gap is measured from the next pass
}
