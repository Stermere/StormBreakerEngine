"""The network, v1.

    feature transformer   6144 -> 512, shared weights, one accumulator per side
    concatenate           [stm accumulator ; non-stm accumulator] -> 1024
    activation            clipped ReLU, [0, 1] in float / [0, 255] in int
    output                1024 -> 1

Every choice here is the conservative one, each with a known upgrade behind an
SPRT: clipped ReLU rather than squared (SCReLU needs int32 intermediates, and
bit-exact export comes first), one output rather than piece-count buckets, 512
wide rather than 1024, 8 king buckets rather than 32 mirrored king squares.
Measure the first one before widening it.

WHY EmbeddingBag. A position has at most 32 active features per perspective out
of 6144. A dense 6144-wide input matmul is about 200x wasted work;
``nn.EmbeddingBag(mode='sum')`` is exactly the sparse-sum primitive an NNUE
accumulator is, and it is where most of the gap to a purpose-built trainer
closes.
"""

from __future__ import annotations

import torch
from torch import nn

from .format import NET_TO_CP, NUM_FEATURES, PAD_INDEX

DEFAULT_HIDDEN = 512


class NNUE(nn.Module):
    def __init__(self, hidden: int = DEFAULT_HIDDEN):
        super().__init__()
        self.hidden = hidden

        # One extra row for the padding slot. padding_idx pins it to zero and
        # keeps it there: it takes no gradient, so a record with 12 pieces
        # contributes exactly 12 features and not 12 plus twenty zeros that
        # drift.
        self.ft = nn.EmbeddingBag(NUM_FEATURES + 1, hidden, mode="sum",
                                  padding_idx=PAD_INDEX)
        self.ft_bias = nn.Parameter(torch.zeros(hidden))
        self.out = nn.Linear(2 * hidden, 1)

        # Small enough that a 32-piece sum starts inside the clipped ReLU's
        # active range. Starting outside it means most units are saturated and
        # gradient-free, and the first epoch is spent escaping that rather than
        # learning.
        nn.init.uniform_(self.ft.weight, -0.02, 0.02)
        nn.init.uniform_(self.out.weight, -0.05, 0.05)
        nn.init.zeros_(self.out.bias)
        with torch.no_grad():
            self.ft.weight[PAD_INDEX].zero_()

    def accumulators(self, white: torch.Tensor, black: torch.Tensor):
        """The two perspective accumulators, before the perspective swap.

        Exposed because Task 3's exporter and the C incremental-update assert
        both need to compare against exactly this quantity.
        """
        return self.ft(white) + self.ft_bias, self.ft(black) + self.ft_bias

    def forward(self, white: torch.Tensor, black: torch.Tensor,
                stm: torch.Tensor) -> torch.Tensor:
        """`stm` is 1.0 when black is to move, shaped (B, 1).

        The output is side-to-move relative, like ``eval_evaluate``: the side
        to move always reads its own accumulator first. Getting this backwards
        produces a net that plays reasonably and hates its own position, and a
        loss curve that looks completely normal - which is why
        :mod:`nnue.sanity` scores the start position rather than trusting the
        curve.
        """
        acc_white, acc_black = self.accumulators(white, black)

        own = acc_black * stm + acc_white * (1.0 - stm)
        other = acc_white * stm + acc_black * (1.0 - stm)

        x = torch.clamp(torch.cat([own, other], dim=1), 0.0, 1.0)
        return self.out(x).squeeze(1)

    def evaluate_cp(self, white: torch.Tensor, black: torch.Tensor,
                    stm: torch.Tensor) -> torch.Tensor:
        """The forward pass in centipawns, the units search.c's margins assume."""
        return self.forward(white, black, stm) * NET_TO_CP


def blended_target(score: torch.Tensor, wdl: torch.Tensor, lam: float,
                   sigmoid_k: float) -> torch.Tensor:
    """``lambda * sigmoid(score / K) + (1 - lambda) * wdl``.

    Mostly distilling the search, with enough game result mixed in to stop the
    net inheriting the teacher's systematic errors. A record whose game result
    is unknown - a labelled EPD with no ``[x.x]`` on it - falls back to
    lambda = 1 rather than being dropped: its search score is just as good as
    any other record's, and pretending its result is a draw would be a lie the
    net would learn.
    """
    outcome = torch.sigmoid(score / sigmoid_k)
    known = wdl <= 2
    result = torch.clamp(wdl.float(), max=2.0) / 2.0
    lam_eff = torch.where(known, torch.full_like(outcome, lam),
                          torch.ones_like(outcome))
    return lam_eff * outcome + (1.0 - lam_eff) * result


def loss_fn(prediction: torch.Tensor, target: torch.Tensor,
            sigmoid_k: float) -> torch.Tensor:
    """MSE in win-probability space.

    The network's raw output is in NET_TO_CP units, so it goes through the same
    sigmoid the target did. Comparing centipawns directly would weight a
    2000cp position as heavily as the 30cp positions that actually decide
    games.
    """
    predicted = torch.sigmoid(prediction * NET_TO_CP / sigmoid_k)
    return torch.nn.functional.mse_loss(predicted, target)
