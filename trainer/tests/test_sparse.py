"""The sparse feature transformer must answer exactly what the dense one does.

There are two spellings of the accumulator in this trainer - an EmbeddingBag
over padded index matrices, and a CSR matrix product over tightly packed ones -
and training runs the second while the exporter, the sanity table and every
equivalence test read the first. Nothing else in the repository would notice
them drifting apart: a feature transformer that is subtly wrong in training
still produces a loss curve that looks completely normal, and the net it
exports is simply worse than it should be.

So this file is the gate. It checks the two agree on OUTPUTS AND ON GRADIENTS,
across every architecture the flags can build, on real records rather than
random indices - a position has a king per side and a piece count that a random
index matrix would not reproduce, and the perspective normalisation is driven
by exactly those.

The tolerance is 1e-5 relative, against a measured worst case around 4e-7. The
two paths sum the same fp32 numbers in different orders, so they are not
bit-identical and no tolerance of zero is available; 1e-5 is two orders of
magnitude clear of the noise and still far tighter than anything that could
hide a real disagreement.
"""

import numpy as np
import pytest
import torch

from nnue.format import PAD_INDEX, read_shard, unpack, unpack_sparse
from nnue.model import NNUE
from nnue.sparse import MAX_SORT_ROWS, build_pattern

TOLERANCE = 1e-5

# Small enough to stay quick on CPU, wide enough that a bucket gets used.
ARCHITECTURES = [
    dict(hidden=32, output_buckets=1),
    dict(hidden=32, output_buckets=4),
    dict(hidden=32, output_buckets=8, uncertainty=True),
    dict(hidden=32, output_buckets=4, feature_factorization=True),
    dict(hidden=32, output_buckets=8, uncertainty=True, feature_factorization=True),
    dict(hidden=32, output_buckets=4, l1_size=16),
    dict(hidden=32, output_buckets=4, l1_size=16, l2_size=16, uncertainty=True),
]


def _ids(architectures):
    return [",".join(f"{k}={v}" for k, v in a.items()) for a in architectures]


@pytest.fixture(scope="module")
def batches(shard_path):
    """One batch of real records, in both presentations."""
    records = np.array(read_shard(shard_path, count=512))
    dense = unpack(records, index_dtype=np.int64)
    sparse = unpack_sparse(records)
    padded = {
        "white": torch.from_numpy(dense["white"]),
        "black": torch.from_numpy(dense["black"]),
        "stm": torch.from_numpy(dense["stm"]).float().unsqueeze(1),
        "piece_count": torch.from_numpy(dense["piece_count"]),
    }
    packed = {k: torch.from_numpy(np.ascontiguousarray(v))
              for k, v in sparse.items() if k != "in_check"}
    return padded, packed


def _run(model, padded, packed, sparse: bool):
    """Outputs and gradients from one path."""
    model.zero_grad(set_to_none=True)
    if sparse:
        outputs = (model.forward_heads_sparse(packed) if model.uncertainty
                   else (model.forward_sparse(packed),))
    else:
        args = (padded["white"], padded["black"], padded["stm"], padded["piece_count"])
        outputs = model.forward_heads(*args) if model.uncertainty else (model(*args),)
    # A quadratic reaches every parameter, including the ones a plain sum would
    # give a constant gradient and so could not tell apart.
    sum(output.square().sum() for output in outputs).backward()
    grads = {name: p.grad.detach().clone()
             for name, p in model.named_parameters() if p.grad is not None}
    return tuple(o.detach() for o in outputs), grads


def _relative(a, b):
    scale = max(a.abs().max().item(), 1e-12)
    return (a - b).abs().max().item() / scale


@pytest.mark.parametrize("arch", ARCHITECTURES, ids=_ids(ARCHITECTURES))
def test_sparse_matches_dense_outputs_and_gradients(arch, batches):
    padded, packed = batches
    torch.manual_seed(0)
    model = NNUE(**arch)
    # Off zero, or a freshly built net's biases would make half the comparison
    # trivially true.
    with torch.no_grad():
        for parameter in model.parameters():
            parameter.add_(torch.randn_like(parameter) * 0.01)
        model.ft.weight[PAD_INDEX].zero_()

    dense_out, dense_grads = _run(model, padded, packed, sparse=False)
    sparse_out, sparse_grads = _run(model, padded, packed, sparse=True)

    assert len(dense_out) == len(sparse_out)
    for dense, sparse in zip(dense_out, sparse_out):
        assert dense.shape == sparse.shape
        assert _relative(dense, sparse) < TOLERANCE

    # Every parameter that takes a gradient on one path must take one on the
    # other. A path that silently stopped training the shared factor, or the
    # padding row, would otherwise pass on outputs alone.
    assert set(dense_grads) == set(sparse_grads)
    for name, dense in dense_grads.items():
        assert _relative(dense, sparse_grads[name]) < TOLERANCE, name


def test_the_padding_row_takes_no_gradient(batches):
    """padding_idx buys this on the dense path; on the sparse one it is that a
    sparsity pattern has no pad entries to carry gradient back through."""
    padded, packed = batches
    torch.manual_seed(1)
    model = NNUE(hidden=32, output_buckets=4)
    _, grads = _run(model, padded, packed, sparse=True)
    assert not torch.count_nonzero(grads["ft.weight"][PAD_INDEX])


def test_the_pattern_describes_the_records_it_was_built_from(batches):
    """The CSR is the batch: one row per position-perspective, one entry per
    piece, and the row pointers in step with the piece counts."""
    _, packed = batches
    pattern = build_pattern(packed["own"], packed["other"], packed["counts"])
    batch = packed["counts"].numel()

    assert pattern.rows == 2 * batch
    assert pattern.nnz == 2 * int(packed["counts"].sum())

    crow = pattern.forward_csr.crow_indices()
    lengths = (crow[1:] - crow[:-1]).to(torch.int32)
    assert torch.equal(lengths, torch.cat([packed["counts"]] * 2).to(torch.int32))

    # CSR requires each row's columns ascending, and the records arrive in
    # square order rather than feature order - so this is the sort doing its
    # job, not a property of the input.
    columns = pattern.forward_csr.col_indices()
    for row in range(0, 2 * batch, 97):
        lo, hi = int(crow[row]), int(crow[row + 1])
        block = columns[lo:hi]
        assert torch.equal(block, torch.sort(block).values)

    # S and its transpose have to hold the same entries, or the backward is
    # computing a gradient for a matrix the forward did not use.
    assert pattern.transpose_csr.col_indices().numel() == pattern.nnz
    assert torch.equal(torch.sort(pattern.transpose_csr.values()).values,
                       torch.sort(pattern.forward_csr.values()).values)


def test_a_batch_too_wide_for_the_sort_key_is_refused(batches):
    """The key packs a row index and a column into one int32. Past that it
    would silently wrap, and the feature transformer would read the wrong
    rows - so it has to be a refusal, not a surprise."""
    _, packed = batches
    counts = torch.ones(MAX_SORT_ROWS, dtype=torch.int32)
    indices = torch.zeros(MAX_SORT_ROWS, dtype=torch.int16)
    with pytest.raises(ValueError, match="sort key"):
        build_pattern(indices, indices, counts)


def test_mismatched_perspectives_are_refused(batches):
    _, packed = batches
    with pytest.raises(ValueError, match="own and other"):
        build_pattern(packed["own"], packed["other"][:-1], packed["counts"])
