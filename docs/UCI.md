# UCI reference

The engine implements the Universal Chess Interface. Anything that speaks UCI —
Cute Chess, En Croissant, fastchess, cutechess-cli, Arena, Nibbler, OpenBench —
can drive it without adaptation.

Protocol layer: [`src/uci.c`](../src/uci.c).

---

## Standard commands

| Command | Notes |
|---|---|
| `uci` | identifies the engine and lists options, ends with `uciok` |
| `isready` | replies `readyok` — answered immediately, **even mid-search** |
| `ucinewgame` | stops any search and clears all inter-game state |
| `setoption name <name> [value <v>]` | see options below |
| `position startpos [moves ...]` | |
| `position fen <fen> [moves ...]` | |
| `go ...` | see below |
| `stop` | asks the search to finish now; `bestmove` follows |
| `ponderhit` | the pondered move was played; the clock is now ours |
| `quit` | stops the search and exits cleanly |

### `go` arguments

All standard arguments are parsed:

`wtime` `btime` `winc` `binc` `movestogo` `depth` `nodes` `movetime` `mate`
`infinite` `ponder` `searchmoves` `perft`

`go perft <depth>` prints a per-move node breakdown in the same format as
Stockfish's, so the two can be diffed directly.

---

## Options

| Option | Type | Default | Range | Notes |
|---|---|---|---|---|
| `Hash` | spin | 16 | 1–65536 | transposition table size in MB |
| `Threads` | spin | 1 | 1–1 | pinned to 1 until the search is parallel |
| `Ponder` | check | false | | the GUI drives pondering with `go ponder` |
| `Move Overhead` | spin | 10 | 0–5000 | ms reserved for GUI/network latency |
| `UCI_Chess960` | check | false | | switches castling move notation |
| `EvalFile` | string | `<internal>` | | **`EVAL=nnue` builds only** — load a net from disk |

`EvalFile` is advertised only by a build that has a network to replace. The net
is embedded at compile time, so the default is not a path; setting one swaps the
evaluation without a rebuild, which is how a candidate net is tried before it is
worth embedding. A load that fails leaves the previous net in place and says why
on an `info string` — a typo in a GUI config must not leave the engine with no
evaluation at all. `<internal>` is accepted and means "keep the embedded one".

`Hash` and `Threads` are mandatory for OpenBench compliance. `Threads` honestly
advertises `max 1` rather than accepting a value it cannot deliver — a test
client that believes an engine is multi-threaded will schedule games wrongly.

`MultiPV` is deliberately **not** advertised. The search reports one line, and
an option that is accepted and then ignored is the same trap as an inflated
`Threads` range: a GUI asking for four lines would silently get one, and an
analysis session would draw conclusions from a list that was never computed.
It returns when the search can actually produce it.

---

## Non-standard commands

Useful during development; GUIs ignore them.

| Command | Purpose |
|---|---|
| `bench [depth]` | deterministic node-count benchmark (OpenBench contract) |
| `perft <depth>` | per-move node breakdown for the current position |
| `perft suite [path] [maxdepth]` | run an EPD perft suite |
| `d` | print the board, FEN and Zobrist key |
| `eval` | print the classical evaluation's term-by-term breakdown |
| `nnue` | which net is loaded: architecture, quantisation, hash, source |
| `nnue eval` | the network's score for the current position, in centipawns |
| `nnue verify <file>` | check the exported test vectors; exits non-zero on any mismatch |

The three `nnue` commands exist only in an `EVAL=nnue` build. `eval` always
traces the *classical* model, in every build: it is the only evaluation with
terms to name, and it stays in the tree as the reference the network is measured
against.

Every one of these also works as a command-line argument, dispatched through the
exact same code path:

```sh
./stormbreaker bench
./stormbreaker perft suite tests/perft/standard.epd 4
```

---

## Current behaviour

Until move generation exists:

- `go` returns `bestmove 0000` (the UCI null move) — the engine is visibly not
  playing rather than illegally playing something wrong.
- `position ... moves ...` cannot apply the moves and emits one
  `info string` explaining why.
- `bench` reports `0 nodes`, honestly.

The handshake, options, threading and `stop` handling are complete and behave
correctly today.

---

## Manual testing

Pipe commands in directly:

```sh
printf 'uci\nisready\nd\nquit\n' | ./stormbreaker
```

On Windows PowerShell, prefer a file to avoid encoding surprises:

```powershell
cmd /c "type test.txt | .\stormbreaker.exe"
```

(The engine tolerates a UTF-8 BOM anyway, since PowerShell adds one when piping
strings.)
