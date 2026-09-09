"""A training reparameterisation must disappear completely at export."""

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from export_net import check_ranges, forward, quantise  # noqa: E402
from nnue.format import NUM_FEATURES, PAD_INDEX, QA, QB, SCALE, WEIGHT_CLIP  # noqa: E402
from nnue.model import (NNUE, SHARED_FEATURES, SHARED_PAD_INDEX,  # noqa: E402
                        effective_feature_weights, from_checkpoint)


def inputs():
    # Different king slots, repeated shared piece-square features, and padding.
    white = torch.tensor([[0, 2, PAD_INDEX], [768, 770, 775]], dtype=torch.int32)
    black = torch.tensor([[769, 771, PAD_INDEX], [1, 3, 6]], dtype=torch.int32)
    return white, black, torch.tensor([[0.0], [1.0]]), torch.tensor([2, 3])


def test_zero_factor_preserves_initial_function_and_rng():
    torch.manual_seed(7)
    control = NNUE(16, 8, uncertainty=True)
    after_control = torch.get_rng_state()
    torch.manual_seed(7)
    factored = NNUE(16, 8, uncertainty=True, feature_factorization=True)
    assert torch.equal(torch.get_rng_state(), after_control)
    for a, b in zip(control.forward_heads(*inputs()), factored.forward_heads(*inputs())):
        torch.testing.assert_close(a, b, rtol=0, atol=0)


@pytest.mark.parametrize("uncertainty", [False, True])
def test_folding_preserves_accumulators_outputs_and_checkpoint(uncertainty):
    model = NNUE(16, 8, uncertainty=uncertainty, feature_factorization=True)
    with torch.no_grad():
        model.ft_shared.weight.uniform_(-0.03, 0.03)
    model.clip_weights()
    folded = NNUE(16, 8, uncertainty=uncertainty)
    folded.load_state_dict(model.folded_state_dict())
    for a, b in zip(model.accumulators(*inputs()[:2]), folded.accumulators(*inputs()[:2])):
        torch.testing.assert_close(a, b, rtol=1e-5, atol=1e-7)
    torch.testing.assert_close(model(*inputs()), folded(*inputs()), rtol=1e-5, atol=1e-7)
    restored = from_checkpoint({"model": model.state_dict(), "arch": model.arch})
    assert restored.feature_factorization
    torch.testing.assert_close(restored(*inputs()), model(*inputs()), rtol=0, atol=0)
    if uncertainty:
        for a, b in zip(model.forward_heads(*inputs()), folded.forward_heads(*inputs())):
            torch.testing.assert_close(a, b, rtol=1e-5, atol=1e-7)


def test_shared_gradients_sum_across_king_slots_and_padding_is_inert():
    model = NNUE(16, feature_factorization=True)
    features = torch.tensor([[2, 770, PAD_INDEX]], dtype=torch.int32)
    model._accumulate(features).sum().backward()
    assert torch.all(model.ft.weight.grad[2] == 1)
    assert torch.all(model.ft.weight.grad[770] == 1)
    assert torch.all(model.ft_shared.weight.grad[2] == 2)
    assert torch.count_nonzero(model.ft_shared.weight.grad[SHARED_PAD_INDEX]) == 0
    assert torch.count_nonzero(model.ft.weight.grad[PAD_INDEX]) == 0


def test_clipping_bounds_the_sum_not_just_the_two_components():
    model = NNUE(16, 8, uncertainty=True, feature_factorization=True)
    with torch.no_grad():
        model.ft.weight.uniform_(-4, 4)
        model.ft_shared.weight.uniform_(-4, 4)
    before = (model.ft.weight[:-1].view(-1, SHARED_FEATURES, 16)
              + model.ft_shared.weight[:-1]).clamp(-WEIGHT_CLIP, WEIGHT_CLIP).detach().clone()
    model.clip_weights()
    actual = model.folded_state_dict()["ft.weight"]
    torch.testing.assert_close(actual[:-1].view_as(before), before, rtol=0, atol=3e-7)
    assert actual[:-1].abs().max() <= WEIGHT_CLIP + 3e-7
    assert torch.count_nonzero(actual[-1]) == 0
    assert torch.count_nonzero(model.ft_shared.weight[-1]) == 0


def test_export_folds_before_rounding_and_has_the_original_shape():
    model = NNUE(16, 8, uncertainty=True, feature_factorization=True)
    with torch.no_grad():
        model.ft.weight.fill_(0.49 / QA)
        model.ft_shared.weight.fill_(0.49 / QA)
    model.clip_weights()
    q = quantise(model.state_dict(), model.arch, QA, QB)
    assert q["ft_w"].shape == (NUM_FEATURES, 16)
    assert np.all(q["ft_w"] == 1)  # rounding each component first would give zero
    q["qb"] = QB
    check_ranges(q, QA)
    folded = NNUE(16, 8, uncertainty=True)
    folded.load_state_dict(model.folded_state_dict())
    other = quantise(folded.state_dict(), folded.arch, QA, QB)
    for name in ("ft_w", "ft_b", "out_w", "out_b", "unc_w", "unc_b"):
        np.testing.assert_array_equal(q[name], other[name])
    white, black, stm, pieces = inputs()
    fields = {"white": white.numpy(), "black": black.numpy(),
              "stm": stm.squeeze(1).numpy(), "piece_count": pieces.numpy()}
    for a, b in zip(forward(q, fields, QA, QB, SCALE), forward(other, fields, QA, QB, SCALE)):
        np.testing.assert_array_equal(a, b)


@pytest.mark.parametrize("factorized", [False, True])
def test_metadata_cannot_silently_drop_or_invent_a_factor(factorized):
    model = NNUE(16, feature_factorization=factorized)
    with pytest.raises(ValueError, match="metadata"):
        effective_feature_weights(model.state_dict(), not factorized)
    with pytest.raises(SystemExit, match="metadata"):
        quantise(model.state_dict(), dict(model.arch, feature_factorization=not factorized), QA, QB)


def test_old_checkpoint_without_factorization_field_still_loads():
    model = NNUE(16)
    arch = dict(model.arch)
    del arch["feature_factorization"]
    restored = from_checkpoint({"model": model.state_dict(), "arch": arch})
    assert not restored.feature_factorization
    torch.testing.assert_close(restored(*inputs()), model(*inputs()), rtol=0, atol=0)


def test_factorized_model_can_learn_and_both_factors_stay_exportable():
    from nnue.model import loss_fn

    torch.manual_seed(0)
    model = NNUE(16, 8, uncertainty=True, feature_factorization=True)
    optimizer = torch.optim.AdamW(model.parameters(), lr=.01, weight_decay=0)
    target = torch.tensor([.25, .75])
    first = loss_fn(model(*inputs()), target, 400).item()
    for _ in range(150):
        optimizer.zero_grad(set_to_none=True)
        loss = loss_fn(model(*inputs()), target, 400)
        loss.backward()
        optimizer.step()
        model.clip_weights()
    assert loss.item() < first * .02
    assert torch.count_nonzero(model.ft_shared.weight[:-1]) > 0
    q = quantise(model.state_dict(), model.arch, QA, QB)
    q["qb"] = QB
    check_ranges(q, QA)