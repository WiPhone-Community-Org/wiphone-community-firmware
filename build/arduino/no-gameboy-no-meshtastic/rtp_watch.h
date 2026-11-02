/*
 * rtp_watch.h — two clocks on a call's RTP session, pulled out of Audio.cpp and WiPhone.ino so
 * the host suite can run them (tests/test_rtpwatch.cpp). Header-only, no Arduino.
 *
 *   RtpSilence  has the far end been quiet long enough to call the call dead?
 *   RtpOrphan   has an RTP session been left armed with no call to own it?
 *
 * ── WHY THE SILENCE RULE MOVED HERE ─────────────────────────────────────────────────────────
 * 🛑 NEITHER HALF OF THE OLD RULE WAS EVER RESET PER CALL. Audio.cpp kept `rtpSilentScan` (the
 * last packet's time) and WiPhone.ino kept `rtpSilentCnt` (how many 60 s windows had passed),
 * and both lived for the whole boot:
 *   - a new call's first packet-less pass measured its silence from the PREVIOUS call's last
 *     packet (or from 0 at boot), so a call placed more than 60 s after the last one scored a
 *     full window of silence before its first packet could arrive;
 *   - the count was 1 after the first window of the boot and, because the teardown did
 *     `cnt = 0` and then fell through to `cnt++`, it was 1 again after every teardown after that.
 * Together: the second call of a boot was torn down the moment it connected, through
 * TinySIP::rtpSilent(), which sends no BYE. Nobody could have seen it while SIP never registered;
 * both phones have registered since 2026-09-04 (`sip=1`), which is what made it reachable.
 *
 * Now the whole rule is one object with one owner (Audio.cpp), started fresh by every call's
 * Audio::newCall(), and a packet clears the strikes: a call ends after RTP_SILENCE_STRIKES windows
 * of CONTINUOUS silence — 120 s — counted within that call only. The main loop no longer counts;
 * RTP_SILENT_ON now means "end this call", nothing weaker.
 *
 * ── WHY THE ORPHAN CLOCK EXISTS ─────────────────────────────────────────────────────────────
 * The hot-mic leak (a finished call's microphone RTP resuming the next time anything powered the
 * codec) was fixed at its source on 2026-08-16: Audio::shutdown() clears every send flag and
 * closes the socket, and every normal call end reaches shutdown(). What remained were the ways
 * a session can be armed WITHOUT a normal end to follow: a SIP account removed or changed
 * mid-call (the SIP block re-inits or stops running, and neither path calls shutdown()), END
 * with no network, a debug easter egg that streamed the mic to a hardcoded LAN address. The idle
 * audio watchdog cannot catch any of them — an armed RtpStream counts as the device moving
 * samples, so it looks busy forever. This clock closes the CLASS instead of listing instances:
 * a session armed while no live call is entitled to it is released after RTP_ORPHAN_RELEASE_MS.
 *
 * ⚠ 3 s IS NOT A TIMEOUT ON ANYTHING REAL. Every call-audio setup arms the session in the same
 * main-loop pass that moves the SIP state into Call, and every teardown calls shutdown() in the
 * same pass that leaves it. A real call is therefore never unentitled for even one pass; the 3 s
 * is headroom for one stalled pass (passes of 1.5-2 s have been measured — LoRa transmits until
 * 0.9.79, a SPIFFS database save still), not a guess at how long a transition takes.
 */
#ifndef RTP_WATCH_H
#define RTP_WATCH_H

#include <stdint.h>

#define RTP_SILENCE_WINDOW_MS  60000u   // one window of no packets at all (was STP_SILENT_PERIOD)
#define RTP_SILENCE_STRIKES    2u       // continuous windows that end a call: 120 s
#define RTP_ORPHAN_RELEASE_MS  3000u    // armed with no call to own it for this long: shut down

struct RtpSilence {
  uint32_t quietSinceMs;   // this call's start, its last packet, or its last strike
  uint8_t  strikes;        // whole windows of silence since the last packet
};

enum RtpQuiet : uint8_t {
  RTP_QUIET_NONE = 0,      // nothing to report
  RTP_QUIET_STRIKE,        // a whole window went by with nothing heard; the call goes on
  RTP_QUIET_END            // RTP_SILENCE_STRIKES windows in a row: end the call
};

/* A call's media starts. Nothing heard yet, and NOTHING FROM THE LAST CALL COUNTS — that is
 * the whole fix; see the note at the top. */
inline void rtpSilenceBegin(RtpSilence& s, uint32_t nowMs) {
  s.quietSinceMs = nowMs;
  s.strikes = 0;
}

/* A packet arrived: the far end is alive. */
inline void rtpSilenceHeard(RtpSilence& s, uint32_t nowMs) {
  s.quietSinceMs = nowMs;
  s.strikes = 0;
}

/* A pass on which no packet arrived. Unsigned subtraction, so millis() wrapping is harmless. */
inline RtpQuiet rtpSilenceQuiet(RtpSilence& s, uint32_t nowMs) {
  if (nowMs - s.quietSinceMs <= RTP_SILENCE_WINDOW_MS) {
    return RTP_QUIET_NONE;
  }
  s.quietSinceMs = nowMs;          // the next strike needs another whole window
  if (++s.strikes >= RTP_SILENCE_STRIKES) {
    s.strikes = 0;
    return RTP_QUIET_END;
  }
  return RTP_QUIET_STRIKE;
}

struct RtpOrphan {
  uint32_t sinceMs;        // when the session was first seen armed with nothing entitled to it
  bool     timing;
};

/* Once per main-loop pass. `armed`: the audio device holds an RTP session (sending the mic, or
 * playing a stream). `entitled`: a live call owns it. Returns true ONCE, when the session has
 * been armed and unowned for RTP_ORPHAN_RELEASE_MS without a break; the caller shuts it down.
 * Any pass on which it is disarmed or owned starts the clock over. */
inline bool rtpOrphanCheck(RtpOrphan& o, bool armed, bool entitled, uint32_t nowMs) {
  if (!armed || entitled) {
    o.timing = false;
    return false;
  }
  if (!o.timing) {
    o.timing = true;
    o.sinceMs = nowMs;
    return false;
  }
  if (nowMs - o.sinceMs < RTP_ORPHAN_RELEASE_MS) {
    return false;
  }
  o.timing = false;
  return true;
}

#endif // RTP_WATCH_H
