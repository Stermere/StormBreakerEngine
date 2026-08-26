"""Feeding the GPU.

Plain PyTorch is 5-20x slower than a purpose-built NNUE trainer and almost all
of the difference is the input pipeline rather than the model. Three things get
most of it back, and all three are here:

  * the records are fixed-size, so a batch is a contiguous SLICE and the
    tensors come out of whole-array arithmetic - there is no per-record Python;
  * a Dataset item is a whole BATCH rather than one record, so the DataLoader's
    collate step and its per-item overhead are paid once per batch instead of
    sixteen thousand times;
  * feature indices are int32. They run to 24576, so this is the same numbers
    in half the bytes - and those bytes are memset in a worker, copied into
    pinned memory and pushed over PCIe, twice per batch.

WHICH LOADER RUNS IS A PROPERTY OF THE DATASET'S SIZE:

  * up to AUTO_CHUNK_BYTES, ``ShardBatches``. A batch is a memmap slice, and
    shuffling the batch ORDER is all the shuffling there is to do.
  * beyond it, ``ShuffledChunks``. The file is read in 64 MB sequential chunks
    and the records are shuffled inside each one.

What the chunked path is and is not worth, measured on a 16.3 GB / 509M-record
set on an NVMe (docs/EXPERIMENTS.md, E13). Read this before assuming it is the
thing to reach for when a run is slow:

  * it is NOT rescuing a collapsing read pattern, because there was not one.
    The memmap path still managed 1.11M positions/s at 16.3 GB - 1.2M
    positions/s is only 38 MB/s of records, so this loader is CPU-bound in
    ``unpack`` and a drive that can seek does not care how scattered the reads
    are. On a spinning disk, or a dataset far past RAM, that would differ.
  * it IS worth ~11% of loader throughput and roughly half the time to reach
    steady state; it holds ONE chunk per worker rather than mapping 16 GB and
    leaving residency to the OS; and it recomposes every batch each epoch
    instead of freezing the grouping at whatever the file's record order made
    it. That last one is the part that matters to the net rather than to the
    clock, and it is the reason to prefer this path at size even though the
    throughput difference is small.

Shards must already be shuffled on disk (``datagen shuffle``) EITHER WAY. The
in-chunk shuffle is a second layer, not a substitute: a 2M-record window over a
file whose consecutive records come from the same game is still a window over
one region of the data, and batches drawn from it are still batches of
near-duplicates. What it buys is that a batch is composed differently every
epoch, instead of being frozen at whatever grouping the file's record order
happened to hand it.
"""

from __future__ import annotations

import math
import os

import numpy as np
import torch
from torch.utils.data import DataLoader, Dataset, IterableDataset, get_worker_info

from .format import RECORD_DTYPE, SRC_MASK, unpack

# Feature indices top out at NUM_FEATURES = 24576. int32 is not a trade here,
# it is the same numbers in half the bytes: 2.1 MB per perspective per batch
# at the default batch size rather than 4.2 MB.
INDEX_DTYPE = np.int32

# Above this many bytes of shard, make_loader picks ShuffledChunks. There is no
# cliff to find here - the chunked path was not slower at any size measured -
# so the threshold is only "big enough to be worth a second code path". Below
# it the simpler loader stays in charge, which is also the one the tests
# exercise and the one small runs have always had.
AUTO_CHUNK_BYTES = 2 * 1024 ** 3

# Records per chunk. 2^21 records is 64 MB: big enough that a drive sees the
# read as sequential, small enough that a handful of workers each holding one
# is not worth thinking about.
DEFAULT_CHUNK_RECORDS = 1 << 21


def _shard_lengths(paths):
    """Record counts per shard, and a loud failure on a truncated one.

    A shard cut short mid-record is the failure mode of an interrupted
    ``datagen shuffle``, and every record after the cut would decode as
    garbage that trains perfectly happily.
    """
    if isinstance(paths, (str, os.PathLike)):
        paths = [paths]
    paths = [os.fspath(p) for p in paths]
    counts = []
    for path in paths:
        size = os.path.getsize(path)
        if size % RECORD_DTYPE.itemsize:
            raise ValueError(f"{path} is not a whole number of records")
        counts.append(size // RECORD_DTYPE.itemsize)
    return paths, counts


def _source_filter(sources):
    return None if not sources else np.array(sorted(set(sources)), dtype=np.uint8)


def _tensors(records: np.ndarray, index_dtype) -> dict:
    fields = unpack(records, index_dtype=index_dtype)
    return {
        "white": torch.from_numpy(fields["white"]),
        "black": torch.from_numpy(fields["black"]),
        "stm": torch.from_numpy(fields["stm"]).float().unsqueeze(1),
        "score": torch.from_numpy(fields["score"]).float(),
        "wdl": torch.from_numpy(fields["wdl"]),
        # Carried for the output bucket. The bucket index itself is not
        # computed here: how many buckets there are is a property of the
        # model, and a loader that baked it in would have to be rebuilt to
        # change it.
        "piece_count": torch.from_numpy(fields["piece_count"]),
    }


class ShardBatches(Dataset):
    """Fixed-size batches over one or more shards, as memmap slices.

    Batches never straddle a file boundary; each shard simply contributes a
    short final batch. Shards concatenate byte-for-byte, so if that matters,
    ``cat`` them first.

    A batch's COMPOSITION is fixed by the file's record order - only the order
    the batches come out in is shuffled. That is fine for a dataset the page
    cache holds; see ShuffledChunks for the one it does not.
    """

    def __init__(self, paths, batch_size: int = 16384, sources=None,
                 index_dtype=INDEX_DTYPE):
        self.paths, counts = _shard_lengths(paths)
        self.batch_size = int(batch_size)
        self.sources = _source_filter(sources)
        self.index_dtype = index_dtype
        self._maps = None

        self.index = []  # (file, start, length)
        self.records = 0
        for f, n in enumerate(counts):
            self.records += n
            for start in range(0, n, self.batch_size):
                self.index.append((f, start, min(self.batch_size, n - start)))

    # A memmap is reopened per worker process rather than pickled across the
    # fork/spawn boundary - on Windows the DataLoader spawns, and a handle that
    # travelled would be reopened anyway.
    def __getstate__(self):
        state = self.__dict__.copy()
        state["_maps"] = None
        return state

    def _map(self, file_index: int) -> np.memmap:
        if self._maps is None:
            self._maps = [np.memmap(p, dtype=RECORD_DTYPE, mode="r") for p in self.paths]
        return self._maps[file_index]

    def describe(self) -> str:
        return f"memmap slices, {len(self.paths)} shard(s)"

    def __len__(self) -> int:
        return len(self.index)

    def __getitem__(self, i: int) -> dict:
        file_index, start, length = self.index[i]
        records = self._map(file_index)[start:start + length]

        if self.sources is not None:
            keep = np.isin(records["flags"] & SRC_MASK, self.sources)
            records = records[keep]
            if len(records) == 0:
                records = self._map(file_index)[start:start + 1]  # never yield empty

        return _tensors(records, self.index_dtype)


class ShuffledChunks(IterableDataset):
    """Batches drawn from large sequential chunks, shuffled inside each chunk.

    For a dataset too big for the page cache. One chunk is one contiguous
    ``read()``; the records inside it are then permuted and sliced into
    batches, so the drive sees 64 MB sequential reads while the model sees a
    fresh grouping of positions every epoch.

    Chunks are partitioned across DataLoader workers, so every record is
    visited exactly once per pass no matter how many workers there are.
    """

    def __init__(self, paths, batch_size: int = 16384,
                 chunk_records: int = DEFAULT_CHUNK_RECORDS, sources=None,
                 shuffle: bool = True, seed: int = 0, index_dtype=INDEX_DTYPE):
        self.paths, counts = _shard_lengths(paths)
        self.batch_size = int(batch_size)
        # A chunk smaller than a batch would make every batch short, which is
        # a silent 10x slowdown rather than an error.
        self.chunk_records = max(int(chunk_records), self.batch_size)
        self.sources = _source_filter(sources)
        self.shuffle = bool(shuffle)
        self.seed = int(seed)
        self.index_dtype = index_dtype
        self._epoch = 0

        self.chunks = []  # (file, start, length)
        self.records = 0
        for f, n in enumerate(counts):
            self.records += n
            for start in range(0, n, self.chunk_records):
                self.chunks.append((f, start, min(self.chunk_records, n - start)))
        self.batches = sum(math.ceil(n / self.batch_size) for _, _, n in self.chunks)

    def set_epoch(self, epoch: int) -> None:
        """Which shuffle the next pass uses.

        A resumed run has to say where it got to, or epoch 4 replays epoch 1's
        record order. Call this before the first iteration: after that the
        workers hold their own copy and only the counter they increment
        themselves matters.
        """
        self._epoch = int(epoch)

    def describe(self) -> str:
        return (f"{len(self.chunks)} sequential chunks of "
                f"{self.chunk_records:,} records, shuffled in-chunk")

    def __len__(self) -> int:
        # Exact without --sources, and an upper bound with it, which is the
        # safe direction: torch only complains when a loader yields MORE
        # batches than its length said it would.
        return self.batches

    def _read(self, file_index: int, start: int, length: int) -> np.ndarray:
        itemsize = RECORD_DTYPE.itemsize
        raw = np.empty(length * itemsize, dtype=np.uint8)
        view = memoryview(raw)
        # buffering=0: `raw` IS the buffer, and a second one in the io layer
        # would double the memory traffic of the widest part of the pipeline.
        with open(self.paths[file_index], "rb", buffering=0) as f:
            f.seek(start * itemsize)
            got = 0
            while got < len(raw):
                n = f.readinto(view[got:])
                if not n:
                    raise EOFError(f"{self.paths[file_index]} ended "
                                   f"{len(raw) - got} bytes short of record {start + length}")
                got += n
        return raw.view(RECORD_DTYPE)

    def __iter__(self):
        epoch, self._epoch = self._epoch, self._epoch + 1
        info = get_worker_info()
        worker, workers = (info.id, info.num_workers) if info else (0, 1)

        order = np.arange(len(self.chunks))
        if self.shuffle:
            # Seeded from the epoch alone, so every worker draws the SAME
            # permutation and the slice below is what partitions it. A
            # per-worker permutation would hand one chunk to two workers and
            # drop another entirely.
            np.random.default_rng([self.seed, epoch]).shuffle(order)

        # torch reseeds a worker per epoch when workers are respawned but not
        # when they persist, and the epoch counter is the other way round.
        # Folding both in means the record order moves every epoch either way,
        # and stays reproducible from torch.manual_seed().
        rng = np.random.default_rng([self.seed, epoch, worker, torch.initial_seed()])

        for chunk in order[worker::workers]:
            file_index, start, length = self.chunks[int(chunk)]
            records = self._read(file_index, start, length)

            if self.sources is not None:
                records = records[np.isin(records["flags"] & SRC_MASK, self.sources)]
                if len(records) == 0:
                    continue

            n = len(records)
            # Permute the INDICES, not the records: gathering a batch at a
            # time keeps one chunk plus one batch resident instead of two
            # chunks, and the gather is inside RAM either way.
            perm = rng.permutation(n) if self.shuffle else None
            for lo in range(0, n, self.batch_size):
                hi = min(lo + self.batch_size, n)
                take = records[perm[lo:hi]] if perm is not None else records[lo:hi]
                yield _tensors(take, self.index_dtype)


def identity_collate(batch):
    """The Dataset already returns batches; the DataLoader must not re-batch."""
    return batch[0]


def identity_item(item):
    """The same, for the iterable path, where there is no list to unwrap."""
    return item


def make_loader(paths, batch_size: int = 16384, workers: int = 4,
                shuffle: bool = True, sources=None, chunk_records=None,
                seed: int = 0, index_dtype=INDEX_DTYPE) -> DataLoader:
    """A loader over ``paths``, chunked if the dataset is big enough to need it.

    ``chunk_records`` picks the strategy: ``None`` decides from the total size
    against AUTO_CHUNK_BYTES, ``0`` forces the memmap path, and a positive
    value forces the chunked one at that size.
    """
    paths, counts = _shard_lengths(paths)
    if chunk_records is None:
        total = sum(counts) * RECORD_DTYPE.itemsize
        chunk_records = DEFAULT_CHUNK_RECORDS if total > AUTO_CHUNK_BYTES else 0

    shared = dict(
        num_workers=workers,
        pin_memory=torch.cuda.is_available(),
        persistent_workers=workers > 0,
        prefetch_factor=4 if workers > 0 else None,
    )

    if chunk_records:
        dataset = ShuffledChunks(paths, batch_size=batch_size, chunk_records=chunk_records,
                                 sources=sources, shuffle=shuffle, seed=seed,
                                 index_dtype=index_dtype)
        # No sampler and no collation: an IterableDataset shuffles itself, and
        # batch_size=None is what stops the DataLoader batching the batches.
        return DataLoader(dataset, batch_size=None, collate_fn=identity_item, **shared)

    dataset = ShardBatches(paths, batch_size=batch_size, sources=sources,
                           index_dtype=index_dtype)
    return DataLoader(dataset, batch_size=1, shuffle=shuffle,
                      collate_fn=identity_collate, **shared)


def set_epoch(loader: DataLoader, epoch: int) -> None:
    """Tell a chunked loader which pass is about to start. A no-op otherwise."""
    setter = getattr(loader.dataset, "set_epoch", None)
    if setter is not None:
        setter(epoch)


def to_device(batch: dict, device) -> dict:
    return {k: v.to(device, non_blocking=True) for k, v in batch.items()}
