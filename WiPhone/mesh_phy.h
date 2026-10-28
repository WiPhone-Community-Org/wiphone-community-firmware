/*
 * mesh_phy.h — Meshtastic-compatible SX1276 LoRa PHY (Phase 2, step 1)
 *
 * A minimal, register-level driver for the RFM95W (Semtech SX1276) that
 * configures the radio to match Meshtastic's on-air PHY: LoRa mode, the
 * LongFast modem preset (BW 250 kHz, SF 11, CR 4/5), sync word 0x2b, and the
 * US channel frequency, then runs RX-continuous. IRQs are polled (no ISR) so
 * the WiPhone superloop stays non-blocking.
 *
 * Step 1 only exposes raw packet RX/TX. Meshtastic PacketHeader parsing,
 * AES-CTR and protobuf are layered on top in later steps (in the service).
 *
 * Reuses RadioHead's RHSoftwareSPI purely as the SPI transport, because that
 * is the exact bit-banged SPI path already proven to work on this board
 * (see lora.cpp). All framing/registers are handled here directly.
 */

#ifndef MESH_PHY_H
#define MESH_PHY_H

#include <Arduino.h>
#include <stdint.h>

// Meshtastic LoRa payload cap (protobuf-encoded MeshPacket over the air).
#define MESH_PHY_MAX_PAYLOAD  256

// LongFast US default primary-channel frequency, in Hz.
// Derived from Meshtastic's US region (freqStart 902.0 MHz, BW 250 kHz):
//   freq = 902.0 + BW/2 + channel_num * BW  → 906.875 MHz for the LongFast slot.
// Overridable so we can retune against a real node during bring-up.
#ifndef MESH_PHY_FREQ_HZ
#define MESH_PHY_FREQ_HZ  906875000UL
#endif

// Meshtastic LoRa sync word (distinguishes the mesh from generic LoRa).
#define MESH_PHY_SYNC_WORD  0x2b

class RHSoftwareSPI;   // forward decl (RadioHead transport)

/* What serviceTx() found. DONE and TIMEOUT are reported ONCE per frame, on the pass that sees
 * them; after that the PHY is IDLE again. */
enum MeshTxState : uint8_t {
  MESH_TX_IDLE    = 0,   // nothing on the air
  MESH_TX_BUSY    = 1,   // a frame is still on the air
  MESH_TX_DONE    = 2,   // TxDone: the frame left whole; the chip is back in RX
  MESH_TX_TIMEOUT = 3,   // it did not finish (no TxDone in time, or the bench / a re-init cut it)
};

class MeshPhy {
public:
  MeshPhy();

  // Initialize SPI + radio. Returns true if the SX1276 is detected
  // (REG_VERSION == 0x12) and configured. Non-fatal on failure (returns false).
  bool begin();

  bool isReady() const { return ready; }

  // Non-blocking RX poll. If a full LoRa packet is available, copies up to
  // `maxLen` bytes into `buf`, sets *outLen, fills RSSI/SNR, and returns true.
  // Otherwise returns false immediately. Always false while a frame is on the air.
  bool poll(uint8_t* buf, uint16_t maxLen, uint8_t* outLen, int16_t* rssi, int8_t* snr);

  /* ── TRANSMIT WITHOUT WAITING ────────────────────────────────────────────────────────────
   * 🛑 THERE IS NO BLOCKING send() ANY MORE, AND THERE MUST NOT BE ONE AGAIN. It put the chip
   * in TX and busy-waited on TxDone for the frame's whole time on air — 518 ms for a 37-byte
   * position beacon, 2157 ms for a full frame — on the superloop task, so the keypad, the
   * screen and the WiFi stack froze with it. Those were the 0.6-1.5 s 'mesh' LOOP STALL lines
   * (~20 an hour, mostly flood relays, all on the airtime lattice in mesh_airtime.h); the
   * database save they were blamed on is ~0.1 s on the card.
   *
   * startSend() writes the FIFO and keys the transmitter (~2-4 ms of bit-banged SPI at 240 MHz,
   * ~9-12 at 80) and returns; the SX1276 transmits on its own. serviceTx(), called every loop
   * pass, reads TxDone and puts the chip back in RX. Everything stays on the loop task: no new
   * task, no lock, no ISR.
   *
   * startSend() REFUSES (false, nothing touched) when: no radio; a frame is already on the air;
   * or RX_DONE is latched — a packet has arrived that poll() has not read yet, and keying the
   * transmitter would clear the flags and lose it (the old send() did exactly that). The caller
   * simply tries again next pass; poll() reads the packet within 10 ms.
   * A send also ends the bench sleep state (see benchSleep()), as it always did. */
  bool startSend(const uint8_t* data, uint8_t len);

  /* Rate-limited to one IRQ read per ~2 ms. ⚠ TxDone is checked BEFORE the deadline: any long
   * pass can come back long after the frame finished, and that frame went out whole — calling
   * it a timeout would fail a receipt for a message that was sent. (A Game Boy session skips
   * the mesh loop but calls MeshtasticService::txComplete(), which calls this — review M2.) */
  MeshTxState serviceTx();
  bool        txBusy() const { return txActive; }
  /* Start of the last finished frame to the pass that SAW its TxDone: an upper bound on its
   * time on air, one loop pass (~5 ms idle) over the true figure — meshLoraAirtimeMs(len) is
   * the exact one. Compare, do not sum. (Until review M2 a game started mid-frame made this the
   * length of the game; the game's passes now finish the frame.) */
  uint32_t    lastTxAirMs() const { return txLastAirMs; }

  /* Is the radio still the radio we configured? Reads REG_VERSION (0x12) and
   * REG_OP_MODE (LoRa bit + RX-continuous, the only mode we idle in). Two
   * registers with two different expected values — a floating MISO cannot pass
   * both by accident, and a chip that lost power and came back in POR defaults
   * (FSK standby, LoRa bit clear) fails the second even though it would pass a
   * bare version probe. That distinction is the whole point: version-only says
   * "present", present-and-misconfigured is DEAF AND MUTE. Cheap: two register
   * reads over the bit-bang, ~240 us (up to three pairs when one comes back
   * garbled: a fault must read wrong three times running - see the .cpp).
   * Call it only between transactions.
   * 🛑 TRUE WITHOUT READING WHILE A FRAME IS ON THE AIR. The op-mode is then TX (0x83), or
   * STANDBY (0x81) once TxDone has fired and serviceTx() has not yet run — neither is the RX
   * mode this checks for, so without the guard a 5 s health tick landing inside a 0.65 s
   * transmit (~13 % of them) would declare the radio LOST and re-initialise it mid-frame,
   * truncating the frame on the air. */
  bool healthCheck();
  /* Health reads that came back wrong and then right on a retry (bus glitches healthCheck()
   * refused to call a dead radio). RAM only; serial `radio` prints it. */
  uint32_t healthGlitches() const { return healthGlitchCount; }

  /* Full re-init for a radio that died and came back (pack reconnected in the
   * field). Re-runs the begin() register sequence on the already-built SPI.
   * Safe to call repeatedly; returns the new ready state.
   *
   * `logFailure` exists because a phone with NO daughterboard at all is a
   * legitimate configuration (stock WiPhone, or ours between plate swaps) and
   * it fails this call every single time, forever. The retry itself is two
   * register reads and stays fast so a swapped pack recovers in seconds — it
   * is only the LOG that needs rationing. Success is always logged. */
  bool reinit(bool logFailure = true);

  uint32_t getFrequencyHz() const { return freqHz; }

  /* BENCH ONLY — the USB-power-meter toggle (serial `power lora sleep|rx`, 2026-09-03).
   * The radio idles in RX-continuous for the whole life of a boot (~11 mA typical on the
   * SX1276 datasheet) and nothing in this firmware ever puts it lower. To MEASURE what that
   * costs rather than quote a datasheet, this parks it in LoRa SLEEP and tells healthCheck()
   * to accept that mode instead of declaring the radio lost and re-initialising it back to RX
   * five seconds later. poll() returns nothing while asleep; the mesh is DEAF for the
   * duration, on purpose. startSend() and reinit() end the bench state, because both leave the
   * chip in RX-continuous anyway (a send once its frame is done).
   * ⚠ Sleeping cuts a frame that is on the air: serviceTx() reports it as MESH_TX_TIMEOUT on
   * the next pass, so the receipt of whatever it was says so instead of claiming it went — UNLESS
   * TxDone had already fired (the frame finished since the last pass): that one is reported
   * DONE, because it went out whole (txCutShort(), review M3). Serial `power lora sleep`
   * refuses while a frame is on the air or waiting, so a bench never cuts a daily phone's text.
   * Returns true when a frame really was cut. */
  bool benchSleep(bool on);
  bool benchAsleep() const { return benchSleeping; }

private:
  uint8_t readReg(uint8_t addr);
  void    writeReg(uint8_t addr, uint8_t val);
  void    readFifo(uint8_t* buf, uint8_t len);
  void    writeFifo(const uint8_t* buf, uint8_t len);

  void setModeIdle();
  void setModeRxContinuous();
  void setFrequency(uint32_t hz);
  void configureLongFast();

  RHSoftwareSPI* spi;
  uint32_t       freqHz;
  bool           ready;
  bool           inRx;
  bool           benchSleeping = false;   // see benchSleep()
  uint32_t       healthGlitchCount = 0;   // see healthGlitches()
  uint8_t        glitchVer = 0;           // the first wrong read of the last glitch, for its log line
  uint8_t        glitchOp  = 0;

  // The frame on the air (see startSend/serviceTx). All touched from the loop task only.
  bool           txActive     = false;
  bool           txAborted    = false;    // cut by benchSleep()/reinit(): one TIMEOUT to report
  bool           txDoneLate   = false;    // ...but TxDone had fired first: one DONE to report
  uint8_t        txLen        = 0;
  uint32_t       txStartMs    = 0;        // millis() just after MODE_TX was written
  uint32_t       txLimitMs    = 0;        // meshLoraTxTimeoutMs(txLen)
  uint32_t       txLastPollMs = 0;        // serviceTx()'s 2 ms rate limit
  uint32_t       txLastAirMs  = 0;        // see lastTxAirMs()
  bool           txCutShort(const char* why);   // true = really cut (false: nothing, or it had finished)
};

extern MeshPhy meshPhy;

#endif // MESH_PHY_H
