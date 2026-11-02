/*
 * kosync.h — the KOSync protocol (KOReader's reading-position sync), the pure half.
 *
 * KOSync is what KOReader, the Xteink X4's CrossPoint firmware and COVEY all speak: a tiny
 * HTTP+JSON protocol that moves ONE number per book — a whole-book percentage — keyed by a
 * document id. On the WiPhone it rides beside LoRa booksync, never instead of it:
 *
 *   the WINDOW  the phone serves KOSync for one book for a few minutes, on its own hotspot
 *               'WiPhone-Books' (192.168.4.1) in the woods, or on its WiFi address at home.
 *               An X4 joins and syncs against it. See kosync_sync.h.
 *   the CLIENT  at home, the phone pushes/pulls its place to/from the configured server
 *               (COVEY) over the WiFi it is already on. It never joins anything.
 *
 * Everything here is plain C++ with no Arduino headers — formatting, JSON, headers, the
 * server's routing and answers, the window's clock, the config file — so tests/test_kosync.cpp
 * compiles these exact bytes on a Mac. What CANNOT be tested there (sockets, the radio, the
 * card) lives in kosync_sync.cpp and is kept as thin as it can be.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * THE WIRE (contract §1 + §7, pinned by tools/gen_kosync_vectors.py)
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 *   GET  /users/auth                 200 {"authorized":"OK"} | 401
 *   GET  /syncs/progress/<document>  200 {document,percentage,progress,device,device_id,
 *                                    timestamp} | 200 {} for a document we do not know | 401
 *   PUT  /syncs/progress             body {document,progress,percentage,device,device_id}
 *                                    -> 200 {"document":..,"timestamp":<server unix s>} | 401
 *   GET  /healthcheck                200 {"state":"OK"} (no auth)
 *   POST /users/create               402 (registration is off: one user, from the config file)
 *
 *   Clients send x-auth-user: <user> and x-auth-key: <MD5(password), lower-case hex>; servers
 *   check those two and nothing else. Our clients always send Accept:
 *   application/vnd.koreader.v1+json (and Content-Type: application/json on a PUT); our
 *   server never requires it.
 *
 * 🛑 AN UNKNOWN DOCUMENT IS `200 {}`, NOT A 404 (contract §7.1 — the reference server's own
 *    answer; KOReader treats anything else as an error). CrossPoint reads {} as "0 %, nobody
 *    has synced this yet", which is exactly true.
 * ⚠ `progress` (KOReader's XPath) is sent EMPTY and ignored on read: this reader has no
 *    XPath to give, and faking one would move a KOReader device to the wrong place. A
 *    KOReader device cannot be MOVED by a percentage-only record; it can still send to us.
 */
#ifndef KOSYNC_H
#define KOSYNC_H

#include <stddef.h>
#include <stdint.h>

#define KOSYNC_ACCEPT          "application/vnd.koreader.v1+json"
/* The header line the phone's OWN home client puts on every request (kosyncBuildGet/Put): the
 * same marker the X4 fork sends and the window looks for (kosyncClientTakesPercentage below).
 * 🛑 NOT DECORATION. COVEY D-163 answers `{}` to an UNMARKED GET when the stored record has no
 * XPointer — and every record a phone wrote is one — so a phone without this line reads `{}`
 * for its own place, decides "this book is not on the server yet" (evaluateOffer) and PUTs
 * its own, possibly older, place over the newer one. On the PUT too: the phone is one client
 * and says the same thing on both verbs, as the fork does (applyAuthHeaders adds it to every
 * request); a home that sorts its writers by the marker then needs no second phone flash. */
#define KOSYNC_MARKER_LINE     "X-BookSync: 1\r\n"
#define KOSYNC_EPSILON         0.001      // |Δp| at or under this is "already synchronized"
#define KOSYNC_DOC_MAX         72         // a document id we will echo (ours are 32 hex)
#define KOSYNC_DEV_MAX         48         // a device name / device id from a peer
#define KOSYNC_USER_MAX        64
#define KOSYNC_KEY_CHARS       33         // MD5 hex + NUL
#define KOSYNC_HOST_MAX        64

// ---------------------------------------------------------------- numbers
/* A percentage as a JSON number: at most 6 decimals, trailing zeros and a bare point
 * stripped ("0", "1", "0.5", "0.231"), clamped to [0, 1]. Returns the length. */
size_t kosyncFormatPct(double p, char* out, size_t cap);

// ---------------------------------------------------------------- JSON
struct KosyncProgress {
  bool    hasPct;                    // false for `{}` — nobody has synced this document
  double  pct;
  bool    hasTimestamp;
  int64_t timestamp;                 // the SERVER's receive time, unix seconds
  char    document[KOSYNC_DOC_MAX];
  char    device[KOSYNC_DEV_MAX];
  char    deviceId[KOSYNC_DEV_MAX];
};

/* Read a progress object (a GET's answer, or a PUT's body). Any key order, any whitespace,
 * any JSON number (exponent form included), unknown keys and nested values skipped, string
 * escapes (\uXXXX too) decoded. False only when `body` is not a JSON object at all. */
bool kosyncParseProgress(const char* body, size_t len, KosyncProgress* out);

// JSON-escape `in` into `out` (no surrounding quotes). Returns the length.
size_t kosyncJsonEscape(const char* in, char* out, size_t cap);

// ---------------------------------------------------------------- HTTP
/* A header's value from a raw request/response header block (first line skipped, names
 * matched case-insensitively, surrounding whitespace trimmed). False when absent. */
bool kosyncHeader(const char* hdrs, const char* name, char* out, size_t cap);

/* x-auth-user and x-auth-key against the configured pair. The key is compared EXACTLY (it is
 * already lower-case hex on every client that exists) and in constant time. An empty
 * configured user or key authorises nobody. */
bool kosyncAuthOk(const char* hdrs, const char* user, const char* keyHex);

/* ── WHO MAY BE TOLD A PERCENTAGE (docs/booksync-simple-plan.md §5.1, 2026-09-26) ─────────
 * The window's GET reply carries `percentage` and `"progress":""` — the phone has no XPointer
 * to give. CrossPoint and the X4 fork fall back to the percentage; Readest ignores an empty
 * progress; KOReader does NOT: its kosync plugin takes a body WITH `percentage` and an empty
 * `progress` to syncToProgress("") = GotoXPointer("") — a crengine null pointer — and lands on
 * PAGE 1 (plugins/kosync.koplugin/main.lua:705-711), and a manual pull applies WITHOUT asking.
 * A body of `{}` gives it "No progress found" instead, and nothing at all on an automatic pull.
 * So the record goes only to a reader that SAYS it can take a percentage: the X4 fork sends
 * `X-BookSync: 1` (KOReaderSyncClient.cpp applyAuthHeaders, on every request), as does the
 * phone's own home client (KOSYNC_MARKER_LINE), and stock CrossPoint sends an
 * `Authorization: Basic` header BESIDE its x-auth-* pair (KOReader sends x-auth-* only). Either
 * header, with any value, is the marker; names match case-insensitively (kosyncHeader). Readest
 * sends Basic INSTEAD of x-auth-* and is 401'd by kosyncAuthOk before the marker is looked at:
 * on 0.9.79 it is served by nobody (the plan's §5.5 "any account" is where that changes). 🛑 Two
 * shortcuts are WRONG: "`{}` unless Basic" blanks the fork, which omits Basic on the peer and
 * hub networks; "a DocFragment pointer for everyone" sends the X4 to page 0 of the chapter (its
 * ProgressMapper maps a chapter-start XPointer there). Auth comes first: a 401 needs no marker. */
bool kosyncClientTakesPercentage(const char* hdrs);

// "HTTP/1.1 200 OK" -> 200; anything else -> -1.
int kosyncStatusCode(const char* statusLine);

/* Undo `Transfer-Encoding: chunked` in place (a home server is free to answer that way even
 * to a Connection: close request). Returns the decoded length, or -1 when it is not valid
 * chunked framing. Chunk extensions are ignored; trailers are dropped. */
long kosyncDechunk(char* buf, size_t len);

/* The request bytes the client sends, headers per contract §7.4 plus KOSYNC_MARKER_LINE (the
 * phone can take a percentage: without it a D-163 COVEY answers `{}` for the phone's own
 * place). `port` 80 is left off the Host header, as every client does. Return the length, or 0
 * if it did not fit. */
size_t kosyncBuildGet(char* out, size_t cap, const char* host, uint16_t port,
                      const char* user, const char* key, const char* doc);
size_t kosyncBuildPut(char* out, size_t cap, const char* host, uint16_t port,
                      const char* user, const char* key, const char* body);
size_t kosyncBuildPutBody(char* out, size_t cap, const char* doc, double pct,
                          const char* device, const char* deviceId);

// ---------------------------------------------------------------- the server
enum KosyncRoute {
  KOSYNC_ROUTE_NONE = 0,
  KOSYNC_ROUTE_HEALTH,
  KOSYNC_ROUTE_AUTH,
  KOSYNC_ROUTE_CREATE,
  KOSYNC_ROUTE_GET_PROGRESS,
  KOSYNC_ROUTE_PUT_PROGRESS,
};

// Does this path belong to KOSync at all? (/users/..., /syncs/..., /healthcheck)
bool kosyncIsPath(const char* path);

// Which route, and for GET_PROGRESS the document id from the path.
KosyncRoute kosyncRoute(const char* method, const char* path, char* doc, size_t cap);

/* The ONE book a window serves, as the phone sees it right now. Both ids answer: the partial
 * MD5 (KOReader's and CrossPoint's "binary" matcher) and the MD5 of the file name
 * (CrossPoint's default). */
struct KosyncServed {
  const char* partial;
  const char* byName;
  bool        pctOk;                 // false: this place has no KOSync percentage -> `{}`
  double      pct;                   // the phone's place, as a KOSync percentage
  const char* device;                // what the phone calls itself ("WiPhone-{user}")
  const char* deviceId;
  uint32_t    turnedAt;              // the phone's last real MOVE here, UTC, or 0 when unknown
};

struct KosyncServeOut {
  int    code;
  bool   pickedUp;                   // a GET or PUT of the window's book: start the grace
  bool   gotPut;                     // a PUT of the window's book arrived
  bool   park;                       // ...and is worth offering (another device, a real move)
  double putPct;
  char   putDevice[KOSYNC_DEV_MAX];
  char   putDeviceId[KOSYNC_DEV_MAX];
  bool   otherDoc;                   // a PUT for some other book: answered 200, discarded
  char   otherDocId[16];             // ...and the start of ITS id, for the screen ("" if none)
  /* A GET of the window's book from a reader that has not said it can take a percentage
   * (kosyncClientTakesPercentage): answered `{}`, so KOReader shows "No progress found" instead
   * of jumping to page 1. Still a pick-up (it may PUT next). One log line a window. */
  bool   unmarkedGet;
};

/* Answer one request. `reply` receives the JSON body. `nowUnix` is 0 when the phone has no
 * TRUSTED clock (none, or only a mesh-set one: kosync_sync.cpp nowUtc()), and is then what
 * `timestamp` says (the receivers treat 0 as "unknown").
 * 🛑 A PUT for another document is answered 200 and DISCARDED: the window is about one book,
 * and the X4 must not see an error for a book the phone simply was not asked about. */
void kosyncServe(const char* method, const char* path, const char* hdrs,
                 const char* body, size_t bodyLen,
                 const char* user, const char* key, const KosyncServed* book,
                 uint32_t nowUnix, KosyncServeOut* out, char* reply, size_t replyCap);

/* Is a peer's place worth a card? Not when it is our own record coming back (see
 * kosyncIsOwnRecord), and not when it is within KOSYNC_EPSILON of where we are. This is the
 * WINDOW's rule: every PUT into a window is a new PUT made on purpose, so it is offered again. */
bool kosyncWorthParking(double theirs, const char* theirDevice, const char* theirDeviceId,
                        double mine, const char* myDevice, const char* myDeviceId);
/* Our own record coming back. 🛑 BY device_id WHEN BOTH SIDES HAVE ONE — the name is only the
 * fallback for a record that carries no id. The name is whatever a person typed (device= in
 * kosync.txt, the Sync settings name), and two phones given one kosync.txt, or one name, are
 * still two devices: matching on the name made each silently ignore the other's place. Ours is
 * MAC-derived (kosyncDeviceId) and always sent, so our own echo is always caught by the id. */
bool kosyncIsOwnRecord(const char* theirDevice, const char* theirDeviceId,
                       const char* myDevice, const char* myDeviceId);

/* ── THE OFFER RULE (shared decision D1, the same on the X4 fork and COVEY) ──────────────
 * A SERVER's record (the home pull on book open, and the read that "Sync my place" makes
 * before it sends) goes to the card — never applied silently — when it is:
 *   from ANOTHER device (kosyncIsOwnRecord),
 *   more than KOSYNC_EPSILON away from where we are (skipped when we cannot place ourselves),
 *   not the very record the person already ANSWERED on the card for this book (kosyncOfferSig —
 *     a new PUT gets a new server timestamp and is offered again; a declined one stays
 *     declined), nor the very record parked on the card and still waiting for that answer,
 *   and NOT PROVABLY OLDER than our last real move. "Provably" needs BOTH clocks: our
 *     last-move stamp (the reader's place actually changing — NOT every save or close, which
 *     is what the CBS1 turnedAt is) and the server's timestamp (0 / missing = unknown; COVEY
 *     serves 0 while its clock is not NTP-synchronised). Either unknown -> offer.
 * The bias is deliberate: a spurious card costs one Back; a hidden position is the bug.
 * 🛑 "DECLINED" MEANS ANSWERED (KS-2). The record's signature used to be saved as "offered" the
 *    moment it was PARKED — before anyone had seen a card. A reboot, a power-off or an inbox
 *    eviction before the book was reopened then lost the card, and the saved signature hid the
 *    place ("already offered") until the X4 sent again; and a "Sync my place" pressed while the
 *    park was on its way sent the phone's older place over it on COVEY. The signature now waits
 *    in the park ledger and reaches the memo only when "Go there" or "Stay" is pressed on THAT
 *    card (kosyncOfferAnswered); a park that is lost unanswered is simply offered again. */
enum KosyncOfferVerdict {
  KOSYNC_OFFER = 0,
  KOSYNC_SKIP_OWN,                   // our own record coming back
  KOSYNC_SKIP_IN_STEP,               // within KOSYNC_EPSILON of where we are
  KOSYNC_SKIP_OFFERED,               // this very record was answered on the card already
  KOSYNC_SKIP_OLDER,                 // older than our last move, both clocks known
  KOSYNC_SKIP_WAITING,               // this very record is on the card, not yet answered
};
struct KosyncOfferIn {
  double      theirPct;
  int64_t     theirTs;               // the server's timestamp; <= 0 = unknown
  const char* theirDevice;
  const char* theirDeviceId;
  bool        mineOk;                // false: we cannot express where we are
  double      minePct;
  const char* myDevice;
  const char* myDeviceId;
  uint32_t    myMovedAt;             // UTC of our last real move in this book; 0 = unknown
  uint32_t    offeredSig;            // kosyncOfferSig of the record last ANSWERED; 0 = none
  uint32_t    pendingSig;            // ...of the record parked and unanswered; 0 = none
};
int         kosyncOfferVerdict(const KosyncOfferIn* in);
/* Of the records a read found (got[i] valid where have[i]; the partial-MD5 id first), the one
 * the offer rule judges: the NEWEST from another device — both ids live on one server, so
 * their timestamps compare; a tie or a missing timestamp keeps the earlier id. -1 when there
 * is none; `sawAny` says whether any record had a percentage at all. */
int         kosyncPickRecord(const KosyncProgress* got, const bool* have, int n,
                             const char* myDevice, const char* myDeviceId, bool* sawAny);
const char* kosyncOfferVerdictText(int verdict);   // "in step", "already offered", ...

/* Who, where, when — one server record, as a number that is never 0. The device_id when the
 * record has one, else its name; the percentage as it goes on the wire (6 decimals). */
uint32_t kosyncOfferSig(int64_t ts, double pct, const char* deviceId, const char* device);

struct KosyncMemoEntry;                      // (the per-book memo, below)
/* ── WHICH LAST MOVE JUDGES A HOME RECORD (KS-1) ──────────────────────────────────────────
 * The offer rule's inputs for the record `g` a home read found. `askedMovedAt` is the last-move
 * stamp AS IT STOOD WHEN THE ASK WAS MADE (the job's snapshot of the book); `me` is the book's
 * memo entry (NULL = none), whose movedAt is the FRESHEST stamp; `pendingSig` the park ledger's
 * unanswered record for this book (kosyncParkPending; 0 = none).
 * 🛑 AN AUTOMATIC ASK IS JUDGED AGAINST THE STAMP AS OF THE ASK. The pull on book open (and the
 *    same pull once the WiFi comes up) is not instant — two connections, up to 2 s of mDNS on a
 *    join's first job, a first connect that times out (3 s) plus the 1 s retry on a station in
 *    modem sleep — and the reader turns pages meanwhile. Judged against the freshest stamp, ONE
 *    turn in those seconds made the X4's newer place "provably older": no card (only the line
 *    "older than your last page turn, not offered"), every later open the same, and the next
 *    "Sync my place" then sent the phone's older place over the X4's on COVEY — and "read on
 *    the X4, then open the book on the phone" is THE flow this feature exists for.
 *    Only the explicit "Sync my place" (`explicitSync`) is judged against the freshest stamp:
 *    the person has just said "this is where I am". */
void kosyncOfferFill(KosyncOfferIn* in, const KosyncProgress* g, bool explicitSync, bool mineOk,
                     double minePct, uint32_t askedMovedAt, const KosyncMemoEntry* me,
                     uint32_t pendingSig, const char* myDevice, const char* myDeviceId);

/* ── THE AUTOMATIC PUSH RULE (shared decision D2) ──────────────────────────────────────
 * A push on close / on leaving the book goes out ONLY when the place moved since the book was
 * opened or since the last successful push of it (`ref`). An open and a close with no reading
 * in between used to PUT the unmoved place over a newer one another device had sent — the
 * server keeps the last PUT, so the X4's place was simply gone. With no reference percentage
 * (the place at open could not be expressed), `movedSinceRef` decides. The explicit "Sync my
 * place" always pushes (after reading the server first — see kosync_sync.h).
 * This is the SESSION half; kosyncClosePushWanted below is the whole rule. */
bool kosyncAutoPushWanted(bool refOk, double refPct, bool movedSinceRef, bool pctOk, double pct);

/* ── WHAT THE PHONE REMEMBERS PER BOOK FOR KOSync ─────────────────────────────────────────
 * 🛑 NOT positions.cbs. The CBS1 turnedAt is stamped on every SAVE (a close, the OK-to-menu
 * flush) and goes on the air; its meaning is LoRa's and stays as it was. This is a separate,
 * small table: the last real MOVE of the place and the server record last ANSWERED on the card
 * (both persisted, in NVS, so a reboot does not re-offer a declined place), what home last had from
 * us (persisted — see kosyncClosePushWanted), plus the session's push reference (never
 * persisted). Keyed by the file-name id; least recently used goes. */
#define KOSYNC_MEMO_MAX 16
struct KosyncMemoEntry {
  char     book[KOSYNC_KEY_CHARS];   // the file-name id (32 hex); "" = a free slot
  uint32_t movedAt;                  // UTC of the last real move, 0 = unknown   (persisted)
  uint32_t offeredSig;               // kosyncOfferSig last ANSWERED, 0 = none  (persisted)
  uint32_t used;                     // recency                                  (persisted)
  bool     sentOk;                   // a PUT of ours for this book succeeded     (persisted)
  double   sentPct;                  // ...and this was its place (6 dp kept)     (persisted)
  bool     unsent;                   // a real move home has not had yet          (persisted)
  bool     refOk;                    // this session: the push reference is known
  bool     movedSinceRef;            // this session: the place moved since `refPct`
  double   refPct;                   // this session: the place at open / at the last push
};
struct KosyncMemo {
  KosyncMemoEntry e[KOSYNC_MEMO_MAX];
  uint32_t tick;
  bool     dirty;                    // a persisted field changed since the last save
};
// The entry for `book`, or NULL. `create`: take a free slot, or the least recently used one.
KosyncMemoEntry* kosyncMemoGet(KosyncMemo* m, const char* book, bool create);
/* The three things that change an entry, kept here so the whole D2 story runs on a Mac:
 *   Opened  the session's reference is the place as the book opens;
 *   Moved   a real move (never a save or a close): the session's flag, the persisted `unsent`,
 *           and the last-move stamp when the clock is known (`nowUtc` 0 = unknown, kept as is);
 *   Sent    a PUT of `pct` succeeded: it is both the session's reference and what home has. */
struct KosyncParkLedger;                     // (the park ledger, below)
/* A book's FILE was renamed (the console's `mv`): the memo is keyed by the md5 of the file name,
 * so without this the book would come back with an empty entry. The last move, the answered offer
 * and `unsent` would be lost: an older home place could be offered again, and a move home never
 * had would not be pushed at the next close. The entry, and any parked offer in `l` (may be NULL),
 * move from `oldBook` to `newBook`. An entry already under `newBook` is dropped: the renamed file's
 * history is the one that describes these bytes now. True when anything moved (the memo is dirty). */
bool kosyncRekeyBook(KosyncMemo* m, KosyncParkLedger* l, const char* oldBook, const char* newBook);
void kosyncMemoOpened(KosyncMemoEntry* e, bool pctOk, double pct);
void kosyncMemoMoved(KosyncMemo* m, KosyncMemoEntry* e, uint32_t nowUtc);
void kosyncMemoSent(KosyncMemo* m, KosyncMemoEntry* e, double pct);
/* ── THE WHOLE CLOSE RULE (D2) ───────────────────────────────────────────────────────────
 * Push when the place moved THIS SESSION (kosyncAutoPushWanted), OR home has not had it from
 * us: it differs from the last place a PUT of ours delivered (`sentPct`, persisted), or — no
 * PUT of ours ever succeeded — a real move is recorded as `unsent`.
 * 🛑 THE SESSION HALF ALONE LOST PLACES. Read 0.40 -> 0.60 off WiFi (the close there cannot
 * push), then open and close the book at home without turning a page: "not moved since this
 * open", nothing sent, and home kept 0.40 for good — as it did after a push that gave up. The
 * persisted half still keeps F3 fixed: a phone whose place home already has from it sends
 * nothing, however often the book is opened. A move away and back to what home has also sends
 * nothing: home has that place from us already. NULL (no memo): push whatever is expressible. */
bool kosyncClosePushWanted(const KosyncMemoEntry* e, bool pctOk, double pct);
// The persisted fields as bytes (magic "KSM2", then 52 bytes an entry). Returns the length.
#define KOSYNC_MEMO_ENTRY_BYTES 52
#define KOSYNC_MEMO_BLOB_MAX (4 + KOSYNC_MEMO_MAX * KOSYNC_MEMO_ENTRY_BYTES)
size_t kosyncMemoPack(const KosyncMemo* m, uint8_t* out, size_t cap);
/* False (and an empty memo) for anything that is not exactly what kosyncMemoPack writes —
 * including the development build's "KSM1" (44-byte entries, never released). */
bool   kosyncMemoUnpack(KosyncMemo* m, const uint8_t* in, size_t len);

// ---------------------------------------------------------------- the home client's retries
/* One connect that did not answer is routine — the first contact after a quiet spell, on a
 * station in modem sleep, was seen failing on hardware and a manual retry then worked — so a
 * push or pull is NOT dropped on it. KOSYNC_CLIENT_TRIES attempts a job; this is the wait
 * after the Nth transport failure (1-based): 1 s, then 4 s, then 0 = give up. */
#define KOSYNC_CLIENT_TRIES 3
uint32_t kosyncRetryDelayMs(int failures);

// ---------------------------------------------------------------- home= by NAME
/* WHY A NAME: COVEY's address is the router's DHCP lease (no reservation, HANDOFF
 * 2026-09-24), so home=192.168.1.55:8088 goes stale — silently: "no answer (3 tries)" — the
 * day the lease moves. avahi on COVEY already publishes covey.local, and follows it onto any
 * network (192.168.89.1 on its own hotspot).
 *
 * home=covey.local (or the bare home=covey) is asked of the LAN by a ONE-SHOT multicast DNS
 * query (RFC 6762 §5.1) sent from an ordinary UDP port. Every responder there (avahi on COVEY,
 * a Mac) answers such a query by UNICAST to the asking port (§6.7), so the lookup is one polled
 * socket and the loop never waits on it. A dotted name that is not .local goes to lwIP's own
 * resolver the same way (kosync_sync.cpp), and an IP literal is used as it is.
 *
 * 🛑 NOT resolveDomain(). It BLOCKS the loop (mdnsResponder.queryHost waits 500 ms for a name
 *    nobody answers), and a .local name NEVER resolved through it: this core's mdns_query_a()
 *    takes the BARE host label and adds "local" itself (IDF 3.3's _mdns_search_init only
 *    strndup()s the name; libmdns.a imports no strchr/strstr/strtok, and its own console
 *    prints "Query A: %s.local") — so "covey.local" went on the air as the ONE label
 *    "covey.local" under .local, which no responder answers. lwIP (LWIP_DNS_SUPPORT_MDNS_QUERIES
 *    is 0 in this core) then asked the router, which has never heard of it. Every book open and
 *    close on WiFi blocked the loop (500 ms of mDNS, then a DNS round trip) and ended "Home:
 *    'covey.local' not found". (Read from the code and libmdns.a, 2026-09-25 — never seen on a
 *    phone: both used an IP.) */
#define KOSYNC_MDNS_PORT 5353
bool     kosyncParseIp(const char* host, uint32_t* ip);   // dotted quad -> wire-order bytes
bool     kosyncHostIsMdns(const char* host);              // one label, or ending in .local
/* The query for `host`'s A record ("covey" and "covey.local" both ask for covey.local): our
 * `id`, one question, QTYPE A, QCLASS IN, no QU bit (the port already says "answer me
 * unicast"). Returns the length, 0 when `host` is not an mDNS name or does not fit. */
size_t   kosyncMdnsQuery(const char* host, uint16_t id, uint8_t* out, size_t cap);
/* The IPv4 address an answer gives for `host`, as the 4 wire bytes memcpy'd into a uint32_t
 * (the representation IPAddress and sin_addr use); 0 when it gives none. Only a RESPONSE
 * carrying our `id` counts (our own query looped back is not one), and only an A record for
 * that very name — in any section, class IN with the cache-flush bit ignored, rdlength 4, TTL
 * not 0 (a goodbye) — with names compared case-insensitively and compression followed. Never
 * reads past `len`, whatever the packet says. */
uint32_t kosyncMdnsAnswer(const uint8_t* pkt, size_t len, const char* host, uint16_t id);

/* ── WHAT THE PHONE KNOWS OF home='s ADDRESS ───────────────────────────────────────────────
 * A NAME is looked up by a home job, in the background: 2 s of mDNS for .local or a bare name,
 * up to 6 s of lwIP's resolver for a DNS name (or a bare name mDNS did not know). The loop
 * never waits on either. An ANSWERED name — or a fallback (below) that then gets an HTTP
 * answer — is used as it is for the rest of that WiFi ASSOCIATION (`assoc` is any number that
 * changes with each GOT_IP, 0 = none yet; the phone uses lastWifiLinkUpMs(), the GOT_IP stamp),
 * until the SSID changes, home= changes, or a job gives up on the address.
 * ⚠ NOT "once per join" for a name nobody answers: nothing is recorded, so the NEXT job asks
 *   again. On a WiFi without COVEY (a friend's; COVEY off with no address kept for this SSID)
 *   that is one more background lookup — 3 multicasts, a socket held 2 s — per book open and
 *   close, and the `kosync` home line never says "this join". Never a wait.
 * A lookup nobody answers FALLS BACK to the address last found for the same home= on the same
 * SSID — kept across a restart — so one lost multicast, or avahi renaming COVEY to
 * covey-2.local after a name clash, does not strand a push the address would still deliver.
 * ⚠ ONE record (one NVS slot): the LAST home=, SSID and address that answered. An answer for
 *   another home=, or for the same name on another WiFi, replaces it — the old one has no
 *   fallback until it answers there again.
 * An IP literal is used as it is: nothing looked up, nothing remembered. */
struct KosyncHomeAddr {
  char     host[KOSYNC_HOST_MAX];            // the home= it is for ("" = nothing known)
  char     ssid[33];                         // the network it was found on
  uint32_t ip;                               // wire-order bytes; 0 = none
  uint32_t assoc;                            // the association it is good for; 0 = none yet
  bool     stale;                            // a job gave up on it: look again first
};
enum KosyncHomePlan { KOSYNC_HOME_USE = 0, KOSYNC_HOME_LOOKUP };
/* What a job does first: USE `*ip` (an IP literal, or a name already answered for this
 * home=, SSID and association), or LOOKUP. */
int      kosyncHomePlan(const KosyncHomeAddr* a, const char* host, const char* ssid,
                        uint32_t assoc, uint32_t* ip);
/* A lookup ended; `answer` 0 = nobody answered. Returns the address to use — the answer, or
 * the fallback (*fallback set) — or 0 for none. An answer is recorded, and *changed says the
 * persisted copy (kosyncHomePack) differs from what is now known: write it. */
uint32_t kosyncHomeLooked(KosyncHomeAddr* a, const char* host, const char* ssid, uint32_t assoc,
                          uint32_t answer, bool* fallback, bool* changed);
// `ip` gave an HTTP answer (any status): good for this association, no lookup until the next.
void     kosyncHomeReached(KosyncHomeAddr* a, const char* host, const char* ssid, uint32_t assoc,
                           uint32_t ip);
// A job gave up with no answer from `ip`: the next job looks the name up again first.
void     kosyncHomeGaveUp(KosyncHomeAddr* a, uint32_t ip);
// Persisted: "KSH1", home= (64), SSID (33), ip (4). assoc/stale are not: a restart looks again.
#define KOSYNC_HOME_BLOB_BYTES (4 + KOSYNC_HOST_MAX + 33 + 4)
size_t   kosyncHomePack(const KosyncHomeAddr* a, uint8_t* out, size_t cap);
bool     kosyncHomeUnpack(KosyncHomeAddr* a, const uint8_t* in, size_t len);

// ---------------------------------------------------------------- the window's transport
/* A window on the phone's WiFi address outlives that WiFi unless someone checks. Checked
 * once a second while a window is on the station, AND by a new ask for a window: OK; BLIP
 * (disconnected for less than `graceMs`, a roam or a blip — hold on); LOST (gone longer, or
 * the WiFi switched off / disabled by its owner, which is no blip: close it, so the next ask
 * can bring up the hotspot); MOVED (connected, but not at the address the window opened on —
 * the auto-switcher took the phone elsewhere). `lostSinceMs` is the caller's; 0 = connected.
 * 🛑 ONE RULE FOR BOTH CALLERS. The ask used to close a station window on ANY status but
 * connected, with no grace: a book opened during a blip at home tore the WiFi window down
 * and hosted 'WiPhone-Books' instead. */
#define KOSYNC_STA_GRACE_MS 5000UL
enum KosyncStaCheck { KOSYNC_STA_OK = 0, KOSYNC_STA_BLIP, KOSYNC_STA_LOST, KOSYNC_STA_MOVED };
int kosyncStationWindowCheck(bool switchedOff, bool connected, uint32_t ipNow, uint32_t ipOpened,
                             uint32_t* lostSinceMs, uint32_t now, uint32_t graceMs);

/* Should a WINDOW's transport wait (up to 2 s, blocking) for the station before hosting a
 * hotspot? Only when an association is genuinely in flight: the radio on and not disabled by
 * the user, the station running, and EITHER a join started within the last 10 s OR the station
 * was UP within the last 10 s (`lastUpMs`: a blip or a roam, whose rejoin is the core's own
 * WiFi.begin() that no join path stamps — without this, a book closed at home during a
 * sub-second blip put the phone on its hotspot for five minutes). A long out-of-range spell
 * has neither, and does not wait. The uploader's own wait is unchanged (a person started it
 * and is watching); a window opens by itself on every book close/open. 0 = never, for both. */
bool kosyncWindowWaitsForSta(bool radioOff, bool userDisabled, bool staMode, uint32_t now,
                             uint32_t lastJoinMs, uint32_t lastUpMs);

/* ── "SYNC MY PLACE": HOME FIRST, THE WINDOW AFTER (docs/booksync-simple-plan.md §5.2) ─────
 * On WiFi with a home= configured, and the home job actually STARTED (kosyncSyncHome took the
 * ask), the window is not opened on the press: it opens from the loop when that job has its
 * verdict — pushed, offered a card, gave up — or after KOSYNC_WINDOW_AFTER_HOME_MAX_MS
 * (kosync_sync.cpp), whichever is first. Off WiFi, with no home, or when the job could not be
 * queued (its note says why), the window opens at once, as it always did. 🛑 WHY: the window
 * used to be opened FIRST and the push queued behind it; a window on the phone's own WiFi
 * address (0.9.79) leaves the station alone, but the coming hotspot-always rule takes the
 * station down under the push, which then dies "not on WiFi any more". Home is asked first
 * BECAUSE the phone is on WiFi now. */
bool kosyncWindowWaitsForHome(bool onWifi, bool homeConfigured, bool homeStarted);

/* What went wrong in a window, for the SCREEN (the X4 says "Upload complete" either way):
 * a PUT for a different document (not the same file on both? `docId` is the start of its
 * id), requests with the wrong user/password, and GETs for the book from a reader that cannot
 * take a percentage (`unmarked`: it was told "No progress found" — KOReader, see
 * kosyncClientTakesPercentage). "" when none of them happened. */
size_t kosyncWindowProblems(uint32_t otherBook, const char* docId, uint32_t unauth,
                            uint32_t unmarked, char* out, size_t cap);

/* ── A READER'S SECOND ID IS NOT ANOTHER BOOK (KS-3) ──────────────────────────────────────
 * The CrossPoint fork with booksync PUTs every upload under BOTH its ids when they differ: the
 * partial MD5 and the MD5 of ITS OWN file name. The same bytes under another name ('Title -
 * Author.epub' on the X4, 'Title_Author.epub' here) match on the partial MD5 — that PUT parks and
 * the card goes up — and not on the file name, and that second PUT put "! A place for a
 * DIFFERENT book arrived" right beside the card from the same exchange: a sync that worked,
 * reported as one that failed, which cheapens the one warning that matters when the files
 * really differ.
 * So a window keeps WHO sent what. A PUT for another document is a DIFFERENT book only when the
 * same device (its device_id; its name when it sends no id) did not ALSO PUT this window's own
 * book in this window — before it or after it: the count is worked out when the line is SHOWN,
 * and the line outlives the window. A PUT that names no device at all is always counted, and so
 * is one the fixed-size log had no room to tell apart: a warning is never hidden for want of
 * room. The rest are counted too (serial `kosync`: "second-id"), never shown as a problem. */
#define KOSYNC_PUTLOG_MAX 6
struct KosyncPutLog {
  uint32_t own[KOSYNC_PUTLOG_MAX];           // who PUT the window's own book (kosyncPeerKey), once each
  uint8_t  nOwn;
  uint32_t by[KOSYNC_PUTLOG_MAX];            // who PUT another document...
  char     doc[KOSYNC_PUTLOG_MAX][16];       // ...which one (the start of its id, as shown)...
  uint32_t n[KOSYNC_PUTLOG_MAX];             // ...and how many times
  uint8_t  nOther;
  uint32_t unlogged;                         // PUTs for another document with no room left
};
// Who a PUT came from, as a number: its device_id, else its name. 0 = it names neither.
uint32_t kosyncPeerKey(const char* device, const char* deviceId);
void     kosyncPutLogOwn(KosyncPutLog* l, const char* device, const char* deviceId);
void     kosyncPutLogOther(KosyncPutLog* l, const char* device, const char* deviceId, const char* docId);
/* How many of the PUTs for another document were a DIFFERENT book (above), and in `*doc` the
 * start of the latest such id ("" when none is known). `secondIds` (may be NULL): the rest — a
 * reader's other id for this window's own book. */
uint32_t kosyncPutLogDifferent(const KosyncPutLog* l, const char** doc, uint32_t* secondIds);

/* ── THE WINDOW'S WARNINGS, AND WHAT CLEARS THEM ──────────────────────────────────────────
 * Kept after the window closes — a person looks at Sync settings AFTER the X4 has said
 * "Upload complete" — and cleared by exactly two things: a NEW window (not the open one being
 * extended for the same book), and a re-read of kosync.txt that was ASKED FOR (serial
 * `kosync reload`) or that finds the file now SAYS something else (kosyncConfigSig).
 * 🛑 NOT BY EVERY RE-READ. Books re-reads the file on every entry and Sync settings on every
 *    visit (app_books.cpp), so clearing there would wipe the warning on the way to the one
 *    screen that shows it. Before this, nothing but a new window cleared them: the last
 *    night's bench warnings survived `kosync reload`. */
struct KosyncConfig;                         // (the config section, below)
/* What a config says, as a number: every field kosyncParseConfig fills. Comments, blank lines
 * and spacing do not change it; the password (as its MD5), home=, device= and the switches do. */
uint32_t kosyncConfigSig(const KosyncConfig* c);
// Does this re-read clear the warnings? Asked for, or the file changed since the last read.
bool   kosyncReloadClears(bool asked, bool readBefore, uint32_t sigBefore, uint32_t sigNow);

// ---------------------------------------------------------------- the Books library
/* Settings files that live beside the books and are NOT books: /books/kosync.txt (it holds
 * the password), smsmirror.txt (at / or /roms) and gemini.txt (Menu > AI's key; the T-Deck
 * firmware keeps it at the card's root). The library scans all three folders for .txt, so
 * without this they were listed — and opening one showed its secrets on screen. */
bool kosyncNotABook(const char* basename);

// ---------------------------------------------------------------- the window's clock
/* A hard millis() deadline plus the picked-up grace. Pure so the wrap and the ordering can
 * be proven on a Mac: `now` is whatever millis() said. */
#define KOSYNC_WINDOW_SYNC_MS   (300UL * 1000UL)   // "Sync my place" / close
#define KOSYNC_WINDOW_OPEN_MS   (60UL * 1000UL)    // opening a book while off WiFi
/* 🛑 TWO GRACES, and the GET's is the LONG one. A stock CrossPoint GETs the place, then loads
 * and maps the book, and in ASK mode (or for a record it has never seen) waits for a HUMAN to
 * choose before it PUTs anything. A 10 s grace from the GET closed the hotspot under that
 * reader while it was still deciding — re-arming on the next request cannot help when the
 * next request is exactly what the close prevents. Only a PUT (the peer has said its piece)
 * earns the short close. */
#define KOSYNC_GRACE_GET_MS     (120UL * 1000UL)   // after a GET for the book
#define KOSYNC_GRACE_PUT_MS     (10UL * 1000UL)    // after a PUT for the book

struct KosyncWindowClock {
  bool     open;
  uint32_t openedMs;
  uint32_t durationMs;
  bool     picked;
  uint32_t pickedMs;                 // the LAST request for the book
  uint32_t graceMs;                  // ...and the grace IT armed (GET long, PUT short)
};

enum KosyncClockState {
  KOSYNC_CLOCK_RUNNING = 0,
  KOSYNC_CLOCK_DEADLINE,             // the hard deadline passed
  KOSYNC_CLOCK_PICKED,               // picked up, and the grace is over
  KOSYNC_CLOCK_CLOSED,               // was never open
};

void     kosyncClockOpen(KosyncWindowClock* c, uint32_t now, uint32_t durationMs);
/* The LAST request for the book decides: a GET arms KOSYNC_GRACE_GET_MS, a PUT
 * KOSYNC_GRACE_PUT_MS, each counted from that request and never past the hard deadline. */
void     kosyncClockPicked(KosyncWindowClock* c, uint32_t now, uint32_t graceMs);
int      kosyncClockDue(const KosyncWindowClock* c, uint32_t now);
uint32_t kosyncClockRemainingMs(const KosyncWindowClock* c, uint32_t now);

// ---------------------------------------------------------------- /books/kosync.txt
/* key=value lines, '#' comments on their OWN lines:
 *   user=<name>            the ONE KOSync account shared by all your devices
 *   password=<secret>      stored only as its MD5 (what goes on the wire); or key=<32 hex>
 *   home=<host or IP>[:port]   the server at home (COVEY: port 8088); http only, no https.
 *                          A name (covey.local, or just covey) is looked up on the WiFi once
 *                          per join, without stopping the phone — see kosyncHomePlan
 *   device=<name>          what this phone calls itself (default "WiPhone-<device name>")
 *   auto=on|off            open a window (and push home) when a book is closed
 *   open_window=on|off     open a 60 s window when a book is opened while off WiFi
 *   hotspot_pass=<8-63 printable ASCII>   the window's own hotspot is WPA2 with this
 *                          password; absent (or invalid — and then SAID so) = open
 * A missing file means the feature is OFF and LoRa booksync is exactly as before.
 *
 * ⚠ A TRAILING " #..." (whitespace, then '#') is a comment on user/home/device/auto/
 * open_window and is cut off. On password/key/hotspot_pass it is REFUSED with the reason
 * shown, never guessed at: "#" is a legal password character, and a comment folded into the
 * MD5 meant a 401 on every exchange with nothing saying why. */
struct KosyncConfig {
  bool     ok;                       // user and a password/key: the feature is on
  char     user[KOSYNC_USER_MAX];
  char     key[KOSYNC_KEY_CHARS];    // MD5(password) — the plaintext is never kept
  bool     passwordShort;            // under 12 characters: shown as a warning
  char     home[KOSYNC_HOST_MAX];    // "" = no client
  uint16_t homePort;
  char     device[40];               // "" = let the caller default it
  bool     autoOnClose;
  bool     openWindow;
  char     problem[64];              // why it is not ok, or a warning; "" when all is well
  /* 🛑 NEVER PRINTED. The one secret here that must be kept in the clear (it is what
   * softAP() takes); shown only as "hotspot: WPA2". "" = the hotspot is open. */
  char     hotspotPass[64];
  char     hotspotNote[48];          // why a hotspot_pass line was ignored; "" otherwise
};

/* A WPA2 passphrase the ESP32's softAP will take: 8-63 characters, printable ASCII (0x20-0x7E).
 * On failure `why` says which rule, in words fit for the Sync settings screen. */
bool kosyncHotspotPassValid(const char* v, const char** why);

bool kosyncParseConfig(const char* text, size_t len, KosyncConfig* out);

// A stable per-device id from the MAC: MD5("WiPhone-" + 12 hex digits), lower-case hex.
void kosyncDeviceId(const uint8_t mac[6], char out[KOSYNC_KEY_CHARS]);

// ---------------------------------------------------------------- into the booksync inbox
/* A peer's place, already converted to (reading chapter, how far through it), as the
 * reader's own SPINE-EQUAL fraction — the number the sync card, applyPending() and
 * epubLocate() all work in. Nothing downstream of the inbox knows KOSync exists.
 *
 * ⚠ Nudged HALF A QUANTUM FORWARD (0.5/65535 of the book). The record is rounded to 6 dp and
 * then quantised UP to 1/65535ths on its way through the CBS1 packer (booksync.cpp), and a
 * chapter START (within 0 — every "fell in the cover, go to the next chapter" case) that the
 * 6 dp rounding nudged below the boundary would locate to the END OF THE PREVIOUS CHAPTER.
 * Half a quantum forward is a few characters at most and keeps every landing at or after the
 * true point; tests/test_kosync.cpp runs every inverse vector through the whole chain. */
double kosyncToReaderFraction(int readIdx, double readWithin, int nRead);

/* The CBS1 text that parks a peer's place in the booksync inbox so the EXISTING card raises
 * ("CrossPoint says: they are at 61%"), with its undo, arming and backward warning. Signed
 * with the LOCAL booksync key (bookSyncDeriveKey of this phone's passcode) — it never goes on
 * the air; it only has to verify here. `device` is the peer's name, as the card shows it.
 * `ids` are the book's booksync ids (epubIds), which is what the inbox matches on. */
bool kosyncParkText(const char* const* ids, int nIds, int readIdx, double readWithin, int nRead,
                    uint32_t turnedAt, const char* device, const uint8_t key[32],
                    char* out, size_t cap);

/* 🛑 ONE KOSync OFFER PER BOOK IN THE INBOX. The inbox holds FOUR records for every book and
 * both transports, and each parked KOSync record carries a fresh random nonce — so a peer
 * PUTting on every page turn, or a reader opening the same book five times, would push
 * OTHER books' LoRa positions out the far end. The ledger remembers which inbox record each
 * KOSync park became, and a new park for the same book REPLACES it. It also keeps the peer's
 * own percentage, so the card can show the number the X4 shows. */
#define KOSYNC_LEDGER_MAX 4
struct KosyncParkLedger {
  char     book[KOSYNC_LEDGER_MAX][KOSYNC_KEY_CHARS];   // the file-name id of the book
  uint32_t id[KOSYNC_LEDGER_MAX];                       // its inbox record (0 = none)
  double   peerPct[KOSYNC_LEDGER_MAX];                  // the peer's own KOSync percentage
  /* The SERVER record the park offers (kosyncOfferSig), waiting for the person's answer; 0 = a
   * window PUT (offered again on every PUT anyway), or answered already. RAM only, on purpose:
   * a park lost to a restart is simply offered again (KS-2, above). */
  uint32_t sig[KOSYNC_LEDGER_MAX];
  int      next;                                        // round-robin slot for a new book
};

/* Park `text` for `book`: remove this ledger's previous record for the book, push, remember
 * the new one and the server record it offers (`sig`, 0 for a window PUT). True when the inbox
 * took it. Plain C++ over booksync_inbox — host-tested. */
bool kosyncParkInto(KosyncParkLedger* l, const char* book, const char* text, double peerPct,
                    uint32_t rxUnix, uint32_t sig);

// The peer's own percentage for the inbox record `inboxId`, if a KOSync park made it.
bool kosyncParkedPeerPct(const KosyncParkLedger* l, uint32_t inboxId, double* pct);

/* Is a KOSync park for `book` still in the inbox — on the card, or waiting for the book to be
 * opened — and so still UNANSWERED? `*sig` (may be NULL) gets the server record it offers (0: a
 * window PUT). An answered card retires every parked record for its book, and a restart or an
 * eviction loses it, so "still in the inbox" is exactly "nobody has answered it yet".
 * With `key` (the local booksync key; NULL = do not check), the parked record must also still
 * VERIFY under it: a park signed before the booksync passcode was changed can never become a
 * card (the reader verifies under the new key), so nobody is going to answer it.
 * 🛑 "Sync my place" sends NOTHING home while this is true (kosync_sync.cpp evaluateOffer), and
 *    neither does any other push (the close's, `kosync push`): its first PUT is refused
 *    (parkAwaitsAnswer) — a place the person has not yet seen is not ours to send over. ⚠ Hence
 *    the key: without it a park the card can never show would hold every push until a restart. */
bool kosyncParkPending(const KosyncParkLedger* l, const char* book, const uint8_t* key,
                       uint32_t* sig);

/* The person ANSWERED the card for inbox record `inboxId` ("Go there" or "Stay where I am"). If
 * a home read parked it, its server record is now the book's `offeredSig` in the memo (marked
 * dirty: save it) and is not offered again; the ledger forgets the pending signature. True when
 * the memo changed. A LoRa card, or a window PUT's: nothing to remember, false. */
bool kosyncOfferAnswered(KosyncParkLedger* l, KosyncMemo* m, uint32_t inboxId);

#endif // KOSYNC_H
