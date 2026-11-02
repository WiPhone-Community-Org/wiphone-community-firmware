/* cpu_clock_policy.h — which CPU frequency the gate may move to, kept in a header of its own so
 * the HOST SUITE tests the decisions the firmware executes (tests/test_cpuclock.cpp; the
 * menu_wrap.h pattern). cpu_clock.cpp does the register work; this file only decides.
 *
 * 🛑 THE RULE THIS FILE EXISTS FOR: THE BBPLL IS NEVER RE-LOCKED WHILE THE WiFi RADIO RUNS.
 *
 * MEASURED 2026-09-25 on phone 1 (uploader cycle = `up on books`, push one file, `up off`):
 *   0.9.78, the gate dropping 240->80 right after the WiFi traffic ....... broke 4 of 4
 *     (MARK disassoc ~10 s after `up off`, then every scan n=0 - deaf until a radio bounce)
 *   same, CPU held at 240 (no switch) ..................................... 0 of 3
 *   phone 2, always 240 (its GPS plate holds the clock) .................. 0 of 3
 *   phone 1, 20 screen-timeout switches with an IDLE radio ............... 0 of 20
 *
 * WHY 240<->80 IS A RE-LOCK AND NOT A DIVIDER. ESP32 TRM v5.8 Table 7.2-2 (CPU_CLK derivation)
 * has exactly three PLL rows, and each names its PLL:
 *     PLL_CLK (320 MHz)  CPUPERIOD_SEL=0  CPU = PLL_CLK/4 =  80 MHz
 *     PLL_CLK (320 MHz)  CPUPERIOD_SEL=1  CPU = PLL_CLK/2 = 160 MHz
 *     PLL_CLK (480 MHz)  CPUPERIOD_SEL=2  CPU = PLL_CLK/2 = 240 MHz
 * There is no /6 anywhere: SEL=0 with the PLL left at 480 is PLL/4 = 120 MHz by that formula,
 * a state no Espressif code ever creates (IDF v5's clk_ll: "ESP32 BBPLL frequency is determined
 * by the cpu freq sel"; its APB/PLL_F160M dividers key off the same field). So "keep the PLL at
 * 480 and take 80 from it" does not exist on this chip, and ESP-IDF's own DFS says so in
 * pm_esp32.c: "We can't switch between 240 and 80/160 without disabling PLL".
 * arduino's setCpuFrequencyMhz(80) from 240 therefore takes rtc_clk_cpu_freq_set_config's full
 * path (libsoc.a, disassembled): CPU to XTAL, rtc_clk_bbpll_disable(), enable, configure(320).
 * TRM 7.2.5: "Wi-Fi and BT can only operate if APB_CLK uses PLL_CLK ... Suspending PLL_CLK
 * requires Wi-Fi and BT to both have entered low-power consumption mode first." The gate did it
 * with the station awake after traffic, and the radio went deaf.
 *
 * THE SAME-PLL SWITCH THAT DOES EXIST: 80 <-> 160, both off PLL 320. That is one write to
 * CPUPERIOD_SEL (libsoc's rtc_clk_cpu_freq_set_config_fast takes its fast path when the PLL it
 * last configured matches: s_cur_pll_freq == config.source_freq_mhz, then
 * rtc_clk_cpu_freq_to_pll_mhz() - the PLL is not touched), and it is exactly what IDF's DFS does
 * with WiFi running when max_freq is 160 ("Otherwise, can use 80MHz CPU frequency when 80MHz APB
 * frequency is requested"). APB stays 80 MHz across it (TRM Table 7.2-4: PLL source -> 80 MHz).
 *
 * SO METHOD NEW IS:
 *   - radio OFF: anything goes - 240 for work, 80 for idle, exactly as before (a re-lock with
 *     no radio running is what the chip allows; the Game Boy, which turns WiFi off, keeps 240).
 *   - radio ON:  the PLL family is frozen. On PLL 320: 160 for work, 80 for idle. On PLL 480
 *     (only if the radio came up at 240 behind the gate's back): 240, and no idle drop until
 *     the radio next stops.
 *   - radio STARTING: cpu_clock.cpp wraps esp_wifi_start() and moves 240 -> 160 BEFORE the
 *     radio exists (cpuPreRadioTarget), so in practice the radio always runs on PLL 320.
 * ⚠ THE COST: with WiFi on, "full speed" is 160 MHz, not 240. Method OLD (`cpu method old`) is
 * the previous behaviour byte for byte, kept for the A/B bench.
 *
 * ⚠ Deliberately free of every Arduino and ESP-IDF header. Keep it that way.
 */
#ifndef CPU_CLOCK_POLICY_H
#define CPU_CLOCK_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum CpuClockMethod : uint8_t {
  CPU_METHOD_OLD = 0,   // arduino setCpuFrequencyMhz(240|80): a PLL re-lock on every change
  CPU_METHOD_NEW = 1,   // same PLL while the radio runs (the default)
};

/* The PLL frequency a documented PLL-sourced CPU frequency runs from (TRM Table 7.2-2); 0 for
 * anything else (XTAL, 8M, APLL), which the gate never targets. */
static inline uint32_t cpuPllMhzFor(uint32_t cpuMhz) {
  if (cpuMhz == 240) {
    return 480;
  }
  if (cpuMhz == 80 || cpuMhz == 160) {
    return 320;
  }
  return 0;
}

/* Would moving from `fromMhz` to `toMhz` re-lock (or start, or stop) the BBPLL? A move within
 * one PLL family is a divider write; anything else goes through rtc_clk_cpu_freq_set_config. */
static inline bool cpuSwitchRelocksPll(uint32_t fromMhz, uint32_t toMhz) {
  if (fromMhz == toMhz) {
    return false;
  }
  const uint32_t a = cpuPllMhzFor(fromMhz);
  return a == 0 || a != cpuPllMhzFor(toMhz);
}

/* The frequency the loop's gate should run at. `wantFull` is the gate's `busy` answer; `cur` is
 * the frequency now; `radioOn` = esp_wifi_start() has run and esp_wifi_stop() has not;
 * `rated160` = the efuse caps this chip at 160 (what setCpuFrequencyMhz(240) quietly maps).
 * 🛑 Invariant (tested exhaustively): under NEW with the radio on, the answer never re-locks. */
static inline uint32_t cpuGateTarget(CpuClockMethod m, bool wantFull, uint32_t cur, bool radioOn,
                                     bool rated160) {
  const uint32_t top = rated160 ? 160u : 240u;
  if (m == CPU_METHOD_OLD || !radioOn) {
    return wantFull ? top : 80u;
  }
  const uint32_t pll = cpuPllMhzFor(cur);
  if (pll == 480) {
    return cur;                       // held: 240 is the ONLY frequency PLL 480 gives
  }
  if (pll == 0) {
    return cur;                       // not on the PLL at all: not the gate's to touch
  }
  return wantFull ? 160u : 80u;       // PLL 320: a divider write either way
}

/* Where the clock must be BEFORE esp_wifi_start() brings the radio up. `cur` itself = no move.
 * Only NEW moves, and only off PLL 480: 160 rather than 80 because whatever turned the radio on
 * is doing work (the gate drops to 80 on its next pass if the phone is idle - same PLL). */
static inline uint32_t cpuPreRadioTarget(CpuClockMethod m, uint32_t cur) {
  if (m != CPU_METHOD_NEW) {
    return cur;
  }
  return cpuPllMhzFor(cur) == 480 ? 160u : cur;
}

/* ── THE `cpu cycle <n> <ms>` BENCH ────────────────────────────────────────────────────────────
 * Drives the gate's want down and up `n` times, one switch every `periodMs`, through the SAME
 * code the gate uses - so it reproduces (method old) or clears (method new) the 4-of-4 break on
 * demand. Down first: 240->80 right after traffic is the direction that broke. The job owns the
 * gate's input while it runs and hands it back after the last (up) switch. */
struct CpuCycleJob {
  uint32_t left;        // switches still to make (2 per cycle)
  uint32_t periodMs;
  uint32_t nextMs;      // millis() of the next switch
  bool     full;        // the want the job is holding now
};

static inline void cpuCycleArm(CpuCycleJob* j, uint32_t cycles, uint32_t periodMs, uint32_t now) {
  j->left = cycles * 2u;
  j->periodMs = periodMs;
  j->nextMs = now;      // the first switch (down) is due at once
  j->full = true;       // so the first toggle lands on idle
}

/* One gate pass. Returns false when no job is running (the caller's own want applies);
 * otherwise sets *wantFull and returns true. Wrap-safe across millis() rollover. */
static inline bool cpuCycleStep(CpuCycleJob* j, uint32_t now, bool* wantFull) {
  if (j->left == 0) {
    return false;
  }
  if ((int32_t)(now - j->nextMs) >= 0) {
    j->full = !j->full;
    j->left--;
    j->nextMs = now + j->periodMs;
  }
  *wantFull = j->full;
  return true;
}

/* The HEALTH line's field: " pll=<MHz> csw=<same-PLL>/<re-locks, radio off>/<re-locks, radio ON>".
 * The third number is the one that matters: it counts the event that broke phone 1's WiFi, so
 * under method NEW it must stay 0 for the whole boot. Returns what snprintf returns. */
static inline int cpuHealthField(char* out, size_t cap, uint32_t pllMhz, uint32_t samePll,
                                 uint32_t relockOff, uint32_t relockOn) {
  if (!out || cap == 0) {
    return 0;
  }
  return snprintf(out, cap, " pll=%lu csw=%lu/%lu/%lu", (unsigned long)pllMhz,
                  (unsigned long)samePll, (unsigned long)relockOff, (unsigned long)relockOn);
}

#endif  // CPU_CLOCK_POLICY_H
