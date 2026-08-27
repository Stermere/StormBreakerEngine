"""
snapshot-baseline.py - freeze the current build as a testing baseline.

    python tools/snapshot-baseline.py --name v0.1
    python tools/snapshot-baseline.py                # names it from the git commit

WHY THIS MATTERS. Elo is only meaningful relative to something fixed. If you
always test "new versus previous", small measurement errors compound in one
direction and the engine can drift backwards while every individual test
"passes". Keep a stable baseline, promote it deliberately, and periodically run
a gauntlet against older baselines to confirm real progress.

The .txt written beside the binary is the point of the exercise as much as the
copy is: six months from now `base-a1b2c3d.exe` means nothing without the
commit and the bench that identify it.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

import common as c

BENCH_LINE = re.compile(r"^\d+ nodes \d+ nps$", re.MULTILINE)


def git(*args: str) -> str:
    try:
        out = subprocess.run(
            ["git", *args], cwd=c.REPO_ROOT, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, timeout=10,
        )
        return out.stdout.strip() if out.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def bench_of(exe: Path) -> str:
    """The engine's own bench line, which is the only identity that survives a
    rename. A baseline whose node count cannot be recovered is a baseline that
    cannot be proved to be the thing a past experiment measured against."""
    try:
        out = subprocess.run(
            [str(exe), "bench"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", timeout=300,
        )
    except (OSError, subprocess.SubprocessError):
        return "(bench failed)"
    m = BENCH_LINE.search(out.stdout)
    return m.group(0) if m else "(bench failed)"


def main() -> int:
    ap = argparse.ArgumentParser(
        prog="snapshot-baseline.py", description="Freeze the build as a baseline."
    )
    ap.add_argument("--name", help="default: base-<short sha>")
    ap.add_argument("--source", help="default: the built stormbreaker")
    args = ap.parse_args()

    source = args.source or c.get_engine_binary()
    c.require(source, "Engine not built. Run 'make' first.")
    source = Path(source).resolve()

    name = args.name
    if not name:
        sha = git("rev-parse", "--short", "HEAD")
        name = f"base-{sha}" if sha else f"base-{c.stamp()}"

    c.ensure_dir(c.BASELINE_DIR)
    dest = c.BASELINE_DIR / f"{name}.exe"
    meta_path = c.BASELINE_DIR / f"{name}.txt"

    # Look at what is about to be replaced rather than just announcing it. A
    # baseline is the fixed end of every comparison made against it, and
    # silently swapping one for a different binary under the same name
    # invalidates every past result that named it.
    if dest.exists():
        c.warn(f"Overwriting existing baseline '{name}'")
        if meta_path.exists():
            for line in meta_path.read_text(encoding="utf-8", errors="replace").splitlines():
                print(f"    old | {line}")
            print()

    shutil.copy2(source, dest)

    meta = "\n".join(
        [
            f"name:    {name}",
            f"source:  {source}",
            f"date:    {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            f"commit:  {git('rev-parse', 'HEAD') or 'unknown'}",
            f"bench:   {bench_of(dest)}",
        ]
    )
    # No BOM: PowerShell 5.1's Set-Content adds one and it breaks naive readers.
    meta_path.write_text(meta + "\n", encoding="utf-8", newline="\n")

    c.ok(f"Baseline saved: {dest}")
    print()
    print(meta)
    print()
    print("Now make your change, rebuild, and run:")
    print(f'  make sprt ARGS="--base {dest}"')
    return 0


if __name__ == "__main__":
    sys.exit(main())
