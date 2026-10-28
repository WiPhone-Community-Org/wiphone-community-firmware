#!/usr/bin/env python3
"""check_ai_glue.py - Menu > AI's current facts and its patience keep their rules in the device
files (2026-10-11).

test_gemini and test_wiki prove the pure halves (gemini.cpp: the date sentence, no tool, the
background in the question's turn, the sources line and its strip, rule 0; wiki_lookup.cpp: the
keywords, the URL, the parse, the background). The rules below live in GLUE the host suite cannot
compile - ai_net.cpp, https_worker.cpp/.h - and each is a decision a later edit could undo with
every test still green:

  1. PATIENCE. AI_TIMEOUT_MS is past HTTPClient's uint16 (65,535 ms: free-tier 3.8 Flash took 84 s
     for one sentence) and no more than 5 minutes; postOnce hands it on with waitSliceMs =
     AI_SLICE_MS, and stopFn = tileFetchCallRecent (a call that rings in ends the wait - review
     2026-10-11). NetHttpReq.timeoutMs is a uint32_t (a uint16_t would wrap 150000 to 18,928
     silently), it has a waitSliceMs, a totalMs and a stopFn.
  2. THE CLIENT WAITS, HTTPClient DOES NOT. netHttps makes its client a NetClient, clamps
     http.setTimeout() to NET_HTTP_TIMEOUT_MAX, arms the client (armFirstByte with q->stopFn)
     BEFORE sendRequest under a condition on waitSliceMs, calls the client's own deadline
     GEM_NET_TIMEOUT, checks Cancel / stopFn after DNS, connects within what is left of totalMs,
     and readBody ends on Cancel / stopFn too.
  3. THE WAIT ITSELF (NetClient::available): only once the request is written (m_sent) - before
     that HTTPClient's connect()/sendHeader() call it too, and a wait there holds the request; it
     honours `*m_cancel` AND m_stopFn, sleeps (vTaskDelay - never spins beside the loop task),
     stop()s at the deadline (or HTTPClient starts a 65 s wait of its own) and at a Cancel, and
     never calls the virtual connected() (which reads through available() and would come back
     into the wait). write() arms it only for a WHOLE write.
  4. TODAY'S DATE, FROM A TRUSTED CLOCK, PER REQUEST; THE BACKGROUND IN THE QUESTION'S TURN.
     aiRun builds the system instruction with geminiSystemText(utcNow(), ...) inside its loop,
     before the request is built; utcNow() is ntpClock.getTrustedUtcTime() (0 = no date at all);
     both geminiBuildRequest calls send that text (never the fixed GEM_SYSTEM_INSTRUCTION) and
     s_work->bg as the background. After a request that got no answer, a call ends the question.
  5. THE ANSWER IS SCRUBBED after geminiAnswerText, and its sources line goes only with background.
  6. THE ZONE'S NAME only when it is the offset the clock is on (e.ok, e.minutes * 60 == tzS).
  7. THE WIKIPEDIA LOOKUP (aiWikiLookup): `lookup=off` honoured before anything; the URL built
     from wikiKeywords' output alone; ONE netHttps call and no loop (never retried); the policy
     User-Agent (a contact URL in it); the ISRG X1 + X2 + Root YE pins; the 24 KB cap; a total
     budget of 20 s at most and a connect cap of 10 s; Cancel and a call honoured; NO header, body
     or key - nothing but the keywords leaves. aiRun calls it once, BEFORE the ladder's loop.
  8. NO TOOL. Neither "url_context" nor "tools" nor google_search in any request the code
     builds (gemini.cpp, ai_net.cpp, code only - comments may tell the story).
  9. THE CHAT KEEPS THE PLAIN QUESTION. aiLoopTick folds s_req->question; the background
     (s_work->bg) is read nowhere but in aiWikiLookup and aiRun.

Round 3 (2026-10-11):
 10. "RETRY IF IT TIMES OUT". aiRetryTimeouts reads NVS ("wpmesh") with ON as the absent value;
     aiAsk copies it into the question; aiRun hands it to the ladder right after geminiLadderStart
     and gives geminiLadderNext the attempt's `net` (rule 6 needs it); a request ended by Cancel is
     the user's word, ended BEFORE the ladder can call it a timeout. The background is built once,
     outside the loop (contract 7), so a retry never looks Wikipedia up again.
 11. THE LOOKUP'S ROUND-3 RULES. wikiKeywords is given the topic's earlier questions (all of them -
     it walks back past a follow-up), and the background comes only through wikiCompose with the
     keywords (relevance; all or nothing: no sources line, no background) - never wikiBackground /
     wikiSources directly.
 12. THE ROUND-2 REVIEW. postOnce asks for the heap bar before every Gemini handshake (q.heapCheck),
     and a NOMEM failure says "reboot first" (lowMemText); netHttps clamps HTTPClient's own wait to
     what is left of a budget right before the request (and sends nothing with none left), tells the
     client where the budget ends, and calls a body cut by it TIMEOUT; NetClient keeps a READING
     GUARD after the first byte - a Cancel, a call or the budget's end stop()s it - instead of
     disarming.
 13. THE KEY FOLDER IS WRITE-ONLY ON THE UPLOADER (app_gbc_xfer.cpp, 2026-10-11). The resume
     question (GET /chunk, both transports) goes through xferHeld, which answers 0 for a key file
     (xferKeyPath = files_paths.h's filePathIsKeyFile: an "API Keys" folder, or a gemini.txt) BEFORE
     any SD.open; chunkOpenFor refuses a resume (off != 0) of a key file with 409 before it opens
     one; and nothing in the file opens a file to READ but xferHeld's stat and /health.log, nor
     lists a folder (no openNextFile / SD.exists) - the server may be an open hotspot.

A contract that cannot find its file or function fails too. The self-test first re-applies each
mistake to an in-memory copy of the real sources and requires the check to trip.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, match_close, strip_code  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
ROOT = REPO / "WiPhone"
FILES = ("ai_net.cpp", "https_worker.cpp", "https_worker.h", "wiki_lookup.h", "gemini.cpp", "app_gbc_xfer.cpp")

_STRIPPED = {}


def stripped(text):
    key = hash(text)
    if key not in _STRIPPED:
        _STRIPPED[key] = strip_code(text)
    return _STRIPPED[key]


def load():
    return {name: (ROOT / name).read_text(encoding="utf-8", errors="replace") for name in FILES}


def body(code, fn, out, where):
    span = function_body(code, fn)
    if not span:
        out.append("%s: %s not found" % (where, fn))
    return span


def args_of(code, start, end, name):
    """The argument text of every call `name(...)` in code[start:end], whitespace squeezed."""
    out = []
    for m in re.finditer(r"\b%s\s*\(" % re.escape(name), code[start:end]):
        po = start + m.end() - 1
        pc = match_close(code, po)
        if pc > 0:
            out.append((start + m.start(), re.sub(r"\s+", " ", code[po + 1:pc]).strip()))
    return out


def define(raw, name):
    """The value text of `#define name ...` in the RAW source (string literals intact), or None."""
    m = re.search(r"^#define\s+%s\s+(.*)$" % re.escape(name), raw, re.M)
    return m.group(1).strip() if m else None


def check(texts):
    out = []
    code = {k: stripped(v) for k, v in texts.items()}
    ai, hw, hh = code["ai_net.cpp"], code["https_worker.cpp"], code["https_worker.h"]

    # 1. the timeout and the request fields
    m = re.search(r"#define\s+AI_TIMEOUT_MS\s+(\d+)u?\b", ai)
    if not m:
        out.append("ai_net.cpp: AI_TIMEOUT_MS not found")
    else:
        v = int(m.group(1))
        if v <= 65535 or v > 300000:
            out.append("ai_net.cpp: AI_TIMEOUT_MS %d - must be past HTTPClient's 65535 and at most 5 min" % v)
    span = body(ai, "postOnce", out, "ai_net.cpp")
    if span:
        s, e = span
        if not re.search(r"\bq\s*\.\s*timeoutMs\s*=\s*AI_TIMEOUT_MS\s*;", ai[s:e]):
            out.append("postOnce: q.timeoutMs is not AI_TIMEOUT_MS")
        if not re.search(r"\bq\s*\.\s*waitSliceMs\s*=\s*AI_SLICE_MS\s*;", ai[s:e]):
            out.append("postOnce: q.waitSliceMs = AI_SLICE_MS missing - the wait would not honour Cancel in slices")
        if not re.search(r"\bq\s*\.\s*stopFn\s*=\s*tileFetchCallRecent\s*;", ai[s:e]):
            out.append("postOnce: q.stopFn = tileFetchCallRecent missing - a ringing call would wait behind the answer")
        if not re.search(r"\bq\s*\.\s*heapCheck\s*=\s*true\s*;", ai[s:e]):
            out.append("postOnce: q.heapCheck = true missing - a heap fragmented by the lookup's handshake would fail generically")
    if not re.search(r"\buint32_t\s+timeoutMs\s*;", hh):
        out.append("https_worker.h: NetHttpReq.timeoutMs is not a uint32_t (150000 would wrap)")
    if not re.search(r"\buint32_t\s+waitSliceMs\s*;", hh):
        out.append("https_worker.h: NetHttpReq has no uint32_t waitSliceMs")
    if not re.search(r"\buint32_t\s+totalMs\s*;", hh):
        out.append("https_worker.h: NetHttpReq has no uint32_t totalMs")
    if not re.search(r"\bNetStopFn\s+stopFn\s*;", hh):
        out.append("https_worker.h: NetHttpReq has no NetStopFn stopFn")
    if not re.search(r"#define\s+NET_HTTP_TIMEOUT_MAX\s+65535u?\b", hh):
        out.append("https_worker.h: NET_HTTP_TIMEOUT_MAX is not 65535 (HTTPClient's uint16)")

    # 2. netHttps
    span = body(hw, "netHttps", out, "https_worker.cpp")
    if span:
        s, e = span
        h = hw[s:e]
        if not re.search(r"\bNetClient\s*\*\s*c\s*=\s*new\s*\(\s*std\s*::\s*nothrow\s*\)\s*NetClient\s*\(", h):
            out.append("netHttps: the client is not a NetClient - nothing can wait past 65 s")
        send = re.search(r"\bhttp\s*\.\s*sendRequest\s*\(", h)
        st = args_of(hw, s, e, "setTimeout")
        if not st or "NET_HTTP_TIMEOUT_MAX" not in st[0][1]:
            out.append("netHttps: http.setTimeout() is not clamped to NET_HTTP_TIMEOUT_MAX (a uint16 would wrap)")
        # 12. the budget's clamp, right before the request
        restm = re.search(r"\bconst\s+uint32_t\s+rest\s*=\s*netLeft\s*\(\s*q\s*,\s*t0\s*\)\s*;", h)
        clamp = re.search(r"\bif\s*\(\s*q\s*->\s*totalMs\s*\)\s*\{\s*uint32_t\s+hw\s*=\s*q\s*->\s*timeoutMs\s*<\s*"
                          r"NET_HTTP_TIMEOUT_MAX\s*\?[^;]*;\s*if\s*\(\s*rest\s*<\s*hw\s*\)\s*\{\s*hw\s*=\s*rest\s*;\s*\}\s*"
                          r"http\s*\.\s*setTimeout\s*\(\s*\(\s*uint16_t\s*\)\s*hw\s*\)\s*;", h)
        if not restm or not clamp or not send or not (restm.start() < clamp.start() < send.start()):
            out.append("netHttps: HTTPClient's wait is not clamped to what is left of a budget right before the request")
        if len(st) != 2:
            out.append("netHttps: %d setTimeout calls - the default at the start and the budget's clamp" % len(st))
        if not re.search(r"\bif\s*\(\s*connected\s*&&\s*rest\s*==\s*0\s*\)\s*\{\s*r\s*->\s*net\s*=\s*GEM_NET_TIMEOUT\s*;", h):
            out.append("netHttps: a budget spent before the request still sends it")
        if not re.search(r"\bif\s*\(\s*q\s*->\s*totalMs\s*\)\s*\{\s*c\s*->\s*budgetEnds\s*\(\s*t0\s*\+\s*q\s*->\s*totalMs\s*\)\s*;", h):
            out.append("netHttps: the client is not told where the budget ends (the reading guard would not end it)")
        if not re.search(r"\bif\s*\(\s*c\s*->\s*timedOut\s*\(\s*\)\s*\)\s*\{\s*r\s*->\s*net\s*=\s*GEM_NET_TIMEOUT\s*;[^}]*\}\s*code\s*=\s*0\s*;", h):
            out.append("netHttps: a body cut at the budget's end is not called TIMEOUT")
        arm = re.search(r"\bc\s*->\s*armFirstByte\s*\(\s*\w+\s*,\s*q\s*->\s*waitSliceMs\s*,\s*q\s*->\s*cancel\s*,"
                        r"\s*q\s*->\s*stopFn\s*\)", h)
        send = re.search(r"\bhttp\s*\.\s*sendRequest\s*\(", h)
        if not arm:
            out.append("netHttps: the client is never armed (armFirstByte(wait, q->waitSliceMs, q->cancel, q->stopFn))")
        elif not send or arm.start() > send.start():
            out.append("netHttps: armFirstByte is not before sendRequest - the wait would never run")
        else:
            # ...under an `if (` whose condition names waitSliceMs
            before = hw[s:s + arm.start()]
            ifm = list(re.finditer(r"\bif\s*\(", before))
            okc = False
            if ifm:
                po = s + ifm[-1].end() - 1
                pc = match_close(hw, po)
                okc = pc > 0 and "waitSliceMs" in hw[po:pc]
            if not okc:
                out.append("netHttps: armFirstByte is not under a condition on q->waitSliceMs (the weather would wait too)")
        if not re.search(r"\belse\s+if\s*\(\s*c\s*->\s*timedOut\s*\(\s*\)\s*\)\s*\{\s*r\s*->\s*net\s*=\s*GEM_NET_TIMEOUT\s*;", h):
            out.append("netHttps: the client's own deadline is not GEM_NET_TIMEOUT ('did not answer in time')")
        dns = re.search(r"\bWiFi\s*\.\s*hostByName\s*\(", h)
        stp = re.search(r"\bif\s*\(\s*netStopped\s*\(\s*q\s*\)\s*\)", h)
        if not dns or not stp or stp.start() < dns.start() or (send and stp.start() > send.start()):
            out.append("netHttps: Cancel / stopFn is not checked after DNS, before the handshake")
        if re.search(r"\bsetConnectTimeout\s*\([^;]*\bq\s*->\s*connectMs|\bc\s*->\s*connect\s*\([^;]*\bq\s*->\s*connectMs", h):
            out.append("netHttps: a connect uses q->connectMs, not the budget-clamped connectMs")
        if not re.search(r"\bconnectMs\s*=\s*q\s*->\s*connectMs\s*<\s*left\s*\?", h):
            out.append("netHttps: the connect is not shortened to what is left of totalMs")
        rb = args_of(hw, s, e, "readBody")
        if not rb or not all(a.endswith("t0") for _, a in rb):
            out.append("netHttps: readBody is not given t0 (the body would not end at totalMs)")
    span = body(hw, "readBody", out, "https_worker.cpp")
    if span:
        s, e = span
        if not re.search(r"\bif\s*\(\s*netStopped\s*\(\s*q\s*\)\s*\)", hw[s:e]):
            out.append("readBody: Cancel / stopFn not honoured between reads")
        if not re.search(r"\bnetLeft\s*\(\s*q\s*,\s*t0\s*\)\s*==\s*0", hw[s:e]):
            out.append("readBody: the totalMs budget does not end the body")
    span = body(hw, "netStopped", out, "https_worker.cpp")
    if span and not re.search(r"q\s*->\s*stopFn\s*&&\s*q\s*->\s*stopFn\s*\(\s*\)", hw[span[0]:span[1]]):
        out.append("netStopped: does not ask q->stopFn")

    # 3. NetClient
    cls = re.search(r"\bclass\s+NetClient\s*:\s*public\s+WiFiClientSecure\s*\{", hw)
    if not cls:
        out.append("https_worker.cpp: class NetClient : public WiFiClientSecure not found")
    else:
        cs, ce = cls.end() - 1, match_close(hw, cls.end() - 1)
        cl = hw[cs:ce]
        av = function_body(cl, "available")
        if not av:
            out.append("NetClient: available() not found")
        else:
            a = cl[av[0]:av[1]]
            gate = re.search(r"\bif\s*\(\s*!\s*m_armed\s*\|\|\s*!\s*m_sent\s*\)\s*\{\s*return\s+n\s*;", a)
            loop = re.search(r"\bfor\s*\(\s*;\s*;\s*\)|\bwhile\s*\(", a)
            if not gate or (loop and gate.start() > loop.start()):
                out.append("NetClient::available: the wait is not gated on m_sent (it would hold the request itself)")
            if not re.search(r"\bif\s*\(\s*\(\s*m_cancel\s*&&\s*\*\s*m_cancel\s*\)\s*\|\|\s*\(\s*m_stopFn\s*&&\s*m_stopFn\s*\(\s*\)\s*\)\s*\)"
                             r"\s*\{[^}]*\bstop\s*\(\s*\)\s*;", a):
                out.append("NetClient::available: a Cancel or a call (m_stopFn) does not end the wait (stop())")
            if not re.search(r"\bm_timedOut\s*=\s*true\s*;\s*stop\s*\(\s*\)\s*;", a):
                out.append("NetClient::available: the deadline does not stop() the client (HTTPClient would wait 65 s more)")
            if not re.search(r"\bvTaskDelay\s*\(", a):
                out.append("NetClient::available: the wait does not sleep - it would spin beside the loop task")
            # 12. the reading guard (after the first byte), before the wait's loop
            gd = re.search(r"\bif\s*\(\s*m_answered\s*\)\s*\{", a)
            if not gd or (loop and gd.start() > loop.start()):
                out.append("NetClient::available: no reading guard after the first byte (if (m_answered) {...})")
            else:
                g = a[gd.start():match_close(a, gd.end() - 1)]
                if not re.search(r"\btold\s*=\s*\(\s*m_cancel\s*&&\s*\*\s*m_cancel\s*\)\s*\|\|\s*\(\s*m_stopFn\s*&&\s*m_stopFn\s*\(\s*\)\s*\)", g):
                    out.append("NetClient: the reading guard does not honour a Cancel AND a call")
                if not re.search(r"\bspent\s*=\s*m_hasEnd\s*&&", g):
                    out.append("NetClient: the reading guard does not end at the budget")
                if not re.search(r"\bif\s*\(\s*told\s*\|\|\s*spent\s*\)\s*\{[^}]*\bstop\s*\(\s*\)\s*;\s*return\s+0\s*;", g):
                    out.append("NetClient: the reading guard does not stop() the client")
            if re.search(r"\bm_armed\s*=\s*false\s*;\s*//\s*the answer began", a) or \
                    len(re.findall(r"\bm_answered\s*=\s*true\s*;", a)) < 2:
                out.append("NetClient::available: the first byte disarms the client - no guard while the answer is read")
            if re.search(r"(?<![\w:])connected\s*\(", a) or re.search(r"\bWiFiClientSecure\s*::\s*connected\s*\(", a):
                out.append("NetClient::available: calls connected(), which reads through available() - back into the wait")
        wr = function_body(cl, "write")
        if not wr:
            out.append("NetClient: write() not found")
        elif not re.search(r"\bif\s*\(\s*size\s*>\s*0\s*&&\s*n\s*==\s*size\s*\)\s*\{[^}]*\bm_sent\s*=\s*true",
                           cl[wr[0]:wr[1]]):
            out.append("NetClient::write: the wait is armed by a write that was not whole")

    # 4 + 5. aiRun
    span = body(ai, "aiRun", out, "ai_net.cpp")
    if span:
        s, e = span
        lp = re.search(r"\bfor\s*\(\s*;\s*;\s*\)\s*\{", ai[s:e])
        sysc = args_of(ai, s, e, "geminiSystemText")
        builds = args_of(ai, s, e, "geminiBuildRequest")
        if not sysc:
            out.append("aiRun: geminiSystemText is not called - no date")
        else:
            pos, a = sysc[0]
            if not a.startswith("utcNow()"):
                out.append("aiRun: geminiSystemText is not given utcNow() (the TRUSTED clock): %s" % a)
            if not lp or pos < s + lp.start():
                out.append("aiRun: geminiSystemText is outside the request loop - the date would not be per request")
            if builds and pos > builds[0][0]:
                out.append("aiRun: geminiSystemText comes after the request is built")
        if len(builds) != 2:
            out.append("aiRun: geminiBuildRequest called %d times (the length, then the body)" % len(builds))
        for _, a in builds:
            parts = [p.strip() for p in a.split(",")]
            if len(parts) < 6 or parts[2] != "q->question" or parts[3] != "s_work->bg" or parts[4] != "s_work->sys":
                out.append("aiRun: geminiBuildRequest(%s) - not the plain question, the lookup's background and "
                           "the built instruction" % a)
        if re.search(r"\bGEM_SYSTEM_INSTRUCTION\b", ai[s:e]):
            out.append("aiRun: sends the fixed GEM_SYSTEM_INSTRUCTION (no date)")
        at = args_of(ai, s, e, "geminiAnswerText")
        sc = re.search(r"\bgeminiScrub\s*\(\s*r\s*->\s*answer\s*,", ai[s:e])
        if not sc or not at or sc.start() + s < at[0][0]:
            out.append("aiRun: the answer is not scrubbed after geminiAnswerText")
        if at and "s_work->bg[0] ? s_work->src : NULL" not in at[0][1]:
            out.append("aiRun: the sources line is not tied to background having gone (%s)" % at[0][1])
        wl = args_of(ai, s, e, "aiWikiLookup")
        if len(wl) != 1:
            out.append("aiRun: aiWikiLookup called %d times - once a question, never per retry" % len(wl))
        elif not lp or wl[0][0] > s + lp.start():
            out.append("aiRun: aiWikiLookup is inside the request loop - it would be retried")
        post = re.search(r"\bpostOnce\s*\(", ai[s:e])
        call = re.search(r"\bif\s*\(\s*code\s*<=\s*0\s*&&\s*!\s*s_cancel\s*&&\s*tileFetchCallRecent\s*\(\s*\)\s*\)", ai[s:e])
        if not call or not post or call.start() < post.start():
            out.append("aiRun: a request ended by a call is not told apart after postOnce (it would read 'connection dropped')")
        # 10. rule 6's inputs
        start = re.search(r"\bgeminiLadderStart\s*\(", ai[s:e])
        rt = re.search(r"\bl\s*\.\s*retryTimeouts\s*=\s*q\s*->\s*retryTimeouts\s*;", ai[s:e])
        if not rt or not start or rt.start() < start.start() or (lp and rt.start() > lp.start()):
            out.append("aiRun: the ladder is not given the setting right after geminiLadderStart (l.retryTimeouts = q->retryTimeouts)")
        nx = args_of(ai, s, e, "geminiLadderNext")
        if len(nx) != 1 or [x.strip() for x in nx[0][1].split(",")][3:5] != ["code", "net"]:
            out.append("aiRun: geminiLadderNext is not given the attempt's code and net (rule 6 needs why)")
        cn = re.search(r"\bif\s*\(\s*code\s*<=\s*0\s*&&\s*s_cancel\s*\)\s*\{\s*r\s*->\s*cancelled\s*=\s*true\s*;\s*break\s*;", ai[s:e])
        if not cn or not post or cn.start() < post.start() or (nx and s + cn.start() > nx[0][0]):
            out.append("aiRun: a request ended by Cancel is not ended before the ladder (rule 6 would retry it)")
        nm = re.search(r"\bif\s*\(\s*code\s*<=\s*0\s*&&\s*net\s*==\s*GEM_NET_NOMEM\s*\)\s*\{\s*lowMemText\s*\(\s*r\s*->\s*fail\s*,", ai[s:e])
        if not nm:
            out.append("aiRun: a NOMEM failure does not say 'reboot first' (lowMemText)")
    span = body(ai, "utcNow", out, "ai_net.cpp")
    if span and not re.search(r"\bntpClock\s*\.\s*getTrustedUtcTime\s*\(\s*\)", ai[span[0]:span[1]]):
        out.append("utcNow: not ntpClock.getTrustedUtcTime() - a mesh-set clock would date the question")

    # 6. aiAsk's zone name
    span = body(ai, "aiAsk", out, "ai_net.cpp")
    if span:
        s, e = span
        if not re.search(r"\bif\s*\(\s*e\s*\.\s*ok\s*&&\s*e\s*\.\s*name\s*&&\s*e\s*\.\s*minutes\s*\*\s*60\s*==\s*s_req\s*->\s*tzS\s*\)",
                         ai[s:e]):
            out.append("aiAsk: the zone's name is not tied to the clock's offset (e.ok, e.minutes * 60 == tzS)")
        if not re.search(r"\bs_req\s*->\s*retryTimeouts\s*=\s*aiRetryTimeouts\s*\(\s*\)\s*;", ai[s:e]):
            out.append("aiAsk: the question does not carry \"Retry if it times out\" (s_req->retryTimeouts)")
    span = body(ai, "aiRetryTimeouts", out, "ai_net.cpp")
    if span:
        rw = strip_comments(texts["ai_net.cpp"][span[0]:span[1]])
        if not re.search(r"\bgetUChar\s*\(\s*AI_PREFS_RETRY\s*,\s*1\s*\)", rw):
            out.append("aiRetryTimeouts: absent is not ON (getUChar(AI_PREFS_RETRY, 1))")
    if ((define(texts["ai_net.cpp"], "AI_PREFS_NS") or "").split() or [""])[0] != '"wpmesh"':
        out.append("ai_net.cpp: AI_PREFS_NS is not the firmware's \"wpmesh\" namespace")

    # 7. the lookup
    raw_ai, raw_wh = texts["ai_net.cpp"], texts["wiki_lookup.h"]
    span = body(ai, "aiWikiLookup", out, "ai_net.cpp")
    if span:
        s, e = span
        w = ai[s:e]
        off = re.search(r"\bif\s*\(\s*!\s*q\s*->\s*cfg\s*\.\s*lookup\s*\)\s*\{[^}]*\breturn\s*;", w)
        kwc = re.search(r"\bwikiKeywords\s*\(", w)
        net = list(re.finditer(r"\bnetHttps\s*\(", w))
        if not off or (kwc and off.start() > kwc.start()) or (net and off.start() > net[0].start()):
            out.append("aiWikiLookup: lookup=off is not honoured before anything else")
        if not re.search(r"\bwikiKeywords\s*\(\s*q\s*->\s*question\s*,", w):
            out.append("aiWikiLookup: the keywords are not the question's (wikiKeywords(q->question, ...))")
        # 11. the walk back and the all-or-nothing composer
        kwa = args_of(ai, s, e, "wikiKeywords")
        if not kwa or [x.strip() for x in kwa[0][1].split(",")][1:3] != ["earlier", "nEarlier"] or \
                not re.search(r"\bconst\s+int\s+nEarlier\s*=\s*topicQuestions\s*\(\s*q\s*,\s*earlier\s*\)\s*;", w):
            out.append("aiWikiLookup: wikiKeywords is not given the topic's earlier questions (the walk back)")
        cp = args_of(ai, s, e, "wikiCompose")
        if len(cp) != 1 or [x.strip() for x in cp[0][1].split(",")][:2] != ["&s_work->wiki", "s_work->kw"]:
            out.append("aiWikiLookup: the background does not come through wikiCompose(&s_work->wiki, s_work->kw, ...) - relevance")
        if re.search(r"\bwikiBackground\s*\(|\bwikiSources\s*\(", w):
            out.append("aiWikiLookup: wikiBackground/wikiSources called directly - past relevance and all-or-nothing")
        if not re.search(r"\bwikiUrl\s*\(\s*s_work\s*->\s*kw\s*,\s*s_work\s*->\s*wikiUrl\s*,", w):
            out.append("aiWikiLookup: the URL is not built from the keywords alone (wikiUrl(s_work->kw, ...))")
        if not re.search(r"\bw\s*\.\s*url\s*=\s*s_work\s*->\s*wikiUrl\s*;", w):
            out.append("aiWikiLookup: the request's URL is not the one built from the keywords")
        if len(net) != 1:
            out.append("aiWikiLookup: %d netHttps calls - exactly ONE GET a question" % len(net))
        if re.search(r"\bfor\s*\(|\bwhile\s*\(|\bgoto\b", w):
            out.append("aiWikiLookup: a loop - the lookup is never retried")
        rw = strip_comments(raw_ai[s:e])            # the same function, string literals intact
        if not re.search(r'\bw\s*\.\s*method\s*=\s*"GET"\s*;', rw):
            out.append("aiWikiLookup: not a GET")
        if re.search(r"\bhdrName\b|\bhdrValue\b|\bcfg\s*\.\s*key\b|\bw\s*\.\s*body\b|\bctxA\b", w) or \
                "x-goog-api-key" in rw:
            out.append("aiWikiLookup: a header, a body, the key or the chat in the request - only the keywords may go")
        for field, want in (("userAgent", "WIKI_USER_AGENT"), ("rootsPem", "AI_WIKI_ROOTS_PEM"),
                            ("cap", "WIKI_RESPONSE_CAP"), ("totalMs", "AI_WIKI_TOTAL_MS"),
                            ("connectMs", "AI_WIKI_CONNECT_MS"), ("stopFn", "tileFetchCallRecent"),
                            ("waitSliceMs", "AI_SLICE_MS"), ("sink", "netWorkerSink ( )")):
            if not re.search(r"\bw\s*\.\s*%s\s*=\s*%s\s*;" % (field, re.escape(want).replace("\\ ", r"\s*")), w):
                out.append("aiWikiLookup: w.%s is not %s" % (field, want))
        if not re.search(r"\bw\s*\.\s*cancel\s*=\s*&\s*s_cancel\s*;", w):
            out.append("aiWikiLookup: Cancel not honoured (w.cancel = &s_cancel)")
    span = body(ai, "topicQuestions", out, "ai_net.cpp")
    if span and not re.search(r"\bout\s*\[\s*n\+\+\s*\]\s*=\s*q\s*->\s*ctxQ\s*\[\s*i\s*\]\s*;", ai[span[0]:span[1]]):
        out.append("topicQuestions: not the context's questions (q->ctxQ)")
    ua = define(raw_wh, "WIKI_USER_AGENT")
    if not ua or not re.match(r'"[A-Za-z][\w.-]*/[\d.]+ \(https://\S+\)[^"]*"$', ua):
        out.append("wiki_lookup.h: WIKI_USER_AGENT is not Wikimedia's policy form (name/version (contact URL)): %s" % ua)
    cap = define(raw_wh, "WIKI_RESPONSE_CAP")
    if not cap or not re.match(r"\(\s*24u?\s*\*\s*1024u?\s*\)", cap):
        out.append("wiki_lookup.h: WIKI_RESPONSE_CAP is not 24 KB: %s" % cap)
    roots = define(raw_ai, "AI_WIKI_ROOTS_PEM")
    if roots is None or roots.split() != ["ISRG_ROOT_X1_PEM", "ISRG_ROOT_X2_PEM", "ISRG_ROOT_YE_PEM"]:
        out.append("ai_net.cpp: AI_WIKI_ROOTS_PEM is not ISRG X1 + X2 + Root YE (en.wikipedia.org's chain): %s" % roots)
    for name, most in (("AI_WIKI_TOTAL_MS", 20000), ("AI_WIKI_CONNECT_MS", 10000)):
        v = define(raw_ai, name)
        m = re.match(r"(\d+)u?\b", v or "")
        if not m or not 0 < int(m.group(1)) <= most:
            out.append("ai_net.cpp: %s is %s - at most %d ms" % (name, v, most))

    # 8. no tool: in the CODE (comments out, string literals kept - where a JSON key would be)
    for name in ("gemini.cpp", "ai_net.cpp"):
        if re.search(r'url_context|google_search|\\"tools\\"', strip_comments(texts[name])):
            out.append("%s: a tool in the code (url_context / \"tools\" / google_search) - none is ever sent" % name)

    # 9. the chat keeps the plain question
    span = body(ai, "aiLoopTick", out, "ai_net.cpp")
    if span:
        s, e = span
        fold = args_of(ai, s, e, "geminiChatFold")
        if not fold or [p.strip() for p in fold[0][1].split(",")][5] != "s_req->question":
            out.append("aiLoopTick: the chat is not folded with the PLAIN question (s_req->question)")
    for m in re.finditer(r"\bs_work\s*->\s*bg\b", ai):
        fs = [f for f in ("aiWikiLookup", "aiRun") if (lambda sp: sp and sp[0] <= m.start() <= sp[1])(function_body(ai, f))]
        if not fs:
            out.append("ai_net.cpp: the background (s_work->bg) is used outside aiWikiLookup/aiRun")
            break

    # 13. the key folder is write-only on the uploader
    xf, raw_xf = code["app_gbc_xfer.cpp"], texts["app_gbc_xfer.cpp"]
    span = body(xf, "xferKeyPath", out, "app_gbc_xfer.cpp")
    if span and not re.search(r"\breturn\s+filePathIsKeyFile\s*\(\s*path\s*\.\s*c_str\s*\(\s*\)\s*\)\s*!=\s*0\s*;",
                              xf[span[0]:span[1]]):
        out.append("xferKeyPath: not files_paths.h's filePathIsKeyFile (the one rule for what a key file is)")
    held = body(xf, "xferHeld", out, "app_gbc_xfer.cpp")
    if held:
        h = xf[held[0]:held[1]]
        guard = re.search(r"\bif\s*\(\s*xferKeyPath\s*\(\s*path\s*\)\s*\)\s*\{\s*return\s+0\s*;", h)
        op = re.search(r"\bSD\s*\.\s*open\s*\(", h)
        if not guard or not op or guard.start() > op.start():
            out.append("xferHeld: a key file is stat'd - `if (xferKeyPath(path)) { return 0; }` must come before SD.open")
    span = body(xf, "chunkOpenFor", out, "app_gbc_xfer.cpp")
    if span:
        h = xf[span[0]:span[1]]
        guard = re.search(r"\bif\s*\(\s*off\s*!=\s*0\s*&&\s*xferKeyPath\s*\(\s*path\s*\)\s*\)\s*\{\s*\*\s*code\s*=\s*409\s*;"
                          r"[^}]*\breturn\s+false\s*;", h)
        op = re.search(r"\bSD\s*\.\s*open\s*\(", h)
        if not guard or not op or guard.start() > op.start():
            out.append("chunkOpenFor: a key file can be resumed from the card (no 409 for off != 0 before the open)")
    for fn in ("handleChunkGet",):
        span = body(xf, fn, out, "app_gbc_xfer.cpp")
        if span and not re.search(r"\bxferHeld\s*\(", xf[span[0]:span[1]]):
            out.append("%s: the resume question does not go through xferHeld" % fn)
    if len(re.findall(r"\bxferHeld\s*\(\s*name\s*\)", xf)) != 2:
        out.append("app_gbc_xfer.cpp: the two GET /chunk answers (legacy and raw) do not both use xferHeld(name)")
    for m in re.finditer(r"\bSD\s*\.\s*open\s*\(", xf):
        po = m.end() - 1
        pc = match_close(xf, po)
        a = xf[po:pc] if pc > 0 else ""
        if re.search(r"\bFILE_(?:WRITE|APPEND)\b", a):
            continue                                   # a write: the uploader's job
        if held and held[0] <= m.start() <= held[1]:
            continue                                   # xferHeld's stat, behind the key guard
        if raw_xf[po:pc + 1] == '("/health.log")':
            continue                                   # /log: that one file only
        out.append("app_gbc_xfer.cpp:%d: SD.open%s opens a file to READ (only /health.log and xferHeld may)"
                   % (xf.count("\n", 0, m.start()) + 1, re.sub(r"\s+", " ", raw_xf[po:pc + 1])))
    if re.search(r"\bopenNextFile\s*\(|\bSD\s*\.\s*exists\s*\(|\bopendir\s*\(", xf):
        out.append("app_gbc_xfer.cpp: the uploader lists or probes the card (openNextFile / SD.exists / opendir)")
    return out


def strip_comments(t):
    """Comments out, string literals KEPT (strip_code blanks both)."""
    out, i, n = [], 0, len(t)
    while i < n:
        if t.startswith("//", i):
            j = t.find("\n", i)
            i = n if j < 0 else j
        elif t.startswith("/*", i):
            j = t.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif t[i] == '"':
            j = i + 1
            while j < n and t[j] != '"':
                j += 2 if t[j] == "\\" else 1
            out.append(t[i:j + 1])
            i = j + 1
        elif t[i] == "'":
            j = i + 1
            while j < n and t[j] != "'":
                j += 2 if t[j] == "\\" else 1
            out.append(t[i:j + 1])
            i = j + 1
        else:
            out.append(t[i])
            i += 1
    return "".join(out)


MUTATIONS = [
    ("the resume probe stats a key file", "app_gbc_xfer.cpp",
     lambda t: t.replace("  if (xferKeyPath(path)) {\n    return 0;", "  if (false) {\n    return 0;", 1)),
    ("a key file resumed from the card", "app_gbc_xfer.cpp",
     lambda t: t.replace("    if (off != 0 && xferKeyPath(path)) {", "    if (false) {", 1)),
    ("a key file is whatever the uploader says", "app_gbc_xfer.cpp",
     lambda t: t.replace("  return filePathIsKeyFile(path.c_str()) != 0;", "  return false;", 1)),
    ("the legacy GET /chunk stats the card itself", "app_gbc_xfer.cpp",
     lambda t: t.replace("  const size_t held = xferHeld(name);\n  char out[16];",
                         "  File kf = SD.open((String(s_cfg->dir) + \"/\" + name).c_str());\n"
                         "  const size_t held = kf ? (size_t)kf.size() : 0;\n  char out[16];", 1)),
    ("a route that reads a file", "app_gbc_xfer.cpp",
     lambda t: t.replace("static void handleNotFound() {\n",
                         "static void handleNotFound() {\n  File peek = SD.open(s_server->uri().c_str());\n", 1)),
    ("a route that lists a folder", "app_gbc_xfer.cpp",
     lambda t: t.replace("static void handleNotFound() {\n",
                         "static void handleNotFound() {\n  File d = SD.open(s_cfg->dir, FILE_WRITE); d.openNextFile();\n", 1)),
    ("the 60 s timeout back", "ai_net.cpp",
     lambda t: t.replace("#define AI_TIMEOUT_MS       150000u", "#define AI_TIMEOUT_MS       60000u", 1)),
    ("no slices for the wait", "ai_net.cpp",
     lambda t: t.replace("  q.waitSliceMs = AI_SLICE_MS;", "", 1)),
    ("a call does not end Gemini's wait", "ai_net.cpp",
     lambda t: t.replace("  q.stopFn = tileFetchCallRecent;", "", 1)),
    ("timeoutMs a uint16 again", "https_worker.h",
     lambda t: t.replace("  uint32_t     timeoutMs;", "  uint16_t     timeoutMs;", 1)),
    ("the unclamped setTimeout", "https_worker.cpp",
     lambda t: re.sub(r"http\.setTimeout\([^;]*;", "http.setTimeout(q->timeoutMs);", t, count=1)),
    ("a plain WiFiClientSecure", "https_worker.cpp",
     lambda t: t.replace("NetClient* c = new (std::nothrow) NetClient();", "WiFiClientSecure* c = new (std::nothrow) WiFiClientSecure();", 1)),
    ("never armed", "https_worker.cpp",
     lambda t: t.replace("c->armFirstByte(wait, q->waitSliceMs, q->cancel, q->stopFn);", ";", 1)),
    ("armed without the call stop", "https_worker.cpp",
     lambda t: t.replace("c->armFirstByte(wait, q->waitSliceMs, q->cancel, q->stopFn);", "c->armFirstByte(wait, q->waitSliceMs, q->cancel, NULL);", 1)),
    ("armed for every request", "https_worker.cpp",
     lambda t: t.replace("if (q->waitSliceMs || q->timeoutMs > NET_HTTP_TIMEOUT_MAX) {", "if (true) {", 1)),
    ("the deadline called LOST", "https_worker.cpp",
     lambda t: t.replace("        } else if (c->timedOut()) {\n          r->net = GEM_NET_TIMEOUT;", "        } else if (c->timedOut()) {\n          r->net = GEM_NET_LOST;", 1)),
    ("no stop check after DNS", "https_worker.cpp",
     lambda t: t.replace("  if (netStopped(q)) {\n    r->net = GEM_NET_LOST;\n    r->ms = millis() - t0;\n    return 0;\n  }", "", 1)),
    ("the connect not shortened to the budget", "https_worker.cpp",
     lambda t: t.replace("connected = c->connect(q->host, 443, (int32_t)connectMs) == 1;", "connected = c->connect(q->host, 443, (int32_t)q->connectMs) == 1;", 1)),
    ("the body past the budget", "https_worker.cpp",
     lambda t: t.replace("if (!readBody(q, r, http, chunked, http.getSize(), t0)) {", "if (!readBody(q, r, http, chunked, http.getSize(), millis())) {", 1)),
    ("the body ignores a call", "https_worker.cpp",
     lambda t: t.replace("  return (q->cancel && *q->cancel) || (q->stopFn && q->stopFn());", "  return (q->cancel && *q->cancel);", 1)),
    ("the wait before the request is written", "https_worker.cpp",
     lambda t: t.replace("    if (!m_armed || !m_sent) {\n      return n;", "    if (!m_armed) {\n      return n;", 1)),
    ("Cancel ignored in the wait", "https_worker.cpp",
     lambda t: t.replace("      if ((m_cancel && *m_cancel) || (m_stopFn && m_stopFn())) {", "      if (m_stopFn && m_stopFn()) {", 1)),
    ("a call ignored in the wait", "https_worker.cpp",
     lambda t: t.replace("      if ((m_cancel && *m_cancel) || (m_stopFn && m_stopFn())) {", "      if (m_cancel && *m_cancel) {", 1)),
    ("no stop at the deadline", "https_worker.cpp",
     lambda t: t.replace("        m_timedOut = true;\n        stop();", "        m_timedOut = true;", 1)),
    ("a spinning wait", "https_worker.cpp",
     lambda t: t.replace("      vTaskDelay(pdMS_TO_TICKS(m_sliceMs));", "", 1)),
    ("the virtual connected() in the wait", "https_worker.cpp",
     lambda t: t.replace("      if (k != 0 || !_connected) {", "      if (k != 0 || !connected()) {", 1)),
    ("a short write arms the wait", "https_worker.cpp",
     lambda t: t.replace("if (size > 0 && n == size) {", "if (size > 0) {", 1)),
    ("the date from an untrusted clock", "ai_net.cpp",
     lambda t: t.replace("geminiSystemText(utcNow(),", "geminiSystemText((int64_t)ntpClock.getExactUtcTime(),", 1)),
    ("the fixed instruction sent", "ai_net.cpp",
     lambda t: t.replace("s_work->bg, s_work->sys, l.budget, s_body", "s_work->bg, GEM_SYSTEM_INSTRUCTION, l.budget, s_body", 1)),
    ("no background sent", "ai_net.cpp",
     lambda t: t.replace("q->question, s_work->bg, s_work->sys, l.budget, s_body", "q->question, NULL, s_work->sys, l.budget, s_body", 1)),
    ("the background glued into the question (and so the chat)", "ai_net.cpp",
     lambda t: t.replace("q->question, s_work->bg, s_work->sys, l.budget,\n", "s_work->bg, q->question, s_work->sys, l.budget,\n", 1)),
    ("the answer not scrubbed", "ai_net.cpp",
     lambda t: t.replace("        geminiScrub(r->answer, sizeof(r->answer), q->cfg.key);\n", "", 1)),
    ("a sources line without background", "ai_net.cpp",
     lambda t: t.replace("s_work->bg[0] ? s_work->src : NULL", "s_work->src", 1)),
    ("utcNow trusting a mesh clock", "ai_net.cpp",
     lambda t: t.replace("return (int64_t)ntpClock.getTrustedUtcTime();", "return (int64_t)ntpClock.getExactUtcTime();", 1)),
    ("a zone name for another offset", "ai_net.cpp",
     lambda t: t.replace("if (e.ok && e.name && e.minutes * 60 == s_req->tzS) {", "if (e.name) {", 1)),
    ("a call after the request not told apart", "ai_net.cpp",
     lambda t: t.replace("if (code <= 0 && !s_cancel && tileFetchCallRecent()) {", "if (false) {", 1)),
    ("the lookup per retry", "ai_net.cpp",
     lambda t: t.replace("  aiWikiLookup(q, r);\n", "", 1).replace("    const uint32_t ta = millis();\n", "    aiWikiLookup(q, r);\n    const uint32_t ta = millis();\n", 1)),
    ("the lookup never made", "ai_net.cpp",
     lambda t: t.replace("  aiWikiLookup(q, r);\n", "", 1)),
    ("lookup=off ignored", "ai_net.cpp",
     lambda t: t.replace("  if (!q->cfg.lookup) {\n", "  if (false) {\n", 1)),
    ("the whole question to Wikipedia", "ai_net.cpp",
     lambda t: t.replace("wikiUrl(s_work->kw, s_work->wikiUrl,", "wikiUrl(q->question, s_work->wikiUrl,", 1)),
    ("a retried lookup", "ai_net.cpp",
     lambda t: t.replace("  int code = netHttps(&w, &res);", "  int code = 0;\n  for (int i = 0; i < 2 && code != 200; i++) code = netHttps(&w, &res);", 1)),
    ("the key sent to Wikipedia", "ai_net.cpp",
     lambda t: t.replace("  w.userAgent = WIKI_USER_AGENT;", "  w.userAgent = WIKI_USER_AGENT;\n  w.hdrName[0] = \"x-goog-api-key\";\n  w.hdrValue[0] = q->cfg.key;", 1)),
    ("a generic User-Agent", "ai_net.cpp",
     lambda t: t.replace("  w.userAgent = WIKI_USER_AGENT;", "  w.userAgent = AI_USER_AGENT;", 1)),
    ("a User-Agent with no contact", "wiki_lookup.h",
     lambda t: t.replace('"NH-devices/1.0 (https://github.com/Nikguy321/wiphone-meshtastic) WiPhone"', '"NH-devices/1.0 WiPhone"', 1)),
    ("Google's roots for Wikipedia", "ai_net.cpp",
     lambda t: t.replace("  w.rootsPem = AI_WIKI_ROOTS_PEM;", "  w.rootsPem = AI_ROOTS_PEM;", 1)),
    ("X1 alone for Wikipedia", "ai_net.cpp",
     lambda t: t.replace("#define AI_WIKI_ROOTS_PEM   ISRG_ROOT_X1_PEM ISRG_ROOT_X2_PEM ISRG_ROOT_YE_PEM", "#define AI_WIKI_ROOTS_PEM   ISRG_ROOT_X1_PEM", 1)),
    ("the whole sink for Wikipedia", "ai_net.cpp",
     lambda t: t.replace("  w.cap = WIKI_RESPONSE_CAP;", "  w.cap = NET_SINK_CAP;", 1)),
    ("a 64 KB response cap", "wiki_lookup.h",
     lambda t: t.replace("#define WIKI_RESPONSE_CAP  (24u * 1024u)", "#define WIKI_RESPONSE_CAP  (64u * 1024u)", 1)),
    ("no total budget", "ai_net.cpp",
     lambda t: t.replace("  w.totalMs = AI_WIKI_TOTAL_MS;", "", 1)),
    ("a 60 s lookup", "ai_net.cpp",
     lambda t: t.replace("#define AI_WIKI_TOTAL_MS    20000u", "#define AI_WIKI_TOTAL_MS    60000u", 1)),
    ("Cancel ignored by the lookup", "ai_net.cpp",
     lambda t: t.replace("  w.cancel = &s_cancel;", "", 1)),
    ("a call ignored by the lookup", "ai_net.cpp",
     lambda t: t.replace("  w.stopFn = tileFetchCallRecent;", "", 1)),
    ("the url_context tool back", "gemini.cpp",
     lambda t: t.replace("  wChar(&w, ']');                                  // and never \"tools\"",
                         "  wChar(&w, ']');\n  wStr(&w, \",\\\"tools\\\":[{\\\"url_context\\\":{}}]\");", 1)),
    ("the chat folded with the background", "ai_net.cpp",
     lambda t: t.replace("geminiChatFold(c, s_req->askedUtc, s_req->topic, !ok, s_res->label, s_req->question, a,",
                         "geminiChatFold(c, s_req->askedUtc, s_req->topic, !ok, s_res->label, s_work->bg, a,", 1)),
    # round 3: "Retry if it times out", the lookup's rules, the round-2 review
    ("the ladder never told the setting", "ai_net.cpp",
     lambda t: t.replace("  l.retryTimeouts = q->retryTimeouts;", "", 1)),
    ("the setting not in the question", "ai_net.cpp",
     lambda t: t.replace("  s_req->retryTimeouts = aiRetryTimeouts();", "", 1)),
    ("the retry OFF when never set", "ai_net.cpp",
     lambda t: t.replace("p.getUChar(AI_PREFS_RETRY, 1)", "p.getUChar(AI_PREFS_RETRY, 0)", 1)),
    ("the setting in another namespace", "ai_net.cpp",
     lambda t: t.replace('#define AI_PREFS_NS     "wpmesh"', '#define AI_PREFS_NS     "aiprefs"', 1)),
    ("the ladder not told why there was no answer", "ai_net.cpp",
     lambda t: t.replace("code, net, rep, utcNow()", "code, GEM_NET_OK, rep, utcNow()", 1)),
    ("a Cancel retried as a timeout", "ai_net.cpp",
     lambda t: t.replace("    if (code <= 0 && s_cancel) {\n", "    if (false) {\n", 1)),
    ("the previous question only", "ai_net.cpp",
     lambda t: t.replace("wikiKeywords(q->question, earlier, nEarlier,", "wikiKeywords(q->question, earlier + nEarlier - 1, 1,", 1)),
    ("relevance skipped (no keywords to compose with)", "ai_net.cpp",
     lambda t: t.replace("wikiCompose(&s_work->wiki, s_work->kw,", "wikiCompose(&s_work->wiki, s_work->wikiUrl,", 1)),
    ("the background built past wikiCompose", "ai_net.cpp",
     lambda t: t.replace("  r->wikiPages = used;\n}", "  wikiBackground(&s_work->wiki, WIKI_BG_MAX, s_work->bg, sizeof(s_work->bg), NULL);\n  r->wikiPages = used;\n}", 1)),
    ("no heap bar before Gemini's handshake", "ai_net.cpp",
     lambda t: t.replace("  q.heapCheck = true;\n", "", 1)),
    ("NOMEM as a generic failure", "ai_net.cpp",
     lambda t: t.replace("if (code <= 0 && net == GEM_NET_NOMEM) {", "if (false) {", 1)),
    ("the budget not clamped before the request", "https_worker.cpp",
     lambda t: t.replace("          if (rest < hw) {\n            hw = rest;\n          }\n", "", 1)),
    ("the clamp gone (only the start's wait)", "https_worker.cpp",
     lambda t: t.replace("          http.setTimeout((uint16_t)hw);\n", "", 1)),
    ("a spent budget still sends", "https_worker.cpp",
     lambda t: t.replace("      if (connected && rest == 0) {", "      if (false) {", 1)),
    ("the guard not told the budget's end", "https_worker.cpp",
     lambda t: t.replace("            c->budgetEnds(t0 + q->totalMs);\n", "", 1)),
    ("a body cut at the budget called LOST", "https_worker.cpp",
     lambda t: t.replace("            if (c->timedOut()) {\n              r->net = GEM_NET_TIMEOUT;", "            if (c->timedOut()) {\n              r->net = GEM_NET_LOST;", 1)),
    ("the first byte disarms (no reading guard)", "https_worker.cpp",
     lambda t: t.replace("      m_answered = true;                           // the answer began", "      m_armed = false;                             // the answer began", 1)),
    ("the guard ignores a Cancel", "https_worker.cpp",
     lambda t: t.replace("const bool told = (m_cancel && *m_cancel) || (m_stopFn && m_stopFn());", "const bool told = (m_stopFn && m_stopFn());", 1)),
    ("the guard ignores a call", "https_worker.cpp",
     lambda t: t.replace("const bool told = (m_cancel && *m_cancel) || (m_stopFn && m_stopFn());", "const bool told = (m_cancel && *m_cancel);", 1)),
    ("the guard ignores the budget", "https_worker.cpp",
     lambda t: t.replace("const bool spent = m_hasEnd && (int32_t)(millis() - m_endMs) >= 0;", "const bool spent = false;", 1)),
    ("the guard does not stop the client", "https_worker.cpp",
     lambda t: t.replace("          m_timedOut = !told;\n          stop();\n", "          m_timedOut = !told;\n", 1)),
]


def selftest(texts):
    if check(texts):
        return False
    ok = True
    for what, name, f in MUTATIONS:
        mutated = dict(texts)
        mutated[name] = f(texts[name])
        if mutated[name] == texts[name]:
            print("  SELF-TEST: %s - the mutation no longer applies (update it with the code)" % what)
            ok = False
            continue
        if not check(mutated):
            print("  SELF-TEST: %s - not caught" % what)
            ok = False
    return ok


def main():
    texts = load()
    problems = check(texts)
    for p in problems:
        print("  FAIL " + p)
    if problems:
        print("check_ai_glue: %d problem(s)" % len(problems))
        return 1
    if not selftest(texts):
        print("check_ai_glue: SELF-TEST FAILED")
        return 1
    print("check_ai_glue: ok (13 contracts, %d mutations caught)" % len(MUTATIONS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
