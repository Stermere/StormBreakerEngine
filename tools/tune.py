"""
tune.py - SPSA tuning of the search parameters exposed by TUNE_SEARCH=on.

    make nnue TUNE_SEARCH=on EXE=stormbreaker-tune   # -> stormbreaker-tune-nnue.exe
    python tools/tune.py --engine ./stormbreaker-tune-nnue.exe --list
    python tools/tune.py --engine ./stormbreaker-tune-nnue.exe --iterations 800
    python tools/tune.py --resume

WHAT THIS IS FOR. Every margin in search.c was fitted against the classical
evaluation's scale and noise profile, and the network shares neither. Fitting
them one at a time by SPRT cannot work: there are eleven of them, they
interact, and each individual effect is far below what a few thousand games
resolves. SPSA gives up the certainty an SPRT provides and buys the ability to
move eleven knobs at once.

WHAT IT IS NOT. It is not a measurement. SPSA will happily walk to a parameter
set that is no better than where it started and report it with a straight face,
because nothing in the procedure tests the null hypothesis. **The output of a
tuning run is a CANDIDATE, and the candidate needs its own SPRT against the
values it replaced before it is worth committing.** Tuning at one time control
also overfits to it; confirm at another before believing it.

THE PARAMETER LIST COMES FROM THE ENGINE. It is read out of the `uci`
handshake rather than duplicated here, so a new TUNABLE in search.c is picked
up automatically with the range its author chose. A tuner carrying its own copy
of the list is a tuner that silently stops covering half of it.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import subprocess
import sys
import time
from pathlib import Path

import common as c

# Standard SPSA exponents (Spall). These are not worth tuning; the per-parameter
# c_end and r_end below are.
ALPHA = 0.602
GAMMA = 0.101

# Floor on the c_end used to size the STEP (not the perturbation).
#
# Travel over a run is about `r_end * c_end * iterations * E[result]`, and
# `c_end` defaults to a twentieth of the declared range. So a parameter declared
# [1, 10] takes a step some 1500x smaller than one declared [2048, 32768] - while
# needing the same absolute resolution as every other integer option, which is
# one. Worked through for E17's run, a parameter with `c_end = 1` travels about
# 0.46 units in 2300 iterations and needs 0.5 to change the value that ships.
#
# E17 is the evidence: of its 21 parameters the three reported unchanged -
# CapHistDivisor, NmpEvalMax, HistBonusDepthMax - were three of the four with
# the smallest c_end.
#
# Flooring c_end for the step alone leaves every wide parameter's arithmetic
# bit-identical and gives the narrow ones a step that can cross an integer. It
# buys travel, and travel is symmetric: a parameter that can now move for signal
# can also random-walk. The drift test in E17 is still what separates the two.
STEP_C_FLOOR = 5.0

# Spin options that are engine configuration rather than search parameters.
NOT_TUNABLE = {
    "hash", "threads", "move overhead", "multipv", "ponder",
    "uci_chess960", "uci_limitstrength", "uci_elo", "evalfile", "clear hash",
}


# ------------------------------------------------------ engine interrogation


def query_options(engine: str, timeout: float = 10.0) -> dict:
    """Runs the UCI handshake and returns {name: (default, min, max)} for every
    spin option that looks like a search parameter."""
    proc = subprocess.Popen(
        [str(Path(engine).resolve())],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    assert proc.stdin is not None and proc.stdout is not None

    options: dict = {}
    try:
        proc.stdin.write("uci\n")
        proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            line = line.strip()
            if line == "uciok":
                break
            if not line.startswith("option name "):
                continue
            # option name RfpMargin type spin default 80 min 20 max 250
            toks = line.split()
            try:
                ti = toks.index("type")
            except ValueError:
                continue
            name = " ".join(toks[2:ti])
            if toks[ti + 1] != "spin" or name.lower() in NOT_TUNABLE:
                continue
            try:
                d = int(toks[toks.index("default") + 1])
                lo = int(toks[toks.index("min") + 1])
                hi = int(toks[toks.index("max") + 1])
            except (ValueError, IndexError):
                continue
            options[name] = (d, lo, hi)
    finally:
        try:
            proc.stdin.write("quit\n")
            proc.stdin.flush()
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    return options


# --------------------------------------------------------------- parameters


class Param:
    """One tunable, and the two numbers that decide how SPSA treats it.

    `c_end` is how far the perturbation still reaches on the last iteration -
    too small and the match result is pure noise, too large and every iteration
    plays two engines that barely resemble the one being tuned. A twentieth of
    the declared range is a starting heuristic, not a law.

    `r_end` sets the final step size relative to that perturbation. 0.002 is
    the value the engine-tuning community converged on; it is deliberately
    small, because SPSA's gradient estimate is one noisy game batch. Note that
    travel scales as `iterations * r_end`, so a value carried over from a
    20000-iteration run onto a 2000-iteration one measures the gradient
    precisely and then declines to act on it - which is exactly what E17's
    first run did.

    `step_c` is `c_end` floored at STEP_C_FLOOR, and it sizes the step while
    `c_end` goes on sizing the perturbation. The two were the same number until
    that turned out to leave narrow-range parameters unable to move at all.
    """

    def __init__(self, name, value, lo, hi, c_end=None, r_end=0.002):
        self.name = name
        self.value = float(value)
        self.lo = float(lo)
        self.hi = float(hi)
        self.start = float(value)
        self.c_end = float(c_end) if c_end else max(1.0, (hi - lo) / 20.0)
        self.r_end = float(r_end)

        # Sizes the step; c_end still sizes the perturbation. Equal for every
        # parameter wide enough that the floor does not bite, which is what
        # keeps this change invisible to them. See STEP_C_FLOOR.
        self.step_c = max(self.c_end, STEP_C_FLOOR)

    def clamp(self, v: float) -> float:
        return min(self.hi, max(self.lo, v))

    def to_dict(self) -> dict:
        return {
            "name": self.name, "value": self.value, "lo": self.lo, "hi": self.hi,
            "start": self.start, "c_end": self.c_end, "r_end": self.r_end,
        }

    @staticmethod
    def from_dict(d: dict) -> "Param":
        p = Param(d["name"], d["value"], d["lo"], d["hi"], d["c_end"], d["r_end"])
        p.start = d.get("start", d["value"])
        return p


# --------------------------------------------------------------------- run


class Tuner:
    def __init__(self, state_path: Path):
        self.state_path = state_path
        self.csv_path = state_path.with_suffix(".csv")
        self.engine = ""
        self.params: list[Param] = []
        self.iteration = 0
        self.iterations = 0
        self.tc = "8+0.08"
        self.tc_label = "STC"
        self.depth = 0  # non-zero replaces the clock with a fixed-depth search
        self.games_per_iter = 16
        self.concurrency = 1
        self.hash_mb = 16
        self.book = None
        self.seed = 0
        self.rng = random.Random()
        self.wins = 0
        self.losses = 0
        self.draws = 0

    # -- persistence ------------------------------------------------------

    def save(self) -> None:
        c.ensure_dir(self.state_path.parent)
        self.state_path.write_text(
            json.dumps(
                {
                    "engine": self.engine,
                    "params": [p.to_dict() for p in self.params],
                    "iteration": self.iteration,
                    "iterations": self.iterations,
                    "tc": self.tc,
                    "tc_label": self.tc_label,
                    "depth": self.depth,
                    "games_per_iter": self.games_per_iter,
                    "concurrency": self.concurrency,
                    "hash_mb": self.hash_mb,
                    "book": self.book,
                    "seed": self.seed,
                    "rng": self.rng.getstate(),
                    "wins": self.wins,
                    "losses": self.losses,
                    "draws": self.draws,
                },
                indent=2,
                default=list,
            ),
            encoding="utf-8",
        )

    def load(self) -> None:
        d = json.loads(self.state_path.read_text(encoding="utf-8"))
        self.engine = d["engine"]
        self.params = [Param.from_dict(p) for p in d["params"]]
        self.iteration = d["iteration"]
        self.iterations = d["iterations"]
        self.tc = d["tc"]
        self.tc_label = d["tc_label"]
        self.depth = d.get("depth", 0)
        self.games_per_iter = d["games_per_iter"]
        self.concurrency = d["concurrency"]
        self.hash_mb = d["hash_mb"]
        self.book = d["book"]
        self.seed = d["seed"]
        self.wins = d.get("wins", 0)
        self.losses = d.get("losses", 0)
        self.draws = d.get("draws", 0)
        # getstate/setstate round-trips through JSON as nested lists; the RNG
        # wants tuples back, and resuming without this replays the same
        # perturbation sequence from scratch.
        st = d.get("rng")
        if st:
            self.rng.setstate((st[0], tuple(st[1]), st[2]))

    def log_row(self, result: float, score: float) -> None:
        # Self-sufficient rather than relying on save() having run: step() logs
        # before main() saves, so on the very first iteration of a fresh run
        # nothing has created the directory yet.
        c.ensure_dir(self.csv_path.parent)
        new = not self.csv_path.exists()
        with self.csv_path.open("a", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            if new:
                w.writerow(["iteration", "result", "score"] + [p.name for p in self.params])
            w.writerow(
                [self.iteration, f"{result:.4f}", f"{score:.4f}"]
                + [f"{p.value:.3f}" for p in self.params]
            )

    # -- the loop ---------------------------------------------------------

    def gains(self, p: Param) -> tuple[float, float]:
        """(a_k, c_k) for this iteration, in the standard SPSA schedule."""
        n = max(1, self.iterations)
        A = 0.1 * n
        # p.step_c rather than a second p.c_end: the end-of-run step works out
        # to `a_end / c_end`, so this makes it `r_end * step_c` and decouples
        # how far the two test engines differ from how far the value moves.
        a_end = p.r_end * p.c_end * p.step_c
        a = a_end * (A + n) ** ALPHA
        c0 = p.c_end * n**GAMMA
        k = self.iteration
        return a / (A + k + 1) ** ALPHA, c0 / (k + 1) ** GAMMA

    def step(self, fastchess: str) -> bool:
        """One SPSA iteration. Returns False if the match failed."""
        deltas = [1 if self.rng.random() < 0.5 else -1 for _ in self.params]
        gains = [self.gains(p) for p in self.params]

        plus, minus = {}, {}
        for p, d, (_, ck) in zip(self.params, deltas, gains):
            plus[p.name] = int(round(p.clamp(p.value + ck * d)))
            minus[p.name] = int(round(p.clamp(p.value - ck * d)))

        pgn = None  # thousands of iterations of PGN is gigabytes of nothing
        fc_args = c.match_args(
            engines=[
                c.engine_args(self.engine, "plus", plus),
                c.engine_args(self.engine, "minus", minus),
            ],
            tc=self.tc,
            rounds=max(1, self.games_per_iter // 2),
            concurrency=self.concurrency,
            pgn=pgn,
            hash_mb=self.hash_mb,
            book=self.book,
            depth=self.depth,
        )

        code, output = c.run_match(fastchess, fc_args, stream=False)
        r = c.parse_result(output)
        if code != 0 or r.games == 0:
            c.fail(f"match failed at iteration {self.iteration} (exit {code})")
            for line in output.strip().splitlines()[-8:]:
                print("    " + line)
            return False

        self.wins += r.wins
        self.losses += r.losses
        self.draws += r.draws

        # Score of `plus` mapped to [-1, +1]: the gradient signal.
        result = 2.0 * r.score - 1.0

        for p, d, (ak, ck) in zip(self.params, deltas, gains):
            # Divide by the perturbation the two engines ACTUALLY differed by,
            # not the one that was asked for. `ck` is a float; what reached the
            # engines went through int(round(clamp(...))) on both sides, and
            # near a bound the clamp can halve the separation or remove it
            # outright. Dividing by ck there understates the gradient exactly
            # where it is already weakest, which is how a parameter sitting on
            # its own bound - HistBonusDepthMax at 20 of [4, 20] - reports as
            # "unchanged" when it was never given a fair measurement.
            #
            # Both sides are integers, so a non-zero separation is at least 1
            # and eff is at least 0.5. Zero means the two engines were the same
            # binary configuration and the result carries no information about
            # this parameter, whatever it says about the others.
            eff = (plus[p.name] - minus[p.name]) / 2.0 * d
            if eff <= 0.0:
                continue
            p.value = p.clamp(p.value + ak * result / eff * d)

        self.iteration += 1
        self.log_row(result, r.score)
        return True

    def report(self) -> None:
        c.section("Parameters")
        print(f"  {'name':<20} {'start':>8} {'now':>8} {'delta':>8}   range")
        print(f"  {'-' * 20} {'-' * 8} {'-' * 8} {'-' * 8}   -----")
        for p in self.params:
            now = int(round(p.value))
            print(
                f"  {p.name:<20} {int(p.start):>8} {now:>8} {now - int(p.start):>+8}"
                f"   [{int(p.lo)}, {int(p.hi)}]"
            )
        print()
        print("  As UCI options:")
        print("    " + " ".join(f"option.{p.name}={int(round(p.value))}" for p in self.params))
        print()
        c.warn("This is a CANDIDATE, not a result. SPRT it against the old values")
        c.warn("before committing anything, and record it in docs/EXPERIMENTS.md.")


# --------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(prog="tune.py", description="SPSA-tune the search parameters.")
    ap.add_argument("--engine", help="a TUNE_SEARCH=on build")
    ap.add_argument("--params", help="comma-separated subset; default is all of them")
    ap.add_argument("--exclude", default="SeeCaptureMargin,SeeQuietMargin",
                    help="comma-separated names to leave alone")
    ap.add_argument("--iterations", type=int, default=1000)
    ap.add_argument("--games-per-iter", type=int, default=0,
                    help="default: 2x concurrency, so every core stays busy")
    ap.add_argument("--tc", default="STC")
    ap.add_argument("--depth", type=int, default=0,
                    help="fixed-depth games instead of a clock. DIAGNOSTIC ONLY - see below")
    ap.add_argument("--concurrency", type=int, default=0)
    ap.add_argument("--hash", type=int, default=16, dest="hash_mb")
    ap.add_argument("--book")
    ap.add_argument("--seed", type=int, default=0, help="0 picks one and records it")
    ap.add_argument("--c-end", type=float, help="override the perturbation for every parameter")
    ap.add_argument("--r-end", type=float, default=0.002, help="final learning rate factor")
    ap.add_argument("--state", default=str(c.TUNE_DIR / "spsa.json"))
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--list", action="store_true", help="show what the engine exposes and exit")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    t = Tuner(Path(args.state))

    if args.resume:
        if not t.state_path.exists():
            c.fail(f"no state at {t.state_path}")
            return 1
        t.load()
        c.ok(f"resumed at iteration {t.iteration}/{t.iterations} from {t.state_path}")
    else:
        engine = args.engine or c.get_engine_binary()
        c.require(engine, "No engine. Build one with: make TUNE_SEARCH=on")
        engine = str(Path(engine).resolve())

        opts = query_options(engine)
        if not opts:
            c.fail("That engine exposes no tunable spin options.")
            print("  It was almost certainly built without TUNE_SEARCH. Rebuild with:")
            print("    make nnue TUNE_SEARCH=on EXE=stormbreaker-tune")
            print("  which writes stormbreaker-tune-nnue.exe - `make nnue` appends the suffix.")
            return 1

        if args.list:
            c.section(f"Tunables exposed by {Path(engine).name}")
            for name, (d, lo, hi) in opts.items():
                print(f"  {name:<20} default {d:>6}   range [{lo}, {hi}]")
            return 0

        wanted = [s.strip() for s in args.params.split(",")] if args.params else list(opts)
        excluded = {s.strip().lower() for s in args.exclude.split(",") if s.strip()}

        params = []
        for name in wanted:
            if name not in opts:
                c.fail(f"engine does not expose '{name}'")
                return 1
            if name.lower() in excluded:
                continue
            d, lo, hi = opts[name]
            params.append(Param(name, d, lo, hi, args.c_end, args.r_end))

        if not params:
            c.fail("nothing left to tune after --exclude")
            return 1

        t.engine = engine
        t.params = params
        t.iterations = args.iterations
        t.tc, t.tc_label = c.resolve_tc(args.tc)
        t.depth = args.depth
        t.concurrency = args.concurrency if args.concurrency > 0 else c.default_concurrency()
        t.games_per_iter = args.games_per_iter or max(2, 2 * t.concurrency)
        if t.games_per_iter % 2:
            t.games_per_iter += 1  # fastchess plays colour-reversed pairs
        t.hash_mb = args.hash_mb
        t.book = args.book or c.get_book()
        t.seed = args.seed or random.randrange(1, 2**31)
        t.rng = random.Random(t.seed)

    # A fresh run must not inherit a previous run's files. log_row() appends, so
    # starting over would write the new rows onto the end of an old CSV and make
    # both unreadable; save() would overwrite the old JSON outright. Rotated
    # rather than deleted, because the previous run is usually the thing the new
    # one is being compared against.
    if not args.resume:
        for stale in (t.state_path, t.csv_path):
            if stale.exists():
                rotated = stale.with_name(f"{stale.stem}-{c.stamp()}{stale.suffix}")
                stale.rename(rotated)
                c.warn(f"existing {stale.name} moved aside to {rotated.name}")

    fastchess = c.require(c.get_fastchess(), "fastchess not found. Run: powershell -File tools\\setup.ps1")

    total_games = t.iterations * t.games_per_iter
    c.section("SPSA configuration")
    print(f"  engine        {t.engine}")
    print(f"  parameters    {len(t.params)}: {', '.join(p.name for p in t.params)}")
    print(f"  iterations    {t.iterations} ({t.iteration} done)")
    print(f"  games/iter    {t.games_per_iter}   total {total_games}")
    if t.depth:
        print(f"  search        fixed depth {t.depth}  (DIAGNOSTIC - values are not shippable)")
    else:
        print(f"  time control  {t.tc}  ({t.tc_label})")
    print(f"  concurrency   {t.concurrency}")
    print(f"  seed          {t.seed}")
    print(f"  state         {t.state_path}")
    print(f"  history       {t.csv_path}")
    print()

    if args.dry_run:
        c.print_command(fastchess, c.match_args(
            engines=[c.engine_args(t.engine, "plus", {p.name: int(p.value) for p in t.params}),
                     c.engine_args(t.engine, "minus", {p.name: int(p.value) for p in t.params})],
            tc=t.tc, rounds=max(1, t.games_per_iter // 2), concurrency=t.concurrency,
            pgn=None, hash_mb=t.hash_mb, book=t.book, depth=t.depth))
        t.report()
        return 0

    start = time.time()
    interrupted = False
    try:
        while t.iteration < t.iterations:
            if not t.step(fastchess):
                t.save()
                return 1

            elapsed = time.time() - start
            done = t.iteration
            rate = elapsed / max(1, done)
            eta = rate * (t.iterations - done)
            print(
                f"  [{done:>5}/{t.iterations}] "
                f"{t.wins}W/{t.losses}L/{t.draws}D  "
                f"eta {eta / 3600:.1f}h  "
                + "  ".join(f"{p.name}={int(round(p.value))}" for p in t.params),
                flush=True,
            )
            t.save()
    except KeyboardInterrupt:
        interrupted = True
        print()
        c.warn("interrupted - state saved, resume with: python tools/tune.py --resume")

    t.save()
    t.report()
    if not interrupted and t.iteration >= t.iterations:
        c.ok(f"finished {t.iterations} iterations in {(time.time() - start) / 3600:.1f}h")
    return 0


if __name__ == "__main__":
    sys.exit(main())
