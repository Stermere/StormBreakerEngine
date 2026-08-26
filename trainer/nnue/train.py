"""Fit the network.

    python -m nnue.train --train external/data/train.cnn \\
                         --val   external/data/val.cnn \\
                         --epochs 10 --out external/nets/net

The acceptance gate for this task is NOT the training loss. It is the held-out
loss plus the sanity table printed after every epoch: a net whose loss curve
looks perfect and whose start position scores +300 has a perspective bug, and
only the second of those two will tell you.

Expect tens of minutes per 100M-position epoch on an RTX 3070, and 5-15 epochs.
At 1024 wide with 6 workers that is about 390k positions/s, and at that point
the GPU is the constraint and not the loader: the loader alone measures 1.2M
positions/s on the same machine. Raise --workers until it stops helping - four
to six is usually where that is - and after that the things that move the
number are --batch-size and the width.

The architecture is SCReLU over 32 mirrored king squares, and the only two
things left to choose are shape: --hidden and --output-buckets. Both are header
fields the engine reads out of the net file, so changing either is a retrain
and an export, with no C change and no flag to keep in sync.

24576 feature rows is a lot to fit. A few million positions leave most rows
seen a handful of times, and the width is what to trade down first - --hidden
512 trains and exports exactly the same way.

ON A DATASET OF HUNDREDS OF MILLIONS OF POSITIONS, two flags stop mattering
only to the impatient and start mattering to the run finishing at all:

  --positions-per-epoch  makes an "epoch" a position count rather than a pass
                         over the file, so the sanity table, the held-out loss
                         and the checkpoint land at a cadence you choose
                         instead of once per 500M positions;
  --resume               picks the run back up from its last checkpoint. A
                         multi-day run WILL be interrupted, and without this
                         the interruption costs the whole run.

Both are documented in trainer/README.md, along with what they do to the LR
schedule and the lambda anneal - which are per-epoch, and so become finer
grained rather than staying put when an epoch gets shorter.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import time

import torch

from . import sanity
from .dataset import make_loader, set_epoch, to_device
from .format import DEFAULT_OUTPUT_BUCKETS
from .model import DEFAULT_HIDDEN, NNUE, arch_from_checkpoint, blended_target, loss_fn

# The evaluation's sigmoid scale, in centipawns. Keeping it equal to the value
# tools/tuner.c fitted is what keeps the target in the same win-probability
# units the linear evaluation was fitted against, so the two are comparable.
DEFAULT_SIGMOID_K = 400.0

# Optimiser, scheduler and RNG live beside the checkpoint rather than in it.
# They are twice the size of the model and no downstream tool wants them: the
# exporter reads {out}.pt, and a 300 MB .pt that is mostly Adam moments would
# be 200 MB of it copied around for nothing.
RESUME_SUFFIX = "-resume.pt"


def count(text: str) -> int:
    """A position count, optionally with a k/M/B suffix.

    ``--positions-per-epoch 50M``. These numbers have eight or nine digits and
    a mistyped one is an epoch that is ten times too long, which is not
    something the first log line makes obvious.
    """
    text = str(text).strip().replace("_", "").replace(",", "")
    scale = {"k": 10 ** 3, "m": 10 ** 6, "b": 10 ** 9, "g": 10 ** 9}.get(text[-1:].lower())
    value = float(text[:-1]) * scale if scale else float(text)
    if value < 0 or value != int(value):
        raise argparse.ArgumentTypeError(f"not a whole non-negative count: {text!r}")
    return int(value)


def save_atomically(obj, path: str) -> None:
    """Write a checkpoint that a crash cannot half-replace.

    ``torch.save`` straight onto the live file leaves a truncated one behind if
    the process dies mid-write, and on a multi-day run the live file is the
    only copy of days of work. This leaves either the old file or the new one.
    """
    tmp = f"{path}.tmp"
    torch.save(obj, tmp)
    os.replace(tmp, path)


def make_optimiser(model, args, device):
    """AdamW, fused on CUDA.

    The fused kernel is the same arithmetic in one launch instead of several
    over 25M parameters, so it is a speedup and not a behavioural change. It
    is guarded rather than assumed because it is only implemented for some
    combinations of device and dtype.
    """
    options = dict(lr=args.lr, weight_decay=args.weight_decay)
    if device.type == "cuda":
        try:
            return torch.optim.AdamW(model.parameters(), fused=True, **options), "fused"
        except (RuntimeError, ValueError):
            pass
    return torch.optim.AdamW(model.parameters(), **options), "default"


def forever(loader):
    """Batches without end, re-entering the loader when it runs dry.

    A virtual epoch is a position count, so its boundary usually falls in the
    middle of a pass over the file. Restarting the loader here rather than in
    the epoch loop is what keeps persistent workers alive across that boundary
    instead of respawning them - which on Windows means re-importing torch in
    every worker.
    """
    while True:
        yield from loader


def load_resume(args, model, optimiser, scheduler, device):
    """Put a run back exactly where it stopped. Returns (next epoch, history)."""
    net_path, state_path = f"{args.out}.pt", f"{args.out}{RESUME_SUFFIX}"
    for path in (net_path, state_path):
        if not os.path.exists(path):
            raise SystemExit(f"--resume: {path} does not exist. A run can only be resumed "
                             f"from its own --out prefix, and only if it completed one "
                             f"epoch.")

    checkpoint = torch.load(net_path, map_location=device, weights_only=False)
    state = torch.load(state_path, map_location=device, weights_only=False)

    # Same rule as everywhere else the shape is read rather than assumed: the
    # architecture comes out of the file, and a disagreement names the field.
    # Weights loaded under a shape that merely happens to fit train fine and
    # are wrong, which is the failure mode worth being loud about.
    arch = arch_from_checkpoint(checkpoint)
    for field, asked in (("hidden", args.hidden), ("output_buckets", args.output_buckets)):
        if arch[field] != asked:
            raise SystemExit(f"--resume: {net_path} has {field} {arch[field]}, this run "
                             f"asks for {asked}. Pass the flags the original run used, or "
                             f"--out somewhere else.")

    model.load_state_dict(checkpoint["model"])
    optimiser.load_state_dict(state["optimiser"])
    scheduler.load_state_dict(state["scheduler"])
    torch.set_rng_state(state["torch_rng"].cpu())
    if state.get("cuda_rng") is not None and torch.cuda.is_available():
        torch.cuda.set_rng_state_all([s.cpu() for s in state["cuda_rng"]])

    history = []
    history_path = f"{args.out}-history.json"
    if os.path.exists(history_path):
        with open(history_path, encoding="utf-8") as f:
            history = json.load(f)
    done = int(state["epoch"])
    history = [row for row in history if row["epoch"] <= done]

    print(f"resumed from {net_path} at epoch {done} "
          f"(train {checkpoint.get('train_loss', float('nan')):.6f})")
    return done + 1, history


def evaluate(model, loader, device, sigmoid_k, lam, limit=None) -> float:
    model.eval()
    # Accumulated on the device: see the note in train() on why a .item() per
    # batch is not free.
    total, seen = torch.zeros((), device=device), 0
    with torch.no_grad():
        for i, batch in enumerate(loader):
            if limit and i >= limit:
                break
            batch = to_device(batch, device)
            prediction = model(batch["white"], batch["black"], batch["stm"],
                               batch["piece_count"])
            target = blended_target(batch["score"], batch["wdl"], lam, sigmoid_k)
            n = prediction.numel()
            total += loss_fn(prediction, target, sigmoid_k) * n
            seen += n
    model.train()
    return total.item() / max(seen, 1)


def train(args) -> None:
    torch.manual_seed(args.seed)

    device = torch.device(args.device or ("cuda" if torch.cuda.is_available() else "cpu"))
    print(f"device: {device}"
          f"{' (' + torch.cuda.get_device_name(0) + ')' if device.type == 'cuda' else ''}")

    model = NNUE(hidden=args.hidden, output_buckets=args.output_buckets).to(device)
    print(f"net:    {model.describe()}")

    train_loader = make_loader(args.train, args.batch_size, args.workers, shuffle=True,
                               sources=args.sources, chunk_records=args.chunk_records,
                               seed=args.seed)
    val_loader = (make_loader(args.val, args.batch_size, max(args.workers // 2, 0),
                              shuffle=False, sources=args.sources,
                              chunk_records=args.chunk_records, seed=args.seed)
                  if args.val else None)

    print(f"train: {train_loader.dataset.records:,} records "
          f"in {len(train_loader):,} batches of {args.batch_size:,}")
    print(f"load:  {train_loader.dataset.describe()}, {args.workers} worker(s)")
    if val_loader:
        print(f"val:   {val_loader.dataset.records:,} records")

    optimiser, flavour = make_optimiser(model, args, device)
    scheduler = torch.optim.lr_scheduler.ExponentialLR(optimiser, gamma=args.lr_gamma)
    print(f"optim: AdamW ({flavour})")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)

    first_epoch, history = 1, []
    if args.resume:
        first_epoch, history = load_resume(args, model, optimiser, scheduler, device)
        if first_epoch > args.epochs:
            print(f"nothing to do: already at epoch {first_epoch - 1} of {args.epochs}")
            return
        # The chunked loader shuffles from an epoch counter it owns, so it has
        # to be told which pass this is or the resumed run replays epoch 1.
        set_epoch(train_loader, first_epoch - 1)

    # An epoch is one pass over the data, and the pass ENDING is what ends it -
    # not a batch count, because with --sources len(loader) is an upper bound
    # and counting to it would spill a little further into the data every
    # epoch. --positions-per-epoch is the other case: there an epoch is a
    # position count, passes and epochs stop lining up, and the loader has to
    # be re-entered across the boundary.
    endless = forever(train_loader) if args.positions_per_epoch else None
    planned = (math.ceil(args.positions_per_epoch / args.batch_size)
               if args.positions_per_epoch else len(train_loader))
    if args.limit_batches:
        planned = min(planned, args.limit_batches)

    for epoch in range(first_epoch, args.epochs + 1):
        # Anneal lambda toward the game result: early epochs distil the search,
        # later ones let the outcome pull the net off the teacher's systematic
        # errors. Both ends are hyperparameters worth two or three runs.
        span = max(args.epochs - 1, 1)
        lam = args.lambda_start + (args.lambda_end - args.lambda_start) * (epoch - 1) / span

        started = time.time()
        # The running loss stays a device tensor rather than being read back
        # every step with .item(). Do not oversell this: measured at well under
        # 1% on a 1024-wide net on a 3070, because a saturated GPU makes the
        # sync nearly free - it waits for work that had to finish anyway. It
        # stops being free exactly where the GPU is NOT saturated (--hidden
        # 512, a bigger batch, a faster card), and it costs nothing to avoid.
        running = torch.zeros((), device=device)
        seen, batches = 0, 0
        arriving = endless if endless is not None else iter(train_loader)

        while True:
            if args.limit_batches and batches >= args.limit_batches:
                break
            if args.positions_per_epoch and seen >= args.positions_per_epoch:
                break
            try:
                batch = next(arriving)
            except StopIteration:
                break  # a plain epoch: the pass over the data ended
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
            running += loss.detach() * n
            seen += n
            batches += 1

            if args.log_every and batches % args.log_every == 0:
                # The only read-back in the loop, and so the only place the CPU
                # waits for the GPU.
                elapsed = max(time.time() - started, 1e-6)
                rate = seen / elapsed
                left = (planned - batches) * elapsed / batches
                print(f"  epoch {epoch} batch {batches:,}/{planned:,} "
                      f"loss {running.item() / seen:.6f}  {rate:,.0f} pos/s  "
                      f"eta {left / 60:.1f}m", flush=True)

        scheduler.step()
        elapsed = time.time() - started
        train_loss = running.item() / max(seen, 1)
        val_loss = (evaluate(model, val_loader, device, args.sigmoid_k, lam,
                             args.limit_batches) if val_loader else float("nan"))

        print(f"epoch {epoch:>3}  lambda {lam:.2f}  lr {scheduler.get_last_lr()[0]:.2e}  "
              f"train {train_loss:.6f}  val {val_loss:.6f}  "
              f"{seen / max(elapsed, 1e-6):,.0f} pos/s  {elapsed:.0f}s", flush=True)

        history.append({"epoch": epoch, "lambda": lam, "train": train_loss,
                        "val": val_loss, "positions": seen})

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
        save_atomically(checkpoint, f"{args.out}.pt")
        if args.checkpoint_every and epoch % args.checkpoint_every == 0:
            save_atomically(checkpoint, f"{args.out}-epoch{epoch}.pt")

        if not args.no_resume_state:
            save_atomically({
                "epoch": epoch,
                "arch": model.arch,
                "optimiser": optimiser.state_dict(),
                "scheduler": scheduler.state_dict(),
                "torch_rng": torch.get_rng_state(),
                "cuda_rng": (torch.cuda.get_rng_state_all()
                             if torch.cuda.is_available() else None),
            }, f"{args.out}{RESUME_SUFFIX}")

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

    parser.add_argument("--positions-per-epoch", type=count, default=0,
                        help="make an epoch this many positions rather than a whole pass, "
                             "so a 500M-position run checkpoints and prints its sanity "
                             "table more than once a day. Accepts 50M / 2B. NOTE that the "
                             "LR decay and the lambda anneal are per EPOCH, so shorter "
                             "epochs mean more of both across the same data")
    parser.add_argument("--resume", action="store_true",
                        help="continue the run at --out from its last completed epoch, "
                             "restoring optimiser, scheduler and RNG state")
    parser.add_argument("--no-resume-state", action="store_true",
                        help=f"do not write {{out}}{RESUME_SUFFIX}. It is about twice the "
                             f"size of the net, so this is worth it only for short runs "
                             f"you would rather restart than resume")

    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--chunk-records", type=count, default=None,
                        help="records per sequential read. Default decides from the "
                             "dataset's size: memmap slices below ~2 GB, 2M-record chunks "
                             "above it. 0 forces memmap slices at any size")
    parser.add_argument("--seed", type=int, default=0,
                        help="seeds shuffling and initialisation, so a run repeats")
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
