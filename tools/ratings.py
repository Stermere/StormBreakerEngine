"""
ratings.py - turn a gauntlet PGN into a cross-table and an absolute rating.

    python tools/ratings.py                          # newest gauntlet PGN
    python tools/ratings.py external/games/X.pgn
    python tools/ratings.py --focus engine
    python tools/ratings.py --anchor clover-3.0=3340 # rate an unlisted seat

`make gauntlet` prints one Elo column, and that column is relative to the field
mean - it moves when the field changes, which makes two gauntlets with
different opponents incomparable. This reads the PGN back and does the two
things that column cannot.

FIRST, the cross-table: W-L-D for every pairing, not just totals. A field
average hides the shape of the result, and the shape is where the information
is - an engine that beats the bottom three by more than its rating predicts and
loses to the top two by more than its rating predicts is not "average", it is
mis-scaled, and only the per-opponent rows show that.

SECOND, the anchor. Every engine in common.py's CCRL_LADDER carries a published
CCRL Blitz rating. Fitting the whole round-robin at once (Bradley-Terry by
maximum likelihood, which is what Ordo and BayesElo do) gives every seat a
rating on one internal scale; sliding that scale until the rated seats sit on
their published numbers puts the unrated ones - this engine, and any baseline -
on the CCRL scale too. That is strictly more than "implied rating against each
rung, averaged": the rungs' games against EACH OTHER constrain the fit as well,
so an anchor that underperforms here is visibly the odd one out instead of
silently dragging the mean.

WHAT THE NUMBER IS NOT. CCRL is a pool, and this is not that pool: different
hardware, a different time control, a different book, and seven opponents
instead of six hundred. The offset is fitted to THESE seven engines, so it
inherits whatever those seven happen to do here. The error bar covers sampling
noise and CCRL's own published error; it does not cover that difference, which
is systematic and unmeasurable from inside. Quote the number as "on the CCRL
Blitz scale", never as "a CCRL rating".

STDLIB ONLY, like every tool here - see common.py.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path

import common as c

LOG10 = math.log(10.0)
SCALE = 400.0 / LOG10  # natural-log Bradley-Terry units -> Elo

# Every bar this tool prints is 95%, because both numbers it has to sit next
# to already are: fastchess's +/- and CCRL's published +/- are both two-sigma.
# Mixing a one-sigma bar into that comparison understates one side by half.
Z95 = 1.959964

TAG = re.compile(r'\[(\w+)\s+"(.*)"\]')
POINTS = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}


# --------------------------------------------------------------- parsing ----


def read_games(path: Path) -> list[dict]:
    """Header-only PGN read: every field this tool needs is a tag, and the
    movetext of a 20000-game gauntlet is most of the file."""
    games, cur = [], {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = TAG.match(line.strip())
            if not m:
                continue
            key, val = m.group(1), m.group(2)
            if key == "Event" and cur:
                games.append(cur)
                cur = {}
            cur[key] = val
    if cur:
        games.append(cur)
    # `*` is an abandoned game - a crash, or a match stopped mid-round. It is
    # not a draw and must not be scored as one.
    return [g for g in games if g.get("Result") in POINTS and g.get("White") and g.get("Black")]


class Table:
    """Everything the fit and the printout need, in one pass over the games."""

    def __init__(self, games: list[dict]):
        names = sorted({g["White"] for g in games} | {g["Black"] for g in games})
        self.names = names
        self.idx = {n: i for i, n in enumerate(names)}
        p = len(names)
        self.n = [[0] * p for _ in range(p)]  # games played
        self.pts = [[0.0] * p for _ in range(p)]  # points scored by row vs column
        self.wld = [[[0, 0, 0] for _ in range(p)] for _ in range(p)]  # W-L-D, row's view
        # Paired openings: fastchess plays each opening twice with the colours
        # reversed, and the two games share a Round. They are one draw from the
        # distribution, not two, so the variance has to be taken over the pair.
        self.groups: dict[tuple, list] = {}

        for g in games:
            i, j = self.idx[g["White"]], self.idx[g["Black"]]
            s = POINTS[g["Result"]]
            self.n[i][j] += 1
            self.n[j][i] += 1
            self.pts[i][j] += s
            self.pts[j][i] += 1.0 - s
            self.wld[i][j][0 if s == 1.0 else 1 if s == 0.0 else 2] += 1
            self.wld[j][i][0 if s == 0.0 else 1 if s == 1.0 else 2] += 1
            lo, hi = (i, j) if i < j else (j, i)
            key = (g.get("Round", ""), lo, hi)
            # Stored from the low-index player's point of view so the two games
            # of a pair add up regardless of who had white in which.
            self.groups.setdefault(key, []).append(s if i == lo else 1.0 - s)

    def played(self, i: int) -> int:
        return sum(self.n[i])

    def score(self, i: int) -> float:
        return sum(self.pts[i])


# ------------------------------------------------------------------- fit ----


def fit_bradley_terry(t: Table, prior: float) -> list[float]:
    """Maximum-likelihood Elo for the whole round-robin at once.

    Minorisation-maximisation (Zermelo's iteration): it needs no derivatives,
    cannot overshoot, and converges monotonically, which matters more here than
    speed - the alternative is shipping a line search nobody will ever tune.

    `prior` adds that many virtual drawn games to every pairing that actually
    met. It is 0 unless some seat scored 0% or 100%, where the likelihood has
    no maximum at all and a rating of -inf would otherwise be printed as if it
    were a measurement.
    """
    p = len(t.names)
    gamma = [1.0] * p
    wins = [
        t.score(i) + prior * sum(1 for j in range(p) if j != i and t.n[i][j]) / 2.0
        for i in range(p)
    ]
    for _ in range(10000):
        worst = 0.0
        for i in range(p):
            denom = 0.0
            for j in range(p):
                if j != i and t.n[i][j]:
                    denom += (t.n[i][j] + prior) / (gamma[i] + gamma[j])
            if denom <= 0.0 or wins[i] <= 0.0:
                continue
            new = wins[i] / denom
            worst = max(worst, abs(math.log(new) - math.log(gamma[i])))
            gamma[i] = new
        mean = sum(math.log(g) for g in gamma) / p
        gamma = [g / math.exp(mean) for g in gamma]
        if worst < 1e-12:
            break
    return [math.log(g) for g in gamma]


def expected(theta: list[float], i: int, j: int) -> float:
    d = theta[i] - theta[j]
    if d > 60:
        return 1.0
    if d < -60:
        return 0.0
    return 1.0 / (1.0 + math.exp(-d))


def covariance(t: Table, theta: list[float]) -> list[list[float]]:
    """Sandwich covariance of the fitted ratings, in Elo^2.

    The naive Fisher information treats a game as a coin flip with variance
    p(1-p), and a chess game is not one: draws make a single game LESS variable
    than that, and the paired opening makes two consecutive games MORE
    correlated than independent. Both corrections come free from measuring the
    spread of the observed pair scores instead of assuming it - the meat of the
    sandwich is empirical, the bread is the model.
    """
    p = len(t.names)
    bread = [[0.0] * p for _ in range(p)]
    for i in range(p):
        for j in range(p):
            if i == j or not t.n[i][j]:
                continue
            e = expected(theta, i, j)
            w = t.n[i][j] * e * (1.0 - e)
            bread[i][i] += w
            bread[i][j] -= w

    meat = [[0.0] * p for _ in range(p)]
    for (_, lo, hi), scores in t.groups.items():
        e = expected(theta, lo, hi)
        resid = sum(scores) - len(scores) * e
        v = resid * resid
        meat[lo][lo] += v
        meat[hi][hi] += v
        meat[lo][hi] -= v
        meat[hi][lo] -= v

    pinv = laplacian_pinv(bread)
    cov = matmul(matmul(pinv, meat), pinv)
    return [[cov[i][j] * SCALE * SCALE for j in range(p)] for i in range(p)]


def matmul(a, b):
    p, q = len(a), len(b[0])
    inner = len(b)
    return [[sum(a[i][k] * b[k][j] for k in range(inner)) for j in range(q)] for i in range(p)]


def laplacian_pinv(a: list[list[float]]) -> list[list[float]]:
    """Pseudo-inverse of a graph Laplacian: (A + J/p)^-1 - J/p.

    A is singular by construction - only rating DIFFERENCES are identified, so
    adding a constant to every rating changes nothing. The J/p term pins the
    mean to zero, which is the normalisation the fit already uses.
    """
    p = len(a)
    k = 1.0 / p
    m = [[a[i][j] + k for j in range(p)] for i in range(p)]
    inv = invert(m)
    return [[inv[i][j] - k for j in range(p)] for i in range(p)]


def invert(m: list[list[float]]) -> list[list[float]]:
    p = len(m)
    aug = [row[:] + [1.0 if i == j else 0.0 for j in range(p)] for i, row in enumerate(m)]
    for col in range(p):
        pivot = max(range(col, p), key=lambda r: abs(aug[r][col]))
        if abs(aug[pivot][col]) < 1e-12:
            c.fail("the field is not connected - some engine never played the rest of it")
            sys.exit(1)
        aug[col], aug[pivot] = aug[pivot], aug[col]
        d = aug[col][col]
        aug[col] = [x / d for x in aug[col]]
        for r in range(p):
            if r == col or aug[r][col] == 0.0:
                continue
            f = aug[r][col]
            aug[r] = [x - f * y for x, y in zip(aug[r], aug[col])]
    return [row[p:] for row in aug]


# ---------------------------------------------------------------- output ----


def elo_from_score(s: float) -> float | None:
    if s <= 0.0 or s >= 1.0:
        return None
    return 400.0 * math.log10(s / (1.0 - s))


def print_crosstable(t: Table) -> None:
    c.section("Cross-table (row engine's W-L-D against column)")
    p = len(t.names)
    w = max(16, max(len(n) for n in t.names) + 2)
    cell = 12
    print(" " * w + "".join(f"{n[: cell - 1]:>{cell}}" for n in t.names) + f"{'score':>9}")
    for i, name in enumerate(t.names):
        row = f"  {name:<{w - 2}}"
        for j in range(p):
            if i == j:
                row += f"{'.':>{cell}}"
            elif not t.n[i][j]:
                row += f"{'-':>{cell}}"
            else:
                win, loss, draw = t.wld[i][j]
                row += f"{f'{win}-{loss}-{draw}':>{cell}}"
        played = t.played(i)
        pct = 100.0 * t.score(i) / played if played else 0.0
        row += f"{pct:>8.1f}%"
        print(row)
    print()


def pair_score_se(t: Table, i: int, j: int) -> float:
    """Standard error of the score RATE in one pairing, from the spread of the
    paired-opening results rather than from a binomial assumption."""
    lo, hi = (i, j) if i < j else (j, i)
    n = t.n[i][j]
    if not n:
        return 0.0
    e = t.pts[i][j] / n
    total = 0.0
    for (_, a, b), scores in t.groups.items():
        if (a, b) != (lo, hi):
            continue
        mine = sum(scores) if i == lo else len(scores) - sum(scores)
        total += (mine - len(scores) * e) ** 2
    return math.sqrt(total) / n


def print_head_to_head(t: Table, focus: str) -> None:
    i = t.idx[focus]
    c.section(f"{focus} against each opponent")
    print(f"  {'opponent':<18}{'CCRL':>7}{'W':>6}{'L':>5}{'D':>5}{'score':>9}{'implied':>16}")
    for j, name in enumerate(t.names):
        if j == i or not t.n[i][j]:
            continue
        win, loss, draw = t.wld[i][j]
        n = t.n[i][j]
        s = t.pts[i][j] / n
        rated = c.ccrl_rating(name)
        d = elo_from_score(s)
        implied = ""
        if rated and d is not None:
            slope = 400.0 / (LOG10 * max(s * (1.0 - s), 1e-6))
            sigma = math.hypot(pair_score_se(t, i, j) * slope, rated[1] / Z95)
            implied = f"{rated[0] + d:.0f} +-{Z95 * sigma:.0f}"
        elif rated:
            implied = ("above " if s >= 1.0 else "below ") + f"{rated[0]}"
        ccrl = f"{rated[0]:>7}" if rated else f"{'-':>7}"
        print(f"  {name:<18}{ccrl}{win:>6}{loss:>5}{draw:>5}{100.0 * s:>8.1f}%{implied:>16}")
    print()
    print("  implied = that rung's CCRL rating plus the Elo this score alone implies.")
    print("  One rung is one datapoint; the fitted table below uses all of them at once.")
    print()


def print_ratings(t: Table, theta: list[float], cov, anchors: dict[str, tuple[int, int]],
                  bounded: set[str]) -> None:
    p = len(t.names)
    elo = [x * SCALE for x in theta]
    se = [math.sqrt(max(cov[i][i], 0.0)) for i in range(p)]
    keys = [i for i, n in enumerate(t.names) if n in anchors]

    # Inverse-variance weights: an anchor is worth trusting in proportion to how
    # well BOTH of its numbers are known - its CCRL rating and its result here.
    weight = {i: 1.0 / (se[i] ** 2 + (anchors[t.names[i]][1] / Z95) ** 2) for i in keys}
    total_w = sum(weight.values())
    offset = 0.0
    if keys:
        offset = sum(weight[i] * (anchors[t.names[i]][0] - elo[i]) for i in keys) / total_w
    u = {i: weight[i] / total_w for i in keys}

    def anchored_se(i: int) -> float:
        # estimate_i = elo_i - sum_k u_k elo_k + sum_k u_k ccrl_k, so the
        # statistical part is the variance of that contrast, not of elo_i.
        coef = [0.0] * p
        coef[i] += 1.0
        for k in keys:
            coef[k] -= u[k]
        var = sum(coef[a] * cov[a][b] * coef[b] for a in range(p) for b in range(p))
        var += sum((u[k] * anchors[t.names[k]][1] / Z95) ** 2 for k in keys)
        return math.sqrt(max(var, 0.0))

    c.section("Ratings on the CCRL Blitz scale (bars are 95%)")
    if not keys:
        c.warn("no rated engine in this field - the internal column is all there is.")
    print(f"  {'engine':<18}{'games':>7}{'internal':>14}{'CCRL':>8}{'estimate':>16}{'resid':>8}")
    for i in sorted(range(p), key=lambda k: -elo[k]):
        rated = anchors.get(t.names[i])
        est = elo[i] + offset
        resid = f"{est - rated[0]:>+8.0f}" if rated else f"{'':>8}"
        ccrl = f"{rated[0]:>8}" if rated else f"{'-':>8}"
        internal = f"{elo[i]:+.0f} +-{Z95 * se[i]:.0f}"
        estimate = f"{est:.0f} +-{Z95 * anchored_se(i):.0f}" if keys else ""
        # A seat that swept or was swept has no maximum-likelihood rating at
        # all, only a bound - and a bound printed with a tight-looking bar
        # beside six real measurements is exactly how it gets read as one.
        mark = " (bound)" if t.names[i] in bounded else ""
        print(f"  {t.names[i]:<18}{t.played(i):>7}{internal:>14}{ccrl}{estimate:>16}"
              f"{resid}{mark}")
    print()
    if not keys:
        return

    # Offset-only assumes the two pools stretch Elo the same way. They need not:
    # a faster time control and a narrower field both compress the spread, and a
    # compressed spread biases every estimate away from the anchors' centre. The
    # fitted slope is the check, and it costs one line to print.
    mr = sum(weight[i] * elo[i] for i in keys) / total_w
    mc = sum(weight[i] * anchors[t.names[i]][0] for i in keys) / total_w
    num = sum(weight[i] * (elo[i] - mr) * (anchors[t.names[i]][0] - mc) for i in keys)
    den = sum(weight[i] * (elo[i] - mr) ** 2 for i in keys)
    slope = num / den if den > 0 else float("nan")
    rms = math.sqrt(sum((elo[i] + offset - anchors[t.names[i]][0]) ** 2 for i in keys) / len(keys))
    print(f"  offset {offset:+.0f} Elo, fitted on {len(keys)} anchors, residual rms {rms:.0f} Elo")
    print(f"  anchor slope {slope:.2f}  (1.00 = this field stretches Elo exactly as CCRL's does)")
    print("  resid is what an anchor's own games here say about it, minus its published")
    print("  rating; a rung far from 0 is one this field disagrees with.")
    print("  The bar covers sampling noise and CCRL's published error, NOT the difference")
    print("  between the two pools - see the module docstring before quoting a number.")
    print()


# ------------------------------------------------------------------ main ----


def newest_pgn() -> Path | None:
    c.ensure_dir(c.GAMES_DIR)
    pool = sorted(c.GAMES_DIR.glob("*-gauntlet.pgn")) or sorted(c.GAMES_DIR.glob("*.pgn"))
    pool = [p for p in pool if p.stat().st_size > 0]
    return max(pool, key=lambda p: p.stat().st_mtime) if pool else None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="ratings.py", description="Rate a gauntlet PGN.")
    ap.add_argument("pgn", nargs="?", help="PGN to read; default is the newest gauntlet")
    ap.add_argument("--focus", default="engine", help="engine to break out per opponent")
    ap.add_argument("--anchor", action="append", default=[], metavar="NAME=ELO[+-ERR]",
                    help="treat NAME as rated; repeatable, overrides the ladder")
    ap.add_argument("--prior", type=float, default=0.0,
                    help="virtual drawn games per pairing; raised automatically if a "
                         "seat scored 0%% or 100%%")
    ap.add_argument("--no-crosstable", action="store_true")
    args = ap.parse_args(argv)

    path = Path(args.pgn) if args.pgn else newest_pgn()
    if not path or not path.exists():
        c.fail("no PGN to read. Run 'make gauntlet' first, or name one.")
        return 1

    games = read_games(path)
    if not games:
        c.fail(f"no finished games in {path}")
        return 1
    t = Table(games)
    if len(t.names) < 2:
        c.fail("a rating table needs at least two engines")
        return 1

    anchors: dict[str, tuple[int, int]] = {}
    for name in t.names:
        rated = c.ccrl_rating(name)
        if rated:
            anchors[name] = rated
    for spec in args.anchor:
        m = re.fullmatch(r"([^=]+)=(-?\d+)(?:\+-(\d+))?", spec.strip())
        if not m:
            c.fail(f"--anchor wants NAME=ELO or NAME=ELO+-ERR, got '{spec}'")
            return 2
        anchors[m.group(1)] = (int(m.group(2)), int(m.group(3) or 20))

    c.section("Gauntlet ratings")
    print(f"  pgn      {path}")
    print(f"  games    {len(games)}")
    print(f"  engines  {len(t.names)}  ({len(anchors)} rated)")
    print()

    if not args.no_crosstable:
        print_crosstable(t)
    if args.focus in t.idx:
        print_head_to_head(t, args.focus)
    elif args.focus:
        c.warn(f"--focus {args.focus} did not play in this PGN; skipping that table.")

    prior = args.prior
    degenerate = [n for i, n in enumerate(t.names)
                  if t.score(i) == 0.0 or t.score(i) == float(t.played(i))]
    if degenerate and prior == 0.0:
        prior = 1.0
        c.warn(f"{', '.join(degenerate)} scored 0% or 100%: no finite rating exists.")
        c.warn("Adding one virtual drawn game per pairing; that seat's number is a bound.")

    theta = fit_bradley_terry(t, prior)
    cov = covariance(t, theta)
    print_ratings(t, theta, cov, anchors, set(degenerate))
    return 0


if __name__ == "__main__":
    sys.exit(main())
