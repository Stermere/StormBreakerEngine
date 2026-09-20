"""
fetch-engines.py - download a ladder of rated opponents for the gauntlet.

    python tools/fetch-engines.py            # fetch any that are missing
    python tools/fetch-engines.py --list     # just show the ladder
    python tools/fetch-engines.py --force    # re-download everything
    python tools/fetch-engines.py --prune    # also delete engines off the ladder

gauntlet.py's problem is that every engine in external/baselines is a snapshot
of THIS engine, so the table it prints is relative to a scale nobody outside
this repository recognises. These opponents are third-party engines with
published CCRL ratings, which turns the same table into an absolute reading:
beating the 3256 rung and losing to the 3593 one says where the engine sits
without going through Stockfish's UCI_Elo rungs and their caveats - a ladder
this repository no longer keeps, now that `--field stockfish` plays the real
thing.

The ladder itself - who is on it, what they are rated, and why those - lives in
common.py as CCRL_LADDER, because `ratings.py` has to read the same ratings
back when it anchors a gauntlet table. This file is only how the binaries
arrive.

STDLIB ONLY, like every tool here - see common.py.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

import common as c

# A browser User-Agent is not cargo-culting: GitHub serves release assets from
# a CDN that 403s urllib's default agent.
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"


def download(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def main() -> int:
    ap = argparse.ArgumentParser(prog="fetch-engines.py", description="Fetch gauntlet opponents.")
    ap.add_argument("--list", action="store_true", help="show the ladder and exit")
    ap.add_argument("--force", action="store_true", help="re-download even if present")
    ap.add_argument("--prune", action="store_true",
                    help="delete binaries in the directory that are not on the ladder")
    args = ap.parse_args()

    dest_dir = c.ensure_dir(c.ENGINES_DIR)

    c.section("Opponent ladder (CCRL Blitz, 1 CPU)")
    for e in c.CCRL_LADDER:
        p = dest_dir / f"{e['name']}.exe"
        print(f"  {e['ccrl']:>5} +-{e['err']:<3} {e['name']:<18}"
              f" {'present' if p.exists() else 'missing'}")
    print()

    # Binaries left over from an older ladder still match gauntlet.py's glob,
    # so a field can quietly be the one it used to be. ENGINES_DIR is this
    # script's directory and nothing else writes to it, but deleting on a fetch
    # is still not this command's job unless asked.
    strays = sorted(p for p in dest_dir.glob("*.exe")
                    if not any(e["name"] == p.stem for e in c.CCRL_LADDER))
    if strays:
        c.warn(f"{len(strays)} binary(s) here are NOT on the ladder and will still "
               "play in the gauntlet, unrated:")
        for p in strays:
            print(f"    {p.name}" + ("  -> deleting" if args.prune else ""))
        if args.prune:
            for p in strays:
                p.unlink()
        else:
            print("    keep them deliberately, or drop them: --prune")
        print()

    if args.list:
        return 0

    failed = []
    for e in c.CCRL_LADDER:
        dest = dest_dir / f"{e['name']}.exe"
        if dest.exists() and not args.force:
            print(f"  have  {e['name']}")
            continue
        print(f"  get   {e['name']} ... ", end="", flush=True)
        try:
            blob = download(e["url"])
            if e["member"]:
                with zipfile.ZipFile(io.BytesIO(blob)) as z:
                    names = [n for n in z.namelist() if n.endswith(e["member"])]
                    if not names:
                        raise KeyError(f"{e['member']} not in {[n for n in z.namelist()]}")
                    blob = z.read(names[0])
            # Write via .part so an interrupted download cannot leave behind a
            # truncated .exe that looks present and then fails mid-match.
            part = dest.with_suffix(".part")
            part.write_bytes(blob)
            part.replace(dest)
            print(f"{len(blob) / 1024:.0f} KiB  sha256:{hashlib.sha256(blob).hexdigest()[:12]}")
        except (urllib.error.URLError, OSError, KeyError, zipfile.BadZipFile) as ex:
            print(f"FAILED: {ex}")
            failed.append(e["name"])

    print()
    if failed:
        c.fail(f"could not fetch: {', '.join(failed)}")
        return 1
    print(f"  -> {dest_dir}")
    print("  gauntlet.py picks these up automatically; 'make gauntlet' to use them.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
