"""The model and the training loop, end to end on a handful of records.

None of this measures whether the network is any good - that is what the
held-out loss and, eventually, an SPRT are for. It measures that the pipeline
is connected: that a batch reaches the model, that the loss has a gradient,
that the optimiser moves the weights in the direction that reduces it, and that
the padding slot stays pinned at zero.
"""

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from nnue.dataset import ShardBatches, identity_collate  # noqa: E402
from nnue.format import PAD_INDEX, pack_fens, unpack  # noqa: E402
from nnue.model import NNUE, blended_target, loss_fn  # noqa: E402
from nnue.sanity import SANITY_POSITIONS, flip_fen, null_fen, score_fens  # noqa: E402

START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"


def tensors(fens):
    fields = unpack(pack_fens(fens))
    return (
        torch.from_numpy(fields["white"]),
        torch.from_numpy(fields["black"]),
        torch.from_numpy(fields["stm"]).float().unsqueeze(1),
    )


def test_forward_shape():
    model = NNUE(hidden=32)
    white, black, stm = tensors([fen for fen, _, _ in SANITY_POSITIONS])
    assert model(white, black, stm).shape == (len(SANITY_POSITIONS),)


def test_padding_embedding_is_zero_and_stays_zero():
    model = NNUE(hidden=16)
    assert torch.count_nonzero(model.ft.weight[PAD_INDEX]) == 0

    white, black, stm = tensors([START, "4k3/8/8/8/8/8/8/4K3 w - - 0 1"])
    prediction = model(white, black, stm)
    prediction.sum().backward()

    torch.optim.SGD(model.parameters(), lr=1.0).step()
    assert torch.count_nonzero(model.ft.weight[PAD_INDEX]) == 0


def test_piece_count_does_not_leak_through_padding():
    """Two positions differing only in piece count must not differ merely
    because one has more padding slots than the other."""
    model = NNUE(hidden=16)
    with torch.no_grad():
        model.ft.weight.uniform_(-0.5, 0.5)
        model.ft.weight[PAD_INDEX].zero_()

    bare = "4k3/8/8/8/8/8/8/4K3 w - - 0 1"
    white, black, stm = tensors([bare])
    accumulator, _ = model.accumulators(white, black)

    # Two kings only: the accumulator must be the sum of exactly two rows plus
    # the bias, with nothing contributed by the thirty padding slots.
    fields = unpack(pack_fens([bare]))
    active = [int(f) for f in fields["white"][0] if f != PAD_INDEX]
    expected = model.ft.weight[active].sum(dim=0) + model.ft_bias
    assert torch.allclose(accumulator[0], expected, atol=1e-6)


def test_blended_target_handles_unknown_results():
    score = torch.tensor([0.0, 0.0, 0.0])
    wdl = torch.tensor([0, 2, 3])  # loss, win, unknown
    target = blended_target(score, wdl, lam=0.5, sigmoid_k=400.0)

    assert target[0].item() == pytest.approx(0.25)  # 0.5*0.5 + 0.5*0.0
    assert target[1].item() == pytest.approx(0.75)  # 0.5*0.5 + 0.5*1.0
    assert target[2].item() == pytest.approx(0.5)   # lambda forced to 1
    assert torch.isfinite(target).all()


def test_a_few_hundred_steps_overfit_a_tiny_batch():
    """The pipeline test. If the loss does not fall here, something between the
    batch and the optimiser is disconnected, and no amount of data will help."""
    torch.manual_seed(0)

    fens = [fen for fen, _, _ in SANITY_POSITIONS]
    white, black, stm = tensors(fens)
    score = torch.tensor([0.0, 20.0, -300.0, 300.0, -900.0, 500.0, 0.0, 700.0, 15.0, -40.0])
    wdl = torch.tensor([1, 1, 0, 2, 0, 2, 1, 2, 1, 1])

    model = NNUE(hidden=64)
    optimiser = torch.optim.AdamW(model.parameters(), lr=3e-3)
    target = blended_target(score, wdl, lam=0.9, sigmoid_k=400.0)

    first = last = None
    for step in range(400):
        prediction = model(white, black, stm)
        loss = loss_fn(prediction, target, 400.0)
        optimiser.zero_grad(set_to_none=True)
        loss.backward()
        optimiser.step()
        if step == 0:
            first = loss.item()
        last = loss.item()

    assert last < first / 10.0, f"loss went {first:.5f} -> {last:.5f}"


def test_colour_flip_is_an_exact_identity_even_untrained():
    """A side-to-move-relative score must be IDENTICAL for a position and for
    that position with both colours swapped, the ranks flipped and the side to
    move swapped - the same position seen from the other side.

    This is structural: it falls out of the feature normalisation and the
    perspective swap in forward(), so it holds on random weights and to
    floating-point precision. Anything else means the normalisation is not
    symmetric, and training will paper over it rather than fix it.
    """
    torch.manual_seed(2)
    model = NNUE(hidden=64)

    fens = [fen for fen, _, _ in SANITY_POSITIONS]
    scores = score_fens(model, fens)
    flipped = score_fens(model, [flip_fen(f) for f in fens])

    worst = max(abs(a - b) for a, b in zip(scores, flipped))
    assert worst < 1e-2, f"largest |score - flip| was {worst:.4f} cp"


def test_the_side_to_move_reads_its_own_accumulator_first():
    """The bug the colour flip cannot see.

    Swapping `own` and `other` in forward() leaves the net perfectly
    colour-symmetric and evaluates every position from the OPPONENT's point of
    view - "plays reasonably, hates its own position". Pinning it down needs a
    net whose output is a known function of one accumulator, so the second half
    of the output layer is zeroed and the first half summed.
    """
    torch.manual_seed(3)
    model = NNUE(hidden=8)
    with torch.no_grad():
        model.ft.weight.uniform_(0.0, 0.03)  # positive, so nothing clips at 0
        model.ft.weight[PAD_INDEX].zero_()
        model.ft_bias.zero_()
        model.out.weight.zero_()
        model.out.weight[0, :model.hidden] = 1.0  # read `own` only
        model.out.bias.zero_()

    fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKB1R w KQkq - 0 1"
    for turn, perspective in (("w", "white"), ("b", "black")):
        board = fen.replace(" w ", f" {turn} ") if turn == "b" else fen
        white, black, stm = tensors([board])
        got = model(white, black, stm)

        fields = unpack(pack_fens([board]))
        active = [int(f) for f in fields[perspective][0] if f != PAD_INDEX]
        want = model.ft.weight[active].sum(dim=0).clamp(0.0, 1.0).sum()

        assert torch.allclose(got[0], want, atol=1e-5), (
            f"with {turn} to move the net summed the wrong accumulator: "
            f"{got.item():.5f} vs {want.item():.5f}"
        )


def test_a_trained_net_prefers_the_side_that_is_material_up():
    """The Task 2 gate in miniature. Fit on targets that are side-to-move
    relative, then check the SIGNS come out right - a net a piece down must
    score negative whichever colour is a piece down."""
    torch.manual_seed(1)

    fens = [fen for fen, _, _ in SANITY_POSITIONS]
    both = fens + [null_fen(f) for f in fens]
    white, black, stm = tensors(both)

    cp = torch.tensor([0.0, 20.0, -300.0, 300.0, -900.0, 500.0, 0.0, 700.0, 15.0, -40.0])
    # Passing the turn negates a side-to-move-relative score.
    score = torch.cat([cp, -cp])
    wdl = torch.full((len(both),), 3)

    model = NNUE(hidden=64)
    optimiser = torch.optim.AdamW(model.parameters(), lr=3e-3)
    target = blended_target(score, wdl, lam=1.0, sigmoid_k=400.0)

    for _ in range(800):
        loss = loss_fn(model(white, black, stm), target, 400.0)
        optimiser.zero_grad(set_to_none=True)
        loss.backward()
        optimiser.step()

    scores = dict(zip([name for _, name, _ in SANITY_POSITIONS], score_fens(model, fens)))
    assert scores["white minus a knight"] < -100
    assert scores["black minus a knight"] > 100
    assert scores["white minus a queen"] < -400
    assert abs(scores["start position"]) < 120


def test_dataset_covers_every_record_exactly_once(shard_path):
    dataset = ShardBatches(shard_path, batch_size=64)
    seen = sum(identity_collate([dataset[i]])["stm"].shape[0] for i in range(len(dataset)))
    assert seen == dataset.records


def test_dataset_batches_are_tensors_of_the_right_dtype(shard_path):
    batch = ShardBatches(shard_path, batch_size=17)[0]
    assert batch["white"].dtype == torch.int64
    assert batch["black"].dtype == torch.int64
    assert batch["stm"].dtype == torch.float32
    assert batch["score"].dtype == torch.float32
    assert batch["stm"].shape == (17, 1)
    assert batch["white"].shape == (17, 32)
    assert np.isfinite(batch["score"].numpy()).all()
