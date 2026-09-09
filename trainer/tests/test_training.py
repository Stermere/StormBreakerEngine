"""Trainer orchestration and regression tests using tiny synthetic shards."""

import json
from pathlib import Path

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from nnue.dataset import ShardBatches, ShuffledChunks, make_loader  # noqa: E402
from nnue.format import pack_fen  # noqa: E402
from nnue.model import NNUE, TargetPolicy  # noqa: E402
from nnue.provenance import sha256_file  # noqa: E402
from nnue.train import Metrics, evaluate, forever, parse_args, train  # noqa: E402


@pytest.fixture
def shard(tmp_path):
    fen = "8/8/8/8/8/4k3/8/4K3 w - - 0 1"
    records = np.array([pack_fen(fen, score=100 * i, wdl=i % 4, source=src)
                        for i, src in enumerate([5, 5, 0, 5, 2, 0, 5, 5])])
    path = tmp_path / "data.cnn"
    records.tofile(path)
    return path


@pytest.mark.parametrize("workers", [0, 2])
@pytest.mark.parametrize("chunks", [0, 4])
def test_filter_never_returns_an_excluded_source(shard, workers, chunks):
    loader = make_loader(shard, batch_size=2, workers=workers, sources=[0],
                         chunk_records=chunks, shuffle=False)
    batches = list(loader)
    assert sum(len(b["source"]) for b in batches) == 2
    assert all(torch.all(b["source"] == 0) for b in batches)
    if not chunks:
        assert loader.dataset.records == 2
        assert len(loader.dataset) == 2


def test_all_excluded_shard_is_empty_not_a_substitute(shard):
    mapped = ShardBatches(shard, 2, sources=[1])
    assert mapped.records == len(mapped) == 0
    chunked = ShuffledChunks(shard, 2, chunk_records=4, sources=[1])
    assert list(chunked) == []
    with pytest.raises(ValueError, match="no training records"):
        next(forever(chunked))


def test_metrics_use_global_weight_denominators_and_actual_unknown_count():
    policy = TargetPolicy(source_weight={0: 1.0, 5: 3.0})
    batch = {"score": torch.tensor([0., 400., -400.]), "wdl": torch.tensor([1, 2, 3]),
             "source": torch.tensor([0, 5, 5]), "progress": torch.zeros(3, dtype=torch.long),
             "piece_count": torch.full((3,), 2)}
    prediction = torch.tensor([0., 0., 0.])
    target = policy.target(batch, .9)
    whole, split = Metrics("cpu", .05), Metrics("cpu", .05)
    whole.update(prediction, None, batch, target, policy)
    for sl in (slice(0, 1), slice(1, 3)):
        split.update(prediction[sl], None, {k: v[sl] for k, v in batch.items()},
                     target[sl], policy)
    assert split.report() == pytest.approx(whole.report())
    report = whole.report()
    expected = (((.5 - target) ** 2) * torch.tensor([1., 3., 3.])).sum() / 7
    assert report["value"] == pytest.approx(expected.item())
    assert report["total"] == report["value"]
    assert report["known_results"] == 2
    assert report["wdl_mse"] == pytest.approx(.125)


def args_for(shard, out, *extra):
    return parse_args(["--train", str(shard), "--val", str(shard), "--out", str(out),
                       "--hidden", "16", "--output-buckets", "8", "--uncertainty",
                       "--batch-size", "4", "--workers", "0", "--device", "cpu",
                       "--epochs", "1", "--log-every", "0", *extra])


def test_finish_is_a_full_pass_with_frozen_lambda_and_separate_checkpoint(shard, tmp_path, monkeypatch):
    monkeypatch.setattr("nnue.train.sanity.report", lambda *a: None)
    out = tmp_path / "run"
    args = args_for(shard, out, "--feature-factorization", "--positions-per-epoch", "4",
                    "--lambda-start", ".9", "--lambda-end", ".7",
                    "--finish-epochs", "1", "--finish-lr", ".00001")
    train(args)
    history = json.loads(Path(f"{out}-history.json").read_text())
    assert [r["stage"] for r in history] == ["main", "finish"]
    assert [r["positions"] for r in history] == [4, 8]
    assert [r["lambda"] for r in history] == [.9, .7]
    assert history[1]["lr"] == 1e-5
    # Unknown WDLs use lambda=1, including in the reported average.
    assert history[1]["lambda_applied"] == pytest.approx(.775)
    state = torch.load(f"{out}.pt", weights_only=True)
    before = torch.load(f"{out}-pre-finish.pt", weights_only=True)
    assert before["epoch"] == 1 and state["epoch"] == 2
    assert before["run_id"] == state["run_id"]
    assert not torch.equal(before["model"]["out.weight"], state["model"]["out.weight"])
    manifest = json.loads(Path(f"{out}-run.json").read_text())
    assert manifest["checkpoint_sha256"] == sha256_file(f"{out}.pt")
    for row in history:
        for split in ("train", "val"):
            m = row["metrics"][split]
            assert m["total"] == pytest.approx(m["value"] + .05 * m["uncertainty"])
        assert row["train"] == row["metrics"]["train"]["value"]
        assert row["val"] == row["metrics"]["val"]["value"]
    # A completed run is a no-op; a new run must not silently overwrite it.
    train(args_for(shard, out, "--feature-factorization", "--positions-per-epoch", "4",
                   "--lambda-start", ".9", "--lambda-end", ".7",
                   "--finish-epochs", "1", "--resume"))
    with pytest.raises(SystemExit, match="already belongs"):
        train(args)


def test_init_from_adds_zero_factor_and_finishes_without_main_epochs(shard, tmp_path, monkeypatch):
    monkeypatch.setattr("nnue.train.sanity.report", lambda *a: None)
    parent = tmp_path / "parent.pt"
    model = NNUE(16, 8, uncertainty=True)
    torch.save({"model": model.state_dict(), "arch": model.arch}, parent)
    out = tmp_path / "finish"
    train(args_for(shard, out, "--epochs", "0", "--init-from", str(parent),
                   "--feature-factorization", "--finish-epochs", "1"))
    state = torch.load(f"{out}.pt", weights_only=True)
    before = torch.load(f"{out}-pre-finish.pt", weights_only=True)
    assert before["epoch"] == 0 and state["stage"] == "finish"
    assert state["arch"]["feature_factorization"]
    assert torch.count_nonzero(before["model"]["ft_shared.weight"]) == 0
    assert state["provenance"]["parent"]["sha256"] == sha256_file(parent)


@pytest.mark.parametrize("extra", [["--finish-lr", "0"], ["--epochs", "-1"],
                                   ["--epochs", "0"], ["--finish-epochs", "-1"],
                                   ["--sources", "6"], ["--lr", "nan"],
                                   ["--init-from", "x.pt", "--resume"]])
def test_invalid_training_flags_are_rejected(extra):
    with pytest.raises(SystemExit):
        parse_args(["--train", "unused.cnn", *extra])


def test_validation_reports_both_losses_and_restores_model_mode(shard):
    model = NNUE(16, 8, uncertainty=True)
    model.eval()
    report = evaluate(model, make_loader(shard, 4, workers=0, shuffle=False), "cpu",
                      TargetPolicy(), .9, unc_weight=.02)
    assert not model.training
    assert report["positions"] == 8
    assert report["uncertainty"] > 0
    assert report["total"] == pytest.approx(report["value"] + .02 * report["uncertainty"])


def test_interrupted_finish_resumes_at_low_lr_with_adam_moments(shard, tmp_path, monkeypatch):
    import nnue.train as training

    monkeypatch.setattr(training.sanity, "report", lambda *a: None)
    extra = ["--feature-factorization", "--finish-epochs", "2", "--finish-lr", ".00001"]
    control, interrupted = tmp_path / "control", tmp_path / "interrupted"
    train(args_for(shard, control, *extra))
    write = training.write_json

    class Interrupted(Exception):
        pass

    def stop_after_first_finish(path, value):
        write(path, value)
        if str(path).endswith("-run.json") and value.get("epoch") == 2:
            raise Interrupted

    monkeypatch.setattr(training, "write_json", stop_after_first_finish)
    with pytest.raises(Interrupted):
        train(args_for(shard, interrupted, *extra))
    partial = torch.load(f"{interrupted}-resume.pt", weights_only=True)
    assert partial["epoch"] == 2
    assert partial["optimiser"]["param_groups"][0]["lr"] == 1e-5
    assert all(s["step"].item() == 4 for s in partial["optimiser"]["state"].values())
    monkeypatch.setattr(training, "write_json", write)
    train(args_for(shard, interrupted, *extra, "--resume"))
    expected = torch.load(f"{control}.pt", weights_only=True)
    actual = torch.load(f"{interrupted}.pt", weights_only=True)
    for name, weights in expected["model"].items():
        torch.testing.assert_close(actual["model"][name], weights, rtol=0, atol=0)
    history = json.loads(Path(f"{interrupted}-history.json").read_text())
    assert [h["stage"] for h in history] == ["main", "finish", "finish"]
    assert [h["lr"] for h in history[1:]] == [1e-5, 1e-5]
    resumed = torch.load(f"{interrupted}-resume.pt", weights_only=True)
    assert all(s["step"].item() == 6 for s in resumed["optimiser"]["state"].values())
    with pytest.raises(SystemExit, match="finish_lr changed"):
        train(args_for(shard, interrupted, *extra, "--finish-lr", ".00002", "--resume"))


@pytest.mark.parametrize("chunks", [0, 4])
def test_all_excluded_training_and_validation_fail_cleanly(shard, tmp_path, monkeypatch, chunks):
    monkeypatch.setattr("nnue.train.sanity.report", lambda *a: None)
    with pytest.raises(ValueError, match="no records remain"):
        train(args_for(shard, tmp_path / "empty", "--sources", "1", "--chunk-records", str(chunks)))


def test_fixed_validation_metrics_do_not_follow_lambda_or_score_clip(shard):
    model = NNUE(16, 8)
    loader = make_loader(shard, 4, workers=0, shuffle=False)
    first = evaluate(model, loader, "cpu", TargetPolicy(), .95)
    second = evaluate(model, loader, "cpu", TargetPolicy(score_clip=100), .3)
    assert first["value"] != second["value"]
    assert first["score_mse"] == second["score_mse"]
    assert first["wdl_mse"] == second["wdl_mse"]