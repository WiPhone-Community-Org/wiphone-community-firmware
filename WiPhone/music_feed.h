/*
 * music_feed.h — how music gets from the card to the DAC without gaps, and an honest count
 * of the times it did not.
 *
 * Arduino-free, like mp3_stream and wav_reader: tests/test_musicfeed.cpp compiles THIS file,
 * the real helix decoder and the real Mp3Stream on the Mac and drives them against a model of
 * ESP-IDF 3.3's I2S DMA with simulated loop passes. Audio::loop() calls pass() once per main-
 * loop pass; Audio supplies the file (MusicSource) and the DMA (MusicSink). Nothing here
 * touches a peripheral.
 *
 * ── WHAT WAS WRONG (0.9.78 and before, measured 2026-09-25) ─────────────────────────────────
 *
 *  1. "gaps" WAS ARITHMETIC, NOT A DROPOUT METER. The feed read at most 512 bytes per call and
 *     ended the pass the first time a decode lacked input. A 192 kbps / 44.1 kHz frame is 627
 *     bytes, so the stream HAD to take (24,000 - 38.28 x 512) / 512 = 8.6 extra input-limited
 *     passes a second, each one "starved and never refused" = +1 gap. Phone 2 read 8-9/s on
 *     every 192k file at 160 MHz AND 240 MHz, and 0.9.78 read 167 in 19 s: the Mac replay of
 *     the same file reads 163. With ZERO true dropouts at passes up to 30 ms. It was also blind
 *     the other way: Hells_Bells (128k) read 0 gaps through screenshots that each stalled the
 *     loop 4.2 s and certainly emptied the DMA.
 *  2. THE REAL DROPOUTS: any loop pass longer than what the DMA held. It was 4 x 1023 stereo
 *     frames at 44.1 kHz, and the IDF 3.3 driver only ever lets a writer get dma_buf_count - 1
 *     buffers ahead (its free queue is xQueueCreate(dma_buf_count - 1)), so ~70 ms. The once-a-
 *     second Now Playing repaint, a map frame (~77 ms), a Files slice (80 ms), a mesh DB save
 *     (~0.1 s on the card) each ate it. The LOOP STALL line only fires above 250 ms.
 *  3. When the ring does run dry, IDF 3.3 mostly REPLAYS old buffers rather than playing
 *     silence (tx_desc_auto_clear only zeroes a buffer when its free queue is full, and a
 *     writer holding a half-filled buffer keeps it from ever being full) — so a dropout sounds
 *     like a stutter, not a click.
 *
 * ── THE DESIGN: MORE SECONDS OF RING PER BYTE OF INTERNAL RAM, FED BY WHOLE FRAMES ──────────
 *
 * The DMA ring is the only slack music has (the loop is the only thing that writes I2S; a
 * PSRAM buffer would sit idle through the very stall it was meant to cover), and it must live
 * in INTERNAL, DMA-capable RAM, the resource whose exhaustion panics this phone. So the ring is
 * made to hold more TIME, not more bytes. {user}, 2026-09-25: "Lower audio quality and having to
 * take time to load in-between tracks are things I can live with."
 *   - MONO: (L+R)/2. Half the bytes per sample. The loudspeaker is one speaker anyway.
 *   - HALF RATE for 44.1/48 kHz files (22.05/24 kHz out) through a 23-tap half-band filter
 *     (flat to 8 kHz, -0.9 dB at 9 kHz, -28 dB above 13.7 kHz, -63 dB above 15.4 kHz). Half the
 *     samples per second. Files at 32 kHz and below play at their own rate.
 *   - 24 buffers x 512 samples = 24 KB, TX ONLY (music never reads the microphone). 23 usable
 *     = 534 ms at 22.05 kHz, 490 ms at 24 kHz, 368 ms at 32 kHz — against ~70 ms before. For
 *     scale: the boot install the phone idles with is 4 x 4092 B TX + the same RX = 32 KB, so
 *     this is 8 KB LESS than that; the mono install a pop or a call leaves is 16 KB (so 8 KB
 *     more than loudspeaker music took before). Each buffer is its own 1 KB allocation, which
 *     fits into holes a 4 KB one cannot.
 *   - WHOLE FRAMES: a pass never ends for lack of bytes. The compressed buffer is topped up to
 *     4 KB (in one read) whenever it falls under 2 KB — at least one of the largest possible
 *     frames (1,441 B) is always there.
 *   - A BUDGET per pass (4 frames, 8 when the ring is under 80%), so music never holds the loop
 *     for long: at most ~37 ms a pass including the card reads, ~73 ms catching up after a stall
 *     (the replay's costs: decode 7.65 ms a frame at 160 MHz, a card read 330 us + 1.95 us/B).
 *     Normally a pass does one frame or none. ⚠ The 80% is load-bearing (review, 2026-09-25):
 *     at LONG passes (a map pan is ~77 ms, a Files slice 80 ms) the pass plus its own decode
 *     needs ~4.2-4.8 frames, so 4 cannot keep up and the ring SETTLES at the catch-up mark. At
 *     40% that left ~200 ms of lead and any extra ~200 ms stall ran it dry (the replay: 4-5
 *     dropouts in 180 s at 77 ms + 300 ms every 10 s); at 80% it settles at ~430 ms and the same
 *     runs are clean. The work per second is set by the audio rate either way; only the level
 *     the ring settles at moves. (120 ms passes at 48 kHz still lose: 8 frames a pass is the CPU.)
 *   - ...AND THE CATCH-UP STAYS ON THE PASS AFTER A LONG ONE. 🛑 Tried and rejected in review
 *     2026-09-25 (c20ccbf, never flashed). Phone 2, Pulse.mp3 (192k) under `maps hold` pans:
 *     drops=0, minLead 265 ms, but ~10 LOOP STALLs of 274-301 ms in 18 s — a tile's last piece
 *     + the redraw (~190-210 ms), then this pass's 8-frame catch-up (88-94 ms). Deferring it (ONE
 *     frame on a pass that followed >= 150 ms while the ring held half; the other seven repaid,
 *     up to 12 a pass, by the passes after) cleared every such line in the host model. It was
 *     wrong twice over:
 *       - IT DROPS OUT WHERE THIS RULE DOES NOT. Nothing runs between a long pass and whatever
 *         comes right after it, so a long pass that decodes less leaves the next stall less
 *         audio. 77 ms passes, a 210 ms pass finding 324 ms queued, then 377 ms: this rule ends
 *         the long pass at 460 ms queued and the 377 leaves 83 ms; the deferral ended it at 337
 *         and ran dry. Of the suite's 324 "long pass, then a 300-450 ms stall" cases (22.05, 24
 *         and 32 kHz out, both decode costs) this rule is clean in 219 and the deferral lost 63
 *         of those, winning none; the review's wider sweep also lost 12 of 74 map pans with one
 *         extra 300-400 ms stall every ~15 s. ANY rule that decodes less on the long pass has a
 *         window of stalls it loses and this one survives; deferring fewer frames only narrows
 *         it (4, 5 or 6 frames on the long pass still fail the suite's envelope at 9.9 ms/frame).
 *       - IT HID NOTHING FROM THE EYE. Music is fed from Audio::loop() at the BOTTOM of loop()
 *         ("audio-loop"), AFTER "redraw" has pushed the frame. So a LOOP STALL line with music
 *         under the map is the heavy frame, already on screen, PLUS the catch-up that follows
 *         it — not a freeze of that length. What the eye gets is push-to-push time: the previous
 *         pass's music + this pass's work. In the model's pans the longest of those was the SAME
 *         under both rules (the heavy frame itself, 219-301 ms), and the worst frame AFTER it
 *         got longer under the deferral, because the repay lands there (a 77 ms pan: 150 -> 187
 *         ms at 7.65 ms/frame, 168 -> 214 at the phone's 9.9). Only two heavy passes back to
 *         back gained (263 -> 219 ms), for minLead 292 -> 175.
 *     So a LOOP STALL line naming 'redraw' while music plays under the map is EXPECTED and is
 *     not the budget's to fix. What would move the pan: a lighter heavy frame (the map's tile
 *     pieces and redraw), or decoding while the loop is blocked (a task of its own, which must
 *     share the card and the SPI bus with the map and the LCD — a bench job, not a rule).
 *     tests/test_musicfeed.cpp ("a long pass, then a stall" and "a map pan") keeps both
 *     findings, with that rule replayed through budgetFor().
 *   - tests/test_musicfeed.cpp, with the real decoder on a 192 kbps-shaped stream against a model
 *     of the IDF 3.3 DMA: zero true dropouts at passes of 5-80 ms, at 10 ms + a 100 ms stall
 *     every second, 300 ms every 10 s and 450 ms every 5 s, and at 77 or 100 ms passes + 300 ms
 *     every 10 s (the old feed: 4.8/s at 50 ms passes, 1.1/s at the 100 ms stall). This is the design the host replay called "F4". (A decode task
 *     was the other candidate: it is not needed for CPU — decode is ~30% of one core at 160 MHz —
 *     and it would put a second I2S writer beside the Audio singleton, which every pop, ring,
 *     call and game borrows.)
 *
 * ── THE COUNTER: drops, and why it cannot lie in the direction that matters ─────────────────
 * The IDF 3.3 driver reports no underrun (its optional event queue says only "a buffer
 * finished"). So the lead — how much audio the ring holds — is BOUNDED from both sides with
 * the only two facts the writer has:
 *   - a write that comes back short means the ring is full RIGHT NOW: every buffer but the one
 *     playing is ours, so the lead is between (bufs-1) x len and bufs x len;
 *   - between writes, the DMA drains exactly `rate` samples a second (the APLL; it never stops
 *     while I2S runs, dry or not).
 * `drops` counts passes that found the upper bound below zero: the ring CERTAINLY ran dry (no
 * false positives — it can under-count a dropout shorter than one buffer, 23 ms, and one stall
 * is one drop). `minLead` is the lowest LOWER bound seen at a pass start: while it stays above
 * zero there was certainly no dropout at all. `drops > 0` = proven gaps; `minLead > 0` = proven
 * clean; anything between is "possibly a gap shorter than one buffer". tests/test_musicfeed.cpp
 * checks both claims against the model's ground truth, 24,000 times over random passes and
 * stalls. ⚠ AFTER A DROP, `drops` IS ONLY A LOWER BOUND UNTIL THE RING IS NEXT FULL: the upper
 * bound restarts at "full" and only a short write pins it again, so a second dry spell inside
 * that refill shows in minLead, not in drops — and at long passes, where the writer never meets
 * a full ring, that can be the rest of the track. On the bench judge a busy run by minLead > 0,
 * never by drops == 0 alone. Any stall longer than the ring itself (534 ms at 22.05 kHz, 490 ms
 * at 24 kHz) is a dropout by construction: the ring is the only slack a loop-fed design has.
 *
 * ── TWO IDF 3.3 DRIVER HABITS THE REST OF AUDIO.CPP HAS TO ALLOW FOR ─────────────────────────
 *   - A freshly installed ring plays itself (zeros) once before anything written reaches the
 *     DAC: its free-buffer queue starts EMPTY and fills one buffer per EOF. So the first track
 *     after a pop, a call or a game starts after up to half a second of silence.
 *   - i2s_start() restarts the DMA at buffer 0 but keeps the free queue as it was — so after
 *     the idle watchdog's i2s_stop() the writer fills buffers in an order the DMA no longer plays
 *     them in. playMusic() installs a fresh ring then (Audio::configureMusicI2S()).
 */

#ifndef MUSIC_FEED_H
#define MUSIC_FEED_H

#include <stdint.h>
#include <stddef.h>

#include "mp3_stream.h"
#include "wav_reader.h"

/* ── The ring music installs (Audio::configureMusicI2S). Mono 16-bit, TX only. ── */
#define MUSIC_DMA_BUFS           24
#define MUSIC_DMA_BUF_SAMPLES    512      // 1 KB each; IDF caps a buffer at 4092 B
/* Files at or above this rate play at half rate. 32 kHz stays whole (16 kHz would cut at 8). */
#define MUSIC_HALF_RATE_FROM_HZ  44100

/* Frames (MP3) or chunks (WAV) decoded per main-loop pass, normally and while catching up. */
#define MUSIC_UNITS_PER_PASS     4
#define MUSIC_UNITS_CATCH_UP     8
#define MUSIC_CATCH_UP_BELOW_PCT 80       // see "A BUDGET per pass" above: 40 dropped out at map pans
/* At a track start, decode until the ring is full (bounded): the "loading" pause {user} allowed. */
#define MUSIC_PREFILL_UNITS      12
/* Silence written after a track's last sample (see padSilence()): 5 x 512 = 116 ms. */
#define MUSIC_END_SILENCE_BUFS   5

/* The compressed buffer (Mp3Stream, 4 KB) is topped up whenever it holds less than this. */
#define MUSIC_READ_BELOW_BYTES   2048
/* PSRAM bounce buffer every card read lands in. ⚠ ALIGNED ON PURPOSE: FatFs reads whole
 * sectors straight into the caller's buffer, and the SD driver copies the SPI FIFO out as
 * 32-bit words (spiTransferBytesNL), which faults on an unaligned destination. */
#define MUSIC_SCRATCH_BYTES      4096
/* Converted output of up to two units (the opening probe keeps two). */
#define MUSIC_STAGE_SAMPLES      2304
/* One unit of input at most: 1152 frames (an MPEG-1 frame; WAV chunks are cut to fit). */
#define MUSIC_UNIT_FRAMES        1152
/* File offsets of the most recent frames, to resume at the one that was PLAYING. */
#define MUSIC_PLACES             32
/* A card read that FAILS before the end of the file (a FatFs error, a card pulled) is retried on
 * the next pass — through MusicSource::reopen(), because FatFs latches a disk error on the open
 * handle and every later f_read just returns it — at most this many passes running, then the
 * track stops with "Card read failed" and keeps its place (the player makes it a pause; F1
 * tries again). It used to be taken for the END of the file: one transient error skipped the rest
 * of the track, and a pulled card walked the whole queue from the loop (review, 2026-09-25). */
#define MUSIC_READ_TRIES         3

/* The DMA as it is installed right now. `gen` changes on every (re)install or rate change —
 * a different ring means the lead is unknown until the next full write. */
struct MusicDma {
  uint16_t bufs;
  uint16_t len;         // samples per buffer (mono samples here)
  uint32_t rate;        // samples per second it drains
  uint32_t gen;
};

class MusicSource {
public:
  virtual ~MusicSource() {}
  /* Bytes read. 0 or < 0 is the end ONLY where the file ends (pos >= size()); anywhere else it
   * is an error — Arduino's File::read() returns 0 for both. */
  virtual int      read(uint8_t* dst, size_t n) = 0;
  virtual bool     seek(uint32_t pos) = 0;
  virtual uint32_t size() = 0;
  /* After a failed read: get a handle that can read again, positioned at `pos`. The phone's
   * source closes and reopens the file (FatFs latches the error on the old one). */
  virtual bool     reopen(uint32_t pos) { return seek(pos); }
};

class MusicSink {
public:
  virtual ~MusicSink() {}
  /* NON-BLOCKING. Returns samples accepted; fewer than asked means the ring is full. */
  virtual size_t   write(const int16_t* samples, size_t n) = 0;
  virtual uint64_t nowUs() = 0;
};

/* ── The lead, bounded. See the note at the top of this file. ── */
class MusicLead {
public:
  void begin(const MusicDma& d, uint64_t nowUs);
  void wrote(uint32_t n) { written += n; }
  /* A write came back short: the ring is full now. */
  void full(uint64_t nowUs);
  /* At the start of a pass, before any write. True when the ring CERTAINLY ran dry since the
   * last pass; the drop is counted and the bounds restart from "empty". */
  bool check(uint64_t nowUs);
  /* After a pass's writes: no bound may exceed what the ring can physically hold. */
  void normalize(uint64_t nowUs);
  int32_t  lo(uint64_t nowUs) const;      // samples; can be negative (possibly dry)
  int32_t  hi(uint64_t nowUs) const;      // samples
  uint32_t cap() const { return capS; }
  uint32_t bufSamples() const { return bufLen; }
  uint32_t rate() const { return r; }
  uint64_t writtenTotal() const { return written; }

  uint32_t drops = 0;
  uint64_t dryUs = 0;                     // certain dry time, summed (a lower bound)
  int32_t  minLo = 0x7fffffff;            // lowest lower bound at a pass start, samples
private:
  int64_t  raw(int32_t calVal, uint64_t nowUs, bool low) const;
  uint32_t capS = 0, bufLen = 0, r = 1;
  uint64_t written = 0;
  uint64_t calUs = 0, calW = 0;
  int32_t  calLo = 0, calHi = 0;
};

/* ── Mono, and half rate through a half-band filter. Stateful across units. ── */
class MusicPcm {
public:
  static const int TAPS = 23;
  static const int HIST = TAPS - 1;
  void begin(bool halfRate);
  bool halving() const { return half; }
  /* `frames` interleaved frames of `ch` (1 or 2) channels in; mono samples out, appended at
   * `out`. `work` needs HIST + frames shorts. `frames` must be even. Returns samples written. */
  size_t convert(const int16_t* in, size_t frames, int ch, int16_t* out, int16_t* work);
private:
  bool     half = false;
  uint8_t  phase = 1;
  int16_t  hist[HIST];
};

struct MusicStats {
  uint32_t drops;         // passes that found the ring CERTAINLY dry
  uint32_t dropMs;        // certain dry time, summed (a lower bound)
  int32_t  minLeadMs;     // lowest lower-bound lead at a pass start; > 0 = certainly no gap
  int32_t  leadMs;        // lead now (middle of the bounds)
  uint32_t capMs;         // what the ring holds when full
  uint32_t units;         // frames (MP3) / chunks (WAV) staged
  uint32_t skipped;       // corrupt frames and frames at the wrong rate stepped over
  uint32_t reservoir;     // frames stepped over for a missing bit reservoir (after a seek)
  uint32_t reads;         // card reads
  uint32_t readErrors;    // of those, reads that failed before the end of the file
  uint32_t readKB;
  uint32_t inBuf;         // compressed bytes waiting
  uint32_t passes;
  uint32_t maxGapMs;      // longest time between two passes while playing: the loop's worst
  uint32_t maxWorkUs;     // longest time one pass spent here
  uint32_t avgWorkUs;
  uint32_t srcHz, outHz;
  uint8_t  srcCh;
  bool     swap;
  bool     ended;
};

class MusicFeed {
public:
  enum Kind : uint8_t { NONE = 0, MP3 = 1, WAV = 2 };

  MusicFeed();
  virtual ~MusicFeed();
  /* The PSRAM buffers, once (~11 KB). False = no PSRAM; music refuses to play. */
  bool begin();

  /* Opening reads, decodes and converts the first frames; no I2S is needed yet.
   * MP3: `audioStart` is the first byte after the ID3 tag; `startAt` a place to resume (0 or
   * <= audioStart = the start). The format is believed only when TWO decodes in a row agree
   * on it (a false sync decodes cleanly at 16/22.05/24/32/48 kHz on ~1% of resumes, and the
   * whole track then played at 0.36x-1.09x speed). After a mid-file start the first decoded
   * frame is DROPPED: it is the IMDCT overlap transient, louder than the music. */
  bool openMp3(MusicSource* src, Mp3Stream* mp3, int16_t* dec, uint32_t audioStart, uint32_t startAt);
  bool openWav(MusicSource* src, const WavInfo& info, int16_t* dec, uint32_t startAt);
  uint32_t outRate() const { return outHz; }
  const char* problem() const { return why; }

  /* The ring is installed (at outRate(), mono): fill it (the prefill) and start counting. */
  void start(MusicSink* sink, const MusicDma& dma);
  /* One main-loop pass. Never waits. */
  void pass(const MusicDma& dma);
  /* The file is done and its last sample has CERTAINLY played (a little silence is queued
   * behind it; the next track's ceasePlayback() zeroes the ring). */
  bool ended() const { return kind != NONE && isEnded; }
  /* The card would not read MUSIC_READ_TRIES passes running, before the end of the file. The
   * feed does nothing more; the caller stops the track keeping its place (problem() says why).
   * NOT ended(): the rest of the track is still on the card. */
  bool failed() const { return kind != NONE && readFailed; }
  bool active() const { return kind != NONE; }
  /* Music is stopping (a pause, a pop, the next track): top the driver's half-filled buffer
   * up to its boundary with silence. Never waits. See the 🛑 note on it. Call before stop(). */
  void closeRing();
  void stop() { kind = NONE; sink = 0; }

  /* The file offset to resume at: two frames before the one PLAYING now (the reservoir and
   * the overlap need them), not the read position, which runs up to 4 KB + the ring ahead. */
  uint32_t playingPos(uint64_t nowUs) const;

  void stats(MusicStats* out, uint64_t nowUs) const;
  void resetStats(uint64_t nowUs);

  /* ⚠ THE ESP32 MONO I2S WORKAROUND. In 16-bit ONLY_LEFT mode the peripheral sends each pair
   * of samples in the wrong order (esp32.com t=11023); Audio::playChunk() has always swapped
   * pairs for the ringtone, the pop and calls. Music did not need it while it sent stereo.
   * A wrong swap is a grit around fs/2 - f: `music swap off` A/Bs it on the bench. */
  bool swapPairs = true;

protected:
  /* The host suite charges simulated decode time here. Nothing on the phone. */
  virtual void chargeDecode(int rc, size_t samplesOut) { (void)rc; (void)samplesOut; }
  /* The units this pass may decode: 4, or 8 under MUSIC_CATCH_UP_BELOW_PCT. `lo` = the lead's
   * lower bound now; `gapUs` = how long the rest of the loop held music since this feed's last
   * pass (0 = not known), which the rule deliberately ignores. The host suite replays a rejected
   * rule here to prove its tests catch it; nothing on the phone overrides it. */
  virtual int budgetFor(int32_t lo, uint32_t cap, uint64_t gapUs);

private:
  void   reset();
  void   run(int units);
  bool   pushStaged(bool* full);
  void   padSilence();
  bool   produce();
  bool   produceMp3();
  bool   produceWav();
  bool   topUp();
  bool   readAgain();               // the retry after a failed read: false = not this pass
  void   readError();
  void   stage(const int16_t* pcm, size_t frames, int ch);
  void   remember(uint32_t fileOff);
  bool   probeMp3(bool midFile);
  uint32_t ringPos() const;     // where the driver's current buffer is filled to, in samples

  Kind         kind = NONE;
  MusicSource* src = 0;
  MusicSink*   sink = 0;
  Mp3Stream*   mp3 = 0;
  int16_t*     dec = 0;          // decode target: Audio::playDec (internal, already exists)
  uint8_t*     scratch = 0;      // PSRAM, MUSIC_SCRATCH_BYTES, aligned
  int16_t*     stageBuf = 0;     // PSRAM, MUSIC_STAGE_SAMPLES
  int16_t*     work = 0;         // PSRAM, HIST + MUSIC_UNIT_FRAMES
  MusicPcm     pcm;
  MusicLead    lead;
  WavInfo      wav;
  WavConverter wavConv;
  const char*  why = 0;

  uint32_t srcPos = 0;           // file offset after the last read
  uint32_t srcSize = 0;          // the file's size at open: a read that stops short of it failed
  uint8_t  readFails = 0;        // failed reads, one per pass at most, since the last good one
  bool     readFailed = false;   // gave up: see failed()
  bool     readHeld = false;     // a read failed THIS pass: no second slow one until the next
  bool     readReopen = false;   // the next read goes through src->reopen(srcPos) first
  bool     readSeek = false;     // the next read seeks to srcPos first (a partial WAV read)
  uint32_t srcHz = 0, outHz = 0;
  uint8_t  srcCh = 0;
  bool     srcEof = false;       // the card has nothing more
  bool     decodeEof = false;    // the decoder has nothing more
  bool     isEnded = false;
  bool     dropNext = false;
  uint32_t wavLeft = 0;          // WAV payload bytes still to read
  int16_t  wavCarry[3];          // converted WAV samples past the last whole group of 4
  uint8_t  wavCarryN = 0;
  uint32_t wavStartFrame = 0;

  size_t   stageOff = 0, stageLeft = 0;
  uint64_t outStaged = 0;        // samples ever staged (the resume ring's clock)
  uint64_t outWritten = 0;       // of those, accepted by the DMA
  uint32_t zerosAfter = 0;       // silence written after the last sample
  uint32_t zeroTarget = 0;       // how much of it (set when the file ends)
  uint64_t ringBase = 0;         // lead.writtenTotal() when the driver last held no half buffer

  struct Place { uint32_t off; uint64_t out; };
  Place    places[MUSIC_PLACES];
  uint8_t  placeHead = 0, placeCount = 0;

  uint32_t dmaGen = 0;
  uint64_t lastPassUs = 0;
  // stats
  uint32_t nUnits = 0, nSkipped = 0, nReservoir = 0, nReads = 0, nPasses = 0, nReadErrors = 0;
  uint64_t readBytes = 0, sumWorkUs = 0, maxGapUs = 0, maxWorkUs = 0;
};

#endif // MUSIC_FEED_H
