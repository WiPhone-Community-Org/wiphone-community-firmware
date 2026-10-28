# Book sync for everyone — the plan (2026-09-26)

{user}'s ask: *"a full set of instructions on the WiPhones for people using KOSync... they won't want to change
their own router settings... multiple WiFi hotspots and on the go... random e-readers or more likely no
e-readers... may only want this for syncing to a second WiPhone when they don't have a LoRa chip... sync when
on the same network without having to reserve on router settings, then if no WiFi do the hotspot thing, and
the LoRa part always works... a separate 'sync over LoRa' vs 'sync with KOSync' option?"*

This is the recommendation from a 10-agent design pass (3 fact reports on the phone, the X4 fork, COVEY and
the KOReader/CrossPoint/Readest clients; 3 designs from different angles; 2 judges; a synthesis; an
adversarial critic whose corrections are folded in below). The hard rule throughout: **an update or a sync
never moves the place someone is at on their own WiPhone** — every carrier ends at the same card, and only
*Go there* writes a position (`applyPending`, app_books.cpp).

## 1. The answer

**Yes, with the split drawn by WHO you sync with, not by protocol.**

- **Phones and COVEY on the same WiFi: no router settings, no addresses.** Each phone broadcasts on the WiFi
  the same signed CBS1 record it already sends over LoRa (UDP 8083 — COVEY's LanSync port). Any device with
  the same passcode shows the card. Nothing to discover, nothing to type.
- **E-readers cannot discover a phone** (they hold one fixed server URL), so they ALWAYS meet the phone on the
  phone's own hotspot at `http://192.168.4.1` — at home, in a cafe, in the woods. Serving the window on the
  phone's station IP is removed, not patched: that closes the "foreign WiFi" gap by construction. KOSync has
  not shipped, so this breaks nobody.
- **LoRa always works** when both ends have a board and the `booksync` channel — and it becomes a switch.
- **Two honest limits:** cafe/hotel WiFi usually keeps devices apart (WiFi sync fails there and the phone
  SAYS so); two phones with no LoRa and no WiFi cannot sync until a later "Nearby" (ESP-NOW) mode is proven.

## 2. Sync settings (Books > Sync settings, also from a new library row "Sync...")

| Row | Default | Shows |
|---|---|---|
| Passcode | none | the same on every device of yours; WiFi sync does nothing until set |
| This device | as today | |
| Sync over LoRa | On | "No LoRa radio in this phone" / "Channel 'booksync': MISSING" |
| Sync over WiFi | On | phones + COVEY on the same WiFi; sends only when you tap |
| E-readers (KOSync) | **Off** | when On: WiFi `WiPhone-Books-7F3A`, Code `48151623` (editable), Server `http://192.168.4.1` |
| Also when closing a book | Off | only with E-readers On |
| Home server | status | only if `/books/kosync.txt` exists (advanced: COVEY) |
| How to sync... | help | the pages in §7 |

**Sync my place**, in order: (1) LoRa; (2) the WiFi broadcast, 3 sends, then 1.5 s listening for acks;
(3) the home push if `kosync.txt` names a home and the phone is on WiFi — reads first, as today — and the
window WAITS for its verdict (⚠ today `kosyncSyncMyPlace` opens the window BEFORE queuing the push, and the
hotspot path removes the station, which kills the push: the order must flip); (4) if E-readers is On, the
5-minute hotspot window — **every time, at home too** (a KOReader at home cannot resolve `covey.local`, so
suppressing the window when home answers would leave it with nothing). Then a result screen, one line per
path: `Radio: sent` / `WiFi 'HomeNet': 1 heard` / `E-reader: WiPhone-Books-7F3A 4:31, 0 joined`.

With E-readers Off the phone never raises a hotspot. During a hotspot window the phone leaves its WiFi
(SIP off) for at most 5 minutes; the window closes 10 s after a PUT.

Precedence: `kosync.txt` keys still work and WIN over the NVS rows (`hotspot_pass=` overrides the code); the
hotspot code is kept in NVS AND mirrored to the card so an NVS wipe does not change it.

## 3. Scenarios

Yes = proven or straightforward once built · Bench N = needs test N (§6) · No = cannot work.

| | 2nd WiPhone, no LoRa | 2nd WiPhone + LoRa | COVEY | X4 (fork) | KOReader (Kobo/Kindle/Android) | phones only |
|---|---|---|---|---|---|---|
| Home WiFi | WiFi: Yes (home routers rarely isolate). Also today via COVEY's server | LoRa Yes + WiFi | LoRa Yes (proven); WiFi once COVEY listens; home push (bench, against a stub — COVEY's server is OFF until creds) | hub `covey.local`: Bench 5; else hotspot: Bench 1 | hotspot, one-way until §5.11: Bench 1-3 | nothing to set up |
| Cafe / hotel | usually No ("nobody heard") | Yes | LoRa Yes; WiFi usually No | hotspot after the fork's "peer first" change (today the X4 may rejoin the saved cafe WiFi if it was the last one or stronger — bench both orders) | hotspot, picked by hand | nothing |
| Friend's / mobile hotspot | both join it: Bench 7 (iPhone likely, Android varies) | Yes | LoRa Yes; WiFi Bench 7 | phone hotspot: Bench 1 | phone hotspot | nothing |
| Woods | No until Nearby (Bench 10) | Yes (km) | LoRa Yes | phone hotspot: Bench 1 | phone hotspot | nothing |

Readers that do not speak KOSync never sync: Kobo's own reader ({user}el), stock Kindle, Boox's reader, Moon+.
⚠ Do NOT tell phones to join COVEY's hotspot: the phone never joins a hotspot (a join wakes SIP/NTP/the SMS
mirror/the tile downloader into a network with no route — kosync_sync.h).

## 4. What each person does

- **KOReader**: on the phone, E-readers On, tap Sync my place. On the reader: join `WiPhone-Books-7F3A` with
  the code; Tools > Progress sync > Custom sync server `http://192.168.4.1`; **Login** (not Register — the phone
  answers 402); any name/password (needs §5.5 "any account"); the same .epub on both. Each time: tap Sync,
  then "Push progress from this device now" → a card on the phone. **Pull gives "No progress found" until
  §5.11 — never page 1.**
- **X4 (fork)**: Peer Wi-Fi Name `WiPhone-Books` (prefix after the fork change), Peer Wi-Fi Password = the
  phone's code, Peer Server URL `http://192.168.4.1`; at home Hub Wi-Fi + `http://covey.local:8088` if Bench 5
  passes, else COVEY's fixed IP; Sync When Opening/Closing On.
- **Second WiPhone**: the same Passcode on both; tap Sync my place on one.
- **COVEY**: the same passcode in its settings.

## 5. What to build, in order

**Before KOReader appears in any instruction (and before 0.9.79 if the time is there):**
1. **KOReader's page-1 bug.** The phone and COVEY send `"progress":""`; KOReader's `syncToProgress("")` is
   `GotoXPointer("")` → crengine null pointer → **page 1** (main.lua:705-711, lvtinydom.cpp, cre.cpp:939-961).
   - Phone: reply per client. The X4 fork adds a marker header (`X-BookSync: 1`); the marker OR a Basic header
     (stock CrossPoint) gets today's reply; everyone else gets `{}` ("No progress found" on a manual pull,
     nothing on an automatic one). Two shortcuts are WRONG: "`{}` when no Basic" blanks the fork (it omits
     Basic on the peer/hub networks, KOReaderSyncClient.cpp:34-35); "DocFragment for everyone" sends the X4
     to page 0 of the chapter (ProgressMapper.cpp:981-986, bypassing `armPercentLanding`).
   - COVEY: it stores each PUT's `progress` and returns it (kosync.py:699,720-726) — KOReader↔KOReader through
     COVEY must keep working — so COVEY sends `{}` to an unmarked client ONLY when the stored progress is empty.
   - ~20 lines each side + tests. Heap 0.
2. **The window order + rule.** Home push first, window waits for its verdict (or a short timeout); no window
   on a station IP, ever; automatic close windows only off WiFi. Host-tested predicate; more than 40 lines
   because of the wait.
3. **Contract test.** No sync/carrier file may reference `savePosition`, `booksSaveOpenPosition`, `applyPending`,
   `epubLocate`, `store->put` or `positions.cbs` — matched on comment-blanked source (the existing check_*.py
   engine), since comments in kosync*.h already mention `positions.cbs`. `golden_positions.h` stays in the run.

**Core (0.9.80):**
4. Screens: settings rows, library row, result screen, receiver notes, help pages (~450 lines, flash; heap 0).
5. Hotspot identity: `WiPhone-Books-<last 4 MAC hex>`, WPA2 with an editable 8-digit code (NVS + card
   mirror), **any account accepted on the hotspot — x-auth or Basic-only (Readest sends Basic instead of
   x-auth)** — the WPA2 code is the lock, so md5(password) never crosses an open network. GET misses counted:
   "nobody came" splits into nobody joined / joined but never asked / asked for another file. (~120 lines;
   the WPA2 heap cost is Bench 1.)
6. DNS answerer during hotspot windows only, raw socket + PSRAM buffer (NOT Arduino DNSServer: `parsePacket`
   = a 1,460 B internal `new[]`). **Answer ONLY `dns.msftncsi.com`** (KOReader's `isOnline` probe) and NXDOMAIN
   everything else — a catch-all makes iOS/Android/macOS treat the hotspot as a captive portal and flood the
   :80 pump. DHCP must offer the phone as router AND DNS (`OFFER_ROUTER`/`OFFER_DNS`) — KOReader's
   `hasDefaultRoute` needs the router. (~100 lines; one socket while a window is open.)
7. WiFi carrier: the exact CBS1 text on UDP 8083, 3 sends, to 255.255.255.255 AND the subnet .255; receive on
   a raw non-blocking socket with a 256 B PSRAM buffer (never `WiFiUDP`). **Verify with the local passcode
   BEFORE parking** (the 4-slot RAM inbox evicts the oldest, so a stranger's records must not push real
   offers out); count "N records with another passcode" instead. Ack: unconditional, rate-limited, carrying
   a per-boot random token (NOT a MAC fragment — that is a tracking beacon on public WiFi); it reveals only
   whether the network keeps devices apart, never whether a passcode matched (COVEY's no-oracle rule).
   Replay: drop a record older than the last seen from that device (RAM is not enough — a captured record
   raises an old card again after a reboot). (~250 lines; est. <0.5 KB internal, 1 of 10 lwIP sockets.)
8. `kosync.txt` hygiene: unknown keys named on screen; `kosync*.txt` matched by prefix, out of the library,
   "rename it"; password masked in Files (~100 lines).
9. COVEY: background 8083 listener; CBS1 through `unpack_mesh`, verified before parking; LanSync announces
   the CBS1 form; the ack; the §5.1 rule (~120 lines Python + tests).
10. X4 fork: prefix match in `chooseServerUrl`/`onDeviceNetwork` AND `WifiSelectionPatience`/`BookSyncHooks`
    (the exact SSID is used there too); join a visible `WiPhone-Books*` before saved networks at sync time
    (⚠ prefix matching will try strangers' phones one timeout each — prefer the SSID whose saved credential
    exists, then others); a failed automatic sync returns after a short note; no Basic auth to an
    unrecognised plain-http server. ⚠ The Books uploader also hosts `WiPhone-Books` (app_gbc_xfer.cpp:140).

**Later, each behind its bench test:** 11. KOReader chapter pointer `/body/DocFragment[spine+1]/body` for
unmarked clients (Bench 4; crengine's createXPointerV2 resolves element paths; DocFragment indexes EVERY spine
item since 2024-01). 12. AP+STA to keep WiFi/SIP up during a window (Bench 9; est. 3-10 KB internal).
13. Nearby (ESP-NOW) for no-LoRa phones off WiFi (Bench 10; `esp_now_init` once, never deinit; listen ~400 ms
on each channel 1-11, sender sweeps; ~350 lines; est. 1-3 KB).

**Do not build:** mDNS service records/browsing (browsing blocks the loop; every phone is `wiphone`; IDF 3.3
replies carry ID 0 and the phone's parser rejects them); a phone as a KOSync client of another phone; phones
joining hotspots; the passcode as a KOSync password on an open network; a passcode-dependent "heard" reply;
phone support for COVEY's JSON LanSync format; https on the loop; router-reservation instructions unless
Bench 5 fails.

## 6. Bench tests (in this order)

1. **The hotspot has never run on hardware.** Window on phone 1, a laptop joins; read the 15 s heap/largest
   line; open vs WPA2; 20 open/close cycles, `largest` back to idle.
2. DHCP options: on the laptop `ipconfig getpacket en0` → `router` and `domain_name_server` = 192.168.4.1.
3. KOReader passes its online check on the hotspot (desktop or Android build): Login + manual Push with no
   "waiting for network". Also an Android phone WITH mobile data on: a no-internet WiFi may not become the
   default network, so 192.168.4.1 may be routed to mobile data — try `http://192.168.4.1/healthcheck`.
4. What KOReader does with each reply (desktop): `""` → page 1 (the bug); `{}` → "No progress found";
   `/body/DocFragment[N]/body` → the right chapter, in a book with non-linear spine items.
5. The X4 and `covey.local` (Sunday): Hub URL `http://covey.local:8088`, sync at home, again after a COVEY
   reboot. If it passes, nobody needs a reservation.
6. The broadcast reaches an idle phone: 20 taps with the receiver in modem sleep; count cards and acks. Then
   across a mesh router system and 2.4↔5 GHz (`nc -u -b` on 8083).
7. Mobile hotspots: two phones on an iPhone hotspot, then Android; before §5.7 exists, a laptop + COVEY.
8. Heap of the listener: `heap` before/after, an overnight soak, LOOP STALL quiet.
9. AP+STA: raise the softAP on an associated phone; heap; 50 cycles; SIP stays registered; no panic.
10. ESP-NOW: WiFi Off, heap around `esp_now_init`; does the station auto-join a saved network?
11. Header size: the :80 pump aborts headers over 768 B (fine for KOReader/X4; capture one Readest request).

## 7. Privacy — a decision for {user}, not a test

The WiFi broadcast is readable by everyone on that network: it shows the book id, the device name and the
fraction. The record's key is ONE SHA-256 of `"covey-booksync-v1"+passcode` (booksync.cpp:16-20, COVEY
`derive_key`): one captured record allows fast offline guessing of a short passcode, one precomputation covers
every user, and the same key signs LoRa records — so a WiFi capture also lets someone forge LoRa records.
"Use 6+ characters" is advice, not a mitigation; a real fix is a slow KDF or a separate WiFi key, which
changes the wire format for COVEY and LoRa peers (a CBS2). The worst case stays: a guesser raises cards, which
the owner refuses — nothing moves without Go there. Options: (a) ship WiFi sync with the warning and a longer
passcode recommended; (b) default WiFi sync Off; (c) a CBS2 with a slow KDF before WiFi ships.

## 8. In-phone instructions (Books > How to sync..., `s_helpLines` pattern, ~24 chars a line)

1. Syncing your place — "Sync tells your other devices your page. They show a card: Go there, or Stay.
   Nothing moves unless you press Go there. Undo works for 5 min while the book is open."
2. Phone to phone — "Set the same Passcode on both phones. Open the book. Tap Sync my place. Radio works
   anywhere in range, if both phones have one. WiFi works when both are on the same WiFi. The other phone
   shows a card on that book."
3. No radio, no WiFi? — "Turn on your mobile's hotspot. Join it on both WiPhones. Tap Sync. Cafe and hotel
   WiFi often keep devices apart. The phone will say 'nobody heard'."
4. COVEY — "Type the same Passcode on COVEY. Be on the same WiFi, or in radio range."
5. E-reader, once — "Turn E-readers On. Tap Sync my place. On the e-reader: WiFi WiPhone-Books-7F3A, Code
   48151623, Server http://192.168.4.1. Log in (not Register) with any name. Use the same .epub file on both."
6. E-reader, each time — "Tap Sync my place. For 5 min the phone makes its own WiFi and leaves yours. Join
   WiPhone-Books-7F3A on the e-reader and sync. If it sends its page, a card shows here."
7. KOReader — "Tools > Progress sync > Custom sync server. KOReader can send its page here. For now the phone
   does not move KOReader."
8. If it didn't work — "Radio: skipped - no booksync channel. Nobody heard - WiFi keeps devices apart. Another
   passcode - set the same one. Nobody joined - put the reader on the WiFi. Never asked - reader thinks it's
   offline. Other file - copy the same .epub to both."
9. Privacy — "WiFi sync shows your book and phone name to others on that WiFi. Turn it off on public WiFi if
   that matters. Use a long Passcode."

## 9. Facts this rests on (verified in code/sources during the pass)

KOReader: one fixed custom URL, plain http fine, auto-sync off by default, `isOnline()` = a DNS lookup of
`dns.msftncsi.com` (so no sync on a no-internet network without the DNS answerer), Kobo/Kindle ship no mDNS,
`{}` → "No progress found", a manual pull applies without asking. X4 fork: picks its server by the exact SSID
joined, tries the last-connected network first, omits Basic on the peer/hub networks, maps a chapter-start
DocFragment to page 0 of the chapter. COVEY: KOSync server on :8088 bound to the default-route address, no
avahi service record (`covey.local` is only the Pi's hostname), LanSync listens only 15 s after a tap on UDP
8083, stores and returns each PUT's `progress`. Phone: mDNS responder already runs (hostname `WiPhone`/`wiphone`,
no service), its own non-blocking mDNS query rejects ID-0 replies (IDF 3.3 sends them), the hotspot window has
never run on hardware and its heap cost is unmeasured, `esp_now` has zero symbols in the ELF, the only write
of a synced place is `applyPending`, positions live on the SD card (an NVS wipe cannot touch them). Sources and
the agents' reports: the session scratchpad `design_facts_{1,2,3}.md`, `design_{4,5,6}.md`,
`design_synthesis.md`, `design_critic.md`, and KOReader/crengine copies under `ko/`.
