"""Compiled base class."""

from mypy_extensions import mypyc_attr


@mypyc_attr(allow_interpreted_subclasses=True)
class Base:
    MAPPING: dict[str, int] = {"a": 1, "b": 2}
    FLAG: bool = False

    def lookup(self, key: str) -> int:
        return self.MAPPING.get(key, -1)

    def check_flag(self) -> bool:
        return self.FLAG
