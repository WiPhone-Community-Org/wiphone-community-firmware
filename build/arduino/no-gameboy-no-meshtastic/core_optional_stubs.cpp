#include "generated_extension_registry.h"

#ifndef WIPHONE_APP_ALMANAC
#include "app_almanac.h"
int gUnits = 0;
int gLegalRule = 0;
int gUsDst = 1;
void almanacPrefsLoad() {}
void unitsSetPref(int) {}
void legalRuleSetPref(int) {}
void usDstSetPref(int) {}
void wxLoopTick() {}
int almanacLegalToday(double, double, AlmanacLegal* out) {
  if (out) out->first = out->last = 0;
  return -1;
}
void almFmtClock(long long, int, int, char* text, unsigned int cap) {
  if (text && cap) text[0] = '\0';
}
void almanacConsole(const char*, void (*)(const char*)) {}
void almanacOpenAtWeather(bool) {}
void almanacWeatherConsole(const char*, void (*)(const char*)) {}
#endif

#ifndef WIPHONE_APP_AI
void aiLoopTick() {}
void aiKeyReload() {}
void aiChatClear() {}
void aiChatNewTopic() {}
bool aiAsk(const char*, char*, unsigned int) { return false; }
bool aiRequestActive() { return false; }
void aiReport(void (*)(const char*)) {}
void aiReportLast(void (*)(const char*)) {}
void aiRetryTimeouts() {}
void aiSetRetryTimeouts(bool) {}
struct GeminiDechunk;
void geminiDechunkInit(GeminiDechunk*) {}
bool geminiDechunkFeed(GeminiDechunk*, const unsigned char*, unsigned int, char*, unsigned int, unsigned int*) { return false; }
#endif
 
#ifndef WIPHONE_APP_ALMANAC
bool wxRequestActive() { return false; }
#endif

#ifndef WIPHONE_APP_BOOKS
#include "app_books.h"
#include "kosync.h"
#include "kosync_sync.h"
void kosyncLoop(bool, bool) {}
bool kosyncClientBusy() { return false; }
void kosyncWindowClose(const char*) {}
bool kosyncWindowActive() { return false; }
bool kosyncIsPath(const char*) { return false; }
const char* kosyncWindowServe(const char*, const char*, const char*, const char*,
                              unsigned int, int* code) {
  if (code) *code = 404;
  return "{\"error\":\"Books app unavailable\"}";
}
bool kosyncReloadConfig(bool) { return false; }
bool kosyncBookRenamed(const char*, const char*) { return false; }
void kosyncDumpStatus(void (*)(const char*)) {}
bool bookSyncIsSyncText(const char*) { return false; }
bool bookSyncInboxPush(const char*, unsigned int, unsigned int) { return false; }
void booksDebugDumpPage() {}
bool booksKosyncBench(const char*, unsigned int, char*, unsigned int) { return false; }
bool booksSaveOpenPosition() { return true; }
#endif

#ifndef WIPHONE_APP_FILES
bool filesJobActive() { return false; }
#endif

#ifndef WIPHONE_APP_PHOTOS
#include "app_photos.h"
bool photosSetWallpaper(const char*, char* why, unsigned int whyLen) {
  if (why && whyLen) why[0] = '\0';
  return false;
}
#endif

#ifndef WIPHONE_APP_GAME_BOY
#include "app_gbc.h"
struct XferConfig;
bool gbcSaveForPowerOff() { return false; }
void gbcStatus(char* out, unsigned int n) {
  if (out && n) out[0] = '\0';
}
#endif

#ifndef WIPHONE_APP_MAPS
#include "app_maps.h"
#include "tile_fetch.h"
volatile bool gMapsActive = false;
bool mapsSaveOpenView() { return true; }
bool mapsConsoleStatus(char* out, unsigned int cap) {
  if (out && cap) out[0] = '\0';
  return false;
}
bool mapsConsoleGoto(double, double, int, const char*, char* why, unsigned int whyCap) {
  if (why && whyCap) why[0] = '\0';
  return false;
}
bool mapsConsoleElev(char* out, unsigned int cap) {
  if (out && cap) out[0] = '\0';
  return false;
}
void tileFetchPowerOff() {}
bool tileFetchActive() { return false; }
void tileFetchPause(bool, bool, bool) {}
void tileFetchTick(const TileFetchWorld*) {}
bool tileFetchCallRecent() { return false; }
void tileFetchStop() {}
bool tileFetchStart(const TileJobSpec*, char*, unsigned int) { return false; }
void tileFetchReport(void (*)(const char*)) {}
int tileFetchEstimate(const TileJobSpec*, unsigned long long*, unsigned long long*, int*) { return 0; }
void tileFetchBenchReport(void (*)(const char*)) {}
bool tileFetchBenchStart(const char*, int) { return false; }
const TileSource* tileSource(int) { return NULL; }
bool tileSetCustomUrl(const char*, const char*, char*, unsigned int) { return false; }
int tilePlanParseElevArg(const char*) { return -1; }
int tilePlanParseDlurl(const char*, char*, unsigned int, char*, unsigned int) { return -1; }
void tlsInstallPsramHook() {}
bool elevCardPresent() { return false; }
int elevSampleCard(double, double, double*, int*, bool*) { return -1; }
int elevSampleText(char*, unsigned int, int, double, int) { return 0; }
#endif
