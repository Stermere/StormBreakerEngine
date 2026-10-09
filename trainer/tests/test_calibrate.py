"""The scale diagnostics recover scales they are handed, and run end to end on a shard."""

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from nnue.calibrate import evaluate_shard, fit_scale, main, refit  # noqa: E402
from nnue.format import pack_fen  # noqa: E402
from nnue.model import NNUE  # noqa: E402


def test_fit_scale_recovers_a_proportional_label_scale():
    pred = np.linspace(-900, 900, 3001)
    for factor in (0.891, 1.0, 1.135):
        assert fit_scale(pred, factor * pred) == pytest.approx(factor, abs=0.003)


def test_fit_scale_leaves_proven_scores_out():
    pred = np.linspace(-500, 500, 1001)
    score = 1.2 * pred
    score[::50] = 31000  # mates: on no scale at all
    assert fit_scale(pred, score) == pytest.approx(1.2, abs=0.003)


def test_refit_finds_an_underconfident_net_and_never_scores_worse_than_raw():
    rng = np.random.default_rng(0)
    pred = rng.normal(0, 300, 20000)
    target = 1 / (1 + np.exp(-1.1 * pred / 400))
    wdl = rng.integers(0, 4, 20000)  # 3 is "unknown" and must be left out of the WDL MSE
    m = refit(pred, target, 1.1 * pred, wdl)
    assert m["scale"] == pytest.approx(1.1, abs=0.003)
    assert m["value"] <= m["raw_value"]
    assert m["score_mse"] < 1e-6
    assert not m["edge"]


@pytest.fixture
def shard(tmp_path):
    fens = ["8/8/8/8/8/4k3/8/4K3 w - - 0 1", "4k3/8/8/8/8/8/8/R3K3 w Q - 0 1",
            "4k3/8/8/8/8/8/8/R3K3 b Q - 0 1"]
    records = np.array([pack_fen(fens[i % 3], score=50 * i - 100, wdl=i % 4, source=0)
                        for i in range(10)])
    path = tmp_path / "s.cnn"
    records.tofile(path)
    return path


def test_evaluate_shard_reads_every_record_once(shard):
    d = evaluate_shard(NNUE(16, 8), str(shard), "cpu", batch_size=4)
    assert len(d["pred"]) == len(d["score"]) == len(d["wdl"]) == 10
    assert d["score"].tolist() == [50.0 * i - 100 for i in range(10)]
    assert len(evaluate_shard(NNUE(16, 8), str(shard), "cpu", records=3)["pred"]) == 3


def test_both_commands_run(shard, tmp_path, capsys):
    net = tmp_path / "n.pt"
    model = NNUE(16, 8)
    torch.save({"model": model.state_dict(), "arch": model.arch}, net)
    main(["--device", "cpu", "scale", str(net), str(shard), str(shard)])
    assert "1.000x the first" in capsys.readouterr().out
    main(["--device", "cpu", "refit", str(shard), str(net)])
    assert str(net) in capsys.readouterr().out
