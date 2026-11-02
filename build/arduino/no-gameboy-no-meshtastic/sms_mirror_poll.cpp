#include "sms_mirror_poll.h"
#include "sms_mirror.h"
#include "sms_mirror_rx.h"
#include "config.h"
#include "helpers.h"
#include "Networks.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <SD.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // strtoll, for the persisted since-id

// ---------------------------------------------------------------- tuning
static const uint32_t POLL_INTERVAL_MS   = 120000;   // 2 min: LoRa carries anything urgent
static const uint32_t RETRY_INTERVAL_MS  = 300000;   // 5 min after a failure, so a COVEY that
                                                     // is simply off does not cost a
                                                     // connect() attempt every two minutes
static const uint32_t CONNECT_TIMEOUT_MS = 600;      // the ONLY call here that can block
/* ⚠ A STALL timeout, not a whole-response budget — measured into that distinction on
 * 2026-08-18. The first-ever poll (since=0) pulls the entire mirrored history, and each
 * record's ingest runs a dedup scan that re-parses partition files; 61 records took
 * longer than 8 s of wall clock and a whole-response deadline declared a healthy,
 * progressing catch-up "timed out" — over and over, 5 minutes apart. The deadline now
 * re-arms whenever bytes arrive and ingest progresses; 8 s of genuine silence still
 * kills the connection, and TOTAL_BUDGET_MS below caps a server that will not shut up. */
static const uint32_t READ_TIMEOUT_MS    = 8000;     // max silence once connected
static const uint32_t TOTAL_BUDGET_MS    = 120000;   // absolute cap per poll, stall or not
static const uint32_t HOST_CACHE_MS      = 600000;   // 10 min; re-resolved sooner on failure
static const int      READ_BUDGET        = 512;      // bytes consumed per main-loop pass

/* Both, and `/roms` is the one that matters in practice: the WiFi uploader writes THERE, not
 * to the card's root (app_gbc_xfer.cpp), so `/roms/smsmirror.txt` is the only one of these
 * that can be put on the phone without pulling the SD card out. The root path is kept first
 * because it is where someone editing the card on a computer would naturally put it. */
static const char* CONFIG_FILES[] = { "/smsmirror.txt", "/roms/smsmirror.txt" };
static const int   CONFIG_FILE_N  = 2;

/* ⚠ THE HIGH-WATER MARK IS PERSISTED, and a decoded backtrace is why. `s_sinceId` was
 * RAM-only, so every boot polled `since=0` and COVEY served its ENTIRE store — and every
 * record ran the full ingest scan (paged partition loads, dedup against the newest 60) just
 * to conclude "duplicate". That storm lands in the first minutes after boot, exactly when
 * the heap is at its most fragile, and on 2026-08-18 it aborted the phone mid-ingest:
 * NanoIni::parse -> loadPartition -> preload -> ingestMirrored -> ingestLine, reset_reason=4.
 * One small SD file turns the boot poll back into an increment.
 * Delete the file to force a full resync — that is the documented recovery, not a bug. */
static const char* SINCE_FILE = "/roms/smsmirror.since";

// ---------------------------------------------------------------- state
enum PollState {
  PS_IDLE = 0,
  PS_CONNECT,
  PS_HEADERS,
  PS_BODY,
};

static bool        s_loaded    = false;      // config read and usable
static uint32_t    s_triedLoad = 0;          // millis() of the last attempt; 0 = never
static char        s_host[64]  = {0};
static char        s_token[96] = {0};
static uint16_t    s_port      = 8087;

static PollState   s_state     = PS_IDLE;
static WiFiClient  s_client;
static uint32_t    s_nextPollMs   = 0;
static uint32_t    s_deadlineMs   = 0;
static uint32_t    s_pollStartMs  = 0;
static uint32_t    s_hostResolvedAt = 0;
static IPAddress   s_hostIp((uint32_t)0);
static int64_t     s_sinceId   = 0;          // high-water VoIP.ms id
static char        s_line[SMS_MIRROR_LINE_MAX + 8];
static uint16_t    s_lineLen   = 0;
static bool        s_lineOverflow = false;
static const char* s_status    = "idle";
static bool        s_forceNow  = false;
/* Per-POLL totals, not per-call. One poll is spread over many main-loop passes, so the
 * summary at the end has to accumulate somewhere that survives between them. */
static int         s_lines     = 0;          // records seen in this poll
static int         s_added     = 0;          // ...of which were new

// ---------------------------------------------------------------- config
static void trimEol(char* s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
    s[--n] = '\0';
  }
}

static bool loadConfig() {
  if (s_loaded) {
    return true;
  }
  /* ⚠ RE-CHECKED PERIODICALLY, not once at boot, and that matters more than it looks.
   * The intended way to install this file is the WiFi uploader — i.e. WHILE THE PHONE IS
   * RUNNING. Giving up permanently on the first miss would mean uploading the config and
   * watching nothing happen, with no message anywhere, until someone thought to reboot.
   * That is the exact silent-failure shape this feature already has too much of.
   * Re-reading a small file every 30 s costs nothing next to that. */
  const uint32_t now = millis() | 1u;
  if (s_triedLoad && (uint32_t)(now - s_triedLoad) < 30000u) {
    return false;
  }
  const bool first = (s_triedLoad == 0);
  s_triedLoad = now;

  File f;
  for (int i = 0; i < CONFIG_FILE_N && !f; i++) {
    f = SD.open(CONFIG_FILES[i], FILE_READ);
  }
  if (!f) {
    s_status = "off (no smsmirror.txt)";
    if (first) {
      // Once, at boot. Says the feature exists and is waiting, rather than saying nothing.
      log_e("SMSMIRROR poll: no smsmirror.txt on SD (/ or /roms) - LAN sync off");
    }
    return false;
  }
  char buf[3][128] = {{0}, {0}, {0}};
  for (int i = 0; i < 3 && f.available(); i++) {
    size_t n = f.readBytesUntil('\n', buf[i], sizeof(buf[i]) - 1);
    buf[i][n] = '\0';
    trimEol(buf[i]);
  }
  f.close();

  snprintf(s_host, sizeof(s_host), "%s", buf[0]);
  snprintf(s_token, sizeof(s_token), "%s", buf[1]);
  if (buf[2][0]) {
    int p = atoi(buf[2]);
    if (p > 0 && p < 65536) {
      s_port = (uint16_t)p;
    }
  }
  if (!s_host[0] || !s_token[0]) {
    s_status = "off (host or token missing)";
    return false;
  }

  /* Restore the high-water id, once per (re)load. A failed parse or a missing file just
   * means since=0 — a full, deduped resync, which is slow but never wrong. */
  File sf = SD.open(SINCE_FILE, FILE_READ);
  if (sf) {
    char buf[24] = {0};
    size_t n = sf.readBytes(buf, sizeof(buf) - 1);
    buf[n] = '\0';
    sf.close();
    long long v = strtoll(buf, NULL, 10);
    if (v > 0 && v > s_sinceId) {
      s_sinceId = v;
    }
  }

  s_loaded = true;
  s_status = "idle";
  /* ⚠ The token is never logged, here or anywhere. It is the only thing standing between the
   * LAN and a readable copy of every text on the account. */
  log_e("SMSMIRROR poll configured: %s:%u", s_host, (unsigned)s_port);
  return true;
}

bool smsMirrorPollConfigured() {
  return loadConfig();
}

bool smsMirrorPollBusy() {
  return s_state != PS_IDLE;
}

void smsMirrorPollRequestNow() {
  s_forceNow = true;
}

const char* smsMirrorPollStatus() {
  return s_status ? s_status : "idle";
}

// ---------------------------------------------------------------- helpers
static void finish(const char* why, bool ok, int added, int lines) {
  s_client.stop();
  s_state = PS_IDLE;
  s_status = ok ? "idle" : why;
  s_nextPollMs = millis() + (ok ? POLL_INTERVAL_MS : RETRY_INTERVAL_MS);

  /* ⚠ SAY SOMETHING. Both outcomes were silent in the first version, and that made a
   * working poll and a broken one indistinguishable — the same trap this feature keeps
   * setting for itself, and the reason the mesh hook logs at log_e too.
   *
   * A FAILURE always prints: it is rare, and it is the thing someone is looking for.
   * A SUCCESS prints only when it did something, plus once for the first one ever, so a
   * healthy phone is not writing a line every two minutes forever. */
  static bool s_everSucceeded = false;
  if (!ok) {
    log_e("SMSMIRROR poll failed: %s (retry in %us)", why,
          (unsigned)(RETRY_INTERVAL_MS / 1000));
  } else if (added > 0 || !s_everSucceeded) {
    log_e("SMSMIRROR poll ok: %d new of %d record(s), since=%lld",
          added, lines, (long long)s_sinceId);
  }
  if (ok) {
    s_everSucceeded = true;

    /* Persist the mark when it moved — see SINCE_FILE. Written only on a successful poll
     * that actually advanced, so a phone hearing nothing new never touches the card. */
    static int64_t s_persistedSinceId = 0;
    if (s_sinceId > s_persistedSinceId) {
      /* ⚠ arduino-esp32 1.0.6 does NOT create parent directories on open-for-write, so on
       * a card prepared on a computer (config at the root, /roms never made by the
       * uploader) the persist would fail silently forever. mkdir is idempotent here. */
      SD.mkdir("/roms");
      File sf = SD.open(SINCE_FILE, FILE_WRITE);
      if (sf) {
        char buf[24];
        int w = snprintf(buf, sizeof(buf), "%lld", (long long)s_sinceId);
        if (w > 0) {
          sf.write((const uint8_t*)buf, (size_t)w);
        }
        sf.close();
        s_persistedSinceId = s_sinceId;
      } else {
        static bool s_warnedOnce = false;
        if (!s_warnedOnce) {
          s_warnedOnce = true;
          log_e("SMSMIRROR: cannot write %s - since-id not persisting (full resync every boot)",
                SINCE_FILE);
        }
      }
    }
  }

  if (!ok) {
    /* Re-resolve next time. A connection that failed is the best evidence available that a
     * cached address has gone stale — COVEY moves between SmithWifi and {user}-wifi and comes
     * back on a different subnet. */
    s_hostResolvedAt = 0;

    /* ⚠ AND RE-READ THE CONFIG FILE. Once it loaded successfully, loadConfig() short-
     * circuited forever — so editing smsmirror.txt to point at COVEY's new address did
     * NOTHING until the phone was rebooted, and the poller went on dialling a dead IP while
     * the fix sat unread on the card. A failure is precisely the moment the stored address
     * is most likely to be the thing that is wrong, so that is when it is worth re-reading.
     * The 30 s throttle in loadConfig() keeps this from touching the SD card in a loop. */
    s_loaded = false;
  }
}

/* Resolve, with our own cache on top of resolveDomain()'s.
 *
 * ⚠ resolveDomain() deliberately does NOT cache `.local` answers, so calling it every poll
 * with `covey.local` would be a 500 ms BLOCKING mDNS query every two minutes — small, but
 * exactly the class of thing that added up to the 5-second freezes. Caching here costs one
 * stale connect() after COVEY changes address, which finish() already handles. */
static bool resolveHost() {
  const uint32_t now = millis() | 1u;
  if (s_hostResolvedAt && (uint32_t)(now - s_hostResolvedAt) < HOST_CACHE_MS &&
      (uint32_t)s_hostIp != 0) {
    return true;
  }
  IPAddress ip;
  if (ip.fromString(s_host)) {          // a literal address costs no lookup at all
    s_hostIp = ip;
    s_hostResolvedAt = now;
    return true;
  }
  ip = resolveDomain(s_host);
  if ((uint32_t)ip == 0) {
    return false;
  }
  s_hostIp = ip;
  s_hostResolvedAt = now;
  return true;
}

// One CSM1 line -> the store. Returns 1 if it was new.
static int ingestLine() {
  s_line[s_lineLen] = '\0';
  int added = 0;
  if (s_lineOverflow) {
    /* A line longer than any record we can build. Dropped rather than parsed from its
     * middle, and SAID SO: a silently skipped record is a text that never appears, with
     * nothing anywhere to explain it. */
    log_e("SMSMIRROR poll: over-long line dropped (%u bytes)", (unsigned)s_lineLen);
  } else if (smsMirrorIsMirrorLine(s_line)) {
    s_lines++;
    int r = smsMirrorIngestLine(s_line);
    if (r > 0) {
      added = 1;
      s_added++;
    }
    /* ⚠ THE MARK ADVANCES ONLY PAST RECORDS THE STORE ACCEPTED (stored, r>0, or already
     * had, r==0). A REFUSED record (r<0: no SIP account yet, an unbuildable peer URI, the
     * message db failing to load) must NOT be skipped: with the mark persisted to SD, one
     * refusal would become PERMANENT silent loss of that text — before persistence, a
     * reboot's since=0 refetch quietly healed exactly this case, and the fix must not
     * remove that healing. A refused record is re-served next poll and deduped once
     * whatever refused it is fixed. */
    if (r >= 0) {
      SmsMirrorRecord rec;
      if (smsMirrorUnpack(s_line, &rec) && rec.id > s_sinceId) {
        s_sinceId = rec.id;
      }
    }
  }
  s_lineLen = 0;
  s_lineOverflow = false;
  return added;
}

// ---------------------------------------------------------------- the step
int smsMirrorPollLoop(bool mayUseNetwork) {
  // Config and status FIRST, before the gate — see the header for why. Noticing that a
  // config file has appeared is not network work and must not be skipped with it.
  if (!loadConfig()) {
    return 0;
  }
  if (!mayUseNetwork && s_state == PS_IDLE) {
    return 0;                       // nothing in flight: just do not start anything
  }
  const uint32_t now = millis();

  if (s_state == PS_IDLE) {
    /* Signed difference, not `now < s_nextPollMs`: millis() wraps every ~49 days and an
     * unsigned compare across the wrap would park the poller for another 49. */
    if (!s_forceNow && (int32_t)(now - s_nextPollMs) < 0) {
      return 0;
    }
    if (WiFi.status() != WL_CONNECTED) {
      s_status = "waiting for wifi";
      s_nextPollMs = now + POLL_INTERVAL_MS;
      return 0;
    }
    s_forceNow = false;
    if (!resolveHost()) {
      finish("host not found", false, 0, 0);
      return 0;
    }
    s_state = PS_CONNECT;
    s_status = "connecting";
    return 0;                       // connect on the NEXT pass, so this one stays cheap
  }

  if (s_state == PS_CONNECT) {
    /* The one call here that can block, bounded to CONNECT_TIMEOUT_MS. On a LAN a real
     * COVEY answers in single-digit milliseconds; the timeout only bites when it is off. */
    if (!s_client.connect(s_hostIp, s_port, CONNECT_TIMEOUT_MS)) {
      finish("no answer from COVEY", false, 0, 0);
      return 0;
    }
    char req[320];
    int w = snprintf(req, sizeof(req),
                     "GET /sms?since=%lld HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "%s: %s\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     (long long)s_sinceId, s_host, "X-Covey-Token", s_token);
    if (w <= 0 || (size_t)w >= sizeof(req)) {
      finish("request too long", false, 0, 0);
      return 0;
    }
    s_client.print(req);
    s_state = PS_HEADERS;
    s_status = "reading";
    s_lines = 0;                    // per-POLL totals for the summary in finish()
    s_added = 0;
    s_deadlineMs = now + READ_TIMEOUT_MS;
    s_pollStartMs = now;
    s_lineLen = 0;
    s_lineOverflow = false;
    return 0;
  }

  // --- PS_HEADERS / PS_BODY: consume at most READ_BUDGET bytes this pass.
  if ((int32_t)(now - s_deadlineMs) >= 0) {
    finish("timed out", false, s_added, s_lines);
    return 0;
  }
  if ((uint32_t)(now - s_pollStartMs) > TOTAL_BUDGET_MS) {
    finish("poll exceeded total budget", false, s_added, s_lines);
    return 0;
  }

  int added = 0;
  int budget = READ_BUDGET;
  int consumed = 0;                    // actual bytes taken this pass — see the re-arm below
  while (budget-- > 0 && s_client.available()) {
    int c = s_client.read();
    if (c < 0) {
      break;
    }
    consumed++;
    if (c == '\r') {
      continue;                    // CRLF: the LF does the work
    }
    if (c == '\n') {
      if (s_state == PS_HEADERS) {
        if (s_lineLen == 0) {
          s_state = PS_BODY;       // blank line ends the headers
        } else if (s_lineLen >= 12 && !strncmp(s_line, "HTTP/1.", 7)) {
          s_line[s_lineLen] = '\0';
          if (strstr(s_line, " 200") == NULL) {
            /* 403 is the token being wrong, and it is worth saying plainly: every other
             * failure mode here looks the same from the outside. */
            log_e("SMSMIRROR poll: %s", s_line);
            finish(strstr(s_line, " 403") ? "rejected: bad token" : "COVEY refused", false, 0, 0);
            return added;
          }
        }
        s_lineLen = 0;
        s_lineOverflow = false;
      } else {
        added += ingestLine();
      }
      continue;
    }
    if (s_lineLen < sizeof(s_line) - 1) {
      s_line[s_lineLen++] = (char)c;
    } else {
      s_lineOverflow = true;       // keep draining to the newline; drop the record
    }
  }

  /* ⚠ `consumed`, not `budget < READ_BUDGET`: the loop's `budget--` decrements before the
   * available() check, so budget was always below READ_BUDGET even on a zero-byte pass and
   * the stall deadline re-armed forever — only the 120 s total cap ever fired. Caught by
   * the adversarial review the same evening it shipped. */
  if (consumed > 0) {
    s_deadlineMs = now + READ_TIMEOUT_MS;    // bytes actually arrived: re-arm the deadline
  }

  if (!s_client.connected() && !s_client.available()) {
    if (s_state == PS_BODY && s_lineLen) {
      added += ingestLine();       // a final line with no trailing newline
    }
    finish("idle", true, s_added, s_lines);
  }
  return added;
}
