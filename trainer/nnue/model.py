"""The network.

    feature transformer   24576 -> H, shared weights, one accumulator per side
    concatenate           [stm accumulator ; non-stm accumulator] -> 2H
    activation            SCReLU, clamp(x, 0, 1)^2 in float / [0, QA^2] in int
    output                2H -> B, the row chosen by piece count

optionally with a layer stack in place of that flat output, per bucket:

    L1                    2H -> l1_size,      clamp(x, 0, 1)
    L2                    l1_size -> l2_size, clamp(x, 0, 1)     (if l2_size)
    L3                    -> B, the row chosen by piece count

Four things are choices, and all four are pure SHAPE - header fields the
engine reads out of the net file, needing no C change to move:

    --hidden           H, a multiple of 16, up to NNUE_MAX_HIDDEN (2048)
    --output-buckets   B, any divisor of 32
    --l1-size          0 for the flat output layer, else a multiple of 16
    --l2-size          0 for L1 -> L3 directly, else a multiple of 16

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
    L1_CLIP,
    L2_CLIP,
    L3_CLIP,
    NET_TO_CP,
    NUM_FEATURES,
    PAD_INDEX,
    PIECE_PLANES,
    SQUARES,
    SOURCE_NAMES,
    STACK_WIDTH_MULTIPLE,
    WEIGHT_CLIP,
    check_output_buckets,
    output_bucket,
    phase_endgameness,
    progress_closeness,
)

DEFAULT_HIDDEN = 1024

# Off by default
DEFAULT_L1_SIZE = 0
DEFAULT_L2_SIZE = 0

# src/nnue.h requires this of the hidden width: the accumulator is walked
# sixteen int16 lanes at a time, and a remainder loop is code that would run on
# no net anyone would train. Refusing here rather than at load is the
# difference between a flag error and an overnight run that cannot be exported.
WIDTH_MULTIPLE = 16
SHARED_FEATURES = PIECE_PLANES * SQUARES
SHARED_PAD_INDEX = SHARED_FEATURES


def effective_feature_weights(state: dict, factorized: bool) -> torch.Tensor:
    """Fold the training-only PSQT factor BEFORE quantisation, never after it."""
    ft = state["ft.weight"]
    if ft.ndim != 2 or ft.shape[0] != NUM_FEATURES + 1:
        raise ValueError("feature transformer has the wrong shape")
    if torch.count_nonzero(ft[PAD_INDEX]):
        raise ValueError("the padding embedding drifted off zero")
    if ("ft_shared.weight" in state) != factorized:
        raise ValueError("feature_factorization metadata disagrees with the shared weights")
    if not factorized:
        return ft
    shared = state["ft_shared.weight"]
    if shared.shape != (SHARED_FEATURES + 1, ft.shape[1]):
        raise ValueError("shared feature transformer has the wrong shape")
    if torch.count_nonzero(shared[SHARED_PAD_INDEX]):
        raise ValueError("the shared padding embedding drifted off zero")
    folded = ft[:-1].reshape(-1, SHARED_FEATURES, ft.shape[1]) + shared[:-1]
    return torch.cat((folded.reshape(-1, ft.shape[1]), ft[-1:]), dim=0)


class NNUE(nn.Module):
    def __init__(self, hidden: int = DEFAULT_HIDDEN,
                 output_buckets: int = DEFAULT_OUTPUT_BUCKETS,
                 uncertainty: bool = False, feature_factorization: bool = False,
                 l1_size: int = DEFAULT_L1_SIZE, l2_size: int = DEFAULT_L2_SIZE):
        super().__init__()
        if hidden < 1 or hidden % WIDTH_MULTIPLE:
            raise ValueError(f"hidden width must be a multiple of {WIDTH_MULTIPLE}, "
                             f"got {hidden} - src/nnue.c would refuse the net")

        l1_size, l2_size = int(l1_size), int(l2_size)
        for name, width in (("l1_size", l1_size), ("l2_size", l2_size)):
            if width < 0 or (width and width % STACK_WIDTH_MULTIPLE):
                raise ValueError(
                    f"{name} must be 0 or a multiple of {STACK_WIDTH_MULTIPLE}, got {width} "
                    f"- it is an inner-loop length in src/nnue.c, which carries no "
                    f"remainder loop")
        # L2 reads L1's output. Without an L1 there is nothing for it to read,
        # and silently promoting it to "L1 of that width" would train a net
        # nobody asked for.
        if l2_size and not l1_size:
            raise ValueError("l2_size requires l1_size: L2 reads L1's output, and there "
                             "is no L1 to read. Pass --l1-size too, or drop --l2-size")

        self.hidden = hidden
        self.output_buckets = check_output_buckets(output_buckets)
        self.uncertainty = bool(uncertainty)
        self.feature_factorization = bool(feature_factorization)
        self.l1_size = l1_size
        self.l2_size = l2_size

        # One extra row for the padding slot. padding_idx pins it to zero and
        # keeps it there: it takes no gradient, so a record with 12 pieces
        # contributes exactly 12 features and not 12 plus twenty zeros that
        # drift.
        self.ft = nn.EmbeddingBag(NUM_FEATURES + 1, hidden, mode="sum", padding_idx=PAD_INDEX)
        self.ft_bias = nn.Parameter(torch.zeros(hidden))

        # EVERY per-bucket layer computes all buckets and gathers the one the
        # piece count asks for, rather than gathering the weights first. The
        # gather-weights version materialises a (batch, units, inputs) tensor -
        # a gigabyte at batch 16384 - to save compute that measures at ~3% of a
        # step. See _pick_rows().
        if self.l1_size:
            self.l1 = nn.Linear(2 * hidden, self.output_buckets * self.l1_size)
            if self.l2_size:
                self.l2 = nn.Linear(self.l1_size, self.output_buckets * self.l2_size)
            self.l3 = nn.Linear(self.trunk_width, self.output_buckets)
        else:
            self.out = nn.Linear(2 * hidden, self.output_buckets)

        # The uncertainty head: a second output layer on the same trunk,
        # predicting the SCALE of the value head's own error against the search
        # label - E[|score - value|] - per bucket, in the same units as the
        # value. It exists so search.c can widen its pruning margins where the
        # evaluation is unreliable and tighten them where it is not (the idea
        # E20 measured with a cruder signal). Optional because the head is a
        # file-format feature: a net without one is still a complete net.
        #
        # WITH A STACK IT READS THE STACK'S LAST HIDDEN LAYER, not the
        # accumulator. Three reasons, and the first is the one that decides it:
        # hanging a 2H -> 1 layer off the accumulator keeps the int16 SIMD
        # product that Task 6 exists to remove, so the head would need a
        # quantisation scale of its own while the value head no longer does.
        # It is also ~64x cheaper to evaluate, and the stack's learned features
        # are a better basis for "how wrong is this likely to be" than a linear
        # read of the accumulator. The head has to be retrained either way and
        # `make unc-probe` re-centres unc_scale() on whatever comes out, which
        # NNUE.md 5c already requires after any retrain.
        if self.uncertainty:
            self.unc = nn.Linear(self.trunk_width, self.output_buckets)

        # Small enough that a 32-piece sum starts inside the activation's
        # active range. Starting outside it means most units are saturated and
        # gradient-free, and the first epoch is spent escaping that rather than
        # learning - which SCReLU makes worse, not better: its gradient at the
        # bottom of the range is 2x, so a unit parked near zero learns slowly
        # from both ends.
        nn.init.uniform_(self.ft.weight, -0.02, 0.02)
        if self.l1_size:
            # The same argument one stage further in. A stack layer's
            # activation is clamp(x, 0, 1), so a unit that starts outside
            # [0, 1] is flat on one side and learns from neither - and with
            # 2H inputs already in [0, 1] the sum is what decides that. Torch's
            # default 1/sqrt(fan_in) init keeps the sum near zero; the bias
            # then puts it in the MIDDLE of the active range rather than on
            # its floor, which is where half the units would otherwise sit.
            for layer in self.stack_layers:
                nn.init.constant_(layer.bias, 0.5)
            nn.init.uniform_(self.l3.weight, -0.05, 0.05)
            nn.init.zeros_(self.l3.bias)
        else:
            nn.init.uniform_(self.out.weight, -0.05, 0.05)
            nn.init.zeros_(self.out.bias)
        if self.uncertainty:
            nn.init.uniform_(self.unc.weight, -0.05, 0.05)
            nn.init.zeros_(self.unc.bias)
        with torch.no_grad():
            self.ft.weight[PAD_INDEX].zero_()

        if self.feature_factorization:
            # Zero starts from the same function AND RNG state as the control.
            # The factor learns common piece-square effects across king slots.
            with torch.random.fork_rng(devices=[]):
                self.ft_shared = nn.EmbeddingBag(SHARED_FEATURES + 1, hidden, mode="sum",
                                                padding_idx=SHARED_PAD_INDEX)
            nn.init.zeros_(self.ft_shared.weight)

    # ------------------------------------------------------------ shape ----

    @property
    def trunk_width(self) -> int:
        """Width of the vector the output heads read.

        The activated accumulator without a stack, the stack's last hidden
        layer with one. Both heads read it, which is what keeps them one pass
        over one trunk.
        """
        if not self.l1_size:
            return 2 * self.hidden
        return self.l2_size or self.l1_size

    @property
    def stack_layers(self) -> list:
        """L1 and L2 - the layers with a clamp(x, 0, 1) after them.

        L3 is not one of them: it is the output layer, it has no activation,
        and its clip and its quantisation scale are the output layer's.
        """
        if not self.l1_size:
            return []
        return [self.l1] + ([self.l2] if self.l2_size else [])

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
            "feature_factorization": self.feature_factorization,
            "l1_size": self.l1_size,
            "l2_size": self.l2_size,
        }

    def describe(self) -> str:
        stack = "".join(f" -> {w}" for w in (self.l1_size, self.l2_size) if w)
        return (f"{NUM_FEATURES} -> {self.hidden}x2{stack} -> {self.output_buckets}, "
                f"{ACTIVATION_NAME}, {FEATURE_SET_NAME}"
                + (", +uncertainty" if self.uncertainty else "")
                + (", +training-only PSQT factor" if self.feature_factorization else ""))

    # -------------------------------------------------------- the forward --

    def activate(self, x: torch.Tensor) -> torch.Tensor:
        """SCReLU, in the units the quantised net will use.

        Clamped to [0, 1] first - the float image of [0, QA] - and squared
        after. Squaring first would make negative accumulators positive, which
        is a different and much worse function that still trains.
        """
        clamped = torch.clamp(x, 0.0, 1.0)
        return clamped * clamped

    def stack_activate(self, x: torch.Tensor) -> torch.Tensor:
        """The activation between stack layers: clipped ReLU, not SCReLU.

        The float image of `min(max(s, 0) >> SHIFT, 255)`, which is the one
        integer stage src/nnue.c will run for L1 and L2. Both halves matter:
        the clamp at zero is the ReLU, and the clamp at one is the
        requantisation ceiling that puts the result back in the [0, 255] range
        the next stage's weights are scaled against. Training without the
        upper clamp would fit a net whose activations the quantised engine
        cannot represent, and it would look completely normal doing it.
        """
        return torch.clamp(x, 0.0, 1.0)

    def accumulators(self, white: torch.Tensor, black: torch.Tensor):
        """The two perspective accumulators, before the perspective swap.

        Exposed because the exporter and the C incremental-update assert both
        need to compare against exactly this quantity.
        """
        return self._accumulate(white), self._accumulate(black)

    def _accumulate(self, features: torch.Tensor) -> torch.Tensor:
        acc = self.ft(features) + self.ft_bias
        if self.feature_factorization:
            shared = torch.where(features == PAD_INDEX, SHARED_PAD_INDEX,
                                 features % SHARED_FEATURES)
            acc = acc + self.ft_shared(shared)
        return acc

    def folded_state_dict(self) -> dict:
        """Weights for the unchanged engine architecture; does not mutate this model."""
        state = self.state_dict()
        state["ft.weight"] = effective_feature_weights(state, self.feature_factorization)
        state.pop("ft_shared.weight", None)
        return state

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
        return self._pick(self._value_head(self._trunk(white, black, stm, piece_count)),
                          piece_count)

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
        x = self._trunk(white, black, stm, piece_count)
        return (self._pick(self._value_head(x), piece_count),
                self._pick(self.unc(x), piece_count))

    def _activated(self, white: torch.Tensor, black: torch.Tensor,
                   stm: torch.Tensor) -> torch.Tensor:
        acc_white, acc_black = self.accumulators(white, black)

        own = acc_black * stm + acc_white * (1.0 - stm)
        other = acc_white * stm + acc_black * (1.0 - stm)

        return self.activate(torch.cat([own, other], dim=1))

    def _value_head(self, trunk: torch.Tensor) -> torch.Tensor:
        return self.l3(trunk) if self.l1_size else self.out(trunk)

    def _trunk(self, white: torch.Tensor, black: torch.Tensor, stm: torch.Tensor,
               piece_count: torch.Tensor | None) -> torch.Tensor:
        """The vector both output heads read, after the stack if there is one.

        The stack's layers are per-bucket, so the bucket is selected HERE and
        not only at the end - which is why `piece_count` reaches this far down.
        A stacked net with more than one bucket cannot answer without it, and
        that is the same refusal _pick() makes for the flat architecture.
        """
        x = self._activated(white, black, stm)
        if not self.l1_size:
            return x

        x = self.stack_activate(self._pick_rows(self.l1(x), self.l1_size, piece_count))
        if self.l2_size:
            x = self.stack_activate(self._pick_rows(self.l2(x), self.l2_size, piece_count))
        return x

    def _pick_rows(self, y: torch.Tensor, width: int,
                   piece_count: torch.Tensor | None) -> torch.Tensor:
        """One bucket's `width` outputs out of the (B, buckets * width) a
        per-bucket layer produces.

        The layer computes every bucket and this throws all but one away. That
        is deliberate: gathering the WEIGHTS instead - (batch, width, inputs) -
        is a gigabyte at batch 16384 and 2H inputs, to save compute that
        measures at roughly 3% of a training step against a feature
        transformer that is doing 32 embedding rows per perspective anyway.
        """
        if self.output_buckets == 1:
            return y
        if piece_count is None:
            raise ValueError(f"this net has {self.output_buckets} output buckets and needs "
                             f"piece_count to choose one")

        bucket = output_bucket(piece_count.long(), self.output_buckets)
        rows = y.view(-1, self.output_buckets, width)
        return rows.gather(1, bucket.view(-1, 1, 1).expand(-1, 1, width)).squeeze(1)

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
        if self.feature_factorization:
            residual = self.ft.weight[:-1].view(-1, SHARED_FEATURES, self.hidden)
            shared = self.ft_shared.weight[:-1]
            effective = (residual + shared).clamp_(-bound, bound)
            # Bound the decomposition too, without changing its effective sum.
            # Clipping the two terms independently would not bound that sum.
            shared.clamp_(-bound, bound)
            residual.copy_(effective - shared)
            self.ft_shared.weight[SHARED_PAD_INDEX].zero_()
        else:
            self.ft.weight.clamp_(-bound, bound)
        self.ft_bias.clamp_(-bound, bound)

        # One bound per layer, each from that layer's OWN integer constraint -
        # see the block under WEIGHT_CLIP in format.py. The flat output layer
        # and the stack do not share one because they do not share an
        # arithmetic: `out` is capped by an int16 SIMD product, L1 and L2 by an
        # int32 sum, L3 by int16 storage.
        if self.l1_size:
            self.l1.weight.clamp_(-L1_CLIP, L1_CLIP)
            self.l1.bias.clamp_(-L1_CLIP, L1_CLIP)
            if self.l2_size:
                self.l2.weight.clamp_(-L2_CLIP, L2_CLIP)
                self.l2.bias.clamp_(-L2_CLIP, L2_CLIP)
            self.l3.weight.clamp_(-L3_CLIP, L3_CLIP)
            self.l3.bias.clamp_(-L3_CLIP, L3_CLIP)
        else:
            self.out.weight.clamp_(-bound, bound)

        # The uncertainty head quantises exactly as the value head it sits
        # beside: the same input vector, the same scale, the same bound. With
        # a stack that is L3's; without one it is the flat layer's int16 SIMD
        # product.
        if self.uncertainty:
            head_bound = L3_CLIP if self.l1_size else bound
            self.unc.weight.clamp_(-head_bound, head_bound)
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
        "feature_factorization": bool(arch.get("feature_factorization", False)),
        # Absent for the same reason and with the same safety: every
        # checkpoint written before the stack existed is a flat net, zero is
        # what flat means, and load_state_dict cross checks it either way
        # because a stacked model has no `out.*` tensor and a flat one has no
        # `l1.*`.
        "l1_size": int(arch.get("l1_size", 0)),
        "l2_size": int(arch.get("l2_size", 0)),
    }


def from_checkpoint(state: dict) -> NNUE:
    """A model with the checkpoint's own architecture, weights loaded.

    Never construct an NNUE from remembered flags to load a checkpoint into:
    a width or a bucket count that disagrees fails loudly here, but weights
    loaded under the wrong feature set would run, and be wrong.
    """
    arch = arch_from_checkpoint(state)
    model = NNUE(hidden=arch["hidden"], output_buckets=arch["output_buckets"],
                 uncertainty=arch["uncertainty"],
                 feature_factorization=arch["feature_factorization"],
                 l1_size=arch["l1_size"], l2_size=arch["l2_size"])
    model.load_state_dict(state["model"])
    return model


def blended_target(score: torch.Tensor, wdl: torch.Tensor, lam, sigmoid_k: float
                   ) -> torch.Tensor:
    """``lambda * sigmoid(score / K) + (1 - lambda) * wdl``.

    Mostly distilling the search, with enough game result mixed in to stop the
    net inheriting the teacher's systematic errors. A record whose game result
    is unknown - a labelled EPD with no ``[x.x]`` on it, or a game the ply cap
    stopped - falls back to lambda = 1 rather than being dropped: its search
    score is just as good as any other record's, and pretending its result is a
    draw would be a lie the net would learn.

    ``lam`` is a float or a per-record tensor. The tensor case is the whole
    point of :class:`TargetPolicy`: one lambda over a dataset has to price the
    result's informativeness in at the average, and the average is not where
    any record lives.
    """
    outcome = torch.sigmoid(score / sigmoid_k)
    known = wdl <= 2
    result = torch.clamp(wdl.float(), max=2.0) / 2.0
    if not torch.is_tensor(lam):
        lam = torch.full_like(outcome, float(lam))
    lam_eff = torch.where(known, lam.to(outcome.dtype), torch.ones_like(outcome))
    return lam_eff * outcome + (1.0 - lam_eff) * result


def loss_fn(prediction: torch.Tensor, target: torch.Tensor, sigmoid_k: float,
            weight: torch.Tensor | None = None) -> torch.Tensor:
    """MSE in win-probability space, optionally weighted per record.

    The network's raw output is in NET_TO_CP units, so it goes through the same
    sigmoid the target did. Comparing centipawns directly would weight a
    2000cp position as heavily as the 30cp positions that actually decide
    games.

    ``weight`` re-weights the mixture without regenerating anything: a run can
    give tree samples half the pull of game-line positions and keep every
    record in the batch. Normalised by the weights' own sum, so the loss stays
    comparable to an unweighted run's rather than scaling with the mixture.
    """
    predicted = torch.sigmoid(prediction * NET_TO_CP / sigmoid_k)
    if weight is None:
        return torch.nn.functional.mse_loss(predicted, target)
    squared = (predicted - target) ** 2
    return (squared * weight).sum() / weight.sum().clamp_min(1e-8)


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


# ------------------------------------------------------------- the target ----


class TargetPolicy:
    """Everything between a record and the number the loss compares against.

    WHY THIS IS NOT JUST A FLOAT. The target is

        lambda * sigmoid(score / K) + (1 - lambda) * wdl

    and lambda prices how much the GAME RESULT is worth against the search
    score. That price is not the same for every record, and a single number for
    the whole dataset has to pay the average of prices that differ by a lot:

      * a result twenty moves away is nearly a coin flip about the position in
        front of it; three plies away it is the truth;
      * the search score's SYSTEMATIC errors - fortresses, compensation, an
        endgame it cannot convert - are what the result term exists to correct,
        and they are concentrated in the endgame;
      * a `human` or `engine` record's result is real, but it is someone
        else's continuation. A different question, not a worse answer.

    Each of those is one dial here, and every dial defaults to zero, so a
    policy built with no arguments computes exactly what a single lambda
    computed.
    The composition is deliberately additive and clipped rather than clever:

        lam = clip(base + progress_delta * closeness + pieces_delta * endgame,
                   lambda_min, lambda_max)
        lam = per-source override, where one is given
        lam = 1 wherever the game result is unknown           (structural)

    ``base`` is the epoch anneal the caller passes in, so the deltas ride on
    top of it rather than replacing it.
    """

    def __init__(self, sigmoid_k: float = 400.0, score_clip: int = 0,
                 progress_delta: float = 0.0, pieces_delta: float = 0.0,
                 source_lambda: dict | None = None, source_weight: dict | None = None,
                 lambda_min: float = 0.0, lambda_max: float = 1.0):
        self.sigmoid_k = float(sigmoid_k)
        self.score_clip = int(score_clip)
        self.progress_delta = float(progress_delta)
        self.pieces_delta = float(pieces_delta)
        self.lambda_min = float(lambda_min)
        self.lambda_max = float(lambda_max)

        n = len(SOURCE_NAMES)
        self.source_lambda = dict(source_lambda or {})
        self.source_weight = dict(source_weight or {})

        # Lookup tables rather than a dict walked per batch: source is an
        # int64 column already, so this is one gather.
        self._lam_by_source = torch.zeros(n)
        self._lam_given = torch.zeros(n, dtype=torch.bool)
        for src, value in self.source_lambda.items():
            self._lam_by_source[src] = float(value)
            self._lam_given[src] = True

        self._weight_by_source = torch.ones(n)
        for src, value in self.source_weight.items():
            self._weight_by_source[src] = float(value)
        self.weighted = bool(self.source_weight)

        self._device = None

    def to(self, device):
        if self._device != device:
            self._lam_by_source = self._lam_by_source.to(device)
            self._lam_given = self._lam_given.to(device)
            self._weight_by_source = self._weight_by_source.to(device)
            self._device = device
        return self

    # ------------------------------------------------------------ pieces ----

    def score(self, batch: dict) -> torch.Tensor:
        """The label, clipped if asked.

        A mate or a tablebase score is a PROVEN result rather than an
        evaluation, and ``sigmoid(31500 / 400)`` is 1.0 to every digit a float
        carries - so those records train the net toward a target no bounded
        network can reach, and the weight clip guarantees it is bounded. Before
        gen-5 the question did not arise, because `-maxscore 2000` dropped
        them; now that games are played to mate and tablebases label the
        endings, they are a real share of the dataset (`datagen stats` prints
        it). Clipping keeps the position and its sign and drops only the claim
        that the net should be infinitely confident about it.
        """
        score = batch["score"].float()
        if self.score_clip > 0:
            score = score.clamp(-self.score_clip, self.score_clip)
        return score

    def lambdas(self, batch: dict, base: float) -> torch.Tensor:
        """The per-record weight on the search score."""
        progress = batch["progress"]
        lam = torch.full_like(progress, float(base), dtype=torch.float32)

        if self.progress_delta:
            lam = lam + self.progress_delta * progress_closeness(progress).float()
        if self.pieces_delta:
            lam = lam + self.pieces_delta * phase_endgameness(batch["piece_count"]).float()

        lam = lam.clamp(self.lambda_min, self.lambda_max)

        if self.source_lambda:
            self.to(lam.device)
            source = batch["source"]
            lam = torch.where(self._lam_given[source], self._lam_by_source[source], lam)

        return lam

    def target(self, batch: dict, base: float) -> torch.Tensor:
        return blended_target(self.score(batch), batch["wdl"],
                              self.lambdas(batch, base), self.sigmoid_k)

    def weights(self, batch: dict) -> torch.Tensor | None:
        """Per-record loss weights, or None when every source counts the same.

        This is the mixture dial. `--sources` can only DROP a source, and
        dropping is the wrong tool when the question is whether tree samples
        want half the pull of game-line ones rather than none of it.
        """
        if not self.weighted:
            return None
        self.to(batch["source"].device)
        return self._weight_by_source[batch["source"]]

    def describe(self) -> str:
        parts = []
        if self.score_clip:
            parts.append(f"score clipped to +-{self.score_clip}cp")
        if self.progress_delta:
            parts.append(f"lambda {self.progress_delta:+.2f} at the game's end")
        if self.pieces_delta:
            parts.append(f"lambda {self.pieces_delta:+.2f} at bare kings")
        for src, value in sorted(self.source_lambda.items()):
            parts.append(f"lambda {value:.2f} for {SOURCE_NAMES[src]}")
        for src, value in sorted(self.source_weight.items()):
            parts.append(f"weight {value:g} for {SOURCE_NAMES[src]}")
        return ", ".join(parts) if parts else "flat lambda, no clip, no re-weighting"
