#!/usr/bin/env python3
"""Build a T9 EXTRA dictionary — the user's own vocabulary, for the SD card.

WHY THIS IS A FILE ON THE CARD AND NOT COMPILED IN
--------------------------------------------------
Three reasons, in order of how much they matter:

1. Nothing third-party ends up in the distributed firmware. The webflasher installs the same
   image for everyone, and one person's BattleTech vocabulary has no business in a stranger's
   phone. It also means the licence question never arises: the words stay on the card of the
   person who wanted them.
2. Adding words costs a file copy, not a reflash.
3. It generalises. This is not "a BattleTech build", it is "bring your own words" — ham radio
   callsigns, medical terms, place names, a team roster.

The cost is one SD read at boot. That is deliberately NOT on the keypress path: reading a
dictionary per keystroke is exactly the 1.5 s freeze this firmware has fought before.

FORMAT
------
Plain UTF-8 text, one lowercase word per line, ALREADY SORTED by keypad digit key and then
by descending relevance. The phone does not sort it — it binary-searches it exactly as it
searches the built-in table, so the order in this file IS the candidate order. A hand-edited
file that is out of order will simply fail to find some words; it cannot crash the phone.

The words always rank BELOW the built-in English ones, because the engine searches the main
table first and appends this run after it. That is the behaviour you want from jargon: there
when you need it, never in the way.

INPUT
-----
Wiki page titles, one per line (see tools/fetch_sarna_titles.py), or any plain word list.
Words already in the built-in dictionary are dropped — they are already reachable.

USAGE
-----
    python3 tools/gen_t9_extra.py \\
        --titles tools/t9-corpus/sarna-titles.txt \\
        --exclude-freq tools/t9-corpus/en_50k.txt \\
        --min-titles 2 --out /Volumes/WIPHONE/t9/extra.txt      # the /t9 dir must exist
"""
import argparse
import collections
import importlib.util
import os
import re
import sys

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("gen_t9_dict", os.path.join(_here, "gen_t9_dict.py"))
_g = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_g)

# Roman numerals and ordinal suffixes are not words — they are what is left over when the
# digits are stripped out of "1st Battalion" and "Battle of Tukayyid II".
_ROMAN = re.compile(r"^(?=[ivxlcdm]+$)m*(cm|cd|d?c{0,3})(xc|xl|l?x{0,3})(ix|iv|v?i{0,3})$")
_ORDINALS = {"th", "st", "nd", "rd"}
_VOWELS = set("aeiouy")


def usable(w):
    """Reject what a keypad dictionary should not carry, before frequency is even considered."""
    if len(w) < 4:
        return False              # every real short word is already in the built-in table
    if w in _ORDINALS or _ROMAN.match(w):
        return False
    if not (_VOWELS & set(w)):
        return False              # sls, tro, rnd: abbreviations, not words
    return True


def builtin_words(freq_path, limit, short_len, short_max_rank, max_len):
    """Rebuild the shipped dictionary's word SET, so this tool drops what is already there.

    Deliberately re-derived with gen_t9_dict's own loader and the same filters rather than
    parsed out of the generated header: one definition of what the built-in list contains.
    """
    ranked = _g.load_freq(freq_path)
    out = set()
    for pos, w in enumerate(ranked):
        if len(w) > max_len:
            continue
        if short_max_rank and len(w) <= short_len and pos >= short_max_rank:
            continue
        out.add(w)
        if len(out) >= limit:
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--titles", required=True, help="page titles or a plain word list, one per line")
    ap.add_argument("--exclude-freq", help="the frequency list the built-in dictionary was built from")
    ap.add_argument("--limit", type=int, default=25000, help="must match the built-in build")
    # MUST MATCH gen_t9_dict.py's default, or the "already built in" set this subtracts
    # is not the set the phone actually carries and words get duplicated or lost.
    ap.add_argument("--short-len", type=int, default=2)
    ap.add_argument("--short-max-rank", type=int, default=1000)
    ap.add_argument("--max-len", type=int, default=15)
    ap.add_argument("--min-titles", type=int, default=2,
                    help="keep a word only if it appears in at least this many titles. The "
                         "one-title tail is where the junk lives and it is the only thing "
                         "that lengthens candidate runs")
    ap.add_argument("--out", help="the file to write; omit with --preview")
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()

    if not args.out and not args.preview:
        ap.error("give --out, or --preview to inspect without writing")

    have = set()
    if args.exclude_freq:
        have = builtin_words(args.exclude_freq, args.limit, args.short_len,
                             args.short_max_rank, args.max_len)

    counts = collections.Counter()
    lines = 0
    with open(args.titles, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            lines += 1
            # A trailing "(disambiguation)" is wiki plumbing, not vocabulary.
            line = re.sub(r"\(.*?\)", " ", line)
            for raw in re.split(r"[^A-Za-z']+", line):
                w = _g.normalise(raw)
                if w and len(w) <= args.max_len and w not in have and usable(w):
                    counts[w] += 1

    kept = [w for w, c in counts.items() if c >= args.min_titles]
    # Relevance rank = how many titles mention it. Then sorted by digit key so the phone can
    # binary-search it, with the rank breaking ties INSIDE a key — the same contract as the
    # built-in table.
    rank = {w: i for i, w in enumerate(sorted(kept, key=lambda w: (-counts[w], w)))}
    table = sorted(kept, key=lambda w: (_g.digit_key(w), rank[w]))

    if args.preview or not args.out:
        runs = collections.Counter(_g.digit_key(w) for w in table)
        print("titles read        : %d" % lines)
        print("distinct usable    : %d" % len(counts))
        print("kept (>= %d titles): %d" % (args.min_titles, len(table)))
        print("longest word       : %d" % (max(len(w) for w in table) if table else 0))
        print("file size          : %.1f KB" % (sum(len(w) + 1 for w in table) / 1024.0))
        print("worst run WITHIN the extra table: %d" % (max(runs.values()) if runs else 0))
        print()
        by_rank = sorted(kept, key=lambda w: rank[w])
        print("most relevant 30 : %s" % " ".join(by_rank[:30]))
        print("least relevant 15: %s" % " ".join(by_rank[-15:]))

    if not args.out:
        return

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# WiPhone T9 extra dictionary. One word per line, sorted by keypad digit\n")
        f.write("# key then by relevance. The phone does NOT re-sort this: the order here is\n")
        f.write("# the candidate order. Generated by tools/gen_t9_extra.py - see that file.\n")
        for w in table:
            f.write(w + "\n")
    print("wrote %s: %d words, %.1f KB" % (args.out, len(table),
                                           os.path.getsize(args.out) / 1024.0))


if __name__ == "__main__":
    main()
