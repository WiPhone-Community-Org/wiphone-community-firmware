# T9 source corpus

Not committed — see `.gitignore`. Refetch it before regenerating `t9_dict.h`:

```bash
mkdir -p tools/t9-corpus
curl -sSL -o tools/t9-corpus/en_50k.txt \
  https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/en/en_50k.txt
shasum -a 256 tools/t9-corpus/en_50k.txt
# expect 5351ff405b1126ef555791dd4d9798a48e3e9a501a9fc481a9da957752cfb458
```

That hash is also recorded in the provenance banner of the generated
`WiPhone/src/assets/t9_dict.h`, so a rebuild can be proven to have used the same input.

**Source:** [hermitdave/FrequencyWords](https://github.com/hermitdave/FrequencyWords),
word frequencies derived from the OpenSubtitles corpus. **Licence: MIT.**

Only an *ordered list of ordinary English words* is compiled into the firmware — no counts,
no n-grams, no contiguous source text. Regenerate with:

```bash
python3 tools/gen_t9_dict.py --freq tools/t9-corpus/en_50k.txt --limit 25000 \
  --out WiPhone/src/assets/t9_dict.h \
  --freq-name "OpenSubtitles 2018, English 50k (hermitdave/FrequencyWords, content/2018/en/en_50k.txt)" \
  --freq-url "https://github.com/hermitdave/FrequencyWords" --freq-licence "MIT"
```

---

# The extra dictionary (your own words)

Optional, and it never ships in the firmware — it is a file on the SD card at
`/t9/extra.txt`, loaded into PSRAM at boot. That is deliberate: one person's jargon has no
business in a stranger's phone, and keeping it off the card means the licence of whatever
you harvested is never a question for anyone else.

Corpus files here are gitignored, and so is the generated word list. Only the tools are
committed.

## From a wiki (the BattleTech example)

```bash
python3 tools/fetch_sarna_titles.py --out tools/t9-corpus/sarna-titles.txt
python3 tools/gen_t9_extra.py \
  --titles tools/t9-corpus/sarna-titles.txt \
  --exclude-freq tools/t9-corpus/en_50k.txt \
  --min-titles 2 --out /tmp/extra.txt
```

`--min-titles 2` is the useful knob. Words appearing in exactly one title across a 93,000
article wiki are where both the junk and the cost live — that tail is the only thing that
lengthens candidate runs. At 2 you get ~8,700 words for 68 KB and the worst run stays at 9.

**Page TITLES only.** This never requests article text: sarna's prose is GNU FDL 1.2 and a
vocabulary list is not a copy of it. See the header of `fetch_sarna_titles.py`.

## From any word list

`--titles` takes any file with one entry per line, so a team roster, a list of callsigns or
place names works the same way. Words already in the built-in dictionary are dropped.

## Getting it onto the phone

Over WiFi, without opening the phone:

```bash
# on the phone's console:  up on t9
python3 tools/chunk_push.py http://<phone-ip> extra.txt
# then:                    up off   and   t9 reload
```

The file must be named `extra.txt` — the uploader keeps the browser's filename. Or copy it
to `/t9/extra.txt` on the card yourself, with the phone **off**.

`t9` on the console reports how many extra words are loaded and, if none, why.
