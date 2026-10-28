#!/usr/bin/env python3
"""Generate tests/vectors_wire.h — interop vectors for WiPhone/mesh_wire.cpp.

THE POINT: this phone hand-rolls the Meshtastic Data and User protobufs as literal tag
bytes (no nanopb anywhere in the firmware). That is small and fast and it rots SILENTLY:
a field number that drifts does not crash, it just makes this phone invisible to every
other radio on the mesh. The only honest check is against the protobuf runtime every
other node actually uses, so that is what this emits.

Covers, all from upstream's own definitions:
  * Data  encode — the bytes we transmit must equal the bytes python produces
  * User  encode — the NODEINFO payload, same bar
  * Data/User decode — parse bytes PYTHON produced, including messages carrying fields
    this firmware does not know, which is the forward-compatibility property that lets
    upstream keep adding fields without breaking us
  * PortNum values — that our #defines still name the same apps

NOT covered here, deliberately, so nobody mistakes this for full coverage:
  * The 16-byte on-air PacketHeader. It is firmware-internal (src/mesh/RadioInterface.h),
    NOT in the protobufs, so the python package cannot speak to it. test_wire.cpp checks
    its layout and flag packing structurally instead, which catches packing/field drift
    on our side but cannot catch upstream changing the header.
  (The channel hash USED to be listed here as uncovered, because it lived in
   mesh_crypto.cpp beside AES and could not build on a host. It was split into
   mesh_hash.cpp on 2026-09-02 and is now pinned below like everything else.)

    python3 tools/gen_wire_vectors.py --out tests/vectors_wire.h

Needs: pip install meshtastic   (the generated header is what is committed; this script
only regenerates it — run it after any upstream bump and re-run ./tests/run_tests.sh)
"""

import argparse
import sys

try:
    from meshtastic.protobuf import mesh_pb2, portnums_pb2
    import meshtastic.util as mt_util
    from importlib.metadata import version as _pkgver
except ImportError:
    sys.exit("need the meshtastic package: pip install meshtastic")


# ── Data encode cases ────────────────────────────────────────────────────────────────
# (label, portnum, payload, want_response, request_id)
DATA_CASES = [
    ("text_plain",     1, b"hello mesh",                False, 0),
    ("text_empty",     1, b"",                          False, 0),
    ("text_wantresp",  1, b"who are you",               True,  0),
    ("position",       3, bytes.fromhex("0d1a2b3c4d"),  False, 0),
    ("nodeinfo",       4, bytes.fromhex("0a0921303034"), False, 0),
    ("routing_ack",    5, bytes.fromhex("0800"),        False, 0x11223344),
    ("routing_nak",    5, bytes.fromhex("0801"),        False, 0xDEADBEEF),
    ("waypoint",       8, bytes.fromhex("0d01000000"),  False, 0),
    # 234 is MESH_TEXT_LEN, the payload BUFFER size. ⚠ It is NOT what a text message can
    # be: meshTxText() rejects textLen > MESH_TEXT_LEN - 1, so 233 is the real text cap.
    # 234 is kept as an encoder case anyway because it is past the point where the length
    # varint goes from one byte to two, which is exactly what a hand-rolled encoder gets
    # wrong. Non-text portnums reach these sizes on their own.
    ("text_max",       1, bytes(range(256))[:234],      False, 0),
    ("text_127",       1, b"x" * 127,                   False, 0),
    ("text_128",       1, b"x" * 128,                   False, 0),
    # ⚠ PORTNUM IS A VARINT, not a byte. 71 is the largest this firmware speaks today, so
    # the one-byte encoder it used to have was never wrong on air — but Meshtastic's enum
    # runs to 511 (PRIVATE_APP is 256) and the truncation was silent. 127/128 straddle the
    # continuation bit; 256 is the case that used to encode as portnum ZERO.
    ("port_127",     127, b"x",                         False, 0),
    ("port_128",     128, b"x",                         False, 0),
    ("port_256",     256, b"x",                         False, 0),
]

# ── User (NODEINFO payload) encode cases ─────────────────────────────────────────────
# (label, node_num, long_name, short_name, public_key or None). The id field is derived
# here the same way the firmware does it — "!%08x" — so the test covers that spelling too.
KEY32 = bytes(range(0x20, 0x40))
USER_CASES = [
    ("user_plain",  0x00449040, "WiPhone-{user}", "NHWP", None),
    ("user_pki",    0x00449040, "WiPhone-{user}", "NHWP", KEY32),
    ("user_small",  0x0000007f, "A",            "A",    None),
    ("user_utf8",   0x12345678, "{user}’s phone", "NHé", None),
    ("user_hi_bit", 0xFFFFFFFF, "Top Node",     "TOPN", KEY32),
    # proto3 omits a string field holding its default, so an empty name must emit NOTHING.
    # This firmware used to emit `12 00` / `1a 00` — legal, non-canonical, and it also made
    # our own decoder show the node with no name at all.
    ("user_no_long",  0x00449040, "",             "NHWP", None),
    ("user_no_short", 0x00449040, "WiPhone-{user}", "",     None),
    ("user_no_names", 0x00449040, "",             "",     KEY32),
]


def c_bytes(b: bytes) -> str:
    return ", ".join(f"0x{x:02x}" for x in b) if b else "0"


def emit(out, argv):
    ver = _pkgver("meshtastic")
    w = out.write
    w("/*\n")
    w(" * vectors_wire.h — GENERATED by tools/gen_wire_vectors.py. Do not hand-edit.\n")
    w(" *\n")
    w(f" * Produced by the real Meshtastic protobuf runtime, package version {ver}.\n")
    w(" * Every byte array here is what upstream's own encoder emits; tests/test_wire.cpp\n")
    w(" * asserts WiPhone/mesh_wire.cpp produces and parses exactly these.\n")
    w(" *\n")
    w(" * ⚠ AFTER AN UPSTREAM BUMP: pip install -U meshtastic, re-run the generator, then\n")
    w(" * ./tests/run_tests.sh. A diff in this file IS the protocol drift report — read it\n")
    w(" * before regenerating, because regenerating is what makes the failure go away.\n")
    w(" */\n\n")
    w("#ifndef VECTORS_WIRE_H\n#define VECTORS_WIRE_H\n\n")
    w(f'#define WIRE_VEC_MESHTASTIC_VERSION "{ver}"\n\n')

    # PortNum values, straight from upstream's enum.
    w("/* PortNum values, from upstream's portnums.proto. test_wire.cpp asserts the\n")
    w(" * firmware's own #defines still equal these — a renumbering here would send our\n")
    w(" * packets to the wrong app on every other radio. */\n")
    for name, macro in [("TEXT_MESSAGE_APP", "TEXT_MESSAGE"), ("POSITION_APP", "POSITION"),
                        ("NODEINFO_APP", "NODEINFO"), ("ROUTING_APP", "ROUTING"),
                        ("WAYPOINT_APP", "WAYPOINT"), ("NEIGHBORINFO_APP", "NEIGHBORINFO")]:
        w(f"#define WIRE_UP_PORT_{macro:<13} {portnums_pb2.PortNum.Value(name)}\n")
    w("\n")

    # ---- Channel hash, against meshtastic.util.generate_channel_hash --------------------
    # Full key bytes on both sides (upstream expands a 1-byte PSK itself; the phone stores
    # channels already expanded, so handing both the same 16/32 bytes is the fair test).
    DEFAULT16 = bytes.fromhex("d4f1bb3a20290759f0bcffabcf4e6901")
    K32A = bytes(range(0x40, 0x60))
    K32B = bytes((i * 37 + 11) & 0xFF for i in range(32))
    hashes = [
        ("LongFast",   DEFAULT16),
        ("Howe group", K32A),
        ("hunt-group", K32B),
        ("booksync",   K32A),
        ("",           K32B),                       # empty name is legal
        ("Cafe\u0301 ridge", K32A),                # combining accent: multi-byte UTF-8
        ("LongFast",   bytes(16)),                  # all-zero key
    ]
    w("/* Channel hash vectors from meshtastic.util.generate_channel_hash. mesh_hash.cpp is\n")
    w(" * Arduino-free since 2026-09-02 so test_wire.cpp compiles it and asserts these. */\n")
    w("typedef struct {\n  const char* label;\n  const char* name;\n  const unsigned char* key;\n"
      "  int keyLen;\n  unsigned char hash;\n} WireHashVec;\n\n")
    for i, (nm, key) in enumerate(hashes):
        w(f"static const unsigned char WIRE_HASH_K_{i}[] = {{ {c_bytes(key)} }};\n")
    w("\nstatic const WireHashVec WIRE_HASH[] = {\n")
    for i, (nm, key) in enumerate(hashes):
        h = mt_util.generate_channel_hash(nm, key)
        def esc2(t):
            out, prev = "", False
            for b in t.encode("utf-8"):
                ch = chr(b)
                if b > 0x7e or b < 0x20 or ch in '"\\':
                    out += f"\\x{b:02x}"; prev = True
                else:
                    if prev and ch in "0123456789abcdefABCDEF":
                        out += '""'
                    out += ch; prev = False
            return out
        w(f'  {{ "hash_{i}", "{esc2(nm)}", WIRE_HASH_K_{i}, {len(key)}, 0x{h:02x} }},\n')
    w("};\n")
    w(f"#define WIRE_HASH_N {len(hashes)}\n\n")

    # ---- Data encode ----
    w("typedef struct {\n  const char* label;\n  int portnum;\n"
      "  const unsigned char* payload;\n  int payloadLen;\n  int wantResponse;\n"
      "  unsigned long requestId;\n  const unsigned char* bytes;\n  int len;\n} WireDataVec;\n\n")
    for i, (label, port, payload, wr, rid) in enumerate(DATA_CASES):
        d = mesh_pb2.Data()
        d.portnum = port
        d.payload = payload
        if wr:
            d.want_response = True
        if rid:
            d.request_id = rid
        d.bitfield = 2 if wr else 0        # bit1 = want_response; bit0 (ok-to-MQTT) off
        enc = d.SerializeToString()
        w(f"static const unsigned char WIRE_DATA_PL_{i}[] = {{ {c_bytes(payload)} }};\n")
        w(f"static const unsigned char WIRE_DATA_B_{i}[]  = {{ {c_bytes(enc)} }};\n")
    w("\nstatic const WireDataVec WIRE_DATA[] = {\n")
    for i, (label, port, payload, wr, rid) in enumerate(DATA_CASES):
        d = mesh_pb2.Data()
        d.portnum = port; d.payload = payload
        if wr: d.want_response = True
        if rid: d.request_id = rid
        d.bitfield = 2 if wr else 0
        enc = d.SerializeToString()
        w(f'  {{ "{label}", {port}, WIRE_DATA_PL_{i}, {len(payload)}, {int(wr)}, '
          f'{rid}UL, WIRE_DATA_B_{i}, {len(enc)} }},\n')
    w("};\n")
    w(f"#define WIRE_DATA_N {len(DATA_CASES)}\n\n")

    # ---- User encode ----
    w("typedef struct {\n  const char* label;\n  unsigned long nodeNum;\n  const char* longName;\n"
      "  const char* shortName;\n  const unsigned char* pubKey;\n  int hasKey;\n"
      "  const unsigned char* bytes;\n  int len;\n} WireUserVec;\n\n")
    for i, (label, node, ln, sn, key) in enumerate(USER_CASES):
        u = mesh_pb2.User()
        u.id = "!%08x" % node; u.long_name = ln; u.short_name = sn
        if key:
            u.public_key = key
        enc = u.SerializeToString()
        w(f"static const unsigned char WIRE_USER_K_{i}[] = {{ {c_bytes(key or b'')} }};\n")
        w(f"static const unsigned char WIRE_USER_B_{i}[] = {{ {c_bytes(enc)} }};\n")
    w("\nstatic const WireUserVec WIRE_USER[] = {\n")
    def esc(t):
        """C string literal body. A bare \\xNN is GREEDY in C/C++ — it swallows any
        following hex digit, so "Caf\\xc3\\xa9a" lexes \\xa9a as one (overflowing) escape.
        Splitting the literal (`"\\xc3\\xa9" "a"`) terminates it; adjacent literals
        concatenate, so the bytes are unchanged. Found by review 2026-09-02: the committed
        cases happened to be safe, which is exactly how this ships broken later."""
        out, prev_escape = "", False
        for b in t.encode("utf-8"):
            ch = chr(b)
            if b > 0x7e or b < 0x20 or ch in '"\\':
                out += f"\\x{b:02x}"
                prev_escape = True
            else:
                if prev_escape and ch in "0123456789abcdefABCDEF":
                    out += '""'
                out += ch
                prev_escape = False
        return out
    for i, (label, node, ln, sn, key) in enumerate(USER_CASES):
        u = mesh_pb2.User()
        u.id = "!%08x" % node; u.long_name = ln; u.short_name = sn
        if key:
            u.public_key = key
        enc = u.SerializeToString()
        w(f'  {{ "{label}", 0x{node:08x}UL, "{esc(ln)}", "{esc(sn)}", '
          f'WIRE_USER_K_{i}, {1 if key else 0}, WIRE_USER_B_{i}, {len(enc)} }},\n')
    w("};\n")
    w(f"#define WIRE_USER_N {len(USER_CASES)}\n\n")

    # ---- Decode: bytes python produced, including fields we do not know ----
    w("/* Decode cases. The 'unknown_*' ones carry Data/User fields this firmware does not\n"
      " * parse (dest, source, reply_id, emoji / macaddr, hw_model, role). They exist to\n"
      " * prove the parsers SKIP what they do not recognise instead of choking — the exact\n"
      " * property that lets Meshtastic keep adding fields without breaking this phone. */\n")
    w("typedef struct {\n  const char* label;\n  const unsigned char* bytes;\n  int len;\n"
      "  int portnum;\n  const unsigned char* payload;\n  int payloadLen;\n"
      "  int wantResp;\n  unsigned long requestId;\n} WireParseVec;\n\n")

    parse_cases = []
    # plain
    d = mesh_pb2.Data(); d.portnum = 1; d.payload = b"plain text"; d.bitfield = 0
    parse_cases.append(("parse_plain", d.SerializeToString(), 1, b"plain text", 0, 0))
    # every field we DO read
    d = mesh_pb2.Data(); d.portnum = 5; d.payload = b"\x08\x00"
    d.want_response = True; d.request_id = 0xCAFEBABE; d.bitfield = 2
    parse_cases.append(("parse_all_known", d.SerializeToString(), 5, b"\x08\x00", 1, 0xCAFEBABE))
    # fields we do NOT read, interleaved around the ones we do
    d = mesh_pb2.Data(); d.portnum = 1; d.payload = b"future proof"
    d.dest = 0x11111111; d.source = 0x22222222; d.reply_id = 0x33333333
    d.emoji = 0x44444444; d.want_response = True; d.bitfield = 2
    parse_cases.append(("parse_unknown_fields", d.SerializeToString(), 1, b"future proof", 1, 0))
    # ⚠ THE CASE ABOVE IS WEAKER THAN IT LOOKS and the review caught it: protobuf serialises
    # in field-number order, so the unknown fields (4,5,7,8) all land AFTER everything it
    # asserts. A broken skip could not change the result. THIS one puts request_id (6)
    # BETWEEN the unknown 4/5 and the unknown 7/8, so reading it correctly proves the parser
    # actually walked over two unknown fields to reach it.
    d = mesh_pb2.Data(); d.portnum = 5; d.payload = b"\x08\x00"
    d.dest = 0x11111111; d.source = 0x22222222; d.request_id = 0xCAFEBABE
    d.reply_id = 0x33333333; d.emoji = 0x44444444; d.bitfield = 0
    parse_cases.append(("parse_skip_then_read", d.SerializeToString(), 5, b"\x08\x00", 0, 0xCAFEBABE))
    # Every decode payload above is under 128 bytes, so the LENGTH VARINT is one byte and the
    # multi-byte path in the parser was never exercised on decode. 200 bytes forces two.
    big = bytes((i * 7 + 3) & 0xFF for i in range(200))
    d = mesh_pb2.Data(); d.portnum = 1; d.payload = big; d.bitfield = 0
    parse_cases.append(("parse_long_payload", d.SerializeToString(), 1, big, 0, 0))
    # The deliberate 0.9.54 behaviour change, from the receiving side: upstream omits an
    # empty payload entirely, so field 2 is ABSENT. The parser must report length 0 and the
    # NODEINFO/text consumers must not treat it as a packet with a payload.
    d = mesh_pb2.Data(); d.portnum = 1; d.payload = b""; d.bitfield = 0
    parse_cases.append(("parse_absent_payload", d.SerializeToString(), 1, b"", 0, 0))
    for i, (label, enc, port, payload, wr, rid) in enumerate(parse_cases):
        w(f"static const unsigned char WIRE_PARSE_B_{i}[]  = {{ {c_bytes(enc)} }};\n")
        w(f"static const unsigned char WIRE_PARSE_PL_{i}[] = {{ {c_bytes(payload)} }};\n")
    w("\nstatic const WireParseVec WIRE_PARSE[] = {\n")
    for i, (label, enc, port, payload, wr, rid) in enumerate(parse_cases):
        w(f'  {{ "{label}", WIRE_PARSE_B_{i}, {len(enc)}, {port}, WIRE_PARSE_PL_{i}, '
          f'{len(payload)}, {wr}, {rid}UL }},\n')
    w("};\n")
    w(f"#define WIRE_PARSE_N {len(parse_cases)}\n\n")

    # ---- User decode, with unknown fields ----
    w("typedef struct {\n  const char* label;\n  const unsigned char* bytes;\n  int len;\n"
      "  const char* expectName;\n  const unsigned char* expectKey;\n  int hasKey;\n} WireUParseVec;\n\n")
    uparse = []
    u = mesh_pb2.User(); u.id = "!00449040"; u.long_name = "Long Wins"; u.short_name = "SHRT"
    uparse.append(("uparse_prefers_long", u.SerializeToString(), "Long Wins", None))
    u = mesh_pb2.User(); u.id = "!00449040"; u.short_name = "ONLY"
    uparse.append(("uparse_short_fallback", u.SerializeToString(), "ONLY", None))
    u = mesh_pb2.User(); u.id = "!00449040"; u.long_name = "Keyed Node"; u.short_name = "KEYD"
    u.public_key = KEY32; u.macaddr = b"\x01\x02\x03\x04\x05\x06"
    u.hw_model = 9; u.is_licensed = True; u.role = 1
    uparse.append(("uparse_unknown_fields", u.SerializeToString(), "Keyed Node", KEY32))
    # ⚠ mesh_wire.h promises the key is taken ONLY when it is exactly 32 bytes — stock strips
    # it for licensed/ham operators, so a short or absent key is normal traffic, not an error.
    # Nothing tested that guard: every keyed vector carried exactly 32 bytes, so deleting the
    # length check passed 84/84.
    u = mesh_pb2.User(); u.id = "!00449040"; u.long_name = "Short Key"; u.short_name = "SK16"
    u.public_key = bytes(range(16))
    uparse.append(("uparse_key_wrong_len", u.SerializeToString(), "Short Key", None))
    # An empty long_name is OMITTED by proto3, so the short_name fallback must fire.
    u = mesh_pb2.User(); u.id = "!00449040"; u.short_name = "FBCK"; u.hw_model = 9
    uparse.append(("uparse_empty_long_omitted", u.SerializeToString(), "FBCK", None))
    for i, (label, enc, name, key) in enumerate(uparse):
        w(f"static const unsigned char WIRE_UP_B_{i}[] = {{ {c_bytes(enc)} }};\n")
        w(f"static const unsigned char WIRE_UP_K_{i}[] = {{ {c_bytes(key or b'')} }};\n")
    w("\nstatic const WireUParseVec WIRE_UPARSE[] = {\n")
    for i, (label, enc, name, key) in enumerate(uparse):
        w(f'  {{ "{label}", WIRE_UP_B_{i}, {len(enc)}, "{name}", WIRE_UP_K_{i}, '
          f'{1 if key else 0} }},\n')
    w("};\n")
    w(f"#define WIRE_UPARSE_N {len(uparse)}\n\n")
    w("#endif // VECTORS_WIRE_H\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    with open(a.out, "w") as f:
        emit(f, sys.argv)
    print(f"wrote {a.out} (meshtastic {_pkgver('meshtastic')})")


if __name__ == "__main__":
    main()
