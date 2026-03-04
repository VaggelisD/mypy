# mypyc Bug: Shadow Vtable Misalignment with Property Getters/Setters

## Summary

When a class with `allow_interpreted_subclasses=True` has properties with setters, the **shadow vtable** (used for interpreted subclasses) is missing getter glue methods. This causes all vtable entries after the first property to be at wrong indices, leading to **segfaults** when calling any method on interpreted subclasses (e.g. `type()`-created subclasses).

## Minimal Reproduction

```python
# test_vtable_property.py
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
    # Direct usage works
    b = Base()
    print("Direct:", b.parse([1, 2, 3], "test"))

    # Dynamic subclass segfaults (without fix)
    Sub = type("Sub", (Base,), {"__slots__": ()})
    s = Sub()
    print("Dynamic:", s.parse([1, 2, 3], "test"))


if __name__ == "__main__":
    test()
```

**Note:** The repro may not segfault on small classes because the vtable misalignment might not shift entries enough to hit an invalid pointer. The bug is proven by inspecting the generated C code (see below). In production (sqlglot's Parser with 391 vtable entries), it reliably segfaults.

## Root Cause

In `mypyc/irbuild/function.py`, `handle_ext_method()` generates shadow glue methods for `allow_interpreted_subclasses` classes:

```python
# Line 537-539 (BUGGY)
if class_ir.allow_interpreted_subclasses:
    f = gen_glue(builder, func_ir.sig, func_ir, class_ir, class_ir, fdef, do_py_ops=True)
    class_ir.glue_methods[(class_ir, name)] = f  # name = fdef.name
```

For properties, `fdef.name` is the **same** for both getter and setter (e.g., `"sql"` for both `@property def sql` and `@sql.setter def sql`). The setter's glue **overwrites** the getter's glue in the `glue_methods` dict because they share the key `(class_ir, "sql")`.

Later, in `mypyc/irbuild/vtable.py`, the vtable is built by looking up shadow methods:

```python
shadow = cls.glue_methods.get((cls, fn.name))
entries.append(VTableMethod(t, fn.name, fn, shadow))
```

For the getter (`fn.name = "sql"`), this retrieves the **setter's** glue (wrong!). For the setter (`fn.name = "__mypyc_setter__sql"`), this retrieves `None` (the setter's glue was stored under `"sql"`, not `"__mypyc_setter__sql"`).

## Proof: Generated C Vtable Comparison

**Native vtable (correct):**
```c
CPyVTableItem Base_vtable_scratch[] = {
    (CPyVTableItem)CPyDef_Base_____init__,
    (CPyVTableItem)CPyDef_Base___sql,                        // index 1: getter
    (CPyVTableItem)CPyDef_Base_____mypyc_setter__sql,        // index 2: setter
    (CPyVTableItem)CPyDef_Base____chunks,                    // index 3: getter
    (CPyVTableItem)CPyDef_Base_____mypyc_setter___3_chunks,  // index 4: setter
    (CPyVTableItem)CPyDef_Base____counter,                   // index 5: getter
    (CPyVTableItem)CPyDef_Base_____mypyc_setter___3_counter, // index 6: setter
    (CPyVTableItem)CPyDef_Base___parse,                      // index 7
    ...
};
```

**Shadow vtable WITHOUT fix (broken):**
```c
CPyVTableItem Base_vtable_shadow_scratch[] = {
    (CPyVTableItem)CPyDef_Base_____init___3__Base_glue,
    (CPyVTableItem)CPyDef_Base_____mypyc_setter__sql__Base_glue,   // index 1: SETTER glue (should be getter!)
    (CPyVTableItem)CPyDef_Base_____mypyc_setter__sql,              // index 2: native setter (no glue!)
    (CPyVTableItem)CPyDef_Base_____mypyc_setter___3_chunks__Base_glue, // WRONG
    (CPyVTableItem)CPyDef_Base_____mypyc_setter___3_chunks,            // WRONG
    (CPyVTableItem)CPyDef_Base_____mypyc_setter___3_counter__Base_glue, // WRONG
    (CPyVTableItem)CPyDef_Base_____mypyc_setter___3_counter,            // WRONG
    (CPyVTableItem)CPyDef_Base___parse__Base_glue,                      // index 7: shifted!
    ...
};
```

3 getter glues are missing. Every entry after index 0 is misaligned. When compiled code does `vtable[7]` expecting `parse`, the shadow vtable has `_counter` (native, not even a glue) at that index.

**Shadow vtable WITH fix (correct):**
```c
CPyVTableItem Base_vtable_shadow_scratch[] = {
    (CPyVTableItem)CPyDef_Base_____init___3__Base_glue,
    (CPyVTableItem)CPyDef_Base___sql__Base_glue,                   // index 1: getter glue ✓
    (CPyVTableItem)CPyDef_Base_____mypyc_setter__sql__Base_glue,   // index 2: setter glue ✓
    (CPyVTableItem)CPyDef_Base____chunks__Base_glue,               // index 3: getter glue ✓
    (CPyVTableItem)CPyDef_Base_____mypyc_setter___3_chunks__Base_glue,  // ✓
    (CPyVTableItem)CPyDef_Base____counter__Base_glue,              // index 5: getter glue ✓
    (CPyVTableItem)CPyDef_Base_____mypyc_setter___3_counter__Base_glue, // ✓
    (CPyVTableItem)CPyDef_Base___parse__Base_glue,                 // index 7: correct ✓
    ...
};
```

## Fix

**File:** `mypyc/irbuild/function.py`, line 539

Use `func_ir.decl.name` (unique per method) instead of `fdef.name` (shared for getter/setter):

```python
# BEFORE (buggy):
class_ir.glue_methods[(class_ir, name)] = f

# AFTER (fixed):
class_ir.glue_methods[(class_ir, func_ir.decl.name)] = f
```

Additionally, a `gen_glue_property_setter()` function was added to generate proper shadow glue for property setters. The shadow glue uses `PyObject_SetAttr(self, "property_name", value)` instead of trying to call `__mypyc_setter__<name>` (which doesn't exist as a Python-level attribute on interpreted subclasses).

## Impact

Any class with `allow_interpreted_subclasses=True` that has **at least one property with a setter** will have a broken shadow vtable. Interpreted subclasses (including `type()`-created dynamic subclasses) will segfault when calling methods whose vtable indices fall after the first property.

Classes without property setters (only getters, or no properties) are not affected.
