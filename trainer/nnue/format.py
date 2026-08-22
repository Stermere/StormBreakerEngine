"""The packed record format, and the features the network sees.

This module is the Python half of a contract whose C half is
``tools/datagen.c``. Both spell out the same 32-byte layout, and
``trainer/tests/test_format.py`` checks them against each other by comparing
FENs reconstructed here with FENs printed by ``datagen dump``. If the two ever
disagree the network trains on positions that are not the positions the engine
labelled, the loss curve looks completely normal, and nothing else notices.

    offset size  field
    0      8     occupied bitboard
    8      16    piece nibbles, one per occupied square, LSB-first square order
    24     1     bit 7 side to move; bits 0-6 en passant square (127 = none)
    25     1     halfmove clock
    26     2     fullmove number
    28     2     score, centipawns, side-to-move relative (int16)
    30     1     WDL from the side to move: 0 loss, 1 draw, 2 win, 3 unknown
    31     1     flags: bits 0-2 source tag, bit 3 in check, 4-7 reserved

Piece codes are ``type | color << 3`` with types 0-5 (pawn..king); type 6 is a
rook that still carries a castling right, which is how castling rights ride in
the nibbles instead of costing a byte.
"""

from __future__ import annotations

import numpy as np

# ---------------------------------------------------------------- record ----

RECORD_DTYPE = np.dtype(
    [
        ("occupied", "<u8"),
        ("pieces", "u1", (16,)),
        ("stm_ep", "u1"),
        ("halfmove", "u1"),
        ("fullmove", "<u2"),
        ("score", "<i2"),
        ("wdl", "u1"),
        ("flags", "u1"),
    ]
)
assert RECORD_DTYPE.itemsize == 32, "the record format is 32 bytes and numpy must agree"

POLICY_DTYPE = np.dtype([("best", "<u2"), ("cutoff", "<u2")])

EP_NONE = 127
NIB_ROOK_CASTLE = 6
SRC_MASK = 0x07
IN_CHECK = 0x08

WDL_LOSS, WDL_DRAW, WDL_WIN, WDL_UNKNOWN = 0, 1, 2, 3

SOURCE_NAMES = ("selfplay", "tree", "human", "engine", "book", "other")

# ---------------------------------------------------------------- features --
#
# (king slot, piece, square) per perspective, over both colours' pieces:
#
#     32 mirrored king squares x 12 planes x 64 squares = 24576
#
# The normalisation is the classical evaluation's, verbatim - rank-flip for the
# perspective's owner, file-mirror when that king sits on the kingside - so a
# mirrored king stands on one of 32 squares, and the net indexes that square
# directly. Reusing the normalisation is the payoff for having built the linear
# model as a factorised HalfKA: the extraction already existed and was already
# the thing the tuner fits.
#
# Indexing the square rather than a bucketing of it is what lets the net say
# "this knight is good with the king on g1 and bad with it on h1". It costs
# feature rows, and rows are what a large dataset is for.
#
# A position has at most 32 pieces, so a batch is a dense (B, 32) index matrix;
# records with fewer pieces pad with an entry whose embedding is pinned to zero.

KING_SQUARES = 32
PIECE_PLANES = 12
SQUARES = 64
MAX_PIECES = 32

NUM_FEATURES = KING_SQUARES * PIECE_PLANES * SQUARES  # 24576
PAD_INDEX = NUM_FEATURES

# NnueFeatureSet and NnueActivation in src/nnue.h. One of each is implemented,
# and the loader rejects anything else by name.
FEATURE_SET_TAG = 1
FEATURE_SET_NAME = "halfka-32sq"
ACTIVATION_TAG = 1
ACTIVATION_NAME = "screlu"


def king_index(normalised_king):
    """The king slot for an already-normalised king square.

    ``normalised_king`` has been rank-flipped for black and file-mirrored, so
    its file is 0-3 and there are exactly 32 squares it can be on. Spelled out
    again in ``nnue_king_square()`` in src/nnue.c, deliberately: the net's
    indexing is free to move on while the classical evaluation's stays where
    the tuner fitted it.
    """
    return (normalised_king >> 3) * 4 + (normalised_king & 7)


# ---------------------------------------------------------- output buckets --
#
# One output layer per phase, selected by piece count. A single output has to
# answer for a 32-piece opening and a 5-piece endgame with one row of weights,
# and the two want opposite things from the same accumulator; separating them
# is most of what output buckets buy.
#
# Piece count is the phase proxy because it is free - the engine has the
# popcount in hand - and because selection then costs one index rather than a
# branch per unit. Both kings are always on, so the count is 2..32 and the
# index is (count - 2) // (32 // buckets), which needs the count to divide 32.

DEFAULT_OUTPUT_BUCKETS = 8


def check_output_buckets(buckets: int) -> int:
    if buckets < 1 or 32 % buckets:
        raise ValueError(f"output buckets must be a divisor of 32, got {buckets}")
    return buckets


def output_bucket(piece_count, buckets: int):
    """Which output row a position reads.

    Works on ints, numpy arrays and torch tensors alike, which is the point:
    src/nnue.c, the numpy reference and the trainer have to agree on this to
    the last position, and three spellings of it is three chances to disagree.
    """
    check_output_buckets(buckets)
    return (piece_count - 2) // (32 // buckets)


# ------------------------------------------------------------ quantisation --
#
# These live here, beside the features, rather than in the exporter, because
# the TRAINER needs them too: weights are clipped during training to the range
# that keeps the engine's integer arithmetic in bounds, and a clip computed
# from a different QB than the exporter quantises with is a clip that does not
# clip.
#
#     eval_cp = raw * SCALE / (QA * QB)
#
# so a float model output of 1.0 is SCALE centipawns. That is what pins the
# float model's units to the quantised ones and makes the exporter a pure
# re-scaling rather than a re-interpretation.

QA = 255
QB = 64
SCALE = 400
NET_TO_CP = float(SCALE)

# The bound weight clipping enforces, in float units.
#
# The OUTPUT weights are the binding constraint, and not because of anything
# the scalar C code does - it sums SCReLU terms in int64. A SIMD SCReLU
# multiplies the clamped activation by the weight as int16 (`v * w`, then a
# widening madd), so `QA * max|w_int|` has to fit int16: 255 * 127 = 32385
# against 32767.
#
# Clipping the feature transformer to the same bound is a bonus rather than a
# requirement, and it makes the int16 accumulator safe by construction: 32
# pieces plus the bias is at most 33 * QA * 1.984 = 16.7k, well inside int16,
# so the exporter's accumulator bound check becomes a formality instead of a
# coin toss decided by how training happened to go.
WEIGHT_CLIP = 127.0 / QB


# ------------------------------------------------------------ unpacking -----


def unpack(records: np.ndarray) -> dict:
    """Explode a batch of packed records into arrays.

    Everything here is whole-array arithmetic on a memmap slice: there is no
    per-record Python, which is the difference between a loader that keeps a
    GPU busy and one that does not.

    Returns a dict with, for a batch of B records:

        white       (B, 32) int64 feature indices from white's perspective
        black       (B, 32) int64 feature indices from black's perspective
        stm         (B,)    int64, 1 when black is to move
        score       (B,)    int32 centipawns, side-to-move relative
        wdl         (B,)    int64 in {0, 1, 2, 3}
        source      (B,)    int64 source tag
        in_check    (B,)    bool
        piece_count (B,)    int64
    """
    batch = len(records)

    occ = np.ascontiguousarray(records["occupied"])
    bits = np.unpackbits(occ.view(np.uint8).reshape(batch, 8), axis=1, bitorder="little")

    counts = bits.sum(axis=1).astype(np.int64)
    rows, squares = np.nonzero(bits)  # row-major, so ascending square per record
    squares = squares.astype(np.int64)

    packed = np.ascontiguousarray(records["pieces"]).reshape(batch, 16)
    nibbles = np.empty((batch, 32), dtype=np.uint8)
    nibbles[:, 0::2] = packed & 0x0F
    nibbles[:, 1::2] = packed >> 4

    # Position of each piece within its own record, which is both the nibble
    # index and the column it occupies in the padded feature matrix.
    starts = np.zeros(batch, dtype=np.int64)
    np.cumsum(counts[:-1], out=starts[1:])
    slot = np.arange(rows.size, dtype=np.int64) - starts[rows]

    codes = nibbles[rows, slot].astype(np.int64)
    colour = codes >> 3
    raw = codes & 7
    ptype = np.where(raw == NIB_ROOK_CASTLE, 3, raw)  # a castling rook is a rook

    kings = {}
    for c in (0, 1):
        sel = (ptype == 5) & (colour == c)
        ksq = np.zeros(batch, dtype=np.int64)
        ksq[rows[sel]] = squares[sel]
        kings[c] = ksq

    out = {}
    for perspective in (0, 1):
        ksq = kings[perspective]
        # Rank-flip so the perspective's owner always reads the board from
        # rank 1, then mirror when that king sits on the kingside. The mirror
        # is driven by the KING and applied to both squares, or the table would
        # be indexed inconsistently - the same rule TERM_PSQK follows.
        king_n = ksq ^ 56 if perspective == 1 else ksq
        mirror = (king_n & 7) >= 4
        king_n = np.where(mirror, king_n ^ 7, king_n)
        king_slot = king_index(king_n)

        sq_n = squares ^ 56 if perspective == 1 else squares
        sq_n = np.where(mirror[rows], sq_n ^ 7, sq_n)

        plane = np.where(colour == perspective, 0, 6) + ptype
        feature = king_slot[rows] * (PIECE_PLANES * SQUARES) + plane * SQUARES + sq_n

        idx = np.full((batch, MAX_PIECES), PAD_INDEX, dtype=np.int64)
        idx[rows, slot] = feature
        out["white" if perspective == 0 else "black"] = idx

    stm_ep = records["stm_ep"]
    flags = records["flags"]

    out["stm"] = (stm_ep >> 7).astype(np.int64)
    out["score"] = records["score"].astype(np.int32)
    out["wdl"] = records["wdl"].astype(np.int64)
    out["source"] = (flags & SRC_MASK).astype(np.int64)
    out["in_check"] = (flags & IN_CHECK) != 0
    out["piece_count"] = counts
    return out


# ------------------------------------------------------ FEN <-> record ------

_PIECE_CHARS = "PNBRQK"


def _square(file_: int, rank: int) -> int:
    return rank * 8 + file_


def record_to_fen(record: np.void) -> str:
    """The inverse of :func:`pack_fen`, and the mirror of ``record_to_fen`` in
    ``tools/datagen.c``. Used by the tests to prove the two agree."""
    board = [None] * 64
    castle_rooks = []

    occupied = int(record["occupied"])
    nibbles = bytes(record["pieces"])

    n = 0
    bb = occupied
    while bb:
        sq = (bb & -bb).bit_length() - 1
        bb &= bb - 1
        code = (nibbles[n >> 1] >> ((n & 1) * 4)) & 0xF
        n += 1

        raw = code & 7
        if raw > NIB_ROOK_CASTLE:
            raise ValueError(f"unused piece code {code} at square {sq}")
        black = bool(code & 8)
        ptype = 3 if raw == NIB_ROOK_CASTLE else raw
        char = _PIECE_CHARS[ptype]
        board[sq] = char.lower() if black else char
        if raw == NIB_ROOK_CASTLE:
            castle_rooks.append(sq)

    rows = []
    for rank in range(7, -1, -1):
        row, empty = "", 0
        for file_ in range(8):
            piece = board[_square(file_, rank)]
            if piece is None:
                empty += 1
                continue
            if empty:
                row += str(empty)
                empty = 0
            row += piece
        if empty:
            row += str(empty)
        rows.append(row)
    placement = "/".join(rows)

    stm_ep = int(record["stm_ep"])
    black_to_move = bool(stm_ep & 0x80)

    kings = {}
    for sq, piece in enumerate(board):
        if piece == "K":
            kings[False] = sq
        elif piece == "k":
            kings[True] = sq

    rights = ""
    for sq in castle_rooks:
        black = board[sq].islower()
        kingside = (sq & 7) > (kings[black] & 7)
        rights += ("k" if kingside else "q") if black else ("K" if kingside else "Q")
    # FEN spells rights in KQkq order regardless of the order the rooks were
    # walked in, which is ascending square.
    rights = "".join(c for c in "KQkq" if c in rights) or "-"

    ep = stm_ep & 0x7F
    ep_str = "-" if ep == EP_NONE else "abcdefgh"[ep & 7] + str((ep >> 3) + 1)

    return (
        f"{placement} {'b' if black_to_move else 'w'} {rights} {ep_str} "
        f"{int(record['halfmove'])} {int(record['fullmove'])}"
    )


def _ep_target_is_real(board: list, ep: int, black_to_move: bool) -> bool:
    """board.c's ``ep_target_is_real``: the square is only kept when a pawn
    really did double-push to it. board_set_fen normalises this away, so a
    record produced from a parsed FEN never carries a bogus one - and this
    function is what keeps :func:`pack_fen` producing the same bytes."""
    up = -8 if black_to_move else 8
    rel_rank = (ep >> 3) ^ (7 if black_to_move else 0)
    if rel_rank != 5:  # RANK_6 seen from the side to move
        return False
    pawn_sq, from_sq = ep - up, ep + up
    if not 0 <= pawn_sq < 64 or not 0 <= from_sq < 64:
        return False
    want = "P" if black_to_move else "p"
    return board[pawn_sq] == want and board[ep] is None and board[from_sq] is None


def pack_fen(fen: str, score: int = 0, wdl: int = WDL_UNKNOWN, source: int = 5,
             in_check: bool = False) -> np.void:
    """Pack a FEN into a record, the way ``datagen`` would have.

    This is how :mod:`nnue.sanity` gets features for a hand-written position
    without going through a shard: one feature path, exercised by both.
    """
    parts = fen.split()
    if len(parts) < 4:
        raise ValueError(f"not a FEN: {fen!r}")
    placement, side, rights, ep_field = parts[:4]
    halfmove = int(parts[4]) if len(parts) > 4 else 0
    fullmove = int(parts[5]) if len(parts) > 5 else 1

    board = [None] * 64
    rank, file_ = 7, 0
    for ch in placement:
        if ch == "/":
            rank, file_ = rank - 1, 0
        elif ch.isdigit():
            file_ += int(ch)
        else:
            board[_square(file_, rank)] = ch
            file_ += 1

    kings = {}
    for sq, piece in enumerate(board):
        if piece == "K":
            kings[False] = sq
        elif piece == "k":
            kings[True] = sq
    if False not in kings or True not in kings:
        raise ValueError(f"FEN is missing a king: {fen!r}")

    # The rook a right refers to: the outermost one on the king's side of the
    # king, matching castling_rook() in datagen.c.
    castle_rooks = set()
    for flag, black, kingside in (("K", False, True), ("Q", False, False),
                                  ("k", True, True), ("q", True, False)):
        if flag not in rights:
            continue
        ksq = kings[black]
        want = "r" if black else "R"
        candidates = [
            sq
            for sq in range(64)
            if board[sq] == want and (sq >> 3) == (ksq >> 3)
            and ((sq & 7) > (ksq & 7) if kingside else (sq & 7) < (ksq & 7))
        ]
        if candidates:
            castle_rooks.add(max(candidates) if kingside else min(candidates))

    black_to_move = side == "b"
    ep = EP_NONE
    if ep_field != "-":
        candidate = "abcdefgh".index(ep_field[0]) + (int(ep_field[1]) - 1) * 8
        if _ep_target_is_real(board, candidate, black_to_move):
            ep = candidate

    occupied = 0
    nibbles = bytearray(16)
    n = 0
    for sq in range(64):
        piece = board[sq]
        if piece is None:
            continue
        occupied |= 1 << sq
        black = piece.islower()
        ptype = _PIECE_CHARS.index(piece.upper())
        code = NIB_ROOK_CASTLE if sq in castle_rooks else ptype
        code |= 8 if black else 0
        nibbles[n >> 1] |= code << ((n & 1) * 4)
        n += 1

    record = np.zeros(1, dtype=RECORD_DTYPE)[0]
    record["occupied"] = occupied
    record["pieces"] = np.frombuffer(bytes(nibbles), dtype=np.uint8)
    record["stm_ep"] = ep | (0x80 if black_to_move else 0)
    record["halfmove"] = min(halfmove, 255)
    record["fullmove"] = min(fullmove, 65535)
    record["score"] = max(-32000, min(32000, score))
    record["wdl"] = wdl
    record["flags"] = (source & SRC_MASK) | (IN_CHECK if in_check else 0)
    return record


def pack_fens(fens) -> np.ndarray:
    """A record array from an iterable of FENs, ready for :func:`unpack`."""
    fens = list(fens)
    out = np.zeros(len(fens), dtype=RECORD_DTYPE)
    for i, fen in enumerate(fens):
        out[i] = pack_fen(fen)
    return out


def read_shard(path, count=None, offset=0) -> np.ndarray:
    """Memory-map a shard. Never read one with ``np.fromfile``: a full dataset
    is gigabytes and the trainer only ever touches a batch at a time."""
    data = np.memmap(path, dtype=RECORD_DTYPE, mode="r")
    if count is None:
        return data[offset:]
    return data[offset:offset + count]
