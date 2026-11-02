/*
 * replay_proto.h — mesh history replay, the wire format (v1).
 *
 * See docs/replay-spec.md. COVEY asks `RPL? t1 t2 max` on the booksync channel
 * after a blind window; this phone answers with `RPL <ts> <sender8> <chan>
 * <text>` records (oldest first, several per packet, original timestamps) and
 * closes with `RPL. sent gap`. COVEY's covey_ui/replay.py is the reference
 * implementation; tests/test_replay.cpp pins this file against vectors
 * GENERATED from it (tools/gen_replay_vectors.py) — byte-identical output is
 * the contract, because the wire format that drifts is the wire format that
 * "worked yesterday".
 *
 * Arduino-free ON PURPOSE: the host test suite compiles this file with the
 * Mac's compiler (the same rule booksync.cpp lives by).
 */

#ifndef REPLAY_PROTO_H
#define REPLAY_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Payload budget per packet — clears Meshtastic's 234-byte ceiling with margin. */
#define REPLAY_PACKET_BUDGET 180

/* Any replay-protocol line, unknown future forms included — machine traffic,
 * never chat (a v2 frame must not render as a bubble on a v1 device). */
bool replayIsReplayText(const char* text);

/* `RPL? <t1> <t2> <max>` -> false on anything malformed, inverted, or
 * non-positive (a garbled request must not ask for the whole ring). */
bool replayParseRequest(const char* text, uint32_t* t1, uint32_t* t2, int* maxn);

/* One record line: `RPL <ts> <sender8> <chan> <text>`. The channel name is
 * percent-escaped for exactly '%' and ' ' ('Howe group' is real); text rides
 * verbatim minus newlines. Returns the line length, or -1 if it cannot fit
 * cap. Matches replay.py's make_record_line byte for byte. */
int replayFormatRecord(char* out, size_t cap,
                       uint32_t ts, uint32_t sender,
                       const char* chanName, const char* text);

/* `RPL. <sent> <gap>` — the closing summary. Returns length or -1. */
int replayFormatSummary(char* out, size_t cap, int sent, bool gap);

#endif // REPLAY_PROTO_H
