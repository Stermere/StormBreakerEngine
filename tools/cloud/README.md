# Distributed datagen

These scripts distribute corpus labelling across a Hetzner fleet. A job that
takes about 12 days on the reference 14-core desktop can complete in a few
hours, depending on fleet size.

The engine needs no changes to support any of this. `datagen label` is
deterministic and already partitions its input by line, and `.cnn`/`.pol` are
headerless fixed-size records that concatenate with `cat` — see the format
contract at the top of `tools/datagen.c`.

## The pieces

| File | Runs on | Does |
|---|---|---|
| `job.env.template` | — | Committed defaults. Copy to `job.env`. |
| `job.env` | — | **Gitignored.** Your values. A generation is one pass of it. |
| `provision.sh` | box | Clone at `$COMMIT`, build, run the acceptance gate. |
| `prepare-corpus.sh` | box | Split the corpus into `$CHUNKS`. **Once, ever.** |
| `run-box.sh` | box | This box's share: label, selfplay, or calibrate. |
| `fleet.ps1` | local | Create, provision, launch, watch, destroy. |
| `aggregate.ps1` | local | Pull a generation, check it, shuffle it into one file. |

Work is addressed by **chunk** (label) or **batch** (selfplay), never by box,
and every unit drops a done-marker on the hub when it completes. So a rerun
skips finished units, and a unit from a box that died can be re-dispatched
anywhere without renaming a thing.

## Configuration

`job.env.template` is committed and holds working defaults; `job.env` is
gitignored and holds yours. Start with:

```powershell
Copy-Item tools\cloud\job.env.template tools\cloud\job.env
```

Two values are required; the remaining settings have defaults.

| What | Where | Value |
|---|---|---|
| `HUB` | `job.env` | Your Storage Box as `user@host`, e.g. `u123456@u123456.your-storagebox.de`. Ships as `REPLACE_ME` and the scripts refuse to run until you change it. |
| `HCLOUD_TOKEN` | environment | `$env:HCLOUD_TOKEN = '...'` — a Cloud API token. Deliberately *not* a line in `job.env`: it is a credential, and it would end up in your shell history and your backups. |

`SSH_KEY_FILE` is already pointed at `%USERPROFILE%/.ssh/hetzner_datagen`, and
`SSH_KEY=datagen` is the name the key must be registered under in the Hetzner
console — match those two and there is nothing to change.

Worth knowing about the rest: `GEN` you bump every generation; `BOXES` comes
out of `fleet.ps1 calibrate`; `NODES` and `EVAL` define what the dataset *is*
and should change only deliberately. Use forward slashes in any path — sh
strips backslashes on assignment, and the loader now rejects them rather than
silently mangling the value.

## One-time setup

**1. Hetzner Cloud** — sign up at <https://www.hetzner.com/cloud>, then at
<https://console.hetzner.cloud> create a project. In it:

- *Security → SSH Keys*: upload `~/.ssh/hetzner_datagen.pub` and name it
  `datagen`, matching `SSH_KEY`.
- *Security → API Tokens*: create a read/write token, then locally
  `$env:HCLOUD_TOKEN = '...'`.


**2. Storage Box** — order a BX11 (1 TB, ~€3.81/mo) from the storage section of
the console. This is the hub. Put its user and host in `HUB`; the port is 23,
not 22, which is a Storage Box quirk rather than a typo.

**3. `hcloud` CLI** — `winget install HetznerCloud.CLI`.

**3a. If the engine repo is private** — the boxes clone it with the same key,
so give that key read access to the repo: *GitHub → the repo → Settings → Deploy
keys → Add deploy key*, paste `~/.ssh/hetzner_datagen.pub`, leave *Allow write
access* unchecked. Then set `REPO` to the ssh form:

```
REPO=git@github.com:Stermere/StormBreakerEngine.git
```

An https `REPO` only works for a public repo. A box has no GitHub credentials
and no one to type any, so the clone fails with `could not read Username`.

**4. Upload and split the corpus, once.** This is the only large thing that
ever leaves your machine, and only on the first generation:

```powershell
# ~11.6 GB -> ~1.8 GB. Ten minutes or so; it is the compression, not the upload.
zstd -12 -T0 external\training\human.epd -o human.epd.zst

# creates datagen/{corpus,nets} on the Storage Box, then uploads.
# scp cannot create remote directories, which is why this is a command and not
# a bare scp - a fresh Storage Box is empty.
.\tools\cloud\fleet.ps1 push-corpus

# rents one cheap box (PREPARE_TYPE, default cx32), splits the corpus into
# CHUNKS pieces, verifies the split is lossless, uploads them, deletes the box.
.\tools\cloud\fleet.ps1 prepare-corpus
```

`prepare-corpus` needs its own box because the intermediates are ~25 GB and
because `split` wants Unix line semantics. It skips provisioning entirely — the
split needs `zstd` and `split`, not the engine — so it takes a few minutes, not
the ten that a build would add. Pass `-Keep` to leave the box up if it fails and
you want to poke at it.

After this the hub holds `corpus/chunk_00.zst` … `chunk_07.zst`, and no
generation ever needs to upload anything again.

## Running a generation

```powershell
# 1. publish the net, if EVAL=nnue, and the book, if JOB=selfplay. Both are
#    idempotent, and `up` and `calibrate` do them for you - these exist for
#    when you want it done early.
.\tools\cloud\fleet.ps1 push-net
.\tools\cloud\fleet.ps1 push-book

# 2. size the fleet from a measurement, not from an estimate
.\tools\cloud\fleet.ps1 calibrate

# 3. edit job.env: GEN, and BOXES from what calibrate reported
.\tools\cloud\fleet.ps1 up
.\tools\cloud\fleet.ps1 status
.\tools\cloud\fleet.ps1 logs 2

# 4. when every unit has a done-marker
.\tools\cloud\aggregate.ps1 -Parallel 8
.\tools\cloud\fleet.ps1 down     # billing is hourly - do not forget this
```

`aggregate.ps1` leaves a shuffled `external/data/$GEN.cnn` ready to train on.

Cost is roughly flat in box count: Hetzner bills by the hour, so eight boxes
finish in half the time for the same money. Box count buys wall time, not
throughput per euro.

That last point is exactly why `up` provisions boxes **concurrently**. Every box
starts billing the moment it is created, and provisioning is the long pole —
apt, a clone, a build, the acceptance gate, and with `SYZYGY` set a ~939 MB
download. One at a time, box *N* sat idle and paid-for through everyone else's
build. `PROVISION_PARALLEL` (default 8) caps how many run at once, because a
whole fleet pulling from the Ubuntu and tablebase mirrors on the same second is
both impolite and no faster than a queue.

Waiting for a box to answer ssh and copying the scripts over stays serial, and
the provisioning it kicks off overlaps that: box 0 is already building while
box 1 is still being waited for. Hetzner addresses are sometimes unroutable for
minutes after a create, and this is what makes that one box's wait cheap rather
than blocking.

Each box's build goes to its own log in `%TEMP%\fleet-<gen>-b<N>.provision.log`
rather than to the console, because fourteen interleaved builds are unreadable.
On failure the tail is printed and the path named. A box that still fails is
retried once, serially, and then reported and skipped — its units keep their
done-markers on the hub, so a later `up` claims them.

## Things that fail silently

Each of these produces data that looks fine and is wrong. They are the reason
the scripts are shaped the way they are.

- **Self-play seeds must differ per batch.** `-seed` defaults to `1` and is
  never drawn from the clock, so two batches at the same seed emit
  byte-identical shards — and nothing downstream notices, because dedup is
  per-shard, `shuffle` does not dedup, and the trainer does not dedup.
  `SELFPLAY_SEED_STRIDE` is large and prime because worker *k* derives
  `seed + k*0x9E3779B97F4A7C15`, so adjacent integer seeds would put two
  batches' workers a single draw apart.
- **`THREADS` must not change between attempts on the same chunk.** Workers
  split their input by line, so a different count reads different lines, and a
  `-resume` after a change appends records for lines it never labelled.
- **`NODES` must match across every shard in a dataset.** A self-play position
  and a human one mean the same thing only if they were labelled the same way.
  Mixing node budgets is the reason the previous run was discarded.
- **One missing `.pol` costs you every policy target.** `datagen shuffle` drops
  the sidecar for the *entire* dataset if any single input lacks one, rather
  than failing. `aggregate.ps1` checks presence and length before shuffling and
  treats a mismatch as fatal.
- **`EVAL=nnue` makes the net a build input.** It is embedded with `.incbin` at
  compile time, so two boxes that fetched different files produce labels that
  are individually plausible and mutually incomparable. `NET_SHA` pins it by
  hash and `provision.sh` refuses to continue if the fetched net disagrees.
- **A dirty tree poisons attribution.** The Makefile stamps `-dirty` into every
  manifest, and a dataset that cannot be tied to an exact build cannot be
  compared to the next generation's. `provision.sh` refuses to build one.
- **Half a tablebase set is worse than none.** A box missing tables does not
  error — it labels low-piece positions from the search instead, and those
  records are indistinguishable from everyone else's in the same shard
  directory. `provision.sh` size-checks every file against the mirror's index
  and then runs `datagen syzygy`, which probes known endgames with that box's
  own binary. See the tablebase section below.
- **`SYZYGY` must match across every shard in a dataset**, for exactly the
  reason `NODES` must: a label made with tablebases and one made without are
  two different label definitions. Nothing enforces it — the manifest records
  `syzygy_path` and `syzygy_men` for a human, and `verify -relabel` with a
  mismatched setting fails on every low-piece record, loudly, which is the
  warning that matters.

## Dedup gets weaker as the fleet grows

Dedup is per-worker, per-shard; there is no cross-shard pass anywhere in the
pipeline. Going from 14 shards to ~400 means each dedup table sees far fewer
positions. The corpus is largely pre-deduped by `tuner extract` already —
retention was 97.6% at 14-way — so expect roughly 2% additional duplicates.

That is tolerable, and cheaper than a global dedup pass nobody has needed yet.
Watch the retention `datagen stats` reports against that 97.6% baseline; a
materially worse number is the trigger to add one.

## The opening book (JOB=selfplay)

A self-play generation is only as broad as the positions its games start from,
and everything after the opening is deterministic — fixed nodes, one thread,
no randomised move choice. So `BOOK_SHA` and `SELFPLAY_OPENING` are the two
settings that decide what a generation covers.

`BOOK_SHA` is the sha256 of an EPD on the hub under `books/`, fetched and
re-verified by `provision.sh` exactly the way the net is, and for the same
reason: a book is the sampler for the whole generation, and two books under one
filename are two datasets nothing downstream can tell apart. Leave it empty and
games start from the initial position, as every generation before `gen-005`
did.

```powershell
.\tools\cloud\fleet.ps1 push-book     # idempotent; `up` and `calibrate` also do it
```

It finds the local file by hashing every `.epd` under `external\books\`, so
`BOOK_SHA` alone drives it; `-Path` overrides. See docs/NNUE.md for how a book
gets extracted out of the CCRL archive, why `SELFPLAY_OPENING` wants to be 2 and
not 8, and why it wants to be the RANGE `2-3` and not the number 2: a book cut
at one ply is one side to move on every line of it, so a fixed count starts
every game in the generation on the same side.

Unlike the net, the book is **not** part of the provisioning stamp — it is a
runtime input, not a build input, so changing it does not cost the fleet a
rebuild. `provision.sh` fetches it before its own early exit, so a box already
provisioned for this commit still picks up a book the config gained later.

## The tablebases (`SYZYGY`)

`SYZYGY=3-4-5` makes every box fetch the 3-4-5-man Syzygy set (~939 MB) during
provisioning and hands `-syzygy` to every `datagen` call it runs afterwards.
Empty means no tablebases, which is what every generation up to `gen-004` did.
Why this matters to data quality — endgames that the search scores as winning
and are dead drawn — is E23 in [docs/EXPERIMENTS.md](../../docs/EXPERIMENTS.md).

**It does not go through the hub, and it is not pinned by hash.** That is the
opposite of the net and the book, deliberately. Those are local artifacts that
exist nowhere else and whose *content* decides what a dataset is, so the hub is
both the only way a box can get them and the reason they need a hash. Syzygy is
canonical published data — there is one correct 5-man set and it has not
changed since 2013 — so what a hash would establish is not in question, and
routing it through the hub would mean a gigabyte of home upload to duplicate a
file already served, faster, from a machine near these boxes. `SYZYGY_URL`
points somewhere else if you would rather it did.

What replaces the hash is a stronger check, because the failure being guarded
against is worse than the wrong file. **A box with half a tablebase set does
not fail.** It labels low-piece positions from the search instead, and those
records land in the same shard directory as everybody else's, correct-looking
and wrong. So:

- every file is checked against the size the mirror's index publishes, and a
  short one is re-fetched rather than trusted;
- then `datagen syzygy <dir>` probes sixteen known endgames — each as given and
  mirrored — with the binary that box just built. Wrong answers, a set that
  does not reach five men, and a position the prober declines to probe at all
  are each a provisioning failure.

You can run that gate anywhere: `./datagen syzygy external/syzygy/3-4-5`, or
`make syzygy-test` against the engine binary.

Both halves are always fetched. WDL answers won/drawn/lost inside the search;
DTZ is what converts at the root under the fifty-move rule, and without it a
labelling search of a five-man position falls back to the evaluation — the
exact bug the tables are there to fix.

Like the book and unlike the net, `SYZYGY` is **not** part of the provisioning
stamp, and `provision.sh` fetches before its early exit — so a box already
provisioned for this commit picks up tablebases the config gained later without
a rebuild.

`run-box.sh` passes `-syzygy` to `label`, `selfplay`, `calibrate` **and** to the
`verify -relabel` it runs before uploading. That last one is not decoration: a
label made with tablebases does not reproduce without them, so a verify that
omitted the flag would fail on every low-piece record of perfectly good data.

## Bootstrapping onto the net

`docs/NNUE.md` wants generation *n+1* labelled by searches that used the net
from generation *n*. When you get there: upload the net to `$HUB_DIR/nets/`
named by its sha256, then set `EVAL=nnue` and `NET_SHA` in `job.env`.
`provision.sh` already fetches and verifies it. Nothing else changes.
