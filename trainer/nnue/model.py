"""The network.

    feature transformer   24576 -> H, shared weights, one accumulator per side
    concatenate           [stm accumulator ; non-stm accumulator] -> 2H
    activation            SCReLU, clamp(x, 0, 1)^2 in float / [0, QA^2] in int
    output                2H -> B, the row chosen by piece count

Two things are choices, and both are pure SHAPE - a header field the engine
reads out of the net file, needing no C change to move:

    --hidden           H, a multiple of 16, up to NNUE_MAX_HIDDEN (2048)
    --output-buckets   B, any divisor of 32

Nothing else is a choice. The feature set is (32 mirrored king squares, piece,
square) and the activation is SCReLU, because those are the ones worth running
and the inference path in src/nnue.c is written for them: no branch per unit,
no branch per piece. A second activation or feature set is a case in
nnue_output() or nnue_perspective() and a NEW enum value, not a flag.

WHY SCReLU. clamp(x, 0, 1)^2 rather than clamp(x, 0, 1): the square is not
piecewise linear, so a single hidden unit can express "this matters more the
more of it there is" instead of needing two units and a bias to fake a knee.
What it costs is arithmetic range - the quantised activation is QA^2 rather
than QA, so the output sum is rescaled by QA and the weights are clipped to
keep the SIMD int16 multiply in range. See WEIGHT_CLIP in format.py.

WHY EmbeddingBag. A position has at most 32 active features per perspective out
of 24576. A dense 24576-wide input matmul is ~800x wasted work;
``nn.EmbeddingBag(mode='sum')`` is exactly the sparse-sum primitive an NNUE
accumulator is, and it is where most of the gap to a purpose-built trainer
closes.
"""

from __future__ import annotations

import torch
from torch import nn

from .format import (
    ACTIVATION_NAME,
    DEFAULT_OUTPUT_BUCKETS,
    FEATURE_SET_NAME,
    NET_TO_CP,
    NUM_FEATURES,
    PAD_INDEX,
    WEIGHT_CLIP,
    check_output_buckets,
    output_bucket,
)

DEFAULT_HIDDEN = 1024

# src/nnue.h requires this of the hidden width: the accumulator is walked
# sixteen int16 lanes at a time, and a remainder loop is code that would run on
# no net anyone would train. Refusing here rather than at load is the
# difference between a flag error and an overnight run that cannot be exported.
WIDTH_MULTIPLE = 16


class NNUE(nn.Module):
    def __init__(self, hidden: int = DEFAULT_HIDDEN,
                 output_buckets: int = DEFAULT_OUTPUT_BUCKETS,
                 uncertainty: bool = False):
        super().__init__()
        if hidden < 1 or hidden % WIDTH_MULTIPLE:
            raise ValueError(f"hidden width must be a multiple of {WIDTH_MULTIPLE}, "
                             f"got {hidden} - src/nnue.c would refuse the net")

        self.hidden = hidden
        self.output_buckets = check_output_buckets(output_buckets)
        self.uncertainty = bool(uncertainty)

        # One extra row for the padding slot. padding_idx pins it to zero and
        # keeps it there: it takes no gradient, so a record with 12 pieces
        # contributes exactly 12 features and not 12 plus twenty zeros that
        # drift.
        self.ft = nn.EmbeddingBag(NUM_FEATURES + 1, hidden, mode="sum", padding_idx=PAD_INDEX)
        self.ft_bias = nn.Parameter(torch.zeros(hidden))
        self.out = nn.Linear(2 * hidden, self.output_buckets)

        # The uncertainty head: a second output layer on the same activated
        # trunk, predicting the SCALE of the value head's own error against the
        # search label - E[|score - value|] - per bucket, in the same units as
        # the value. It exists so search.c can widen its pruning margins where
        # the evaluation is unreliable and tighten them where it is not (the
        # idea E20 measured with a cruder signal). Optional because the head is
        # a file-format feature: a net without one is still a complete net.
        if self.uncertainty:
            self.unc = nn.Linear(2 * hidden, self.output_buckets)

        # Small enough that a 32-piece sum starts inside the activation's
        # active range. Starting outside it means most units are saturated and
        # gradient-free, and the first epoch is spent escaping that rather than
        # learning - which SCReLU makes worse, not better: its gradient at the
        # bottom of the range is 2x, so a unit parked near zero learns slowly
        # from both ends.
        nn.init.uniform_(self.ft.weight, -0.02, 0.02)
        nn.init.uniform_(self.out.weight, -0.05, 0.05)
        nn.init.zeros_(self.out.bias)
        if self.uncertainty:
            nn.init.uniform_(self.unc.weight, -0.05, 0.05)
            nn.init.zeros_(self.unc.bias)
        with torch.no_grad():
            self.ft.weight[PAD_INDEX].zero_()

    # ------------------------------------------------------------ shape ----

    @property
    def arch(self) -> dict:
        """Everything the exporter needs to describe this net in a header.

        Written into every checkpoint so a .pt is self-describing: nothing
        downstream is ever told what it is holding, which is the same reason
        the net file carries its own shape. The two constants are in here so
        that a checkpoint from a build with a different feature set is refused
        rather than exported under the wrong tag.
        """
        return {
            "hidden": self.hidden,
            "output_buckets": self.output_buckets,
            "features": FEATURE_SET_NAME,
            "activation": ACTIVATION_NAME,
            "uncertainty": self.uncertainty,
        }

    def describe(self) -> str:
        return (f"{NUM_FEATURES} -> {self.hidden}x2 -> {self.output_buckets}, "
                f"{ACTIVATION_NAME}, {FEATURE_SET_NAME}"
                + (", +uncertainty" if self.uncertainty else ""))

    # -------------------------------------------------------- the forward --

    def activate(self, x: torch.Tensor) -> torch.Tensor:
        """SCReLU, in the units the quantised net will use.

        Clamped to [0, 1] first - the float image of [0, QA] - and squared
        after. Squaring first would make negative accumulators positive, which
        is a different and much worse function that still trains.
        """
        clamped = torch.clamp(x, 0.0, 1.0)
        return clamped * clamped

    def accumulators(self, white: torch.Tensor, black: torch.Tensor):
        """The two perspective accumulators, before the perspective swap.

        Exposed because the exporter and the C incremental-update assert both
        need to compare against exactly this quantity.
        """
        return self.ft(white) + self.ft_bias, self.ft(black) + self.ft_bias

    def forward(self, white: torch.Tensor, black: torch.Tensor, stm: torch.Tensor,
                piece_count: torch.Tensor | None = None) -> torch.Tensor:
        """`stm` is 1.0 when black is to move, shaped (B, 1).

        The output is side-to-move relative, like ``eval_evaluate``: the side
        to move always reads its own accumulator first. Getting this backwards
        produces a net that plays reasonably and hates its own position, and a
        loss curve that looks completely normal - which is why
        :mod:`nnue.sanity` scores the start position rather than trusting the
        curve.

        `piece_count` selects the output bucket and is required whenever there
        is more than one. It is not optional-with-a-default on purpose: a
        default would silently evaluate every position out of the opening
        bucket, and the loss curve would look completely normal for that too.
        """
        return self._pick(self.out(self._activated(white, black, stm)), piece_count)

    def forward_heads(self, white: torch.Tensor, black: torch.Tensor, stm: torch.Tensor,
                      piece_count: torch.Tensor | None = None) -> tuple:
        """Value and predicted-|error|, sharing one pass over the trunk.

        Only valid on a net built with ``uncertainty=True``; the training loop
        is the caller. The uncertainty output is raw and unclamped here - the
        non-negativity clamp is an inference-time convention, applied
        identically by the exporter's reference and src/nnue.c, and clamping in
        training would zero the gradient exactly where the head most needs to
        learn it overshot.
        """
        x = self._activated(white, black, stm)
        return self._pick(self.out(x), piece_count), self._pick(self.unc(x), piece_count)

    def _activated(self, white: torch.Tensor, black: torch.Tensor,
                   stm: torch.Tensor) -> torch.Tensor:
        acc_white, acc_black = self.accumulators(white, black)

        own = acc_black * stm + acc_white * (1.0 - stm)
        other = acc_white * stm + acc_black * (1.0 - stm)

        return self.activate(torch.cat([own, other], dim=1))

    def _pick(self, y: torch.Tensor, piece_count: torch.Tensor | None) -> torch.Tensor:
        if self.output_buckets == 1:
            return y.squeeze(1)
        if piece_count is None:
            raise ValueError(f"this net has {self.output_buckets} output buckets and needs "
                             f"piece_count to choose one")

        bucket = output_bucket(piece_count.long(), self.output_buckets)
        return y.gather(1, bucket.view(-1, 1)).squeeze(1)

    def evaluate_cp(self, white: torch.Tensor, black: torch.Tensor, stm: torch.Tensor,
                    piece_count: torch.Tensor | None = None) -> torch.Tensor:
        """The forward pass in centipawns, the units search.c's margins assume."""
        return self.forward(white, black, stm, piece_count) * NET_TO_CP

    # ------------------------------------------------------------- clipping --

    @torch.no_grad()
    def clip_weights(self, bound: float = WEIGHT_CLIP) -> None:
        """Hold the weights inside the range the quantised engine represents.

        Called after every optimiser step. This is not regularisation and it is
        not optional: the bound is what lets the exporter promise that `QA * w`
        fits int16, which is what the SIMD activation multiply needs, and it is
        what keeps the int16 accumulator in range by construction. Clipping
        during training rather than at export is the difference between a net
        that is 0.1% worse than its float self and one that has weights lopped
        off it at the end.
        """
        self.ft.weight.clamp_(-bound, bound)
        self.ft_bias.clamp_(-bound, bound)
        self.out.weight.clamp_(-bound, bound)
        # The uncertainty head multiplies the same QA-clamped activation as the
        # value head in the same int16 SIMD lanes, so it lives under the same
        # bound for the same reason.
        if self.uncertainty:
            self.unc.weight.clamp_(-bound, bound)
        # clamp_ leaves a zero row at zero, but the pad row is load-bearing
        # enough to re-pin rather than reason about.
        self.ft.weight[PAD_INDEX].zero_()


def arch_from_checkpoint(state: dict) -> dict:
    """The architecture a checkpoint describes, or a loud failure.

    There is no fallback for a checkpoint that does not carry one. A .pt from
    before the architecture was recorded is a different network - a different
    feature set, a different activation - and the only two things that could be
    done with it are to refuse it and to export it as something it is not.
    """
    arch = state.get("arch")
    if not arch:
        raise SystemExit(
            "this checkpoint carries no 'arch' field, so it predates the current network "
            "and describes a model this build cannot run. Retrain: the feature set and "
            "the activation both changed, and there is nothing in the file that says "
            "which ones it used."
        )

    for field, expected in (("features", FEATURE_SET_NAME), ("activation", ACTIVATION_NAME)):
        if arch.get(field) != expected:
            raise SystemExit(
                f"checkpoint was trained with {field} {arch.get(field)!r}, this build runs "
                f"{expected!r}. Retrain, or add the case to src/nnue.c and the exporter."
            )

    return {
        "hidden": int(arch["hidden"]),
        "output_buckets": int(arch["output_buckets"]),
        "features": FEATURE_SET_NAME,
        "activation": ACTIVATION_NAME,
        # Absent from every checkpoint written before the head existed, and
        # absent means the same thing as False: no head. That is a safe
        # default in a way a shape default would not be - the weights either
        # contain an `unc.*` tensor or they do not, and load_state_dict cross
        # checks it against this flag either way.
        "uncertainty": bool(arch.get("uncertainty", False)),
    }


def from_checkpoint(state: dict) -> NNUE:
    """A model with the checkpoint's own architecture, weights loaded.

    Never construct an NNUE from remembered flags to load a checkpoint into:
    a width or a bucket count that disagrees fails loudly here, but weights
    loaded under the wrong feature set would run, and be wrong.
    """
    arch = arch_from_checkpoint(state)
    model = NNUE(hidden=arch["hidden"], output_buckets=arch["output_buckets"],
                 uncertainty=arch["uncertainty"])
    model.load_state_dict(state["model"])
    return model


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


def uncertainty_loss_fn(unc_prediction: torch.Tensor, value_prediction: torch.Tensor,
                        score: torch.Tensor) -> torch.Tensor:
    """L1 against the value head's own absolute error, in net units.

    The target is |search score - value|, DETACHED: the uncertainty head
    observes the value head and must never steer it - a joint target would let
    the value head shrink the loss by making its errors more predictable
    rather than smaller. The trunk still receives the head's gradient, which
    is the deliberate part of joint training: features that explain the
    error's size are features worth having.

    L1 rather than MSE because the prediction is then the conditional MEDIAN
    of |error| rather than its mean, which a heavy-tailed residual would
    otherwise let a few blown positions dominate. In cp space via NET units so
    the head's output rides the same quantisation pipeline as the value's.
    """
    residual = (score / NET_TO_CP - value_prediction).abs().detach()
    return torch.nn.functional.l1_loss(unc_prediction, residual)
