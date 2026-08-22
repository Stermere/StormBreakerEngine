# Distributed datagen

Labelling the human corpus takes ~12 days on a 14-core desktop. This runs the
same job across a Hetzner fleet in a few hours, and is built to be rerun once
per training generation rather than driven by hand.

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

## What you actually have to set

`job.env.template` is committed and holds working defaults; `job.env` is
gitignored and holds yours. Start with:

```powershell
Copy-Item tools\cloud\job.env.template tools\cloud\job.env
```

Then exactly **two** things need your input. Everything else in the file is a
default that works.

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

**Request a quota increase now.** New projects cap out well below `4 × ccx63`
(192 vCPU), and the support ticket is the long pole in this whole plan.

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
# 1. size the fleet from a measurement, not from an estimate
.\tools\cloud\fleet.ps1 calibrate

# 2. edit job.env: GEN, and BOXES from what calibrate reported
.\tools\cloud\fleet.ps1 up
.\tools\cloud\fleet.ps1 status
.\tools\cloud\fleet.ps1 logs 2

# 3. when every unit has a done-marker
.\tools\cloud\aggregate.ps1
.\tools\cloud\fleet.ps1 down     # billing is hourly - do not forget this
```

`aggregate.ps1` leaves a shuffled `external/data/$GEN.cnn` ready to train on.

Cost is roughly flat in box count: Hetzner bills by the hour, so eight boxes
finish in half the time for the same money. Box count buys wall time, not
throughput per euro.

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

## Dedup gets weaker as the fleet grows

Dedup is per-worker, per-shard; there is no cross-shard pass anywhere in the
pipeline. Going from 14 shards to ~400 means each dedup table sees far fewer
positions. The corpus is largely pre-deduped by `tuner extract` already —
retention was 97.6% at 14-way — so expect roughly 2% additional duplicates.

That is tolerable, and cheaper than a global dedup pass nobody has needed yet.
Watch the retention `datagen stats` reports against that 97.6% baseline; a
materially worse number is the trigger to add one.

## Bootstrapping onto the net

`docs/NNUE.md` wants generation *n+1* labelled by searches that used the net
from generation *n*. When you get there: upload the net to `$HUB_DIR/nets/`
named by its sha256, then set `EVAL=nnue` and `NET_SHA` in `job.env`.
`provision.sh` already fetches and verifies it. Nothing else changes.
