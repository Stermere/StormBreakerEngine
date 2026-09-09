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

While it runs, a panel redraws in place with the field's running table (see
progress.py): games, score, and both Elo columns - internal, and the CCRL-scale
estimate - refitted every few seconds by ratings.py's own model, so the display
cannot come to disagree with the table printed at the end. It is still a
PROGRESS display: early ratings are noise, a seat that has not yet lost has a
bound rather than a rating (marked `*`), and the reading that gets quoted is
the one ratings.py prints from the finished PGN. `--raw` streams fastchess's
own output instead.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

import common as c
import progress
import ratings


class LiveRatings:
    """Refits the field's ratings periodically, for the panel.

    A refit is a Bradley-Terry fit over the whole field plus a matrix
    inversion. That is nothing at eight seats and not free several times a
    second, so it happens on a timer and only once enough new games have
    arrived to move anything.
    """

    def __init__(self, interval: float = 5.0, min_new: int = 4, min_games: int = 12):
        self.interval = interval
        self.min_new = min_new
        self.min_games = min_games
        self.rows: list = []
        self.info: dict = {}
        self._at = 0.0
        self._count = -1

    def due(self) -> None:
        """Make the next update() refit, whatever the timer says."""
        self._at = 0.0
        self._count = -1

    def update(self, games: list[dict]) -> tuple[list, dict]:
        now = time.time()
        fresh = len(games) - self._count
        if (len(games) >= self.min_games
                and fresh >= self.min_new
                and now - self._at >= self.interval):
            self._at, self._count = now, len(games)
            try:
                self.rows, self.info = ratings.rate_games(games)
            except Exception:
                # A live display is not worth ending a gauntlet over: a fit that
                # will not converge on this many games converges on the next
                # batch, and ratings.py re-does all of it from the PGN anyway.
                self.rows, self.info = [], {}
        return self.rows, self.info


def panel(m: c.MatchMonitor, live_ratings: "LiveRatings", *,
          seats: int, tc: str, tc_label: str) -> list[str]:
    """Overall progress, plus the running table.

    The per-seat rows are counted from fastchess's `Finished game` lines rather
    than from its periodic blocks, because a round robin prints one block per
    PAIRING and never a view of the whole field. The two Elo columns are
    ratings.py's own - the same fit `make ratings` prints when the match ends,
    run over the games so far. They move, and early ones are noise; the reading
    that gets quoted is the one at the end, off the PGN.
    """
    rows = [f"== Gauntlet  {seats} seats   {tc} ({tc_label})"]

    played = m.played
    games = f"{played:,}"
    if m.total:
        games = f"{played:,}/{m.total:,} {progress.bar(played / m.total, 12)}"
    timing = f"   elapsed {progress.hms(m.elapsed)}"
    eta = m.eta()
    if eta is not None:
        timing += f"   eta {progress.hms(eta)}"
    rows.append(f"   games   {games}{timing}   {m.games_per_hour():,.0f} games/h")

    if m.pairing:
        rows.append(f"   playing {m.pairing[0]} vs {m.pairing[1]}"
                    + (f"   TIME LOSSES {m.time_losses}" if m.time_losses else ""))

    if not m.seats:
        rows.append("   (waiting for the first finished game ...)")
        return rows

    def score(name: str) -> float:
        wins, losses, draws = m.seats[name]
        total = wins + losses + draws
        return (wins + draws / 2.0) / total if total else 0.0

    rated, info = live_ratings.update(m.games_log)
    # Ranked, and by the strongest measure available: the fitted rating once
    # there is one, raw score until then. Nothing is pinned to the top - the
    # engine under test is a seat in the table like the rest of the field.
    order = [r.name for r in rated if r.name in m.seats]
    order += sorted((n for n in m.seats if n not in order), key=score, reverse=True)
    fitted = {r.name: r for r in rated}

    width = max(12, min(24, max(len(name) for name in m.seats)))
    header = f"   {'seat':<{width}} {'games':>7} {'W-L-D':>13} {'score':>7}"
    if rated:
        header += f" {'internal':>13} {'CCRL est':>14}"
    rows.append(header)

    provisional = False
    for name in order:
        wins, losses, draws = m.seats[name]
        line = (f"   {name[:width]:<{width}} {wins + losses + draws:>7,}"
                f" {f'{wins}-{losses}-{draws}':>13} {score(name):>6.1%}")
        r = fitted.get(name)
        if r is not None:
            line += f" {f'{r.internal:+.0f} +-{r.internal_pm:.0f}':>13}"
            if r.estimate is not None:
                # A seat that has not yet lost (or not yet won) has no finite
                # rating, only a bound. Early in a gauntlet that is most of
                # them, and an unmarked bound reads as a measurement.
                mark = "*" if r.bound else " "
                line += f" {f'{r.estimate:.0f} +-{r.estimate_pm:.0f}':>13}{mark}"
                provisional |= r.bound
        rows.append(line)

    if rated and info.get("anchors"):
        note = (f"   anchored on {info['anchors']} rated seat(s), offset "
                f"{info['offset']:+.0f}; the CCRL column is a scale, not a CCRL rating")
        rows.append(note)
    elif rated:
        rows.append("   no rated seat in this field - internal Elo is all there is")
    if provisional:
        rows.append("   * bound, not a rating: that seat has not lost or not won a game yet")
    return rows


def run_watched(fastchess: str, fc_args: list[str], **panel_kwargs) -> tuple[int, str]:
    """The match behind a panel; anomalies still scroll above it."""
    monitor = c.MatchMonitor(record_games=True)
    live_ratings = LiveRatings()
    live = progress.Live()

    def draw(force: bool = False) -> None:
        live.update(panel(monitor, live_ratings, **panel_kwargs), force=force)

    def on_line(line: str) -> None:
        scroll = monitor.feed(line)
        if scroll:
            live.log(f"  [fastchess] {scroll}")
        draw()

    draw(force=True)
    try:
        return c.run_match(fastchess, fc_args, on_line=on_line)
    finally:
        # One last fit so the closing frame reflects every game played, not
        # whatever the refit timer last allowed.
        live_ratings.due()
        live.finish(panel(monitor, live_ratings, **panel_kwargs))


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
    ap.add_argument("--raw", action="store_true",
                    help="stream fastchess's output instead of the live panel")
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

    if args.raw:
        code, output = c.run_match(fastchess, fc_args, stream=True)
    else:
        code, output = run_watched(
            fastchess, fc_args,
            seats=len(engines), tc=tc, tc_label=tc_label,
        )
        if not pgn.exists() or not pgn.stat().st_size:
            # Nothing was played, and the panel is not a place to look for why.
            c.fail(f"no games were recorded (exit {code}).")
            for line in output.strip().splitlines()[-15:]:
                print("    " + line)

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
