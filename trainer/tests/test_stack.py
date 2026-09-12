"""The optional L1/L2/L3 stack (Task 6 in docs/NNUE.md).

Two classes of thing are checked here, and only the second is about the stack
being any good - which is what a held-out loss and eventually an SPRT are for.

The first is that turning it ON changes the architecture and turning it OFF
changes NOTHING: `--l1-size 0` has to stay the net that shipped, to the last
bit, because it is the control every stacked net is measured against and a
control that quietly drifted would make the comparison meaningless without
failing anything.

The second is that the float model is the float IMAGE of the integer
arithmetic src/nnue.c will run - the clamp at one as much as the clamp at zero.
A stack trained without the upper clamp fits activations the quantised engine
cannot represent, and it trains to a completely normal loss curve doing it.
"""

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from export_net import check_ranges, forward, quantise  # noqa: E402
from nnue.format import (  # noqa: E402
    L1_CLIP,
    L1_SHIFT,
    L2_CLIP,
    L2_SHIFT,
    L3_CLIP,
    L3_SCALE,
    NUM_FEATURES,
    PAD_INDEX,
    QA,
    QB,
    SCALE,
    STACK_QA,
    STACK_WIDTH_MULTIPLE,
    WEIGHT_CLIP,
    pack_fens,
    unpack,
)
from nnue.model import (NNUE, arch_from_checkpoint, blended_target,  # noqa: E402
                        from_checkpoint, loss_fn)
from nnue.sanity import SANITY_POSITIONS  # noqa: E402


def inputs():
    """Two positions, different king slots, one of them padded."""
    white = torch.tensor([[0, 2, PAD_INDEX], [768, 770, 775]], dtype=torch.int32)
    black = torch.tensor([[769, 771, PAD_INDEX], [1, 3, 6]], dtype=torch.int32)
    return white, black, torch.tensor([[0.0], [1.0]]), torch.tensor([2, 3])


# ------------------------------------------------------------- the control --


def test_the_flat_net_is_untouched_by_the_stack_existing():
    """`l1_size = 0` is the shipped architecture, including which tensors the
    checkpoint holds. An `l1.*` or `l3.*` in a flat net's state dict would make
    every checkpoint written since incompatible with the exporter."""
    model = NNUE(16, 8, uncertainty=True)

    assert model.l1_size == 0 and model.l2_size == 0
    assert model.trunk_width == 32
    assert model.stack_layers == []
    assert hasattr(model, "out") and not hasattr(model, "l1")
    assert set(model.state_dict()) == {
        "ft.weight", "ft_bias", "out.weight", "out.bias", "unc.weight", "unc.bias",
    }


def test_the_stack_replaces_the_flat_layer_rather_than_joining_it():
    """Both would train. Only one of them is a net the engine can be given."""
    model = NNUE(16, 8, uncertainty=True, l1_size=16, l2_size=32)

    assert not hasattr(model, "out")
    assert model.l1.weight.shape == (8 * 16, 32)
    assert model.l2.weight.shape == (8 * 32, 16)
    assert model.l3.weight.shape == (8, 32)
    # Both heads read the stack's last hidden layer, not the accumulator.
    assert model.trunk_width == 32
    assert model.unc.weight.shape == (8, 32)


def test_l2_off_runs_l1_into_the_output_layer():
    model = NNUE(16, 8, uncertainty=True, l1_size=16)

    assert not hasattr(model, "l2")
    assert model.trunk_width == 16
    assert model.l3.weight.shape == (8, 16)
    assert model.unc.weight.shape == (8, 16)


@pytest.mark.parametrize("l1,l2", [(0, 0), (16, 0), (16, 32), (32, 16)])
def test_every_configuration_produces_a_finite_score(l1, l2):
    model = NNUE(16, 8, uncertainty=True, l1_size=l1, l2_size=l2)
    value, unc = model.forward_heads(*inputs())

    assert value.shape == (2,) and unc.shape == (2,)
    assert torch.isfinite(value).all() and torch.isfinite(unc).all()
    # forward() and forward_heads() answer from one pass over one trunk.
    torch.testing.assert_close(value, model(*inputs()), rtol=0, atol=0)


# ------------------------------------------------------- shapes are refused --


@pytest.mark.parametrize("width", [1, 8, 17, 24, -16])
def test_a_stack_width_the_engine_could_not_vectorise_is_refused(width):
    """l1_size and l2_size are inner-loop lengths in src/nnue.c, which carries
    no remainder loop. Failing here turns an overnight run that cannot be
    exported into a flag error."""
    with pytest.raises(ValueError, match=f"multiple of {STACK_WIDTH_MULTIPLE}"):
        NNUE(16, 8, l1_size=width)
    with pytest.raises(ValueError, match=f"multiple of {STACK_WIDTH_MULTIPLE}"):
        NNUE(16, 8, l1_size=16, l2_size=width)


def test_l2_without_l1_is_refused_rather_than_promoted():
    """Silently treating it as "L1 of that width" would train a net nobody
    asked for and record an arch that says so."""
    with pytest.raises(ValueError, match="l2_size requires l1_size"):
        NNUE(16, 8, l2_size=32)


# ----------------------------------------------------------- the activation --


def test_the_stack_activation_is_the_integer_stage_it_stands_for():
    """`min(max(s, 0) >> SHIFT, 255)` in float is `clamp(x, 0, 1)`. The upper
    clamp is the requantisation ceiling, not a nicety: without it the trained
    activations do not fit the range the next stage's weights are scaled
    against."""
    model = NNUE(16, l1_size=16)
    x = torch.tensor([[-3.0, -0.25, 0.0, 0.5, 1.0, 4.0]])

    got = model.stack_activate(x)
    assert torch.allclose(got, torch.tensor([[0.0, 0.0, 0.0, 0.5, 1.0, 1.0]]))
    # And it is NOT SCReLU - the stack squares nothing.
    assert not torch.allclose(got, model.activate(x))


def test_a_stacked_net_reproduces_its_own_arithmetic_by_hand():
    """The one test that would catch a bucket gathered from the wrong axis.

    `_pick_rows` reshapes a (batch, buckets * width) layer output and gathers
    one bucket's slice; getting the reshape and the gather consistent is the
    whole of it, and a transposed view still trains.
    """
    torch.manual_seed(3)
    model = NNUE(16, 8, l1_size=16, l2_size=32)
    white, black, stm, pieces = inputs()

    with torch.no_grad():
        activated = model._activated(white, black, stm)
        h1 = model.l1(activated).view(2, 8, 16)
        h2_in = torch.stack([model.stack_activate(h1[i, b]) for i, b in enumerate([0, 0])])
        h2 = model.l2(h2_in).view(2, 8, 32)
        trunk = torch.stack([model.stack_activate(h2[i, b]) for i, b in enumerate([0, 0])])
        expected = torch.stack([model.l3(trunk[i])[b] for i, b in enumerate([0, 0])])

        torch.testing.assert_close(model(white, black, stm, pieces), expected,
                                   rtol=1e-6, atol=1e-6)


def test_a_bucketed_stack_refuses_to_guess_the_piece_count():
    """The stack's layers are per-bucket too, so the refusal has to happen in
    the trunk and not only at the final gather."""
    model = NNUE(16, 8, l1_size=16)
    white, black, stm, _ = inputs()

    with pytest.raises(ValueError, match="piece_count"):
        model(white, black, stm)


@pytest.mark.parametrize("l1,l2,buckets", [(16, 0, 1), (16, 32, 1), (16, 32, 8)])
def test_a_stacked_net_can_still_overfit_a_tiny_batch(l1, l2, buckets):
    """The pipeline test, through the stack. If the loss does not fall here,
    something between the batch and the optimiser is disconnected.

    The stack has its own way to be disconnected that the flat net does not:
    `clamp(x, 0, 1)` is flat on both sides, so a layer whose units all start
    saturated takes no gradient at all and trains to a loss that never moves.
    That is what the 0.5 bias init is for, and this is what would catch it
    going away. Run with and without buckets, because a per-bucket gather that
    detaches the gradient looks exactly like this passing on one
    parametrisation and failing on the other.
    """
    torch.manual_seed(0)

    model = NNUE(hidden=64, output_buckets=buckets, l1_size=l1, l2_size=l2)
    fens = [fen for fen, _, _ in SANITY_POSITIONS]
    white, black, stm, pieces = pack_fens(fens), None, None, None
    fields = unpack(white)
    white = torch.from_numpy(fields["white"].astype(np.int64))
    black = torch.from_numpy(fields["black"].astype(np.int64))
    stm = torch.from_numpy(fields["stm"].astype(np.float32)).view(-1, 1)
    pieces = torch.from_numpy(fields["piece_count"].astype(np.int64))

    score = torch.tensor([0.0, 20.0, -300.0, 300.0, -900.0, 500.0, 0.0, 700.0, 15.0, -40.0])
    wdl = torch.tensor([1, 1, 0, 2, 0, 2, 1, 2, 1, 1])

    optimiser = torch.optim.AdamW(model.parameters(), lr=3e-3)
    target = blended_target(score, wdl, lam=0.9, sigmoid_k=400.0)

    first = last = None
    for step in range(400):
        loss = loss_fn(model(white, black, stm, pieces), target, 400.0)
        optimiser.zero_grad(set_to_none=True)
        loss.backward()
        optimiser.step()
        model.clip_weights()
        if step == 0:
            first = loss.item()
        last = loss.item()

    assert last < first / 10.0, f"loss went {first:.5f} -> {last:.5f}"


# ---------------------------------------------------------------- clipping --


def test_each_layer_is_clipped_by_its_own_bound():
    """One bound per layer, each from that layer's own integer constraint. The
    flat output layer's int16 SIMD product is not L1's int32 sum, and sharing
    one number between them is the defect Task 6 exists to remove."""
    model = NNUE(32, 8, uncertainty=True, l1_size=16, l2_size=32)
    with torch.no_grad():
        for parameter in model.parameters():
            parameter.uniform_(-50.0, 50.0)

    model.clip_weights()

    for name, tensor, bound in (
        ("ft.weight", model.ft.weight, WEIGHT_CLIP),
        ("ft_bias", model.ft_bias, WEIGHT_CLIP),
        ("l1.weight", model.l1.weight, L1_CLIP),
        ("l1.bias", model.l1.bias, L1_CLIP),
        ("l2.weight", model.l2.weight, L2_CLIP),
        ("l2.bias", model.l2.bias, L2_CLIP),
        ("l3.weight", model.l3.weight, L3_CLIP),
        ("unc.weight", model.unc.weight, L3_CLIP),
    ):
        assert tensor.abs().max().item() <= bound + 1e-6, f"{name} escaped its bound"
    assert torch.count_nonzero(model.ft.weight[PAD_INDEX]) == 0


def test_the_clips_keep_the_quantised_stack_inside_int32():
    """The bound the clips exist to hold, computed the way the exporter will:
    the input to L1 is at most QA in every lane, so the worst sum is
    `QA * sum|w|` plus the bias, over the widest net the engine will load."""
    qa, widest_2h = 255, 2 * 2048
    l1_sum = widest_2h * qa * (L1_CLIP * 512)
    assert l1_sum + L1_CLIP * qa * 512 < np.iinfo(np.int32).max

    # L3 is stored int16, which binds long before its int32 sum does.
    assert L3_CLIP * 512 <= np.iinfo(np.int16).max


# -------------------------------------------------------------- checkpoints --


def test_a_stacked_checkpoint_describes_its_own_stack():
    model = NNUE(48, 4, uncertainty=True, l1_size=16, l2_size=32)
    state = {"model": model.state_dict(), "arch": model.arch}

    assert arch_from_checkpoint(state)["l1_size"] == 16
    assert arch_from_checkpoint(state)["l2_size"] == 32

    restored = from_checkpoint(state)
    assert restored.arch == model.arch
    torch.testing.assert_close(restored(*inputs()), model(*inputs()), rtol=0, atol=0)


def test_a_checkpoint_from_before_the_stack_is_a_flat_net():
    """Absent means zero, and zero means flat. Every checkpoint on disk today
    predates the stack, and none of them may start loading as something else."""
    flat = NNUE(48, 4, uncertainty=True)
    old_style = {k: v for k, v in flat.arch.items() if k not in ("l1_size", "l2_size")}

    restored = from_checkpoint({"model": flat.state_dict(), "arch": old_style})
    assert restored.l1_size == 0 and restored.l2_size == 0
    torch.testing.assert_close(restored(*inputs()), flat(*inputs()), rtol=0, atol=0)


def test_the_describe_line_names_the_stack():
    assert "512x2 -> 8" in NNUE(512, 8).describe()
    assert "512x2 -> 16 -> 32 -> 8" in NNUE(512, 8, l1_size=16, l2_size=32).describe()
    assert "512x2 -> 16 -> 8" in NNUE(512, 8, l1_size=16).describe()


# ----------------------------------------------------------------- export ----


def test_the_exporter_still_takes_a_flat_checkpoint():
    model = NNUE(16, 8, uncertainty=True)
    q = quantise(model.state_dict(), model.arch, QA, QB)
    assert q["hidden"] == 16 and q["buckets"] == 8 and q["l1_size"] == 0
    assert q["trunk"] == 32


@pytest.mark.parametrize("l1,l2", [(16, 0), (16, 32), (32, 16)])
def test_a_stacked_checkpoint_quantises_to_the_shape_the_engine_reads(l1, l2):
    model = NNUE(16, 8, uncertainty=True, l1_size=l1, l2_size=l2)
    q = quantise(model.state_dict(), model.arch, STACK_QA, L3_SCALE)

    assert q["l1_size"] == l1 and q["l2_size"] == l2
    assert q["trunk"] == (l2 or l1)
    assert q["l1_shift"] == L1_SHIFT and q["l2_shift"] == (L2_SHIFT if l2 else 0)
    # src/nnue.c reads l1Weight[bucket][unit][input] out of a flat span, which
    # is this array's C order and nothing else.
    assert q["l1_w"].shape == (8 * l1, 32)
    assert q["l1_b"].shape == (8 * l1,)   # flat: the engine indexes [bucket * l1 + unit]
    # Both heads read the trunk, so both are (buckets, trunk).
    assert q["out_w"].shape == (8, l2 or l1)
    assert q["unc_w"].shape == (8, l2 or l1)


def test_a_stacked_export_needs_a_power_of_two_qa():
    """The stack rescales the SCReLU output per element, where a shift is a
    shift and a divide is a divide. src/nnue.c rejects such a net at load;
    saying so here turns that into a flag error."""
    model = NNUE(16, 8, l1_size=16)
    with pytest.raises(SystemExit, match="not a power of two"):
        quantise(model.state_dict(), model.arch, 255, L3_SCALE)

    # ... and the flat architecture is untouched by that rule, because it
    # divides by qa once at the end rather than per element.
    quantise(NNUE(16, 8).state_dict(), NNUE(16, 8).arch, 255, QB)


def test_the_quantised_stack_tracks_its_own_float_model():
    """The end-to-end check on the numpy reference: quantise, put the SAME
    numbers back into the float model, and require the integer forward to land
    where the float one does.

    This is what would catch the reference and the trainer disagreeing about
    the architecture - a bucket indexed off the wrong axis, L2 reading the
    accumulator instead of L1, the activation shifted by the wrong amount.
    `make nnue-test` proves C equals numpy; nothing there proves numpy equals
    the model that was trained, and a matched pair of wrong implementations
    passes it.
    """
    torch.manual_seed(0)
    model = NNUE(hidden=32, output_buckets=8, l1_size=16, l2_size=32)
    with torch.no_grad():
        model.ft.weight.uniform_(-0.08, 0.08)
        model.ft_bias.uniform_(-0.05, 0.05)
        model.l1.weight.uniform_(-0.15, 0.15)
        model.l1.bias.uniform_(0.2, 0.6)
        model.l2.weight.uniform_(-0.4, 0.4)
        model.l2.bias.uniform_(0.2, 0.6)
        model.l3.weight.uniform_(-1.5, 1.5)
        model.l3.bias.uniform_(-0.2, 0.2)
    model.clip_weights()

    q = quantise(model.state_dict(), model.arch, STACK_QA, L3_SCALE)
    q["qb"] = L3_SCALE
    check_ranges(q, STACK_QA)

    # The float model, running the numbers the integer one will actually use.
    with torch.no_grad():
        model.ft.weight[:NUM_FEATURES] = torch.from_numpy(
            q["ft_w"].astype(np.float64) / STACK_QA).float()
        model.ft.weight[PAD_INDEX].zero_()
        model.ft_bias.copy_(torch.from_numpy(q["ft_b"].astype(np.float64) / STACK_QA).float())
        model.l1.weight.copy_(
            torch.from_numpy(q["l1_w"].astype(np.float64) / (1 << L1_SHIFT)).float())
        model.l1.bias.copy_(torch.from_numpy(
            q["l1_b"].astype(np.float64).reshape(-1) / (STACK_QA * (1 << L1_SHIFT))).float())
        model.l2.weight.copy_(
            torch.from_numpy(q["l2_w"].astype(np.float64) / (1 << L2_SHIFT)).float())
        model.l2.bias.copy_(torch.from_numpy(
            q["l2_b"].astype(np.float64).reshape(-1) / (STACK_QA * (1 << L2_SHIFT))).float())
        model.l3.weight.copy_(
            torch.from_numpy(q["out_w"].astype(np.float64) / L3_SCALE).float())
        model.l3.bias.copy_(torch.from_numpy(
            q["out_b"].astype(np.float64) / (STACK_QA * L3_SCALE)).float())

    fields = unpack(pack_fens(FENS))
    _raw, cp, _u, _uc = forward(q, fields, STACK_QA, L3_SCALE, SCALE)

    with torch.no_grad():
        model.eval()
        float_cp = model.evaluate_cp(
            torch.from_numpy(fields["white"]),
            torch.from_numpy(fields["black"]),
            torch.from_numpy(fields["stm"]).float().unsqueeze(1),
            torch.from_numpy(fields["piece_count"]),
        ).numpy()

    # Not exact, and it cannot be: every stage rounds where the float model does
    # not. What it must not do is DIVERGE - a wrong axis or a wrong shift puts
    # this in the hundreds - and it must not DRIFT ONE WAY, which is what
    # nnue_requantise()'s rounding half is for. Truncating instead measures
    # -4.3 cp mean signed against -0.9 here, on weights deliberately spread
    # over the whole clipped range.
    error = cp - float_cp
    assert np.abs(error).max() < 6.0, (cp, float_cp)
    assert abs(error.mean()) < 2.0, f"a one-sided drift of {error.mean():.2f} cp"



FENS = [
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "3r1rk1/p3qppp/2bb1n2/1p6/3P4/1B3N2/PP2QPPP/R1B2RK1 w - - 2 18",
    "8/8/8/4k3/8/8/8/4K3 w - - 0 1",
]

