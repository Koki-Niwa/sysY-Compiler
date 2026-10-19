#!/usr/bin/env python3
"""Sparse flat value table for large initializer-plan objects.

Kept out of initplan_sem.py so that file stays under its line ratchet.
"""

SPARSE_FLAT_THRESHOLD = 1 << 18   # ~2 MB of list pointers


class SparseFlat(object):
    """Large-object expected-value table: only explicitly written indices.

    A dense list for `int x[600][600][600]` needs 1.7 GB of pointers; four
    concurrent checker workers then hit MemoryError under an 8 GB limit
    (observed on sl2.sy). This keeps the len()/index interface and returns
    the zero value for unwritten positions.
    """

    __slots__ = ('_n', '_zero', '_cells')

    def __init__(self, n, zero):
        self._n = n
        self._zero = zero
        self._cells = {}

    def __len__(self):
        return self._n

    def __getitem__(self, i):
        if not 0 <= i < self._n:
            raise IndexError(i)
        return self._cells.get(i, self._zero)

    def __setitem__(self, i, value):
        if not 0 <= i < self._n:
            raise IndexError(i)
        self._cells[i] = value


def make_flat(total, zero):
    if total > SPARSE_FLAT_THRESHOLD:
        return SparseFlat(total, zero)
    return [zero] * total
