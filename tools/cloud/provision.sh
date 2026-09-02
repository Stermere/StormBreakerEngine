#!/bin/sh
# Bring one box from a bare cloud image to a verified datagen binary.
#
# Idempotent: re-running against an already-provisioned box at the same
# commit/flags re-verifies and exits. That matters because fleet.ps1 may
# re-dispatch a box whose run died, and reprovisioning must not be a special
# case.
#
# Usage:  ./provision.sh [path/to/job.env]

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$DIR/common.sh"

load_config "${1:-$DIR/job.env}"
require REPO COMMIT ARCH EVAL

# git prompts for a username when an https remote refuses the anonymous fetch,
# and a prompt on a box nobody is sitting at is a hang or a baffling "could not
# read Username" a minute later. Fail on the spot instead.
export GIT_TERMINAL_PROMPT=0

# systemd hands a service a near-empty environment - no HOME - while ssh sets it.
# Under `set -u` that difference is fatal rather than merely different, so the
# same script that works over ssh dies one line in when run as a unit.
WORK="${WORK:-${HOME:-/root}/engine}"
STAMP="$WORK/.provisioned"
# Bumped when a change in here alters the artifact rather than just the route to
# it. The stamp carries it, so a box provisioned by an older recipe rebuilds
# instead of being trusted - the .git/info/exclude fix below changes what gets
# built (a -dirty stamp becomes a clean one) without COMMIT moving at all.
PROVISION_REV=2

# Deliberately without BOOK_SHA or SYZYGY: both are RUNTIME inputs that datagen
# opens by path, not build inputs the way the net is, and changing either must
# not cost the fleet a rebuild. fetch_book and fetch_syzygy below are what keep
# them in step instead.
BUILDID="$COMMIT $ARCH $EVAL ${NET_SHA:-} rev$PROVISION_REV"

# Where the tablebases land, and where job.env's -syzygy will point.
TB_DIR="$WORK/external/syzygy"

# The opening book `selfplay -book` draws start positions from, by hash for the
# same reason the net is: two books under one name are two datasets that nothing
# downstream can tell apart. Absent BOOK_SHA, games start from the initial
# position and this is a no-op.
#
# The hash is in the FILENAME, not just on the hub, because the shard manifest
# records the path datagen was handed. Landing every book as book.epd made that
# field constant and threw the identity away at the last step - which bites the
# moment one generation is extended with a second book, as gen-004 was.
fetch_book() {
    [ -n "${BOOK_SHA:-}" ] || return 0
    require HUB HUB_DIR HUB_PORT

    _dst="$WORK/external/books/book-$BOOK_SHA.epd"
    mkdir -p "$WORK/external/books"

    if [ -f "$_dst" ] && [ "$(sha256sum "$_dst" | cut -d' ' -f1)" = "$BOOK_SHA" ]; then
        log "book $BOOK_SHA already present"
        return 0
    fi

    hub_get "books/$BOOK_SHA.epd" "$_dst"
    _got=$(sha256sum "$_dst" | cut -d' ' -f1)
    [ "$_got" = "$BOOK_SHA" ] || die "book sha256 is $_got, expected $BOOK_SHA"
    log "book $BOOK_SHA verified"
}

#
# The Syzygy tablebases, fetched from a public mirror rather than from the hub.
#
# That is the opposite of the net and the book, deliberately. Those are local
# artifacts that exist nowhere else, so the hub is the only way a box can get
# them. The tablebases are canonical published data - there is exactly one
# correct 5-man Syzygy set and it has not changed since 2013 - so routing them
# through the hub would mean uploading a gigabyte from a home connection to
# duplicate a file that is already served, faster, from a machine next door to
# these boxes. SYZYGY_URL points somewhere else if you would rather it did.
#
# Verification is the point of doing this here rather than in run-box.sh. A box
# with half a tablebase set does not fail: it silently labels low-piece
# positions from the search instead, and those records are indistinguishable
# from everyone else's in the same shard directory. So the size the mirror
# publishes is checked per file, and then `datagen syzygy` probes known
# endgames with the binary this box just built.
#
fetch_syzygy() {
    [ -n "${SYZYGY:-}" ] || return 0

    _url="${SYZYGY_URL:-https://tablebase.lichess.ovh/tables/standard}"
    _dst="$TB_DIR/$SYZYGY"
    mkdir -p "$_dst"

    # WDL answers won/drawn/lost inside the search; DTZ is what converts at the
    # root under the fifty-move rule, and without it a labelling search of a
    # five-man position falls back to the evaluation - the exact bug the tables
    # are here to fix. Both halves, always.
    for _half in wdl dtz; do
        _index="$_url/$SYZYGY-$_half/"
        _listing=$(curl -fsS "$_index") || die "cannot read the tablebase index at $_index"

        # nginx autoindex: `<a href="NAME">NAME</a>  DATE  SIZE`. The name and
        # the size off the same line keep them in step.
        printf '%s\n' "$_listing" \
            | sed -n 's|.*href="\(K[A-Za-z]*vK[A-Za-z]*\.rtb[wz]\)".*[^0-9]\([0-9][0-9]*\)[[:space:]]*$|\1 \2|p' \
            > /tmp/tb-$_half.list
        _n=$(wc -l < /tmp/tb-$_half.list)
        [ "$_n" -gt 0 ] || die "no tables listed at $_index (mirror layout changed?)"

        _missing=0
        while read -r _name _size; do
            _f="$_dst/$_name"
            # Size, not mere existence: an interrupted download leaves a short
            # file that looks present forever otherwise.
            if [ -f "$_f" ] && [ "$(wc -c < "$_f")" = "$_size" ]; then
                continue
            fi
            _missing=$((_missing + 1))
            # --fail so an HTML error page never lands as a table; -o to a
            # .part first so an interrupted transfer cannot be mistaken for a
            # complete one on the next run.
            curl -fsS --retry 3 --retry-delay 2 -o "$_f.part" "$_index$_name" \
                || die "cannot fetch $_name from $_index"
            _got=$(wc -c < "$_f.part")
            [ "$_got" = "$_size" ] || die "$_name is $_got bytes, the index says $_size"
            mv "$_f.part" "$_f"
        done < /tmp/tb-$_half.list

        rm -f /tmp/tb-$_half.list
        if [ "$_missing" -eq 0 ]; then
            log "syzygy $SYZYGY-$_half: $_n tables already present"
        else
            log "syzygy $SYZYGY-$_half: fetched $_missing of $_n tables"
        fi
    done

    # The functional gate. Wrong answers, a set that does not cover five men,
    # and a position the prober declines to probe at all are each a non-zero
    # exit here rather than a shard full of quietly search-only labels.
    log "verifying the tablebases with this box's own datagen"
    "$WORK/datagen" syzygy "$_dst" || die "the tablebases at $_dst did not pass the probe suite"
}

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$BUILDID" ] && [ -x "$WORK/datagen" ]; then
    log "already provisioned for [$BUILDID]"
    # Before the early exit, not after it: a box provisioned for this build by an
    # earlier generation still needs whatever book and tablebases THIS one names,
    # and skipping that is a run that dies on its first batch.
    fetch_book
    fetch_syzygy
    sha256sum "$WORK/datagen"
    exit 0
fi

log "installing build dependencies"
export DEBIAN_FRONTEND=noninteractive
# needrestart ships enabled on Ubuntu 24.04 and asks which services to restart,
# which on an unattended box is a hang rather than a question.
export NEEDRESTART_MODE=a
# A fresh cloud image runs apt-daily and unattended-upgrades at boot, and they
# hold the dpkg lock for minutes. Without a lock timeout apt gives up or waits
# without saying why - this is the other half of what looked like a random hang.
APT_OPTS="-o DPkg::Lock::Timeout=600 -o Acquire::Retries=3"
# shellcheck disable=SC2086
apt-get $APT_OPTS update -qq
# shellcheck disable=SC2086
apt-get $APT_OPTS install -y -qq build-essential git rsync zstd curl >/dev/null

# A private repository is cloned over ssh with the key fleet.ps1 already put on
# this box, registered as a read-only deploy key. That keeps the credential
# situation as it is - one purpose-built key, no token in job.env and none on
# disk here - rather than adding a second kind of secret to the fleet.
case "$REPO" in
git@*|ssh://*)
    _host=$(printf '%s' "$REPO" | sed -e 's|^ssh://||' -e 's|^[^@]*@||' -e 's|[:/].*$||')
    mkdir -p "$HOME/.ssh"
    chmod 700 "$HOME/.ssh"
    if ! ssh-keygen -F "$_host" >/dev/null 2>&1; then
        if [ "$_host" = github.com ]; then
            # From api.github.com over TLS rather than ssh-keyscan, which trusts
            # whatever answers - and no one is going to compare a fingerprint by
            # hand on a box that exists for an afternoon.
            _keys=$(curl -fsS https://api.github.com/meta 2>/dev/null \
                | grep -oE '"ssh-(ed25519|rsa) [A-Za-z0-9+/=]+"' | tr -d '"' || true)
            [ -n "$_keys" ] || die "could not fetch github.com host keys from api.github.com"
            printf '%s\n' "$_keys" | sed "s|^|$_host |" >> "$HOME/.ssh/known_hosts"
        else
            ssh-keyscan "$_host" >> "$HOME/.ssh/known_hosts" 2>/dev/null \
                || die "no host key for $_host"
        fi
        log "host key for $_host recorded"
    fi
    ;;
esac

if [ ! -d "$WORK/.git" ]; then
    log "cloning $REPO"
    # Named, because git's own answer here is "could not read Username", which
    # reads like a missing tool rather than a private repository.
    git clone --quiet "$REPO" "$WORK" || die \
        "cannot clone $REPO." \
        "If it is private, https has no credentials on this box: set REPO to the ssh form" \
        "(git@github.com:owner/repo.git) and add the public half of SSH_KEY_FILE to the" \
        "repository as a read-only deploy key."
fi

cd "$WORK"

# `make datagen` writes ./datagen into the work tree. If this clone does not
# ignore it, the tree is dirty by the time `make datagen-test` re-reads git,
# which re-stamps DATAGEN_COMMIT as <commit>-dirty, rebuilds, and hands the
# generation a binary whose manifests cannot be attributed to a commit. Excluded
# in the clone rather than trusting .gitignore, so this also holds for commits
# pushed before that ignore rule existed.
grep -qx datagen .git/info/exclude 2>/dev/null || printf '%s\n' datagen >> .git/info/exclude

git fetch --quiet origin || die "cannot fetch from $REPO"
git checkout --quiet --detach "$COMMIT" \
    || die "$COMMIT does not exist on the remote (push it before running the fleet)"

# The Makefile stamps `-dirty` into every shard manifest for a dirty tree, and a
# dataset that cannot be attributed to an exact build cannot be compared to the
# next generation's. Refuse rather than silently produce unattributable data.
[ -z "$(git status --porcelain)" ] || die "working tree at $COMMIT is dirty; refusing to build"
log "building $(git rev-parse --short HEAD), ARCH=$ARCH EVAL=$EVAL"

if [ "$EVAL" = nnue ]; then
    require NET_SHA
    mkdir -p external/nets
    # By hash, never by name: the net is embedded with .incbin at compile time,
    # so a box that fetches a different file produces labels that look fine and
    # are not comparable to anything.
    hub_get "nets/$NET_SHA.nnue" external/nets/net.nnue
    got=$(sha256sum external/nets/net.nnue | cut -d' ' -f1)
    [ "$got" = "$NET_SHA" ] || die "net sha256 is $got, expected $NET_SHA"
    log "net $NET_SHA verified"
fi

make datagen "ARCH=$ARCH" "EVAL=$EVAL"

# The pre-build check above cannot see this: the build is what dirties the tree.
# A second look costs nothing and catches any future build output that lands
# somewhere git notices.
if [ -n "$(git status --porcelain)" ]; then
    git status --porcelain >&2
    die "the build dirtied the work tree (above); its shards would be stamped -dirty"
fi

# The Task 1 acceptance gate: records round-trip to identical bytes and a label
# re-searched from scratch reproduces its score. A box that fails this produces
# data that is worse than useless, because nothing downstream would notice.
log "running the datagen acceptance gate"
make datagen-test "ARCH=$ARCH"

# After the build, and after the dirty check above: external/ is gitignored, but
# the order means a book or tablebase download can never be what a "the build
# dirtied the tree" failure is actually reporting. fetch_syzygy also needs the
# datagen binary that the build above produced, to verify what it fetched.
fetch_book
fetch_syzygy

printf '%s' "$BUILDID" > "$STAMP"
log "provisioned"
sha256sum "$WORK/datagen"
