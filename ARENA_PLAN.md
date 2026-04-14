# Arena allocation for mypyc

`@mypyc_attr(arena=True)` marks a function whose body runs inside a
thread-local bump allocator. Allocations during the body skip per-object
refcounting and are reclaimed in one step when the outermost call exits.
Useful for hot throwaway work (parse, tokenize) where the result either
isn't returned or is processed before exit.

## Status

Implemented and benchmarked. `tokenize_arena` (discards output) is
~14–16 % faster than regular `Tokenizer.tokenize` on TPCH.

## Constraints

- **Return type must be `None`.** Any returned bump pointer dangles
  after chunks reset. The codegen emits `_CPy_arena_exit_obj` for
  refcounted returns to keep the link, but that stub is a no-op that
  returns the (now-dangling) pointer verbatim — caller access is UB.
- **Single interpreter, Python 3.12+.** Uses `_Py_IMMORTAL_INITIAL_REFCNT`
  to stamp bump allocations. Not free-threaded-aware.

## How it works

Four mechanisms keep bump pointers from escaping into long-lived CPython
caches.

### 1. Compiler skips refcount ops in the body

`mypyc/transform/refcount.py:insert_ref_count_opcodes` early-returns for
`ir.decl.arena` functions. The IR has no `IncRef` / `DecRef` ops — they
never get emitted. `KeepAlive` ops are stripped (codegen still wants
them gone).

Outside these functions, everything is unchanged.

### 2. Thread-local bump allocator

`mypyc/lib-rt/arena.c` installs `_arena_malloc` / `_arena_calloc` /
`_arena_realloc` / `_arena_free` as hooks on `PYMEM_DOMAIN_OBJ` and
`PYMEM_DOMAIN_MEM` at module init. When `_CPy_arena_active > 0`, malloc
calls go through `_bump_alloc` into 64 KiB chunks; free calls no-op.

Chunks are reset to a pool (cap 16) at outermost exit. Used bytes are
zeroed on return so the next `_bump_alloc` finds pre-zeroed guard bytes
without a per-allocation memset. Fresh chunks use `calloc` to get the
same guarantee in one shot.

Each allocation carries a 16-byte `_BumpHeader` (size) and a 32-byte
guard zone (covers CPython's PyGC_Head + optional managed-dict slot).

### 3. Immortal stamp

Bump memory can't be freed individually. To keep CPython from trying to,
we stamp every allocation's refcount to `_Py_IMMORTAL_INITIAL_REFCNT`
and un-track GC-managed objects from the private gen-0 list.

- **mypyc classes** — `mypyc/codegen/emitclass.py` installs
  `_CPy_arena_stamp_alloc` on the class's `tp_alloc` slot. Post-alloc
  stamp happens after the object is initialized.
- **libpython built-ins** — `mypyc/lib-rt/mypyc_util.h` wraps common
  allocators (`PyList_New`, `PyDict_New`, `PyLong_FromLong`,
  `PyUnicode_*`, `PyType_GenericAlloc`, …) with `#define` macros that
  pass the result through `_CPy_arena_stamp_{gc,nongc,any}`. The gc /
  nongc split avoids a `PyObject_IS_GC` check on the hot path.

### 4. GC-list swap + freelist block

- **GC-list swap** — `_CPy_arena_swap_in` redirects gen-0's sentinel at
  itself (empty private list). `_PyObject_GC_TRACK` calls during the
  body append to this private chain. Stamp helpers immediately un-track
  so the private list stays small.
  At exit, `_CPy_arena_swap_out` walks the list once — most entries
  have already untracked themselves, only heap entries (from
  arena-suspended paths like `parser_init`) are migrated back.
- **Freelist block** — `_CPy_arena_freelists_save_and_block` snapshots
  `_Py_freelists` and sets every head to NULL with size = MAX/2
  (rejects pushes, returns NULL on pop). Restored at exit.

### Spot fix: `getargsfast.c:parser_init`

The cached `kwtuple` and interned keyword strings must outlive the
arena, so `parser_init` temporarily clears `_CPy_arena_active` around
its `PyTuple_New` / `PyUnicode_InternInPlace` calls to force heap
allocation.

## Code layout

- `mypyc/lib-rt/arena.c` — bump allocator, PyMem hooks, GC list swap,
  freelist save/block, stamp_alloc, install entry.
- `mypyc/lib-rt/mypyc_util.h` — thread-local declarations,
  `_CPy_arena_enter` / `_exit`, stamp helpers, allocator macros.
- `mypyc/lib-rt/init.c` — calls `_CPy_arena_install()` at module init.
- `mypyc/lib-rt/getargsfast.c` — arena-suspend spot fix.
- `mypyc/transform/refcount.py` — refcount skip for arena functions.
- `mypyc/codegen/emitfunc.py` — emits `_CPy_arena_enter` at prologue,
  `_CPy_arena_exit` / `_CPy_arena_exit_obj` at return sites.
- `mypyc/codegen/emitclass.py` — installs `_CPy_arena_stamp_alloc` as
  `tp_alloc` on every mypyc class.
- `mypyc/ir/func_ir.py` — `arena: bool` field on `FuncDecl`.
- `mypyc/irbuild/util.py` — `"arena"` in `MYPYC_ATTRS`.
- `mypyc/irbuild/prepare.py` — reads the `arena` attr from decorators.

## Benchmark

TPCH tokenize, `tokenizer_core.py` + `trie.py` mypyc-compiled at O2
(see `sqlglot/sqlglotc/setup.py` minimized to those two files for fast
iteration):

```
Tokenizer.tokenize       median =  3.69 ms
tokenize_arena(None)     median =  3.16 ms
ratio 0.856x — ~14 % faster, stable memory
```

GC-disabled (CPU-only) ratio is ~0.90x (~10 % faster). The extra few
percent with GC enabled comes from GC being disabled inside arena, so
auto-collections don't add p95 jitter.

Remaining hot spots in the profile are all real tokenizer work
(`CPyStr_GetItem`, `_advance`, `_scan`, `PyUnicode_FromOrdinal`).
Nothing arena-specific appears in the top 15.

## Known gotchas

1. **`mypycify` regenerates .c files every build.** Runtime changes live
   in `mypyc/lib-rt/`; don't edit generated C.
2. **Editable-install venvs** cache a copy of `mypyc/lib-rt/`. After
   editing headers/sources, rebuild the target project rather than
   expecting `pip install -e` to re-sync.
3. **`Py_SET_REFCNT` on 3.14** writes only the low 32 bits of
   `ob_refcnt` (union: low 32 = refcount, high 32 = flags). Use
   `_Py_IMMORTAL_INITIAL_REFCNT` as the sentinel.
4. **The guard byte size must cover both PyGC_Head and the managed-dict
   slot** (16 + 16 = 32). Shrinking it breaks managed-dict types.
5. **`getargsfast.c`'s arena-suspend** creates the one class of heap
   entries in the private gen-0 list. `_CPy_arena_swap_out` migrates
   them back. Don't remove that walk.

## Non-goals

- Bulk-free mid-body (would need escape analysis).
- Deep-copy-at-return (tried, 10x slowdown — see git history).
- Free-threaded (Py_GIL_DISABLED) build support.

## Edge-case inventory

See `mypyc/lib-rt/edge_cases.md` for specific correctness bugs hit
during development (GC-list corruption, int-freelist stale pointers,
immortal-stamp prevents tp_dealloc, chunk pool reuse, intern-dict
pollution). Tests for these are TBD.
