#!/bin/sh
# Split the corpus into CHUNKS pieces and put them on the hub. Runs ONCE, ever -
# every later generation reads these same chunks.
#
# This runs on a box rather than locally because the intermediate is ~25 GB
# (whole EPD + chunks + their compressed copies) and because splitting is I/O
# bound: it does not need the fleet, just one box with disk.
#
# Expects $HUB_DIR/corpus/human.epd.zst to already be uploaded - that single
# ~1.8 GB file is the only large thing that ever leaves your machine.
#
# Usage:  ./prepare-corpus.sh [path/to/job.env]

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$DIR/common.sh"

load_config "${1:-$DIR/job.env}"
require HUB HUB_PORT HUB_DIR CHUNKS

WORK="${WORK:-$HOME/corpus}"
SRC="${CORPUS_NAME:-human.epd}"
mkdir -p "$WORK"
cd "$WORK"

if hub_exists "corpus/chunk_$(printf '%02d' $((CHUNKS - 1))).zst"; then
    log "corpus already split into $CHUNKS chunks on the hub; nothing to do"
    exit 0
fi

command -v zstd >/dev/null || { apt-get update -qq && apt-get install -y -qq zstd rsync; }

if [ ! -f "$SRC" ]; then
    log "fetching corpus/$SRC.zst from the hub"
    hub_get "corpus/$SRC.zst" "$SRC.zst"
    log "decompressing"
    zstd -d -q -f "$SRC.zst" -o "$SRC"
fi

lines=$(wc -l < "$SRC")
log "$SRC: $lines lines, splitting into $CHUNKS chunks"

# -n l/N splits on LINE boundaries: a chunk never ends mid-FEN, which a byte
# split would do and which datagen would then reject or, worse, mislabel.
# --filter compresses each piece as it is written, so the uncompressed chunks
# never exist on disk at the same time as the source.
rm -f chunk_*.zst
split -n "l/$CHUNKS" -d --filter='zstd -12 -q -o "$FILE.zst"' "$SRC" chunk_

# The split must be lossless: a chunk boundary that dropped or duplicated a line
# would shift every downstream count and nothing else checks it.
total=0
for f in chunk_*.zst; do
    n=$(zstd -dc "$f" | wc -l)
    log "$f: $n lines"
    total=$((total + n))
done
[ "$total" -eq "$lines" ] || die "split produced $total lines from $lines - refusing to upload"
log "split verified: $total lines"

for f in chunk_*.zst; do
    log "uploading $f"
    hub_put "$f" "corpus/$f"
done

log "corpus ready: $CHUNKS chunks on the hub"
