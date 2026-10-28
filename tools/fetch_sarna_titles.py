#!/usr/bin/env python3
"""Fetch article TITLES from the BattleTechWiki (sarna.net) for the T9 extra dictionary.

WHAT THIS TAKES, AND WHAT IT DELIBERATELY DOES NOT
--------------------------------------------------
Page titles only, through the MediaWiki API — a list of NAMES. It never requests article
text. That distinction is the whole point: sarna's prose is GNU FDL 1.2, and this has no
interest in the prose. Individual words and short names are not copyrightable on their own,
and what ends up on the phone is a vocabulary list, not documentation.

BattleTech and the unit names are trademarks of The Topps Company / Catalyst Game Labs.
Trademark law governs using a mark in commerce to identify goods; a word sitting in a
phone's predictive-text dictionary is not that. (Every phone dictionary contains "Google".)

⚠ Not legal advice, and worth re-reading if this output is ever SHIPPED rather than kept on
one person's SD card. The recommended shape — a file on the card, loaded at boot — keeps
third-party vocabulary out of the distributed firmware entirely.

BEING A GOOD CITIZEN
--------------------
robots.txt permits /wiki/ (only forum scripts and MediaWiki diff/oldid views are excluded).
This uses the documented API rather than scraping HTML, identifies itself, asks for the
largest page the API allows so there are as few requests as possible, and sleeps between
them. It is resumable: re-running appends from where the last continue token left off.

USAGE
-----
    python3 tools/fetch_sarna_titles.py --out tools/t9-corpus/sarna-titles.txt
    python3 tools/fetch_sarna_titles.py --out ... --max-requests 20     # a taster
"""
import argparse
import json
import sys
import time
import urllib.parse
import urllib.request

API = "https://www.sarna.net/wiki/api.php"
UA = ("WiPhone-T9-dictionary/1.0 (personal firmware project; "
      "github.com/Nikguy321/wiphone-meshtastic)")


def fetch(params):
    url = API + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=45) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--namespace", type=int, default=0,
                    help="0 = articles. Others are Talk/Category/Help and are not vocabulary")
    ap.add_argument("--limit", type=int, default=500, help="titles per request; 500 is the API max")
    ap.add_argument("--max-requests", type=int, default=400)
    ap.add_argument("--delay", type=float, default=0.4, help="seconds between requests")
    args = ap.parse_args()

    titles, cont, requests_made = [], None, 0
    while requests_made < args.max_requests:
        params = {
            "action": "query", "list": "allpages", "format": "json",
            "apnamespace": args.namespace, "aplimit": args.limit,
        }
        if cont:
            params["apcontinue"] = cont
        try:
            data = fetch(params)
        except Exception as e:
            sys.stderr.write("request %d failed (%s) - stopping and keeping what we have\n"
                             % (requests_made + 1, e))
            break
        requests_made += 1
        batch = data.get("query", {}).get("allpages", [])
        titles.extend(p["title"] for p in batch)
        sys.stderr.write("\r%d titles in %d requests" % (len(titles), requests_made))
        sys.stderr.flush()
        cont = data.get("continue", {}).get("apcontinue")
        if not cont:
            break
        time.sleep(args.delay)

    sys.stderr.write("\n")
    with open(args.out, "w", encoding="utf-8") as f:
        for t in titles:
            f.write(t + "\n")
    print("wrote %s: %d titles in %d requests%s"
          % (args.out, len(titles), requests_made,
             "" if not cont else "  (MORE REMAIN - raise --max-requests)"))


if __name__ == "__main__":
    main()
