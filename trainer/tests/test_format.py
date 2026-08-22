"""The format contract between tools/datagen.c and nnue/format.py.

If these fail, the network is training on positions that are not the positions
the engine labelled. Nothing downstream detects that: the loss curve is normal,
the net is merely weak, and the cause is invisible for a month.
"""

import numpy as np
import pytest

from nnue.format import (
    IN_CHECK,
    MAX_PIECES,
    NUM_FEATURES,
    PAD_INDEX,
    RECORD_DTYPE,
    SOURCE_NAMES,
    SRC_MASK,
    pack_fen,
    read_shard,
    record_to_fen,
    unpack,
)

START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"


def test_record_is_thirty_two_bytes():
    assert RECORD_DTYPE.itemsize == 32


def test_python_and_c_agree_on_every_field(shard_path, datagen_dump):
    """The gate. Decode the same records twice - once by datagen, once here -
    and require identical FENs, scores, WDLs, source tags and check bits."""
    rows = datagen_dump(256)
    records = read_shard(shard_path, count=len(rows))
    assert len(records) == len(rows)

    for i, (record, row) in enumerate(zip(records, rows)):
        assert record_to_fen(record) == row["fen"], f"record {i}"
        assert int(record["score"]) == row["score"], f"record {i}"
        assert int(record["wdl"]) == row["wdl"], f"record {i}"
        assert SOURCE_NAMES[int(record["flags"]) & SRC_MASK] == row["source"], f"record {i}"
        assert bool(int(record["flags"]) & IN_CHECK) is row["in_check"], f"record {i}"


def test_pack_fen_inverts_record_to_fen(shard_path):
    """FEN -> record must give back the bytes the record started with. This is
    the Python half of the round-trip `datagen verify` runs in C."""
    records = read_shard(shard_path, count=512)

    for i, record in enumerate(records):
        fen = record_to_fen(record)
        repacked = pack_fen(
            fen,
            score=int(record["score"]),
            wdl=int(record["wdl"]),
            source=int(record["flags"]) & SRC_MASK,
            in_check=bool(int(record["flags"]) & IN_CHECK),
        )
        assert repacked.tobytes() == record.tobytes(), f"record {i}: {fen}"


def test_pack_fen_round_trips_hand_written_positions():
    fens = [
        START,
        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "4k3/8/8/8/8/8/8/4K2R w K - 13 47",
        "r3k3/8/8/8/8/8/8/4K3 b q - 0 1",
        "8/8/8/8/8/8/8/K6k w - - 99 300",
    ]
    for fen in fens:
        assert record_to_fen(pack_fen(fen)) == fen


def test_en_passant_is_normalised_the_way_board_set_fen_does_it():
    """board.c drops an en passant square that no pawn actually double-pushed
    to. pack_fen must drop it too, or a hand-written FEN packs to bytes datagen
    would never have written."""
    bogus = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e6 0 1"
    assert record_to_fen(pack_fen(bogus)).split()[3] == "-"

    real = "rnbqkbnr/pppp1ppp/8/4p3/8/8/PPPPPPPP/RNBQKBNR w KQkq e6 0 2"
    assert record_to_fen(pack_fen(real)).split()[3] == "e6"


def test_castling_rights_survive_the_nibble_encoding():
    for rights in ("KQkq", "Kq", "Q", "kq", "-"):
        fen = f"r3k2r/8/8/8/8/8/8/R3K2R w {rights} - 0 1"
        assert record_to_fen(pack_fen(fen)).split()[2] == rights


def test_unpack_shapes_and_bounds(shard_path):
    records = read_shard(shard_path, count=64)
    fields = unpack(records)

    for key in ("white", "black"):
        idx = fields[key]
        assert idx.shape == (len(records), MAX_PIECES)
        assert idx.min() >= 0
        assert idx.max() <= PAD_INDEX
        real = idx[idx != PAD_INDEX]
        assert real.max() < NUM_FEATURES

    # Exactly one feature per piece, per perspective.
    counts = fields["piece_count"]
    for key in ("white", "black"):
        assert np.array_equal((fields[key] != PAD_INDEX).sum(axis=1), counts)

    assert set(np.unique(fields["stm"])) <= {0, 1}
    assert fields["wdl"].max() <= 3
    assert counts.min() >= 2 and counts.max() <= MAX_PIECES


def test_unpack_matches_per_record_packing(shard_path):
    """Batched unpacking must give the same answer as unpacking one record at a
    time - the vectorised index arithmetic is the easiest thing here to get
    subtly wrong for one row in a batch."""
    records = read_shard(shard_path, count=97)  # deliberately not a round number
    batched = unpack(records)

    for i in range(0, len(records), 13):
        single = unpack(records[i:i + 1])
        assert np.array_equal(single["white"][0], batched["white"][i])
        assert np.array_equal(single["black"][0], batched["black"][i])


@pytest.mark.parametrize("count", [1, 2, 31, 97])
def test_unpack_handles_any_batch_size(shard_path, count):
    fields = unpack(read_shard(shard_path, count=count))
    assert fields["white"].shape[0] == count
