/* test_cpuclock.cpp — the CPU clock gate's decisions (WiPhone/cpu_clock_policy.h).
 *
 * WHY THIS IS WORTH A SUITE: the fault it guards is SILENT and DELAYED. A gate that re-locks the
 * BBPLL under a running radio does not fail at the switch - the WiFi disassociates ~10 s later
 * and then hears nothing until a radio bounce (phone 1, 2026-09-25: 4 of 4 uploader cycles). No
 * log line says "the clock did it", so the one place the rule can be checked for EVERY state is
 * here. The functions under test are the REAL ones cpu_clock.cpp calls.
 *
 * The scenario at the bottom replays the bench: boot, the radio coming up, the uploader cycle
 * (`up on books`, traffic, `up off`), a Game Boy game, and back - once under method OLD (which
 * must reproduce the re-lock under the radio) and once under NEW (which must not).
 */
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "../WiPhone/cpu_clock_policy.h"

static int failures = 0;
static int checks = 0;

static void group(const char* name) {
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  } else {
    printf("  ok  %s\n", what);
  }
}

/* A phone in miniature: the clock, the radio, and the counters cpu_clock.cpp keeps - driven by
 * the same two policy calls the firmware makes (the esp_wifi_start() wrapper and the gate). */
struct Sim {
  CpuClockMethod m;
  bool rated160;
  uint32_t cur;
  bool radio;
  int samePll, relockOff, relockOn;
};

static void simMove(Sim* s, uint32_t to) {
  if (to == s->cur) {
    return;
  }
  if (!cpuSwitchRelocksPll(s->cur, to)) {
    s->samePll++;
  } else if (s->radio) {
    s->relockOn++;
  } else {
    s->relockOff++;
  }
  s->cur = to;
}
static void simRadioStart(Sim* s) {
  if (!s->radio) {
    simMove(s, cpuPreRadioTarget(s->m, s->cur));   // before the radio exists
  }
  s->radio = true;
}
static void simRadioStop(Sim* s) {
  s->radio = false;
}
static void simGate(Sim* s, bool wantFull) {
  simMove(s, cpuGateTarget(s->m, wantFull, s->cur, s->radio, s->rated160));
}

/* The bench, as a script: what the phone did on 2026-09-25 plus a game and a rescue bounce. */
static Sim runDay(CpuClockMethod m, bool rated160) {
  Sim s = {m, rated160, 240, false, 0, 0, 0};   // the app starts at 240 on PLL 480
  simRadioStart(&s);                             // setup(): wifiState.init() -> WiFi.mode(STA)
  simGate(&s, true);                             // boot splash, screen lit
  simGate(&s, false);                            // screen times out
  for (int i = 0; i < 4; i++) {                  // the uploader cycle, 4 times
    simGate(&s, true);                           //   `up on books`: xferOn() -> busy
    simGate(&s, true);                           //   the push
    simGate(&s, false);                          //   `up off`: idle AT ONCE, right after traffic
  }
  simGate(&s, true);                             // screen on, pick a game
  simRadioStop(&s);                              // startGame(): WiFi.mode(WIFI_OFF)
  simMove(&s, cpuGateTarget(s.m, true, s.cur, s.radio, s.rated160));   // cpuClockRaise("gbc")
  simGate(&s, true);                             // playing
  simRadioStart(&s);                             // quit: WiFi.mode(WIFI_STA)
  simGate(&s, false);                            // screen off
  simRadioStop(&s);                              // `wifi bounce`: off...
  simRadioStart(&s);                             // ...and on
  simGate(&s, true);
  simGate(&s, false);
  return s;
}

int main() {
  group("cpuPllMhzFor: the three PLL rows of TRM Table 7.2-2, nothing else");
  ok(cpuPllMhzFor(80) == 320, "80 runs off PLL 320 (CPUPERIOD_SEL 0, /4)");
  ok(cpuPllMhzFor(160) == 320, "160 runs off PLL 320 (CPUPERIOD_SEL 1, /2)");
  ok(cpuPllMhzFor(240) == 480, "240 runs off PLL 480 (CPUPERIOD_SEL 2, /2)");
  ok(cpuPllMhzFor(40) == 0, "40 is the XTAL, not the PLL");
  ok(cpuPllMhzFor(120) == 0, "120 (PLL 480 / 4) is not a state the gate knows - there is no /6");
  ok(cpuPllMhzFor(0) == 0, "0 is nothing");

  group("cpuSwitchRelocksPll: a divider inside one PLL, a re-lock across");
  ok(!cpuSwitchRelocksPll(80, 80), "no move, no re-lock");
  ok(!cpuSwitchRelocksPll(80, 160), "80 -> 160: divider only");
  ok(!cpuSwitchRelocksPll(160, 80), "160 -> 80: divider only");
  ok(cpuSwitchRelocksPll(240, 80), "240 -> 80: RE-LOCK (480 -> 320) - the move that broke phone 1");
  ok(cpuSwitchRelocksPll(80, 240), "80 -> 240: re-lock (320 -> 480)");
  ok(cpuSwitchRelocksPll(240, 160), "240 -> 160: re-lock");
  ok(cpuSwitchRelocksPll(160, 240), "160 -> 240: re-lock");
  ok(cpuSwitchRelocksPll(40, 80), "XTAL -> PLL starts the PLL: counted as a re-lock");
  ok(cpuSwitchRelocksPll(80, 40), "PLL -> XTAL stops it: counted as a re-lock");

  group("method OLD: the previous gate, byte for byte (240 busy, 80 idle, radio or not)");
  {
    const uint32_t curs[] = {80, 160, 240};
    bool allOld = true;
    for (uint32_t c : curs) {
      for (int radio = 0; radio < 2; radio++) {
        allOld &= cpuGateTarget(CPU_METHOD_OLD, true, c, radio, false) == 240;
        allOld &= cpuGateTarget(CPU_METHOD_OLD, false, c, radio, false) == 80;
      }
    }
    ok(allOld, "every (cur, radio): full = 240, idle = 80");
    ok(cpuGateTarget(CPU_METHOD_OLD, true, 80, false, true) == 160,
       "a chip rated 160: full = 160 (what setCpuFrequencyMhz(240) maps it to anyway)");
    ok(cpuPreRadioTarget(CPU_METHOD_OLD, 240) == 240, "OLD never moves the clock before the radio starts");
  }

  group("method NEW, radio OFF: exactly the old behaviour (a re-lock with no radio is allowed)");
  ok(cpuGateTarget(CPU_METHOD_NEW, true, 80, false, false) == 240, "busy -> 240 (the Game Boy's clock)");
  ok(cpuGateTarget(CPU_METHOD_NEW, false, 240, false, false) == 80, "idle -> 80");
  ok(cpuGateTarget(CPU_METHOD_NEW, true, 160, false, false) == 240, "from 160, busy -> 240");
  ok(cpuGateTarget(CPU_METHOD_NEW, true, 80, false, true) == 160, "rated 160: busy -> 160, never 240");

  group("method NEW, radio ON: the PLL family is frozen");
  ok(cpuGateTarget(CPU_METHOD_NEW, true, 80, true, false) == 160, "PLL 320, busy -> 160 (not 240: that is a re-lock)");
  ok(cpuGateTarget(CPU_METHOD_NEW, false, 160, true, false) == 80, "PLL 320, idle -> 80");
  ok(cpuGateTarget(CPU_METHOD_NEW, true, 160, true, false) == 160, "PLL 320, busy at 160 stays");
  ok(cpuGateTarget(CPU_METHOD_NEW, false, 80, true, false) == 80, "PLL 320, idle at 80 stays");
  ok(cpuGateTarget(CPU_METHOD_NEW, false, 240, true, false) == 240,
     "PLL 480 (radio came up at 240): idle is HELD at 240 - 80 would re-lock under the radio");
  ok(cpuGateTarget(CPU_METHOD_NEW, true, 240, true, false) == 240, "PLL 480, busy stays 240");
  ok(cpuGateTarget(CPU_METHOD_NEW, false, 40, true, false) == 40, "not on the PLL: left alone");
  {
    const uint32_t curs[] = {40, 80, 160, 240};
    bool never = true;
    int cases = 0;
    for (uint32_t c : curs) {
      for (int full = 0; full < 2; full++) {
        for (int rated = 0; rated < 2; rated++) {
          const uint32_t t = cpuGateTarget(CPU_METHOD_NEW, full, c, true, rated);
          never &= !cpuSwitchRelocksPll(c, t);
          cases++;
        }
      }
    }
    char what[128];
    snprintf(what, sizeof(what), "🛑 INVARIANT: no NEW answer re-locks the PLL with the radio on (%d cases)", cases);
    ok(never, what);
  }
  {
    bool oldBreaks = cpuSwitchRelocksPll(240, cpuGateTarget(CPU_METHOD_OLD, false, 240, true, false));
    ok(oldBreaks, "and OLD does, from 240 with the radio on (the invariant is not vacuous)");
  }

  group("cpuPreRadioTarget: off PLL 480 BEFORE the radio exists");
  ok(cpuPreRadioTarget(CPU_METHOD_NEW, 240) == 160, "240 -> 160 (work was asked for; the gate drops to 80 if idle)");
  ok(!cpuSwitchRelocksPll(cpuPreRadioTarget(CPU_METHOD_NEW, 240), 80),
     "and from there idle is a divider, not a re-lock");
  ok(cpuPreRadioTarget(CPU_METHOD_NEW, 80) == 80, "80 stays (already PLL 320)");
  ok(cpuPreRadioTarget(CPU_METHOD_NEW, 160) == 160, "160 stays");
  ok(cpuPreRadioTarget(CPU_METHOD_NEW, 40) == 40, "the XTAL is not the wrapper's business");

  group("the bench day, replayed: boot, 4 uploader cycles, a game, a bounce");
  {
    const Sim o = runDay(CPU_METHOD_OLD, false);
    char what[160];
    snprintf(what, sizeof(what), "OLD re-locks the PLL under the radio (%d times) - the 4/4 break reproduces", o.relockOn);
    ok(o.relockOn >= 4, what);
    const Sim n = runDay(CPU_METHOD_NEW, false);
    snprintf(what, sizeof(what), "NEW: 0 re-locks with the radio on (%d same-PLL, %d re-locks radio off)",
             n.samePll, n.relockOff);
    ok(n.relockOn == 0, what);
    ok(n.samePll >= 8, "NEW still moves: the uploader cycles are 160 <-> 80 divider writes");
    ok(n.relockOff == 3, "NEW's only re-locks are 3 radio-off edges: boot, game in, game out (the bounce finds 80)");
    ok(n.cur == 80 && n.radio, "and it ends idle at 80 with the radio up");
    const Sim r = runDay(CPU_METHOD_NEW, true);
    ok(r.relockOn == 0, "a chip rated 160: 0 re-locks with the radio on too");
    ok(r.cur == 80, "rated 160 ends idle at 80");
  }
  {
    /* The radio comes up at 240 behind the gate's back is impossible through the wrapper, but
     * `cpu method old` -> `cpu method new` at 240 with the radio up gets there: held, not broken. */
    Sim s = {CPU_METHOD_OLD, false, 80, false, 0, 0, 0};
    simRadioStart(&s);
    simGate(&s, true);                      // OLD: 80 -> 240 under the radio (counted)
    s.m = CPU_METHOD_NEW;                   // the A/B switch
    const int before = s.relockOn;
    simGate(&s, false);
    ok(s.cur == 240 && s.relockOn == before, "method new at 240 with the radio on: held at 240, no re-lock");
    simRadioStop(&s);
    simGate(&s, false);
    ok(s.cur == 80 && s.relockOn == before, "the radio stops: 240 -> 80 is allowed now");
    simRadioStart(&s);
    simGate(&s, true);
    ok(s.cur == 160 && s.relockOn == before, "back up: PLL 320, busy = 160");
  }

  group("cpuCycleStep: n down/up pairs, down first, the gate handed back after the last up");
  {
    CpuCycleJob j;
    cpuCycleArm(&j, 3, 500, 1000);
    bool w = true;
    int toggles = 0;
    bool last = true;
    bool downFirst = false;
    uint32_t t = 1000;
    for (; t < 1000 + 10 * 500; t += 50) {
      bool want = true;
      if (!cpuCycleStep(&j, t, &want)) {
        break;
      }
      if (t == 1000) {
        downFirst = !want;
      }
      if (want != last) {
        toggles++;
        last = want;
      }
      w = want;
    }
    ok(downFirst, "the first switch is DOWN (240->80 after traffic is the direction that broke)");
    ok(toggles == 6, "3 pairs = 6 switches");
    ok(w == true, "the last one is UP");
    ok(t == 3550, "the job lets go one pass after the last switch (at 3500 ms), not before");
    bool want = false;
    ok(!cpuCycleStep(&j, t + 5000, &want), "a finished job stays finished");
  }
  {
    CpuCycleJob j;
    cpuCycleArm(&j, 1, 0x200, 0xFFFFFF00u);   // straddles millis() rollover
    bool want = true;
    ok(cpuCycleStep(&j, 0xFFFFFF00u, &want) && !want, "down at arm time, just before the wrap");
    ok(cpuCycleStep(&j, 0x00000050u, &want) && !want, "still down just after the wrap (0x150 ms in)");
    ok(cpuCycleStep(&j, 0x00000100u, &want) && want, "up once 0x200 ms have passed across the wrap");
    ok(!cpuCycleStep(&j, 0x00000110u, &want), "and done");
  }
  {
    CpuCycleJob j;
    cpuCycleArm(&j, 5, 100, 0);
    cpuCycleArm(&j, 0, 0, 0);                 // `cpu cycle 0`
    bool want = true;
    ok(!cpuCycleStep(&j, 10, &want), "0 cycles = stop: the gate's own want applies at once");
  }

  group("cpuHealthField: the HEALTH line's pll= csw=");
  {
    char b[64];
    const int n = cpuHealthField(b, sizeof(b), 320, 12, 3, 0);
    ok(!strcmp(b, " pll=320 csw=12/3/0"), "\" pll=320 csw=12/3/0\"");
    ok(n == (int)strlen(b), "returns the length written");
    char small[8];
    memset(small, 'x', sizeof(small));
    cpuHealthField(small, sizeof(small), 480, 1, 2, 3);
    ok(small[7] == '\0' && !strncmp(small, " pll=48", 7), "truncates inside the buffer, terminated");
    ok(cpuHealthField(b, 0, 320, 0, 0, 0) == 0, "cap 0 writes nothing");
    ok(cpuHealthField(NULL, 16, 320, 0, 0, 0) == 0, "no buffer writes nothing");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
