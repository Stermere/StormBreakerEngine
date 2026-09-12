"""Quantise a trained checkpoint into a .nnue file, and write the test vectors
that prove src/nnue.c reproduces it exactly.

    python tools/export_net.py            # external/nets/net.pt -> net.nnue
    python tools/export_net.py net-fact   # a bare name resolves under external/nets

Two outputs and one gate:

  * ``<out>.nnue``     the weights the engine embeds, with a header describing
                       its own architecture (see src/nnue.h)
  * ``<out>.vectors``  ``<raw> <cp> <fen>`` for N positions, computed here in
                       numpy integer arithmetic
  * ``<out>.sha256``   the hash the engine prints, so a bench node count can be
                       attributed to a specific net

``make nnue-test`` then runs the engine over the vectors and requires EXACT
equality on every line. Exact, not close: the quantised network is integer
arithmetic and integer arithmetic is reproducible. A tolerance would be a bug
generator - a disagreement of one means something rounds differently, and that
something is worth about 30 Elo by the time anyone notices it in a game.

WHY THIS IS A SECOND IMPLEMENTATION, NOT A SHARED ONE. The forward pass below
is written in numpy from the header's own fields, deliberately without
consulting torch. If both sides called the same code the test would prove only
that the code equals itself. The point is that two people writing the same
specification in two languages disagree exactly where the specification is
ambiguous, and every place they disagree here - the truncating division, the
clamp bounds, the perspective order, WHERE THE SCReLU RESCALE HAPPENS - is a
place the engine could have been silently wrong.

UPGRADING THE NETWORK. The shape - width and output buckets - is read out of
the checkpoint and written into the header, so a wider or differently-bucketed
retrain needs no change here and none in the engine. The feature set and the
activation are not parameters: one of each is implemented, here and in
src/nnue.c, and a new one needs a case in both plus a new enum value in
src/nnue.h. A checkpoint that disagrees is refused rather than exported under a
tag it does not match.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "trainer"))

from nnue.format import (  # noqa: E402
    ACTIVATION_TAG,
    FEATURE_SET_NAME,
    FEATURE_SET_TAG,
    L1_SHIFT,
    L2_SHIFT,
    L3_SCALE,
    NUM_FEATURES,
    PAD_INDEX,
    QA,
    QB,
    SCALE,
    STACK_QA,
    output_bucket,
    pack_fens,
    read_shard,
    record_to_fen,
    unpack,
)
from nnue.model import arch_from_checkpoint, effective_feature_weights  # noqa: E402
from nnue.provenance import sha256_file, write_json  # noqa: E402

# ------------------------------------------------------------------ format --

MAGIC = b"CKNNUE\0\0"
# Must equal NNUE_FORMAT_VERSION in src/nnue.h. 3 added the four layer-stack
# fields; every v1 and v2 net fails on the version rather than being read with
# a header that has since grown.
FORMAT_VERSION = 3

# 8 bytes magic, eight u32, one i32 (scale), one u32 (payload), four u32 of
# stack shape, 32-byte tag, 16 reserved. Must stay identical to NnueHeader in
# src/nnue.h, which carries a _Static_assert on its size for exactly this
# reason.
HEADER_FMT = "<8s8IiI4I32s16s"
HEADER_BYTES = 112
assert struct.calcsize(HEADER_FMT) == HEADER_BYTES

# Defaults from nnue/format.py, which is also where the TRAINER reads them:
# weight clipping during training is computed from these, and a clip fitted to
# a different QB than the export quantises with is a clip that does not clip.
DEFAULT_QA = QA
DEFAULT_QB = QB
DEFAULT_SCALE = SCALE

# Paired with NNUE_EVAL_LIMIT in src/nnue.c. Engine policy rather than a
# property of the net, which is why it is not a header field - a static
# evaluation that wanders into mate territory makes the search report forced
# mates that do not exist. The export prints the largest |cp| it actually saw,
# so it is visible when a net starts creeping toward the clamp.
EVAL_LIMIT = 20000

# The most men src/board.c will accept on a board. The accumulator bound below
# is what it is for: the engine will evaluate any diagram its FEN parser
# accepts, so the proof that int16 cannot wrap has to cover that many rows.
#
# It was 64 - "a board has 64 squares" - which reserved half the accumulator's
# range for diagrams no legal game can reach, put the shipped net at 99.5% of
# int16, and forced the feature transformer's weight clip down to a bound that
# belongs to the output layer. Task 6 step 1 in docs/NNUE.md. KEEP THIS EQUAL
# TO THE CAP IN board_set_fen: the two are one decision, and a proof covering
# fewer men than the parser accepts is not a proof.
ENGINE_MAX_PIECES = 32

INT16_MAX = 32767
INT32_MAX = 2**31 - 1

# ------------------------------------------------------------------- paths --
NETS_DIR = "external/nets"
DEFAULT_CHECKPOINT = NETS_DIR + "/net.pt"
DEFAULT_OUT = NETS_DIR + "/net.nnue"  # what the default build embeds


def resolve_checkpoint(name: str) -> str:
    """A checkpoint by path, or by the bare name of one in external/nets.

    An INPUT can be searched for, because the answer is checked against the
    filesystem: a literal path that exists always wins, and nothing is guessed
    when a guess could be wrong. The output side below cannot do this and does
    not try.
    """
    candidates = [name]
    if not os.path.splitext(name)[1]:
        candidates.append(name + ".pt")
    if not os.path.dirname(name):
        candidates += [f"{NETS_DIR}/{c}" for c in candidates]

    for candidate in candidates:
        if os.path.exists(candidate):
            return candidate

    tried = "\n  ".join(candidates)
    raise SystemExit(f"no checkpoint at any of:\n  {tried}")


def resolve_out(name: str) -> str:
    """The .nnue to write. Only a BARE STEM is expanded.

    `-o cand` means external/nets/cand.nnue, but anything already shaped like a
    path or a file - `cand.nnue`, `./cand.nnue`, the EVALFILE make passes - is
    taken verbatim. Moving an output the caller spelled out would let the file
    this writes and the file the engine embeds be two different files, which is
    invariant 8's failure mode arriving through the back door.
    """
    if os.path.dirname(name) or os.path.splitext(name)[1]:
        return name
    return f"{NETS_DIR}/{name}.nnue"


def trunc_div(num: np.ndarray, den: int) -> np.ndarray:
    """Integer division that TRUNCATES toward zero, as C's ``/`` does.

    numpy's ``//`` floors, so ``-7 // 4`` is -2 where C gives -1. Every negative
    evaluation would be one centipawn low, which is to say about half the test
    vectors would fail and the engine would be marginally wrong forever if the
    test had been written with a tolerance.

    SCReLU divides twice - once to undo the squared accumulator scale, once for
    centipawns - so the rule matters in two places rather than one.
    """
    return np.sign(num) * (np.abs(num) // den)


# ------------------------------------------------------------ quantisation --


def resolve_scales(args, arch: dict) -> None:
    """Fill in --qa/--qb from the architecture when they were not given.

    Both defaults are properties of the shape rather than preferences. A
    STACKED net needs a power-of-two qa, because its activation shifts by
    log2(qa) per element instead of dividing once at the end; and its final
    layer can carry a much finer scale than 64, because the int16 SIMD product
    that caps a flat output weight at 128 levels is not in its path at all.
    Those are the two numbers the quantisation block in trainer/nnue/format.py
    is written against, so taking them from anywhere else is how the trainer's
    clips stop matching what the export actually does.
    """
    stacked = bool(arch.get("l1_size", 0))
    if args.qa is None:
        args.qa = STACK_QA if stacked else DEFAULT_QA
    if args.qb is None:
        args.qb = L3_SCALE if stacked else DEFAULT_QB


def quantise(state: dict, arch: dict, qa: int, qb: int) -> dict:
    """Round the float weights onto the integer grid the engine reads.

    Rounding is half-to-even (``np.rint``). Which rule is used does not affect
    the C/Python equivalence - only Python ever rounds, the engine just reads
    the stored integers - but it is the sort of thing worth pinning rather than
    inheriting.

    Everything comes back int32. The values all fit int16; int32 is the working
    type so that a later product does not silently wrap in numpy the way it
    would in an int16 array.
    """
    buckets = arch["output_buckets"]

    l1_size = int(arch.get("l1_size", 0))
    l2_size = int(arch.get("l2_size", 0))

    # A stacked net's activation is `(x * x) >> log2(qa)` rather than a divide
    # by qa, because it is applied per element instead of once at the end of a
    # fused dot product. src/nnue.c rejects a stacked net whose qa is not a
    # power of two; saying so here turns that into a flag error.
    if l1_size and (qa & (qa - 1)):
        raise SystemExit(
            f"qa {qa} is not a power of two, which a net with a layer stack requires: its "
            f"activation shifts by log2(qa) per element rather than dividing once. "
            f"Re-export with --qa {1 << (qa.bit_length())} (the default for a stacked "
            f"checkpoint), or train without the stack."
        )

    try:
        ft = effective_feature_weights(state, bool(arch.get("feature_factorization", False)))
    except ValueError as error:
        raise SystemExit(str(error)) from error
    ft_w = ft.detach().cpu().numpy().astype(np.float64)
    ft_b = state["ft_bias"].detach().cpu().numpy().astype(np.float64)

    def tensor(name):
        return state[name].detach().cpu().numpy().astype(np.float64)

    # The final layer is `out.*` without a stack and `l3.*` with one - the same
    # (buckets, trunk) shape either way, because both architectures end in one
    # linear layer per bucket over whatever the heads read.
    head = "l3" if l1_size else "out"
    out_w = tensor(f"{head}.weight")
    out_b = tensor(f"{head}.bias").reshape(-1)

    # The padding row exists only so a batch can be a dense (B, 32) matrix. It
    # is pinned to zero in training and is not part of the model.
    if ft_w.shape[0] != NUM_FEATURES + 1:
        raise SystemExit(
            f"checkpoint has {ft_w.shape[0]} feature rows, this build's feature set has "
            f"{NUM_FEATURES} + 1 padding. It was written by a different model."
        )
    assert np.all(ft_w[PAD_INDEX] == 0.0), \
        "the padding embedding drifted off zero in training"
    ft_w = ft_w[:NUM_FEATURES]

    hidden = int(ft_w.shape[1])
    trunk = (l2_size or l1_size) if l1_size else 2 * hidden
    if out_w.shape != (buckets, trunk) or out_b.shape != (buckets,):
        raise SystemExit(
            f"output layer is {out_w.shape}/{out_b.shape}, expected "
            f"{(buckets, trunk)}/{(buckets,)} for {buckets} output buckets"
        )

    q = {
        "ft_w": np.rint(ft_w * qa).astype(np.int32),
        "ft_b": np.rint(ft_b * qa).astype(np.int32),
        "out_w": np.rint(out_w * qb).astype(np.int32),
        "out_b": np.rint(out_b * qa * qb).astype(np.int32),
        "hidden": hidden,
        "buckets": buckets,
        "trunk": trunk,
        "l1_size": l1_size,
        "l2_size": l2_size,
        "l1_shift": L1_SHIFT if l1_size else 0,
        "l2_shift": L2_SHIFT if l2_size else 0,
        "uncertainty": bool(arch.get("uncertainty", False)),
    }

    # The stack. Each stage's weights ride a scale of 2**shift and its bias
    # `qa * 2**shift`, which is what makes `sum >> shift` land back in [0, qa]
    # with no factor folded into the weights - see the block under WEIGHT_CLIP
    # in trainer/nnue/format.py, and Task 6 in docs/NNUE.md for the whole
    # arithmetic. torch stores a per-bucket layer as (buckets * width, inputs);
    # the engine reads [bucket][unit][input], which is the same bytes in the
    # same order.
    if l1_size:
        l1_scale = 1 << L1_SHIFT
        q["l1_w"] = np.rint(tensor("l1.weight") * l1_scale).astype(np.int32)
        q["l1_b"] = np.rint(tensor("l1.bias") * qa * l1_scale).astype(np.int32)
        if q["l1_w"].shape != (buckets * l1_size, 2 * hidden):
            raise SystemExit(
                f"L1 is {q['l1_w'].shape}, expected {(buckets * l1_size, 2 * hidden)}")
        if l2_size:
            l2_scale = 1 << L2_SHIFT
            q["l2_w"] = np.rint(tensor("l2.weight") * l2_scale).astype(np.int32)
            q["l2_b"] = np.rint(tensor("l2.bias") * qa * l2_scale).astype(np.int32)
            if q["l2_w"].shape != (buckets * l2_size, l1_size):
                raise SystemExit(
                    f"L2 is {q['l2_w'].shape}, expected {(buckets * l2_size, l1_size)}")

    # The head's presence is claimed twice - by the arch record and by the
    # weights themselves - and the two must agree, because the exporter that
    # trusts one over the other writes either a head of garbage or a header
    # that promises one it does not have.
    has_unc_weights = "unc.weight" in state
    if q["uncertainty"] != has_unc_weights:
        raise SystemExit(
            f"checkpoint arch says uncertainty={q['uncertainty']} but its weights "
            f"{'do' if has_unc_weights else 'do not'} contain an unc.* tensor"
        )

    if q["uncertainty"]:
        unc_w = tensor("unc.weight")
        unc_b = tensor("unc.bias").reshape(-1)
        if unc_w.shape != (buckets, trunk) or unc_b.shape != (buckets,):
            raise SystemExit(
                f"uncertainty layer is {unc_w.shape}/{unc_b.shape}, expected "
                f"{(buckets, trunk)}/{(buckets,)} for {buckets} output buckets"
            )
        # The same grid as the value head: the C forward runs both heads
        # through the same SCReLU arithmetic, so they must be quantised the
        # same way or one of them is a different function than was trained.
        q["unc_w"] = np.rint(unc_w * qb).astype(np.int32)
        q["unc_b"] = np.rint(unc_b * qa * qb).astype(np.int32)

    return q


def check_ranges(q: dict, qa: int) -> dict:
    """Refuse to export a net whose arithmetic could overflow the engine's.

    Every bound here is SOUND over every position the engine will accept -
    derived from the weights and from "a board holds at most ENGINE_MAX_PIECES
    men" - rather than measured over the ten thousand positions the gate
    happens to cover. A bound that
    holds on a sample and not in general produces a net that passes every test
    and blunders once a tournament.

    ``int16`` weights
        A stored weight that does not fit is simply a broken file.

    ``int16`` accumulator
        src/nnue.c sums the accumulator in int16, because that is what puts
        sixteen lanes in an AVX2 register and halves the bytes moved - and the
        accumulator is the evaluation. Nothing there checks for overflow, so
        this is the check. Bias plus the ENGINE_MAX_PIECES largest positive
        weights in a column bounds it over every position the ENGINE will
        accept - a fuller board than legal play reaches - not over a sample.

    ``int16`` activation product
        The engine's SCReLU forms ``v * w`` as int16 before widening, so
        ``QA * max|w|`` must fit int16. This is the one bound the engine cannot
        recover from at runtime - the multiply simply wraps, the net scores
        plausibly, and it loses Elo silently. Training clips the weights to
        keep it true; --no-weight-clip is what makes it fail.

    ``int32`` output
        The raw output is returned as int32 by both implementations.
    """
    limits = {}

    names = ["ft_w", "ft_b", "out_w", "out_b"]
    if q["l1_size"]:
        names += ["l1_w", "l1_b"]
        if q["l2_size"]:
            names += ["l2_w", "l2_b"]
    if q["uncertainty"]:
        names += ["unc_w", "unc_b"]
    for name in names:
        peak = int(np.abs(q[name]).max())
        limits[f"{name}_peak"] = peak
        if name not in ("out_b", "unc_b", "l1_b", "l2_b") and peak > INT16_MAX:
            raise SystemExit(
                f"{name} quantises to {peak}, which does not fit int16. The net is "
                f"unusable at this QA/QB; retrain with weight clipping or lower the scale."
            )

    ft_w, ft_b = q["ft_w"], q["ft_b"]
    top = np.sort(ft_w, axis=0)
    hi = ft_b + np.clip(top[-ENGINE_MAX_PIECES:], 0, None).sum(axis=0, dtype=np.int64)
    lo = ft_b + np.clip(top[:ENGINE_MAX_PIECES], None, 0).sum(axis=0, dtype=np.int64)
    worst = int(max(np.abs(hi).max(), np.abs(lo).max()))
    limits["accumulator_bound"] = worst
    if worst > INT16_MAX:
        raise SystemExit(
            f"the accumulator can reach {worst} on a {ENGINE_MAX_PIECES}-man board, past "
            f"int16. An incremental accumulator would wrap. Retrain with a smaller QA or "
            f"with weight clipping - or, if you would rather keep the net, lower "
            f"ENGINE_MAX_PIECES here AND the matching cap in board_set_fen, which costs "
            f"only the ability to load unusually crowded puzzles."
        )

    # The uncertainty head rides the same int16 SIMD multiply as the value
    # head, so its weights live under the same bound.
    #
    # ONLY THE FLAT ARCHITECTURE HAS THIS BOUND, and removing it is the point of
    # the stack. `nnue_flat_head()` forms `v * w` as int16 before a widening
    # madd, so `QA * max|w|` has to fit int16 - which caps a quantised output
    # weight at 128 however much range the layer wants. A stacked net's dot
    # products are plain `madd_epi16`, whose products are formed in 32 bits, so
    # there is no intermediate to overflow and the only bound is the int32 sum
    # checked below.
    if not q["l1_size"]:
        activation_product = qa * max(limits["out_w_peak"], limits.get("unc_w_peak", 0))
        limits["activation_product_bound"] = activation_product
        if activation_product > INT16_MAX:
            raise SystemExit(
                f"QA * max|out_w| is {activation_product}, past int16. The engine's SCReLU "
                f"multiplies the clamped activation by the weight as int16, so this net "
                f"would wrap in play. Retrain with weight clipping - the bound is "
                f"{INT16_MAX // qa} quantised units, {INT16_MAX // qa / q['qb']:.3f} in "
                f"float, which is WEIGHT_CLIP in trainer/nnue/format.py - or lower QA."
            )

    # Each stack stage's int32 sum, bounded the same way: every input is at
    # most QA, so the worst case is `|bias| + QA * sum|w|` per unit. This is the
    # bound src/nnue.c's `nnue_dot()` relies on, and it covers a SIMD lane's
    # partial sum as well as the total, because a subset of those terms cannot
    # exceed their own absolute sum.
    for stage, weights, biases in (("l1", "l1_w", "l1_b"), ("l2", "l2_w", "l2_b")):
        if weights not in q:
            continue
        # The rounding half nnue_requantise() adds before shifting is part of
        # what has to fit, so it is part of the bound.
        half = 1 << (q[f"{stage}_shift"] - 1)
        per_unit = np.abs(q[weights]).sum(axis=1, dtype=np.int64)
        bound = int((np.abs(q[biases]).reshape(-1) + qa * per_unit).max()) + half
        limits[f"{stage}_bound"] = bound
        if bound > INT32_MAX:
            raise SystemExit(
                f"the {stage.upper()} sum can reach {bound}, past int32. Retrain with a "
                f"tighter {stage.upper()}_CLIP in trainer/nnue/format.py, or narrow the "
                f"layer feeding it."
            )

    # |raw| over every possible activation vector. SCReLU's extra factor of QA
    # is divided back out before the bias is added, so the bound is QA * the
    # weight sum, exactly as it would be without the square.
    per_bucket = np.abs(q["out_w"]).sum(axis=1, dtype=np.int64)
    out_bound = int((np.abs(q["out_b"]) + qa * per_bucket).max())
    limits["output_bound"] = out_bound
    if out_bound > INT32_MAX:
        raise SystemExit(f"the output sum can reach {out_bound}, past int32.")

    if q["uncertainty"]:
        per_bucket = np.abs(q["unc_w"]).sum(axis=1, dtype=np.int64)
        unc_bound = int((np.abs(q["unc_b"]) + qa * per_bucket).max())
        limits["uncertainty_bound"] = unc_bound
        if unc_bound > INT32_MAX:
            raise SystemExit(f"the uncertainty sum can reach {unc_bound}, past int32.")

    return limits


# --------------------------------------------------------- reference model --


def requantise(sums: np.ndarray, shift: int, ceiling: int) -> np.ndarray:
    """One stack stage's int32 sum, back into [0, ceiling].

    CLAMP BEFORE THE SHIFT, as src/nnue.c does. `>>` on a negative int32 is
    implementation-defined in C17, and clamping to zero first is what makes the
    C line, numpy's `>>` and a floor division all agree on every value. Written
    this way rather than shifting first and clamping after specifically because
    the two differ, and only on the negatives - which is exactly the class of
    disagreement a tolerance would hide.

    The half-shift ROUNDS TO NEAREST. A bare shift truncates, and truncating at
    every stage of every unit is a systematic downward bias rather than noise:
    it compounds across the stack and leaves the quantised net uniformly more
    pessimistic than the trained one.
    """
    return np.minimum((np.maximum(sums, 0) + (1 << (shift - 1))) >> shift, ceiling)


def stack_trunk(q: dict, x: np.ndarray, buckets: np.ndarray, qa: int) -> np.ndarray:
    """The vector both output heads read, for a chunk of positions.

    Grouped by bucket rather than gathering per-position weights: a
    (positions, units, inputs) temporary is 33 MB a chunk and there are at most
    32 distinct buckets, so the group-by is both smaller and faster. It changes
    nothing about the arithmetic - each position still meets exactly its own
    bucket's weights.
    """
    act_shift = qa.bit_length() - 1
    assert 1 << act_shift == qa, "a stacked net's qa must be a power of two"

    # The SCReLU output as the int16 vector L1 reads. The flat path never
    # materialises this - it fuses the square into its own dot product and
    # divides by qa once at the end - which is why `>> act_shift` appears here
    # and nowhere above.
    a = (x * x) >> act_shift

    hidden2, l1_size, l2_size = a.shape[1], q["l1_size"], q["l2_size"]
    l1_w = q["l1_w"].astype(np.int64).reshape(-1, l1_size, hidden2)
    l1_b = q["l1_b"].astype(np.int64).reshape(-1, l1_size)

    out = np.empty((x.shape[0], l2_size or l1_size), dtype=np.int64)
    for b in np.unique(buckets):
        rows = buckets == b
        h = requantise(a[rows] @ l1_w[b].T + l1_b[b], q["l1_shift"], qa)
        if l2_size:
            l2_w = q["l2_w"].astype(np.int64).reshape(-1, l2_size, l1_size)
            l2_b = q["l2_b"].astype(np.int64).reshape(-1, l2_size)
            h = requantise(h @ l2_w[b].T + l2_b[b], q["l2_shift"], qa)
        out[rows] = h
    return out


def forward(q: dict, fields: dict, qa: int, qb: int, scale: int, chunk: int = 256) -> tuple:
    """The quantised forward pass, in integers, exactly as src/nnue.c runs it.

    Chunked because ``table[idx]`` materialises (B, 32, hidden): at ten thousand
    positions and 1024 wide that is 2.6 GB, and there is no reason to pay it.

    The SCReLU rescale is the subtle part. The activation is ``v^2`` where
    ``v <= QA``, so a term is at QA^2 * QB while the bias is at QA * QB. The sum
    is therefore divided by QA - truncating, toward zero - BEFORE the bias is
    added, and src/nnue.c does the same in the same order. Adding the bias
    first, or flooring instead of truncating, changes about half the vectors by
    one and nothing else.

    This sums in int64 and the engine's AVX2 path sums int32 lanes, flushing
    often enough that they cannot overflow. Integer addition is associative, so
    the two orders agree exactly - which is what `make nnue-test` asserts, on
    whichever path the binary was built with.
    """
    hidden, buckets = q["hidden"], q["buckets"]
    white, black, stm = fields["white"], fields["black"], fields["stm"]
    n = len(stm)

    bucket = np.asarray(output_bucket(fields["piece_count"], buckets), dtype=np.int64)

    # Row pad_index was dropped from the exported weights, so pad has to map to
    # an explicit zero rather than to a row that no longer exists.
    table = np.concatenate([q["ft_w"], np.zeros((1, hidden), dtype=np.int32)], axis=0)

    raw = np.empty(n, dtype=np.int64)
    unc_raw = np.empty(n, dtype=np.int64) if q["uncertainty"] else None

    for start in range(0, n, chunk):
        stop = min(start + chunk, n)
        idx_w, idx_b = white[start:stop], black[start:stop]

        acc_w = q["ft_b"] + table[idx_w].sum(axis=1, dtype=np.int64)
        acc_b = q["ft_b"] + table[idx_b].sum(axis=1, dtype=np.int64)

        # The side to move always reads its own accumulator first. Getting this
        # backwards gives a net that plays reasonably and hates its position.
        black_moves = stm[start:stop].astype(bool)[:, None]
        own = np.where(black_moves, acc_b, acc_w)
        other = np.where(black_moves, acc_w, acc_b)

        x = np.concatenate([np.clip(own, 0, qa), np.clip(other, 0, qa)], axis=1)
        buckets_here = bucket[start:stop]

        if not q["l1_size"]:
            w = q["out_w"][buckets_here].astype(np.int64)
            b = q["out_b"][buckets_here].astype(np.int64)

            raw[start:stop] = trunc_div((x * x * w).sum(axis=1), qa) + b

            if unc_raw is not None:
                uw = q["unc_w"][buckets_here].astype(np.int64)
                ub = q["unc_b"][buckets_here].astype(np.int64)
                unc_raw[start:stop] = trunc_div((x * x * uw).sum(axis=1), qa) + ub
            continue

        trunk = stack_trunk(q, x, buckets_here, qa)
        w = q["out_w"][buckets_here].astype(np.int64)
        b = q["out_b"][buckets_here].astype(np.int64)
        raw[start:stop] = (trunk * w).sum(axis=1) + b

        if unc_raw is not None:
            uw = q["unc_w"][buckets_here].astype(np.int64)
            ub = q["unc_b"][buckets_here].astype(np.int64)
            unc_raw[start:stop] = (trunk * uw).sum(axis=1) + ub

    if np.abs(raw).max(initial=0) > INT32_MAX:
        raise SystemExit("a test position overflowed int32; the bound check is wrong")

    cp = np.clip(trunc_div(raw * scale, qa * qb), -EVAL_LIMIT, EVAL_LIMIT)

    if unc_raw is None:
        return raw, cp, None, None

    if np.abs(unc_raw).max(initial=0) > INT32_MAX:
        raise SystemExit("a test position overflowed int32 in the uncertainty head")

    # The head predicts a magnitude, so the clamp floor is zero rather than
    # -EVAL_LIMIT: a negative prediction is the head saying "less than any
    # error I can express", and the engine clamps identically.
    unc_cp = np.clip(trunc_div(unc_raw * scale, qa * qb), 0, EVAL_LIMIT)
    return raw, cp, unc_raw, unc_cp


# ------------------------------------------------------------- positions ----

# Used when neither a shard nor a FEN file is available. Small, but it still
# covers both perspectives, both mirror halves, every piece type, several
# output buckets and an empty board - which is most of what the feature
# extraction and the bucket selection can get wrong.
FALLBACK_FENS = [
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "8/8/8/4k3/8/8/8/4K3 w - - 0 1",
    "3r1rk1/p3qppp/2bb1n2/1p6/3P4/1B3N2/PP2QPPP/R1B2RK1 w - - 2 18",
    "8/1r3k2/8/2R5/8/5K2/8/8 b - - 0 1",
]


def collect_fens(args) -> list:
    """Test positions, chosen deterministically so the gate is reproducible."""
    if args.fens:
        with open(args.fens, encoding="utf-8") as f:
            fens = [line.strip() for line in f if line.strip() and not line.startswith("#")]
        return fens[: args.count]

    shard = args.positions
    if shard is None:
        for guess in ("external/data/train.cnn", "external/data/all.cnn",
                      "external/data/shard00.cnn", "external/datagen-test/all.cnn"):
            if os.path.exists(guess):
                shard = guess
                break

    if shard is None:
        print("no shard found; falling back to the built-in positions "
              "(pass --positions <shard.cnn> for the full 10k gate)")
        return FALLBACK_FENS

    records = read_shard(shard)
    total = len(records)
    # Evenly spaced rather than random: the same shard always yields the same
    # vectors, so a failure is reproducible without carrying a seed around.
    want = min(args.count, total)
    picks = np.linspace(0, total - 1, want, dtype=np.int64) if want else np.empty(0, np.int64)
    print(f"positions: {want} of {total} from {shard}")
    return [record_to_fen(records[int(i)]) for i in picks]


# ------------------------------------------------------------------ write ---


def write_net(path: str, q: dict, args, tag: str) -> bytes:
    hidden, buckets, trunk = q["hidden"], q["buckets"], q["trunk"]

    # The order src/nnue.h documents, and nnue_adopt() walks in exactly this
    # sequence. Note where the stack sits: BEFORE the output layer, because the
    # output layer reads the trunk and the trunk is what the stack produces.
    blocks = [
        q["ft_w"].astype(np.int16).tobytes(order="C"),
        q["ft_b"].astype(np.int16).tobytes(order="C"),
    ]
    expect = NUM_FEATURES * hidden * 2 + hidden * 2

    if q["l1_size"]:
        blocks += [
            q["l1_w"].astype(np.int16).tobytes(order="C"),
            q["l1_b"].astype(np.int32).tobytes(order="C"),
        ]
        expect += buckets * q["l1_size"] * 2 * hidden * 2 + buckets * q["l1_size"] * 4
        if q["l2_size"]:
            blocks += [
                q["l2_w"].astype(np.int16).tobytes(order="C"),
                q["l2_b"].astype(np.int32).tobytes(order="C"),
            ]
            expect += buckets * q["l2_size"] * q["l1_size"] * 2 + buckets * q["l2_size"] * 4

    blocks += [
        q["out_w"].astype(np.int16).tobytes(order="C"),
        q["out_b"].astype(np.int32).tobytes(order="C"),
    ]
    expect += buckets * trunk * 2 + buckets * 4

    if q["uncertainty"]:
        blocks += [
            q["unc_w"].astype(np.int16).tobytes(order="C"),
            q["unc_b"].astype(np.int32).tobytes(order="C"),
        ]
        expect += buckets * trunk * 2 + buckets * 4
    payload = b"".join(blocks)
    assert len(payload) == expect, (len(payload), expect)

    # The head's presence rides in reserved[0] rather than a version bump, and
    # that is what the reserved bytes are for: every headless net stays
    # loadable by old and new engines alike, and an engine that predates the
    # head computes a smaller payload than a flagged header claims and rejects
    # the file loudly on the size check. src/nnue.h documents the byte beside
    # the struct.
    reserved = (b"\x01" if q["uncertainty"] else b"\x00") + b"\x00" * 15

    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        FORMAT_VERSION,
        FEATURE_SET_TAG,
        ACTIVATION_TAG,
        NUM_FEATURES,
        hidden,
        buckets,
        args.qa,
        args.qb,
        args.scale,
        len(payload),
        q["l1_size"],
        q["l2_size"],
        q["l1_shift"],
        q["l2_shift"],
        tag.encode("utf-8")[:32],
        reserved,
    )

    blob = header + payload
    with open(path, "wb") as f:
        f.write(blob)
    return blob


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"""examples (through make, which adds the venv python):

  make nnue-export                          {DEFAULT_CHECKPOINT} -> {DEFAULT_OUT}
  make nnue-export ARGS="net-fact -f"       {NETS_DIR}/net-fact.pt -> {DEFAULT_OUT}
  make nnue-export ARGS="net-fact -o cand"  ... -> {NETS_DIR}/cand.nnue
  make nnue-test   ARGS="net-fact" EVALFILE={NETS_DIR}/cand.nnue
""")
    parser.add_argument("checkpoint", nargs="?", default=None,
                        help=f"a .pt written by nnue.train, or the bare name of one in "
                             f"{NETS_DIR} (default: {DEFAULT_CHECKPOINT})")
    parser.add_argument("-o", "--out", "--output", default=None, dest="out",
                        help=f"output .nnue; a bare name lands in {NETS_DIR} "
                             f"(default: {DEFAULT_OUT}, which is what the engine embeds)")
    parser.add_argument("--positions", default=None,
                        help="a .cnn shard to draw test positions from")
    parser.add_argument("--fens", default=None, help="a file of FENs, one per line, instead")
    parser.add_argument("--count", type=int, default=10000, help="test vectors to write")
    parser.add_argument("--vectors", default=None, help="override the vectors path")
    # Defaulted from the CHECKPOINT rather than here, because the right scales
    # are a property of the architecture: a stacked net needs a power-of-two qa
    # for its per-element activation shift, and its final layer is free of the
    # int16 SIMD product that pins the flat one at 128 levels. See resolve_scales().
    parser.add_argument("--qa", type=int, default=None,
                        help=f"activation scale (default: {DEFAULT_QA} flat, "
                             f"{STACK_QA} stacked)")
    parser.add_argument("--qb", type=int, default=None,
                        help=f"output weight scale (default: {DEFAULT_QB} flat, "
                             f"{L3_SCALE} stacked)")
    parser.add_argument("--scale", type=int, default=DEFAULT_SCALE,
                        help="centipawns per unit of float output")
    parser.add_argument("--tag", default=None,
                        help="up to 32 bytes of provenance stamped into the header")
    parser.add_argument("-f", "--overwrite", action="store_true",
                        help="explicitly replace an existing export from a different checkpoint")
    args = parser.parse_args()

    checkpoint = resolve_checkpoint(args.checkpoint or DEFAULT_CHECKPOINT)
    out = resolve_out(args.out or DEFAULT_OUT)
    vectors = args.vectors or (out + ".vectors")
    manifest_path = os.path.splitext(out)[0] + ".json"
    checkpoint_path = os.path.abspath(checkpoint)
    checkpoint_hash = sha256_file(checkpoint_path)
    if os.path.exists(out) and not args.overwrite:
        previous = None
        if os.path.exists(manifest_path):
            with open(manifest_path, encoding="utf-8") as f:
                previous = json.load(f)
        # Name what actually differs. Re-exporting an epoch overwrites the checkpoint in
        # place, so the two paths are routinely the same file and only the hashes tell
        # them apart - a message built from basenames reads "x is net.pt, not net.pt".
        was = (previous or {}).get("checkpoint")
        if not previous:
            reason = f"{out} has no manifest beside it, so what wrote it cannot be checked"
        elif previous.get("sha256") != sha256_file(out):
            reason = f"{out} has changed since {os.path.basename(manifest_path)} described it"
        elif previous.get("checkpoint_sha256") != checkpoint_hash:
            reason = (f"{out} is {was} ({str(previous.get('checkpoint_sha256'))[:12]}), "
                      f"not {checkpoint_path} ({checkpoint_hash[:12]})")
        else:
            reason = None
        if reason:
            raise SystemExit(
                f"{reason}\n"
                f"replacing a different or unverified export needs -f/--overwrite, "
                f"or name a distinct -o path")

    # Imported here so that --help works without a torch install.
    import torch

    state = torch.load(checkpoint_path, map_location="cpu", weights_only=False)
    if sha256_file(checkpoint_path) != checkpoint_hash:
        raise SystemExit("checkpoint changed while being loaded; export a saved epoch checkpoint")
    arch = arch_from_checkpoint(state)
    resolve_scales(args, arch)
    q = quantise(state["model"], arch, args.qa, args.qb)
    q["qb"] = args.qb

    if q["hidden"] != int(state.get("hidden", q["hidden"])):
        raise SystemExit("checkpoint's hidden field disagrees with its own weights")

    limits = check_ranges(q, args.qa)
    tag = args.tag or f"e{state.get('epoch', '?')}-h{q['hidden']}-{checkpoint_hash[:12]}"

    blob = write_net(out, q, args, tag)
    digest = hashlib.sha256(blob).hexdigest()
    with open(out + ".sha256", "w", encoding="utf-8") as f:
        f.write(f"{digest}  {os.path.basename(out)}\n")

    print(f"wrote {out}  ({len(blob):,} bytes)")
    # The checkpoint as RESOLVED, not as typed: `net-fact` and the default both
    # arrive here as a path, and which one they arrived as is the first thing
    # anyone re-reading this output wants to know.
    print(f"  from      {checkpoint}  ({checkpoint_hash[:12]})")
    stack = "".join(f" -> {w}" for w in (q["l1_size"], q["l2_size"]) if w)
    print(f"  arch      {NUM_FEATURES} -> {q['hidden']}x2{stack} -> {q['buckets']}, "
          f"screlu, {FEATURE_SET_NAME}"
          + (", +uncertainty" if q["uncertainty"] else ""))
    print(f"  quant     qa {args.qa}  qb {args.qb}  scale {args.scale}"
          + (f"  shifts {q['l1_shift']}/{q['l2_shift']}" if q["l1_size"] else ""))
    print(f"  peaks     ft_w {limits['ft_w_peak']}  ft_b {limits['ft_b_peak']}  "
          f"out_w {limits['out_w_peak']}  (int16 holds {INT16_MAX})")

    # The int16 activation product is the FLAT architecture's bound and exists
    # only there - removing it is what the stack is for - so the stack reports
    # its own int32 sums in its place rather than a number that does not apply.
    bounds = [f"accumulator |x| <= {limits['accumulator_bound']} (int16)"]
    if "activation_product_bound" in limits:
        bounds.append(f"activation product <= {limits['activation_product_bound']} (int16)")
    for stage in ("l1", "l2"):
        if f"{stage}_bound" in limits:
            bounds.append(f"{stage.upper()} |x| <= {limits[f'{stage}_bound']} (int32)")
    bounds.append(f"output |x| <= {limits['output_bound']} (int32)")
    print("  bounds    " + ", ".join(bounds))
    print(f"  sha256    {digest}")

    # ------------------------------------------------------------ vectors --
    fens = collect_fens(args)
    fields = unpack(pack_fens(fens))
    raw, cp, unc_raw, unc_cp = forward(q, fields, args.qa, args.qb, args.scale)

    with open(vectors, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# {os.path.basename(out)} sha256 {digest}\n")
        if unc_raw is not None:
            f.write("# <raw int32> <centipawns> <unc raw> <unc cp> <fen>  "
                    "- src/nnue.c must reproduce all four exactly\n")
            for value, centipawns, uraw, ucp, fen in zip(raw, cp, unc_raw, unc_cp, fens):
                f.write(f"{int(value)} {int(centipawns)} {int(uraw)} {int(ucp)} {fen}\n")
        else:
            f.write("# <raw int32> <centipawns> <fen>  "
                    "- src/nnue.c must reproduce both exactly\n")
            for value, centipawns, fen in zip(raw, cp, fens):
                f.write(f"{int(value)} {int(centipawns)} {fen}\n")
    print(f"wrote {vectors}  ({len(fens):,} vectors)")

    # How far the quantised net drifted from the float one it came from. Not a
    # gate - the gate is C against these vectors - but a quantisation that has
    # gone badly wrong shows up here first, and in units anyone can judge.
    # SCReLU drifts more than a linear activation would for the same weights:
    # squaring a rounded activation squares its rounding error too.
    with torch.no_grad():
        from nnue.model import from_checkpoint

        model = from_checkpoint(state)
        model.eval()
        float_cp = model.evaluate_cp(
            torch.from_numpy(fields["white"]),
            torch.from_numpy(fields["black"]),
            torch.from_numpy(fields["stm"]).float().unsqueeze(1),
            torch.from_numpy(fields["piece_count"]),
        ).numpy()

    drift = np.abs(float_cp - cp)
    print(f"  vs float  mean |diff| {drift.mean():.2f} cp, max {drift.max():.2f} cp")
    print(f"  scores    |cp| max {np.abs(cp).max()} (clamp is {EVAL_LIMIT})")
    if unc_cp is not None:
        print(f"  unc       predicted |error| mean {unc_cp.mean():.1f} cp, "
              f"median {np.median(unc_cp):.1f} cp, max {unc_cp.max()} cp "
              f"- search.c's UncSigma* constants are centred against this")

    manifest = {
        "net": os.path.basename(out),
        "sha256": digest,
        "checkpoint": checkpoint_path,
        "checkpoint_sha256": checkpoint_hash,
        "run_id": state.get("run_id"),
        "stage": state.get("stage", "main"),
        "feature_factorization": arch["feature_factorization"],
        "metrics": state.get("metrics"),
        "epoch": state.get("epoch"),
        "val_loss": state.get("val_loss"),
        "format_version": FORMAT_VERSION,
        "features": NUM_FEATURES,
        "hidden": q["hidden"],
        "output_buckets": q["buckets"],
        "activation": "screlu",
        "feature_set": FEATURE_SET_NAME,
        "uncertainty": q["uncertainty"],
        "qa": args.qa,
        "qb": args.qb,
        "scale": args.scale,
        "tag": tag,
        "limits": limits,
        "vectors": os.path.basename(vectors),
        "vector_count": len(fens),
        "quantisation_drift_cp": {"mean": float(drift.mean()), "max": float(drift.max())},
    }
    write_json(manifest_path, manifest)


if __name__ == "__main__":
    main()
