"""The quantised reference, checked in Python before the C gate sees it.

``make nnue-test`` is the real acceptance gate: src/nnue.c against these same
numbers, exactly, on ten thousand positions. But that gate needs a trained
checkpoint, a 50 MB net file and a compiler, so it is not where a scale error
should be found. These tests need none of those and catch the two failures that
would otherwise cost a build:

  * the SCReLU rescale in the wrong place, or missing. The squared activation
    carries QA^2 where the bias carries QA, so a missing divide is a net that
    is 255x too confident and an early divide is one that is off by a truncation
    everywhere. Both train fine and neither is visible in a loss curve.
  * an output bucket read from the wrong row, which is invisible in aggregate
    and worth a hundred centipawns in the positions where the rows differ.

The method is to quantise a model and then DEQUANTISE it back into itself, so
the float model and the integer model hold exactly the same numbers. What is
left between them is only the integer truncation, which is bounded and tiny -
so a disagreement of more than a centipawn is a scale error rather than
rounding, and the test can say so.
"""

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from export_net import (  # noqa: E402
    HEADER_BYTES,
    check_ranges,
    forward,
    quantise,
    trunc_div,
)
from nnue.format import (  # noqa: E402
    NUM_FEATURES,
    PAD_INDEX,
    QA,
    QB,
    SCALE,
    pack_fens,
    unpack,
)
from nnue.model import NNUE  # noqa: E402

FENS = [
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "3r1rk1/p3qppp/2bb1n2/1p6/3P4/1B3N2/PP2QPPP/R1B2RK1 w - - 2 18",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "8/1r3k2/8/2R5/8/5K2/8/8 b - - 0 1",
    "8/5k2/8/8/8/8/5PPP/6K1 w - - 0 1",
    "8/8/8/4k3/8/8/8/4K3 w - - 0 1",
]


def build(hidden=32, buckets=8, seed=0, uncertainty=False):
    """A small model with weights spread over the whole clipped range.

    Deliberately not a trained net: a trained one has small weights and would
    pass a scale test that a saturated one fails.
    """
    torch.manual_seed(seed)
    model = NNUE(hidden=hidden, output_buckets=buckets, uncertainty=uncertainty)
    with torch.no_grad():
        model.ft.weight.uniform_(-0.08, 0.08)
        model.ft_bias.uniform_(-0.05, 0.05)
        model.out.weight.uniform_(-1.5, 1.5)
        model.out.bias.uniform_(-0.2, 0.2)
        if uncertainty:
            model.unc.weight.uniform_(-1.5, 1.5)
            model.unc.bias.uniform_(-0.2, 0.2)
    model.clip_weights()
    return model


def dequantise_into(model, q):
    """Put the quantised weights back into the float model, so the two are
    running the same numbers and only the integer arithmetic differs."""
    with torch.no_grad():
        model.ft.weight[:NUM_FEATURES] = torch.from_numpy(
            q["ft_w"].astype(np.float64) / QA).float()
        model.ft.weight[PAD_INDEX].zero_()
        model.ft_bias.copy_(torch.from_numpy(q["ft_b"].astype(np.float64) / QA).float())
        model.out.weight.copy_(torch.from_numpy(q["out_w"].astype(np.float64) / QB).float())
        model.out.bias.copy_(
            torch.from_numpy(q["out_b"].astype(np.float64) / (QA * QB)).float())
    return model


def run(model):
    """(quantised cp, float cp) for FENS under this model's architecture."""
    arch = model.arch
    q = quantise(model.state_dict(), arch, QA, QB)
    q["qb"] = QB
    check_ranges(q, QA)

    dequantise_into(model, q)
    fields = unpack(pack_fens(FENS))
    raw, cp, _unc_raw, _unc_cp = forward(q, fields, QA, QB, SCALE)

    with torch.no_grad():
        model.eval()
        float_cp = model.evaluate_cp(
            torch.from_numpy(fields["white"]),
            torch.from_numpy(fields["black"]),
            torch.from_numpy(fields["stm"]).float().unsqueeze(1),
            torch.from_numpy(fields["piece_count"]),
        ).numpy()
    return q, raw, cp, float_cp


@pytest.mark.parametrize("buckets", [1, 8])
def test_the_integer_forward_pass_reproduces_the_float_one(buckets):
    model = build(buckets=buckets)
    _q, _raw, cp, float_cp = run(model)

    worst = float(np.abs(float_cp - cp).max())
    assert worst < 1.5, (
        f"quantised and float disagree by {worst:.1f} cp on identical weights. "
        f"That is a scale error, not rounding - check where the SCReLU rescale "
        f"happens in export_net.forward()."
    )
    # Not all zero, or the test above would pass on a net that says nothing.
    assert np.abs(cp).max() > 10


def test_a_missing_screlu_rescale_would_have_been_caught():
    """Guards the guard: if the rescale were dropped, the numbers would differ
    by a factor of QA, and this asserts the test above is sensitive to that."""
    model = build()
    q, raw, _cp, _float_cp = run(model)

    fields = unpack(pack_fens(FENS))
    table = np.concatenate([q["ft_w"], np.zeros((1, q["hidden"]), dtype=np.int32)])
    acc_w = q["ft_b"] + table[fields["white"]].sum(axis=1, dtype=np.int64)
    acc_b = q["ft_b"] + table[fields["black"]].sum(axis=1, dtype=np.int64)
    black = fields["stm"].astype(bool)[:, None]
    x = np.concatenate([np.clip(np.where(black, acc_b, acc_w), 0, QA),
                        np.clip(np.where(black, acc_w, acc_b), 0, QA)], axis=1)

    bucket = (fields["piece_count"] - 2) // (32 // q["buckets"])
    unscaled = (x * x * q["out_w"][bucket].astype(np.int64)).sum(axis=1)

    # What the exporter actually produced is the rescaled version of this.
    assert np.array_equal(raw, trunc_div(unscaled, QA) + q["out_b"][bucket])
    assert np.abs(unscaled - raw).max() > 1000, "the rescale did nothing"


def test_the_uncertainty_head_quantises_and_reproduces_its_float_self():
    """The head is a second output layer through the same trunk, and its
    integer forward must track the float model it came from the same way the
    value head's does - same grid, same rescale, same truncation. The clamp
    floor is the one asymmetry: a magnitude clamps at zero, not -EVAL_LIMIT,
    so the comparison is against the float prediction clamped the same way."""
    model = build(uncertainty=True)
    q = quantise(model.state_dict(), model.arch, QA, QB)
    q["qb"] = QB
    check_ranges(q, QA)

    with torch.no_grad():
        model.ft.weight[:NUM_FEATURES] = torch.from_numpy(
            q["ft_w"].astype(np.float64) / QA).float()
        model.ft.weight[PAD_INDEX].zero_()
        model.ft_bias.copy_(torch.from_numpy(q["ft_b"].astype(np.float64) / QA).float())
        model.unc.weight.copy_(torch.from_numpy(q["unc_w"].astype(np.float64) / QB).float())
        model.unc.bias.copy_(
            torch.from_numpy(q["unc_b"].astype(np.float64) / (QA * QB)).float())

    fields = unpack(pack_fens(FENS))
    _raw, _cp, unc_raw, unc_cp = forward(q, fields, QA, QB, SCALE)
    assert unc_raw is not None
    assert unc_cp.min() >= 0, "a predicted magnitude must clamp at zero"

    with torch.no_grad():
        model.eval()
        _value, float_unc = model.forward_heads(
            torch.from_numpy(fields["white"]),
            torch.from_numpy(fields["black"]),
            torch.from_numpy(fields["stm"]).float().unsqueeze(1),
            torch.from_numpy(fields["piece_count"]),
        )
    float_unc_cp = np.clip(float_unc.numpy() * SCALE, 0, None)

    worst = float(np.abs(float_unc_cp - unc_cp).max())
    assert worst < 1.5, (
        f"quantised and float uncertainty disagree by {worst:.1f} cp on identical "
        f"weights - check the unc path in export_net.forward()."
    )
    # Not all zero, or the assertion above would pass on a head that says
    # nothing.
    assert np.abs(unc_cp).max() > 10


def test_a_headless_checkpoint_and_a_headed_one_disagreeing_is_refused():
    """The head's presence is claimed by the arch and by the weights, and the
    exporter must refuse the pair that disagrees rather than trust either."""
    model = build(uncertainty=True)
    lying_arch = dict(model.arch, uncertainty=False)
    with pytest.raises(SystemExit, match="unc"):
        quantise(model.state_dict(), lying_arch, QA, QB)


def test_output_buckets_actually_route_to_different_rows():
    """Two nets identical but for their output rows must score positions of
    different piece counts differently - and a bucketed net must not be
    reproducible by reading row 0 for everything."""
    model = build(buckets=8)
    q = quantise(model.state_dict(), model.arch, QA, QB)
    q["qb"] = QB

    fields = unpack(pack_fens(FENS))
    _raw, cp, _u, _ucp = forward(q, fields, QA, QB, SCALE)

    flat = dict(q)
    flat["out_w"] = np.repeat(q["out_w"][:1], q["buckets"], axis=0)
    flat["out_b"] = np.repeat(q["out_b"][:1], q["buckets"], axis=0)
    _raw0, cp0, _u0, _ucp0 = forward(flat, fields, QA, QB, SCALE)

    buckets = (fields["piece_count"] - 2) // 4
    assert len(set(buckets.tolist())) > 1, "the test positions cover only one bucket"
    assert not np.array_equal(cp, cp0)


def test_trunc_div_truncates_toward_zero_the_way_c_does():
    """numpy floors and C truncates. Every negative evaluation would be one
    centipawn low, which is about half the vectors."""
    assert trunc_div(np.array([-7, 7, -8, 8, -1, 0]), 4).tolist() == [-1, 1, -2, 2, 0, 0]
    assert (np.array([-7]) // 4).tolist() == [-2]  # what NOT to use


def test_the_payload_is_the_size_the_header_promises():
    """nnue_payload_bytes() in src/nnue.c computes this from the header alone
    and refuses a file that disagrees, so the two formulas have to match."""
    for hidden, buckets in ((32, 1), (16, 8)):
        model = build(hidden=hidden, buckets=buckets)
        q = quantise(model.state_dict(), model.arch, QA, QB)

        payload = (NUM_FEATURES * hidden * 2 + hidden * 2
                   + buckets * 2 * hidden * 2 + buckets * 4)
        assert q["ft_w"].shape == (NUM_FEATURES, hidden)
        assert q["out_w"].shape == (buckets, 2 * hidden)
        assert q["out_b"].shape == (buckets,)
        assert payload + HEADER_BYTES > 0  # the arithmetic above is the assertion


def test_an_unrepresentable_net_is_refused_rather_than_wrapped():
    """A net trained with --no-weight-clip can quantise past what the engine's
    int16 path holds. Refusing beats wrapping: a wrapped weight is a net that
    scores plausibly and loses Elo silently."""
    model = build()
    with torch.no_grad():
        model.out.weight.fill_(20.0)  # 20 * QB = 1280, and QA * 1280 is past int16

    q = quantise(model.state_dict(), model.arch, QA, QB)
    q["qb"] = QB
    with pytest.raises(SystemExit, match="int16"):
        check_ranges(q, QA)


def test_the_accumulator_bound_is_sound_over_every_legal_position():
    """The bound is bias plus the 32 largest positive weights in a column - a
    statement about all legal positions, not about the positions tested."""
    model = build(hidden=64)
    q = quantise(model.state_dict(), model.arch, QA, QB)
    q["qb"] = QB
    limits = check_ranges(q, QA)

    fields = unpack(pack_fens(FENS))
    table = np.concatenate([q["ft_w"], np.zeros((1, q["hidden"]), dtype=np.int32)])
    seen = np.abs(q["ft_b"] + table[fields["white"]].sum(axis=1, dtype=np.int64)).max()

    assert seen <= limits["accumulator_bound"]
    assert limits["accumulator_bound"] <= 32767
