/*
 * mesh_airtime.h — how long a LoRa frame is on the air, and how much text fits in one.
 *
 * Two pieces of arithmetic that used to live as guesses in comments and constants:
 *
 *   1. AIRTIME. The PHY gives up on TxDone after a timeout, and until 2026-09-19 that
 *      timeout was a flat 2000 ms. A maximum-length frame at the registers this phone writes
 *      (SF11, BW 250 kHz, CR 4/5, explicit header, CRC on, LDRO off, 16-symbol preamble) is
 *      2157 ms on the air — so a long channel text was declared "TX timeout", its IRQ flags
 *      cleared and the chip forced back to RX WHILE THE PA WAS STILL KEYED. Truncated on
 *      air, send() false, and the whole stall paid anyway. Filed as a latent P2 in
 *      docs/HANDOFF.md; the fix is that the wait follows the frame.
 *      🔑 THE SAME ARITHMETIC NAMED THE 'mesh' LOOP STALLS (0.9.79). Every value in the
 *      health log's `STALL ... mesh N ms` lines sat on this function's lattice — 166 ms of
 *      preamble + (8 + 5k) x 8.192 ms, steps of 40.96 ms — plus the bit-banged FIFO write
 *      (+2..+4 ms at 240 MHz, +9..+12 at 80): the old send() BLOCKED the superloop for the
 *      frame's time on air. It no longer does (MeshPhy::startSend/serviceTx, mesh_txq.h).
 *
 *   2. THE TEXT BUDGET. The Data protobuf around a text costs bytes, the 16-byte header
 *      costs bytes, a PKI DM's CCM tag and nonce cost 12 more, and the LoRa frame is 255.
 *      MESH_TEXT_LEN (234) is the RECEIVE buffer and was also the compose cap, so the
 *      widget let a person type a message the radio could never send. The largest text a
 *      plain channel frame holds is 232, a PKI DM 220 — and the Meshtastic phone apps stop
 *      the user at 200 UTF-8 bytes (Android MESSAGE_CHARACTER_LIMIT_BYTES, iOS
 *      TextMessageField.maxbytes), so that is what "everybody else" can send back.
 *
 * 🔑 ARDUINO-FREE ON PURPOSE. tests/test_airtime.cpp compiles this on the Mac and asserts
 * the airtimes against numbers worked by hand from Semtech AN1200.13, and the budgets
 * against frames actually built by meshBuildData(). mesh_phy.cpp cannot be built on a host
 * (RadioHead, Hardware.h), which is why the formula does not live there.
 */

#ifndef MESH_AIRTIME_H
#define MESH_AIRTIME_H

#include <stdint.h>
#include <stddef.h>

#include "mesh_packet.h"   // MESH_HEADER_LEN
#include "mesh_pki.h"      // MESH_PKI_OVERHEAD

/* ── THE MODEM, AS mesh_phy.cpp's configureLongFast() WRITES IT ──────────────────────────
 * MODEM_CONFIG_1 0x82: BW 250 kHz, CR 4/5, explicit header.  MODEM_CONFIG_2 0xB4: SF11,
 * CRC on.  MODEM_CONFIG_3 0x04: LowDataRateOptimize OFF (stock's rule is on only when a
 * symbol is >= 16 ms; ours is 8.192 ms).  Preamble 16 symbols.
 * ⚠ If configureLongFast() changes, change these — the timeout is derived from them. */
#define MESH_LORA_SF             11
#define MESH_LORA_BW_HZ          250000UL
#define MESH_LORA_CR             1        // AN1200.13's CR term: rate 4/(4+CR) = 4/5
#define MESH_LORA_PREAMBLE_SYMS  16
#define MESH_LORA_CRC_ON         true
#define MESH_LORA_EXPLICIT_HDR   true     // implicit header (IH) = 0
#define MESH_LORA_LDRO           false

/* Time on air in microseconds for a `frameLen`-byte LoRa frame — Semtech AN1200.13:
 *   Tsym      = 2^SF / BW
 *   Tpreamble = (nPreamble + 4.25) * Tsym
 *   nPayload  = 8 + max(ceil((8*PL - 4*SF + 28 + 16*CRC - 20*IH) / (4*(SF - 2*DE))) * (CR+4), 0)
 * Integer arithmetic throughout (the 4.25 is carried as quarter-symbols), exact for the
 * phone's settings: Tsym is 8192 us exactly, so every result here is a whole microsecond. */
uint32_t meshLoraAirtimeUs(size_t frameLen, unsigned sf, uint32_t bwHz, unsigned cr,
                           unsigned preambleSyms, bool crcOn, bool explicitHeader, bool ldro);

/* The same, for the phone's own registers, in whole milliseconds ROUNDED UP.
 *   254/255 B -> 2157   249 B -> 2116   60 B -> 682   37 B -> 519   20 B -> 396 */
uint32_t meshLoraAirtimeMs(size_t frameLen);

/* How long MeshPhy::serviceTx() waits for TxDone before calling the frame lost: airtime +
 * 25 % + 300 ms, never under 1000 ms. The 25 % covers crystal tolerance and the SX1276's own
 * ramp; the 300 ms covers a loop pass that comes round late; the floor keeps a short frame's
 * wait where it always was. ⚠ This used to be how long a transmit could hold the superloop
 * (~3 s for a full frame); since 0.9.79 the loop runs while the chip transmits, so it is only
 * a deadline, and nothing waits on it. */
uint32_t meshLoraTxTimeoutMs(size_t frameLen);

/* ── HOW MUCH TEXT FITS ───────────────────────────────────────────────────────────────── */

#define MESH_LORA_FRAME_MAX   255   // Semtech's cap for the SX12xx FIFO; stock's MAX_LORA_PAYLOAD_LEN

/* What the phone apps let a person type: 200 UTF-8 bytes on Android
 * (MessageScreenComponents.kt MESSAGE_CHARACTER_LIMIT_BYTES) and iOS (TextMessageField.swift
 * maxbytes), read 2026-09-19. Smaller than the wire allows, and that is the point: it is the
 * longest message the people on the other end can SEND, it leaves airtime margin, and a
 * cap that matches theirs is one nobody has to explain. */
#define MESH_TEXT_CLIENT_CAP  200

/* Bytes meshBuildData() wraps around a TEXT_MESSAGE payload of `payloadLen`: portnum (2),
 * the payload tag and its one- or two-byte length (payload present only), and the bitfield
 * (2). ⚠ MIRRORS meshBuildData, and tests/test_airtime.cpp holds the two together for every
 * length from 0 to 250 — change one, the suite says so. */
size_t meshDataOverhead(size_t payloadLen);

/* The longest text that fits one frame: 16-byte header + Data + text (+ 12 for a PKI DM)
 * <= 255. Plain channel 232, PKI DM 220. */
size_t meshTextBudget(bool pki);

/* The cap the compose field is built with: the smaller of the wire budget and what the
 * phone apps allow. 200 for both today; expressed per thread type so a change to either
 * number lands in the right place by itself. */
size_t meshComposeCap(bool pki);

#endif // MESH_AIRTIME_H
