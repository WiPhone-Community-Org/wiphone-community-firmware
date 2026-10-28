/* test_notify.cpp — the notification one-shot's stop timer, against the shipping sound.
 *
 * WHY THIS IS WORTH A SUITE: "sometimes the sound plays, sometimes it won't" and "sometimes
 * the motor just barely starts" were both the SAME fault — a timestamp taken at the top of a
 * main-loop pass, stamped onto the buzz and the pop after a mesh DB save had stalled that
 * pass by up to 1.5 s, so the next pass judged both finished a few ms after they began. The
 * stamps now come from millis() at the moment of the write; that part cannot be tested here.
 *
 * What CAN be tested is the arithmetic that replaced the literal 280 ms stop timer: it is
 * derived from the size of pop_pcm at the rate playPop() configures, and it has to stop EARLY
 * for the looping SPIFFS player (a late stop replays the attack, which is queued in the DMA
 * behind the tail) and LATE for the play-once memory source (an early stop cuts the tail).
 * The sound itself is included here so the claims the margin rests on — 90 ms of pop, then
 * silence to 320 ms, and an attack loud enough that replaying it would be heard — are pinned
 * to the bytes the phone ships, not to a comment.
 */
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "../WiPhone/notify_timing.h"
#include "../WiPhone/src/assets/pop_sound.h"

static int failures = 0;
static int checks = 0;

static void group(const char *name) {
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char *what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  } else {
    printf("  ok  %s\n", what);
  }
}

static int16_t sampleAt(size_t i) {
  // little-endian 16-bit signed, as the codec is fed
  return (int16_t)(pop_pcm[2 * i] | (pop_pcm[2 * i + 1] << 8));
}

/* ── A MODEL OF IDF 3.3's I2S TX RING, for the lead (integration review 3, A1) ────────────────
 * Just enough of driver/i2s.c to say WHEN each written sample reaches the DAC:
 *   - the DMA plays buffer `cur` a frame per tick and wraps round the ring;
 *   - the free-buffer queue is dma_buf_count - 1 DEEP, one short of the ring (objdump of the
 *     libdriver.a this build links: i2s_create_dma_queue() passes `addi.n a10, a3, -1` to
 *     xQueueGenericCreate), and it starts EMPTY on a fresh install;
 *   - at each buffer's end (the TX EOF interrupt) that buffer is pushed onto the queue - and when
 *     the queue is full the ISR first takes the OLDEST entry off and, with tx_desc_auto_clear
 *     (installI2S() sets it), zeroes it. With the queue one short, that oldest entry is the buffer
 *     the DMA is starting to play, so a full queue holds every buffer but the one playing;
 *   - i2s_write(timeout 0) takes a buffer off the queue only when it holds one, and fills it
 *     from its start; nothing written goes anywhere else. The writer's buffer is always left full
 *     (wbuf = -1 between pops): the pop's padding writes until the queue refuses. A half-filled
 *     curr_ptr (notify_timing.h's ⚠) is not modelled;
 *   - a RESTARTED ring (i2s_start() on a stopped one) keeps the queue the last session left, DMA
 *     back at buffer 0.
 * The writer is the LocalPcm source, run on each loop pass (the notify pump is the pass at t = 0,
 * the caller's stamp): the memory source writes pop_pcm's samples and then silence; the looping
 * SPIFFS file wraps to sample 0 and goes on. The pop teardown at the stop timer is ceasePlayback()'s
 * i2s_zero_dma_buffer(): every buffer zeroed, so a sample not yet played is never heard. Time is in
 * frames (8 per ms at 8 kHz). A written sample is tagged play * 2,560 + index. */
struct RingModel {
  static const int kBufs = (int)NOTIFY_POP_RING_BUFS;
  static const int kQueue = kBufs - 1;       // IDF: xQueueCreate(dma_buf_count - 1, ...)
  static const int kLen = (int)NOTIFY_POP_RING_BUF_SAMPLES;
  int data[kBufs][kLen];      // the tagged sample a slot holds, -1 = silence
  int queue[kQueue];
  int qn = 0;
  int cur = 0, pos = 0;       // the DMA
  int wbuf = -1, wpos = 0;    // the writer's current buffer (IDF's curr_ptr / rw_pos)
  RingModel() {
    zeroAll();
  }
  void zeroAll() {                           // i2s_zero_dma_buffer(): the queue is untouched
    for (int b = 0; b < kBufs; b++) {
      for (int i = 0; i < kLen; i++) {
        data[b][i] = -1;
      }
    }
  }
  void push(int b) {
    if (qn == kQueue) {                      // full: the ISR takes the oldest off and clears it
      const int old = pop();
      for (int i = 0; i < kLen; i++) {
        data[old][i] = -1;
      }
    }
    queue[qn++] = b;
  }
  int pop() {
    const int b = queue[0];
    for (int i = 1; i < qn; i++) {
      queue[i - 1] = queue[i];
    }
    qn--;
    return b;
  }
  /* Write from sample `next` on - to `total` and then silence, or round and round when `loops` -
   * until the queue has nothing to give. */
  void write(int& next, int total, bool loops) {
    for (;;) {
      if (wbuf < 0) {
        if (qn == 0) {
          return;                            // timeout 0: come back next pass
        }
        wbuf = pop();
        wpos = 0;
      }
      data[wbuf][wpos++] = (loops || next < total) ? next++ : -1;
      if (wpos == kLen) {
        wbuf = -1;
      }
    }
  }
  /* One frame out of the DAC. Returns the tagged sample heard, or -1. */
  int tick() {
    const int s = data[cur][pos];
    if (++pos == kLen) {
      push(cur);
      cur = (cur + 1) % kBufs;
      pos = 0;
    }
    return s;
  }
};

static const int kChirp = 720;               // the 90 ms of pop at the head of pop_pcm, pinned below

/* The ring as the pop finds it at the stamp. */
struct Scene {
  int queueInit;      // >= 0: RESTARTED, the queue full, starting at that buffer; < 0: installed empty
  uint32_t busyMs;    // first a writer kept the ring fed this long (the last pop), then its teardown
                      // zeroed it - the queue left as that writer drained it
  uint32_t idleMs;    // then the DMA ran unwritten this long (the SPIFFS open; a ring left idle)
  bool loops;         // the SPIFFS file (wraps) instead of pop_pcm (plays once, then silence)
};

struct Heard {
  int chirp;          // samples of the first play's chirp that reached the DAC
  int all;            // samples of the first play, all 2,560
  int second;         // samples of the SECOND play's chirp (only the looping file has one)
  int firstMs;        // ms after the stamp that the first play's sample 0 was heard, -1 = never
};

/* Run one pop on the model until the teardown at `stopMs` after the stamp. */
static Heard modelRun(const Scene& sc, uint32_t stopMs, uint32_t passMs) {
  RingModel m;
  if (sc.queueInit >= 0) {
    for (int i = 0; i < RingModel::kQueue; i++) {
      m.push((i + sc.queueInit) % RingModel::kBufs);
    }
  }
  const int total = (int)(sizeof(pop_pcm) / 2);
  const uint32_t fpm = NOTIFY_POP_RATE_HZ / 1000;        // frames per ms
  const uint32_t pass = passMs * fpm;
  for (uint32_t t = 0; t < sc.busyMs * fpm; t++) {
    if (t % pass == 0) {
      int done = total;
      m.write(done, total, false);           // the last pop's padding
    }
    m.tick();
  }
  if (sc.busyMs) {
    m.zeroAll();                             // its teardown
  }
  for (uint32_t t = 0; t < sc.idleMs * fpm; t++) {
    m.tick();
  }
  int next = 0;
  Heard h = {0, 0, 0, -1};
  for (uint32_t t = 0; t < stopMs * fpm; t++) {
    if (t % pass == 0) {
      m.write(next, total, sc.loops);        // the pump at t = 0, then every loop pass
    }
    const int s = m.tick();
    if (s < 0) {
      continue;
    }
    const int play = s / total, idx = s % total;
    if (play == 0) {
      h.all++;
      h.chirp += idx < kChirp ? 1 : 0;
      if (idx == 0 && h.firstMs < 0) {
        h.firstMs = (int)(t / fpm);
      }
    } else if (play == 1 && idx < kChirp) {
      h.second++;
    }
  }
  return h;                                  // the teardown zeroes what is left
}

int main() {
  group("notifyPcmMs: bytes at a rate -> playing time, rounded up");
  ok(notifyPcmMs(5120, 8000, 2) == 320, "5,120 bytes of 8 kHz 16-bit mono is 320 ms");
  ok(notifyPcmMs(1440, 8000, 2) == 90, "1,440 bytes (720 samples) is 90 ms");
  ok(notifyPcmMs(2, 8000, 2) == 1, "one sample rounds UP to 1 ms, never down to 0");
  ok(notifyPcmMs(1, 8000, 2) == 0, "a stray odd byte is no sample at all");
  ok(notifyPcmMs(5120, 0, 2) == 0 && notifyPcmMs(5120, 8000, 0) == 0,
     "a zero rate or sample size cannot divide by zero");
  ok(notifyPcmMs(5120, 16000, 2) == 160, "the same bytes at 16 kHz last half as long");

  group("notifyPopStopMs: the looping file player must stop EARLY");
  ok(notifyPopStopMs(5120, 8000, 40, true) == 280,
     "the shipping sound stops at 280 ms - the old literal, now derived");
  ok(notifyPopStopMs(5120, 8000, 40, true) < 320,
     "strictly before the wrap, so the second attack never sounds");
  ok(notifyPopStopMs(5120, 8000, 400, true) == 320,
     "a margin larger than the sound falls back to the sound's own length");

  group("notifyPopStopMs: the play-once memory source must stop LATE");
  ok(notifyPopStopMs(5120, 8000, 40, false) == 360,
     "the shipping sound stops at 360 ms - the whole array plus the margin");
  ok(notifyPopStopMs(5120, 8000, 40, false) > 320, "after the last sample has been queued");
  ok(notifyPopStopMs(5120, 8000, 40, false) < 640,
     "and well short of what a second full play would take");
  ok(notifyPopStopMs(160, 8000, 40, false) == 19,
     "a 10 ms sound with a 40 ms margin is capped at 19: no second play, ever");
  ok(notifyPopStopMs(0, 8000, 40, false) == 40 && notifyPopStopMs(0, 8000, 0, false) == 1,
     "an empty buffer still yields a non-zero timer, so the teardown always runs");

  group("the shipping pop_pcm: the facts the margin rests on");
  const size_t bytes = sizeof(pop_pcm);
  const size_t samples = bytes / 2;
  ok(bytes == 5120 && notifyPcmMs(bytes, NOTIFY_POP_RATE_HZ, NOTIFY_POP_BYTES_PER_SAMPLE) == 320,
     "5,120 bytes = 320 ms at the rate playPop() configures");
  size_t lastLoud = 0;
  for (size_t i = 0; i < samples; i++) {
    if (sampleAt(i) != 0) {
      lastLoud = i;
    }
  }
  const uint32_t audibleMs = (uint32_t)((lastLoud + 1) * 1000 / NOTIFY_POP_RATE_HZ);
  const uint32_t silenceMs = 320 - audibleMs;
  char line[200];
  snprintf(line, sizeof(line), "the sound is %u ms long and then silent for %u ms",
           (unsigned)audibleMs, (unsigned)silenceMs);
  ok(audibleMs == 90 && silenceMs == 230, line);
  ok((int)(lastLoud + 1) == kChirp, "the chirp is exactly 720 samples - the model's kChirp");
  ok(silenceMs >= NOTIFY_POP_MARGIN_MS,
     "so the file player's early stop cuts nothing but silence");
  int peakEarly = 0;
  for (size_t i = 0; i < NOTIFY_POP_MARGIN_MS * NOTIFY_POP_RATE_HZ / 1000; i++) {
    const int a = abs((int)sampleAt(i));
    if (a > peakEarly) {
      peakEarly = a;
    }
  }
  snprintf(line, sizeof(line), "the first %u ms peak at %d of 32767 - a replayed attack would be HEARD",
           (unsigned)NOTIFY_POP_MARGIN_MS, peakEarly);
  ok(peakEarly > 8000, line);

  group("the ring's lead: one trip round a fresh ring before the first written sample (A1)");
  const uint32_t trip = notifyRingTripMs(NOTIFY_POP_RING_BUFS, NOTIFY_POP_RING_BUF_SAMPLES,
                                         NOTIFY_POP_RATE_HZ);
  ok(trip == 512, "4 x 1024 mono at 8 kHz is 512 ms round");
  ok(notifyRingTripMs(4, 1023, 16000) == 256, "a stereo 1023 at 16 kHz rounds UP (255.75 -> 256)");
  ok(notifyRingTripMs(0, 1024, 8000) == 0 && notifyRingTripMs(4, 0, 8000) == 0 &&
     notifyRingTripMs(4, 1024, 0) == 0, "no ring (no driver) is no lead, and no divide by zero");
  ok(notifyPopLeadMs(NOTIFY_RING_FRESH, 4, 1024, 8000, false) == 512 &&
     notifyPopLeadMs(NOTIFY_RING_RESTARTED, 4, 1024, 8000, false) == 512 &&
     notifyPopLeadMs(NOTIFY_RING_RUNNING, 4, 1024, 8000, false) == 512,
     "the play-once source (stop LATE) takes the whole trip in EVERY state - each is bounded by one");
  ok(notifyPopLeadMs(NOTIFY_RING_FRESH, 4, 1024, 8000, true) == 0 &&
     notifyPopLeadMs(NOTIFY_RING_RESTARTED, 4, 1024, 8000, true) == 0 &&
     notifyPopLeadMs(NOTIFY_RING_RUNNING, 4, 1024, 8000, true) == 0,
     "the looping file (stop EARLY) takes none in any state - its open ran the first trip");
  ok(notifyPopLeadMs(NOTIFY_RING_FRESH, 0, 1024, 8000, false) == 0,
     "no ring, no lead");

  group("the stop timer with the lead");
  const uint32_t flashStop = notifyPopTimerMs(sizeof(pop_pcm), NOTIFY_POP_RATE_HZ, NOTIFY_POP_MARGIN_MS,
                                              false, notifyPopLeadMs(NOTIFY_RING_FRESH, 4, 1024, 8000, false));
  ok(flashStop == 872, "the shipping sound stops at 872 ms (360 + 512)");
  ok(flashStop > trip + 320, "after the array's last sample has reached the DAC (512 + 320)");
  ok(notifyPopTimerMs(sizeof(pop_pcm), NOTIFY_POP_RATE_HZ, NOTIFY_POP_MARGIN_MS, false, 0) == 360,
     "with no lead it is the old 360, unchanged");
  const uint32_t fileStop = notifyPopTimerMs(sizeof(pop_pcm), NOTIFY_POP_RATE_HZ, NOTIFY_POP_MARGIN_MS,
                                             true, notifyPopLeadMs(NOTIFY_RING_FRESH, 4, 1024, 8000, true));
  ok(fileStop == 280 && fileStop > 128 + 90 && fileStop < 320,
     "the looping file stops at 280 in every state: after the chirp (heard by 218), before the wrap (320)");

  group("the model, FRESH: the old 360 ms stop erased the chirp; the lead hears it");
  const int all = (int)(sizeof(pop_pcm) / 2);
  static const uint32_t passes[] = {5, 20, 50};
  const size_t nPasses = sizeof(passes) / sizeof(passes[0]);
  const Scene fresh = {-1, 0, 0, false};
  bool oldSilent = true, newWhole = true, arrayWhole = true;
  for (size_t p = 0; p < nPasses; p++) {
    oldSilent = oldSilent && modelRun(fresh, 360, passes[p]).chirp == 0;
    newWhole = newWhole && modelRun(fresh, flashStop, passes[p]).chirp == kChirp;
    arrayWhole = arrayWhole && modelRun(fresh, flashStop, passes[p]).all == all;
  }
  ok(oldSilent, "FRESH ring, stop at 360 ms: 0 of 720 chirp samples heard, at 5/20/50 ms passes "
                "(the bug: the teardown zeroed them ~150 ms before they were due)");
  ok(newWhole, "FRESH ring, stop at 872 ms: all 720 heard, at every pass length");
  ok(arrayWhole, "...and the whole 2,560-sample array, not just the chirp");
  ok(modelRun(fresh, 600, 5).firstMs == 512 && modelRun(fresh, 511, 5).chirp == 0,
     "the first sample reaches the DAC at exactly one trip (512 ms), not before");

  group("the model, RESTARTED and RUNNING: bounded by one trip, so the memory source takes it");
  int oldRestartMin = kChirp, newRestartMin = kChirp;
  for (int rot = 0; rot < RingModel::kBufs; rot++) {
    const Scene restarted = {rot, 0, 0, false};
    const int o = modelRun(restarted, 360, 20).chirp;
    const int n = modelRun(restarted, flashStop, 20).chirp;
    oldRestartMin = o < oldRestartMin ? o : oldRestartMin;
    newRestartMin = n < newRestartMin ? n : newRestartMin;
  }
  snprintf(line, sizeof(line), "RESTARTED ring, every queue order: old stop hears as few as %d of "
           "720, the stop with the trip hears %d", oldRestartMin, newRestartMin);
  ok(oldRestartMin < kChirp && newRestartMin == kChirp, line);
  /* Idle since its last write for 1,024 ms - a buffer boundary exactly at the stamp - so the DMA has
   * just begun a buffer: with the queue one short, the write goes to the NEXT one, 128 ms on. (The
   * old model's full-depth queue put it in the buffer playing, heard at once.) */
  const Scene idle = {-1, 0, 1024, false};
  const Heard hIdle = modelRun(idle, 360, 20);
  snprintf(line, sizeof(line), "RUNNING, idle 1 s: the first write lands in the NEXT buffer (heard at "
           "%d ms, one buffer), the chirp whole (%d) inside even the plain 360 stop", hIdle.firstMs,
           hIdle.chirp);
  ok(hIdle.firstMs == 128 && hIdle.chirp == kChirp, line);
  /* A burst: the last pop's padding drained the queue up to its teardown (872 ms of it on a fresh
   * ring), and this pop starts 0-50 ms later, before three buffers have come back. */
  static const uint32_t gaps[] = {0, 20, 50};
  int burstOldMax = 0, burstNewMin = kChirp;
  for (size_t g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++) {
    for (size_t p = 0; p < 2; p++) {                   // 5 and 20 ms passes
      const Scene burst = {-1, 872, gaps[g], false};
      const int o = modelRun(burst, 360, passes[p]).chirp;
      const int n = modelRun(burst, flashStop, passes[p]).chirp;
      burstOldMax = o > burstOldMax ? o : burstOldMax;
      burstNewMin = n < burstNewMin ? n : burstNewMin;
    }
  }
  snprintf(line, sizeof(line), "RUNNING, a pop 0-50 ms after the last one's teardown: the plain 360 "
           "stop hears at most %d of 720 (the premise 'RUNNING's queue is full' fails), the trip %d",
           burstOldMax, burstNewMin);
  ok(burstOldMax < kChirp / 2 && burstNewMin == kChirp, line);

  group("the model, the looping file: its SPIFFS open runs the ring before the stamp");
  /* FRESH ring, DMA started in turnOn(), then the open (0.5-1.6 s measured), then the stamp. */
  static const uint32_t opens[] = {500, 800, 1200, 1600};
  bool fileChirp = true, fileNoReplay = true, oldReplay = true;
  for (size_t o = 0; o < sizeof(opens) / sizeof(opens[0]); o++) {
    for (size_t p = 0; p < nPasses; p++) {
      const Scene sc = {-1, 0, opens[o], true};
      const Heard h = modelRun(sc, fileStop, passes[p]);
      fileChirp = fileChirp && h.chirp == kChirp;
      fileNoReplay = fileNoReplay && h.second == 0;
      oldReplay = oldReplay && modelRun(sc, 792, passes[p]).second == kChirp;
    }
  }
  ok(fileChirp, "opens of 0.5/0.8/1.2/1.6 s, stop at 280: the first chirp is heard whole");
  ok(fileNoReplay, "...and not one sample of the second attack");
  ok(oldReplay, "the first cut's 792 ms stop (the FRESH trip taken for the file) replayed the "
                "whole second attack, at every open and pass length");
  static const uint32_t fastOpens[] = {0, 100, 300};
  bool fastNoReplay = true;
  for (size_t o = 0; o < sizeof(fastOpens) / sizeof(fastOpens[0]); o++) {
    const Scene sc = {-1, 0, fastOpens[o], true};
    fastNoReplay = fastNoReplay && modelRun(sc, fileStop, 20).second == 0;
  }
  ok(fastNoReplay, "an open faster than 384 ms (never measured) may cut the chirp, but never replays it");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
