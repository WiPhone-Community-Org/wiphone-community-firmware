# The web flasher, and how updates get discovered — plan and feasibility

**Status: 🎉 LIVE at https://nikguy321.github.io/wiphone-meshtastic/ (2026-08-19),
now driving esptool-js DIRECTLY — see the erase incident below before touching this.**

## ⚠ THE ERASE INCIDENT (2026-08-19) — why this page does not use ESP Web Tools

The first field test wiped the tester's phone: WiFi networks, SIP account, contacts,
message history — everything in SPIFFS. Two separate bugs:
1. The raw SDK bootloader's header says 4MB; `pio upload` patches it to 16MB on the wire.
   The verbatim-writing browser installed a 4MB-believing bootloader that rejected the
   16MB partition table and reset-looped. Fixed: `make_webflasher.sh` ships ONE
   `merge_bin` image — esptool applies its own header patching.
2. **ESP Web Tools erases the whole chip on any "new installation", and with no Improv
   serial in this firmware EVERY install is a "new installation".** `new_install_prompt_
   erase: false` means erase WITHOUT ASKING (verified against their docs). Fixed: the
   page now drives **esptool-js directly** — Install/Update never erases, and a factory
   reset exists only as an explicit, confirm-guarded advanced option. Owner's rule:
   the flasher must never clear user data unless explicitly chosen.

Recovery notes that worked: `pio run -t uploadfs` restores the SPIFFS baseline (fonts,
sounds, default configs); deleting `/roms/smsmirror.since` (Files app) makes the COVEY
mirror re-serve the whole SMS history, rebuilding the message store; WiFi/SIP/contacts
are re-entered by hand (SIP password lives in the VoIP.ms portal).
Serving 0.9.4; page title, manifest and all four parts verified over HTTPS. Publishing a
new version is two commands: `tools/make_webflasher.sh && tools/publish_webflasher.sh`
(the second recreates `gh-pages` as a single orphan commit — binary history never
accumulates). The phone's Settings > Firmware update screen leads with the page.
⚠ Awaiting the first real browser flash ({user}), then it is shareable.

Everything below is analyzed against this codebase's known walls; nothing here re-plans
around the OTA/TLS wall.

## 1. The flasher itself — feasible, prototype done

Exactly the pattern of the T-Deck's installer (jeeab.github.io/t-ui): a static page using
**ESP Web Tools** (esptool-js + Web Serial under the hood). Plug in, click Install, pick
the port, done — no Python, no command line. Chrome/Edge only (Web Serial), HTTPS
required, which GitHub Pages provides.

- `webflasher/index.html` — the page (install button, driver help, honest "what this does").
- `webflasher/manifest.json` + the four binaries — staged by `tools/make_webflasher.sh`
  from the current `pio run` build (bootloader@0x1000, partitions@0x8000,
  boot_app0@0xe000, app@0x10000 — the same offsets `pio run -t upload` writes).
- `new_install_prompt_erase: false` — an update must never offer to wipe NVS/SD-adjacent
  state as the default path.

### Go-live steps (deliberately not done yet — say the word)
1. Enable GitHub Pages on this repo (Settings > Pages > deploy from `main` `/webflasher`,
   or a `gh-pages` branch). The repo is already public.
2. Decide the binary hosting: commit-per-release into `webflasher/` (simple, bloats
   history slowly) **or** GitHub Release assets with absolute URLs in the manifest
   (cleaner; needs `Access-Control-Allow-Origin` care — release assets redirect to a CDN
   that serves CORS headers, and ESP Web Tools handles this; verify once at go-live).
3. Test-flash one phone from the published page before telling anyone.
4. Update the phone's **Settings > Firmware update** screen text to lead with the page URL
   and keep the USB/Python route as the alternative (the screen is flash-resident string
   literals — a 10-line edit held until the URL is final).

## 2. Update discovery — the constraint, and what fits it

**The phone cannot ask GitHub anything.** TLS needs ~33 KB of contiguous internal heap
against ~20-31 KB free (measured twice; see the OTA wall in HANDOFF.md), GitHub is
HTTPS-only, and — per {user}, 2026-08-19 — **COVEY must not be load-bearing here, because
only {user} has one.** So the phone-side "check once a day against the repo" in the original
notes cannot exist for the general user, on any build of this firmware.

What replaces it, in order of value:

- **The flasher page is the check.** It always shows and installs the latest version.
  "Is there an update?" becomes "open the page" — the same muscle memory as installing.
- **The phone knows its own age, offline.** The build embeds `__DATE__`; once NTP has
  synced, the phone can honestly say "this firmware is N months old". A gentle nudge
  (Settings tile hint + a line on the Firmware update screen) after ~6 months costs no
  network, no infra, and is never wrong — it claims *old*, not *outdated*. This delivers
  the "persistent but not annoying" flow from the notes without a server. Small,
  self-contained, worth doing when the page URL is live.
- **(Bonus, {user}-only, optional)** COVEY can relay the real latest-version string over the
  LAN (a `/version` route beside `/sms`), lighting the exact popup/tile/!!! flow from the
  notes on {user}'s phone. Explicitly NOT the mechanism for anyone else.

## 3. What was considered and rejected
- **Phone checks GitHub directly** — the TLS wall. Closed twice; stays closed.
- **A plain-HTTP version endpoint** — would work (a version string is not a secret), but
  requires running a server forever; a dead endpoint would make every phone claim it is
  current. No owned infra, no dependency.
- **DNS TXT record check** — plain UDP, no TLS, elegant; needs an owned domain. Same
  no-infra rule. Worth revisiting only if a domain ever exists anyway.
- **OTA from the browser page over WiFi** — no TLS on the phone, and the OTA transport is
  dead (see `WiPhone/ota.h`). USB is the update path, full stop.
