"""Compiled base class."""
from mypy_extensions import mypyc_attr
from typing import Dict


@mypyc_attr(allow_interpreted_subclasses=True)
class Base:
    MAPPING: Dict[str, int] = {"a": 1, "b": 2}
    FLAG: bool = False

    def lookup(self, key: str) -> int:
        return self.MAPPING.get(key, -1)

    def check_flag(self) -> bool:
        return self.FLAG
