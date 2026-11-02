/*
 * meshtastic_service.h — WiPhone Meshtastic integration (Phase 3 shell)
 *
 * Background service that owns the mesh radio, an in-RAM store of recent
 * messages and a small node database, and exposes a simple non-blocking API
 * to the WiPhone UI (see app_meshtastic.*).
 *
 * In this first patch the radio layer is STUBBED: setup()/loop() do not touch
 * hardware yet, and send*() echo locally. The real Meshtastic SX1276 PHY
 * (sync word 0x2b, LongFast preset, AES-CTR, protobuf) will be added behind
 * this same API in a later patch without changing the UI.
 */

#ifndef MESHTASTIC_SERVICE_H
#define MESHTASTIC_SERVICE_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>         // offsetof (MeshNode's layout guard)
#include "mesh_txq.h"       // the radio's send queue and relay slots (pure, host-tested)
#include "self_pos.h"       // where this phone is: GPS, the hotspot phone's GPS (pure, host-tested)

#define MESH_MSG_CAP        1000     // total message capacity (allocated in PSRAM)
#define MESH_MAX_PER_CHAT    150     // max stored messages per conversation

/* ── HOW MUCH CHAT HISTORY SURVIVES A REBOOT ───────────────────────────────────────────────
 * The two caps above bound what RAM holds for this session; these bound what is WRITTEN.
 * They are separate numbers because the two costs are nothing alike: the ring is PSRAM and
 * practically free, while a save blocks the whole superloop — keypad, screen and WiFi stack
 * together — for as long as the write takes.
 *
 * MEASURED on this phone (`bench`, 8 KB the same shape a real save has):
 *     SD       48-57 ms
 *     SPIFFS   2599-2845 ms      <- about FIFTY times slower
 *
 * So the card gets a generous slice and a card-less phone gets a small one, rather than a
 * pause anybody can feel. At 248 bytes a message:
 *     SD      40/chat, 200 total  ->  ~48 KB   (a fraction of a second)
 *     SPIFFS  12/chat,  60 total  ->  ~15 KB   (the size the file already was)
 *
 * ⚠ These are FLOORS on what a person sees after a reboot, not on what they see while the
 * phone is running — scrollback within a session is still MESH_MAX_PER_CHAT deep. */
#define MESH_PERSIST_PER_CHAT_SD      40
#define MESH_PERSIST_TOTAL_SD        200
#define MESH_PERSIST_PER_CHAT_FLASH   12
#define MESH_PERSIST_TOTAL_FLASH      60
/* Known-node database size. ⚠ WAS 32, AND THE TABLE WAS A PLAIN MEMBER ARRAY — so it lived in
 * BSS, i.e. the INTERNAL RAM that actually panics this phone, at 84 bytes a node. It is a
 * ps_malloc'd pointer now (same treatment as `messages`), which BOTH gives back ~2.6 KB of
 * internal RAM and makes the cap a question about PSRAM (3.6 MB free) rather than about the
 * scarce pool. {user}, on a public LongFast mesh: "let's see how big we can grow the node list
 * without any big detriments. I do enjoy seeing the mesh."
 *
 * 200 x 84 B = ~17 KB of PSRAM and the same again in the SPIFFS database (partition is 3.6 MB).
 * Saves are debounced and only fire on a real change — a new node, a name, a learned key — not
 * on every packet, so a bigger table does not mean proportionally more flash writes.
 * The binding constraint at this size is not memory, it is a person scrolling the list, which
 * is what MESH_NODE_FAVOURITE below exists to answer. */
#define MESH_MAX_NODES      200      // known-node database size (PSRAM)
#define MESH_TEXT_LEN       234      // max text payload (Meshtastic on-air max ~233)
#define MESH_NAME_LEN        24      // node long/short name buffer
#define MESH_SHORT_NAME_MAX   4      // Meshtastic short name: 4 characters, by convention

/* How often we re-announce our own NodeInfo. Matches stock firmware's
 * node_info_broadcast_secs default of 3 hours, so this phone behaves like the radios
 * around it. Discovery is passive: without this, anything that boots or clears its node
 * DB after us never learns our name. */
#define MESH_NODEINFO_PERIOD_MS     (3UL * 60UL * 60UL * 1000UL)
/* Minimum gap between NodeInfo replies to want_response requests. One "ask everyone to
 * announce" reaches every node at once; answering instantly every time collides on air and
 * wastes the channel. Real firmware damps these deliberately. */
#define MESH_NODEINFO_REPLY_MIN_MS  (60UL * 1000UL)
#define MESH_MAX_CHANNELS     8      // channels we can monitor at once
#define MESH_KEY_LEN         32      // max channel key length (AES-256)

#define MESH_BROADCAST_ADDR  0x00000000u   // "to" value for a channel broadcast

/* ---- GPS freshness: TWO numbers, because they answer two questions --------
 *
 * MESH_GPS_FRESH_MS — "is this number still worth SHOWING?" At 1 Hz NMEA a
 * healthy receiver refreshes every second, so 120 s is 120 consecutive missed
 * epochs: unambiguously "not receiving", while still long enough to ride out a
 * walk under a hemlock and long enough that a 518 ms transmit blind spot is
 * invisible. It replaces the two hardcoded 120000UL sites that used to keep
 * this rule (resolveReference and the My node "on (fix)" label) — they must
 * never be able to disagree about whether the GPS is working.
 *
 * MESH_POS_TX_FRESH_MS — "is it worth OTHER PEOPLE'S trust?" Tighter, because
 * showing a 90-second-old position to the person holding the phone is fine (it
 * is their own position and they know where they are), while spending airtime
 * to tell a hunting party where someone is, from a receiver that has missed 30
 * straight epochs, is a guess dressed as a measurement. Skipping costs nothing:
 * the next slot is minutes away and honest silence beats a confident wrong dot
 * on somebody else's map. */
/* ⚠ ONE number each, kept in self_pos.h since 0.9.82: every source of "where this phone is"
 * (the woods plate's GPS, the hotspot phone's GPS) answers to the same two bars, and the pure
 * picker that applies them is the one the host suite proves (tests/test_selfpos.cpp reads these
 * two lines and fails if they stop being those). */
#define MESH_GPS_FRESH_MS    SELF_SHOW_FRESH_MS
#define MESH_POS_TX_FRESH_MS SELF_TX_FRESH_MS

/* Fix-quality limits live in mesh_pos.h, beside the movement rule and the host tests
 * that prove both. A second copy here is how two thresholds drift apart. */

/* Reference sentinel: "measure everything from THIS PHONE'S OWN GPS", chosen
 * deliberately in Places rather than fallen into. Checked before findWaypoint(),
 * so a waypoint can never shadow it. A real Meshtastic waypoint id is a random
 * uint32, so the collision chance is 2^-32 and its only consequence is that one
 * waypoint could not be picked as a reference. Also unreachable by upgrade: the
 * only writer of `refwp` is setReferenceId() from a real waypoint id or 0, so no
 * previously-shipped firmware can have stored this value. */
#define MESH_REF_GPS         0xFFFFFFFFu

/* What resolveReferenceEx() resolved to — what the screens need to say WHICH answer it was
 * without comparing the display name against "GPS" (the Almanac did; a second measured source
 * would have read as the pin). Since 0.9.82 explicit "Here" (MESH_REF_GPS) means this phone's
 * live fix — its own GPS, else the hotspot phone's, else a Wi-Fi location that two agreeing
 * lookups and the scans since stand behind (named with its size) — and still never the pin. */
enum {
  MESH_REFK_NONE = 0,
  MESH_REFK_GPS,              // a live fix from this phone's own receiver ("GPS")
  MESH_REFK_PHONE,            // a live fix relayed from the hotspot phone ("phone")
  MESH_REFK_LAST_GPS,         // explicit Here, the fix has gone stale ("last GPS")
  MESH_REFK_LAST_PHONE,       // ...ditto from the phone ("last phone")
  MESH_REFK_WAYPOINT,         // a waypoint chosen in Places (its own name)
  MESH_REFK_PIN,              // the hand-set pin ("me")
  MESH_REFK_NET,              // a Wi-Fi location (geoloc.h), named with its size ("Wi-Fi +/-60 m")
};

typedef enum {
  MESH_RADIO_UNINITIALIZED = 0,
  MESH_RADIO_STUBBED,               // service running, no real PHY yet
  MESH_RADIO_READY,                 // real radio initialized (future)
  MESH_RADIO_ERROR,
} MeshRadioState;

/* MeshMessage.flags bits (kept in one byte so the struct size is stable).
 *
 * ⚠ THE STRUCT SIZE IS LOAD-BEARING, WHICH IS WHY THE RECEIPT LIVES IN SPARE BITS HERE
 * RATHER THAN IN A NEW FIELD. saveDb() writes sizeof(MeshMessage) into the header and
 * loadDb() REJECTS THE WHOLE FILE if it does not match — nodes, messages AND waypoints
 * together. Growing this struct to hold a packet id would have silently binned {user}'s
 * message history, his node list, and "Camp" (which the waypoint code explicitly says must
 * survive a reboot) on the first boot after the update. Spare bits cost nothing and the
 * final delivery state persists exactly like MESH_MSG_READ does. */
#define MESH_MSG_OUTGOING  0x01     // sent from this device
#define MESH_MSG_READ      0x02     // the user has viewed this message's thread
/* Delivery receipt, for OUTGOING messages only. Neither bit set = sent, no acknowledgement
 * (yet). These are DELIVERY receipts, NOT read receipts: Meshtastic has no concept of a
 * human having read anything, and the strongest signal the protocol offers is that a radio
 * acknowledged the packet. The UI wording says so rather than implying more. */
#define MESH_MSG_DELIVERED 0x04     // a routing ACK came back for this message
#define MESH_MSG_FAILED    0x08     // a routing NAK came back (no route, no key, ...)

/* How many sent-but-unacknowledged messages can be tracked at once. Acks come back in
 * seconds or never; eight is far more than a person types in that window, and the table is
 * round-robin so a flood costs the oldest receipt rather than the newest. */
#define MESH_ACK_PENDING   8

typedef struct {
  uint32_t from;                    // sender node number
  uint32_t to;                      // MESH_BROADCAST_ADDR for channel, else dest node
  uint32_t timeMs;                  // millis() when stored (real RX time added later)
  uint8_t  channelHash;             // channel this message belongs to
  uint8_t  flags;                   // MESH_MSG_OUTGOING | MESH_MSG_READ
  char     text[MESH_TEXT_LEN];
} MeshMessage;

// MeshNode.pkiFlags bits.
#define MESH_NODE_HAS_KEY       0x01   // pubKey holds this node's X25519 key
#define MESH_NODE_KEY_MISMATCH  0x02   // a LATER NodeInfo carried a DIFFERENT key
/* ⭐ Starred by the user. Two effects, and the second is the one that matters:
 *   1. sorts to the TOP of the node list, ahead of everything else;
 *   2. is NEVER evicted while any unstarred node remains — the same protection a learned
 *      public key earns, for the same reason. A mesh of strangers should not be able to push
 *      out the handful of nodes you actually care about. See upsertNode(). */
#define MESH_NODE_FAVOURITE     0x04

/* How many nodes can be starred. Kept small and in INTERNAL ram on purpose (128 B): this is a
 * hand-curated list, not a mesh-sized one, and it has to be consultable the instant a node is
 * created. */
#define MESH_MAX_FAVOURITES     32
                                       // (kept the first one — trust-on-first-use,
                                       // like stock; Clear nodes re-learns)

typedef struct {
  uint32_t nodeNum;                 // node number (0 = empty slot)
  char     name[MESH_NAME_LEN];     // display name
  uint32_t lastHeardMs;             // millis() of last packet (0 = unknown)
  int16_t  snr;                     // last SNR (x1, stub for now)
  /* PKC: the node's X25519 public key, learned from its NodeInfo (User field 8).
   * Appended at the END so existing readers keep their offsets; saveDb/loadDb
   * migrate the v1 on-flash layout. The derived AES key is deliberately NOT
   * stored here: it lives in a tiny LRU (see pkiCache) so 32 nodes don't cost
   * another kilobyte of internal RAM — RAM is what panics this phone. */
  uint8_t  pubKey[MESH_KEY_LEN];
  uint8_t  pkiFlags;                // MESH_NODE_HAS_KEY | MESH_NODE_KEY_MISMATCH
  /* The precision_bits its last position carried (Position field 23, mesh_pos.h): 0 = absent or
   * full; 1..31 = the sender cut the point to a cell (a stock channel set to 13 bits sends a
   * ~2.9 km one) and the map draws a ring of meshPosPrecisionRadiusM() round it.
   * 🛑 IN THE PADDING BYTE THAT WAS ALREADY HERE (before the int32 below), so sizeof(MeshNode)
   * and every on-card database stay exactly as they were — loadDb() refuses a file whose node
   * size differs, which would cost {user} his nodes, messages and waypoints. The static_asserts
   * under the struct hold it there. Every node table this firmware ever saved has a 0 in this
   * byte (the table is zeroed when allocated, a slot when evicted, a loaded record before it is
   * read), i.e. "not degraded" — exactly what those positions were. */
  uint8_t  posPrec;
  /* Last heard position (port 3 broadcasts — COVEY sends one every 5 min).
   * posHeardMs==0 means never heard. Appended after the v2 fields; DB migrates
   * v1/v2 in place.
   * 🛑 FOR OUR OWN ENTRY THIS IS THE MANUAL PIN AND NOTHING ELSE. The GPS does
   * NOT mirror its fix in here, however tempting that looks: the only writers
   * are setMyPin()/clearMyPin(), and writing it per fix would set dbDirty on
   * every NMEA epoch and drag the database save (~0.1 s on the card, ~1.5-2.8 s
   * on SPIFFS with no card) into a 1 Hz loop. The self row in Nodes reads
   * getGpsFix() live instead. */
  int32_t  latI, lonI;              // 1e-7 degrees
  uint32_t posHeardMs;              // millis() when the position arrived
} MeshNode;
static_assert(offsetof(MeshNode, posPrec) == offsetof(MeshNode, pkiFlags) + 1 &&
              offsetof(MeshNode, latI) == offsetof(MeshNode, posPrec) + 1,
              "MeshNode::posPrec must sit in the padding before latI: the DB's node size is fixed");

// A waypoint shared over the mesh (port 8) — camp, the truck, a stand. The
// reference point for every distance the UI shows.
typedef struct {
  uint32_t id;                      // 0 = empty slot
  int32_t  latI, lonI;
  uint32_t expire;                  // unix seconds, 0 = never
  uint32_t lockedTo;                // owner node: only it may change/expire this (0 = anyone)
  uint32_t heardMs;                 // millis() of last update (0 after a reboot restore)
  char     name[20];                // MESH_WP_NAME_LEN
} MeshWaypoint;

#define MESH_MAX_WAYPOINTS  8

typedef struct {
  char    name[MESH_NAME_LEN];      // channel name (e.g. "LongFast", "Howe group")
  uint8_t key[MESH_KEY_LEN];        // channel key (AES-128 or AES-256)
  uint8_t keyLen;                   // 16 or 32
  uint8_t hash;                     // channel hash (name ^ key), matches packet header
} MeshChannel;

/* What the radio has done this boot — the serial `radio` line. RAM only, zero at boot.
 * ⚠ "own" and "relay" count frames that FINISHED on the air (TxDone), not frames queued. */
struct MeshTxStats {
  uint32_t own;             // our own frames that left whole: texts, beacons, NodeInfo, ACKs
  uint32_t relay;           // flood relays that left whole
  uint32_t timeout;         // frames that never finished (no TxDone in time, or cut by the bench)
  uint32_t relayCancelled;  // relays dropped because somebody else relayed that packet first
  uint32_t refused;         // own frames refused because the queue was full ("radio busy")
  uint32_t airMsTotal;      // COMPUTED time on air of every finished frame (meshLoraAirtimeMs)
  uint32_t lastAirMs;       // the last finished frame, start to the pass that saw TxDone
  uint8_t  lastLen;         // ...its length (meshLoraAirtimeMs(lastLen) is the prediction)
  uint8_t  lastKind;        // ...and what it was (MeshTxKind)
  /* The longest the chip sat DEAF after a frame this boot: seen minus predicted, i.e. TxDone to
   * the pass that put it back in RX (the SX1276 idles in STANDBY meanwhile and hears nothing).
   * One idle pass is ~5 ms; a long pass (a map frame, a WiFi scan) shows up here. A Game Boy
   * game or a legacy whole-file upload no longer counts whole: both call txComplete() on their
   * own cadence (review M2), so this stays the answer to "does a busy screen lose the quick
   * reply to our own frame" - a DM's ACK, or the first relay of our broadcast. */
  uint32_t worstDeafMs;
};

class MeshtasticService {
public:
  MeshtasticService();

  // Lifecycle (called from the WiPhone superloop)
  void setup();                     // init identity + stub radio; safe if called once
  bool loop();                      // non-blocking tick; returns true if a new
                                    // inbound message was stored this call

  // ---- UI-facing API -------------------------------------------------------
  // Messages are ordered newest-first: index 0 is the most recent.
  int                getMessageCount() const;
  const MeshMessage* getMessage(int index) const;   // NULL if out of range

  int                getUnreadTotal() const;        // count of unread messages
  // Mark a conversation's messages read (channel by hash, or DM by peer).
  void               markRead(bool isChannel, uint8_t channelHash, uint32_t peer);

  // Nodes come back in DISPLAY order: starred first, then most-recently-heard.
  int                getNodeCount() const;
  const MeshNode*    getNode(int index) const;      // NULL if out of range
  const MeshNode*    findNode(uint32_t nodeNum) const;
  bool               toggleFavourite(uint32_t nodeNum);   // returns the NEW starred state
  bool               isFavourite(uint32_t nodeNum) const;
  /* Re-sort the node list NOW. The UI calls this when it (re)builds the Nodes screen and at
   * no other time — see the note on getNode(). */
  void               refreshNodeOrder();
  /* ⚠ HOLD THE DATABASE SAVE WHILE SOMEONE IS USING THE PHONE. On SPIFFS (no card) a save
   * blocks the superloop for ~1.5-2.8 s — measured (`bench`) — and everything here is one task,
   * so that freezes the keypad, the screen AND the WiFi stack together. On the card it is
   * ~50-130 ms (since 0.9.19), which is why the gate still exists but is no longer the story.
   * 🛑 THE 0.6-1.5 s 'mesh' STALLS WERE NOT THIS. They were LoRa transmits — the old
   * MeshPhy::send() waited out the frame's time on air, and nothing on that path looked at
   * this flag (see mesh_txq.h). The main loop calls this because it is the only place that
   * can see both the keypad and this service. */
  void               setUiIdle(bool idle) { uiIdle = idle; }
  /* The database lives on the SD card when there is one (about fifty times faster than SPIFFS
   * — measured; see meshFs()). Reported by the main loop rather than read from GUI here, so
   * this service keeps no dependency on the UI.
   *
   * 🛑 CALL THIS BEFORE setup(), NOT ONLY FROM loop(). It is the only writer of the flag
   * meshFs() consults, and setup() is where loadDb()/loadFavourites() run. Until 0.9.19 it
   * was called from loop() alone, so at load time the answer was always "no card": the phone
   * READ SPIFFS every boot while every save WROTE the card, and the whole message history
   * went into a file nothing ever opened. Defined in the .cpp because the flag it sets is a
   * file-static there. */
  void               setCardPresent(bool present);
  bool               dbOnCard() const { return cardIn; }
  /* Which file loadDb() actually read at boot: "SD", "SPIFFS", or "none". A static literal,
   * safe to hold. Exists because the bug above was invisible — the normal load path logged
   * nothing at all, so a phone reading the wrong filesystem looked exactly like a phone with
   * no history. */
  const char*        dbLoadedFrom() const { return dbSource; }
  /* How many messages the NEXT save will write, under the caps above. */
  int                persistedMessageCount() const;
  /* BENCH ONLY (serial `meshdb cut`): leave the card in EXACTLY the state a power cut between the
   * save's remove() and rename() leaves — a whole /meshdb.tmp and no /meshdb.bin — so the boot
   * recovery (meshRecoverTmp, mesh_dbfile.h) can be proven on the phone's own filesystem. The
   * real file is RENAMED aside to /meshdb.cut rather than removed, so a recovery that fails
   * still has the database one rename away — and it REFUSES while an earlier /meshdb.cut is
   * still there (after a failed recovery that file is the database), and on SPIFFS (where no
   * `rm` or computer can reach it). Blocks for one whole save (~0.1 s on the card).
   * NULL = done (the serial command then reboots, before a save can heal it); else the reason. */
  const char*        benchCutSave();

  // ---- Channels ------------------------------------------------------------
  int                getChannelCount() const;
  const MeshChannel* getChannel(int index) const;
  const MeshChannel* findChannelByHash(uint8_t hash) const;
  /* 🔑 "Anything sent here is readable by any Meshtastic radio in range."
   * A PROPERTY OF THE KEY, never of the name or the index. initDefaultChannel()
   * seeds slot 0 as LongFast, but loadChannels() overwrites slot 0 from flash
   * and addChannel() merges by NAME, so a private channel can land at index 0
   * and a public one called 'hunt-group' can land anywhere. keyLen == 0 counts
   * as public because an unencrypted channel is more open than LongFast, not
   * less; a NULL channel counts as public because unknown must fail safe. */
  bool channelIsPublic(const MeshChannel* ch) const;
  /* 'booksync'/'smsmirror' carry traffic between these two devices and have no
   * human audience. Public so the position picker can LABEL them — it lists
   * them rather than hiding them, because a screen whose job is to say
   * truthfully where your location goes must not have gaps in it. */
  bool channelIsMachine(const MeshChannel* ch) const;
  // Parse a Meshtastic channel URL (https://meshtastic.org/e/#... or the raw
  // base64 fragment) and merge its channels in. Returns # of channels added.
  int  applyChannelUrl(const char* url);

  // Send a broadcast on a specific channel (by hash), or a DM (primary channel).
  bool sendChannelMessage(uint8_t channelHash, const char* text);
  bool sendDirectMessage(uint32_t destNode, const char* text);
  /* Why the last send*() returned false, in the words the compose screen shows after
   * "Not sent: " — "too long for the mesh", "radio busy, try again", "no radio", "no
   * channel". A static literal, safe to hold; NULL after a send that went out. Every
   * refusal site sets it, including the pre-check that keeps an oversize DM out of the
   * deferred PKI queue (where it used to fail a tick later, invisibly). Until 2026-09-19
   * the UI ignored the bool and dropped into the thread either way, so a refused message
   * simply vanished. */
  const char* lastSendError() const { return lastSendErr; }

  /* ── THE SEND QUEUE (mesh_txq.h) ─────────────────────────────────────────────────────────
   * ⚠ "SENT" NOW MEANS "QUEUED FOR THE RADIO". send*() returning true means the frame is in
   * the queue; it goes on the air within a pass or two (behind at most one frame already on
   * the air, ~0.5-2.2 s). A frame that then fails to leave — TxDone never comes, or the radio
   * is lost while it waits — fails its message's receipt ("timed out" / "no radio interface"),
   * through the same resolveAck() an inbound NAK uses, so the thread never shows a message as
   * gone that never went. A full queue is refused at once, "radio busy, try again". */
  const MeshTxStats& txStats() const;
  int      txQueued() const;         // own frames waiting (not counting the one on the air)
  /* 🛑 WHAT BECAME OF ONE OWN FRAME — QUEUED, SENT or FAILED (mesh_txq.h, review M1). The map's
   * pin share/unshare/delete, Books' "Sync my place" and the pin row used to take "queued" for
   * "on the air"; they keep the id and ask. UNKNOWN (forgotten after 16 newer frames, or never
   * noted) means "no word" and must be read as the caller's safe default, never as SENT. */
  uint8_t  txOutcome(uint32_t packetId) const;
  /* The packet id of the frame the last successful send queued, 0 after a refusal. Read it
   * IMMEDIATELY after the call that returned true — the next send overwrites it. */
  uint32_t lastQueuedPacketId() const;
  /* Finish the frame on the air (TxDone -> RX, stats, outcome, a timeout's receipt) and start
   * NOTHING. Called by loop() through txPump(), and from the two places that keep loop() from
   * running for minutes: a Game Boy game (WiPhone.ino) and the legacy whole-file upload
   * (app_gbc_xfer.cpp) — review M2. Loop task only; costs one bool test when nothing is on
   * the air. */
  void     txComplete();
  /* May a BACKGROUND originator queue now? Nothing on the air and nothing waiting. Told no,
   * it keeps its deadline / owed flag and asks again next pass. Public since review M3: the
   * serial bench commands that reboot or sleep (`meshdb cut`, `power sleep`, `power lora
   * sleep`) refuse while it is false, so a frame is never cut or stranded from the cable. */
  bool     txPipelineIdle() const;
  int      txQueueCap() const;       // 8, or 2 on the internal fallback, 0 with no queue at all
  int      txQueueMaxDepth() const;  // deepest the own queue has been this boot
  int      relaysPending() const;    // relays waiting out their jitter

  // ---- Neighbour info (portnum 71) -----------------------------------------
  /* Who we hear DIRECTLY, and (optionally) telling the mesh about it so a
   * who-hears-whom map can be drawn. Off by default: it is airtime. */
  uint32_t getNeighborInterval() const { return nbrIntervalSecs; }   // 0 = off
  void     setNeighborInterval(uint32_t secs);                       // persists (NVS)
  int      getDirectNeighborCount() const { return nbrCount; }
  bool     getDirectNeighbor(int i, uint32_t* node, int* snr, uint32_t* ageMs) const;
  uint32_t lastNeighborTxMs() const { return nbrLastTxMs; }
  int      lastNeighborTxCount() const { return nbrLastTxCount; }
  const char* neighborChannelName() const;   // NULL = no private channel to send on
  int      announceNeighborsNow();           // announce on every private channel; count sent

  // ---- Mesh history replay (docs/replay-spec.md) ---------------------------
  // This phone remembers the channel texts it hears; COVEY, blind while its
  // radio is lent, asks `RPL?` on the booksync channel and gets them back.
  int      replayRingCount() const   { return replayCount; }
  int      replayPendingPackets() const;
  uint32_t replayLastServedMs() const { return replayServedMs; }
  int      replayLastServedN() const  { return replayServedN; }

  // ---- This node's identity ------------------------------------------------
  const char* getMyLongName()  const { return myLongName; }
  const char* getMyShortName() const { return myShortName; }
  void setMyName(const char* longName);      // persist (NVS) + re-announce
  /* Set the 4-char short name other radios show. Persisted, and it STOPS the short name
   * following the long one. Pass NULL or "" to go back to deriving it. */
  void setMyShortName(const char* shortName);
  bool isShortNameCustom() const { return shortNameCustom; }
  void announceNodeInfo(bool wantResponse);  // broadcast our NodeInfo now

  // Hop limit for packets we originate (1..7). Persisted in NVS.
  uint8_t getHopLimit() const { return myHopLimit; }
  void    setHopLimit(uint8_t hops);

  // ---- PKC (Meshtastic 2.5+ DM crypto — see mesh_pki.h) --------------------
  // Our X25519 identity. Generated once, persisted in NVS; announced in every
  // NodeInfo so 2.5+ nodes will DM us (they refuse without it).
  bool           pkiIsReady()   const { return pkiReady; }
  const uint8_t* pkiPublicKey() const { return myPkiPub; }

  // ---- Positions & places (see mesh_pos.h) ---------------------------------
  int                 getWaypointCount() const;
  const MeshWaypoint* getWaypoint(int index) const;       // NULL if out of range
  const MeshWaypoint* findWaypoint(uint32_t id) const;

  /* The pin is a USER-DECLARED position ("I'm at camp") — a statement, not a
   * measurement, and it stays that even now the plate has a GPS. Setting it
   * persists to NVS and broadcasts one Position so COVEY's map shows this
   * phone. pinnedAtUnix is 0 when the clock was unknown at pin time. setMyPin
   * returns the ANNOUNCE result: false means the pin stuck locally but never
   * went on air — the UI must not imply otherwise.
   * 🛑 NOTHING ON THE GPS PATH WRITES THE PIN. setMyPin() does an NVS write and
   * an unconditional announce, so "let the GPS update the pin" would mean a
   * flash write and a transmission on every fix. The beacon reads the GPS's
   * own slot (getSelfFix) and the pin is left exactly where the user put it. */
  bool getMyPin(int32_t* latI, int32_t* lonI, uint32_t* pinnedAtUnix) const;
  bool setMyPin(int32_t latI, int32_t lonI);              // persist + announce
  void clearMyPin();
  bool pinAnnounceOk() const { return lastAnnounceOk; }   // queued, and not since failed
  /* 🛑 QUEUED IS NOT SENT (review M1): MESH_TXO_QUEUED while the announce waits for the radio
   * ("sending"), SENT once it left whole, FAILED if it never did or was refused outright. */
  uint8_t pinAnnounceOutcome() const { return lastAnnounceOk ? announceOutcome : (uint8_t)MESH_TXO_FAILED; }

  /* ---- Putting a place of your own on everyone's map (Maps -> Share) -------
   * The Maps app's pins are private marks on a private file (map_pins.h). This is the one
   * door between them and the air, and going through it is always an explicit act: a person
   * highlighted a pin and chose Share.
   *
   * Creates or updates a waypoint locally AND broadcasts it, preferring a private channel
   * exactly as the "I'm here" pin announce does — a location on LongFast is readable by every
   * Meshtastic radio in RF range, and a hunting party is not who that serves.
   *
   * `id` of 0 means "this pin has never been shared, give it an id". Passing back the id from
   * a previous share UPDATES that waypoint on everyone's map instead of adding a second pin
   * beside it, which is the whole reason MapPin carries the id around.
   *
   * The waypoint is locked to this node, so another radio cannot move or delete your camp.
   *
   * Returns the waypoint id (0 = nothing was done), and *onAir reports whether it actually
   * transmitted — a pin that stuck locally but never left the phone must not be shown as
   * shared, which is the same honesty rule setMyPin() follows. */
  uint32_t shareWaypoint(uint32_t id, int32_t latI, int32_t lonI, const char* name,
                         uint32_t expire, bool* onAir, const MeshChannel* ch = NULL);

  /* Take a shared place off everyone's map: broadcast the positionless DELETION marker (see
   * meshWaypointBuild) and drop it here too. Returns whether the marker reached the air; the
   * local removal happens either way, because a person who said "unshare" has said it. */
  bool     unshareWaypoint(uint32_t id, const MeshChannel* ch = NULL);
  /* The channel of that NAME on this phone, or NULL. Pins remember their channel by name
   * (map_pins.h) for the same reason the beacon does: names travel, indexes do not. */
  const MeshChannel* findChannelByName(const char* name) const;

  /* Latched "positions or places changed" signal for the UI — read-and-clear.
   * Separate from loop()'s new-MESSAGE return on purpose: a waypoint arriving
   * must refresh an open Places screen but must NOT buzz, pop up, or light the
   * unread icon. (The book-sync 'receiver shows nothing' lesson, D-089.) */
  bool takePlacesNews() { bool n = placesNews; placesNews = false; return n; }

  /* Which place distances are measured FROM. Three modes, persisted as `refwp`:
   *   MESH_REF_GPS  — explicitly "where I am": this phone's live fix (its own
   *                   GPS, else the hotspot phone's, else a live Wi-Fi location
   *                   named "Wi-Fi +/-N m"; since 0.9.82 - the owner's ranking).
   *                   NEVER falls through to the pin: a stale fix keeps its
   *                   coordinates and renames itself "last GPS"/"last phone",
   *                   and no fix at all returns false so the UI shows no
   *                   distances. A reference frame that moves on its own is
   *                   the failure this mode exists to rule out.
   *   a waypoint id — that place, always.
   *   0 (automatic, the shipping default) — a live fix (GPS, phone, Wi-Fi), else the pin.
   *   An expired Wi-Fi fix is gone, never a "last Wi-Fi" (self_pos.h): the stale fall-back of
   *   explicit Here is a GPS or phone fix only.
   * resolveReference() gives the coords + a display name, false if none exist;
   * resolveReferenceEx() also says which of those it was (MESH_REFK_*). */
  uint32_t getReferenceId() const { return refWaypointId; }
  void     setReferenceId(uint32_t id);
  bool     resolveReference(int32_t* latI, int32_t* lonI,
                            char* name, size_t nameCap) const;
  bool     resolveReferenceEx(int32_t* latI, int32_t* lonI, char* name, size_t nameCap,
                              uint8_t* kind) const;
  /* True when the reference is explicit Here and its fix has gone stale — the
   * Status screen wants the precise age, which resolveReference() only hints at
   * by calling itself "last GPS"/"last phone". (The name is from when the GPS
   * was the only source.) */
  bool     referenceIsStaleGps(uint32_t* ageMs) const;

  /* ---- The woods backplate's GPS (nmea.h) ----------------------------------
   * A LIVE fix (fresh within MESH_GPS_FRESH_MS) slots into resolveReference
   * between the chosen waypoint and the manual pin: "where am I NOW" beats a
   * pin set hours ago, but an explicit waypoint choice still beats both.
   *
   * ⚠ THIS BLOCK USED TO PROMISE "It NEVER touches announce — putting this
   * phone's position on the air stays a deliberate act (the COVEY public-
   * LongFast lesson)." That invariant is deliberately relaxed: {user} asked for
   * the GPS to be able to report to the mesh. The deliberate act did not go
   * away, it MOVED — to arming the feature (setPosInterval) and naming the
   * channel (setPosChannelName), both default-off, both persisted, with the
   * public channel behind a two-press confirm on screen. Nothing here can put
   * a byte on the air that a person did not switch on and aim.
   *
   * setGpsEnabled() mirrors WiPhone.ino's gGpsNmea in here. Without it this
   * service kept answering "GPS" with coordinates from a receiver that had
   * been switched off up to two minutes earlier. It writes nothing and blocks
   * on nothing. */
  /* One completed RMC or GGA from the woods UART (the reader's fix() right after feed() returned
   * true). WHAT it may change is self_pos.h's selfGpsApply, host-tested, and nothing else (0.9.82,
   * the phone-location review of 2026-10-10): only a sentence that itself carries a measured
   * position stamps the fix — so a run of no-fix GGAs with the RMCs lost can no longer re-stamp
   * the last position, with the last good sats/HDOP, as NOW (the map, the Almanac, the beacon and
   * the time zone all believed it). The motion comes only from a stamping RMC and the altitude
   * only from a stamping GGA, each with its own sentence's time (review 2026-09-27). Stamped with
   * millis() here, not the loop pass's `now`: the loop drains a whole UART buffer per pass. */
  void gpsUpdate(const NmeaFix& fx);
  /* The plate's last fix (false when there has never been one), its age, and ITS OWN rating
   * (sats/hdopX10 as the GPS slot holds them, -1 = not said or no fix): the numbers to judge
   * that fix by (meshPosFixUsable - the Almanac's "last GPS"). Not the sky now: getGpsSky. */
  bool getGpsFix(int32_t* latI, int32_t* lonI, uint32_t* ageMs,
                 int* sats, int* hdopX10) const;
  /* The receiver's own last word on its sky: the last GGA's sats and HDOP x10 whatever its
   * quality, -1 where that sentence left a field empty (or before any GGA), with or without a
   * fix - "sats in view" on `gps` and the Almanac's satellite rows. ⚠ NEVER A RATING FOR A FIX:
   * after a POOR fix one empty no-fix GGA says -1/-1 here, which meshPosFixUsable passes
   * (review 2026-10-11); judge a fix by getGpsFix's numbers. */
  void getGpsSky(int* sats, int* hdopX10) const {
    if (sats) {
      *sats = gpsRx.sats;
    }
    if (hdopX10) {
      *hdopX10 = gpsRx.hdopX10;
    }
  }
  /* The Almanac's Position screen (0.9.80): the last fix GGA's own altitude (m, -10000 = unknown)
   * and that GGA's age, the last fix RMC's speed over ground (knots x100, -1 = unknown) and course
   * over ground (degrees true x10, -1 = unknown) and that RMC's age, and how old the fix is. False
   * (nothing written) when there has never been a fix. An age with nothing behind it is
   * UINT32_MAX. A separate accessor rather than a wider getGpsFix(), which has three call sites.
   * It says nothing about the receiver being on: ask isGpsEnabled(). */
  bool getGpsMotion(int* altM, uint32_t* altAgeMs, int32_t* speedKnX100, int32_t* courseX10,
                    uint32_t* motionAgeMs, uint32_t* ageMs) const;
  bool gpsEverFixed() const { return gpsRx.fixMs != 0; }
  void setGpsEnabled(bool on) { gpsEnabled = on; selfSlots[SELF_GPS - 1].enabled = on; }
  bool isGpsEnabled() const { return gpsEnabled; }

  /* ---- WHERE THIS PHONE IS, from every source (self_pos.h, 0.9.82) ----------
   * The woods plate's GPS (gpsUpdate, above — still the receiver's own story for
   * `gps`, the GPS receiver row and the Almanac's satellite details) and the GPS
   * of the phone whose hotspot this one has joined, relayed as NMEA by an app on
   * it (phone_pos.cpp feeds phoneUpdate, through self_pos's one-GGA quality gate).
   * Every "where am I" on a screen or on the air asks getSelfFix(), which is
   * self_pos.h's selfPick(): GPS outranks the phone, 120 s to show, 30 s to send,
   * a poor fix never counts as live.
   * 🛑 RAM ONLY, like the GPS fields: nothing here writes NVS or sets dbDirty (the
   * note on MeshNode's latI — a 1 Hz stream would drag the database save into
   * the loop), and the phone's bytes never reach the clock (self_pos.h). */
  /* `bench` = the fix came from a `phonepos at` address (a Mac running tools/nmea_phone_stub.py
   * standing in for the phone): it is kept as a BENCH fix, exactly like `pos inject` — drawn,
   * never sent by the schedule, never to a public channel — so a test stream can never put a
   * made-up position on the air as a measurement. */
  void phoneUpdate(int32_t latI, int32_t lonI, int sats, int hdopX10, bool bench = false);
  /* "Phone location: auto | off" (NVS wpmesh/phloc; phone_pos.h owns the writes). Auto on a new
   * phone; the first boot of an UPGRADED one decides once (loadPosSettings): off when a beacon
   * was armed with the GPS off or to a public channel, so nothing starts going out unasked. */
  void setPhoneEnabled(bool on);
  bool isPhoneEnabled() const { return selfSlots[SELF_PHONE - 1].enabled; }
  /* The best LIVE fix (selfPick). forTx: the 30 s bar and NEVER a `pos inject` fix — the
   * scheduled beacon's question. Otherwise the 120 s bar, bench fixes included — the
   * screens'. SELF_NONE (out->src too) when nothing qualifies. */
  int  getSelfFix(SelfFix* out, bool forTx) const;
  /* The most recently stamped fix of any enabled source, whatever its age or quality (the
   * grey "last GPS"/"last phone" marker): out->state says LIVE/OLD/POOR on the 120 s bar. */
  int  getSelfNewest(SelfFix* out) const;
  /* One source's own fix, whatever its age or quality, bench fixes included (`pos`, the
   * Almanac's "last ..." place). False when that source is off or has no fix. */
  bool getSourceFix(int src, SelfFix* out) const;
  bool anySourceEnabled() const { return selfAnyEnabled(selfSlots); }
  /* The slots themselves, read-only, for the clock's time zone (tz_auto.h, 0.9.82): Auto asks
   * its OWN question of them (tzaPickFix - the 30 s bar, never a bench fix, an iPhone fix only
   * far from any zone line), which is none of the questions above. */
  const SelfSlot* getSelfSlots() const { return selfSlots; }
  /* May anything be SENT from a source that is switched on? GPS, phone, and the Wi-Fi slot only
   * with "map + mesh" (self_pos.h, selfAnyEnabledForTx): "map only" shows a Wi-Fi location on
   * this phone's map without the beacon counting it as something to put on the air. */
  bool anySourceEnabledForTx() const { return selfAnyEnabledForTx(selfSlots); }
  /* ---- Wi-Fi location (geoloc_net.h, 0.9.82): the SELF_NET slot -------------
   * "Wi-Fi location: off | map only | map + mesh" (NVS wpmesh/netloc, geoloc_net.cpp owns the
   * writes): the slot is ENABLED unless off and SENDABLE only with "map + mesh" (noTx otherwise -
   * self_pos.h's MAP_ONLY). OFF forgets the fix at once. geoloc_net.cpp is the slot's only
   * writer (geoloc.h's fold and geoOnScan, through netSlot()); RAM only like every slot. */
  void      setNetLocSetting(int setting);
  SelfSlot* netSlot() { return &selfSlots[SELF_NET - 1]; }
  // A position or place changed outside this service (a Wi-Fi fix came or went): open lists redraw.
  void      notePlacesChanged() { placesNews = true; }
  /* `pos inject <gps|phone> ...` (bench, RAM only): write a fix into that source's slot,
   * flagged injected — drawn and measured from like a real one, NEVER sent by the schedule,
   * and `pos now` with one logs INJECTED, sends it as MANUAL (not a measurement) and refuses a
   * public channel. The next real fix of that source replaces it. False when `src` is not GPS
   * or PHONE. */
  bool injectSelfFix(int src, int32_t latI, int32_t lonI, int sats, int hdopX10);
  int  clearInjectedFixes();               // `pos inject clear`: how many were dropped

  /* ---- Periodic position beacon -------------------------------------------
   * OFF by default, with NO channel chosen by default. Three separate things
   * must be true before one byte goes out: a location source is on (the GPS
   * receiver, or Phone location), an interval is chosen, and a channel is
   * named. See loop()'s beacon block.
   *
   * setPosInterval() re-arms (posNextTxMs = 0) exactly as setNeighborInterval
   * does, so the keypress that switches reporting on NEVER transmits — the
   * first beacon is a full period away. For a neighbour map that is politeness;
   * for a live location it is a privacy property. */
  uint32_t    getPosInterval() const { return posIntervalSecs; }   // 0 = off
  void        setPosInterval(uint32_t secs);                       // persists (NVS)
  const char* getPosChannelName() const { return posChanName; }    // "" = unset
  void        setPosChannelName(const char* name);                 // persists (NVS)
  /* The chosen channel, re-resolved BY NAME every time. NULL when unset or no
   * longer on this phone. Names travel and indexes do not (see the ReplayHeard
   * note); a stale index would aim a live location beacon at the wrong channel
   * the first time applyChannelUrl() rewrites the table. */
  const MeshChannel* getPosChannel() const;
  /* Was the chosen channel public AT THE MOMENT IT WAS CHOSEN? The one hard
   * refusal in this feature compares that against what the channel is NOW: a
   * channel that has since been re-keyed to the stock default would otherwise
   * start broadcasting a person's location in the clear, months after they
   * armed it, in complete silence. A user who deliberately confirmed LongFast
   * has this true and is not overridden. */
  bool        posChannelWasPublic() const { return posChanWasPublic; }
  uint32_t    getPosLastTxMs() const { return posLastTxMs; }       // 0 = never
  bool        posLastSendOk()  const { return posLastOk; }         // queued, and not since failed
  /* The last beacon's frame (mesh_txq.h MeshTxOutcome): QUEUED = "sending", SENT = it left whole,
   * FAILED = refused or never left. Rows that say "sent N min ago" ask this first (review M1). */
  uint8_t     posLastOutcome() const { return posLastOk ? posOutcome : (uint8_t)MESH_TXO_FAILED; }
  /* True when a slot has come round and is still waiting for a fix worth sending. */
  bool        posBeaconDue()   const { return posDue; }
  /* Why nothing is being sent right now — NULL when reporting is actually
   * running (or is off, which the caller can see from getPosInterval()). The
   * string is a static literal, safe to hold. */
  const char* posBlockedReason() const;
  int         getPosSkipRuns() const { return posSkipRuns; }
  /* Bench only (serial `pos now`): build and queue one beacon immediately,
   * ignoring the interval and the movement gate but NOT the safety rules.
   * (It used to BLOCK ~518 ms in meshPhy.send(); since 0.9.79 nothing does.)
   * Unlike the schedule it may send a `pos inject` fix — logged INJECTED and
   * sent as MANUAL. NEVER to a public channel, whatever the fix: the cable is
   * not the on-screen consent (0.9.82). *usedInjected says whether it did. */
  bool        sendSelfPositionNow(bool* usedInjected = NULL);
  // Why `pos now` would not send right now (the forced send's own gate), or NULL.
  const char* posNowBlockedReason() const;

  // Maintenance (persisted immediately).
  void clearMessages();                      // wipe all stored messages
  void clearNodes();                         // wipe node DB (keeps this node)
  bool forgetNodeKey(uint32_t nodeNum);      // serial `pki forget`: re-trust ONE node's next key

  // ---- Status --------------------------------------------------------------
  MeshRadioState getRadioState() const { return radioState; }
  const char*    getRegion()     const { return region; }
  const char*    getChannelName()const { return channelName; }
  const char*    getModemPreset()const { return modemPreset; }
  uint32_t       getMyNodeNum()  const { return myNodeNum; }

private:
  void      storeMessage(uint32_t from, uint32_t to, uint8_t channelHash, const char* text, bool outgoing);
  MeshNode* upsertNode(uint32_t nodeNum, const char* name);
  void      seedStubData();         // demo node + welcome message (stub only)
  bool      seenPacketId(uint32_t id);   // dedup rebroadcasts of the same packet
  void      saveDb();               // persist nodes + messages (SD when there is a card)
  void      loadDb();               // restore nodes + messages (SD when there is a card)
  /* Fill `keep` (one byte per message, oldest first) with the retention decision, and return
   * how many are marked. `keep` may be NULL to just count. See mesh_retain.h. */
  int       selectPersisted(uint8_t* keep) const;
  const char* dbSource;             // which file loadDb() read; see dbLoadedFrom()
  uint32_t  chatKeyOf(uint32_t from, uint32_t to) const;   // conversation id (0 = broadcast)
  void      removeMessageAt(int idx);

  /* ---- Delivery receipts ---------------------------------------------------
   * A routing ACK/NAK identifies the message it answers by PACKET ID, but a packet id has
   * nowhere to live on MeshMessage (see the flags note above — growing the struct discards
   * the whole database). So the association is held here, in RAM, only for the seconds
   * between sending and the ack coming back.
   *
   * ⚠ IT IS KEYED ON timeMs, NOT ON THE MESSAGE'S INDEX. Indices are not stable: the store
   * compacts on removeMessageAt(), so the per-chat and global caps can shift every message
   * down while an ack is still in flight, and the receipt would land on somebody else's
   * message. timeMs is millis() at store time and two texts cannot be typed in the same
   * millisecond.
   *
   * Pending entries do NOT survive a reboot, and that is the honest behaviour: an ack for a
   * message sent before a restart can no longer be attributed to it. The RESOLVED state does
   * survive, because it lives in MeshMessage.flags, which is persisted. */
  struct PendingAck {
    uint32_t packetId;              // 0 = free slot
    uint32_t msgTimeMs;             // identifies the MeshMessage this answers
  };
  PendingAck pendingAcks[MESH_ACK_PENDING];
  uint8_t    pendingAckNext;        // round-robin; oldest unanswered is overwritten first
  uint32_t   lastStoredTimeMs;      // timeMs of the message storeMessage() just appended

  void      notePendingAck(uint32_t packetId, uint32_t msgTimeMs);   // after a successful TX
  void      resolveAck(uint32_t packetId, uint8_t errorReason);   // on an inbound ROUTING
  void      setMessageReceipt(uint32_t msgTimeMs, bool delivered, uint8_t err = 0);

  // Channels
  void      initDefaultChannel();   // channel 0 = LongFast
  bool      addChannel(const char* name, const uint8_t* key, uint8_t keyLen);   // merge
  void      loadChannels();         // from SPIFFS
  void      saveChannels();         // to SPIFFS

  /* ⭐ STARRED IDS LIVE IN THEIR OWN TINY FILE, NOT ONLY AS A BIT IN THE BIG DATABASE.
   * The node table is machine data — every entry in it is rediscoverable by listening to the
   * mesh. The stars are the one thing in there that is NOT: they are {user}'s judgement about
   * which nodes matter, and nothing can regenerate them. So they get their own durability,
   * the same way channels do, instead of riding along in a ~250 KB file that is rewritten
   * whenever any node is heard.
   *
   * It also means a star SURVIVES EVICTION. The list is the source of truth and the flag on
   * MeshNode is a cache of it, re-applied whenever a node is created — so a starred node that
   * got pushed out of a full table comes back starred (and immediately eviction-proof again)
   * the moment it is heard from.
   *
   * SPIFFS, deliberately, NOT the SD card: this is 4 bytes per star, so size is irrelevant,
   * and SPIFFS is always mounted whereas a card can be absent. */
  uint32_t  favIds[MESH_MAX_FAVOURITES];
  uint8_t   favCount;
  bool      favDirty;               // set by toggleFavourite, drained by loop()
  bool      uiIdle;                 // main loop's verdict: is nobody touching the phone?
  bool      cardIn;                 // main loop's verdict: is an SD card seated? (see meshFs)

  /* ── THE DATABASE SAVE IS SPREAD ACROSS LOOP PASSES ─────────────────────────────────────
   * MEASURED 2026-08-24: SPIFFS on this part writes at about 6 KB/s, so saving the database
   * in one go blocked the superloop for ~1.5 s — and one task means the keypad, the screen and
   * the WiFi stack all stopped with it. That is {user}'s "menus freeze and WiFi drops".
   * ⚠ HISTORY SINCE 0.9.19: with a card in, the database is on SD and a whole save is ~50-130 ms
   * (the `bench` numbers in the .cpp). The 0.6-1.5 s 'mesh' stalls that were still blamed on
   * this afterwards were LoRa transmits (mesh_txq.h).
   * The cost is per-BYTE, not per-call (batching ~55 writes into 2 changed almost nothing), so
   * the total is irreducible — but it does NOT have to be paid all at once.
   * The image is built into PSRAM in one pass (memcpy only, no I/O), then dribbled to flash a
   * bounded chunk per loop pass. Snapshotting first also makes the write atomic in a second
   * sense: nodes and messages cannot change halfway through and tear the file.
   * ⚠ A save in flight holds an open File across passes. That is fine — it writes to the TEMP
   * file, and the rename only happens on completion, so an interrupted save leaves the old
   * database untouched exactly as before. */
  uint8_t*  saveBuf;                // PSRAM snapshot of the database image
  uint32_t  saveLen, saveOff;       // image size, and how much has reached flash
  bool      saveActive;             // (the open File itself is a static in the .cpp, so this
                                    //  header need not drag FS.h into every consumer)
  void      saveDbStep();           // one bounded chunk; called from loop()
  void      loadFavourites();       // from SPIFFS, into favIds
  void      saveFavourites();       // to SPIFFS, atomically
  void      applyFavouriteFlag(MeshNode* n) const;   // stamp the cached bit from favIds
  MeshChannel channels[MESH_MAX_CHANNELS];
  int         channelCount;

  // Messages, oldest at [0] and newest at [msgCount-1]. Allocated in PSRAM
  // (capacity MESH_MSG_CAP); capped at MESH_MAX_PER_CHAT per conversation.
  MeshMessage* messages;
  int          msgCount;

  MeshNode*   nodes;                // PSRAM (see MESH_MAX_NODES); internal fallback
  /* Display order, rebuilt lazily: starred first, then most-recently-heard. An INDEX rather
   * than sorting `nodes` itself, because findNode()/upsertNode() hand out MeshNode* that
   * callers hold across statements — re-sorting the array under them would dangle. */
  uint16_t*   nodeOrder;
  bool        orderDirty;
  int         orderCount;           // nodeCount the current order was built for
  void        rebuildNodeOrder();
  int         nodeCount;

  MeshRadioState radioState;
  const char*    region;
  const char*    channelName;
  const char*    modemPreset;
  uint32_t       myNodeNum;
  char           myLongName[MESH_NAME_LEN];   // editable, persisted in NVS
  char           myShortName[8];              // up to 4 chars (Meshtastic)
  bool           shortNameCustom;             // user set it: do NOT re-derive from long
  uint8_t        myHopLimit;                  // hop limit for originated packets

  /* NodeInfo beaconing. Discovery on a LoRa mesh is PASSIVE — a node is learned only when
   * it transmits — so a node that never re-announces is invisible to anything that arrives
   * after it. See the periodic block in loop(). */
  uint32_t       nextNodeInfoMs;              // when the next periodic announce is due
  uint32_t       lastNodeInfoTxMs;            // damps replies to want_response requests

  // ---- PKC state -----------------------------------------------------------
  uint8_t  myPkiPriv[MESH_KEY_LEN];           // X25519 private key (NVS "pkipriv")
  uint8_t  myPkiPub[MESH_KEY_LEN];            // matching public key (announced)
  bool     pkiReady;                          // keypair loaded/generated OK

  /* Derived-AES-key cache, 2 entries. One X25519 derive costs ~3 KB of
   * TRANSIENT stack and tens of ms, so it is only ever run at SUPERLOOP depth
   * (RX key-learn, boot pre-warm, the pending-DM drain in loop()) — never from
   * GUI code. sendDirectMessage at GUI depth uses cache hits only and QUEUES
   * on a miss. Two entries cover the real fleet (COVEY + one more). */
  struct PkiCacheEntry {
    uint32_t node;                            // 0 = empty
    uint8_t  key[MESH_KEY_LEN];               // SHA-256(X25519 shared secret)
  };
  PkiCacheEntry pkiCache[2];
  uint8_t       pkiCacheNext;                 // round-robin victim
  void loadOrCreatePkiKeys();                 // NVS load, or generate + store
  bool pkiKeyForNode(const MeshNode* n, uint8_t keyOut[MESH_KEY_LEN]);  // derive (superloop only!)
  bool pkiKeyCached(uint32_t nodeNum, uint8_t keyOut[MESH_KEY_LEN]) const;
  void pkiCacheClear() { memset(pkiCache, 0, sizeof(pkiCache)); pkiCacheNext = 0; }
  void pkiLearnKey(MeshNode* n, const uint8_t* key);   // TOFU store + eager derive

  /* DMs queued because their AES key wasn't cached at send time (GUI depth).
   * Drained by loop() one per tick: derive there, send, local-echo. */
  struct PendingDm {
    uint32_t dest;
    char     text[MESH_TEXT_LEN];
    bool     active;
    /* This DM is echoed into the store at QUEUE time but does not reach the air until
     * loop() drains it a tick later, by which point lastStoredTimeMs has moved on. Carry
     * the identity across so the receipt still lands on the right message. */
    uint32_t msgTimeMs;
  };
  PendingDm pendingDm[2];

  // Packet ids of DMs we stored and ACKed: a retransmission of one of these is
  // re-ACKed (our ACK may have been lost) but never re-stored.
  uint32_t recentAckIds[8];
  uint8_t  recentAckPos;

  // ---- Positions & places state --------------------------------------------
  MeshWaypoint waypoints[MESH_MAX_WAYPOINTS];
  int          waypointCount;
  int32_t      myPinLatI, myPinLonI;      // 0/0 + myPinSet=false = no pin
  bool         myPinSet;
  uint32_t     myPinAtUnix;               // when pinned (0 = clock was unknown)
  uint32_t     refWaypointId;             // 0 = use the pin
  bool         placesNews;                // latched: positions/waypoints changed
  bool         lastAnnounceOk;            // last announceMyPosition() was queued, and has not failed
  uint32_t     announceTxId;              // ...its frame's packet id (0 = none), for noteOwnOutcome()
  uint8_t      announceOutcome;           // ...and what became of it (MeshTxOutcome)
  const char*  lastSendErr;               // see lastSendError(); a literal or NULL
  uint32_t     nextWpSweepMs;             // next local waypoint-expiry sweep
  /* The woods plate's receiver (self_pos.h, SelfGpsRx): its last fix — position, time, rating,
   * the stamping GGA's altitude and the stamping RMC's motion, each with its own stamp — and its
   * own last word on the sky (the last GGA's sats/HDOP). Written ONLY by selfGpsApply, from
   * gpsUpdate(). ~70 B, RAM only. */
  SelfGpsRx    gpsRx;
  /* Mirror of WiPhone.ino's gGpsNmea, set by setGpsEnabled() from the two
   * places that toggle it (the My node row and serial `gps on|off`). Read here
   * rather than extern'd because gGpsNmea is not visible in this translation
   * unit, and because reaching into the .ino's globals from a service is how
   * the next such flag ends up out of sync too. Seeded in setup() from the same
   * NVS key the .ino reads, so there is no boot-order dependency between them. */
  bool         gpsEnabled;
  /* Every source's last fix, indexed src - 1 (self_pos.h). The GPS slot mirrors gpsRx's fix,
   * written in gpsUpdate() (or by `pos inject gps`); the PHONE slot is written
   * only by phoneUpdate() and the bench; SELF_NET (Wi-Fi location, geoloc.h) only by
   * geoloc_net.cpp, switched by setNetLocSetting(). ~110 B of BSS. */
  SelfSlot     selfSlots[SELF_SRC_COUNT];
  /* What the beacon SENT before, for the Wi-Fi slot's two rules (self_pos.h, SelfNetTxMem): the
   * last Wi-Fi cell and the fix it came from (keep the cell while the fix stays put - review item
   * 15), and the last GPS/phone beacon (a precise fix of the last hour wins). ~36 B, RAM only. */
  SelfNetTxMem netTx;

  /* ---- Periodic position beacon state -------------------------------------
   * ~48 bytes of BSS, no heap, nothing allocated per broadcast. */
  uint32_t     posIntervalSecs;           // 0 = off (NVS "posint")
  char         posChanName[MESH_NAME_LEN];// target channel NAME, "" = unset (NVS "poschan")
  bool         posChanWasPublic;          // was it public when picked? (NVS "pospub")
  uint32_t     posNextTxMs;               // 0 = not armed yet (wait a full period)
  int32_t      posLastLatI, posLastLonI;  // last position actually TRANSMITTED
  bool         posLastValid;              // ...and whether there is one
  int          posSkipRuns;               // consecutive slots the movement gate ate
  uint32_t     posLastTxMs;               // millis() of the last beacon (0 = never)
  bool         posLastOk;                 // queued, and not since failed (see posLastOutcome())
  uint32_t     posTxId;                   // the last beacon's packet id (0 = none)
  uint8_t      posOutcome;                // ...and what became of it (MeshTxOutcome)
  /* ⚠ THE SLOT IS OWED, NOT SPENT. Set when the interval comes round; cleared only when the
   * beacon is actually resolved (sent, or skipped because the phone has not moved). A slot
   * that finds no usable fix stays owed and fires the moment one arrives — see the long note
   * at the tick. Never more than one is owed, so a long blind spell cannot burst. */
  bool         posDue;
  void loadPosSettings();                 // NVS, once, in setup()
  /* The one place a Position payload is built and handed to the radio. Both the
   * manual pin announce and the GPS beacon go through it, so there is exactly
   * one set of bytes and one log line to reason about. */
  bool sendPositionOn(int32_t latI, int32_t lonI, const MeshChannel* ch, const char* why);
  /* ...and with where a MEASURED fix came from (mesh_pos.h's location_source / precision_bits;
   * 0/0 sends meshPosBuild's bytes, which is what sendPositionOn does) and field 4's time (0 =
   * left out; a Wi-Fi fix's is its confirming scan's, self_pos.h's selfFixSendTime). */
  bool sendPositionOnEx(int32_t latI, int32_t lonI, const MeshChannel* ch, const char* why,
                        uint8_t locSrc, uint8_t precBits, uint32_t timeUtc);
  // posBlockedReason() for a purpose (SELF_PICK_TX, plus SELF_PICK_INJECTED for `pos now`).
  const char* posBlockedReasonFor(unsigned flags) const;
  /* The one place a Waypoint payload is built and handed to the radio, mirroring
   * sendPositionOn so both have one set of bytes and one log line. */
  bool sendWaypointOn(const uint8_t* payload, size_t len, const MeshChannel* ch,
                      const char* why, const char* name);
  /* The private channel the announce path prefers; NULL is impossible (it falls back to
   * channels[0]) but the caller still checks, because channelCount can be 0. */
  const MeshChannel* announceChannel() const;
  /* The beacon (safety rules inside): the scheduled one (forced false: never a bench fix) or
   * `pos now` (forced true). */
  bool sendSelfPosition(bool forced, bool* usedInjected = NULL);

  void upsertWaypoint(uint32_t id, int32_t latI, int32_t lonI,
                      uint32_t expire, uint32_t lockedTo, const char* name);
  void loadPin();                          // NVS
  bool announceMyPosition();               // one Position broadcast (private ch preferred)

  void loadMyName();                          // load from NVS or derive default
  void deriveShortName();                     // short = first chars of long

  // Flood-routing rebroadcast slots (CLIENT role: relay others' packets). See mesh_txq.h.
  MeshRelaySlot rebroadcast[MESH_RELAY_SLOTS];
  void scheduleRebroadcast(const uint8_t* pkt, uint8_t len);

  /* ── THE TRANSMIT PUMP (mesh_txq.h) ──────────────────────────────────────────────────────
   * The FIRST thing loop() does: service the frame on the air (TxDone -> back to RX; a
   * timeout fails an own frame's receipt), then start at most ONE frame — the own queue's
   * front, else the earliest-due relay. 🛑 It must run before the 5 s health check and before
   * poll(): a long pass can come back to a chip in STANDBY with TxDone latched, and finishing
   * that frame first is what puts it back in RX before anything listens. Its first half is
   * txComplete() (public: a game and the legacy upload call it where this loop cannot run). */
  void txPump();
  /* The radio is gone: every queued own frame's receipt fails with `err`, relays are dropped. */
  void txDropAll(uint8_t err);
  /* Record an own frame's outcome (the ring, and the pin-announce / beacon rows it names). */
  void noteOwnOutcome(uint32_t packetId, uint8_t outcome);
  /* A NodeInfo somebody asked for (want_response) that is waiting for an idle pipeline. The
   * damping decision is still taken when the request arrives (lastNodeInfoTxMs). */
  bool nodeInfoOwed;

  // Recently-seen packet ids, to drop mesh rebroadcasts of the same packet.
  uint32_t recentPktIds[16];
  int      recentPktPos;

  // ---- Neighbour info state -------------------------------------------------
  /* DELIBERATELY NOT PERSISTED and deliberately not part of MeshNode: who is
   * within direct earshot is a LIVE property (it changes as you walk), and
   * adding a field to MeshNode would force a node-DB migration for data that
   * is stale the moment it is restored. */
  struct DirectNeighbor {
    uint32_t nodeNum;
    uint32_t heardMs;
    int8_t   snr;
  };
  DirectNeighbor nbr[16];
  int            nbrCount;
  uint32_t       nbrIntervalSecs;    // 0 = off (NVS "nbrint")
  uint32_t       nbrNextTxMs;
  uint8_t        nbrPendingMask;    // channels still to announce this cycle (bit = index)
  uint32_t       nbrDripMs;         // next send in the current cycle
  uint32_t       nbrLastTxMs;        // millis() of the last announce (0 = never)
  int            nbrLastTxCount;     // neighbours in it
  void neighborHeard(uint32_t node, int8_t snr, bool direct);
  bool announceNeighborsOn(const MeshChannel* ch);
  const MeshChannel* neighborChannel() const;

  // ---- Mesh history replay state (docs/replay-spec.md) ---------------------
  /* The ring lives in PSRAM (setup(); ~12 KB) — NOT internal heap, which this
   * phone has none of to spare. NULL = allocation failed = feature inert. */
  struct ReplayHeard {
    uint32_t rxUnix;                    // WiPhone clock when heard (never 0 in ring)
    uint32_t sender;
    char     chan[16];                  // channel NAME (names travel; indexes differ)
    char     text[160];
  };
  ReplayHeard* replayRing;
  int          replayHead;              // next write slot
  int          replayCount;
  /* Reply packets, built at request time, dripped one per REPLAY_TX_GAP_MS —
   * LoRa airtime politeness. A new request restarts the queue (latest wins). */
  char*        replayPkts;              // PSRAM slab: REPLAY_MAX_PKTS × REPLAY_PKT_STRIDE
  int          replayPktCount;
  int          replayPktNext;
  uint32_t     replayNextTxMs;
  uint8_t      replayChanHash;          // the channel the request arrived on
  uint32_t     replayCoverFrom;         // unix secs this ring's coverage began (0 = none yet)
  uint32_t     replayServedMs;          // millis() of the last served request (0 = never)
  int          replayServedN;           // records in that reply
  void replayCapture(uint32_t sender, const MeshChannel* ch, const char* text);
  void replayHandleRequest(const MeshChannel* ch, const char* text);
  void replayPump();

  bool     dbDirty;                 // unsaved changes pending
  uint32_t lastSaveMs;              // last persistence write (millis)

  bool     initialized;
};

// Single global instance (defined in meshtastic_service.cpp).
extern MeshtasticService meshService;

#endif // MESHTASTIC_SERVICE_H
