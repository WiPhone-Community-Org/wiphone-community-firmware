/*
 * wav_reader.h — what a .wav says, and how to get it to the headphone jack in stereo.
 *
 * Arduino-free on purpose, like book_layout: tests/test_music.cpp compiles this file with
 * the Mac's compiler under ASan and feeds it real headers, including malformed ones.
 *
 * ── WHY WAV FIRST, AND WHY MP3 IS A SEPARATE JOB ────────────────────────────────────
 *
 * The phone's audio path turns out to be built for stereo music already, and only the
 * decoder was ever missing. configureI2S() selects I2S_CHANNEL_FMT_RIGHT_LEFT whenever
 * monoOut is false, playChunk() has a direct interleaved L/R case, the codec is a real
 * WM875x, the I2S clock runs off the APLL (so 44100 is exact, not approximated), and
 * `playDec[2400]` is exactly 1152 stereo frames — one MPEG-1 Layer III frame, which is
 * not a coincidence. The vestigial `Playback::LocalMp3` enum, the commented-out
 * playFile("/ringtone.mp3") in WiPhone.ino and "TODO: migrate to dr_mp3" at the top of
 * Audio.h are the rest of that unfinished work.
 *
 * MP3 is therefore worth having and is deliberately NOT in the way of shipping music.
 * The problem is not the audio path, it is where a decoder lives: the internal heap runs
 * about 16 KB free with WiFi up, libhelix wants ~29 KB, and its individual allocations
 * are each under the 16 KB threshold at which arduino-esp32 diverts a malloc to PSRAM —
 * so an unpatched helix takes the memory the WiFi PHY needs, which is precisely the
 * failure that rebooted the phone when a book was opened.
 *
 * So the format layer is a seam, not a decision: WAV ships now and costs no heap and
 * almost no CPU, and an MP3 decoder becomes another producer of interleaved frames into
 * the same buffer. Building it the other way round would put a working music player
 * behind an allocator problem.
 *
 * ⚠ Convert on the desktop if you care how it sounds. The resampler here is linear
 * interpolation — fine, but not a good anti-aliasing filter. ffmpeg's is. Matching the
 * output rate skips it entirely, and `passthrough()` then makes playback a memcpy.
 */

#ifndef WAV_READER_H
#define WAV_READER_H

#include <stdint.h>
#include <stddef.h>

struct WavInfo {
  uint32_t sampleRate;
  uint16_t channels;
  uint16_t bits;         // 8, 16, 24 or 32, integer PCM
  uint32_t dataOffset;   // byte offset of the data chunk's PAYLOAD within the file
  uint32_t dataBytes;    // payload length
  bool     ok;
  const char* problem;   // why not, in words fit to show on screen
};

/* Walk the RIFF chunks in `buf` (the first bytes of a file) and fill `out`.
 *
 * ⚠ Both `fmt ` and `data` must appear within `buf`. Real files put them in the first few
 * hundred bytes; a file with a huge LIST/INFO block ahead of `data` is rejected with a
 * problem string rather than mis-parsed. The app reads 1 KB, which covers everything
 * ffmpeg, sox and QuickTime emit.
 *
 * Returns out->ok. Never reads outside `buf` even on a truncated or hostile file — the
 * uploader accepts anything, so this is parsing untrusted input. */
bool wavParseHeader(const uint8_t* buf, size_t len, WavInfo* out);

/* Bytes per source frame (all channels, one sample time). */
inline uint32_t wavFrameBytes(const WavInfo& in) {
  return (uint32_t)in.channels * (in.bits / 8);
}

/* Turns source frames into output frames at the phone's rate.
 *
 * An OUTPUT FRAME is 2 interleaved samples (L,R) in stereo or 1 in mono — the same
 * layout Audio::playChunk() already expects for dataChannels 2 and 1 respectively, so
 * the result can be written straight into `playDec`.
 *
 * Pull-based and resumable: feed it whole frames, it writes what fits and reports how
 * many source frames it used, so the caller can hold back the remainder. That matters
 * because the file is read in fixed blocks that do not divide evenly into frames. */
class WavConverter {
public:
  WavConverter();

  /* `stereoOut` follows the headphone jack: stereo when something is plugged in, mono
   * into the earpiece otherwise. A mono SOURCE played in stereo is duplicated to both
   * channels rather than being left silent on the right. */
  void begin(const WavInfo& in, uint32_t outRate, bool stereoOut);

  /* Convert up to `srcFrames` source frames into at most `outFrames` output frames.
   * Returns output FRAMES written; sets *framesUsed to source frames consumed.
   * `out` must have room for outFrames * outChannels() samples. */
  size_t feed(const uint8_t* src, size_t srcFrames, int16_t* out, size_t outFrames, size_t* framesUsed);

  int  outChannels() const { return stereo ? 2 : 1; }

  /* True when no work is needed at all: rates match, and the source channel count already
   * matches the output. The caller can then read straight from the file into playDec and
   * skip this class entirely, which is the common case for a properly converted library. */
  bool passthrough() const { return direct; }

private:
  uint16_t channels;    // source channels
  uint16_t bits;
  bool     stereo;      // output channels == 2
  uint32_t step;        // source frames per output frame, 16.16 fixed point
  uint32_t frac;        // position between s0 and s1, 16.16

  /* s0 and s1 are two CONSECUTIVE source frames and `frac` is the position between them,
   * so every output obeys out = lerp(s0, s1, frac). Priming that invariant needs two
   * frames and the first call may carry only one — `havePair` is what lets the second
   * arrive later without the first output being wrong.
   * ⚠ Getting this wrong does not crash and does not look wrong in a debugger; it plays
   * the file at the wrong speed. The test suite asserts on exact decimated samples for
   * this reason. */
  int16_t  s0L, s0R, s1L, s1R;
  bool     primed;
  bool     havePair;
  bool     direct;

  void readFrame(const uint8_t* src, size_t frame, int16_t* l, int16_t* r) const;
};

#endif // WAV_READER_H
