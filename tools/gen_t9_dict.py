#!/usr/bin/env python3
"""Generate WiPhone/src/assets/t9_dict.h — the flash-resident T9 word table.

WHY THIS SHAPE
--------------
The phone has ~15-20 KB of free INTERNAL heap in steady state, and an allocation failure
inside `new` throws with nothing catching it (see the note on MenuOption in GUI.h). So the
dictionary must cost ZERO heap. It does: this emits a `static const` table, PROGMEM is a
no-op on ESP32, and .rodata is memory-mapped, so every read is a plain pointer dereference
straight out of flash with no copy. The 3.2 MB DROM window is the ceiling, not the ~20 KB heap.

WHY THERE ARE NO FREQUENCIES IN THE OUTPUT
------------------------------------------
Rank is the STORAGE ORDER, not a stored field. Words are sorted by (digit key, then descending
frequency), so "most likely candidate first" and "press DOWN for the next one" both fall out of
an index into a contiguous run. That costs zero bytes — and it also means the shipped artefact
is nothing but an ordered list of ordinary English words: no counts, no n-grams, no contiguous
source text. That matters for provenance as much as for size.

WHY SORTED BY DIGIT KEY
-----------------------
The digit key is RECOMPUTABLE from the word (a/b/c -> 2 ... w/x/y/z -> 9), so it is never
stored either. Sorting by the key STRING makes both lookups a contiguous run:
  exact match   ("228" -> cat, bat, act...)  = the run where key == the typed digits
  completions   ("228" -> cats, cattle...)   = the run where key startswith the typed digits
One ordering, one binary search, both behaviours.

APOSTROPHES
-----------
"don't" is keyed as if it were "dont" — the apostrophe is skipped when computing the key but
kept in the word that gets inserted. That is what the old phones did: you type d-o-n-t and get
"don't" without reaching for the symbols row.

SHORT WORDS ARE FILTERED BY RANK, NOT BY A DICTIONARY
-----------------------------------------------------
🛑 --short-len IS 2, NOT 3, AND THAT MATTERS. At 3 this filter removed 990 of the 1,115
three-letter words and left 125: "cat", "bat", "cop", "bus", "cup", "art", "cry", "sat",
"mat", "tip", "gym", "pop" and "pub" were all UNTYPABLE, and 401 of the 512 three-digit keys
matched nothing at all. The worked example three paragraphs down — 228 -> cat, bat, act —
was describing a table that did not contain cat or bat. The reasoning below is sound for
TWO letters and was wrongly extended to three; a three-letter English word is common enough
that rank cannot separate it from an initialism, and the initialisms that get in (bff, omg,
fbi, dna) are mostly things people type anyway.

A subtitle corpus\'s two-letter tokens are mostly initialisms and letter-spellings
— tv, dr, pm, cd, uk, hq, iq, em, ll, ls — and they are poison precisely because they land on
the SHORT keys, which are the ones people hit most. But a genuine short English word is by
definition frequent, so ranking separates them for free: at a cutoff of 1,000 the two-letter
survivors are exactly the real ones, and at 2,000 the initialisms and first names return.
Hence --short-max-rank. It needs no curated list and it works the same way for any language.

🛑 DO NOT PASS /usr/share/dict/web2 TO --allow. It was tried and it is the wrong KIND of list.
web2 is Webster\'s Second — HEADWORDS only — so it rejects every plural and inflection
("things", "years", "guys", "minutes", "women") and, remarkably, does not contain "has" at
all, while it DOES contain "ba", "ca" and "aa". Measured: it dropped 459 of the 3,000 most
frequent English words. --allow is for an INFLECTED word list (SCOWL and friends); given a
headword dictionary it removes the good words and keeps the junk.

USAGE
-----
    python3 tools/gen_t9_dict.py --freq tools/t9-corpus/en_50k.txt --limit 25000 --preview
    python3 tools/gen_t9_dict.py --freq tools/t9-corpus/en_50k.txt --limit 25000 \\
        --out WiPhone/src/assets/t9_dict.h \\
        --freq-name "OpenSubtitles 2018 en_50k (hermitdave/FrequencyWords)" \\
        --freq-url  "https://github.com/hermitdave/FrequencyWords" \\
        --freq-licence "MIT"

--freq  "word count" or "word" per line, most frequent first (count optional; file order is
        the tiebreak and the fallback ranking).
--allow optional INFLECTED spelling whitelist; one word per line. See the warning above.
--out   omit it (with --preview) to inspect what would ship without writing anything.
"""
import argparse
import hashlib
import os
import re
import sys

# a/b/c -> 2 ... w/x/y/z -> 9. The one mapping every keypad in the world agrees on.
_KEYPAD = {}
for _digit, _letters in (
    ("2", "abc"), ("3", "def"), ("4", "ghi"), ("5", "jkl"),
    ("6", "mno"), ("7", "pqrs"), ("8", "tuv"), ("9", "wxyz"),
):
    for _ch in _letters:
        _KEYPAD[_ch] = _digit

# Letters, and the apostrophe forms people actually type. U+2019 is normalised to ASCII "'"
# because nothing in this firmware renders a non-ASCII glyph — see the meshStarName note in
# app_meshtastic.cpp; a smart quote would come out as a hollow box.
_WORD_RE = re.compile(r"^[a-z]+(?:'[a-z]+)*$")
_RUN3_RE = re.compile(r"(.)\1\1")

# ── CONTRACTIONS ──────────────────────────────────────────────────────────────────────────
# Subtitle corpora are tokenised on the apostrophe, so "didn't" arrives as "didn" + "'t".
# The fragment is not a word and the real word is missing entirely — and these are among the
# most frequent things anyone types. Left alone, a 25,000-word dictionary cannot spell
# "didn't", while offering "didn" as a candidate.
#
# Unambiguous stems are REPAIRED in place, keeping the frequency rank the fragment earned.
_REPAIR = {
    "didn": "didn't",    "doesn": "doesn't",   "isn": "isn't",       "wasn": "wasn't",
    "wouldn": "wouldn't", "couldn": "couldn't", "shouldn": "shouldn't", "aren": "aren't",
    "weren": "weren't",  "hasn": "hasn't",     "haven": "haven't",   "hadn": "hadn't",
    "mustn": "mustn't",  "needn": "needn't",   "ain": "ain't",       "daren": "daren't",
}

# These stems are REAL WORDS in their own right ("don", "can", "won", "let", "shan"), so the
# base is kept and the contraction is added alongside it. They never compete: a contraction
# has a longer digit key than its stem, so they land in different runs.
_ALSO = {
    "can": "can't", "don": "don't", "won": "won't", "let": "let's", "shan": "shan't",
}

# ── WHAT A PHONE DICTIONARY SHOULD NOT OFFER ───────────────────────────────────────────────
# Two kinds, and the list is short on purpose — a big blocklist is a big opinion.
#
# 1. Subtitle plumbing. "subtitles", "sync", "corrected", "translated", "ripped" and friends
#    are frequent in this corpus for a reason that has nothing to do with English: they are
#    in the credit lines of the files themselves.
# 2. Slurs. They sat at ranks 4,600-6,500 and would have been offered as predictions. Old T9
#    phones excluded this vocabulary deliberately and were right to; a word nobody wants
#    suggested is worse than a word missing. Ordinary profanity is NOT here — "damn", "hell",
#    "crap" and the rest stay, because people type them and the phone is not a chaperone.
#
# ⚠ A HEURISTIC CANNOT DO THIS JOB. "drop tokens whose capitalised form outnumbers their
# lowercase form" was considered and is unsafe on subtitle data: it deletes "i" outright and
# endangers every commonly sentence-initial word. Curation is the only honest option.
_BLOCK = {
    "subtitles", "subtitle", "subs", "sync", "synced", "synchronized", "synchronised",
    "corrected", "translated", "translation", "captioning", "captions", "ripped",
    "encoded", "resync", "uploaded", "downloaded",
    "nigger", "niggers", "nigga", "niggas", "faggot", "faggots", "fag", "fags",
    "cunt", "cunts", "retard", "retarded", "spic", "chink", "kike", "tranny",
}

# ── WORDS THE CORPUS IS TOO OLD TO HAVE ────────────────────────────────────────────────────
# OpenSubtitles 2018 systematically lacks the vocabulary of using a phone, because film
# dialogue rarely contains it. Measured against the shipped table: wifi, bluetooth, usb,
# username, login, browser, screenshot, emoji and smartphone were all absent, and those are
# words somebody texts on a Tuesday.
#
# Curated, short, and deliberately GENERAL. Project vocabulary — meshtastic, lora, repeater —
# is NOT here: that is exactly what the SD extra dictionary is for, and baking one project's
# jargon into everybody's firmware is the thing that design exists to avoid.
#
# Injected at a middling rank so they are plausible candidates without displacing core words.
_MODERN_RANK = 5000
_MODERN = [
    "wifi", "bluetooth", "usb", "username", "login", "logout", "browser", "screenshot",
    "emoji", "smartphone", "hotspot", "charging", "unlock", "reboot", "offline",
]

# The 's / 're / 've / 'll / 'd forms, which tokenisation splits off entirely so they cannot
# be recovered from the corpus at all. Curated rather than derived: this is the closed set of
# things people actually type, and every one of them was on the old phones.
#
# ⚠ ORDERED BY THEIR OWN FREQUENCY AND GIVEN THEIR OWN RANK BAND, not their base word's.
# Inserting each at `base + 1` put the whole i-family at ranks 6-9 — ahead of "the" at 10 —
# so 4-3 offered "I'd" before "he", and 4-6 offered "I'm" before "in". Inserting in list
# order also REVERSED each family, because every later insert pushed the earlier ones back.
# A fixed band below the core vocabulary fixes both: common enough to find, never ahead of
# the hundred words that matter most.
_CONTRACTION_RANK = 150
_COMMON_CONTRACTIONS = [
    "it's", "that's", "i'm", "don't", "you're", "there's", "what's", "he's", "she's",
    "we're", "they're", "i'll", "i've", "let's", "here's", "who's", "where's", "how's",
    "you'll", "you've", "we'll", "we've", "they'll", "they've", "it'll",
    "i'd", "you'd", "he'd", "she'd", "we'd", "they'd", "o'clock", "y'all",
]


def digit_key(word):
    """The keypad digits for a word. Apostrophes are skipped, not encoded."""
    return "".join(_KEYPAD[c] for c in word if c != "'")


def normalise(raw):
    """Lowercase, normalise the apostrophe, and reject anything that is not a plain word.

    Returns None for entries a keypad cannot produce: digits, hyphens, accents, single
    letters that are not real words, and the URL/markup debris that subtitle corpora carry.
    """
    w = raw.strip().lower().replace("’", "'")
    if not w or not _WORD_RE.match(w):
        return None
    # No English word has three identical letters in a row. Subtitles are full of stretched
    # interjections ("aaaah", "ohhh", "hmmmm") and they arrive frequent enough to rank.
    if _RUN3_RE.search(w):
        return None
    if w in _BLOCK:
        return None
    # "a" and "i" are real; no other single letter is worth a dictionary slot, and they
    # would otherwise outrank real words on any frequency list.
    if len(w) == 1 and w not in ("a", "i"):
        return None
    return w


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_freq(path):
    """Ordered list of words, most frequent first, deduplicated. File order is authoritative.

    Accepts "word count" or bare "word". The count is read only to re-sort within the file if
    it is present and monotonically wrong; otherwise file order wins, because that is what
    every published frequency list already guarantees.
    """
    out, seen = [], set()

    def emit(word):
        if word and word not in seen:
            seen.add(word)
            out.append(word)

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            w = normalise(parts[0])
            if w is None:
                continue
            if w in _REPAIR:
                emit(_REPAIR[w])          # "didn" was never a word; "didn't" is
                continue
            emit(w)
            if w in _ALSO:
                emit(_ALSO[w])            # keep "can", add "can't" just behind it

    # Vocabulary the corpus is too old to RANK, promoted to a fixed middling position.
    #
    # ⚠ PROMOTED, NOT MERELY ADDED. Most of these ARE in the corpus — "wifi" sits at rank
    # 32,756 — they are just far below the 25,000 cut, because film dialogue rarely says
    # them. Skipping words already present therefore did nothing at all: the word stayed
    # where it was and was dropped with the rest of the tail. Remove first, then insert.
    for i, w in enumerate(_MODERN):
        if w in seen:
            try:
                out.remove(w)
            except ValueError:
                pass
        seen.add(w)
        out.insert(min(_MODERN_RANK + i, len(out)), w)

    # Anything tokenisation destroyed outright, at the rank its base form earned so it lands
    # among words of comparable frequency rather than at the bottom of the table.
    # ⚠ y'all and o'clock have no usable base — normalise() rejects single letters other than
    # a/i — so the old base-lookup fallback appended them past position 46,000 and the 25,000
    # cut discarded both. A fixed band needs no base at all.
    for i, c in enumerate(_COMMON_CONTRACTIONS):
        if c in seen:
            continue
        out.insert(min(_CONTRACTION_RANK + i, len(out)), c)
        seen.add(c)
    return out


def load_allow(path):
    allow = set()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            w = normalise(line)
            if w is not None:
                allow.add(w)
            # A whitelist of base forms should not reject the possessive/contraction of a
            # word it does contain, so accept "dog" as authority for "dog's".
    return allow


def c_string(word):
    """A C string literal. Only [a-z'] reach here, so the sole escape needed is the quote."""
    return '"%s"' % word.replace("'", "\\'")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--freq", required=True, help="frequency-ordered word list")
    ap.add_argument("--allow", help="optional spelling whitelist")
    ap.add_argument("--limit", type=int, default=25000)
    ap.add_argument("--max-len", type=int, default=15,
                    help="drop words longer than this (they are never typed on a keypad)")
    ap.add_argument("--short-len", type=int, default=2,
                    help="words this long or shorter must also pass --short-max-rank. TWO, "
                         "not three: see the docstring. Three gutted the three-letter words")
    ap.add_argument("--short-max-rank", type=int, default=1000,
                    help="a word of --short-len or fewer characters is kept only if it ranks "
                         "this high; 0 disables. Removes initialisms without a curated list")
    ap.add_argument("--out", help="header to write; omit with --preview to write nothing")
    ap.add_argument("--preview", action="store_true", help="print the stats and some samples")
    ap.add_argument("--freq-name", default="", help="human name of the frequency source")
    ap.add_argument("--freq-url", default="", help="URL the frequency source came from")
    ap.add_argument("--freq-licence", default="", help="its licence, quoted exactly")
    ap.add_argument("--allow-name", default="")
    ap.add_argument("--allow-url", default="")
    ap.add_argument("--allow-licence", default="")
    args = ap.parse_args()

    if not args.out and not args.preview:
        ap.error("give --out, or --preview to inspect without writing")

    ranked = load_freq(args.freq)
    allow = load_allow(args.allow) if args.allow else None

    kept, dropped_spelling, dropped_long, dropped_short = [], 0, 0, 0
    for pos, w in enumerate(ranked):
        if len(w) > args.max_len:
            dropped_long += 1
            continue
        # A real two- or three-letter word is common; an initialism is not. See the docstring.
        if args.short_max_rank and len(w) <= args.short_len and pos >= args.short_max_rank:
            dropped_short += 1
            continue
        if allow is not None and w not in allow:
            # A contraction or possessive is accepted on the strength of its base form,
            # so a whitelist without "don't" still admits it if it has "don".
            base = w.split("'")[0]
            if base not in allow:
                dropped_spelling += 1
                continue
        kept.append(w)
        if len(kept) >= args.limit:
            break

    if not kept:
        sys.exit("nothing survived filtering — check --freq and --allow")

    # rank = position in `kept`. Sorting by (key, rank) puts every candidate run in
    # frequency order, which is the whole candidate model.
    rank = {w: i for i, w in enumerate(kept)}
    table = sorted(kept, key=lambda w: (digit_key(w), rank[w]))

    keys = [digit_key(w) for w in table]
    blob_bytes = sum(len(w) + 1 for w in table)
    ptr_bytes = len(table) * 4
    max_len = max(len(w) for w in table)
    max_key = max(len(k) for k in keys)

    # The candidate cap has to be a compile-time constant the engine can never overflow,
    # so measure the worst collision rather than guessing it.
    runs, i = [], 0
    while i < len(keys):
        j = i
        while j < len(keys) and keys[j] == keys[i]:
            j += 1
        runs.append(j - i)
        i = j
    worst_run = max(runs)
    first_hit = sum(1 for n in runs if n == 1)

    if args.preview:
        print("words kept          : %d of %d ranked" % (len(table), len(ranked)))
        print("dropped (spelling)  : %d" % dropped_spelling)
        print("dropped (too long)  : %d" % dropped_long)
        print("dropped (short+rare): %d" % dropped_short)
        print("longest word        : %d chars" % max_len)
        print("longest digit key   : %d" % max_key)
        print("distinct digit keys : %d" % len(runs))
        print("worst collision     : %d words on one key" % worst_run)
        print("unambiguous keys    : %d (%.1f%%)" % (first_hit, 100.0 * first_hit / len(runs)))
        print("flash: strings %s + pointers %s = %s"
              % (_kb(blob_bytes), _kb(ptr_bytes), _kb(blob_bytes + ptr_bytes)))
        print()
        print("first 12 in table order (key, word):")
        for n in range(min(12, len(table))):
            print("   %-10s %s" % (keys[n], table[n]))
        worst_at = runs.index(worst_run)
        start = sum(runs[:worst_at])
        print("worst collision run (%s):" % keys[start])
        print("   " + " ".join(table[start:start + worst_run]))

    if not args.out:
        return

    L = []
    w = L.append
    w("/* GENERATED by tools/gen_t9_dict.py — do not edit.")
    w(" *")
    w(" * The T9 predictive-text word table. %d words, sorted by keypad digit key and then by" % len(table))
    w(" * descending frequency, so a candidate run is contiguous and already in the order the")
    w(" * user wants to see it. Neither the key nor the frequency is stored: the key is")
    w(" * recomputed from the word, and the rank IS the position within the run.")
    w(" *")
    w(" * Lives in flash .rodata and is read by plain pointer dereference — ZERO internal heap,")
    w(" * which is the property the whole feature depends on (see the note on MenuOption in")
    w(" * GUI.h for what happens to this phone when something data-driven is put on that heap).")
    w(" *")
    w(" * Longest word %d chars, longest key %d digits, worst collision %d words on one key," % (max_len, max_key, worst_run))
    w(" * %d distinct keys of which %d (%.1f%%) are unambiguous." % (len(runs), first_hit, 100.0 * first_hit / len(runs)))
    w(" * Flash cost: %s of strings plus %s of pointers = %s." % (_kb(blob_bytes), _kb(ptr_bytes), _kb(blob_bytes + ptr_bytes)))
    w(" *")
    w(" * PROVENANCE — regenerate with the same inputs to reproduce this file byte for byte.")
    _provenance(w, "frequency", args.freq, args.freq_name, args.freq_url, args.freq_licence)
    if args.allow:
        _provenance(w, "whitelist", args.allow, args.allow_name, args.allow_url, args.allow_licence)
    w(" *   limit=%d max-len=%d short-len=%d short-max-rank=%d"
      % (args.limit, args.max_len, args.short_len, args.short_max_rank))
    w(" */")
    w("#pragma once")
    w("")
    w("#include <stdint.h>")
    w("")
    w("// Longest word in the table, NOT counting the NUL. Buffers that hold a candidate")
    w("// must be at least this + 1.")
    w("#define T9_MAX_WORD_LEN   %d" % max_len)
    w("// Longest digit key, i.e. the most keypresses a single word can take.")
    w("#define T9_MAX_KEY_LEN    %d" % max_key)
    w("// The worst measured collision on this table. A candidate buffer of this size can")
    w("// never overflow for an EXACT-length match. Completions are capped separately.")
    w("#define T9_WORST_RUN      %d" % worst_run)
    w("")
    w("/* Sorted by digit key (as a string), then by descending frequency within the key.")
    w(" * Do not reorder: the engine binary-searches this and takes candidate N as the Nth")
    w(" * entry of the matching run. */")
    w("static const char* const T9_WORDS[] = {")
    # Twelve to a line keeps the file diffable and the line length sane.
    for n in range(0, len(table), 12):
        w("  " + ", ".join(c_string(x) for x in table[n:n + 12]) + ",")
    w("};")
    w("static const uint16_t T9_WORD_COUNT = %d;" % len(table))
    if len(table) > 65535:
        sys.exit("more than 65535 words: widen T9_WORD_COUNT and the engine's index type")
    w("")

    text = "\n".join(L) + "\n"
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s (%s, %d words)" % (args.out, _kb(len(text.encode())), len(table)))


def _provenance(w, label, path, name, url, licence):
    w(" *   %s: %s" % (label, name or os.path.basename(path)))
    if url:
        w(" *     from   %s" % url)
    if licence:
        w(" *     licence %s" % licence)
    w(" *     sha256 %s" % sha256(path))


def _kb(n):
    return "%.1f KB" % (n / 1024.0) if n >= 1024 else "%d B" % n


if __name__ == "__main__":
    main()
