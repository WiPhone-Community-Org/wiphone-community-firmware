/*
 * test_musicfeed.cpp — the music feed, driven by simulated main-loop passes against a model of
 * ESP-IDF 3.3's I2S DMA, with the REAL helix decoder and the REAL Mp3Stream in the loop.
 *
 * What it proves, and why each thing matters on the phone (docs/HANDOFF.md, 2026-09-25):
 *   1. The OLD feed (0.9.78: 512-byte reads, one decode per read, a pass ends when a decode
 *      lacks input, 4 x 1023 stereo at 44.1 kHz) reproduces the phone: ~8.6 "gaps" a second on
 *      a 192 kbps stream with ZERO true dropouts at 10 ms passes (the counter was arithmetic),
 *      and real dropouts at 50 ms passes (which the phone's screen redraw produces).
 *   2. The NEW feed has zero true dropouts on the same stream at every pass length the old one
 *      failed at, and through 100 ms and 300 ms stalls — and when a stall IS longer than the
 *      ring, its `drops` counter agrees with the model's ground truth.
 *   3. The counter's bounds are bounds: across random passes and stalls the lead the model
 *      measures is always inside [lo, hi], and a drop is never counted that did not happen.
 *   4. MAINDATA_UNDERFLOW is stepped over, not re-fed; a resume never decodes a frame twice,
 *      and a false sync cannot choose the output rate.
 *   5. Mono/half-rate conversion: DC exact, 1 kHz passes, 18 kHz is gone, and it is the same
 *      whether fed in one block or frame by frame.
 *   6. The end of a track plays out whole, and nothing but silence plays after it.
 *   7. (review, 2026-09-25) Long passes WITH a stall stay clean (the 80% catch-up mark), every
 *      stall is counted even where the ring is seldom full again (normalize()), a card read that
 *      fails mid-track is an error and not the end of the file, and a WAV's units keep the mono
 *      pair swap on its pairs.
 *   8. (the phone's map-pan bench, 2026-09-25) The catch-up STAYS on the pass after a long one.
 *      A rule that deferred it (c20ccbf: one frame there, the rest repaid by the passes after)
 *      is replayed through budgetFor() to keep the two reasons it was rejected: a long pass
 *      followed right away by a stall runs the ring dry where the shipping rule does not, and in
 *      a map pan the longest push-to-push interval stayed the same while the worst frame after
 *      it got longer — it only moved the catch-up off the LOOP STALL line's pass, because music
 *      is fed AFTER the redraw has pushed the frame.
 *
 * Costs are the ones the replay was calibrated with: decode 7.65 ms/frame (5,103 us measured at
 * 240 MHz x 1.5 for 160 MHz), card read 330 us + 1.95 us/byte (1.33 ms per 512 B vs 1,281 us
 * measured), i2s_write 25 us a call, the old per-sample path 2.5 us a sample.
 *
 * The synthetic streams are real MPEG-1/2 Layer III bitstreams (valid headers, all-zero side
 * info and main data) at exactly the frame sizes of {user}'s files, so helix decodes every frame
 * for real — silence, but with the byte pattern that made 192 kbps fail. No music is in the
 * repo; the optional fixture tests use tests/fixtures/mp3/track.mp3 (gitignored).
 */
#include "../WiPhone/music_feed.h"
#include "../WiPhone/mp3_stream.h"
#include "../WiPhone/wav_reader.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <deque>
#include <vector>

static int g_pass = 0, g_fail = 0;
static const char* g_group = "";

static void group(const char* name) {
  g_group = name;
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char* what) {
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
    printf("  \033[31mFAIL\033[0m %s :: %s\n", g_group, what);
  }
}

static void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("  \033[2m");
  vprintf(fmt, ap);
  printf("\033[0m\n");
  va_end(ap);
}

// ─── synthetic Layer III streams ─────────────────────────────────────────────────────────

struct Stream {
  std::vector<uint8_t> bytes;
  std::vector<uint32_t> frameOff;       // file offset of every frame
  int rate = 0;
};

/* MPEG-1 Layer III, stereo, `kbps` at `rate`, every frame's main_data_begin = `mdb`. The side
 * info and main data are zeros: helix decodes each frame (to silence) exactly as it would a
 * real one, and the frame sizes, padding pattern and reservoir references are real. */
static Stream makeMpeg1(int rate, int kbps, int frames, int mdb = 0) {
  static const int kbpsTab[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  int bIdx = 0;
  for (int i = 1; i < 15; i++) {
    if (kbpsTab[i] == kbps) {
      bIdx = i;
    }
  }
  const int rIdx = rate == 44100 ? 0 : rate == 48000 ? 1 : 2;
  Stream s;
  s.rate = rate;
  const long num = 144L * kbps * 1000;
  long rest = 0;
  for (int f = 0; f < frames; f++) {
    rest += num % rate;
    int pad = 0;
    if (rest >= rate) {
      rest -= rate;
      pad = 1;
    }
    const int len = (int)(num / rate) + pad;
    s.frameOff.push_back((uint32_t)s.bytes.size());
    const size_t at = s.bytes.size();
    s.bytes.resize(at + len, 0);
    uint8_t* p = &s.bytes[at];
    p[0] = 0xFF;
    p[1] = 0xFB;                                         // MPEG-1, Layer III, no CRC
    p[2] = (uint8_t)((bIdx << 4) | (rIdx << 2) | (pad << 1));
    p[3] = 0x00;                                         // stereo
    p[4] = (uint8_t)(mdb >> 1);                          // main_data_begin, 9 bits
    p[5] = (uint8_t)((mdb & 1) << 7);
  }
  return s;
}

static void appendWavHeader(std::vector<uint8_t>& b, int rate, int ch, uint32_t dataBytes) {
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); };
  auto u16 = [&](uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); };
  b.insert(b.end(), {'R', 'I', 'F', 'F'});
  u32(36 + dataBytes);
  b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(16);
  u16(1);
  u16((uint16_t)ch);
  u32((uint32_t)rate);
  u32((uint32_t)(rate * ch * 2));
  u16((uint16_t)(ch * 2));
  u16(16);
  b.insert(b.end(), {'d', 'a', 't', 'a'});
  u32(dataBytes);
}

// ─── the IDF 3.3 I2S DMA ─────────────────────────────────────────────────────────────────
/* driver/i2s.c, release/v3.3: `bufs` buffers of `blen` frames played in a hardware ring with
 * an EOF interrupt per buffer; a free-buffer queue of bufs - 1 entries, EMPTY at install; the
 * ISR pushes the finished buffer, first popping (and, with tx_desc_auto_clear, zeroing) the
 * oldest entry if the queue is full; i2s_write(timeout 0) fills curr_ptr, takes the next free
 * buffer when it is full, and returns short when the queue is empty. Every slot carries the
 * sequence number of the frame written into it, so what PLAYS is judged frame by frame. */
struct DmaModel {
  int nbuf, blen;
  double rate;
  std::vector<int64_t> tag;          // -1 = never written / cleared
  std::vector<int16_t> val;
  int hwBuf = 0, hwPos = 0;
  std::deque<int> q;
  int qcap;
  int curr = -1, rw = 0;
  int64_t ticks = 0;
  double t0us = 0;
  int64_t nextId = 0;                // next sequence number a write assigns
  int64_t expect = 0;                // next sequence number that should play
  bool started = false, inOrder = true, judging = true;
  long episodes = 0;                 // in order -> not in order (each an audible defect)
  int64_t dryFrames = 0;
  int64_t nonzeroAfterEnd = 0;       // audible samples played after judging stopped
  int64_t lastRealId = -1;           // set by the caller: judging stops once this has played
  double lastRealPlayedMs = -1;
  int64_t startId = 0;               // the first sample of the stream being judged
  double startedMs = -1;             // when it played
  std::vector<double> episodeMs;

  DmaModel(int nb, int bl, double r) : nbuf(nb), blen(bl), rate(r), tag(nb * bl, -1), val(nb * bl, 0), qcap(nb - 1) {}

  void playOne(double nowMs) {
    const int s = hwBuf * blen + hwPos;
    const int64_t c = tag[s];
    if (!judging) {
      if (val[s] != 0) {
        nonzeroAfterEnd++;
      }
    } else if (!started) {
      if (c == startId) {
        started = true;
        expect = startId + 1;
        startedMs = nowMs;
      }
    } else if (c == expect) {
      expect++;
      inOrder = true;
    } else {
      if (inOrder) {
        episodes++;
        episodeMs.push_back(nowMs);
      }
      inOrder = false;
      dryFrames++;
      if (c > expect) {
        expect = c + 1;              // a skip: frames written and never played
      }
    }
    if (judging && lastRealId >= 0 && expect > lastRealId) {
      judging = false;
      lastRealPlayedMs = nowMs;
    }
    if (++hwPos == blen) {
      if ((int)q.size() == qcap) {
        const int d = q.front();
        q.pop_front();
        for (int i = 0; i < blen; i++) {
          tag[d * blen + i] = -1;    // tx_desc_auto_clear
          val[d * blen + i] = 0;
        }
      }
      q.push_back(hwBuf);
      hwBuf = (hwBuf + 1) % nbuf;
      hwPos = 0;
    }
  }
  /* i2s_zero_dma_buffer(): the contents go, the queue and the writer's buffer stay. */
  void zeroContents() {
    for (size_t i = 0; i < tag.size(); i++) {
      tag[i] = -1;
      val[i] = 0;
    }
  }
  /* A new stream starts with the next sample written: judge from there. */
  void rearm() {
    started = false;
    inOrder = true;
    startId = nextId;
  }
  void advance(double us) {
    const int64_t target = (int64_t)((us - t0us) * rate / 1e6);
    while (ticks < target) {
      playOne((t0us + ticks * 1e6 / rate) / 1000.0);
      ticks++;
    }
  }
  int write(int n, const int16_t* v) {
    int w = 0;
    while (n > 0) {
      if (curr < 0 || rw == blen) {
        if (q.empty()) {
          break;
        }
        curr = q.front();
        q.pop_front();
        rw = 0;
      }
      int k = blen - rw;
      if (k > n) {
        k = n;
      }
      for (int i = 0; i < k; i++) {
        tag[curr * blen + rw + i] = nextId + w + i;
        val[curr * blen + rw + i] = v ? v[w + i] : 1;
      }
      rw += k;
      n -= k;
      w += k;
    }
    nextId += w;
    return w;
  }
  /* Frames written and not yet played, while playing in order (the truth the counter bounds). */
  int64_t trueLead() const {
    return started ? nextId - expect : -1;
  }
};

// ─── the phone around the feed: a clock, the card, the DMA ──────────────────────────────

struct Clock {
  double us = 0;
};

struct MemSource : public MusicSource {
  const std::vector<uint8_t>* d = nullptr;
  size_t pos = 0;
  Clock* clk = nullptr;
  int read(uint8_t* dst, size_t n) override {
    const size_t left = d->size() - pos;
    if (n > left) {
      n = left;
    }
    memcpy(dst, d->data() + pos, n);
    pos += n;
    if (clk) {
      clk->us += 330 + 1.95 * (double)n;
    }
    return (int)n;
  }
  bool seek(uint32_t p) override {
    pos = p <= d->size() ? p : d->size();
    return true;
  }
  uint32_t size() override {
    return (uint32_t)d->size();
  }
};

struct ModelSink : public MusicSink {
  DmaModel* m = nullptr;
  Clock* clk = nullptr;
  bool keepValues = false;
  long shortWrites = 0;                // writes the DMA did not take whole: "the ring is full"
  size_t write(const int16_t* s, size_t n) override {
    m->advance(clk->us);
    const int w = m->write((int)n, keepValues ? s : nullptr);
    clk->us += 25;
    if ((size_t)w < n) {
      shortWrites++;
    }
    return (size_t)w;
  }
  uint64_t nowUs() override {
    return (uint64_t)clk->us;
  }
};

/* A ModelSink that also keeps every sample the DMA accepted, in order. */
struct RecSink : public ModelSink {
  std::vector<int16_t> rec;
  size_t write(const int16_t* s, size_t n) override {
    const size_t w = ModelSink::write(s, n);
    rec.insert(rec.end(), s, s + w);
    return w;
  }
};

/* THE REJECTED RULE (section 8; music_feed.h, "...AND THE CATCH-UP STAYS ON THE PASS AFTER A
 * LONG ONE"), line for line from c20ccbf's MusicBudget and its constants: a pass that followed
 * >= 150 ms elsewhere while the ring held half decodes ONE unit and owes the rest of its budget;
 * the passes after it decode their own budget plus what is owed, up to 12; a long pass under
 * half does the plain catch-up and repays only under 30%; a ring that refuses a write clears the
 * debt. Kept only so the tests below can show they catch it. */
struct DeferRule {
  int owed = 0;
  int base = 0;
  int next(int32_t lo, uint32_t cap, uint64_t gapUs) {
    const int64_t l = lo;
    const int64_t c = cap;
    base = l < c * MUSIC_CATCH_UP_BELOW_PCT / 100 ? MUSIC_UNITS_CATCH_UP : MUSIC_UNITS_PER_PASS;
    const bool longPass = gapUs >= 150000;
    if (longPass && l >= c * 50 / 100) {
      owed += base - 1;
      if (owed > MUSIC_UNITS_CATCH_UP) {
        owed = MUSIC_UNITS_CATCH_UP;
      }
      return 1;
    }
    if (longPass && l >= c * 30 / 100) {
      return base;
    }
    const int u = base + owed;
    return u > 12 ? 12 : u;
  }
  void done(int units, bool full) {
    if (full) {
      owed = 0;
      return;
    }
    if (units > base) {
      const int paid = units - base;
      owed = paid >= owed ? 0 : owed - paid;
    }
  }
};

struct SimFeed : public MusicFeed {
  Clock* clk = nullptr;
  double decodeUs = 7650;
  /* Set: the pass budget is the rejected rule above, told what each pass did (the units charged
   * here, and whether `seen` took a write short) when the next pass asks. Unset: the phone's. */
  DeferRule* defer = nullptr;
  const ModelSink* seen = nullptr;
protected:
  void chargeDecode(int rc, size_t samples) override {
    (void)samples;
    if (clk) {
      clk->us += rc > 0 ? decodeUs + 150 : 40;       // + the mono/half-band conversion
    }
    if (rc > 0) {
      units++;
    }
  }
  int budgetFor(int32_t lo, uint32_t cap, uint64_t gapUs) override {
    if (!defer) {
      return MusicFeed::budgetFor(lo, cap, gapUs);
    }
    const long shorts = seen ? seen->shortWrites : 0;
    if (asked) {
      defer->done(units, shorts != shortsBefore);
    }
    asked = true;
    units = 0;
    shortsBefore = shorts;
    return defer->next(lo, cap, gapUs);
  }
private:
  int units = 0;
  bool asked = false;
  long shortsBefore = 0;
};

struct Scenario {
  const char* name;
  double passMs;
  double spikeMs;
  double spikeEveryS;
  double secs;
};

struct Result {
  long trueEpisodes = 0;
  double trueDryMs = 0;
  uint32_t drops = 0, dropMs = 0;
  int32_t minLeadMs = 0;
  double gapsPerSec = 0;          // the old counter
  uint32_t maxWorkUs = 0;
  double capMs = 0;
};

/* The loop: every pass is the rest of the phone (passMs, plus a stall every spikeEveryS),
 * then one feed pass. Returns what the model heard and what the feed said. */
static Result runNew(const Stream& st, const Scenario& sc, double decodeUs = 7650) {
  Result R;
  Clock clk;
  MemSource src;
  src.d = &st.bytes;
  src.clk = &clk;
  Mp3Stream mp3;
  mp3.begin();
  static int16_t dec[2400];
  SimFeed feed;
  feed.clk = &clk;
  feed.decodeUs = decodeUs;
  feed.begin();
  feed.swapPairs = false;
  if (!feed.openMp3(&src, &mp3, dec, 0, 0)) {
    ok(false, "the synthetic stream opens");
    return R;
  }
  DmaModel m(MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate());
  m.t0us = clk.us;                                  // i2s_driver_install + i2s_start
  ModelSink sink;
  sink.m = &m;
  sink.clk = &clk;
  MusicDma dma = {MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate(), 1};
  feed.start(&sink, dma);
  const double end = clk.us + sc.secs * 1e6;
  double nextSpike = clk.us + sc.spikeEveryS * 1e6;
  while (clk.us < end) {
    clk.us += sc.passMs * 1000;
    if (sc.spikeEveryS > 0 && clk.us >= nextSpike) {
      clk.us += sc.spikeMs * 1000;
      nextSpike += sc.spikeEveryS * 1e6;
    }
    uint32_t before = 0;
    if (getenv("MF_DEBUG")) {
      MusicStats b;
      feed.stats(&b, (uint64_t)clk.us);
      before = b.drops;
    }
    feed.pass(dma);
    if (getenv("MF_DEBUG")) {
      MusicStats a;
      feed.stats(&a, (uint64_t)clk.us);
      if (a.drops != before) {
        printf("    drop counted at pass start %.1f ms (dropMs now %u)\n", (clk.us - m.t0us) / 1000.0, a.dropMs);
      }
    }
  }
  m.advance(clk.us);
  MusicStats s;
  feed.stats(&s, (uint64_t)clk.us);
  if (getenv("MF_DEBUG") && m.episodes) {
    printf("    [%s] episodes at:", sc.name);
    for (double e : m.episodeMs) {
      printf(" %.1f", e - m.t0us / 1000.0);
    }
    printf(" ms (dry %.0f ms)\n", m.dryFrames * 1000.0 / feed.outRate());
  }
  R.trueEpisodes = m.episodes;
  R.trueDryMs = m.dryFrames * 1000.0 / feed.outRate();
  R.drops = s.drops;
  R.dropMs = s.dropMs;
  R.minLeadMs = s.minLeadMs;
  R.maxWorkUs = s.maxWorkUs;
  R.capMs = s.capMs;
  return R;
}

/* ── THE OLD FEED, as shipped in 0.9.78 (Audio.cpp at 87393a6), line for line ─────────────
 * playMusic()'s 64-try first decode with 512-byte reads; Audio::loop(): the per-sample
 * playChunk() at the top, then guard 12 x { pushMusicChunk() (non-blocking), fillMusicFrame()
 * (<= 512 B read, ONE decode) }, and the starvedNow/musicWasStarved "gaps" rule. Headphones
 * (stereo, 4 x 1023 frames at 44.1 kHz) — the loudspeaker path was mono garbage besides. */
struct OldFeed {
  Mp3Stream mp3;
  MemSource* src = nullptr;
  DmaModel* m = nullptr;
  Clock* clk = nullptr;
  int16_t playDec[2400];
  uint32_t cur = 0, left = 0;
  bool eof = false, wasStarved = false;
  uint32_t gaps = 0;
  bool avail() const { return src->pos < src->d->size(); }
  void readUpTo512() {
    uint8_t tmp[512];
    size_t room = mp3.space();
    if (room > sizeof(tmp)) {
      room = sizeof(tmp);
    }
    if (room > 0 && avail()) {
      const int n = src->read(tmp, room);
      if (n > 0) {
        mp3.fill(tmp, (size_t)n);
      }
    }
  }
  int decode(Mp3Info* info) {
    const int rc = mp3.decode(playDec, info);
    clk->us += rc > 0 ? 7650 : 40;
    return rc;
  }
  bool open() {
    mp3.begin();
    mp3.reset();
    src->seek(0);
    for (int t = 0; t < 64; t++) {
      readUpTo512();
      Mp3Info info;
      const int s = decode(&info);
      if (s > 0) {
        cur = 0;
        left = (uint32_t)(s / info.channels);
        return true;
      }
    }
    return false;
  }
  bool push() {
    if (left == 0) {
      return true;
    }
    m->advance(clk->us);
    const int w = m->write((int)left, nullptr);
    clk->us += 25;
    cur += w;
    left -= w;
    return left == 0;
  }
  bool playChunk() {
    while (left > 0) {
      clk->us += 2.5;
      m->advance(clk->us);
      if (m->write(1, nullptr) != 1) {
        return false;
      }
      left--;
      cur++;
    }
    return true;
  }
  bool fill() {
    if (left > 0) {
      return true;
    }
    readUpTo512();
    Mp3Info info;
    const int s = decode(&info);
    if (s > 0) {
      cur = 0;
      left = (uint32_t)(s / info.channels);
      return true;
    }
    if (!avail() && mp3.space() > 0) {
      eof = true;
    }
    return false;
  }
  void loop() {
    if (left > 0) {
      playChunk();
    }
    bool starvedNow = left == 0 && !eof;
    for (int g = 0; g < 12; g++) {
      if (left > 0 && !push()) {
        starvedNow = false;
        break;
      }
      if (eof) {
        break;
      }
      if (!fill()) {
        break;
      }
    }
    if (starvedNow && !wasStarved) {
      gaps++;
    }
    wasStarved = starvedNow;
  }
};

static Result runOld(const Stream& st, const Scenario& sc) {
  Result R;
  Clock clk;
  MemSource src;
  src.d = &st.bytes;
  src.clk = &clk;
  OldFeed f;
  f.src = &src;
  f.clk = &clk;
  DmaModel m(4, 1023, st.rate);        // 4 x 1024 requested; IDF caps a stereo buffer at 4092 B
  f.m = &m;
  if (!f.open()) {
    ok(false, "the old feed opens the stream");
    return R;
  }
  m.t0us = clk.us;
  const double start = clk.us;
  const double end = clk.us + sc.secs * 1e6;
  double nextSpike = clk.us + sc.spikeEveryS * 1e6;
  while (clk.us < end) {
    clk.us += sc.passMs * 1000;
    if (sc.spikeEveryS > 0 && clk.us >= nextSpike) {
      clk.us += sc.spikeMs * 1000;
      nextSpike += sc.spikeEveryS * 1e6;
    }
    f.loop();
  }
  m.advance(clk.us);
  R.trueEpisodes = m.episodes;
  R.trueDryMs = m.dryFrames * 1000.0 / st.rate;
  R.gapsPerSec = f.gaps / ((clk.us - start) / 1e6);
  return R;
}

// ─── 1 + 2: old vs new on the 192 kbps shape ────────────────────────────────────────────

static void testOldVsNew() {
  group("the 192 kbps stream: the old feed vs the new one");
  const Stream s192 = makeMpeg1(44100, 192, 2400);      // 62.7 s, 627/626-byte frames
  ok(s192.bytes.size() > 1400000 && s192.frameOff[1] - s192.frameOff[0] >= 626,
     "the stream has Pulse/Peinture's frame size (626.9 B at 192k/44.1k)");

  {
    const Scenario sc = {"10 ms passes", 10, 0, 0, 19};
    const Result r = runOld(s192, sc);
    note("OLD, 10 ms passes, 19 s: gaps %.2f/s, TRUE dropouts %ld (phone on 0.9.78: 167 in 19 s = 8.8/s)",
         r.gapsPerSec, r.trueEpisodes);
    ok(r.gapsPerSec > 7.5 && r.gapsPerSec < 9.5,
       "the old 'gaps' counter reads ~8.6/s on 192k (the phone's 8-9/s) ...");
    ok(r.trueEpisodes == 0, "... while the DMA never once ran dry: it counted arithmetic");
  }
  {
    const Scenario sc = {"50 ms passes", 50, 0, 0, 30};
    const Result r = runOld(s192, sc);
    note("OLD, 50 ms passes, 30 s: TRUE dropouts %.2f/s, %.0f ms of dry audio",
         r.trueEpisodes / 30.0, r.trueDryMs);
    ok(r.trueEpisodes >= 30, "the old feed really drops out at 50 ms passes (the replay: 4.7/s)");
  }
  {
    const Scenario sc = {"10 ms + 100 ms stall every 1 s", 10, 100, 1, 30};
    const Result r = runOld(s192, sc);
    note("OLD, 10 ms + 100 ms every 1 s: TRUE dropouts %.2f/s", r.trueEpisodes / 30.0);
    ok(r.trueEpisodes >= 20, "and at a 100 ms stall every second (a map frame, a repaint + more)");
  }

  const Scenario grid[] = {
    {"5 ms passes", 5, 0, 0, 60},
    {"10 ms passes", 10, 0, 0, 60},
    {"20 ms passes", 20, 0, 0, 60},
    {"30 ms passes", 30, 0, 0, 60},
    {"40 ms passes", 40, 0, 0, 60},
    {"50 ms passes", 50, 0, 0, 60},
    {"60 ms passes", 60, 0, 0, 60},
    {"80 ms passes", 80, 0, 0, 60},
    {"10 ms + 100 ms every 1 s", 10, 100, 1, 60},
    {"10 ms + 300 ms every 10 s", 10, 300, 10, 60},
    {"40 ms + 300 ms every 10 s", 40, 300, 10, 60},
    {"10 ms + 450 ms every 5 s", 10, 450, 5, 60},
    /* Long passes AND a stall (review, 2026-09-25). At a map pan's ~77 ms a pass plus its own
     * decode needs more than 4 frames, so the ring settles at the catch-up mark: at 40% (a2aa516)
     * both of these dropped out on every few stalls while every row above stayed green. */
    {"77 ms (map pan) + 300 ms every 10 s", 77, 300, 10, 60},
    {"100 ms + 300 ms every 10 s", 100, 300, 10, 60},
  };
  for (const Scenario& sc : grid) {
    const Result r = runNew(s192, sc);
    note("NEW, %-26s true dropouts %ld, drops %u, minLead %d ms (ring %.0f ms), worst pass here %.1f ms",
         sc.name, r.trueEpisodes, r.drops, (int)r.minLeadMs, r.capMs, r.maxWorkUs / 1000.0);
    char what[160];
    snprintf(what, sizeof(what), "NEW feed, %s: no true dropout", sc.name);
    ok(r.trueEpisodes == 0, what);
    snprintf(what, sizeof(what), "NEW feed, %s: drops says 0 and minLead proves it (> 0)", sc.name);
    ok(r.drops == 0 && r.minLeadMs > 0, what);
  }
  /* The work one pass may add to the loop: the budget (4 frames, 8 catching up), not the ring. */
  {
    const Scenario sc = {"10 ms + 450 ms every 5 s", 10, 450, 5, 30};
    const Result r = runNew(s192, sc);
    ok(r.maxWorkUs < 75000, "no pass spends more than ~8 frames' decode on music, even catching up");
  }
}

static void testOtherShapes() {
  group("the other shapes: 48 kHz / 128k (Hells_Bells), 320k, a slower decode");
  const Stream s48 = makeMpeg1(48000, 128, 1500);       // 384-byte frames, 24 kHz out
  const Stream s320 = makeMpeg1(44100, 320, 1500);      // 1044-byte frames
  const Scenario hard[] = {
    {"40 ms + 300 ms every 10 s", 40, 300, 10, 36},
    {"60 ms passes", 60, 0, 0, 36},
    {"10 ms + 100 ms every 1 s", 10, 100, 1, 36},
    {"77 ms (map pan) + 300 ms every 10 s", 77, 300, 10, 36},
  };
  for (const Scenario& sc : hard) {
    const Result a = runNew(s48, sc);
    const Result b = runNew(s320, sc);
    note("48k/128k %-26s true %ld drops %u minLead %d ms | 320k: true %ld drops %u minLead %d ms",
         sc.name, a.trueEpisodes, a.drops, (int)a.minLeadMs, b.trueEpisodes, b.drops, (int)b.minLeadMs);
    char what[160];
    snprintf(what, sizeof(what), "48 kHz (24 kHz out), %s: clean", sc.name);
    ok(a.trueEpisodes == 0 && a.drops == 0, what);
    snprintf(what, sizeof(what), "320 kbps, %s: clean", sc.name);
    ok(b.trueEpisodes == 0 && b.drops == 0, what);
  }
  /* 240 MHz is faster; 80 MHz is not a music clock (music forces full speed), but a decode 50%
   * slower than 160 MHz's still keeps up at ordinary passes. */
  const Stream s192 = makeMpeg1(44100, 192, 1500);
  const Scenario sc = {"20 ms passes, decode 11.5 ms", 20, 0, 0, 36};
  const Result r = runNew(s192, sc, 11500);
  ok(r.trueEpisodes == 0 && r.drops == 0, "a decode 1.5x slower than measured still keeps up");
}

// ─── 8: a long pass, then a stall — the catch-up stays on the pass after a long one ─────

/* The rest of the loop, pass by pass: passes of `lightMs`; from `firstS` on, every `everyS`
 * seconds, `heavy` passes of `heavyMs` back to back, then (if `thenMs`) `between` light passes
 * and one pass of `thenMs` — a stall landing right after the long pass. */
struct Plan {
  double lightMs;
  double heavyMs;
  int heavy;
  int between;
  double thenMs;
  double firstS, everyS, secs;
};

/* Per pass: the rest of the loop and then this pass's music, in ms. On the phone the frame is
 * pushed at the end of the rest ("redraw") and music is fed after it ("audio-loop", the bottom of
 * loop()), so the LOOP STALL line measures rest + music of ONE pass, while the eye gets push to
 * push: the previous pass's music + this pass's rest. */
struct PlanRun {
  long episodes = 0;
  uint32_t drops = 0;
  int32_t minLeadMs = 0;
  std::vector<double> rest, work;
  std::vector<int> kind;                      // 0 light, 1 heavy, 2 the stall after
  double maxIter() const {
    double m = 0;
    for (size_t i = 0; i < rest.size(); i++) {
      m = std::max(m, rest[i] + work[i]);
    }
    return m;
  }
  long itersOver(double ms) const {
    long n = 0;
    for (size_t i = 0; i < rest.size(); i++) {
      n += rest[i] + work[i] > ms;
    }
    return n;
  }
  double frame(size_t i) const {
    return (i ? work[i - 1] : 0) + rest[i];
  }
  double maxFrame() const {
    double m = 0;
    for (size_t i = 0; i < rest.size(); i++) {
      m = std::max(m, frame(i));
    }
    return m;
  }
  long framesOver(double ms) const {
    long n = 0;
    for (size_t i = 0; i < rest.size(); i++) {
      n += frame(i) > ms;
    }
    return n;
  }
  /* The longest of the three frames after each burst of heavy passes: where a catch-up lands. */
  double afterHeavy() const {
    double m = 0;
    for (size_t i = 1; i < rest.size(); i++) {
      if (kind[i - 1] == 1 && kind[i] != 1) {
        for (size_t k = i; k < i + 3 && k < rest.size(); k++) {
          m = std::max(m, frame(k));
        }
      }
    }
    return m;
  }
  double heavyWork() const {
    double m = 0;
    for (size_t i = 0; i < rest.size(); i++) {
      if (kind[i] == 1) {
        m = std::max(m, work[i]);
      }
    }
    return m;
  }
};

static PlanRun runPlan(const Stream& st, const Plan& p, double decodeUs, bool deferral) {
  PlanRun R;
  Clock clk;
  MemSource src;
  src.d = &st.bytes;
  src.clk = &clk;
  Mp3Stream mp3;
  mp3.begin();
  static int16_t dec[2400];
  DeferRule rule;
  SimFeed feed;
  feed.clk = &clk;
  feed.decodeUs = decodeUs;
  feed.defer = deferral ? &rule : nullptr;
  feed.begin();
  feed.swapPairs = false;
  if (!feed.openMp3(&src, &mp3, dec, 0, 0)) {
    ok(false, "the synthetic stream opens");
    return R;
  }
  DmaModel m(MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate());
  m.t0us = clk.us;
  ModelSink sink;
  sink.m = &m;
  sink.clk = &clk;
  feed.seen = &sink;
  MusicDma dma = {MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate(), 1};
  feed.start(&sink, dma);
  const double t0 = clk.us;
  double nextS = p.firstS;
  int heavyLeft = 0, betweenLeft = 0;
  bool thenDue = false;
  while (clk.us - t0 < p.secs * 1e6) {
    const double t = (clk.us - t0) / 1e6;
    if (heavyLeft == 0 && betweenLeft == 0 && !thenDue && t >= nextS) {
      heavyLeft = p.heavy;
      thenDue = p.thenMs > 0;
      betweenLeft = thenDue ? p.between : 0;
      nextS += p.everyS;
    }
    double rest = p.lightMs;
    int kind = 0;
    if (heavyLeft > 0) {
      heavyLeft--;
      rest = p.heavyMs;
      kind = 1;
    } else if (betweenLeft > 0) {
      betweenLeft--;
    } else if (thenDue) {
      thenDue = false;
      rest = p.thenMs;
      kind = 2;
    }
    clk.us += rest * 1000;
    const double a = clk.us;
    feed.pass(dma);
    R.rest.push_back(rest);
    R.work.push_back((clk.us - a) / 1000.0);
    R.kind.push_back(kind);
  }
  m.advance(clk.us);
  MusicStats s;
  feed.stats(&s, (uint64_t)clk.us);
  R.episodes = m.episodes;
  R.drops = s.drops;
  R.minLeadMs = s.minLeadMs;
  return R;
}

/* The review of c20ccbf (2026-09-25), problem 1, as a test: a long pass (a tile + the redraw, a Files slice
 * that ran over, a mesh send) and then, before any short pass, a stall. Nothing runs between the
 * two, so what the ring holds at the second is what the long pass left it: a rule that decodes
 * less on the long pass loses stalls this one survives. */
static void testLongPassThenStall() {
  group("a long pass, then a stall: the catch-up stays on the pass after a long one");
  /* `envelopeMs`: the long pass and the stall after it together, up to which this rule keeps
   * >= 20 ms in the ring in every case below (22.05 kHz: 27 ms at the worst; 24 kHz: 43; 32 kHz,
   * a 384 ms ring that the long pass's 8 frames refill by 288 ms: 57). */
  struct Src {
    const char* name;
    Stream s;
    double envelopeMs;
  };
  const Src srcs[] = {
    {"44.1k/192k (22.05 kHz out)", makeMpeg1(44100, 192, 400), 600},
    {"48k/128k (24 kHz out)", makeMpeg1(48000, 128, 420), 530},
    {"32k/128k (32 kHz, whole)", makeMpeg1(32000, 128, 300), 530},
  };
  const double costs[] = {7650, 9900};
  const double bases[] = {10, 40, 77};
  const double longs[] = {150, 190, 230};
  const double stalls[] = {300, 400, 450};
  long cases = 0, clean = 0, rescued = 0;
  for (const Src& src : srcs) {
    long deferLost = 0;                       // inside the envelope: clean here, dry deferred
    for (double dc : costs) {
      long inside = 0, lost = 0;
      int32_t worst = 1 << 30;
      for (double base : bases) {
        for (double A : longs) {
          for (int between = 0; between <= 1; between++) {
            for (double B : stalls) {
              /* ~3 s of passes to settle, the long pass, `between` short ones, the stall, and
               * 1.5 s to hear the ring come back. */
              const Plan p = {base, A, 1, between, B, 3.0, 100.0, 4.5};
              const PlanRun o = runPlan(src.s, p, dc, false);
              const PlanRun d = runPlan(src.s, p, dc, true);
              const bool oClean = o.episodes == 0 && o.drops == 0;
              const bool dClean = d.episodes == 0 && d.drops == 0;
              cases++;
              clean += oClean;
              rescued += !oClean && dClean;
              if (A + B <= src.envelopeMs) {
                inside++;
                worst = std::min(worst, o.minLeadMs);
                if (!oClean || o.minLeadMs <= 0) {
                  lost++;
                  note("  LOST: %s @%.2f, %.0f ms passes, %.0f then %d short then %.0f: minLead %d", src.name,
                       dc / 1000.0, base, A, between, B, (int)o.minLeadMs);
                }
                deferLost += oClean && !dClean;
              }
              if (getenv("MF_PAIRS")) {
                note("%-26s @%.2f %2.0f ms, %3.0f then %d short then %3.0f: minLead %4d%s | deferral %4d%s",
                     src.name, dc / 1000.0, base, A, between, B, (int)o.minLeadMs, oClean ? "" : " DROPOUT",
                     (int)d.minLeadMs, dClean ? "" : " DROPOUT");
              }
            }
          }
        }
      }
      char what[220];
      snprintf(what, sizeof(what),
               "%s @%.2f: a long pass of 150-230 ms, 0-1 short passes, then a stall, together <= %.0f ms: "
               "no dropout in %ld cases (least left %d ms)",
               src.name, dc / 1000.0, src.envelopeMs, inside, (int)worst);
      ok(lost == 0, what);
      note("%-26s @%.2f: long pass + stall <= %.0f ms: %ld cases, %ld dropouts, least left %d ms", src.name,
           dc / 1000.0, src.envelopeMs, inside, lost, (int)worst);
    }
    note("%-26s        the replayed deferral ran dry in %ld of them", src.name, deferLost);
    char what[200];
    snprintf(what, sizeof(what), "%s: (the replayed deferral runs dry in %ld of those: the test has teeth)",
             src.name, deferLost);
    ok(deferLost > 0, what);
  }
  note("all %ld cases, 300-450 ms stalls: this rule clean in %ld; the deferral clean in %ld that this rule lost",
       cases, clean, rescued);
  ok(rescued == 0, "(and deferring never rescues a case this rule loses)");
}

/* The map pan the phone measured (2026-09-25, Pulse.mp3 192k under `maps hold`): short passes and
 * every so often a HEAVY one — a tile's last piece + the full redraw, ~190-210 ms. The LOOP STALL
 * lines read 274-301 ms with 88-94 ms of music on top (9.9 ms a frame reproduces the 91). */
static void testMapPan() {
  group("a map pan: the catch-up after the heavy pass is expected, and deferring it hides nothing");
  const Stream s = makeMpeg1(44100, 192, 2600);
  const double costs[] = {7650, 9900};
  struct Shape {
    const char* name;
    Plan p;
  };
  const Shape shapes[] = {
    {"40 ms + a 190 ms pass every 2 s", {40, 190, 1, 0, 0, 2, 2, 60}},
    {"77 ms + a 190 ms pass every 2 s", {77, 190, 1, 0, 0, 2, 2, 60}},
    {"77 ms + a 210 ms pass every 2 s", {77, 210, 1, 0, 0, 2, 2, 60}},
    {"100 ms + a 190 ms pass every 2 s", {100, 190, 1, 0, 0, 2, 2, 60}},
  };
  for (double dc : costs) {
    const double frame = (dc + 2000) / 1000.0;                  // a frame's decode + its share of reads
    for (const Shape& sh : shapes) {
      const PlanRun o = runPlan(s, sh.p, dc, false);
      const PlanRun d = runPlan(s, sh.p, dc, true);
      note("%-34s @%.2f ms/frame", sh.name, dc / 1000.0);
      note("  this rule: music on the heavy pass %5.1f ms | LOOP STALL pass max %5.1f, %3ld over 250 | "
           "push to push max %5.1f, after the heavy one %5.1f, %3ld over 200 | minLead %d",
           o.heavyWork(), o.maxIter(), o.itersOver(250), o.maxFrame(), o.afterHeavy(), o.framesOver(200),
           (int)o.minLeadMs);
      note("  deferral:  music on the heavy pass %5.1f ms | LOOP STALL pass max %5.1f, %3ld over 250 | "
           "push to push max %5.1f, after the heavy one %5.1f, %3ld over 200 | minLead %d",
           d.heavyWork(), d.maxIter(), d.itersOver(250), d.maxFrame(), d.afterHeavy(), d.framesOver(200),
           (int)d.minLeadMs);
      char what[200];
      snprintf(what, sizeof(what), "%s @%.2f: clean, minLead > 200 ms", sh.name, dc / 1000.0);
      ok(o.episodes == 0 && o.drops == 0 && o.minLeadMs > 200, what);
      snprintf(what, sizeof(what),
           "%s @%.2f: the heavy pass carries the whole catch-up and the LOOP STALL line names it: EXPECTED "
           "(read music_feed.h before giving the long pass a rule of its own)",
           sh.name, dc / 1000.0);
      ok(o.heavyWork() > 7 * frame && o.itersOver(250) >= 25, what);
      snprintf(what, sizeof(what),
           "%s @%.2f: (the deferral clears that line, but the longest push to push is no shorter ...)",
           sh.name, dc / 1000.0);
      ok(d.itersOver(250) == 0 && d.maxFrame() >= o.maxFrame() - 0.5, what);
      snprintf(what, sizeof(what), "%s @%.2f: (... and the worst frame after the heavy one is longer)", sh.name,
               dc / 1000.0);
      ok(d.afterHeavy() > o.afterHeavy() + 2 * frame, what);
    }
    /* Back to back: the map loads a tile in pieces a pass apart, so heavy passes are normally
     * apart; two or three in a row are the hard case for the ring. */
    const Plan pair = {40, 190, 2, 0, 0, 2, 2, 60};
    const Plan three = {40, 230, 3, 0, 0, 3, 3, 60};
    const PlanRun p = runPlan(s, pair, dc, false);
    const PlanRun q = runPlan(s, three, dc, false);
    note("40 ms + 2 x 190 ms back to back / 2 s @%.2f: minLead %d, push to push max %.1f", dc / 1000.0,
         (int)p.minLeadMs, p.maxFrame());
    note("40 ms + 3 x 230 ms back to back / 3 s @%.2f: minLead %d, push to push max %.1f", dc / 1000.0,
         (int)q.minLeadMs, q.maxFrame());
    char what[200];
    snprintf(what, sizeof(what), "2 x 190 ms back to back @%.2f: clean, minLead > 200 ms", dc / 1000.0);
    ok(p.episodes == 0 && p.drops == 0 && p.minLeadMs > 200, what);
    snprintf(what, sizeof(what), "3 x 230 ms back to back @%.2f: clean, minLead > 50 ms", dc / 1000.0);
    ok(q.episodes == 0 && q.drops == 0 && q.minLeadMs > 50, what);
  }
}

// ─── the counter's honesty when it fails ────────────────────────────────────────────────

static void testDropsAreTrue() {
  group("when the ring DOES run dry, `drops` says so (and never more than happened)");
  const Stream s192 = makeMpeg1(44100, 192, 2400);
  /* A stall longer than the ring is ONE audible dropout. (The model can split one into two
   * "episodes": IDF 3.3 keeps cycling the ring through a long stall and replays the writer's
   * half-filled buffer, which briefly holds the very next samples in order.) */
  struct Stall {
    Scenario sc;
    uint32_t stalls;          // stalls in the window that outlast the ring
  };
  const Stall stalls[] = {
    {{"1 s stall every 10 s", 10, 1000, 10, 60}, 6},
    {{"0.8 s stall every 7 s", 20, 800, 7, 60}, 8},
    {{"4.2 s stall (a screenshot) every 20 s", 10, 4200, 20, 60}, 3},
  };
  for (const Stall& st : stalls) {
    const Result r = runNew(s192, st.sc);
    note("%-38s true dropouts %ld (%.0f ms) | drops %u (%u ms)", st.sc.name, r.trueEpisodes, r.trueDryMs,
         r.drops, r.dropMs);
    char what[160];
    snprintf(what, sizeof(what), "%s: one drop per stall that outlasts the ring, none invented", st.sc.name);
    ok(r.drops == st.stalls && (long)r.drops <= r.trueEpisodes, what);
    snprintf(what, sizeof(what), "%s: dropMs is a lower bound, short by at most ~1.5 buffers a drop", st.sc.name);
    ok(r.dropMs <= r.trueDryMs + 1 && r.dropMs + 40.0 * r.drops >= r.trueDryMs, what);
    snprintf(what, sizeof(what), "%s: minLead went below zero", st.sc.name);
    ok(r.minLeadMs < 0, what);
  }
  /* After a drop the upper bound restarts at "full", and only normalize() stops it staying loose
   * by everything written since: at LONG passes the writer seldom meets a full ring to pin it
   * again, so without it the next stall inside that stretch is not counted (review, 2026-09-25:
   * deleting the normalize() call in run() was the one mutation no test caught; here it
   * counts 6 of these 12 stalls). */
  {
    const Scenario sc = {"120 ms passes + 0.8 s stall every 5 s", 120, 800, 5, 60};
    const Result r = runNew(s192, sc);
    note("%-38s true dropouts %ld (%.0f ms) | drops %u (%u ms)", sc.name, r.trueEpisodes, r.trueDryMs,
         r.drops, r.dropMs);
    ok(r.drops == 12 && (long)r.drops <= r.trueEpisodes,
       "120 ms passes + 0.8 s stalls: every stall is counted, though the ring is seldom full between");
  }
}

/* The same claim at the unit level: a fresh (or just-dropped) ring's upper bound is "full", and a
 * writer that is never refused must not leave it at full + everything written since. */
static void testLeadNormalize() {
  group("MusicLead::normalize(): no bound above what the ring can hold");
  const uint32_t cap = MUSIC_DMA_BUFS * MUSIC_DMA_BUF_SAMPLES;
  const MusicDma d = {MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, 22050, 1};
  MusicLead a;
  a.begin(d, 0);
  a.wrote(cap);                          // a whole ring written, and never a short write
  a.normalize(0);
  ok(a.hi(0) == (int32_t)cap && a.lo(0) <= (int32_t)cap, "both bounds at most a full ring after normalize()");
  /* 700 ms with nothing written: the ring (557 ms) is certainly dry, by ~140 ms. Un-normalized, the
   * upper bound would still be +a full ring and this would read as clean. */
  ok(a.check(700000), "a 700 ms stall after it is a drop");
  ok(a.drops == 1 && a.dryUs > 100000, "counted once, with the dry time it certainly had");
}

// ─── WAV units: whole groups of four, so the pair swap stays on its pairs ─────────────────

/* The mono pair swap is done per staged unit, so every unit must hand the ring an EVEN number of
 * samples. A WAV's first unit used to be odd at half rate (the converter's one-frame lag, then
 * `& ~1`): from there on every pair was swapped with the wrong partner. Played twice, swap on
 * and swap off, the first must be the second with each pair exchanged — from the first sample
 * to the last. */
static void testWavUnits() {
  group("a WAV's units keep the mono pair swap on its pairs (44.1 kHz stereo, half rate)");
  const int fs = 44100, secs = 2;
  std::vector<uint8_t> w;
  appendWavHeader(w, fs, 2, (uint32_t)(fs * secs * 4));
  for (int i = 0; i < fs * secs; i++) {
    const int16_t l = (int16_t)(9000 * sin(2 * M_PI * 440 * i / fs));
    const int16_t r = (int16_t)(7000 * sin(2 * M_PI * 1250 * i / fs));
    for (int16_t x : {l, r}) {
      w.push_back((uint8_t)x);
      w.push_back((uint8_t)(x >> 8));
    }
  }
  WavInfo info;
  ok(wavParseHeader(w.data(), w.size(), &info), "(the WAV parses)");
  std::vector<int16_t> heard[2];
  for (int sw = 0; sw < 2; sw++) {
    Clock clk;
    MemSource src;
    src.d = &w;
    src.clk = &clk;
    static int16_t dec[2400];
    SimFeed feed;
    feed.clk = &clk;
    feed.decodeUs = 0;
    feed.begin();
    feed.swapPairs = sw == 1;
    feed.openWav(&src, info, dec, 0);
    DmaModel m(MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate());
    m.t0us = clk.us;
    RecSink sink;
    sink.m = &m;
    sink.clk = &clk;
    sink.keepValues = true;
    MusicDma dma = {MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate(), 1};
    feed.start(&sink, dma);
    for (int i = 0; i < 600 && !feed.ended(); i++) {
      clk.us += 10000;
      feed.pass(dma);
    }
    heard[sw] = sink.rec;
  }
  const size_t len = heard[0].size() < heard[1].size() ? heard[0].size() : heard[1].size();
  size_t wrong = 0, firstWrong = (size_t)-1;
  for (size_t k = 0; k + 1 < len; k += 2) {
    if (heard[1][k] != heard[0][k + 1] || heard[1][k + 1] != heard[0][k]) {
      wrong++;
      if (firstWrong == (size_t)-1) {
        firstWrong = k;
      }
    }
  }
  note("%zu samples each way; %zu pairs not exchanged (first at sample %ld)", len, wrong,
       firstWrong == (size_t)-1 ? -1L : (long)firstWrong);
  ok(len > (size_t)(fs * secs / 2) - 8, "(the whole track was heard both ways)");
  ok(wrong == 0, "every pair swapped with its own partner, from the first unit to the last");
}

// ─── a card that will not read ──────────────────────────────────────────────────────────

/* A MemSource that fails on purpose: reads starting at or past `failFrom` return 0 `failReads`
 * times (-1 = for ever), or come back `shortBy` bytes short once. 0 is what Arduino's
 * File::read() returns for an error AND for the end of the file. */
struct FlakySource : public MemSource {
  size_t failFrom = (size_t)-1;
  int failReads = 0;
  int shortBy = 0;
  int failedReads = 0, reopens = 0;
  int read(uint8_t* dst, size_t n) override {
    if (pos >= failFrom && pos < d->size()) {
      if (failReads != 0) {
        if (failReads > 0) {
          failReads--;
        }
        failedReads++;
        return 0;
      }
      if (shortBy > 0 && n > (size_t)shortBy) {
        n -= (size_t)shortBy;
        shortBy = 0;
      }
    }
    return MemSource::read(dst, n);
  }
  bool reopen(uint32_t p) override {
    reopens++;
    return seek(p);
  }
};

struct FlakyRun {
  bool ended = false, failed = false;
  long episodes = 0;
  MusicStats st;
  uint32_t playingAtFail = 0;
  uint32_t readsAfterFail = 0;          // reads attempted in the 100 passes after failed()
  int maxFailsInAPass = 0;              // failed reads attempted in any one pass (or the prefill)
};


/* 10 ms passes until ended() or failed(), then 100 more passes to see that a failed feed stays
 * quiet (Audio::loop() stops it at once on the phone; the feed must not need that to be safe). */
static FlakyRun runFlaky(FlakySource& src, bool wav, const WavInfo* info, double passMs = 10) {
  FlakyRun R;
  memset(&R.st, 0, sizeof(R.st));
  Clock clk;
  src.clk = &clk;
  Mp3Stream mp3;
  mp3.begin();
  static int16_t dec[2400];
  SimFeed feed;
  feed.clk = &clk;
  feed.decodeUs = wav ? 0 : 7650;
  feed.begin();
  feed.swapPairs = false;
  const bool opened = wav ? feed.openWav(&src, *info, dec, 0) : feed.openMp3(&src, &mp3, dec, 0, 0);
  if (!opened) {
    ok(false, "the stream opens");
    return R;
  }
  DmaModel m(MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate());
  m.t0us = clk.us;
  ModelSink sink;
  sink.m = &m;
  sink.clk = &clk;
  sink.keepValues = true;
  MusicDma dma = {MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate(), 1};
  int f0 = src.failedReads;
  feed.start(&sink, dma);
  R.maxFailsInAPass = src.failedReads - f0;
  for (int i = 0; i < 6000 && !feed.ended() && !feed.failed(); i++) {
    clk.us += passMs * 1000;
    f0 = src.failedReads;
    feed.pass(dma);
    if (src.failedReads - f0 > R.maxFailsInAPass) {
      R.maxFailsInAPass = src.failedReads - f0;
    }
  }
  m.advance(clk.us);
  R.ended = feed.ended();
  R.failed = feed.failed();
  R.episodes = m.episodes;
  if (R.failed) {
    R.playingAtFail = feed.playingPos((uint64_t)clk.us);
    MusicStats before;
    feed.stats(&before, (uint64_t)clk.us);
    for (int i = 0; i < 100; i++) {
      clk.us += 10000;
      feed.pass(dma);
    }
    MusicStats after;
    feed.stats(&after, (uint64_t)clk.us);
    R.readsAfterFail = after.reads - before.reads;
  }
  feed.stats(&R.st, (uint64_t)clk.us);
  return R;
}

static void testReadErrors() {
  group("a card read that fails mid-track is an error, not the end of the file");
  const Stream st = makeMpeg1(44100, 192, 600);          // 15.7 s
  const size_t at = st.frameOff[200];
  {
    FlakySource clean;
    clean.d = &st.bytes;
    const FlakyRun c = runFlaky(clean, false, nullptr);
    FlakySource src;
    src.d = &st.bytes;
    src.failFrom = at;
    src.failReads = MUSIC_READ_TRIES - 1;
    const FlakyRun r = runFlaky(src, false, nullptr);
    note("MP3, %d failed reads at byte %zu: ended %d failed %d, units %u (clean run %u), reopens %d, dropouts %ld",
         src.failedReads, at, (int)r.ended, (int)r.failed, r.st.units, c.st.units, src.reopens, r.episodes);
    ok(r.ended && !r.failed, "MP3: MUSIC_READ_TRIES - 1 failed reads in a row are retried, and the track plays to its end");
    ok(r.st.units == c.st.units && c.st.units >= 598, "... every frame of it: nothing after the failed reads was skipped");
    ok(r.st.readErrors == (uint32_t)(MUSIC_READ_TRIES - 1) && src.reopens == MUSIC_READ_TRIES - 1,
       "... each failed read counted, and each retry on a NEW handle (FatFs latches the error)");
    ok(r.episodes == 0, "... without a dropout: the retries cost a pass each, the ring covers them");
  }
  {
    FlakySource src;
    src.d = &st.bytes;
    src.failFrom = at;
    src.failReads = -1;                                   // the card is gone
    /* 60 ms passes: each pass decodes a few frames, so a feed that retried within a pass would. */
    const FlakyRun r = runFlaky(src, false, nullptr, 60);
    note("MP3, card gone at byte %zu: failed %d ended %d, read errors %u, place %u, reads after %u",
         at, (int)r.failed, (int)r.ended, r.st.readErrors, r.playingAtFail, r.readsAfterFail);
    ok(r.failed && !r.ended, "MP3, card pulled: failed(), NOT ended() — the player must not go to the next track");
    ok(r.st.readErrors == (uint32_t)MUSIC_READ_TRIES && r.maxFailsInAPass == 1,
       "... after exactly MUSIC_READ_TRIES failed reads, never two in one pass (each can be an SD timeout)");
    ok(r.readsAfterFail == 0, "... and then it reads nothing more");
    ok(r.playingAtFail > st.frameOff[150] && r.playingAtFail < at,
       "... and the place to resume is where it was playing, before the bytes it could not read");
  }
  /* WAV: 16 kHz mono (no filter, no halving), every sample its own index, so a skip, a repeat or
   * a misaligned byte shows as a break in the ramp. */
  const int fs = 16000, n = fs * 3;
  std::vector<uint8_t> w;
  appendWavHeader(w, fs, 1, (uint32_t)n * 2);
  for (int i = 0; i < n; i++) {
    const int16_t x = (int16_t)(1 + i % 20000);
    w.push_back((uint8_t)x);
    w.push_back((uint8_t)(x >> 8));
  }
  WavInfo info;
  ok(wavParseHeader(w.data(), w.size(), &info), "(the WAV parses)");
  const size_t wat = info.dataOffset + 30000;
  {
    FlakySource src;
    src.d = &w;
    src.failFrom = wat;
    src.failReads = -1;
    const FlakyRun r = runFlaky(src, true, &info);
    ok(r.failed && !r.ended && r.readsAfterFail == 0, "WAV, card pulled: failed(), not ended(), then quiet");
  }
  {
    /* A read that comes back 3 bytes short, mid-file (an error part-way through a read). The
     * odd bytes must be read again, not dropped: dropped, every sample after them is built from
     * the wrong byte pair — noise for the rest of the track. */
    FlakySource src;
    src.d = &w;
    src.failFrom = wat;
    src.shortBy = 3;
    Clock clk;
    src.clk = &clk;
    static int16_t dec[2400];
    SimFeed feed;
    feed.clk = &clk;
    feed.decodeUs = 0;
    feed.begin();
    feed.swapPairs = false;
    ok(feed.openWav(&src, info, dec, 0) && feed.outRate() == (uint32_t)fs, "(a 16 kHz WAV plays at its own rate)");
    DmaModel m(MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, fs);
    m.t0us = clk.us;
    RecSink sink;
    sink.m = &m;
    sink.clk = &clk;
    sink.keepValues = true;
    MusicDma dma = {MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, (uint32_t)fs, 1};
    feed.start(&sink, dma);
    for (int i = 0; i < 800 && !feed.ended(); i++) {
      clk.us += 10000;
      feed.pass(dma);
    }
    m.advance(clk.us);
    /* The ramp never holds a 0; the silence around it does. */
    long samples = 0, breaks = 0;
    int16_t prev = 0;
    for (int16_t v : sink.rec) {
      if (v == 0) {
        continue;
      }
      if (samples > 0 && !(v == prev + 1 || (prev == 20000 && v == 1))) {
        breaks++;
      }
      prev = v;
      samples++;
    }
    note("WAV, one read 3 bytes short at byte %zu: %ld samples heard of %d, %ld breaks in the ramp",
         wat, samples, n, breaks);
    ok(feed.ended() && m.episodes == 0, "WAV, a read 3 bytes short: played to the end, in order");
    /* The converter holds the file's last frame and the feed its last 0-3 samples (whole groups
     * of 4 only, see produceWav()): those few at the very end are all that may be missing. */
    ok(samples >= n - 4 && samples <= n && breaks == 0,
       "... every sample once, in order, built from the right bytes (none dropped at the start either)");
  }
}

/* The bounds themselves, against the model, with a writer that behaves badly on purpose:
 * random refills (sometimes full, sometimes a little), random gaps, occasional long stalls. */
static void testLeadBounds() {
  group("MusicLead: the model's lead is always inside [lo, hi]; no drop is ever invented");
  srand(12345);
  const int rates[] = {22050, 24000, 32000};
  long checks = 0, outside = 0, falseDrops = 0, missedLong = 0, trueEpisodesTotal = 0, drops = 0;
  long hiddenDry = 0;
  for (int trial = 0; trial < 12; trial++) {
    const int rate = rates[trial % 3];
    DmaModel m(MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, rate);
    MusicLead lead;
    double t = 0;
    MusicDma d = {MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, (uint32_t)rate, 1};
    lead.begin(d, 0);
    long lastEpisodes = 0;
    int64_t lastDryFrames = 0;
    bool recovering = false;         // a drop was counted and the ring has not been full since
    double lastNegLoMs = -1e9;
    double lastDryEndMs = -1e9;
    for (int step = 0; step < 4000; step++) {
      /* the rest of the loop */
      double gapMs = 1 + rand() % 60;
      if (rand() % 40 == 0) {
        gapMs = 200 + rand() % 1500;
      }
      t += gapMs * 1000;
      m.advance(t);
      const long newEpisodes = m.episodes - lastEpisodes;
      const bool wasDry = m.dryFrames > lastDryFrames;   // it played something out of order
      /* check() at the pass start */
      const int32_t lo = lead.lo((uint64_t)t);
      const int32_t hi = lead.hi((uint64_t)t);
      const bool counted = lead.check((uint64_t)t);
      /* The honesty claim: while the lower bound has stayed >= 0 (for a whole ring's time), the
       * model played nothing out of order. The pass that FOLLOWS a dry spell's refill can still
       * see the model resolve IDF 3.3's scrambled ring (a partial buffer the DMA had passed plays
       * a ring later) — that is the same audible dropout, already shown by lo < 0. */
      if (lo < 0) {
        lastNegLoMs = t / 1000.0;
      }
      if (wasDry && t / 1000.0 - lastNegLoMs > 1000) {
        hiddenDry++;                     // minLead would have claimed "certainly clean"
        if (getenv("MF_DEBUG")) {
          printf("    hidden: trial %d step %d dry %lld lo=%d hi=%d gap=%.1f\n", trial, step,
                 (long long)(m.dryFrames - lastDryFrames), lo, hi, gapMs);
        }
      }
      if (counted) {
        recovering = true;
        drops++;
        if (!wasDry) {
          falseDrops++;
          if (getenv("MF_DEBUG")) {
            printf("    invented: trial %d step %d t=%.1f ms lo=%d hi=%d truth=%lld inOrder=%d started=%d gap=%.1f\n",
                   trial, step, t / 1000.0, lo, hi, (long long)m.trueLead(), (int)m.inOrder, (int)m.started, gapMs);
          }
        }
      } else if (!recovering && m.dryFrames - lastDryFrames > MUSIC_DMA_BUF_SAMPLES + rate / 500) {
        /* the model played more than a buffer + 2 ms out of order since the last pass, the
         * bounds were pinned (not just after a drop), and the check did not see it */
        missedLong++;
        if (getenv("MF_DEBUG")) {
          printf("    missed: trial %d step %d dry %lld lo=%d hi=%d gap=%.1f\n", trial, step,
                 (long long)(m.dryFrames - lastDryFrames), lo, hi, gapMs);
        }
      }
      trueEpisodesTotal += newEpisodes;
      if (wasDry) {
        lastDryEndMs = t / 1000.0;
      }
      lastEpisodes = m.episodes;
      lastDryFrames = m.dryFrames;
      /* the bounds, while playing in order and well clear of the last dry spell */
      if (m.started && m.inOrder && t / 1000.0 - lastDryEndMs > 1000) {
        const int64_t truth = m.trueLead();
        checks++;
        if (truth < lo || truth > hi) {
          outside++;
          if (getenv("MF_DEBUG")) {
            printf("    outside: trial %d step %d t=%.1f ms lo=%d hi=%d truth=%lld gap=%.1f lastDry=%.1f\n",
                   trial, step, t / 1000.0, lo, hi, (long long)truth, gapMs, lastDryEndMs);
          }
        }
      }
      /* the writes of this pass: sometimes to full, sometimes a little */
      const int want = (rand() % 3 == 0) ? 64 + rand() % 900 : 1 << 20;
      int left = want;
      while (left > 0) {
        const int ask = left > 576 ? 576 : left;
        const int w = m.write(ask, nullptr);
        t += 25;
        m.advance(t);
        lead.wrote((uint32_t)w);
        left -= w;
        if (w < ask) {
          lead.full((uint64_t)t);
          recovering = false;
          break;
        }
      }
      lead.normalize((uint64_t)t);
    }
  }
  note("%ld bound checks, %ld outside; %ld true dropouts, %ld drops counted, %ld invented, %ld long ones missed, "
       "%ld dry passes with lo >= 0",
       checks, outside, trueEpisodesTotal, drops, falseDrops, missedLong, hiddenDry);
  ok(checks > 10000, "the bounds were checked many times");
  ok(outside == 0, "the lead the model measures is always within [lo, hi]");
  ok(falseDrops == 0, "a drop is never counted unless the ring played something out of order since the last pass");
  ok(missedLong == 0, "a dry spell longer than one buffer is counted (outside the refill after a drop)");
  ok(hiddenDry == 0, "every pass after ANY dry playback had lo < 0: minLead > 0 really means no dropout");
  ok(trueEpisodesTotal > 50 && drops > 0, "and the stalls did produce dropouts to count");
}

// ─── 4: MAINDATA, resume, false syncs ───────────────────────────────────────────────────

static void testReservoir() {
  group("MAINDATA_UNDERFLOW is stepped over, never handed back");
  const Stream st = makeMpeg1(44100, 192, 40, 200);       // every frame reaches 200 B back
  Mp3Stream s;
  s.begin();
  static int16_t pcm[MP3_MAX_FRAME_SAMPLES];
  Mp3Info info;
  const uint32_t f5 = st.frameOff[5];
  const uint32_t len5 = st.frameOff[6] - f5;
  s.fill(&st.bytes[f5], 3000);
  const size_t before = s.buffered();
  const int rc = s.decode(pcm, &info);
  ok(rc == MP3_SKIPPED_RESERVOIR, "a frame whose reservoir is missing says so (not 'need bytes')");
  ok(before - s.buffered() == len5 && s.lastFrameBytes() == len5,
     "and it is CONSUMED - exactly one frame - so the next call cannot re-feed it");
  const int rc2 = s.decode(pcm, &info);
  ok(rc2 == 2304, "the next frame decodes: the skipped one filled its reservoir");

  /* A whole resume: each frame decoded at most once, offsets strictly rising. */
  Mp3Stream r;
  r.begin();
  size_t pos = st.frameOff[11];
  uint32_t lastOk = 0;
  int good = 0, refed = 0, skips = 0;
  for (int i = 0; i < 200 && (pos < st.bytes.size() || r.buffered() > 0); i++) {
    if (r.space() > 0 && pos < st.bytes.size()) {
      size_t take = st.bytes.size() - pos;
      if (take > r.space()) {
        take = r.space();
      }
      pos += r.fill(&st.bytes[pos], take);
    }
    const int k = r.decode(pcm, &info);
    const uint32_t off = (uint32_t)(pos - r.buffered() - r.lastFrameBytes());
    if (k > 0) {
      if (good > 0 && off <= lastOk) {
        refed++;
      }
      lastOk = off;
      good++;
    } else if (k == MP3_SKIPPED_RESERVOIR) {
      skips++;
    } else if (k == 0 && pos >= st.bytes.size()) {
      break;
    }
  }
  ok(skips == 1, "a resume costs exactly one reservoir skip");
  ok(good == 40 - 11 - 1, "every later frame decodes exactly once");
  ok(refed == 0, "no frame is decoded twice (the 45-61% re-feed of 0.9.78)");

  /* reset() forgets helix's reservoir too: after it, the first frame reports the truth. */
  Mp3Stream z;
  z.begin();
  z.fill(&st.bytes[st.frameOff[1]], 3000);
  (void)z.decode(pcm, &info);                        // skip
  ok(z.decode(pcm, &info) == 2304, "(reservoir built)");
  z.reset();
  z.fill(&st.bytes[st.frameOff[20]], 3000);
  ok(z.decode(pcm, &info) == MP3_SKIPPED_RESERVOIR,
     "after reset() the old position's reservoir is gone (the 'loud burst' on resume)");
}

static void testFalseSync() {
  group("the output rate is chosen by two agreeing frames, not by a false sync");
  Stream st = makeMpeg1(44100, 192, 60);
  /* Inside frame 20's (zero) main data, a perfectly valid MPEG-2 16 kHz mono frame header — the
   * kind of thing album-art-free audio data contains by chance on ~1% of resume offsets. */
  const uint32_t fake = st.frameOff[20] + 36 + 100;
  st.bytes[fake] = 0xFF;
  st.bytes[fake + 1] = 0xF3;                          // MPEG-2, Layer III, no CRC
  st.bytes[fake + 2] = 0x48;                          // 32 kbps, 16 kHz
  st.bytes[fake + 3] = 0xC0;                          // mono
  {
    Mp3Stream s;
    s.begin();
    static int16_t pcm[MP3_MAX_FRAME_SAMPLES];
    Mp3Info info;
    memset(&info, 0, sizeof(info));
    s.fill(&st.bytes[fake], 4096);
    int first = 0;
    for (int i = 0; i < 8 && first <= 0; i++) {
      first = s.decode(pcm, &info);
    }
    ok(first > 0 && info.sampleRate == 16000,
       "(the trap is real: the FIRST decode from there is a clean 16 kHz frame)");
  }
  MemSource src;
  src.d = &st.bytes;
  Mp3Stream mp3;
  mp3.begin();
  static int16_t dec[2400];
  MusicFeed feed;
  feed.begin();
  ok(feed.openMp3(&src, &mp3, dec, 0, fake), "a resume at the false sync opens");
  ok(feed.outRate() == 22050, "and plays at 22.05 kHz (44.1 halved), not at 16 kHz (0.36x speed)");
}

static uint8_t* readFile(const char* path, size_t* len) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    fclose(f);
    return NULL;
  }
  uint8_t* b = (uint8_t*)malloc((size_t)n);
  const size_t got = b ? fread(b, 1, (size_t)n, f) : 0;
  fclose(f);
  *len = got;
  return b;
}

/* On a real track (the gitignored fixture): a resume two frames back, first decode dropped,
 * is bit-exact from the frame that was playing; and 200 random resume offsets all choose the
 * true rate (the old first-decode rule's misses are printed, not asserted: fixture-dependent). */
static void testRealResume() {
  group("resuming a real track (fixture)");
  size_t len = 0;
  uint8_t* file = readFile("tests/fixtures/mp3/track.mp3", &len);
  if (!file) {
    printf("  \033[33mSKIP\033[0m no fixture - run tools/gen_mp3_fixtures.sh <file.mp3>\n");
    return;
  }
  const uint32_t start = mp3Id3v2Size(file, len);
  /* The continuous decode: every frame's offset and PCM. */
  std::vector<uint32_t> offs;
  std::vector<std::vector<int16_t> > frames;
  int trueRate = 0;
  {
    Mp3Stream s;
    s.begin();
    size_t pos = start;
    static int16_t pcm[MP3_MAX_FRAME_SAMPLES];
    Mp3Info info;
    for (int i = 0; i < 100000 && frames.size() < 400; i++) {
      if (s.space() > 0 && pos < len) {
        size_t take = len - pos;
        if (take > s.space()) {
          take = s.space();
        }
        pos += s.fill(file + pos, take);
      }
      const int k = s.decode(pcm, &info);
      if (k > 0) {
        offs.push_back((uint32_t)(pos - s.buffered() - s.lastFrameBytes()));
        frames.push_back(std::vector<int16_t>(pcm, pcm + k));
        trueRate = info.sampleRate;
      } else if (k == 0 && pos >= len) {
        break;
      }
    }
  }
  ok(frames.size() > 100, "the fixture decodes continuously");
  int exact = 0, tried = 0, refed = 0;
  for (size_t k = 10; k + 5 < frames.size(); k += 23) {
    Mp3Stream s;
    s.begin();
    size_t pos = offs[k - 2];
    static int16_t pcm[MP3_MAX_FRAME_SAMPLES];
    Mp3Info info;
    int good = 0;
    uint32_t lastOff = 0;
    bool matched = false;
    for (int i = 0; i < 40 && good < 3; i++) {
      if (s.space() > 0 && pos < len) {
        size_t take = len - pos;
        if (take > s.space()) {
          take = s.space();
        }
        pos += s.fill(file + pos, take);
      }
      const int n = s.decode(pcm, &info);
      if (n > 0) {
        const uint32_t off = (uint32_t)(pos - s.buffered() - s.lastFrameBytes());
        if (good > 0 && off <= lastOff) {
          refed++;
        }
        lastOff = off;
        good++;
        if (good == 2) {                              // the first is the dropped transient
          size_t idx = 0;
          while (idx < offs.size() && offs[idx] != off) {
            idx++;
          }
          matched = idx < offs.size() && frames[idx].size() == (size_t)n &&
                    memcmp(frames[idx].data(), pcm, (size_t)n * 2) == 0;
        }
      }
    }
    tried++;
    if (matched) {
      exact++;
    }
  }
  note("%d resumes two frames back: %d bit-exact from the second decoded frame, %d re-fed", tried, exact, refed);
  ok(tried > 5 && exact == tried, "the frame after the dropped transient is bit-exact every time");
  ok(refed == 0, "and no frame is ever decoded twice");

  /* 200 random resume offsets through the feed's opening. */
  srand(777);
  int right = 0, oldWrong = 0;
  const int N = 200;
  for (int i = 0; i < N; i++) {
    const uint32_t at = start + 1000 + (uint32_t)(rand() % (int)(len - start - 60000));
    std::vector<uint8_t> v(file, file + len);
    MemSource src;
    src.d = &v;
    Mp3Stream mp3;
    mp3.begin();
    static int16_t dec[2400];
    MusicFeed feed;
    feed.begin();
    if (feed.openMp3(&src, &mp3, dec, start, at)) {
      const uint32_t want = trueRate >= MUSIC_HALF_RATE_FROM_HZ ? trueRate / 2 : trueRate;
      if (feed.outRate() == want) {
        right++;
      }
    }
    /* what 0.9.78's first-successful-decode rule would have configured */
    Mp3Stream o;
    o.begin();
    size_t pos = at;
    static int16_t pcm[MP3_MAX_FRAME_SAMPLES];
    Mp3Info info;
    for (int t = 0; t < 64; t++) {
      if (o.space() > 0 && pos < len) {
        size_t take = len - pos;
        if (take > 512) {
          take = 512;
        }
        if (take > o.space()) {
          take = o.space();
        }
        pos += o.fill(file + pos, take);
      }
      const int n = o.decode(pcm, &info);
      if (n > 0) {
        if (info.sampleRate != trueRate) {
          oldWrong++;
        }
        break;
      }
    }
  }
  note("%d/%d random resumes chose the true rate; the old first-decode rule got %d wrong", right, N, oldWrong);
  ok(right == N, "every resume chooses the true rate");
  free(file);
}

// ─── 5: mono, half rate ─────────────────────────────────────────────────────────────────

static double rmsOf(const std::vector<int16_t>& v, size_t from) {
  double s = 0;
  size_t n = 0;
  for (size_t i = from; i < v.size(); i++) {
    s += (double)v[i] * v[i];
    n++;
  }
  return n ? sqrt(s / n) : 0;
}

static std::vector<int16_t> halfOf(const std::vector<int16_t>& stereo, size_t chunk) {
  MusicPcm p;
  p.begin(true);
  std::vector<int16_t> out;
  std::vector<int16_t> work(MusicPcm::HIST + MUSIC_UNIT_FRAMES + 8);
  std::vector<int16_t> o(MUSIC_UNIT_FRAMES);
  const size_t frames = stereo.size() / 2;
  for (size_t f = 0; f < frames; f += chunk) {
    const size_t n = f + chunk <= frames ? chunk : frames - f;
    const size_t k = p.convert(&stereo[2 * f], n, 2, o.data(), work.data());
    out.insert(out.end(), o.begin(), o.begin() + k);
  }
  return out;
}

static void testPcm() {
  group("mono and half rate");
  const int fs = 44100;
  std::vector<int16_t> dc(2 * 11520, 10000);
  std::vector<int16_t> h = halfOf(dc, 1152);
  ok(h.size() == 5760, "2:1: 11,520 frames in, 5,760 samples out");
  bool exact = true;
  for (size_t i = 12; i < h.size(); i++) {
    exact = exact && h[i] == 10000;
  }
  ok(exact, "DC passes exactly (the taps sum to 32768)");

  auto tone = [&](double hz, double amp) {
    std::vector<int16_t> v(2 * 44100);
    for (int i = 0; i < 44100; i++) {
      const int16_t x = (int16_t)lrint(amp * sin(2 * M_PI * hz * i / fs));
      v[2 * i] = x;
      v[2 * i + 1] = x;
    }
    return v;
  };
  const std::vector<int16_t> t1 = tone(1000, 16000);
  const std::vector<int16_t> o1 = halfOf(t1, 1152);
  const double in1 = 16000 / sqrt(2.0);
  const double db1 = 20 * log10(rmsOf(o1, 100) / in1);
  const std::vector<int16_t> o18 = halfOf(tone(18000, 16000), 1152);
  const double db18 = 20 * log10(rmsOf(o18, 100) / in1 + 1e-9);
  const std::vector<int16_t> o8 = halfOf(tone(8000, 16000), 1152);
  const double db8 = 20 * log10(rmsOf(o8, 100) / in1);
  note("1 kHz %.2f dB, 8 kHz %.2f dB, 18 kHz %.1f dB", db1, db8, db18);
  ok(fabs(db1) < 0.1, "1 kHz passes (within 0.1 dB)");
  ok(db8 > -0.5, "8 kHz passes (within 0.5 dB)");
  ok(db18 < -45, "18 kHz, which would fold to 4.05 kHz, is gone (< -45 dB)");

  const std::vector<int16_t> a = halfOf(t1, 1152);
  const std::vector<int16_t> b = halfOf(t1, 576);
  const std::vector<int16_t> c = halfOf(t1, 44100 / 4 * 4 > 1152 ? 1152 : 1152);
  const std::vector<int16_t> d = halfOf(t1, 4);
  ok(a == b && a == c && a == d, "frame-by-frame, half frames or 4 at a time: identical output");

  MusicPcm p;
  p.begin(false);
  std::vector<int16_t> work(MusicPcm::HIST + 16), out(8);
  const int16_t lr[8] = {1000, -1000, 30000, 30000, -32768, -32768, 7, 9};
  const size_t n = p.convert(lr, 4, 2, out.data(), work.data());
  ok(n == 4 && out[0] == 0 && out[1] == 30000 && out[2] == -32768 && out[3] == 8,
     "(L+R)/2: out of phase cancels, full scale does not clip, no half-rate below 44.1 kHz");
}

// ─── 6: the end of a track ──────────────────────────────────────────────────────────────

static void testEnd() {
  group("the end of a track plays out whole, then silence");
  /* A WAV so every real sample is non-zero (a positive tone) and silence is identifiable. */
  const int fs = 44100, secs = 3;
  std::vector<uint8_t> w;
  const uint32_t dataBytes = (uint32_t)(fs * secs * 4);
  appendWavHeader(w, fs, 2, dataBytes);
  for (int i = 0; i < fs * secs; i++) {
    const int16_t x = (int16_t)(8000 + 3000 * sin(2 * M_PI * 440 * i / fs));
    for (int c = 0; c < 2; c++) {
      w.push_back((uint8_t)x);
      w.push_back((uint8_t)(x >> 8));
    }
  }
  WavInfo info;
  ok(wavParseHeader(w.data(), w.size(), &info), "(the WAV parses)");
  Clock clk;
  MemSource src;
  src.d = &w;
  src.clk = &clk;
  static int16_t dec[2400];
  SimFeed feed;
  feed.clk = &clk;
  feed.decodeUs = 0;
  feed.begin();
  ok(feed.openWav(&src, info, dec, 0), "a WAV opens");
  ok(feed.outRate() == 22050, "44.1 kHz WAV plays at 22.05 kHz too");
  DmaModel m(MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate());
  m.t0us = clk.us;
  ModelSink sink;
  sink.m = &m;
  sink.clk = &clk;
  sink.keepValues = true;
  MusicDma dma = {MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate(), 1};
  feed.start(&sink, dma);
  /* The real samples are the first fs*secs/2 written (the filter's output count). */
  m.lastRealId = (int64_t)fs * secs / 2 - 1;
  double endedAt = -1;
  uint32_t midPos = 0;
  int64_t midPlaying = -1;
  for (int i = 0; i < 1000 && endedAt < 0; i++) {
    clk.us += 10000;
    feed.pass(dma);
    if (i == 100) {
      m.advance(clk.us);
      midPos = feed.playingPos((uint64_t)clk.us);
      midPlaying = m.expect;
    }
    if (feed.ended()) {
      endedAt = clk.us;
    }
  }
  m.advance(clk.us);
  ok(endedAt > 0, "ended() arrives");
  ok(m.episodes == 0, "not one sample of the track was dropped or repeated");
  ok(!m.judging, "and the last sample had played by the time ended() said so");
  /* The track-change gap: from the last sample playing to ended() saying so. */
  const double lateMs = endedAt / 1000.0 - m.lastRealPlayedMs;
  note("ended() came %.0f ms after the last sample played (10 ms passes)", lateMs);
  ok(lateMs >= 0 && lateMs < 50, "and within a buffer and a pass of it (not a whole ring later)");
  /* A pass later the next track (or a stop) calls ceasePlayback(), which zeroes the ring. Until
   * then only the silence we queued may play. */
  clk.us += 60000;                                   // a slow pass
  m.advance(clk.us);
  ok(m.nonzeroAfterEnd == 0, "between the last sample and the next track only silence plays");
  const uint32_t truePos = info.dataOffset + (uint32_t)(midPlaying * 2 * 4);
  const long err = (long)midPos - (long)truePos;
  note("pause mid-track: resume at byte %u, the sample playing was at byte %u (%+ld bytes = %+.1f ms)",
       midPos, truePos, err, err / 4.0 / 44.1);
  ok(labs(err) <= 4L * 44 * 30, "a WAV resumes within 30 ms of the sample that was playing");
}

/* Pause and resume through the feed: the resume offset is the frame that was PLAYING (minus the
 * two the decoder needs), not the read position 4 KB + half a second ahead of it. */
static void testPlayingPos() {
  group("the resume offset is the frame that was playing");
  const Stream st = makeMpeg1(44100, 192, 1200);
  Clock clk;
  MemSource src;
  src.d = &st.bytes;
  src.clk = &clk;
  Mp3Stream mp3;
  mp3.begin();
  static int16_t dec[2400];
  SimFeed feed;
  feed.clk = &clk;
  feed.begin();
  feed.openMp3(&src, &mp3, dec, 0, 0);
  DmaModel m(MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate());
  m.t0us = clk.us;
  ModelSink sink;
  sink.m = &m;
  sink.clk = &clk;
  MusicDma dma = {MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, feed.outRate(), 1};
  feed.start(&sink, dma);
  int minBack = 1 << 30, maxBack = -(1 << 30);
  for (int i = 1; i <= 8; i++) {
    for (int k = 0; k < 150; k++) {
      clk.us += 17000;
      feed.pass(dma);
    }
    m.advance(clk.us);
    const uint32_t pos = feed.playingPos((uint64_t)clk.us);
    const int playingFrame = (int)(m.expect / 576);   // 576 output samples a frame
    int resumeFrame = 0;
    while (resumeFrame + 1 < (int)st.frameOff.size() && st.frameOff[resumeFrame + 1] <= pos) {
      resumeFrame++;
    }
    const int back = playingFrame - resumeFrame;
    if (getenv("MF_DEBUG")) {
      MusicStats x;
      feed.stats(&x, (uint64_t)clk.us);
      printf("    pos %u -> frame %d; model expect %lld -> frame %d; written %lld lead %d ms\n", pos, resumeFrame,
             (long long)m.expect, playingFrame, (long long)m.nextId, (int)x.leadMs);
    }
    if (back < minBack) {
      minBack = back;
    }
    if (back > maxBack) {
      maxBack = back;
    }
  }
  note("the resume offset lands %d-%d frames before the one playing (2 by design; the read position is ~26 ahead)",
       minBack, maxBack);
  ok(minBack >= 1 && maxBack <= 4, "two frames back, give or take the ring's one-buffer uncertainty");
}

/* Pause, wait, resume — on the SAME ring (no reinstall: the ring kept running, as it does for
 * the first 30 s of a pause). With closeRing() the resumed track plays in order within a buffer
 * or two; without it (the driver's half-filled buffer left behind) it is shown, not asserted. */
static void testSameRing() {
  group("pause and resume on the same ring: in order, and at once");
  const Stream st = makeMpeg1(44100, 192, 1500);
  long worstWith = 0, worstWithout = 0, scrambledWithout = 0, cases = 0;
  double worstLatWith = 0;
  for (int trial = 0; trial < 60; trial++)
  for (int variant = 0; variant < 2; variant++) {
    Clock clk;
    MemSource src;
    src.d = &st.bytes;
    src.clk = &clk;
    Mp3Stream mp3;
    mp3.begin();
    static int16_t dec[2400];
    SimFeed a;
    a.clk = &clk;
    a.begin();
    a.openMp3(&src, &mp3, dec, 0, 0);
    DmaModel m(MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, a.outRate());
    m.t0us = clk.us;
    ModelSink sink;
    sink.m = &m;
    sink.clk = &clk;
    MusicDma dma = {MUSIC_DMA_BUFS, MUSIC_DMA_BUF_SAMPLES, a.outRate(), 1};
    a.start(&sink, dma);
    const int before = 150 + (trial * 7) % 97;        // where in the ring the pause lands
    for (int i = 0; i < before; i++) {
      clk.us += 13000;
      a.pass(dma);
    }
    if (trial % 2) {
      /* The case closeRing() exists for: the pause lands right after a pass that could NOT
       * fill the ring (a stall drained it; the budget ran out first), so the driver is left
       * holding a half-filled buffer. */
      clk.us += 400000.0 + 9000.0 * (trial % 11);
      a.pass(dma);
    }
    const uint32_t pos = a.playingPos((uint64_t)clk.us);
    m.advance(clk.us);
    if (variant == 0) {
      a.closeRing();                                 // what Audio::ceasePlayback() does
    }
    a.stop();
    m.zeroContents();                                // ...and its i2s_zero_dma_buffer()
    const double pausedUs = 50000.0 + 97000.0 * (trial % 23);   // 0.05-2.2 s: the ring runs on, unwritten
    clk.us += pausedUs;
    m.advance(clk.us);
    m.rearm();
    const long ep0 = m.episodes;
    SimFeed b;
    b.clk = &clk;
    b.begin();
    b.openMp3(&src, &mp3, dec, 0, pos);
    const double tResume = clk.us;
    b.start(&sink, dma);
    for (int i = 0; i < 300; i++) {
      clk.us += 13000;
      b.pass(dma);
    }
    m.advance(clk.us);
    const long ep = m.episodes - ep0;
    /* The zeroed buffers still queued when the pause began play out first (the ring is half a
     * second deep, and ceasePlayback() only zeroes them): a resume within half a second waits
     * out the rest of that. Beyond it, a resume starts within a buffer or two. */
    const double ringMs = MUSIC_DMA_BUFS * MUSIC_DMA_BUF_SAMPLES * 1000.0 / a.outRate();
    double owed = ringMs - pausedUs / 1000.0;
    if (owed < 0) {
      owed = 0;
    }
    const double latency = m.started ? m.startedMs - tResume / 1000.0 - owed : 1e9;
    if (variant == 0) {
      cases++;
      if (ep > worstWith) {
        worstWith = ep;
      }
      if (!m.started) {
        worstWith = 1 << 20;
      }
      if (latency > worstLatWith) {
        worstLatWith = latency;
      }
    } else {
      if (ep > worstWithout || !m.started) {
        worstWithout = m.started ? ep : 1 << 20;
      }
      if (ep > 0 || !m.started) {
        scrambledWithout++;
      }
    }
  }
  note("%ld pauses at different points and lengths: with closeRing() worst %ld out-of-order episodes, "
       "worst start %.0f ms past the silence still queued; without it %ld of %ld came back out of order",
       cases, worstWith, worstLatWith, scrambledWithout, cases);
  ok(scrambledWithout > 0, "(the case is real: without closeRing() some resumes come back out of order)");
  ok(worstWith == 0, "with closeRing(): every resumed track plays entirely in order");
  ok(worstLatWith < 50, "and starts within a buffer or two of the silence the pause left queued");
}

int main() {
  testPcm();
  testSameRing();
  testReservoir();
  testFalseSync();
  testLeadBounds();
  testOldVsNew();
  testOtherShapes();
  testLongPassThenStall();
  testMapPan();
  testDropsAreTrue();
  testLeadNormalize();
  testReadErrors();
  testWavUnits();
  testEnd();
  testPlayingPos();
  testRealResume();

  printf("\n%s%d passed, %d failed\033[0m\n", g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
