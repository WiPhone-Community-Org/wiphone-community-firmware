/*
 * battery_curve.h — state of charge from cell voltage, off phone 1's measured discharge.
 *
 * 🔑 THE CW2015 GAUGE RUNS AN UNPROGRAMMED BATTERY MODEL. Its BATINFO profile — the 64-byte
 * cell characterisation CellWise generates from the actual cell — is never written anywhere in
 * this firmware (CW2015::configure() only clears MODE), so `soc` comes from a generic curve.
 * Measured on phone 1's first genuine full-to-empty run (2026-09-02, WiFi off, 9h16m,
 * tests/fixtures/p1_discharge_2026-09-03.tsv): the chip read ~7 points optimistic through the
 * middle and then hit 0 % at 3.56 V (up=471 min; 3.54 V by up=479) while the cell ran on to
 * 3.30 V — **1.42 hours (85 min, 15.5 % of the run) of real runtime reported as empty.** Mean
 * error 5.7 points, worst 16.4. (First written as "3.54 V / 1.3 h" from the up=479 sample; the
 * fixture's first soc=0 row is up=471 — corrected 2026-09-03 afternoon.)
 *
 * This table replaces it. SOC is defined as the fraction of usable runtime left at the
 * measured load: 100 % at the unplug, 0 % at the last sample before the firmware's own 3.30 V
 * power-off (WiPhone.ino battery tick: `v <= 3.3` -> powerOff(); NOT a brown-out), linear in
 * time between (constant load ⇒ constant current ⇒ charge linear in time). Voltage was
 * binned at 10 mV and the 50 mV breakpoints below are the bin means. Against the run it
 * was built from: mean error 0.46 points, worst 2.2 (tests/test_battery.cpp pins both).
 *
 * ⚠ WHAT THIS IS AND IS NOT.
 *  - It is PHONE 1's curve (UFX 603048A, 900 mAh, no backplate) at an IDLE, WIFI-OFF load —
 *    the woods configuration. {user}'s decision 2026-09-03: this is the firmware's standard;
 *    phone 2 and other people's phones use it as the best guess we have.
 *  - It is load-dependent. Voltage sags under load, so with the screen on or WiFi hunting
 *    the same true SOC reads a LOWER voltage and this table UNDER-reports — the conservative
 *    direction, which is the right way for a battery display to be wrong.
 *  - It is fitted to ONE run and tested against that run. That proves the table reproduces
 *    the curve, not that the curve is the same on another day. A second phone-1 run would.
 *  - 🛑 IT IS A DISCHARGE CURVE AND MUST NOT BE SHOWN WHILE CHARGING. Measured 2026-09-03, 45 min
 *    into a charge from empty at 3.89 V: this table said 61 %, the chip said 20 %. Charge current
 *    lifts the terminal voltage ~100 mV, so the same true SOC reads a HIGHER voltage and the
 *    table over-reports by 30-40 points — optimistic, the wrong way to be wrong. WiPhone.ino's
 *    battery tick therefore shows the CHIP's number while `battCharged` and this curve otherwise.
 *    A charge-side correction would need a logged full charge; phone 1's health.log is recording
 *    one (soc= vs est= vs v=, once a minute) as this is written.
 *  - ⚠ Battery experiments happen on PHONE 1. Phone 2 wears the backplate, whose cell feeds
 *    the internal one and holds it at 4.1-4.2 V for hours; `v`, `soc` and duration all
 *    measure two cells there and nothing in the log can tell them apart.
 *
 * Arduino-free; the host suite compiles it against the recorded run.
 */

#ifndef BATTERY_CURVE_H
#define BATTERY_CURVE_H

/* State of charge, 0..100, from the cell voltage in volts. Clamped: anything at or below
 * the floor reads 0, anything at or above the top reads 100. Linear between breakpoints. */
int batterySocFromVoltage(float volts);

/* The table's ends, so callers and tests do not restate them. */
#define BATTERY_CURVE_FLOOR_MV 3300   // the firmware's own power-off threshold (WiPhone.ino `v <= 3.3`), not a brown-out
#define BATTERY_CURVE_TOP_MV   4160   // first sample after the unplug, surface charge relaxed

#endif // BATTERY_CURVE_H
