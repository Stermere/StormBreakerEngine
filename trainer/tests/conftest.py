import os
import subprocess
import sys

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "trainer"))
# tools/ too: export_net.py is the quantised reference, and tests/test_export.py
# checks it here rather than waiting for `make nnue-test` to need a compiler, a
# checkpoint and a 50 MB net file to say the same thing.
sys.path.insert(0, os.path.join(REPO, "tools"))


def pytest_addoption(parser):
    parser.addoption(
        "--shard",
        default=os.path.join(REPO, "external", "datagen-test", "all.cnn"),
        help="a shard produced by `make datagen-test`",
    )
    parser.addoption(
        "--datagen",
        default=None,
        help="path to the datagen binary (default: ./datagen[.exe] in the repo root)",
    )


@pytest.fixture(scope="session")
def shard_path(request):
    path = request.config.getoption("--shard")
    if not os.path.exists(path):
        pytest.skip(f"no shard at {path}; run `make datagen-test` first")
    return path


@pytest.fixture(scope="session")
def datagen(request):
    path = request.config.getoption("--datagen")
    if path is None:
        for candidate in ("datagen.exe", "datagen"):
            candidate = os.path.join(REPO, candidate)
            if os.path.exists(candidate):
                path = candidate
                break
    if path is None or not os.path.exists(path):
        pytest.skip("no datagen binary; run `make datagen` first")
    return path


@pytest.fixture(scope="session")
def datagen_dump(datagen, shard_path):
    """`datagen dump` output, parsed. This is the C side of the format contract:
    the FENs here were produced by the same code that wrote the shard."""

    def run(count=256):
        result = subprocess.run(
            [datagen, "dump", shard_path, "-n", str(count)],
            capture_output=True, text=True, check=True,
        )
        lines = result.stdout.strip().splitlines()
        assert lines[0].startswith("fen;"), lines[:2]
        rows = []
        for line in lines[1:]:
            fen, score, wdl, source, in_check, progress = line.split(";")
            rows.append({
                "fen": fen,
                "score": int(score),
                "wdl": int(wdl),
                "source": source,
                "in_check": in_check == "1",
                "progress": int(progress),
            })
        return rows

    return run
