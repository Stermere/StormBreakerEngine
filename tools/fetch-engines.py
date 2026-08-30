"""
fetch-engines.py - download a ladder of rated opponents for the gauntlet.

    python tools/fetch-engines.py            # fetch any that are missing
    python tools/fetch-engines.py --list     # just show the ladder
    python tools/fetch-engines.py --force    # re-download everything

gauntlet.py's problem is that every engine in external/baselines is a snapshot
of THIS engine, so the table it prints is relative to a scale nobody outside
this repository recognises. These opponents are third-party engines with
published CCRL ratings, which turns the same table into an absolute reading:
beating the 3256 rung and losing to the 3426 one says where the engine sits
without going through Stockfish's UCI_Elo ladder and its caveats (rating.py).

WHY THESE FIVE. They span 3008-3426 in ~100 Elo steps, which brackets the
engine on both sides - a field entirely above or entirely below it produces
near-0% or near-100% scores, which carry almost no information per game. They
come from five different authors, so a quirk this engine happens to exploit in
one of them cannot flatter the whole table. All are self-contained: no
companion net file to place, no config to write.

RATINGS ARE CCRL BLITZ (2+1), single CPU, as published for these exact
versions. They are not this repository's scale and not FIDE's; a rating list
is a pool, and ours is not CCRL's pool. Treat them as calibrated landmarks,
not as a number to claim.

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

GH = "https://github.com"

# `member` names the file to pull out of a .zip asset; None means the asset is
# the executable itself. `ccrl` is CCRL Blitz, 1 CPU, for this exact version.
LADDER = [
    {
        "name": "halogen-8.1",
        "ccrl": 3008,
        "url": f"{GH}/KierenP/Halogen/releases/download/v8.1/Halogen8.1-x64-pext-avx2.exe",
        "member": None,
    },
    {
        "name": "berserk-4.1.0",
        "ccrl": 3133,
        "url": f"{GH}/jhonnold/berserk/releases/download/4.1.0/berserk-4.1.0-x64-avx2-pext.exe",
        "member": None,
    },
    {
        "name": "weiss-1.4",
        "ccrl": 3256,
        "url": f"{GH}/TerjeKir/weiss/releases/download/v1.4/Weiss-1.4-windows-collection.zip",
        "member": "weiss-pext.exe",
    },
    {
        "name": "clover-3.0",
        "ccrl": 3340,
        "url": f"{GH}/lucametehau/CloverEngine/releases/download/v3.0/Clover.3.0-avx2.exe",
        "member": None,
    },
    {
        "name": "ethereal-12.75",
        "ccrl": 3426,
        "url": f"{GH}/AndyGrant/Ethereal/releases/download/v12.75/Ethereal12.75-x64-pext-avx2.exe",
        "member": None,
    },
]


def download(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def main() -> int:
    ap = argparse.ArgumentParser(prog="fetch-engines.py", description="Fetch gauntlet opponents.")
    ap.add_argument("--list", action="store_true", help="show the ladder and exit")
    ap.add_argument("--force", action="store_true", help="re-download even if present")
    args = ap.parse_args()

    dest_dir = c.ensure_dir(c.ENGINES_DIR)

    c.section("Opponent ladder (CCRL Blitz, 1 CPU)")
    for e in LADDER:
        p = dest_dir / f"{e['name']}.exe"
        print(f"  {e['ccrl']:>5}  {e['name']:<16} {'present' if p.exists() else 'missing'}")
    print()
    if args.list:
        return 0

    failed = []
    for e in LADDER:
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
