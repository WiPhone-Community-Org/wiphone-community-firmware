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

#ifndef __CONFIG_H
#define __CONFIG_H

#define WIPHONE_PRODUCTION          // configure code for production, not development and testing

//#define TEST_DB_PINS                // this requires USER_SERIAL to be not defined, or not used.
// later need to only configure the daughterboard UART when we are actually using it, and disable it when not using

#ifndef WIPHONE_PRODUCTION
//#define BATTERY_BLINKING_OFF        // turn off battery blinking while charging (useful in debugging)
#define DIAGNOSTICS_ONLY            // limit the UI to the Diagnostics screen
//#define STEAL_THE_USER_BUTTONS      // this will switch between 2 different sets of definitions for the user button functions
#endif

/* Diagnostic switch, normally OFF. Defining SCREEN_ALWAYS_ON_TEST forces doDimming() and
 * doSleeping() (GUI.h) to false, so the screen never dims or sleeps and the screen-on heap
 * behaviour can be soaked with nobody holding the phone.
 *
 * Used 2026-08-16 to establish that screen-on ALONE does not ratchet the heap: 8.6 minutes
 * awake with SIP registered drifted `largest` by only -1,792 bytes and never pa{user}ed. That
 * pointed away from the redraw loop and toward app-switching, and away from a blanket change
 * to TFT_eSprite's allocator that would have cost the emulator frame rate for nothing.
 *
 * Leave it OFF: it defeats the sleep timeout and pins the CPU high.
//#define SCREEN_ALWAYS_ON_TEST */

/* "-NH" marks {user} Howe's firmware apart from the stock WiPhone releases, on the splash, in
 * Settings, in `ver` and in the installer's manifest. A SUFFIX on purpose: the updater's
 * compVersions() (ota.cpp) reads "%u.%u.%u" and ignores what follows, so the check against the
 * server still compares numbers; a prefix would parse as 0.0.0 and make every server version
 * look newer. Never "rc" or "db" in the tag: compVersions() gives those letters meanings. */
#define FIRMWARE_VERSION "0.9.82-NH"

#define BUILD_GAMES

// Meshtastic integration. When MESHTASTIC_PHY is defined, the real SX1276
// Meshtastic radio driver (mesh_phy.*) owns the LoRa chip, and the legacy
// WiPhone RadioHead LoRa messaging is disabled (one radio can't run both).
// Comment this out to fall back to the stubbed Meshtastic service and keep
// the old WiPhone LoRa messaging behavior.
#define MESHTASTIC_PHY

#define IF_LOG(level)                       if (ARDUHAL_LOG_LEVEL >= ESP_LOG_ ## level || LOG_LOCAL_LEVEL >= ESP_LOG_ ## level)
#define LOG_MEM_STATUS printf("Free Memory: %d\r\n", ESP.getFreeHeap())

// hopefully you can find this instead of spending 45 minutes (for the second time) figuring out how to set the log level:
// In the Arduino IDE, choose from the dropdown located at: Tools>Core Debug Level
//
// some keywords: ESP_LOGE ESP_LOGW ESP_LOGI ESP_LOGD ESP_LOGV esp_log_level_set esp_log.h CONFIG_LOG_DEFAULT_LEVEL


/* ================== Timings ================== */

#define KEYPAD_IDLE_MS                2000u       // 2 s
#define KEYPAD_LEDS_ON_MS             5000u       // 5 s
#define HANGUP_TIMEOUT_MS             5000u       // 5 s
#define BATTERY_CHECK_PERIOD_MS       15000u      // 15 s
#define USB_CHECK_PERIOD_MS           750u        // 0.75 s - this is also the the period of blinking for charging icon
#define WIFI_CHECK_PERIOD_MS          2000u       // 2 s
#define TIME_UPDATE_DELAY_MS          600000u     // 10 minutes: after what time to connect to NTP server to update the clock?
#define TIME_UPDATE_RETRY_DELAY_MS    500u        // 0.5 s: after what time to check back for a reply from NTP server?
#define TIME_UPDATE_MINUTE_MS         60000u      // 1 minute: how often to update system clock and generate TIME_UPDATE_EVENT (must be 1 minute, unless you know what you are doing)
#define WIFI_RETRY_PERIOD_MS          20000u      // 20 s
/* ── LONG DRY SPELL: off WiFi for this long and both cadences ease hard ──────────────────
 * 5 min is {user}'s number (he proposed 10, then argued down to 5 himself, and he is right:
 * the screen-wake escape hatch forces an immediate scan AND retry, so a long back-off costs
 * far less responsiveness than it looks like it should). Asked for after a 50-minute drive
 * home measured ~106 mV/h against
 * ~60 mV/h associated. See Networks::inLongDrySpell() for why the JOIN RETRY, not the scan,
 * is what these numbers are really cutting. */
#define WIFI_DRY_SPELL_LONG_MS        300000u     // 5 min with no association
/* 🔑 SCAN OFTEN, JOIN RARELY — and this is the right way round, which is NOT how 0.9.48/49
 * had it. A scan is ~350 ms and it is the SENSOR; a failed join is up to 30 s of radio and it
 * is the COST. Stretching the scan alongside the retry (15 min, as first shipped) throttled
 * the cheap thing and pushed worst-case rejoin latency out to 15 minutes. Five minutes of
 * scanning is 12/hour = ~4 s of radio an hour, and with worthAttemptingJoin() gating the join
 * on what those scans saw, the expensive half costs almost nothing. Out-of-range duty works
 * out at ~1.4 % against 16.8 % before, with BETTER return latency than either. */
#define WIFI_DRY_SCAN_PERIOD_MS       300000u     // keep scanning every 5 min: it is the sensor
#define WIFI_DRY_RETRY_PERIOD_MS      600000u     // ...and try to join every 10 min, not 3
/* Scan evidence older than this is treated as no evidence at all, so worthAttemptingJoin()
 * fails open. Must comfortably exceed the dry-spell scan cadence (15 min) or the gate would
 * expire between scans and never fire. */
#define WIFI_SCAN_EVIDENCE_MS         1200000u    // 20 min
/* One blind attempt every N skips: the insurance for a hidden SSID or a lying scan. At the
 * 10-minute dry retry that is roughly one blind try every 40 minutes. */
#define WIFI_JOIN_BLIND_EVERY         4u


/* ================== Keyboard constants ================== */

#define WIPHONE_SHIFT_KEY     '#'
#define WIPHONE_SYMBOLS_KEY   '*'
#define WIPHONE_UNLOCK_KEY2   '*'

// Key codes
#define WIPHONE_KEY_BACK    8       // top right button
#define WIPHONE_KEY_OK      12      // button in the middle
#define WIPHONE_KEY_UP      17
#define WIPHONE_KEY_LEFT    18
#define WIPHONE_KEY_RIGHT   19
#define WIPHONE_KEY_DOWN    20
#define WIPHONE_KEY_SELECT  21      // top left button
#define WIPHONE_KEY_CALL    22      // below SELECT (on the left side)
#define WIPHONE_KEY_END     23      // below BACK (on the right side)
#define WIPHONE_KEY_F1      24
#define WIPHONE_KEY_F2      25
#define WIPHONE_KEY_F3      26
#define WIPHONE_KEY_F4      27

/* ================== Timezone ================== */

//#define TIME_OFFSET_UTC             0           // Coordinated Universal Time
//#define TIME_OFFSET_UTC_PLUS_1      3600        // Europe/Berlin, EU time
//#define TIME_OFFSET_UTC_PLUS_8      28800       // Asia/Shanghai, China time
//#define TIME_OFFSET_UTC_MINUS_5     18000       // America/New_York, winter time
//#define TIME_OFFSET_UTC_MINUS_6     21600       // America/Chicago, winter time
//#define TIME_OFFSET_UTC_MINUS_8     -28800      // America/Los_Angeles, West Coast winter time

#define ONE_HOUR_IN_SECONDS         3600
#define DEFAULT_TIME_OFFSET         (0 * ONE_HOUR_IN_SECONDS)    // UTC+0

//#define UDP_SIP

#endif // __CONFIG_H
