/*
 * map_pins.h — the pins you drop on the map, and the one line of text each one is on the card.
 *
 * ── WHY PINS ARE NOT WAYPOINTS ───────────────────────────────────────────────────────────
 * The mesh already has a shared place list: MeshWaypoint, eight slots, broadcast on port 8,
 * owner-locked, expiring, and interoperable with COVEY's map (meshtastic_service.h). A map
 * pin is NOT that. It is a private mark on your own map — "park here", "the gate", "blood
 * trail starts" — and there are more than eight of those on any real day out.
 *
 * So pins live in a plain text file on the SD card and cost the mesh database nothing:
 *   - the eight-slot waypoint table stays exactly the size the on-flash DB format expects
 *     (growing it is a migration, and loadDb() REJECTS a file whose sizes do not match),
 *   - a pin never goes on the air until someone chooses to share it, which is the same
 *     deliberate act the position beacon and the "I'm here" pin are built around,
 *   - and a shared pin keeps the waypoint id it was given, so editing it later updates the
 *     waypoint on everyone's map instead of creating a second one beside it.
 *
 * ── ONE LINE PER PIN, AND NOTHING CLEVER ─────────────────────────────────────────────────
 *     # comment
 *     <latI>,<lonI>,<sharedId>,<channel>,<name>
 * The channel is the NAME of the mesh channel the pin was shared on (empty if never shared),
 * so a rename re-send and "Take it off the mesh" go out where the pin already is — a
 * retraction on a different channel than the share strands the waypoint on every other radio.
 * Names travel and indexes do not: a channel URL applied later re-numbers the table.
 * A line with only three commas is the older four-field form and still loads (channel "").
 * latI/lonI are 1e-7 degrees — the SAME fixed point the Meshtastic wire format uses, so a pin
 * becomes a waypoint with no conversion and therefore no rounding. A float text format would
 * have been friendlier to read and would have moved every pin a few centimetres on every
 * save/load round trip.
 *
 * The name is the REST OF THE LINE, so it may contain spaces; it may not contain a comma
 * (that is the field separator) or a control character, and mapPinSanitizeName() is what
 * guarantees it. Everything here is pure and Arduino-free so tests/test_maptiles.cpp can prove
 * the round trip on the host — including the malformed lines, which is the half that matters:
 * a card is a thing people edit by hand and a half-written line must cost one pin, never the file.
 */
#ifndef MAP_PINS_H
#define MAP_PINS_H

#include <stddef.h>
#include <stdint.h>

/* 20 bytes including the terminator, EXACTLY MESH_WP_NAME_LEN. Not a coincidence and not to
 * be widened on its own: a pin longer than a waypoint name would be silently truncated the
 * first time it was shared, and the user would have named a thing on their map that their
 * friends see under a different, shorter name. */
#define MAP_PIN_NAME_LEN   20
#define MAP_PIN_CHAN_LEN   24      // == MESH_NAME_LEN; the channel name, or "" if never shared
#define MAP_PIN_LINE_MAX   128     // longest line mapPinFormatLine can produce, plus slack

typedef struct {
  int32_t  latI, lonI;             // 1e-7 degrees
  uint32_t sharedId;               // mesh waypoint id if shared, 0 if this pin is private
  char     name[MAP_PIN_NAME_LEN];
  char     chan[MAP_PIN_CHAN_LEN]; // the channel it is shared on, by name; "" = none yet
} MapPin;

/* Parse one line of the pins file.
 *   1  = a pin was written to *out
 *   0  = blank line or comment; nothing written, not an error
 *  -1  = malformed; nothing written. The caller SKIPS it and keeps reading. */
int  mapPinParseLine(const char* line, MapPin* out);

/* Render a pin as its line, WITHOUT a trailing newline. Returns the length, or 0 if it would
 * not fit in `cap` (in which case out is emptied — a truncated line reloads as a different
 * pin, or as a malformed one, and either is worse than refusing). */
int  mapPinFormatLine(const MapPin* p, char* out, size_t cap);

/* Make any user-typed string safe to be a pin name: strips control characters, turns commas
 * into spaces (the separator cannot survive in the field), collapses the result's leading and
 * trailing spaces, and truncates to cap-1. An empty result becomes "Pin". */
void mapPinSanitizeName(const char* in, char* out, size_t cap);

/* Choose the next unused "Pin N" name. Returns N. Scans the existing names so that deleting
 * Pin 2 and adding one gives you Pin 2 back rather than Pin 4 — on a screen this small, the
 * numbers people see should stay small. */
int  mapPinAutoName(const MapPin* pins, int n, char* out, size_t cap);

/* Which pin is under the crosshair? Takes the already-projected viewport coordinates (so this
 * stays pure) and returns the index of the nearest within maxPx, or -1.
 * ⚠ Ties go to the LOWEST index, deliberately: two pins on the same spot must resolve the
 * same way every press, or "select, edit, select again" edits a different pin the second time. */
int  mapPinPickNearest(const int* vx, const int* vy, int n, int px, int py, int maxPx);

/* ── A PIN'S FRAME, UNTIL THE RADIO SAYS WHAT BECAME OF IT ──────────────────────────────────
 * 🛑 QUEUED IS NOT ON THE AIR (review M1, 2026-09-25; mesh_txq.h, MeshTxOutcome). A share, a
 * re-share, "Take it off the mesh" and a delete of a shared pin each queue ONE waypoint frame,
 * and the map used to act on "queued" as if it had gone: "Take it off the mesh" cleared the
 * pin's waypoint id at once, so a retraction that then failed stranded the place on every
 * other radio with nothing left on this phone that could ever retract it. Now the map keeps
 * the frame's packet id and asks, and THIS decides what the answer means — pure, so the rule
 * is proven on the host (tests/test_maptiles.cpp) rather than read off a screen.
 *
 * `outcome` is the MeshTxOutcome value (0 unknown, 1 queued, 2 sent, 3 failed). `final` is
 * "no more waiting": the Maps app is closing, another pin operation is starting, or the radio
 * has said nothing for too long. A final QUEUED is read like UNKNOWN — NO WORD — and no word
 * takes the SAFE side, which is always the side that keeps a waypoint id: an id the mesh does
 * not hold costs one harmless extra retraction later, an id thrown away costs a stale camp on
 * everyone's map forever. The one exception is a delete, which keeps what deleting always did
 * (the pin gone) — the frame is far likelier to be on its way than lost. */
enum MapPinMeshOp : uint8_t {
  MAP_PINOP_SHARE   = 1,   // a pin shared for the first time (the id was just made up)
  MAP_PINOP_RESHARE = 2,   // an update of a pin the mesh already holds (moved, renamed, re-sent)
  MAP_PINOP_UNSHARE = 3,   // "Take it off the mesh"
  MAP_PINOP_DELETE  = 4,   // a shared pin deleted: the retraction
};

enum MapPinMeshAct : uint8_t {
  MAP_PINACT_WAIT      = 0,   // still queued: keep asking
  MAP_PINACT_DONE      = 1,   // it went (or no word on a delete): nothing more to change
  MAP_PINACT_FORGET_ID = 2,   // the retraction went: NOW clear the pin's waypoint id
  MAP_PINACT_ROLL_BACK = 3,   // a first share never left: clear the id, the pin stays just yours
  MAP_PINACT_KEEP_ID   = 4,   // it did not go, or no word: the id stays (the mesh may hold it)
  MAP_PINACT_RESTORE   = 5,   // a retraction never left: put the deleted pin back, id and all
};

uint8_t mapPinMeshSettle(uint8_t op, uint8_t outcome, bool final);

/* 🛑 A RETRACTION STILL IN THE QUEUE IS NOT SETTLED BY THE NEXT OPERATION ON THE SAME PIN
 * (integration review of M1, 2026-09-25). The id is kept until the retraction is reported
 * SENT, so for the second or so it waits, the pin still carries its waypoint id — and Move
 * here and Rename re-share any pin that carries one. That re-share settles the old frame
 * first with no more waiting (final), a final QUEUED unshare is KEEP_ID above, and the update
 * then goes out in the queue RIGHT BEHIND the retraction: the place comes back on every other
 * radio, which is the one thing "Take it off the mesh" was pressed to prevent (before M1 the
 * id was cleared at once, so neither handler re-shared). The settle rule is right — no word
 * keeps the id — so the guard is a separate question asked BEFORE it: is a retraction of
 * THIS pin's waypoint still waiting? True only for an unshare or a delete, still QUEUED, of a
 * non-zero waypoint id equal to the pin's. The map then refuses the re-share (sharePin) and
 * Move/Rename change the pin here only, leaving the retraction to finish. Pure, like the
 * settle: tests/test_maptiles.cpp. `outcome` is the MeshTxOutcome value. */
bool mapPinRetractionQueued(uint8_t op, uint8_t outcome, uint32_t opWpId, uint32_t pinWpId);

#endif // MAP_PINS_H
