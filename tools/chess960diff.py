#!/usr/bin/env python3
"""Differential Chess960 move generation testing against an independent engine.

Chess960 castling is the only part of this engine's rules that cannot be
checked against the published perft numbers everyone quotes, because those
numbers only cover the standard array. So it is checked the way the Syzygy
prober was (docs/EXPERIMENTS.md E24): against a second implementation, over
positions neither implementation was written with in mind.

Two modes:

    campaign    Walk random Chess960 games, comparing `perft divide` with the
                oracle at every position. Divides, not totals - a mismatch
                names the move that is wrong, which is most of the debugging.

    seal-startpos
                Emit an .epd suite covering all 960 start positions, counts
                taken from the oracle.

    reseal      Rewrite an existing suite's counts from the oracle in place,
                keeping every comment - so the campaign's verdict keeps being
                re-checked by `make perft` long after this script last ran.

The oracle must speak UCI, support UCI_Chess960 and answer `go perft`.
Stockfish does; so does almost every engine strong enough to be worth
comparing against.

Usage:
    python tools/chess960diff.py campaign [-oracle stockfish] [-games 200]
    python tools/chess960diff.py seal-startpos <out.epd> [-depth 4]
    python tools/chess960diff.py reseal <suite.epd> [out.epd]
"""

import argparse
import io
import queue
import random
import re
import shlex
import subprocess
import sys
import threading
import time

# --------------------------------------------------------------- SP numbering

# The ten arrangements of K, R, N on five squares with the king between the
# rooks. This is an INDEPENDENT transcription of the same table board.c holds:
# if the two ever disagree, `campaign` compares positions the oracle never saw
# and `seal-startpos` writes a suite the engine cannot reproduce. That is the
# point of writing it twice.
KRN = ["NNRKR", "NRNKR", "NRKNR", "NRKRN", "RNNKR",
       "RNKNR", "RNKRN", "RKNNR", "RKNRN", "RKRNN"]


def sp_backrank(idx):
    """The back rank of Scharnagl start position `idx`, as uppercase letters."""
    if not 0 <= idx < 960:
        raise ValueError("SP index out of range: %d" % idx)

    rank = [" "] * 8
    n = idx
    rank[2 * (n % 4) + 1] = "B"   # light-squared bishop: b, d, f, h
    n //= 4
    rank[2 * (n % 4)] = "B"       # dark-squared bishop:  a, c, e, g
    n //= 4

    free = [f for f in range(8) if rank[f] == " "]
    rank[free[n % 6]] = "Q"
    n //= 6

    for f, piece in zip([f for f in range(8) if rank[f] == " "], KRN[n]):
        rank[f] = piece
    return "".join(rank)


def sp_fen(idx):
    """Start position `idx` as a Shredder-FEN, the spelling board.c emits."""
    back = sp_backrank(idx)
    a_side = chr(ord("A") + back.index("R"))
    h_side = chr(ord("A") + back.rindex("R"))
    return "%s/pppppppp/8/8/8/8/PPPPPPPP/%s w %s%s%s%s - 0 1" % (
        back.lower(), back, h_side, a_side, h_side.lower(), a_side.lower())


# ------------------------------------------------------------------- engines

class Engine:
    """A UCI engine driven synchronously, one perft at a time.

    Output is drained by ONE long-lived thread into a queue, rather than read
    on demand. Reading on demand looks simpler and is wrong on both counts
    that matter here: a per-call reader buffers ahead and swallows lines meant
    for the next call, and an engine that dies mid-handshake hangs the script
    forever - which is indistinguishable, from the outside, from a
    differential test that is merely being slow.
    """

    def __init__(self, command, chess960=True):
        self.name = command
        # shell=False and an explicit argv: shell=True would route through
        # cmd.exe on Windows, which rejects the "./engine" spelling that every
        # other part of this repo uses.
        self.proc = subprocess.Popen(
            shlex.split(command, posix=False) if isinstance(command, str) else command,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            universal_newlines=True, bufsize=1)

        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._drain, daemon=True)
        self.reader.start()

        self.send("uci")
        self.read_until("uciok")
        if chess960:
            self.send("setoption name UCI_Chess960 value true")
        self.send("isready")
        self.read_until("readyok")

    def _drain(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip("\r\n"))
        self.lines.put(None)  # EOF sentinel: the engine is gone

    def send(self, line):
        if self.proc.poll() is not None:
            raise RuntimeError("%s: engine exited (status %s)" % (self.name, self.proc.poll()))
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def read_until(self, token, timeout=120.0):
        collected = []
        deadline = time.monotonic() + timeout
        while True:
            try:
                line = self.lines.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                raise RuntimeError("%s: timed out waiting for '%s'" % (self.name, token))
            if line is None:
                raise RuntimeError("%s: engine exited while waiting for '%s'" % (self.name, token))
            collected.append(line)
            if line.startswith(token):
                return collected

    def divide(self, fen, depth):
        """{move: nodes} plus the total, for `fen` at `depth`."""
        self.send("position fen " + fen)
        self.send(self.perft_command % depth)
        lines = self.read_until("Nodes searched:")

        moves, total = {}, None
        for line in lines:
            if line.startswith("Nodes searched:"):
                total = int(line.split(":")[1])
            elif ":" in line and not line.startswith("info"):
                move, _, count = line.partition(":")
                move, count = move.strip(), count.strip()
                if count.isdigit() and 4 <= len(move) <= 5:
                    moves[move] = int(count)
        if total is None:
            raise RuntimeError("%s: no perft output for %s" % (self.name, fen))
        return moves, total

    def quit(self):
        try:
            self.send("quit")
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


class Oracle(Engine):
    perft_command = "go perft %d"


class Subject(Engine):
    perft_command = "perft %d"


# ------------------------------------------------------------------ campaign

def report_mismatch(fen, depth, ours, theirs):
    print("\nMISMATCH  depth %d\n  fen %s" % (depth, fen))
    for move in sorted(set(ours) | set(theirs)):
        a, b = ours.get(move), theirs.get(move)
        if a != b:
            print("  %-6s engine %-12s oracle %s" % (
                move, "-" if a is None else a, "-" if b is None else b))


def campaign(args):
    depth = int(args.depth)
    subject = Subject(args.engine)
    oracle = Oracle(args.oracle)
    rng = random.Random(args.seed)
    positions = failures = 0

    try:
        for game in range(args.games):
            fen = sp_fen(rng.randrange(960))

            for _ in range(args.plies):
                # Shallow at every position beats deep at a few: castling bugs
                # need the right SHAPE on the board, not depth, and a random
                # walk visits far more shapes per second this way.
                ours, our_total = subject.divide(fen, depth)
                theirs, their_total = oracle.divide(fen, depth)
                positions += 1

                if ours != theirs:
                    report_mismatch(fen, depth, ours, theirs)
                    failures += 1
                    break

                if not ours:
                    break  # checkmate or stalemate: nothing left to walk into

                # Advance both engines the same way, then re-read the FEN from
                # the ORACLE so a bug in our own FEN writer cannot quietly
                # feed us positions the oracle never confirmed.
                move = rng.choice(sorted(ours))
                oracle.send("position fen %s moves %s" % (fen, move))
                oracle.send("d")
                for line in oracle.read_until("Checkers:", timeout=20.0):
                    if line.startswith("Fen:"):
                        fen = line.split(":", 1)[1].strip()

            if (game + 1) % 10 == 0:
                print("  %d games, %d positions, %d failures"
                      % (game + 1, positions, failures), flush=True)
    finally:
        subject.quit()
        oracle.quit()

    print("\n%d positions compared at depth %d, %d failures" % (positions, depth, failures))
    return 1 if failures else 0


# ---------------------------------------------------------------- sealing

def parse_fen_castling(fen):
    """(king square, [castling rook squares]) per colour, from a FEN.

    Enough FEN parsing to know which move in a divide is a castle, and no
    more. Written here rather than asked of either engine on purpose: a
    suite whose castling lines are validated by the engine under test would
    certify itself.
    """
    placement, _, rights = fen.split()[:3]
    board = {}
    for rank, row in enumerate(placement.split("/")):
        f = 0
        for ch in row:
            if ch.isdigit():
                f += int(ch)
            else:
                board["abcdefgh"[f] + str(8 - rank)] = ch
                f += 1

    out = {}
    for colour, king, rook in (("w", "K", "R"), ("b", "k", "r")):
        home = "1" if colour == "w" else "8"
        ksq = next((sq for sq, pc in board.items() if pc == king), None)
        rooks = []
        for ch in rights:
            if ch == "-" or (ch.islower() != (colour == "b")):
                continue
            u = ch.upper()
            files = [f for f in "ABCDEFGH"
                     if board.get(f.lower() + home) == rook]
            if u in ("K", "Q") and ksq:
                side = [f for f in files
                        if (f.lower() > ksq[0]) == (u == "K")]
                if side:
                    rooks.append((max if u == "K" else min)(side).lower() + home)
            elif u in files:
                rooks.append(u.lower() + home)
        out[colour] = (ksq, rooks)
    return out


def castling_moves_in(fen, moves):
    """The castling moves among `moves`, in king-takes-own-rook spelling."""
    stm = fen.split()[1]
    ksq, rooks = parse_fen_castling(fen)[stm]
    return [] if not ksq else [ksq + r for r in rooks if ksq + r in moves]


def reseal(args):
    """Rewrite an .epd's counts from the oracle, preserving the file verbatim.

    In place and comment-preserving because the comments ARE the suite: each
    one names the rule its line is testing, and a regenerated file that lost
    them would leave twenty near-identical FENs and no way to tell which rule
    a failure broke.
    """
    oracle = Oracle(args.oracle)
    spec = re.compile(r";\s*D(\d+)\s+\d+")
    out, checked, silent = [], 0, []

    try:
        for line in io.open(args.src, encoding="utf-8"):
            stripped = line.strip()
            if not stripped or stripped.startswith("#") or ";" not in stripped:
                out.append(line)
                continue

            fen = stripped.split(";")[0].strip()
            depths = [int(d) for d in spec.findall(stripped)]
            counts = {}
            for depth in depths:
                _, counts[depth] = oracle.divide(fen, depth)

            # Does this line even offer a castle? A "castling is illegal here"
            # test is only meaningful next to the move it forbids, and a
            # positive test that generates no castle at all is testing nothing.
            root, _ = oracle.divide(fen, 1)
            castles = castling_moves_in(fen, root)
            if not castles:
                silent.append(fen)

            body = spec.sub(lambda m: ";D%s %d" % (m.group(1), counts[int(m.group(1))]), stripped)
            out.append(body + "\n")
            checked += 1
            print("  %-56s %s  castles: %s"
                  % (fen, " ".join("D%d %d" % (d, counts[d]) for d in depths),
                     ",".join(castles) if castles else "NONE"), flush=True)
    finally:
        oracle.quit()

    io.open(args.out, "w", encoding="utf-8", newline="\n").writelines(out)
    print("\nresealed %d positions into %s" % (checked, args.out))
    if silent:
        print("\nNOTE: %d position(s) generate no castling move at the root. That is\n"
              "correct for a line testing that a castle is FORBIDDEN, and a bug in the\n"
              "suite for any other:" % len(silent))
        for fen in silent:
            print("  " + fen)
    return 0


def seal_startpos(args):
    depths = sorted(int(d) for d in args.depth.split(","))
    header = (
        "# Perft for all 960 Chess960 start positions, sealed from an independent\n"
        "# engine. Regenerate with:\n"
        "#   python tools/chess960diff.py seal-startpos tests/perft/chess960-startpos.epd \\\n"
        "#       -depth %s -oracle <uci engine>\n"
        "#\n"
        "# One line per Scharnagl SP number, in order, so the Nth position line is\n"
        "# SP N-1. Every castling geometry Chess960 can produce appears here at least\n"
        "# once, which is what makes this the broad net; tests/perft/chess960.epd is\n"
        "# the sharp one. Two depths so the fast gate can cap at the shallower:\n"
        "#   make perft       depth %d, seconds\n"
        "#   make perft-all   both, minutes\n"
        "#\n"
        "# SP 518 is the standard array, and its counts must equal the standard start\n"
        "# position's in tests/perft/standard.epd. The two suites reach the same board\n"
        "# down completely different code paths - one parses KQkq into fixed squares,\n"
        "# the other derives the array from an index and spells its rights HAha - so\n"
        "# agreeing there is worth more than either count alone.\n"
        "#\n"
        "# This file also pins the SP NUMBERING. board.c derives the array from the\n"
        "# index; if that derivation drifted by even one position, these FENs would\n"
        "# stop being the ones `chess960 selftest` generates, and it says so.\n"
        % (",".join(str(d) for d in depths), depths[0]))

    oracle = Oracle(args.oracle)
    try:
        with io.open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(header)
            for i in range(960):
                fen = sp_fen(i)
                counts = [oracle.divide(fen, d)[1] for d in depths]
                f.write("%s %s\n" % (fen, " ".join(";D%d %d" % (d, c)
                                                   for d, c in zip(depths, counts))))
                if i % 120 == 0:
                    print("  SP %3d  %s  %s" % (i, fen.split()[0],
                                                " ".join(map(str, counts))), flush=True)
    finally:
        oracle.quit()
    print("\nwrote 960 positions into %s" % args.out)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["campaign", "seal-startpos", "reseal"])
    ap.add_argument("src", nargs="?", help="reseal: the suite to re-seal in place")
    ap.add_argument("out", nargs="?", help="output .epd (defaults to src for reseal)")
    ap.add_argument("-oracle", default="stockfish", help="oracle engine command")
    ap.add_argument("-engine", default="./stormbreaker", help="engine under test")
    ap.add_argument("-games", type=int, default=200)
    ap.add_argument("-plies", type=int, default=40)
    ap.add_argument("-depth", default="4",
                    help="perft depth; seal-startpos accepts a comma-separated list")
    ap.add_argument("-seed", type=int, default=1)
    args = ap.parse_args()

    if args.mode == "campaign":
        return campaign(args)
    if not args.src:
        ap.error("%s needs a path" % args.mode)
    if args.mode == "seal-startpos":
        args.out = args.src
        return seal_startpos(args)
    args.out = args.out or args.src
    return reseal(args)


if __name__ == "__main__":
    sys.exit(main())
