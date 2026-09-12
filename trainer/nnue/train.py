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

The architecture is SCReLU over 32 mirrored king squares, and the only things
left to choose are shape: --hidden, --output-buckets, and --l1-size/--l2-size
for the layer stack that can replace the flat output layer. All four are header
fields the engine reads out of the net file, so changing any of them is a
retrain and an export, with no C change and no flag to keep in sync.

--l1-size 0 is the architecture that shipped and is the default, so a run that
names none of this computes exactly what it computed before. Task 6 in
docs/NNUE.md is what the stack is for and how it is quantised; a stacked
checkpoint exports and runs like any other, at qa 256 and qb 512 rather than
255 and 64, which tools/export_net.py picks from the checkpoint itself.

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
import uuid

import torch

from . import sanity
from .dataset import make_loader, set_epoch, to_device
from .format import DEFAULT_OUTPUT_BUCKETS, NET_TO_CP, SOURCE_NAMES
from .model import (DEFAULT_HIDDEN, DEFAULT_L1_SIZE, DEFAULT_L2_SIZE, NNUE, TargetPolicy,
                    arch_from_checkpoint, blended_target, from_checkpoint, loss_fn,
                    uncertainty_loss_fn)
from .provenance import code_identity, dataset_identity, sha256_file, write_json

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


def parse_source_map(text, flag: str) -> dict:
    """``selfplay=0.9,tree=1.0`` into ``{0: 0.9, 1: 1.0}``.

    Sources are named rather than numbered because ``--lambda-source 1=1.0``
    is a number nobody can check against the shard's stats output, and the
    tags are already spelled out in ``datagen stats``. An unknown name is
    fatal: a typo that silently applied to nothing would leave a run doing
    something other than what its command line says, which is the whole class
    of bug this policy exists to make visible.
    """
    if not text:
        return {}
    out = {}
    for item in str(text).replace(";", ",").split(","):
        item = item.strip()
        if not item:
            continue
        name, _, value = item.partition("=")
        name = name.strip().lower()
        if name not in SOURCE_NAMES:
            raise SystemExit(f"{flag}: '{name}' is not a source tag. "
                             f"One of: {', '.join(SOURCE_NAMES)}")
        try:
            out[SOURCE_NAMES.index(name)] = float(value)
        except ValueError:
            raise SystemExit(f"{flag}: '{item}' is not NAME=NUMBER") from None
    return out


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
        yielded = False
        for batch in loader:
            yielded = True
            yield batch
        if not yielded:
            raise ValueError("no training records remain after source filtering")


def load_resume(args, model, optimiser, scheduler, device):
    """Restore saved training state; returns (next epoch, history, checkpoint).

    Loader prefetch/cursor state is not checkpointed. A resumed virtual epoch
    starts a fresh shuffled pass, not the exact next batch of the old iterator.
    """
    net_path, state_path = f"{args.out}.pt", f"{args.out}{RESUME_SUFFIX}"
    for path in (net_path, state_path):
        if not os.path.exists(path):
            raise SystemExit(f"--resume: {path} does not exist. A run can only be resumed "
                             f"from its own --out prefix, and only if it completed one "
                             f"epoch.")

    checkpoint = torch.load(net_path, map_location=device, weights_only=False)
    state = torch.load(state_path, map_location=device, weights_only=False)
    if state["epoch"] != checkpoint["epoch"] or state.get("run_id") != checkpoint.get("run_id"):
        raise SystemExit("--resume: checkpoint and optimiser belong to different saves")
    if state.get("checkpoint_sha256") and state["checkpoint_sha256"] != sha256_file(net_path):
        raise SystemExit("--resume: checkpoint hash does not match the optimiser state")
    if checkpoint.get("run_id"):
        # Changing the main-stage length moves the lambda schedule and the
        # finishing boundary. Fine-tuning is --init-from, not an ambiguous resume.
        fixed = ("epochs", "finish_epochs", "finish_lr", "positions_per_epoch",
                 "lr", "lr_gamma", "weight_decay", "batch_size", "seed", "workers",
                 "chunk_records", "limit_batches", "sigmoid_k", "score_clip", "unc_weight",
                 "lambda_start", "lambda_end", "lambda_progress", "lambda_pieces",
                 "lambda_source", "lambda_min", "lambda_max", "source_weight", "sources",
                 "no_weight_clip")
        for key in fixed:
            if checkpoint["args"].get(key) != getattr(args, key):
                raise SystemExit(f"--resume: {key} changed; use --init-from for a new recipe")
        if checkpoint.get("data") != {"train": dataset_identity(args.train),
                                      "val": dataset_identity(args.val)}:
            raise SystemExit("--resume: training/validation data identity changed")

    # Same rule as everywhere else the shape is read rather than assumed: the
    # architecture comes out of the file, and a disagreement names the field.
    # Weights loaded under a shape that merely happens to fit train fine and
    # are wrong, which is the failure mode worth being loud about.
    arch = arch_from_checkpoint(checkpoint)
    for field, asked in (("hidden", args.hidden), ("output_buckets", args.output_buckets),
                         ("uncertainty", args.uncertainty),
                         ("feature_factorization", args.feature_factorization),
                         ("l1_size", args.l1_size), ("l2_size", args.l2_size)):
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
    return done + 1, history, checkpoint


class Metrics:
    """Global denominators: averaging weighted batch means biases validation.

    score_mse and wdl_mse use a fixed K=400 with raw labels, no mixture weights
    or lambda schedule. They are diagnostic comparisons, not training targets.
    """

    def __init__(self, device, unc_weight):
        self.sums = torch.zeros(7, device=device, dtype=torch.float64)
        self.unc_weight = unc_weight

    @torch.no_grad()
    def update(self, prediction, unc, batch, target, policy):
        probability = torch.sigmoid(prediction.detach() * NET_TO_CP / policy.sigmoid_k)
        squared = (probability - target) ** 2
        weights = policy.weights(batch)
        n = prediction.numel()
        mass = squared.new_tensor(n) if weights is None else weights.sum()
        value_sum = squared.sum() if weights is None else (squared * weights).sum()
        unc_sum = (squared.new_zeros(()) if unc is None else
                   (unc.detach() - (policy.score(batch) / NET_TO_CP
                                    - prediction.detach()).abs()).abs().sum())
        fixed = torch.sigmoid(prediction.detach() * NET_TO_CP / DEFAULT_SIGMOID_K)
        score_sum = ((fixed - torch.sigmoid(batch["score"] / DEFAULT_SIGMOID_K)) ** 2).sum()
        known = batch["wdl"] <= 2
        result = batch["wdl"].float().clamp_max(2) / 2
        wdl_sum = (((fixed - result) ** 2) * known).sum()
        self.sums += torch.stack((value_sum, mass, unc_sum, squared.new_tensor(n),
                                  score_sum, wdl_sum, known.sum()))

    def report(self):
        value, mass, unc, n, score, wdl, known = self.sums.tolist()
        if n == 0:
            raise ValueError("no records remain after source filtering")
        if not all(math.isfinite(x) for x in (value, mass, unc, n, score, wdl, known)):
            raise ValueError("non-finite training/validation metrics; refusing to save a bad net")
        value /= max(mass, 1e-8)
        unc /= n
        return {"value": value, "uncertainty": unc,
                "weighted_uncertainty": self.unc_weight * unc,
                "total": value + self.unc_weight * unc,
                "score_mse": score / n, "wdl_mse": wdl / known if known else None,
                "positions": int(n), "known_results": int(known)}


def evaluate(model, loader, device, policy, lam, limit=None, unc_weight=0.05) -> dict:
    """Held-out loss under the SAME target policy the training loop used.

    Scoring the validation set against a different target would make the two
    curves incomparable, which is the only thing a validation curve is for.
    """
    was_training = model.training
    model.eval()
    # Accumulated on the device: see the note in train() on why a .item() per
    # batch is not free.
    metrics = Metrics(device, unc_weight)
    with torch.no_grad():
        for i, batch in enumerate(loader):
            if limit and i >= limit:
                break
            batch = to_device(batch, device)
            if model.uncertainty:
                prediction, unc = model.forward_heads(batch["white"], batch["black"],
                                                      batch["stm"], batch["piece_count"])
            else:
                prediction = model(batch["white"], batch["black"], batch["stm"],
                                   batch["piece_count"])
                unc = None
            target = policy.target(batch, lam)
            metrics.update(prediction, unc, batch, target, policy)
    model.train(was_training)
    return metrics.report()


def train(args) -> None:
    existing = [f"{args.out}{suffix}" for suffix in (".pt", RESUME_SUFFIX, "-history.json",
                                                   "-run.json", "-pre-finish.pt", ".nnue", ".json")]
    if not args.resume and any(os.path.exists(p) for p in existing):
        raise SystemExit("output prefix already belongs to a run; choose a new --out or --resume")
    torch.manual_seed(args.seed)

    device = torch.device(args.device or ("cuda" if torch.cuda.is_available() else "cpu"))
    print(f"device: {device}"
          f"{' (' + torch.cuda.get_device_name(0) + ')' if device.type == 'cuda' else ''}")

    parent = None
    if args.init_from:
        source_path = os.path.abspath(args.init_from)
        source_hash = sha256_file(source_path)
        source = torch.load(source_path, map_location="cpu", weights_only=False)
        if sha256_file(source_path) != source_hash:
            raise SystemExit("--init-from checkpoint changed while being loaded")
        original = from_checkpoint(source)
        args.hidden, args.output_buckets = original.hidden, original.output_buckets
        args.uncertainty = original.uncertainty
        args.l1_size, args.l2_size = original.l1_size, original.l2_size
        args.feature_factorization = args.feature_factorization or original.feature_factorization
        model = NNUE(hidden=args.hidden, output_buckets=args.output_buckets,
                     uncertainty=args.uncertainty,
                     feature_factorization=args.feature_factorization,
                     l1_size=args.l1_size, l2_size=args.l2_size)
        weights = original.state_dict()
        if args.feature_factorization and not original.feature_factorization:
            weights["ft_shared.weight"] = model.ft_shared.weight.detach().clone()
        model.load_state_dict(weights)
        parent = {"checkpoint": source_path, "sha256": source_hash,
                  "run_id": source.get("run_id")}
        del original, source, weights
    else:
        model = NNUE(hidden=args.hidden, output_buckets=args.output_buckets,
                     uncertainty=args.uncertainty,
                     feature_factorization=args.feature_factorization,
                     l1_size=args.l1_size, l2_size=args.l2_size)
    model = model.to(device)
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
    if args.sources and hasattr(train_loader.dataset, "chunks"):
        print("       chunked source filtering: input counts above are upper bounds; "
              "epoch metrics report the records actually used")
    if val_loader is not None:
        print(f"val:   {val_loader.dataset.records:,} records")

    optimiser, flavour = make_optimiser(model, args, device)
    scheduler = torch.optim.lr_scheduler.ExponentialLR(optimiser, gamma=args.lr_gamma)
    print(f"optim: AdamW ({flavour})")

    policy = TargetPolicy(
        sigmoid_k=args.sigmoid_k,
        score_clip=args.score_clip,
        progress_delta=args.lambda_progress,
        pieces_delta=args.lambda_pieces,
        source_lambda=parse_source_map(args.lambda_source, "--lambda-source"),
        source_weight=parse_source_map(args.source_weight, "--source-weight"),
        lambda_min=args.lambda_min,
        lambda_max=args.lambda_max,
    ).to(device)
    print(f"target: {policy.describe()}")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)

    first_epoch, history = 1, []
    run_id = uuid.uuid4().hex
    data = {"train": dataset_identity(args.train), "val": dataset_identity(args.val)}
    provenance = {"code": code_identity(), "torch": str(torch.__version__),
                  "device": str(device), "parent": parent}
    if args.resume:
        first_epoch, history, restored = load_resume(args, model, optimiser, scheduler, device)
        run_id = restored.get("run_id", run_id)
        current_environment = provenance
        provenance = restored.get("provenance", provenance)
        if first_epoch > args.epochs + args.finish_epochs:
            print(f"nothing to do: already at epoch {first_epoch - 1} "
                  f"of {args.epochs + args.finish_epochs}")
            return
        provenance.setdefault("resumes", []).append({
            "after_epoch": first_epoch - 1, "code": current_environment["code"],
            "torch": current_environment["torch"], "device": current_environment["device"]})
        # The chunked loader shuffles from an epoch counter it owns, so it has
        # to be told which pass this is or the resumed run replays epoch 1.
        set_epoch(train_loader, first_epoch - 1)
        if args.positions_per_epoch and first_epoch <= args.epochs:
            print("resume: virtual-epoch loader restarts from a fresh shuffled pass; "
                  "prefetched batches and the mid-pass cursor are not checkpointed")
        del restored
    write_json(f"{args.out}-run.json", {"run_id": run_id, "arch": model.arch,
                                       "args": vars(args), "data": data,
                                       "provenance": provenance})
    print(f"run:    {run_id}  ({os.path.abspath(args.out)})")
    if args.init_from:
        print("init:   architecture/weights inherited; optimizer is new. Target and loss "
              "settings come from this command, not the parent checkpoint")
    if args.finish_epochs:
        print(f"finish: {args.finish_epochs} full pass(es) at lr {args.finish_lr:g}; "
              "lambda held at --lambda-end, Adam moments retained")
        if first_epoch == 1 and args.epochs == 0:
            save_atomically({"model": model.state_dict(), "arch": model.arch,
                             "hidden": args.hidden, "epoch": 0, "stage": "pre-finish",
                             "run_id": run_id, "args": vars(args), "data": data,
                             "provenance": provenance}, f"{args.out}-pre-finish.pt")

    # An epoch is one pass over the data, and the pass ENDING is what ends it -
    # not a batch count, because with --sources len(loader) is an upper bound
    # and counting to it would spill a little further into the data every
    # epoch. --positions-per-epoch is the other case: there an epoch is a
    # position count, passes and epochs stop lining up, and the loader has to
    # be re-entered across the boundary.
    endless = (forever(train_loader)
               if args.positions_per_epoch and first_epoch <= args.epochs else None)

    for epoch in range(first_epoch, args.epochs + args.finish_epochs + 1):
        finishing = epoch > args.epochs
        stage = "finish" if finishing else "main"
        if finishing:
            # Do not let the main exponential scheduler overwrite the final LR.
            for group in optimiser.param_groups:
                group["lr"] = args.finish_lr
            if endless is not None:
                # A finish is a WHOLE new pass, even after partial virtual epochs.
                endless = None
                del arriving
                train_loader = make_loader(args.train, args.batch_size, args.workers,
                                           shuffle=True, sources=args.sources,
                                           chunk_records=args.chunk_records, seed=args.seed)
                set_epoch(train_loader, epoch - 1)
        position_limit = 0 if finishing else args.positions_per_epoch
        planned = (math.ceil(position_limit / args.batch_size)
                   if position_limit else len(train_loader))
        if args.limit_batches:
            planned = min(planned, args.limit_batches)
        used_lr = optimiser.param_groups[0]["lr"]
        # Anneal lambda toward the game result: early epochs distil the search,
        # later ones let the outcome pull the net off the teacher's systematic
        # errors. Both ends are hyperparameters worth two or three runs.
        span = max(args.epochs - 1, 1)
        lam = (args.lambda_end if finishing else
               args.lambda_start + (args.lambda_end - args.lambda_start) * (epoch - 1) / span)

        started = time.time()
        # The running loss stays a device tensor rather than being read back
        # every step with .item(). Do not oversell this: measured at well under
        # 1% on a 1024-wide net on a 3070, because a saturated GPU makes the
        # sync nearly free - it waits for work that had to finish anyway. It
        # stops being free exactly where the GPU is NOT saturated (--hidden
        # 512, a bigger batch, a faster card), and it costs nothing to avoid.
        running = torch.zeros((), device=device)
        metrics = Metrics(device, args.unc_weight)
        # The lambda that was actually applied, averaged over the epoch. With a
        # schedule this is the only number that says what the run did: the flags
        # say what was asked for, and the mixture decides what that came out as.
        lam_sum = torch.zeros((), device=device)
        seen, batches = 0, 0
        arriving = endless if endless is not None else iter(train_loader)

        while True:
            if args.limit_batches and batches >= args.limit_batches:
                break
            if position_limit and seen >= position_limit:
                break
            try:
                batch = next(arriving)
            except StopIteration:
                break  # a plain epoch: the pass over the data ended
            batch = to_device(batch, device)

            lambdas = policy.lambdas(batch, lam)
            score = policy.score(batch)
            target = blended_target(score, batch["wdl"], lambdas, args.sigmoid_k)
            weight = policy.weights(batch)
            if args.uncertainty:
                prediction, unc = model.forward_heads(batch["white"], batch["black"],
                                                      batch["stm"], batch["piece_count"])
                # The clipped score, not the raw one: the value head is trained
                # toward the clipped label, so its residual has to be measured
                # against that same label or the head learns to predict an error
                # the value head was never asked to avoid.
                loss = loss_fn(prediction, target, args.sigmoid_k, weight) \
                    + args.unc_weight * uncertainty_loss_fn(unc, prediction, score)
            else:
                unc = None
                prediction = model(batch["white"], batch["black"], batch["stm"],
                                   batch["piece_count"])
                loss = loss_fn(prediction, target, args.sigmoid_k, weight)

            optimiser.zero_grad(set_to_none=True)
            loss.backward()
            optimiser.step()

            # After the step, every step. This is what makes the exported net
            # representable rather than merely hopefully-representable: see
            # WEIGHT_CLIP in format.py for the bound and why it is that number.
            if not args.no_weight_clip:
                model.clip_weights()

            n = prediction.numel()
            metrics.update(prediction, unc, batch, target, policy)
            running += loss.detach() * n
            lam_sum += torch.where(batch["wdl"] <= 2, lambdas,
                                   torch.ones_like(lambdas)).detach().sum()
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

        elapsed = time.time() - started
        train_metrics = metrics.report()
        if not finishing:
            scheduler.step()
        train_loss = train_metrics["value"]
        applied = lam_sum.item() / max(seen, 1)
        val_metrics = (evaluate(model, val_loader, device, policy, lam,
                                args.limit_batches, args.unc_weight)
                       if val_loader is not None else None)
        val_loss = val_metrics["value"] if val_metrics else None

        # Both lambdas: the one the schedule asked for, and the one the data
        # actually got. They differ by whatever the deltas and the overrides
        # did, and a run whose applied lambda is not where it was meant to be
        # is a flag typo that nothing else would report.
        applied_note = "" if abs(applied - lam) < 5e-4 else f" (applied {applied:.3f})"
        val_text = f"{val_loss:.6f}" if val_loss is not None else "none"
        print(f"epoch {epoch:>3} [{stage}]  lambda {lam:.2f}{applied_note}  "
              f"lr {used_lr:.2e}  "
              f"train value {train_loss:.6f}  val value {val_text}  "
              f"{seen / max(elapsed, 1e-6):,.0f} pos/s  {elapsed:.0f}s", flush=True)
        for label, values in (("train", train_metrics), ("val", val_metrics)):
            if values is not None:
                wdl_text = (f"{values['wdl_mse']:.6f}" if values["wdl_mse"] is not None
                            else "none")
                print(f"  {label}: unc L1 {values['uncertainty']:.6f}  "
                      f"weighted unc {values['weighted_uncertainty']:.6f}  "
                      f"total {values['total']:.6f}  fixed score MSE {values['score_mse']:.6f}  "
                      f"fixed WDL MSE {wdl_text}")

        history.append({"epoch": epoch, "stage": stage, "lr": used_lr,
                        "lambda": lam, "lambda_applied": applied, "metrics_version": 2,
                        "train": train_loss, "val": val_loss, "positions": seen,
                        "metrics": {"train": train_metrics, "val": val_metrics}})

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
            "stage": stage,
            "run_id": run_id,
            "data": data,
            "provenance": provenance,
            "metrics_version": 2,
            "metrics": {"train": train_metrics, "val": val_metrics},
            "sigmoid_k": args.sigmoid_k,
            "train_loss": train_loss,
            "val_loss": val_loss,
            "args": vars(args),
        }
        save_atomically(checkpoint, f"{args.out}.pt")
        checkpoint_hash = sha256_file(f"{args.out}.pt")
        if args.finish_epochs and epoch == args.epochs:
            save_atomically(checkpoint, f"{args.out}-pre-finish.pt")
        if args.checkpoint_every and epoch % args.checkpoint_every == 0:
            save_atomically(checkpoint, f"{args.out}-epoch{epoch}.pt")

        if not args.no_resume_state:
            save_atomically({
                "epoch": epoch,
                "run_id": run_id,
                "checkpoint_sha256": checkpoint_hash,
                "arch": model.arch,
                "optimiser": optimiser.state_dict(),
                "scheduler": scheduler.state_dict(),
                "torch_rng": torch.get_rng_state(),
                "cuda_rng": (torch.cuda.get_rng_state_all()
                             if torch.cuda.is_available() else None),
            }, f"{args.out}{RESUME_SUFFIX}")

        write_json(f"{args.out}-history.json", history)
        write_json(f"{args.out}-run.json", {"run_id": run_id, "arch": model.arch,
                   "args": vars(args), "data": data, "provenance": provenance,
                   "checkpoint": os.path.abspath(f"{args.out}.pt"),
                   "checkpoint_sha256": checkpoint_hash, "epoch": epoch, "stage": stage})

    print(f"wrote {args.out}.pt")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--train", nargs="+", required=True, help="shard(s) to fit")
    parser.add_argument("--val", nargs="*", default=None, help="held-out shard(s)")
    parser.add_argument("--out", default="external/nets/net", help="checkpoint prefix")

    parser.add_argument("--epochs", type=int, default=10)
    parser.add_argument("--finish-epochs", type=int, default=0,
                        help="extra full data passes at --finish-lr after the main epochs; "
                             "0 disables. Unaffected by --positions-per-epoch")
    parser.add_argument("--finish-lr", type=float, default=1e-5,
                        help="constant LR for the finishing passes; retains Adam moments "
                             "and holds the target at --lambda-end (default: 1e-5)")
    parser.add_argument("--batch-size", type=int, default=16384)
    parser.add_argument("--hidden", type=int, default=DEFAULT_HIDDEN,
                        help="hidden width per perspective; a multiple of 16")
    parser.add_argument("--l1-size", type=int, default=DEFAULT_L1_SIZE,
                        help="width of L1, the first layer of the stack that replaces the "
                             "flat output layer. 0 (the default) is the architecture that "
                             "shipped, and is the control every stacked net is measured "
                             "against; otherwise a multiple of 16. See Task 6 in "
                             "docs/NNUE.md")
    parser.add_argument("--l2-size", type=int, default=DEFAULT_L2_SIZE,
                        help="width of L2. 0 runs L1 straight into the output layer. "
                             "Requires --l1-size, and is a multiple of 16")
    parser.add_argument("--output-buckets", type=int, default=DEFAULT_OUTPUT_BUCKETS,
                        help="output rows, selected by piece count; must divide 32")
    parser.add_argument("--feature-factorization", action="store_true",
                        help="train shared piece-square embeddings alongside the king-specific "
                             "transformer; folded at export with no engine inference cost")
    parser.add_argument("--uncertainty", action="store_true",
                        help="add the uncertainty head: a second output layer predicting "
                             "|search score - value| per bucket, which search.c uses to "
                             "scale its pruning margins (docs/NNUE.md Task 5b). A header "
                             "flag the engine reads from the net file; older nets and "
                             "older engines are both unaffected")
    parser.add_argument("--unc-weight", type=float, default=0.05,
                        help="weight of the uncertainty head's L1 term in the total loss. "
                             "A hyperparameter, not a fitted value: the value loss is MSE "
                             "in win-probability space and the head's is L1 in net units, "
                             "so their natural scales differ by roughly this factor")
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

    # The per-record lambda dials. Every one of them defaults to no effect, so
    # a run that names none computes exactly what a single-lambda run computed.
    # See TargetPolicy in model.py for why one lambda over a whole dataset is
    # paying the average of prices that are not the same.
    parser.add_argument("--lambda-progress", type=float, default=0.0, metavar="DELTA",
                        help="shift lambda by this much at the END of a game, tapering to "
                             "zero 112+ plies away from it. NEGATIVE trusts the game "
                             "result more where it is nearly certain and the search less: "
                             "-0.2 with --lambda-start 0.95 runs 0.95 in the opening and "
                             "0.75 on the last few plies. Needs the game-progress nibble, "
                             "which datagen writes for selfplay shards and cannot write "
                             "for `label` ones - those keep the base lambda")
    parser.add_argument("--lambda-pieces", type=float, default=0.0, metavar="DELTA",
                        help="shift lambda by this much at bare kings, tapering to zero at "
                             "a full board. The other half of the same idea: the search "
                             "score's SYSTEMATIC errors - fortresses, compensation, an "
                             "ending it cannot convert - are what the result term corrects, "
                             "and they live in the endgame")
    parser.add_argument("--lambda-source", default=None, metavar="NAME=V,...",
                        help="override lambda outright for a source tag, e.g. "
                             "'human=1.0'. A human or engine record's result is real, but "
                             "it is someone else's continuation - whether that is worth "
                             "the same as our own is a measurement, not a discount")
    parser.add_argument("--lambda-min", type=float, default=0.0,
                        help="floor for the deltas above; overrides are not clipped")
    parser.add_argument("--lambda-max", type=float, default=1.0,
                        help="ceiling for the deltas above")
    parser.add_argument("--score-clip", type=int, default=0, metavar="CP",
                        help="clamp |score| to this many centipawns before the sigmoid. A "
                             "mate or tablebase score is a proven result rather than an "
                             "evaluation, and sigmoid(31500/400) is 1.0 to every digit a "
                             "float carries - a target no clipped-weight net can reach. "
                             "`datagen stats` prints what share of a shard is decisive. "
                             "0 disables")
    parser.add_argument("--source-weight", default=None, metavar="NAME=W,...",
                        help="per-record loss weight by source, e.g. 'tree=0.5'. Sets the "
                             "mixture without regenerating anything, where --sources can "
                             "only drop a source entirely")

    parser.add_argument("--positions-per-epoch", type=count, default=0,
                        help="make an epoch this many positions rather than a whole pass, "
                             "so a 500M-position run checkpoints and prints its sanity "
                             "table more than once a day. Accepts 50M / 2B. NOTE that the "
                             "LR decay and the lambda anneal are per EPOCH, so shorter "
                             "epochs mean more of both across the same data")
    parser.add_argument("--resume", action="store_true",
                        help="continue the run at --out from its last completed epoch, "
                             "restoring optimiser, scheduler and RNG state")
    parser.add_argument("--init-from", default=None, metavar="CHECKPOINT",
                        help="start a NEW run from checkpoint weights, with a fresh optimizer. "
                             "Reads architecture from the file; --feature-factorization may "
                             "add a zero shared factor to an unfactorized checkpoint. "
                             "Use --epochs 0 --finish-epochs 1 for a finishing-only run")
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
    args = parser.parse_args(argv)
    if args.resume and args.init_from:
        parser.error("--resume and --init-from are mutually exclusive")
    if args.epochs < 0 or args.finish_epochs < 0 or args.epochs + args.finish_epochs < 1:
        parser.error("need a positive number of main or finishing epochs")
    if args.epochs == 0 and not (args.init_from or args.resume):
        parser.error("--epochs 0 needs --init-from (or --resume for that run)")
    for name in ("lr", "lr_gamma", "finish_lr", "sigmoid_k"):
        if not math.isfinite(getattr(args, name)) or getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive and finite")
    if args.batch_size < 1 or args.workers < 0:
        parser.error("--batch-size must be positive and --workers non-negative")
    for name in ("positions_per_epoch", "limit_batches", "checkpoint_every"):
        if getattr(args, name) < 0:
            parser.error(f"--{name.replace('_', '-')} must be non-negative")
    if args.chunk_records is not None and args.chunk_records < 0:
        parser.error("--chunk-records must be non-negative")
    if args.sources and any(s < 0 or s >= len(SOURCE_NAMES) for s in args.sources):
        parser.error(f"--sources must be in 0..{len(SOURCE_NAMES) - 1}")
    return args


def main() -> None:
    train(parse_args())


if __name__ == "__main__":
    # Required on Windows: the DataLoader spawns worker processes, which
    # re-import this module.
    main()
