"""Small, explicit identities for training runs and the files they produce."""

from __future__ import annotations

import hashlib
import json
import math
import os
import subprocess
from pathlib import Path


def sha256_file(path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value) -> None:
    # Legacy checkpoints/history used NaN for missing validation. Keep those
    # readable while writing standards-compliant JSON rather than bare NaN.
    def clean(obj):
        if isinstance(obj, float) and not math.isfinite(obj):
            return None
        if isinstance(obj, dict):
            return {k: clean(v) for k, v in obj.items()}
        if isinstance(obj, (list, tuple)):
            return [clean(v) for v in obj]
        return obj

    tmp = f"{path}.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(clean(value), f, indent=2, allow_nan=False)
        f.write("\n")
    os.replace(tmp, path)


def dataset_identity(paths) -> list:
    """Inventory, NOT a content hash of a potentially multi-terabyte corpus.

    Hash the small generation manifest when present; name the weaker file
    size/mtime identity explicitly rather than claiming to have hashed the data.
    """
    result = []
    for path in paths or []:
        path = Path(path).resolve()
        stat = path.stat()
        entry = {"path": str(path), "bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns}
        manifest = path.with_suffix(".json")
        if manifest.is_file():
            entry["manifest_sha256"] = sha256_file(manifest)
        result.append(entry)
    return result


def code_identity() -> dict:
    root = Path(__file__).resolve().parents[2]
    result = {}
    try:
        result["commit"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True,
            stderr=subprocess.DEVNULL).strip()
        result["dirty"] = bool(subprocess.check_output(
            ["git", "status", "--porcelain", "--untracked-files=no"], cwd=root,
            text=True, stderr=subprocess.DEVNULL).strip())
    except (OSError, subprocess.CalledProcessError):
        result["commit"] = None
    # A dirty commit name is not enough to identify the code that did the fit.
    result["trainer_sha256"] = {
        str(p.relative_to(root)): sha256_file(p)
        for p in sorted((root / "trainer" / "nnue").glob("*.py"))
    }
    return result