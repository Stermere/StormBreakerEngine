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
| `UCI_Chess960` | check | false | | Chess960 castling notation (see below) |
| `SyzygyPath` | string | `<empty>` | | directory of Syzygy tablebases; empty disables probing |
| `EvalFile` | string | `<internal>` | | **`EVAL=nnue` builds only** — load a net from disk |

`UCI_Chess960` selects how castling is **spelled**, not which rules apply. The
rules come from the position: `board_set_fen` derives the castling geometry
from the diagram, so a Chess960 board is played correctly whether or not the
option was ever sent. What the option changes is the notation —

| | castling move | FEN castling field |
|---|---|---|
| off | `e1g1` — the king's destination | `KQkq` |
| on | `e1h1` — king captures own rook | `HAha` — the rook's file |

— and the king-takes-rook form is the only unambiguous one on a Chess960 board,
where a king on b1 castling long and a king on b1 stepping to c1 are both
`b1c1`. Because of that the engine also turns the option **on by itself** when
it is given a FEN only Chess960 can describe (a king off the e-file, a castling
rook off the a- or h-file, or rights spelled with file letters), and says so on
an `info string`. It never turns itself off: guessing wrong in that direction
would hand the GUI a move meaning two things, while guessing wrong the other
way costs a GUI that already speaks Chess960 a spelling it also understands.

Both FEN spellings are accepted on input regardless of the option. Shredder
form (`AHah`) is required when one side has two rooks on the same side of its
king — `KQkq` can only name the outermost rook, so it cannot describe such a
position at all.

`SyzygyPath` takes a directory (several, `;`-separated on Windows and `:`
elsewhere) and loads what it finds; `make syzygy-fetch` downloads the 3-4-5-man
set. Setting it reports the piece count found on an `info string`, and a path
holding no usable tables says so rather than failing silently — probing that
never fires looks exactly like probing that works.

With tables loaded the search probes WDL at interior nodes and DTZ at the root,
and `info` lines carry a `tbhits` field. **Leave it empty for `bench`**: node
counts must not depend on which files a machine has, so the benchmark contract
only holds with probing off. `<empty>` clears it again.

`EvalFile` is advertised only by a build that has a network to replace. The net
is embedded at compile time, so the default is not a path; setting one swaps the
evaluation without a rebuild, which is how a candidate net is tried before it is
worth embedding. A load that fails leaves the previous net in place and says why
on an `info string` — a typo in a GUI config must not leave the engine with no
evaluation at all. `<internal>` is accepted and means "keep the embedded one".

`Hash` and `Threads` are required for OpenBench compliance. `Threads` advertises
`max 1` because the search is currently single-threaded.

`MultiPV` is not advertised because the search currently reports one line.

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
| `syzygy` | how many men the loaded tablebases cover, if any |
| `syzygy verify <dir>` | probe known endgames against the tables in `dir`; exits non-zero on any wrong answer (`make syzygy-test`) |
| `syzygy manifest <dir> <file>` | re-derive every material configuration's probe checksum and compare against a sealed manifest; names the endgame that differs |

The three `nnue` commands exist only in an `EVAL=nnue` build. `eval` always
traces the *classical* model, in every build: it is the only evaluation with
terms to name, and it stays in the tree as the reference the network is measured
against.

Each command also works as a command-line argument through the same code path:

```sh
./stormbreaker bench
./stormbreaker perft suite tests/perft/standard.epd 4
```

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
