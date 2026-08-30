"""
gauntlet.py - run the engine against a field of opponents and report a table.

    python tools/gauntlet.py --games 200
    python tools/gauntlet.py --opponents external/baselines/v0.1.exe
    python tools/gauntlet.py --include-stockfish --skill-level 5
    python tools/gauntlet.py --field engines --games 200

Where sprt.py answers "is this patch better than the last version?", a gauntlet
answers "how strong is the engine, actually?". Use it to confirm real progress
across several baselines at once, and to catch the regression self-play hides:
an engine can beat its previous self while getting worse against a different
style of opponent, because each successive test only ever asked it to beat the
one opponent it was overfitting to.

The default field is external/baselines (past versions of this engine) plus
external/engines (third-party engines with published CCRL ratings, fetched by
`make engines-fetch`). Use `--field engines` for a reading against rated
opponents alone. The CCRL numbers are landmarks on someone else's pool, not a
rating this engine has earned - see the caveats at the top of rating.py, which
apply just as much here.

NOTE ON STOCKFISH: at full strength it wins 100% and tells you nothing. Use
--skill-level to weaken it to something informative, and raise it as the engine
improves. For a calibrated *rating* rather than a table, use rating.py, which
drives UCI_Elo properly - Skill Level and UCI_Elo are different mechanisms and
only the latter claims to be on an Elo scale.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import common as c


def main() -> int:
    ap = argparse.ArgumentParser(prog="gauntlet.py", description="Play a field of opponents.")
    ap.add_argument("--engine")
    ap.add_argument("--opponents", default="",
                    help="comma-separated paths; default is every saved baseline plus "
                         "every fetched third-party engine")
    ap.add_argument("--field", choices=("all", "baselines", "engines"), default="all",
                    help="which default field to play; ignored when --opponents is given")
    ap.add_argument("--games", type=int, default=100, help="games per pairing")
    ap.add_argument("--tc", default="STC")
    ap.add_argument("--concurrency", type=int, default=0)
    ap.add_argument("--hash", type=int, default=16, dest="hash_mb")
    ap.add_argument("--include-stockfish", action="store_true")
    ap.add_argument("--skill-level", type=int, default=0)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    fastchess = c.require(c.get_fastchess(), "fastchess not found. Run: powershell -File tools\\setup.ps1")

    engine = args.engine or c.get_engine_binary()
    c.require(engine, "Engine not built. Run 'make' first.")
    engine = str(Path(engine).resolve())

    tc, tc_label = c.resolve_tc(args.tc)
    concurrency = args.concurrency if args.concurrency > 0 else c.default_concurrency()

    if args.opponents:
        opponents = [s.strip() for s in re.split(r"[,;]", args.opponents) if s.strip()]
    else:
        # Baselines say "better than before"; the fetched engines carry published
        # CCRL ratings, which is the only thing here that puts the table on a
        # scale someone outside this repository can read. Default to both, since
        # the round-robin below then anchors the baselines against rated players
        # rather than leaving the whole field floating relative to the dev build.
        opponents = []
        if args.field in ("all", "baselines"):
            c.ensure_dir(c.BASELINE_DIR)
            opponents += [str(p) for p in sorted(c.BASELINE_DIR.glob("*.exe"))]
        if args.field in ("all", "engines"):
            c.ensure_dir(c.ENGINES_DIR)
            opponents += [str(p) for p in sorted(c.ENGINES_DIR.glob("*.exe"))]

    engines = [c.engine_args(engine, "engine")]
    for opp in opponents:
        p = Path(opp)
        if not p.exists():
            c.warn(f"skipping missing opponent: {opp}")
            continue
        engines.append(c.engine_args(str(p.resolve()), p.stem))

    if args.include_stockfish:
        sf = c.get_stockfish()
        if sf:
            # "Skill Level" really does contain a space; fastchess passes the
            # whole token through to the engine, which is what UCI expects.
            engines.append(
                c.engine_args(sf, f"SF-skill{args.skill_level}",
                              {"Skill Level": args.skill_level})
            )
        else:
            c.warn("Stockfish not found; continuing without it.")

    if len(engines) < 2:
        c.fail("No opponents. Snapshot a baseline, fetch the rated field, or pass "
               "--opponents / --include-stockfish.")
        print("  make snapshot ARGS=\"--name v0.1\"")
        print("  make engines-fetch")
        return 1

    c.ensure_dir(c.GAMES_DIR)
    pgn = c.GAMES_DIR / f"{c.stamp()}-gauntlet.pgn"

    fc_args = c.match_args(
        engines=engines,
        tc=tc,
        rounds=max(1, args.games // 2),
        concurrency=concurrency,
        pgn=pgn,
        hash_mb=args.hash_mb,
        book=c.get_book(),
        # Round-robin so the opponents also play each other, which anchors the
        # Elo table instead of leaving every rating relative to a single engine.
        extra=["-tournament", "roundrobin"],
    )

    c.section("Gauntlet")
    print(f"  engine       {engine}")
    print(f"  opponents    {len(engines) - 1}")
    print(f"  time control {tc}  ({tc_label})")
    print(f"  games        ~{args.games} per pairing")
    print(f"  concurrency  {concurrency}")
    print(f"  pgn          {pgn}")
    print()

    if args.dry_run:
        c.print_command(fastchess, fc_args)
        return 0

    code, _ = c.run_match(fastchess, fc_args, stream=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
