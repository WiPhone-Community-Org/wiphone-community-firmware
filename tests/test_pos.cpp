/*
 * test_pos.cpp — mesh_pos.cpp: Position/Waypoint payload parsing and the
 * distance/bearing math behind "1.4km NE of Camp". Reference values computed
 * independently in Python (same haversine/bearing formulas, double precision).
 */

#include "../WiPhone/mesh_pos.h"
#include "vectors_pos.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static int failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

static size_t putF32(uint8_t* b, uint8_t tag, uint32_t v) {
  b[0] = tag; b[1] = v; b[2] = v >> 8; b[3] = v >> 16; b[4] = v >> 24;
  return 5;
}

int main() {
  printf("test_pos\n");

  const int32_t SEA_LAT = 476062000, SEA_LON = -1223321000;   // 47.6062, -122.3321
  const int32_t PDX_LAT = 455152000, PDX_LON = -1226784000;   // 45.5152, -122.6784

  // ---- Position parse -------------------------------------------------------
  {
    uint8_t b[64];
    size_t n = 0;
    n += putF32(b + n, 0x0d, (uint32_t)SEA_LAT);     // latitude_i
    n += putF32(b + n, 0x15, (uint32_t)SEA_LON);     // longitude_i
    b[n++] = 0x18; b[n++] = 0x64;                    // altitude=100 (varint, skipped)
    n += putF32(b + n, 0x25, 1766000000u);           // time

    int32_t lat = 0, lon = 0; uint32_t t = 0;
    CHECK(meshPosParse(b, n, &lat, &lon, &t) &&
          lat == SEA_LAT && lon == SEA_LON && t == 1766000000u, "position parses");

    /* A REAL stock-firmware Position: high-numbered fields use TWO-byte varint
     * tags. sats_in_view = field 19 (tag 0x98 0x01), seq_number = field 22
     * (tag 0xB0 0x01) and precision_bits = field 23 (tag 0xB8 0x01) — stock
     * appends precision_bits to every channel broadcast. A one-byte tag reader
     * shears apart here (the adversarial review's catch).
     * ⚠ Until 0.9.82 this comment called 0xB0 0x01 precision_bits. It is 22,
     * seq_number; mesh.proto puts precision_bits at 23. Nothing read either field
     * then, so the mislabel cost nothing — meshPosParseEx reads 23 now, and the
     * two values below differ so a 22/23 mix-up shows. */
    uint8_t stock[64];
    size_t sn = 0;
    stock[sn++] = 0x98; stock[sn++] = 0x01; stock[sn++] = 0x07;   // sats_in_view=7
    sn += putF32(stock + sn, 0x0d, (uint32_t)SEA_LAT);
    sn += putF32(stock + sn, 0x15, (uint32_t)SEA_LON);
    stock[sn++] = 0xB0; stock[sn++] = 0x01; stock[sn++] = 0x05;   // seq_number=5 (field 22)
    stock[sn++] = 0xB8; stock[sn++] = 0x01; stock[sn++] = 0x20;   // precision_bits=32 (field 23)
    CHECK(meshPosParse(stock, sn, &lat, &lon, &t) &&
          lat == SEA_LAT && lon == SEA_LON, "field>=16 tags (stock seq_number, precision_bits) survive");
    MeshPosMsg pm;
    CHECK(meshPosParseEx(stock, sn, &pm) && pm.precBits == 32 && pm.locSrc == 0,
          "precision_bits read from field 23, never from seq_number (22)");

    CHECK(!meshPosParse(b, 5, &lat, &lon, &t), "lat-only position refused");
    CHECK(!meshPosParse(b, 3, &lat, &lon, &t), "truncated fixed32 refused");
    uint8_t junk[4] = { 0x0b, 0x01, 0x02, 0x03 };    // wire type 3: not our protobuf
    CHECK(!meshPosParse(junk, sizeof(junk), &lat, &lon, &t), "unknown wire type refused");
  }

  // ---- build -> parse round trip -------------------------------------------
  {
    uint8_t b[20];
    size_t n = meshPosBuild(b, PDX_LAT, PDX_LON, 1766000123u);
    int32_t lat, lon; uint32_t t;
    CHECK(n == 15 && meshPosParse(b, n, &lat, &lon, &t) &&
          lat == PDX_LAT && lon == PDX_LON && t == 1766000123u, "own announce round-trips");
    n = meshPosBuild(b, PDX_LAT, PDX_LON, 0);
    CHECK(n == 10 && meshPosParse(b, n, &lat, &lon, &t) && t == 0, "time omitted when unknown");

    /* 🔑 THE TWO SIZES ABOVE ARE AN AIRTIME CONTRACT, not a formatting detail.
     * 15 B payload + 6 B Data protobuf framing + 16 B MeshPacketHeader = 37 B
     * on air, which at the LongFast registers this repo writes (SF11, BW250,
     * CR4/5, preamble 16 — mesh_phy.cpp:114) is 518 ms of transmission. Every
     * duty-cycle number behind the offered beacon intervals is derived from
     * that. If either size changes, the intervals need re-justifying and this
     * assertion is what says so. */

    /* Southern hemisphere AND western longitude: both axes negative, which is
     * the sfixed32 case most likely to be got wrong by a sign-extension slip.
     * (Hobart is -42.88, 147.33 — one negative axis; this is Punta Arenas.) */
    const int32_t PA_LAT = -531629000, PA_LON = -708546000;
    n = meshPosBuild(b, PA_LAT, PA_LON, 1766000123u);
    CHECK(n == 15 && meshPosParse(b, n, &lat, &lon, &t) &&
          lat == PA_LAT && lon == PA_LON && t == 1766000123u,
          "negative lat AND lon round-trip");
  }

  // ---- Waypoint parse -------------------------------------------------------
  {
    uint8_t b[64];
    size_t n = 0;
    b[n++] = 0x08; b[n++] = 0x2a;                    // id=42
    n += putF32(b + n, 0x15, (uint32_t)SEA_LAT);     // latitude_i (field 2)
    n += putF32(b + n, 0x1d, (uint32_t)SEA_LON);     // longitude_i (field 3)
    b[n++] = 0x20; b[n++] = 0x00;                    // expire=0
    b[n++] = 0x32; b[n++] = 4;                       // name="Camp"
    memcpy(b + n, "Camp", 4); n += 4;

    MeshWaypointMsg wp;
    CHECK(meshWaypointParse(b, n, &wp) && wp.id == 42 && wp.latI == SEA_LAT &&
          wp.lonI == SEA_LON && wp.expire == 0 && strcmp(wp.name, "Camp") == 0,
          "waypoint parses");

    uint8_t noId[16];
    size_t m = 0;
    m += putF32(noId + m, 0x15, (uint32_t)SEA_LAT);
    m += putF32(noId + m, 0x1d, (uint32_t)SEA_LON);
    CHECK(!meshWaypointParse(noId, m, &wp), "waypoint without id refused");

    /* id + NO position = a DELETION marker (COVEY's map sends exactly this on
     * delete — waypoints.is_delete: absence of position is the discriminator).
     * Must PARSE (hasPos false), not be rejected: rejecting it silently broke
     * delete-on-COVEY -> delete-on-phone. */
    uint8_t del[8];
    m = 0;
    del[m++] = 0x08; del[m++] = 0x2a;                // id=42, nothing else
    CHECK(meshWaypointParse(del, m, &wp) && wp.id == 42 && !wp.hasPos,
          "id-only waypoint parses as a delete marker");

    // Oversized name is truncated, never overrun (ASan watches).
    uint8_t big[64];
    m = 0;
    big[m++] = 0x08; big[m++] = 0x01;
    m += putF32(big + m, 0x15, (uint32_t)SEA_LAT);
    m += putF32(big + m, 0x1d, (uint32_t)SEA_LON);
    big[m++] = 0x32; big[m++] = 30;
    memset(big + m, 'A', 30); m += 30;
    CHECK(meshWaypointParse(big, m, &wp) && strlen(wp.name) == MESH_WP_NAME_LEN - 1,
          "30-char name truncated safely");
  }

  // ---- geometry (python-checked references) ---------------------------------
  {
    double d = meshPosDistanceM(SEA_LAT, SEA_LON, PDX_LAT, PDX_LON);
    CHECK(fabs(d - 234010.5) < 30.0, "Seattle->Portland ~234.0 km");
    CHECK(meshPosBearingDeg(SEA_LAT, SEA_LON, PDX_LAT, PDX_LON) == 187, "bearing 187 (S)");

    // Hunt scale: ~1 km due east at 47N, ~500 m due north.
    d = meshPosDistanceM(470000000, -1200000000, 470000000, -1199868290);
    CHECK(fabs(d - 998.8) < 2.0, "1 km east at hunt scale");
    CHECK(meshPosBearingDeg(470000000, -1200000000, 470000000, -1199868290) == 90, "east = 90");
    d = meshPosDistanceM(470000000, -1200000000, 470044966, -1200000000);
    CHECK(fabs(d - 500.0) < 1.0, "500 m north");
    CHECK(meshPosBearingDeg(470000000, -1200000000, 470044966, -1200000000) == 0, "north = 0");
    CHECK(meshPosDistanceM(SEA_LAT, SEA_LON, SEA_LAT, SEA_LON) < 0.01, "zero distance to self");
  }

  /* ---- the position beacon's movement gate ---------------------------------
   * Offsets computed independently in Python with the same haversine (the
   * method this file already uses for the geometry block above):
   *   99 m due north of 47.0,-120.0  = +8903 in 1e-7 deg  (measured 98.997 m)
   *  101 m due north                 = +9083              (measured 100.998 m)
   * The pair straddles MESH_POS_MIN_MOVE_M from both sides on purpose: an
   * off-by-one in the comparison shows up as exactly one of them flipping. */
  {
    const int32_t LAT = 470000000, LON = -1200000000;

    CHECK(meshPosShouldBeacon(false, 0, 0, LAT, LON, 0),
          "first beacon after boot always transmits");

    // Parked: suppressed for MESH_POS_MAX_SKIPS slots, then forced out.
    CHECK(!meshPosShouldBeacon(true, LAT, LON, LAT, LON, 0) &&
          !meshPosShouldBeacon(true, LAT, LON, LAT, LON, 1) &&
          !meshPosShouldBeacon(true, LAT, LON, LAT, LON, 2),
          "stationary phone stays quiet for three slots");
    CHECK(meshPosShouldBeacon(true, LAT, LON, LAT, LON, MESH_POS_MAX_SKIPS),
          "keep-alive floor: the fourth slot transmits anyway");

    CHECK(!meshPosShouldBeacon(true, LAT, LON, LAT + 8903, LON, 0), "99 m does not beacon");
    CHECK(meshPosShouldBeacon(true, LAT, LON, LAT + 9083, LON, 0), "101 m beacons");

    // 5 km: transmits at every skip count, including a freshly reset one.
    CHECK(meshPosShouldBeacon(true, LAT, LON, LAT, LON + 658550, 0) &&
          meshPosShouldBeacon(true, LAT, LON, LAT, LON + 658550, 2),
          "5 km beacons regardless of skip count");

    /* Sign handling: a step ACROSS the antimeridian is 111 m, not 40,000 km.
     * If meshPosDistanceM ever loses the wrap, the gate would read a two-metre
     * shuffle at the date line as a continent-crossing move and transmit every
     * slot — the one place this arithmetic can silently cost real airtime. */
    CHECK(meshPosShouldBeacon(true, 0, 1799995000, 0, -1799995000, 0),
          "antimeridian: 111 m across the line beacons");
    CHECK(!meshPosShouldBeacon(true, 0, 1799999500, 0, -1799999500, 0),
          "antimeridian: 11 m across the line does not");
    // ...and across the equator, where the latitude sign flips.
    CHECK(meshPosShouldBeacon(true, -4497, 0, 4497, 0, 0) &&
          !meshPosShouldBeacon(true, -2000, 0, 2000, 0, 0),
          "equator crossing: 100 m beacons, 44 m does not");
  }

  /* ---- the fix-quality gate ------------------------------------------------
   * 🛑 THIS RULE HAS A MEASUREMENT BEHIND IT, not a preference. On 2026-08-25 an
   * indoor WiPhone reported `sats=3 hdop=6.4` at a position (40.0,-100.0 stands in
   * for it; real fixes are not kept in this public repo) while the
   * phone was actually at a known spot about 20 km away. Three satellites
   * is a 2D fix: it assumes an altitude, and a wrong assumption puts the error
   * into the horizontal, which is the one number a hunting party reads. Nothing
   * on the beacon path refused it before this gate existed. */
  {
    CHECK(!meshPosFixUsable(3, 64), "the measured 20 km fix (sats=3, HDOP 6.4) is refused");
    CHECK(!meshPosFixUsable(0, -1) && !meshPosFixUsable(1, -1) && !meshPosFixUsable(2, -1),
          "0-2 satellites cannot be a fix at all");
    CHECK(!meshPosFixUsable(MESH_POS_MIN_SATS - 1, 10) &&
          meshPosFixUsable(MESH_POS_MIN_SATS, 10),
          "the satellite bar straddles MESH_POS_MIN_SATS exactly");
    CHECK(meshPosFixUsable(9, 12), "a good open-sky fix passes");
    CHECK(meshPosFixUsable(6, 45), "a working under-canopy fix (6 sats, HDOP 4.5) passes");

    /* ⚠ UNKNOWN IS NOT FAILURE. Both fields are -1 until a GGA supplies them, and
     * a receiver that never emits GGA must still be able to report — refusing on
     * silence would be a different bug than the one this gate fixes. */
    CHECK(meshPosFixUsable(-1, -1), "unknown quality is allowed, not refused");
    CHECK(!meshPosFixUsable(-1, 250), "unknown sats + bad HDOP still refuses, on HDOP alone");
    CHECK(meshPosFixUsable(8, -1), "good sats + unknown HDOP passes");

    // The HDOP backstop, straddled from both sides.
    CHECK(meshPosFixUsable(8, MESH_POS_MAX_HDOP_X10) &&
          !meshPosFixUsable(8, MESH_POS_MAX_HDOP_X10 + 1),
          "the HDOP bar straddles MESH_POS_MAX_HDOP_X10 exactly");
  }

  // ---- presentation ---------------------------------------------------------
  {
    CHECK(strcmp(meshPosCompass8(0), "N") == 0 && strcmp(meshPosCompass8(359), "N") == 0 &&
          strcmp(meshPosCompass8(23), "NE") == 0 && strcmp(meshPosCompass8(187), "S") == 0 &&
          strcmp(meshPosCompass8(292), "W") == 0 && strcmp(meshPosCompass8(293), "NW") == 0,
          "compass points");
    char s[16];
    meshPosFmtDist(850, s, sizeof(s));    CHECK(strcmp(s, "850m") == 0, "850m");
    meshPosFmtDist(1437, s, sizeof(s));   CHECK(strcmp(s, "1.4km") == 0, "1.4km");
    meshPosFmtDist(23400, s, sizeof(s));  CHECK(strcmp(s, "23km") == 0, "23km");
    meshPosFmtDist(999.6, s, sizeof(s));  CHECK(strcmp(s, "1.0km") == 0, "999.6 rounds up to km");
  }

  if (failures) {
    printf("test_pos: %d FAILURE(S)\n", failures);
    return 1;
  }

  // ---- Against the REAL protobuf runtime (vectors_pos.h, generated) -----------
  /* Everything above this line checks mesh_pos.cpp against bytes written by hand in
   * this file — the author's understanding of the protocol, tested against itself.
   * These check it against what upstream's own encoder emits, which is what COVEY's
   * RAK actually puts on the air every five minutes. Byte equality on encode; exact
   * field values on decode; and the stock-shaped Position carries every field a real
   * radio sends, two-byte tags included. */
  {
    printf("  [upstream vectors, meshtastic %s]\n", POS_VEC_MESHTASTIC_VERSION);
    char nm[96];
    for (int i = 0; i < POS_ENC_N; i++) {
      const PosEncVec* v = &POS_ENC[i];
      uint8_t out[64];
      size_t n = meshPosBuild(out, (int32_t)v->latI, (int32_t)v->lonI, (uint32_t)v->time);
      snprintf(nm, sizeof(nm), "encode '%s' bytes == upstream", v->label);
      bool same = ((int)n == v->len) && memcmp(out, v->bytes, n) == 0;
      CHECK(same, nm);
      if (!same) {
        printf("    ours  :"); for (size_t k = 0; k < n; k++) printf(" %02x", out[k]); printf("\n");
        printf("    theirs:"); for (int k = 0; k < v->len; k++) printf(" %02x", v->bytes[k]); printf("\n");
      }
    }
    for (int i = 0; i < POS_DEC_N; i++) {
      const PosDecVec* v = &POS_DEC[i];
      int32_t lat = 0x7ABCDEF0, lon = 0x7ABCDEF0; uint32_t t = 0xDEADBEEF;
      bool ok = meshPosParse(v->bytes, (size_t)v->len, &lat, &lon, &t);
      snprintf(nm, sizeof(nm), "decode '%s' accepted == %d", v->label, v->expectOk);
      CHECK(ok == (v->expectOk != 0), nm);
      if (v->expectOk) {
        snprintf(nm, sizeof(nm), "decode '%s' lat/lon/time", v->label);
        CHECK(lat == (int32_t)v->latI && lon == (int32_t)v->lonI && t == (uint32_t)v->time, nm);
      }
    }
    for (int i = 0; i < WP_DEC_N; i++) {
      const WpDecVec* v = &WP_DEC[i];
      MeshWaypointMsg wp;
      bool ok = meshWaypointParse(v->bytes, (size_t)v->len, &wp);
      snprintf(nm, sizeof(nm), "waypoint '%s' accepted == %d", v->label, v->expectOk);
      CHECK(ok == (v->expectOk != 0), nm);
      if (v->expectOk) {
        snprintf(nm, sizeof(nm), "waypoint '%s' id/hasPos/lat/lon", v->label);
        CHECK(wp.id == (uint32_t)v->id && (int)wp.hasPos == v->hasPos &&
              (!v->hasPos || (wp.latI == (int32_t)v->latI && wp.lonI == (int32_t)v->lonI)), nm);
        snprintf(nm, sizeof(nm), "waypoint '%s' expire/lockedTo", v->label);
        CHECK(wp.expire == (uint32_t)v->expire && wp.lockedTo == (uint32_t)v->lockedTo, nm);
        snprintf(nm, sizeof(nm), "waypoint '%s' name '%s'", v->label, wp.name);
        CHECK(strcmp(wp.name, v->name) == 0, nm);
      }
    }
  }

  // ---- Waypoint build (the bytes that put a pin on COVEY's map) --------------
  {
    uint8_t b[MESH_WP_BUILD_MAX];
    size_t n = meshWaypointBuild(b, sizeof(b), 0x1234u, true, SEA_LAT, SEA_LON,
                                 0, 0xdeadbeefu, "Camp");
    CHECK(n > 0, "waypoint builds");

    // Byte-exact: the field numbers and wire types are the interop contract.
    const uint8_t want[] = {
      0x08, 0xb4, 0x24,                                     // 1: id = 0x1234 varint
      0x15, 0x30, 0x21, 0x60, 0x1c,                         // 2: latitude_i sfixed32
      0x1d, 0x58, 0x9a, 0x15, 0xb7,                         // 3: longitude_i sfixed32
      0x28, 0xef, 0xfd, 0xb6, 0xf5, 0x0d,                   // 5: locked_to varint
      0x32, 0x04, 'C', 'a', 'm', 'p',                       // 6: name
    };
    CHECK(n == sizeof(want) && memcmp(b, want, n) == 0, "waypoint bytes are exactly right");

    // ...and it reads back as itself.
    MeshWaypointMsg wp;
    CHECK(meshWaypointParse(b, n, &wp) && wp.id == 0x1234u && wp.hasPos &&
          wp.latI == SEA_LAT && wp.lonI == SEA_LON && wp.lockedTo == 0xdeadbeefu &&
          wp.expire == 0 && strcmp(wp.name, "Camp") == 0, "waypoint round trips");

    // The deletion marker: an id and nothing else. hasPos false is the discriminator.
    n = meshWaypointBuild(b, sizeof(b), 0x1234u, false, SEA_LAT, SEA_LON, 0, 0, NULL);
    CHECK(n == 3 && b[0] == 0x08, "a deletion marker is the id alone");
    CHECK(meshWaypointParse(b, n, &wp) && wp.id == 0x1234u && !wp.hasPos,
          "the deletion marker parses as a positionless waypoint");

    // An expiring waypoint carries field 4.
    n = meshWaypointBuild(b, sizeof(b), 7, true, SEA_LAT, SEA_LON, 1766000000u, 0, "Stand");
    CHECK(meshWaypointParse(b, n, &wp) && wp.expire == 1766000000u &&
          !strcmp(wp.name, "Stand"), "expire survives the round trip");

    // Refusals: never a partial write.
    uint8_t guard[MESH_WP_BUILD_MAX];
    memset(guard, 0xa5, sizeof(guard));
    CHECK(meshWaypointBuild(guard, sizeof(guard), 0, true, SEA_LAT, SEA_LON, 0, 0, "x") == 0 &&
          guard[0] == 0xa5, "a waypoint with no id is refused, and nothing is written");
    CHECK(meshWaypointBuild(guard, 4, 9, true, SEA_LAT, SEA_LON, 0, 0, "x") == 0 &&
          guard[0] == 0xa5, "a buffer too small is refused, and nothing is written");

    // A name longer than the field is cut to the field, not to the buffer.
    n = meshWaypointBuild(b, sizeof(b), 9, true, SEA_LAT, SEA_LON, 0, 0,
                          "an extremely long waypoint name");
    CHECK(n > 0 && meshWaypointParse(b, n, &wp) &&
          strlen(wp.name) == MESH_WP_NAME_LEN - 1, "an over-long name is cut to the field");
  }

  /* ---- where a fix came from, and how sharp it is (0.9.82) ------------------------------
   * Dummy coordinates only from here down (40.0,-100.0 and friends): this repo is public. */
  const int32_t D_LAT = 400000000, D_LON = -1000000000;

  // ---- meshPosTruncate: the firmware's cut, by hand and against a second formula ----------
  {
    CHECK(meshPosTruncate(D_LAT, 0) == D_LAT && meshPosTruncate(D_LAT, 32) == D_LAT &&
          meshPosTruncate(D_LAT, 33) == D_LAT, "0 and >= 32 bits leave a coordinate alone");
    /* 17 bits: a cell of 2^15 = 32768 units. 400012345 // 32768 = 12207, so the cell starts at
     * 399,998,976 and its middle is +16384 = 400,015,360 (worked by hand). */
    CHECK(meshPosTruncate(400012345, 17) == 400015360, "17 bits: the cell's middle, by hand");
    /* 1 bit: only the sign survives, then the middle of that half: +-2^30. */
    CHECK(meshPosTruncate(0, 1) == 1073741824 && meshPosTruncate(-1, 1) == -1073741824 &&
          meshPosTruncate(2147483647, 1) == 1073741824, "1 bit: the middle of each half");

    /* The mask on two's-complement bits IS floor division for a negative number. Every value
     * below, at every depth, against floor(v / cell) * cell + cell / 2 in int64 — and the cut
     * is idempotent and never moves a point more than half a cell. */
    const int32_t probes[] = { 0, 1, -1, 7, -7, 2147483647, (int32_t)0x80000000, D_LAT, D_LON,
                               -338688200, 1512092900, 899999999, -899999999, 1799999999,
                               -1799999999, 123456789, -123456789 };
    bool allOk = true;
    for (uint32_t p = 1; p < 32; p++) {
      const int64_t cell = (int64_t)1 << (32 - p);
      for (size_t k = 0; k < sizeof(probes) / sizeof(probes[0]); k++) {
        const int64_t v = probes[k];
        const int64_t q = v >= 0 ? v / cell : -((-v + cell - 1) / cell);
        const int64_t want = q * cell + cell / 2;
        const int32_t got = meshPosTruncate((int32_t)v, p);
        const int64_t moved = (int64_t)got - v;
        if (got != want || meshPosTruncate(got, p) != got ||
            moved > cell / 2 || moved < -(cell / 2)) {
          allOk = false;
          printf("    p=%u v=%lld: got %d want %lld\n", (unsigned)p, (long long)v, got, (long long)want);
        }
      }
    }
    CHECK(allOk, "every depth 1..31 equals floor division, is idempotent, moves <= half a cell");
  }

  // ---- precision_bits -> the circle a receiver draws -----------------------------------------
  {
    /* The Android app's table (core/model/.../util/PositionPrecision.kt, carried over from its
     * OSMdroid marker): the formula must land within 1.2 cm of every entry. */
    const double table[10] = { 23345.484932, 11672.7369, 5836.36288, 2918.175876,
                               1459.0823719999053, 729.53562, 364.7622, 182.375556,
                               91.182212, 45.58554 };
    bool near = true;
    for (int p = 10; p <= 19; p++) {
      if (fabs(meshPosPrecisionRadiusM((uint32_t)p) - table[p - 10]) > 0.012) {
        near = false;
      }
    }
    CHECK(near, "radius 10..19 bits within 1.2 cm of Android's table");
    CHECK(meshPosPrecisionRadiusM(0) == 0.0 && meshPosPrecisionRadiusM(32) == 0.0 &&
          meshPosPrecisionRadiusM(40) == 0.0, "0 and >= 32 bits draw no circle");
    CHECK(!meshPosIsApprox(0) && meshPosIsApprox(1) && meshPosIsApprox(13) &&
          meshPosIsApprox(MESH_POS_APPROX_MAX_BITS) && !meshPosIsApprox(MESH_POS_APPROX_MAX_BITS + 1) &&
          !meshPosIsApprox(32), "approx: 1..20 bits, not 0, 21+ or 32");
  }

  // ---- meshPosBuildEx: by hand first, then against the runtime ----------------------------
  {
    uint8_t b[MESH_POS_BUILD_EX_MAX];
    const uint32_t T = 1766000000u;
    size_t n = meshPosBuildEx(b, sizeof(b), D_LAT, D_LON, T, MESH_LOC_EXTERNAL, MESH_POS_PREC_FULL);
    /* 🔑 THE AIRTIME CONTRACT (mesh_pos.h): a measured fix with a clock is 20 B, 42 on air,
     * 559 ms — tests/test_airtime.cpp holds the 559. */
    const uint8_t tail[] = { 0x28, 0x03, 0xb8, 0x01, 0x20 };   // field 5 = 3, field 23 = 32
    CHECK(n == 20 && n == MESH_POS_BUILD_EX_MAX && memcmp(b + 15, tail, 5) == 0,
          "the phone beacon: 20 B ending 28 03 b8 01 20");
    uint8_t legacy[16];
    size_t ln = meshPosBuild(legacy, D_LAT, D_LON, T);
    CHECK(memcmp(b, legacy, 15) == 0, "...and its first 15 B are meshPosBuild's");
    n = meshPosBuildEx(b, sizeof(b), D_LAT, D_LON, T, 0, 0);
    CHECK(n == ln && memcmp(b, legacy, n) == 0, "source 0, precision 0: exactly meshPosBuild (timed)");
    ln = meshPosBuild(legacy, D_LAT, D_LON, 0);
    n = meshPosBuildEx(b, sizeof(b), D_LAT, D_LON, 0, 0, 0);
    CHECK(n == ln && n == 10 && memcmp(b, legacy, n) == 0, "...and untimed");

    // Refusals write NOTHING.
    uint8_t guard[MESH_POS_BUILD_EX_MAX];
    memset(guard, 0xa5, sizeof(guard));
    CHECK(meshPosBuildEx(guard, sizeof(guard), D_LAT, D_LON, T, 4, 32) == 0 && guard[0] == 0xa5,
          "location_source 4 refused, nothing written");
    CHECK(meshPosBuildEx(guard, sizeof(guard), D_LAT, D_LON, T, 3, 33) == 0 && guard[0] == 0xa5,
          "precision_bits 33 refused, nothing written");
    CHECK(meshPosBuildEx(guard, 19, D_LAT, D_LON, T, 3, 32) == 0 && guard[0] == 0xa5,
          "a 19-byte buffer for a 20-byte position: refused, nothing written");
    CHECK(meshPosBuildEx(guard, 15, D_LAT, D_LON, 0, 3, 32) == 15,
          "...while 15 bytes hold the untimed one exactly");
    CHECK(meshPosBuildEx(NULL, 64, D_LAT, D_LON, T, 3, 32) == 0, "a NULL buffer is refused");

    /* 1..31 bits: the builder cuts the coordinates itself, so raw and pre-cut inputs give the
     * same bytes — the payload can never claim fewer bits than it carries. */
    uint8_t raw[MESH_POS_BUILD_EX_MAX], cut[MESH_POS_BUILD_EX_MAX];
    size_t rn = meshPosBuildEx(raw, sizeof(raw), 400012345, -1000067891, T, 0, 17);
    size_t cn = meshPosBuildEx(cut, sizeof(cut), meshPosTruncate(400012345, 17),
                               meshPosTruncate(-1000067891, 17), T, 0, 17);
    MeshPosMsg m;
    CHECK(rn == 18 && rn == cn && memcmp(raw, cut, rn) == 0 && meshPosParseEx(raw, rn, &m) &&
          m.latI == 400015360 && m.precBits == 17 && m.locSrc == 0,
          "17 bits: 18 B, the coordinates cut by the builder itself");

    printf("  [meshPosBuildEx / meshPosParseEx vs upstream, meshtastic %s]\n", POS_VEC_MESHTASTIC_VERSION);
    char nm[112];
    for (int i = 0; i < POS_ENCX_N; i++) {
      const PosEncXVec* v = &POS_ENCX[i];
      uint8_t out[MESH_POS_BUILD_EX_MAX];
      size_t on = meshPosBuildEx(out, sizeof(out), (int32_t)v->latI, (int32_t)v->lonI,
                                 (uint32_t)v->time, (uint32_t)v->locSrc, (uint32_t)v->precBits);
      snprintf(nm, sizeof(nm), "encode '%s' bytes == upstream", v->label);
      bool same = ((int)on == v->len) && memcmp(out, v->bytes, on) == 0;
      CHECK(same, nm);
      if (!same) {
        printf("    ours  :"); for (size_t k = 0; k < on; k++) printf(" %02x", out[k]); printf("\n");
        printf("    theirs:"); for (int k = 0; k < v->len; k++) printf(" %02x", v->bytes[k]); printf("\n");
      }
      MeshPosMsg back;
      snprintf(nm, sizeof(nm), "encode '%s' reads back (cut coords, source, precision)", v->label);
      CHECK(meshPosParseEx(out, on, &back) && back.latI == (int32_t)v->cutLatI &&
            back.lonI == (int32_t)v->cutLonI && back.time == (uint32_t)v->time &&
            back.locSrc == v->locSrc && back.precBits == v->precBits, nm);
    }
    for (int i = 0; i < POS_DECX_N; i++) {
      const PosDecXVec* v = &POS_DECX[i];
      MeshPosMsg dm;
      memset(&dm, 0x5a, sizeof(dm));
      bool ok = meshPosParseEx(v->bytes, (size_t)v->len, &dm);
      snprintf(nm, sizeof(nm), "decodeEx '%s' accepted == %d", v->label, v->expectOk);
      CHECK(ok == (v->expectOk != 0), nm);
      if (v->expectOk) {
        snprintf(nm, sizeof(nm), "decodeEx '%s' lat/lon/time/source/precision", v->label);
        CHECK(dm.latI == (int32_t)v->latI && dm.lonI == (int32_t)v->lonI &&
              dm.time == (uint32_t)v->time && dm.locSrc == v->locSrc &&
              dm.precBits == v->precBits, nm);
        int32_t la = 0, lo = 0;
        uint32_t tt = 0;
        snprintf(nm, sizeof(nm), "decode '%s' through the old meshPosParse too", v->label);
        CHECK(meshPosParse(v->bytes, (size_t)v->len, &la, &lo, &tt) && la == dm.latI &&
              lo == dm.lonI && tt == dm.time, nm);
      }
    }
    // The stock-shaped vector of 0.9.7x carried location_source 2 and precision_bits 32 all along.
    MeshPosMsg sm;
    CHECK(meshPosParseEx(POS_DEC[1].bytes, (size_t)POS_DEC[1].len, &sm) && sm.locSrc == 2 &&
          sm.precBits == 32, "the stock-shaped Position's source (INTERNAL) and precision (32) are read");
    CHECK(!meshPosParseEx(NULL, 5, &sm) && sm.latI == 0 && !meshPosParseEx(b, 5, NULL),
          "NULL input or output refused");
  }

  /* 🛑 Until 0.9.82 this line printed "all passed" and returned 0 whatever the blocks after
   * the early return above had found: a failing waypoint-build CHECK printed FAIL and still
   * passed the suite. */
  if (failures) {
    printf("test_pos: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("test_pos: all passed\n");
  return 0;
}
