# Mesh history replay — v1 spec (WiPhone → COVEY)

**Agreed with {user} 2026-08-21.** The problem it solves: while COVEY's radio is
lent to the phone (or COVEY is off), covey-ui is blind — and the radio banks
NOTHING for an absent client (D-111, measured). The WiPhone, parked on the same
channels, hears everything. On regaining the radio, COVEY asks the WiPhone what
it missed; the WiPhone replays compacted records with their ORIGINAL timestamps.

Out of scope, deliberately: DMs (the WiPhone hears the packets but cannot
decrypt them — PKC; the phone app's own history is the record for bag-mode DMs);
messages older than the WiPhone's ring (reboot clears it; the reply says so).

## Transport

The `booksync` private channel, both directions — same trust domain, same
encryption, same is-machine-traffic filtering both parsers already apply. All
payloads are UTF-8 text, ≤ 180 bytes to clear Meshtastic's payload ceiling with
margin.

## Wire format (v1 — pinned by generated interop vectors)

Request (COVEY → broadcast on booksync):

    RPL? <t1> <t2> <max>

- `t1`,`t2`: unix seconds, the blind window (t1 = last time COVEY had the radio).
- `max`: most records the asker wants (COVEY sends 40).
- The WiPhone answers only if it holds records in the window. Every WiPhone
  hearing this may answer; v1 assumes one WiPhone (true of this household).

Records (WiPhone → broadcast on booksync), several per packet, one per line:

    RPL <ts> <sender8> <chan> <text…>

- `ts`: unix seconds the WiPhone HEARD it (its NTP clock — this is the
  timestamp COVEY renders, so history lands where it happened, not at sync).
- `sender8`: 8 lowercase hex digits of the sender node (no `!`).
- `chan`: the CHANNEL NAME as the WiPhone knows it (names travel better than
  indexes — index mapping can differ per device; COVEY routes by name and
  falls back to its primary channel for a name it lacks).
- `text…`: rest of line, verbatim (newlines cannot occur in Meshtastic texts).
- Records within a packet are separated by `\n`. Packets are packed greedily
  under the 180-byte budget, OLDEST FIRST (so a cut-short replay is the oldest
  chunk of the story, and the summary says what's missing).

Summary (always last, its own packet or appended if it fits):

    RPL. <sent> <gap>

- `sent`: how many records were replayed.
- `gap`: `1` if the window asked for more than the ring still held (or `max`
  clipped it) — COVEY shows "some older messages beyond recovery"; else `0`.

Parsers on BOTH sides ignore unknown `RPL`-prefixed forms silently (forward
compatibility), and the booksync/chat filters must treat any `RPL`-prefixed
text as machine traffic (never render as chat).

## WiPhone behavior

- A PSRAM ring of the last 64 channel TEXT messages it hears or sends:
  `{ts, senderNode, chanNameHash→name, text[160]}`. Chat channels only —
  machine traffic (booksync/smsmirror records, RPL itself) never enters the ring.
- On `RPL?`: filter ring to [t1, t2], cap at `max`, pack packets oldest-first,
  queue them, DRIP one packet per ~3 s from the main loop (airtime politeness),
  summary last. A new `RPL?` while draining restarts the queue (latest ask wins).
- Serial `replay` command prints the ring occupancy + last request served
  (diagnostics-first rule stays; this feature is otherwise invisible on the
  phone).

## COVEY behavior

- Persist `mesh_last_attached` (unix secs) in prefs, refreshed on a slow tick
  (~60 s) while the backend holds the radio, and stamped at lend_radio().
- On every successful reattach in `_connect_loop`: window = [last_attached-60,
  now]. If window > 120 s, send ONE `RPL?` via the quiet path (no local echo),
  then refresh `mesh_last_attached`. At most one ask per attach.
- Ingest `RPL` records: parse, route by channel NAME (fallback: primary);
  dedup against the target conversation's recent messages by
  (sender-node, |ts−ts'| ≤ 2 s, text) — covers both the RAK's own banked
  deliveries and repeated replays; own-node records (sender == my node) render
  `out=True, sender="me", state="sent"`; others render as normal incoming but
  DO NOT bump unread (a backfill is not a new arrival). All keep the replayed
  `ts`.
- On `RPL.`: toast "Recovered N message(s) from WiPhone" (+ " — some older
  messages beyond recovery" when gap=1). Zero recovered + gap=0 = no toast.

## Interop pinning

`tools/gen_replay_vectors.py` (WiPhone repo) imports COVEY's `replay.py` and
emits `tests/vectors_replay.h`: request strings, packed record packets
(including UTF-8 text, emoji, 180-byte boundary cases), and expected parses.
`tests/test_replay.cpp` runs the WiPhone implementation against them — the same
discipline test_booksync uses, because the wire format that drifts is the wire
format that "worked yesterday".

## Clock truths (learned on the first live content exchange)

- `ts` is UTC epoch seconds. ⚠ On the WiPhone that is `ntpClock.getExactUtcTime()`
  — `getExactUnixTime()` is the LOCAL-shifted epoch (sun.cpp derives the tz
  offset from their difference), and stamping it put every record 7 hours off.
- The asker pads BOTH window edges (COVEY: t1 −60 s, t2 +600 s). The Pi has no
  RTC and either clock can drift; a window ending exactly at the asker's "now"
  silently excluded records a skewed clock stamped in the future — the first
  content exchange served 0 records for exactly this reason, with both records
  sitting in the ring. Dedup makes a generous window free; a tight one costs
  whole messages. Skew beyond ±10 min degrades to the gap-honest behavior and
  waits for v2's GPS time.

## v2 lines (recorded, not built)

- GPS-true time in the summary record once the woods plate ships — lets COVEY
  discipline its RTC-less clock from the replay.
- LAN transport when both devices share WiFi (no airtime).
- Suppress the morning ask when the RAK's own banked queue already delivered
  the window (correlation is the hard part; dedup makes it merely redundant
  today).
