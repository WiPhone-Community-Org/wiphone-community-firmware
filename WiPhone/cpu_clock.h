/* cpu_clock.h — the one place the CPU frequency changes (0.9.79 dev, 2026-09-25).
 *
 * The loop's gate (WiPhone.ino, "cpu-tick") says full or idle; this module says what that means
 * in MHz without ever re-locking the BBPLL under a running WiFi radio. The rule, the TRM rows and
 * the measurement behind it are in cpu_clock_policy.h; the register work is in cpu_clock.cpp.
 *
 * 🛑 NOTHING ELSE MAY CALL setCpuFrequencyMhz(). Two reasons, both measured: it re-locks the PLL
 * on any 240<->80/160 move (phone 1's deaf WiFi, 4 of 4), and it fires the core's APB-change
 * callbacks even when APB does not move, which parks the loop forever with the GPS UART streaming
 * at 115200 (the gGpsNmea deadlock note in WiPhone.ino). Method NEW does neither.
 */
#ifndef CPU_CLOCK_H
#define CPU_CLOCK_H

#include <stddef.h>
#include <stdint.h>
#include "cpu_clock_policy.h"

/* Reads the hardware and the efuse rating. Idempotent; setup() calls it first, and every entry
 * point below calls it too (the esp_wifi_start() wrapper can run before setup gets that far). */
void cpuClockInit();

/* THE GATE: once per loop pass with the gate's own answer (busy -> full). Moves only on a change.
 * `why` must be a string literal (it is kept by pointer for `cpu`). Loop task only. */
void cpuClockGate(bool wantFull, const char* why);

/* Raise-only, for work that cannot wait for the bottom of the pass (the Game Boy, straight after
 * it has turned WiFi off). Never lowers the clock; same PLL rule as the gate. */
void cpuClockRaise(const char* why);

CpuClockMethod cpuClockMethod();
void           cpuClockSetMethod(CpuClockMethod m);   // RAM only: every boot starts NEW
bool           cpuClockRadioOn();                      // esp_wifi_start() ran, esp_wifi_stop() did not

/* `cpu cycle <n> <ms>`: n down/up pairs through the gate's own code, one switch per `ms`.
 * 0 cycles stops a running job. */
void cpuClockCycleStart(uint32_t cycles, uint32_t periodMs);

struct CpuClockStatus {
  uint32_t mhz;            // the hardware, read now
  uint32_t pllMhz;         // the PLL it runs from (0 = not on the PLL)
  uint32_t believedMhz;    // what this module last set; != mhz means something switched behind it
  CpuClockMethod method;
  bool     radioOn;
  bool     rated160;
  uint32_t samePll;        // divider-only switches (80<->160, or 240<->... never)
  uint32_t relockOff;      // PLL re-locks with the radio stopped (allowed)
  uint32_t relockOn;       // PLL re-locks with the radio RUNNING - the fault; NEW keeps this 0
  uint32_t held;           // times idle was refused because PLL 480 + radio on (NEW)
  uint32_t preRadio;       // 240->160 moves made by the esp_wifi_start() wrapper
  bool     anySwitch;
  uint32_t lastFrom, lastTo, lastAgoMs;
  bool     lastRelock, lastRadioOn;
  const char* lastWhy;
  uint32_t cycleLeft, cyclePeriodMs;
};
void cpuClockStatus(CpuClockStatus* out);

/* The HEALTH line's " pll=<MHz> csw=<same>/<re-lock radio off>/<re-lock radio ON>". */
int cpuClockHealth(char* out, size_t cap);

#endif  // CPU_CLOCK_H
