/*
 * test_replay.cpp — replay_proto.cpp vs the frozen behavior of COVEY's
 * replay.py (vectors_replay.h, generated). Byte-for-byte agreement is the
 * whole point: the request grammar (including python str.split() whitespace
 * semantics and uint32 bounds), record-line emission (channel escaping,
 * newline scrubbing, emoji as UTF-8 bytes), summaries, and the RPL
 * machine-traffic claim. Plus the C-only edges the reference cannot express:
 * buffer caps must fail loudly (-1), never truncate a record silently.
 */
#include "../WiPhone/replay_proto.h"
#include "vectors_replay.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

static void ok(bool cond, const char* what) {
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  }
}

static void testRequests() {
  printf("\n\033[1mrequests: parse exactly what the reference parses\033[0m\n");
  char what[160];
  for (int i = 0; i < RPL_REQ_VEC_N; i++) {
    const RplReqVec* v = &RPL_REQ_VEC[i];
    uint32_t t1 = 0, t2 = 0;
    int maxn = 0;
    bool got = replayParseRequest(v->text, &t1, &t2, &maxn);
    snprintf(what, sizeof(what), "req %d validity ('%.40s')", i, v->text);
    ok(got == (v->valid != 0), what);
    if (got && v->valid) {
      snprintf(what, sizeof(what), "req %d fields", i);
      ok(t1 == v->t1 && t2 == v->t2 && maxn == v->maxn, what);
    }
  }
}

static void testRecords() {
  printf("\n\033[1mrecord lines: byte-identical to the reference\033[0m\n");
  char buf[512], what[160];
  for (int i = 0; i < RPL_REC_VEC_N; i++) {
    const RplRecVec* v = &RPL_REC_VEC[i];
    int n = replayFormatRecord(buf, sizeof(buf), v->ts, v->sender, v->chan, v->text);
    snprintf(what, sizeof(what), "rec %d formats", i);
    ok(n >= 0, what);
    snprintf(what, sizeof(what), "rec %d bytes match ('%.40s')", i, v->line);
    ok(n >= 0 && strcmp(buf, v->line) == 0, what);
  }
}

static void testSummaries() {
  printf("\n\033[1msummaries\033[0m\n");
  char buf[64], what[96];
  for (int i = 0; i < RPL_SUM_VEC_N; i++) {
    const RplSumVec* v = &RPL_SUM_VEC[i];
    int n = replayFormatSummary(buf, sizeof(buf), v->sent, v->gap != 0);
    snprintf(what, sizeof(what), "sum %d matches '%s'", i, v->text);
    ok(n >= 0 && strcmp(buf, v->text) == 0, what);
  }
  ok(replayFormatSummary(buf, sizeof(buf), -1, false) == -1,
     "negative count refused");
}

static void testClaim() {
  printf("\n\033[1mthe RPL machine-traffic claim\033[0m\n");
  char what[96];
  for (int i = 0; i < RPL_IS_REPLAY_N; i++) {
    snprintf(what, sizeof(what), "claims '%.30s'", RPL_IS_REPLAY[i]);
    ok(replayIsReplayText(RPL_IS_REPLAY[i]), what);
  }
  for (int i = 0; i < RPL_NOT_REPLAY_N; i++) {
    snprintf(what, sizeof(what), "does not claim '%.30s'", RPL_NOT_REPLAY[i]);
    ok(!replayIsReplayText(RPL_NOT_REPLAY[i]), what);
  }
  ok(!replayIsReplayText(NULL), "NULL is not claimed");
}

static void testCaps() {
  printf("\n\033[1mC-only edges: caps fail loudly, never truncate\033[0m\n");
  char tiny[24];
  ok(replayFormatRecord(tiny, sizeof(tiny), 1755791640, 0x62B8D2FDu,
                        "Howe group", "a text that cannot fit") == -1,
     "over-cap record refuses rather than truncates");
  char big[512];
  char longchan[80];
  memset(longchan, ' ', sizeof(longchan) - 1);
  longchan[sizeof(longchan) - 1] = '\0';
  ok(replayFormatRecord(big, sizeof(big), 1, 1, longchan, "x") == -1,
     "a channel name whose escape overflows refuses");
  uint32_t t1, t2;
  int mx;
  ok(!replayParseRequest(NULL, &t1, &t2, &mx), "NULL request refused");
}

int main() {
  testRequests();
  testRecords();
  testSummaries();
  testClaim();
  testCaps();

  printf("\n%s%d passed, %d failed\033[0m\n", g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
