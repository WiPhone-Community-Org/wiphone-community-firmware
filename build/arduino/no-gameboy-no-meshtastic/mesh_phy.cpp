/*
 * mesh_phy.cpp — Meshtastic-compatible SX1276 LoRa PHY (Phase 2, step 1)
 */

#include "mesh_phy.h"
#include "mesh_airtime.h"      // how long the frame we are about to send is on the air
#include "Hardware.h"          // RFM95_CS, RFM95_INT, HSPI_MISO/MOSI/SCLK
#include <RHSoftwareSPI.h>     // RadioHead bit-banged SPI transport

// ---- SX1276 register map (LoRa mode) --------------------------------------
#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PA_CONFIG            0x09
#define REG_FIFO_ADDR_PTR        0x0D
#define REG_FIFO_TX_BASE_ADDR    0x0E
#define REG_FIFO_RX_BASE_ADDR    0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS            0x12
#define REG_RX_NB_BYTES          0x13
#define REG_PKT_SNR_VALUE        0x19
#define REG_PKT_RSSI_VALUE       0x1A
#define REG_MODEM_CONFIG_1       0x1D
#define REG_MODEM_CONFIG_2       0x1E
#define REG_PREAMBLE_MSB         0x20
#define REG_PREAMBLE_LSB         0x21
#define REG_PAYLOAD_LENGTH       0x22
#define REG_MODEM_CONFIG_3       0x26
#define REG_SYNC_WORD            0x39
#define REG_DIO_MAPPING_1        0x40
#define REG_VERSION              0x42
#define REG_PA_DAC               0x4D

// OP_MODE bits
#define MODE_LONG_RANGE_MODE     0x80    // LoRa (must set in SLEEP)
#define MODE_SLEEP               0x00
#define MODE_STDBY               0x01
#define MODE_TX                  0x03
#define MODE_RX_CONTINUOUS       0x05

// IRQ_FLAGS bits
#define IRQ_TX_DONE_MASK         0x08
#define IRQ_PAYLOAD_CRC_ERROR    0x20
#define IRQ_RX_DONE_MASK         0x40

// PA config
#define PA_BOOST                 0x80

#define SX1276_VERSION           0x12

MeshPhy meshPhy;

MeshPhy::MeshPhy()
  : spi(NULL), freqHz(MESH_PHY_FREQ_HZ), ready(false), inRx(false) {}

// ---- Low-level SPI (manual CS on RFM95_CS) --------------------------------

uint8_t MeshPhy::readReg(uint8_t addr) {
  digitalWrite(RFM95_CS, LOW);
  spi->transfer(addr & 0x7f);            // MSB=0 → read
  uint8_t val = spi->transfer(0x00);
  digitalWrite(RFM95_CS, HIGH);
  return val;
}

void MeshPhy::writeReg(uint8_t addr, uint8_t val) {
  digitalWrite(RFM95_CS, LOW);
  spi->transfer(addr | 0x80);            // MSB=1 → write
  spi->transfer(val);
  digitalWrite(RFM95_CS, HIGH);
}

void MeshPhy::readFifo(uint8_t* buf, uint8_t len) {
  digitalWrite(RFM95_CS, LOW);
  spi->transfer(REG_FIFO & 0x7f);
  for (uint8_t i = 0; i < len; i++) {
    buf[i] = spi->transfer(0x00);
  }
  digitalWrite(RFM95_CS, HIGH);
}

void MeshPhy::writeFifo(const uint8_t* buf, uint8_t len) {
  digitalWrite(RFM95_CS, LOW);
  spi->transfer(REG_FIFO | 0x80);
  for (uint8_t i = 0; i < len; i++) {
    spi->transfer(buf[i]);
  }
  digitalWrite(RFM95_CS, HIGH);
}

// ---- Mode / config --------------------------------------------------------

void MeshPhy::setModeIdle() {
  writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
  inRx = false;
}

void MeshPhy::setModeRxContinuous() {
  writeReg(REG_DIO_MAPPING_1, 0x00);     // DIO0 = RxDone (unused; we poll)
  writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_RX_CONTINUOUS);
  inRx = true;
}

void MeshPhy::setFrequency(uint32_t hz) {
  freqHz = hz;
  // Frf = freq * 2^19 / 32MHz
  uint64_t frf = ((uint64_t)hz << 19) / 32000000ULL;
  writeReg(REG_FRF_MSB, (uint8_t)(frf >> 16));
  writeReg(REG_FRF_MID, (uint8_t)(frf >> 8));
  writeReg(REG_FRF_LSB, (uint8_t)(frf >> 0));
}

void MeshPhy::configureLongFast() {
  // MODEM_CONFIG_1: BW=250kHz (0b1000<<4), CR=4/5 (0b001<<1), explicit header (0)
  writeReg(REG_MODEM_CONFIG_1, (0x08 << 4) | (0x01 << 1) | 0x00);   // 0x82
  // MODEM_CONFIG_2: SF=11 (0xB0), RxPayloadCrcOn=1 (0x04)
  writeReg(REG_MODEM_CONFIG_2, (11 << 4) | 0x04);                   // 0xB4
  // MODEM_CONFIG_3: AGC auto on (0x04); LowDataRateOptimize off (symbol < 16ms)
  writeReg(REG_MODEM_CONFIG_3, 0x04);
  // Preamble length = 16 symbols (Meshtastic default)
  writeReg(REG_PREAMBLE_MSB, 0x00);
  writeReg(REG_PREAMBLE_LSB, 0x10);
  // Meshtastic sync word
  writeReg(REG_SYNC_WORD, MESH_PHY_SYNC_WORD);
}

// ---- Public ---------------------------------------------------------------

bool MeshPhy::begin() {
  pinMode(RFM95_CS, OUTPUT);
  digitalWrite(RFM95_CS, HIGH);

  spi = new RHSoftwareSPI();
  spi->setPins(HSPI_MISO, HSPI_MOSI, HSPI_SCLK);
  spi->begin();

  // RST is not wired on this board (RFM95_RST == -1); rely on register config.

  return reinit();
}

bool MeshPhy::healthCheck() {
  if (!ready) {
    return false;
  }
  if (txActive) {
    return true;                         // mid-frame: TX/STANDBY is not a fault - see the header
  }
  /* ⚠ ONE BAD READ IS NOT A DEAD RADIO. Every "health FAILED" in both phones' logs from
   * 2026-09-23 to 09-25 (five, ~one per phone per several hours, 0.9.78 and 0.9.79 alike) was
   * a single register garbled on the bit-banged bus: ver=0x10/0x13/0x03/0x1A where the chip
   * says 0x12, or op=0xC5 where it says 0x85 — one or two bits, with the other register
   * right. RADIO LOST re-inits the chip and, since the TX queue (0.9.79), fails every queued
   * message with "no radio interface", so a glitch cost real messages. A dead pack or a POR
   * reset reads wrong EVERY time, so a fault must survive three reads in a row; a read that
   * comes right on a retry is counted (serial `radio`) and logged, never acted on. */
  uint8_t ver = 0;
  uint8_t op  = 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    ver = readReg(REG_VERSION);
    op  = readReg(REG_OP_MODE);
    const bool rx    = ver == SX1276_VERSION && op == (MODE_LONG_RANGE_MODE | MODE_RX_CONTINUOUS);
    const bool bench = benchSleeping && ver == SX1276_VERSION &&
                       op == (MODE_LONG_RANGE_MODE | MODE_SLEEP);   // `power lora sleep` — see benchSleep()
    if (rx || bench) {
      if (attempt > 0) {
        healthGlitchCount++;
        log_e("MeshPhy: a health read came back garbled (ver=0x%02X op=0x%02X) and read right on "
              "retry %d - bus glitch, radio kept", glitchVer, glitchOp, attempt);
      }
      return true;
    }
    if (attempt == 0) {
      glitchVer = ver;
      glitchOp = op;
    }
  }
  /* The radio is gone (woods pack died - with R3-R6 fitted the rail collapses
   * and nothing answers) or it lost power and rebooted into POR defaults (pack
   * reconnected: version still answers 0x12 but the mode is FSK standby - a
   * bare version probe would call that healthy while it hears and says
   * nothing). Either way: stop trusting it, stop driving it. */
  log_e("MeshPhy: health FAILED (ver=0x%02X op=0x%02X) - radio gone or reset", ver, op);
  ready = false;
  inRx = false;
  return false;
}

/* A frame that was on the air will not finish: say so once, through serviceTx(), so the
 * service can fail its receipt rather than wait for a TxDone that is never coming.
 * ⚠ ASK THE CHIP FIRST (review M3, 2026-09-25). serviceTx() reads TxDone at most every 2 ms and
 * only on a loop pass, so a frame can have FINISHED WHOLE since the last look — and calling it
 * cut made its message's receipt say "failed: timed out" for a text that went. TX_DONE set is
 * reported as DONE instead. RX_DONE cannot be set while the chip transmits, so a read with it
 * set (0xFF from a floating bus: the rail collapsed under a re-init) is not believed, and the
 * frame counts as cut — the safe side for a receipt. Returns true when it really was cut. */
bool MeshPhy::txCutShort(const char* why) {
  if (!txActive) {
    return false;
  }
  const uint8_t irq = readReg(REG_IRQ_FLAGS);
  txActive = false;
  if ((irq & IRQ_TX_DONE_MASK) && !(irq & IRQ_RX_DONE_MASK)) {
    txDoneLate = true;
    txLastAirMs = millis() - txStartMs;  // an upper bound, like serviceTx()'s
    log_e("MeshPhy: TX of %u B had already finished when %s came (%u ms) - reported sent",
          (unsigned)txLen, why, (unsigned)txLastAirMs);
    return false;
  }
  txAborted = true;
  log_e("MeshPhy: TX of %u B cut short after %u ms by %s", (unsigned)txLen,
        (unsigned)(millis() - txStartMs), why);
  return true;
}

bool MeshPhy::benchSleep(bool on) {
  if (!ready) {
    return false;
  }
  bool cut = false;
  if (on) {
    cut = txCutShort("`power lora sleep`");
    writeReg(REG_IRQ_FLAGS, 0xFF);
    writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP);
    inRx = false;
    benchSleeping = true;
  } else {
    benchSleeping = false;
    /* ⚠ NOT mid-frame: `power lora rx` typed while a frame is on the air would force the chip
     * into RX with the PA keyed — the frame truncated on air, and a TIMEOUT for a frame that
     * was fine. serviceTx() puts it back in RX the moment TxDone comes. */
    if (!txActive) {
      setModeRxContinuous();
    }
  }
  return cut;
}

bool MeshPhy::reinit(bool logFailure) {
  benchSleeping = false;                 // a re-init always ends in RX-continuous
  txCutShort("a radio re-init");         // (unreachable today: re-init only follows a failed
                                         //  health check, and that is never mid-frame)
  // Probe the chip: must be in SLEEP to switch to LoRa mode.
  writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP);
  delay(10);
  uint8_t ver = readReg(REG_VERSION);
  if (ver != SX1276_VERSION) {
    if (logFailure) {
      log_e("MeshPhy: SX1276 not found (REG_VERSION=0x%02X, expected 0x12)%s", ver,
            ver == 0x00 ? " - no plate fitted, or its rail is dead" : "");
    }
    ready = false;
    return false;
  }
  /* Second opinion on a different register with a different expected value: we
   * just wrote SLEEP+LoRa, so a real chip reads it back. A floating MISO that
   * happened to produce 0x12 above will not also produce 0x80 here. */
  uint8_t op = readReg(REG_OP_MODE);
  if (op != (MODE_LONG_RANGE_MODE | MODE_SLEEP)) {
    if (logFailure) {
      log_e("MeshPhy: probe readback wrong (op=0x%02X, wrote 0x80) - floating bus?", op);
    }
    ready = false;
    return false;
  }
  log_i("MeshPhy: SX1276 detected (version 0x%02X)", ver);

  setModeIdle();
  setFrequency(freqHz);
  configureLongFast();

  // FIFO base addresses: give full 256-byte FIFO to both TX and RX.
  writeReg(REG_FIFO_TX_BASE_ADDR, 0x00);
  writeReg(REG_FIFO_RX_BASE_ADDR, 0x00);

  // PA: PA_BOOST, ~17 dBm (bring-up level; refined later).
  writeReg(REG_PA_CONFIG, PA_BOOST | 0x0F);
  writeReg(REG_PA_DAC, 0x84);

  setModeRxContinuous();
  ready = true;
  log_i("MeshPhy: LongFast RX @ %lu Hz, sync 0x%02X", (unsigned long)freqHz, MESH_PHY_SYNC_WORD);
  return true;
}

bool MeshPhy::poll(uint8_t* buf, uint16_t maxLen, uint8_t* outLen, int16_t* rssi, int8_t* snr) {
  if (!ready || benchSleeping || txActive) {
    return false;                        // mid-frame the IRQ flags are serviceTx()'s, not ours
  }
  /* ⚠ RATE-LIMITED to every 10 ms. This is called from the main loop at ~1 kHz, and the
   * IRQ read below goes over BIT-BANGED SPI (~64 GPIO ops + per-bit delays) — one register
   * read per millisecond kept the core measurably busy at idle for nothing: at SF11/250k
   * the 16-symbol preamble alone is ~131 ms on air and RX_DONE LATCHES until cleared (and
   * startSend() will not key the transmitter over a latched one) — so a 10 ms cadence
   * cannot lose a packet and adds at most 10 ms of RX latency. TX is serviceTx()'s, below.
   * DIO0 gating would be cheaper still, but that pin's wiring has never been verified on
   * hardware; do not switch without measuring it first. */
  static uint32_t s_lastPollMs = 0;
  const uint32_t nowMs = millis();
  if ((uint32_t)(nowMs - s_lastPollMs) < 10) {
    return false;
  }
  s_lastPollMs = nowMs;
  uint8_t irq = readReg(REG_IRQ_FLAGS);
  if (!(irq & IRQ_RX_DONE_MASK)) {
    return false;                        // nothing received
  }
  // Clear all IRQ flags (write-1-to-clear).
  writeReg(REG_IRQ_FLAGS, 0xFF);

  if (irq & IRQ_PAYLOAD_CRC_ERROR) {
    log_w("MeshPhy: RX CRC error, dropping");
    return false;
  }

  uint16_t len = readReg(REG_RX_NB_BYTES);
  if (len > maxLen) {
    len = maxLen;
  }
  writeReg(REG_FIFO_ADDR_PTR, readReg(REG_FIFO_RX_CURRENT_ADDR));
  readFifo(buf, (uint8_t)len);

  if (outLen) {
    *outLen = len;
  }
  if (snr) {
    *snr = (int8_t)readReg(REG_PKT_SNR_VALUE) / 4;
  }
  if (rssi) {
    // LoRa RSSI (HF port): -157 + raw
    *rssi = -157 + (int16_t)readReg(REG_PKT_RSSI_VALUE);
  }
  return true;
}

bool MeshPhy::startSend(const uint8_t* data, uint8_t len) {
  if (!ready || !data || len == 0 || txActive) {
    return false;
  }
  /* A packet has landed and poll() has not read it yet: keying the transmitter now would
   * clear RX_DONE below and throw it away. Let poll() have it (<= 10 ms) and go next pass. */
  if (readReg(REG_IRQ_FLAGS) & IRQ_RX_DONE_MASK) {
    return false;
  }
  benchSleeping = false;                 // a send ends the bench state: serviceTx() returns to RX
  setModeIdle();
  writeReg(REG_FIFO_ADDR_PTR, 0x00);
  writeFifo(data, len);
  writeReg(REG_PAYLOAD_LENGTH, len);
  writeReg(REG_IRQ_FLAGS, 0xFF);
  writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_TX);
  txActive = true;
  txAborted = false;
  txLen = len;
  txStartMs = millis();
  txLastPollMs = txStartMs;
  /* How long to wait for TxDone before calling the frame lost: airtime + 25 % + 300 ms, never
   * under 1 s (mesh_airtime.h). ⚠ It follows the FRAME: until 2026-09-19 it was a flat 2000 ms,
   * a maximum-length frame is 2157 ms on the air, and a long text was cut off mid-air. It
   * costs nothing to wait now — the loop runs while the chip transmits. */
  txLimitMs = meshLoraTxTimeoutMs(len);
  return true;
}

MeshTxState MeshPhy::serviceTx() {
  if (!txActive) {
    if (txDoneLate) {
      txDoneLate = false;
      return MESH_TX_DONE;               // finished whole before benchSleep()/reinit() looked
    }
    if (txAborted) {
      txAborted = false;
      return MESH_TX_TIMEOUT;            // cut short by benchSleep()/reinit(): report it once
    }
    return MESH_TX_IDLE;
  }
  const uint32_t now = millis();
  /* Two milliseconds between IRQ reads: each is ~64 GPIO toggles of bit-banged SPI, the loop
   * idles at a 5 ms tick, and a frame is >= 354 ms on the air (the 16-byte header alone), so
   * this costs nothing in accuracy and keeps a busy loop from reading the chip every
   * microsecond. */
  if ((uint32_t)(now - txLastPollMs) < 2) {
    return MESH_TX_BUSY;
  }
  txLastPollMs = now;
  /* ⚠ TxDone FIRST, the deadline second — see the header: a pass that comes back late (a Game
   * Boy session skips this whole loop) finds a frame that finished long ago, and it went whole. */
  if (readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK) {
    writeReg(REG_IRQ_FLAGS, 0xFF);
    setModeRxContinuous();
    txActive = false;
    txLastAirMs = now - txStartMs;
    return MESH_TX_DONE;
  }
  if ((uint32_t)(now - txStartMs) > txLimitMs) {
    log_e("MeshPhy: TX timeout after %u ms (%u B frame, %u ms airtime)",
          (unsigned)txLimitMs, (unsigned)txLen, (unsigned)meshLoraAirtimeMs(txLen));
    writeReg(REG_IRQ_FLAGS, 0xFF);
    setModeRxContinuous();
    txActive = false;
    return MESH_TX_TIMEOUT;
  }
  return MESH_TX_BUSY;
}
