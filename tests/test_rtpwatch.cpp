/* test_rtpwatch.cpp — the two clocks on a call's RTP session (WiPhone/rtp_watch.h).
 *
 * WHY THIS IS WORTH A SUITE: both clocks exist because of state that outlived the thing it
 * described. The silence rule kept its last-packet time and its window count for the WHOLE
 * BOOT, so the second call of a boot scored the gap since the first call as silence and was
 * torn down at connect — with no BYE, and nothing on screen to say why. Nobody could have seen
 * it while SIP never registered; both phones register now. The orphan clock is the backstop for
 * the hot-mic class: an RTP session left armed with no call to own it.
 *
 * The OLD silence rule is reproduced below, line for line from the pre-0.9.79 Audio.cpp and
 * WiPhone.ino, and driven through the same boot as the new one — so the suite shows the fault
 * and the fix side by side instead of asserting a claim about code that is gone.
 *
 * WiPhone.ino and Audio.cpp cannot be compiled here (Arduino, I2S, the codec); the header is
 * the whole of the decision, and they only feed it millis() and whether a packet arrived.
 */
#include <cstdio>
#include <cstdint>

#include "../WiPhone/rtp_watch.h"

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

/* ── The pre-0.9.79 rule, verbatim in behaviour ────────────────────────────────────────────
 * Audio.cpp (RtpStream branch, per pass):
 *     if (len == 0 && millis() - rtpSilentScan > 60000) { rtpSilentScan = millis(); FLAG = ON; }
 *     if (len > 0) { rtpSilentScan = millis(); FLAG = OFF; }
 * WiPhone.ino (top of the next pass):
 *     if (FLAG == ON) { FLAG = OFF; if (cnt == 1) { cnt = 0; END THE CALL } cnt++; }
 * Nothing reset either variable per call; both start at 0 at boot. */
struct OldRule {
  uint32_t scan = 0;
  uint8_t  cnt = 0;
  bool     flag = false;

  // One main-loop pass. Returns true when the loop would end the call.
  bool pass(uint32_t now, bool packet) {
    bool ended = false;
    if (flag) {                         // the loop consumes last pass's flag first
      flag = false;
      if (cnt == 1) {
        cnt = 0;
        ended = true;
      }
      cnt++;
    }
    if (!packet && now - scan > 60000u) {
      scan = now;
      flag = true;
    }
    if (packet) {
      scan = now;
      flag = false;
    }
    return ended;
  }
};

/* The new rule, wired the way Audio.cpp and WiPhone.ino wire it: newCall() begins, a packet is
 * heard, a packet-less pass is quiet, and RTP_QUIET_END raises the flag the loop acts on.
 * ⚠ A RE-IMPLEMENTATION of that wiring, so it proves the header, not the callers: reverting the
 * begin in Audio::newCall() leaves this suite green. tests/check_call_audio.py is what pins the
 * real calls in place (a review found exactly that gap). */
struct NewRule {
  RtpSilence s = {0, 0};
  bool flag = false;

  void newCall(uint32_t now) {
    rtpSilenceBegin(s, now);
    flag = false;
  }
  bool pass(uint32_t now, bool packet) {
    bool ended = false;
    if (flag) {
      flag = false;
      ended = true;
    }
    if (!packet && rtpSilenceQuiet(s, now) == RTP_QUIET_END) {
      flag = true;
    }
    if (packet) {
      rtpSilenceHeard(s, now);
      flag = false;
    }
    return ended;
  }
};

/* A call: connect at `start`, the first `quietLeadMs` without packets (the first pass always
 * precedes the first packet), then a packet every 20 ms until `end`. Passes every 10 ms.
 * Returns the ms after `start` at which the call was ended, or -1 if it ran to `end`. */
template <typename Rule>
static long runCall(Rule& r, uint32_t start, uint32_t end, uint32_t quietLeadMs, bool begin) {
  if (begin) {
    r.newCall(start);
  }
  for (uint32_t t = start; t != end; t += 10) {
    const bool packet = (t - start) >= quietLeadMs && ((t - start) % 20) == 0;
    if (r.pass(t, packet)) {
      return (long)(t - start);
    }
  }
  return -1;
}

struct OldRuleAdapter : OldRule {
  void newCall(uint32_t) {}             // the old code had nothing to call here
};

int main() {
  group("the numbers");
  ok(RTP_SILENCE_WINDOW_MS == 60000u, "one window is 60 s (the old STP_SILENT_PERIOD)");
  ok(RTP_SILENCE_WINDOW_MS * RTP_SILENCE_STRIKES == 120000u,
     "a call ends after 120 s of continuous silence");
  ok(RTP_ORPHAN_RELEASE_MS == 3000u, "an orphaned session is released after 3 s");

  group("THE FAULT: the old rule ends the second call of a boot at connect");
  {
    OldRuleAdapter old;
    // Call 1 at 2 min after boot, 60 s long; call 2 five minutes later.
    const long c1 = runCall(old, 120000u, 180000u, 30, false);
    ok(c1 < 0, "old rule: the first call of the boot survives (its first window only counts 1)");
    const long c2 = runCall(old, 480000u, 540000u, 30, false);
    char line[128];
    snprintf(line, sizeof(line),
             "old rule: the SECOND call is ended %ld ms after it connects - before any packet",
             c2);
    ok(c2 >= 0 && c2 < 30, line);
  }

  group("THE FIX: the same boot under the new rule");
  {
    NewRule neu;
    ok(runCall(neu, 120000u, 180000u, 30, true) < 0, "the first call survives");
    ok(runCall(neu, 480000u, 540000u, 30, true) < 0, "the second call survives its connect");
    ok(runCall(neu, 900000u, 960000u, 30, true) < 0, "and the third");
    // A call placed at the very first moment after boot: the old rule measured from 0.
    NewRule early;
    ok(runCall(early, 61000u, 121000u, 30, true) < 0,
       "a call a minute after boot is not scored against millis() = 0");
  }

  /* WHY Audio::newCall() MUST CALL rtpSilenceBegin() — the wiring, not just the header. The
   * clock is one object for the whole boot (Audio.cpp's s_rtpSilence), so without a per-call
   * begin a strike left over from the LAST call is still on it. A call that hangs up with one
   * strike outstanding (a minute or more of far-end silence, then END) leaves strikes = 1, and
   * the next call's first packet-less pass — which always precedes its first packet — scores the
   * gap since that strike as the SECOND window: RTP_QUIET_END, the call torn down at connect by
   * TinySIP::rtpSilent(), no BYE. tests/check_call_audio.py pins the begin inside newCall(). */
  group("the per-call begin is load-bearing: a strike outstanding at hang-up");
  {
    NewRule neu;
    // Call 1: connects at 10 s, talks for 20 s, then the far end goes quiet for 70 s and the
    // user hangs up. One strike, still outstanding when the call ends.
    neu.newCall(10000u);
    bool ended = false;
    uint32_t t = 10000u;
    for (; t < 30000u; t += 10) {
      ended |= neu.pass(t, (t % 20) == 0);
    }
    for (; t < 100000u; t += 10) {
      ended |= neu.pass(t, false);
    }
    ok(!ended && neu.s.strikes == 1, "call 1 hangs up with ONE strike outstanding, still up");

    NewRule noBegin = neu;             // the same boot, the next call WITHOUT newCall()'s begin
    const long c2 = runCall(noBegin, 400000u, 460000u, 30, false);
    char line[128];
    snprintf(line, sizeof(line),
             "no begin: the next call is ended %ld ms after it connects - before any packet", c2);
    ok(c2 >= 0 && c2 < 30, line);

    ok(runCall(neu, 400000u, 460000u, 30, true) < 0,
       "with the begin: the next call starts with no strikes and survives");
  }

  group("120 s of CONTINUOUS silence ends a call, and nothing shorter does");
  {
    RtpSilence s;
    rtpSilenceBegin(s, 1000u);
    long strikeAt = -1, endAt = -1;
    for (uint32_t t = 1000u; t <= 1000u + 130000u; t += 10) {
      const RtpQuiet q = rtpSilenceQuiet(s, t);
      if (q == RTP_QUIET_STRIKE && strikeAt < 0) {
        strikeAt = (long)(t - 1000u);
      }
      if (q == RTP_QUIET_END) {
        endAt = (long)(t - 1000u);
        break;
      }
    }
    ok(strikeAt == 60010, "the first strike lands on the first pass PAST 60 s");
    ok(endAt == 120020, "the end lands on the first pass past the SECOND whole window");
  }
  {
    RtpSilence s;
    rtpSilenceBegin(s, 0u);
    ok(rtpSilenceQuiet(s, 60000u) == RTP_QUIET_NONE, "exactly 60,000 ms is not yet a window");
    ok(rtpSilenceQuiet(s, 60001u) == RTP_QUIET_STRIKE, "60,001 ms is");
    ok(rtpSilenceQuiet(s, 120001u) == RTP_QUIET_NONE,
       "the second window is measured from the strike, not from the start");
    ok(rtpSilenceQuiet(s, 120002u) == RTP_QUIET_END, "and ends the call one window later");
    ok(s.strikes == 0, "the END clears the strikes");
  }

  group("a packet clears the strikes: two separate quiet minutes are not a dead call");
  {
    NewRule neu;
    neu.newCall(0u);
    bool ended = false;
    uint32_t t = 0;
    for (; t < 61000u; t += 10) {       // 61 s of nothing: one strike
      ended |= neu.pass(t, false);
    }
    ok(!ended && neu.s.strikes == 1, "61 s quiet: one strike, the call goes on");
    ended |= neu.pass(t, true);         // the far end speaks
    ok(neu.s.strikes == 0, "one packet: no strikes");
    for (t += 10; t < 123000u; t += 10) {  // another 61 s of nothing
      ended |= neu.pass(t, false);
    }
    ok(!ended, "a second, SEPARATE quiet minute does not end the call");

    OldRule old;
    bool oldEnded = false;
    for (t = 0; t < 61000u; t += 10) {
      oldEnded |= old.pass(t, false);
    }
    oldEnded |= old.pass(t, true);
    for (t += 10; t < 123000u; t += 10) {
      oldEnded |= old.pass(t, false);
    }
    ok(oldEnded, "(the old rule ended it: its count never heard the packet)");
  }

  group("millis() wrapping mid-call");
  {
    RtpSilence s;
    const uint32_t start = 0xFFFFFFFFu - 30000u;   // 30 s before the wrap
    rtpSilenceBegin(s, start);
    ok(rtpSilenceQuiet(s, start + 50000u) == RTP_QUIET_NONE, "50 s in, across the wrap: nothing");
    ok(rtpSilenceQuiet(s, start + 60001u) == RTP_QUIET_STRIKE, "60 s in, across the wrap: a strike");
  }

  group("the orphan clock: never under a live call");
  {
    RtpOrphan o = {0, false};
    bool fired = false;
    // A two-hour call, a pass every 10 ms: armed and owned the whole time.
    for (uint32_t t = 5000u; t < 5000u + 2u * 3600u * 1000u; t += 10) {
      fired |= rtpOrphanCheck(o, true, true, t);
    }
    ok(!fired, "armed AND owned for two hours: never fires");

    // The real shape of a call's life: owned from the pass it is armed, and shut down in the
    // pass that leaves the live set (a teardown can lag one stalled pass - 1.5 s measured).
    RtpOrphan r = {0, false};
    bool f = false;
    f |= rtpOrphanCheck(r, false, true, 0u);       // ringing: owned, nothing armed
    f |= rtpOrphanCheck(r, true, true, 10u);       // Call + armed in the same pass
    f |= rtpOrphanCheck(r, true, false, 20u);      // left the set; shutdown lags...
    f |= rtpOrphanCheck(r, true, false, 1520u);    // ...by one 1.5 s stalled pass
    f |= rtpOrphanCheck(r, false, false, 1530u);   // shut down
    ok(!f, "a teardown one stalled pass late does not trip it");
  }

  group("the orphan clock: an armed session with nothing to own it is released");
  {
    RtpOrphan o = {0, false};
    ok(!rtpOrphanCheck(o, true, false, 100u), "first unowned pass starts the clock");
    ok(!rtpOrphanCheck(o, true, false, 3099u), "2,999 ms: not yet");
    ok(rtpOrphanCheck(o, true, false, 3100u), "3,000 ms: fires");
    ok(!rtpOrphanCheck(o, true, false, 3110u),
       "fires ONCE - if nobody shuts it down it starts timing again");
    ok(rtpOrphanCheck(o, true, false, 6110u), "...and fires again 3 s later");
  }
  {
    RtpOrphan o = {0, false};
    bool f = false;
    for (uint32_t t = 0; t < 60000u; t += 10) {
      f |= rtpOrphanCheck(o, false, false, t);
    }
    ok(!f, "nothing armed: never fires, owned or not");
  }
  {
    RtpOrphan o = {0, false};
    rtpOrphanCheck(o, true, false, 0u);
    rtpOrphanCheck(o, true, false, 2900u);
    rtpOrphanCheck(o, true, true, 2950u);          // one owned pass
    ok(!rtpOrphanCheck(o, true, false, 3000u), "one owned pass restarts the clock");
    ok(!rtpOrphanCheck(o, true, false, 5999u), "so 2,999 ms later: not yet");
    ok(rtpOrphanCheck(o, true, false, 6000u), "3,000 ms after the restart: fires");
  }
  {
    RtpOrphan o = {0, false};
    const uint32_t t0 = 0xFFFFFFFFu - 1000u;
    rtpOrphanCheck(o, true, false, t0);
    ok(!rtpOrphanCheck(o, true, false, t0 + 2000u), "across the millis() wrap: 2 s is not yet");
    ok(rtpOrphanCheck(o, true, false, t0 + 3000u), "across the millis() wrap: 3 s fires");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
