/* prefs_almanac.cpp - see prefs_almanac.h. */

#include "prefs_almanac.h"
#include <Preferences.h>

static const char PREFS_NS[] = "wpmesh";

int gUnits = UNITS_METRIC;
int gLegalRule = LEGAL_RULE_30MIN;
int gUsDst = 1;

void almanacPrefsLoad() {
  Preferences p;
  /* Read-only begin() FAILS when the namespace has never been written (a new phone): the
   * defaults above stand, which is exactly "today's behaviour". */
  if (p.begin(PREFS_NS, true)) {
    gUnits = p.getInt("units", UNITS_METRIC);
    gLegalRule = p.getInt("legalrule", LEGAL_RULE_30MIN);
    gUsDst = p.getInt("almdst", 1);
    p.end();
  }
  if (gUnits != UNITS_METRIC && gUnits != UNITS_US) {
    gUnits = UNITS_METRIC;
  }
  if (gLegalRule != LEGAL_RULE_30MIN && gLegalRule != LEGAL_RULE_CIVIL) {
    gLegalRule = LEGAL_RULE_30MIN;
  }
  if (gUsDst != 0 && gUsDst != 1) {
    gUsDst = 1;
  }
}

static void putPref(const char* key, int v) {
  Preferences p;
  if (p.begin(PREFS_NS, false)) {
    p.putInt(key, v);
    p.end();
  } else {
    log_e("PREFS: could not open NVS '%s' to save %s=%d (kept for this boot only)", PREFS_NS, key, v);
  }
}

void unitsSetPref(int units) {
  gUnits = (units == UNITS_US) ? UNITS_US : UNITS_METRIC;
  putPref("units", gUnits);
}

void legalRuleSetPref(int rule) {
  gLegalRule = (rule == LEGAL_RULE_CIVIL) ? LEGAL_RULE_CIVIL : LEGAL_RULE_30MIN;
  putPref("legalrule", gLegalRule);
}

void usDstSetPref(int on) {
  gUsDst = on ? 1 : 0;
  putPref("almdst", gUsDst);
}
