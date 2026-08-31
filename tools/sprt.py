"""
sprt.py - run a sequential probability ratio test between two builds.

Answers "is this better than the last version?" and nothing else. For a
field, a rated ladder or a Stockfish rung, see gauntlet.py.

    python tools/sprt.py --smoke
    python tools/sprt.py
    python tools/sprt.py --tc LTC
    python tools/sprt.py --dev ./stormbreaker-nnue.exe --base external/baselines/v0.1.exe

Read docs/TESTING.md before trusting a result. The short version: one change
per test, do not read the number early, and a result whose interval spans zero
has not shown anything however good the point estimate looks.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import common as c


def parse_options(text: str) -> dict:
    """NAME=VALUE pairs, comma or space separated."""
    out = {}
    for tok in re.split(r"[,\s]+", text.strip()):
        if not tok:
            continue
        if "=" not in tok:
            c.fail(f"bad option {tok!r}, expected NAME=VALUE")
            sys.exit(2)
        k, v = tok.split("=", 1)
        out[k.removeprefix("option.")] = v
    return out


def options_from_state(path: str) -> dict:
    """The current parameter values out of a tune.py checkpoint.

    Reading them rather than retyping them is the point: a 21-parameter set
    copied by hand is a set with a typo in it, and the resulting SPRT measures
    something nobody intended.
    """
    d = json.loads(Path(path).read_text(encoding="utf-8"))
    return {p["name"]: int(round(p["value"])) for p in d["params"]}


def main() -> int:
    ap = argparse.ArgumentParser(
        prog="sprt.py",
        description="SPRT one build against another.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--dev", help="engine under test (default: the built stormbreaker)")
    ap.add_argument("--base", help="baseline (default: newest in external/baselines)")
    ap.add_argument("--tc", default="STC", help="VSTC, STC, LTC, or a literal like 10+0.1")
    ap.add_argument("--bounds", help='"elo0,elo1"; defaults per time control')
    ap.add_argument("--concurrency", type=int, default=0)
    ap.add_argument("--hash", type=int, default=16, dest="hash_mb")
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--rounds", type=int, default=40000, help="game-pair cap; default runs to a verdict")
    ap.add_argument("--book")
    ap.add_argument("--dev-options", default="",
                    help='UCI options for dev only: "RfpMargin=72 DeltaMargin=222". '
                         'An "option." prefix is tolerated so tune.py output pastes straight in.')
    ap.add_argument("--dev-from",
                    help="read dev's options from a tune.py state file (external/tune/spsa.json)")
    ap.add_argument("--smoke", action="store_true", help="verify the pipeline with two quick games")
    ap.add_argument("--dry-run", action="store_true", help="print the command and exit")
    args = ap.parse_args()

    fastchess = c.require(c.get_fastchess(), "fastchess not found. Run: powershell -File tools\\setup.ps1")

    tc, tc_label = c.resolve_tc(args.tc)
    elo0, elo1 = c.resolve_bounds(tc_label, args.bounds)
    concurrency = args.concurrency if args.concurrency > 0 else c.default_concurrency()
    rounds = args.rounds

    # ------------------------------------------------------------ engines --

    if args.smoke:
        # Stockfish against itself: the point is to prove the runner, book,
        # concurrency and PGN output all work, not to measure anything.
        sf = c.require(c.get_stockfish(), "Stockfish not found. Run: powershell -File tools\\setup.ps1")
        dev_path, base_path = sf, sf
        dev_name, base_name = "SF-dev", "SF-base"
        tc, tc_label = c.TC_PRESETS["VSTC"], "VSTC"
        rounds = 2
    else:
        dev = args.dev or c.get_engine_binary()
        c.require(dev, "Engine not built. Run 'make' first.")
        dev_path = str(Path(dev).resolve())

        base = args.base or c.latest_baseline()
        if not base:
            c.fail("No baseline found.")
            print("  Snapshot the current build as the baseline first:")
            print('    make snapshot ARGS="--name v0.1"')
            print("  then make your change, rebuild, and re-run this script.")
            return 1
        base_path = str(Path(base).resolve())
        dev_name = "dev"
        base_name = Path(base_path).stem

        # Same binary on both sides is the right way to test a parameter set:
        # it isolates the options as the only difference. fastchess needs the
        # two seats named differently, though.
        if Path(dev_path) == Path(base_path):
            dev_name, base_name = "tuned", "default"

    dev_options = {}
    if args.dev_from:
        dev_options.update(options_from_state(args.dev_from))
    if args.dev_options:
        dev_options.update(parse_options(args.dev_options))

    book = args.book or c.get_book()

    c.ensure_dir(c.GAMES_DIR)
    pgn = c.GAMES_DIR / f"{c.stamp()}-{tc_label}.pgn"

    extra = []
    if not args.smoke:
        # model=normalized measures in normalised Elo, which makes results
        # comparable across time controls and books.
        extra = ["-sprt", f"elo0={elo0}", f"elo1={elo1}", "alpha=0.05", "beta=0.05",
                 "model=normalized"]

    fc_args = c.match_args(
        engines=[
            c.engine_args(dev_path, dev_name, dev_options),
            c.engine_args(base_path, base_name),
        ],
        tc=tc,
        rounds=rounds,
        concurrency=concurrency,
        pgn=pgn,
        hash_mb=args.hash_mb,
        threads=args.threads,
        book=book,
        extra=extra,
    )

    c.section("SPRT configuration")
    print(f"  dev          {dev_path}")
    print(f"  base         {base_path}")
    print(f"  time control {tc}  ({tc_label})")
    if not args.smoke:
        print(f"  bounds       [{elo0}, {elo1}]  alpha=0.05 beta=0.05")
    print(f"  concurrency  {concurrency}")
    print(f"  hash/threads {args.hash_mb} MB / {args.threads}")
    if book:
        print(f"  book         {book}")
    print(f"  pgn          {pgn}")
    if dev_options:
        print(f"  dev options  {len(dev_options)}: " +
              " ".join(f"{k}={v}" for k, v in dev_options.items()))
    print()

    if args.dry_run:
        c.print_command(fastchess, fc_args)
        return 0

    code, output = c.run_match(fastchess, fc_args, stream=True)

    print()
    if args.smoke:
        if code == 0:
            c.ok("Smoke test passed - runner, book, concurrency and PGN output all work.")
            if pgn.exists():
                games = pgn.read_text(encoding="utf-8", errors="replace").count("[Event ")
                c.ok(f"{games} games written to {pgn}")
        else:
            c.fail(f"Smoke test failed (exit {code}).")
        return code

    r = c.parse_result(output)
    if r.games:
        c.section("Result")
        print(f"  {r.games} games   {r.wins}W / {r.losses}L / {r.draws}D   {r.score:.2%}")
        if r.elo is not None:
            print(f"  Elo {r.elo:+.2f}")
        if r.llr is not None:
            print(f"  LLR {r.llr:+.2f}  bounds [{elo0}, {elo1}]")
        print(f"  pgn {pgn}")
        print()
        print("  Record it in docs/EXPERIMENTS.md - including if it failed.")

    return code


if __name__ == "__main__":
    sys.exit(main())
