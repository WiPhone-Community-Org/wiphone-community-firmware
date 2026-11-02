/*
 * mesh_pos.h — Meshtastic position & waypoint payloads, and the small geometry
 * needed to say "1.4km NE of Camp".
 *
 * ⚠ THIS HEADER USED TO SAY "the phone has NO GPS (yet): it only LISTENS".
 * That has been false since the woods backplate shipped — the u-blox M10 is
 * read as NMEA by nmea.cpp and its fix reaches meshtastic_service. Both
 * directions now meet here:
 *   INBOUND  — COVEY broadcasts its Position every 5 minutes and shares
 *              Waypoints from its map; this file turns those payloads into
 *              distances and bearings from a chosen reference point (a
 *              waypoint, this phone's own GPS, or the user-declared pin).
 *   OUTBOUND — meshPosBuild() serves the manual "I'm here" pin announce;
 *              meshPosBuildEx() adds where a MEASURED fix came from and how
 *              precise it is, for the position beacon (meshtastic_service.cpp).
 *
 * Wire formats verified against meshtastic/protobufs master (2026-08-19; fields
 * 5 and 23 re-read 2026-10-10 against mesh.proto and firmware 2.8.2):
 *   Position (port 3): latitude_i=1 sfixed32, longitude_i=2 sfixed32,
 *                      altitude=3 int32, time=4 fixed32 — lat/lon are 1e-7 deg.
 *                      location_source=5 enum (varint), precision_bits=23 uint32
 *                      (varint, TWO-byte tag 0xB8 0x01). 22 is seq_number.
 *   Waypoint (port 8): id=1, latitude_i=2, longitude_i=3, expire=4,
 *                      locked_to=5, name=6.
 *
 * Self-contained, no Arduino headers: tests/test_pos.cpp proves the shipping
 * bytes and math on the host (book_hash.h explains the philosophy).
 */
#ifndef MESH_POS_H
#define MESH_POS_H

#include <stddef.h>
#include <stdint.h>

#define MESH_WP_NAME_LEN 20     // bounded copy of Waypoint.name

typedef struct {
  uint32_t id;
  int32_t  latI, lonI;          // 1e-7 degrees
  uint32_t expire;              // unix seconds, 0 = never
  uint32_t lockedTo;            // node that owns edits, 0 = anyone
  char     name[MESH_WP_NAME_LEN];
  bool     hasPos;              // both lat and lon were present
} MeshWaypointMsg;

// Parse a Position payload. True only when BOTH latitude_i and longitude_i
// were present (a fix-less position is useless here). timeOut optional.
bool meshPosParse(const uint8_t* pl, size_t len,
                  int32_t* latI, int32_t* lonI, uint32_t* timeOut);

/* ── WHERE A POSITION CAME FROM, AND HOW SHARP IT IS (fields 5 and 23) ─────────────────────
 * location_source (mesh.proto LocSource): what the SENDER says produced the fix. Stock firmware
 * stamps its own GPS INTERNAL and a fixed/typed position MANUAL; the official Android and iOS
 * apps stamp the phone's GPS EXTERNAL. Stock receivers also read it as "may I take this
 * packet's TIME?" — they refuse UNSET and MANUAL (PositionModule.cpp, "Ignore time from mesh:
 * unknown or manual source"), so INTERNAL/EXTERNAL is a claim of a MEASUREMENT, never a label
 * for a typed pin or a network guess.
 *
 * precision_bits: how many top bits of each coordinate the sender KEPT. A sender that degrades
 * a position (a channel set to 13 bits, a coarse fix) cuts both coordinates to that many bits
 * and moves the point to the middle of the cell (meshPosTruncate); receivers draw a circle.
 * 0 (absent) and 32 both mean "not degraded" — 0 is just what every sender that never heard of
 * the field sends, including this phone's own meshPosBuild. */
#define MESH_LOC_UNSET        0
#define MESH_LOC_MANUAL       1
#define MESH_LOC_INTERNAL     2     // this device's own GNSS (the woods plate)
#define MESH_LOC_EXTERNAL     3     // a GNSS the device was handed (the Android/iOS apps: the phone's)
#define MESH_POS_PREC_FULL   32

typedef struct {
  int32_t  latI, lonI;          // 1e-7 degrees (already cut to the cell's centre by the sender)
  uint32_t time;                // field 4, 0 = absent
  uint8_t  locSrc;              // field 5 as sent, MESH_LOC_*; 0 = absent or UNSET; > 255 reads 255
  uint8_t  precBits;            // field 23, 0 = absent; above 32 reads 32 (the firmware's own min)
} MeshPosMsg;

/* The same walk as meshPosParse (which now wraps it), keeping fields 5 and 23 as well. True
 * only when both latitude_i and longitude_i were present; *out is zeroed first either way.
 * Fields 5/23 count only as varints (wire 0): anything else under those numbers is not what
 * mesh.proto says, and is skipped like every field this file does not read. */
bool meshPosParseEx(const uint8_t* pl, size_t len, MeshPosMsg* out);

/* The radius, in metres, of the circle a receiver should draw for `precBits`: half the cell's
 * north-south size, 23,905,787.925008 / 2^p — the Android app's formula, which agrees with its
 * table (10 -> 23,345 m ... 18 -> 91.2 m, 19 -> 45.6 m) to within 1.2 cm (test_pos checks
 * it). 0 for a position that is not degraded (0 or >= 32). The east-west size is that times
 * cos(latitude), so the worst point of a cell is further away than this: a radius, not a
 * guarantee. */
double meshPosPrecisionRadiusM(uint32_t precBits);

/* Is a received position worth calling APPROXIMATE on a screen? 1..20 bits: a half-cell of
 * 22.8 m and up. From 21 bits (11.4 m) the cut is finer than a phone GPS's own error, so a ring
 * or an "approx" would claim an uncertainty the sender did not add. */
#define MESH_POS_APPROX_MAX_BITS  20
bool meshPosIsApprox(uint32_t precBits);

/* Cut one coordinate to `p` bits and move it to the middle of the cell — firmware's
 * truncateCoordinate (src/mesh/PositionPrecision.cpp) and Android's coarsenCoordinate, on the
 * coordinate's two's-complement bits:  (v & (UINT32_MAX << (32-p))) + (1 << (31-p)).
 * p == 0 or p >= 32 returns v unchanged (the firmware's own rule). Idempotent: a cut value cut
 * again is itself, so cutting before meshPosBuildEx (which cuts too) is harmless. */
int32_t meshPosTruncate(int32_t v, uint32_t p);

// Parse a Waypoint payload. True when id != 0; check out->hasPos — a waypoint
// WITHOUT a position is a DELETION marker (the Meshtastic convention: absence
// of position is the discriminator, since expire==0 means "never expires").
bool meshWaypointParse(const uint8_t* pl, size_t len, MeshWaypointMsg* out);

/* Build a minimal Position payload {latitude_i, longitude_i, [time]}. Returns
 * bytes written: 15 with a known clock, 10 without.
 *
 * ⚠ TWO CALLERS WITH DIFFERENT PROVENANCE share these bytes now — the manual
 * "I'm here" pin (a statement by the user) and the periodic GPS beacon (a
 * measurement). The payload cannot tell them apart and neither can a receiver,
 * so the distinction lives entirely in which caller decided to transmit. That
 * is why the beacon's freshness gate sits in the service, not here.
 *
 * The 15/10 sizes are asserted by tests/test_pos.cpp on purpose: the beacon's
 * whole airtime budget (37 B on air, 518 ms at SF11/BW250) is computed from
 * them, so a silent change to this function invalidates the duty-cycle
 * reasoning behind the offered intervals. */
size_t meshPosBuild(uint8_t* out, int32_t latI, int32_t lonI, uint32_t time);

/* A Position that says where it came from: {latitude_i, longitude_i, [time], [location_source],
 * [precision_bits]}, in field order — byte-identical to the protobuf runtime's own encoding
 * (tests/vectors_pos.h, POS_ENCX). Returns bytes written, or 0 with NOTHING written when the
 * buffer is too small, `locSrc` is not a MESH_LOC_* value, or `precBits` is above 32.
 *
 *   locSrc 0    field 5 left out (UNSET, proto3's default).
 *   precBits 0  field 23 left out: the bytes meshPosBuild has always sent, which every receiver
 *               reads as precise. ⚠ NOT the firmware's channel setting 0, which means "send no
 *               position at all" — that decision belongs to the caller, before this is called.
 *   1..31       BOTH coordinates are cut to the cell's centre (meshPosTruncate) here, so the
 *               bytes can never claim fewer bits than they carry.
 *   32          full precision, said explicitly. Old iOS builds hid a node whose precision was
 *               0 ("unspecified"; firmware's TypeConversions.cpp still rewrites 0 to 32 for
 *               them), which is why a measured fix sends 32 rather than leaving the field out.
 *
 * 🔑 THE SIZE IS AN AIRTIME CONTRACT, like meshPosBuild's: a measured fix with a clock is
 * 5+5+5 + 2 (field 5) + 3 (field 23) = 20 B, 42 B on air = 559 ms at SF11/BW250 (518 ms for
 * the old 15). Anything up to 22 B stays at 559 ms; 23..27 B cost 600 ms — which is why no
 * altitude is sent (128 m and up is a 3-byte varint, any negative one is 11 bytes).
 * tests/test_pos.cpp asserts the 20, tests/test_airtime.cpp the 559. */
#define MESH_POS_BUILD_EX_MAX   20
size_t meshPosBuildEx(uint8_t* out, size_t cap, int32_t latI, int32_t lonI, uint32_t time,
                      uint32_t locSrc, uint32_t precBits);

/* Longest payload meshWaypointBuild can produce: id (1+5) + lat (5) + lon (5) + expire (1+5)
 * + locked_to (1+5) + name (1+1+19). Sized here so callers can put the buffer on the stack
 * without guessing. */
#define MESH_WP_BUILD_MAX   64

/* Build a Waypoint payload (port 8) — the bytes COVEY's map reads to put a pin on it.
 *
 * ⚠ `hasPos == false` builds the DELETION marker: id alone, no position. That is the
 * Meshtastic convention and it is the ONLY way to take a waypoint off other people's maps
 * (expire cannot do it — expire == 0 legitimately means "never"). meshWaypointParse documents
 * the receiving half of the same rule.
 *
 * `lockedTo` should be this node's number for a waypoint we own: it is what stops somebody
 * else's radio quietly moving or deleting your camp. 0 means anyone may edit.
 *
 * Returns bytes written, or 0 if id is 0 or the buffer is too small. Nothing is written on 0.
 * tests/test_pos.cpp checks the bytes AND round-trips them back through meshWaypointParse. */
size_t meshWaypointBuild(uint8_t* out, size_t cap, uint32_t id, bool hasPos,
                         int32_t latI, int32_t lonI, uint32_t expire, uint32_t lockedTo,
                         const char* name);

// Great-circle distance in meters (haversine, R=6371km — meters-accurate at
// hunt scale, which is all the display shows).
double meshPosDistanceM(int32_t aLatI, int32_t aLonI, int32_t bLatI, int32_t bLonI);

// Initial bearing from a to b, degrees 0..359 (0 = true north).
int meshPosBearingDeg(int32_t aLatI, int32_t aLonI, int32_t bLatI, int32_t bLonI);

// "N" "NE" "E" ... for a bearing in degrees.
const char* meshPosCompass8(int deg);

// "850m" / "1.4km" / "23km".
void meshPosFmtDist(double meters, char* out, size_t cap);

/* ---- The position beacon's movement gate --------------------------------
 * A stationary phone re-sending an identical position is pure waste of a
 * shared band. A phone that goes silent for hours looks like a dead radio,
 * and on a hunting channel "he's still on the stand" is real information.
 * So: suppress the slot when the fix has barely moved, but never suppress
 * more than MESH_POS_MAX_SKIPS in a row. At a 15-minute interval a parked
 * phone therefore transmits somewhere between every 15 and every 60 minutes.
 *
 * 100 m matches upstream's broadcast_smart_minimum_distance.
 * UNKNOWN: that default is upstream knowledge, not verifiable in this tree.
 *
 * This is the ONE piece of the beacon that is arithmetic rather than policy,
 * so it lives in this Arduino-free file where tests/test_pos.cpp can prove it. */
#define MESH_POS_MIN_MOVE_M   100.0
#define MESH_POS_MAX_SKIPS    3

/* ── HOW GOOD A FIX HAS TO BE BEFORE IT IS SOMEBODY ELSE'S PROBLEM ────────────────────────
 * ⚠ THREE SATELLITES IS A 2D FIX: it solves lat/lon by ASSUMING an altitude, and a wrong
 * assumption pushes the error SIDEWAYS — into the one number a hunting party reads. MEASURED
 * 2026-08-25: an indoor WiPhone reported `sats=3 hdop=6.4` at a position (40.0,-100.0 stands in
 * for it here; real fixes are not kept in this public repo) while the phone was actually at a
 * known spot **about 20 km away**, and nothing would have refused it. Four is the arithmetic minimum for a real 3D fix, so this is the standard bar
 * and not a strict one; under canopy a working receiver tracks five to ten. HDOP 10 is the
 * usual "poor" boundary and is a backstop, not the main gate. */
#define MESH_POS_MIN_SATS         4
#define MESH_POS_MAX_HDOP_X10   100

/* True when this slot should actually transmit. `skipRuns` is how many
 * consecutive slots have already been suppressed. haveLast == false (nothing
 * transmitted since boot) always transmits — the first beacon is what tells
 * the mesh this node exists. */
/* Is this fix good enough to put on other people's maps?
 * ⚠ REFUSES ONLY WHAT IT POSITIVELY KNOWS IS BAD: both fields are -1 when the receiver has
 * not said (they come from GGA), and an unknown is NOT a failure — refusing on silence would
 * break any receiver that does not emit GGA, which is a different bug than the one this fixes.
 * Pure, so tests/test_pos.cpp can prove it. */
bool meshPosFixUsable(int sats, int hdopX10);

bool meshPosShouldBeacon(bool haveLast, int32_t lastLatI, int32_t lastLonI,
                         int32_t latI, int32_t lonI, int skipRuns);

#endif // MESH_POS_H
