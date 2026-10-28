/*
 * https_worker.cpp - see https_worker.h. The worker, its queue of job kinds, and the one HTTPS
 * exchange both kinds make (factored out of ai_net.cpp's postOnce/readBody, 0.9.81: the AI's
 * requests go through it exactly as they went before; the weather's add the timed handshake, the
 * second heap check and the tap).
 */
#include "https_worker.h"
#include "gemini.h"             // GEM_NET_*, and the de-chunker (geminiDechunkFeed)
#include "ai_net.h"             // aiRequestActive (netRequestActive)
#include "weather_net.h"        // wxRequestActive (netRequestActive)
#include "geoloc_net.h"         // geoRequestActive (netRequestActive)

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <new>
#include <string.h>

#define NET_READ_BUF  1024u

static StaticTask_t      s_tcb;
static StackType_t*      s_stack = NULL;
static TaskHandle_t      s_task  = NULL;
static SemaphoreHandle_t s_go    = NULL;
static char*             s_sink  = NULL;    // NET_SINK_CAP + 1, PSRAM
static uint8_t*          s_raw   = NULL;    // a read off the socket (PSRAM)
static char*             s_dec   = NULL;    // ...de-chunked (PSRAM): never more bytes than were read

/* The queue: one flag a kind (set on the loop, cleared by the worker as it starts the job). */
static volatile bool     s_queued[NET_JOB_KINDS] = { false, false, false };
static NetJobFn          s_run[NET_JOB_KINDS] = { NULL, NULL, NULL };
static volatile int      s_running = -1;

void netHeapNow(uint32_t* freeB, uint32_t* largest, uint32_t* minEver) {
  multi_heap_info_t h;
  heap_caps_get_info(&h, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (freeB)   *freeB   = h.total_free_bytes;
  if (largest) *largest = h.largest_free_block;
  if (minEver) *minEver = h.minimum_free_bytes;
}

bool netHeapOkForHandshake(uint32_t* freeB, uint32_t* largest) {
  uint32_t f = 0, l = 0;
  netHeapNow(&f, &l, NULL);
  if (freeB) *freeB = f;
  if (largest) *largest = l;
  const uint32_t extra = s_stack ? 0u : (uint32_t)NET_STACK_BYTES;
  return l >= NET_CONNECT_LARGEST + extra && f >= NET_CONNECT_FREE + extra;
}

static void workerTask(void*) {
  for (;;) {
    xSemaphoreTake(s_go, portMAX_DELAY);
    for (;;) {
      int k = -1;
      for (int i = 0; i < NET_JOB_KINDS; i++) {      // the AI first: a person is waiting on it (then WX, GEO)
        if (s_queued[i] && s_run[i]) {
          k = i;
          break;
        }
      }
      if (k < 0) {
        break;                                       // back on the semaphore: nothing else on this stack
      }
      s_running = k;
      s_queued[k] = false;
      s_run[k]();                                    // returns, so its locals are destructed
      s_running = -1;
    }
  }
}

bool netWorkerReady(char* why, size_t whyCap) {
  if (!s_sink) {
    s_sink = (char*)heap_caps_malloc(NET_SINK_CAP + 1, MALLOC_CAP_SPIRAM);
    s_raw = (uint8_t*)heap_caps_malloc(NET_READ_BUF, MALLOC_CAP_SPIRAM);
    s_dec = (char*)heap_caps_malloc(NET_READ_BUF, MALLOC_CAP_SPIRAM);
    if (!s_sink || !s_raw || !s_dec) {
      free(s_sink);
      free(s_raw);
      free(s_dec);
      s_sink = NULL;
      s_raw = NULL;
      s_dec = NULL;
      strlcpy(why, "No memory for the request", whyCap);
      return false;
    }
  }
#if defined(configSUPPORT_STATIC_ALLOCATION) && configSUPPORT_STATIC_ALLOCATION
  if (!s_stack) {
    s_stack = (StackType_t*)heap_caps_malloc(NET_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_stack) {
      strlcpy(why, "No internal RAM for the network task - reboot first", whyCap);
      return false;
    }
  }
#endif
  if (!s_go) {
    s_go = xSemaphoreCreateBinary();
    if (!s_go) {
      strlcpy(why, "No RAM for the network task", whyCap);
      return false;
    }
  }
  if (!s_task) {
    /* "aiworker": the name `ai`, panicwatch and the bench notes have used since it was the AI's. */
#if defined(configSUPPORT_STATIC_ALLOCATION) && configSUPPORT_STATIC_ALLOCATION
    s_task = xTaskCreateStaticPinnedToCore(workerTask, "aiworker", NET_STACK_BYTES, NULL, 1, s_stack,
                                           &s_tcb, 1);
#else
    BaseType_t created = xTaskCreatePinnedToCore(workerTask, "aiworker", NET_STACK_BYTES,
                                                 NULL, 1, &s_task, 1);
    if (created != pdPASS) s_task = NULL;
#endif
    if (!s_task) {
      strlcpy(why, "Could not start the network task", whyCap);
      return false;
    }
  }
  return true;
}

bool netWorkerCarved() {
  return s_stack != NULL;
}

void netWorkerSubmit(int kind, NetJobFn run) {
  if (kind < 0 || kind >= NET_JOB_KINDS || !s_go) {
    return;
  }
  s_run[kind] = run;
  s_queued[kind] = true;
  xSemaphoreGive(s_go);
}

int netWorkerRunning() {
  return s_running;
}

bool netWorkerQueued(int kind) {
  return kind >= 0 && kind < NET_JOB_KINDS && s_queued[kind];
}

bool netRequestActive() {
  /* Every kind, or one of them runs its handshake at the 80 MHz idle clock beside a 1-tick loop
   * (tests/check_geoloc_review.py pins the Wi-Fi location term). */
  return aiRequestActive() || wxRequestActive() || geoRequestActive();
}

char* netWorkerSink() {
  return s_sink;
}

uint32_t netWorkerStackFloor() {
  return s_task ? (uint32_t)(uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t)) : 0u;
}

// ── one exchange ─────────────────────────────────────────────────────────────────────────

/* Bytes of body into the sink while they fit (the rest flagged, not kept), every one to the tap. */
static void put(const NetHttpReq* q, NetHttpRes* r, const char* p, size_t n) {
  if (q->tap && n) {
    q->tap(q->tapCtx, p, n);
  }
  size_t room = r->got < q->cap ? q->cap - r->got : 0;
  size_t take = n < room ? n : room;
  if (take) {
    memcpy(q->sink + r->got, p, take);
    r->got += take;
  }
  if (take < n) {
    r->overflow = true;
  }
}

/* `cancel`, or the request's other reason to end (stopFn: a call rang in). */
static bool netStopped(const NetHttpReq* q) {
  return (q->cancel && *q->cancel) || (q->stopFn && q->stopFn());
}

/* What is left of the request's totalMs budget since t0; UINT32_MAX when it has none. */
static uint32_t netLeft(const NetHttpReq* q, uint32_t t0) {
  if (!q->totalMs) {
    return 0xFFFFFFFFu;
  }
  const uint32_t used = millis() - t0;
  return used < q->totalMs ? q->totalMs - used : 0u;
}

/* The body after the headers: de-chunked by hand, read to its END whatever the cap (an overflow is
 * flagged, its bytes dropped). True with the whole body read; false with r->net saying why. */
static bool readBody(const NetHttpReq* q, NetHttpRes* r, HTTPClient& http, bool chunked, int size,
                     uint32_t t0) {
  WiFiClient* s = http.getStreamPtr();
  r->got = 0;
  if (!s) {
    r->net = GEM_NET_LOST;
    return false;
  }
  GeminiDechunk d;
  geminiDechunkInit(&d);
  size_t seen = 0;                                   // a Content-Length body's bytes, kept or not
  uint32_t idle = millis();
  for (;;) {
    if (netStopped(q)) {
      r->net = GEM_NET_LOST;
      return false;
    }
    if (netLeft(q, t0) == 0) {
      r->net = GEM_NET_TIMEOUT;                      // the request's whole budget is spent
      return false;
    }
    const int avail = s->available();
    if (avail > 0) {
      const int want = avail < (int)NET_READ_BUF ? avail : (int)NET_READ_BUF;
      const int n = s->read(s_raw, (size_t)want);
      if (n > 0) {
        idle = millis();
        if (chunked) {
          size_t dn = 0;
          /* A read of n raw bytes de-chunks to at most n: s_dec (NET_READ_BUF) never overflows. */
          if (!geminiDechunkFeed(&d, s_raw, (size_t)n, s_dec, NET_READ_BUF, &dn)) {
            r->net = GEM_NET_BAD_BODY;
            return false;
          }
          put(q, r, s_dec, dn);
          if (d.done) {
            break;
          }
        } else {
          size_t n2 = (size_t)n;
          if (size >= 0 && seen + n2 > (size_t)size) {
            n2 = (size_t)size - seen;                // bytes past Content-Length are not body
          }
          put(q, r, (const char*)s_raw, n2);
          seen += n2;
          if (size >= 0 && seen >= (size_t)size) {
            break;
          }
        }
      }
    } else if (!s->connected()) {
      break;                                         // closed: whole only if the framing says so
    } else if (millis() - idle > q->bodyIdleMs) {
      r->net = GEM_NET_TIMEOUT;
      return false;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));                  // never spin: the loop task shares this core
    }
  }
  if ((chunked && !d.done) || (!chunked && size >= 0 && seen < (size_t)size)) {
    r->net = GEM_NET_LOST;
    return false;
  }
  q->sink[r->got] = '\0';
  return true;
}

/* THE WAIT FOR AN ANSWER'S FIRST BYTE, PAST HTTPClient's uint16 (2026-10-11). HTTPClient waits for
 * the response in handleHeaderResponse(), polling _client->available() every 10 ms against
 * _tcpTimeout - a uint16_t (setTimeout(uint16_t): 65,535 ms at most; arduino-esp32 1.0.6
 * HTTPClient.h) - and never looks at a Cancel. A Gemini answer is not sent until it is whole, and
 * one that reads a web page first, or thinks long, ran past COVEY's 60 s. So a request that asks
 * (waitSliceMs, or a timeoutMs past NET_HTTP_TIMEOUT_MAX) is made on this client: once the request
 * has been WRITTEN, its available() itself waits for the first byte - in slices, honouring
 * `cancel`, up to timeoutMs from the write - and from that byte on it is a plain WiFiClientSecure
 * again (the gaps between header lines and the body keep HTTPClient's and readBody's timeouts).
 *   - "Once written": HTTPClient calls available() through connected() BEFORE it writes too
 *     (connect(), sendHeader()); a wait there would hold the request itself. Every byte of the
 *     request goes through write(buf, size) - the headers, then the body - and a short write
 *     disarms the wait (the request failed; WiFiClientSecure stop()s on an error).
 *   - The deadline or a Cancel: the client stop()s, so HTTPClient's next connected() is false and
 *     sendRequest returns at once (HTTPC_ERROR_NOT_CONNECTED); netHttps asks timedOut() to say
 *     TIMEOUT rather than LOST. Without the stop HTTPClient would start a 65 s wait of its own.
 *   - Nothing in the wait calls a virtual that comes back here (WiFiClientSecure::connected()
 *     reads through available()): it reads _connected (WiFiClient's, protected) instead.
 *   - (review 2026-10-11) A CALL ends the wait too: `stopFn` is polled with the Cancel every slice
 *     (the AI passes tileFetchCallRecent - a flag the loop sets every pass from the SIP state), so
 *     a call that rings in during a 2.5-minute wait is not kept waiting behind it; the AI then
 *     says "A call came in".
 *   - (round-2 review, 2026-10-11) AFTER the first byte HTTPClient reads the header lines, waiting
 *     between them up to its own timeout (netHttps clamps it to what is left of a budget) - and
 *     never looks at a Cancel. So while the answer is being read, every time nothing is waiting
 *     (available() would say 0) a Cancel, a call or the request's budget end (budgetEnds: the
 *     Wikipedia lookup's 20 s) stop()s the client too - HTTPClient's next connected() is false and
 *     it returns at once. Data that IS waiting is never cut: readBody checks the same between reads.
 * Unarmed - the weather, Wi-Fi location - it is WiFiClientSecure exactly. */
class NetClient : public WiFiClientSecure {
public:
  void armFirstByte(uint32_t waitMs, uint32_t sliceMs, volatile bool* cancel, NetStopFn stopFn) {
    m_armed = true;
    m_sent = false;
    m_answered = false;
    m_waitMs = waitMs;
    m_sliceMs = sliceMs ? sliceMs : 10u;
    m_cancel = cancel;
    m_stopFn = stopFn;
  }
  /* The request's budget ends at millis() == atMs (an armed request with a totalMs). */
  void budgetEnds(uint32_t atMs) {
    m_hasEnd = true;
    m_endMs = atMs;
  }
  bool timedOut() const {
    return m_timedOut;
  }

  /* (No `using WiFiClientSecure::write`: it re-exports Print's overloads, private there. Nothing
   * writes through a NetClient*: HTTPClient holds a WiFiClient*.) */
  size_t write(const uint8_t* buf, size_t size) override {
    const size_t n = WiFiClientSecure::write(buf, size);
    if (m_armed) {
      if (size > 0 && n == size) {
        m_sent = true;
        m_sentMs = millis();                       // the deadline runs from the LAST byte sent
      } else {
        m_armed = false;
      }
    }
    return n;
  }

  int available() override {
    const int n = WiFiClientSecure::available();
    if (!m_armed || !m_sent) {
      return n;
    }
    if (m_answered) {
      /* Reading the answer: nothing waiting now - a Cancel, a call or the budget's end stops it here
       * (HTTPClient's header loop would sit out its own timeout, blind to all three). */
      if (n == 0 && _connected) {
        const bool told = (m_cancel && *m_cancel) || (m_stopFn && m_stopFn());
        const bool spent = m_hasEnd && (int32_t)(millis() - m_endMs) >= 0;
        if (told || spent) {
          m_armed = false;
          m_timedOut = !told;
          stop();
          return 0;
        }
      }
      return n;
    }
    if (n != 0 || !_connected) {
      m_answered = true;                           // the answer began (or the socket is gone)
      return n;
    }
    for (;;) {
      if ((m_cancel && *m_cancel) || (m_stopFn && m_stopFn())) {
        m_armed = false;
        stop();
        return 0;
      }
      if ((uint32_t)(millis() - m_sentMs) >= m_waitMs) {
        m_armed = false;
        m_timedOut = true;
        stop();
        return 0;
      }
      vTaskDelay(pdMS_TO_TICKS(m_sliceMs));        // the loop task shares this core: never spin
      const int k = WiFiClientSecure::available();
      if (k != 0 || !_connected) {
        m_answered = true;                         // ...the reading guard above from here on
        return k;
      }
    }
  }

private:
  bool           m_armed = false;
  bool           m_sent = false;
  bool           m_answered = false;             // the first byte came: the reading guard
  bool           m_timedOut = false;
  bool           m_hasEnd = false;
  uint32_t       m_endMs = 0;
  uint32_t       m_waitMs = 0;
  uint32_t       m_sliceMs = 10;
  uint32_t       m_sentMs = 0;
  volatile bool* m_cancel = NULL;
  NetStopFn      m_stopFn = NULL;
};

/* A refused / failed connect, by WiFiClientSecure::lastError(). */
static int netOfTls(int le) {
  if (le == -0x2700) return GEM_NET_TLS;             // MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
  if (le == -0x7F00) return GEM_NET_NOMEM;           // MBEDTLS_ERR_SSL_ALLOC_FAILED
  if (le < -1) return GEM_NET_TLS_OTHER;
  return GEM_NET_CONNECT;                            // refused, unreachable, or the handshake cap
}

int netHttps(const NetHttpReq* q, NetHttpRes* r) {
  memset(r, 0, sizeof(*r));
  r->net = GEM_NET_OK;
  const uint32_t t0 = millis();
  if (!q->sink || !q->rootsPem || !s_raw) {
    r->net = GEM_NET_NOMEM;
    return 0;
  }
  q->sink[0] = '\0';
  IPAddress ip;
  if (!WiFi.hostByName(q->host, ip)) {               // first: "no internet" told apart from a refusal
    r->net = GEM_NET_DNS;
    r->dnsMs = r->ms = millis() - t0;
    return 0;
  }
  r->dnsMs = millis() - t0;
  /* DNS can take lwIP's ~14 s: a Cancel, a call or a spent budget meanwhile ends it here, before
   * a handshake starts (one that has started runs to its connect cap). */
  if (netStopped(q)) {
    r->net = GEM_NET_LOST;
    r->ms = millis() - t0;
    return 0;
  }
  const uint32_t left = netLeft(q, t0);
  if (left == 0) {
    r->net = GEM_NET_TIMEOUT;
    r->ms = millis() - t0;
    return 0;
  }
  const uint32_t connectMs = q->connectMs < left ? q->connectMs : left;   // the budget's rest, at most
  if (q->heapCheck && !netHeapOkForHandshake(NULL, NULL)) {
    r->net = GEM_NET_NOMEM;                          // the bar again, right before THIS handshake
    r->ms = millis() - t0;
    return 0;
  }
  NetClient* c = new (std::nothrow) NetClient();   // a WiFiClientSecure; armed only for a long wait
  if (!c) {
    r->net = GEM_NET_NOMEM;
    r->ms = millis() - t0;
    return 0;
  }
  c->setCACert(q->rootsPem);
  int code = 0;
  {
    /* An inner block: ~HTTPClient calls stop() on its client, which must still exist. */
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout((int32_t)connectMs);
    http.setTimeout((uint16_t)(q->timeoutMs < NET_HTTP_TIMEOUT_MAX ? q->timeoutMs : NET_HTTP_TIMEOUT_MAX));
    http.setUserAgent(q->userAgent);
    static const char* WANT[] = { "Transfer-Encoding", "Date" };
    http.collectHeaders(WANT, 2);
    /* begin() BEFORE the client connects: on a fresh HTTPClient (_host "") begin(client, url) finds
     * the host "switched" and stop()s a connected client, and sendRequest then handshakes AGAIN -
     * untimed and past the heap bar (arduino-esp32 1.0.6 HTTPClient::beginInternal; review
     * 2026-10-03). Unconnected here, begin() only records the URL, and sendRequest's connect()
     * finds the socket up and rides it. (The client's setTimeout() it then skips is a no-op on
     * WiFiClientSecure in 1.0.6: nothing is lost.) */
    if (!http.begin(*c, q->url)) {
      r->net = GEM_NET_CONNECT;
    } else {
      bool connected = true;
      if (q->connectFirst) {
        const uint32_t th = millis();
        connected = c->connect(q->host, 443, (int32_t)connectMs) == 1;
        r->hsMs = millis() - th;
        if (!connected) {
          char eb[64];
          r->tlsErr = c->lastError(eb, sizeof(eb));
          r->net = netOfTls(r->tlsErr);
        }
      }
      /* What is left of a budget now that DNS and the handshake are done (UINT32_MAX: none). */
      const uint32_t rest = netLeft(q, t0);
      if (connected && rest == 0) {
        r->net = GEM_NET_TIMEOUT;                    // spent before a byte was sent: nothing goes
        connected = false;
      }
      if (connected) {
        for (int i = 0; i < 3 && q->hdrName[i]; i++) {
          http.addHeader(q->hdrName[i], q->hdrValue[i] ? q->hdrValue[i] : "");
        }
        if (q->totalMs) {
          /* A budget: HTTPClient's own wait (a uint16 - its header loop's gap between lines, and the
           * whole wait when the client is not armed) clamped HERE, right before the request, to what
           * is left of it (round-2 review, 2026-10-11: set only at the start, a header stall could run
           * the 20 s lookup well past 20). The reading guard (NetClient) ends it at the budget too,
           * and at a Cancel or a call. Requests without one keep the wait set above, as before. */
          uint32_t hw = q->timeoutMs < NET_HTTP_TIMEOUT_MAX ? q->timeoutMs : NET_HTTP_TIMEOUT_MAX;
          if (rest < hw) {
            hw = rest;
          }
          http.setTimeout((uint16_t)hw);
        }
        if (q->waitSliceMs || q->timeoutMs > NET_HTTP_TIMEOUT_MAX) {
          /* NetClient, above: up to timeoutMs - or what is left of totalMs, if that is less - and,
           * with a budget, its end for the reading guard. */
          const uint32_t wait = q->timeoutMs < rest ? q->timeoutMs : rest;
          c->armFirstByte(wait, q->waitSliceMs, q->cancel, q->stopFn);
          if (q->totalMs) {
            c->budgetEnds(t0 + q->totalMs);
          }
        }
        code = http.sendRequest(q->method, (uint8_t*)q->body, q->bodyLen);
        netHeapNow(NULL, &r->largestAfterSend, NULL);
        if (code > 0) {
          strlcpy(r->date, http.header("Date").c_str(), sizeof(r->date));
          const bool chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
          if (!readBody(q, r, http, chunked, http.getSize(), t0)) {
            if (c->timedOut()) {
              r->net = GEM_NET_TIMEOUT;              // the reading guard met the budget's end
            }
            code = 0;
          }
        } else if (c->timedOut()) {
          r->net = GEM_NET_TIMEOUT;                // the client's own wait ran out (it stop()ped)
          code = 0;
        } else if (code == HTTPC_ERROR_CONNECTION_REFUSED) {
          char eb[64];
          r->tlsErr = c->lastError(eb, sizeof(eb));
          r->net = netOfTls(r->tlsErr);
          code = 0;
        } else if (code == HTTPC_ERROR_READ_TIMEOUT) {
          r->net = GEM_NET_TIMEOUT;
          code = 0;
        } else if (code == HTTPC_ERROR_TOO_LESS_RAM) {
          r->net = GEM_NET_NOMEM;
          code = 0;
        } else if (code == HTTPC_ERROR_ENCODING) {
          r->net = GEM_NET_BAD_BODY;
          code = 0;
        } else {
          r->net = GEM_NET_LOST;
          code = 0;
        }
      }
    }
    http.end();
  }
  c->stop();
  delete c;
  r->code = code;
  r->ms = millis() - t0;
  return code;
}
