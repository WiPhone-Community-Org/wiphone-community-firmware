#include "ExtensionRegistry.h"
#include "GUI.h"
#include "Storage.h"

void NotificationPatternsLoad(void* context) {
  wiphone::SettingContext* setting = static_cast<wiphone::SettingContext*>(context);
  if (!setting || !setting->value) return;
  ControlState* state = static_cast<ControlState*>(setting->value);
  CriticalFile* cfg = static_cast<CriticalFile*>(setting->storage);
  if (!cfg) return;
  if ((cfg->load() || cfg->restore()) && !cfg->isEmpty() && cfg->hasSection("audio")) {
    state->ringerMode = (uint8_t) (*cfg)["audio"].getIntValueSafe("ringer_mode", state->ringerMode);
    state->notifySipMode = (uint8_t) (*cfg)["audio"].getIntValueSafe("notify_sip_mode", state->notifySipMode);
    state->notifyMeshMode = (uint8_t) (*cfg)["audio"].getIntValueSafe("notify_mesh_mode", state->notifyMeshMode);
    state->notifyVolume = (int8_t) (*cfg)["audio"].getIntValueSafe("notify_vol", state->notifyVolume);
  }
}

void NotificationPatternsSave(void* context) {
  wiphone::SettingContext* setting = static_cast<wiphone::SettingContext*>(context);
  if (!setting || !setting->value) return;
  ControlState* state = static_cast<ControlState*>(setting->value);
  CriticalFile* cfg = static_cast<CriticalFile*>(setting->storage);
  if (!cfg) return;
  if (!cfg->hasSection("audio")) cfg->addSection("audio");
  (*cfg)["audio"]["ringer_mode"] = (int) state->ringerMode;
  (*cfg)["audio"]["notify_sip_mode"] = (int) state->notifySipMode;
  (*cfg)["audio"]["notify_mesh_mode"] = (int) state->notifyMeshMode;
  (*cfg)["audio"]["notify_vol"] = (int) state->notifyVolume;
  cfg->store();
}
