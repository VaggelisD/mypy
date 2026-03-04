"""Repro: shadow vtable misalignment with property getters/setters.

The bug: setter shadow glue overwrites getter shadow glue because both
use fdef.name as key. This shifts all subsequent vtable entries, so
methods after properties get called at wrong indices -> segfault.
"""
from mypy_extensions import mypyc_attr
from typing import List, Optional, Callable


class Core:
    def __init__(self) -> None:
        self.chunks: List[List[int]] = [[1, 2], [3, 4]]
        self.sql: str = ""
        self.counter: int = 0


@mypyc_attr(allow_interpreted_subclasses=True)
class Base:
    def __init__(self) -> None:
        self._core = Core()

    @property
    def sql(self) -> str:
        return self._core.sql

    @sql.setter
    def sql(self, value: str) -> None:
        self._core.sql = value

    @property
    def _chunks(self) -> List[List[int]]:
        return self._core.chunks

    @_chunks.setter
    def _chunks(self, value: List[List[int]]) -> None:
        self._core.chunks = value

    @property
    def _counter(self) -> int:
        return self._core.counter

    @_counter.setter
    def _counter(self, value: int) -> None:
        self._core.counter = value

    # Methods AFTER the properties — these will be at wrong vtable offsets
    def parse(self, tokens: List[int], sql: str) -> List[int]:
        return self._parse(tokens, sql)

    def _parse(self, tokens: List[int], sql: str) -> List[int]:
        self.sql = sql
        self._chunks = [tokens]
        return self._parse_batch(lambda self: self._parse_item())

    def _parse_batch(self, callback: Callable[["Base"], Optional[int]]) -> List[int]:
        results: List[int] = []
        for chunk in self._chunks:
            for item in chunk:
                r = callback(self)
                if r is not None:
                    results.append(r)
        return results

    def _parse_item(self) -> Optional[int]:
        return 42


def test() -> None:
    b = Base()
    print("Direct:", b.parse([1, 2, 3], "test"))

    Sub = type("Sub", (Base,), {"__slots__": ()})
    s = Sub()
    print("Dynamic:", s.parse([1, 2, 3], "test"))


if __name__ == "__main__":
    test()
