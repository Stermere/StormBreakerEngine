"""Scale diagnostics for shards and checkpoints - the two offline checks of E52.

    python -m nnue.calibrate scale NET.pt SHARD [SHARD ...] [--records 2M]
    python -m nnue.calibrate refit VAL.cnn NET.pt [NET.pt ...]

``scale`` measures each shard's LABEL SCALE, with a trained net as the ruler.
Every datagen generation labels positions with the net it had, and a net trained
on this repository's target comes out ~13% more confident than its labels - so
gen-006-u's scores are 1.135x gen-006's for the same advantage, and gen-005's
0.891x. Mixed raw, the net can fit none of them and its scale drifts toward
whichever shard the last chunks came from. The ratios this prints, relative to
the first shard, are what ``--score-scale`` takes (inverted: a shard at 0.891x
wants ``SHARD=1.122``). They do not depend much on which net is the ruler; E52
got the same ratios to 0.5% from three different ones.

``refit`` scores checkpoints on a validation shard after refitting each one's
eval scale. Raw validation loss mostly measures how confident a net happens to
be that epoch, not how well it orders positions - E52's nets swung +-3% between
epochs on a 0.1% weight change. Refitted, the WDL column (the eval as a
predictor of the game result) put five SPRT'd nets in Elo order at ~23 Elo per
0.001. It is a screen, not a verdict: it stopped tracking Elo below a base
lambda of ~0.6, and it cannot see a coverage gain from data unlike the
validation set. Use it to decide what is worth an SPRT.

Both read records straight off a memmap in the main process - no DataLoader,
so no worker processes to spawn - and only the first ``--records`` of each
shard, which is a fair sample because shards are shuffled on disk.
"""

from __future__ import annotations

import argparse
import math

import numpy as np
import torch

from .dataset import PROVEN_SCORE, _sparse_tensors, to_device
from .format import NET_TO_CP, RECORD_DTYPE
from .model import TargetPolicy, from_checkpoint

# A sample big enough that the fitted scale is stable to the third digit.
DEFAULT_RECORDS = 2_000_000
# Wide enough for every generation measured so far (0.79 .. 1.20), fine enough
# that grid steps are below the noise.
SCALES = np.linspace(0.6, 1.6, 401)
K = 400.0


def _sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def fit_scale(pred_cp: np.ndarray, score_cp: np.ndarray, clip: float = 2000.0) -> float:
    """The a minimising MSE(sigmoid(a * pred / K), sigmoid(clip(score) / K)).

    In win-probability space rather than centipawns, so a few positions at
    +1500 do not decide the fit - the same reason the training loss is there.
    Proven scores (mates, tablebase wins) are left out: they are on no
    engine's scale, which is also why ``--score-scale`` leaves them alone.
    """
    keep = np.abs(score_cp) < PROVEN_SCORE
    pred_cp, score_cp = pred_cp[keep], score_cp[keep]
    t = _sigmoid(np.clip(score_cp, -clip, clip) / K)
    errors = [np.mean((_sigmoid(a * pred_cp / K) - t) ** 2) for a in SCALES]
    return float(SCALES[int(np.argmin(errors))])


def refit(pred_cp: np.ndarray, target: np.ndarray, score_cp: np.ndarray,
          wdl: np.ndarray) -> dict:
    """Validation metrics at the eval scale that best fits ``target``.

    ``target`` is the blended training target; the scale is fitted to it, and
    the fixed score and WDL MSEs are then read at that scale - the definition
    E52's numbers use.
    """
    errors = [np.mean((_sigmoid(a * pred_cp / K) - target) ** 2) for a in SCALES]
    i = int(np.argmin(errors))
    p = _sigmoid(SCALES[i] * pred_cp / K)
    known = wdl <= 2
    result = np.minimum(wdl, 2).astype(np.float64) / 2.0
    return {
        "scale": float(SCALES[i]),
        "raw_value": float(np.mean((_sigmoid(pred_cp / K) - target) ** 2)),
        "value": float(errors[i]),
        "score_mse": float(np.mean((p - _sigmoid(score_cp / K)) ** 2)),
        "wdl_mse": float(np.mean(((p - result) ** 2)[known])) if known.any() else math.nan,
        "edge": i in (0, len(SCALES) - 1),
    }


@torch.no_grad()
def evaluate_shard(model, path: str, device, records: int | None = None,
                   batch_size: int = 16384, policy: TargetPolicy | None = None,
                   lam: float = 0.95) -> dict:
    """Eval (cp), raw score, WDL and - with a policy - the blended target, per record."""
    data = np.memmap(path, dtype=RECORD_DTYPE, mode="r")
    n = len(data) if not records else min(records, len(data))
    out = {"pred": [], "score": [], "wdl": [], "target": []}
    model.eval()
    for lo in range(0, n, batch_size):
        batch = to_device(_sparse_tensors(np.array(data[lo:min(lo + batch_size, n)])), device)
        pred = (model.forward_heads_sparse(batch)[0] if model.uncertainty
                else model.forward_sparse(batch))
        out["pred"].append((pred * NET_TO_CP).double().cpu().numpy())
        out["score"].append(batch["score"].double().cpu().numpy())
        out["wdl"].append(batch["wdl"].cpu().numpy())
        if policy is not None:
            out["target"].append(policy.target(batch, lam).double().cpu().numpy())
    return {k: np.concatenate(v) if v else None for k, v in out.items()}


def _load(path: str, device):
    ck = torch.load(path, map_location="cpu", weights_only=False)
    return from_checkpoint(ck).to(device)


def _records(text: str) -> int:
    text = text.strip().lower().replace("_", "")
    scale = {"k": 10 ** 3, "m": 10 ** 6, "b": 10 ** 9}.get(text[-1:], 1)
    return int(float(text[:-1] if scale > 1 else text) * scale)


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    s = sub.add_parser("scale", help="each shard's label scale, relative to the first")
    s.add_argument("net", help="checkpoint used as the ruler")
    s.add_argument("shards", nargs="+")
    s.add_argument("--records", type=_records, default=DEFAULT_RECORDS)
    r = sub.add_parser("refit", help="validation metrics at each net's best-fitting scale")
    r.add_argument("val", help="validation shard")
    r.add_argument("nets", nargs="+")
    r.add_argument("--records", type=_records, default=0, help="0: the whole shard")
    r.add_argument("--lambda", dest="lam", type=float, default=0.95)
    r.add_argument("--lambda-progress", type=float, default=-0.2)
    r.add_argument("--score-clip", type=int, default=2000)
    parser.add_argument("--device", default=None)
    args = parser.parse_args(argv)
    device = torch.device(args.device or ("cuda" if torch.cuda.is_available() else "cpu"))

    if args.command == "scale":
        model = _load(args.net, device)
        found = []
        for path in args.shards:
            d = evaluate_shard(model, path, device, args.records)
            found.append(fit_scale(d["pred"], d["score"]))
        ref = found[0]
        print(f"ruler: {args.net}  ({args.records:,} records per shard)")
        for path, a in zip(args.shards, found):
            print(f"  {path:<50} a {a:.4f}   {a / ref:.3f}x the first   "
                  f"--score-scale factor to match it: {ref / a:.3f}")
        return

    policy = TargetPolicy(sigmoid_k=K, score_clip=args.score_clip,
                          progress_delta=args.lambda_progress).to(device)
    print(f"{'net':<40} {'raw':>9} {'scale':>6} {'refit':>9} {'score':>9} {'wdl':>8}")
    for net in args.nets:
        d = evaluate_shard(_load(net, device), args.val, device, args.records or None,
                           policy=policy, lam=args.lam)
        m = refit(d["pred"], d["target"], d["score"], d["wdl"])
        edge = "  (scale at the grid's edge)" if m["edge"] else ""
        print(f"{net:<40} {m['raw_value']:9.6f} {m['scale']:6.3f} {m['value']:9.6f} "
              f"{m['score_mse']:9.6f} {m['wdl_mse']:8.5f}{edge}", flush=True)


if __name__ == "__main__":
    main()
