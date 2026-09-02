"""
fetch-syzygy.py - download the 3-4-5-man Syzygy tablebases.

    python tools/fetch-syzygy.py             # fetch whatever is missing (~939 MB)
    python tools/fetch-syzygy.py --list      # show what is present / missing
    python tools/fetch-syzygy.py --wdl-only  # just the .rtbw half (~378 MB)

The engine probes these when `SyzygyPath` (UCI) or `-syzygy` (datagen) points
at the directory; nothing links against them and a clean clone has none. WDL
(.rtbw) answers won/drawn/lost inside the search; DTZ (.rtbz) is what converts
a won 5-man ending at the root under the fifty-move rule - fetch both unless
disk is the constraint, because WDL-only play can shuffle a tablebase win into
a fifty-move draw, which is the exact failure the tables exist to prevent.

Each file is verified against the byte size the mirror's own index publishes,
and is downloaded via a .part rename so an interrupted run never leaves a
truncated table looking present. The prober validates each table's magic and
geometry at load, so size plus that check is the integrity story. Re-running fetches only
what is missing or the wrong size.

STDLIB ONLY, like every tool here - see common.py.
"""

from __future__ import annotations

import argparse
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

import common as c

# The lichess.org mirror: stable, fast, and its autoindex publishes the byte
# size of every table, which is what --list and the integrity check read.
MIRROR = "https://tablebase.lichess.ovh/tables/standard"
SETS = {"wdl": "3-4-5-wdl", "dtz": "3-4-5-dtz"}
TABLE_COUNT = 145  # 3-4-5 men is exactly this many endgames per set

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"

DEST = c.EXTERNAL_DIR / "syzygy" / "3-4-5"

# href, then whitespace-padded date and size columns - nginx autoindex format.
INDEX_RE = re.compile(r'href="(K[A-Z]*vK[A-Z]*\.rtb[wz])".*?(\d+)\s*$', re.MULTILINE)


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=300) as r:
        return r.read()


def read_index(set_dir: str) -> dict[str, int]:
    """Table name -> byte size, from the mirror's own directory listing."""
    html = fetch(f"{MIRROR}/{set_dir}/").decode("utf-8", "replace")
    tables = {name: int(size) for name, size in INDEX_RE.findall(html)}
    if len(tables) != TABLE_COUNT:
        c.fail(f"{set_dir} index lists {len(tables)} tables, expected {TABLE_COUNT} - "
               "the mirror layout changed; do not trust a partial set")
        sys.exit(1)
    return tables


def main() -> int:
    ap = argparse.ArgumentParser(prog="fetch-syzygy.py",
                                 description="Fetch 3-4-5-man Syzygy tablebases.")
    ap.add_argument("--list", action="store_true", help="show status and exit")
    ap.add_argument("--wdl-only", action="store_true",
                    help="skip the DTZ half (the engine then cannot convert at the root)")
    ap.add_argument("--dest", type=Path, default=DEST, help=f"target directory ({DEST})")
    args = ap.parse_args()

    sets = ["wdl"] if args.wdl_only else ["wdl", "dtz"]

    c.section("Syzygy 3-4-5")
    try:
        want: dict[str, tuple[str, int]] = {}  # name -> (set dir, size)
        for s in sets:
            for name, size in read_index(SETS[s]).items():
                want[name] = (SETS[s], size)
    except (urllib.error.URLError, OSError) as ex:
        c.fail(f"could not read the mirror index: {ex}")
        return 1

    dest = c.ensure_dir(args.dest)
    missing = {n: v for n, v in want.items()
               if not (dest / n).exists() or (dest / n).stat().st_size != v[1]}
    total_mb = sum(size for _, size in want.values()) / 2**20
    missing_mb = sum(size for _, size in missing.values()) / 2**20

    print(f"  {len(want) - len(missing)}/{len(want)} tables present in {dest}")
    print(f"  missing: {len(missing)} files, {missing_mb:.0f} of {total_mb:.0f} MiB")
    if args.list or not missing:
        if not missing:
            print("  complete. Point SyzygyPath / -syzygy at the directory above.")
        return 0

    done = 0
    for name, (set_dir, size) in sorted(missing.items()):
        done += 1
        print(f"  [{done}/{len(missing)}] {name} ({size / 2**20:.1f} MiB) ... ",
              end="", flush=True)
        try:
            blob = fetch(f"{MIRROR}/{set_dir}/{name}")
            if len(blob) != size:
                raise OSError(f"got {len(blob)} bytes, index says {size}")
            part = dest / (name + ".part")
            part.write_bytes(blob)
            part.replace(dest / name)
            print("ok")
        except (urllib.error.URLError, OSError) as ex:
            print(f"FAILED: {ex}")
            c.fail("fetch interrupted - re-run to resume; nothing partial was kept")
            return 1

    print(f"  -> {dest}")
    print("  point SyzygyPath (UCI) or -syzygy (datagen) at this directory.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
