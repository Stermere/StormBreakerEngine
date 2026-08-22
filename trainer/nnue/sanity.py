"""Score positions whose evaluation is known, and look at the numbers.

This has caught more bugs than any loss curve ever will. A net with a flipped
perspective plays reasonably, hates its own position, and trains to a loss
curve that looks completely normal. Nothing in the training loop notices; the
start position scoring +300 does.

The table prints three numbers per position, and they check different things:

``score``
    The evaluation, in centipawns, from the side to move - the same convention
    ``eval_evaluate`` uses.

``flip``
    The same position with both colours swapped, the ranks flipped and the side
    to move swapped. That is the same position seen from the other side, and
    because the score is side-to-move relative it must come back **identical**.
    This is a structural identity of the architecture, not something training
    produces: it holds on an untrained net, to floating-point precision, and a
    non-zero ``score - flip`` means the feature normalisation is not symmetric.

``null``
    The same board with the other side to move, which is a different position
    and should score roughly the **negative**. This is the semantic check, and
    the one that catches the bug the flip cannot: reading the non-side-to-move
    accumulator first is still perfectly colour-symmetric, and still evaluates
    every position from the opponent's point of view.

    It is only meaningful once the net has learnt something. A net that scores
    everything within a few centipawns of zero has no signs worth checking, so
    the summary only judges positions where both readings are decisive and says
    so plainly when there are too few - an alarm that fires on every early
    epoch is an alarm nobody reads by epoch ten.

Then read the material rows. A position a piece down should score roughly a
piece, with the right sign. Only ``flip`` is exact; everything else is a
judgement call, and the number to react to is one with the wrong sign or the
wrong order of magnitude.
"""

from __future__ import annotations

import argparse

import torch

from .format import pack_fens, unpack
from .model import NNUE

# FEN, description, and the rough centipawn score a working net should give.
SANITY_POSITIONS = [
    ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "start position", 0),
    ("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1", "after 1.e4", 0),
    ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKB1R w KQkq - 0 1", "white minus a knight", -300),
    ("rnbqkb1r/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "black minus a knight", 300),
    ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNB1KBNR w KQkq - 0 1", "white minus a queen", -900),
    ("4k3/8/8/8/8/8/8/R3K3 w Q - 0 1", "rook up, bare kings", 500),
    ("8/8/8/4k3/8/8/8/4K3 w - - 0 1", "bare kings", 0),
    ("8/5k2/8/8/8/8/5PPP/6K1 w - - 0 1", "three pawns up", 700),
    ("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3", "italian game", 0),
    ("3r1rk1/p3qppp/2bb1n2/1p6/3P4/1B3N2/PP2QPPP/R1B2RK1 w - - 2 18", "quiet middlegame", 0),
]


def flip_fen(fen: str) -> str:
    """Swap colours, flip ranks, swap the side to move.

    The result is the same position seen from the other side. A
    side-to-move-relative evaluation must therefore score it identically - see
    the module docstring; this is not the "negate the score" transform.
    """
    placement, side, rights, ep, *rest = fen.split()

    ranks = placement.split("/")
    flipped = "/".join(
        "".join(c.lower() if c.isupper() else c.upper() if c.islower() else c for c in rank)
        for rank in reversed(ranks)
    )

    new_rights = "".join(c.lower() if c.isupper() else c.upper() for c in rights if c != "-")
    new_rights = "".join(c for c in "KQkq" if c in new_rights) or "-"

    new_ep = "-" if ep == "-" else ep[0] + str(9 - int(ep[1]))
    tail = " ".join(rest) if rest else "0 1"
    return f"{flipped} {'b' if side == 'w' else 'w'} {new_rights} {new_ep} {tail}"


def null_fen(fen: str) -> str:
    """Hand the same board to the other side.

    A different position, and one that may not be reachable by legal play - the
    side that just "moved" can be left in check. That is fine: the network
    evaluates a board, and this is a diagnostic rather than a game. The en
    passant square goes, because it belonged to the side that no longer moves.
    """
    placement, side, rights, _ep, *rest = fen.split()
    tail = " ".join(rest) if rest else "0 1"
    return f"{placement} {'b' if side == 'w' else 'w'} {rights} - {tail}"


@torch.no_grad()
def score_fens(model: NNUE, fens, device=None) -> list:
    """Centipawn scores, side-to-move relative, one per FEN."""
    device = device or next(model.parameters()).device
    fields = unpack(pack_fens(fens))

    white = torch.from_numpy(fields["white"]).to(device)
    black = torch.from_numpy(fields["black"]).to(device)
    stm = torch.from_numpy(fields["stm"]).float().unsqueeze(1).to(device)

    was_training = model.training
    model.eval()
    scores = model.evaluate_cp(white, black, stm).cpu().tolist()
    model.train(was_training)
    return scores


def report(model: NNUE, device=None, positions=SANITY_POSITIONS) -> float:
    """Prints the table and returns the largest ``|score - flip|`` seen."""
    fens = [fen for fen, _, _ in positions]

    scores = score_fens(model, fens, device)
    flipped = score_fens(model, [flip_fen(f) for f in fens], device)
    nulled = score_fens(model, [null_fen(f) for f in fens], device)

    print(f"{'position':<24}{'score':>8}{'flip':>8}{'null':>8}{'expected':>10}")
    print("-" * 58)
    for (_, name, expected), score, other, null in zip(positions, scores, flipped, nulled):
        print(f"{name:<24}{score:>8.0f}{other:>8.0f}{null:>8.0f}{expected:>10}")

    asymmetry = max(abs(a - b) for a, b in zip(scores, flipped))
    print(f"\nflip: largest |score - flip| = {asymmetry:.1f} cp "
          f"({'exact, as it must be' if asymmetry < 1.0 else 'BUG: features are not symmetric'})")

    # Reported rather than asserted: tempo makes it approximate, and a
    # position both readings call roughly equal has no sign worth testing.
    decisive = [(a, b) for a, b in zip(scores, nulled) if abs(a) > 50 and abs(b) > 50]
    if len(decisive) < 4:
        print("null: too few decisive positions to judge yet - train further")
    else:
        agree = sum(1 for a, b in decisive if a * b < 0)
        verdict = "expected" if agree >= len(decisive) - 1 else "CHECK own/other IN forward()"
        print(f"null: {agree}/{len(decisive)} decisive positions flip sign when the turn "
              f"is passed ({verdict})")
    return asymmetry


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("checkpoint", help="a .pt written by nnue.train")
    parser.add_argument("--device", default=None)
    args = parser.parse_args()

    state = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    model = NNUE(hidden=state.get("hidden", 512))
    model.load_state_dict(state["model"])

    device = torch.device(args.device or ("cuda" if torch.cuda.is_available() else "cpu"))
    model.to(device)
    report(model, device)


if __name__ == "__main__":
    main()
