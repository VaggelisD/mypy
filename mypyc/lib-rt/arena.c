// Bump arena for @mypyc_attr(arena=True).
//
// Inside an @arena function body, all PyObject_Malloc / PyMem_Malloc calls
// route to a thread-local bump allocator. Chunks are reset at outermost
// arena exit; every bump pointer becomes invalid at that moment, so @arena
// functions must not return refcounted values that reach the caller.
//
// Four mechanisms keep bump pointers from leaking into long-lived CPython
// caches where they'd dangle after arena exit:
//
//   1. GC-list swap. At outermost enter, point gen-0's sentinel at itself
//      (empty private list). _PyObject_GC_TRACK calls during the arena
//      body link into this private chain. At exit, restore gen-0's
//      original head — the private chain is orphaned.
//
//   2. Freelist save-and-block. Snapshot the whole `_Py_freelists` struct
//      at outermost enter, then set every head=NULL and size=MAX/2. Pushes
//      are rejected (caller falls back to tp_free → our free hook → no-op
//      on bump); pops return NULL. Restore the snapshot at exit — pre-arena
//      heap entries stay cached for post-arena use.
//
//   3. tp_alloc immortal stamp. Mypyc-compiled classes route allocation
//      through `_CPy_arena_stamp_alloc`, installed in their tp_alloc slot
//      by codegen (see mypyc/codegen/emitclass.py). Post-alloc sets
//      refcount to _Py_IMMORTAL_INITIAL_REFCNT so Py_DECREF is a no-op and
//      tp_dealloc never runs on bump objects.
//
//   4. Built-in allocator macros. `PyList_New`, `PyDict_New`, `PyLong_*`,
//      `PyUnicode_*`, etc. are wrapped via #define in mypyc_util.h with
//      inline stamp helpers (_CPy_arena_stamp_{gc,nongc}). Mypyc-compiled
//      code picks up the macros at include time; libpython internals do
//      not (they're built against the vanilla headers) — but the freelist
//      block and free-hook still cover those.
//
// Plus one mypyc-runtime spot fix: `getargsfast.c::parser_init` suspends
// `_CPy_arena_active` around its `PyUnicode_InternInPlace` call so bump
// strings don't pollute CPython's global intern dict.

// Py_BUILD_CORE must be defined before Python.h so the public header's
// inline accessors agree on struct layouts with the internal headers we
// use for freelist manipulation.
#ifndef Py_BUILD_CORE
#  define Py_BUILD_CORE 1
#endif
#include <Python.h>
#include <stdlib.h>
#include <string.h>
#include "internal/pycore_freelist.h"
#include "CPy.h"

// ---------------------------------------------------------------------
// Bump allocator
// ---------------------------------------------------------------------

#define _CPY_ARENA_CHUNK_SIZE  (64 * 1024)
#define _CPY_ARENA_ALIGN       16

// Bytes reserved before each user pointer. Must be >= the largest
// pre-header CPython uses (PyGC_Head = 16, plus managed-dict slot = 16
// more for types with Py_TPFLAGS_PREHEADER). Without this, a chunk's
// bytes reused for a managed-dict GC type can make the GC head appear
// "already tracked".
#define _CPY_GUARD_SIZE        32

// Cap on chunks held in the thread-local pool between arena calls.
// Enough for typical parse workloads to avoid glibc malloc churn.
#define _CPY_ARENA_POOL_MAX    16

typedef union {
    size_t size;
    char _align[_CPY_ARENA_ALIGN];
} _BumpHeader;

typedef struct _ArenaChunk {
    struct _ArenaChunk *next;
    char *cur;
    char *end;
    char data[];
} _ArenaChunk;

CPyThreadLocal int _CPy_arena_active = 0;
static CPyThreadLocal _ArenaChunk *_arena_head = NULL;
static CPyThreadLocal _ArenaChunk *_arena_pool = NULL;

static inline size_t _align_up(size_t n) {
    return (n + _CPY_ARENA_ALIGN - 1) & ~(size_t)(_CPY_ARENA_ALIGN - 1);
}

static void *_bump_alloc(size_t user_size) {
    size_t aligned = _align_up(user_size);
    size_t total = sizeof(_BumpHeader) + _CPY_GUARD_SIZE + aligned;

    _ArenaChunk *c = _arena_head;
    if (c == NULL || (size_t)(c->end - c->cur) < total) {
        size_t payload = total > _CPY_ARENA_CHUNK_SIZE ? total : _CPY_ARENA_CHUNK_SIZE;
        if (_arena_pool != NULL && (size_t)(_arena_pool->end - _arena_pool->data) >= payload) {
            // Pool chunks are pre-zeroed on return (_CPy_arena_reset memsets
            // their used portion).
            c = _arena_pool;
            _arena_pool = c->next;
        } else {
            // calloc zeroes the payload in one shot. _bump_alloc then doesn't
            // need to memset guard bytes per allocation — they stay zero
            // because nothing writes between [header_end, user_start).
            c = (_ArenaChunk *)calloc(1, sizeof(_ArenaChunk) + payload);
            if (c == NULL) { PyErr_NoMemory(); return NULL; }
            c->end = c->data + payload;
        }
        c->cur = c->data;
        c->next = _arena_head;
        _arena_head = c;
    }
    _BumpHeader *h = (_BumpHeader *)c->cur;
    h->size = user_size;
    c->cur += total;
    return (char *)h + sizeof(_BumpHeader) + _CPY_GUARD_SIZE;
}

// ---------------------------------------------------------------------
// PyMem allocator hooks (PYMEM_DOMAIN_OBJ and PYMEM_DOMAIN_MEM).
// `ctx` points at the saved-original allocator struct for the domain.
// ---------------------------------------------------------------------

static PyMemAllocatorEx _orig_obj_alloc;
static PyMemAllocatorEx _orig_mem_alloc;

static void *_arena_malloc(void *ctx, size_t size) {
    if (_CPy_arena_active) return _bump_alloc(size);
    PyMemAllocatorEx *orig = (PyMemAllocatorEx *)ctx;
    return orig->malloc(orig->ctx, size);
}

static void *_arena_calloc(void *ctx, size_t nelem, size_t elsize) {
    if (_CPy_arena_active) {
        size_t total = nelem * elsize;
        void *p = _bump_alloc(total);
        if (p) memset(p, 0, total);
        return p;
    }
    PyMemAllocatorEx *orig = (PyMemAllocatorEx *)ctx;
    return orig->calloc(orig->ctx, nelem, elsize);
}

static void *_arena_realloc(void *ctx, void *ptr, size_t size) {
    if (_CPy_arena_active) {
        // Assume ptr is bump (every allocation during arena goes through us).
        // Read the size header and copy. NULL ptr behaves like malloc.
        if (ptr == NULL) return _bump_alloc(size);
        _BumpHeader *h = (_BumpHeader *)((char *)ptr - _CPY_GUARD_SIZE - sizeof(_BumpHeader));
        size_t old_size = h->size;
        void *new_p = _bump_alloc(size);
        if (new_p == NULL) return NULL;
        memcpy(new_p, ptr, old_size < size ? old_size : size);
        return new_p;
    }
    PyMemAllocatorEx *orig = (PyMemAllocatorEx *)ctx;
    return orig->realloc(orig->ctx, ptr, size);
}

static void _arena_free(void *ctx, void *ptr) {
    // During arena, skip all frees: bump allocations are reclaimed on chunk
    // reset, and any rare pre-arena heap pointer that dies here is
    // temporarily leaked to pymalloc's pool (reclaimed later). Dropping the
    // bump-vs-heap check lets the free hook be a single branch.
    if (_CPy_arena_active) return;
    PyMemAllocatorEx *orig = (PyMemAllocatorEx *)ctx;
    orig->free(orig->ctx, ptr);
}

// ---------------------------------------------------------------------
// GC-list swap
//
// We self-detect gen-0's sentinel at install time via a probe allocation,
// then at each arena enter we redirect the sentinel to point at itself
// (empty private list). Every _PyObject_GC_TRACK during the arena body
// appends to this private chain instead of real gen-0.
// ---------------------------------------------------------------------

static PyGC_Head *_arena_gc_head = NULL;
static CPyThreadLocal PyGC_Head _arena_saved_head;
CPyThreadLocal int _CPy_arena_saved_gc_enabled;

// Heuristic: does `node`'s body look like a valid PyObject? Used to find
// the sentinel — the one entry in gen-0 that has no PyObject after it.
static int _looks_like_pyobject_after(PyGC_Head *node) {
    PyObject *maybe = (PyObject *)((char *)node + sizeof(PyGC_Head));
    Py_ssize_t rc = maybe->ob_refcnt;
    if (rc <= 0 || rc > ((Py_ssize_t)1 << 30)) return 0;
    PyTypeObject *t = Py_TYPE(maybe);
    if (t == NULL || (uintptr_t)t < 0x10000) return 0;
    PyTypeObject *t_type = Py_TYPE((PyObject *)t);
    if (t_type == &PyType_Type) return 1;
    if (t_type == NULL || (uintptr_t)t_type < 0x10000) return 0;
    return Py_TYPE((PyObject *)t_type) == &PyType_Type;
}

static PyGC_Head *_arena_find_gc_head(void) {
    PyObject *probe = PyDict_New();
    if (probe == NULL) return NULL;
    PyGC_Head *probe_gc = (PyGC_Head *)((char *)probe - sizeof(PyGC_Head));
    PyGC_Head *head = NULL;
    PyGC_Head *node = probe_gc;
    // _gc_next uses low bits for flags — mask before chasing.
    for (int i = 0; i < 1000000; i++) {
        node = (PyGC_Head *)(node->_gc_next & _PyGC_PREV_MASK);
        if (node == NULL || node == probe_gc) break;
        if (!_looks_like_pyobject_after(node)) { head = node; break; }
    }
    Py_DECREF(probe);
    return head;
}

void _CPy_arena_swap_in(void) {
    if (_arena_gc_head == NULL) return;
    _arena_saved_head = *_arena_gc_head;
    _arena_gc_head->_gc_next = (uintptr_t)_arena_gc_head;
    _arena_gc_head->_gc_prev = (uintptr_t)_arena_gc_head;
    _CPy_arena_saved_gc_enabled = PyGC_IsEnabled();
    PyGC_Disable();
}

// Walk our private chain at exit. Most nodes are bump objects that were
// stamped immortal and untracked by stamp_{alloc,obj} — they're absent
// here. The few remaining entries are heap objects tracked by
// arena-suspended paths (e.g., `parser_init`) that need migrating back
// into real gen-0 so their later tp_dealloc uses the right list. Detach
// any bump stragglers before restoring the sentinel.
void _CPy_arena_swap_out(void) {
    if (_arena_gc_head == NULL) return;

    PyGC_Head *heap_queue = NULL;
    PyGC_Head *node = (PyGC_Head *)(_arena_gc_head->_gc_next & _PyGC_PREV_MASK);
    while (node != _arena_gc_head) {
        PyGC_Head *next = (PyGC_Head *)(node->_gc_next & _PyGC_PREV_MASK);
        PyObject *op = (PyObject *)((char *)node + sizeof(PyGC_Head));

        // Check if op lives inside one of our bump chunks. Inline scan —
        // only hit at exit, not the hot path.
        int is_bump = 0;
        const char *cp = (const char *)op;
        for (_ArenaChunk *c = _arena_head; c != NULL; c = c->next) {
            if (cp >= c->data && cp < c->end) { is_bump = 1; break; }
        }

        if (!is_bump) {
            // Splice out and queue for re-tracking into real gen-0.
            PyGC_Head *prev = (PyGC_Head *)(node->_gc_prev & _PyGC_PREV_MASK);
            _PyGCHead_SET_NEXT(prev, next);
            _PyGCHead_SET_PREV(next, prev);
            node->_gc_next = (uintptr_t)heap_queue;
            node->_gc_prev = 0;
            heap_queue = node;
        }
        node = next;
    }

    // Detach any remaining bump-only chain from the sentinel so its entries
    // can't reach into real gen-0 if their tp_dealloc runs after exit.
    PyGC_Head *first = (PyGC_Head *)(_arena_gc_head->_gc_next & _PyGC_PREV_MASK);
    if (first != _arena_gc_head) {
        PyGC_Head *last = (PyGC_Head *)(_arena_gc_head->_gc_prev & _PyGC_PREV_MASK);
        _PyGCHead_SET_NEXT(last, first);
        _PyGCHead_SET_PREV(first, last);
    }
    *_arena_gc_head = _arena_saved_head;

    // Re-track migrated heap entries.
    while (heap_queue != NULL) {
        PyGC_Head *nxt = (PyGC_Head *)heap_queue->_gc_next;
        heap_queue->_gc_next = 0;
        PyObject_GC_Track((PyObject *)((char *)heap_queue + sizeof(PyGC_Head)));
        heap_queue = nxt;
    }
    // GC stays disabled here. _CPy_arena_exit re-enables after chunk reset
    // so auto-collection can't walk transient state.
}

// ---------------------------------------------------------------------
// Freelist save-and-block
//
// `_Py_freelists` is a flat struct of `_Py_freelist` fields (one is an
// array of PyTuple_MAXSAVESIZE); we treat the whole thing as a
// `_Py_freelist[]` and memcpy it for save/restore.
// ---------------------------------------------------------------------

static CPyThreadLocal struct _Py_freelists _arena_saved_freelists;
#define _ARENA_FREELIST_COUNT (sizeof(struct _Py_freelists) / sizeof(struct _Py_freelist))

void _CPy_arena_freelists_save_and_block(void) {
    struct _Py_freelists *fls = _Py_freelists_GET();
    if (fls == NULL) return;
    _arena_saved_freelists = *fls;
    struct _Py_freelist *arr = (struct _Py_freelist *)fls;
    for (size_t i = 0; i < _ARENA_FREELIST_COUNT; i++) {
        arr[i].freelist = NULL;
        arr[i].size = PY_SSIZE_T_MAX / 2;  // any push sees size >= maxsize and rejects
    }
}

void _CPy_arena_freelists_restore(void) {
    struct _Py_freelists *fls = _Py_freelists_GET();
    if (fls == NULL) return;
    *fls = _arena_saved_freelists;
}

// ---------------------------------------------------------------------
// Chunk reset — return chunks to the pool (zeroing their used portion
// in one memset) or free them when the pool is full.
// ---------------------------------------------------------------------

void _CPy_arena_reset(void) {
    int pool_count = 0;
    for (_ArenaChunk *p = _arena_pool; p != NULL; p = p->next) pool_count++;

    _ArenaChunk *c = _arena_head;
    while (c != NULL) {
        _ArenaChunk *next = c->next;
        int standard_size = (size_t)(c->end - c->data) == _CPY_ARENA_CHUNK_SIZE;
        if (pool_count < _CPY_ARENA_POOL_MAX && standard_size) {
            // Zero what was written so the next _bump_alloc finds pre-zeroed
            // guard bytes without a per-allocation memset.
            memset(c->data, 0, (size_t)(c->cur - c->data));
            c->cur = c->data;
            c->next = _arena_pool;
            _arena_pool = c;
            pool_count++;
        } else {
            free(c);
        }
        c = next;
    }
    _arena_head = NULL;
}

// ---------------------------------------------------------------------
// tp_alloc hook (installed on every mypyc class by codegen).
// Post-alloc: stamp refcount immortal and un-track from the private gen-0
// list so _CPy_arena_swap_out doesn't have to walk thousands of entries.
// ---------------------------------------------------------------------

PyObject *_CPy_arena_stamp_alloc(PyTypeObject *tp, Py_ssize_t nitems) {
    PyObject *op = PyType_GenericAlloc(tp, nitems);
    if (op != NULL && _CPy_arena_active) {
        Py_SET_REFCNT(op, _Py_IMMORTAL_INITIAL_REFCNT);
        if (PyObject_IS_GC(op)) PyObject_GC_UnTrack(op);
    }
    return op;
}

// ---------------------------------------------------------------------
// Install hooks (idempotent, called at module init from init.c).
// ---------------------------------------------------------------------

static int _arena_installed = 0;

static void _install_domain(PyMemAllocatorDomain domain, PyMemAllocatorEx *saved) {
    PyMem_GetAllocator(domain, saved);
    PyMemAllocatorEx hook = {
        .ctx     = saved,
        .malloc  = _arena_malloc,
        .calloc  = _arena_calloc,
        .realloc = _arena_realloc,
        .free    = _arena_free,
    };
    PyMem_SetAllocator(domain, &hook);
}

void _CPy_arena_install(void) {
    if (_arena_installed) return;
    _install_domain(PYMEM_DOMAIN_OBJ, &_orig_obj_alloc);
    _install_domain(PYMEM_DOMAIN_MEM, &_orig_mem_alloc);
    _arena_gc_head = _arena_find_gc_head();
    _arena_installed = 1;
}
