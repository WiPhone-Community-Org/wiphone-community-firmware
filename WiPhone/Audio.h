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

/*
 * Audio.h
 *
 *  Class to handle I2S peripheral of ESP32, hardware audio codec, amplifier IC, microphone
 *  data, audio encoding/decoding, audio RTP streams, etc.
*/

// TODO:
// - migrate to dr_mp3
// - add WAV capability (dr_wav)

#ifndef __AUDIO_H_
#define __AUDIO_H_

#include "Arduino.h"
#include "FS.h"
#include "SPI.h"
#include "SPIFFS.h"
#include "driver/i2s.h"
#include "config.h"
#include "Hardware.h"
#include "Networks.h"
#include "helpers.h"
#include "RTPacket.h"
#include "mp3_stream.h"
#include "wav_reader.h"
#include "music_feed.h"
#include "notify_timing.h"   // NotifyRing: how playPop() found the I2S ring (its stop timer's lead)

#define AUDIO_INLINE inline __attribute__((always_inline))

#define DR_WAV_NO_CONVERSION_API
#define DR_WAV_NO_STDIO
#include "src/audio/dr_wav.h"

// These are used in WiPhone.ino
#include "src/audio/g722_encoder.h"
#include "src/audio/g722_decoder.h"
#include "src/audio/g711.h"

extern AUDIO_CODEC_CLASS  codec;

#define LOUDSPEAKER 1
#define EARSPEAKER 0

/* Far-end RTP silence. The window and the strike count live in rtp_watch.h (host-tested); this
 * flag is only the hand-off to the main loop, and since 0.9.79 RTP_SILENT_ON means "END THIS
 * CALL" — two windows of continuous silence within the call — not "one window went by". */
extern uint8_t    rtpSilentPeriod;
#define RTP_SILENT_ON     0x02
#define RTP_SILENT_OFF    0x00
/* Description
 *     used for profiling the audio loop
 */
struct CycleInfo {
  uint32_t time[7];
  uint32_t samples[2];

  CycleInfo() {
    memset(time, 0, sizeof(time));
    memset(samples, 0, sizeof(samples));
  }

  void show() {
    char buf[100];
    char* p = buf;
    int last = 0;
    for (int i=1; i<sizeof(time)/sizeof(time[0]); i++) {
      if (time[i]!=0) {
        p += sprintf(p, "%d ", time[i]-time[last]);
        last = i;
      } else {
        p += sprintf(p, "- ");
      }
    }
    p += sprintf(p, "/ ");
    for (int i=0; i<sizeof(samples)/sizeof(samples[0]); i++) {
      p += sprintf(p, "%d ", samples[i]);
    }
    log_d("%s", buf);
  }
};
typedef struct CycleInfo CycleInfo_t;


class Audio  {

public:
  Audio(bool stereoOut, int BCLK, int LRC, int DOUT, int DIN);
  ~Audio();

  // Configuring
  /* Install I2S as every consumer but music expects it: TX+RX, 4 x 1024, channel format from
   * monoOut. A no-op when that is what is installed. */
  void configureI2S();
  /* Uninstall the ring and install the DEFAULT one again, fresh, at the current settings, and
   * leave it stopped the way shutdown() leaves one. For a borrower that installed the ring while
   * its own large internal blocks sat in the heap (the Game Boy): called once those are freed,
   * so the heap places the ring again with them gone (0.9.79, see Audio.cpp). Refuses while the
   * device is on, and with no driver to move. True = the ring was reinstalled (and left stopped).
   * False with i2sReady() still true = refused; false with it now false = the install FAILED and
   * there is no driver until the next start() installs one. */
  bool reseatI2S();
  bool setSampleRate(int hz);         // TODO: which or these purely configuring, and which reset the configuration?
  bool setBitsPerSample(int bits);
  void setMonoOutput(bool mono);      // TODO: force Mono and not force mono

  // Actions
  void loop();
  void ceasePlayback();
  void report();
  bool start();
  void pause();
  void resume();
  bool shutdown();
  void setVolumes(int8_t speakerVol, int8_t headphonesVol, int8_t loudspeakerVol);
  void getVolumes(int8_t &speakerVol, int8_t &headphonesVol, int8_t &loudspeakerVol);
  /* The master mute (Settings > Mute all sounds, serial `mute`). Applied AT THE CODEC — the
   * DAC's soft-mute bit plus both output pairs at their minimum, and the loudspeaker
   * amplifier left off — whenever the LOUDSPEAKER is the route: the ring, the message chirp,
   * the mesh pop, music, the Game Boy. The earpiece (a call you answered) and headphones are
   * not muted: nobody else hears them. The vibrate motor is not sound and is not touched. The
   * volumes the apps set are kept underneath (getVolumes still answers them), so unmuting is
   * exact. {user}, 2026-09-21, at work: "a master 'mute' button within the settings. Just make
   * sure it also works for the Gameboy emulator". */
  void setMuted(bool muted);
  bool isMuted() const {
    return muted;
  }
  void setHeadphones(bool plugged);
  /* THE JACK, AND ONLY THE JACK (review 3, A5): the headphone-detect pin's reading, at boot and on
   * every edge (WiPhone.ino). Records it - `jackHeadphones` is written nowhere else - and routes
   * to it. restore() puts THIS back after a pop, not the flag the pop's snapshot saw: the pop
   * forces headphones off, and an unplug inside it changed nothing live, so the snapshot's `true`
   * came back and the next ring, track and call went to an empty jack until the next edge. */
  void jackSensed(bool plugged);
  bool getHeadphones(void);
  void chooseSpeaker(bool loudspeaker);
  bool isLoudspeaker() {
    return this->loudspeaker;
  }
  bool error() {
    return this->err != WM8750_ERROR_OK;
  }

  /* ── Music ──────────────────────────────────────────────────────────────────────
   * Plays an MP3 or a WAV from the card. Unlike playFile(), which loops forever
   * because it was written for a ringtone, this one ENDS — musicEnded() is what lets
   * a playlist advance.
   *
   * 🛑 0.9.79: ALWAYS MONO, AND AT HALF RATE FOR 44.1/48 kHz FILES, ON ITS OWN I2S RING
   * (music_feed.h has the whole story and the measurements). The jack no longer decides the
   * format — it only routes (setHeadphones() reconfigures the live codec). Until 0.9.79 the
   * jack decided stereo vs mono, and the MONO path played garbage from the first MP3 commit:
   * it told I2S the decoder's interleaved L,R,L,R... was 1152 mono samples, so the first half
   * of every 26 ms frame played an octave low and the second half never played. Every track
   * ever heard from the loudspeaker was that.
   *
   * ⚠ Do not call during a call. Music and RTP are both Playback modes and there is one
   * I2S peripheral; the caller stops music when a call arrives. Since 0.9.79 it REFUSES while
   * an RTP session is armed (musicError() "In a call") — see the note at its top. */
  bool playMusic(fs::FS *fs, const char* path, uint32_t startAt = 0);
  /* The file offset to resume at: two frames before the one PLAYING now. (It was the READ
   * position, which the feed now keeps up to 4 KB + half a second of ring ahead of the ear.)
   * Handed back to playMusic() as `startAt` to resume a paused track where it left off. */
  uint32_t musicFilePos();
  void stopMusic();
  bool musicPlaying() const {
    return this->playback == Playback::LocalMp3 || this->playback == Playback::LocalWav;
  }
  /* True once the file is finished AND its last sample has certainly played. */
  bool musicEnded() const {
    return this->musicPlaying() && this->feed && this->feed->ended();
  }
  const char* musicError() const {
    return this->musicProblem;
  }
  /* The feed's counters, for the Now Playing line, `audio` and `music` (serial). See the
   * note at the bottom of music_feed.h for what each one can and cannot claim. False when no
   * track has been opened since boot. */
  bool musicStats(MusicStats* out);
  void musicResetStats();
  void musicSetSwap(bool on);
  bool musicSwap() const {
    return this->feed ? this->feed->swapPairs : true;
  }
  /* Music stopped by something OTHER than the player (a pop, the ring, a call, shutdown()):
   * where it was and when, handed over ONCE. The player turns it into a pause, so F1 carries
   * on instead of restarting the song at 0:00 — which is what every mesh message used to cost. */
  bool musicTakeStopPlace(uint32_t* pos, uint32_t* stoppedMs);
  /* The codec's tone: bass boost, treble shelf, de-emphasis. See WM8750::setTone(). */
  void setCodecTone(uint8_t tone);
  uint8_t codecTone() const;

  bool playFile(fs::FS *fs, const char* path);
  bool playRecord();
  bool playRingtone(fs::FS *fs);
  /* Short notification sound (/pop.pcm).
   *
   * `vol` is the LOUDSPEAKER level in dB; it used to be hardwired to maximum, which is why
   * a mesh message announced itself at the same volume whatever the phone's settings said.
   * Settings > Notifications owns this now. MaxVolume keeps the old behaviour for any caller
   * that does not care. */
  bool playPop(fs::FS *fs, int8_t vol = Audio::MaxLoudspeakerVolume);
  /* The same sound from a buffer already in memory — pop_pcm[] in flash — with the SPIFFS
   * file as the fallback when `pcm` is null or empty. The file's open alone has been measured
   * at 1.2-1.6 s on this phone (docs/HANDOFF.md, "SPIFFS here is pathologically slow"), and
   * it happened INSIDE the notification with the motor already running: a chirp made the
   * buzz a second long, which was one of the two halves of "the vibrate length is very
   * inconsistent". A buffer plays ONCE and then pads with silence — see the LocalPcm branch
   * of loop() — so a late stop cannot replay it the way the looping file player does. */
  bool playPop(const uint8_t* pcm, size_t pcmLen, fs::FS *fallbackFs,
               int8_t vol = Audio::MaxLoudspeakerVolume);
  /* Why the last playPop() returned false, for the caller's log line; null after a success. */
  const char* popError() const {
    return this->popProblem;
  }
  bool popFromMemory() const {
    return this->pcmMem != nullptr;
  }
  /* After a successful playPop(): how long after it returned the first sample it wrote reaches
   * the DAC, for the caller's stop timer (notify_timing.h: notifyPopLeadMs(), notifyPopTimerMs()).
   * 🛑 One whole trip round the ring (512 ms) for the pop_pcm buffer (stop LATE), whatever state
   * the ring was in - each is bounded by one trip: the teardown used to zero the buffers at 360 ms,
   * 150 ms before the chirp could sound (review 3, A1). 0 for the SPIFFS fallback (`sourceLoops`,
   * stop EARLY): its open runs the first trip before the caller's stamp. popRingState() is for
   * the log. */
  uint32_t popLeadMs(bool sourceLoops) const;
  NotifyRing popRingState() const {
    return this->popRing;
  }
  bool rewind() {
    return this->playFile(this->playbackFS, this->playbackFilename.c_str());
  }

  // Actions related to RTP
  void newCall();
  void showAudioStats();

  enum : uint8_t {
    ULAW_RTP_PAYLOAD = 0,         // G.711, u-Law / PCMU
    ALAW_RTP_PAYLOAD = 8,         // G.711, A-Law / PCMA
    G722_RTP_PAYLOAD = 9          // G.722
  };
  uint16_t openRtpConnection(uint16_t rtpLocalPort);                         // the port that will be listened to AND from which RTP will be sent TODO: allows these two to be different
  bool playRtpStream(uint8_t payloadType, uint16_t rtpRemotePort = 0);       // remote port - play audio only from that port

  // Actions related to microphone
  // TODO: first open port, than feed that port to TinySIP for SDP
  // TODO: currently the mic configuration is the same as the playback configuration, which might be not desirable (at 48 kHz sample rate, especially)
  bool turnMicOn();      // turn on mic, calculate average intensity, but otherwise don't do anything with the data     TODO: check whether it needs to be called before start() and whether it's used properly
  bool sendRtpStreamFromMic(uint8_t payloadType, IPAddress rtpRemoteIP, uint16_t rtpRemotePort);
  bool recordFromMic();
  bool isRecordingFinished() {
    return this->recordFinished;
  }
  bool saveWavRecord(fs::FS *fs, const char* pathName);
  void ceaseRecording();
  void setMicAvg(uint32_t mic);
  uint32_t getMicAvg();

  /* Snapshot / put back the whole output configuration around a one-shot sound.
   * These were declared here with a "TODO" from the beginning and never implemented, which
   * is why every notification pop has been leaking device state. Implemented 2026-08-15. */
  void preserve();        // remember current configs to restore playback later
  void restore();         // restore preserved state
  /* Forget the snapshot without putting it back: the pop's teardown, when something else has
   * taken the device since (its configuration stands; restore() would pull it out from under
   * it, and a snapshot left held would be restored by the NEXT pop instead of its own).
   * ⚠ Except the jack's reading: the pop forced headphones off, and a sensor is not a setting
   * the new owner chose - a track that took over the pop follows the jack (review 3, A5). */
  void discardPreserved() {
    this->presValid = false;
    this->setHeadphones(this->jackHeadphones);
  }
  /* Every write of an output volume to the codec goes through here, so the mute cannot be
   * undone by a volume change from any app (and codec.setVolume() itself clears the DAC
   * mute bit, so the bit is re-asserted after each). */
  void applyVolume(int8_t loudspeakerVol, int8_t otherVol);
  bool muted = false;

  // Properties
  const char* getTitle() {
    return this->title.length()>0 ? this->title.c_str() : this->playbackBasename.length() ? this->playbackBasename.c_str() : "";
  }
  const char* getArtist() {
    return this->artist.length()>0 ? this->artist.c_str() : "";
  }

  uint32_t getFileSize();
  uint32_t getFilePos();
  bool isOn() {
    return this->audioOn;
  }
  /* Is an I2S driver installed? False only after a failed install (installI2S() frees the old ring
   * before it builds the new one). The Game Boy's quit line reads it to tell a reseat that failed
   * from one that had nothing to move. */
  bool i2sReady() const {
    return this->i2sInstalled;
  }
  /* Is the device actually MOVING SAMPLES right now?
   *
   * ⚠ DELIBERATELY NARROW, AND DELIBERATELY NOT A SESSION PREDICATE. This answers one
   * question — "is the hardware doing work" — and nothing else. It is the device half of the
   * audio idle watchdog in WiPhone.ino; the session half (a call up, the phone ringing, a
   * notification pop mid-play, the emulator holding the device) lives there, with the state
   * that knows about those things. Every consumer states what it needs.
   *
   * ⚠ microphoneOn / microphoneStreamOut are NOT consulted, on purpose. They are cleared in
   * exactly one place — shutdown() — so a call that ended without one leaves them latched
   * true forever (that is the hot-mic leak documented at the top of shutdown()). Consulting
   * them here would let the very fault the watchdog exists to clean up switch the watchdog
   * off. The SIP call state in the loop is the authority on whether a call is live. */
  bool movingSamples() const {
    /* ⚠ RECORDING COUNTS, and it is a SEPARATE FLAG. recordFromMic() (Audio.cpp:1571-1601)
     * allocates the buffer, calls turnOn()/turnMicOn() and sets microphoneRecord — it never
     * touches `playback`. Reading only `playback` therefore reported "not in use" throughout a
     * live recording, which let the idle watchdog shut the device down mid-record. Unlike
     * microphoneOn (see the note below) microphoneRecord is NOT sticky: ceaseRecording() and
     * saveWavRecord() both clear it. */
    return this->playback != Playback::Nothing || this->microphoneRecord;
  }
  /* Does the device hold an RTP SESSION — the microphone streaming out, or a call's incoming
   * stream playing? The question the orphan clock in WiPhone.ino asks (see rtp_watch.h).
   *
   * ⚠ A NEW PREDICATE ON PURPOSE, not an edit to movingSamples() above: that one deliberately
   * ignores the mic flags so a latched flag cannot switch the idle watchdog off. This one asks
   * exactly about those flags, because a latched send flag is the hazard it exists to catch.
   * microphoneOn alone is NOT a session — the mic-level meters set it with nothing going out;
   * the send gate in loop() is microphoneStreamOut. */
  bool rtpSessionArmed() const {
    return this->microphoneStreamOut || this->playback == Playback::RtpStream;
  }
  /* Read-only views for the serial `audio` line: is the mic being read, is it being SENT, and
   * to which port. After any call ends all three must read 0. */
  bool micOn() const {
    return this->microphoneOn;
  }
  bool micStreamOut() const {
    return this->microphoneStreamOut;
  }
  uint16_t rtpPeerPort() const {
    return this->rtpRemotePort;
  }
  bool isEof() {
    return this->playbackEof;
  }
  int  getBps() {
    return this->bps;
  };
  int packetSizeSamples(int duration);

  static const i2s_port_t i2s_num = I2S_NUM_0;

  // Volume range in the audio codec chip
  static const int8_t MaxVolume = 6;
  static const int8_t MuteVolume = -69;

  // Software limit for the loudspeaker (otherwise can burn)
  static const int8_t MaxLoudspeakerVolume = 0;

  // Profiling
  //LinearArray<CycleInfo_t, false> profile;

  bool playSampleChunk();

protected:
  bool turnOn();                            // enable the audio systems and main loop if not enabled already
  bool playFile();
  bool playPcm(const uint8_t* data, size_t len);   // LocalPcm from a buffer, once; see playPop()
  bool setDataChannels(int channels);
  bool setFilePos(uint32_t pos);
  bool playChunk();
  AUDIO_INLINE bool playSample();
  void codecReconfig();

  // Specific to MP3
  void readID3Metadata();
  int  decodeMp3Bytes(uint8_t *data, size_t len);

protected:

  // What to play in DAC (speaker & headphones)?
  enum class Playback { Nothing, RtpStream, LocalMp3, Record, LocalPcm, LocalWav };

  /* ── Music state ────────────────────────────────────────────────────────────────
   * The decoder and the feed are POINTERS, allocated on first use, and everything big they
   * own lives in PSRAM. This object is global; 4 KB of input buffer, 29 KB of decoder state
   * and ~11 KB of feed buffers sitting in it would take the internal heap the WiFi PHY needs.
   * See helix_memory.c. The feed object itself is placed in PSRAM too (Audio.cpp). */
  Mp3Stream*  mp3 = nullptr;
  MusicFeed*  feed = nullptr;
  bool        ensureFeed();
  /* Music's own I2S install: mono 16-bit, TX only, MUSIC_DMA_BUFS x MUSIC_DMA_BUF_SAMPLES, at
   * `sampleRate`. Only playMusic() asks for it; every other consumer goes through
   * configureI2S() (via the setters), which puts the default geometry back — so no consumer
   * can inherit music's ring, the bug class this singleton keeps producing. */
  void        configureMusicI2S(bool fresh);
  void        installI2S(bool music, bool fresh);
  MusicDma    musicDma() const;
  const char*  musicProblem = nullptr;
  /* Set by ceasePlayback() when it ends music that the player did not stop (see
   * musicTakeStopPlace()). stopMusic() — the player's own stop — clears it. */
  bool        musicStopValid = false;
  uint32_t    musicStopPos = 0;
  uint32_t    musicStopMs = 0;

  bool        audioOn = false;              // I2S and audio codec are turned ON
  bool        audioLoop = true;             // do the audio processing if audio is ON?
  bool        microphoneOn = false;         // TODO: configure I2S and audio codec based on this value (currently microphone is ON whenever audio is ON)
  Playback    playback;                     // what are we currently feeding to DAC?
  bool        microphoneStreamOut;          // do we send microphone data in RTP stream?
  bool        microphoneRecord;             // do we record microphone data to a local file?
  int16_t     sample[2];
  bool        headphones = false;           // if headphones are plugged in, need to send output only to headphones (not earspeaker and/or loudspeaker)
  bool        loudspeaker = false;           // which speaker to use: loudspeaker (true) or earspeaker (false)?
  int8_t      earpieceVol = 6;            // small speaker connected directly to the audio codec IC
  int8_t      headphonesVol = 6;
  int8_t      loudspeakerVol = 0;         // big speaker connected to the amplifier

  int         sampleRate;                   // how many samples per second
  uint8_t     bps = 16;                     // bitsPerSample
  uint8_t     dataChannels = 2;             // number of channels in the MP3 file; used by playChunk
  bool        monoOut = false;              // does I2S driver expect one (left only) or two channels (right and left)?

  /* What the I2S driver is CURRENTLY installed with, so configureI2S() can skip a reinstall
   * that would change nothing. See the note on it in Audio.cpp — each reinstall reallocates
   * ~16 KB of internal DMA memory, and internal RAM is what this phone runs out of. */
  bool        i2sInstalled = false;
  int         i2sRate = 0;                  // kept TRUE by setSampleRate() too (0.9.79)
  /* setSampleRate() restarted the DMA at buffer 0 under the driver's old free-buffer queue:
   * the next configureI2S()/configureMusicI2S() must reinstall even if everything matches. */
  bool        i2sQueueStale = false;
  uint8_t     i2sBps = 0;
  bool        i2sMono = false;
  /* The ring's geometry and direction (0.9.79): music installs its own (see
   * configureMusicI2S()), so the cache must know which one is in. i2sGen changes on every
   * install and every rate change — the music feed's lead is unknown across one. */
  uint16_t    i2sBufs = 0;
  uint16_t    i2sLen = 0;
  bool        i2sRx = false;
  uint32_t    i2sGen = 0;

  /* Snapshot taken by preserve() and put back by restore(). See the comment on those in
   * Audio.cpp: a one-shot sound reconfigures the whole device and used to leave it that way. */
  bool        presValid = false;
  int         presSampleRate = 0;
  uint8_t     presBps = 16;
  uint8_t     presDataChannels = 2;
  bool        presMonoOut = false;
  /* No presHeadphones (review 3, A5): the jack is a sensor, not a setting to snapshot. */
  bool        jackHeadphones = false;       // the pin's last reading: written by jackSensed() ONLY
  bool        presLoudspeaker = false;
  int8_t      presEarpieceVol = 0;
  int8_t      presHeadphonesVol = 0;
  int8_t      presLoudspeakerVol = 0;

  // Local playback file
  fs::FS*     playbackFS;                   // filesystem
  String      playbackFilename="";          // full path of the playback file in the filesystem
  String      playbackBasename="";          // basename (shor filename)
  File        playbackFile;                 // MP3 file
  bool        playbackEof = false;

  /* Memory-backed LocalPcm source (playPcm). Null = the LocalPcm branch reads playbackFile
   * as it always did. Cleared by ceasePlayback(), which every playback start goes through,
   * so the ringtone — LocalPcm from a file — can never be handed the pop's bytes. Nothing is
   * allocated: the buffer is the caller's and the chunks go through playDec. */
  const uint8_t* pcmMem = nullptr;
  size_t      pcmMemLen = 0;
  size_t      pcmMemPos = 0;
  const char* popProblem = nullptr;         // playPop()'s reason for a false, see popError()
  NotifyRing  popRing = NOTIFY_RING_RUNNING;   // how the last playPop() found the ring, see popLeadMs()

  String      artist;
  String      title;

  // Record buffer (PCM)
  uint16_t*   recordRaw = NULL;             // temporary buffer in PSRAM where the audio data is being stored
  size_t      recordRawSizeSamples;
  int         recordRawR;
  int         recordRawW;
  bool        recordFinished;

  // Play buffers: encoded and decoded (PCM)
  uint8_t     playEnc[1600];                // undecoded audio (MP3) / receiving buffer for UDP packets
  uint16_t    playEncR=0;                   // read index
  uint16_t    playEncW=0;                   // write index

  int16_t     playDec[2400];                // decoded audio (PCM): 1-channel: mono (max. 2400 samples);  2-channel: interleaved L/R (maximum 1152 frames, 2*1200 = 2400 samples)
  // NOTE: this is sufficient for 150 ms of 16000 Hz mono audio (e.g. decoded G.722)
  uint16_t    playDecFramesLeft = 0;
  uint16_t    playDecCurFrame;
  bool        playDecEvenSample = 1;        // if true, sample is swapped with the next in mono playback

  // Mic buffers: raw (PCM) and encoded
  uint16_t    micRaw[2049];
  uint16_t    micRawW;
  uint16_t    micRawR;                      // micRawR < micRawW, if equal -> empty
  uint8_t     micEnc[1600];

  uint32_t    micAvg[4];
  uint16_t    micAvgNext = 0;

  bool        calcMicIntensity;             // Do we need to calculate microphone average input?

  // Specific to MP3
  int         id3Size=0;                    // length id3 tag
  int         nextSync=0;
  int         bytesLeft=0;
  int         bitrate=0;                    // TODO: what is this?
  uint8_t     rev=0;                        // revision
  bool        f_podcast = false;            // set if found ID3Header in stream
  bool        f_extHead = false;            // ID3 extended header
  bool        f_mp3 = false;                // indicates mp3
  bool        mp3Playing = false;           // valid mp3 stream recognized
  uint32_t    lastRate;                     // TODO: what is this?

  // Incoming RTP audio stream
  WiFiUDP     rtp;
  IPAddress   rtpRemoteIP;
  uint16_t    rtpRemotePort = 0;
  uint8_t     rtpPayloadType;
  RTPacket    rtpSend;                      // this one is initialized with parameters from
  RTPacket    rtpRecv;
  bool        firstPacket;                  // is the next incoming packet will the first in audio stream?
  uint16_t    lastSequenceNum;              // last RTP sequence num
  //uint32_t    pos;                          // position in playback     TODO
  uint16_t    rtpPort;
  uint16_t    rtcpPort;
  uint16_t    voipPacketSize;

  // Call quality of service (QoS)
  uint32_t    rtcpPacketsReceived;
  uint32_t    packetsReceived;              // total UDP packets received during all
  uint32_t    packetsGood;                  // audio packets count that have no issues
  uint32_t    packetsWrongPayload;          // audio format does not match negotiated one
  uint32_t    packetsMissed;                // packets not played (either completely missing or out of order)
  uint32_t    packetsUnord;                 // packets out of order arriving now (temporary)

  uint32_t    packetsSent;                  // total UDP packets attempted to send
  uint32_t    packetsSendingFailed;         // total packets failed to send

  // Codecs
  G722_DEC_CTX* g722Decoder;
  G722_ENC_CTX* g722Encoder;

  // Debug
  uint32_t    loopCnt = 0;
  uint32_t    runCnt = 0;
  uint32_t    rtpCnt = 0;
  int         sampleX = 0;

  static const uint16_t audio_sample[];
  static const uint16_t VOIP_PACKET_DURATION_MS = 20;     // maximum anticipated packet duration (we always leave this length in the output buffer in anticipation of such packet)
  static const uint32_t PACKET_PCM_WSIZE_8KHZ = 160;      // number of samples for 20ms PCM 16-bit/8kHz, 1-chanel
  static const uint32_t PACKET_PCM_WSIZE_16KHZ = 320;     // number of samples for 20ms PCM 16-bit/16kHz, 1-chanel
  static const uint32_t RECORDING_SIZE_SAMPLES = 1<<20;   // 1 MB

  // Power masks
  static const uint16_t POWER_ALL = 0;
  static const uint16_t DAC_HEADPHONES  = WM8750_POWER2_DAC | WM8750_POWER2_OUT1;
  static const uint16_t DAC_EARSPEAKER  = WM8750_POWER2_DAC | WM8750_POWER2_OUT3 | WM8750_POWER2_LOUT1;
  static const uint16_t DAC_LOUDSPEAKER = WM8750_POWER2_DAC | WM8750_POWER2_OUT2;

  enum : int { APLL_AUTO = -1, APLL_ENABLE = 1, APLL_DISABLE = 0 };

  wm8750_err_t err;

  float m_amplitude;
  float m_frequency;
  float m_phase;
  float m_time;
  float m_deltaTime;
};

#endif /* __AUDIO_H_ */
