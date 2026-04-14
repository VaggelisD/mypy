# Arena edge cases

Catalog of correctness bugs we hit while building `@mypyc_attr(arena=True)`. Each entry documents the failure mode, root cause, and fix. Test cases TBD.

Current model: arena functions return None (discarded on exit, chunks reset). Returning data is not supported — the deep-copy machinery was tried and removed as too expensive.

## 1. Gen-0 GC list corruption on restore

**Symptom:** After arena exit, a later `gc.collect()` walks a malformed gen-0 list and crashes.

**Root cause:** During arena body, mypyc tp_alloc-stamped objects get linked into a private gen-0 list. Naively restoring the real gen-0 doesn't detach the bump-only chain — entries' `_gc_prev` still points at the sentinel of the private list.

**Fix:** Migrate-heap-out approach. On swap-out: walk the private list, splice heap entries back into real gen-0 (re-track), detach bump-only chain from the private sentinel, restore real sentinel.

**Test idea:** Allocate many mypyc-class objects inside arena, return immediately (discard), then call `gc.collect()` several times. Assert no crash.

---

## 2. Int freelist pop hands back stale bump addresses

**Symptom:** Temporary PyLongs dying during the arena body get pushed to the ints freelist by CPython's dealloc path, even though they point into bump memory. Later pops return these stale pointers.

**Root cause:** `_Py_freelist_push` only checks `size >= maxsize` to decide whether to accept. Default `maxsize` (100) is easy to fit in, so bump pointers happily land in the cache.

**Fix:** At outermost arena enter, snapshot `_Py_freelists` and set every `freelist.size` to `PY_SSIZE_T_MAX / 2`. Pushes are rejected (size > maxsize), pops return NULL (head NULL, size not touched). At outermost exit, restore the snapshot — pre-arena heap entries are cached for post-arena use.

**Test idea:** Inside arena, repeatedly create and drop large-ish PyLongs (say `hash(sql) * 7`). After exit, allocate a normal PyLong; assert its address is not in any bump chunk range (`_CPy_dbg_was_bump(id(x)) == 0`).

---

## 3. Immortal stamp prevents tp_dealloc on bump objects

**Symptom:** Without stamp, a bump-allocated mypyc instance whose refcount drops to zero runs `tp_dealloc`, which may untrack from GC, clear slots, and call `PyObject_Free`. `PyObject_Free` is a no-op on bump, but the object has already been "cleared" — subsequent references see a zombie.

**Root cause:** Refcount reaching zero triggers deallocation even when memory can't be freed.

**Fix:** mypyc codegen installs `_CPy_arena_stamp_alloc` in every compiled class's `tp_alloc` slot. After allocation, if `_CPy_arena_active`, refcount is set to `_Py_IMMORTAL_INITIAL_REFCNT`. `Py_DECREF` becomes a no-op for these objects.

**Test idea:** Inside arena, create a mypyc class instance, assign it to a local that goes out of scope, then access it again via another reference (held in `args` dict or similar). Assert no crash.

---

## 4. Chunk reuse via pool (not malloc/free churn)

**Symptom:** Large numbers of arena calls hit malloc/free heavily, defeating the speed win.

**Root cause:** Naive: free every chunk on reset. Every enter pays another malloc.

**Fix:** `_arena_reset` returns chunks to `_arena_pool` (up to `_CPY_ARENA_POOL_MAX`). Next `_bump_alloc` that needs a chunk pulls from the pool first. Steady-state allocations never touch glibc malloc.

**Test idea:** Run arena N times in a loop; record malloc counter delta (via `malloc_stats`); assert near-zero growth after warmup.

---

## 5. Interp-level caches poisoned by bump strings

**Symptom:** After enough arena calls, CPython's global intern dict contains bump strings. Next post-arena access crashes.

**Root cause:** `PyUnicode_InternInPlace` stores the passed-in string directly in the intern dict; there's no copy.

**Fix:** mypyc runtime spot fix in `getargsfast.c::parser_init`: suspend `_CPy_arena_active` around the `PyUnicode_InternInPlace` call so the intern dict gets a heap string.

**Test idea:** Call arena function with string literals many times, then after exit trigger intern via another path; assert no crash.

---

## Obsolete (from the deep-copy-on-return era; removed)

These bugs were real issues with the earlier "return persistent data" design. That path was dropped; they no longer apply.

- **CPython fast-paths** (`PyUnicode_Substring`, `copy.deepcopy` atomic short-circuit) returning the same object — matter only if we tried to produce heap copies.
- **`copy._atomic_types` short-circuit** — we tried and then reverted monkey-patching stdlib copy.
- **Read-only getsets (`gs->set == NULL`)**, **MRO `tp_base` walk**, **heap containers with bump values**, **user `__deepcopy__` direct-assign bypass** — only relevant when driving stdlib deepcopy + pre-walk.
- **Int freelist stale pointers polluting `_PyLong_Copy`** — that specific failure required `_PyLong_Copy` during deepcopy; the general freelist bug (#2 above) is still live.
- **Tuple mutation on heap tuples** — only mattered for in-place pre-walk replacement.
- **`free(): invalid pointer` at shutdown** — went away with the freelist fix.

## Open items

- **Exception paths.** If a user `@arena` function raises, does the exit path still restore gen-0 / freelists / GC enablement? Unit test TBD.
- **Nested `@arena` calls.** Inner call sees `active > 0`, should be a pure no-op. Worth asserting behavior doesn't change.
- **Free-threaded builds.** All thread-locals are per-thread; the GC-list swap and freelists-GET path differ under `Py_GIL_DISABLED`. We support it in principle; needs explicit coverage.
- **Arena function returning refcounted data.** Currently compiles (via the `_CPy_arena_exit_obj` stub) but the return is UB. Better to error at mypyc codegen time.
