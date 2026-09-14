"""The feature transformer as a sparse matrix product.

The accumulator is ``sum of W[f] over the position's active features``, and
there are two ways to spell that. ``nn.EmbeddingBag`` is the obvious one and is
what this trainer used to do; it is also, measured on an RTX 3070 at hidden 512
and batch 16384, 5.7 ms per perspective against 2.0 ms for the same arithmetic
written as ``S @ W`` with S a CSR matrix of ones. Almost all of the difference
is the BACKWARD: EmbeddingBag's dense backward sorts the index array and runs a
segment reduction over a gradient the full width of the table, every step.

So the accumulator is a sparse matmul here:

    acc   = S @ W          S is (rows, 24576), one row per position-perspective
    dL/dW = S^T @ dL/dacc

and both are plain cuSPARSE SpMM calls. The transpose is built explicitly
rather than left to autograd, because ``torch.sparse.mm``'s own backward runs
the TRANSPOSED SpMM kernel, which measured slower than building S^T and running
the untransposed one.

THREE THINGS MAKE THIS FASTER THAN THE EMBEDDING BAG, and it is worth being
precise about which, because only the first is about the kernel:

  * the padding is gone. A padded (B, 32) matrix is half padding at the piece
    counts real data has - gen-005 averages 15.4 pieces - and EmbeddingBag
    multiplies by every one of those pad slots. A sparsity pattern simply does
    not contain them, so the FT does half the work.
  * the perspective swap happens on the indices, in the loader, not on the
    accumulators. See ``unpack_sparse``.
  * the backward is one SpMM instead of a sort plus a segment reduction.

ON DETERMINISM, since it is the obvious objection: cuSPARSE's SpMM reduces in
whatever order its work partitioning lands on, so two runs from one seed are no
longer bit-identical, where EmbeddingBag's sorted backward made them so. This
does not matter here, and it is worth saying why rather than leaving it as a
worry. The disagreement is ~1e-6 relative - fp32 summation order, the same
arithmetic in a different sequence - and training is chaotic in that
perturbation anyway: any two runs diverge visibly within an epoch whatever the
reduction order. Nothing downstream reads a checkpoint bit-wise. Nets are
judged by playing them, which is an SPRT with its own error bars measured in
Elo, not in ulps. Invariant 1 is about the ENGINE's bench being reproducible,
and no trainer output feeds it.

What does have to hold is that this computes the same function as the
EmbeddingBag path, and that is a testable claim rather than a hopeful one:
``trainer/tests/test_sparse.py`` checks outputs AND gradients across every
architecture the flags can build, on real records, to 1e-5 relative.
"""

from __future__ import annotations

import warnings

import torch

from .format import MAX_PIECES, NUM_FEATURES

# Feature indices reach 24575 and row indices reach 2 * batch. Both are packed
# into one int32 sort key below, so the batch this supports is bounded by
# 2^31 / 32768 - which is 65536 positions, well past any batch that fits in a
# consumer GPU. Refusing loudly beats a silently wrapped key.
_KEY_SHIFT = 32768
MAX_SORT_ROWS = (2 ** 31 - 1) // _KEY_SHIFT

def _swallow_csr_beta_notice() -> None:
    """Spend torch's one-shot "CSR is beta" warning here, on a 1x1 matrix.

    CSR is the format this module exists to use, so the notice carries no
    information - but it is a TORCH_WARN_ONCE, which means whichever batch
    happens to build the first pattern prints it into the middle of a training
    log. Triggering it deliberately at import, with the filter in place, is the
    only way to silence it that does not put a warnings context manager inside
    the step. A filter alone is not enough: pytest resets the filters around
    every test, so the notice would come back in the suite.
    """
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        torch.sparse_csr_tensor(torch.zeros(2, dtype=torch.int32),
                                torch.zeros(0, dtype=torch.int32),
                                torch.zeros(0), size=(1, 1), check_invariants=False)


_swallow_csr_beta_notice()


class _SparseFT(torch.autograd.Function):
    """``S @ table``, with the backward run off a prebuilt ``S^T``."""

    @staticmethod
    def forward(ctx, table, forward_csr, transpose_csr):
        ctx.transpose_csr = transpose_csr
        return torch.sparse.mm(forward_csr, table)

    @staticmethod
    def backward(ctx, grad):
        # .contiguous() because cuSPARSE needs a dense operand it can stride
        # over, and the incoming gradient is a slice often enough to matter.
        return torch.sparse.mm(ctx.transpose_csr, grad.contiguous()), None, None


class Pattern:
    """The sparsity pattern of one batch, as S and S^T in CSR.

    Built once per batch and read twice - forward and backward - so the two
    sorts it costs are paid once for both.
    """

    __slots__ = ("forward_csr", "transpose_csr", "rows", "nnz")

    def __init__(self, forward_csr, transpose_csr, rows, nnz):
        self.forward_csr = forward_csr
        self.transpose_csr = transpose_csr
        self.rows = rows
        self.nnz = nnz

    def apply(self, table: torch.Tensor) -> torch.Tensor:
        """The stacked accumulators: (2 * batch, hidden), own on top."""
        return _SparseFT.apply(table, self.forward_csr, self.transpose_csr)


class _Scratch:
    """Per-device buffers the pattern builder reuses.

    Rebuilding an arange and a vector of ones every batch is two allocations
    and two kernel launches per step for numbers that never change. At the
    sizes involved the allocation is not the problem - the launch is, because
    this whole step is bound by how fast the CPU can issue work.
    """

    __slots__ = ("rows", "ones", "bounds", "columns")

    def __init__(self):
        self.rows = self.ones = self.bounds = None
        self.columns = -1

    def take(self, device, rows: int, columns: int):
        if self.rows is None or self.rows.numel() < rows:
            self.rows = torch.arange(rows, device=device, dtype=torch.int32)
            # One entry per feature slot a batch could occupy. MAX_PIECES is
            # the ceiling on a position's features, so this never regrows.
            self.ones = torch.ones(rows * MAX_PIECES, device=device)
        if self.columns != columns or self.bounds is None or self.bounds.device != device:
            # Where feature f's block starts, in the packed sort key below.
            self.bounds = (torch.arange(columns + 1, device=device, dtype=torch.int32)
                           * _KEY_SHIFT)
            self.columns = columns
        return self.rows[:rows], self.ones, self.bounds


_scratch = _Scratch()


def build_pattern(own: torch.Tensor, other: torch.Tensor, counts: torch.Tensor,
                  columns: int = NUM_FEATURES) -> Pattern:
    """A :class:`Pattern` from what ``unpack_sparse`` produced.

    ``own`` and ``other`` are tightly packed feature indices in record order,
    ``counts`` the pieces per record. Own and other describe the same position
    from two sides, so they share one length per record and stack into a single
    matrix of 2 * batch rows - one SpMM for both perspectives rather than two.

    NOTHING HERE MAY READ A VALUE BACK TO THE HOST. This runs once per step in
    a loop whose limit is how fast the CPU issues work, and a single
    ``int(tensor)`` costs far more than the arithmetic it guards: it drains the
    queue, so the GPU finishes everything outstanding and then sits idle while
    Python catches up. That is why the row count comes from ``numel()`` rather
    than from the last row pointer, why ``repeat_interleave`` is told its
    output size, and why the transpose's row pointers come from
    ``searchsorted`` rather than from ``bincount``.
    """
    if own.shape != other.shape:
        raise ValueError(f"own and other must match: {tuple(own.shape)} vs {tuple(other.shape)}")
    device = own.device
    batch = counts.numel()
    rows = 2 * batch
    nnz = 2 * own.numel()
    if rows > MAX_SORT_ROWS:
        raise ValueError(f"batch of {batch} needs {rows} matrix rows, past the {MAX_SORT_ROWS} "
                         f"this module's int32 sort key can address")
    row_ids, ones, bounds = _scratch.take(device, rows, columns)
    ones = ones[:nnz]

    lengths = torch.cat([counts, counts]).to(torch.int32)
    crow = torch.zeros(rows + 1, device=device, dtype=torch.int32)
    torch.cumsum(lengths, 0, dtype=torch.int32, out=crow[1:])

    cols = torch.cat([own, other]).to(torch.int32)
    row_of = torch.repeat_interleave(row_ids, lengths, output_size=nnz)

    # CSR wants each row's columns ascending, and a decoded record hands its
    # pieces over in square order, not feature order. Both matrices come out of
    # one packed key apiece: (row, column) sorted gives S, (column, row) gives
    # S^T. The key is exact - a column is under 2^15 and a row under 2^16 - so
    # the low half of the sorted key IS the index array, and reading it back
    # out with a mask costs less than gathering through a permutation.
    order = torch.sort(torch.add(cols, row_of, alpha=_KEY_SHIFT)).values
    forward_csr = torch.sparse_csr_tensor(crow, order & (_KEY_SHIFT - 1), ones,
                                          size=(rows, columns), check_invariants=False)

    order_t = torch.sort(torch.add(row_of, cols, alpha=_KEY_SHIFT)).values
    # Feature f's block starts at the first key at or past f * _KEY_SHIFT. That
    # is the transpose's row pointer, straight out of the sorted key, with no
    # histogram and no host round trip.
    crow_t = torch.searchsorted(order_t, bounds, out_int32=True)
    transpose_csr = torch.sparse_csr_tensor(crow_t, order_t & (_KEY_SHIFT - 1), ones,
                                            size=(columns, rows), check_invariants=False)

    return Pattern(forward_csr, transpose_csr, rows, nnz)
