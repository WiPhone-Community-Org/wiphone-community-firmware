#include "ExtensionRegistry.h"
#include "GUI.h"
#include "Storage.h"

void VibrationPatternsLoad(void* context) {
  wiphone::SettingContext* setting = static_cast<wiphone::SettingContext*>(context);
  if (!setting || !setting->value) return;
  ControlState* state = static_cast<ControlState*>(setting->value);
  CriticalFile* cfg = static_cast<CriticalFile*>(setting->storage);
  if (!cfg) return;
  if ((cfg->load() || cfg->restore()) && !cfg->isEmpty() && cfg->hasSection("audio")) {
    state->notifyVibroMs = (uint16_t) constrain(
        (*cfg)["audio"].getIntValueSafe("notify_vibro_ms", state->notifyVibroMs), 50, 650);
  }
}

void VibrationPatternsSave(void* context) {
  wiphone::SettingContext* setting = static_cast<wiphone::SettingContext*>(context);
  if (!setting || !setting->value) return;
  ControlState* state = static_cast<ControlState*>(setting->value);
  CriticalFile* cfg = static_cast<CriticalFile*>(setting->storage);
  if (!cfg) return;
  if (!cfg->hasSection("audio")) cfg->addSection("audio");
  (*cfg)["audio"]["notify_vibro_ms"] = (int) constrain(state->notifyVibroMs, 50, 650);
  cfg->store();
}
