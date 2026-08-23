"""Fit the network.

    python -m nnue.train --train external/data/train.cnn \\
                         --val   external/data/val.cnn \\
                         --epochs 10 --out external/nets/net

The acceptance gate for this task is NOT the training loss. It is the held-out
loss plus the sanity table printed after every epoch: a net whose loss curve
looks perfect and whose start position scores +300 has a perspective bug, and
only the second of those two will tell you.

Expect tens of minutes per 100M-position epoch on an RTX 3070, and 5-15 epochs.
If it is much worse than that the bottleneck is the loader, not the model - try
--workers and a larger --batch-size before touching anything else.

The architecture is SCReLU over 32 mirrored king squares, and the only two
things left to choose are shape: --hidden and --output-buckets. Both are header
fields the engine reads out of the net file, so changing either is a retrain
and an export, with no C change and no flag to keep in sync.

24576 feature rows is a lot to fit. A few million positions leave most rows
seen a handful of times, and the width is what to trade down first - --hidden
512 trains and exports exactly the same way.
"""

from __future__ import annotations

import argparse
import json
import os
import time

import torch

from . import sanity
from .dataset import make_loader, to_device
from .format import DEFAULT_OUTPUT_BUCKETS
from .model import DEFAULT_HIDDEN, NNUE, blended_target, loss_fn

# The evaluation's sigmoid scale, in centipawns. Keeping it equal to the value
# tools/tuner.c fitted is what keeps the target in the same win-probability
# units the linear evaluation was fitted against, so the two are comparable.
DEFAULT_SIGMOID_K = 400.0


def evaluate(model, loader, device, sigmoid_k, lam, limit=None) -> float:
    model.eval()
    total, seen = 0.0, 0
    with torch.no_grad():
        for i, batch in enumerate(loader):
            if limit and i >= limit:
                break
            batch = to_device(batch, device)
            prediction = model(batch["white"], batch["black"], batch["stm"],
                               batch["piece_count"])
            target = blended_target(batch["score"], batch["wdl"], lam, sigmoid_k)
            n = prediction.numel()
            total += loss_fn(prediction, target, sigmoid_k).item() * n
            seen += n
    model.train()
    return total / max(seen, 1)


def train(args) -> None:
    device = torch.device(args.device or ("cuda" if torch.cuda.is_available() else "cpu"))
    print(f"device: {device}"
          f"{' (' + torch.cuda.get_device_name(0) + ')' if device.type == 'cuda' else ''}")

    model = NNUE(hidden=args.hidden, output_buckets=args.output_buckets).to(device)
    print(f"net:    {model.describe()}")

    train_loader = make_loader(args.train, args.batch_size, args.workers, shuffle=True,
                               sources=args.sources)
    val_loader = (make_loader(args.val, args.batch_size, max(args.workers // 2, 0),
                              shuffle=False, sources=args.sources) if args.val else None)

    print(f"train: {train_loader.dataset.records:,} records "
          f"in {len(train_loader)} batches of {args.batch_size}")
    if val_loader:
        print(f"val:   {val_loader.dataset.records:,} records")
    optimiser = torch.optim.AdamW(model.parameters(), lr=args.lr,
                                  weight_decay=args.weight_decay)
    scheduler = torch.optim.lr_scheduler.ExponentialLR(optimiser, gamma=args.lr_gamma)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    history = []

    for epoch in range(1, args.epochs + 1):
        # Anneal lambda toward the game result: early epochs distil the search,
        # later ones let the outcome pull the net off the teacher's systematic
        # errors. Both ends are hyperparameters worth two or three runs.
        span = max(args.epochs - 1, 1)
        lam = args.lambda_start + (args.lambda_end - args.lambda_start) * (epoch - 1) / span

        started = time.time()
        running, seen, batches = 0.0, 0, 0

        for batch in train_loader:
            if args.limit_batches and batches >= args.limit_batches:
                break
            batch = to_device(batch, device)

            prediction = model(batch["white"], batch["black"], batch["stm"],
                               batch["piece_count"])
            target = blended_target(batch["score"], batch["wdl"], lam, args.sigmoid_k)
            loss = loss_fn(prediction, target, args.sigmoid_k)

            optimiser.zero_grad(set_to_none=True)
            loss.backward()
            optimiser.step()

            # After the step, every step. This is what makes the exported net
            # representable rather than merely hopefully-representable: see
            # WEIGHT_CLIP in format.py for the bound and why it is that number.
            if not args.no_weight_clip:
                model.clip_weights()

            n = prediction.numel()
            running += loss.item() * n
            seen += n
            batches += 1

            if args.log_every and batches % args.log_every == 0:
                rate = seen / max(time.time() - started, 1e-6)
                print(f"  epoch {epoch} batch {batches}/{len(train_loader)} "
                      f"loss {running / seen:.6f}  {rate:,.0f} pos/s", flush=True)

        scheduler.step()
        elapsed = time.time() - started
        train_loss = running / max(seen, 1)
        val_loss = (evaluate(model, val_loader, device, args.sigmoid_k, lam,
                             args.limit_batches) if val_loader else float("nan"))

        print(f"epoch {epoch:>3}  lambda {lam:.2f}  lr {scheduler.get_last_lr()[0]:.2e}  "
              f"train {train_loss:.6f}  val {val_loss:.6f}  "
              f"{seen / max(elapsed, 1e-6):,.0f} pos/s  {elapsed:.0f}s", flush=True)

        history.append({"epoch": epoch, "lambda": lam, "train": train_loss, "val": val_loss})

        print()
        sanity.report(model, device)
        print()

        checkpoint = {
            "model": model.state_dict(),
            # Self-describing on purpose: the exporter reads the architecture
            # out of the checkpoint rather than being told it again on the
            # command line, so there is no pair of flags to keep in sync and no
            # way to export a net under the wrong shape.
            "arch": model.arch,
            "hidden": args.hidden,
            "epoch": epoch,
            "sigmoid_k": args.sigmoid_k,
            "train_loss": train_loss,
            "val_loss": val_loss,
            "args": vars(args),
        }
        torch.save(checkpoint, f"{args.out}.pt")
        if args.checkpoint_every and epoch % args.checkpoint_every == 0:
            torch.save(checkpoint, f"{args.out}-epoch{epoch}.pt")

        with open(f"{args.out}-history.json", "w", encoding="utf-8") as f:
            json.dump(history, f, indent=2)

    print(f"wrote {args.out}.pt")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--train", nargs="+", required=True, help="shard(s) to fit")
    parser.add_argument("--val", nargs="*", default=None, help="held-out shard(s)")
    parser.add_argument("--out", default="external/nets/net", help="checkpoint prefix")

    parser.add_argument("--epochs", type=int, default=10)
    parser.add_argument("--batch-size", type=int, default=16384)
    parser.add_argument("--hidden", type=int, default=DEFAULT_HIDDEN,
                        help="hidden width per perspective; a multiple of 16")
    parser.add_argument("--output-buckets", type=int, default=DEFAULT_OUTPUT_BUCKETS,
                        help="output rows, selected by piece count; must divide 32")
    parser.add_argument("--no-weight-clip", action="store_true",
                        help="do not hold weights inside the quantised range. The export "
                             "will then refuse nets it cannot represent, which is the "
                             "point: this flag is for measuring what the clip costs")
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--lr-gamma", type=float, default=0.8, help="LR decay per epoch")
    parser.add_argument("--weight-decay", type=float, default=0.0)

    parser.add_argument("--lambda-start", type=float, default=0.9,
                        help="weight on the search score in epoch 1")
    parser.add_argument("--lambda-end", type=float, default=0.9,
                        help="weight on the search score in the final epoch")
    parser.add_argument("--sigmoid-k", type=float, default=DEFAULT_SIGMOID_K,
                        help="centipawns per unit of the win-probability sigmoid")

    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--device", default=None)
    parser.add_argument("--sources", type=int, nargs="*", default=None,
                        help="keep only these source tags (0 selfplay, 1 tree, "
                             "2 human, 3 engine, 4 book, 5 other)")
    parser.add_argument("--limit-batches", type=int, default=0,
                        help="stop each epoch after N batches (smoke tests)")
    parser.add_argument("--log-every", type=int, default=50)
    parser.add_argument("--checkpoint-every", type=int, default=0)
    return parser.parse_args(argv)


def main() -> None:
    train(parse_args())


if __name__ == "__main__":
    # Required on Windows: the DataLoader spawns worker processes, which
    # re-import this module.
    main()
