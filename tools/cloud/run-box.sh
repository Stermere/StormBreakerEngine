#!/bin/sh
# Run this box's share of a generation.
#
# Work is addressed by CHUNK (label) or BATCH (selfplay), never by box, and every
# unit carries a done-marker on the hub. Two consequences worth the indirection:
# a rerun skips finished units instead of redoing them, and a unit belonging to a
# box that died can be re-dispatched to any other box without renaming anything.
#
# Usage:  ./run-box.sh <box_index> [path/to/job.env]

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$DIR/common.sh"

BOX="${1:?usage: run-box.sh <box_index> [job.env]}"

# load_config sources job.env with `set -a`, so anything the caller exported is
# about to be overwritten by the file. Capture the one knob worth overriding
# from the environment first: `JOB=calibrate ./run-box.sh 0` has to beat the
# JOB= in the config, or calibration silently runs the real job.
_job_override="${JOB:-}"
load_config "${2:-$DIR/job.env}"
if [ -n "$_job_override" ]; then
    JOB="$_job_override"
fi

require GEN JOB NODES HUB HUB_DIR BOXES
resolve_threads

# systemd hands a service a near-empty environment - no HOME - while ssh sets it.
# Under `set -u` that difference is fatal rather than merely different, so the
# same script that works over ssh dies one line in when run as a unit.
WORK="${WORK:-${HOME:-/root}/engine}"
DATAGEN="$WORK/datagen"
[ -x "$DATAGEN" ] || die "no datagen at $DATAGEN - run provision.sh first"

cd "$WORK"
mkdir -p data corpus

log "box $BOX/$BOXES, job=$JOB gen=$GEN nodes=$NODES threads=$THREADS"

# Every unit is verified on the box before it is uploaded. Relabelling a sample
# from a cleared engine is the only check that catches a box whose build differs
# from the rest of the fleet, and it is far cheaper here than after a download.
verify_and_push() {
    _first="$1"
    _glob="$2"
    _marker="$3"

    # -nodes and -hash come from the shard's own manifest rather than from
    # job.env. A label reproduces only under the conditions it was made under,
    # and the hash size is one of them: search_clear() empties the table but
    # keeps its geometry, so a position collides differently in a 64 MB table
    # than in an 8 MB one and roughly one record in a thousand scores
    # differently. `selfplay` labels with 64 MB and `verify` used to default to
    # 8, which failed one sampled record in every few shards for no reason -
    # after hours of selfplay that had produced perfectly good data.
    #
    # datagen reads the manifest itself now. The binary here was built at
    # $COMMIT and may predate that, so pass the values explicitly; when both
    # know, they agree and nothing warns.
    _man="${_first%.cnn}.json"
    [ -f "$_man" ] || die "no manifest beside $_first"
    _nodes=$(sed -n 's/.*"nodes": *\([0-9][0-9]*\).*/\1/p' "$_man" | head -1)
    _hash=$(sed -n 's/.*"hash_mb": *\([0-9][0-9]*\).*/\1/p' "$_man" | head -1)
    [ -n "$_nodes" ] && [ -n "$_hash" ] || die "$_man records no nodes/hash_mb"

    log "verifying $_first at $_nodes nodes, $_hash MB"
    "$DATAGEN" verify "$_first" -relabel 256 -nodes "$_nodes" -hash "$_hash"

    log "uploading $_glob"
    for _f in $_glob; do
        hub_put "$_f" "$GEN/shards/$(basename "$_f")"
    done

    : > "data/$_marker"
    hub_put "data/$_marker" "$GEN/done/$_marker"
    log "$_marker complete"
}

fetch_chunk() {
    _name="$1"
    if [ ! -f "corpus/chunk_$_name" ]; then
        log "fetching corpus chunk $_name"
        hub_get "corpus/chunk_$_name.zst" "corpus/chunk_$_name.zst"
        zstd -d -q -f "corpus/chunk_$_name.zst" -o "corpus/chunk_$_name"
        rm -f "corpus/chunk_$_name.zst"
    fi
}

run_label() {
    require LABEL_SOURCE CHUNKS
    _i=0
    while [ "$_i" -lt "$CHUNKS" ]; do
        if mine "$_i"; then
            _n=$(printf '%02d' "$_i")
            _marker="${GEN}_${LABEL_SOURCE}_c${_n}.done"

            if hub_exists "$GEN/done/$_marker"; then
                log "chunk $_n already done, skipping"
            else
                fetch_chunk "$_n"
                _pat="data/${GEN}_${LABEL_SOURCE}_c${_n}_%02d.cnn"
                log "labelling chunk $_n"
                # -resume checkpoints every 30s. THREADS must not change between
                # attempts on the same chunk: workers split the input by line, so
                # a different count reads different lines and the resumed shard
                # would hold records for lines it never labelled.
                "$DATAGEN" label "corpus/chunk_$_n" -o "$_pat" \
                    -source "$LABEL_SOURCE" -nodes "$NODES" -threads "$THREADS" -resume

                verify_and_push "data/${GEN}_${LABEL_SOURCE}_c${_n}_00.cnn" \
                    "data/${GEN}_${LABEL_SOURCE}_c${_n}_*" "$_marker"
                rm -f "corpus/chunk_$_n"
            fi
        fi
        _i=$((_i + 1))
    done
}

run_selfplay() {
    require SELFPLAY_GAMES SELFPLAY_BATCHES SELFPLAY_SEED_BASE SELFPLAY_SEED_STRIDE

    # Built once, here, rather than per batch. Each knob is optional and falls
    # through to datagen's own default when job.env does not name it, so a
    # config from an earlier generation still describes the same job it did.
    _sp_opts=""
    if [ -n "${BOOK_SHA:-}" ]; then
        # Named for its hash, so the shard manifest records WHICH book the games
        # were drawn from. gen-004 is extended with a second book; under one
        # filename the two would be indistinguishable after the fact.
        _book="external/books/book-$BOOK_SHA.epd"
        [ -f "$_book" ] || die "BOOK_SHA is set but $_book is missing - reprovision"
        _sp_opts="$_sp_opts -book $_book"
    fi
    [ -z "${SELFPLAY_TREE:-}" ] || _sp_opts="$_sp_opts -tree $SELFPLAY_TREE"
    [ -z "${SELFPLAY_OPENING:-}" ] || _sp_opts="$_sp_opts -opening $SELFPLAY_OPENING"
    [ -z "${SELFPLAY_OPENING_SCORE:-}" ] \
        || _sp_opts="$_sp_opts -openingscore $SELFPLAY_OPENING_SCORE"
    log "selfplay options:${_sp_opts:- (datagen defaults)}"

    _i=0
    while [ "$_i" -lt "$SELFPLAY_BATCHES" ]; do
        if mine "$_i"; then
            _n=$(printf '%02d' "$_i")
            _marker="${GEN}_sp_b${_n}.done"
            _seed=$((SELFPLAY_SEED_BASE + _i * SELFPLAY_SEED_STRIDE))

            if hub_exists "$GEN/done/$_marker"; then
                log "batch $_n already done, skipping"
            else
                _pat="data/${GEN}_sp_b${_n}_%02d.cnn"
                log "selfplay batch $_n, seed $_seed"
                # selfplay has no -resume: an interrupted batch is redone from
                # scratch, which is why batches exist at all. Nothing is uploaded
                # until datagen exits 0, so a partial shard never reaches the hub.
                # $_sp_opts is deliberately unquoted: it is a built-up
                # argument list, not one argument.
                # shellcheck disable=SC2086
                "$DATAGEN" selfplay -o "$_pat" -games "$SELFPLAY_GAMES" \
                    -nodes "$NODES" -threads "$THREADS" -seed "$_seed" $_sp_opts

                verify_and_push "data/${GEN}_sp_b${_n}_00.cnn" \
                    "data/${GEN}_sp_b${_n}_*" "$_marker"
            fi
        fi
        _i=$((_i + 1))
    done
}

# Phase 3 of the plan: measure records/sec on the real build before sizing a
# fleet from a guess. Writes nothing to the hub.
run_calibrate() {
    require LABEL_SOURCE
    fetch_chunk 00
    _max="${CALIBRATE_MAX:-5000}"
    rm -f data/calib_*
    log "calibrating: $_max records per worker across $THREADS workers"

    _t0=$(date +%s)
    "$DATAGEN" label corpus/chunk_00 -o 'data/calib_%02d.cnn' \
        -source "$LABEL_SOURCE" -nodes "$NODES" -threads "$THREADS" -max "$_max" -quiet
    _t1=$(date +%s)

    _bytes=$(cat data/calib_*.cnn | wc -c)
    _recs=$((_bytes / 32))
    _secs=$((_t1 - _t0))
    [ "$_secs" -gt 0 ] || _secs=1

    _per_box=$((_recs / _secs))
    # Integer division, so a too-short calibration can floor the per-core rate to
    # zero and turn the projection into a divide-by-zero. Say so instead.
    _per_core=$((_per_box / THREADS))
    printf '\ncalibration: %d records in %ds across %d workers\n' "$_recs" "$_secs" "$THREADS"
    printf '  %d records/sec/box\n' "$_per_box"
    if [ "$_per_core" -lt 1 ]; then
        printf '  under 1 record/sec/core - raise CALIBRATE_MAX and rerun\n'
    else
        printf '  %d records/sec/core\n' "$_per_core"
        printf '  186.5M positions => ~%d core-hours, ~%d box-hours at %d cores\n' \
            "$((186500000 / _per_core / 3600))" \
            "$((186500000 / _per_box / 3600))" "$THREADS"
    fi
    rm -f data/calib_*
}

case "$JOB" in
    label)     run_label ;;
    selfplay)  run_selfplay ;;
    calibrate) run_calibrate ;;
    *)         die "JOB must be label, selfplay or calibrate (got '$JOB')" ;;
esac

log "box $BOX finished"
