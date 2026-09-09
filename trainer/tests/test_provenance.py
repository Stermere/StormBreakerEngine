"""An export must identify its checkpoint, not merely a reused file name."""

import json
import sys

import pytest

torch = pytest.importorskip("torch")

import export_net  # noqa: E402
from nnue.model import NNUE  # noqa: E402
from nnue.provenance import dataset_identity, sha256_file, write_json  # noqa: E402
from nnue.train import parse_args, train  # noqa: E402
from nnue.format import pack_fens  # noqa: E402


def test_export_records_hashes_and_refuses_a_stale_export(tmp_path, monkeypatch):
    model = NNUE(16, 8, uncertainty=True, feature_factorization=True)
    checkpoint = tmp_path / "test.pt"
    state = {"model": model.state_dict(), "arch": model.arch, "epoch": 1,
             "run_id": "test-run", "stage": "finish"}
    torch.save(state, checkpoint)
    output = tmp_path / "test.nnue"
    fens = tmp_path / "test.fens"
    fens.write_text("8/8/8/8/8/4k3/8/4K3 w - - 0 1\n")
    argv = ["export_net", str(checkpoint), "--out", str(output), "--fens", str(fens)]
    monkeypatch.setattr(sys, "argv", argv)
    export_net.main()
    manifest = json.loads(output.with_suffix(".json").read_text())
    assert manifest["checkpoint"] == str(checkpoint.resolve())
    assert manifest["checkpoint_sha256"] == sha256_file(checkpoint)
    assert manifest["sha256"] == sha256_file(output)
    assert manifest["run_id"] == "test-run"
    assert manifest["feature_factorization"]
    assert manifest["stage"] == "finish"
    # Repeating exactly the same export is safe.
    export_net.main()
    state["epoch"] = 2
    torch.save(state, checkpoint)
    with pytest.raises(SystemExit, match="different or unverified"):
        export_net.main()
    monkeypatch.setattr(sys, "argv", [*argv, "--overwrite"])
    export_net.main()
    manifest = json.loads(output.with_suffix(".json").read_text())
    assert manifest["checkpoint_sha256"] == sha256_file(checkpoint)
    # Replacing only the binary must not leave the old manifest looking valid.
    output.write_bytes(b"different net")
    monkeypatch.setattr(sys, "argv", argv)
    with pytest.raises(SystemExit, match="different or unverified"):
        export_net.main()


def test_dataset_inventory_names_the_manifest_hash_not_a_data_hash(tmp_path):
    shard = tmp_path / "data.cnn"
    shard.write_bytes(b"example")
    manifest = shard.with_suffix(".json")
    manifest.write_text('{"records": 1}')
    identity = dataset_identity([shard])[0]
    assert identity["path"] == str(shard.resolve())
    assert identity["bytes"] == 7
    assert identity["manifest_sha256"] == sha256_file(manifest)
    assert "sha256" not in identity


def test_resume_rejects_mixed_checkpoint_and_optimizer(tmp_path, monkeypatch):
    monkeypatch.setattr("nnue.train.sanity.report", lambda *a: None)
    shard = tmp_path / "data.cnn"
    pack_fens(["8/8/8/8/8/4k3/8/4K3 w - - 0 1"]).tofile(shard)
    out = tmp_path / "run"
    argv = ["--train", str(shard), "--out", str(out), "--epochs", "1",
            "--hidden", "16", "--workers", "0", "--device", "cpu"]
    train(parse_args(argv))
    saved = torch.load(f"{out}.pt", weights_only=True)
    saved["epoch"] += 1
    torch.save(saved, f"{out}.pt")
    with pytest.raises(SystemExit, match="different saves"):
        train(parse_args([*argv, "--resume"]))


def test_legacy_missing_metrics_become_json_null(tmp_path):
    path = tmp_path / "metrics.json"
    write_json(path, {"history": [{"val": float("nan")}], "max": float("inf")})
    assert json.loads(path.read_text()) == {"history": [{"val": None}], "max": None}