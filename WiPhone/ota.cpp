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

#include "ota.h"
#include <WiFi.h>
#include "Networks.h"
#include "esp_log.h"
#include <Update.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include "esp_ota_ops.h"
#include "isrg_roots.h"      // ISRG Root X1, shared with the Almanac's weather

#define CA_CERT_MAX_SZ 10*1024

/* ISRG Root X1 — the Let's Encrypt root, valid to June 2035.
 *
 * ⚠ This replaces a pinned LEAF certificate for wiphone.io that EXPIRED in April 2021.
 * That is why the update page stopped working: not a dead link, a dead certificate. A
 * leaf is the wrong thing to pin anyway — it is reissued every 90 days, so pinning one
 * guarantees the feature breaks on a timer.
 *
 * A ROOT is the right level: it signs the whole chain, changes about once a decade, and
 * this one covers raw.githubusercontent.com, which is where updates now come from. */
static const char *defaultCaCert = ISRG_ROOT_X1_PEM;   // isrg_roots.h (moved there in 0.9.81, the same bytes)

static int compVersions ( const char * version1, const char * version2 ) {
  uint32_t major1 = 0, minor1 = 0, bugfix1 = 0, rc1 = 0;
  uint32_t major2 = 0, minor2 = 0, bugfix2 = 0, rc2 = 0;

  if (strstr(version1, "db") != NULL || strstr(version2, "db") != NULL) {
    if (strcmp(version1, version2) == 0 ) {
      return 0;
    }

    return -1;
  }

  if (strstr(version1, "rc") != NULL) {
    sscanf(version1, "%u.%u.%urc%u", &major1, &minor1, &bugfix1, &rc1);
  } else {
    sscanf(version1, "%u.%u.%u", &major1, &minor1, &bugfix1);
  }

  if (strstr(version2, "rc") != NULL) {
    sscanf(version2, "%u.%u.%urc%u", &major2, &minor2, &bugfix2, &rc2);
  } else {
    sscanf(version2, "%u.%u.%u", &major2, &minor2, &bugfix2);
  }

  log_d("v: %u %u %u %u -> %u %u %u %u", major1, minor1, bugfix1, rc1, major2, minor2, bugfix2, rc2);

  if (major1 < major2) {
    return -1;
  }
  if (major1 > major2) {
    return 1;
  }
  if (minor1 < minor2) {
    return -1;
  }
  if (minor1 > minor2) {
    return 1;
  }
  if (bugfix1 < bugfix2) {
    return -1;
  }
  if (bugfix1 > bugfix2) {
    return 1;
  }
  if (rc1 < rc2) {
    return -1;
  }
  if (rc1 > rc2) {
    return 1;
  }

  return 0;
}


/* Is this a saved URL pointing at the old, dead update server?
 *
 * ⚠ Needed because a stored serverIni BEATS the compiled-in default, by design — that is
 * how a user override is meant to work. But the phone has been carrying wiphone.io in
 * /user_ota.ini since before that host stopped serving manifests, so without this the new
 * default would never be used and the URL box would keep showing the dead link no matter
 * what the firmware says. Treated as "no preference" rather than as a choice. */
static bool isDeadUpdateUrl(const char* url) {
  return url && strstr(url, "wiphone.io") != NULL;
}

static bool loadRootCACert(char *certBuf, size_t certBufSz) {
  char fname[500] = {0};

  /* ⚠ /wiphone.pem is deliberately NOT consulted any more. It ships in the factory
   * SPIFFS, so it always won this test — and it is an expired leaf for a host that no
   * longer serves updates, which meant every TLS handshake failed no matter what the
   * URL pointed at. Only an explicit /user.pem overrides the built-in root now. */
  if (SPIFFS.exists("/user.pem")) {
    strlcpy(fname, "/user.pem", sizeof(fname));
  } else {
    certBuf[0] = '\0';
    strlcpy(certBuf, defaultCaCert, certBufSz);
    log_i("CA: built-in ISRG Root X1");
    return true;
  }

  if (!SPIFFS.exists(fname)) {
    // Default to hard coded pem file
    strlcpy(certBuf, defaultCaCert, certBufSz);
  } else {
    File cert = SPIFFS.open(fname);

    if (!cert) {
      log_d("Unable to load pem file: %s", fname);
      return false;
    }

    int i = 0;
    memset((void*)certBuf, 0x00, certBufSz);
    while (cert.available() && i < certBufSz) {
      certBuf[i++] = cert.read();
    }
  }

  log_i("CA cert is: [%s]", certBuf);

  return true;
}

static char *getOtaIniFileName() {
  static char fileName[1024] = {0};

  if (SPIFFS.exists("/user_ota.ini")) {
    strlcpy(fileName, "/user_ota.ini", sizeof(fileName));
  } else {
    strlcpy(fileName, "/ota.ini", sizeof(fileName));
  }

  log_d("Config file: %s", fileName);

  return fileName;
}

Ota::Ota(std::string inifile) : inifileLocation_(inifile), fwVersion_(""), fwUrl_(""), lastLoad_(0), iniLocation_(std::string("https://") + std::string(DEFAULT_INI_HOST) + std::string(DEFAULT_INI_LOC)) {
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  const char* l = otaIni[0].getValueSafe("serverIni", "0");

  if (strlen(l) > 3 && !isDeadUpdateUrl(l)) {
    iniLocation_ = std::string(l);
  }
}

void Ota::resetIni() {
  if (SPIFFS.exists("/user_ota.ini")) {
    SPIFFS.remove("/user_ota.ini");
  }
  reset();

  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  iniLocation_ = std::string("https://") + std::string(DEFAULT_INI_HOST) + std::string(DEFAULT_INI_LOC);
  log_d("Removed user ini file");
}

void Ota::ensureUserVersion() {
  log_d("Ota::ensureUserVersion: %d", SPIFFS.exists("/user_ota.ini"));
  // Ensure we are using the user version of the config file as the user is about to edit something
  if (strcmp(getOtaIniFileName(), "/ota.ini") == 0) {
    CriticalFile userIni("/user_ota.ini");
    CriticalFile wiphoneIni("/ota.ini");

    userIni.load();
    wiphoneIni.load();

    userIni[0]["autoUpdate"] = wiphoneIni[0].getValueSafe("autoUpdate", "yes");
    userIni[0]["serverIni"] = wiphoneIni[0].getValueSafe("serverIni", "");
    userIni[0]["errorCode"] = "";
    userIni[0]["errorString"] = "";
    userIni[0]["serverVersion"] = "";
    userIni[0]["latestServerVersion"] = "";
    userIni[0]["newVersion"] = "";

    userIni.store();
    reset();
    log_d("Ota::ensureUserVersion: %d", SPIFFS.exists("/user_ota.ini"));
  }
}

const char* Ota::getIniUrl() {
  return iniLocation_.c_str();
}

const char* Ota::getIniHost() {
  log_d("ini url: [%s] [%d]", iniLocation_.c_str());
  if (iniLocation_.rfind("https://", 0) != std::string::npos) {
    std::size_t epos = iniLocation_.find('/', 8);

    if (epos != std::string::npos) {
      size_t len = iniLocation_.length();
      char *host = (char*)extMalloc(len);
      memset(host, 0x00, len);
      strlcpy(host, iniLocation_.substr(8, epos - 8).c_str(), len);
      return host;
    }
  }

  size_t len = strlen(DEFAULT_INI_HOST) + 1;
  char *host = (char*)extMalloc(len);
  strlcpy(host, DEFAULT_INI_HOST, len);
  return host;
}

const char* Ota::getIniPath() {
  if (iniLocation_.rfind("https://", 0) != std::string::npos) {
    std::size_t spos = iniLocation_.find('/', 8);

    if (spos != std::string::npos) {
      size_t len = iniLocation_.length();
      char *path = (char*)extMalloc(len);
      memset(path, 0x00, len);
      strlcpy(path, iniLocation_.substr(spos).c_str(), len);
      return path;
    }
  }

  size_t len = strlen(DEFAULT_INI_LOC) + 1;
  char *path = (char*)extMalloc(len);
  strlcpy(path, DEFAULT_INI_LOC, len);
  return path;
}

const char* Ota::getServerVersion() {
  static char s[300] = {0};
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  strlcpy(s, otaIni[0].getValueSafe("latestServerVersion", "0"), sizeof(s));

  return s;
}

const char* Ota::getLastErrorCode() {
  static char s[300] = {0};
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  strlcpy(s, otaIni[0].getValueSafe("errorCode", "0"), sizeof(s));

  return s;
}

const char* Ota::getLastErrorString() {
  static char s[300] = {0};
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  strlcpy(s, otaIni[0].getValueSafe("errorString", "0"), sizeof(s));

  return s;
}

void Ota::saveAutoUpdate(bool autoUpdate) {
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  otaIni[0]["autoUpdate"] = autoUpdate ? "yes" : "no";
  otaIni.store();
}

bool Ota::autoUpdateEnabled() {
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  log_d("autoUpdateEnabled: %s", otaIni[0].getValueSafe("autoUpdate", "0"));

  if (strcmp(otaIni[0].getValueSafe("autoUpdate", "0"), "no") == 0) {
    return false;
  }

  return true;
}

bool Ota::userRequestedUpdate() {
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  if (strcmp(otaIni[0].getValueSafe("userRequested", "0"), "yes") == 0) {
    return true;
  }

  return false;
}

void Ota::setUserRequestedUpdate(bool userUpdate) {
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  otaIni[0]["userRequested"] = userUpdate ? "yes" : "no";

  if (userUpdate) {
    otaIni[0]["serverVersion"] = "";
    otaIni[0]["newVersion"] = "";
  }

  otaIni.store();
}

bool Ota::updateExists(bool loadIni) {
  log_i("#### Ota::updateExists: %d %d", loadIni, lastLoad_);
  if (lastLoad_ == 0) {
    if (loadIni && !loadIniFile()) {
      log_i("# Returning false");
      return false;
    }
  }

  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  fwVersion_ = std::string(otaIni[0].getValueSafe("latestServerVersion", "0"));

  if (fwVersion_ == "0") {
    log_i("#### Returning false: %s", fwVersion_.c_str());
    return false;
  }

  log_d("### Ota version in ini file: [%s] [%s] [%s]", otaIni[0].getValueSafe("latestServerVersion", "0"),
        otaIni[0].getValueSafe("oldVersion", "0"), fwVersion_.c_str());

  if (fwVersion_ != "" && fwVersion_ == std::string(otaIni[0].getValueSafe("newVersion", "0"))) {
    otaIni[0]["errorString"] =  "Install prev failed";
    otaIni[0]["errorCode"] =  "-900";
    otaIni.store();
    log_d("#### Ignoring fw update as it failed last time");
    return false;
  }

  const char *svs = fwVersion_.c_str();
  char *lvs = FIRMWARE_VERSION;

  log_i("Current version: %s server version: %s", lvs, svs);

  int diff = compVersions(lvs, svs);

  if (diff < 0) {
    return true;
  }

  return false;
}

bool Ota::doUpdate() {
  if (lastLoad_ == 0 || (millis() - lastLoad_) > OTA_UPDATE_CHECK_INTERVAL) {
    if (!loadIniFile()) {
      return false;
    }
  }

  if (fwUrl_ == "") {
    return false;
  }

  if (wifiState.isConnected()) {
    log_d("##### Doing OTA");

    const char* url = fwUrl_.c_str();

    if (url == NULL) {
      return false;
    }

    WiFiClientSecure client;
    char *rootCACertificate = (char*)extMalloc(CA_CERT_MAX_SZ);
    if (!loadRootCACert(rootCACertificate, CA_CERT_MAX_SZ)) {
      log_e("Unable to load cert file");
      return false;
    }

    client.setCACert(rootCACertificate);

    log_i("Doing firmware update: [%s]", url);

    CriticalFile otaIni(getOtaIniFileName());
    otaIni.load();
    otaIni[0]["newVersion"] = fwVersion_.c_str();
    otaIni[0]["oldVersion"] = FIRMWARE_VERSION;
    otaIni[0]["hadJustUpdated"] = "yes";
    otaIni.store();

    t_httpUpdate_return ret = httpUpdate.update(client, url);

    char errorCode[30] = {0};

    switch (ret) {
    case HTTP_UPDATE_FAILED:
      snprintf(errorCode, sizeof(errorCode), "%d", httpUpdate.getLastError());
      log_i("HTTP_UPDATE_FAILED Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      otaIni[0]["newVersion"] = "";
      otaIni[0]["oldVersion"] = "";
      otaIni[0]["errorString"] =  httpUpdate.getLastErrorString().c_str();
      otaIni[0]["errorCode"] =  errorCode;
      otaIni[0]["userRequested"] = "";
      otaIni.store();
      break;

    case HTTP_UPDATE_NO_UPDATES:
      log_i("HTTP_UPDATE_NO_UPDATES");
      break;

    case HTTP_UPDATE_OK:
      log_i("HTTP_UPDATE_OK");
      otaIni[0]["errorString"] =  "";
      otaIni[0]["errorCode"] = "";
      break;
    }

    if (rootCACertificate != NULL) {
      free(rootCACertificate);
    }
  }

  return false;
}

bool Ota::hasJustUpdated() {
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  if (strcmp(otaIni[0].getValueSafe("hadJustUpdated", "0"), "yes") == 0) {
    log_d("We've just updated: [%s]", otaIni[0].getValueSafe("newVersion", "0"));

    if (strcmp(otaIni[0].getValueSafe("newVersion", "0"), otaIni[0].getValueSafe("oldVersion", "0")) == 0) {
      log_e("Failed an update: %s %s", otaIni[0].getValueSafe("newVersion", "0"), otaIni[0].getValueSafe("oldVersion", "0"));
      otaIni[0]["errorString"] =  "Rollback";
      otaIni[0]["errorCode"] = "-900";
      otaIni.store();
    }
    return true;
  }

  log_d("Not booted after update: [%s]", otaIni[0].getValueSafe("newVersion", "0"));
  return false;
}

bool Ota::commitUpdate() {
  log_d("Ota::commitUpdate");
  esp_ota_mark_app_valid_cancel_rollback();
  IniFile otaIni(getOtaIniFileName());
  otaIni.load();
  otaIni[0]["newVersion"] = "";
  otaIni[0]["oldVersion"] = "";
  otaIni[0]["errorString"] =  "";
  otaIni[0]["errorCode"] = "";
  otaIni[0]["hadJustUpdated"] = "";
  otaIni.store();
  return true;
}

void Ota::setIniUrl(const char* url) {
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();
  otaIni[0]["serverIni"] = url;
  otaIni.store();
}

void Ota::backgroundUpdateCheck() {

}

void Ota::reset() {
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();
  otaIni[0]["errorCode"] = "";
  otaIni[0]["errorString"] = "";
  otaIni[0]["serverVersion"] = "";
  otaIni[0]["latestServerVersion"] = "";
  otaIni[0]["newVersion"] = "";
  otaIni[0]["hadJustUpdated"] = "";

  otaIni.store();
}

bool Ota::loadIniFile() {
  lastLoad_ = millis();
  CriticalFile otaIni(getOtaIniFileName());
  otaIni.load();

  const char* l = otaIni[0].getValueSafe("serverIni", "0");

  log_d("Ini location read from file: [%s]", l);

  if (strlen(l) > 4 && !isDeadUpdateUrl(l)) {
    log_d("Setting initfileLocation");
    inifileLocation_ = std::string(l);
    iniLocation_ = inifileLocation_;
  } else if (isDeadUpdateUrl(l)) {
    log_i("Ignoring stored wiphone.io update URL; using built-in default");
  }


  const char *hostname = getIniHost();
  std::string hname = std::string(hostname);


  const char *inipath = getIniPath();
  std::string path = std::string(inipath);

  char *request = (char*)extMalloc(8192);
  snprintf(request, 8192,
           "GET %s HTTP/1.1\r\n"
           "Host: %s\r\n"
           "Connection: close\r\n\r\n",
           inipath, hostname);

  free((void*)inipath);
  free((void*)hostname);

  log_d("Loading OTA ini file: [%s] [%s] [%s]", inifileLocation_.c_str(), hname.c_str(), path.c_str());
  log_d("Request is: [%s]", request);

  //WiFiClient client;
  WiFiClientSecure *client = new WiFiClientSecure;

  if (client == NULL) {
    log_e("Unable to allocate client");
    otaIni[0]["errorCode"] = "-300";
    otaIni[0]["errorString"] = "Can't alloc client";
    otaIni.store();
    return false;
  }

  char *rootCACertificate = (char*)extMalloc(CA_CERT_MAX_SZ);
  if (!loadRootCACert(rootCACertificate, CA_CERT_MAX_SZ)) {
    log_e("Unable to load cert file");
    return false;
  }

  client->setCACert(rootCACertificate);
  if (!client->connect(hname.c_str(), 443)) {
    log_e("Unable to connect to server");
    delete client;
    otaIni[0]["errorCode"] = "-301";
    otaIni[0]["errorString"] = "Can't connect to server";
    otaIni.store();
    return false;
  }

  log_d("After connected to server");

  client->print(request);
  unsigned long timeout = millis();
  while (client->available() == 0) {
    if (millis() - timeout > 15000) {
      log_e("Client Timeout !");
      client->stop();
      delete client;
      otaIni[0]["errorCode"] = "-302";
      otaIni[0]["errorString"] = "Timeout";
      otaIni.store();
      return false;
    }
  }

  free(request);

  log_d("Data available");

  bool content = false;
  std::string iniData = "";

  while (client->available()) {
    String l = client->readStringUntil('\r');
    log_d("Read: %s %d", l.c_str(), content);

    if (l.indexOf("404 Not Found") > 0) {
      otaIni[0]["errorCode"] = "-404";
      otaIni[0]["errorString"] = "Not found";
      otaIni.store();
      return false;
    }

    if (!content && l.length() < 2) {
      content = true;
    } else if (content) {
      iniData += std::string(l.c_str());
    }
  }

  log_d("Done reading");

  log_d("Ini file: %s", iniData.c_str());

  NanoIni::Config fwVersion(iniData.c_str());

  const char *svs = fwVersion[0].getValueSafe("version", "0");
  char *lvs = FIRMWARE_VERSION;

  int diff = compVersions(lvs, svs);
  const char* url = fwVersion[0].getValueSafe("url", "0");

  log_d("Diff is: %d, url: %s", diff, url);

  lastLoad_ = millis();
  {
    CriticalFile otaIni(getOtaIniFileName());
    otaIni.load();
    otaIni[0]["latestServerVersion"] = svs;
    otaIni[0]["errorCode"] = "";
    otaIni[0]["errorString"] = "";
    otaIni.store();
  }

  delete client;

  if (rootCACertificate != NULL) {
    free(rootCACertificate);
  }

  if (diff < 0) {
    log_i("Found a fimware update: %s to %s", lvs, svs);

    fwUrl_ = std::string(url);
    fwVersion_ = std::string(svs);

    CriticalFile otaIni = CriticalFile(getOtaIniFileName());
    otaIni.load();
    otaIni[0]["serverVersion"] = svs;
    otaIni[0]["latestServerVersion"] = svs;
    otaIni.store();

    return true;
  }

  return false;
}
