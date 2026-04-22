# `separate=True` fixes: notes and status

This branch (`separate_flag`) contains two commits that make
`mypycify(..., separate=True)` work against real-world projects. Prior to
these commits, `separate=True` was effectively an experimental flag: the
only coverage in mypyc's CI is `mypyc/test/test_run.py::TestRunSeparate`
against two toy fixture files (`run-multimodule.test`, `run-mypy-sim.test`),
and mypy's own `setup.py` never sets `separate=True` (it sets `multi_file`
on Windows only). Real projects stressed the flag and turned up seven
latent bugs.

The two commits are split so it's clear which fixes were required to get
**sqlglot** building & testing cleanly, vs which additional fixes are
needed to also get **mypy**'s self-compile working.

```
$ git log --oneline separate_flag
<sha2> [mypyc] Additional separate=True fixes needed for mypy self-compile
<sha1> [mypyc] Make separate=True compilation work for real-world projects (sqlglot)
```

---

## Commit 1: sqlglot-essential fixes

**Scope:** Everything needed for sqlglot to build + test + run end-to-end
under `separate=True`. sqlglot is a ~100-module SQL parser/transpiler with
cross-group class inheritance, mypyc-generated generator helper classes,
non-extension subclasses with fast methods, and mutual cross-module
dependencies.

Six bug categories:

### 1. Non-extension classes never have vtables

`is_method_final` in `ClassIR` falls back to `is_final_class` when
`subclasses()` returns `None` (which it does under `separate=True`:
children tracking is disabled). That makes `emit_method_call` take the
vtable path. But non-extension classes skip `compute_vtable()`, so
`vtable_entry()` asserts `self.vtable is not None` and crashes.

**Fix:** short-circuit `is_method_final` to `True` for non-ext classes —
their methods are compiled as direct C functions, never dispatched via
vtable.

Trigger in sqlglot: `DType(AutoName)` (an enum) defines `into_expr`, which
mypyc wrapped as a `__mypyc_fast_into_expr` method. Any `DType.FOO.into_expr()`
cross-group crashed.

### 2. Cross-group method call requires only the decl, not the body

`emit_method_call` asserted `method = class_ir.get_method(name)` was
non-None. Under `separate=True`, a method's `FuncIR` body lives only in
the defining group; consumers see only the `FuncDecl` via `method_decls`.

**Fix:** use `method_decl(name)` (always available, walks MRO) and drop
the assert. Split `native_function_type(fn)` into a decl-taking overload.

Trigger in sqlglot: mypyc-generated generator helper classes (e.g.
`find_all_in_scope_gen`) with `__mypyc_generator_helper__` methods called
cross-group.

### 3. Cross-group native/wrapper calls bypassed the exports table

A dozen call sites in `emitwrapper.py`, `emitfunc.py`, and `emitclass.py`
hardcoded `NATIVE_PREFIX + cname()` or `PREFIX + cname()` without going
through `get_group_prefix()`. In separate mode, those symbols live in
sibling groups — the C compiler needs `exports_other.CPyDef_foo(...)`,
not `CPyDef_foo(...)`.

**Fix:** add `Emitter.native_function_call(decl)` and
`Emitter.wrapper_function_call(decl)` helpers that do the right thing,
and migrate all offending sites. Also make `CPyPy_*` wrapper declarations
`needs_export=True` so those symbols actually reach the exports table.

Trigger: any class using an `__init__` inherited across groups (e.g.
`Tables(AbstractMappingSchema)`, `TSQLParser(Parser)`); richcompare
wrappers on classes whose `__eq__` lives in a parent group.

### 4. Defer cross-group imports out of `PyInit`

The shared lib's `exec_<group>` function used to `PyImport_ImportModule`
sibling groups at PyInit time, and `PyCapsule_Import` their export
tables. But the shim's PyInit gets invoked while the enclosing Python
package's `__init__.py` is still executing, and Python's top-down getattr
walk for dotted imports tries `getattr(pkg, submod)` — which fails with
`AttributeError: partially initialized module …` because Python hasn't
yet set the submodule attribute on the parent.

**Fix:** split `exec_<group>` into two C functions:
- `exec_<short>`: capsule setup only, safe to run at PyInit time.
- `ensure_deps_<short>`: cross-group imports + exports-table memcpy.
  Idempotent, exposed as another capsule on the shared lib.

The shim (`module_shim.tmpl`) calls `ensure_deps` just before invoking the
per-module init capsule — by that time Python has settled the parent
package state for this shim's load. Also use
`PyImport_ImportModuleLevel(name, NULL, NULL, fromlist=("*",), 0)` so the
shared lib lookup returns the leaf via `sys.modules` (no dotted getattr
walk), and fetch exports capsules directly via `PyObject_GetAttrString`
instead of `PyCapsule_Import` (which itself performs the dotted walk).

### 5. Fix broken `CPyImport_ImportFrom` fallback

In `lib-rt/misc_ops.c`, the fallback path (when a name isn't found on the
imported module) intended to look up the submodule in `sys.modules`. The
comment even says "simplification of `PyImport_GetModule()`". But the code
called `PyObject_GetItem(module, fullname)` — modules don't implement
`__getitem__`, so the fallback always raised `TypeError` and fell into
the error path. Additionally, the error path called `Py_DECREF` on a
pointer that could be NULL (for built-in modules with no `__file__`).

**Fix:** use `PyImport_GetModule(fullname)` (what the comment actually
describes) and `Py_XDECREF` for potentially-NULL pointers.

### 6. Incremental-mode plumbing

Two small fixes for the IR cache path used when `separate=True` enables
mypy incremental mode:

- `compile_modules_to_ir` populates `deser_ctx.classes/functions` with
  freshly built `ClassIR`/`FuncIR` so later cache-loaded SCCs can resolve
  cross-SCC references (otherwise `ClassIR.deserialize` raises `KeyError`
  on a referenced class from a dirty-and-rebuilt SCC).
- `load_type_map` tolerates mypy-synthetic `TypeInfo` entries (e.g.
  `<subclass of X and Y>`) that have no corresponding mypyc `ClassIR`.

---

## Commit 2: mypy-specific additional fixes

**Scope:** Two additional bugs that sqlglot didn't trigger but mypy does.
These are genuinely generic fixes, not mypy-specific hacks.

### 1. Cross-group struct-field access didn't register a group dep

Emitted code like `((FooObject *)obj)->attr` requires the `FooObject`
struct declaration to be visible in the current translation unit. That
struct lives in the defining group's `__native_<group>.h`. Consumer
TUs only `#include` other groups' headers for groups listed in
`context.group_deps` — which is populated by `get_module_group_prefix`
when a cross-group function call or static is emitted. Direct struct-
field access used `struct_name()` without going through the group
prefix, so the dep was never registered.

Result for mypy: ~40 clang errors like
`error: use of undeclared identifier 'mypy___options___OptionsObject'`
when `checkexpr` reads `options.line_checking_stats` etc.

**Fix:** `Emitter.register_group_dep(cl)` helper, called from
`get_attr_expr` for both the receiver's class and the declaring class.

### 2. Runtime-file `#include` name collisions

Every generated `__native_<group>.c` starts with
`#include "int_ops.c"` (and similar for the other runtime C files in
`mypyc/lib-rt/`). Under `separate=True`, the shim `.c` files generated by
mypyc share a directory with the `__native_<group>.c` that #includes
them. Mypy has `mypyc/lower/int_ops.py`, whose shim lands at
`build/mypyc/lower/int_ops.c` — same basename as the runtime file. Clang
searches the includer's directory first (`""` form), picks up the shim,
and every shim file defines `PyInit___init__` → redefinition errors.

**Fix:** switch the runtime includes to `<name>` form (search `-I` paths
only, skip the includer's directory). The runtime directory is already
on the `-I` list.

---

## What still doesn't work: mypy incremental rebuild

**Clean build:** works with `separate=True`. Mypy self-compiles in ~60 s
with `-j 11` (vs ~1:40 monolithic), produces a functional mypy.

**Incremental edit-rebuild:** broken in one edge case. After a clean
build, editing any mypy `.py` that references `mypy.nodes.SymbolTable()`
cross-group re-emits C that calls
`exports_mypy___nodes.CPyDef_mypy___nodes___SymbolTable()`. But
`SymbolTable` is a `dict` subclass, and `generate_class_type_decl` skips
the native `CPyDef_` constructor declaration for classes with
`builtin_base` set (they're supposed to be instantiated through a
Python-level call). So `nodes`'s cached exports table has no
`CPyDef_SymbolTable` entry, and clang errors out.

**What's different about clean vs. incremental:** in the clean build,
all SCCs are fresh — `checker.py`'s build sees `SymbolTable` with
`builtin_base="PyDictObject"` populated by `prepare_class_def` and emits
a Python-level `PyObject_Call`. Under incremental, `nodes`'s SCC is
cached: `SymbolTable`'s `ClassIR` is deserialized from the `.ir.json`
cache. The deserialized `ClassIR` has `builtin_base="PyDictObject"` —
that field does serialize/deserialize correctly (verified by grep) — yet
codegen still emits the native `CPyDef_` call. Some other flag or
downstream-computed state must govern the decision; I haven't found it.

### To investigate next

1. Dump the `SymbolTable` `ClassIR` after a fresh clean build and after a
   cache-deserialize and diff them field by field. The divergent field
   that governs codegen's fresh-vs-cached choice is the culprit.
2. Grep `irbuild/classdef.py` / `irbuild/expression.py` / `irbuild/
   specialize.py` for the site that decides between `CPyDef_` ctor and
   `PyObject_Call` for class instantiation — what condition does it
   check? Compare that field's value fresh vs deserialized.
3. If the bug is in the serialize roundtrip, fix either the serializer
   (include the missing field) or the codegen check (use a field that is
   preserved).
4. Likely also need an analogous check for `BaseException` subclasses.
5. Once fixed, verify mypy's own test suite still passes in separate
   mode. Then flip `separate=True` permanently in `mypy/setup.py` to
   cut mypy's rebuild cost from ~100 s to ~3 s per edit.

---

## sqlglot-side changes

`separate=True` can cause sqlglot's own package init to expose a
circular-import pattern that the old monolithic flow happened to work
around. Two minimal sqlglot-side tweaks (in the sqlglot repo, not this
mypy fork) cover it:

1. **`sqlglot/__init__.py`** — the pre-existing bootstrap that pre-loads
   `*__mypyc*.so` under bare names was written for the legacy hash-named
   single-shared-lib build. Under `separate=True`, per-module shared libs
   sit next to their `.py` siblings and are resolved via normal dotted
   imports — skip those in the bootstrap. 8-line change.

2. **`sqlglot/optimizer/__init__.py`** — was eagerly
   `from sqlglot.optimizer.optimizer import RULES, optimize`, which
   cascades to `from sqlglot import Schema, exp`. Under `separate=True`'s
   eager cross-group init, this can fire before `sqlglot`'s own
   `__init__.py` has bound those names, causing a circular-import
   `ImportError`. Convert to PEP 562 lazy `__getattr__` so the imports
   defer until first use.

Both changes are minor and pure-Python. They ship cleanly on the sqlglot
main branch independent of this fork.

---

## Benchmarks (macOS arm64, Python 3.14.2, MYPYC_OPT=0)

### sqlglot (`separate=True` vs monolithic)

| Scenario | Monolithic | separate=True | Speedup |
|---|---|---|---|
| Clean build | 110 s | 60 s | 1.8× |
| No-op rebuild | 110 s | 1.4 s | 80× |
| Edit 1 file rebuild | 110 s | 3.3 s | 33× |
| Tests passing | 984/988 | 1227/1229 | parity |

(Pre-existing env-only failures in both runs; diff is tests discovered.)

### mypy self-compile

| Scenario | Monolithic | separate=True | Speedup |
|---|---|---|---|
| Clean, no `-j` | 1:40 | 2:59 | 0.56× (more overhead per group) |
| Clean, `-j 11` | ~1:00 (est.) | 1:04 | parity |
| No-op rebuild | ~0.5 s | ~1:00 | ⚠️ setuptools-copy overhead |
| Edit 1 file rebuild | 1:40 | ⚠️ broken | see "What still doesn't work" |

The "no-op" 1:00 is entirely setuptools copying 428 `.so` files serially
(mypyc itself did zero work). There's a `stamp` file Makefile approach
documented in the sqlglot CHANGELOG that reduces this to ~0 but it's
pure plumbing and doesn't touch mypyc.
