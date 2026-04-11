"""Type mapper for raw C mode.

Maps mypy types to raw C-compatible RTypes instead of Python-wrapped types.
The key difference: int maps to int64_rprimitive (plain int64_t) instead of
int_rprimitive (CPyTagged), avoiding tagged pointer overhead entirely.
"""

from __future__ import annotations

from mypy.types import Instance, Type, get_proper_type
from mypyc.ir.rtypes import (
    RType,
    bool_rprimitive,
    float_rprimitive,
    int64_rprimitive,
    object_rprimitive,
)
from mypyc.irbuild.mapper import Mapper


class RawcMapper(Mapper):
    """Mapper that produces raw C types.

    Overrides type_to_rtype so that Python-heavy types get mapped
    to their raw C equivalents:
      - int  -> int64_rprimitive  (int64_t, not CPyTagged)
      - float -> float_rprimitive (double, same as standard)
      - bool -> bool_rprimitive   (char 0/1, same as standard)
      - None -> none_rprimitive   (same as standard)

    Types that don't have a raw C equivalent (str, list, dict, user
    classes) fall back to object_rprimitive for now. These will be
    extended with raw C struct types in later phases.
    """

    def type_to_rtype(self, typ: Type | None) -> RType:
        if typ is None:
            return object_rprimitive

        typ = get_proper_type(typ)
        if isinstance(typ, Instance):
            if typ.type.fullname == "builtins.int":
                return int64_rprimitive
            elif typ.type.fullname == "builtins.float":
                return float_rprimitive
            elif typ.type.fullname == "builtins.bool":
                return bool_rprimitive

        # For everything else, use the standard mapper.
        # This ensures we don't break anything for types we haven't
        # explicitly handled yet.
        return super().type_to_rtype(typ)
