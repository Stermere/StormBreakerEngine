"""Quantise a trained checkpoint into a .nnue file, and write the test vectors
that prove src/nnue.c reproduces it exactly.

    python tools/export_net.py external/nets/net.pt

Two outputs and one gate:

  * ``<out>.nnue``     the weights the engine embeds, with a header describing
                       its own architecture (see src/nnue.h)
  * ``<out>.vectors``  ``<raw> <cp> <fen>`` for N positions, computed here in
                       numpy integer arithmetic
  * ``<out>.sha256``   the hash the engine prints, so a bench node count can be
                       attributed to a specific net

``make nnue-test`` then runs the engine over the vectors and requires EXACT
equality on every line. Exact, not close: the quantised network is integer
arithmetic and integer arithmetic is reproducible. A tolerance would be a bug
generator - a disagreement of one means something rounds differently, and that
something is worth about 30 Elo by the time anyone notices it in a game.

WHY THIS IS A SECOND IMPLEMENTATION, NOT A SHARED ONE. The forward pass below
is written in numpy from the header's own fields, deliberately without
consulting torch. If both sides called the same code the test would prove only
that the code equals itself. The point is that two people writing the same
specification in two languages disagree exactly where the specification is
ambiguous, and every place they disagree here - the truncating division, the
clamp bounds, the perspective order - is a place the engine could have been
silently wrong.

UPGRADING THE NETWORK. Everything architectural is read out of the checkpoint
and written into the header, so a wider retrain needs no change here and none
in the engine. A new activation or feature set needs a case in both this file
and src/nnue.c, and the enums in src/nnue.h say where.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "trainer"))

from nnue.format import (  # noqa: E402
    MAX_PIECES,
    NET_TO_CP,
    NUM_FEATURES,
    PAD_INDEX,
    pack_fens,
    read_shard,
    record_to_fen,
    unpack,
)

# ------------------------------------------------------------------ format --

MAGIC = b"CKNNUE\0\0"
FORMAT_VERSION = 1

FEATURES_HALFKA_8BUCKET = 1
ACT_CRELU = 0

# 8 bytes magic, eight u32, one i32 (scale), one u32 (payload), 32-byte tag,
# 16 reserved. Must stay identical to NnueHeader in src/nnue.h, which carries a
# _Static_assert on its size for exactly this reason.
HEADER_FMT = "<8s8IiI32s16s"
HEADER_BYTES = 96
assert struct.calcsize(HEADER_FMT) == HEADER_BYTES

# Defaults from docs/NNUE.md. SCALE is not free to choose here: the trainer's
# NET_TO_CP already fixed it, so that a target computed by the trainer means
# what a target computed by tools/tuner.c means.
DEFAULT_QA = 255
DEFAULT_QB = 64
DEFAULT_SCALE = int(NET_TO_CP)

# Paired with NNUE_EVAL_LIMIT in src/nnue.c. Engine policy rather than a
# property of the net, which is why it is not a header field - a static
# evaluation that wanders into mate territory makes the search report forced
# mates that do not exist. The export prints the largest |cp| it actually saw,
# so it is visible when a net starts creeping toward the clamp.
EVAL_LIMIT = 20000


def trunc_div(num: np.ndarray, den: int) -> np.ndarray:
    """Integer division that TRUNCATES toward zero, as C's ``/`` does.

    numpy's ``//`` floors, so ``-7 // 4`` is -2 where C gives -1. Every negative
    evaluation would be one centipawn low, which is to say about half the test
    vectors would fail and the engine would be marginally wrong forever if the
    test had been written with a tolerance.
    """
    return np.sign(num) * (np.abs(num) // den)


# ------------------------------------------------------------ quantisation --


def quantise(state: dict, qa: int, qb: int) -> dict:
    """Round the float weights onto the integer grid the engine reads.

    Rounding is half-to-even (``np.rint``). Which rule is used does not affect
    the C/Python equivalence - only Python ever rounds, the engine just reads
    the stored integers - but it is the sort of thing worth pinning rather than
    inheriting.
    """
    ft_w = state["ft.weight"].detach().cpu().numpy().astype(np.float64)
    ft_b = state["ft_bias"].detach().cpu().numpy().astype(np.float64)
    out_w = state["out.weight"].detach().cpu().numpy().astype(np.float64).reshape(-1)
    out_b = float(state["out.bias"].detach().cpu().numpy().reshape(-1)[0])

    # The padding row exists only so a batch can be a dense (B, 32) matrix. It
    # is pinned to zero in training and is not part of the model.
    assert ft_w.shape[0] == NUM_FEATURES + 1, f"unexpected feature rows {ft_w.shape[0]}"
    assert np.all(ft_w[PAD_INDEX] == 0.0), "the padding embedding drifted off zero in training"
    ft_w = ft_w[:NUM_FEATURES]

    return {
        "ft_w": np.rint(ft_w * qa).astype(np.int64),
        "ft_b": np.rint(ft_b * qa).astype(np.int64),
        "out_w": np.rint(out_w * qb).astype(np.int64),
        "out_b": int(np.rint(out_b * qa * qb)),
        "hidden": int(ft_w.shape[1]),
    }


def check_ranges(q: dict, qa: int) -> dict:
    """Refuse to export a net whose arithmetic could overflow the engine's.

    Three bounds, and the middle one is the one that matters:

    ``int16`` weights
        A stored weight that does not fit is simply a broken file.

    ``int16`` accumulator
        src/nnue.c sums the accumulator in int32 today, so nothing overflows
        there - but the stored accumulator is int16, and the incremental
        version that Task 4 needs will sum in int16 because that is what SIMD
        wants. A position has at most 32 pieces, so bias plus the 32 largest
        positive weights in a column is a SOUND upper bound over every legal
        position, not a sample. Checking it now is what makes the narrower
        future version safe without re-deriving anything.

    ``int32`` output
        255 times the sum of the absolute output weights, which likewise bounds
        every possible input rather than the ones we happened to test.
    """
    limits = {}

    for name, arr in (("ft_w", q["ft_w"]), ("ft_b", q["ft_b"]), ("out_w", q["out_w"])):
        peak = int(np.abs(arr).max())
        if peak > 32767:
            raise SystemExit(
                f"{name} quantises to {peak}, which does not fit int16. The net is "
                f"unusable at this QA/QB; retrain with weight clipping or lower the scale."
            )
        limits[f"{name}_peak"] = peak

    # Sound bound over all legal positions: 32 pieces, so 32 rows contribute.
    ft_w, ft_b = q["ft_w"], q["ft_b"]
    top = np.sort(ft_w, axis=0)
    hi = ft_b + np.clip(top[-MAX_PIECES:], 0, None).sum(axis=0)
    lo = ft_b + np.clip(top[:MAX_PIECES], None, 0).sum(axis=0)
    worst = int(max(np.abs(hi).max(), np.abs(lo).max()))
    limits["accumulator_bound"] = worst
    if worst > 32767:
        raise SystemExit(
            f"the accumulator can reach {worst}, past int16. An incremental accumulator "
            f"would wrap. Retrain with a smaller QA or with weight clipping."
        )

    out_bound = int(abs(q["out_b"]) + qa * int(np.abs(q["out_w"]).sum()))
    limits["output_bound"] = out_bound
    if out_bound > 2**31 - 1:
        raise SystemExit(f"the output sum can reach {out_bound}, past int32.")

    return limits


# --------------------------------------------------------- reference model --


def forward(q: dict, fields: dict, qa: int, qb: int, scale: int, chunk: int = 512) -> tuple:
    """The quantised forward pass, in integers, exactly as src/nnue.c runs it.

    Chunked because ``ft_w[idx]`` materialises (B, 32, hidden): at ten thousand
    positions and 512 wide that is 650 MB, and there is no reason to pay it.
    """
    hidden = q["hidden"]
    white, black, stm = fields["white"], fields["black"], fields["stm"]
    n = len(stm)

    # Row PAD_INDEX was dropped from the exported weights, so pad has to map to
    # an explicit zero rather than to a row that no longer exists.
    table = np.concatenate([q["ft_w"], np.zeros((1, hidden), dtype=np.int64)], axis=0)

    raw = np.empty(n, dtype=np.int64)

    for start in range(0, n, chunk):
        stop = min(start + chunk, n)
        idx_w, idx_b = white[start:stop], black[start:stop]

        acc_w = q["ft_b"] + table[idx_w].sum(axis=1)
        acc_b = q["ft_b"] + table[idx_b].sum(axis=1)

        # The side to move always reads its own accumulator first. Getting this
        # backwards gives a net that plays reasonably and hates its position.
        black_moves = stm[start:stop].astype(bool)[:, None]
        own = np.where(black_moves, acc_b, acc_w)
        other = np.where(black_moves, acc_w, acc_b)

        x = np.concatenate([np.clip(own, 0, qa), np.clip(other, 0, qa)], axis=1)
        raw[start:stop] = q["out_b"] + (x * q["out_w"]).sum(axis=1)

    cp = np.clip(trunc_div(raw * scale, qa * qb), -EVAL_LIMIT, EVAL_LIMIT)
    return raw, cp


# ------------------------------------------------------------- positions ----

# Used when neither a shard nor a FEN file is available. Small, but it still
# covers both perspectives, both mirror halves, every piece type and an empty
# board - which is most of what the feature extraction can get wrong.
FALLBACK_FENS = [
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "8/8/8/4k3/8/8/8/4K3 w - - 0 1",
    "3r1rk1/p3qppp/2bb1n2/1p6/3P4/1B3N2/PP2QPPP/R1B2RK1 w - - 2 18",
    "8/1r3k2/8/2R5/8/5K2/8/8 b - - 0 1",
]


def collect_fens(args) -> list:
    """Test positions, chosen deterministically so the gate is reproducible."""
    if args.fens:
        with open(args.fens, encoding="utf-8") as f:
            fens = [line.strip() for line in f if line.strip() and not line.startswith("#")]
        return fens[: args.count]

    shard = args.positions
    if shard is None:
        for guess in ("external/data/train.cnn", "external/data/all.cnn",
                      "external/data/shard00.cnn", "external/datagen-test/all.cnn"):
            if os.path.exists(guess):
                shard = guess
                break

    if shard is None:
        print("no shard found; falling back to the built-in positions "
              "(pass --positions <shard.cnn> for the full 10k gate)")
        return FALLBACK_FENS

    records = read_shard(shard)
    total = len(records)
    # Evenly spaced rather than random: the same shard always yields the same
    # vectors, so a failure is reproducible without carrying a seed around.
    want = min(args.count, total)
    picks = np.linspace(0, total - 1, want, dtype=np.int64) if want else np.empty(0, np.int64)
    print(f"positions: {want} of {total} from {shard}")
    return [record_to_fen(records[int(i)]) for i in picks]


# ------------------------------------------------------------------ write ---


def write_net(path: str, q: dict, args, tag: str) -> bytes:
    hidden = q["hidden"]
    payload = b"".join(
        [
            q["ft_w"].astype(np.int16).tobytes(order="C"),
            q["ft_b"].astype(np.int16).tobytes(order="C"),
            q["out_w"].astype(np.int16).tobytes(order="C"),
            np.int32(q["out_b"]).tobytes(),
        ]
    )
    expect = NUM_FEATURES * hidden * 2 + hidden * 2 + 2 * hidden * 2 + 4
    assert len(payload) == expect, (len(payload), expect)

    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        FORMAT_VERSION,
        FEATURES_HALFKA_8BUCKET,
        ACT_CRELU,
        NUM_FEATURES,
        hidden,
        1,  # output buckets
        args.qa,
        args.qb,
        args.scale,
        len(payload),
        tag.encode("utf-8")[:32],
        b"\0" * 16,
    )

    blob = header + payload
    with open(path, "wb") as f:
        f.write(blob)
    return blob


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("checkpoint", help="a .pt written by nnue.train")
    parser.add_argument("-o", "--out", default=None,
                        help="output .nnue (default: the checkpoint with a .nnue suffix)")
    parser.add_argument("--positions", default=None,
                        help="a .cnn shard to draw test positions from")
    parser.add_argument("--fens", default=None, help="a file of FENs, one per line, instead")
    parser.add_argument("--count", type=int, default=10000, help="test vectors to write")
    parser.add_argument("--vectors", default=None, help="override the vectors path")
    parser.add_argument("--qa", type=int, default=DEFAULT_QA)
    parser.add_argument("--qb", type=int, default=DEFAULT_QB)
    parser.add_argument("--scale", type=int, default=DEFAULT_SCALE,
                        help="centipawns per unit of float output")
    parser.add_argument("--tag", default=None,
                        help="up to 32 bytes of provenance stamped into the header")
    args = parser.parse_args()

    out = args.out or (os.path.splitext(args.checkpoint)[0] + ".nnue")
    vectors = args.vectors or (out + ".vectors")

    # Imported here so that --help works without a torch install.
    import torch

    state = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    model_state = state["model"]
    q = quantise(model_state, args.qa, args.qb)

    if q["hidden"] != int(state.get("hidden", q["hidden"])):
        raise SystemExit("checkpoint's hidden field disagrees with its own weights")

    limits = check_ranges(q, args.qa)
    tag = args.tag or f"epoch{state.get('epoch', '?')}-h{q['hidden']}"

    blob = write_net(out, q, args, tag)
    digest = hashlib.sha256(blob).hexdigest()
    with open(out + ".sha256", "w", encoding="utf-8") as f:
        f.write(f"{digest}  {os.path.basename(out)}\n")

    print(f"wrote {out}  ({len(blob):,} bytes)")
    print(f"  arch      {NUM_FEATURES} -> {q['hidden']}x2 -> 1, crelu, 8 king buckets")
    print(f"  quant     qa {args.qa}  qb {args.qb}  scale {args.scale}")
    print(f"  peaks     ft_w {limits['ft_w_peak']}  ft_b {limits['ft_b_peak']}  "
          f"out_w {limits['out_w_peak']}  (int16 holds 32767)")
    print(f"  bounds    accumulator |x| <= {limits['accumulator_bound']} (int16), "
          f"output |x| <= {limits['output_bound']} (int32)")
    print(f"  sha256    {digest}")

    # ------------------------------------------------------------ vectors --
    fens = collect_fens(args)
    fields = unpack(pack_fens(fens))
    raw, cp = forward(q, fields, args.qa, args.qb, args.scale)

    with open(vectors, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# {os.path.basename(out)} sha256 {digest}\n")
        f.write("# <raw int32> <centipawns> <fen>  - src/nnue.c must reproduce both exactly\n")
        for value, centipawns, fen in zip(raw, cp, fens):
            f.write(f"{int(value)} {int(centipawns)} {fen}\n")
    print(f"wrote {vectors}  ({len(fens):,} vectors)")

    # How far the quantised net drifted from the float one it came from. Not a
    # gate - the gate is C against these vectors - but a quantisation that has
    # gone badly wrong shows up here first, and in units anyone can judge.
    with torch.no_grad():
        from nnue.model import NNUE

        model = NNUE(hidden=q["hidden"])
        model.load_state_dict(model_state)
        model.eval()
        float_cp = model.evaluate_cp(
            torch.from_numpy(fields["white"]),
            torch.from_numpy(fields["black"]),
            torch.from_numpy(fields["stm"]).float().unsqueeze(1),
        ).numpy()

    drift = np.abs(float_cp - cp)
    print(f"  vs float  mean |diff| {drift.mean():.2f} cp, max {drift.max():.2f} cp")
    print(f"  scores    |cp| max {np.abs(cp).max()} (clamp is {EVAL_LIMIT})")

    manifest = {
        "net": os.path.basename(out),
        "sha256": digest,
        "checkpoint": os.path.basename(args.checkpoint),
        "epoch": state.get("epoch"),
        "val_loss": state.get("val_loss"),
        "format_version": FORMAT_VERSION,
        "features": NUM_FEATURES,
        "hidden": q["hidden"],
        "output_buckets": 1,
        "activation": "crelu",
        "feature_set": "halfka-8bucket",
        "qa": args.qa,
        "qb": args.qb,
        "scale": args.scale,
        "tag": tag,
        "limits": limits,
        "vectors": os.path.basename(vectors),
        "vector_count": len(fens),
        "quantisation_drift_cp": {"mean": float(drift.mean()), "max": float(drift.max())},
    }
    with open(os.path.splitext(out)[0] + ".json", "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)


if __name__ == "__main__":
    main()
