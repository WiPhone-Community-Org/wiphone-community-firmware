#!/bin/bash
# cardday.sh — the tile pool, card by card.  See docs/maps.md "Pooling the tiles of several
# devices" for what this is doing and why it is cards rather than WiFi.
#
# Card day, in this order:
#   cardday.sh status                          # 0. THE EVENING BEFORE: the master, and COVEY's
#                                              #    streamed-only places — download those on
#                                              #    COVEY first, or the phones will not get them
#   cardday.sh covey pull                      # 1. COVEY's DOWNLOADED tiles into the master
#                                              #    (never its streamed ones)
#   cardday.sh phone2 pull /Volumes/WIPHONE    # 2. OLD card -> a full backup + its tiles into the master
#   cardday.sh phone2 push /Volumes/NEWCARD    # 3. that backup + the WHOLE master onto the new card
#   cardday.sh covey push                      # 4. whatever COVEY lacks, over SSH, into its
#                                              #    downloaded tree
# (phone1 the same as phone2; pull every phone before pushing any, so each card gets them all.)
#
# Never deletes a tile, at either end, and every step is re-runnable: a tile already on the
# card at the right length is skipped, a PNG already in the master is kept (an original beats
# a tile that has been through RGB565; the covey pull promotes with a hard link, which cannot
# replace one), and the backup is an --ignore-existing rsync. The only thing removed is the
# covey pull's own staging copies in <master>/.incoming.
#
# ELEVATION (0.9.80): the maps' altitude readout reads /maps/elev/<z>/<x>/<y>.elv on a card and
# /root/covey-elev/<z>/<x>/<y>.elv on COVEY. Their master is ~/elev-master/elev, built by
# tools/make_elev_tiles.py - NOT under the tile master, where push_band would convert it into a
# map area. card_clone.sh --no-tiles skips every numbered folder under /maps, elev included, so
# without the step below a card swap would silently drop the layer: push copies it, covey push
# rsyncs it. (Pull does not: the Mac's copy is the master, the card's is a copy of it.)
#
# CARDDAY_MASTER, CARDDAY_CARDS, CARDDAY_ELEV and CARDDAY_COVEY_HOST override the master tree, the
# card backups, the elevation master and the ssh host.
set -u -o pipefail   # pipefail: `step | tee -a "$LOG" || die` tests the step, not tee
REPO=/Users/{username}/wiphone
M=${CARDDAY_MASTER:-~/tiles-master}
ELEV=${CARDDAY_ELEV:-~/elev-master/elev}
CARDS=${CARDDAY_CARDS:-~/wiphone-cards}
COVEY=${CARDDAY_COVEY_HOST:-covey}
LOG=$M/cardday.log
TILE=131072                              # one .565 tile on a card
CARD_KEEP=$((256 * 1024 * 1024))         # room a push leaves on a card: health.log, mesh DB, pins, saves
COVEY_KEEP=$((4 * 1024 * 1024 * 1024))   # room a covey push leaves on COVEY's card

say() { echo "$(date '+%H:%M:%S') $*" | tee -a "$LOG"; }
die() { echo "$*" >&2; exit 2; }

# usgs-img is photography: COVEY's cache holds it as JPEG bytes under .png names (its
# downloader keeps whatever the server sent), and a photo as a true-colour PNG is 105 KB
# against 42 KB. Anything map-shaped stays PNG.
jpeg_flag() { case "$1" in usgs-img|*-img|*aerial*) echo "--jpeg" ;; *) echo "" ;; esac; }

count_png() { find "$1" -type f -name '*.png' 2>/dev/null | wc -l | tr -d ' '; }
count_565() { find "$1" -type f -name '*.565' 2>/dev/null | wc -l | tr -d ' '; }
gb() { awk -v b="$1" 'BEGIN { printf "%.1f GB", b / 1073741824 }'; }

# Tiles in one zoom band of one tree: a master source (a PNG with something in it) or a card
# area (a .565 of exactly the phone's length — anything else convert_tiles.py rewrites).
count_band() {   # $1 tree, $2 lowest zoom, $3 highest, $4 png|565
  local n=0 zd z
  for zd in "$1"/*/; do
    [ -d "$zd" ] || continue
    z=$(basename "$zd")
    case "$z" in ''|*[!0-9]*) continue ;; esac
    [ "$z" -ge "$2" ] && [ "$z" -le "$3" ] || continue
    if [ "$4" = png ]; then
      n=$((n + $(find "$zd" -type f -name '*.png' -size +0 | wc -l)))
    else
      n=$((n + $(find "$zd" -type f -name '*.565' -size ${TILE}c | wc -l)))
    fi
  done
  echo "$n"
}

# Bytes the master's z$1-$2 still needs on card $3. Counts, not paths: it assumes what the
# card holds is a subset of the master, which a pull before the push makes true.
band_need() {
  local need=0 s m c
  for s in "$M"/*/; do
    [ -d "$s" ] || continue
    m=$(count_band "$s" "$1" "$2" png)
    c=$(count_band "$3/maps/$(basename "$s")" "$1" "$2" 565)
    [ "$m" -gt "$c" ] && need=$((need + (m - c) * TILE))
  done
  echo "$need"
}

card_free() { echo $(( $(df -k "$1" | tail -1 | awk '{print $4}') * 1024 )); }

status() {
  echo "master $M:"
  local tot=0
  for s in "$M"/*/; do
    [ -d "$s" ] || continue
    local n; n=$(count_png "$s")
    [ "$n" = 0 ] && continue
    printf "  %-12s %7d tiles  %6s  (z%s)\n" "$(basename "$s")" "$n" \
           "$(du -sh "$s" | cut -f1)" "$(ls "$s" | sort -n | tr '\n' ' ' | sed 's/ $//')"
    tot=$((tot + n))
  done
  printf "  %-12s %7d tiles = %d.%d GB as .565 on a card\n" TOTAL "$tot" \
         $((tot * 131072 / 1073741824)) $(( (tot * 131072 % 1073741824) * 10 / 1073741824 ))
  for c in "$CARDS"/*/; do
    [ -d "$c" ] || continue
    printf "  backup %-8s %d files, %s\n" "$(basename "$c")" \
           "$(find "$c" -type f ! -name '._*' | wc -l | tr -d ' ')" "$(du -sh "$c" | cut -f1)"
  done
  # Best effort, and quick to give up: status must work with COVEY switched off. --master: a
  # streamed-only place whose ground the master already holds is listed apart (the phones get it).
  python3 "$REPO/tools/covey_pull.py" report --host "$COVEY" --master "$M"
}

pull() {          # $1 = phone tag, $2 = card volume
  local tag=$1 card=$2 dst=$CARDS/$1
  [ -d "$card" ] || die "$card is not mounted (ls /Volumes)"
  [ -d "$card/maps" ] || say "⚠ no /maps on this card — copying it anyway"
  say "=== PULL $tag from $card ==="
  # The manifest is taken from the CARD, not from the copy: it is the record of what this card
  # held, and it is what a later push is checked against.
  ( cd "$card" && find . -type f ! -name '._*' -exec stat -f '%z %N' {} + | sort -k2 ) > "$M/manifest_$tag.txt"
  say "$tag: the card holds $(wc -l < "$M/manifest_$tag.txt" | tr -d ' ') files, $(grep -c '\.565$' "$M/manifest_$tag.txt" | tr -d ' ') of them tiles"
  # Everything but the tiles: they come back from the master, which ends this pull holding a
  # PNG for every one of them. See the note in card_clone.sh.
  "$REPO/tools/card_clone.sh" pull --no-tiles "$card" "$dst" 2>&1 | tee -a "$LOG" || die "the card copy did not finish — re-run this same command"
  local added=0
  for a in "$card"/maps/*/; do          # read the tiles off the CARD (they are not in the copy)
    [ -d "$a" ] || continue
    local area; area=$(basename "$a")
    local have; have=$(count_565 "$a")
    [ "$have" = 0 ] && { say "  $area: no tiles, skipped"; continue; }
    local before; before=$(count_png "$M/$area")
    say "  $area: $have tiles on the card -> $M/$area (had $before)"
    python3 "$REPO/tools/tiles_565_to_png.py" "$a" "$M/$area" $(jpeg_flag "$area") 2>&1 | grep -E '^(wrote|[0-9]+ tile|  (skipped|stopped))' | tee -a "$LOG"
    # The converter's own status, not grep's: a STOPPED run (the Mac's disk full) exits 1.
    [ "${PIPESTATUS[0]}" = 0 ] || die "$area: the tiles did not all come off the card — see above; re-run this same command"
    local after; after=$(count_png "$M/$area")
    say "  $area: master $before -> $after (+$((after - before)) this phone had that nothing else did)"
    added=$((added + after - before))
  done
  say "=== PULL $tag done: +$added tiles into the master ==="
  say "Eject it:  diskutil eject $card"
}

push() {          # $1 = phone tag, $2 = card volume
  local tag=$1 card=$2 src=$CARDS/$1
  [ -d "$card" ] || die "$card is not mounted (ls /Volumes)"
  [ -d "$src" ] || die "no backup at $src — run 'cardday.sh $tag pull' with the OLD card first"
  case "$(diskutil info "$card" | awk -F: '/File System Personality/{print $2}' | xargs)" in
    *FAT32*|*MS-DOS*) ;;
    *) die "$card is not FAT32 — the phone does not read exFAT. diskutil eraseDisk FAT32 WIPHONE MBRFormat diskN" ;;
  esac
  say "=== PUSH $tag to $card ==="
  say "step 1: the old card's contents ($(find "$src" -type f ! -name '._*' | wc -l | tr -d ' ') files, tiles excluded — they are step 2)"
  "$REPO/tools/card_clone.sh" push --no-tiles "$src" "$card" 2>&1 | grep -v '^eject before' | tee -a "$LOG" || die "the restore did not finish — re-run this same command"
  # Two bands, every source's z0-16 first and then z17: a card that fills loses only z17, never
  # a whole source behind OTM ($M/*/ is alphabetical). z18 and up are never written — OTM's z18
  # is a "max zoom layer = 17" placeholder, not a map.
  local need free z17="written"
  need=$(band_need 0 16 "$card"); free=$(card_free "$card")
  [ "$need" -le $((free - CARD_KEEP)) ] || die "the card has $(gb "$free") free and z0-16 alone needs $(gb "$need") more (plus $(gb $CARD_KEEP) kept free for the phone) — this card is too small for the pool"
  say "step 2: the master, z0-16 of every source ($(gb "$need") to write, $(gb "$free") free)"
  push_band "$card" 0 16
  need=$(band_need 17 17 "$card"); free=$(card_free "$card")
  if [ "$need" -gt $((free - CARD_KEEP)) ]; then
    z17="LEFT OFF: no room"
    say "⚠ step 3: z17 NOT written — it needs $(gb "$need") and the card has $(gb "$free") left (plus $(gb $CARD_KEEP) kept free for the phone). Everything else is on the card."
  else
    say "step 3: the master's z17 ($(gb "$need") to write, $(gb "$free") free)"
    push_band "$card" 17 17
  fi
  push_elev "$card"
  dot_clean -m "$card" 2>/dev/null
  say "=== PUSH $tag done: $(count_565 "$card/maps") tiles, $(df -h "$card" | tail -1 | awk '{print $4}') free (z17 $z17) ==="
  say "Eject it:  diskutil eject $card    then: phone on, Maps, Menu > Rescan card"
}

push_elev() {     # $1 = card volume: the altitude layer, after the maps (it is the smaller loss)
  local card=$1 need free n
  if [ ! -d "$ELEV" ]; then
    say "step 4: no elevation master at $ELEV - the altitude readout will say so (tools/make_elev_tiles.py all)"
    return 0
  fi
  n=$(find "$ELEV" -type f -name '*.elv' -size ${TILE}c | wc -l | tr -d ' ')
  need=$(( $(du -sk "$ELEV" | cut -f1) * 1024 )); free=$(card_free "$card")
  if [ "$need" -gt $((free - CARD_KEEP)) ]; then
    say "⚠ step 4: elevation NOT written - it needs up to $(gb "$need") and the card has $(gb "$free") left"
    return 0
  fi
  say "step 4: elevation, $n tiles (up to $(gb "$need"))"
  mkdir -p "$card/maps/elev" || die "could not make $card/maps/elev"
  # -rt --ignore-existing: FAT32 keeps no owners, and a tile already there at any size is left
  # (make_elev_tiles.py never changes a tile's bytes for the same source PNG).
  rsync -rt --ignore-existing --exclude '._*' --exclude '*.tmp' "$ELEV/" "$card/maps/elev/" 2>&1 | tail -2 | tee -a "$LOG"
  [ "${PIPESTATUS[0]}" = 0 ] || die "elevation: the copy STOPPED - re-run this same command, it carries on"
  say "  elev: $(find "$card/maps/elev" -type f -name '*.elv' -size ${TILE}c | wc -l | tr -d ' ') tiles on the card"
}

push_band() {     # $1 = card volume, $2-$3 = zoom band
  local card=$1 lo=$2 hi=$3 s area t0
  for s in "$M"/*/; do
    [ -d "$s" ] || continue
    area=$(basename "$s")
    [ "$(count_band "$s" "$lo" "$hi" png)" = 0 ] && continue
    t0=$SECONDS
    python3 "$REPO/tools/convert_tiles.py" "$s" "$card/maps/$area" --zoom "$lo-$hi" 2>&1 | grep -E '^(wrote|[0-9]+ tile|  (skipped|stopped))' | tee -a "$LOG"
    # The converter's own status: it exits 1 when it STOPPED (card full or pulled).
    [ "${PIPESTATUS[0]}" = 0 ] || die "$area z$lo-$hi: the card copy STOPPED — see above; re-run this same command once fixed, it carries on"
    say "  $area z$lo-$hi: $(count_band "$card/maps/$area" "$lo" "$hi" 565) tiles on the card ($((SECONDS - t0))s)"
  done
}

covey_pull() {
  [ -d "$M" ] || die "no master at $M"
  say "=== PULL covey: its DOWNLOADED tiles into the master; what it only streamed stays there ==="
  # Never --without-report here: with no streamed-only report from COVEY the pull REFUSES (exit
  # 2), because the COVEY that cannot give one still keeps its streamed tiles in the downloaded tree.
  python3 -u "$REPO/tools/covey_pull.py" pull --master "$M" --host "$COVEY" 2>&1 | tee -a "$LOG"
  case "${PIPESTATUS[0]}" in
    0) ;;
    2) die "the COVEY pull did not start — nothing moved; read the lines above" ;;
    *) die "the COVEY pull did not finish cleanly — read the ⚠ lines above; re-run this same command (what landed is kept)" ;;
  esac
}

covey_push() {
  local s area t0 avail need refused=""
  say "=== PUSH covey: the master into its DOWNLOADED tree ==="
  for s in "$M"/*/; do
    [ -d "$s" ] || continue
    area=$(basename "$s")
    [ "$(count_png "$s")" = 0 ] && continue
    # sudo: /root is root's, and df cannot stat a path it cannot reach. The whole source is
    # counted, not only what COVEY lacks, so this errs towards refusing.
    avail=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$COVEY" "sudo df -B1 --output=avail /root/covey-tiles | tail -1" | tr -d ' ')
    case "$avail" in ''|*[!0-9]*) die "could not read COVEY's free space — is it on and reachable (ssh $COVEY)?" ;; esac
    need=$(( $(du -sk "$s" | cut -f1) * 1024 ))
    if [ $((avail - need)) -lt $COVEY_KEEP ]; then
      say "⚠ $area NOT pushed: it is $(gb "$need") and COVEY has $(gb "$avail") free — that would leave under $(gb $COVEY_KEEP)"
      refused="$refused $area"
      continue
    fi
    t0=$SECONDS
    # -rt, not -a: a root receiver would otherwise keep the Mac's uid 501 / gid 20 on every tile.
    rsync -rt --ignore-existing --rsync-path='sudo rsync' "$s" "$COVEY:/root/covey-tiles/$area/" 2>&1 | tail -2 | tee -a "$LOG"
    [ "${PIPESTATUS[0]}" = 0 ] || die "$area: the rsync to COVEY did not finish — re-run this same command"
    say "  $area: $(ssh -o BatchMode=yes "$COVEY" "sudo find /root/covey-tiles/$area -type f -name '*.png' -size +0 | wc -l" | tr -d ' ') tiles on COVEY ($((SECONDS - t0))s)"
  done
  if [ -d "$ELEV" ]; then
    t0=$SECONDS
    # Beside the tile tree, never inside it: /root/covey-tiles is the tile store's, and its
    # janitor judges every file there as a tile.
    rsync -rt --ignore-existing --exclude '._*' --exclude '*.tmp' --rsync-path='sudo rsync' "$ELEV/" "$COVEY:/root/covey-elev/" 2>&1 | tail -2 | tee -a "$LOG"
    [ "${PIPESTATUS[0]}" = 0 ] || die "elevation: the rsync to COVEY did not finish - re-run this same command"
    say "  elev: $(ssh -o BatchMode=yes "$COVEY" "sudo find /root/covey-elev -type f -name '*.elv' -size ${TILE}c | wc -l" | tr -d ' ') tiles on COVEY ($((SECONDS - t0))s)"
  else
    say "  elev: no elevation master at $ELEV - skipped (tools/make_elev_tiles.py all)"
  fi
  [ -z "$refused" ] || die "=== PUSH covey: NOT pushed for want of room on COVEY:$refused ==="
  say "=== PUSH covey done ==="
}

case "${1:-}" in
  status) status ;;
  covey)
    case "${2:-}" in
      pull) covey_pull ;;
      push) covey_push ;;
      *) die "usage: cardday.sh covey pull|push" ;;
    esac ;;
  phone1|phone2)
    case "${2:-}" in
      pull) [ $# -eq 3 ] || die "usage: cardday.sh $1 pull /Volumes/<card>"; pull "$1" "$3" ;;
      push) [ $# -eq 3 ] || die "usage: cardday.sh $1 push /Volumes/<card>"; push "$1" "$3" ;;
      *) die "usage: cardday.sh $1 pull|push /Volumes/<card>" ;;
    esac ;;
  *) awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' "$0"; exit 2 ;;
esac
