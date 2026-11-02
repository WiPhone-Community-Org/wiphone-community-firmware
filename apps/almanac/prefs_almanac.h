/*
 * prefs_almanac.h - the device-wide settings of the Almanac release (0.9.80), held in ONE
 * place so every app reads the same value: the map's scale bar and distances, the altitude
 * readout, the Meshtastic lists, the serial console and the Almanac all look at gUnits; the
 * Almanac's legal-light countdown looks at gLegalRule. docs/almanac.md, "Units" and "Legal light".
 *
 * NVS namespace "wpmesh" (the GPS switch's), keys <= 15 characters:
 *   "units"      0 = metric (the default: what the firmware always printed), 1 = US (ft, mi, mph; F, inHg, in for the weather)
 *   "legalrule"  0 = 30 minutes either side of sunrise/sunset (the default, Washington's
 *                big-game rule), 1 = civil twilight (sun centre 6 deg below the horizon)
 *   "almdst"     1 = US daylight saving applies here (the default): the Almanac's DATE reminds
 *                before the US clocks change, and a day stepped across a change is shown in the
 *                offset that day will have. 0 = no (Hawaii, Arizona: the clocks never change).
 *
 * Loaded once at boot (WiPhone.ino, beside the "gpsen" read); a setter writes NVS and the global
 * together, so a screen that changes one is seen by every other screen at its next redraw.
 * ⚠ The globals are read from the loop task only (draw paths and the console); nothing here is
 * for a background task.
 */
#ifndef PREFS_ALMANAC_H
#define PREFS_ALMANAC_H

#include <stdint.h>
#include "units.h"           // UNITS_METRIC / UNITS_US

#define LEGAL_RULE_30MIN   0
#define LEGAL_RULE_CIVIL   1

extern int gUnits;       // UNITS_METRIC or UNITS_US; anything else reads as metric (units.h)
extern int gLegalRule;   // LEGAL_RULE_30MIN or LEGAL_RULE_CIVIL
extern int gUsDst;       // 1 = US daylight saving applies (default), 0 = no (HI, AZ)

/* Read them all from NVS. A missing namespace or key (a new phone, the first boot of 0.9.80) is
 * the default; a stored value out of range is the default too. */
void almanacPrefsLoad();
/* Set the global and persist it. A value out of range is clamped to the default. */
void unitsSetPref(int units);
void legalRuleSetPref(int rule);
void usDstSetPref(int on);

#endif // PREFS_ALMANAC_H
