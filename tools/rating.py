"""
rating.py - estimate the engine's ABSOLUTE rating against a calibrated ladder.

    python tools/rating.py
    python tools/rating.py --levels 2600,2800,3000 --games 200 --tc LTC

sprt.py answers "is this better than the last version?" and gauntlet.py
answers "how does it do against a field". Neither produces a number on a scale
anyone outside this repository recognises, because every opponent in
external/baselines is itself unrated. This plays the engine against Stockfish
at several UCI_Elo settings and fits one rating to the results.

HOW MUCH TO TRUST THE NUMBER. Less than its confidence interval suggests.

  - UCI_Elo is Stockfish's own calibration against its own reference pool. It
    is approximate, and it is not the CCRL or FIDE scale even though it is
    expressed in the same units.
  - A strength-limited engine is not the same thing as a genuinely weaker one.
    Stockfish at UCI_Elo 2600 plays mostly 3600-strength moves with occasional
    deliberate errors; a real 2600 engine is uniformly weaker. Those two error
    distributions are not interchangeable.
  - Ratings are time-control specific. A number measured at 8+0.08 is a blitz
    number and should not be compared to a 40/15 list.

Treat the result as "roughly this class, on this ladder, at this time control".
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path

import common as c


def tally_from_pgn(pgn: Path, levels: list[int]) -> dict:
    """Counts results per opponent out of the PGN.

    fastchess prints a ranking table for a gauntlet rather than a "Results of X
    vs Y" block per pairing, and that table's Score column is against the whole
    field. The PGN is exact and does not depend on how the runner chooses to
    format its summary.
    """
    tally = {f"SF-{lvl}": {"level": lvl, "w": 0, "l": 0, "d": 0} for lvl in levels}
    if not pgn.exists():
        return tally

    white = black = ""
    tag = re.compile(r'^\[(White|Black|Result)\s+"(.*)"\]')
    with pgn.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            m = tag.match(line.strip())
            if not m:
                continue
            key, val = m.group(1), m.group(2)
            if key == "White":
                white = val
            elif key == "Black":
                black = val
            else:
                if white == "engine":
                    opp, engine_is_white = black, True
                elif black == "engine":
                    opp, engine_is_white = white, False
                else:
                    continue
                if opp not in tally:
                    continue
                if val == "1-0":
                    tally[opp]["w" if engine_is_white else "l"] += 1
                elif val == "0-1":
                    tally[opp]["l" if engine_is_white else "w"] += 1
                elif val == "1/2-1/2":
                    tally[opp]["d"] += 1
    return tally


def main() -> int:
    ap = argparse.ArgumentParser(prog="rating.py", description="Absolute rating vs a SF ladder.")
    ap.add_argument("--engine")
    ap.add_argument("--levels", default="2200,2400,2600,2800,3000")
    ap.add_argument("--games", type=int, default=100, help="games per level")
    ap.add_argument("--tc", default="STC")
    ap.add_argument("--concurrency", type=int, default=0)
    ap.add_argument("--hash", type=int, default=16, dest="hash_mb")
    ap.add_argument("--pgn", help="skip the matches and re-score an existing PGN")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    levels = sorted({int(x) for x in re.split(r"[,;\s]+", args.levels) if x})
    if not levels:
        c.fail("no valid --levels given")
        return 1

    tc, tc_label = c.resolve_tc(args.tc)

    if args.pgn:
        pgn = Path(args.pgn)
    else:
        fastchess = c.require(c.get_fastchess(), "fastchess not found. Run: powershell -File tools\\setup.ps1")
        sf = c.require(c.get_stockfish(), "Stockfish not found. Run: powershell -File tools\\setup.ps1")
        engine = args.engine or c.get_engine_binary()
        c.require(engine, "Engine not built. Run 'make' first.")
        engine = str(Path(engine).resolve())

        concurrency = args.concurrency if args.concurrency > 0 else c.default_concurrency()

        engines = [c.engine_args(engine, "engine")]
        for lvl in levels:
            # UCI_Elo is ignored unless UCI_LimitStrength is on - setting one
            # without the other silently gives you a full-strength Stockfish,
            # which is exactly the mistake that makes an engine look 400 points
            # weaker than it is.
            engines.append(
                c.engine_args(sf, f"SF-{lvl}", {"UCI_LimitStrength": "true", "UCI_Elo": lvl})
            )

        c.ensure_dir(c.GAMES_DIR)
        pgn = c.GAMES_DIR / f"{c.stamp()}-rating.pgn"

        fc_args = c.match_args(
            engines=engines,
            tc=tc,
            rounds=max(1, args.games // 2),
            concurrency=concurrency,
            pgn=pgn,
            hash_mb=args.hash_mb,
            book=c.get_book(),
            # gauntlet, not roundrobin: the ladder rungs playing each other
            # would cost most of the games and tell us nothing about us.
            extra=["-tournament", "gauntlet"],
        )

        c.section("Rating estimate")
        print(f"  engine       {engine}")
        print(f"  ladder       Stockfish UCI_Elo {', '.join(str(l) for l in levels)}")
        print(f"  time control {tc}  ({tc_label})")
        print(f"  games        {args.games} per level ({args.games * len(levels)} total)")
        print(f"  pgn          {pgn}")
        print()

        if args.dry_run:
            c.print_command(fastchess, fc_args)
            return 0

        code, _ = c.run_match(fastchess, fc_args, stream=True)
        if code != 0:
            c.warn(f"fastchess exited {code}; scoring whatever reached the PGN")

    # ------------------------------------------------------------ estimate --
    #
    # Each level gives an independent estimate of our rating:
    #
    #     R = level + 400 * log10(S / (1 - S))
    #
    # with standard error 400 / (ln10 * sqrt(S(1-S)N)). Levels we sweep or get
    # swept by have S near 0 or 1, where that error explodes - which is correct,
    # since such a result genuinely does not locate us. Combining by inverse
    # variance therefore weights the informative rungs automatically.

    tally = tally_from_pgn(pgn, levels)
    rows = [(t["level"], t["w"], t["l"], t["d"]) for t in tally.values() if t["w"] + t["l"] + t["d"]]
    if not rows:
        c.warn("No games found to score.")
        return 0

    c.section("Per-level estimates")
    print("  level    games    W-L-D          score     implied rating")
    print("  -----    -----    ---------      -----     --------------")

    wsum = vsum = 0.0
    for level, w, l, d in sorted(rows):
        n = w + l + d
        s = (w + 0.5 * d) / n
        wld = f"{w}-{l}-{d}"
        if s <= 0.0 or s >= 1.0:
            note = "lost every game" if s <= 0.0 else "won every game"
            print(f"  {level:>5}    {n:>5}    {wld:<9}      {s:>5.1%}     -- ({note})")
            continue

        est = level + 400.0 * math.log10(s / (1.0 - s))
        se = 400.0 / (math.log(10) * math.sqrt(s * (1 - s) * n))
        wsum += est / (se * se)
        vsum += 1.0 / (se * se)
        print(f"  {level:>5}    {n:>5}    {wld:<9}      {s:>5.1%}     {est:>6.0f} +/- {1.96 * se:.0f}")

    print()
    if vsum <= 0:
        c.warn("Every level was a sweep in one direction; re-run with a bracket that fits.")
        return 0

    combined = wsum / vsum
    ci = 1.96 / math.sqrt(vsum)
    c.ok(f"Combined estimate: {combined:.0f} +/- {ci:.0f} Elo  ({tc}, Stockfish UCI_Elo ladder)")
    print()
    c.warn("The interval is statistical only. It excludes the ladder's own")
    c.warn("calibration error, which is larger. Sanity check that the score")
    c.warn("falls monotonically as the level rises - if it does not, the")
    c.warn("ladder is not engaging and the number is meaningless.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
