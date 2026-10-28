# Stretch goal — the "woods" backplate

**Status: parts IN HAND (2026-08-20), bench checks next, nothing soldered. Firmware: WRITTEN, ships dormant — see the firmware section.**
Captured 2026-08-10 so the reasoning is not lost. Diagrams: [`woods-backplate.svg`](woods-backplate.svg)
(the gist) and **[`woods-backplate-wiring.svg`](woods-backplate-wiring.svg) — the leg-by-leg build
sheet** (every pad, every cap leg, numbered wire run list, print A3; RFM95W pads verified against
HopeRF DS V2.0 Table 2 + Figure 2 on 2026-08-20). Build from the wiring sheet, not from prose.

> **Revised 2026-08-11 against WiPhone's own published pinout, which settles two of the three
> "measure this first" items and kills one architecture outright.**
> - **The 5 V pogo pin is USB VBUS and reads 0 V on battery** — measure-item #1 is ANSWERED, and
>   the answer is the bad one. **The GPS moves to 3.3 V** (the M100 Mini takes 3.3–5 V, so this
>   costs nothing).
> - **You CAN feed power in through the 5 V pin** — that is how the official Mega Battery Pack
>   works, and it is the right architecture for this plate. (An earlier revision of this doc said
>   the opposite; the Mega disproves it. See *The battery* below.)
> - **The phone's 3.3 V pin is a MIC5219 rated 150–200 mA**, and RFM95W TX + GPS ≈ 150 mA. Give
>   the plate **its own regulator**; measure-item #2 is answered by the datasheet, not the scope.

A fatter, higher-capacity backplate for time in the woods: the LoRa radio, a GPS, a real
915 MHz whip on an SMA bulkhead, and an expanded battery — on a screw-terminal plate instead
of the stock LoRa one.

## Parts on hand

| Part | Notes |
|---|---|
| HopeRF **RFM95W** (SX1276, 915 MHz), ×2 | Same module the stock LoRa plate uses. Bare **`ANA`** pad — no u.FL on the module itself |
| **HGLRC M100 Mini** GPS | u-blox, UART, 4 pads: `GND / 5V / TX / RX` |
| Larger 1S battery | |
| Screw-terminal backplate | No LoRa chip on it — the RFM95W would be added |

## What the stock LoRa plate (v2.0) actually carries

Read off a photo of the real board, so treat placement as certain and **values as unknown**.

| Ref | Part | Almost certainly |
|---|---|---|
| U1 | RFM95W (marked RF96) | the radio |
| **J5** | **IPEX MHF / u.FL jack** | the antenna connector the module lacks |
| C1 | one capacitor | supply decoupling / bulk |
| R1, R2 | pair beside the module's DIGITAL pins | ✅ MEASURED 2026-08-21: **~98.9 kΩ / ~52 kΩ** — pull-ups, NOT series damping (that would be tens of ohms) and NOT in the RF path. In-circuit readings, so "about 100 k and about 51 k" |
| R6 | sits between the module and J5 | in or beside the **RF path** — most likely a 0 Ω link |
| ~~R7, R11~~ | — | ✅ **DO NOT EXIST on v2.2** ({user}'s board, photographed 2026-08-21). They came from a v2.0 assumption; nothing to copy |
| — | FPC antenna on 3M 300LSE + coax to J5 | stock antenna |
| — | "PCB Slot" cutout | cable exit for an external antenna |

✅ **RESOLVED 2026-08-21 — first from {user}'s photo, then CONFIRMED against WiPhone's own published
design files** (they are on wiphone.io, not GitHub; the ESP32-WiPhone org has firmware and connector
footprints only). Authoritative values, from the Eagle netlist + the official BOM, cross-checked
against the schematic PDF:

| ref | published value | note |
|---|---|---|
| R1 | **100 kΩ** 0603 5% | pull-up, U1 `!RESET` → 3.3 V ({user} measured 98.9 kΩ ✅) |
| R2 | **100 kΩ** 0603 5% | pull-up, U1 `NSS` → 3.3 V ({user}'s 52 kΩ is an in-circuit parallel path, not a different part) |
| R6 | **0 Ω** 0603 | series link, the ONLY thing between radio and connector |
| C2, C3 | **DNP** | the shunt pads of the pi footprint, deliberately unpopulated |
| C1 | 100 nF | decoupling |
| J1 | U.FL | ⚠ the stock plate has **no SMA at all** |

🔑 **The netlist settles the RF question outright**: `ANT = {R6.2, C3.2, U1.ANT}` and
`N$9 = {R6.1, C2.2, J1.SIGNAL}`. The path is RFM95W ANT → 0 Ω → U.FL. **No matching network, no
filter, no PA.** So soldering coax straight from `ANA` to the SMA pigtail is not a compromise —
it is what the factory board does with one less joint. (It also means that on a stock plate, a
missing or cold-reflowed R6 disconnects the antenna completely.)

⚠ The published schematic is **v2.1**; WiPhone documents v2.0–v2.2 but publishes drawings for
v2.1 only. {user}'s board is a **v2.2** and matches the v2.1 parts list as far as the photo shows.
⚠ The 868 MHz and 915 MHz products serve byte-identical design files — one board, U1 differs.

Sources (design files also copied to `~/Downloads/wiphone-lora-v2.1/` — deliberately NOT committed
here, they are WiPhone's to distribute):
- https://wiphone.io/docs/LoRa/latest/LoRa.html — docs page linking all three files
- `EagleDesignFiles.rar` → `LoRa_DB-v2.1.sch` / `.brd` (Eagle 9.5.2 XML) — the authoritative netlist
- `20200824LoRa_v2.1.xlsx` — official BOM ("Resistor 0R 0603 qty 1 = R6")
- `LoRa_DB-v2.1-sch.pdf` — schematic, dated 2020-08-24

(Originally resolved by photographing {user}'s actual plate, a v2.2, NOT the v2.0 this doc assumed.) The stock RF route is: module `ANA` → PCB trace → a **C2 / R6 / C3 pi-network
footprint** → **J1, a u.FL jack** → pigtail. R6 is the series position of a tuning network that
exists to compensate a PCB TRACE.

🔑 **Which makes R6 moot for this build.** The plate here solders coax straight from `ANA` to the
SMA pigtail (BOM option b): the coax IS the 50 Ω path, by construction, with no trace to tune and
no network to copy. The "one genuinely open RF question" below is therefore answered by being
irrelevant, not by a value — the only thing that matters is keeping that joint short and the braid
well grounded.

(Original warning kept, because it is why the question was asked:)
⚠ **The resistor VALUES have not been established.** They are unmarked 0402/0603. R6's placement
makes "0 Ω RF link" the obvious read, and obvious is not verified — a non-zero series part in a
50 Ω path would be a matching component that must be copied exactly. Get the v2.0 schematic
(WiPhone published this as open hardware) or measure R1/R2/R6/R7/R11 with the plate off the phone.

## ⚡ The one non-obvious design decision: leave DIO0 unconnected

`Hardware.h` has **`RFM95_INT 38`**, and the pogo header calls GPIO 38 **`DB_RXD`**. The stock
plate's silkscreen lists `INT`, so v2.0 really does tie the radio's DIO0 to that pin.

That looks fatal for the GPS, because **GPIO 34–39 are input-only on the ESP32**, making GPIO 38
the only pin on the header that can serve as a UART RX. The GPS must have it.

**It is free.** `mesh_phy.cpp` drives CS and **polls** — it never calls `attachInterrupt` and never
references `RFM95_INT`. Only the legacy `lora.cpp` (RadioHead) uses the interrupt, and that path is
gated off whenever `MESHTASTIC_PHY` is defined in `config.h`.

So on the woods plate: **do not populate the DIO0 → GPIO 38 connection**, and route the GPS TX
there instead. Everything else about the radio wiring can be copied from v2.0 verbatim. This is
the single deliberate deviation from the reference design, and it is only safe because the
Meshtastic PHY polls — **if the PHY is ever changed to be interrupt-driven, this plate breaks.**

## Pin map

| Signal | Pin | Notes |
|---|---|---|
| SPI MISO | GPIO 12 | |
| SPI MOSI | GPIO 13 | |
| SPI SCK | GPIO 14 | |
| RFM95W NSS | GPIO 27 | `RFM95_CS` |
| RFM95W RESET | — | `RFM95_RST -1`; not connected on the stock plate either |
| RFM95W DIO0 | — | **deliberately omitted**, see above |
| GPS TX → ESP32 RX | GPIO 38 | input-only pin; the GPS must have this one |
| GPS RX ← ESP32 TX | GPIO 32 | |
| RFM95W power | **backplate 3.3 V** | not the phone's 3.3 V pin — see below |
| GPS power | **backplate 3.3 V** | ⚠ **not** the 5 V pin; it is dead on battery |
| External battery | VBAT / GND | ⚠ see below |

No level shifters: the ESP32, the RFM95W and the GPS UART are all 3.3 V logic.

## Give the plate its own 3.3 V regulator

The header's 3.3 V pin is the output of a **MIC5219-3.3BM5**, which WiPhone documents as
"designed for 150 mA to 200 mA output current applications". The RFM95W pulls **120 mA** transmitting
at +20 dBm and the M100 another ~30 mA — about 150 mA together, i.e. the whole budget, before the
phone's own daughterboard needs anything. Put an LDO or buck on the plate, sized for ≥ 250 mA.
**v2 (2026-08-22): feed it from BOTH — the PowerBoost's 5.2 V *and* the phone's VBAT, OR'd
through two Schottkys into a BUCK-BOOST.** Pack alive, 5.2 V wins and the phone's cell is
untouched; pack dead, VBAT takes over and the radio and GPS keep working. This is the direct
answer to the failure found on the bench today: the plate's radio lives on the external pack and
the phone can outlive it by hours.

*(v1, superseded, kept because the reasoning still matters:)* "Feed it from the PowerBoost's
5.2 V, not VBAT — the chosen TLV62569's 3.4 V input floor would hit dropout at the bottom of a 1S
discharge, exactly when you are furthest from the truck." **That objection was about the PART,
not the idea.** A buck cannot make 3.3 V from a 3.4 V input; a buck-boost has no such floor, so
swapping the regulator dissolves it.

⚠ **v2 removes NO charging path.** The PowerBoost stays exactly where it is: it still charges the
phone through W4 (the measured Mega-style 5 V push), still charges its own pack over its own
microUSB, and phone-USB still reaches the plate through D1. The only thing that changed is that
the regulator now has a second source to fall back on.

⚠ **D2's reverse blocking is safety-critical** — it is what stops the pack's 4.9 V from
back-feeding VBAT and uncontrolled-charging the phone's cell. Meter the band before power.

⚠ **Gate its EN pin off the phone's 3.3 V pin.** Otherwise the plate's rail is live while the phone
is off and back-powers the ESP32 through its GPIO clamp diodes. One wire, and it removes a whole
class of problem.

✅ **MEASURED ({user}, 2026-08-21):** the header's 3.3 V pin reads **3.3 V with the phone ON and
0 V with the phone OFF**. Two things follow, both load-bearing:
1. **The EN gating above works.** The pin is a true phone-power-state signal, so tying the
   TLV62569's EN to it really does collapse the plate's rail when the phone sleeps — the plate
   cannot quietly drain the pack overnight.
2. **`ENABLE_DAUGHTER_33V` is not gating this rail** (open question 3 below, answered): the rail
   simply follows phone power, with no firmware assertion involved.

⚠ `ENABLE_DAUGHTER_33V` (SX1509 pin 4) exists in `Hardware.h` but the only line that writes it —
`WiPhone.ino:1222` — is **commented out**, so firmware never asserts it. The stock LoRa plate works
regardless, so the daughterboard 3.3 V is evidently not gated by it. ✅ Confirmed with a meter
2026-08-21 (see above): safe to rely on that pin as an EN source.

## Measure these before committing to a PCB

1. ~~Is the header's 5 V live on battery?~~ **ANSWERED — no.** WiPhone's docs: "around 4.5 to 5 VDC
   when the device is plugged into the USB port and **0 when not connected to a USB source**." The
   GPS therefore runs from 3.3 V. The M100 Mini accepts 3.3–5 V, so this is free.
2. ~~Can the 3.3 V rail take RFM95W TX peaks?~~ **ANSWERED — not comfortably.** MIC5219, 150–200 mA.
   Own regulator, as above.
3. **Does the screw-terminal plate break out all ten signals?** The published Header Breakout lists
   5V, VBAT, 3.3V, GND, DB_RXD(38), DB_TXD(32), MISO(12), MOSI(13), CLK(14), CS(27), I2C and D0–D5 —
   so on paper yes. Count them on the physical plate anyway.
4. ~~**The one genuinely open RF question: R1/R2/R6/R7/R11 on the stock v2.0 plate.**~~ ✅ **CLOSED
   2026-08-21** — see the resolved block above: the board is a v2.2, R7/R11 do not exist, R1/R2 are
   ~99 k/~52 k digital pull-ups, and R6 belongs to a trace-tuning network this build does not have.
   (Original text:) Still unmeasured;
   R6 sits in the RF path and "0 Ω link" is the obvious read, not a verified one.

## 🔴 The battery — and the one thing that does NOT work

**Paralleling two packs at unequal charge is what destroyed both packs' protection FETs in July
2026** and caused the whole no-boot saga. Voltage-matching before every connection works, but it is
a ritual that has to be performed correctly *every single time*, forever. So the goal is an
architecture that does not parallel cells at all.

### ✅ The 5 V pogo pin is an input, and the tap is behind the protection

WiPhone's own **Mega Battery Pack** settles this. It

> "acts exactly like an external power bank, but you don't need a cable since it's connected
> **through the daughterboard headers**"

and it is built on an **IP5306** whose output is a **5 V 2.4 A boost**. The 5 V pin is the only
header pin that can take a 5 V boost, so the pack drives it — and the phone charges from it.

That fixes the topology. The fuse, the reverse-protection diode and the current limiter sit between
the **USB connector** and an internal 5 V node; the pogo pin and the charger both hang off that node:

    USB ─[1 A fuse]─[rev-prot diode]─[limiter]─┬─→ 5 V pogo pin
                                               └─→ charger IC

Every documented fact fits this and only this:

| doc says | because |
|---|---|
| "0 when not connected to a USB source" | nothing else drives the node — until a pack does |
| "only recommend… <100mA", "drops below 4V at higher current" | that describes **drawing** current out, back across the fuse/diode/limiter |
| "drop at reverse voltage protection diode and current limiter" | those parts are upstream of the tap, not between the tap and the charger |
| the Mega pushes 2.4 A in through the headers | **pushing** in bypasses all three and goes straight to the charger |

✅ **MEASURED AND CONFIRMED ({user}, 2026-08-21, bench):** with no USB attached, 5.0 V fed to the
5 V pogo pin at a 200 mA limit, **the phone charges.** The topology above is no longer an
inference — the pogo pin reaches the charger, and the PowerBoost will charge the phone in the
woods exactly as the plate assumes.

(The original wording, kept because it is why the test existed: "This is inferred from the
vendor's wording plus the existence of the Mega, not read off a schematic.")

### What happens if you plug USB in while the pack is running

**Nothing bad, and the diode is precisely why.** The host's VBUS only reaches the node after the
fuse, diode and limiter — call it ~4.6–4.7 V. Your pack sits directly on the node at 5.0–5.2 V, so
**the pack wins**: the diode is reverse-biased and no current can flow back into the host port,
which is the job that diode exists to do. D+/D− are untouched, so serial and flashing behave
normally. If the pack sags below ~4.6 V the host quietly takes over. Neither case is a fault.

Cosmetic only: `BATTERY_PPR_PIN` (GPIO 37) reads "USB present" whenever the pack is on, so the
charge icon blinks continuously.

### How to build it

**A — ✅ copy the Mega: an IP5306 module, boost output → the header's 5 V pin.**
Same SoC, and a one-part answer to "mini charging circuit + tiny 5 V booster with its own USB-in":
2.1 A charger, 5 V 2.4 A boost, its own Micro-B input, USB-A out, four charge LEDs, key ON/OFF. Pair
it with IP3005A-class cell protection as the Mega does, plus a fuse in the external leg. The phone's
charger then manages the internal cell normally — no cell paralleling, no matched-voltage ritual,
hot-pluggable in the field, **and the USB port stays free.**

⚠ **IP5306 no-load shutdown.** Most modules cut the boost when the load falls below ~45–100 mA. If
the phone ever idles under that, the pack drops out mid-use. Check the variant before committing —
some expose it as a register or strap setting; a small bleed resistor is the crude fallback.

⚠ **Pass-through is not documented.** Whether the phone's own USB can charge the *pack* depends on
the IP5306's VIN sitting on that same node. Plausible, untested, and worth confirming on the bench
rather than designing around.

**B — ⚠ fallback if the 5 V route disappoints: buck/boost to a current-limited ~4.15 V CV → VBAT.**
Safe *because* it is both voltage- and current-limited, which is exactly what raw paralleling
lacks — it cannot dump an unlimited fault current into a low cell. But it bypasses the phone's
charger and will confuse the CW2015 gauge.

**C — 🛑 hard-parallel onto VBAT.** The July 2026 failure. If done anyway: match within ~50 mV
before every connection; both packs 1S, same chemistry, each with its own PCM; a polyfuse in the
external leg; a connector rather than solder so the packs can be split for charging.

**A bigger pack charges slower, not harder.** Charge current is fixed by the phone's charger.

## Antenna

`ANA` → u.FL/IPEX MHF jack → pigtail → SMA bulkhead → 915 MHz whip. Keep the ANA-to-connector run
short (~5 mm), 50 Ω, grounded both sides. **Never key up with no antenna fitted** — it kills the PA.

⚠ COVEY runs this exact topology and its pigtail is currently a *ranked suspect* in the "heard zero
nodes several miles from home" investigation. Buy a decent pigtail, strain-relieve it where it
crosses the battery, and make sure the u.FL is properly **clicked**, not resting. If the woods
plate ever seems deaf, that joint is the first place to look — and unlike COVEY, this plate has a
known-good FPC antenna to swap against, which is a far better diagnostic than COVEY has ever had.

## Minimum extra parts

📦 **The orderable version of this list, with live stock and links, is in
[`woods-backplate-bom.md`](woods-backplate-bom.md)** — two vendors, Adafruit + Amazon.

- 1 × u.FL / IPEX MHF jack
- 1 × u.FL → SMA bulkhead pigtail
- 1 × 915 MHz SMA whip
- **1 × 3.3 V BUCK-BOOST, ≥ 250 mA, with an EN pin** (the plate's own rail) — ⚠ **buck-boost, not
  a buck**: v2 feeds it from VBAT as well as the pack, and a 1S cell ends at 3.0 V. Fitted choice:
  a TPS63020 module (2–5.5 V in, EN + PS pads, 26 × 18 mm) or Pololu S9V11E2F3 (10.9 × 16.5 mm,
  <0.2 mA quiescent, 100 kΩ EN pull-up that matches the old TLV's, so R1/W12 carry over unchanged).
- **2 × Schottky diodes (D1, D2)** — 1N5819 / SS14 / BAT54. D1 in the PowerBoost 5 V leg (W6),
  D2 in the VBAT leg (W20), both cathodes to the regulator's VIN.
- 2 × 100 nF (RFM95W 3.3 V, GPS 3.3 V)
- 1 × 10 µF (RFM95W 3.3 V bulk)
- 1 × 22–47 µF (GPS 3.3 V bulk)
- 1 × polyfuse + connector (external battery leg)
- **4 × 1 kΩ series resistors (R3–R6)** in the phone-driven lines — W14 MOSI, W15 SCK, W16 NSS,
  W18 GPS-RX. ⚠ Added 2026-08-22 after a measurement, not a guess: with the pack dead and the
  phone alive, the phone's driven pins back-fed the plate through ESD clamp diodes — the 3.3 V
  rail floated at **2.54 V** (one diode below 3.3), the 5 V node at 2.18 V (backwards through the
  TLV). An RFM95W runs at 2.5 V: it answered its version probe, the mesh screen said READY, RX
  worked — and TX was silently impossible (120 mA through clamp diodes is not a thing). 1 kΩ caps
  each clamp path at ~2 mA so the rail collapses, the clamps carry no meaningful DC, and a firmware
  re-probe can tell the truth. MISO (W13) and GPS-TX (W17) get **none** — they are plate-driven,
  and the EN gate means the plate is never powered while the phone is off. SPI timing is untouched:
  the bit-bang runs ~15 µs/bit against ~20 ns of added RC.
- **1 × 10 kΩ NSS pull-up (R7), RFM pad 5 → the plate's 3.3 V rail — REQUIRED** (added
  2026-08-22, adversarial review finding). GPIO 27 floats during every ESP32 reset; if NSS drifts
  low the radio drives MISO — and MISO is GPIO 12 = **MTDI, the flash-voltage boot strap**. MISO
  high at reset release sets VDD_SDIO to 1.8 V and the phone fails to boot. The stock v2.2 LoRa
  plate fits exactly this part (its R2, 100 k to 3.3 V) — the vendor knew. Referenced to the
  PLATE rail on purpose: when the pack is dead the rail is dead, so R7 adds no phantom feed.
- optional 10 kΩ pull-up on RESET
- **for the battery half: 1 × IP5306 module** (charger + boost + USB-in), per route A above

Note v2.0 gets away with a **single** capacitor because the module carries its own decoupling; the
above is deliberately more conservative than the reference.

## Firmware: written 2026-08-20, ships dormant

The phone reads the GPS as of the post-0.9.7 tree — `nmea.{h,cpp}` (checksum-strict, junk-tolerant
NMEA RMC/GGA → 1e-7-degree fixed point, host-proven in `tests/test_nmea.cpp` against
Python-computed vectors) feeding `meshService.gpsUpdate()` off the existing USER_SERIAL UART2
(GPIO 38 RX / 32 TX @ **115200** — the stock pins, and the module's real rate:
✅ MEASURED 2026-08-22 by scanning 9600/19200/38400/57600/115200/230400, of which only 115200
produced sentences. The 9600 originally written here was copied from COVEY's D-033, which
recorded the plan; COVEY's own `covey_ui/gps.py` has run this same module at 115200 since D-062.
`gps baud <n>` retunes it live and persists, so a different module needs no reflash.)
A live fix (< 2 min old)
becomes the reference between a chosen waypoint and the manual pin, so `sun`, Nodes distances and
Places work from the phone's own position with no waypoint heard.

**Dormant by default**: the reader is off until serial `gps on` (persisted in NVS); off, the stock
user-serial GUI path is byte-for-byte untouched. `gps` prints bytes/sentences/bad-checksum/fix —
bytes rising with zero sentences = wrong baud, the first bench question.

⚠ **Deliberately NOT done: no automatic position broadcasts.** A GPS fix never goes on the air by
itself — announcing stays the manual "I'm here" act (the COVEY public-LongFast lesson). Wiring an
auto-broadcast interval is a decision for {user}, not a default.

## Sources for the 2026-08-11 power findings

Every quoted claim about the pogo header comes from WiPhone's own published documentation, not from
measurement on this unit. Re-check anything load-bearing with a meter before it costs money.

- 5 V / VBAT / 3.3 V pin descriptions, the 1 A fuse, the reverse-voltage protection diode and the
  <100 mA recommendation — [Technical Details](https://wiphone.io/docs/WiPhone/latest/technical_manual/hardware/technical_details.html)
- The screw-terminal plate's full signal list — [Header Breakout](https://www.wiphone.io/docs/Header_Breakout/latest/Header%20Breakout.html)
- The official extended-battery daughterboard — its IP5306 (2.1 A charger + 5 V 2.4 A boost),
  IP3005A protection, 4000 mAh cell, and the load-bearing sentence that it "acts exactly like an
  external power bank, but you don't need a cable since it's connected through the daughterboard
  headers" — [Mega Battery Pack](https://www.wiphone.io/docs/Mega/latest/)
- Daughterboard pin/GPIO mapping — [Daughter Board Design Guide](https://wiphone.io/docs/WiPhone/latest/technical_manual/hardware/daughter_board_design_guide.html)
- RFM95W 120 mA TX @ +20 dBm, 1.8–3.7 V supply — [HopeRF RFM95W datasheet](https://cdn.sparkfun.com/assets/a/9/6/1/0/RFM95W-V2.0.pdf)
- M100 Mini accepts 3.3–5 V — [HGLRC M100 Mini](https://docs.cirkitdesigner.com/component/92368671-8460-49b7-a4e5-704d83ce4aa4/hglrc-m100-mini-gps)
