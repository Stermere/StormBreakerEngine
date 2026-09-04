"""
gauntlet.py - run the engine against a field of opponents and report a table.

    python tools/gauntlet.py --games 200
    python tools/gauntlet.py --opponents external/baselines/v0.1.exe
    python tools/gauntlet.py --field engines --games 200
    python tools/gauntlet.py --field stockfish --games 200

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
rating this engine has earned.

STOCKFISH. `--field stockfish` plays it and nothing else; `--include-stockfish`
adds it to whatever field is already there. Both play it at full strength, which
is the point - the engine is now close enough that an unlimited Stockfish is an
informative opponent rather than a guaranteed 100% loss, and full strength
avoids the objection that sinks every handicapped ladder: a strength-limited
engine plays mostly full-strength moves with occasional deliberate errors, which
is not the error distribution any real opponent produces. `--skill-level N`
(0-20) applies Stockfish's handicap anyway if a weaker seat is wanted; it is a
handicap, not an Elo scale, and no number derived from it should be quoted as
one.

For an absolute reading prefer `--field engines`, whose rungs are real engines
at full strength carrying published ratings - see docs/EXPERIMENTS.md,
"Absolute strength", for what that ladder can and cannot support. When the
match finishes this hands the PGN to ratings.py, which prints the cross-table
and puts every seat on the CCRL scale; `make ratings` re-runs that alone.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import common as c
import ratings


def main() -> int:
    ap = argparse.ArgumentParser(prog="gauntlet.py", description="Play a field of opponents.")
    ap.add_argument("--engine")
    ap.add_argument("--opponents", default="",
                    help="comma-separated paths; default is every saved baseline plus "
                         "every fetched third-party engine")
    ap.add_argument("--field", choices=("all", "baselines", "engines", "stockfish"), default="all",
                    help="which default field to play; ignored when --opponents is given")
    ap.add_argument("--games", type=int, default=100, help="games per pairing")
    ap.add_argument("--tc", default="STC")
    ap.add_argument("--concurrency", type=int, default=0)
    ap.add_argument("--hash", type=int, default=16, dest="hash_mb")
    ap.add_argument("--include-stockfish", action="store_true",
                    help="add Stockfish to the field, alongside the rest of it")
    ap.add_argument("--skill-level", type=int, default=None,
                    help="handicap Stockfish (0-20); the default plays it at full strength")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    want_sf = args.include_stockfish or args.field == "stockfish"
    if not want_sf and args.skill_level is not None:
        # Accepting a strength knob and then ignoring it is how a run gets read
        # as a match it never was. Refuse instead.
        c.fail("--skill-level configures Stockfish, which is not in this field. "
               "Add --field stockfish or --include-stockfish.")
        return 1

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

    sf_seat = ""
    if want_sf:
        sf = c.get_stockfish()
        if not sf:
            missing = "Stockfish not found. Run: powershell -File tools\\setup.ps1"
            if args.field == "stockfish":
                c.fail(missing)
                return 1
            c.warn(f"{missing} Continuing without it.")
        elif args.skill_level is None:
            sf_seat = "SF"
            engines.append(c.engine_args(sf, sf_seat))
        else:
            # "Skill Level" really does contain a space; fastchess passes the
            # whole token through to the engine, which is what UCI expects.
            sf_seat = f"SF-skill{args.skill_level}"
            engines.append(c.engine_args(sf, sf_seat, {"Skill Level": args.skill_level}))

    if len(engines) < 2:
        c.fail("No opponents. Snapshot a baseline, fetch the rated field, or play "
               "Stockfish on its own.")
        print("  make snapshot ARGS=\"--name v0.1\"")
        print("  make engines-fetch")
        print("  make gauntlet ARGS=\"--field stockfish\"")
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
    if sf_seat:
        print(f"  stockfish    {sf_seat}")
    print(f"  time control {tc}  ({tc_label})")
    print(f"  games        ~{args.games} per pairing")
    print(f"  concurrency  {concurrency}")
    print(f"  pgn          {pgn}")
    print()

    if args.dry_run:
        c.print_command(fastchess, fc_args)
        return 0

    code, _ = c.run_match(fastchess, fc_args, stream=True)

    # The Elo column fastchess just printed is relative to THIS field's mean,
    # so it moves when the field does and two gauntlets cannot be compared.
    # Anchoring it on the rated seats is the entire reason the field has rated
    # seats in it, and a step that has to be remembered afterwards is a step
    # that stops happening. An interrupted match still leaves a readable PGN.
    if pgn.exists() and pgn.stat().st_size:
        ratings.main([str(pgn)])
    return code


if __name__ == "__main__":
    sys.exit(main())
