"""
common.py - shared paths, discovery, the rated ladder, and match-running.

The PowerShell equivalent of this file is common.ps1, which still serves the
Windows-integration scripts (setup, register-engines, gui). Anything with real
logic in it lives here instead: PowerShell 5.1 is the only PowerShell on a
default Windows box, and it has no `&&`, no ternary, no null-coalescing, and a
`ConvertFrom-Json` that hands back a PSCustomObject rather than a dict. Those
are survivable in a wrapper and miserable in a tuner.

STDLIB ONLY, deliberately. These scripts must run on whatever `python` is on
PATH with nothing installed and no virtualenv - see trainer/.venv for the one
place in this repository that legitimately needs its own environment. A tool
that gates an SPRT behind a `pip install` is a tool that stops being run.

Everything here is DISCOVERED rather than hardcoded, so it keeps working after
a winget upgrade moves a binary, and on a different machine.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

# ----------------------------------------------------------------- paths ----

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent
EXTERNAL_DIR = REPO_ROOT / "external"
BOOKS_DIR = EXTERNAL_DIR / "books"
ENGINES_DIR = EXTERNAL_DIR / "engines"
BASELINE_DIR = EXTERNAL_DIR / "baselines"
GAMES_DIR = EXTERNAL_DIR / "games"
TUNE_DIR = EXTERNAL_DIR / "tune"


def ensure_dir(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    return path


# ------------------------------------------------------------- discovery ----


def find_first(candidates) -> str | None:
    """First candidate that exists, or None.

    A candidate is either a bare command name resolved through PATH, or an
    absolute path tried directly.
    """
    for c in candidates:
        if c is None:
            continue
        p = Path(os.path.expandvars(str(c)))
        if p.is_absolute():
            if p.exists():
                return str(p.resolve())
        else:
            found = shutil.which(str(c))
            if found:
                return found
    return None


def get_fastchess() -> str | None:
    """The match runner. winget installs it under the alias `fast-chess`."""
    local = os.environ.get("LOCALAPPDATA", "")
    return find_first(
        [
            "fastchess",
            "fast-chess",
            Path(local) / "Microsoft" / "WinGet" / "Links" / "fast-chess.exe" if local else None,
        ]
    )


def get_stockfish() -> str | None:
    local = os.environ.get("LOCALAPPDATA", "")
    return find_first(
        [
            "stockfish",
            Path(local) / "Microsoft" / "WinGet" / "Links" / "stockfish.exe" if local else None,
        ]
    )


def get_engine_binary() -> str | None:
    """The freshly built default engine."""
    for name in ("stormbreaker.exe", "stormbreaker"):
        p = REPO_ROOT / name
        if p.exists():
            return str(p)
    return None


def get_book() -> str | None:
    for name in ("UHO_Lichess_4852_v1.epd",):
        p = BOOKS_DIR / name
        if p.exists():
            return str(p)
    return None


def latest_baseline() -> str | None:
    """Most recently snapshotted baseline, which is what -Base defaulted to."""
    ensure_dir(BASELINE_DIR)
    exes = sorted(BASELINE_DIR.glob("*.exe"), key=lambda p: p.stat().st_mtime, reverse=True)
    return str(exes[0]) if exes else None


# --------------------------------------------------------------- presets ----

# VSTC finds gross breakage, STC finds most regressions cheaply, LTC catches
# search changes whose benefit only appears at depth.
TC_PRESETS = {"VSTC": "2+0.02", "STC": "8+0.08", "LTC": "40+0.4"}

# elo0 is the null hypothesis (no gain), elo1 the alternative (a gain worth
# keeping). Wider bounds resolve faster but wave through smaller regressions.
BOUND_PRESETS = {"VSTC": (0.0, 5.0), "STC": (0.0, 5.0), "LTC": (0.5, 4.5)}


def resolve_tc(tc: str) -> tuple[str, str]:
    """Returns (time control string, label). An unrecognised value passes through
    verbatim so `--tc 60+0.6` works without adding a preset for it."""
    key = tc.upper()
    if key in TC_PRESETS:
        return TC_PRESETS[key], key
    return tc, tc


def resolve_bounds(label: str, explicit: str | None) -> tuple[float, float]:
    if explicit:
        parts = [p for p in re.split(r"[,;\s]+", explicit) if p]
        if len(parts) != 2:
            fail(f"--bounds wants two numbers, got '{explicit}'")
            sys.exit(2)
        return float(parts[0]), float(parts[1])
    return BOUND_PRESETS.get(label, (0.0, 5.0))


def default_concurrency() -> int:
    """Leave two cores for the OS and for whatever else is running.
    Oversubscribing distorts every time-based result, which is the fastest way
    to get an SPRT verdict that does not reproduce."""
    return max(1, (os.cpu_count() or 4) - 2)


# ---------------------------------------------------------------- ladder ----

# The rated opponent ladder. `fetch-engines.py` downloads it into ENGINES_DIR
# and `ratings.py` reads the ratings back off it, so the two cannot disagree
# about which engine carries which number.
#
# WHY THESE SEVEN. They span 3008-3593 in roughly 100-Elo steps, which brackets
# the engine on both sides - a field entirely above or entirely below it scores
# near 0% or near 100%, which carries almost no information per game. They come
# from seven different authors, so a quirk this engine happens to exploit in one
# of them cannot flatter the whole table. All are self-contained: no companion
# net file to place, no config to write.
#
# RATINGS ARE CCRL BLITZ (2+1), single CPU, as published for these exact
# versions, with CCRL's own published error bar in `err`. They are not this
# repository's scale and not FIDE's; a rating list is a pool, and ours is not
# CCRL's pool. Treat them as calibrated landmarks, not as a number to claim.
#
# `member` names the file to pull out of a .zip asset; None means the asset is
# the executable itself.
GH = "https://github.com"

CCRL_LADDER = [
    {
        "name": "halogen-8.1",
        "ccrl": 3008,
        "err": 18,
        "url": f"{GH}/KierenP/Halogen/releases/download/v8.1/Halogen8.1-x64-pext-avx2.exe",
        "member": None,
    },
    {
        "name": "berserk-4.1.0",
        "ccrl": 3133,
        "err": 17,
        "url": f"{GH}/jhonnold/berserk/releases/download/4.1.0/berserk-4.1.0-x64-avx2-pext.exe",
        "member": None,
    },
    {
        "name": "weiss-1.4",
        "ccrl": 3256,
        "err": 16,
        "url": f"{GH}/TerjeKir/weiss/releases/download/v1.4/Weiss-1.4-windows-collection.zip",
        "member": "weiss-pext.exe",
    },
    {
        "name": "clover-3.0",
        "ccrl": 3340,
        "err": 16,
        "url": f"{GH}/lucametehau/CloverEngine/releases/download/v3.0/Clover.3.0-avx2.exe",
        "member": None,
    },
    {
        "name": "ethereal-12.75",
        "ccrl": 3426,
        "err": 11,
        "url": f"{GH}/AndyGrant/Ethereal/releases/download/v12.75/Ethereal12.75-x64-pext-avx2.exe",
        "member": None,
    },
    {
        "name": "carp-3.0.1",
        "ccrl": 3528,
        "err": 9,
        "url": f"{GH}/dede1751/carp/releases/download/v3.0.1/carp-v3.0.1-win-x86_64-V3.exe",
        "member": None,
    },
    {
        "name": "koivisto-8.0",
        "ccrl": 3593,
        "err": 10,
        "url": f"{GH}/Luecx/Koivisto/releases/download/v8.0/Koivisto_8.0-x64-windows-avx2.exe",
        "member": None,
    },
]


def ccrl_rating(name: str) -> tuple[int, int] | None:
    """(rating, published error) for a ladder engine, or None if unrated here.

    Matches on the stem the gauntlet names a seat after, so it works on a PGN
    without needing the binaries to still be on disk.
    """
    for e in CCRL_LADDER:
        if e["name"] == name:
            return e["ccrl"], e["err"]
    return None

# ---------------------------------------------------------------- output ----

_COLOR = sys.stdout.isatty() and os.environ.get("NO_COLOR") is None


def _c(code: str, text: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _COLOR else text


def section(title: str) -> None:
    print()
    print(_c("36", f"== {title}"))


def ok(msg: str) -> None:
    print(_c("32", f"  [ok]   {msg}"))


def warn(msg: str) -> None:
    print(_c("33", f"  [warn] {msg}"))


def fail(msg: str) -> None:
    print(_c("31", f"  [fail] {msg}"))


def stamp() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


# ------------------------------------------------------------ match args ----


def book_args(book: str | None) -> list[str]:
    """`-repeat` elsewhere gives both engines each opening from both colours.
    Without a book, games start from the initial position and repeat heavily,
    which makes every result unreliable rather than merely slower."""
    if not book:
        warn("No opening book found - games will start from the initial position.")
        warn("This produces duplicate games and unreliable results. Run tools\\setup.ps1.")
        return []
    return ["-openings", f"file={Path(book).resolve()}", "format=epd", "order=random"]


def engine_args(cmd: str, name: str, options: dict | None = None) -> list[str]:
    args = ["-engine", f"cmd={Path(cmd).resolve()}", f"name={name}"]
    for k, v in (options or {}).items():
        args.append(f"option.{k}={v}")
    return args


def match_args(
    *,
    engines: list[list[str]],
    tc: str,
    rounds: int,
    concurrency: int,
    pgn: Path | None,
    hash_mb: int = 16,
    threads: int = 1,
    book: str | None = None,
    extra: list[str] | None = None,
    depth: int | None = None,
) -> list[str]:
    """The argument vector shared by every match this repository runs.

    `-recover` restarts an engine that crashes rather than aborting the match:
    a young engine WILL crash, and losing 20000 games to it is avoidable.

    `depth` replaces the clock with a fixed-depth search. It is a DIAGNOSTIC
    mode and not a way to measure strength: at a fixed depth, searching more
    nodes is free, so every pruning parameter is rewarded for pruning less and
    a tuner pointed at it will happily walk them all in the wrong direction.
    Useful precisely because that direction is known in advance.
    """
    args: list[str] = []
    for e in engines:
        args += e
    args += [
        "-each",
        f"depth={depth}" if depth else f"tc={tc}",
        f"option.Hash={hash_mb}",
        f"option.Threads={threads}",
        "-rounds",
        str(rounds),
        "-games",
        "2",
        "-repeat",
        "-concurrency",
        str(concurrency),
        "-recover",
    ]
    if pgn is not None:
        args += ["-pgnout", f"file={pgn}"]
    args += book_args(book)
    args += extra or []
    return args


# --------------------------------------------------------------- running ----


def print_command(exe: str, args: list[str]) -> None:
    section("fastchess command")
    print(f"  {exe} " + " ".join(args))
    print()


def run_match(exe: str, args: list[str], *, stream: bool = True) -> tuple[int, str]:
    """Runs fastchess. Returns (exit code, captured stdout).

    `stream=True` echoes as it goes, which is what a human watching an SPRT
    wants; the tuner captures instead, because it runs thousands of these and
    only reads the final tally.
    """
    if stream:
        proc = subprocess.Popen(
            args=[exe] + args,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        chunks = []
        assert proc.stdout is not None
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            chunks.append(line)
        proc.wait()
        return proc.returncode, "".join(chunks)

    proc = subprocess.run(
        args=[exe] + args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return proc.returncode, proc.stdout


# --------------------------------------------------------------- parsing ----

_RE_GAMES = re.compile(
    r"Games:\s*(\d+),\s*Wins:\s*(\d+),\s*Losses:\s*(\d+),\s*Draws:\s*(\d+),\s*"
    r"Points:\s*([\d.]+)\s*\(([\d.]+)\s*%\)"
)
_RE_ELO = re.compile(r"Elo:\s*(-?[\d.]+)\s*\+/-\s*([\d.]+)")
_RE_LLR = re.compile(r"LLR:\s*(-?[\d.]+)")


class MatchResult:
    """The last result block fastchess printed.

    Parsed from stdout rather than from the PGN because fastchess computes the
    pentanomial statistics itself and re-deriving them here would be a second
    implementation of the same arithmetic to keep in agreement.
    """

    def __init__(self, games=0, wins=0, losses=0, draws=0, points=0.0, elo=None, llr=None):
        self.games = games
        self.wins = wins
        self.losses = losses
        self.draws = draws
        self.points = points
        self.elo = elo
        self.llr = llr

    @property
    def score(self) -> float:
        """Score rate in [0, 1] from the first engine's point of view."""
        return self.points / self.games if self.games else 0.5

    def __repr__(self) -> str:
        return (
            f"MatchResult(games={self.games}, W{self.wins}/L{self.losses}/D{self.draws}, "
            f"score={self.score:.4f}, elo={self.elo}, llr={self.llr})"
        )


def parse_result(output: str) -> MatchResult:
    """Reads the LAST result block, since fastchess prints one periodically."""
    r = MatchResult()

    matches = list(_RE_GAMES.finditer(output))
    if matches:
        m = matches[-1]
        r.games = int(m.group(1))
        r.wins = int(m.group(2))
        r.losses = int(m.group(3))
        r.draws = int(m.group(4))
        r.points = float(m.group(5))

    elos = list(_RE_ELO.finditer(output))
    if elos:
        r.elo = float(elos[-1].group(1))

    llrs = list(_RE_LLR.finditer(output))
    if llrs:
        r.llr = float(llrs[-1].group(1))

    return r


def require(value, message: str):
    """Exits with a diagnostic rather than a traceback. A missing binary is a
    setup problem, and a stack trace is the wrong way to report one."""
    if not value:
        fail(message)
        sys.exit(1)
    return value
