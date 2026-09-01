# Releasing

Release instructions for engine binaries and the NNUE network. GitHub Actions
builds the binaries; the network is published separately.

## What a release ships

Fourteen binaries, from
[`.github/workflows/release.yml`](../.github/workflows/release.yml):

| | popcnt | avx2 | bmi2 | arm64 |
|---|---|---|---|---|
| **linux** | ✓ | ✓ | ✓ | |
| **windows** | ✓ | ✓ | ✓ | |
| **macos** | | | | ✓ |

each in both evaluations, named
`stormbreaker-<version>-<platform>-<arch>[-nnue]`. A `-nnue` binary has the net
compiled into it and needs no extra files.

`avx512` is not published because GitHub's runners cannot test it reliably and
it offers limited benefit outside Skylake-X. `native` is not published because
it is not portable.

## Network distribution

A net is 50 MB. The repository does not carry binaries, so `external/nets/` is
gitignored, and a clean checkout has no net at all. But `EVAL=nnue` *embeds*
one with `.incbin` at build time, which means a CI runner, an OpenBench worker
and anyone building from a fresh clone all need to obtain the exact net that
commit expects, before the compiler runs.

Networks use a separate release tagged by file hash:

```
net-1ee5325add50      net.nnue, net.nnue.vectors, net.nnue.sha256
```

The tag is `net-` plus the first twelve hex digits of the SHA-256, so a tag can
never come to mean a different file. The Makefile pins one:

```make
NET_TAG    ?= net-1ee5325add50
NET_SHA256 ?= 1ee5325add50950b3b8fb34c742988436664615895f02504dc5e2be9ea15c418
```

`make net-fetch` downloads the network, verifies its hash before installation,
and rejects a mismatch. The hash, rather than the filename, identifies the
network used by a build.

> These two lines say *which* net a build embeds.
> [EXPERIMENTS.md](EXPERIMENTS.md) says *why* that one — beside the SPRT that
> adopted it. Bump them in the same commit.

## Publishing a net

Publish each network once from a local machine:

```powershell
powershell -ExecutionPolicy Bypass -File tools\publish-net.ps1 -UpdateMakefile
# or: make net-publish
```

It hashes `external/nets/net.nnue`, derives the tag, creates the release,
uploads the three assets under fixed names, and rewrites the pin in the
Makefile. Re-running is safe — an existing tag is checked, not replaced.

It needs a token with `contents: write`, in `$env:GH_TOKEN` or
`$env:GITHUB_TOKEN` (or an authenticated `gh`, whose token it will borrow).
Use `-DryRun` to see what it would do without a token.

`net.nnue.vectors` is required. It contains 10,000 `(FEN, expected_int)` pairs
used by the release workflow to verify each architecture against the quantised
reference without a Python environment. The publishing script rejects a
network without these vectors.

## Cutting a release

The release workflow is manual and has no `push` trigger.

1. **Publish the net** if it changed, as above, and commit the new pin.
2. **Bump `ENGINE_VERSION`** in [`src/types.h`](../src/types.h) and commit. The
   workflow refuses to publish a tag that disagrees with it — when those two
   drift it is the UCI `id name` line that lies, and that is the string every
   GUI, tournament and rating list records.
3. **Tag and push it**: `git tag v0.2.0 && git push origin v0.2.0`.
4. **Actions → Release → Run workflow**, and type `v0.2.0`.

Leave the tag box **empty** for a dry run: the same fourteen builds and the
same gates, no release created. Worth doing the first time, and after any
change to the workflow.

A version with a suffix (`0.2.0-rc1`, `0.1.0-dev`) is published as a
prerelease, so it never takes the "Latest release" slot.

## What the workflow checks before publishing

The workflow runs these checks before publishing:

| Gate | Catches |
|---|---|
| Tag matches `ENGINE_VERSION` | A binary that reports a version nobody can trace |
| `make perft-all` at full depth | Illegal moves — forfeited games for every downloader |
| UCI handshake, and `id name` | A binary that no GUI can talk to |
| Bench prints `<nodes> nodes <nps> nps`, twice, identically | Invariants 1 and 2, per binary |
| Bench agrees across architectures | `-mbmi2` computing something *different*, not just faster |
| Embedded net is the pinned one | The wrong net, shipped quietly |
| `nnue verify` on 10,000 vectors, per architecture | A wrong SIMD path in `src/nnue.c` — plausible scores, lost Elo |
| `SHA256SUMS.txt` over every asset | Allows users to verify downloaded assets |

The cross-architecture bench check deserves its own note. An `ARCH` profile may
only change *how* an attack set is computed, never *what* it is. If two
architectures of one evaluation disagree on the node count, one of them is
searching a different tree, and the bench has stopped being a fingerprint —
which would quietly invalidate every measurement in
[EXPERIMENTS.md](EXPERIMENTS.md) taken with it.

## When it fails

**`could not fetch net net-…`** — the net release does not exist yet, or the
pin names one that was never uploaded. Run `make net-publish`.

**`net release … has no net.nnue.vectors asset`** — a net was uploaded without
its vectors, probably by hand. Re-run `tools\publish-net.ps1`, which uploads
all three.

**`tag v0.2.0 does not match ENGINE_VERSION 0.1.0-dev`** — bump
`src/types.h`, commit, and move the tag.

**`classical bench differs by architecture`** — a real bug, not a flake. Do not
retry it; find out which architecture is the odd one out and why.

## The classical binaries

Both evaluations ship. The network is much the stronger engine — E11 measured
+238 Elo at STC — and the release notes say so, but `EVAL=classical` remains
the Makefile default and the classical binaries remain published. They are the
only thing that runs where a 50 MB embedded net is unwelcome, they are what the
tuner fits, and keeping them buildable is what keeps the two comparable.
