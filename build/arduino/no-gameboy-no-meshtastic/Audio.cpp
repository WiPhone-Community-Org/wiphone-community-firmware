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
 *
 *  MP3 decoding logic borrowed from Wolle (schreibfaul1).
 *  Source: https://github.com/schreibfaul1/ESP32-audioI2S
 *  It was later licenced under GPL-3.0.
 */

// TODO:
// - use i2s_write for entire batches instead of "playSample()", introduce an additional interleaving output buffer for that
// - force mono (for enforced mono in MP3 player), otherwise - allow monoOut to be set according to dataChannels

#include "Audio.h"
#include "config.h"
#include "rtp_watch.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <new>

//#define TAG "audio" // log tags don't seem to work in Arduino

uint8_t    rtpSilentPeriod = 0x0;
/* The far end's silence, per call. Started fresh by newCall(); see rtp_watch.h for why the old
 * pair (a boot-long `rtpSilentScan` here, a boot-long counter in WiPhone.ino) ended the second
 * call of every boot at connect. */
static RtpSilence s_rtpSilence = {0, 0};

AUDIO_CODEC_CLASS  codec(AUDIO_CODEC_I2C_ADDR, I2C_SDA_PIN, I2C_SCK_PIN);

const uint16_t Audio::audio_sample[] = {
  // change every 32 bytes (500 Hz sound for 16000 Hz mono)
  //0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101,
  // change every 16 bytes (1000 Hz sound for 16000 Hz mono), means period of 16 samples, 16000 sample rate / 16 = 1000 KHz
  // 128 samples here
  0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101,
  //0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x80FF, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00, 0x7F00,
  // change every 8 bytes (2000 Hz sound for 16000 Hz mono)
  //0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0x0101, 0x0101,
  // change every 4 bytes (4000 Hz sound for 16000 Hz mono)
  //0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101, 0xFEFE, 0xFEFE, 0x0101, 0x0101,
};

Audio::Audio(bool stereoOut, int BCLK, int LRC, int DOUT, int DIN) : playbackFS(&SPIFFS) {

  log_d("Audio::Audio: %d", ESP.getFreeHeap());
  // Initialize variables
  this->audioOn = false;
  this->audioLoop = false;
  this->playback = Playback::Nothing;
  this->microphoneStreamOut = false;
  this->microphoneRecord = false;

  // Configure I2S interface
  this->bps = 16;
  this->sampleRate = 16000;
  this->monoOut = !stereoOut;
  this->dataChannels = this->monoOut ? 1 : 2;     // provisional
  this->voipPacketSize = this->packetSizeSamples(VOIP_PACKET_DURATION_MS);
  log_d("Audio::Audio: voip %d", ESP.getFreeHeap());
  this->configureI2S();

  log_d("Audio::Audio: i2s %d", ESP.getFreeHeap());

  // Set pinout
  i2s_pin_config_t pins = {
    .bck_io_num   = BCLK,
    .ws_io_num    = LRC,              //  wclk,
    .data_out_num = DOUT,
    .data_in_num  = DIN
  };

  i2s_set_pin((i2s_port_t) i2s_num, &pins);

  log_d("Audio::Audio: pins %d", ESP.getFreeHeap());

  // Initialize audio codec
  err = codec.powerUp(stereoOut, 32000, POWER_ALL, AUDIO_MCLK_CRYSTAL_KHZ);
  codec.shutDown();

  log_d("Audio::Audio: codec %d", ESP.getFreeHeap());

  // Populate sequence ID, SSRC and timestamp
  rtpSend.newSession(true);

  log_d("Audio::Audio: rtp %d", ESP.getFreeHeap());

  // G.722 decoder & encoder
  g722Decoder = g722_decoder_new(64000, 0);       // TODO: check if it doesn't take a lot of memory (otherwise initialize only when needed)
  g722Encoder = g722_encoder_new(64000, 0);

  log_d("Audio::Audio: end %d", ESP.getFreeHeap());
}

/* Description:
 *     configures I2S according to the internal values:
 *       - this->bps
 *       - this->sampleRate
 *       - this->monoOut
 *     in the DEFAULT geometry (TX+RX, 4 x 1024). Music's own is configureMusicI2S().
 */
void Audio::configureI2S() {
  this->installI2S(false, false);
}

/* Music's own ring. See music_feed.h for the sizes and the measurements behind them.
 *
 * 🛑 `fresh` FORCES A REINSTALL, and playMusic() asks for one whenever the ring was STOPPED
 * (the idle watchdog's shutdown() -> i2s_stop()) or the rate is changing. IDF 3.3's i2s_start()
 * — which i2s_set_sample_rates() also runs — restarts the DMA at buffer 0 but keeps the driver's
 * free-buffer queue as it was, so the writer fills buffers in an order the DMA no longer plays
 * them in: the first trip round the ring comes out SCRAMBLED. With 4 x 1024 that was up to
 * ~90 ms; with music's 24 buffers it would be half a second of jumbled audio at a resume. A
 * fresh install has an empty queue and a DMA at buffer 0, which agree. Same size, freed and
 * reallocated back to back, so it lands in the blocks it just left. */
void Audio::configureMusicI2S(bool fresh) {
  this->installI2S(true, fresh);
}

/* 🛑 WHERE THE RING SITS IS DECIDED WHEN IT IS INSTALLED, AND A GAME INSTALLS IT OVER ITS OWN RAM
 * (0.9.79). IDF 3.3's heap is BEST FIT: every DMA buffer (4 KB in the default ring: 4 TX + 4 RX)
 * goes into the SMALLEST free block that holds it, at that block's low end, and multi_heap never
 * moves it after. The Game Boy's startGame() allocates its task stacks, VRAM and audio buffer
 * (32 KB) at the bottom of the phone's one big free block BEFORE its setMonoOutput() reinstalls
 * this ring. On phone 1 (the boot ring before the game) what was left of the big block, 30,896 B,
 * was then SMALLER than the 32,860 B hole the old ring left, so best fit put 7 of the 8 new
 * buffers right on top of the emulator's blocks and the 8th in the old hole (in-game largest
 * 28,740 = that hole less one buffer, to the byte). When the game ends the emulator's blocks free
 * BELOW the ring and the big block stays cut in two for the rest of the boot: largest 63,716 ->
 * 32,816 on phone 1 (exactly the four emulator blocks, 32,768 + 4 x 12 B of heap poisoning) and
 * 64,152 -> 28,732 on phone 2, with free back where it was. Proven by music's own install on
 * phone 2 moving the ring away: largest 28,732 -> 64,316, so nothing else was left in the block.
 *
 * Reinstalled here once the borrower's blocks are gone, best fit sees the heap the way it was
 * before the game: the old ring's hole (the smaller block) takes the buffers again and the big
 * block is whole. ⚠ Through installI2S(), never a bare uninstall: its cache must stay true (see
 * the note there). It FREES before it installs, so free RAM never dips below where it started
 * and the new buffers always have a home (the eight they just left, at least).
 *
 * Stopped after, if the device is off: i2s_driver_install() starts the DMA (i2s_set_clk ends in
 * i2s_start), and the ring it replaces was stopped by shutdown(). A stopped fresh ring has an
 * empty queue and the DMA at buffer 0, which is what the next start() expects.
 * ⚠ Only with the device OFF: a powered codec losing its clocks under it clicks.
 * ⚠ A FAILED install here leaves NO driver (the old ring was freed first): false, with i2sReady()
 * now false, which the caller's log line tells apart from a refusal. Not retried here - the heap
 * that just refused would refuse again; start() installs one on the next use, or refuses. */
bool Audio::reseatI2S() {
  if (this->audioOn || !this->i2sInstalled) {
    return false;
  }
  this->installI2S(false, true);            // DEFAULT geometry, fresh: uninstall, then install
  if (this->i2sInstalled) {
    i2s_stop(i2s_num);                      // as shutdown() left the ring it replaces
  }
  return this->i2sInstalled;
}

void Audio::installI2S(bool music, bool fresh) {
  /* ── DO NOT REINSTALL THE DRIVER TO CHANGE NOTHING ────────────────────────────────────
   * The TODO that used to sit here ("does it create pop noise? if so - reduce number of
   * calls") was asking for this, and there is a second, larger reason to do it.
   *
   * Every call uninstalls and reinstalls the driver, which frees and reallocates its DMA
   * ring in INTERNAL, DMA-capable RAM: dma_buf_count(4) x dma_buf_len(1024, which IDF caps at
   * 1023 in stereo) x 2ch x 2B for TX, and the same again for RX — ~32 KB in stereo, ~16 KB in
   * mono. Internal contiguous RAM is the resource whose exhaustion panics this phone, and
   * callers arrive in clusters: starting a track sets rate, channels and mono in sequence, so
   * one play could churn it several times over. Measured on hardware: starting music dropped
   * the largest free block 15,648 -> 8,200, and the MusicApp object itself accounted for
   * only 224 of that.
   *
   * So: install only when the driver is not installed, or when something it depends on has
   * actually changed. Callers can keep calling this as "make I2S match my settings" — which
   * is what every one of them means — without paying for a teardown each time.
   *
   * 🛑 0.9.79: THERE ARE TWO GEOMETRIES NOW, AND ONLY MUSIC ASKS FOR ITS OWN. Music installs
   * mono, TX only, MUSIC_DMA_BUFS x MUSIC_DMA_BUF_SAMPLES (24 KB; see music_feed.h). Every other
   * path — configureI2S(), which every setter calls — asks for the DEFAULT one, so a pop, a
   * ring, a call or the Game Boy that follows a track gets a reinstall back to it instead of
   * inheriting music's ring. (The Game Boy is paced by the DMA draining; a call reads the
   * microphone, and music's install has no RX.) turnMicOn() calls configureI2S() for the same
   * reason: the mic-level meters never call a setter.
   *
   * ⚠ Safe to cache because NOTHING ELSE installs or uninstalls the driver: start() uses
   * i2s_start() and shutdown() uses i2s_stop(), neither of which uninstalls. Verified by
   * grep over the whole tree. If that ever stops being true, this cache goes stale and the
   * symptom is silence, so re-check it before adding an uninstall anywhere else. */
  const uint16_t bufs = music ? MUSIC_DMA_BUFS : 4;
  const uint16_t len  = music ? MUSIC_DMA_BUF_SAMPLES : 1024;
  const bool     rx   = !music;
  const bool     mono = music ? true : this->monoOut;
  const uint8_t  bps  = music ? 16 : this->bps;
  if (!fresh &&
      !this->i2sQueueStale &&             // see setSampleRate(): the DMA was restarted under it
      this->i2sInstalled &&
      this->i2sRate == this->sampleRate &&
      this->i2sBps  == bps &&
      this->i2sMono == mono &&
      this->i2sBufs == bufs &&
      this->i2sLen  == len &&
      this->i2sRx   == rx) {
    return;
  }

  if (this->i2sInstalled) {
    i2s_driver_uninstall(i2s_num);
    this->i2sInstalled = false;
    /* ⚠ THE GEOMETRY GOES WITH THE DRIVER (review 3, A2). If the install below fails there is no
     * ring, and a cache still saying "RX, 4 x 1024" let the loop's mic block i2s_read() a NULL
     * driver (LedMicApp after a game or a call). Set again only by a successful install. */
    this->i2sRx = false;
    this->i2sBufs = 0;
    this->i2sLen = 0;
  }
  i2s_config_t i2s_config = {
    .mode = static_cast<i2s_mode_t> (rx ? (I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX)
                                        : (I2S_MODE_MASTER | I2S_MODE_TX)),
    .sample_rate = this->sampleRate,
    .bits_per_sample = (bps == 16 ? I2S_BITS_PER_SAMPLE_16BIT : I2S_BITS_PER_SAMPLE_8BIT ),
    .channel_format = (mono ? I2S_CHANNEL_FMT_ONLY_LEFT : I2S_CHANNEL_FMT_RIGHT_LEFT),
    .communication_format = static_cast<i2s_comm_format_t> (I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, // high interrupt priority
    .dma_buf_count = bufs,
    .dma_buf_len = len,
    .use_apll=APLL_ENABLE,
    .tx_desc_auto_clear=true,  // new in V1.0.1
    .fixed_mclk=-1
  };
  log_d("Audio::Audio: before driver install %d", ESP.getFreeHeap());
  if (i2s_driver_install((i2s_port_t)i2s_num, &i2s_config, 0, NULL) == ESP_OK) {
    this->i2sInstalled = true;
    this->i2sRate = this->sampleRate;
    this->i2sBps  = bps;
    this->i2sMono = mono;
    this->i2sBufs = bufs;
    this->i2sLen  = len;
    this->i2sRx   = rx;
    this->i2sQueueStale = false;          // a fresh ring: empty queue, DMA at buffer 0 — they agree
    this->i2sGen++;
    /* 🛑 A RING INSTALLED WITH THE DEVICE OFF IS LEFT STOPPED (review 3, A6), the way shutdown()
     * leaves one. i2s_driver_install() STARTS the DMA (i2s_set_clk ends in i2s_start), and
     * several installs happen with the device off: the constructor's at boot, a pop's restore()
     * after something shut the device down inside the pop (hanging up while the far end rang,
     * closing a mic app), the Game Boy's reseat. Each left a clocked ring - APLL, I2S and ~16-31
     * end-of-buffer interrupts a second - running with audioOn false, which the idle watchdog
     * cannot see (it zeroes its clock whenever !isOn()) and nothing stopped until the next sound.
     * A stopped fresh ring has an empty queue and the DMA at buffer 0: what start() expects.
     * ⚠ Only with the device OFF - a powered codec losing its clocks clicks; with it on, the new
     * ring must run (the caller is mid-sound). reseatI2S() still stops its own (harmless twice). */
    if (!this->audioOn) {
      i2s_stop(i2s_num);
    }
  }
  log_d("Audio::Audio: after driver install %d", ESP.getFreeHeap());
  this->report();
}

/* The installed ring, for the music feed's lead. IDF caps one DMA buffer at 4092 bytes, so a
 * stereo 1024 is really 1023 — the feed must use what the driver built, not what was asked. */
MusicDma Audio::musicDma() const {
  MusicDma d;
  const uint32_t frameBytes = (this->i2sMono ? 1u : 2u) * (this->i2sBps == 16 ? 2u : 1u);
  uint32_t len = this->i2sLen;
  if (len * frameBytes > 4092) {
    len = 4092 / frameBytes;
  }
  d.bufs = this->i2sBufs;
  d.len = (uint16_t)len;
  d.rate = (uint32_t)(this->i2sRate > 0 ? this->i2sRate : 1);
  d.gen = this->i2sGen;
  return d;
}

void Audio::report() {
  log_d("Audio configs:");
  log_d(" - SR:   %d", this->sampleRate);
  log_d(" - bps:  %d", this->bps);
  log_d(" - ch:   %d", this->dataChannels);
  log_d(" - mono: %d", (int) this->monoOut);
  log_d(" - headphones: %d", (int) this->headphones);
  log_d(" - speaker: %d", (int) this->loudspeaker);
}

bool Audio::start() {
  bool succ = true;

  /* 🛑 NO DRIVER, NO START (0.9.79, review of the Game Boy's reseat). IDF 3.3's i2s_start()
   * dereferences p_i2s_obj[0] with no NULL check (checked in libdriver.a), and a FAILED install
   * leaves no driver: installI2S() uninstalls the old ring before it builds the new one, and
   * i2s_driver_install() uninstalls itself when its DMA allocation fails. Any install can fail that
   * way - music's, a pop's, the Game Boy's reseat at quit - and playMusic() calls turnOn() BEFORE
   * it installs its own ring, so the next track after a failed install was a LoadProhibited panic.
   * Put the default back once (the heap may have moved since) and refuse, device off, if even
   * that fails. A no-op whenever a driver is in, which is every other time. */
  if (!this->i2sInstalled) {
    this->configureI2S();
    if (!this->i2sInstalled) {
      log_e("AUDIO: no I2S driver and the install failed (internal free %u, largest %u) - not starting",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      return false;
    }
  }

  // Turn on the audio codec IC
  log_v("turning ON audio codec");
  // TODO: feed result into succ
  uint16_t powerMask = this->headphones ? DAC_HEADPHONES : (this->loudspeaker ? DAC_LOUDSPEAKER : DAC_EARSPEAKER);
  codec.powerUp(!this->monoOut, 32000, powerMask, AUDIO_MCLK_CRYSTAL_KHZ);
  applyVolume(MuteVolume, MuteVolume);     // mute: avoid sudden pop

  // Turn on amplifier (separate IC) if needed — never while the master mute is on
#ifdef WIPHONE_INTEGRATED
  if (!this->headphones && this->loudspeaker && !this->muted) {
    log_v("turning ON amplifier");
    amplifierEnable(4);
  }
  bool amped = true;
#endif

  // Turn on I2S peripheral
  log_v("turning ON I2S");
  if (i2s_start(i2s_num)!=ESP_OK) {
    succ = false;
  }

  // Turn on the volume
  applyVolume(this->loudspeakerVol, this->headphones ? this->headphonesVol : this->earpieceVol);

  this->audioOn = succ;

  return succ && amped;
}

void Audio::setHeadphones(bool plugged) {
  if (this->headphones != plugged) {
    this->headphones = plugged;
    if (this->audioOn) {
      this->codecReconfig();
    }
  }
}

bool Audio::getHeadphones(void) {
  return this->headphones;
}

void Audio::chooseSpeaker(bool loudspeaker) {
  if (this->loudspeaker != loudspeaker) {
    this->loudspeaker = loudspeaker;
    if (this->audioOn) {
      this->codecReconfig();
    }
  }
}

void Audio::codecReconfig() {
  // Turn off the audio codec IC
  log_v("turning audio codec OFF");
  codec.mute();         // to minimize pop noise
  codec.shutDown();     // TODO: feed the result into succ

  log_v("turning audio codec ON");
  uint16_t powerMask = this->headphones ? DAC_HEADPHONES : (this->loudspeaker ? DAC_LOUDSPEAKER : DAC_EARSPEAKER);
  codec.powerUp(!this->monoOut, 32000, powerMask, AUDIO_MCLK_CRYSTAL_KHZ);
  applyVolume(MuteVolume, MuteVolume);     // mute: avoid sudden pop

  // Switch amplifier (separate IC) if needed — never on while the master mute is on
#ifdef WIPHONE_INTEGRATED
  if (!this->headphones && this->loudspeaker && !this->muted) {
    log_v("turning amplifier ON");
    amplifierEnable(4);
  } else {
    log_v("turning amplifier OFF");
    amplifierEnable(0);
  }
#endif

  // Turn on the volume
  applyVolume(this->loudspeakerVol, this->headphones ? this->headphonesVol : this->earpieceVol);
}

/* The mute silences THE LOUDSPEAKER ROUTE: the ring, the message chirp, the mesh pop, music
 * and the Game Boy all play through it, and it is the one output the room hears. A call
 * answered to the earpiece, or anything on headphones, plays as normal — nobody else can
 * hear those, and a muted phone that could not take a call would be a phone left unmuted.
 * (In-call "Loud Spkr" while muted is silent, like everything else on that route.) */
void Audio::applyVolume(int8_t loudspeakerVol, int8_t otherVol) {
  if (this->muted && !this->headphones && this->loudspeaker) {
    /* Outputs at their floor AND the DAC soft-muted: codec.setVolume() clears the mute bit
     * as it goes (its "Unmute DAC" line), so the bit is written back after it. */
    codec.setVolume(MuteVolume, MuteVolume);
    codec.mute();
    return;
  }
  codec.setVolume(loudspeakerVol, otherVol);
}

void Audio::setMuted(bool m) {
  if (this->muted == m) {
    return;
  }
  this->muted = m;
  log_e("AUDIO: master mute %s", m ? "ON" : "off");
  if (!this->audioOn) {
    return;                                // start() applies it when the device next comes up
  }
  /* Live: the codec path first, then the amplifier — muting turns the amp off before the
   * DAC could pop; unmuting brings the DAC up before the amp hears it. */
  if (m) {
#ifdef WIPHONE_INTEGRATED
    amplifierEnable(0);
#endif
    applyVolume(this->loudspeakerVol, this->headphones ? this->headphonesVol : this->earpieceVol);
  } else {
    applyVolume(this->loudspeakerVol, this->headphones ? this->headphonesVol : this->earpieceVol);
#ifdef WIPHONE_INTEGRATED
    if (!this->headphones && this->loudspeaker) {
      amplifierEnable(4);
    }
#endif
  }
}

void Audio::pause() {
  // Stop processing audio buffers (the main audio loop)
  this->audioLoop = false;

  // Clear only immediate audio playback (DMA) buffer to stop the sound
  if (this->i2sInstalled) {
    i2s_zero_dma_buffer((i2s_port_t)i2s_num);
  }
}

void Audio::resume() {
  this->audioLoop = true;
}

bool Audio::shutdown() {
  bool succ = true;

  // Turn off the audio codec IC
  codec.mute();         // to minimize pop noise
  codec.shutDown();     // TODO: feed the result into succ

  // Clear the buffers, close the file
  this->ceaseRecording();
  this->ceasePlayback();

  /* 🛑 END THE RTP SESSION HERE, not just the audio device.
   *
   * Every path that ends a call comes through shutdown(): local hang-up, remote BYE, WiFi lost
   * mid-call, and the RTP-silence timeout (see WiPhone.ino). Until 2026-08-16 not one of them
   * cleared any of the state below. `microphoneOn` was set true by turnMicOn() and **never set
   * false anywhere in the codebase**; `microphoneStreamOut` was cleared only in the
   * constructor; `rtpRemotePort`/`rtpRemoteIP` kept the last caller's address indefinitely.
   *
   * The send gate in the audio loop is `microphoneOn && bps==16`, then
   * `microphoneStreamOut && rtpRemotePort` — and nothing else. `audioOn` merely decides whether
   * the loop runs at all. So the next thing to bring audio back up resumed streaming the
   * MICROPHONE to whoever called last, and a mesh notification pop is enough to do it. With a
   * public DID on this phone that is a hot mic aimed at the last stranger who dialled.
   *
   * Clearing here is safe because setup always re-arms explicitly — openRtpConnection(),
   * sendRtpStreamFromMic() and playRtpStream() are always issued together.
   *
   * The socket is closed as well, and that part is not optional: with rtpRemotePort back to 0
   * the receive filter ("from the expected port, OR no port set at all") would accept RTP from
   * anywhere on the network. */
  this->microphoneOn = false;
  this->microphoneStreamOut = false;
  this->rtpRemotePort = 0;
  this->rtpRemoteIP = IPAddress();
  this->rtp.stop();

  // Tun off the amp
  //if (!allDigitalWrite(AMPLIFIER_SHUTDOWN, LOW)) succ = false;
  amplifierEnable(0);

  // Turn off I2S peripheral
  /* ⚠ Only with a driver: IDF 3.3's i2s_stop() dereferences p_i2s_obj[0] with no NULL check,
   * inside its critical section, and after a failed install there is none (see start()). */
  if (this->i2sInstalled) {
    if (i2s_stop(i2s_num)!=ESP_OK) {
      succ = false;
    }
  }

  this->audioOn = false;

  return succ;
}

Audio::~Audio() {
  this->shutdown();
}

bool Audio::turnOn() {
  /* 🛑 NO DRIVER, NO WRITER - WHETHER OR NOT THE DEVICE IS ON (review 3, A2). fd2c54f put this
   * check in start(), but start() runs only when audioOn is false, and a reinstall can fail with
   * the device ON: a pop, the ring or a call's setters swap music's ring for the default one
   * under a playing track (installI2S() frees the old ring first). turnOn() then said yes, the
   * caller armed its source, and the next audio->loop() pump's i2s_write() dereferenced IDF 3.3's
   * NULL p_i2s_obj[0] - a LoadProhibited panic (an incoming call over music rebooted the phone).
   * One reinstall (the heap may have moved), then refuse. With the device on, the new ring runs
   * (i2s_driver_install() starts it); off, start() below starts it. */
  if (!this->i2sInstalled) {
    this->configureI2S();
    if (!this->i2sInstalled) {
      log_e("AUDIO: no I2S driver and the install failed (internal free %u, largest %u) - refused",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      return false;
    }
  }

  // Start audio systems
  if (!this->audioOn && !this->start()) {
    return false;
  }

  // Let the audio processing run
  this->audioLoop = true;

  return true;
}

void Audio::setVolumes(int8_t earpieceVol, int8_t headphonesVol, int8_t loudspeakerVol) {
  if (earpieceVol > MaxVolume) {
    earpieceVol = MaxVolume;
  }
  if (earpieceVol < MuteVolume) {
    earpieceVol = MuteVolume;
  }
  if (headphonesVol > MaxVolume) {
    headphonesVol = MaxVolume;
  }
  if (headphonesVol < MuteVolume) {
    headphonesVol = MuteVolume;
  }
  if (loudspeakerVol > MaxLoudspeakerVolume) {
    loudspeakerVol = MaxLoudspeakerVolume;
  }
  if (loudspeakerVol < MuteVolume) {
    loudspeakerVol = MuteVolume;
  }
  this->earpieceVol = earpieceVol;
  this->headphonesVol = headphonesVol;
  this->loudspeakerVol = loudspeakerVol;
  applyVolume(loudspeakerVol, this->headphones ? headphonesVol : earpieceVol);
}

void Audio::getVolumes(int8_t &speakerVol, int8_t &headphonesVol, int8_t &loudspeakerVol) {
  speakerVol = this->earpieceVol;
  headphonesVol = this->headphonesVol;
  loudspeakerVol = this->loudspeakerVol;
}

bool Audio::playFile(fs::FS *fs, const char* path) {
  this->ceasePlayback();
  this->playbackFS = fs;
  this->title = "";
  this->artist = "";
  this->playbackFilename = path;
  if(!this->playbackFilename.startsWith("/")) {
    this->playbackFilename="/"+this->playbackFilename;
  }
  this->playbackBasename = this->playbackFilename.substring(this->playbackFilename.lastIndexOf('/') + 1, this->playbackFilename.length());
  return this->playFile();
}

bool Audio::playRecord() {
  if (this->recordRawW == 0) {
    return false;
  }
  this->ceasePlayback();
  this->playback = Playback::Record;
  this->recordRawR = 0;
  this->setDataChannels(1);
  return true;
}

bool Audio::playFile() {
  log_d("Reading file: %s", this->playbackFilename.c_str());
  this->playbackFile = this->playbackFS->open(this->playbackFilename.c_str());
  if (!this->playbackFile) {
    log_d("Failed to open file for reading");
    return false;
  }

  // Start the audio systems (if not started)
  if (!this->turnOn()) {
    return false;
  }

  uint16_t i=0, s=0;

  // Reset buffers
  this->playEncW=0;
  this->playEncR=0;
  this->playDecFramesLeft = 0;
  memset(this->playDec, 0, sizeof(this->playDec));      // not necessary

  this->playback = Playback::LocalPcm;
  this->playbackEof = false;

  return true;
}

/* ── Where the music feed gets its bytes and puts its samples ────────────────────────────
 * The feed (music_feed.cpp) is Arduino-free so the host suite can drive it against a model
 * of the DMA; these two adapters are all it knows of the card and of I2S. File-static: two
 * vtable pointers and a File*, in internal RAM. */
class AudioMusicFile : public MusicSource {
public:
  File* f = nullptr;
  fs::FS* fs = nullptr;
  const String* path = nullptr;       // Audio::playbackFilename: only playMusic()/playFile() move it
  int read(uint8_t* dst, size_t n) override {
    return f ? (int)f->read(dst, n) : -1;
  }
  bool seek(uint32_t pos) override {
    return f && f->seek(pos);
  }
  uint32_t size() override {
    return f ? (uint32_t)f->size() : 0;
  }
  /* After a failed read (MUSIC_READ_TRIES in music_feed.h): FatFs latches a disk error on the
   * FIL, so every later read on this handle fails too. A new handle is the only retry. */
  bool reopen(uint32_t pos) override {
    if (!f || !fs || !path) {
      return false;
    }
    f->close();
    *f = fs->open(path->c_str());
    return *f && f->seek(pos);
  }
};

class AudioMusicI2s : public MusicSink {
public:
  /* ⚠ TIMEOUT 0, ALWAYS. The loop task must never wait for the DMA: a short count is the
   * feed's "the ring is full" and it simply comes back next pass. */
  size_t write(const int16_t* s, size_t n) override {
    size_t w = 0;
    i2s_write(Audio::i2s_num, (const char*)s, n * sizeof(int16_t), &w, 0);
    return w / sizeof(int16_t);
  }
  uint64_t nowUs() override {
    return (uint64_t)esp_timer_get_time();
  }
};

static AudioMusicFile s_musicFile;
static AudioMusicI2s  s_musicI2s;

/* The feed object and its buffers, in PSRAM, once. Kept between tracks for the same reason
 * the decoder is: allocating and freeing per track is how PSRAM gets fragmented. */
bool Audio::ensureFeed() {
  if (!this->feed) {
    void* mem = heap_caps_malloc(sizeof(MusicFeed), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) {
      return false;
    }
    this->feed = new (mem) MusicFeed();
  }
  return this->feed->begin();
}

/* Open an MP3 or WAV, decode its first frames, and give it music's own I2S ring.
 *
 * ⚠ The format is decided by the CONTENT, not the extension. The uploader has no
 * extension filter, so a .wav that is really an MP3 is an ordinary thing to meet. */
bool Audio::playMusic(fs::FS *fs, const char* path, uint32_t startAt) {
  /* 🛑 NEVER OVER A CALL. Everything below replaces `playback` (RtpStream -> LocalMp3/Wav) and
   * reinstalls I2S at the track's rate and channel count. Under a live call that was two faults
   * at once, and F1 or F2 from any screen reached it (the transport keys had no call guard):
   *   - the caller went silent for the rest of the call — playback no longer RtpStream, and
   *     nothing ever sets it back;
   *   - the microphone kept sending, but packetSizeSamples() reads the SHARED sampleRate and
   *     dataChannels, so 20 ms became 882 samples at 44.1 kHz (garble at the far end) or 1764
   *     with headphones in (over micEnc's 1600 bytes, so every packet was dropped: silence).
   * The keys are gated in WiPhone.ino now; this is the device refusing on its own behalf, so a
   * caller nobody has written yet cannot do it either. musicError() says why, on the screen. */
  if (this->playback == Playback::RtpStream || this->microphoneStreamOut) {
    this->musicProblem = "In a call";
    return false;
  }
  this->stopMusic();
  this->musicProblem = NULL;
  this->musicStopValid = false;

  if (!fs || !path) {
    this->musicProblem = "No file";
    return false;
  }
  if (!this->ensureFeed()) {
    this->musicProblem = "No memory for music";
    return false;
  }
  this->playbackFS = fs;
  this->playbackFilename = path;
  this->playbackFile = fs->open(path);
  if (!this->playbackFile) {
    this->musicProblem = "Cannot open";
    return false;
  }

  /* One kilobyte covers an ID3 header and every WAV header ffmpeg, sox or QuickTime
   * emits. playEnc is reused as scratch: it is 1600 bytes, already exists, and is
   * literally described as the undecoded-audio buffer. */
  uint8_t* hdr = (uint8_t*)this->playEnc;
  const size_t want = 1024;
  int got = this->playbackFile.read(hdr, want);
  if (got < 12) {
    this->playbackFile.close();
    this->musicProblem = "File too short";
    return false;
  }

  s_musicFile.f = &this->playbackFile;
  s_musicFile.fs = fs;
  s_musicFile.path = &this->playbackFilename;
  WavInfo wav;
  bool opened = false;
  Playback kind = Playback::Nothing;
  if (wavParseHeader(hdr, (size_t)got, &wav)) {
    opened = this->feed->openWav(&s_musicFile, wav, this->playDec, startAt);
    kind = Playback::LocalWav;
  } else {
    if (!this->mp3) {
      this->mp3 = new Mp3Stream();
    }
    if (!this->mp3 || !this->mp3->begin()) {
      this->playbackFile.close();
      this->musicProblem = "No memory for MP3";
      return false;
    }
    /* Skip the ID3 tag (helix would otherwise hunt for a sync word inside album art), or go
     * straight back to a paused place. The feed resets helix, steps over the frame whose bit
     * reservoir is missing, drops the overlap transient after it, and believes a format only
     * when two frames in a row agree on it — see MusicFeed::openMp3(). */
    const uint32_t audioStart = mp3Id3v2Size(hdr, (size_t)got);
    opened = this->feed->openMp3(&s_musicFile, this->mp3, this->playDec, audioStart, startAt);
    kind = Playback::LocalMp3;
  }
  if (!opened) {
    this->playbackFile.close();
    this->musicProblem = this->feed->problem() ? this->feed->problem() : "Not playable audio";
    return false;
  }
  this->playback = kind;

  /* Was the ring RUNNING? If the idle watchdog stopped it, turnOn()'s i2s_start() restarts the
   * DMA at buffer 0 under a stale free-queue — see configureMusicI2S(). */
  const bool ringWasRunning = this->audioOn;
  if (!this->turnOn()) {
    this->playbackFile.close();
    this->playback = Playback::Nothing;
    this->feed->stop();
    this->musicProblem = "Audio would not start";
    return false;
  }

  /* Music's own ring, mono, at the feed's output rate (half the file's for 44.1/48 kHz).
   * Every output parameter is SET here, not inherited — the rule for every consumer of this
   * singleton. codec.setAudioPath(false) routes the left channel to both outputs, which is
   * what setMonoOutput(true) has always done alongside the I2S format. */
  this->bps = 16;
  this->dataChannels = 1;
  this->monoOut = true;
  this->sampleRate = (int)this->feed->outRate();
  this->voipPacketSize = this->packetSizeSamples(VOIP_PACKET_DURATION_MS);
  this->configureMusicI2S(!ringWasRunning);  // a rate change reinstalls anyway (the cache)
  /* 🛑 DID IT INSTALL? installI2S() uninstalls first, and music's ring is 24 x 1 KB of DMA-capable
   * INTERNAL RAM plus 24 descriptors — 8 KB more than the mono default a pop or a call leaves.
   * If i2s_driver_install() failed there is NO driver, and IDF 3.3's i2s_zero_dma_buffer() and
   * i2s_write() dereference p_i2s_obj[0] with no NULL check: a LoadProhibited panic, not a track
   * that will not play (review, 2026-09-25). Refuse, and try to put the default back. */
  if (!this->i2sInstalled || this->i2sBufs != MUSIC_DMA_BUFS) {
    this->playbackFile.close();
    this->feed->stop();
    this->playback = Playback::Nothing;
    this->musicProblem = "No memory for music";
    log_e("MUSIC: I2S install failed (internal free %u, largest %u) - not playing",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    this->configureI2S();
    return false;
  }
  codec.setAudioPath(false);
  /* Clean silence, and the ring's write position back on a sample PAIR (a per-sample mono
   * write — the ring, the pop — can leave it on an odd sample, and the mono pair swap
   * below it would then be inverted for the whole track). i2s_zero_dma_buffer() pads to 4
   * bytes; see IDF 3.3 driver/i2s.c. */
  i2s_zero_dma_buffer(i2s_num);
  this->playDecFramesLeft = 0;               // music never goes through playChunk()

  /* The prefill: decode until the ring refuses. The "loading" pause {user} allowed. */
  this->feed->start(&s_musicI2s, this->musicDma());
  return true;
}

uint32_t Audio::musicFilePos() {
  if (this->musicPlaying() && this->feed) {
    return this->feed->playingPos((uint64_t)esp_timer_get_time());
  }
  return this->playbackFile ? (uint32_t)this->playbackFile.position() : 0;
}

bool Audio::musicStats(MusicStats* out) {
  if (!this->feed || !out) {
    return false;
  }
  this->feed->stats(out, (uint64_t)esp_timer_get_time());
  return true;
}

void Audio::musicResetStats() {
  if (this->feed) {
    this->feed->resetStats((uint64_t)esp_timer_get_time());
  }
}

void Audio::musicSetSwap(bool on) {
  if (this->ensureFeed()) {
    this->feed->swapPairs = on;       // from the next frame decoded
  }
}

bool Audio::musicTakeStopPlace(uint32_t* pos, uint32_t* stoppedMs) {
  if (!this->musicStopValid) {
    return false;
  }
  this->musicStopValid = false;
  if (pos) {
    *pos = this->musicStopPos;
  }
  if (stoppedMs) {
    *stoppedMs = this->musicStopMs;
  }
  return true;
}

void Audio::setCodecTone(uint8_t tone) {
#if AUDIO_CODEC == AUDIO_CODEC_WM8750
  codec.setTone(tone);
  if (this->audioOn) {
    this->codecReconfig();            // powerUp() writes the tone; mute, route and levels follow
  }
#else
  (void)tone;
#endif
}

uint8_t Audio::codecTone() const {
#if AUDIO_CODEC == AUDIO_CODEC_WM8750
  return codec.tone();
#else
  return 0;
#endif
}

void Audio::stopMusic() {
  if (this->musicPlaying()) {
    if (this->playbackFile) {
      this->playbackFile.close();
    }
    this->ceasePlayback();
  }
  /* The player stopped it itself: it knows the place (musicFilePos() before this call), so
   * this is not a "stopped by something else" for musicTakeStopPlace(). */
  this->musicStopValid = false;
  if (this->mp3) {
    this->mp3->reset();     // keep the 29 KB; re-allocating per track fragments PSRAM
  }
}

void Audio::ceasePlayback() {
  if (this->playback == Playback::LocalMp3 || this->playback == Playback::LocalWav) {
    /* Music ended by whoever called this — a pop, the ring, a call, shutdown(). Keep where it
     * was, so the player can make it a pause (musicTakeStopPlace()). Before the file closes:
     * the feed computes the place from its own frame log, not from the file. */
    if (this->feed && this->feed->active()) {
      this->musicStopPos = this->feed->playingPos((uint64_t)esp_timer_get_time());
      this->musicStopMs = millis();
      this->musicStopValid = true;
    }
    /* 🛑 closeRing() WRITES I2S (its zeros go through the sink's i2s_write()), so ONLY WITH A
     * DRIVER. A reinstall can fail under a playing track and leave none - the Recorder's setters
     * after F1 resumed music inside it, with the internal heap exhausted - and IDF 3.3's
     * i2s_write() dereferences p_i2s_obj[0] with no NULL check: the next F1, or any shutdown(),
     * pa{user}ed here (integration review 3, R3-4 verification). With no driver there is no
     * current buffer to top up either: the next install starts with none held. The place above
     * is still recorded, and the feed still stops. */
    if (this->feed) {
      if (this->i2sInstalled) {
        this->feed->closeRing();            // leave IDF's current buffer full: see closeRing()
      }
      this->feed->stop();
    }
    /* WAV too: until 0.9.79 only an MP3's file was closed here, so a pop over a WAV left it
     * open until the next track's open() replaced the handle. */
    playbackFile.close();
  }
  this->playback = Playback::Nothing;
  this->pcmMem = nullptr;                   // the next LocalPcm reads its FILE unless told otherwise
  /* The memory pop latches playbackEof when it runs out, and nothing else cleared it: a
   * ringtone whose open then FAILED read isEof() true every pass and called rewind() — a
   * SPIFFS open per pass — while resetting the vibro state machine so the motor never
   * buzzed (review, 2026-09-19). A source that is gone has no end to report. */
  this->playbackEof = false;
  if (this->i2sInstalled) {                 // no driver after a failed install: IDF would crash
    i2s_zero_dma_buffer((i2s_port_t)i2s_num);
  }
  memset(this->playDec, 0, sizeof(this->playDec));
  this->playDecFramesLeft = 0;
  this->playEncW = 0;
}

/* Description:
 *     push out decoded samples from `playDec` into I2S DMA buffer
 * Return:
 *     false if could not push out a sample at some point
 */
bool Audio::playChunk() {
//    // Play at most 20ms worth of samples here
//    uint32_t packet = this->sampleRate / 50;
//    uint32_t cutoff = this->playDecFramesLeft > packet ? this->playDecFramesLeft - packet : 0;
  const uint32_t cutoff = 0;
  if (this->monoOut) {
    if (this->dataChannels==1) {
      // Should have been simple case: direct copying, but swapping the neighboring samples (ESP32 bug workaround)
      while (this->playDecFramesLeft > cutoff) {
        if (this->playDecEvenSample) {
          if (this->playDecFramesLeft > 1) {
            this->sample[0] = this->playDec[this->playDecCurFrame + 1];
          } else {
            this->sample[0] = this->playDec[this->playDecCurFrame];
          }
        } else {
          if (this->playDecCurFrame > 0) {
            this->sample[0] = this->playDec[this->playDecCurFrame - 1];
          } else {
            this->sample[0] = this->playDec[this->playDecCurFrame];
          }
        }
        if (!this->playSample()) {
          return false;
        }
        this->playDecFramesLeft--;
        this->playDecCurFrame++;
        this->playDecEvenSample = !this->playDecEvenSample;
      }
    } else if (this->dataChannels==2) {
      // Complex case: average of two channels (TODO: is this a correct way to mux two channels?)
      while (this->playDecFramesLeft > cutoff) {
        this->sample[0] = (this->playDec[this->playDecCurFrame * 2] >> 1) + (this->playDec[this->playDecCurFrame * 2 + 1] >> 1);
        if (!this->playSample()) {
          return false;
        }
        this->playDecFramesLeft--;
        this->playDecCurFrame++;
      }
    }
  } else {
    if (this->dataChannels==1) {
      // Complex case: duplication of a single channel (inefficient and should never happen - if there is only one channel, need to switch to mono output)
      while (this->playDecFramesLeft > cutoff) {
        this->sample[0] = this->sample[1] = this->playDec[this->playDecCurFrame];
        if (!this->playSample()) {
          return false;
        }
        this->playDecFramesLeft--;
        this->playDecCurFrame++;
      }
    } else if (this->dataChannels==2) {
      // Simple case: direct copying      TODO: maybe could use i2s_write directly
      while (this->playDecFramesLeft > cutoff) {
        this->sample[0] = this->playDec[this->playDecCurFrame * 2];
        this->sample[1] = this->playDec[this->playDecCurFrame * 2 + 1];
        if (!this->playSample()) {
          return false;
        }
        this->playDecFramesLeft--;
        this->playDecCurFrame++;
      }
    }
  }
  return true;
}

/* Description:
 *     same as playChunk, but plays sample
 *     (that is tries to fill output DMA buffer with audio_sample)
 */
bool Audio::playSampleChunk() {
  uint32_t i = (uint32_t) -1;
  uint32_t cnt = 0;
  if (this->monoOut) {
    if (this->dataChannels==1) {
      while (++cnt) {
        this->sample[0] = audio_sample[ i = (++i<sizeof(audio_sample)/sizeof(audio_sample[0]) ? i : 0) ];
        if (!this->playSample()) {
          goto ret;
        }
      }
    } else if (this->dataChannels==2) {
      while (++cnt) {
        // Simplification
        this->sample[0] = audio_sample[ i = (++i<sizeof(audio_sample)/sizeof(audio_sample[0]) ? i : 0) ];
        if (!this->playSample()) {
          goto ret;
        }
      }
    }
  } else {
    if (this->dataChannels==1) {
      while (++cnt) {
        this->sample[0] = this->sample[1] = audio_sample[ i = (++i<sizeof(audio_sample)/sizeof(audio_sample[0]) ? i : 0) ];
        if (!this->playSample()) {
          goto ret;
        }
      }
    } else if (this->dataChannels==2) {
      while (++cnt) {
        this->sample[0] = audio_sample[ i = (++i<sizeof(audio_sample)/sizeof(audio_sample[0]) ? i : 0) ];
        this->sample[1] = audio_sample[ i = (++i<sizeof(audio_sample)/sizeof(audio_sample[0]) ? i : 0) ];
        if (!this->playSample()) {
          goto ret;
        }
      }
    }
  }
ret:
  log_d("samples written: %u", --cnt);
  return cnt > 0;
}

bool Audio::playRingtone(fs::FS *fs) {
  this->ceasePlayback();
  this->playback = Playback::LocalPcm;
  this->setDataChannels(1);
  this->setSampleRate(8000);      // the RATE FIRST, the bits last: see playPop()
  this->setMonoOutput(true);
  this->setBitsPerSample(16);

  /* 🛑 NO DRIVER, NO RING (review 3, A2). startRingtone() has already start()ed the device on
   * whatever ring was in - music's, when a call comes in over a track - and the setters above
   * swap it out (freeing it first). If that install failed, the first ring pass wrote a NULL
   * driver: an incoming call rebooted the phone. The motor still rings. */
  if (!this->i2sInstalled) {
    log_e("AUDIO: ringtone refused - no I2S driver (the install failed)");
    this->playback = Playback::Nothing;
    return false;
  }
  if (!this->turnOn()) {
    this->playback = Playback::Nothing;
    return false;
  }
  return this->playFile(fs, "/ringtone.pcm");
}

// Short one-shot notification sound (raw 16-bit mono 8kHz PCM). Forced out the
// loudspeaker at max level so it's audible as a notification regardless of the
// headphone-detect state; the sample amplitude controls how loud the pop is.
/* ── SNAPSHOT AND PUT BACK THE OUTPUT CONFIGURATION ───────────────────────────────────
 * Declared in Audio.h from the beginning with a bare "TODO" and never implemented — which
 * is why one-shot sounds have been permanently reconfiguring the device.
 *
 * playPop() sets SIX parameters (channels, bits, rate, mono, headphones, speaker, volumes)
 * and the pop teardown in WiPhone.ino only called ceasePlayback(), restoring none of them.
 * So a single Meshtastic notification left the phone at 8 kHz, mono, loudspeaker-forced and
 * at maximum volume, for good. Two measured consequences:
 *   - music afterwards played MONO OUT OF THE LOUDSPEAKER with headphones plugged in,
 *     because music_player's wantStereo() read audio->getHeadphones() (gone in 0.9.79:
 *     music is always mono now, and only the route follows the jack);
 *   - monoOut = true is exactly what paces the Game Boy at 50% (it is clocked by a blocking
 *     i2s_write, so a mono sink drains at half rate — see app_gbc.cpp). A mesh message was a
 *     far more frequent trigger for that than the music player ever was.
 *
 * ⚠ configureI2S() UNINSTALLS AND REINSTALLS THE I2S DRIVER, which reallocates ~16 KB of
 * internal DMA buffers. Internal heap is this phone's scarcest resource and fragmenting it is
 * what causes the reset_reason=4 panics, so restore() does that AT MOST ONCE and only when
 * something actually differs. Putting the values back through the individual setters would
 * have cost several reinstalls per notification — worse than the bug being fixed.
 *
 * ⚠ `playback` is deliberately NOT preserved. ceasePlayback() closes the playing file, so
 * restoring the enum would point the decoder at a dead handle. A pop still stops the current
 * track; that is a separate issue and is NOT fixed here. */
void Audio::preserve() {
  if (this->presValid) {
    return;         // already holding a snapshot — do not overwrite it with the pop's own state
  }
  this->presSampleRate     = this->sampleRate;
  this->presBps            = this->bps;
  this->presDataChannels   = this->dataChannels;
  this->presMonoOut        = this->monoOut;
  this->presLoudspeaker    = this->loudspeaker;
  this->presEarpieceVol    = this->earpieceVol;
  this->presHeadphonesVol  = this->headphonesVol;
  this->presLoudspeakerVol = this->loudspeakerVol;
  this->presValid = true;
}

void Audio::restore() {
  if (!this->presValid) {
    return;
  }
  this->presValid = false;

  const bool monoChanged = (this->monoOut != this->presMonoOut);
  const bool needI2S = monoChanged ||
                       (this->sampleRate != this->presSampleRate) ||
                       (this->bps        != this->presBps);

  this->sampleRate   = this->presSampleRate;
  this->bps          = this->presBps;
  this->dataChannels = this->presDataChannels;
  this->monoOut      = this->presMonoOut;

  if (needI2S) {
    this->configureI2S();                       // exactly one reinstall, and only if needed
  }
  if (monoChanged) {
    codec.setAudioPath(!this->monoOut);         // mirrors what setMonoOutput() would have done
  }
  /* Speaker routing before volume: setVolumes() picks earpiece-vs-headphones off
   * this->headphones, so it has to see the restored value. Both setters below already
   * no-op when unchanged.
   * 🛑 THE JACK AS IT IS NOW, NOT AS THE SNAPSHOT SAW IT (review 3, A5). The pin is read only on
   * its edge (WiPhone.ino), and playPop() forces headphones off: an unplug inside the pop wrote
   * false over false - no change - and the snapshot's `true` came back here, with nothing to read
   * the pin again until the next edge. The ring then went to the empty jack (start() builds
   * DAC_HEADPHONES whenever `headphones` is set - motor only, a missed call), and so did music
   * and the next call's far end. Plugging in inside a pop did the reverse. */
  this->setHeadphones(this->jackHeadphones);
  this->chooseSpeaker(this->presLoudspeaker);
  this->setVolumes(this->presEarpieceVol, this->presHeadphonesVol, this->presLoudspeakerVol);
}

void Audio::jackSensed(bool plugged) {
  this->jackHeadphones = plugged;
  this->setHeadphones(plugged);
}

uint32_t Audio::popLeadMs(bool sourceLoops) const {
  if (!this->i2sInstalled) {
    return 0;
  }
  const MusicDma d = this->musicDma();       // the ring as installed, IDF's per-buffer cap applied
  return notifyPopLeadMs(this->popRing, d.bufs, d.len, d.rate, sourceLoops);
}

bool Audio::playPop(fs::FS *fs, int8_t vol) {
  return this->playPop(nullptr, 0, fs, vol);
}

bool Audio::playPop(const uint8_t* pcm, size_t pcmLen, fs::FS *fs, int8_t vol) {
  this->popProblem = nullptr;
  /* For the lead's log line (review 3, A1; notify_timing.h): did this pop install the ring, restart
   * a stopped one, or find one running? i2sGen moves on every install and every rate change - and a
   * rate change always reinstalls below (it marks the queue stale). The memory source's lead no
   * longer depends on it (every state is bounded by one trip, and it takes the trip), but the
   * bench reads which case ran from here. */
  const uint32_t gen0 = this->i2sGen;
  const bool wasOn = this->audioOn;
  this->preserve();          // ⚠ BEFORE anything below changes it
  this->ceasePlayback();
  this->playback = Playback::LocalPcm;
  this->setDataChannels(1);
  /* ⚠ THE RATE FIRST, THE BITS LAST (0.9.79 review). A rate change marks the ring's queue stale,
   * so setMonoOutput()'s configureI2S() reinstalls — ONCE, at 8 kHz mono, onto a clean ring —
   * and setBitsPerSample() then matches. The old order (bits, rate, mono) reinstalled at the OLD
   * rate straight after a music session (music's own ring had to go) and again for the new one:
   * ~16 KB of internal DMA memory freed and taken again for nothing, on the phone's most
   * frequent sound. Every order ends in the same configuration. */
  this->setSampleRate(8000);
  this->setMonoOutput(true);
  this->setBitsPerSample(16);
  /* 🛑 NO DRIVER, NO POP (review 3, A2): the setters above free the old ring before they install
   * the pop's, twice over (setMonoOutput(), then setBitsPerSample()'s retry). Over a playing track
   * the device is still ON, so turnOn() would have said yes and the notify pump's first
   * i2s_write() dereferenced a NULL driver. (turnOn() refuses too now; this names the reason.) */
  if (!this->i2sInstalled) {
    this->popProblem = "no I2S driver (the install failed)";
    this->playback = Playback::Nothing;
    this->restore();
    return false;
  }
  this->setHeadphones(false);                       // force speaker path (not headphones)
  this->chooseSpeaker(true);                        // loudspeaker (not the tiny earpiece)
  /* ⚠ The loudspeaker level is the caller's, not a constant. This line used to force
   * maximum on every notification regardless of any setting, which is exactly why a mesh
   * chirp was as loud as the phone could make it. preserve()/restore() around this call
   * still puts the previous six parameters back — see Audio::preserve(). */
  if (vol > Audio::MaxLoudspeakerVolume) {
    vol = Audio::MaxLoudspeakerVolume;
  }
  this->setVolumes(Audio::MaxVolume, Audio::MaxVolume, vol);

  if (!this->turnOn()) {
    this->popProblem = "turnOn failed (codec/I2S start)";
    this->playback = Playback::Nothing;
    this->restore();      // nothing will call restore() for us if the pop never starts
    return false;
  }
  this->popRing = (this->i2sGen != gen0) ? NOTIFY_RING_FRESH
                  : (!wasOn ? NOTIFY_RING_RESTARTED : NOTIFY_RING_RUNNING);
  /* The buffer first; the file only when there is no buffer. Not "if the buffer fails":
   * playPcm() cannot fail once turnOn() has succeeded, and a fallback that could run after
   * a partial start would be exactly the kind of silent second path this codebase keeps
   * finding. */
  if (pcm && pcmLen >= 2) {
    if (this->playPcm(pcm, pcmLen)) {
      return true;
    }
    this->popProblem = "playPcm refused after turnOn - should be impossible";
    this->restore();
    return false;
  }
  if (!fs) {
    this->popProblem = "no buffer and no filesystem";
    this->restore();
    return false;
  }
  if (!this->playFile(fs, "/pop.pcm")) {
    this->popProblem = "open /pop.pcm failed";    // turnOn() already succeeded, so it is the open
    this->restore();      // ditto: the caller only arms the teardown when we return true
    return false;
  }
  return true;
}

/* A LocalPcm source that is a buffer instead of a file: the same decode loop, fed by memcpy.
 * Plays ONCE. When the buffer runs out the LocalPcm branch of loop() pads with zeros instead
 * of wrapping, because the file player's wrap is what queues a second attack into a 512 ms
 * DMA that the caller's stop timer then has to beat — see notify_timing.h. */
bool Audio::playPcm(const uint8_t* data, size_t len) {
  if (!data || len < 2) {
    return false;
  }
  this->ceasePlayback();    // clears pcmMem too; set it back below, AFTER the buffers are reset
  if (!this->turnOn()) {
    return false;
  }
  this->playEncW = 0;
  this->playEncR = 0;
  this->playDecFramesLeft = 0;
  memset(this->playDec, 0, sizeof(this->playDec));
  this->pcmMem = data;
  this->pcmMemLen = len;
  this->pcmMemPos = 0;
  this->playback = Playback::LocalPcm;
  this->playbackEof = false;
  return true;
}

/* Description:
 *     decode chunk of audio and play it
 */
void Audio::loop() {
//  this->loopCnt++;    // remove
//  if (this->loopCnt % 1000 == 0) {
//    log_d("%d / run=%d / rtp=%d", this->loopCnt, this->runCnt, this->rtpCnt);
//  }
  /* ⚠ AND NOTHING WITHOUT A DRIVER (review 3, A2). Every branch below reaches IDF 3.3's i2s_write()
   * or i2s_read(), which dereference p_i2s_obj[0] with no NULL check (checked in libdriver.a),
   * and a failed reinstall leaves the device ON with no driver: a restore() at a pop's end, a
   * pop or the ring swapping music's ring out. The music feed had its own guard; LocalPcm,
   * RtpStream, Record and the microphone did not. */
  if (!this->audioLoop || !this->audioOn || !this->i2sInstalled) {
    return;  // don't do anything if audio systems (I2S peripheral and audio codec) are turned off
  }
//  this->runCnt++;   // remove

  // PLAY PART: play readily available decoded data

//  // Profiling
//  CycleInfo_t info;
//  info.time[0] = micros();

  //uint32_t oldSmp = this->playDecFramesLeft;
  if (this->playDecFramesLeft>0) {
    this->playChunk();
  }
  //info.time[1] = micros();
  //info.samples[0] = oldSmp - this->playDecFramesLeft;

  // MICROPHONE PART: process/encode/send microphone data

  /* ⚠ i2sRx: music's install (0.9.79) has no RX side, and i2s_read() on it fails with an IDF
   * error line on every pass. microphoneOn is latched until shutdown(), so it can still be true
   * from a recording when a track starts. Every mic user goes through turnMicOn(), which puts
   * the RX-capable install back first. */
  if (this->microphoneOn && this->bps==16 && this->i2sRx) {      // TODO: different bits-per-sample are not supported for simplicity

    // Ensure the microphone data starts at the beginning
    if (this->micRawR > 0) {
      memmove(this->micRaw, this->micRaw + this->micRawR, sizeof(this->micRaw[0]) * (this->micRawW - this->micRawR));
      this->micRawW -= this->micRawR;
      this->micRawR = 0;
    }
    //info.time[2] = micros();

    // Read microphone data
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(i2s_num,  (char*) (this->micRaw + this->micRawW),  sizeof(this->micRaw) - this->micRawW * sizeof(this->micRaw[0]),  &bytesRead,  0);
    //info.time[3] = micros();
    if (err == ESP_OK) {

      // BPS == 16 is assumed below

      // Swap neighboring samples (ESP32 bug, see here: https://esp32.com/viewtopic.php?t=11023)
      uint16_t* start = this->micRaw + ((this->micRawW / 2) * 2);                       // first sample pair
      const uint16_t samplesRead = bytesRead/2;
      uint16_t* p = this->micRaw + (((this->micRawW + samplesRead) / 2) * 2);           // past last sample pair
      while (p > start) {
        p -= 2;
        uint16_t x = *(p+1);
        *(p+1) = *p;
        *p = x;
      }

      this->micRawW += bytesRead / 2;           // bps = 16 assumed

      size_t packetSizeWords = this->packetSizeSamples(20);       // 20 ms packet
      if (this->micRawW >= packetSizeWords) {
        // At least 20 ms of microphone data collected

        // Calculate microphone input intensity
        if (this->calcMicIntensity) {             // avoid doing it during the call to save a bit of compute power
          uint32_t micSum = 0;
          if (this->bps==16) {
            for (int j = 0; j < packetSizeWords; j++) {
              micSum += abs((int16_t) this->micRaw[j]);
            }
            this->setMicAvg(micSum / packetSizeWords);
          } else if (this->bps==8) {
            // ...should never happen (8-bit not fully implemented yet)
            for (int j = packetSizeWords*2; j > 0;) {
              uint32_t temp =  *((int8_t*)this->micRaw + --j); // temp is necessary due to some weirdness in the Arduino abs() implementation. See: https://www.arduino.cc/reference/en/language/functions/math/abs/
              temp = abs(temp);
              micSum += temp  << 8;
              //micSum += abs( *((int8_t*)this->micRaw + --j) ) << 8;

            }
            this->setMicAvg(micSum / packetSizeWords / 2);
          }
        }

        // Output the microphone data: send via network and/or save to a file

        if (this->microphoneStreamOut && rtpRemotePort) {

//          // DEBUG: replace all the microphone data with audio sample
//          this->micRawW -= bytesRead / 2;
//          for (int j=0; j<bytesRead / 2; j++) {
//            this->micRaw[this->micRawW++] = audio_sample[sampleX++];
//            if (sampleX >= sizeof(audio_sample)/sizeof(audio_sample[0])) sampleX = 0;
//          }

          // Compress PCM to G.722 (640 bytes to 160 bytes) or to G.711 (320 bytes to 160 bytes)
          int bytes = 0;

          /* ⚠ micEnc is a FIXED 1600-byte buffer and the G.711 compressors take a SAMPLE COUNT,
           * not a destination length — they write one byte per sample and cannot be told to
           * stop. packetSizeWords comes from packetSizeSamples(20), which scales with the
           * NEGOTIATED sample rate, so the only thing that has ever kept this in bounds is that
           * rate staying sane: 8 kHz gives 160 bytes, 16 kHz gives 320, and the arithmetic just
           * keeps going from there. Drop the packet instead of running off the end of the
           * buffer. micRawR still advances below, so the pipeline does not stall — we lose 20 ms
           * of audio rather than corrupting whatever follows micEnc in memory.
           * (G.722 emits half a byte per sample, so this bound covers it too.) */
          if (packetSizeWords > sizeof(this->micEnc)) {
            log_e("mic packet too large for micEnc: %u samples > %u bytes - dropped",
                  (unsigned) packetSizeWords, (unsigned) sizeof(this->micEnc));
          } else if (rtpPayloadType == Audio::G722_RTP_PAYLOAD) {
            bytes = g722_encode(g722Encoder, (const int16_t*) this->micRaw, packetSizeWords, (uint8_t*) this->micEnc);
          } else if (rtpPayloadType == Audio::ALAW_RTP_PAYLOAD) {
            alaw_compress(packetSizeWords, (const int16_t*) this->micRaw, (uint8_t*) this->micEnc);
            bytes = packetSizeWords;
          } else if (rtpPayloadType == Audio::ULAW_RTP_PAYLOAD) {
            ulaw_compress(packetSizeWords, (const int16_t*) this->micRaw, (uint8_t*) this->micEnc);
            bytes = packetSizeWords;
          }

          if (bytes > 0) {
            // Create RTP packet

            RTPacketHeader *rtpHeader = rtpSend.generateHeader(bytes);

            // Send RTP packet
            rtp.beginPacket(rtpRemoteIP, rtpRemotePort);
            rtp.write((uint8_t*)rtpHeader, sizeof(RTPacketHeader));
            rtp.write(this->micEnc, bytes);         // TODO: this unnecesarily (and rather slowly) copies the buffer

            // TODO: leave 12 bytes in the head of micEnc free for the RTP header, implement and use udp.writeFast()
            if (!rtp.endPacket()) {
              this->packetsSendingFailed++;
            }
            this->packetsSent++;

          } else {
            log_d("enc fail");
          }

        }

        if (this->microphoneRecord && !this->recordFinished) {
//          // DEBUG: drop every other sample and send that way
//          uint16_t dummy[packetSizeWords/2];
//          for (int j=packetSizeWords/2; j>0;) {
//            j-=2;
//            dummy[j] = this->micRaw[j*2];
//          }
//          if (this->recordFile) {
//            this->recordFile.write((const uint8_t*) this->micRaw, packetSizeWords * 2);
//            log_d("w %d", packetSizeWords * 2);
//            this->recordFile.write((const uint8_t*) dummy, packetSizeWords);    // DEBUG
//            log_d("b %d", packetSizeWords);
//          }

          // Record raw audio to file
//          if (this->recordFile) {
//            this->recordFile.write((const uint8_t*) this->micRaw, packetSizeWords * 2);
//            //log_d("w %d", packetSizeWords * 2);
//          }

          // Copy audio to recording buffer
          if (this->recordRawW + packetSizeWords <= this->recordRawSizeSamples) {
            memcpy(this->recordRaw + this->recordRawW, this->micRaw, packetSizeWords * 2);
            this->recordRawW += packetSizeWords;
          } else {
            this->recordFinished = true;
          }
        }

        // Discard microphone data
        this->micRawR = packetSizeWords;
      }
    }
  }

  // DECODING PART: decode current audio stream and place data into the output buffer

  //info.time[4] = micros();

  if (this->playback == Playback::LocalPcm) {
    if (this->playDecFramesLeft == 0) {
      static int pcm_offset = 0;
      int res;

      if (this->pcmMem) {
        /* Memory source (playPcm): play the buffer through once, then silence. The file
         * branch below wraps; this one must NOT — a pop that wraps replays its attack, and
         * the whole point of the buffer is that the stop timer no longer has to be exact. */
        const size_t left = this->pcmMemLen - this->pcmMemPos;
        size_t n = left < sizeof(this->playDec) ? left : sizeof(this->playDec);
        if (n > 0) {
          memcpy(this->playDec, this->pcmMem + this->pcmMemPos, n);
          this->pcmMemPos += n;
        } else {
          memset(this->playDec, 0, sizeof(this->playDec));
          n = sizeof(this->playDec);
          this->playbackEof = true;       // every sample handed to the DMA; padding from here
        }
        res = (int)n;
      } else {
        if (!playbackFile.available()) {
          this->setFilePos(0);
        }

        res = playbackFile.read((uint8_t*)this->playDec, sizeof(this->playDec));
      }

      this->playDecCurFrame = 0;
      this->playDecFramesLeft = res / 2;

      this->playChunk();
    }


  } else if (this->playback == Playback::LocalMp3 || this->playback == Playback::LocalWav) {
    /* ── ONE PASS OF THE MUSIC FEED (music_feed.h) ──────────────────────────────────
     * Decode whole frames into music's own mono ring until it refuses (never waiting),
     * at most MUSIC_UNITS_PER_PASS of them (more when the ring is low), and count, honestly,
     * whether the ring ran dry since the last pass.
     *
     * 🛑 WHAT THIS REPLACED, measured 2026-09-25 on both phones and replayed on the Mac: a
     * 512-byte read and ONE decode per fill, the pass ending the first time a decode lacked
     * input. A 192 kbps frame is 627 bytes, so 8.6 passes a second ended that way and each
     * counted as a "gap" — 167 in 19 s on 0.9.78, with no dropout at all at short passes; and
     * the ring (~70 ms usable, 4 x 1023 stereo) ran dry for real under any pass longer than
     * that, which the counter mostly missed. And the top-of-loop playChunk() pushed 25-61% of
     * all music one SAMPLE per i2s_write. Music never touches playDecFramesLeft now, so that
     * per-sample path never runs for it. */
    if (this->feed && this->i2sInstalled) {
      this->feed->pass(this->musicDma());
      if (this->feed->failed()) {
        /* The card would not read, MUSIC_READ_TRIES passes running, before the end of the file
         * (music_feed.h). Stop HERE, keeping the place — ceasePlayback() hands it to the player,
         * which makes it a pause with the reason on screen, so F1 tries again — instead of
         * ending the track: that skipped the rest of it, and a pulled card walked the queue. */
        this->musicProblem = this->feed->problem();
        log_e("MUSIC: %s - stopped, place kept", this->musicProblem ? this->musicProblem : "?");
        this->ceasePlayback();
      }
    }

  } else if (this->playback == Playback::Record) {
    if (this->playDecFramesLeft <= 0 && this->recordRaw) {
      int sz = (this->recordRawW - this->recordRawR) * sizeof(this->recordRaw[0]);
      if (sz > 0 && this->recordRawR < this->recordRawSizeSamples) {
        if (sz > sizeof(this->playDec)) {
          sz = sizeof(this->playDec);
        }
        memcpy(this->playDec, this->recordRaw + this->recordRawR, sz);

        this->playDecCurFrame = 0;
        this->playDecFramesLeft = sz / sizeof(this->playDec[0]);     // 16-bit samples assumed implicitly; TODO: use current bits per sample
        this->recordRawR += sz / sizeof(this->recordRaw[0]);
        this->playChunk();
      }
    }

  } else if (this->playback == Playback::RtpStream) {

    if (this->playDecCurFrame > 0) {
      // Move the data to beginning of output buffer (because we are about to receive some more data)     // TODO: maybe do the same for MP3?
      memmove(this->playDec, this->playDec + this->playDecCurFrame, sizeof(this->playDec[0]) * this->playDecFramesLeft);
      this->playDecCurFrame = 0;
    }

    // Do not attempt to decode if the buffer doesn't have much free space
    uint16_t playDecFreeSpace = sizeof(this->playDec)/sizeof(this->playDec[0]) - this->playDecFramesLeft;
    if (wifiState.isConnected() &&
        (!this->playDecCurFrame || playDecFreeSpace >= this->voipPacketSize)) { // if the output buffer is empty or has enough space for a big voip packet (NOTE: former is not always part of latter)
//      // Debug
//      this->rtpCnt++;       // remove

      // RECEIVE AUDIO STREAM

      // Receive RTP packet
      if (rtp.available()) {
        // should never happen, but just to be safe
        log_d("RTP flushed");
        rtp.flush();
      };

      int32_t len = udpParsePacketSafe(rtp);

      if (len == 0) {
        const RtpQuiet q = rtpSilenceQuiet(s_rtpSilence, millis());
        if (q == RTP_QUIET_STRIKE) {
          log_d("NO RTP PACKETS FROM REMOTE PART for %u s - one more window ends the call",
                (unsigned)(RTP_SILENCE_WINDOW_MS / 1000));
        } else if (q == RTP_QUIET_END) {
          rtpSilentPeriod = RTP_SILENT_ON;       // the main loop ends the call on its next pass
        }
      }

      if (len > 0) {
        rtpSilenceHeard(s_rtpSilence, millis());
        rtpSilentPeriod = RTP_SILENT_OFF;
        //log_d("RTP packet received: %d", len);

        // Stats
        this->packetsReceived++;
        uint16_t remotePort = rtp.remotePort();
        if (rtp.remotePort() % 2 == 0) {
          this->rtpPort = rtp.remotePort();
        } else {
          //this->rtcpPort = rtp.remotePort();
          //this->rtcpPacketsReceived++;
        }

        // Debug
        //      if (this->rtpCnt % 10 == 0) {
        //        rtp.beginPacket("192.168.1.15", remotePort+1);
        //        rtp.write((const uint8_t*) "ACK", 3);
        //        if (!rtp.endPacket()) log_d("sending fail");
        //      }

        // Parse packet

        len = rtp.read(playEnc, sizeof(playEnc) - 1);
        if (len > 12) {
          if (rtp.remotePort() == rtpRemotePort || !rtpRemotePort) {    // ensuring that the audio comes from the right port; TODO: ensure also that it comes from the right IP
            // Parse RTP packet
            //uint8_t payloadType = rtpRecv.decodeHeader(playEnc);

            rtpRecv.setHeader(playEnc);
            uint8_t payloadType = rtpRecv.getPayloadType();

            if (payloadType == rtpPayloadType) {
              // Did packets arrive in correct sequence?
              bool inSeq = false;


              uint16_t seqDiff = (rtpRecv.getSequenceNumber() >= this->lastSequenceNum) ?
                                 rtpRecv.getSequenceNumber() - this->lastSequenceNum :
                                 0xffffu - this->lastSequenceNum + rtpRecv.getSequenceNumber();




              /*
              uint16_t seqDiff = (rtpRecv.getSequenceNum() >= this->lastSequenceNum) ?
                                   rtpRecv.getSequenceNum() - this->lastSequenceNum :
                                   0xffffu - this->lastSequenceNum + rtpRecv.getSequenceNum();
                                   */
              if (this->firstPacket) {
                inSeq = true;
                this->firstPacket = false;
                log_i("Sound source (SSRC): %u", rtpRecv.getSSRC());
              }

              if (seqDiff > 0 && seqDiff <= 1000) {   // not more than 20 seconds apart (20ms packet)
                // Packet in order (maybe some packets missed)
                inSeq = true;
                if (seqDiff > 1) {
                  // Some packets were missed
                  log_d("miss %d", seqDiff - 1);
                  this->packetsMissed += seqDiff - 1;
                }
                // Show how many packets arrived not in order until this one got received
                if (this->packetsUnord > 0) {
                  log_d("unord %d", this->packetsUnord);
                  this->packetsUnord = 0;
                }
              } else if (seqDiff > 0) {
                // Packet not in order -> count packets arriving not in order, until one received that is in order
                this->packetsUnord++;
              } else {
                // This packet was already received before
                log_d("dup");
              }

              // Decode packet audio if in correct sequence
              if (inSeq) {
                this->packetsGood++;

                //info.time[5] = micros();

                // Decode packet. If packet is too big -> drop it;      TODO: decode and use packet partially

                const int32_t RTP_HEADER_SIZE = 12;                  // TODO: make RTP class tell the header size
                if (payloadType == G722_RTP_PAYLOAD) {
                  if ((len - RTP_HEADER_SIZE)*2 < playDecFreeSpace) {         // G.722 typically decodes 160 bytes into 320 samples (640 bytes)
                    int16_t samplesDecoded = g722_decode(g722Decoder, playEnc + RTP_HEADER_SIZE, len - RTP_HEADER_SIZE, playDec + playDecCurFrame);
                    if (samplesDecoded > 0) {
                      playDecFramesLeft += samplesDecoded;
                    }
                  }
                } else if (payloadType == ALAW_RTP_PAYLOAD) {
                  if (len - RTP_HEADER_SIZE < playDecFreeSpace) {             // G.711 typically decodes 160 bytes into 160 samples (320 bytes)
                    alaw_expand(len, playEnc + RTP_HEADER_SIZE, playDec + playDecCurFrame);
                    playDecFramesLeft += len - RTP_HEADER_SIZE;     // G.711 just turns each byte into two bytes (except for 12 bytes of the RTP header)
                  }
                } else if (payloadType == ULAW_RTP_PAYLOAD) {
                  if (len - RTP_HEADER_SIZE < playDecFreeSpace) {             // G.711 typically decodes 160 bytes into 160 samples (320 bytes)
                    ulaw_expand(len, playEnc + RTP_HEADER_SIZE, playDec + playDecCurFrame);
                    playDecFramesLeft += len - RTP_HEADER_SIZE;
                  }
                }

                // Remember sequence number
                // TODO: if this sequence is incorrect, entire call audio might be discarded; add resiliency
                //this->lastSequenceNum = rtpRecv.getSequenceNum();
                this->lastSequenceNum = rtpRecv.getSequenceNumber();
              }
            } else {
              this->packetsWrongPayload++;
              log_d("unknown fmt %d", payloadType);
            }
          } else {
            //log_d("audio from incorrect port");
          }
        } else if (len > 0) {
          log_d("packet too short");
        }
        //} else if (len < 0 && len!=-3) {
        //  // Debugging
        //  log_d("parse packet err=%d", len);
      }

      // TODO: figure out why RTCP doesn't work
//        // Receive RTCP packets
//        if (udpRtcp.available()) {
//          // should never happen, but just to be safe
//          DEBUG("RTCP flushed");
//          udpRtcp.flush();
//        };
//        if (udpRtcp.parsePacket()>0) {
//          // Stats
//          gui.state.rtcpPort = udpRtcp.remotePort();
//          gui.state.rtcpPacketsReceived++;
//
//          // Parse packet
//          l = udpRtcp.read(recv_buff, sizeof(recv_buff)-1);
//        }


      // Play right away, don't wait for the next loop
      //oldSmp = this->playDecFramesLeft;
      if (this->playDecFramesLeft>0) {
        this->playChunk();
      }
      //info.samples[1] = oldSmp - this->playDecFramesLeft;

    }
    if (wifiState.isConnected()) {
      // Not enough space in the receiving buffer for another packet
      // TODO: better to drop packet which was not yet decoded
      if (this->playDecFramesLeft > this->voipPacketSize) {
        this->playDecFramesLeft -= this->voipPacketSize;
        this->playDecCurFrame += this->voipPacketSize;
        log_d("decoded packet dropped");
      }
    }
  }

  //info.time[6] = micros();
  //if (info.time[6] - info.time[0] >= 6)
  //  profile.add(info);
}


uint32_t Audio::getFileSize() {
  if (!playbackFile) {
    return 0;
  }
  return playbackFile.size();
}

uint32_t Audio::getFilePos() {
  if (!playbackFile) {
    return 0;
  }
  return playbackFile.position();
}

bool Audio::setFilePos(uint32_t pos) {
  if (!playbackFile) {
    return false;
  }
  return playbackFile.seek(pos);
}

/* Description:
 *     calculate number of samples in an audio packet of given duration
 *     (e.g. 160 samples for 20 ms 8000 Hz audio, 320 samples for 20 ms of 16 KHz audio)
 * Paramters:
 *     duration in milliseconds
 * Return:
 *     number of samples (typically, number of 16-bit words for a given duration)
 */
int Audio::packetSizeSamples(int duration) {
  return this->dataChannels * this->sampleRate * duration / 1000;
}

bool Audio::setSampleRate(int freq) {
  log_d("SAMPLE RATE = %d", freq);
  this->sampleRate = freq;
  /* ⚠ THE CACHE STAYS TRUE, AND THE NEXT INSTALL REQUEST IS A FRESH ONE (0.9.79, review).
   *
   * i2s_set_sample_rates() is i2s_set_clk(): with the bits unchanged it keeps the DMA ring and
   * its free-buffer queue, then i2s_stop() + i2s_start() — the DMA restarts at buffer 0 while the
   * queue stays in its old finish order, so the next trip round the ring plays SCRAMBLED
   * (music_feed.h; tests/test_musicfeed.cpp, "pause and resume on the same ring").
   *   - i2sRate follows the hardware (musicDma() and a later configureI2S() read it): the old
   *     cache kept the install-time rate, and a later configureI2S() asking for THAT rate
   *     "matched" and left the hardware at this one.
   *   - i2sQueueStale makes the next configureI2S() REINSTALL even when everything else
   *     matches. Without it (a2aa516), every sequence that ends in setMonoOutput(true) — a pop,
   *     the ringtone, a call's audio — found the mono default already in after a music session
   *     and skipped the reinstall, so from the second pop on the 300 ms chirp came late, out of
   *     order and cut short by its stop timer. 0.9.78 got a fresh ring there by accident (the
   *     stale rate never matched); this gets it on purpose, at the same one reinstall a pop
   *     (playPop() and playRingtone() set the rate FIRST so the reinstall that follows is the
   *     only one, even straight after music's own ring).
   *   - An UNCHANGED rate touches nothing: no restart, nothing to go stale. A call sets its rate
   *     for the microphone and again for the far end's stream (sendRtpStreamFromMic() then
   *     playRtpStream()), and the second used to restart the ring under the first trip of it.
   *   - No driver (a failed install): IDF 3.3 would dereference NULL. The next install sets it. */
  if (this->i2sInstalled && this->i2sRate != freq) {
    if (i2s_set_sample_rates((i2s_port_t) i2s_num, freq) == ESP_OK) {
      this->i2sRate = freq;
    }
    this->i2sQueueStale = true;
    this->i2sGen++;                    // a new rate is a new ring to the music feed's lead
  }
  this->voipPacketSize = this->packetSizeSamples(VOIP_PACKET_DURATION_MS);
  return true;
}

bool Audio::setBitsPerSample(int bits) {
  if ( (bits != 16) && (bits != 8) ) {
    return false;
  }
  this->bps = bits;
  this->configureI2S();
  //i2s_set_clk((i2s_port_t) i2s_num, this->sampleRate, this->bps==16 ? I2S_BITS_PER_SAMPLE_16BIT : I2S_BITS_PER_SAMPLE_8BIT, this->monoOut ? I2S_CHANNEL_MONO : I2S_CHANNEL_STEREO );      // TODO: does it work?
  return true;
}

void Audio::setMonoOutput(bool mono) {
  // this->monoOut affects I2S interface
  log_d("monoOut = %s", mono ? "true" : "false");
  this->monoOut = mono;
  this->configureI2S();
  codec.setAudioPath(!mono);
};

bool Audio::setDataChannels(int ch) {
  // this->dataChannels shows how many channels are being decoded (from MP3 or another audio stream)
  if ( (ch < 1) || (ch > 2) ) {
    return false;
  }
  this->dataChannels = ch;
  this->voipPacketSize = this->packetSizeSamples(VOIP_PACKET_DURATION_MS);
  log_d("Channels=%i", this->dataChannels);
  return true;
}

AUDIO_INLINE bool Audio::playSample() {
  if (this->bps == 8) {
    // Upsample from unsigned 8 bits to signed 16 bits
    this->sample[0] = (((int16_t)(this->sample[0]&0xff)) - 128) << 8;
    this->sample[1] = (((int16_t)(this->sample[1]&0xff)) - 128) << 8;
  }

  size_t bytesWritten;
  if (this->monoOut) {
    esp_err_t err = i2s_write((i2s_port_t) i2s_num, ((const char*)this->sample), sizeof(this->sample[0]), &bytesWritten, 0);
    return (err==ESP_OK && bytesWritten==sizeof(this->sample[0]));
  } else {
    esp_err_t err = i2s_write((i2s_port_t) i2s_num, ((const char*)this->sample), sizeof(this->sample),    &bytesWritten, 0);
    return (err==ESP_OK && bytesWritten==sizeof(this->sample));
  }
}

void Audio::newCall() {
  /* 🛑 THE SILENCE CLOCK STARTS AT THIS CALL. It used to carry over from the last one, which
   * ended the second call of every boot at connect — see rtp_watch.h. The flag too: a verdict
   * about the last call's far end is not one about this call's. */
  rtpSilenceBegin(s_rtpSilence, millis());
  rtpSilentPeriod = RTP_SILENT_OFF;

  this->firstPacket = true;
  this->lastSequenceNum = 0;

  this->rtpPort = 0;
  this->rtcpPort = 0;
  this->rtcpPacketsReceived = 0;

  // QoS stats
  this->packetsReceived = 0;
  this->packetsGood = 0;
  this->packetsWrongPayload = 0;
  this->packetsMissed = 0;
  this->packetsUnord = 0;

  this->packetsSent = 0;
  this->packetsSendingFailed = 0;
}

void Audio::showAudioStats() {
  log_d("Incoming audio packets:");
  log_d(" received:  %d", this->packetsReceived);
  log_d("     good:  %d", this->packetsGood);
  log_d("    wrong:  %d", this->packetsWrongPayload);
  log_d("     miss:  %d", this->packetsMissed);
  if (this->packetsGood > 0 && this->packetsMissed > 0) {
    log_d("good/(miss+good): %.2f%%", (float) this->packetsGood/(this->packetsGood + this->packetsMissed)*100);
  }
  log_d("    unord: %d", this->packetsUnord);

  log_d("Outgoing audio packets:");
  log_d("    total:  %d", this->packetsSent);
  log_d("   failed:  %d (%.2f%%)", this->packetsSendingFailed, (float) this->packetsSendingFailed/this->packetsSent*100);

  log_d("Total RTCP packets received: %d", this->rtcpPacketsReceived);
  log_d(" RTP port: %d", this->rtpPort);
  log_d("RTCP port: %d", this->rtcpPort);
}

uint16_t Audio::openRtpConnection(uint16_t rtpLocalPort) {
  rtp.begin(rtpLocalPort);          // TODO: check if successful, allow search for a free port (or next port) on its own
  return rtpLocalPort;
}

bool Audio::playRtpStream(uint8_t payloadType, uint16_t remotePort) {
  log_d("playing rtp");

  // Determine sample rate and initialize audio configs
  uint16_t sampleRate = (payloadType == ALAW_RTP_PAYLOAD || payloadType == ULAW_RTP_PAYLOAD) ? 8000 : 16000;      // default is 16000
  this->setSampleRate(sampleRate);
  this->setDataChannels(1);
  this->setMonoOutput(true);      // this should be called last (since it shows all the configs via Serial)   TODO

  /* 🛑 NO DRIVER, NO STREAM (review 3, A2). A caller dialling over a playing track leaves the
   * device ON (the yield pauses the track, not the codec), and the reinstall above frees music's
   * ring first: if it failed, the first RTP packet's playChunk() wrote a NULL driver. No audio
   * for this call beats a reboot in the middle of it. */
  if (!this->i2sInstalled) {
    log_e("AUDIO: call audio refused - no I2S driver (the install failed)");
    return false;
  }

  // Start the audio systems (if not started)
  if (!this->turnOn()) {
    return false;
  }

  // Prepare to receive packets
  rtpRemotePort = remotePort;
  rtpPayloadType = payloadType;
  log_d("rtpPayloadType = %d", rtpPayloadType);

  // Reset QoS variables
  this->newCall();

  // Clear buffers
  this->playEncW=0;
  this->playEncR=0;
  this->playDecFramesLeft = 0;

  // Reset debugging
  this->loopCnt = this->runCnt = this->rtpCnt = 0;

  // Kickstart playback
  this->playback = Playback::RtpStream;

  return true;
}

bool Audio::sendRtpStreamFromMic(uint8_t payloadType, IPAddress remoteAddr, uint16_t remotePort) {

  // TODO: check correctness of the parameters
  this->rtpPayloadType = payloadType;
  this->rtpRemoteIP = remoteAddr;
  this->rtpRemotePort = remotePort;

  // Determine sample rate and initialize audio configs
  // Configuration is exactly the same as for playback
  uint16_t sampleRate = (payloadType == ALAW_RTP_PAYLOAD || payloadType == ULAW_RTP_PAYLOAD) ? 8000 : 16000;      // default is 16000
  this->setSampleRate(sampleRate);
  this->setDataChannels(1);
  this->setMonoOutput(true);      // this should be called last (since it shows all the configs via Serial)   TODO

  // Start the audio systems (if not started)
  if (!this->turnOn()) {
    return false;
  }

  if (!this->turnMicOn()) {
    return false;
  }
  this->calcMicIntensity = false;

  // Prepare RTP header for sending
  rtpSend.setPayloadType(payloadType);
  rtpSend.newSession();
  // Kickstart streaming
  this->microphoneStreamOut = true;
  /* ⚠ This success path fell off the end with no return — undefined behaviour in C++, and the
   * value it handed back was whatever the return register held (the 2026-09 audit read `this`
   * there: true by accident). Every caller ignores the result, which is all that kept it
   * harmless. */
  return true;
}

bool Audio::recordFromMic() {

  if (this->playback == Playback::Record) {
    this->ceasePlayback();
  }

  if (this->recordRaw == NULL) {
    this->recordRaw = (uint16_t*) extMalloc(RECORDING_SIZE_SAMPLES);
  }
  if (this->recordRaw == NULL) {
    log_d("failed allocating 1MB recording buffer");
    return false;
  }
  this->recordRawW = this->recordRawR = 0;
  this->recordRawSizeSamples = RECORDING_SIZE_SAMPLES/2;
  this->recordFinished = false;

  // Start the audio systems (if not started)
  if (!this->turnOn()) {
    return false;
  }

  if (!this->turnMicOn()) {
    return false;
  }

  // Kickstart recording
  this->microphoneRecord = true;

  return true;
}

bool Audio::saveWavRecord(fs::FS *fs, const char* pathName) {

  this->microphoneRecord = false;   // if we are saving, we automatically stop recording

  File recordFile = fs->open(pathName, FILE_WRITE);
  if (recordFile) {
    log_d("created file");
  } else {
    log_d("failed creating file");
    return false;
  }

  if (this->recordRawW > 0) {
    recordFile.write((const uint8_t*) this->recordRaw, this->recordRawW * 2);
    log_i("%d bytes written to audio file", this->recordRawW * 2);
  }
  recordFile.close();

  return true;
}

void Audio::ceaseRecording() {
  this->microphoneRecord = false;
  freeNull((void **) &this->recordRaw);
  /* 🛑 ZERO THE INDICES WITH THE BUFFER. This freed recordRaw and left recordRawW at whatever
   * the recording had reached — and saveWavRecord() writes `recordFile.write(recordRaw,
   * recordRawW * 2)` behind `if (recordRawW > 0)` (:1615-1616). So ANY shutdown() during a
   * recording (shutdown() calls ceaseRecording() at :275) armed a read from address 0 on the
   * next Save. Pre-existing, and reachable from every teardown path — the audio idle watchdog
   * in WiPhone.ino would merely have been the first thing to trigger it reliably.
   * The buffer and the indices that describe it must die together. */
  this->recordRawW = this->recordRawR = 0;
  this->recordFinished = false;
}

bool Audio::turnMicOn() {

  /* ⚠ NOT UNDER A PLAYING TRACK (review, 2026-09-25). The configureI2S() below swaps music's
   * 24 x 512 ring for the 4 x 1024 default under the feed — it plays on, on ~186 ms of ring, for
   * the rest of the track. The Mic test, Recorder and LED mic apps pause the player before they
   * start (GUI.cpp); this is the device refusing on its own behalf for any caller that does not.
   * ceasePlayback() keeps the place (musicTakeStopPlace()): the player makes it a pause. */
  if (this->musicPlaying()) {
    this->ceasePlayback();
  }

  /* ⚠ AN INSTALL WITH AN RX SIDE FIRST (0.9.79). Music's own I2S install is TX only, and the
   * mic-level meters (GUI.cpp) come here straight after start() without calling any setter,
   * so after a track they would have read nothing. configureI2S() is a no-op when the default
   * install is already in; after music it puts it back. */
  this->configureI2S();

  // Start the audio systems (if not started)
  // TODO: separate electrically switching microphone ON into this routine
  if (!this->turnOn()) {
    return false;
  }

  // Reset mic buffers
  this->micRawR = 0;
  this->micRawW = 0;
  memset(this->micAvg, 0, sizeof(this->micAvg));

  // Start microphone data processing (calculate average intensity)
  this->microphoneOn = true;
  this->calcMicIntensity = true;

  return true;
}

/* Description:
 *     save data point to the microphone volume averaging array
 */
void Audio::setMicAvg(uint32_t mic) {
  this->micAvg[this->micAvgNext++] = mic;
  if (this->micAvgNext >= sizeof(this->micAvg)/sizeof(this->micAvg[0])) {
    this->micAvgNext = 0;
  }
}

/* Description:
 *      get average microphone volume
 */
uint32_t Audio::getMicAvg() {
  uint32_t val = 0;
  for (int i = 0; i < sizeof(this->micAvg)/sizeof(this->micAvg[0]); i++) {
    val += this->micAvg[i];
  }
  return val / (sizeof(this->micAvg)/sizeof(this->micAvg[0]));
}
