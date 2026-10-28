/*
 * test_sms_mirror.cpp — the CSM1 record that carries a text between COVEY and the phone.
 *
 * Everything guarded here is silent when wrong. A mirror record does not appear in the
 * Chats list, does not chime and does not log at a level this build compiles in, so the
 * only symptoms of a bad parse are "a text is missing" or "a text is in there twice" —
 * noticed days later, with nothing to look at. The bytes have to be right the first time.
 *
 * The three that would actually bite:
 *
 *   - THE NUMBER SPELLINGS. The same person reaches this store as `4255550142` (COVEY),
 *     `14255550142@seattle1.voip.ms` (phonebook) and `+1 (425) 555-0142` (typed). If they
 *     do not reduce to one identity, one correspondent becomes three threads.
 *   - THE MULTI-LINE TEXT. A raw newline in a last-position field splits a record in half
 *     on a line-oriented transport and takes the NEXT record down with it.
 *   - THE PAIRING PREDICATE, which is what stops a text the phone already has from being
 *     stored a second time when COVEY mirrors it back.
 */
#include "../WiPhone/sms_mirror.h"
#include "vectors_smsmirror.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
static const char* g_group = "";

static void group(const char* name) {
  g_group = name;
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char* what) {
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
    printf("  \033[31mFAIL\033[0m %s :: %s\n", g_group, what);
  }
}

static void eqStr(const char* got, const char* want, const char* what) {
  if (got && want && strcmp(got, want) == 0) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s  want=\"%s\" got=\"%s\"\n",
         g_group, what, want ? want : "(null)", got ? got : "(null)");
}

static void eqI64(long long got, long long want, const char* what) {
  if (got == want) {
    g_pass++;
    return;
  }
  g_fail++;
  printf("  \033[31mFAIL\033[0m %s :: %s  want=%lld got=%lld\n", g_group, what, want, got);
}

static SmsMirrorRecord rec(long long id, const char* peer, bool out, unsigned long ts,
                           const char* text) {
  SmsMirrorRecord r;
  memset(&r, 0, sizeof(r));
  r.id = (int64_t)id;
  snprintf(r.peer, sizeof(r.peer), "%s", peer);
  r.out = out;
  r.ts = (uint32_t)ts;
  snprintf(r.text, sizeof(r.text), "%s", text);
  return r;
}

// ---------------------------------------------------------------- digits
static void testDigits() {
  group("a correspondent has one identity, however it is spelled");

  char d[SMS_MIRROR_PEER_MAX];

  smsMirrorDigits("4255550142", d, sizeof(d));
  eqStr(d, "4255550142", "bare ten digits");

  smsMirrorDigits("14255550142", d, sizeof(d));
  eqStr(d, "4255550142", "leading country code dropped");

  smsMirrorDigits("+1 (425) 555-0142", d, sizeof(d));
  eqStr(d, "4255550142", "as a human types it");

  smsMirrorDigits("14255550142@seattle1.voip.ms", d, sizeof(d));
  eqStr(d, "4255550142", "as the phonebook holds it");

  smsMirrorDigits("sip:+14255550142@seattle1.voip.ms", d, sizeof(d));
  eqStr(d, "4255550142", "as an inbound SIP MESSAGE arrives");

  /* The host is not identity, and this is the case that proves it: an IP host is full of
   * digits that would otherwise be appended to the number. */
  smsMirrorDigits("4255550142@192.168.1.55", d, sizeof(d));
  eqStr(d, "4255550142", "an IP host contributes no digits");

  ok(smsMirrorDigits("", d, sizeof(d)) == 0, "empty string has no identity");
  ok(smsMirrorDigits("nobody", d, sizeof(d)) == 0, "a digitless string has no identity");
  ok(smsMirrorDigits(NULL, d, sizeof(d)) == 0, "NULL has no identity");

  /* Refuse rather than truncate. A truncated 40-digit string would land on some real
   * correspondent's number and quietly merge two threads. */
  ok(smsMirrorDigits("1234567890123456789012345678901234567890", d, sizeof(d)) == 0,
     "an over-long digit run is refused, not cut down to a real number");
}

// ---------------------------------------------------------------- round trip
static void testRoundTrip() {
  group("a record survives the wire unchanged");

  SmsMirrorRecord in = rec(884451201LL, "4255550142", false, 0x68a1b2c3UL,
                           "on my way, be there in ten");
  char line[SMS_MIRROR_LINE_MAX];
  ok(smsMirrorPack(&in, line, sizeof(line)), "packs");
  ok(smsMirrorIsMirrorText(line), "and is recognised as mirror traffic");

  SmsMirrorRecord got;
  ok(smsMirrorUnpack(line, &got), "unpacks");
  eqI64((long long)got.id, 884451201LL, "id");
  eqStr(got.peer, "4255550142", "peer");
  ok(got.out == false, "direction: received");
  eqI64((long long)got.ts, 0x68a1b2c3LL, "timestamp");
  eqStr(got.text, "on my way, be there in ten", "text");

  SmsMirrorRecord sent = rec(884451202LL, "+1 (425) 555-0142", true, 1, "ok");
  ok(smsMirrorPack(&sent, line, sizeof(line)), "packs a sent one");
  ok(smsMirrorUnpack(line, &got), "unpacks it");
  ok(got.out == true, "direction: sent");
  eqStr(got.peer, "4255550142", "peer normalised on the way out");
}

static void testTextIsTakenVerbatim() {
  group("the text is the rest of the line, whatever is in it");

  char line[SMS_MIRROR_LINE_MAX];
  SmsMirrorRecord got;

  // Spaces need no escaping precisely because nothing is parsed after the text.
  SmsMirrorRecord spaces = rec(1, "4255550142", true, 2,
                               "sure  -  see you at 5 (bring the  map)");
  ok(smsMirrorPack(&spaces, line, sizeof(line)), "packs a text full of spaces");
  ok(smsMirrorUnpack(line, &got), "unpacks it");
  eqStr(got.text, "sure  -  see you at 5 (bring the  map)", "runs of spaces are preserved");

  // A leading space is part of the message: only ONE separator is consumed.
  SmsMirrorRecord lead = rec(2, "4255550142", true, 2, " leading space");
  ok(smsMirrorPack(&lead, line, sizeof(line)), "packs a leading space");
  ok(smsMirrorUnpack(line, &got), "unpacks it");
  eqStr(got.text, " leading space", "a leading space is not eaten");

  SmsMirrorRecord empty = rec(3, "4255550142", false, 2, "");
  ok(smsMirrorPack(&empty, line, sizeof(line)), "packs an empty text");
  ok(smsMirrorUnpack(line, &got), "an empty text is not a parse failure");
  eqStr(got.text, "", "and comes back empty");

  // A text that itself looks like a record must not be re-parsed as one.
  SmsMirrorRecord nested = rec(4, "4255550142", false, 2, "CSM1 99 5551234567 o 1 gotcha");
  ok(smsMirrorPack(&nested, line, sizeof(line)), "packs a text that looks like a record");
  ok(smsMirrorUnpack(line, &got), "unpacks it");
  eqStr(got.text, "CSM1 99 5551234567 o 1 gotcha", "the nested record stays text");
  eqI64((long long)got.id, 4, "and the real id is still the outer one");
}

static void testMultiLine() {
  group("a multi-line text cannot break the line-oriented transport");

  SmsMirrorRecord in = rec(77, "4255550142", false, 9, "line one\nline two\r\nline three");
  char line[SMS_MIRROR_LINE_MAX];
  ok(smsMirrorPack(&in, line, sizeof(line)), "packs");

  ok(strchr(line, '\n') == NULL, "no raw newline survives into the wire form");
  ok(strchr(line, '\r') == NULL, "no raw carriage return either");

  SmsMirrorRecord got;
  ok(smsMirrorUnpack(line, &got), "unpacks");
  eqStr(got.text, "line one\nline two\r\nline three", "and the newlines come back exactly");

  // A backslash the user actually typed must survive being the escape character.
  SmsMirrorRecord slash = rec(78, "4255550142", true, 9, "C:\\path\\n not a newline");
  ok(smsMirrorPack(&slash, line, sizeof(line)), "packs a literal backslash");
  ok(smsMirrorUnpack(line, &got), "unpacks it");
  eqStr(got.text, "C:\\path\\n not a newline", "a typed \\n stays two characters");

  /* The transport strips the CRLF an HTTP body puts on each line. That is only safe
   * because a real newline is escaped, which is the whole point of the escaping. */
  SmsMirrorRecord plain = rec(79, "4255550142", true, 9, "no newlines here");
  char withCrlf[SMS_MIRROR_LINE_MAX + 4];
  ok(smsMirrorPack(&plain, line, sizeof(line)), "packs");
  snprintf(withCrlf, sizeof(withCrlf), "%s\r\n", line);
  ok(smsMirrorUnpack(withCrlf, &got), "unpacks a line delivered with CRLF");
  eqStr(got.text, "no newlines here", "and the CRLF is not part of the message");
}

static void testFits() {
  group("the worst realistic record still fits a packet");

  /* ⚠ 160 EXPLICITLY, not SMS_MIRROR_TEXT_MAX. Those used to be the same number and are not
   * any more, and conflating them is what this asserts against: 160 is the SEND limit (what
   * VoIP.ms accepts and what must fit a LoRa packet), while the buffer is twice that because
   * an inbound CONCATENATED SMS arrives as one longer record. */
  char text[161];
  memset(text, 'W', sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';
  ok(strlen(text) == 160, "a maximum-length SMS is 160 characters");

  SmsMirrorRecord big = rec(2147483647LL, "4255550142", false, 0xffffffffUL, text);
  char line[SMS_MIRROR_LINE_MAX];
  ok(smsMirrorPack(&big, line, sizeof(line)), "packs");
  ok(strlen(line) <= SMS_MIRROR_MESH_MAX,
     "and fits the on-air budget with the largest id and timestamp");

  SmsMirrorRecord got;
  ok(smsMirrorUnpack(line, &got), "unpacks");
  eqStr(got.text, text, "all 160 characters survive");

  /* A CONCATENATED inbound text is longer than anything we could send, and must survive:
   * it does not fit a packet (the mesh sender skips it) but the LAN path carries it whole.
   * Truncating it instead would parse perfectly and quietly lose the end of the message. */
  char concat[SMS_MIRROR_TEXT_MAX];
  memset(concat, 'C', sizeof(concat) - 1);
  concat[sizeof(concat) - 1] = '\0';
  ok(strlen(concat) == 320, "an inbound concatenated SMS can be 320 characters");
  SmsMirrorRecord longIn = rec(5, "4255550142", false, 7, concat);
  char meshOnly[SMS_MIRROR_MESH_MAX];
  ok(!smsMirrorPack(&longIn, meshOnly, sizeof(meshOnly)),
     "...which will not fit a LoRa packet");
  ok(smsMirrorPack(&longIn, line, sizeof(line)), "...but does fit the LAN buffer");
  unsigned truncBefore = smsMirrorTruncations;
  ok(smsMirrorUnpack(line, &got), "...and unpacks");
  eqStr(got.text, concat, "...with all 320 characters intact");
  ok(smsMirrorTruncations == truncBefore, "...and nothing was counted as truncated");

  /* Escaping can push a record past the radio's budget. It must FAIL rather than
   * truncate: a truncated copy would not match the phone's own copy of the same text and
   * would be stored a second time. The LAN path, with a bigger buffer, still carries it. */
  char nl[SMS_MIRROR_TEXT_MAX];
  for (size_t i = 0; i < sizeof(nl) - 1; i++) {
    nl[i] = '\n';
  }
  nl[sizeof(nl) - 1] = '\0';
  SmsMirrorRecord pathological = rec(1, "4255550142", false, 1, nl);
  char meshBuf[SMS_MIRROR_MESH_MAX];
  ok(!smsMirrorPack(&pathological, meshBuf, sizeof(meshBuf)),
     "160 newlines will not fit a packet, and pack says so");
  eqStr(meshBuf, "", "a failed pack leaves nothing behind to send by accident");
  ok(smsMirrorPack(&pathological, line, sizeof(line)),
     "but the LAN buffer still carries it");
}

// ---------------------------------------------------------------- rejection
static void testRejects() {
  group("malformed input is refused, never guessed at");

  SmsMirrorRecord got;
  ok(!smsMirrorUnpack(NULL, &got), "NULL");
  ok(!smsMirrorUnpack("", &got), "empty");
  ok(!smsMirrorUnpack("hello there", &got), "an ordinary chat message");
  ok(!smsMirrorUnpack("CSM1", &got), "the prefix alone");
  ok(!smsMirrorUnpack("CSM1x 1 4255550142 o 1 hi", &got), "a prefix that only looks right");
  ok(!smsMirrorUnpack("CSM2 1 4255550142 o 1 hi", &got), "a future version");
  ok(!smsMirrorUnpack("CSM1 1 4255550142 o", &got), "a truncated record");
  ok(!smsMirrorUnpack("CSM1 abc 4255550142 o 1 hi", &got), "a non-numeric id");
  ok(!smsMirrorUnpack("CSM1 1 nobody o 1 hi", &got), "a peer with no digits");
  ok(!smsMirrorUnpack("CSM1 1 4255550142 x 1 hi", &got), "an unknown direction");
  ok(!smsMirrorUnpack("CSM1 1 4255550142 o zz hi", &got), "a non-hex timestamp");
  ok(!smsMirrorUnpack("CSM1 1 4255550142 o 123456789 hi", &got), "an over-long timestamp");

  ok(!smsMirrorIsMirrorText(NULL), "NULL is not mirror traffic");
  ok(!smsMirrorIsMirrorText("CSM1"), "the bare prefix is not mirror traffic");
  ok(smsMirrorIsMirrorText("CSM1 1 4255550142 o 1 hi"), "a real record is");

  // COVEY writes a negative id for a text it has sent but not yet seen echoed back.
  ok(smsMirrorUnpack("CSM1 -5 4255550142 o 1 pending", &got), "a negative id parses");
  eqI64((long long)got.id, -5, "and keeps its sign");
}

// ---------------------------------------------------------------- pairing
static void testSameMessage() {
  group("pairing a mirrored text against the phone's own copy");

  SmsMirrorRecord fromCovey = rec(900, "4255550142", true, 1000, "running late");
  SmsMirrorRecord ownCopy   = rec(0, "14255550142@seattle1.voip.ms", true, 4000, "running late");
  ok(smsMirrorSameMessage(&fromCovey, &ownCopy),
     "same text and direction, different spelling of the number, different clock");

  /* The clock is deliberately not consulted. COVEY measured VoIP.ms dates landing up to an
   * HOUR in the future, so any window tight enough to discriminate would reject real
   * matches. This asserts that a three-hour gap still pairs. */
  SmsMirrorRecord muchLater = rec(0, "4255550142", true, 1000 + 3 * 3600, "running late");
  ok(smsMirrorSameMessage(&fromCovey, &muchLater), "a three-hour clock difference still pairs");

  SmsMirrorRecord otherWay = rec(0, "4255550142", false, 1000, "running late");
  ok(!smsMirrorSameMessage(&fromCovey, &otherWay), "the other direction is a different message");

  SmsMirrorRecord otherPeer = rec(0, "5551234567", true, 1000, "running late");
  ok(!smsMirrorSameMessage(&fromCovey, &otherPeer), "another correspondent is a different message");

  SmsMirrorRecord otherText = rec(0, "4255550142", true, 1000, "running late!");
  ok(!smsMirrorSameMessage(&fromCovey, &otherText), "one character of difference is a different message");

  /* Two identical texts to the same person ARE two messages. This predicate cannot tell
   * them apart and is not supposed to — the ingest side pairs them off oldest-first, which
   * is why it must never be used to decide "already have it" on its own. */
  SmsMirrorRecord ok1 = rec(1, "4255550142", true, 10, "ok");
  SmsMirrorRecord ok2 = rec(2, "4255550142", true, 20, "ok");
  ok(smsMirrorSameMessage(&ok1, &ok2),
     "two genuinely separate identical texts look the same here, by design");
}

// ---------------------------------------------------------------- interop
/* The only test here that proves anything about the OTHER device.
 *
 * Every case above exercises this file against itself, which cannot catch the failure that
 * actually matters: COVEY encoding something this parser reads differently. These lines were
 * produced by RUNNING covey_ui/smsmirror.py (tools/gen_smsmirror_vectors.py), so a pass means
 * the bytes the radio will carry are the bytes this phone understands.
 *
 * ⚠ If this fails after a format change, regenerate the vectors — do not edit them to match.
 * They are the other device's opinion, and it is the one that has to be satisfied.
 */
static void testInterop() {
  group("COVEY's own bytes, parsed by the phone's parser");

  ok(SM_VEC_N > 0, "there are vectors to check");
  for (int i = 0; i < SM_VEC_N; i++) {
    const SmVec& v = SM_VECS[i];
    char what[160];

    snprintf(what, sizeof(what), "%s: recognised as mirror traffic", v.label);
    ok(smsMirrorIsMirrorText(v.line), what);

    SmsMirrorRecord got;
    snprintf(what, sizeof(what), "%s: parses", v.label);
    if (!smsMirrorUnpack(v.line, &got)) {
      ok(false, what);
      continue;
    }
    ok(true, what);

    snprintf(what, sizeof(what), "%s: id", v.label);
    eqI64((long long)got.id, v.id, what);
    snprintf(what, sizeof(what), "%s: peer", v.label);
    eqStr(got.peer, v.peer, what);
    snprintf(what, sizeof(what), "%s: direction", v.label);
    ok(got.out == v.out, what);
    snprintf(what, sizeof(what), "%s: timestamp", v.label);
    eqI64((long long)got.ts, (long long)v.ts, what);
    snprintf(what, sizeof(what), "%s: text recovered exactly", v.label);
    eqStr(got.text, v.text, what);

    /* And back again: what this phone would put on the wire for the same record has to be
     * what COVEY sent, or the mirror only works in one direction. */
    char reencoded[SMS_MIRROR_LINE_MAX];
    snprintf(what, sizeof(what), "%s: re-encodes to the identical line", v.label);
    if (smsMirrorPack(&got, reencoded, sizeof(reencoded))) {
      eqStr(reencoded, v.line, what);
    } else {
      ok(false, what);
    }
  }

  // All three spellings of the one number must have arrived as one identity.
  const char* e164 = NULL;
  const char* pretty = NULL;
  const char* sipuri = NULL;
  for (int i = 0; i < SM_VEC_N; i++) {
    if (!strcmp(SM_VECS[i].label, "peer_e164")) {
      e164 = SM_VECS[i].peer;
    } else if (!strcmp(SM_VECS[i].label, "peer_pretty")) {
      pretty = SM_VECS[i].peer;
    } else if (!strcmp(SM_VECS[i].label, "peer_sipuri")) {
      sipuri = SM_VECS[i].peer;
    }
  }
  ok(e164 && pretty && sipuri, "the three number spellings are present");
  if (e164 && pretty && sipuri) {
    eqStr(pretty, e164, "a human-typed number normalises like an E.164 one");
    eqStr(sipuri, e164, "and so does a SIP URI off the wire");
  }
}

int main() {
  testDigits();
  testRoundTrip();
  testTextIsTakenVerbatim();
  testMultiLine();
  testFits();
  testRejects();
  testSameMessage();
  testInterop();
  printf("\n%s%d passed, %d failed\033[0m\n", g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
