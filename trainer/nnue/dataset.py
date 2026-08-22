"""Feeding the GPU.

Plain PyTorch is 5-20x slower than a purpose-built NNUE trainer and almost all
of the difference is the input pipeline rather than the model. Two things get
most of it back, and both are here:

  * the records are fixed-size, so a batch is a memmap SLICE and the tensors
    come out of whole-array arithmetic - there is no per-record Python;
  * a Dataset item is a whole BATCH rather than one record, so the DataLoader's
    collate step and its per-item overhead are paid once per batch instead of
    sixteen thousand times.

Shards must already be shuffled on disk (``datagen shuffle``). A DataLoader
shuffle buffer is not a substitute: a 100k-record window over a file whose
consecutive records come from the same game is not a shuffle, and the resulting
batches are batches of near-duplicates.
"""

from __future__ import annotations

import os

import numpy as np
import torch
from torch.utils.data import DataLoader, Dataset

from .format import RECORD_DTYPE, unpack


class ShardBatches(Dataset):
    """Fixed-size batches over one or more shards.

    Batches never straddle a file boundary; each shard simply contributes a
    short final batch. Shards concatenate byte-for-byte, so if that matters,
    ``cat`` them first.
    """

    def __init__(self, paths, batch_size: int = 16384, sources=None):
        if isinstance(paths, (str, os.PathLike)):
            paths = [paths]
        self.paths = [os.fspath(p) for p in paths]
        self.batch_size = int(batch_size)
        self.sources = set(sources) if sources else None
        self._maps = None

        self.index = []  # (file, start, length)
        self.records = 0
        for f, path in enumerate(self.paths):
            size = os.path.getsize(path)
            if size % RECORD_DTYPE.itemsize:
                raise ValueError(f"{path} is not a whole number of records")
            n = size // RECORD_DTYPE.itemsize
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

    def __len__(self) -> int:
        return len(self.index)

    def __getitem__(self, i: int) -> dict:
        file_index, start, length = self.index[i]
        records = self._map(file_index)[start:start + length]

        if self.sources is not None:
            keep = np.isin(records["flags"] & 0x07, list(self.sources))
            records = records[keep]
            if len(records) == 0:
                records = self._map(file_index)[start:start + 1]  # never yield empty

        fields = unpack(records)
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


def identity_collate(batch):
    """The Dataset already returns batches; the DataLoader must not re-batch."""
    return batch[0]


def make_loader(paths, batch_size: int = 16384, workers: int = 4,
                shuffle: bool = True, sources=None) -> DataLoader:
    dataset = ShardBatches(paths, batch_size=batch_size, sources=sources)
    return DataLoader(
        dataset,
        batch_size=1,
        shuffle=shuffle,
        num_workers=workers,
        collate_fn=identity_collate,
        pin_memory=torch.cuda.is_available(),
        persistent_workers=workers > 0,
        prefetch_factor=4 if workers > 0 else None,
    )


def to_device(batch: dict, device) -> dict:
    return {k: v.to(device, non_blocking=True) for k, v in batch.items()}
