"""Invariants of the feature extraction.

The normalisation - rank-flip for the perspective's owner, file-mirror when
that king is on the kingside, then the mirrored king square - has two
symmetries that must hold exactly. Both catch the classic bugs:

  * a position and its file-mirror produce the same features. A mirror applied
    to the king but not the pieces, or with the wrong comparison, breaks this.
  * white's features of a position equal black's features of the same position
    with the colours swapped and the ranks flipped. A perspective that reads
    its own pieces as the enemy's breaks this, and nothing else will notice.
"""

import numpy as np
import pytest

from nnue.format import (
    KING_SQUARES,
    MAX_PIECES,
    NUM_FEATURES,
    PAD_INDEX,
    PIECE_PLANES,
    SQUARES,
    king_index,
    pack_fens,
    unpack,
)
from nnue.sanity import flip_fen

# Deliberately without castling rights or en passant squares: the point here is
# the geometry, and mirroring rights correctly in the test would be a second
# implementation of something the test is not testing.
POSITIONS = [
    "4k3/8/8/8/8/8/8/4K3 w - - 0 1",
    "6k1/5ppp/8/8/8/8/5PPP/6K1 w - - 0 1",
    "2r3k1/1b1nqpbp/pp4p1/5P2/1PN5/4Q3/P5PP/1B2B1K1 b - - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "1k1r4/pp1b1R2/3q2pp/4p3/2B5/4Q3/PPP2B2/2K5 b - - 0 1",
    "8/8/4k3/3nn3/8/8/4K3/8 w - - 0 1",
]

START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"


def mirror_fen(fen: str) -> str:
    """Reflect the board across the d/e file boundary."""
    placement, side, rights, ep, *rest = fen.split()
    assert rights == "-" and ep == "-", "mirroring rights/ep is out of scope here"

    ranks = []
    for rank in placement.split("/"):
        expanded = "".join("." * int(c) if c.isdigit() else c for c in rank)[::-1]
        out, empty = "", 0
        for ch in expanded:
            if ch == ".":
                empty += 1
                continue
            if empty:
                out += str(empty)
                empty = 0
            out += ch
        if empty:
            out += str(empty)
        ranks.append(out)

    tail = " ".join(rest) if rest else "0 1"
    return f"{'/'.join(ranks)} {side} - - {tail}"


def feature_sets(fens):
    fields = unpack(pack_fens(fens))
    out = []
    for i in range(len(fens)):
        out.append({
            key: sorted(int(v) for v in fields[key][i] if v != PAD_INDEX)
            for key in ("white", "black")
        })
    return out


@pytest.mark.parametrize("fen", POSITIONS)
def test_file_mirror_leaves_features_unchanged(fen):
    original, mirrored = feature_sets([fen, mirror_fen(fen)])
    assert original["white"] == mirrored["white"]
    assert original["black"] == mirrored["black"]


@pytest.mark.parametrize("fen", POSITIONS + [START])
def test_colour_flip_swaps_the_perspectives(fen):
    original, flipped = feature_sets([fen, flip_fen(fen)])
    assert original["white"] == flipped["black"]
    assert original["black"] == flipped["white"]


def test_start_position_is_symmetric():
    """The start position maps onto itself under a colour flip, so its two
    perspectives must produce identical features."""
    (start,) = feature_sets([START])
    assert start["white"] == start["black"]
    assert len(start["white"]) == 32


def test_feature_index_decomposes_as_documented():
    """slot * 768 + plane * 64 + square, with own pieces on planes 0-5."""
    (bare,) = feature_sets(["4k3/8/8/8/8/8/8/4K3 w - - 0 1"])

    # Two kings, so two features per perspective: our king on plane 5, theirs
    # on plane 11.
    assert len(bare["white"]) == 2
    planes = sorted((f % (PIECE_PLANES * SQUARES)) // SQUARES for f in bare["white"])
    assert planes == [5, 11]

    # Both kings are on the e-file, which mirrors, so both perspectives land in
    # the same slot and the two perspectives are identical here.
    assert bare["white"] == bare["black"]


def test_every_piece_gets_exactly_one_feature():
    fields = unpack(pack_fens(POSITIONS + [START]))
    counts = fields["piece_count"]
    for key in ("white", "black"):
        active = (fields[key] != PAD_INDEX).sum(axis=1)
        assert np.array_equal(active, counts)
        assert fields[key].shape[1] == MAX_PIECES
        assert fields[key][fields[key] != PAD_INDEX].max() < NUM_FEATURES


def test_a_mirrored_king_reaches_exactly_thirty_two_slots():
    """The feature count is 32 x 12 x 64, and the 32 is a claim about geometry:
    after the file-mirror a king stands on files 0-3 and nowhere else. If it
    could reach a fifth file the table would be indexed out of range; if it
    reached fewer, a quarter of the net would be dead weight."""
    squares = np.arange(64)
    reachable = (squares & 7) < 4
    slots = king_index(squares[reachable])

    assert sorted(slots.tolist()) == list(range(KING_SQUARES))
    assert NUM_FEATURES == KING_SQUARES * PIECE_PLANES * SQUARES


def test_side_to_move_bit():
    fields = unpack(pack_fens([START, flip_fen(START)]))
    assert fields["stm"].tolist() == [0, 1]
