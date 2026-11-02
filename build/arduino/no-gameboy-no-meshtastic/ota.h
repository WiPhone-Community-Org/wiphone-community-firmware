/*
Copyright © 2019, 2020, 2021, 2022 HackEDA, Inc.
Licensed under the WiPhone Public License v.1.0 (the "License"); you
may not use this file except in compliance with the License. You may
obtain a copy of the License at
https://wiphone.io/WiPhone_Public_License_v1.0.txt.

Unless required by applicable law or agreed to in writing, software,
hardware or documentation distributed under the License is distributed
on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/

#ifndef OTA_H
#define OTA_H

#include <WiFi.h>
#include "Networks.h"
#include "esp_log.h"
#include <Update.h>
#include <string>

/* 🛑 OTA IS DISABLED AT SOURCE, AND THIS IS THE SWITCH.
 *
 * The transport cannot work on this build. The TLS handshake needs ~33 KB of internal heap
 * (mbedTLS allocates a 16 KB IN buffer AND a 16 KB OUT buffer per CONFIG_MBEDTLS_SSL_MAX_
 * CONTENT_LEN, and CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC is NOT set, so none of it may come from
 * PSRAM) while this phone has ~19 KB of internal heap in TOTAL and ~14.9 KB contiguous on a
 * fresh boot. It fails inside client->connect() before one byte of HTTP — the `-301` error
 * and the `start_ssl_client: -1` line that has been in every boot log all along.
 *
 * ⚠ Why a hard gate and not just "turn auto-update off": autoUpdateEnabled() DEFAULTS TO TRUE
 * (it returns false only if the ini explicitly says "no"), and the screen that used to set it
 * is gone. Without this, every boot spends its time on a handshake that cannot succeed.
 *
 * Set to 1 only when the TRANSPORT actually changes — a plain-HTTP mirror, or a TLS stack that
 * can allocate from PSRAM. Turning it back on without that just restores the failure. */
#define OTA_TRANSPORT_AVAILABLE   0

#define OTA_UPDATE_CHECK_INTERVAL 60*1000*60
/* Updates come from {user}'s own repo now — wiphone.io's manifest is long gone.
 *
 * ⚠ raw.githubusercontent.com specifically, NOT a github.com release URL. The manifest
 * is fetched with a hand-rolled socket in loadIniFile() that reads one response and has
 * no redirect handling at all; release-asset URLs answer 302 and would silently look
 * like an empty file. raw serves 200 directly. The firmware BINARY goes through
 * httpUpdate, which does follow redirects, but keeping both on raw avoids the trap. */
#define DEFAULT_INI_HOST "raw.githubusercontent.com"
#define DEFAULT_INI_LOC "/Nikguy321/wiphone-meshtastic/main/ota/wiphone-ota.ini"

class Ota {
public:
  Ota(std::string inifile);

  bool updateExists(bool loadIni=true);
  bool doUpdate();
  bool hasJustUpdated();
  bool commitUpdate();
  void backgroundUpdateCheck();

  void saveAutoUpdate(bool autoUpdate);
  bool autoUpdateEnabled();
  bool userRequestedUpdate();
  void setUserRequestedUpdate(bool userUpdate);
  void setIniUrl(const char* url);
  void ensureUserVersion();
  void resetIni();

  const char* getIniUrl();
  const char* getIniHost();
  const char* getIniPath();
  const char* getServerVersion();
  const char* getLastErrorCode();
  const char* getLastErrorString();

  void reset();

private:
  std::string inifileLocation_;
  std::string fwUrl_;
  std::string fwVersion_;
  uint32_t lastLoad_;

  std::string iniLocation_;

  bool loadIniFile();
};


#endif // OTA_H
