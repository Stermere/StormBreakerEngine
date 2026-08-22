#!/bin/sh
# Shared helpers for the datagen fleet scripts. Mirrors tools/common.ps1 in
# intent: config loading and the handful of hub operations, in one place so the
# box scripts stay readable.
#
# POSIX sh on purpose - this runs on whatever a fresh cloud image ships with.

set -eu

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

log() {
    printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"
}

# A backslash is rejected along with the shell metacharacters: sh strips it on an
# unquoted assignment, so a Windows path would arrive with its separators gone
# and no error. Use forward slashes - ssh on Windows accepts them.
# job.env is restricted to bare KEY=value lines precisely so that sourcing it is
# safe. Validate that before trusting it, rather than after: a stray quote or a
# command substitution in a config file would otherwise execute here with the
# script's privileges.
load_config() {
    _cfg="${1:?load_config needs a path}"
    # POSIX `.` searches PATH when its operand has no slash, and PATH does not
    # include the current directory: `load_config job.env` looked for job.env in
    # /usr/bin and friends and died with "job.env: not found" one line after
    # [ -f ] had confirmed the file was right there. Callers that pass an
    # absolute path never saw it, which is why provision.sh worked and
    # run-box.sh - launched as `cd /root/cloud && ./run-box.sh 0 job.env` - did
    # not.
    case "$_cfg" in
        */*) ;;
        *) _cfg="./$_cfg" ;;
    esac
    [ -f "$_cfg" ] || die "no config at $_cfg"

    if grep -nEv '^[[:space:]]*(#.*)?$|^[A-Z_][A-Z0-9_]*=[^\ "'"'"'`$;&|<>()]*$' "$_cfg" >&2; then
        die "$_cfg has lines that are not bare KEY=value (shown above)"
    fi

    # Anchored past any '#' so the template's own explanation of REPLACE_ME does
    # not report itself as an unedited value.
    if grep -qE '^[^#]*REPLACE_ME' "$_cfg"; then
        grep -nE '^[^#]*REPLACE_ME' "$_cfg" >&2
        die "$_cfg still has REPLACE_ME placeholders (shown above)"
    fi

    set -a
    # shellcheck disable=SC1090
    . "$_cfg"
    set +a
}

require() {
    for _v in "$@"; do
        eval "_val=\${$_v:-}"
        [ -n "$_val" ] || die "$_v is not set in job.env"
    done
}

# THREADS=auto means "every core on this box". Resolved once, here, so that the
# value that reaches datagen is the value that gets logged and recorded.
resolve_threads() {
    if [ "${THREADS:-auto}" = auto ]; then
        THREADS=$(nproc)
    fi
    export THREADS
}

hub_base() {
    printf '%s:%s' "$HUB" "$HUB_DIR"
}

hub_ssh() {
    ssh -p "$HUB_PORT" -o BatchMode=yes -o StrictHostKeyChecking=accept-new "$HUB" "$@"
}

hub_mkdir() {
    # Storage Boxes run a restricted shell that supports mkdir; if that ever
    # changes, rsync --mkpath below still creates the tree.
    hub_ssh mkdir -p "$HUB_DIR/$1" 2>/dev/null || true
}

hub_get() {
    _remote="$1"
    _local="$2"
    rsync -a --partial --inplace -e "ssh -p $HUB_PORT -o StrictHostKeyChecking=accept-new" \
        "$(hub_base)/$_remote" "$_local"
}

hub_put() {
    _local="$1"
    _remote="$2"
    hub_mkdir "$(dirname "$_remote")"
    rsync -a --partial --inplace --mkpath \
        -e "ssh -p $HUB_PORT -o StrictHostKeyChecking=accept-new" \
        "$_local" "$(hub_base)/$_remote"
}

# Non-zero when the path is absent. This gates the done-markers that make a
# rerun skip finished work, so "absent" and "could not tell" must not collapse
# into the same answer: rsync exits 23 for a missing file and something else
# entirely for a connection failure, and treating the latter as "not done" would
# silently redo hours of labelling. Anything that is not a clean yes or no is
# fatal instead.
hub_exists() {
    # `|| _rc=$?` rather than testing $? after an if: a failed if-with-no-else
    # exits 0, which would swallow the code this function exists to read.
    _rc=0
    rsync --list-only -e "ssh -p $HUB_PORT -o StrictHostKeyChecking=accept-new" \
        "$(hub_base)/$1" >/dev/null 2>&1 || _rc=$?
    if [ "$_rc" -eq 0 ]; then
        return 0
    fi
    if [ "$_rc" -ne 23 ]; then
        die "cannot reach the hub to check $1 (rsync exit $_rc)"
    fi
    return 1
}

# Work is assigned by index, round-robin. Both the label chunks and the selfplay
# batches use this, so a fleet resize redistributes without renaming anything.
mine() {
    _index="$1"
    [ "$((_index % BOXES))" -eq "$BOX" ]
}
