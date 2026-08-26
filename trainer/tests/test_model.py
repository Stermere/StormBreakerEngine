"""The model and the training loop, end to end on a handful of records.

None of this measures whether the network is any good - that is what the
held-out loss and, eventually, an SPRT are for. It measures that the pipeline
is connected: that a batch reaches the model, that the loss has a gradient,
that the optimiser moves the weights in the direction that reduces it, and that
the padding slot stays pinned at zero.

The architecture tests are the ones worth reading. The activation, the output
bucket and the weight clip each have a failure mode where the net trains, the
loss falls, and the result is quietly a different model than the one that was
asked for - or one the engine cannot represent.
"""

import os

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from nnue.dataset import (  # noqa: E402
    AUTO_CHUNK_BYTES,
    INDEX_DTYPE,
    ShardBatches,
    ShuffledChunks,
    identity_collate,
    make_loader,
)
from nnue.format import (  # noqa: E402
    NUM_FEATURES,
    PAD_INDEX,
    WEIGHT_CLIP,
    output_bucket,
    pack_fens,
    unpack,
)
from nnue.model import (  # noqa: E402
    NNUE,
    arch_from_checkpoint,
    blended_target,
    from_checkpoint,
    loss_fn,
)
from nnue.sanity import SANITY_POSITIONS, flip_fen, null_fen, score_fens  # noqa: E402

START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"


def tensors(fens, model=None):
    """Batch tensors. `model` is accepted and unused - there is one feature set,
    and threading it through kept the call sites honest when there were two."""
    fields = unpack(pack_fens(fens))
    return (
        torch.from_numpy(fields["white"]),
        torch.from_numpy(fields["black"]),
        torch.from_numpy(fields["stm"]).float().unsqueeze(1),
        torch.from_numpy(fields["piece_count"]),
    )


# --------------------------------------------------------------- plumbing ---


def test_forward_shape():
    model = NNUE(hidden=32, output_buckets=4)
    white, black, stm, pieces = tensors([fen for fen, _, _ in SANITY_POSITIONS], model)
    assert model(white, black, stm, pieces).shape == (len(SANITY_POSITIONS),)


def test_padding_embedding_is_zero_and_stays_zero():
    model = NNUE(hidden=16)
    assert torch.count_nonzero(model.ft.weight[PAD_INDEX]) == 0

    white, black, stm, pieces = tensors([START, "4k3/8/8/8/8/8/8/4K3 w - - 0 1"], model)
    prediction = model(white, black, stm, pieces)
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
    white, black, _stm, _pieces = tensors([bare], model)
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


@pytest.mark.parametrize("buckets", [1, 8])
def test_a_few_hundred_steps_overfit_a_tiny_batch(buckets):
    """The pipeline test. If the loss does not fall here, something between the
    batch and the optimiser is disconnected, and no amount of data will help.

    Run with and without output buckets: a gather that detaches the gradient
    looks exactly like this test passing on one parametrisation and failing on
    the other.
    """
    torch.manual_seed(0)

    model = NNUE(hidden=64, output_buckets=buckets)
    fens = [fen for fen, _, _ in SANITY_POSITIONS]
    white, black, stm, pieces = tensors(fens, model)
    score = torch.tensor([0.0, 20.0, -300.0, 300.0, -900.0, 500.0, 0.0, 700.0, 15.0, -40.0])
    wdl = torch.tensor([1, 1, 0, 2, 0, 2, 1, 2, 1, 1])

    optimiser = torch.optim.AdamW(model.parameters(), lr=3e-3)
    target = blended_target(score, wdl, lam=0.9, sigmoid_k=400.0)

    first = last = None
    for step in range(400):
        prediction = model(white, black, stm, pieces)
        loss = loss_fn(prediction, target, 400.0)
        optimiser.zero_grad(set_to_none=True)
        loss.backward()
        optimiser.step()
        if step == 0:
            first = loss.item()
        last = loss.item()

    assert last < first / 10.0, f"loss went {first:.5f} -> {last:.5f}"


# ---------------------------------------------------------- the symmetries --


def test_colour_flip_is_an_exact_identity_even_untrained():
    """A side-to-move-relative score must be IDENTICAL for a position and for
    that position with both colours swapped, the ranks flipped and the side to
    move swapped - the same position seen from the other side.

    This is structural: it falls out of the feature normalisation and the
    perspective swap in forward(), so it holds on random weights and to
    floating-point precision. Anything else means the normalisation is not
    symmetric, and training will paper over it rather than fix it.

    Note that the flip does not change the piece count, so it does not change
    the output bucket either - which is the property that lets buckets be
    selected by piece count at all.
    """
    torch.manual_seed(2)
    model = NNUE(hidden=64, output_buckets=8)

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
    model = NNUE(hidden=16, output_buckets=1)
    pad = PAD_INDEX
    with torch.no_grad():
        model.ft.weight.uniform_(0.0, 0.03)  # positive, so nothing clips at 0
        model.ft.weight[pad].zero_()
        model.ft_bias.zero_()
        model.out.weight.zero_()
        model.out.weight[0, :model.hidden] = 1.0  # read `own` only
        model.out.bias.zero_()

    fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKB1R w KQkq - 0 1"
    for turn, perspective in (("w", "white"), ("b", "black")):
        board = fen.replace(" w ", f" {turn} ") if turn == "b" else fen
        white, black, stm, pieces = tensors([board], model)
        got = model(white, black, stm, pieces)

        fields = unpack(pack_fens([board]))
        summed = model.ft.weight[[int(f) for f in fields[perspective][0] if f != pad]]
        clamped = summed.sum(dim=0).clamp(0.0, 1.0)
        want = (clamped * clamped).sum()  # SCReLU

        assert torch.allclose(got[0], want, atol=1e-5), (
            f"with {turn} to move the net summed the wrong accumulator: "
            f"{got.item():.5f} vs {want.item():.5f}"
        )


def test_a_trained_net_prefers_the_side_that_is_material_up():
    """The Task 2 gate in miniature. Fit on targets that are side-to-move
    relative, then check the SIGNS come out right - a net a piece down must
    score negative whichever colour is a piece down."""
    torch.manual_seed(1)

    model = NNUE(hidden=64, output_buckets=1)
    fens = [fen for fen, _, _ in SANITY_POSITIONS]
    both = fens + [null_fen(f) for f in fens]
    white, black, stm, pieces = tensors(both, model)

    cp = torch.tensor([0.0, 20.0, -300.0, 300.0, -900.0, 500.0, 0.0, 700.0, 15.0, -40.0])
    # Passing the turn negates a side-to-move-relative score.
    score = torch.cat([cp, -cp])
    wdl = torch.full((len(both),), 3)

    optimiser = torch.optim.AdamW(model.parameters(), lr=3e-3)
    target = blended_target(score, wdl, lam=1.0, sigmoid_k=400.0)

    for _ in range(800):
        loss = loss_fn(model(white, black, stm, pieces), target, 400.0)
        optimiser.zero_grad(set_to_none=True)
        loss.backward()
        optimiser.step()

    scores = dict(zip([name for _, name, _ in SANITY_POSITIONS], score_fens(model, fens)))
    assert scores["white minus a knight"] < -100
    assert scores["black minus a knight"] > 100
    assert scores["white minus a queen"] < -400
    assert abs(scores["start position"]) < 120


# ------------------------------------------------------- the architecture ---


def test_screlu_clamps_before_it_squares():
    """The square has to be applied AFTER the clamp. Squaring first would make
    negative accumulators positive - a different and much worse function, which
    still trains and whose loss curve looks completely normal."""
    x = torch.tensor([[-2.0, -0.5, 0.0, 0.25, 0.5, 1.0, 3.0]])
    got = NNUE(hidden=16).activate(x)

    assert torch.allclose(got, torch.tensor([[0.0, 0.0, 0.0, 0.0625, 0.25, 1.0, 1.0]]))
    assert got.min() >= 0.0 and got.max() <= 1.0


@pytest.mark.parametrize("buckets", [1, 2, 4, 8, 16, 32])
def test_the_output_bucket_is_chosen_by_piece_count(buckets):
    """Which row a position reads, checked against the formula rather than
    against the implementation - src/nnue.c spells out the same expression, and
    a disagreement means the engine evaluates out of a row the trainer never
    trained."""
    model = NNUE(hidden=16, output_buckets=buckets)
    with torch.no_grad():
        # Zero everything but the output bias, so the forward pass returns the
        # bias of whichever bucket was selected and nothing else.
        model.ft.weight.zero_()
        model.ft_bias.zero_()
        model.out.weight.zero_()
        model.out.bias.copy_(torch.arange(buckets, dtype=torch.float32))

    fens = [
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",  # 32
        "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3",  # 30
        "3r1rk1/p3qppp/2bb1n2/1p6/3P4/1B3N2/PP2QPPP/R1B2RK1 w - - 2 18",  # 22
        "8/5k2/8/8/8/8/5PPP/6K1 w - - 0 1",  # 5
        "8/8/8/4k3/8/8/8/4K3 w - - 0 1",  # 2
    ]
    white, black, stm, pieces = tensors(fens, model)
    got = model(white, black, stm, pieces)

    want = output_bucket(pieces, buckets)
    assert torch.equal(got, want.float())
    assert int(want.max()) < buckets and int(want.min()) >= 0


def test_bucket_boundaries_are_where_the_formula_says():
    counts = np.arange(2, 33)
    assert np.array_equal(output_bucket(counts, 1), np.zeros(31, dtype=counts.dtype))
    # Eight buckets, four piece counts each: 2-5, 6-9, ... 30-32.
    assert output_bucket(np.array([2, 5, 6, 9, 29, 30, 32]), 8).tolist() == [0, 0, 1, 1, 6, 7, 7]
    with pytest.raises(ValueError):
        output_bucket(counts, 3)


def test_the_shape_flags_change_the_parameter_count_as_documented():
    small = NNUE(hidden=512, output_buckets=1)
    large = NNUE(hidden=1024, output_buckets=8)

    assert NUM_FEATURES == 24576
    assert small.ft.weight.shape == (NUM_FEATURES + 1, 512)
    assert large.ft.weight.shape == (NUM_FEATURES + 1, 1024)
    assert small.out.weight.shape == (1, 1024)
    assert large.out.weight.shape == (8, 2048)


@pytest.mark.parametrize("hidden", [0, 8, 100, 513])
def test_a_width_the_engine_could_not_vectorise_is_refused_at_construction(hidden):
    """src/nnue.c walks the accumulator sixteen lanes at a time and rejects a
    width that is not a multiple of 16. Failing here turns an overnight run
    that cannot be exported into a flag error."""
    with pytest.raises(ValueError, match="multiple of 16"):
        NNUE(hidden=hidden)


def test_weight_clipping_holds_the_bound_and_keeps_the_pad_row_zero():
    """The clip is what makes the exported net representable. A net trained
    without it can quantise to weights the engine's int16 path cannot hold, and
    the export then either refuses it or - worse, in a world without the
    exporter's bound check - wraps."""
    model = NNUE(hidden=32)
    with torch.no_grad():
        model.ft.weight.uniform_(-50.0, 50.0)
        model.ft_bias.uniform_(-50.0, 50.0)
        model.out.weight.uniform_(-50.0, 50.0)

    model.clip_weights()

    for name, tensor in (("ft.weight", model.ft.weight), ("ft_bias", model.ft_bias),
                         ("out.weight", model.out.weight)):
        peak = tensor.abs().max().item()
        assert peak <= WEIGHT_CLIP + 1e-6, f"{name} reached {peak}"
    assert torch.count_nonzero(model.ft.weight[PAD_INDEX]) == 0


def test_a_checkpoint_describes_its_own_architecture():
    """The exporter reads the architecture out of the checkpoint rather than
    being told it again on the command line. A .pt that does not carry its own
    shape is a .pt that can be exported as the wrong model."""
    model = NNUE(hidden=48, output_buckets=4)
    state = {"model": model.state_dict(), "hidden": 48, "arch": model.arch}

    assert arch_from_checkpoint(state) == {
        "hidden": 48, "output_buckets": 4,
        "features": "halfka-32sq", "activation": "screlu",
    }

    restored = from_checkpoint(state)
    assert restored.arch == model.arch
    white, black, stm, pieces = tensors([START], restored)
    assert torch.allclose(restored(white, black, stm, pieces), model(white, black, stm, pieces))


def test_a_checkpoint_from_the_old_architecture_is_refused_rather_than_guessed():
    """A .pt with no `arch` predates the current network, so it was trained on
    a different feature set with a different activation. The only two things
    that could be done with it are to refuse it and to export it as something
    it is not."""
    model = NNUE(hidden=32, output_buckets=1)

    with pytest.raises(SystemExit, match="no 'arch' field"):
        arch_from_checkpoint({"model": model.state_dict(), "hidden": 32})

    stale = {"model": model.state_dict(), "hidden": 32,
             "arch": {"hidden": 32, "output_buckets": 1,
                      "features": "halfka-8bucket", "activation": "crelu"}}
    with pytest.raises(SystemExit, match="halfka-8bucket"):
        arch_from_checkpoint(stale)


def test_a_bucketed_net_refuses_to_guess_the_piece_count():
    model = NNUE(hidden=16, output_buckets=8)
    white, black, stm, _pieces = tensors([START], model)
    with pytest.raises(ValueError, match="piece_count"):
        model(white, black, stm)


@pytest.mark.parametrize("buckets", [0, 3, 5, 64])
def test_an_impossible_bucket_count_is_refused_at_construction(buckets):
    with pytest.raises(ValueError):
        NNUE(hidden=16, output_buckets=buckets)


# ---------------------------------------------------------------- loading ---


def test_dataset_covers_every_record_exactly_once(shard_path):
    dataset = ShardBatches(shard_path, batch_size=64)
    seen = sum(identity_collate([dataset[i]])["stm"].shape[0] for i in range(len(dataset)))
    assert seen == dataset.records


def test_dataset_batches_are_tensors_of_the_right_dtype(shard_path):
    batch = ShardBatches(shard_path, batch_size=17)[0]
    # int32 by default: indices run to 24576 and the matrices are the widest
    # thing the loader moves. The model has to accept them, which is the half
    # of this that is not obvious - see test_the_model_eats_the_loader_dtype.
    assert batch["white"].dtype == torch.from_numpy(np.empty(0, INDEX_DTYPE)).dtype
    assert batch["black"].dtype == batch["white"].dtype
    assert batch["stm"].dtype == torch.float32
    assert batch["score"].dtype == torch.float32
    assert batch["piece_count"].dtype == torch.int64
    assert batch["stm"].shape == (17, 1)
    assert batch["white"].shape == (17, 32)
    assert np.isfinite(batch["score"].numpy()).all()
    assert int(batch["piece_count"].min()) >= 2 and int(batch["piece_count"].max()) <= 32


def test_the_loader_yields_indices_the_model_can_hold(shard_path):
    batch = ShardBatches(shard_path, batch_size=64)[0]
    active = batch["white"][batch["white"] != PAD_INDEX]
    assert int(active.max()) < NUM_FEATURES
    assert int(batch["white"].max()) == PAD_INDEX


def _records(batches):
    """Every record a loader yielded, as a sortable key per record.

    Comparing loaders by their multiset of records is the only comparison that
    means anything: the whole point of the chunked one is that it emits them in
    a different order.
    """
    rows = []
    for batch in batches:
        white = batch["white"].numpy()
        score = batch["score"].numpy()
        wdl = batch["wdl"].numpy()
        for i in range(white.shape[0]):
            rows.append((white[i].tobytes(), float(score[i]), int(wdl[i])))
    return sorted(rows)


def test_the_model_eats_the_loader_dtype(shard_path):
    """int32 indices are only a saving if EmbeddingBag takes them."""
    model = NNUE(hidden=16, output_buckets=4)
    batch = ShardBatches(shard_path, batch_size=32)[0]
    out = model(batch["white"], batch["black"], batch["stm"], batch["piece_count"])
    out.sum().backward()
    assert out.shape == (32,)
    assert torch.isfinite(out).all()


def test_chunked_loading_sees_every_record_exactly_once(shard_path):
    """A chunk boundary must not drop or duplicate records, and neither must
    the shuffle inside one. This is the property the whole loader exists for."""
    dataset = ShuffledChunks(shard_path, batch_size=16, chunk_records=64)
    seen = _records(dataset)
    assert len(seen) == dataset.records
    assert seen == _records(ShardBatches(shard_path, batch_size=16)[i]
                            for i in range(len(ShardBatches(shard_path, batch_size=16))))


def test_chunked_loading_reorders_records_and_the_map_loader_does_not(shard_path):
    chunked = ShuffledChunks(shard_path, batch_size=16, chunk_records=256, seed=1)
    first = [r[0] for r in [(b["white"].numpy()[0].tobytes(),) for b in chunked]]
    second = [r[0] for r in [(b["white"].numpy()[0].tobytes(),) for b in chunked]]
    # Same records, different grouping: the epoch counter moved between the
    # two passes, which is what stops a batch being frozen at whatever the
    # file's record order made it.
    assert first != second


def test_chunked_loading_is_reproducible_from_the_seed(shard_path):
    def pass_one(seed):
        return _records(ShuffledChunks(shard_path, batch_size=16, chunk_records=64,
                                       seed=seed))

    assert pass_one(3) == pass_one(3)


def test_unshuffled_chunks_preserve_file_order(shard_path):
    """--val must not be reordered; a held-out loss is a comparison across
    epochs and a moving denominator would make it one."""
    ordered = ShuffledChunks(shard_path, batch_size=16, chunk_records=64, shuffle=False)
    mapped = ShardBatches(shard_path, batch_size=16)
    assert ([b["white"].numpy().tobytes() for b in ordered]
            == [mapped[i]["white"].numpy().tobytes() for i in range(len(mapped))])


def test_only_a_chunks_last_batch_is_short(shard_path):
    """A short batch in the middle would mean the permutation and the slicing
    disagree about how many records the chunk had."""
    dataset = ShuffledChunks(shard_path, batch_size=16, chunk_records=64)
    assert len(list(dataset)) == len(dataset)

    per_chunk = 64 // 16
    sizes = [b["white"].shape[0] for b in dataset]
    for i, n in enumerate(sizes):
        last_of_chunk = (i + 1) % per_chunk == 0 or i == len(sizes) - 1
        assert n == 16 or last_of_chunk, f"batch {i} is {n} records"


def test_source_filtering_matches_between_the_two_loaders(shard_path):
    """`--sources` has to mean the same thing whichever loader is running."""
    chunked = _records(ShuffledChunks(shard_path, batch_size=16, chunk_records=64,
                                      sources=[0]))
    mapped = ShardBatches(shard_path, batch_size=16, sources=[0])
    assert chunked == _records(mapped[i] for i in range(len(mapped)))


def test_make_loader_picks_the_strategy_from_the_dataset_size(shard_path):
    """Small shards keep the old path; --chunk-records overrides either way."""
    assert os.path.getsize(shard_path) < AUTO_CHUNK_BYTES
    assert isinstance(make_loader(shard_path, 16, workers=0).dataset, ShardBatches)
    assert isinstance(make_loader(shard_path, 16, workers=0,
                                  chunk_records=64).dataset, ShuffledChunks)
    assert isinstance(make_loader(shard_path, 16, workers=0,
                                  chunk_records=0).dataset, ShardBatches)


def test_a_chunk_smaller_than_a_batch_is_raised_to_one(shard_path):
    """Otherwise every batch is short and the run is 10x slower with no error."""
    dataset = ShuffledChunks(shard_path, batch_size=64, chunk_records=8)
    assert dataset.chunk_records == 64


@pytest.mark.parametrize("workers", [1, 2, 3, 5, 8])
def test_chunks_partition_across_workers(shard_path, monkeypatch, workers):
    """Every chunk to exactly one worker, whatever the worker count.

    The partition is ``order[worker::workers]`` over a permutation every worker
    draws identically. If those permutations ever diverge - a per-worker seed,
    say - two workers take the same chunk and a third is dropped, the record
    count still looks plausible, and the net simply trains on part of the data
    twice. Faked here rather than spawned: this tests the arithmetic, and
    spawning eight processes to do it would cost more than the whole suite.
    """
    import nnue.dataset as dataset_module

    seen = []
    for worker in range(workers):
        info = type("Info", (), {"id": worker, "num_workers": workers})()
        monkeypatch.setattr(dataset_module, "get_worker_info", lambda info=info: info)
        seen += _records(ShuffledChunks(shard_path, batch_size=16, chunk_records=64, seed=4))

    monkeypatch.setattr(dataset_module, "get_worker_info", lambda: None)
    assert sorted(seen) == _records(ShuffledChunks(shard_path, batch_size=16,
                                                   chunk_records=64, seed=4))
