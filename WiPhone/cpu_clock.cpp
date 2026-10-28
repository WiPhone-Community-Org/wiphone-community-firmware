/* cpu_clock.cpp — the CPU frequency, moved without re-locking the PLL under a running radio.
 * The rule and the evidence are in cpu_clock_policy.h. This file is the register work, the
 * esp_wifi_start()/esp_wifi_stop() wrappers that tell it whether the radio runs, and the counters
 * the `cpu` command and the HEALTH line print.
 *
 * WHAT A SWITCH HERE DOES, AND WHAT IT DELIBERATELY DOES NOT (vs arduino's setCpuFrequencyMhz,
 * esp32-hal-cpu.c:143-212, which method OLD still calls):
 *   - rtc_clk_cpu_freq_mhz_to_config() + rtc_clk_cpu_freq_set_config_fast(). Disassembled from
 *     this core's libsoc.a: the fast path runs iff config.source == PLL and config.source_freq_mhz
 *     == s_cur_pll_freq (the PLL libsoc last configured), and is then rtc_clk_cpu_freq_to_pll_mhz:
 *     write DPORT_CPU_PER_CONF_REG, DIG_DBIAS_WAK, SOC_CLK_SEL=PLL, rtc_clk_apb_freq_update(80 MHz),
 *     ets_update_cpu_frequency(mhz), wait one slow cycle - the PLL is not touched. Anything else
 *     falls back to rtc_clk_cpu_freq_set_config (XTAL, BBPLL off, on, configure), which the
 *     policy only ever asks for while the radio is stopped.
 *   - _xt_tick_divisor, the FreeRTOS tick (CCOMPARE counts CPU cycles) - as arduino does.
 *   - NOT the APB-change callbacks, NOT rtc_clk_apb_freq_update/esp_timer_impl_update_apb_freq
 *     from outside: APB is 80 MHz before and after every move this module makes (TRM Table 7.2-4:
 *     a PLL-sourced CPU clock gives an 80 MHz APB; arduino's own calculateApb() agrees for any
 *     freq >= 80), so there is nothing for a peripheral to re-divide. Skipping them is also what
 *     makes this switch immune to the gGpsNmea UART deadlock (WiPhone.ino): that deadlock lives
 *     INSIDE uart_on_apb_change, which the core fires even when APB does not move.
 *   - Inside a critical section, so a re-lock's XTAL window (~100-150 us: two slow-clock waits and
 *     the 80 us BBPLL enable delay) cannot be stretched by a context switch, and so the radio-on
 *     test and the switch are one step against the esp_wifi_start() wrapper.
 */
#include "cpu_clock.h"

#include <Arduino.h>
#include <esp_wifi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/xtensa_timer.h"   // _xt_tick_divisor, XT_TICK_PER_SEC
#include "soc/rtc.h"                 // rtc_clk_cpu_freq_{mhz_to_config,set_config_fast,get_config}
#include "soc/efuse_reg.h"           // the 160-MHz rating, as setCpuFrequencyMhz reads it
#include "rom/uart.h"                // uart_tx_wait_idle: the console, before an XTAL window

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool              s_inited   = false;
static bool              s_rated160 = false;
static CpuClockMethod    s_method   = CPU_METHOD_NEW;
static volatile bool     s_radioOn  = false;   // written under s_mux by the start wrapper
static volatile uint32_t s_curMhz   = 0;       // what this module last set (hardware at init)

// Counters: bumped under s_mux (the wrapper can run on the WiFi event task, the gate on the loop).
static volatile uint32_t s_samePll = 0, s_relockOff = 0, s_relockOn = 0, s_held = 0, s_preRadio = 0;
static volatile bool     s_preRadioNote = false;     // the wrapper cannot log (event task): the gate does
static volatile uint32_t s_preRadioFrom = 0, s_preRadioTo = 0;

// The last gate/raise switch, for `cpu`. Loop task only.
static bool        s_anySwitch = false;
static uint32_t    s_lastFrom = 0, s_lastTo = 0, s_lastMs = 0;
static bool        s_lastRelock = false, s_lastRadioOn = false;
static const char* s_lastWhy = "";
static bool        s_heldNoted = false;
static uint32_t    s_failedTarget = 0;             // log a refused/failed target once, not per pass

static CpuCycleJob s_cycle = {0, 0, 0, false};
static bool        s_cycleRunning = false;
static uint32_t    s_cycleBase[3] = {0, 0, 0};

static uint32_t hwMhz(uint32_t* pllOut) {
  rtc_cpu_freq_config_t c;
  rtc_clk_cpu_freq_get_config(&c);
  if (pllOut) {
    *pllOut = (c.source == RTC_CPU_FREQ_SRC_PLL) ? c.source_freq_mhz : 0;
  }
  return c.freq_mhz;
}

void cpuClockInit() {
  if (s_inited) {
    return;
  }
  s_rated160 = REG_GET_BIT(EFUSE_BLK0_RDATA3_REG, EFUSE_RD_CHIP_CPU_FREQ_RATED) &&
               REG_GET_BIT(EFUSE_BLK0_RDATA3_REG, EFUSE_RD_CHIP_CPU_FREQ_LOW);
  s_curMhz = hwMhz(NULL);
  s_inited = true;
}

/* Called with s_mux held and `conf` already built for the target. */
static inline void applyLocked(const rtc_cpu_freq_config_t* conf) {
  rtc_clk_cpu_freq_set_config_fast(conf);
  _xt_tick_divisor = conf->freq_mhz * 1000000u / XT_TICK_PER_SEC;
  s_curMhz = conf->freq_mhz;
}

/* Method NEW. Returns true if it switched; *relock says which kind it was. The radio test is
 * repeated INSIDE the lock: a radio that started since the policy was asked wins, and a re-lock
 * is refused (the next pass asks again and gets a same-PLL answer). */
static bool switchNew(uint32_t target, bool* relock, bool* radioAtSwitch) {
  rtc_cpu_freq_config_t conf;
  if (!rtc_clk_cpu_freq_mhz_to_config(target, &conf)) {
    return false;
  }
  if (cpuSwitchRelocksPll(s_curMhz, target)) {
    uart_tx_wait_idle(CONFIG_CONSOLE_UART_NUM);   // console bytes do not survive an XTAL window
  }
  bool did = false;
  portENTER_CRITICAL(&s_mux);
  const bool r = cpuSwitchRelocksPll(s_curMhz, target);
  *relock = r;
  *radioAtSwitch = s_radioOn;
  if (s_curMhz != target && !(r && s_radioOn)) {
    applyLocked(&conf);
    if (r) {
      s_relockOff++;
    } else {
      s_samePll++;
    }
    did = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return did;
}

/* Method OLD: the previous behaviour, byte for byte - arduino's setCpuFrequencyMhz with its PLL
 * re-lock and its APB callbacks. Counted so the A/B shows what it did under the radio. */
static bool switchOld(uint32_t target, bool* relock, bool* radioAtSwitch) {
  const uint32_t from = s_curMhz;
  *relock = cpuSwitchRelocksPll(from, target);
  *radioAtSwitch = s_radioOn;
  if (!setCpuFrequencyMhz(target)) {
    return false;
  }
  const uint32_t now = hwMhz(NULL);
  portENTER_CRITICAL(&s_mux);
  s_curMhz = now;
  if (!*relock) {
    s_samePll++;
  } else if (*radioAtSwitch) {
    s_relockOn++;
  } else {
    s_relockOff++;
  }
  portEXIT_CRITICAL(&s_mux);
  return true;
}

/* Move to `target` and say so. Loop task (the gate and cpuClockRaise). */
static void moveTo(uint32_t target, const char* why) {
  const uint32_t from = s_curMhz;
  bool relock = false, radio = false;
  const bool did = (s_method == CPU_METHOD_NEW) ? switchNew(target, &relock, &radio)
                                                : switchOld(target, &relock, &radio);
  if (!did) {
    if (s_curMhz == target) {
      return;                         // the start wrapper got there first: nothing to say
    }
    if (s_failedTarget != target) {
      s_failedTarget = target;
      log_e("CPU %luMHz (%s) NOT set: %s", (unsigned long)target, why,
            (relock && radio) ? "a PLL re-lock with the radio on - refused (method new)"
                              : "the core refused the frequency");
    }
    return;
  }
  s_failedTarget = 0;
  s_heldNoted = false;
  s_anySwitch = true;
  s_lastFrom = from;
  s_lastTo = s_curMhz;
  s_lastMs = millis();
  s_lastRelock = relock;
  s_lastRadioOn = radio;
  s_lastWhy = why;
  uint32_t pll = 0;
  const uint32_t hw = hwMhz(&pll);
  /* "CPU <n>MHz (<why>)" is the line the gate has always printed; the tail is new. */
  if (!relock) {
    log_e("CPU %luMHz (%s) same PLL %lu - divider only", (unsigned long)hw, why, (unsigned long)pll);
  } else {
    log_e("CPU %luMHz (%s) PLL %lu->%lu re-lock, radio %s%s", (unsigned long)hw, why,
          (unsigned long)cpuPllMhzFor(from), (unsigned long)pll, radio ? "ON" : "off",
          radio ? " - THE 4/4 BREAK (method old)" : "");
  }
  if (hw != target) {
    log_e("CPU WARNING: asked for %lu MHz, the hardware reads %lu MHz", (unsigned long)target, (unsigned long)hw);
  }
}

void cpuClockGate(bool wantFull, const char* why) {
  cpuClockInit();
  if (s_preRadioNote) {
    s_preRadioNote = false;
    log_e("CPU %luMHz (before the radio started) PLL 480->320 re-lock with the radio still OFF "
          "(was %lu MHz): the radio runs on PLL 320, so 80<->160 is a divider from here",
          (unsigned long)s_preRadioTo, (unsigned long)s_preRadioFrom);
  }
  bool cyc = false;
  if (cpuCycleStep(&s_cycle, millis(), &cyc)) {
    wantFull = cyc;
    why = cyc ? "cycle up" : "cycle down";
    s_cycleRunning = true;
  } else if (s_cycleRunning) {
    s_cycleRunning = false;
    log_e("CPU cycle done (method %s): %lu same-PLL, %lu PLL re-locks radio off, %lu WITH THE RADIO ON",
          s_method == CPU_METHOD_NEW ? "new" : "old", (unsigned long)(s_samePll - s_cycleBase[0]),
          (unsigned long)(s_relockOff - s_cycleBase[1]), (unsigned long)(s_relockOn - s_cycleBase[2]));
  }
  const uint32_t cur = s_curMhz;
  const uint32_t target = cpuGateTarget(s_method, wantFull, cur, s_radioOn, s_rated160);
  if (target == cur) {
    if (!wantFull && cur == 240 && s_radioOn && s_method == CPU_METHOD_NEW && !s_heldNoted) {
      s_heldNoted = true;
      portENTER_CRITICAL(&s_mux);
      s_held++;
      portEXIT_CRITICAL(&s_mux);
      log_e("CPU held at 240MHz (%s): the PLL is at 480 and the radio is on - 80 would re-lock it "
            "under the radio. It drops when the radio next stops.", why);
    }
    return;
  }
  moveTo(target, why);
}

void cpuClockRaise(const char* why) {
  cpuClockInit();
  const uint32_t cur = s_curMhz;
  const uint32_t target = cpuGateTarget(s_method, true, cur, s_radioOn, s_rated160);
  if (target > cur) {
    moveTo(target, why);
  }
}

CpuClockMethod cpuClockMethod() {
  return s_method;
}

void cpuClockSetMethod(CpuClockMethod m) {
  s_method = m;
  s_heldNoted = false;
  s_failedTarget = 0;
}

bool cpuClockRadioOn() {
  return s_radioOn;
}

void cpuClockCycleStart(uint32_t cycles, uint32_t periodMs) {
  s_cycleBase[0] = s_samePll;
  s_cycleBase[1] = s_relockOff;
  s_cycleBase[2] = s_relockOn;
  cpuCycleArm(&s_cycle, cycles, periodMs, millis());
  s_cycleRunning = false;
}

void cpuClockStatus(CpuClockStatus* o) {
  cpuClockInit();
  o->mhz = hwMhz(&o->pllMhz);
  o->believedMhz = s_curMhz;
  o->method = s_method;
  o->radioOn = s_radioOn;
  o->rated160 = s_rated160;
  o->samePll = s_samePll;
  o->relockOff = s_relockOff;
  o->relockOn = s_relockOn;
  o->held = s_held;
  o->preRadio = s_preRadio;
  o->anySwitch = s_anySwitch;
  o->lastFrom = s_lastFrom;
  o->lastTo = s_lastTo;
  o->lastAgoMs = millis() - s_lastMs;
  o->lastRelock = s_lastRelock;
  o->lastRadioOn = s_lastRadioOn;
  o->lastWhy = s_lastWhy;
  o->cycleLeft = s_cycle.left;
  o->cyclePeriodMs = s_cycle.periodMs;
}

int cpuClockHealth(char* out, size_t cap) {
  uint32_t pll = 0;
  hwMhz(&pll);
  return cpuHealthField(out, cap, pll, s_samePll, s_relockOff, s_relockOn);
}

/* ── THE RADIO'S TWO EDGES, SEEN WHEREVER THEY COME FROM ──────────────────────────────────────
 * platformio.ini links with -Wl,--wrap=esp_wifi_start and --wrap=esp_wifi_stop, so EVERY caller
 * lands here first: the core's espWiFiStart/espWiFiStop, the two bare esp_wifi_start() calls in
 * GUI.cpp, Networks::disable()'s bare esp_wifi_stop(), a scan's enableSTA(true) - and the core's
 * event-task auto-reconnect, whose WiFi.begin() restarts a radio that disconnect(true) just
 * stopped (the Networks::bounceRadio note). ⚠ That last one runs on the WiFi EVENT task: nothing
 * here may log or allocate, only the critical-section stores below.
 *
 * ⚠ Tracked at the IDF call, NOT with WiFi.getMode(): the core's getMode() answers from its own
 * _esp_wifi_started, which the GUI's bare esp_wifi_start() never sets - it reads NULL with the
 * radio running. And not with an event either: STA_START arrives after the PHY is already up.
 *
 * START: under method NEW, a PLL-480 clock is moved to 160 FIRST (the radio does not exist yet,
 * so the re-lock is the allowed kind), and the radio is marked on in the same critical section -
 * so a gate pass on the other core either finished before, or sees the radio. Marked on BEFORE
 * the real call on purpose (the PHY comes up inside it); unmarked only if the start failed from
 * a stopped radio.
 * STOP: marked off only after esp_wifi_stop() says OK. A plain store: this also runs from the
 * shutdown handler on esp_restart(). */
extern "C" esp_err_t __real_esp_wifi_start(void);
extern "C" esp_err_t __real_esp_wifi_stop(void);

extern "C" esp_err_t __wrap_esp_wifi_start(void) {
  cpuClockInit();
  /* The console drain is keyed on a guess made OUTSIDE the lock (uart_tx_wait_idle spins on the
   * UART and must not run with interrupts off). The DECISION is made inside it, from the s_curMhz
   * the lock protects: decided out here, a gate pass on the other core that re-locked 80->240
   * (radio off, allowed) between the read and the lock left `want` stale, the in-lock re-check
   * failed, and the radio came up on PLL 480 - safe, but idle then held at 240 until the radio
   * next stopped, possibly for hours (review, 2026-09-25). A lost race now costs at most a few
   * console bytes in the XTAL window. rtc_clk_cpu_freq_mhz_to_config is fit for the lock: IRAM
   * (esp32.project.ld places libsoc rtc_clk.* there) and, disassembled, a table lookup whose only
   * call is rtc_clk_xtal_freq_get(), one register read. */
  if (!s_radioOn && cpuPreRadioTarget(s_method, s_curMhz) != s_curMhz) {
    uart_tx_wait_idle(CONFIG_CONSOLE_UART_NUM);
  }
  rtc_cpu_freq_config_t conf;
  portENTER_CRITICAL(&s_mux);
  const bool wasOn = s_radioOn;
  const uint32_t from = s_curMhz;
  const uint32_t want = cpuPreRadioTarget(s_method, from);
  if (!wasOn && want != from && rtc_clk_cpu_freq_mhz_to_config(want, &conf)) {
    applyLocked(&conf);
    s_relockOff++;
    s_preRadio++;
    s_preRadioFrom = from;
    s_preRadioTo = s_curMhz;
    s_preRadioNote = true;
  }
  s_radioOn = true;
  portEXIT_CRITICAL(&s_mux);
  const esp_err_t r = __real_esp_wifi_start();
  if (r != ESP_OK && !wasOn) {
    s_radioOn = false;
  }
  return r;
}

extern "C" esp_err_t __wrap_esp_wifi_stop(void) {
  const esp_err_t r = __real_esp_wifi_stop();
  if (r == ESP_OK) {
    s_radioOn = false;
  }
  return r;
}
